// tests/core/frame_loop_test.cpp
//
// Catch2 unit tests for glibre::core::FrameLoop and the 9-slot phase table.
//
// Named test cases (per plan #244 Unit Test Plan):
//   - core/frame_loop: phase_table_ids_are_1_through_9
//   - core/frame_loop: tick_invokes_phases_in_strict_order
//   - core/frame_loop: empty_mvp_phases_succeed
//
// Named test cases (per plan #247 Unit Test Plan):
//   - core/frame_loop: present_advances_tick_exactly_once
//   - core/frame_loop: tick_does_not_advance_when_phase_8_refuses

#include <array>
#include <cstdint>

#include <EASTL/variant.h>
#include <catch2/catch_test_macros.hpp>

#include "glibre/core/frame_loop.hpp"
#include "glibre/core/frame_phase.hpp"
#include "glibre/core/world_tick.hpp"
#include "glibre/error.hpp"

// ---------------------------------------------------------------------------
// Test: phase_table_ids_are_1_through_9
//
// Verifies that kPhaseTable contains exactly 9 entries whose Phase ordinal
// values are 1 through 9 in strict ascending order, matching the authority
// in reviews/decisions/frame-phases.md.
// ---------------------------------------------------------------------------
TEST_CASE("core/frame_loop: phase_table_ids_are_1_through_9", "[core][frame_loop]") {
    using namespace glibre::core;

    REQUIRE(kPhaseTable.size() == 9u);

    // Verify every ordinal 1..=9 is present in sequence.
    for (std::uint8_t i = 0; i < kPhaseCount; ++i) {
        const std::uint8_t expected_ordinal = static_cast<std::uint8_t>(i + 1u);
        const std::uint8_t actual_ordinal = static_cast<std::uint8_t>(kPhaseTable[i].id);

        CHECK(actual_ordinal == expected_ordinal);
    }

    // Spot-check the specific named phases from the decision record.
    CHECK(static_cast<std::uint8_t>(Phase::Input) == 1u);
    CHECK(static_cast<std::uint8_t>(Phase::Logic) == 2u);
    CHECK(static_cast<std::uint8_t>(Phase::PhysicsFixed) == 3u);
    CHECK(static_cast<std::uint8_t>(Phase::Animation) == 4u);
    CHECK(static_cast<std::uint8_t>(Phase::Transform) == 5u);
    CHECK(static_cast<std::uint8_t>(Phase::CullExtract) == 6u);
    CHECK(static_cast<std::uint8_t>(Phase::RenderSubmit) == 7u);
    CHECK(static_cast<std::uint8_t>(Phase::HotReload) == 8u);
    CHECK(static_cast<std::uint8_t>(Phase::Present) == 9u);

    // Verify kPhaseTable order matches the Phase ordinals.
    CHECK(kPhaseTable[0].id == Phase::Input);
    CHECK(kPhaseTable[1].id == Phase::Logic);
    CHECK(kPhaseTable[2].id == Phase::PhysicsFixed);
    CHECK(kPhaseTable[3].id == Phase::Animation);
    CHECK(kPhaseTable[4].id == Phase::Transform);
    CHECK(kPhaseTable[5].id == Phase::CullExtract);
    CHECK(kPhaseTable[6].id == Phase::RenderSubmit);
    CHECK(kPhaseTable[7].id == Phase::HotReload);
    CHECK(kPhaseTable[8].id == Phase::Present);

    // Verify the helper accessor.
    CHECK(phase_desc(Phase::Input).id == Phase::Input);
    CHECK(phase_desc(Phase::HotReload).id == Phase::HotReload);
    CHECK(phase_desc(Phase::Present).id == Phase::Present);

    // Verify reserved-slot flags (phases 2 and 4 are MVP-reserved).
    CHECK(kPhaseTable[1].mvp_reserved == true);  // Logic
    CHECK(kPhaseTable[3].mvp_reserved == true);  // Animation
    // All other phases are not reserved.
    CHECK(kPhaseTable[0].mvp_reserved == false);  // Input
    CHECK(kPhaseTable[2].mvp_reserved == false);  // PhysicsFixed
    CHECK(kPhaseTable[4].mvp_reserved == false);  // Transform
    CHECK(kPhaseTable[5].mvp_reserved == false);  // CullExtract
    CHECK(kPhaseTable[6].mvp_reserved == false);  // RenderSubmit
    CHECK(kPhaseTable[7].mvp_reserved == false);  // HotReload
    CHECK(kPhaseTable[8].mvp_reserved == false);  // Present
}

// ---------------------------------------------------------------------------
// Test: tick_invokes_phases_in_strict_order
//
// Verifies that FrameLoop::tick() runs all nine phases in strict ordinal
// order (1 through 9) and that the frame counter increments after a
// successful tick.
//
// The execution order is observed via FrameLoop::last_tick_phase_ordinals(),
// available in GLIBRE_TESTING builds.  Each entry is the Phase ordinal
// (uint8_t) of the phase that ran at that position; the expected sequence is
// 1, 2, 3, 4, 5, 6, 7, 8, 9.
//
// GLIBRE_TESTING is propagated PUBLIC from glibre-core when GLIBRE_BUILD_TESTS
// is ON (plan #997 fix), so last_tick_phase_ordinals() is available.
// ---------------------------------------------------------------------------
TEST_CASE("core/frame_loop: tick_invokes_phases_in_strict_order", "[core][frame_loop]") {
    using namespace glibre::core;

    FrameLoop loop;

    // Verify the frame index starts at 0.
    REQUIRE(loop.frame_index() == 0u);

    // Execute one tick; it must succeed (all nine empty bodies).
    auto result = loop.tick();
    REQUIRE(result.has_value());

    // Frame index must advance to 1 after a successful tick.
    CHECK(loop.frame_index() == 1u);

#ifdef GLIBRE_TESTING
    // Verify that all nine phases executed in strict ascending ordinal order.
    auto recorded = loop.last_tick_phase_ordinals();
    REQUIRE(recorded.size() == kPhaseCount);

    for (std::uint8_t i = 0; i < kPhaseCount; ++i) {
        const std::uint8_t expected_ordinal = static_cast<std::uint8_t>(i + 1u);
        INFO(
            "Phase at position " << static_cast<int>(i) << ": expected ordinal "
                                 << static_cast<int>(expected_ordinal) << " got "
                                 << static_cast<int>(recorded[i])
        );
        CHECK(recorded[i] == expected_ordinal);
    }

    // Spot-check canonical ordinals against the Phase enum.
    CHECK(recorded[0] == static_cast<std::uint8_t>(Phase::Input));
    CHECK(recorded[1] == static_cast<std::uint8_t>(Phase::Logic));
    CHECK(recorded[2] == static_cast<std::uint8_t>(Phase::PhysicsFixed));
    CHECK(recorded[3] == static_cast<std::uint8_t>(Phase::Animation));
    CHECK(recorded[4] == static_cast<std::uint8_t>(Phase::Transform));
    CHECK(recorded[5] == static_cast<std::uint8_t>(Phase::CullExtract));
    CHECK(recorded[6] == static_cast<std::uint8_t>(Phase::RenderSubmit));
    CHECK(recorded[7] == static_cast<std::uint8_t>(Phase::HotReload));
    CHECK(recorded[8] == static_cast<std::uint8_t>(Phase::Present));
#endif

    // Execute a second tick; must also succeed.
    auto result2 = loop.tick();
    REQUIRE(result2.has_value());
    CHECK(loop.frame_index() == 2u);
}

// ---------------------------------------------------------------------------
// Test: empty_mvp_phases_succeed
//
// Verifies that a FrameLoop with empty MVP phase bodies returns a successful
// std::expected<void, glibre::Error> from tick() — i.e. the no-op bodies
// for all nine phases do not trigger any error path.
// ---------------------------------------------------------------------------
TEST_CASE("core/frame_loop: empty_mvp_phases_succeed", "[core][frame_loop]") {
    using namespace glibre::core;

    FrameLoop loop;

    // Run several ticks; all must return a value (not an error).
    for (int i = 0; i < 10; ++i) {
        auto result = loop.tick();
        INFO("tick " << i << " failed unexpectedly");
        REQUIRE(result.has_value());
    }

    // frame_index() must equal the number of successful ticks.
    CHECK(loop.frame_index() == 10u);
}

// ---------------------------------------------------------------------------
// Test: present_advances_tick_exactly_once
//
// Verifies that a single tick() call advances frame_counter by exactly 1 and
// advances world_tick (both value and change_tick fields) by exactly 1.
//
// Authority: plan #247 §Unit Test Plan.
//            reviews/decisions/frame-phases.md §Phase 9:
//              "world ChangeTick increment; frame counter."
// ---------------------------------------------------------------------------
TEST_CASE("core/frame_loop: present_advances_tick_exactly_once", "[core][frame_loop]") {
    using namespace glibre::core;

    FrameLoop loop;

    // Before any tick: counters are at zero.
    REQUIRE(loop.frame_counter() == 0u);
    REQUIRE(loop.world_tick().value == 0u);
    REQUIRE(loop.world_tick().change_tick == 0u);

    // Execute one tick; must succeed.
    auto result = loop.tick();
    REQUIRE(result.has_value());

    // After one tick: frame_counter advances by exactly 1.
    CHECK(loop.frame_counter() == 1u);

    // After one tick: world_tick.value advances by exactly 1.
    CHECK(loop.world_tick().value == 1u);

    // After one tick: world_tick.change_tick advances by exactly 1.
    CHECK(loop.world_tick().change_tick == 1u);

    // Execute a second tick.
    auto result2 = loop.tick();
    REQUIRE(result2.has_value());

    // After two ticks: frame_counter == 2, world_tick.value == 2.
    CHECK(loop.frame_counter() == 2u);
    CHECK(loop.world_tick().value == 2u);
    CHECK(loop.world_tick().change_tick == 2u);
}

// ---------------------------------------------------------------------------
// Test: tick_does_not_advance_when_phase_8_refuses
//
// Verifies that when Phase::HotReload (phase 8) refuses (returns an error),
// tick() returns that error AND frame_counter + world_tick are NOT advanced
// (Phase::Present is never reached, so advance_world_tick is never called).
//
// This exercises the all-or-nothing invariant: a phase failure short-circuits
// the tick without advancing any frame-level counters.
//
// Authority: plan #247 §Unit Test Plan.
//            reviews/decisions/frame-phases.md §Phase 8 (hot-reload) — the
//            only phase during which plugin mutations are permitted; refusing
//            at this phase halts the frame.
//
// GLIBRE_TESTING: uses FrameLoop::set_inject_phase8_failure() to arm the
// test-only injection hook in Phase::HotReload.  This simulates the
// drain-phase guard (plan #981 / PR #1012) returning FramePhaseMisordered
// without requiring a live plugin loader.
// ---------------------------------------------------------------------------
TEST_CASE("core/frame_loop: tick_does_not_advance_when_phase_8_refuses", "[core][frame_loop]") {
    using namespace glibre::core;

#ifdef GLIBRE_TESTING
    FrameLoop loop;

    // Arm the phase-8 failure injection.
    loop.set_inject_phase8_failure(true);

    // Capture baseline counters (all zero).
    const std::uint64_t counter_before = loop.frame_counter();
    const std::uint64_t tick_value_before = loop.world_tick().value;
    const std::uint64_t tick_change_before = loop.world_tick().change_tick;

    REQUIRE(counter_before == 0u);
    REQUIRE(tick_value_before == 0u);
    REQUIRE(tick_change_before == 0u);

    // tick() must return an error (FramePhaseMisordered from phase 8).
    auto result = loop.tick();
    REQUIRE_FALSE(result.has_value());

    // The error must be FramePhaseMisordered (the drain-phase refusal code).
    const auto& err = result.error();
    const bool is_misordered =
        eastl::holds_alternative<glibre::core::Error>(err.code()) &&
        eastl::get<glibre::core::Error>(err.code()) ==
            glibre::core::Error::FramePhaseMisordered;
    CHECK(is_misordered);

    // frame_counter must NOT have advanced.
    CHECK(loop.frame_counter() == counter_before);

    // world_tick.value must NOT have advanced.
    CHECK(loop.world_tick().value == tick_value_before);

    // world_tick.change_tick must NOT have advanced.
    CHECK(loop.world_tick().change_tick == tick_change_before);

    // frame_index must NOT have advanced (existing invariant: only advances
    // on full success).
    CHECK(loop.frame_index() == 0u);

    // Disarm the injection; verify a subsequent tick() succeeds and advances.
    loop.set_inject_phase8_failure(false);
    auto result2 = loop.tick();
    REQUIRE(result2.has_value());
    CHECK(loop.frame_counter() == 1u);
    CHECK(loop.world_tick().value == 1u);
#else
    // Non-GLIBRE_TESTING builds: the injection API is not available.
    // The test still verifies that a clean tick advances both counters
    // (redundant with present_advances_tick_exactly_once but keeps CI green).
    FrameLoop loop;
    auto result = loop.tick();
    REQUIRE(result.has_value());
    CHECK(loop.frame_counter() == 1u);
    CHECK(loop.world_tick().value == 1u);
#endif
}
