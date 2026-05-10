cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/asset_handle — unit tests for glibre::core::AssetHandle<T>
#
# Plan: #598 — feat(core): AssetHandle generational table + per-T registry
# Authority: specs/core/SPEC.md §4.7, §5.2, §6.8;
#            core/include/glibre/core/asset_handle.hpp
#
# Named test cases (plan #598 Unit Test Plan):
#   - core/asset_handle: bit_layout_index_generation_typetag
#   - core/asset_handle: opaque_equality_compares_by_bits
# ---------------------------------------------------------------------------

add_executable(glibre-core-asset-handle-tests
    asset_handle_test.cpp
)

target_link_libraries(glibre-core-asset-handle-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::compile_contract
)

target_compile_features(glibre-core-asset-handle-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-asset-handle-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
# Catch2 3.x supports -fno-exceptions when REQUIRE_THROWS is not used;
# this test file uses REQUIRE / CHECK only.
target_compile_options(glibre-core-asset-handle-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # Catch2 3.x macros use __COUNTER__ which triggers -Wpedantic C2y
    # extension warning with clang >= 22; suppress from third-party macro.
    -Wno-c2y-extensions
)

# GLIBRE_TESTING is propagated PUBLIC from glibre-core when GLIBRE_BUILD_TESTS=ON,
# so no PRIVATE definition is needed here.

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-asset-handle-tests)
