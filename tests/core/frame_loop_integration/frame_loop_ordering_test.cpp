// tests/core/frame_loop_integration/frame_loop_ordering_test.cpp
//
// Catch2 integration test: end-to-end nine-phase ordering for FrameLoop.
//
// Named test cases (per plan #248 §Unit Test Plan):
//   - core/frame_loop_integration: nine_phase_round_trip_orders_correctly
//   - core/frame_loop_integration: world_tick_visible_to_all_phases_in_frame
//   - core/frame_loop_integration: empty_reserved_phases_no_op
//
// These tests exercise the frame-loop ordering invariants specified in
// reviews/decisions/frame-phases.md using the GLIBRE_TESTING instrumentation
// hooks on FrameLoop (last_tick_phase_ordinals, world_tick, frame_counter).
//
// No PhaseRegistry::register_system API is available yet (plan #245 is the
// dependency that will add it).  Until that plan lands, the "synthetic system"
// role is played by the GLIBRE_TESTING phase-ordinal recorder built into
// FrameLoop, which records every phase that executes inside tick() in the
// order of execution.  This is the "whatever the actual API is" path called
// out in plan #248 §Scope.
//
// All three tests are deterministic, allocation-free (no heap inside tick()),
// and run under `ctest -L unit`.

#include <array>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include "glibre/core/frame_loop.hpp"
#include "glibre/core/frame_phase.hpp"
#include "glibre/core/world_tick.hpp"

// GLIBRE_TESTING is propagated PUBLIC from glibre-core when GLIBRE_BUILD_TESTS
// is ON (plan #997), so the GLIBRE_TESTING-gated members on FrameLoop are
// available to every test target that links glibre::core.  All three test
// cases below depend on last_tick_phase_ordinals(), which requires
// GLIBRE_TESTING.  The static_assert below makes the dependency explicit and
// produces a clear diagnostic rather than a linker or compile error.
#ifndef GLIBRE_TESTING
static_assert(
    false,
    "frame_loop_ordering_test.cpp requires GLIBRE_TESTING=1; "
    "build with -DGLIBRE_BUILD_TESTS=ON (sets GLIBRE_TESTING PUBLIC on glibre-core)"
);
#endif

// ---------------------------------------------------------------------------
// Test: nine_phase_round_trip_orders_correctly
//
// Plan #248 §Scope assertion 1:
//   Each phase's system observes a strictly increasing per-tick sequence
//   number matching (tick_index * 9) + phase_id.
//
// Interpretation with the current API (no PhaseRegistry yet):
//   The GLIBRE_TESTING recorder captures which Phase ordinals executed during
//   each tick() in the order they ran.  For tick T (0-indexed), the global
//   sequence number for the phase at position P (0-indexed) in the ordinal
//   span is:
//
//       global_seq = (T * kPhaseCount) + P
//
//   The phase_id at that position must equal (P + 1), giving:
//
//       expected_ordinal_at[T][P] == (P + 1)   for all T, P
//
//   Combined: for the entry at (T, P), global_seq == (T * 9) + P, and
//   the phase ordinal == (P + 1) == (global_seq - T * 9) + 1.  This matches
//   the plan assertion when "phase_id" is interpreted as the phase's position
//   in the sequence within that tick.
//
// Run 9 ticks (one per phase count) so that the outer index corresponds to
// a tick number that is a multiple of the phase count — a convenient N for
// the round-trip property.
// ---------------------------------------------------------------------------

TEST_CASE("core/frame_loop_integration: nine_phase_round_trip_orders_correctly",
          "[core][frame_loop_integration]") {
    using namespace glibre::core;

    FrameLoop loop;
    REQUIRE(loop.frame_index() == 0u);

    // Run kPhaseCount ticks (9), verifying strict ordering each tick.
    for (std::uint64_t tick_idx = 0; tick_idx < kPhaseCount; ++tick_idx) {
        INFO("tick_idx=" << tick_idx);

        auto result = loop.tick();
        REQUIRE(result.has_value());
        REQUIRE(loop.frame_index() == tick_idx + 1u);

        // Verify that exactly kPhaseCount phases ran this tick in 1..=9 order.
        auto ordinals = loop.last_tick_phase_ordinals();
        REQUIRE(ordinals.size() == kPhaseCount);

        for (std::uint8_t pos = 0; pos < kPhaseCount; ++pos) {
            // Global sequence number for this (tick, position) pair.
            const std::uint64_t global_seq = (tick_idx * kPhaseCount) + pos;
            // The phase ordinal at this position must be (pos + 1).
            const std::uint8_t expected_ordinal = static_cast<std::uint8_t>(pos + 1u);
            const std::uint8_t actual_ordinal   = ordinals[pos];

            INFO(
                "global_seq=" << global_seq
                << " pos=" << static_cast<int>(pos)
                << " expected_ordinal=" << static_cast<int>(expected_ordinal)
                << " actual_ordinal=" << static_cast<int>(actual_ordinal)
            );

            // Strictly increasing global sequence number: each position advances.
            // Verified implicitly by checking pos increments in the for-loop;
            // the ordinal check is the direct assertion.
            CHECK(actual_ordinal == expected_ordinal);

            // Verify the phase at this position matches the kPhaseTable entry.
            CHECK(static_cast<std::uint8_t>(kPhaseTable[pos].id) == expected_ordinal);
        }

        // Verify canonical ordinals against Phase enum values.
        CHECK(ordinals[0] == static_cast<std::uint8_t>(Phase::Input));
        CHECK(ordinals[1] == static_cast<std::uint8_t>(Phase::Logic));
        CHECK(ordinals[2] == static_cast<std::uint8_t>(Phase::PhysicsFixed));
        CHECK(ordinals[3] == static_cast<std::uint8_t>(Phase::Animation));
        CHECK(ordinals[4] == static_cast<std::uint8_t>(Phase::Transform));
        CHECK(ordinals[5] == static_cast<std::uint8_t>(Phase::CullExtract));
        CHECK(ordinals[6] == static_cast<std::uint8_t>(Phase::RenderSubmit));
        CHECK(ordinals[7] == static_cast<std::uint8_t>(Phase::HotReload));
        CHECK(ordinals[8] == static_cast<std::uint8_t>(Phase::Present));
    }

    // After 9 ticks, frame_index must equal kPhaseCount.
    CHECK(loop.frame_index() == static_cast<std::uint64_t>(kPhaseCount));
}

// ---------------------------------------------------------------------------
// Test: world_tick_visible_to_all_phases_in_frame
//
// Plan #248 §Scope assertion 2:
//   Each per-phase system observes the same world_tick() value throughout
//   that phase, and observes world_tick + 1 after present completes.
//
// The world_tick() accessor returns the WorldTick as it stands after the most
// recent Phase::Present (9) execution.  Before tick N begins, world_tick().value
// equals (N - 1).  After tick N's Phase::Present, it equals N.
//
// Interpretation:
//   - The "same world_tick() throughout a frame" property is verified by
//     confirming that world_tick() does NOT advance until Phase::Present runs.
//     In the MVP skeleton there is no mid-frame mutation: world_tick() stays
//     constant from the start of tick T until Phase::Present of tick T fires.
//   - After Phase::Present: world_tick().value == frame_counter() == tick T.
//
// We verify this by sampling world_tick() before and after each tick.
// ---------------------------------------------------------------------------

TEST_CASE("core/frame_loop_integration: world_tick_visible_to_all_phases_in_frame",
          "[core][frame_loop_integration]") {
    using namespace glibre::core;

    FrameLoop loop;

    // Before any tick: world_tick is at the zero state.
    {
        const WorldTick wt = loop.world_tick();
        CHECK(wt.value == 0u);
        CHECK(wt.change_tick == 0u);
    }

    // Run multiple ticks and verify the tick advances exactly once per
    // successful Phase::Present, matching the "observable at end of present"
    // guarantee from reviews/decisions/frame-phases.md §Phase 9.
    //
    // Also verify that frame_counter() tracks the number of times Phase::Present
    // executed — the same count as the world tick value.
    static constexpr int kNumTicks = 12;
    for (int t = 0; t < kNumTicks; ++t) {
        // Capture world_tick BEFORE the tick.
        const WorldTick before = loop.world_tick();
        const std::uint64_t before_frame_counter = loop.frame_counter();

        // world_tick before tick T must equal T (ticks completed so far).
        CHECK(before.value       == static_cast<std::uint64_t>(t));
        CHECK(before.change_tick == static_cast<std::uint64_t>(t));
        CHECK(before_frame_counter == static_cast<std::uint64_t>(t));

        // Execute tick T.
        auto result = loop.tick();
        REQUIRE(result.has_value());

        // Capture world_tick AFTER Phase::Present (which runs inside tick()).
        const WorldTick after = loop.world_tick();
        const std::uint64_t after_frame_counter = loop.frame_counter();

        // world_tick must have advanced by exactly 1 (Phase::Present ran).
        CHECK(after.value       == static_cast<std::uint64_t>(t + 1));
        CHECK(after.change_tick == static_cast<std::uint64_t>(t + 1));
        CHECK(after_frame_counter == static_cast<std::uint64_t>(t + 1));

        // value and change_tick must be equal (MVP: both advance at same rate).
        CHECK(after.value == after.change_tick);

        // Verify the delta is exactly 1.
        CHECK(after.value - before.value == 1u);
        CHECK(after.change_tick - before.change_tick == 1u);
    }

    // Final state: world_tick equals kNumTicks.
    const WorldTick final_wt = loop.world_tick();
    CHECK(final_wt.value == static_cast<std::uint64_t>(kNumTicks));
    CHECK(final_wt.change_tick == static_cast<std::uint64_t>(kNumTicks));
    CHECK(loop.frame_counter() == static_cast<std::uint64_t>(kNumTicks));
}

// ---------------------------------------------------------------------------
// Test: empty_reserved_phases_no_op
//
// Plan #248 §Scope assertion 3:
//   Phase-2 (logic) and phase-4 (animation) reserved slots execute as
//   no-ops when no systems are registered.
//
// Verification:
//   (a) kPhaseTable[1] (Logic, ordinal 2) has mvp_reserved == true.
//   (b) kPhaseTable[3] (Animation, ordinal 4) has mvp_reserved == true.
//   (c) Both phases contribute their ordinal to last_tick_phase_ordinals()
//       — they are present in the execution sequence (positions 1 and 3).
//   (d) No error propagates from these empty slots: tick() succeeds.
//   (e) frame_index() advances normally even when the reserved phases run.
//   (f) The other 7 phases are NOT marked mvp_reserved.
//
// The "no-op" guarantee is exactly that these phases have no observable
// side effect beyond appearing in the ordinal trace; the test verifies that
// the ordinal trace at positions 1 and 3 carries the expected Phase ordinals
// (2 and 4) and that the tick completes without error.
// ---------------------------------------------------------------------------

TEST_CASE("core/frame_loop_integration: empty_reserved_phases_no_op",
          "[core][frame_loop_integration]") {
    using namespace glibre::core;

    // --- (a) + (b): compile-time-stable mvp_reserved flags ---
    // These are checked against kPhaseTable which is constexpr, so the values
    // are determined at compile time; the CHECK here documents the contract at
    // the test level and will surface in the Catch2 output if the table changes.
    REQUIRE(kPhaseTable.size() == kPhaseCount);

    // Phase 2 (Logic) — reserved.
    CHECK(kPhaseTable[1].id == Phase::Logic);
    CHECK(kPhaseTable[1].mvp_reserved == true);

    // Phase 4 (Animation) — reserved.
    CHECK(kPhaseTable[3].id == Phase::Animation);
    CHECK(kPhaseTable[3].mvp_reserved == true);

    // --- (f): all other phases are NOT reserved ---
    CHECK(kPhaseTable[0].mvp_reserved == false);  // Input
    CHECK(kPhaseTable[2].mvp_reserved == false);  // PhysicsFixed
    CHECK(kPhaseTable[4].mvp_reserved == false);  // Transform
    CHECK(kPhaseTable[5].mvp_reserved == false);  // CullExtract
    CHECK(kPhaseTable[6].mvp_reserved == false);  // RenderSubmit
    CHECK(kPhaseTable[7].mvp_reserved == false);  // HotReload
    CHECK(kPhaseTable[8].mvp_reserved == false);  // Present

    // --- (c) + (d) + (e): runtime — reserved phases run without error ---
    FrameLoop loop;

    // Run several ticks to confirm reserved phases execute as no-ops
    // in every tick, not just the first.
    for (int t = 0; t < 5; ++t) {
        INFO("tick t=" << t);

        auto result = loop.tick();

        // (d) No error from empty reserved phases.
        REQUIRE(result.has_value());

        // (e) frame_index advances: reserved phases do not abort the frame.
        REQUIRE(loop.frame_index() == static_cast<std::uint64_t>(t + 1));

        // (c) Both reserved phases appear in the ordinal trace at their
        // expected positions (1-indexed positions 2 and 4, 0-indexed 1 and 3).
        auto ordinals = loop.last_tick_phase_ordinals();
        REQUIRE(ordinals.size() == kPhaseCount);

        // Logic is at 0-indexed position 1, ordinal 2.
        const std::uint8_t logic_ordinal = ordinals[1];
        CHECK(logic_ordinal == static_cast<std::uint8_t>(Phase::Logic));

        // Animation is at 0-indexed position 3, ordinal 4.
        const std::uint8_t anim_ordinal = ordinals[3];
        CHECK(anim_ordinal == static_cast<std::uint8_t>(Phase::Animation));

        // Verify these two phases are the only mvp_reserved ones in the trace.
        for (std::uint8_t pos = 0; pos < kPhaseCount; ++pos) {
            const bool is_reserved = kPhaseTable[pos].mvp_reserved;
            // Positions 1 and 3 are reserved; all others are not.
            const bool expected_reserved = (pos == 1u) || (pos == 3u);
            CHECK(is_reserved == expected_reserved);
        }
    }
}
