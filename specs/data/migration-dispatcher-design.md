# data — Detailed Design: migration-dispatcher aggregate

> Detailed design for the `MigrationDispatcher` aggregate declared in
> `specs/data/SPEC.md` §4.7 (composition) on top of §4.6 (single-step
> bodies). Refines §4.5–§4.8, §5 (`migration.hpp`), §7.4, §8.2, §9.2,
> §10.1–§10.2 in place; cites
> `reviews/decisions/fory-codegen.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/error-model.md`,
> `reviews/decisions/frame-phases.md`, and
> `reviews/decisions/perf-budget.md`. Sibling designs:
> `specs/core/hot-reload-barrier-design.md` (the caller),
> `specs/core/type-registry-design.md` (the version source),
> `specs/data/middleman-dylib-design.md` (the host TU; spike #734).
> Does not introduce new public surface beyond the §5 stub of
> `specs/data/SPEC.md`; deviations from the cited records would
> require an amendment spike, not an in-place edit.

Refs: spike #738 — `[SPIKE] design-data-migration-dispatcher-detailed`.
Parent: #729. Sibling task-breakdown spike blocked-by this deliverable.
Cross-cuts: #706 (hot-reload-barrier detailed design), #734 (middleman
dylib detailed design), #499
(`hot-reload-migration-arena-grow-vs-refuse`, post-MVP).

## 1. Purpose

`MigrationDispatcher` is the single aggregate inside `glibre-types.dylib`
that, given an `(FQN, src_version, dst_version)` triple and a single
inbound payload, walks the per-FQN linear migration chain hop-by-hop
and yields the migrated payload at the destination version — or returns
a typed `glibre::Error` describing why the walk could not complete. It
is the verb to `MigrationChain`'s noun: §4.7 of `specs/data/SPEC.md`
records the per-FQN function table; this design records the loop that
*invokes* that table on a single payload.

The dispatcher's one responsibility — **walking a per-FQN linear,
strictly-monotonic version graph and invoking each codegen-emitted
single-step `MigrationFn` in ascending order against an arena-backed
scratch pair** — is sharp. If the chain-walk loop, the arena
recycling rule, the chain-coverage refusal, the cycle-detection
gate, or the failure-isolation discipline change, this design
changes. Anything else is out of scope.

What the dispatcher **explicitly refuses to own**:

- **Migration function bodies.** Owned by the originating context
  that authored the schema; codegen-emitted from `.fory` declarations
  and registered via `glibre_types_register_migration` at static-init
  (§4.6 of `specs/data/SPEC.md`; `fory-codegen.md` §"Migration
  Mechanic" #2). The dispatcher dereferences function pointers; it
  never reads `.fory` files and never compiles bodies.
- **Hot-reload barrier orchestration.** Owned by `core::HotReloadBarrier`
  (#706, `specs/core/hot-reload-barrier-design.md` §3, §4, §8.2). The
  barrier owns the four-step `drain → swap → migrate → resume` state
  machine, the per-phase migration arena, the rendezvous, the
  observer bus. The dispatcher is invoked *inside* the barrier's
  `Migrate` substep on a per-row basis; it has no knowledge of phase
  ordering, plugin staging, or vtable swapping.
- **`World` traversal.** The barrier (or, on the cold/load path, the
  caller of `Envelope<T>::deserialize`) iterates rows; the
  dispatcher receives one source/destination pair at a time. It
  never sees a `World&` and never queries archetype storage.
- **Schema authoring rules.** Tag-sort, reserved-tag enforcement,
  `since` validation, and the `current_version` decision are
  `Foryc`'s concerns (§4.2 of `specs/data/SPEC.md`). The dispatcher
  trusts the registry: if `(FQN, N → N+1)` exists in the table, it
  is well-formed.
- **`Envelope` decode / encode.** Reading the envelope to discover
  `(FQN, version)` and writing the migrated payload back to bytes
  belong to `Envelope<T>` (§4.8). The dispatcher operates on
  already-typed C++ structs (`const VN&`, `VNplus1&`).
- **ABI hash validation.** Owned by the loader (`plugin-abi.md`
  §"Loader Sequence"). The dispatcher runs only after the loader
  has cleared ABI checks for both the host and the candidate
  dylib; a stale chain entry pointing into a dlclosed plugin's
  `.text` is impossible by §4.5 invariant 4 (registry is read-only
  after static-init of the live middleman build) plus the loader's
  rollback discipline.
- **Persistence side-effects.** No log writes, no `spdlog` calls,
  no global counters, no event bus. Logging is the handler's
  job (`error-model.md` §"Logging / Telemetry" #1; §10.4 of
  `specs/data/SPEC.md`). The dispatcher returns a typed `Error`
  and is silent.
- **Caching of migrated payloads.** Each invocation walks the
  chain afresh; there is no per-payload memoization. PHILOSOPHY's
  "two concrete users" rule defers a lazy-migration cache
  (`specs/data/SPEC.md` §9.6).

The SRP boundary is sharp by construction: every other migration-shape
concern (chain construction at codegen time, registration at
static-init, arena ownership at the barrier, envelope decode at the
deserialize site) lives in a sibling aggregate or a sibling context.
The dispatcher is a single function — `dispatch_migration(...)` — and a single
cycle/coverage validator that runs at static-init.

## 2. Requirements coverage

This section verifies that every harmonius MVP-scope requirement /
design clause about schema-driven forward migration is either covered
by the design below or explicitly refused with rationale. Inputs:

- `harmonius/docs/design/data-systems/attributes-effects.md` —
  versioned `MeterDefinition`, `AttributeSchema`, `EffectDefinition`
  archive types.
- `harmonius/docs/design/data-systems/containers-slots.md` —
  versioned container row archive types.
- `harmonius/docs/design/data-systems/data-tables.md` — versioned
  row archive types with `rkyv` derivations.
- `harmonius/docs/design/data-systems/directed-graphs.md` —
  versioned graph node/edge archive types.
- `harmonius/docs/design/core-runtime/` — hot-reload migration
  references (research input only).

| Harmonius clause                                                                                           | Glibre disposition                                                                                                                                                                                                                                                                                                                          |
|------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Per-domain `rkyv`-derived archive types (`MeterDefinition`, container rows, table rows, graph nodes/edges) | **Covered, collapsed.** All N domain-specific archive disciplines collapse into the single `MigrationDispatcher` walking `MigrationChain` tables emitted by `Foryc` (`specs/data/SPEC.md` §3.2; `fory-codegen.md` §"Migration Mechanic"). One dispatcher, one arena rule, one refusal envelope.                                                |
| Forward `vN → vN+1` archive migration scattered per type                                                   | **Covered.** `migrate_<Type>_v<N>_to_v<N+1>` (§4.6 of `specs/data/SPEC.md`) is the only shape; the dispatcher composes the chain in ascending order (§4.7 inv. 2). No type is responsible for its own dispatch loop.                                                                                                                            |
| Multi-step migration (`vN → vN+k`) ad-hoc per type                                                          | **Covered, collapsed.** No `vN → vN+2` direct migration is permitted (§4.6 inv. 5); multi-step composition is the dispatcher's exclusive job (§4.7 inv. 2). Collapses two failure modes (forgot-an-intermediate-step, wrong-direct-jump) into one codegen-time chain-coverage check.                                                            |
| Migration body allocates from heap / temporary `Vec` / `String` (rkyv pattern)                              | **Refused, replaced by arena.** `Arena&` is the only allocation surface (§4.6 inv. 2; `hot-reload-protocol.md` §"Migrate Function Contract" #5). The dispatcher resets the arena between steps (§4.7 inv. 5) so peak memory is bounded by the largest single-step `(VN, VNplus1)` working-set, not the chain length.                            |
| Migration body reads `World` / domain registries / loggers                                                  | **Refused.** §4.6 inv. 4 (locality), `hot-reload-protocol.md` §"Migrate Function Contract" #6 (no `World&`, `Registry&`, logger). The dispatcher passes only `(const VN&, VNplus1&, Arena&)`. Replaces every harmonius "domain-aware migration" pattern.                                                                                       |
| Late-bound migration discovery (look up by version at deserialize time)                                    | **Covered.** `Envelope<T>::deserialize` reads the envelope, calls the dispatcher with `(FQN, src_version, current_version)`. No out-of-band hint (§4.8 inv. 5). The chain table is statically populated at middleman load (§4.5 inv. 4).                                                                                                       |
| Bidirectional migration (downgrade `vN+1 → vN` for save compat)                                            | **Refused.** Forward-only is the engine-wide rule (`specs/data/SPEC.md` §4.10 inv. 2 — strictly monotonic versions; `fory-codegen.md` §"Migration Mechanic" — single-direction chain). A downgrade story belongs to a future asset-archive context, not the spine. Not in MVP.                                                                |
| Skipping intermediate versions for performance                                                              | **Refused.** §4.7 inv. 2 (strict ascending, no skipping). Performance gate is per-step ≤ 50 µs (§9 below); fast enough not to motivate skip optimisations. Two concrete users would have to demand it (PHILOSOPHY §10).                                                                                                                          |
| Parallel migration of independent rows                                                                     | **Refused.** Single-threaded at the barrier (`hot-reload-protocol.md` "Consequences"; `frame-phases.md` Open Q #5; `specs/core/hot-reload-barrier-design.md` §6.1). The dispatcher is invoked from one thread; concurrency lives nowhere in this design (§6 below).                                                                              |
| Asynchronous / streaming migration                                                                         | **Refused.** Migration is barrier-time only (§5 below). The cold/load path is also synchronous (one row at a time, blocking the loader thread). Async is a future post-MVP concern; reopens via a fresh sub-epic.                                                                                                                                |
| Migration cancellation / partial commit                                                                    | **Refused.** Failure is all-or-nothing per row (§4.7 inv. 4 — no partial mutation observable to the caller; §4 below — destination is not yielded on failure). The barrier's outer rollback handles cross-row atomicity (`specs/core/hot-reload-barrier-design.md` §4, §10).                                                                    |
| Migration audit log / per-step telemetry                                                                   | **Partial — covered by the report span returned to the barrier.** The dispatcher itself emits no log (§10.4 of `specs/data/SPEC.md`); the barrier's `MigrationReport` carries the count and the sorted span of `FQN`s actually migrated (§8.2 of `specs/data/SPEC.md`). Per-step telemetry is `spdlog`-side at the handler boundary.            |

Net result: the harmonius "scattered `rkyv`-derived archive types"
pattern is replaced end-to-end by one Fory codegen pipeline, one
static migration table, and one chain-walking dispatcher. The
collapse is significant and intentional (per Occam, §3.2 of
`specs/data/SPEC.md`): N domain-specific migration disciplines →
1 dispatcher.

Glibre-native requirements added beyond harmonius:

- **Linear strictly-monotonic version graph per FQN** (§3.1 below).
  Cycles are codegen-time / static-init impossible (§4.7 inv. 2 of
  `specs/data/SPEC.md`; §10.1 arm `MigrationCycle`).
- **Arena-reset-between-steps** (§4.7 inv. 5). Bounds peak memory
  to one `(VN, VNplus1)` pair, not chain length.
- **In-place commit on success, allocate-then-replace per row**
  (§3.4 below; cites #499). The dispatcher writes the migrated
  bytes into the caller-supplied destination only on full chain
  success; on failure the destination is untouched.
- **Single-threaded barrier dispatch** (§6 below). No locking; no
  atomic stores; no compare-exchange. The dispatcher is callable
  from one thread because the barrier guarantees one-thread
  invocation.
- **ABI hash composition contribution** (§7 below). The
  dispatcher table layout is part of the middleman's ABI hash via
  the per-`(FQN, from, to)` source-hash digest, but the dispatcher
  *function* itself contributes nothing; the composition is over
  the data the dispatcher reads, never over its code.

## 3. Detailed model

### 3.1 Per-FQN version graph

The dispatcher's view of one FQN's migration story is a linear,
strictly-monotonic, single-rooted directed graph:

```text
v1 ──migrate_T_v1_to_v2──▶ v2 ──migrate_T_v2_to_v3──▶ v3 ──...──▶ vM
```

Properties (each enforced at codegen time by `Foryc` and re-checked
at static-init by the dispatcher's chain validator, §3.5):

1. **Linearity.** Exactly one out-edge per intermediate vertex; no
   branching. Disjoint paths from `vN` to `vM` are forbidden. (§4.7
   inv. 1 of `specs/data/SPEC.md`.)
2. **Strict monotonicity.** Every edge goes from `vN` to `vN+1`;
   `to_version - from_version == 1`. No `vN → vN+k` shortcuts (§4.6
   inv. 5).
3. **Single root.** Vertex `v1` is the lowest; the chain starts at
   the lowest version that has ever shipped and reaches `vM` =
   `current_version`. Versions older than `v1` are unreachable —
   payloads tagged with such a version raise
   `data::Error::MigrationStepMissing` (§10.1 of `specs/data/SPEC.md`).
4. **Acyclicity.** No back-edges; cycles raise `MigrationCycle`
   (codegen-time or static-init time, §10.1 arm 8).
5. **Coverage.** For every `FQN` whose `current_version` is `M ≥ 2`,
   exactly `M − 1` edges exist: `(v1 → v2), (v2 → v3), …, (vM−1 → vM)`.
   Missing any edge fails codegen (§4.7 inv. 1).

The graph is materialised as a contiguous, ascending-sorted
`eastl::span<const MigrationEntry>` keyed off
`from_version` (§5 of `specs/data/SPEC.md`,
`include/glibre/types/migration.hpp`). Lookup of edge `(N → N+1)` is
`O(1)` via direct array index `N − v1` (where `v1 = entry[0].from_version`;
SPEC §4.7 inv. 1 guarantees `v1 = 1` for all in-build chains, so
in practice this reduces to `N − 1` — the generalized form is
retained for defensive correctness and to match the §3.5 density
check); the dispatcher validates the index by checking
`entry.from_version == N` before invoking.

### 3.2 Aggregate state

The dispatcher is **stateless**. It reads from the immutable registry
(§4.5 of `specs/data/SPEC.md`) and writes only into the
caller-supplied destination and arena. There are no member variables,
no caches, no counters, no thread-local storage. The aggregate is a
free function (`dispatch_migration(...)`) plus a static-init
chain validator (`validate_chain(...)`); both live in the
middleman dylib and bind no per-process state.

```text
MigrationDispatcher (stateless module inside glibre-types.dylib)
├── dispatch_migration(...)          // §4 below — the chain-walk loop
├── validate_chain(...)              // §3.5 below — static-init gate
└── (no member variables, no caches)
```

The arena cells the dispatcher uses are *parameters*, owned by the
caller (`HotReloadBarrier`'s 16 MiB phase-8 arena per
`specs/core/hot-reload-barrier-design.md` §3.1 and `perf-budget.md`
"Allocator Rules" #6). The dispatcher takes a borrowed `Arena&` and
returns it in the same logical state on every exit path (§4.7 inv. 5
of `specs/data/SPEC.md`).

### 3.3 Hop-by-hop walk

For an inbound `(FQN, src_version)` and a registry-resolved
`current_version`, the dispatcher walks `current_version −
src_version` hops in ascending order. Each hop:

1. **Index.** `entry = chain[from − v1]` (where `v1` is the lowest
   chain entry's `from_version`; for the dense chain of §3.1 this is
   always `v1 = 1`).
2. **Sanity.** Assert `entry.from_version == from` and
   `entry.to_version == from + 1`. A mismatch is `MigrationCycle` or
   chain-table corruption — both are static-init failures and never
   reach this code path in a healthy build.
3. **Allocate.** Reserve `sizeof(VNplus1)` (aligned to
   `alignof(VNplus1)`) from the supplied `Arena&`. Allocation
   failure → `data::Error::ArenaExhausted` arm (§10 below; refuse
   policy from #499).
4. **Invoke.** Call `entry.invoke(src_ptr, dst_ptr, arena, &out_err)`.
   Erased trampoline casts back to typed
   `MigrationFn<VN, VNplus1>` per the registration macro (§5 of
   `specs/data/SPEC.md`, `migration.hpp`).
5. **Branch.** Non-default `out_err` →
   `data::Error::SchemaMigrationFailure` (§10), wrap the inner
   error's `(FQN, from, to)` triple, return `unexpected`.
6. **Reset.** Reset the arena to its pre-step high-water mark
   (§4.7 inv. 5 of `specs/data/SPEC.md`); the just-produced
   `VNplus1` lives in a small handoff cell (§3.4) that is *not*
   reset.
7. **Advance.** `src_ptr ← dst_ptr`; `from ← from + 1`; loop until
   `from == current_version`.

The handoff cell is a single arena sub-allocation outside the
"reset between steps" region — it carries the just-produced
`VNplus1` so the next hop's source pointer is valid after the
arena reset. Sized as `max_over_chain(sizeof(Vk))`; the
dispatcher allocates two such cells at entry (a "ping" and a
"pong"), alternating between them per step. This caps the
arena's per-payload working-set to exactly `2 *
max_over_chain(sizeof(Vk)) + max_over_chain(scratch_step_k)`.

### 3.4 In-place vs allocate-then-replace policy

The barrier presents one row at a time (`specs/core/hot-reload-barrier-design.md`
§8.2): an `(src_row_ptr, dst_row_ptr)` pair where `dst_row_ptr` is
the storage row that will hold the migrated bytes on success. The
dispatcher's policy is **allocate-then-replace per row, in-place at
commit**:

1. **Allocate.** Per-row scratch (the ping/pong cells of §3.3) lives
   exclusively in the supplied `Arena&`; the destination row is not
   touched until the chain succeeds.
2. **Replace.** On full chain success, the dispatcher copies the
   final `VM` bytes into the caller's destination cell (the
   `dst_row_ptr` slot). The copy is `std::memcpy` for trivially
   copyable types (the common case; §4.2 of `specs/data/SPEC.md`)
   or a generated copy thunk for types with non-trivial members
   (still `noexcept` and arena-free; the thunk lives in the
   generated header).
3. **Refuse on failure.** On any hop's failure the dispatcher
   returns `unexpected` *before* writing the destination cell.
   The destination remains byte-identical to its pre-call state.

This is the answer to #499 (`hot-reload-migration-arena-grow-vs-refuse`):
the dispatcher does **not** grow the arena and does **not** retry.
If allocation refuses, the arena is exhausted and the row's
migration is refused with `ArenaExhausted` (§10). The barrier's
arena is sized by `perf-budget.md` "Allocator Rules" #6 (16 MiB
core-row budget, with `data`'s 4 MiB sub-ceiling per
`specs/data/SPEC.md` §9.2). The grow-policy decision itself is
post-MVP (#499) and lives outside this design — when it lands it
amends §3.4 here, not the dispatcher's contract elsewhere.

### 3.5 Static-init chain validator

`validate_chain(SchemaId, SchemaVersion, span<const MigrationEntry>) -> std::expected<void, data::Error>` runs
once per FQN at middleman static-init (§4.3 inv. 5 of
`specs/data/SPEC.md`), called from the codegen-emitted registry
construction inside `glibre-types.dylib`. It checks:

1. **Density.** For chain length `L`, every index `i ∈ [0, L)` has
   `entry[i].from_version == v1 + i` and
   `entry[i].to_version == v1 + i + 1` (where `v1 = entry[0].from_version`,
   the lowest version in the chain; SPEC §4.7 inv. 1 guarantees `v1 = 1`
   for all in-build chains, reducing these to `i + 1` and `i + 2`
   respectively — the generalized form matches §3.1 and handles
   any hypothetical future chain starting above version 1).
   Violation → `MigrationCycle`
   (gap or back-edge) or `MigrationStepMissing` (truncated chain).
2. **Monotonic.** `entry[i].to_version > entry[i].from_version`,
   strictly. Violation → `MigrationCycle`.
3. **Function-pointer non-null.** `entry[i].invoke != nullptr`.
   Violation → `MigrationStepMissing` (a registration was elided).
4. **Coverage to `current_version`.** `entry[L−1].to_version ==
   current_version` (the registry's recorded version, §4.5). Two
   directions of failure:
   - **Chain too short** (`current_version > entry[L−1].to_version`): a
     chain step was forgotten between schema-bump and codegen; raise
     `MigrationStepMissing`.
   - **Chain too long** (`entry[L−1].to_version > current_version`): the
     chain overshoots the registry's declared version — a codegen/bump
     mismatch that structurally resembles an unregistered forward step;
     raise `MigrationCycle` (same density-gate arm used for gaps and
     back-edges).

Validator failures at static-init are **fatal** — the middleman
refuses to load and the process aborts via
`std::abort()` per `specs/data/SPEC.md` §10.2 row
`MigrationCycle` / `SchemaRegistryConflict`. A corrupt chain table
is a build-system defect, not a runtime branch.

### 3.6 Object responsibilities (SRP)

- **`MigrationDispatcher` (this design)** — the chain-walk loop,
  the per-step allocate / invoke / reset / advance machinery, the
  arena ping-pong cell management, the static-init chain
  validator. Owns no chain entries; owns no migration bodies;
  owns no hot-reload state. Stateless.
- **`MigrationChain`** (§4.7 of `specs/data/SPEC.md`) — the
  per-FQN `eastl::span<const MigrationEntry>` materialised inside
  the registry entry. Constructed at codegen time by `Foryc`;
  populated at static-init by `glibre_types_register_migration`
  calls. Read-only thereafter.
- **`Migration` body** (§4.6 of `specs/data/SPEC.md`) — the
  single-step function the originating context wrote. Pure,
  arena-only, deterministic, total. The dispatcher dereferences
  it; never inspects it.
- **`SchemaRegistry`** (§4.5 of `specs/data/SPEC.md`) — provides
  the `(current_version, MigrationChain)` for an FQN. The
  dispatcher reads; never writes.
- **`Arena`** (§4.6 inv. 2 of `specs/data/SPEC.md`,
  `specs/core/hot-reload-barrier-design.md` §3.1) — owned by the
  barrier (16 MiB) or by the cold/load path's caller. The
  dispatcher takes a borrow.
- **`HotReloadBarrier`** (`specs/core/hot-reload-barrier-design.md`
  §8.2) — the only invoker of `dispatch_migration` at barrier
  time. Iterates rows; resets the arena between rows.
- **`Envelope<T>::deserialize`** (§4.8 of `specs/data/SPEC.md`) —
  the only invoker at cold/load time. Reads the envelope, calls
  the dispatcher, hands the migrated value back to the caller.

This is the same SRP split `specs/data/SPEC.md` §§4.6–4.8 already
record; the design doc only sharpens which arguments cross the
seam at which step.

## 4. Public surface

The dispatcher's public C++ surface is the §5 `migration.hpp` stub
of `specs/data/SPEC.md` plus one entry point this design pins down
(`dispatch_migration`, the chain-walk loop the barrier and
`Envelope<T>::deserialize` invoke). No new types are introduced;
the surface is purely a verb on the noun-types §5 already
declares.

```cpp
// glibre-types.dylib public surface, additive to §5 of
// specs/data/SPEC.md migration.hpp.
//
// Compiles under clang++ -std=c++23 -fsyntax-only -fno-exceptions
// against libc++ on macOS.

#pragma once

#include <EASTL/span.h>
#include <expected>

#include <glibre/error.hpp>
#include <glibre/types/identity.hpp>
#include <glibre/types/migration.hpp>      // MigrationEntry, Arena

namespace glibre::types {

// Walk the per-FQN migration chain from `src_version` up to
// `current_version`, invoking each codegen-emitted single-step
// MigrationFn against the arena and the caller-supplied destination
// cell.
//
// Only the fields actually consumed are passed: the registry's
// recorded current version, the chain span, and the schema identifier
// for error payload population.  The caller (barrier sweep) extracts
// these from RegistryEntry before calling; this keeps the internal
// seam narrow and simplifies unit-test fixture construction (no full
// RegistryEntry needed).
//
// Preconditions (asserted in debug; UB in release if violated — the
// dispatcher trusts the registry and the barrier):
//   - `chain` is the validated (§3.5) MigrationEntry slice for
//     `schema`.
//   - `src_payload` points to a fully-initialised V<src_version>
//     value sized per the schema's recorded V<src_version>::sizeof.
//   - `dst_payload` points to a destination cell sized per
//     `sizeof(V<current_version>)` and aligned per
//     `alignof(V<current_version>)`.
//   - `arena` has at least `2 * max_over_chain(sizeof(Vk)) +
//     max_over_chain(scratch_step_k)` bytes free (caller-sized).
//   - `src_version >= 1` and `src_version <= current_version`.
//
// Postconditions on success:
//   - `*dst_payload` holds a fully-initialised V<current_version> value.
//   - The arena is reset to its pre-call high-water mark.
//   - `src_payload` is unread after return (caller may free / reuse).
//
// Postconditions on failure:
//   - `*dst_payload` is byte-identical to its pre-call state
//     (allocate-then-replace policy, §3.4).
//   - The arena is reset to its pre-call high-water mark.
//   - The returned Error carries the (FQN, from, to) triple of the
//     refusing or missing step.
//
// Failure arms (mapping in §10):
//   - data::Error::MigrationStepMissing
//   - data::Error::SchemaMigrationFailure (body returned unexpected;
//     carries the (FQN, from, to) triple of the refusing step per
//     §10.1 of specs/data/SPEC.md)
//   - data::Error::ArenaExhausted (post-MVP arm; #499 refuse policy)
//   - data::Error::MigrationCycle (impossible at runtime; static-init
//     arm only — see §3.5)
[[nodiscard]] auto dispatch_migration(
    SchemaId                          schema,
    eastl::span<const MigrationEntry> chain,
    SchemaVersion                     src_version,
    SchemaVersion                     current_version,
    const void*                       src_payload,
    void*                             dst_payload,
    Arena&                            arena
) noexcept -> std::expected<void, ::glibre::Error>;

// Invoked exactly once per FQN at middleman static-init from inside
// glibre_types_register_migration's chain-finalisation pass (§4.3
// inv. 5 of specs/data/SPEC.md). Validates the chain density,
// monotonicity, and coverage gates of §3.5. Returns void on success
// or data::Error carrying one of {MigrationStepMissing, MigrationCycle}
// on violation (see §3.5 for each gate's arm).
[[nodiscard]] auto validate_chain(
    SchemaId                          schema,
    SchemaVersion                     current_version,
    eastl::span<const MigrationEntry> chain
) noexcept -> std::expected<void, data::Error>;

}  // namespace glibre::types
```

The public surface is two free functions; no new struct, no new
enum, no new class. The `data::Error` arms `MigrationStepMissing`,
`MigrationCycle`, `SchemaMigrationFailure`, and the new `ArenaExhausted` arm
(amendment, see §10) are the only failure types crossing the
boundary. Both functions are `noexcept` and `-fno-exceptions`-clean
per the engine-wide error-model rule (`error-model.md` "Decision"
#3).

### 4.1 Why the trampoline shape

`MigrationEntry::invoke` is type-erased
(`void(const void*, void*, Arena&, glibre::Error*)`) so the entry
table is a homogeneous array of function pointers across every FQN
and every step. The dispatcher casts back to the typed
`MigrationFn<VN, VNplus1>` only at the call site, at which point
the codegen header has already paired `(VN, VNplus1)` with the
trampoline. Erasure is a registry-side concern (§4.5 of
`specs/data/SPEC.md`); the dispatcher never sees concrete `VN`
types.

The C-ABI return shape is **plain `void` with an out-parameter
`Error*`** rather than `std::expected<void, Error>` because:

- `std::expected` is not stable across a C ABI; the `MigrationEntry::invoke`
  pointer must round-trip through `dlsym` in some test-only
  paths (e.g. the §11 fixture that monkey-patches a single step
  via `force_migration_failure`, §8.6 of `specs/data/SPEC.md`).
- Codegen-emitted bodies write to the out-parameter only on
  failure; the dispatcher checks the out-parameter against a
  default-constructed `Error{}` to decide success / failure.
- The C++ wrapper macro `GLIBRE_REGISTER_MIGRATION(<Type>, <N>,
  <N+1>, <fn>)` (§5 of `specs/data/SPEC.md`) hides the
  out-parameter from the body author; bodies write
  `std::expected<void, glibre::Error>` and the macro lifts it
  into the C-ABI shape.

### 4.2 Why no `dispatch_migration_chain` for spans of rows

The dispatcher operates on **one payload at a time**. The barrier's
per-row loop is the right place to amortise constant overhead
(arena reset, registry lookup, version comparison) — pulling that
loop into the data context would re-introduce a `World&`-shaped
parameter we explicitly refuse (§1). The barrier already pays the
loop cost (`specs/core/hot-reload-barrier-design.md` §8.2 step 2);
duplicating it inside the dispatcher would double the seam and
violate SRP.

If a future post-MVP user (e.g. a save-file rehydrator) demands a
batch shape, it lives in a sibling aggregate (`save-loader`) that
*calls* the dispatcher in its own loop. Two concrete users
(PHILOSOPHY §10) would have to demand the abstraction; one is not
enough.

## 5. Hot/cold path split

The dispatcher has **no hot path**. Every invocation is rare,
bounded, and triggered by an event the runtime treats as
exceptional:

- **Hot-reload barrier-time (warm path).** Phase 8 on a reload
  frame (`hot-reload-protocol.md` §"Step 3 — Migrate";
  `specs/core/hot-reload-barrier-design.md` §3.3 row 8). One call
  per row whose stored `SchemaVersion` is below the live
  `current_version`. Frequency: at most one reload per phase 8;
  `pending_count` is the gate (`specs/core/hot-reload-barrier-design.md`
  §3.1).
- **Cold load (cold path).** `Envelope<T>::deserialize` on bytes
  whose envelope `SchemaVersion` is below the registry's
  `current_version`. Frequency: bounded by save-load events, scene
  load, plugin-private snapshot rehydration. Never on the per-frame
  steady-state path — `specs/data/SPEC.md` §9.3 "Steady-state S1
  frames perform zero `Envelope` serialize / deserialize operations
  on the hot path."
- **Per-frame steady-state.** Zero invocations. The runtime path
  uses already-current payloads; the registry's
  `current_version` matches the payload's stored version;
  no migration runs.

The dispatcher's per-frame steady-state cost is **0**. The
budget cell (`specs/data/SPEC.md` §9.2 row `MigrationDispatcher`,
4 MiB sub-ceiling, "invoked at hot-reload only; 0 on hot path")
is the contract that makes this measurable.

### 5.1 Module placement

The dispatcher source lives in
`<middleman>/dispatcher.cpp` inside `glibre-types.dylib` (per
`specs/data/middleman-dylib-design.md`, spike #734). Its public
header is `<glibre/types/migration.hpp>` (the same header §5 of
`specs/data/SPEC.md` already declares for `MigrationFn`,
`MigrationEntry`, and the registration entry-point). The two
new declarations of §4 above are appended to the existing
header; no new file is introduced.

This places the dispatcher inside the same translation unit
boundary as the registry — both populated at middleman static-init,
both immutable thereafter, both consumed only through
`glibre-types.dylib`'s C-ABI exports. Plugins do **not** link
the dispatcher source (§4.10 inv. 4 of `specs/data/SPEC.md`); they
reach it exclusively through `Envelope<T>::deserialize` and the
loader's barrier-time call.

### 5.2 No cold/warm asymmetry

Both the warm path (barrier) and the cold path (deserialize) call
the same `dispatch_migration(...)` body. The only differences are
**which arena the caller supplies** (the barrier's 16 MiB phase-8
arena vs. the deserialize call site's per-call arena, sized per
`Envelope<T>::deserialize`'s caller policy) and **what the caller
does on failure** (barrier rolls back the row, deserialize
returns `unexpected` to the upstream consumer). The dispatcher's
behaviour is identical.

## 6. Concurrency

The dispatcher runs **single-threaded at the barrier**. No
concurrent migrations exist anywhere in MVP scope; the contract
is enforced upstream and inherited by this design.

### 6.1 Threading model

- **Barrier-time (warm path).** Phase 8 is exclusive on the
  game-loop thread (`frame-phases.md` Open Q #5;
  `specs/core/hot-reload-barrier-design.md` §6.1). The barrier
  holds the world's exclusive write lock; no system body, no
  worker, no I/O thread runs. The dispatcher inherits this
  exclusivity by virtue of being called only from inside the
  barrier's `Migrate` substep.
- **Cold/load path.** `Envelope<T>::deserialize` is called from
  whichever thread owns the load operation (typically the
  loader thread for save-game rehydration, or the editor thread
  for snapshot replay). The dispatcher requires only that **at
  most one thread per `Arena&`** invokes it concurrently — the
  arena is the synchronisation point, not the registry. The
  registry itself is read-only after static-init (§4.5 inv. 4 of
  `specs/data/SPEC.md`), so concurrent reads are lock-free and
  safe.
- **Static-init.** `validate_chain(...)` runs from the
  middleman's static-init constructor, which `dlopen` serialises
  per macOS `dyld` semantics. No concurrency.

### 6.2 No locks, no atomics

The dispatcher uses **zero** synchronisation primitives. No
mutexes, no atomics, no fences, no thread-local storage. This
is a load-bearing claim: if a future need for multi-threaded
migration arises, it requires a new aggregate (or an amendment
to this design) — not a quietly-added lock here.

The justification is mechanical: the dispatcher reads only
const data (the registry entry), writes only data the caller
owns (the destination cell, the arena), and calls only
codegen-emitted bodies that themselves run lock-free per §4.6
inv. 1, 2, 4 of `specs/data/SPEC.md` (deterministic, arena-only,
local). No shared mutable state exists for the dispatcher to
guard.

### 6.3 Reentrancy

`dispatch_migration` is **not reentrant** within a single
`Arena&`. A migration body that tried to recursively call
`dispatch_migration` against the same arena would corrupt the
ping-pong cells (§3.3). This is forbidden by §4.6 inv. 4 of
`specs/data/SPEC.md` ("the migration touches only fields of
`VN` and `VNplus1`; it does not consult the `SchemaRegistry`")
and by `hot-reload-protocol.md` §"Migrate Function Contract" #6
("no reference to `World`, `Registry`, the logger, or any
plugin's symbols"). The dispatcher does not check for it
(checking would be runtime overhead for an authoring discipline
already enforced by codegen review); a future post-MVP debug
build may add a re-entry guard if the discipline proves
brittle.

The dispatcher *is* reentrant across distinct `Arena&`s: two
threads each holding their own arena may each call
`dispatch_migration` concurrently. This is the cold-path
allowance of §6.1.

### 6.4 No cross-FQN ordering

Because the barrier serialises all migrations within a single
phase 8 (`specs/core/hot-reload-barrier-design.md` §3.2), the
dispatcher does not encode any "FQN A migrates before FQN B"
ordering. The barrier's per-plugin transaction order
(`hot-reload-protocol.md` §"Failure & Rollback") is the sole
source of ordering; dispatcher invocations are identical
regardless of FQN.

## 7. Persistence + ABI

### 7.1 Dispatcher table is part of the middleman dylib

The dispatcher table — the union of every FQN's `MigrationChain`
— is a static array compiled into `glibre-types.dylib` and
populated at middleman static-init (§4.3 inv. 5, §4.5 inv. 4 of
`specs/data/SPEC.md`; spike #734 for the middleman host TU
design). Plugins do **not** link the table; they reach it only
through `Envelope<T>::deserialize` (which calls into the
middleman) or through the loader's barrier-time
`migrate(...)` call (which also calls into the middleman). The
table is read-only after static-init; mutation is impossible
through any public API.

The table's storage layout mirrors the registry layout
(`specs/data/SPEC.md` §5 `RegistryEntry::migrations`): each
`RegistryEntry` carries an `eastl::span<const MigrationEntry>`
into the per-FQN sub-array. The sub-array is contiguous,
ascending-sorted by `from_version`, `O(1)`-indexed.

### 7.2 ABI hash composition

The dispatcher contributes to the middleman's
`glibre_types_abi_hash()` (§4.4 of `specs/data/SPEC.md`) via the
*data* it reads, not the *code* it runs. The composition rule
(per §4.4 inv. 1 of `specs/data/SPEC.md`):

> Blake3 over the concatenated, sorted per-type schema source
> hashes embedded in `glibre-types.dylib`.

The per-FQN schema source hash already includes the schema's
`current_version` and the set of registered migration provider
identifiers (per `fory-codegen.md` §"Schema File Format"
`migration v2_to_v3 { provider "..." }`). Therefore:

1. **Schema-bump → ABI hash bump.** A new `vN+1` schema entry
   alters the per-FQN source hash (the schema file gained a
   `migration` block); the middleman ABI hash changes; the
   plugin loader's hash check refuses any plugin built against
   the prior middleman (`plugin-abi.md` §"Loader Sequence" step
   4; `specs/data/SPEC.md` §10.1 arm 1).
2. **Migration body change without schema bump → no ABI hash
   change.** The schema source bytes are unchanged; the
   provider identifier (the function name) is unchanged; the
   middleman ABI hash is unchanged. This is correct: the body
   change is a behaviour change, not an ABI change. The body
   is still re-linked into the middleman on every middleman
   build (§4.5 inv. 4); the loader's ABI hash check is the
   wrong gate for body-correctness.
3. **Dispatcher source change → no ABI hash change.** The
   dispatcher's code does not appear in any `.fory` schema; its
   change is a middleman-internal refactor. Body-correctness
   gates are CI tests (§11), not the ABI hash.

The ABI hash gate is a **wire-level contract**, not a
build-level one (`specs/core/hot-reload-barrier-design.md` §7.2
states the same for the barrier). The dispatcher's contribution
is precisely its read-set (the chain table); its write-set (the
destination cell + arena) is per-call and contributes nothing.

### 7.3 No persisted dispatcher state

The dispatcher persists nothing. No log file, no on-disk
counter, no per-process state. The `MigrationReport` returned
by the barrier's `migrate(...)` (§8.2 of `specs/data/SPEC.md`)
records the count and the sorted span of migrated FQNs **per
barrier invocation**; that report is in-memory only and lives
no longer than the synchronous observer notification window
(`specs/data/SPEC.md` §8.5).

### 7.4 SONAME stability

Per §4.3 inv. 3 of `specs/data/SPEC.md` and `fory-codegen.md`
"ABI Stability Rules" #5, the middleman's SONAME bumps only on
ABI-breaking changes (i.e. when the dispatcher's public C-ABI
return shape changes, or when the per-`MigrationEntry` layout
gains a non-additive field). The §4 surface above is layout-
stable: future additions append to `MigrationEntry` only, never
reorder; future signatures of `dispatch_migration` may grow
parameters only at the end with default values; no SONAME bump
results from a body refactor or from a single FQN's chain
extension.

## 8. Hot-reload — the migration step of the barrier

The dispatcher *is* the migration step of the hot-reload barrier
(`specs/core/hot-reload-barrier-design.md` §4.4 sequence
`drain → swap → migrate → resume`, step 3). This section pins
the in/out contract.

### 8.1 In-contract: what the barrier provides

At the entry to step 3 the barrier guarantees:

1. **Live `SchemaRegistry` after the swap.** The registry has
   been updated to reflect Q's `(FQN, current_version,
   MigrationChain)` triples (per
   `specs/core/hot-reload-barrier-design.md` §4.4 step 2.4 and
   §8.3). Reads of any FQN in `incoming` resolve to Q's chain.
2. **Per-row `(src_ptr, dst_ptr)` pairs.** The barrier walks
   `World` storage one row at a time and presents to the
   dispatcher exactly the source and destination cells per
   `specs/core/hot-reload-barrier-design.md` §8.2. Source rows
   carry the stored `SchemaVersion` recorded in the per-row
   header.
3. **A per-payload arena slice.** Sized per
   `specs/data/SPEC.md` §9.2 row `MigrationDispatcher` (4 MiB
   sub-ceiling within the barrier's 16 MiB phase-8 arena).
   Reset between rows by the barrier; the dispatcher resets
   between *steps* (§3.3).
4. **Single-threaded invocation.** The barrier holds phase 8's
   exclusive lock; no concurrent dispatcher invocation exists
   (§6.1).
5. **ABI continuity.** The barrier has already verified
   (`specs/core/hot-reload-barrier-design.md` §7.1) that
   `Q.glibre_types_abi_hash() == host.glibre_types_abi_hash()`
   in Mode A, or that the §8.3 gates of `specs/data/SPEC.md`
   passed in Mode B. The dispatcher trusts the hash.

### 8.2 Out-contract: what the dispatcher returns

On a row's chain success: `expected<void, Error>` with `void`,
the destination cell holding the migrated `VM` bytes, the arena
reset to its pre-call high-water mark. The barrier increments
its `migrated_types` counter and proceeds to the next row.

On any hop's failure: `unexpected(Error)` wrapping the
refusing `(FQN, from, to)` triple under
`data::Error::SchemaMigrationFailure` (`specs/data/SPEC.md`
§10.1) or `data::Error::ArenaExhausted` (§10 below). The
destination cell is byte-identical to its pre-call state; the
arena is reset; no partial mutation is observable. The barrier
escalates: its own per-plugin rollback runs
(`specs/core/hot-reload-barrier-design.md` §10.1 row "Migrate
function returns `unexpected(...)`"), the loader un-swaps the
vtable (`hot-reload-protocol.md` §"Failure & Rollback"), and
the previous-good plugin keeps running.

The dispatcher does not call into the barrier; the barrier is
the only side that knows how to roll back. This preserves the
SRP split: dispatch is a pure function over `(registry, src,
arena)`; rollback is a transactional concern owned by the
barrier.

### 8.3 Schema-set continuity is the barrier's job

The §8.2 of `specs/data/SPEC.md` schema-set continuity check —
"every `FQN` in `outgoing` referenced by a surviving row is
also in `incoming`" — is performed by the barrier's
`migrate(...)` entry point (the wrapper around the
dispatcher), **not** by `dispatch_migration` itself. The
dispatcher operates one row at a time and never enumerates
FQNs across the registry; the barrier owns the enumeration.

This split keeps the dispatcher's complexity bounded:
`O(chain_length)` per row, `O(1)` registry reads, no FQN
iteration. The barrier's wrapper pays the `O(N_FQNs)` continuity
check once per reload, not per row.

### 8.4 Mode-B (middleman self-reload) interlock

The §8.3 of `specs/data/SPEC.md` Mode-B gate (middleman
self-reload) requires `MigrationChain` coverage to be checked
*before any byte is migrated*. The dispatcher contributes the
`validate_chain(...)` entry point (§3.5 here, §4 surface) for
this gate: the barrier enumerates `Q_types`'s registry entries,
calls `validate_chain` for each, and refuses the reload on any
non-`Ok` return. This is the §8.3 gate-2 mechanism; the
dispatcher provides the validator, the barrier orchestrates.

Mode B is **post-MVP** (`specs/data/SPEC.md` §8.3,
`hot-reload-protocol.md` "Open Questions" #2). The validator
exists in MVP because static-init also uses it (§3.5); Mode-B
adoption only re-uses the validator at a different call site.

## 9. Performance

### 9.1 Budget — quoted from `specs/data/SPEC.md` §9.2

The dispatcher's row of the per-aggregate budget:

| Cell                               | Value                                              | Source                                              |
|------------------------------------|----------------------------------------------------|-----------------------------------------------------|
| Per-frame steady-state             | 0 — invoked at hot-reload only; 0 on hot path      | §9.2 of `specs/data/SPEC.md`                        |
| Heap sub-ceiling                   | 4 MiB (counted under `data` ContextTag)            | §9.2 row `MigrationDispatcher`                      |
| Phase-8 contribution               | inside `data`'s 0.20 ms reload-frame share         | §9.1 of `specs/data/SPEC.md`                        |
| Per-step migration call            | ≤ 50 µs (CI gate)                                  | §9.4 row 3 (`migration_dispatch_v_minus_1.bench.cpp`) |
| Concurrency                         | single-threaded                                    | §6.1 here; `hot-reload-protocol.md` Consequences    |

The phase-8 contribution is part of the barrier's 0.40 ms
reload-frame ceiling (`specs/core/hot-reload-barrier-design.md`
§9.2 substep "Migrate (per plugin)" `<= 0.20 ms`). The
dispatcher's *share* of that 0.20 ms is the per-row chain walk;
the remaining cost (registry lookup, per-row in-place commit,
arena reset between rows) lives in the barrier.

### 9.2 Per-payload migration wall-time budget

For a single payload of `(FQN, src_version → current_version)`:

| Quantity                          | Budget        | Notes |
|-----------------------------------|---------------|-------|
| Per-step invocation               | ≤ 50 µs       | `migration_dispatch_v_minus_1.bench.cpp` (§9.4 row 3 of `specs/data/SPEC.md`) |
| Per-payload total (1-step)        | ≤ 50 µs       | One-version-old payload — the common case |
| Per-payload total (k-step)        | ≤ 50 µs × k   | Linear in chain length; no parallelism |
| Per-row arena reset between steps | ≤ 1 µs        | Pointer assignment + scratch invalidation; bounded by ping/pong cell size |
| Per-row in-place commit           | ≤ 1 µs        | `std::memcpy` for trivially-copyable; bounded by `sizeof(VM)` |

The 50 µs ceiling per step is the engine-wide promise to plugin
authors: a migration body that exceeds it triggers a CI failure
and forces the author to either simplify the body or split the
migration across multiple `vN → vN+1` hops (which the chain
walks at the same 50 µs ceiling each).

### 9.3 Arena cell sizing for peak migration set

The dispatcher's arena working-set is bounded by:

```
arena_use(payload) =
    2 * max_over_chain(sizeof(Vk))            // ping + pong handoff cells
  + max_over_chain(scratch_step_k)            // per-step body scratch
```

Sized so the largest single payload fits within `data`'s 4 MiB
sub-ceiling (`specs/data/SPEC.md` §9.2). For typical persistent
aggregates (transforms, attribute sets, container rows; see
`harmonius/docs/design/data-systems/` for size baselines), peak
single-payload arena use is `O(KB)`, leaving multi-MiB headroom
for chain-length and scratch growth.

The 4 MiB sub-ceiling sizes the *peak migration set* — the
largest single row's arena working-set. The barrier's
per-row arena reset (`specs/core/hot-reload-barrier-design.md`
§8.2 step 3) ensures that `arena_use(payload)` is the only
quantity that matters, not `Σ arena_use(payload_i)` across all
rows. This is the load-bearing reason the dispatcher is
reset-between-steps and reset-between-rows: peak memory is one
payload's working-set, regardless of row count or chain length.

### 9.4 Allocator behaviour

- **All dispatcher allocations carry the `data` `ContextTag`**
  per `perf-budget.md` "Allocator Rules" #1 and
  `specs/data/SPEC.md` §9.5.
- **Dispatcher arena counts against `data`'s 4 MiB sub-ceiling**
  per `specs/data/SPEC.md` §9.5 #3, which itself counts against
  `core`'s 16 MiB phase-8 arena ceiling
  (`perf-budget.md` "Allocator Rules" #6,
  `specs/core/hot-reload-barrier-design.md` §9.3).
- **No raw `new` / `malloc`** inside the dispatcher source per
  the `-Wglibre-no-raw-alloc` build flag.
- **Arena exhaustion is a refusal**, not a grow (per #499; §10
  arm `ArenaExhausted` below). Strict-mode debug builds raise
  the same arm; release builds raise the same arm — the
  dispatcher's behaviour is identical across build modes.

### 9.5 CI gate

`perf-budget.yml` "CI Gate Spec" inherits `specs/data/SPEC.md`
§9.4 row 3 verbatim:

> `migration_dispatch_v_minus_1.bench.cpp` — deserializes a
> one-version-old payload through `MigrationDispatcher` for
> every registered single-step migration in the build,
> asserting per-call ≤ 50 µs.

The dispatcher's plan-level Catch2 `BENCHMARK` block (§11.3
below) asserts both the per-step (`≤ 50 µs`) and the
per-payload (`≤ 50 µs × k`) ceilings against the registered
migration set.

## 10. Failure modes

The dispatcher surfaces failure exclusively through the
`data::Error` closed sum (§10.1 of `specs/data/SPEC.md`),
returned via `std::expected<void, glibre::Error>` (`error-model.md`
"Decision" #3). One amendment to `specs/data/SPEC.md` §10.1 is
required (and recorded here for the implementation plan to
land in the same PR as the new failure point per §10 of
`specs/data/SPEC.md`):

- **`ArenaExhausted`** — append to `data::Error::ErrorTag` as
  enumerator value 10. Arm fires when the per-payload arena
  cannot satisfy a hop's allocation request and the refuse
  policy of #499 is in effect (the only policy in MVP).

The complete dispatcher-emitted arm set is below.

### 10.1 Refusal arms (enumerated)

| Arm                              | Trigger                                                                                                                  | Detection point                  | Recovery                                                                                                                                           | Severity | core::Error wrapping                                                          |
|----------------------------------|--------------------------------------------------------------------------------------------------------------------------|----------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------|----------|-------------------------------------------------------------------------------|
| `MigrationStepMissing`           | The chain table for `(FQN, src_version)` lacks the entry whose `from_version == src_version` (`specs/data/SPEC.md` §10.1 row 7). | `dispatch_migration` step 1 (§3.3 index) | refuse decode (cold path); refuse load (warm path — barrier rolls back).                                                                            | error / warn | `core::Error::SchemaMigrationFailed` per `specs/data/SPEC.md` §10.3.        |
| `SchemaMigrationFailure`         | A `MigrationFn` body returned `std::unexpected(...)` (§10.1 row 2 of `specs/data/SPEC.md`). | `dispatch_migration` step 5 (§3.3 branch) | refuse decode (cold path); refuse load (warm path). Per-row rollback discipline (§4.7 inv. 4 of `specs/data/SPEC.md`); destination untouched.       | error / warn | `core::Error::SchemaMigrationFailed`; the data arm carries the `(FQN, from, to)` of the refusing step. |
| `ArenaExhausted` (NEW)           | The arena cannot satisfy a hop's allocation request; #499 refuse policy.                                                  | `dispatch_migration` step 3 (§3.3 allocate) | refuse decode (cold path); refuse load (warm path). The arena was already at capacity before the dispatcher entered; no partial bytes published.   | error / warn | `core::Error::SchemaMigrationFailed` (collapses into the same loader response as `MigrationStepMissing` — operator profiles the migration set and either reduces row count or, post-#499, increases the arena). |
| `MigrationCycle`                 | The chain table contains a back-edge or a non-monotonic `from → to`. Codegen-time and static-init time only; never reaches `dispatch_migration`. | `validate_chain` (§3.5)         | abort build (codegen path) / process abort (static-init path). Same as `specs/data/SPEC.md` §10.1 arm 8.                                              | info / fatal | None — codegen / static-init arm; never crosses into normal runtime. |
| (impossible at runtime: `SchemaRegistryConflict`, `ReservedTagViolation`, `DeserializeError`, `EnvelopeTruncated`, `SchemaUnknown`, `AbiHashMismatch`) | These arms exist elsewhere in `data::Error`; the dispatcher does not raise them. The registry is read-only by the time the dispatcher runs; the envelope was already decoded; the registry lookup already succeeded. | (other aggregates) | (other recovery paths) | (other) | (per `specs/data/SPEC.md` §10) |

### 10.2 Failure isolation

§4.7 inv. 4 of `specs/data/SPEC.md` is the load-bearing
invariant: "no partial mutation is observable to the caller
(the destination value is not exposed on failure)." The
dispatcher implements this by:

1. **Allocate-then-replace** (§3.4). Destination cell is
   untouched until the chain succeeds.
2. **Arena reset on every exit path** (§3.3 step 6, plus
   `try`/`catch`-free single-return-point structure). On
   failure the arena is reset to the call-entry high-water
   mark before returning.
3. **No global state to corrupt.** §6.2 (no locks, no atomics,
   no thread-local storage) ensures that a half-completed
   chain leaves no observable side-effect anywhere.

### 10.3 Cycle detection is static

`MigrationCycle` is a codegen-time and static-init-time arm
**only**. The dispatcher's runtime walk never re-checks for
cycles because `validate_chain(...)` already proved
acyclicity at static-init (§3.5 step 2). A cycle that somehow
reached runtime would be a memory-corruption defect, not a
data error; the dispatcher has no recovery story for it
(matching `specs/data/SPEC.md` §10.1 arm 8 severity:
`fatal` at static-init, `info` at codegen).

### 10.4 Logging discipline

The dispatcher emits **no** log calls. Per
`specs/data/SPEC.md` §10.4 and `error-model.md` §"Logging /
Telemetry" #1, every constructed `data::Error` is logged at
the boundary that *handles* it — the loader (warm path) or
the deserialize call site (cold path). The dispatcher
returns a typed `Error` and is silent.

The handler that receives a dispatcher-raised `Error`
populates the `(FQN, from, to)` triple into the log's
structured fields. Empty triples (which the dispatcher
never produces) would be omitted by the `error-model.md`
§"Logging" #2 rule.

## 11. Test plan

Tests are unit-first (per-row behaviour against synthetic
chains) and integration-second (full barrier-time migration
sweep), backing the `specs/data/SPEC.md` §11 acceptance
criteria #368, #369, #371, #372 and the `error-model.md`
§"Test coverage obligations" rule.

### 11.1 Unit tests — `MigrationDispatcher`

Located at `tests/data/migration/dispatcher/`. Each fixture
constructs a synthetic `MigrationChain` against the test-only
`bump_schema_version` fixture (§8.6 of `specs/data/SPEC.md`)
and asserts on the dispatcher's behaviour for one call.

Required cases:

1. **Identity.** `src_version == current_version` is rejected
   at the dispatcher entry (the caller — barrier or
   deserialize — should not have called the dispatcher in the
   first place; the dispatcher returns `unexpected(MigrationStepMissing)`
   with `from == to` to surface the misuse). Also asserts
   the destination cell is byte-identical to its pre-call
   state.
2. **One-step (`v1 → v2`).** The common case: a chain with
   one entry, one hop, success. Asserts `*dst_payload`
   equals the expected `V2` value (golden), arena is reset,
   `expected.has_value()`.
3. **Two-step (`v1 → v3`).** Tests the ping-pong cell
   alternation. Same shape as #2 but two entries and two
   hops; intermediate `V2` is *not* exposed to the caller.
4. **k-step for k ∈ {3, 4, 5}.** Verifies linearity and that
   the per-step time budget (≤ 50 µs) holds across chain
   lengths typical of MVP schemas.
5. **Mid-chain failure.** A chain whose third hop's body
   returns `unexpected`. Asserts `expected.error()` carries
   `(FQN, 3, 4)`, destination is byte-identical to pre-call,
   arena is reset.
6. **Chain-coverage gap.** A chain whose entries are `[(2 →
   3), (3 → 4)]` and a payload at `src_version == 1`.
   Asserts `expected.error()` carries
   `MigrationStepMissing` with `(from = 1, to = 2)`.
7. **Arena exhaustion.** A chain whose first hop's body
   requests more bytes than the arena holds. Asserts
   `expected.error()` carries `ArenaExhausted`,
   destination is byte-identical, arena is reset (no
   leak past the high-water mark).
8. **Per-step row table coverage.** For every `(FQN,
   from, to)` in the live registry, dispatch one
   one-version-old payload and assert success. Backs
   `specs/data/SPEC.md` §11 row #368 / #369. The row count
   matches the registered chain length; the test enumerates
   the registry, not a hard-coded list, so new schemas pick
   up coverage automatically.

### 11.2 Unit tests — `validate_chain`

Located at `tests/data/migration/validator/`. Cover the §3.5
gates against synthetic chains.

Required cases:

1. **Dense ascending chain.** `[(1→2), (2→3), (3→4)]` with
   `current_version == 4`. Asserts `Ok`.
2. **Truncated chain.** `[(1→2), (2→3)]` with
   `current_version == 4`. Asserts `MigrationStepMissing`.
3. **Cycle.** `[(1→2), (2→1)]`. Asserts
   `MigrationCycle` with `(step_from = 2, step_to = 1)`.
4. **Gap.** `[(1→2), (3→4)]` with `current_version == 4`.
   Asserts `MigrationCycle` (the §3.5 inv. 1 density check;
   the gap is structurally indistinguishable from a
   reordered cycle, and the validator surfaces both as
   `MigrationCycle` to keep the codegen-time error message
   actionable).
5. **Null function pointer.** `[(1→2 with invoke = nullptr)]`.
   Asserts `MigrationStepMissing`.
6. **Coverage past `current_version`.** `[(1→2), (2→3)]`
   with `current_version == 2`. Asserts
   `MigrationCycle` (the chain overshoots the registry's
   declared current version — chain-too-long direction per
   §3.5 bullet 4; same density-gate arm as gaps and
   back-edges).

Each `validate_chain` fixture asserts both the correct arm
and the populated payload fields per `specs/data/SPEC.md`
§10.5.

### 11.3 Integration tests — full barrier-time migration sweep

Located at `tests/data/integration/hot_reload/`. Drive the
barrier's `migrate(...)` against a synthetic world snapshot
and assert end-to-end behaviour.

Required cases:

1. **Sweep success.** A world with N rows of types `T1, T2,
   T3`, each at version `current_version - 1`. Run
   `migrate(...)` once; assert every row migrated, every
   row's storage now holds `current_version` bytes,
   `MigrationReport::count == N`, `migrated` span sorted
   ascending.
2. **Sweep refusal — missing chain step.** Same world but
   one type's chain is missing the `(N - 1 → N)` step.
   Assert `migrate(...)` returns `unexpected(MigrationStepMissing)`,
   no row in `World` was touched, `MigrationReport`'s
   partial contents (the rows migrated before the refusing
   FQN was encountered) are surfaced to the loader.
3. **Sweep refusal — body fails.** Inject `force_migration_failure`
   for a known `(FQN, from, to)` (per
   `specs/data/SPEC.md` §8.6). Assert `unexpected(SchemaMigrationFailure)`
   wraps the failing triple, the world is byte-identical to
   the pre-`migrate` state, the arena is reset.
4. **Sweep refusal — arena exhaustion.** Use a
   barrier-arena sized below the largest row's working-set;
   assert `unexpected(ArenaExhausted)`, world untouched,
   arena reset. Documents the #499 refuse policy.
5. **Idempotence under retry.** Call `migrate(...)` against
   an already-current world; assert
   `MigrationReport{count = 0, migrated = {}}`, no per-row
   work, no arena allocation. Backs §8.2 step 5
   "Idempotence" of `specs/data/SPEC.md`.
6. **Per-step time budget under load.** Run sweep #1 with N
   = 10 000 rows of a type whose `current_version - 1`
   chain step is at the 50 µs ceiling. Assert the wall-clock
   total fits the 0.20 ms phase-8 substep cell only when the
   number of migrated rows × step-cost is within budget;
   over-budget cases surface as a CI perf-gate failure (not
   a runtime refusal — the budget is a CI contract, not a
   runtime gate).

### 11.4 Perf benchmarks

Located at `data/runtime/test/perf/migration_dispatch_v_minus_1.bench.cpp`.
Required by `specs/data/SPEC.md` §9.4 row 3:

- **`migration_dispatch_v_minus_1.bench.cpp`** — deserializes
  a one-version-old payload through `MigrationDispatcher`
  for every registered single-step migration in the build,
  asserting per-call ≤ 50 µs over a 100k-iteration window.
  Verifies the §9.2 per-step ceiling.

The benchmark is parameterised across the live chain table:
a new schema bump that adds a chain entry automatically
adds a benchmark row; CI fails if any new row exceeds the
ceiling.

### 11.5 Coverage matrix

| Concern                                          | Test                                        | Discharge |
|--------------------------------------------------|---------------------------------------------|-----------|
| Per-step body table coverage                     | §11.1 case 8                                | `specs/data/SPEC.md` §11 #368 / #369 |
| Per-payload chain composition                    | §11.1 cases 2, 3, 4                          | `specs/data/SPEC.md` §11 #368 |
| Mid-chain failure isolation                      | §11.1 case 5; §11.3 case 3                  | `specs/data/SPEC.md` §10.5 row `SchemaMigrationFailure` |
| Chain coverage gap                                | §11.1 case 6; §11.2 case 2                  | `specs/data/SPEC.md` §10.5 row `MigrationStepMissing` |
| Cycle detection                                  | §11.2 case 3                                | `specs/data/SPEC.md` §10.5 row `MigrationCycle` |
| Arena exhaustion (#499 refuse policy)            | §11.1 case 7; §11.3 case 4                  | new arm `ArenaExhausted` (§10.1) |
| Idempotence on already-current registry          | §11.3 case 5                                | `specs/data/SPEC.md` §8.2 step 5 |
| Per-step wall-time budget                        | §11.4                                       | `specs/data/SPEC.md` §9.4 row 3 |
| Static-init validator gates                      | §11.2 cases 1, 4, 5, 6                      | §3.5 here |
| Sweep-time integration                           | §11.3 cases 1, 2, 6                         | `specs/data/SPEC.md` §11 #371 / #372 |

### 11.6 Story closure

Acceptance criteria #368, #369, #371, and #372 of
`specs/data/SPEC.md` close on the union of §11.1 + §11.3 +
§11.4. Each story carries a Catch2 test by name per
`specs/data/SPEC.md` §11; the names below are the
implementation plan's contract:

- #368 → `MigrationDispatcher::dispatches_chain_in_ascending_order`
  (§11.1 cases 2, 3, 4).
- #369 → `MigrationDispatcher::per_schema_round_trip_golden`
  (§11.1 case 8).
- #371 → `MigrationDispatcher::sweep_walks_world_at_phase_8`
  (§11.3 case 1).
- #372 → `MigrationDispatcher::phase_8_budget_within_target`
  (§11.4 + §11.3 case 6).

## 12. Open questions

- [OPEN] **Arena grow vs. refuse policy** — #499
  (`hot-reload-migration-arena-grow-vs-refuse`). The dispatcher
  adopts the refuse policy in MVP (§3.4, §10 arm
  `ArenaExhausted`). The post-MVP decision record at
  `reviews/decisions/hot-reload-arena-policy.md` will either
  amend §3.4 to enable a two-phase grow, or confirm the
  refuse policy permanently. No behaviour change in this
  design until the record lands.
- [OPEN] **Mode-B middleman self-reload enabling spike**
  (`hot-reload-protocol.md` "Open Questions" #2;
  `specs/data/SPEC.md` §8.3). The dispatcher provides
  `validate_chain` for the Mode-B gate today (it is
  needed by static-init regardless), but Mode-B is not
  enabled in MVP. When the spike opens, §8.4 here gains
  the operator-facing details (engine-restart fallback,
  CI gate to prove every reload candidate's chain is
  coverage-complete before phase 8 is allowed to enter
  Mode B).
- [OPEN] **Lazy-migration cache**
  (`specs/data/SPEC.md` §9.6). Two concrete users would have
  to demand it (PHILOSOPHY §10). Currently zero; defer.
- [OPEN] **Reentrancy guard in debug builds**. §6.3 forbids
  reentrancy by authoring discipline. A debug-only
  thread-local guard could turn a body bug into an
  immediate diagnostic; the cost is one TLS load per
  invocation. Decide when the first such bug bites or
  when a plan to introduce the guard is opened.
- [OPEN] **Source-location capture for the
  `(FQN, from, to)` triple**. The dispatcher fills the
  triple from registry data; the originating context's
  body line that caused the refusal is not recorded.
  `error-model.md` "Open Questions" #3 records the
  same question for `ErrorContext`; defer to that
  resolution.
