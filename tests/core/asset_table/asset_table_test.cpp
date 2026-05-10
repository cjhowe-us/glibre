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
//   - core/asset_table: generation_saturation_retires_slot
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
// Test: core/asset_table: foreign_type_tag_resolves_to_AssetStale
//
// HIGH-1 (r1): resolve() and release() must reject a handle whose type_tag
// does not match the table's type_tag_ (SPEC §4.7 inv.1, §6.8).
//
// A handle built with a mismatching type_tag must resolve to AssetStale even
// if the slot index and generation are valid within the table.
// ===========================================================================

TEST_CASE("core/asset_table: foreign_type_tag_resolves_to_AssetStale", "[core][asset_table]") {
    using namespace glibre::core;
    using namespace glibre::core::detail;

    glibre::PerContextAllocator alloc{glibre::ContextTag::core, 1024 * 1024};

    // Table constructed with type_tag == 0.
    AssetTable<int> table{alloc, detail::AssetTypeTag{0}};

    // Insert a valid payload — handle carries type_tag 0.
    auto valid_handle = table.insert(42);
    auto [idx, gen, tag] = asset_unpack(valid_handle);
    CHECK(tag == 0u);  // sanity: tag matches table

    // Forge a handle with type_tag == 1 (different from the table's tag 0),
    // but the same slot index and generation so only the tag differs.
    auto foreign_handle = asset_pack<int>(idx, gen, AssetTypeTag{1});

    // resolve() must return AssetStale for the foreign-tagged handle.
    auto r = table.resolve(foreign_handle);
    REQUIRE(!r.has_value());
    CHECK(std::holds_alternative<glibre::core::Error>(r.error().code()));
    CHECK(std::get<glibre::core::Error>(r.error().code()) == glibre::core::Error::AssetStale);

    // The genuine handle must still resolve correctly (no side-effect).
    auto r2 = table.resolve(valid_handle);
    REQUIRE(r2.has_value());
    CHECK(*r2.value() == 42);

    // release() with the foreign-tagged handle must be a no-op:
    // the valid handle must still resolve after the spurious release attempt.
    table.release(foreign_handle);
    auto r3 = table.resolve(valid_handle);
    REQUIRE(r3.has_value());
    CHECK(*r3.value() == 42);
}

// ===========================================================================
// Test: core/asset_registry: reset_for_testing_clears_type_map
//
// MED-4 (r1): after reset_for_testing(), re-registering types in a different
// order must not produce a UB cast (the old per-T-static s_tag_index bug).
//
// Sequence: insert A, insert B, reset, insert B first (should get tag 0),
// insert A second (should get tag 1). Both must resolve to their own payloads.
// ===========================================================================

#ifdef GLIBRE_TESTING
TEST_CASE("core/asset_registry: reset_for_testing_clears_type_map", "[core][asset_registry]") {
    using namespace glibre::core;

    AssetRegistry& reg = AssetRegistry::instance();

    // -- Phase 1: register int (tag 0) and float (tag 1). ----------------
    reg.reset_for_testing();
    CHECK(reg.registered_type_count() == 0u);

    auto hi = reg.insert<int>(10);
    auto hf = reg.insert<float>(3.14f);
    CHECK(reg.registered_type_count() == 2u);

    // Sanity: both resolve.
    REQUIRE(reg.resolve(hi).has_value());
    CHECK(*reg.resolve(hi).value() == 10);
    REQUIRE(reg.resolve(hf).has_value());
    CHECK(*reg.resolve(hf).value() == 3.14f);

    // -- Phase 2: reset, then register in reverse order. -----------------
    reg.reset_for_testing();
    CHECK(reg.registered_type_count() == 0u);

    // float inserted first — should now receive tag 0 (previously int's tag).
    auto hf2 = reg.insert<float>(2.71f);
    CHECK(reg.registered_type_count() == 1u);

    // int inserted second — should receive tag 1.
    auto hi2 = reg.insert<int>(99);
    CHECK(reg.registered_type_count() == 2u);

    // Both must resolve to their own payloads (not each other's).
    REQUIRE(reg.resolve(hf2).has_value());
    CHECK(*reg.resolve(hf2).value() == 2.71f);

    REQUIRE(reg.resolve(hi2).has_value());
    CHECK(*reg.resolve(hi2).value() == 99);

    // Cleanup.
    reg.reset_for_testing();
}
#endif  // GLIBRE_TESTING

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

// ===========================================================================
// Test: core/asset_table: generation_saturation_retires_slot
//
// MED-3 (r2): generation saturation behavior added in R1 is untested.
//
// SPEC §6.8, §6.12.3: after kAssetGenerationMax release/reinsert cycles the
// 22-bit generation counter is exhausted; the slot is permanently retired
// (not returned to the free list) to avoid silent handle aliasing.
//
// Procedure:
//   1. Insert slot 0 (generation 1).
//   2. Loop kAssetGenerationMax - 1 times: release slot (gen increments),
//      re-insert (reuses same slot, picks up the incremented generation).
//      After the loop, the active handle for slot 0 carries
//      generation == kAssetGenerationMax - 1; the slot holds
//      generation == kAssetGenerationMax - 1 as well.
//   3. Release once more: generation reaches kAssetGenerationMax; the
//      release path detects saturation (>= kAssetGenerationMax) and does
//      NOT add the slot to the free list.
//   4. A new insert must create slot 1 (new slot, not reuse slot 0).
//      slot_count() must be 2.
//   5. slot 0 resolves to AssetStale for all stale handles (saturation does
//      not affect staleness semantics).
// ===========================================================================

TEST_CASE("core/asset_table: generation_saturation_retires_slot", "[core][asset_table]") {
    using namespace glibre::core;
    using namespace glibre::core::detail;

    // Allocate enough for kAssetGenerationMax iterations × small header overhead.
    // Each cycle: Slot<int> stays in the vector; free_list_ grows/shrinks by 1
    // entry per cycle (one uint32_t).  Memory budget: 2 × sizeof(Slot<int>) +
    // 2 × sizeof(uint32_t) + generous headroom.
    glibre::PerContextAllocator alloc{glibre::ContextTag::core, 4 * 1024 * 1024};  // 4 MiB
    AssetTable<int> table{alloc, AssetTypeTag{0}};

    // Step 1: first insert → slot 0, generation 1.
    auto h = table.insert(0);
    CHECK(table.slot_count() == 1u);

    // Step 2: cycle kAssetGenerationMax - 1 times.
    // Each iteration: release (increments gen) → re-insert (reuses slot 0 with
    // the already-incremented generation).
    //
    // After i iterations: slot 0 has generation == i+1 and h carries gen i+1.
    // After the full kAssetGenerationMax - 1 iterations:
    //   slot.generation == kAssetGenerationMax, h carries kAssetGenerationMax.
    for (std::uint32_t i = 1; i < kAssetGenerationMax; ++i) {
        table.release(h);
        h = table.insert(static_cast<int>(i));  // reuses slot 0 with generation i+1
    }

    // At this point: slot 0 has generation == kAssetGenerationMax, live == true.
    // The handle h carries generation kAssetGenerationMax.
    CHECK(table.slot_count() == 1u);  // no new slots allocated during cycling

    // Step 3: final saturating release.
    // slot.generation == kAssetGenerationMax → guard fires → slot NOT returned
    // to free list.
    table.release(h);

    // Step 4: new insert must allocate slot 1 (slot 0 is retired).
    auto h2 = table.insert(999);
    CHECK(table.slot_count() == 2u);  // new slot appended, not reuse of slot 0

    // Verify h2 is a valid, different handle resolving to 999.
    auto r2 = table.resolve(h2);
    REQUIRE(r2.has_value());
    CHECK(*r2.value() == 999);

    // Step 5: old handle h is stale (released).
    auto r_stale = table.resolve(h);
    REQUIRE(!r_stale.has_value());
    CHECK(std::get<glibre::core::Error>(r_stale.error().code()) == glibre::core::Error::AssetStale);
}
