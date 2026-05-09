#pragma once
// core/include/glibre/core/plugin_loader_registry.hpp
//
// PluginLoaderRegistry — tracks loaded plugins and enforces the four
// compatibility gates described in reviews/decisions/plugin-abi.md
// §"Loader Sequence" steps 4–7.
//
// This class is NOT a singleton; callers own the registry value.  In
// production the engine owns one instance per process.  In tests,
// each TEST_CASE constructs its own independent registry.
//
// Gates enforced (in order):
//   1. ABI hash gate (step 4)
//      manifest.abi_hash AND the exported glibre_plugin_abi_hash symbol
//      must both equal the host's expected ABI hash.  Mismatch →
//      core::Error::PluginAbiHashMismatch.
//   2. Engine version gate (step 5)
//      manifest.min_engine_version ≤ host engine version.  Violation →
//      core::Error::PluginEngineTooOld.
//   3. Name uniqueness gate (step 6)
//      manifest.name must not already be registered.  Duplicate →
//      core::Error::PluginNameCollision.
//   4. Dependency gate (step 7)
//      every entry in manifest.depends_on must already be registered.
//      Missing → core::Error::PluginDependencyMissing.
//
// Out of scope (plan #231):
//   - glibre_plugin_register invocation
//   - schedule rebuild
//   - migrate / hot-reload
//
// PHILOSOPHY §11: EASTL replaces std:: for runtime data structures.
// std:: is retained for std::expected (Result alias), std::string_view
// (passed through to EASTL), std::filesystem.
//
// PHILOSOPHY §9: "Plugin ABI gated by middleman dylib hash."

#include <cstdint>
#include <string_view>

#include <EASTL/string.h>
#include <EASTL/string_view.h>
#include <EASTL/vector.h>
#include <EASTL/hash_map.h>

#include <glibre/error.hpp>
#include <glibre/core/plugin_manifest.hpp>

namespace glibre::core {

// ---------------------------------------------------------------------------
// PluginRecord — immutable descriptor stored per registered plugin.
//
// Stored in the registry after a plugin passes all gates.  The record is
// used by subsequent loads for name-collision and dependency checks.
// ---------------------------------------------------------------------------

struct PluginRecord {
    eastl::string name;      // manifest.name  — must be unique
    SemVer        version;   // manifest.version
    eastl::string path;      // filesystem path to the .dylib (may be empty in tests)
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
    // validate_abi_hash — run gate 1 (ABI hash) only.
    //
    // Compares manifest.abi_hash against expected_abi_hash.  The caller
    // supplies the expected_abi_hash that was read from the host's compiled-in
    // `glibre_types_abi_hash()` AND from the plugin's exported
    // `glibre_plugin_abi_hash` symbol.  Both values must have been checked
    // externally before calling this helper (the loader calls it twice — once
    // against the symbol, once against the manifest field — and this method
    // checks the manifest field only; the caller checks the symbol equality
    // separately).
    //
    // Per plugin-abi.md §"Loader Sequence" step 4:
    //   "Both must agree, both must equal the host's glibre_types_abi_hash()."
    //
    // On mismatch returns std::unexpected(core::Error::PluginAbiHashMismatch).
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void> validate_abi_hash(
        const PluginManifest& manifest,
        eastl::string_view    expected_abi_hash) const noexcept;

    // -----------------------------------------------------------------------
    // validate_engine_version — run gate 2 (engine version).
    //
    // Checks manifest.min_engine_version ≤ host engine version.
    // On violation returns core::Error::PluginEngineTooOld.
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void> validate_engine_version(
        const PluginManifest& manifest) const noexcept;

    // -----------------------------------------------------------------------
    // validate_name_unique — run gate 3 (name uniqueness).
    //
    // Checks manifest.name is not already registered.
    // On duplicate returns core::Error::PluginNameCollision.
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void> validate_name_unique(
        const PluginManifest& manifest) const noexcept;

    // -----------------------------------------------------------------------
    // validate_dependencies — run gate 4 (dependency resolution).
    //
    // Checks every entry in manifest.depends_on is already registered.
    // On the first missing dependency returns core::Error::PluginDependencyMissing.
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void> validate_dependencies(
        const PluginManifest& manifest) const noexcept;

    // -----------------------------------------------------------------------
    // validate_all — run all four gates in order (steps 4–7).
    //
    // The symbol_abi_hash argument is the value read from the plugin's
    // exported `glibre_plugin_abi_hash` C symbol (a const char* that the
    // loader dlsym()s).  The manifest.abi_hash field is also checked; both
    // must equal expected_abi_hash per plugin-abi.md §step 4.
    //
    // Returns the first error encountered; gates are run in plugin-abi.md
    // ordering (hash → engine version → name → deps).
    // -----------------------------------------------------------------------

    [[nodiscard]] Result<void> validate_all(
        const PluginManifest& manifest,
        eastl::string_view    expected_abi_hash,
        eastl::string_view    symbol_abi_hash) const noexcept;

    // -----------------------------------------------------------------------
    // register_plugin — record a plugin as successfully loaded.
    //
    // Call this AFTER validate_all succeeds and AFTER glibre_plugin_register
    // (plan #231) completes without error.  The registry then makes the
    // plugin's name visible to subsequent dependency checks.
    //
    // path is the filesystem path to the .dylib; may be empty in tests.
    //
    // Precondition: validate_all was called with the same manifest and
    // returned success.  Violating this precondition produces undefined
    // behaviour; debug builds abort.
    // -----------------------------------------------------------------------

    void register_plugin(const PluginManifest& manifest,
                         eastl::string_view    path) noexcept;

    // -----------------------------------------------------------------------
    // is_registered — query whether a plugin name is already registered.
    // -----------------------------------------------------------------------

    [[nodiscard]] bool is_registered(eastl::string_view name) const noexcept;

    // -----------------------------------------------------------------------
    // loaded_count — number of successfully registered plugins.
    // -----------------------------------------------------------------------

    [[nodiscard]] std::size_t loaded_count() const noexcept;

private:
    SemVer host_engine_version_;

    // key = plugin name (eastl::string), value = PluginRecord
    // eastl::hash_map per PHILOSOPHY §11 (EASTL containers).
    eastl::hash_map<eastl::string, PluginRecord> loaded_;
};

}  // namespace glibre::core
