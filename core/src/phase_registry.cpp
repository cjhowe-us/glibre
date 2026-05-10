// core/src/phase_registry.cpp
//
// Implementation of glibre::core::PhaseRegistry.
//
// Authority: plan #245, reviews/decisions/frame-phases.md §Decision.
//            reviews/decisions/eastl-removal.md §1 (matrix rows 1-3, 9),
//            §3 (PerContextAllocatorResource PMR adapter shape).
//
// Storage layout:
//   A std::array<std::pmr::vector<SystemEntry>, kPhaseCount> (9 elements)
//   keyed by static_cast<std::size_t>(phase) - 1.
//   Iteration order within each phase is registration order (push_back order).
//   PMR vectors are backed by a std::pmr::memory_resource* provided at
//   construction (in engine code: a PerContextAllocatorResource{ContextTag::core}
//   so all system-entry heap allocations are tracked under the core context
//   ceiling per perf-budget.md §Allocator Rules #1).
//
// Idempotency (hot-reload-protocol.md §4.1):
//   register_system() performs a linear scan of the existing entries for the
//   given phase.  If an entry with matching fqn exists, the call is a no-op.
//   Expected n < 32 per phase in MVP, so O(n) is acceptable.
//
// No-exceptions contract:
//   All public methods are noexcept.  PMR containers route through
//   the provided memory_resource, which in engine builds calls std::abort()
//   on ceiling breach in GLIBRE_ALLOC_STRICT builds (the PMR interface cannot
//   return failure without throwing, and the engine compiles -fno-exceptions).
//
// Migration note (plan #1045):
//   EASTL types replaced per reviews/decisions/eastl-removal.md:
//     eastl::vector<SystemEntry>        -> std::pmr::vector<SystemEntry>
//     eastl::string                     -> std::pmr::string
//     eastl::string_view                -> std::string_view
//     eastl::fixed_function<64, void()> -> std::move_only_function<void()>
//     eastl::move                       -> std::move

#include "glibre/core/phase_registry.hpp"

#include <cstddef>
#include <string_view>
#include <utility>

namespace glibre::core {

// ---------------------------------------------------------------------------
// PhaseRegistry::PhaseRegistry — construct with PMR resource backing.
//
// Initialises each per-phase std::pmr::vector with a pointer to the given
// memory resource so that all system-entry allocations are tracked through
// whichever resource the caller provides (in engine code: a
// PerContextAllocatorResource for the core context; in tests: a monotonic
// buffer resource or the default heap resource).
//
// Precondition: mr must not be null and must outlive this PhaseRegistry.
// ---------------------------------------------------------------------------

PhaseRegistry::PhaseRegistry(std::pmr::memory_resource* mr) noexcept
    : mr_{mr},
      systems_{
          std::pmr::vector<SystemEntry>{mr},
          std::pmr::vector<SystemEntry>{mr},
          std::pmr::vector<SystemEntry>{mr},
          std::pmr::vector<SystemEntry>{mr},
          std::pmr::vector<SystemEntry>{mr},
          std::pmr::vector<SystemEntry>{mr},
          std::pmr::vector<SystemEntry>{mr},
          std::pmr::vector<SystemEntry>{mr},
          std::pmr::vector<SystemEntry>{mr},
      } {}

// ---------------------------------------------------------------------------
// register_system — add a system to a phase slot.
// ---------------------------------------------------------------------------

void PhaseRegistry::register_system(Phase phase, std::string_view fqn, PhaseSystemFn fn) noexcept {
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
    // PMR string is constructed with mr_ as the allocator so the FQN copy
    // is tracked through the same resource as the vector elements.
    list.emplace_back(fqn, std::move(fn), mr_);
}

// ---------------------------------------------------------------------------
// drain_all — remove all registered systems from every phase (coarse clear).
//
// SCOPE: MVP shutdown and unit-test fixtures only; NOT the per-plugin hot-
// reload drain (hot-reload-protocol.md §Step 1 is per-plugin, not global).
// See header doc-comment for the full rationale.
// ---------------------------------------------------------------------------

void PhaseRegistry::drain_all() noexcept {
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
