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
#include <string>
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
// By reassembling from tokens, the output is independent of:
//   - indentation style
//   - blank lines
//   - spacing around punctuation (`Foo{` vs `Foo {`)
//   - comments
//
// The output changes when:
//   - any identifier, keyword, or literal is added, removed, or renamed
//   - any punctuation character changes
// -----------------------------------------------------------------------
[[nodiscard]] std::string canonicalize_source(std::string_view src) {
    std::string out;
    out.reserve(src.size());

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
// This is a plain inline helper; it exists to avoid repeating the
// hasher init/update/finalize pattern.
// -----------------------------------------------------------------------
[[nodiscard]] Blake3Digest blake3_of(const void* data, std::size_t len) noexcept {
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, len);
    Blake3Digest digest{};
    blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    return digest;
}

// Overload for std::string.
[[nodiscard]] Blake3Digest blake3_of(const std::string& s) noexcept {
    return blake3_of(s.data(), s.size());
}

}  // anonymous namespace

// -----------------------------------------------------------------------
// compute_source_hash
// -----------------------------------------------------------------------
Result<Blake3Digest> compute_source_hash(std::string_view raw_source) noexcept {
    const std::string canonical = canonicalize_source(raw_source);
    return blake3_of(canonical);
}

// -----------------------------------------------------------------------
// compute_abi_hash
// -----------------------------------------------------------------------
Result<Blake3Digest> compute_abi_hash(const Schema& schema) noexcept {
    // Sort TypeDecls by fqn (Unicode code-point order = byte order for
    // the UTF-8 subset used in .fory FQNs).
    eastl::vector<const TypeDecl*> sorted_types;
    sorted_types.reserve(schema.types.size());
    for (const auto& td : schema.types)
        sorted_types.push_back(&td);

    eastl::sort(sorted_types.begin(), sorted_types.end(), [](const TypeDecl* a, const TypeDecl* b) {
        return a->fqn < b->fqn;
    });

    // Build the canonical representation fed into blake3.
    // Format:
    //   "<fqn>:<version_decimal>\n"
    //   "  <tag>:<type_name>:<field_name>\n"  (for each field, tag-sorted)
    // No trailing newline after the last entry.
    std::string canonical;
    canonical.reserve(256);

    bool first_type = true;
    for (const TypeDecl* td_ptr : sorted_types) {
        const TypeDecl& td = *td_ptr;

        if (!first_type)
            canonical += '\n';
        first_type = false;

        // Type header line.
        canonical += std::string(td.fqn.data(), td.fqn.size());
        canonical += ':';
        canonical += std::to_string(td.version);
        canonical += '\n';

        // Sort fields by tag ascending.
        eastl::vector<const FieldDecl*> sorted_fields;
        sorted_fields.reserve(td.fields.size());
        for (const auto& fd : td.fields)
            sorted_fields.push_back(&fd);

        eastl::sort(
            sorted_fields.begin(), sorted_fields.end(),
            [](const FieldDecl* a, const FieldDecl* b) { return a->tag < b->tag; }
        );

        for (const FieldDecl* fd_ptr : sorted_fields) {
            const FieldDecl& fd = *fd_ptr;
            canonical += "  ";
            canonical += std::to_string(fd.tag);
            canonical += ':';
            canonical += std::string(fd.type_name.data(), fd.type_name.size());
            canonical += ':';
            canonical += std::string(fd.name.data(), fd.name.size());
            canonical += '\n';
        }
    }

    // Remove the trailing newline if present (after the last field line).
    // Per spec: "No trailing newline" refers to between top-level types;
    // however this impl ends with '\n' after the last field — keep it for
    // simplicity (the hash is deterministic as long as the convention is
    // consistent).  Remove trailing newline for strict spec compliance.
    if (!canonical.empty() && canonical.back() == '\n')
        canonical.pop_back();

    return blake3_of(canonical);
}

// -----------------------------------------------------------------------
// format_as_uint64_hex
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
