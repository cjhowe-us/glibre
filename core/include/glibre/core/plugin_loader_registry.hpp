#pragma once
// core/include/glibre/core/plugin_loader_registry.hpp
//
// PluginLoaderRegistry — tracks loaded plugins and enforces the compatibility
// gates described in reviews/decisions/plugin-abi.md §"Loader Sequence"
// steps 4–11 (gate validation in steps 4–7; post-gate actions in 8–11).
//
// This class is NOT a singleton; callers own the registry value.  In
// production the engine owns one instance per process.  In tests,
// each TEST_CASE constructs its own independent registry.
//
// Gates enforced (in order, steps 4–7):
//   1. ABI hash gate (step 4) — two sub-checks in plugin-abi.md §step 4 order:
//      a) validate_manifest_abi_hash: manifest.abi_hash == expected_abi_hash.
//      b) validate_symbol_abi_hash:   symbol value == expected_abi_hash.
//      Both must pass; either mismatch → core::Error::PluginAbiHashMismatch.
//      validate_all runs (a) then (b) per plugin-abi.md §step 4 text.
//      Individual granular callers may invoke each sub-check directly.
//   2. Engine version gate (step 5)
//      manifest.min_engine_version ≤ host engine version.  Violation →
//      core::Error::PluginEngineTooOld.
//   3. Name uniqueness gate (step 6)
//      manifest.name must not already be registered WITH A DIFFERENT file
//      path.  Same name + same path is idempotent (allows hot-reload
//      re-registration of the same dylib).  Different path →
//      core::Error::PluginNameCollision.
//   4. Dependency gate (step 7)
//      every entry in manifest.depends_on must already be registered.
//      Missing → core::Error::PluginDependencyMissing.
//
// Post-gate actions (steps 9–11) — added by plan #231:
//   call_register   — invokes glibre_plugin_register(&ctx) after gates pass.
//                     On failure → core::Error::PluginInitFailed; caller
//                     is responsible for compensating cleanup (dlclose).
//   rebuild_schedule — recomputes the per-phase system schedule topology.
//                     MVP stub returns success unconditionally.
//                     TODO(#247, #248): real schedule graph rebuild.
//   migrate_components — walks per-type migration tables (plan #221 format).
//                     MVP stub returns success unconditionally.
//                     TODO(#221): real archetype migration once data-context
//                     migration tables land.
//
// PHILOSOPHY §11: EASTL replaces std:: for runtime data structures.
// std:: is retained for std::expected (Result alias) and std::filesystem.
//
// PHILOSOPHY §9: "Plugin ABI gated by middleman dylib hash."

#include <cstdint>

#include <EASTL/functional.h>
#include <EASTL/hash_map.h>
#include <EASTL/string.h>
#include <EASTL/string_view.h>
#include <EASTL/vector.h>
#include <glibre/core/plugin_entry.hpp>   // RegisterFn
#include <glibre/core/plugin_manifest.hpp>
#include <glibre/error.hpp>

namespace glibre::core {

// ---------------------------------------------------------------------------
// PluginRecord — immutable descriptor stored per registered plugin.
//
// Stored in the registry after a plugin passes all gates.  The record is
// used by subsequent loads for name-collision and dependency checks.
// ---------------------------------------------------------------------------

struct PluginRecord {
    eastl::string name;  // manifest.name  — must be unique
    SemVer version;      // manifest.version
    eastl::string path;  // filesystem path to the .dylib (may be empty in tests)
};

// ---------------------------------------------------------------------------
// PluginLoaderRegistry — owns the loaded-plugin table and validates gates.
//
// Thread safety: None.  Mutations must happen during phase 8 (HotReload)
// when the world is already drained (frame-phases.md §8).  No external
// locking is provided; the engine's frame-phase barrier is the only guard.
// ---------------------------------------------------------------------------

class PluginLoaderRegistry {
public:
    // -----------------------------------------------------------------------
    // PluginLoaderRegistry() — default-constructed, empty registry.
    //
    // The host engine version is injected at construction time so that it
    // participates in gate checks without coupling the registry to a global
    // constant.  Tests supply a synthetic version; production supplies the
    // real `glibre_core_version` value.
    // -----------------------------------------------------------------------

    explicit PluginLoaderRegistry(SemVer host_engine_version) noexcept;

    // -----------------------------------------------------------------------
    // validate_manifest_abi_hash — gate 1a (manifest field only).
    //
    // Compares manifest.abi_hash against expected_abi_hash.
    // Per plugin-abi.md §"Loader Sequence" step 4, the manifest field and
    // the exported symbol must both agree with the host's hash.  This
    // method checks the manifest field; validate_symbol_abi_hash checks
    // the compiled-in symbol value.
    //
    // On mismatch returns std::unexpected(core::Error::PluginAbiHashMismatch).
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void> validate_manifest_abi_hash(
        const PluginManifest& manifest, eastl::string_view expected_abi_hash
    ) const noexcept;

    // -----------------------------------------------------------------------
    // validate_symbol_abi_hash — gate 1b (exported symbol value only).
    //
    // Compares the value the loader read from the plugin's exported
    // `glibre_plugin_abi_hash` C symbol against expected_abi_hash.
    // The redundant check (step 4) catches a malformed manifest whose
    // abi_hash field disagrees with the compiled-in symbol.
    //
    // On mismatch returns std::unexpected(core::Error::PluginAbiHashMismatch).
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void> validate_symbol_abi_hash(
        eastl::string_view symbol_abi_hash, eastl::string_view expected_abi_hash
    ) const noexcept;

    // -----------------------------------------------------------------------
    // validate_engine_version — run gate 2 (engine version).
    //
    // Checks manifest.min_engine_version ≤ host engine version.
    // On violation returns core::Error::PluginEngineTooOld.
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void>
    validate_engine_version(const PluginManifest& manifest) const noexcept;

    // -----------------------------------------------------------------------
    // validate_name_unique — run gate 3 (name uniqueness).
    //
    // Per plugin-abi.md §"Loader Sequence" step 6:
    //   "refuse if a plugin with the same name is already registered with
    //    a different file path."
    //
    // Collision is fired only when name matches AND file_path differs.
    // Same name + same file_path is treated as an idempotent re-registration
    // (the hot-reload path in plan #231 loads the same .dylib again after a
    // swap; the registry must accept it without error).
    //
    // On collision returns core::Error::PluginNameCollision.
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void> validate_name_unique(
        const PluginManifest& manifest, eastl::string_view file_path
    ) const noexcept;

    // -----------------------------------------------------------------------
    // validate_dependencies — run gate 4 (dependency resolution).
    //
    // Checks every entry in manifest.depends_on is already registered.
    // On the first missing dependency returns core::Error::PluginDependencyMissing.
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void> validate_dependencies(const PluginManifest& manifest) const noexcept;

    // -----------------------------------------------------------------------
    // validate_all — run all four gates in order (steps 4–7).
    //
    // symbol_abi_hash: value read from the plugin's exported
    //   `glibre_plugin_abi_hash` C symbol (a const char* that the loader
    //   dlsym()s).  The manifest.abi_hash field is also checked; both
    //   must equal expected_abi_hash per plugin-abi.md §step 4.
    //
    // file_path: the .dylib path being loaded, forwarded to
    //   validate_name_unique for the "different file path" qualifier
    //   (plugin-abi.md §step 6).
    //
    // Returns the first error encountered; gates are run in plugin-abi.md
    // ordering (hash → engine version → name → deps).
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void> validate_all(
        const PluginManifest& manifest,
        eastl::string_view expected_abi_hash,
        eastl::string_view symbol_abi_hash,
        eastl::string_view file_path
    ) const noexcept;

    // -----------------------------------------------------------------------
    // register_plugin — record a plugin as successfully loaded.
    //
    // Call this AFTER validate_all succeeds and AFTER glibre_plugin_register
    // (plan #231) completes without error.  The registry then makes the
    // plugin's name visible to subsequent dependency checks.
    //
    // path is the filesystem path to the .dylib; may be empty in tests.
    //
    // Returns an error (without modifying the registry) if the name is
    // already registered with a different path.  This is an unconditional
    // precondition check — not a debug-only assert — so release builds are
    // protected from silent overwrites.
    //
    // Same name + same path: idempotent no-op, returns success.
    // Same name + different path: returns core::Error::PluginNameCollision.
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void>
    register_plugin(const PluginManifest& manifest, eastl::string_view path) noexcept;

    // -----------------------------------------------------------------------
    // is_registered — query whether a plugin name is already registered.
    // -----------------------------------------------------------------------

    [[nodiscard]] bool is_registered(eastl::string_view name) const noexcept;

    // -----------------------------------------------------------------------
    // loaded_count — number of successfully registered plugins.
    // -----------------------------------------------------------------------

    [[nodiscard]] std::size_t loaded_count() const noexcept;

    // -----------------------------------------------------------------------
    // call_register — loader step 9 (plugin-abi.md §"Loader Sequence").
    //
    // Invokes the resolved glibre_plugin_register function pointer with the
    // provided PluginContext reference.  This step must be called AFTER all
    // four validate_* gates have passed.
    //
    // Preconditions (unchecked; caller must ensure):
    //   • validate_all() returned success for this plugin.
    //   • The world is drained (phase 8 invariant, frame-phases.md §8).
    //   • register_fn is not nullptr.
    //
    // On failure:
    //   Returns core::Error::PluginInitFailed, carrying the inner error code
    //   returned by glibre_plugin_register in ErrorContext::detail as a
    //   best-effort string (plugin-abi.md §"Failure Modes" step 9).
    //   The caller is responsible for compensating cleanup:
    //     - dlclose the dylib handle (the loader owns the handle).
    //     - Remove any partial registrations the plugin made.
    //   This method does NOT call dlclose; it only invokes the entry-point.
    //
    // @param register_fn  Function pointer resolved from dlsym.
    // @param ctx          Engine context passed by reference to the plugin.
    // -----------------------------------------------------------------------

    [[nodiscard]] static Result<void>
    call_register(RegisterFn register_fn, PluginContext& ctx) noexcept;

    // -----------------------------------------------------------------------
    // rebuild_schedule — loader step 10 (plugin-abi.md §"Loader Sequence").
    //
    // Recomputes the per-phase system schedule from the union of all loaded
    // plugins' declared (reads, writes, after, before) edges.  A cycle →
    // core::Error::SystemScheduleCycle; the loader rolls back the last
    // plugin's registration and aborts that single load (other plugins keep
    // running).
    //
    // MVP STUB: Returns success unconditionally.  Real schedule graph
    // topology sort from SystemDecl.after / SystemDecl.before edges is
    // deferred to frame-loop plans #247 and #248 which define SystemRegistry
    // and the full schedule graph builder.
    //
    // TODO(#247, #248): replace stub with topological sort over loaded
    // plugins' SystemDecl vectors; return SystemScheduleCycle on cycle.
    //
    // Per plugin-abi.md §"Open Questions" point 1, per-plugin ownership
    // records for rollback may use an eastl::vector<eastl::string>; revisit
    // at the hot-reload story.  The MVP stub does not yet record ownership.
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void> rebuild_schedule() const noexcept;

    // -----------------------------------------------------------------------
    // migrate_components — loader step 11 (plugin-abi.md §"Loader Sequence").
    //
    // For each persistent component whose schema bumped versions, runs the
    // per-type deserialize<T> migration path against the pre-swap snapshot
    // (fory-codegen.md §"Migration Mechanic").
    //
    // MVP STUB: Returns success unconditionally when from_version ==
    // to_version (no migration needed) and also when from_version !=
    // to_version (real migration deferred — see TODO below).
    //
    // On real failure the caller must:
    //   1. Call glibre_plugin_unregister if exported (step 9 reverse).
    //   2. dlclose the dylib handle.
    //   3. Abort the load, leaving prior plugins running.
    //
    // Error arm: core::Error::SchemaMigrationFailed.
    //
    // TODO(#221): real per-type migration once glibre-foryc emits
    // glibre_plugin_migrations_<TypeName> export tables.  The table format
    // is specified in plan #221 (foryc migrations); this stub acknowledges
    // the interface contract while deferring the walk implementation.
    //
    // @param from_version  Component schema version before this load.
    // @param to_version    Component schema version this plugin declares.
    // -----------------------------------------------------------------------

    [[nodiscard]] static Result<void>
    migrate_components(std::uint32_t from_version, std::uint32_t to_version) noexcept;

private:
    // -----------------------------------------------------------------------
    // is_collision — name/path collision predicate (single source of truth).
    //
    // Returns true when a plugin with the same name is already registered
    // with a DIFFERENT file path — the condition that fires PluginNameCollision.
    // Same name + same path (hot-reload idempotent path): returns false.
    // Name not present: returns false.
    //
    // Used by both validate_name_unique and register_plugin so the predicate
    // has one definition (SOLID SRP: one reason to change).
    // -----------------------------------------------------------------------

    [[nodiscard]] bool
    is_collision(eastl::string_view name, eastl::string_view file_path) const noexcept;

    SemVer host_engine_version_;

    // key = plugin name (eastl::string), value = PluginRecord
    // eastl::hash_map per PHILOSOPHY §11 (EASTL containers).
    //
    // transparent_string_hash + equal_to<void> enable heterogeneous lookup:
    //   loaded_.find(eastl::string_view{...})  — no eastl::string allocation
    //   per lookup (MED-3: avoids per-call key materialisation).
    eastl::
        hash_map<eastl::string, PluginRecord, eastl::transparent_string_hash, eastl::equal_to<void>>
            loaded_;
};

}  // namespace glibre::core
