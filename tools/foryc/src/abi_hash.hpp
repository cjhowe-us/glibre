// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/abi_hash.hpp
//
// Schema source-hash and collection ABI hash computation for glibre-foryc
// (plan #222).
//
// Two independent hashes are produced:
//
// 1. Per-schema source-hash (compute_source_hash)
//    blake3 over the RAW bytes of the .fory source file — no
//    canonicalization.  This is the `schema_source_blake3` term referenced
//    by plugin-abi.md §"ABI Hash Function" point 1 and issue #222 §Scope
//    point 1.  Two source files that are bit-for-bit identical produce the
//    same hash; any byte difference (whitespace, comments, field rename)
//    changes it.  Use compute_canonical_source_hash (below) if you want
//    a whitespace-invariant hash for diagnostics.
//
// 2. Collection ABI hash (compute_collection_abi_hash)
//    Locked recipe from plugin-abi.md §"ABI Hash Function" point 1:
//      For each TypeDecl, sorted by fqn in Unicode code-point order:
//        feed: fqn || ":" || version_le_bytes(4) || ":" || source_blake3_bytes(32)
//      Entries are separated by "\n" (single byte); no trailing newline.
//      Output: blake3 of the full byte stream → 32-byte digest.
//    Where:
//      - version_le_bytes(4): the TypeDecl's version field as a u32
//        encoded in 4 little-endian bytes (host-independent canonical form).
//      - source_blake3_bytes(32): the 32 raw bytes of the schema's
//        compute_source_hash digest (NOT the hex string).
//    This is the value the plugin loader compares at load time (#230).
//
// Helpers:
//   format_as_full_hex      — returns the canonical 64-char lowercase hex
//                             string of a full 32-byte blake3 digest.
//                             This is the WIRE FORMAT required by
//                             plugin-abi.md §"ABI Hash Function" point 2
//                             and issue #222 §Scope point 3.
//
//   format_as_uint64_hex    — interprets the FIRST 8 bytes of the digest
//                             as a BIG-ENDIAN uint64_t and returns a
//                             16-char lowercase hex string.  For per-type
//                             embedding ONLY (kAbiHash_Foo constants in
//                             generated headers, plan #220).  NOT the wire
//                             form; do not use for glibre_types_abi_hash.
//                             Endianness note: big-endian is chosen so that
//                             `std::format("{:016x}", val)` rendering of the
//                             digest prefix matches the first 16 chars of
//                             format_as_full_hex, simplifying visual
//                             cross-checking.
//
// PHILOSOPHY §11: EASTL replaces std containers.
//   eastl::string, eastl::array for IR/return types.
//   std::expected (no eastl equivalent) for the Result alias.
//   std::string_view at public API boundaries (zero-copy interop).

#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

#include <EASTL/array.h>
#include <EASTL/string.h>
#include <EASTL/vector.h>

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
// Hash the RAW bytes of `raw_source` with blake3 and return the 32-byte
// digest.  No canonicalization is applied.  This is the
// `schema_source_blake3` term in the ABI hash recipe (plugin-abi.md
// §"ABI Hash Function" point 1, issue #222 §Scope point 1).
//
// Return value: always succeeds (blake3 never fails on in-memory data).
// The Result wrapper exists to be composable with the parse pipeline.
// -----------------------------------------------------------------------
[[nodiscard]] Result<Blake3Digest>
compute_source_hash(std::string_view raw_source) noexcept;

// -----------------------------------------------------------------------
// compute_canonical_source_hash  [diagnostic / opt-in]
//
// Canonicalize `raw_source` (strip line-comments and collapse whitespace
// to single spaces), then hash the result with blake3.  Two .fory files
// that differ only in whitespace/comments produce the same canonical hash.
//
// NOT used for the wire-format ABI hash or the plugin manifest
// `schema_hash` field.  Kept as an opt-in diagnostic helper.
// -----------------------------------------------------------------------
[[nodiscard]] Result<Blake3Digest>
compute_canonical_source_hash(std::string_view raw_source) noexcept;

// -----------------------------------------------------------------------
// SchemaWithDigest
//
// Pairs a parsed Schema with its raw-source blake3 digest so callers
// cannot accidentally pass mismatched parallel vectors.  The pairing
// is structural — impossible to mis-order or mis-size.
// -----------------------------------------------------------------------
struct SchemaWithDigest {
    Schema       schema;
    Blake3Digest source_digest;
};

// -----------------------------------------------------------------------
// compute_collection_abi_hash
//
// Compute the engine-wide ABI hash over a collection of (Schema, source
// digest) pairs per the locked recipe in plugin-abi.md §"ABI Hash
// Function" point 1.
//
// Recipe:
//   1. Collect all TypeDecls from all entries.
//   2. Sort TypeDecls by fqn in Unicode code-point (byte) order.
//   3. For each TypeDecl in that order, feed into blake3:
//        fqn_bytes || ":" || version_le_bytes(4) || ":" || source_digest_bytes(32)
//      where version_le_bytes(4) is the u32 version in little-endian byte
//      order and source_digest_bytes(32) is the 32 raw bytes of the
//      compute_source_hash digest for the schema that contains this TypeDecl.
//   4. Between entries feed "\n" (single byte); no trailing newline.
//   5. Finalize the hasher → 32-byte digest.
//
// Return value: always succeeds for non-empty input.  Empty collection
// returns the blake3 of an empty input.
// -----------------------------------------------------------------------
[[nodiscard]] Result<Blake3Digest>
compute_collection_abi_hash(
    const eastl::vector<SchemaWithDigest>& entries
) noexcept;

// -----------------------------------------------------------------------
// format_as_full_hex
//
// Returns the full 32-byte digest as a 64-character lowercase hex string.
// This is the wire format required by:
//   - plugin-abi.md §"ABI Hash Function" point 2 (64-char lowercase hex)
//   - issue #222 §Scope point 3
//   - the manifest `abi_hash : string` field in PluginManifest
//   - glibre_types_abi_hash() return value
// -----------------------------------------------------------------------
[[nodiscard]] eastl::string format_as_full_hex(const Blake3Digest& digest) noexcept;

// -----------------------------------------------------------------------
// format_as_uint64_hex
//
// For per-type uint64_t embedding in generated headers (plan #220) ONLY.
// NOT the wire-format ABI hash.
//
// Interprets the first 8 bytes of `digest` as a big-endian uint64_t and
// returns the 16-character lowercase hex representation:
//   inline constexpr uint64_t kAbiHash_Foo = 0x<result>;
//
// Endianness: big-endian so the result is a prefix of format_as_full_hex
// (first 16 chars match), enabling visual cross-checking.
// -----------------------------------------------------------------------
[[nodiscard]] eastl::string format_as_uint64_hex(const Blake3Digest& digest) noexcept;

}  // namespace glibre::tools::foryc
