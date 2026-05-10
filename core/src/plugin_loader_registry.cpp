// core/src/plugin_loader_registry.cpp
//
// PluginLoaderRegistry — gates 4–7 of the plugin loader sequence,
// plus the phase-8 drain guard (plan #981).
//
// Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" steps 4–7, 8
//            and §"Failure Modes → core::Error".
//            reviews/decisions/frame-phases.md §8.
//
// Plans: #230 (ABI hash + version + name + deps gates, steps 4–7).
//        #981 (phase-8 drain guard — validate_drain_phase).
// Out of scope: dlopen/dlsym (plan #229); post-gate actions 9–11
//   (plugin_loader_actions.cpp, plan #231).

#include "glibre/core/plugin_loader_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace glibre::core {

// ---------------------------------------------------------------------------
// PluginLoaderRegistry — constructor
// ---------------------------------------------------------------------------

PluginLoaderRegistry::PluginLoaderRegistry(SemVer host_engine_version) noexcept
    : host_engine_version_{host_engine_version} {}

// ---------------------------------------------------------------------------
// validate_drain_phase — phase-8 precondition guard (plan #981).
//
// Authority: plugin-abi.md §"Loader Sequence" step 8 and frame-phases.md §8.
//
// Returns success only when current_phase == Phase::HotReload (phase 8).
// Any other phase returns core::Error::FramePhaseMisordered.
//
// SRP note: the caller injects current_phase; PluginLoaderRegistry does not
// own or query phase state directly (plans #247/#248 will supply the real
// FramePhaseTracker; this function is the minimal injection point).
// ---------------------------------------------------------------------------

Result<void> PluginLoaderRegistry::validate_drain_phase(Phase current_phase) noexcept {
    if (current_phase != Phase::HotReload) {
        return std::unexpected(
            glibre::Error{
                core::Error::FramePhaseMisordered,
                ErrorContext{
                    .file = __FILE__,
                    .line = __LINE__,
                    .detail = "registry mutation attempted outside phase 8 (HotReload); "
                              "call_register, rebuild_schedule, and migrate_components "
                              "are only permitted during the hot-reload barrier",
                },
            }
        );
    }
    return {};
}

// ---------------------------------------------------------------------------
// validate_manifest_abi_hash — gate 1a (plugin-abi.md step 4, manifest field)
//
// manifest.abi_hash must equal expected_abi_hash.
// ---------------------------------------------------------------------------

Result<void> PluginLoaderRegistry::validate_manifest_abi_hash(
    const PluginManifest& manifest, eastl::string_view expected_abi_hash
) const noexcept {

    // manifest.abi_hash is std::pmr::string; expected_abi_hash is eastl::string_view.
    // Bridge via string_view so std::pmr::string::operator== can compare.
    if (manifest.abi_hash != std::string_view{expected_abi_hash.data(), expected_abi_hash.size()}) {
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

    for (const std::pmr::string& dep : manifest.depends_on) {
        // Heterogeneous lookup: eastl::string_view{dep.data(), dep.size()} bridges
        // the std::pmr::string element to the eastl::transparent_string_hash key.
        if (loaded_.find(eastl::string_view{dep.data(), dep.size()}) == loaded_.end()) {
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

    // Bridge manifest.name (std::pmr::string) → eastl::string_view once.
    // All three uses below (collision check, idempotent lookup, and map key
    // construction) draw from this single materialization.
    const eastl::string_view name_sv{manifest.name.data(), manifest.name.size()};

    // Unconditional precondition check — protects release builds from
    // silent overwrites (replaces the former debug-only assert).
    if (is_collision(name_sv, path)) {
        return std::unexpected(glibre::Error{core::Error::PluginNameCollision});
    }

    const auto it = loaded_.find(name_sv);
    if (it != loaded_.end()) {
        // Same name + same path: idempotent re-registration, no-op.
        return {};
    }

    // Materialise one eastl::string for rec.name and reuse it as the map key.
    eastl::string name_owned{name_sv.data(), name_sv.size()};

    PluginRecord rec;
    rec.name = name_owned;
    rec.version = manifest.version;
    rec.path = eastl::string{path.data(), path.size()};

    loaded_.emplace(eastl::move(name_owned), eastl::move(rec));
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
