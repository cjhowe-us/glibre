// core/src/alloc/per_context_allocator.cpp
//
// Implementation of glibre::PerContextAllocator.
// Authority: reviews/decisions/perf-budget.md §Allocator Rules #1-3, plan #238.
//
// ## Backing allocation
//
//   posix_memalign() is used as the raw backing allocator per PHILOSOPHY §11
//   ("raw allocation carve-out below all allocator abstractions").  It
//   satisfies arbitrary power-of-two alignment without padding, unlike
//   std::aligned_alloc which requires size to be a multiple of align.
//
//   OOM (posix_memalign returns non-zero) calls std::abort() because the
//   engine does not attempt to recover from system-level out-of-memory; the
//   CI diagnostic build catches budget overruns via OutOfBudget before any
//   actual OOM occurs.
//
// ## Ceiling enforcement (GLIBRE_ALLOC_STRICT)
//
//   The pre-increment compare-exchange loop ensures atomicity between the
//   ceiling check and the byte-counter update.  If two concurrent allocations
//   both pass the ceiling check before either commits, the later one may still
//   push bytes_used above the ceiling by at most `bytes` (one allocation unit).
//   This is acceptable: the ceiling is a soft circuit-breaker in CI, not a
//   hard memory limit.  The test for per_context_allocator_rejects_alloc_over_ceiling
//   runs single-threaded so the test is deterministic; the threadsafe test
//   only checks that the total counter is correct, not that concurrent
//   allocations near the ceiling are perfectly serialized.
//
// ## Shipping-mode soft warning (Rule #3)
//
//   When GLIBRE_ALLOC_STRICT is not set, ceiling overruns emit spdlog::warn
//   once per ContextTag for the lifetime of the PerContextAllocator instance,
//   via the per-instance warn_once_flag_ member in alloc.hpp.  The flag is
//   per-instance (not TU-static) so tests can construct multiple allocators
//   with independent warn state and verify the warn-once path independently.
//   Full once-per-tag-per-frame throttling (linked to FrameLoop phase 9 drain)
//   is deferred to plan #241 (perf-budget framework).
//
// ## bytes == 0 allocations
//
//   bytes == 0 is forwarded as bytes = 1 to posix_memalign (allocating a
//   uniquely-addressed pointer) but the byte counter is NOT incremented.
//   This matches the C++ standard's "zero-size allocation returns a unique
//   non-null pointer" contract.

#include <cstdlib>  // posix_memalign, free, std::abort

#include <spdlog/spdlog.h>

#include "glibre/alloc.hpp"

namespace glibre {

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------

PerContextAllocator::PerContextAllocator(ContextTag tag, std::uint64_t ceiling_bytes) noexcept
    : tag_{tag},
      ceiling_{ceiling_bytes} {
    register_allocator(*this);
}

PerContextAllocator::PerContextAllocator(ContextTag tag) noexcept
    : PerContextAllocator{tag, kContextCeilings[static_cast<std::uint8_t>(tag)]} {}

// ---------------------------------------------------------------------------
// allocate
// ---------------------------------------------------------------------------

Result<void*> PerContextAllocator::allocate(std::size_t bytes, std::size_t align) noexcept {
    // Normalise alignment: 0 → alignof(std::max_align_t), less than
    // sizeof(void*) → sizeof(void*) (posix_memalign minimum).
    if (align == 0) {
        align = alignof(std::max_align_t);
    }
    // posix_memalign requires align >= sizeof(void*) and a power of two.
    // Callers must pass power-of-two alignment; non-power-of-two is UB per
    // contract (same as raw allocator contracts in game engines).
    if (align < sizeof(void*)) {
        align = sizeof(void*);
    }

    // Zero-size allocations: do not advance the counter, but still return
    // a valid uniquely-addressed pointer.
    const std::size_t actual_bytes = (bytes == 0) ? 1 : bytes;

#if defined(GLIBRE_ALLOC_STRICT) && GLIBRE_ALLOC_STRICT
    // Strict mode: ceiling check with atomic CAS loop.
    //
    // Load current usage, check that adding `bytes` does not exceed the
    // ceiling, then attempt to update atomically.  On contention the loop
    // retries with the fresh value.
    std::uint64_t current = bytes_used_.load(std::memory_order_relaxed);
    while (true) {
        const std::uint64_t after = current + static_cast<std::uint64_t>(bytes);
        if (bytes > 0 && after > ceiling_) {
            return std::unexpected{Error{core::Error::OutOfBudget}};
        }
        if (bytes_used_.compare_exchange_weak(
                current, after, std::memory_order_acq_rel, std::memory_order_relaxed
            )) {
            break;
        }
        // CAS failed: `current` has been refreshed with the actual value; retry.
    }
#else
    // Shipping mode: update counter after allocation; emit warn-once on overrun.
    if (bytes > 0) {
        const std::uint64_t after =
            bytes_used_.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed) +
            static_cast<std::uint64_t>(bytes);
        if (after > ceiling_) {
            // Per-instance warn-once flag (alloc.hpp §Shipping-build soft warning).
            bool already_warned = warn_once_flag_.load(std::memory_order_relaxed);
            if (!already_warned &&
                warn_once_flag_.compare_exchange_strong(
                    already_warned, true, std::memory_order_relaxed, std::memory_order_relaxed
                )) {
                // Rule #3 (perf-budget.md §Allocator Rules): emit spdlog::warn
                // once per ContextTag on ceiling overrun in shipping builds.
                // MVP throttle is once-per-tag-per-instance-lifetime; per-frame
                // reset is deferred to plan #241.
                spdlog::warn(
                    "PerContextAllocator: context tag {} exceeded ceiling "
                    "(requested {} bytes, ceiling {} bytes, live {} bytes) — "
                    "perf-budget.md Rule #3 [further overruns suppressed for this instance]",
                    static_cast<std::uint8_t>(tag_),
                    bytes,
                    ceiling_,
                    after
                );
            }
        }
    }
#endif

    void* ptr = nullptr;
    const int rc = ::posix_memalign(&ptr, align, actual_bytes);
    if (rc != 0) {
        // System OOM: the engine aborts.  We do not attempt to recover from
        // system-level out-of-memory.  The byte counter may be slightly
        // inconsistent after abort() but that is irrelevant for a crashing
        // process.
        std::abort();
    }

    return ptr;
}

// ---------------------------------------------------------------------------
// deallocate
// ---------------------------------------------------------------------------

void PerContextAllocator::deallocate(void* p, std::size_t bytes) noexcept {
    if (p == nullptr) {
        return;
    }
    ::free(p);
    if (bytes > 0) {
        bytes_used_.fetch_sub(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// bytes_used
// ---------------------------------------------------------------------------

std::uint64_t PerContextAllocator::bytes_used() const noexcept {
    return bytes_used_.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// register_allocator — MVP stub (plan #241 wires the real registry)
// ---------------------------------------------------------------------------

void register_allocator([[maybe_unused]] PerContextAllocator& alloc) noexcept {
    // Intentional no-op in MVP.  Plan #241 (perf-budget framework) will
    // populate a global registry that the CI gate and perf HUD enumerate.
    // The stable call-site ABI means existing callers need no change when
    // plan #241 lands.
    //
    // Test observability (MED-7) is deferred: verifying this call fires
    // requires a registry or hook whose design lives in plan #241.
    // See alloc.hpp §register_allocator observability.
}

}  // namespace glibre
