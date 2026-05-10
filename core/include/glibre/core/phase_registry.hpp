#pragma once
// core/include/glibre/core/phase_registry.hpp
//
// glibre::core::PhaseRegistry — runtime registry of system callbacks per phase.
//
// Authority: plan #245, reviews/decisions/frame-phases.md §Decision.
//
// Responsibilities:
//   - Accept system-callback registrations for any of the nine frame phases.
//   - Guarantee idempotency: re-registering the same (phase, fqn) pair is a
//     no-op.  This satisfies hot-reload-protocol.md step 4.1, which drains
//     and re-registers systems across a reload; idempotency prevents duplicate
//     entries from accumulating.
//   - Provide iteration in registration order — iteration order is the only
//     ordering guarantee; no sort is applied.
//   - Enforce that for_each_system() makes no allocation.  Iteration walks
//     a pre-built vector and calls each stored callable in-place.
//
// Invariants:
//   (a) Registry is mutated ONLY at plugin register/drain time (hot-reload
//       protocol §4.1), never from within tick().  for_each_system is not
//       thread-safe with concurrent register_system calls.
//   (b) PhaseSystemFn is a std::move_only_function<void()>.
//       Per reviews/decisions/eastl-removal.md matrix row 9 + Open Question 1:
//       "the migration PLAN picks std::move_only_function<Sig> by default and
//       only opens an inline-storage polyfill if the per-context SPEC §9
//       micro-benchmark fires."  std::move_only_function is not yet shipped in
//       the locked toolchain's libc++; a compat polyfill at
//       glibre/compat/move_only_function.hpp exposes the name now and is deleted
//       when libc++ ships the implementation.
//       Callers must ensure any state captured by the callable outlives the
//       PhaseRegistry or the subsequent drain_all() call.
//   (c) No heap allocation occurs inside for_each_system() — iteration is
//       a simple range walk over a pre-built std::pmr::vector.
//   (d) Storage: a std::array<std::pmr::vector<SystemEntry>, kPhaseCount>
//       keyed by static_cast<std::size_t>(phase) - 1  (Phase ordinals are
//       1..=9; subtract 1 for 0-indexed array access).  PMR vectors are backed
//       by the std::pmr::memory_resource* provided at construction, routing
//       all allocations through the per-context ceiling.
//   (e) -fno-exceptions clean; noexcept throughout the public surface.
//
// PHILOSOPHY §11: libc++ standard library is the canonical runtime substrate
// per reviews/decisions/eastl-removal.md.  std::pmr::vector, std::pmr::string,
// std::string_view, and std::move_only_function replace the prior EASTL types.
//
// Out of scope (plan #245 §Out of scope):
//   - Read/write set validation per system — next plan.
//   - ABI hashing of system fn pointers — plugin manifest plan.

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <vector>

// Polyfill for std::move_only_function<Sig> — exposes the C++23 type name
// today; backed by std::function<Sig> until libc++ ships the real type.
// Per reviews/decisions/eastl-removal.md §1 matrix row 9 + Open Question 1.
// Delete this include (and replace with <functional>) when libc++ ships
// std::move_only_function and core/include/glibre/compat/move_only_function.hpp
// is removed.
#include "glibre/compat/move_only_function.hpp"
#include "glibre/core/frame_phase.hpp"

namespace glibre::core {

// ---------------------------------------------------------------------------
// PhaseSystemFn — callable type for one registered phase-system callback.
//
// void() const signature: the system callback takes no arguments, returns void,
// and is const-invocable.  The `const` on the call signature matches
// std::move_only_function's C++23 contract for const-safe dispatch: when
// for_each_system iterates via `const PhaseSystemFn& fn`, calling `fn()`
// compiles because operator() is const on the `void() const` specialisation.
// Without `const` here, the real C++23 std::move_only_function<void()> has a
// non-const operator(), which would make `fn()` in for_each_system ill-formed
// the moment the polyfill is deleted.  Using `void() const` is the correct
// long-term signature and is valid under both the polyfill and the real type.
//
// std::move_only_function<void() const> per reviews/decisions/eastl-removal.md
// matrix row 9 (eastl::fixed_function → std::move_only_function) and Open
// Question 1: "picks std::move_only_function<Sig> by default; only opens an
// inline-storage polyfill if the per-context SPEC §9 micro-benchmark fires."
//
// Named `PhaseSystemFn` (not `SystemFn`) to avoid collision with the spec
// §5.6 `SystemFn = void (*)(SystemContext&) noexcept` type that will be
// introduced when plan #246 lands access-set validation.  That type is a
// free-standing function pointer; this type is a move-only function capturing
// closure — they are distinct, and sharing a name would create two
// `glibre::core::SystemFn` types in the same namespace.
// ---------------------------------------------------------------------------
using PhaseSystemFn = std::move_only_function<void() const>;

// Compile-time pin: PhaseSystemFn must remain std::move_only_function<void() const>.
// Guard is conditional on __cpp_lib_move_only_function so it is only active when
// the real libc++ type is present; under the polyfill the static_assert would be
// tautological (polyfill aliases std::move_only_function to std::function, making
// is_same always true regardless of the typedef) and would give false confidence.
#ifdef __cpp_lib_move_only_function
static_assert(
    !std::is_copy_constructible_v<PhaseSystemFn>,
    "PhaseSystemFn must be move-only (std::move_only_function<void() const>); "
    "a copy-constructible type was aliased — check reviews/decisions/eastl-removal.md matrix row 9"
);
#endif

// ---------------------------------------------------------------------------
// PhaseRegistry
//
// Stores per-phase system lists.  Each system is identified by a fully-
// qualified name (FQN) string and holds a PhaseSystemFn callable.
//
// Thread safety: register_system() and drain_all() are NOT safe to call
// concurrently with for_each_system() or with each other.  The intended
// call pattern is:
//   - Phase::HotReload body: drain_all() → register_system(…) for each system.
//   - run_phase() body: for_each_system(…) — read-only iteration.
// No lock is required because drain/register occurs only in Phase 8 while
// Phase 1..=7 and 9 use for_each_system (which does not write the lists).
//
// Non-copyable, non-movable: registry owns its per-phase system vectors.
//
// Allocation: std::pmr::vector<SystemEntry> per phase, backed by the
// std::pmr::memory_resource* provided at construction.  The resource pointer
// MUST remain valid for the entire lifetime of the PhaseRegistry instance
// (standard PMR lifetime contract).
//
// Per reviews/decisions/eastl-removal.md §3 (PMR adapter shape) and spike
// #1031 (PhaseRegistry allocator parity): PhaseRegistry takes a
// std::pmr::memory_resource* at construction (typically
// glibre::PerContextAllocatorResource wrapped around a PerContextAllocator{
// ContextTag::core}) so that all system-entry heap allocations are tracked
// under the core context ceiling (perf-budget.md §Allocator Rules #1).
//
// Design note — std::pmr::memory_resource* vs. PerContextAllocatorResource:
//   The header accepts the stdlib PMR interface rather than the concrete
//   glibre type in order to avoid a direct include of glibre/alloc.hpp from
//   this header.  Including glibre/alloc.hpp here would create a ContextTag
//   redefinition conflict in any TU that also includes glibre/core/frame_loop.hpp
//   (which transitively includes glibre/perf_budget.hpp — a second ContextTag
//   definition in glibre::).  Unifying alloc.hpp and perf_budget.hpp's
//   ContextTag is tracked under initiative #1032.  Until then callers supply
//   a glibre::PerContextAllocatorResource instance (or a monotonic_buffer_resource
//   for tests) and pass its address.  Production callers MUST supply
//   PerContextAllocatorResource{PerContextAllocator{ContextTag::core}} per
//   perf-budget.md §Allocator Rules #1 (chore #1070).
// ---------------------------------------------------------------------------
class PhaseRegistry {
public:
    // Construct a PhaseRegistry backed by the given PMR resource.
    //
    // `mr` must point to a valid std::pmr::memory_resource that outlives this
    // PhaseRegistry instance and all PMR containers it owns (standard PMR
    // lifetime contract).
    //
    // Recommended usage in engine code:
    //   glibre::PerContextAllocator alloc{ContextTag::core};
    //   glibre::PerContextAllocatorResource mr{alloc};
    //   PhaseRegistry reg{&mr};
    //
    // For unit tests (no ceiling enforcement required):
    //   std::pmr::monotonic_buffer_resource mr;
    //   PhaseRegistry reg{&mr};
    //   // or use std::pmr::get_default_resource() for the default heap
    //
    // Production callers MUST pass a PerContextAllocatorResource backed by
    // PerContextAllocator{ContextTag::core} per perf-budget.md §Allocator
    // Rules #1 (chore #1070).  The std::pmr::memory_resource* interface avoids
    // a direct alloc.hpp include that would create a ContextTag redefinition
    // conflict with perf_budget.hpp in TUs that also include frame_loop.hpp
    // (see Design note above; tracked under initiative #1032).
    explicit PhaseRegistry(std::pmr::memory_resource* mr) noexcept;

    PhaseRegistry(const PhaseRegistry&) = delete;
    PhaseRegistry& operator=(const PhaseRegistry&) = delete;
    PhaseRegistry(PhaseRegistry&&) = delete;
    PhaseRegistry& operator=(PhaseRegistry&&) = delete;

    ~PhaseRegistry() noexcept = default;

    // register_system() — add a system callback to a phase slot.
    //
    // The system is identified by a fully-qualified name (fqn); if a system
    // with the same fqn is already registered in the given phase, this call
    // is a no-op (idempotency guarantee, hot-reload-protocol.md §4.1).
    //
    // fn must be a callable convertible to PhaseSystemFn (std::move_only_function
    // with void() signature).  The callable is moved into the stored entry.
    // Any state captured by fn must remain valid for the lifetime of this
    // PhaseRegistry or the next drain_all() call.
    //
    // phase: must be a valid named Phase enumerator (Input..Present, 1..=9).
    //        Passing a raw-cast invalid ordinal is undefined behaviour (the
    //        array index is not bounds-checked in release builds).
    //
    // fqn: fully-qualified name, e.g. "physics.rigid_body_integrate".
    //      The string_view is copied into a std::pmr::string on first
    //      registration.  After this call returns, fqn need not remain valid.
    //
    // Complexity: O(n) where n is the number of systems already in the phase
    // (linear scan for idempotency; expected n < 32 per phase in MVP).
    //
    // noexcept: std::pmr::vector::emplace_back and std::pmr::string construction
    // go through the provided memory_resource which calls std::abort() on
    // ceiling breach in GLIBRE_ALLOC_STRICT builds (the PMR interface cannot
    // return failure without throwing, and the engine compiles -fno-exceptions).
    void register_system(Phase phase, std::string_view fqn, PhaseSystemFn fn) noexcept;

    // drain_all() — remove all registered systems from every phase at once.
    //
    // SCOPE: this is a coarse, phase-global clear, appropriate for two
    // situations only:
    //   1. MVP shutdown / process exit (clearing the registry before the
    //      PhaseRegistry destructor runs).
    //   2. Unit-test fixtures that need a blank-slate registry between test
    //      cases (e.g. per_phase_isolation drain-and-re-register step).
    //
    // It is NOT the correct primitive for the hot-reload-protocol.md §Step 1
    // drain sequence.  That protocol drains per-plugin (each outgoing plugin
    // calls `glibre_plugin_drain`), whereas drain_all() blindly clears every
    // phase regardless of which plugin owns the systems.  Plan #251 will add
    // a scoped `drain_plugin(std::string_view plugin_fqn)` once the
    // per-plugin system-ownership map exists.
    //
    // Complexity: O(total registered systems across all phases).
    // noexcept: clearing std::pmr::vector does not throw (-fno-exceptions build).
    void drain_all() noexcept;

    // for_each_system() — invoke F once per system in registration order.
    //
    // The systems for `phase` are iterated in registration order (first
    // registered = first visited).  F is called with a const reference to the
    // PhaseSystemFn for each entry:
    //
    //     registry.for_each_system(Phase::Transform, [](const PhaseSystemFn& fn) {
    //         fn();
    //     });
    //
    // Invariant: no allocation occurs inside this function.  The iteration
    // is a simple range loop over the pre-built std::pmr::vector.
    //
    // F must be callable as F(const PhaseSystemFn&) -> void and must be
    // nothrow-invocable; the static_assert below enforces this at compile time
    // so that the noexcept on for_each_system() is well-founded and the
    // engine never silently terminates from a callback that slips in an
    // exception-throwing path.
    //
    // F is taken by value to avoid the cppcoreguidelines-missing-std-forward
    // lint; the callable is not stored and does not escape this call, so a
    // value copy (which the compiler elides for lambdas in practice) is
    // equivalent to a forwarded reference for this use case.
    //
    // phase: valid named Phase enumerator (1..=9).
    //
    // NOLINT justification for pro-bounds-constant-array-index:
    //   phase_index() returns a value in [0, kPhaseCount-1] by construction
    //   (Phase is a closed enum whose only valid values are 1..=9, and
    //   phase_index subtracts 1).  The array has exactly kPhaseCount elements.
    //   A runtime-variable index is unavoidable here; the invariant is
    //   maintained by the closed-enum precondition, not by constexpr indexing.
    template<typename F>
    void for_each_system(Phase phase, F f) const noexcept {
        static_assert(
            std::is_nothrow_invocable_v<F&, const PhaseSystemFn&>,
            "for_each_system callback F must be nothrow-invocable as "
            "F(const PhaseSystemFn&); add noexcept to the lambda or use a "
            "nothrow wrapper so the noexcept guarantee on this function holds."
        );
        const auto idx = phase_index(phase);
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const auto& list = systems_[idx];
        // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (const SystemEntry& entry : list) {
            f(entry.fn);
        }
    }

    // system_count() — number of registered systems in a given phase.
    // Primarily for tests and telemetry; not called on the hot path.
    // NOLINT: pro-bounds-*-array-index — see for_each_system rationale.
    [[nodiscard]] std::size_t system_count(Phase phase) const noexcept {
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        return systems_[phase_index(phase)].size();
        // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    }

    // total_system_count() — total registered systems across all phases.
    [[nodiscard]] std::size_t total_system_count() const noexcept;

private:
    // -----------------------------------------------------------------------
    // SystemEntry — one registered system.
    //
    // fqn: owned copy of the fully-qualified system name.
    //      std::pmr::string per reviews/decisions/eastl-removal.md matrix row 1
    //      (eastl::string → std::pmr::string); allocated from mr_.
    //
    // fn:  the callable.  std::move_only_function<void()>; heap-backed.
    // -----------------------------------------------------------------------
    struct SystemEntry {
        std::pmr::string fqn;
        // fn uses void() const call signature via PhaseSystemFn, so operator()
        // is const on both the polyfill and the real C++23 type.  No mutable
        // qualifier is needed — for_each_system iterates via const SystemEntry&
        // and calls fn() without modifying the stored callable.
        PhaseSystemFn fn;

        SystemEntry(std::string_view f, PhaseSystemFn cb, std::pmr::memory_resource* mr) noexcept
            : fqn(f, mr),
              fn(std::move(cb)) {}
    };

    // phase_index() — convert Phase ordinal to 0-indexed array subscript.
    //
    // Phase ordinals are 1..=9; subtract 1 for the 0-indexed array.
    // Phase is a closed enum class; valid values are 1..=9 only.
    //
    // Debug-only bounds check (mirrors frame_loop.cpp:158-162 pattern):
    //   In debug builds, asserts that the underlying ordinal is in [1, kPhaseCount].
    //   A static_cast<Phase>(0) or static_cast<Phase>(255) from a future caller
    //   (e.g. deserialising Phase from manifest bytes) trips the assert rather than
    //   silently returning an out-of-range array index.  In release builds the
    //   check is compiled out (#ifndef NDEBUG), matching the project norm that
    //   closed-enum preconditions are asserted in debug and trusted in release.
    [[nodiscard]] static constexpr std::size_t phase_index(Phase p) noexcept {
#ifndef NDEBUG
        const auto ordinal = static_cast<std::uint8_t>(p);
        assert(
            ordinal >= 1u && ordinal <= static_cast<std::uint8_t>(kPhaseCount) &&
            "phase_index: Phase ordinal out of range [1, kPhaseCount]; "
            "raw-cast or uninitialised Phase value passed"
        );
#endif
        return static_cast<std::size_t>(static_cast<std::uint8_t>(p)) - 1u;
    }

    // mr_ — non-owning pointer to the PMR memory resource backing all
    // per-phase system vectors.  Typically a PerContextAllocatorResource
    // wrapping a PerContextAllocator{ContextTag::core} (see alloc.hpp §3).
    // MUST outlive this PhaseRegistry (standard PMR lifetime contract).
    std::pmr::memory_resource* mr_;

    // Per-phase system lists, 0-indexed (systems_[0] = Phase::Input, etc.).
    // std::pmr::vector per reviews/decisions/eastl-removal.md matrix row 3.
    // Backed by mr_; allocated lazily when first system is registered;
    // drained (not destroyed) on hot-reload.
    std::array<std::pmr::vector<SystemEntry>, kPhaseCount> systems_;
};

}  // namespace glibre::core
