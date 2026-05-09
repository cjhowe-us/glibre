// tests/core/error_variant/error_variant_test.cpp
//
// Catch2 unit tests codifying glibre::Error variant shape + Result<T> alias.
// Authority: plan #233, reviews/decisions/error-model.md.
//
// Named test cases (plan #233 dispatch DoD):
//   - error_variant_holds_alternative_per_context
//   - result_alias_default_constructs_to_void_success
//   - result_alias_propagates_error_via_unexpected
//   - error_context_round_trip
//
// Design constraints:
//   - -fno-exceptions / -fno-rtti (error-model.md §Decision 3).
//   - No REQUIRE_THROWS usage.
//   - Tests are intentionally minimal — they pin the *contract*, not the impl.

#include <cstdint>
#include <type_traits>

#include <EASTL/variant.h>
#include <catch2/catch_test_macros.hpp>
#include <glibre/error.hpp>
#include <glibre/error_register.hpp>

// ===========================================================================
// TEST: error_variant_holds_alternative_per_context
//
// Construct glibre::Error with each registered per-context enumerator and
// verify that eastl::holds_alternative<ctx::Error> returns true for the
// correct arm and false for all others.
//
// This is the primary contract test: the Variant discriminates correctly.
// ===========================================================================

TEST_CASE("error_variant_holds_alternative_per_context", "[core][error][variant]") {
    SECTION("core::Error arm is selected when constructed from core::Error") {
        const glibre::Error err{glibre::core::Error::PluginAbiHashMismatch};

        REQUIRE(eastl::holds_alternative<glibre::core::Error>(err.code()));
        REQUIRE_FALSE(eastl::holds_alternative<glibre::render::Error>(err.code()));
        REQUIRE_FALSE(eastl::holds_alternative<glibre::tools::Error>(err.code()));

        // The stored value is the exact enumerator passed in.
        const auto* arm = eastl::get_if<glibre::core::Error>(&err.code());
        REQUIRE(arm != nullptr);
        REQUIRE(*arm == glibre::core::Error::PluginAbiHashMismatch);
    }

    SECTION("render::Error arm is selected when constructed from render::Error") {
        const glibre::Error err{glibre::render::Error::DeviceLost};

        REQUIRE_FALSE(eastl::holds_alternative<glibre::core::Error>(err.code()));
        REQUIRE(eastl::holds_alternative<glibre::render::Error>(err.code()));
        REQUIRE_FALSE(eastl::holds_alternative<glibre::tools::Error>(err.code()));

        const auto* arm = eastl::get_if<glibre::render::Error>(&err.code());
        REQUIRE(arm != nullptr);
        REQUIRE(*arm == glibre::render::Error::DeviceLost);
    }

    SECTION("tools::Error arm is selected when constructed from tools::Error") {
        const glibre::Error err{glibre::tools::Error::ForycSyntaxError};

        REQUIRE_FALSE(eastl::holds_alternative<glibre::core::Error>(err.code()));
        REQUIRE_FALSE(eastl::holds_alternative<glibre::render::Error>(err.code()));
        REQUIRE(eastl::holds_alternative<glibre::tools::Error>(err.code()));

        const auto* arm = eastl::get_if<glibre::tools::Error>(&err.code());
        REQUIRE(arm != nullptr);
        REQUIRE(*arm == glibre::tools::Error::ForycSyntaxError);
    }

    // Compile-time: variant arm count matches the registered manifest.
    // (The static_assert lives in error_register.hpp; including it is sufficient.)
    // mirrors error_register.hpp — intentionally redundant for test-side
    // documentation: if the header-level assert is removed, the test still pins
    // the contract and the breakage is caught here.
    static_assert(
        eastl::variant_size_v<glibre::Error::Variant> == glibre::kExpectedArmCount,
        "Variant arm count drifted from kExpectedArmCount — "
        "update error_register.hpp when adding a new context."
    );
}

// ===========================================================================
// TEST: result_alias_default_constructs_to_void_success
//
// Result<void> default-constructs to the value state (has_value() == true).
// This is the canonical "no error" baseline; every function returning
// Result<void> can use `return {};` for success.
// ===========================================================================

TEST_CASE("result_alias_default_constructs_to_void_success", "[core][error][result]") {
    // Default-construct a Result<void> and verify it holds a value, not an error.
    const glibre::Result<void> r;

    REQUIRE(r.has_value());
    REQUIRE(static_cast<bool>(r));

    // Compile-time: alias identity — must remain std::expected<T, glibre::Error>.
    // mirrors error.hpp:188 — intentionally redundant for test-side documentation:
    // if the header-level assert is removed, the test still pins the contract.
    static_assert(
        std::is_same_v<glibre::Result<void>, std::expected<void, glibre::Error>>,
        "glibre::Result<T> must be an alias for std::expected<T, glibre::Error>."
    );

    // Underlying type contract: per-context enums use std::uint16_t.
    // mirrors error.hpp:196 — intentionally redundant for test-side documentation.
    static_assert(
        std::is_same_v<std::underlying_type_t<glibre::core::Error>, std::uint16_t>,
        "core::Error underlying type must be std::uint16_t."
    );
}

// ===========================================================================
// TEST: result_alias_propagates_error_via_unexpected
//
// Result<int> constructed from std::unexpected(Error{...}) must:
//   - has_value() == false
//   - error().code() holds the correct arm and enumerator
//
// This pins the round-trip contract through std::unexpected.
// ===========================================================================

TEST_CASE("result_alias_propagates_error_via_unexpected", "[core][error][result]") {
    constexpr glibre::core::Error sentinel = glibre::core::Error::OutOfBudget;

    const glibre::Result<int> r = std::unexpected(glibre::Error{sentinel});

    REQUIRE_FALSE(r.has_value());
    REQUIRE_FALSE(static_cast<bool>(r));

    // error() returns the glibre::Error that was wrapped in std::unexpected.
    const glibre::Error& err = r.error();
    REQUIRE(eastl::holds_alternative<glibre::core::Error>(err.code()));

    const auto* arm = eastl::get_if<glibre::core::Error>(&err.code());
    REQUIRE(arm != nullptr);
    REQUIRE(*arm == sentinel);
}

// ===========================================================================
// TEST: error_context_round_trip
//
// Construct glibre::Error with a non-default ErrorContext and verify that
// where() returns the exact file, line, and detail values supplied.
//
// Pins the ErrorContext POD contract: field access is lossless.
// ===========================================================================

TEST_CASE("error_context_round_trip", "[core][error][context]") {
    constexpr eastl::string_view test_file = "tests/core/error_variant/error_variant_test.cpp";
    constexpr int test_line = 42;
    constexpr eastl::string_view test_detail = "synthetic error for round-trip test";

    const glibre::ErrorContext ctx{
        .file   = test_file,
        .line   = test_line,
        .detail = test_detail,
    };
    const glibre::Error err{glibre::core::Error::HotReloadRefused, ctx};

    // Variant arm is correct.
    REQUIRE(eastl::holds_alternative<glibre::core::Error>(err.code()));
    const auto* arm = eastl::get_if<glibre::core::Error>(&err.code());
    REQUIRE(arm != nullptr);
    REQUIRE(*arm == glibre::core::Error::HotReloadRefused);

    // Context fields survive the round-trip unchanged.
    REQUIRE(err.where().file   == test_file);
    REQUIRE(err.where().line   == test_line);
    REQUIRE(err.where().detail == test_detail);

    SECTION("default ErrorContext is empty") {
        // When glibre::Error is constructed without an explicit ErrorContext
        // (the path used by every production Result<T>=std::unexpected(Error{e})
        // call site that does not supply a GLIBRE_TRY_AT location), all three
        // fields must be at their zero-value: empty string_views and line 0.
        // This guards against accidental aggregate-init drift in ErrorContext.
        const glibre::Error bare{glibre::core::Error::OutOfBudget};

        REQUIRE(bare.where().file.empty());
        REQUIRE(bare.where().line == 0);
        REQUIRE(bare.where().detail.empty());
    }
}
