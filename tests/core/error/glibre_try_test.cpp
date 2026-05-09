// tests/core/error/glibre_try_test.cpp
//
// Catch2 unit tests for GLIBRE_TRY and GLIBRE_TRY_VOID macros (plan #235).
//
// Named test cases (plan #235 Unit Test Plan, DoD):
//   - glibre_try_propagates_error_unchanged
//   - glibre_try_binds_value_on_success
//   - glibre_try_works_with_void_result
//   - glibre_try_in_nested_calls
//
// Design constraints:
//   - -fno-exceptions / -fno-rtti (error-model.md §Decision 3).
//   - All tests use REQUIRE, never REQUIRE_THROWS, to stay clean under
//     -fno-exceptions (Catch2 3.x is compatible when REQUIRE_THROWS is absent).

#include <cstdint>

#include <EASTL/variant.h>
#include <catch2/catch_test_macros.hpp>
#include <glibre/error.hpp>

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

namespace {

// Sentinel error enums used in tests — avoids depending on specific production
// enumerators and keeps tests self-contained.

[[nodiscard]] glibre::Result<int> returns_value(int v) {
    return v;
}

[[nodiscard]] glibre::Result<int> returns_error(glibre::core::Error e) {
    return std::unexpected(glibre::Error{e});
}

[[nodiscard]] glibre::Result<void> returns_void_ok() {
    return {};
}

[[nodiscard]] glibre::Result<void> returns_void_error(glibre::core::Error e) {
    return std::unexpected(glibre::Error{e});
}

// ---------------------------------------------------------------------------
// Callers that use GLIBRE_TRY and GLIBRE_TRY_VOID — live inside a function
// that returns glibre::Result<T> so early-return is valid.
// ---------------------------------------------------------------------------

/// Wraps returns_error.  If the inner call fails, GLIBRE_TRY propagates.
[[nodiscard]] glibre::Result<int> caller_that_propagates(glibre::core::Error e) {
    GLIBRE_TRY(x, returns_error(e));
    // Not reached on error path.
    return x + 1;
}

/// Wraps returns_value.  On success, GLIBRE_TRY binds the value.
[[nodiscard]] glibre::Result<int> caller_that_succeeds(int v) {
    GLIBRE_TRY(x, returns_value(v));
    return x * 2;
}

/// Wraps returns_void_error.  GLIBRE_TRY_VOID propagates the error.
[[nodiscard]] glibre::Result<int> caller_with_void_step(glibre::core::Error e) {
    GLIBRE_TRY_VOID(returns_void_error(e));
    // Not reached on error path.
    return 42;
}

/// Wraps returns_void_ok.  GLIBRE_TRY_VOID does not return; caller continues.
[[nodiscard]] glibre::Result<int> caller_with_void_step_ok() {
    GLIBRE_TRY_VOID(returns_void_ok());
    return 99;
}

/// Chain of two GLIBRE_TRY calls, each on its own line.
/// Verifies that the __LINE__-mangled temporaries do not collide.
[[nodiscard]] glibre::Result<int> caller_chained(int a, int b) {
    GLIBRE_TRY(x, returns_value(a));
    GLIBRE_TRY(y, returns_value(b));
    return x + y;
}

/// Chain that fails at the second call.
[[nodiscard]] glibre::Result<int> caller_chained_fail_second(glibre::core::Error e) {
    GLIBRE_TRY(x, returns_value(10));
    GLIBRE_TRY(y, returns_error(e));
    // x is in scope but y failed: the expression below is unreachable.
    return x + y;
}

}  // namespace

// ---------------------------------------------------------------------------
// TEST: glibre_try_propagates_error_unchanged
//
// When the inner call returns an error, GLIBRE_TRY must propagate the exact
// same glibre::Error variant arm without altering the enumerator.
// ---------------------------------------------------------------------------

TEST_CASE("glibre_try_propagates_error_unchanged", "[core][error][glibre_try]") {
    constexpr glibre::core::Error sentinel = glibre::core::Error::OutOfBudget;

    auto result = caller_that_propagates(sentinel);

    REQUIRE_FALSE(result.has_value());

    const auto* arm = eastl::get_if<glibre::core::Error>(&result.error().code());
    REQUIRE(arm != nullptr);
    REQUIRE(*arm == sentinel);
}

// ---------------------------------------------------------------------------
// TEST: glibre_try_binds_value_on_success
//
// When the inner call succeeds, GLIBRE_TRY must bind the unwrapped value
// to the named local and allow the caller to use it.
// ---------------------------------------------------------------------------

TEST_CASE("glibre_try_binds_value_on_success", "[core][error][glibre_try]") {
    constexpr int input = 7;
    auto result = caller_that_succeeds(input);

    REQUIRE(result.has_value());
    REQUIRE(*result == input * 2);  // caller returns x * 2 where x == input
}

// ---------------------------------------------------------------------------
// TEST: glibre_try_works_with_void_result
//
// GLIBRE_TRY_VOID must propagate errors from Result<void> call sites, and
// must not return when the inner call succeeds.
// ---------------------------------------------------------------------------

TEST_CASE("glibre_try_works_with_void_result", "[core][error][glibre_try]") {
    SECTION("propagates error from Result<void>") {
        constexpr glibre::core::Error sentinel = glibre::core::Error::SystemScheduleCycle;

        auto result = caller_with_void_step(sentinel);

        REQUIRE_FALSE(result.has_value());
        const auto* arm = eastl::get_if<glibre::core::Error>(&result.error().code());
        REQUIRE(arm != nullptr);
        REQUIRE(*arm == sentinel);
    }

    SECTION("continues past successful Result<void>") {
        auto result = caller_with_void_step_ok();

        REQUIRE(result.has_value());
        REQUIRE(*result == 99);
    }
}

// ---------------------------------------------------------------------------
// TEST: glibre_try_in_nested_calls
//
// Multiple GLIBRE_TRY calls on separate lines in the same function must not
// cause name collisions (the __LINE__ mangling keeps temporaries distinct).
// Also verifies that failure at the second call propagates correctly.
// ---------------------------------------------------------------------------

TEST_CASE("glibre_try_in_nested_calls", "[core][error][glibre_try]") {
    SECTION("both calls succeed — values are summed") {
        auto result = caller_chained(3, 5);

        REQUIRE(result.has_value());
        REQUIRE(*result == 8);
    }

    SECTION("second call fails — error propagates, not first value") {
        constexpr glibre::core::Error sentinel = glibre::core::Error::ScheduleAccessConflict;

        auto result = caller_chained_fail_second(sentinel);

        REQUIRE_FALSE(result.has_value());
        const auto* arm = eastl::get_if<glibre::core::Error>(&result.error().code());
        REQUIRE(arm != nullptr);
        REQUIRE(*arm == sentinel);
    }
}
