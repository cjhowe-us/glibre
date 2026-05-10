cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# tests/tools/foryc — unit tests for glibre-foryc (plans #219, #220, #221, #225)
#
# foryc-parser-tests:           parser IR tests (plan #219)
# foryc-emit-header-tests:      header emission tests (plan #220)
# foryc-emit-migration-tests:   migration dispatcher emission tests (plan #221)
# foryc-emit-manifest-tests:    manifest emission tests (plan #225)
#
# All test binaries pull tool sources directly so tests run without a
# shared-library boundary.
# ---------------------------------------------------------------------------

find_package(Catch2 3 CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# foryc-parser-tests — plan #219 parser unit tests
# ---------------------------------------------------------------------------

add_executable(foryc-parser-tests
    foryc_parser_test.cpp
    # Pull parser source directly into the test binary.
    ${CMAKE_SOURCE_DIR}/tools/foryc/src/parser.cpp
)

target_include_directories(foryc-parser-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/foryc/src
)

target_link_libraries(foryc-parser-tests
    PRIVATE
        Catch2::Catch2WithMain
        glibre::core   # glibre::Error (std::variant-based; eastl-removal.md §4)
        glibre::compile_contract
)

target_compile_features(foryc-parser-tests PRIVATE cxx_std_23)

set_target_properties(foryc-parser-tests PROPERTIES CXX_MODULE_STD OFF)

target_compile_options(foryc-parser-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions
)

# ---------------------------------------------------------------------------
# foryc-emit-header-tests — plan #220 header emission unit tests
# ---------------------------------------------------------------------------

add_executable(foryc-emit-header-tests
    foryc_emit_header_test.cpp
    # Pull both parser and emit_header sources directly into the test binary.
    ${CMAKE_SOURCE_DIR}/tools/foryc/src/parser.cpp
    ${CMAKE_SOURCE_DIR}/tools/foryc/src/emit_header.cpp
)

target_include_directories(foryc-emit-header-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/foryc/src
)

target_link_libraries(foryc-emit-header-tests
    PRIVATE
        Catch2::Catch2WithMain
        glibre::core   # glibre::Error (std::variant-based; eastl-removal.md §4)
        glibre::compile_contract
)

target_compile_features(foryc-emit-header-tests PRIVATE cxx_std_23)

set_target_properties(foryc-emit-header-tests PROPERTIES CXX_MODULE_STD OFF)

target_compile_options(foryc-emit-header-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions
)

# ---------------------------------------------------------------------------
# foryc-emit-migration-tests — plan #221 migration dispatcher emission tests
# ---------------------------------------------------------------------------

add_executable(foryc-emit-migration-tests
    foryc_emit_migration_test.cpp
    # Pull parser and emit_migration sources directly into the test binary.
    ${CMAKE_SOURCE_DIR}/tools/foryc/src/parser.cpp
    ${CMAKE_SOURCE_DIR}/tools/foryc/src/emit_migration.cpp
)

target_include_directories(foryc-emit-migration-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/foryc/src
)

target_link_libraries(foryc-emit-migration-tests
    PRIVATE
        Catch2::Catch2WithMain
        glibre::core   # glibre::Error (std::variant-based; eastl-removal.md §4)
        glibre::compile_contract
)

# Pass include paths to the round-trip compile test so it can supply
# -I flags when invoking clang++ as a subprocess.
# GLIBRE_CORE_INCLUDE_DIR: path to core/include (provides glibre/error.hpp).
# glibre/error.hpp uses only libc++ headers (std::variant, std::expected, etc.) —
# no EASTL dependency since eastl-removal.md §4 migration (plan #1060 / PR #1086).
target_compile_definitions(foryc-emit-migration-tests
    PRIVATE
        GLIBRE_CORE_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/core/include"
)

target_compile_features(foryc-emit-migration-tests PRIVATE cxx_std_23)

set_target_properties(foryc-emit-migration-tests PROPERTIES CXX_MODULE_STD OFF)

target_compile_options(foryc-emit-migration-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions
)

# ---------------------------------------------------------------------------
# foryc-emit-manifest-tests — plan #225 manifest emission unit tests
# ---------------------------------------------------------------------------

add_executable(foryc-emit-manifest-tests
    foryc_emit_manifest_test.cpp
    # Pull parser and emit_manifest sources directly into the test binary.
    ${CMAKE_SOURCE_DIR}/tools/foryc/src/parser.cpp
    ${CMAKE_SOURCE_DIR}/tools/foryc/src/emit_manifest.cpp
)

target_include_directories(foryc-emit-manifest-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/foryc/src
)

target_link_libraries(foryc-emit-manifest-tests
    PRIVATE
        Catch2::Catch2WithMain
        glibre::core   # glibre::Error (std::variant-based; eastl-removal.md §4)
        glibre::compile_contract
)

target_compile_features(foryc-emit-manifest-tests PRIVATE cxx_std_23)

set_target_properties(foryc-emit-manifest-tests PROPERTIES CXX_MODULE_STD OFF)

target_compile_options(foryc-emit-manifest-tests PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions
)

include(CTest)
include(Catch)
catch_discover_tests(foryc-parser-tests)
catch_discover_tests(foryc-emit-header-tests)
catch_discover_tests(foryc-emit-migration-tests)
catch_discover_tests(foryc-emit-manifest-tests)

# Golden-output harness (plan #226)
add_subdirectory(golden)
