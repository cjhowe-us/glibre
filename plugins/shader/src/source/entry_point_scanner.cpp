// plugins/shader/src/source/entry_point_scanner.cpp
//
// Entry-point scanner for Slang source files.
//
// Finds patterns of the form:
//   [shader("vertex")]          (or other stage strings)
//   void vs_main(...)           (the immediately-following function signature)
//
// Authority: specs/shader/SPEC.md §4.1 invariant 1:
//   "Every emitted EntryPoint has exactly one stage attribute."
//
// Scanning strategy:
//   Step 1. Walk source bytes char-by-char looking for '[' characters.
//   Step 2. At each '[', attempt to match [shader("stage")] literally.
//   Step 3. Past the attribute, skip whitespace and additional [...] blocks,
//           then match: <return_type> <ws> <fn_name> <ws>* '('.
//   Step 4. Collect (fn_name, stage_str) pairs in source-encounter order
//           using a std::pmr::vector (preserves deterministic first-occurrence
//           order — reviews/decisions/eastl-removal.md §2 R4; fixes HIGH-1).
//   Step 5. Detect duplicates: if any fn_name appears more than once with a
//           different stage_str → EntryPointStageAmbiguous.
//
// No <regex>, <unordered_map> — reviews/decisions/eastl-removal.md §1 (HIGH-2).
//
// Note: this scanner deliberately avoids full Slang parsing.  Full parsing
// is slangc's job (§4.3).

#include "entry_point_scanner.hpp"

#include <cstring>
#include <optional>

namespace glibre::shader::detail {

namespace {

// ---------------------------------------------------------------------------
// Character-class helpers
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr bool is_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] constexpr bool is_ident_start(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

[[nodiscard]] constexpr bool is_ident_cont(char c) noexcept {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

// ---------------------------------------------------------------------------
// Hand-rolled token-scanning helpers that work on a [begin, end) range.
// All functions advance *pos past the matched region and return true on match.
// ---------------------------------------------------------------------------

/// Skip any whitespace characters.  Always succeeds.
void skip_ws(const char* base, std::size_t len, std::size_t& pos) noexcept {
    while (pos < len && is_ws(base[pos])) {
        ++pos;
    }
}

/// Attempt to match a literal prefix at pos.
/// Returns true and advances pos on success; leaves pos unchanged on failure.
[[nodiscard]] bool
match_literal(const char* base, std::size_t len, std::size_t& pos, const char* literal) noexcept {
    std::size_t llen = std::strlen(literal);
    if (pos + llen > len)
        return false;
    if (std::memcmp(base + pos, literal, llen) != 0)
        return false;
    pos += llen;
    return true;
}

/// Attempt to read an identifier at pos.  On success, *out_begin and *out_len
/// delimit the identifier.
[[nodiscard]] bool scan_ident(
    const char* base,
    std::size_t len,
    std::size_t& pos,
    std::size_t* out_begin,
    std::size_t* out_len
) noexcept {
    if (pos >= len || !is_ident_start(base[pos]))
        return false;
    std::size_t start = pos;
    while (pos < len && is_ident_cont(base[pos])) {
        ++pos;
    }
    *out_begin = start;
    *out_len = pos - start;
    return true;
}

/// Skip a single [...] block (no nesting support).  Returns true and advances
/// pos past the closing ']' on success.
[[nodiscard]] bool skip_attr_block(const char* base, std::size_t len, std::size_t& pos) noexcept {
    if (pos >= len || base[pos] != '[')
        return false;
    ++pos;
    while (pos < len && base[pos] != ']') {
        ++pos;
    }
    if (pos >= len)
        return false;
    ++pos;  // consume ']'
    return true;
}

// ---------------------------------------------------------------------------
// Stage-string → Stage enum mapping
// ---------------------------------------------------------------------------

[[nodiscard]] std::optional<Stage> parse_stage(const char* str, std::size_t len) noexcept {
    auto eq = [&](const char* lit) noexcept {
        std::size_t llen = std::strlen(lit);
        return llen == len && std::memcmp(str, lit, len) == 0;
    };
    if (eq("vertex"))
        return Stage::Vertex;
    if (eq("pixel"))
        return Stage::Pixel;
    if (eq("compute"))
        return Stage::Compute;
    if (eq("mesh"))
        return Stage::Mesh;
    if (eq("amplification"))
        return Stage::Amplification;
    if (eq("library"))
        return Stage::Library;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// RawEntryPoint — collected in source-encounter order
// ---------------------------------------------------------------------------

struct RawEntryPoint {
    std::pmr::string stage_str;
    std::pmr::string fn_name;
};

/// Walk `source` and collect (stage_str, fn_name) pairs in the order they
/// appear in the source (source-position order).
///
/// For each '[' in the source we attempt:
///   [shader("<stage>")]
/// then skip whitespace + additional [...] blocks, then match:
///   <return_type_ident> <ws+> <fn_name_ident> <ws>* '('
///
/// Preserves first-occurrence source order (no hash maps) —
/// reviews/decisions/eastl-removal.md §2 R4.
///
/// mr — allocates result vector and all intermediate strings under this
///      resource (ContextTag::shader, perf-budget.md §Allocator Rules #1).
std::pmr::vector<RawEntryPoint>
extract_raw_entry_points(const char* src, std::size_t len, std::pmr::memory_resource* mr) {
    std::pmr::vector<RawEntryPoint> results{mr};

    std::size_t i = 0;
    while (i < len) {
        // Fast-path: look for '['.
        if (src[i] != '[') {
            ++i;
            continue;
        }

        // Attempt to match [shader("<stage>")].
        // Pattern: [shader("  <-- already at '[', match up through opening '"'
        std::size_t pos = i;
        if (!match_literal(src, len, pos, "[shader(\"")) {
            ++i;
            continue;
        }

        // Read the stage string content (pos is now past the opening '"').
        // Scan until we hit the closing '"'.
        std::size_t stage_begin = pos;
        while (pos < len && src[pos] != '"' && src[pos] != '\n') {
            ++pos;
        }
        if (pos >= len || src[pos] != '"') {
            ++i;
            continue;
        }
        std::size_t stage_len = pos - stage_begin;
        ++pos;  // consume closing '"'

        // Match the closing )] of the attribute.
        if (!match_literal(src, len, pos, ")]")) {
            ++i;
            continue;
        }

        // We have a valid [shader("...")] attribute ending at pos.
        // stage_begin..stage_begin+stage_len are the stage bytes.
        std::pmr::string stage_str{src + stage_begin, stage_len, mr};

        // Step B: skip whitespace.
        skip_ws(src, len, pos);

        // Step C: skip additional [...] attribute blocks.
        while (pos < len && src[pos] == '[') {
            std::size_t saved = pos;
            if (!skip_attr_block(src, len, pos)) {
                pos = saved;
                break;
            }
            skip_ws(src, len, pos);
        }

        // Step D: match return_type_ident WS+ fn_name_ident WS* '('.
        std::size_t rt_begin = 0, rt_len = 0;
        if (!scan_ident(src, len, pos, &rt_begin, &rt_len)) {
            // No identifier — not a function declaration.
            i = pos;
            continue;
        }

        // Must have at least one whitespace between return type and name.
        if (pos >= len || !is_ws(src[pos])) {
            i = pos;
            continue;
        }
        skip_ws(src, len, pos);

        std::size_t fn_begin = 0, fn_len = 0;
        if (!scan_ident(src, len, pos, &fn_begin, &fn_len)) {
            i = pos;
            continue;
        }

        skip_ws(src, len, pos);

        if (pos >= len || src[pos] != '(') {
            i = pos;
            continue;
        }

        // Valid entry point found.
        std::pmr::string fn_name{src + fn_begin, fn_len, mr};
        results.push_back(RawEntryPoint{std::move(stage_str), std::move(fn_name)});

        // Advance main cursor past the '[shader("...")]' attribute only, NOT
        // past the function name.  This allows a subsequent [shader("...")] on
        // the next line (stacked attributes pattern) to be found and paired with
        // the same function — which will then be flagged as EntryPointStageAmbiguous.
        // If we advanced past the function we would miss stacked attributes.
        ++i;
    }

    return results;
}

}  // namespace

std::expected<std::pmr::vector<EntryPoint>, Error>
scan_entry_points(const std::pmr::string& source, std::pmr::memory_resource* mr) {
    const char* src = source.c_str();
    std::size_t len = source.size();

    std::pmr::vector<RawEntryPoint> raw = extract_raw_entry_points(src, len, mr);

    // Detect duplicate / ambiguous entries using a linear search over the (small)
    // result set.  We keep insertion order intact — never use a hash map
    // (PHILOSOPHY §7).
    //
    // Spec §4.1 invariant 1: "Every emitted EntryPoint has exactly one stage
    // attribute."  Two distinct error arms apply (R2 MED-3 fix):
    //
    //   same fn_name + different stage → EntryPointStageAmbiguous
    //     (the function is bound to two pipeline stages; the compiler would have
    //      to pick one — ambiguous by spec).
    //
    //   same fn_name + same stage → treat as redefinition (e.g. duplicate include).
    //     The duplicate occurrence is silently collapsed: only the first encounter
    //     is emitted.  This is the correct behaviour because including the same
    //     file twice is idiomatic and the two declarations are identical.
    for (std::size_t a = 0; a < raw.size(); ++a) {
        for (std::size_t b = a + 1; b < raw.size(); ++b) {
            if (raw[a].fn_name == raw[b].fn_name) {
                if (raw[a].stage_str != raw[b].stage_str) {
                    // Different stages on the same function name — ambiguous.
                    return std::unexpected(Error::EntryPointStageAmbiguous);
                }
                // Same stage on the same function name — mark the duplicate
                // for suppression (clear its fn_name so the build loop skips it).
                raw[b].fn_name.clear();
            }
        }
    }

    // Build result in source-encounter order, skipping unknown stage strings and
    // suppressed (duplicate same-stage) entries.
    std::pmr::vector<EntryPoint> result{mr};
    result.reserve(raw.size());
    for (const auto& rep : raw) {
        if (rep.fn_name.empty()) {
            continue;  // Suppressed duplicate same-stage entry.
        }
        auto maybe_stage = parse_stage(rep.stage_str.c_str(), rep.stage_str.size());
        if (!maybe_stage) {
            continue;  // Unknown stage string — skip.
        }
        // Construct EntryPoint::name with mr so the string allocates under
        // ContextTag::shader (perf-budget.md §Allocator Rules #1).
        result.push_back(EntryPoint{std::pmr::string{rep.fn_name.c_str(), mr}, *maybe_stage});
    }

    return result;
}

}  // namespace glibre::shader::detail
