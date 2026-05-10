cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/asset_table — unit tests for glibre::core::AssetTable<T>
#                          and glibre::core::AssetRegistry
#
# Plan: #598 — feat(core): AssetHandle generational table + per-T registry
# Authority: specs/core/SPEC.md §4.7, §6.8;
#            core/src/asset/asset_table.hpp
#            core/src/asset/asset_registry.hpp
#
# Named test cases (plan #598 Unit Test Plan):
#   - core/asset_table: insert_returns_unique_handle
#   - core/asset_table: release_increments_generation
#   - core/asset_table: stale_handle_resolves_to_AssetStale
#   - core/asset_table: free_index_reused_lowest_first
#   - core/asset_registry: per_T_table_instantiated_on_first_insert
#
# Note: asset_table.hpp and asset_registry.hpp are INTERNAL headers
# (core/src/asset/), not part of the public include surface.  This test
# target reaches them via an PRIVATE include_directories entry.
# ---------------------------------------------------------------------------

add_executable(glibre-core-asset-table-tests
    asset_table_test.cpp
)

target_include_directories(glibre-core-asset-table-tests
    PRIVATE
        # Internal asset headers (asset_table.hpp, asset_registry.hpp).
        ${CMAKE_SOURCE_DIR}/core/src/asset
)

target_link_libraries(glibre-core-asset-table-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::test_contract
)

target_compile_features(glibre-core-asset-table-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-asset-table-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
# Catch2 3.x supports -fno-exceptions when REQUIRE_THROWS is not used;
# this test file uses REQUIRE / CHECK only.
target_compile_options(glibre-core-asset-table-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # -Wno-c2y-extensions is inherited from glibre::test_contract (cmake/Compile.cmake).
)

# GLIBRE_TESTING is propagated PUBLIC from glibre-core when GLIBRE_BUILD_TESTS=ON,
# so no PRIVATE definition is needed here.

# GLIBRE_ALLOC_STRICT: strict mode for ceiling enforcement during tests.
target_compile_definitions(glibre-core-asset-table-tests PRIVATE
    GLIBRE_ALLOC_STRICT=1
)

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-asset-table-tests)
