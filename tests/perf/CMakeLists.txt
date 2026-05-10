cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/perf — per-context CI perf-budget gate benchmarks
#
# Authority: plan #242 (scaffolding), plan #243 (S1 sample-scene fixture),
#            perf-budget.md §CI Gate Spec #1.
#
# Each BENCHMARK_CELL in this target expands to a Catch2 TEST_CASE that:
#   (a) runs a BENCHMARK block for statistical reporting, and
#   (b) asserts the timed single run stays under the cell ceiling.
#
# Named test cases (plan #242 DoD):
#   - BENCHMARK_CELL_core_phase_under_budget
#   - BENCHMARK_CELL_render_phase_under_budget
#
# Named test cases (plan #243 DoD):
#   - s1_scene_core_phase_within_budget
#   - s1_scene_render_phase_within_budget
#
# S1 Synthetic Scene Invariants (plan #243, perf-budget.md §S1 scenario):
#   entity_count=100, archetype_count=4, system_count=2, viewport=1920x1080
#   (canonical 1+200+8 entity split deferred to plan #1003)
#
# Build flags:
#   - NO -fno-exceptions: Catch2 BENCHMARK requires exception support
#     (CATCH_TRY/CATCH_CATCH_ALL in catch_benchmark.hpp).  Test
#     executables are not on the engine ABI boundary; the flag is not
#     mandated for test-only binaries.
#   - -O2: release-like optimisations so benchmark measurements reflect
#     realistic instruction counts.  The CI gate uses the macos-debug
#     preset (CMAKE_BUILD_TYPE=Debug) but we override per-target so that
#     timing assertions are not inflated by debug-build overhead.
# ---------------------------------------------------------------------------

add_executable(glibre-perf-budget-gate-tests
    perf_budget_gate_test.cpp
    s1_sample_scene_test.cpp   # plan #243: S1 sample-scene perf fixture
)

target_link_libraries(glibre-perf-budget-gate-tests PRIVATE Catch2::Catch2WithMain)

target_compile_features(glibre-perf-budget-gate-tests PRIVATE cxx_std_23)

set_target_properties(glibre-perf-budget-gate-tests PROPERTIES CXX_MODULE_STD OFF)

# Include glibre core headers for perf_bench.hpp.
# Does NOT link glibre::core for the following reason:
#   plan #243 uses only the header-only BENCHMARK_CELL_BODY macro and the
#   local ContextTag enum copy from perf_bench.hpp.  Collapsing the duplicate
#   ContextTag (perf_bench.hpp vs. perf_budget.hpp) requires updating the
#   namespace alias in perf_bench.hpp; that refactor is deferred to plan #1003
#   which already links glibre::core for the load_s1() fixture helper.
#   See issue #1003 and the perf_bench.hpp §Dependency note.
target_include_directories(
    glibre-perf-budget-gate-tests PRIVATE "${CMAKE_SOURCE_DIR}/core/include"
)

# -O2 per-target for realistic benchmark measurements.
# -Wno-c2y-extensions: This target intentionally enables exceptions (Catch2
# BENCHMARK requires CATCH_TRY/CATCH_CATCH_ALL) so it cannot link
# glibre::test_contract (which inherits -fno-exceptions from
# glibre::compile_contract).  The Catch2 __COUNTER__ warning is suppressed
# here directly instead.  See cmake/Compile.cmake for the centralised variant.
target_compile_options(
    glibre-perf-budget-gate-tests
    PRIVATE -O2
            -Wall
            -Wextra
            -Wpedantic
            -Wno-unused-parameter
            -Wno-c2y-extensions
)

include(CTest)
include(Catch)
catch_discover_tests(glibre-perf-budget-gate-tests)
