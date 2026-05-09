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

target_link_libraries(glibre-plugin-noop
    PRIVATE
        glibre::core
)

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
