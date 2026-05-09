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
//   • ABI boundary string rule: The convenience methods (register_component,
//     register_system) accept string arguments as a (const char*, std::size_t)
//     pair rather than eastl::string_view.  EASTL has no formal ABI stability
//     guarantee; exposing eastl::string_view across the plugin boundary would
//     break plugins compiled against a different EASTL version.  The engine-
//     side implementation converts to eastl::string_view internally.
//     (PHILOSOPHY §11: "POD spans / handles only" across the ABI surface.)
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

namespace glibre::core {

class World;           // ECS world — plan: #ECS-world (pending)
class TypeRegistry;    // component-type registry — plan: #type-registry (pending)
class SystemRegistry;  // system-schedule registry — plan: #system-registry (pending)
class PassRegistry;    // render-graph pass registry — plan: #pass-registry (pending)
class PanelRegistry;   // editor UI panel registry — plan: #panel-registry (pending)
class LogSink;         // structured spdlog sink — plan: #log-sink (pending)

// PluginManifest is defined in glibre/types/plugin_manifest.hpp (fory-codegen
// plan #223/#225).  For MVP the forward declaration is sufficient to form the
// reference in PluginContext.  The loader passes the deserialized manifest
// back into register() so the plugin can iterate its declared items rather
// than duplicating the list in code (plugin-abi.md §"Registration Entry-Point
// Signature").
class PluginManifest;  // Fory-generated — plan: #223 / #225 (in-flight)

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
    // registry types are fully defined.  These declarations exist here so
    // plugin code can call them at the correct site; the compiler defers
    // resolution to link time.
    //
    // All return glibre::Result<void> so failure propagates naturally
    // through std::expected without exceptions.
    //
    // Per plugin-abi.md §"Registration Entry-Point Signature": the
    // registries record per-plugin ownership so the loader can run
    // compensating unregister() of any partial registration if
    // glibre_plugin_register() returns an unexpected(err).
    //
    // ABI boundary: string arguments cross as (const char*, std::size_t)
    // pairs — no EASTL or std:: string types on the boundary surface.
    // The engine-side impl (plan #229) converts to eastl::string_view
    // internally.  Plugins that already hold an eastl::string_view can
    // pass .data() and .size().
    // ------------------------------------------------------------------

    /// Register a component type by fully-qualified name.
    /// @param type_name      pointer to the component fqn UTF-8 bytes
    /// @param type_name_len  byte length of type_name (no NUL terminator needed)
    /// @param schema_hash      pointer to the blake3 hex string bytes
    /// @param schema_hash_len  byte length of schema_hash
    /// @param storage_hint  0=archetype, 1=sparse, 2=singleton
    [[nodiscard]] glibre::Result<void> register_component(
        const char*  type_name,
        std::size_t  type_name_len,
        const char*  schema_hash,
        std::size_t  schema_hash_len,
        std::uint8_t storage_hint
    ) noexcept;

    /// Register a system into the named phase.
    /// @param name      pointer to the system name UTF-8 bytes
    /// @param name_len  byte length of name (no NUL terminator needed)
    /// @param phase     frame phase ordinal (1..=9 from frame_phase.hpp)
    [[nodiscard]] glibre::Result<void> register_system(
        const char*  name,
        std::size_t  name_len,
        std::uint8_t phase
    ) noexcept;
};

}  // namespace glibre::core
