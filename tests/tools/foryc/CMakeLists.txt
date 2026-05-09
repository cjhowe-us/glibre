cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# tests/tools/foryc — unit tests for glibre-foryc parser (plan #219) and
#                     abi_hash module (plan #222).
#
# Tests parse_string() and abi_hash functions directly; no subprocess or
# filesystem required for the parser unit tests (only
# walks_input_directory_recursively uses tmp).
# ---------------------------------------------------------------------------

find_package(Catch2 3 CONFIG REQUIRED)
find_package(blake3 CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# foryc-parser-tests — parser IR unit tests (plan #219)
# ---------------------------------------------------------------------------
add_executable(foryc-parser-tests
    foryc_parser_test.cpp
    # Pull parser source directly into the test binary so we can unit-test
    # without a shared library boundary.
    ${CMAKE_SOURCE_DIR}/tools/foryc/src/parser.cpp
)

target_include_directories(foryc-parser-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/foryc/src
)

target_link_libraries(foryc-parser-tests
    PRIVATE
        Catch2::Catch2WithMain
        glibre::core   # glibre::Error + EASTL
)

target_compile_features(foryc-parser-tests PRIVATE cxx_std_23)

set_target_properties(foryc-parser-tests PROPERTIES CXX_MODULE_STD OFF)

target_compile_options(foryc-parser-tests PRIVATE
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions
)

# ---------------------------------------------------------------------------
# foryc-abi-hash-tests — abi_hash + source_hash unit tests (plan #222)
# ---------------------------------------------------------------------------
add_executable(foryc-abi-hash-tests
    foryc_abi_hash_test.cpp
    # Pull parser + abi_hash sources directly so we can unit-test without
    # a shared library boundary.
    ${CMAKE_SOURCE_DIR}/tools/foryc/src/parser.cpp
    ${CMAKE_SOURCE_DIR}/tools/foryc/src/abi_hash.cpp
)

target_include_directories(foryc-abi-hash-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/foryc/src
)

target_link_libraries(foryc-abi-hash-tests
    PRIVATE
        Catch2::Catch2WithMain
        glibre::core        # glibre::Error + EASTL
        BLAKE3::blake3      # blake3_hasher_* C API
)

target_compile_features(foryc-abi-hash-tests PRIVATE cxx_std_23)

set_target_properties(foryc-abi-hash-tests PROPERTIES CXX_MODULE_STD OFF)

target_compile_options(foryc-abi-hash-tests PRIVATE
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions
)

# ---------------------------------------------------------------------------
# Test discovery
# ---------------------------------------------------------------------------
include(CTest)
include(Catch)
catch_discover_tests(foryc-parser-tests)
catch_discover_tests(foryc-abi-hash-tests)
