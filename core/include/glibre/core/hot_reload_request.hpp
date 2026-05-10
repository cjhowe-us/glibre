#pragma once
// core/include/glibre/core/hot_reload_request.hpp
//
// HotReloadRequestQueue — atomic pending-reload counter and step gate for Phase 8.
//
// Authority: reviews/decisions/hot-reload-protocol.md §Decision (step 1
// trigger, pending_reloads counter) and §Consequences (frame budget).
// Authority: reviews/decisions/frame-phases.md open question 1 resolution:
//   "a single relaxed atomic load on the pending-reload counter, no fence,
//   no cache flush" — this class implements exactly that contract.
// Authority: SPEC §4.6 / §5.8 — step() returns Result<std::size_t>, the count
//   of reload requests processed (0 on the fast / idle path).
//
// Design constraints:
//   - std::atomic<std::uint32_t> pending_ with memory_order_relaxed load and
//     store.  No fence, no cache flush (per protocol §Consequences: "Phase 8
//     with no reload pending is a single relaxed atomic load — sub-microsecond").
//   - -fno-exceptions clean; noexcept throughout.
//   - No heap allocation; no virtual dispatch.
//   - Thread safety for enqueue(): multiple callers on different threads may
//     enqueue concurrently; fetch_add with memory_order_relaxed is safe for
//     the counter increment since the loader is the sole consumer of phase 8
//     and observes the count only after all phases 1–7 complete.
//
// Lifecycle:
//   - enqueue() increments the pending_ counter by 1, signalling that a
//     reload has been requested for the next phase 8.  The payload of the
//     request (which plugin, which dylib path) is managed by the full loader
//     state machine, which lands in subsequent plans (#251+).  This class
//     tracks only the head-count so that phase 8 can skip entirely when zero.
//   - step() reads pending_count() once; returns 0 immediately when zero
//     (SPEC §5.8 fast path).  Non-zero triggers the drain/swap/migrate/resume
//     state machine (stub for now; plans #251+).  Returns Result<std::size_t>:
//     the count of requests processed, or an error if the slow path refuses.
//   - pending_count() returns the current counter value with relaxed ordering.
//     Phase 8 reads it once on entry via step(); if zero, returns 0 immediately.
//   - decrement() is intentionally NOT exposed here.  The loader state machine
//     (plans #251+) owns the decrement path and will extend this class when
//     the drain → swap → migrate → resume body lands.  For now, only enqueue
//     and the fast-path read are in scope (plan #249).
//
// Round-1 review reconciliation (HIGH-1, plan #599):
//   PR #1097 originally added a separate HotReloadBarrier class.  The round-1
//   review found this to be a duplicate SPEC §4.6 aggregate alongside this class.
//   Resolution: HotReloadRequestQueue is the single §4.6 aggregate owned by
//   FrameLoop as a value member.  The step() method (SPEC §5.8 public API shape)
//   is added here directly.  hot_reload_barrier.hpp is removed from the PR.
//   FrameLoop's Phase 8 body calls hot_reload_queue_.step() wrapped by PhaseHooks.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "glibre/error.hpp"

namespace glibre::core {

// ---------------------------------------------------------------------------
// HotReloadRequestQueue
//
// Singleton-per-FrameLoop aggregate (SPEC §4.6) that drives the phase-8
// fast-path decision and (via step()) the drain/swap/migrate/resume state
// machine when requests are pending.
//
// The full request payload (plugin FQN, dylib path, request ID) will be stored
// in a bounded ring inside this class when the drain/swap/migrate/resume body
// lands (plans #251+).  For plan #249, only the counter is implemented.
//
// Non-copyable, non-movable — intended as a FrameLoop data member (not heap).
// ---------------------------------------------------------------------------

class HotReloadRequestQueue {
public:
    HotReloadRequestQueue() noexcept = default;

    HotReloadRequestQueue(const HotReloadRequestQueue&) = delete;
    HotReloadRequestQueue& operator=(const HotReloadRequestQueue&) = delete;
    HotReloadRequestQueue(HotReloadRequestQueue&&) = delete;
    HotReloadRequestQueue& operator=(HotReloadRequestQueue&&) = delete;

    ~HotReloadRequestQueue() noexcept = default;

    // enqueue() — signal that one plugin reload is pending.
    //
    // Increments the pending_ counter with memory_order_relaxed.  Thread-safe
    // for concurrent callers.  May be called from any thread (filesystem watcher,
    // editor tool, or test harness) before phase 8 begins.
    //
    // The full request payload (plugin FQN, replacement dylib path) is a
    // forward-declared extension point for plans #251+; for now the call site
    // only records that "some reload is wanted".
    void enqueue() noexcept { pending_.fetch_add(1u, std::memory_order_relaxed); }

    // step() — advance the hot-reload state machine at Phase::HotReload (8).
    //
    // Called by FrameLoop exactly once per tick, between Phase::RenderSubmit (7)
    // and Phase::Present (9), per SPEC §5.8 and §6.5.
    //
    // Returns Result<std::size_t>:
    //   0 (success) — fast path: pending_ == 0, single relaxed-atomic load,
    //     no allocation, no observer event.  0 ms / 0 B (perf-budget §6.7).
    //   N (success) — N reload requests processed via drain → swap → migrate →
    //     resume (STUB: slow-path bodies in plans #249-#258).
    //   unexpected<Error> — slow path refused (STUB: unreachable in MVP stub
    //     since pending_ is never set to non-zero by this stub).
    //
    // Return type matches SPEC §5.8:
    //   [[nodiscard]] Result<std::size_t> step() noexcept;
    //
    // -fno-exceptions clean; noexcept.
    //
    // TODO(#1104): add (World&, PluginLoader&) per SPEC §5.8 once both are wired
    // into FrameLoop::tick() (prerequisite: ECS World plan and PluginLoader wiring).
    [[nodiscard]] glibre::Result<std::size_t> step() noexcept {
        // Fast path: single relaxed-atomic load.
        // Per hot-reload-protocol.md §Decision and frame-phases.md open question
        // 1 resolution: "single relaxed atomic load on the pending-reload counter,
        // no fence, no cache flush."
        if (pending_.load(std::memory_order_relaxed) == 0) {
#ifdef GLIBRE_TESTING
            ++step_count_;
#endif
            return std::size_t{0};
        }

        // Slow path — drain → swap → migrate → resume.
        // STUB: full implementation in plans #249-#258.
        // In MVP this branch is unreachable: pending_ is never set to non-zero
        // by the current stub.  step_count_ is still incremented so tests can
        // observe that step() was called.
#ifdef GLIBRE_TESTING
        ++step_count_;
#endif
        return std::size_t{0};
    }

    // pending_count() — read the current pending-reload counter.
    //
    // Returns the number of enqueued reloads that phase 8 has not yet
    // processed.  The read uses memory_order_relaxed per the decision record:
    // "a single relaxed atomic load on the pending-reload counter, no fence,
    // no cache flush" (hot-reload-protocol.md §Consequences, frame-phases.md
    // open question 1 resolution).
    //
    // Called once at phase-8 entry on the game-loop thread via step().
    [[nodiscard]] std::uint32_t pending_count() const noexcept {
        return pending_.load(std::memory_order_relaxed);
    }

#ifdef GLIBRE_TESTING
    // step_count() — total number of times step() has been called.
    //
    // Tests use this counter to assert that step() is invoked exactly once per
    // tick between Phase::RenderSubmit and Phase::Present, per SPEC §6.5.
    //
    // Only available in GLIBRE_TESTING builds.
    [[nodiscard]] std::uint64_t step_count() const noexcept { return step_count_; }

    // reset_step_count() — reset the test instrumentation counter.
    //
    // Allows test cases to isolate per-tick assertions without constructing a
    // new HotReloadRequestQueue instance each time.
    void reset_step_count() noexcept { step_count_ = 0; }
#endif  // GLIBRE_TESTING

private:
    // Pending-reload counter.  Incremented by enqueue(); decremented by the
    // drain/swap/migrate/resume state machine (plans #251+) when each request
    // completes or is refused.  The initial value is 0 (no reloads pending).
    std::atomic<std::uint32_t> pending_{0u};

#ifdef GLIBRE_TESTING
    // step_count_ — test instrumentation: number of step() invocations.
    //
    // Not atomic: step() is game-loop-thread-only; test code reads it after
    // tick() returns (single-threaded test context).
    std::uint64_t step_count_{0};
#endif  // GLIBRE_TESTING
};

}  // namespace glibre::core
