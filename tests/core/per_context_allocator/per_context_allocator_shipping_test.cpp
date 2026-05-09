// tests/core/per_context_allocator/per_context_allocator_shipping_test.cpp
//
// Shipping-mode (GLIBRE_ALLOC_STRICT=0) unit tests for glibre::PerContextAllocator.
// Authority: core/include/glibre/alloc.hpp, plan #238,
//            reviews/decisions/perf-budget.md §Allocator Rules #3.
//
// Round-2 review MED-5: the strict-mode unit tests hard-code GLIBRE_ALLOC_STRICT=1
// so Rule #3 (soft-warn on overrun, always-return-valid-pointer in shipping builds)
// was unverified.  This sibling test target compiles with GLIBRE_ALLOC_STRICT=0
// and exercises the shipping-mode code path.
//
// Named test cases:
//   - per_context_allocator_shipping_overrun_returns_valid_ptr
//   - per_context_allocator_shipping_bytes_used_above_ceiling
//   - per_context_allocator_shipping_warn_once_per_instance
//
// Design constraints:
//   - GLIBRE_ALLOC_STRICT=0 is set by the CMakeLists (not defined here).
//   - -fno-exceptions clean: no REQUIRE_THROWS.
//   - spdlog sink interception uses spdlog::set_level(trace) + custom sink so
//     the warn-once emission is observable without requiring a separate process.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <EASTL/vector.h>
#include <catch2/catch_test_macros.hpp>
#include <glibre/alloc.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

// ===========================================================================
// Test: per_context_allocator_shipping_overrun_returns_valid_ptr
//
// In shipping builds (GLIBRE_ALLOC_STRICT=0), allocating past the ceiling
// returns a valid pointer (not OutOfBudget).  Rule #3 guarantees release-build
// robustness.
// ===========================================================================

TEST_CASE("per_context_allocator_shipping_overrun_returns_valid_ptr", "[core][alloc][shipping]") {
    // Tiny ceiling: 50 bytes.
    constexpr std::uint64_t kCeiling = 50;
    glibre::PerContextAllocator alloc{glibre::ContextTag::data, kCeiling};

    // First allocation: 30 bytes — within ceiling.
    auto r1 = alloc.allocate(30);
    REQUIRE(r1.has_value());

    // Second allocation: 30 bytes — would push total to 60, exceeding 50-byte ceiling.
    // In shipping mode this must succeed (return a valid pointer, not OutOfBudget).
    auto r2 = alloc.allocate(30);
    REQUIRE(r2.has_value());
    CHECK(*r2 != nullptr);

    alloc.deallocate(*r1, 30);
    alloc.deallocate(*r2, 30);
}

// ===========================================================================
// Test: per_context_allocator_shipping_bytes_used_above_ceiling
//
// In shipping mode, bytes_used() reflects actual live bytes even when above
// the ceiling, so the perf HUD can surface the overrun.
// ===========================================================================

TEST_CASE("per_context_allocator_shipping_bytes_used_above_ceiling", "[core][alloc][shipping]") {
    constexpr std::uint64_t kCeiling = 50;
    glibre::PerContextAllocator alloc{glibre::ContextTag::shader, kCeiling};

    auto r1 = alloc.allocate(30);
    REQUIRE(r1.has_value());
    auto r2 = alloc.allocate(30);
    REQUIRE(r2.has_value());

    // bytes_used must be 60, which is above the 50-byte ceiling.
    CHECK(alloc.bytes_used() == 60);
    CHECK(alloc.bytes_used() > alloc.bytes_ceiling());

    alloc.deallocate(*r1, 30);
    alloc.deallocate(*r2, 30);
}

// ===========================================================================
// Test: per_context_allocator_shipping_warn_once_per_instance
//
// Rule #3: the spdlog warn is emitted exactly once per PerContextAllocator
// instance on ceiling overrun.  A second overrun from the same instance
// does not emit a second warn.  A new instance starts with a fresh flag.
//
// Verification: route spdlog to an ostringstream sink and count matching
// warn lines.
// ===========================================================================

TEST_CASE("per_context_allocator_shipping_warn_once_per_instance", "[core][alloc][shipping]") {
    // Redirect spdlog output to a string for assertion.
    std::ostringstream captured;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(captured);
    auto logger = std::make_shared<spdlog::logger>("test_warn_once", sink);
    logger->set_level(spdlog::level::warn);
    // Replace the default logger so the warn in allocate() goes here.
    spdlog::set_default_logger(logger);

    constexpr std::uint64_t kCeiling = 10;

    {
        glibre::PerContextAllocator alloc{glibre::ContextTag::tools, kCeiling};

        // First allocation over the ceiling triggers the warn.
        auto r1 = alloc.allocate(20);
        REQUIRE(r1.has_value());

        // Second allocation over the ceiling should NOT trigger another warn
        // (warn-once per instance).
        auto r2 = alloc.allocate(20);
        REQUIRE(r2.has_value());

        alloc.deallocate(*r1, 20);
        alloc.deallocate(*r2, 20);
    }

    // Flush the sink.
    logger->flush();

    const std::string output = captured.str();
    // Count occurrences of the canonical warn prefix.
    std::size_t warn_count = 0;
    std::size_t pos = 0;
    const std::string needle = "PerContextAllocator: context tag";
    while ((pos = output.find(needle, pos)) != std::string::npos) {
        ++warn_count;
        pos += needle.size();
    }
    // Exactly one warn must have been emitted for the two overruns above.
    CHECK(warn_count == 1);

    {
        // A fresh allocator instance must start with its own untripped flag.
        glibre::PerContextAllocator alloc2{glibre::ContextTag::tools, kCeiling};
        captured.str("");  // clear the sink
        auto r3 = alloc2.allocate(20);
        REQUIRE(r3.has_value());
        alloc2.deallocate(*r3, 20);
        logger->flush();
        const std::string output2 = captured.str();
        // The second instance must also emit exactly one warn.
        std::size_t warn_count2 = 0;
        std::size_t pos2 = 0;
        while ((pos2 = output2.find(needle, pos2)) != std::string::npos) {
            ++warn_count2;
            pos2 += needle.size();
        }
        CHECK(warn_count2 == 1);
    }

    // Restore default logger to avoid polluting other tests.
    spdlog::set_default_logger(spdlog::default_logger());
}
