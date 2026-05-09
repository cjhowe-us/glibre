cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/per_context_allocator — unit tests for glibre::PerContextAllocator
#
# Plan: #238 — feat(core): PerContextAllocator with per-tag heap ceiling
# Authority: core/include/glibre/alloc.hpp,
#            reviews/decisions/perf-budget.md §Allocator Rules #1-3
#
# Two test targets:
#   glibre-core-per-context-allocator-tests
#       Compiled with GLIBRE_ALLOC_STRICT=1.  Verifies strict-mode ceiling
#       enforcement (Rule #2): OutOfBudget on overrun, counter accuracy,
#       dealloc, alignment, thread safety.
#
#   glibre-core-per-context-allocator-shipping-tests  (round-2 MED-5)
#       Compiled with GLIBRE_ALLOC_STRICT=0.  Verifies the shipping-mode
#       soft-warn path (Rule #3): overrun returns valid ptr, bytes_used()
#       reflects above-ceiling, warn emitted exactly once per instance.
#
# Note: register_allocator() observability test (MED-7) is deferred.
# See alloc.hpp §register_allocator observability and follow-up plan
# [PLAN] test(core): allocator registry observability (iterate #238).
# ---------------------------------------------------------------------------

# ---- strict-mode suite (GLIBRE_ALLOC_STRICT=1) ----------------------------

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

# ---- shipping-mode suite (GLIBRE_ALLOC_STRICT=0) --------------------------
# Round-2 review MED-5: verifies Rule #3 (soft-warn path).

add_executable(glibre-core-per-context-allocator-shipping-tests
    per_context_allocator_shipping_test.cpp
)

target_link_libraries(glibre-core-per-context-allocator-shipping-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
)

target_compile_features(glibre-core-per-context-allocator-shipping-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-per-context-allocator-shipping-tests PROPERTIES CXX_MODULE_STD OFF)

# Force GLIBRE_ALLOC_STRICT=0 regardless of CMAKE_BUILD_TYPE so the shipping
# code path is exercised even in a Debug cmake configuration.
target_compile_definitions(glibre-core-per-context-allocator-shipping-tests PRIVATE
    GLIBRE_ALLOC_STRICT=0
)

target_compile_options(glibre-core-per-context-allocator-shipping-tests PRIVATE
    -fno-exceptions
    -fno-rtti
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions
)

catch_discover_tests(glibre-core-per-context-allocator-shipping-tests)
