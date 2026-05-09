cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/plugin_loader_registry — unit tests for PluginLoaderRegistry
#
# Plan: #230 — PluginLoader ABI hash + version + name + deps gates.
# Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" steps 4–7.
#
# Named test cases (plan #230 Unit Test Plan + DoD):
#   - plugin_loader_rejects_abi_hash_mismatch
#   - plugin_loader_rejects_unknown_dependency
#   - plugin_loader_accepts_compatible_plugin
#   - plugin_loader_rejects_duplicate_name
#   - plugin_loader_rejects_incompatible_version
# ---------------------------------------------------------------------------

add_executable(glibre-core-plugin-loader-registry-tests
    plugin_loader_registry_test.cpp
)

target_link_libraries(glibre-core-plugin-loader-registry-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::compile_contract
)

target_compile_features(glibre-core-plugin-loader-registry-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-plugin-loader-registry-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
# Catch2 3.x supports -fno-exceptions builds when CATCH_CONFIG_DISABLE_EXCEPTIONS
# is defined.  These tests do not use REQUIRE_THROWS.
target_compile_options(glibre-core-plugin-loader-registry-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # Catch2 3.x macros use __COUNTER__ which triggers -Wpedantic C2y
    # extension warning with clang >= 22; suppress from third-party macro.
    -Wno-c2y-extensions
)

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-plugin-loader-registry-tests)
