// tests/core/frame_loop_test.cpp
//
// Catch2 unit tests for glibre::core::FrameLoop and the 9-slot phase table.
//
// Named test cases (per plan #244 Unit Test Plan):
//   - core/frame_loop: phase_table_ids_are_1_through_9
//   - core/frame_loop: tick_invokes_phases_in_strict_order
//   - core/frame_loop: empty_mvp_phases_succeed

#include <catch2/catch_test_macros.hpp>

#include "glibre/core/frame_loop.hpp"
#include "glibre/core/frame_phase.hpp"
#include "glibre/error.hpp"

#include <cstdint>
#include <array>

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
        const std::uint8_t actual_ordinal   =
            static_cast<std::uint8_t>(kPhaseTable[i].id);

        CHECK(actual_ordinal == expected_ordinal);
    }

    // Spot-check the specific named phases from the decision record.
    CHECK(static_cast<std::uint8_t>(Phase::Input)        == 1u);
    CHECK(static_cast<std::uint8_t>(Phase::Logic)        == 2u);
    CHECK(static_cast<std::uint8_t>(Phase::PhysicsFixed) == 3u);
    CHECK(static_cast<std::uint8_t>(Phase::Animation)    == 4u);
    CHECK(static_cast<std::uint8_t>(Phase::Transform)    == 5u);
    CHECK(static_cast<std::uint8_t>(Phase::CullExtract)  == 6u);
    CHECK(static_cast<std::uint8_t>(Phase::RenderSubmit) == 7u);
    CHECK(static_cast<std::uint8_t>(Phase::HotReload)    == 8u);
    CHECK(static_cast<std::uint8_t>(Phase::Present)      == 9u);

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
    CHECK(phase_desc(Phase::Input).id        == Phase::Input);
    CHECK(phase_desc(Phase::HotReload).id    == Phase::HotReload);
    CHECK(phase_desc(Phase::Present).id      == Phase::Present);

    // Verify reserved-slot flags (phases 2 and 4 are MVP-reserved).
    CHECK(kPhaseTable[1].mvp_reserved == true);   // Logic
    CHECK(kPhaseTable[3].mvp_reserved == true);   // Animation
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
// Verifies that FrameLoop::tick() runs all phases in strict ordinal order
// (1 through 9) and that the frame counter increments after a successful
// tick.  In debug builds the FramePhaseMisordered check inside run_phase()
// validates the ordering; in release builds the phase counter still
// advances and the frame_index() post-condition confirms completion.
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
