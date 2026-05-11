cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/shader/reflection — unit tests for the slangc reflection record ingester
#
# Plan: #514 — feat(shader): slangc reflection record ingester.
# Authority: specs/shader/SPEC.md §4.4, §6.3.
#
# Named test cases (plan #514 Unit Test Plan + DoD):
#   - reflection_blob_extracts_canonical_metadata_from_slangc_record
#   - slangc_reflection_record_returns_ReflectionExtractionFailed_on_truncated_record
#   - slangc_reflection_record_lists_all_entry_points_with_stage_attribute
#   - slangc_reflection_record_yields_register_ranges_per_stage_mask
#   - blob_builder_canonicalizes_binding_names_deterministically
#   - reflection_blob_equality_is_structural_over_named_fields
# ---------------------------------------------------------------------------

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    return()
endif()

add_executable(glibre-shader-reflection-tests
    reflection_test.cpp
)

target_link_libraries(glibre-shader-reflection-tests
    PRIVATE
        glibre::shader
        Catch2::Catch2WithMain
        glibre::compile_contract
)

target_compile_features(glibre-shader-reflection-tests PRIVATE cxx_std_23)

set_target_properties(glibre-shader-reflection-tests PROPERTIES CXX_MODULE_STD OFF)

target_compile_options(glibre-shader-reflection-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # Catch2 3.x macros use __COUNTER__ which triggers -Wpedantic C2y
    # extension warning with clang >= 22; suppress from third-party macro.
    -Wno-c2y-extensions
)

# Expose the reflection internal headers to the test (slangc_reflection_record.hpp,
# blob_builder.hpp).  These are private to the shader plugin; the test is the
# sole consumer outside the plugin itself.
target_include_directories(glibre-shader-reflection-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/shader/src/reflection
)

include(CTest)
include(Catch)
catch_discover_tests(glibre-shader-reflection-tests)
