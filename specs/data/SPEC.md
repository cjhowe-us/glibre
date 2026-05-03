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

The `data` context decomposes into nine SRP-bounded aggregates. Each owns
exactly one reason to change; together they form the type-and-schema spine
described in §1. Aggregates are listed in dependency order: each consumes
only what precedes it. Invariants are stated as predicates that hold at
every public API boundary; violation of any predicate is a defect, not a
runtime branch.

### 4.1 `Schema` (authoring aggregate)

**Owns:** the in-memory representation of one `data/schemas/<ctx>/<Type>.fory`
file — its `FQN`, monotonically-increasing `SchemaVersion`, ordered set of
`Tag`s (each with field name, builtin/generated type reference, `since`
version, optional default), and the set of reserved `Tag`s. Equivalence
class: one `Schema` per `(FQN, SchemaVersion)`.

**Reason to change:** the `.fory` file format itself — adding a builtin,
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

### 4.2 `Foryc` (codegen-tool aggregate)

**Owns:** the host-only `glibre-foryc` executable: parsing `.fory` files
into `Schema` values, validating each `Schema` against the invariants in
§4.1, emitting tag-sorted `Generated Type` headers/sources, the
`_registry.cpp` and `_abi_hash.cpp` companions, and a stamp file the
CMake graph depends on. `Foryc` is the only writer of generated middleman
sources.

**Reason to change:** the codegen output format — generated struct shape,
the registry table layout, the dispatcher emission strategy. The schema
language and the hash function are owned elsewhere (§4.1, §4.4).

**Invariants:**

1. Output is a pure function of the input schema set: identical input
   bytes plus identical `glibre-foryc` binary produce identical generated
   sources, byte-equal across hosts (PHILOSOPHY §7).
2. Every `Generated Type` is `final`, vtable-free, has `= default` ctors,
   contains only builtins or other generated types, and has its fields
   ordered by ascending `Tag` (not declaration order). `Foryc` asserts
   `std::is_trivially_copyable_v<T>` whenever the field set permits and
   fails the build otherwise on schemas that claim that profile.
3. Layout-additive rule: a new tag is permitted only if its sorted
   position appends past the offset of the prior version's last field;
   any other change forces a `SchemaVersion` bump and a registered
   `Migration`. `Foryc` proves this property at codegen time and refuses
   to emit on violation.
4. Reserved-tag enforcement is machine-checked: comparing the in-tree
   `Schema` against its prior committed version, `Foryc` errors on any
   reuse of a number ever shipped.
5. `Foryc` never opens the network, never reads paths outside the
   declared schema root and the configured output directory, and
   allocates only inside an arena it owns. Determinism does not depend
   on filesystem iteration order — schema files are sorted by `FQN` before
   processing.

### 4.3 `Middleman` (`glibre-types.dylib` aggregate)

**Owns:** the single shared library that holds every `Generated Type`,
the populated `SchemaRegistry`, the migration dispatcher, the embedded
`AbiHash`, and the C-stable entry-point table. The middleman is the
*only* ABI surface plugins link; no plugin links Apache Fory directly.

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
   `glibre_types_register_migration_<fqn>`. Their signatures are stable
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

1. `AbiHash := blake3( concat( sort_by_fqn( { schema_source_hash(s) :
   s ∈ Schemas } ) ) )`. Sort key is the lexicographic byte order of
   `FQN`; concatenation has no separators because each `schema_source_hash`
   is fixed-width (Blake3 256-bit). The result is a 32-byte digest.
2. `schema_source_hash(s) := blake3( canonicalize(s) )` where
   `canonicalize` is the rule defined in §4.1 (sorted tags, normalized
   whitespace, defaults in tag order).
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
deserialize_fn, MigrationChain, ReflectionBlob)`. Populated at static-init
time by codegen-emitted register calls; no entries are added or removed
at runtime.

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
   reconciled by `Foryc` at configure time.

### 4.6 `Migration` (single-step aggregate)

**Owns:** one pure function
`migrate_<Type>_v<N>_to_v<N+1>(const VN&, VNplus1&, Arena&) ->
std::expected<void, glibre::Error>`, written by the originating context
and registered through a codegen-emitted macro. One `Migration` per
`(FQN, N → N+1)` pair.

**Reason to change:** the body of the migration — semantic translation
between two versions of one type. The dispatch protocol and the per-type
function-table layout live in their own aggregates (§4.7, §4.5).

**Invariants:**

1. Determinism: identical inputs produce byte-equal outputs across hosts
   and runs. No reads of wall clock, RNG state, environment, locale,
   or filesystem.
2. Allocation discipline: the migration may allocate only within the
   supplied `Arena&`. No global heap calls, no STL containers that
   default-construct allocators, no I/O of any kind.
3. Totality: the function is total over every value of `VN` produced by
   `deserialize_v<N>`. Returning `std::unexpected` is reserved for
   *defective* payloads (e.g. a foreign-key tag pointing to a missing
   sibling); domain-valid `VN` instances must always yield a `VNplus1`.
4. Locality: the migration touches only fields of `VN` and `VNplus1`;
   it does not consult the `SchemaRegistry`, does not call into other
   contexts' code, and does not read or mutate global state.
5. Single-step shape: there is no `vN → vN+2` migration. Multi-step
   migrations are composed exclusively by `MigrationChain` (§4.7), so
   the per-step contract stays minimal.

### 4.7 `MigrationChain` (composition aggregate)

**Owns:** the per-`FQN` sequence of `Migration`s the dispatcher composes
when an inbound payload's `SchemaVersion` is less than the current
version. Stored inside the registry entry as a contiguous function table
indexed by `(N → N+1)`.

**Reason to change:** the dispatch protocol — how partial chains are
detected, how arena memory is recycled between steps, how chain failure
is reported.

**Invariants:**

1. Coverage: for every `FQN` whose current version is `M`, the chain
   contains exactly one `Migration` for each step `1→2, 2→3, …, M-1→M`.
   Missing any step is a codegen-time build error; the partial chain
   never ships.
2. Application order: the dispatcher applies `Migration`s strictly in
   ascending order with no parallelism and no skipping; `vN → vM`
   composes through every intermediate.
3. Single-pass semantics: a chain is applied at most once per inbound
   payload. The output is yielded as `VM` directly to the caller; no
   intermediate version escapes the dispatcher.
4. Failure isolation: a single `Migration` returning `std::unexpected`
   stops the chain immediately; the dispatcher returns
   `data::Error::SchemaMigrationFailure` carrying the failing
   `(FQN, N → N+1)` pair. No partial mutation is observable to the
   caller (the destination value is not exposed on failure).
5. Arena recycling: between steps the dispatcher resets the per-payload
   arena to its initial high-water mark. Cross-step retention is
   forbidden; each `Migration` writes into a fresh `VNplus1` and reads
   only its `VN` source.

### 4.8 `Envelope` (wire-form aggregate)

**Owns:** the Fory-defined header `(FQN, SchemaVersion, payload_length,
flags)` prepended to every persistent payload. Read first by every
`deserialize_<fqn>` to dispatch decode and migration; written first by
every `serialize_<fqn>`.

**Reason to change:** the wire-level header layout — adding a flag bit,
extending the version width, introducing a new envelope tag. Payload
encoding lives inside the generated serializers.

**Invariants:**

1. Self-describing: the envelope is sufficient to identify the type and
   version of any persistent byte sequence in the engine. No external
   schema discovery is required at deserialize time.
2. Fixed prefix: the envelope occupies a deterministic byte-count prefix
   of the payload; the suffix is the Fory-encoded body of the
   `Generated Type`. Readers may peek the envelope without consuming
   the body.
3. Byte order: little-endian for all integer fields. Fixed across all
   hosts (PHILOSOPHY §7).
4. Round-trip identity: for every `T` and every legal value `t : T`,
   `deserialize<T>(serialize<T>(t)) == t` byte-for-byte after
   re-serialization. Tested per-schema via Catch2 goldens under
   `tests/data/schemas/<ctx>/<Type>.cpp`.
5. Version-driven dispatch: `deserialize` reads the envelope, looks up
   the registry entry by `FQN`, compares `SchemaVersion` to the entry's
   current version, and either decodes directly or runs the
   `MigrationChain`. The envelope is the single source of dispatch
   truth — no out-of-band hints.

### 4.9 `ReflectionBlob` (introspection aggregate)

**Owns:** the compact, codegen-emitted descriptor of one `Generated
Type`'s fields and tags — name strings, tag numbers, builtin or generated
type references, `since` versions — embedded in the registry entry and
queryable from tools and editor builds.

**Reason to change:** the descriptor's record layout — what fields are
described, how name strings are interned, how nested generated-type
references are resolved.

**Invariants:**

1. Static-only: the blob is data the codegen wrote, not a runtime
   reflection facility. Shipping builds may include it for editor /
   tools support but never use it to dispatch behavior on the hot path
   (PHILOSOPHY §6).
2. Read-only: the blob is `constexpr` where the field set permits and
   `const` otherwise; no API mutates it. Editor edits to a `.fory` file
   regenerate the middleman; the live blob is never patched in place.
3. Faithfulness: every active tag in the source `Schema` has exactly one
   entry in its `ReflectionBlob`; reserved tags do not appear. The blob
   is sorted by ascending tag, matching the generated struct's field
   order.
4. Self-contained: name strings live in a per-type interning table
   inside the blob — the blob does not borrow strings from outside the
   middleman, so editor introspection works without re-loading source
   `.fory` files.
5. Stripped optionality: a build flag (off by default in shipping
   profiles, on for editor / tools) controls inclusion of the blob. When
   stripped, the registry entry holds a null pointer in the
   `ReflectionBlob` slot and editor-only callers handle the null
   explicitly; the runtime hot path never queries the slot.

### 4.10 Cross-aggregate invariants

These hold across the spine and are the load-bearing guarantees the data
context promises every consumer:

1. **Persistence ⇔ Schema:** every type whose instances cross a save,
   hot-reload, or plugin-dylib boundary has exactly one `Schema` and
   exactly one `SchemaRegistry` entry; conversely, every registry
   entry corresponds to a real `.fory` file in `data/schemas/`. The
   biconditional is enforced at configure time by `Foryc` and at
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
4. **Single dylib boundary:** plugins do not link Apache Fory, do not
   link `blake3`, and do not include any header the codegen wrote
   except via `glibre-types.dylib`. The middleman is the only ABI
   surface plugins link.
5. **Total migration coverage:** for every `FQN` whose current version
   is `M ≥ 2`, a `MigrationChain` of length `M-1` exists and every
   step is total over its prior version (§4.6 inv. 3). Builds that
   fail this fail at codegen, never at runtime.
6. **Determinism inheritance:** the spine adds no nondeterminism over
   its inputs — `Foryc` is deterministic, `AbiHash` is deterministic,
   `serialize`/`deserialize`/`MigrationChain` are deterministic,
   `Envelope` is endian-pinned. Snapshots round-trip byte-equal across
   hosts (PHILOSOPHY §7).
7. **No cross-aggregate mutation:** each aggregate is mutated only by
   its owner. Domain plugins write `Migration` bodies but never edit
   the registry, the dispatcher, the envelope, the abi-hash, or the
   reflection blob. The data context owns plumbing; domain contexts
   own their type's bytes.

## 5. Public Interface

The public interface of the `data` context is the surface exported by the
`glibre-types.dylib` middleman (§4.3) plus the per-type generated headers
emitted by `glibre-foryc` (§4.2). Every plugin and every host binary that
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
//   include/glibre/types/envelope.hpp      — EnvelopeHeader, Envelope<T>
//   include/glibre/types/migration.hpp     — MigrationFn, MigrationEntry,
//                                            register entry-point
//   include/glibre/types/registry.hpp      — RegistryEntry, SchemaRegistry
//   include/glibre/types/abi_hash.hpp      — glibre_types_abi_hash()
//   include/glibre/types/plugin_manifest.hpp — PluginManifest + sub-types
//   include/glibre/types/reflection.hpp    — ReflectionBlob (editor-only)
//
// The stub below is the union, with section banners matching the
// per-file split. § references point at this spec.

#pragma once

#include <array>
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

// Blake3-256 of the canonicalized .fory bytes (§4.4 inv. 2).
using SchemaSourceHash = std::array<std::byte, 32>;

// ---- error.hpp ----------------------------------------------------------

// Closed sum of every failure the data context can raise at a public
// boundary (§10). Per error-model.md §"Composition Rules" #2 callers
// translate these into their own context's enum at the call site.
namespace data {
enum class Error : std::uint16_t {
    AbiHashMismatch,         // §4.4 inv. 3 — hash compared at plugin load
    SchemaMigrationFailure,  // §4.7 inv. 4 — chain step returned unexpected
    DeserializeError,        // §4.8 inv. 5 — newer-than-host or malformed
    ReservedTagViolation,    // §4.1 inv. 3, §4.2 inv. 4 — codegen-time
    SchemaRegistryConflict,  // §4.5 inv. 1 — duplicate FQN at static-init
};
}  // namespace data

// ---- reflection.hpp (editor / tools only) -------------------------------

// Compact, codegen-emitted descriptor of one generated type's tags
// (§4.9). Read-only; never used to dispatch behavior on the hot path
// (PHILOSOPHY §6, §4.9 inv. 1). Stripped to nullptr in shipping builds
// (§4.9 inv. 5).
struct ReflectionField {
    std::string_view name{};
    std::uint16_t    tag{0};
    std::string_view type_name{};   // builtin or another FQN
    SchemaVersion    since{0};
};

struct ReflectionBlob {
    SchemaId                          schema{};
    SchemaVersion                     version{0};
    std::span<const ReflectionField>  fields{};   // tag-sorted ascending
};

// ---- envelope.hpp -------------------------------------------------------

// Fixed-width Fory envelope prefix. Self-describing, little-endian
// (§4.8 inv. 1, 3); peekable without consuming the payload (§4.8 inv. 2).
struct EnvelopeHeader {
    SchemaId      schema{};
    SchemaVersion version{0};
    std::uint32_t payload_length{0};
    std::uint32_t flags{0};            // reserved for future use
};

// Typed wrappers around the per-FQN extern "C" trampolines emitted by
// codegen (§4.3 inv. 4). Specializations live in each generated
// <glibre/types/<ctx>/<Type>.hpp>; this primary template is left
// undefined so misuse is a link-time error rather than a runtime one.
template <class T>
struct Envelope {
    // Write envelope + payload into `dst`; returns bytes_written.
    // Failure path is `data::Error::DeserializeError`-shaped only when
    // `dst` is too small (size queryable via the registry).
    static auto serialize(const T& value,
                          std::span<std::byte> dst) noexcept
        -> std::expected<std::size_t, data::Error>;

    // Read envelope, dispatch by SchemaVersion, and run MigrationChain
    // when the inbound version is older (§4.7). Newer-than-host ⇒
    // data::Error::DeserializeError; chain failure ⇒
    // data::Error::SchemaMigrationFailure.
    static auto deserialize(std::span<const std::byte> src) noexcept
        -> std::expected<T, data::Error>;
};

// ---- migration.hpp ------------------------------------------------------

// Per-payload arena owned by the dispatcher (§4.6 inv. 2). Migration
// bodies allocate only here; treated as opaque by user code.
class Arena;

// Single-step migration body, written by the originating context.
// Pure, deterministic, total over deserialize_v<N> outputs (§4.6).
template <class VN, class VNplus1>
using MigrationFn =
    auto (*)(const VN& src, VNplus1& dst, Arena& arena) noexcept
        -> std::expected<void, ::glibre::Error>;

// Type-erased migration record stored in the registry (§4.7).
// Adjacent records form one FQN's MigrationChain, indexed by
// from_version ascending.
struct MigrationEntry {
    SchemaVersion from_version{0};
    SchemaVersion to_version{0};
    // Erased trampoline; codegen casts back to the typed
    // MigrationFn<VN, VNplus1> at registration time.
    void (*invoke)(const void* src, void* dst, Arena& arena,
                   ::glibre::Error* out_err) noexcept = nullptr;
};

// Plain-enum return so the C-ABI boundary stays free of std::expected.
// The C++ wrapper macro lifts this into `Result<void>` for callers.
namespace data {
enum class RegisterStatus : std::uint16_t {
    Ok = 0,
    SchemaRegistryConflict =
        static_cast<std::uint16_t>(Error::SchemaRegistryConflict),
};
}  // namespace data

// Codegen-emitted entry-point. The owning context calls this at
// static-init (§4.3 inv. 5) via the
// `GLIBRE_REGISTER_MIGRATION(<Type>, <N>, <N+1>, <fn>)` macro placed in
// the generated `<Type>_migrations.hpp` companion header.
extern "C" auto glibre_types_register_migration(
    SchemaId       schema,
    MigrationEntry entry) noexcept -> data::RegisterStatus;

// ---- registry.hpp -------------------------------------------------------

// Read-only entry produced by codegen and inserted at static-init
// (§4.5). Layout is fixed; new fields are appended only.
struct RegistryEntry {
    SchemaId         schema{};
    SchemaVersion    version{0};
    SchemaSourceHash source_hash{};

    // Type-erased serialize/deserialize trampolines; the typed
    // Envelope<T> specializations resolve to these at link time
    // (§4.3 inv. 4).
    std::expected<std::size_t, data::Error> (*serialize)(
        const void* value, std::span<std::byte> dst) noexcept = nullptr;
    std::expected<void, data::Error> (*deserialize)(
        std::span<const std::byte> src,
        void* out_value) noexcept = nullptr;

    std::span<const MigrationEntry> migrations{};
    const ReflectionBlob*           reflection{nullptr};  // null in ship
};

class SchemaRegistry {
public:
    // Binary search by FQN (§4.5 inv. 3). O(log N), allocation-free.
    // Returns nullptr if no entry matches.
    auto lookup(SchemaId schema) const noexcept -> const RegistryEntry*;

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

// Wire-shape of `plugin.fory`; codegen-serialized into the plugin's
// .rodata, deserialized by the loader before invoking any plugin C++
// code. Mirrors reviews/decisions/plugin-abi.md §"Plugin Manifest
// Schema" verbatim. PluginManifest itself is a Fory-versioned schema
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

### 5.2 Serialized schemas (Fory)

Persistent types are authored as `data/schemas/<ctx>/<Type>.fory` files
(§4.1; format defined in `reviews/decisions/fory-codegen.md` §"Schema
File Format"). The schemas the data context itself owns — referenced by
the C++ structs above — are:

- `data/schemas/core/PluginManifest.fory` — the `PluginManifest` /
  `SemVer` / `ComponentDecl` / `SystemDecl` / `PassDecl` / `PanelDecl`
  bundle declared in `reviews/decisions/plugin-abi.md` §"Plugin
  Manifest Schema". Codegen emits the structs visible in §5 and the
  per-plugin `manifest.cpp` blob.

(No standalone envelope schema file exists; the envelope is Fory-defined
and shared across every type — its layout is fixed by `Envelope<T>` in
§5 and §4.8.)

Domain-owned schemas (`Transform`, `Mesh`, ...) live in their
originating context's directory under `data/schemas/<ctx>/`; the data
context only guarantees the round-trip.

### 5.3 Error types

The closed sum is `glibre::types::data::Error` declared above. Each arm
maps to one §4 invariant:

| Arm                       | Raised when                                                  | Origin                   |
|---------------------------|--------------------------------------------------------------|--------------------------|
| `AbiHashMismatch`         | plugin's compiled-in ABI hash ≠ host's                       | §4.4 inv. 3              |
| `SchemaMigrationFailure`  | a `MigrationFn` returned `unexpected` or chain incomplete    | §4.7 inv. 1, 4           |
| `DeserializeError`        | malformed envelope, unknown FQN, newer-than-host version     | §4.8 inv. 5              |
| `ReservedTagViolation`    | codegen detects reuse of a previously-shipped tag            | §4.1 inv. 3, §4.2 inv. 4 |
| `SchemaRegistryConflict`  | static-init insert collides on FQN                           | §4.5 inv. 1              |

Plugin loader code wraps these into `core::Error` arms per
`reviews/decisions/plugin-abi.md` §"Failure Modes → core::Error";
domain code wraps into its own context's `Error` per
`reviews/decisions/error-model.md` §"Composition Rules" #2.

Verification: the stub above compiles under
`clang++ -std=c++23 -fsyntax-only -fno-exceptions` on the toolchain
documented in `reviews/decisions/fory-codegen.md` §"Open Questions" #4
(libc++ as shipped with the macOS Xcode 15 / Homebrew-LLVM clang).

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
