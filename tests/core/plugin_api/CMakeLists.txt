cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/plugin_api — unit tests for glibre::core PluginContext + entry-point
#
# Named test cases (plan #228 Unit Test Plan):
#   - plugin_context_is_pod_aggregate
#   - register_signature_compiles
#   - noop_plugin_dylib_builds
#   - noop_plugin_exports_four_required_symbols
#   - plugin_context_register_smoke
#   - plugin_context_register_after_phase8_fails
# ---------------------------------------------------------------------------

add_executable(glibre-core-plugin-api-tests
    plugin_api_test.cpp
)

target_link_libraries(glibre-core-plugin-api-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::compile_contract
)

target_compile_features(glibre-core-plugin-api-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-plugin-api-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
target_compile_options(glibre-core-plugin-api-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions
)

# ---------------------------------------------------------------------------
# GLIBRE_NOOP_DYLIB_PATH — injected so the nm-based symbol-export test can
# find the noop plugin dylib without hardcoding a path.
#
# $<TARGET_FILE:glibre-plugin-noop> resolves to the full absolute path of the
# built .dylib at build time.  The test uses this string via popen(nm ...).
#
# The noop_plugin_exports_four_required_symbols test skips gracefully when
# GLIBRE_NOOP_DYLIB_PATH is not defined (e.g. GLIBRE_BUILD_EXAMPLES=OFF).
# ---------------------------------------------------------------------------

if(TARGET glibre-plugin-noop)
    # Inject the dylib path as a quoted string literal.
    target_compile_definitions(glibre-core-plugin-api-tests PRIVATE
        GLIBRE_NOOP_DYLIB_PATH="$<TARGET_FILE:glibre-plugin-noop>"
    )

    # Ensure the noop plugin is built before this test binary runs.
    add_dependencies(glibre-core-plugin-api-tests glibre-plugin-noop)
endif()

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-plugin-api-tests)
