#pragma once
// core/src/asset/asset_table.hpp
//
// AssetTable<T> — generational slot-vector + free-list handle table.
//
// Authority: specs/core/SPEC.md §4.7, §6.8; plan #598.
//
// ## Responsibilities (SRP)
//
//   AssetTable<T> owns the assignment and retirement of AssetHandle<T> handles
//   for one payload type T.  It is the single module responsible for:
//     1. Issuing new AssetHandle<T> handles (insert).
//     2. Retiring AssetHandle<T> handles (release), bumping the slot's
//        generation to invalidate all outstanding handles to that slot.
//     3. Resolving an AssetHandle<T> to its payload pointer (resolve) —
//        returns core::Error::AssetStale on generation mismatch.
//
//   It does NOT own I/O, asset loading, GPU resources, or file paths.
//   Those belong to the resolving plugin per SPEC §4.7 invariant 4.
//
// ## Slot vector
//
//   A `std::pmr::vector<Slot<T>>` backed by PerContextAllocatorResource{core}
//   grows monotonically.  Slot entries are never compacted in MVP (SPEC §6.8);
//   the index is stable for the slot's lifetime.
//
//   Slot layout:
//     generation : uint32_t      — current generation; 0 means never allocated.
//     live       : bool          — true when the slot holds a live asset.
//     payload    : optional<T>   — engaged while live; disengaged after release.
//                                  optional avoids requiring T to be default-
//                                  constructible (SPEC §4.7 inv. 2).
//
// ## Free list
//
//   A `std::pmr::vector<uint32_t>` holds indices of released slots.
//   Reuse order is lowest-index-first (PHILOSOPHY §7: determinism).
//   The free list is kept sorted ascending by insertion in sorted order
//   on release.
//
// ## Generation invariant (§4.7 invariant 1 / §6.8)
//
//   Slot starts at generation 0 (never-issued state).
//   First insert into a slot: generation is set to 1; AssetHandle is issued
//   with generation 1.
//   Every release increments the generation BEFORE the slot enters the free
//   list — outstanding handles immediately become stale on release.
//   Reuse after release uses the already-incremented generation (does not
//   increment again on re-insert).
//
// ## Thread safety
//   Not thread-safe.  AssetTable is owned by AssetRegistry; AssetRegistry
//   guards access at process initialization (single-thread bootstrap per
//   SPEC §6.10 MVP single-thread note).
//
// ## -fno-exceptions clean
//   All public methods are noexcept.  std::pmr::vector may call std::abort()
//   on OOM (the PMR do_allocate contract under -fno-exceptions; see alloc.hpp).

#include <algorithm>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <vector>

#include <glibre/alloc.hpp>
#include <glibre/core/asset_handle.hpp>
#include <glibre/error.hpp>

namespace glibre::core {

// ---------------------------------------------------------------------------
// AssetTable<T>
// ---------------------------------------------------------------------------

template<class T>
class AssetTable {
public:
    /// Construct an AssetTable backed by the given PerContextAllocator.
    ///
    /// `type_tag` is the 2-bit tag embedded in every handle issued by this
    /// table.  It must be < 4 (fits in 2 bits).
    ///
    /// `alloc` must outlive this AssetTable (the PMR resource holds a reference
    /// to it).  Pass the engine's long-lived core-context allocator.
    explicit AssetTable(PerContextAllocator& alloc, detail::AssetTypeTag type_tag) noexcept
        : mr_{alloc},
          type_tag_{type_tag},
          slots_(&mr_),
          free_list_(&mr_) {}

    // Non-copyable, non-movable — holds a reference (via PMR resource) to the
    // backing allocator.
    AssetTable(const AssetTable&) = delete;
    AssetTable& operator=(const AssetTable&) = delete;
    AssetTable(AssetTable&&) = delete;
    AssetTable& operator=(AssetTable&&) = delete;

    ~AssetTable() noexcept = default;

    // -------------------------------------------------------------------------
    // insert(payload) — store a payload and issue a new AssetHandle<T>.
    //
    // If the free list is non-empty, the lowest-indexed free slot is reused
    // (PHILOSOPHY §7 determinism — lowest-free-index-first).  Otherwise a new
    // slot is appended to the slot vector.
    //
    // Generation rules:
    //   New slot:    generation is set to 1 (first issuance).
    //   Reused slot: generation is the value left by the preceding release
    //                (which already incremented it); it is NOT incremented here.
    //
    // Returns the new AssetHandle<T>.
    //
    // Signature: T&& (SPEC §6.8 stub contract — rvalue ref for move-only payloads).
    // -------------------------------------------------------------------------
    [[nodiscard]] AssetHandle<T> insert(T&& payload) noexcept {
        if (!free_list_.empty()) {
            // Reuse the lowest-indexed free slot.
            const auto idx = static_cast<detail::AssetIndex>(free_list_.front());
            // O(n) erase from front — shifts the tail. Acceptable for MVP:
            // SPEC §6.12.3 ceiling ~2^20 and single-threaded bootstrap (§6.10).
            free_list_.erase(free_list_.begin());

            auto& slot = slots_[static_cast<std::size_t>(idx)];
            slot.payload.emplace(std::move(payload));
            slot.live = true;
            // generation was already incremented on release; reuse it directly.
            return detail::asset_pack<T>(idx, slot.generation, type_tag_);
        }

        // Append a new slot.
        const auto idx = static_cast<detail::AssetIndex>(slots_.size());
        slots_.push_back(
            Slot{/*.generation=*/1, /*.live=*/true, std::optional<T>{std::move(payload)}}
        );
        return detail::asset_pack<T>(idx, 1u, type_tag_);
    }

    // -------------------------------------------------------------------------
    // resolve(handle) — validate and return a pointer to the stored payload.
    //
    // Returns core::Error::AssetStale when:
    //   - The handle's type_tag does not match this table's type_tag_ (SPEC
    //     §4.7 inv.1, §6.8 — a foreign-typed handle is treated as stale).
    //   - The handle's index is out of range.
    //   - The slot is not live (was released and not yet reused).
    //   - The handle's generation does not match the slot's current generation.
    // -------------------------------------------------------------------------
    [[nodiscard]] Result<T*> resolve(AssetHandle<T> handle) noexcept {
        auto [idx, gen, tag] = detail::asset_unpack(handle);
        if (static_cast<detail::AssetTypeTag>(tag) != type_tag_) {
            // Handle belongs to a different typed table — treat as stale.
            // SPEC §4.7 inv.1, §6.8: type_tag is part of handle identity.
            return std::unexpected(
                glibre::Error{
                    core::Error::AssetStale, ErrorContext{__FILE__, __LINE__, "type_tag mismatch"}
                }
            );
        }
        if (idx >= slots_.size()) {
            return std::unexpected(
                glibre::Error{
                    core::Error::AssetStale,
                    ErrorContext{__FILE__, __LINE__, "handle index out of range"}
                }
            );
        }
        auto& slot = slots_[static_cast<std::size_t>(idx)];
        if (!slot.live || slot.generation != gen) {
            return std::unexpected(
                glibre::Error{
                    core::Error::AssetStale,
                    ErrorContext{__FILE__, __LINE__, "generation mismatch or slot not live"}
                }
            );
        }
        return std::addressof(*slot.payload);
    }

    // -------------------------------------------------------------------------
    // release(handle) — retire an AssetHandle<T> slot.
    //
    // On a stale or out-of-range handle, this is a no-op (release is idempotent
    // for safety, matching despawn semantics for entity handles).
    //
    // On success:
    //   1. The slot's live flag is cleared.
    //   2. The slot's generation is incremented (making all outstanding handles
    //      for this slot immediately stale — §4.7 invariant 1, §6.8).
    //   3. The slot index is inserted into the free list in ascending sorted
    //      order (lowest-index-first reuse — PHILOSOPHY §7 determinism).
    // -------------------------------------------------------------------------
    void release(AssetHandle<T> handle) noexcept {
        auto [idx, gen, tag] = detail::asset_unpack(handle);
        if (static_cast<detail::AssetTypeTag>(tag) != type_tag_) {
            return;  // foreign-typed handle: no-op (SPEC §4.7 inv.1)
        }
        if (idx >= slots_.size()) {
            return;  // out-of-range: no-op
        }
        auto& slot = slots_[static_cast<std::size_t>(idx)];
        if (!slot.live || slot.generation != gen) {
            return;  // stale: no-op
        }
        slot.live = false;
        slot.payload.reset();  // destroy payload; slot is no longer live
        // Guard against 22-bit generation overflow (SPEC §6.8, §6.12.3).
        // After 2^22 release/reinsert cycles the slot is permanently retired —
        // leaked rather than silently reissued with a colliding generation.
        // MVP asset ceiling ~2^20 means saturation is pathological; compaction
        // is deferred to a post-MVP spike (SPEC §6.12.3).
        if (slot.generation >= detail::kAssetGenerationMax) {
            // Permanently retire this slot; do not return it to the free list.
            return;
        }
        ++slot.generation;  // invalidate outstanding handles immediately
        // Insert into free list in sorted ascending order.
        // O(n) per insert — acceptable for MVP (SPEC §6.12.3 ceiling ~2^20).
        const auto pos =
            std::lower_bound(free_list_.begin(), free_list_.end(), static_cast<std::uint32_t>(idx));
        free_list_.insert(pos, static_cast<std::uint32_t>(idx));
    }

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
    // Slot — per-slot state in the slot vector (SPEC §6.8 Slot layout).
    //
    // payload is std::optional<T> rather than T{} to avoid requiring
    // T to be default-constructible (SPEC §4.7 inv. 2: insert(T&&) contract
    // permits move-only types).  payload is engaged by insert() via emplace()
    // and disengaged by release() via reset(), so the storage is free when
    // the slot is not live.
    // -------------------------------------------------------------------------
    struct Slot {
        detail::AssetGeneration generation{0};  // 0 = never allocated
        bool live{false};
        std::optional<T> payload{std::nullopt};
    };

    // PMR resource — must be declared before slots_ and free_list_ so that it
    // is constructed first and destroyed last (member-initialization order).
    PerContextAllocatorResource mr_;

    // 2-bit type tag embedded in every issued handle.
    detail::AssetTypeTag type_tag_;

    // Slot vector — grows monotonically; never shrinks (SPEC §6.8).
    std::pmr::vector<Slot> slots_;

    // Free list — sorted ascending by slot index (lowest-index-first reuse).
    std::pmr::vector<std::uint32_t> free_list_;
};

}  // namespace glibre::core
