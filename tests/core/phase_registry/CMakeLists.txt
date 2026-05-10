cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/phase_registry — unit tests for glibre::core::PhaseRegistry
#
# Named test cases (plan #245 Unit Test Plan / DoD):
#   - core/phase_registry: register_system_idempotent
#   - core/phase_registry: iteration_order_matches_registration_order
#   - core/phase_registry: per_phase_isolation
# ---------------------------------------------------------------------------

add_executable(glibre-core-phase-registry-tests
    phase_registry_test.cpp
)

target_link_libraries(glibre-core-phase-registry-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::test_contract
)

target_compile_features(glibre-core-phase-registry-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-phase-registry-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
target_compile_options(glibre-core-phase-registry-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # -Wno-c2y-extensions is inherited from glibre::test_contract (cmake/Compile.cmake).
)

# GLIBRE_TESTING is propagated PUBLIC from glibre-core (core/CMakeLists.txt)
# when GLIBRE_BUILD_TESTS=ON, so glibre-core-phase-registry-tests receives it
# transitively via target_link_libraries(... glibre::core).
# No PRIVATE definition needed here — see plan #997.

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-phase-registry-tests)
