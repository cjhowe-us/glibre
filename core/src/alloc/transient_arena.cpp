// core/src/alloc/transient_arena.cpp
//
// Implementation of glibre::TransientArena.
//
// Design rationale lives in core/include/glibre/transient_arena.hpp.
// perf-budget.md §Allocator Rules #4 is the authoritative spec.

#include "glibre/transient_arena.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace glibre {

// ---------------------------------------------------------------------------
// Constructor
//
// OOM behaviour: `std::make_unique_for_overwrite<std::byte[]>` calls
// `::operator new[]`, which throws `std::bad_alloc` on allocation failure
// (per [expr.new]/17).  Under `-fno-exceptions` (Glibre's compile flag),
// clang's ABI converts any unhandled `throw` expression to a call to
// `std::terminate()` (per [except.terminate]/1; cppreference: "if exceptions
// are disabled, std::terminate is called directly").  The process therefore
// terminates — not `abort()` directly, but `std::terminate()` which by
// default calls `std::abort()` unless the terminate handler has been replaced.
// This matches the prior `eastl::make_unique<std::byte[]>` abort-on-OOM
// posture; both paths terminate the process on backing-store OOM.
//
// Fallible / recoverable arena construction (returning Result<TransientArena>)
// is tracked as spike #1064 (deferred; post-MVP).
// ---------------------------------------------------------------------------

TransientArena::TransientArena(std::size_t capacity_bytes)
    : storage_{
          capacity_bytes > 0 ? std::make_unique_for_overwrite<std::byte[]>(capacity_bytes) : nullptr
      },
      capacity_{capacity_bytes},
      cursor_{0},
      high_watermark_{0} {}

// ---------------------------------------------------------------------------
// allocate — O(1) bump pointer
//
// Algorithm:
//   1. Round the current cursor up to the requested alignment.
//   2. Check that cursor_after_align + bytes <= capacity_.
//   3. Return a pointer to storage_[cursor_after_align].
//   4. Advance cursor_ to cursor_after_align + bytes.
//
// Alignment requirement: align must be a power of two.
// ---------------------------------------------------------------------------

[[nodiscard]] glibre::Result<void*>
TransientArena::allocate(std::size_t bytes, std::size_t align) noexcept {
    // align must be a power of two in [1, 4096].
    assert(
        align >= 1u && align <= 4096u && (align & (align - 1u)) == 0u &&
        "TransientArena::allocate: align must be a power-of-two in [1, 4096]"
    );

    // Align the cursor up to the requested alignment using standard bit trick.
    // cursor_aligned = (cursor_ + align - 1) & ~(align - 1).
    // This is well-defined for power-of-two align and fits in size_t.
    const std::size_t align_mask = align - 1u;
    const std::size_t cursor_aligned = (cursor_ + align_mask) & ~align_mask;

    // Check for capacity.  All three quantities are size_t; overflow of
    // cursor_aligned + bytes is guarded by the capacity_ check.
    if (cursor_aligned > capacity_ || bytes > capacity_ - cursor_aligned) {
        return std::unexpected(glibre::Error{core::Error::TransientArenaExhausted});
    }

    // The storage_ may be null when capacity_ == 0; in that case we already
    // returned an error above, so this access is safe.
    void* const ptr = storage_.get() + cursor_aligned;
    cursor_ = cursor_aligned + bytes;
    return ptr;
}

// ---------------------------------------------------------------------------
// drain — O(1) cursor reset; updates high_watermark_
// ---------------------------------------------------------------------------

void TransientArena::drain() noexcept {
    if (cursor_ > high_watermark_) {
        high_watermark_ = cursor_;
    }
    cursor_ = 0;
}

// ---------------------------------------------------------------------------
// assert_drained — CI / diagnostic check for undrained allocations
//
// perf-budget.md §Allocator Rules #4: drain failure (allocations still
// live past phase 9) is core::Error::OutOfBudget with "transient arena leak"
// as the detail.
// ---------------------------------------------------------------------------

[[nodiscard]] glibre::Result<void> TransientArena::assert_drained() const noexcept {
    if (cursor_ != 0) {
        return std::unexpected(
            glibre::Error{
                glibre::core::Error::OutOfBudget,
                glibre::ErrorContext{
                    .file = "core/src/alloc/transient_arena.cpp",
                    .line = __LINE__,
                    .detail = "transient arena leak",
                },
            }
        );
    }
    return {};
}

}  // namespace glibre
