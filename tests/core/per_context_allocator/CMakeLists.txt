cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/per_context_allocator — unit tests for glibre::PerContextAllocator
#
# Plan: #238 — feat(core): PerContextAllocator with per-tag heap ceiling
# Authority: core/include/glibre/alloc.hpp,
#            reviews/decisions/perf-budget.md §Allocator Rules #1-3
#
# Named test cases (plan #238 Unit Test Plan + DoD):
#   - per_context_allocator_tracks_bytes_per_tag
#   - per_context_allocator_rejects_alloc_over_ceiling
#   - per_context_allocator_release_decrements_counter
#   - per_context_allocator_alignment_respected
#   - per_context_allocator_threadsafe_allocations
# ---------------------------------------------------------------------------

add_executable(glibre-core-per-context-allocator-tests
    per_context_allocator_test.cpp
)

target_link_libraries(glibre-core-per-context-allocator-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
)

target_compile_features(glibre-core-per-context-allocator-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-per-context-allocator-tests PROPERTIES CXX_MODULE_STD OFF)

# Enable strict-mode ceiling enforcement for all tests in this suite.
# perf-budget.md §Allocator Rules #2: "hard ceiling in diagnostic/debug builds
# when GLIBRE_ALLOC_STRICT=1".  Unit tests exercise the strict-mode path so
# that the ceiling contract is verified at CI time.
target_compile_definitions(glibre-core-per-context-allocator-tests PRIVATE
    GLIBRE_ALLOC_STRICT=1
)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
# Catch2 3.x supports -fno-exceptions when REQUIRE_THROWS is not used;
# this test file uses REQUIRE / CHECK only.
target_compile_options(glibre-core-per-context-allocator-tests PRIVATE
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
catch_discover_tests(glibre-core-per-context-allocator-tests)
