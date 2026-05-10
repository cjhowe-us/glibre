cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/shader — Catch2 unit tests for the shader bounded context
#
# NOTE: Shader tests require the glibre-shader STATIC library, which is
# macOS-only (see plugins/shader/CMakeLists.txt iOS guard). Skip on iOS
# sub-project builds to match the plugin's iOS skip pattern.
# ---------------------------------------------------------------------------

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    return()
endif()

add_subdirectory(source)     # plan #508  — ShaderSource unit tests
add_subdirectory(descriptor) # plan #1087 — DescriptorLayout::derive unit tests
