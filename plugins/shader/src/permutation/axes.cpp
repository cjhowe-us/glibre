// plugins/shader/src/permutation/axes.cpp
//
// Axis-validity predicate for the 4-axis permutation key.
//
// SRP: "Validate that every field of a PermutationKey falls within its
//       respective axis cardinality — no byte-layout or codec dependency."
//
// Authority: specs/shader/SPEC.md §4.2 invariant 3.
//
// is_well_formed() lives here rather than in packed_key.cpp because it has no
// dependency on the byte-layout encoding (packed_key.cpp's SRP).  Moving it here
// makes the separation explicit: axis cardinality checks belong with axis
// definitions (axes.hpp), not with the codec that serialises them.

#include "axes.hpp"

namespace glibre::shader {

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

}  // namespace glibre::shader
