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
#include <string>
#include <string_view>
#include <vector>

#include <glibre/shader/shader.hpp>

namespace glibre::shader::detail {

/// Entry on the cycle-detection visit stack.
/// Both `path` and `path_lower` are allocated under `PreprocessContext::mr`.
/// Storing the lowercased form alongside the original avoids re-allocating a
/// fresh lowercase copy for every element of the stack on every include
/// directive encountered (LOW-1 R2: cache once at push time).
struct VisitEntry {
    std::pmr::string path;        // project-relative path, original case
    std::pmr::string path_lower;  // ASCII-lowercased for case-insensitive comparison
};

/// State passed through the recursive include resolution.
///
/// Lifetime contract: PreprocessContext is a call-scoped aggregate.
/// It MUST NOT outlive the path, vector, and memory_resource arguments passed
/// at construction.
///   - project_root  : borrowed; caller owns the path object for the full
///                     duration of expand_includes (and any recursive calls).
///                     Do NOT store a PreprocessContext in a member field or
///                     return it from a function — that would dangle this ref.
///   - include_closure: borrowed reference into the caller's accumulator.
///   - mr            : memory resource used for visit_stack strings and all
///                     PMR containers inside expand_includes / read_file.
///                     Must be the same resource passed to ShaderSource::open()
///                     so that every allocation is tagged under ContextTag::shader
///                     (perf-budget.md §Allocator Rules #1).

struct PreprocessContext {
    // caller owns; do not extend lifetime beyond the enclosing expand_includes call.
    const std::filesystem::path& project_root;
    std::pmr::vector<IncludeNode>& include_closure;  // accumulates as we expand
    std::pmr::memory_resource* mr;                   // allocation resource (never null)
    std::pmr::vector<VisitEntry> visit_stack;        // for cycle detection
};

/// ASCII-lowercase `sv` into a new `std::pmr::string` allocated under `mr`.
/// Used to compare include paths in a case-insensitive manner (macOS HFS+/APFS).
[[nodiscard]] std::pmr::string ascii_lower(std::string_view sv, std::pmr::memory_resource* mr);

/// Expand the contents of `source_bytes` by resolving all #include "..."
/// directives recursively into `ctx`.
///
/// On success, returns the fully expanded text (UTF-8 bytes as std::pmr::string).
/// On error, returns Error::IncludeEscape or Error::IncludeCycle.
[[nodiscard]] std::expected<std::pmr::string, Error> expand_includes(
    const std::pmr::string& source_bytes,
    const std::filesystem::path& current_file,
    PreprocessContext& ctx
);

/// Read a file from disk, strip UTF-8 BOM, validate UTF-8 encoding, reject
/// empty files.  Returns Error::SourceNotFound if the path does not exist
/// or cannot be read.  Returns Error::EncodingInvalid for non-UTF-8 or empty
/// content.
///
/// mr — allocates the returned string under this resource so the allocation
///      is counted under ContextTag::shader (perf-budget.md §Allocator Rules #1).
[[nodiscard]] std::expected<std::pmr::string, Error>
read_and_normalize_file(const std::filesystem::path& path, std::pmr::memory_resource* mr);

}  // namespace glibre::shader::detail
