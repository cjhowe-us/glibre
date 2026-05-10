cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/plugin_loader — unit tests for glibre::core::PluginLoader
#
# Plan: #229 — PluginLoader dlopen + dlsym + manifest read
# Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" steps 1–3.
#
# Named test cases (plan #229 Unit Test Plan + DoD):
#   - plugin_loader_open_loads_noop_plugin
#   - plugin_loader_open_returns_error_on_missing_path
#   - plugin_loader_open_returns_error_on_missing_symbols
#   - dlopen_failure_returns_plugin_dlopen_failed
#   - missing_symbol_returns_plugin_missing_entry_point
#   - success_reads_manifest_with_expected_fields
#   - invalid_manifest_returns_plugin_manifest_invalid
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# glibre-plugin-stub-no-symbols — a valid .dylib that intentionally exports
# none of the four required plugin entry-point symbols.  Used by the
# plugin_loader_open_returns_error_on_missing_symbols and
# missing_symbol_returns_plugin_missing_entry_point test cases.
# ---------------------------------------------------------------------------

add_library(glibre-plugin-stub-no-symbols MODULE
    stub_no_symbols.cpp
)

target_link_libraries(glibre-plugin-stub-no-symbols PRIVATE glibre::compile_contract)

target_compile_features(glibre-plugin-stub-no-symbols PRIVATE cxx_std_23)

set_target_properties(glibre-plugin-stub-no-symbols PROPERTIES
    SUFFIX ".dylib"
    PREFIX ""
    CXX_VISIBILITY_PRESET  hidden
    VISIBILITY_INLINES_HIDDEN ON
    CXX_MODULE_STD OFF
)

target_compile_options(glibre-plugin-stub-no-symbols PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # -Wno-c2y-extensions omitted: this stub contains no Catch2 includes and
    # therefore no __COUNTER__ expansions that trigger the warning.  The flag is
    # only needed (and provided) by glibre::test_contract for Catch2 executables.
)

# ---------------------------------------------------------------------------
# glibre-core-plugin-loader-tests — Catch2 test binary
# ---------------------------------------------------------------------------

add_executable(glibre-core-plugin-loader-tests
    plugin_loader_test.cpp
)

target_link_libraries(glibre-core-plugin-loader-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::test_contract
)

target_compile_features(glibre-core-plugin-loader-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-plugin-loader-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions and -fno-rtti are provided via glibre::compile_contract.
target_compile_options(glibre-core-plugin-loader-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # -Wno-c2y-extensions is inherited from glibre::test_contract (cmake/Compile.cmake).
)

# ---------------------------------------------------------------------------
# Compile definitions — inject dylib paths at build time so tests can locate
# the plugin dylibs without hardcoding absolute paths.
# ---------------------------------------------------------------------------

# GLIBRE_NOOP_DYLIB_PATH — path to the reference noop plugin.
# Guards: tests that depend on this path skip via SKIP() if it is absent.
if(TARGET glibre-plugin-noop)
    target_compile_definitions(glibre-core-plugin-loader-tests PRIVATE
        GLIBRE_NOOP_DYLIB_PATH="$<TARGET_FILE:glibre-plugin-noop>"
    )
    add_dependencies(glibre-core-plugin-loader-tests glibre-plugin-noop)
endif()

# GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH — path to the stub dylib without plugin symbols.
target_compile_definitions(glibre-core-plugin-loader-tests PRIVATE
    GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH="$<TARGET_FILE:glibre-plugin-stub-no-symbols>"
)
add_dependencies(glibre-core-plugin-loader-tests glibre-plugin-stub-no-symbols)

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-plugin-loader-tests)
