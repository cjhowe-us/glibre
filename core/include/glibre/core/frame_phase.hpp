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
#include <cassert>
#include <cstdint>
#include <utility>

// EASTL substrate — PHILOSOPHY §11 mandates eastl::string_view for engine
// runtime data structures; std::string_view is not permitted in engine code.
#include <EASTL/string_view.h>

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
    Phase id;                           // numeric ordinal (1..=9)
    eastl::string_view name;            // canonical kebab-case name (telemetry/replay-stable
                                        // per frame-phases.md §Consequence #2)
                                        // PHILOSOPHY §11: eastl::string_view, not std::
    eastl::string_view owning_context;  // bounded-context owner per frame-phases.md
                                        // PHILOSOPHY §11: eastl::string_view, not std::
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

// Phase names are kebab-case, telemetry/replay-stable per
// frame-phases.md §Consequence #2.  Do NOT change these strings without
// an amendment spike against frame-phases.md — persisted profiler
// traces, replay records, and e2e fixtures reference them by value.
inline constexpr std::array<PhaseDesc, kPhaseCount> kPhaseTable{{
    {Phase::Input,        "input",         "platform",           false},
    {Phase::Logic,        "logic",         "gameplay/scripting", true},
    {Phase::PhysicsFixed, "physics-fixed", "physics",            false},
    {Phase::Animation,    "animation",     "animation",          true},
    {Phase::Transform,    "transform",     "core",               false},
    {Phase::CullExtract,  "cull-extract",  "render",             false},
    {Phase::RenderSubmit, "render-submit", "render",             false},
    {Phase::HotReload,    "hot-reload",    "core",               false},
    {Phase::Present,      "present",       "platform",           false},
}};

// ---------------------------------------------------------------------------
// Compile-time ordinal invariant — enforce that kPhaseTable[i].id == i+1
// for all i in [0, kPhaseCount).  This guards against accidental misordering
// in the table initialiser; it fires at compile time, not runtime.
// ---------------------------------------------------------------------------
namespace detail {
template<std::size_t... I>
constexpr bool kPhaseTableOrdinalsAreSequential(std::index_sequence<I...>) {
    return ((static_cast<std::uint8_t>(kPhaseTable[I].id) == static_cast<std::uint8_t>(I + 1u)) && ...);
}
}  // namespace detail

static_assert(
    detail::kPhaseTableOrdinalsAreSequential(std::make_index_sequence<kPhaseCount>{}),
    "kPhaseTable entries must appear in strict ordinal order 1..=9");

// -----------------------------------------------------------------------
// Helper — look up a PhaseDesc by Phase ordinal (O(1), constexpr)
// -----------------------------------------------------------------------

[[nodiscard]] constexpr const PhaseDesc& phase_desc(Phase p) noexcept {
    // Precondition: p must be a valid Phase ordinal in [kPhaseMin, kPhaseMax].
    // Phase is a closed enum class : u8 — the only legitimately constructible
    // values are the named enumerators (Input=1 .. Present=9).  Callers must
    // never fabricate a Phase via raw static_cast from an arbitrary integer.
    //
    // A debug assert fires on ordinal underflow (0) or overflow (>9) so that
    // any caller that bypasses the enum with a rogue cast is caught at test
    // time.  Release builds trust the closed-enum invariant and omit the
    // branch (NDEBUG defined).  The assert is kept as documentation even in
    // constexpr context — compilers evaluate it only in non-consteval paths.
    const auto ordinal = static_cast<std::uint8_t>(p);
    assert(ordinal >= kPhaseMin && ordinal <= kPhaseMax &&
           "phase_desc: Phase ordinal out of range [1,9]; caller supplied an "
           "invalid raw-cast value — use the named Phase enumerators only");
    // kPhaseTable is 0-indexed; Phase ordinals are 1..=9.
    return kPhaseTable[ordinal - 1u];
}

}  // namespace glibre::core
