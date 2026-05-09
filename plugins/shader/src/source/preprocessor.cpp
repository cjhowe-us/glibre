// plugins/shader/src/source/preprocessor.cpp
//
// Implementation of textual #include expansion for Slang source files.
//
// Authority: specs/shader/SPEC.md §4.1 invariant 3:
//   "The include-graph closure is acyclic and resolves entirely under the
//    project source root; absolute or upward-escaping includes are rejected at
//    construction time with shader::Error::IncludeEscape."
//
// Algorithm:
//   1. Scan lines for the pattern `#include "..."`.
//   2. For each found include path, validate it is not absolute and does not
//      escape the project root after lexical_normal resolution.
//   3. Detect cycles by checking the visit_stack in PreprocessContext.
//   4. Recursively expand. Accumulate the IncludeNode in include_closure.
//   5. Return concatenated expanded source.

#include "preprocessor.hpp"

#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include <EASTL/algorithm.h>

// BLAKE3 for per-include content hashing (specs/shader/SPEC.md §4.1).
#include <blake3.h>

namespace glibre::shader::detail {

namespace {

/// Compute BLAKE3 over a raw byte buffer, return a ShaderHash.
[[nodiscard]] ShaderHash blake3_hash(const void* data, std::size_t len) noexcept {
    ShaderHash result{};
    blake3_hasher hasher{};
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, len);
    blake3_hasher_finalize(&hasher, reinterpret_cast<uint8_t*>(result.bytes.data()), 32);
    return result;
}

/// Read a file from disk into an eastl::string.
/// Returns Error::SourceNotFound if the path does not exist or cannot be opened.
[[nodiscard]] std::expected<eastl::string, Error> read_file(const std::filesystem::path& path) {
    // Use std::ifstream (EASTL does not own file I/O).
    std::ifstream ifs{path, std::ios::binary};
    if (!ifs.is_open()) {
        return std::unexpected(Error::SourceNotFound);
    }
    std::ostringstream buf;
    buf << ifs.rdbuf();
    std::string std_content = buf.str();
    return eastl::string{std_content.data(), std_content.size()};
}

/// Validate that `include_path` is not absolute and does not escape
/// project_root after resolution from `current_dir`.
///
/// Returns the canonical absolute path on success, or Error::IncludeEscape.
[[nodiscard]] std::expected<std::filesystem::path, Error> validate_include(
    const std::filesystem::path& include_path,
    const std::filesystem::path& current_dir,
    const std::filesystem::path& project_root
) {
    // Reject absolute paths outright (§4.1 invariant 3).
    if (include_path.is_absolute()) {
        return std::unexpected(Error::IncludeEscape);
    }

    // Lexically resolve relative to the current file's directory.
    std::filesystem::path resolved = (current_dir / include_path).lexically_normal();

    // Reject ../ escapes: the resolved path must start with project_root.
    // Use lexically_relative to check containment without hitting disk.
    auto rel = resolved.lexically_relative(project_root);
    // lexically_relative returns an empty path (or one starting with ..) when
    // resolved is not under project_root.
    if (rel.empty() || rel.native().starts_with("..")) {
        return std::unexpected(Error::IncludeEscape);
    }

    return resolved;
}

}  // namespace

std::expected<eastl::string, Error> expand_includes(
    const eastl::string& source_bytes,
    const std::filesystem::path& current_file,
    PreprocessContext& ctx
) {
    // The regex matches: optional leading whitespace, #include, whitespace,
    // then a double-quoted path.  We only support #include "..." (project-relative),
    // never #include <...> (system headers are not relevant to Slang source).
    static const std::regex kIncludePattern{R"re(^[ \t]*#[ \t]*include[ \t]+"([^"]+)")re"};

    std::filesystem::path current_dir = current_file.parent_path();
    eastl::string expanded;
    expanded.reserve(source_bytes.size());

    // Convert eastl::string to a std::string for line iteration.
    std::istringstream iss{std::string{source_bytes.c_str(), source_bytes.size()}};
    std::string line;

    while (std::getline(iss, line)) {
        std::smatch m;
        if (std::regex_search(line, m, kIncludePattern)) {
            std::filesystem::path include_rel{m[1].str()};

            // Step 1: validate (no absolute, no ../ escape).
            auto abs_result = validate_include(include_rel, current_dir, ctx.project_root);
            if (!abs_result) {
                return std::unexpected(abs_result.error());
            }
            std::filesystem::path abs_path = std::move(*abs_result);

            // Step 2: compute project-relative path for the include node.
            std::filesystem::path proj_rel =
                abs_path.lexically_relative(ctx.project_root).lexically_normal();
            eastl::string proj_rel_str{proj_rel.native().c_str()};

            // Step 3: cycle detection — check visit_stack.
            for (const auto& visited : ctx.visit_stack) {
                if (visited == proj_rel_str) {
                    return std::unexpected(Error::IncludeCycle);
                }
            }

            // Step 4: read included file.
            auto file_result = read_file(abs_path);
            if (!file_result) {
                return std::unexpected(file_result.error());
            }
            eastl::string included_bytes = std::move(*file_result);

            // Step 5: build include node with content hash.
            ShaderHash content_hash = blake3_hash(included_bytes.data(), included_bytes.size());
            ctx.include_closure.push_back(IncludeNode{proj_rel_str, content_hash});

            // Step 6: push onto visit stack and recurse.
            ctx.visit_stack.push_back(proj_rel_str);
            auto sub_result = expand_includes(included_bytes, abs_path, ctx);
            ctx.visit_stack.pop_back();
            if (!sub_result) {
                return std::unexpected(sub_result.error());
            }

            // Append expanded sub-content.
            expanded += *sub_result;
            // Re-add newline that getline consumed.
            expanded += '\n';
        } else {
            // Non-include line: pass through verbatim.
            expanded += eastl::string{line.c_str(), line.size()};
            expanded += '\n';
        }
    }

    return expanded;
}

}  // namespace glibre::shader::detail
