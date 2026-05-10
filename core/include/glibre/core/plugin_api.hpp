#pragma once
// core/include/glibre/core/plugin_api.hpp
//
// PluginContext — the stable aggregate the engine passes to every plugin's
// glibre_plugin_register entry-point.
//
// DESIGN (reviews/decisions/plugin-abi.md §"Registration Entry-Point Signature"):
//
//   • PluginContext is a POD-like aggregate passed to every plugin's
//     glibre_plugin_register() entry-point.  It is *not* a class with a
//     vtable; per Open Question #3 in plugin-abi.md the decision leaned
//     toward a POD aggregate to keep the extern "C" boundary clean.
//     Binary layout stability is guarded by PluginManifest.min_engine_version
//     (see §"Versioning Rules", axis 4).
//
//   • Most fields are references, so a plugin cannot take ownership or
//     stash them past the call.  The loader guarantees the context outlives
//     the call; after register() returns the context is destroyed and any
//     cached reference is dangling.
//
//   • PluginContext::alloc is a value-typed AllocatorHandle (plan #989).
//     AllocatorHandle is copy/move-constructible and non-assignable; it
//     internally holds a PerContextAllocator& so the engine's long-lived
//     allocator is not copied.  This makes PluginContext a mixed-storage
//     aggregate (references + one value type) rather than a pure reference
//     aggregate.  The "POD-like" characterisation refers to the absence of a
//     vtable and user-provided constructors; it does not require all fields
//     to be references.
//
//   • All registry types are forward-declared as opaque tags here.  Their
//     real definitions land in their owning plans (ECS world, type registry,
//     render pass registry, editor panel registry).  Plugins that need a
//     real definition include the owning header alongside this one; this
//     header has no transitive pull on ECS or render headers.
//
//   • PHILOSOPHY §11 (updated per reviews/decisions/eastl-removal.md): public plugin
//     ABI surfaces never expose std:: or std::pmr:: containers — they cross the
//     boundary as POD spans / handles only.
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

#include <glibre/alloc.hpp>
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

    // ------------------------------------------------------------------
    // Allocator handle — APPEND-ONLY field; must remain last.
    // ------------------------------------------------------------------

    /// Tag-stamped allocator handle for this plugin.
    ///
    /// The loader stamps the ContextTag corresponding to the registering
    /// plugin's bounded context at glibre_plugin_register time
    /// (perf-budget.md §Allocator Rules #1, plan #989).  Plugin call sites
    /// use this handle exclusively — they do not supply a ContextTag on each
    /// allocation call, eliminating the coupling between plugin code and the
    /// engine's tag enum.
    ///
    /// The handle is a non-owning lightweight wrapper; plugins must not
    /// persist it past the plugin's own lifetime.
    ///
    /// AllocatorHandle is a value type (copy/move-constructible, non-assignable)
    /// that internally holds a PerContextAllocator reference.  Its presence here
    /// makes PluginContext a mixed-storage aggregate (references + value types)
    /// rather than a pure reference aggregate; the POD-like characterisation
    /// in the file-level DESIGN block refers to the absence of a vtable and
    /// user-provided constructors, not to the storage category of each field.
    glibre::AllocatorHandle alloc;
};

// ---------------------------------------------------------------------------
// RegisterFn — loader-side function-pointer type for glibre_plugin_register.
//
// Placed in plugin_api.hpp (not plugin_entry.hpp) so that the engine loader
// can obtain the typedef without pulling in the extern "C" prototype
// declarations for glibre_plugin_register and glibre_plugin_unregister.
// Those prototypes are plugin-author-facing and should not appear in engine
// TUs (loader-side) that resolve the symbol via dlsym.  Including
// plugin_entry.hpp from a loader header would contaminate every engine TU
// that includes the loader with extern "C" plugin symbol declarations —
// a layering violation per LOW-7 (round-1 review).
//
// noexcept is intentionally absent from the function type: since C++17,
// noexcept is part of the function type.  A plugin compiled against a header
// that omits noexcept would produce a different pointer type; omitting it
// here keeps the cast from the raw dlsym void* valid without a noexcept
// mismatch.
//
// Usage (loader side — plan #229):
//   #include <glibre/core/plugin_api.hpp>   // RegisterFn lives here
//   RegisterFn fn{nullptr};
//   std::memcpy(&fn, &sym_register, sizeof(fn));
// ---------------------------------------------------------------------------
using RegisterFn = glibre::Result<void> (*)(glibre::core::PluginContext&);

}  // namespace glibre::core
