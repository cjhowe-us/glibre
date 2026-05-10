cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/shader/source — unit tests for glibre::shader::ShaderSource
#
# Plan: #508 — ShaderSource open + include resolver + entry-point scanner.
# Authority: specs/shader/SPEC.md §4.1.
#
# Named test cases (plan #508 Unit Test Plan + DoD):
#   - shader_source_open_validates_entry_points
#   - shader_source_resolves_includes
#   - shader_source_include_closure_rejects_escape_and_cycle
#   - shader_source_open_returns_SourceNotFound_for_missing_path
#   - shader_source_preprocessed_total_hash_is_byte_stable_across_calls
#   - shader_source_open_returns_EntryPointStageAmbiguous_on_dual_attributes
#   - include_resolver_rejects_absolute_path_with_IncludeEscape
#   - include_resolver_rejects_upward_traversal_with_IncludeEscape
#   - include_resolver_detects_cycle_with_IncludeCycle
# ---------------------------------------------------------------------------

add_executable(glibre-shader-source-tests
    shader_source_test.cpp
)

target_link_libraries(glibre-shader-source-tests
    PRIVATE
        glibre::shader
        Catch2::Catch2WithMain
        glibre::test_contract
)

target_compile_features(glibre-shader-source-tests PRIVATE cxx_std_23)

set_target_properties(glibre-shader-source-tests PROPERTIES CXX_MODULE_STD OFF)

# Provide the absolute path to the test fixture directory at compile time.
# Tests use GLIBRE_SHADER_FIXTURE_DIR to open fixture .slang files without
# depending on the current working directory at test runtime.
target_compile_definitions(glibre-shader-source-tests PRIVATE
    GLIBRE_SHADER_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_compile_options(glibre-shader-source-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # -Wno-c2y-extensions is inherited from glibre::test_contract (cmake/Compile.cmake).
)

include(CTest)
include(Catch)
catch_discover_tests(glibre-shader-source-tests)
