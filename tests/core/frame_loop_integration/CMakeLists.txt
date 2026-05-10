cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/frame_loop_integration — integration tests for FrameLoop
# nine-phase ordering.
#
# Plan: #248 — test(core): frame-loop ordering integration test.
# Authority: reviews/decisions/frame-phases.md §"Phase Table" and §"Consequences".
#
# Named test cases:
#   - core/frame_loop_integration: nine_phase_round_trip_orders_correctly
#   - core/frame_loop_integration: world_tick_visible_to_all_phases_in_frame
#   - core/frame_loop_integration: empty_reserved_phases_no_op
#
# Test strategy:
#   Uses two GLIBRE_TESTING-gated accessors on FrameLoop (propagated PUBLIC
#   from glibre-core when GLIBRE_BUILD_TESTS=ON) to observe per-tick state:
#     - last_tick_phase_ordinals()       : per-tick phase execution order
#     - last_tick_per_phase_world_ticks(): per-phase world_tick snapshots
#   Neither accessor requires a live PhaseRegistry or system-registration
#   API (plan #245, not yet landed).  Tests are deterministic,
#   allocation-free, and require no network or filesystem — `ctest -L unit`.
# ---------------------------------------------------------------------------

add_executable(glibre-core-frame-loop-integration-tests
    frame_loop_ordering_test.cpp
)

target_link_libraries(glibre-core-frame-loop-integration-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::compile_contract
)

target_compile_features(glibre-core-frame-loop-integration-tests PRIVATE cxx_std_23)

# GLIBRE_TESTING is propagated PUBLIC from glibre-core when GLIBRE_BUILD_TESTS
# is ON (plan #997), so this target receives it transitively via glibre::core.
# No separate PRIVATE definition is needed here.
set_target_properties(glibre-core-frame-loop-integration-tests PROPERTIES
    CXX_MODULE_STD OFF
)

# -fno-exceptions and -fno-rtti are provided via glibre::compile_contract.
# Catch2 3.x supports -fno-exceptions builds; these tests do not use
# REQUIRE_THROWS.
target_compile_options(glibre-core-frame-loop-integration-tests PRIVATE
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
catch_discover_tests(glibre-core-frame-loop-integration-tests)
