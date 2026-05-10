// tests/core/entity/entity_test.cpp
//
// Catch2 unit tests for glibre::core::Entity value object.
//
// Authority: specs/core/SPEC.md §4.3, §5.2; plan #557.
//
// Named test cases (plan #557 Unit Test Plan):
//   - core/entity: bits_layout_index_low_generation_high
//   - core/entity: opaque_equality_compares_by_bits
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS usage.
//   - Tests exercise public API (Entity bits, operator==) and the internal
//     core::detail pack/unpack helpers (restricted to core::detail namespace).

#include <cstdint>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <glibre/core/entity.hpp>

// ===========================================================================
// Test: bits_layout_index_low_generation_high
//
// SPEC §4.3: Entity is a 64-bit handle encoding a 32-bit slot index in the
// low half and a 32-bit generation counter in the high half.
//
// Verifies:
//   A. detail::pack(index, generation) produces the expected bit layout.
//   B. detail::unpack(entity) round-trips correctly from pack().
//   C. A zero-bits Entity{} unpacks to (index=0, generation=0).
//   D. Generation occupies bits[63:32]; index occupies bits[31:0].
//   E. Entity is trivially copyable (value-type contract).
// ===========================================================================

TEST_CASE("core/entity: bits_layout_index_low_generation_high", "[core][entity]") {
    // Compile-time: Entity must be trivially copyable (plain value object).
    // Note: default member initializer `bits{}` means Entity is not trivially
    // default constructible (the initializer is user-provided at the aggregate
    // level), but it IS trivially copyable — copies are bitwise.
    static_assert(
        std::is_trivially_copyable_v<glibre::core::Entity>, "Entity must be trivially copyable."
    );

    using namespace glibre::core;
    using namespace glibre::core::detail;

    SECTION("pack(0, 0) produces bits == 0") {
        const Entity e = pack(0, 0);
        CHECK(e.bits == 0ULL);
    }

    SECTION("pack(idx, gen) — index in low 32 bits, generation in high 32 bits") {
        // Choose values that are distinct in both halves.
        constexpr EntityIndex kIdx = 0xABCD'1234U;
        constexpr EntityGeneration kGen = 0xDEAD'BEEFu;

        const Entity e = pack(kIdx, kGen);

        // Low 32 bits = index.
        CHECK((e.bits & 0xFFFF'FFFFULL) == static_cast<std::uint64_t>(kIdx));
        // High 32 bits = generation.
        CHECK((e.bits >> 32U) == static_cast<std::uint64_t>(kGen));
    }

    SECTION("unpack round-trips pack") {
        constexpr EntityIndex kIdx = 42U;
        constexpr EntityGeneration kGen = 7U;

        const Entity e = pack(kIdx, kGen);
        auto [idx, gen] = unpack(e);

        CHECK(idx == kIdx);
        CHECK(gen == kGen);
    }

    SECTION("default-constructed Entity{} unpacks to (index=0, generation=0)") {
        const Entity e{};
        auto [idx, gen] = unpack(e);
        CHECK(idx == 0U);
        CHECK(gen == 0U);
    }

    SECTION("index maximum (UINT32_MAX) round-trips without corruption") {
        constexpr EntityIndex kMaxIdx = 0xFFFF'FFFFu;
        constexpr EntityGeneration kGen = 1U;

        const Entity e = pack(kMaxIdx, kGen);
        auto [idx, gen] = unpack(e);

        CHECK(idx == kMaxIdx);
        CHECK(gen == kGen);
    }

    SECTION("generation maximum (UINT32_MAX) round-trips without corruption") {
        constexpr EntityIndex kIdx = 1U;
        constexpr EntityGeneration kMaxGen = 0xFFFF'FFFFu;

        const Entity e = pack(kIdx, kMaxGen);
        auto [idx, gen] = unpack(e);

        CHECK(idx == kIdx);
        CHECK(gen == kMaxGen);
    }
}

// ===========================================================================
// Test: opaque_equality_compares_by_bits
//
// SPEC §4.3 invariant 2: Entity is opaque; public equality is on the bits
// field, not on a decomposed (index, generation) struct.
//
// Verifies:
//   A. Two Entity values with identical bits compare equal.
//   B. Two Entity values that differ in the index part compare unequal.
//   C. Two Entity values that differ in the generation part compare unequal.
//   D. operator== is constexpr (compile-time evaluable).
// ===========================================================================

TEST_CASE("core/entity: opaque_equality_compares_by_bits", "[core][entity]") {
    using namespace glibre::core;
    using namespace glibre::core::detail;

    SECTION("identical bits → equal") {
        const Entity a = pack(10U, 3U);
        const Entity b = pack(10U, 3U);
        CHECK(a == b);
    }

    SECTION("different index → not equal") {
        const Entity a = pack(10U, 3U);
        const Entity b = pack(11U, 3U);
        CHECK(!(a == b));
    }

    SECTION("different generation → not equal") {
        const Entity a = pack(10U, 3U);
        const Entity b = pack(10U, 4U);
        CHECK(!(a == b));
    }

    SECTION("default-constructed Entity{} equals itself") {
        const Entity a{};
        const Entity b{};
        CHECK(a == b);
    }

    SECTION("operator== is constexpr") {
        // If operator== is not constexpr this static_assert fails to compile.
        constexpr Entity a = pack(5U, 2U);
        constexpr Entity b = pack(5U, 2U);
        constexpr Entity c = pack(5U, 3U);
        static_assert(a == b, "constexpr equality must hold for identical entities");
        static_assert(!(a == c), "constexpr inequality must hold for mismatched generation");
    }
}
