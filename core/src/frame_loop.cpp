// core/src/frame_loop.cpp
//
// Implementation of glibre::core::FrameLoop.
//
// Phase ordering is authoritative in reviews/decisions/frame-phases.md.
// The check for FramePhaseMisordered is enabled in debug builds only
// (NDEBUG not defined), per SPEC §4.4 invariant 1 and the schedule-
// frame-design.md §3.1 note: "Violation is a … debug-build runtime
// assertion core::Error::FramePhaseMisordered."

#include "glibre/core/frame_loop.hpp"

#include <cstdint>
#include <expected>

#include "glibre/core/frame_phase.hpp"
#include "glibre/error.hpp"

namespace glibre::core {

// ---------------------------------------------------------------------------
// run_phase — execute one phase slot.
//
// In debug builds the expected_ordinal parameter enforces strict ordering:
// if the phase's numeric value differs from the expected counter, the tick
// is aborted with FramePhaseMisordered.  This catches any future regression
// where the phase walk order or the kPhaseTable order is misaligned.
//
// In release builds the ordinal check is compiled out (NDEBUG defined →
// the branch is dead; the compiler eliminates it).
//
// MVP phase bodies are empty (no-op): the skeleton ships the ordering
// infrastructure; individual context plans fill the bodies.
// ---------------------------------------------------------------------------

[[nodiscard]] std::expected<void, glibre::Error>
FrameLoop::run_phase(Phase phase, std::uint8_t expected_ordinal) noexcept {
#ifndef NDEBUG
    const auto ordinal = static_cast<std::uint8_t>(phase);
    if (ordinal != expected_ordinal) {
        return std::unexpected(
            glibre::Error{core::Error::FramePhaseMisordered});
    }
#else
    (void)expected_ordinal;
#endif

    // --- Empty MVP phase bodies ---
    // Each phase slot executes nothing in this skeleton.  Future plans
    // for each owning context will inject bodies here (or through a
    // registered system dispatch, once the schedule compilation plan
    // lands).  The slot exists to validate the ordering infrastructure
    // and to give the CI a stable hook for phase-level benchmarking.
    switch (phase) {
        case Phase::Input:        /* platform — MVP empty */ break;
        case Phase::Logic:        /* gameplay/scripting — reserved empty */ break;
        case Phase::PhysicsFixed: /* physics — MVP empty */ break;
        case Phase::Animation:    /* animation — reserved empty */ break;
        case Phase::Transform:    /* core — MVP empty */ break;
        case Phase::CullExtract:  /* render — MVP empty */ break;
        case Phase::RenderSubmit: /* render — MVP empty */ break;
        case Phase::HotReload:    /* core (barrier) — MVP empty */ break;
        case Phase::Present:      /* platform — MVP empty */ break;
    }

    return {};  // success — no allocation
}

// ---------------------------------------------------------------------------
// tick — advance one engine frame.
//
// Phases are walked in strict numeric order (1 through 9) using the
// kPhaseTable as the authoritative ordering source.  A per-phase counter
// (expected_ordinal) advances monotonically; run_phase validates it in
// debug builds.
//
// No heap allocation occurs inside this function.
// ---------------------------------------------------------------------------

[[nodiscard]] std::expected<void, glibre::Error>
FrameLoop::tick() noexcept {
    std::uint8_t expected_ordinal = kPhaseMin;

    for (const PhaseDesc& desc : kPhaseTable) {
        auto result = run_phase(desc.id, expected_ordinal);
        if (!result) {
            return result;  // propagate error; frame_index_ not incremented
        }
        ++expected_ordinal;
    }

    ++frame_index_;
    return {};
}

}  // namespace glibre::core
