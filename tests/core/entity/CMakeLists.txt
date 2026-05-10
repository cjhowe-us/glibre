cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/entity — unit tests for glibre::core::Entity value object
#
# Plan: #557 — feat(core): Entity ID encoding + generational allocator
# Authority: specs/core/SPEC.md §4.3, §5.2;
#            core/include/glibre/core/entity.hpp
#
# Named test cases (plan #557 Unit Test Plan):
#   - core/entity: bits_layout_index_low_generation_high
#   - core/entity: opaque_equality_compares_by_bits
# ---------------------------------------------------------------------------

add_executable(glibre-core-entity-tests
    entity_test.cpp
)

target_link_libraries(glibre-core-entity-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::test_contract
)

target_compile_features(glibre-core-entity-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-entity-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
# Catch2 3.x supports -fno-exceptions when REQUIRE_THROWS is not used;
# this test file uses REQUIRE / CHECK only.
target_compile_options(glibre-core-entity-tests PRIVATE
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
catch_discover_tests(glibre-core-entity-tests)
