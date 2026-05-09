cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# glibre-plugin-noop — reference no-op plugin
#
# Purpose:
#   • Compile-time / link-time verification that plugin_context.hpp is
#     self-contained and that the four required C export symbols build.
#   • Canonical plugin shape for plugin authors to copy.
#
# This target does NOT ship in distribution builds; it is a development
# artifact / link-test.  The nm-based symbol-export test in
# tests/core/plugin_api exercises this dylib's exported symbols.
#
# See: reviews/decisions/plugin-abi.md §"Plugin file shape"
#      core/include/glibre/core/plugin_context.hpp
# ---------------------------------------------------------------------------

add_library(glibre-plugin-noop MODULE
    plugin_noop.cpp
)

# plugin-abi.md §"Plugin file shape" rule 2: plugins must NOT link glibre-core.
# They link only glibre-types.dylib (added here once that target exists in plan
# #223/#225).  For MVP the plugin needs only the C-ABI declarations and the
# PluginContext POD layout — both header-only from core/include.
#
# EASTL headers are needed transitively via plugin_context.hpp → error.hpp.
# Since the noop plugin does not link glibre::core (which exports EASTL PUBLIC),
# we use find_package(EASTL) to obtain the include path and link the interface.
#
# TODO(#224): Switch to `target_link_libraries(glibre-plugin-noop PRIVATE
#   glibre::types)` once plan #224 (glibre-types.dylib CMake target) lands.
#   glibre-types.dylib will link and re-export EASTL PUBLIC, making the
#   find_package(EASTL) + direct link below unnecessary.
#
# WARNING: The direct EASTL link here risks two independent EASTL runtimes
# inside one process if any EASTL singleton/global state (allocator hooks,
# debug counters, hash seeds) diverges between the host copy and the plugin
# copy loaded via dlopen(RTLD_LOCAL).  Once #224 lands, both the engine and
# all plugins will share a single EASTL runtime via glibre-types.dylib.
# See MED finding round-1 of PR #959 for details.
find_package(EASTL CONFIG REQUIRED)

target_include_directories(glibre-plugin-noop
    PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
)

target_link_libraries(glibre-plugin-noop PRIVATE EASTL)

target_compile_features(glibre-plugin-noop PRIVATE cxx_std_23)

set_target_properties(glibre-plugin-noop PROPERTIES
    # .dylib extension for macOS shared libraries / bundles.
    SUFFIX ".dylib"
    # Conventional plugin naming: no "lib" prefix.
    PREFIX ""
    # Hide all symbols by default; entry-points carry [[gnu::visibility("default")]].
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # Do not use `import std;` — plugin_noop.cpp uses #include-based C++23.
    CXX_MODULE_STD OFF
)

target_compile_options(glibre-plugin-noop PRIVATE
    -fno-exceptions
    -fno-rtti
    -fvisibility=hidden
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
)
