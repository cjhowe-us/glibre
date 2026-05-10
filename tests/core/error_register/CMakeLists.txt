cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/error_register — unit tests for the per-context Error enum
# registration convention.
#
# Plan: #234 — feat(core): per-context Error enum registration convention
# Authority: core/include/glibre/error_register.hpp,
#            reviews/decisions/error-model.md §Decision 2 + §Composition Rules
#
# Named test cases (plan #234 Unit Test Plan + DoD):
#   - error_register_per_context_arms_unique
#   - error_register_compile_time_arm_count
#   - error_register_lists_known_contexts
# ---------------------------------------------------------------------------

add_executable(glibre-core-error-register-tests
    error_register_test.cpp
)

target_link_libraries(glibre-core-error-register-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::test_contract
)

target_compile_features(glibre-core-error-register-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-error-register-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
# Catch2 3.x supports -fno-exceptions when REQUIRE_THROWS is not used;
# glibre-core-error-register-tests never uses REQUIRE_THROWS.
target_compile_options(glibre-core-error-register-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # -Wno-c2y-extensions is inherited from glibre::test_contract (cmake/Compile.cmake).
)

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-error-register-tests)
