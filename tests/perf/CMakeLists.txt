cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/perf — per-context CI perf-budget gate benchmarks
#
# Authority: plan #242, perf-budget.md §CI Gate Spec #1.
#
# Each BENCHMARK_CELL in this target expands to a Catch2 TEST_CASE that:
#   (a) runs a BENCHMARK block for statistical reporting, and
#   (b) asserts the timed single run stays under the cell ceiling.
#
# Named test cases (plan #242 DoD):
#   - BENCHMARK_CELL_core_phase_under_budget
#   - BENCHMARK_CELL_render_phase_under_budget
#
# Build flags:
#   - NO -fno-exceptions: Catch2 BENCHMARK requires exception support
#     (CATCH_TRY/CATCH_CATCH_ALL in catch_benchmark.hpp).  Test
#     executables are not on the engine ABI boundary; the flag is not
#     mandated for test-only binaries (only for engine library targets).
#   - -O2: release-like optimisations so that benchmark measurements
#     reflect realistic instruction counts rather than debug-build artefacts.
#     The CI gate uses the macos-debug preset (CMAKE_BUILD_TYPE=Debug)
#     but we override per-target via COMPILE_OPTIONS so that the timing
#     assertions are not inflated by debug-build overhead.
# ---------------------------------------------------------------------------

add_executable(glibre-perf-budget-gate-tests
    perf_budget_gate_test.cpp
)

target_link_libraries(glibre-perf-budget-gate-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
)

target_compile_features(glibre-perf-budget-gate-tests PRIVATE cxx_std_23)

set_target_properties(glibre-perf-budget-gate-tests PROPERTIES CXX_MODULE_STD OFF)

# -O2 per-target for realistic benchmark measurements.
# -Wall / -Wextra / -Wpedantic kept for correctness.
# -Wno-c2y-extensions: Catch2 3.x macros use __COUNTER__ which triggers the
# C2Y extension warning with clang >= 22; suppress from third-party macros.
target_compile_options(glibre-perf-budget-gate-tests PRIVATE
    -O2
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions
)

include(CTest)
include(Catch)
catch_discover_tests(glibre-perf-budget-gate-tests)
