// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/abi_hash.cpp
//
// Schema source-hash and ABI hash implementation (plan #222).
// See abi_hash.hpp for the full design contract.

#include "abi_hash.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <string_view>

// BLAKE3 C API (vcpkg "blake3" port, target blake3::blake3).
#include <blake3.h>

#include <EASTL/algorithm.h>
#include <EASTL/sort.h>
#include <EASTL/string.h>
#include <EASTL/vector.h>

#include "parser.hpp"

namespace glibre::tools::foryc {

namespace {

// -----------------------------------------------------------------------
// canonicalize_source
//
// Produce a canonical token-sequence string from .fory source:
//   1. Strip line-comments ("// ..." through end of line).
//   2. Emit each lexical token separated by exactly one space.
//
// Tokens are:
//   - Identifiers / keywords: [a-zA-Z_][a-zA-Z0-9_]*
//   - Integer literals: [0-9]+
//   - String literals: "..." (content preserved verbatim including interior
//     spaces; the surrounding quotes are included)
//   - Single-character punctuation: { } : < > , .
//
// This function is used only by compute_canonical_source_hash.  The
// wire-format compute_source_hash hashes raw bytes directly.
// -----------------------------------------------------------------------
[[nodiscard]] eastl::string canonicalize_source(std::string_view src) {
    eastl::string out;
    out.reserve(static_cast<eastl::string::size_type>(src.size()));

    std::size_t i = 0;
    bool need_space = false;

    auto emit_token = [&](std::string_view tok) {
        if (need_space)
            out += ' ';
        out.append(tok.data(), tok.size());
        need_space = true;
    };

    while (i < src.size()) {
        const char c = src[i];

        // Strip line comments.
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n')
                ++i;
            continue;
        }

        // Skip whitespace.
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }

        // String literal: capture everything between the outer quotes.
        if (c == '"') {
            const std::size_t start = i++;
            while (i < src.size() && src[i] != '"') {
                // Allow newlines inside strings (error in real source, but be
                // permissive for canonicalization).
                ++i;
            }
            if (i < src.size())
                ++i;  // consume closing '"'
            emit_token(src.substr(start, i - start));
            continue;
        }

        // Integer literal.
        if (std::isdigit(static_cast<unsigned char>(c))) {
            const std::size_t start = i;
            while (i < src.size() && std::isdigit(static_cast<unsigned char>(src[i])))
                ++i;
            emit_token(src.substr(start, i - start));
            continue;
        }

        // Identifier / keyword.
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            const std::size_t start = i;
            while (i < src.size()) {
                const char ch = src[i];
                if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_')
                    break;
                ++i;
            }
            emit_token(src.substr(start, i - start));
            continue;
        }

        // Single-character punctuation: { } : < > , .
        {
            const char punct[2] = {c, '\0'};
            emit_token(std::string_view{punct, 1});
            ++i;
        }
    }

    return out;
}

// -----------------------------------------------------------------------
// blake3_of
//
// Hash `data` with blake3 and return the 32-byte digest.
// -----------------------------------------------------------------------
[[nodiscard]] Blake3Digest blake3_of(const void* data, std::size_t len) noexcept {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, len);
    Blake3Digest digest{};
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    return digest;
}

// Overload for eastl::string.
[[nodiscard]] Blake3Digest blake3_of(const eastl::string& s) noexcept {
    return blake3_of(s.data(), s.size());
}

// Encode `val` as 4 little-endian bytes into `out[0..3]`.
// Used for the version field in the collection ABI hash recipe.
inline void encode_le32(uint32_t val, uint8_t out[4]) noexcept {
    out[0] = static_cast<uint8_t>(val & 0xFFu);
    out[1] = static_cast<uint8_t>((val >> 8u) & 0xFFu);
    out[2] = static_cast<uint8_t>((val >> 16u) & 0xFFu);
    out[3] = static_cast<uint8_t>((val >> 24u) & 0xFFu);
}

}  // anonymous namespace

// -----------------------------------------------------------------------
// compute_source_hash
//
// Hash the RAW bytes of `raw_source` directly — no canonicalization.
// This is the `schema_source_blake3` term in the ABI hash recipe
// (plugin-abi.md §"ABI Hash Function" point 1).
// -----------------------------------------------------------------------
Result<Blake3Digest> compute_source_hash(std::string_view raw_source) noexcept {
    return blake3_of(raw_source.data(), raw_source.size());
}

// -----------------------------------------------------------------------
// compute_canonical_source_hash
//
// Canonical (whitespace-invariant) source hash — diagnostic opt-in only.
// NOT used for the wire-format ABI hash.
// -----------------------------------------------------------------------
Result<Blake3Digest> compute_canonical_source_hash(std::string_view raw_source) noexcept {
    const eastl::string canonical = canonicalize_source(raw_source);
    return blake3_of(canonical);
}

// -----------------------------------------------------------------------
// compute_collection_abi_hash
//
// Recipe (plugin-abi.md §"ABI Hash Function" point 1):
//   For each TypeDecl, sorted by fqn in Unicode code-point order:
//     feed: fqn_bytes || ":" || version_le_bytes(4) || ":" || source_digest_bytes(32)
//   Between entries feed "\n"; no trailing newline.
//   Finalize → 32-byte digest.
// -----------------------------------------------------------------------
Result<Blake3Digest>
compute_collection_abi_hash(
    const eastl::vector<Schema>& schemas,
    const eastl::vector<Blake3Digest>& source_digests
) noexcept {
    // Flatten all TypeDecls, each paired with the source digest of its
    // containing schema.
    struct TypeEntry {
        const TypeDecl* decl;
        const Blake3Digest* src_digest;
    };
    eastl::vector<TypeEntry> entries;
    for (eastl::vector<Schema>::size_type si = 0; si < schemas.size(); ++si) {
        const Blake3Digest& dig = source_digests[si];
        for (const TypeDecl& td : schemas[si].types)
            entries.push_back({&td, &dig});
    }

    // Sort by fqn in Unicode code-point (byte) order.
    eastl::sort(entries.begin(), entries.end(), [](const TypeEntry& a, const TypeEntry& b) {
        return a.decl->fqn < b.decl->fqn;
    });

    // Stream entries into a single blake3 hasher.
    // Format per entry: fqn || ":" || version_le(4) || ":" || src_digest(32)
    // Separator between entries: "\n"
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);

    static constexpr uint8_t kColon = static_cast<uint8_t>(':');
    static constexpr uint8_t kNewline = static_cast<uint8_t>('\n');

    bool first_entry = true;
    for (const TypeEntry& e : entries) {
        if (!first_entry)
            blake3_hasher_update(&hasher, &kNewline, 1);
        first_entry = false;

        // fqn bytes
        blake3_hasher_update(&hasher, e.decl->fqn.data(), e.decl->fqn.size());

        // ":"
        blake3_hasher_update(&hasher, &kColon, 1);

        // version as 4 little-endian bytes
        uint8_t ver_le[4];
        encode_le32(e.decl->version, ver_le);
        blake3_hasher_update(&hasher, ver_le, 4);

        // ":"
        blake3_hasher_update(&hasher, &kColon, 1);

        // 32 raw bytes of the source digest
        blake3_hasher_update(&hasher, e.src_digest->data(), e.src_digest->size());
    }

    Blake3Digest digest{};
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    return digest;
}

// -----------------------------------------------------------------------
// format_as_full_hex
//
// 64-character lowercase hex of the full 32-byte digest.
// This is the wire format (plugin-abi.md §"ABI Hash Function" point 2).
// -----------------------------------------------------------------------
eastl::string format_as_full_hex(const Blake3Digest& digest) noexcept {
    static constexpr char kHexChars[] = "0123456789abcdef";
    eastl::string out;
    out.resize(64);
    for (std::size_t i = 0; i < 32; ++i) {
        out[2 * i]     = kHexChars[(digest[i] >> 4) & 0x0Fu];
        out[2 * i + 1] = kHexChars[digest[i] & 0x0Fu];
    }
    return out;
}

// -----------------------------------------------------------------------
// format_as_uint64_hex
//
// For per-type uint64_t embedding in generated headers (plan #220) ONLY.
// NOT the wire-format ABI hash.
//
// First 8 bytes of digest interpreted as big-endian uint64_t.
// Returns 16-char lowercase hex.
// Endianness: big-endian so the result is a prefix of format_as_full_hex.
// -----------------------------------------------------------------------
eastl::string format_as_uint64_hex(const Blake3Digest& digest) noexcept {
    // Interpret first 8 bytes as big-endian uint64_t.
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i)
        val = (val << 8) | static_cast<uint64_t>(digest[static_cast<std::size_t>(i)]);

    // Format as 16-char lowercase hex with leading zeros.
    const std::string formatted = std::format("{:016x}", val);
    return eastl::string(formatted.data(), formatted.size());
}

}  // namespace glibre::tools::foryc
