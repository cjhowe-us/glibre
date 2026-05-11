# Data Context Spec

## 1. Purpose

The `data` context owns the **type and schema spine** that lets every plugin
agree on the shape of persistent state without sharing headers. Concretely it
owns: the `data/schemas/<ctx>/<Type>.fbs` schema files (format and lint), the
`glbr-sergeant` host codegen tool, the generated `glibre-types.dylib`
middleman that every plugin links, the per-type schema registry
(`(fqn, version, blake3-hash)`), the schema alias table (type-rename and
field-rename aliases for structural evolution), the loader-side hot-reload
integration at the frame-8 barrier (`reconcile(...)`), and the single exported
`glibre_types_abi_hash` value the plugin loader compares on load. Errors thrown
from this layer are typed (`AbiHashMismatch`, `SchemaMigrationFailure`,
`DeserializeError`, `ReservedTagViolation`, `SchemaRegistryConflict`) and live
in the middleman.
**It refuses to own** domain semantics or runtime behavior of any kind:
no ECS storage or scheduling (that is `core`), no inventories or stat
modifiers or effect ticking or quest graphs (those would be plugin-side
domain contexts that *use* this spine; harmonius's `R-16.1`–`R-16.4`
data-systems requirements are deliberately out of scope here), no transport
or network framing (a future `net` context wraps Flatbuffers blobs but does not
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
| Schema | A `data/schemas/<ctx>/<Type>.fbs` file declaring one persistent type's fields, tags, version, and migrations. The authoring artifact. One file per persistent aggregate, owned by the originating bounded context. |
| Schema Source Hash | Blake3 of the `.bfbs` binary schema bytes for a single type. Recorded per-type in the registry (Q7, ADR #1121). |
| ABI Hash | `glibre_types_abi_hash()` — Blake3 over the LF-joined, FQN-sorted per-schema entries of `(fqn + ":" + version_le + ":" + schema_source_hash)`, embedded in `glibre-types.dylib`. Single scalar the plugin loader compares on load; mismatch → refuse load. See §4.4 inv. 1 and `reviews/decisions/plugin-abi.md` §"ABI Hash Function" rule 1. |
| Tag | The immutable wire-level field identifier inside a schema. Once shipped, tag numbers are never reused; removed fields become `reserved` tags. Tag-sorted ascending defines generated struct field order. |
| Reserved Tag | A previously used tag whose field was removed; codegen forbids reuse and `glbr-sergeant` fails the build on a reserved-tag collision. |
| Schema Version | Monotonically increasing integer on each schema. Bumped when fields are added/removed/renamed. Encoded in the Flatbuffers envelope of every payload. |
| Builtin | A name from the audited primitive set (`u8`/`u16`/`u32`/`u64`/`i8`/`i16`/`i32`/`i64`/`f32`/`f64`/`bool`/`string`/`bytes`/`vec3f`/`quatf`/`entity`/`list<T>`/`map<K,V>`/`option<T>`) that compiles to a fixed C++ type in `glibre/types/_builtins.hpp`. |
| Generated Type | A `final`, vtable-free, declaration-order-independent C++ struct emitted by `glbr-sergeant` for one schema. POD-like; trivially copyable when the field set permits; layout is tag-sorted. |
| Middleman | `glibre-types.dylib` — the single shared library that holds every generated type, the registry, the migration dispatcher, and the ABI hash. Every plugin and the runtime / editor binaries link it; no plugin links Flatbuffers directly. |
| Sergeant | `glbr-sergeant`, the host-only codegen tool. Wraps Flatbuffers's C++ generator, enforces glibre's ABI rules (tag-sort, reserved-tag check, layout-additive assertion), emits `_registry.cpp` and `_abi_hash.cpp`. |
| Schema Registry | The static table inside `glibre-types.dylib` mapping each `fqn` to `(version, schema_source_hash, serialize_fn, deserialize_fn, migrations)`. Populated at static-init time; no runtime mutation. |
| Reflection Blob | The compact, codegen-emitted descriptor of a generated type's fields and tags, queryable in tools and editor builds via the registry. **Not** present-as-runtime-reflection in shipping builds; it is data the codegen wrote, not introspection. |
| Structural Evolution | Forward-compatible schema change (append field, deprecate field, field-rename alias, type-rename alias) that Flatbuffers handles automatically via declared defaults and zero-copy reads. No per-step migration functions; `flatc --conform` verifies structural compatibility (Q1, ADR #1121). |
| SchemaAliasTable | The in-process table holding type-rename aliases (old-FQN → new-FQN forward-map) and field-rename alias accessor records for all registered types. Populated at static-init via `glibre_types_register_fqn_alias`; read-only thereafter. Replaces MigrationChain (Q1). |
| Envelope | A Flatbuffers size-prefixed buffer with a 4-byte `file_identifier` at offset 4 and `schema_version`/`flags` fields inside the root table (Q4, ADR #1121). The `EnvelopeHeader` struct is DELETED; framing is native Flatbuffers. |
| Persistent Aggregate | A type whose instances cross either a save boundary, a hot-reload boundary, or a plugin-dylib boundary. Anything persistent has a schema; anything not persistent does not. |
| FQN | Fully-qualified name of a generated type, e.g. `glibre.core.Transform`. The schema's primary identity in the registry and on the wire. |
| Hot-Reload Barrier | The frame-8 boundary at which the loader drains the world, swaps plugin dylibs, runs `reconcile(...)` to verify schema-set continuity and refresh the alias table, and either resumes or rejects the swap (PHILOSOPHY §8). Structural evolution (absent fields → Flatbuffers defaults) is automatic; no per-step migration dispatch occurs. |

## 3. Derived From

Harmonius prior art is treated as research input only; every conclusion is
re-derived against PHILOSOPHY (`/Users/cjhowe/Code/glibre/PHILOSOPHY.md`)
and the engine-wide decision in
`reviews/decisions/flatbuffers-codegen.md`. The harmonius `data-systems/` corpus
is not requirements truth for the glibre `data` context — most of what it
calls "data systems" is *domain* matter that the glibre `data` context
explicitly **refuses** (see refusals below). What survives the re-derivation
is the single cross-cutting concern that every harmonius data-system shares:
*every persistent type wants a schema, a serializer, and a forward-migration
story*. That concern collapses into glibre's Flatbuffers codegen pipeline,
producing the `glibre-types.dylib` middleman and the ABI-hash gate
(§1, §2; `reviews/decisions/flatbuffers-codegen.md`).

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
  Flatbuffers codegen flow whose schema files live at
  `data/schemas/<ctx>/<Type>.fbs`, whose generated types compile into the
  single `glibre-types.dylib` middleman, and whose hot-reload migrations
  run at the frame-8 barrier (`reviews/decisions/flatbuffers-codegen.md` §Pipeline,
  §Migration Mechanic). The glibre rationale is PHILOSOPHY §6 (zero
  runtime reflection in shipping builds), §7 (deterministic byte-equal
  snapshots), §9 (ABI-hash refusal at plugin load), and §10 (one collapsed
  primitive per Occam pass).

The Occam collapse, stated as a single sentence:
> N domain-specific harmonius `rkyv` archive derivations → 1 glibre
> Flatbuffers-codegen pipeline producing 1 middleman dylib gated by 1 ABI hash.

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
  may wrap Flatbuffers blobs, but no transport policy lives here (per §1).
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

The `data` context decomposes into nine SRP-bounded aggregates. Each owns
exactly one reason to change; together they form the type-and-schema spine
described in §1. Aggregates are listed in dependency order: each consumes
only what precedes it. Invariants are stated as predicates that hold at
every public API boundary; violation of any predicate is a defect, not a
runtime branch.

### 4.1 `Schema` (authoring aggregate)

**Owns:** the in-memory representation of one `data/schemas/<ctx>/<Type>.fbs`
file — its `FQN`, monotonically-increasing `SchemaVersion`, ordered set of
`Tag`s (each with field name, builtin/generated type reference, `since`
version, optional default), and the set of reserved `Tag`s. Equivalence
class: one `Schema` per `(FQN, SchemaVersion)`.

**Reason to change:** the `.fbs` file format itself — adding a builtin,
introducing default-value syntax, or changing the canonicalization rule
that feeds the source hash.

**Invariants** (every public API boundary):

1. `FQN` matches `[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+\.[A-Z][A-Za-z0-9]*`
   (lowercased dotted-context path, PascalCase type leaf). No two schemas
   in the workspace share an `FQN`.
2. `SchemaVersion` is a `std::uint32_t`, strictly positive, monotonic
   across the schema's history (no gaps, no decreases). Bumped exactly
   when the field set is added/removed/renamed.
3. Active and reserved `Tag`s share one number space; every tag number is
   unique and immutable once shipped. Removing a field migrates its tag
   number into the reserved set; codegen rejects any subsequent reuse.
4. Every active tag's type is either a `Builtin` from
   `glibre/types/_builtins.hpp` or another `Generated Type` whose `Schema`
   is registrable in the same build. No cycles in the type graph.
5. `since` on each tag is `≤ SchemaVersion`. Any tag with `since == N`
   that is absent from a `version < N` payload is materialized via its
   declared default; missing default on a non-`option<T>` tag is a
   codegen error.
6. The canonicalized byte sequence of the `Schema` (sorted tags, normalized
   whitespace, defaults serialized in tag order) is the only input to its
   `Schema Source Hash` — defined in §4.4 — making the hash a pure function
   of the schema's semantic content.

### 4.2 `Sergeant` (codegen-tool aggregate)

**Owns:** the host-only `glbr-sergeant` executable: parsing `.fbs` files
into `Schema` values, validating each `Schema` against the invariants in
§4.1, emitting tag-sorted `Generated Type` headers/sources, the
`_registry.cpp` and `_abi_hash.cpp` companions, and a stamp file the
CMake graph depends on. `Sergeant` is the only writer of generated middleman
sources.

**Reason to change:** the codegen output format — generated struct shape,
the registry table layout, the dispatcher emission strategy. The schema
language and the hash function are owned elsewhere (§4.1, §4.4).

**Invariants:**

1. Output is a pure function of the input schema set: identical input
   bytes plus identical `glbr-sergeant` binary produce identical generated
   sources, byte-equal across hosts (PHILOSOPHY §7).
2. Every `Generated Type` is `final`, vtable-free, has `= default` ctors,
   contains only builtins or other generated types, and has its fields
   ordered by ascending `Tag` (not declaration order). `Sergeant` asserts
   `std::is_trivially_copyable_v<T>` whenever the field set permits and
   fails the build otherwise on schemas that claim that profile.
3. Layout-additive rule: a new tag is permitted only if its sorted
   position appends past the offset of the prior version's last field;
   any other change forces a `SchemaVersion` bump and a registered
   `Migration`. `Sergeant` proves this property at codegen time and refuses
   to emit on violation.
4. Reserved-tag enforcement is machine-checked: comparing the in-tree
   `Schema` against its prior committed version, `Sergeant` errors on any
   reuse of a number ever shipped.
5. `Sergeant` never opens the network, never reads paths outside the
   declared schema root and the configured output directory, and
   allocates only inside an arena it owns. Determinism does not depend
   on filesystem iteration order — schema files are sorted by `FQN` before
   processing.

### 4.3 `Middleman` (`glibre-types.dylib` aggregate)

**Owns:** the single shared library that holds every `Generated Type`,
the populated `SchemaRegistry`, the migration dispatcher, the embedded
`AbiHash`, and the C-stable entry-point table. The middleman is the
*only* ABI surface plugins link; no plugin links Flatbuffers directly.

**Reason to change:** the dylib's binary contract — exported symbol set,
SONAME bumps, link-time inclusion of new generated TUs. Per-type
serialization mechanics live inside generated code; per-type semantics
live in their owning contexts.

**Invariants:**

1. Every plugin dylib and every host binary (runtime, editor, tools) that
   exchanges persistent bytes links exactly one `glibre-types.dylib`. A
   process that loads two distinct middleman builds is undefined; the
   plugin loader refuses to bring up such a process at startup.
2. Exported C entry points are limited to:
   `glibre_types_abi_hash`, `glibre_types_serialize_<fqn>`,
   `glibre_types_deserialize_<fqn>`,
   `glibre_types_register_migration`,
   `glibre_types_last_register_error`. Their signatures are stable
   across patch releases; adding a new `<fqn>` is additive and does not
   bump SONAME.
3. SONAME bumps only on ABI-breaking schema changes — those that violate
   the layout-additive rule (§4.2) or remove an exported entry point.
   Additive schema changes keep SONAME and bump only the embedded
   `AbiHash`; the loader catches mismatches via the hash check (§4.4).
4. The middleman exposes no C++ class templates across the dylib
   boundary: thin wrapper templates exist in headers but resolve to
   `extern "C"` symbols at link time, making the boundary C-ABI-safe by
   construction.
5. Static-initialization order inside the middleman is fixed: builtins
   register before generated types; generated types register before
   migrations are wired. The dispatcher table is read-only after
   `main()` begins.

### 4.4 `AbiHash` (gate-value aggregate)

**Owns:** the single Blake3 scalar `glibre_types_abi_hash` exported by
the middleman, plus the construction rule that produces it.

**Reason to change:** the hash's construction algorithm itself — switching
hash function, changing the canonicalization of the input vector.

**Invariants:**

1. `AbiHash := blake3( join( "\n", sort_by_fqn( { fqn_utf8(s) || ":" ||
   version_le(s) || ":" || schema_source_hash(s) : s ∈ Schemas } ) ) )`.
   Sort key is the canonical Unicode code-point order of `FQN`;
   entries are separated by a single LF byte (`\n`); no trailing
   newline. `version_le` is the declared `version` integer as 4
   bytes little-endian; `schema_source_hash` is Blake3-256 of the
   **`.bfbs` binary schema bytes** for that type (Q7, ADR #1121 —
   NOT the `.fbs` source text; the canonicalization module is deleted).
   The outer blake3 result is a 32-byte digest. Normative source:
   `reviews/decisions/plugin-abi.md` §"ABI Hash Function" rule 1.
   Outer ABI hash construction (Blake3 over LF-joined, FQN-sorted
   entries) is UNCHANGED.
2. `schema_source_hash(s) := blake3( bfbs_bytes(s) )` where
   `bfbs_bytes(s)` is the binary `.bfbs` output emitted by
   `flatc --bfbs` for schema `s`. The `.bfbs` format is canonical by
   construction — `flatc` produces deterministic output for identical
   `.fbs` input (PHILOSOPHY §7). The hand-rolled canonicalization
   module (§7.3) is DELETED; `.bfbs` bytes replace it as the hash
   input (Q7, ADR #1121).
3. The plugin loader compares its host's compiled-in `AbiHash` against
   each loaded plugin's compiled-in value byte-for-byte. Mismatch →
   `core::Error::PluginAbiHashMismatch` and refusal to load (PHILOSOPHY
   §9; `reviews/decisions/error-model.md`).
4. The hash is a build-time constant: no runtime computation, no
   dependence on linker order beyond the canonicalization rule above.
   Two builds from identical schema bytes on different hosts produce the
   same `AbiHash` (PHILOSOPHY §7).
5. The hash carries no semantic meaning beyond identity; it is not a
   version number and is not human-readable. Logging uses its hex form
   alongside the SONAME.

### 4.5 `SchemaRegistry` (lookup aggregate)

**Owns:** the static, immutable table inside the middleman mapping each
`FQN` to `(SchemaVersion, schema_source_hash, serialize_fn,
deserialize_fn, ReflectionBlob)`. Populated at static-init
time by codegen-emitted register calls; no entries are added or removed
at runtime. FQN aliasing is held in the companion `SchemaAliasTable` (§4.7),
not in this table.

**Reason to change:** the per-entry record shape — adding a new
codegen-emitted field, removing one, or changing how lookup is keyed.

**Invariants:**

1. `FQN` uniquely identifies a registry entry; insertion of a duplicate
   `FQN` fails static-init with `data::Error::SchemaRegistryConflict`.
   Two schemas may share a `schema_source_hash` only if their bytes are
   byte-equal, in which case codegen must collapse them upstream.
2. `serialize_fn` and `deserialize_fn` are non-null function pointers
   for every entry; codegen guarantees their existence by emitting them
   alongside each generated type.
3. Lookup by `FQN` is `O(log N)` over a sorted array (binary search);
   the chosen layout is contiguous, cache-friendly, and free of any
   allocation. Iteration order is `FQN`-sorted, matching the canonical
   order used by `AbiHash`.
4. The registry is read-only after static-init: the dispatcher holds it
   by `const&` and never publishes a mutating reference. Hot-reload
   loads a new middleman instance rather than mutating the live one.
5. Every persistent type in the engine has exactly one registry entry;
   conversely, no entry exists without a corresponding `Schema` source
   file under `data/schemas/`. The CMake glob and the registry are
   reconciled by `Sergeant` at configure time.

### 4.6 `SchemaEvolution` (structural evolution aggregate)

**Owns:** the structural evolution rules that allow a schema to grow
without per-version migration functions. Evolution is enforced by
`flatc --conform <old.fbs>` at build time and by two alias mechanisms
at decode time.

**Q1 (ADR #1121): Per-version `migrate_T_vN_to_vN+1` functions are
DROPPED.** Structural evolution only; semantic changes go to cook-time
transforms under `glibre-cook`, not runtime migrations.

**Two alias mechanisms:**

1. **Field-rename aliases.** The old field is deprecated at its
   original tag (annotated `deprecated` in the `.fbs` file); the new
   field is added at the next free tag. `glbr-sergeant` emits both
   accessors: the new name is canonical; the old name is a
   `[[deprecated]]` thin forwarder aliasing the new accessor. No
   data migration; the wire bytes are structurally compatible.

2. **Type-rename aliases.** The `SchemaRegistry` holds a forward-map
   of `old-FQN → new-FQN`. When the loader encounters an envelope
   bearing the old FQN, it resolves transparently to the new FQN's
   registry entry before dispatch. No wire change; the alias lives
   entirely inside the registry's lookup table.

**Reason to change:** the alias mechanisms themselves — adding a new
category of structural evolution that `flatc --conform` cannot check,
or changing how the SchemaRegistry forward-map is populated.

**Invariants:**

1. `flatc --conform <old.fbs> <new.fbs>` passes for every pair of
   consecutive schema versions shipped in the same build. A schema
   change that fails `--conform` is not a structural evolution; it is
   a semantic change that goes to cook-time (`glibre-cook`).
2. Field-rename aliases preserve tag immutability (§4.1 inv. 3): the
   old tag number is `deprecated`, not `reserved`. No tag is ever
   reused; the deprecated tag slot remains forever in the tag space.
3. Type-rename aliases are one-directional: `old-FQN → new-FQN`. The
   registry never holds cycles; the loader resolves at most one hop.
4. Cook-time transforms (owned by `glibre-cook`, NOT `glbr-sergeant`)
   handle semantic changes: unit conversion, FK rebind, derived-field
   invalidation. They run once at content cook time against `.bfbs`
   produced by sergeant, producing a stable Flatbuffers buffer the
   runtime reads zero-copy. `glbr-sergeant` owns schema validation and
   `.fbs` → `.bfbs` / `.hpp` emission only; it never invokes
   `glibre-cook`'s transform logic.
5. A structural evolution that reduces field count (field removal via
   `deprecated`) is the only "removal" the schema language permits.
   Removing an entire type (FQN) from the registry is a semantic
   change requiring a cook-time migration and a type-rename alias that
   maps old-FQN to a tombstone entry.

### 4.7 `SchemaAliasTable` (alias-resolution aggregate)

**Owns:** the per-registry forward-maps that resolve deprecated names
to their canonical successors without per-version migration functions.
Two tables: (a) the FQN forward-map (type-rename aliases, §4.6 alias
mechanism 2) and (b) the per-FQN deprecated-field accessor table
(field-rename aliases, §4.6 alias mechanism 1).

**Q1 (ADR #1121): replaces MigrationChain.** There is no per-step
`MigrationFn` dispatch; version gaps are handled at decode time by
Flatbuffers' built-in field-presence semantics (absent fields take the
declared default), with the alias tables providing the name-compatibility
layer on top.

**Reason to change:** the alias-lookup protocol — how the FQN
forward-map is built, how field-rename forwarders are emitted, how
the loader resolves a deprecated FQN in a single hop.

**Invariants:**

1. The FQN forward-map is acyclic (no `A → B → A` chains). The loader
   detects cycles at static-init and raises `data::Error::SchemaRegistryConflict`.
2. Every entry in the FQN forward-map resolves to a live `SchemaRegistry`
   entry in one hop. Multi-hop chains are flattened by `glbr-sergeant`
   at codegen time.
3. Field-rename alias accessors are emitted as `[[deprecated]]`
   forwarders by `glbr-sergeant` into the generated header. They
   compile to zero overhead (inline alias, same underlying tag read).
4. The alias table is read-only after static-init: populated by
   codegen-emitted calls during the middleman's static-init sequence,
   never mutated at runtime.
5. Structural evolution only: the alias table has no arena, no
   `MigrationFn` pointers, and no chain composition. Any change
   that cannot be expressed as append-or-deprecate-or-alias goes to
   `glibre-cook` at cook time (§4.6 inv. 4).

### 4.8 `Envelope` (wire-form aggregate)

**Q4 (ADR #1121):** The `EnvelopeHeader { fqn, version, payload_length, flags }`
struct is DELETED as a separate wire envelope. Replaced by Flatbuffers
native wire framing:

- **`file_identifier`** — 4-byte magic at offset 4 of every
  size-prefixed buffer. Names the root table type (identifies the FQN
  family). Set in the `.fbs` root declaration; `glbr-sergeant` enforces
  one identifier per schema file.
- **Size-prefix** — 4-byte little-endian buffer length at offset 0.
  Flatbuffers' standard size-prefixed buffer layout; readers call
  `flatbuffers::GetSizePrefixedRoot<T>()`.
- **`schema_version: uint32`** — lives INSIDE the root table at
  field-id 2. Carries the `SchemaVersion` without a separate header.
- **`flags: uint32`** — lives INSIDE the root table at field-id 3.
  Reserved for future use; current builds emit 0. New flags append at
  higher field IDs without invalidating the wire format (structural
  evolution per §4.6).

**Owns:** the Flatbuffers native size-prefix + `file_identifier` framing
for every persistent payload. Read by every `deserialize_<fqn>` to
dispatch decode; written by every `serialize_<fqn>` via the
`FlatBufferBuilder` size-prefix API.

**Reason to change:** the wire-level framing conventions — changing which
fields live inside the root table, adding a flag bit (appends at next
free field-id), or adopting a different Flatbuffers framing convention.

**Invariants:**

1. Self-describing: the `file_identifier` magic (4 bytes) plus the
   root table's `schema_version` field are sufficient to identify the
   type and version of any persistent byte sequence in the engine.
   No external schema discovery is required at deserialize time.
2. Size-prefixed layout: every buffer is size-prefixed per the
   Flatbuffers spec. Readers peek the 4-byte size, then validate the
   remainder with `flatbuffers::Verifier`. Truncation before the
   4-byte size yields `data::Error::EnvelopeTruncated`.
3. Byte order: little-endian for all integer fields (Flatbuffers
   invariant). Fixed across all hosts (PHILOSOPHY §7).
4. Round-trip identity: for every `T` and every legal value `t : T`,
   `deserialize<T>(serialize<T>(t)) == t` byte-for-byte after
   re-serialization. Tested per-schema via Catch2 goldens under
   `tests/data/schemas/<ctx>/<Type>.cpp`.
5. Version-driven dispatch: `deserialize` reads the `file_identifier`
   (type identity), looks up the registry entry by FQN, reads
   `schema_version` from the root table, compares to the entry's
   current version, and either decodes directly (structural evolution:
   absent fields take their declared defaults) or resolves via alias
   tables (§4.7). The `file_identifier` + `schema_version` pair is
   the single source of dispatch truth — no out-of-band hints.

### 4.9 `ReflectionBlob` (introspection aggregate)

**Q8 (ADR #1121):** `ReflectionBlob` is `std::span<const std::byte>`
over the per-type `.bfbs` (binary Flatbuffers schema) slice emitted by
`glbr-sergeant --bfbs`. The hand-rolled descriptor emitter is DELETED.
Editor-mode consumers use `flatbuffers::reflection::Schema` from upstream
to interpret the `.bfbs` bytes; no custom descriptor struct exists.

**Owns:** a `std::span<const std::byte>` borrow into the per-context
`.bfbs` sidecar bytes (§6, §8 hot-reload contract, §10 Q10). The
registry entry holds this span in editor / tools builds; the span is
null-width in shipping builds (PHILOSOPHY §6).

**Reason to change:** the `.bfbs` slice boundary — which bytes constitute
one type's schema blob vs. the combined context schema.

**Invariants:**

1. Static-only: the blob is a read-only view into the `.bfbs` bytes
   emitted by `glbr-sergeant` at build time, not a runtime reflection
   facility. Shipping builds carry no `.bfbs` bytes; the span is
   empty (PHILOSOPHY §6, §4.9 inv. 5 below).
2. Read-only: the underlying bytes are `const`; no API mutates them.
   Editor edits to a `.fbs` file re-run `glbr-sergeant`, which
   regenerates the `.bfbs` sidecar; the live blob is never patched in place.
3. Faithfulness: the `.bfbs` bytes are a faithful binary encoding of the
   `.fbs` schema as parsed by `flatc --bfbs`. Every active field and
   deprecated field in the source schema is represented; `flatbuffers::
   reflection::Schema` exposes the full field set to editor consumers.
4. Self-contained: the `.bfbs` sidecar for a context embeds all type
   definitions reachable from the context's root schemas. Editor
   introspection does not re-load `.fbs` source files; the sidecar
   is the single artifact it reads.
5. Stripped optionality: `.bfbs` sidecars are present only in editor /
   tools builds (§6 internal architecture, §8 hot-reload contract,
   Q10). In shipping builds the registry entry holds an empty span
   and editor-only callers handle the empty span explicitly; the
   runtime hot path never queries the slot.

### 4.10 Cross-aggregate invariants

These hold across the spine and are the load-bearing guarantees the data
context promises every consumer:

1. **Persistence ⇔ Schema:** every type whose instances cross a save,
   hot-reload, or plugin-dylib boundary has exactly one `Schema` and
   exactly one `SchemaRegistry` entry; conversely, every registry
   entry corresponds to a real `.fbs` file in `data/schemas/`. The
   biconditional is enforced at configure time by `Sergeant` and at
   build time by the registry's static-init pass (§4.5).
2. **Monotonic versions:** a type's `SchemaVersion` is strictly
   monotonic across its history; older versions never reappear. A
   payload tagged with a version greater than the registry's current
   version yields `data::Error::DeserializeError` (newer-than-host
   payload), never silent truncation.
3. **AbiHash is the only ABI surface:** a plugin's compiled-in
   `AbiHash` matches the host's iff the plugin links the same
   middleman build. Mismatch triggers refusal with
   `core::Error::PluginAbiHashMismatch`; the previous-good plugin
   keeps running (`reviews/decisions/error-model.md`).
4. **Single dylib boundary:** plugins link `glibre-types.dylib` and
   consume Flatbuffers-generated accessor types at the plugin ABI surface
   (Q6, ADR #1121 — PHILOSOPHY §11 amended). Plugins do not link
   `blake3` directly and do not include any hand-written header except
   via `glibre-types.dylib`. Flatbuffers-generated types (`flatbuffers::
   Offset<T>`, `FlatBufferBuilder`, table-accessor pointers) satisfy
   the offset-stable-ABI invariant: offset-table layout is part of the
   Flatbuffers binary format spec, host-invariant by construction.
   `std::*` / `std::pmr::*` containers remain prohibited at the dylib
   boundary (PHILOSOPHY §11 Clause A/B split).
5. **Structural evolution coverage:** for every `FQN` whose current
   version is `M ≥ 2`, all field additions since version 1 are
   append-only and `flatc --conform` passes for every consecutive
   version pair (§4.6 inv. 1). The SchemaAliasTable (§4.7) holds
   forward-maps for any field or type renames. Builds that fail
   `--conform` fail at codegen, never at runtime.
6. **Determinism inheritance:** the spine adds no nondeterminism over
   its inputs — `Sergeant` is deterministic, `AbiHash` is deterministic
   (hashes `.bfbs` bytes, Q7), `serialize`/`deserialize` are
   deterministic, `Envelope` framing is Flatbuffers-native (Q4).
   Snapshots round-trip byte-equal across hosts (PHILOSOPHY §7).
7. **No cross-aggregate mutation:** each aggregate is mutated only by
   its owner. Domain plugins contribute field-rename or type-rename
   aliases through codegen-emitted registrations but never edit the
   registry, the alias table, the envelope framing, the abi-hash, or
   the reflection blob. The data context owns plumbing; domain contexts
   own their type's bytes.

## 5. Public Interface

**Q6 (ADR #1121 — PHILOSOPHY §11 amended):** The plugin ABI surface now
carries Flatbuffers-generated accessor types (`flatbuffers::Offset<T>`,
`FlatBufferBuilder`, table-accessor pointers) in addition to POD spans /
handles. This is consistent with PHILOSOPHY §11's permissive clause: the
underlying invariant is host-stable offset layout, which Flatbuffers
satisfies via its binary format specification (not via C++ ABI flags).
The `std::*` / `std::pmr::*` container prohibition at the dylib boundary
is UNCHANGED (Clause A borrow semantics remain the default read path;
Clause B opt-in ownership transfer applies for cross-frame retained
buffers — see PHILOSOPHY §11 for the full two-clause split).

The public interface of the `data` context is the surface exported by the
`glibre-types.dylib` middleman (§4.3) plus the per-type generated headers
emitted by `glbr-sergeant` (§4.2). Every plugin and every host binary that
exchanges persistent bytes consumes only what is declared below; nothing
in the data context is reachable except through the middleman.

The header stub that follows compiles under
`clang++ -std=c++23 -fsyntax-only -fno-exceptions` against libc++ on
macOS (the `-fno-exceptions` flag is the engine-wide default per
`reviews/decisions/error-model.md` §Decision #3). It is intentionally
declaration-only: the registry, ABI hash, and per-type trampolines are
populated by codegen-emitted definitions inside the middleman and by
generated `<glibre/types/<ctx>/<Type>.hpp>` headers that this stub does
not enumerate.

The `glibre::Error` type, `glibre::Result<T>` alias, and `glibre::core`
error enum are owned by `core` and live in `<glibre/error.hpp>` per
`reviews/decisions/error-model.md`. The stub forward-declares them as
the ambient context this header is consumed in; the data context does
not redefine them.

```cpp
// glibre-types.dylib public surface (header stub).
//
// Real headers split into:
//   include/glibre/types/identity.hpp      — SchemaId / SchemaVersion / hash
//   include/glibre/types/error.hpp         — data::Error closed sum
//   include/glibre/types/envelope.hpp      — Envelope<T> (FB size-prefix wrapper)
//   include/glibre/types/alias_table.hpp   — SchemaAliasTable, FQN forward-map
//   include/glibre/types/registry.hpp      — RegistryEntry, SchemaRegistry
//   include/glibre/types/abi_hash.hpp      — glibre_types_abi_hash()
//   include/glibre/types/plugin_manifest.hpp — PluginManifest + sub-types
//   include/glibre/types/reflection.hpp    — ReflectionBlob (editor-only, .bfbs span)
//
// The stub below is the union, with section banners matching the
// per-file split. § references point at this spec.
//
// Q6 (PHILOSOPHY §11 amended): Flatbuffers-generated accessor types appear
// at the plugin ABI surface. std::* / std::pmr::* containers remain
// prohibited (Clause A borrow / Clause B opt-in ownership still apply).

#pragma once

#include <flatbuffers/flatbuffers.h>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

// glibre::Error and glibre::Result<T> live in <glibre/error.hpp>; only
// forward-declared here so this stub stays self-contained.
namespace glibre {
class Error;
template <class T> using Result = std::expected<T, Error>;
}  // namespace glibre

namespace glibre::types {

// ---- identity.hpp -------------------------------------------------------

// FQN string view, lowercased dotted-context path + PascalCase leaf,
// e.g. "glibre.core.Transform". Storage is owned by the registry's
// per-type interning table (§4.5 inv. 3); SchemaId itself is a borrow.
// Per §4.1 inv. 1.
struct SchemaId {
    std::string_view fqn{};

    constexpr bool operator==(const SchemaId&) const noexcept = default;
    constexpr auto operator<=>(const SchemaId&) const noexcept = default;
};

// Monotonically increasing, strictly positive version (§4.1 inv. 2).
using SchemaVersion = std::uint32_t;

// Blake3-256 of the .bfbs binary schema bytes for a single type (§4.4 inv. 2,
// Q7 ADR #1121). Input is the per-type .bfbs output from flatc --bfbs.
using SchemaSourceHash = std::array<std::byte, 32>;

// ---- error.hpp ----------------------------------------------------------

// Closed sum of every failure the data context can raise at a public
// boundary (§10). Per error-model.md §"Composition Rules" #2 callers
// translate these into their own context's enum at the call site.
// §5 uses the same ErrorTag + Error struct shape defined in §10.1.
// The 5-arm `enum class Error` stub that previously appeared here was
// a draft; it is superseded by the 10-arm closed sum below. See §10.1
// for the normative definition with full payload fields and per-arm
// trigger / recovery / severity documentation.
namespace data {

// Wire-time location attached to DeserializeError / SchemaUnknown /
// EnvelopeTruncated.
// `offset` is the byte index within the inbound payload at which
// decoding stopped, measured from the start of the size-prefixed buffer
// (§4.8 inv. 2). Non-payload arms set this to a sentinel — see §10.2.
struct WireSite {
    SchemaId      schema{};       // the FQN the file_identifier claimed
    SchemaVersion version{0};     // the schema_version field value
    std::uint32_t offset{0};      // byte index where decode failed
};

// Tag values are stable across patch releases (§10 closing paragraph).
// Removing or reordering an arm is an ABI break (§4.3 inv. 3).
enum class ErrorTag : std::uint16_t {
    AbiHashMismatch          = 1,  // §4.4 inv. 3 — hash compared at plugin load
    SchemaMigrationFailure   = 2,  // §4.7 inv. 4 — chain step returned unexpected
    DeserializeError         = 3,  // §4.8 inv. 5 — newer-than-host or malformed
    ReservedTagViolation     = 4,  // §4.1 inv. 3, §4.2 inv. 4 — codegen-time
    SchemaRegistryConflict   = 5,  // §4.5 inv. 1 — duplicate FQN at static-init
    SchemaUnknown            = 6,  // FQN not in live registry at deserialize
    MigrationStepMissing     = 7,  // coverage gap: no chain entry for inbound version
    MigrationCycle           = 8,  // back-edge detected in migration chain
    EnvelopeTruncated        = 9,  // physical truncation before/during envelope read
};

// Closed sum. Plain-aggregate layout so the C-ABI trampolines (§4.3 inv. 4)
// can return it through std::expected without crossing a non-trivial type
// boundary. See §10.1 for full payload-field documentation and §10.2 for
// the per-arm trigger / recovery / severity / core::Error mapping table.
struct Error {
    ErrorTag tag{};

    // Set on tags 3, 6, 9; default-constructed on every other arm.
    WireSite at{};

    // Set on tags 2, 7, 8, and 10: identifies the migration step that
    // refused, the missing step in the chain, the back-edge of the
    // detected cycle (tags 2/7/8), or the drifted FQN (tag 10).
    // For tag 10 (SourceHashMismatch): step_schema = the drifted FQN;
    // step_from == step_to == 0 (no version change, only hash changed).
    // (from, to) are non-zero on arms 2, 7, 8; (from==0, to==0) on arm 10
    // only (no version change, only hash changed).
    SchemaId      step_schema{};
    SchemaVersion step_from{0};
    SchemaVersion step_to{0};

    // Set on tags 1 and 10: the host's compiled-in hash and the
    // offending plugin's compiled-in hash, hex form. For tag 1
    // (AbiHashMismatch) these are the global glibre_types_abi_hash
    // values (§4.4 inv. 5). For tag 10 (SourceHashMismatch) these are
    // the per-FQN SchemaSourceHash values (hex-encoded, §3.2 of
    // schema-registry-design.md). Both borrow from glibre-types.dylib
    // .rodata; lifetime is process-scoped. std::string_view (not
    // eastl::string_view) per PHILOSOPHY §11 final sentence: public
    // plugin ABI surfaces use POD views only; std::string_view is stable
    // across the dylib boundary because both sides compile against the
    // same libc++ (reviews/decisions/plugin-abi.md §"Registration
    // Entry-Point Signature"). Empty string_views on every other arm.
    std::string_view host_hash{};
    std::string_view plugin_hash{};

    // Set on tag 4 (ReservedTagViolation): the tag number whose
    // reuse was attempted; 0 on every other arm.
    std::uint16_t reserved_tag{0};
};

}  // namespace data

// ---- reflection.hpp (editor / tools only) -------------------------------

// Q8 (ADR #1121): ReflectionBlob is std::span<const std::byte> over the
// per-type .bfbs binary schema bytes emitted by glbr-sergeant --bfbs.
// The hand-rolled ReflectionField / ReflectionKind descriptor structs are
// DELETED. Editor consumers use flatbuffers::reflection::Schema from
// upstream to interpret the bytes (§4.9).
//
// Stripped to empty span in shipping builds (§4.9 inv. 5, PHILOSOPHY §6).
using ReflectionBlob = std::span<const std::byte>;  // .bfbs slice; empty in ship

// ---- envelope.hpp -------------------------------------------------------

// Q4 (ADR #1121): EnvelopeHeader { fqn, version, payload_length, flags }
// is DELETED as a separate wire envelope. Wire framing is now Flatbuffers
// native size-prefix + file_identifier (§4.8). The fields migrate inside
// the root table:
//   schema_version : uint32 at field-id 2
//   flags          : uint32 at field-id 3
//
// Typed wrappers around the per-FQN extern "C" trampolines emitted by
// codegen (§4.3 inv. 4). Specializations live in each generated
// <glibre/types/<ctx>/<Type>.hpp>; this primary template is left
// undefined so misuse is a link-time error rather than a runtime one.
template <class T>
struct Envelope {
    // Build a size-prefixed Flatbuffers buffer from `value`.
    // The file_identifier and schema_version field are set automatically
    // by the generated serializer.
    static auto serialize(const T& value,
                          std::span<std::byte> dst) noexcept
        -> std::expected<std::size_t, data::Error>;

    // Read file_identifier, verify with flatbuffers::Verifier, dispatch
    // by schema_version field (§4.8 inv. 5). Newer-than-current ⇒
    // data::ErrorTag::DeserializeError; absent fields take declared
    // defaults (structural evolution, §4.6).
    static auto deserialize(std::span<const std::byte> src) noexcept
        -> std::expected<T, data::Error>;
};

// ---- alias_table.hpp ----------------------------------------------------

// Q1 (ADR #1121): replaces migration.hpp. Per-registry alias tables
// for field-rename and type-rename schema evolution (§4.7).

// Type-rename alias entry: maps a deprecated FQN to its successor.
// Populated at static-init by glbr-sergeant-emitted registration calls.
struct FqnAliasEntry {
    std::string_view old_fqn{};  // deprecated FQN as it appears on the wire
    std::string_view new_fqn{};  // canonical successor FQN in the live registry
};

// Plain-enum return so the C-ABI boundary stays free of std::expected.
namespace data {
enum class RegisterStatus : std::uint16_t {
    Ok = 0,
    SchemaRegistryConflict =
        static_cast<std::uint16_t>(ErrorTag::SchemaRegistryConflict),
    AliasCycle = 11,  // FQN forward-map would create a cycle (§4.7 inv. 1)
};
}  // namespace data

// Codegen-emitted entry-point for type-rename alias registration.
// Called at static-init by glbr-sergeant-emitted code (§4.7 inv. 4).
extern "C" auto glibre_types_register_fqn_alias(
    FqnAliasEntry entry) noexcept -> data::RegisterStatus;

// ---- registry.hpp -------------------------------------------------------

// Read-only entry produced by codegen and inserted at static-init
// (§4.5). Layout is fixed; new fields are appended only.
struct RegistryEntry {
    SchemaId         schema{};
    SchemaVersion    version{0};
    SchemaSourceHash source_hash{};  // Blake3 of .bfbs bytes (Q7)

    // Type-erased serialize/deserialize trampolines; the typed
    // Envelope<T> specializations resolve to these at link time
    // (§4.3 inv. 4).
    std::expected<std::size_t, data::Error> (*serialize)(
        const void* value, std::span<std::byte> dst) noexcept = nullptr;
    std::expected<void, data::Error> (*deserialize)(
        std::span<const std::byte> src,
        void* out_value) noexcept = nullptr;

    // Q8: ReflectionBlob is .bfbs bytes span (empty in shipping builds).
    ReflectionBlob reflection{};  // empty span in ship, .bfbs slice in editor
};

class SchemaRegistry {
public:
    // Binary search by FQN (§4.5 inv. 3). O(log N), allocation-free.
    // Returns unexpected(data::Error{ .tag = ErrorTag::SchemaUnknown })
    // if no entry matches; otherwise a non-null pointer into entries_.
    auto lookup(SchemaId schema) const noexcept
        -> std::expected<const RegistryEntry*, data::Error>;

    // FQN-sorted iteration order, matching AbiHash canonicalization
    // (§4.4 inv. 1, §4.5 inv. 3).
    auto entries() const noexcept -> std::span<const RegistryEntry>;

    // The single immutable instance lives inside glibre-types.dylib;
    // hot-reload publishes a *new* instance rather than mutating the
    // live one (§4.5 inv. 4, §4.10 inv. 7).
    static auto instance() noexcept -> const SchemaRegistry&;

private:
    SchemaRegistry() = default;
};

// ---- abi_hash.hpp -------------------------------------------------------

// 64-char lowercase hex blake3 string compiled into glibre-types.dylib
// at codegen time (§4.4). Plugins re-export the same value as
// `glibre_plugin_abi_hash`; the loader compares them byte-for-byte and
// refuses load on mismatch with `core::Error::PluginAbiHashMismatch`
// (PHILOSOPHY §9; reviews/decisions/plugin-abi.md §"Loader Sequence"
// step 4).
extern "C" auto glibre_types_abi_hash() noexcept -> const char*;

// ---- plugin_manifest.hpp ------------------------------------------------

// Wire-shape of `plugin.fbs`; codegen-serialized into the plugin's
// .rodata, deserialized by the loader before invoking any plugin C++
// code. Mirrors reviews/decisions/plugin-abi.md §"Plugin Manifest
// Schema" verbatim. PluginManifest itself is a Flatbuffers-versioned schema
// owned by `glibre.core`; this struct is the C++ projection.

struct SemVer {
    std::uint16_t major{0};
    std::uint16_t minor{0};
    std::uint16_t patch{0};
};

struct ComponentDecl {
    std::string_view fqn{};
    std::string_view schema_hash{};   // hex form of SchemaSourceHash
    std::uint8_t     storage_hint{0}; // archetype / sparse / singleton
};

struct SystemDecl {
    std::string_view                  name{};
    std::uint8_t                      phase{0};   // 1..=9, frame-phases
    std::span<const std::string_view> reads{};
    std::span<const std::string_view> writes{};
    std::span<const std::string_view> after{};
    std::span<const std::string_view> before{};
};

struct PassDecl {
    std::string_view                  name{};
    std::uint8_t                      render_phase{0};   // 6 or 7
    std::span<const std::string_view> inputs{};
    std::span<const std::string_view> outputs{};
};

struct PanelDecl {
    std::string_view id{};
    std::string_view title{};
    std::uint8_t     area{0};
};

struct PluginManifest {
    std::string_view                  name{};
    SemVer                            version{};
    std::string_view                  abi_hash{};
    SemVer                            min_engine_version{};
    std::span<const ComponentDecl>    components{};
    std::span<const SystemDecl>       systems{};
    std::span<const PassDecl>         passes{};
    std::span<const PanelDecl>        panels{};
    std::span<const std::string_view> depends_on{};
};

}  // namespace glibre::types
```

### 5.1 Events

The data context emits no events. Schema-version bumps are observable
only through the codegen-driven recomputation of
`glibre_types_abi_hash()`; the plugin loader translates that into a
`core::Error::PluginAbiHashMismatch` refusal at load time
(`reviews/decisions/plugin-abi.md` §"Loader Sequence" step 4) — that is
the data context's only externally-visible side effect.

### 5.2 Serialized schemas (Flatbuffers)

Persistent types are authored as `data/schemas/<ctx>/<Type>.fbs` files
(§4.1; Flatbuffers IDL format per `reviews/decisions/flatbuffers-vs-fory.md`).
The schemas the data context itself owns — referenced by the C++ structs
above — are:

- `data/schemas/core/PluginManifest.fbs` — the `PluginManifest` /
  `SemVer` / `ComponentDecl` / `SystemDecl` / `PassDecl` / `PanelDecl`
  bundle declared in `reviews/decisions/plugin-abi.md` §"Plugin
  Manifest Schema". `flatc --cpp` emits the Flatbuffers accessor types
  visible in §5; `glbr-sergeant` emits the per-plugin `manifest.cpp`
  blob (§6.5).

(No standalone envelope schema file exists; the wire framing is
Flatbuffers native size-prefix + `file_identifier` — see §4.8 / §7.2.4.
The `schema_version` and `flags` fields live inside each root table.)

Domain-owned schemas (`Transform`, `Mesh`, ...) live in their
originating context's directory under `data/schemas/<ctx>/`; the data
context only guarantees the round-trip.

### 5.3 Error types

The closed sum is `glibre::types::data::Error` declared above. Each arm
maps to one §4 invariant:

| Arm                       | Raised when                                                  | Origin                   |
|---------------------------|--------------------------------------------------------------|--------------------------|
| `AbiHashMismatch`         | plugin's compiled-in ABI hash ≠ host's                       | §4.4 inv. 3              |
| `SchemaMigrationFailure`  | schema-set continuity failure (FQN in outgoing missing from incoming); DEPRECATED for per-step MigrationFn use (Q1/Q5) | §8.2 step 1              |
| `DeserializeError`        | malformed envelope or newer-than-host version                | §4.8 inv. 5              |
| `ReservedTagViolation`    | codegen detects reuse of a previously-shipped tag            | §4.1 inv. 3, §4.2 inv. 4 |
| `SchemaRegistryConflict`  | static-init insert collides on FQN                           | §4.5 inv. 1              |
| `SchemaUnknown`           | envelope FQN absent from the live `SchemaRegistry`           | §4.8 inv. 5              |
| `MigrationStepMissing`    | inbound `schema_version` predates registered minimum (save file from old build); RETAINED per Q5 | §4.8 inv. 5 |
| `MigrationCycle`          | alias registration would produce a cycle in the FQN forward-map (codegen/init); RETAINED per Q5 | §4.7 inv. 2  |
| `EnvelopeTruncated`       | byte span too short for Flatbuffers size-prefix; KEEP per Q5  | §4.8 inv. 1, 2           |

Plugin loader code wraps these into `core::Error` arms per
`reviews/decisions/plugin-abi.md` §"Failure Modes → core::Error";
domain code wraps into its own context's `Error` per
`reviews/decisions/error-model.md` §"Composition Rules" #2.

Verification: the stub above compiles under
`clang++ -std=c++23 -fsyntax-only -fno-exceptions` on the toolchain
documented in `reviews/decisions/flatbuffers-codegen.md` §"Open Questions" #4
(libc++ as shipped with the macOS Xcode 15 / Homebrew-LLVM clang).

## 6. Internal Architecture

Non-binding sketch for implementers. Section §4 pins the *what* (the
nine SRP-bounded aggregates and their cross-aggregate invariants);
§5 pins the *contract surface* (the C++ headers exported by
`glibre-types.dylib`); this section sketches the *how* — the module
layout, the codegen pipeline shape, and the runtime data flow that
together produce the surface in §5 while preserving the invariants
in §4.

The split below is binding only insofar as the public-surface
guarantees in §5 require it; private internals (file names within a
module, function signatures that never cross a header boundary,
algorithmic choices inside a single TU) remain implementer's choice.
Where this section names a directory or symbol, that name is the one
the rest of the spec, the decision records, and the issue tracker
will use; no synonyms.

### 6.1 Module layout

The data context's implementation splits into three modules along the
single seam every aggregate already implies: code that runs on the
*build host* and produces sources, code that runs at *engine runtime*
and consumes those sources, and the *generated artifacts* themselves.
The split is mechanical: each module owns one phase of the pipeline,
each phase has one reason to change, and no module reaches across the
seam to mutate another's outputs.

#### 6.1.1 `tools/sergeant/` — host codegen tool (build-time only)

A standalone host executable, **not shipped at runtime**. Owns the
`Sergeant` aggregate (§4.2). It is a thin driver around `flatc`: it
invokes `flatc --cpp` for C++ header emission, `flatc --bfbs` for binary
schema emission, `flatc --conform` for structural-evolution checks, and
adds glibre-specific concerns: sidecar `.bfbs` emission, ABI hash
computation over `.bfbs` bytes (Q7), per-plugin `manifest.cpp`
generation, field-rename / type-rename alias enforcement, and FQN
forward-map maintenance.

**Q10 (ADR #1121):** `.bfbs` sidecar files are emitted by `glbr-sergeant`
next to the context dylib (NOT embedded in the middleman). They are
loaded only in editor mode. Shipping builds carry no `.bfbs` bytes.

Translation-unit shape (illustrative, not normative beyond the names
the rest of the spec already uses):

```
tools/sergeant/
  src/
    main.cpp                 # arg parsing, deterministic file walk;
                             # flatc driver wrapper; --conform runner
    alias_emitter.cpp        # field-rename / type-rename alias registration
                             # code emitted into _alias_table.cpp
    bfbs_hash.cpp            # blake3 over .bfbs bytes (Q7); ABI hash
    abi_hash_emitter.cpp     # generates _abi_hash.cpp
    manifest_emitter.cpp     # generates per-plugin manifest.cpp
    fqn_map.cpp              # FQN forward-map (type-rename aliases)
  CMakeLists.txt             # host-only; invokes flatc via find_program
  tests/                     # Catch2 unit tests for alias_emitter, etc.
```

`glbr-sergeant` invokes `flatc` as a subprocess (found via `find_program`
in CMake; installed by the Flatbuffers vcpkg port). It also links `blake3`
privately for ABI hash computation. Neither dependency leaks past this
module's boundary. The tool is the only writer of `.bfbs` sidecars and
the only reader of `.fbs` source files. It performs no network I/O,
opens no files outside the configured schema root and the configured
output directory, and is bit-deterministic across hosts (§4.2 inv. 1).

#### 6.1.2 `data/runtime/` — middleman runtime sources (in `glibre-types.dylib`)

The hand-written, **runtime-shipped** translation units that
`glibre-types.dylib` (§4.3) compiles into its binary alongside the
codegen-emitted sources from §6.1.3. These are the implementations
behind the §5 headers; they own the runtime mechanics that the
generated code calls into.

```
data/runtime/
  include/                   # public headers; mirror §5 file split
    glibre/types/
      identity.hpp
      error.hpp
      envelope.hpp           # Envelope<T> size-prefix wrapper (Q4)
      alias_table.hpp        # SchemaAliasTable, FQN forward-map (Q1)
      registry.hpp
      abi_hash.hpp
      plugin_manifest.hpp
      reflection.hpp         # ReflectionBlob = std::span<const std::byte> (Q8)
  src/
    schema_registry.cpp      # SchemaRegistry::instance, lookup,
                             # entries; static-init ordering (§4.3 inv. 5)
    envelope.cpp             # FB size-prefix + file_identifier read/write;
                             # Verifier call; peek schema_version (§4.8)
    alias_table.cpp          # SchemaAliasTable; FQN forward-map lookup;
                             # field-rename alias registration (§4.7)
    register_fqn_alias.cpp   # glibre_types_register_fqn_alias entry
    abi_hash.cpp             # glibre_types_abi_hash() trampoline that
                             # returns the codegen-embedded constant
    plugin_manifest.cpp      # PluginManifest deserialize helpers
                             # consumed by the core plugin loader
    static_init_check.cpp    # Phase C invariant assertions (§4.3 inv. 5,
                             # §4.5, §4.7); constructor priority 65535
  CMakeLists.txt             # contributes to the glibre-types target
  tests/                     # Catch2 unit tests for alias_table,
                             # registry, envelope
```

`data/runtime/` is the only module that may keep mutable state in
process memory, and even that state is restricted to (a) static-init
populated read-only structures. There is no per-payload arena (migrations
are dropped, Q1); the only scratch allocation is the `FlatBufferBuilder`
used by serialize, which is caller-owned. No I/O of any kind.

#### 6.1.3 `data/codegen-output/` — generated artifacts (build dir, not in repo source tree)

A **build-directory artifact** populated by `glbr-sergeant` at
configure / build time. Lives at
`${CMAKE_BINARY_DIR}/generated/glibre-types/` and is **not committed
to the repository**; the directory is regenerated from
`data/schemas/**/*.fbs` on every Ninja re-glob (per
`reviews/decisions/flatbuffers-codegen.md` §"CMake Integration"). The data
context's `.gitignore` suppresses any accidental check-in.

```
data/codegen-output/                 # (build dir; symbolic name only)
  include/glibre/types/<ctx>/
    <Type>.hpp                       # flatc --cpp output; one per .fbs;
                                     # field-rename alias [[deprecated]]
                                     # forwarders appended by sergeant
    <Type>_alias.hpp                 # GLIBRE_REGISTER_FQN_ALIAS macros
                                     # for type-rename aliases (Q1, §4.7)
  src/<ctx>/
    <Type>.cpp                       # serialize/deserialize bodies;
                                     # extern "C" trampolines per
                                     # §4.3 inv. 4
  src/_registry.cpp                  # static-init RegistryEntry
                                     # inserts; FQN-sorted (§4.5)
  src/_alias_table.cpp               # static-init FqnAliasEntry inserts
                                     # (Q1, §4.7; type-rename aliases)
  src/_abi_hash.cpp                  # const char* literal returned by
                                     # glibre_types_abi_hash() (§6.4)
  src/_manifest_<plugin>.cpp         # one TU per discovered
                                     # plugins/*/plugin.fbs (§6.5)
  bfbs/<ctx>.bfbs                    # Q10: sidecar .bfbs per context,
                                     # emitted next to context dylib;
                                     # editor-only; not in shipping build
  .stamp                             # CMake dependency stamp file
```

All sources under `data/codegen-output/` compile into
`glibre-types.dylib` alongside `data/runtime/`. No human ever edits a
file here; reproducibility flows from the rule that *identical
inputs produce byte-equal outputs* (§4.2 inv. 1) on every supported
host.

The three-module split is the §6.1 SRP collapse: build-host
codegen ↔ runtime engine ↔ generated bytes. Each owns one reason to
change and depends only on the modules upstream of it (§6.1.1 →
§6.1.3 → §6.1.2 ⇒ `glibre-types.dylib`).

### 6.2 Codegen pipeline

`glbr-sergeant` runs a five-stage pipeline over the schema set; the
stages are sequential, each stage's output is the input to the next,
and the pipeline is a pure function of the schema set plus the tool
binary and the `flatc` version it invokes (§4.2 inv. 1).

**Stage 1 — Conform check.** For every `.fbs` schema whose committed
prior version exists in the build tree, sergeant runs
`flatc --conform <prior.fbs> <new.fbs>`. Any schema change that fails
`--conform` is a semantic change, not a structural evolution; sergeant
exits non-zero and the build fails before emitting any output (§4.6 inv. 1).

**Stage 2 — C++ header emission.** Sergeant invokes `flatc --cpp`
(with `--cpp-fp scoped` for scoped enums) over the full `.fbs` set.
For each schema file, `flatc` writes `<Type>.hpp` into the codegen
output directory. Sergeant then appends field-rename alias forwarders
(annotated `[[deprecated]]`) for any field deprecations found in the
`.fbs` source (§4.6 alias mechanism 1; §4.7).

**Stage 3 — `.bfbs` emission.** Sergeant invokes `flatc --bfbs`
over the full `.fbs` set, writing per-context `<ctx>.bfbs` sidecar
files (Q10, ADR #1121). These sidecars are placed next to the context
dylib in the build output (not embedded in the middleman). Shipping
CMake profiles exclude the sidecars from the install target; editor
profiles include them (PHILOSOPHY §6).

**Stage 4 — ABI hash computation.** For each schema `s`, sergeant
computes `blake3(bfbs_bytes(s))` — the per-type `SchemaSourceHash`
(Q7, §4.4 inv. 2). The outer ABI hash is then
`blake3(join("\n", sort_by_fqn({fqn||":"+version_le||":"+source_hash : s ∈ schemas})))`
per §4.4 inv. 1. The result is hex-encoded and emitted into
`data/codegen-output/src/_abi_hash.cpp` as a `constexpr` literal (§6.4).

**Stage 5 — Registry and alias TUs.** From the validated schema set
sergeant writes:

1. **Registry TU.** `data/codegen-output/src/_registry.cpp` collects
   every per-type `RegistryEntry` into the static-init table
   (`SchemaRegistry::instance()`, §4.5). Entries are emitted in `FQN`
   byte-sort order (§4.5 inv. 3, §4.4 inv. 1).
2. **Alias table TU.** `data/codegen-output/src/_alias_table.cpp`
   emits `glibre_types_register_fqn_alias` calls for every type-rename
   alias found across the schema set (§4.7 inv. 4).
3. **Manifest TUs.** One `data/codegen-output/src/_manifest_<plugin>.cpp`
   per discovered `plugins/<plugin>/plugin.fbs`, embedding the
   Flatbuffers-serialized `PluginManifest` blob into the plugin's
   `.rodata` (§6.5).

Emission is deterministic: file iteration is `FQN`-sorted, every
generated symbol is namespaced, generated newlines are LF, and the
emitter never reads back its own outputs. Two independent runs of
`glbr-sergeant` over the same schema set on different hosts produce
byte-equal sources (§4.2 inv. 1, PHILOSOPHY §7).

The pipeline's input and output sides are pinned by §7 (the schema
file format) and §5 (the public C++ surface) respectively; this
section specifies only the five phases that connect them and the
ordering rules each phase must obey.

### 6.3 `AliasResolver` (runtime alias lookup)

Lives in `data/runtime/src/alias_table.cpp`. Owns the runtime side
of the `SchemaAliasTable` aggregate (§4.7): looking up deprecated FQNs
in the FQN forward-map, and exposing the field-rename alias accessor
registration API.

**Q1 (ADR #1121): replaces MigrationDispatcher.** There is no per-step
migration dispatch; the only runtime resolution is name aliasing.

**FQN forward-map lookup.** On `Envelope<T>::deserialize(src)`:

1. Read the Flatbuffers size-prefix (4 bytes) and `file_identifier`
   (4-byte magic at offset 4) — §4.8 inv. 1, 2. Truncation before
   byte 8 → `data::Error::EnvelopeTruncated`.
2. Resolve the `file_identifier` to an FQN string via the
   `file_identifier → FQN` table baked into the generated header.
   If the FQN is not in the live registry, check the FQN forward-map
   (§4.7 inv. 2); if the forward-map resolves in one hop, use the
   canonical FQN. Unresolvable → `data::Error::SchemaUnknown`.
3. Look up `RegistryEntry` by the (possibly-resolved) FQN (§4.5 inv. 3).
4. Read `schema_version` from the root table (field-id 2). If
   `schema_version > entry.version` (newer-than-host) →
   `data::Error::DeserializeError` (§4.8 inv. 5).
5. Invoke `entry.deserialize(src, &out)`. Absent fields take their
   declared Flatbuffers defaults (structural evolution; §4.6 inv. 1).

**Purity.** The resolver reads only the alias table and the registry
it holds by `const&`, never mutates either, and allocates nothing.

**Hot-reload integration.** At the frame-8 barrier the loader passes
the pre-swap snapshot through the same deserialize path against the
post-swap registry. Because there are no migration steps, the only
data-layer work is alias resolution + Flatbuffers zero-copy read.
Structural evolution (absent fields → defaults) is handled by `flatc`-
generated accessors; no arena, no chain, no dispatcher (§8; §8.4).

### 6.4 ABI-hash construction and export

The `AbiHash` aggregate (§4.4) lives at the seam between codegen and
runtime: the value is *computed* at codegen time and *exported* at
runtime as a stable C symbol the plugin loader compares.

**Computation (codegen-time).**
`glbr-sergeant`'s `bfbs_hash` module (§6.1.1) consumes the `.bfbs`
sidecar bytes produced in §6.2 stage 3 and produces the
`_abi_hash.cpp` translation unit by:

1. For each schema `s`, compute
   `schema_source_hash(s) = blake3(bfbs_bytes(s))` where
   `bfbs_bytes(s)` is the per-type `.bfbs` output of `flatc --bfbs`
   for schema `s` (Q7, §4.4 inv. 2). The canonicalization module
   (§7.3) is DELETED; `.bfbs` is canonical by construction. The
   output is a 32-byte digest.
2. Form a per-schema entry string: `fqn_utf8(s) || ":" || version_le(s)
   || ":" || schema_source_hash(s)` where `version_le` is the declared
   version as 4 bytes little-endian and `schema_source_hash` is the
   raw 32-byte Blake3 digest from step 1. Sort these entry
   strings by the byte order of each `Schema`'s `FQN` (§4.4 inv. 1).
3. Join the sorted entry strings with a single LF byte (`\n`) between
   each adjacent pair; no trailing newline. This is the normative rule
   from §4.4 inv. 1 and `reviews/decisions/plugin-abi.md` §"ABI Hash
   Function" rule 1. (Outer ABI hash construction is UNCHANGED from
   the prior design; only the per-type hash input changed, Q7.)
4. Compute `blake3(joined_string)` — the resulting 32-byte digest is
   the canonical `AbiHash`.
5. Hex-encode the digest (lowercase, 64 characters) and embed it as
   a `constexpr` `const char*` string literal in
   `_abi_hash.cpp`.

**Export (runtime).** The runtime side exposes the embedded literal
through one C-stable entry point declared in §5 §abi_hash.hpp:

```cpp
extern "C" const char* glibre_types_abi_hash() noexcept;
```

The body lives in `data/runtime/src/abi_hash.cpp` and is a one-line
return of the codegen-embedded literal; the symbol is exported with
default visibility from `glibre-types.dylib`. Plugins re-export the
identical string under `glibre_plugin_abi_hash`, captured at the
plugin's compile time against the same headers
(`reviews/decisions/plugin-abi.md` §"ABI Hash Function"). The loader
compares the two byte-for-byte (no parsing) and refuses load on
mismatch with `core::Error::PluginAbiHashMismatch`
(`reviews/decisions/plugin-abi.md` §"Loader Sequence" step 4).

The export is `noexcept`, allocates nothing, and is callable from
static-init contexts. `glibre_types_abi_hash()` is the *only* path
through which the hash crosses a dylib boundary; no other API exposes
the digest, the canonicalization rule, or the construction inputs.

### 6.5 Plugin-manifest emission (cross-cut with `core`)

The data context owns the Flatbuffers-serialized layout of `PluginManifest`
(§5 §plugin_manifest.hpp; `reviews/decisions/plugin-abi.md`
§"Plugin Manifest Schema") but does not own the loader that consumes
it. `glbr-sergeant` extends its file walk to also process
`plugins/<plugin>/plugin.fbs` files, emitting one
`data/codegen-output/src/_manifest_<plugin>.cpp` per discovered
manifest source. Each emitted TU embeds the Flatbuffers-serialized
`PluginManifest` blob into a `.rodata`-resident `std::byte` array,
defines the plugin-side `glibre_plugin_manifest` /
`glibre_plugin_manifest_size` exports against that array, and
provides the plugin-side `glibre_plugin_abi_hash` re-export of the
same constant `glibre_types_abi_hash()` returns. The loader's read
path is owned by `core` (`reviews/decisions/plugin-abi.md`
§"Loader Sequence" step 3); the data context's only contribution is
the bytes the loader reads.

### 6.6 Build-graph composition

The CMake graph that wires the three modules together is pinned by
`reviews/decisions/flatbuffers-codegen.md` §"CMake Integration"; this
section names the targets, not their internals.

```
glbr-sergeant            (host executable, tools/sergeant/;
                           thin flatc driver + bfbs_hash + alias_emitter)
   │
   ├── flatc (vcpkg-installed host tool; invoked as subprocess)
   │
   ▼
glibre-types-codegen    (custom target; depends on schema glob +
   │                     glbr-sergeant + flatc; outputs to
   │                     data/codegen-output/.stamp)
   │
   ├── data/codegen-output/bfbs/<ctx>.bfbs   (Q10 sidecar; editor-only)
   │
   ▼
glibre-types            (SHARED library; sources = data/runtime/src/
                         + data/codegen-output/src/; depends on
                         glibre-types-codegen)
```

`glbr-sergeant` builds first and is host-only. Schema globs use
`CONFIGURE_DEPENDS` so a touched `.fbs` re-triggers regeneration
without a CMake rerun. The middleman dylib's link line consumes both
hand-written runtime sources and codegen-emitted sources as one unit;
the static-init ordering rule (§4.3 inv. 5) is preserved by the
emitter writing builtin-registration TUs lexicographically before
generated-type TUs.

`.bfbs` sidecars (Q10) are installed next to the context dylib in
editor / tools CMake install targets only. The shipping install target
excludes them (CMake `COMPONENT editor` separation). The middleman
dylib does NOT embed `.bfbs` bytes; they are filesystem-adjacent in
editor builds and absent in shipping builds (PHILOSOPHY §6).

No plugin links `glbr-sergeant`; every plugin links exactly one
`glibre-types.dylib` and may consume Flatbuffers-generated accessor
types at its ABI surface (Q6, §4.10 inv. 4).

### 6.7 Section closure

§6 specifies the *layout* and *plumbing*; it does not redefine any
contract that §4 (aggregates / invariants), §5 (public surface), §7
(schemas), or §8 (hot-reload) already pin. Implementers consult §6
for *where the code lives* and *how the pieces talk*; they consult
§4 / §5 / §7 / §8 for *what each piece must guarantee*. Conflicts
between §6 and any of those sections resolve in favor of §4 / §5 /
§7 / §8; this section is non-binding except where it names a module
or symbol that the rest of the spec already references.

## 7. Persistence & Schemas

The `data` context owns the persistence spine itself; it does not own
domain payloads. What the spine itself persists is a **meta-schema
layer** — the byte-level representation of `Schema` (§4.1), the
`SchemaRegistry` table (§4.5), the per-`FQN` `SchemaAliasTable` records
(§4.7), and the construction inputs to `AbiHash` (§4.4). Those
meta-schemas are the only schemas this section enumerates; every
other persistent type (a `Transform`, a `PluginManifest`, a `Mesh`)
is owned by the originating context's `specs/<ctx>/SPEC.md` §7.

### 7.1 The `.fbs` schema-file format

A `.fbs` file is the authoring artifact that defines exactly one
`Schema` (§4.1). The file format is the load-bearing input to
`Sergeant` (§4.2) and, transitively, to every byte the spine emits.
This subsection pins its bytes; `glbr-sergeant` is the only writer
(via test fixtures) and the only reader.

**Storage shape.** `.fbs` files are UTF-8 text with LF line
endings, NFC-normalized, no BOM. The canonicalization rule that
feeds `schema_source_hash` (§4.4 inv. 2) operates on the *parsed*
form — see §7.3 — so trailing whitespace and reorder of optional
clauses do not perturb the hash. The text form below is the
authored grammar, not the canonical form.

**Header.** Every file opens with a single `schema` declaration
naming the `FQN` and braces:

```flatbuffers
schema glibre.core.Transform {
  version 3
  since   "0.1.0"
  ...
}
```

The opening token `schema` is the file's magic; a file whose first
non-whitespace, non-comment token is not `schema` is rejected with
`ReservedTagViolation` (re-used here as the codegen-front-end's
generic syntactic-error arm — see §10) before any further parsing.
There is no separate magic number because `.fbs` is text; the
file *extension* and the leading `schema` keyword together form the
identifier. Binary headers live on the wire (`Envelope`, §4.8 / §7.5),
not in source files.

**Field declarations.** Inside the braces, an ordered sequence of
clauses describes the schema. Field clauses use the form:

```flatbuffers
field <name> : <type> tag <N> since <V>
field <name> : <type> tag <N> since <V> default <expr>
field <name> : <type> tag <N> since <V> reserved
```

with the following grammar (BNF-shape; whitespace insensitive):

```text
SchemaFile  := SchemaDecl
SchemaDecl  := "schema" FQN "{" Header Clause* "}"
Header      := VersionLine SinceLine?
VersionLine := "version" UINT
SinceLine   := "since" STRING                  # advisory SemVer
Clause      := FieldClause | ReservedClause
              | AliasClause | CommentClause
FieldClause := "field" Ident ":" TypeRef
              "tag" UINT "since" UINT
              ("default" DefaultExpr)?
              ("deprecated" "alias" Ident)?    # Q1 field-rename alias
ReservedClause := "reserved" "tag" UINT
                  ("removed_in" UINT)?
                  ("comment" STRING)?
AliasClause := "type_alias" FQN               # Q1 type-rename alias:
                                              # maps old FQN to this type
TypeRef     := Builtin | FQN | "list" "<" TypeRef ">"
              | "map" "<" Builtin "," TypeRef ">"
              | "option" "<" TypeRef ">"
Builtin     := "uint8" | "uint16" | "uint32" | "uint64"
              | "int8"  | "int16"  | "int32"  | "int64"
              | "float32" | "float64" | "bool"
              | "string" | "bytes"
              | "vec3f" | "quatf"              # Q9: map to FB struct
              | "entity"
DefaultExpr := Number | "true" | "false"
              | StringLit | "{" DefaultExpr ("," DefaultExpr)* "}"
              | "none"
CommentClause := "#" rest-of-line
```

**Q9 (ADR #1121) — `vec3f` / `quatf` are Flatbuffers `struct` (fixed
layout, byte-equal deterministic).** These PHILOSOPHY §7 math primitives
have stable layouts that are industry-invariant; evolvability is not
needed. A Flatbuffers `struct` packs fields consecutively at explicit
alignments with no vtable and no field-id mapping; `flatc --cpp` emits
a C++ class whose layout is `reinterpret_cast`-compatible with the wire
bytes (zero-copy read). See `https://flatbuffers.dev/internals/` for the
normative layout rules. Byte-equal determinism applies to both the wire
form and the in-memory C++ object — the property PHILOSOPHY §7 requires.
Other primitives keep their semantics but are spelled in Flatbuffers IDL:
`uint8` / `uint16` / `uint32` / `uint64` / `int8` / `int16` / `int32` /
`int64` / `float32` / `float64` (note the spelling change from the
prior `u8`/`u16`/`i8`/`i16`/`f32`/`f64` shorthand).

Names follow §4.1 inv. 1: lowercased dotted-context path with a
PascalCase leaf for `FQN`; `[a-z][a-z0-9_]*` for field `Ident`s.

**Field-set rules** (each enforced at `Sergeant` parse time):

1. Every active `tag` integer is unique across the file. The same
   number space covers active and reserved tags (§4.1 inv. 3).
2. `since <V>` on every field satisfies `1 ≤ V ≤ version`
   (§4.1 inv. 5).
3. A `field` whose type is non-`option<T>` and whose `since` is
   greater than `1` must carry a `default` clause. Codegen-time
   error otherwise.
4. Every `TypeRef` resolves either to a `Builtin` (audited in
   `glibre/types/_builtins.hpp`) or to another `FQN` whose `.fbs`
   file is present in the same `Sergeant` invocation; cross-schema
   cycles are detected and rejected (§4.1 inv. 4).
5. `default` expressions are typed against their `TypeRef`:
   numeric literals must fit the integer/float width; `{a, b, c}`
   composites must match a struct's tag-sorted field shape;
   `none` is the only legal default for `option<T>`. The expression
   is a build-time constant — no function calls, no other-field
   references.

**Reserved clauses.** A deprecated field (field removed from use via
`deprecated` in Flatbuffers IDL) also appears as a `reserved tag`
line in glibre's `.fbs` dialect to make the tag reservation explicit.
The optional `removed_in <V>` records the schema version at which the
field was deprecated; the optional `comment` is a free-form string for
human readers. Tag reservation is enforced by `glbr-sergeant`; the
`deprecated` annotation is also present in the raw `.fbs` file for
`flatc --conform` to recognize.

**Field-rename alias clauses (Q1).** An optional `deprecated alias
<new_name>` clause on a deprecated field declaration instructs
`glbr-sergeant` to emit a `[[deprecated]]` accessor forwarding to the
new field name in the generated C++ header (§4.6 alias mechanism 1).

**Type-rename alias clauses (Q1).** An optional `type_alias <old_fqn>`
clause in a schema's header declares that this type is the canonical
successor of `<old_fqn>`. Sergeant emits a `glibre_types_register_fqn_alias`
call for the pair (§4.7 inv. 2; §5 alias_table.hpp).

**Migration clauses: REMOVED (Q1, ADR #1121).** Per-version
`migrate_T_vN_to_vN+1` migration clauses are not present in glibre
`.fbs` files. Structural evolution only; semantic changes go to
cook-time transforms under `glibre-cook` (§4.6 inv. 4).

**Comments.** `#` introduces a line comment; comments are stripped
before `schema_source_hash` computation. There are no block comments.

#### 7.1.1 Worked example (domain payload)

```flatbuffers
schema glibre.core.Transform {
  version 3
  since   "0.1.0"

  field translation : vec3f   tag 1 since 1
  field rotation    : quatf   tag 2 since 1
  field scale       : vec3f   tag 3 since 1   default { 1.0, 1.0, 1.0 }
  field flags       : uint32  tag 4 since 2   default 0
  field parent      : entity  tag 5 since 3   default none
  reserved tag 6  removed_in 3  comment "old `lod_bias`"
  # No migration clauses — structural evolution only (Q1, ADR #1121).
  # fields added at tag 4 (since 2) and tag 5 (since 3) fill in with
  # declared defaults on older payloads (flatc --bfbs / flatc --conform).
  # vec3f and quatf map to Flatbuffers struct (Q9, ADR #1121).
}
```

This is the same shape used in `reviews/decisions/flatbuffers-vs-fory.md`
§Implementation plan step 2, lifted to spec-precision. No migration
clauses (Q1); `uint32` spelling per Flatbuffers IDL (Q9). `vec3f` and
`quatf` compile to Flatbuffers `struct` definitions in the generated
Flatbuffers schema (canonical layout, zero-copy, byte-equal, PHILOSOPHY §7).

### 7.2 Meta-schemas owned by `data`

The `data` context's own persistent records — the rows of the
`SchemaRegistry`, the dispatcher's per-`FQN` migration tables, and
the inputs to `AbiHash` — are themselves authored as `.fbs`
schemas, but they are **bootstrap meta-schemas**: they describe
the spine and so cannot use the spine's runtime migration to
evolve. Their evolution rule is documented in §7.6.

Files owned by `data`:

- `data/schemas/meta/SchemaSourceRecord.fbs` — one row per
  registered `FQN`: the schema source hash, version, source path,
  and migration-chain length. Persisted as the byte form of one
  `RegistryEntry` (§4.5) minus the runtime function pointers.
- `data/schemas/meta/MigrationTableRecord.fbs` — one row per
  registered `(FQN, N → N+1)`: the provider symbol name, the
  source schema FQN, the from/to versions, and a content hash of
  the migration's input/output type pair.
- `data/schemas/meta/AbiHashManifest.fbs` — the
  `(blake3_hex, count, [SchemaSourceRecord*])` triple `Sergeant`
  emits as the input log to `AbiHash`. This is the build-time
  artifact that lets reviewers reproduce the hash from sources.

The Flatbuffers native wire framing (§4.8, §7.2.4 — size-prefix +
`file_identifier` + `schema_version`/`flags` fields in the root table)
is *not* one of these meta-schema files: it is defined by the
Flatbuffers binary format specification, not by a glibre-authored
`.fbs` schema. The `EnvelopeHeader` struct (prior Fory approach) is
DELETED (Q4, ADR #1121); no `.fbs` source file exists for the wire
framing.

These files compile to POD records under `glibre::types::data::*`
(emitted into `glibre-types.dylib`), the same way every other
generated type does — they ride the same codegen pipeline,
participate in the registry, and contribute to `AbiHash`. What
makes them *meta* is the **bootstrap rule** in §7.6, not their
file format.

#### 7.2.1 `SchemaSourceRecord`

```flatbuffers
schema glibre.data.SchemaSourceRecord {
  version 1
  since   "0.1.0"

  field fqn          : string                     tag 1 since 1
  field version      : u32                        tag 2 since 1
  field source_hash  : bytes                      tag 3 since 1
  field source_path  : string                     tag 4 since 1
  field migration_count : u32                     tag 5 since 1
  field reflection_present : bool                 tag 6 since 1
}
```

- `source_hash` is exactly 32 bytes (Blake3-256 of the canonical
  schema form per §7.3); shorter or longer sequences are
  `DeserializeError`.
- `source_path` is the workspace-relative path of the originating
  `.fbs` file, sorted by canonical Unicode code-point order
  before `AbiHash` ingests the table (§4.4 inv. 1).
- `migration_count` matches `version - 1` exactly (§4.7 inv. 1);
  any other value is a `SchemaRegistryConflict` at static-init.
- `reflection_present` mirrors §4.9 inv. 5; the runtime path
  never branches on this value, but tools and the editor do.

#### 7.2.2 `MigrationTableRecord`

```flatbuffers
schema glibre.data.MigrationTableRecord {
  version 1
  since   "0.1.0"

  field fqn           : string  tag 1 since 1
  field from_version  : u32     tag 2 since 1
  field to_version    : u32     tag 3 since 1
  field provider_name : string  tag 4 since 1
  field source_hash_from : bytes tag 5 since 1
  field source_hash_to   : bytes tag 6 since 1
}
```

- `to_version == from_version + 1` is required (§4.6 inv. 5);
  multi-step records are illegal at codegen.
- `source_hash_from`/`source_hash_to` are the Blake3-256 hashes of
  the two endpoint schemas. They make the migration's input/output
  contract content-addressable: a migration is defined relative to
  fixed schema-versions of its endpoint types and `Sergeant` rejects
  re-binding a provider symbol against a different `(from, to)`
  pair (§4.7 inv. 1, §4.6 inv. 4).
- `provider_name` is the fully-qualified C++ symbol name codegen
  emits the dispatcher hookup against. The symbol must resolve at
  link time; missing symbols are a `glibre-types.dylib` link error,
  not a runtime `SchemaMigrationFailure`.

#### 7.2.3 `AbiHashManifest`

```flatbuffers
schema glibre.data.AbiHashManifest {
  version 1
  since   "0.1.0"

  field abi_hash_hex  : string                     tag 1 since 1
  field sergeant_version : SemVer                     tag 2 since 1
  field entries       : list<SchemaSourceRecord>   tag 3 since 1
}
```

- `abi_hash_hex` is a 64-character lowercase hex Blake3-256 string
  (§4.4 inv. 1) computed exactly as
  `blake3( join( "\n", sort_by_fqn( { fqn_utf8(s) || ":" || version_le(s) || ":" || schema_source_hash(s) : s ∈ entries } ) ) )`;
  the 32-byte digest is then hex-encoded to produce the 64-char string
  stored in this field. Entry strings use the same LF separator and
  no trailing newline as §4.4 inv. 1 specifies.
- `sergeant_version` is the `glbr-sergeant` SemVer that produced this
  manifest. It exists for diagnostic reproducibility — two builds
  with byte-identical entries but different `sergeant_version` must
  still produce the same `abi_hash_hex`, so the field is **not**
  an input to the digest (§4.4 inv. 4).
- `entries` is sorted by `fqn` ascending (lexicographic byte order)
  before serialization. Out-of-order lists produce
  `DeserializeError` so the file's bytes are a faithful audit
  artifact.
- `SemVer` here is the `glibre.core.SemVer` schema declared in
  `reviews/decisions/plugin-abi.md` §"Plugin Manifest Schema".

#### 7.2.4 Flatbuffers native wire framing (Q4 — replaces EnvelopeHeader)

**Q4 (ADR #1121):** The `EnvelopeHeader` struct is DELETED. The wire
framing is now Flatbuffers native:

```text
Flatbuffers size-prefixed buffer layout (§4.8, Q4):
  [0..3]   : uint32 LE — buffer byte count (size-prefix)
  [4..7]   : 4-byte file_identifier — type tag (ASCII magic)
  [8..]    : Flatbuffers root table payload
               field-id 2: schema_version : uint32 (LE)
               field-id 3: flags          : uint32 (LE, reserved; emit 0)
               ...         type-specific fields
```

- The `file_identifier` (4-byte magic at offset 4) names the type
  family. It is declared in the `.fbs` root table and baked into the
  Flatbuffers-generated C++ at codegen time.
- `schema_version` (field-id 2) carries the `SchemaVersion` INSIDE
  the root table; no separate header prefix.
- `flags` (field-id 3) is reserved-by-name. Adding a flag bit is a
  field append (structural evolution, Q1 / §4.6) — no bootstrap-rule
  change required unless the bit alters the wire layout semantics.
- No `EnvelopeHeader` C++ struct exists; `glibre::types::Envelope<T>`
  exposes `serialize`/`deserialize` only (§5 envelope.hpp).
- Per-version goldens (§7.5) now compare size-prefixed Flatbuffers
  buffers; `flatbuffers::Verifier` validates each golden on load.
- This framing is NOT listed in `AbiHashManifest.entries`; changes to
  it force a `glbr-sergeant` release and a new `AbiHash` value via
  the meta-schema bootstrap rule (§7.6), not the per-schema source-hash
  path — same rule as before, now operating on `.bfbs` bytes (Q7).

### 7.3 Schema-source hash input (Q7 — `.bfbs` replaces canonicalization)

**Q7 (ADR #1121): The hand-rolled schema-source canonicalization rule is
DELETED.** The `schema_source_hash` of a schema `s` is now:

```
schema_source_hash(s) := blake3( bfbs_bytes(s) )
```

where `bfbs_bytes(s)` is the binary Flatbuffers schema (`--bfbs` output)
emitted by `flatc` for schema `s`. The `.bfbs` format is canonical by
construction: `flatc` produces deterministic binary output for identical
`.fbs` input across all supported hosts (PHILOSOPHY §7). No separate
canonicalization pass is required; no `canonicalize.cpp` module exists in
`glbr-sergeant`.

The outer ABI hash construction (Blake3 over LF-joined, FQN-sorted
`(fqn || ":" || version_le || ":" || schema_source_hash)` entries) is
**UNCHANGED** from the prior design (§4.4 inv. 1). Only the per-type
hash input changed: `.bfbs` bytes replace `.fbs` source bytes.

The hash result is a 32-byte digest stored as the `source_hash` field of
`SchemaSourceRecord` (§7.2.1) and as the per-type input to `AbiHash`
(§4.4 inv. 1, inv. 2).

### 7.4 Structural evolution rules (Q1 — replaces Migration rules)

**Q1 (ADR #1121): Per-version migration functions are DROPPED.**
Structural evolution only; semantic changes go to `glibre-cook` at
cook time. The rules below govern what counts as a structural evolution
and what requires a cook-time semantic transform.

1. **Append-only field additions are structural.** Adding a new field
   at the next free tag is structural evolution: `flatc --conform` passes,
   absent fields in older payloads take their declared Flatbuffers default,
   and the schema version bumps without authoring any migration provider.
   `Sergeant` enforces this via `--conform` check (§6.2 stage 1).

2. **Field deprecation (field removal) is structural.** Marking a field
   `deprecated` in the `.fbs` file is a structural change: the tag slot
   is retained in the type-space (`reserved` in glibre dialect), the field
   disappears from the generated accessor API (replaced by a
   `[[deprecated]]` thin forwarder if a field-rename alias is declared,
   else simply inaccessible). Older payloads still carry the bytes; the
   generated accessor silently ignores them. `flatc --conform` accepts
   deprecations.

3. **Field-rename aliases are structural.** A `deprecated alias <new>`
   clause emits a `[[deprecated]]` forwarder in the generated header.
   No wire change; structural by construction.

4. **Type-rename aliases are structural.** A `type_alias <old_fqn>`
   clause registers the forward-map entry in the SchemaAliasTable. No
   wire change; the loader resolves in one hop (§4.7 inv. 2).

5. **Any other change is semantic and goes to `glibre-cook`.** Field
   meaning changes, unit conversion, FK rebind, and derived-field
   invalidation are cook-time transforms. `glibre-cook` reads `.bfbs`
   produced by sergeant and consumes Flatbuffers buffers; it never
   invokes `flatc` itself. Transforms are checked-in source under
   `tools/glibre-cook/transforms/<context>/<from-hash>_to_<to-hash>.cpp`
   and run once at content cook time.

6. **Tag reuse is forbidden — forever.** Reserved tags (§4.1 inv. 3)
   carry forward across versions; `Sergeant` errors with
   `ReservedTagViolation` on any reuse. There is no structural evolution
   shape for rebinding a tag to a different field type.

### 7.5 Wire format and round-trip identity

**Q4 (ADR #1121):** Every persistent payload is a Flatbuffers
size-prefixed buffer (§7.2.4, §4.8). The `file_identifier` (4-byte
magic at buffer offset 4) plus the `schema_version` field inside the
root table are the sole source of dispatch truth at deserialize time
(§4.8 inv. 5). The `EnvelopeHeader` prefix is DELETED.

Round-trip identity (§4.8 inv. 4) is tested per-schema via
Catch2 goldens at `tests/data/schemas/<ctx>/<Type>.cpp`:

1. Construct an instance `t` from a deterministic constructor
   recipe.
2. `bytes := Envelope<T>::serialize(t)` — produces a Flatbuffers
   size-prefixed buffer.
3. Assert `bytes` byte-equal a checked-in `golden.bin`
   (or regenerate under `--update-goldens`). Validate with
   `flatbuffers::Verifier` before comparing.
4. `t' := Envelope<T>::deserialize(bytes).value()`.
5. Assert `t' == t` (zero-copy read; accessor-level field comparison).
6. Assert `Envelope<T>::serialize(t') == bytes`.

Per-version goldens are kept under
`tests/data/schemas/<ctx>/<Type>/v<N>.bin` so older payloads
exercise the structural evolution path (Q1):

1. Read the `vN` golden bytes.
2. `t := Envelope<T>::deserialize(bytes).value()` — absent fields
   since version `N` take their declared Flatbuffers defaults
   (no migration dispatch; structural evolution only).
3. Assert `t` matches the version-`M` golden (accessor-level).
4. Assert `Envelope<T>::serialize(t)` byte-equals the version-`M`
   serialized golden.

Structural evolution (append-only field additions with defaults) is
exercised at the alias-table layer (§4.7) rather than a migration
dispatcher; the data context guarantees *round-trip* identity, which
is the property loader and hot-reload code consume.

### 7.6 Bootstrap rule for meta-schemas

The meta-schemas in §7.2 (`SchemaSourceRecord`,
`MigrationTableRecord`, `AbiHashManifest`, and the native
Flatbuffers wire framing in §7.2.4) describe the spine itself
and so **cannot** evolve through the in-process structural-evolution
path they describe. The bootstrap rule decouples them from the runtime
path:

1. **Out-of-band versioning.** Each meta-schema carries the
   normal `version` integer in its `.fbs` header, but the
   `version` is bumped *only* in lockstep with a `glbr-sergeant`
   release. The release notes call out the bump and the manual
   rebuild steps tools and existing build artifacts must take.
2. **No structural evolution at runtime.** Meta-schemas under
   `data/schemas/meta/` are processed normally by `glbr-sergeant`
   (including `--conform` checks), but older meta-schema payloads
   are not consumed at runtime — they are regenerated from source
   by the new `glbr-sergeant`. There is no `--conform` regression
   requirement for meta-schemas between releases; only between the
   current and prior build in the same release.
3. **Single live version per build.** A given `glbr-sergeant`
   release emits exactly one version of each meta-schema. The
   middleman dylib produced by that release reads and writes
   only that version. There is no v1-schema reader inside a build
   that emits v2-schema payloads.
4. **Cross-build artefacts are reproduced, not evolved.**
   `AbiHashManifest` files, intermediate `.sergeant-stamp` blobs,
   and the `.bfbs` sidecars embedded in the build tree are
   *artefacts of the build*, not user-data. When a meta-schema
   bumps, the artefacts are regenerated from authoring sources by
   the new `glbr-sergeant`; no cook-time transform or structural
   evolution pass is run.
5. **Manual steps live in `glbr-sergeant` release notes.** When
   the native wire framing (§7.2.4) gains a new flag field, the
   release notes spell out (a) the wire-format diff, (b) any
   on-disk artefact rebuild needed, (c) any tooling step
   downstream consumers must take. The data context owns these
   notes alongside the `glbr-sergeant` source repository.
6. **Hash invariants hold.** Meta-schemas contribute to `AbiHash`
   exactly like any other schema (§4.4 inv. 1), via `.bfbs` bytes
   (Q7). A meta-schema bump forces an `AbiHash` change, which
   forces every plugin to rebuild — the same gate that catches any
   other schema-shape drift (`reviews/decisions/plugin-abi.md`
   §"Versioning Rules"). The bootstrap rule lifts only the
   *runtime structural-evolution* obligation, not the ABI-gating one.

The collapse: domain schemas evolve structurally at runtime via
append-with-defaults and alias tables; the spine itself evolves at
*release time* via `glbr-sergeant`. Two surfaces, one ABI gate.

### 7.7 Cross-context obligations

Every other context's `specs/<ctx>/SPEC.md` §7 enumerates the
domain schemas the context owns under
`data/schemas/<ctx>/<Type>.fbs`. Those sections inherit this
section's rules:

1. The grammar in §7.1 is the only legal `.fbs` syntax.
2. Structural evolution rules obey §7.4 (Q1 — no migration providers).
   Semantic changes go to `glibre-cook` cook-time transforms; the
   `data` context does not gate those transforms.
3. Round-trip goldens follow the harness in §7.5; per-version
   goldens are mandatory whenever the schema's version exceeds 1,
   to exercise the structural evolution path (absent fields → defaults).
4. The `data` context does **not** own those schemas — adding a
   new domain schema requires no edit to this section. What `data`
   owns is the meta-schema layer in §7.2 and the rules above.

The biconditional in §4.10 inv. 1 is the load-bearing connector:
every `.fbs` file under `data/schemas/<ctx>/` corresponds to one
registry entry, and every registry entry corresponds to one
`.fbs` file — verified at configure time by `Sergeant` (§4.5 inv. 5).

## 8. Hot-Reload Contract

The `data` context is the persistence spine; it does not run the
hot-reload state machine. The loader at the frame-8 barrier owns the
four-step **drain → swap → migrate → resume** protocol
(`reviews/decisions/hot-reload-protocol.md` §"Protocol Sequence"); the
data context owns step 3 (**migrate**) and the gate values steps 2 and
4 consult (`AbiHash`, `SchemaRegistry`). This section pins the contract
between those owners: what bytes survive, what `reconcile(...)` must do,
which refusals are typed at this layer, and how observers are notified
without ever seeing a half-swapped registry.

Two reload modes exist and the rules differ between them. **Mode A —
per-plugin reload** (the common case) is what every domain plugin
exercises in dev and CI workflows. **Mode B — `glibre-types.dylib`
reload** is rare, requires engine restart by default, and is permitted
only under the strict conditions in §8.3.

### 8.1 What survives a swap

The §4.10 inv. 6 determinism guarantee combines with the
hot-reload-protocol survival rule (`reviews/decisions/hot-reload-protocol.md`
§"State Survival Rules") to yield one mechanical predicate the data
context promises to preserve:

> **Data survives the swap if and only if its type has a `.fbs`
> schema registered in the live `SchemaRegistry`.**

This biconditional is the same one the loader checks; the data context
guarantees the *forward* direction (registered ⇒ preserved by either
identity or structural evolution) and the codegen guarantees the *reverse*
direction (no schema ⇒ no registry entry ⇒ not persistent ⇒ not
preserved). Concretely, the spine preserves:

1. **Generated-type byte storage.** Every ECS component, world
   singleton, asset payload, or plugin-private record whose C++ type
   is a `Generated Type` (§4.2) survives. When the new plugin's
   `SchemaVersion` for the type exceeds the stored version, Flatbuffers
   zero-copy reads handle absent fields via declared defaults (structural
   evolution, Q1); no in-place per-row dispatch is required. Rows whose
   stored version equals the current version are passed through unchanged.
2. **The `SchemaRegistry` itself.** In Mode A the live registry is
   not mutated — Q's manifest re-references existing entries by
   FQN, and the loader's step 2.4 appends only entries Q introduces
   that are *new* and append-additive. In Mode B the live registry
   is *replaced wholesale* (§8.3); no in-place mutation crosses a
   reload.
3. **The `AbiHash` value.** Mode A leaves the host's
   `glibre_types_abi_hash()` unchanged by definition (§4.4 inv. 4
   makes it a build-time constant of the middleman, not of any
   plugin). Mode B is the only reload that publishes a new
   `AbiHash`, and it does so by replacing the middleman as a unit
   (§8.3).
4. **`.bfbs` sidecar bytes — editor-only, not preserved across reloads
   (Q10).** `.bfbs` sidecar files are filesystem-adjacent to the
   context dylib; on hot-reload the loader re-reads the new sidecar
   from disk (editor mode only). The `.bfbs` bytes are not carried in
   process memory between reloads; the `ReflectionBlob` span (§4.9) is
   re-sliced from the new sidecar after each successful swap.
   Shipping builds carry no `.bfbs` bytes at all.

State that does **not** survive (consistent with
`reviews/decisions/hot-reload-protocol.md` §"State Survival Rules"
"Re-derived"):

- Plugin-private types without a `.fbs` schema. By §4.10 inv. 1
  these have no registry entry and the spine has no structural-evolution
  story for them.
- Field-rename alias forwarders for plugin-emitted types whose plugin
  is being swapped out — the forwarders point into the *outgoing*
  plugin's generated headers. The new plugin's codegen emits updated
  alias registrations at static-init via `glibre_types_register_fqn_alias`
  before the loader proceeds past step 4.1 (§4.5 inv. 4 still holds:
  the registry is read-only after static-init *of the live middleman build*).
- Editor-only `ReflectionBlob` `.bfbs` spans for types whose schemas
  dropped between Q and P (Q10). Tools re-read the new sidecar; an
  empty span for a dropped type is the expected state.

### 8.2 The `reconcile(...)` responsibility

**Q1 (ADR #1121): The `migrate(...)` function is renamed and simplified
to `reconcile(...)`.** There are no per-step `MigrationFn` dispatches;
the only data-layer work at hot-reload is schema-set continuity
verification and alias-table refresh.

```cpp
namespace glibre::types::data {

// Called by the loader at hot-reload-protocol step 3, once per
// outgoing-plugin / incoming-plugin pair. Validates schema-set
// continuity and refreshes the alias table (Q1).
// Structural evolution (absent fields → defaults) is handled
// automatically by Flatbuffers-generated accessors; no per-row
// dispatch is required.
[[nodiscard]] auto reconcile(
    const SchemaRegistry& outgoing,    // pre-swap registry view
    const SchemaRegistry& incoming,    // post-swap registry view
    World&                world        // borrowed; storage walk only
) noexcept -> std::expected<ReconcileReport, Error>;

}  // namespace glibre::types::data
```

**Responsibility.** `reconcile(...)` performs *exactly* the data
context's part of the loader's step 3:

1. **Schema-set continuity check.** For every `FQN` registered in
   `outgoing` and referenced by any surviving storage row in
   `world`, `reconcile` asserts the same `FQN` is registered in
   `incoming` (directly or via a type-rename alias in §4.7).
   A missing `FQN` is a major-version change; `reconcile` returns
   `unexpected(Error::SchemaMigrationFailure)` carrying the dropped
   `FQN` in the detail payload (the arm is retained per Q5, §10).
2. **Alias-table refresh.** For any type-rename alias in `incoming`
   that was absent in `outgoing`, `reconcile` registers the new
   alias entry in the live `SchemaAliasTable` (§4.7). This is the
   only write to the alias table that happens at hot-reload; the
   registry itself is append-only (§4.5 inv. 4).
3. **No per-row dispatch.** Flatbuffers' zero-copy read handles
   absent fields via declared defaults (structural evolution, §4.6).
   There is no arena, no per-step chain, no in-place overwrite;
   each storage row is readable by the incoming accessor immediately.
4. **Validation, not transformation.** `reconcile` does not run any
   plugin-private code, never reads `World` outside the FQN
   continuity check, never touches the filesystem, and allocates nothing.
5. **Report-out.** On success `ReconcileReport` contains the count
   of FQNs resolved via alias and the sorted span of FQNs whose
   `schema_version` changed between `outgoing` and `incoming`
   (i.e. those with a new structural append). The loader forwards
   the span into the `HotReloadCompleted` event at protocol step 4.3.

**Out of scope — `reconcile(...)` does not.** The function does *not*
dlopen, dlsym, swap vtables, mutate the system schedule, log
structured warnings, or publish observer events (§1; §4.10 inv. 7).

**Idempotence.** Re-invoking `reconcile(...)` against an `outgoing`
whose registry already matches `incoming` is a no-op that returns
`ReconcileReport{count = 0, changed = {}}`. This makes step-3
retries safe in the loader's rollback path.

### 8.3 Self-reload of `glibre-types.dylib`

The middleman dylib itself is a hot-reload candidate only under the
strictest gate in the spine. By default it requires engine restart
(PHILOSOPHY §8 / `reviews/decisions/hot-reload-protocol.md`
§"Consequences" — self-reload of the middleman is listed as out of
MVP scope). The data context records the contract this section
satisfies *if and when* a future MVP+ spike turns the gate on:

A new middleman build `Q-types` may replace the live `P-types` at
phase 8 only if **all** of the following hold; failing any single
condition refuses the reload and leaves `P-types` live:

1. **Every loaded plugin proves ABI continuity.** For every plugin
   `P_i` currently registered, `P_i.glibre_plugin_abi_hash() ==
   Q_types.glibre_types_abi_hash()` *or* the plugin is also being
   reloaded in the same phase 8 against `Q-types`. A single plugin
   compiled against the old hash defeats the reload; the loader
   returns `core::Error::PluginAbiHashMismatch` carrying the
   offending plugin's name.
2. **Schema-set continuity is verified.** For every `FQN` in
   `P_types.SchemaRegistry` whose `SchemaVersion` differs from the
   `FQN`'s version in `Q_types.SchemaRegistry`, the new version must
   be structurally compatible (i.e. `flatc --conform` passes for the
   old vs. new schema pair). Missing a FQN entirely refuses with
   `Error::SchemaMigrationFailure` (`reviews/decisions/hot-reload-protocol.md`
   §"Refusal Cases" #2; the arm is retained per Q5, §10). Continuity
   is checked via `Q-types`'s `AbiHashManifest` before any reader
   is swapped.
3. **Meta-schema bootstrap rule holds.** Per §7.6, a
   `glibre-types.dylib` reload that bumps any of the meta-schemas
   (`SchemaSourceRecord`, `MigrationTableRecord`,
   `AbiHashManifest`, or the native wire framing §7.2.4)
   requires release-time tooling via `glbr-sergeant`, not
   in-process structural evolution. A self-reload that crosses such
   a bump is refused by
   construction; the operator either rebuilds the world from
   sources via the new `glbr-sergeant` (acceptable) or restarts the
   process against the new middleman (acceptable). In-process
   self-reload is *only* permitted when the new build's
   meta-schemas are byte-identical to the live build's.
4. **Live registry replacement is wholesale.** When all gates pass,
   the loader publishes `Q_types.SchemaRegistry` as the new
   `instance()` (§4.5 inv. 4) atomically alongside the vtable
   swap; the previous registry is dropped only after every plugin
   in the same phase 8 has completed its step 4. There is no
   intermediate "merged" registry — readers see exactly one
   well-formed `SchemaRegistry` at every observable moment.

The collapse: a `glibre-types.dylib` reload is **a multi-plugin
reload plus a meta-schema-frozen middleman swap**, gated by
*every* loaded plugin's ABI hash and by *every* migrated type's
chain. The default failure mode — engine restart — is preferable
in MVP, and this contract exists so that the future enabling spike
inherits a clear refusal envelope rather than ad-hoc rules.

### 8.4 Refusal cases owned by `data`

The hot-reload-protocol enumerates three umbrella refusal cases
(`reviews/decisions/hot-reload-protocol.md` §"Refusal Cases"). The
data context types and raises exactly two of them:

| Loader symptom                                      | Data-typed cause                          | Detection point                              |
|-----------------------------------------------------|-------------------------------------------|----------------------------------------------|
| `Q.glibre_types_abi_hash() != host.abi_hash()`      | `Error::AbiHashMismatch` (§5.3)           | step 2.1 (Mode A); §8.3 gate 1 (Mode B)      |
| Schema-set continuity check: FQN in `outgoing` missing from `incoming` (not resolvable via alias) | `Error::SchemaMigrationFailure` (§5.3, Q5 retained) | `reconcile(...)` step 1 (§8.2); §8.3 gate 2 |
| `Q.glibre_plugin_register` returned `unexpected`    | (not data — `core::Error::PluginInitFailed`) | step 4.1                                  |

The third case is included for completeness only; its detection and
typing live in `core` per `reviews/decisions/plugin-abi.md`
§"Failure Modes → core::Error". The data context contributes no
`ReconcileReport` context to it — `reconcile(...)` returns only
when schema-set continuity is already confirmed.

**Schema-set continuity refusals** (§8.2 step 1) raise
`Error::SchemaMigrationFailure` (retained per Q5, §10). The data
context takes the §10 closed sum at face value: a missing FQN in
`incoming` is a structural evolution gap that the plugin author must
resolve by adding a type-rename alias (§7.4 rule 4). The arm is
retained so existing `core::Error::SchemaMigrationFailed` wrappers
and operator tooling continue to function without change.

**Rollback safety.** `reconcile(...)` is validation-only: it never
mutates storage rows, never writes arena bytes, never runs
plugin-private code. A refusal inside `reconcile(...)` leaves the
world byte-identical to its pre-`reconcile` state — there is no
partial mutation to roll back. The loader's outer rollback
(un-swap vtable, re-register P) proceeds immediately.

**Logging.** Refusals are logged exactly once at `warn` by the
loader (`reviews/decisions/hot-reload-protocol.md` §"Refusal
Cases"); the data context emits no log of its own. The structured
fields the loader records include the `ReconcileReport`'s alias
count and changed-FQN span so operators can see what continuity
check blocked the swap.

### 8.5 Observer notification — `SchemaRegistry` change

The data context publishes one observer event around hot-reload, on
the loader thread, synchronous with the loader's
`HotReloadCompleted`/`HotReloadRefused` events
(`reviews/decisions/hot-reload-protocol.md` §"Observer Notification"):

```cpp
namespace glibre::types::data {

struct SchemaRegistryChange {
    enum class Kind : std::uint8_t {
        EntriesAppended = 0,    // Mode A: new FQNs added by Q
        VersionsBumped  = 1,    // Mode A: SchemaVersion of an existing FQN moved up
        AliasesAdded    = 2,    // Mode A: new type-rename aliases registered by Q (Q1)
        RegistryReplaced = 3,   // Mode B: full SchemaRegistry instance() swap
    };
    Kind                              kind{Kind::EntriesAppended};
    std::span<const SchemaId>         affected_fqns{};   // sorted ascending
    std::span<const SchemaId>         aliased_fqns{};    // FQNs resolved via new aliases (Q1)
};

// Subscribers observe a fully-swapped, fully-migrated registry.
// Synchronous on the loader thread; no allocation past the call
// boundary (the spans alias loader-owned storage that lives until
// the event handler returns).
[[nodiscard]] auto subscribe_schema_registry_change(
    void* userdata,
    void (*on_change)(void* userdata, const SchemaRegistryChange&) noexcept
) noexcept -> SubscriptionId;

}  // namespace glibre::types::data
```

**Atomicity.** The event is delivered after `reconcile(...)` succeeds
and after the loader's own step 4.2 caches are rebuilt — i.e. the
subscriber sees the post-swap world exactly the way it will tick
in frame N+1. Subscribers never observe a half-reconciled registry
(§4.5 inv. 4 + §8.1 inv. 2 combined: Mode A's registry is
append-only mid-phase-8, Mode B's swap is wholesale).

**Subscriber discipline.** Subscribers may walk the registry and
build derived caches; they may not call back into the data
context's mutating API (the registry is read-only by §4.5 inv. 4).
The MVP subscriber set is ≤ 10 (editor live-reload UI, e2e
harness, profiler attach-point); registry-walk cost dominates the
notification cost and is captured in the §9 budget.

**No queue.** The observer bus is synchronous and unbuffered. A
subscriber that throws (impossible — `noexcept` boundary) or that
takes excess time to return delays the loader's exit from phase 8;
the `core` perf budget bounds the latency the loader tolerates and
this section adopts that bound by reference rather than restating
it.

### 8.6 Test hooks — deterministic schema-version bump fixture

Hot-reload is testable in-process per
`reviews/decisions/hot-reload-protocol.md` §"Test Hooks". The data
context exports the schema-side counterpart to the loader's
`enqueue_hot_reload`: a deterministic fixture that builds a synthetic
`SchemaRegistry` whose entry for one FQN has a bumped `SchemaVersion`
and, optionally, a paired type-rename or field-rename alias — without
touching the filesystem.

```cpp
#if defined(GLIBRE_E2E)
namespace glibre::types::data::test {

// Builds a fresh SchemaRegistry instance whose entry for `fqn` is
// upgraded by exactly one version. The returned registry is suitable
// for passing to the loader's enqueue_hot_reload as the post-swap
// view; existing entries for other FQNs are copied identically.
//
// Pure, allocation-free. The bumped version is recorded in a
// deterministic SchemaSourceHash derived solely from the
// (fqn, new_version) pair's synthetic .bfbs bytes (Q7), so
// repeated calls with the same arguments produce byte-identical
// registries (PHILOSOPHY §7).
auto bump_schema_version(
    const SchemaRegistry& base,
    SchemaId              fqn,
    SchemaVersion         new_version      // = base.version(fqn) + 1
) noexcept -> std::expected<const SchemaRegistry*, Error>;

// Registers a synthetic type-rename alias in a copy of `base`.
// Used to test that reconcile(...) accepts an incoming registry
// that carries the alias, and that SchemaRegistryChange::Kind::AliasesAdded
// fires (Q1). Idempotent.
auto add_fqn_alias(
    const SchemaRegistry& base,
    SchemaId              old_fqn,
    SchemaId              new_fqn
) noexcept -> std::expected<const SchemaRegistry*, Error>;

// Forces the next reconcile() call's continuity check to treat
// `fqn` as missing from the incoming registry, returning
// Error::SchemaMigrationFailure. Used to exercise the full
// rollback path in the loader's failure tests
// (reviews/decisions/hot-reload-protocol.md §"Test Hooks" CI
// scenario 4). Idempotent; clearing requires
// `clear_force_continuity_failure(fqn)`.
void force_continuity_failure(SchemaId fqn) noexcept;
void clear_force_continuity_failure(SchemaId fqn) noexcept;

}  // namespace glibre::types::data::test
#endif
```

**Determinism.** `bump_schema_version` is a pure function of its
inputs; the synthesized `SchemaSourceHash` is reproducible across
runs and hosts. The fixture never allocates outside the system
allocator (test-only context) and never writes to disk. Tests under
`tests/data/schemas/hot_reload/` exercise:

1. Happy-path structural bump: a `vN` payload deserialized with the
   `vN+1` accessor reads absent fields as their Flatbuffers defaults;
   `SchemaRegistryChange::Kind::VersionsBumped` fires once with the
   bumped FQN; `Envelope<T>::deserialize` byte-equals a checked-in golden.
2. Continuity-gap refusal: `force_continuity_failure` forces
   `reconcile(...)` to return `Error::SchemaMigrationFailure`
   without touching any storage row (§8.4 rollback safety).
3. Type-rename alias: `add_fqn_alias` builds an incoming registry
   carrying a type-rename alias; `reconcile(...)` succeeds;
   `SchemaRegistryChange::Kind::AliasesAdded` fires with the aliased FQN.
4. Append-only continuity: a bump that adds a new FQN (rather
   than upgrading an existing one) fires
   `SchemaRegistryChange::Kind::EntriesAppended` and
   `reconcile(...)` reports zero alias resolutions.

These fixtures back the §11 acceptance criteria and are the only
allowed entry into the spine's hot-reload machinery from test code.

### 8.7 Cross-context obligations

Every other context's `specs/<ctx>/SPEC.md` §8 specifies *its*
plugin-side hot-reload obligations against the contract above.
Those sections inherit:

1. **Structural evolution obeys §7.4 (Q1)** — field additions,
   field-rename aliases, and type-rename aliases are registered via
   codegen at static-init. There are no `MigrationFn` bodies to
   author; semantic transforms go to `glibre-cook`.
2. **`SchemaRegistryChange` subscribers may not allocate** during
   the synchronous notification window. The editor and tools
   subscribe a pre-allocated handler; runtime contexts that need
   notification must pre-arrange their cache structure at
   `glibre_plugin_register` time.
3. **No context but `data` mutates the registry.** Plugins
   contribute entries through codegen (statically) and aliases
   through `glibre_types_register_fqn_alias` (at static-init);
   nothing else writes (§4.10 inv. 7).
4. **Self-reload of `glibre-types.dylib` is opt-in.** Contexts
   may not assume Mode B is enabled; the default in MVP is
   process restart (§8.3, PHILOSOPHY §8).

The collapse: domain plugins register alias entries via codegen
and subscribe to registry changes; the data context owns one entry
point (`reconcile(...)`), one observer event (`SchemaRegistryChange`),
two refusal arms (`AbiHashMismatch`, `SchemaMigrationFailure`),
and one deterministic test fixture (`bump_schema_version`).
Everything else lives in the loader (`core`) or in the
originating contexts.

## 9. Performance Budget

Quotes the `data` row of the engine-wide per-context budget locked in
`reviews/decisions/perf-budget.md` and refines it into per-aggregate
ceilings that sum into the row. Numbers are the contract; CI gates
enforce them per Allocator Rules and the per-context unit benchmarks
required by the decision record.

### 9.1 Context row

| Quantity            | Budget    | Source / phase ownership                                                    |
|---------------------|-----------|-----------------------------------------------------------------------------|
| CPU sim (per frame) | 0.20 ms   | persistence spine; per-frame work is reconcile handoff + handle bookkeeping |
| CPU submit          | 0.00 ms   | data does not record GPU work                                                |
| GPU                 | n/a       | data owns no Metal heaps or encoders                                         |
| Heap ceiling        | 32 MiB    | resident middleman tables + active migration scratch                         |
| Phase ownership     | none      | participates inside phase 8 (hot-reload barrier) on reload frames only       |

The 0.20 ms sim cell is reserved so a `reconcile(...)` call that lands on a
hot-reload frame (Scenario S2 from `perf-budget.md`) does not blow out
the budget; steady-state cost in S1 is near zero. `data` records no
CPU-submit, no GPU, and owns no phase outright — the `reconcile(...)` step
is invoked by `core` from inside phase 8 and accounted under
`data`'s tag via `glibre::PerContextAllocator`.

### 9.2 Per-aggregate budget

Aggregates are the nine SRP-bounded units defined in §4. Each row
quotes a CPU per-call cost (or per-frame, where the work is per-frame),
a heap sub-ceiling, and the invocation site. Sub-ceilings sum to the
32 MiB row above; CPU per-frame contributions sum into the 0.20 ms cell.

| Aggregate             | CPU cost                                                                | Heap sub-ceiling | Invocation site                                              |
|-----------------------|-------------------------------------------------------------------------|------------------|--------------------------------------------------------------|
| `Schema`              | 0 (host-only authoring; not in runtime)                                 | 0                | not loaded in shipping build                                 |
| `Sergeant`               | 0 (host build-tool, not in runtime budget)                              | 0                | build graph only; no runtime presence                        |
| `Middleman` (dylib)   | 0 per frame (link-time presence; init at process start)                 | 0 sub-ceiling    | static-init populates `SchemaRegistry`; no per-frame work    |
| `AbiHash`             | 0 (`O(1)` static string return; called once at plugin load)             | 0                | plugin-loader handshake; not on the hot path                 |
| `SchemaRegistry`      | ~10 ns per lookup (read-only static `flat_map` over `(fqn, version)`)   | 8 MiB            | called from `Envelope` deserialize; bounded by call count    |
| `SchemaAliasTable`    | ~10 ns per FQN lookup (one-hop forward-map; read-only after static-init) | counted in `SchemaRegistry` 8 MiB                            | `reconcile(...)` alias-table refresh; `Envelope` deserialize FQN resolve |
| `Envelope`            | per-call cost varies by schema; sample-scene fixture target 0.1 ms total per frame | 16 MiB scratch | every serialize / deserialize call site                      |
| `ReflectionBlob`      | 0 in shipping build (editor / tools only)                               | 4 MiB (tools build); 0 (shipping)                            | inspector and dump tools                                     |
| `AliasResolver`       | invoked at hot-reload only; 0 on hot path                               | 4 MiB            | phase 8 hot-reload barrier — `reconcile(...)` called by `core` |

Heap sub-ceilings sum: 8 (`SchemaRegistry` + `SchemaAliasTable`) + 16 (`Envelope` scratch) +
4 (`AliasResolver`) + 4 (`ReflectionBlob`, tools build only) =
32 MiB. The shipping build does not carry the `ReflectionBlob`
sub-ceiling; the 4 MiB it would occupy is reserved as `data`-context
slack within the 32 MiB row and is not counted against any other
aggregate.

### 9.3 Sample-scene fixture target

Under the `S1` fixture defined in `reviews/decisions/perf-budget.md`
(1 character + 200 props + 8 dynamic lights at 1920x1080), the
`Envelope` aggregate is the only `data` aggregate with measurable
per-frame cost. Steady-state S1 frames perform zero `Envelope`
serialize / deserialize operations on the hot path: persistent state
moves through `core`'s ECS storage, not through Flatbuffers wire form. The
per-frame Envelope budget is therefore reserved for incidental
serializations (e.g. snapshot capture for the editor's time-rewind
scrubber, save-on-checkpoint events) and capped at **0.1 ms total per
frame** across all call sites. Steady-state: ~0 ms; budget: 0.1 ms.

The remaining 0.10 ms of the 0.20 ms cell absorbs the `reconcile(...)`
handoff cost on a reload frame (S2): a single plugin's schema-set
continuity check and alias-table refresh runs inside phase 8, and
`data`'s portion of that 0.40 ms phase-8 budget is the
`reconcile(...)` pass over the registered schemas. Structural evolution
(absent fields → Flatbuffers defaults) is handled automatically by
zero-copy reads, so there is no per-row dispatch cost. This is one
frame per reload, not per frame.

### 9.4 CI gate — round-trip benchmarks

The `perf-budget.yml` workflow described in
`reviews/decisions/perf-budget.md` runs Catch2 `BENCHMARK` blocks under
`data/runtime/test/perf/` on every PR touching this context. Required
benchmarks:

1. **`envelope_round_trip_s1.bench.cpp`** — serializes then
   deserializes one of every persistent aggregate type registered for
   the S1 fixture, asserting wall-clock total `<= 0.10 ms` per
   1000-iteration window. Verifies the per-frame `Envelope` ceiling.
2. **`schema_registry_lookup.bench.cpp`** — looks up a representative
   set of `(fqn, version)` pairs from `SchemaRegistry`, asserting per-
   lookup `<= 10 ns` over a 100k-iteration window. Verifies the
   `SchemaRegistry` per-call invariant.
3. **`reconcile_continuity.bench.cpp`** — runs `reconcile(...)` for
   every registered schema against a synthetic incoming registry that
   bumps every FQN by one version, asserting total `<= 0.10 ms` over
   a 1000-iteration window. Establishes a continuity-check cost
   ceiling so a future schema growth does not silently blow phase-8
   past its 0.40 ms slot. Replaces `migration_dispatch_v_minus_1.bench.cpp`
   (Q1, ADR #1121 — no per-step dispatch; structural evolution is
   automatic).
4. **Heap ceiling.** A diagnostic build with
   `GLIBRE_ALLOC_STRICT=1` exercises the same fixtures and asserts
   that resident bytes under the `data` `ContextTag` never exceed 32
   MiB. Per Allocator Rules in `perf-budget.md`, exceeding the
   ceiling is an `OutOfBudget` `std::expected` arm, not a soft
   warning, in the gate-build configuration.

Round-trip identity (round-trip preserves bytes) is already a §7.5
acceptance criterion; the perf gate adds the *time* and *space*
contracts on top of it.

### 9.5 Allocation rules

`data` allocates exclusively through the `glibre::PerContextAllocator`
handle stamped at `glibre_plugin_register` time, tagged `data`. Per
the engine-wide rules:

1. `SchemaRegistry`'s 8 MiB is a static-init allocation: live for the
   entire process lifetime, freed at process exit. It does not pass
   through the per-frame transient arena.
2. `Envelope`'s 16 MiB scratch is a per-frame transient arena drained
   at phase 9; allocations leaking past the drain are an
   `OutOfBudget` "leak" arm per the engine allocator rules.
3. `AliasResolver`'s 4 MiB is a phase-8-only working set for the
   `reconcile(...)` call (alias-table diff + FQN continuity scan),
   allocated from the allocator owned by `core` (16 MiB ceiling inside
   `core`'s 64 MiB row); `data`'s 4 MiB sub-ceiling counts against
   `data`'s tag, not `core`'s. No arena is allocated — `reconcile(...)`
   is validation-only and makes no copies of storage rows.
4. `Sergeant` is a host build tool. Its memory consumption is not in the
   runtime budget; it is governed by build-graph CI runner limits
   (separate concern).
5. The `AbiHash` export returns a pointer to a `constexpr` string
   embedded in `glibre-types.dylib` — zero runtime allocation, zero
   heap accounting.

### 9.6 Headroom posture

`data` does not own a slice of the engine-wide 1.5 ms sim headroom
locked in `perf-budget.md`. Future per-frame work in this context (for
example, a `data`-side snapshot ring for time-rewind that runs every
frame instead of on demand) consumes the existing 0.20 ms cell first;
once that is full, an amendment to this section and to the engine
decision record is required before extending into headroom. The 4 MiB
of heap that the shipping build does not spend on `ReflectionBlob` is
not slack to be silently consumed; it is reserved against future
growth of `Envelope` scratch. A lazy alias-resolution cache is not part of
the MVP — per PHILOSOPHY's "two concrete users" rule, that abstraction
is deferred until two callers demand it; introducing one will require
a fresh sub-epic that re-amends this section and `perf-budget.md`.

## 10. Failure Modes & Error Model

The `data` context surfaces failure exclusively through a closed sum
type `glibre::types::data::Error`. Per
`reviews/decisions/error-model.md` §"Composition Rules" #1 the enum is
a leaf — no arm of `data::Error` nests another context's error type;
instead, callers translate at the boundary they cross. Per §"Type
Sketch" of the same record, `data::Error` rides into the engine-wide
`glibre::Error` variant once the data context's enum is appended to
the central `Error::Variant` alias in `core/include/glibre/error.hpp`.

Per `-fno-exceptions` engine policy (§"Decision" #3 of the error-model
record), every public `data` boundary in §5 returns `std::expected<T,
data::Error>` (or `std::expected<T, glibre::Error>` when migrations
need to surface non-data failure across the dispatcher boundary —
see §10.2 row "MigrationStepMissing").

§10 is the closed enumeration. New arms may be appended only via a
spec amendment that ships in the same PR as the new failure point;
removing or reordering an arm is an ABI-breaking change that follows
§4.3 inv. 3 (SONAME bump).

### 10.1 The closed sum

```cpp
// glibre-types public surface, lifted from §5 with payload-bearing
// arms made explicit. Tag values are stable across patch releases;
// removing or reordering an arm is an ABI break (§4.3 inv. 3).

namespace glibre::types::data {

// Wire-time location attached to DeserializeError / SchemaUnknown /
// EnvelopeTruncated.
// `offset` is the byte index *within the inbound payload* at which
// decoding stopped, measured from the start of the Flatbuffers
// size-prefix (§4.8 inv. 2). Non-payload arms set this to a sentinel — see §10.2.
struct WireSite {
    SchemaId      schema{};       // the FQN the envelope claimed
    SchemaVersion version{0};     // the version the envelope claimed
    std::uint32_t offset{0};      // byte index where decode failed
};

enum class ErrorTag : std::uint16_t {
    AbiHashMismatch          = 1,
    SchemaMigrationFailure   = 2,  // Q5 (ADR #1121): DEPRECATED for runtime migration use.
                                   // Per-step MigrationFn dispatch is dropped (Q1). This
                                   // arm is RETAINED for schema-set continuity refusals
                                   // (§8.2 step 1: FQN in outgoing missing from incoming)
                                   // and §8.3 gate 2. Final disposition deferred per
                                   // round-2 carry on #1122. DO NOT remove the arm.
    DeserializeError         = 3,
    ReservedTagViolation     = 4,  // Q5 (ADR #1121): KEEP — defense-in-depth at codegen.
    SchemaRegistryConflict   = 5,
    SchemaUnknown            = 6,
    MigrationStepMissing     = 7,  // Q5: semantically deprecated (no chain to have gaps
                                   // in), but RETAINED so existing core::Error wrappers
                                   // don't break. Fires if a save file carries a
                                   // schema_version that predates the registered schema.
    MigrationCycle           = 8,
    EnvelopeTruncated        = 9,  // Q5 (ADR #1121): KEEP — physical truncation detection.
    SourceHashMismatch       = 10,  // §3.8 of schema-registry-design.md: same FQN +
                                    // same version, byte-different SchemaSourceHash.
                                    // Distinct from AbiHashMismatch (arm 1), which
                                    // compares the global glibre_types_abi_hash; this
                                    // arm fires when only one FQN's source bytes drifted.
};

// Closed sum. Plain-aggregate layout so the C-ABI trampolines in §5
// can return it without dragging std::variant across the boundary.
struct Error {
    ErrorTag tag{};

    // Set on tags 3, 6, and 9; default-constructed on every other arm.
    WireSite at{};

    // Set on tags 2, 7, 8, and 10: identifies the migration step that
    // refused, the missing step in the chain, the back-edge of the
    // detected cycle (tags 2/7/8), or the drifted FQN (tag 10).
    // For tag 10 (SourceHashMismatch): step_schema = the drifted FQN;
    // step_from == step_to == 0 (no version change, only hash changed).
    // (from, to) are non-zero on arms 2, 7, 8; (from==0, to==0) on arm 10
    // only (no version change, only hash changed).
    SchemaId      step_schema{};
    SchemaVersion step_from{0};
    SchemaVersion step_to{0};

    // Set on tags 1 and 10: the host's compiled-in hash and the offending
    // plugin's compiled-in hash, hex form (§4.4 inv. 5). For arm 1 these
    // are the global glibre_types_abi_hash values; for arm 10 they are
    // the per-FQN SchemaSourceHash values (hex-encoded, §3.2 of
    // schema-registry-design.md). Empty string_views on every other arm.
    std::string_view host_hash{};
    std::string_view plugin_hash{};

    // Set on tag 4 (ReservedTagViolation): the tag number whose
    // reuse was attempted; 0 on every other arm.
    std::uint16_t reserved_tag{0};
};

}  // namespace glibre::types::data
```

The aggregate keeps every arm's payload contiguous and trivially
copyable so the C-ABI trampolines (§4.3 inv. 4) can return it through
`std::expected` without crossing a non-trivial type boundary. Arms
that do not use a particular field leave it default-constructed; the
table in §10.2 records exactly which payload fields each arm
populates.

### 10.2 Per-arm trigger / recovery / severity / mapping

Recovery vocabulary — terms used in the table column:

- **refuse load** — the loader at phase 8 logs `warn` and leaves the
  previous-good plugin live. No state mutation; observers see
  `core::Error::HotReloadRefused` carrying this `data::Error` in
  detail (`reviews/decisions/error-model.md` §"Hot-reload refusals";
  `reviews/decisions/plugin-abi.md` §"Loader Sequence" step 11).
- **abort plugin** — the loader runs the compensating un-register of
  any partial registration, `dlclose`s the candidate dylib, and
  surfaces `core::Error::PluginInitFailed`
  (`reviews/decisions/plugin-abi.md` §"Loader Sequence" step 9). The
  rest of the engine continues.
- **abort build** — codegen-time only. `glbr-sergeant` writes no
  output, exits non-zero, and CMake fails the configure / build step.
  No runtime path can observe this arm.
- **refuse decode** — `Envelope<T>::deserialize` returns
  `std::unexpected(...)`; the calling site decides whether to drop the
  payload, fall back to a default, or escalate. The world is not
  mutated.
- **report** — diagnostic surfacing only; the operation itself does
  not retry. Used for arms whose detection is informational rather
  than load-bearing (e.g. tools-build introspection).
- **process abort** — the engine cannot proceed without violating an
  invariant; `glibre::log_error(err, fatal)` precedes
  `std::abort()`. Reserved for arms whose detection means the build
  itself is broken (per §4.5 inv. 1, registry conflicts at static-init
  cannot be recovered from in-process).

Severity vocabulary mirrors `error-model.md` §"Logging / Telemetry"
levels: **fatal** (process abort), **error** (operation refused, log
at `error`), **warn** (hot-reload refusal — log at `warn` per the
error-model rule), **info** (codegen / tools diagnostic).

| Arm | Trigger | Detection point | Payload fields | Recovery | Severity | core::Error mapping |
|-----|---------|-----------------|----------------|----------|----------|---------------------|
| `AbiHashMismatch` | A loaded plugin's compiled-in `glibre_plugin_abi_hash` byte-string differs from the host's `glibre_types_abi_hash()` (§4.4 inv. 3). | `core` plugin loader, step 4 (`reviews/decisions/plugin-abi.md` §"Loader Sequence"). | `host_hash`, `plugin_hash`. | refuse load — `dlclose` the candidate, log at `warn`, leave previous-good plugin live. | warn | `core::Error::PluginAbiHashMismatch`. The data arm carries the two hex strings; the core arm wraps it via `ErrorContext::detail`. |
| `SchemaMigrationFailure` | The schema-set continuity check at `reconcile(...)` §8.2 step 1 found an `FQN` registered in `outgoing` but missing from `incoming` (not resolvable via type-rename alias), OR the meta-schema bootstrap rule (§8.3 gate 2) refused a Mode-B middleman swap. Per-step `MigrationFn` dispatch is DROPPED (Q1); this arm is RETAINED for continuity refusals only (Q5, §10.1). | `reconcile(...)` step 1 (§8.2); §8.3 gate 2. | `step_schema`, `step_from`, `step_to`. | At hot-reload: refuse load — `reconcile(...)` is validation-only; no storage rows are mutated; the loader un-swaps the vtable. | warn (hot-reload). | `core::Error::SchemaMigrationFailed` (`reviews/decisions/plugin-abi.md` §"Failure Modes" row 11). The data arm carries the `(FQN, from, to)` triple; the core arm wraps. |
| `DeserializeError` | Inbound payload's `SchemaVersion` exceeds the live registry's current version for the same `FQN` (§4.10 inv. 2 — newer-than-host), OR the payload bytes failed Flatbuffers's per-tag decode for any reason other than truncation (e.g. tag-type mismatch, builtin range-check failure, nested-type decode refusal). | `Envelope<T>::deserialize` body (§4.8 inv. 5); generated `glibre_types_deserialize_<fqn>` trampoline (§4.3 inv. 2). | `at.schema`, `at.version`, `at.offset`. | refuse decode — caller decides drop / default / escalate. No partial value escapes. | error | `core::Error` has no dedicated arm; callers that wish to surface to the engine-wide variant pass the `data::Error` through unchanged (a `data::Error` is a leaf variant arm of `glibre::Error` per error-model.md §"Type Sketch"). |
| `ReservedTagViolation` | A `.fbs` source file under `data/schemas/` reuses a tag number that the prior committed version of the same `FQN` retired into the `reserved` set (§4.1 inv. 3). Codegen-time only; runtime cannot observe. | `glbr-sergeant` reserved-tag enforcement (§4.2 inv. 4). | `step_schema` (the offending FQN), `reserved_tag` (the reused tag number). | abort build — no output written; CMake configure / build fails. | info (codegen diagnostic; promoted to build error by the host tool's exit code). | None — codegen-time arm; never crosses into the runtime engine. Listed in the closed sum so codegen surfaces a typed enumerator alongside its message rather than a free-form string. |
| `SchemaRegistryConflict` | Two registry entries share an `FQN` (§4.5 inv. 1). At codegen time this is impossible (§4.10 inv. 1 biconditional); at static-init this fires when two distinct middleman builds load into one process (§4.3 inv. 1). | Middleman static-init (§4.3 inv. 5); also reachable from §8.3 Mode-B reload if the new `Q-types` registry duplicates an `FQN` introduced by an outgoing plugin. | `step_schema` (the duplicated FQN). | process abort at static-init (§4.3 inv. 1 makes a two-middleman process undefined; the spine refuses to run). At Mode-B hot-reload: refuse load — un-swap the candidate registry, leave `P-types` live. | fatal (static-init) / warn (Mode-B). | `core::Error` has no dedicated arm; the loader at Mode-B wraps via `core::Error::HotReloadRefused`. The static-init path is a fatal log + `std::abort`. |
| `SchemaUnknown` | An inbound payload's envelope `FQN` is not present in the live `SchemaRegistry` and is not resolvable via the `SchemaAliasTable`. Fires when a save file or world snapshot contains a type the current build does not register, OR when a plugin attempts to deserialize bytes for a `Generated Type` whose schema dropped between Q and P (§8.1 "State that does not survive"). | `Envelope<T>::deserialize` envelope-read step (§4.8 inv. 5); `reconcile(...)` schema-set continuity check (§8.2 step 1) — the continuity arm raises `SchemaMigrationFailure` instead per §8.4, so `SchemaUnknown` only fires at the *deserialize* path, not the *reconcile* path. | `at.schema`, `at.version`, `at.offset = 0` (the failure is at the Flatbuffers size-prefix / file_identifier, not inside the payload). | refuse decode — caller decides drop / default / escalate. Save-file loaders typically translate to a "skip unknown record" warning; per-frame deserializers escalate. | error | `core::Error` has no dedicated arm; the engine-wide variant carries the `data::Error` through. (Distinct from `SchemaMigrationFailure`: that arm fires only when the FQN *is* known and the continuity check refused; this arm fires when the FQN is not known at all.) |
| `MigrationStepMissing` | A save file or snapshot carries a `schema_version` field inside the root table that is older than the minimum version the current build's `SchemaAliasTable` can resolve. No `MigrationChain` dispatch exists (Q1: chain is DROPPED); the arm fires exclusively at the deserialize path when the inbound version predates all registered schemas for the FQN. RETAINED for ABI continuity per Q5 (existing `core::Error` wrappers). | `Envelope<T>::deserialize` (§4.8 inv. 5) — fires when `schema_version` field is older than the type's registered minimum. | `step_schema`, `step_from` (the inbound version), `step_to` (the registered version). | refuse decode — caller decides drop / re-cook via `glibre-cook`. | error | Wrapped by `core::Error::SchemaMigrationFailed` when surfaced through the loader (`reviews/decisions/plugin-abi.md` §"Failure Modes" row 11). RETAINED per Q5 so existing `core::Error` wrappers and operator tooling continue to function. |
| `MigrationCycle` | Detected at static-init time when a `glibre_types_register_fqn_alias` call would produce a cycle in the type-rename forward-map (§4.7 inv. 2 — one-hop max, acyclic). RETAINED for ABI continuity per Q5. | `glibre_types_register_fqn_alias` call at static-init (§4.7 inv. 2). | `step_schema`, `step_from = 0`, `step_to = 0` — the alias pair that would close a cycle. | process abort — a registered cycle means the alias table is corrupt; mirroring `SchemaRegistryConflict`. | fatal (static-init). | None — static-init arm; never crosses into normal runtime. The static-init fatal path is logged + `std::abort`. |
| `EnvelopeTruncated` | The inbound byte span is shorter than the Flatbuffers size-prefix (4 bytes), OR `flatbuffers::Verifier` rejects the buffer as physically incomplete. (Distinct from `DeserializeError`: that arm covers structurally-complete-but-semantically-invalid payloads; this arm covers physically-incomplete byte runs.) (Q5: KEEP — physical truncation detection.) | `Envelope<T>::deserialize` envelope-read step (§4.8 inv. 1, 2) via `flatbuffers::Verifier`. | `at.schema` (default-constructed if truncation occurs before the `file_identifier` is readable), `at.version` (likewise), `at.offset` (the byte count that *was* readable before truncation). | refuse decode — caller drops / re-requests. World untouched. | error | `core::Error` has no dedicated arm; passed through. Save-file readers surface this as a "truncated record" diagnostic and continue past the next valid size-prefix; per-frame deserializers escalate. |
| `SourceHashMismatch` | An existing `(FQN, version)` pair appears in a candidate plugin with a byte-different `SchemaSourceHash` (i.e. the `.bfbs` bytes for that type changed without a version bump — Q7). Distinct from `AbiHashMismatch` (arm 1), which fires on a global `glibre_types_abi_hash` mismatch; this arm fires when only one FQN's `.bfbs` bytes drifted. | Mode-A barrier diff at phase-8 step 2. | `step_schema` (the drifted FQN), `host_hash` (live registry's hex-encoded `SchemaSourceHash`), `plugin_hash` (incoming candidate's hex-encoded `SchemaSourceHash`). `step_from == step_to == 0` (no version change). | refuse load — log at `warn`; leave previous-good plugin live. The operator must version-bump the schema and add a structural evolution entry (§7.4) rather than recompile only (the remediation differs from arm 1). | warn | `core::Error::PluginAbiHashMismatch`. Both arm 1 and arm 10 wrap into the same `core::Error` arm because the loader's response is identical (refuse, log warn, leave live); the `data::Error` distinction gives operators the per-FQN detail they need to diagnose the root cause. |

### 10.3 Composition with `core::Error`

Per `reviews/decisions/error-model.md` §"Composition Rules" #2 every
arm above is a leaf in `data`'s context. Five arms have a dedicated
`core::Error` wrapping (rows 1, 2, 5, 7, 10) because the `core` plugin
loader is the call site that raises them on `data`'s behalf; the
remaining five surface to outer contexts by passing the `data::Error`
through the `glibre::Error` variant unchanged. The mapping is fixed at
the loader's call sites:

| `data::Error` arm | When `core` raises it | `core::Error` wrapping |
|-------------------|-----------------------|------------------------|
| `AbiHashMismatch` | Phase-8 plugin load step 4. | `PluginAbiHashMismatch` (`plugin-abi.md` §"Failure Modes" row 4). |
| `SchemaMigrationFailure` | Phase-8 `reconcile(...)` step 1 (FQN missing from incoming); §8.3 gate 2 (Mode-B continuity). | `SchemaMigrationFailed` (`plugin-abi.md` §"Failure Modes" row 11). |
| `MigrationStepMissing` | `Envelope<T>::deserialize` — inbound `schema_version` older than registered minimum (save file from older build). RETAINED per Q5. | `SchemaMigrationFailed` — same wrap as `SchemaMigrationFailure`; the loader's response (refuse, log, leave previous-good live) is identical for both arms. |
| `SchemaRegistryConflict` | Mode-B reload only; the static-init path is fatal and never reaches the loader. | `HotReloadRefused` carrying the `data::Error` in `ErrorContext::detail`. |
| `MigrationCycle` | Never — cycles are codegen-time or static-init time exclusively. | None. |
| `SourceHashMismatch` | Phase-8 barrier diff step 2 (Mode-A only); `schema-registry-design.md` §8.1. | `PluginAbiHashMismatch` — same wrapping as arm 1 (`AbiHashMismatch`); the loader's response (refuse load, log warn, leave previous-good live) is identical. The `data::Error` arm carries the per-FQN detail; the `core::Error` arm provides a uniform hot-reload-refusal surface to callers above `core`. |
| `DeserializeError`, `EnvelopeTruncated`, `ReservedTagViolation`, `SchemaUnknown` | Surface only at the `data` layer; `core` does not wrap them. | None — they appear in `glibre::Error::Variant` as the `data::Error` arm directly. |

Two arms collapse onto one `core::Error::SchemaMigrationFailed`
(`SchemaMigrationFailure` and `MigrationStepMissing`) per the
hot-reload protocol's choice to treat schema-set continuity gaps and
version-too-old payloads identically from the loader's perspective.
The data layer keeps the distinction because operators reading logs
benefit from knowing *why* the refusal fired; the loader does not,
because its response is identical either way. This is the
§"Composition Rules" #2 rule applied honestly: cross-context
translation is local, explicit, and unit-tested at the loader's
call site.

### 10.4 Logging discipline

Per `reviews/decisions/error-model.md` §"Logging / Telemetry" #1
every constructed `data::Error` is logged exactly once at the
boundary that *handles* it — never at the boundary that *raises* it.
The data context never logs from inside `Envelope<T>::deserialize`
or `reconcile(...)`. The handler — which
is either:

- the `core` plugin loader (for arms 1, 2, 5, 7, 10), or
- the calling context's deserialize site (for arms 3, 6, 9), or
- `glbr-sergeant` itself (for arm 4 codegen path), or
- the middleman's static-init (for arms 5, 8 static-init path)

— is responsible for the single `glibre::log_error(err, level)` call.
The structured fields the handler emits are the ones the arm's
payload populates (table §10.2): never empty strings, never
default-constructed sentinels masquerading as data. The handler
that calls `log_error` for an arm whose payload field is unset
omits that key/value pair entirely — `spdlog` records absence
rather than a misleading zero.

`spdlog` level mapping (mirrors §10.2 severity column):

- **fatal** → `spdlog::level::critical`, followed by `std::abort()`.
- **warn** → `spdlog::level::warn` (hot-reload refusals; the
  previous-good plugin keeps running).
- **error** → `spdlog::level::err` (operation refused, world
  untouched).
- **info** → `spdlog::level::info` (codegen and tools diagnostics).

### 10.5 Test coverage obligations

Every arm in §10.1 carries at least one Catch2 case under
`tests/data/errors/` that constructs the failure deterministically and
asserts on the payload fields the arm populates. The fixtures back
the §11 acceptance criteria.

| Arm | Fixture |
|-----|---------|
| `AbiHashMismatch` | Two middleman builds with one schema bytewise different; the test loads a plugin built against build A into a host running build B and asserts `host_hash != plugin_hash`. |
| `SchemaMigrationFailure` | The `force_continuity_failure` test hook from §8.6; asserts that `reconcile(...)` returns this arm with `step_schema` = the forcibly-missing FQN. |
| `DeserializeError` | A hand-rolled byte buffer with a tag-type mismatch in a known schema; assertion on `at.offset` matches the byte index of the offending tag. |
| `ReservedTagViolation` | A `.fbs` source under `tests/data/schemas/golden/reserved_tag_reuse/` that reuses a previously-committed tag number; assertion on `Sergeant`'s exit code and the `reserved_tag` payload field in the codegen-emitted diagnostic. |
| `SchemaRegistryConflict` | A test-only middleman build that registers two schemas under the same FQN; assertion on `step_schema` and on the `std::abort` path via a death-test. |
| `SchemaUnknown` | An envelope whose `FQN` is `glibre.test.NeverRegistered`; assertion on `at.schema == "glibre.test.NeverRegistered"`. |
| `MigrationStepMissing` | A hand-rolled save-file buffer whose `schema_version` field in the root table is older than the registered minimum for the FQN; assert `step_from` == inbound version, `step_to` == registered version. RETAINED arm per Q5. |
| `MigrationCycle` | A test-only static-init sequence that registers two `glibre_types_register_fqn_alias` calls that would form a cycle (`A → B`, `B → A`); assertion on the process-abort death-test and the `step_schema` payload (the alias pair that closes the cycle). |
| `EnvelopeTruncated` | A `std::span<const std::byte>` shorter than 4 bytes (below Flatbuffers size-prefix minimum); assertion on `at.offset == src.size()`. (Q5: KEEP.) |
| `SourceHashMismatch` | A barrier diff against a candidate middleman carrying the same `(FQN, version)` as the live registry but with a byte-different `SchemaSourceHash` (simulate by patching one byte of the source-hash literal in a test-only `_registry.cpp`); assertions: (a) the loader returns `core::Error::PluginAbiHashMismatch`, (b) the `data::Error` detail has `tag == SourceHashMismatch`, `step_schema == drifted_fqn`, `host_hash != plugin_hash`, `step_from == step_to == 0`. Location: `tests/data/errors/schema_registry_arms_test.cpp` (aligns with §11.4 of `schema-registry-design.md`). |

Each fixture asserts both (a) the correct arm fires and (b) the
payload fields it claims to populate are non-default. Arms whose
payload includes optional spans assert empty spans for the unset
case, never null pointers.

## 11. Acceptance Criteria

GitHub `type:user-story` issues this spec closes (drafted under spike
#63; each carries a Catch2 test name plus the story-required E2E
`.glibre-trace`):

- #362 — data/schema: author persistent type via `.fbs` schema file (§4.1, §4.2, §7.1) — pts:3
- #363 — data/sergeant: enforce tag-sort layout-additive rule (§4.2 inv #3) — pts:3
- #364 — data/sergeant: deterministic, host-stable codegen (§4.2 inv #1, #5) — pts:2
- #365 — data/abi-hash: plugin loader refuses dylibs whose ABI hash mismatches (§4.4 inv #3, §4.10 inv #3) — pts:3
- #366 — data/abi-hash: `glibre_types_abi_hash` reproducible from sources (§4.4 inv #1, #2, #4) — pts:2
- #367 — data/envelope: serialize/deserialize round-trip byte-equal across hosts (§4.8 inv #4, §4.10 inv #6) — pts:3
- #368 — data/alias-table: `SchemaAliasTable` resolves type-rename and field-rename aliases in one hop (§4.7) — pts:5
- #369 — data/alias-table: per-schema structural evolution golden harness (§7.5; §9.4) — pts:3
- #370 — data/manifest: plugins ship Flatbuffers-serialized `PluginManifest` in `.rodata` (§5 plugin_manifest.hpp; §6.5) — pts:5
- #371 — data/hot-reload: `reconcile(...)` verifies schema-set continuity and refreshes alias table at frame-8 (§8.2) — pts:5
- #372 — data/hot-reload: phase-8 budget within 0.20 ms / 4 MiB on S1 (§9.1, §9.2, §9.5) — pts:3
- #373 — data/reflection: `ReflectionBlob` present in editor, stripped from shipping (§4.9; PHILOSOPHY §6) — pts:3
- #374 — data/registry: `SchemaRegistry` refuses duplicate FQN, `O(log N)` lookup (§4.5 inv #1, #3; §9.4) — pts:2

Total: 13 stories, 42 pts roll up into sub-epic #53.

Each must have a Catch2 test by name.

## 12. Open Questions

None. All MVP-blocking questions are resolved in §1–§11 of this spec
or in the decision records cited there
(`reviews/decisions/{flatbuffers-codegen,plugin-abi,perf-budget,hot-reload-protocol,frame-phases,error-model}.md`).
Forward-looking, post-MVP design questions (e.g. a `data`-side
lazy-migration cache) are deferred per PHILOSOPHY's
"two concrete users" rule and will be reopened only via a fresh
sub-epic that re-amends the affected section.
