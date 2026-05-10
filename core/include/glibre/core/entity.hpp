#pragma once
// core/include/glibre/core/entity.hpp
//
// Entity — opaque 64-bit generational handle.
//
// Authority: specs/core/SPEC.md §4.3, §5.2; plan #557.
//
// ## Design
//
//   Entity is a value object.  It carries a 64-bit integer (`bits`) that
//   encodes a 32-bit slot index in the low half and a 32-bit generation
//   counter in the high half.  The split is private to `core::detail` so
//   that public callers never depend on the physical bit layout (SPEC §4.3
//   invariant 2: opacity).
//
//   Packing layout:
//     bits[31: 0] = slot index    (low 32 bits)
//     bits[63:32] = generation    (high 32 bits)
//
//   Rationale for index-low / generation-high: a zero-initialised Entity
//   (bits == 0) corresponds to slot 0, generation 0, which the allocator
//   reserves as the "null" / unallocated state (generation 0 is never issued
//   to a caller — the first spawned entity at any slot starts at generation 1).
//   This means a default-constructed Entity{} is always stale, which is the
//   safe default (SPEC §4.3 invariant 1).
//
// ## -fno-exceptions clean
//   All functions are `noexcept`.  No exceptions thrown or propagated.
//
// ## Public surface
//   `struct Entity { uint64_t bits; }` with `operator==`.
//   `detail::pack(index, generation)` and `detail::unpack(entity)` are
//   restricted to `core::detail` — not part of the public plugin ABI.

#include <cstdint>
#include <utility>

namespace glibre::core {

// ---------------------------------------------------------------------------
// Entity — opaque 64-bit generational handle (SPEC §4.3, §5.2)
// ---------------------------------------------------------------------------

struct Entity {
    std::uint64_t bits{};

    // Equality compares the raw bits (SPEC §5.2): two Entity values are equal
    // iff they encode the same (index, generation) pair.
    friend constexpr bool operator==(Entity, Entity) noexcept = default;

    /// Test-only factory: constructs Entity from raw (index, generation) bits.
    [[nodiscard]] static constexpr Entity from_bits_for_testing(
        std::uint32_t index,
        std::uint32_t generation
    ) noexcept {
        return detail::pack(index, generation);
    }
};

// ---------------------------------------------------------------------------
// core::detail — internal packing helpers (NOT part of public plugin ABI)
//
// These helpers are used by EntityAllocator and (eventually) World to pack
// and unpack the index/generation split.  Public plugin code never calls them;
// they are in a sub-namespace rather than a private-class scope so that
// implementation files can access them without a forward-declared friend.
//
// Layout:  bits[31:0] = index  |  bits[63:32] = generation
// ---------------------------------------------------------------------------

namespace detail {

/// The underlying integer type for both the slot index and the generation.
/// Using uint32_t keeps the packing to exactly 64 bits.
using EntityIndex = std::uint32_t;
using EntityGeneration = std::uint32_t;

/// SlotIndex — plain alias used in resolve() return types and Archetype
/// addressing.  Kept as a distinct name (not EntityIndex) to avoid confusing
/// the packed slot coordinate with arbitrary index arithmetic.
using SlotIndex = EntityIndex;

/// Pack a (index, generation) pair into an Entity's bits.
///
/// index      — slot index in the EntityAllocator's slot vector.
/// generation — current generation counter for that slot.
///
/// Precondition: both values fit in uint32_t (trivially true by type).
[[nodiscard]] constexpr Entity pack(EntityIndex index, EntityGeneration generation) noexcept {
    return Entity{
        (static_cast<std::uint64_t>(generation) << 32U) | static_cast<std::uint64_t>(index)
    };
}

/// Unpack the (index, generation) pair from an Entity's bits.
///
/// Returns {index, generation} in that order.
[[nodiscard]] constexpr std::pair<EntityIndex, EntityGeneration> unpack(Entity e) noexcept {
    auto index = static_cast<EntityIndex>(e.bits & 0xFFFF'FFFFULL);
    auto generation = static_cast<EntityGeneration>(e.bits >> 32U);
    return {index, generation};
}

}  // namespace detail

}  // namespace glibre::core
