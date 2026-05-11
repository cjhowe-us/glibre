// tests/shader/permutation/permutation_test.cpp
//
// Catch2 unit tests for the PermutationKey codec, PermutationIndex bijection,
// and EnumerationTable cross-product walker.
//
// Plan: #509 — feat(shader/permutation): PermutationKey codec + cross-product enumerator.
// Authority: specs/shader/SPEC.md §4.2.
//
// Named test cases (plan #509 Unit Test Plan + DoD):
//   shader/permutation: permutation_key_encoding_is_total_injective_and_bit_stable
//   shader/permutation: permutation_key_from_bytes_rejects_out_of_range_with_PermutationKeyMalformed
//   shader/permutation: permutation_index_ordering_matches_tuple_field_ascending_order
//   shader/permutation: permutation_index_round_trips_through_key_bijectively
//   shader/permutation: enumeration_table_yields_full_cross_product_in_canonical_order
//   shader/permutation: feature_set_bit_positions_are_stable_across_revs
//
// Design constraints:
//   - -fno-exceptions compatible (error-model.md §Decision 3).
//   - No REQUIRE_THROWS.
//   - Assertions use REQUIRE / CHECK with std::expected::has_value() / error().

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

// Internal permutation headers (exposed via target_include_directories in CMakeLists).
#include "axes.hpp"
#include "enumeration_table.hpp"
#include "packed_key.hpp"
#include "permutation_index.hpp"

// Public shader boundary.
#include <glibre/shader/shader.hpp>

// Import permutation types into scope for brevity.
using namespace glibre::shader;

// ---------------------------------------------------------------------------
// Test: permutation_key_encoding_is_total_injective_and_bit_stable
//
// Verifies §4.2 invariant 1:
//   - to_bytes() and from_bytes() are a round-trip for every well-formed key.
//   - The encoding is injective: distinct well-formed keys produce distinct
//     byte arrays.
//   - The encoding is bit-stable: a known byte array decodes to the exact
//     expected key (not just structurally equivalent).
// ---------------------------------------------------------------------------

TEST_CASE(
    "permutation_key_encoding_is_total_injective_and_bit_stable",
    "[shader][permutation]"
) {
    SECTION("default key encodes to all-zero prefix bytes") {
        PermutationKey k{};
        auto b = k.to_bytes();
        // ShadingModel::Standard == 0
        CHECK(static_cast<std::uint8_t>(b[0]) == 0u);
        // FeatureSet{} == 0
        CHECK(static_cast<std::uint8_t>(b[1]) == 0u);
        CHECK(static_cast<std::uint8_t>(b[2]) == 0u);
        // RenderPath::Forward == 0
        CHECK(static_cast<std::uint8_t>(b[3]) == 0u);
        // LODTier::Desktop == 2
        CHECK(static_cast<std::uint8_t>(b[4]) == 2u);
        // reserved
        CHECK(static_cast<std::uint8_t>(b[5]) == 0u);
    }

    SECTION("round-trip: to_bytes then from_bytes recovers original key") {
        // Exhaustive round-trip over every well-formed key in the cross-product.
        std::uint32_t count = 0u;
        for (std::uint32_t sm = 0; sm < kShadingModelCount; ++sm) {
            for (std::uint32_t feat = 0; feat < (1u << kFeatureBitCount); ++feat) {
                for (std::uint32_t rp = 0; rp < kRenderPathCount; ++rp) {
                    for (std::uint32_t lod = 0; lod < kLODTierCount; ++lod) {
                        PermutationKey orig{};
                        orig.shading_model = static_cast<ShadingModel>(sm);
                        orig.features      = FeatureSet{static_cast<std::uint16_t>(feat)};
                        orig.render_path   = static_cast<RenderPath>(rp);
                        orig.lod_tier      = static_cast<LODTier>(lod);

                        auto bytes  = orig.to_bytes();
                        auto result = PermutationKey::from_bytes(bytes);
                        REQUIRE(result.has_value());
                        CHECK(result.value() == orig);
                        ++count;
                    }
                }
            }
        }
        // Sanity: we iterated over exactly kPermutationCrossProductCardinality keys.
        CHECK(count == kPermutationCrossProductCardinality);
    }

    SECTION("encoding is injective: distinct keys produce distinct byte arrays") {
        // Collect byte arrays for every well-formed key and check for uniqueness.
        using ByteArray = PermutationKey::PackedBytes;
        std::vector<ByteArray> all_bytes;
        all_bytes.reserve(kPermutationCrossProductCardinality);

        for (std::uint32_t sm = 0; sm < kShadingModelCount; ++sm) {
            for (std::uint32_t feat = 0; feat < (1u << kFeatureBitCount); ++feat) {
                for (std::uint32_t rp = 0; rp < kRenderPathCount; ++rp) {
                    for (std::uint32_t lod = 0; lod < kLODTierCount; ++lod) {
                        PermutationKey k{};
                        k.shading_model = static_cast<ShadingModel>(sm);
                        k.features      = FeatureSet{static_cast<std::uint16_t>(feat)};
                        k.render_path   = static_cast<RenderPath>(rp);
                        k.lod_tier      = static_cast<LODTier>(lod);
                        all_bytes.push_back(k.to_bytes());
                    }
                }
            }
        }

        std::sort(all_bytes.begin(), all_bytes.end());
        const auto unique_end = std::unique(all_bytes.begin(), all_bytes.end());
        CHECK(std::distance(all_bytes.begin(), unique_end)
            == static_cast<std::ptrdiff_t>(kPermutationCrossProductCardinality));
    }

    SECTION("bit-stable: known key produces known byte pattern") {
        // Manually construct a key and verify the exact byte layout.
        // ShadingModel::Hair == 2, FeatureSet{Skinned|AlphaTest} == bits 0 and 2 set == 0x05,
        // RenderPath::Shadow == 3, LODTier::Mobile == 0.
        PermutationKey k{};
        k.shading_model = ShadingModel::Hair;
        k.features.set(FeatureBit::Skinned);
        k.features.set(FeatureBit::AlphaTest);
        k.render_path = RenderPath::Shadow;
        k.lod_tier    = LODTier::Mobile;

        auto b = k.to_bytes();
        CHECK(static_cast<std::uint8_t>(b[0]) == 2u);   // Hair
        CHECK(static_cast<std::uint8_t>(b[1]) == 0x05u); // Skinned|AlphaTest
        CHECK(static_cast<std::uint8_t>(b[2]) == 0u);    // feat high byte always 0
        CHECK(static_cast<std::uint8_t>(b[3]) == 3u);    // Shadow
        CHECK(static_cast<std::uint8_t>(b[4]) == 0u);    // Mobile
        CHECK(static_cast<std::uint8_t>(b[5]) == 0u);    // reserved

        // Round-trip back.
        auto rt = PermutationKey::from_bytes(b);
        REQUIRE(rt.has_value());
        CHECK(rt.value() == k);
    }
}

// ---------------------------------------------------------------------------
// Test: permutation_key_from_bytes_rejects_out_of_range_with_PermutationKeyMalformed
//
// Verifies §4.2 invariant 1 — from_bytes() rejects malformed inputs.
// ---------------------------------------------------------------------------

TEST_CASE(
    "permutation_key_from_bytes_rejects_out_of_range_with_PermutationKeyMalformed",
    "[shader][permutation]"
) {
    using BA = PermutationKey::PackedBytes;

    SECTION("ShadingModel out of range") {
        BA b{};
        b[0] = std::byte{static_cast<std::uint8_t>(kShadingModelCount)};  // == 8, out of range
        auto r = PermutationKey::from_bytes(b);
        REQUIRE(!r.has_value());
        const auto* shader_err = std::get_if<glibre::shader::Error>(&r.error().code());
        REQUIRE(shader_err != nullptr);
        CHECK(*shader_err == glibre::shader::Error::PermutationKeyMalformed);
    }

    SECTION("FeatureSet reserved bits set (bit 6 is reserved)") {
        BA b{};
        b[0] = std::byte{0u};           // ShadingModel::Standard
        b[1] = std::byte{0x40u};        // bit 6 set — reserved
        b[3] = std::byte{0u};           // RenderPath::Forward
        b[4] = std::byte{2u};           // LODTier::Desktop
        auto r = PermutationKey::from_bytes(b);
        REQUIRE(!r.has_value());
        const auto* shader_err = std::get_if<glibre::shader::Error>(&r.error().code());
        REQUIRE(shader_err != nullptr);
        CHECK(*shader_err == glibre::shader::Error::PermutationKeyMalformed);
    }

    SECTION("FeatureSet high byte non-zero") {
        BA b{};
        b[2] = std::byte{0x01u};        // feat_hi != 0 — reserved
        auto r = PermutationKey::from_bytes(b);
        REQUIRE(!r.has_value());
        const auto* shader_err = std::get_if<glibre::shader::Error>(&r.error().code());
        REQUIRE(shader_err != nullptr);
        CHECK(*shader_err == glibre::shader::Error::PermutationKeyMalformed);
    }

    SECTION("RenderPath out of range") {
        BA b{};
        b[3] = std::byte{static_cast<std::uint8_t>(kRenderPathCount)};  // == 6
        auto r = PermutationKey::from_bytes(b);
        REQUIRE(!r.has_value());
        const auto* shader_err = std::get_if<glibre::shader::Error>(&r.error().code());
        REQUIRE(shader_err != nullptr);
        CHECK(*shader_err == glibre::shader::Error::PermutationKeyMalformed);
    }

    SECTION("LODTier out of range") {
        BA b{};
        b[4] = std::byte{static_cast<std::uint8_t>(kLODTierCount)};  // == 4
        auto r = PermutationKey::from_bytes(b);
        REQUIRE(!r.has_value());
        const auto* shader_err = std::get_if<glibre::shader::Error>(&r.error().code());
        REQUIRE(shader_err != nullptr);
        CHECK(*shader_err == glibre::shader::Error::PermutationKeyMalformed);
    }

    SECTION("reserved byte non-zero") {
        BA b{};
        b[4] = std::byte{2u};  // LODTier::Desktop (keep in range)
        b[5] = std::byte{0x01u};
        auto r = PermutationKey::from_bytes(b);
        REQUIRE(!r.has_value());
        const auto* shader_err = std::get_if<glibre::shader::Error>(&r.error().code());
        REQUIRE(shader_err != nullptr);
        CHECK(*shader_err == glibre::shader::Error::PermutationKeyMalformed);
    }
}

// ---------------------------------------------------------------------------
// Test: permutation_index_ordering_matches_tuple_field_ascending_order
//
// Verifies §4.2 invariant 2 — the index order is lexicographic on the tuple
// (ShadingModel, FeatureSet, RenderPath, LODTier) ascending.
// ---------------------------------------------------------------------------

TEST_CASE(
    "permutation_index_ordering_matches_tuple_field_ascending_order",
    "[shader][permutation]"
) {
    SECTION("increasing ShadingModel increases index") {
        PermutationKey a{}, b{};
        a.shading_model = ShadingModel::Standard;  // 0
        b.shading_model = ShadingModel::Skin;      // 1
        a.lod_tier = b.lod_tier = LODTier::Desktop;
        CHECK(to_index(a).value < to_index(b).value);
    }

    SECTION("increasing FeatureSet increases index (same SM, RP, LOD)") {
        PermutationKey a{}, b{};
        a.lod_tier = b.lod_tier = LODTier::Desktop;
        a.features = FeatureSet{0u};
        b.features = FeatureSet{1u};
        CHECK(to_index(a).value < to_index(b).value);
    }

    SECTION("increasing RenderPath increases index (same SM, feat, LOD)") {
        PermutationKey a{}, b{};
        a.lod_tier = b.lod_tier = LODTier::Desktop;
        a.render_path = RenderPath::Forward;    // 0
        b.render_path = RenderPath::Deferred;   // 1
        CHECK(to_index(a).value < to_index(b).value);
    }

    SECTION("increasing LODTier increases index (same SM, feat, RP)") {
        PermutationKey a{}, b{};
        a.lod_tier = LODTier::Mobile;  // 0
        b.lod_tier = LODTier::Switch;  // 1
        CHECK(to_index(a).value < to_index(b).value);
    }

    SECTION("ShadingModel dominates: higher SM beats lower feat/rp/lod") {
        PermutationKey a{}, b{};
        a.shading_model = ShadingModel::Standard;
        a.features      = FeatureSet{static_cast<std::uint16_t>((1u << kFeatureBitCount) - 1u)};
        a.render_path   = static_cast<RenderPath>(kRenderPathCount - 1);
        a.lod_tier      = static_cast<LODTier>(kLODTierCount - 1);

        b.shading_model = ShadingModel::Skin;  // one step up
        b.features      = FeatureSet{0u};
        b.render_path   = RenderPath::Forward;
        b.lod_tier      = LODTier::Mobile;

        CHECK(to_index(a).value < to_index(b).value);
    }

    SECTION("indices span [0, kPermutationCrossProductCardinality) densely") {
        // The first key in tuple-field order should produce index 0.
        PermutationKey first{};
        first.shading_model = static_cast<ShadingModel>(0u);
        first.features      = FeatureSet{0u};
        first.render_path   = static_cast<RenderPath>(0u);
        first.lod_tier      = static_cast<LODTier>(0u);
        CHECK(to_index(first).value == 0u);

        // The last key should produce index kPermutationCrossProductCardinality - 1.
        PermutationKey last{};
        last.shading_model = static_cast<ShadingModel>(kShadingModelCount - 1u);
        last.features      = FeatureSet{static_cast<std::uint16_t>((1u << kFeatureBitCount) - 1u)};
        last.render_path   = static_cast<RenderPath>(kRenderPathCount - 1u);
        last.lod_tier      = static_cast<LODTier>(kLODTierCount - 1u);
        CHECK(to_index(last).value == kPermutationCrossProductCardinality - 1u);
    }
}

// ---------------------------------------------------------------------------
// Test: permutation_index_round_trips_through_key_bijectively
//
// Verifies the bijection property: to_index and from_index are inverses.
// ---------------------------------------------------------------------------

TEST_CASE(
    "permutation_index_round_trips_through_key_bijectively",
    "[shader][permutation]"
) {
    SECTION("every index in [0, kPermutationCrossProductCardinality) decodes and re-encodes") {
        for (std::uint32_t i = 0; i < kPermutationCrossProductCardinality; ++i) {
            auto key_result = from_index(PermutationIndex{i});
            REQUIRE(key_result.has_value());
            const PermutationKey& k = key_result.value();
            CHECK(to_index(k).value == i);
        }
    }

    SECTION("from_index out of range returns PermutationKeyOutOfRange") {
        auto r = from_index(PermutationIndex{kPermutationCrossProductCardinality});
        REQUIRE(!r.has_value());
        const auto* shader_err = std::get_if<glibre::shader::Error>(&r.error().code());
        REQUIRE(shader_err != nullptr);
        CHECK(*shader_err == glibre::shader::Error::PermutationKeyOutOfRange);
    }

    SECTION("from_index with max uint32 returns PermutationKeyOutOfRange") {
        auto r = from_index(PermutationIndex{0xFFFF'FFFFu});
        REQUIRE(!r.has_value());
        const auto* shader_err = std::get_if<glibre::shader::Error>(&r.error().code());
        REQUIRE(shader_err != nullptr);
        CHECK(*shader_err == glibre::shader::Error::PermutationKeyOutOfRange);
    }
}

// ---------------------------------------------------------------------------
// Test: enumeration_table_yields_full_cross_product_in_canonical_order
//
// Verifies that EnumerationTable::walk_all() visits every key in the cross-
// product exactly once, in tuple-field ascending order, with no duplicates
// and no omissions.
// ---------------------------------------------------------------------------

TEST_CASE(
    "enumeration_table_yields_full_cross_product_in_canonical_order",
    "[shader][permutation]"
) {
    permutation::EnumerationTable table;

    SECTION("walk_all visits every key in canonical order") {
        std::vector<PermutationKey> visited;
        visited.reserve(kPermutationCrossProductCardinality);
        table.walk_all([&](const PermutationKey& k) { visited.push_back(k); });

        // Total count.
        REQUIRE(visited.size() == kPermutationCrossProductCardinality);

        // Verify ascending index order.
        for (std::size_t i = 0; i + 1 < visited.size(); ++i) {
            CHECK(to_index(visited[i]).value < to_index(visited[i + 1]).value);
        }

        // Verify the ith key matches from_index(i).
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(visited.size()); ++i) {
            auto expected = from_index(PermutationIndex{i});
            REQUIRE(expected.has_value());
            CHECK(visited[i] == expected.value());
        }
    }

    SECTION("pruned walk visits only keys passing the predicate") {
        // Only accept keys with no features set (FeatureSet == 0).
        std::vector<PermutationKey> visited;
        table.walk(
            [](const PermutationKey& k) noexcept { return k.features.bits() == 0u; },
            [&](const PermutationKey& k) { visited.push_back(k); }
        );

        // Expected count: kShadingModelCount * 1 * kRenderPathCount * kLODTierCount.
        const std::size_t expected_count =
            kShadingModelCount * kRenderPathCount * kLODTierCount;
        CHECK(visited.size() == expected_count);

        // All visited keys must have no features.
        for (const auto& k : visited) {
            CHECK(k.features.bits() == 0u);
        }
    }

    SECTION("walk with reject-all predicate visits nothing") {
        std::size_t count = 0u;
        table.walk(
            [](const PermutationKey&) noexcept { return false; },
            [&count](const PermutationKey&) noexcept { ++count; }
        );
        CHECK(count == 0u);
    }

    SECTION("walk_all produces no duplicates") {
        std::vector<std::uint32_t> indices;
        indices.reserve(kPermutationCrossProductCardinality);
        table.walk_all([&](const PermutationKey& k) {
            indices.push_back(to_index(k).value);
        });
        std::sort(indices.begin(), indices.end());
        auto it = std::unique(indices.begin(), indices.end());
        CHECK(std::distance(indices.begin(), it)
            == static_cast<std::ptrdiff_t>(kPermutationCrossProductCardinality));
    }
}

// ---------------------------------------------------------------------------
// Test: feature_set_bit_positions_are_stable_across_revs
//
// Verifies §4.2 invariant 3 — the FeatureBit positions are pinned to the
// values declared in the spec.  This test fails (by design) if a FeatureBit
// enumerator is repositioned without a spec amendment.
// ---------------------------------------------------------------------------

TEST_CASE(
    "feature_set_bit_positions_are_stable_across_revs",
    "[shader][permutation]"
) {
    // Bit positions per spec §2 / §5 public header.
    SECTION("Skinned is bit 0") {
        CHECK(static_cast<std::uint8_t>(FeatureBit::Skinned) == 0u);
        FeatureSet fs{};
        fs.set(FeatureBit::Skinned);
        CHECK(fs.bits() == 0x0001u);
    }

    SECTION("MotionVectors is bit 1") {
        CHECK(static_cast<std::uint8_t>(FeatureBit::MotionVectors) == 1u);
        FeatureSet fs{};
        fs.set(FeatureBit::MotionVectors);
        CHECK(fs.bits() == 0x0002u);
    }

    SECTION("AlphaTest is bit 2") {
        CHECK(static_cast<std::uint8_t>(FeatureBit::AlphaTest) == 2u);
        FeatureSet fs{};
        fs.set(FeatureBit::AlphaTest);
        CHECK(fs.bits() == 0x0004u);
    }

    SECTION("Decal is bit 3") {
        CHECK(static_cast<std::uint8_t>(FeatureBit::Decal) == 3u);
        FeatureSet fs{};
        fs.set(FeatureBit::Decal);
        CHECK(fs.bits() == 0x0008u);
    }

    SECTION("VirtualTexture is bit 4") {
        CHECK(static_cast<std::uint8_t>(FeatureBit::VirtualTexture) == 4u);
        FeatureSet fs{};
        fs.set(FeatureBit::VirtualTexture);
        CHECK(fs.bits() == 0x0010u);
    }

    SECTION("RT is bit 5") {
        CHECK(static_cast<std::uint8_t>(FeatureBit::RT) == 5u);
        FeatureSet fs{};
        fs.set(FeatureBit::RT);
        CHECK(fs.bits() == 0x0020u);
    }

    SECTION("kFeatureBitCount is 6") {
        CHECK(kFeatureBitCount == 6u);
    }

    SECTION("kFeatureBitMask is 0x003F") {
        CHECK(kFeatureBitMask == 0x003Fu);
    }

    SECTION("all features set produces correct mask") {
        FeatureSet fs{};
        fs.set(FeatureBit::Skinned);
        fs.set(FeatureBit::MotionVectors);
        fs.set(FeatureBit::AlphaTest);
        fs.set(FeatureBit::Decal);
        fs.set(FeatureBit::VirtualTexture);
        fs.set(FeatureBit::RT);
        CHECK(fs.bits() == kFeatureBitMask);
    }

    SECTION("kPermutationCrossProductCardinality == 12288") {
        // 8 * 64 * 6 * 4 = 12288
        CHECK(kPermutationCrossProductCardinality == 12'288u);
    }
}
