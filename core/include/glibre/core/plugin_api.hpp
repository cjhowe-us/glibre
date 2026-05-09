#pragma once
// core/include/glibre/core/plugin_api.hpp
//
// PluginContext — the stable aggregate the engine passes to every plugin's
// glibre_plugin_register entry-point.
//
// DESIGN (reviews/decisions/plugin-abi.md §"Registration Entry-Point Signature"):
//
//   • PluginContext is a POD-like aggregate of references.  It is *not* a
//     class with a vtable; per Open Question #3 in plugin-abi.md the decision
//     leaned toward a POD aggregate to keep the extern "C" boundary clean.
//     Binary layout stability is guarded by PluginManifest.min_engine_version
//     (see §"Versioning Rules", axis 4).
//
//   • The aggregate contains references, not pointers, so a plugin cannot
//     take ownership or stash the context past the call.  The loader
//     guarantees the context outlives the call; after register() returns the
//     context is destroyed and any cached reference is dangling.
//
//   • All registry types are forward-declared as opaque tags here.  Their
//     real definitions land in their owning plans (ECS world, type registry,
//     render pass registry, editor panel registry).  Plugins that need a
//     real definition include the owning header alongside this one; this
//     header has no transitive pull on ECS or render headers.
//
//   • PHILOSOPHY §11: public plugin ABI surfaces never expose eastl:: or
//     std:: containers — they cross the boundary as POD spans / handles only.
//     PluginContext carries only references to engine-owned registries.
//     The LogSink reference is spdlog-backed; spdlog itself is a compile-time
//     dependency of the plugin (header-only), not a runtime ABI surface.
//
//   • Registration: plugins call the registry objects directly through the
//     references exposed in PluginContext (type_registry, system_registry,
//     pass_registry, panel_registry).  No convenience wrapper methods live on
//     PluginContext itself — that would split responsibility between the context
//     aggregate and the registries that own registration logic (SRP).  The real
//     TypeRegistry and SystemRegistry APIs land in plan #229 when those types
//     are fully defined.  Until then, plugins reference the opaque registry
//     fields and plan #229 will define the complete callable surface.
//
//   • Registration phase: plugin_api.md §"Loader Sequence" step 9 — all
//     mutations happen during phase 8 (HotReload); the world is already
//     drained when register() is called.  The Phase enum from
//     glibre/core/frame_phase.hpp is usable by plugins that call
//     register_system() with an explicit phase argument (forwarded to
//     SystemRegistry).
//
// Compilation requirements:
//   -fno-exceptions (error-model.md §Decision 3)
//   C++23
//   No runtime reflection

#include <cstddef>
#include <cstdint>

#include <glibre/error.hpp>

// ---------------------------------------------------------------------------
// Forward declarations — opaque tags for MVP.
// Real types land in their respective bounded-context plans.
// Plugins that register components must include the owning header
// (e.g. glibre/core/world.hpp when it lands in plan #ECS-world) in addition
// to this header.  Forward declarations here make plugin_api.hpp compilable
// in isolation and free it from transitive ECS / render / editor includes.
//
// Forward-declared as 'class' to match the expected definition keyword from
// #229 onwards and avoid -Wmismatched-tags diagnostics.
// ---------------------------------------------------------------------------

// PluginManifest shipped in PR #952 (plan #223).  Include the canonical
// header rather than forward-declaring to avoid -Wmismatched-tags when TUs
// include both plugin_api.hpp and plugin_manifest.hpp.
#include <glibre/core/plugin_manifest.hpp>

namespace glibre::core {

class World;           // ECS world — plan: #ECS-world (pending)
class TypeRegistry;    // component-type registry — plan: #type-registry (pending)
class SystemRegistry;  // system-schedule registry — plan: #system-registry (pending)
class PassRegistry;    // render-graph pass registry — plan: #pass-registry (pending)
class PanelRegistry;   // editor UI panel registry — plan: #panel-registry (pending)
class LogSink;         // structured spdlog sink — plan: #log-sink (pending)

// ---------------------------------------------------------------------------
// PluginContext — stable POD-like aggregate passed by reference to every
// glibre_plugin_register() invocation.
//
// Field layout is ABI-stable under the min_engine_version guard.  Adding
// fields is additive (append only); removing or reordering fields requires
// bumping the engine's SemVer and all plugins' min_engine_version.
//
// Per plugin-abi.md §"Registration Entry-Point Signature":
//   "PluginContext is a reference, not a pointer, so the plugin cannot take
//    ownership or store it past the call."
// ---------------------------------------------------------------------------

struct PluginContext {
    // ------------------------------------------------------------------
    // Core engine registries
    // ------------------------------------------------------------------

    /// ECS world the plugin registers components and systems into.
    World& world;

    /// Component-type registry.  Plugins call register_component() against
    /// this object directly once plan #229 defines the real TypeRegistry API.
    TypeRegistry& type_registry;

    /// System-schedule registry.  Plugins call register_system() against
    /// this sink.  The loader rebuilds the schedule topology from all
    /// registered systems after all plugins' register() calls complete
    /// (loader sequence step 10).
    SystemRegistry& system_registry;

    /// Render-graph pass registry.  Only meaningful for plugins contributing
    /// render passes (phases 6 / 7).  Editor and physics plugins leave this
    /// untouched.
    PassRegistry& pass_registry;

    /// Editor UI panel registry.  No-op for non-editor builds (the registry
    /// is a stub in shipping runtime).
    PanelRegistry& panel_registry;

    // ------------------------------------------------------------------
    // Manifest back-reference
    // ------------------------------------------------------------------

    /// The loader passes the deserialized PluginManifest back into
    /// register() so the plugin can iterate its declared components,
    /// systems, passes, and panels rather than duplicating that list.
    /// The manifest is owned by the loader; its lifetime outlives the
    /// register() call.
    const PluginManifest& manifest;

    // ------------------------------------------------------------------
    // Diagnostics
    // ------------------------------------------------------------------

    /// Structured spdlog-backed sink for registration-time diagnostics.
    /// The plugin MUST use this sink (not a global logger) so that log
    /// lines carry the plugin name as a contextual tag.
    LogSink& log;
};

}  // namespace glibre::core
