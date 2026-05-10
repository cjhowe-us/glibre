// tests/core/asset_handle/asset_handle_test.cpp
//
// Catch2 unit tests for glibre::core::AssetHandle<T>.
//
// Authority: specs/core/SPEC.md §4.7, §5.2, §6.8; plan #598 Unit Test Plan.
//
// Named test cases (plan #598 Unit Test Plan + DoD):
//   - core/asset_handle: bit_layout_index_generation_typetag
//   - core/asset_handle: opaque_equality_compares_by_bits
//
// Design constraints:
//   - -fno-exceptions (error-model.md §Decision 3).
//   - No REQUIRE_THROWS usage.
//   - Tests exercise the public API surface (AssetHandle<T> + detail helpers)
//     only; internal slot vectors are not accessed directly.

#include <cstdint>
#include <limits>
#include <tuple>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <glibre/core/asset_handle.hpp>

// ===========================================================================
// Test: core/asset_handle: bit_layout_index_generation_typetag
//
// Verifies that the bit packing follows SPEC §6.8:
//   bits[39: 0] = index      (40 bits)
//   bits[61:40] = generation (22 bits)
//   bits[63:62] = type_tag   (2 bits)
//
// Approach: pack a known (index, generation, type_tag) triple, then unpack
// it and verify the round-trip is lossless.  Also verify that each field is
// isolated to its designated bit region by checking the raw bits directly.
// ===========================================================================

TEST_CASE("core/asset_handle: bit_layout_index_generation_typetag", "[core][asset_handle]") {
    using namespace glibre::core;
    using namespace glibre::core::detail;

    // Verify bit-width constants.
    static_assert(kAssetIndexBits == 40u);
    static_assert(kAssetGenerationBits == 22u);
    static_assert(kAssetTypeTagBits == 2u);
    static_assert(
        kAssetIndexBits + kAssetGenerationBits + kAssetTypeTagBits == 64u,
        "Bit fields must sum to exactly 64 bits."
    );

    struct MockPayload {};

    // -----------------------------------------------------------------------
    // Round-trip with sub-maximum values (verify packing logic).
    // -----------------------------------------------------------------------
    {
        constexpr AssetIndex test_index = (AssetIndex{1} << 39u) - 1u;          // 2^39 - 1
        constexpr AssetGeneration test_gen = (AssetGeneration{1} << 21u) - 1u;  // 2^21 - 1
        constexpr AssetTypeTag test_tag = 3u;                                   // max 2-bit value

        auto handle = asset_pack<MockPayload>(test_index, test_gen, test_tag);

        auto [idx, gen, tag] = asset_unpack(handle);
        CHECK(idx == test_index);
        CHECK(gen == test_gen);
        CHECK(tag == test_tag);

        // Verify raw bit isolation:
        CHECK((handle.bits & kAssetIndexMask) == test_index);
        CHECK(
            ((handle.bits >> kAssetIndexBits) & kAssetGenerationMask) ==
            static_cast<std::uint64_t>(test_gen)
        );
        CHECK(
            ((handle.bits >> (kAssetIndexBits + kAssetGenerationBits)) & kAssetTypeTagMask) ==
            static_cast<std::uint64_t>(test_tag)
        );
    }

    // -----------------------------------------------------------------------
    // Round-trip with TRUE field maxima — exercises every bit of every field.
    // Previously the test used 2^39-1 / 2^21-1 (one bit short of the maximum),
    // so a shift-by-39 instead of shift-by-40 bug would not be caught.
    // -----------------------------------------------------------------------
    {
        constexpr AssetIndex max_index = kAssetIndexMax;          // all 40 low bits set
        constexpr AssetGeneration max_gen = kAssetGenerationMax;  // all 22 gen bits set
        constexpr AssetTypeTag max_tag = kAssetTypeTagMax;        // both tag bits set

        auto handle = asset_pack<MockPayload>(max_index, max_gen, max_tag);

        auto [idx, gen, tag] = asset_unpack(handle);
        CHECK(idx == max_index);
        CHECK(gen == max_gen);
        CHECK(tag == max_tag);

        // Raw isolation at true maxima.
        CHECK((handle.bits & kAssetIndexMask) == max_index);
        CHECK(
            ((handle.bits >> kAssetIndexBits) & kAssetGenerationMask) ==
            static_cast<std::uint64_t>(max_gen)
        );
        CHECK(
            ((handle.bits >> (kAssetIndexBits + kAssetGenerationBits)) & kAssetTypeTagMask) ==
            static_cast<std::uint64_t>(max_tag)
        );

        // The all-bits-set pattern (with all three maxima packed) must equal
        // std::numeric_limits<uint64_t>::max() to confirm no bit is left unused.
        CHECK(handle.bits == std::numeric_limits<std::uint64_t>::max());
    }

    // -----------------------------------------------------------------------
    // Overflow truncation: packing values one above each maximum.
    // asset_pack masks each field — values that exceed the field width must
    // truncate to 0 (wraparound via mask).
    // -----------------------------------------------------------------------
    {
        // kAssetIndexMax + 1 = 2^40, which truncates to 0 after masking.
        constexpr AssetIndex overflow_index = kAssetIndexMax + AssetIndex{1};
        auto h_idx = asset_pack<MockPayload>(overflow_index, 1u, 0u);
        auto [i, g, t] = asset_unpack(h_idx);
        CHECK(i == 0u);  // truncated

        // kAssetGenerationMax + 1 = 2^22, truncates to 0.
        constexpr AssetGeneration overflow_gen = kAssetGenerationMax + AssetGeneration{1};
        auto h_gen = asset_pack<MockPayload>(1u, overflow_gen, 0u);
        auto [i2, g2, t2] = asset_unpack(h_gen);
        CHECK(g2 == 0u);  // truncated

        // kAssetTypeTagMax + 1 = 4, truncates to 0.
        constexpr AssetTypeTag overflow_tag = kAssetTypeTagMax + AssetTypeTag{1};
        auto h_tag = asset_pack<MockPayload>(1u, 1u, overflow_tag);
        auto [i3, g3, t3] = asset_unpack(h_tag);
        CHECK(t3 == 0u);  // truncated
    }

    // -----------------------------------------------------------------------
    // Zero-value handle: all fields zero (the "null / never-issued" state).
    // -----------------------------------------------------------------------
    {
        AssetHandle<MockPayload> zero_handle{};
        auto [zi, zg, zt] = asset_unpack(zero_handle);
        CHECK(zi == 0u);
        CHECK(zg == 0u);
        CHECK(zt == 0u);
    }
}

// ===========================================================================
// Test: core/asset_handle: opaque_equality_compares_by_bits
//
// Verifies that operator== on AssetHandle<T> compares by raw bits (SPEC §5.2)
// and that two handles with the same bits are equal regardless of how they
// were constructed, and two handles with different bits are not equal.
// ===========================================================================

TEST_CASE("core/asset_handle: opaque_equality_compares_by_bits", "[core][asset_handle]") {
    using namespace glibre::core;
    using namespace glibre::core::detail;

    struct MockPayload {};

    // Same (index, generation, type_tag) → same bits → equal.
    auto h1 = asset_pack<MockPayload>(42u, 7u, 1u);
    auto h2 = asset_pack<MockPayload>(42u, 7u, 1u);
    CHECK(h1 == h2);

    // Different index → different bits → not equal.
    auto h3 = asset_pack<MockPayload>(43u, 7u, 1u);
    CHECK(!(h1 == h3));

    // Different generation → different bits → not equal.
    auto h4 = asset_pack<MockPayload>(42u, 8u, 1u);
    CHECK(!(h1 == h4));

    // Different type_tag → different bits → not equal.
    auto h5 = asset_pack<MockPayload>(42u, 7u, 2u);
    CHECK(!(h1 == h5));

    // Direct bits construction matches pack.
    AssetHandle<MockPayload> h6{h1.bits};
    CHECK(h6 == h1);

    // Default-constructed is all-zero → equal to another default-constructed.
    AssetHandle<MockPayload> hz1{};
    AssetHandle<MockPayload> hz2{};
    CHECK(hz1 == hz2);
    // And not equal to a real handle.
    CHECK(!(hz1 == h1));
}
