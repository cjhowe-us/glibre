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
//   1. Walk source bytes line-by-line looking for the literal pattern
//      `#include "..."` (no regex — PHILOSOPHY §11 / HIGH-2).
//   2. For each found include path, delegate validation and resolution to
//      detail::resolve_include (include_resolver.hpp — HIGH-3 fix).
//   3. Detect cycles by checking the visit_stack in PreprocessContext.
//      Comparison is ASCII-lowercased to handle macOS case-insensitive
//      filesystems (MED-2 fix).
//   4. Recursively expand.  Accumulate the IncludeNode in include_closure.
//   5. Return concatenated expanded source.
//
// Limitation (MED-3, documented):
//   The include scanner works line-by-line on raw bytes.  It does NOT track
//   block-comment or line-comment state.  A `#include "x"` token that appears
//   inside a `/* ... */` block comment whose opener is on a previous line WILL
//   be treated as a real include directive.
//   This is intentional for the ingestion-validation purpose of §4.1: we are
//   conservative (may reject valid-but-commented-out includes) rather than
//   silently accepting them.  slangc owns the authoritative parse.

#include "preprocessor.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>

#include "include_resolver.hpp"
// BLAKE3 per-include content hashing — shared helper (R2 HIGH-1).
#include "shader_hash.hpp"

namespace glibre::shader::detail {

namespace {

// ---------------------------------------------------------------------------
// File I/O — raw byte slurp via <cstdio> (no std::ifstream / std::fstream).
// Returns Error::SourceNotFound if the file cannot be opened.
// ---------------------------------------------------------------------------

[[nodiscard]] std::expected<std::pmr::string, Error>
read_file_raw(const std::filesystem::path& path, std::pmr::memory_resource* mr) {
    // Open binary — we handle newline normalisation at the UTF-8 validation
    // layer rather than in the OS read.
    FILE* f = std::fopen(path.c_str(), "rb");  // NOLINT(cppcoreguidelines-owning-memory)
    if (!f) {
        return std::unexpected(Error::SourceNotFound);
    }

    // Stat the size so we can allocate exactly once.
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);  // NOLINT(cppcoreguidelines-owning-memory)
        return std::unexpected(Error::SourceNotFound);
    }
    long file_size_l = std::ftell(f);
    std::rewind(f);

    if (file_size_l < 0) {
        std::fclose(f);  // NOLINT(cppcoreguidelines-owning-memory)
        return std::unexpected(Error::SourceNotFound);
    }

    std::size_t file_size = static_cast<std::size_t>(file_size_l);

    std::pmr::string buf{mr};
    buf.resize(file_size);

    if (file_size > 0 && std::fread(buf.data(), 1, file_size, f) != file_size) {
        std::fclose(f);  // NOLINT(cppcoreguidelines-owning-memory)
        return std::unexpected(Error::SourceNotFound);
    }

    std::fclose(f);  // NOLINT(cppcoreguidelines-owning-memory)
    return buf;
}

// ---------------------------------------------------------------------------
// UTF-8 / BOM / empty normalization (HIGH-4).
//
// Strips a UTF-8 BOM (EF BB BF) if present.
// Returns Error::EncodingInvalid if:
//   - The file is empty (after BOM strip), OR
//   - The bytes are not well-formed UTF-8.
//
// Well-formed UTF-8 validation is a lightweight state-machine scan — no heap,
// no regex, -fno-exceptions compatible.
// ---------------------------------------------------------------------------

[[nodiscard]] bool is_valid_utf8(const char* data, std::size_t len) noexcept {
    std::size_t i = 0;
    while (i < len) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        std::size_t extra = 0;
        if (c < 0x80u) {
            ++i;
            continue;
        } else if ((c & 0xE0u) == 0xC0u) {
            extra = 1;
            // Overlong: C0/C1 are forbidden.
            if (c < 0xC2u)
                return false;
        } else if ((c & 0xF0u) == 0xE0u) {
            extra = 2;
        } else if ((c & 0xF8u) == 0xF0u) {
            extra = 3;
            // Code-points above U+10FFFF are forbidden.
            if (c > 0xF4u)
                return false;
        } else {
            return false;  // Invalid leading byte.
        }
        ++i;
        for (std::size_t j = 0; j < extra; ++j, ++i) {
            if (i >= len)
                return false;
            unsigned char cont = static_cast<unsigned char>(data[i]);
            if ((cont & 0xC0u) != 0x80u)
                return false;
        }
    }
    return true;
}

/// Strip UTF-8 BOM if present, validate remaining bytes, reject empty files.
/// The input `bytes` already carries the mr from read_file_raw; no additional
/// mr parameter needed here.
[[nodiscard]] std::expected<std::pmr::string, Error>
normalize_source_bytes(std::pmr::string bytes) {
    // Strip BOM (EF BB BF).
    constexpr unsigned char kBom[3] = {0xEFu, 0xBBu, 0xBFu};
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == kBom[0] &&
        static_cast<unsigned char>(bytes[1]) == kBom[1] &&
        static_cast<unsigned char>(bytes[2]) == kBom[2]) {
        bytes.erase(0, 3);
    }

    if (bytes.empty()) {
        return std::unexpected(Error::EncodingInvalid);
    }

    if (!is_valid_utf8(bytes.c_str(), bytes.size())) {
        return std::unexpected(Error::EncodingInvalid);
    }

    return bytes;
}

// ---------------------------------------------------------------------------
// Unified file-read entry point — raw read + normalization.
// ---------------------------------------------------------------------------

[[nodiscard]] std::expected<std::pmr::string, Error>
read_file(const std::filesystem::path& path, std::pmr::memory_resource* mr) {
    auto raw = read_file_raw(path, mr);
    if (!raw)
        return raw;
    return normalize_source_bytes(std::move(*raw));
}

// ---------------------------------------------------------------------------
// Case-fold helper for cycle detection (MED-2).
//
// macOS HFS+/APFS is case-preserving but case-insensitive by default.
// `#include "Foo.slang"` and `#include "foo.slang"` resolve to the same
// file; we lowercase path strings before comparison to catch such cases.
// ---------------------------------------------------------------------------

[[nodiscard]] std::pmr::string ascii_lower(std::string_view sv, std::pmr::memory_resource* mr) {
    std::pmr::string out{mr};
    out.resize(sv.size());
    for (std::size_t i = 0; i < sv.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(sv[i])));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Hand-rolled #include scanner (replaces <regex> — HIGH-2 / LOW-2).
//
// Pattern matched per line:
//   ^[ \t]*#[ \t]*include[ \t]+"<path>"
//
// Returns the include path string if matched, or an empty optional.
// ---------------------------------------------------------------------------

[[nodiscard]] std::optional<std::pmr::string>
scan_include_line(const char* line, std::size_t len, std::pmr::memory_resource* mr) noexcept {
    std::size_t pos = 0;

    // Skip leading horizontal whitespace.
    while (pos < len && (line[pos] == ' ' || line[pos] == '\t'))
        ++pos;

    // '#'
    if (pos >= len || line[pos] != '#')
        return std::nullopt;
    ++pos;

    // Optional whitespace after '#'.
    while (pos < len && (line[pos] == ' ' || line[pos] == '\t'))
        ++pos;

    // "include"
    constexpr const char kInclude[] = "include";
    constexpr std::size_t kIncludeLen = 7;
    if (pos + kIncludeLen > len)
        return std::nullopt;
    if (std::memcmp(line + pos, kInclude, kIncludeLen) != 0)
        return std::nullopt;
    pos += kIncludeLen;

    // At least one whitespace after "include".
    if (pos >= len || (line[pos] != ' ' && line[pos] != '\t'))
        return std::nullopt;
    while (pos < len && (line[pos] == ' ' || line[pos] == '\t'))
        ++pos;

    // Opening '"'.
    if (pos >= len || line[pos] != '"')
        return std::nullopt;
    ++pos;

    // Path content until closing '"'.
    std::size_t path_start = pos;
    while (pos < len && line[pos] != '"')
        ++pos;
    if (pos >= len)
        return std::nullopt;  // no closing '"'

    std::size_t path_len = pos - path_start;
    if (path_len == 0)
        return std::nullopt;  // empty include path

    return std::pmr::string{line + path_start, path_len, mr};
}

}  // namespace

std::expected<std::pmr::string, Error> expand_includes(
    const std::pmr::string& source_bytes,
    const std::filesystem::path& current_file,
    PreprocessContext& ctx
) {
    std::filesystem::path current_dir = current_file.parent_path();
    std::pmr::string expanded{ctx.mr};
    expanded.reserve(source_bytes.size());

    // Walk source bytes line-by-line without <sstream> or std::getline.
    const char* src = source_bytes.c_str();
    std::size_t src_len = source_bytes.size();
    std::size_t line_start = 0;

    while (line_start < src_len) {
        // Find end of line.
        std::size_t line_end = line_start;
        while (line_end < src_len && src[line_end] != '\n') {
            ++line_end;
        }

        // line = [line_start, line_end) — does not include '\n'.
        const char* line = src + line_start;
        std::size_t line_len = line_end - line_start;

        // Advance past '\n' for next iteration.
        // Whether or not there is a trailing newline, we always advance
        // at least past line_end so the outer loop terminates.
        bool had_newline = (line_end < src_len && src[line_end] == '\n');
        line_start = line_end + 1u;  // always advances: past '\n' or to src_len+1

        // Attempt to match a #include "..." directive.
        auto include_path_opt = scan_include_line(line, line_len, ctx.mr);
        if (include_path_opt) {
            std::filesystem::path include_rel{include_path_opt->c_str()};

            // Step 1: validate and resolve (HIGH-3: delegate to resolve_include).
            auto abs_result = resolve_include(include_rel, current_dir, ctx.project_root);
            if (!abs_result) {
                return std::unexpected(abs_result.error());
            }
            std::filesystem::path abs_path = std::move(*abs_result);

            // Step 2: compute project-relative path for the include node.
            std::filesystem::path proj_rel =
                abs_path.lexically_relative(ctx.project_root).lexically_normal();
            std::pmr::string proj_rel_str{proj_rel.native().c_str(), ctx.mr};

            // Step 3: cycle detection — case-insensitive comparison (MED-2).
            std::pmr::string proj_rel_lower = ascii_lower(proj_rel_str, ctx.mr);
            for (const auto& visited : ctx.visit_stack) {
                std::pmr::string visited_lower = ascii_lower(visited, ctx.mr);
                if (visited_lower == proj_rel_lower) {
                    return std::unexpected(Error::IncludeCycle);
                }
            }

            // Step 4: read included file.
            auto file_result = read_file(abs_path, ctx.mr);
            if (!file_result) {
                // MED-4: §10 SourceNotFound covers both root-file and include-target
                // failures.  The distinction is carried by context: callers of
                // shader_source.cpp that surface this error to a log should use
                // proj_rel_str from their ErrorContext (attached at the
                // glibre::Error construction site in shader_source.cpp).
                // expand_includes returns Error (glibre::shader::Error enum) which
                // is wrapped into glibre::Error with an ErrorContext by the caller.
                return std::unexpected(file_result.error());
            }
            std::pmr::string included_bytes = std::move(*file_result);

            // Step 5: build include node with content hash (R2 HIGH-1: shared helper).
            ShaderHash content_hash = blake3_hash(included_bytes.data(), included_bytes.size());
            // Construct IncludeNode with the context mr so the string member also
            // allocates under ContextTag::shader (perf-budget.md §Allocator Rules #1).
            // PMR string copy uses the destination's allocator by default; supply mr
            // explicitly so the IncludeNode's path string does not escape to the
            // default resource.
            ctx.include_closure.push_back(
                IncludeNode{std::pmr::string{proj_rel_str.c_str(), ctx.mr}, content_hash}
            );

            // Step 6: push onto visit stack and recurse.
            ctx.visit_stack.push_back(proj_rel_str);
            auto sub_result = expand_includes(included_bytes, abs_path, ctx);
            ctx.visit_stack.pop_back();
            if (!sub_result) {
                return std::unexpected(sub_result.error());
            }

            // Append expanded sub-content.
            expanded += *sub_result;
        } else {
            // Non-include line: pass through verbatim.
            expanded.append(line, line_len);
        }

        // Re-emit the newline that terminated this line (if any).
        if (had_newline) {
            expanded += '\n';
        }
    }

    return expanded;
}

// ---------------------------------------------------------------------------
// read_and_normalize_file: public entry point for shader_source.cpp
// (HIGH-4 normalisation).  Allocates the returned string under `mr`.
// ---------------------------------------------------------------------------

std::expected<std::pmr::string, Error>
read_and_normalize_file(const std::filesystem::path& path, std::pmr::memory_resource* mr) {
    return read_file(path, mr);
}

}  // namespace glibre::shader::detail
