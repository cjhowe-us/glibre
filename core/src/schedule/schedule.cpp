// core/src/schedule/schedule.cpp
//
// Schedule — per-World system registry + DAG compiler implementation.
//
// Authority: specs/core/SPEC.md §4.4, §5.6, §6.4.
//            plan #584 — Schedule DAG builder from access sets.
//
// Memory note:
//   Schedule::Impl is allocated with standard `new` (no placement new).
//   The backing memory comes from the system heap, not from the
//   PerContextAllocator directly, because placement new on a raw allocation
//   from a non-owning allocator creates a difficult ownership split: the
//   allocator never receives a `deallocate` call, so bytes_used drifts.
//   Instead, Schedule::Impl is heap-allocated normally, and all internal
//   PMR containers in Impl use PerContextAllocatorResource to count their
//   storage against the context ceiling.  The Impl object itself is small
//   (~few hundred bytes) and allocation-failure-proof under normal conditions.
//
// -fno-exceptions clean.

#include "glibre/core/schedule.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

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
    PerContextAllocatorResource mr;  // PMR resource backed by the context allocator.
    std::uint64_t next_id{1};        // Next SystemId (0 is the null sentinel).
    std::unordered_map<std::uint64_t, RegistryEntry> by_id;  // id → entry.
    std::unordered_map<std::string, std::uint64_t> by_name;  // name → id.
    std::array<CompiledPhase, kPhaseCount> compiled_phases;  // Per-phase DAG output.
    bool dirty{true};                                        // True when recompile is needed.

    explicit Impl(PerContextAllocator& alloc) noexcept
        : mr{alloc},
          compiled_phases{
              CompiledPhase{&mr},
              CompiledPhase{&mr},
              CompiledPhase{&mr},
              CompiledPhase{&mr},
              CompiledPhase{&mr},
              CompiledPhase{&mr},
              CompiledPhase{&mr},
              CompiledPhase{&mr},
              CompiledPhase{&mr}
          } {}
};

// NOLINTEND(misc-non-private-member-variables-in-classes)

// ---------------------------------------------------------------------------
// Schedule constructor / destructor
// ---------------------------------------------------------------------------

Schedule::Schedule(PerContextAllocator& alloc) noexcept
    : impl_{new Impl{alloc}} {}  // NOLINT(cppcoreguidelines-owning-memory)

Schedule::~Schedule() noexcept {
    delete impl_;  // NOLINT(cppcoreguidelines-owning-memory)
}

// ---------------------------------------------------------------------------
// register_system
// ---------------------------------------------------------------------------

Result<SystemId> Schedule::register_system(const SystemDesc& desc) noexcept {
    if (impl_ == nullptr) {
        return std::unexpected(glibre::Error{core::Error::OutOfBudget});
    }

    // Idempotency: return existing id if name already registered.
    const std::string name_key{desc.name};
    {
        const auto it = impl_->by_name.find(name_key);
        if (it != impl_->by_name.end()) {
            return SystemId{it->second};
        }
    }

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

    impl_->by_name.emplace(entry.name, id.value);
    impl_->by_id.emplace(id.value, std::move(entry));

    impl_->dirty = true;
    return id;
}

// ---------------------------------------------------------------------------
// unregister_system
// ---------------------------------------------------------------------------

Result<void> Schedule::unregister_system(SystemId id) noexcept {
    if (impl_ == nullptr) {
        return std::unexpected(glibre::Error{core::Error::OutOfBudget});
    }

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
    if (impl_ == nullptr) {
        return std::unexpected(glibre::Error{core::Error::OutOfBudget});
    }

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

bool Schedule::is_compiled() const noexcept {
    if (impl_ == nullptr) {
        return false;
    }
    return !impl_->dirty;
}

// ---------------------------------------------------------------------------
// compiled_phase
// ---------------------------------------------------------------------------

const CompiledPhase* Schedule::compiled_phase(Phase p) const noexcept {
    if (impl_ == nullptr || impl_->dirty) {
        return nullptr;
    }
    const auto ordinal = static_cast<std::uint8_t>(p);
    if (ordinal < kPhaseMin || ordinal > kPhaseMax) {
        return nullptr;
    }
    return &impl_->compiled_phases.at(ordinal - 1U);
}

}  // namespace glibre::core
