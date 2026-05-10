cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/schedule — unit tests for glibre::core::Schedule DAG builder
#
# Authority: plan #584 (Schedule DAG builder from access sets).
#            specs/core/SPEC.md §4.4, §5.6, §6.4.
#
# Named test cases (plan #584 Unit Test Plan / DoD):
#   - core/schedule: register_unregister_idempotent_for_same_id
#   - core/schedule: dag_edge_writes_reads_intersection
#   - core/schedule: dag_edge_writes_writes_intersection
#   - core/schedule: dag_edge_after_before_explicit
#   - core/schedule: cycle_yields_SystemScheduleCycle
#   - core/schedule: access_conflict_no_valid_order_yields_ScheduleAccessConflict
#   - core/schedule: tiebreaker_lex_order_of_fqn
#   - core/schedule: compile_idempotent_when_set_unchanged
# ---------------------------------------------------------------------------

add_executable(glibre-core-schedule-tests
    schedule_test.cpp
)

target_link_libraries(glibre-core-schedule-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::test_contract
)

target_compile_features(glibre-core-schedule-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-schedule-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
target_compile_options(glibre-core-schedule-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # -Wno-c2y-extensions is inherited from glibre::test_contract (cmake/Compile.cmake).
)

# GLIBRE_TESTING is propagated PUBLIC from glibre-core (core/CMakeLists.txt)
# when GLIBRE_BUILD_TESTS=ON, so glibre-core-schedule-tests receives it
# transitively via target_link_libraries(... glibre::core).
# No PRIVATE definition needed here — see plan #997.

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-schedule-tests)
