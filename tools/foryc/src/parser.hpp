// SPDX-License-Identifier: Apache-2.0
// tools/foryc/src/parser.hpp
//
// Minimal .fory schema parser for glibre-foryc (plan #219).
//
// Grammar subset derived from reviews/decisions/fory-codegen.md
// §"Schema File Format". The full grammar is:
//
//   schema <fqn> {
//     version  <integer>
//     since    "<semver>"
//     field <name> : <type>   tag <integer>  since <integer>  [default {...}]
//     [field ...]
//   }
//   migration v<N>_to_v<N+1> {
//     provider "<fqn-function>"
//   }
//
// This plan implements:
//   - schema block header (fqn + version + optional since)
//   - field declarations (name, type, tag, since; default skipped as opaque)
//   - migration blocks (parsed but not stored — out of scope for this plan)
//   - line comments starting with "//"
//
// Not implemented (future plans #220–#225):
//   - Code generation / header emission
//   - ABI hash emission
//   - Migration dispatcher emit
//
// PHILOSOPHY §11: EASTL replaces std containers. This file uses
//   eastl::string, eastl::vector (IR storage).
//   std:: is retained where EASTL has no equivalent:
//     std::expected (no eastl::expected),
//     std::filesystem (path type for parse_file API),
//     std::string_view (zero-copy interop at API boundaries),
//     std::string / std::ifstream / std::ostringstream (file I/O in
//       parse_file — EASTL has no file-stream equivalent),
//     std::array (k_builtins table — fixed-size compile-time array),
//     std::isalpha / std::isdigit / std::from_chars (character classification
//       and parsing, no EASTL equivalent).
//   Container/string types in the IR are eastl::.

#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

#include <EASTL/string.h>
#include <EASTL/vector.h>

#include "glibre/error.hpp"

namespace glibre::tools::foryc {

// -----------------------------------------------------------------------
// Builtin type set (from fory-codegen.md §"Rules")
// -----------------------------------------------------------------------
// The canonical set of scalar/aggregate type names that the parser
// accepts without a schema-defined declaration. Any field type not in
// this set is reported as ForycUnknownType.
static constexpr std::string_view k_builtins[] = {
    "u8",     "u16",   "u32",   "u64",   "i8",     "i16",    "i32",
    "i64",    "f32",   "f64",   "bool",  "vec2f",  "vec3f",  "vec4f",
    "vec2i",  "vec3i", "vec4i", "quatf", "entity", "string", "bytes",
    "list",    // list<T> — generic name checked as prefix
    "map",     // map<K,V>
    "option",  // option<T>
};

// -----------------------------------------------------------------------
// Schema IR
// -----------------------------------------------------------------------

struct FieldDecl {
    eastl::string name;
    eastl::string type_name;  // raw type string (may include generics)
    std::uint32_t tag{0};
    std::uint32_t since{0};
};

struct TypeDecl {
    eastl::string fqn;  // e.g. "glibre.core.Transform"
    std::uint32_t version{0};
    eastl::string since_version;  // semver string, may be empty
    eastl::vector<FieldDecl> fields;
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
[[nodiscard]] ParseResult parse_file(const std::filesystem::path& path) noexcept;

// Parse .fory source from an in-memory string (used by unit tests).
[[nodiscard]] ParseResult
parse_string(std::string_view source, std::string_view virtual_path = "<string>") noexcept;

}  // namespace glibre::tools::foryc
