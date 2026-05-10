cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/plugin_loader_register — unit tests for loader steps 9–11
#
# Plan: #231 — PluginLoaderRegistry register + schedule rebuild + migrate.
# Plan: #982 — SystemScheduleCycle integration test (step 10 failure path).
# Plan: #983 — SchemaMigrationFailed integration test (step 11 failure path).
# Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" steps 9–11.
#
# Named test cases:
#   - register_invokes_plugin_entry_point   (step 9: happy path)
#   - register_failure_cleans_up_dlopen     (step 9: failure path)
#   - rebuild_schedule_smoke                (step 10: MVP stub smoke)
#   - rebuild_schedule_returns_cycle_on_conflicting_systems (step 10: #982)
#   - migrate_components_no_op_when_versions_equal (step 11: MVP stub)
#   - migrate_components_returns_failed_on_broken_migration_step (step 11: #983)
#
# Stub dylibs:
#   glibre-plugin-stub-register-fails — valid .dylib whose
#     glibre_plugin_register returns std::unexpected(PluginInitFailed).
#     Source: stub_register_fails.cpp (this directory).
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# glibre-plugin-stub-register-fails — stub that returns failure from register.
#
# Exports all four required C symbols; glibre_plugin_register always returns
# std::unexpected(core::Error::PluginInitFailed).
#
# Does NOT link glibre-core (plugin-abi.md §"Plugin file shape" rule 2).
# Includes glibre-core headers for the PluginContext type in the register
# signature and for glibre::Error (needed for the unexpected return).
# ---------------------------------------------------------------------------

add_library(glibre-plugin-stub-register-fails MODULE
    stub_register_fails.cpp
)

target_include_directories(glibre-plugin-stub-register-fails
    PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
)

target_compile_features(glibre-plugin-stub-register-fails PRIVATE cxx_std_23)

target_link_libraries(glibre-plugin-stub-register-fails PRIVATE glibre::compile_contract)

set_target_properties(glibre-plugin-stub-register-fails PROPERTIES
    SUFFIX ".dylib"
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    CXX_MODULE_STD OFF
)

target_compile_options(glibre-plugin-stub-register-fails PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions
)

# ---------------------------------------------------------------------------
# glibre-core-plugin-loader-register-tests — Catch2 test binary
# ---------------------------------------------------------------------------

add_executable(glibre-core-plugin-loader-register-tests
    plugin_loader_register_test.cpp
)

target_link_libraries(glibre-core-plugin-loader-register-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::compile_contract
)

target_compile_features(glibre-core-plugin-loader-register-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-plugin-loader-register-tests PROPERTIES
    CXX_MODULE_STD OFF
)

# -fno-exceptions and -fno-rtti are provided via glibre::compile_contract.
# Catch2 3.x supports -fno-exceptions builds.
target_compile_options(glibre-core-plugin-loader-register-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # Catch2 3.x macros use __COUNTER__ which triggers -Wpedantic C2y
    # extension warning with clang >= 22; suppress from third-party macro.
    -Wno-c2y-extensions
)

# ---------------------------------------------------------------------------
# Compile definitions — inject dylib paths so tests can locate plugin dylibs
# without hardcoding absolute paths.
# ---------------------------------------------------------------------------

# GLIBRE_NOOP_DYLIB_PATH — noop reference plugin (all-zeros hash, success register).
# Used by: register_invokes_plugin_entry_point.
# Guard: test FAIL()s (not SKIP()s) when absent — it is part of the DoD for #231.
if(TARGET glibre-plugin-noop)
    target_compile_definitions(glibre-core-plugin-loader-register-tests PRIVATE
        GLIBRE_NOOP_DYLIB_PATH="$<TARGET_FILE:glibre-plugin-noop>"
    )
    add_dependencies(glibre-core-plugin-loader-register-tests glibre-plugin-noop)
endif()

# GLIBRE_STUB_REGISTER_FAILS_DYLIB_PATH — stub whose register() returns failure.
# Used by: register_failure_cleans_up_dlopen.
# Unconditional: this target is defined in this CMakeLists.txt; if it is missing
# the configure step will error earlier.
target_compile_definitions(glibre-core-plugin-loader-register-tests PRIVATE
    GLIBRE_STUB_REGISTER_FAILS_DYLIB_PATH="$<TARGET_FILE:glibre-plugin-stub-register-fails>"
)
add_dependencies(glibre-core-plugin-loader-register-tests glibre-plugin-stub-register-fails)

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-plugin-loader-register-tests)
