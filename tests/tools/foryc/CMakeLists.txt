cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# tests/tools/foryc — unit tests for glibre-foryc parser (plan #219)
#
# Tests parse_string() directly; no subprocess or filesystem required for
# the parser unit tests (only walks_input_directory_recursively uses tmp).
# ---------------------------------------------------------------------------

find_package(Catch2 3 CONFIG REQUIRED)

# Fory::fory is NOT linked here yet — foryc-parser-tests exercises the
# parser IR only. Fory reflection/serializer APIs will be required when
# codegen emission lands (plans #220–#222).
# Add back: find_package(Fory CONFIG REQUIRED) + Fory::fory in
# target_link_libraries once those plans are implemented.

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

include(CTest)
include(Catch)
catch_discover_tests(foryc-parser-tests)
