#pragma once
// core/include/glibre/core/frame_phase.hpp
//
// Immutable 9-slot frame-phase table for glibre::core::FrameLoop.
//
// Phase ordering is authoritative in reviews/decisions/frame-phases.md.
// Do NOT re-litigate slot numbers here.  Adding a phase requires an
// amendment spike against frame-phases.md; this enum is intentionally
// closed (SPEC §3.2 collapse #1, SPEC §5.3).
//
// -fno-exceptions clean; no allocation; all data is constexpr POD.

#include <array>
#include <cstdint>
#include <string_view>

namespace glibre::core {

// -----------------------------------------------------------------------
// Phase — closed enum of the nine frame-phases in strict numeric order
// -----------------------------------------------------------------------

enum class Phase : std::uint8_t {
    Input = 1,
    Logic = 2,  // reserved — empty body in MVP (gameplay/scripting)
    PhysicsFixed = 3,
    Animation = 4,  // reserved — empty body in MVP (animation plugin)
    Transform = 5,
    CullExtract = 6,
    RenderSubmit = 7,
    HotReload = 8,
    Present = 9,
};

inline constexpr std::uint8_t kPhaseCount = 9;
inline constexpr std::uint8_t kPhaseMin = 1;
inline constexpr std::uint8_t kPhaseMax = 9;

// -----------------------------------------------------------------------
// PhaseDesc — compile-time descriptor for one phase slot
// -----------------------------------------------------------------------

struct PhaseDesc {
    Phase id;                         // numeric ordinal (1..=9)
    std::string_view name;            // canonical lower_snake_case name
    std::string_view owning_context;  // bounded-context owner per frame-phases.md
    bool mvp_reserved;                // true → empty body in MVP (phases 2, 4)
};

// -----------------------------------------------------------------------
// kPhaseTable — compile-time, zero-allocation phase registry
//
// Entries are in strict numeric order matching the Phase ordinal values.
// This table is the single in-code copy of the information in
// reviews/decisions/frame-phases.md; if they diverge, the decision
// record wins and this file must be updated.
// -----------------------------------------------------------------------

inline constexpr std::array<PhaseDesc, kPhaseCount> kPhaseTable{{
    {Phase::Input, "input", "platform", false},
    {Phase::Logic, "logic", "gameplay/scripting", true},
    {Phase::PhysicsFixed, "physics_fixed", "physics", false},
    {Phase::Animation, "animation", "animation", true},
    {Phase::Transform, "transform", "core", false},
    {Phase::CullExtract, "cull_extract", "render", false},
    {Phase::RenderSubmit, "render_submit", "render", false},
    {Phase::HotReload, "hot_reload", "core", false},
    {Phase::Present, "present", "platform", false},
}};

// -----------------------------------------------------------------------
// Helper — look up a PhaseDesc by Phase ordinal (O(1), constexpr)
// -----------------------------------------------------------------------

[[nodiscard]] constexpr const PhaseDesc& phase_desc(Phase p) noexcept {
    // kPhaseTable is 0-indexed; Phase ordinals are 1..=9.
    return kPhaseTable[static_cast<std::uint8_t>(p) - 1u];
}

}  // namespace glibre::core
