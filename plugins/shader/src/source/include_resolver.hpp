#pragma once
// plugins/shader/src/source/include_resolver.hpp
//
// Internal header — project-rooted include-path resolver.
//
// SRP: "Map a raw include path string to a canonical absolute path,
//       enforcing project-root containment (§4.1 invariant 3)."
//
// Authority: specs/shader/SPEC.md §4.1.
//
// Sole implementation of the §4.1 invariant 3 (project-root containment) rule.
// The shader preprocessor (preprocessor.cpp) and ShaderSource::open
// (shader_source.cpp) both call into this header; do not duplicate the rule
// elsewhere.

#include <expected>
#include <filesystem>

#include <EASTL/string_view.h>
#include <glibre/shader/shader.hpp>

namespace glibre::shader::detail {

/// Check whether `abs_path` is contained within `project_root`.
///
/// Returns true if `abs_path` lexically normalises to a sub-path of
/// `project_root` (no ../ escapes), false otherwise.
///
/// This helper encapsulates the §4.1 invariant 3 containment predicate so
/// that both resolve_include and ShaderSource::open call the same rule
/// without duplicating the predicate inline (R2 HIGH-2).
[[nodiscard]] bool validate_in_project_root(
    const std::filesystem::path& abs_path, const std::filesystem::path& project_root
) noexcept;

/// Resolve `include_path` relative to `current_dir`, checking:
///   1. `include_path` must not be absolute → Error::IncludeEscape.
///   2. The lexically-normalised result must remain within `project_root`
///      (no ../ escapes) → Error::IncludeEscape.
///
/// On success returns the canonical absolute path.
[[nodiscard]] std::expected<std::filesystem::path, Error> resolve_include(
    const std::filesystem::path& include_path,
    const std::filesystem::path& current_dir,
    const std::filesystem::path& project_root
);

}  // namespace glibre::shader::detail
