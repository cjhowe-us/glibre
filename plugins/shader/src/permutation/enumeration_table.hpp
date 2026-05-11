#pragma once
// plugins/shader/src/permutation/enumeration_table.hpp
//
// Build-time enumerator that yields the (project-pruned) PermutationKey set
// in canonical ascending order.
//
// SRP: "Walk the cross-product, apply a caller-supplied pruning predicate,
//       and yield the surviving keys in tuple-field ascending order."
//
// Authority: specs/shader/SPEC.md §6.1 (build-time enumeration), §4.2 invariant 2.
//
// The table is an offline / tooling construct; it is included in shipping builds
// per the §6.1 table (permutation/ is listed as "included" in shipping) but
// the cooker that drives it is excluded.  EnumerationTable itself is a pure
// value-computing type with no subprocess or IO dependency, so including it
// in shipping is safe.
//
// Usage (full cross-product, no pruning):
//
//   EnumerationTable table;
//   table.walk(
//       [](const PermutationKey&) { return true; },   // accept all
//       [](const PermutationKey& k) { /* process k */ }
//   );
//
// Usage (pruned):
//
//   EnumerationTable table;
//   table.walk(
//       [](const PermutationKey& k) {
//           // Reject Mobile tier for Skin shading model.
//           return !(k.shading_model == ShadingModel::Skin
//                    && k.lod_tier == LODTier::Mobile);
//       },
//       [&](const PermutationKey& k) { keys.push_back(k); }
//   );
//
// The caller-supplied Predicate must be callable as bool(const PermutationKey&).
// The caller-supplied Visitor  must be callable as void(const PermutationKey&).
//
// Iteration order is canonical tuple-field ascending:
//   outer → ShadingModel (0..kShadingModelCount-1)
//   then  → FeatureSet   (0..kFeatureSetCardinality-1)
//   then  → RenderPath   (0..kRenderPathCount-1)
//   inner → LODTier      (0..kLODTierCount-1)
//
// NOTE: Do NOT include this header from outside the permutation/ directory
//       or from tests that do not add the PRIVATE include path for the
//       permutation/ directory.

#include <concepts>
#include <functional>

#include "axes.hpp"
#include "permutation_index.hpp"

namespace glibre::shader::permutation {

// ---------------------------------------------------------------------------
// Pruning predicate concept — callable as bool(const PermutationKey&).
// ---------------------------------------------------------------------------

template<typename F>
concept Pruner = std::invocable<F, const PermutationKey&> &&
                 std::same_as<std::invoke_result_t<F, const PermutationKey&>, bool>;

// ---------------------------------------------------------------------------
// Visitor concept — callable as void(const PermutationKey&).
// ---------------------------------------------------------------------------

template<typename F>
concept Visitor = std::invocable<F, const PermutationKey&>;

// ---------------------------------------------------------------------------
// EnumerationTable
//
// Stateless.  walk() iterates the full cross-product in tuple-field order,
// calls pruner(key) for each, and calls visitor(key) for those that pass.
// ---------------------------------------------------------------------------

class EnumerationTable {
public:
    // Walk the full cross-product.  For each key K (in canonical ascending
    // tuple-field order):
    //   1. Call pruner(K).  If false, skip K.
    //   2. Call visitor(K).
    //
    // Iteration order:
    //   for sm in [0, kShadingModelCount):
    //     for feat in [0, kFeatureSetCardinality):
    //       for rp in [0, kRenderPathCount):
    //         for lod in [0, kLODTierCount):
    //           key = { sm, feat, rp, lod }
    //           if pruner(key): visitor(key)
    template<Pruner P, Visitor V>
    void walk(P&& pruner, V&& visitor) const noexcept(
        noexcept(pruner(std::declval<const PermutationKey&>())) &&
        noexcept(visitor(std::declval<const PermutationKey&>()))
    ) {
        for (std::uint32_t sm = 0; sm < kShadingModelCount; ++sm) {
            for (std::uint32_t feat = 0; feat < kFeatureSetCardinality; ++feat) {
                for (std::uint32_t rp = 0; rp < kRenderPathCount; ++rp) {
                    for (std::uint32_t lod = 0; lod < kLODTierCount; ++lod) {
                        PermutationKey k{};
                        k.shading_model = static_cast<ShadingModel>(static_cast<std::uint8_t>(sm));
                        k.features = FeatureSet{static_cast<std::uint16_t>(feat)};
                        k.render_path = static_cast<RenderPath>(static_cast<std::uint8_t>(rp));
                        k.lod_tier = static_cast<LODTier>(static_cast<std::uint8_t>(lod));
                        if (pruner(k)) {
                            visitor(k);
                        }
                    }
                }
            }
        }
    }

    // Convenience overload: walk without pruning (accept all).
    template<Visitor V>
    void
    walk_all(V&& visitor) const noexcept(noexcept(visitor(std::declval<const PermutationKey&>()))) {
        walk([](const PermutationKey&) noexcept { return true; }, std::forward<V>(visitor));
    }
};

}  // namespace glibre::shader::permutation
