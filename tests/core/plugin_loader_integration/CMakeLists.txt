cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/plugin_loader_integration — integration tests for PluginLoader
# + PluginLoaderRegistry end-to-end against real .dylib plugins.
#
# Plan: #232 — plugin loader integration — full failure-mode coverage.
# Plan: #968 — plugin loader integration — manifest_invalid / engine_too_old /
#              dependency_missing failure modes.
# Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" steps 1–7,
#            §"Failure Modes → core::Error" table.
#
# Named test cases:
#   - integration_dlopen_failure_propagates          (step 1: dlopen failure)
#   - integration_missing_symbol_propagates          (step 2: missing symbols)
#   - integration_abi_hash_mismatch_propagates       (step 4: hash mismatch)
#   - integration_name_collision_propagates          (step 6: duplicate name)
#   - integration_happy_path_loads_and_validates     (success path)
#   - integration_manifest_invalid_propagates        (step 3: manifest deser fails)
#   - integration_engine_too_old_propagates          (step 5: engine too old)
#   - integration_dependency_missing_propagates      (step 7: dep not registered)
#
# Stub dylibs:
#   glibre-plugin-stub-no-symbols       — valid .dylib with NO required symbols
#     (reused from tests/core/plugin_loader, plan #229).
#   glibre-plugin-stub-wrong-abi        — valid .dylib with wrong abi_hash
#     (new for plan #232; source: stub_wrong_abi_hash.cpp).
#   glibre-plugin-stub-invalid-manifest — valid .dylib with garbage manifest
#     blob; paired with a .manifest sidecar so PluginManifest::open returns
#     PluginManifestInvalid (new for plan #968; source: stub_invalid_manifest.cpp).
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# glibre-plugin-stub-wrong-abi — exports all four required symbols but uses a
# deliberately wrong ABI hash value ("ffff...ffff").
#
# Purpose: trigger PluginAbiHashMismatch at gate-1b (validate_symbol_abi_hash)
# and gate-1a (validate_manifest_abi_hash) in integration tests.
#
# Does NOT link glibre-core (plugin-abi.md §"Plugin file shape" rule 2).
# Only needs glibre-core include paths for the PluginContext type reference
# used in the glibre_plugin_register signature.
# ---------------------------------------------------------------------------

add_library(glibre-plugin-stub-wrong-abi MODULE
    stub_wrong_abi_hash.cpp
)

target_include_directories(glibre-plugin-stub-wrong-abi
    PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
)

target_compile_features(glibre-plugin-stub-wrong-abi PRIVATE cxx_std_23)

target_link_libraries(glibre-plugin-stub-wrong-abi PRIVATE glibre::compile_contract)

set_target_properties(glibre-plugin-stub-wrong-abi PROPERTIES
    SUFFIX ".dylib"
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    CXX_MODULE_STD OFF
)

target_compile_options(glibre-plugin-stub-wrong-abi PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions
)

# ---------------------------------------------------------------------------
# glibre-plugin-stub-invalid-manifest — exports all four required symbols but
# points glibre_plugin_manifest at garbage bytes so step 3 (Fory deserialize)
# fires PluginManifestInvalid.
#
# A sidecar <stub>.manifest file is created at build time so that the loader's
# sidecar-read path (plan #229 step 3 deferral) has a real file to attempt to
# deserialize — PluginManifest::open() returns PluginManifestInvalid for any
# file that exists but cannot be Fory-decoded (stub impl in
# core/src/plugin_manifest.cpp).
#
# The stub's glibre_plugin_abi_hash uses the same all-zero sentinel as the
# noop plugin so the ABI hash gate (step 4) does not interfere.
# ---------------------------------------------------------------------------

add_library(glibre-plugin-stub-invalid-manifest MODULE
    stub_invalid_manifest.cpp
)

target_include_directories(glibre-plugin-stub-invalid-manifest
    PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
)

target_compile_features(glibre-plugin-stub-invalid-manifest PRIVATE cxx_std_23)

target_link_libraries(glibre-plugin-stub-invalid-manifest PRIVATE glibre::compile_contract)

set_target_properties(glibre-plugin-stub-invalid-manifest PROPERTIES
    SUFFIX ".dylib"
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    CXX_MODULE_STD OFF
)

target_compile_options(glibre-plugin-stub-invalid-manifest PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions
)

# Create a sidecar <stub>.manifest file at build time so the loader's sidecar
# read path finds a real file to attempt to parse.  Content is intentionally
# empty — PluginManifest::open() returns PluginManifestInvalid for any regular
# file whose bytes cannot be Fory-decoded (core/src/plugin_manifest.cpp stub).
#
# cmake -E touch creates an empty regular file without shell redirection
# operators, making this portable across all CMake-supported generators.
#
# The sidecar must be created AFTER the stub dylib is built (its path depends
# on TARGET_FILE), so we use add_custom_command(TARGET POST_BUILD).
add_custom_command(
    TARGET glibre-plugin-stub-invalid-manifest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E touch
        "$<TARGET_FILE:glibre-plugin-stub-invalid-manifest>.manifest"
    COMMENT "Creating invalid sidecar manifest for glibre-plugin-stub-invalid-manifest"
    VERBATIM
)

# ---------------------------------------------------------------------------
# glibre-core-plugin-loader-integration-tests — Catch2 integration test binary
# ---------------------------------------------------------------------------

add_executable(glibre-core-plugin-loader-integration-tests
    plugin_loader_integration_test.cpp
)

target_link_libraries(glibre-core-plugin-loader-integration-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::test_contract
)

target_compile_features(glibre-core-plugin-loader-integration-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-plugin-loader-integration-tests PROPERTIES
    CXX_MODULE_STD OFF
)

# -fno-exceptions and -fno-rtti are provided via glibre::compile_contract.
# Catch2 3.x supports -fno-exceptions builds when CATCH_CONFIG_DISABLE_EXCEPTIONS
# is defined.  These tests do not use REQUIRE_THROWS.
target_compile_options(glibre-core-plugin-loader-integration-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # -Wno-c2y-extensions is inherited from glibre::test_contract (cmake/Compile.cmake).
)

# ---------------------------------------------------------------------------
# Compile definitions — inject dylib paths so tests can locate plugin dylibs
# without hardcoding absolute paths.
# ---------------------------------------------------------------------------

# GLIBRE_NOOP_DYLIB_PATH — reference noop plugin (glibre-plugin-noop.dylib).
# Used by: integration_name_collision_propagates,
#          integration_happy_path_loads_and_validates.
# Guarded: tests skip gracefully when GLIBRE_BUILD_EXAMPLES=OFF.
if(TARGET glibre-plugin-noop)
    target_compile_definitions(glibre-core-plugin-loader-integration-tests PRIVATE
        GLIBRE_NOOP_DYLIB_PATH="$<TARGET_FILE:glibre-plugin-noop>"
    )
    add_dependencies(glibre-core-plugin-loader-integration-tests glibre-plugin-noop)
endif()

# GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH — stub with no plugin symbols.
# Used by: integration_missing_symbol_propagates.
# Reused from tests/core/plugin_loader (plan #229); the target
# glibre-plugin-stub-no-symbols is defined in that subdirectory's
# CMakeLists.txt which is added BEFORE this one in tests/core/CMakeLists.txt.
if(TARGET glibre-plugin-stub-no-symbols)
    target_compile_definitions(glibre-core-plugin-loader-integration-tests PRIVATE
        GLIBRE_STUB_NO_SYMBOLS_DYLIB_PATH="$<TARGET_FILE:glibre-plugin-stub-no-symbols>"
    )
    add_dependencies(glibre-core-plugin-loader-integration-tests glibre-plugin-stub-no-symbols)
endif()

# GLIBRE_STUB_WRONG_ABI_DYLIB_PATH — stub with wrong ABI hash.
# Used by: integration_abi_hash_mismatch_propagates.
target_compile_definitions(glibre-core-plugin-loader-integration-tests PRIVATE
    GLIBRE_STUB_WRONG_ABI_DYLIB_PATH="$<TARGET_FILE:glibre-plugin-stub-wrong-abi>"
)
add_dependencies(glibre-core-plugin-loader-integration-tests glibre-plugin-stub-wrong-abi)

# GLIBRE_STUB_INVALID_MANIFEST_DYLIB_PATH — stub with garbage manifest blob +
# sidecar .manifest file.
# Used by: integration_manifest_invalid_propagates.
# Injected unconditionally — glibre-plugin-stub-invalid-manifest is defined
# above and always built.
target_compile_definitions(glibre-core-plugin-loader-integration-tests PRIVATE
    GLIBRE_STUB_INVALID_MANIFEST_DYLIB_PATH="$<TARGET_FILE:glibre-plugin-stub-invalid-manifest>"
)
add_dependencies(glibre-core-plugin-loader-integration-tests glibre-plugin-stub-invalid-manifest)

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-plugin-loader-integration-tests)
