#pragma once
// core/include/glibre/core/plugin_loader_actions.hpp
//
// Loader-procedure free functions — steps 9–11 of the plugin loader sequence.
//
// Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" steps 9–11,
//            §"Failure Modes → core::Error" rows 9, 10, 11.
//
// These functions implement the post-gate actions that run after all four
// PluginLoaderRegistry gate checks (steps 4–7) pass.  They are free functions
// rather than PluginLoaderRegistry members because they do not read or mutate
// the registry-of-records state (the loaded_ map and host_engine_version_).
// Keeping them separate respects the SRP boundary that motivated splitting
// PluginLoader (RAII handle + dlopen/dlsym, plan #229) from PluginLoaderRegistry
// (gate validation + records, plan #230) in the first place.
//
// Callers: PluginLoader (plan #229) orchestrates steps 1–11; these free
//          functions are called after validate_all() succeeds and before
//          register_plugin() records the newly loaded plugin.
//
// PHILOSOPHY §11: EASTL replaces std:: for runtime data structures.
// std::expected (Result alias) is retained per §11 exception list.

#include <cstdint>

#include <glibre/core/plugin_api.hpp>  // RegisterFn, PluginContext
#include <glibre/error.hpp>

namespace glibre::core {

// ---------------------------------------------------------------------------
// call_register — loader step 9 (plugin-abi.md §"Loader Sequence").
//
// Invokes the resolved glibre_plugin_register function pointer with the
// provided PluginContext reference.  This step must be called AFTER all four
// validate_* gates have passed.
//
// Preconditions (unchecked; caller must ensure):
//   • validate_all() returned success for this plugin.
//   • The world is drained (phase 8 invariant, frame-phases.md §8).
//   • register_fn is not nullptr; a null pointer indicates a caller logic
//     error that maps to step 2 (missing entry-point), not step 9 (register
//     failure).  A null pointer → core::Error::PluginMissingEntryPoint.
//
// On failure:
//   If register_fn is null → core::Error::PluginMissingEntryPoint
//     (plugin-abi.md §"Failure Modes" step 2 — null/unresolved entry-point).
//   If register_fn returns std::unexpected → core::Error::PluginInitFailed,
//     carrying the inner error code string in ErrorContext::detail
//     (plugin-abi.md §"Failure Modes" step 9).
//
//   The caller is responsible for compensating cleanup on any failure:
//     - dlclose the dylib handle (the loader owns the handle).
//     - Remove any partial registrations the plugin made.
//   This function does NOT call dlclose; it only invokes the entry-point.
//
// @param register_fn  Function pointer resolved from dlsym.
// @param ctx          Engine context passed by reference to the plugin.
// ---------------------------------------------------------------------------

[[nodiscard]] Result<void> call_register(RegisterFn register_fn, PluginContext& ctx) noexcept;

// ---------------------------------------------------------------------------
// rebuild_schedule — loader step 10 (plugin-abi.md §"Loader Sequence").
//
// Recomputes the per-phase system schedule from the union of all loaded
// plugins' declared (reads, writes, after, before) edges.  A cycle →
// core::Error::SystemScheduleCycle; the loader rolls back the last plugin's
// registration and aborts that single load (other plugins keep running).
//
// MVP STUB: Returns success unconditionally.  Real schedule graph topology
// sort from SystemDecl.after / SystemDecl.before edges is deferred to
// frame-loop plans #247 and #248 which define SystemRegistry and the full
// schedule graph builder.
//
// TODO(#247, #248): replace stub with topological sort over loaded plugins'
// SystemDecl vectors; return SystemScheduleCycle on cycle.
// ---------------------------------------------------------------------------

[[nodiscard]] Result<void> rebuild_schedule() noexcept;

// ---------------------------------------------------------------------------
// migrate_components — loader step 11 (plugin-abi.md §"Loader Sequence").
//
// For each persistent component whose schema bumped versions, runs the
// per-type deserialize<T> migration path against the pre-swap snapshot
// (fory-codegen.md §"Migration Mechanic").
//
// MVP STUB: Returns success unconditionally when from_version == to_version
// (no migration needed) and also when from_version != to_version (real
// migration deferred — see TODO below).
//
// On real failure the caller must:
//   1. Call glibre_plugin_unregister if exported (step 9 reverse).
//   2. dlclose the dylib handle.
//   3. Abort the load, leaving prior plugins running.
//
// Error arm: core::Error::SchemaMigrationFailed.
//
// TODO(#221): real per-type migration once glibre-foryc emits
// glibre_plugin_migrations_<TypeName> export tables.  The table format is
// specified in plan #221 (foryc migrations); this stub acknowledges the
// interface contract while deferring the walk implementation.
//
// @param from_version  Component schema version before this load.
// @param to_version    Component schema version this plugin declares.
//
// PROVISIONAL SIGNATURE NOTE: The per-call (from_version, to_version)
// parameter pair is a placeholder for the MVP stub.  When plan #221 lands,
// the signature will likely change to accept a per-type migration table
// handle (e.g. a MigrationChain* or span<MigrationStep>) so that the
// caller passes the glibre_plugin_migrations_<TypeName> table pointer
// directly.  Do not build call sites that depend on the current parameter
// shape surviving unchanged across plan #221.
// ---------------------------------------------------------------------------

[[nodiscard]] Result<void>
migrate_components(std::uint32_t from_version, std::uint32_t to_version) noexcept;

}  // namespace glibre::core
