#pragma once
// core/include/glibre/core/frame_loop.hpp
//
// glibre::core::FrameLoop — engine-wide frame driver.
//
// Responsibilities (per core/SPEC.md §4.4 and schedule-frame-design.md §1):
//   - Walk the nine phases in strict numeric order each tick.
//   - In debug builds, detect and report out-of-sequence phase execution
//     via core::Error::FramePhaseMisordered.
//   - Empty MVP phase bodies — phases 1, 2, 3, 4, 6, 7, 8, 9 are no-ops
//     until each owning context's plan lands its body (phase 5 transform
//     is similarly empty at this skeleton stage).
//   - No dynamic allocation inside tick().
//   - -fno-exceptions clean; noexcept throughout the public surface.
//
// See reviews/decisions/frame-phases.md for the authoritative ordering.
// See reviews/decisions/error-model.md for std::expected usage rules.

#include <array>
#include <cstdint>
#include <span>

#include "glibre/core/frame_phase.hpp"
#include "glibre/error.hpp"

namespace glibre::core {

// -----------------------------------------------------------------------
// FrameLoop
//
// MVP skeleton: runs the nine phases in strict numeric order, returning
// an error on any detected ordering violation (debug builds only).
//
// Invariants guaranteed per tick():
//   (a) Phases execute in order Phase::Input (1) through Phase::Present (9).
//   (b) No phase observes writes from a later phase of the same frame.
//   (c) No heap allocation occurs inside tick().
//   (d) The call returns glibre::Result<void>; callers must inspect the result.
// -----------------------------------------------------------------------

class FrameLoop {
public:
    FrameLoop() noexcept = default;

    // Non-copyable, non-movable (owns frame state).
    FrameLoop(const FrameLoop&) = delete;
    FrameLoop& operator=(const FrameLoop&) = delete;
    FrameLoop(FrameLoop&&) = delete;
    FrameLoop& operator=(FrameLoop&&) = delete;

    ~FrameLoop() noexcept = default;

    // tick() — advance one engine frame.
    //
    // Walks all nine phases in numeric order.  In debug builds a
    // per-frame phase counter is maintained; any phase that attempts
    // to execute out of sequence (e.g. because a future registration
    // mechanism misfires) returns core::Error::FramePhaseMisordered.
    //
    // Returns:
    //   glibre::Result<void> (= std::expected<void, glibre::Error>) —
    //   success is a value-initialised expected; failure wraps
    //   core::Error::FramePhaseMisordered.
    [[nodiscard]] glibre::Result<void> tick() noexcept;

    // frame_index() — the number of successfully completed ticks.
    // Incremented only after all nine phases succeed.
    [[nodiscard]] std::uint64_t frame_index() const noexcept { return frame_index_; }

private:
    // run_phase() — execute one phase.  Returns an error if the phase
    // ordinal does not match the expected_ordinal (debug builds only).
    // The ordinal check guards against future registration mechanisms
    // that might supply phases in a different order than kPhaseTable.
    [[nodiscard]] glibre::Result<void>
    run_phase(Phase phase, std::uint8_t expected_ordinal) noexcept;

    std::uint64_t frame_index_{0};

#ifdef GLIBRE_TESTING
    // Under GLIBRE_TESTING builds the last tick's phase execution order is
    // recorded so tests can assert strict ordering without relying solely
    // on the debug-build ordinal check.  Fixed-size; kPhaseCount entries.
    std::array<std::uint8_t, kPhaseCount> last_tick_phase_ordinals_{};
    std::uint8_t last_tick_phase_count_{0};

public:
    // Returns the sequence of Phase ordinals visited during the most recent
    // successful tick (kPhaseCount entries, in execution order).
    // Only available when compiled with -DGLIBRE_TESTING.
    [[nodiscard]] std::span<const std::uint8_t> last_tick_phase_ordinals() const noexcept {
        return {last_tick_phase_ordinals_.data(), last_tick_phase_count_};
    }
#endif
};

}  // namespace glibre::core
