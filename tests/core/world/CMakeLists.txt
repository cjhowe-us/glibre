cmake_minimum_required(VERSION 4.3.0)

# ---------------------------------------------------------------------------
# tests/core/world — world-context unit tests
#
# Targets:
#   glibre-core-world-entity-foreign-world-refusal-tests — plan #940
#     EntityForeignWorld refusal-shape contract:
#       - world: entity_foreign_world_arm_present
#       - world: entity_outside_slot_range_yields_EntityStale_in_mvp
#
# Subdirectories:
#   perf/  — macOS PMC sampler helper + perf alarm tests (plan #944)
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# plan #940 — EntityForeignWorld refusal-shape contract
#
# Tests EntityForeignWorld arm presence in core::Error and the MVP EntityStale
# routing for entities whose slot index lies outside the allocator range.
#
# Authority: specs/core/world-design.md §10.1, §11.4;
#            reviews/decisions/error-model.md §Logging (arm_to_string contract).
#
# Named test cases:
#   - world: entity_foreign_world_arm_present
#   - world: entity_outside_slot_range_yields_EntityStale_in_mvp
#
# NOTE: EntityAllocator is an internal type (core/src/world/).  This target
# adds core/src/world as a PRIVATE include directory so the test file can
# #include "entity_allocator.hpp" directly — the same pattern used by
# tests/core/entity_allocator/.  This does NOT expose the internal header to
# other library targets or plugin targets.
# ---------------------------------------------------------------------------

add_executable(glibre-core-world-entity-foreign-world-refusal-tests
    test_entity_foreign_world_refusal.cpp
)

target_link_libraries(glibre-core-world-entity-foreign-world-refusal-tests
    PRIVATE
        glibre::core
        Catch2::Catch2WithMain
        glibre::compile_contract
)

# Add the internal source directory so the test can include entity_allocator.hpp.
# PRIVATE — only this test target sees the path.
target_include_directories(glibre-core-world-entity-foreign-world-refusal-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/core/src/world
)

target_compile_features(glibre-core-world-entity-foreign-world-refusal-tests PRIVATE cxx_std_23)

set_target_properties(glibre-core-world-entity-foreign-world-refusal-tests
    PROPERTIES CXX_MODULE_STD OFF
)

# -fno-exceptions clean per reviews/decisions/error-model.md §Decision 3.
# Catch2 3.x supports -fno-exceptions when REQUIRE_THROWS is not used;
# this test file uses REQUIRE / CHECK only.
target_compile_options(glibre-core-world-entity-foreign-world-refusal-tests
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
        # Catch2 3.x macros use __COUNTER__ which triggers -Wpedantic C2y
        # extension warning with clang >= 22; suppress from third-party macro.
        # This flag is set per-target (not in glibre::compile_contract) because
        # centralising it requires touching shared CMake infra that is out of
        # scope for plan #940.  Centralisation is tracked in issue #1101.
        -Wno-c2y-extensions
)

# GLIBRE_TESTING is propagated PUBLIC from glibre-core when GLIBRE_BUILD_TESTS=ON,
# so no PRIVATE definition is needed here.

include(CTest)
include(Catch)
catch_discover_tests(glibre-core-world-entity-foreign-world-refusal-tests)

add_subdirectory(perf)  # plan #944 — macOS PMC sampler helper
