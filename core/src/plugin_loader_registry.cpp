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
//        #1044 (EASTL → libc++ container migration).
//
// Container migration (plan #1044, reviews/decisions/eastl-removal.md):
//   eastl::hash_map  →  std::pmr::unordered_map (OQ-2 fallback: std::pmr::flat_map
//     alias not present in libc++ 22 despite __cpp_lib_flat_map = 202511).
//   eastl::string    →  std::pmr::string
//   eastl::string_view → std::string_view
//   Heterogeneous lookup preserved via glibre::TransparentStringHash + std::equal_to<>.
//
// Out of scope: dlopen/dlsym (plan #229); post-gate actions 9–11
//   (plugin_loader_actions.cpp, plan #231).

#include "glibre/core/plugin_loader_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace glibre::core {

// ---------------------------------------------------------------------------
// PluginLoaderRegistry — constructor
//
// Initialises the memory resource pointer and constructs the unordered_map
// with the supplied resource so all map storage is accounted under the
// backing allocator (PerContextAllocatorResource for production, or
// get_default_resource() for tests).
//
// RAII note: mr_ is declared before loaded_ in the header so that the
// resource is initialised before the map's allocator captures the pointer.
// ---------------------------------------------------------------------------

PluginLoaderRegistry::PluginLoaderRegistry(SemVer host_engine_version,
                                           std::pmr::memory_resource* mr) noexcept
    : host_engine_version_{host_engine_version},
      mr_{mr},
      loaded_{mr_} {}

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
// manifest.abi_hash is std::pmr::string; heterogeneous comparison against
// std::string_view works because std::pmr::string::operator== is defined for
// std::string_view arguments (operator==(std::string_view) via C++17 char_traits).
// ---------------------------------------------------------------------------

Result<void> PluginLoaderRegistry::validate_manifest_abi_hash(
    const PluginManifest& manifest, std::string_view expected_abi_hash
) const noexcept {

    if (std::string_view{manifest.abi_hash} != expected_abi_hash) {
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
    std::string_view symbol_abi_hash, std::string_view expected_abi_hash
) const noexcept {

    if (symbol_abi_hash != expected_abi_hash) {
        return std::unexpected(glibre::Error{core::Error::PluginAbiHashMismatch});
    }
    return {};
}

// ---------------------------------------------------------------------------
// validate_engine_version — gate 2 (plugin-abi.md step 5)
//
// manifest.min_engine_version <= host engine version.
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
    const PluginManifest& manifest, std::string_view file_path
) const noexcept {

    if (is_collision(std::string_view{manifest.name}, file_path)) {
        return std::unexpected(glibre::Error{core::Error::PluginNameCollision});
    }
    // Name absent, or same name + same path: idempotent; not an error.
    return {};
}

// ---------------------------------------------------------------------------
// validate_dependencies — gate 4 (plugin-abi.md step 7)
//
// Every entry in manifest.depends_on must already be registered.
// Returns on the first missing dependency.
//
// Heterogeneous lookup: std::string_view{dep} avoids materialising a
// temporary std::pmr::string key per iteration (TransparentStringHash).
// ---------------------------------------------------------------------------

Result<void>
PluginLoaderRegistry::validate_dependencies(const PluginManifest& manifest) const noexcept {

    for (const std::pmr::string& dep : manifest.depends_on) {
        // Heterogeneous lookup: std::string_view avoids materialising a
        // temporary std::pmr::string key per call (transparent hash + eq).
        if (loaded_.find(std::string_view{dep}) == loaded_.end()) {
            return std::unexpected(glibre::Error{core::Error::PluginDependencyMissing});
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// is_collision — name/path collision predicate (MED-4: single source of truth).
//
// Returns true when a plugin with the same name is already registered with a
// DIFFERENT file path -- the condition that must fire PluginNameCollision.
// Same name + same path is idempotent (hot-reload re-registration); returns
// false.  Name not present: returns false.
//
// Heterogeneous lookup on both name and path via std::string_view.
// ---------------------------------------------------------------------------

bool PluginLoaderRegistry::is_collision(
    std::string_view name, std::string_view file_path
) const noexcept {
    const auto it = loaded_.find(name);
    if (it == loaded_.end()) {
        return false;
    }
    return std::string_view{it->second.path} != file_path;
}

// ---------------------------------------------------------------------------
// validate_all — run gates 1-4 in order.
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
    std::string_view expected_abi_hash,
    std::string_view symbol_abi_hash,
    std::string_view file_path
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
//
// Key storage: the std::pmr::string key is constructed under mr_ so the
// stored key's lifetime is tied to loaded_, not to the caller's manifest.
// ---------------------------------------------------------------------------

Result<void> PluginLoaderRegistry::register_plugin(
    const PluginManifest& manifest, std::string_view path
) noexcept {

    // Unconditional precondition check -- protects release builds from
    // silent overwrites (replaces the former debug-only assert).
    if (is_collision(std::string_view{manifest.name}, path)) {
        return std::unexpected(glibre::Error{core::Error::PluginNameCollision});
    }

    const auto it = loaded_.find(std::string_view{manifest.name});
    if (it != loaded_.end()) {
        // Same name + same path: idempotent re-registration, no-op.
        return {};
    }

    PluginRecord rec;
    rec.name = std::pmr::string{manifest.name, mr_};
    rec.version = manifest.version;
    rec.path = std::pmr::string{path, mr_};

    loaded_.emplace(std::pmr::string{manifest.name, mr_}, std::move(rec));
    return {};
}

// ---------------------------------------------------------------------------
// is_registered
//
// Heterogeneous lookup: std::string_view avoids materialising a temporary
// std::pmr::string key per call (TransparentStringHash + std::equal_to<>).
// ---------------------------------------------------------------------------

bool PluginLoaderRegistry::is_registered(std::string_view name) const noexcept {
    return loaded_.find(name) != loaded_.end();
}

// ---------------------------------------------------------------------------
// loaded_count
// ---------------------------------------------------------------------------

std::size_t PluginLoaderRegistry::loaded_count() const noexcept { return loaded_.size(); }

}  // namespace glibre::core
