# data — Detailed Design: schema-registry aggregate

> Detailed design for the `SchemaRegistry` aggregate declared in
> `specs/data/SPEC.md` §4.5. Refines §4.5, §4.10 inv. 1, §5 (registry.hpp
> stub), §6, §7.1, §8.1, §8.5, §9.2, §10.1 in place; cites
> `reviews/decisions/fory-codegen.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/error-model.md`,
> `reviews/decisions/frame-phases.md`, and
> `reviews/decisions/perf-budget.md`. Sibling design (different
> abstraction): `specs/core/type-registry-design.md` — see §1 for the
> SRP fence. Does not introduce new public surface beyond
> `specs/data/SPEC.md` §5; deviations from the cited records would
> require an amendment spike, not an in-place edit.

Refs: spike #730 — `[SPIKE] design-data-schema-registry-detailed`.
Parent sub-epic #729. Sibling task-breakdown spike blocked-by this
deliverable.

## 1. Purpose

`SchemaRegistry` is the per-process catalog of **Fory schemas** that
the `data` context owns inside `glibre-types.dylib`. Concretely it
maps every fully-qualified type name (`FQN`) declared by some
`data/schemas/<ctx>/<Type>.fory` file to the codegen-emitted
descriptor that the runtime serdes pipeline needs:
`(SchemaVersion, SchemaSourceHash, serialize_fn, deserialize_fn,
MigrationChain, ReflectionBlob*)` (`specs/data/SPEC.md` §4.5).

The registry's one responsibility — **catalog Fory schemas with their
stable FQN, monotonic version chain, and source-blake3 hash for the
lifetime of a process** — is sharp. If the descriptor record shape,
the per-FQN version-chain rule, the lookup keying, the ABI-hash
composition input shape, or the registry's interaction with hot-reload
changes, this design changes. Anything else is out of scope.

What the schema-registry **explicitly refuses to own**:

- **ECS component descriptor catalog (`core::TypeRegistry`).**
  `core::TypeRegistry` (`specs/core/type-registry-design.md`) maps
  `TypeId` → ECS column descriptor (`size`, `align`, `drop_thunk`,
  `copy_thunk`, `storage_hint`). It is a sibling registry in a
  different bounded context. The fence is sharp:
  - `core::TypeRegistry` keys are dense `TypeId.value : u64`.
    `data::SchemaRegistry` keys are `SchemaId{eastl::string_view fqn}`.
  - `core::TypeRegistry` describes ECS storage layout. `data::SchemaRegistry`
    describes Fory wire encoding plus migration chains.
  - `core::TypeRegistry`'s `Descriptor::schema_source_blake3` and
    `schema_version` fields are **read out of `SchemaRegistry`** at
    plugin admission (`specs/core/type-registry-design.md` §3.7) —
    `core` consumes; `data` is the source of truth.
  - A type may have a `core::TypeRegistry` entry (it is an ECS
    component) **and** a `data::SchemaRegistry` entry (it is
    persistent), or only the latter (asset payload, plugin-private
    persistent record), but the two registries are queried through
    different APIs and live in different dylibs.
- **The middleman dylib build target itself.** `glibre-types.dylib`
  (`specs/data/SPEC.md` §4.3, spike #734) owns its module layout,
  static-init sequence, plugin-link contract, and SONAME policy.
  This design only specifies the registry table the dylib carries.
- **Envelope serdes.** `Envelope<T>::serialize` /
  `Envelope<T>::deserialize` (`specs/data/SPEC.md` §4.8, spike #736)
  are the consumers of `SchemaRegistry`; they call `lookup(fqn)`
  through a cached descriptor pointer. The registry exposes the
  pointer; envelope serdes interprets bytes.
- **Migration dispatcher.** `MigrationDispatcher`
  (`specs/data/SPEC.md` §6.3, spike #738) composes the per-FQN
  `MigrationChain` into actual migration runs at the hot-reload
  barrier. The registry stores the chain table; the dispatcher
  walks it. The fence is exactly the "registry holds; dispatcher
  invokes" rule that `specs/core/type-registry-design.md` §8.2 also
  observes.
- **Schema source-canonicalization.** The `.fory` file format and
  its byte-canonicalization rule (`specs/data/SPEC.md` §7.1, §7.3)
  are owned by the `Schema` aggregate (§4.1) and the `Foryc`
  codegen tool (§4.2). The registry stores the resulting blake3
  digest; it does not recompute it.
- **Runtime reflection.** `ReflectionBlob` is descriptor-resident
  data the codegen wrote (`specs/data/SPEC.md` §4.9 inv. 1); the
  registry exposes the slot but performs no introspection itself.
  Shipping builds null the slot per §4.9 inv. 5.
- **Mid-frame mutation.** Registrations land at frame boundaries
  only — static-init pre-frame for the single live middleman build,
  or wholesale registry replacement at phase 8 hot-reload (Mode B,
  out of MVP scope per §8.3 of the SPEC). The registry refuses
  mid-frame writes by construction (§6 below).

The SRP boundary is sharp by construction: the schema-registry catalogs
Fory schemas (an authoring artifact); the type-registry catalogs ECS
component types (a runtime storage artifact); the middleman dylib
hosts both via separate translation units. Two reasons to change →
two designs. This document covers the first.

## 2. Requirements coverage

This section verifies that every harmonius MVP-scope requirement /
design clause about a Fory-schema-registry-shaped catalog is either
covered by the design below or explicitly refused with rationale.
Inputs (research only — every conclusion re-derived against
PHILOSOPHY and the cited decision records per `specs/data/SPEC.md`
§3):

- `harmonius/docs/requirements/data-systems/data-tables.md` — the
  `R-16.3.x` table-schema clauses.
- `harmonius/docs/requirements/data-systems/attributes-effects.md`,
  `containers-slots.md`, `directed-graphs.md` — collectively the
  `rkyv` archive-derive scatter that this registry collapses (per
  `specs/data/SPEC.md` §3.2 Occam-collapse statement).
- `reviews/decisions/fory-codegen.md` §"middleman dylib exposes" #3
  (the single sentence that locks the registry's record shape).

| Harmonius clause                                                                                  | Glibre disposition                                                                                                                                                                                                                                                                                                                                                                                |
|---------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-16.3.1 / .2 / .3** — Typed table schemas with named, typed columns; per-row identity and version | **Partially covered, scope-narrowed.** The "typed schema with named, typed fields" axis is covered: every persistent type — including any future `data-table` row type — is authored as a `.fory` file and ends up as a `SchemaRegistry` entry. Per-row identity / per-row version / table-level evaluation are **refused** (`specs/data/SPEC.md` §3.3): the registry catalogs **types**, not **rows**. |
| **R-16.3.x** — Foreign-key resolution, indices, joins, prototype-chain inheritance, locale tables  | **Refused** (`specs/data/SPEC.md` §3.3). None are serialization concerns; queries against authored data live in domain plugins, not in the registry.                                                                                                                                                                                                                                              |
| Implicit across `R-16.x` — Each domain rolls its own `Archive`/`Serialize`/`Deserialize` derivations | **Collapsed.** N domain-specific harmonius `rkyv` derivations → 1 glibre Fory-codegen pipeline producing 1 middleman dylib gated by 1 `glibre_types_abi_hash` (`specs/data/SPEC.md` §3.2; `reviews/decisions/fory-codegen.md` §"Pipeline"). The registry is the single shared catalog that absorbs all of those scattered concerns into one read-only table.                                       |
| Implicit — "Anything persistent has a schema; anything not persistent does not"                    | **Covered.** The `SchemaRegistry` ⇔ `.fory` file biconditional is the load-bearing §4.10 inv. 1 of `specs/data/SPEC.md`; this design inherits it (§3.6 below) and asserts it at codegen time.                                                                                                                                                                                                     |

Glibre-native requirements added beyond harmonius:

- **Per-FQN strictly-monotonic version chain.** A type's
  `SchemaVersion` is strictly monotonic across its history; older
  versions never reappear (`specs/data/SPEC.md` §4.10 inv. 2). The
  registry enforces this at static-init by rejecting any duplicate
  `(FQN, version)` pair and at hot-reload by refusing
  `version < current` for an already-registered FQN (§3.6 below).
- **Source-blake3 byte equality is the layout-stability oracle.**
  Two registry entries with the same `FQN` and the same
  `SchemaVersion` must have byte-equal `SchemaSourceHash`; otherwise
  the plugin loader refuses with `core::Error::PluginAbiHashMismatch`
  (`reviews/decisions/plugin-abi.md` §"ABI Hash Function";
  `specs/core/type-registry-design.md` §3.7). Detected at admission
  time without running any plugin code.
- **`SchemaRegistry::lookup(fqn)` returns a pointer that is stable
  for the registry's lifetime.** Codegen-emitted serdes thunks
  resolve the pointer once at load + barrier, cache it, and
  dereference per-call without re-traversing the registry (§5
  below). The pointer-stability rule is what makes "lookup at
  load + barrier; runtime serdes reads through cache" workable.
- **Wholesale-replacement hot-reload.** Mode A (per-plugin reload)
  appends rows; Mode B (middleman self-reload, MVP-deferred)
  replaces the registry instance wholesale (`specs/data/SPEC.md`
  §8.3 gate 4). There is no in-place mid-frame mutation in either
  mode; readers always see exactly one well-formed `SchemaRegistry`.

Net result: the only harmonius `data-systems/` clause the registry
absorbs is the "every persistent type wants a schema with a
serializer and a forward-migration story" cross-cutting collapse
(`specs/data/SPEC.md` §3.2). The rest is refused under PHILOSOPHY §6
(no runtime reflection in shipping) and routed to the originating
domain plugins (R-16.1, R-16.2, R-16.4) or the editor / tools layer.

## 3. Detailed model

### 3.1 Aggregate composition

```text
SchemaRegistry (root, owned by glibre-types.dylib)
├── eastl::vector<RegistryEntry>             entries_       (sorted ascending by FQN)
├── eastl::hash_map<eastl::string_view,
│                   std::uint32_t>           by_fqn_index_  (FQN view → index into entries_)
├── eastl::vector<MigrationEntry>            chain_pool_    (per-FQN chains live in slices of this pool)
├── eastl::vector<eastl::string>             fqn_storage_   (interning table; owns the strings entries_ borrows)
├── std::atomic<std::uint32_t>               size_          (published count; relaxed-acquire on read)
└── eastl::string_view                       abi_hash_view_ (borrowed from glibre-types.dylib's abi_hash literal)
```

Container choice rationale (per PHILOSOPHY §11 / EASTL replaces
`std::` containers):

- **`eastl::vector<RegistryEntry>` `entries_`** — sorted ascending
  by `FQN`. Lookup is binary search per `specs/data/SPEC.md` §4.5
  inv. 3 (allocation-free, cache-friendly, `O(log N)`); iteration
  order matches the canonical order that drives ABI-hash
  composition (§3.4). Allocation is tagged
  `glibre::PerContextAllocator{ContextTag::data}`
  (`reviews/decisions/perf-budget.md` §"Allocator Rules" #1).
  `RegistryEntry` is the trivially-copyable POD declared in
  `specs/data/SPEC.md` §5 (registry.hpp stub) — see §3.2 below.
- **`eastl::hash_map<eastl::string_view, std::uint32_t>` `by_fqn_index_`** —
  optional accelerator the cold-path admission paths (§3.5 path C)
  use to detect FQN collisions in `O(1)`. Hot-path serdes never
  touches it; the codegen-emitted descriptor pointer (§5) bypasses
  the map entirely. Built lazily on first cold-path miss; sized
  against the descriptor reserve so it never rehashes mid-session.
- **`eastl::vector<MigrationEntry>` `chain_pool_`** — every per-FQN
  `MigrationChain` is a contiguous slice of this pool. The
  `RegistryEntry::migrations` span borrows into the pool. Single
  pool keeps per-chain memory dense and pointer-stable; new
  registrations append slices (Mode A); a Mode-B reload allocates
  a fresh pool inside the new registry instance and the old pool
  is dropped wholesale (§8 below).
- **`eastl::vector<eastl::string>` `fqn_storage_`** — interning
  table for FQN strings. The registry's public API surface keys on
  `eastl::string_view` (per `specs/data/SPEC.md` §5
  `SchemaId::fqn`), and the interning table is what makes those
  views' lifetime equal the registry's. Borrowed from the
  middleman's static-init descriptor concatenation; never relocated
  after the first commit (§5 pointer-stability rule below).
- **`std::atomic<std::uint32_t>` `size_`** — single piece of
  registry state visible to multi-thread readers. Published with
  `memory_order_release` at static-init's final commit and (Mode-B
  only) at the wholesale `instance()` swap; loaded with
  `memory_order_acquire` by every `lookup`. `u32` because
  R-16.3-style schema counts max in the low thousands; the budget
  in §9 caps the registry well under `2^16` entries.
- **`eastl::string_view` `abi_hash_view_`** — borrowed lifetime;
  the underlying string is the `constexpr` literal exported by
  `glibre_types_abi_hash()` (`specs/data/SPEC.md` §4.4 inv. 5;
  `reviews/decisions/fory-codegen.md` §"middleman dylib exposes" #3).
  Captured once at registry construction so the registry can
  cite it inside `SchemaRegistryConflict` / `AbiHashMismatch`
  diagnostics without reaching back into the dylib's static
  storage at refusal time.

The registry instance is owned by `glibre-types.dylib` itself (the
middleman holds the singleton; `SchemaRegistry::instance()` returns
a `const&` view, per `specs/data/SPEC.md` §4.5 inv. 4 and the §5
stub). One registry per process — Mode B reload publishes a *new*
instance pointer rather than mutating the live one (§8 below).

### 3.2 Per-FQN `RegistryEntry`

The record shape is locked by the §5 stub of `specs/data/SPEC.md`;
this design refines field semantics, layout, and provenance.

```cpp
// from specs/data/SPEC.md §5 registry.hpp (locked).
namespace glibre::types {

struct RegistryEntry {
    SchemaId         schema{};                    // FQN-bearing borrow.
    SchemaVersion    version{0};                  // Current monotonic version.
    SchemaSourceHash source_hash{};               // Blake3 of canonical .fory bytes.

    std::expected<std::size_t, data::Error> (*serialize)(
        const void* value, std::span<std::byte> dst) noexcept = nullptr;
    std::expected<void, data::Error> (*deserialize)(
        std::span<const std::byte> src,
        void* out_value) noexcept = nullptr;

    std::span<const MigrationEntry> migrations{};  // slice of chain_pool_.
    const ReflectionBlob*           reflection{nullptr};  // null in shipping.
};

}  // namespace glibre::types
```

Field semantics:

- **`schema.fqn`** — fully-qualified name, e.g.
  `glibre.core.Transform`. Borrowed from `fqn_storage_` (§3.1);
  lifetime equals the registry instance's lifetime. Conforms to
  `specs/data/SPEC.md` §4.1 inv. 1's regex
  (`[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+\.[A-Z][A-Za-z0-9]*`).
  Validated at codegen time; a non-conforming FQN never reaches the
  registry.
- **`version`** — the current `.fory` schema's declared
  `SchemaVersion` integer (`specs/data/SPEC.md` §4.1 inv. 2).
  Strictly monotonic across the schema's history; used by
  `Envelope<T>::deserialize` to dispatch decode-direct vs. run the
  `MigrationChain` (`specs/data/SPEC.md` §4.8 inv. 5). Used by
  `core::HotReloadBarrier` step 3 to decide whether a migration
  must run for surviving storage of this type
  (`specs/core/type-registry-design.md` §8.2).
- **`source_hash`** — raw 32-byte Blake3-256 of the canonicalized
  bytes of the originating `.fory` source file
  (`specs/data/SPEC.md` §7.3 canonicalization rule). Consumed by:
  1. The §3.4 ABI-hash composition (the registry's per-entry
     contribution to `glibre_types_abi_hash()`).
  2. The hot-reload diff at the barrier (§8 below): byte-equality
     across reloads of the same `(FQN, version)` is the
     layout-stability oracle.
  3. The `SourceHashMismatch` refusal (§10 below) — codegen
     guarantees byte-equal source hashes for byte-equal sources, so
     a mismatch at static-init is a build defect that cannot reach
     production via the §3.6 codegen-time biconditional.
  Stored as raw bytes (not hex) to keep entries compact; the
  human-readable form is derived only when a refusal logs.
- **`serialize` / `deserialize`** — codegen-emitted type-erased
  trampolines per `specs/data/SPEC.md` §4.3 inv. 4 / §5
  registry.hpp. Non-null for every entry (§4.5 inv. 2). The typed
  `Envelope<T>` specializations resolve to these at link time;
  envelope serdes never re-traverses the registry to resolve them
  (the cached descriptor pointer makes the resolution one-shot).
- **`migrations`** — span of `MigrationEntry` rows over a slice of
  `chain_pool_` (§3.1). Sorted ascending by `from_version`
  (`specs/data/SPEC.md` §4.7 inv. 1, 2). For a type at current
  version `M ≥ 2`, the span has length `M - 1` and covers every
  step `1→2, 2→3, …, M-1→M`. Coverage gaps are codegen-time build
  errors (`specs/data/SPEC.md` §4.10 inv. 5); the registry never
  carries a partial chain past the build.
- **`reflection`** — non-owning pointer to a codegen-emitted
  `ReflectionBlob` (`specs/data/SPEC.md` §4.9). Null in shipping
  builds (§4.9 inv. 5); non-null in editor / tools builds. The
  registry treats it as opaque storage; introspection lives in the
  consumer (editor / tools).

Entry size: `RegistryEntry` is on the order of 96 bytes
(`SchemaId` view = 16, `SchemaVersion` = 4, `SchemaSourceHash` = 32,
two function pointers = 16, `migrations` span = 16, `reflection*` =
8, plus padding to 16-byte alignment). The build asserts a stable
sizeof at static-init for the heap accounting in §9. `RegistryEntry`
is `std::is_trivially_copyable_v<RegistryEntry>` so the on-wire
codegen blob the dylib carries (§7 below) round-trips byte-equal.

### 3.3 `SchemaId` as a borrowed FQN view

```cpp
// from specs/data/SPEC.md §5 identity.hpp (locked).
struct SchemaId {
    eastl::string_view fqn{};
    constexpr bool operator==(const SchemaId&) const noexcept = default;
    constexpr auto operator<=>(const SchemaId&) const noexcept = default;
};
```

Encoding rule:

1. **`SchemaId` is a string-view borrow.** The underlying bytes
   live in the registry's `fqn_storage_` (§3.1) and have static
   lifetime within the live middleman build. Consumers may
   freely copy `SchemaId` by value; the view never dangles within
   one registry instance.
2. **Equality is byte-equal FQN comparison.** No
   case-folding, no normalization (the FQN regex restricts the
   character set so byte-equality and Unicode-equality coincide).
   Three-way comparison is byte-lex order — the same canonical order
   that drives `entries_` sort and `glibre_types_abi_hash` input
   (§3.4).
3. **Cross-instance lifetime caveat.** A `SchemaId` borrowed from
   registry instance `R0` does not necessarily refer to live storage
   under instance `R1` after a Mode-B swap (§8 below). MVP
   subscribers either capture by-value `SchemaSourceHash` /
   `SchemaVersion` for cross-swap state or re-resolve through
   `lookup(fqn)` after a `SchemaRegistryChange::RegistryReplaced`
   notification.

Unlike `core::TypeId` (a dense `u64` index), `SchemaId` is a string
view: schema authoring is FQN-keyed and the registry trades the
~16-byte view against a dense-integer scheme because there is no
serdes-side need for an integer type code (the wire-form
`EnvelopeHeader` already carries `SchemaId.fqn` — `specs/data/SPEC.md`
§4.8). A future `EnvelopeHeader` compaction story may add an
optional integer alias; that is `[OPEN]` (§12).

### 3.4 ABI-hash composition input

`glibre_types_abi_hash` is the single scalar the plugin loader
compares on load (`reviews/decisions/plugin-abi.md` §"ABI Hash
Function"; `specs/data/SPEC.md` §4.4). The schema-registry
contributes the per-entry inputs to that hash. The recipe (locked
from `reviews/decisions/fory-codegen.md` §"middleman dylib exposes"
#3 and `specs/data/SPEC.md` §4.4 inv. 1):

1. For every `RegistryEntry` in `entries_` (already sorted ascending
   by FQN — §3.1):
   - emit the byte sequence
     `fqn || ":" || version_le || ":" || source_hash`
     where `version_le` is the `SchemaVersion` (`u32`)
     little-endian-encoded and `source_hash` is the raw 32-byte
     blake3 digest.
2. Concatenate the per-entry sequences with a single `0x0A`
   (newline) separator; no trailing newline.
3. `glibre_types_abi_hash = blake3(input)`, hex-encoded
   (lowercase, 64 chars), stored as a `constexpr` string literal in
   `glibre/types/abi_hash.hpp` and exported via
   `extern "C" auto glibre_types_abi_hash() noexcept -> const char*`
   (`specs/data/SPEC.md` §5 abi_hash.hpp).

Inputs **not** in the hash:

- `serialize_fn` / `deserialize_fn` function-pointer values (they
  are link-time addresses, not part of the contract);
- `MigrationEntry::invoke` function pointers (likewise);
- `ReflectionBlob*` slots;
- registration order / authoring order (the canonical sort by FQN
  collapses author order out);
- compiler / version / pragma packing / `__DATE__` / header
  timestamps (mirroring `specs/core/type-registry-design.md` §7.2's
  exclusion list).

The hash is a property of the **contract** — the (FQN, version,
source bytes) triple per type — not the build environment. Two
middleman builds compiled against byte-equal `.fory` source sets on
different hosts produce byte-equal `glibre_types_abi_hash` values;
this is the property that makes `core::TypeRegistry`'s
schema-source-blake3 drift-check (`specs/core/type-registry-design.md`
§3.7) a meaningful gate.

The composition rule is intentionally identical to `core`'s
type-registry recipe (`specs/core/type-registry-design.md` §7.2)
because the two registries share the same set of `.fory` source
files. The collapse: **one ABI hash, computed once, derivable from
the registry alone**.

### 3.5 Lookup paths

The registry exposes three lookup paths, each with a documented
cost class:

| Path                                  | Caller                                       | Cost          | Frequency                        |
|---------------------------------------|----------------------------------------------|---------------|----------------------------------|
| **A — `lookup(SchemaId)`**            | `Envelope<T>::serialize/deserialize` cache   | `O(log N)` binary search | Cold-path: load + barrier |
| **B — `entries()` iteration**         | ABI-hash composition; editor inspector       | `O(N)`        | Cold-path: build + tools         |
| **C — `find_index_by_fqn(fqn)`**      | Static-init insertion / collision detection  | `O(1)` hash-map | Cold-path: static-init only    |

**Path A — `lookup(SchemaId fqn)`** is the foundational read
path. Implemented as a binary search over the FQN-sorted `entries_`
vector (`specs/data/SPEC.md` §4.5 inv. 3); allocation-free,
branch-predictable, never touches the hash map. Returns
`const RegistryEntry*` (nullptr on miss — translated into
`data::Error::SchemaUnknown` by the calling envelope serdes per
`specs/data/SPEC.md` §10.2). **Per the hot/cold split (§5 below),
envelope serdes calls this path *exactly once per FQN per session*:
at codegen-emitted thunk static-init or at the hot-reload barrier's
`SchemaRegistryChange` notification handler. The resolved
`RegistryEntry*` is then cached by the consumer; per-call serdes
dereferences the cached pointer without re-traversing.**

**Path B — `entries()`** returns a `std::span<const RegistryEntry>`
in canonical order. The §3.4 ABI-hash composition consumes it
once at codegen-emit. The editor's schema browser consumes it once
per `SchemaRegistryChange` event. The shipping runtime hot path
never iterates entries.

**Path C — `find_index_by_fqn(fqn)`** is the auxiliary `O(1)` lookup
used at static-init by the dylib's `_registry.cpp`-emitted
insertion sequence to detect duplicate FQN before any vector append
fires (§3.6 below). Equivalent to scanning `by_fqn_index_`. Not
exposed in the public API surface (`specs/data/SPEC.md` §5
registry.hpp); only the dylib's own static-init code reaches it.

The three paths cover every legitimate consumer:

- **Path A / cached pointer** — runtime serdes (Envelope<T> per type).
- **Path A / direct call** — hot-reload barrier consulting
  `incoming.lookup(fqn)` against `outgoing.lookup(fqn)` for the
  schema-set continuity check (`specs/data/SPEC.md` §8.2 step 1).
- **Path A / direct call** — `core::TypeRegistry`'s loader-side
  schema-hash-drift check (`specs/core/type-registry-design.md`
  §3.7) reads `RegistryEntry::source_hash` and `version` from
  `data::SchemaRegistry::instance()`.
- **Path B** — codegen `_abi_hash.cpp` emits the concatenated input
  bytes from `entries()`; the editor `SchemaBrowserPanel` walks
  `entries()` once on `SchemaRegistryChange`.
- **Path C** — `_registry.cpp` static-init only.

Anything not on this list is either the wrong registry (use
`core::TypeRegistry`), the wrong layer (envelope serdes for
per-payload work), or out-of-contract.

### 3.6 Codegen-emitted descriptors

Every registry entry is **emitted by `glibre-foryc`**
(`reviews/decisions/fory-codegen.md` §"Pipeline"), not authored by
hand. The pipeline:

1. Each `data/schemas/<ctx>/<Type>.fory` (per `fory-codegen.md`
   §"Schema File Format" and `specs/data/SPEC.md` §7.1 file format)
   declares one persistent type with its `FQN`, `SchemaVersion`,
   ordered tags, reserved-tag set, and migration-provider names.
2. `glibre-foryc` computes the canonicalized source bytes
   (`specs/data/SPEC.md` §7.3 canonicalization rule) and the
   `SchemaSourceHash = blake3(canonicalized_bytes)`.
3. `glibre-foryc` emits, alongside the POD struct
   `glibre/types/<ctx>/<Type>.hpp`, the per-type registry literal
   `glibre/types/<ctx>/<Type>.registry.hpp` containing a
   `constexpr RegistryEntry` with `(SchemaId{fqn},
   SchemaVersion{N}, SchemaSourceHash{...},
   &serialize_<fqn_mangled>, &deserialize_<fqn_mangled>,
   migration_chain_for_<fqn_mangled>, reflection_for_<fqn_mangled>
   or nullptr)`.
4. `glibre-types.dylib`'s codegen-emitted `_registry.cpp`
   concatenates every type's registry literal into an
   FQN-sorted-ascending array and constructs the singleton
   `SchemaRegistry` at static-init via:
   - `entries_` push_back per literal (sorted-input fast path; no
     re-sort at runtime);
   - `fqn_storage_` interning (each FQN's bytes copied into the
     interning table; the `SchemaId.fqn` view is rebound to the
     interned copy);
   - `chain_pool_` append per type's `MigrationEntry` slice; the
     entry's `migrations` span is fixed up to point into the pool
     after the appends settle.
5. After every entry has landed, `_registry.cpp` calls the private
   `commit_static_init()` which publishes `size_` with
   `memory_order_release` (§6 below).

Codegen-time guarantees (asserted by `glibre-foryc`, never reach
runtime):

- **No duplicate FQNs.** A second `.fory` declaring an existing FQN
  fails the codegen build; the build error names both source files
  (`specs/data/SPEC.md` §4.5 inv. 1; `reviews/decisions/fory-codegen.md`
  §"ABI Stability Rules"). The registry-side
  `SchemaRegistryConflict` arm (§10 below) is the static-init
  safety-net that fires only if two distinct middleman builds load
  into one process — a configuration the spine refuses to run
  (`specs/data/SPEC.md` §4.3 inv. 1).
- **Strictly-monotonic version chain.** For a type with current
  version `M`, the schema set must contain exactly the versions
  `[M]` (the live header, current shape) and the chain must
  cover every step `1 → 2, 2 → 3, …, M-1 → M`
  (`specs/data/SPEC.md` §4.7 inv. 1, §4.10 inv. 5). A coverage
  gap is a codegen build error; the registry never carries a
  partial chain. Bumping the same `SchemaVersion` integer twice
  (e.g. two simultaneously-merged PRs both bump from `N` to
  `N+1`) is detected at codegen by the `MigrationStepMissing` /
  duplicate `MigrationEntry::from_version` check.
- **Source-hash byte-equality.** `glibre-foryc`'s canonicalization
  is deterministic (`specs/data/SPEC.md` §7.3 inv.); two builds of
  byte-equal `.fory` source produce byte-equal
  `SchemaSourceHash`. This is the load-bearing oracle the
  `SourceHashMismatch` refusal (§10 below) relies on at
  static-init's collision check and at hot-reload's drift check.
- **Biconditional with `data/schemas/`.** Every `.fory` file under
  `data/schemas/` produces exactly one registry entry; conversely,
  every entry corresponds to a real `.fory` file
  (`specs/data/SPEC.md` §4.10 inv. 1). The CMake glob and the
  registry are reconciled by `glibre-foryc` at configure time
  (`reviews/decisions/fory-codegen.md` §"CMake Integration").

### 3.7 Static-init insertion sequence

```cpp
// glibre-types.dylib internals — _registry.cpp (codegen-emitted).
namespace glibre::types::detail {

void register_one(SchemaRegistry& reg, RegistryEntry literal) noexcept {
    // 1. Intern the FQN.
    const auto interned_fqn = reg.fqn_storage_.emplace_back(literal.schema.fqn);
    literal.schema.fqn = eastl::string_view{interned_fqn};

    // 2. Detect duplicate FQN — codegen-time assertion is the load-bearing
    //    guarantee, but the static-init path enforces it again as a defense-
    //    in-depth measure (two middleman builds in one process).
    if (auto idx = reg.find_index_by_fqn(literal.schema.fqn); idx) {
        glibre::log_error(data::Error{
            .tag         = data::ErrorTag::SchemaRegistryConflict,
            .step_schema = literal.schema,
        }, glibre::Severity::Fatal);
        std::abort();   // §10 fatal arm — the build is broken.
    }

    // 3. Append migration chain slice into chain_pool_.
    const auto chain_begin = reg.chain_pool_.size();
    for (const auto& step : literal.migrations) {
        reg.chain_pool_.push_back(step);
    }
    literal.migrations = std::span<const MigrationEntry>{
        reg.chain_pool_.data() + chain_begin,
        literal.migrations.size()};

    // 4. Append entry. entries_ is kept sorted; literals arrive in
    //    canonical order so push_back preserves the sort.
    reg.entries_.push_back(literal);
    reg.by_fqn_index_.emplace(
        literal.schema.fqn,
        static_cast<std::uint32_t>(reg.entries_.size() - 1));
}

void commit_static_init(SchemaRegistry& reg) noexcept {
    reg.size_.store(static_cast<std::uint32_t>(reg.entries_.size()),
                    std::memory_order_release);
}

}  // namespace glibre::types::detail
```

The sequence:

1. **Reserve capacity once** at the start of static-init for the
   codegen-known descriptor count — `entries_`, `by_fqn_index_`,
   and `chain_pool_` are sized so no later `push_back` reallocates.
   Pointer-stability rule (§5 below).
2. **Per type** invoke `register_one(reg, codegen_literal)`. The
   FQN-collision check at step 2 uses path C of the §3.5 lookup.
   `chain_pool_` slicing (step 3) is the only address-fixup the
   registry performs after literal construction.
3. **Single commit** publishes `size_` (step 4 of `commit_static_init`).
   No reader observes a partial registry — `size_` is `0` until
   the very last `push_back` returns, and the release publish
   makes every prior write visible to every subsequent acquire
   load.
4. **No partial registration.** Static-init either commits all
   entries or aborts on the first conflict (step 2 fatal path).
   There is no rollback discipline at static-init; the build is
   the rollback boundary.

The §3.6 codegen biconditional makes the static-init path
collision-free in production: if two distinct middleman builds
*could* reach static-init in one process, the loader's ABI-hash
gate would have refused at least one of them earlier
(`reviews/decisions/plugin-abi.md` §"Loader Sequence" step 4).
The fatal-abort path exists exclusively for the misconfiguration
"two `.dylib`s named `glibre-types.dylib` got loaded into the same
process" — a defect the spine refuses to run with
(`specs/data/SPEC.md` §4.3 inv. 1).

### 3.8 The `SourceHashMismatch` arm (new)

The `specs/data/SPEC.md` §10 closed sum enumerates nine arms; this
design adds **one new closed-sum arm** the registry's hot-reload
drift check raises. The arm is appended only via this design
deliverable shipping in the same PR that introduces it (per
`specs/data/SPEC.md` §10 closed-enumeration discipline; §10.3
amendment-required note below).

```cpp
// Amendment to specs/data/SPEC.md §10.1 ErrorTag.
enum class ErrorTag : std::uint16_t {
    AbiHashMismatch          = 1,
    SchemaMigrationFailure   = 2,
    DeserializeError         = 3,
    ReservedTagViolation     = 4,
    SchemaRegistryConflict   = 5,
    SchemaUnknown            = 6,
    MigrationStepMissing     = 7,
    MigrationCycle           = 8,
    EnvelopeTruncated        = 9,
    SourceHashMismatch       = 10,   // (new) — same FQN, same version,
                                     //          differing SchemaSourceHash.
};

// Payload uses the existing fields; populates `step_schema` with the
// drifted FQN, `step_from = old version`, `step_to = old version`, and
// reuses `host_hash` / `plugin_hash` for the two hex source-hash strings.
```

The arm's discriminator: an FQN appears in both the outgoing and
the incoming registries, with the **same `SchemaVersion`**, but
with a **byte-different `SchemaSourceHash`**. By the §3.6 codegen
biconditional, byte-equal source bytes produce byte-equal hashes,
so a hash mismatch at hot-reload means the schema *bytes* drifted
(reordered tags, retypped fields, or any other change) without a
version bump — a strict ABI break that the loader translates into
`core::Error::PluginAbiHashMismatch`
(`specs/core/type-registry-design.md` §3.7 already raises this
arm; the data layer now types it identically). The new arm
distinguishes "global ABI hash mismatch" (arm 1) from "single FQN
drift" (arm 10) so operators reading logs see the offending type
without re-deriving it from the global hash diff.

§10 below records the arm's full row (trigger, recovery, severity,
mapping). §10.3 below records the SPEC §10.1 amendment that this
deliverable's PR carries.

## 4. Public surface

### 4.1 Public types (locked from `specs/data/SPEC.md` §5)

The public surface is exported by `glibre-types.dylib` and exposed
through the headers split listed in `specs/data/SPEC.md` §5
preamble:

```cpp
// include/glibre/types/registry.hpp — locked.
namespace glibre::types {

class SchemaRegistry {
public:
    [[nodiscard]] auto lookup(SchemaId schema) const noexcept
        -> const RegistryEntry*;

    [[nodiscard]] auto entries() const noexcept
        -> std::span<const RegistryEntry>;

    [[nodiscard]] static auto instance() noexcept
        -> const SchemaRegistry&;

private:
    SchemaRegistry() = default;
};

}  // namespace glibre::types
```

The class declaration is exactly what `specs/data/SPEC.md` §5
registry.hpp publishes; this design specifies its semantics, its
storage, and its concurrency model without widening the surface.

### 4.2 `SchemaRegistry` operations

| Operation               | Signature                                              | Cost            | Discipline                                |
|-------------------------|--------------------------------------------------------|-----------------|-------------------------------------------|
| `lookup`                | `(SchemaId) -> const RegistryEntry*`                   | `O(log N)`      | Lock-free; acquire-load on `size_`.       |
| `entries`               | `() -> std::span<const RegistryEntry>`                 | `O(1)`          | Returned span lives until the next Mode-B swap. |
| `instance`              | `() -> const SchemaRegistry&`                          | `O(1)` static   | The single live registry; pointer is stable for the live middleman build. |

`std::expected` is *not* applied to these calls. `lookup` returns
`nullptr` on miss (the call is structurally `Result<-shaped` only at
the envelope-serdes layer above; the registry itself is a borrowed
pointer source). `entries` and `instance` cannot fail — they expose
existing storage.

The codegen-emitted serdes layer wraps `lookup`'s `nullptr` into
`std::unexpected(data::Error{ .tag = ErrorTag::SchemaUnknown,
.at = WireSite{...} })` (`specs/data/SPEC.md` §10.2 row
`SchemaUnknown`); the registry surface itself stays
allocation-free and exception-free.

Per `reviews/decisions/error-model.md` §"Decision" #3 every public
boundary in the data context is `noexcept`. Per §"Decision" #2 no
exceptions cross the ABI boundary: `lookup` returns a pointer,
`entries` / `instance` return references — none can throw. The
schema-registry public surface inherits the engine-wide rule by
construction.

### 4.3 Loader-internal seam

The middleman's static-init path (§3.7) reaches a private
internal-linkage helper for insertion. That helper is invisible
outside the dylib and not part of the public ABI:

```cpp
namespace glibre::types::detail {
    void register_one(SchemaRegistry&, RegistryEntry literal) noexcept;
    void commit_static_init(SchemaRegistry&) noexcept;
}  // namespace glibre::types::detail
```

`register_one` and `commit_static_init` are **never** exported as
`extern "C"` symbols; they are TU-local within
`glibre-types.dylib`'s codegen-emitted `_registry.cpp`. The
plugin-abi seam is composed of `glibre_types_abi_hash`,
`glibre_types_register_migration`, and the per-FQN
`glibre_types_serialize_<fqn>` / `glibre_types_deserialize_<fqn>`
trampolines (`reviews/decisions/fory-codegen.md` §"middleman dylib
exposes"; `specs/data/SPEC.md` §4.3 inv. 4). The registry's
mutators are not on that list.

#### 4.3-bis Batch-mutation API (Mode-A hot-reload seam)

The hot-reload barrier (§6.3) uses a narrow batch-mutation seam on
top of the static-init helpers above. This seam is also internal-linkage
only and never exported as `extern "C"`:

```cpp
namespace glibre::types::detail {

// Opaque token identifying one in-flight Mode-A append batch.
// Allocated by begin_batch; consumed by commit_batch or rollback_batch.
// Non-copyable; move-only. Lives on the loader thread's stack.
struct BatchToken {
    eastl::uint32_t id{};  // monotonically-increasing batch serial
    eastl::uint32_t start_size{};  // entries_.size() at begin_batch
};

// Begin a new Mode-A append batch. Must be called from the barrier's
// loader-thread, at phase-8 entry only (§6.1 inv. 1). Returns the
// token the caller must pass to commit_batch or rollback_batch.
// Pre-condition: no batch currently open on this registry instance.
[[nodiscard]]
BatchToken begin_batch(SchemaRegistry&) noexcept;

// Commit an in-flight batch: atomically publishes all entries appended
// since begin_batch (i.e. entries_[token.start_size..size_)) by
// advancing the registry's published-size atomic with release semantics
// (§6.2 inv. 2). Invalidates the token.
// Returns: unexpected(core::Error::OutOfBudget) if the reserve was
// exhausted during the batch (the entries past the ceiling are
// rolled back before returning). On success returns void.
// noexcept: yes — the append path is pre-checked at begin_batch.
[[nodiscard]]
std::expected<void, core::Error> commit_batch(SchemaRegistry&, BatchToken&) noexcept;

// Roll back an in-flight batch: truncates entries_ back to
// token.start_size and tombstones any chain-pool slices added during
// the batch. Leaves the registry byte-identical to its state at
// begin_batch. Always noexcept; the rollback path must not fail.
void rollback_batch(SchemaRegistry&, BatchToken&) noexcept;

}  // namespace glibre::types::detail
```

**Contracts:**

- `begin_batch` / `commit_batch` / `rollback_batch` must be called on
  the same loader thread that holds the barrier's exclusive lock (§6.1
  inv. 1). No inter-thread transfer of `BatchToken` is permitted.
- `size_` (the publicly-visible entry count, acquired by readers via
  acquire semantics, §6.2) is **not advanced** until `commit_batch`
  returns successfully. Readers racing the append see the pre-batch
  snapshot; the release fence in `commit_batch` ensures they see the
  full committed set after the fence.
- If `rollback_batch` is called without a preceding `begin_batch`, the
  behaviour is undefined (debug builds assert).
- After `commit_batch` or `rollback_batch`, the `BatchToken` is
  invalidated; reuse is undefined.
- `core::Error::OutOfBudget` from `commit_batch` implies the entries
  appended during the batch that exceeded the reserve ceiling have been
  rolled back; the caller must treat the batch as failed and call
  `rollback_batch` to complete the cleanup of any partial
  chain-pool state.

### 4.4 No reflective surface

The registry exposes **no** reflective API: no
`get_field_descriptor(fqn, field_name)`, no
`construct_default(fqn) -> void*`, no
`set_field_by_path(value*, "a.b.c", new_value)`. PHILOSOPHY §6
forbids runtime reflection in shipping; the
editor / tools build observes the same surface and reads the
`ReflectionBlob` slot directly (the slot is `const ReflectionBlob*`,
not a member function call) — see `specs/data/SPEC.md` §4.9. The
registry has no reflective body of its own.

The `entries()` iteration in editor builds is the closest thing to
"reflection" the registry exposes; it is a passive walk over the
catalog, not a dispatch surface. Editor code that wants per-field
descriptors reaches into `RegistryEntry::reflection` (which is
codegen-emitted descriptor bytes, not a runtime introspection
facility — `specs/data/SPEC.md` §4.9 inv. 1).

## 5. Hot / cold path split

The registry's interaction with every other engine subsystem is
**cold-path** by design:

| Path  | Caller                                          | Trigger                                | Frequency                | Budget                                |
|-------|-------------------------------------------------|----------------------------------------|--------------------------|---------------------------------------|
| Cold  | `glibre-foryc` codegen emits literals           | Build time                             | Once per build           | n/a (build cost)                      |
| Cold  | `_registry.cpp` static-init                     | `glibre-types.dylib` load              | Once per process         | <1 ms (load 100s of descriptors)      |
| Cold  | `_abi_hash.cpp` ABI-hash composition            | Build time                             | Once per build           | n/a (build cost)                      |
| Cold  | `lookup` in codegen-emitted serdes thunk static-init | First serialize/deserialize per `T` per session | Once per type per session | <1 µs per call                |
| Cold  | `lookup` in hot-reload barrier                  | Phase 8 schema-set continuity check    | Per surviving FQN per reload | <1 µs per call                  |
| Cold  | `lookup` in `core::TypeRegistry` admission      | `PluginLoader::load` step 9            | Per `ComponentDecl` per load | <1 µs per call                  |
| Cold  | `entries` / Path B walk                         | Editor `SchemaBrowserPanel` repaint    | Per `SchemaRegistryChange` event | `O(N)` per event              |
| Hot   | (none — the registry is not on the every-frame path) | n/a                              | n/a                      | n/a                                   |

The fast-path in the engine **does not consult the registry every
frame**. The codegen-emitted `Envelope<T>` thunks
(`specs/data/SPEC.md` §4.3 inv. 4 + §4.8 inv. 5) capture the
`RegistryEntry*` pointer at first use (resolved against
`SchemaRegistry::instance().lookup(fqn_for<T>())`) and bake the
resolved pointer into a function-local static. Per-frame
`Envelope<T>::serialize` / `deserialize` calls dereference the
cached pointer directly; the registry is never re-traversed
per-call.

The pattern (**lookup at load + barrier; runtime serdes resolves
through cached descriptor pointer**) is what makes the §9 per-frame
cell **zero**:

```cpp
// glibre-types.dylib codegen-emitted Envelope<T> thunk body —
// sketched form, not part of the public surface.
namespace glibre::types::detail {

template <class T>
auto resolve_entry_for() noexcept -> const RegistryEntry* {
    static const RegistryEntry* cached =
        SchemaRegistry::instance().lookup(SchemaId{fqn_for<T>()});
    return cached;
}

}  // namespace glibre::types::detail
```

**Pointer-stability rules** (the seam that makes the cache safe):

1. **`entries_` is never relocated after the first commit.** The
   registry reserves capacity for the codegen-emitted descriptor
   count plus the §3.7 reserve at `_registry.cpp` static-init;
   subsequent appends in Mode-A reload (§8 below) consume from the
   reserve without reallocation. Exhausting the reserve refuses
   with `core::Error::OutOfBudget` (a resource-allocation failure
   in the core error enum, not a schema-semantic failure and therefore
   not a `data::ErrorTag` arm — per `reviews/decisions/error-model.md`
   §"Type Sketch" and §"Composition Rules" #2).
2. **`RegistryEntry*` handed out by `lookup` remains valid for the
   live registry instance's lifetime.** Cached pointers in
   compiled serdes thunks survive across frame boundaries and
   plugin loads (Mode A). Mode-B reload (§8.3 of the SPEC) is the
   only event that invalidates the pointer; subscribers
   re-resolve through `instance().lookup(fqn)` after the
   `SchemaRegistryChange::RegistryReplaced` notification.
3. **`MigrationEntry*` slices held by `RegistryEntry::migrations`
   remain valid for the live registry instance's lifetime.** The
   `chain_pool_` reserve covers every type's chain at static-init;
   no slice is rebound mid-instance. Mode-A reload does not
   relocate the pool (it appends to it), so existing slices stay
   address-stable.

The split makes the registry's per-frame budget cell (§9) **zero**
on the steady-state hot path, and bounded by `O(M log N)` on the
cold-path admission and barrier paths (M = entries the candidate
contributes; N = registry size).

## 6. Concurrency

The registry follows the same single-thread-mutation discipline
that governs `core::TypeRegistry`
(`specs/core/type-registry-design.md` §6) and
`reviews/decisions/hot-reload-protocol.md` §"Concurrency": **writes
only at frame phase boundaries; reads acquire-release atomic; no
fine-grained locks**.

### 6.1 Threading rules

1. **Mutations are loader-thread only and only at frame phase
   boundaries.**
   - Static-init: one-shot, single-threaded, before any frame
     ticks (§3.7).
   - Mode-A hot reload: registrations append at phase 8 only,
     under the barrier's exclusive lock
     (`reviews/decisions/hot-reload-protocol.md` §"Step 4 —
     Resume"). Calls from any other thread or any other phase
     refuse with `core::Error::FramePhaseMisordered` (debug-build
     assertion; shipping builds rely on the loader's API surface
     being the only mutation entry).
   - Mode-B reload (deferred per `specs/data/SPEC.md` §8.3): the
     loader publishes a *new* `SchemaRegistry` instance pointer
     atomically alongside the vtable swap. The previous instance
     is dropped after every phase-8-collected plugin completes
     resume. There is no in-place mutation across Mode B.
2. **Reads are lock-free, every thread.** `lookup`, `entries`, and
   `instance` are safe from any thread at any time. Single-writer
   multi-reader: readers loading `size_` with
   `memory_order_acquire` see the last committed batch's data;
   the writer publishes with `memory_order_release` at static-init
   commit and at Mode-A `commit_batch`.
3. **Phase boundaries are the only sync points.** Within a Mode-A
   batch, readers that observe `size_` for some intermediate
   value see only fully-constructed entries: appends update entry
   bytes before the size publish (release-acquire fence pair), so
   a reader cannot tear-read a half-initialized `RegistryEntry`.

### 6.2 Memory ordering

```cpp
// Write side (loader thread, inside batch / static-init):
entries_[next_idx] = built_entry;                       // 1. Plain store.
size_.store(next_idx + 1, std::memory_order_release);   // 2. Publish.

// Read side (any thread):
const auto sz = size_.load(std::memory_order_acquire);  // 3. Sync.
if (idx >= sz) return nullptr;                          // miss → SchemaUnknown.
return &entries_[idx];                                  // 4. Plain load, fenced.
```

The publish-subscribe pattern is correct because:

- The writer initializes `entries_[idx]` (and the chain-pool slice
  it borrows) before bumping `size_`, and the release-acquire
  fence pair makes the row's bytes visible to every reader that
  observes the new size.
- The reader loads `size_` with acquire; any subsequent load
  against `entries_[i]` for `i < sz` is fenced behind the size
  load.
- `entries_` itself is never relocated (§5 pointer-stability rule
  1), so the storage pointer captured by readers remains valid
  across writes.

Mode-B reload's atomicity is not satisfied by `size_` alone — the
**registry instance** changes (`instance()` returns a different
reference). The loader publishes the new instance through a
`std::atomic<const SchemaRegistry*>` indirection inside
`glibre-types.dylib`'s static storage; subscribers that hold a
cached `RegistryEntry*` through Mode-B observe a stale pointer,
which is why Mode-B fires the `SchemaRegistryChange::RegistryReplaced`
notification (`specs/data/SPEC.md` §8.5) — the consumer's
discipline is to re-resolve. Mode-B is MVP-deferred; the indirection
is sketched here for completeness.

### 6.3 Interaction with the hot-reload barrier

Per `specs/data/SPEC.md` §8.2 and `reviews/decisions/hot-reload-protocol.md`
§"Step 3 — Migrate" / §"Step 4 — Resume":

1. **Phase-8 entry.** The barrier acquires the registry's
   loader-thread exclusive lock (the same single-thread invariant
   that gates `core::TypeRegistry` per §6 of its design).
2. **Schema-set continuity check (`migrate(...)` step 1).**
   For every FQN registered in the outgoing snapshot, the
   barrier calls `incoming.lookup(fqn)`. A nullptr return →
   `data::Error::SchemaMigrationFailure` (`specs/data/SPEC.md`
   §8.2 step 1) — refuse the swap.
3. **Per-FQN version-chain query.** For every FQN whose stored
   version differs from `incoming.lookup(fqn)->version`, the
   barrier dispatches the migration chain via
   `MigrationDispatcher` (#738) — the dispatcher reads the
   chain through `RegistryEntry::migrations`. The registry
   itself never invokes a migration body.
4. **Mode-A append.** Newly-introduced FQNs from the candidate
   plugin land via the same `register_one` path as static-init,
   except the `commit_batch` call publishes only the appended
   range. The append is bracketed by `begin_batch(p)` /
   `commit_batch(p)` / `rollback_batch(p)` — the same discipline
   as `core::TypeRegistry` §3.4, applied to the schema registry.
5. **Mode-B replacement.** `instance()` is rebound atomically;
   subscribers re-resolve via `SchemaRegistryChange::RegistryReplaced`.

The barrier-registry seam is intentionally narrow: `lookup`,
`entries`, and (in Mode A only) the loader-internal
`begin_batch` / `commit_batch` / `rollback_batch` triplet. The
registry exposes nothing else to the barrier; the barrier exposes
nothing else of itself to the registry.

## 7. Persistence + ABI

### 7.1 Descriptors are codegen-emitted

The registry's contents are **emitted by `glibre-foryc` at build
time** (§3.6) and persisted exclusively as **C++ source baked
into `glibre-types.dylib`**. There is no on-disk wire form for the
registry itself — the only persistent inputs are the
`data/schemas/<ctx>/<Type>.fory` source files
(`specs/data/SPEC.md` §7.1 file format) and the recomputed
`glibre_types_abi_hash`. Those plus the middleman binary are the
sole sources of truth; the registry is a runtime projection.

This is intentional. A serialized registry (a "catalog file" the
process reads at startup) would introduce:

- a separate persistent format with its own canonicalization rule,
- a separate version axis (catalog format vs schema format),
- a separate hash gate (catalog hash vs schema-source hash),
- a separate failure mode (catalog-file-missing vs FQN-missing).

The collapse — **emit the registry as `_registry.cpp`; recompute
`glibre_types_abi_hash` from the same source** — yields one
artifact, one hash, one gate.

### 7.2 ABI hash composition

§3.4 above is the load-bearing recipe. Cross-referencing the
`reviews/decisions/plugin-abi.md` §"ABI Hash Function" and
`reviews/decisions/fory-codegen.md` §"middleman dylib exposes" #3
inputs:

1. Per-entry contribution: `fqn || ":" || version_le || ":" ||
   source_hash`.
2. Concatenation order: ascending Unicode code-point order over
   `fqn`.
3. Separator: single `0x0A` byte.
4. Final hash: `blake3(input)`, lowercase-hex 64 chars.

Inputs **not** in the hash are listed in §3.4 above. The exclusion
list is identical to `core::TypeRegistry`'s (`specs/core/type-registry-design.md`
§7.2) by construction — both registries derive from the same
`.fory` source set, and a divergence in the exclusion lists would
break the cross-registry consistency that
`specs/core/type-registry-design.md` §3.7 relies on.

### 7.3 Per-FQN ABI provenance

The per-entry `(FQN, SchemaVersion, SchemaSourceHash)` triple is
the per-FQN ABI provenance the registry exposes. It answers three
distinct questions:

| Question                                    | Field                          | Mismatch refusal arm                       |
|---------------------------------------------|--------------------------------|--------------------------------------------|
| "Does this plugin link the right middleman?" | `glibre_types_abi_hash`        | `core::Error::PluginAbiHashMismatch`       |
| "Is the layout of this FQN the same?"       | `SchemaSourceHash`             | `data::Error::SourceHashMismatch` (§3.8) → wraps to `core::Error::PluginAbiHashMismatch` |
| "Is the version the same or newer?"         | `SchemaVersion`                | `data::Error::SchemaMigrationFailure` (regression) / chain dispatch (forward) |

The triple-axis discipline matches the three independent version
axes (`reviews/decisions/plugin-abi.md` §"Versioning Rules"): the
global hash answers the first axis (middleman ABI), the per-FQN
source hash answers the second (layout drift on a single FQN),
the per-FQN version answers the third (semantic evolution).

The registry exposes the triple through `RegistryEntry`
(§3.2). `core::TypeRegistry` (`specs/core/type-registry-design.md`
§3.3) carries the same triple in *its* descriptor; the two
descriptors are populated from the same source-of-truth (the
`_registry.cpp` codegen blob). Drift between the two is impossible
by construction — they are different projections of the same
codegen output.

### 7.4 ABI stability rules (transcribed)

Inherited from `reviews/decisions/fory-codegen.md` §"ABI Stability
Rules" and `specs/data/SPEC.md` §4.10 cross-aggregate invariants:

1. The registry's record shape (`RegistryEntry`) is closed by this
   design and changes only under a layout-breaking middleman
   SONAME bump (`fory-codegen.md` §"ABI Stability" #5;
   `specs/data/SPEC.md` §4.3 inv. 3).
2. The `entries_` sort order is ascending by FQN; this is the
   canonical order that `glibre_types_abi_hash` consumes (§3.4).
   Author-order independence is what makes the hash a property of
   the contract, not the build environment.
3. Adding a new FQN is ABI-additive: `glibre_types_abi_hash`
   changes (a new entry contributes new bytes), the SONAME is
   *not* bumped, plugins built against the prior hash are
   refused at load (`reviews/decisions/plugin-abi.md`
   §"Versioning Rules" Axis 1). This is the standard
   "additive-but-hash-bumped" path.
4. Bumping a `SchemaVersion` for an existing FQN is ABI-additive
   in the same way: hash changes, SONAME unchanged, plugins
   rebuild against the new middleman.
5. A `SourceHashMismatch` for an existing `(FQN, SchemaVersion)`
   pair is **not** ABI-additive — it indicates a non-additive
   schema change without a version bump; the SONAME bumps and
   `core` refuses every prior-built plugin
   (`reviews/decisions/plugin-abi.md` §"Versioning Rules" Axis 2;
   `specs/core/type-registry-design.md` §7.4 #3).
6. Removing an FQN is **not permitted** in MVP (it would
   strand persistent state with no migration story; per
   `specs/data/SPEC.md` §8.2 step 1 the schema-set continuity
   check refuses the swap). Post-MVP, an FQN-drop story would
   require its own spike that defines the migration-to-deletion
   protocol.

## 8. Hot-reload integration

The registry is a participant in `core::HotReloadBarrier`'s
four-step state machine (`specs/data/SPEC.md` §8;
`reviews/decisions/hot-reload-protocol.md`):

| Step       | Owner   | Registry's role                                                                     |
|------------|---------|--------------------------------------------------------------------------------------|
| 1. Drain   | Barrier | None. Existing entries remain pointer-stable.                                        |
| 2. Swap    | Barrier | Provides `lookup(existing_fqn)` to validate `(version, source_hash)` parity (§3.8).  |
| 3. Migrate | Barrier | Provides `lookup` for chain dispatch; `MigrationDispatcher` (#738) walks chains.     |
| 4. Resume  | Barrier | Commits the candidate's batch (`commit_batch`); publishes new entries (Mode A).      |

### 8.1 Registry diff at the barrier

Before vtable mutation (step 2), the barrier computes a per-plugin
diff between the surviving registry state and the candidate's
schema set (the candidate's `manifest.components` plus any
schemas the plugin's `_registry.cpp` carries). The diff is
`O(M)` over the candidate's schema count and runs once per
hot-reload transaction:

| Diff class              | Old state                  | New state                                    | Action                                                                                          |
|-------------------------|----------------------------|----------------------------------------------|-------------------------------------------------------------------------------------------------|
| **Added FQN**           | Not in registry            | In candidate                                 | Mode-A append at step 2.4; commit at step 4. Pure-additive; no migration. Notify `EntriesAppended`. |
| **Unchanged FQN**       | `(version, hash) = (V, H)` | Same `(V, H)`                                | Idempotent re-resolve; no registry mutation. No migration. No notification.                     |
| **Bumped FQN**          | `(version, hash) = (V, H_v)`     | `(V', H_{v'})` with `V' > V` and chain `V → V'` covers every step | Mode-A: append the new chain steps to `chain_pool_`; bump the entry's `version` field at commit. Migrate surviving storage. Notify `VersionsBumped`. |
| **Source-hash drift**   | `(version, hash) = (V, H)` | `(V, H')` with `H' ≠ H`                      | Refuse: `data::Error::SourceHashMismatch` (§3.8) — wraps to `core::Error::PluginAbiHashMismatch`. |
| **Removed FQN**         | In registry, surviving rows | Absent from candidate                       | Refuse: `data::Error::SchemaMigrationFailure` (`specs/data/SPEC.md` §8.2 step 1) — wraps to `core::Error::SchemaMigrationFailed`. |
| **Version regression**  | `version = V`              | `(V', H_v')` with `V' < V`                   | Refuse: `data::Error::SchemaMigrationFailure` carrying the regression triple. (See §10 below.)  |
| **Reused version w/ different hash** | `(V, H)`         | `(V, H')` (different)                        | Same as "Source-hash drift" — `data::Error::SourceHashMismatch`.                                |

The diff classes correspond one-for-one to the §10 refusal arms
relevant to the registry plus the two pass-through-or-migrate
classes. They are the data-context counterpart of
`specs/core/type-registry-design.md` §8.1's diff table; the two
diffs are computed independently but the failure surface is
identical (a single `core::Error` arm wraps all of them).

### 8.2 Migration dispatch

The registry **does not** invoke migration functions itself.
Migrations live in the per-FQN `RegistryEntry::migrations` slice
(populated at codegen time by `_registry.cpp` and Mode-A-extended
at hot-reload). The barrier's `MigrationDispatcher` (#738) is the
caller; the registry's contribution is providing the chain span
through `lookup(fqn)->migrations`.

```cpp
// Inside MigrationDispatcher (#738), at HotReloadBarrier step 3.
const auto* entry = SchemaRegistry::instance().lookup(SchemaId{fqn});
if (entry == nullptr) {
    return std::unexpected(data::Error{ .tag = ErrorTag::SchemaMigrationFailure,
                                        .step_schema = SchemaId{fqn} });
}
for (auto v = stored_version; v < entry->version; ++v) {
    auto step = find_step(entry->migrations, v, v + 1);
    if (step == nullptr) {
        return std::unexpected(data::Error{ .tag = ErrorTag::MigrationStepMissing,
                                            .step_schema = entry->schema,
                                            .step_from = v,
                                            .step_to = v + 1 });
    }
    GLIBRE_TRY(step->invoke(...));
}
```

The chain runs out-of-band of the registry; the registry's role is
informational. After every storage row of every bumped type's
storage is migrated successfully, the barrier commits the batch
(which updates `entries_[i].version` to the new value); on any
chain failure, the barrier rolls back (which leaves entries at the
prior version).

The "lookup at load + barrier" rule (§5) extends to this path:
`MigrationDispatcher` calls `lookup` once per FQN per barrier run,
caches the `RegistryEntry*`, and dispatches per-row migrations
through the cached pointer.

### 8.3 What survives the swap (registry-side)

Per `specs/data/SPEC.md` §8.1:

- **Every entry admitted before the swap survives.** The registry
  is append-only as observed externally (Mode A); the swap may add
  new entries, may bump an existing entry's `SchemaVersion`, but
  never removes a slot or invalidates an FQN's lookup. (Mode B is
  the wholesale-replacement exception — but Mode B is MVP-deferred,
  and even in Mode B the new instance's entries are a superset of
  the old's by §8.3 of the SPEC.)
- **`SchemaId` views remain valid.** A `SchemaId` issued before
  the swap addresses the same FQN-string after the swap (Mode A).
  Mode B subscribers re-resolve through `lookup` after
  `RegistryReplaced`.
- **`RegistryEntry*` pointers handed out by `lookup` are stable.**
  Pointer-stability rule (§5) survives the swap because `entries_`
  is never relocated in Mode A. Mode B is the only event that
  invalidates the pointer.
- **`SchemaSourceHash` is stable for unchanged FQNs.** Drift on
  an existing FQN refuses the swap (§8.1 row "source-hash drift")
  so the survivors are all hash-equal across the swap.

Because the registry is append-only and pointer-stable in Mode A,
a hot-reload that adds new FQNs is a pure-additive `O(M)` append;
a hot-reload that bumps versions changes only the `SchemaVersion`
field of existing entries (a 32-bit store under the loader thread's
exclusive ownership of phase 8 — single relaxed atomic store, not
a re-allocation). The fast path is correspondingly cheap.

### 8.4 Refusal cases (cross-reference)

The §10 table below transcribes the registry-relevant refusals;
the authoritative table is `specs/data/SPEC.md` §10.2. The
registry-specific refusals:

- `SchemaUnknown` — `lookup` miss at the consumer's call site
  (translated by envelope serdes; the registry returns nullptr).
- `SchemaRegistryConflict` — duplicate FQN at static-init.
- `SchemaMigrationFailure` — schema-set continuity refusal at
  the barrier.
- `MigrationStepMissing` — chain coverage gap at the barrier.
- `SourceHashMismatch` (new, §3.8) — drift on an existing
  `(FQN, version)` pair.

Every refusal preserves the registry's prior state byte-for-byte:

- Static-init refusal (§3.7 fatal-abort) terminates the process
  before any frame ticks; there is no surviving state to corrupt.
- Mode-A refusal walks `rollback_batch` per the §6.3 sequence;
  appended entries and chain-pool slices that the failed batch
  added are discarded, `size_` is left at its pre-batch value
  (the publish never fires).
- Mode-B refusal leaves the live `instance()` pointer unchanged;
  the new instance is dropped wholesale.

## 9. Performance

The registry's contribution to per-frame and per-load budgets,
locked against `reviews/decisions/perf-budget.md`
§"Per-Context Budget Table" and `specs/data/SPEC.md` §9:

### 9.1 Per-frame (steady-state)

| Cell                        | Budget               | Source                                |
|-----------------------------|----------------------|---------------------------------------|
| CPU sim                     | 0 ms                 | not on the every-frame path           |
| CPU submit                  | 0 ms                 | data records no GPU work              |
| GPU                         | n/a                  | data owns no Metal work               |
| Phase ownership             | none                 | participates in phase 8 only          |

The registry contributes **nothing** to the steady-state per-frame
cell. Codegen-emitted serdes thunks cache the `RegistryEntry*`
once and never re-traverse (§5); the per-frame budget for the
data context (`specs/data/SPEC.md` §9.1 cell — 0.20 ms sim) is
absorbed by `Envelope<T>` payload work, not by registry lookups.

### 9.2 Lookup wall-time budget

| Operation                  | Budget                     | Source                                |
|----------------------------|----------------------------|---------------------------------------|
| `lookup(SchemaId)`         | ≤ 10 ns per call           | `specs/data/SPEC.md` §9.2 row `SchemaRegistry` |
| `entries()`                | ≤ 1 ns (pointer return)    | trivial accessor                      |
| `instance()`               | ≤ 1 ns                     | static reference                      |
| Mode-A `commit_batch`      | ≤ 50 µs per batch          | release fence + small-vector append   |
| Mode-A `rollback_batch`    | ≤ 50 µs per batch          | walk owner ledger + tombstone marks   |
| Static-init `register_one` | ≤ 1 µs per entry           | hash-map insert + vector push         |

The 10 ns / call ceiling for `lookup` is the SPEC's locked budget
(`specs/data/SPEC.md` §9.2). With ~1k entries (R-1.3.1's ≥10 000
type ceiling holds for `core::TypeRegistry`; the schema-registry's
budget is sized lower because not every component is persistent),
binary-search depth is ≤ 10 cache-line probes; modern Apple Silicon
hits the budget with comfortable headroom. The CI gate
(`schema_registry_lookup.bench.cpp`, `specs/data/SPEC.md` §9.4
benchmark #2) asserts the budget in a 100k-iteration window.

### 9.3 Per-load (cold path)

| Operation                  | Budget                     | Source                                |
|----------------------------|----------------------------|---------------------------------------|
| `_registry.cpp` static-init | ≤ 1 ms (100s of entries)  | one-shot at `glibre-types.dylib` load |
| Hot-reload Mode-A append   | ≤ 50 µs per added FQN      | bounded by `commit_batch`             |
| Hot-reload Mode-A version-bump | ≤ 1 µs per bumped FQN  | single relaxed store on `entries_[i].version` |
| Hot-reload Mode-A diff     | ≤ 100 µs per candidate     | walk candidate's schema set; per-FQN `lookup`  |
| Mode-B replacement         | ≤ 5 ms (deferred, MVP+)    | wholesale instance allocation + atomic swap   |

The hot-reload Mode-A budget is dominated by `commit_batch`'s
release fence and the small-vector appends; the per-row
migration cost is owned by `MigrationDispatcher` (#738) and not
counted here.

### 9.4 Heap accounting (per-context cell)

| Bucket                         | Sub-ceiling | Source                            |
|--------------------------------|-------------|-----------------------------------|
| `entries_` (registry table)    | 6 MiB       | up to ~64k 96-byte entries (well above MVP target of ~1k) |
| `by_fqn_index_` (hash map)     | 1 MiB       | bucket + key view storage          |
| `chain_pool_`                  | 0.5 MiB     | total `MigrationEntry` slice storage across all FQNs    |
| `fqn_storage_` (FQN intern)    | 0.5 MiB     | sum of FQN string bytes            |

Sub-ceilings sum to **8 MiB**, which matches the
`SchemaRegistry` row in `specs/data/SPEC.md` §9.2. The data
context's overall 32 MiB ceiling absorbs this row plus the
`Envelope` 16 MiB scratch, the `MigrationDispatcher` 4 MiB chain
arena, and the editor-only `ReflectionBlob` 4 MiB. Allocation is
tagged `ContextTag::data` and validated by the
`GLIBRE_ALLOC_STRICT=1` gate
(`reviews/decisions/perf-budget.md` §"Allocator Rules" #4;
`specs/data/SPEC.md` §9.4 benchmark #4).

The per-context heap cell is a **static-init allocation** — live
for the entire process lifetime, freed at process exit. It does
not pass through the per-frame transient arena; allocations
leaking from the registry into the per-frame arena would be an
`OutOfBudget` "leak" arm
(`reviews/decisions/perf-budget.md` §"Allocator Rules" #2;
`specs/data/SPEC.md` §9.5 #2).

### 9.5 Performance contract per harmonius R-1.3.1

The harmonius reflection-and-type-system requirement R-1.3.1
calls for "≥10 000 types, O(1) lookup, lock-free concurrent reads".
The schema-registry partially answers this requirement (the
type-registry's R-1.3.1 row in `specs/core/type-registry-design.md`
§9.4 is the dominant answer for ECS components):

- **≥10 000 types** — the `entries_` reserve at 6 MiB / 96 bytes
  per entry yields ~64k entry capacity; the budget covers the
  R-1.3.1 ceiling 6×.
- **O(1) lookup** — the schema-registry's `lookup` is `O(log N)`
  binary search rather than `O(1)`. The 10 ns wall-time budget
  (§9.2) is the harmonius "<500 ns" path-access budget tightened
  by ~50× — the binary-search constant is small enough that the
  log factor is hidden inside the wall-time budget. Strict
  `O(1)` would require a hash-map-keyed registry; the SPEC
  chose binary search for cache-friendliness and allocation-free
  structure (`specs/data/SPEC.md` §4.5 inv. 3).
- **Lock-free concurrent reads** — covered by the
  acquire-load-on-`size_` pattern (§6.2); appends within a Mode-A
  batch are loader-thread-only, so multi-reader is bottlenecked
  only by the `size_` atomic.

The differential-with-`core::TypeRegistry` is recorded in §1: the
two registries are different abstractions answering different
slices of R-1.3.1, with `core::TypeRegistry` providing the dense
`u64` index path for ECS storage and `data::SchemaRegistry`
providing the FQN path for serdes.

## 10. Failure modes

The registry surfaces failure exclusively through
`glibre::types::data::Error` (`specs/data/SPEC.md` §10). Per
`reviews/decisions/error-model.md` §"Decision" #3 every public
boundary is `noexcept` and returns `std::expected<T, Error>` (or
nullable pointer at the registry layer; the envelope-serdes layer
above lifts to `std::expected`).

### 10.1 Registry-emitted `data::Error` arms

The registry contributes the following arms to the §10.1 closed
sum (with §3.8 amendment):

| Arm                          | Trigger                                                                                                                  | Detection point                                                | Payload                                  | Recovery                            | Severity              | core::Error mapping                  |
|------------------------------|--------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------|------------------------------------------|-------------------------------------|-----------------------|---------------------------------------|
| `SchemaUnknown`              | `lookup(fqn)` returns nullptr at envelope serdes; FQN not in live registry.                                              | Cached resolution at thunk static-init; barrier diff path.     | `at.schema = fqn`, `at.offset = 0`       | refuse decode (envelope path); refuse load (barrier). | error / warn          | None — passed through `glibre::Error::Variant` (deserialize); wraps `core::Error::SchemaMigrationFailed` (barrier removed-FQN). |
| `SchemaRegistryConflict`     | Duplicate FQN at static-init insertion sequence step 2 (§3.7).                                                           | `register_one` collision check.                                | `step_schema = duplicated_fqn`           | process abort — `std::abort()`.     | fatal                 | None — fatal at static-init never reaches the loader. |
| `SchemaMigrationFailure`     | Schema-set continuity check refuses (§8.1 row "removed FQN" / "version regression").                                     | `migrate(...)` step 1 (§8.2 of SPEC).                          | `step_schema = removed_or_regressed_fqn` | refuse load — Mode A: rollback batch; Mode B: drop new instance. | warn                  | `core::Error::SchemaMigrationFailed`. |
| `MigrationStepMissing`       | A `MigrationChain` for a known FQN lacks an entry whose `from_version` matches the inbound version.                       | `MigrationDispatcher::dispatch` (§8.2 above).                  | `step_schema`, `step_from`, `step_to`    | refuse decode / refuse load.        | error / warn          | `core::Error::SchemaMigrationFailed` (collapses with `SchemaMigrationFailure`). |
| `SourceHashMismatch` *(new)* | An existing `(FQN, version)` pair appears in the candidate with a byte-different `SchemaSourceHash` (§8.1 row "source-hash drift"). | Mode-A diff at barrier step 2 (§8.1).                | `step_schema`, `host_hash` (live), `plugin_hash` (incoming). | refuse load — log at `warn`, leave previous-good plugin live. | warn  | `core::Error::PluginAbiHashMismatch`. |

The `SourceHashMismatch` arm is the §3.8 amendment. Operators
reading logs benefit from the distinction "global ABI hash
mismatch" (arm 1) vs "single FQN drift" (arm 10), because the
remediation differs (rebuild the offending plugin against the new
middleman vs version-bump the schema and write a migration). Both
wrap into `core::Error::PluginAbiHashMismatch` because the loader's
response is identical: refuse load, log warn, leave previous-good
plugin live.

### 10.2 What the registry does NOT raise

For clarity, the §10 closed sum has arms the registry does **not**
detect:

- `AbiHashMismatch` (arm 1) — the `core` plugin loader compares
  hashes (`reviews/decisions/plugin-abi.md` §"Loader Sequence"
  step 4); the registry contributes the per-entry inputs to the
  hash but does not perform the comparison.
- `DeserializeError` (arm 3) — `Envelope<T>::deserialize`
  detects malformed payload bodies; the registry exposes the
  thunk pointer, not the decoder logic.
- `ReservedTagViolation` (arm 4) — codegen-time only; emitted by
  `glibre-foryc`, never reaches the registry.
- `MigrationCycle` (arm 8) — codegen-time and static-init-time;
  the static-init detector lives inside the `_registry.cpp` chain
  composition, not inside the registry's `register_one` body.
- `EnvelopeTruncated` (arm 9) — `Envelope<T>::deserialize`
  envelope-read step.

These arms surface elsewhere in the data context; the registry's
public surface stays narrow.

### 10.3 SPEC §10.1 amendment (addressed in follow-up)

Adopting `SourceHashMismatch = 10` required an in-place amendment
to `specs/data/SPEC.md` §10.1's `ErrorTag` enumerator and §10.2's
per-arm table. The amendment was not included in the PR that
originally landed this design (PR #833) — SPEC §10's
closed-enumeration discipline mandates co-shipping, and the split
created a 9-arm SPEC enum vs. a 10-arm design. This was identified
in the round-1 review (finding HIGH-1) and addressed by adding:

- `SourceHashMismatch = 10` to the `ErrorTag` enum (§10.1).
- The full trigger/recovery/severity/mapping row to §10.2.
- The `core::Error::PluginAbiHashMismatch` wrapping row to §10.3.
- Arm 10 to the loader-handler bullet in §10.4.
- The `SourceHashMismatch` fixture row to §10.5.

The §3.8 sketch above remains the authoritative per-arm specification.
SPEC §10.1–§10.5 now reflects it in full.

## 11. Test plan

Every arm in §10.1 carries at least one Catch2 case. The registry's
test surface is split into unit tests (registry mechanics in
isolation) and integration tests (registry under the loader and
barrier, with codegen-emitted middleman fixtures).

### 11.1 Unit tests — `SchemaRegistry`

Location: `tests/data/registry/schema_registry_test.cpp`.

| Case                                       | Asserts                                                                       |
|--------------------------------------------|-------------------------------------------------------------------------------|
| `lookup_hit`                               | `lookup(fqn)` for a registered FQN returns non-null; pointer dereferences to expected `(version, source_hash)`. |
| `lookup_miss`                              | `lookup(fqn)` for an unregistered FQN returns nullptr.                        |
| `lookup_log_n`                             | `BENCHMARK` over 1k entries asserts ≤ 10 ns per call (§9.2).                 |
| `entries_canonical_order`                  | `entries()` returns FQN-sorted-ascending span; `is_sorted` over the FQN field. |
| `instance_singleton`                       | Two calls to `instance()` return references to the same object.               |
| `static_init_idempotent_views`             | After static-init, `SchemaId.fqn` views compare byte-equal to the FQN strings the literals declared. |

### 11.2 Unit tests — version chain

Location: `tests/data/registry/version_chain_test.cpp`.

| Case                                       | Asserts                                                                       |
|--------------------------------------------|-------------------------------------------------------------------------------|
| `monotonic_chain_present`                  | For an FQN at version 3, `entry->migrations` contains exactly steps `(1→2)` and `(2→3)`, in order. |
| `chain_coverage_gap_codegen_refuses`       | A test-only `glibre-foryc` invocation against a synthetic chain with a missing step exits non-zero with `MigrationStepMissing` diagnostic. |
| `chain_cycle_codegen_refuses`              | A test-only `glibre-foryc` invocation against `[(1→2),(2→1)]` exits non-zero with `MigrationCycle` diagnostic. |
| `chain_pool_slice_stable`                  | After Mode-A append of a new chain, existing `entry->migrations` spans for unrelated FQNs compare pointer-equal to pre-append.   |

### 11.3 Unit tests — source-hash byte-equality

Location: `tests/data/registry/source_hash_test.cpp`.

| Case                                       | Asserts                                                                       |
|--------------------------------------------|-------------------------------------------------------------------------------|
| `byte_equal_source_byte_equal_hash`        | Two `glibre-foryc` invocations against byte-equal `.fory` source produce byte-equal `SchemaSourceHash`.        |
| `byte_different_source_byte_different_hash`| Two `glibre-foryc` invocations against byte-different `.fory` source produce byte-different hashes, even when the canonicalized form differs by whitespace alone (canonicalization rule §7.3 reproducibility). |
| `hash_byte_equality_is_layout_oracle`      | A test-only middleman with two registry literals carrying the same FQN and the same version but byte-different source hashes triggers `SchemaRegistryConflict` at static-init (the conflict arm enforces the byte-equality oracle). |
| `abi_hash_recomputable_from_entries`       | `glibre_types_abi_hash()` compares byte-equal to a recomputation derived from `SchemaRegistry::instance().entries()` via the §3.4 recipe. |

### 11.4 Unit tests — failure arms

Location: `tests/data/errors/schema_registry_arms_test.cpp`. One
case per arm in §10.1 (matching the SPEC §10.5 obligation table):

| Arm                          | Fixture                                                                                                                        |
|------------------------------|--------------------------------------------------------------------------------------------------------------------------------|
| `SchemaUnknown`              | An envelope whose FQN is `glibre.test.NeverRegistered`; assertion on `at.schema == "glibre.test.NeverRegistered"`.            |
| `SchemaRegistryConflict`     | A test-only `_registry.cpp` that invokes `register_one` twice with the same FQN; assertion on `step_schema` and on the `std::abort` path via a death-test. |
| `SchemaMigrationFailure`     | The barrier's schema-set continuity test against an outgoing-only FQN; `step_schema` carries the dropped FQN.                  |
| `MigrationStepMissing`       | A registry whose chain for an FQN at version 4 starts at step `(2→3)`; deserialize a `v1` payload and assert `step_from == 1`. |
| `SourceHashMismatch` *(new)* | A barrier diff against a candidate carrying the same `(FQN, version)` with a byte-different `SchemaSourceHash`; `host_hash` and `plugin_hash` populate the payload. |

### 11.5 Integration tests

Location: `tests/data/registry/integration/`.

1. **`plugin_registration_sweep_test.cpp`** — load a synthetic
   middleman build with N=200 schemas, verify
   `SchemaRegistry::instance().entries().size() == 200`,
   verify `lookup` succeeds for every FQN, verify
   `glibre_types_abi_hash()` is byte-equal to the recomputation
   from `entries()`. Covers static-init, path A, path B, and
   ABI-hash composition end-to-end.
2. **`hot_reload_diff_test.cpp`** — drives the loader's barrier
   with a candidate plugin contributing (a) one added FQN, (b)
   one bumped FQN with a complete chain, (c) one unchanged FQN.
   Asserts `SchemaRegistryChange::Kind::EntriesAppended` fires
   with the added FQN, then `Kind::VersionsBumped` fires with the
   bumped FQN, and that the integration test's checked-in goldens
   for each arm match the expected payload.
3. **`hot_reload_drift_refuses_test.cpp`** — same harness as #2,
   but the candidate carries `(FQN, version)` with a drifted
   `SchemaSourceHash`. Asserts the loader returns
   `core::Error::PluginAbiHashMismatch`, the live registry's
   `instance()` is pointer-identical to its pre-swap value, and
   `lookup(drifted_fqn)->source_hash` is byte-equal to the
   pre-swap value.
4. **`hot_reload_chain_gap_refuses_test.cpp`** — candidate with
   a bumped FQN whose chain is missing a step. Asserts
   `core::Error::SchemaMigrationFailed`, world rolled back, no
   row partially migrated.

### 11.6 Coverage matrix

| Section | Test                                                            |
|---------|-----------------------------------------------------------------|
| §3.2    | `lookup_hit`, `static_init_idempotent_views`                    |
| §3.4    | `abi_hash_recomputable_from_entries`                            |
| §3.5    | `lookup_hit`, `lookup_miss`, `entries_canonical_order`          |
| §3.6    | `chain_coverage_gap_codegen_refuses`, `chain_cycle_codegen_refuses` |
| §3.7    | `SchemaRegistryConflict` death-test                             |
| §3.8    | `SourceHashMismatch` arm test; integration test #3              |
| §5      | `lookup_log_n` benchmark                                        |
| §6      | `chain_pool_slice_stable` (pointer-stability under Mode-A)      |
| §8.1    | integration tests #2, #3, #4                                    |
| §9.2    | `lookup_log_n` benchmark; SPEC §9.4 benchmark #2                |
| §10.1   | `tests/data/errors/schema_registry_arms_test.cpp` (one case per arm) |

## 12. Open questions

- `[OPEN]` — Whether `EnvelopeHeader` (`specs/data/SPEC.md` §4.8)
  should grow an optional dense-integer alias for `SchemaId` to
  reduce per-payload header size on small wire records (e.g.
  per-component snapshot stream). Resolution gate: a perf
  follow-up spike under `data` post-MVP that quantifies the wire
  overhead saved against the registry-side complexity of
  maintaining a stable integer-to-FQN mapping across plugin loads.
- `[OPEN]` — Whether Mode-B middleman self-reload (`specs/data/SPEC.md`
  §8.3) should publish a `SchemaRegistry::instance()`
  indirection (`std::atomic<const SchemaRegistry*>`) at MVP, even
  though the reload itself is deferred. The benefit is making the
  pointer-stability rule (§5) explicit at the type system layer
  rather than as a documentation convention. Resolution gate: the
  Mode-B spike that turns the gate on (post-MVP) decides.
- `[OPEN]` — Whether the registry should expose a `versions(fqn)
  -> std::span<const SchemaVersion>` accessor that walks the
  per-FQN historical version chain (the chain that preceded
  `current`). Editor / tools could use it to render version
  history; the SPEC §4.7 chain currently exposes only steps
  (`MigrationEntry`), not the bare version sequence. Resolution
  gate: the editor's `SchemaBrowserPanel` story
  (`specs/data/SPEC.md` §11 acceptance #373) decides whether the
  accessor is needed.
- `[OPEN]` — Whether a hard cap on `entries_` size (e.g. 16k
  entries, sized inside the 6 MiB sub-ceiling at §9.4) should
  refuse static-init with `OutOfBudget` rather than silently
  consuming the data context's heap headroom. Resolution gate:
  CI gate authoring under `data/runtime/test/perf/` (the §9.4
  benchmark #4 owner). MVP target is well under 1k entries; the
  cap matters only if a future content-driven schema explosion
  triggers the ceiling.
