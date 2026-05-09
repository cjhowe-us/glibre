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

#include <cassert>
#include <cstdint>

#include "glibre/alloc.hpp"                      // PerContextAllocator, AllocatorHandle
#include "glibre/core/context_tag_resolver.hpp"  // derive_context_tag
#include "glibre/core/plugin_api.hpp"            // PluginContext, RegisterFn
#include "glibre/log_error.hpp"                  // glibre::variant_code_string

namespace glibre::core {

// ---------------------------------------------------------------------------
// call_register_stamped — production step 9 with AllocatorHandle stamping
//
// Implements the full seam described in issue #989 §Scope and in perf-budget.md
// §Allocator Rules #1:
//
//   derive_context_tag(manifest.name)
//     → AllocatorHandle{per_context_alloc, tag}
//     → PluginContext{..., alloc_handle}
//     → call_register(register_fn, ctx)
//
// This is the only production call path that invokes call_register.  The lower-
// level call_register(RegisterFn, PluginContext&) overload is retained for unit
// tests that need to supply a hand-crafted PluginContext (e.g. stub tests in
// plan #231) while this function is the production-facing entry-point.
//
// Authority: issue #989 §Scope; perf-budget.md §Allocator Rules #1; plugin-abi.md
//            §"Loader Sequence" step 9.
// ---------------------------------------------------------------------------

Result<void> call_register_stamped(
    RegisterFn register_fn,
    glibre::PerContextAllocator& per_context_alloc,
    const PluginManifest& manifest,
    World& world,
    TypeRegistry& type_registry,
    SystemRegistry& system_registry,
    PassRegistry& pass_registry,
    PanelRegistry& panel_registry,
    LogSink& log
) noexcept {
    // Step 1: derive the ContextTag from the manifest plugin name.
    //
    // The manifest name follows the convention "glibre.<context>[.<sub>...]".
    // derive_context_tag extracts the second dot-delimited component and maps it
    // to the corresponding ContextTag enumerator.  An unknown or malformed name
    // returns PluginManifestInvalid; the gates in plan #230 should have caught
    // this earlier, but we propagate cleanly here rather than asserting.
    auto tag_result = derive_context_tag(eastl::string_view{manifest.name.c_str()});
    if (!tag_result) {
        return std::unexpected(tag_result.error());
    }

    // Step 2: construct the stamped AllocatorHandle.
    //
    // The tag is stamped once at handle-construction time so the plugin's
    // allocate() / deallocate() call sites are tag-free (perf-budget.md
    // §Allocator Rules #1).  per_context_alloc is owned by the bounded context
    // and outlives this call and the returned handle.
    glibre::AllocatorHandle alloc_handle{per_context_alloc, *tag_result};

    // Step 3: build the PluginContext aggregate.
    //
    // PluginContext is a POD-like mixed-storage aggregate (references + one value
    // type: alloc).  The aggregate is built here on the stack and passed by
    // reference to the plugin's glibre_plugin_register entry-point.  The context
    // is destroyed when call_register returns; plugins must not stash pointers to
    // it past the call (plugin-abi.md §"Registration Entry-Point Signature").
    PluginContext ctx{
        .world = world,
        .type_registry = type_registry,
        .system_registry = system_registry,
        .pass_registry = pass_registry,
        .panel_registry = panel_registry,
        .manifest = manifest,
        .log = log,
        .alloc = alloc_handle,
    };

    // Step 4: invoke call_register with the fully-stamped context.
    return call_register(register_fn, ctx);
}

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
// rebuild_schedule(rebuild_fn) — test-injectable overload (plan #982).
//
// Calls rebuild_fn() and propagates its result unchanged.  This overload
// exists to provide a test-injectable seam so that the SystemScheduleCycle
// failure path is exercisable before plans #247/#248 land with the real
// topology sort.
//
// In production (plan #247/#248), overload (1) (no-parameter form) will be
// updated to call the real schedule builder; this overload remains available
// as a permanent test-injection point that mirrors the MigrationStepFn seam
// for step 11.
//
// Design note: rebuild_fn is a plain function pointer (not eastl::function<>)
// to avoid heap allocation and vtable overhead.  All test-injection scenarios
// supply file-scope function pointers, so the plain pointer is sufficient
// (mirrors the MigrationStepFn design note in migrate_components).
// ---------------------------------------------------------------------------

Result<void> rebuild_schedule(ScheduleRebuildFn rebuild_fn) noexcept {
    // Precondition: rebuild_fn must be non-null (documented in the header —
    // the injectable overload requires a valid callable).  Fire a debug-build
    // assertion consistent with the codebase pattern.
#ifndef NDEBUG
    assert(
        rebuild_fn != nullptr &&
        "rebuild_schedule precondition: "
        "rebuild_fn must be non-null — caller must supply a valid schedule rebuild step"
    );
#endif

    // Invoke the injected rebuild step and propagate the result unchanged.
    return rebuild_fn();
}

// ---------------------------------------------------------------------------
// migrate_components — loader step 11 (plugin-abi.md §"Loader Sequence")
//
// Two overloads; see plugin_loader_actions.hpp for the full contract.
//
// Overload (1): migrate_components(from_version, to_version)
//   MVP STUB: returns success unconditionally for any (from_version,
//   to_version) pair, including when they differ.
//
//   The real implementation walks glibre_plugin_migrations_<TypeName>
//   export tables emitted by glibre-foryc (plan #221) and dispatches each
//   migration step against the pre-swap archetype snapshot (fory-codegen.md
//   §"Migration Mechanic").
//
// Overload (2): migrate_components(from_version, to_version, step_fn)
//   Test-injectable path.  Calls step_fn() when from_version != to_version
//   and propagates its result.  The no-op path (from == to) is handled
//   before step_fn is invoked so a null step_fn is never dereferenced on the
//   no-op path (the non-null precondition only applies when from != to).
//
//   Design note: the step_fn parameter is intentionally a plain function
//   pointer rather than eastl::function<>.  eastl::function carries heap
//   allocation and vtable overhead.  All test-injection scenarios supply
//   file-scope or lambda-converted-to-function-pointer callables, so the
//   plain pointer is sufficient and avoids the allocation.
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

Result<void> migrate_components(
    std::uint32_t from_version, std::uint32_t to_version, MigrationStepFn step_fn
) noexcept {
    // No migration needed when from == to (identity case).
    // step_fn is not called: callers may pass a valid mock without it running.
    if (from_version == to_version) {
        return {};
    }

    // Precondition: step_fn must be non-null when from != to (documented in
    // plugin_loader_actions.hpp @param step_fn "Must be non-null").  A null
    // pointer here is a caller logic error — the caller must supply a migration
    // step when versions differ.  Fire a debug-build assertion consistent with
    // the codebase pattern (see hot_reload_validate precondition asserts above).
#ifndef NDEBUG
    assert(
        step_fn != nullptr && "migrate_components precondition: "
                              "step_fn must be non-null when from_version != to_version — "
                              "caller must supply a migration step for schema bumps"
    );
#endif

    // Invoke the migration step and propagate the result unchanged.
    // In production (plan #221) this will be replaced by a per-type migration
    // table walk; this overload exists solely to provide a test-injectable seam
    // so that the SchemaMigrationFailed path is exercisable before #221 lands.
    return step_fn();
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
// Check B — ABI hash equality (protocol §2.1, incoming-vs-outgoing form):
//   The incoming manifest's abi_hash must equal the outgoing manifest's
//   abi_hash.
//
//   Protocol §2.1 specifies incoming == host_glibre_types_abi_hash
//   (incoming-vs-host).  Here we compare incoming-vs-outgoing.  This is
//   equivalent under the PRECONDITION that PluginLoaderRegistry::validate_all()
//   confirmed BOTH manifests equal the host hash before this function is
//   called.  Transitivity: both equal host → they equal each other.  The
//   comparison of the two manifests also surfaces manifest-vs-manifest
//   disagreement as a distinct diagnostic (see .hpp docstring for full
//   rationale).
//
//   PRECONDITION ASSERTION: The #ifndef NDEBUG / assert() below fires in debug
//   builds if outgoing.abi_hash is empty (a proxy for "validate_all not called"),
//   catching the most common misuse without requiring a host_abi_hash parameter.
//
//   Failure → core::Error::PluginAbiHashMismatch.
//
// Check C — SemVer major version (plan #250 extension — NOT a verbatim
//            derivation of protocol §2.2):
//   The incoming plugin's major version must equal the outgoing's.  Minor and
//   patch may advance.  A major bump signals a breaking change requiring a
//   fresh world.
//
//   Protocol §2.2 defines a component-type-set superset check on
//   (fqn, schema_version).  Check C here is a FASTER MANIFEST-ONLY GATE that
//   plan #250 adds as a precondition: if the major versions differ, the swap
//   is refused immediately without touching ECS state.  The full superset check
//   from protocol §2.2 is deferred to plans #251+ and will supplement, not
//   replace, this check.  See .hpp docstring for full divergence note.
//
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

    // Check B — ABI hash equality (hot-reload-protocol §2.1, incoming-vs-outgoing form).
    //
    // Protocol §2.1 requires incoming == host_glibre_types_abi_hash (incoming-vs-host).
    // We compare incoming-vs-outgoing here, which is equivalent under the precondition
    // that PluginLoaderRegistry::validate_all() confirmed BOTH manifests equal the host
    // hash before this function is called.  See the function-level comment above (and
    // the .hpp docstring) for the full equivalence proof and rationale.
    //
    // The assertion below fires in debug builds if outgoing.abi_hash is empty, which
    // is a proxy for "validate_all() was never called for the outgoing plugin".  An
    // empty hash string cannot be a valid 64-char blake3 hex string, so it indicates
    // a caller-contract violation rather than a legitimate hash mismatch.
#ifndef NDEBUG
    assert(
        !outgoing.abi_hash.empty() &&
        "hot_reload_validate precondition: "
        "outgoing must have been validated by PluginLoaderRegistry::validate_all() "
        "before calling hot_reload_validate — outgoing.abi_hash is empty"
    );
    assert(
        !incoming.abi_hash.empty() &&
        "hot_reload_validate precondition: "
        "incoming must have been validated by PluginLoaderRegistry::validate_all() "
        "before calling hot_reload_validate — incoming.abi_hash is empty"
    );
#endif
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

    // Check C — SemVer major version (plan #250 extension of hot-reload-protocol §2.2).
    //
    // Protocol §2.2 defines a component-type-set superset check on (fqn, schema_version).
    // This check is NOT a verbatim derivation of that rule.  It is an additional manifest-only
    // precondition introduced by plan #250: if the incoming plugin's SemVer major differs from
    // the outgoing plugin's, the swap is refused immediately without touching ECS state.  The
    // full superset check from protocol §2.2 (which requires archetype storage access) is
    // deferred to plans #251+.  When those plans land, both checks will run; this one fires
    // first as an inexpensive fast-path gate.
    //
    // The protocol's descriptive phrase "major-version change" refers to a symptom of dropping
    // a registered type (the superset check failing), not to a SemVer field comparison.  Plan
    // #250 adopts SemVer-major equality as a stricter, observable proxy for that symptom.  Any
    // plugin where major changed is presumed to have made breaking schema changes; it gets a
    // clear, early refusal rather than waiting for the ECS-level check.
    //
    // See the .hpp docstring (Check C divergence note) for the full decision reasoning.
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
