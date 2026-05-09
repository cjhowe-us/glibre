cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# tests/tools/foryc/golden — golden-output harness for glibre-foryc (plan #226)
#
# PURPOSE:
#   Runs glibre-foryc's emit_header pass over checked-in .fory fixtures and
#   compares the output byte-for-byte against checked-in .golden files.
#   Regressions in codegen output are caught deterministically.
#
# REGENERATING GOLDENS:
#   Set GLIBRE_UPDATE_GOLDEN=1 before running ctest to overwrite the checked-in
#   golden files with the current actual output:
#
#     GLIBRE_UPDATE_GOLDEN=1 ctest --preset macos-debug -R foryc_golden_
#
#   Review the diff with `git diff tests/tools/foryc/golden/expected/` before
#   committing.  Bumping goldens intentionally is a one-line PR titled
#   "test(data): refresh codegen goldens for <reason>".
#
# ADDING A NEW FIXTURE:
#   1. Add <name>.fory to golden/fixtures/.
#   2. Add the basename to k_fixture_basenames[] in golden_runner_test.cpp AND
#      to the GENERATE() call in foryc_golden_emit_header_matches_expected.
#   3. Run with GLIBRE_UPDATE_GOLDEN=1 to generate the initial golden.
#   4. Review the generated golden and commit both files.
#
# LAYOUT:
#   golden/fixtures/  — input .fory schemas (small, named by feature)
#   golden/expected/  — checked-in expected outputs (*.hpp.golden)
# ---------------------------------------------------------------------------

find_package(Catch2 3 CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# foryc-golden-tests — Catch2 binary for the golden harness (plan #226)
# ---------------------------------------------------------------------------

add_executable(foryc-golden-tests
    golden_runner_test.cpp
    # Pull parser and emit_header sources directly — no shared-library boundary.
    ${CMAKE_SOURCE_DIR}/tools/foryc/src/parser.cpp
    ${CMAKE_SOURCE_DIR}/tools/foryc/src/emit_header.cpp
)

target_include_directories(foryc-golden-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/foryc/src
)

target_link_libraries(foryc-golden-tests
    PRIVATE
        Catch2::Catch2WithMain
        glibre::core   # glibre::Error + EASTL
)

target_compile_features(foryc-golden-tests PRIVATE cxx_std_23)

set_target_properties(foryc-golden-tests PROPERTIES CXX_MODULE_STD OFF)

target_compile_options(foryc-golden-tests PRIVATE
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions
)

# Inject the absolute path to this golden/ directory so the test binary can
# locate golden/fixtures/ and golden/expected/ regardless of CWD.
target_compile_definitions(foryc-golden-tests
    PRIVATE
        GLIBRE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

include(CTest)
include(Catch)
catch_discover_tests(foryc-golden-tests)
