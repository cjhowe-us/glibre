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

Harmonius requirement IDs / file paths cited as research input. Note any
collapse decisions (multiple harmonius concepts → one glibre primitive).

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
