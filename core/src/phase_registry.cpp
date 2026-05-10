// core/src/phase_registry.cpp
//
// Implementation of glibre::core::PhaseRegistry.
//
// Authority: plan #245, reviews/decisions/frame-phases.md §Decision.
//
// Storage layout:
//   A std::array<eastl::vector<SystemEntry>, kPhaseCount> (9 elements)
//   keyed by static_cast<std::size_t>(phase) - 1.
//   Iteration order within each phase is registration order (push_back order).
//
// Idempotency (hot-reload-protocol.md §4.1):
//   register_system() performs a linear scan of the existing entries for the
//   given phase.  If an entry with matching fqn exists, the call is a no-op.
//   Expected n < 32 per phase in MVP, so O(n) is acceptable.
//
// No-exceptions contract:
//   All public methods are noexcept; EASTL never throws when glibre is built
//   with -fno-exceptions.

#include "glibre/core/phase_registry.hpp"

#include <cstddef>

#include <EASTL/string_view.h>

namespace glibre::core {

// ---------------------------------------------------------------------------
// register_system — add a system to a phase slot.
// ---------------------------------------------------------------------------

void PhaseRegistry::register_system(Phase phase, eastl::string_view fqn, SystemFn fn) noexcept {
    // NOLINT: pro-bounds-*-array-index — phase_index() returns [0, kPhaseCount-1]
    // by construction from the closed-enum Phase (see phase_registry.hpp rationale).
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    auto& list = systems_[phase_index(phase)];
    // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

    // Idempotency check: scan for an existing entry with the same fqn.
    // O(n) linear scan; expected n < 32 per phase in MVP (frame-phases.md).
    for (const SystemEntry& entry : list) {
        if (entry.fqn == fqn) {
            // Already registered — no-op per hot-reload-protocol.md §4.1.
            return;
        }
    }

    // First registration: append a new entry.
    list.emplace_back(fqn, eastl::move(fn));
}

// ---------------------------------------------------------------------------
// drain — remove all registered systems from every phase.
// ---------------------------------------------------------------------------

void PhaseRegistry::drain() noexcept {
    for (auto& list : systems_) {
        list.clear();
    }
}

// ---------------------------------------------------------------------------
// total_system_count — sum of all per-phase system counts.
// ---------------------------------------------------------------------------

std::size_t PhaseRegistry::total_system_count() const noexcept {
    std::size_t total = 0;
    for (const auto& list : systems_) {
        total += list.size();
    }
    return total;
}

}  // namespace glibre::core
