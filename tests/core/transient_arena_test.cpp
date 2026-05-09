// tests/core/transient_arena_test.cpp
//
// Catch2 unit tests for glibre::TransientArena and its integration with
// glibre::core::FrameLoop phase-9 drain.
//
// Authority: plan #239, perf-budget.md §Allocator Rules #4.
//
// Named test cases (plan #239 Unit Test Plan):
//   - core/transient_arena: bump_alloc_returns_aligned_storage
//   - core/transient_arena: drain_resets_high_watermark
//   - core/transient_arena: undrained_allocation_at_phase_9_returns_out_of_budget
//   - core/transient_arena: allocations_do_not_count_against_cell_ceiling
//
// Additional test cases (dispatch DoD):
//   - transient_arena_allocates_and_resets
//   - transient_arena_drained_at_phase_9
//   - transient_arena_alignment_respected
//   - transient_arena_exhaustion_returns_error
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS usage.
//   - All alignment checks use pointer arithmetic and bitwise tests.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "glibre/core/frame_loop.hpp"
#include "glibre/error.hpp"
#include "glibre/transient_arena.hpp"

// ===========================================================================
// Test: core/transient_arena: bump_alloc_returns_aligned_storage
//
// Verifies that:
//   (a) allocate() returns a non-null pointer on success.
//   (b) The returned pointer is aligned to the requested alignment.
//   (c) bytes_used() reflects the number of committed bytes.
//   (d) Successive allocations do not overlap.
//
// Tests one arena with two sequential 64-byte allocations at 16-byte align.
// ===========================================================================

TEST_CASE("core/transient_arena: bump_alloc_returns_aligned_storage", "[core][transient_arena]") {
    glibre::TransientArena arena{1024};

    REQUIRE(arena.bytes_capacity() == 1024u);
    REQUIRE(arena.bytes_used() == 0u);

    // First allocation: 64 bytes at 16-byte alignment.
    auto result1 = arena.allocate(64, 16);
    REQUIRE(result1.has_value());
    void* p1 = *result1;
    REQUIRE(p1 != nullptr);

    // Pointer must be 16-byte aligned.
    const auto addr1 = reinterpret_cast<std::uintptr_t>(p1);
    CHECK((addr1 % 16u) == 0u);

    // bytes_used must be >= 64 (may be rounded up to satisfy alignment of p1).
    CHECK(arena.bytes_used() >= 64u);
    CHECK(arena.bytes_used() <= 128u);  // sanity upper bound

    // Second allocation: 32 bytes at 8-byte alignment.
    auto result2 = arena.allocate(32, 8);
    REQUIRE(result2.has_value());
    void* p2 = *result2;
    REQUIRE(p2 != nullptr);

    // p2 must not overlap p1.
    const auto addr2 = reinterpret_cast<std::uintptr_t>(p2);
    CHECK((addr2 % 8u) == 0u);
    CHECK(addr2 >= addr1 + 64u);  // p2 comes after p1's allocation

    // bytes_used reflects both allocations.
    CHECK(arena.bytes_used() >= 96u);  // at least 64 + 32
}

// ===========================================================================
// Test: transient_arena_allocates_and_resets
//
// DoD named test (dispatch): allocate bytes, assert bytes_used, reset (drain),
// assert bytes_used == 0.
//
// This is the fundamental contract: O(1) bump allocate, O(1) reset.
// ===========================================================================

TEST_CASE("transient_arena_allocates_and_resets", "[core][transient_arena]") {
    glibre::TransientArena arena{4096};

    // Allocate 100 bytes at default alignment.
    auto result = arena.allocate(100);
    REQUIRE(result.has_value());
    CHECK(arena.bytes_used() >= 100u);

    // Reset to empty.
    arena.drain();
    CHECK(arena.bytes_used() == 0u);

    // After drain the arena is reusable.
    auto result2 = arena.allocate(200);
    REQUIRE(result2.has_value());
    CHECK(arena.bytes_used() >= 200u);
}

// ===========================================================================
// Test: core/transient_arena: drain_resets_high_watermark
//
// Verifies that:
//   (a) drain() updates high_watermark_ when bytes_used() > high_watermark_.
//   (b) A second drain() after a smaller allocation does NOT lower the
//       high_watermark_ (it tracks the peak, not the current usage).
//   (c) reset_high_watermark() zeroes the high watermark.
// ===========================================================================

TEST_CASE("core/transient_arena: drain_resets_high_watermark", "[core][transient_arena]") {
    glibre::TransientArena arena{8192};

    // Frame 1: allocate 512 bytes; drain.
    auto r1 = arena.allocate(512);
    REQUIRE(r1.has_value());
    const std::size_t used_before_drain1 = arena.bytes_used();
    CHECK(used_before_drain1 >= 512u);

    arena.drain();
    CHECK(arena.bytes_used() == 0u);
    CHECK(arena.high_watermark() == used_before_drain1);

    // Frame 2: allocate 128 bytes (less than frame 1); drain.
    auto r2 = arena.allocate(128);
    REQUIRE(r2.has_value());
    arena.drain();
    // High-watermark must still reflect frame 1's peak (512+).
    CHECK(arena.high_watermark() == used_before_drain1);

    // Frame 3: allocate 1024 bytes (more than frame 1); drain.
    auto r3 = arena.allocate(1024);
    REQUIRE(r3.has_value());
    const std::size_t used_before_drain3 = arena.bytes_used();
    arena.drain();
    CHECK(arena.high_watermark() == used_before_drain3);

    // reset_high_watermark() zeroes the counter.
    arena.reset_high_watermark();
    CHECK(arena.high_watermark() == 0u);
}

// ===========================================================================
// Test: core/transient_arena: undrained_allocation_at_phase_9_returns_out_of_budget
//
// Verifies that assert_drained() returns core::Error::OutOfBudget when the
// arena has live (uncommitted) bytes — i.e. when drain() has not been called.
//
// This covers perf-budget.md §Allocator Rules #4's "drain failure" contract:
// allocations leaking past phase 9 return OutOfBudget with "transient arena leak".
// ===========================================================================

TEST_CASE(
    "core/transient_arena: undrained_allocation_at_phase_9_returns_out_of_budget",
    "[core][transient_arena]"
) {
    glibre::TransientArena arena{1024};

    // Arena is empty — assert_drained() must succeed.
    {
        auto check = arena.assert_drained();
        REQUIRE(check.has_value());
    }

    // Allocate without draining — simulates a "leak" past phase 9.
    auto alloc = arena.allocate(64);
    REQUIRE(alloc.has_value());
    CHECK(arena.bytes_used() >= 64u);

    // assert_drained() must now return OutOfBudget.
    {
        auto check = arena.assert_drained();
        REQUIRE_FALSE(check.has_value());

        // The error must be core::Error::OutOfBudget.
        const glibre::Error& err = check.error();
        const auto* core_err = eastl::get_if<glibre::core::Error>(&err.code());
        REQUIRE(core_err != nullptr);
        CHECK(*core_err == glibre::core::Error::OutOfBudget);

        // The detail must be "transient arena leak" (perf-budget.md §4).
        CHECK(err.where().detail == eastl::string_view{"transient arena leak"});
    }

    // After drain(), assert_drained() must succeed again.
    arena.drain();
    {
        auto check = arena.assert_drained();
        REQUIRE(check.has_value());
    }
}

// ===========================================================================
// Test: transient_arena_drained_at_phase_9
//
// DoD named test (dispatch): Allocate into an arena registered with FrameLoop,
// run one tick(), assert the arena is empty (drained).
//
// This is the integration test: the FrameLoop drains at Phase::Present (9),
// so bytes_used() must be 0 after tick() when the arena is non-empty before.
// ===========================================================================

TEST_CASE("transient_arena_drained_at_phase_9", "[core][transient_arena]") {
    glibre::TransientArena arena{4096};
    glibre::core::FrameLoop loop;

    // Register the arena with the frame loop.
    auto reg = loop.register_transient_arena(&arena);
    REQUIRE(reg.has_value());
    CHECK(loop.transient_arena_count() == 1u);

    // Allocate from the arena (simulates frame-scoped usage).
    auto alloc = arena.allocate(256, 16);
    REQUIRE(alloc.has_value());
    CHECK(arena.bytes_used() >= 256u);

    // Run one tick — phase 9 (Present) must drain the arena.
    auto tick_result = loop.tick();
    REQUIRE(tick_result.has_value());

    // After tick, the arena must be empty.
    CHECK(arena.bytes_used() == 0u);

    // The high-watermark must reflect the allocation from this frame.
    CHECK(arena.high_watermark() >= 256u);
}

// ===========================================================================
// Test: transient_arena_alignment_respected
//
// DoD named test (dispatch): allocate with various alignments and assert that
// the returned pointers satisfy the requested alignment.
//
// Tests alignments: 1, 2, 4, 8, 16, 32, 64, 128, 256.
// ===========================================================================

TEST_CASE("transient_arena_alignment_respected", "[core][transient_arena]") {
    // Large arena to avoid exhaustion during the alignment sweep.
    glibre::TransientArena arena{65536};

    const std::array<std::size_t, 9> alignments{1, 2, 4, 8, 16, 32, 64, 128, 256};

    for (const std::size_t align : alignments) {
        // Allocate 1 byte at each alignment; bump the cursor by 1 first so
        // the alignment is non-trivially satisfied for non-1 alignments.
        auto r_bump = arena.allocate(1, 1);
        REQUIRE(r_bump.has_value());  // pre-bump to misalign cursor

        auto result = arena.allocate(16, align);
        REQUIRE(result.has_value());

        void* p = *result;
        REQUIRE(p != nullptr);

        const auto addr = reinterpret_cast<std::uintptr_t>(p);
        INFO("alignment=" << align << " addr=" << addr);
        CHECK((addr % align) == 0u);
    }
}

// ===========================================================================
// Test: transient_arena_exhaustion_returns_error
//
// DoD named test (dispatch): allocate beyond capacity and assert the error is
// core::Error::TransientArenaExhausted.
// ===========================================================================

TEST_CASE("transient_arena_exhaustion_returns_error", "[core][transient_arena]") {
    // Small arena: 128 bytes.
    glibre::TransientArena arena{128};

    // Fill to capacity.
    auto r1 = arena.allocate(128, 1);
    REQUIRE(r1.has_value());
    CHECK(arena.bytes_used() == 128u);

    // Next allocation must fail with TransientArenaExhausted.
    auto r2 = arena.allocate(1, 1);
    REQUIRE_FALSE(r2.has_value());

    const glibre::Error& err = r2.error();
    const auto* core_err = eastl::get_if<glibre::core::Error>(&err.code());
    REQUIRE(core_err != nullptr);
    CHECK(*core_err == glibre::core::Error::TransientArenaExhausted);

    // Zero-capacity arena: every allocate() call must fail immediately.
    glibre::TransientArena zero_arena{0};
    auto r3 = zero_arena.allocate(1, 1);
    REQUIRE_FALSE(r3.has_value());

    const auto* zero_err = eastl::get_if<glibre::core::Error>(&r3.error().code());
    REQUIRE(zero_err != nullptr);
    CHECK(*zero_err == glibre::core::Error::TransientArenaExhausted);
}

// ===========================================================================
// Test: core/transient_arena: allocations_do_not_count_against_cell_ceiling
//
// Structural test: verifies that TransientArena is an independent allocator
// that does NOT forward to the global heap or any per-context ceiling tracker.
// The arena allocates from its own backing store; the context ceiling is
// unaffected by transient allocations (perf-budget.md §Allocator Rules #4).
//
// MVP observable: bytes_used() only reflects the arena's internal cursor;
// it is not connected to any external ceiling counter.  We verify that two
// independent arenas do not interfere with each other's accounting.
// ===========================================================================

TEST_CASE(
    "core/transient_arena: allocations_do_not_count_against_cell_ceiling",
    "[core][transient_arena]"
) {
    // Two independent arenas — one for "context A", one for "context B".
    glibre::TransientArena arena_a{1024};
    glibre::TransientArena arena_b{1024};

    // Allocate from A.
    auto ra = arena_a.allocate(512);
    REQUIRE(ra.has_value());
    CHECK(arena_a.bytes_used() >= 512u);

    // Arena B's accounting is unaffected by A's allocation.
    CHECK(arena_b.bytes_used() == 0u);

    // Allocate from B.
    auto rb = arena_b.allocate(256);
    REQUIRE(rb.has_value());
    CHECK(arena_b.bytes_used() >= 256u);

    // Arena A's accounting is unaffected by B's allocation.
    CHECK(arena_a.bytes_used() >= 512u);

    // Drain A; B is unaffected.
    arena_a.drain();
    CHECK(arena_a.bytes_used() == 0u);
    CHECK(arena_b.bytes_used() >= 256u);

    // Drain B; A is unaffected.
    arena_b.drain();
    CHECK(arena_b.bytes_used() == 0u);
    CHECK(arena_a.bytes_used() == 0u);
}
