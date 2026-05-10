cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# tests/data/schemas/geometry — Fory side-record schema parse tests (plan #696)
#
# Tests the three geometry Fory side-record schemas introduced in plan #696:
#   - CookManifest.fory        (specs/geometry/SPEC.md §7.1.1)
#   - BLASRecipeRecord.fory    (specs/geometry/SPEC.md §7.1.2)
#   - MeshSourceMetadata.fory  (specs/geometry/SPEC.md §7.1.3)
#
# Named tests (DoD smoke-parse):
#   - geometry_cook_manifest_schema_parses
#   - geometry_blas_recipe_record_schema_parses
#   - geometry_mesh_source_metadata_schema_parses
#
# Named tests (plan #696 Unit Test Plan — structural invariants):
#   - data/schemas/geometry/CookManifest: round_trip_preserves_triple_lock
#   - data/schemas/geometry/CookManifest: rejects_meshlet_caps_other_than_64_124
#   - data/schemas/geometry/CookManifest: rejects_unknown_draco_profile_value
#   - data/schemas/geometry/BLASRecipeRecord: round_trip_preserves_descriptor_order
#   - data/schemas/geometry/BLASRecipeRecord: rejects_format_hash_mismatch_with_companion_pak
#   - data/schemas/geometry/MeshSourceMetadata: round_trip_preserves_bounding_box
#
# Sources pulled directly from tools/foryc/src/ (same pattern as other
# schema/foryc tests) so the binary is self-contained with no shared-library
# boundary at test time.
# ---------------------------------------------------------------------------

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    return()
endif()

find_package(Catch2 3 CONFIG REQUIRED)

add_executable(geometry-schema-parse-tests
    geometry_schema_parse_test.cpp
    # Pull parser source directly into the test binary.
    ${CMAKE_SOURCE_DIR}/tools/foryc/src/parser.cpp
)

target_include_directories(geometry-schema-parse-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/foryc/src
)

target_link_libraries(geometry-schema-parse-tests
    PRIVATE
        Catch2::Catch2WithMain
        glibre::core   # glibre::Error
        glibre::test_contract
)

target_compile_features(geometry-schema-parse-tests PRIVATE cxx_std_23)

# CXX_MODULE_STD OFF: Catch2 + clang on macOS 26 has duplicate-symbol issues
# when std module import is enabled.  Consistent with all other test targets.
set_target_properties(geometry-schema-parse-tests PROPERTIES CXX_MODULE_STD OFF)

target_compile_options(geometry-schema-parse-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # -Wno-c2y-extensions is inherited from glibre::test_contract (cmake/Compile.cmake).
)

include(CTest)
include(Catch)
catch_discover_tests(geometry-schema-parse-tests)

# Thread the on-disk schema directory into the test binary so parse tests
# exercise the canonical .fory artefacts named by the DoD file_exists: block.
# The macro expands to an absolute path at configure time; ctest does not need
# to set any working directory for file reads to succeed.
target_compile_definitions(geometry-schema-parse-tests
    PRIVATE
        GEOMETRY_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/data/schemas/geometry"
)
