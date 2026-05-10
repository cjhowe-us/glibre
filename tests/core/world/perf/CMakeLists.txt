cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/world/perf — macOS PMC sampler unit tests
#
# Authority: plan #944.
#
# Named test cases (plan #944 Unit Test Plan / DoD):
#   - world/perf: pmc_sampler_smoke_returns_struct_with_zero_stub_counters
#
# Build flag:
#   GLIBRE_ENABLE_PMC (default ON) — gates the PmcSampler TU.
#   When OFF, pmc_sampler.cpp is still compiled but start_sampling() /
#   stop_sampling() are zero-stubs unconditionally.  The test still
#   exercises the interface and passes.
#
# Platform:
#   macOS-only (enforced by root CMakeLists.txt); the pmc_sampler.cpp TU
#   compiles on non-Apple platforms via its #else stub branch, but the CI
#   runner is macos-26 per ci.yml.
#
# Artifact output:
#   tests/core/world/perf/_artifacts/l1d_miss.json is produced by the
#   alarm plan (#938) that consumes this helper.  The ci.yml artifact-upload
#   step points at this path; the file is not created by this test (only the
#   infrastructure is wired up here).
# ---------------------------------------------------------------------------

option(GLIBRE_ENABLE_PMC "Enable macOS PMC (kperf/KPC) hardware counter sampling"
       ON)

add_executable(glibre-core-world-perf-tests
    pmc_sampler_test.cpp
    pmc_sampler.cpp
)

target_link_libraries(glibre-core-world-perf-tests
    PRIVATE
        Catch2::Catch2WithMain
)

target_compile_features(glibre-core-world-perf-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-world-perf-tests PROPERTIES CXX_MODULE_STD OFF)

# Expose the directory containing pmc_sampler.hpp to the test TU.
target_include_directories(glibre-core-world-perf-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

# GLIBRE_ENABLE_PMC compile-time gate.
if(GLIBRE_ENABLE_PMC)
    target_compile_definitions(glibre-core-world-perf-tests PRIVATE
        GLIBRE_ENABLE_PMC=1
    )
else()
    target_compile_definitions(glibre-core-world-perf-tests PRIVATE
        GLIBRE_ENABLE_PMC=0
    )
endif()

target_compile_options(glibre-core-world-perf-tests
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        # Catch2 TEST_CASE uses __COUNTER__ which clang ≥21 flags as a C2y
        # extension under -Wpedantic.  Suppress until Catch2 ships a fix or
        # until we switch to a C2y language standard that makes it non-pedantic.
        -Wno-c2y-extensions
)

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-world-perf-tests)
