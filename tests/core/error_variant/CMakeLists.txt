cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/error_variant — unit tests codifying glibre::Error variant shape
#                             and Result<T> alias contract.
#
# Plan: #233 — glibre::Error variant set + std::expected<T,Error> alias
# Authority: reviews/decisions/error-model.md §Type Sketch,
#            core/include/glibre/error.hpp,
#            core/include/glibre/error_register.hpp
#
# Named test cases (plan #233 dispatch DoD):
#   - error_variant_holds_alternative_per_context
#   - result_alias_default_constructs_to_void_success
#   - result_alias_propagates_error_via_unexpected
#   - error_context_round_trip
#
# Named test cases added by plan #1049 (EASTL sweep + std::visit + Overloaded):
#   - core/error_variant: visit_with_overloaded_helper_is_exhaustive
# ---------------------------------------------------------------------------

add_executable(glibre-core-error-variant-tests
    error_variant_test.cpp
)

target_link_libraries(glibre-core-error-variant-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::compile_contract
)

target_compile_features(glibre-core-error-variant-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-error-variant-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
# Catch2 3.x supports -fno-exceptions when REQUIRE_THROWS is not used;
# glibre-core-error-variant-tests never uses REQUIRE_THROWS.
target_compile_options(glibre-core-error-variant-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # Catch2 3.x macros use __COUNTER__ which triggers -Wpedantic C2y
    # extension warning with clang >= 22; suppress from third-party macro.
    # TODO: remove once Catch2 upstream lands a fix (tracked at
    # https://github.com/catchorg/Catch2/issues/2892).  Until then, narrowing
    # via SYSTEM includes does not help because __COUNTER__ is expanded in our
    # TU by macros that originate in Catch2 headers — the diagnostic fires in
    # our code, not in the system include.  The same flag is applied to sibling
    # targets error_register and error_propagation for consistency.
    -Wno-c2y-extensions
)

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-error-variant-tests)
