#pragma once
// plugins/shader/src/permutation/permutation_index.hpp
//
// Dense ordinal codec for the 4-axis permutation cross-product.
//
// SRP: "Compute the bijection between PermutationKey and a dense
//       [0, kPermutationCrossProductCardinality) index using
//       mixed-radix encode in tuple-field order."
//
// Authority: specs/shader/SPEC.md §4.2 invariant 2.
//
// Mixed-radix layout (tuple-field order: ShadingModel, FeatureSet, RenderPath, LODTier):
//
//   index = sm * (kFeatureSetCardinality * kRenderPathCount * kLODTierCount)
//         + feat * (kRenderPathCount * kLODTierCount)
//         + rp   * kLODTierCount
//         + lod
//
//   where kFeatureSetCardinality = 1 << kFeatureBitCount = 64.
//
// Total: 8 * 64 * 6 * 4 = 12 288 = kPermutationCrossProductCardinality.
//
// NOTE: Do NOT include this header from outside the permutation/ directory.
//       The public to_index / from_index declarations live in shader.hpp.

#include "axes.hpp"

namespace glibre::shader::permutation {

// Mixed-radix cardinality of the FeatureSet axis (2^kFeatureBitCount).
inline constexpr std::uint32_t kFeatureSetCardinality = 1u << kFeatureBitCount;

// Step sizes for the mixed-radix encoding in tuple-field order.
inline constexpr std::uint32_t kLODStride     = 1u;
inline constexpr std::uint32_t kRenderStride  = kLODTierCount * kLODStride;
inline constexpr std::uint32_t kFeatureStride = kRenderPathCount * kRenderStride;
inline constexpr std::uint32_t kShadingStride = kFeatureSetCardinality * kFeatureStride;

// Encode a well-formed PermutationKey to its dense ordinal.
// Pre-condition: key.is_well_formed() must be true.
// Result is always in [0, kPermutationCrossProductCardinality).
PermutationIndex permutation_index_encode(const PermutationKey& key) noexcept;

// Decode a dense ordinal back to the corresponding PermutationKey.
// Returns Error::PermutationKeyOutOfRange when index.value >= kPermutationCrossProductCardinality.
glibre::Result<PermutationKey>
permutation_index_decode(const PermutationIndex& index) noexcept;

}  // namespace glibre::shader::permutation
