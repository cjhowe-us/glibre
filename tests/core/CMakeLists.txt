cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# tests/core — unit tests for glibre::core
# ---------------------------------------------------------------------------

add_executable(glibre-core-tests
    frame_loop_test.cpp
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

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-tests)
