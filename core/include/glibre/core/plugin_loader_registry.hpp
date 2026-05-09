#pragma once
// core/include/glibre/core/plugin_loader_registry.hpp
//
// PluginLoaderRegistry — tracks loaded plugins and enforces the compatibility
// gates described in reviews/decisions/plugin-abi.md §"Loader Sequence"
// steps 4–7.
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
// Post-gate actions (steps 9–11) live in plugin_loader_actions.hpp as free
// functions.  They are free functions — not methods here — because they do
// not read or mutate this class's registry-of-records state (loaded_ map and
// host_engine_version_).  Mixing registry-of-records state with loader-
// procedure logic would introduce a second reason to change this class (SRP).
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
#include <glibre/core/frame_phase.hpp>
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
    // validate_drain_phase — phase-8 precondition guard (plan #981).
    //
    // Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" step 8:
    //   "at this point the world is already drained (frame-phases §8 guarantee).
    //    The loader is free to mutate type registry, system schedule, pass
    //    registry, panel registry."
    //   reviews/decisions/frame-phases.md §8 — hot-reload is the ONLY phase
    //   during which registry mutations (call_register, rebuild_schedule,
    //   migrate_components) are permitted.
    //
    // SRP boundary: PluginLoaderRegistry does NOT own phase state.  The caller
    // (PluginLoader in the phase-8 frame loop) supplies the current frame phase
    // as a parameter.  Plans #247 and #248 will land the full FramePhaseTracker
    // API; this guard is the minimal injection point they can replace.
    //
    // Returns success when current_phase == Phase::HotReload (phase 8).
    // Returns std::unexpected(core::Error::FramePhaseMisordered) on any other
    // phase — registry mutations outside the hot-reload barrier are forbidden.
    //
    // Call this BEFORE any registry mutation (validate_all, register_plugin,
    // rebuild_schedule, migrate_components) to enforce the frame-phase invariant
    // in debug and release builds alike.
    // -----------------------------------------------------------------------

    [[nodiscard]] static Result<void> validate_drain_phase(Phase current_phase) noexcept;

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
