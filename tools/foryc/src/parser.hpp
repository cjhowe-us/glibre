// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/parser.hpp
//
// Minimal .fory schema parser for glibre-foryc (plans #219, #221).
//
// Grammar subset derived from reviews/decisions/fory-codegen.md
// §"Schema File Format". The full grammar is:
//
//   schema <fqn> {
//     version  <integer>
//     since    "<semver>"
//     field <name> : <type>   tag <integer>  since <integer>  [default {...}]
//     migration from <N> to <M> calls "<provider-symbol>"
//     [field | migration ...]
//   }
//   migration v<N>_to_v<N+1> {
//     provider "<fqn-function>"
//   }
//
// This plan implements:
//   - schema block header (fqn + version + optional since)
//   - field declarations (name, type, tag, since; default skipped as opaque)
//   - migration declarations inside schema blocks (plan #221):
//       migration from <N> to <M> calls "<provider-symbol>"
//     Stored as MigrationDecl in TypeDecl::migrations.
//   - top-level migration blocks (parsed and skipped — pre-existing behaviour)
//   - line comments starting with "//"
//
// Migration syntax choice (plan #221):
//   The dispatch prompt specifies: derive a minimal subset if the full
//   grammar is not pinned. Chosen form places migration declarations
//   inside the schema block they belong to, using quoted string literals
//   for the provider symbol (which may contain "::" — not a valid bareword
//   in the lexer's Ident token). This is self-contained and requires no
//   lookup of adjacent schema blocks.
//
//   Example:
//     schema glibre.core.Transform {
//       version 3
//       field translation : vec3f tag 1
//       migration from 1 to 2 calls "glibre::core::migrate_Transform_v1_to_v2"
//       migration from 2 to 3 calls "glibre::core::migrate_Transform_v2_to_v3"
//     }
//
// Not implemented (future plans #222–#225):
//   - ABI hash emission
//
// PHILOSOPHY §11: EASTL replaces std containers. This file uses
//   eastl::string, eastl::vector (IR storage).
//   std:: is retained where EASTL has no equivalent:
//     std::expected (no eastl::expected),
//     std::filesystem (path type for parse_file API),
//     std::string_view (zero-copy interop at API boundaries),
//     std::string / std::ifstream / std::ostringstream (file I/O in
//       parse_file — EASTL has no file-stream equivalent),
//     std::isalpha / std::isdigit / std::from_chars (character classification
//       and parsing, no EASTL equivalent).
//   Container/string types in the IR are eastl::.
//
// Builtin type membership is derived from builtin_map.hpp — the single
// source of truth shared with emit_header.cpp.

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include <EASTL/string.h>
#include <EASTL/vector.h>

#include "glibre/error.hpp"

#include "builtin_map.hpp"

namespace glibre::tools::foryc {

// -----------------------------------------------------------------------
// Schema IR
// -----------------------------------------------------------------------

struct FieldDecl {
    eastl::string name;
    eastl::string type_name;  // raw type string (may include generics)
    std::uint32_t tag{0};
    std::uint32_t since{0};
};

// MigrationDecl — one migration step declared inside a schema block.
//
// Syntax: migration from <from_version> to <to_version> calls "<provider>"
//
// `provider` is the fully-qualified C++ free-function name (e.g.
// "glibre::core::migrate_Transform_v2_to_v3") that the owning context
// must supply.  The codegen emits a dispatcher table that maps
// (from_version, to_version) → this function pointer.
//
// Plan #221 (emit migration dispatcher).
struct MigrationDecl {
    std::uint32_t from_version{0};
    std::uint32_t to_version{0};
    eastl::string provider;  // raw provider symbol (quotes stripped)
};

struct TypeDecl {
    eastl::string fqn;  // e.g. "glibre.core.Transform"
    std::uint32_t version{0};
    eastl::string since_version;  // semver string, may be empty
    eastl::vector<FieldDecl> fields;
    eastl::vector<MigrationDecl> migrations;  // populated by plan #221 parser
};

struct Schema {
    eastl::vector<TypeDecl> types;  // one entry per `schema` block
    eastl::string source_path;      // absolute path to the .fory file
};

// -----------------------------------------------------------------------
// Parse result
// -----------------------------------------------------------------------

using ParseResult = glibre::Result<Schema>;

// Parse a single .fory source file.
// Returns the Schema IR on success, or a glibre::Error wrapping a
// tools::Error enumerator on failure.
//
// Error codes:
//   tools::Error::ForycSyntaxError       — unexpected token or malformed input
//   tools::Error::ForycDuplicateTag      — two fields share a tag number
//   tools::Error::ForycNonMonotoneVersion— version is not > 0 or not unique
//   tools::Error::ForycUnknownType       — field type not in builtins set
//   tools::Error::ForycIOError           — file read failure
//   tools::Error::ForycInvalidIdentifier — FQN segment contains "__" (reserved
//     for codegen mangling; plan #1010, fory-codegen.md §"ABI Stability Rules"
//     point 4)
[[nodiscard]] ParseResult parse_file(const std::filesystem::path& path) noexcept;

// Parse .fory source from an in-memory string (used by unit tests).
[[nodiscard]] ParseResult
parse_string(std::string_view source, std::string_view virtual_path = "<string>") noexcept;

}  // namespace glibre::tools::foryc
