// tests/core/perf_s1_fixture/perf_s1_fixture_test.cpp
//
// Unit tests for the canonical S1 sample-scene fixture loader.
//
// Authority: plan #1003, reviews/decisions/perf-budget.md §Justification Per Cell (S1)
//
// S1 canonical invariants (perf-budget.md §S1 scenario):
//   "sample scene = 1 character + 200 props + 8 dynamic lights at 1920x1080"
//   entity_count = 209 (1 + 200 + 8)
//
// Named test cases (plan #1003 Unit Test Plan / DoD):
//   - fixture_loads_with_expected_entity_count
//   - fixture_is_deterministic_across_runs
//   - manifest_cell_budgets_match_decision_record

#include <catch2/catch_test_macros.hpp>

#include "fixtures/s1.hpp"

// ---------------------------------------------------------------------------
// fixture_loads_with_expected_entity_count
//
// Verifies that load_s1() returns the canonical 209-entity S1 scene:
//   - entity_count == 209 (1 char + 200 props + 8 lights)
//   - archetype_count == 3 (character, prop, dynamic_light are distinct archetypes)
//   - system_count == 2 (core transform sweep + render visibility sweep)
//   - viewport == 1920x1080
// ---------------------------------------------------------------------------
TEST_CASE("fixture_loads_with_expected_entity_count", "[s1][fixture][core]") {
    const auto scene = glibre::testing::load_s1();

    REQUIRE(scene.entity_count == 209u);
    REQUIRE(scene.archetype_count == 3u);
    REQUIRE(scene.system_count == 2u);
    REQUIRE(scene.viewport.width == 1920u);
    REQUIRE(scene.viewport.height == 1080u);
}

// ---------------------------------------------------------------------------
// fixture_is_deterministic_across_runs
//
// Verifies that two consecutive calls to load_s1() in the same process
// return bit-identical S1Scene values.
//
// This tests PHILOSOPHY §7 (determinism by default): the fixture must
// produce identical output regardless of call order, timing, thread state,
// or address-space layout.  The placement_seed is read from the manifest
// (not generated from time or address), so identical seeds across calls
// is the expected outcome.
// ---------------------------------------------------------------------------
TEST_CASE("fixture_is_deterministic_across_runs", "[s1][fixture][core]") {
    const auto scene_a = glibre::testing::load_s1();
    const auto scene_b = glibre::testing::load_s1();

    // Entity composition must be identical.
    REQUIRE(scene_a.entity_count == scene_b.entity_count);
    REQUIRE(scene_a.archetype_count == scene_b.archetype_count);
    REQUIRE(scene_a.system_count == scene_b.system_count);

    // Archetype breakdown must be identical.
    REQUIRE(scene_a.archetypes.character == scene_b.archetypes.character);
    REQUIRE(scene_a.archetypes.prop == scene_b.archetypes.prop);
    REQUIRE(scene_a.archetypes.dynamic_light == scene_b.archetypes.dynamic_light);

    // Viewport must be identical.
    REQUIRE(scene_a.viewport.width == scene_b.viewport.width);
    REQUIRE(scene_a.viewport.height == scene_b.viewport.height);

    // Placement seed must be identical (deterministic cross-host reproducibility).
    REQUIRE(scene_a.placement_seed == scene_b.placement_seed);

    // Budget cells must be identical.
    REQUIRE(scene_a.budgets.core_cpu_sim_ns == scene_b.budgets.core_cpu_sim_ns);
    REQUIRE(scene_a.budgets.core_cpu_submit_ns == scene_b.budgets.core_cpu_submit_ns);
    REQUIRE(scene_a.budgets.render_cpu_sim_ns == scene_b.budgets.render_cpu_sim_ns);
    REQUIRE(scene_a.budgets.render_cpu_submit_ns == scene_b.budgets.render_cpu_submit_ns);
    REQUIRE(scene_a.budgets.geometry_cpu_sim_ns == scene_b.budgets.geometry_cpu_sim_ns);
    REQUIRE(scene_a.budgets.geometry_cpu_submit_ns == scene_b.budgets.geometry_cpu_submit_ns);
    REQUIRE(scene_a.budgets.physics_cpu_sim_ns == scene_b.budgets.physics_cpu_sim_ns);
    REQUIRE(scene_a.budgets.physics_cpu_submit_ns == scene_b.budgets.physics_cpu_submit_ns);
}

// ---------------------------------------------------------------------------
// manifest_cell_budgets_match_decision_record
//
// Verifies that every *_ns field in the loaded manifest matches the verbatim
// cell value in reviews/decisions/perf-budget.md §Per-Context Budget Table.
//
// This test embeds the canonical map from the decision record and asserts
// equality.  Failing this test means either:
//   (a) the manifest drifted from the decision record, or
//   (b) the decision record was amended without a fixture update spike.
//
// Both cases require a perf-budget amendment spike before the test can be
// re-greened (perf-budget.md §Consequences: "a fixture change requires a
// perf-budget amendment spike").
//
// Canonical values (perf-budget.md §Per-Context Budget Table, ms → ns):
//   core:     sim  0.40 ms =  400000 ns, submit 0.05 ms =   50000 ns
//   render:   sim  0.10 ms =  100000 ns, submit 1.40 ms = 1400000 ns
//   geometry: sim  0.30 ms =  300000 ns, submit 0.20 ms =  200000 ns
//   physics:  sim  2.00 ms = 2000000 ns, submit 0.00 ms =       0 ns
// ---------------------------------------------------------------------------
TEST_CASE("manifest_cell_budgets_match_decision_record", "[s1][fixture][core][perf-budget]") {
    const auto scene = glibre::testing::load_s1();
    const auto& b = scene.budgets;

    // core context (perf-budget.md: 0.40 ms sim, 0.05 ms submit)
    CHECK(b.core_cpu_sim_ns == 400'000u);
    CHECK(b.core_cpu_submit_ns == 50'000u);

    // render context (perf-budget.md: 0.10 ms sim, 1.40 ms submit)
    CHECK(b.render_cpu_sim_ns == 100'000u);
    CHECK(b.render_cpu_submit_ns == 1'400'000u);

    // geometry context (perf-budget.md: 0.30 ms sim, 0.20 ms submit)
    CHECK(b.geometry_cpu_sim_ns == 300'000u);
    CHECK(b.geometry_cpu_submit_ns == 200'000u);

    // physics context (perf-budget.md: 2.00 ms sim, 0.00 ms submit)
    CHECK(b.physics_cpu_sim_ns == 2'000'000u);
    CHECK(b.physics_cpu_submit_ns == 0u);
}

// ---------------------------------------------------------------------------
// s1_fixture_archetype_counts_match_scenario
//
// Verifies the archetype breakdown: 1 character + 200 props + 8 dynamic lights.
// This is a supplementary test that corresponds to the DoD unit_test_named
// entry in the dispatch prompt (s1_fixture_archetype_counts_match_scenario).
// ---------------------------------------------------------------------------
TEST_CASE("s1_fixture_archetype_counts_match_scenario", "[s1][fixture][core]") {
    const auto scene = glibre::testing::load_s1();

    REQUIRE(scene.archetypes.character == 1u);
    REQUIRE(scene.archetypes.prop == 200u);
    REQUIRE(scene.archetypes.dynamic_light == 8u);

    // Sanity: archetype counts sum to entity_count.
    const std::uint32_t sum =
        scene.archetypes.character + scene.archetypes.prop + scene.archetypes.dynamic_light;
    REQUIRE(sum == scene.entity_count);
}

// ---------------------------------------------------------------------------
// s1_fixture_viewport_is_1920x1080
//
// Verifies the viewport descriptor.  This corresponds to the DoD
// unit_test_named entry in the dispatch prompt.
// ---------------------------------------------------------------------------
TEST_CASE("s1_fixture_viewport_is_1920x1080", "[s1][fixture][core]") {
    const auto scene = glibre::testing::load_s1();

    REQUIRE(scene.viewport.width == 1920u);
    REQUIRE(scene.viewport.height == 1080u);
}

// ---------------------------------------------------------------------------
// s1_fixture_loads_canonical_209_entities
//
// Verifies entity_count == 209 (1 char + 200 props + 8 lights).
// This name matches the DoD unit_test_named entry in the dispatch prompt.
// ---------------------------------------------------------------------------
TEST_CASE("s1_fixture_loads_canonical_209_entities", "[s1][fixture][core]") {
    const auto scene = glibre::testing::load_s1();
    REQUIRE(scene.entity_count == 209u);
}

// ---------------------------------------------------------------------------
// s1_fixture_budgets_match_perf_budget_md
//
// Verifies loaded budgets match the verbatim spec values.
// This name matches the DoD unit_test_named entry in the dispatch prompt.
// ---------------------------------------------------------------------------
TEST_CASE("s1_fixture_budgets_match_perf_budget_md", "[s1][fixture][core][perf-budget]") {
    const auto scene = glibre::testing::load_s1();
    const auto& b = scene.budgets;

    // All values verbatim from perf-budget.md §Per-Context Budget Table.
    CHECK(b.core_cpu_sim_ns == 400'000u);
    CHECK(b.core_cpu_submit_ns == 50'000u);
    CHECK(b.render_cpu_sim_ns == 100'000u);
    CHECK(b.render_cpu_submit_ns == 1'400'000u);
    CHECK(b.geometry_cpu_sim_ns == 300'000u);
    CHECK(b.geometry_cpu_submit_ns == 200'000u);
    CHECK(b.physics_cpu_sim_ns == 2'000'000u);
    CHECK(b.physics_cpu_submit_ns == 0u);
}
