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
// ## bytes == 0 allocations
//
//   bytes == 0 is forwarded as bytes = 1 to posix_memalign (allocating a
//   uniquely-addressed pointer) but the byte counter is NOT incremented.
//   This matches the C++ standard's "zero-size allocation returns a unique
//   non-null pointer" contract.

#include "glibre/per_context_allocator.hpp"

#include <cassert>
#include <cerrno>
#include <cstdlib>  // std::abort
#include <cstring>  // std::memset (debug zero; not used in release)

// posix_memalign is POSIX (macOS, Linux). Required by platform baseline.
#include <cstdlib>  // posix_memalign / free on POSIX

namespace glibre {

// ---------------------------------------------------------------------------
// Constructors
// ---------------------------------------------------------------------------

PerContextAllocator::PerContextAllocator(ContextTag tag, std::uint64_t ceiling_bytes) noexcept
    : tag_{tag},
      ceiling_{ceiling_bytes} {}

PerContextAllocator::PerContextAllocator(ContextTag tag) noexcept
    : PerContextAllocator{tag, kContextCeilings[static_cast<std::uint8_t>(tag)]} {}

// ---------------------------------------------------------------------------
// allocate
// ---------------------------------------------------------------------------

glibre::Result<void*>
PerContextAllocator::allocate(std::size_t bytes, std::size_t align) noexcept {
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
            return std::unexpected{glibre::Error{core::Error::OutOfBudget}};
        }
        if (bytes_used_.compare_exchange_weak(
                current, after,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            break;
        }
        // CAS failed: `current` has been refreshed with the actual value; retry.
    }
#endif

    void* ptr = nullptr;
    const int rc = ::posix_memalign(&ptr, align, actual_bytes);
    if (rc != 0) {
        // System OOM: the engine aborts (we do not recover from OOM).
        // Undo the byte counter increment in strict mode before aborting so
        // that any atexit / destructor-based logging sees consistent state.
#if defined(GLIBRE_ALLOC_STRICT) && GLIBRE_ALLOC_STRICT
        if (bytes > 0) {
            bytes_used_.fetch_sub(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);
        }
#endif
        std::abort();
    }

#if !defined(GLIBRE_ALLOC_STRICT) || !GLIBRE_ALLOC_STRICT
    // Shipping mode: update counter after allocation succeeds (no ceiling check).
    if (bytes > 0) {
        bytes_used_.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);
    }
#endif

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
}

}  // namespace glibre
