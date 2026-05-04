# data — Detailed Design: reflection-blob aggregate

> Detailed design for the `ReflectionBlob` aggregate declared in
> `specs/data/SPEC.md` §4.9. Refines §4.9, §5 (`reflection.hpp` stub),
> §6.2 stage 4 emit-step #2, §7.2.1 (the `reflection_present` meta-field),
> §8.1 (state survival), §9.2 (per-aggregate budget row), §10.1 (closed
> sum extension) of the SPEC in place; cites
> `reviews/decisions/error-model.md`,
> `reviews/decisions/fory-codegen.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/perf-budget.md`, and
> `reviews/decisions/frame-phases.md`. Sibling siblings on `main`:
> `specs/data/envelope-serdes-design.md`,
> `specs/data/schema-registry-design.md`,
> `specs/core/type-registry-design.md`. All conclusions independently
> re-derived; harmonius prior art
> (`harmonius/docs/requirements/core-runtime/reflection-and-type-system.md`,
> R-1.3.1 .. R-1.3.11) cited as research input only — every clause
> evaluated against PHILOSOPHY §6 (no runtime reflection in shipping
> builds) before being kept, narrowed, or refused.

Refs: spike #740 — `[SPIKE] design-data-reflection-blob-detailed`.
Parent sub-epic #729. Sibling task-breakdown spike blocked-by this
deliverable.

## 1. Purpose

`ReflectionBlob` is the **editor-only descriptor table** for one
codegen-emitted `Generated Type`. Its single responsibility is to
**carry the static field-and-tag descriptor of one schema, in a
self-contained read-only form, so that the editor's property
inspector and asset browser can render forms over byte buffers
without linking the runtime serdes hot path**.

Concretely the aggregate owns, for one `(SchemaId, SchemaVersion)`
pair:

1. The list of active fields, each described by `(name, tag,
   type_kind, since, byte_offset, byte_size)` (§3.1).
2. A per-blob name interning table so name strings resolve without
   re-loading source `.fory` files (§3.2).
3. A per-blob lookup index keyed by `tag` for `O(log N)` field
   lookup inside the inspector loop (§3.3).
4. A per-context partition pointer back into the registry so the
   editor can iterate "every blob in `physics::*`" without scanning
   the whole catalog (§3.4).

What the aggregate **explicitly refuses to own**:

- **Runtime reflection.** Per `PHILOSOPHY.md` §6 and §4.9 inv. 1 of
  the SPEC, the engine has zero runtime reflection in shipping builds.
  `ReflectionBlob` is **never** linked into a shipping binary; it is
  emitted into a separate translation unit gated behind the
  `GLIBRE_EDITOR` define (§4.4). Shipping `RegistryEntry::reflection`
  is `nullptr` (§4.9 inv. 5).
- **Behaviour dispatch.** No code path in the engine uses the blob
  to choose what to do. Serialization, deserialization, migration,
  ECS storage layout, and ABI hashing all consume codegen-emitted
  static functions — never the descriptor table. Confirmed by the
  `RegistryEntry::reflection` slot being **read-only** and
  **not consulted** by `Envelope<T>::serialize` /
  `Envelope<T>::deserialize` (sibling design
  `specs/data/envelope-serdes-design.md` §3, §6).
- **Editor UI rendering.** The blob is a *data* artifact. How the
  inspector turns a `ReflectionField` into a Dear ImGui row, how the
  asset browser draws thumbnails, how `EditCommand`s commit through
  `CommandStack` — that is `tools/glibre-editor/`'s territory
  (`specs/tools/SPEC.md` §4.4 — `Inspector` / `InspectorView` /
  `ReflectedField`). This design pins the *byte view* the editor
  reads; it does not pin the widget tree on top.
- **Field value get/set.** The blob contains *describable shape*
  (offsets, sizes, type kinds), not getter/setter closures. The
  editor synthesises the read-and-write closures from the blob
  plus the live byte buffer (§4.3). A getter/setter table embedded
  in the blob would be runtime reflection (refused).
- **Property paths.** Harmonius R-1.3.3 (`transform.position.x`)
  is **out of MVP scope** — the editor walks one level at a time
  via `ReflectionField::type_kind == FQN` and a recursive descent
  into the nested blob (§3.5). A flattened path-string API is
  post-MVP (§12 [OPEN] #1).
- **Trait registration.** Harmonius R-1.3.7's "register
  Serialize trait against TypeId" is refused — Glibre uses
  codegen-emitted typed trampolines (`fory-codegen.md` §"Decision"
  #4); there is no runtime trait table.
- **`DynamicValue` interchange.** Harmonius R-1.3.5 is refused —
  the editor reads bytes and renders rows; it does not box them
  into a heap-allocated polymorphic value. A `DynamicValue`
  abstraction would re-introduce runtime reflection.
- **Attribute / metadata system.** Harmonius R-1.3.6
  (`range`, `display_name`, `serialization hints`) is **deferred
  to post-MVP** (§12 [OPEN] #2); the MVP blob carries `name`, `tag`,
  `type_kind`, `since`. Display names and ranges live in a future
  per-field annotation table.
- **Persisted format.** The blob is a build artifact, **not** a
  wire-format. It is not a Fory-serialized payload; it is C++
  `constexpr` / `const` static data emitted as `.cpp` source by
  `glibre-foryc`. There is no on-disk `.reflection-blob` file
  format. The `reflection_present : bool` field of
  `SchemaSourceRecord` (SPEC §7.2.1, tag 6) records *whether* the
  blob was emitted; nothing about *what* it contains crosses the
  envelope (§7).

The SRP boundary is sharp: if the descriptor record shape (the
`ReflectionField` columns, the interning rule, the lookup index
shape, the partitioning rule) changes, this design changes.
Anything else — the `.fory` source format, the schema registry's
binary search, the envelope, the migration chain, the editor's
widget tree, the C++ codegen pipeline outside the
`reflection_emitter.cpp` stage — does not.

## 2. Requirements coverage

Mapping of harmonius requirements
(`harmonius/docs/requirements/core-runtime/reflection-and-type-system.md`)
and SPEC §4.9 invariants to glibre MVP coverage. Every entry is
independently re-derived against PHILOSOPHY §6 (no runtime
reflection in shipping builds).

| Source                                                         | Glibre disposition (MVP) | Coverage site                                                                                                  |
|----------------------------------------------------------------|--------------------------|----------------------------------------------------------------------------------------------------------------|
| Harmonius R-1.3.1 — runtime type registry, ≥10k types, O(1)    | **Refused (runtime)**, **Re-derived (editor)** | `core::TypeRegistry` covers ECS storage; `data::SchemaRegistry` (sibling design) covers schema catalog. The `ReflectionBlob` is *not* a runtime registry; it is per-`RegistryEntry` static data the editor reads through the existing `SchemaRegistry::lookup` (`O(log N)` over ~10⁴ FQNs). |
| Harmonius R-1.3.2 — type descriptors with size / align / drop / clone / default | **Re-derived (narrowed)** | §3.1 — blob carries `byte_offset`, `byte_size`, `type_kind`, `since`. Size and align come from the codegen-emitted struct; drop / clone / default ctor are implicit in the typed C++ struct and **never** type-erased into a thunk table (that would be runtime reflection). |
| Harmonius R-1.3.3 — path-based access (`transform.position.x`), <500 ns, 8 segments | **Refused (MVP)**         | Post-MVP. §3.5 walks one level via the recursive `FQN` field-kind; multi-segment path strings deferred to §12 [OPEN] #1. The editor today drives nesting through its own widget tree, not a path string. |
| Harmonius R-1.3.4 — uniform collection trait (Vec / HashMap)   | **Refused (MVP)**         | Post-MVP. §3.1 represents `list<T>` and `map<K,V>` as `type_kind` cases the editor renders in MVP as a read-only summary ("3 items"); structural editing of containers via reflection deferred to §12 [OPEN] #3. |
| Harmonius R-1.3.5 — `DynamicValue` interchange + diff/patch    | **Refused**               | Runtime reflection by definition. The editor reads bytes through the typed `Envelope<T>::deserialize` into a typed value, edits it via widgets, and round-trips through `serialize`. No boxed polymorphic value crosses any boundary. |
| Harmonius R-1.3.6 — attribute system (range, display_name, …)  | **Deferred (post-MVP)**   | §12 [OPEN] #2 — MVP blob carries no attributes; the editor uses field name verbatim, no per-field range clamping, no ordering reorder. Annotations join the blob as a parallel `eastl::span<const ReflectionAttribute>` once two concrete consumers exist. |
| Harmonius R-1.3.7 — register trait impls against TypeId        | **Refused**               | Runtime reflection. Glibre uses codegen-emitted typed trampolines (`fory-codegen.md` §"Decision" #4) — there is no runtime trait table. Editor-only "render this kind of field" dispatch is a `switch` over `ReflectionField::type_kind` (§4.3), compiled into the editor binary. |
| Harmonius R-1.3.8 — `Reflect` trait + derive macro             | **Refused**               | Runtime reflection. Glibre's equivalent is `glibre-foryc` codegen: `.fory` schema → C++ struct + (editor-only) `ReflectionBlob`. No vtable, no trait object. |
| Harmonius R-1.3.9 — `reflect(skip)`, `reflect(rename)`, `reflect(default)` | **Refused (MVP)**         | The `.fory` schema source has no equivalent attributes in MVP. Field ordering, naming, and defaults are implicit in the schema. Post-MVP attribute table (§12 [OPEN] #2) revisits this. |
| Harmonius R-1.3.10 — sub-traits (Struct / Enum / List / Map)   | **Re-derived (narrowed)** | §3.1 — `ReflectionField::type_kind` is a closed sum the editor switches on. This is a *static dispatch* over codegen-known kinds, not a runtime sub-trait registry. |
| Harmonius R-1.3.11 — `FromReflect`                             | **Refused**               | The editor never builds typed values from a dynamic blob; it edits raw bytes between matched `Envelope<T>::deserialize` / `serialize` calls. `FromReflect` is unnecessary. |
| SPEC §4.9 inv. 1 — static-only, no hot-path dispatch           | **Covered**               | §4.4 — `GLIBRE_EDITOR`-gated header; `RegistryEntry::reflection` is `nullptr` in shipping (§7.1).                                |
| SPEC §4.9 inv. 2 — read-only, never patched                    | **Covered**               | §3.6, §6 — all blob storage is `constexpr` or `const`; no API mutates it.                                       |
| SPEC §4.9 inv. 3 — faithfulness (every active tag, no reserved tags, ascending) | **Covered** | §3.1, §6.2 emit-stage rule; §11 unit test `reflection_blob_tag_sorted_active_only` enforces.                  |
| SPEC §4.9 inv. 4 — self-contained name interning               | **Covered**               | §3.2 — interning table per blob, no borrow from outside the middleman.                                          |
| SPEC §4.9 inv. 5 — stripped optional, null in shipping         | **Covered**               | §4.4 surface gating; §7.1 ABI rule; §11 unit test `reflection_blob_null_in_shipping`.                           |
| SPEC §5 §reflection.hpp stub                                   | **Refined**               | §4 restates the stub with editor-only namespace, `tools::Error` arm, and the lookup index added (§3.3).         |
| SPEC §6.2 stage 4 emit step #2                                 | **Covered**               | §6.1 — the `reflection_emitter.cpp` codegen stage produces the blob TUs deterministically (`fory-codegen.md` §"Pipeline").       |
| SPEC §7.2.1 `reflection_present : bool` (tag 6)                | **Covered**               | §7.2 — the `SchemaSourceRecord` field is the build-artifact gate; this design pins that the runtime path never branches on it.   |
| SPEC §8.1 — `ReflectionBlob` interning tables for dropped types do not survive reload | **Covered** | §8 — Mode-A reload: blob spans for dropped FQNs are released after the loader's swap-completion barrier; the editor reloads the blob set on the same frame. |
| SPEC §9.2 — 4 MiB tools-build sub-ceiling, 0 in shipping       | **Covered**               | §9 — heap accounting; allocation rules.                                                                         |
| SPEC §10 — closed sum extension                                | **Covered**               | §10 — three new arms (`BlobLoadFailed`, `FQNNotFound`, `FieldOutOfRange`) routed to `tools::Error` in editor builds; SPEC §10 amendment proposed inline. |
| `reviews/decisions/error-model.md` `std::expected<T, glibre::Error>` boundary | **Covered** | §4 surface — every public function returns `std::expected<T, glibre::Error>` with `tools::Error` leaf arms.     |
| `reviews/decisions/plugin-abi.md` middleman-only ABI surface   | **Covered**               | §7.2 — the editor links the **same** `glibre-types.dylib` symbols runtime does, plus *editor-only* TUs that reference no plugin code.                |
| `reviews/decisions/hot-reload-protocol.md` swap discipline     | **Covered**               | §8 — blob refresh is observed via `SchemaRegistry`'s post-swap barrier; no second hot-reload mechanism.         |
| `reviews/decisions/perf-budget.md` `data` row                  | **Covered**               | §9 — soft sub-frame budget for inspector queries; tool builds only.                                             |
| `reviews/decisions/frame-phases.md` phase ownership            | **Refused (none)**        | The reflection blob owns no frame phase; editor queries run on the editor's UI thread, gated by §6.             |

Coverage rule: every harmonius clause and every SPEC invariant
either lands in this design or is refused with a one-line rationale.
No silent drops.

Glibre-native obligations beyond harmonius:

- **Editor-only by construction.** Harmonius blurred the
  reflection / type-system / property-path stack into one runtime
  facility. Glibre fences runtime reflection out of the engine
  entirely; the entire descriptor table lives behind `GLIBRE_EDITOR`.
  This is the single largest divergence from harmonius and the
  reason ten of its eleven clauses are refused or narrowed.
- **Per-context partitioning.** The editor's asset browser scrolls
  by plugin / context; sibling designs do not. The blob therefore
  carries enough metadata for the registry to group blobs by
  context-prefix without re-parsing the FQN strings (§3.4).
- **Hot-reload-aware refresh.** The blob set is rebuilt on every
  Mode-A reload because plugins may register new schemas; the
  editor must observe the new descriptors on the same frame the
  loader publishes the new registry. The refresh path is documented
  in §8 — a refinement harmonius's runtime registry never had to
  consider because it never partitioned by plugin lifetime.

## 3. Detailed model

### 3.1 The descriptor record

The blob is, on the editor's side, a flat array of `ReflectionField`
records sorted by ascending `tag`. The record is a POD aggregate
designed to be stable under `constexpr` initialisation:

```cpp
namespace glibre::types {

// Closed sum of declarable Fory field kinds, mirroring the
// `.fory` schema grammar (SPEC §7.1). Editor switch over this
// kind is the single dispatch surface (§4.3).
enum class ReflectionKind : std::uint8_t {
    Bool       = 1,
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
    String,             // utf-8 byte run
    Bytes,              // opaque byte run
    Enum,               // type_name resolves to enum FQN
    Struct,             // type_name resolves to generated-type FQN
    List,               // payload type follows in `element_kind`
    Map,                // (k, v) follow in `element_kind` + `value_kind`
    SchemaIdRef,        // identity-only reference; no recursion
};

struct ReflectionField {
    eastl::string_view  name{};         // interned in this blob (§3.2)
    std::uint16_t       tag{0};         // ascending across the array
    ReflectionKind      kind{};         // primary kind
    ReflectionKind      element_kind{}; // for List / Map; default for scalars
    ReflectionKind      value_kind{};   // for Map only
    eastl::string_view  type_name{};    // interned; non-empty for Enum/Struct/SchemaIdRef
    SchemaVersion       since{1};       // first version this tag appears
    std::uint32_t       byte_offset{0}; // byte offset in the generated struct
    std::uint32_t       byte_size{0};   // sizeof of this field's storage slot
};

}  // namespace glibre::types
```

Field semantics:

- **`name`** — the field name from the `.fory` source; the
  interning table for the blob owns the bytes (§3.2). UTF-8.
  Maximum length is 255 bytes (a `.fory` lint rule, not enforced
  by the runtime).
- **`tag`** — the wire tag declared by the `.fory` schema. Always
  in the range `1..=65534`; tag 0 is reserved (PHILOSOPHY §11
  collapse note).
- **`kind`** — discriminates the editor's render switch (§4.3).
  Ordered numerically so the editor's compiled `switch` picks up a
  jump table with no padding gaps.
- **`element_kind`** / **`value_kind`** — populated only for
  containers; default-constructed otherwise. The editor reads them
  conditionally on `kind`.
- **`type_name`** — for `Enum`, `Struct`, and `SchemaIdRef` kinds
  this is the FQN string of the referenced type. The editor uses
  it to recurse into the nested type's blob (§3.5). Non-recursive
  kinds leave it empty.
- **`since`** — the first `SchemaVersion` in which this tag was
  declared. The editor uses it to dim or annotate fields added
  after the version of the byte buffer being viewed.
- **`byte_offset`** / **`byte_size`** — the field's location and
  width inside the generated struct's memory layout. The editor
  uses these to compute the byte slice it edits inside a typed
  buffer; they are codegen-emitted from the same struct generator
  that wrote the `<Type>.cpp` body.

The blob is sorted by ascending `tag`, matching the generated
struct's field order (§4.9 inv. 3). Reserved tags (declared in the
`.fory` source's `reserved` set) **do not** appear; the codegen
filters them out.

### 3.2 Per-blob name interning

Every blob owns a contiguous byte arena of UTF-8 strings (one for
each unique `name` and `type_name` referenced) and emits its
`eastl::string_view` members as views into that arena. Codegen lays
the arena out as a `static constexpr eastl::array<char, N>` per
blob, deduplicates strings within the blob, and writes the views
as `(arena_offset, length)` pairs at C++ initialisation time.

Cross-blob interning is **not** performed: each blob's arena is
self-contained so that:

1. The editor can free a single blob (e.g. for a dropped FQN after
   Mode-A reload) without dangling references from other blobs.
2. The blob's compile-time foot-print is bounded by its own
   schema's name set, not by the global FQN catalog.
3. The codegen TU graph is a forest, not a DAG: each `<Type>.cpp`
   reflection-emitter section depends only on its own schema,
   matching `fory-codegen.md` §"Pipeline" determinism.

The arena is `constexpr` when the schema is a leaf (no nested
generated types) and `const` static when it references another
generated type (the `type_name` of a nested `Struct` / `Enum`
field is the *referenced* type's FQN, which the codegen also
interns into this blob — duplicating bytes in service of inv. 4).

The duplication cost is bounded: average FQN length is ~32 bytes;
a typical `Generated Type` references at most ~3 nested types. The
total per-blob name arena ceiling is **2 KiB** (a `.fory` lint rule
for MVP); the 4 MiB blob sub-ceiling (§9) accommodates ~2000 blobs
worst-case.

### 3.3 Per-blob tag-lookup index

The editor's inspector loop renders rows in tag order (the natural
iteration over `fields`); however it also needs to look up a single
field by tag when handling an inbound `EditCommand` re-applied
after redo or a network sync. To make this `O(log N)` without
allocating a side index at runtime, the codegen emits a **secondary
sorted span**:

```cpp
struct ReflectionBlob {
    SchemaId                              schema{};
    SchemaVersion                         version{1};
    eastl::span<const ReflectionField>    fields{};       // tag-sorted ascending
    eastl::span<const std::uint16_t>      tag_to_index{}; // dense if max_tag < 256, sparse otherwise
};
```

`tag_to_index` is one of two shapes (codegen picks at emit time):

1. **Dense** — `tag_to_index.size() == max_tag + 1`; index `t`
   gives `fields[tag_to_index[t]]` directly, or a sentinel value
   `0xFFFF` if `t` is reserved or absent. Used when
   `max_tag < 2 × fields.size()` (no significant gap waste).
2. **Sparse** — `tag_to_index.size() == fields.size()`; the editor
   binary-searches `fields[].tag` directly. Used when the schema
   reserves large tag-number ranges between active fields.

The choice is recorded implicitly: dense index has
`tag_to_index.size() != fields.size()`. The editor calls a
`field_for_tag(blob, tag)` accessor (§4.3) that picks the right
form internally.

For MVP the dense form is the modal case (≤8 fields per schema,
contiguous tag numbers); the sparse form is reserved for evolved
schemas that have retired several tags into the `reserved` set.

### 3.4 Per-context partitioning

The editor's asset browser groups types by *context* (the dotted
prefix before the leaf, e.g. `glibre.physics.*`). To support this
without scanning the whole catalog at every panel-frame, the
codegen emits a **per-context blob span** alongside the per-FQN
blobs:

```cpp
namespace glibre::types::reflection_table {

struct ContextPartition {
    eastl::string_view               context_prefix{};   // e.g. "glibre.physics"
    eastl::span<const RegistryEntry* const> entries{};   // tag-sorted by FQN; entries with reflection != nullptr
};

extern const eastl::span<const ContextPartition>
    glibre_types_reflection_partitions;

}  // namespace glibre::types::reflection_table
```

The partition span is itself emitted only in editor builds and is
empty (zero-sized span over a `nullptr` data pointer) in shipping
builds — the `extern const` declaration always exists so the
editor's translation units can compile against the same header in
both build profiles, but the data is zero in shipping (consistent
with §4.9 inv. 5 and §7.1 below).

Partitioning rule:

- Context prefix is the FQN minus its final dot-separated segment
  (`glibre.physics.RigidBody` → `glibre.physics`). The codegen
  computes prefixes deterministically from the FQN-sorted catalog
  and emits one `ContextPartition` per unique prefix.
- Within a partition, entries are FQN-sorted (matches the
  `SchemaRegistry`'s primary order — sibling design
  `specs/data/schema-registry-design.md` §3.4).
- A registry entry whose `reflection == nullptr` (because a single
  schema disabled emission via a `.fory` opt-out, post-MVP) is
  **excluded** from its partition. MVP has no opt-out, so all
  entries appear.

### 3.5 Recursion through nested generated types

A `ReflectionField` whose `kind == Struct` or `kind == Enum` (or
`SchemaIdRef`) carries a `type_name` FQN. The editor resolves the
nested type by calling `SchemaRegistry::lookup(SchemaId{type_name})`
and reading the resulting `RegistryEntry::reflection` blob.

Recursion rules:

1. **Non-cyclical.** A schema may not reference itself directly or
   transitively (`fory-codegen.md` §"ABI Stability Rules" #3 — the
   `Foryc` reachability check rejects cycles at codegen time).
   The editor therefore needs no cycle guard.
2. **Bounded depth.** A `.fory` lint rule pins maximum nesting
   depth at 8 (matches harmonius R-1.3.3's "8 segments" — the only
   piece of R-1.3.3 we keep). Nesting deeper is a codegen error,
   not a runtime guard.
3. **One level at a time.** The editor reads one nested blob per
   inspector row expansion; it does *not* construct a flattened
   "path" string. A future post-MVP path-string API (§12 [OPEN] #1)
   would compose nested lookups but does not change the blob shape.

### 3.6 Lifetime and immutability

Per §4.9 inv. 2 the blob is read-only after load; per §4.9 inv. 4
its strings live in the blob's own arena. Concretely:

- **Static-init publication.** Each `<Type>.cpp` editor-only TU
  declares the blob as `inline constexpr ReflectionBlob
  glibre_types_<ctx>_<Type>_reflection { ... }` when the schema
  is a leaf (the field set has no `Struct` / `Enum` references
  whose addresses are not yet known) or as
  `inline const ReflectionBlob ...` when references force `const`-
  but-not-`constexpr` initialisation (the inner blob's address is
  another TU's symbol).
- **Registry wiring.** The codegen-emitted
  `_registry.cpp` (SPEC §6.2 stage 4 step #3) sets
  `RegistryEntry::reflection = &glibre_types_<ctx>_<Type>_reflection`
  in editor builds and `nullptr` in shipping (§7.1).
- **No runtime mutation.** No public function takes a `ReflectionBlob*`
  by non-const pointer; the editor's read API returns
  `const ReflectionBlob*` (§4.3). Edits to a `.fory` file
  regenerate the middleman build; the live blob is never patched.

## 4. Public surface

The §5 `reflection.hpp` stub from `specs/data/SPEC.md` is
authoritative in shape; this section restates it with editor-only
gating, tool-build-only declarations, and the lookup index added in
§3.3.

### 4.1 Header location and gating

The header is split between the `glibre-types.dylib` middleman and
the editor binary:

- `include/glibre/types/reflection.hpp` — the **types** (`ReflectionField`,
  `ReflectionBlob`, `ContextPartition`). Always declared; the
  `RegistryEntry::reflection` slot needs a forward declaration in
  shipping builds so the registry struct's layout is identical
  across configurations (§7.1).
- `tools/glibre-editor/include/glibre/editor/reflection.hpp` — the
  **API** (`field_for_tag`, `partitions`, `lookup_blob`, recursion
  helpers). Compiled only when `GLIBRE_EDITOR` is defined.

The runtime translation units never `#include` the editor header.
A `static_assert(!defined(GLIBRE_EDITOR) || GLIBRE_EDITOR_OK, ...)`
guard inside the editor TU prevents accidental inclusion via a
plugin's transitive include graph.

### 4.2 Types (always declared, in the middleman header)

```cpp
// include/glibre/types/reflection.hpp
#pragma once
#include <EASTL/span.h>
#include <EASTL/string_view.h>
#include <cstdint>
#include "glibre/types/identity.hpp"   // SchemaId, SchemaVersion

namespace glibre::types {

enum class ReflectionKind : std::uint8_t {
    Bool = 1, I8, I16, I32, I64, U8, U16, U32, U64, F32, F64,
    String, Bytes, Enum, Struct, List, Map, SchemaIdRef,
};

struct ReflectionField {
    eastl::string_view name{};
    std::uint16_t      tag{0};
    ReflectionKind     kind{};
    ReflectionKind     element_kind{};
    ReflectionKind     value_kind{};
    eastl::string_view type_name{};
    SchemaVersion      since{1};
    std::uint32_t      byte_offset{0};
    std::uint32_t      byte_size{0};

    constexpr bool operator==(const ReflectionField&) const noexcept = default;
};

struct ReflectionBlob {
    SchemaId                              schema{};
    SchemaVersion                         version{1};
    eastl::span<const ReflectionField>    fields{};
    eastl::span<const std::uint16_t>      tag_to_index{};

    constexpr bool operator==(const ReflectionBlob&) const noexcept = default;
};

struct ContextPartition {
    eastl::string_view                          context_prefix{};
    eastl::span<const RegistryEntry* const>     entries{};
};

}  // namespace glibre::types
```

These types are POD-shaped, trivially-copyable, and have no
non-default constructors. They appear identically in shipping and
editor builds; only the *data* differs (§7.1).

### 4.3 Editor-only API (gated behind `GLIBRE_EDITOR`)

```cpp
// tools/glibre-editor/include/glibre/editor/reflection.hpp
#pragma once

#if !defined(GLIBRE_EDITOR)
#  error "glibre/editor/reflection.hpp included from a non-editor build"
#endif

#include <expected>
#include "glibre/types/reflection.hpp"
#include "glibre/types/registry.hpp"
#include "glibre/error.hpp"          // glibre::Error (core), Result<T>

namespace glibre::editor::reflection {

// Look up a blob by FQN. Returns nullptr arm only via the typed
// failure path (see Result<T> below); the function never returns
// a default-constructed ReflectionBlob.
[[nodiscard]] auto lookup_blob(SchemaId fqn) noexcept
    -> Result<const types::ReflectionBlob*>;

// Look up a field by tag inside a blob. Picks the dense or sparse
// form internally (§3.3).
[[nodiscard]] auto field_for_tag(
    const types::ReflectionBlob& blob,
    std::uint16_t                tag) noexcept
    -> Result<const types::ReflectionField*>;

// Resolve a Struct/Enum/SchemaIdRef field's type_name to its blob.
// One-level recursion (§3.5).
[[nodiscard]] auto resolve_nested(
    const types::ReflectionField& field) noexcept
    -> Result<const types::ReflectionBlob*>;

// Iterate all blobs whose FQN starts with context_prefix.
// Returns an empty span if no partition matches; never an error.
[[nodiscard]] auto partition(
    eastl::string_view context_prefix) noexcept
    -> eastl::span<const types::RegistryEntry* const>;

// All partitions; iterates the whole catalog grouped by context.
[[nodiscard]] auto all_partitions() noexcept
    -> eastl::span<const types::ContextPartition>;

}  // namespace glibre::editor::reflection
```

Error model: per `reviews/decisions/error-model.md`, all public
boundaries return `std::expected<T, glibre::Error>`. The
`tools::Error` enum (declared in `specs/tools/SPEC.md` §5) is
extended with the three reflection-blob arms in §10. The data
context's `data::Error` is **not** extended; reflection failures
surface as editor-context errors because the editor is the only
caller. (This keeps `data::Error` a leaf about *spine* failures,
not introspection failures, per §"Composition Rules" #1.)

### 4.4 Build gating

Per SPEC §4.9 inv. 5, blob emission is controlled by a build flag.
Concretely:

1. The `glibre-types.dylib` CMake target defines a private
   `GLIBRE_EMIT_REFLECTION` option, default **OFF in shipping**
   profiles (`-DGLIBRE_PROFILE=ship`) and **ON in editor / tools**
   profiles (`-DGLIBRE_PROFILE=editor`).
2. When `GLIBRE_EMIT_REFLECTION=ON`, the codegen-emitted
   `<Type>.cpp` files include the `ReflectionBlob` initialisation
   block; when OFF, that block is `#if`-d out and the
   `RegistryEntry::reflection` field is `nullptr`.
3. `GLIBRE_EDITOR` is defined **only** in the editor binary's
   compilation; it gates the editor-side API header
   (`tools/glibre-editor/include/glibre/editor/reflection.hpp`).
4. The two flags are independent: a tools build of
   `glibre-types.dylib` (with `GLIBRE_EMIT_REFLECTION=ON`) is
   linked into the editor binary (which defines `GLIBRE_EDITOR`).
   A shipping middleman + shipping runtime never defines either
   flag, and the runtime cannot accidentally call the editor API
   (the header errors-out on inclusion).
5. ABI: the build flag does **not** change the middleman SONAME.
   The same-shape `RegistryEntry` (which always has a
   `const ReflectionBlob*` slot) is part of the runtime ABI; whether
   the slot is null is a per-build *value*, not a *layout*. The
   ABI hash (`fory-codegen.md` §"ABI Stability Rules") therefore
   does not change between shipping and editor profiles. (See §7.2.)

### 4.5 Surface rules

- `noexcept` on every export. Per `error-model.md`,
  `-fno-exceptions` is engine-wide; no exception crosses any
  reflection boundary.
- All return types are pointers or POD spans; no `eastl::vector`,
  `std::vector`, `std::string`, or other heap-owning value type
  appears in any signature.
- Blob pointers are stable for the entire lifetime of the loaded
  middleman build (which is a process lifetime in MVP, since
  Mode-B middleman swap is out of scope per SPEC §8.3).
- `partition` and `all_partitions` return spans into static data;
  the editor must not store their data pointers across a Mode-A
  reload barrier (§8).

## 5. Hot / cold path split

The reflection blob is, by construction, **off the shipping hot
path entirely**. PHILOSOPHY §6 forbids runtime reflection in
shipping; the blob is `nullptr` in shipping (§4.4). Hot/cold
budget therefore applies only to the editor's UI loop.

| Path        | Trigger                                              | Frequency                          | Budget                                                                |
|-------------|------------------------------------------------------|------------------------------------|------------------------------------------------------------------------|
| Hot (editor inspector inner loop) | `field_for_tag` while drawing rows for a selected entity    | One call per visible field per inspector frame; ≤ 100 fields/frame typical | ≤ 50 ns per call (`tag_to_index` indexed lookup); ≤ 5 µs total/frame   |
| Hot (editor inspector inner loop) | `lookup_blob` once per `(Entity, ComponentType)` pair       | Once per inspector view per panel-frame; ≤ 32 components/frame typical    | ≤ 10 ns per call (delegates to `SchemaRegistry::lookup` `O(log N)`)    |
| Warm (asset browser scroll)       | `partition` per visible group                              | Once per scroll cell per editor frame; ≤ 16 groups/frame typical          | ≤ 100 ns per call (linear search over ≤ 64 partitions); ≤ 2 µs/frame   |
| Warm (asset browser scroll)       | `all_partitions` once when the browser opens                | One call per panel-open event                                             | < 50 ns (returns a static span)                                        |
| Cold (recursion on row expand)    | `resolve_nested` when the user expands a nested struct row | Per expansion event                                                       | < 1 µs per call (one `lookup_blob` + bounds check)                      |
| Cold (Mode-A reload refresh)      | Editor reloads its blob set after a plugin reload          | Per Mode-A reload event                                                   | < 1 ms total (refreshes ≤ 200 partitioned spans); §8                    |
| (none — shipping)                 | —                                                          | —                                                                         | The shipping build performs zero reflection ops; `RegistryEntry::reflection` is `nullptr` and is never dereferenced. |

The **inspector inner loop** is the critical path. Inside one
inspector panel-frame the editor:

1. Iterates the selected entities (≤ 16).
2. For each entity, iterates its components (≤ 32).
3. For each component, calls `lookup_blob(component_schema_id)` once
   to obtain the blob (cached per panel-frame in the
   `InspectorView`'s state).
4. Iterates `blob.fields` in tag order (≤ 32 fields per typical
   component) and calls `field_for_tag` only on the *currently-being-
   redrawn* field (one per row).

Steady-state inspector cost is therefore:
`16 × 32 × (10 ns + 32 × 50 ns) ≈ 800 µs per inspector frame`
worst-case — well inside the editor's per-frame UI budget. The blob
itself contributes ~50 ns per visible row.

The **codec internal split** (within `lookup_blob` /
`field_for_tag`):

- **Hot inner.** Tag indexing in the dense form is one bounded
  array index; in the sparse form it is a `eastl::lower_bound` over
  the field array. Both branch-prediction-friendly under the
  modal "user is scrolling, tag is in range" case.
- **Cold dispatcher.** None — there is no fallback path. A
  reflection failure is a typed error (§10), not a slow-path
  retry.

## 6. Concurrency

The `ReflectionBlob` aggregate is **read-only after load**. All
public API functions in §4.3 are **pure functions** of their inputs:

1. They read only the constant blob array, the `SchemaRegistry`
   instance (which is itself read-only after static-init per SPEC
   §4.5 inv. 4), and the partition span — all `static const`.
2. They write nothing.
3. They allocate nothing.
4. They do not log, touch the filesystem / network / clock / RNG,
   or mutate any global state.

Thread-safety guarantees:

- Two threads may concurrently call any of the §4.3 functions
  with arbitrary (overlapping or disjoint) arguments **with no
  synchronization**. The shared data is `const`.
- The editor's UI thread is the only thread that calls reflection
  in MVP. A future asset-bake worker that wants to read blobs
  (e.g. to compute thumbnails using field metadata) inherits the
  same lock-free guarantee.
- No atomic, no mutex, no read-write lock appears in the
  reflection implementation. The single concurrency primitive is
  C++'s "two reads of `const` data are race-free" rule.

The blob set itself is replaced wholesale at Mode-A reload (§8);
during the swap the loader holds the engine-wide phase-8
exclusive lock (`hot-reload-protocol.md` §"Step 2 — Swap"), which
already happens to fence the editor's UI thread out of any
reflection access. Post-swap the editor sees the new
`SchemaRegistry::instance()` view and its `RegistryEntry::reflection`
pointers; it never observes a torn read.

## 7. Persistence + ABI

### 7.1 The blob is a build artifact, not a wire format

The reflection blob is **never serialized**. It is not a Fory
payload, has no envelope, has no version-migration story, and does
not appear in any save file or asset-cook output. It is C++ code
the codegen emitted into a `.cpp` translation unit.

This is the load-bearing distinction from `Schema`,
`SchemaRegistry`, `MigrationChain`, and `Envelope` — those are
on the persistence path because their bytes cross a save boundary;
the reflection blob's "bytes" are the compiled `.text` /
`.rodata` of the editor build itself.

Consequences:

- No `.fory` schema describes `ReflectionBlob`. The middleman's
  meta-schemas (SPEC §7.2.1 .. §7.2.4) are
  `SchemaSourceRecord`, `MigrationTableRecord`, `AbiHashManifest`,
  and `EnvelopeHeader` — four meta-schemas, none of which describe
  the reflection blob.
- The `reflection_present : bool` field of `SchemaSourceRecord`
  (SPEC §7.2.1, tag 6) is the *only* persisted bit about the
  blob; it records whether the `<Type>.cpp` for this schema
  carries reflection data. The runtime path never branches on this
  field; only the editor reads it (for example, to dim catalog
  entries whose blob was stripped at build time).
- Save-file readers that encounter an envelope do not need the
  blob. The blob is not on the deserialize path.

### 7.2 ABI surface and the SONAME rule

The `RegistryEntry::reflection` slot has type
`const ReflectionBlob*`. Its **layout** is part of the middleman's
ABI surface; its **value** is a build-time decision per §4.4:

- Shipping middleman build: `nullptr` for every entry. Editor-only
  TUs (the `<Type>.cpp` reflection blocks) are not compiled and not
  linked.
- Editor middleman build: pointer to a `static const ReflectionBlob`
  in the editor-only TU.

Because the *layout* is identical, the middleman's
`glibre_types_abi_hash` (SPEC §4.4) is identical across the two
builds — a plugin compiled against a shipping middleman can be
loaded by an editor middleman of the same version, and vice versa.
This is the design's most important ABI consequence: the editor
adds reflection without bumping the middleman SONAME.

Extension rule: adding fields to `ReflectionField` or
`ReflectionBlob` is a **layout change** that *does* bump the
middleman SONAME (and the ABI hash) — the registry's layout depends
transitively. Extending `ReflectionKind` is *also* a layout change
in MVP because the enum width is `u8` and an extension could be
read out-of-range by the editor. Both paths are major-version bumps
(§7.1 of the envelope-serdes design's wire-format-stability rule).

### 7.3 Cross-build read invariants

There are no cross-build read invariants for the blob — the blob
is in-process data only. A shipping process never reads a blob; an
editor process always reads its own (compiled-in) blob. There is no
"older blob from a previous build" scenario equivalent to the
envelope's source-hash-fallback path.

### 7.4 Allocation and storage class

Per `reviews/decisions/perf-budget.md` §"Allocator Rules" and SPEC
§9.2:

- All blob storage is `static const` or `static constexpr` —
  zero per-frame allocation, zero per-context-tag attribution.
  The 4 MiB sub-ceiling in SPEC §9.2 covers the resident `.rodata`
  size of all blobs combined; it is reserved at link time, not
  allocated at runtime.
- Editor-side query state (e.g. an inspector's cached
  `const ReflectionBlob*` per `(Entity, ComponentType)`) is owned
  by the editor's `tools` context heap, not by `data`.
- The blob aggregate itself contributes **zero** to `data`'s
  per-frame heap accounting — it is `.rodata`, not heap.

## 8. Hot-reload

The reflection blob participates in the engine-wide hot-reload
protocol (`reviews/decisions/hot-reload-protocol.md`) as a
**read-only consumer** of the registry's swap discipline. There is
no second hot-reload mechanism for the blob; it follows the
registry's.

### 8.1 Mode-A reload (plugin swap)

The common case: a single plugin's `.dylib` is swapped at frame
phase 8. Per SPEC §4.5 inv. 4 the registry is read-only after
static-init *of the live middleman build*; static-init re-runs at
plugin admission for plugin-private types whose schemas are
emitted by the new plugin's static-init.

Refresh path:

1. **Pre-swap (loader phase 8.0).** The editor's inspector and
   asset-browser panels capture the surviving entities and
   their visible components. The editor *does not* dereference
   blob pointers during the swap window; the loader's exclusive
   lock fences UI thread access.
2. **Swap (loader phase 8.1).** The new plugin's static-init
   registers its schemas with `SchemaRegistry`. New
   `RegistryEntry`s gain populated `reflection` pointers (in
   the editor build) into the *new* plugin's `.text` / `.rodata`.
   Outgoing plugin's entries are removed; their blob pointers
   become invalid by `dlclose`.
3. **Migrate (loader phase 8.2).** The data context's
   `migrate(...)` (SPEC §8.2) walks ECS storage and migrates
   bytes. The reflection blob is *not* consulted; the migration
   reads `MigrationChain` from the registry. This is the design's
   key point — the blob does not feed migration.
4. **Resume (loader phase 8.3).** The editor's UI thread
   resumes. On the next inspector panel-frame, the editor calls
   `lookup_blob` for each visible component. Components whose
   `SchemaId` no longer exists in the registry surface as
   `tools::Error::FQNNotFound` (§10) and the inspector renders
   a "type removed by reload" placeholder instead of the form.
5. **Partition refresh.** The
   `glibre_types_reflection_partitions` extern data is a
   pointer-set into the registry's entry array. After the swap
   the partition span's *contents* (the entry pointers it
   indexes) reflect the new registry; the editor re-walks the
   partitions on its next asset-browser frame and observes the
   new layout.

Per SPEC §8.1 ("State that does not survive"), editor-only
`ReflectionBlob` interning tables for types whose schemas
dropped between Q (outgoing) and P (incoming) **do not survive**
the reload — they belong to the unloaded `.dylib` and the editor
must drop any cached pointers into them. The `lookup_blob`
caller-cache rule (§4.3 — "blob pointers are stable for the
lifetime of the loaded middleman build") expressly does not
extend across a Mode-A reload of a *plugin* that owned that
blob; the editor's cache is keyed by `SchemaId` and refreshed on
each inspector panel-frame, so stale pointers are detected on the
first reload-frame inspector cycle.

### 8.2 Mode-B reload (middleman swap, out of MVP)

Out of MVP scope per SPEC §8.3. When it lands, the entire blob
catalog is replaced as a unit (the new middleman build carries its
own `<Type>.cpp` blobs); the gate-3 meta-schema bootstrap rule
applies symmetrically to the (still empty in MVP)
`reflection`-relevant meta-schemas. No incremental refresh is
designed for Mode-B reload because Mode-B is itself wholesale by
§8.3 inv. 1.

### 8.3 Refusal cases

The reflection blob contributes to **no** hot-reload refusal arm.
A blob mismatch (e.g. plugin Q exposed `glibre.physics.RigidBody`
v3 with field `restitution` and plugin P exposes the same FQN at
v3 with field `bounciness`) is detected upstream by the
`SchemaRegistry`'s source-hash check (sibling
`specs/data/schema-registry-design.md` §8): same FQN, same
version, different `source_hash` → `SchemaRegistryConflict`. The
blob shape is downstream of that gate; if the gate refuses the
swap, the blob is never consulted.

The editor's inspector therefore never sees a blob whose schema
has *silently* changed shape — either the registry rejected the
swap and the blob is unchanged, or the registry accepted a clean
version bump and the new blob describes the new fields. Both
paths are observable to the editor through the existing
`SchemaRegistry::lookup` API; no reflection-specific refresh
event is exposed.

### 8.4 What survives a swap (summary)

| State                                                             | Survives Mode-A reload? | Notes                                                    |
|-------------------------------------------------------------------|-------------------------|----------------------------------------------------------|
| Blob pointers for FQNs registered by surviving plugins            | Yes (pointers stable)   | Same `.text` / `.rodata` segment.                        |
| Blob pointers for FQNs registered by the **outgoing** plugin only | No (`dlclose` invalid)  | Editor cache must invalidate; per-panel-frame refresh.   |
| Blob pointers for FQNs registered by the **incoming** plugin only | New (post-swap)         | First observed on the post-swap inspector frame.         |
| Per-context partition spans                                       | Refreshed in place      | Partition span pointers are static; their indexed entries are current. |
| Editor's `InspectorView` cached blob pointer                      | Discarded per panel-frame | The view re-`lookup_blob`s on every frame; no cross-frame stale pointer. |

## 9. Performance

The reflection blob is **editor-only** and contributes **zero** to
the shipping per-frame budget. Editor-side it has a soft sub-frame
latency target: every reflection query must complete within the
editor's UI frame quantum (typically 16 ms at 60 Hz, but the editor
targets 8 ms-or-better main-thread headroom under all S1 fixtures).

### 9.1 Per-call wall-time budget (editor build, M-series Apple Silicon, `-O2 -fno-exceptions`)

| Call                          | Wall-time target | Allocation | Mechanism                                                         |
|-------------------------------|------------------|------------|-------------------------------------------------------------------|
| `lookup_blob(fqn)`            | ≤ 10 ns          | 0          | Delegates to `SchemaRegistry::lookup` (sibling design — `O(log N)` over ~10⁴ entries; benchmark `schema_registry_lookup.bench.cpp`). |
| `field_for_tag(blob, tag)`    | ≤ 50 ns          | 0          | Dense form: one indexed read; sparse form: `eastl::lower_bound` over ≤ 32 fields. |
| `resolve_nested(field)`       | ≤ 1 µs           | 0          | One `SchemaRegistry::lookup` plus a null-check.                    |
| `partition(prefix)`           | ≤ 100 ns         | 0          | Linear search over ≤ 64 `ContextPartition`s.                       |
| `all_partitions()`            | < 50 ns          | 0          | Returns a static span; no work.                                    |

### 9.2 Per-frame editor budget

The inspector inner loop's worst-case per-frame contribution
(decomposed in §5):

- 16 selected entities × 32 components × `lookup_blob` (10 ns) =
  **5.12 µs**.
- 16 × 32 × 32 visible fields × `field_for_tag` (50 ns) =
  **819 µs**.
- 16 expand-row events × `resolve_nested` (1 µs) = **16 µs**.

Total per-frame reflection cost: **≤ ~1 ms in the worst case**,
**≤ ~50 µs** in the steady-state (one entity selected, one
component visible, ~10 fields). The editor's per-frame UI budget
amply absorbs this; reflection is not the dominant cost.

### 9.3 Per-context heap

Per SPEC §9.2 the `ReflectionBlob` sub-ceiling is **4 MiB** in
tools builds and **0** in shipping. The 4 MiB ceiling covers:

- Per-blob `ReflectionField` arrays: ~10⁴ schemas × ~16 fields/avg
  × 56 bytes/field ≈ **9 MiB worst-case** — exceeds the ceiling.
  Mitigation: the MVP catalog is closer to 10³ schemas
  (`fory-codegen.md` §"Pipeline" projection); the 4 MiB ceiling
  holds for ~4500 schemas at the same average fan-out. Sibling
  task-breakdown spike will amend the ceiling if the catalog
  grows past 4500 schemas before MVP ships. Open Question §12 [OPEN] #4.
- Per-blob name interning arenas: ≤ 2 KiB per blob × ~10³ blobs
  ≈ 2 MiB.
- Tag-to-index spans: 2 bytes per slot × ~16 slots per blob × ~10³
  blobs ≈ 32 KiB (negligible).
- `ContextPartition` array: ~10² partitions × 24 bytes/partition
  ≈ 2.4 KiB (negligible).

The 4 MiB is `.rodata` resident; the runtime never touches the
heap for reflection storage. Per `perf-budget.md` "Allocator Rules",
zero allocation in any reflection function — the same discipline
the schema registry follows for its 8 MiB.

### 9.4 No CI gate (editor-only)

Per SPEC §9.4 the four CI gates run benchmarks for envelope
round-trip, registry lookup, migration dispatch, and heap ceiling.
**No reflection benchmark runs in CI** because reflection is
editor-only — the editor's frame budget is a soft target, not a
gate-able one. The editor has its own profiling harness
(`tools/SPEC.md` §10 `.glibre-trace`) that records inspector frame
times; reflection cost is observable there.

A diagnostic build with `GLIBRE_ALLOC_STRICT=1` (`perf-budget.md`)
exercises the editor under a synthetic "every component selected"
fixture and asserts that resident bytes under the
`tools` context tag stay within budget; reflection's 4 MiB falls
within `data`'s tag, not `tools`'s, and the same `GLIBRE_ALLOC_STRICT`
build asserts the 4 MiB ceiling on the `data` tag.

## 10. Failure modes

The reflection blob's failure surface extends `tools::Error`
(`specs/tools/SPEC.md` §5) with three editor-context arms. The
`data::Error` enum (SPEC §10.1) is **not** extended — reflection
is editor territory; routing failures into the data context's
closed sum would violate `error-model.md` §"Composition Rules" #1
(per-context enums are leaves; cross-context translation happens
at the call site that crosses the boundary).

| Arm                                  | Trigger                                                                                                                            | Detection point                                          | Payload fields                                                  | Recovery                                       | Severity | Mapping to engine-wide `glibre::Error` |
|--------------------------------------|------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------|------------------------------------------------------------------|------------------------------------------------|----------|----------------------------------------|
| `tools::Error::BlobLoadFailed`       | `lookup_blob(fqn)` succeeded at the registry layer but `RegistryEntry::reflection == nullptr`. Fires only in a *misconfigured* editor build (the `GLIBRE_EMIT_REFLECTION` flag was OFF for some `<Type>.cpp` but ON for others). | `editor::reflection::lookup_blob` post-`SchemaRegistry::lookup` null-check. | `schema = fqn`. | refuse query — inspector renders "no descriptor for type" placeholder, asset browser omits the entry. | error    | passed through as the `tools::Error` arm of `glibre::Error::Variant`. |
| `tools::Error::FQNNotFound`          | `lookup_blob(fqn)` issued for an FQN that has no registry entry. Fires when the editor caches a stale `SchemaId` across a Mode-A reload (§8) or when a plugin holding a referenced FQN unloaded. | `editor::reflection::lookup_blob` after `SchemaRegistry::lookup` returns its `SchemaUnknown` error.    | `schema = fqn`. | refuse query — inspector renders "type unloaded" placeholder; the editor's cache evicts the entry. | error    | passed through as the `tools::Error` arm; never wrapped into `core::Error::HotReloadRefused` because the reload itself succeeded — only the editor's stale cache is the issue. |
| `tools::Error::FieldOutOfRange`      | `field_for_tag(blob, tag)` issued with a `tag` not present in the blob (either a reserved tag or a tag never declared). Fires when an `EditCommand` from undo / redo or a network sync references a tag that was retired. | `editor::reflection::field_for_tag` after the dense / sparse lookup misses. | `schema = blob.schema`, `tag = the offending tag`. | refuse query — the `EditCommand` is rejected by the editor's command stack with a `tools::Error::CommandConflict` wrap upstream. | warn     | passed through as the `tools::Error` arm; the `CommandStack` translates to its own `CommandConflict` arm at its boundary (`specs/tools/SPEC.md` §4.7). |

`tools::Error` arm definitions (proposed amendment to
`specs/tools/SPEC.md` §5; flagged as §12 [OPEN] #5):

```cpp
namespace glibre::tools {
enum class Error : std::uint16_t {
    LayoutLoadFailed,        // already in §5
    TraceWriteFailed,        // already in §5
    InspectorUnknownType,    // already in §5
    CommandConflict,         // already in §5
    Refused,                 // already in §5
    BlobLoadFailed,          // §10 — new
    FQNNotFound,             // §10 — new
    FieldOutOfRange,         // §10 — new
};
}
```

The amendment is deliberately additive: the existing five arms are
unchanged; the three new arms are appended, preserving tag-value
stability for the existing arms (per `error-model.md` "Composition
Rules" #5 — removing an enumerator is breaking; appending is not).

**Recovery vocabulary** (mirrors SPEC §10.2):

- **refuse query** — the editor function returns
  `unexpected(...)`; the caller (inspector or asset browser) renders
  a placeholder row instead of the form. The world is not mutated.

**Severity** mirrors `error-model.md` §"Logging / Telemetry":

- **error** — `BlobLoadFailed`, `FQNNotFound`. Logged at the
  editor's per-panel handler (one `glibre::log_error(err, error)`
  per panel-frame, deduplicated by FQN per `error-model.md`
  §"Logging / Telemetry" #3 spirit).
- **warn** — `FieldOutOfRange`. The arm is benign (an `EditCommand`
  that no longer applies); the editor logs once and skips.

**Logging discipline.** Per `error-model.md` §"Logging /
Telemetry" #1 the reflection layer **does not log** from inside
`editor::reflection::*`; the calling editor panel logs once.

**Detection sequencing.** `lookup_blob` calls
`SchemaRegistry::lookup` first; that returns `data::Error::SchemaUnknown`
if the FQN is unknown. `lookup_blob` translates `SchemaUnknown` to
`tools::Error::FQNNotFound` at its boundary (the cross-context
translation rule from `error-model.md` §"Composition Rules" #2).
Only when the registry returned a valid entry but the
`reflection` slot is null does `lookup_blob` raise
`BlobLoadFailed`. The two arms are therefore disjoint.

## 11. Test plan

Acceptance is a Catch2 test suite under `tests/data/reflection/`
(unit tests; runs in any build with `GLIBRE_EMIT_REFLECTION=ON`)
plus an integration suite under `tests/tools/reflection/` (driven
through a headless editor harness; runs only in `editor` profile).
The unit tests are owned by the per-issue `type:plan` issues that
this design feeds.

### 11.1 Unit tests

Descriptor round-trip and shape (one Catch2 case per fixture):

| Test                                              | Asserts                                                                                                              |
|---------------------------------------------------|----------------------------------------------------------------------------------------------------------------------|
| `reflection_blob_layout`                          | `sizeof(ReflectionField) == 56` (or whatever the codegen-emitted layout fixes); `is_trivially_copyable_v == true`     |
| `reflection_blob_tag_sorted_active_only`          | For every emitted blob, `fields[i].tag < fields[i+1].tag` and `fields[i].tag` is in the schema's active tag set     |
| `reflection_blob_intern_self_contained`           | For every emitted blob, every `name`/`type_name` view points into the blob's own arena (address-range check)         |
| `reflection_blob_dense_form_chosen_when_compact`  | A schema with `max_tag < 2 × fields.size()` emits a dense `tag_to_index`                                              |
| `reflection_blob_sparse_form_chosen_when_gappy`   | A schema with retired tags spanning a wide range emits a sparse `tag_to_index`                                        |
| `reflection_blob_byte_offset_matches_struct`      | `offsetof(GeneratedStruct, field) == blob.fields[i].byte_offset` for every field, every emitted struct                 |
| `reflection_blob_byte_size_matches_struct`        | `sizeof(GeneratedStruct::field) == blob.fields[i].byte_size` for every field                                          |
| `reflection_blob_since_monotonic`                 | `fields[i].since` is in `1..=blob.version` for every emitted blob                                                     |
| `reflection_blob_kind_completeness`               | Every `ReflectionKind` enumerator has at least one fixture exercising it across the test schema set                  |

Lookup correctness:

| Test                                              | Asserts                                                                                                               |
|---------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------|
| `reflection_field_for_tag_dense_present`          | `field_for_tag(dense_blob, t)` returns the field whose `tag == t` for every present `t`                                |
| `reflection_field_for_tag_dense_absent`           | `field_for_tag(dense_blob, t)` returns `tools::Error::FieldOutOfRange` when `t` is reserved or out-of-range            |
| `reflection_field_for_tag_sparse_present`         | Same, sparse form                                                                                                      |
| `reflection_field_for_tag_sparse_absent`          | Same, sparse form, asserting `tools::Error::FieldOutOfRange`                                                           |
| `reflection_lookup_blob_present`                  | `lookup_blob(known_fqn)` returns the expected pointer                                                                  |
| `reflection_lookup_blob_unknown_fqn`              | `lookup_blob(unknown_fqn)` returns `tools::Error::FQNNotFound`                                                          |
| `reflection_lookup_blob_null_slot`                | A test fixture with a null `RegistryEntry::reflection` returns `tools::Error::BlobLoadFailed`                          |
| `reflection_resolve_nested_struct`                | A field with `kind == Struct` returns the referenced FQN's blob                                                        |
| `reflection_resolve_nested_enum`                  | A field with `kind == Enum` returns the referenced enum's blob                                                         |
| `reflection_resolve_nested_scalar_refused`        | A field with a scalar `kind` returns `tools::Error::FieldOutOfRange` from `resolve_nested` (precondition check)        |

Stripped-build behaviour:

| Test                                              | Asserts                                                                                                                |
|---------------------------------------------------|------------------------------------------------------------------------------------------------------------------------|
| `reflection_blob_null_in_shipping`                | Built with `GLIBRE_EMIT_REFLECTION=OFF`: every `RegistryEntry::reflection == nullptr` and the editor header errors-out on inclusion |
| `reflection_blob_present_in_editor`               | Built with `GLIBRE_EMIT_REFLECTION=ON`: every `RegistryEntry::reflection != nullptr` and the partitions span is non-empty |
| `reflection_abi_hash_unchanged_across_profiles`   | `glibre_types_abi_hash()` returns the same string in shipping and editor builds (§7.2)                                  |

Concurrency:

| Test                                              | Asserts                                                                                                                |
|---------------------------------------------------|------------------------------------------------------------------------------------------------------------------------|
| `reflection_lookup_thread_safe_disjoint`          | 8 threads each calling `lookup_blob` on a distinct FQN return correct pointers (TSan-clean)                            |
| `reflection_lookup_thread_safe_shared_fqn`        | 8 threads each calling `lookup_blob` on the same FQN return the same pointer (TSan-clean)                              |
| `reflection_field_for_tag_thread_safe`            | 8 threads concurrently calling `field_for_tag` on shared blobs return identical results (TSan-clean)                   |

Determinism (PHILOSOPHY §7):

| Test                                              | Asserts                                                                                                                |
|---------------------------------------------------|------------------------------------------------------------------------------------------------------------------------|
| `reflection_partition_iteration_deterministic`    | `all_partitions()` returns a span whose `context_prefix` values are sorted and stable across builds                    |
| `reflection_blob_byte_equal_across_hosts`         | Codegen-emitted blob `.cpp` files are byte-equal across CI matrix hosts (extends `fory-codegen.md` determinism gate)   |

### 11.2 Integration tests

| Test                                                      | Asserts                                                                                                       |
|-----------------------------------------------------------|---------------------------------------------------------------------------------------------------------------|
| `inspector_renders_form_for_known_component`              | Headless editor selects an entity with a registered component; the inspector enumerates `blob.fields` and emits one row per field. |
| `inspector_renders_placeholder_for_unknown_component`     | Headless editor selects an entity whose component schema was unloaded; inspector logs `FQNNotFound` and renders the "type unloaded" placeholder. |
| `inspector_recurses_through_nested_struct`                | A schema with a nested-struct field expands one level on user click; the recursion produces fields from the nested blob. |
| `inspector_handles_field_out_of_range`                    | An `EditCommand` referencing a retired tag yields `FieldOutOfRange`; `CommandStack` rejects with `CommandConflict`. |
| `asset_browser_groups_by_context`                         | `all_partitions` returns partitions whose `context_prefix` maps to the asset-browser groups; the panel renders one group per partition. |
| `hot_reload_refresh_picks_up_new_schemas`                 | After a Mode-A reload that adds a new schema, the next inspector frame's `lookup_blob` returns the new blob; the partitions span includes the new entry. |
| `hot_reload_refresh_drops_removed_schemas`                | After a Mode-A reload that removes a schema, the next inspector frame's `lookup_blob(removed_fqn)` returns `FQNNotFound`; no torn read. |
| `hot_reload_blob_pointer_stability_within_build`          | Across a Mode-A reload that does not touch a given plugin's schemas, the surviving blob pointers are byte-identical pre- and post-swap. |

### 11.3 Performance gates

No CI benchmark runs for reflection (editor-only, soft budget;
§9.4). The diagnostic `GLIBRE_ALLOC_STRICT=1` build asserts the
4 MiB heap ceiling on the `data` tag during an editor smoke test
(part of the editor profile's CI workflow, not the engine's
shipping perf gate).

### 11.4 Trace instrumentation

The `tools` context's `.glibre-trace` format records inspector
frame times (`specs/tools/SPEC.md` §10). Trace points relevant to
reflection:

- `editor.inspector.frame.begin` / `end`: per-panel-frame envelope.
- `editor.inspector.lookup_blob` (with `fqn`, `cache_hit`).
- `editor.inspector.field_for_tag` (with `tag`, `form` ∈
  `{dense, sparse}`).

Trace points are compiled out in shipping (`GLIBRE_TRACE = 0`) and
on by default in editor profiles. Their cost is accounted under
the `tools` context, not `data` (mirroring envelope-serdes-design
§11.4).

## 12. Open questions

- [OPEN] **#1 — Path-string property API.** Harmonius R-1.3.3
  ("path access in 8 segments under 500 ns") is refused for MVP;
  the editor walks one level at a time via `resolve_nested`. A
  flattened path-string API (`get_field("transform.position.x")`)
  would compose those lookups into a single call. Defer until two
  concrete consumers demand it (PHILOSOPHY's "two concrete users"
  rule). Plausible consumers: an animation-binding plugin, a
  network-replication delta plugin. Owner: post-MVP `editor`
  sub-epic.

- [OPEN] **#2 — Per-field attribute table.** Harmonius R-1.3.6's
  `range`, `display_name`, and serialisation hints are deferred.
  When they enter, the proposed shape is a parallel
  `eastl::span<const ReflectionAttribute>` per blob, keyed by
  `tag`, with a closed-sum `ReflectionAttribute` over MVP-relevant
  kinds (display name, numeric range, hidden flag). The blob's
  current shape is forward-compatible: adding the span field is
  the only ABI break, and it triggers the same SONAME bump §7.2
  documents. Owner: post-MVP `editor` sub-epic, gated on a real
  user need (e.g. a property whose range constraint cannot be
  expressed in the `.fory` schema).

- [OPEN] **#3 — Container structural editing.** MVP renders
  `list<T>` and `map<K,V>` as read-only summaries ("3 items") in
  the inspector — adding / removing elements is post-MVP. The blob
  shape already encodes container kinds (§3.1); the open question
  is whether structural editing surfaces through reflection
  (a `ReflectedField::resize(...)`-style closure) or through
  bespoke editor widgets per container shape. Defer to the first
  story that requires editor-side container mutation.

- [OPEN] **#4 — 4 MiB heap ceiling under realistic catalog
  growth.** §9.3 shows the ceiling holds for ~4500 schemas at
  ~16 fields/avg; MVP catalog projection is ~10³ schemas. If the
  catalog grows past 4500 schemas before MVP ships, an amendment
  to `perf-budget.md` and SPEC §9.2 is required. Sibling task-
  breakdown spike is asked to verify the projection against
  every plugin's `.fory` schema set in CI.

- [OPEN] **#5 — `tools::Error` enum amendment.** §10 proposes
  three new arms (`BlobLoadFailed`, `FQNNotFound`, `FieldOutOfRange`).
  The amendment ships with the first plan PR that introduces
  `tools/glibre-editor/src/reflection.cpp`. Owner: tools sub-epic.
  Verify with `tools/SPEC.md` editor: confirm that
  `InspectorUnknownType` (already declared) is not the same arm as
  `FQNNotFound` — `InspectorUnknownType` covers unknown component
  types in the inspector (a per-row diagnostic), `FQNNotFound`
  covers unknown FQN at `lookup_blob` (a per-blob diagnostic).
  Two distinct concerns, two distinct arms.

- [OPEN] **#6 — Recursion-depth limit enforcement.** §3.5 pins
  maximum nesting depth at 8 via a `.fory` lint rule. The lint
  is currently informal; the codegen does not enforce it. The
  enforcement plan is to add a `Foryc` reachability-depth check
  alongside the cycle check (`fory-codegen.md` §"ABI Stability
  Rules" #3). Owner: the same plan that adds the `Foryc` cycle
  check; sibling spike `[SPIKE] task-breakdown-data-reflection-blob-detailed`
  records the dependency.

- [OPEN] **#7 — Editor-only TU layout in the build graph.** §4.4
  pins the build flag (`GLIBRE_EMIT_REFLECTION`) and the SONAME-
  invariance rule (§7.2). The open question is whether the
  reflection `<Type>.cpp` blocks live in a *separate* tools-only
  archive (e.g. `glibre-types-reflection.a` linked into
  `glibre-editor` only) or in the same `glibre-types.dylib` with
  conditional compilation. Both forms preserve the SONAME
  invariance; the trade-off is rebuild speed (separate archive
  rebuilds independently when only the blob shape changes) vs.
  cohesion (same-dylib keeps the registry table and its blobs in
  one TU). Defer to the implementation plan.
