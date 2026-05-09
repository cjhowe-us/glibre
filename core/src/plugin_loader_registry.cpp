// core/src/plugin_loader_registry.cpp
//
// PluginLoaderRegistry — gates 4–7 of the plugin loader sequence.
//
// Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" steps 4–7
//            and §"Failure Modes → core::Error".
//
// Plan: #230 (ABI hash + version + name + deps gates).
// Out of scope: dlopen/dlsym (plan #229), register + rebuild + migrate (#231).

#include "glibre/core/plugin_loader_registry.hpp"

#include <cstddef>

namespace glibre::core {

// ---------------------------------------------------------------------------
// PluginLoaderRegistry — constructor
// ---------------------------------------------------------------------------

PluginLoaderRegistry::PluginLoaderRegistry(SemVer host_engine_version) noexcept
    : host_engine_version_{host_engine_version} {}

// ---------------------------------------------------------------------------
// validate_manifest_abi_hash — gate 1a (plugin-abi.md step 4, manifest field)
//
// manifest.abi_hash must equal expected_abi_hash.
// ---------------------------------------------------------------------------

Result<void> PluginLoaderRegistry::validate_manifest_abi_hash(
    const PluginManifest& manifest, eastl::string_view expected_abi_hash
) const noexcept {

    if (manifest.abi_hash != expected_abi_hash) {
        return std::unexpected(glibre::Error{core::Error::PluginAbiHashMismatch});
    }
    return {};
}

// ---------------------------------------------------------------------------
// validate_symbol_abi_hash — gate 1b (plugin-abi.md step 4, exported symbol)
//
// The value from the plugin's exported glibre_plugin_abi_hash C symbol must
// equal expected_abi_hash.  The redundant check catches a malformed manifest
// whose abi_hash field disagrees with the compiled-in symbol.
// ---------------------------------------------------------------------------

Result<void> PluginLoaderRegistry::validate_symbol_abi_hash(
    eastl::string_view symbol_abi_hash, eastl::string_view expected_abi_hash
) const noexcept {

    if (symbol_abi_hash != expected_abi_hash) {
        return std::unexpected(glibre::Error{core::Error::PluginAbiHashMismatch});
    }
    return {};
}

// ---------------------------------------------------------------------------
// validate_engine_version — gate 2 (plugin-abi.md step 5)
//
// manifest.min_engine_version ≤ host engine version.
// The SemVer operator<= is defined in plugin_manifest.hpp.
// ---------------------------------------------------------------------------

Result<void>
PluginLoaderRegistry::validate_engine_version(const PluginManifest& manifest) const noexcept {

    // Refuse if the host is older than the plugin's minimum requirement.
    if (host_engine_version_ < manifest.min_engine_version) {
        return std::unexpected(glibre::Error{core::Error::PluginEngineTooOld});
    }
    return {};
}

// ---------------------------------------------------------------------------
// validate_name_unique — gate 3 (plugin-abi.md step 6)
//
// Collision fires only when name matches AND file_path differs.
// Same name + same path: idempotent re-registration (hot-reload re-load path).
// Same name + different path: PluginNameCollision.
// ---------------------------------------------------------------------------

Result<void> PluginLoaderRegistry::validate_name_unique(
    const PluginManifest& manifest, eastl::string_view file_path
) const noexcept {

    if (is_collision(eastl::string_view{manifest.name.c_str()}, file_path)) {
        return std::unexpected(glibre::Error{core::Error::PluginNameCollision});
    }
    // Name absent, or same name + same path → idempotent; not an error.
    return {};
}

// ---------------------------------------------------------------------------
// validate_dependencies — gate 4 (plugin-abi.md step 7)
//
// Every entry in manifest.depends_on must already be registered.
// Returns on the first missing dependency.
// ---------------------------------------------------------------------------

Result<void>
PluginLoaderRegistry::validate_dependencies(const PluginManifest& manifest) const noexcept {

    for (const eastl::string& dep : manifest.depends_on) {
        // Heterogeneous lookup: eastl::string_view avoids materialising a
        // temporary eastl::string key per call (transparent_string_hash).
        if (loaded_.find(eastl::string_view{dep}) == loaded_.end()) {
            return std::unexpected(glibre::Error{core::Error::PluginDependencyMissing});
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// is_collision — name/path collision predicate (MED-4: single source of truth).
//
// Returns true when a plugin with the same name is already registered with a
// DIFFERENT file path — the condition that must fire PluginNameCollision.
// Same name + same path is idempotent (hot-reload re-registration); returns
// false.  Name not present: returns false.
// ---------------------------------------------------------------------------

bool PluginLoaderRegistry::is_collision(
    eastl::string_view name, eastl::string_view file_path
) const noexcept {
    const auto it = loaded_.find(name);
    if (it == loaded_.end()) {
        return false;
    }
    return it->second.path != file_path;
}

// ---------------------------------------------------------------------------
// validate_all — run gates 1–4 in order.
//
// Both manifest.abi_hash and symbol_abi_hash must equal expected_abi_hash
// per plugin-abi.md §step 4 ("the redundant check catches a malformed
// manifest whose abi_hash field disagrees with the compiled-in symbol").
//
// file_path is forwarded to validate_name_unique for the "different file
// path" qualifier of step 6.
// ---------------------------------------------------------------------------

Result<void> PluginLoaderRegistry::validate_all(
    const PluginManifest& manifest,
    eastl::string_view expected_abi_hash,
    eastl::string_view symbol_abi_hash,
    eastl::string_view file_path
) const noexcept {

    // Gate 1a: manifest-side ABI hash check (plugin-abi.md §step 4, leading check).
    if (auto r = validate_manifest_abi_hash(manifest, expected_abi_hash); !r) {
        return r;
    }

    // Gate 1b: symbol-side ABI hash check (redundant; catches malformed manifest).
    if (auto r = validate_symbol_abi_hash(symbol_abi_hash, expected_abi_hash); !r) {
        return r;
    }

    // Gate 2: engine version.
    if (auto r = validate_engine_version(manifest); !r) {
        return r;
    }

    // Gate 3: name uniqueness (with file-path qualifier).
    if (auto r = validate_name_unique(manifest, file_path); !r) {
        return r;
    }

    // Gate 4: dependency resolution.
    return validate_dependencies(manifest);
}

// ---------------------------------------------------------------------------
// register_plugin — record a successfully validated plugin.
//
// Returns an error (without mutating the registry) if name is already
// registered with a different path.  Same name + same path is an idempotent
// no-op (returns success without re-inserting).
// ---------------------------------------------------------------------------

Result<void> PluginLoaderRegistry::register_plugin(
    const PluginManifest& manifest, eastl::string_view path
) noexcept {

    // Unconditional precondition check — protects release builds from
    // silent overwrites (replaces the former debug-only assert).
    if (is_collision(eastl::string_view{manifest.name.c_str()}, path)) {
        return std::unexpected(glibre::Error{core::Error::PluginNameCollision});
    }

    const auto it = loaded_.find(manifest.name);
    if (it != loaded_.end()) {
        // Same name + same path: idempotent re-registration, no-op.
        return {};
    }

    PluginRecord rec;
    rec.name = manifest.name;
    rec.version = manifest.version;
    rec.path = eastl::string{path.data(), path.size()};

    loaded_.emplace(manifest.name, eastl::move(rec));
    return {};
}

// ---------------------------------------------------------------------------
// is_registered
// ---------------------------------------------------------------------------

bool PluginLoaderRegistry::is_registered(eastl::string_view name) const noexcept {
    // Heterogeneous lookup: no eastl::string allocation per call.
    return loaded_.find(name) != loaded_.end();
}

// ---------------------------------------------------------------------------
// loaded_count
// ---------------------------------------------------------------------------

std::size_t PluginLoaderRegistry::loaded_count() const noexcept { return loaded_.size(); }

}  // namespace glibre::core
