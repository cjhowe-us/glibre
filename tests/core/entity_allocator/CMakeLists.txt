cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/entity_allocator — unit tests for glibre::core::EntityAllocator
#
# Plan: #557 — feat(core): Entity ID encoding + generational allocator
# Authority: specs/core/SPEC.md §4.1, §4.3;
#            core/src/world/entity_allocator.hpp
#
# Named test cases (plan #557 Unit Test Plan):
#   - core/entity_allocator: spawn_returns_unique_handles
#   - core/entity_allocator: despawn_increments_generation
#   - core/entity_allocator: stale_handle_resolves_to_EntityStale
#   - core/entity_allocator: free_list_reuses_lowest_index_first
#
# NOTE: EntityAllocator is an internal type (core/src/world/).  The test
# target adds core/src as a PRIVATE include directory so the test file can
# #include "entity_allocator.hpp" directly.  This does NOT expose the
# internal header to other library targets or plugin targets.
# ---------------------------------------------------------------------------

add_executable(glibre-core-entity-allocator-tests
    entity_allocator_test.cpp
)

target_link_libraries(glibre-core-entity-allocator-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::test_contract
)

# Add the internal source directory so the test can include entity_allocator.hpp.
# PRIVATE — only this test target sees the path; glibre::core already compiled
# entity_allocator.cpp and exposed nothing to its public interface.
target_include_directories(glibre-core-entity-allocator-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/core/src/world
)

target_compile_features(glibre-core-entity-allocator-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-entity-allocator-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
# Catch2 3.x supports -fno-exceptions when REQUIRE_THROWS is not used;
# this test file uses REQUIRE / CHECK only.
target_compile_options(glibre-core-entity-allocator-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # -Wno-c2y-extensions is inherited from glibre::test_contract (cmake/Compile.cmake).
)

# GLIBRE_TESTING is propagated PUBLIC from glibre-core when GLIBRE_BUILD_TESTS=ON,
# so no PRIVATE definition is needed here.

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-entity-allocator-tests)
