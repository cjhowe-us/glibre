# Hot-Reload Barrier — Detailed Design

> Detailed design for the `core::HotReloadBarrier` aggregate (specs/core/SPEC.md
> §4.6 / §5.8 / §6.7). Frame-phase 8 gate that drains in-flight plugin work,
> swaps plugin dylibs, migrates schema-versioned component storages, and resumes
> the world — atomically per plugin. Closes spike #706.

- Refs:
  `specs/core/SPEC.md` §1, §4.5, §4.6, §4.9, §5.1, §5.8, §5.12, §6.7, §7.1, §10
  `reviews/decisions/hot-reload-protocol.md`,
  `reviews/decisions/frame-phases.md`,
  `reviews/decisions/plugin-abi.md`,
  `reviews/decisions/error-model.md`,
  `reviews/decisions/perf-budget.md`,
  `reviews/decisions/fory-codegen.md`,
  `PHILOSOPHY.md` §3, §6, §7, §8, §9, §11.
- Parent spike: #699. Sibling spike (blocked on this): task-breakdown spike
  for the barrier. Cross-cuts: #704 (plugin loader detailed design), #492
  (core-owned-types-hot-reload-migration-policy), #499
  (hot-reload-migration-arena-grow-vs-refuse).

## 1. Purpose

`HotReloadBarrier` is the single aggregate that owns the **drain → swap →
migrate → resume** state machine at frame-phase 8. It is the only point in
a glibre frame at which plugin `.dylib` code may be replaced and at which
component-storage bytes may be transformed by schema-version migration. It
guarantees:

1. **Atomicity of code replacement** — between two consecutive frames any
   observer (system body, ECS query, editor panel, e2e harness) sees either
   the entirely-pre-swap or entirely-post-swap state for a given plugin;
   never a half-swapped vtable, never a half-migrated storage row.
2. **Refusal-preserves-prior-state** — when any of the three enumerated
   refusal cases fires (ABI hash mismatch, schema migration failure, plugin
   init error), the barrier rolls back so the previous-good plugin instance
   keeps running and the world is byte-equal to its pre-barrier state.
3. **Frame-boundary discipline** — phase 8 runs after `render-submit` and
   before `present`; no system body executes during it. PHILOSOPHY §8 and
   `frame-phases.md` give us exactly one barrier position per frame.

It refuses to own:

- **Plugin discovery / `dlopen` mechanics** — owned by `PluginLoader` (§4.5,
  `plugin-abi.md`). The barrier asks the loader to load and unload; it does
  not call `dlopen`/`dlsym` itself.
- **Migration function bodies** — owned by the originating context that
  authored the schema (`hot-reload-protocol.md` "Migrate Function Contract";
  `fory-codegen.md` "Migration Mechanic"). The barrier is the dispatcher.
- **`World` mutation by plugin code** — only the outgoing plugin's
  `glibre_plugin_drain` and the incoming plugin's `glibre_plugin_register`
  may run during phase 8, and even those receive `World&` for narrow,
  contractually-bounded edits (release/acquire GPU handles, register
  systems, rebuild caches).
- **Self-reload of `core`** — out of scope per PHILOSOPHY (greatly reduced
  MVP) and `hot-reload-protocol.md` "Open Questions" #2.
- **Filesystem watchers / IDE integration** — the barrier accepts in-process
  reload requests via a queue; the watcher is a thin wrapper that calls
  `request_reload`.

The aggregate's SRP boundary is the rules of frame-boundary plugin swap
(SPEC §4.6 SRP justification). If the drain protocol, the migration
runner, or the refusal-rollback discipline change, this aggregate changes;
nothing else.

## 2. Requirements coverage

The barrier is responsible for fulfilling the following invariants and
requirements drawn from the locked decision records and the §4.6 SPEC
aggregate. Each row points at the section of this design that discharges it.

| Source                                                 | Requirement                                                              | Discharged by |
|--------------------------------------------------------|--------------------------------------------------------------------------|--------------|
| PHILOSOPHY §8                                          | Hot-reload at frame boundaries; never mid-frame                          | §3, §4, §6   |
| PHILOSOPHY §9                                          | Plugin ABI gated by middleman dylib hash; refuse on mismatch             | §7, §10      |
| `frame-phases.md` row 8                                | Drain → swap → migrate → resume; idle if no reload requested             | §3, §4, §5, §6 |
| `frame-phases.md` Open Q #1                            | Phase 8 with no reload pending must be sub-microsecond no-op             | §3, §5, §9   |
| `frame-phases.md` Open Q #5                            | Migration stays on game-loop thread for MVP                              | §6, §9       |
| `hot-reload-protocol.md` "Decision"                    | Four numbered steps; per-plugin atomicity, not phase-wide                | §3, §4       |
| `hot-reload-protocol.md` "State Survival Rules"        | "Has a `.fory` schema → survives" rule                                   | §3, §7       |
| `hot-reload-protocol.md` "Migrate Function Contract"   | Pure, deterministic, total, arena-only, idempotent                       | §7           |
| `hot-reload-protocol.md` "Refusal Cases"               | Exactly three refusal arms wrapped under `HotReloadRefused`              | §10          |
| `hot-reload-protocol.md` "Observer Notification"       | Synchronous bus between barrier steps 4.2 and 4.3                        | §4, §6       |
| `hot-reload-protocol.md` "Failure & Rollback"          | Per-plugin transactional rollback; arena reset on migrate failure        | §4, §10      |
| `hot-reload-protocol.md` "Test Hooks"                  | `enqueue_hot_reload` + `await_reload` under `GLIBRE_E2E`                 | §4, §11      |
| `plugin-abi.md` "Loader Sequence"                      | Recheck ABI hash + manifest under barrier, not just first load           | §7, §8       |
| `plugin-abi.md` "Versioning Rules"                     | ABI hash, SONAME, plugin SemVer, min_engine_version each move alone      | §7           |
| `error-model.md` "Type Sketch"                         | `core::Error::HotReload`, `HotReloadDrainTimeout`, `HotReloadAbiHashMismatch`, `HotReloadSelfReference`, `SchemaMigrationFailed`, `PluginInitFailed` | §10 |
| `error-model.md` "Logging / Telemetry"                 | Refusals logged at `warn`, never `error`                                 | §10          |
| `perf-budget.md` "Per-Context Budget Table" / "Pipelined Frame Timing" | Phase 8 idle <0.1 ms; reload-frame ≤0.40 ms one-shot; 16 MiB migration arena | §9 |
| `perf-budget.md` "Allocator Rules" #6                  | Migration arena owned by `core`; exceeding refuses with `SchemaMigrationFailed` | §9, §10 |
| `fory-codegen.md` "Migration Mechanic"                 | Static migration table populated by middleman; engine reuses it         | §7           |
| SPEC §4.6 invariant 1                                  | Fires only at phase 8                                                    | §3, §4, §6   |
| SPEC §4.6 invariant 2                                  | Drain completeness; bounded budget; `HotReloadDrainTimeout` on overrun   | §6, §10      |
| SPEC §4.6 invariant 3                                  | ABI hash recheck before swap                                             | §7, §10      |
| SPEC §4.6 invariant 4                                  | Migration atomicity; no observable partial state                         | §4, §10      |
| SPEC §4.6 invariant 5                                  | No mid-frame command buffers survive a swap                              | §6           |
| SPEC §4.6 invariant 6                                  | Single-position barrier per frame                                        | §3           |
| SPEC §4.6 invariant 7                                  | Refusal restores world byte-for-byte                                     | §4, §10      |

**Out-of-MVP, refused with rationale.** The harmonius
`hot-reload-protocol.md` lists eleven `ReloadSubject` arms (middleman
dylib, shader module, material graph, logic graph, behaviour tree, vfx
graph, animation state machine, data table, texture / mesh / audio
asset, procedural graph) and a `VersionEpoch` counter, a `SymbolManifest`
delta computation, and a `ReloadResult::Deferred` arm. Glibre's barrier
refuses to model these:

- **Asset / shader / material / behaviour / vfx / animation / data-table
  reload** — these are owned by the responsible plugin's `register` body
  (or by a separate "asset reload" path in phase 1 per
  `hot-reload-protocol.md` Open Q #5). The barrier does **not** know
  about subject taxonomies; from its vantage everything is "swap one
  plugin dylib." Per Occam, two collapsing requirements (per-subsystem
  reload + plugin-dylib reload) become one primitive (plugin-dylib
  reload at phase 8). If a future spike opens an asset-only reload path
  it lives in another aggregate, not here.
- **`VersionEpoch` counter** — replaced by the existing `World`
  `ChangeTick` (§4.1) plus the per-reload `frame_index` recorded in
  `HotReloadCheckpoint` (§7.1). Two clocks for one purpose is a §3.2
  collapse violation.
- **`SymbolManifest` deltas** — plugin code identity is determined by
  the middleman ABI hash (PHILOSOPHY §9). Symbol-level deltas are an
  implementation detail of the dynamic linker, not a public protocol.
  Refused.
- **`ReloadResult::Deferred`** — a request is either Pending,
  Completed, or Refused (SPEC §5.8 `ReloadOutcome`). Deferral collapses
  to "Pending until next phase 8."
- **Compile-error rollback as a barrier responsibility** (harmonius
  F-1.11.3) — compilation is editor / build-system territory; the
  barrier sees only finished `.dylib`s. Refused.

## 3. Detailed model

### 3.1 Aggregate state

```text
HotReloadBarrier (singleton, owned by core::FrameLoop)
├── pending_count       : std::atomic<std::uint32_t>   (the hot-path predicate)
├── pending_requests    : eastl::vector<ReloadRequest> (game-loop-thread only)
├── statuses            : eastl::hash_map<ReloadRequestId, ReloadStatus>
├── observers           : eastl::vector<ObserverEntry> (observer bus, §4.4)
├── migration_arena     : glibre::Arena                (16 MiB ceiling)
├── txn_log             : eastl::vector<Transaction>   (one per in-flight reload)
└── self_reload_guard   : bool                         (always false in MVP)

ReloadRequest = {
    id                       : ReloadRequestId  (monotonic u64)
    plugin_fqn               : eastl::string    (from manifest)
    replacement_dylib_path   : std::filesystem::path
    enqueue_frame_index      : std::uint64_t
}

ReloadStatus = { outcome : ReloadOutcome, cause : Error }

Transaction (lives only inside step()) = {
    request                  : const ReloadRequest&
    outgoing_plugin_id       : core::PluginId
    candidate_dl_handle      : void*           (from PluginLoader staging)
    candidate_manifest       : PluginManifest  (deserialized, validated)
    drained                  : bool
    swapped                  : bool
    migrated_types           : eastl::vector<TypeFqn>
    pre_migration_snapshot   : SnapshotRef     (per-storage row checkpoints)
}
```

### 3.2 State-machine view

```
        request_reload(fqn, path)            step()
              │                                │
              ▼                                ▼
         ┌─────────┐                     ┌─────────────┐
   Idle ─┤ Queued  │── pending_count++   │ pending == 0│  ── relaxed load,
         └────┬────┘                     │  → return 0 │     return immediately
              │                          └─────────────┘
              │                                │ pending > 0
              ▼                                ▼
         ┌────────┐  fail            ┌──────────────────┐
         │ Drain  ├──────────────┐   │ for each request │
         └────┬───┘  drain ok    │   │  begin_txn       │
              ▼                  │   │   ▼              │
         ┌────────┐  fail        │   │  Drain  ─┐       │
         │ Swap   ├──────────────┤   │   │ ok   │       │
         └────┬───┘              │   │   ▼      ▼ fail  │
              ▼                  │   │  Swap   rollback │
         ┌────────┐  fail        │   │   │              │
         │Migrate ├──────────────┤   │   ▼              │
         └────┬───┘              │   │  Migrate         │
              ▼                  │   │   │              │
         ┌────────┐  fail        │   │   ▼              │
         │ Resume ├──────────────┘   │  Resume          │
         └────┬───┘                  │   │              │
              ▼                      │  publish events  │
         ┌────────┐                  └──────────────────┘
         │ Done   │ → publish HotReloadCompleted          │
         └────────┘                                       │
                                                          ▼
                                                  Refused (per-plugin
                                                  rollback, others
                                                  continue)
```

`step()` is the only state-mutator on the barrier path; `request_reload`
is the only enqueue. Both are non-`const` member functions on the
game-loop driver thread (§6 concurrency).

### 3.3 Phase-table placement

`frame-phases.md` is the authority on phase ordering. The barrier is row
#8 verbatim:

| # | Name | Owning context | What runs (this design's body) | Allowed reads | Allowed writes | Exit guarantee |
|---|------|----------------|-------------------------------|---------------|----------------|----------------|
| 1 | input         | platform | (no barrier work)                                                | (per row)     | (per row)      | (per row)      |
| 2 | logic         | (deferred) | (no barrier work)                                              |               |                |                |
| 3 | physics-fixed | physics  | (no barrier work)                                                |               |                |                |
| 4 | animation     | (deferred) | (no barrier work)                                              |               |                |                |
| 5 | transform     | core     | (no barrier work)                                                |               |                |                |
| 6 | cull-extract  | render   | (no barrier work)                                                |               |                |                |
| 7 | render-submit | render   | (no barrier work)                                                |               |                |                |
| **8** | **hot-reload** | **core** | **`HotReloadBarrier::step` — Drain → Swap → Migrate → Resume; idle on `pending_count == 0`.** | `PluginRegistry`, `pending_requests`, candidate dylib `.rodata` (manifest, abi-hash symbol) | Plugin vtables, `TypeRegistry` appends, migrated component storages, observer bus | All loaded plugins satisfy ABI hash; component storages migrated; refusal cases logged and old plugin kept |
| 9 | present | platform | (no barrier work) | | | |

The barrier is the **only** call between phase 7's exit (command buffers
enqueued to the GPU but not yet presented) and phase 9's entry (drawable
acquire + present). PHILOSOPHY §8, `frame-phases.md` "Hot-reload at slot
8, after submit and before present", and SPEC §4.6 invariant 6 lock this
single position. Any plugin that wants other-phase reload behavior must
go through the barrier — there is no second hot-reload slot.

### 3.4 Object responsibilities (SRP)

- **`HotReloadBarrier`** — the four-step state machine, the request
  queue, the observer bus, the migration arena. Owns no plugin code
  itself.
- **`PluginLoader`** (§4.5, plugin-abi.md) — `dlopen`, `dlsym`, manifest
  read, hash check, registries. The barrier asks the loader for
  candidate dylibs (staged), and asks it to swap-in / unload. Loader
  steps 1–7 in `plugin-abi.md` "Loader Sequence" run **inside** the
  barrier's Swap step (not before phase 8 entry) so that a candidate
  whose manifest fails to deserialize does not even get a vtable swap
  attempt.
- **`glibre-types.dylib`** — owns `glibre_types_abi_hash()` and the
  static migration table; the barrier reads both and never writes
  either.
- **Originating contexts** (per-plugin) — own the migrate function
  bodies. The barrier dispatches; the body executes.
- **Observer subscribers** (editor, e2e) — receive `HotReloadStarted /
  Completed / Refused` events synchronously; they may neither call back
  into the barrier nor mutate the world during the callback (§6.4).

This is the same SRP split SPEC §4.6 / §6.7 already record; the design
doc only sharpens which messages cross the seam at which step.

## 4. Public surface

The barrier's public C++ surface is the §5.8 SPEC stub plus three
additions this design pins down (observer (un)subscription, an explicit
"is-idle" predicate for instrumentation, and the E2E test hooks under
`GLIBRE_E2E`). All return `glibre::Result<T>` (i.e.
`std::expected<T, glibre::Error>`); per `error-model.md` no exceptions
cross the boundary.

```cpp
// core/include/glibre/core/hot_reload.hpp — facade header.
// Aggregates §5.8 of specs/core/SPEC.md and adds observer + E2E hooks.

#pragma once
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>

#include <EASTL/string_view.h>
#include <EASTL/vector.h>

#include <glibre/error.hpp>
#include <glibre/core/types_fwd.hpp>     // ReloadRequestId, ReloadStatus,
                                         // HotReload*Event from §5.

namespace glibre::core {

// 1. Request enqueue --------------------------------------------------------
//    Idempotent within one frame: re-requesting the same plugin_fqn
//    coalesces (the latest replacement_dylib_path wins). Non-blocking.

[[nodiscard]] Result<ReloadRequestId>
HotReloadBarrier::request_reload(
    eastl::string_view             plugin_fqn,
    const std::filesystem::path&   replacement_dylib_path) noexcept;

// 2. Step the state machine ------------------------------------------------
//    Called once per frame from FrameLoop at Phase::HotReload entry.
//    Returns the count of requests processed (0 when idle — the no-op
//    path; the only side-effect is one relaxed atomic load).

[[nodiscard]] Result<std::size_t>
HotReloadBarrier::step() noexcept;

// 3. Status poll -----------------------------------------------------------
[[nodiscard]] ReloadStatus
HotReloadBarrier::status(ReloadRequestId id) const noexcept;

// 4. Idle predicate (instrumentation, no side effects) ---------------------
//    Used by the editor's perf HUD and the dependency-check workflow.
[[nodiscard]] bool
HotReloadBarrier::is_idle() const noexcept;

// 5. Observer bus ----------------------------------------------------------
//    Subscribers are called synchronously by the barrier on the
//    game-loop thread between barrier steps 4.2 and 4.3
//    (hot-reload-protocol.md §"Observer Notification"). Subscribers
//    MUST be idempotent and MUST NOT call back into the barrier or
//    mutate the World during the callback.

using HotReloadObserverFn = void (*)(
    void*                                   user_data,
    const HotReloadEventVariant&            event) noexcept;

struct HotReloadObserverHandle {
    std::uint64_t value{};
    friend constexpr bool operator==(
        HotReloadObserverHandle, HotReloadObserverHandle) noexcept = default;
};

[[nodiscard]] Result<HotReloadObserverHandle>
HotReloadBarrier::subscribe(
    HotReloadObserverFn fn,
    void*               user_data) noexcept;

[[nodiscard]] Result<void>
HotReloadBarrier::unsubscribe(HotReloadObserverHandle handle) noexcept;

// 6. E2E-only deterministic hooks (compiled out in shipping) ---------------
//    Identical surface to hot-reload-protocol.md §"Test Hooks". Lives
//    under #if defined(GLIBRE_E2E) and is not linked into runtime.

#if defined(GLIBRE_E2E)
namespace test {

[[nodiscard]] ReloadRequestId
enqueue_hot_reload(
    eastl::string_view             plugin_fqn,
    const std::filesystem::path&   replacement_dylib_path) noexcept;

[[nodiscard]] Result<void>
await_reload(ReloadRequestId id) noexcept;

}  // namespace test
#endif

}  // namespace glibre::core
```

The four-step sequence the public surface implements is the contract from
§4.4 below. Plugin-side surfaces required by the barrier
(`glibre_plugin_drain`, `glibre_plugin_register`,
`glibre_plugin_unregister`, `glibre_plugin_abi_hash`,
`glibre_plugin_manifest`, `glibre_plugin_manifest_size`) are owned by
`plugin-abi.md` and not redefined here.

### 4.1 `request_reload` semantics

- **Thread-safety:** `request_reload` is callable from any thread that
  holds a non-borrowed reference to the barrier (filesystem watcher
  thread, editor UI thread, asset-import worker thread, e2e harness
  thread). Internally it takes a short MPSC enqueue lock and bumps
  `pending_count` with `memory_order_release` so phase 8's relaxed load
  observes the count once the request is queued. (This is the **only**
  multithreaded entry point on the barrier; see §6.)
- **Coalescing:** if a request for `plugin_fqn` is already pending in the
  current frame's queue, the new request overwrites
  `replacement_dylib_path` and returns the existing `ReloadRequestId`.
  Two callers asking for the same plugin within a frame see one
  completion event, one refusal at most. The latest path wins because
  the editor's compile-then-link pipeline produces a single newest
  artifact per plugin.
- **Validation deferred:** `request_reload` does not `dlopen` the
  replacement, does not parse its manifest, does not check the ABI hash.
  Those happen inside `step()` so refusals always surface through the
  same observer + status path.
- **`HotReloadSelfReference` guard:** if `plugin_fqn == "glibre.core"`
  the call returns `unexpected(core::Error::HotReloadSelfReference)`
  immediately. Self-reload of `core` is out of scope; rejecting at the
  request site makes the refusal visible at the call boundary instead of
  delaying it to phase 8.

### 4.2 `step()` semantics

- **Hot path (idle frame):** one relaxed load on `pending_count`. If
  zero, return `Result<std::size_t>{0}` immediately. No fence, no
  cache flush, no allocations, no observer dispatch. This is the
  steady-state phase-8 cost the perf budget assumes (§9).
- **Reload frame:** drain the queue, run the four-step transaction per
  request (§4.4), publish events, reset the migration arena, return
  the count of requests processed. The result is `Result<size_t>`
  because the barrier's bookkeeping itself can fail in pathological
  ways (e.g. observer-list allocation under arena exhaustion → the
  same `OutOfBudget` arm `error-model.md` defines for
  `PerContextAllocator` overflow).
- **No mid-step yields:** `step()` is synchronous on the game-loop
  thread. It does not coroutine-yield, does not consult a watchdog
  (the watchdog is the drain-timeout invariant; §6.2), and does not
  return until every queued request has reached a terminal state.

### 4.3 `status` and `is_idle`

- `status(id)` returns the cached `ReloadStatus` for any id ever
  returned by `request_reload`. Statuses are retained across frames
  (the editor's "live reload" UI polls the previous frame's id) but
  are evicted in LRU order when their count exceeds 256 — a fixed
  budget consistent with the per-context allocator ceiling. Eviction
  observable via `is_idle` post-clear is acceptable; the editor only
  cares about the most recent N requests.
- `is_idle()` returns `pending_count == 0 && txn_log.empty()` —
  truthful with respect to phase-8 work-in-flight. Used by the perf
  HUD and by the dep-check CI workflow that asserts a clean shutdown.

### 4.4 Sequence: `quiesce → drain → swap → migrate → resume`

Per request, run inside `step()` in this order. Each numbered substep
matches `hot-reload-protocol.md` §"Protocol Sequence" with two
clarifications introduced by this design (the explicit "quiesce"
precondition and the per-substep observer-call point).

**Quiesce (precondition, free).** Phases 1–7 of frame N have already
executed; phase 9 of frame N has not begun. The schedule guarantees
no system body is in flight (single-worker MVP, §6.10 of SPEC). The
barrier asserts `World::is_quiescent()` (debug build only) before
entering Drain.

**Step 1 — Drain.** For the outgoing plugin P:

1. Take the world's exclusive write lock. (Documentation in MVP; held
   for free since no other thread runs in phase 8.)
2. Call `P::glibre_plugin_drain(World&) noexcept -> Result<void>`.
   Plugin must:
   - flush per-frame queues into middleman-typed components;
   - release plugin-private GPU resource handles (Metal pipeline
     states, descriptor heaps, transient buffers);
   - cancel and join any worker tasks the plugin spawned.
3. Verify post-conditions: every plugin-private allocation either
   lives in a middleman-typed storage or is released. The barrier
   does not enforce this mechanically; it documents the contract and
   the e2e tests assert it.
4. Drain budget: `<= 0.10 ms` per plugin (§9). Overrun →
   `core::Error::HotReloadDrainTimeout` (SPEC §10 row), abort this
   plugin's transaction, leave P live, publish `HotReloadRefused`.

**Step 2 — Swap.** For the outgoing P, candidate Q:

1. **Stage the candidate via `PluginLoader`.** Loader runs steps 1–7
   of `plugin-abi.md` "Loader Sequence" against the candidate dylib:
   `dlopen` (`PluginDlopenFailed`), `dlsym` four entry points
   (`PluginMissingEntryPoint`), deserialize manifest
   (`PluginManifestInvalid`), ABI-hash recheck (must equal both the
   redundant symbol and the host's `glibre_types_abi_hash()` —
   `PluginAbiHashMismatch`), engine-version check
   (`PluginEngineTooOld`), name-collision check
   (`PluginNameCollision`), dependency check
   (`PluginDependencyMissing` / `PluginDependencyCycle`). Any failure
   here aborts before any vtable mutation; abort path: `dlclose` Q,
   leave P live.
2. **Manifest superset check.** Q's `(fqn, schema_version)` set on
   `components` must be a superset-or-equal of P's surviving
   storages. A subset means Q dropped a type P registered — that is a
   major-version change and is refused via
   `core::Error::HotReloadRefused` (umbrella) wrapping
   `core::Error::SchemaMigrationFailed` (the rule that "no chain
   exists for type T" applies symmetrically to "type T is missing
   entirely").
3. **Atomic vtable replacement.** Replace P's entry in the plugin
   registry with Q's. Single relaxed store under exclusive ownership.
   Type-registry appends (Q's new types, if any) happen here too;
   the type-registry append-only rule (§4.9) ensures no observer
   sees a removed type.
4. Postcondition: every system call site dispatches to Q; no thread
   holds a live reference to P's code (P's `dlclose` is deferred
   until after Resume so a rollback can re-link P without a second
   `dlopen`).

**Step 3 — Migrate.** For each persistent component / singleton T
declared by Q whose `schema_version > stored_version`:

1. Look up the `(stored_version → current_version)` chain in
   `glibre-types.dylib`'s migration table. Missing chain →
   `core::Error::SchemaMigrationFailed`, roll back to P (un-swap
   vtable, drop Q's type-registry appends, re-register P's types,
   `dlclose` Q).
2. For each row in the storage, allocate a scratch buffer of
   `sizeof(T_current)` from the migration arena (16 MiB ceiling,
   §9). Run the chain step-by-step:
   `migrate_T_vN_to_vNplus1(const T_vN&, T_vNplus1&, Arena&)
   -> Result<void>`.
3. **Arena exhaustion.** If the arena cannot satisfy a row's
   allocation, refuse with `core::Error::SchemaMigrationFailed` and
   roll back. (§4.10 SPEC open question: grow vs refuse — refuse in
   MVP per `perf-budget.md` Allocator Rules #6; `[OPEN]` cross-ref
   to #499 in §12.)
4. On success of the full chain, overwrite the row in place. Size is
   bounded by the new struct's compile-time size per
   `fory-codegen.md` §ABI Stability rule 3 (additive-or-bumped
   versions, never a free reorder).
5. On any per-row failure, the partially-migrated rows for *that
   row* live only in the arena which is reset on failure; previously
   committed rows for the same type T are reverted by running the
   inverse migrate chain if the originating context declared the
   migration invertible (`hot-reload-protocol.md` "Failure &
   Rollback" step 4). Non-invertible migration with mid-step failure
   is a contract violation and terminates the process (loud, immediate,
   in CI) per the same record.

**Step 4 — Resume.** For Q:

1. **Pre-event observer dispatch (4.1).** Construct
   `HotReloadCompleted{plugin_fqn, old_hash, new_hash, migrated_types}`.
   Defer dispatch until 4.3.
2. Call `Q::glibre_plugin_register(PluginContext&) noexcept ->
   Result<void>`. Plugin re-acquires GPU resources, registers systems
   into the phase table (idempotent w.r.t. system identity), rebuilds
   plugin-private caches keyed off middleman state.
   - Failure → `core::Error::PluginInitFailed` (umbrella
     `HotReloadRefused`); roll back swap + migrate; re-invoke
     `P::glibre_plugin_register` to rebuild P's caches dropped at
     drain (P::register is idempotent per `plugin-abi.md`).
3. **Observer dispatch (4.3).** Synchronously invoke every
   subscribed `HotReloadObserverFn` with the
   `HotReloadCompletedEvent` (or `HotReloadRefusedEvent`) — exactly
   once per plugin-transaction terminal state.
4. `dlclose` P's old handle (only after a successful Resume).
   Decrement `pending_count`.

When the request queue is exhausted, `step()` resets the migration
arena (drains 16 MiB back to the per-context allocator), clears
`pending_requests`, and returns the processed count. Phase 9 begins.

### 4.5 Observer event variants

The barrier publishes the existing `HotReloadStartedEvent`,
`HotReloadCompletedEvent`, `HotReloadRefusedEvent` triplet (SPEC §5.12).
This design pins their dispatch points:

- `HotReloadStartedEvent` — before Step 1 (Drain) entry, per request.
- `HotReloadCompletedEvent` — at Step 4.3 after `register` succeeds,
  before `dlclose(P)`. Subscribers see a fully-swapped, fully-migrated,
  fully-rebuilt world.
- `HotReloadRefusedEvent` — at the moment the rollback completes, with
  `cause` set to the most-specific arm (`PluginAbiHashMismatch`,
  `SchemaMigrationFailed`, or `PluginInitFailed`) wrapped under
  `HotReloadRefused`.

`HotReloadEventVariant` is the EASTL-`variant` over the three event POD
structs; subscribers dispatch on the active alternative.

## 5. Hot/cold path split

`HotReloadBarrier` is unique among `core` aggregates in having a true
hot path that runs every frame and a cold path that runs only on
reload-frames. The split must be visible in the source so the optimizer
keeps the hot path in icache.

### 5.1 Hot path — `step()` when idle

```cpp
[[nodiscard]] Result<std::size_t>
HotReloadBarrier::step() noexcept {
    if (pending_count_.load(std::memory_order_relaxed) == 0) [[likely]] {
        return 0;  // single relaxed load + branch + return.
    }
    return step_cold();   // out-of-line, never inlined.
}
```

- **Body:** one relaxed atomic load, one comparison, one return. No
  cache fence, no observer dispatch, no allocations. Compiles to
  ≈ 4 instructions on arm64 (load-acquire-relaxed, cmp, b.eq, ret).
- **Budget:** sub-microsecond, well inside the `<0.1 ms` ceiling
  `frame-phases.md` and `perf-budget.md` set for phase 8. Resolves
  `frame-phases.md` Open Q #1.
- **Memory order:** `relaxed` is correct because `request_reload`
  publishes `pending_count` with `release` and the only consumer is
  this same thread on a later frame; no inter-frame synchronization
  data-race exists.

### 5.2 Cold path — `step_cold()`

Out-of-line (`[[gnu::cold]]`) function that runs the four-step state
machine. Cold path costs (§9):

- Drain: `<= 0.10 ms` per plugin.
- Swap: `<= 0.05 ms` per plugin (loader sequence steps 1–7 plus the
  vtable / type-registry append).
- Migrate: bounded by `Σ rows × per-row migrate cost`; cell budget is
  the 0.40 ms one-shot `perf-budget.md` carves out for reload frames.
- Resume: `<= 0.15 ms` per plugin (register + cache rebuild + observer
  dispatch).

Cold path may allocate (migration arena, txn log entries, observer
event POD payloads); allocation goes through the per-context allocator
under the `core` `ContextTag`.

### 5.3 Module placement

```
core/
├── include/glibre/core/
│   └── hot_reload.hpp          # public surface (§4 of this design)
└── src/hot-reload/
    ├── barrier.cpp              # step()/request_reload(); hot path inline
    ├── transaction.cpp          # begin_txn/commit/rollback
    ├── drain.cpp                # Step 1 implementation
    ├── swap.cpp                 # Step 2 implementation
    ├── migrate.cpp              # Step 3 implementation
    ├── resume.cpp               # Step 4 implementation
    ├── arena.cpp                # 16 MiB migration arena
    ├── observers.cpp            # observer bus (subscribe / dispatch)
    └── e2e_hooks.cpp            # #if GLIBRE_E2E { test::* }
```

The split mirrors §6.1 of the SPEC's "one sub-module per aggregate" rule
and gives the linker a clean `--gc-sections` boundary so e2e hooks drop
out of shipping builds.

## 6. Concurrency

### 6.1 Threading model

- **Game-loop driver thread** owns the entire phase-8 execution.
  `step()`, all four transaction substeps, observer dispatch, migration
  arena management, and `dlclose` calls run here. This is the single
  writer.
- **Multi-producer enqueue thread(s)** are permitted to call
  `request_reload`. Producers in MVP: filesystem watcher (1 thread),
  editor UI (1 thread), asset-import worker pool (≤4 threads), e2e
  harness (1 thread).
- **No worker fan-out inside the barrier in MVP.** Migration runs
  serially on the game-loop thread; this resolves `frame-phases.md`
  Open Q #5 in the negative for MVP — the determinism cost of a worker
  fence (`std::atomic_thread_fence` plus a per-row dependency graph)
  is worse than the one-frame stall we accept.

### 6.2 Rendezvous protocol — "every worker at safepoint before swap"

Even though MVP runs systems on a single worker, the barrier's design
must describe the rendezvous so the post-MVP per-system-parallelism
plan (SPEC §6.10) drops in without re-spec'ing the barrier.

The rendezvous is **already implicit in frame-phases**: phases 1–7 of
frame N have completed (i.e. all worker threads have joined back to
the driver thread at phase 7's exit barrier) before phase 8 begins.
Concretely:

1. **Schedule-level fence.** `Schedule::run_phase(N)` exits only after
   every system in phase N has reported completion. In MVP this is
   trivially true because the driver thread runs them all; post-MVP
   the fork-join wrapper emitted by `glibre-foryc` (§6.4 of SPEC) inserts
   `std::atomic_thread_fence(memory_order_acq_rel)` at phase-N exit.
2. **Phase-7 exit guarantee.** `frame-phases.md` row 7 states "command
   buffer for frame N is enqueued to the GPU; sim-side reads of
   `RenderFrame` are done." This is the safepoint the barrier requires.
3. **Barrier-entry assertion.** In debug builds, `step_cold` asserts
   `World::is_quiescent()` (no live `Query` iterators, no system body
   on stack, no `CommandBuffer` mid-append) before Drain. The assertion
   uses thread-local counters that systems bump on entry / exit.
4. **Drain watchdog.** If a plugin's `glibre_plugin_drain` does not
   return within the 0.10 ms cell, the barrier records
   `HotReloadDrainTimeout` and refuses. This is a fail-stop watchdog,
   not a cooperative one — the plugin is given exactly one budget
   slice. The watchdog is implemented as a `std::chrono::steady_clock`
   sample at drain entry plus a comparison at drain exit (no preempting
   timer; the budget is enforced post-hoc, the failure is logged with
   the actual measured time, and the swap is aborted before any
   irreversible state change).

### 6.3 Mutual exclusion

- `pending_requests`, `statuses`, `observers`, `migration_arena`, and
  `txn_log` are all owned by the barrier and accessed from the
  game-loop thread only. No locks needed.
- `pending_count` is `std::atomic<std::uint32_t>`; producers store with
  `release`, the consumer loads with `relaxed` because the consumer is
  the game-loop thread and the only happens-before edge needed is
  "request enqueued before phase-8 entry of the next frame", which is
  established by the producer storing into `pending_requests` (an
  EASTL vector under a tiny mutex) before bumping `pending_count`.
- The producer-side mutex around `pending_requests` is the only lock
  on the barrier. Held for `O(1)` time per enqueue (string copy +
  vector push). Lock contention is bounded by the producer count
  (`<= 7` threads in MVP).

### 6.4 Observer reentrancy

Observers are called synchronously between Resume substeps 4.2 and
4.3. Subscribers must obey:

- **No recursive `request_reload` from inside an observer callback.**
  Doing so deadlocks the barrier (the producer-side mutex is reentrant
  on POSIX but we forbid it for SRP — the barrier is a state machine,
  not a workflow engine). Enforced by an `eastl::vector<bool>`
  reentrancy guard set in `step_cold` and unset on return; reentrant
  calls return `unexpected(core::Error::HotReloadRefused)` with a
  detail string `"reentrant request_reload"`.
- **No `World` mutation from inside an observer callback.** The
  callback receives a `const HotReloadEventVariant&`; it does not get
  a `World&`. Editors that want to refresh their views push a
  deferred command into the next frame's input phase.
- **No long-running work.** Observer dispatch is on the critical path
  of the reload frame's 0.40 ms one-shot budget. Subscribers should
  do `O(1)` bookkeeping (latch a flag, push a UI event into a
  ring buffer) and return.

### 6.5 Self-reload guard

`HotReloadSelfReference` (SPEC §10) — the barrier itself lives in
`core`; reloading core is out of scope. The guard is implemented as a
fast-path `eastl::string_view{"glibre.core"} == plugin_fqn` test in
`request_reload`. Future "metaloader" work (`hot-reload-protocol.md`
Open Q #2) lifts the restriction at the cost of a second-level
middleman observing the loader being swapped — explicitly post-MVP.

## 7. Persistence + ABI

### 7.1 What the barrier validates pre-swap (Step 2 staging)

The barrier (via the loader staging path) validates the candidate
dylib against three contractually-stable wire surfaces before any
state mutation:

1. **`glibre_types_abi_hash` byte-equality.** The candidate exports
   `glibre_plugin_abi_hash` (a 64-char blake3 hex string compiled in
   from `glibre-types.dylib`'s headers). The barrier compares it
   byte-for-byte against the host's `glibre_types_abi_hash()`.
   Mismatch → `PluginAbiHashMismatch` (refused under
   `HotReloadRefused`). `fory-codegen.md` "ABI Hash Function" defines
   the inputs to the digest (sorted `(fqn || ":" || version_le ||
   ":" || schema_source_blake3)`); the barrier never computes the
   digest, only compares. PHILOSOPHY §9.
2. **`PluginManifest` Fory deserialization.** The barrier asks
   `glibre::types::deserialize<PluginManifest>(span{ptr, size})` over
   the `.rodata`-embedded blob. The Fory pipeline validates tags,
   field versions, and union arms per `plugin-abi.md` "Plugin Manifest
   Schema". Failure → `PluginManifestInvalid` (refused under
   `HotReloadRefused`).
3. **Manifest's `abi_hash` field byte-equality with both the
   redundant symbol and the host hash.** A defensive double-check
   guards against a tampered or corrupt manifest whose declared
   `abi_hash` disagrees with the compiled-in symbol. `plugin-abi.md`
   "Loader Sequence" step 4 already requires this; the barrier
   inherits it.

In addition, the barrier validates a fourth surface that is
barrier-specific (not loader-specific):

4. **`schema_version` per surviving storage.** For every middleman
   component type T that exists in `World` storage at the moment of
   the swap, the barrier confirms Q's `ComponentDecl{fqn=T, ...}.
   schema_hash` either equals the stored version's
   `schema_hash` (identity case — no migration needed) or is
   reachable via the static migration table (`fory-codegen.md`
   "Migration Mechanic"). Unreachable → `SchemaMigrationFailed`
   (refused). The check runs **before** Step 3 actually mutates any
   row; pre-flight failure leaves the world byte-equal.

### 7.2 ABI-hash gate is a wire-level contract, not a build-level one

The four inputs to `glibre_types_abi_hash` (per `fory-codegen.md`)
are *only* schema source hashes plus version numbers. The hash is
deliberately decoupled from:

- compiler identity (`clang-21` vs `clang-22`),
- compile flags (`-O0` vs `-O3`, `-fsanitize=*`),
- header timestamps,
- libc++ minor version,
- the engine's own SemVer (`glibre-core` version).

Two collapses to honour: (a) ABI-hash decoupling means a release-build
plugin can hot-reload into a debug-build engine and vice versa, which
the e2e harness relies on; (b) the engine's SemVer is gated separately
via `min_engine_version` (the SemVer-comparison check; refused under
`PluginEngineTooOld`). Three independent version axes (`abi_hash`,
plugin SemVer, engine SemVer) — `plugin-abi.md` "Versioning Rules"
already locks this.

### 7.3 Pre-swap state snapshot (in-memory only)

The barrier captures a per-storage snapshot reference (not a byte
copy) before Step 3 mutates rows. The reference is:

- pointer + length per chunk per archetype that holds T,
- the chunk's pre-mutation `last_modified` `ChangeTick` array (4 KiB
  per archetype × ~256 archetypes = ≤1 MiB total),
- the migration arena's "high water mark" pointer.

Rollback (§4.4 Step 3 failure) restores rows by:

1. Re-running the **inverse migration chain** from current → stored
   for any rows already overwritten (the originating context
   declares per-schema invertibility per
   `hot-reload-protocol.md` "Failure & Rollback" step 4);
2. Resetting the migration arena to the high-water mark;
3. Decrementing each touched chunk's `ChangeTick` to the snapshotted
   value so `Changed` filters in frame N+1 do not see ghost edits.

For non-invertible schemas (declared so by the originating context),
mid-Step-3 failure is a process termination per
`hot-reload-protocol.md` "Failure & Rollback" — silent recovery would
mask bugs the deterministic-snapshot contract cannot tolerate. Loud,
immediate, in CI.

### 7.4 Persisted barrier metadata

`HotReloadCheckpoint.fory` (SPEC §7.1) is the only barrier-authored
schema; it is **diagnostic-only**, not consulted at startup. Captured
between Drain and Swap, finalized at Resume, written exclusively to
crash-report bundles. Fields verbatim from §7.1:

```
plugin_fqn, old_abi_hash, new_abi_hash, frame_index, current_tick,
migrated_types, carryover_payload
```

The `carryover_payload` is opaque to the barrier — the originating
context's Fory-serialized snapshot, dispatched through the data
context's `MigrationChain`. Core does not interpret bytes.

`LoadedPluginRecord.fory` (SPEC §7.1) is bookkeeping owned by the
loader, not the barrier. The barrier only triggers re-emission of
the record after a successful Resume (so crash dumps contain the
post-reload load set).

### 7.5 What the barrier does **not** persist

- Component data (owned by the originating plugin's schema family).
- World snapshots (owned by the future `data`/`content` snapshot
  story, not by core; SPEC §7.2).
- Migration scratch buffers (arena-only, never published).
- `PluginManifest` blobs (owned by the loader; the barrier reads
  through the loader and never re-serializes).

## 8. Hot-reload (meta) — interlock with loader and migration dispatcher

The barrier sits at the centre of three other aggregates' work; this
section pins the interlocks.

### 8.1 Interlock with `PluginLoader` (#704)

`PluginLoader` and `HotReloadBarrier` are peer aggregates inside the
`plugin/` and `hot-reload/` sub-modules respectively (SPEC §6.1). The
seam between them is exactly two function calls:

```cpp
// plugin/loader.hpp — internal facade.

namespace glibre::core::detail {

// Stage a candidate dylib without committing it to the registry.
// Performs `dlopen` (RTLD_NOW | RTLD_LOCAL), `dlsym` of the four
// entry points, manifest deserialize, and the ABI hash + engine
// version + name + dependency checks. Returns a stage handle the
// barrier hands back at commit / abort time.
[[nodiscard]] Result<StagedPluginHandle>
PluginLoader::stage_candidate(
    eastl::string_view             plugin_fqn,
    const std::filesystem::path&   replacement_dylib_path) noexcept;

// Commit the staged candidate, replacing the live plugin's vtable
// pointer atomically and appending its TypeRegistry / SystemRegistry
// entries. Caller (the barrier) holds the world's exclusive write
// lock; this is a relaxed atomic store.
[[nodiscard]] Result<void>
PluginLoader::commit_swap(StagedPluginHandle s) noexcept;

// Drop the staged candidate without committing (called on Drain
// failure or candidate validation failure). dlcloses the candidate.
void
PluginLoader::abort_stage(StagedPluginHandle s) noexcept;

// Roll back a previously-committed swap (called on Migrate or Resume
// failure). Reverses the type-registry appends, restores the prior
// vtable, and dlcloses the new candidate.
[[nodiscard]] Result<void>
PluginLoader::rollback_swap(StagedPluginHandle s) noexcept;

}  // namespace glibre::core::detail
```

Why three states (stage / commit / abort) instead of a single load
call: the barrier needs to validate manifests and hashes **before**
calling Drain on the outgoing plugin, but it must not commit the
swap until Drain succeeds. Three states make the loader's
preconditions for each transition explicit and let the barrier abort
cleanly when Drain refuses.

The loader does not know about phase 8 timing; the barrier is the
only caller of these four functions. Other entry points to the
loader (`load`, `unload`, `list` from §5.9) run at process startup
and shutdown only, never inside phase 8.

### 8.2 Interlock with the migration dispatcher (data context)

The migration dispatcher lives in `glibre-types.dylib` per
`fory-codegen.md` "Migration Mechanic". The barrier consumes one
function per `(fqn, fromVersion, toVersion)` triple, looked up in the
static table:

```cpp
namespace glibre::types {

using MigrateFnPtr = std::expected<void, glibre::Error> (*)(
    const void* src,
    void*       dst,
    Arena&      scratch) noexcept;

[[nodiscard]] eastl::span<const MigrateFnPtr>
lookup_migration_chain(
    TypeFqn          fqn,
    SchemaVersion    from,
    SchemaVersion    to) noexcept;  // empty span = chain unreachable.

}  // namespace glibre::types
```

The barrier:
1. Calls `lookup_migration_chain` once per type T being migrated,
   pre-flight (§7.1 row 4 above). Empty span → `SchemaMigrationFailed`,
   refuse, no rows touched.
2. Per row, walks the returned span: allocate from arena, call the
   pointer with `(&src_row, &dst_row, arena)`, `expected.value()` →
   continue, `unexpected(err)` → roll back per §7.3.
3. Resets the arena to the high-water mark on terminal failure or
   on per-type completion (whichever happens first), so per-row scratch
   never accumulates.

The dispatcher is **never** invoked outside the barrier. Plugins that
want migration-shaped logic (e.g. asset-format upgrades) author a
schema bump and a migration body; they do not reach into
`lookup_migration_chain`.

### 8.3 Interlock with the type registry

The barrier mutates the `TypeRegistry` exactly once per reload, at
Step 2.4: appends Q's type registrations (per `plugin-abi.md`
"Loader Sequence" step 8.4-equivalent inside the loader's
`commit_swap`). The registry is append-only within a session
(SPEC §4.9 invariant 1). On rollback the appended entries are
removed by `PluginLoader::rollback_swap`; this is an internal
relaxation of "append-only" that is safe because the appended
entries are observable only after `commit_swap` returns and are
removed before any system runs.

### 8.4 Interlock with the world's `ChangeTick` clock

The barrier does **not** advance `ChangeTick`. The clock advances
exactly once per frame, in phase 9 (`frame-phases.md` row 9
"world ChangeTick increment"). Migrated rows therefore do not become
visible as `Changed` until frame N+1. Editors and e2e harnesses
that observe `Changed` filters do not see double-counting from the
swap.

### 8.5 No interlock with `CommandBuffer`

Phase 8 does not run system bodies, so no `CommandBuffer` is in
flight. Outgoing plugin's `glibre_plugin_drain` may not allocate a
`CommandBuffer` (no `World&` write lock in the public API for
plugin code). If a plugin's drain body needs to "delete a row" the
correct path is to mutate the row in place to a tombstone and let
the next frame's GC system reclaim it; this is a plugin-internal
discipline, not a barrier responsibility.

## 9. Performance

### 9.1 Budget — quoted from `perf-budget.md`

The barrier inherits the `core` row of the per-context budget:

| Cell                              | Value            | Source                                          |
|-----------------------------------|------------------|-------------------------------------------------|
| Phase-8 idle steady-state         | `< 0.1 ms`       | `perf-budget.md` "Pipelined Frame Timing" line 8 / `frame-phases.md` Consequences |
| Phase-8 reload-frame one-shot     | `<= 0.40 ms`     | `perf-budget.md` "Justification Per Cell" — core / "S2 hot-reload" + "CI Gate Spec" #4 |
| Migration arena ceiling           | 16 MiB           | `perf-budget.md` "Allocator Rules" #6           |
| Migration arena ContextTag        | `core`           | `perf-budget.md` "Allocator Rules" #1           |
| Heap accounting against `core` 64 MiB cell | yes     | `perf-budget.md` "Per-Context Budget Table"     |
| Concurrency model                 | single-threaded  | `perf-budget.md` "Justification Per Cell" — core; `hot-reload-protocol.md` Consequences |

The reload-frame cost is permitted to skirt the steady-state headroom
exactly once per reload (`perf-budget.md` "Pipelined Frame Timing"
note, "the reload path itself is permitted to overrun a single
frame's budget exactly once per reload"). Beyond that one frame the
plugin's first post-reload frame must fit the steady-state budget;
the e2e harness asserts this.

### 9.2 Per-substep allowance (within the 0.40 ms reload one-shot)

| Substep                                                | Allowance      | Notes |
|--------------------------------------------------------|----------------|-------|
| Hot path (idle)                                        | sub-µs         | one relaxed load |
| Drain (per plugin)                                     | `<= 0.10 ms`   | watchdog limit; overrun → `HotReloadDrainTimeout` |
| Swap (loader stage + commit, per plugin)               | `<= 0.05 ms`   | `dlopen`/`dlsym` are macOS dyld-bounded; manifest deserialize is bounded by `PluginManifest` size (<8 KiB typical) |
| Migrate (per plugin)                                   | `<= 0.20 ms`   | dominates the reload-frame cost; per-row cost × row count; 16 MiB arena ceiling |
| Resume (per plugin)                                    | `<= 0.05 ms`   | `register` body is per-plugin; observer dispatch ≤10 subscribers in MVP |
| Plugin total (one plugin per reload-frame in MVP)       | `<= 0.40 ms`   | sums to the perf-budget cell |

Multi-plugin reload-frames are permitted; the per-plugin transactions
are independent (`hot-reload-protocol.md` "Failure & Rollback" —
per-plugin atomicity, not phase-wide). The 0.40 ms ceiling is
per-frame (not per-plugin); a reload-frame that contains two plugin
transactions must fit both within 0.40 ms or else the second plugin's
transaction is deferred to the next phase 8 (still in `pending_requests`,
no refusal). This deferral is invisible to callers — `request_reload`
returns `Pending` until eventually `Completed` or `Refused`.

### 9.3 Allocator behaviour

- **All barrier allocations carry the `core` `ContextTag`** per
  `perf-budget.md` "Allocator Rules" #1.
- **Migration arena counts against `core`'s 64 MiB heap cell** per
  Allocator Rules #6, with a 16 MiB sub-ceiling. Strict-mode debug
  builds return `OutOfBudget` if the arena pushes the `core` tag
  over its 64 MiB cap (this is distinct from the migration-arena
  ceiling itself, which surfaces as `SchemaMigrationFailed`).
- **No raw `new` / `malloc`** inside the barrier source tree per
  the `-Wglibre-no-raw-alloc` build flag (Allocator Rules
  preamble).
- **Observer event POD payloads** (subscriber-visible
  `HotReloadStarted/Completed/RefusedEvent` structs) are stack-
  allocated; the spans they carry (`migrated_types`) point into the
  barrier's `txn.migrated_types` vector and are valid only for the
  duration of the synchronous callback.

### 9.4 CI gate

`perf-budget.yml` "CI Gate Spec" #4 covers the barrier directly:

> Hot-reload frame budget. A scripted reload (S2) runs on the
> nightly job; the reload-frame phase 8 cost must be ≤ 0.40 ms.
> PR fails if exceeded.

The barrier's plan-level Catch2 `BENCHMARK` block (§11 below) asserts
both the idle (`< 0.1 ms`, p99) and the reload-frame (`≤ 0.40 ms`)
ceilings against the S2 fixture.

## 10. Failure modes

The barrier maps every refusal to an arm of `core::Error` (SPEC §10
table). All barrier-originated refusals are wrapped under the
umbrella `core::Error::HotReload` so consumers can both pattern-match
"a hot-reload was refused" and inspect the specific cause.

### 10.1 Refusal arms (enumerated)

| Cause                                                | Detected at           | Arm                                       | Wrapped under | Recovery                                                                 |
|------------------------------------------------------|------------------------|-------------------------------------------|---------------|---------------------------------------------------------------------------|
| `request_reload(plugin_fqn == "glibre.core")`        | `request_reload`       | `HotReloadSelfReference`                  | (direct)      | Caller fixes the request; barrier never even attempts swap                |
| Drain budget exceeded (>0.10 ms)                     | Step 1 watchdog        | `HotReloadDrainTimeout`                   | `HotReload`   | Plugin author shortens drain; refused this frame, retry next phase 8     |
| `dlopen` of candidate failed                         | Step 2.1 (loader)      | `PluginDlopenFailed`                      | `HotReload`   | Operator re-builds candidate; refused                                     |
| Required entry-point symbol missing                  | Step 2.1 (loader)      | `PluginMissingEntryPoint`                 | `HotReload`   | Operator re-links candidate; refused                                      |
| Manifest deserialize failed                          | Step 2.1 (loader)      | `PluginManifestInvalid`                   | `HotReload`   | Operator regenerates manifest via `glibre-foryc`; refused                |
| ABI hash mismatch (manifest, symbol, or host)        | Step 2.1 (loader)      | `PluginAbiHashMismatch` / `HotReloadAbiHashMismatch` (synonym pre-MVP, §10.4) | `HotReload`   | Operator rebuilds candidate against current `glibre-types.dylib`; refused |
| `min_engine_version` > host engine version           | Step 2.1 (loader)      | `PluginEngineTooOld`                      | `HotReload`   | Operator updates engine or downgrades plugin's minimum; refused           |
| Plugin name collision                                | Step 2.1 (loader)      | `PluginNameCollision`                     | `HotReload`   | Operator renames or unloads conflicting plugin; refused                   |
| Unmet `depends_on`                                   | Step 2.1 (loader)      | `PluginDependencyMissing`                 | `HotReload`   | Operator loads dependency first; refused                                  |
| Dependency cycle                                     | Step 2.1 (loader)      | `PluginDependencyCycle`                   | `HotReload`   | Operator breaks cycle; refused                                            |
| Manifest superset check failed (Q dropped a type)    | Step 2.2               | `SchemaMigrationFailed`                   | `HotReload`   | Operator restores type or schedules major-version migration; refused      |
| Migration chain unreachable for a surviving type     | Step 3.1 pre-flight    | `SchemaMigrationFailed`                   | `HotReload`   | Originating context ships a migrate function for the missing step; refused |
| Migrate function returns `unexpected(...)`           | Step 3.2               | `SchemaMigrationFailed`                   | `HotReload`   | Originating context fixes invariant violation; refused                    |
| Migration arena exhausted                            | Step 3.2 alloc         | `SchemaMigrationFailed`                   | `HotReload`   | Operator reduces row count or increases arena (post-MVP); refused          |
| `glibre_plugin_register` returns `unexpected(...)`   | Step 4.2               | `PluginInitFailed`                        | `HotReload`   | Plugin author fixes register-time invariant; refused                      |
| Schedule cycle after Q's system registrations        | Step 4.2 schedule build | `SystemScheduleCycle`                    | `HotReload`   | Plugin author breaks intra-phase cycle; refused                           |
| Migration arena oversized vs `core` 64 MiB heap cell | any allocation         | `OutOfBudget`                             | `HotReload`   | Operator profiles; refused                                                |

`HotReload` is the umbrella; refused arms always set both
`Error::variant()` to the specific arm and wrap it under
`HotReload` via the `error-model.md` `ErrorContext::detail` payload.
Consumers may match either.

### 10.2 Logging

Every refusal logs exactly once via `glibre::log_error(err, level::warn)`
per `error-model.md` "Logging / Telemetry" — `warn` level, never
`error`, because the engine continues running on the prior plugin.
Structured fields per `hot-reload-protocol.md` "Refusal Cases":

- `plugin_fqn`,
- `attempted_dylib_path`,
- `host_abi_hash`,
- `plugin_abi_hash`,
- `cause` (the arm enumerator name),
- `detail` (free-form, e.g. `dlerror()` text or the failed migrate
  function's name).

### 10.3 Termination cases (loud, not refusal)

Three contract violations terminate the process:

1. **Step 2.3 vtable-store fault.** Single relaxed store under
   exclusive ownership cannot fail by construction. If it does, the
   loader's invariants are broken; terminate with a diagnostic core
   dump.
2. **Step 3 mid-row failure with non-invertible migration declared.**
   Per `hot-reload-protocol.md` "Failure & Rollback" — silent recovery
   would mask bugs the deterministic-snapshot contract cannot
   tolerate. Terminate.
3. **Observer reentrancy beyond depth 1.** A subscriber that calls
   `request_reload` is one bug; one that recurses indefinitely is
   another. Depth check in `step_cold` terminates after one
   reentrant call (the first reentrant call returns
   `unexpected(HotReloadRefused)`; a second is structurally
   impossible without a misuse of the `unsubscribe` path, which
   terminates).

These three cases are the only barrier-side terminations. Everything
else is a refusal.

### 10.4 Notes on naming consistency

The SPEC §5.1 enum carries both `core::Error::HotReload` (umbrella)
and `core::Error::HotReloadAbiHashMismatch` (specific). The
`plugin-abi.md` "Failure Modes" table maps the same condition to
`PluginAbiHashMismatch`. The two names are synonyms in MVP — the
loader and the barrier detect the same condition at different call
sites and the SPEC pre-allocated names for both.
`task-breakdown-core-hot-reload-barrier-detailed` (the sibling
spike) will reconcile the names in code (one canonical arm, the
other an alias) without changing the public surface; this is a
plan-level concern.

## 11. Test plan

Tests live under `tests/core/hot_reload/` (Catch2). Coverage targets:
the rendezvous protocol, every refusal arm in §10, the observer
contract, the migration-arena lifecycle, and the perf-budget
ceilings. Stories drive the suite (PHILOSOPHY workflow); the
`type:plan` issues spawned by the sibling task-breakdown spike will
host the per-test acceptance criteria.

### 11.1 Unit tests

| Test name                                                 | What it asserts                                                                                          |
|-----------------------------------------------------------|----------------------------------------------------------------------------------------------------------|
| `hot_reload_step_idle_is_relaxed_load`                    | `step()` with `pending_count == 0` returns `0`, performs no allocations, no observer dispatch (verified via mock allocator + observer counter). |
| `hot_reload_request_coalesces_within_frame`               | Two `request_reload` calls on the same `plugin_fqn` in one frame return the same `ReloadRequestId` and result in one Completed event. |
| `hot_reload_request_self_reference_refused`               | `request_reload("glibre.core", ...)` returns `unexpected(HotReloadSelfReference)`; barrier remains idle.   |
| `hot_reload_status_lru_evicts_at_256`                     | After 257 unique requests the oldest status is evicted; barrier reports `is_idle()` after the queue drains. |
| `hot_reload_observer_subscribe_unsubscribe_idempotent`     | `subscribe`+`unsubscribe` round-trips with stable handles; double-unsubscribe returns `unexpected(...)` without crashing. |
| `hot_reload_observer_reentrancy_refused`                  | An observer that calls `request_reload` during its callback receives an immediate `unexpected(HotReloadRefused)` and the outer transaction continues. |
| `hot_reload_arena_resets_between_rows`                    | After Step 3 completes for one type, the arena's high-water mark equals its start mark; a second type starts at the same offset. |
| `hot_reload_arena_exhaustion_refuses`                     | A migration that requests >16 MiB arena returns `SchemaMigrationFailed`; world rows are byte-equal to pre-swap. |

### 11.2 Integration tests (per refusal arm)

One test per arm in §10.1, using fixture plugins under
`tests/e2e/plugins/`. Fixture roster (extending the trio
`hot-reload-protocol.md` "Test Hooks" already names):

| Fixture                            | Triggers                                                              |
|------------------------------------|-----------------------------------------------------------------------|
| `bad-abi-hash`                     | `PluginAbiHashMismatch`                                               |
| `failing-init`                     | `PluginInitFailed`                                                    |
| `v1-to-v2-migration`               | Happy-path schema migration                                           |
| `slow-drain`                       | `HotReloadDrainTimeout` (drain body sleeps past 0.10 ms)              |
| `dropped-component`                | `SchemaMigrationFailed` (manifest superset check)                     |
| `missing-migration-chain`          | `SchemaMigrationFailed` (no chain registered)                         |
| `force-migrate-fail`               | `SchemaMigrationFailed` mid-row, exercises rollback                   |
| `arena-bomb`                       | `SchemaMigrationFailed` from arena exhaustion                         |
| `dlopen-broken`                    | `PluginDlopenFailed`                                                  |
| `missing-entry-point`              | `PluginMissingEntryPoint`                                             |
| `corrupt-manifest`                 | `PluginManifestInvalid`                                               |
| `engine-too-old`                   | `PluginEngineTooOld`                                                  |
| `name-collision`                   | `PluginNameCollision`                                                 |
| `dep-missing`                      | `PluginDependencyMissing`                                             |
| `dep-cycle`                        | `PluginDependencyCycle`                                               |
| `schedule-cycle`                   | `SystemScheduleCycle`                                                 |
| `non-invertible-mid-row-fail`      | Process termination (loud); CI asserts the exit code + log line       |

Each integration test:

1. Invokes `glibre::core::test::enqueue_hot_reload(...)` with the
   fixture path.
2. Drives one frame to completion via the e2e harness.
3. Calls `await_reload(id)` and asserts the resulting
   `ReloadStatus.outcome == Refused` (or `Completed` for the happy
   paths) with the expected `cause` arm.
4. Asserts the prior plugin (or the new plugin, on success) is the
   one ticking on the next frame, via a fixture-side counter that
   the plugin bumps in its system body.
5. Asserts a single `warn`-level log line per refusal with the
   expected structured fields (`plugin_fqn`, `cause`, etc.).

Schema migration goldens follow `hot-reload-protocol.md` "Test Hooks"
exactly: `vN payload → migrated → re-serialize` is byte-equal to a
recorded `vN+1 payload`. One golden per fixture's migration chain.

### 11.3 Rendezvous-protocol tests

Cover the §6.2 rendezvous explicitly so the post-MVP parallelism
plan does not regress the contract:

| Test name                                                  | What it asserts                                                                                  |
|------------------------------------------------------------|--------------------------------------------------------------------------------------------------|
| `barrier_asserts_quiescence_at_phase_8_entry`              | A test-only worker-thread counter starts at 0 at phase-8 entry; the assertion fires (debug build) if a phantom thread bumps it. |
| `request_reload_is_safe_from_producer_thread`              | Spawn 4 producer threads each calling `request_reload`; observe that the count of completed reloads equals the count of unique `plugin_fqn`s requested (coalesce-on-fqn). |
| `pending_count_release_acquire_visibility`                 | Producer's release-store on `pending_count` is observable by the next phase-8 relaxed load (verified by ThreadSanitizer + an explicit fence test). |
| `drain_watchdog_logs_actual_measured_time`                 | `HotReloadDrainTimeout` log carries the measured drain duration, not just the budget value.       |

### 11.4 Perf benchmarks

Catch2 `BENCHMARK` blocks per `perf-budget.md` "CI Gate Spec":

| Benchmark                                       | Asserts                                                                          |
|-------------------------------------------------|----------------------------------------------------------------------------------|
| `hot_reload_idle_step_p99_under_0_1_ms`         | 600-frame run, idle phase 8, p99 wall-clock `< 0.1 ms`                           |
| `hot_reload_reload_frame_under_0_4_ms`          | One scripted reload via `enqueue_hot_reload`; phase 8 wall-clock `<= 0.40 ms`     |
| `hot_reload_arena_resize_zero_alloc_idle`       | Idle phase 8 records zero allocations under the `core` ContextTag (mock allocator) |

These run in the nightly job; failures block the next morning's PRs
until investigated.

### 11.5 Observer contract tests

| Test                                              | What it asserts                                                                                        |
|---------------------------------------------------|--------------------------------------------------------------------------------------------------------|
| `observer_sees_completed_after_register_returns`  | A subscriber observing `HotReloadCompleted` finds Q's systems already in the schedule and Q's component types in the type registry. |
| `observer_sees_refused_with_specific_cause`       | A subscriber observing `HotReloadRefused` finds `cause` set to the specific arm (not just the umbrella). |
| `observer_callback_holds_no_world_lock`           | A subscriber's callback that calls `World::is_alive(...)` succeeds; the callback receives `const HotReloadEventVariant&` only. |

### 11.6 Story closure

This design satisfies (when implemented) the §11 acceptance-criteria
slot in SPEC for the barrier aggregate. The sibling
`task-breakdown-core-hot-reload-barrier-detailed` spike will spawn
`type:plan` issues whose tests come from §11 of this design plus the
S2 fixture in `e2e/perf/`. Stories that close on the barrier are the
ones already enumerated in the core SPEC §11; this design adds no
new acceptance criteria.

## 12. Open questions

- `[OPEN]` **Migration arena: grow vs refuse.** Refuse in MVP per
  `perf-budget.md` Allocator Rules #6 and §4.4 Step 3. Whether to
  permit arena growth on demand inside one phase (post-MVP) is
  tracked under #499
  (`hot-reload-migration-arena-grow-vs-refuse`); the barrier's
  public surface is forward-compatible with either resolution.
- `[OPEN]` **Core-owned types — migration policy.** Two
  HotReloadCheckpoint / LoadedPluginRecord schemas in §7 carry
  diagnostic-only data. Whether their migration policy is "refuse if
  unreachable" (current default) or "permit best-effort recovery
  with a warning" is owned by #492
  (`core-owned-types-hot-reload-migration-policy`); resolution
  changes only the migration-body contract, not the barrier's state
  machine.
- `[OPEN]` **Self-reload of `core` (the metaloader).** Out of scope
  for MVP per `hot-reload-protocol.md` Open Q #2. The
  `HotReloadSelfReference` guard rejects at the request site; lifting
  the rejection requires a second-level middleman able to observe
  the loader being swapped. Defer until a story asks for it.
- `[OPEN]` **Inverse migration grammar.** Whether `.fory` schemas
  declare invertibility via an explicit clause or by static analysis
  of the forward migration body is owned by `data` SPEC §7 / 
  `hot-reload-protocol.md` Open Q #3. The barrier consumes whatever
  signal the data context exposes; no barrier change is needed when
  the policy lands.
- `[OPEN]` **Concurrent reloads of inter-dependent plugins.** Phase 8
  reloads in registration order today; if plugin A's `register`
  reads plugin B's vtable and both reload in the same phase, we need
  a topological order over `pending_requests`. MVP does not require
  this (all plugin dependencies are core-only); revisit when the
  first such dependency exists per `hot-reload-protocol.md`
  Open Q #4.
- `[OPEN]` **Editor-driven partial reload (asset / shader only).**
  An editor wants to swap a single shader without re-running the
  full plugin protocol. The likely answer is a separate "asset
  reload" path in phase 1 (`hot-reload-protocol.md` Open Q #5),
  outside this aggregate. The barrier remains plugin-dylib-only.
- `[OPEN]` **Cancellation of a pending request.** Cheap to add (a
  flag on `ReloadRequest`), but no MVP story asks for it
  (`hot-reload-protocol.md` Open Q #6). The public surface above
  does not expose cancellation; adding it is additive.
- `[OPEN]` **Self-reload guard naming.** SPEC §10 reserves
  `HotReloadSelfReference`. Whether to also check non-`glibre.core`
  self-reference patterns (e.g. a plugin reloading itself from
  within its own observer callback) is bounded by the §6.4
  reentrancy guard; no separate arm needed in MVP.
- `[OPEN]` **`HotReloadAbiHashMismatch` vs `PluginAbiHashMismatch`
  reconciliation.** Synonyms in SPEC §10 today; the implementation
  plan should pick one canonical arm and alias the other for ABI
  stability. No design decision required here.
