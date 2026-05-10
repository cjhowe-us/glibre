#pragma once
// core/include/glibre/transient_arena.hpp
//
// glibre::TransientArena — per-context bump-pointer transient allocator.
//
// Design (perf-budget.md §Allocator Rules #4):
//   Each bounded context owns one TransientArena.  Allocations are O(1)
//   bump-pointer (no header, no free-list, no destructor tracking).
//   Callers MUST only store trivially-destructible data in the arena.
//   The arena is drained (cursor reset to zero) by the frame loop at the
//   end of Phase::Present (phase 9).  Drain is always O(1).
//
//   High-watermark is tracked across drain cycles so the perf HUD can
//   display peak transient usage without polling mid-frame.
//
//   Allocation failure (capacity exhausted) returns
//   std::unexpected{core::Error::TransientArenaExhausted} rather than
//   aborting, so callers can fall back gracefully in release builds and
//   the CI diagnostic build catches budget violations via REQUIRE checks.
//
// API:
//   TransientArena arena{256 * 1024};       // 256 KiB backing store
//   Result<void*> p = arena.allocate(64, 16);
//   arena.drain();                           // called by FrameLoop at phase 9
//   size_t peak = arena.high_watermark();   // bytes used since last drain
//
// Thread safety:
//   None.  Each context owns one arena instance; callers are responsible
//   for ensuring single-threaded access within a frame.
//
// Placement:
//   The arena lives in core/ (SPEC §4, perf-budget.md §Allocator Rules)
//   because the frame loop is the drainer and both reside in core.  Per-
//   context *ownership* is enforced by callers (each plugin obtains a ref
//   to its arena from the future PerContextAllocator handle — plan #238);
//   in the MVP skeleton the arena is constructed and held directly.
//
// -fno-exceptions clean.  No heap allocation inside this class beyond
// the one backing-store allocation in the constructor.
//
// PerContextAllocator bypass — pre-existing behaviour (perf-budget.md §Allocator Rules #4):
//   TransientArena has always allocated its backing store via `::operator new[]`
//   (currently expressed as `std::make_unique_for_overwrite<std::byte[]>(N)`),
//   bypassing PerContextAllocator's per-tag heap accounting.  This is by design:
//   transient / per-frame arenas are explicitly exempted from the per-context
//   ceiling because they are frame-bounded (drained at Phase::Present).
//   The eastl-removal migration (refs #1041) preserves this exemption verbatim —
//   no new bypass was introduced by that migration.

#include <cstddef>
#include <memory>

#include "glibre/error.hpp"

namespace glibre {

// ---------------------------------------------------------------------------
// TransientArena — O(1) bump-pointer allocator, O(1) drain
// ---------------------------------------------------------------------------

class TransientArena {
public:
    // storage_pointer — canonical type of TransientArena's backing store.
    //
    // Exposed as a public type alias so migration-guard tests can assert the
    // full smart-pointer type without access to the private `storage_` member.
    // Any accidental reversion to eastl::unique_ptr<std::byte[]> will break
    // the static_assert in `core/transient_arena: storage_held_by_std_unique_ptr`.
    using storage_pointer = std::unique_ptr<std::byte[]>;

    // Construct an arena with `capacity_bytes` of backing storage.
    //
    // The backing store is allocated once on construction via
    // `std::make_unique_for_overwrite<std::byte[]>` (C++20).  No further
    // allocation occurs for the lifetime of this object.  capacity_bytes == 0
    // is valid: every allocate() call will return TransientArenaExhausted.
    explicit TransientArena(std::size_t capacity_bytes);

    // Non-copyable, non-movable.  Arenas are long-lived, context-scoped
    // objects; moving them would dangle pointers held by callers.
    TransientArena(const TransientArena&) = delete;
    TransientArena& operator=(const TransientArena&) = delete;
    TransientArena(TransientArena&&) = delete;
    TransientArena& operator=(TransientArena&&) = delete;

    ~TransientArena() noexcept = default;

    // allocate(bytes, align) — bump-allocate `bytes` aligned to `align`.
    //
    // `align` must be a power of two in [1, 4096].  Violation is a
    // debug-build assert; release builds produce undefined behaviour
    // (consistent with raw allocator contracts in game engines).
    //
    // Returns:
    //   Result<void*>  — pointer to the aligned region on success, or
    //   std::unexpected{core::Error::TransientArenaExhausted} when the
    //   arena has insufficient capacity for the requested allocation.
    //
    // bytes == 0 is defined: returns a valid, uniquely-aligned pointer
    // with no bytes committed (the cursor does not advance past the
    // alignment pad if any).
    [[nodiscard]] glibre::Result<void*>
    allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t)) noexcept;

    // drain() — reset the arena to empty.
    //
    // Called by FrameLoop at the end of Phase::Present (phase 9).
    // Advances high_watermark_ if bytes_used() > high_watermark_.
    // Sets cursor_ to 0.  O(1); does NOT zero the backing store.
    void drain() noexcept;

    // bytes_used() — bytes committed since the last drain().
    [[nodiscard]] std::size_t bytes_used() const noexcept { return cursor_; }

    // bytes_capacity() — total backing-store bytes available.
    [[nodiscard]] std::size_t bytes_capacity() const noexcept { return capacity_; }

    // high_watermark() — peak bytes_used() observed across all drain cycles
    // since construction (or since the last explicit reset_high_watermark()).
    [[nodiscard]] std::size_t high_watermark() const noexcept { return high_watermark_; }

    // reset_high_watermark() — zero the high-watermark counter.
    // Intended for perf-HUD tools that want per-second peak windows rather
    // than all-time peaks.  Not called by the frame loop.
    void reset_high_watermark() noexcept { high_watermark_ = 0; }

    // assert_drained() — CI / diagnostic check that the arena has been drained.
    //
    // Returns success if bytes_used() == 0 (drain was called).
    // Returns core::Error::OutOfBudget with detail "transient arena leak" if
    // bytes_used() > 0 (allocations are still live past the expected drain point).
    //
    // This method implements perf-budget.md §Allocator Rules #4's "drain failure"
    // contract: allocations that survive past phase 9 are reported as OutOfBudget.
    // Called by CI diagnostic builds; not called in shipping builds.
    [[nodiscard]] glibre::Result<void> assert_drained() const noexcept;

private:
    storage_pointer storage_;
    std::size_t capacity_{0};
    std::size_t cursor_{0};
    std::size_t high_watermark_{0};

    // Paired static_assert: storage_ must be exactly storage_pointer.
    // The public alias and this field declaration must stay in sync; if either
    // is changed without the other, this assertion fires at compile time.
    // Placed after storage_ so decltype(storage_) resolves within class scope.
    // Companion to the test-side assert in core/transient_arena_test.cpp
    // (test "core/transient_arena: storage_held_by_std_unique_ptr", refs #1051).
    static_assert(
        std::is_same_v<decltype(storage_), storage_pointer>,
        "TransientArena::storage_ must be exactly storage_pointer "
        "(std::unique_ptr<std::byte[]>) — eastl reversion guard (refs #1051)"
    );
};

}  // namespace glibre
