#pragma once
// plugins/shader/src/source/include_resolver.hpp
//
// Internal header — project-rooted include-path resolver.
//
// SRP: "Map a raw include path string to a canonical absolute path,
//       enforcing project-root containment (§4.1 invariant 3)."
//
// Authority: specs/shader/SPEC.md §4.1.
// This is a thin façade over the validation logic in preprocessor.cpp;
// the entry-point scanner and any future tooling can call it without
// pulling in the full preprocessor.

#include <expected>
#include <filesystem>

#include <EASTL/string_view.h>
#include <glibre/shader/shader.hpp>

namespace glibre::shader::detail {

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
