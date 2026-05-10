// tests/core/hot_reload/hot_reload_request_test.cpp
//
// Catch2 unit tests for glibre::core::HotReloadRequestQueue and the
// Phase 8 fast-path wired in glibre::core::FrameLoop.
//
// Authority: core/include/glibre/core/hot_reload_request.hpp,
//            core/src/frame_loop.cpp (Phase::HotReload body),
//            reviews/decisions/hot-reload-protocol.md §Decision (step 1
//            trigger; pending_reloads counter; single relaxed atomic load),
//            reviews/decisions/frame-phases.md open question 1 resolution.
//
// Plan: #249 — feat(core): pending-reload counter + phase 8 fast-path no-op.
//
// Named test cases (plan #249 Unit Test Plan + DoD):
//   - phase_8_is_no_op_when_no_pending_reloads
//   - enqueue_increments_pending_count
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS usage.
//   - No filesystem access — FrameLoop is constructed in-memory.
//   - GLIBRE_TESTING is propagated transitively from glibre::core when
//     GLIBRE_BUILD_TESTS=ON (core/CMakeLists.txt PUBLIC definition).

#include <catch2/catch_test_macros.hpp>

#include "glibre/core/frame_loop.hpp"
#include "glibre/core/hot_reload_request.hpp"
#include "glibre/error.hpp"

// ===========================================================================
// Test: phase_8_is_no_op_when_no_pending_reloads
//
// Verifies that Phase 8 returns success immediately (true no-op) when the
// HotReloadRequestQueue reports zero pending reloads.
//
// Authority: reviews/decisions/hot-reload-protocol.md §Decision:
//   "The state machine is a true no-op when no reload is pending (answers
//    frame-phases.md open question 1: a single relaxed atomic load on the
//    pending-reload counter, no fence, no cache flush)."
//
// Approach: construct a FrameLoop (which owns the queue), do NOT enqueue
// any request, then tick() one full frame and assert success.  The frame
// counter must advance to 1, proving all nine phases — including Phase 8
// with zero pending reloads — completed without error.
// ===========================================================================

TEST_CASE("phase_8_is_no_op_when_no_pending_reloads", "[core][hot_reload]") {
    glibre::core::FrameLoop loop;

    // Precondition: no reloads enqueued.
    CHECK(loop.hot_reload_queue().pending_count() == 0u);

    // tick() must succeed — Phase 8 fast-path returns {} immediately.
    const auto result = loop.tick();
    REQUIRE(result.has_value());

    // Proof that Phase::Present executed (frame_counter_ advanced).
    CHECK(loop.frame_counter() == 1u);
}

// ===========================================================================
// Test: enqueue_increments_pending_count
//
// Verifies that each call to HotReloadRequestQueue::enqueue() increments
// the pending_ counter by exactly 1, visible via pending_count().
//
// Authority: hot_reload_request.hpp — enqueue() uses
//   pending_.fetch_add(1u, memory_order_relaxed).
//
// Approach: construct a standalone HotReloadRequestQueue, call enqueue()
// multiple times, and assert the counter advances monotonically.  This is
// a unit test of the counter primitive in isolation; the FrameLoop wiring
// is covered separately in phase_8_is_no_op_when_no_pending_reloads.
// ===========================================================================

TEST_CASE("enqueue_increments_pending_count", "[core][hot_reload]") {
    glibre::core::HotReloadRequestQueue queue;

    // Initial state: zero pending.
    CHECK(queue.pending_count() == 0u);

    // First enqueue: counter must reach 1.
    queue.enqueue();
    CHECK(queue.pending_count() == 1u);

    // Second enqueue: counter must reach 2.
    queue.enqueue();
    CHECK(queue.pending_count() == 2u);

    // Third enqueue: counter must reach 3.
    queue.enqueue();
    CHECK(queue.pending_count() == 3u);
}
