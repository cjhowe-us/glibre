// core/src/plugin_loader_registry.cpp
//
// PluginLoaderRegistry — gates 4–7 and post-gate actions 9–11 of the plugin
// loader sequence.
//
// Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" steps 4–11
//            and §"Failure Modes → core::Error".
//
// Plans: #230 (ABI hash + version + name + deps gates, steps 4–7).
//        #231 (register call + rebuild + migrate stubs, steps 9–11).
// Out of scope: dlopen/dlsym (plan #229).

#include "glibre/core/plugin_loader_registry.hpp"

#include <cstddef>
#include <cstdint>

#include "glibre/core/plugin_api.hpp"  // PluginContext — required by call_register

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

// ---------------------------------------------------------------------------
// call_register — loader step 9 (plugin-abi.md §"Loader Sequence")
//
// Invokes the resolved glibre_plugin_register function pointer with the
// provided PluginContext reference.  On failure wraps the returned error
// detail in core::Error::PluginInitFailed per plugin-abi.md §step 9.
//
// Caller responsibilities on failure:
//   - dlclose the dylib handle (the loader owns the handle, not this method).
//   - Remove any partial registrations the plugin may have made.
//
// MVP ownership-record note (plugin-abi.md §"Open Questions" point 1):
//   Per-plugin ownership records for compensating unregister are deferred to
//   the hot-reload story.  MVP load sequence aborts the entire load on
//   register() failure; no partial unwind of individual registry entries is
//   attempted here (the plugin either succeeds fully or the whole plugin load
//   is rolled back at the dlopen level by the caller).
// ---------------------------------------------------------------------------

Result<void>
PluginLoaderRegistry::call_register(RegisterFn register_fn, PluginContext& ctx) noexcept {
    // register_fn must be non-null; the loader resolved it via dlsym step 2.
    // A null pointer here indicates a caller logic error; refuse loudly.
    if (register_fn == nullptr) {
        return std::unexpected(
            glibre::Error{
                core::Error::PluginInitFailed,
                ErrorContext{
                    .file = __FILE__,
                    .line = __LINE__,
                    .detail = "register_fn is null — caller must resolve symbol before calling",
                },
            }
        );
    }

    // Invoke the plugin entry-point.
    // glibre_plugin_register returns std::expected<void, glibre::Error>; on
    // failure the inner error is the plugin's own diagnostic, not a loader
    // error.  We wrap it in PluginInitFailed so the loader's error arm is
    // stable regardless of which inner code the plugin emits.
    // (plugin-abi.md §"Failure Modes" step 9 mandates PluginInitFailed.)
    if (auto r = register_fn(ctx); !r) {
        return std::unexpected(
            glibre::Error{
                core::Error::PluginInitFailed,
                ErrorContext{
                    .file = __FILE__,
                    .line = __LINE__,
                    // detail: best-effort stable literal; real structured error
                    // is logged by the loader via glibre::log_error per
                    // error-model.md §"Logging / Telemetry" rule 1.
                    .detail = "glibre_plugin_register returned unexpected error",
                },
            }
        );
    }
    return {};
}

// ---------------------------------------------------------------------------
// rebuild_schedule — loader step 10 (plugin-abi.md §"Loader Sequence")
//
// MVP STUB: returns success unconditionally.
//
// The real implementation recomputes the per-phase system schedule from the
// union of all loaded plugins' declared (reads, writes, after, before) edges
// and detects cycles (→ core::Error::SystemScheduleCycle).
//
// TODO(#247): integrate with frame-loop SystemRegistry schedule builder.
// TODO(#248): topological sort of SystemDecl ordering edges within each phase.
// ---------------------------------------------------------------------------

Result<void> PluginLoaderRegistry::rebuild_schedule() const noexcept {
    // MVP stub — no topology to rebuild yet (SystemRegistry not landed).
    // When #247 / #248 land, replace this body with:
    //   return system_registry_.rebuild_schedule();  // → SystemScheduleCycle on cycle
    return {};
}

// ---------------------------------------------------------------------------
// migrate_components — loader step 11 (plugin-abi.md §"Loader Sequence")
//
// MVP STUB: returns success unconditionally for any (from_version, to_version)
// pair, including when they differ.
//
// The real implementation walks glibre_plugin_migrations_<TypeName> export
// tables emitted by glibre-foryc (plan #221) and dispatches each migration
// step against the pre-swap archetype snapshot (fory-codegen.md §"Migration
// Mechanic").
//
// TODO(#221): walk per-type migration tables once glibre-foryc emits them.
//   Table format per plan #221: glibre_plugin_migrations_<TypeName> is an
//   array of MigrationStep{from_v, to_v, fn} terminated by a sentinel entry
//   with both versions == 0.  The loader dlsym()s each type's table and calls
//   fn(raw_bytes, size) for each step whose from_v == current schema version.
// ---------------------------------------------------------------------------

Result<void> PluginLoaderRegistry::migrate_components(
    std::uint32_t /*from_version*/, std::uint32_t /*to_version*/
) noexcept {
    // MVP stub — real archetype migration deferred to plan #221.
    // When from_version == to_version there is nothing to migrate; this stub
    // handles that case correctly.  When they differ, the real implementation
    // will dispatch the migration chain; the stub silently succeeds (acceptable
    // for MVP since no persistent archetype data exists yet).
    return {};
}

}  // namespace glibre::core
