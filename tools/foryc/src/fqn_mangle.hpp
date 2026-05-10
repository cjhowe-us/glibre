// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/fqn_mangle.hpp
//
// Shared FQN-mangling helpers for glibre-foryc emitters (plan #1010).
//
// Extracted so that parser.cpp, emit_migration.cpp, and future emitters
// (emit_header, emit_manifest) can all reuse the same "no '__' in segment"
// invariant and the same dot-to-double-underscore transformation without
// code duplication.
//
// fory-codegen.md §"ABI Stability Rules" point 4:
//   Dots in a dotted FQN are replaced by "__" to form C symbol suffixes.
//   Any identifier segment that already contains "__" would alias a
//   multi-segment FQN, making fqn_to_mangled non-injective.  Callers must
//   validate every segment before mangling via segment_contains_double_underscore.
//
// Usage (in any foryc emitter):
//
//   #include "fqn_mangle.hpp"
//   ...
//   if (fqn_mangle::segment_contains_double_underscore(seg))
//       return std::unexpected{glibre::Error{tools::Error::ForycInvalidIdentifier}};
//   ...
//   const std::string mangled = fqn_mangle::fqn_to_mangled(td.fqn);
//
// PHILOSOPHY §11: libc++ stdlib is canonical. Tools are one-shot CLIs —
// plain std::string (no PMR).

#pragma once

#include <string>
#include <string_view>

namespace glibre::tools::foryc::fqn_mangle {

// segment_contains_double_underscore — return true if seg contains "__".
//
// "__" (double-underscore) is reserved by the codegen mangling scheme
// (fory-codegen.md §"ABI Stability Rules" point 4, plan #1010): dots in a
// dotted FQN are replaced by "__" to form C symbol suffixes.  A segment
// that already contains "__" would alias a multi-segment FQN, making
// fqn_to_mangled non-injective and defeating collision-prevention.
//
// Called by parse_fqn() at parse time and by emit_migration()'s per-TypeDecl
// loop for defensive re-validation of synthetic IR built without going through
// the parser.
[[nodiscard]] inline bool segment_contains_double_underscore(std::string_view seg) noexcept {
    const std::size_t sz = seg.size();
    for (std::size_t i = 0; i + 1 < sz; ++i) {
        if (seg[i] == '_' && seg[i + 1] == '_')
            return true;
    }
    return false;
}

// fqn_to_mangled — mangle a dotted FQN into a C-symbol-safe string.
//
// Replaces every '.' with '__' (double-underscore) so that two types with
// the same unqualified name but different FQNs produce distinct C symbols
// (fory-codegen.md §"ABI Stability Rules" point 4, plan #1010).
//
// Pre-condition: no segment of fqn contains "__" (enforced by
// segment_contains_double_underscore; caller must have checked first).
//
// Examples:
//   "glibre.core.Transform" → "glibre__core__Transform"
//   "glibre.Transform"      → "glibre__Transform"
//   "Transform"             → "Transform"   (single segment: no dots, no change)
[[nodiscard]] inline std::string fqn_to_mangled(const std::string& fqn) noexcept {
    std::string out;
    out.reserve(fqn.size() * 2);  // upper bound: each char emits at most 2 chars ('.' → "__")
    for (std::size_t i = 0; i < fqn.size(); ++i) {
        if (fqn[i] == '.') {
            out += "__";
        } else {
            out += fqn[i];
        }
    }
    return out;
}

}  // namespace glibre::tools::foryc::fqn_mangle
