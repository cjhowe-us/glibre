// core/src/context_tag_resolver.cpp
//
// derive_context_tag implementation — pure string→ContextTag map.
//
// SRP: this translation unit has exactly one reason to change — the mapping
// between bounded-context name strings (from perf-budget.md §Per-Context
// Budget Table) and ContextTag enumerators.  It has zero dependency on
// dlopen, dlsym, PluginManifest binary layout, or any loader lifecycle state.
//
// Authority: perf-budget.md §Allocator Rules #1, plan #989.

#include "glibre/core/context_tag_resolver.hpp"

#include <EASTL/string_view.h>

#include "glibre/error.hpp"

namespace glibre::core {

// ---------------------------------------------------------------------------
// derive_context_tag — map a dot-namespaced plugin name to its ContextTag.
//
// Plugin naming convention: "glibre.<context>[.<sub>...]".
// Step 1: skip the leading "glibre." prefix (first dot-delimited component).
// Step 2: extract the second component (the context name).
// Step 3: compare against the nine known bounded-context names from
//         perf-budget.md §Per-Context Budget Table.
//
// Returns core::Error::PluginManifestInvalid on:
//   - name has fewer than two dot-delimited components.
//   - second component does not match any known ContextTag name.
// ---------------------------------------------------------------------------

[[nodiscard]] glibre::Result<glibre::ContextTag>
derive_context_tag(eastl::string_view plugin_name) noexcept {
    // Find the first dot — skips the leading namespace component (e.g. "glibre").
    const auto first_dot = plugin_name.find('.');
    if (first_dot == eastl::string_view::npos) {
        return std::unexpected(
            glibre::Error{
                core::Error::PluginManifestInvalid,
                ErrorContext{
                    .file = __FILE__,
                    .line = __LINE__,
                    .detail =
                        "plugin name has no dot separator (expected glibre.<context>[.<sub>...])",
                },
            }
        );
    }

    // Remaining string after the first dot: "<context>[.<sub>...]"
    const auto after_prefix = plugin_name.substr(first_dot + 1);

    // Extract the context component (up to the next dot, or the whole remainder).
    const auto second_dot = after_prefix.find('.');
    const auto context = (second_dot == eastl::string_view::npos)
                             ? after_prefix
                             : after_prefix.substr(0, second_dot);

    if (context.empty()) {
        return std::unexpected(
            glibre::Error{
                core::Error::PluginManifestInvalid,
                ErrorContext{
                    .file = __FILE__,
                    .line = __LINE__,
                    .detail = "plugin name has empty context component",
                },
            }
        );
    }

    // Map context string to ContextTag (perf-budget.md §Per-Context Budget Table).
    using eastl::string_view;
    if (context == string_view{"core"})
        return glibre::ContextTag::core;
    if (context == string_view{"platform"})
        return glibre::ContextTag::platform;
    if (context == string_view{"data"})
        return glibre::ContextTag::data;
    if (context == string_view{"shader"})
        return glibre::ContextTag::shader;
    if (context == string_view{"render"})
        return glibre::ContextTag::render;
    if (context == string_view{"geometry"})
        return glibre::ContextTag::geometry;
    if (context == string_view{"physics"})
        return glibre::ContextTag::physics;
    if (context == string_view{"content"})
        return glibre::ContextTag::content;
    if (context == string_view{"tools"})
        return glibre::ContextTag::tools;

    return std::unexpected(
        glibre::Error{
            core::Error::PluginManifestInvalid,
            ErrorContext{
                .file = __FILE__,
                .line = __LINE__,
                .detail = "plugin name context component does not match any known ContextTag",
            },
        }
    );
}

}  // namespace glibre::core
