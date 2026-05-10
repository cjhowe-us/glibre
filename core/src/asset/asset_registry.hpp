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
#include <cstdint>

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
        return table_for<T>().insert(std::move(payload));
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
    }
#endif

private:
    explicit AssetRegistry(PerContextAllocator& alloc) noexcept
        : alloc_{alloc} {}

    // -------------------------------------------------------------------------
    // table_for<T>() — return (creating if necessary) the AssetTable<T>.
    //
    // Uses a function-template-local static index to locate the entry in the
    // tables_ array.  The index is assigned the first time table_for<T>() is
    // called for a given T; subsequent calls return the cached entry.
    //
    // Implementation note: We use a per-T function-local atomic_bool + index
    // to perform type registration.  In MVP single-threaded bootstrap this is
    // simple and correct.
    // -------------------------------------------------------------------------
    template<class T>
    [[nodiscard]] AssetTable<T>& table_for() noexcept {
        // Static per-T slot index — assigned on first call for a given T.
        // kUnassigned sentinel: tables_.size() is at most kMaxAssetPayloadTypes
        // (4), so using ~0u as "unassigned" is safe.
        //
        // After reset_for_testing() the slot ptr is nulled but s_tag_index
        // retains its value.  We detect this case by checking ptr and
        // re-allocating the table at the same index.
        static std::size_t s_tag_index = kUnassigned;

        if (s_tag_index == kUnassigned) {
            // First call ever for this T — assign a new type_tag index.
            assert(
                next_type_tag_ < kMaxAssetPayloadTypes &&
                "AssetRegistry: more than 4 payload types registered (2-bit type_tag limit)"
            );
            s_tag_index = next_type_tag_++;
        }

        if (!tables_[s_tag_index].has_value()) {
            // Either first call or re-initialization after reset_for_testing().
            // In the reset case, s_tag_index is already assigned; we simply
            // re-create the table at the same index without incrementing
            // next_type_tag_ again.
            if (s_tag_index >= next_type_tag_) {
                // Re-registration after reset: bump counter back.
                next_type_tag_ = s_tag_index + 1;
            }
            tables_[s_tag_index] =
                make_erased_table<T>(alloc_, static_cast<detail::AssetTypeTag>(s_tag_index));
        }

        // SAFETY: s_tag_index is valid and the slot is initialized.
        return *static_cast<AssetTable<T>*>(tables_[s_tag_index].ptr);
    }

    static constexpr std::size_t kUnassigned = ~std::size_t{0};

    // Backing allocator for all AssetTable storage.
    PerContextAllocator& alloc_;

    // Per-T table entries (type-erased via void-deleter unique_ptr wrapper).
    // We store them as unique_ptr<void, void(*)(void*)> so that each table's
    // destructor is called correctly despite type erasure.
    //
    // Implementation: We use a custom deleter that casts back to the concrete
    // type.  The concrete type is baked into the deleter at construction time.
    struct ErasedTable {
        void* ptr{nullptr};
        void (*deleter)(void*){nullptr};

        ErasedTable() noexcept = default;

        ErasedTable(const ErasedTable&) = delete;
        ErasedTable& operator=(const ErasedTable&) = delete;

        ErasedTable(ErasedTable&& other) noexcept
            : ptr{other.ptr},
              deleter{other.deleter} {
            other.ptr = nullptr;
            other.deleter = nullptr;
        }

        ErasedTable& operator=(ErasedTable&& other) noexcept {
            if (this != &other) {
                reset();
                ptr = other.ptr;
                deleter = other.deleter;
                other.ptr = nullptr;
                other.deleter = nullptr;
            }
            return *this;
        }

        ~ErasedTable() noexcept { reset(); }

        void reset() noexcept {
            if (ptr && deleter) {
                deleter(ptr);
            }
            ptr = nullptr;
            deleter = nullptr;
        }

        [[nodiscard]] bool has_value() const noexcept { return ptr != nullptr; }
    };

    // Allocate up to kMaxAssetPayloadTypes tables.
    std::array<ErasedTable, kMaxAssetPayloadTypes> tables_{};

    // Next type_tag to assign on first insert for a new type T.
    std::size_t next_type_tag_{0};

    // -------------------------------------------------------------------------
    // make_unique helper — constructs AssetTable<T> and wraps in ErasedTable.
    // -------------------------------------------------------------------------
    template<class T>
    static ErasedTable
    make_erased_table(PerContextAllocator& alloc, detail::AssetTypeTag tag) noexcept {
        auto* p = new AssetTable<T>(alloc, tag);  // NOLINT(cppcoreguidelines-owning-memory)
        ErasedTable entry;
        entry.ptr = p;
        entry.deleter = [](void* raw) noexcept {
            delete static_cast<AssetTable<T>*>(raw);  // NOLINT
        };
        return entry;
    }
};

}  // namespace glibre::core
