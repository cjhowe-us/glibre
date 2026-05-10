cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/shader/descriptor — unit tests for glibre::shader::DescriptorLayout
#
# Plan: #1087 — iterate-shader-descriptor-allocator-threading.
# Authority: specs/shader/SPEC.md §4.5.
#
# Named test cases (plan #1087 Unit Test Plan + DoD):
#   - descriptor_derive_uses_pmr_resource
# ---------------------------------------------------------------------------

add_executable(glibre-shader-descriptor-tests
    descriptor_layout_test.cpp
)

target_link_libraries(glibre-shader-descriptor-tests
    PRIVATE
        glibre::shader
        Catch2::Catch2WithMain
        glibre::compile_contract
)

target_compile_features(glibre-shader-descriptor-tests PRIVATE cxx_std_23)

set_target_properties(glibre-shader-descriptor-tests PROPERTIES CXX_MODULE_STD OFF)

target_compile_options(glibre-shader-descriptor-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # Catch2 3.x macros use __COUNTER__ which triggers -Wpedantic C2y
    # extension warning with clang >= 22; suppress from third-party macro.
    -Wno-c2y-extensions
)

include(CTest)
include(Catch)
catch_discover_tests(glibre-shader-descriptor-tests)
