#pragma once
// plugins/shader/src/permutation/axes.hpp
//
// Re-export facade: pulls the 4-axis permutation types from the public boundary
// into the permutation/ private include namespace.
//
// SRP: "Provide a single-include façade for permutation/ TUs that need the
//       four axis types (ShadingModel, FeatureSet, RenderPath, LODTier) and
//       their cardinality constants without pulling in the full public surface."
//
// Authority: specs/shader/SPEC.md §2, §4.2 invariant 3.
//
// This file declares no new types.  All types and constants
// (ShadingModel, FeatureBit, FeatureSet, RenderPath, LODTier,
// kShadingModelCount, kFeatureBitCount, kFeatureBitMask, kRenderPathCount,
// kLODTierCount, PermutationKey, PermutationIndex,
// kPermutationCrossProductCardinality) originate in
// include/glibre/shader/shader.hpp and are re-exported here verbatim.
//
// NOTE: Do NOT include this header from outside the permutation/ directory —
//       include <glibre/shader/shader.hpp> instead.

// Pull in the authoritative declarations from the public boundary.
#include <glibre/shader/shader.hpp>
