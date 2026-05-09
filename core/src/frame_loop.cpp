// core/src/frame_loop.cpp
//
// Implementation of glibre::core::FrameLoop.
//
// Phase ordering is authoritative in reviews/decisions/frame-phases.md.
// The check for FramePhaseMisordered is enabled in debug builds only
// (NDEBUG not defined), per SPEC §4.4 invariant 1 and the schedule-
// frame-design.md §3.1 note: "Violation is a … debug-build runtime
// assertion core::Error::FramePhaseMisordered."
//
// TransientArena drain at phase 9 (Phase::Present) is per
// perf-budget.md §Allocator Rules #4 and plan #239.

#include "glibre/core/frame_loop.hpp"

#include <cassert>
#include <cstdint>

#include "glibre/core/frame_phase.hpp"
#include "glibre/error.hpp"
#include "glibre/transient_arena.hpp"

namespace glibre::core {

// ---------------------------------------------------------------------------
// register_transient_arena — add an arena to the drain registry.
// ---------------------------------------------------------------------------

[[nodiscard]] glibre::Result<void>
FrameLoop::register_transient_arena(glibre::TransientArena* arena) noexcept {
    // Null pointer is a precondition violation; assert in debug builds.
    // In release builds we skip silently rather than crash, to avoid undefined
    // behaviour at phase 9 drain.  Callers must ensure the pointer is valid.
    assert(arena != nullptr && "register_transient_arena: null arena pointer");
    if (arena == nullptr) {
        return {};  // skip silently in release; debug assert fires above
    }
    if (arena_count_ >= kMaxTransientArenas) {
        return std::unexpected(glibre::Error{
            core::Error::OutOfBudget,
            glibre::ErrorContext{
                .file = "core/src/frame_loop.cpp",
                .line = __LINE__,
                .detail = "transient arena registry full",
            },
        });
    }
    arenas_[arena_count_++] = arena;
    return {};
}

// ---------------------------------------------------------------------------
// run_phase — execute one phase slot.
//
// Debug-build ordinal guard: verifies that the Phase ordinal supplied at the
// call site matches the position expected by the sequential walk.  In the
// current skeleton this check cannot fire (kPhaseTable is statically verified
// to be in ordinal order 1..=9 by the static_assert in frame_phase.hpp), but
// the guard is load-bearing for correctness once a future schedule-compilation
// plan introduces a dynamic phase-body registration mechanism that might supply
// phases out of order.  Keeping the check here means the registration path
// must also pass through run_phase, where order violations are caught.
// Specifically, sibling plan #245 (core/frame-loop: phase ownership +
// register_system API) is the planned consumer that will route registered
// system invocations through run_phase(); at that point the ordinal mismatch
// becomes observable when a mis-ordered system registration slips through.
//
// In release builds the ordinal check is compiled out (NDEBUG defined).
//
// MVP phase bodies are empty (no-op): the skeleton ships the ordering
// infrastructure; individual context plans fill the bodies.
//
// Phase::Present (9) additionally drains all registered TransientArena
// instances (perf-budget.md §Allocator Rules #4, plan #239).
// ---------------------------------------------------------------------------

[[nodiscard]] glibre::Result<void>
FrameLoop::run_phase(Phase phase, std::uint8_t expected_ordinal) noexcept {
#ifndef NDEBUG
    const auto ordinal = static_cast<std::uint8_t>(phase);
    if (ordinal != expected_ordinal) {
        return std::unexpected(glibre::Error{core::Error::FramePhaseMisordered});
    }
#else
    (void)expected_ordinal;
#endif

    // --- Empty MVP phase bodies ---
    // Each phase slot executes nothing in this skeleton.  Future plans
    // for each owning context will inject bodies here (or through a
    // registered system dispatch, once the schedule compilation plan
    // lands).  The slot exists to validate the ordering infrastructure
    // and to give the CI a stable hook for phase-level benchmarking.
    switch (phase) {
    case Phase::Input: /* platform — MVP empty */
        break;
    case Phase::Logic: /* gameplay/scripting — reserved empty */
        break;
    case Phase::PhysicsFixed: /* physics — MVP empty */
        break;
    case Phase::Animation: /* animation — reserved empty */
        break;
    case Phase::Transform: /* core — MVP empty */
        break;
    case Phase::CullExtract: /* render — MVP empty */
        break;
    case Phase::RenderSubmit: /* render — MVP empty */
        break;
    case Phase::HotReload: /* core (barrier) — MVP empty */
        break;
    case Phase::Present: /* platform — drains transient arenas at frame end */
        // Drain all registered transient arenas at end of frame.
        // perf-budget.md §Allocator Rules #4: transient arenas must be
        // drained by phase 9 so they do not count against context ceilings.
        // drain() is O(1) per arena; no heap allocation occurs.
        for (std::size_t i = 0; i < arena_count_; ++i) {
            arenas_[i]->drain();
        }
        break;
    }

    return {};  // success — no allocation
}

// ---------------------------------------------------------------------------
// tick — advance one engine frame.
//
// Phases are walked in strict numeric order (1 through 9) using the
// kPhaseTable as the authoritative ordering source.  A per-phase counter
// (expected_ordinal) advances monotonically; run_phase validates it in
// debug builds.
//
// No heap allocation occurs inside this function.
// ---------------------------------------------------------------------------

[[nodiscard]] glibre::Result<void> FrameLoop::tick() noexcept {
    std::uint8_t expected_ordinal = kPhaseMin;

#ifdef GLIBRE_TESTING
    last_tick_phase_count_ = 0;
#endif

    for (const PhaseDesc& desc : kPhaseTable) {
        auto result = run_phase(desc.id, expected_ordinal);
        if (!result) {
            return result;  // propagate error; frame_index_ not incremented
        }
#ifdef GLIBRE_TESTING
        // Record the ordinal of each phase visited, in execution order.
        last_tick_phase_ordinals_[last_tick_phase_count_++] = static_cast<std::uint8_t>(desc.id);
#endif
        ++expected_ordinal;
    }

    ++frame_index_;
    return {};
}

}  // namespace glibre::core
