cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/perf_budget — unit tests for glibre::PerfBudget
#
# Named test cases (plan #241 Unit Test Plan / DoD):
#   - perf_budget_records_cpu_time_per_context
#   - perf_budget_records_heap_bytes_per_context
#   - perf_budget_reset_at_phase_9
#   - perf_budget_concurrent_records_thread_safe
# ---------------------------------------------------------------------------

add_executable(glibre-core-perf-budget-tests
    perf_budget_test.cpp
)

target_link_libraries(glibre-core-perf-budget-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::compile_contract
)

target_compile_features(glibre-core-perf-budget-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-perf-budget-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
# std::thread / std::atomic are permitted carve-outs per PHILOSOPHY §11.
target_compile_options(glibre-core-perf-budget-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions
)

# Enable GLIBRE_TESTING so that FrameLoop::last_tick_phase_ordinals() is
# available in the reset_at_phase_9 test.
target_compile_definitions(glibre-core-perf-budget-tests PRIVATE GLIBRE_TESTING)

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-perf-budget-tests)
