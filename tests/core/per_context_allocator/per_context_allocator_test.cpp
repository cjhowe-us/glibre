// tests/core/per_context_allocator/per_context_allocator_test.cpp
//
// Catch2 unit tests for glibre::PerContextAllocator.
// Authority: core/include/glibre/alloc.hpp, plan #238, plan #990,
//            reviews/decisions/perf-budget.md §Allocator Rules #1-3.
//
// Named test cases (plan #238 Unit Test Plan):
//   - per_context_allocator_tracks_bytes_per_tag
//   - per_context_allocator_rejects_alloc_over_ceiling
//   - per_context_allocator_release_decrements_counter
//   - per_context_allocator_alignment_respected
//   - per_context_allocator_threadsafe_allocations
//
// Named test cases (plan #990 Unit Test Plan):
//   - per_context_allocator_register_fires_on_construction
//
// The plan #990 test uses a GLIBRE_TESTING-gated call counter in
// register_allocator() (option 1 from the plan — chosen because
// AllocatorRegistry from plan #241 does not yet expose an enumeration API).
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS.
//   - GLIBRE_ALLOC_STRICT=1 is set by the CMakeLists so strict-mode ceiling
//     enforcement is active in all test cases.
//   - Thread-safety test uses std::thread; std::vector<T> for test fixtures
//     per reviews/decisions/eastl-removal.md §Consequences/Test-fixtures.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <glibre/alloc.hpp>

// ===========================================================================
// Test: per_context_allocator_tracks_bytes_per_tag
//
// Allocating bytes advances bytes_used() by the corresponding amount.
// Two sequential allocations sum correctly.
// ===========================================================================

TEST_CASE("per_context_allocator_tracks_bytes_per_tag", "[core][alloc]") {
    // Use a large ceiling (1 MiB) so this test never trips it.
    constexpr std::uint64_t kCeiling = 1024ULL * 1024ULL;
    glibre::PerContextAllocator alloc{glibre::ContextTag::core, kCeiling};

    REQUIRE(alloc.bytes_used() == 0);

    // Allocate 100 bytes.
    auto r1 = alloc.allocate(100);
    REQUIRE(r1.has_value());
    CHECK(alloc.bytes_used() == 100);

    // Allocate another 50 bytes — total must be 150.
    auto r2 = alloc.allocate(50);
    REQUIRE(r2.has_value());
    CHECK(alloc.bytes_used() == 150);

    // Cleanup (not checked for counter correctness here; covered by
    // per_context_allocator_release_decrements_counter below).
    alloc.deallocate(*r1, 100);
    alloc.deallocate(*r2, 50);
}

// ===========================================================================
// Test: per_context_allocator_rejects_alloc_over_ceiling
//
// In GLIBRE_ALLOC_STRICT builds, allocating past the ceiling returns
// core::Error::OutOfBudget.  The byte counter must NOT advance on refusal.
// ===========================================================================

TEST_CASE("per_context_allocator_rejects_alloc_over_ceiling", "[core][alloc]") {
    // Small ceiling: 100 bytes.
    constexpr std::uint64_t kCeiling = 100;
    glibre::PerContextAllocator alloc{glibre::ContextTag::physics, kCeiling};

    // First allocation: 60 bytes — fits within ceiling.
    auto r1 = alloc.allocate(60);
    REQUIRE(r1.has_value());
    CHECK(alloc.bytes_used() == 60);

    // Second allocation: 50 bytes — would push total to 110, exceeding 100.
    // Expect OutOfBudget.
    auto r2 = alloc.allocate(50);
    REQUIRE_FALSE(r2.has_value());

    // Verify the error code is core::Error::OutOfBudget.
    const glibre::Error& err = r2.error();
    const auto* core_err_ptr = std::get_if<glibre::core::Error>(&err.code());
    const bool is_out_of_budget =
        core_err_ptr != nullptr && *core_err_ptr == glibre::core::Error::OutOfBudget;
    CHECK(is_out_of_budget);

    // Counter must NOT have advanced on the rejected allocation.
    CHECK(alloc.bytes_used() == 60);

    // An allocation that exactly fits the remaining 40 bytes must succeed.
    auto r3 = alloc.allocate(40);
    REQUIRE(r3.has_value());
    CHECK(alloc.bytes_used() == 100);

    alloc.deallocate(*r1, 60);
    alloc.deallocate(*r3, 40);
}

// ===========================================================================
// Test: per_context_allocator_release_decrements_counter
//
// deallocate(p, bytes) reduces bytes_used() by the correct amount.
// After all allocations are released, bytes_used() returns to zero.
// ===========================================================================

TEST_CASE("per_context_allocator_release_decrements_counter", "[core][alloc]") {
    constexpr std::uint64_t kCeiling = 1024ULL * 1024ULL;
    glibre::PerContextAllocator alloc{glibre::ContextTag::render, kCeiling};

    auto r = alloc.allocate(100);
    REQUIRE(r.has_value());
    CHECK(alloc.bytes_used() == 100);

    alloc.deallocate(*r, 100);
    CHECK(alloc.bytes_used() == 0);
}

// ===========================================================================
// Test: per_context_allocator_alignment_respected
//
// allocate() with 16, 32, and 64-byte alignment returns pointers whose
// addresses are divisible by the requested alignment.
//
// Also verifies the two normalisation branches in allocate():
//   - align == 0 normalises to alignof(std::max_align_t).
//   - align < sizeof(void*) (e.g. align == 1) normalises to sizeof(void*).
// ===========================================================================

TEST_CASE("per_context_allocator_alignment_respected", "[core][alloc]") {
    constexpr std::uint64_t kCeiling = 1024ULL * 1024ULL;
    glibre::PerContextAllocator alloc{glibre::ContextTag::geometry, kCeiling};

    // 16-byte alignment
    {
        auto r = alloc.allocate(64, 16);
        REQUIRE(r.has_value());
        const auto addr = reinterpret_cast<std::uintptr_t>(*r);
        CHECK((addr % 16) == 0);
        alloc.deallocate(*r, 64);
    }

    // 32-byte alignment
    {
        auto r = alloc.allocate(64, 32);
        REQUIRE(r.has_value());
        const auto addr = reinterpret_cast<std::uintptr_t>(*r);
        CHECK((addr % 32) == 0);
        alloc.deallocate(*r, 64);
    }

    // 64-byte alignment (cache-line size)
    {
        auto r = alloc.allocate(128, 64);
        REQUIRE(r.has_value());
        const auto addr = reinterpret_cast<std::uintptr_t>(*r);
        CHECK((addr % 64) == 0);
        alloc.deallocate(*r, 128);
    }

    // align == 0 normalisation: must produce a validly aligned pointer.
    // The contract normalises 0 → alignof(std::max_align_t), so the result
    // must be aligned to at least alignof(std::max_align_t).
    {
        auto r = alloc.allocate(64, 0);
        REQUIRE(r.has_value());
        const auto addr = reinterpret_cast<std::uintptr_t>(*r);
        CHECK((addr % alignof(std::max_align_t)) == 0);
        alloc.deallocate(*r, 64);
    }

    // align < sizeof(void*) normalisation: align == 1 is below the
    // posix_memalign minimum.  Contract normalises it to sizeof(void*).
    // The returned pointer must be at least sizeof(void*)-aligned.
    {
        auto r = alloc.allocate(64, 1);
        REQUIRE(r.has_value());
        const auto addr = reinterpret_cast<std::uintptr_t>(*r);
        CHECK((addr % sizeof(void*)) == 0);
        alloc.deallocate(*r, 64);
    }

    // Counter must be back to zero after all deallocations.
    CHECK(alloc.bytes_used() == 0);
}

// ===========================================================================
// Test: per_context_allocator_threadsafe_allocations
//
// N threads each allocate exactly `kBytesPerThread` bytes from the same
// PerContextAllocator.  After all threads complete, bytes_used() must equal
// N * kBytesPerThread (each allocation is counted exactly once).
//
// In GLIBRE_ALLOC_STRICT mode the ceiling is set above the total expected
// usage so that no thread hits OutOfBudget.
// ===========================================================================

TEST_CASE("per_context_allocator_threadsafe_allocations", "[core][alloc]") {
    constexpr int kThreadCount = 8;
    constexpr std::size_t kBytesPerThread = 256;
    constexpr std::uint64_t kCeiling =
        static_cast<std::uint64_t>(kThreadCount) * kBytesPerThread * 2;  // generous headroom

    glibre::PerContextAllocator alloc{glibre::ContextTag::content, kCeiling};

    // Each thread allocates kBytesPerThread and stores its pointer here.
    // Protected by the thread join barrier (no mutex needed: each element is
    // written by exactly one thread and read only after join).
    // std::vector per reviews/decisions/eastl-removal.md §Consequences/Test-fixtures.
    std::vector<void*> ptrs(kThreadCount, nullptr);

    {
        std::vector<std::thread> threads;
        threads.reserve(kThreadCount);
        for (int i = 0; i < kThreadCount; ++i) {
            threads.emplace_back([&alloc, &ptrs, i]() {
                auto r = alloc.allocate(kBytesPerThread);
                // Each thread's allocation must succeed (ceiling is generous).
                if (r.has_value()) {
                    ptrs[i] = *r;
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }
    }

    // All allocations must have succeeded.
    bool all_succeeded = true;
    for (int i = 0; i < kThreadCount; ++i) {
        if (ptrs[i] == nullptr) {
            all_succeeded = false;
        }
    }
    CHECK(all_succeeded);

    // Total bytes_used must equal N * kBytesPerThread.
    CHECK(alloc.bytes_used() == static_cast<std::uint64_t>(kThreadCount) * kBytesPerThread);

    // Cleanup.
    for (int i = 0; i < kThreadCount; ++i) {
        if (ptrs[i] != nullptr) {
            alloc.deallocate(ptrs[i], kBytesPerThread);
        }
    }
    CHECK(alloc.bytes_used() == 0);
}

// ===========================================================================
// Test: per_context_allocator_register_fires_on_construction
//
// Plan #990: verifies that every PerContextAllocator constructor calls
// register_allocator(*this) exactly once.
//
// Approach (option 1 from plan #990):  A GLIBRE_TESTING-gated atomic counter
// is incremented inside register_allocator() for every call.  The test resets
// the counter, constructs N instances, and asserts the count equals N.
//
// Both constructors are exercised:
//   (a) PerContextAllocator(ContextTag, uint64_t) — explicit ceiling
//   (b) PerContextAllocator(ContextTag)           — ceiling from kContextCeilings
//
// The two-tag variant is also included: three instances across two distinct
// ContextTags must each increment the counter independently (the counter is
// TU-global and tag-agnostic; the test is not asserting per-tag counts).
//
// GLIBRE_TESTING=1 is set by CMakeLists for the strict-mode test target that
// includes this file.  The counter API is not available when GLIBRE_TESTING is
// not defined so this section is compiled conditionally.
// ===========================================================================

#ifdef GLIBRE_TESTING

TEST_CASE("per_context_allocator_register_fires_on_construction", "[core][alloc]") {
    // Counter is TU-global; reset at the top of each SECTION so that prior
    // constructions (from other sections or test cases) do not bleed in.

    SECTION("explicit-ceiling ctor fires exactly once") {
        glibre::testing_reset_register_allocator_call_count();
        {
            glibre::PerContextAllocator a{glibre::ContextTag::core, 1024ULL};
        }
        CHECK(glibre::testing_register_allocator_call_count() == 1u);
    }

    SECTION("tag-only ctor fires exactly once") {
        glibre::testing_reset_register_allocator_call_count();
        {
            glibre::PerContextAllocator b{glibre::ContextTag::physics};
        }
        CHECK(glibre::testing_register_allocator_call_count() == 1u);
    }

    SECTION("N instances produce exactly N calls") {
        // Construct kN instances across two distinct ContextTags; assert the
        // counter equals kN (one call per instance, tag-agnostic).
        constexpr int kN = 3;
        glibre::testing_reset_register_allocator_call_count();
        {
            glibre::PerContextAllocator c0{glibre::ContextTag::render, 512ULL * 1024ULL * 1024ULL};
            glibre::PerContextAllocator c1{
                glibre::ContextTag::geometry, 256ULL * 1024ULL * 1024ULL
            };
            glibre::PerContextAllocator c2{glibre::ContextTag::tools};
        }
        CHECK(glibre::testing_register_allocator_call_count() == static_cast<std::uint64_t>(kN));
    }
}

#endif  // GLIBRE_TESTING
