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
    src/eastl_alloc.cpp              # EASTL operator new[] substrate (PHILOSOPHY §11)
    src/plugin_manifest.cpp
    src/plugin_loader.cpp            # PluginLoader: dlopen+dlsym+manifest read (plan #229)
    src/plugin_loader_registry.cpp   # PluginLoaderRegistry: ABI+version+name+deps gates (plan #230)
    src/plugin_loader_actions.cpp    # Loader-procedure free functions: steps 9–11 (plan #231)
    src/log_error.cpp                # Structured error logging helper (plan #236)
    src/alloc/transient_arena.cpp    # Per-context transient bump-pointer arena (plan #239)
)

# EASTL is the substrate container/string/variant library per PHILOSOPHY §11.
# glibre/error.hpp uses eastl::variant and eastl::string_view; linking EASTL
# PUBLIC so every consumer of glibre::core transitively gets the include path.
find_package(EASTL CONFIG REQUIRED)

# spdlog — structured logging.  Used by log_error.cpp (plan #236).
# Linked PUBLIC so consumers of glibre::core can call log_error() without a
# separate find_package(spdlog) in their own CMakeLists.txt.
find_package(spdlog CONFIG REQUIRED)

target_include_directories(glibre-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(glibre-core
    PUBLIC
        EASTL
        spdlog::spdlog
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
    # Promote -Wswitch to an error so hand-written to_string() and tag_string()
    # completeness is enforced at build time, not just by runtime unit tests.
    # This is the guarantee described in log_error.hpp §"to_string design choice".
    -Werror=switch
)

# ---------------------------------------------------------------------------
# Alias for consistent glibre:: namespace usage by consumers
# ---------------------------------------------------------------------------
add_library(glibre::core ALIAS glibre-core)
