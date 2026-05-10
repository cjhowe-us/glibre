// core/src/world/entity_allocator.cpp
//
// EntityAllocator implementation — generational slot-vector + free-list.
//
// Authority: specs/core/SPEC.md §4.1, §4.3; plan #557.
// See entity_allocator.hpp for the full design rationale.

#include "entity_allocator.hpp"

#include <algorithm>  // std::lower_bound
#include <cstdint>
#include <utility>

#include <glibre/alloc.hpp>
#include <glibre/core/entity.hpp>
#include <glibre/error.hpp>

namespace glibre::core {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

EntityAllocator::EntityAllocator(PerContextAllocator& alloc) noexcept
    : mr_{alloc},
      slots_{&mr_},
      free_list_{&mr_},
      alive_count_{0} {}

// ---------------------------------------------------------------------------
// spawn()
// ---------------------------------------------------------------------------

Result<Entity> EntityAllocator::spawn() noexcept {
    if (!free_list_.empty()) {
        // Reuse the lowest-indexed free slot.
        const detail::EntityIndex idx = free_list_.front();
        free_list_.erase(free_list_.begin());

        // Generation was already incremented at despawn time; reuse it as-is.
        Slot& slot = slots_[idx];
        slot.alive = true;
        ++alive_count_;

        return detail::pack(idx, slot.generation);
    }

    // No free slot — append a new one.
    const auto idx = static_cast<detail::EntityIndex>(slots_.size());
    Slot& slot = slots_.emplace_back();
    slot.generation = 1;  // First issuance: generation 0 is "never allocated".
    slot.alive = true;
    ++alive_count_;

    return detail::pack(idx, slot.generation);
}

// ---------------------------------------------------------------------------
// despawn()
// ---------------------------------------------------------------------------

Result<void> EntityAllocator::despawn(Entity e) noexcept {
    // Validate via resolve — reuses the common validation logic.
    GLIBRE_TRY(idx, resolve(e));

    Slot& slot = slots_[idx];

    // Mark as dead and increment generation (makes the old handle stale).
    slot.alive = false;
    ++slot.generation;
    --alive_count_;

    // Insert into the free list in sorted order (lowest-index-first per
    // PHILOSOPHY §7 determinism).  std::lower_bound finds the insertion point
    // in O(N) for a sorted pmr::vector; acceptable for MVP entity counts.
    const auto pos = std::lower_bound(free_list_.begin(), free_list_.end(), idx);
    free_list_.insert(pos, idx);

    return {};  // Result<void> success
}

// ---------------------------------------------------------------------------
// is_alive()
// ---------------------------------------------------------------------------

bool EntityAllocator::is_alive(Entity e) const noexcept {
    auto [idx, gen] = detail::unpack(e);
    if (idx >= static_cast<detail::EntityIndex>(slots_.size())) {
        return false;
    }
    const Slot& slot = slots_[idx];
    return slot.alive && slot.generation == gen;
}

// ---------------------------------------------------------------------------
// resolve()
// ---------------------------------------------------------------------------

Result<detail::SlotIndex> EntityAllocator::resolve(Entity e) const noexcept {
    auto [idx, gen] = detail::unpack(e);
    if (idx >= static_cast<detail::EntityIndex>(slots_.size())) {
        return std::unexpected(glibre::Error{core::Error::EntityStale});
    }
    const Slot& slot = slots_[idx];
    if (!slot.alive || slot.generation != gen) {
        return std::unexpected(glibre::Error{core::Error::EntityStale});
    }
    return idx;
}

}  // namespace glibre::core
