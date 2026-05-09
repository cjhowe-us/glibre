#pragma once
// core/include/glibre/perf_budget.hpp
//
// glibre::PerfBudget — per-context CPU/GPU/heap performance counter store.
//
// Design (perf-budget.md §Decision, §CI Gate Spec):
//   Records CPU nanoseconds (sim + submit), GPU nanoseconds (stub — real
//   capture from MTLCounterSampleBuffer lands in a later render plan), and
//   heap live bytes per ContextTag.  Counters are atomic so any context
//   can record from its owning thread without external synchronisation.
//
//   The FrameLoop calls reset() at the end of Phase::Present (phase 9)
//   to zero all per-frame counters before the next frame's work begins.
//
//   Intended use in MVP:
//     PerfBudget budget;
//     budget.record_cpu(ContextTag::Physics, elapsed_ns);
//     budget.record_heap_alloc(ContextTag::Render, bytes);
//     PerfBudgetSample s = budget.sample(ContextTag::Physics);
//     // ... at frame end: FrameLoop calls budget.reset()
//
//   Thread safety:
//     record_* and sample() are safe to call from multiple threads
//     concurrently (atomics with relaxed ordering for accumulators;
//     see §Concurrency note below).
//     reset() is called by the frame loop on the driver thread and must
//     not race with concurrent record_* calls.  Callers must ensure that
//     all recording in phases 1-8 is complete before phase 9 resets.
//     (This is guaranteed by the FrameLoop's sequential phase walk.)
//
//   PHILOSOPHY §11: std::atomic is a language/runtime utility that EASTL
//   does not own; it is retained here per the PHILOSOPHY §11 carve-out for
//   std::atomic, std::thread, and std::mutex.  EASTL containers / strings
//   are used where applicable; std::vector/string are not used here.
//
// -fno-exceptions clean.  No heap allocation after construction.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>

namespace glibre {

// ---------------------------------------------------------------------------
// ContextTag — bounded-context identifiers matching perf-budget.md table
//
// These values index into PerfBudget's counter rows.  The enum is closed;
// adding a context requires an amendment to perf-budget.md.
// ---------------------------------------------------------------------------

enum class ContextTag : std::uint8_t {
    Core = 0,
    Platform,
    Data,
    Shader,
    Render,
    Geometry,
    Physics,
    Content,
    Tools,
    E2E,  // test-only context; no shipping-build budget ceiling
          // (perf-budget.md §Budget Table: e2e row is n/a for all cells;
          //  perf-budget.md §Rationale: encoding n/a as a row rather than
          //  gating behind GLIBRE_TESTING avoids ODR violations from the
          //  library being compiled without GLIBRE_TESTING while tests are).
};

// kContextTagCount — number of valid ContextTag entries.
//
// Kept as a free constexpr rather than a Count_ enumerator inside the enum so
// that an out-of-range integer cast to ContextTag cannot silently alias a
// sentinel value (pattern from frame_phase.hpp §kPhaseCount).
inline constexpr std::size_t kContextTagCount = 10u;

// ---------------------------------------------------------------------------
// PerfBudgetSample — snapshot of one context's per-frame counters
// ---------------------------------------------------------------------------

struct PerfBudgetSample {
    std::uint64_t cpu_ns{0};     // accumulated CPU nanoseconds this frame
    std::uint64_t gpu_ns{0};     // accumulated GPU nanoseconds (stub; 0 until Metal plan)
    std::uint64_t heap_bytes{0}; // live heap bytes at snapshot time
};

// ---------------------------------------------------------------------------
// PerfBudget — per-context accumulating counter store
//
// One instance is owned by the FrameLoop (or test fixture).  The instance
// must outlive any thread that calls record_* on it.
// ---------------------------------------------------------------------------

class PerfBudget {
public:
    PerfBudget() noexcept = default;

    // Non-copyable, non-movable.  Counter rows are long-lived; moving them
    // would dangle pointers held by the frame loop or test fixtures.
    PerfBudget(const PerfBudget&) = delete;
    PerfBudget& operator=(const PerfBudget&) = delete;
    PerfBudget(PerfBudget&&) = delete;
    PerfBudget& operator=(PerfBudget&&) = delete;

    ~PerfBudget() noexcept = default;

    // record_cpu(tag, ns) — add `ns` nanoseconds to the CPU counter for `tag`.
    //
    // Thread-safe: uses std::atomic fetch_add with relaxed ordering.
    // Callers accumulate the delta for one phase slice; the total is read
    // at the end of the frame via sample().
    void record_cpu(ContextTag tag, std::uint64_t ns) noexcept;

    // record_gpu(tag, ns) — STUB.  Does NOT capture real GPU time.
    //
    // Real GPU nanoseconds come from MTLCounterSampleBuffer, which is
    // implemented by the render plan (out of scope for plan #241).  Until
    // that plan lands, callers may pass through 0 or placeholder values.
    // Follow-up search tag: [GPU-COUNTER-STUB] — grep for this tag when
    // wiring up MTLCounterSampleBuffer to replace this stub.
    //
    // Accumulates `ns` into the GPU counter for `tag`.
    // Thread-safe: same atomics contract as record_cpu.
    void record_gpu(ContextTag tag, std::uint64_t ns) noexcept;

    // record_heap_alloc(tag, bytes) — add `bytes` to the heap counter for `tag`.
    //
    // Called by PerContextAllocator on every successful allocation.
    // Thread-safe: same atomics contract as record_cpu.
    void record_heap_alloc(ContextTag tag, std::uint64_t bytes) noexcept;

    // record_heap_free(tag, bytes) — subtract `bytes` from the heap counter for `tag`.
    //
    // Called by PerContextAllocator on every deallocation.  Saturating:
    // will not underflow below zero (wrapping on unsigned is defined but
    // physically meaningless; the check clamps before the subtract).
    // Thread-safe: same atomics contract as record_cpu.
    void record_heap_free(ContextTag tag, std::uint64_t bytes) noexcept;

    // sample(tag) — return a snapshot of the current counters for `tag`.
    //
    // Reads three atomics with relaxed ordering.  The returned snapshot is
    // coherent only on the thread that observes it after all recording in
    // the current frame has completed (e.g. after phase 8 and before reset()
    // in phase 9).  Under concurrent recording the snapshot is a point-in-time
    // read; callers that need a consistent snapshot must synchronise externally.
    [[nodiscard]] PerfBudgetSample sample(ContextTag tag) const noexcept;

    // reset() — zero all per-frame counters across all contexts.
    //
    // Called by FrameLoop at the end of Phase::Present (phase 9) before
    // the next frame's work begins.  Must be called from the frame-loop
    // driver thread with no concurrent record_* calls in flight.
    void reset() noexcept;

private:
    // Per-context counter rows.  Each row holds three atomics.
    // Layout: rows[tag].{cpu_ns, gpu_ns, heap_bytes}.
    // Using std::atomic<uint64_t> — PHILOSOPHY §11 carve-out for std::atomic.
    struct Row {
        std::atomic<std::uint64_t> cpu_ns{0};
        std::atomic<std::uint64_t> gpu_ns{0};
        std::atomic<std::uint64_t> heap_bytes{0};
    };

    std::array<Row, kContextTagCount> rows_{};
};

}  // namespace glibre
