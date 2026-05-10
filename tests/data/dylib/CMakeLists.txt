cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# tests/data/dylib — glibre-types.dylib link-surface tests
#
# Verifies that a minimal consumer (simulating a future plugin) can link
# ONLY glibre-types and call the exported anchor symbol.
#
# Per reviews/decisions/plugin-abi.md §"Plugin file shape" rule 2:
#   plugins link ONLY glibre-types.dylib, not glibre-core.
# This test executable mirrors that constraint — it links glibre-types
# only, not glibre::core.
# ---------------------------------------------------------------------------

find_package(Catch2 CONFIG REQUIRED)

add_executable(glibre-types-dylib-tests
    glibre_types_link_test.cpp
)

target_link_libraries(glibre-types-dylib-tests
    PRIVATE
        glibre::types          # the middleman dylib under test
        Catch2::Catch2WithMain  # test runner
        # Intentionally no glibre::core — mirrors plugin-abi.md rule 2
)

target_compile_features(glibre-types-dylib-tests PRIVATE cxx_std_23)

# Same CXX_MODULE_STD OFF as all other test targets (Catch2 + clang-22
# duplicate-symbol issue on macOS 26; see tests/core/CMakeLists.txt note).
set_target_properties(glibre-types-dylib-tests PROPERTIES CXX_MODULE_STD OFF)

target_compile_options(glibre-types-dylib-tests PRIVATE
    # No -fno-exceptions/-fno-rtti: Catch2's REQUIRE macro throws on failure;
    # building tests without exceptions would call std::terminate instead of
    # producing structured Catch2 diagnostics.  Test executables are not part
    # of the shipping plugin link surface so these constraints do not apply.
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    # This target intentionally enables exceptions (Catch2 REQUIRE uses throws) so
    # it cannot link glibre::test_contract.  -Wno-c2y-extensions suppressed directly.
    -Wno-c2y-extensions
)

# Inject the path to libglibre-types.dylib so the nm/otool regression test
# (glibre_types_dylib_excludes_fory_and_blake3_symbols) can inspect the
# dylib's public symbol table without depending on a hard-coded build path.
# $<TARGET_FILE:glibre-types> expands to the full path of the built dylib.
target_compile_definitions(glibre-types-dylib-tests PRIVATE
    GLIBRE_TYPES_DYLIB_PATH="$<TARGET_FILE:glibre-types>"
)

include(CTest)
include(Catch)
catch_discover_tests(glibre-types-dylib-tests)
