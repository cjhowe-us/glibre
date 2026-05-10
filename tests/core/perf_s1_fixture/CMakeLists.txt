cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/perf_s1_fixture — canonical S1 fixture loader unit tests
#
# Authority: plan #1003, reviews/decisions/perf-budget.md §CI Gate Spec #1.
#
# Named test cases (plan #1003 Unit Test Plan / DoD):
#   - fixture_loads_with_expected_entity_count
#   - fixture_is_deterministic_across_runs
#   - manifest_cell_budgets_match_decision_record
#   - s1_fixture_archetype_counts_match_scenario
#   - s1_fixture_viewport_is_1920x1080
#   - s1_fixture_loads_canonical_209_entities
#   - s1_fixture_budgets_match_perf_budget_md
#
# The fixture header (core/tests/fixtures/s1.hpp) is header-only and reads
# e2e/perf/s1/scene.yaml at test runtime via std::ifstream.  It locates the
# manifest via GLIBRE_REPO_ROOT env-var or __FILE__-based upward traversal.
#
# Build flags:
#   - No -fno-exceptions: std::ifstream and std::runtime_error are used in
#     the fixture header for manifest loading.  Test executables are not on
#     the engine ABI boundary; the flag is not mandated for test-only binaries.
#   - -O2 is NOT applied here: fixture tests are correctness tests, not timing
#     benchmarks.  Debug-build overhead does not affect the assertions.
# ---------------------------------------------------------------------------

add_executable(glibre-core-perf-s1-fixture-tests
    perf_s1_fixture_test.cpp
)

target_link_libraries(glibre-core-perf-s1-fixture-tests
    PRIVATE
        Catch2::Catch2WithMain
)

target_compile_features(glibre-core-perf-s1-fixture-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-perf-s1-fixture-tests PROPERTIES CXX_MODULE_STD OFF)

# Include the core/tests/fixtures/ directory so the test file can include
# "fixtures/s1.hpp" without a full path.
target_include_directories(
    glibre-core-perf-s1-fixture-tests PRIVATE
    "${CMAKE_SOURCE_DIR}/core/tests"
)

target_compile_options(
    glibre-core-perf-s1-fixture-tests
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
        # This target intentionally enables exceptions (std::ifstream / std::runtime_error
        # in the fixture header) so it cannot link glibre::test_contract (which inherits
        # -fno-exceptions from glibre::compile_contract).  The Catch2 __COUNTER__ warning
        # is suppressed here directly instead.
        -Wno-c2y-extensions
)

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-perf-s1-fixture-tests)
