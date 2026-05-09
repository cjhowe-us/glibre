cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/per_context_allocator — unit tests for glibre::PerContextAllocator
#
# Plan: #238 — feat(core): PerContextAllocator with per-tag heap ceiling
# Plan: #990 — test(core): allocator registry observability (iterate #238)
# Authority: core/include/glibre/alloc.hpp,
#            reviews/decisions/perf-budget.md §Allocator Rules #1-3
#
# Two test targets:
#   glibre-core-per-context-allocator-tests
#       Compiled with GLIBRE_ALLOC_STRICT=1 and GLIBRE_TESTING=1.
#       Verifies strict-mode ceiling enforcement (Rule #2): OutOfBudget on
#       overrun, counter accuracy, dealloc, alignment, thread safety, and
#       register_allocator() observability (plan #990).
#
#   glibre-core-per-context-allocator-shipping-tests  (round-2 MED-5)
#       Compiled with GLIBRE_ALLOC_STRICT=0.  Verifies the shipping-mode
#       soft-warn path (Rule #3): overrun returns valid ptr, bytes_used()
#       reflects above-ceiling, warn emitted exactly once per instance.
# ---------------------------------------------------------------------------

# ---- strict-mode suite (GLIBRE_ALLOC_STRICT=1) ----------------------------
# Covers PerContextAllocator (plan #238) and AllocatorHandle (plan #989).

add_executable(glibre-core-per-context-allocator-tests
    per_context_allocator_test.cpp
    allocator_handle_test.cpp
)

target_link_libraries(glibre-core-per-context-allocator-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::compile_contract
)

target_compile_features(glibre-core-per-context-allocator-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-per-context-allocator-tests PROPERTIES CXX_MODULE_STD OFF)

# Enable strict-mode ceiling enforcement for all tests in this suite.
# perf-budget.md §Allocator Rules #2: "hard ceiling in diagnostic/debug builds
# when GLIBRE_ALLOC_STRICT=1".  Unit tests exercise the strict-mode path so
# that the ceiling contract is verified at CI time.
#
# GLIBRE_TESTING is NOT redefined here; it propagates transitively from the
# PUBLIC target_compile_definitions on glibre-core (core/CMakeLists.txt:130-132)
# via target_link_libraries(... glibre::core) above.  Redefining it PRIVATE
# here would create a false impression that the test target is the source of
# truth for the macro and would risk drift if GLIBRE_BUILD_TESTS semantics change.
target_compile_definitions(glibre-core-per-context-allocator-tests PRIVATE
    GLIBRE_ALLOC_STRICT=1
)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
# Catch2 3.x supports -fno-exceptions when REQUIRE_THROWS is not used;
# this test file uses REQUIRE / CHECK only.
target_compile_options(glibre-core-per-context-allocator-tests PRIVATE
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
#
# glibre::core is compiled with GLIBRE_ALLOC_STRICT=1 in Debug builds (via
# PUBLIC target_compile_definitions).  To exercise the GLIBRE_ALLOC_STRICT=0
# code path in per_context_allocator.cpp we must recompile that TU directly
# in this target — we cannot simply link libglibre-core.a and override the
# flag, because the flag governs the library's own compiled code, not just
# the test's #include expansions.
#
# We therefore compile per_context_allocator.cpp directly into the test
# executable alongside the test file, and link everything else from
# glibre::core.  The single recompiled TU's GLIBRE_ALLOC_STRICT=0 definition
# overrides the library copy for that TU only.  The rest of glibre-core
# (eastl_alloc, log_error, etc.) comes from the library via link.

add_executable(glibre-core-per-context-allocator-shipping-tests
    per_context_allocator_shipping_test.cpp
    # Recompile the allocator TU with GLIBRE_ALLOC_STRICT=0 so the shipping
    # code path (soft-warn, always-return-valid-ptr) is actually exercised.
    ${CMAKE_SOURCE_DIR}/core/src/alloc/per_context_allocator.cpp
)

target_link_libraries(glibre-core-per-context-allocator-shipping-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::compile_contract
)

target_compile_features(glibre-core-per-context-allocator-shipping-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-per-context-allocator-shipping-tests PROPERTIES CXX_MODULE_STD OFF)

# Force GLIBRE_ALLOC_STRICT=0 for this target (both the test file and the
# directly-included allocator TU).  glibre::core PUBLIC-defines
# GLIBRE_ALLOC_STRICT=1 in Debug; CMake appends INTERFACE (PUBLIC) defines
# AFTER the target's own PRIVATE defines, so the last definition wins in
# the C preprocessor.  Use -U to undefine before redefining to 0.
target_compile_options(glibre-core-per-context-allocator-shipping-tests PRIVATE
    # -UGLIBRE_ALLOC_STRICT placed before all other definitions so the
    # -DGLIBRE_ALLOC_STRICT=0 redefinition below wins.
    -UGLIBRE_ALLOC_STRICT
    -DGLIBRE_ALLOC_STRICT=0
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Wno-c2y-extensions
    # Suppress any residual macro-redefined diagnostic from the undefine+redefine.
    -Wno-macro-redefined
)

catch_discover_tests(glibre-core-per-context-allocator-shipping-tests)
