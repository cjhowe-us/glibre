#pragma once
// plugins/shader/src/source/entry_point_scanner.hpp
//
// Internal header — scans Slang source text for entry-point attributes.
//
// SRP: "Extract (name, stage) pairs for every [shader("...")] function
//       attribute in a source string; reject dual-annotated functions."
//
// Authority: specs/shader/SPEC.md §4.1 invariant 1:
//   "Every emitted EntryPoint has exactly one stage attribute."

#include <expected>
#include <string>
#include <vector>

#include <glibre/shader/shader.hpp>

namespace glibre::shader::detail {

/// Scan `source` for [shader("...")] attributes and the function names that
/// immediately follow them.
///
/// Returns a vector of EntryPoint records on success.
///
/// Returns Error::EntryPointStageAmbiguous if any function name appears with
/// more than one stage attribute.
///
/// Stage strings recognised (case-sensitive, matching Slang conventions):
///   "vertex", "pixel", "compute", "mesh", "amplification", "library"
///
/// mr — memory resource used for the returned vector and all intermediate
///      strings inside the scan.  Must be the ContextTag::shader resource
///      so that all allocations are accounted under the shader ceiling
///      (perf-budget.md §Allocator Rules #1).
[[nodiscard]] std::expected<std::pmr::vector<EntryPoint>, Error>
scan_entry_points(const std::pmr::string& source, std::pmr::memory_resource* mr);

}  // namespace glibre::shader::detail
