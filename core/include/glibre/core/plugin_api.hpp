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
#include <expected>

// EASTL substrate — PHILOSOPHY §11 mandates eastl::string_view for engine
// runtime data structures; std::string_view is not permitted in engine code.
#include <EASTL/string_view.h>

#include <glibre/error.hpp>

// ---------------------------------------------------------------------------
// Forward declarations — opaque tags for MVP.
// Real types land in their respective bounded-context plans.
// Plugins that register components must include the owning header
// (e.g. glibre/core/world.hpp when it lands in plan #ECS-world) in addition
// to this header.  Forward declarations here make plugin_api.hpp compilable
// in isolation and free it from transitive ECS / render / editor includes.
// ---------------------------------------------------------------------------

namespace glibre::core {

struct World;           // ECS world — plan: #ECS-world (pending)
struct TypeRegistry;    // component-type registry — plan: #type-registry (pending)
struct SystemRegistry;  // system-schedule registry — plan: #system-registry (pending)
struct PassRegistry;    // render-graph pass registry — plan: #pass-registry (pending)
struct PanelRegistry;   // editor UI panel registry — plan: #panel-registry (pending)
struct LogSink;         // structured spdlog sink — plan: #log-sink (pending)

// PluginManifest is defined in glibre/types/plugin_manifest.hpp (fory-codegen
// plan #223/#225).  For MVP the forward declaration is sufficient to form the
// reference in PluginContext.  The loader passes the deserialized manifest
// back into register() so the plugin can iterate its declared items rather
// than duplicating the list in code (plugin-abi.md §"Registration Entry-Point
// Signature").
struct PluginManifest;  // Fory-generated — plan: #223 / #225 (in-flight)

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
    /// this sink (forwarded by the convenience methods below).
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
    // Convenience registration methods (declarations only — MVP stubs)
    //
    // Implementations live in the loader plan (#229) when the real
    // registry types are defined.  These declarations exist here so
    // plugin code can call them at the correct site; the compiler
    // defers resolution to link time.
    //
    // All return glibre::Result<void> so failure propagates naturally
    // through std::expected without exceptions.
    //
    // Per plugin-abi.md §"Registration Entry-Point Signature": the
    // registries record per-plugin ownership so the loader can run
    // compensating unregister() of any partial registration if
    // glibre_plugin_register() returns an unexpected(err).
    //
    // PHILOSOPHY §11: eastl::string_view (not std::string_view) is used
    // for all name/type-name arguments that cross engine public API
    // boundaries. These are declared with EASTL's string_view so that
    // plugin code pulls in the same EASTL headers as the engine core.
    // ------------------------------------------------------------------

    /// Register a component type by fully-qualified name.
    /// @param type_name  fully-qualified component fqn (e.g. "glibre.render.Mesh")
    /// @param schema_hash  blake3 hex of the component's .fory schema source
    /// @param storage_hint  0=archetype, 1=sparse, 2=singleton
    [[nodiscard]] glibre::Result<void> register_component(
        eastl::string_view type_name,
        eastl::string_view schema_hash,
        std::uint8_t       storage_hint
    ) noexcept;

    /// Register a system into the named phase.
    /// @param name   system identifier (unique within the phase)
    /// @param phase  frame phase ordinal (1..=9 from frame_phase.hpp)
    [[nodiscard]] glibre::Result<void> register_system(
        eastl::string_view type_name,
        std::uint8_t       phase
    ) noexcept;
};

}  // namespace glibre::core
