#pragma once
// plugins/shader/src/permutation/axes.hpp
//
// Closed enums for the 4-axis permutation key and their cardinalities.
// Mirrors the declarations in the public shader.hpp boundary.
//
// SRP: "Declare and cardinality-annotate the four axes of the permutation
//       cross-product."
//
// Authority: specs/shader/SPEC.md §2, §4.2 invariant 3.
//
// Each closed enum has a documented cardinality constant.  FeatureSet bit
// positions are fixed; adding a FeatureBit or ShadingModel enumerator is a
// spec amendment (§4.2 invariant 3).
//
// NOTE: The types declared here are re-exported from the public boundary
// (include/glibre/shader/shader.hpp).  This private header lets the
// permutation/ implementation TUs refer to the axes without pulling in the
// full public surface.  Do NOT include this header from outside the
// permutation/ directory — include <glibre/shader/shader.hpp> instead.

// Pull in the authoritative declarations from the public boundary.
// All types (ShadingModel, FeatureBit, FeatureSet, RenderPath, LODTier, and
// their cardinality constants) are declared there.  This header exists as an
// SRP-aligned entry point for the permutation/ TU set.
#include <glibre/shader/shader.hpp>
