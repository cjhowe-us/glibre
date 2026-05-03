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

#### 6.1.1 `tools/glibre-foryc/` — host codegen tool (build-time only)

A standalone host executable, **not shipped at runtime**. Owns the
`Foryc` aggregate (§4.2). Its sole job is to read every
`data/schemas/<ctx>/<Type>.fory` source under the configured schema
root, validate each parsed `Schema` against §4.1, and emit the
generated C++ sources, the registry-population TU, the per-plugin
manifest TU, and the embedded `AbiHash` constant.

Translation-unit shape (illustrative, not normative beyond the names
the rest of the spec already uses):

```
tools/glibre-foryc/
  src/
    main.cpp                 # arg parsing, deterministic file walk
    lexer.cpp                # .fory -> token stream (§6.2 step 1)
    parser.cpp               # tokens -> Schema AST (§6.2 step 2)
    validator.cpp            # Schema -> validated Schema (§6.2 step 3)
    emitter/
      header_emitter.cpp     # generates <ctx>/<Type>.hpp
      body_emitter.cpp       # generates <ctx>/<Type>.cpp
      registry_emitter.cpp   # generates _registry.cpp
      reflection_emitter.cpp # generates per-type ReflectionBlob data
      abi_hash_emitter.cpp   # generates _abi_hash.cpp
      manifest_emitter.cpp   # generates per-plugin manifest.cpp
    canonicalize.cpp         # parsed-form canonicalization (§7.3)
    blake3_wrapper.cpp       # blake3 over canonicalized bytes
  CMakeLists.txt             # links Apache Fory privately; host-only
  tests/                     # Catch2 unit tests for lexer/parser/etc.
```

`glibre-foryc` links Apache Fory and `blake3` privately; neither
dependency leaks past this module's boundary. The tool is the only
writer of files inside `data/codegen-output/` (§6.1.3) and the only
reader of `.fory` source files. It performs no network I/O, opens no
files outside the configured schema root and the configured output
directory, and is bit-deterministic across hosts (§4.2 inv. 5).

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
      envelope.hpp
      migration.hpp
      registry.hpp
      abi_hash.hpp
      plugin_manifest.hpp
      reflection.hpp
  src/
    schema_registry.cpp      # SchemaRegistry::instance, lookup,
                             # entries; static-init ordering (§4.3 inv. 5)
    envelope.cpp             # EnvelopeHeader read/write; little-endian
                             # pinning (§4.8 inv. 3); peek primitives
    migration_dispatcher.cpp # MigrationDispatcher (§6.3); keyed by
                             # (SchemaId, version-pair); arena reset
    register_migration.cpp   # glibre_types_register_migration entry
    arena.cpp                # per-payload Arena; reset to high-water
                             # between chain steps (§4.7 inv. 5)
    abi_hash.cpp             # glibre_types_abi_hash() trampoline that
                             # returns the codegen-embedded constant
    plugin_manifest.cpp      # PluginManifest deserialize helpers
                             # consumed by the core plugin loader
  CMakeLists.txt             # contributes to the glibre-types target
  tests/                     # Catch2 unit tests for dispatcher,
                             # registry, envelope, arena
```

`data/runtime/` is the only module that may keep mutable state in
process memory, and even that state is restricted to (a) static-init
populated read-only structures and (b) the per-payload `Arena` whose
lifetime is bounded by a single `deserialize` call. There is no
allocation outside an arena and no I/O of any kind.

#### 6.1.3 `data/codegen-output/` — generated artifacts (build dir, not in repo source tree)

A **build-directory artifact** populated by `glibre-foryc` at
configure / build time. Lives at
`${CMAKE_BINARY_DIR}/generated/glibre-types/` and is **not committed
to the repository**; the directory is regenerated from
`data/schemas/**/*.fory` on every Ninja re-glob (per
`reviews/decisions/fory-codegen.md` §"CMake Integration"). The data
context's `.gitignore` suppresses any accidental check-in.

```
data/codegen-output/                 # (build dir; symbolic name only)
  include/glibre/types/<ctx>/
    <Type>.hpp                       # one per .fory file; tag-sorted
                                     # struct, declaration-order-
                                     # independent (§4.2 inv. 2)
    <Type>_migrations.hpp            # GLIBRE_REGISTER_MIGRATION
                                     # macros; included by the owning
                                     # context's TU (§5 §migration.hpp)
  src/<ctx>/
    <Type>.cpp                       # serialize/deserialize bodies;
                                     # extern "C" trampolines per
                                     # §4.3 inv. 4
  src/_registry.cpp                  # static-init RegistryEntry
                                     # inserts; FQN-sorted (§4.5)
  src/_abi_hash.cpp                  # const char* literal returned by
                                     # glibre_types_abi_hash() (§6.4)
  src/_manifest_<plugin>.cpp         # one TU per discovered
                                     # plugins/*/plugin.fory (§6.5)
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

`glibre-foryc` runs a four-stage pipeline over the schema set; the
stages are sequential, each stage's output is the input to the next,
and the pipeline is a pure function of the schema set plus the tool
binary itself (§4.2 inv. 1).

**Stage 1 — Lex.** Each `.fory` source under the schema root is
opened, UTF-8 / NFC-normalized (§7.1 storage shape), and lexed into
a token stream against the grammar in §7.1. Lexer errors raise
`ReservedTagViolation` (the codegen front-end's generic syntactic-
error arm — see §10) with a file:line:col anchor. The lexer is
streaming and allocates only inside the tool's arena.

**Stage 2 — Parse.** The token stream is parsed into an in-memory
`Schema` AST node — the §4.1 aggregate's authoring-time
representation. Parser errors raise `ReservedTagViolation` with the
same anchor shape. The parser produces one `Schema` value per file;
no cross-file information is consulted at this stage.

**Stage 3 — Validate.** Each `Schema` is checked against the §4.1
invariants individually (FQN well-formedness, monotonic version,
unique tag numbers, reserved-tag immutability, `since ≤ version`,
default-on-non-`option` rule) and then against cross-schema
invariants (no two `Schema`s share an `FQN`, every `TypeRef` to
another generated type resolves to a `Schema` in the current set,
no cycles in the type graph, no reuse of any tag number ever shipped
in a prior committed version of the same `FQN`). Layout-additive
checking (§4.2 inv. 3) compares each `Schema` against the prior
committed version's parsed form: a new tag is permitted only if its
sorted position appends past the prior version's last field's
offset. Failures raise the §4 invariant's matching `data::Error` arm
(`ReservedTagViolation`, `SchemaRegistryConflict`, etc., per §10);
the build fails before any source is emitted.

**Stage 4 — Emit.** From the validated `Schema` set the emitter
writes the four artifact families:

1. **Headers and bodies.** One header
   `data/codegen-output/include/glibre/types/<ctx>/<Type>.hpp` and
   one body `data/codegen-output/src/<ctx>/<Type>.cpp` per `.fory`
   file. The header declares the `final` POD-like struct with
   tag-sorted fields and the `Envelope<T>` specialization
   declarations (§5 §envelope.hpp); the body emits the
   `serialize`/`deserialize` trampolines (per §4.3 inv. 4) and
   their `extern "C"` exports.
2. **Reflection blob.** Per-type `ReflectionField` arrays are emitted
   into the generated body (§4.9), inlined as `constexpr` data when
   the field set permits and `const` static arrays otherwise. A
   build flag (off by default in shipping profiles, on for
   editor / tools) controls whether the blob is wired into the
   `RegistryEntry::reflection` slot or left null (§4.9 inv. 5).
3. **Registry TU.** A single `data/codegen-output/src/_registry.cpp`
   collects every per-type `RegistryEntry` into the static-init
   table the runtime exposes via `SchemaRegistry::instance()`
   (§4.5). Entries are emitted in `FQN` byte-sort order so the
   binary search in §4.5 inv. 3 is constant-time over a contiguous
   array — the same ordering `AbiHash` consumes (§4.4 inv. 1, §6.4).
4. **ABI-hash TU.** A single `data/codegen-output/src/_abi_hash.cpp`
   embeds the 64-character lowercase hex `AbiHash` string as a
   `constexpr` literal and provides the body of
   `glibre_types_abi_hash()` (§6.4).
5. **Manifest TUs.** One
   `data/codegen-output/src/_manifest_<plugin>.cpp` per discovered
   `plugins/<plugin>/plugin.fory`, embedding the Fory-serialized
   `PluginManifest` blob into the plugin's `.rodata` (§6.5).

Emission is deterministic: file iteration is `FQN`-sorted, every
generated symbol is namespaced, generated newlines are LF, and the
emitter never reads back its own outputs. Two independent runs of
`glibre-foryc` over the same schema set on different hosts produce
byte-equal sources (§4.2 inv. 1, PHILOSOPHY §7).

The pipeline's input and output sides are pinned by §7 (the schema
file format) and §5 (the public C++ surface) respectively; this
section specifies only the four phases that connect them and the
ordering rules each phase must obey.

### 6.3 `MigrationDispatcher` (runtime composition)

Lives in `data/runtime/src/migration_dispatcher.cpp`. Owns
composition of the `MigrationChain` aggregate (§4.7) at deserialize
time. The dispatcher is the single runtime consumer of the
codegen-emitted migration tables; no other call site invokes
`MigrationFn`s directly.

**Key.** Lookup is keyed by `(SchemaId, version-pair)` where
`version-pair` is `(from_version, to_version)` and `to_version ==
from_version + 1`. No multi-step keys exist — the dispatcher
composes `vN → vM` exclusively by iterating single-step entries
(§4.6 inv. 5; §4.7 inv. 2). Hash table or sorted side-array is an
implementation detail; the spec requires only constant-time amortized
lookup and `FQN`-then-`from_version` deterministic iteration order
when the chain is walked.

**Storage.** The per-`FQN` migration entries live as a contiguous
`std::span<const MigrationEntry>` inside the registry entry for that
type (§4.5; §5 §registry.hpp). Codegen emits them in
`from_version`-ascending order so the dispatcher can index by
`from_version - 1` without sorting at runtime.

**Invocation contract.** On `Envelope<T>::deserialize(src)`:

1. Read `EnvelopeHeader` (§4.8 inv. 1, 2).
2. Look up `RegistryEntry` by `header.schema` (§4.5 inv. 3); unknown
   `FQN` → `data::Error::DeserializeError`.
3. Compare `header.version` against `entry.version`:
   * Equal → invoke `entry.deserialize(src, &out)` directly, return
     the value.
   * `header.version > entry.version` (newer-than-host) →
     `data::Error::DeserializeError` (§4.8 inv. 5).
   * `header.version < entry.version` → walk the migration chain.
4. Migration walk. For `n = header.version; n < entry.version; ++n`
   the dispatcher:
   * Indexes `entry.migrations[n - 1]` (the `(n → n+1)` entry).
   * Allocates a fresh `VNplus1` inside the per-payload `Arena`.
   * Resets the arena to its post-deserialize high-water mark
     between steps (§4.7 inv. 5).
   * Invokes the type-erased trampoline; on `unexpected` returns
     `data::Error::SchemaMigrationFailure` carrying the failing
     `(FQN, n → n+1)` pair (§4.7 inv. 4).
5. The final `entry.version`-shaped value is moved into the
   caller-owned `out_value`; intermediates never escape the
   dispatcher (§4.7 inv. 3).

**Purity.** Every step the dispatcher calls is a pure function
(§4.6 inv. 1, 4). The dispatcher itself reads only the registry it
holds by `const&`, never mutates the registry, never publishes a
mutating reference (§4.5 inv. 4), and allocates only inside the
caller-supplied arena.

**Hot-reload integration.** At the frame-8 barrier the loader passes
the pre-swap snapshot through this same code path against the
post-swap registry; success yields a migrated component row, failure
yields `core::Error::SchemaMigrationFailed` and reverts the swap
(§8; `reviews/decisions/hot-reload-protocol.md` §"Step 3 —
Migrate"; `reviews/decisions/plugin-abi.md` §"Loader Sequence" step
11). The dispatcher itself is unaware of the hot-reload phase — the
code path is identical to a cold deserialize.

### 6.4 ABI-hash construction and export

The `AbiHash` aggregate (§4.4) lives at the seam between codegen and
runtime: the value is *computed* at codegen time and *exported* at
runtime as a stable C symbol the plugin loader compares.

**Computation (codegen-time).**
`glibre-foryc`'s `abi_hash_emitter` consumes the validated `Schema`
set produced by stage 3 of §6.2 and produces the
`_abi_hash.cpp` translation unit by:

1. For each `Schema s`, compute
   `schema_source_hash(s) = blake3(canonicalize(s))` where
   `canonicalize` is the parsed-form canonicalization defined in
   §7.3 (§4.4 inv. 2). The output is a 32-byte digest.
2. Sort the resulting digests by the byte order of each `Schema`'s
   `FQN` (§4.4 inv. 1).
3. Concatenate the sorted digests with no separators (each digest
   is fixed-width, so the boundary is unambiguous).
4. Compute `blake3(concatenation)` — the resulting 32-byte digest is
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

The data context owns the Fory-serialized layout of `PluginManifest`
(§5 §plugin_manifest.hpp; `reviews/decisions/plugin-abi.md`
§"Plugin Manifest Schema") but does not own the loader that consumes
it. `glibre-foryc` extends its file walk to also process
`plugins/<plugin>/plugin.fory` files, emitting one
`data/codegen-output/src/_manifest_<plugin>.cpp` per discovered
manifest source. Each emitted TU embeds the Fory-serialized
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
`reviews/decisions/fory-codegen.md` §"CMake Integration"; this
section names the targets, not their internals.

```
glibre-foryc            (host executable, tools/glibre-foryc/)
   │
   ▼
glibre-types-codegen    (custom target; depends on schema glob +
   │                     glibre-foryc; outputs to
   │                     data/codegen-output/.stamp)
   ▼
glibre-types            (SHARED library; sources = data/runtime/src/
                         + data/codegen-output/src/; depends on
                         glibre-types-codegen)
```

`glibre-foryc` builds first and is host-only. Schema globs use
`CONFIGURE_DEPENDS` so a touched `.fory` re-triggers regeneration
without a CMake rerun. The middleman dylib's link line consumes both
hand-written runtime sources and codegen-emitted sources as one unit;
the static-init ordering rule (§4.3 inv. 5) is preserved by the
emitter writing builtin-registration TUs lexicographically before
generated-type TUs.

No plugin links `glibre-foryc`; no plugin links `Apache Fory`
directly; every plugin links exactly one `glibre-types.dylib`
(§4.10 inv. 4).

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
`SchemaRegistry` table (§4.5), the per-`FQN` `MigrationChain` records
(§4.7), and the construction inputs to `AbiHash` (§4.4). Those
meta-schemas are the only schemas this section enumerates; every
other persistent type (a `Transform`, a `PluginManifest`, a `Mesh`)
is owned by the originating context's `specs/<ctx>/SPEC.md` §7.

### 7.1 The `.fory` schema-file format

A `.fory` file is the authoring artifact that defines exactly one
`Schema` (§4.1). The file format is the load-bearing input to
`Foryc` (§4.2) and, transitively, to every byte the spine emits.
This subsection pins its bytes; `glibre-foryc` is the only writer
(via test fixtures) and the only reader.

**Storage shape.** `.fory` files are UTF-8 text with LF line
endings, NFC-normalized, no BOM. The canonicalization rule that
feeds `schema_source_hash` (§4.4 inv. 2) operates on the *parsed*
form — see §7.3 — so trailing whitespace and reorder of optional
clauses do not perturb the hash. The text form below is the
authored grammar, not the canonical form.

**Header.** Every file opens with a single `schema` declaration
naming the `FQN` and braces:

```fory
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
There is no separate magic number because `.fory` is text; the
file *extension* and the leading `schema` keyword together form the
identifier. Binary headers live on the wire (`Envelope`, §4.8 / §7.5),
not in source files.

**Field declarations.** Inside the braces, an ordered sequence of
clauses describes the schema. Field clauses use the form:

```fory
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
              | MigrationClause | CommentClause
FieldClause := "field" Ident ":" TypeRef
              "tag" UINT "since" UINT
              ("default" DefaultExpr)?
ReservedClause := "reserved" "tag" UINT
                  ("removed_in" UINT)?
                  ("comment" STRING)?
MigrationClause := "migration" "v" UINT "_to_v" UINT
                   "{" "provider" STRING "}"
TypeRef     := Builtin | FQN | "list" "<" TypeRef ">"
              | "map" "<" Builtin "," TypeRef ">"
              | "option" "<" TypeRef ">"
Builtin     := "u8" | "u16" | "u32" | "u64"
              | "i8" | "i16" | "i32" | "i64"
              | "f32" | "f64" | "bool"
              | "string" | "bytes"
              | "vec3f" | "quatf" | "entity"
DefaultExpr := Number | "true" | "false"
              | StringLit | "{" DefaultExpr ("," DefaultExpr)* "}"
              | "none"
CommentClause := "#" rest-of-line
```

Names follow §4.1 inv. 1: lowercased dotted-context path with a
PascalCase leaf for `FQN`; `[a-z][a-z0-9_]*` for field `Ident`s.

**Field-set rules** (each enforced at `Foryc` parse time):

1. Every active `tag` integer is unique across the file. The same
   number space covers active and reserved tags (§4.1 inv. 3).
2. `since <V>` on every field satisfies `1 ≤ V ≤ version`
   (§4.1 inv. 5).
3. A `field` whose type is non-`option<T>` and whose `since` is
   greater than `1` must carry a `default` clause. Codegen-time
   error otherwise.
4. Every `TypeRef` resolves either to a `Builtin` (audited in
   `glibre/types/_builtins.hpp`) or to another `FQN` whose `.fory`
   file is present in the same `Foryc` invocation; cross-schema
   cycles are detected and rejected (§4.1 inv. 4).
5. `default` expressions are typed against their `TypeRef`:
   numeric literals must fit the integer/float width; `{a, b, c}`
   composites must match a struct's tag-sorted field shape;
   `none` is the only legal default for `option<T>`. The expression
   is a build-time constant — no function calls, no other-field
   references.

**Reserved clauses.** A removed field becomes a `reserved tag`
line. The optional `removed_in <V>` records the schema version at
which the field disappeared; the optional `comment` is a free-form
string for human readers. The `removed_in` field is parsed and
written into the canonical form so `Foryc` can produce stable
diagnostics, but it has no effect on `schema_source_hash` (the
hash is computed over the parsed form including `removed_in`,
matching invariant §4.1 inv. 6 — the canonicalization is a pure
function of the schema's declared content).

**Migration clauses.** An optional `migration vN_to_vN+1 { provider
"<symbol>" }` clause names the originating context's free-function
symbol for that step (§4.6). There is exactly one clause per
`(N → N+1)` pair the file declares, and the union of clauses
covers `1→2, 2→3, …, version-1→version` (§4.7 inv. 1). Codegen
emits the dispatcher hookup; the body lives in the owning
context's translation unit.

**Comments.** `#` introduces a line comment; comments are stripped
before canonicalization. There are no block comments.

#### 7.1.1 Worked example (domain payload)

```fory
schema glibre.core.Transform {
  version 3
  since   "0.1.0"

  field translation : vec3f  tag 1 since 1
  field rotation    : quatf  tag 2 since 1
  field scale       : vec3f  tag 3 since 1   default { 1.0, 1.0, 1.0 }
  field flags       : u32    tag 4 since 2   default 0
  field parent      : entity tag 5 since 3   default none
  reserved tag 6  removed_in 3  comment "old `lod_bias`"

  migration v1_to_v2 { provider "glibre::core::migrate_Transform_v1_to_v2" }
  migration v2_to_v3 { provider "glibre::core::migrate_Transform_v2_to_v3" }
}
```

This is the same shape used in `reviews/decisions/fory-codegen.md`
§"Schema File Format", lifted to spec-precision and with reserved
+ migration clauses spelled out.

### 7.2 Meta-schemas owned by `data`

The `data` context's own persistent records — the rows of the
`SchemaRegistry`, the dispatcher's per-`FQN` migration tables, and
the inputs to `AbiHash` — are themselves authored as `.fory`
schemas, but they are **bootstrap meta-schemas**: they describe
the spine and so cannot use the spine's runtime migration to
evolve. Their evolution rule is documented in §7.6.

Files owned by `data`:

- `data/schemas/meta/SchemaSourceRecord.fory` — one row per
  registered `FQN`: the schema source hash, version, source path,
  and migration-chain length. Persisted as the byte form of one
  `RegistryEntry` (§4.5) minus the runtime function pointers.
- `data/schemas/meta/MigrationTableRecord.fory` — one row per
  registered `(FQN, N → N+1)`: the provider symbol name, the
  source schema FQN, the from/to versions, and a content hash of
  the migration's input/output type pair.
- `data/schemas/meta/AbiHashManifest.fory` — the
  `(blake3_hex, count, [SchemaSourceRecord*])` triple `Foryc`
  emits as the input log to `AbiHash`. This is the build-time
  artifact that lets reviewers reproduce the hash from sources.

The `EnvelopeHeader` (§4.8) is *not* one of these files: per §5.2,
the envelope is Fory-defined — its bytes are produced by Fory's
own header encoder rather than by a glibre-authored schema.
§7.2.4 below describes the byte shape the spine pins on top of
that Fory-defined header, but no `.fory` source file exists for it.

These files compile to POD records under `glibre::types::data::*`
(emitted into `glibre-types.dylib`), the same way every other
generated type does — they ride the same codegen pipeline,
participate in the registry, and contribute to `AbiHash`. What
makes them *meta* is the **bootstrap rule** in §7.6, not their
file format.

#### 7.2.1 `SchemaSourceRecord`

```fory
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
  `.fory` file, sorted by canonical Unicode code-point order
  before `AbiHash` ingests the table (§4.4 inv. 1).
- `migration_count` matches `version - 1` exactly (§4.7 inv. 1);
  any other value is a `SchemaRegistryConflict` at static-init.
- `reflection_present` mirrors §4.9 inv. 5; the runtime path
  never branches on this value, but tools and the editor do.

#### 7.2.2 `MigrationTableRecord`

```fory
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
  fixed schema-versions of its endpoint types and `Foryc` rejects
  re-binding a provider symbol against a different `(from, to)`
  pair (§4.7 inv. 1, §4.6 inv. 4).
- `provider_name` is the fully-qualified C++ symbol name codegen
  emits the dispatcher hookup against. The symbol must resolve at
  link time; missing symbols are a `glibre-types.dylib` link error,
  not a runtime `SchemaMigrationFailure`.

#### 7.2.3 `AbiHashManifest`

```fory
schema glibre.data.AbiHashManifest {
  version 1
  since   "0.1.0"

  field abi_hash_hex  : string                     tag 1 since 1
  field foryc_version : SemVer                     tag 2 since 1
  field entries       : list<SchemaSourceRecord>   tag 3 since 1
}
```

- `abi_hash_hex` is a 64-character lowercase hex Blake3-256 string
  (§4.4 inv. 1) computed exactly as
  `blake3( concat( sort_by_fqn( source_hash(s) for s in entries ) ) )`.
- `foryc_version` is the `glibre-foryc` SemVer that produced this
  manifest. It exists for diagnostic reproducibility — two builds
  with byte-identical entries but different `foryc_version` must
  still produce the same `abi_hash_hex`, so the field is **not**
  an input to the digest (§4.4 inv. 4).
- `entries` is sorted by `fqn` ascending (lexicographic byte order)
  before serialization. Out-of-order lists produce
  `DeserializeError` so the file's bytes are a faithful audit
  artifact.
- `SemVer` here is the `glibre.core.SemVer` schema declared in
  `reviews/decisions/plugin-abi.md` §"Plugin Manifest Schema".

#### 7.2.4 `EnvelopeHeader` (Fory-defined; byte-shape note)

The wire-form prefix every persistent payload carries (§4.8) is
emitted by Fory's own header encoder, not by a glibre-authored
`.fory` schema. The spine pins its observable byte shape here so
the per-version goldens in §7.5 and the dispatcher logic in §4.8
inv. 5 have a single source of truth:

```text
EnvelopeHeader (logical layout, Fory-encoded):
  fqn            : string   # FQN of the payload's Generated Type
  schema_version : u32      # SchemaVersion encoded little-endian
  payload_length : u32      # body byte count, little-endian
  flags          : u32      # reserved; current builds emit 0
```

- This is the same logical record that surfaces as
  `glibre::types::EnvelopeHeader` in §5; the C++ projection is the
  authoritative API, the listing above is the read-only byte
  shape.
- `flags` is reserved-by-name. Adding a flag bit forces a
  Fory-defined header version bump that the spine treats as a
  meta-schema change subject to §7.6's bootstrap rule (release-time
  migration via `glibre-foryc`, not in-process `MigrationChain`).
- The header is *not* listed in `AbiHashManifest.entries`; it
  rides Fory's own version handling. Changes to its layout still
  force a `glibre-foryc` release and therefore a rebuilt
  middleman with a new `AbiHash` value, but via meta-schema
  release notes rather than the per-schema source-hash path.

### 7.3 The schema-source canonicalization rule

The `schema_source_hash` of a `.fory` file is `blake3(canonical(s))`
where `canonical` is the deterministic byte serialization defined
below. Identical schemas in source must produce byte-identical
canonical forms regardless of authoring whitespace, comment
placement, or clause order.

The canonical form is a single UTF-8 byte sequence built by
emitting the *parsed* schema in the following fixed shape:

```text
schema <fqn>\n
version <V>\n
since "<semver>"\n                # if present; else absent
[for each active field, sorted by ascending tag:]
  field <name> : <type-canonical> tag <N> since <V>[ default <expr-canonical>]\n
[for each reserved tag, sorted by ascending tag:]
  reserved tag <N>[ removed_in <V>][ comment "<...>"]\n
[for each migration, sorted by ascending from-version:]
  migration v<N>_to_v<N+1> { provider "<symbol>" }\n
\n
```

Rules feeding the byte form:

1. **Sort by tag** for active fields and reserved entries (§4.1
   inv. 3). Sort by `from_version` for migration clauses
   (§4.7 inv. 1).
2. **Single-space separators** — no double spaces, no tabs.
3. **LF line endings** — never CR or CRLF.
4. **Type canonicalization** — `list`/`map`/`option` use no
   internal whitespace: `option<u32>`, not `option< u32 >`.
   Generic-type generic argument's canonicalization is recursive.
5. **Default-expression canonicalization** — integer literals
   normalize to base-10, no leading zeros, no underscores; float
   literals normalize to round-trippable shortest form
   (`std::to_chars` `chars_format::shortest`); composite defaults
   `{...}` use `, ` (comma-space) separators.
6. **String escaping** — `"` and `\` only; control characters
   reject at parse time (canonical form never sees them).
7. **No comments** — `#` lines are stripped entirely; their
   positions and contents are not part of identity.
8. **Trailing single empty line** — every canonical form ends
   with `\n\n`. This makes concatenation in §4.4 unambiguous.

The hash is `blake3(canonical_bytes)`; the result is a 32-byte
digest stored as the `source_hash` of `SchemaSourceRecord`
(§7.2.1) and as input to `AbiHash` (§4.4 inv. 1).

### 7.4 Migration rules (engine-wide)

Migrations are owned per-type by the originating context (§4.6),
but the rules they obey are owned here:

1. **Stepwise only.** A migration is `vN → vN+1`. There is no
   `vN → vN+2`. The dispatcher composes chains (§4.7); writers
   never short-circuit. (`Foryc` rejects a `migration v1_to_v3`
   clause as a `SchemaMigrationFailure`-shaped codegen error.)
2. **Pure and deterministic.** The provider function must satisfy
   §4.6 inv. 1–4: no clock, no RNG, no global state, no I/O,
   no allocation outside the supplied `Arena&`.
3. **Total over its prior-version domain.** For every `VN`
   produced by `deserialize_v<N>`, the provider yields a `VNplus1`.
   `std::unexpected` is reserved for *defective* payloads — e.g.
   a foreign-key tag pointing to an absent sibling — never for a
   missing default that the schema authors should have declared.
4. **Compositional.** Hot-reload migration runs every step in
   ascending order against a single per-payload arena reset
   between steps (§4.7 inv. 5); cross-step retention is forbidden.
5. **Complete coverage.** For every `FQN` whose current version
   is `M`, exactly `M-1` migration providers exist; the build
   fails at codegen otherwise (§4.7 inv. 1). The data context
   never ships a partial chain.
6. **Adding a field is not always a migration.** Per §4.2 inv. 3,
   appending a new tag past the prior version's last offset is
   ABI-additive: codegen synthesizes the default at deserialize
   time and the schema bumps without authoring a provider. A
   new tag whose sorted position is not append-past-end forces a
   migration. Authors do not choose which case applies; `Foryc`
   does.
7. **Tag reuse is forbidden — forever.** Reserved tags (§4.1
   inv. 3) carry forward across versions; `Foryc` errors with
   `ReservedTagViolation` on any reuse. There is no migration
   shape for rebinding a tag to a new field.

### 7.5 Wire format and round-trip identity

Every persistent payload is the byte concatenation of the
`EnvelopeHeader` (§7.2.4) followed by the Fory-encoded body of
the `Generated Type`. The envelope is a fixed self-describing
prefix (§4.8 inv. 1, 2), little-endian (§4.8 inv. 3), and is the
sole source of dispatch truth at deserialize time (§4.8 inv. 5).

Round-trip identity (§4.8 inv. 4) is tested per-schema via
Catch2 goldens at `tests/data/schemas/<ctx>/<Type>.cpp`:

1. Construct an instance `t` from a deterministic constructor
   recipe.
2. `bytes := Envelope<T>::serialize(t)`.
3. Assert `bytes` byte-equal a checked-in `golden.fory.bin`
   (or regenerate under `--update-goldens`).
4. `t' := Envelope<T>::deserialize(bytes).value()`.
5. Assert `t' == t` (POD-equality, tag-sorted comparison).
6. Assert `Envelope<T>::serialize(t') == bytes`.

Per-version goldens are kept under
`tests/data/schemas/<ctx>/<Type>/v<N>.fory.bin` so older payloads
exercise the migration path:

1. Read the `vN` golden bytes.
2. `t := Envelope<T>::deserialize(bytes).value()` — runs the
   dispatcher, applying every step `vN → vN+1, … → vM`.
3. Assert `t` matches the version-`M` golden.
4. Assert `Envelope<T>::serialize(t)` byte-equals the version-`M`
   serialized golden.

The `MigrationChain` is exercised as an integration test against
the registry rather than per-step: the data context guarantees
*chain-level* round-trip, which is the property loader and
hot-reload code consume.

### 7.6 Bootstrap rule for meta-schemas

The meta-schemas in §7.2 (`SchemaSourceRecord`,
`MigrationTableRecord`, `AbiHashManifest`, `EnvelopeHeader`)
describe the spine itself and so **cannot** evolve through the
in-process `MigrationChain` they describe — that would require
the spine to be running before its own description is parsable.
The bootstrap rule decouples them from the runtime migration
path:

1. **Out-of-band versioning.** Each meta-schema carries the
   normal `version` integer in its `.fory` header, but the
   `version` is bumped *only* in lockstep with a `glibre-foryc`
   release. The release notes call out the bump and the manual
   migration steps tools and existing build artifacts must
   take.
2. **No registered migration providers.** Meta-schemas declare
   no `migration` clauses. `Foryc` recognises files under
   `data/schemas/meta/` and lifts the §7.4 rule #5 coverage
   requirement for those files only — partial chains are
   acceptable because the chain never runs at runtime.
3. **Single live version per build.** A given `glibre-foryc`
   release emits exactly one version of each meta-schema. The
   middleman dylib produced by that release reads and writes
   only that version of each meta-schema. There is no
   `EnvelopeHeader v1` reader inside a build that emits
   `EnvelopeHeader v2`.
4. **Cross-build artefacts are reproduced, not migrated.**
   `AbiHashManifest` files, intermediate `.foryc-stamp` blobs,
   and the meta-schema binary descriptors embedded in
   `glibre-types.dylib` are *artefacts of the build*, not
   user-data. When a meta-schema bumps, the artefacts are
   regenerated from authoring sources by the new `glibre-foryc`;
   no migration pass is run.
5. **Manual migration steps live in `glibre-foryc` release
   notes.** When an `EnvelopeHeader` flag bit is added, the
   release notes spell out (a) the wire-format diff, (b) any
   on-disk artefact rebuild needed, (c) any tooling step
   downstream consumers must take. The data context owns these
   notes alongside the `glibre-foryc` source repository; the
   spec does not enumerate per-release steps.
6. **Hash invariants hold.** Even though meta-schemas do not
   migrate, they *do* contribute to `AbiHash` exactly like any
   other schema (§4.4 inv. 1). A meta-schema bump therefore
   forces an `AbiHash` change, which forces every plugin
   consuming the spine to rebuild — the same gate that catches
   any other schema-shape drift (`reviews/decisions/plugin-abi.md`
   §"Versioning Rules"). The bootstrap rule lifts only the
   *runtime-migration* obligation, not the ABI-gating one.

The collapse: domain schemas migrate at runtime via the spine;
the spine itself migrates at *release time* via `glibre-foryc`.
Two surfaces, one ABI gate.

### 7.7 Cross-context obligations

Every other context's `specs/<ctx>/SPEC.md` §7 enumerates the
domain schemas the context owns under
`data/schemas/<ctx>/<Type>.fory`. Those sections inherit this
section's rules:

1. The grammar in §7.1 is the only legal `.fory` syntax.
2. Migration providers obey §7.4.
3. Round-trip goldens follow the harness in §7.5; per-version
   goldens are mandatory whenever the schema's version exceeds 1.
4. The `data` context does **not** own those schemas — adding a
   new domain schema requires no edit to this section. What `data`
   owns is the meta-schema layer in §7.2 and the rules above.

The biconditional in §4.10 inv. 1 is the load-bearing connector:
every `.fory` file under `data/schemas/<ctx>/` corresponds to one
registry entry, and every registry entry corresponds to one
`.fory` file — verified at configure time by `Foryc` (§4.5 inv. 5).

## 8. Hot-Reload Contract

The `data` context is the persistence spine; it does not run the
hot-reload state machine. The loader at the frame-8 barrier owns the
four-step **drain → swap → migrate → resume** protocol
(`reviews/decisions/hot-reload-protocol.md` §"Protocol Sequence"); the
data context owns step 3 (**migrate**) and the gate values steps 2 and
4 consult (`AbiHash`, `SchemaRegistry`). This section pins the contract
between those owners: what bytes survive, what `migrate(...)` must do,
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

> **Data survives the swap if and only if its type has a `.fory`
> schema registered in the live `SchemaRegistry`.**

This biconditional is the same one the loader checks; the data context
guarantees the *forward* direction (registered ⇒ preserved by either
identity or migration) and the codegen guarantees the *reverse*
direction (no schema ⇒ no registry entry ⇒ not persistent ⇒ not
preserved). Concretely, the spine preserves:

1. **Generated-type byte storage.** Every ECS component, world
   singleton, asset payload, or plugin-private record whose C++ type
   is a `Generated Type` (§4.2) survives. Storage rows are migrated
   in place when the new plugin's `SchemaVersion` for the type
   exceeds the version recorded in the storage's per-row header
   (`reviews/decisions/hot-reload-protocol.md` §"Step 3 — Migrate"
   step 3); rows whose stored version equals the current version
   are passed through unchanged.
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
4. **Per-payload arena state — *not* preserved.** The dispatcher's
   `Arena` (§4.6) is per-payload and reset between steps; nothing
   in the arena survives the migrate phase, let alone the swap.
   This is restated here only because step 3.4 of the loader
   protocol resets it on failure — see §8.4.

State that does **not** survive (consistent with
`reviews/decisions/hot-reload-protocol.md` §"State Survival Rules"
"Re-derived"):

- Plugin-private types without a `.fory` schema. By §4.10 inv. 1
  these have no registry entry and the spine has no migration
  story for them.
- The `MigrationChain` function-pointer table for plugin-emitted
  types whose plugin is being swapped out — the table's entries
  point into the *outgoing* plugin's `.text` segment and are
  invalidated by `dlclose`. The new plugin's static-init
  re-registers replacements through
  `glibre_types_register_migration` before the loader proceeds
  past step 4.1 (§4.5 inv. 4 still holds: the registry is
  read-only after static-init *of the live middleman build*).
- Editor-only `ReflectionBlob` interning tables for types whose
  schemas dropped between Q and P. Tools handle the null pointer
  per §4.9 inv. 5.

### 8.2 The `migrate(...)` responsibility

Every plugin reload routes through one entry point in the data
context:

```cpp
namespace glibre::types::data {

// Called by the loader at hot-reload-protocol step 3, once per
// outgoing-plugin / incoming-plugin pair. Drives MigrationDispatcher
// across the version gap and validates schema-set continuity.
[[nodiscard]] auto migrate(
    const SchemaRegistry& outgoing,    // pre-swap registry view
    const SchemaRegistry& incoming,    // post-swap registry view
    World&                world,        // borrowed; storage walk only
    Arena&                arena         // per-payload, reset between rows
) noexcept -> std::expected<MigrationReport, Error>;

}  // namespace glibre::types::data
```

**Responsibility.** `migrate(...)` performs *exactly* the data
context's part of the loader's step 3:

1. **Schema-set continuity check.** For every `FQN` registered in
   `outgoing` and referenced by any surviving storage row in
   `world`, `migrate` asserts the same `FQN` is registered in
   `incoming`. A missing `FQN` is a major-version change, not a
   hot-reload (`reviews/decisions/hot-reload-protocol.md` §"Step 2 —
   Swap" step 2.2); `migrate` returns `unexpected(Error::
   SchemaMigrationFailure)` carrying the dropped `FQN` in the
   detail payload.
2. **Per-row dispatch.** For each surviving storage row whose stored
   `SchemaVersion` is less than `incoming.lookup(fqn)->version`,
   `migrate` invokes the `MigrationDispatcher` (§4.7,
   `reviews/decisions/hot-reload-protocol.md` §"Step 3 — Migrate"):
   the dispatcher composes the per-step `MigrationFn`s into the
   chain `(stored_version → current_version)`, allocating into
   `arena` and resetting it between rows.
3. **Per-row in-place commit.** On chain success, the new bytes
   overwrite the row in place; on chain failure, the arena is
   reset and the row is left in its *outgoing* state (the loader's
   rollback path then un-swaps the vtable per
   `reviews/decisions/hot-reload-protocol.md` §"Failure & Rollback").
   At most one row is half-overwritten at any instant, and the
   loader's exclusive lock on phase 8 hides that intermediate from
   every observer.
4. **Validation, not interpretation.** `migrate` does not decide
   *which* migration to run; it dispatches by `(FQN, from, to)`
   tuples that the registry already records. It does not run any
   plugin-private code beyond the `MigrationFn` bodies registered
   through `glibre_types_register_migration`. It never reads
   `World` outside the storage row currently being migrated, never
   touches the file system, and never allocates outside `arena`.
5. **Report-out.** On success `MigrationReport` contains the count
   and the sorted span of `FQN`s actually migrated (i.e. those
   whose stored version differed from the current); the loader
   forwards the span into the `HotReloadCompleted` event published
   at protocol step 4.3.

**Out of scope — `migrate(...)` does not.** The function does *not*
dlopen, dlsym, swap vtables, mutate the system schedule, log
structured warnings, or publish observer events. Each of those is
the loader's responsibility per
`reviews/decisions/hot-reload-protocol.md` and
`reviews/decisions/plugin-abi.md`. The data context refuses to host
any of them (§1; §4.10 inv. 7).

**Idempotence.** Re-invoking `migrate(...)` against an `outgoing`
whose registry already matches `incoming` is a no-op that returns
`MigrationReport{count = 0, migrated = {}}`. This makes step-3
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
2. **Migration-chain coverage is complete.** For every `FQN` in
   `P_types.SchemaRegistry` whose `SchemaVersion` differs from the
   `FQN`'s version in `Q_types.SchemaRegistry`, the chain
   `(P_version → Q_version)` is fully present in
   `Q_types.SchemaRegistry`. Missing any step refuses with
   `Error::SchemaMigrationFailure` (`reviews/decisions/hot-reload-protocol.md`
   §"Refusal Cases" #2). Coverage is checked *before* any byte is
   migrated, against the meta-schemas in §7.2 — the loader reads
   `Q-types`'s `AbiHashManifest` and walks every entry.
3. **Meta-schema bootstrap rule holds.** Per §7.6, a
   `glibre-types.dylib` reload that bumps any of the meta-schemas
   (`SchemaSourceRecord`, `MigrationTableRecord`,
   `AbiHashManifest`, `EnvelopeHeader`) requires release-time
   migration via `glibre-foryc`, not in-process `MigrationChain`.
   A self-reload that crosses such a bump is refused by
   construction; the operator either rebuilds the world from
   sources via the new `glibre-foryc` (acceptable) or restarts the
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
| `MigrationChain` missing or `MigrationFn` returned `unexpected` | `Error::SchemaMigrationFailure` (§5.3) | step 3.1 / 3.2; §8.3 gate 2                  |
| `Q.glibre_plugin_register` returned `unexpected`    | (not data — `core::Error::PluginInitFailed`) | step 4.1                                  |

The third case is included for completeness only; its detection and
typing live in `core` per `reviews/decisions/plugin-abi.md`
§"Failure Modes → core::Error". The data context contributes to it
solely through `MigrationReport`-carried context attached to the
plugin's failed `register` (the new plugin may consult the report
to know which migrations ran, but the data context does not type
its failure mode).

**Schema-set continuity refusals** (§8.2 step 1) raise
`Error::SchemaMigrationFailure` rather than a new arm. The data
context takes the §10 closed sum at face value: a missing schema
in `incoming` is a defective migration *story* (the plugin author
failed to write the migration that drops or relocates the type),
and the loader surfaces it identically to a mid-chain failure.
This collapses two failure modes into one error arm and one
typed `core::Error::SchemaMigrationFailed` wrapping (per
`reviews/decisions/plugin-abi.md` §"Failure Modes" row 11).

**Per-row rollback discipline.** Every refusal that fires inside
`migrate(...)` leaves the world byte-identical to its pre-`migrate`
state — at most one row's worth of arena bytes is in flight at any
instant, and a single failing row's arena is reset before the
function returns. The loader's outer rollback (un-swap vtable,
re-register P) then proceeds without any bytes-in-flight from the
data layer (`reviews/decisions/hot-reload-protocol.md` §"Failure &
Rollback" — failure at step 3).

**Logging.** Refusals are logged exactly once at `warn` by the
loader (`reviews/decisions/hot-reload-protocol.md` §"Refusal
Cases"); the data context emits no log of its own. The structured
fields the loader records include the `MigrationReport`'s partial
contents so operators can see how far the chain progressed before
the refusing step.

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
        RegistryReplaced = 2,   // Mode B: full SchemaRegistry instance() swap
    };
    Kind                              kind{Kind::EntriesAppended};
    std::span<const SchemaId>         affected_fqns{};   // sorted ascending
    std::span<const MigrationReport>  reports{};         // one per migrated FQN
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

**Atomicity.** The event is delivered after `migrate(...)` succeeds
and after the loader's own step 4.2 caches are rebuilt — i.e. the
subscriber sees the post-swap world exactly the way it will tick
in frame N+1. Subscribers never observe a half-migrated registry
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
`enqueue_hot_reload`: a deterministic fixture that bumps a single
schema's `SchemaVersion` and registers a paired migration without
touching the filesystem.

```cpp
#if defined(GLIBRE_E2E)
namespace glibre::types::data::test {

// Builds a fresh SchemaRegistry instance whose entry for `fqn` is
// upgraded by exactly one version, with the supplied migration
// function wired into the chain. The returned registry is suitable
// for passing to the loader's enqueue_hot_reload as the post-swap
// view; existing entries for other FQNs are copied identically.
//
// Pure, allocation-free past the supplied arena. The bumped version
// is recorded in a deterministic SchemaSourceHash derived solely
// from the (fqn, new_version, migration_provider_name) triple, so
// repeated calls with the same arguments produce byte-identical
// registries (PHILOSOPHY §7).
auto bump_schema_version(
    const SchemaRegistry& base,
    SchemaId              fqn,
    MigrationEntry        new_step,         // from_version = base.version, to_version = base.version + 1
    Arena&                arena
) noexcept -> std::expected<const SchemaRegistry*, Error>;

// Forces the next migrate() call against `fqn` at `(N → N+1)` to
// return Error::SchemaMigrationFailure. Used to exercise the full
// rollback path in the loader's failure tests
// (reviews/decisions/hot-reload-protocol.md §"Test Hooks" CI
// scenario 4). Idempotent; clearing requires
// `clear_force_migration_failure(fqn)`.
void force_migration_failure(SchemaId fqn,
                             SchemaVersion from,
                             SchemaVersion to) noexcept;
void clear_force_migration_failure(SchemaId fqn) noexcept;

}  // namespace glibre::types::data::test
#endif
```

**Determinism.** `bump_schema_version` is a pure function of its
inputs; the synthesized `SchemaSourceHash` is reproducible across
runs and hosts. The fixture never allocates outside the supplied
arena and never writes to disk. Tests under
`tests/data/schemas/hot_reload/` exercise:

1. Happy-path bump: stored `vN` payloads migrate to `vN+1`,
   `SchemaRegistryChange::Kind::VersionsBumped` fires once with
   the bumped FQN, post-bump `Envelope<T>::deserialize` byte-equals
   a checked-in golden.
2. Missing-step refusal: a `MigrationEntry` whose `from_version`
   is two steps behind raises `Error::SchemaMigrationFailure`
   without touching a single storage row (§8.4 per-row rollback
   discipline).
3. Forced-failure rollback: `force_migration_failure` injected
   mid-chain leaves storage byte-identical to its pre-`migrate`
   state and the registry pointer-identical to `outgoing`.
4. Append-only continuity: a bump that adds a new FQN (rather
   than upgrading an existing one) fires
   `SchemaRegistryChange::Kind::EntriesAppended` and the migrate
   pass is a no-op (`MigrationReport::count == 0`).

These fixtures back the §11 acceptance criteria and are the only
allowed entry into the spine's mutation-time machinery from test
code.

### 8.7 Cross-context obligations

Every other context's `specs/<ctx>/SPEC.md` §8 specifies *its*
plugin-side hot-reload obligations against the contract above.
Those sections inherit:

1. **Migration providers obey §7.4** — pure, deterministic, total
   over their input domain. The data context's `migrate(...)`
   calls them through the dispatcher; it does not validate their
   bodies.
2. **`SchemaRegistryChange` subscribers may not allocate** during
   the synchronous notification window. The editor and tools
   subscribe a pre-allocated handler; runtime contexts that need
   notification must pre-arrange their cache structure at
   `glibre_plugin_register` time.
3. **No context but `data` mutates the registry.** Plugins
   contribute entries through codegen (statically) and migrations
   through `glibre_types_register_migration` (at static-init);
   nothing else writes (§4.10 inv. 7).
4. **Self-reload of `glibre-types.dylib` is opt-in.** Contexts
   may not assume Mode B is enabled; the default in MVP is
   process restart (§8.3, PHILOSOPHY §8).

The collapse: domain plugins write `MigrationFn` bodies and
subscribe to registry changes; the data context owns one entry
point (`migrate(...)`), one observer event (`SchemaRegistryChange`),
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
| CPU sim (per frame) | 0.20 ms   | persistence spine; per-frame work is migration handoff + handle bookkeeping |
| CPU submit          | 0.00 ms   | data does not record GPU work                                                |
| GPU                 | n/a       | data owns no Metal heaps or encoders                                         |
| Heap ceiling        | 32 MiB    | resident middleman tables + active migration scratch                         |
| Phase ownership     | none      | participates inside phase 8 (hot-reload barrier) on reload frames only       |

The 0.20 ms sim cell is reserved so a schema-migration that lands on a
hot-reload frame (Scenario S2 from `perf-budget.md`) does not blow out
the budget; steady-state cost in S1 is near zero. `data` records no
CPU-submit, no GPU, and owns no phase outright — the migration step it
performs is invoked by `core` from inside phase 8 and accounted under
`data`'s tag via `glibre::PerContextAllocator`.

### 9.2 Per-aggregate budget

Aggregates are the nine SRP-bounded units defined in §4. Each row
quotes a CPU per-call cost (or per-frame, where the work is per-frame),
a heap sub-ceiling, and the invocation site. Sub-ceilings sum to the
32 MiB row above; CPU per-frame contributions sum into the 0.20 ms cell.

| Aggregate             | CPU cost                                                                | Heap sub-ceiling | Invocation site                                              |
|-----------------------|-------------------------------------------------------------------------|------------------|--------------------------------------------------------------|
| `Schema`              | 0 (host-only authoring; not in runtime)                                 | 0                | not loaded in shipping build                                 |
| `Foryc`               | 0 (host build-tool, not in runtime budget)                              | 0                | build graph only; no runtime presence                        |
| `Middleman` (dylib)   | 0 per frame (link-time presence; init at process start)                 | 0 sub-ceiling    | static-init populates `SchemaRegistry`; no per-frame work    |
| `AbiHash`             | 0 (`O(1)` static string return; called once at plugin load)             | 0                | plugin-loader handshake; not on the hot path                 |
| `SchemaRegistry`      | ~10 ns per lookup (read-only static `flat_map` over `(fqn, version)`)   | 8 MiB            | called from `Envelope` deserialize; bounded by call count    |
| `Migration` (single)  | invoked at hot-reload only; 0 on the hot path                           | counted in `MigrationDispatcher` 4 MiB                       | one call per migrated payload during phase 8                 |
| `MigrationChain`      | composition only; cost folds into `MigrationDispatcher`                 | counted in `MigrationDispatcher` 4 MiB                       | composed once at registry init; replayed per dispatch        |
| `Envelope`            | per-call cost varies by schema; sample-scene fixture target 0.1 ms total per frame | 16 MiB scratch | every serialize / deserialize call site                      |
| `ReflectionBlob`      | 0 in shipping build (editor / tools only)                               | 4 MiB (tools build); 0 (shipping)                            | inspector and dump tools                                     |
| `MigrationDispatcher` | invoked at hot-reload only; 0 on hot path                               | 4 MiB            | phase 8 hot-reload barrier (`core` calls into `data`)        |

Heap sub-ceilings sum: 8 (`SchemaRegistry`) + 16 (`Envelope` scratch) +
4 (`MigrationDispatcher`) + 4 (`ReflectionBlob`, tools build only) =
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
moves through `core`'s ECS storage, not through Fory wire form. The
per-frame Envelope budget is therefore reserved for incidental
serializations (e.g. snapshot capture for the editor's time-rewind
scrubber, save-on-checkpoint events) and capped at **0.1 ms total per
frame** across all call sites. Steady-state: ~0 ms; budget: 0.1 ms.

The remaining 0.10 ms of the 0.20 ms cell absorbs the migration
handoff cost on a reload frame (S2): a single plugin's component
storage migrates through `MigrationDispatcher` inside phase 8, and
`data`'s portion of that 0.40 ms phase-8 budget is the
deserialize-with-migration over the world snapshot. This is one frame
per reload, not per frame.

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
3. **`migration_dispatch_v_minus_1.bench.cpp`** — deserializes a
   one-version-old payload through `MigrationDispatcher` for every
   registered single-step migration in the build, asserting per-call
   `<= 50 us`. Establishes a dispatch-cost ceiling so a future schema
   change does not silently grow phase-8 cost beyond its 0.40 ms slot.
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
3. `MigrationDispatcher`'s 4 MiB is a phase-8-only arena, allocated
   from the migration arena owned by `core` (16 MiB ceiling inside
   `core`'s 64 MiB row); `data`'s 4 MiB sub-ceiling counts against
   `data`'s tag, not `core`'s.
4. `Foryc` is a host build tool. Its memory consumption is not in the
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
growth of `Envelope` scratch or a still-undecided lazy-migration cache
(see §12 open question).

## 10. Failure Modes & Error Model

Typed errors. Recovery.

## 11. Acceptance Criteria

GitHub `type:user-story` issues this spec closes:

- #TBD — `<title>`

Each must have a Catch2 test by name.

## 12. Open Questions

- Owner / resolution gate.
