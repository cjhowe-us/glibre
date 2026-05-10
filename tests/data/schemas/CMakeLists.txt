cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# tests/data/schemas — runtime schema migration tests (plan #977)
#
# Tests the migration dispatcher tables produced by emit_migration()
# (glibre-foryc, plan #221) against fixture schemas.  Each test case:
#   1. Parses a fixture schema via parse_string().
#   2. Emits a C++ migration dispatcher TU via emit_migration().
#   3. Compiles the TU + a stub provider body into a .dylib via clang++.
#   4. dlopen()s the dylib and dlsym()s the per-type migration table symbols.
#   5. Walks the migration chain using a test-local chain_walk() helper that
#      mirrors the logic described in fory-codegen.md §"Migration Mechanic" 3.
#
# Named tests (plan #977 Unit Test Plan):
#   - migration_v1_to_v3_chain_succeeds
#   - migration_byte_round_trip_is_stable
#   - missing_chain_yields_schema_migration_failure
#   - provider_io_or_alloc_is_caught_by_sanitizer  (hidden "[.]" — sanitizer
#     infra not yet plumbed; see test body for deferral rationale)
#
# Compile definitions injected (same pattern as tests/tools/foryc/CMakeLists.txt):
#   GLIBRE_CORE_INCLUDE_DIR — path to core/include (provides glibre/error.hpp)
#   GLIBRE_VCPKG_INCLUDE_DIR — vcpkg installed include dir (provides EASTL headers)
# ---------------------------------------------------------------------------

# Only run on the host (macOS); skip iOS builds where Catch2 is not installed
# and where subprocess-based compile + dlopen tests cannot run.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    return()
endif()

find_package(Catch2 3 CONFIG REQUIRED)
find_package(EASTL CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# schema-migration-tests binary
#
# Pulls foryc parser + emit_migration sources directly so there is no
# shared-library boundary and the test binary is self-contained.
# Same link pattern as foryc-emit-migration-tests in tests/tools/foryc/.
# ---------------------------------------------------------------------------

add_executable(schema-migration-tests
    schema_migration_test.cpp
    # Pull parser and emit_migration sources directly into the test binary.
    ${CMAKE_SOURCE_DIR}/tools/foryc/src/parser.cpp
    ${CMAKE_SOURCE_DIR}/tools/foryc/src/emit_migration.cpp
)

target_include_directories(schema-migration-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/foryc/src
)

target_link_libraries(schema-migration-tests
    PRIVATE
        Catch2::Catch2WithMain
        glibre::core   # glibre::Error + EASTL
        glibre::compile_contract
)

# Inject include paths for the subprocess clang++ calls inside the tests.
# The tests compile generated TUs that include "glibre/error.hpp" and EASTL
# headers; these paths are passed via -I flags inside the test binary.
get_target_property(_eastl_inc EASTL INTERFACE_INCLUDE_DIRECTORIES)
target_compile_definitions(schema-migration-tests
    PRIVATE
        GLIBRE_CORE_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/core/include"
        GLIBRE_VCPKG_INCLUDE_DIR="${_eastl_inc}"
)

target_compile_features(schema-migration-tests PRIVATE cxx_std_23)

# CXX_MODULE_STD OFF: same reason as all other test targets — Catch2 +
# clang on macOS are incompatible with std module import (duplicate symbols).
set_target_properties(schema-migration-tests PROPERTIES CXX_MODULE_STD OFF)

target_compile_options(schema-migration-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions   # Catch2 uses __COUNTER__; suppress clang pedantic warning
)

include(CTest)
include(Catch)
# Discover all tests.  The "[.]" hidden tag on the sanitizer test means it
# does not appear in the default run but IS discovered; CI must not rely on
# its pass/fail status until the sanitizer preset is plumbed.
catch_discover_tests(schema-migration-tests)
