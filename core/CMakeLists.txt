cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# glibre-core — minimal core hub
#
# Hosts the engine-wide runtime that every plugin links against:
# ECS, plugin loader, hot-reload barrier, frame loop, type registry,
# asset handles (per core/SPEC.md §1).
#
# This initial CMakeLists.txt wires the frame-loop skeleton (plan #244).
# Additional source files are added as subsequent plans land.
# ---------------------------------------------------------------------------

add_library(glibre-core STATIC
    src/frame_loop.cpp
)

target_include_directories(glibre-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(glibre-core PUBLIC cxx_std_23)

# Do not require `import std;` — frame-loop skeleton uses traditional headers.
# CXX_MODULE_STD is set by the root CMAKE_CXX_MODULE_STD=ON; override to OFF
# here since this target uses #include-based C++23, not module import std.
set_target_properties(glibre-core PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions on all engine code per reviews/decisions/error-model.md §Decision 3.
# The editor-UI carve-out (ImGui) is in tools/, not here.
target_compile_options(glibre-core PRIVATE
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
)

# ---------------------------------------------------------------------------
# Alias for consistent glibre:: namespace usage by consumers
# ---------------------------------------------------------------------------
add_library(glibre::core ALIAS glibre-core)
