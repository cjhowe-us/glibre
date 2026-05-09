// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/abi_hash.hpp
//
// Schema source-hash and ABI hash computation for glibre-foryc (plan #222).
//
// Two independent hashes are computed per schema collection:
//
// 1. Per-schema source-hash (compute_source_hash)
//    blake3 over the *canonicalized* source bytes: whitespace-collapsed,
//    comments stripped, token order preserved.  Two schemas that are
//    semantically identical but differ in formatting yield the same
//    source-hash.  A field rename or type change always changes it.
//
// 2. Collection ABI hash (compute_abi_hash)
//    blake3 over the canonical IR walk: TypeDecls sorted by fqn in
//    Unicode code-point order; each type contributes
//      "<fqn>:<version_decimal>:<per-field-contribution>..."
//    where each field contributes "<tag_decimal>:<type_name>:<name>"
//    in tag-ascending order.  Fields that merely change in whitespace
//    but keep the same fqn/version/tag/type/name produce the same ABI
//    hash.  A field rename, type change, tag renumbering, or new field
//    always changes it.
//
// Helper:
//   format_as_uint64_hex — takes the first 8 bytes of a 32-byte digest
//   and returns a 16-character lowercase hex string for embedding as a
//   uint64_t constant in generated C++ headers.
//
// PHILOSOPHY §11: EASTL replaces std containers.
//   eastl::string, eastl::array, eastl::string_view for IR/return types.
//   std::expected (no eastl equivalent) for the Result alias.
//   std::string_view at public API boundaries (zero-copy interop).

#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

#include <EASTL/array.h>
#include <EASTL/string.h>

#include "glibre/error.hpp"
#include "parser.hpp"  // Schema, TypeDecl, FieldDecl

namespace glibre::tools::foryc {

// -----------------------------------------------------------------------
// Type aliases
// -----------------------------------------------------------------------

// 32-byte (256-bit) blake3 digest.
using Blake3Digest = eastl::array<uint8_t, 32>;

// -----------------------------------------------------------------------
// compute_source_hash
//
// Canonicalize `raw_source` (strip line-comments and collapse whitespace
// runs to single spaces; blank lines are elided).  Feed the canonical
// bytes into a fresh blake3 hasher and return the 32-byte digest.
//
// The canonical form is: tokens separated by a single space, newlines
// between top-level declarations.  Comments ("// ...") are removed.
// Leading/trailing whitespace on each token is removed.
//
// Return value: always succeeds unless blake3 is unavailable (which
// cannot happen at runtime — this function never touches I/O).
// The Result wrapper exists to be composable with the parse pipeline.
// -----------------------------------------------------------------------
[[nodiscard]] Result<Blake3Digest>
compute_source_hash(std::string_view raw_source) noexcept;

// -----------------------------------------------------------------------
// compute_abi_hash
//
// Walk all TypeDecls in `schema` (sorted by fqn in Unicode code-point
// order).  For each TypeDecl, visit all FieldDecls in tag-ascending
// order.  Feed the canonical representation into blake3 and return the
// 32-byte digest.
//
// Canonical representation fed to blake3:
//   For each TypeDecl (fqn-sorted):
//     feed "<fqn>:<version_decimal>\n"
//     For each FieldDecl (tag-sorted):
//       feed "  <tag_decimal>:<type_name>:<field_name>\n"
//   No trailing newline after the last entry.
//
// This representation is independent of:
//   - whitespace in the source file
//   - comment presence
//   - declaration order in the source file
//
// It changes when:
//   - any field is renamed (field_name changes)
//   - any field type changes (type_name changes)
//   - any field's tag is renumbered
//   - a field is added or removed
//   - the schema version is bumped
// -----------------------------------------------------------------------
[[nodiscard]] Result<Blake3Digest> compute_abi_hash(const Schema& schema) noexcept;

// -----------------------------------------------------------------------
// format_as_uint64_hex
//
// Interprets the first 8 bytes of `digest` as a big-endian uint64_t and
// returns the 16-character lowercase hex representation, suitable for
// embedding as a C++ uint64_t literal:
//   inline constexpr uint64_t kAbiHash_Foo = 0x<result>;
// -----------------------------------------------------------------------
[[nodiscard]] eastl::string format_as_uint64_hex(const Blake3Digest& digest) noexcept;

}  // namespace glibre::tools::foryc
