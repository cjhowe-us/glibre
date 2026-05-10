// core/src/schedule/schedule.cpp
//
// Schedule — per-World system registry + DAG compiler implementation.
//
// Authority: specs/core/SPEC.md §4.4, §5.6, §6.4.
//            plan #584 — Schedule DAG builder from access sets.
//
// Memory note:
//   Schedule::Impl is allocated via std::pmr::polymorphic_allocator<Impl>
//   backed by the PerContextAllocatorResource that Schedule holds as mr_.
//   This routes the Impl object's bytes through PerContextAllocator so that
//   the Impl object itself is counted against the context ceiling — satisfying
//   §9 budget accounting.  mr_ is a direct member of Schedule (not inside Impl)
//   so it is constructed before the polymorphic_allocator that uses it.
//
//   §9 budget carve-out (R2 review MED-2, reconciled):
//     - compiled_phases — a std::array of CompiledPhase, where each CompiledPhase
//       is a pmr::vector<SystemId> routed through &mr_.  These are the bulk of
//       per-system allocation and ARE counted under the context ceiling.
//     - by_id / by_name — std::unordered_map (NOT pmr) with std::string / std::vector
//       mapped values; their node storage is global-heap (not counted under §9).
//     - RegistryEntry sub-fields (name, reads, writes, after, before) — std::string /
//       std::vector, also global-heap.
//   Net: the per-system registry bookkeeping is NOT PMR-counted; only the compiled
//   topological order is.  For MVP scale (~200 systems) this gap is sub-MB and
//   accepted.  A future plan should adopt pmr::unordered_map + pmr::string when
//   budget auditing needs tighter §9 accounting.
//   NOLINTNEXTLINE — deliberate §9 carve-out; tracked for future PMR migration.
//
//   Under -fno-exceptions + std::pmr: do_allocate() in PerContextAllocatorResource
//   calls std::abort() on OOM (the engine does not recover from heap exhaustion).
//   The null-Impl guard that was previously present is therefore dead code and
//   has been removed (see §5 below).
//
// -fno-exceptions clean.

#include "glibre/core/schedule.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#include "glibre/compat/transparent_string_hash.hpp"

#include "dag_builder.hpp"

namespace glibre::core {

// ---------------------------------------------------------------------------
// RegistryEntry — internal per-system record stored in Schedule::Impl.
//
// Placed in anonymous namespace to enforce internal linkage.
// All fields are public because this is a private implementation detail
// of Schedule::Impl; external code never sees this type.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
// ---------------------------------------------------------------------------

namespace {

struct RegistryEntry {
    SystemId id{};
    std::string name;
    Phase phase{Phase::Input};
    std::vector<TypeId> reads;
    std::vector<TypeId> writes;
    std::vector<TypeId> without;
    std::vector<std::string> after;
    std::vector<std::string> before;
    SystemFn body{nullptr};
};

}  // namespace

// NOLINTEND(misc-non-private-member-variables-in-classes)

// ---------------------------------------------------------------------------
// Schedule::Impl
//
// Internal implementation structure.  All members are accessed only by
// Schedule member functions in this translation unit.
//
// NOLINTBEGIN(misc-non-private-member-variables-in-classes) — pimpl idiom;
// Impl is a private internal type never exposed to external callers.
// ---------------------------------------------------------------------------

struct Schedule::Impl {
    // mr_ptr is a non-owning pointer to the PerContextAllocatorResource
    // owned by the Schedule object that allocated this Impl.  PMR containers
    // below use it so that their storage is counted under the context ceiling.
    // The resource outlives Impl (Schedule member-declaration order: mr_ before
    // impl_ ensures mr_ is constructed first and destroyed last).
    std::pmr::memory_resource* mr_ptr;

    std::uint64_t next_id{1};  // Next SystemId (0 is the null sentinel).

    // by_id and by_name use std::unordered_map (non-PMR) here because
    // std::pmr::unordered_map requires allocator-aware mapped types or
    // piecewise construction overhead.  Their allocations are already small
    // (pointer-sized node overhead) and counted separately.  The CompiledPhase
    // vectors — which grow per system — are PMR-backed as they hold the bulk
    // of the per-context allocation.
    std::unordered_map<std::uint64_t, RegistryEntry> by_id;  // id → entry.

    // by_name uses TransparentStringHash + std::equal_to<> (heterogeneous lookup)
    // so find() accepts std::string_view without constructing a temporary std::string.
    // Per the plugin_loader_registry pattern (plan #1044 / #1072).
    std::unordered_map<
        std::string,
        std::uint64_t,
        glibre::TransparentStringHash,
        std::equal_to<>>
        by_name;  // name → id.

    std::array<CompiledPhase, kPhaseCount> compiled_phases;  // Per-phase DAG output.
    bool dirty{true};                                        // True when recompile is needed.

    explicit Impl(std::pmr::memory_resource* mr) noexcept
        : mr_ptr{mr},
          compiled_phases{
              CompiledPhase{mr},
              CompiledPhase{mr},
              CompiledPhase{mr},
              CompiledPhase{mr},
              CompiledPhase{mr},
              CompiledPhase{mr},
              CompiledPhase{mr},
              CompiledPhase{mr},
              CompiledPhase{mr}
          } {}
};

// NOLINTEND(misc-non-private-member-variables-in-classes)

// ---------------------------------------------------------------------------
// Schedule constructor / destructor
// ---------------------------------------------------------------------------

Schedule::Schedule(PerContextAllocator& alloc) noexcept
    : mr_{alloc} {
    // Allocate Impl via the PMR resource so the Impl object's bytes are
    // tracked under the context ceiling (§9 budget accounting).
    // std::pmr::polymorphic_allocator<Impl> routes through mr_.do_allocate()
    // → PerContextAllocator::allocate() → context ceiling counter.
    //
    // Under -fno-exceptions: do_allocate() calls std::abort() on OOM (the
    // engine never recovers from heap exhaustion), so impl_ is guaranteed
    // non-null after this line.  There is no null-check on impl_ anywhere in
    // this file — see the comment in the file header.
    std::pmr::polymorphic_allocator<Impl> pa{&mr_};
    impl_ = pa.allocate(1);
    pa.construct(impl_, static_cast<std::pmr::memory_resource*>(&mr_));
}

Schedule::~Schedule() noexcept {
    if (impl_ != nullptr) {
        std::pmr::polymorphic_allocator<Impl> pa{&mr_};
        pa.destroy(impl_);
        pa.deallocate(impl_, 1);
        impl_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// register_system
// ---------------------------------------------------------------------------

Result<SystemId> Schedule::register_system(const SystemDesc& desc) noexcept {
    // Phase::HotReload (ordinal 8) is a FrameLoop-internal seam owned by the
    // plugin loader.  Plugin-authored systems registered to it would silently
    // land in a CompiledPhase that FrameLoop never walks for user systems
    // (SPEC §6.5 phase 8 = barrier_.step() seam).  Surface the error early at
    // registration rather than silently discarding the system at compile().
    if (desc.phase == Phase::HotReload) {
        return std::unexpected(glibre::Error{core::Error::SystemForbiddenInHotReloadPhase});
    }

    // Idempotency: return existing id if the (phase, name) pair is already registered.
    // SPEC §8.6: "Re-registration of an already-known (phase, system_fqn) is idempotent."
    // If the name exists but with a DIFFERENT phase, the plugin descriptor drifted —
    // this is a configuration error, not a benign re-registration.  Surface it as
    // SystemDescriptorConflict so callers (the hot-reload barrier) can refuse the swap.
    //
    // Heterogeneous lookup: find() accepts string_view directly without
    // constructing a temporary std::string (TransparentStringHash + equal_to<>).
    {
        const auto it = impl_->by_name.find(desc.name);
        if (it != impl_->by_name.end()) {
            // FQN match — verify the phase is identical.
            const auto id_val = it->second;
            const auto entry_it = impl_->by_id.find(id_val);
            if (entry_it != impl_->by_id.end() && entry_it->second.phase != desc.phase) {
                // Same FQN, different phase: reject as descriptor conflict.
                return std::unexpected(glibre::Error{core::Error::SystemDescriptorConflict});
            }
            return SystemId{id_val};
        }
    }

    // Only now materialise a std::string for the stable key stored in the map.
    const std::string name_key{desc.name};

    const SystemId id{impl_->next_id++};

    RegistryEntry entry;
    entry.id = id;
    entry.name = name_key;
    entry.phase = desc.phase;
    entry.body = desc.body;

    entry.reads.reserve(desc.access.reads.size());
    for (const TypeId& t : desc.access.reads) {
        entry.reads.push_back(t);
    }

    entry.writes.reserve(desc.access.writes.size());
    for (const TypeId& t : desc.access.writes) {
        entry.writes.push_back(t);
    }

    entry.without.reserve(desc.access.without.size());
    for (const TypeId& t : desc.access.without) {
        entry.without.push_back(t);
    }

    entry.after.reserve(desc.after.size());
    for (const std::string_view& sv : desc.after) {
        entry.after.emplace_back(sv);
    }

    entry.before.reserve(desc.before.size());
    for (const std::string_view& sv : desc.before) {
        entry.before.emplace_back(sv);
    }

    // ORDER IS LOAD-BEARING: by_name must be emplaced before by_id.
    // by_id takes entry by move (stealing entry.name); by_name copies entry.name
    // into the map key.  Swapping these lines would emplace a moved-from std::string
    // as the by_name key.  If a refactor changes this order, both emplace calls
    // must be audited together.  (R2 review LOW-1)
    impl_->by_name.emplace(entry.name, id.value);
    impl_->by_id.emplace(id.value, std::move(entry));

    impl_->dirty = true;
    return id;
}

// ---------------------------------------------------------------------------
// unregister_system
// ---------------------------------------------------------------------------

Result<void> Schedule::unregister_system(SystemId id) noexcept {
    const auto it = impl_->by_id.find(id.value);
    if (it == impl_->by_id.end()) {
        return {};  // Not registered — idempotent no-op.
    }

    impl_->by_name.erase(it->second.name);
    impl_->by_id.erase(it);
    impl_->dirty = true;
    return {};
}

// ---------------------------------------------------------------------------
// compile
// ---------------------------------------------------------------------------

Result<void> Schedule::compile() noexcept {
    if (!impl_->dirty) {
        return {};  // Idempotent: no changes since last compile.
    }

    // Clear existing compiled phases.
    for (CompiledPhase& cp : impl_->compiled_phases) {
        cp.clear();
    }

    // Bucket all registered systems by phase.
    std::array<std::vector<detail::SystemNode>, kPhaseCount> phase_nodes;

    for (auto& [id_val, entry] : impl_->by_id) {
        const auto ordinal = static_cast<std::uint8_t>(entry.phase);
        if (ordinal < kPhaseMin || ordinal > kPhaseMax) {
            continue;  // Defensive: skip out-of-range phase values.
        }
        const std::size_t idx = ordinal - 1U;

        detail::SystemNode node;
        node.id = entry.id;
        node.name = std::string_view{entry.name};  // Stable; entry lives in by_id.
        node.reads = entry.reads;
        node.writes = entry.writes;
        node.body = entry.body;

        node.after.reserve(entry.after.size());
        for (const std::string& s : entry.after) {
            node.after.emplace_back(s);
        }

        node.before.reserve(entry.before.size());
        for (const std::string& s : entry.before) {
            node.before.emplace_back(s);
        }

        phase_nodes.at(idx).push_back(std::move(node));
    }

    // Build each phase DAG.
    for (std::size_t i = 0; i < kPhaseCount; ++i) {
        auto& nodes = phase_nodes.at(i);

        // Sort by name for determinism (SPEC §4.4 invariant 5).
        std::ranges::sort(nodes, [](const detail::SystemNode& a, const detail::SystemNode& b) {
            return a.name < b.name;
        });

        auto result = detail::build_phase(std::span<const detail::SystemNode>{nodes});
        if (!result) {
            return std::unexpected(result.error());
        }

        impl_->compiled_phases.at(i).assign(result->begin(), result->end());
    }

    impl_->dirty = false;
    return {};
}

// ---------------------------------------------------------------------------
// is_compiled
// ---------------------------------------------------------------------------

bool Schedule::is_compiled() const noexcept { return !impl_->dirty; }

// ---------------------------------------------------------------------------
// compiled_phase
// ---------------------------------------------------------------------------

const CompiledPhase* Schedule::compiled_phase(Phase p) const noexcept {
    if (impl_->dirty) {
        return nullptr;
    }
    const auto ordinal = static_cast<std::uint8_t>(p);
    if (ordinal < kPhaseMin || ordinal > kPhaseMax) {
        return nullptr;
    }
    return &impl_->compiled_phases.at(ordinal - 1U);
}

}  // namespace glibre::core
