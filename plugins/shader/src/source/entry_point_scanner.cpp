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
//   Step 1. Find all [shader("stage")] occurrences and their end positions.
//   Step 2. For each attribute occurrence, scan forward past any additional
//           [...] attribute blocks to find the function name that follows.
//           A "function name" is an identifier directly followed by '(' after
//           skipping a return type (void or any identifier) and whitespace.
//   Step 3. Group (stage_str, fn_name) pairs by fn_name.
//           If any fn_name appears more than once → EntryPointStageAmbiguous.
//
// Note: this scanner deliberately avoids full Slang parsing.  Full parsing
// is slangc's job (§4.3).

#include "entry_point_scanner.hpp"

#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace glibre::shader::detail {

namespace {

/// Map Slang stage attribute string to our Stage enum.
std::optional<Stage> parse_stage(const std::string& stage_str) {
    if (stage_str == "vertex")        return Stage::Vertex;
    if (stage_str == "pixel")         return Stage::Pixel;
    if (stage_str == "compute")       return Stage::Compute;
    if (stage_str == "mesh")          return Stage::Mesh;
    if (stage_str == "amplification") return Stage::Amplification;
    if (stage_str == "library")       return Stage::Library;
    return std::nullopt;
}

struct RawEntryPoint {
    std::string stage_str;
    std::string fn_name;
};

/// Scan source string for [shader("...")] attributes and the function names
/// that follow them.
///
/// For each attribute found, scan forward in the source:
///   - Skip whitespace/newlines.
///   - Skip additional [...] attribute blocks.
///   - Match a return type (identifier or void).
///   - Match whitespace.
///   - Capture the next identifier as the function name.
///   - Confirm it is followed by '('.
std::vector<RawEntryPoint>
extract_raw_entry_points(const std::string& source) {
    // Match [shader("stage")] — captures stage string.
    static const std::regex kAttrRe{
        R"re(\[shader\("([a-zA-Z]+)"\)\])re"
    };
    // Match fn_decl: (return_type WS+ fn_name WS* '(')
    // The return_type is any identifier (incl. void).
    // fn_name is the SECOND identifier — the one immediately before '('.
    //
    // We look for this pattern in the tail after all attribute blocks.
    // A simplified pattern that matches "word WS+ word WS* (" where the
    // second word is the function name.
    static const std::regex kFnDeclRe{
        R"re(([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*\()re"
    };
    // Match a single attribute block [...] (any content, non-nested).
    static const std::regex kAttrBlockRe{
        R"re(\[[^\]]*\])re"
    };
    // Match an identifier (for skip-return-type scanning).
    static const std::regex kIdentRe{
        R"re([A-Za-z_]\w*)re"
    };

    std::vector<RawEntryPoint> results;

    auto it  = std::sregex_iterator{source.begin(), source.end(), kAttrRe};
    auto end = std::sregex_iterator{};

    for (; it != end; ++it) {
        const std::smatch& m = *it;
        std::string stage_str = m[1].str();

        // pos points to the character immediately after the matched attribute.
        auto attr_end = static_cast<std::size_t>(m.position() + m.length());
        std::string tail = source.substr(attr_end);

        // Step A: skip whitespace + newlines.
        std::size_t pos = 0;
        while (pos < tail.size() &&
               (tail[pos] == ' ' || tail[pos] == '\t' ||
                tail[pos] == '\r' || tail[pos] == '\n')) {
            ++pos;
        }

        // Step B: skip additional [...] attribute blocks (possibly multiple).
        // Repeat until no more attribute blocks at the current position.
        bool skipped = true;
        while (skipped) {
            skipped = false;
            std::string from_pos = tail.substr(pos);
            std::smatch ab;
            // Only match at the beginning of from_pos.
            if (std::regex_search(from_pos, ab, kAttrBlockRe) &&
                ab.position() == 0) {
                pos += static_cast<std::size_t>(ab.length());
                // Skip trailing whitespace/newlines.
                while (pos < tail.size() &&
                       (tail[pos] == ' ' || tail[pos] == '\t' ||
                        tail[pos] == '\r' || tail[pos] == '\n')) {
                    ++pos;
                }
                skipped = true;
            }
        }

        // Step C: match the function declaration pattern in the remaining tail.
        std::string decl_tail = tail.substr(pos);
        std::smatch fn_m;
        if (!std::regex_search(decl_tail, fn_m, kFnDeclRe)) {
            // No function declaration found after this attribute — skip.
            continue;
        }

        // Verify the match starts at the beginning of decl_tail (after WS skip).
        if (fn_m.position() != 0) {
            // There is non-whitespace non-attribute content before the function —
            // this attribute may not directly precede a function declaration.
            continue;
        }

        // fn_m[1] = return type, fn_m[2] = function name.
        std::string fn_name = fn_m[2].str();

        results.push_back(RawEntryPoint{std::move(stage_str), std::move(fn_name)});
    }

    return results;
}

}  // namespace

std::expected<eastl::vector<EntryPoint>, Error>
scan_entry_points(const eastl::string& source) {
    std::string std_source{source.c_str(), source.size()};

    std::vector<RawEntryPoint> raw = extract_raw_entry_points(std_source);

    // Group by function name.
    std::unordered_map<std::string, std::vector<std::string>> seen;
    for (auto& rep : raw) {
        seen[rep.fn_name].push_back(rep.stage_str);
    }

    // Detect ambiguous entries (same function name with multiple [shader("...")] attrs).
    for (const auto& [name, stages] : seen) {
        if (stages.size() > 1u) {
            return std::unexpected(Error::EntryPointStageAmbiguous);
        }
    }

    // Build result.
    eastl::vector<EntryPoint> result;
    result.reserve(static_cast<eastl::vector<EntryPoint>::size_type>(seen.size()));
    for (const auto& [name, stages] : seen) {
        auto maybe_stage = parse_stage(stages[0]);
        if (!maybe_stage) {
            continue;  // Unknown stage string — skip.
        }
        result.push_back(EntryPoint{
            eastl::string{name.c_str(), name.size()},
            *maybe_stage
        });
    }

    return result;
}

}  // namespace glibre::shader::detail
