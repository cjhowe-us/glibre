// plugins/shader/src/permutation/packed_key.cpp
//
// PermutationKey packed codec — to_bytes() and from_bytes().
//
// Authority: specs/shader/SPEC.md §4.2 invariant 1.
//
// Packed layout:
//   byte[0] : ShadingModel  (uint8, 0..kShadingModelCount-1)
//   byte[1] : FeatureSet bits 0..7    (low byte, bits 6..7 = 0 while kFeatureBitCount==6)
//   byte[2] : FeatureSet bits 8..15   (high byte; always 0 while kFeatureBitCount <= 8)
//   byte[3] : RenderPath   (uint8, 0..kRenderPathCount-1)
//   byte[4] : LODTier      (uint8, 0..kLODTierCount-1)
//   byte[5] : reserved     (always 0x00)
//
// The layout is host-endian independent because each axis occupies exactly
// one or two bytes and is laid out by explicit index.

#include "packed_key.hpp"

#include <glibre/error.hpp>

namespace glibre::shader::permutation {

PermutationKey::PackedBytes packed_key_to_bytes(const PermutationKey& key) noexcept {
    PermutationKey::PackedBytes out{};
    out[0] = static_cast<std::byte>(static_cast<std::uint8_t>(key.shading_model));
    // FeatureSet is uint16; lay out low byte then high byte explicitly.
    const std::uint16_t feat_bits = key.features.bits();
    out[1] = static_cast<std::byte>(static_cast<std::uint8_t>(feat_bits & 0x00FFu));
    out[2] = static_cast<std::byte>(static_cast<std::uint8_t>((feat_bits >> 8u) & 0x00FFu));
    out[3] = static_cast<std::byte>(static_cast<std::uint8_t>(key.render_path));
    out[4] = static_cast<std::byte>(static_cast<std::uint8_t>(key.lod_tier));
    out[5] = std::byte{0x00};
    return out;
}

glibre::Result<PermutationKey>
packed_key_from_bytes(const PermutationKey::PackedBytes& bytes) noexcept {
    using enum glibre::shader::Error;

    const auto sm_raw = static_cast<std::uint8_t>(bytes[0]);
    const auto feat_lo = static_cast<std::uint8_t>(bytes[1]);
    const auto feat_hi = static_cast<std::uint8_t>(bytes[2]);
    const auto rp_raw = static_cast<std::uint8_t>(bytes[3]);
    const auto lod_raw = static_cast<std::uint8_t>(bytes[4]);
    const auto rsv = static_cast<std::uint8_t>(bytes[5]);

    // Validate ShadingModel range.
    if (sm_raw >= static_cast<std::uint8_t>(kShadingModelCount)) {
        return std::unexpected(glibre::Error{PermutationKeyMalformed});
    }
    // Validate FeatureSet — no reserved bits set (feat_hi must be 0 while
    // kFeatureBitCount <= 8; feat_lo must not set bits above kFeatureBitMask).
    if (feat_hi != 0u) {
        return std::unexpected(glibre::Error{PermutationKeyMalformed});
    }
    if ((feat_lo & static_cast<std::uint8_t>(~kFeatureBitMask)) != 0u) {
        return std::unexpected(glibre::Error{PermutationKeyMalformed});
    }
    // Validate RenderPath range.
    if (rp_raw >= static_cast<std::uint8_t>(kRenderPathCount)) {
        return std::unexpected(glibre::Error{PermutationKeyMalformed});
    }
    // Validate LODTier range.
    if (lod_raw >= static_cast<std::uint8_t>(kLODTierCount)) {
        return std::unexpected(glibre::Error{PermutationKeyMalformed});
    }
    // Reserved byte must be zero.
    if (rsv != 0u) {
        return std::unexpected(glibre::Error{PermutationKeyMalformed});
    }

    PermutationKey key{};
    key.shading_model = static_cast<ShadingModel>(sm_raw);
    key.features = FeatureSet{static_cast<std::uint16_t>(feat_lo)};
    key.render_path = static_cast<RenderPath>(rp_raw);
    key.lod_tier = static_cast<LODTier>(lod_raw);
    return key;
}

}  // namespace glibre::shader::permutation

// ---------------------------------------------------------------------------
// PermutationKey public member implementations (defined in shader.hpp).
// These free-standing implementations wire the permutation:: helpers into
// the public type.
// ---------------------------------------------------------------------------

namespace glibre::shader {

PermutationKey::PackedBytes PermutationKey::to_bytes() const noexcept {
    return permutation::packed_key_to_bytes(*this);
}

glibre::Result<PermutationKey> PermutationKey::from_bytes(const PackedBytes& bytes) noexcept {
    return permutation::packed_key_from_bytes(bytes);
}

bool PermutationKey::is_well_formed() const noexcept {
    const auto sm_raw = static_cast<std::uint8_t>(shading_model);
    const auto rp_raw = static_cast<std::uint8_t>(render_path);
    const auto lod_raw = static_cast<std::uint8_t>(lod_tier);
    // FeatureSet high byte is conceptually 0 because kFeatureBitCount <= 8.
    // The accessor bits() returns a uint16; bits above kFeatureBitMask must be 0.
    return sm_raw < static_cast<std::uint8_t>(kShadingModelCount) &&
           (features.bits() & ~kFeatureBitMask) == 0u &&
           rp_raw < static_cast<std::uint8_t>(kRenderPathCount) &&
           lod_raw < static_cast<std::uint8_t>(kLODTierCount);
}

bool permutation_key_byte_less(const PermutationKey& a, const PermutationKey& b) noexcept {
    const auto ab = a.to_bytes();
    const auto bb = b.to_bytes();
    return ab < bb;
}

}  // namespace glibre::shader
