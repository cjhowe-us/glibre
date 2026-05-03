# Decision Record — Hot-Reload Protocol

- Refs: spike #7 (decide-hot-reload-barrier), parent sub-epic #3, epic #2
- Owner context: `core` (plugin loader)
- Depends on: `frame-phases.md` (phase 8 barrier), `error-model.md`
  (`std::expected<T, glibre::Error>`), `fory-codegen.md`
  (`glibre-types.dylib` middleman)

## Status

Accepted (spike output). Implementation deferred to plan issues spawned
by `task-breakdown-core-plugin-loader`. Resolves open questions 1 and 5
of `frame-phases.md`.

## Context

PHILOSOPHY §8 makes hot-reload at frame boundaries a first-class
engine concern; PHILOSOPHY §3 forces every domain into a `.dylib` and
gives the core sole authority over the loader and the type registry;
PHILOSOPHY §9 demands refusal on ABI hash mismatch. The frame-phases
record locks the barrier at phase 8 (after `render-submit`, before
`present`), giving us exactly one point per frame where the loader
may swap plugin code without observing partially-recorded GPU command
buffers or torn ECS storage.

What remains is the protocol that runs *inside* phase 8 when at least
one reload is requested:

1. How is in-flight work drained so the swap is observably atomic from
   the perspective of every other phase and every plugin observer?
2. Which state survives the swap, and which is re-derived from
   surviving state?
3. What is the migrate-function contract that the originating context
   must satisfy for every persistent component / asset / singleton it
   owns?
4. Which refusal cases reject the swap and leave the previous-good
   plugin running, and how is that surfaced to operators / tests?
5. How are observers (other plugins, the editor, the e2e harness)
   notified atomically — i.e. without ever seeing a half-swapped
   world?
6. What is the failure / rollback policy when migration fails partway
   through, and how is it tested deterministically?

The error-model decision already gives us the surface: every refusal
returns `std::expected<void, glibre::Error>` whose payload is one of
the three `core::Error` arms named in PHILOSOPHY §9 + the schema
migration arm from `fory-codegen.md`. The fory-codegen decision
already gives us the schema-stable layer: `glibre-types.dylib`
exposes `glibre_types_abi_hash()` and a per-type migration table, so
the loader never needs Fory itself and never needs plugin-private
headers to migrate component storages.

This record locks the protocol that ties those pieces together.

## Decision

Phase 8 implements a four-step state machine — **drain → swap →
migrate → resume** — driven by the loader on the game-loop thread.
The state machine is a true no-op when no reload is pending (answers
`frame-phases.md` open question 1: a single relaxed atomic load on
the pending-reload counter, no fence, no cache flush). When at least
one reload is pending, the four steps run to completion or roll back
atomically; partial application is forbidden.

The swap is observably atomic from every non-loader vantage point:
- Phases 1–7 of frame N already completed before phase 8 begins.
- Phase 9 of frame N has not yet begun; it sees only post-swap
  vtables when it runs.
- Plugin code does not execute during phase 8 except (a) the
  outgoing plugin's `glibre_plugin_drain` callback during step 1,
  (b) the originating context's pure migrate functions during step 3,
  (c) the incoming plugin's `glibre_plugin_register` during step 4.
  No system bodies run.

State that survives the swap is exactly the state owned by core or
by `glibre-types.dylib`: ECS archetype storages (component bytes),
asset handles, world singletons declared as middleman types, the
plugin registry itself, and the next-frame input buffer. State that
does not survive — caches, JIT-derived dispatch tables, GPU resource
handles internal to a plugin — must be re-derived by the new plugin
in `glibre_plugin_register`.

Refusal cases (three, enumerated below) abort at the earliest step
that detects them, leave the previous-good plugin instance live and
linked, log a structured `warn`-level entry per the error-model
record, and return `core::Error::HotReloadRefused` with a nested
cause. The next phase 8 may retry once the operator addresses the
refusal cause.

## Protocol Sequence

The state machine has four numbered steps. Each step's preconditions
are the previous step's postconditions; failure at step K rolls back
through steps `K..1` in reverse and yields `core::Error::HotReloadRefused`.

### Step 1 — Drain

Trigger: `pending_reloads > 0` at phase 8 entry.

Per outgoing plugin P:

1. Loader takes the world's exclusive write lock (already held since
   no system runs in phase 8; this is documentation, not contention).
2. Loader calls `P::glibre_plugin_drain(World&) noexcept ->
   std::expected<void, glibre::Error>`. The plugin must:
   - Flush any internal per-frame queues into ECS components owned
     by middleman types (so the data survives the swap).
   - Release any GPU resource handles it owns; the next plugin
     will re-acquire equivalents in `register`.
   - Cancel any worker tasks it spawned and wait for them to retire.
3. Postcondition: every byte of plugin-survivable state lives in
   middleman-typed ECS storages or middleman-typed singletons; no
   thread other than the loader is inside plugin code.

Failure (drain returns unexpected): emit
`core::Error::HotReloadRefused` wrapping the plugin's error, skip
steps 2–4 for this plugin, leave it loaded. Other pending plugins
in the same phase 8 are processed independently (per-plugin
atomicity, not phase-wide).

### Step 2 — Swap

Per outgoing plugin P, incoming candidate Q:

1. Verify `Q::glibre_types_abi_hash() == host_glibre_types_abi_hash`.
   Mismatch → `core::Error::PluginAbiHashMismatch`, abort, restore
   P. Q's `dlopen`d image is `dlclose`d.
2. Verify Q's manifest's `(fqn, schema_version)` set is a superset
   or equal-set of P's surviving component storages. A subset
   means Q dropped a type P registered; that is a major-version
   change requiring a fresh world, not a hot-reload — refuse with
   `core::Error::HotReloadRefused`.
3. Atomically replace P's vtable pointer in the plugin registry
   with Q's. The vtable is a single pointer indirection from
   every system call site; the swap is one relaxed store guarded
   by the loader's exclusive ownership of phase 8.
4. Append Q's type registrations to the type registry. The
   registry is append-only within a session; Q cannot re-register
   a type P already registered with a different layout (hash check
   above guarantees layouts match).

Postcondition: every system call site dispatches to Q; no live
references to P's code remain on any thread.

### Step 3 — Migrate

For each persistent component / singleton type T owned by Q whose
`schema_version` exceeds the version recorded in the surviving
storage's header:

1. Loader queries `glibre-types.dylib`'s migration table for the
   chain `(stored_version → current_version)` for T. Missing chain
   → `core::Error::SchemaMigrationFailed`, abort, restore P
   (un-swap vtable, re-register P's types, drop Q).
2. For each row in the storage, the loader allocates a scratch
   buffer of `sizeof(T_current)` from a per-phase migration arena
   and invokes the chain step-by-step:
   `migrate_T_vN_to_vNplus1(const T_vN&, T_vNplus1&) -> Result<void>`.
3. On success of the full chain, the new bytes overwrite the row
   in place (size is bounded by the new struct's size, which is a
   compile-time constant per `fory-codegen.md` §ABI Stability).
4. On any migrate-step failure, the partially-migrated rows are
   discarded (the arena is reset, never published), and the loader
   rolls back per Failure & Rollback below.

Migrate functions are pure (no I/O, no allocation outside the supplied
arena, deterministic) per `fory-codegen.md`; the loader enforces
purity by passing only the source struct, the destination struct,
and an arena allocator — no `World&`, no logger, no clock.

### Step 4 — Resume

1. Loader calls `Q::glibre_plugin_register(World&, Registry&)
   noexcept -> std::expected<void, glibre::Error>`. The plugin must:
   - Re-acquire any GPU resources it released in P::drain.
   - Register its systems into the phase table (the registration
     API is idempotent w.r.t. system identity; re-registering the
     same `(phase, system_fqn)` is a no-op).
   - Build any internal caches keyed off middleman state.
2. Failure (register returns unexpected) is treated identically to
   a step-3 failure: full rollback of swap + migrate, P remains
   live. This is the third refusal case.
3. Loader publishes a `HotReloadCompleted{plugin_fqn, old_hash,
   new_hash, migrated_types}` event to the observer bus (see
   Observer Notification).
4. Loader decrements `pending_reloads`. When it reaches zero,
   phase 8 exits and phase 9 (present) begins.

## State Survival Rules

**Preserved across the swap** (loader guarantees these bytes are
unchanged or migrated-in-place):

- ECS archetype storages whose components are middleman types.
- World singletons declared as middleman types.
- Asset handles in the core asset registry (the handle is a stable
  `u64`; the asset's bytes live in core, not in any plugin).
- The plugin registry itself, the type registry, and the phase
  table's structure.
- The frame counter, the world tick, and the next-frame input
  buffer (already populated by phase 1 of frame N+1's predecessor
  in pipelined mode — see frame-phases §Notes).
- The PRNG state if registered as a middleman singleton (which
  the determinism contract requires).

**Re-derived by the incoming plugin in `glibre_plugin_register`**:

- GPU resource handles internal to the plugin (Metal pipeline
  states, descriptor heaps, transient buffers).
- Per-plugin caches and acceleration structures keyed off ECS
  state (BVHs, broadphase grids, audio mix graphs).
- JIT-derived dispatch tables, function-pointer caches, hash maps
  keyed off plugin-private types.
- Worker thread pools owned by the plugin (drain killed them; the
  new plugin spins them back up if it wants them).
- Any plugin-private singleton not declared via a middleman schema
  — by definition these have no on-disk format and no migration
  contract, so they cannot survive.

The rule is mechanical: **if it has a `.fory` schema, it survives;
otherwise, it does not.** This collapses the survival question to a
single check inspectable in `glibre-types.dylib`'s registry.

## Migrate Function Contract

**Signature** (generated by `glibre-foryc`, body provided by the
originating context):

```cpp
namespace glibre::<ctx> {

[[nodiscard]] std::expected<void, glibre::Error>
migrate_<Type>_v<N>_to_v<N+1>(
    const glibre::types::<ctx>::<Type>_v<N>&  src,
    glibre::types::<ctx>::<Type>_v<N+1>&      dst,
    glibre::Arena&                            scratch
) noexcept;

}
```

**Invariants** (enforced by code review and by the unit tests
required under `tests/data/schemas/`):

1. **Pure**: no global state read, no global state written, no I/O,
   no syscalls, no clock reads, no PRNG.
2. **Deterministic**: byte-equal output for byte-equal input across
   hosts, runs, and toolchain versions in the supported set.
3. **Idempotent under composition**: `migrate_v1_to_v3` defined as
   `migrate_v1_to_v2 >> migrate_v2_to_v3` must produce the same
   result as a hypothetical hand-written direct migration. The
   loader does not check this; the originating context tests it.
4. **Total on the input domain**: for every value of `<Type>_v<N>`
   that could legitimately be on disk (i.e. that passed
   `<Type>_v<N>` deserialization), the migrate function returns
   `std::expected<void>` with a defined output. Returning
   `unexpected` is reserved for invariant violations the
   originating context wants to surface as
   `Error::SchemaMigrationFailed`; "I don't know how to migrate
   this value" is a bug, not a refusal.
5. **No allocation outside `scratch`**: the loader supplies a
   per-phase arena; the migrate function may take temporary
   buffers from it but must not call the global allocator. The
   arena is reset between rows.
6. **No reference to `World`, `Registry`, the logger, or any
   plugin's symbols**: the function takes only its three
   parameters. This is what makes migrations relocatable into
   `glibre-types.dylib`'s static migration table.

**Idempotence in the swap-replay sense**: re-running the same
migrate chain over already-migrated data must be a no-op or fail
deterministically. The loader prevents this by tagging each
storage row with its current schema version after migration; the
migrate function itself does not need an idempotence guard.

## Refusal Cases

Exactly three cases refuse the swap and leave the previous-good
plugin live. Each maps to one named `core::Error` arm wrapped in
`core::Error::HotReloadRefused` so consumers see both the umbrella
("a hot-reload was refused") and the specific cause.

1. **ABI hash mismatch.** `Q::glibre_types_abi_hash() !=
   host_glibre_types_abi_hash`. Detected at step 2.1, before any
   vtable mutation. Cause arm: `core::Error::PluginAbiHashMismatch`.
   Operator action: rebuild the plugin against the current
   `glibre-types.dylib`. This is the PHILOSOPHY §9 case.
2. **Schema migration failure.** A persistent type's stored version
   has no chain to its current version, or some migrate-chain step
   returns `unexpected`. Detected at step 3.1 or 3.2. Cause arm:
   `core::Error::SchemaMigrationFailed`. The migration arena is
   reset (no partial bytes published) before the cause is returned.
   Operator action: ship a migrate function for the missing chain
   step, or restore from a snapshot taken at the prior schema.
3. **Plugin init returns error.** `Q::glibre_plugin_register` returns
   an unexpected `glibre::Error`. Detected at step 4.1. Cause arm:
   `core::Error::PluginInitFailed`, carrying the plugin's reported
   inner error in its detail payload. The loader rolls back the
   step-2 vtable swap and the step-3 migrations (see Failure &
   Rollback). Operator action: read the plugin's own log and fix
   whichever invariant `register` enforces.

Each refusal is logged exactly once at `warn` level via
`glibre::log_error` per the error-model decision, with structured
fields: `plugin_fqn`, `attempted_dylib_path`, `host_abi_hash`,
`plugin_abi_hash`, and the cause arm's enumerator name. Refusals
do not escalate to `error` because the engine continues running on
the prior code.

## Observer Notification

A single observer bus, owned by the loader, publishes two event
types around hot-reload:

- `HotReloadStarted { plugin_fqn, old_dylib_path, new_dylib_path }`
  — emitted at phase 8 entry once per plugin pending reload, before
  step 1 begins.
- `HotReloadCompleted { plugin_fqn, old_abi_hash, new_abi_hash,
  migrated_types: span<TypeFqn> }` — emitted at the end of step 4
  on success. On refusal, instead emit
  `HotReloadRefused { plugin_fqn, cause: core::Error }`.

Atomicity: subscribers are called synchronously by the loader on the
game-loop thread, between steps 4.2 and 4.3 (i.e., after the new
plugin has rebuilt its caches but before the next plugin's reload
begins, and before phase 9 begins). Subscribers see a fully-swapped,
fully-migrated world; they never observe a half-swapped state. This
is the contract that makes the editor's "live reload" UI safe and
the e2e harness's golden snapshots reproducible.

The observer API is itself a middleman type
(`glibre::types::core::HotReloadEvent`), so its layout survives any
core-runtime reload (which would be a self-reload — out of MVP
scope, listed in Open Questions).

## Failure & Rollback

The protocol's atomicity guarantee is **per-plugin**, not phase-wide.
Each pending plugin reload is its own transaction; one plugin's
refusal does not block another plugin's swap in the same phase 8.

Within one plugin's transaction, rollback is **all-or-nothing**:

- Failure at **step 1 (drain)**: the plugin never moved. No state
  changes to undo. Mark the request as refused and continue.
- Failure at **step 2 (swap)**: the vtable mutation is the only
  state change, and step 2.1 / 2.2 detect refusal *before* mutation.
  If 2.3 itself faulted (it cannot, by construction — single
  relaxed store under exclusive ownership), terminate the process;
  this is a contract violation, not a recoverable error.
- Failure at **step 3 (migrate)**: rows already migrated this phase
  live only in the migration arena, which is reset on failure.
  Storage rows are overwritten in step 3.3 only after the full
  chain succeeds for that row. **Therefore at most one row is
  half-overwritten at any point — and the loader's exclusive lock
  on phase 8 means no observer ever sees that intermediate.** On
  failure, the arena is reset, the type registry is rolled back
  (Q's type registrations from step 2.4 are removed), the vtable
  pointer is restored to P, and P's `glibre_plugin_register` is
  re-invoked to rebuild any caches it dropped in step 1. (This is
  why P::register is required to be idempotent; it may run a
  second time after a failed swap.)
- Failure at **step 4 (resume)**: identical rollback to step 3.
  Migrated bytes are reverted by running the **inverse migrate
  chain** if one exists, or by refusing the rollback and
  terminating if no inverse exists. The originating context
  declares per `.fory` schema whether its migrations are
  invertible; non-invertible migrations force step-4 failure to
  terminate (loud, immediate, in CI). The default is invertible:
  most field additions and renames have trivial inverses.

The "terminate on contract violation" cases are deliberately loud
because they indicate the loader's own invariants were broken;
silent recovery would mask bugs that the deterministic-snapshot
contract cannot tolerate.

## Test Hooks

The loader exposes one E2E-only entry point under
`#if defined(GLIBRE_E2E)` guards, allowing the e2e harness to
trigger reloads deterministically without touching the filesystem:

```cpp
namespace glibre::core::test {

// Requests a hot-reload of `plugin_fqn` to `replacement_dylib_path`
// at the next phase 8. Multiple calls before phase 8 enqueue
// multiple reloads. Returns the request id so the caller can block
// on the matching HotReloadCompleted/Refused event.
ReloadRequestId enqueue_hot_reload(
    std::string_view plugin_fqn,
    std::filesystem::path replacement_dylib_path
) noexcept;

// Blocks the calling thread until the referenced request reaches a
// terminal state. Used by E2E goldens; never linked into runtime.
std::expected<void, glibre::Error>
await_reload(ReloadRequestId) noexcept;

}
```

In addition, fixture plugins under `tests/e2e/plugins/` ship two
intentionally-failing reload variants (`bad-abi-hash`,
`failing-init`) and one schema-bumping variant
(`v1-to-v2-migration`). The CI matrix exercises:

- happy-path reload with no schema change (assert
  `HotReloadCompleted` fires; assert frame counter advances by
  exactly 1 between request and completion);
- each refusal case (assert `HotReloadRefused` fires with the
  expected `cause` arm; assert prior plugin still ticks);
- schema migration with deterministic golden round-trip
  (`vN payload → migrated → re-serialize` is byte-equal to a
  recorded `vN+1 payload`);
- mid-migration injected failure via a test-only
  `migrate_T_v1_to_v2_force_fail` symbol, asserting full rollback
  including type-registry revert.

All four CI scenarios run in a single deterministic frame because
the trigger is in-process; no filesystem watcher is involved. The
filesystem-watcher path used in development builds is a thin wrapper
that calls `enqueue_hot_reload`, so testing the in-process path
covers the loader's own state machine.

## Rationale

- **Drain → swap → migrate → resume names the four distinct
  responsibilities** of the protocol (quiesce, replace code,
  reshape data, rehydrate caches). Collapsing any pair would put
  two reasons to change in one step (SRP violation).
- **Per-plugin atomicity, not phase-wide** matches the failure
  domains: a render-plugin refusal must not block a physics-plugin
  reload that is otherwise valid. Phase-wide atomicity would force
  operators to roll back unrelated work on every refusal.
- **Survival rule = "has a `.fory` schema"** collapses the survival
  question into one mechanical check that the loader performs
  against `glibre-types.dylib`'s already-existing registry. No
  per-plugin survival manifest, no per-component opt-in flag.
- **Migrate functions live in the originating context, called from
  the middleman** matches `fory-codegen.md`'s decision: the
  context owns invariants, the middleman owns plumbing. The loader
  needs neither.
- **Three refusal cases, named explicitly** because PHILOSOPHY §9
  promises hash-mismatch refusal and the error-model decision
  reserves matching `core::Error` arms. Adding a fourth case would
  require both a PHILOSOPHY amendment and a new error arm; the
  friction is intentional.
- **Observer notification on the loader thread** (synchronous, not
  queued) means the editor and e2e harness see swap completion
  without races against the next frame's phase 1. The cost — a
  few microseconds per subscriber — is irrelevant inside phase
  8's 0.1 ms idle budget (frame-phases §Consequences) when the
  subscriber count is small (<10 in MVP).
- **Test hooks under `GLIBRE_E2E`** make the contract executable.
  The harness need not race a filesystem watcher; the protocol is
  the same in test and production. This resolves the "deterministic
  trigger" requirement of the e2e context's SPEC §6.

## Consequences

- **Plugin authors must implement three exported entry points**:
  `glibre_plugin_drain`, `glibre_plugin_register`,
  `glibre_types_abi_hash`. The first two are part of the plugin
  ABI listed in PHILOSOPHY §3; the third comes from
  `glibre-types.dylib` linkage and is automatic.
- **Originating contexts must ship migrate functions for every
  schema-version bump** they make to a persistent type. Catch2
  golden round-trip tests under `tests/data/schemas/` are
  mandatory per `fory-codegen.md` §Consequences; this record adds
  the **inverse migration** test as a same-file companion when the
  context declares the schema invertible.
- **The migration arena is a per-phase loader-owned `glibre::Arena`**
  whose budget must be set high enough to hold the largest single
  storage row's `sizeof(T_v1) + sizeof(T_v2) + ... + sizeof(T_vN)`.
  The implementation plan picks a starting size and a growth
  strategy; this record requires only that the arena be reset
  between rows and never grown across plugins in the same phase.
- **Frame budget**: phase 8 with no reload pending is a single
  relaxed atomic load — sub-microsecond. Phase 8 with one reload
  pending is bounded by `drain + swap + Σ(migrate cost per row) +
  register`. The frame-phases consequence (≤0.1 ms idle) is
  preserved; the reload path itself is permitted to overrun a
  single frame's budget exactly once per reload, surfacing in the
  profiler trace as a known stall. (Frame-phases open question 5
  asks whether to move migration to a worker; this record answers
  "no, in MVP" — the determinism cost of a worker fence is worse
  than the one-frame stall, and reloads happen in dev workflows
  where stalls are acceptable.)
- **Self-reload of `core` is out of scope.** Only plugin dylibs
  reload at phase 8; the core runtime requires a process restart.
  Listed in Open Questions.
- **Replay determinism is preserved**: golden snapshots taken at
  frame N from before-reload bytes are byte-equal to snapshots
  taken at frame N from a re-played run, because (a) phase 8
  publishes nothing observable in frame N (it precedes phase 9),
  and (b) post-reload behaviour is captured starting at frame
  N+1 with the new plugin's deterministic systems.

## Open Questions

1. **Per-phase migration arena sizing**: pick a default budget
   (e.g. 4 MiB) versus a `World`-owned scaling policy. Defer to
   the loader implementation plan; this record only requires the
   arena exist and reset between rows.
2. **Self-reload of `core`**: a future spike may relax the
   "process restart" requirement, but it requires a second-level
   middleman (a "metaloader") that can observe the loader itself
   being swapped. Not in MVP.
3. **Inverse migrations**: should the `.fory` grammar grow an
   explicit `inverse_provider` clause, or do we infer invertibility
   from a static analysis of the forward migration? Defer to the
   `data` SPEC §7 implementation.
4. **Concurrent reloads of inter-dependent plugins**: phase 8
   currently reloads in registration order. If plugin A's
   `register` reads plugin B's vtable, and both are reloading
   the same phase, we need a topological order. Triage when the
   first such dependency exists; for MVP all plugin dependencies
   are core-only.
5. **Editor-driven partial reload**: the editor wants to swap a
   single shader without re-running the full plugin protocol.
   The likely answer is a separate "asset reload" path that does
   not touch vtables and runs in phase 1 (input-equivalent), but
   this is editor-context territory and out of scope here.
6. **Cancellation**: should the operator be able to cancel a
   pending reload between phase 1 and phase 8? Cheap to add
   (a flag on the request struct), but no MVP user story asks
   for it. Deferred until one does.
