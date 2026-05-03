# Data Context Spec

## 1. Purpose

The `data` context owns the **type and schema spine** that lets every plugin
agree on the shape of persistent state without sharing headers. Concretely it
owns: the `data/schemas/<ctx>/<Type>.fory` schema files (format and lint), the
`glibre-foryc` host codegen tool, the generated `glibre-types.dylib`
middleman that every plugin links, the per-type schema registry
(`(fqn, version, blake3-hash)`), the migration dispatcher (vN → vN+1 chain
plus the loader-side hot-reload integration at the frame-8 barrier), and the
single exported `glibre_types_abi_hash` value the plugin loader compares on
load. Errors thrown from this layer are typed (`AbiHashMismatch`,
`SchemaMigrationFailure`, `DeserializeError`, `ReservedTagViolation`,
`SchemaRegistryConflict`) and live in the middleman.
**It refuses to own** domain semantics or runtime behavior of any kind:
no ECS storage or scheduling (that is `core`), no inventories or stat
modifiers or effect ticking or quest graphs (those would be plugin-side
domain contexts that *use* this spine; harmonius's `R-16.1`–`R-16.4`
data-systems requirements are deliberately out of scope here), no transport
or network framing (a future `net` context wraps Fory blobs but does not
live here), no on-disk asset I/O or file-format negotiation (`platform` /
`content` own loaders; this context only defines the byte layout they read
and write), no rendering, no editor authoring UX, no determinism policy
beyond the byte-equality guarantee that flows from a single canonical wire
format and tag-sorted struct layout. The single thing that crosses the
dylib boundary is bytes; the single thing the data context promises is
that those bytes round-trip through a generated, version-aware,
ABI-hashed C++ struct with deterministic layout.

## 2. Ubiquitous Language

Terms used unchanged in code, schema files, status comments, and tests.

| Term | Meaning |
|------|---------|
| Schema | A `data/schemas/<ctx>/<Type>.fory` file declaring one persistent type's fields, tags, version, and migrations. The authoring artifact. One file per persistent aggregate, owned by the originating bounded context. |
| Schema Source Hash | Blake3 of the canonicalized bytes of a single `.fory` file. Recorded per-type in the registry. |
| ABI Hash | `glibre_types_abi_hash()` — Blake3 over the concatenated, sorted per-type schema source hashes embedded in `glibre-types.dylib`. Single scalar the plugin loader compares on load; mismatch → refuse load. |
| Tag | The immutable wire-level field identifier inside a schema. Once shipped, tag numbers are never reused; removed fields become `reserved` tags. Tag-sorted ascending defines generated struct field order. |
| Reserved Tag | A previously used tag whose field was removed; codegen forbids reuse and `glibre-foryc` fails the build on a reserved-tag collision. |
| Schema Version | Monotonically increasing integer on each schema. Bumped when fields are added/removed/renamed. Encoded in the Fory envelope of every payload. |
| Builtin | A name from the audited primitive set (`u8`/`u16`/`u32`/`u64`/`i8`/`i16`/`i32`/`i64`/`f32`/`f64`/`bool`/`string`/`bytes`/`vec3f`/`quatf`/`entity`/`list<T>`/`map<K,V>`/`option<T>`) that compiles to a fixed C++ type in `glibre/types/_builtins.hpp`. |
| Generated Type | A `final`, vtable-free, declaration-order-independent C++ struct emitted by `glibre-foryc` for one schema. POD-like; trivially copyable when the field set permits; layout is tag-sorted. |
| Middleman | `glibre-types.dylib` — the single shared library that holds every generated type, the registry, the migration dispatcher, and the ABI hash. Every plugin and the runtime / editor binaries link it; no plugin links Fory directly. |
| Foryc | `glibre-foryc`, the host-only codegen tool. Wraps Apache Fory's C++ generator, enforces glibre's ABI rules (tag-sort, reserved-tag check, layout-additive assertion), emits `_registry.cpp` and `_abi_hash.cpp`. |
| Schema Registry | The static table inside `glibre-types.dylib` mapping each `fqn` to `(version, schema_source_hash, serialize_fn, deserialize_fn, migrations)`. Populated at static-init time; no runtime mutation. |
| Reflection Blob | The compact, codegen-emitted descriptor of a generated type's fields and tags, queryable in tools and editor builds via the registry. **Not** present-as-runtime-reflection in shipping builds; it is data the codegen wrote, not introspection. |
| Migration | A pure free function `migrate_<Type>_v<N>_to_v<N+1>(const VN&, VNplus1&) -> std::expected<void, glibre::Error>`, written by the originating context and registered through a codegen-emitted macro. Deterministic; no I/O; no allocation outside the supplied arena. |
| Migration Chain | The sequence of single-step migrations the dispatcher composes when an inbound payload's schema version is older than current. Missing chain → `SchemaMigrationFailure`. |
| Envelope | The Fory-defined header `(fqn, version, …)` prepended to every payload; what the deserializer reads first to dispatch decode + migration. |
| Persistent Aggregate | A type whose instances cross either a save boundary, a hot-reload boundary, or a plugin-dylib boundary. Anything persistent has a schema; anything not persistent does not. |
| FQN | Fully-qualified name of a generated type, e.g. `glibre.core.Transform`. The schema's primary identity in the registry and on the wire. |
| Hot-Reload Barrier | The frame-8 boundary at which the loader drains the world, swaps plugin dylibs, runs deserialize-with-migration over the snapshot, and either resumes or rejects the swap (PHILOSOPHY §8). The data context owns the migration step inside this barrier. |

## 3. Derived From

Harmonius prior art is treated as research input only; every conclusion is
re-derived against PHILOSOPHY (`/Users/cjhowe/Code/glibre/PHILOSOPHY.md`)
and the engine-wide decision in
`reviews/decisions/fory-codegen.md`. The harmonius `data-systems/` corpus
is not requirements truth for the glibre `data` context — most of what it
calls "data systems" is *domain* matter that the glibre `data` context
explicitly **refuses** (see refusals below). What survives the re-derivation
is the single cross-cutting concern that every harmonius data-system shares:
*every persistent type wants a schema, a serializer, and a forward-migration
story*. That concern collapses into glibre's Apache Fory codegen pipeline,
producing the `glibre-types.dylib` middleman and the ABI-hash gate
(§1, §2; `reviews/decisions/fory-codegen.md`).

### 3.1 Files cited (input only)

Requirements (`/Users/cjhowe/Code/harmonius/docs/requirements/data-systems/`):

- `attributes-effects.md` — `R-16.1.1`–`R-16.1.x` (Meters, Attributes,
  Modifier Stacks, Effects).
- `containers-slots.md` — `R-16.2.1`–`R-16.2.x` (Containers, Grid layout,
  Stacking, Sockets).
- `data-tables.md` — `R-16.3.1`–`R-16.3.x` (Schemas, Rows, Foreign Keys,
  Indices, Locale).
- `directed-graphs.md` — `R-16.4.1`–`R-16.4.x` (Topology, Conditional /
  Ordered variants, Queries, Tree ops).

Design (`/Users/cjhowe/Code/harmonius/docs/design/data-systems/`):

- `attributes-effects.md` (and `-test-cases.md`) — informs the *shape* of a
  schema-driven definition, not the runtime semantics.
- `containers-slots.md` (and `-test-cases.md`) — informs the *shape* of
  bounded-collection persistent state, not container behavior.
- `data-tables.md` (and `-test-cases.md`) — informs the *shape* of typed
  row schemas with constraints and FK references; **table evaluation,
  joins, indices, and inheritance are out of scope here**.
- `directed-graphs.md` (and `-test-cases.md`) — informs the *shape* of
  graph topology persistence; **graph algorithms, traversal, conditional
  evaluation, and tree operations are out of scope here**.
- `composition.md` (and `-test-cases.md`) — referenced for the cross-cutting
  binding between immutable definitions and ECS components, which in glibre
  is split: the *binding* belongs to `core` (ECS storage + plugin loader),
  the *type and schema* belong here.

### 3.2 What is borrowed (the cross-cutting collapse)

A single concern — and only this concern — is fused from harmonius into the
glibre `data` context, replacing the `rkyv`-based archive scheme used across
every cited design doc:

- **Schema-driven serialization with monotonically-versioned types and
  forward migrations.** Harmonius scattered this concern across `rkyv`
  archive derivations on `MeterDefinition`, `AttributeSchema`,
  `EffectDefinition`, container row types, data-table row types, and graph
  node/edge types — each domain rolling its own
  `Archive`/`Serialize`/`Deserialize` discipline (cf.
  `attributes-effects.md` §"Serialization and Replication";
  `directed-graphs.md` §"All types derive rkyv …"; `data-tables.md` row
  schemas; `containers-slots.md` definitions). Glibre collapses every one
  of those scattered serialization disciplines into **one** pipeline: the
  Apache Fory codegen flow whose schema files live at
  `data/schemas/<ctx>/<Type>.fory`, whose generated types compile into the
  single `glibre-types.dylib` middleman, and whose hot-reload migrations
  run at the frame-8 barrier (`reviews/decisions/fory-codegen.md` §Pipeline,
  §Migration Mechanic). The glibre rationale is PHILOSOPHY §6 (zero
  runtime reflection in shipping builds), §7 (deterministic byte-equal
  snapshots), §9 (ABI-hash refusal at plugin load), and §10 (one collapsed
  primitive per Occam pass).

The Occam collapse, stated as a single sentence:
> N domain-specific harmonius `rkyv` archive derivations → 1 glibre
> Fory-codegen pipeline producing 1 middleman dylib gated by 1 ABI hash.

### 3.3 What is explicitly refused (sent elsewhere)

The harmonius `data-systems/` package mostly describes **domain logic**
that glibre's `data` context refuses to host. These responsibilities
belong to the future *game-framework* layer (a plugin family that *uses*
the data spine, not part of the engine core), and their `R-16.x` IDs are
**not** in scope for this spec:

- **Attributes / Effects (`R-16.1.1`–`R-16.1.x`).** Meter ticking,
  threshold event firing, modifier-stack evaluation, effect
  type/duration/period semantics, stacking rules — all reject. The `data`
  context only owns the *bytes* a future `Meter` or `AttributeSet` schema
  would round-trip through; the runtime behavior belongs in a
  game-framework plugin context (or an even higher-level title plugin).
- **Containers / Slots / Sockets (`R-16.2.1`–`R-16.2.x`).** Capacity and
  weight enforcement, grid bin-packing, stacking and merge rules, nesting
  depth validation, sort operations, socket compatibility tags, modifier
  propagation, visual override binding — all reject. None of these are
  serialization concerns.
- **Data Tables (`R-16.3.1`–`R-16.3.x`).** Typed table evaluation, load-time
  constraint validation, prototype-chain row inheritance, foreign-key
  resolution, cross-table joins, hash and BTree indices, locale tables —
  all reject. Authored data lives in schemas and assets owned by
  `content` / `platform`; *queries against* that data live in domain
  contexts. The `data` spine only ensures the row bytes round-trip
  deterministically through generated types.
- **Directed Graphs (`R-16.4.1`–`R-16.4.x`).** `DirectedGraph<N,E>`
  primitive, cycle detection, topological sort, conditional / ordered
  variants, weighted shortest-path / reachability, BFS / DFS traversal,
  tree operations (LCA, subtree, ancestor path) — all reject. Graph
  topology *as persisted bytes* is a schema concern; graph *evaluation*
  is a domain plugin concern (quest, dialogue, talent, ability).
- **Transport / network framing.** Out of scope; a future `net` context
  may wrap Fory blobs, but no transport policy lives here (per §1).
- **IO routing / file-format negotiation.** Out of scope; `platform` /
  `content` own loaders. The `data` context only defines the byte layout
  those loaders read and write (per §1).
- **Domain logic of any kind.** No invariants beyond byte-equal round-trip
  through a versioned, ABI-hashed type live in `data`.

This refusal list is the load-bearing claim of §3: harmonius's "data
systems" is mostly *not* what glibre calls `data`. The glibre `data`
context is intentionally smaller — exactly the type-and-schema spine
described in §1.

## 4. Aggregates & Invariants

- Aggregate / entity / value object.
- Invariants that must hold at every public API boundary.

## 5. Public Interface

```cpp
// header-only stub goes here
```

Event types, serialized schemas (Fory), error types.

## 6. Internal Architecture

Non-binding sketch for implementers.

## 7. Persistence & Schemas

Fory schemas. Migration rules.

## 8. Hot-Reload Contract

What survives swap, what `migrate(...)` must do, what triggers refusal.

## 9. Performance Budget

Cycles / frame, memory ceiling, allocation rules.

## 10. Failure Modes & Error Model

Typed errors. Recovery.

## 11. Acceptance Criteria

GitHub `type:user-story` issues this spec closes:

- #TBD — `<title>`

Each must have a Catch2 test by name.

## 12. Open Questions

- Owner / resolution gate.
