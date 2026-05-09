#pragma once
// plugins/shader/src/source/preprocessor.hpp
//
// Internal header — include + source-text expander for Slang source files.
//
// SRP: "Expand #include directives and produce a concatenated source blob."
// This module does NOT compile or parse Slang; it performs only textual
// include expansion sufficient to detect entry points and compute a
// content hash for the preprocessed output.
//
// Authority: specs/shader/SPEC.md §4.1.
//
// Known limitation (MED-3):
//   The include scanner is line-based and does NOT track comment state.
//   A `#include "x"` directive that appears inside a `/* ... */` block
//   comment spanning multiple lines WILL be treated as a real include and
//   may produce Error::IncludeEscape or Error::IncludeCycle.  This is
//   intentionally conservative: slangc owns the authoritative parse;
//   we only need to validate + present the include graph.

#include <expected>
#include <filesystem>

#include <EASTL/string.h>
#include <EASTL/vector.h>
#include <glibre/shader/shader.hpp>

namespace glibre::shader::detail {

/// State passed through the recursive include resolution.
struct PreprocessContext {
    const std::filesystem::path& project_root;
    eastl::vector<IncludeNode>& include_closure;  // accumulates as we expand
    eastl::vector<eastl::string> visit_stack;     // for cycle detection
};

/// Expand the contents of `source_bytes` by resolving all #include "..."
/// directives recursively into `ctx`.
///
/// On success, returns the fully expanded text (UTF-8 bytes as eastl::string).
/// On error, returns Error::IncludeEscape or Error::IncludeCycle.
[[nodiscard]] std::expected<eastl::string, Error> expand_includes(
    const eastl::string& source_bytes,
    const std::filesystem::path& current_file,
    PreprocessContext& ctx
);

/// Read a file from disk, strip UTF-8 BOM, validate UTF-8 encoding, reject
/// empty files.  Returns Error::SourceNotFound if the path does not exist
/// or cannot be read.  Returns Error::EncodingInvalid for non-UTF-8 or empty
/// content.
[[nodiscard]] std::expected<eastl::string, Error>
read_and_normalize_file(const std::filesystem::path& path);

}  // namespace glibre::shader::detail
