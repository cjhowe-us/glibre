# Plugin-Loader Detailed Design

> Detailed design for the `core` context's plugin-loader aggregate
> (`Plugin` / `PluginLoader` / `Manifest` / `AbiHash`, SPEC §4.5).
> Refines §5.9 / §6.6 of `specs/core/SPEC.md` and the
> `reviews/decisions/plugin-abi.md` decision record.
> All conclusions re-derived; harmonius prior art (`harmonius/docs/
> requirements/core-runtime/plugin-system.md`,
> `harmonius/docs/design/core-runtime/events-plugins.md`) cited as
> research input only.

Refs: spike #704 — `[SPIKE] design-core-plugin-loader-detailed`.
Parent sub-epic #699. Sibling task-breakdown spike blocked-by this
deliverable.

## 1. Purpose

`PluginLoader` is the single component in `glibre-core` permitted to
call `dlopen` / `dlsym` / `dlclose` and to mutate the four engine-wide
registries (`TypeRegistry`, `SystemRegistry`, `PassRegistry`,
`PanelRegistry`). Its one responsibility is **admitting a `.dylib`
across the ABI seam**: read the manifest as inert bytes, gate on
`AbiHash` + engine-version + name + dependency, run the plugin's
`glibre_plugin_register` exactly once, and rebuild the schedule with
the new plugin's declared access set. On any refusal it leaves the
loaded set byte-equal to the pre-call snapshot.

What the loader explicitly refuses to own:

- The hot-reload state machine (drain → swap → migrate → resume) —
  owned by `HotReloadBarrier` (§4.6, hot-reload-protocol.md). The
  loader supplies `dlopen` and the registration call; the barrier
  composes them with drain + migrate.
- Schema migration bodies — owned by the originating bounded context
  per `fory-codegen.md` §"Migration Mechanic"; called from
  `glibre-types.dylib`'s static migration table during step 11 of the
  loader sequence.
- Mid-frame admission — `PluginLoader::load` mid-frame is queued and
  run at `Phase::HotReload` (PHILOSOPHY §8; frame-phases.md).
- Plugin groups, capability advertisement, runtime reflection —
  refused (PHILOSOPHY §6, §4.5 invariant 6, R-1.6.2 / R-1.6.8 routed
  to §3.3).

The loader's SRP boundary is sharp: if the `dlopen` flags, the manifest
schema, the entry-point set, the hash-gate algorithm, the dependency
resolution rule, the schedule-rebuild trigger, or the registry-rollback
discipline change, this design changes. Anything else is out of scope.

## 2. Requirements Coverage

Mapping of harmonius requirements (`R-1.6.*`,
`harmonius/docs/requirements/core-runtime/plugin-system.md`) to glibre
MVP refusal-or-coverage. Every entry is independently re-derived.

| Harmonius req                     | Glibre disposition (MVP) | Coverage site                                                                                |
|-----------------------------------|--------------------------|----------------------------------------------------------------------------------------------|
| R-1.6.1 plugin trait + auto-order | **Covered**              | `PluginManifest.depends_on` (plugin-abi.md), topological load (§3.4 below; §4.5 inv. #3).    |
| R-1.6.2 plugin groups, disable    | **Refused (deferred)**   | §4.5 invariant #6; manifest carrying groups is rejected. Group composition is editor-tooling territory, not loader.       |
| R-1.6.3 declare deps + conflicts  | **Partial — deps yes, conflicts deferred** | `depends_on` + `PluginDependencyMissing` (§3.4). Conflicts (mutual-exclusion) deferred until first concrete need.|
| R-1.6.4 topo sort, cycles, full chain on errors | **Covered**    | §3.4 algorithm; `PluginDependencyCycle`; error-model.md `ErrorContext::detail` carries the chain.|
| R-1.6.5 unload + reload, preserve state, dlopen/dlclose | **Covered (loader half)** | `PluginLoader::load` / `unload` (§4.2); reload routes through `HotReloadBarrier`. State preservation is barrier-side per hot-reload-protocol.md.|
| R-1.6.6 reload < 2s, preserve 100% state, type-named migration error | **Covered** | Migration owned by barrier (hot-reload-protocol.md §3); `SchemaMigrationFailed` carries the failing type's FQN in `ErrorContext::detail`. |
| R-1.6.7 SemVer + ABI hash gate    | **Covered**              | `PluginManifest.version` + `min_engine_version` + `abi_hash` (plugin-abi.md §"Versioning Rules"); §3.5 below.|
| R-1.6.8 capability negotiation    | **Refused**              | §4.5 invariant #6. Capabilities are reflective-style runtime queries; PHILOSOPHY §6 forbids runtime reflection in shipping. The middleman type registry is the static substitute. |
| R-1.6.9 single middleman dylib    | **Covered**              | `glibre-types.dylib` per fory-codegen.md; loader gates on its hash.                          |
| R-1.6.10 shipping static-link     | **Out of scope (loader)**| The loader is the dynamic-link path used by editor + dev runtime. Shipping static-link is a separate build-system story (post-MVP); the §5 surface stays unchanged because static-linked builds compile out the loader's `dlopen` body and route registration through a generated `register_static_plugins()` call site.|

Coverage rule: every harmonius requirement above either lands in this
design or is refused with a one-line rationale. No silent drops.

Glibre-native requirements added beyond harmonius:

- **Refusal at every step is logged once and leaves the world byte-equal
  to the pre-call snapshot** (PHILOSOPHY §9 + error-model.md). The
  rollback discipline is §6 + §10.
- **Manifest is data not code** — read before any plugin C++ runs
  (plugin-abi.md §"Rationale"). A malicious or malformed manifest
  cannot execute plugin code before the gate. This collapses
  R-1.6.7 (ABI hash) and R-1.6.3 (declare deps + conflicts) into one
  primitive: a Fory blob in `.rodata`.
- **Three independent version axes** — `glibre_types_abi_hash` (auto),
  middleman SONAME (manual on layout-break), `PluginManifest.version`
  (per-plugin SemVer). Plugin-abi.md §"Versioning Rules" locked this;
  this design transcribes the loader's checks (§3.5).

## 3. Detailed Model

### 3.1 Aggregate composition

```text
PluginLoader (root, owned by core)
├── std::pmr::vector<LoadedPlugin>  loaded_     (registration order)
├── std::pmr::unordered_map<eastl::string, PluginId>  by_name_  (collision check)
├── TypeRegistry&    types_      (ref obtained at construction)
├── SystemRegistry&  systems_    (ref)
├── PassRegistry&    passes_     (ref)
├── PanelRegistry&   panels_     (ref)
├── HotReloadBarrier& barrier_   (ref; for reload routing)
└── LogSink&         log_        (spdlog-backed)
```

Each `LoadedPlugin` is the loader's per-dylib value object (§3.3). The
loader is owned by the `World`'s startup composer; one loader instance
per `World`.

### 3.2 `PluginManifest` (Fory schema)

Authored as `plugins/<name>/plugin.fory` and codegenned by
`glibre-foryc` into a `.rodata` blob baked into the plugin's
translation unit (plugin-abi.md §"Manifest source-of-truth"). The
schema (locked verbatim from plugin-abi.md §"Plugin Manifest Schema"):

```fory
schema glibre.core.PluginManifest {
  version 1
  since   "0.1.0"

  field name              : string             tag 1 since 1
  field version           : SemVer             tag 2 since 1
  field abi_hash          : string             tag 3 since 1   # 64-char blake3 hex
  field min_engine_version: SemVer             tag 4 since 1
  field components        : list<ComponentDecl> tag 5 since 1
  field systems           : list<SystemDecl>    tag 6 since 1
  field passes            : list<PassDecl>      tag 7 since 1
  field panels            : list<PanelDecl>     tag 8 since 1
  field depends_on        : list<string>        tag 9 since 1
}
```

Plus the four sub-schemas (`SemVer`, `ComponentDecl`, `SystemDecl`,
`PassDecl`, `PanelDecl`) defined in plugin-abi.md verbatim. Field
semantics:

- `name` — fully-qualified plugin id (e.g. `glibre.render`). Unique
  per loaded set. Collision → `PluginNameCollision`.
- `version` — plugin's own SemVer. Informational at the loader; logged.
- `abi_hash` — 64-char lowercase blake3 hex string. **Must equal**
  the host's `glibre_types_abi_hash()` AND the plugin's redundant
  `glibre_plugin_abi_hash` exported symbol. Both checks at step 4.
- `min_engine_version` — minimum `glibre-core` SemVer. Below →
  `PluginEngineTooOld`.
- `components` / `systems` / `passes` / `panels` — declarative
  registration manifest; loader uses these to drive the registries
  during step 9 (register) and step 10 (schedule-rebuild). The plugin
  may register only items declared here; surplus registrations during
  `glibre_plugin_register` are refused as `PluginInitFailed` with a
  diagnostic detail.
- `depends_on` — list of `PluginManifest.name` strings that must be
  loaded before this plugin's register runs. Topologically sorted.

Manifest authoring rule: **`plugin.fory` is the single source of
truth**. The plugin author never hand-writes the blob; `glibre-foryc`
emits a generated `manifest.cpp` translation unit that the plugin's
build links into the dylib (plugin-abi.md §"Manifest source-of-truth").
Build-system rule: a plugin whose source registers a component fqn
absent from `manifest.components` fails at `glibre_plugin_register`
with `PluginInitFailed` (detail: `surplus-registration: <fqn>`).

### 3.3 `LoadedPlugin` (internal value object)

```cpp
// core/src/plugin/loader.hpp — internal.
struct LoadedPlugin {
    PluginId          id;                  // monotonic, never reused.
    eastl::string     fqn;                 // PluginManifest.name (deep-copied).
    std::filesystem::path  dylib_path;     // absolute, canonical.
    void*             dl_handle;           // RTLD_NOW | RTLD_LOCAL handle.

    // dlsym'd at load; index by EntryPoint enum.
    eastl::array<void*, 4>  entry_points;

    // Deserialized once at load. Survives the call so the plugin can
    // reread its own declared items via PluginContext::manifest.
    glibre::types::PluginManifest  manifest;

    // Per-plugin ownership records (rollback ledger). Each entry is
    // populated by the matching registry when register() touches it;
    // unload / refusal walks each list in reverse.
    std::pmr::vector<TypeId>   owned_types;
    std::pmr::vector<SystemId> owned_systems;
    std::pmr::vector<PassId>   owned_passes;
    std::pmr::vector<PanelId>  owned_panels;
};

enum class EntryPoint : std::size_t {
    AbiHash       = 0,   // glibre_plugin_abi_hash
    ManifestPtr   = 1,   // glibre_plugin_manifest
    ManifestSize  = 2,   // glibre_plugin_manifest_size
    Register      = 3,   // glibre_plugin_register
};
```

Notes on container choice:

- `eastl::string` / `eastl::array` — runtime data structures per
  PHILOSOPHY §11 (no `std::string` / `std::array` in runtime data).
- `std::filesystem::path` — kept on the `std::` side because PHILOSOPHY
  §11 explicitly retains `std::filesystem`.
- `std::pmr::vector` for ownership ledgers — uses the per-context
  allocator (perf-budget.md §"Allocator Rules"); allocations tagged
  `ContextTag::core`.

### 3.4 `PluginContext` (passed to `glibre_plugin_register`)

```cpp
// core/include/glibre/core/plugin_api.hpp — public.
namespace glibre::core {

struct PluginContext {
    World&            world;
    TypeRegistry&     types;
    SystemRegistry&   systems;
    PassRegistry&     passes;
    PanelRegistry&    panels;
    const glibre::types::PluginManifest&  manifest;
    LogSink&          log;
};

// Plugin entry-point shape (extern "C" on the wire; the std::expected
// crosses only the middleman boundary, both sides built against the
// same libc++ that built glibre-types.dylib).
extern "C" std::expected<void, glibre::Error>
glibre_plugin_register(PluginContext& ctx) noexcept;

}  // namespace glibre::core
```

Locked from plugin-abi.md §"Registration Entry-Point Signature".
`PluginContext` is a POD-like aggregate (see §12 [OPEN] #1) of
references; it is **not** owned by the plugin and may not outlive the
call. The loader destroys it after `register` returns; cached pointers
are dangling.

### 3.5 Loader sequence (re-stated, transcribed from plugin-abi.md)

The full algorithm of `PluginLoader::load(manifest_path)`:

1. **Resolve dylib path.** From `manifest_path` (a directory containing
   `plugin.fory` + the built `.dylib`), compute the canonical absolute
   path of the `.dylib` file. Failure → `PluginManifestInvalid`
   (detail: `dylib-not-found`).
2. **`dlopen(path, RTLD_NOW | RTLD_LOCAL)`.** `RTLD_NOW` forces
   immediate symbol resolution (missing middleman symbols surface here,
   not later). `RTLD_LOCAL` keeps plugin symbols out of the global
   namespace. Failure → `PluginDlopenFailed`, attach `dlerror()` text
   to `ErrorContext::detail`. Abort.
3. **`dlsym` the four entry points** (§3.3 `EntryPoint` enum). Any
   missing symbol → `PluginMissingEntryPoint`, `dlclose`, abort.
4. **Read manifest.** Call `glibre_plugin_manifest()` (returns
   `const std::byte*`) and `glibre_plugin_manifest_size()`. Pass the
   resulting `std::span<const std::byte>` to
   `glibre::types::deserialize<PluginManifest>`. Failure →
   `PluginManifestInvalid`, `dlclose`, abort.
5. **ABI-hash gate (double check).** Compare:
   - `manifest.abi_hash` (string field, 64 chars)
   - `glibre_plugin_abi_hash()` (exported C string)
   - `glibre_types_abi_hash()` (host's compiled-in value)

   All three must be byte-equal. Any mismatch →
   `PluginAbiHashMismatch`, `dlclose`, abort. (The redundant pair
   catches a forged manifest whose `abi_hash` field disagrees with the
   compiled-in symbol.)
6. **Engine-version gate.** Compare host's `glibre_core_version`
   against `manifest.min_engine_version` (lexicographic SemVer).
   Below → `PluginEngineTooOld`, `dlclose`, abort.
7. **Name-collision gate.** If `by_name_` already contains
   `manifest.name` mapped to a `LoadedPlugin` with a different
   `dylib_path`, refuse. Same name + same path is idempotent (a no-op
   success returning the existing `PluginId`). →
   `PluginNameCollision`, `dlclose`, abort.
8. **Dependency-resolution gate.** For every `dep` in
   `manifest.depends_on`: if `dep` is not present in `by_name_` → cache
   (`pending_` queue) and return `PluginDependencyMissing` only if no
   batch resolution is in progress; otherwise the batched
   topological-sort path (§3.6) handles ordering. Cycles in the
   intra-batch dependency graph → `PluginDependencyCycle`, `dlclose`
   every member of the failing cycle, abort the batch.
9. **Allocate `PluginId`** (next monotonic value; never reused).
   Construct `LoadedPlugin` with empty ownership ledgers. Append to
   `loaded_`; insert into `by_name_`.
10. **Invoke `glibre_plugin_register(ctx)`.** Build the `PluginContext`
    on the loader thread's stack pointing at the four registries, the
    deserialized manifest, the world, and the log sink. Each registry
    in turn appends to the matching `LoadedPlugin::owned_*` ledger as
    the plugin calls `register_*` on it. Failure (returned
    `unexpected(err)`) → `PluginInitFailed`, attach the inner error to
    `ErrorContext::detail`, run **compensating unregister** (§6.2),
    `dlclose`, abort.
11. **Schedule rebuild.** Recompute the per-phase system DAG from the
    union of every `LoadedPlugin::manifest.systems` (declared
    `(reads, writes, after, before)`). Cycle → `SystemScheduleCycle`;
    compensating unregister of the just-loaded plugin; `dlclose`;
    abort.
12. **Schema migration trigger** (only if this `load` was invoked from
    the hot-reload barrier — see §8). For every persistent component
    type whose schema bumped between the surviving storage's recorded
    version and the new plugin's `manifest.components[].schema_hash`,
    invoke the migration path (`glibre-types.dylib`'s static
    migration table, hot-reload-protocol.md §3). Failure →
    `SchemaMigrationFailed`. The barrier — not the loader — owns
    rollback in this case (§8.5 of SPEC + §6.3 below).
13. **Success.** Return the `PluginId`. The plugin's systems may
    execute starting at the next frame's `Phase::Input`.

Loader-sequence invariants:

- **No plugin C++ runs before step 10.** Manifest read, hash check,
  engine-version, name, deps, all run against inert data. Refusal
  before step 10 means the world is untouched.
- **Step 10 mutations are reversible** by walking the four ownership
  ledgers in reverse. The registries support
  `unregister_<thing>(<id>)` for every register call they admit; this
  is what makes step 10's "Refuse" recoverable into a clean rollback.
- **Steps 11–12 mutations are reversible** by step-10's reverse plus
  re-running the previous schedule build (cached as the prior
  successful compile output) and resetting the migration arena.

### 3.6 Multi-plugin batched load

`PluginLoader::load_batch(span<path>)` (internal, exposed via
`HotReloadBarrier` and the editor's "load all plugins" startup path)
runs steps 2–4 for every member, then performs one topological sort
over the union dependency graph, then runs steps 5–13 in topological
order. A cycle aborts the entire batch (no partial admission); a
single member's refusal at steps 5–13 aborts only that member, with
its dependents (downstream nodes) refused as
`PluginDependencyMissing` (their predecessor was rejected). Other
plugins in the batch with no broken-dependency path complete normally.

Algorithm:

```
let manifests = parallel { for p in batch: dlopen + dlsym + read manifest }
let graph     = build_directed_graph(manifests, edge="depends_on")
if graph.has_cycle(): refuse batch with PluginDependencyCycle (path in detail)
let order     = topological_sort(graph)
for m in order:
    if any predecessor of m was refused this batch:
        refuse m with PluginDependencyMissing (detail: predecessor fqn)
        continue
    run steps 5..13 for m
```

The parallel dlopen/dlsym/manifest-read in step 1 is a perf
optimization for cold-start (16+ plugins on a sample editor session);
it is safe because step 1 mutates only per-thread loader scratch
memory until the topological sort serializes onto the loader thread.

### 3.7 `dlopen` / `dlclose` lifecycle

The loader is the **only** holder of dylib handles. Handle lifetime
spans from step 2 (admit) to either:

- step's-abort `dlclose` (refusal path; handle dropped), or
- explicit `unload(PluginId)` (operator path; runs the compensating
  unregister, then `dlclose`), or
- process exit (no `dlclose`; the OS reclaims).

`dlclose` is called at most once per handle. The barrier-driven
hot-reload swap drops the *outgoing* plugin's handle exactly once,
at the swap's commit point (hot-reload-protocol.md §"Step 2 — Swap").

`RTLD_LOCAL` plus the §4.5 invariant "no cross-plugin direct symbol
use" plus the build-system rule "plugins link only `glibre-types.dylib`"
together guarantee that no plugin can resolve another plugin's
symbols. Cross-plugin communication is exclusively through middleman
types and the registries.

## 4. Public Surface

The §5.9 stub from `specs/core/SPEC.md` is authoritative. This section
restates it with the design's per-method behaviour annotations.

### 4.1 Types (locked from §5.9)

```cpp
namespace glibre::core {

struct PluginId {
    std::uint64_t value{};
    friend constexpr bool operator==(PluginId, PluginId) noexcept = default;
};

struct LoadedPlugin {
    PluginId          id{};
    std::string_view  name{};        // borrowed from loader storage
    std::string_view  abi_hash{};    // 64-char blake3 hex
    std::string_view  dylib_path{};
};

}  // namespace glibre::core
```

Borrow rules: every `string_view` in `LoadedPlugin` is valid for the
duration of the `list()` call **and** until the next `load` / `unload`
that mutates `loaded_`. Callers that need stable strings copy.

### 4.2 `PluginLoader` operations

```cpp
class PluginLoader {
public:
    [[nodiscard]] static Result<PluginLoader*>
    create(World& world, HotReloadBarrier& barrier) noexcept;
    static void destroy(PluginLoader* loader) noexcept;

    [[nodiscard]] Result<PluginId>
    load(const std::filesystem::path& manifest_path) noexcept;

    [[nodiscard]] Result<void> unload(PluginId id) noexcept;

    [[nodiscard]] std::span<const LoadedPlugin> list() const noexcept;

protected:
    PluginLoader() noexcept;
    ~PluginLoader();
};
```

Per-method contract:

- **`create`** — allocates the loader, captures references to the four
  registries via `world.registries()`, captures the barrier ref. May
  return `OutOfBudget` if the `core` 64 MiB cell is exhausted; returns
  the loader pointer otherwise. Single-threaded; called once at
  startup.
- **`load`** — runs §3.5's loader sequence. Mid-frame caller (i.e.
  `current_phase != Phase::HotReload`): the call is enqueued onto the
  barrier as a `request_reload`-equivalent; it returns
  `PluginId(0)` plus a `pending` flag set in `LoadedPlugin::id`'s
  high bit (decoded by `list()`)? **Resolved**: the public API
  returns the eventual `PluginId` synchronously when called during
  startup (no frame in progress) and refuses with
  `core::Error::FramePhaseMisordered` mid-frame; the editor's
  filesystem-watcher path goes through `HotReloadBarrier::request_reload`
  directly, not through `PluginLoader::load`. (See §6 concurrency.)
- **`unload`** — runs the compensating unregister of `id` (§6.2),
  `dlclose`s the handle, removes from `loaded_` and `by_name_`. Mid-
  frame: refused with `FramePhaseMisordered`; queue via barrier.
  Refuses with `PluginDependencyMissing` (detail: dependent plugin
  fqn) if any other loaded plugin lists `id`'s name in its
  `depends_on` — no orphan dependencies allowed.
- **`list`** — `O(N)` snapshot of `loaded_` projected onto
  `LoadedPlugin`. Returns a borrowed span; lifetime per §4.1.
  Read-only; safe to call from any thread.

### 4.3 Public ABI surface (extern "C")

The plugin-side exports — **never** `std::*` containers, never
`eastl::*` containers (PHILOSOPHY §11 "Public plugin ABI surfaces never
expose `std::` containers or `eastl::` containers — they cross the
boundary as POD spans / handles only"):

```cpp
extern "C" {

// 32-byte blake3, hex-encoded, 64 chars + null. Static lifetime.
const char* glibre_plugin_abi_hash() noexcept;

// Pointer + size of the Fory-serialized PluginManifest blob in
// .rodata. Static lifetime. Read once at load.
const std::byte* glibre_plugin_manifest()      noexcept;
std::size_t      glibre_plugin_manifest_size() noexcept;

// Registration entry. std::expected crosses only the middleman
// boundary; both sides compile against the same libc++ that built
// glibre-types.dylib (fory-codegen.md §"Open Questions" #4).
std::expected<void, glibre::Error>
glibre_plugin_register(glibre::core::PluginContext& ctx) noexcept;

}  // extern "C"
```

Surface rules:

- `noexcept` on every export. Throwing across the C ABI is undefined;
  exception-aware third-party code (Fory, Jolt, FBX SDK) must be
  wrapped at its first ingress point (error-model.md §"Consequences").
- No runtime reflection in shipping (PHILOSOPHY §6). Manifest fields
  describe the plugin's static surface; the loader and editor read
  these declarative fields. Plugins do not query "what types are
  registered" at runtime; they consume `PluginContext::manifest`.
- Symmetric `glibre_plugin_unregister` is **optional in MVP**, **mandatory
  once hot-reload migrations land** (plugin-abi.md §"Open Questions"
  #1). Hot-reload depends on it; first-load + simple-unload do not.

## 5. Hot / Cold Path Split

Plugin admission is exclusively a **cold path** by design:

| Path  | Trigger                               | Frequency                     | Budget                                          |
|-------|---------------------------------------|-------------------------------|-------------------------------------------------|
| Cold  | `PluginLoader::load` startup          | Once per plugin per session   | 0–N seconds; not in steady-state frame budget   |
| Cold  | `PluginLoader::load` operator (editor)| Tens per session              | Same                                            |
| Cold  | `HotReloadBarrier::step` reload frame | Per filesystem event / save   | ≤0.40 ms (perf-budget.md "hot-reload frame")    |
| Hot   | `HotReloadBarrier::step` idle         | Every frame (60 Hz)           | <0.10 ms steady-state (perf-budget.md phase 8)  |
| Hot   | `PluginLoader::list`                  | Editor inspector refresh      | O(N), <0.05 ms for MVP N ≤ 16                   |

Hot-path invariants (the loader's contribution to the every-frame budget):

- The barrier's no-reload-pending fast path is a **single relaxed
  atomic load** on `pending_reloads` (hot-reload-protocol.md §"Decision";
  perf-budget.md §"Pipelined Frame Timing"). The loader contributes
  zero memory traffic to that load — its state is read only when the
  counter is non-zero.
- `PluginLoader::list` is read-only; the underlying `loaded_` vector
  changes only at frame-boundary admit/unload, so callers from `tools`
  / editor see a consistent snapshot without locking (single-writer
  multi-reader; the writer lives on the loader thread, readers on the
  editor render thread).

Cold-path invariants:

- `dlopen` is called only on cold paths — never on the every-frame path.
- Manifest deserialization (Fory) is called only on cold paths — Fory
  itself is forbidden inside `glibre-core` (§3.5 step 4 calls into
  `glibre-types.dylib`'s `deserialize<PluginManifest>` which has Fory
  as a private dependency).
- Schedule rebuild is cold-path only; the per-frame schedule reads a
  pre-compiled `CompiledFrame` (SPEC §6.4).

The cold/hot split is what makes the loader's budget cell
(`core` 0.40 ms sim + 0.05 ms submit, perf-budget.md) **independent of
plugin count**. Plugin count grows the cold-path duration linearly;
the hot path stays sub-microsecond.

## 6. Concurrency

The loader runs **single-threaded on the game-loop driver thread**
during `Phase::HotReload` (frame phase 8) and during world startup.
This is the only execution model.

### 6.1 Threading rules

1. **Loader-mutating calls run only at phase 8 or pre-frame.**
   `PluginLoader::load`, `unload`, and `HotReloadBarrier::request_reload`
   may be called from any thread, but their effect (registry mutation,
   `dlopen`, schedule rebuild) is queued and applied on the loader
   thread at phase 8. Callers from other threads receive a
   `ReloadRequestId` to poll. (This is the rule that resolves
   `PluginLoader::load` mid-frame's behaviour from §4.2: the public
   API enqueues, the work runs at phase 8.)

2. **Reads are lock-free.** `PluginLoader::list` reads a
   `std::pmr::vector<LoadedPlugin>` whose mutating writes are confined
   to the loader thread. The vector's storage pointer is loaded with
   `std::memory_order_acquire` on read; writes publish with
   `std::memory_order_release`. Callers see either the pre-mutation
   snapshot or the post-mutation snapshot — never a torn intermediate.

3. **Phase 8 is exclusive.** No system body runs during phase 8
   (frame-phases.md). The loader has implicit exclusive access to the
   World, the registries, and the schedule for the duration of the
   barrier step. No locks are needed inside the loader sequence;
   correctness derives from the schedule's exclusion guarantee.

4. **Filesystem-watcher integration.** The editor (or dev runtime) may
   spawn a filesystem watcher thread that detects rebuilt `.dylib`
   files and calls `HotReloadBarrier::request_reload(fqn, path)`. The
   watcher thread never touches the loader directly; the barrier's
   request queue is an MPSC ring (single consumer = loader thread).

### 6.2 Compensating unregister discipline

A `LoadedPlugin`'s ownership ledgers (`owned_types`, `owned_systems`,
`owned_passes`, `owned_panels`) are populated by the four registries
during step 10 of the loader sequence. On step-10 / step-11 / step-12
failure, the loader walks each ledger in reverse and calls
`unregister_*(id)` on the matching registry. Each registry's
`unregister` is the strict inverse of `register` (every state mutation
is undone in reverse order); the migration arena from step 12 is
reset; the prior compiled schedule is restored from the
last-successful cache.

The discipline produces byte-equal pre-call state on every refusal —
this is the §1 / §10 invariant tested in §11.

### 6.3 Interaction with the hot-reload barrier

The barrier is the loader's **caller** during phase 8, not the
loader's owner. The barrier:

1. Drives drain (calls outgoing plugin's `glibre_plugin_drain`).
2. Calls `PluginLoader::load(replacement_manifest_path)` which runs
   §3.5 steps 2–11 against the candidate dylib.
3. On success, swaps the live plugin's `LoadedPlugin` slot with the new
   one (vtable replacement is a single relaxed store under the loader
   thread's exclusive ownership of phase 8).
4. Calls the migration table for every persistent component type
   whose schema bumped (step 12 of §3.5 + hot-reload-protocol.md
   §"Step 3 — Migrate").
5. On any of (1)–(4) failing, calls `PluginLoader::rollback(<txn>)`
   which restores the prior `LoadedPlugin` and re-runs its
   `glibre_plugin_register` (idempotent per plugin-abi.md §"Decision"
   rule for register).

The loader exposes `rollback` only to the barrier — it is **not** in
the §4.2 public surface. The barrier-loader seam is intentional and
narrow; it is the single coupling point between the loader aggregate
and the hot-reload aggregate.

## 7. Persistence + ABI

### 7.1 Manifest is Fory-serialized in `.rodata`

The plugin's `manifest.cpp` translation unit (generated by
`glibre-foryc` from `plugin.fory`) embeds the serialized blob via:

```cpp
namespace {
alignas(std::byte) constinit
const std::byte kManifestBlob[] = { /* ... Fory bytes ... */ };
constexpr std::size_t kManifestSize = sizeof(kManifestBlob);
}

extern "C" const std::byte* glibre_plugin_manifest()      noexcept { return kManifestBlob; }
extern "C" std::size_t      glibre_plugin_manifest_size() noexcept { return kManifestSize;  }
```

The blob is in `.rodata`; the loader reads it directly via the symbol
without copying (§3.5 step 4 deserializes from a borrowed
`std::span<const std::byte>`).

`PluginManifest` itself is a Fory-versioned schema (see §3.2). Its
own version evolution follows fory-codegen.md §"Migration Mechanic":
when the manifest schema bumps from v1 to v2, the originating context
(`core`) ships a `migrate_PluginManifest_v1_to_v2(...)` free function;
the deserializer routes through it automatically. This is normal
schema evolution, not loader-special-cased (§12 [OPEN] #2).

### 7.2 `AbiHash` sources

The `glibre_types_abi_hash()` value is a **single blake3 digest** over
the canonical-ordered concatenation of every Fory schema's
`(fqn, version, schema_source_blake3)`. Sources, in order:

1. Every `data/schemas/<context>/*.fory` file's source SHA (i.e. the
   raw bytes of the .fory file, blake3-hashed). This includes
   `core/PluginManifest.fory` itself.
2. The schema's declared `version` integer (little-endian, 4 bytes).
3. The schema's `fqn` (UTF-8 bytes, no length prefix; entries
   separated by `\n`).

Concatenation order is canonical Unicode code-point order over
`fqn`. The 32-byte digest is hex-encoded (lowercase, 64 chars) and
embedded as a string literal in `glibre/types/abi_hash.hpp`. Plugins
re-export the identical string under `glibre_plugin_abi_hash`,
captured at the plugin's compile time. Hash equality is byte-string
equality of the hex form; no parsing required at load.

**What is NOT in the hash:** header timestamps, compiler identity,
optimization flags, `__DATE__`/`__TIME__`. The hash is a property of
the **contract**, not the build environment. This is what makes two
plugins built on different machines compatible if and only if they
share the same schema set.

### 7.3 ABI stability rules (transcribed from fory-codegen.md §"ABI Stability Rules")

1. Generated structs are tag-sorted ascending; layout independent of
   schema authoring order.
2. Generated structs are `final`, contain only built-in scalars or
   other generated types, no virtuals, no vtables, no user-defined
   ctors beyond `= default`.
3. Adding a field at a new tag is ABI-additive only if it appends past
   the last existing field's offset; otherwise codegen bumps the
   schema's major version and forces a migration. `glibre-foryc`
   asserts this at generation time and fails the build on violation.
4. The middleman dylib's exported C entry points are limited to the
   four enumerated in fory-codegen.md §"ABI Stability Rules" #4.
5. Middleman SONAME is bumped only on layout-breaking schema changes;
   minor schema additions keep SONAME but bump the embedded hash. The
   plugin loader's hash check catches the latter; the dynamic linker's
   SONAME check catches the former (before the loader's step 5 ever
   runs).

## 8. Hot-Reload Integration

The loader integrates with `HotReloadBarrier` (SPEC §4.6, §8;
hot-reload-protocol.md) as the **swap** half of the four-step state
machine. The complete picture:

| Step       | Owner   | Loader's role                                                                      |
|------------|---------|------------------------------------------------------------------------------------|
| 1. Drain   | Barrier | None directly (barrier calls outgoing plugin's `glibre_plugin_drain`).             |
| 2. Swap    | Barrier | **Loader runs**: §3.5 steps 2–11 for the candidate dylib. Returns new `LoadedPlugin`. |
| 3. Migrate | Barrier | None directly (barrier invokes `glibre-types.dylib` migration table).              |
| 4. Resume  | Barrier | Loader's `glibre_plugin_register` already ran in step 2; barrier publishes events. |

### 8.1 What the loader contributes to each step

**Step 2 (swap):**

- Runs the full §3.5 loader sequence (`dlopen` → `dlsym` → manifest →
  hash check → engine version → name (idempotent: same fqn replacing
  old fqn is allowed iff old fqn is the outgoing plugin) → deps → 
  register → schedule rebuild) on the *candidate* dylib.
- The previously-loaded plugin's `dl_handle` is **not** dropped until
  step 4 commit. If step 2 fails, the candidate's handle is
  `dlclose`d and the previous plugin remains live and linked.
- ABI hash is checked **twice** even on hot-reload: once at first load
  (already enforced) and again here. A plugin that loaded once may
  still fail the recheck if `glibre-types.dylib` was itself reloaded
  in the same phase 8 — this is `core::Error::HotReloadAbiHashMismatch`
  (SPEC §4.6 invariant 3).

**Step 3 (migrate):**

- Loader supplies the per-phase migration arena (16 MiB ceiling per
  perf-budget.md §"Allocator Rules" rule 6) to the migration table.
  The arena is reset between rows; never grows across plugins in the
  same phase.
- Loader does not invoke migrate functions itself — they live in
  `glibre-types.dylib`'s static migration table populated by the
  originating context (fory-codegen.md §"Migration Mechanic").

**Step 4 (resume):**

- Loader does nothing in step 4 itself; `glibre_plugin_register` ran
  during step 2. The barrier publishes
  `HotReloadCompletedEvent` synchronously between barrier-internal
  steps 4.2 and 4.3 (hot-reload-protocol.md §"Observer Notification").

### 8.2 State `migrate(...)` handoff

The `migrate(...)` contract is the originating context's, not the
loader's. The loader's only obligation is to **deliver the candidate
schema set** (via `manifest.components`) and **the surviving storage's
recorded version** (read from the storage's per-archetype version
header, written when the storage was last admitted). The migration
table inside `glibre-types.dylib` matches `(stored_version,
current_version)` and runs the chain.

Failure (missing chain or migrate-step `unexpected`) →
`SchemaMigrationFailed`; loader unwinds via §6.2 + barrier-driven
re-register of the prior plugin (hot-reload-protocol.md §"Failure &
Rollback"). The previous plugin remains live; the engine continues
ticking on the prior code.

### 8.3 Refusal cases (cross-reference)

The full §10.1 table from `specs/core/SPEC.md` is the closed
enumeration of refusals. The hot-reload-relevant subset (every refusal
that can fire under the barrier's umbrella):

- `PluginAbiHashMismatch` — loader step 5 on the candidate. Wrapped
  under `HotReload`, logged at `warn`.
- `HotReloadAbiHashMismatch` — recheck failure between first load and
  swap (§4.6 invariant 3).
- `SchemaMigrationFailed` — barrier step 3.
- `PluginInitFailed` — `glibre_plugin_register` returned `unexpected`
  on the candidate.
- `HotReloadDrainTimeout` — drain budget exceeded (barrier step 1).
- `PluginManifestInvalid` — candidate's manifest fails Fory deser, OR
  candidate dropped a component fqn the prior plugin owned.

Every refusal preserves the prior plugin's code as live. This is
PHILOSOPHY §9 + PHILOSOPHY §8 jointly: refuse on hash mismatch + only
swap at frame boundary = **stale-but-working over half-loaded**.

## 9. Performance

The loader's contribution to per-frame and per-load budgets, locked
against perf-budget.md §"Per-Context Budget Table" + §"Pipelined Frame
Timing":

### 9.1 Per-frame (steady-state, hot path)

| Cell                    | Budget        | Source                               |
|-------------------------|---------------|--------------------------------------|
| `core` CPU sim          | 0.40 ms total | perf-budget.md row `core`            |
|   of which loader phase 8 idle | <0.10 ms | perf-budget.md "phase 8 idle"        |
| `core` CPU submit       | 0.05 ms       | (no loader work in submit phases)    |
| `core` heap (loader's share) | ~4 MiB / 64 MiB | per-plugin LoadedPlugin × ledgers   |

Phase 8 idle path is a single relaxed atomic load on
`pending_reloads`. The loader's own state (`loaded_`, `by_name_`) is
not touched on the idle path. This budget is independent of plugin
count.

### 9.2 Per-load (cold path, one-shot)

| Step | Operation                       | Budget          | Notes                                                       |
|------|---------------------------------|-----------------|-------------------------------------------------------------|
| 2    | `dlopen RTLD_NOW \| RTLD_LOCAL` | ~5–20 ms (dyld) | Driven by macOS dyld; bounded by plugin's static link size. |
| 3    | `dlsym ×4`                      | <0.1 ms         | Hash table lookup in dyld's local namespace.                |
| 4    | manifest deserialize (Fory)     | <0.5 ms         | Fory-decode N ≤ 64 fields + lists; in `glibre-types.dylib`. |
| 5    | hash compare ×3                 | <0.001 ms       | Three 64-byte string compares.                              |
| 6    | engine-version compare          | <0.001 ms       | SemVer triple comparison.                                   |
| 7    | name collision lookup           | <0.05 ms        | `unordered_map::find`, N ≤ 16.                              |
| 8    | dependency topo sort            | <0.5 ms         | N ≤ 16 plugins, edges ≤ N²; Kahn's algorithm.              |
| 10   | `glibre_plugin_register`        | plugin-defined  | Bounded by plugin author; perf-budget.md row of owning ctx.|
| 11   | schedule rebuild                | 1–5 ms          | Bucketed-O(systems) per #493 spike. Plugin-count linear.    |
| 12   | migrate (only on reload)        | ≤0.40 ms        | perf-budget.md "hot-reload frame" cap.                      |

**Total cold-load budget:** dominated by `dlopen` and the plugin's own
`register`. Targeted at <50 ms per plugin; not in steady-state frame
budget.

**Hot-reload frame total:** drain + swap + migrate ≤ 0.40 ms, capped
by perf-budget.md "hot-reload frame budget" (CI-gated). Step 11
(schedule rebuild) is included in that 0.40 ms; if it ever exceeds
~0.20 ms in profiling, the bucketed-O fallback from spike #493 kicks
in.

### 9.3 Per-context cell impact

The loader's heap accounting:

- `LoadedPlugin` × N: each ~256 B (path + name + ledgers' headers).
- Ownership ledger entries: per-component / per-system, ~16 B each.
  16 plugins × ~32 components × ~8 systems × 16 B ≈ 16 KiB.
- Per-phase migration arena: 16 MiB (transient; not counted against
  cell ceiling per perf-budget.md §"Allocator Rules" rule 4 — the
  arena drains by phase 9).
- Manifest deserialized PODs retained for `PluginContext::manifest`:
  ~2–8 KiB per plugin.

Total resident: ~256 KiB for an MVP-scale 16-plugin set. Well inside
the 64 MiB `core` ceiling.

## 10. Failure Modes

The §10.1 failure-mode table in `specs/core/SPEC.md` is authoritative.
This section enumerates the **plugin-loader-specific** rows and adds
per-arm operator-action notes.

### 10.1 Loader-emitted `core::Error` arms

| Arm                       | Step (§3.5) | Recovery   | Operator action                                                                                |
|---------------------------|-------------|------------|------------------------------------------------------------------------------------------------|
| `PluginDlopenFailed`      | 2           | Refuse     | Read `dlerror()` text from `ErrorContext::detail`. Common cause: missing transitive dep.       |
| `PluginMissingEntryPoint` | 3           | Refuse     | Plugin build did not export all four C symbols. Check `manifest.cpp` codegen output.           |
| `PluginManifestInvalid`   | 4           | Refuse     | Fory blob malformed or schema-version mismatch. Rebuild plugin against current `glibre-foryc`. |
| `PluginAbiHashMismatch`   | 5           | Refuse     | Plugin built against an older `glibre-types.dylib`. Rebuild plugin.                            |
| `PluginEngineTooOld`      | 6           | Refuse     | Plugin's `min_engine_version` exceeds host's. Upgrade engine or downgrade plugin.              |
| `PluginNameCollision`     | 7           | Refuse     | Two plugins claiming same `manifest.name` from different paths. Rename or unload one.          |
| `PluginDependencyMissing` | 8           | Refuse     | A `depends_on` entry not registered. Load the dependency first (or batch-load).                |
| `PluginDependencyCycle`   | 8           | Refuse     | `depends_on` graph has a cycle. The chain is in `ErrorContext::detail`. Break the cycle.       |
| `PluginInitFailed`        | 10          | Rollback   | Plugin's `register` returned `unexpected`. Inner error in detail. Fix plugin.                  |
| `SystemScheduleCycle`     | 11          | Rollback   | Plugin's declared `(after, before)` introduced a cycle. Loader rolls back; plugin unloaded.    |
| `SchemaMigrationFailed`   | 12          | Rollback   | Missing chain or migrate-step `unexpected`. Ship migrate function or restore from snapshot.    |

Refuse vs Rollback distinction (per §10 of SPEC):

- **Refuse**: no state moved; the call returns `std::unexpected` with
  the ledgers untouched. Steps 2–8 fall here (the loader has not yet
  invoked any plugin C++).
- **Rollback**: state partially advanced (registries received entries,
  schedule was recompiled); the loader walks the ownership ledgers in
  reverse to restore the pre-call snapshot byte-for-byte. Steps 10–12
  fall here.

Every refusal logs **exactly once at the boundary where it is handled**
(error-model.md §"Logging / Telemetry" #1). The handling boundary is
`HotReloadBarrier::step` for hot-reload-driven loads (level: `warn`,
under the `HotReload` umbrella) and `FrameLoop::tick` startup for
first-time loads (level: `error`, no umbrella).

### 10.2 What the loader does NOT raise

Out-of-scope (raised by other aggregates and propagated through the
loader):

- `EntityStale`, `EntityForeignWorld`, `HierarchyCycle` — World's;
  the plugin's `register` may touch the World and propagate these.
- `TypeUnregistered`, `TypeRegistryClosed` — TypeRegistry's; raised
  if `register` calls `set_component` on an unregistered type.
- `ScheduleAccessConflict` — Schedule's; raised at compile, not at
  registration.
- `OutOfBudget` — engine-wide allocator's (perf-budget.md §"Allocator
  Rules"); the loader propagates if its own allocations exceed cell.
- `HotReload`, `HotReloadDrainTimeout`, `HotReloadAbiHashMismatch`,
  `HotReloadSelfReference` — barrier's; the loader is invoked under
  the umbrella but does not own these arms.

### 10.3 Abort cases (process termination)

Reserved for loader contract violations the design's invariants cannot
tolerate (SPEC §10.2 #4):

- A registry's `unregister` returns `unexpected` during compensating
  rollback. This means the rollback discipline itself is broken;
  silent recovery would mask determinism-snapshot bugs. Emit
  `glibre::log_error(err, error)` and `std::terminate`.
- `dlclose` returns non-zero on the candidate handle after a refusal.
  The handle is leaked but the engine continues; this is logged at
  `error` and the leaked handle is recorded for post-mortem. This is
  **not** an abort in MVP — `dlclose` failures on macOS are rare and
  recoverable; the leaked handle costs ~8 KiB of dyld bookkeeping.

## 11. Test Plan

### 11.1 Unit tests (Catch2, `tests/core/plugin/`)

Each row of the §10.1 failure-mode table maps to one or more unit
tests. The harness uses **fixture-built malformed plugins** generated
by a `tests/core/plugin/fixtures/` Makefile:

| Test name                                  | Drives arm                | Fixture                                              |
|--------------------------------------------|---------------------------|------------------------------------------------------|
| `loader.dlopen_failed_returns_arm`         | `PluginDlopenFailed`      | A missing-transitive-dep `.dylib`.                   |
| `loader.missing_entry_point_returns_arm`   | `PluginMissingEntryPoint` | Plugin omitting `glibre_plugin_register` export.     |
| `loader.manifest_malformed_returns_arm`    | `PluginManifestInvalid`   | Plugin with truncated Fory blob.                     |
| `loader.abi_hash_mismatch_returns_arm`     | `PluginAbiHashMismatch`   | Plugin built against stale `glibre-types.dylib`.     |
| `loader.engine_too_old_returns_arm`        | `PluginEngineTooOld`      | Plugin with elevated `min_engine_version`.           |
| `loader.name_collision_returns_arm`        | `PluginNameCollision`     | Two plugins same `name`, different paths.            |
| `loader.dependency_missing_returns_arm`    | `PluginDependencyMissing` | Plugin depending on unloaded `glibre.foo`.           |
| `loader.dependency_cycle_returns_arm`      | `PluginDependencyCycle`   | Triple A→B→C→A.                                      |
| `loader.init_failed_runs_compensating_unregister` | `PluginInitFailed`  | Plugin whose `register` returns `unexpected`.        |
| `loader.schedule_cycle_rolls_back`         | `SystemScheduleCycle`     | Plugin with intra-phase `after`/`before` cycle.      |
| `loader.migrate_failed_rolls_back`         | `SchemaMigrationFailed`   | Plugin bumping schema with no migrate function.      |
| `manifest.parse_canonical_blob_round_trip` | (positive)                | Hand-rolled known-good blob.                         |
| `manifest.tag_sorted_layout`               | (positive)                | Asserts `PluginManifest` field offsets are tag-sort. |
| `abi_hash.compute_canonical_order`         | (positive)                | Asserts blake3 is byte-equal across host runs.       |
| `abi_hash.compute_excludes_build_env`      | (positive)                | Builds same schemas with `__DATE__`/`__TIME__` macro variations; hash unchanged. |
| `loader.batch_topological_order`           | (positive)                | Three plugins in reverse dep order; assert load order is forward.|
| `loader.list_lockfree_consistent_view`     | (positive, concurrency)   | Reader thread + loader thread; assert no torn read.  |
| `loader.idempotent_same_path_load`         | (positive)                | Same path twice → same `PluginId`, no second `dlopen`.|
| `loader.unload_orphan_check_refuses`       | (positive)                | Unload of A while B `depends_on` A → `PluginDependencyMissing`.|

### 11.2 Integration tests (Catch2, `tests/core/integration/`)

Drives the full failure-mode coverage per the sibling plan #232:

- `integration.first_load_happy_path` — load 3 inter-dependent plugins;
  assert frame N+1 sees their systems run.
- `integration.full_failure_table` — drives every arm in §10.1 in
  sequence, asserting prior-good plugins remain live after each
  refusal.
- `integration.rollback_byte_equal` — asserts world-state byte-equal
  before-and-after a refused load via deterministic snapshot
  comparison (PHILOSOPHY §7).
- `integration.hot_reload_round_trip` — startup + reload cycle;
  asserts ECS state survives + barrier emits the expected events.

### 11.3 E2E coverage

E2E traces under `tests/e2e/plugins/` ship the fixture plugins from
hot-reload-protocol.md §"Test Hooks" plus three additions for the
loader's load-time arms:

- `bad-manifest` (truncated Fory blob).
- `bad-deps` (cycle).
- `bad-init` (`register` returns unexpected).

Each fixture has a recorded `.glibre-trace` golden; CI replays asserts
byte-equal trace output across runs (deterministic-replay obligation,
SPEC §10.2 #7).

### 11.4 Performance microbenchmarks

Catch2 `BENCHMARK` blocks under `tests/core/perf/loader_bench.cpp`:

- `bench.dlopen_dlsym_manifest_read` — asserts <1 ms for an MVP-scale
  manifest (fits the cold-load budget of §9.2).
- `bench.phase8_idle_no_reload` — asserts <100 ns (single relaxed
  atomic load).
- `bench.batch_topological_load_16` — asserts <50 ms total for 16
  inter-dependent plugins.

CI gates per perf-budget.md §"CI Gate Spec" #1: any benchmark
exceeding its budget fails the PR.

## 12. Open Questions

- **[OPEN] PluginContext layout stability** (plugin-abi.md §"Open
  Questions" #3): keep as POD aggregate or a stable-vtable class?
  Lean toward POD aggregate; verify via prototype before the loader's
  first plan issue lands. Touches the engine ABI guarded by
  `min_engine_version`.

- **[OPEN] PluginManifest schema versioning rollover** (plugin-abi.md
  §"Open Questions" #4): the manifest itself is a Fory schema and may
  evolve. Migration uses the standard `migrate_PluginManifest_v1_to_v2`
  mechanism (fory-codegen.md §"Migration Mechanic"), but the loader
  must handle the case where a plugin's manifest is itself stale.
  Resolve in the first plan that lands the loader.

- **[OPEN] Symmetric `glibre_plugin_unregister` mandatory window**
  (plugin-abi.md §"Open Questions" #1): MVP makes it optional; the
  first hot-reload plan makes it mandatory. The loader's compensating
  unregister currently walks the four ownership ledgers; making
  `glibre_plugin_unregister` mandatory adds a fifth call (the plugin's
  own teardown of its private state) at the start of the rollback.
  Decide whether to call it before or after registry rollback.

- **[OPEN] Conflict declarations** (R-1.6.3 partial coverage): mutual-
  exclusion between two plugins (e.g. two competing physics plugins).
  Defer until first concrete need; the manifest schema can extend
  additively (new tag, no version bump) per fory-codegen.md.

- **[OPEN] Filesystem-watcher loop coalescing**: if the watcher fires
  10 times for one save (editors flush in bursts), should the
  barrier coalesce into one reload? Cheap to implement (dedupe on
  `plugin_fqn` in the request queue); add as a small follow-up plan
  once the watcher exists.

- **[OPEN] Diagnostic enrichment of `PluginDependencyCycle`**
  (R-1.6.4 "report full chain"): plugin-abi.md §"Failure Modes"
  attaches the chain to `ErrorContext::detail` as a string. Verify
  the chain is rendered as `A -> B -> C -> A` in the editor's plugin
  panel; format owned by editor-side rendering, not by loader. Track
  in tools spec.

- **[OPEN] Cross-plugin direct symbol use** (plugin-abi.md §"Open
  Questions" #2): forbidden in this design. A future scripting-plugin
  spike may need a typed channel. If introduced, the channel goes
  through the type registry, never via `dlsym` between plugins.
  Confirm at scripting-plugin spike.

- **[OPEN] Code-signing / notarization on macOS** (plugin-abi.md
  §"Open Questions" #5): out of scope. The loader treats unsigned
  plugins identically; distribution policy for shipped plugins is a
  release-engineering decision, not an ABI one.
