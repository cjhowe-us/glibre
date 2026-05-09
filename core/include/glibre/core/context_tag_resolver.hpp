#pragma once
// core/include/glibre/core/context_tag_resolver.hpp
//
// derive_context_tag — map a dot-namespaced plugin name to its bounded-context
// ContextTag.
//
// SRP note: this is a pure string→enum map with zero dependency on dlopen,
// dlsym, manifest binary state, or any loader lifecycle concern.  It is
// separated from plugin_loader.hpp so that the two distinct reasons to change
// (loader protocol changes vs. ContextTag/bounded-context naming changes) do
// not couple a single translation unit (MED-2, round-2 review).
//
// Authority: perf-budget.md §Allocator Rules #1, plan #989.
// Caller:    plugin_loader_actions.cpp::call_register_stamped (HIGH-1, round-2).

#include <EASTL/string_view.h>
#include <glibre/alloc.hpp>  // ContextTag
#include <glibre/error.hpp>

namespace glibre::core {

// ---------------------------------------------------------------------------
// derive_context_tag — map a dot-namespaced plugin name to its ContextTag.
//
// Plugin names follow the convention "glibre.<context>[.<component>...]".
// The second dot-delimited component names the bounded context (e.g. "render"
// in "glibre.render.camera" → ContextTag::render).
//
// This is the loader-side stamping function called at glibre_plugin_register
// time (perf-budget.md §Allocator Rules #1, plan #989).  The caller extracts
// the manifest name from PluginManifest::name and passes it here; the returned
// ContextTag is used to stamp an AllocatorHandle before constructing the
// PluginContext passed to the plugin's entry-point.
//
// Returns:
//   ContextTag matching the context component of the name, on success.
//   core::Error::PluginManifestInvalid if the name cannot be parsed or the
//   context component does not map to a known ContextTag.
//
// Known mappings (perf-budget.md §Per-Context Budget Table):
//   "core"     → ContextTag::core
//   "platform" → ContextTag::platform
//   "data"     → ContextTag::data
//   "shader"   → ContextTag::shader
//   "render"   → ContextTag::render
//   "geometry" → ContextTag::geometry
//   "physics"  → ContextTag::physics
//   "content"  → ContextTag::content
//   "tools"    → ContextTag::tools
// ---------------------------------------------------------------------------

[[nodiscard]] glibre::Result<glibre::ContextTag>
derive_context_tag(eastl::string_view plugin_name) noexcept;

}  // namespace glibre::core
