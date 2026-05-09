// core/src/plugin_loader_actions.cpp
//
// Loader-procedure free functions — steps 9–11 of the plugin loader sequence,
// and hot-reload swap candidate validation (plan #250).
//
// Authority: reviews/decisions/plugin-abi.md §"Loader Sequence" steps 9–11
//            and §"Failure Modes → core::Error".
//            reviews/decisions/hot-reload-protocol.md §"Step 2 — Swap" 2.1–2.2.
//
// Plans: #229 (dlopen/dlsym/manifest, steps 1–3).
//        #230 (ABI hash + version + name + deps gates, steps 4–7).
//        #231 (register call + rebuild + migrate stubs, steps 9–11).
//        #250 (hot-reload manifest + ABI hash gate — hot_reload_validate).
// Out of scope: dlopen/dlsym (plan #229); registry-of-records state (plan #230).

#include "glibre/core/plugin_loader_actions.hpp"

#include <cstdint>

#include "glibre/core/plugin_api.hpp"  // PluginContext, RegisterFn
#include "glibre/log_error.hpp"        // glibre::variant_code_string

namespace glibre::core {

// ---------------------------------------------------------------------------
// call_register — loader step 9 (plugin-abi.md §"Loader Sequence")
//
// Invokes the resolved glibre_plugin_register function pointer with the
// provided PluginContext reference.  On failure wraps the returned error
// detail in core::Error::PluginInitFailed per plugin-abi.md §step 9.
//
// Null pointer case: a null register_fn indicates the symbol was not resolved
// (loader step 2 failure).  Per plugin-abi.md §"Failure Modes" the null/
// unresolved entry-point maps to step 2 → PluginMissingEntryPoint, not step 9.
//
// Caller responsibilities on any failure:
//   - dlclose the dylib handle (the loader owns the handle, not this function).
//   - Remove any partial registrations the plugin may have made.
//
// MVP ownership-record note (plugin-abi.md §"Open Questions" point 1):
//   Per-plugin ownership records for compensating unregister are deferred to
//   the hot-reload story.  MVP load sequence aborts the entire load on
//   register() failure; no partial unwind of individual registry entries is
//   attempted here (the plugin either succeeds fully or the whole plugin load
//   is rolled back at the dlopen level by the caller).
// ---------------------------------------------------------------------------

Result<void> call_register(RegisterFn register_fn, PluginContext& ctx) noexcept {
    // Null register_fn: symbol was never resolved — this is a step-2 condition
    // (missing entry-point), not a step-9 condition (plugin register failure).
    // Per plugin-abi.md §"Failure Modes" table, step 2 → PluginMissingEntryPoint.
    if (register_fn == nullptr) {
        return std::unexpected(
            glibre::Error{
                core::Error::PluginMissingEntryPoint,
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
    // failure the inner error is the plugin's own diagnostic.  We wrap it in
    // PluginInitFailed so the loader's error arm is stable regardless of which
    // inner code the plugin emits (plugin-abi.md §"Failure Modes" step 9
    // mandates PluginInitFailed, *carrying the inner error in ErrorContext::detail*).
    //
    // glibre::variant_code_string() returns a const char* pointing to a
    // string literal from the hand-written to_string() overloads in log_error.hpp.
    // Those literals have static storage duration and are therefore safe to store
    // in the eastl::string_view detail field — no pointer instability concern
    // (contrast with dlerror() text per plugin-abi.md step 1 dlerror() note).
    if (auto r = register_fn(ctx); !r) {
        const char* inner_detail = glibre::variant_code_string(r.error());
        return std::unexpected(
            glibre::Error{
                core::Error::PluginInitFailed,
                ErrorContext{
                    .file = __FILE__,
                    .line = __LINE__,
                    // detail carries the inner error's stable enumerator name
                    // (plugin-abi.md §"Loader Sequence" step 9).
                    .detail = eastl::string_view{inner_detail},
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

Result<void> rebuild_schedule() noexcept {
    // MVP stub — no topology to rebuild yet (SystemRegistry not landed).
    // When #247 / #248 land, replace this body with:
    //   return system_registry.rebuild_schedule();  // → SystemScheduleCycle on cycle
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

Result<void> migrate_components(
    std::uint32_t /*from_version*/, std::uint32_t /*to_version*/
) noexcept {
    // MVP stub — real archetype migration deferred to plan #221.
    // When from_version == to_version there is nothing to migrate; this stub
    // handles that case correctly.  When they differ, the real implementation
    // will dispatch the migration chain; the stub silently succeeds (acceptable
    // for MVP since no persistent archetype data exists yet).
    return {};
}

// ---------------------------------------------------------------------------
// hot_reload_validate — pre-swap compatibility check (plan #250).
//
// Authority: reviews/decisions/hot-reload-protocol.md §"Step 2 — Swap" 2.1–2.2.
//
// Performs three manifest-only checks in order.  The checks do not touch ECS
// state (archetype storage, migration arena) — those checks belong to plans
// #251+.
//
// Check A — Plugin name identity:
//   The incoming plugin must have the same name as the outgoing plugin.  A
//   different name is a configuration error (wrong dylib).
//   Failure → core::Error::PluginNameMismatch.
//
// Check B — ABI hash equality (protocol §2.1):
//   The incoming manifest's abi_hash must equal the outgoing manifest's
//   abi_hash.  Both were already validated against the host hash by
//   PluginLoaderRegistry::validate_all() before this function is called,
//   so manifest-vs-manifest equality implies both-vs-host equality.
//   Failure → core::Error::PluginAbiHashMismatch.
//
// Check C — SemVer major version compatibility (protocol §2.2 compatible swap):
//   The incoming plugin's major version must equal the outgoing's.  Minor and
//   patch may advance.  A major bump signals a breaking change requiring a
//   fresh world.
//   Failure → core::Error::HotReloadRefused.
// ---------------------------------------------------------------------------

Result<void>
hot_reload_validate(const PluginManifest& outgoing, const PluginManifest& incoming) noexcept {
    // Check A — Name identity.
    // Both plugins must declare the same name.  A different name means the
    // operator supplied the wrong dylib as a swap candidate.
    if (incoming.name != outgoing.name) {
        return std::unexpected(
            glibre::Error{
                core::Error::PluginNameMismatch,
                ErrorContext{
                    .file = __FILE__,
                    .line = __LINE__,
                    .detail = "incoming plugin name does not match outgoing plugin name",
                },
            }
        );
    }

    // Check B — ABI hash equality (hot-reload-protocol §2.1).
    // The two manifests must carry identical abi_hash strings.  The
    // PluginLoaderRegistry already confirmed each against the host hash
    // independently; here we confirm they agree with each other (a belt-and-
    // suspenders guard and a clear error message if they somehow diverge).
    if (incoming.abi_hash != outgoing.abi_hash) {
        return std::unexpected(
            glibre::Error{
                core::Error::PluginAbiHashMismatch,
                ErrorContext{
                    .file = __FILE__,
                    .line = __LINE__,
                    .detail = "incoming plugin abi_hash does not match outgoing plugin abi_hash",
                },
            }
        );
    }

    // Check C — SemVer major version compatibility (hot-reload-protocol §2.2).
    // A major-version change is a breaking redesign that cannot be hot-reloaded
    // into a running world; it requires a process restart with a fresh world.
    if (incoming.version.major != outgoing.version.major) {
        return std::unexpected(
            glibre::Error{
                core::Error::HotReloadRefused,
                ErrorContext{
                    .file = __FILE__,
                    .line = __LINE__,
                    .detail = "incoming plugin major version differs from outgoing — "
                              "major-version change requires a fresh world, not a hot-reload",
                },
            }
        );
    }

    return {};
}

}  // namespace glibre::core
