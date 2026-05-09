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

#include <cassert>
#include <cstddef>

#include <EASTL/algorithm.h>

namespace glibre::core {

// ---------------------------------------------------------------------------
// PluginLoaderRegistry — constructor
// ---------------------------------------------------------------------------

PluginLoaderRegistry::PluginLoaderRegistry(SemVer host_engine_version) noexcept
    : host_engine_version_{host_engine_version} {}

// ---------------------------------------------------------------------------
// validate_abi_hash — gate 1 (plugin-abi.md step 4, manifest field only)
//
// The manifest field manifest.abi_hash must equal expected_abi_hash.
// The caller also checks the exported symbol value separately; this method
// covers only the manifest field side of the redundant check.
// ---------------------------------------------------------------------------

Result<void> PluginLoaderRegistry::validate_abi_hash(
    const PluginManifest& manifest,
    eastl::string_view    expected_abi_hash) const noexcept {

    if (manifest.abi_hash != expected_abi_hash) {
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

Result<void> PluginLoaderRegistry::validate_engine_version(
    const PluginManifest& manifest) const noexcept {

    // Refuse if the host is older than the plugin's minimum requirement.
    if (host_engine_version_ < manifest.min_engine_version) {
        return std::unexpected(glibre::Error{core::Error::PluginEngineTooOld});
    }
    return {};
}

// ---------------------------------------------------------------------------
// validate_name_unique — gate 3 (plugin-abi.md step 6)
//
// manifest.name must not already exist in the registry.
// ---------------------------------------------------------------------------

Result<void> PluginLoaderRegistry::validate_name_unique(
    const PluginManifest& manifest) const noexcept {

    if (loaded_.find(manifest.name) != loaded_.end()) {
        return std::unexpected(glibre::Error{core::Error::PluginNameCollision});
    }
    return {};
}

// ---------------------------------------------------------------------------
// validate_dependencies — gate 4 (plugin-abi.md step 7)
//
// Every entry in manifest.depends_on must already be registered.
// Returns on the first missing dependency.
// ---------------------------------------------------------------------------

Result<void> PluginLoaderRegistry::validate_dependencies(
    const PluginManifest& manifest) const noexcept {

    for (const eastl::string& dep : manifest.depends_on) {
        if (loaded_.find(dep) == loaded_.end()) {
            return std::unexpected(glibre::Error{core::Error::PluginDependencyMissing});
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// validate_all — run gates 1–4 in order.
//
// Both manifest.abi_hash and symbol_abi_hash must equal expected_abi_hash
// per plugin-abi.md §step 4 ("the redundant check catches a malformed
// manifest whose abi_hash field disagrees with the compiled-in symbol").
// ---------------------------------------------------------------------------

Result<void> PluginLoaderRegistry::validate_all(
    const PluginManifest& manifest,
    eastl::string_view    expected_abi_hash,
    eastl::string_view    symbol_abi_hash) const noexcept {

    // Gate 1a: symbol-side ABI hash check.
    if (symbol_abi_hash != expected_abi_hash) {
        return std::unexpected(glibre::Error{core::Error::PluginAbiHashMismatch});
    }

    // Gate 1b: manifest-side ABI hash check.
    if (auto r = validate_abi_hash(manifest, expected_abi_hash); !r) {
        return r;
    }

    // Gate 2: engine version.
    if (auto r = validate_engine_version(manifest); !r) {
        return r;
    }

    // Gate 3: name uniqueness.
    if (auto r = validate_name_unique(manifest); !r) {
        return r;
    }

    // Gate 4: dependency resolution.
    return validate_dependencies(manifest);
}

// ---------------------------------------------------------------------------
// register_plugin — record a successfully validated plugin.
// ---------------------------------------------------------------------------

void PluginLoaderRegistry::register_plugin(
    const PluginManifest& manifest,
    eastl::string_view    path) noexcept {

    // Debug-mode precondition: must not already be registered.
    assert(loaded_.find(manifest.name) == loaded_.end() &&
           "register_plugin called for a name that is already registered; "
           "call validate_all first");

    PluginRecord rec;
    rec.name    = manifest.name;
    rec.version = manifest.version;
    rec.path    = eastl::string{path.data(), path.size()};

    loaded_.emplace(manifest.name, eastl::move(rec));
}

// ---------------------------------------------------------------------------
// is_registered
// ---------------------------------------------------------------------------

bool PluginLoaderRegistry::is_registered(eastl::string_view name) const noexcept {
    return loaded_.find(eastl::string{name.data(), name.size()}) != loaded_.end();
}

// ---------------------------------------------------------------------------
// loaded_count
// ---------------------------------------------------------------------------

std::size_t PluginLoaderRegistry::loaded_count() const noexcept {
    return loaded_.size();
}

}  // namespace glibre::core
