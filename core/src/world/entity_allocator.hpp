#pragma once
// core/src/world/entity_allocator.hpp
//
// EntityAllocator — generational slot-vector + free-list allocator.
//
// Authority: specs/core/SPEC.md §4.1, §4.3; plan #557.
//
// ## Responsibilities (SRP)
//
//   EntityAllocator owns the assignment of Entity handles.  It is the single
//   module responsible for:
//     1. Issuing new Entity handles (spawn).
//     2. Retiring Entity handles (despawn), incrementing the slot's generation.
//     3. Resolving an Entity to its slot index (resolve) — used internally by
//        World to look up the archetype row for an entity.
//     4. Answering liveness queries (is_alive).
//
//   It does NOT own component storage, archetypes, or World-level invariants.
//   Those belong to the World aggregate (next plan).
//
// ## Slot vector
//
//   A `std::pmr::vector<Slot>` backed by PerContextAllocatorResource{core}
//   grows monotonically.  Slot entries are never removed; generation counters
//   distinguish live from stale handles for a given slot index.
//
//   Slot layout:
//     generation : uint32_t — current generation; 0 means never allocated.
//     alive      : bool     — true when the slot holds a live entity.
//
// ## Free list
//
//   A `std::pmr::vector<uint32_t>` holds indices of despawned slots.
//   Reuse order is lowest-index-first (PHILOSOPHY §7: determinism).
//   The free list is kept sorted by insertion of the despawned index in the
//   correct position using a linear scan.  For MVP entity counts this is
//   acceptable; a priority_queue or sorted insert would be used for larger
//   populations.
//
//   Generation is incremented when the slot is despawned (before it enters
//   the free list).  On reuse the same incremented generation is used in the
//   new Entity handle so that old handles are stale immediately on despawn,
//   not on re-spawn.
//
// ## Generation invariant
//
//   Slot starts at generation 0 (never-issued).
//   First spawn of a slot: generation is incremented to 1; Entity is issued
//   with generation 1.
//   Every despawn increments the generation.
//   Reuse does NOT increment again; the generation after despawn is reused
//   for the new handle.  The old handle (generation N) is therefore stale
//   the moment despawn is called (resolve returns EntityStale for it).
//
// ## Error arms
//
//   resolve() returns core::Error::EntityStale when the entity's generation
//   does not match the slot's current generation, or when the slot's alive
//   flag is false.
//
// ## -fno-exceptions clean
//   All public methods are noexcept.  std::pmr::vector may call std::abort()
//   on OOM (the PMR do_allocate contract under -fno-exceptions; see alloc.hpp).
//
// ## Thread safety
//   Not thread-safe.  EntityAllocator is owned by World; World-level locking
//   (a separate concern) guards all mutations.

#include <cstdint>
#include <memory_resource>
#include <vector>

#include <glibre/alloc.hpp>
#include <glibre/core/entity.hpp>
#include <glibre/error.hpp>

namespace glibre::core {

// ---------------------------------------------------------------------------
// EntityAllocator
// ---------------------------------------------------------------------------

class EntityAllocator {
public:
    /// Construct an EntityAllocator backed by the given PerContextAllocator.
    ///
    /// `alloc` must outlive this EntityAllocator (the PMR resource holds a
    /// reference to it).  Pass the engine's long-lived core-context allocator.
    explicit EntityAllocator(PerContextAllocator& alloc) noexcept;

    // Non-copyable, non-movable — holds a reference (via PMR resource) to the
    // backing allocator.
    EntityAllocator(const EntityAllocator&) = delete;
    EntityAllocator& operator=(const EntityAllocator&) = delete;
    EntityAllocator(EntityAllocator&&) = delete;
    EntityAllocator& operator=(EntityAllocator&&) = delete;

    ~EntityAllocator() noexcept = default;

    // -------------------------------------------------------------------------
    // spawn() — issue a new Entity handle.
    //
    // If the free list is non-empty, the lowest-indexed free slot is reused.
    // Otherwise a new slot is appended to the slot vector.
    //
    // Generation rules:
    //   New slot:   generation is set to 1 (first issuance).
    //   Reused slot: generation is the value left by the preceding despawn
    //                (which already incremented it); it is NOT incremented again
    //                here.
    //
    // Returns the new Entity handle.  The return type is Result<Entity> to
    // keep the door open for OutOfBudget (GLIBRE_ALLOC_STRICT vector growth)
    // in future; currently always returns success for non-OOM conditions.
    //
    // On OOM: std::abort() via the PMR contract (no Result<> unwinding needed).
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<Entity> spawn() noexcept;

    // -------------------------------------------------------------------------
    // despawn(e) — retire an Entity handle.
    //
    // Precondition: `e` must be alive (is_alive(e) == true).  Calling despawn
    // on a stale or foreign handle returns core::Error::EntityStale.
    //
    // On success:
    //   1. The slot's alive flag is cleared.
    //   2. The slot's generation is incremented (making the old handle stale).
    //   3. The slot index is inserted into the free list in sorted order
    //      (lowest-index-first per PHILOSOPHY §7 determinism).
    //
    // Returns Result<void>.
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<void> despawn(Entity e) noexcept;

    // -------------------------------------------------------------------------
    // is_alive(e) — check liveness without returning an error.
    //
    // Returns true iff the entity's slot exists and alive flag is set and
    // generation matches.  Always returns false for a default-constructed
    // Entity{} (bits == 0, generation == 0).
    // -------------------------------------------------------------------------
    [[nodiscard]] bool is_alive(Entity e) const noexcept;

    // -------------------------------------------------------------------------
    // resolve(e) — validate and return the slot index for internal use.
    //
    // Used by World to find the archetype row for a given Entity.  Returns the
    // SlotIndex (a uint32_t) on success, or core::Error::EntityStale if the
    // entity is dead or its generation does not match.
    //
    // This is an internal API: the return type is not exposed in the public
    // plugin ABI (plugin surfaces cross the boundary as Entity opaque handles).
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<detail::SlotIndex> resolve(Entity e) const noexcept;

    // -------------------------------------------------------------------------
    // alive_count() — number of currently-live entities.
    //
    // O(1): maintained as a counter on spawn/despawn.  Useful for tests and
    // telemetry; not part of the public plugin ABI.
    // -------------------------------------------------------------------------
    [[nodiscard]] std::uint32_t alive_count() const noexcept { return alive_count_; }

    // -------------------------------------------------------------------------
    // slot_count() — total number of allocated slots (live + free).
    //
    // Monotonically increasing.  Used in tests to verify slot growth.
    // -------------------------------------------------------------------------
    [[nodiscard]] std::uint32_t slot_count() const noexcept {
        return static_cast<std::uint32_t>(slots_.size());
    }

private:
    // -------------------------------------------------------------------------
    // Slot — per-slot state in the slot vector.
    // -------------------------------------------------------------------------
    struct Slot {
        detail::EntityGeneration generation{0};  // 0 = never allocated
        bool alive{false};
    };

    // PMR resource — must be declared before slots_ and free_list_ so that it
    // is constructed first and destroyed last (member-initialization order).
    PerContextAllocatorResource mr_;

    // Slot vector — grows monotonically; never shrinks.
    std::pmr::vector<Slot> slots_;

    // Free list — sorted ascending by slot index (lowest-index-first reuse).
    std::pmr::vector<detail::EntityIndex> free_list_;

    // Live-entity counter — incremented on spawn, decremented on despawn.
    std::uint32_t alive_count_{0};
};

}  // namespace glibre::core
