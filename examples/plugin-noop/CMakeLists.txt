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
# Plugins are not permitted to link glibre-core, but they may link EASTL
# header-only (the static lib is a single-TU stubs target; linking it here
# avoids a duplicate symbol with the engine's own EASTL link only when the
# plugin is loaded via dlopen RTLD_LOCAL — which is exactly the policy of
# the loader).
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
