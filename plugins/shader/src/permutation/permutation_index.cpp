// plugins/shader/src/permutation/permutation_index.cpp
//
// Dense ordinal bijection for the 4-axis permutation cross-product.
//
// Authority: specs/shader/SPEC.md §4.2 invariant 2.
//
// Mixed-radix layout (tuple-field order: ShadingModel, FeatureSet, RenderPath, LODTier):
//   index = sm_raw * kShadingStride
//         + feat   * kFeatureStride
//         + rp_raw * kRenderStride
//         + lod_raw * kLODStride
//
// Decode reverses each field via successive integer division + modulo, the
// same tuple-field order.

#include "permutation_index.hpp"

#include <glibre/error.hpp>

namespace glibre::shader::permutation {

PermutationIndex permutation_index_encode(const PermutationKey& key) noexcept {
    const std::uint32_t sm =
        static_cast<std::uint32_t>(static_cast<std::uint8_t>(key.shading_model));
    const std::uint32_t feat = static_cast<std::uint32_t>(key.features.bits());
    const std::uint32_t rp = static_cast<std::uint32_t>(static_cast<std::uint8_t>(key.render_path));
    const std::uint32_t lod = static_cast<std::uint32_t>(static_cast<std::uint8_t>(key.lod_tier));

    return PermutationIndex{
        sm * kShadingStride + feat * kFeatureStride + rp * kRenderStride + lod * kLODStride
    };
}

glibre::Result<PermutationKey> permutation_index_decode(const PermutationIndex& idx) noexcept {
    if (idx.value >= kPermutationCrossProductCardinality) {
        return std::unexpected(glibre::Error{glibre::shader::Error::PermutationKeyOutOfRange});
    }

    std::uint32_t remaining = idx.value;

    const std::uint32_t sm_raw = remaining / kShadingStride;
    remaining %= kShadingStride;

    const std::uint32_t feat_raw = remaining / kFeatureStride;
    remaining %= kFeatureStride;

    const std::uint32_t rp_raw = remaining / kRenderStride;
    remaining %= kRenderStride;

    const std::uint32_t lod_raw = remaining / kLODStride;

    PermutationKey key{};
    key.shading_model = static_cast<ShadingModel>(static_cast<std::uint8_t>(sm_raw));
    key.features = FeatureSet{static_cast<std::uint16_t>(feat_raw)};
    key.render_path = static_cast<RenderPath>(static_cast<std::uint8_t>(rp_raw));
    key.lod_tier = static_cast<LODTier>(static_cast<std::uint8_t>(lod_raw));
    return key;
}

}  // namespace glibre::shader::permutation

// ---------------------------------------------------------------------------
// Public free functions declared in shader.hpp — wire into permutation:: helpers.
// ---------------------------------------------------------------------------

namespace glibre::shader {

PermutationIndex to_index(const PermutationKey& key) noexcept {
    return permutation::permutation_index_encode(key);
}

glibre::Result<PermutationKey> from_index(const PermutationIndex& idx) noexcept {
    return permutation::permutation_index_decode(idx);
}

}  // namespace glibre::shader
