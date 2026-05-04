# core — Detailed Design: type-registry aggregate

> Detailed design for the `TypeRegistry` / `TypeId` aggregate declared
> in `specs/core/SPEC.md` §4.9. Refines §4.9, §5.1–§5.5, §6.9, §7.1,
> §8.4, §9.3, §10.1 in place; cites
> `reviews/decisions/fory-codegen.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/error-model.md`,
> `reviews/decisions/frame-phases.md`, and
> `reviews/decisions/perf-budget.md`. Sibling designs:
> `specs/core/plugin-loader-design.md`,
> `specs/core/schedule-frame-design.md`. Does not introduce new public
> surface beyond the §5 stub; deviations from the cited records would
> require an amendment spike, not an in-place edit.

Refs: spike #712 — `[SPIKE] design-core-type-registry-detailed`.
Parent sub-epic #699. Sibling task-breakdown spike blocked-by this
deliverable.

## 1. Purpose

`TypeRegistry` is the single component in `glibre-core` that resolves
a stable codegen-emitted `TypeId` to the descriptor every other core
aggregate needs to operate on type-erased bytes:

1. **`World` / archetype storage** consults the registry once per
   archetype-shape compile to bake `(size, align)` into the chunk
   layout (§6.2 of `core/SPEC.md`).
2. **`Schedule`** consults the registry once per access-set
   intersection to validate that every `TypeId` named in a
   `SystemDecl.reads` / `.writes` set is admitted (§6.4).
3. **`PluginLoader`** appends type entries during
   `glibre_plugin_register` and validates `ComponentDecl.schema_hash`
   equality across reloads (§3.5 step 9 of
   `plugin-loader-design.md`, §8.4 step 2.2 of `core/SPEC.md`).
4. **`HotReloadBarrier`** queries the registry's recorded
   `schema_version` per type to decide whether a migration chain must
   run on the surviving storage (§8.5 of `core/SPEC.md`,
   `hot-reload-protocol.md` §"Step 3 — Migrate").

The registry's one responsibility — **mapping `TypeId` to the
codegen-emitted column descriptor and ABI provenance for the lifetime
of a process** — is sharp. If the descriptor schema, the `TypeId`
encoding, the ABI hash composition rule, the append-only mutation
discipline, or the registry's interaction with hot-reload changes,
this design changes. Anything else is out of scope.

What the registry **explicitly refuses to own**:

- **Storage layout** — `World` / `Archetype` / `Chunk` (§4.2 of
  `core/SPEC.md`) own the column bytes, the move-between-archetype
  paths, and the SoA stencils. The registry exposes `(size, align,
  drop_thunk, copy_thunk)` and stops there.
- **Serialization** — `glibre-types.dylib` (the middleman, per
  `fory-codegen.md`) owns the per-type `serialize` /
  `deserialize` / migration table. The registry does not interpret
  Fory bytes.
- **Type ownership** — plugins (per `plugin-abi.md`) own the
  declarations. `core` owns only the lookup table that lets the rest
  of the engine cross the ABI seam by stable integer.
- **Reflective surfaces** — `DynamicValue`, path-based property
  access, `Reflect` / `FromReflect` traits, attribute / metadata
  systems, sub-trait dispatch (R-1.3.3 — R-1.3.11 of harmonius)
  are **refused** in core (PHILOSOPHY §6, §3.3 of `core/SPEC.md`).
  Editor-time introspection is the editor's job; runtime reflection
  is forbidden in shipping.
- **Type registration of non-component bytes** — events, assets, and
  resources that are *not* ECS columns do not belong in the
  component-storage descriptor table. Resource singletons share the
  `TypeId` namespace (§4.10 of `core/SPEC.md` lists `Resource map
  (TypeId → typed singleton)`) but the registry's descriptor row is
  identical (size / align / drop / copy) — no separate "resource
  table" exists.
- **Mid-frame mutation** — registrations land at frame boundaries
  only (load-time pre-frame, or phase 8 hot-reload barrier). The
  registry refuses every mutation outside those windows with
  `core::Error::TypeRegistryClosed` or `FramePhaseMisordered`.

The SRP boundary is sharp by construction: every other registry-shape
data structure (`SystemRegistry`, `PassRegistry`, `PanelRegistry`)
lives in a sibling sub-module (§6.1 of `core/SPEC.md`). Cross-module
includes are forbidden by the build's per-sub-module visibility rules
(§6.1).

## 2. Requirements coverage

This section verifies that every harmonius MVP-scope requirement /
design clause about reflection + the type system is either covered by
the design below or explicitly refused with rationale. Inputs:

- `harmonius/docs/requirements/core-runtime/reflection-and-type-system.md`
  (R-1.3.1 through R-1.3.11).

| Harmonius clause                                                                                                  | Glibre disposition                                                                                                                                                                                                                                                                                                                          |
|-------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-1.3.1** — Runtime type registry, immutable after init, lock-free concurrent reads, ≥10 000 types, O(1) lookup, duplicate-id diagnostic naming both types | **Covered.** `TypeRegistry` is immutable-after-init within a session (§4.9 of `core/SPEC.md` invariant 1); reads are lock-free by construction (§3.4 below); `TypeId` is a `u64` so the address space exceeds 10 000 trivially; descriptor lookup is O(1) via a flat array indexed by `TypeId.value` (§3.5); duplicate-id refusal is a *codegen-time* assertion in `glibre-foryc` (it never reaches runtime), with the build error naming both fqns (§3.6). The "lock-free concurrent reads" requirement is satisfied because the registry is **append-only** within a session and a relaxed acquire-load on the size publishes additions (§6.2). |
| **R-1.3.2** — Type descriptors carry size, alignment, drop, clone, default-ctor, field layout, variant layout                   | **Partial — covered for size/align/drop/copy; refused for default-ctor / field layout / variant layout.** The descriptor (§3.3) carries `{ size, align, drop_thunk, copy_thunk }` plus `(fqn, schema_version, schema_source_blake3)` for ABI provenance. Default-construct, field-by-name access, and variant-tag inspection are reflective surfaces refused by §3.3 of `core/SPEC.md` (PHILOSOPHY §6). Construction defaults are codegen-emitted directly into POD types by `glibre-foryc` per `fory-codegen.md` §"Schema File Format"; field layout is recorded in the `.fory` schema, not in a runtime descriptor. |
| **R-1.3.3 / R-1.3.4** — Property system: path-based read/write, dynamic-length collections, <500 ns path access                 | **Refused.** PHILOSOPHY §6 forbids runtime reflection in shipping; PHILOSOPHY §10 (Occam's razor) collapses path-based access into the editor's edit-time inspector (which reads the `.fory` schema directly, not a runtime registry). The `editor` and `data` contexts (§3.3 of `core/SPEC.md`) are the substitutes for harmonius's "property system at runtime"; no runtime API in `core`. |
| **R-1.3.5** — DynamicValue holding any reflected value, diff/patch                                                              | **Refused.** Same rationale as R-1.3.3 / R-1.3.4. The DynamicValue interchange use case is replaced at the engine level by Fory-typed POD round-trips (`fory-codegen.md` §"Migration Mechanic"); no `DynamicValue` type exists in `core`. |
| **R-1.3.6** — User-defined key-value attributes on types and fields                                                             | **Refused.** Attributes that affect serialization belong on the `.fory` schema (`fory-codegen.md` §"Schema File Format" — `default { ... }`, `since`, `tag` are the supported attribute set). Attributes that affect editor display belong on editor-side code (`tools` SPEC). No runtime attribute table in `core`. |
| **R-1.3.7** — Trait registration against type IDs for runtime resolution                                                        | **Refused.** Core resolves type-erased function pointers via the codegen-emitted `drop_thunk` / `copy_thunk` (§3.3) — the only "trait dispatch" core needs. Generic trait registries (Serialize / Hash / Compare per type) belong to the originating context (e.g., the data context's serializer is the `glibre-types.dylib` static dispatch table). |
| **R-1.3.8 / R-1.3.9 / R-1.3.10 / R-1.3.11** — Reflect / FromReflect traits, derive macro, sub-traits, attribute set             | **Refused, replaced by codegen.** Reflect-trait dispatch is the runtime-reflection model PHILOSOPHY §6 explicitly forbids in shipping. The Rust prior art's mechanism (procedural-macro derive emitting trait impls) collapses to `glibre-foryc` codegen emitting POD types and a static descriptor table at build time. Editor-time introspection is replaced by editor reading the `.fory` schema files directly, plus the `glibre-types.dylib`-exposed registry for type-id stability. |

Net result: R-1.3.1 + the size/align/drop/clone subset of R-1.3.2 are
the only harmonius requirements `core`'s `TypeRegistry` accepts; the
rest are refused under PHILOSOPHY §6 and routed to either editor-time
tooling (via the `.fory` schemas + `tools`) or to per-context
serialization (via `glibre-types.dylib`). The collapse is significant
and intentional: harmonius's reflective property system is replaced
end-to-end by static codegen.

Glibre-native requirements added beyond harmonius:

- **`TypeId` is stable across plugins built against the same
  `glibre_types_abi_hash`** — every plugin obtains the identical
  `TypeId.value` for a given fqn at build time (§3.2 below). Plugins
  cannot manufacture a `TypeId` at runtime; they cite codegen-emitted
  constants from `glibre-types.dylib`.
- **Schema-hash drift on an existing fqn refuses the load** — a
  plugin rebuilt with a layout change (without a version bump) cannot
  silently re-register the same `TypeId` under a different layout.
  Detected at `PluginLoader::load` step 9 and at `HotReloadBarrier`
  step 2.2 (§3.7 below).
- **Append-only within a session** — a `TypeId` admitted at frame N
  is admitted at every later frame; no remove path. This collapses
  the `World` archetype invariant (§4.2 #3 of `core/SPEC.md`) and the
  hot-reload survival rule (§8.1 of `core/SPEC.md`) into one
  primitive.

## 3. Detailed model

### 3.1 Aggregate composition

```text
TypeRegistry (root, owned by core)
├── eastl::vector<Descriptor>            descriptors_   (indexed by TypeId.value)
├── eastl::hash_map<eastl::string,
│                   TypeId>              by_fqn_        (fqn → TypeId)
├── std::atomic<std::uint64_t>           size_          (published count; relaxed on read)
├── eastl::vector<PluginId>              owner_         (descriptors_[i].owner)
├── eastl::array<TypeId, kBuiltinCount>  builtins_      (core-owned IDs, static-init)
└── eastl::string_view                   abi_hash_view_ (borrowed from glibre-types.dylib)
```

Container choice rationale (per PHILOSOPHY §11):

- `eastl::vector<Descriptor>` — runtime data structure; `Descriptor` is
  trivially copyable POD (§3.3); allocation tagged `ContextTag::core`
  (`perf-budget.md` §"Allocator Rules" #1).
- `eastl::hash_map<eastl::string, TypeId>` — `by_fqn_` exists for the
  `O(1)` cold-path lookup `find(fqn) → TypeId` used by
  `PluginLoader::load` step 9 to detect collisions and by editor
  tooling. Hot-path code never touches this map.
- `std::atomic<std::uint64_t>` — the one piece of registry state
  visible to multi-thread readers; ordered acquire / release pair
  (§3.4). Relaxed loads on the read side are safe because callers
  observe only entries with index `< size_`.
- `eastl::array<TypeId, kBuiltinCount>` — core's own built-in types
  (entity bookkeeping, change-tick value, the `LocalTransform` /
  `GlobalTransform` / `PreviousGlobalTransform` triplet that core's
  phase-5 system reads) get `TypeId` slots reserved at static-init
  time so core code can cite them as `constexpr`. The integer values
  themselves are codegen-emitted from `glibre-types.dylib`'s
  `_registry.cpp`.
- `eastl::string_view` — borrowed lifetime; the underlying string is
  the static-storage `glibre_types_abi_hash()` literal in
  `glibre-types.dylib`. Captured once at registry construction.

The registry instance is owned by the `World`'s startup composer
(§4.10 of `core/SPEC.md`); one registry per `World`, but in MVP one
`World` per process so the registry is effectively process-wide. The
plural-`World` future is not in scope.

### 3.2 `TypeId` encoding

```cpp
// specs/core/SPEC.md §5.2 (locked).
struct TypeId {
    std::uint64_t value{};
    friend constexpr bool operator==(TypeId, TypeId) noexcept = default;
};
```

Encoding rule:

1. **`TypeId.value` is a dense ascending index into
   `descriptors_`.** Slot 0 is reserved as `TypeId::invalid()` (a
   sentinel returned from refusal cases). Slots 1..N are admitted in
   `glibre-foryc`-emitted canonical order — every plugin built
   against the same `glibre_types_abi_hash` sees the **identical**
   integer for a given fqn, by construction.
2. **Canonical order is ascending Unicode code-point order over fqn**
   (the same canonical order that drives `glibre_types_abi_hash`'s
   blake3 input — `fory-codegen.md` §"middleman dylib exposes" #1,
   `plugin-abi.md` §"ABI Hash Function" #1). This makes the
   `TypeId.value` for fqn `glibre.core.Transform` byte-for-byte the
   same in every plugin that links the same middleman.
3. **`TypeId` is stable for the lifetime of a process.** Append-only
   within a session: a plugin that registers a new fqn appends a new
   slot at index `size_++`; no slot is reused or compacted (§4.9
   invariant 1 of `core/SPEC.md`).
4. **`TypeId.value` is **not** stable across processes whose
   middleman binaries differ.** The ABI hash check at load
   (`plugin-loader-design.md` §3.5 step 5) refuses any plugin built
   against a different middleman, so within a single running engine
   instance the integer is universally consistent.
5. **`TypeId::invalid()` is `TypeId{0}`.** Public API surfaces that
   take a `TypeId` reject `TypeId::invalid()` as
   `core::Error::TypeUnregistered` — even if slot 0 is "reserved",
   it is never "registered". This is what makes the dense index +
   sentinel encoding safe.

The reserved bit budget is generous (`u64`); the dense index uses
the bottom ~24 bits even at scale (see §9 below). The remaining bits
are reserved for future use — `[OPEN]` whether a future
plurality-`World` story uses some of those bits as a world tag.

### 3.3 Per-type `Descriptor`

```cpp
// core/src/type-registry/descriptor.hpp — internal.
namespace glibre::core::detail {

using DropThunk = void (*)(void* dst) noexcept;
using CopyThunk = void (*)(void* dst, const void* src) noexcept;

struct Descriptor {
    eastl::string_view  fqn;                  // borrowed; static lifetime in middleman.
    std::uint32_t       size;                 // sizeof(T), bytes; layout-stable per fory-codegen.md §ABI.
    std::uint16_t       align;                // alignof(T); always a power of two.
    std::uint16_t       schema_version;       // matches .fory `version` integer.
    std::array<std::byte, 32>  schema_source_blake3;  // raw 32-byte digest of .fory source.
    DropThunk           drop;                 // codegen-emitted; safe to call on a moved-from slot.
    CopyThunk           copy;                 // codegen-emitted; bitwise for trivially-copyable types.
    std::uint8_t        storage_hint;         // archetype | sparse | singleton — see plugin-abi.md ComponentDecl.
    std::uint8_t        flags;                // bit 0 = is_resource_singleton; bits 1..7 reserved.
    PluginId            owner;                // PluginId{0} for core-owned (built-in) types.
    SchemaFqn           type_fqn;             // alias of fqn for callers that hold ownership.
};

static_assert(std::is_trivially_copyable_v<Descriptor>);

}  // namespace glibre::core::detail
```

Field semantics:

- **`fqn`** — fully-qualified name, e.g. `glibre.core.Transform` or
  `glibre.render.Mesh`. Borrowed from `glibre-types.dylib`'s static
  storage; lifetime equals process lifetime. Never reallocated.
- **`size` / `align`** — codegen-emitted at the time the matching
  `.fory` schema was compiled into `glibre-types.dylib`. Layout
  stability is enforced at codegen time (`fory-codegen.md` §"ABI
  Stability Rules" #1, #2). `size` fits in `u32` because schema
  layout caps individual struct size at 4 GiB, vastly larger than any
  realistic component.
- **`schema_version`** — the `.fory` schema's declared `version`
  integer. Bumped per the additive-only rule
  (`fory-codegen.md` §"Schema File Format"). Used by
  `HotReloadBarrier` step 3 to determine whether a migration chain
  must run for surviving storage of this type.
- **`schema_source_blake3`** — raw 32-byte blake3 digest of the
  `.fory` source bytes that produced this descriptor. Used at hot-
  reload step 2.2 to detect schema-hash drift on an existing fqn
  (§3.7). Stored as raw bytes (not hex) to save 32 bytes per row;
  the registry never logs this field, so the human-readable form is
  derived only when a refusal fires.
- **`drop`** — codegen-emitted destructor thunk. For
  trivially-destructible types (which the additive-only ABI rule
  encourages), this is a no-op function pointer; callers may still
  invoke it unconditionally. Required so the world's archetype-move
  paths (§6.2 of `core/SPEC.md`) can run a uniform "drop column"
  step.
- **`copy`** — codegen-emitted copy thunk. For trivially-copyable
  types it expands to `memcpy(dst, src, size)`; for types holding
  Fory-managed handles it dispatches to the type's emitted copy
  routine. The thunk lives in `glibre-types.dylib` to keep
  `glibre-core` independent of any plugin's code.
- **`storage_hint`** — `archetype | sparse | singleton` (per
  `plugin-abi.md` §"Plugin Manifest Schema" `ComponentDecl
  storage_hint : u8`). Drives the world's choice of storage backend
  per-type. `singleton` types live in the world's resource map
  (§4.1 of `core/SPEC.md`), not in archetype columns.
- **`flags.is_resource_singleton`** — set iff the type is registered
  as a singleton resource rather than a component column. The world
  checks this bit at `set_component` / `get_component` to refuse
  cross-cell access.
- **`owner`** — the `PluginId` of the registering plugin. Used by
  the loader's compensating-unregister discipline
  (`plugin-loader-design.md` §6.2): on plugin refusal, all
  descriptors with `owner == failing_plugin_id` are appended to the
  rollback ledger, but **not** physically removed (§3.4 below
  documents the soft-tombstone discipline that preserves the
  append-only contract while still rejecting future lookups for
  refused entries).
- **`type_fqn`** — a re-export of `fqn` under a strongly-typed alias
  (`SchemaFqn`); see §3.5 lookup paths.

Descriptor size: 80 bytes (8 + 4 + 2 + 2 + 32 + 8 + 8 + 1 + 1 + 8 +
8, padded to 16-byte alignment — 80 fits in five 16-byte cache lines'
worth of dense-vector storage, though the actual layout is
implementation-driven; the build asserts a stable size at static
init for the heap accounting in §9).

### 3.4 Per-plugin registration table

The registry maintains an auxiliary side-table keyed by `PluginId` so
that the loader's compensating-unregister can revert all descriptors
contributed by a refused plugin without a full scan of `descriptors_`:

```cpp
namespace glibre::core::detail {

struct OwnerEntry {
    eastl::vector<TypeId>   owned_types;   // appended at register; cleared at unregister.
    std::uint64_t           appended_at;   // size_ snapshot before this plugin's batch.
};

eastl::hash_map<PluginId, OwnerEntry>  by_owner_;

}  // namespace glibre::core::detail
```

Discipline (the "soft-tombstone" rule):

1. **Register-batch open.** When the loader calls
   `TypeRegistry::begin_batch(PluginId p)`, the registry snapshots
   `size_` into `by_owner_[p].appended_at`. The batch is bracketed
   by exactly one `commit_batch` or `rollback_batch` call.
2. **Register.** During the batch, each successful
   `register_type(p, decl)` appends to `descriptors_`, increments
   `size_` (relaxed publish — §3.4 below), and pushes the new `TypeId`
   into `by_owner_[p].owned_types`.
3. **Commit-batch.** At commit, `size_` is published with
   `memory_order_release`; readers seeing the new value are
   guaranteed to see the descriptors at indices `[appended_at, size_)`
   via the dependent-load discipline (descriptors_'s storage pointer
   is captured before the size load).
4. **Rollback-batch.** On loader refusal, the registry walks
   `by_owner_[p].owned_types` in reverse, and for each `TypeId`:
   - if `id.value == size_ - 1`, performs a true pop (decrement
     `size_` with release, drop the descriptor row);
   - else, marks the descriptor as **tombstoned** (`flags.bit_7 = 1`,
     `fqn = ""`, descriptors thereafter return
     `core::Error::TypeUnregistered` for the slot) and removes the
     fqn from `by_fqn_`.
5. **Append-only externally.** From outside `core/src/type-registry/`,
   the registry is observably append-only: no public API exposes
   `unregister`, no `TypeId` admitted across a successful commit
   ever returns to "unregistered" state. Tombstones are visible only
   inside the rollback path; once a plugin's batch commits, its
   tombstoning is impossible.

The tombstone discipline preserves the append-only invariant
(§4.9 #1 of `core/SPEC.md`) **as observed by the rest of the engine**
while still letting the loader walk back a refused batch byte-for-byte.
Without tombstones, two concurrent batches (which MVP forbids — see
§3.4) could interleave their `TypeId.value`s and a rollback could not
restore dense ordering.

For MVP's single-loader-thread model, batches are strictly serial:
`begin_batch` → registrations → `commit_batch` or `rollback_batch`,
all on the loader thread, all between phases. The serialization
makes the "rollback always pops" path the common case; tombstones are
the safety net for the multi-threaded loader future.

### 3.5 Lookup paths

The registry exposes three distinct lookup paths, each with a
documented cost class:

| Path                                | Caller                          | Cost          | Frequency               |
|-------------------------------------|---------------------------------|---------------|-------------------------|
| `descriptor(TypeId)`                | World, Schedule (compile-time)  | O(1) array    | Cold-path: load + barrier |
| `find_by_fqn(fqn) → Result<TypeId>` | PluginLoader, editor inspector  | O(1) hash-map | Cold-path: load only      |
| `iterate_owned(PluginId, fn)`       | PluginLoader rollback           | O(M) per-plugin | Cold-path: refusal      |

**`descriptor(TypeId t)`** — the foundational path. Validated against
`size_` (acquire load) and the slot's tombstone bit; returns
`Result<const Descriptor&>` with `core::Error::TypeUnregistered` on
miss. Compile-time consumers (the codegen-emitted thunk in `world/`
that resolves component columns by `TypeId`) call this **once at
schedule-compile** and bake the resolved pointer into the
`SystemThunk`; per-frame execution never touches the registry.

**`find_by_fqn(fqn)`** — used during plugin admission to detect
collisions (two plugins claiming the same fqn) and by the editor's
type browser. Implemented as `eastl::hash_map<eastl::string, TypeId>`;
amortized O(1) with a cap on rehashing tied to the 64 MiB `core` heap
ceiling. Never on the hot path.

**`iterate_owned(PluginId, fn)`** — the rollback ledger. Used only by
`PluginLoader::rollback_batch` (§3.4); not exposed to plugin code.
Per-plugin O(M) where M is the number of types the plugin registered.

### 3.6 Codegen-emitted descriptors

Every descriptor row is **emitted by `glibre-foryc`**, not authored
by hand. The pipeline:

1. Each `data/schemas/<ctx>/<Type>.fory` (per `fory-codegen.md`
   §"Schema File Format") declares one persistent type.
2. `glibre-foryc` emits, alongside the POD struct,
   `glibre/types/<ctx>/<Type>.descriptor.hpp` defining a `constexpr`
   descriptor literal with `(fqn, size, align, schema_version,
   schema_source_blake3, drop_thunk, copy_thunk, storage_hint,
   flags)`.
3. `glibre-types.dylib`'s codegen-emitted `_registry.cpp`
   concatenates every type's descriptor into a sorted-by-fqn array
   and exposes it via:
   ```cpp
   extern "C" const std::byte* glibre_types_descriptor_table()      noexcept;
   extern "C" std::size_t      glibre_types_descriptor_table_size() noexcept;
   ```
4. At `World::create()`, core's `TypeRegistry::create()` reads the
   middleman's descriptor table once via the C entry points,
   deserializes each descriptor (the on-wire form is a Fory-typed
   POD — same encoding rules as any other middleman type per
   `fory-codegen.md` §"Schema File Format") and appends to
   `descriptors_` in canonical order. Every plugin then registers its
   own additions inside `glibre_plugin_register` via the §3.7 surface.

Codegen-time guarantees (asserted by `glibre-foryc`, never reach
runtime):

- **No duplicate fqns.** A second `.fory` declaring `glibre.core.Transform`
  fails the build — the build error names both source files.
  This satisfies the R-1.3.1 "duplicate type IDs SHALL produce a
  diagnostic error naming both conflicting types" requirement at
  build time, not at runtime; the runtime can never observe a
  duplicate (`fory-codegen.md` §"ABI Stability Rules" implicit:
  fqns are unique by file system).
- **Descriptor layout stability.** `glibre-foryc` asserts
  `sizeof(Descriptor)` matches the recorded size at every codegen
  run; a layout drift (compiler upgrade adding padding) fails the
  build until the recorded size is updated.
- **Canonical ordering.** The descriptor table is emitted in
  ascending Unicode code-point order over fqn — the same order that
  `glibre_types_abi_hash` uses. This guarantees `TypeId.value`
  byte-equivalence across builds against the same hash.

Build-time error format (as authored under the `task-breakdown-data-plugin`
spike, `fory-codegen.md` §"Open Questions" #2):

```
glibre-foryc: error: duplicate fqn 'glibre.core.Transform'
  first defined: data/schemas/core/Transform.fory:1
  redefined:    plugins/render/data/schemas/core/Transform.fory:1
```

### 3.7 Plugin registration entrypoint

The registry's contribution to `PluginContext` (§3.4 of
`plugin-loader-design.md`) is the `TypeRegistry&` field. During
`glibre_plugin_register`, the plugin populates the registry by
calling, for each component declared in its `manifest.components`:

```cpp
// core/include/glibre/core/type_registry.hpp — public.
[[nodiscard]] Result<TypeId>
TypeRegistry::register_component(
    PluginId             owner,
    const ComponentDecl& decl       // from PluginManifest
) noexcept;
```

Per-call discipline:

1. **Manifest cross-check.** `decl.fqn` must appear in
   `manifest.components`. Surplus registration (a plugin registering
   a fqn absent from its manifest) is refused with
   `core::Error::PluginInitFailed` carrying detail
   `"surplus-registration: <fqn>"` (per `plugin-loader-design.md`
   §3.2 surplus-registration rule). Detected by the loader, not the
   registry — the registry receives only the trusted
   `ComponentDecl`.
2. **Existing-fqn check (`find_by_fqn`).** If `decl.fqn` is already
   registered:
   - **Same `schema_source_blake3`** → idempotent re-registration.
     Returns the existing `TypeId`. This admits the hot-reload case
     where Q registers the same types as P with the same schema.
   - **Different `schema_source_blake3`** → schema-hash drift.
     Refused with `core::Error::PluginAbiHashMismatch` (§4.9 #2 of
     `core/SPEC.md`, §8.4 step 2.2 of `core/SPEC.md`). The loader
     wraps this under `core::Error::HotReload` for hot-reload
     contexts (§8.7 of `core/SPEC.md`).
   - **Different `schema_version`** → version regression. If
     `decl.schema_version < existing.schema_version`, refused with
     a new `core::Error::SchemaVersionRegression` arm (§3.8 below;
     see §10.1 row addition). Newer-version is **never** silently
     re-registered; the hot-reload barrier is the only path that
     advances `schema_version`, and it does so by appending a new
     descriptor row alongside the migration table — *not* by
     mutating the existing slot. (See §6 below for the discipline.)
3. **New-fqn admit.** Append to `descriptors_` at the next slot,
   record into `by_owner_[owner].owned_types`, increment `size_` with
   `memory_order_release`, return the new `TypeId`. The append is
   fenced by `commit_batch`'s release; concurrent reads (§3.4) see
   the new entry only after the batch commits.
4. **Closed-after-init.** Outside an open batch, every register call
   refuses with `core::Error::TypeRegistryClosed` (§4.9 #1 of
   `core/SPEC.md`). The barrier opens batches at phase 8; the
   loader opens batches at startup. Plugins themselves never open
   batches — they only register inside the loader's batch. The
   batch-open / commit / rollback APIs are loader-internal (§4.2
   below).

### 3.8 The `SchemaVersionRegression` arm (new)

`core::Error::SchemaVersionRegression` is a *new* arm added by this
design to the §5.1 enumeration in `core/SPEC.md`. Trigger:

- `register_component` finds the fqn already registered with a
  newer `schema_version` than the candidate.

This case is forbidden because allowing a newer plugin to be replaced
by an older one across a hot-reload would invalidate the surviving
storage's header (which records the newer version's bytes). Refusing
the regression at registration time keeps the registry consistent
with the storage.

`SchemaVersionRegression` joins the `Plugin*` family at load time and
the `HotReload`-wrapped family at barrier time per the umbrella rule
(§8.7 of `core/SPEC.md`). The §10.1 addition is in §10 below.

## 4. Public surface

The §5.4 forward-decl in `specs/core/SPEC.md` (`class TypeRegistry`)
is authoritative. The §5.5 `World::type_registry()` accessor is
authoritative. This section restates the registry's narrow surface
plus the loader-internal seam.

### 4.1 Public types (locked from `core/SPEC.md` §5)

```cpp
// specs/core/SPEC.md §5.2 — TypeId is locked.
struct TypeId {
    std::uint64_t value{};
    friend constexpr bool operator==(TypeId, TypeId) noexcept = default;
};
```

`TypeId::invalid()` is a public constexpr helper:

```cpp
namespace glibre::core {
constexpr TypeId TypeId_invalid{ .value = 0 };
}
```

### 4.2 `TypeRegistry` operations

```cpp
// core/include/glibre/core/type_registry.hpp — public.
namespace glibre::core {

class TypeRegistry {
public:
    // Lookup (every-thread, lock-free; see §6.2 for ordering).
    [[nodiscard]] Result<const detail::Descriptor*>
    descriptor(TypeId id) const noexcept;

    [[nodiscard]] Result<TypeId>
    find_by_fqn(eastl::string_view fqn) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept;   // acquire load on size_

    // Registration (loader-only — invariant enforced by the API
    // accepting only PluginId values minted by the loader).
    [[nodiscard]] Result<TypeId>
    register_component(PluginId owner, const types::ComponentDecl& decl) noexcept;

protected:
    TypeRegistry() noexcept;
    ~TypeRegistry();
};

}  // namespace glibre::core
```

Per-method contract:

- **`descriptor(TypeId id)`** — lock-free O(1) array access; bounds
  check against `size_` (acquire); tombstone check; on miss,
  `core::Error::TypeUnregistered`. Safe from any thread between
  frame phases. **Hot-path readers (codegen-emitted system thunks)
  do not call this every frame**; they call it once at schedule
  compile and cache the descriptor pointer.
- **`find_by_fqn(fqn)`** — O(1) average via `eastl::hash_map`. Used
  by the loader and by editor tooling. Refuses with
  `core::Error::TypeUnregistered` on miss.
- **`size()`** — published count of registered types. Acquire-load
  on `size_`; pairs with `register_component`'s release-publish.
  Editor surfaces use this for the type browser; never on hot path.
- **`register_component(owner, decl)`** — see §3.7; refuses with
  `TypeRegistryClosed` outside an open batch,
  `PluginAbiHashMismatch` on schema-hash drift,
  `SchemaVersionRegression` on version regression, or
  `OutOfBudget` on heap exhaustion (`perf-budget.md` §"Allocator
  Rules" #2).

The `TypeRegistry`'s constructor and destructor are protected so
callers obtain references only via `World::type_registry()` (§5.5 of
`core/SPEC.md`); no free-standing `create` factory. The world owns
the lifetime.

### 4.3 Loader-internal seam

The batch operations are **not** in the §5 stub; they live in a
private header consumed only by `core/src/plugin/`:

```cpp
// core/src/type-registry/batch.hpp — internal; visibility limited to
// core/src/plugin/ via CMake target_include_directories(SYSTEM PRIVATE).
namespace glibre::core::detail {

[[nodiscard]] Result<void>
TypeRegistry_begin_batch(TypeRegistry& self, PluginId p) noexcept;

[[nodiscard]] Result<void>
TypeRegistry_commit_batch(TypeRegistry& self, PluginId p) noexcept;

[[nodiscard]] Result<void>
TypeRegistry_rollback_batch(TypeRegistry& self, PluginId p) noexcept;

}  // namespace glibre::core::detail
```

Why a free-function seam, not a method: PHILOSOPHY §1 — exposing
batch operations as public methods would invite callers other than
the loader to call them, eroding the SRP boundary that says **only**
the loader admits types. The free-function seam under `detail::`
plus CMake visibility rules makes the boundary mechanical, not
documentary.

### 4.4 No reflective surface

Per §3.3 of `core/SPEC.md` and §2 above, the public surface
**does not include**:

- `Reflect` / `FromReflect` traits (R-1.3.8 / R-1.3.11).
- `DynamicValue` (R-1.3.5).
- Path-based property access (R-1.3.3).
- Attribute / metadata queries (R-1.3.6).
- Trait-by-type-id registration (R-1.3.7).
- Sub-trait dispatch (R-1.3.10).

These surfaces, if they appear at all, live in the `editor` SPEC
(edit-time) or the `data` SPEC (`glibre-types.dylib` static dispatch).
The `core` registry stops at `(size, align, drop_thunk, copy_thunk)`
and never grows reflective fields.

## 5. Hot / cold path split

The registry's interaction with every other engine subsystem is
**cold-path** by design:

| Path  | Caller                                | Trigger                          | Frequency             | Budget                                |
|-------|---------------------------------------|----------------------------------|-----------------------|---------------------------------------|
| Cold  | `glibre-foryc` codegen emits literals | Build time                       | Once per build        | n/a (build cost)                      |
| Cold  | `TypeRegistry::create`                | `World::create` startup          | Once per process      | <1 ms (load 100s of descriptors)      |
| Cold  | `register_component`                  | `glibre_plugin_register`         | Once per type per session | <0.05 ms per call (`plugin-loader-design.md` §9.2) |
| Cold  | `find_by_fqn`                         | `PluginLoader::load` collision   | Per-plugin per load   | <0.05 ms                              |
| Cold  | `descriptor(TypeId)`                  | `Schedule::compile` thunk emit   | Per-system per compile | <1 µs per call                        |
| Cold  | `descriptor(TypeId)`                  | Hot-reload barrier step 2.2 / 3  | Per-type per reload   | <1 µs per call                        |
| **Cold** | `iterate_owned`                    | Loader rollback                  | Per-refusal           | O(M) per plugin                       |
| Hot   | (none — the registry is not on the every-frame path) | n/a            | n/a                   | n/a                                   |

The fast-path in the engine **does not consult the registry every
frame**. The codegen-emitted system thunk (`core/SPEC.md` §6.2 + §6.4)
captures the descriptor pointer at schedule-compile and bakes the
component-column resolution into the thunk body. Per-frame execution
walks the resolved pointers directly; the registry's pointer-stable
storage (`eastl::vector<Descriptor>`) is the seam that makes this
safe.

Pointer stability rules:

1. **`descriptors_` is never relocated after the first commit.** The
   registry reserves capacity for the codegen-emitted descriptor
   count plus a generous margin (10×, capped at 64 KiB of
   descriptors, matching the R-1.3.1 ≥10 000 type budget) at
   `TypeRegistry::create`. Subsequent appends (plugin
   registrations) consume from the reserve without reallocation.
   Exhausting the reserve refuses with `core::Error::OutOfBudget`.
2. **Descriptor pointers handed out by `descriptor(TypeId)` remain
   valid for the registry's lifetime.** Cached pointers in
   compiled system thunks survive across frame boundaries, plugin
   loads, and hot-reload swaps — provided the descriptor itself is
   not tombstoned.
3. **Tombstoned descriptors retain pointer validity** but every
   subsequent `descriptor()` call returns
   `core::Error::TypeUnregistered`. Cached thunks that resolved
   against a tombstoned slot would have been compensating-unregistered
   by the same loader rollback; consumers that bypass the loader's
   discipline are out of contract.

The split makes the registry's per-frame budget cell (§9 below)
**zero**: the registry contributes nothing to the every-frame
critical path.

## 6. Concurrency

The registry follows the same single-thread-per-frame discipline that
governs the rest of `core` (§6.10 of `core/SPEC.md`):

### 6.1 Threading rules

1. **Mutations are loader-thread only.**
   `register_component`, `begin_batch`, `commit_batch`,
   `rollback_batch` run on the game-loop driver thread, exclusively
   inside `Phase::HotReload` (phase 8) or pre-frame at startup.
   Calls from any other thread refuse with
   `core::Error::FramePhaseMisordered` (debug-build assertion;
   shipping builds undefined-behavior because the contract is "loader
   thread only" — the loader's API surface is the only path that
   reaches the registry's mutators).
2. **Reads are lock-free, every thread.** `descriptor()`,
   `find_by_fqn()`, `size()` are safe from any thread at any time.
   Single-writer multi-reader: readers loading `size_` with
   `memory_order_acquire` see at most the last committed batch's
   data; writers publish with `memory_order_release` at
   `commit_batch`.
3. **Batch boundaries are the only sync points.** Within a batch,
   readers that observe `size_` for some intermediate value see only
   *fully-constructed* descriptor entries: appends update the entry
   bytes before the size publish (release-acquire fence), so a
   reader cannot tear-read a half-initialized descriptor.

### 6.2 Memory ordering

```cpp
// Write side (loader thread, inside batch):
descriptors_[next_idx] = built_descriptor;     // 1. Plain store.
size_.store(next_idx + 1, std::memory_order_release);  // 2. Publish.

// Read side (any thread):
const auto sz = size_.load(std::memory_order_acquire);  // 3. Sync.
if (id.value >= sz) return TypeUnregistered;
return &descriptors_[id.value];                 // 4. Plain load — acquire makes the prior store visible.
```

This is the standard publish/subscribe pattern. It is correct because:

- The writer initializes the descriptor row before bumping `size_`,
  and the release-acquire fence pair makes the row's bytes visible
  to every reader that observes the new size.
- The reader loads `size_` with acquire; any subsequent load against
  `descriptors_[i]` for `i < sz` is fenced behind the size load.
- `descriptors_` itself is never relocated (§5 pointer-stability
  rule 1), so the storage pointer captured by readers remains valid
  across writes.

Tombstoning (§3.4 step 4) is a write to the descriptor's `flags`
field that occurs **only on the loader thread, only inside an open
batch**, and is followed by the same release publish at
`rollback_batch`. Readers that resolved a `TypeId` before the
tombstone observe the original `flags`; readers that resolve after
see the tombstone bit and refuse — both behaviours are correct under
the append-only-from-outside contract.

### 6.3 Interaction with the hot-reload barrier

The barrier owns the registry's mutation window during phase 8:

1. **Phase-8 entry.** Barrier calls `TypeRegistry_begin_batch(p)`
   for each pending plugin reload before invoking the loader's
   §3.5 admission sequence on the candidate. The batch is the
   transactional boundary for the candidate's type registrations.
2. **Step 2.2 (schema-coverage check).** Barrier calls
   `descriptor(existing_type_id)` to read the surviving slot's
   `schema_source_blake3` and `schema_version`, compares against
   the candidate's `ComponentDecl`. Drift refuses (§3.7).
3. **Step 2.4 (type-registry append).** Loader's
   `register_component` calls extend `descriptors_`. The batch is
   *not* committed yet — the new entries are visible to readers
   only after step 4 (resume) commits.
4. **Step 4.2 (commit on resume success).** Barrier calls
   `TypeRegistry_commit_batch(p)` after `glibre_plugin_register`
   returns ok and the schedule rebuild succeeds. Readers between
   step 4.2 and step 5 (event publish) see the new entries.
5. **Refusal path (any step).** Barrier calls
   `TypeRegistry_rollback_batch(p)` and the registry walks
   `by_owner_[p].owned_types` per §3.4 step 4. Readers never see
   the rolled-back entries.

The barrier-registry seam is intentionally narrow: three free
functions (`begin_batch`, `commit_batch`, `rollback_batch`) plus
`register_component`. The registry exposes nothing else to the
barrier; the barrier exposes nothing else of itself to the registry.

## 7. Persistence + ABI

### 7.1 Descriptors are Fory-serialized in `glibre-types.dylib`

Per `fory-codegen.md` §"middleman dylib exposes" #3, the middleman
exposes a "schema registry" with per-type
`(fqn, version, blake3-hash-of-schema-source)` entries plus the
descriptor table generated from the same schemas. The registry's
on-disk form is the same Fory-encoded blob the middleman generates;
core reads it once at startup (`TypeRegistry::create`) and never
writes it back. The registry is **process-only state**: it persists
within a process, not across process boundaries.

What does persist across process boundaries is the **`.fory` source
files** (`data/schemas/<ctx>/<Type>.fory`) and the recomputed
`glibre_types_abi_hash`. Those plus the middleman binary are the
sole sources of truth; the registry is a runtime projection.

### 7.2 ABI hash composition

The `glibre_types_abi_hash` recipe (locked from `fory-codegen.md`
§"middleman dylib exposes" #3, `plugin-abi.md` §"ABI Hash Function"):

1. For every `data/schemas/<ctx>/*.fory` file (including
   `core/PluginManifest.fory` and core's own component schemas):
   - compute `schema_source_blake3 = blake3(raw .fory bytes)`.
2. Build the hash input as the byte concatenation of
   `(fqn || ":" || version_le || ":" || schema_source_blake3)` for
   every schema, ordered by ascending Unicode code-point on `fqn`,
   newline-separated, no trailing newline.
3. `glibre_types_abi_hash = blake3(hash_input)`, hex-encoded
   (lowercase, 64 chars), stored as a string literal in
   `glibre/types/abi_hash.hpp` and exported as
   `glibre_types_abi_hash() -> const char*`.

Inputs **not** in the hash:

- header file timestamps,
- compiler identity / version,
- optimization flags,
- `__DATE__` / `__TIME__` macros,
- pragma packing or any other layout directive,
- the order schemas were authored in.

The hash is a property of the **contract**, not the build environment.
This is what makes `TypeId` byte-equivalent across plugins built on
different machines if and only if their schema set matches.

### 7.3 Per-type ABI provenance

The descriptor's `(fqn, schema_version, schema_source_blake3)` triple
is the per-type ABI provenance. Together with the global
`glibre_types_abi_hash`, it answers three distinct questions:

| Question                                    | Field                          | Mismatch refusal arm                       |
|---------------------------------------------|--------------------------------|--------------------------------------------|
| "Does this plugin link the right middleman?" | `glibre_types_abi_hash`        | `core::Error::PluginAbiHashMismatch`       |
| "Is the layout of this fqn the same?"       | `schema_source_blake3`         | `core::Error::PluginAbiHashMismatch` (§3.7) |
| "Is the version the same or newer?"         | `schema_version`               | `core::Error::SchemaVersionRegression`     |

The triple-axis discipline matches the three independent version
axes (`plugin-abi.md` §"Versioning Rules"): the global hash answers
the first axis (middleman ABI), the per-type hash answers the second
(layout drift on a single fqn), the per-type version answers the
third (semantic evolution). Every refusal is detectable at admission
time without running any plugin code.

### 7.4 ABI stability rules (transcribed)

1. Generated POD structs are tag-sorted ascending; layout independent
   of schema author order (`fory-codegen.md` §"ABI Stability" #1).
2. Generated structs are `final`, contain only built-in scalars or
   other generated types, no virtuals, no vtables, no user-defined
   ctors beyond `= default` (`fory-codegen.md` §"ABI Stability" #2).
3. Adding a field at a new tag is ABI-additive only if it appends
   past the last existing field's offset; otherwise codegen bumps
   the schema's major version and forces a migration
   (`fory-codegen.md` §"ABI Stability" #3).
4. The descriptor table itself is layout-stable: `Descriptor`'s
   field set is closed by this design and changes only under a
   layout-breaking middleman SONAME bump
   (`fory-codegen.md` §"ABI Stability" #5).
5. Removing a field converts its tag to `reserved` immediately;
   codegen rejects tag reuse (`fory-codegen.md` §"Schema File Format"
   reserved-tag rule).

## 8. Hot-reload integration

The registry is a participant in `HotReloadBarrier`'s four-step state
machine (`core/SPEC.md` §8, `hot-reload-protocol.md`):

| Step       | Owner   | Registry's role                                                                     |
|------------|---------|--------------------------------------------------------------------------------------|
| 1. Drain   | Barrier | None. Existing descriptors remain pointer-stable.                                    |
| 2. Swap    | Barrier | Provides `descriptor(existing_type_id)` to validate `schema_hash` parity (§3.7).     |
| 3. Migrate | Barrier | Reads `(stored_version, current_version)` per type to trigger migration chains.      |
| 4. Resume  | Barrier | Commits the candidate's batch (`commit_batch`), publishes new entries.               |

### 8.1 Registry diff at the barrier

Before vtable mutation (step 2 of the barrier), the barrier computes
a per-plugin diff between the surviving registry state and the
candidate's `manifest.components`:

| Diff class              | Old state         | New state                             | Action                                                                                          |
|-------------------------|-------------------|---------------------------------------|-------------------------------------------------------------------------------------------------|
| **Added type**          | Not in registry   | In `manifest.components`              | Register a new descriptor at step 2.4; commit at step 4. Pure-additive; no migration.           |
| **Unchanged type**      | In registry, hash equal | Same hash + version             | Idempotent re-registration; descriptor's `owner` updated to the new `PluginId`. No migration.   |
| **Bumped version**      | In registry, version `N` | Same fqn + same blake3 + version `N+k` (k>0) | Migrate: queue migration chain `N → N+k` per type; descriptor's `schema_version` updated at step 4. |
| **Schema-hash drift**   | In registry, blake3 = X | Same fqn + blake3 ≠ X            | Refuse: `core::Error::PluginAbiHashMismatch` wrapped under `HotReload`. (§3.7, §8.7 of `core/SPEC.md`.) |
| **Removed type**        | In registry       | Absent from `manifest.components`     | Refuse: `core::Error::PluginManifestInvalid` wrapped under `HotReload`. (§8.4 step 2.2 of `core/SPEC.md`.) |
| **Version regression**  | In registry, version `N` | Same fqn + same blake3 + version `M < N` | Refuse: `core::Error::SchemaVersionRegression` wrapped under `HotReload`. (§3.8, this design.) |

The diff classes correspond one-for-one to the §10 refusal arms
relevant to the registry plus the two migration-or-pass-through
classes. The diff is computed by walking
`manifest.components` and querying `find_by_fqn` for each entry; it
is `O(M)` over the manifest's component count and runs once per
hot-reload transaction. The cost is bounded by `plugin-loader-design.md`
§9.2's per-load budget (the loader's step-9 register sequence
incorporates the diff).

### 8.2 Migration dispatch

The registry **does not** invoke migration functions itself.
Migrations live in `glibre-types.dylib`'s static migration table
(`fory-codegen.md` §"Migration Mechanic"). The registry's
contribution is the two version numbers passed to the migration
table:

```cpp
// Inside HotReloadBarrier::step, at step 3.
for (const auto& bumped_type : diff_bumped_types) {
    const auto stored_version  = registry.descriptor(bumped_type.id)->schema_version;
    const auto current_version = bumped_type.candidate_version;
    auto migrate_result = glibre::types::migration_chain(
        bumped_type.id, stored_version, current_version, arena);
    if (!migrate_result) {
        return std::unexpected{core::Error::SchemaMigrationFailed};
    }
}
```

The chain runs out-of-band of the registry; the registry's role is
informational. After every row of every bumped type's storage is
migrated successfully, the barrier commits the batch (which updates
the descriptors' `schema_version` to the new value); on any chain
failure, the barrier rolls back (which leaves descriptors at the
prior version).

### 8.3 What survives the swap (registry-side)

Per §8.1 of `core/SPEC.md`:

- **Every descriptor admitted before the swap survives.** The
  registry is append-only as observed externally; the swap may add
  new descriptors, may update an existing descriptor's
  `schema_version` and `owner`, but never removes a slot or
  invalidates a `TypeId.value`.
- **`TypeId` integers are stable.** A `TypeId` issued before the
  swap addresses the same fqn after the swap. Every codegen-emitted
  thunk that captured a `TypeId` continues to work.
- **Descriptor pointers handed out by `descriptor()` are stable.**
  Pointer-stability rule (§5) survives the swap because
  `descriptors_` is never relocated.
- **`schema_source_blake3` is stable for unchanged types.** Drift
  on an existing fqn refuses the swap (§8.1 row "schema-hash
  drift") so the survivors are all hash-equal across the swap.

Because the registry is append-only and pointer-stable, a hot-reload
that adds new types is a pure-additive `O(M)` append; a hot-reload
that bumps versions changes only the `schema_version` field of
existing descriptors (a 16-bit store under the loader thread's
exclusive ownership of phase 8 — single relaxed atomic store, not
a re-allocation). The fast path is correspondingly cheap.

### 8.4 Refusal cases (cross-reference)

The §10 table below transcribes the registry-relevant refusals; the
authoritative table is §10.1 of `core/SPEC.md`. The registry-
specific refusals:

- `TypeUnregistered` — `descriptor()` or `find_by_fqn()` miss.
- `TypeRegistryClosed` — mutation outside an open batch.
- `PluginAbiHashMismatch` — schema-hash drift on existing fqn.
- `SchemaVersionRegression` — version regression on existing fqn (new arm, §3.8).
- `OutOfBudget` — descriptor reserve exhausted; `eastl::vector`
  push_back would re-allocate beyond the heap ceiling.

Every refusal preserves the registry's prior state byte-for-byte.

## 9. Performance

The registry's contribution to per-frame and per-load budgets, locked
against `perf-budget.md` §"Per-Context Budget Table" and §9.3 of
`core/SPEC.md`:

### 9.1 Per-frame (steady-state)

| Cell                        | Budget               | Source                           |
|-----------------------------|----------------------|----------------------------------|
| `core` CPU sim (registry)   | 0 ms                 | The registry is not on the per-frame path (§5). |
| `core` CPU submit (registry)| 0 ms                 | Same.                            |

The registry's per-frame contribution is **zero** by construction:
the codegen-emitted system thunks resolve component columns by
captured `Descriptor*` (resolved once at schedule compile), bypassing
the registry on every subsequent frame. The registry's data is read
only when the schedule is compiled — at startup or at successful
hot-reload — never during steady-state ticking.

### 9.2 Per-load (cold path)

| Operation                                  | Budget          | Source                                     |
|--------------------------------------------|-----------------|--------------------------------------------|
| `TypeRegistry::create` (load descriptor table) | <1 ms (300 types) | One-time deserialize from `glibre-types.dylib` |
| `register_component` (per call)            | <1 µs           | hash-map insert + vector append            |
| `find_by_fqn` (per call)                   | <100 ns         | hash-map lookup                            |
| `descriptor(TypeId)` (per call)            | <10 ns          | array index + bounds check                 |
| `begin_batch` / `commit_batch` / `rollback_batch` | <1 µs    | snapshot / atomic store                    |

The wall-time per-call budgets are derived from the per-call cost of
each underlying data-structure op on M1 firestorm; the totals fold
into the loader's §9.2 cold-load budget
(`plugin-loader-design.md` §9.2). The registry is not the bottleneck;
`dlopen` and the plugin's own `register` body dominate.

### 9.3 Heap accounting

| Component                          | Resident bytes (MVP scale)        | Budget cell                        |
|------------------------------------|------------------------------------|-------------------------------------|
| `descriptors_` (vector)            | ~80 B × 2 048 = 160 KiB            | `core` 64 MiB; registry sub-share ~256 KiB |
| `by_fqn_` (hash map)               | ~64 B × 2 048 = 128 KiB            | Same.                               |
| `by_owner_` (hash map)             | ~96 B × 16 plugins = 1.5 KiB       | Same.                               |
| Reserve over MVP scale (10× margin)| ~1.2 MiB total                     | Same.                               |

Reserve sizing: the R-1.3.1 ≥10 000 type budget means the descriptor
vector reserves 10 240 slots × 80 B ≈ 800 KiB at construction
(rounded up to a 1 MiB allocation by `PerContextAllocator`'s slab
size). MVP plugin counts top out at ~16 plugins × ~32 components
each ≈ 512 types; the reserve covers 20× headroom. Exhausting the
reserve refuses with `OutOfBudget` per `perf-budget.md` Allocator
Rule #2.

The registry's resident heap is well inside `core`'s 64 MiB ceiling
and inside the §9.3 of `core/SPEC.md` per-aggregate sub-share for
the type-registry sub-module.

### 9.4 Performance contract per harmonius R-1.3.1

R-1.3.1 demands:

- **≥10 000 registered types**: covered by the reserve (§9.3) and
  the `u64` `TypeId` namespace.
- **O(1) lookup**: covered by the dense array + bounds check (§3.5,
  §6.2). No string lookup on the lookup-by-id path; `find_by_fqn`
  is hash-map O(1) average.
- **Lock-free concurrent reads from any thread**: covered by the
  acquire-load + pointer-stability + tombstone discipline (§6.2).
  No mutex, no read-side fence beyond the acquire load.
- **Duplicate type IDs SHALL produce a diagnostic error naming both
  conflicting types**: covered at *codegen time* (§3.6); the runtime
  can never observe a duplicate because `glibre-foryc` rejects the
  build with both source-file paths in the error message.

The "ThreadSanitizer-clean reads from 8 threads" verification is in
the §11 test plan as `tests/core/type-registry/perf/lockfree_reads_8t`.

## 10. Failure modes

The §10.1 failure-mode table in `specs/core/SPEC.md` is authoritative.
This section enumerates the **registry-emitted** rows and adds one
new arm (`SchemaVersionRegression`) that this design contributes.

### 10.1 Registry-emitted `core::Error` arms

| Arm                          | Trigger                                                                                                          | Recovery   | Operator action                                                                              |
|------------------------------|------------------------------------------------------------------------------------------------------------------|------------|----------------------------------------------------------------------------------------------|
| `TypeUnregistered`           | `descriptor()` or `find_by_fqn()` for a `TypeId` / fqn not present (or tombstoned) in the registry.              | Refuse     | Verify the plugin that owns the fqn is loaded; check `manifest.components`.                  |
| `TypeRegistryClosed`         | `register_component` called outside an open batch (`begin_batch` not called or already committed).               | Refuse     | Loader bug; report an issue. Plugins never see this directly because they call inside the loader's batch. |
| `PluginAbiHashMismatch`      | `register_component` finds the fqn already registered with a different `schema_source_blake3` (§3.7).            | Refuse / Rollback | Bump the schema version; ship a migration body. (`fory-codegen.md` §"Migration Mechanic").     |
| `SchemaVersionRegression`    | `register_component` finds the fqn already registered with a newer `schema_version` (§3.8). **NEW arm.**         | Refuse / Rollback | Plugin is older than the running engine state; rebuild against the current middleman.        |
| `OutOfBudget`                | Descriptor reserve exhausted; `eastl::vector` would re-allocate past the heap ceiling.                            | Refuse     | Increase the reserve (next cycle), or split the plugin set across two engine instances.      |
| `FramePhaseMisordered`       | (Debug-build assertion only.) Mutation called from a thread other than the loader thread or outside phase 8.    | Abort      | Loader bug; report an issue.                                                                 |

### 10.2 What the registry does NOT raise

Out-of-scope; raised by other aggregates:

- `EntityStale`, `EntityForeignWorld`, `HierarchyCycle` — `World`'s.
- `ScheduleAccessConflict`, `SystemScheduleCycle` — `Schedule`'s.
- `Plugin*` (other than `PluginAbiHashMismatch`) — `PluginLoader`'s
  (`plugin-loader-design.md` §10.1).
- `HotReload`, `HotReloadDrainTimeout`, `HotReloadAbiHashMismatch`,
  `HotReloadSelfReference`, `SchemaMigrationFailed` —
  `HotReloadBarrier`'s (§8 of `core/SPEC.md`).
- `AssetStale` — `AssetHandle`'s.
- `CommandBufferOverflow` — `CommandBuffer`'s.

The registry's refusals can be **wrapped under** `core::Error::HotReload`
when raised from inside `HotReloadBarrier::step` (§8.7 of
`core/SPEC.md`); the umbrella is the barrier's contribution, not the
registry's.

### 10.3 SPEC §10.1 amendment required

This design introduces `core::Error::SchemaVersionRegression` as a
new arm. The §10.1 failure-mode table in `specs/core/SPEC.md` must
gain one row (in plan-issue terms — not amended in this spike per
"design committed via PR with title …; issue stays OPEN"):

```
| `SchemaVersionRegression`        | A type re-registers an existing fqn with a `schema_version` strictly less than the registered version (§4.9, type-registry-design §3.8). | Refuse / Rollback | `HotReloadRefusedEvent` (when raised under §8.4) | `error` (at load) / `warn` (under §8.7 umbrella) |
```

The matching enumerator in §5.1 (`enum class core::Error`) and the
type-sketch in `error-model.md` (Type Sketch line for `core::Error`)
both gain one entry. The §11 acceptance criteria gain a Catch2 test
named `core/type-registry/register/version_regression_returns_arm`.
Tracked as an open question (§12 below).

## 11. Test plan

Tests are split between Catch2 unit tests under
`tests/core/type-registry/` and Catch2 + integration tests under
`tests/core/integration/`. Every test names a §10 row, a §3 algorithm
step, or a §4 surface contract. Coverage closure: every §10 row and
every §3 sub-section has at least one test.

### 11.1 Unit tests — `TypeRegistry`

| TC ID                                                          | Trigger                                                                              | Expectation                                                                                              | §-link        |
|----------------------------------------------------------------|--------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------|---------------|
| `core/type-registry/lookup/descriptor_by_id_ok`                | Register one type, query its `descriptor(TypeId)`.                                  | Returns ok with `(size, align, fqn, schema_version)` matching the registration.                         | §3.5          |
| `core/type-registry/lookup/descriptor_invalid_id`              | Query `descriptor(TypeId::invalid())`.                                              | Returns `unexpected(TypeUnregistered)`.                                                                 | §3.2 #5       |
| `core/type-registry/lookup/descriptor_out_of_range`            | Query `descriptor(TypeId{ size_ + 1 })`.                                            | Returns `unexpected(TypeUnregistered)`.                                                                 | §3.5          |
| `core/type-registry/lookup/find_by_fqn_ok`                     | Register `glibre.test.Foo`; call `find_by_fqn("glibre.test.Foo")`.                  | Returns the same `TypeId` as the register call.                                                         | §3.5          |
| `core/type-registry/lookup/find_by_fqn_miss`                   | `find_by_fqn("glibre.test.Bar")` with no such fqn registered.                       | Returns `unexpected(TypeUnregistered)`.                                                                 | §3.5          |
| `core/type-registry/register/idempotent_same_hash`             | Register the same fqn + same `schema_source_blake3` + same version twice.           | Second call returns the same `TypeId`; descriptor is unchanged.                                         | §3.7 #2 same  |
| `core/type-registry/register/schema_hash_drift_refuses`        | Register fqn `Foo` with hash A, then with hash B.                                   | Second call returns `unexpected(PluginAbiHashMismatch)`; first descriptor unchanged.                    | §3.7 #2 diff  |
| `core/type-registry/register/version_regression_returns_arm`   | Register fqn `Foo` v3, then attempt registration of v2 with same hash.              | Returns `unexpected(SchemaVersionRegression)`. Registry state byte-equal to pre-call.                   | §3.8          |
| `core/type-registry/register/version_advance_replaces_owner`   | Register fqn `Foo` v1, then v2 with same hash. (Inside a barrier batch.)            | Descriptor's `schema_version` is 2; `owner` is the new plugin; `TypeId` unchanged.                      | §3.7 #2 newer |
| `core/type-registry/register/closed_after_init_refuses`        | Call `register_component` outside an open batch.                                    | Returns `unexpected(TypeRegistryClosed)`.                                                              | §4.9 inv. 1   |
| `core/type-registry/abi/typeid_canonical_order`                | Build two test plugins with disjoint type sets; compute their `TypeId` for shared core fqns. | Same `TypeId.value` integer for the same fqn across both plugins (canonical-order proof).               | §3.2 #2       |
| `core/type-registry/abi/abi_hash_byte_equal_across_runs`       | Run `glibre-foryc` twice on the same `data/schemas/`; compare the embedded hash.    | Hex strings byte-equal.                                                                                 | §7.2          |
| `core/type-registry/abi/abi_hash_excludes_build_env`           | Build the middleman with `__DATE__` / `__TIME__` macro variations.                   | `glibre_types_abi_hash` byte-equal across the variations.                                                | §7.2          |
| `core/type-registry/abi/fory_round_trip_descriptor`            | Serialize a `Descriptor` POD via Fory; deserialize; compare.                        | Byte-equal round-trip; `is_trivially_copyable_v<Descriptor>` holds.                                     | §3.3, §7.1    |
| `core/type-registry/batch/commit_publishes_atomically`         | Open batch, register N types, commit. Read `size_` from a worker thread.            | Reader observes either the pre-commit or post-commit size, never an intermediate.                       | §6.2          |
| `core/type-registry/batch/rollback_pops_dense`                 | Open batch, register N types at the tail of `descriptors_`, rollback.               | `size_` returns to the pre-batch value; descriptors removed; no tombstones used (the dense-pop case).    | §3.4 step 4   |
| `core/type-registry/batch/rollback_tombstones_when_interleaved`| (E2E only via `GLIBRE_E2E` test seam.) Two batches interleave on the test thread; rollback the earlier. | Earlier batch's slots are tombstoned; lookups for those slots return `TypeUnregistered`; later batch's slots survive. | §3.4 step 4 |
| `core/type-registry/perf/lookup_bench`                         | `BENCHMARK_CELL` over 10 000 random `descriptor(TypeId)` calls.                     | Mean time < 50 ns; p99 < 200 ns; resident heap < 1.2 MiB.                                               | §9.2          |
| `core/type-registry/perf/lockfree_reads_8t`                    | 8 reader threads + 1 writer thread × 1 000 batches; ThreadSanitizer enabled.        | No races reported; reader observations are monotone-non-decreasing on `size_`.                          | §6.2, §9.4    |

### 11.2 Unit tests — codegen + middleman ABI

| TC ID                                                  | Trigger                                                                       | Expectation                                                                                          | §-link |
|--------------------------------------------------------|-------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------|--------|
| `core/type-registry/codegen/duplicate_fqn_build_fails` | Author two `.fory` files declaring the same fqn under different paths.        | `glibre-foryc` exits non-zero; stderr names both source-file paths.                                  | §3.6   |
| `core/type-registry/codegen/descriptor_size_stable`    | After a clang upgrade or padding-affecting change, regenerate descriptors.    | Build refuses if `sizeof(Descriptor)` changed without an explicit recorded-size update.              | §3.6   |
| `core/type-registry/codegen/canonical_order`           | Author schemas in three different filesystem orderings; codegen and inspect.  | Descriptor table is byte-equal across the orderings.                                                 | §3.6   |

### 11.3 Integration tests

| TC ID                                                       | Trigger                                                                                                         | Expectation                                                                                              | §-link        |
|-------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------|---------------|
| `core/integration/type-registry/plugin_register_full_sweep` | Three plugins registering a total of ~64 types; assert post-load that every fqn resolves.                       | All `find_by_fqn` calls return ok; descriptor pointers stable across plugin loads.                       | §3.7          |
| `core/integration/type-registry/hot_reload_added_types`     | Reload one plugin whose new manifest adds a new component fqn.                                                  | Post-reload, `size_` increased by 1; old types unchanged; new fqn resolves.                              | §8.1 added    |
| `core/integration/type-registry/hot_reload_bumped_version`  | Reload one plugin whose `Foo` schema bumps from v1 to v2 with same blake3 (additive-only field).                | Post-reload, descriptor's `schema_version` is 2; existing storage rows migrated; integration with §11.1's `version_advance_replaces_owner`. | §8.1 bumped  |
| `core/integration/type-registry/hot_reload_drift_refuses`   | Reload a plugin whose manifest declares the same fqn with a different `schema_source_blake3`.                  | `barrier.step()` returns refusal; `HotReloadRefusedEvent` fires with `core::Error::HotReload` umbrella + inner `PluginAbiHashMismatch`; pre-reload registry byte-equal. | §8.1 drift |
| `core/integration/type-registry/hot_reload_dropped_refuses` | Reload a plugin whose manifest drops a previously-registered fqn.                                              | `barrier.step()` returns refusal with inner `PluginManifestInvalid`; registry byte-equal.               | §8.1 dropped  |
| `core/integration/type-registry/hot_reload_regression_refuses` | Reload a plugin whose manifest declares an older version than the surviving registry entry.                   | `barrier.step()` returns refusal with inner `SchemaVersionRegression`; registry byte-equal.             | §3.8, §8.1    |
| `core/integration/type-registry/cross_plugin_typeid_byte_equal` | Two plugins citing the same core fqn (`glibre.core.Transform`) compare their compiled-in `TypeId` constants. | Integers byte-equal.                                                                                     | §3.2 #2       |
| `core/integration/golden/type_registry_byte_equal_replay`   | Drive the full §10 row set with deterministic seeds; capture the structured error stream.                       | Stream byte-equal across two runs (deterministic-replay obligation, `core/SPEC.md` §10.2 #7).            | §10           |

### 11.4 Coverage matrix

Every §10 row has at least one test (Refuse-arms named in §11.1;
hot-reload-context arms named in §11.3). Every §3 algorithm step has
at least one unit test. The single performance benchmark
(`core/type-registry/perf/lookup_bench`) covers the §9.4 R-1.3.1
performance contract; the multi-thread test
(`core/type-registry/perf/lockfree_reads_8t`) covers the
"lock-free concurrent reads" clause.

## 12. Open questions

- **[OPEN] `SchemaVersionRegression` enum addition to
  `core/SPEC.md` §5.1 + `error-model.md` Type Sketch.** This design
  introduces the arm but per the "design committed via PR; issue
  stays OPEN" rule, the SPEC amendment is the next plan in the
  sequence (sibling task-breakdown spike). The amendment is
  mechanical: add the enumerator, add a §10.1 row, add a §11
  acceptance-criteria test name. No semantic disagreement with any
  decision record.

- **[OPEN] `TypeId` bit-budget allocation for plurality-`World`
  (post-MVP).** §3.2 reserves the upper bits of `TypeId.value`. A
  future multi-world story (`core/SPEC.md` §3.3 R-1.4.* refusal at
  current scope) may want some of those bits as a world tag. Keep
  the bottom 24 bits as the dense index; reserve the top 32 bits
  for that future use. Resolution gate: the multi-world spike when
  it lands.

- **[OPEN] Reserve sizing for `descriptors_`.** §9.3 picks 10 240
  slots (10× the R-1.3.1 budget) at construction. If MVP plugin
  authors push past this in a single session, the registry refuses
  with `OutOfBudget`. Resolution: bench the actual MVP sample-scene
  type count and either commit a fixed reserve or expose a
  `World::create` parameter. Defer until the MVP plugin set
  stabilizes.

- **[OPEN] Tombstone discipline necessity in MVP.** §3.4 admits
  tombstones to support a future multi-thread loader; MVP is
  strictly serial so the dense-pop branch is the only path
  exercised. The tombstone code lives behind a runtime branch (a
  free bit on `flags`) — keeping it costs ~1 instruction per
  lookup. Confirm the cost is acceptable; alternative is to
  conditionally compile out the tombstone path under `-DGLIBRE_MVP`
  and re-enable when multi-thread loader lands.

- **[OPEN] `iterate_owned` exposure beyond loader.** §3.5 marks
  `iterate_owned` as loader-internal. Editor tooling may want a
  read-only enumeration of "types owned by plugin P" for the type
  browser; if so, expose a public `iterate_owned_const` that
  returns a span of `TypeId`s. Defer until the editor type-browser
  story opens.

- **[OPEN] `Descriptor::flags` bit-7 reservation for
  `is_resource_singleton`.** §3.3 dedicates bit 0 to
  `is_resource_singleton`; bits 1..6 are unallocated; bit 7 is the
  tombstone bit. If a future descriptor field needs more than one
  more bit (e.g., `is_replicated`, `is_serializable`, `is_event`),
  the byte will need to grow. Keep at one byte for MVP and revisit
  when the third flag lands.

- **[OPEN] Editor-time descriptor enumeration vs.
  schema-file enumeration.** The editor's "type browser" feature
  (post-MVP) needs a list of every fqn + schema. The registry can
  serve this via `iterate_owned_const`-like APIs, or the editor
  can read the `.fory` source directly. The latter is cheaper in
  shipping (no descriptor table marshaling) but requires the
  editor to understand the `.fory` grammar. Defer to the
  `tools` SPEC; not blocking for `core`.

- **[OPEN] Per-`World` `TypeId` namespacing.** MVP runs one
  `World` per process so the registry is effectively process-wide.
  When the plurality-`World` story (`core/SPEC.md` §3.3 R-1.4.*
  defer) lands, two worlds must agree on `TypeId.value` for the
  same fqn (otherwise cross-world handle migration is impossible).
  Likely resolution: the `TypeRegistry` is a process-singleton
  shared by every `World`, with each `World` owning its own
  storage but referencing the same registry. Confirm at the
  multi-world spike.
