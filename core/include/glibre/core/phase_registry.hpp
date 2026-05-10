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
//   (b) SystemFn is an eastl::fixed_function<kSystemFnStorageBytes, void()>.
//       The fixed-storage callable avoids heap allocation on construction and
//       makes the stored callable self-contained without a heap pointer.
//       Callers must ensure any state captured by the callable outlives the
//       PhaseRegistry or the subsequent drain() call.
//   (c) No heap allocation occurs inside for_each_system() — iteration is
//       a simple range walk over a pre-built eastl::vector.
//   (d) Storage: a std::array<eastl::vector<SystemEntry>, kPhaseCount>
//       keyed by static_cast<std::size_t>(phase) - 1  (Phase ordinals are
//       1..=9; subtract 1 for 0-indexed array access).
//   (e) -fno-exceptions clean; noexcept throughout the public surface.
//
// PHILOSOPHY §11: eastl::string and eastl::vector are the runtime substrate;
// std::string / std::vector are not used here.
//
// Out of scope (plan #245 §Out of scope):
//   - Read/write set validation per system — next plan.
//   - ABI hashing of system fn pointers — plugin manifest plan.

#include <array>
#include <cstddef>
#include <cstdint>

// EASTL substrate — PHILOSOPHY §11 mandates eastl:: for engine runtime data.
#include <EASTL/fixed_function.h>
#include <EASTL/string.h>
#include <EASTL/string_view.h>
#include <EASTL/vector.h>

#include "glibre/core/frame_phase.hpp"

namespace glibre::core {

// ---------------------------------------------------------------------------
// SystemFn — callable type for one registered system.
//
// void() signature: the system callback takes no arguments and returns void.
// The fixed-function storage is 64 bytes — large enough for a lambda
// capturing a pair of pointers (context + data), which covers all known
// MVP system call patterns.
//
// PHILOSOPHY §11: eastl::fixed_function, not std::function.
// ---------------------------------------------------------------------------
inline constexpr std::size_t kSystemFnStorageBytes = 64;
using SystemFn = eastl::fixed_function<kSystemFnStorageBytes, void()>;

// ---------------------------------------------------------------------------
// PhaseRegistry
//
// Stores per-phase system lists.  Each system is identified by a fully-
// qualified name (FQN) string and holds a SystemFn callable.
//
// Thread safety: register_system() and drain() are NOT safe to call
// concurrently with for_each_system() or with each other.  The intended
// call pattern is:
//   - Phase::HotReload body: drain() → register_system(…) for each system.
//   - run_phase() body: for_each_system(…) — read-only iteration.
// No lock is required because drain/register occurs only in Phase 8 while
// Phase 1..=7 and 9 use for_each_system (which does not write the lists).
//
// Non-copyable, non-movable: registry owns its per-phase system vectors.
// ---------------------------------------------------------------------------
class PhaseRegistry {
public:
    PhaseRegistry() noexcept = default;

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
    // fn must be a callable convertible to SystemFn (eastl::fixed_function
    // with kSystemFnStorageBytes internal storage).  The callable is copied
    // or moved into the stored entry.  Any state captured by fn must remain
    // valid for the lifetime of this PhaseRegistry or the next drain() call.
    //
    // phase: must be a valid named Phase enumerator (Input..Present, 1..=9).
    //        Passing a raw-cast invalid ordinal is undefined behaviour (the
    //        array index is not bounds-checked in release builds).
    //
    // fqn: fully-qualified name, e.g. "physics.rigid_body_integrate".
    //      The string_view is copied into an eastl::string on first
    //      registration.  After this call returns, fqn need not remain valid.
    //
    // Complexity: O(n) where n is the number of systems already in the phase
    // (linear scan for idempotency; expected n < 32 per phase in MVP).
    //
    // noexcept: the eastl::vector push_back and eastl::string construction
    // may throw in theory (EASTL uses EASTL_EXCEPTIONS_ENABLED), but glibre
    // builds with -fno-exceptions so EASTL never throws; the function is
    // declared noexcept to match the project's exception-free contract.
    void register_system(Phase phase, eastl::string_view fqn, SystemFn fn) noexcept;

    // drain() — remove all registered systems from all phases.
    //
    // Called by the hot-reload protocol (Phase::HotReload) before re-
    // registering the new set of systems.  Equivalent to clearing every
    // per-phase vector.  After drain() returns, every phase has zero systems.
    //
    // Complexity: O(total registered systems).
    // noexcept: clearing eastl::vector does not throw (no-exceptions build).
    void drain() noexcept;

    // for_each_system() — invoke F once per system in registration order.
    //
    // The systems for `phase` are iterated in registration order (first
    // registered = first visited).  F is called with a reference to the
    // SystemFn for each entry:
    //
    //     registry.for_each_system(Phase::Transform, [](SystemFn& fn) {
    //         fn();  // invoke the system
    //     });
    //
    // Invariant: no allocation occurs inside this function.  The iteration
    // is a simple range loop over the pre-built eastl::vector.
    //
    // F must be callable as F(SystemFn&) -> void.
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
        const auto idx = phase_index(phase);
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const auto& list = systems_[idx];
        // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        for (const SystemEntry& entry : list) {
            f(const_cast<SystemFn&>(entry.fn));
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
    //      eastl::string per PHILOSOPHY §11; allocated once on registration,
    //      freed on drain() or registry destruction.
    //
    // fn:  the callable.  Fixed storage, no heap pointer.
    // -----------------------------------------------------------------------
    struct SystemEntry {
        eastl::string fqn;
        SystemFn fn;

        SystemEntry(eastl::string_view f, SystemFn cb) noexcept
            : fqn(f.data(), f.size()),
              fn(eastl::move(cb)) {}
    };

    // phase_index() — convert Phase ordinal to 0-indexed array subscript.
    //
    // Phase ordinals are 1..=9; subtract 1 for the 0-indexed array.
    // Phase is a closed enum class; valid values are 1..=9 only.
    // No bounds check is performed in this inline helper (the debug assert
    // in phase_desc() covers misuse when called via that accessor).
    [[nodiscard]] static constexpr std::size_t phase_index(Phase p) noexcept {
        return static_cast<std::size_t>(static_cast<std::uint8_t>(p)) - 1u;
    }

    // Per-phase system lists, 0-indexed (systems_[0] = Phase::Input, etc.).
    // eastl::vector per PHILOSOPHY §11.  Allocated lazily when first system
    // is registered; drained (not destroyed) on hot-reload.
    std::array<eastl::vector<SystemEntry>, kPhaseCount> systems_;
};

}  // namespace glibre::core
