// core/src/perf_budget.cpp
//
// Implementation of glibre::PerfBudget.
//
// All counter mutations are std::atomic fetch_add / store operations with
// relaxed ordering.  Relaxed ordering is sufficient because:
//   (a) The counter rows are not used to synchronise other data — they only
//       accumulate numeric values.
//   (b) The FrameLoop's sequential phase walk guarantees that reset() is
//       called on the driver thread after all recording threads have completed
//       their phase work; the phase completion is the synchronisation barrier,
//       not the atomic ordering.
//   (c) For the concurrent-recording test (N threads all recording, then join,
//       then assert aggregate) the join provides the required barrier.
//
// Authority: perf-budget.md §Decision, plan #241.

#include "glibre/perf_budget.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace glibre {

// ---------------------------------------------------------------------------
// Internal helper — convert a ContextTag to a row index.
//
// Asserts in debug builds that the tag is in range.  ContextTag::Count_ is
// not a valid caller-supplied value; callers must use named enumerators only.
// ---------------------------------------------------------------------------

namespace {
[[nodiscard]] std::size_t tag_index(ContextTag tag) noexcept {
    const auto idx = static_cast<std::size_t>(tag);
    assert(
        idx < kContextTagCount &&
        "PerfBudget: ContextTag is out-of-range; "
        "caller supplied a raw cast value beyond kContextTagCount"
    );
    return idx;
}
}  // namespace

// ---------------------------------------------------------------------------
// record_cpu
// ---------------------------------------------------------------------------

void PerfBudget::record_cpu(ContextTag tag, std::uint64_t ns) noexcept {
    rows_[tag_index(tag)].cpu_ns.fetch_add(ns, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// record_gpu
// ---------------------------------------------------------------------------

void PerfBudget::record_gpu(ContextTag tag, std::uint64_t ns) noexcept {
    rows_[tag_index(tag)].gpu_ns.fetch_add(ns, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// record_heap_alloc
// ---------------------------------------------------------------------------

void PerfBudget::record_heap_alloc(ContextTag tag, std::uint64_t bytes) noexcept {
    rows_[tag_index(tag)].heap_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// record_heap_free — saturating subtract
// ---------------------------------------------------------------------------

void PerfBudget::record_heap_free(ContextTag tag, std::uint64_t bytes) noexcept {
    auto& atom = rows_[tag_index(tag)].heap_bytes;
    // Saturating subtract: loop until we can CAS a value that does not wrap.
    // In practice only one or zero iterations execute because heap_free is
    // called on the allocating thread and bytes <= current live bytes.
    std::uint64_t current = atom.load(std::memory_order_relaxed);
    while (true) {
        const std::uint64_t desired = (bytes <= current) ? (current - bytes) : 0u;
        if (atom.compare_exchange_weak(current, desired, std::memory_order_relaxed)) {
            break;
        }
        // current updated by CAS failure — retry
    }
}

// ---------------------------------------------------------------------------
// sample
// ---------------------------------------------------------------------------

PerfBudgetSample PerfBudget::sample(ContextTag tag) const noexcept {
    const auto& row = rows_[tag_index(tag)];
    return PerfBudgetSample{
        .cpu_ns     = row.cpu_ns.load(std::memory_order_relaxed),
        .gpu_ns     = row.gpu_ns.load(std::memory_order_relaxed),
        .heap_bytes = row.heap_bytes.load(std::memory_order_relaxed),
    };
}

// ---------------------------------------------------------------------------
// reset — zero all counter rows (called by FrameLoop at phase 9)
// ---------------------------------------------------------------------------

void PerfBudget::reset() noexcept {
    for (auto& row : rows_) {
        row.cpu_ns.store(0u, std::memory_order_relaxed);
        row.gpu_ns.store(0u, std::memory_order_relaxed);
        row.heap_bytes.store(0u, std::memory_order_relaxed);
    }
}

}  // namespace glibre
