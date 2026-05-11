#pragma once
// plugins/shader/src/permutation/packed_key.hpp
//
// PermutationKey::to_bytes() and ::from_bytes() — bit-stable packed encoding.
//
// SRP: "Implement the total, injective, bit-stable round-trip between a
//       well-formed PermutationKey and its 6-byte canonical encoding."
//
// Authority: specs/shader/SPEC.md §4.2 invariants 1–3.
//
// Packed layout (little-endian, 6 bytes total):
//   byte[0] : ShadingModel (uint8 — values 0..7)
//   byte[1] : FeatureSet low byte  (bits 0..5 defined; bits 6..15 reserved = 0)
//   byte[2] : FeatureSet high byte (always 0 while kFeatureBitCount <= 8)
//   byte[3] : RenderPath  (uint8 — values 0..5)
//   byte[4] : LODTier     (uint8 — values 0..3)
//   byte[5] : reserved    = 0x00
//
// Invariants enforced by from_bytes():
//   - byte[0] in [0, kShadingModelCount)         → Error::PermutationKeyMalformed
//   - byte[1] & ~kFeatureBitMask == 0            → Error::PermutationKeyMalformed
//   - byte[2] == 0                                → Error::PermutationKeyMalformed
//   - byte[3] in [0, kRenderPathCount)           → Error::PermutationKeyMalformed
//   - byte[4] in [0, kLODTierCount)              → Error::PermutationKeyMalformed
//   - byte[5] == 0                                → Error::PermutationKeyMalformed
//
// NOTE: Do NOT include this header from outside the permutation/ directory.
//       The public PermutationKey::to_bytes / from_bytes declarations live in
//       include/glibre/shader/shader.hpp.

#include "axes.hpp"

namespace glibre::shader::permutation {

// Implement PermutationKey::to_bytes().
// Returns a 6-byte little-endian packed representation.
// Always succeeds for a well-formed key.
PermutationKey::PackedBytes packed_key_to_bytes(const PermutationKey& key) noexcept;

// Implement PermutationKey::from_bytes().
// Returns Error::PermutationKeyMalformed if any reserved bits or out-of-range
// values are detected.
glibre::Result<PermutationKey>
packed_key_from_bytes(const PermutationKey::PackedBytes& bytes) noexcept;

}  // namespace glibre::shader::permutation
