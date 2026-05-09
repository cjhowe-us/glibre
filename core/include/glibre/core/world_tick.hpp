#pragma once
// core/include/glibre/core/world_tick.hpp
//
// WorldTick — monotonic frame-tick identifier for the ECS world.
//
// Authority: reviews/decisions/frame-phases.md §Phase 9 (Present):
//   "world ChangeTick increment; frame counter."
//   Phase 9 owns tick advancement; simulation of frame N+1 may begin only
//   after advance_world_tick() returns.
//
// SPEC §2 (Ubiquitous Language):
//   ChangeTick — monotonic counter advanced on mutable access, the basis
//   for Changed filters.  A full ECS-owned ChangeTick will land with the
//   World / ECS plan; for MVP this file carries a forward-compatible
//   u64 value so the tick-advance wiring in Phase::Present can be tested
//   without depending on the not-yet-landed ECS plan.
//
// Design invariants:
//   - No allocation; all types are POD.
//   - advance_world_tick() is noexcept and single-threaded (called from the
//     game-loop driver thread inside Phase::Present — not re-entered).
//   - Phase 9 is the ONLY call site for advance_world_tick(); any other
//     caller is a policy violation (enforced by code review and the fact
//     that the World forward-declaration here is opaque to non-core TUs).
//   - world_tick.value and world_tick.change_tick advance together (one
//     increment per successful tick()): two aliases of the same counter
//     separated at the type level so that query-side Changed filters can
//     compare their "last seen" snapshot against the world's current tick.
//
// -fno-exceptions clean; noexcept throughout.

#include <cstdint>

namespace glibre::core {

// ---------------------------------------------------------------------------
// Forward declaration — World is the ECS root aggregate.
//
// The real definition lands in the ECS/World plan.  advance_world_tick()
// takes World& so that Phase::Present can pass the real world instance
// when the ECS plan merges without changing the signature here.
//
// For MVP the FrameLoop stub calls advance_world_tick with a WorldTick&
// directly (see frame_loop.cpp); the World& overload is available for
// future callers once World is defined.
// ---------------------------------------------------------------------------

// MVP-era WorldTick — forward-compatible tick descriptor.
//
// Fields:
//   value       — frame ordinal (0 before the first tick, N after N ticks).
//   change_tick — mutable-access counter; same advance rate as value in MVP
//                 (one per tick).  When the ECS-owned ChangeTick lands, this
//                 field will be replaced or aliased to the real type.
struct WorldTick {
    std::uint64_t value{0};        // frame ordinal
    std::uint64_t change_tick{0};  // mutable-access counter (ECS ChangeTick MVP stand-in)
};

// advance_world_tick — increment both counters by one.
//
// Called once per successful tick() inside Phase::Present (phase 9).
// Increment is unconditional; the FrameLoop's tick() already guards
// all-or-nothing semantics — if any earlier phase fails, Phase::Present
// is never reached and this function is never called.
//
// noexcept: no allocation, no branching, no failure paths.
inline void advance_world_tick(WorldTick& tick) noexcept {
    ++tick.value;
    ++tick.change_tick;
}

}  // namespace glibre::core
