# Decision Record: Plugin ABI

- Refs: spike #13 (decide-plugin-abi-hash), parent sub-epic on plugin
  loader, epic #2 (cross-cutting foundation).
- Owner context: `core` (plugin loader).
- Builds on: `fory-codegen.md`, `error-model.md`, `frame-phases.md`.

## Status

Accepted (spike output). Implementation deferred to plan issues under
the core plugin-loader sub-epic. This record locks the plugin file
layout, manifest schema, registration entry-point signature, loader
sequence, and failure-to-error mapping.

## Context

PHILOSOPHY §3 makes every domain a plugin `.dylib`; PHILOSOPHY §9
makes the plugin ABI gated by a middleman dylib hash; PHILOSOPHY §8
forbids mid-frame swaps. The frame-phases decision pins the swap to
phase 8 (drain → swap → migrate → resume). The fory-codegen decision
introduced `glibre-types.dylib` as the single middleman that every
plugin links and exposed `glibre_types_abi_hash()` as a single blake3
over the concatenated, sorted schema-source hashes. The error-model
decision made `core::Error` the carrier for all plugin-loader refusal
cases.

What this spike answers, that the upstream decisions did not:

1. The exact extern "C" surface a plugin must export.
2. The on-dylib manifest format (Fory-serialized) and how the loader
   reads it before invoking any plugin C++ code.
3. The deterministic loader sequence (`dlopen` → `dlsym` →
   manifest-read → hash-check → register → resume) and which
   `core::Error` arm fires at each refusal point.
4. Versioning rules — when ABI hash bumps, when SONAME bumps, when a
   plugin's own `version` field bumps — so the three move
   independently and predictably.

## Decision

### Plugin file shape

1. Each plugin = exactly one `.dylib` file. No multi-dylib plugins;
   if a domain needs sub-libraries, they are statically linked into
   the plugin dylib.
2. Each plugin links **only** `glibre-types.dylib` from the engine's
   side. It must not link `glibre-core` or any other plugin. Cross-
   plugin communication happens entirely through ECS components and
   types defined in the middleman.
3. Each plugin exports exactly four C symbols:
   - `glibre_plugin_abi_hash` — `extern "C" const char*`, 32-byte
     blake3 hex string compiled in from the middleman headers at
     build time. The plugin obtains this value by `#include
     <glibre/types/abi_hash.hpp>` and re-exports
     `glibre_types_abi_hash()`'s value as a string literal.
   - `glibre_plugin_manifest` — `extern "C" const std::byte*`,
     pointing to a Fory-serialized `PluginManifest` blob baked into
     a `.rodata` section by the plugin's build (generated header
     emitted by the codegen pipeline below).
   - `glibre_plugin_manifest_size` — `extern "C" std::size_t`, the
     byte length of that blob.
   - `glibre_plugin_register` — the registration entry-point
     (signature in §"Registration Entry-Point Signature").

### Manifest source-of-truth

The manifest is authored as a `plugin.fory` file at the plugin's
crate root, e.g. `plugins/render/plugin.fory`. The codegen step that
already runs for `data/schemas/*.fory` is extended to also process
`plugins/*/plugin.fory`, emitting a generated `manifest.cpp` that
embeds the Fory-serialized manifest blob into the plugin's
translation unit. Authors never write the blob by hand.

## ABI Hash Function

Reaffirmed from `fory-codegen.md` §"middleman dylib exposes":

1. The middleman dylib computes, at codegen time, a single blake3
   digest over the concatenation of `(fqn || ":" || version_le ||
   ":" || schema_source_blake3)` for every schema, with entries
   ordered by fully-qualified name in canonical Unicode code-point
   order. Newlines (`\n`, single byte) separate entries; no trailing
   newline.
2. The 32-byte digest is hex-encoded (lowercase, 64 chars) and
   embedded as a string literal in `glibre/types/abi_hash.hpp`. The
   middleman exports `glibre_types_abi_hash() -> const char*`
   returning the same string.
3. Plugins re-export the identical string under
   `glibre_plugin_abi_hash`, captured at the plugin's compile time.
   Hash equality is byte-string equality of the hex form; no parsing
   required at load time.
4. Inputs to the digest are *only* the schema source hashes plus
   version numbers — not header file timestamps, not compiler
   identity, not flags. This makes the hash a property of the
   contract, not the build environment.

## Plugin Manifest Schema

Authored as `plugin.fory`; generated POD struct lives in
`glibre::types::PluginManifest`. Fory-serialized blob shape:

```fory
schema glibre.core.PluginManifest {
  version 1
  since   "0.1.0"

  field name              : string             tag 1 since 1
  field version           : SemVer             tag 2 since 1
  field abi_hash          : string             tag 3 since 1
  field min_engine_version: SemVer             tag 4 since 1
  field components        : list<ComponentDecl> tag 5 since 1
  field systems           : list<SystemDecl>    tag 6 since 1
  field passes            : list<PassDecl>      tag 7 since 1
  field panels            : list<PanelDecl>     tag 8 since 1
  field depends_on        : list<string>        tag 9 since 1
}

schema glibre.core.SemVer {
  version 1
  field major : u16 tag 1 since 1
  field minor : u16 tag 2 since 1
  field patch : u16 tag 3 since 1
}

schema glibre.core.ComponentDecl {
  version 1
  field fqn          : string  tag 1 since 1
  field schema_hash  : string  tag 2 since 1   # blake3 of the .fory source
  field storage_hint : u8      tag 3 since 1   # archetype/sparse/singleton
}

schema glibre.core.SystemDecl {
  version 1
  field name      : string         tag 1 since 1
  field phase     : u8             tag 2 since 1   # 1..=9 from frame-phases.md
  field reads     : list<string>   tag 3 since 1
  field writes    : list<string>   tag 4 since 1
  field after     : list<string>   tag 5 since 1   # ordering edges within phase
  field before    : list<string>   tag 6 since 1
}

schema glibre.core.PassDecl {
  version 1
  field name        : string  tag 1 since 1
  field render_phase: u8      tag 2 since 1   # phase 6 or 7
  field inputs      : list<string> tag 3 since 1
  field outputs     : list<string> tag 4 since 1
}

schema glibre.core.PanelDecl {
  version 1
  field id     : string  tag 1 since 1
  field title  : string  tag 2 since 1
  field area   : u8      tag 3 since 1   # docked area enum
}
```

Field-by-field semantics for `PluginManifest`:

- **name** — fully-qualified plugin id (e.g. `glibre.render`).
  Unique across loaded plugins; collision is `core::Error::
  PluginNameCollision`.
- **version** — plugin's own SemVer. Independent of `abi_hash` —
  see "Versioning Rules".
- **abi_hash** — the 64-char hex blake3 the plugin was compiled
  against. Must equal the host's `glibre_types_abi_hash()`.
- **min_engine_version** — the minimum `glibre-core` SemVer this
  plugin tolerates; the loader rejects below this with
  `core::Error::PluginEngineTooOld`.
- **components** — every component type the plugin registers into
  the type registry. `schema_hash` lets the loader detect a plugin
  that thinks it owns a type whose schema has since drifted (a
  weaker check than the global `abi_hash`, but a useful one for
  diagnostics).
- **systems** — system descriptors. `phase` is the 1..=9 phase id
  from `frame-phases.md`; `reads`/`writes` are component fqns;
  `after`/`before` are intra-phase ordering edges naming other
  systems by name. The loader topologically sorts within each
  phase and refuses cycles.
- **passes** — render-graph passes (only meaningful for plugins
  registering into render phases 6/7).
- **panels** — editor-UI panels (no-op for non-editor builds).
- **depends_on** — plugin names that must already be registered
  before this plugin's `register` runs. Used for ordering only;
  plugins still talk through the type registry, not direct calls.

## Registration Entry-Point Signature

```cpp
// extern "C" prototype, exported by every plugin .dylib.
// Signature is C-ABI on the wire; the std::expected return crosses
// only the middleman dylib boundary, both sides of which compile
// against the same libc++ that built glibre-types.dylib (per
// fory-codegen.md §"Open Questions" #4).

#include <expected>
#include <glibre/error.hpp>
#include <glibre/core/plugin_api.hpp>

extern "C" std::expected<void, glibre::Error>
glibre_plugin_register(glibre::core::PluginContext& ctx) noexcept;
```

Where `glibre::core::PluginContext` is a stable struct exported by
`glibre-core` (not by the middleman) and contains:

- `World&` — the ECS world the plugin registers components/systems
  into.
- `TypeRegistry&` — for component-type registration.
- `SystemRegistry&` — for system registration into named phases.
- `PassRegistry&` — for render-graph pass registration.
- `PanelRegistry&` — editor UI registration.
- `const PluginManifest&` — the loader passes the same manifest
  back to the plugin so register() can iterate its declared items
  rather than duplicating the list in code.
- `LogSink&` — `spdlog`-backed structured sink the plugin uses for
  registration-time diagnostics.

`PluginContext` is a reference, not a pointer, so the plugin cannot
take ownership or store it past the call. The loader guarantees the
context outlives the call; after `register` returns the context is
destroyed and any cached pointer is dangling.

`register` returns `std::expected<void, glibre::Error>`. On failure
the loader treats the entire load as refused and runs the symmetric
unregister of any partial registration the plugin made. Plugins
that need symmetric teardown ship a paired
`glibre_plugin_unregister(PluginContext&)` (same shape) — optional
in MVP, mandatory once hot-reload migrations land.

## Loader Sequence

Executed during phase 8 (frame-phases.md). The loader is the only
component permitted to touch plugin dylibs.

1. **dlopen** the candidate `.dylib` with `RTLD_NOW | RTLD_LOCAL`.
   `RTLD_NOW` forces immediate symbol resolution, surfacing missing
   middleman symbols as a load failure rather than a later
   crash. `RTLD_LOCAL` keeps the plugin's symbols out of the global
   namespace.
   - On `dlopen` failure → `core::Error::PluginDlopenFailed`,
     attach `dlerror()` text to `ErrorContext::detail`. Abort
     sequence; no further steps.
2. **dlsym** the four required symbols: `glibre_plugin_abi_hash`,
   `glibre_plugin_manifest`, `glibre_plugin_manifest_size`,
   `glibre_plugin_register`. Any missing symbol →
   `core::Error::PluginMissingEntryPoint`, dlclose, abort.
3. **Read & deserialize the manifest** by calling
   `glibre::types::deserialize<PluginManifest>(std::span{ptr,
   size})`. Failure → `core::Error::PluginManifestInvalid`,
   dlclose, abort.
4. **Hash check**: compare `manifest.abi_hash` (and the redundant
   `glibre_plugin_abi_hash` exported symbol — both must agree, both
   must equal the host's `glibre_types_abi_hash()`). Mismatch →
   `core::Error::PluginAbiHashMismatch`. The redundant check
   catches a malformed manifest whose abi_hash field disagrees
   with the compiled-in symbol. dlclose, abort.
5. **Engine version check**: the host's compiled-in
   `glibre_core_version` ≥ `manifest.min_engine_version`. Failure
   → `core::Error::PluginEngineTooOld`. dlclose, abort.
6. **Name collision check**: refuse if a plugin with the same
   `name` is already registered with a different file path. →
   `core::Error::PluginNameCollision`. dlclose, abort.
7. **Dependency resolution**: every entry in `depends_on` must
   already be registered. Missing → `core::Error::
   PluginDependencyMissing`. dlclose, abort. (Multi-plugin loads
   in one phase-8 pass topologically sort by `depends_on` before
   reaching this step.)
8. **Drain**: at this point the world is already drained (frame-
   phases §8 guarantee). The loader is free to mutate type
   registry, system schedule, pass registry, panel registry.
9. **Register**: invoke `glibre_plugin_register(ctx)`.
   - Success → continue.
   - Returned `std::unexpected(err)` → wrap as
     `core::Error::PluginInitFailed`, *carrying the inner error*
     in `ErrorContext::detail`. Run the loader's compensating
     unregister of anything the plugin partially registered (the
     registries record per-plugin ownership, so this is bounded).
     dlclose, abort.
10. **Schedule rebuild**: the loader recomputes the per-phase
    system schedule from the union of all loaded plugins'
    declared `(reads, writes, after, before)`. A cycle → 
    `core::Error::SystemScheduleCycle`. The loader rolls back
    the last plugin's registration and aborts that single load;
    other plugins keep running.
11. **Migrate**: for each persistent component whose schema bumped
    versions, run the `deserialize<T>` migration path against the
    pre-swap snapshot (already detailed in fory-codegen.md
    §"Migration Mechanic"). Failure → 
    `core::Error::SchemaMigrationFailed`; reverse step 9 (call
    `glibre_plugin_unregister` if exported), then dlclose, abort.
12. **Resume**: phase 8 returns; phase 9 (present) runs; subsequent
    frames see the new plugin live.

Failure handling rule (uniform): every refusal logs through
`glibre::log_error(err, warn)` per error-model.md §"Hot-reload
refusals", leaves the previously-loaded version of the plugin (if
any) live, and the engine continues at the prior frame's behaviour.

## Versioning Rules

Three independent version axes; each moves on its own trigger.

1. **`glibre_types_abi_hash` (blake3 hex string)** — recomputed
   automatically by the codegen tool whenever any schema source
   under `data/schemas/` changes. Bumps every time. Plugins compiled
   against an older hash refuse to load.
2. **`glibre-types.dylib` SONAME** — bumped manually only on a
   layout-breaking schema change (struct reorder, removed field
   without `reserved`, type substitution). Pure additive schema
   evolutions keep SONAME but bump the hash. The dynamic linker
   rejects mismatched SONAMEs before our hash check ever runs;
   the hash check covers the smaller hash-changes-but-SONAME-stable
   window.
3. **`PluginManifest.version` (SemVer)** — owned by the plugin
   author. Bumped per the plugin's own release cadence and
   independent of (1) and (2). The engine does not interpret this
   field beyond logging it; it surfaces in editor UI and crash
   reports.
4. **`PluginManifest.min_engine_version` (SemVer)** — set by the
   plugin author to the lowest `glibre-core` it tolerates. Engine
   bumps its own SemVer per error-model.md additions, frame-phase
   schedule changes, or `PluginContext` shape changes.

The loader compares (1) for ABI gating, (2) is enforced by dyld,
(4) is checked manually, and (3) is informational. The three never
collapse into one number; they answer different questions.

## Failure Modes → core::Error

Every refusal point in the loader sequence maps to exactly one
`core::Error` arm. Per error-model.md §"Type Sketch" the variant is
extended monotonically; the new arms below are added under
`namespace core`:

| Loader step | Failure                                 | core::Error arm                  |
|-------------|-----------------------------------------|----------------------------------|
| 1           | `dlopen` returns null                   | `PluginDlopenFailed`             |
| 2           | required symbol missing                 | `PluginMissingEntryPoint`        |
| 3           | manifest blob fails Fory deserialize    | `PluginManifestInvalid`          |
| 4           | `abi_hash` mismatch (manifest or symbol)| `PluginAbiHashMismatch`          |
| 5           | engine older than `min_engine_version`  | `PluginEngineTooOld`             |
| 6           | duplicate plugin `name`                 | `PluginNameCollision`            |
| 7           | unmet `depends_on` entry                | `PluginDependencyMissing`        |
| 9           | `register()` returned `unexpected(...)` | `PluginInitFailed`               |
| 10          | system schedule cycle                   | `SystemScheduleCycle`            |
| 11          | migration step failed or chain missing  | `SchemaMigrationFailed`          |
| any         | refusal observed by hot-reload barrier  | `HotReloadRefused` (wraps inner) |

`PluginAbiHashMismatch`, `SchemaMigrationFailed`, `HotReloadRefused`
already appear in the error-model.md sketch; the rest are new arms
this decision adds. Every arm rides the same `glibre::Error`
struct with `ErrorContext` carrying the plugin path, the offending
hash, or the inner error string.

## Rationale

- **One dylib per plugin, one middleman to link.** Two collapsing
  requirements (ABI gating, type-sharing) become one primitive: a
  single hash on a single shared library. Plugins cannot accidentally
  link the wrong copy because there is only one.
- **Manifest is data, not code.** Fory-serialized in `.rodata`
  means the loader can read every plugin's declared surface area
  *before* running any plugin C++ code. This makes the hash check,
  dependency resolution, and schedule build pure data operations
  on inert bytes. A malicious or corrupt plugin cannot run code
  before the gate.
- **`std::expected<void, glibre::Error>` for `register`.** The
  error-model decision already mandates this shape at every public
  boundary; reusing it here makes plugin init a normal
  return-value error path, not a special case.
- **Three independent version axes.** Conflating the ABI hash with
  plugin SemVer would force a plugin patch release every time the
  engine touched any unrelated schema. Splitting them lets each
  axis answer one question (Are we wire-compatible? / What
  semantics did the plugin ship? / Is the engine new enough?) and
  collapses the three into a single load decision at step 4–5.
- **Refusal at phase 8 is logged, not fatal.** error-model.md
  §"Hot-reload refusals" already pinned this; this record extends
  the rule to every step in the loader sequence. Stale-but-working
  is always preferable to half-loaded.

## Consequences

- The codegen tool must learn `plugin.fory` files in addition to
  schema files. Build-graph note for the plan that lands the
  loader: `glibre-foryc` becomes the producer of both
  `glibre-types.dylib` *and* per-plugin `manifest.cpp`. Acceptable
  cost; the tool already iterates over `.fory` inputs.
- Every plugin gains a `plugin.fory` source file at its crate root.
  The plan that splits the existing prototype scaffold into
  per-domain plugins (`render/`, `physics/`, …) creates these.
- The `core::Error` enum grows seven new arms. Per the error-model
  composition rules, each arm is leaf-level and has a stable
  `to_string` mapping. Tests under `tests/core/loader/` cover the
  full table above (one Catch2 case per row, exercising the failure
  via a fixture-built malformed plugin).
- The plugin loader becomes the single point that composes all
  upstream decisions: it reads the manifest (fory-codegen),
  produces structured errors (error-model), and runs only at
  phase 8 (frame-phases). No other module in core orchestrates
  these three.
- Editor UI gains a "loaded plugins" panel from the manifest
  fields essentially for free; a future editor plan consumes the
  registry without touching the loader.

## Open Questions

1. **Symmetric unregister: optional in MVP, mandatory at hot-reload.**
   The MVP loader supports unload only via process restart. The
   first hot-reload plan will make `glibre_plugin_unregister`
   mandatory and decide whether the registries should keep
   per-plugin ownership records as a `std::vector` or a tagged
   side-table. Defer.
2. **Cross-plugin direct symbol use** is forbidden in this record,
   but a future scripting-plugin spike may need an explicit cross-
   plugin function-table hop (e.g. a render plugin exposing GPU
   resources to a UI plugin). If that need lands, it goes through
   a typed channel registered with the type registry, not via
   `dlsym` between plugins. Confirm at scripting-plugin spike.
3. **`PluginContext` struct stability** — adding fields is
   additive (new methods, new references appended), but the
   struct's binary layout is part of the engine ABI that
   `min_engine_version` guards. The plan that lands the loader
   must decide whether `PluginContext` is a class with a stable
   v-table or a POD-like aggregate. Lean toward POD aggregate to
   keep the `extern "C"` boundary clean, but verify with a
   prototype.
4. **Manifest Fory schema versioning** — `PluginManifest` itself
   is a Fory-versioned schema and may evolve. Migration of the
   manifest type uses the same mechanism as any other schema
   (fory-codegen.md §"Migration Mechanic"); no special-casing
   needed, but call it out in the plan.
5. **Code-signing / notarization on macOS** — out of scope for
   this record. The loader treats unsigned plugins identically;
   distribution policy for shipped plugins is a release-engineering
   decision, not an ABI one.
