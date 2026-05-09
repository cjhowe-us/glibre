// plugins/shader/src/source/include_resolver.cpp
//
// Project-rooted include-path resolver.
//
// Authority: specs/shader/SPEC.md §4.1 invariant 3:
//   "The include-graph closure is acyclic and resolves entirely under the
//    project source root; absolute or upward-escaping includes are rejected at
//    construction time with shader::Error::IncludeEscape."

#include "include_resolver.hpp"

namespace glibre::shader::detail {

std::expected<std::filesystem::path, Error>
resolve_include(const std::filesystem::path& include_path,
                const std::filesystem::path& current_dir,
                const std::filesystem::path& project_root) {
    // Rule 1: absolute paths are forbidden (IncludeEscape).
    if (include_path.is_absolute()) {
        return std::unexpected(Error::IncludeEscape);
    }

    // Lexically resolve relative to the current file's directory.
    std::filesystem::path resolved =
        (current_dir / include_path).lexically_normal();

    // Rule 2: resolved path must remain within project_root.
    // lexically_relative returns an empty path or one starting with ".."
    // when resolved lies outside project_root.
    auto rel = resolved.lexically_relative(project_root);
    if (rel.empty() || rel.native().starts_with("..")) {
        return std::unexpected(Error::IncludeEscape);
    }

    return resolved;
}

}  // namespace glibre::shader::detail
