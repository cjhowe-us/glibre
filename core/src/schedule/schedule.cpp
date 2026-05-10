// core/src/schedule/schedule.cpp
//
// Schedule — per-World system registry + DAG compiler implementation.
//
// Authority: specs/core/SPEC.md §4.4, §5.6, §6.4.
//            plan #584 — Schedule DAG builder from access sets.
//
// Design:
//   Schedule::Impl holds:
//     - A map of SystemId → RegistryEntry (all registration data).
//     - A monotonic ID counter.
//     - Per-phase CompiledPhase storage (indexed by Phase ordinal 1..9).
//     - A dirty flag; set to true on any registration change.
//
//   compile() clears and rebuilds all nine phases via build_phase().
//
// Memory:
//   Impl is allocated once from the PerContextAllocator at Schedule
//   construction.  All PMR containers inside Impl use the
//   PerContextAllocatorResource backed by the same allocator so that
//   allocations count toward the core context ceiling.
//
// -fno-exceptions clean.

#include "glibre/core/schedule.hpp"
#include "dag_builder.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <unordered_map>
#include <vector>

namespace glibre::core {

// ---------------------------------------------------------------------------
// RegistryEntry — internal per-system record stored in Schedule::Impl.
// ---------------------------------------------------------------------------

struct RegistryEntry {
    SystemId id;
    std::string name;  // Owning copy of the FQN (std::string for simplicity).
    Phase phase;
    std::vector<TypeId> reads;
    std::vector<TypeId> writes;
    std::vector<TypeId> without;
    std::vector<std::string> after;   // Owning copies.
    std::vector<std::string> before;  // Owning copies.
    SystemFn body;
};

// ---------------------------------------------------------------------------
// Schedule::Impl
// ---------------------------------------------------------------------------

struct Schedule::Impl {
    // Allocator resource for PMR containers.
    PerContextAllocatorResource mr;

    // Next SystemId to assign (starts at 1; 0 is null/invalid).
    std::uint64_t next_id{1};

    // Primary registry: SystemId → RegistryEntry.
    // Keyed by value for O(1) lookup in unregister_system().
    std::unordered_map<std::uint64_t, RegistryEntry> by_id;

    // Name → SystemId for idempotency check in register_system().
    std::unordered_map<std::string, std::uint64_t> by_name;

    // Compiled output: one CompiledPhase per Phase (1..9).
    // Indexed by Phase ordinal - 1 (i.e., index 0 = Phase::Input).
    std::array<CompiledPhase, kPhaseCount> compiled_phases;

    // Dirty flag: true when a registration change has invalidated the DAGs.
    bool dirty{true};

    explicit Impl(PerContextAllocator& alloc) noexcept
        : mr{alloc},
          compiled_phases{
              CompiledPhase{&mr}, CompiledPhase{&mr}, CompiledPhase{&mr},
              CompiledPhase{&mr}, CompiledPhase{&mr}, CompiledPhase{&mr},
              CompiledPhase{&mr}, CompiledPhase{&mr}, CompiledPhase{&mr}} {}
};

// ---------------------------------------------------------------------------
// Schedule constructor / destructor
// ---------------------------------------------------------------------------

Schedule::Schedule(PerContextAllocator& alloc) noexcept {
    // Allocate Impl from the PerContextAllocator using placement via raw alloc.
    auto res = alloc.allocate(sizeof(Impl), alignof(Impl));
    if (!res) {
        impl_ = nullptr;
        return;
    }
    impl_ = new (*res) Impl{alloc};  // Placement-new.
}

Schedule::~Schedule() noexcept {
    if (impl_) {
        // Explicitly destroy (placement-new requires explicit destruction).
        // The backing memory is leaked intentionally here — the allocator is
        // a context singleton whose lifetime matches or exceeds the Schedule.
        // In a full engine this would return memory to the PerContextAllocator;
        // for the MVP the destructor just destroys the object in place.
        impl_->~Impl();
        // Note: memory is not freed here because PerContextAllocator has no
        // bulk-free / arena-reset at destruction (MVP).  The bytes_used counter
        // will be non-zero but the allocator is torn down with the engine anyway.
        impl_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// register_system
// ---------------------------------------------------------------------------

Result<SystemId> Schedule::register_system(const SystemDesc& desc) noexcept {
    if (!impl_) {
        // Construction failed (allocation error).
        return std::unexpected(glibre::Error{core::Error::OutOfBudget});
    }

    // Idempotency: if name already registered, return existing id.
    {
        std::string name_key{desc.name};
        auto it = impl_->by_name.find(name_key);
        if (it != impl_->by_name.end()) {
            return SystemId{it->second};
        }
    }

    // Assign new id.
    const SystemId id{impl_->next_id++};

    // Build RegistryEntry (copies all span data into owned storage).
    RegistryEntry entry;
    entry.id = id;
    entry.name = std::string{desc.name};
    entry.phase = desc.phase;
    entry.body = desc.body;

    entry.reads.reserve(desc.access.reads.size());
    for (const auto& t : desc.access.reads) entry.reads.push_back(t);

    entry.writes.reserve(desc.access.writes.size());
    for (const auto& t : desc.access.writes) entry.writes.push_back(t);

    entry.without.reserve(desc.access.without.size());
    for (const auto& t : desc.access.without) entry.without.push_back(t);

    entry.after.reserve(desc.after.size());
    for (const auto& sv : desc.after) entry.after.push_back(std::string{sv});

    entry.before.reserve(desc.before.size());
    for (const auto& sv : desc.before) entry.before.push_back(std::string{sv});

    impl_->by_name.emplace(entry.name, id.value);
    impl_->by_id.emplace(id.value, std::move(entry));

    impl_->dirty = true;
    return id;
}

// ---------------------------------------------------------------------------
// unregister_system
// ---------------------------------------------------------------------------

Result<void> Schedule::unregister_system(SystemId id) noexcept {
    if (!impl_) {
        return std::unexpected(glibre::Error{core::Error::OutOfBudget});
    }

    auto it = impl_->by_id.find(id.value);
    if (it == impl_->by_id.end()) {
        // Not registered — idempotent no-op.
        return {};
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
    if (!impl_) {
        return std::unexpected(glibre::Error{core::Error::OutOfBudget});
    }

    // Idempotent: skip if no changes since last compile.
    if (!impl_->dirty) {
        return {};
    }

    // Clear existing compiled phases.
    for (auto& cp : impl_->compiled_phases) {
        cp.clear();
    }

    // Build nodes grouped by phase.
    // Phase ordinals are 1..9; we iterate all registered systems and bucket them.
    std::array<std::vector<detail::SystemNode>, kPhaseCount> phase_nodes;

    for (auto& [id_val, entry] : impl_->by_id) {
        const auto ordinal = static_cast<std::uint8_t>(entry.phase);
        if (ordinal < kPhaseMin || ordinal > kPhaseMax) continue;  // Defensive.
        const std::size_t idx = ordinal - 1u;

        detail::SystemNode node;
        node.id = entry.id;
        node.name = std::string_view{entry.name};  // Non-owning; entry is stable.
        node.reads = entry.reads;
        node.writes = entry.writes;
        node.body = entry.body;

        // after/before: convert std::string to std::string_view.
        node.after.reserve(entry.after.size());
        for (const auto& s : entry.after) node.after.push_back(std::string_view{s});

        node.before.reserve(entry.before.size());
        for (const auto& s : entry.before) node.before.push_back(std::string_view{s});

        phase_nodes[idx].push_back(std::move(node));
    }

    // For each phase: sort nodes by name for determinism, then build DAG.
    for (std::size_t i = 0; i < kPhaseCount; ++i) {
        auto& nodes = phase_nodes[i];

        // Sort by name (tiebreaker §4.4 invariant 5) so that the DAG builder
        // operates on a deterministic input order.
        std::sort(nodes.begin(), nodes.end(),
                  [](const detail::SystemNode& a, const detail::SystemNode& b) {
                      return a.name < b.name;
                  });

        auto result = detail::build_phase(std::span<const detail::SystemNode>{nodes});
        if (!result) {
            return std::unexpected(result.error());
        }

        // Move the sorted SystemId vector into the PMR-backed CompiledPhase.
        auto& cp = impl_->compiled_phases[i];
        cp.assign(result->begin(), result->end());
    }

    impl_->dirty = false;
    return {};
}

// ---------------------------------------------------------------------------
// is_compiled
// ---------------------------------------------------------------------------

bool Schedule::is_compiled() const noexcept {
    if (!impl_) return false;
    return !impl_->dirty;
}

// ---------------------------------------------------------------------------
// compiled_phase
// ---------------------------------------------------------------------------

const CompiledPhase* Schedule::compiled_phase(Phase p) const noexcept {
    if (!impl_ || impl_->dirty) return nullptr;
    const auto ordinal = static_cast<std::uint8_t>(p);
    if (ordinal < kPhaseMin || ordinal > kPhaseMax) return nullptr;
    return &impl_->compiled_phases[ordinal - 1u];
}

}  // namespace glibre::core
