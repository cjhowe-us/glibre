cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/hot_reload — unit tests for hot_reload_validate() and
#                          HotReloadRequestQueue / Phase 8 fast-path.
#
# Plan: #250 — feat(core): hot-reload plugin manifest + ABI hash gate.
# Plan: #249 — feat(core): pending-reload counter + phase 8 fast-path no-op.
# Authority: core/include/glibre/core/plugin_loader_actions.hpp,
#            core/include/glibre/core/hot_reload_request.hpp,
#            reviews/decisions/hot-reload-protocol.md §"Step 2 — Swap",
#            reviews/decisions/hot-reload-protocol.md §Decision (step 1),
#            reviews/decisions/frame-phases.md open question 1 resolution.
#
# Named test cases (plan #250):
#   - hot_reload_rejects_abi_hash_mismatch
#   - hot_reload_accepts_compatible_swap
#   - hot_reload_rejects_name_mismatch
#   - hot_reload_rejects_major_version_change
#
# Named test cases (plan #249):
#   - phase_8_is_no_op_when_no_pending_reloads
#   - enqueue_increments_pending_count
# ---------------------------------------------------------------------------

add_executable(glibre-core-hot-reload-tests
    hot_reload_validate_test.cpp
    hot_reload_request_test.cpp
)

target_link_libraries(glibre-core-hot-reload-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::compile_contract
)

target_compile_features(glibre-core-hot-reload-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-hot-reload-tests PROPERTIES CXX_MODULE_STD OFF)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
# Catch2 3.x supports -fno-exceptions when REQUIRE_THROWS is not used;
# glibre-core-hot-reload-tests never uses REQUIRE_THROWS.
target_compile_options(glibre-core-hot-reload-tests PRIVATE
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
catch_discover_tests(glibre-core-hot-reload-tests)
