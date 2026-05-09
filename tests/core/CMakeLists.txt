cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# tests/core — unit tests for glibre::core
# ---------------------------------------------------------------------------

add_subdirectory(plugin_api)
add_subdirectory(plugin_loader)

add_executable(glibre-core-tests
    frame_loop_test.cpp
    plugin_manifest/plugin_manifest_test.cpp
)

target_link_libraries(glibre-core-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
)

target_compile_features(glibre-core-tests PRIVATE cxx_std_23)

# Same as glibre-core: no `import std;` in test files.
set_target_properties(glibre-core-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
# Catch2 3.x supports -fno-exceptions builds when CATCH_CONFIG_DISABLE_EXCEPTIONS
# is defined (see Catch2 docs §"Compile-time configuration").
# LOW-9 review finding: Catch2 internals are compatible with -fno-exceptions when
# REQUIRE_THROWS is not used; glibre-core-tests never uses REQUIRE_THROWS.
target_compile_options(glibre-core-tests PRIVATE
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # Catch2 3.x macros use __COUNTER__ which triggers -Wpedantic C2y
    # extension warning with clang >= 22; suppress from third-party macro.
    -Wno-c2y-extensions
)

# Enable test-only recording hooks in FrameLoop (MED-6 fix).
# The GLIBRE_TESTING macro activates last_tick_phase_ordinals_ tracking
# in FrameLoop so order-verification tests can observe execution order.
target_compile_definitions(glibre-core-tests PRIVATE GLIBRE_TESTING)

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-tests)
