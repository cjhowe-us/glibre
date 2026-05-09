// tests/core/perf_budget/perf_budget_test.cpp
//
// Catch2 unit tests for glibre::PerfBudget.
//
// Authority: plan #241, perf-budget.md §Decision.
//
// Named test cases (plan #241 DoD):
//   - perf_budget_records_cpu_time_per_context
//   - perf_budget_records_heap_bytes_per_context
//   - perf_budget_reset_at_phase_9
//   - perf_budget_concurrent_records_thread_safe
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS usage.
//   - std::thread and std::atomic are permitted carve-outs (PHILOSOPHY §11).

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "glibre/core/frame_loop.hpp"
#include "glibre/perf_budget.hpp"

// ===========================================================================
// Test: perf_budget_records_cpu_time_per_context
//
// Verifies that:
//   (a) record_cpu(Core, 100) accumulates to sample(Core).cpu_ns == 100.
//   (b) The recording does not bleed into other contexts (Physics unchanged).
//   (c) A second record_cpu call accumulates (100 + 50 = 150).
//   (d) gpu_ns and heap_bytes remain zero after only cpu recording.
// ===========================================================================
TEST_CASE("perf_budget_records_cpu_time_per_context", "[core][perf_budget]") {
    glibre::PerfBudget budget;

    // Initial state: all counters zero.
    {
        auto s = budget.sample(glibre::ContextTag::Core);
        REQUIRE(s.cpu_ns == 0u);
        REQUIRE(s.gpu_ns == 0u);
        REQUIRE(s.heap_bytes == 0u);
    }

    // Record 100 ns for Core.
    budget.record_cpu(glibre::ContextTag::Core, 100u);
    {
        auto s = budget.sample(glibre::ContextTag::Core);
        CHECK(s.cpu_ns == 100u);
        CHECK(s.gpu_ns == 0u);
        CHECK(s.heap_bytes == 0u);
    }

    // Physics must be unaffected.
    {
        auto s = budget.sample(glibre::ContextTag::Physics);
        CHECK(s.cpu_ns == 0u);
    }

    // Accumulate a second slice.
    budget.record_cpu(glibre::ContextTag::Core, 50u);
    {
        auto s = budget.sample(glibre::ContextTag::Core);
        CHECK(s.cpu_ns == 150u);
    }
}

// ===========================================================================
// Test: perf_budget_records_heap_bytes_per_context
//
// Verifies that:
//   (a) record_heap_alloc(Data, 4096) produces sample(Data).heap_bytes == 4096.
//   (b) record_heap_free(Data, 1024) reduces heap_bytes to 3072.
//   (c) Saturating free: free(heap_bytes + 1) clamps to 0, not wrapping.
//   (d) Other contexts (Core) are unaffected.
// ===========================================================================
TEST_CASE("perf_budget_records_heap_bytes_per_context", "[core][perf_budget]") {
    glibre::PerfBudget budget;

    // Record an allocation for Data.
    budget.record_heap_alloc(glibre::ContextTag::Data, 4096u);
    {
        auto s = budget.sample(glibre::ContextTag::Data);
        REQUIRE(s.heap_bytes == 4096u);
        CHECK(s.cpu_ns == 0u);
        CHECK(s.gpu_ns == 0u);
    }

    // Core must be unaffected.
    {
        auto s = budget.sample(glibre::ContextTag::Core);
        CHECK(s.heap_bytes == 0u);
    }

    // Partial free.
    budget.record_heap_free(glibre::ContextTag::Data, 1024u);
    {
        auto s = budget.sample(glibre::ContextTag::Data);
        CHECK(s.heap_bytes == 3072u);
    }

    // Saturating free: request more than available — must clamp to zero.
    budget.record_heap_free(glibre::ContextTag::Data, 99999u);
    {
        auto s = budget.sample(glibre::ContextTag::Data);
        CHECK(s.heap_bytes == 0u);
    }

    // GPU stub: record_gpu accumulates without interaction with heap counter.
    budget.record_gpu(glibre::ContextTag::Render, 8000000u);  // 8 ms in ns
    {
        auto s = budget.sample(glibre::ContextTag::Render);
        CHECK(s.gpu_ns == 8000000u);
        CHECK(s.cpu_ns == 0u);
        CHECK(s.heap_bytes == 0u);
    }
}

// ===========================================================================
// Test: perf_budget_reset_at_phase_9
//
// Verifies that:
//   (a) After recording values, FrameLoop::tick() zeros all counters via
//       the Phase::Present (9) reset hook.
//   (b) The FrameLoop frame_index increments, confirming tick() succeeded.
//   (c) Counters remain zero immediately after reset (before new recording).
//
// Approach:
//   1. Construct a PerfBudget.
//   2. Register it with a FrameLoop via set_perf_budget().
//   3. Record non-zero values across multiple contexts.
//   4. Call tick() — phase 9 resets all counters.
//   5. Assert all counters are zero.
// ===========================================================================
TEST_CASE("perf_budget_reset_at_phase_9", "[core][perf_budget]") {
    glibre::PerfBudget budget;
    glibre::core::FrameLoop loop;
    loop.set_perf_budget(&budget);

    // Record values across multiple contexts.
    budget.record_cpu(glibre::ContextTag::Core,     400'000u);   // 0.4 ms
    budget.record_cpu(glibre::ContextTag::Physics,  2'000'000u); // 2 ms
    budget.record_cpu(glibre::ContextTag::Tools,    800'000u);   // 0.8 ms
    budget.record_heap_alloc(glibre::ContextTag::Render, 512u * 1024u * 1024u);
    budget.record_gpu(glibre::ContextTag::Render,   8'000'000u); // 8 ms

    // Verify values are non-zero before tick.
    REQUIRE(budget.sample(glibre::ContextTag::Core).cpu_ns     == 400'000u);
    REQUIRE(budget.sample(glibre::ContextTag::Physics).cpu_ns  == 2'000'000u);
    REQUIRE(budget.sample(glibre::ContextTag::Tools).cpu_ns    == 800'000u);
    REQUIRE(budget.sample(glibre::ContextTag::Render).heap_bytes > 0u);
    REQUIRE(budget.sample(glibre::ContextTag::Render).gpu_ns   == 8'000'000u);

    // Run one frame tick — phase 9 calls budget.reset().
    const auto result = loop.tick();
    REQUIRE(result.has_value());
    CHECK(loop.frame_index() == 1u);

    // All counters must be zero after reset.
    CHECK(budget.sample(glibre::ContextTag::Core).cpu_ns     == 0u);
    CHECK(budget.sample(glibre::ContextTag::Core).gpu_ns     == 0u);
    CHECK(budget.sample(glibre::ContextTag::Core).heap_bytes == 0u);

    CHECK(budget.sample(glibre::ContextTag::Physics).cpu_ns  == 0u);
    CHECK(budget.sample(glibre::ContextTag::Tools).cpu_ns    == 0u);
    CHECK(budget.sample(glibre::ContextTag::Render).gpu_ns   == 0u);
    CHECK(budget.sample(glibre::ContextTag::Render).heap_bytes == 0u);

    // All 10 contexts must be zeroed.
    for (std::size_t i = 0; i < glibre::kContextTagCount; ++i) {
        const auto tag = static_cast<glibre::ContextTag>(i);
        auto s = budget.sample(tag);
        INFO("Context tag " << i << " cpu_ns = " << s.cpu_ns);
        CHECK(s.cpu_ns     == 0u);
        CHECK(s.gpu_ns     == 0u);
        CHECK(s.heap_bytes == 0u);
    }
}

// ===========================================================================
// Test: perf_budget_concurrent_records_thread_safe
//
// Verifies that N threads simultaneously calling record_cpu on the same
// ContextTag produce the correct aggregate sum.
//
// Approach:
//   - Spawn N threads (e.g. 8); each calls record_cpu(Physics, delta) M times.
//   - Join all threads.
//   - Assert sample(Physics).cpu_ns == N * M * delta.
//
// The test does NOT race reset() with record_cpu — that ordering is
// guaranteed by the FrameLoop's sequential phase walk in production.
// The concurrent-recording path tested here is the common case: multiple
// contexts recording on their owning threads during phases 1-8.
// ===========================================================================
TEST_CASE("perf_budget_concurrent_records_thread_safe", "[core][perf_budget]") {
    glibre::PerfBudget budget;

    constexpr int kThreads = 8;
    constexpr int kRecordsPerThread = 1000;
    constexpr std::uint64_t kDeltaNs = 100u;

    // Use std::thread (PHILOSOPHY §11 carve-out — std::thread retained for
    // concurrency; EASTL does not own a thread primitive).
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&budget]() {
            for (int i = 0; i < kRecordsPerThread; ++i) {
                budget.record_cpu(glibre::ContextTag::Physics, kDeltaNs);
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    const std::uint64_t expected =
        static_cast<std::uint64_t>(kThreads) *
        static_cast<std::uint64_t>(kRecordsPerThread) *
        kDeltaNs;

    auto s = budget.sample(glibre::ContextTag::Physics);
    REQUIRE(s.cpu_ns == expected);

    // Other contexts must be unaffected.
    CHECK(budget.sample(glibre::ContextTag::Core).cpu_ns == 0u);
    CHECK(budget.sample(glibre::ContextTag::Render).cpu_ns == 0u);
}
