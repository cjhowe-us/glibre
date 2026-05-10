cmake_minimum_required(VERSION 3.30)

# ---------------------------------------------------------------------------
# tests/tools/foryc/golden — golden-output harness for glibre-foryc
#
# PURPOSE:
#   Runs glibre-foryc emission passes over checked-in .fory fixtures and
#   compares the output byte-for-byte against checked-in .golden files.
#   Regressions in codegen output are caught deterministically.
#
# TARGETS:
#   foryc-golden-tests          — emit_header golden harness (plan #226)
#   foryc-migration-golden-tests — emit_migration golden harness (plan #227)
#                                  Built only when emit_migration.{hpp,cpp}
#                                  are present (requires plan #221 / PR #975).
#
# REGENERATING GOLDENS:
#   Set GLIBRE_UPDATE_GOLDEN=1 before running ctest to overwrite the checked-in
#   golden files with the current actual output:
#
#     # emit_header goldens
#     GLIBRE_UPDATE_GOLDEN=1 ctest --preset macos-debug -R foryc_golden_
#
#     # emit_migration goldens (after #221 merges)
#     GLIBRE_UPDATE_GOLDEN=1 ctest --preset macos-debug -R foryc_migration_golden_
#
#   Review the diff with `git diff tests/tools/foryc/golden/` before
#   committing.  Bumping goldens intentionally is a one-line PR titled
#   "test(data): refresh codegen goldens for <reason>".
#
# LAYOUT:
#   golden/fixtures/           — input .fory schemas for emit_header
#   golden/expected/           — checked-in .hpp.golden outputs
#   golden/fixtures_migration/ — input .fory schemas for emit_migration
#   golden/expected_migration/ — checked-in .migrations.cpp.golden outputs
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
        glibre::core   # glibre::Error (std::variant-based; eastl-removal.md §4)
        glibre::compile_contract
)

target_compile_features(foryc-golden-tests PRIVATE cxx_std_23)

set_target_properties(foryc-golden-tests PROPERTIES CXX_MODULE_STD OFF)

target_compile_options(foryc-golden-tests PRIVATE
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

# ---------------------------------------------------------------------------
# foryc-migration-golden-tests — migration dispatcher golden harness (plan #227)
#
# Guarded by an if(EXISTS ...) check so that the migration golden target
# compiles only after plan #221 (emit_migration) merges and adds
# emit_migration.{hpp,cpp} to tools/foryc/src/.
#
# When #221 has not yet merged:
#   cmake --preset macos-debug silently skips this target.
#   foryc_migration_golden_v1_to_v2_matches_expected will not appear in ctest.
#
# When #221 has merged (emit_migration.hpp present):
#   cmake --preset macos-debug builds foryc-migration-golden-tests.
#   ctest -R foryc_migration_golden_ discovers and runs the test.
#   GLIBRE_UPDATE_GOLDEN=1 regenerates expected_migration/*.migrations.cpp.golden.
# ---------------------------------------------------------------------------

set(_EMIT_MIGRATION_HPP "${CMAKE_SOURCE_DIR}/tools/foryc/src/emit_migration.hpp")
set(_EMIT_MIGRATION_CPP "${CMAKE_SOURCE_DIR}/tools/foryc/src/emit_migration.cpp")

# Tell CMake to re-run configure automatically when emit_migration.hpp appears
# (i.e., when PR #975 merges).  Without this, local devs must manually invoke
# `cmake --preset macos-debug` again after #975 lands to pick up the new target.
# CI is unaffected (it always does a fresh configure), but the declare-once
# property removes the reconfigure step for local workflows.
set_property(DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_EMIT_MIGRATION_HPP}"
    "${_EMIT_MIGRATION_CPP}"
)

if(EXISTS "${_EMIT_MIGRATION_HPP}" AND EXISTS "${_EMIT_MIGRATION_CPP}")
    add_executable(foryc-migration-golden-tests
        golden_migration_test.cpp
        # Pull parser and emit_migration sources directly — no shared-library
        # boundary; mirrors the foryc-golden-tests pattern.
        ${CMAKE_SOURCE_DIR}/tools/foryc/src/parser.cpp
        ${_EMIT_MIGRATION_CPP}
    )

    target_include_directories(foryc-migration-golden-tests
        PRIVATE
            ${CMAKE_SOURCE_DIR}/tools/foryc/src
    )

    target_link_libraries(foryc-migration-golden-tests
        PRIVATE
            Catch2::Catch2WithMain
            glibre::core   # glibre::Error (std::variant-based; eastl-removal.md §4)
            glibre::compile_contract
    )

    target_compile_features(foryc-migration-golden-tests PRIVATE cxx_std_23)

    set_target_properties(foryc-migration-golden-tests PROPERTIES CXX_MODULE_STD OFF)

    # -fno-exceptions and -fno-rtti are provided via glibre::compile_contract.
    target_compile_options(foryc-migration-golden-tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
        -Wno-c2y-extensions
    )

    # Inject the absolute path to this golden/ directory so the binary can
    # locate fixtures_migration/ and expected_migration/ regardless of CWD.
    target_compile_definitions(foryc-migration-golden-tests
        PRIVATE
            GLIBRE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )

    catch_discover_tests(foryc-migration-golden-tests)
else()
    message(STATUS
        "foryc-migration-golden-tests: SKIPPED "
        "(emit_migration.{hpp,cpp} not yet present — requires plan #221 / PR #975)"
    )
endif()
