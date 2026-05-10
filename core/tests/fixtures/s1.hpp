#pragma once
// core/tests/fixtures/s1.hpp
//
// Catch2 fixture helper — canonical S1 sample-scene loader.
//
// Authority: plan #1003, reviews/decisions/perf-budget.md §Justification Per Cell (S1).
//
// Usage:
//   #include "fixtures/s1.hpp"
//
//   TEST_CASE("my_bench", "[perf][core][s1]") {
//       auto scene = glibre::testing::load_s1();
//       REQUIRE(scene.entity_count == 209u);
//       // ...
//   }
//
// Design decisions:
//   - Header-only; no glibre::core link required.  Reads the YAML manifest
//     at test setup via a std::ifstream-based parser.
//   - std::filesystem / std::ifstream are PHILOSOPHY §11 permitted carve-outs
//     (language/runtime utilities EASTL does not own).
//   - No YAML library dependency.  The manifest is a hand-written subset of
//     YAML (key: value pairs only); parsing is a simple line-scan.  This is
//     intentional: the manifest is machine-written and machine-read; structural
//     complexity in the parser would be scope creep.
//   - S1Scene is plain data (no destructors, no allocation) so it is safe to
//     construct in any test thread without allocator setup.
//   - load_s1() locates the manifest at runtime using the GLIBRE_REPO_ROOT
//     environment variable if set, otherwise by traversing from __FILE__
//     upward until e2e/perf/s1/scene.yaml is found.
//
// PHILOSOPHY §7 (determinism): placement_seed is read and stored so callers
// can expand deterministic transforms from seed + entity_index arithmetic.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace glibre::testing {

// ---------------------------------------------------------------------------
// Viewport — S1 viewport descriptor
// ---------------------------------------------------------------------------

struct S1Viewport {
    std::uint32_t width  = 0u;
    std::uint32_t height = 0u;
};

// ---------------------------------------------------------------------------
// S1Budgets — per-context CPU budget cells (nanoseconds)
//
// Verbatim from reviews/decisions/perf-budget.md §Per-Context Budget Table.
// Fields match the budget keys in e2e/perf/s1/scene.yaml.
// ---------------------------------------------------------------------------

struct S1Budgets {
    std::uint64_t core_cpu_sim_ns        = 0u;
    std::uint64_t core_cpu_submit_ns     = 0u;
    std::uint64_t render_cpu_sim_ns      = 0u;
    std::uint64_t render_cpu_submit_ns   = 0u;
    std::uint64_t geometry_cpu_sim_ns    = 0u;
    std::uint64_t geometry_cpu_submit_ns = 0u;
    std::uint64_t physics_cpu_sim_ns     = 0u;
    std::uint64_t physics_cpu_submit_ns  = 0u;
};

// ---------------------------------------------------------------------------
// S1ArchetypeCounts — entity composition by archetype
// ---------------------------------------------------------------------------

struct S1ArchetypeCounts {
    std::uint32_t character     = 0u;   // 1
    std::uint32_t prop          = 0u;   // 200
    std::uint32_t dynamic_light = 0u;   // 8
};

// ---------------------------------------------------------------------------
// S1Scene — aggregate returned by load_s1()
//
// All fields are plain data; the struct is safe to copy and compare.
// ---------------------------------------------------------------------------

struct S1Scene {
    std::uint32_t     entity_count    = 0u;
    std::uint32_t     archetype_count = 0u;
    std::uint32_t     system_count    = 0u;
    S1ArchetypeCounts archetypes      = {};
    S1Viewport        viewport        = {};
    std::uint32_t     placement_seed  = 0u;
    S1Budgets         budgets         = {};
};

// ---------------------------------------------------------------------------
// detail — internal helpers
// ---------------------------------------------------------------------------

namespace detail {

// locate_repo_root: traverse upward from a starting path until a directory
// containing e2e/perf/s1/scene.yaml is found.  Returns the repo root path.
// Throws std::runtime_error on failure.
inline std::filesystem::path locate_repo_root() {
    // Allow override via environment variable (e.g. in CI).
    if (const char* env = std::getenv("GLIBRE_REPO_ROOT")) {
        return std::filesystem::path(env);
    }

    // Derive from __FILE__ (absolute path of this header at compile time).
    // Walk up until e2e/perf/s1/scene.yaml exists as a sibling.
    std::filesystem::path p = std::filesystem::path(__FILE__).parent_path();
    while (p.has_parent_path() && p != p.parent_path()) {
        if (std::filesystem::exists(p / "e2e" / "perf" / "s1" / "scene.yaml")) {
            return p;
        }
        p = p.parent_path();
    }
    throw std::runtime_error(
        "glibre::testing::load_s1(): cannot locate repo root from " __FILE__
        ". Set GLIBRE_REPO_ROOT to the absolute repo path.");
}

// trim: remove leading/trailing ASCII whitespace from a string, return result.
inline std::string trim(const std::string& s) {
    const std::size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, (last - first) + 1u);
}

// parse_uint64: parse decimal or 0x-prefixed hex uint64 from a non-empty string.
inline std::uint64_t parse_uint64(const std::string& s) {
    return static_cast<std::uint64_t>(std::stoull(s, nullptr, 0));
}

// parse_uint32: parse decimal or 0x-prefixed hex uint32 from a non-empty string.
inline std::uint32_t parse_uint32(const std::string& s) {
    return static_cast<std::uint32_t>(std::stoull(s, nullptr, 0));
}

// ParsedLine: result of stripping a single line from the manifest.
struct ParsedLine {
    std::string  key;           // trimmed key (left of first colon)
    std::string  val;           // trimmed value (right of first colon); empty for block headers
    std::size_t  indent;        // leading space count (0 = top-level)
    bool         has_val;       // true if a non-empty value was found
};

// parse_line: strip comment, split on first colon, return ParsedLine.
// Returns false (via has_val=false, key.empty()) if the line has no colon.
inline ParsedLine parse_line(std::string line) {
    // Strip inline comment (first '#').
    const std::size_t hash = line.find('#');
    if (hash != std::string::npos) {
        line = line.substr(0, hash);
    }

    // Count leading spaces/tabs for indentation.
    std::size_t indent = 0u;
    for (std::size_t i = 0u; i < line.size(); ++i) {
        if (line[i] == ' ' || line[i] == '\t') {
            ++indent;
        } else {
            break;
        }
    }

    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) {
        return {"", "", indent, false};
    }

    const std::string key = trim(line.substr(0u, colon));
    const std::string val = trim(line.substr(colon + 1u));
    return {key, val, indent, !val.empty()};
}

}  // namespace detail

// ---------------------------------------------------------------------------
// load_s1() — load the canonical S1 manifest and return an S1Scene aggregate.
//
// Reads e2e/perf/s1/scene.yaml from the repo root (located at runtime via
// GLIBRE_REPO_ROOT env-var or __FILE__-based upward traversal).
//
// Throws std::runtime_error if the manifest cannot be found or parsed.
//
// Thread-safety: each call opens and parses independently; safe to call from
// multiple test threads (different S1Scene objects, no shared state).
// ---------------------------------------------------------------------------
[[nodiscard]] inline S1Scene load_s1() {
    const std::filesystem::path root = detail::locate_repo_root();
    const std::filesystem::path manifest = root / "e2e" / "perf" / "s1" / "scene.yaml";

    std::ifstream ifs(manifest);
    if (!ifs.is_open()) {
        throw std::runtime_error(
            "glibre::testing::load_s1(): cannot open " + manifest.string());
    }

    S1Scene scene;

    // ---------------------------------------------------------------------------
    // Parser state
    //
    // The manifest uses a simple structured YAML format:
    //   - Top-level scalar fields (entity_count, system_count, placement_seed)
    //   - Two-level blocks (archetypes > archetype_name > count:)
    //   - One-level blocks (viewport > width/height, budgets > key: value)
    //
    // Strategy: single-pass parser that tracks current section and nesting.
    // When a block header is encountered (non-empty key, empty value), we
    // switch sections.  When a value is encountered inside a section, we
    // dispatch to the appropriate field.
    // ---------------------------------------------------------------------------

    enum class Section { None, Archetypes, Viewport, Budgets };
    Section section = Section::None;
    std::string arch_name;  // tracks the current archetype name in the Archetypes block

    std::string raw;
    while (std::getline(ifs, raw)) {
        const auto p = detail::parse_line(raw);

        // Skip blank lines and lines with no colon.
        if (p.key.empty()) {
            continue;
        }

        // ---------------------------------------------------------------------------
        // Top-level (indent == 0): section headers and top-level scalar fields.
        // ---------------------------------------------------------------------------
        if (p.indent == 0u) {
            arch_name.clear();

            if (!p.has_val) {
                // Block header — switch section.
                if      (p.key == "archetypes") { section = Section::Archetypes; }
                else if (p.key == "viewport")   { section = Section::Viewport;   }
                else if (p.key == "budgets")    { section = Section::Budgets;    }
                else                            { section = Section::None;        }
            } else {
                // Top-level scalar field.
                section = Section::None;
                if      (p.key == "entity_count")   { scene.entity_count   = detail::parse_uint32(p.val); }
                else if (p.key == "system_count")   { scene.system_count   = detail::parse_uint32(p.val); }
                else if (p.key == "placement_seed") { scene.placement_seed = detail::parse_uint32(p.val); }
                // "version" is parsed but not stored (schema version, reserved for future use).
            }
            continue;
        }

        // ---------------------------------------------------------------------------
        // Indented lines: dispatch to current section.
        // ---------------------------------------------------------------------------

        switch (section) {
        case Section::None:
            break;

        case Section::Archetypes:
            if (p.indent == 2u && !p.has_val) {
                // Archetype block header (e.g. "  character:").
                arch_name = p.key;
            } else if (p.indent == 4u && p.has_val && p.key == "count" && !arch_name.empty()) {
                // Count field nested under an archetype name.
                const std::uint32_t n = detail::parse_uint32(p.val);
                if      (arch_name == "character")     { scene.archetypes.character     = n; }
                else if (arch_name == "prop")          { scene.archetypes.prop          = n; }
                else if (arch_name == "dynamic_light") { scene.archetypes.dynamic_light = n; }
            }
            break;

        case Section::Viewport:
            if (p.indent > 0u && p.has_val) {
                if      (p.key == "width")  { scene.viewport.width  = detail::parse_uint32(p.val); }
                else if (p.key == "height") { scene.viewport.height = detail::parse_uint32(p.val); }
            }
            break;

        case Section::Budgets:
            if (p.indent > 0u && p.has_val) {
                auto& b = scene.budgets;
                if      (p.key == "core_cpu_sim_ns")        { b.core_cpu_sim_ns        = detail::parse_uint64(p.val); }
                else if (p.key == "core_cpu_submit_ns")     { b.core_cpu_submit_ns     = detail::parse_uint64(p.val); }
                else if (p.key == "render_cpu_sim_ns")      { b.render_cpu_sim_ns      = detail::parse_uint64(p.val); }
                else if (p.key == "render_cpu_submit_ns")   { b.render_cpu_submit_ns   = detail::parse_uint64(p.val); }
                else if (p.key == "geometry_cpu_sim_ns")    { b.geometry_cpu_sim_ns    = detail::parse_uint64(p.val); }
                else if (p.key == "geometry_cpu_submit_ns") { b.geometry_cpu_submit_ns = detail::parse_uint64(p.val); }
                else if (p.key == "physics_cpu_sim_ns")     { b.physics_cpu_sim_ns     = detail::parse_uint64(p.val); }
                else if (p.key == "physics_cpu_submit_ns")  { b.physics_cpu_submit_ns  = detail::parse_uint64(p.val); }
            }
            break;
        }
    }

    // ---------------------------------------------------------------------------
    // Derive archetype_count: number of distinct archetypes with count > 0.
    // ---------------------------------------------------------------------------
    scene.archetype_count = 0u;
    if (scene.archetypes.character     > 0u) { ++scene.archetype_count; }
    if (scene.archetypes.prop          > 0u) { ++scene.archetype_count; }
    if (scene.archetypes.dynamic_light > 0u) { ++scene.archetype_count; }

    return scene;
}

}  // namespace glibre::testing
