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
// Iteration order is defined by the mixed-radix bijection in permutation_index.hpp
// (§4.2 invariant 2 — single source of truth).  walk() iterates ordinals
// [0, kPermutationCrossProductCardinality) and decodes each via
// permutation_index_decode, so the iteration order is always the same as the
// index order — no independent field loop is needed.
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
    // Iteration is driven by permutation_index_decode over
    // [0, kPermutationCrossProductCardinality), keeping the index bijection
    // (§4.2 invariant 2) as the single source of truth for field ordering.
    // operator* on a valid Result<PermutationKey> is noexcept (std::expected).
    template<Pruner P, Visitor V>
    void walk(P&& pruner, V&& visitor) const noexcept(
        noexcept(pruner(std::declval<const PermutationKey&>())) &&
        noexcept(visitor(std::declval<const PermutationKey&>()))
    ) {
        for (PermutationIndex i{0}; i.value < kPermutationCrossProductCardinality; ++i.value) {
            // permutation_index_decode is noexcept and always succeeds for
            // i.value in [0, kPermutationCrossProductCardinality).
            const PermutationKey k = *permutation_index_decode(i);
            if (pruner(k)) {
                visitor(k);
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
