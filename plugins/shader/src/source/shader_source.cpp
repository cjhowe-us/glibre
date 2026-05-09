// plugins/shader/src/source/shader_source.cpp
//
// ShaderSource aggregate root implementation (§4.1 of specs/shader/SPEC.md).
//
// Responsibility: "Own one Slang translation unit on disk and the set of
// stage-tagged entry points it exposes."
//
// Algorithm of ShaderSource::open():
//   1. Reject if project_relative is absolute.
//   2. Construct abs_path = project_root / project_relative.
//   3. Validate abs_path is within project_root via detail::validate_in_project_root.
//   4. Read + normalize the root file (UTF-8 BOM strip, encoding check,
//      empty-file rejection).  Return Error::SourceNotFound if absent,
//      Error::EncodingInvalid if malformed.
//   5. Run include expansion (preprocessor.hpp) → PreprocessedSource bytes
//      plus include_closure accumulator.
//   6. Run entry-point scanner → eastl::vector<EntryPoint>.
//   7. Compute total BLAKE3 over expanded bytes (detail::blake3_hash from shader_hash.hpp).
//   8. Assemble and return ShaderSource.

#include <cstring>

#include <glibre/shader/shader.hpp>

#include "entry_point_scanner.hpp"
#include "include_resolver.hpp"
#include "preprocessor.hpp"
#include "shader_hash.hpp"

namespace glibre::shader {

// ---------------------------------------------------------------------------
// ShaderSource::open
// ---------------------------------------------------------------------------

glibre::Result<ShaderSource> ShaderSource::open(
    const std::filesystem::path& project_root, const std::filesystem::path& project_relative
) {
    // Reject absolute project_relative (would escape the project root).
    if (project_relative.is_absolute()) {
        return std::unexpected(Error::IncludeEscape);
    }

    std::filesystem::path abs_path = (project_root / project_relative).lexically_normal();

    // Validate abs_path is within project_root using the shared containment
    // helper (R2 HIGH-2: consolidates the duplicate inline predicate into a
    // single SRP module — detail::validate_in_project_root in include_resolver.hpp).
    if (!detail::validate_in_project_root(abs_path, project_root)) {
        return std::unexpected(Error::IncludeEscape);
    }

    // Step 1: read + normalize root file (HIGH-4: BOM strip, UTF-8 check,
    // empty-file rejection via read_and_normalize_file).
    auto root_result = detail::read_and_normalize_file(abs_path);
    if (!root_result) {
        return std::unexpected(root_result.error());
    }
    eastl::string root_bytes = std::move(*root_result);

    // Step 2: expand includes.
    eastl::vector<IncludeNode> include_closure;
    detail::PreprocessContext ctx{
        project_root, include_closure, {}  // empty visit_stack
    };

    // Push the root file onto the visit stack so it participates in cycle detection.
    auto proj_rel_norm = project_relative.lexically_normal();
    eastl::string root_rel_str{proj_rel_norm.native().c_str(), proj_rel_norm.native().size()};
    ctx.visit_stack.push_back(root_rel_str);

    auto expanded_result = detail::expand_includes(root_bytes, abs_path, ctx);
    if (!expanded_result) {
        return std::unexpected(expanded_result.error());
    }
    eastl::string expanded = std::move(*expanded_result);

    // Step 3: scan entry points from the EXPANDED source (so we also see
    // entry points declared in included files).
    auto ep_result = detail::scan_entry_points(expanded);
    if (!ep_result) {
        return std::unexpected(ep_result.error());
    }
    eastl::vector<EntryPoint> entry_points = std::move(*ep_result);

    // Step 4: compute total hash over the expanded byte stream.
    ShaderHash total_hash = detail::blake3_hash(expanded.data(), expanded.size());

    // Step 5: pack into bytes for PreprocessedSource.
    // LOW-1 fix: use memcpy instead of a manual byte-cast loop.
    eastl::vector<std::byte> expanded_bytes;
    expanded_bytes.resize(expanded.size());
    std::memcpy(expanded_bytes.data(), expanded.data(), expanded.size());

    // Step 6: assemble ShaderSource.
    // MED-5: all fields are populated exactly here via move-only open().
    // No public setters exist; post-construction mutation is closed off.
    ShaderSource src;
    src.id_ = SourceId{root_rel_str};
    src.entry_points_ = std::move(entry_points);
    src.preprocessed_ =
        PreprocessedSource{std::move(expanded_bytes), std::move(include_closure), total_hash};

    return src;
}

// ---------------------------------------------------------------------------
// ShaderSource accessors
// ---------------------------------------------------------------------------

const SourceId& ShaderSource::id() const noexcept { return id_; }

// MED-5: entry_points() returns a span over entry_points_.  The ShaderSource
// is populated only once (via open()) and has no public mutation path, so the
// span lifetime matches the owning ShaderSource lifetime.  Callers must not
// hold a span across a move of the ShaderSource.
eastl::span<const EntryPoint> ShaderSource::entry_points() const noexcept {
    return eastl::span<const EntryPoint>{entry_points_.data(), entry_points_.size()};
}

const PreprocessedSource& ShaderSource::preprocessed() const noexcept { return preprocessed_; }

}  // namespace glibre::shader
