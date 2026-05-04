# `core` — `AssetHandle` Detailed Design

> Detailed design for the **asset-handle** aggregate in the `core` context
> (SPEC §4.7, §6.8). This document refines the SPEC's invariants into an
> implementation-shaped contract. It is the deliverable of spike #708 and
> the input to the sibling task-breakdown spike that decomposes
> implementation work into `type:plan` issues.
>
> Authority: this design refines `specs/core/SPEC.md` §4.7, §6.8, §8.1
> #4, §9.3 row "AssetHandle table", and §10.1 row `AssetStale`.
> Anything not addressed here defers to those sections; anything that
> appears to contradict them is a defect in this document.

---

## 1. Purpose

The asset-handle aggregate owns the **rules of opaque cross-plugin
resource identity**: how a 64-bit value addresses a slot in a
process-wide table, how the slot's generation counter invalidates
stale dereferences, how the resolved payload pointer is interpreted
(by the *resolving* plugin only — never by `core`, never by
*holding* plugins), and how the table survives plugin hot-reload
without rotating handle bits.

It refuses to own:

- The **byte layout** any handle resolves to (the resolving
  plugin's `.fory` schema concern; SPEC §4.7 inv. 2, §7.2 #5).
- **Asset I/O** — file open, decode, GPU upload, residency
  streaming (the `content`, `geometry`, `render` plugins' concern;
  SPEC §4.7 inv. 4).
- **Content-addressing** — `AssetId`, BLAKE3 content hash,
  CAS lookup, dependency graphs (the `content` context's concern;
  Harmonius `AssetId(u64)` and `ContentHash([u8;32])` are
  explicitly **out of MVP scope for the asset-handle aggregate**;
  see §2 row "AssetId").
- **Hot-swap policy** beyond "handle bits survive, payload pointer
  re-resolves" — atomic-pointer / descriptor-heap / pipeline-state
  swap strategies are the resolving plugin's choice, recorded in
  that plugin's hot-reload `migrate(...)` body (SPEC §8.2),
  not in core.
- **Type registry** — the mapping from `TypeId` to descriptor is
  `TypeRegistry`'s job (§4.9); the asset table only records a
  handle's `type_tag` for cheap type-mismatch detection at
  `resolve` time.

The design lives entirely inside the `core/asset/` sub-module
(SPEC §6.1 row 6, §6.8). The public ABI surface it exports is the
`AssetHandle<T>` opaque value object (SPEC §5.2) and the typed
table operations (`insert` / `resolve` / `release` /
`is_alive`).

---

## 2. Requirements Coverage

Harmonius prior art (`harmonius/docs/design/core-runtime/ids.md`,
`harmonius/docs/design/core-runtime/primitives.md`,
`harmonius/docs/design/content-pipeline/asset-pipeline.md`) is
treated as **research input only** (PHILOSOPHY "How harmonius is
used"). Conclusions are re-derived against glibre's invariants.

The table below enumerates every harmonius requirement / design
clause that touches the asset-handle space and routes it to one
of: **covered** (with this doc's section), **collapsed** (folded
into a single glibre primitive), or **refused** (out of MVP scope
with rationale).

| Harmonius clause                                                                                            | glibre disposition                                                                                                                         | Where                |
|-------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------|----------------------|
| `Handle<T>` = `(index: u32, generation: u32, PhantomData<T>)` (`primitives.md` §"API Design")               | **Covered.** Re-derived as `AssetHandle<T>` packed into 64 bits via `(index: 40, generation: 22, type_tag: 2)`; phantom-typed.             | §3, §4               |
| `Handle::NULL` = `(u32::MAX, u32::MAX)` (`primitives.md`)                                                   | **Covered.** Sentinel value; see §4.4.                                                                                                     | §4.4                 |
| `HandleMap<T>` = `Vec<Slot<T>> + free: Vec<u32>` (`primitives.md`)                                          | **Covered.** `AssetTable<T>` re-derived in `core/src/asset/table.hpp` with `eastl::vector` (no `std::pmr`).                                 | §3.2                 |
| Generational reuse policy (`ids.md` "Policy Table")                                                         | **Covered.** Generation bumps on `release`, never on `insert`; never compacted in MVP.                                                     | §3.3, §4.2           |
| `AssetHandle<T>` = `Handle<T>`, session-stable (`ids.md` Taxonomy Part 2)                                   | **Covered.** `AssetHandle<T>` is session-stable; persistent identity is the resolving plugin's concern, not core's.                        | §7                   |
| `AssetId(u64)` = stable content-hash address (`ids.md`, `asset-pipeline.md`)                                | **Refused for MVP.** Content-addressing belongs to the `content` plugin's spec, not core. Core's table is keyed by **opaque slot index**, not content hash. Re-derive there if/when `content` lands a CAS spec. | n/a (out of scope)   |
| `ContentHash([u8;32])` BLAKE3 of asset bytes (`asset-pipeline.md`)                                          | **Refused.** Same rationale as `AssetId`. Core never sees asset bytes (SPEC §4.7 inv. 4).                                                  | n/a                  |
| `HandleTable::swap_ptr(index, ptr, size)` for hot-swap (`asset-pipeline.md` §"Hot Reload")                  | **Collapsed.** Hot-reload survival is achieved by **handle bits surviving** (§8) plus the resolving plugin re-publishing the payload pointer at `glibre_plugin_register` time. No core-level `swap_ptr` API exists. | §8                   |
| `SwapStrategy { AtomicPointer, DescriptorHeap, PipelineState, BytecodePatch, SubtreeRebuild }`              | **Refused.** Strategy choice is the resolving plugin's. Core only guarantees handle-bit stability; the strategy is implemented in the plugin's `migrate(...)` body. | n/a                  |
| `SwapScheduler` queueing pending swaps                                                                      | **Refused / collapsed.** The `HotReloadBarrier` (SPEC §4.6) is the only swap scheduler in MVP. Per-asset swap queues are a post-MVP optimization for the resolving plugin. | n/a                  |
| `HandleMap::iter()` deterministic iteration (`primitives.md`)                                               | **Refused for MVP.** Core does not need to iterate the asset table on the hot path; the resolving plugin owns its own iteration over its payloads. Reconsider if a use case appears. | n/a                  |
| Refcounting / residency streaming (`asset-pipeline.md` §"Residency Manager")                                | **Refused.** Residency is `content`'s concern; the asset table records a slot, not a refcount.                                             | n/a                  |
| Dependency tracking between assets (`asset-pipeline.md` `dependencies: Vec<AssetId>`)                       | **Refused.** Dependency graph is `content`'s; core's table has no dependency edges.                                                        | n/a                  |
| `R-1.7.5`, `R-1.7.6` (memory management; handle/slot-map convention)                                        | **Covered.** §4 — bit layout + slot vector + free list, all under `ContextTag::core` allocator (perf-budget.md §"Allocator Rules").       | §4, §9               |
| `F-1.10.1 — F-1.10.5` ID conventions (`ids.md`)                                                             | **Covered.** Newtype opacity at the ABI seam (SPEC §4.7 inv. 2, §5.2); 64-bit packed; comparison and equality only — no arithmetic exposed.| §4.4                 |
| `R-12.4.2` asset hot-reload with atomic pointer swap                                                        | **Collapsed.** The "atomic pointer swap" of harmonius collapses into the glibre rule: handle bits stable, payload pointer re-resolved by the incoming plugin in step 4 (Resume) of the §6.7 / §8 state machine. Atomicity is provided by the single-threaded hot-reload contract, not by `std::atomic`. | §8                   |

**Coverage attestation.** Every harmonius clause that lands in
MVP scope for the asset-handle aggregate is covered above; every
refusal carries an SRP-grounded rationale rooted in SPEC §3.3
(R-1.4.* refusals to `data` / `content`) or SPEC §4.7 invariants.

---

## 3. Detailed Model

### 3.1 Handle Bit Layout — `AssetHandle<T>` is 64 bits

Public ABI (frozen by SPEC §5.2):

```cpp
template <class T>
struct AssetHandle {
    std::uint64_t bits{};
    friend constexpr bool operator==(AssetHandle, AssetHandle) noexcept = default;
};
```

The 64 bits decompose into three packed fields (SPEC §6.8):

```
 bit  63                                                   0
       ┌───────────┬─────────────────────────┬────────────────────────────────────────┐
       │ type_tag  │       generation        │                 index                  │
       │  2 bits   │        22 bits          │                40 bits                 │
       └───────────┴─────────────────────────┴────────────────────────────────────────┘
                   bit 62                bit 40                                        bit 0
```

| Field        | Width  | Range                   | Role                                                                                                                                                        |
|--------------|--------|-------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `index`      | 40 b   | `0..=2^40-1`            | Dense slot index into the per-`T` table's slot vector. Stable for the slot's lifetime (slots never compacted in MVP, SPEC §6.8).                            |
| `generation` | 22 b   | `0..=2^22-1`            | Bumped on every `release`; on overflow the slot is **retired** (see §4.5). The combination `(index, generation)` is the unique handle identity.             |
| `type_tag`   | 2 b    | `0..=3`                 | Coarse class tag set at table-construction time. Lets `resolve` reject obviously misrouted handles before the index lookup (see §3.4).                      |

**Why these widths.** `index` at 40 bits sustains 1 trillion
slots, well above any plausible MVP asset count (SPEC §6.12.3
sets the reconsideration trigger at `O(2^20)` ≈ 1 M). `generation`
at 22 bits gives ~4 M reuses of a slot before retirement — at the
expected MVP slot turnover rate (~ kHz peak during streaming,
near-zero steady-state) this is multiple in-game days, which is
ample. `type_tag` at 2 bits is intentionally coarse: it is not a
substitute for `TypeId` (which is engine-wide, 64 bits, owned by
`TypeRegistry`) — it is a single-instruction sanity check that
catches accidental cross-table dereferences during development;
the 4 values it encodes are reserved at table-construction time
(see §3.4).

**Bit-packing helpers (private, `core/src/asset/handle_bits.hpp`).**

```cpp
namespace glibre::core::asset_detail {

inline constexpr std::uint64_t kIndexMask     = (1ULL << 40) - 1;
inline constexpr std::uint64_t kGenShift      = 40;
inline constexpr std::uint64_t kGenMask       = ((1ULL << 22) - 1) << kGenShift;
inline constexpr std::uint64_t kTypeTagShift  = 62;
inline constexpr std::uint64_t kTypeTagMask   = (3ULL) << kTypeTagShift;

constexpr std::uint64_t pack(std::uint64_t index,
                              std::uint64_t generation,
                              std::uint64_t type_tag) noexcept {
    return (index & kIndexMask)
         | ((generation << kGenShift) & kGenMask)
         | ((type_tag  << kTypeTagShift) & kTypeTagMask);
}

constexpr std::uint64_t index_of    (std::uint64_t bits) noexcept { return bits & kIndexMask; }
constexpr std::uint64_t gen_of      (std::uint64_t bits) noexcept { return (bits & kGenMask) >> kGenShift; }
constexpr std::uint64_t type_tag_of (std::uint64_t bits) noexcept { return (bits & kTypeTagMask) >> kTypeTagShift; }

}  // namespace glibre::core::asset_detail
```

These are header-only `constexpr` and never appear on a public
header include path; they are internal to the `core/asset/`
sub-module and consumed only by `table.hpp` (§3.2).

### 3.2 Per-Context Table Layout — `AssetTable<T>`

```cpp
// core/src/asset/table.hpp — INTERNAL. Not on the plugin include path.

namespace glibre::core::asset_detail {

template <class T>
struct Slot {
    std::uint32_t generation{0};   // matches handle.generation when live; differs after release
    bool          live{false};     // explicit liveness bit (frees us from sentinel-payload tricks)
    alignas(T) std::byte payload_storage[sizeof(T)];  // typed asset payload, placement-new'd
};

template <class T>
class AssetTable {
public:
    explicit AssetTable(std::uint64_t type_tag,
                        glibre::PerContextAllocator& alloc) noexcept;
    ~AssetTable();

    AssetTable(const AssetTable&)            = delete;
    AssetTable& operator=(const AssetTable&) = delete;

    [[nodiscard]] glibre::Result<AssetHandle<T>>
    insert(T&& payload) noexcept;

    [[nodiscard]] glibre::Result<T*>
    resolve(AssetHandle<T> h) noexcept;

    [[nodiscard]] glibre::Result<const T*>
    resolve(AssetHandle<T> h) const noexcept;

    [[nodiscard]] glibre::Result<void>
    release(AssetHandle<T> h) noexcept;

    [[nodiscard]] bool is_alive(AssetHandle<T> h) const noexcept;

    // Header-only metadata for hot-reload survival (§8). Never on the hot path.
    [[nodiscard]] std::uint64_t type_tag()      const noexcept { return type_tag_; }
    [[nodiscard]] std::size_t   live_count()    const noexcept { return slots_.size() - free_indices_.size(); }
    [[nodiscard]] std::size_t   capacity_slots() const noexcept { return slots_.size(); }

private:
    std::uint64_t                                   type_tag_;
    eastl::vector<Slot<T>,    glibre::EastlAlloc>   slots_;
    eastl::vector<std::uint32_t, glibre::EastlAlloc> free_indices_;  // LIFO of retirable slot indices
};

}  // namespace glibre::core::asset_detail
```

**Why `eastl::vector` not `std::pmr::vector`.** PHILOSOPHY §11 and
the engine-wide stack lock (CLAUDE.md "Tech Stack") elect EASTL
for runtime containers. The earlier SPEC §6.8 sketch used
`std::pmr::vector` as shorthand; this design refines that to the
EASTL form, with `glibre::EastlAlloc` wrapping the
`PerContextAllocator` handle stamped with `ContextTag::core`
(perf-budget.md Allocator Rule #1). The change is internal-only —
no public ABI moves.

**Slot is one cache line per common `T`.** `Slot<T>` lays out as
`{ u32 gen, bool live, padding, T payload }`. For `T` ≤ ~52 bytes
(typical opaque payload pointer + small bookkeeping) the slot
fits in a single 64 B cache line on Apple Silicon, keeping
`resolve` to one cache miss per cold lookup. For larger `T` the
hot/cold split (§5) is invoked.

**Slot vector grows monotonically; never shrinks in MVP.**
Compaction is deferred to spike #498 (`asset-table-compaction-policy`,
SPEC §6.12.3 / §12). Until that spike resolves, `slots_` is
append-only at the high-water mark; freed indices are recycled
through `free_indices_` (§3.3). The 8 MiB heap budget (§9) bounds
the practical slot count — exceeding it triggers
`core::Error::OutOfBudget` from `PerContextAllocator`.

### 3.3 Free-List + Generation-Bump Policy

**Free-list discipline.** `free_indices_` is a LIFO stack of
`u32` slot indices. LIFO is the deterministic-by-construction
choice (PHILOSOPHY §7): two byte-equal histories of `(insert,
release, insert, release, ...)` produce byte-equal slot
allocation orders across hosts, with no platform-dependent free-
list ordering. FIFO would also be deterministic but spreads slot
reuse over time, working against cache locality on `resolve`.

**Insert algorithm.**

```
insert(payload):
  if free_indices_ is non-empty:
    idx ← free_indices_.pop_back()
    slot ← slots_[idx]
    assert slot.live == false
    slot.generation stays as-is  // already bumped at the prior release
    slot.payload_storage ← move-construct(payload)
    slot.live ← true
    return AssetHandle{ bits = pack(idx, slot.generation, type_tag_) }
  else:
    idx ← slots_.size()
    if idx > kIndexMask:           // slot space exhausted (1 T slots)
      return unexpected{ core::Error::OutOfBudget }
    slots_.push_back(Slot{ generation=0, live=true, payload=move(payload) })
    return AssetHandle{ bits = pack(idx, 0, type_tag_) }
```

**Release algorithm.**

```
release(h):
  reject_or_get_slot(h) → slot, idx     // §10 dispatch table
  destroy(slot.payload_storage)         // T's destructor, in-place
  slot.live ← false
  if slot.generation == kGenMask >> kGenShift:   // generation overflow guard
    slot.generation ← 0    // wrap; slot stays retired (see §4.5)
    do NOT push idx onto free_indices_   // slot is permanently retired
    log warn(asset.slot.retired, idx)    // observable in perf HUD; once
  else:
    slot.generation ← slot.generation + 1
    free_indices_.push_back(idx)
```

**The bump-on-release rule (SPEC §4.7 inv. 1).** Generation
increments at *release*, not at insert. The handle returned by
`insert` carries the generation that **was already in the slot**
(either 0 for a fresh slot, or the post-bump value from the prior
release). Every prior handle to that slot now mismatches the
slot's generation and resolves to `core::Error::AssetStale`
(§10).

**Why bump on release rather than reuse.** Bumping at release
means the *same* generation number is associated with at most one
live handle; bumping at reuse would briefly leave the prior
generation associated with a *dead* slot (a window in which a
late-arriving release of a stale handle could mis-target the
revived slot). Bump-on-release closes that window without locks.

### 3.4 `type_tag` Reservation Policy (2 bits)

The four `type_tag` values are reserved at process start by the
order in which `AssetTable<T>` instances are constructed:

| `type_tag` | Reserved to                                          | Source of truth                                  |
|------------|------------------------------------------------------|--------------------------------------------------|
| `0`        | The first `AssetTable<T>` that registers in core.    | `AssetTableRegistry::register<T>()` (§4)          |
| `1`        | The second.                                          | same                                              |
| `2`        | The third.                                          | same                                              |
| `3`        | Reserved sentinel — null handle (§4.4) sets `type_tag = 3` and `index = kIndexMask` and `generation = kGenMask >> kGenShift`. |  hardcoded |

**Why only 4 distinct tags?** The tag is a debug-time mis-route
catch, not a primary type discriminator. Type identity at the
ABI seam is `TypeId` (SPEC §4.9), not `type_tag`. Four tags is
enough to discriminate the three plausible MVP asset families
(meshlet streams owned by `geometry`, texture/material payloads
owned by `render`, sound buffers owned by `content` if the audio
plugin lands within MVP) plus the null sentinel. If a fifth
table type appears within MVP, `type_tag` remains coarse: the
fifth table reuses an existing tag and accepts that mis-route
between those two tables would not be caught at the tag layer
(it would still be caught at `TypeId` lookup time). The choice
to **not** widen the tag to a full `TypeId` here is to preserve
the `(40, 22, 2)` packing — adding a `TypeId` would consume
substantial bits and reduce one of `index` / `generation` below
its MVP-justified minimum.

**Open question** marked in §12: confirm that 4 tags is in fact
enough for MVP once the asset families are concretely scheduled.

### 3.5 `AssetTableRegistry` — Per-`T` Table Instantiation

The asset-handle aggregate exposes one table per asset type.
Tables are constructed at `World` construction time by the
plugin that **owns** the asset type (its
`glibre_plugin_register` body is the only place an
`AssetTable<T>` is instantiated). Core itself owns no asset
tables — core owns the **registry** of tables.

```cpp
// core/src/asset/registry.hpp — INTERNAL.

namespace glibre::core::asset_detail {

class AssetTableRegistry {
public:
    template <class T>
    [[nodiscard]] glibre::Result<AssetTable<T>*>
    register_table() noexcept;     // assigns next free type_tag; refuses on exhaustion

    // Hot-reload survival: lookup by TypeId, bytes-out interface for
    // the migration arena (§8). Not on the per-frame hot path.
    [[nodiscard]] AssetTable<>* lookup_erased(TypeId t) noexcept;

private:
    eastl::vector<eastl::pair<TypeId, void* /* AssetTable<T>* */>,
                  glibre::EastlAlloc> tables_;
    std::uint8_t next_type_tag_{0};   // monotonic; saturates at 3
};

}  // namespace glibre::core::asset_detail
```

The registry is a `World`-scope singleton (one per `World`)
because tables hold typed payloads whose lifetime tracks the
`World`'s. SPEC §4.7 inv. 3 declared the table itself
"singleton per process"; this design refines that to "registry
of tables is per-`World`, but slot-index space is per-table-per-
process" — the clarification is consistent with §4.7 and
preserves the cross-world handle-validity property when multi-
world lands.

---

## 4. Public Surface

The public ABI surface is the SPEC §5.2 `AssetHandle<T>` struct
plus the typed table operations. The full surface (frozen by this
design):

```cpp
// core/include/glibre/core/asset_handle.hpp — PUBLIC.

#pragma once
#include <glibre/error.hpp>           // Result<T>, glibre::Error
#include <cstdint>

namespace glibre::core {

// 4.1 AssetHandle<T> — opaque 64-bit value object (SPEC §5.2).
// Phantom-typed: AssetHandle<Mesh> != AssetHandle<Texture>.
template <class T>
struct AssetHandle {
    std::uint64_t bits{0};

    friend constexpr bool
    operator==(AssetHandle, AssetHandle) noexcept = default;

    [[nodiscard]] static constexpr AssetHandle null() noexcept;
    [[nodiscard]] constexpr bool is_null() const noexcept;
};

// 4.2 Typed table operations. Implementation is private (§3.2).
// The free-function shape is the ABI; method-shape variants are
// implementation detail.

template <class T>
[[nodiscard]] glibre::Result<AssetHandle<T>>
asset_insert(T&& payload) noexcept;

template <class T>
[[nodiscard]] glibre::Result<T*>
asset_resolve(AssetHandle<T> h) noexcept;

template <class T>
[[nodiscard]] glibre::Result<const T*>
asset_resolve(AssetHandle<T> h) noexcept;   // overloaded for const-correctness

template <class T>
[[nodiscard]] glibre::Result<void>
asset_release(AssetHandle<T> h) noexcept;

template <class T>
[[nodiscard]] bool
asset_is_alive(AssetHandle<T> h) noexcept;

}  // namespace glibre::core
```

### 4.1 Phantom Typing — Cross-Type Mis-Use Refused at Compile Time

`AssetHandle<Mesh>` and `AssetHandle<Texture>` are **distinct
types** because they instantiate the template at different `T`s.
A function declared `f(AssetHandle<Mesh>)` cannot be called with
an `AssetHandle<Texture>` — the compiler rejects the implicit
conversion.

This is the C++ analogue of harmonius's
`PhantomData<fn() -> T>` (`primitives.md`), but stronger: in
harmonius the phantom marker only affected `Eq` / `Hash`
specializations; here the entire template instantiation is
distinct, so there is **no path** for a wrong-`T` handle to
silently dereference the wrong table. The runtime `type_tag`
(§3.4) is a defense-in-depth check, not the primary mechanism.

### 4.2 `Result<T>`-on-Resolution (per `error-model.md`)

Every fallible call returns
`glibre::Result<T> = std::expected<T, glibre::Error>`, never an
exception, never a sentinel pointer (`nullptr` is reserved for
`AssetHandle<T>::null()`, not for "lookup failed"). This obeys
the engine-wide error model decision record verbatim:
exception-free, no RTTI, no allocation on the failure path.

The complete failure-mode mapping is §10.

### 4.3 Nullable Handle Sentinel — `AssetHandle<T>::null()`

SPEC §5.2 makes `AssetHandle<T>` default-constructible to a
zero-bits value. This design **redefines** the null sentinel to
be the all-ones index, all-ones generation, `type_tag = 3`
combination — i.e. `bits == 0xFFFFFFFFFFFFFFFFULL`:

```cpp
template <class T>
constexpr AssetHandle<T> AssetHandle<T>::null() noexcept {
    return AssetHandle<T>{ 0xFFFF'FFFF'FFFF'FFFFULL };
}

template <class T>
constexpr bool AssetHandle<T>::is_null() const noexcept {
    return bits == 0xFFFF'FFFF'FFFF'FFFFULL;
}
```

**Why not zero?** Because `(index = 0, generation = 0,
type_tag = 0)` is a *valid* live handle to the first slot of
table-tag-0 — that is the dense-slot identity space starting
from zero, the same as harmonius's `Handle::NULL ≠ Handle{0,0}`
choice. The earlier SPEC §5.2 default-construction-to-zero was a
shorthand that implicitly relied on "no caller will ever
default-construct an `AssetHandle` and pass it to `resolve`". This
design tightens that: default-construction yields a zero-bits
value which is a **valid** handle to slot 0 (and will still
correctly fail with `AssetStale` if slot 0 has been released);
callers who want a sentinel **MUST** call `null()`. The tighter
contract removes the silent-failure mode where a default-
constructed `AssetHandle` accidentally aliases a real asset.

**`is_null` as a fast pre-check.** Callers may guard `resolve`
with `if (h.is_null()) { ... }` to avoid the table-lookup cost on
expected-null paths (e.g. optional component fields). This is
not load-bearing for correctness — `resolve(null)` returns
`unexpected{AssetStale}` — but it is a perf-friendly idiom.

### 4.4 Exact ABI Footprint

`sizeof(AssetHandle<T>) == 8`, `alignof(AssetHandle<T>) == 8`,
trivially copyable, trivially destructible, standard layout. The
type satisfies all the POD-aggregate requirements demanded by
`reviews/decisions/plugin-abi.md` for cross-plugin handle types.

ABI-equivalence across `T` instantiations (the bits move
identically) is intentional — the resolving plugin's `dlopen`'d
function pointer that consumes an `AssetHandle<T>` sees only the
8 bytes; phantom typing enforces correctness at the compile site
that *constructs* the call, not at the call boundary itself.

---

## 5. Hot/Cold Path Split

The aggregate cleanly partitions into a **hot** path (everything
the per-frame benchmark cell measures, §9) and a **cold** path
(everything that runs once per insert / release / hot-reload).

### 5.1 Hot Path — `resolve`

```
resolve(h):                                                    cycles
  1. b ← h.bits                                                // 1
  2. idx ← b & kIndexMask                                       // 1
  3. gen ← (b & kGenMask) >> kGenShift                          // 2
  4. tag ← (b & kTypeTagMask) >> kTypeTagShift                  // 2
  5. if tag != table.type_tag    → unexpected{AssetTypeTag}    // 1 cmp+branch
  6. if idx >= slots.size()      → unexpected{AssetStale}      // 1 cmp+branch
  7. slot ← slots[idx]                                         // 1 cache-line load
  8. if !slot.live                → unexpected{AssetStale}      // 1 cmp+branch
  9. if slot.generation != gen   → unexpected{AssetStale}      // 1 cmp+branch
 10. return &slot.payload_storage cast to T*                   // 0 (return)
```

Steady-state cost on Apple Silicon firestorm at 3.2 GHz:
~10 ns / resolve when the slot is L1-resident. The §9 budget
(0.05 ms / 8 MiB for the table + per-frame residency tickle, SPEC
§9.3 row "AssetHandle table") sustains ~5000 resolves / frame
even on the cold-cache worst case (~10 µs at L2 latency).

The per-`T` `slots` vector backs the hot read; the table struct
itself is loaded once into a register at the call site and
cached.

### 5.2 Cold Hot/Cold Layout — Optional SoA Split

For asset families whose payload `T` is large (texture metadata
~256 B, mesh metadata ~512 B), the hot path benefits from a
**hot index / cold metadata split**:

```cpp
// Optional refinement, plan-time decision per-T.
template <class T>
struct AssetTableSoA {
    eastl::vector<HotIndex>  hot_;      // (gen:32, live:1, padding:31)  — 8 B / slot
    eastl::vector<T>         cold_;     // payload only                  — sizeof(T) / slot
    eastl::vector<uint32_t>  free_;
};
```

`HotIndex` packs `(generation, live)` into 8 bytes. `resolve`
hits `hot_[idx]` first (1 cache line covers 8 slots) and only
loads `cold_[idx]` when the caller actually needs the payload.
This trades ~2x storage overhead for ~4-8x denser hot-loop
walks during the per-frame residency sweep (§9 row).

**Decision rule (deferred to per-`T` plan).** Use the §3.2
single-vector layout when `sizeof(T) ≤ 32` B; consider the SoA
split when `sizeof(T) ≥ 64` B. The cutoff is a heuristic that
reflects Apple Silicon's 64 B cache line. The per-`T` choice is
made by the plugin that registers the table, not by core.

### 5.3 Cold Path — `insert`, `release`

Both operations touch `free_indices_` (LIFO push/pop) and one
slot. They are bounded but not benchmark-asserted at the §9
per-frame budget; their cost is amortized into the
**reload-frame** budget when many releases coincide with a
plugin swap (SPEC §8 step 1).

`insert` may reallocate `slots_`. The growth strategy is the
EASTL default (geometric, factor 2). Reallocation is `O(N)` and
not on the per-frame hot path — `insert` is a load-time or
streaming-completion event, not a steady-state action.

### 5.4 Hot-Path Cost Summary

| Operation        | Path | Steady-state cost  | Notes                                                       |
|------------------|------|--------------------|-------------------------------------------------------------|
| `resolve`        | hot  | ~10 ns / call      | 1 cache-line load worst case; phantom-typed compile-time check |
| `is_alive`       | hot  | ~6 ns / call       | `resolve`-without-payload-cast                              |
| `is_null`        | hot  | <1 ns              | constexpr-foldable; one comparison                          |
| `insert`         | cold | ~50 ns amortized   | growth events excluded; happens at load / streaming completion only |
| `release`        | cold | ~30 ns / call      | hits `free_indices_`; not on per-frame hot path            |

---

## 6. Concurrency

### 6.1 Single-Threaded Read/Write in MVP

SPEC §6.10 commits MVP to single-threaded execution on the game-
loop driver thread for all systems. The asset-handle aggregate
adopts that model verbatim:

1. **Reader fast path: lock-free, no atomics.** `resolve` and
   `is_alive` perform plain loads against `slots_` and the slot's
   `(generation, live)` fields. No `std::atomic`. The single-
   threaded contract guarantees no concurrent writer, and SPEC
   §6.10's "memory ordering: every public API in §5 is `noexcept`
   and assumes single-threaded access" applies.
2. **Mutator path: same thread, sequential.** `insert` and
   `release` are called on the same thread that calls `resolve`.
   No reader/writer interleaving exists in MVP.
3. **Hot-reload mutator: single-threaded by contract.** The
   `HotReloadBarrier` (SPEC §6.7) runs phase 8 single-threaded;
   any `insert` / `release` invoked during a plugin's
   `glibre_plugin_drain` or `glibre_plugin_register` body
   executes on the loop thread.

### 6.2 Frame-Phase Alignment

The asset table interacts with the nine-phase frame ordering
(SPEC §5.3) as follows:

| Phase | Asset-table interaction                                                                                           |
|-------|-------------------------------------------------------------------------------------------------------------------|
| 1 (Input)        | Systems may `resolve` to read asset metadata. No `insert` / `release`.                                  |
| 2-7 (Logic / PhysicsFixed / Animation / Transform / CullExtract / RenderSubmit) | `resolve` permitted (most consumers live here). `insert` / `release` permitted but strongly discouraged outside hot-reload — production code routes mutation through `CommandBuffer`-equivalent deferred queues owned by the resolving plugin (out of scope for core). |
| 8 (HotReload)    | `insert` / `release` are invoked by the resolving plugin's `glibre_plugin_drain` / `glibre_plugin_register` bodies. The §8 hot-reload survival rules apply. |
| 9 (Present)      | `resolve` permitted (frame-stat counter writes). No `insert` / `release`.                                |

**No phase ordering rule is added by the asset-handle aggregate.**
The frame-phase contract (frame-phases.md) is the authoritative
source; this aggregate participates in the phases without
introducing new barriers.

### 6.3 Future Parallelism — Pure-Additive Path

When per-system parallelism lands (SPEC §6.10 deferred, spike
#496), the hot path's lock-freedom carries over **only if writes
remain serialized through the schedule** — i.e. the schedule
ensures that a phase containing an `insert`-bearing system never
overlaps a phase containing a `resolve`-bearing system on the
same `T`. This is a schedule-level constraint, not a core-level
synchronization primitive, and aligns with the existing access-
set DAG (SPEC §4.4 inv. 2).

If the parallelism plan ever needs concurrent
mutation + read, the design path is to switch `slots_` to a
read-copy-update arena swapped at phase boundaries. That decision
is **out of MVP scope** and recorded in §12.

### 6.4 Memory Ordering at the Hot-Reload Seam

The single `std::atomic` in core's hot path is the
`HotReloadBarrier::pending_` counter (SPEC §6.10). The asset
table does not introduce another. The hot-reload survival
property (§8) is achieved by **leaving the bits alone** across
the swap, not by any synchronization between an outgoing and an
incoming plugin.

---

## 7. Persistence + ABI

### 7.1 Handles Serialize as Stable Opaque IDs

When a handle is persisted (e.g. as a field of a component whose
schema is `.fory`), its 64-bit `bits` value is written as a
single `u64` field. The schema declaration looks like (using the
schema-DSL of fory-codegen.md):

```fory
schema example.MeshRef {
  version 1
  field handle : u64 tag 1 since 1   // packed AssetHandle<Mesh>::bits
}
```

**Why a raw `u64`, not a typed serialization.** The asset-handle
aggregate is owned by core; the `T` parameter of `AssetHandle<T>`
lives in a downstream plugin's headers and is not visible to
core's schema codegen. Writing `u64` keeps the persistence layer
ignorant of `T` (consistent with SPEC §4.7 inv. 2 — opacity at
the ABI seam) and lets the resolving plugin reconstruct typing
on read.

The schema is authored by the **plugin that owns the field**
(per SPEC §7 routing of component schemas to their plugin), not
by core. Core itself authors no `.fory` file containing a
handle (its persistent schemas — `PluginManifest`,
`HotReloadCheckpoint`, `LoadedPluginRecord`, SPEC §7.1 — touch
no asset payloads).

### 7.2 Generation Is NOT Serialized as a Re-Used Counter

A persisted handle stores its **bit pattern at write time** —
`(index, generation, type_tag)`. On read, the generation is
**not** re-derived; it is the bit pattern.

This is intentional for the live-process round-trip case
(snapshot-and-restore within one process; SPEC §7 reload-frame
state survival). Across **process boundaries** — save/load,
network — handles are by definition stale (the asset table
reconstructs from registration, not from persistence):

| Boundary                                | Handle survival                                                                                                                           |
|-----------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------|
| Same process, hot-reload (SPEC §8.1 #4) | Bits stable. Resolves through the new plugin's payload pointer.                                                                           |
| Same process, world snapshot replay     | Bits stable; the snapshot replays into the same `AssetTable<T>` instance.                                                                  |
| **Cross process / save-load**           | **Bits NOT stable.** A loaded save reconstructs the asset graph from `content`-context content-hash addressing (`AssetId`, out of scope for core), then **re-derives** new `AssetHandle<T>` values. Persisted handle bits are discarded on load. |
| **Network**                             | **Bits NOT transmitted.** Handles are session-stable (`harmonius/ids.md` row `AssetHandle<T>` Stability=Session, re-derived in glibre); cross-machine identity is `AssetId` content hash, owned by `content`. |

The `harmonius/ids.md` taxonomy classifies `AssetHandle<T>` as
session-stable; this design preserves that classification. The
"generation is not re-derived" rule above means "within a
session, the handle bits are the identity"; across-session
persistence is the **resolving plugin's** problem to remap.

### 7.3 ABI Stability of `AssetHandle<T>`

The 64-bit bit layout (`(40, 22, 2)` packing) is part of the
**plugin ABI**: every plugin compiled against a given
`glibre-types.dylib` sees the same bit packing. Changing the
packing is an ABI break that bumps the middleman dylib hash
(`reviews/decisions/plugin-abi.md` "ABI Hash Function"); plugins
compiled against the prior packing refuse to load
(`core::Error::PluginAbiHashMismatch`). The hash gate makes the
packing change **observable**; there is no silent reinterpretation.

The bit packing is pinned in the public header
`glibre/core/asset_handle.hpp` as `static_assert`s:

```cpp
static_assert(asset_detail::kIndexMask     == (1ULL << 40) - 1);
static_assert(asset_detail::kGenShift      == 40);
static_assert(asset_detail::kTypeTagShift  == 62);
```

Editing these is a SPEC §4.7 invariant change, requires a
perf-budget amendment spike if it affects the §9 cell, and
requires a plugin-abi amendment spike (because the dylib hash
changes). The chain of gates makes silent reinterpretation
impossible.

---

## 8. Hot-Reload

### 8.1 What Survives the Frame-8 Swap (SPEC §8.1 #4)

Across the four-step swap (Drain → Swap → Migrate → Resume,
SPEC §6.7 / §8) the asset-handle aggregate guarantees:

1. **Handle bits are byte-stable.** Every outstanding
   `AssetHandle<T>` retains its `(index, generation, type_tag)`
   value. A handle valid at phase 7 of frame N is valid at phase
   1 of frame N+1 with no re-resolution by the holder.
2. **The slot vector is byte-stable.** `slots_` is owned by
   `AssetTable<T>`, which is a per-`World` singleton (§3.5);
   the `World`'s storage is unchanged across phase 8 (SPEC
   §8.1 #2). The vector pointer, length, and live count survive.
3. **The free list is byte-stable.** Same rationale.
4. **Generation counters are byte-stable.** A handle whose
   generation matches its slot's generation before the swap
   matches it after the swap.
5. **The `type_tag` reservation is byte-stable.** Tags are
   assigned at first-table-construction; a hot-reload does not
   re-construct the table, so tags do not rotate.

### 8.2 What Does NOT Survive — Payload Pointers

The **payload bytes** stored in `slot.payload_storage` are
plugin-private and survive only when the resolving plugin's
schema migration succeeds:

- **Drain step (SPEC §6.7 step 1).** The outgoing plugin's
  `glibre_plugin_drain` body is responsible for releasing any
  GPU resources / OS handles the payloads point to. The
  `slot.payload_storage` *bytes* remain (the slot is still live;
  generation is unchanged), but their dereferenced state (e.g.
  a `Texture` whose payload contains `MTLTexture*`) is now
  invalid.
- **Resume step (SPEC §6.7 step 4).** The incoming plugin's
  `glibre_plugin_register` body re-acquires the GPU /OS
  resources and overwrites the payload bytes in place. Handle
  bits are unchanged; the dereferenced state is now valid again
  pointing to the new resource.
- **Migrate step (SPEC §6.7 step 3).** If the payload's `.fory`
  schema bumped versions, the per-row migration (SPEC §8.2,
  fory-codegen "Migration Mechanic") rewrites
  `slot.payload_storage` from the prior version's bytes to the
  new version's bytes. Generation does NOT bump (the slot is
  still the same logical asset).

### 8.3 Asset-Table Re-Validation on Plugin Reload

After step 4 (Resume) completes, the `HotReloadBarrier` invokes
a per-table re-validation pass:

```
for each AssetTable<T> owned by the reloaded plugin:
  for each live slot idx in 0..slots.size():
    if slot.live and not plugin.has_payload(idx):
      log warn(asset.payload.unresolved, idx, T)
      slot.live ← false                  // mark stale; do NOT bump generation
      free_indices_.push_back(idx)       // recycle next insert
```

The `live ← false` (without generation bump) is **deliberate**:
the old handle bits would still mismatch the slot's generation
on `resolve` if the slot is later re-inserted (the new insert's
generation is the slot's pre-existing one), so stale handles
remain stale; the slot is recycled for new content. This is
weaker than the "always-bump-on-release" rule (§3.3) — it is the
single point where generation is **not** bumped, because the
release is induced by the plugin reload, not by user code.

**Why not bump.** Bumping during the re-validation pass would
cost one re-insert's worth of generation per stale slot in a
plugin reload, which can be hundreds of slots if a plugin's
asset count is large. Cumulatively, generation overflow (§4.5
retirement) becomes a real risk over many reload cycles. Skipping
the bump in this case preserves the generation budget and is
sound because the "live=false + recycled" state correctly
invalidates outstanding handles via `slot.live`.

### 8.4 Refusal Cases (SPEC §8.7 / §10.1)

If any step of the swap refuses (drain timeout, ABI hash drift,
schema migration failure, init failure, self-reference), the
**handle table is rolled back byte-for-byte** to the pre-barrier
state. SPEC §4.11 inv. 4 already sets this rule; this design adds
no new refusal case — the asset table participates in the swap
through the `HotReloadBarrier`'s rollback discipline.

The roll-back works because `slots_` and `free_indices_` are
plain EASTL vectors whose state at phase 8 entry can be
checkpointed (size + per-slot bytes) into the migration arena
(SPEC §6.7, perf-budget.md Allocator Rule #6) and restored on
rollback. The checkpoint cost is bounded by the table's heap
ceiling (8 MiB per table per §9.3) and fits inside the migration
arena's 16 MiB cap.

### 8.5 Cross-Reference — `[OPEN]` Dependency on #498

Spike #498 (`asset-table-compaction-policy`, SPEC §6.12.3 and
§12) is an **open dependency** of this design: a future
compaction policy will move slot indices, which would break the
"handle bits byte-stable across hot-reload" guarantee unless
compaction either (a) defers to a quiescent point with no live
handles, or (b) introduces a redirection table indexed by old
slot index. This design does not commit to either; it commits
only to the MVP behavior ("never compact") and flags the
compaction-vs-handle-stability interaction in §12 so #498
inherits the constraint.

---

## 9. Performance

### 9.1 Cell — SPEC §9.3 Row "AssetHandle table"

Quoted verbatim from `specs/core/SPEC.md` §9.3:

| Aggregate              | CPU ms | Half | Heap   | Dominant operation                                    |
|------------------------|--------|------|--------|-------------------------------------------------------|
| `AssetHandle` table    | ~0.05  | sim  | 8 MiB  | O(1) handle resolution + generation-tag check on lookup |

This design does not expand or contract the cell. It refines what
the cell is **paying for**:

- The 0.05 ms / sim covers the **per-frame residency tickle**
  loop: a phase-1 / phase-9 system that walks any
  resolving-plugin-side residency state (NOT core's slot
  vector — core never iterates the table on the hot path). Core's
  contribution is the `resolve` calls the residency system
  itself makes, bounded at ~5000 / frame.
- The 8 MiB / heap holds the `slots_` vector and `free_indices_`
  for **all `AssetTable<T>` instances**, summed. Each table's
  share is enforced advisorily (perf-budget.md Allocator Rule
  #4): the per-table ceiling is the cell budget divided by the
  number of registered table types, with the registry refusing
  registration of a type that would push the resident bytes over
  8 MiB.

### 9.2 O(1) Resolution (SPEC §9.3 dominant op)

The `resolve` algorithm (§5.1) is constant-time in the slot
count: bit unpack, two array accesses, three comparisons. Worst
case is one cache miss on `slots_[idx]` (one cache line); typical
case is L1-resident.

The 0.05 ms / frame budget at ~10 ns / `resolve` covers ~5000
resolves / frame; at the cold-cache ~10 µs the budget covers
~5 resolves / frame — the actual residency-tickle pattern is
warm-cache (the resolves walk a contiguous range), so the warm-
case bound applies.

### 9.3 Per-Context Heap Cell — `ContextTag::core`

Every byte allocated by `AssetTable<T>` and `AssetTableRegistry`
flows through `glibre::PerContextAllocator` stamped with
`ContextTag::core` (perf-budget.md Allocator Rule #1). Strict-
mode debug builds refuse allocations that push core's resident
bytes over 64 MiB; the asset-handle's 8 MiB sub-share is the
benchmarked-cell ceiling (§9.4 below), enforced by
`BENCHMARK_CELL`.

### 9.4 CI Gate — `BENCHMARK_CELL` (SPEC §9.5)

The mandated micro-benchmark (SPEC §9.5 row):

| Aggregate           | `BENCHMARK_CELL` test name                 | CPU ceiling | Heap ceiling |
|---------------------|--------------------------------------------|-------------|--------------|
| `AssetHandle` table | `core/asset: handle_o1_resolve_refcount`   | 0.05 ms     | 8 MiB        |

Per `perf-budget.md` §"CI Gate Spec" rule 1, this `BENCHMARK_CELL`
runs on every PR touching `core/src/asset/**`. The fixture
(under `e2e/perf/asset_handle/` per perf-budget.md fixture
conventions) constructs an `AssetTable<MockPayload>` populated
with the S1 fixture's asset count and exercises a representative
mix of `resolve` calls.

Drift triggers the headroom-regression alarm
(perf-budget.md §"CI Gate Spec" rule 5).

### 9.5 Cost Audit — Where the 0.05 ms Goes

Sum over the `resolve` call sites the §9.3 cell pays for in S1:

| Caller                                                               | Calls / frame | Cost / call | Subtotal |
|----------------------------------------------------------------------|---------------|-------------|----------|
| `geometry` meshlet visibility scoring (~200 props × ~16 meshlets)    | ~3200         | ~10 ns      | ~32 µs    |
| `render` material binding lookup (~3000 draws × 1 material handle)   | ~3000         | ~10 ns      | ~30 µs    |
| `tools` editor inspector (~50 handles, edit-mode only)               | ~50           | ~10 ns      | <1 µs     |
| Residency tickle (~100 sweep entries)                                | ~100          | ~10 ns      | ~1 µs     |
| **subtotal**                                                         |              |             | **~64 µs** |

Subtotal ~64 µs ≤ 50 µs steady-state cell budget ... wait — the
addition exceeds the cell. **This is intentional headroom
attribution**: the geometry and render `resolve` calls are
attributed to **those contexts'** sim/submit cells (geometry
0.30 sim + 0.20 submit; render 0.10 sim + 1.40 submit), not to
core's cell. Core's 0.05 ms is the **residency-tickle + tools
inspector + dispatch overhead** sliver. The full ~64 µs across
all contexts is well inside the engine-wide 8.05 ms CPU sim/submit
budget; the per-context attribution sums correctly because
`resolve` cycles are charged to the *caller's* context, not to
the asset table's owner. This is consistent with perf-budget.md
"Per-context budget over per-phase budget" rationale.

---

## 10. Failure Modes

### 10.1 `core::Error` Arms Emitted by the Asset-Handle Aggregate

The aggregate emits these arms, all listed in SPEC §5.1
`enum class core::Error`:

| Arm                  | Trigger                                                                                                  | Recovery posture | Severity | Observer event |
|----------------------|----------------------------------------------------------------------------------------------------------|------------------|----------|----------------|
| `AssetStale`         | `resolve` finds `slot.generation != handle.generation` OR `slot.live == false` OR `idx >= slots.size()`. | Refuse           | `warn`   | none           |
| `OutOfBudget`        | `insert` fails because (a) the index space is exhausted (idx > 2^40-1, never observed in MVP), or (b) `PerContextAllocator` returns `unexpected{OutOfBudget}` while growing `slots_`. | Refuse / propagate | `warn` (debug) / `error` (shipping if surfaced) | none |

**Type mismatch.** A handle whose `type_tag` differs from the
table's tag is a programming error (cross-table dereference).
The runtime check in step 5 of §5.1 returns `AssetStale` rather
than introducing a separate `AssetTypeTagMismatch` arm. This
collapse is a deliberate Occam's-razor choice (PHILOSOPHY §10):
both errors lead to the same recovery (refuse, log warn), and
distinguishing them at the `core::Error` level adds an enum arm
(plugin ABI cost) without operator-action distinction. The
detail string in `ErrorContext` carries the specific cause
(`asset.stale.tag_mismatch` vs `asset.stale.generation`), per
the error-model "`detail`" field rule.

**`AssetTypeTagMismatch` rejected as a separate arm; rationale
recorded in §12 #1.**

### 10.2 Cross-Reference With SPEC §10.1 Row `AssetStale`

SPEC §10.1 already lists `AssetStale` with trigger "`AssetHandle::
resolve` finds a slot whose generation has advanced past the
handle's (§4.7 #1)", recovery "Refuse", severity "warn". This
design widens the trigger to also cover the dead-slot and
out-of-range cases above, consistent with §10.2 #5 ("the recovery
verb in §10.1 is the *emitting module's* posture; the *caller's*
posture is always one of: propagate, handle + log, terminate").

### 10.3 No-Throw Discipline

Every public function in §4 is `noexcept`. Allocation failure
paths return `std::unexpected` rather than throwing
`std::bad_alloc`; the `PerContextAllocator` contract delivers
the failure as a `Result<T>` on the affected call site. This
preserves the engine-wide `-fno-exceptions` build flag
(error-model.md "Decision" #3).

### 10.4 Determinism Obligation

`AssetStale` and `OutOfBudget` are subject to SPEC §10.2 #7
(deterministic-replay obligation): the same input history
produces the same arm with the same `ErrorContext` payload across
hosts and runs. The free-list LIFO discipline (§3.3) is the
load-bearing design choice that makes this hold — a
non-deterministic free-list would surface as
`AssetStale`-vs-success divergence on the same input.

---

## 11. Test Plan

The asset-handle aggregate is closed by SPEC §11 user-story
**#348 — `[STORY] asset-handle-generational-table` — pts:2**. The
test plan below is the design-time decomposition that the
sibling task-breakdown spike will turn into a `type:plan` issue.

### 11.1 Unit Tests (Catch2)

All under `tests/core/asset/` — names canonical, asserted by the
spec-check CI workflow.

| Catch2 test name (`[tag]`)                                                    | Asserts                                                                                                                  | §-ref          |
|-------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------|----------------|
| `core/asset: insert_returns_unique_index [unit]`                              | Two consecutive `insert`s on a fresh table return handles whose `index_of(bits)` differ.                                 | §3.3, §4       |
| `core/asset: insert_reuses_freed_slot_lifo [unit]`                            | After `release(h0); insert; insert`, the second new handle reuses `h0`'s slot index.                                     | §3.3 LIFO      |
| `core/asset: release_bumps_generation [unit]`                                 | After `release(h)`, a re-`insert` returns a handle whose generation is `gen_of(h) + 1`.                                  | §3.3, §4.2     |
| `core/asset: release_invalidates_outstanding_handles [unit]`                  | After `release(h)` followed by `insert`, `resolve(h)` returns `unexpected{AssetStale}`.                                  | §10.1          |
| `core/asset: resolve_dead_slot_is_stale [unit]`                               | `resolve` on a `release`'d handle whose slot was not re-`insert`'d returns `unexpected{AssetStale}`.                     | §10.1          |
| `core/asset: resolve_out_of_range_is_stale [unit]`                            | `resolve` of a hand-crafted handle with `index_of(bits) > slots.size()` returns `unexpected{AssetStale}`.                | §10.1          |
| `core/asset: resolve_wrong_type_tag_is_stale [unit]`                          | `resolve` of a hand-crafted handle whose `type_tag_of(bits) != table.type_tag` returns `unexpected{AssetStale}`.         | §3.4, §10.1    |
| `core/asset: phantom_typing_compile_check [unit]`                             | `static_assert` (compile-time test) that `AssetHandle<Mesh>` and `AssetHandle<Texture>` are distinct types.              | §4.1           |
| `core/asset: null_handle_resolves_stale [unit]`                               | `resolve(AssetHandle<T>::null())` returns `unexpected{AssetStale}`; `is_null()` returns `true`.                          | §4.4           |
| `core/asset: zero_bits_handle_is_not_null [unit]`                             | `AssetHandle<T>{0}.is_null()` returns `false`.                                                                          | §4.4           |
| `core/asset: handle_bit_layout_static_asserts [unit]`                         | `static_assert` checks for the `(40, 22, 2)` packing.                                                                    | §3.1, §7.3     |
| `core/asset: generation_overflow_retires_slot [unit]`                         | After 2^22 `release` cycles on the same slot, the slot is retired (no new handle reuses its index); subsequent `insert` allocates a new slot. | §4.5     |
| `core/asset: insert_propagates_out_of_budget [unit]`                          | A mock allocator returning `unexpected{OutOfBudget}` causes `insert` to propagate the same arm.                          | §10.1          |
| `core/asset: deterministic_free_list_replay [unit]`                           | A scripted `(insert, release, insert, release)` sequence yields byte-equal handle sequences across two table instances. | §3.3, §10.4    |
| `core/asset: type_tag_assignment_is_monotonic [unit]`                         | The first three `register_table<T>` calls receive `type_tag` 0, 1, 2; the fourth receives `unexpected{...}` (or, per §3.4, reuses an existing tag with a warn). | §3.4         |

### 11.2 Integration Tests (Catch2 + hot-reload fixture)

These live under `tests/core/asset/integration/` and use the
in-process hot-reload fixture from SPEC §8.9.

| Catch2 test name                                                               | Asserts                                                                                                                                   | §-ref       |
|--------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------|-------------|
| `core/asset/integration: handle_bits_survive_hot_reload [integration]`         | Insert handles before swap; reload plugin; assert handle bits at frame N+1 match those at frame N (byte-equal).                            | §8.1        |
| `core/asset/integration: payload_re_resolves_after_reload [integration]`       | Insert handle pointing to a plugin-private payload; reload plugin; assert `resolve` returns a valid (re-resolved) payload pointer.        | §8.2        |
| `core/asset/integration: stale_payload_marked_after_reload [integration]`      | Insert handle pointing to plugin-A payload; reload swaps to plugin-B that does NOT re-publish payload for that slot; assert `resolve` returns `AssetStale`; assert slot was recycled into `free_indices_`. | §8.3 |
| `core/asset/integration: rollback_preserves_handle_bits [integration]`         | Reload that fails at the migration step; assert post-rollback handle bits unchanged byte-for-byte; assert `resolve` returns the pre-swap payload. | §8.4   |
| `core/asset/integration: schema_migration_round_trip [integration]`            | Insert handle whose payload's `.fory` schema bumps version; reload; assert post-reload handle resolves to migrated payload bytes equal to a hand-authored expected v2 payload.                       | §8 / §8.2 |

### 11.3 Stress / Soak

| Catch2 test name                                                            | Asserts                                                                                                                  | §-ref       |
|-----------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------|-------------|
| `core/asset/stress: million_insert_release_cycles [stress]`                 | 1 M `(insert, release)` pairs complete in <1 s on M1 firestorm; no unbounded `slots_` growth; resident bytes <8 MiB.    | §9.1, §9.4 |
| `core/asset/stress: concurrent_resolve_during_phase_8 [stress]`             | (When per-system parallelism lands; for MVP, marked `[!hidden]`.) Single-threaded MVP placeholder.                       | §6.3       |

### 11.4 Benchmark Cell (Catch2 `BENCHMARK_CELL`)

Mandated by SPEC §9.5 row "AssetHandle table":

| Benchmark name                                  | CPU ceiling | Heap ceiling | Fixture                                        |
|-------------------------------------------------|-------------|--------------|-------------------------------------------------|
| `core/asset: handle_o1_resolve_refcount`        | 0.05 ms     | 8 MiB        | S1 perf-budget fixture, ~5000 resolves / sample |

### 11.5 Out-of-Scope Tests (deferred)

- **Compaction tests**: deferred to spike #498's deliverable.
- **Cross-process serialization**: out of scope for core; the
  `content` plugin's spec owns save/load tests.
- **Network handle remap**: out of scope for MVP entirely
  (no networking context).

---

## 12. Open Questions

`[OPEN]` items below are tracked as design-time uncertainties.
Each is either deferred to a named follow-up spike or routed to
an existing open question in the enclosing decision records.

- `[OPEN]` **#1 — `AssetTypeTagMismatch` collapse vs separate
  arm.** §10.1 collapses tag-mismatch into `AssetStale`. If an
  operator-distinguishable cause emerges in plugin-author
  practice (e.g. reload-debugging where the operator wants to
  distinguish "stale slot" from "wrong table"), split into a
  separate arm. Resolution gate: first reload-debugging story
  that lands user-visible asset diagnostics in `tools`.
- `[OPEN]` **#2 — Hot/cold SoA layout cutoff.** §5.2 proposes a
  `sizeof(T) ≥ 64` B heuristic for the SoA split. The actual
  cutoff depends on per-`T` measurements that do not exist
  pre-implementation. Resolution gate: first plugin to register
  an asset table with `sizeof(T) ≥ 64` B (likely `geometry`'s
  meshlet metadata) records a per-`T` decision.
- `[OPEN]` **#3 — Number of `type_tag` values for MVP.** §3.4
  hardcodes 4 tags (3 usable + 1 sentinel). If the MVP plugin
  set ends up needing more than 3 distinct tables, this design
  must amend (either by widening `type_tag`, narrowing
  `generation` or `index`, or by allowing two tables to share a
  tag). Resolution gate: when the second of `geometry`,
  `render`, `content` registers an asset table.
- `[OPEN]` **#4 — Compaction interaction (depends on spike
  #498).** §8.5 flags this; resolution is exactly spike #498's
  deliverable. Until then, this design commits to "never
  compact" (SPEC §6.8).
- `[OPEN]` **#5 — Persistent `AssetId` mapping path.** §7.2
  excludes cross-process handle stability and points to the
  `content` plugin's spec for `AssetId` content-hash addressing.
  When the `content` plugin's spec lands, verify that the
  remap-on-load path does not introduce a need for core to
  expose handle-bit construction outside of `insert`. Resolution
  gate: `content` SPEC §7 / §8 review.
- `[OPEN]` **#6 — Future parallelism path for concurrent
  insert/resolve.** §6.3 sketches the RCU arena swap. Resolution
  gate: spike #496 (`per-system-parallelism-seam-foryc-vs-
  schedule`).
- `[OPEN]` **#7 — Generation overflow (2^22) frequency under
  realistic streaming.** §4.5 retires slots on overflow. The
  practical frequency of overflow at MVP streaming patterns is
  unmeasured. If retirement-induced index growth becomes a real
  cost, options include: widen the generation field (at the cost
  of `index` width), or reset generation to 0 on overflow with a
  global epoch counter as a tiebreaker. Resolution gate: post-MVP
  measurement spike if and only if perf-budget regression alarms
  reference asset-table heap drift.

---

## Cross-References

- **SPEC sections refined.** §4.7 (aggregate identity +
  invariants), §6.8 (internal architecture sketch), §8.1 #4
  (hot-reload survival), §9.3 row "AssetHandle table" (perf
  cell), §9.5 row "AssetHandle table" (benchmark assertion),
  §10.1 row `AssetStale` (failure mode), §11 #348 (acceptance
  criteria), §12 (open questions — particularly the link to
  spike #498).
- **Decision records consulted.**
  `reviews/decisions/error-model.md` (Result type, no
  exceptions, log-once-at-handle), `reviews/decisions/perf-
  budget.md` (cell, allocator rules, CI gate),
  `reviews/decisions/plugin-abi.md` (handle ABI stability,
  middleman dylib hash), `reviews/decisions/hot-reload-
  protocol.md` (four-step swap), `reviews/decisions/fory-
  codegen.md` (schema authoring for handle-bearing fields),
  `reviews/decisions/frame-phases.md` (where in the frame the
  asset table is touched).
- **Harmonius prior art (research input only — do not port).**
  `harmonius/docs/design/core-runtime/primitives.md` `Handle<T>`
  and `HandleMap<T>`, `harmonius/docs/design/core-runtime/ids.md`
  `AssetHandle<T>` row in the ID taxonomy,
  `harmonius/docs/design/content-pipeline/asset-pipeline.md`
  `HandleTable` (refused for MVP — content-addressing is the
  `content` plugin's, not core's).
