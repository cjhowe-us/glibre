#pragma once
// core/src/asset/asset_registry.hpp
//
// AssetRegistry — process-wide singleton hosting per-T AssetTable instances.
//
// Authority: specs/core/SPEC.md §4.7, §6.8; plan #598.
//
// ## Responsibilities (SRP)
//
//   AssetRegistry is the single module responsible for:
//     1. Lazily instantiating one AssetTable<T> per payload type T on the
//        first insert() call for that type.
//     2. Routing insert/resolve/release calls to the correct AssetTable<T>.
//     3. Assigning a stable 2-bit type_tag to each registered type (max 4
//        types in MVP — §6.8 bit layout allocates 2 bits for type_tag).
//
//   It does NOT own I/O, asset loading, GPU resources, or the rules of
//   what bytes a handle resolves to (that is the resolving plugin's concern,
//   per SPEC §4.7 invariant 4).
//
// ## Singleton discipline
//
//   AssetRegistry::instance() returns a reference to the process-wide
//   singleton (Meyers singleton — initialized on first use, destroyed at
//   static-storage cleanup).  Per SPEC §4.7 invariant 3, exactly one
//   asset handle table lives per process.
//
// ## type_tag assignment
//
//   Each distinct T gets a type_tag assigned the first time a table for T
//   is created.  Assignment is sequential starting at 0.  If more than 4
//   types are registered the fifth call will assert (debug) or return a
//   clamped tag (release) — MVP supports at most 4 payload types.
//
//   Post-MVP: type_tag mapping will be driven by the TypeRegistry (issue #13);
//   for MVP the sequential counter is sufficient.
//
// ## Thread safety
//   Not thread-safe in MVP.  All insertions happen at bootstrap (single-thread
//   plugin registration).  SPEC §6.10 MVP: single worker thread; no contention
//   on the asset table.
//
// ## -fno-exceptions clean
//   All public methods are noexcept.

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#include <glibre/alloc.hpp>
#include <glibre/core/asset_handle.hpp>
#include <glibre/error.hpp>

#include "asset_table.hpp"

namespace glibre::core {

// ---------------------------------------------------------------------------
// Maximum number of distinct payload types supported by the 2-bit type_tag.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMaxAssetPayloadTypes = std::size_t{1}
                                                     << detail::kAssetTypeTagBits;  // 4

// ---------------------------------------------------------------------------
// AssetRegistry — process-wide singleton
// ---------------------------------------------------------------------------

class AssetRegistry {
public:
    // -------------------------------------------------------------------------
    // instance() — return the process-wide singleton.
    //
    // Thread safety: initialization is guaranteed by the C++11 standard for
    // function-local statics (executed exactly once, even under concurrent
    // calls).  For MVP, callers are single-threaded at bootstrap, so there is
    // no contention in practice.
    // -------------------------------------------------------------------------
    [[nodiscard]] static AssetRegistry& instance() noexcept {
        // The PerContextAllocator for the registry's own bookkeeping is also
        // a function-local static (initialized once).
        static PerContextAllocator s_alloc{ContextTag::core};
        static AssetRegistry s_instance{s_alloc};
        return s_instance;
    }

    // Non-copyable, non-movable — process-wide singleton.
    AssetRegistry(const AssetRegistry&) = delete;
    AssetRegistry& operator=(const AssetRegistry&) = delete;
    AssetRegistry(AssetRegistry&&) = delete;
    AssetRegistry& operator=(AssetRegistry&&) = delete;

    ~AssetRegistry() noexcept = default;

    // -------------------------------------------------------------------------
    // insert<T>(payload) — store a payload of type T and return a handle.
    //
    // On the first call for type T, a new AssetTable<T> is created and
    // assigned the next sequential type_tag.
    // -------------------------------------------------------------------------
    template<class T>
    [[nodiscard]] AssetHandle<T> insert(T payload) noexcept {
        return table_for<T>().insert(std::move(payload));  // forward as rvalue
    }

    // -------------------------------------------------------------------------
    // resolve<T>(handle) — resolve an AssetHandle<T> to a payload pointer.
    //
    // Returns core::Error::AssetStale on generation mismatch or out-of-range.
    // -------------------------------------------------------------------------
    template<class T>
    [[nodiscard]] Result<T*> resolve(AssetHandle<T> handle) noexcept {
        return table_for<T>().resolve(handle);
    }

    // -------------------------------------------------------------------------
    // release<T>(handle) — retire an AssetHandle<T>.
    // -------------------------------------------------------------------------
    template<class T>
    void release(AssetHandle<T> handle) noexcept {
        table_for<T>().release(handle);
    }

    // -------------------------------------------------------------------------
    // registered_type_count() — number of distinct payload types registered.
    //
    // Monotonically increasing; max kMaxAssetPayloadTypes (4).
    // Used in tests to verify per-T table instantiation.
    // -------------------------------------------------------------------------
    [[nodiscard]] std::size_t registered_type_count() const noexcept { return next_type_tag_; }

    // -------------------------------------------------------------------------
    // reset_for_testing() — clear all tables and reset the type counter.
    //
    // ONLY for use in unit tests.  Allows tests to call insert() on a clean
    // registry without contaminating type_tag assignments across test cases.
    //
    // Available only when GLIBRE_TESTING is defined.
    // -------------------------------------------------------------------------
#ifdef GLIBRE_TESTING
    void reset_for_testing() noexcept {
        for (auto& entry : tables_) {
            entry.reset();
        }
        next_type_tag_ = 0;
        // Reset type-token map so re-registration after reset assigns fresh
        // indices (MED-4 fix: previously s_tag_index was process-lifetime and
        // could produce UB casts if types were registered in a different order
        // post-reset).  Nulling the map entries invalidates all prior tag
        // assignments; the per-T kTypeToken statics remain live (they are
        // process-lifetime) and will be looked up fresh on next table_for<T>().
        for (auto& token : type_token_map_) {
            token = nullptr;
        }
    }
#endif

private:
    explicit AssetRegistry(PerContextAllocator& alloc) noexcept
        : alloc_{alloc} {}

    // -------------------------------------------------------------------------
    // table_for<T>() — return (creating if necessary) the AssetTable<T>.
    //
    // Type assignment is table-driven (type_index_map_ + next_type_tag_) so
    // that reset_for_testing() can clear all state and re-assign correctly.
    //
    // Previously, a per-T function-local static held the tag index.  That
    // design was un-resetable: after reset_for_testing(), a type registered
    // before the reset would still hold its old index, while post-reset
    // registrations would start from 0 — if different types landed at the
    // same index a UB cast would result.  The type_index_map_ array fixes
    // this: reset_for_testing() clears the map and next_type_tag_ together.
    // -------------------------------------------------------------------------
    template<class T>
    [[nodiscard]] AssetTable<T>& table_for() noexcept {
        // Per-T unique token: address of a function-local static char is stable
        // for the process lifetime and unique per template instantiation.
        // No RTTI required (project compiles with -fno-rtti).
        static const char kTypeToken = '\0';
        const void* const key = &kTypeToken;

        // Look up existing tag assignment.
        std::size_t tag_index = kUnassigned;
        for (std::size_t i = 0; i < next_type_tag_; ++i) {
            if (type_token_map_[i] == key) {
                tag_index = i;
                break;
            }
        }

        if (tag_index == kUnassigned) {
            // First call for this T — assign a new type_tag index.
            // Runtime guard (not assert-only): in release builds assert is a
            // no-op; std::abort() ensures we never silently proceed with an
            // out-of-bounds index (LOW-1 fix: release path must not fall
            // through to OOB table access).
            assert(
                next_type_tag_ < kMaxAssetPayloadTypes &&
                "AssetRegistry: more than 4 payload types registered (2-bit type_tag limit)"
            );
            if (next_type_tag_ >= kMaxAssetPayloadTypes) {
                // Release-build safety: abort rather than writing OOB.
                std::abort();
            }
            tag_index = next_type_tag_++;
            type_token_map_[tag_index] = key;
        }

        if (!tables_[tag_index].has_value()) {
            // First call or re-initialization after reset_for_testing().
            tables_[tag_index] =
                make_erased_table<T>(alloc_, static_cast<detail::AssetTypeTag>(tag_index));
        }

        // SAFETY: tag_index is valid and the slot is initialized.
        return *static_cast<AssetTable<T>*>(tables_[tag_index].ptr);
    }

    static constexpr std::size_t kUnassigned = ~std::size_t{0};

    // Backing allocator for all AssetTable storage.
    PerContextAllocator& alloc_;

    // Per-T table entries.
    //
    // Each AssetTable<T> header is allocated through alloc_ (MED-3 fix: the
    // AssetTable<T> object itself must be budget-counted, not just the inner
    // PMR vectors).  ErasedTable stores alloc_ + size so reset() can call
    // alloc_.deallocate() after destroying the table object.
    struct ErasedTable {
        using DestroyFn = void (*)(void*) noexcept;

        void* ptr{nullptr};
        DestroyFn destroy{nullptr};  // calls ~AssetTable<T>
        PerContextAllocator* alloc{nullptr};
        std::size_t alloc_size{0};

        ErasedTable() noexcept = default;

        ErasedTable(const ErasedTable&) = delete;
        ErasedTable& operator=(const ErasedTable&) = delete;

        ErasedTable(ErasedTable&& other) noexcept
            : ptr{other.ptr},
              destroy{other.destroy},
              alloc{other.alloc},
              alloc_size{other.alloc_size} {
            other.ptr = nullptr;
            other.destroy = nullptr;
            other.alloc = nullptr;
            other.alloc_size = 0;
        }

        ErasedTable& operator=(ErasedTable&& other) noexcept {
            if (this != &other) {
                reset();
                ptr = other.ptr;
                destroy = other.destroy;
                alloc = other.alloc;
                alloc_size = other.alloc_size;
                other.ptr = nullptr;
                other.destroy = nullptr;
                other.alloc = nullptr;
                other.alloc_size = 0;
            }
            return *this;
        }

        ~ErasedTable() noexcept { reset(); }

        void reset() noexcept {
            if (ptr) {
                if (destroy) {
                    destroy(ptr);
                }
                if (alloc) {
                    alloc->deallocate(ptr, alloc_size);
                }
            }
            ptr = nullptr;
            destroy = nullptr;
            alloc = nullptr;
            alloc_size = 0;
        }

        [[nodiscard]] bool has_value() const noexcept { return ptr != nullptr; }
    };

    // Allocate up to kMaxAssetPayloadTypes tables.
    std::array<ErasedTable, kMaxAssetPayloadTypes> tables_{};

    // Next type_tag to assign on first insert for a new type T.
    std::size_t next_type_tag_{0};

    // Type-token map: type_token_map_[i] is the address of the per-T static
    // `kTypeToken` char in table_for<T>().  Each template instantiation gets a
    // unique address (guaranteed by the standard for distinct statics), giving
    // us a stable RTTI-free per-type identity.  Populated alongside tables_ and
    // cleared by reset_for_testing() so the per-T static s_tag_index UB
    // (MED-4) cannot occur — the map is the authoritative tag assignment; the
    // per-T static `kTypeToken` address is merely the key, not the assignment.
    std::array<const void*, kMaxAssetPayloadTypes> type_token_map_{};

    // -------------------------------------------------------------------------
    // make_erased_table<T> — allocate AssetTable<T> through alloc_ and wrap.
    //
    // The AssetTable<T> object is placement-new'd into a raw buffer obtained
    // from alloc_ so the header bytes are budget-counted alongside the inner
    // PMR vectors (MED-3 fix: previously used bare `new`, escaping the ceiling).
    // -------------------------------------------------------------------------
    template<class T>
    static ErasedTable
    make_erased_table(PerContextAllocator& alloc, detail::AssetTypeTag tag) noexcept {
        constexpr std::size_t kSize = sizeof(AssetTable<T>);
        constexpr std::size_t kAlign = alignof(AssetTable<T>);

        auto result = alloc.allocate(kSize, kAlign);
        // allocate() aborts on OOM (PerContextAllocator contract); the Result
        // can only be unexpected on ceiling breach in GLIBRE_ALLOC_STRICT mode.
        // In that case we abort as well — no recovery for asset-table OOM.
        if (!result.has_value()) {
            std::abort();
        }
        void* raw = result.value();
        // Placement-new: construct AssetTable<T> in the allocated buffer.
        auto* p = ::new (raw) AssetTable<T>(alloc, tag);

        ErasedTable entry;
        entry.ptr = p;
        entry.destroy = [](void* raw_ptr) noexcept {
            static_cast<AssetTable<T>*>(raw_ptr)->~AssetTable<T>();
        };
        entry.alloc = &alloc;
        entry.alloc_size = kSize;
        return entry;
    }
};

}  // namespace glibre::core
