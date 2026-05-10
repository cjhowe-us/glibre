#pragma once
// core/include/glibre/core/schedule.hpp
//
// Schedule — per-Phase DAG builder from declared access sets.
//
// Authority: specs/core/SPEC.md §4.4, §5.6, §6.4.
//            plan #584 — Schedule DAG builder from access sets.
//
// Public API:
//   AccessSet  — reads / writes / without spans over TypeId.
//   SystemDesc — fully-qualified name + Phase + AccessSet + body +
//                after/before ordering edges.
//   Schedule   — register_system / unregister_system / compile.
//
// Invariants enforced by compile():
//   1. Cycle in the dependency graph → core::Error::SystemScheduleCycle.
//      A cycle can arise from explicit after/before edges OR from mutual
//      access-set intersection edges (e.g. A writes X, B writes X →
//      A→B; B writes X, A writes X → B→A, cycle).
//   2. A pure access-set cycle (no explicit after/before) where the two
//      systems mutually write the same component yields
//      core::Error::ScheduleAccessConflict rather than SystemScheduleCycle,
//      per plan #584 Scope.
//   3. Deterministic tiebreaker: lexicographic order of system FQN (§4.4 #5).
//   4. CompiledPhase is a std::pmr::vector<SystemId> in resolved topological
//      order, walked per frame by FrameLoop.
//
// Storage: allocations under ContextTag::core (PerContextAllocatorResource).
//
// -fno-exceptions clean; all errors propagated via glibre::Result<T>.

#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "glibre/alloc.hpp"
#include "glibre/core/frame_phase.hpp"
#include "glibre/error.hpp"

namespace glibre::core {

// ---------------------------------------------------------------------------
// SystemId — opaque stable identifier assigned at registration time.
//
// Value 0 is the null/invalid sentinel (never returned by register_system).
// SystemIds are stable across compile() calls for the same registration set.
// ---------------------------------------------------------------------------

struct SystemId {
    std::uint64_t value{0};

    friend constexpr bool operator==(SystemId, SystemId) noexcept = default;
    friend constexpr bool operator<(SystemId a, SystemId b) noexcept {
        return a.value < b.value;
    }
};

// ---------------------------------------------------------------------------
// TypeId — opaque component/resource type identifier used in access sets.
//
// Mirrors specs/core/SPEC.md §5.2.  Codegen assigns stable values; the
// schedule only compares them for intersection, never interprets them.
// ---------------------------------------------------------------------------

struct TypeId {
    std::uint64_t value{0};

    friend constexpr bool operator==(TypeId, TypeId) noexcept = default;
};

// ---------------------------------------------------------------------------
// AccessSet — read / write / without spans over TypeId.
//
// Callers retain ownership of the arrays; spans must remain valid for the
// lifetime of the SystemDesc passed to register_system().  Schedule copies
// the contents internally.
// ---------------------------------------------------------------------------

struct AccessSet {
    std::span<const TypeId> reads{};
    std::span<const TypeId> writes{};
    std::span<const TypeId> without{};
};

// ---------------------------------------------------------------------------
// SystemFn — free-standing function pointer for a system body.
//
// Capturing closures are out of scope (ABI-stable across hot-reloads,
// SPEC §5.6).  SystemContext is forward-declared; its definition lands
// with the FrameLoop integration plan.
// ---------------------------------------------------------------------------

class SystemContext;
using SystemFn = void (*)(SystemContext& ctx) noexcept;

// ---------------------------------------------------------------------------
// SystemDesc — full descriptor passed to register_system().
//
// `name` must be a fully-qualified, stable system name (e.g.
// "myplugin::systems::ApplyVelocity").  It is used as the deterministic
// tiebreaker (§4.4 invariant 5) and as the target of after/before edges.
//
// `after`  — this system runs AFTER the named systems.
// `before` — this system runs BEFORE the named systems.
//
// All span members must remain valid until register_system() returns; the
// Schedule copies the pointed-to data into its internal storage.
// ---------------------------------------------------------------------------

struct SystemDesc {
    std::string_view name{};
    Phase phase{};
    AccessSet access{};
    SystemFn body{nullptr};
    std::span<const std::string_view> after{};
    std::span<const std::string_view> before{};
};

// ---------------------------------------------------------------------------
// CompiledPhase — flat resolved execution order for one Phase.
//
// A CompiledPhase is a std::pmr::vector<SystemId> in topological order.
// FrameLoop walks it linearly each frame (single-threaded MVP).
// ---------------------------------------------------------------------------

using CompiledPhase = std::pmr::vector<SystemId>;

// ---------------------------------------------------------------------------
// Schedule — per-World system registry + DAG compiler.
//
// Lifecycle:
//   1. Plugins call register_system() at load time.
//   2. After all plugins are registered, compile() is called once.
//   3. Further register_system() / unregister_system() calls invalidate
//      the compiled state; compile() must be called again.
//   4. compiled_phase(p) returns the resolved order for Phase p.
//
// Thread safety: not thread-safe.  The PluginLoader serialises all
// registration calls at the frame-boundary hot-reload seam.
// ---------------------------------------------------------------------------

class Schedule {
public:
    // Construct a Schedule backed by the given allocator.
    //
    // `alloc` must outlive the Schedule (typically a context-singleton
    // PerContextAllocator for ContextTag::core).
    explicit Schedule(PerContextAllocator& alloc) noexcept;

    // Non-copyable, non-movable (owns PMR containers keyed to the allocator).
    Schedule(const Schedule&) = delete;
    Schedule& operator=(const Schedule&) = delete;
    Schedule(Schedule&&) = delete;
    Schedule& operator=(Schedule&&) = delete;

    ~Schedule() noexcept;

    // register_system — add a system to the registry.
    //
    // If a system with the same `desc.name` is already registered,
    // the call is idempotent and returns the existing SystemId.
    // Otherwise assigns a new SystemId (value > 0, monotonically increasing).
    //
    // Invalidates the compiled state if the set changes.
    //
    // Returns std::unexpected on internal allocation failure (abort is
    // not used here; OOM is surfaced as an error so tests can observe it).
    [[nodiscard]] Result<SystemId> register_system(const SystemDesc& desc) noexcept;

    // unregister_system — remove the system identified by `id`.
    //
    // No-op if `id` does not correspond to a registered system (idempotent
    // in the presence of double-unregistration, which can occur during
    // hot-reload rollback).  Invalidates the compiled state when the set
    // changes.
    [[nodiscard]] Result<void> unregister_system(SystemId id) noexcept;

    // compile — build or rebuild the per-phase DAGs.
    //
    // Idempotent if no registrations changed since the last compile().
    //
    // Algorithm (per SPEC §6.4):
    //   For each Phase 1..9:
    //     1. Collect SystemDesc records whose phase matches.
    //     2. Build directed graph G with edges A→B when:
    //        - A.writes ∩ B.reads ≠ ∅  (read-after-write dependency), or
    //        - A.writes ∩ B.writes ≠ ∅ (write-write conflict), or
    //        - A.name ∈ B.after,        or
    //        - B.name ∈ A.before.
    //     3. Topologically sort G (Kahn's algorithm for determinism).
    //        Cycle from access-set edges → ScheduleAccessConflict.
    //        Cycle from after/before edges → SystemScheduleCycle.
    //     4. Store result as CompiledPhase (std::pmr::vector<SystemId>).
    //
    // Returns std::unexpected on cycle or allocation failure.
    [[nodiscard]] Result<void> compile() noexcept;

    // compiled_phase — returns the compiled execution order for Phase p.
    //
    // Returns nullptr if compile() has not been called or if the compiled
    // state has been invalidated by a registration change.  FrameLoop must
    // call compile() and check for non-null before each frame.
    [[nodiscard]] const CompiledPhase* compiled_phase(Phase p) const noexcept;

    // is_compiled — true if compile() has been called and no subsequent
    // registration change has invalidated the result.
    [[nodiscard]] bool is_compiled() const noexcept;

private:
    struct Impl;
    Impl* impl_;  // Heap-allocated pimpl (PerContextAllocator manages the memory).
};

}  // namespace glibre::core
