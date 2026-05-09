cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/error — unit tests for GLIBRE_TRY / GLIBRE_TRY_VOID macros
#
# Plan: #235 — GLIBRE_TRY macro for std::expected propagation
# Authority: reviews/decisions/error-model.md §Open Questions #2,
#            core/include/glibre/error.hpp
#
# Named test cases (plan #235 Unit Test Plan + DoD):
#   - glibre_try_propagates_error_unchanged
#   - glibre_try_binds_value_on_success
#   - glibre_try_works_with_void_result
#   - glibre_try_in_nested_calls
# ---------------------------------------------------------------------------

add_executable(glibre-core-error-tests
    glibre_try_test.cpp
)

target_link_libraries(glibre-core-error-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
)

target_compile_features(glibre-core-error-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-error-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
# Catch2 3.x supports -fno-exceptions when REQUIRE_THROWS is not used;
# glibre-core-error-tests never uses REQUIRE_THROWS.
target_compile_options(glibre-core-error-tests PRIVATE
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # Catch2 3.x macros use __COUNTER__ which triggers -Wpedantic C2y
    # extension warning with clang >= 22; suppress from third-party macro.
    -Wno-c2y-extensions
)

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-error-tests)
