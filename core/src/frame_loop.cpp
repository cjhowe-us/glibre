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
//
// PerfBudget reset at phase 9 (Phase::Present) is per
// perf-budget.md §CI Gate Spec and plan #241.
//
// PhaseRegistry system dispatch at each phase is per plan #245
// (phase ownership + system register API).  Registered systems are
// invoked inside run_phase() after the built-in MVP phase body,
// in registration order.
//
// PhaseHooks / set_phase_hooks() / Phase 8 hot_reload_queue_.step() call site
// are per plan #599 (HotReloadBarrier call-site wiring).  Phase 8 fires
// on_enter before step() and on_exit after, per SPEC §5.7 / §6.5.
// execute_pending_reloads removed: HotReloadRequestQueue::step() (SPEC §5.8)
// now encapsulates both the fast-path check and the slow-path stub.

#include "glibre/core/frame_loop.hpp"

#include <cstdint>
#include <optional>

#include "glibre/core/frame_phase.hpp"
#include "glibre/core/hot_reload_request.hpp"
#include "glibre/core/phase_registry.hpp"
#include "glibre/core/world_tick.hpp"
#include "glibre/error.hpp"
#include "glibre/perf_budget.hpp"
#include "glibre/transient_arena.hpp"

namespace glibre::core {

// ---------------------------------------------------------------------------
// register_transient_arena — add an arena to the drain registry.
// ---------------------------------------------------------------------------

[[nodiscard]] glibre::Result<void>
FrameLoop::register_transient_arena(glibre::TransientArena* arena) noexcept {
    // Null pointer is a precondition violation.  Return InvalidArgument so
    // callers can distinguish "null" from "registry full" without depending
    // on a debug assert that disappears in release builds.
    if (arena == nullptr) {
        return std::unexpected(
            glibre::Error{
                core::Error::NullArgument,
                glibre::ErrorContext{
                    .file = "core/src/frame_loop.cpp",
                    .line = __LINE__,
                    .detail = "register_transient_arena: null arena pointer",
                },
            }
        );
    }
    if (arena_count_ >= kMaxTransientArenas) {
        return std::unexpected(
            glibre::Error{
                core::Error::OutOfBudget,
                glibre::ErrorContext{
                    .file = "core/src/frame_loop.cpp",
                    .line = __LINE__,
                    .detail = "transient arena registry full",
                },
            }
        );
    }
    arenas_[arena_count_++] = arena;
    return {};
}

// ---------------------------------------------------------------------------
// set_perf_budget — register (or replace, or detach) a PerfBudget.
//
// Out-of-line for seam consistency with register_transient_arena (both are
// public mutators that touch FrameLoop internals).  The assignment is trivial;
// the doc-comment in the header carries the full contract.
// ---------------------------------------------------------------------------

void FrameLoop::set_perf_budget(glibre::PerfBudget* budget) noexcept { perf_budget_ = budget; }

// ---------------------------------------------------------------------------
// set_phase_registry — attach (or detach) a PhaseRegistry.  (plan #245)
//
// Out-of-line for seam consistency.  See header doc-comment for contract.
// ---------------------------------------------------------------------------

void FrameLoop::set_phase_registry(PhaseRegistry* registry) noexcept { phase_registry_ = registry; }

// ---------------------------------------------------------------------------
// set_phase_hooks — register observer hooks for a specific phase.  (plan #599)
//
// Currently stores hooks for Phase::HotReload (8) only.  Future plans extend
// storage to all nine phases; for now a single PhaseHooks slot is sufficient
// for the editor/e2e observer pattern described in the plan.
//
// Returns core::Error::InvalidArgument for any phase other than Phase::HotReload
// so that callers wiring an observer for an unsupported phase receive an explicit
// diagnostic rather than a silent no-op (LOW-1 round-1 review).  Future plans
// extend the slot table and widen the accepted phase set.
// ---------------------------------------------------------------------------

[[nodiscard]] glibre::Result<void>
FrameLoop::set_phase_hooks(Phase phase, PhaseHooks hooks) noexcept {
    if (phase == Phase::HotReload) {
        phase_8_hooks_ = hooks;
        return {};
    }
    // Other phases: not yet supported — return InvalidArgument so callers get
    // an explicit diagnostic instead of a silent no-op (LOW-1, round-1 review).
    return std::unexpected(
        glibre::Error{
            core::Error::InvalidArgument,
            glibre::ErrorContext{
                .file = "core/src/frame_loop.cpp",
                .line = __LINE__,
                .detail = "set_phase_hooks: only Phase::HotReload is supported in this plan",
            },
        }
    );
}

// ---------------------------------------------------------------------------
// present_reset_perf_budget — Phase::Present step (1).
//
// Resets all perf-budget counters unconditionally when a budget is registered.
// Called before present_drain_arenas() so that counters are zeroed for frame
// N+1 even when a transient-arena leak is detected in the same phase.
// ---------------------------------------------------------------------------

void FrameLoop::present_reset_perf_budget() noexcept {
    if (perf_budget_ != nullptr) {
        perf_budget_->reset();
    }
}

// ---------------------------------------------------------------------------
// present_drain_arenas — Phase::Present steps (2) & (3).
//
// Drains every registered TransientArena unconditionally.  In
// GLIBRE_ALLOC_STRICT builds, asserts each arena was empty before draining
// (drain-then-aggregate pattern: all arenas are drained regardless of leak
// detection); returns the first leak error encountered, if any.
//
// The `leak_captured` flag makes the "no leak yet" state explicit rather than
// relying on the default-success reading of Result<void>, which reads inverted
// to human expectations when used as a sentinel.
// ---------------------------------------------------------------------------

[[nodiscard]] glibre::Result<void> FrameLoop::present_drain_arenas() noexcept {
#ifdef GLIBRE_ALLOC_STRICT
    bool leak_captured = false;
    std::optional<glibre::Error> first_leak;
#endif

    for (std::size_t i = 0; i < arena_count_; ++i) {
#ifdef GLIBRE_ALLOC_STRICT
        auto check = arenas_[i]->assert_drained();
        if (!check && !leak_captured) {
            // Capture the first leak; subsequent arenas still get drained.
            leak_captured = true;
            first_leak.emplace(std::move(check.error()));
        }
#endif
        arenas_[i]->drain();  // always drain, regardless of strict-mode result
    }

#ifdef GLIBRE_ALLOC_STRICT
    if (leak_captured) {
        return std::unexpected(std::move(*first_leak));
    }
#endif

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
    case Phase::HotReload: {
        // Phase 8: hot-reload drain barrier (SPEC §4.6, plan #599, plan #249).
        //
        // Execution order (plan #599):
        //   (A) on_enter hook fires (if set via set_phase_hooks(Phase::HotReload))
        //   (B) hot_reload_queue_.step() — single relaxed-atomic load on fast path
        //       (pending_ == 0); drain→swap→migrate→resume on slow path
        //       (SPEC §6.7; slow-path bodies in plans #249-#258).
        //   (C) on_exit hook fires (if set)
        //
        // hot_reload_queue_.step() returns Result<std::size_t>:
        //   0 — fast path (idle, no reloads pending).
        //   N — N requests processed.
        //   unexpected — slow-path refusal (propagated to tick() caller).
        //
        // Per hot-reload-protocol.md §Decision: "Plugin code does not execute
        // during phase 8.  No system bodies run."  Both fast and slow paths
        // return directly from this case, bypassing system dispatch.
        //
        // FOLLOWUP(plan-981-wiring): wire FramePhaseTracker / validate_drain_phase
        // once that plan lands.  The GLIBRE_TESTING injection below exercises
        // the "tick halts on phase failure" path from plan #247 Unit Test Plan.
        //
        // GLIBRE_TESTING injection: when inject_phase8_failure_ is armed,
        // simulate a drain-phase refusal so tests can verify that frame_counter_
        // and world_tick_ do not advance when Phase 8 fails.
#ifdef GLIBRE_TESTING
        if (inject_phase8_failure_) {
            return std::unexpected(glibre::Error{core::Error::FramePhaseMisordered});
        }
#endif
        // (A) on_enter hook — fires before step().
        if (phase_8_hooks_.on_enter != nullptr) {
            phase_8_hooks_.on_enter(Phase::HotReload);
        }
        // (B) step() — single relaxed-atomic load on fast path (SPEC §6.7).
        //     Returns 0 when idle (fast path, no allocation, no observer event).
        //     Propagate slow-path refusal errors back to tick() if they occur.
        auto step_result = hot_reload_queue_.step();
        // (C) on_exit hook — fires after step() regardless of result.
        if (phase_8_hooks_.on_exit != nullptr) {
            phase_8_hooks_.on_exit(Phase::HotReload);
        }
        if (!step_result) {
            return std::unexpected(std::move(step_result.error()));
        }
        // Phase 8 never reaches system dispatch (no system bodies per protocol).
        return {};
    }
    case Phase::Present: /* platform — drains transient arenas at frame end */
        // Phase 9 bookkeeping order (perf-budget.md §CI Gate Spec, plan #241;
        //                            plan #247 §Scope):
        //
        // (0a) Acquire next drawable — STUB (render plugin owns real impl).
        //      Interface only: the render plugin will register a callback here
        //      once the render-plugin plan lands.  MVP: no-op.
        // (0b) Present prior submit fence — STUB (render plugin owns real impl).
        //      Phase 7 (RenderSubmit) enqueued the command buffer and signals
        //      the submit fence; Phase 9 presents that prior work and waits on
        //      the fence for one-frame pipeline semantics.  MVP: no-op stub.
        //
        // (1) Reset per-frame perf-budget counters UNCONDITIONALLY so that
        //     counters do not carry over into the next frame regardless of
        //     whether a transient-arena leak is detected below.  Resetting
        //     first keeps the leak-detection path diagnostic: the caller
        //     sees the error but the budget is already clean for frame N+1.
        // (2) & (3) Drain all registered transient arenas; in
        //     GLIBRE_ALLOC_STRICT builds assert each was empty before drain
        //     and return the first leak error (perf-budget.md §Allocator
        //     Rules #4).  The strict gate compiles to zero cost in non-strict
        //     builds; CI diagnostic builds define -DGLIBRE_ALLOC_STRICT=1.
        // (4) Advance world tick (plan #247, frame-phases.md §Phase 9).
        //     Both WorldTick::value and WorldTick::change_tick increment once.
        //     Simulation of frame N+1 may begin after this returns.
        // (5) Increment frame_counter_ (plan #247 §Scope).
        //
        // NOTE: steps (1)–(3) are core-owned bookkeeping running inside the
        //   platform-owned Phase::Present slot.  This is a deliberate
        //   "core barrier carve-out" documented in the existing comment and
        //   tracked under [SPIKE] iterate-frame-phases-core-barrier-carveout.

        // (0a) Stub: acquire next drawable.
        // TODO(plan:render-swapchain): render plugin registers acquire callback.

        // (0b) Stub: present prior submit fence.
        // TODO(plan:render-swapchain): render plugin registers present callback.

        present_reset_perf_budget();          // (1) zero counters before leak detect
        if (auto r = present_drain_arenas();  // (2)+(3) drain + optional leak error
            !r) {
            return r;
        }
        advance_world_tick(world_tick_);  // (4) tick N complete; N+1 may begin
        ++frame_counter_;                 // (5) present-phase frame counter
        break;
    }

    // --- System dispatch (plan #245) ---
    // After the built-in MVP phase body, invoke all registered systems for
    // this phase in registration order.  No allocation occurs here — the
    // iteration is a range walk over a pre-built std::pmr::vector inside
    // PhaseRegistry::for_each_system().
    //
    // Null phase_registry_ means no systems registered (the default state).
    if (phase_registry_ != nullptr) {
        phase_registry_->for_each_system(phase, [](const PhaseSystemFn& fn) noexcept { fn(); });
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
        // Record the ordinal and post-phase world_tick for each phase visited,
        // in execution order.  The world_tick snapshot is taken after run_phase
        // returns so that Phase::Present's advance_world_tick() is captured at
        // index 8 (kPhaseCount - 1), while indices 0..=7 retain the pre-advance
        // value — this proves mid-frame stability (world_tick invariant).
        last_tick_phase_ordinals_[last_tick_phase_count_] = static_cast<std::uint8_t>(desc.id);
        last_tick_per_phase_world_ticks_[last_tick_phase_count_] = world_tick_;
        ++last_tick_phase_count_;
#endif
        ++expected_ordinal;
    }

    ++frame_index_;
    return {};
}

}  // namespace glibre::core
