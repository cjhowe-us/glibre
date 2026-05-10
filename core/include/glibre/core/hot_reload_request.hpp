#pragma once
// core/include/glibre/core/hot_reload_request.hpp
//
// HotReloadRequestQueue — atomic pending-reload counter exposed to Phase 8.
//
// Authority: reviews/decisions/hot-reload-protocol.md §Decision (step 1
// trigger, pending_reloads counter) and §Consequences (frame budget).
// Authority: reviews/decisions/frame-phases.md open question 1 resolution:
//   "a single relaxed atomic load on the pending-reload counter, no fence,
//   no cache flush" — this class implements exactly that contract.
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
//   - pending_count() returns the current counter value with relaxed ordering.
//     Phase 8 reads it once on entry; if zero, the phase returns immediately.
//   - decrement() is intentionally NOT exposed here.  The loader state machine
//     (plans #251+) owns the decrement path and will extend this class when
//     the drain → swap → migrate → resume body lands.  For now, only enqueue
//     and the fast-path read are in scope (plan #249).

#include <atomic>
#include <cstdint>

namespace glibre::core {

// ---------------------------------------------------------------------------
// HotReloadRequestQueue
//
// Singleton-per-FrameLoop counter that drives the phase-8 fast-path decision.
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

    // pending_count() — read the current pending-reload counter.
    //
    // Returns the number of enqueued reloads that phase 8 has not yet
    // processed.  The read uses memory_order_relaxed per the decision record:
    // "a single relaxed atomic load on the pending-reload counter, no fence,
    // no cache flush" (hot-reload-protocol.md §Consequences, frame-phases.md
    // open question 1 resolution).
    //
    // Called once at phase-8 entry on the game-loop thread.
    [[nodiscard]] std::uint32_t pending_count() const noexcept {
        return pending_.load(std::memory_order_relaxed);
    }

private:
    // Pending-reload counter.  Incremented by enqueue(); decremented by the
    // drain/swap/migrate/resume state machine (plans #251+) when each request
    // completes or is refused.  The initial value is 0 (no reloads pending).
    std::atomic<std::uint32_t> pending_{0u};
};

}  // namespace glibre::core
