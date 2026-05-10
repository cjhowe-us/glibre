// tests/core/entity_allocator/entity_allocator_test.cpp
//
// Catch2 unit tests for glibre::core::EntityAllocator.
//
// Authority: specs/core/SPEC.md §4.1, §4.3; plan #557.
//
// Named test cases (plan #557 Unit Test Plan):
//   - core/entity_allocator: spawn_returns_unique_handles
//   - core/entity_allocator: despawn_increments_generation
//   - core/entity_allocator: stale_handle_resolves_to_EntityStale
//   - core/entity_allocator: free_list_reuses_lowest_index_first
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS usage.
//   - Uses glibre::PerContextAllocator for std::pmr backing, consistent with
//     how EntityAllocator will be used inside World.

#include <cstdint>
#include <unordered_set>

#include <catch2/catch_test_macros.hpp>
#include <glibre/alloc.hpp>
#include <glibre/core/entity.hpp>
#include <glibre/error.hpp>

// EntityAllocator is an internal type in core/src/world/.  Tests reach it
// via a dedicated include from the test-only include path.  The CMakeLists
// adds the core/src directory as a PRIVATE include so only test targets see it.
#include "entity_allocator.hpp"

namespace {

/// Returns true iff the glibre::Error wraps core::Error::EntityStale.
bool is_entity_stale(const glibre::Error& err) noexcept {
    using glibre::core::Error;
    if (const auto* e = std::get_if<Error>(&err.code())) {
        return *e == Error::EntityStale;
    }
    return false;
}

}  // anonymous namespace

// ===========================================================================
// Test: spawn_returns_unique_handles
//
// SPEC §4.3 invariant 1 (handle uniqueness): each spawn must produce an
// Entity value different from every other live entity.
//
// Verifies:
//   A. Spawning N entities produces N distinct Entity values.
//   B. alive_count() equals N after N spawns.
//   C. slot_count() grows monotonically on new-slot spawns.
//   D. is_alive() returns true for every spawned entity.
// ===========================================================================

TEST_CASE("core/entity_allocator: spawn_returns_unique_handles", "[core][entity_allocator]") {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::EntityAllocator ea{alloc};

    constexpr std::uint32_t kN = 64;

    std::unordered_set<std::uint64_t> seen;
    seen.reserve(kN);

    for (std::uint32_t i = 0; i < kN; ++i) {
        auto result = ea.spawn();
        REQUIRE(result.has_value());
        const glibre::core::Entity e = *result;

        // Each bits value must be distinct across all spawned entities.
        const bool inserted = seen.insert(e.bits).second;
        CHECK(inserted);
        CHECK(ea.is_alive(e));
    }

    CHECK(ea.alive_count() == kN);
    CHECK(ea.slot_count() == kN);  // No free-list reuse yet; all new slots.
}

// ===========================================================================
// Test: despawn_increments_generation
//
// SPEC §4.3 invariant 1: generation increments before reuse; any prior Entity
// value for that slot now resolves to core::Error::EntityStale.
//
// Verifies:
//   A. despawn() succeeds for a live entity.
//   B. is_alive() returns false after despawn.
//   C. alive_count() decrements by 1.
//   D. The despawned entity's bits are no longer alive (generation mismatch).
//   E. Despawning a default-constructed Entity{} yields EntityStale.
//   F. Despawning the same entity twice yields EntityStale on the second call.
// ===========================================================================

TEST_CASE("core/entity_allocator: despawn_increments_generation", "[core][entity_allocator]") {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::EntityAllocator ea{alloc};

    SECTION("despawn live entity — succeeds and clears alive") {
        auto spawn_result = ea.spawn();
        REQUIRE(spawn_result.has_value());
        const glibre::core::Entity e = *spawn_result;

        CHECK(ea.is_alive(e));
        CHECK(ea.alive_count() == 1U);

        auto despawn_result = ea.despawn(e);
        REQUIRE(despawn_result.has_value());

        CHECK_FALSE(ea.is_alive(e));
        CHECK(ea.alive_count() == 0U);
    }

    SECTION("despawn null entity (default-constructed) yields EntityStale") {
        const glibre::core::Entity null_e{};
        auto result = ea.despawn(null_e);
        REQUIRE_FALSE(result.has_value());
        CHECK(is_entity_stale(result.error()));
    }

    SECTION("double-despawn yields EntityStale on second call") {
        auto spawn_result = ea.spawn();
        REQUIRE(spawn_result.has_value());
        const glibre::core::Entity e = *spawn_result;

        REQUIRE(ea.despawn(e).has_value());  // first despawn — success
        auto second = ea.despawn(e);         // second despawn — stale
        REQUIRE_FALSE(second.has_value());
        CHECK(is_entity_stale(second.error()));
    }

    SECTION("generation advances across spawn-despawn cycles") {
        // Spawn slot 0, despawn, re-spawn.  The new entity must differ from
        // the old one (generation has advanced).
        auto r1 = ea.spawn();
        REQUIRE(r1.has_value());
        const glibre::core::Entity e1 = *r1;

        REQUIRE(ea.despawn(e1).has_value());

        auto r2 = ea.spawn();
        REQUIRE(r2.has_value());
        const glibre::core::Entity e2 = *r2;

        // Same slot index, different generation → different bits.
        CHECK_FALSE(e1 == e2);
        CHECK(ea.is_alive(e2));
        CHECK_FALSE(ea.is_alive(e1));
    }
}

// ===========================================================================
// Test: stale_handle_resolves_to_EntityStale
//
// SPEC §4.3 invariant 1: resolve() returns core::Error::EntityStale for any
// handle whose generation does not match the current slot generation, or
// whose slot index is out of range.
//
// Verifies:
//   A. A freshly-despawned entity resolve()s to EntityStale.
//   B. A default-constructed Entity{} resolve()s to EntityStale.
//   C. An Entity with an out-of-range slot index resolve()s to EntityStale.
//   D. A live entity resolve()s to its slot index (success path).
// ===========================================================================

TEST_CASE(
    "core/entity_allocator: stale_handle_resolves_to_EntityStale", "[core][entity_allocator]"
) {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::EntityAllocator ea{alloc};

    SECTION("default-constructed entity is always stale") {
        const glibre::core::Entity null_e{};
        auto r = ea.resolve(null_e);
        REQUIRE_FALSE(r.has_value());
        CHECK(is_entity_stale(r.error()));
    }

    SECTION("entity with out-of-range slot index is stale") {
        // Fabricate an entity with a slot index beyond the slot vector.
        const glibre::core::Entity bad = glibre::core::detail::pack(0xFFFF'FFFFU, 1U);
        auto r = ea.resolve(bad);
        REQUIRE_FALSE(r.has_value());
        CHECK(is_entity_stale(r.error()));
    }

    SECTION("live entity resolves to its slot index") {
        auto spawn_result = ea.spawn();
        REQUIRE(spawn_result.has_value());
        const glibre::core::Entity e = *spawn_result;

        auto r = ea.resolve(e);
        REQUIRE(r.has_value());
        // Slot 0 (first spawn on empty allocator).
        CHECK(*r == 0U);
    }

    SECTION("despawned entity resolves to EntityStale") {
        auto spawn_result = ea.spawn();
        REQUIRE(spawn_result.has_value());
        const glibre::core::Entity e = *spawn_result;

        REQUIRE(ea.despawn(e).has_value());

        auto r = ea.resolve(e);
        REQUIRE_FALSE(r.has_value());
        CHECK(is_entity_stale(r.error()));
    }

    SECTION("re-spawned slot — old handle is stale, new handle resolves") {
        auto r1 = ea.spawn();
        REQUIRE(r1.has_value());
        const glibre::core::Entity e1 = *r1;
        REQUIRE(ea.despawn(e1).has_value());

        auto r2 = ea.spawn();  // reuses slot 0
        REQUIRE(r2.has_value());
        const glibre::core::Entity e2 = *r2;

        // Old handle is stale.
        auto old_r = ea.resolve(e1);
        REQUIRE_FALSE(old_r.has_value());
        CHECK(is_entity_stale(old_r.error()));

        // New handle is valid.
        auto new_r = ea.resolve(e2);
        REQUIRE(new_r.has_value());
        CHECK(*new_r == 0U);  // still slot 0
    }
}

// ===========================================================================
// Test: free_list_reuses_lowest_index_first
//
// PHILOSOPHY §7: determinism requires reuse order to be lowest-free-index-first.
// SPEC §4.3 (via plan #557 Agent Notes).
//
// Verifies:
//   A. After despawning multiple entities, the next spawn reuses the slot
//      with the lowest index.
//   B. The order is preserved regardless of despawn order.
//   C. slot_count() does not grow when free-list slots are available.
// ===========================================================================

TEST_CASE(
    "core/entity_allocator: free_list_reuses_lowest_index_first", "[core][entity_allocator]"
) {
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::EntityAllocator ea{alloc};

    // Spawn 4 entities (slots 0, 1, 2, 3).
    glibre::core::Entity entities[4]{};
    for (auto& e : entities) {
        auto r = ea.spawn();
        REQUIRE(r.has_value());
        e = *r;
    }
    REQUIRE(ea.slot_count() == 4U);

    SECTION("despawn in reverse order; respawn uses lowest index first") {
        // Despawn slots 3, 1 (out-of-order to stress the sorted free-list).
        REQUIRE(ea.despawn(entities[3]).has_value());
        REQUIRE(ea.despawn(entities[1]).has_value());

        // Free list now holds [1, 3] sorted ascending.
        // First respawn must reuse slot 1.
        auto r_first = ea.spawn();
        REQUIRE(r_first.has_value());
        auto [idx_first, gen_first] = glibre::core::detail::unpack(*r_first);
        CHECK(idx_first == 1U);

        // Second respawn must reuse slot 3.
        auto r_second = ea.spawn();
        REQUIRE(r_second.has_value());
        auto [idx_second, gen_second] = glibre::core::detail::unpack(*r_second);
        CHECK(idx_second == 3U);

        // slot_count() must not have grown beyond 4.
        CHECK(ea.slot_count() == 4U);
    }

    SECTION("despawn all; respawn in index order 0, 1, 2, 3") {
        for (auto& e : entities) {
            REQUIRE(ea.despawn(e).has_value());
        }
        CHECK(ea.alive_count() == 0U);

        for (std::uint32_t expected_idx = 0; expected_idx < 4; ++expected_idx) {
            auto r = ea.spawn();
            REQUIRE(r.has_value());
            auto [idx, gen] = glibre::core::detail::unpack(*r);
            CHECK(idx == expected_idx);
        }

        CHECK(ea.slot_count() == 4U);  // No new slots created.
    }
}
