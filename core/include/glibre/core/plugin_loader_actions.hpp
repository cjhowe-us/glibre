#pragma once
// core/include/glibre/core/plugin_loader_actions.hpp
//
// Loader-procedure free functions — two axes of stateless loader-sequence work:
//
//   Axis 1 — Initial-load post-gate actions (steps 9–11 of the plugin loader
//             sequence).  These run ONCE when a plugin is first loaded, after
//             all four PluginLoaderRegistry gate checks (steps 4–7) pass.
//             Authority: reviews/decisions/plugin-abi.md §"Loader Sequence"
//             steps 9–11 and §"Failure Modes → core::Error" rows 9, 10, 11.
//
//   Axis 2 — Hot-reload pre-swap validation (hot-reload phase-8 step 2, plan
//             #250).  This check runs EVERY reload cycle, after the drain step
//             and before the vtable swap.  It inspects two loaded-plugin
//             manifests and returns an error if the swap should be refused.
//             Authority: reviews/decisions/hot-reload-protocol.md §"Step 2 —
//             Swap" sub-steps 2.1 and 2.2.
//
// SRP note: the two axes share this file because both expose stateless free
// functions that do not read or mutate PluginLoaderRegistry state (the loaded_
// map and host_engine_version_).  That "no-registry-state" boundary is the
// single responsibility this file enforces.  The test directory split
// (tests/core/plugin_loader/ for Axis 1, tests/core/hot_reload/ for Axis 2)
// signals that the axes MAY be separated into parallel header/source pairs once
// plans #251+ add type-superset checks and migration-arena work that would
// further grow Axis 2's responsibility surface.
//
// TODO(#251): evaluate splitting hot_reload_validate (and its successors from
// plans #251+) into core/include/glibre/core/hot_reload_actions.hpp + .cpp to
// match the test directory structure and keep each file on a single axis.
//
// Callers: PluginLoader (plan #229) orchestrates steps 1–11; the Axis 1
//          functions are called after validate_all() succeeds and before
//          register_plugin() records the newly loaded plugin.
//
// PHILOSOPHY §11: EASTL replaces std:: for runtime data structures.
// std::expected (Result alias) is retained per §11 exception list.

#include <cstdint>

#include <glibre/core/plugin_api.hpp>       // RegisterFn, PluginContext
#include <glibre/core/plugin_manifest.hpp>  // PluginManifest, SemVer
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

// ---------------------------------------------------------------------------
// hot_reload_validate — pre-swap compatibility check (plan #250).
//
// Authority: reviews/decisions/hot-reload-protocol.md §"Step 2 — Swap",
//            sub-steps 2.1 and 2.2.
//
// Validates that an incoming plugin candidate (`incoming`) is a safe swap
// for the currently-loaded plugin (`outgoing`).  This check runs during
// phase 8 after the drain step and before the vtable swap (step 2.3).
//
// Three sequential checks, performed in the order below:
//
//   Check A — Name identity (plan #250 configuration-error guard; no parent
//             step in hot-reload-protocol.md or plugin-abi.md):
//     incoming.name == outgoing.name.  Swapping for a plugin with a different
//     name is a configuration error — the operator loaded the wrong dylib.
//     Failure → core::Error::PluginNameMismatch.
//
//   Check B — ABI hash (hot-reload-protocol §2.1):
//     incoming.abi_hash == outgoing.abi_hash.
//
//     Protocol §2.1 specifies the check as `incoming == host_glibre_types_abi_hash`
//     (incoming-vs-host).  Here we compare incoming-vs-outgoing instead.  This
//     is semantically equivalent under the following precondition:
//
//       PRECONDITION: PluginLoaderRegistry::validate_all() confirmed that BOTH
//       manifests equal the host hash before the loader ever called
//       hot_reload_validate.  If that precondition holds, then:
//         incoming.abi_hash == host   (guaranteed by validate_all)
//         outgoing.abi_hash == host   (guaranteed by validate_all)
//         → incoming.abi_hash == outgoing.abi_hash   (transitively)
//
//     Comparing the two manifests against each other rather than against the
//     host hash surfaces one additional failure mode: a bug where the two
//     loaded manifests disagree with each other despite both nominally passing
//     validate_all (e.g. a race that replaced outgoing's manifest between
//     validate_all and this call).  In practice that race cannot occur because
//     the loader holds the exclusive phase-8 lock, but the belt-and-suspenders
//     check is cheap and the error message is more specific.
//
//     The PRECONDITION is asserted in debug builds via #ifndef NDEBUG /
//     assert() inside hot_reload_validate (see plugin_loader_actions.cpp).
//     Call sites must not invoke this function without first calling
//     validate_all.
//
//     Failure → core::Error::PluginAbiHashMismatch.
//
//   Check C — SemVer major version (plan #250 extension of protocol §2.2):
//     The incoming plugin's semantic version major must equal the outgoing's.
//     The minor/patch may advance (additive changes are safe); they may also
//     stay the same (a patch rebuild).  A major-version change signals a
//     breaking redesign that requires a fresh world, not a hot-reload.
//
//     DIVERGENCE NOTE: hot-reload-protocol §2.2 defines a component-type-set
//     superset check on (fqn, schema_version) — it does NOT define a discrete
//     SemVer-major equality check.  The phrase "major-version change" in the
//     protocol is descriptive language about *why* the superset check fails
//     when a plugin drops a registered type, not a separate criterion.
//
//     Plan #250 introduces SemVer-major equality as an additional,
//     manifest-only precondition that is cheaper to evaluate than the full
//     superset check (which requires archetype storage — deferred to plans
//     #251+).  It is a plan-level extension that supersedes, but does not
//     contradict, protocol §2.2: the superset check will still run in plans
//     #251+ and the major-version check here is a fast-fail gate that catches
//     the most obvious incompatible swap before touching ECS state.
//
//     Failure → core::Error::HotReloadRefused.
//
// Note on type-superset check (protocol §2.2 second bullet):
//   The protocol requires that incoming's component set is a superset-or-equal
//   of outgoing's surviving component storages.  That check depends on the
//   archetype storage and migration arena (plans #251+); it is NOT performed
//   here.  This function's scope is the manifest-only checks that can run
//   without touching ECS state.
//
// Preconditions (unchecked — caller must ensure):
//   • outgoing and incoming are valid PluginManifest values previously
//     extracted from PluginLoader instances whose validate_all() returned
//     success.
//   • This function is called during phase 8, after drain.
//
// @param outgoing  Currently-loaded plugin's manifest.
// @param incoming  Candidate replacement plugin's manifest.
//
// Returns:
//   success (Result<void>{})                     — all checks passed; swap may proceed.
//   core::Error::PluginNameMismatch              — check A failed.
//   core::Error::PluginAbiHashMismatch           — check B failed.
//   core::Error::HotReloadRefused                — check C failed (major version change).
// ---------------------------------------------------------------------------

[[nodiscard]] Result<void>
hot_reload_validate(const PluginManifest& outgoing, const PluginManifest& incoming) noexcept;

}  // namespace glibre::core
