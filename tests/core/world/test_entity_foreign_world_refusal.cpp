// tests/core/world/test_entity_foreign_world_refusal.cpp
//
// Catch2 unit tests for the EntityForeignWorld refusal-shape contract.
//
// Authority: specs/core/world-design.md §10.1 (entity-allocator error arms),
//            §11.4 (EntityForeignWorld unit-test requirement);
//            reviews/decisions/error-model.md §Decision 2 (per-context enum
//            ownership), §Logging (arm_to_string / to_string stability);
//            plan #940.
//
// Named test cases (plan #940 Unit Test Plan):
//   - world: entity_foreign_world_arm_present
//   - world: entity_outside_slot_range_yields_EntityStale_in_mvp
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS usage.
//   - Test case 1 is purely compile-time (static_assert) + a runtime mirror.
//   - Test case 2 exercises EntityAllocator::is_alive() and ::resolve() —
//     the MVP refusal surface is EntityStale for any entity whose slot
//     index lies outside the allocator's range.  World::is_alive() and
//     World::get_component() are not yet available (post-MVP World surface);
//     when they land they will delegate to the same allocator contract.
//
// Multi-world context (plan #940 §Scope):
//   glibre::core::Error::EntityForeignWorld is reserved now so that the
//   closed core::Error enum requires no ABI bump when multi-world lands
//   (post-MVP, SPEC §3.3 deferral R-1.1.35).  The MVP routing is:
//     foreign-world entity → index outside range → EntityStale
//   The EntityForeignWorld arm stays in the enum but is unreachable from
//   any live MVP code path.

#include <string_view>
#include <type_traits>
#include <variant>

#include <catch2/catch_test_macros.hpp>
#include <glibre/alloc.hpp>
#include <glibre/core/entity.hpp>
#include <glibre/error.hpp>
#include <glibre/log_error.hpp>  // to_string overloads (arm_to_string contract)

// EntityAllocator is an internal type (core/src/world/).  The CMakeLists for
// this test target adds core/src/world as a PRIVATE include directory — the
// same pattern used by tests/core/entity_allocator/.
#include "entity_allocator.hpp"

// ===========================================================================
// Test: world: entity_foreign_world_arm_present
//
// Authority: specs/core/world-design.md §11.4:
//   "the EntityForeignWorld arm has a unit test that asserts the refusal
//   shape, but the multi-world boundary itself is post-MVP".
//
// Verifies:
//   A. core::Error::EntityForeignWorld is a syntactically-valid member of the
//      core::Error enum (static_assert ensures the name resolves at compile
//      time).
//   B. The arm's underlying value is distinct from core::Error::EntityStale
//      (they must not collide — error-model.md §Composition Rules R2).
//   C. glibre::core::to_string(core::Error::EntityForeignWorld) returns the
//      stable string "EntityForeignWorld" (arm_to_string contract,
//      error-model.md §Logging rule 2).
//   D. The arm participates in glibre::Error::Variant — i.e. glibre::Error
//      can be constructed from core::Error::EntityForeignWorld.
// ===========================================================================

// A. Compile-time: arm name must resolve.
static_assert(
    std::is_same_v<decltype(glibre::core::Error::EntityForeignWorld), glibre::core::Error>,
    "core::Error::EntityForeignWorld must be a member of the core::Error enum."
);

// B. Compile-time: EntityForeignWorld and EntityStale must not share a value.
static_assert(
    static_cast<std::uint16_t>(glibre::core::Error::EntityForeignWorld) !=
        static_cast<std::uint16_t>(glibre::core::Error::EntityStale),
    "core::Error::EntityForeignWorld must have a distinct underlying value from "
    "core::Error::EntityStale."
);

// D. Compile-time: glibre::Error must be constructible from core::Error::EntityForeignWorld.
static_assert(
    std::constructible_from<glibre::Error::Variant, glibre::core::Error>,
    "glibre::Error::Variant must be constructible from core::Error — "
    "EntityForeignWorld must participate in the tagged union."
);

TEST_CASE("world: entity_foreign_world_arm_present", "[world][entity][error]") {
    // Runtime mirror of compile-time assertion A: the enumerator resolves.
    constexpr auto arm = glibre::core::Error::EntityForeignWorld;
    static_assert(
        std::is_same_v<decltype(arm), const glibre::core::Error>,
        "core::Error::EntityForeignWorld must resolve as a glibre::core::Error value."
    );

    // C (runtime): arm_to_string mapping must return the stable string
    // "EntityForeignWorld" per error-model.md §Logging rule 2.
    constexpr std::string_view expected_name = "EntityForeignWorld";
    constexpr const char* actual_name = glibre::core::to_string(arm);
    static_assert(
        std::string_view{actual_name} == expected_name,
        "glibre::core::to_string(core::Error::EntityForeignWorld) must return "
        "\"EntityForeignWorld\" (stable arm_to_string mapping, error-model.md §Logging)."
    );
    CHECK(std::string_view{actual_name} == expected_name);

    // D (runtime): glibre::Error wraps the arm correctly.
    const glibre::Error err{arm};
    const auto* inner = std::get_if<glibre::core::Error>(&err.code());
    REQUIRE(inner != nullptr);
    CHECK(*inner == glibre::core::Error::EntityForeignWorld);

    // B (runtime): EntityForeignWorld and EntityStale must have distinct values.
    CHECK(
        static_cast<std::uint16_t>(glibre::core::Error::EntityForeignWorld) !=
        static_cast<std::uint16_t>(glibre::core::Error::EntityStale)
    );
}

// ===========================================================================
// Test: world: entity_outside_slot_range_yields_EntityStale_in_mvp
//
// Authority: specs/core/world-design.md §10.1 (entity-allocator arms),
//            plan #940 §Scope (MVP routing: foreign-world entity → EntityStale).
//
// Multi-world is refused for MVP (SPEC §3.3, R-1.1.35).  An entity whose
// slot index exceeds the allocator's current slot count — i.e. it was never
// issued by this allocator, which is the MVP proxy for "foreign-world entity"
// — must be resolved as EntityStale.
//
// Verifies (at the EntityAllocator level, which is the MVP refusal surface):
//   A. EntityAllocator::is_alive() returns false for an Entity whose index
//      exceeds slot_count().
//   B. EntityAllocator::resolve() returns core::Error::EntityStale for the
//      same entity (not EntityForeignWorld — the ForeignWorld arm is
//      unreachable in MVP; stale is the existing catchall).
//
// Note: World::is_alive() and World::get_component() are not yet available
// (they are deliverables of later World implementation plans).  When those
// surfaces land they will delegate to EntityAllocator under the same
// contract.  This test pins the allocator-level contract now so that the
// higher-level World tests can reference it.
// ===========================================================================

namespace {

/// Returns true iff the glibre::Error wraps core::Error::EntityStale.
[[nodiscard]] bool is_entity_stale(const glibre::Error& err) noexcept {
    using glibre::core::Error;
    if (const auto* e = std::get_if<Error>(&err.code())) {
        return *e == Error::EntityStale;
    }
    return false;
}

}  // anonymous namespace

TEST_CASE(
    "world: entity_outside_slot_range_yields_EntityStale_in_mvp", "[world][entity][error]"
) {
    // Construct an allocator backed by the core context (ContextTag::core).
    glibre::PerContextAllocator alloc{glibre::ContextTag::core};
    glibre::core::EntityAllocator allocator{alloc};

    // Spawn one entity so the allocator has exactly one slot.
    const auto spawn_result = allocator.spawn();
    REQUIRE(spawn_result.has_value());
    const std::uint32_t slot_count_after_one_spawn = allocator.slot_count();
    REQUIRE(slot_count_after_one_spawn == 1u);

    // Build an Entity whose index is strictly beyond the allocator's range.
    // The slot_count() is the number of allocated slots (0-based indexing);
    // slot_count_after_one_spawn is the first out-of-range index.
    const glibre::core::Entity foreign_entity =
        glibre::core::detail::pack(slot_count_after_one_spawn, /*generation=*/1u);

    // A. is_alive() must return false for an out-of-range entity.
    CHECK_FALSE(allocator.is_alive(foreign_entity));

    // B. resolve() must return EntityStale for an out-of-range entity.
    //    In MVP, the EntityForeignWorld arm is reserved but unreachable;
    //    EntityStale is the arm that covers all forms of invalid handle.
    const auto resolve_result = allocator.resolve(foreign_entity);
    REQUIRE_FALSE(resolve_result.has_value());
    CHECK(is_entity_stale(resolve_result.error()));

    // Verify the error is NOT EntityForeignWorld — that arm is unreachable MVP.
    const auto* inner = std::get_if<glibre::core::Error>(&resolve_result.error().code());
    REQUIRE(inner != nullptr);
    CHECK(*inner != glibre::core::Error::EntityForeignWorld);
}
