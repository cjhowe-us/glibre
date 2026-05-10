// tests/core/asset_table/asset_table_test.cpp
//
// Catch2 unit tests for glibre::core::AssetTable<T> and AssetRegistry.
//
// Authority: specs/core/SPEC.md §4.7, §6.8; plan #598 Unit Test Plan.
//
// Named test cases (plan #598 Unit Test Plan + DoD):
//   - core/asset_table: insert_returns_unique_handle
//   - core/asset_table: release_increments_generation
//   - core/asset_table: stale_handle_resolves_to_AssetStale
//   - core/asset_table: free_index_reused_lowest_first
//   - core/asset_registry: per_T_table_instantiated_on_first_insert
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS usage.
//   - AssetTable<T> and AssetRegistry are internal (core/src/asset/); this
//     test target links against the source directory directly.

#include <cstdint>
#include <variant>

#include <catch2/catch_test_macros.hpp>
#include <glibre/alloc.hpp>
#include <glibre/error.hpp>

// AssetTable and AssetRegistry live in core/src/asset/ (internal).
// The CMakeLists.txt for this test target adds the source path as an include.
#include "asset_registry.hpp"
#include "asset_table.hpp"

// ===========================================================================
// Test: core/asset_table: insert_returns_unique_handle
//
// Verifies that two successive insert() calls return handles with different
// bits (unique handles per slot assignment).
//
// Also verifies that a freshly inserted handle resolves to the stored payload.
// ===========================================================================

TEST_CASE("core/asset_table: insert_returns_unique_handle", "[core][asset_table]") {
    using namespace glibre::core;

    glibre::PerContextAllocator alloc{glibre::ContextTag::core, 1024 * 1024};  // 1 MiB test ceiling
    AssetTable<int> table{alloc, detail::AssetTypeTag{0}};

    auto h1 = table.insert(42);
    auto h2 = table.insert(99);

    // Handles must be distinct (different slot indices).
    CHECK(!(h1 == h2));

    // resolve() must return the stored value.
    auto r1 = table.resolve(h1);
    REQUIRE(r1.has_value());
    CHECK(*r1.value() == 42);

    auto r2 = table.resolve(h2);
    REQUIRE(r2.has_value());
    CHECK(*r2.value() == 99);
}

// ===========================================================================
// Test: core/asset_table: release_increments_generation
//
// Verifies that after release(), the slot's generation is incremented so that
// a re-inserted slot has a different handle than the released one.
//
// Specifically: insert → release → re-insert (reuses same slot, bumped gen)
// The old handle must now be stale.
// ===========================================================================

TEST_CASE("core/asset_table: release_increments_generation", "[core][asset_table]") {
    using namespace glibre::core;

    glibre::PerContextAllocator alloc{glibre::ContextTag::core, 1024 * 1024};
    AssetTable<int> table{alloc, detail::AssetTypeTag{0}};

    auto h1 = table.insert(10);
    table.release(h1);

    // Reinsert — should reuse slot 0 with generation+1.
    auto h2 = table.insert(20);

    // h2 is for the same slot but a different generation — not equal to h1.
    CHECK(!(h1 == h2));

    // h1 must be stale now.
    auto r1 = table.resolve(h1);
    REQUIRE(!r1.has_value());
    CHECK(std::holds_alternative<glibre::core::Error>(r1.error().code()));
    CHECK(std::get<glibre::core::Error>(r1.error().code()) == glibre::core::Error::AssetStale);

    // h2 resolves correctly.
    auto r2 = table.resolve(h2);
    REQUIRE(r2.has_value());
    CHECK(*r2.value() == 20);
}

// ===========================================================================
// Test: core/asset_table: stale_handle_resolves_to_AssetStale
//
// Verifies that resolve() returns core::Error::AssetStale when:
//   (a) a released handle is resolved after its slot was reused.
//   (b) a handle with an out-of-range index is resolved.
// ===========================================================================

TEST_CASE("core/asset_table: stale_handle_resolves_to_AssetStale", "[core][asset_table]") {
    using namespace glibre::core;

    glibre::PerContextAllocator alloc{glibre::ContextTag::core, 1024 * 1024};
    AssetTable<int> table{alloc, detail::AssetTypeTag{0}};

    // (a) Released handle → AssetStale.
    auto h = table.insert(5);
    table.release(h);

    auto r = table.resolve(h);
    REQUIRE(!r.has_value());
    CHECK(std::holds_alternative<glibre::core::Error>(r.error().code()));
    CHECK(std::get<glibre::core::Error>(r.error().code()) == glibre::core::Error::AssetStale);

    // (b) Default-constructed handle (index=0, generation=0, type_tag=0).
    //     generation 0 is never issued, so this must be stale.
    AssetHandle<int> zero_handle{};
    auto r2 = table.resolve(zero_handle);
    REQUIRE(!r2.has_value());
    CHECK(std::get<glibre::core::Error>(r2.error().code()) == glibre::core::Error::AssetStale);

    // (c) Handle with index well beyond slot_count.
    auto h_oob = detail::asset_pack<int>(detail::AssetIndex{9999}, 1u, detail::AssetTypeTag{0});
    auto r3 = table.resolve(h_oob);
    REQUIRE(!r3.has_value());
    CHECK(std::get<glibre::core::Error>(r3.error().code()) == glibre::core::Error::AssetStale);
}

// ===========================================================================
// Test: core/asset_table: free_index_reused_lowest_first
//
// Verifies that the free list is ordered lowest-index-first (PHILOSOPHY §7
// determinism) when slots are released out of order.
//
// Procedure:
//   1. Insert three payloads → handles h0, h1, h2 (slot indices 0, 1, 2).
//   2. Release h2 (index 2) then h0 (index 0) — out-of-order.
//   3. Insert two new payloads → expect them to occupy slot 0 then slot 2
//      (lowest free first).
// ===========================================================================

TEST_CASE("core/asset_table: free_index_reused_lowest_first", "[core][asset_table]") {
    using namespace glibre::core;

    glibre::PerContextAllocator alloc{glibre::ContextTag::core, 1024 * 1024};
    AssetTable<int> table{alloc, detail::AssetTypeTag{0}};

    auto h0 = table.insert(0);
    auto h1 = table.insert(1);
    auto h2 = table.insert(2);

    // Sanity: three slots.
    CHECK(table.slot_count() == 3u);

    // Release out-of-order (h2 before h0).
    table.release(h2);
    table.release(h0);

    // h1 still alive.
    {
        auto r = table.resolve(h1);
        REQUIRE(r.has_value());
        CHECK(*r.value() == 1);
    }

    // First new insert must reuse slot 0 (lowest free index).
    auto new_h0 = table.insert(100);

    // Second new insert must reuse slot 2 (next lowest free index).
    auto new_h2 = table.insert(200);

    // No new slots should have been allocated.
    CHECK(table.slot_count() == 3u);

    // Verify resolved payloads.
    {
        auto r = table.resolve(new_h0);
        REQUIRE(r.has_value());
        CHECK(*r.value() == 100);
    }
    {
        auto r = table.resolve(new_h2);
        REQUIRE(r.has_value());
        CHECK(*r.value() == 200);
    }

    // Old handles are stale.
    CHECK(!table.resolve(h0).has_value());
    CHECK(!table.resolve(h2).has_value());
}

// ===========================================================================
// Test: core/asset_registry: per_T_table_instantiated_on_first_insert
//
// Verifies that AssetRegistry instantiates a new per-T table on the first
// insert() call for that type, and that a second type gets its own separate
// table (different type_tag).
// ===========================================================================

TEST_CASE(
    "core/asset_registry: per_T_table_instantiated_on_first_insert", "[core][asset_registry]"
) {
    using namespace glibre::core;

    AssetRegistry& reg = AssetRegistry::instance();

#ifdef GLIBRE_TESTING
    // Reset any state from previous test cases.
    reg.reset_for_testing();
    CHECK(reg.registered_type_count() == 0u);
#endif

    // First insert of type int → table created.
    auto hi = reg.insert<int>(42);
    CHECK(reg.registered_type_count() == 1u);

    // Resolve back the int handle.
    auto ri = reg.resolve(hi);
    REQUIRE(ri.has_value());
    CHECK(*ri.value() == 42);

    // First insert of a different type → second table created.
    struct MyPayload {
        int x;
    };

    auto hm = reg.insert(MyPayload{7});
    CHECK(reg.registered_type_count() == 2u);

    auto rm = reg.resolve(hm);
    REQUIRE(rm.has_value());
    CHECK(rm.value()->x == 7);

    // int handle still resolves correctly.
    auto ri2 = reg.resolve(hi);
    REQUIRE(ri2.has_value());
    CHECK(*ri2.value() == 42);

    // Release + re-insert for int.
    reg.release(hi);
    auto hi2 = reg.insert<int>(99);
    auto ri3 = reg.resolve(hi2);
    REQUIRE(ri3.has_value());
    CHECK(*ri3.value() == 99);

    // Old hi handle is stale.
    auto ri_stale = reg.resolve(hi);
    CHECK(!ri_stale.has_value());

#ifdef GLIBRE_TESTING
    // Clean up.
    reg.reset_for_testing();
#endif
}
