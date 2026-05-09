# `core::World` — Detailed Design

> Per-aggregate detailed design fleshing out `specs/core/SPEC.md` §4.1,
> §4.2, §4.3, §4.8, §4.9 (the `World` aggregate and the value
> objects it owns) plus §5.5 / §5.10 (the public surface). Authoritative
> for the `world/` and `type-registry/` sub-modules of `core` (SPEC §6.1
> rows). Does **not** widen scope into `Schedule` (§4.4 — sibling spike
> #701), `PluginLoader` (§4.5 — sibling spike #702), `HotReloadBarrier`
> (§4.6 — sibling spike #703), `AssetHandle` (§4.7 — sibling spike #704),
> or `FrameLoop` (§4.4 — sibling spike #705).
>
> Mandated section list per spike #700: Purpose, Requirements coverage,
> Detailed model, Public surface, Hot/cold path split, Concurrency,
> Persistence + ABI, Hot-reload, Performance, Failure modes, Test plan,
> Open questions.

## 1. Purpose

The `World` aggregate is the codegen-driven archetype ECS at the heart
of `core`. It owns the storage shape (archetype tables of cache-aligned
SoA chunks), the entity addressing scheme (64-bit generational handles
into a slot map), the immutable-after-init type-descriptor lookup
(`TypeRegistry`), the resource map (typed singletons keyed by
`TypeId`), the change-tick clock that backs `Changed<T>` filters, and
the deferred-mutation primitive (per-system `CommandBuffer`s replayed
in deterministic order at sync points). Queries — `With` / `Without` /
`Changed` term sets — compile to opaque `Query*` handles that bind
once at registration time and re-iterate per frame without re-matching
archetype membership; the matching itself is bloom-filter-rejection
plus pre-resolved column-offset arithmetic emitted by `glibre-foryc`
into the middleman dylib.

`World` refuses to own anything outside that boundary. It refuses
runtime type registration (PHILOSOPHY §6); refuses runtime reflection
(no `path.Has<T>()` lookup, no `DynamicValue`, no `Reflect` trait);
refuses scene-hierarchy semantics beyond the bare `ChildOf` forest
needed for SPEC §4.1 invariant 2 (acyclicity for the phase-5 transform
walk that lives in a sibling design); refuses entity-targeted event
routing with capture/bubble propagation (the lifecycle hook *shape* —
`OnAdd` / `OnRemove` / `OnSet` callbacks at storage-mutation point —
stays in `core`; the multi-term-query observer evaluator is routed to
the future `events` plugin per SPEC §3.3); refuses plugin groups,
capability brokering, and graph runtimes; refuses I/O and GPU work in
toto. Anything that two collapsing ECS-flavoured requirements would
otherwise demand collapses into one of the seven primitives `World`
already owns (entity allocator, archetype storage, resource map,
change-tick clock, type registry, command buffer, query plan), or it
is refused with rationale in §2 below.

The single reason `World` would change is the rules of archetype-row
addressing, generation counting, change-tick semantics, or the
codegen-driven storage layout that backs all of those. Any other
reason to change indicates a sibling aggregate is the right home for
the work.

## 2. Requirements Coverage

This section reconciles every harmonius `R-1.1.*` clause (the ECS
storage / entities / queries / change-detection / resources /
observers / command-buffers / worlds / relationships / change-detection
clauses; harmonius does not split these by aggregate) against the
glibre boundary. Every clause is either **covered** by this design,
**covered elsewhere** in `core` (sibling spike), or **refused** with
rationale and SPEC §3.3 routing target. Harmonius is research input
only (PHILOSOPHY §"How harmonius is used"); every conclusion below is
re-derived independently from PHILOSOPHY §1–§11, the engine-wide
decision records, and the SPEC §4 invariants.

The MVP-scope filter applied here is the SPEC §3.3 refusal table plus
the §3.2 Occam collapses; clauses outside that filter are explicitly
refused below with the routing target the SPEC already records.

### 2.1 Storage clauses

| Harmonius clause | Disposition | Rationale / location |
|---|---|---|
| **R-1.1.1** archetype tables, SoA, chunked (16 KiB default, 8–64 KiB tunable) | **Covered** | §3.2 archetype storage. Chunk size is a fixed 16 KiB in MVP per SPEC §6.2; the 8–64 KiB tunable is `[OPEN]` in §12 and tracked by SPEC #494. |
| **R-1.1.2** ≥500 M reads/sec, 64-byte chunk alignment, ≤256 B per-archetype metadata | **Covered with reframed bound** | §9 rephrases the throughput target as the `BENCHMARK_CELL` `core/world: archetype_iteration_change_tick_scan` (≤0.20 ms steady-state for the S1 fixture, SPEC §9.5). 64-byte alignment is held by §3.2 invariant 3 (`alignof(Chunk) >= 64`). 256 B per-archetype-metadata bound is **refused as an MVP gate** — replaced by the §9 heap-cell budget (24 MiB total for `World`) which is the contract CI enforces. Per-archetype budgeting reintroduces a second source of truth; we fold it into the cell. |
| **R-1.1.3** sparse-set storage opt-in via `#[sparse]` | **Refused (deferred)** | Two storage shapes is a second reason to change, splitting `Archetype`. MVP collapses every component into archetype storage. Re-evaluate when at least one MVP component (post-physics, post-render) demonstrates ≥10× archetype churn under the S1 fixture. Tracked as `[OPEN]` in §12. |
| **R-1.1.4** archetype graph with O(1) cached edges for add/remove | **Covered** | §3.4 archetype-graph design. Edge cache is a `eastl::hash_map<(ArchetypeId, TypeId), ArchetypeId>` populated lazily on first add/remove transition; subsequent transitions are O(1) amortized. |

### 2.2 Component clauses

| Harmonius clause | Disposition | Rationale / location |
|---|---|---|
| **R-1.1.5** compile-time + dynamic component registration | **Refused (compile-time only)** | PHILOSOPHY §6 forbids runtime reflection in shipping; §3.2 collapse #3 (codegen-driven archetype layout owned end-to-end). All registrations come from `glibre-types.dylib` via per-plugin `glibre_plugin_register`; `TypeRegistry` is immutable-after-init within a session. The dynamic API is refused. |
| **R-1.1.6** zero-sized tag components | **Covered** | §3.2 row-stride arithmetic naturally accommodates a `0`-size column: codegen emits no per-row bytes, and the type's presence in the archetype `TypeId` set is the discriminant. `BENCHMARK_CELL` for §9 verifies zero per-row bytes. |
| **R-1.1.7** shared components stored once per chunk | **Refused (deferred)** | A second column shape is a second reason to change `Chunk`. MVP refuses; the optimisation is reintroducible additively when an MVP plugin demonstrates a use case (material IDs are render-side data). Tracked as `[OPEN]` in §12. |
| **R-1.1.8** `DynamicBuffer<T>` variable-length per-entity buffers | **Refused (deferred)** | Variable-length storage is a third column shape. Refused for the same reason as R-1.1.7. Plugin authors can model "buffer of T" as an `AssetHandle<T>` payload owned by a plugin's resolver, which is the existing primitive. |
| **R-1.1.9** `#[enableable]` toggle without structural change | **Refused (deferred)** | Toggle-bit-per-entity is a fourth column shape. Refused. The same effect is achievable today with a tag component (R-1.1.6); the cost is one archetype move per toggle. Re-evaluate if a real workload measures the move cost as load-bearing. |
| **R-1.1.10** lifecycle hooks `OnAdd` / `OnRemove` / `OnSet`, ≤50 ns dispatch overhead | **Covered (shape only)** | §3.6 emits `OnAdd` / `OnRemove` / `OnSet` callbacks at archetype-storage mutation point; SPEC §4.1 invariant 7 fixes the firing-order rule (deterministic insertion order, before subsequent queries observe the mutation in the same phase). Multi-term-query observers and entity-event propagation are refused (§3.3, R-1.1.31 / R-1.1.32) — those are the `events` plugin's. The 50 ns budget is rephrased as part of the §9 `World` cell. |
| **R-1.1.11** named bundles with `#[require]` companion auto-add | **Refused (out of MVP)** | Bundle composition is a higher-level abstraction over the existing `set_component` primitive; codegen-driven bundle macros belong in the future scripting/gameplay context, not in `core`. Plugin authors compose bundles client-side as a free-function helper. |

### 2.3 Entity clauses

| Harmonius clause | Disposition | Rationale / location |
|---|---|---|
| **R-1.1.12** 64-bit generational entity (32 idx + 32 gen), ≥4 M per world, O(1) alloc/dealloc/stale-detect, ≤100 ns per op | **Covered** | §3.3 entity-allocator design. SPEC §4.3 fixes the encoding. The 100 ns per-op figure is rephrased as the §9 `World` cell ceiling; spawn/despawn benchmark lives in §11 below. |
| **R-1.1.13** `#[cleanup]` components persisting after destruction | **Refused (deferred)** | Two-phase destruction is a second deletion semantics. MVP uses one despawn primitive that increments the generation atomically and frees the row. Resource teardown is owned by the resolving plugin (GPU buffers via `render`, network sessions via a future `network` plugin), not by `core`. |
| **R-1.1.14** human-readable entity names + path lookup | **Refused** | Per SPEC §3.3 (R-1.1.14–R-1.1.17 routed to `scene` and `gameplay`). String-keyed lookup violates PHILOSOPHY §6 and §7 (deterministic iteration order). |

### 2.4 Relationships and hierarchies

| Harmonius clause | Disposition | Rationale / location |
|---|---|---|
| **R-1.1.15** (Relationship, Target) pairs packed into 64-bit component IDs, wildcard queries | **Refused** | Routed to `scene` per SPEC §3.3. `World` carries only the bare `ChildOf` forest for §4.1 invariant 2 (acyclicity for the phase-5 transform walk). |
| **R-1.1.16** relationship properties (Exclusive / Symmetric / Transitive / Acyclic / cleanup policies) | **Refused** | Routed to `scene`. `core` enforces only acyclicity on `ChildOf`. |
| **R-1.1.17** built-in `ChildOf` with cascade and 256-level depth | **Covered (shape only)** | §3.5 `ChildOf` forest invariant. Cascade-delete is `scene`'s. Depth bound is the recursion-depth defence in §3.5. |

### 2.5 Query clauses

| Harmonius clause | Disposition | Rationale / location |
|---|---|---|
| **R-1.1.18** composable queries with With/Without/Option/Changed/Added, cached after first eval | **Covered (subset)** | §3.7 query design. MVP supports `Reads` (T), `Writes` (T), `Without` (T), `Changed` (T) — the four terms the SPEC §5.6 `AccessSet` already names plus `Changed`. `Option` (T) and `Added` (T) are **refused for MVP**; `Option` is achievable via two queries union, `Added` is `Changed` against tick `0` of the entity's lifetime. Re-evaluate when a render or gameplay system measurably needs them. Cache invariant: an opaque `Query*` handle binds once at compile, and subsequent re-iterations within a frame match no archetypes (PHILOSOPHY §7 fixed iteration order). |
| **R-1.1.19** sort by component value, group by relationship target, stable cached sort | **Refused** | Sorting is a render/gameplay concern (draw-call batching). `core` exposes deterministic iteration order over the union of matching archetypes; sort post-iteration is the consumer's concern. |
| **R-1.1.20** query variables (`$parent`, `$target`) for graph pattern matching | **Refused** | Graph pattern matching belongs to `scene` / `scripting`. |
| **R-1.1.21** parallel-iteration partitioning at archetype/chunk granularity | **Refused for MVP (single-thread sim)** | SPEC §6.10: MVP runs every system on the game-loop driver thread. Per-system parallelism is post-MVP additive; the access-set machinery (§5.6 SystemDesc.access) is already in place to drop fork-join parallelism on without re-spec'ing the world boundary. Tracked as `[OPEN]` in §12 (SPEC #496). |

### 2.6 Aspects, change detection, resources

| Harmonius clause | Disposition | Rationale / location |
|---|---|---|
| **R-1.1.22** named aspect structs grouping a component subset for queries | **Refused** | Aspect-as-syntax-sugar belongs to a higher-level binding layer. `core` exposes `Query*` handles; the aspect derive lives in `scripting` / future codegen consumers. |
| **R-1.1.23** per-component mutation tracking at chunk granularity, ≤8 B per (component × chunk) | **Covered** | §3.8 change-detection design. Per-column `last_modified : ChangeTick` lives in the chunk header (8 bytes); incremented on each mutable access; `Changed<T>` filters compare against the current `World::current_tick()`. |
| **R-1.1.24** typed singleton resources via `Res` / `ResMut` | **Covered** | §3.9 resource map. `Resource<T>` is a `TypeId`-keyed slot in a per-world `eastl::hash_map<TypeId, ResourceSlot>`; access via `World::resource<T>()` / `World::resource_mut<T>()`. Scheduler integration is the sibling `Schedule` aggregate's concern (§4.4 invariant on access-set DAG). |
| **R-1.1.25** non-send resources auto-pinned to game-loop thread | **Covered (degenerate)** | MVP runs everything on the game-loop driver thread (SPEC §6.10), so every resource is trivially game-loop-pinned. The pin distinction reactivates when per-system parallelism lands; tracked as part of the parallelism `[OPEN]`. |

### 2.7 Scheduling clauses

R-1.1.26 — R-1.1.30 (system scheduling, phases, run criteria,
ambiguity detection, exclusive systems) are owned by the `Schedule` /
`Phase` / `FrameLoop` aggregate (SPEC §4.4) and live in sibling spike
#701 / #705. Refused here as out-of-scope for `World`.

### 2.8 Observer clauses

| Harmonius clause | Disposition | Rationale / location |
|---|---|---|
| **R-1.1.31** observers on `OnAdd` / `OnRemove` / `OnSet` / `OnTableCreate` / `OnTableEmpty` with multi-term-query matching, ≥1000 observers/world | **Refused (shape kept; evaluator routed)** | Per SPEC §3.3 / §3.2 collapse #7: the lifecycle hook *shape* (callback at storage-mutation point) stays in `core`; multi-term-query observers and the 1000-observer evaluator move to the `events` plugin. `OnTableCreate` / `OnTableEmpty` collapse into archetype-creation hooks that are out of MVP scope. |
| **R-1.1.32** user-defined entity events with relationship-edge propagation | **Refused** | Routed to `events` plugin. |

### 2.9 Command buffers

| Harmonius clause | Disposition | Rationale / location |
|---|---|---|
| **R-1.1.33** per-system command buffers, deterministic flush, ≤64 KiB typical | **Covered** | §3.10 `CommandBuffer` design. Per-system arena drains at next sync point in deterministic order (PHILOSOPHY §7). The 64 KiB figure is the MVP per-system arena cap; overflow refuses with `core::Error::CommandBufferOverflow` (SPEC §5.1). |
| **R-1.1.34** multiple worker threads recording into the same buffer with sort keys | **Refused for MVP (single-thread)** | Single-thread sim → one writer per buffer; no sort-key machinery needed. Re-evaluate alongside the parallelism `[OPEN]`. |

### 2.10 World clauses

| Harmonius clause | Disposition | Rationale / location |
|---|---|---|
| **R-1.1.35** multiple worlds with flags (Game/Editor/Server/Shadow) | **Refused (deferred)** | SPEC §3.3. MVP runs exactly one `World`. Multi-world reactivates when a second use case (rollback netcode, editor preview) actually exists. |
| **R-1.1.36** entity migration between worlds, ≤10 µs/entity | **Refused** | Same reason as R-1.1.35. The `EntityForeignWorld` arm (SPEC §5.1) reserves the refusal contract for when multi-world lands. |

### 2.11 Templates and state machines

R-1.1.37 — R-1.1.39 (entity templates / IsA inheritance / state
machines) — refused per SPEC §3.3, routed to `gameplay` / `scripting`.
The lifecycle-hook *shape* `core` already owns is the primitive those
contexts compose over.

### 2.12 AoSoA tiling

| Harmonius clause | Disposition | Rationale / location |
|---|---|---|
| **R-1.1.40** AoSoA tiled chunk layout matching SIMD width (4 SSE/NEON, 8 AVX2) | **Refused (deferred)** | Per SPEC §3.2 collapse #3, `glibre` runs codegen end-to-end and could emit AoSoA columns; the SPEC chooses plain SoA for MVP because (a) `core` owns no SIMD-bound workloads (the dominant SIMD path is render's mesh-shader extract, owned by `render`), (b) plain SoA already saturates the §9 cell ceiling, and (c) AoSoA reintroduces a per-platform layout codepath that contradicts PHILOSOPHY §7 (fixed iteration order across hosts). Re-evaluate if a `core`-owned phase-5 transform walk measures as SIMD-bound under S1. |

### 2.13 Compiled query plans

| Harmonius clause | Disposition | Rationale / location |
|---|---|---|
| **R-1.1.41** compiled query plans with bloom-filter archetype rejection, pre-resolved column offsets, prefetch hints; incremental update on new archetypes | **Covered (subset)** | §3.7 query design. Bloom-filter rejection is emitted by `glibre-foryc` into the middleman. Pre-resolved column offsets fall out of the codegen-driven layout (the offset for `T` in archetype `A` is a compile-time constant per (`A.shape`, `T`)). **Prefetch hints are refused for MVP** (the hot loop already saturates L1 on the S1 fixture; manual prefetch is platform-specific and contradicts PHILOSOPHY §7). Incremental plan update on new archetypes lands as `Query::on_new_archetype(...)` which appends-to-bloom and re-checks; never a full recompile (PHILOSOPHY §7). |

### 2.14 Coverage summary

Of the 41 harmonius `R-1.1.*` clauses:

- **17 covered** as `World`-aggregate design here (R-1.1.1, .2, .4, .6,
  .10 shape, .12, .17 shape, .18 subset, .23, .24, .25 degenerate,
  .33, .41 subset).
- **5 owned by sibling aggregates** in `core` (R-1.1.26 — R-1.1.30 →
  Schedule, sibling spike #701).
- **19 refused with rationale** — 11 deferred post-MVP (R-1.1.3, .7,
  .8, .9, .13, .19, .21, .34, .35, .36, .40), 8 routed elsewhere
  (R-1.1.5, .11, .14, .15, .16, .20, .22, .31, .32, .37, .38, .39).

This list is closed; PRs adding any of the refused clauses to `core`
should be rejected and routed to the listed owner.

## 3. Detailed Model

The model below is the implementer's authority for the `world/`
sub-module (SPEC §6.1 row "world/"). Each subsection owns one of the
seven primitives `World` composes; the SPEC §4.1 invariants the
primitive enforces are cited per subsection.

### 3.1 Composition and module boundary

```text
core/src/world/                          (private headers; not on plugin include path)
├── archetype.{hpp,cpp}                  (§3.2  ─ Archetype, Chunk, ColumnDescriptor)
├── archetype_graph.{hpp,cpp}            (§3.4  ─ Edge cache for add/remove transitions)
├── entity_allocator.{hpp,cpp}           (§3.3  ─ Slot map; generational handles)
├── childof_forest.{hpp,cpp}             (§3.5  ─ ChildOf relationship index)
├── lifecycle_hooks.{hpp,cpp}            (§3.6  ─ OnAdd/OnRemove/OnSet shape)
├── query.{hpp,cpp}                      (§3.7  ─ QueryDesc compile, Query iter)
├── change_tick.{hpp,cpp}                (§3.8  ─ World::current_tick clock)
├── resource.{hpp,cpp}                   (§3.9  ─ Typed singleton map)
├── command_buffer.{hpp,cpp}             (§3.10 ─ Per-system deferred mutation log)
└── world.{hpp,cpp}                      (façade; the §5.5 public surface)

core/src/type-registry/                   (private headers; sibling sub-module)
├── type_registry.{hpp,cpp}              (§3.11 ─ TypeId → ColumnDescriptor lookup)
└── type_id.{hpp}                        (TypeId value object)
```

`world.hpp` is the only header outside `core/src/world/` permitted to
include `archetype.hpp` / `entity_allocator.hpp` / etc. The CMake
visibility rule cited by SPEC §6.1 enforces this.

### 3.2 Archetype storage (§4.1 inv. 1, §4.2 inv. 1–5; covers R-1.1.1, R-1.1.2)

An `Archetype` is the unordered set of `TypeId`s defining one storage
table; equal sets share one `Archetype`. The internal record:

```cpp
// core/src/world/archetype.hpp — internal.
namespace glibre::core::detail {

struct ColumnDescriptor {
    TypeId       type_id;
    std::uint32_t size;        // bytes per row, mirrored from TypeRegistry
    std::uint16_t align;       // alignof(T)
    std::uint16_t offset;      // byte offset within Chunk::storage for col 0 row 0
    void (*move_construct)(std::byte* dst, std::byte* src) noexcept;
    void (*destruct)      (std::byte* dst)                 noexcept;
};

class alignas(64) Chunk {                 // 64-byte alignment per R-1.1.2
public:
    static constexpr std::size_t kBytes = 16 * 1024;   // §6.2 fixed; tunable [OPEN]

    // Hot fields (read every iteration step):
    std::uint32_t row_count;
    std::uint32_t row_capacity;
    eastl::array<ChangeTick, /*column count*/ MaxColumnsPerChunk> last_modified;

    // Cold (read only on resize / archetype move):
    std::byte storage[kBytes - /*hot header size*/];
};

class Archetype {
public:
    ArchetypeId                          id;
    eastl::span<const TypeId>            sorted_type_ids;          // canonical
    eastl::vector<ColumnDescriptor,      PerContextAllocator<...>> columns;
    eastl::vector<eastl::unique_ptr<Chunk>, PerContextAllocator<...>> chunks;
    eastl::vector<Entity,                PerContextAllocator<...>> row_to_entity;
    std::uint32_t                        first_nonfull_chunk_index; // hot
};

}  // namespace
```

Invariants (mapped one-to-one to SPEC §4.2):

1. **Component-set immutability.** `Archetype::sorted_type_ids` is
   set at archetype creation by `glibre-foryc`-emitted shape-table
   lookup; a public-API call that adds or removes a component on an
   entity moves the row to a different `Archetype` (per §3.4 graph
   transitions), never mutates the source archetype's set.
2. **Chunk capacity bound.** `row_count <= row_capacity` always;
   `row_capacity = floor((kBytes - sizeof(hot_header)) / row_stride)`
   where `row_stride = Σ aligned(column.size, column.align)`. The
   capacity is computed once at archetype creation and never changes.
3. **SoA column alignment.** Each column's `offset` is computed as
   the running prefix sum of preceding-column aligned strides; codegen
   asserts at build time that `offset % align == 0` for every
   `(archetype shape, column index)` pair. Mismatch fails the build.
4. **Row density.** Removing a row swap-removes the chunk's last row
   into the freed slot. `row_to_entity[freed_row]` is updated and the
   reverse map (`Entity → (Archetype, Chunk, Row)` in the
   `EntityAllocator`) is patched in the same critical section.
5. **Entity-row address validity.** Every public-boundary entry / exit
   the forward and reverse maps agree (cross-checked by debug-build
   asserts in `world.cpp`).

The `Archetype::first_nonfull_chunk_index` cursor (hot field) avoids
an O(chunk count) scan on `set_component` insertion paths; it advances
when the cursor's chunk fills and rewinds on deletion.

### 3.3 Entity allocator (§4.1 inv. 1, §4.3 inv. 1–3; covers R-1.1.12)

A slot-map keyed by 32-bit index, generation-tagged per slot:

```cpp
// core/src/world/entity_allocator.hpp — internal.
namespace glibre::core::detail {

struct EntitySlot {
    ArchetypeId   archetype_id;     // hot (resolution path)
    std::uint32_t chunk_index;      // hot
    std::uint32_t row;              // hot
    std::uint32_t generation;       // hot (validity check)
    bool          live;             // cold (debug only)
};

class EntityAllocator {
public:
    // Hot path:
    [[nodiscard]] Result<Entity> allocate() noexcept;
    [[nodiscard]] Result<void>   deallocate(Entity e) noexcept;
    [[nodiscard]] bool           is_alive(Entity e)   const noexcept;
    [[nodiscard]] Result<EntitySlot const*>
                                 resolve(Entity e)    const noexcept;

private:
    eastl::vector<EntitySlot, PerContextAllocator<...>> slots_;     // hot
    eastl::vector<std::uint32_t, PerContextAllocator<...>> free_;   // cold
    std::uint64_t                                       live_count_; // cold
};

}  // namespace
```

`Entity::bits` is `(generation << 32) | index` (network-byte-order
unspecified — bytes are local to the process). The allocator never
shrinks `slots_`; freed indices are reused via `free_` (LIFO so the
freshest cache lines are reused first). On deallocate the slot's
`generation` increments **before** the index returns to `free_`, so
any retained `Entity` value resolves to `EntityStale` on next
`resolve` (SPEC §5.1, §10.1).

The 4 M-entities-per-world figure from R-1.1.12 is a soft target: the
32-bit index supports 4 G slots; the §9 heap budget caps `slots_` at
~2 M live entries (one slot is 24 B; 24 B × 2 M = 48 MiB which would
exceed `World`'s 24 MiB row of the §9 cell, so the practical MVP
ceiling is closer to 1 M live entities — sufficient for S1 and §11
test fixtures).

### 3.4 Archetype graph (§4.1 inv. 1; covers R-1.1.4)

When an entity adds or removes a component, its row moves from
`Archetype A` (set `S`) to `Archetype B` (set `S ∪ {T}` or
`S \ {T}`). A direct edge cache makes the second-and-onwards
transition O(1) amortized:

```cpp
// core/src/world/archetype_graph.hpp — internal.
namespace glibre::core::detail {

struct EdgeKey {
    ArchetypeId source;
    TypeId      delta;
    bool        is_add;
};

class ArchetypeGraph {
public:
    [[nodiscard]] Result<ArchetypeId>
    resolve(ArchetypeId source, TypeId delta, bool is_add) noexcept;

private:
    eastl::hash_map<EdgeKey, ArchetypeId, EdgeKeyHash,
                    eastl::equal_to<EdgeKey>,
                    PerContextAllocator<...>> edges_;
};

}  // namespace
```

The first traversal of an edge computes the destination archetype
(canonical-sort `sorted_type_ids ± {T}`, look up or create in the
archetype table) and inserts the result into `edges_`. Repeated
traversals are a single hash lookup. Entries are never evicted within
a session.

### 3.5 ChildOf forest (§4.1 inv. 2; covers R-1.1.17 shape only)

`ChildOf` is the only relationship `core` knows about. The full pair-
based relationship system is refused (§2.4); this forest is the bare
minimum the phase-5 `LocalTransform → GlobalTransform` walk (SPEC §4.4
phase ownership) needs. `ChildOf` is materialised as a `(child, parent)`
column pair on the entity's archetype — i.e. it is *not* a separate
data structure but an ordinary component `core` knows by `TypeId`.

The acyclicity invariant (§4.1 inv. 2) is enforced at the
`set_component(child, ChildOfTypeId, parent_bytes)` boundary by a
walk-up from `parent` back to root; if `child` appears in the chain,
the call refuses with `core::Error::HierarchyCycle` (SPEC §5.1).

The 256-level depth bound from R-1.1.17 is enforced by capping the
walk-up at 256 iterations and treating overflow as a cycle (the most
likely cause of >256 ancestor count is a bug). This is a debug-build
loud-fail and a release-build refusal-with-`HierarchyCycle`.

### 3.6 Lifecycle hooks (§4.1 inv. 7; covers R-1.1.10 shape only)

Per-`TypeId` callbacks fire at storage-mutation point. The shape:

```cpp
// core/src/world/lifecycle_hooks.hpp — internal.
namespace glibre::core::detail {

using OnAddFn    = void (*)(World& w, Entity e, std::span<const std::byte> bytes) noexcept;
using OnRemoveFn = void (*)(World& w, Entity e, std::span<const std::byte> bytes) noexcept;
using OnSetFn    = void (*)(World& w, Entity e, std::span<const std::byte> bytes) noexcept;

struct LifecycleHooks {
    OnAddFn    on_add{nullptr};
    OnRemoveFn on_remove{nullptr};
    OnSetFn    on_set{nullptr};
};

class HookTable {
public:
    void              register_hooks(TypeId t, LifecycleHooks h) noexcept;
    LifecycleHooks    lookup        (TypeId t)              const noexcept;
private:
    eastl::vector<LifecycleHooks,
                  PerContextAllocator<...>> by_type_id_;     // dense; one slot per registered TypeId
};

}  // namespace
```

The table is populated during plugin `glibre_plugin_register` (per
the §5.9 `PluginContext` sketch) and stays immutable for the
plugin's lifetime. A hot-reload swap (SPEC §8) replaces the
function-pointer slots atomically with the incoming plugin's
versions; this is exactly the same vtable swap as for systems
(SPEC §8.4 step 3).

Firing order (§4.1 inv. 7): on `set_component(e, t, bytes)` the
sequence is **(1) store bytes → (2) bump tick → (3) fire `OnSet` →
(4) return**; no observer sees an inconsistent intermediate. Multi-
term-query observer evaluation is the `events` plugin's concern —
`core` only fires the per-`TypeId` hook.

### 3.7 Queries (§4.1 inv. 1; covers R-1.1.18 subset, R-1.1.41 subset)

A query is a compiled descriptor. `QueryDesc` is the builder input;
`Query` is the opaque handle returned from `World::compile_query`:

```cpp
// core/src/world/query.hpp — internal.
namespace glibre::core::detail {

struct QueryDesc {
    eastl::span<const TypeId> reads;
    eastl::span<const TypeId> writes;
    eastl::span<const TypeId> without;
    eastl::span<const TypeId> changed;     // Changed<T> filter
    ChangeTick                changed_since{0};   // for Changed; 0 = always-fire
};

class Query {
public:
    [[nodiscard]] QueryIter iter(World& w) noexcept;             // mutable view
    [[nodiscard]] QueryIter iter(World const& w) const noexcept; // read-only view

    // Append a newly-created archetype to the matched set if it
    // satisfies the term constraints. Called by World::create_archetype
    // after the new archetype is materialised; no full recompile.
    void on_new_archetype(ArchetypeId a) noexcept;

private:
    QueryDesc                 desc_;                     // copies of inputs
    eastl::vector<ArchetypeId,
                  PerContextAllocator<...>> matched_;
    BloomFilter               type_filter_;              // 64-bit bloom over read+write set
    BloomFilter               without_filter_;
};

}  // namespace
```

Compile-time work happens inside `World::compile_query`:

1. Compute the bloom filter over `reads ∪ writes ∪ changed` and
   the negative bloom over `without`.
2. Walk the archetype table (typically <256 archetypes per MVP
   world); for each archetype `a`, accept iff
   `bloom_subset(a.types) ⊇ desc_.reads ∪ writes ∪ changed` and
   `bloom_disjoint(a.types, without)`. Bloom-filter rejection is the
   first pass; archetypes that pass the bloom are confirmed by exact
   set comparison.
3. Append every accepted `ArchetypeId` to `matched_`.

Iteration walks `matched_` in `ArchetypeId` order (deterministic per
PHILOSOPHY §7); within an archetype, walks chunks in chunk-index
order (also deterministic); within a chunk, walks rows
`0..row_count`. The pre-resolved column-offset arithmetic is a
constant per (archetype, query.column-index) pair, populated when an
archetype is appended to `matched_`; iteration does no per-row hash
lookup.

`Changed<T>` semantics: a chunk's `last_modified[col]` is compared
against `desc_.changed_since` at chunk-entry; if `last_modified <=
changed_since` the chunk is skipped wholesale (R-1.1.23 chunk-grain
change detection). This is the same primitive that drives reactive
patterns; the `events` plugin layers per-row reactivity on top.

Cache-invariant: a `Query*` handle, once compiled, never matches new
archetypes implicitly — `World::create_archetype` calls
`Query::on_new_archetype` for every live query at archetype-creation
time. The cost of new-archetype creation grows linearly in the live-
query count, which is fine for MVP (live-query count is bounded by
plugin-registered systems; ~hundreds in MVP).

### 3.8 Change-tick clock (§4.1 inv. 4; covers R-1.1.23)

`ChangeTick` is a `std::uint64_t` advanced exclusively at:

- `Phase::Present` (phase 9) entry — the per-frame increment.
- Each mutable archetype-storage write — the per-mutation tag.

```cpp
// core/src/world/change_tick.hpp — internal.
class ChangeTickClock {
public:
    [[nodiscard]] ChangeTick current() const noexcept { return tick_; }

    // Called by FrameLoop at phase 9 entry.
    void advance_frame() noexcept { tick_.value++; }

    // Called by archetype mutation paths (set_component, despawn-with-OnRemove).
    [[nodiscard]] ChangeTick stamp_mutation() noexcept { return ++tick_; }

private:
    ChangeTick tick_{1};   // 0 reserved for "never changed"
};
```

The clock never decreases within a process lifetime (SPEC §4.1
inv. 4); a `u64` at 60 fps with one increment per frame plus ~1k
mutations/frame takes ~10⁵ years to overflow. Hot-reload preserves
the tick (SPEC §8.1 #6).

### 3.9 Resource map (§4.1 inv. 6; covers R-1.1.24)

Typed singletons keyed by `TypeId`. The map is per-`World`:

```cpp
// core/src/world/resource.hpp — internal.
struct ResourceSlot {
    eastl::vector<std::byte, PerContextAllocator<...>> bytes;
    ChangeTick                                         last_modified;
};

class ResourceMap {
public:
    [[nodiscard]] Result<void>
    insert(TypeId t, std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] Result<std::span<const std::byte>>
    get(TypeId t) const noexcept;

    [[nodiscard]] Result<std::span<std::byte>>
    get_mut(TypeId t, ChangeTickClock& clock) noexcept;     // bumps tick

    [[nodiscard]] Result<void>
    remove(TypeId t) noexcept;

private:
    eastl::hash_map<TypeId, ResourceSlot,
                    TypeIdHash, eastl::equal_to<TypeId>,
                    PerContextAllocator<...>> slots_;
};
```

Resource access integrates with `Schedule`'s access-set DAG (sibling
spike #701): a system declares `ResMut<T>` and the schedule treats
the resource as a writer; concurrent read+write within one phase is
refused at compile time (SPEC §4.4 inv. 2).

### 3.10 CommandBuffer (§4.8 inv. 1–5; covers R-1.1.33)

Per-system, per-frame, append-only. Records intended mutations as
fixed-size 32-byte `Command` records into a per-system arena
(`PerContextAllocator` transient pool, drained at phase 9 per SPEC
§9.4 rule 4):

```cpp
// core/src/world/command_buffer.hpp — internal.
enum class CommandKind : std::uint8_t {
    Spawn, Despawn, AddComponent, RemoveComponent, SetComponent,
};

struct Command {
    CommandKind   kind;
    Entity        entity;             // for Spawn, populated on flush
    TypeId        type;               // for AddComponent / RemoveComponent / SetComponent
    std::uint32_t bytes_offset;       // into the buffer's payload arena
    std::uint32_t bytes_size;         // for AddComponent / SetComponent
};
static_assert(sizeof(Command) == 32);

class CommandBuffer {
public:
    [[nodiscard]] Result<Entity> spawn() noexcept;                 // reserves a generational slot up-front
    [[nodiscard]] Result<void>   despawn(Entity e) noexcept;
    [[nodiscard]] Result<void>   add_component   (Entity, TypeId, std::span<const std::byte>) noexcept;
    [[nodiscard]] Result<void>   remove_component(Entity, TypeId) noexcept;
    [[nodiscard]] Result<void>   set_component   (Entity, TypeId, std::span<const std::byte>) noexcept;

    [[nodiscard]] Result<void>   flush(World& w) noexcept;          // called by Schedule at sync points
    void                         clear() noexcept;
    [[nodiscard]] std::size_t    recorded_count() const noexcept;

private:
    eastl::vector<Command,    PerContextAllocator<...>> commands_;
    eastl::vector<std::byte,  PerContextAllocator<...>> payload_;
    std::size_t                                         arena_cap_;   // §9 budget
};
```

Replay order at the sync point (SPEC §4.8 inv. 2): the schedule walks
all systems in lexicographic order of fully-qualified name (PHILOSOPHY
§7); for each system, replays its buffer's commands in insertion
order; entities reserved by `spawn` are committed at this point.

A buffer that exceeds its arena cap refuses further appends with
`core::Error::CommandBufferOverflow` (SPEC §5.1, §10.1). The cap is
declared by §9 below.

### 3.11 TypeRegistry (§4.9 inv. 1–4)

Immutable-after-init lookup from `TypeId` to type descriptor:

```cpp
// core/src/type-registry/type_registry.hpp — internal.
struct TypeDescriptor {
    std::uint32_t size;
    std::uint16_t align;
    void (*destruct)(std::byte*) noexcept;
    void (*move_construct)(std::byte* dst, std::byte* src) noexcept;
};

class TypeRegistry {
public:
    [[nodiscard]] Result<void> register_type(TypeId t, TypeDescriptor d) noexcept;
    void                       seal() noexcept;                                 // called by World::create
    [[nodiscard]] Result<TypeDescriptor> get(TypeId t) const noexcept;
    [[nodiscard]] bool         is_sealed() const noexcept;

private:
    eastl::vector<TypeDescriptor,
                  PerContextAllocator<...>> by_type_id_;     // dense
    eastl::hash_map<TypeId, std::uint32_t,
                    TypeIdHash, eastl::equal_to<TypeId>,
                    PerContextAllocator<...>> index_;
    bool sealed_{false};
};
```

`register_type` calls fail with `core::Error::TypeRegistryClosed`
once `seal()` has run. Hot-reload (SPEC §8) is permitted to extend
the registry **only for new `TypeId`s** (per SPEC §8.4 step 4 type-
registry append); existing entries are immutable.

### 3.12 World façade

The aggregate root composes the eleven primitives above through one
public façade (`world.hpp`, the §5.5 stub). Construction:

```cpp
class World {
public:
    static Result<World*> create() noexcept;
    static void           destroy(World* w) noexcept;

private:
    detail::TypeRegistry          types_;          // sealed at create()
    detail::EntityAllocator       entities_;
    detail::ArchetypeGraph        graph_;
    eastl::vector<detail::Archetype,
                  PerContextAllocator<...>> archetypes_;
    detail::HookTable             hooks_;
    detail::ChangeTickClock       clock_;
    detail::ResourceMap           resources_;
    eastl::vector<detail::Query*,
                  PerContextAllocator<...>> live_queries_;   // for on_new_archetype fanout
    eastl::vector<detail::CommandBuffer*,
                  PerContextAllocator<...>> command_buffers_; // owned by Schedule but referenced here for flush dispatch
};
```

`create()` returns a sealed registry, an empty archetype table (the
empty `Archetype` `[]` is materialised so spawn returns rows in the
zero-component archetype), and a clock at tick 1.

## 4. Public Surface

`World`'s public surface is exactly the §5.5 stub from SPEC plus the
§5.10 `CommandBuffer` stub. Reproduced here with the function-by-
function semantics this design adds. Every signature below is from
SPEC §5.5 / §5.10 verbatim — this design freezes intent, not shape.

```cpp
// SPEC §5.5 — World public façade.
class World {
public:
    static Result<World*> create() noexcept;
    static void           destroy(World* w) noexcept;
    World(const World&)            = delete;
    World& operator=(const World&) = delete;

    [[nodiscard]] Result<Entity> spawn() noexcept;
    [[nodiscard]] Result<void>   despawn(Entity e) noexcept;
    [[nodiscard]] bool           is_alive(Entity e) const noexcept;

    [[nodiscard]] Result<void>
    set_component(Entity e, TypeId t, std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] Result<std::span<const std::byte>>
    get_component(Entity e, TypeId t) const noexcept;

    [[nodiscard]] Result<void>
    remove_component(Entity e, TypeId t) noexcept;

    [[nodiscard]] ChangeTick current_tick() const noexcept;

    [[nodiscard]] Result<ChangeTick>
    last_change_tick(Entity e, TypeId t) const noexcept;

    [[nodiscard]] Result<Query*> compile_query(const QueryDesc& desc) noexcept;

    [[nodiscard]] const TypeRegistry& type_registry() const noexcept;
};
```

Per-call semantics:

- **`create() / destroy()`** — `create()` allocates the world under
  the `core` `PerContextAllocator` tag, runs the static-init
  registration of every `glibre-types.dylib` type (the `_registry.cpp`
  TU emits a `__attribute__((constructor))` trampoline that calls
  `TypeRegistry::register_type` for every codegen-emitted type), then
  calls `seal()`. After `create()` no further type registrations
  succeed; `TypeRegistryClosed` is emitted otherwise. `destroy()`
  unwinds in reverse order (queries → archetypes → entities → types);
  the call is `noexcept` and frees the world's heap budget back to
  `PerContextAllocator`.
- **`spawn()`** — allocates a slot from the entity allocator, places
  the row in the empty archetype `[]`, fires no hooks (no components
  added), returns the `Entity`. Errors: `OutOfBudget` if the slot
  array would exceed the §9 cell.
- **`despawn(e)`** — validates `e`'s generation (`EntityStale` on
  miss); fires `OnRemove` for every component the entity holds, in
  archetype-column order; swap-removes the row; bumps the slot
  generation. Errors: `EntityStale`.
- **`is_alive(e)`** — pure predicate; `false` for stale or never-
  allocated handles. No error path; the function returns `bool`.
- **`set_component(e, t, bytes)`** — validates `e`; validates `t`
  (`TypeUnregistered` if absent from the sealed registry); resolves
  whether the entity already has `t` (if so, in-place overwrite +
  `OnSet`; if not, archetype graph traversal to add `t` + `OnAdd`).
  Bumps the archetype's `chunk.last_modified[col]` and the world
  clock. Errors: `EntityStale`, `TypeUnregistered`, `HierarchyCycle`
  (when `t == ChildOfTypeId` and `bytes` denotes a parent that would
  cycle), `OutOfBudget` (allocation exceeded the cell).
- **`get_component(e, t)` / `last_change_tick(e, t)`** — read-only;
  `EntityStale` / `TypeUnregistered` are the only failures. The
  returned span borrows the archetype's chunk storage; valid until
  the next mutating call on the same world. Callers MUST NOT retain
  it across phase boundaries.
- **`remove_component(e, t)`** — fires `OnRemove`; archetype graph
  traversal to remove `t`; row swap-remove. Errors: `EntityStale`,
  `TypeUnregistered`.
- **`current_tick()`** — pure read of the clock. No errors.
- **`compile_query(desc)`** — validates every `TypeId` in `desc`
  against the sealed registry (`TypeUnregistered` on miss); emits a
  `Query*` per §3.7. The query is owned by the world; lifetime is
  the world's. Errors: `TypeUnregistered`, `OutOfBudget`.
- **`type_registry()`** — borrows the sealed registry. No errors.

`CommandBuffer` (SPEC §5.10) signatures are reproduced verbatim from
the SPEC; semantics are §3.10 above.

Public surface invariants (every function above):

1. **`std::expected<T, glibre::Error>` only**, no exceptions across
   the ABI seam (PHILOSOPHY §1, error-model.md decision).
2. **`noexcept`** on every function; an exception escaping a `World`
   member is undefined behaviour (the editor-UI carve-out from
   error-model.md does not apply to `core`).
3. **No runtime reflection** — every `TypeId` is a codegen-emitted
   stable integer; the public surface never accepts a string-keyed
   type lookup (PHILOSOPHY §6).
4. **No `std::*` containers cross the ABI**; the surface uses only
   `std::span`, `std::expected`, primitive scalars, `Entity`,
   `TypeId`, `ChangeTick` (per PHILOSOPHY §11 the public surface is
   `eastl::`-free at the boundary too — the spans of bytes are POD).
5. **Determinism**: equal call sequences from a given starting
   world-state produce byte-equal world-state outcomes (PHILOSOPHY
   §7). Tested in §11.

## 5. Hot/Cold Path Split

The `World` aggregate is iterated once per phase per system; the
critical hot path is `Query::iter` (§3.7) over the matched-archetype
set. Field placement is annotated against this loop.

### 5.1 Hot fields (touched per-frame, inner loops)

Every field below appears on the path from `Query::iter` through
`Chunk` row iteration. They live within the first cache line of their
owning struct (`alignas(64)`) and are read-only during system
execution wherever possible.

| Owner            | Field                                              | Why hot                                                                    |
|------------------|----------------------------------------------------|----------------------------------------------------------------------------|
| `Chunk`          | `row_count`                                        | Loop bound for every chunk iteration.                                      |
| `Chunk`          | `last_modified[col]`                               | Read at chunk-entry for `Changed<T>` filter chunk-skip.                     |
| `Chunk`          | `storage[ColumnDescriptor::offset .. ]` (relevant column for the query) | The actual SoA bytes the system reads/writes. |
| `Archetype`      | `first_nonfull_chunk_index`                        | Hot on `set_component` (insertion path).                                   |
| `Archetype`      | `columns[col].size` / `align` / `offset`           | Pre-resolved column offset; one constant per archetype per query column.   |
| `Query`          | `matched_` archetype id list                        | The outer loop of `Query::iter`.                                            |
| `Query`          | `type_filter_` bloom (during `on_new_archetype`)   | Constant-time archetype rejection on world growth.                         |
| `EntitySlot`     | `archetype_id`, `chunk_index`, `row`, `generation` | Hot on `World::get_component` / `set_component` resolution.                 |
| `ChangeTickClock`| `tick_`                                            | Read on every mutable access (`stamp_mutation`).                            |

### 5.2 Cold fields (touched during init, hot-reload, debug)

| Owner            | Field                                  | Why cold                                                              |
|------------------|----------------------------------------|-----------------------------------------------------------------------|
| `Archetype`      | `id`, `sorted_type_ids`                | Read at archetype-table lookup (rare; once per archetype migration).   |
| `Archetype`      | `chunks` (the unique_ptr vector itself; chunk *content* is hot) | Iterated once per chunk, not per row. |
| `Archetype`      | `row_to_entity`                        | Read on swap-remove (rare relative to read).                          |
| `EntityAllocator`| `slots_` (the vector header)           | Hot on resolution; the slot *fields* are hot, the vector header is cold.|
| `EntityAllocator`| `free_`                                | Touched on alloc/dealloc, not on resolution.                          |
| `EntityAllocator`| `live_count_`                          | Diagnostic only.                                                      |
| `EntitySlot`     | `live`                                 | Debug-only; release builds omit (the `generation` check is the truth). |
| `TypeRegistry`   | the entire struct                      | Read at world creation, plugin registration, archetype creation; never per-row. |
| `ResourceMap`    | `slots_`                               | Read at resource-system entry, not per-row.                           |
| `ArchetypeGraph` | `edges_`                               | Read at component add/remove (one lookup per structural change).       |
| `HookTable`      | `by_type_id_`                          | Read at component add/remove/set (one lookup per structural change).   |
| `CommandBuffer`  | `commands_`, `payload_`                | Hot during system execution; cold relative to query iteration.         |
| `World`          | `live_queries_`                        | Iterated only on archetype creation (rare).                            |

### 5.3 Layout enforcement

`Chunk`'s hot header fits in a single 64-byte cache line:

```cpp
static_assert(sizeof(Chunk::row_count) +
              sizeof(Chunk::row_capacity) +
              sizeof(Chunk::last_modified) <= 64);
```

`alignas(64)` on `Chunk` and `EntitySlot` is required; the build
asserts both. PMC (Apple Silicon performance-monitoring counter)
sampling under the §11 perf test verifies the hot loop's L1-D miss
rate remains under 5% on the S1 fixture; drift is the §9.5 alarm.

## 6. Concurrency

MVP runs every system on the game-loop driver thread (SPEC §6.10).
This collapses the concurrency surface to a small, exhaustive set:

### 6.1 Read-only operations

May run in any phase 1..=9. Read-only against `World` storage; no
exclusive lock required.

- `World::is_alive`, `get_component`, `last_change_tick`, `current_tick`
- `World::type_registry`
- `Query::iter(World const&)` — read-only view.
- `CommandBuffer::recorded_count` (read of one's own buffer).

These execute concurrently with each other on a single thread (the
"concurrency" is sequential composition); when per-system parallelism
lands post-MVP they will execute concurrently across worker threads
provided the schedule's access-set DAG (sibling spike #701) admits
them.

### 6.2 Read-write operations

Run only in phases that own the corresponding write set per the
Schedule's access-set DAG (sibling spike #701). For MVP single-thread
sim the constraint degrades to "no system runs concurrently with
another", but the DAG is collected nonetheless so the post-MVP
parallel dispatcher drops in without re-spec'ing the world boundary.

- `World::spawn`, `despawn`, `set_component`, `remove_component`
- `World::compile_query` (mutates `live_queries_`; runs only at
  plugin-register time, never during system execution).
- `Query::on_new_archetype` (mutates `matched_`; runs only inside
  `World::create_archetype`, which is itself a write).
- `CommandBuffer::flush` — the only writer that observes `World`'s
  archetype storage from outside a system body. Runs at sync points
  the schedule defines (sibling spike #701).
- `ChangeTickClock::stamp_mutation`, `advance_frame` — mutate the
  clock; called by mutating storage paths and by `FrameLoop`'s
  phase-9 entry, respectively.

### 6.3 Phase-by-phase admissibility

| Phase | World ops admitted in MVP                                                                            |
|-------|-------------------------------------------------------------------------------------------------------|
| 1 Input    | Read + write (platform plugin's input-mapping systems write `Input` / `ActionEvent` components). |
| 2 Logic    | Reserved slot; deferred body in MVP. Future gameplay-plugin systems will read-write here.        |
| 3 PhysicsFixed | Read + write (physics plugin's substeps).                                                    |
| 4 Animation| Reserved slot; deferred body in MVP.                                                              |
| 5 Transform| Read + write (core's `LocalTransform → GlobalTransform` propagation; the only `core`-owned write phase apart from 8). |
| 6 CullExtract | Read-only against archetype storage; renders an immutable `RenderFrame` extract.              |
| 7 RenderSubmit | No archetype reads — operates against the immutable `RenderFrame`.                            |
| 8 HotReload| `World` is quiescent; only the `HotReloadBarrier` (sibling spike #703) touches archetype storage via the migration arena. |
| 9 Present  | `ChangeTickClock::advance_frame` only; no archetype access.                                      |

The §4.4 invariant 1 (phase numeric order, no later-phase writes
observed by earlier phases) is enforced by the Schedule (sibling spike
#701); `World` itself does not check it. `World` does enforce, in
debug builds, the §4.4 invariant 4 access-set discipline via a per-
thread access token: a system that reads/writes a `TypeId` outside
its declared access set hits a debug assert.

### 6.4 Memory ordering

Every public `World` API is `noexcept` and assumes single-threaded
access except where marked. The one exception is the change-tick
clock's `current()` read which is permitted from any thread under a
relaxed atomic — but in MVP this is never exercised because every
caller is on the driver thread. The post-MVP parallel dispatcher will
make the clock's read-write a relaxed `std::atomic<u64>`; the design
reserves the upgrade path now without paying for it.

`World`'s heap is owned by the `core` `PerContextAllocator`; the
allocator is single-threaded for MVP (SPEC §9.4) and will gain a
per-thread arena when parallelism lands.

## 7. Persistence + ABI

Per SPEC §7.4 the `World`'s storage shape (entities, archetypes,
chunks, columns) is **plugin-owned** — `core` authors no `.fory`
schema for component bytes. The schemas `core` *does* author live in
SPEC §7.1; of those, none is `World`-aggregate-specific (the
manifest family is owned by `Plugin/PluginLoader`, sibling spike
#702; `HotReloadCheckpoint` is owned by `HotReloadBarrier`, sibling
spike #703; `LoadedPluginRecord` is owned by `PluginLoader`).

Concretely:

### 7.1 Schemas this design owns

**None.** The `World` aggregate's persistent surface is empty: every
byte that survives a hot-reload (SPEC §8.1) is either a plugin-owned
component (whose schema lives under `data/schemas/<plugin>/`) or a
sibling-aggregate schema (manifest family, checkpoint, loaded-plugin
record). This design's persistence section is therefore an
**explicit refusal** of any `World`-side `.fory` schema; PRs adding
one should be rejected and routed to either (a) the originating
plugin's `data/schemas/<plugin>/` directory, or (b) the sibling
aggregate that already owns the persisted concept.

The refusal is load-bearing: it preserves the SPEC §7.4 statement
that `core` never authors the schema for archetype-column bytes,
which in turn preserves PHILOSOPHY §6 (no runtime reflection in
shipping; no `core`-side knowledge of plugin-component byte layout).

### 7.2 ABI hash sources `World` participates in

`World` is a host-side consumer of the `glibre_types_abi_hash` from
`glibre-types.dylib` (per `fory-codegen.md` and `plugin-abi.md`);
the hash is computed over the **component schemas** plugins declare,
not over `World` itself. `World` does not contribute bytes to the
hash. The middleman dylib's exported codegen (the
`_registry.cpp` and `_abi_hash.cpp` TUs) populates the
`TypeRegistry` (§3.11) at static init; the `World::create()` call
seals the registry and freezes the type set for that process-session.

ABI hash sources `World` cares about, listed for the implementer's
plan:

1. **Per-component schema source** (`data/schemas/<plugin>/<T>.fory`)
   — owned by the originating plugin context; one component, one
   schema, one entry in the abi-hash digest input.
2. **`PluginManifest` family** (SPEC §7.1; manifest, ComponentDecl,
   SystemDecl, …) — owned by sibling spike #702, but `World` reads
   `ComponentDecl.fqn` and `ComponentDecl.schema_hash` at plugin
   registration time to populate `TypeRegistry`.
3. **`glibre_types_abi_hash`** — the single 64-char hex string
   `glibre-types.dylib` exports; the loader compares it against the
   plugin's compiled-in copy. `World` never reads this directly; it
   is a precondition for any plugin's `glibre_plugin_register` to
   reach the world at all.

`World` exports no symbols of its own that contribute to the hash;
its ABI shape is the SPEC §5.5 / §5.10 stub, not a Fory-versioned
schema. Changes to that stub bump `glibre-core`'s SemVer (per
`plugin-abi.md` §"Versioning Rules" axis 4), not the ABI hash.

### 7.3 What survives a session boundary (read: nothing)

The `World` aggregate is **runtime-only**. Process restart
reconstructs every byte from plugin registration; there is no
`World` snapshot persisted to disk by `core`. World snapshots are
the future `data` / `content` context's concern (SPEC §7.2 #2);
`core` only routes the bytes through `set_component` /
`get_component`. The replay-determinism requirement (PHILOSOPHY §7,
SPEC §10.2 #7) is met by re-running the deterministic system
schedule from a known input trace, not by loading a snapshot.

## 8. Hot-Reload

Hot-reload semantics are owned by the `HotReloadBarrier` aggregate
(SPEC §4.6 / §8, sibling spike #703). This section states what
**`World`** must hold steady across the swap, what `migrate(...)`
the barrier asks `World` to support, and the refusal cases this
design contributes.

### 8.1 What survives the swap

Per SPEC §8.1 the survival rule is mechanical: **a value survives
iff its type has a `.fory` schema**. Specialised to the `World`
aggregate's primitives:

| Primitive (§3 ref) | Survives swap?                                                                          |
|--------------------|------------------------------------------------------------------------------------------|
| `Entity` bits (§3.3) | **Yes.** Slot index + generation are unchanged. SPEC §8.1 #2.                          |
| `EntitySlot` (`archetype_id`, `chunk_index`, `row`) | **Yes**, modulo per-row migration: rows whose archetype still exists keep their addressing; rows whose archetype's component schemas migrated are rewritten in place by the barrier (SPEC §8.5). |
| `Archetype` shape (`sorted_type_ids`) | **Yes**, provided every constituent `TypeId` survives (i.e., the new plugin's `ComponentDecl` covers it). A subset (Q drops a type P registered) is a refusal — SPEC §8.4 step 2.2. |
| `Chunk` bytes (component data) | **Yes**, with per-row migration applied for any type whose `schema_version` bumped. Migration is in-place (SPEC §8.5 step 3.3). |
| `Chunk::last_modified[col]` (change ticks) | **Yes** as raw values (the clock is unaffected), but their *meaning* against the new schema is the migration body's responsibility. |
| `ChangeTickClock::tick_`     | **Yes.** SPEC §8.1 #6.                                                                     |
| `TypeRegistry` (§3.11)       | **Yes**, append-only across the swap. New `TypeId`s the incoming plugin declares are added (SPEC §8.4 step 4); existing entries are immutable. |
| `ResourceMap` (§3.9)         | **Yes** — typed singleton bytes are middleman-typed, so they migrate with the same per-row mechanism as components. |
| `ArchetypeGraph::edges_` (§3.4) | **Yes** — the graph is a function of the (immutable) archetype set; survives unchanged. |
| `HookTable` function pointers (§3.6) | **No** — function pointers reference the outgoing plugin's code. The barrier replaces them at SPEC §8.4 step 3 vtable-swap. |
| `Query*` handles (§3.7)      | **Yes** as identity; the matched-archetype-id list is unchanged because no archetype is rebuilt. The pre-resolved column offsets are recomputed if the archetype's column layout changed (it cannot, because schema_hash drift is a refusal — SPEC §8.4 step 2.2). |
| `CommandBuffer` arenas (§3.10) | **No** — drained at SPEC §8.3 step 1 (drain). All in-flight buffers either flush or `clear()` before the swap. |

### 8.2 What `migrate(...)` must do

`World` itself does not author migration bodies — those are owned by
each component's originating plugin per `fory-codegen.md` and SPEC
§7.4. What this design contributes is the **per-row migration call
site**: the barrier (SPEC §8.5 step 3) walks each surviving
`Archetype` row by row, allocates `sizeof(T_current)` from the
barrier's per-phase migration arena (SPEC §9.3 row 5; 16 MiB cell
under `core`'s 64 MiB), invokes the per-type chain, and overwrites
the row's bytes in place on full-chain success. `World`'s only job
is to expose the row addressing (`Archetype × Chunk × Row → byte
span`) the barrier walks; that addressing is exactly the §3.2
storage layout this design freezes.

The migration body contract — pure, deterministic, arena-only,
single-step, local — is restated in SPEC §8.2 / §7.3 and is not this
design's authority. `World` only guarantees that:

1. The byte spans handed to the migration body are the post-drain
   storage rows (no in-flight `CommandBuffer` writes outstanding).
2. The same `(Archetype, Chunk, Row)` triple addresses the same
   logical entity before and after the swap.
3. No system is executing concurrently with the migration walk
   (SPEC §6.10 single-thread sim; `World` exclusive lock is
   trivially held during phase 8).

### 8.3 Refusal cases this design contributes

`World` raises these arms during the barrier's traversal; per SPEC
§8.7 they are wrapped under `core::Error::HotReload` when raised
inside a barrier transaction.

| Detection point                                                                                            | `core::Error` arm                  | SPEC §10.1 row |
|-----------------------------------------------------------------------------------------------------------|------------------------------------|----------------|
| Q's `ComponentDecl` set is missing a `TypeId` P registered (storage cannot survive)                        | `PluginManifestInvalid`            | §10.1          |
| Q's `ComponentDecl.schema_hash` differs on an existing FQN (storage layout would mismatch)                 | `PluginAbiHashMismatch`            | §10.1          |
| The migration arena would overflow walking the row set (the `World` allocates the row-size estimate ahead) | `OutOfBudget` (engine-wide)        | §10.2 #6        |

`World` does **not** raise `HotReloadDrainTimeout`,
`HotReloadAbiHashMismatch`, `HotReloadSelfReference`, or
`SchemaMigrationFailed` — those originate inside the barrier's own
state machine (sibling spike #703). The arms `World` does contribute
are detected before the barrier mutates any storage byte; on refusal
the prior storage is bytewise unchanged and the prior-good plugin
remains live (SPEC §8.8 #2).

### 8.4 Self-reload refusal

`core` is not hot-reloadable in MVP (SPEC §3.3, hot-reload-protocol
§"Open Questions" #2). `World` therefore never participates as the
*outgoing* plugin in a swap; it is always the host. A
`request_reload(plugin_fqn=…)` whose target would resolve to a
`core`-owned plugin is refused at request time with
`core::Error::HotReloadSelfReference` (SPEC §5.1, §10.1) by the
barrier — `World` itself does not need to detect it, but the
refusal is recorded here so the cross-aggregate invariant is
explicit.

## 9. Performance

Authority: `reviews/decisions/perf-budget.md` Per-Context Budget
Table assigns `core` the cell **0.40 ms CPU sim + 0.05 ms CPU
submit + n/a GPU + 64 MiB heap**. The SPEC §9.3 per-aggregate split
gives `World` the row **0.20 ms sim + 24 MiB heap**. This design
does not widen those numbers; it refines the breakdown of the row
across the §3 primitives so the §11 `BENCHMARK_CELL` test name
`core/world: archetype_iteration_change_tick_scan` (SPEC §9.5) has
a well-defined target.

### 9.1 Cited cell (verbatim from `perf-budget.md` and SPEC §9.3)

| Axis           | Budget              | Source                                  |
|----------------|---------------------|-----------------------------------------|
| CPU sim        | 0.20 ms             | SPEC §9.3 row "World"                    |
| CPU submit     | (n/a — `World` has no phase-7 work) | SPEC §9.3 row "World"      |
| GPU            | n/a                 | `core` owns no rendering                 |
| Heap           | 24 MiB              | SPEC §9.3 row "World"                    |
| Phase ownership| 5 (transform), participates as read-only in 1, 3, 6 (renders read), and as quiescent in 7, 8, 9 | SPEC §9.2 / `frame-phases.md` |

### 9.2 Sub-budget per primitive

The 0.20 ms sim cell decomposes across the §3 primitives. Numbers
below sum to ≤0.20 ms under the S1 fixture (`perf-budget.md`
§"Justification Per Cell" — 1 character + 200 props + 8 lights at
1080p, ~2 k entities total). The sub-budget is advisory at the
benchmark layer (the `BENCHMARK_CELL` asserts the row total, not
the per-primitive split); per-primitive drift surfaces as a
flame-graph hotspot in the §11 perf-trace artifact.

| Primitive (§3 ref)           | CPU ms (sim) | Heap (MiB) | Dominant op                                                  |
|------------------------------|--------------|------------|--------------------------------------------------------------|
| Archetype iteration (§3.2 + §3.7) | ~0.12     | 16         | The query hot loop; chunk walk + change-tick scan.            |
| Entity allocator (§3.3)       | ~0.02       | 4          | `resolve()` on `get_component`; one cache line per entity.    |
| Archetype graph (§3.4)        | ~0.01       | 1          | Edge cache lookup on add/remove transitions.                  |
| ChildOf forest (§3.5)         | ~0.00       | <0.5       | Cycle check on `set_component(ChildOfTypeId, …)` only.        |
| Lifecycle hooks (§3.6)        | ~0.01       | <0.5       | Function-pointer call per add/remove/set; no-op for most types.|
| Change-tick clock (§3.8)      | ~0.00       | <0.1       | One increment per mutable access.                              |
| Resource map (§3.9)           | ~0.01       | 1          | Hash lookup on `Res<T>` access.                                |
| TypeRegistry (§3.11)          | ~0.00       | 0.5        | Read-only; populated once at init.                             |
| Command buffer arenas (§3.10) | ~0.03       | 1          | Per-system replay; cost is amortised under the §9.3 `CommandBuffer` row not this row. |
| **Sub-total**                 | **~0.20**   | **~24**    | Sums into SPEC §9.3 row "World".                                |

### 9.3 Allocator cells declared

Per SPEC §9.4 every allocation under `core/src/world/**` and
`core/src/type-registry/**` is stamped `ContextTag::core`. The
24 MiB sub-share of the §9.2 cell ceiling is enforced by the §11
`BENCHMARK_CELL` heap-residency assertion (`<=24` MiB resident at
frame end on the S1 fixture); the cell ceiling itself is the 64 MiB
`core` row enforced by `glibre::PerContextAllocator` in strict
mode.

`CommandBuffer` arena cap (SPEC §4.8 inv. 4) is **64 KiB per system**
(matches R-1.1.33 typical-usage figure); a system that exceeds this
under one frame of S1 is a fixture bug and surfaces as
`core::Error::CommandBufferOverflow`.

### 9.4 GPU and submit halves

`World` consumes **no** GPU budget and **no** CPU submit budget.
Phase 5 transform propagation (the `core`-owned write phase that
walks `World` storage) is sim-half work; phase 6 cull-extract reads
`World` storage but is `render`'s budget, not `World`'s.

### 9.5 CI gate

The mandated `BENCHMARK_CELL` block per SPEC §9.5 row 1:

```cpp
// tests/core/world/bench_archetype_iteration.cpp
BENCHMARK_CELL("core/world: archetype_iteration_change_tick_scan") {
    auto fx = S1Fixture::create();              // 1 char + 200 props + 8 lights
    auto query = fx.world->compile_query(...);  // Reads<LocalTransform>, Reads<MeshHandle>
    BENCHMARK("steady-state iteration") {
        for (auto chunk : query->iter(*fx.world)) { /* read-only walk */ }
    };
    REQUIRE(elapsed_ms <= 0.20);
    REQUIRE(resident_bytes(ContextTag::core, /*world subshare*/) <= 24 * 1024 * 1024);
}
```

The benchmark fixture lives at `tests/core/world/fixture_s1.cpp`
and is the canonical S1 instance for `World` perf assertions.

## 10. Failure Modes

`World` raises a closed subset of the `core::Error` enum (SPEC §5.1).
Each arm below names the trigger condition, the recovery posture, the
log severity, and the SPEC §10.1 row that documents it.

The arms are split by `World` primitive (§3 references). All arms are
documented authoritatively in SPEC §10.1; this section adds the
per-primitive trigger detail the implementer needs.

### 10.1 Entity-allocator arms (§3.3)

| Arm                     | Trigger                                                                                  | Recovery | Severity | SPEC ref |
|-------------------------|------------------------------------------------------------------------------------------|----------|----------|----------|
| `EntityStale`           | `EntityAllocator::resolve(e)` finds `slots_[e.index].generation != e.generation`         | Refuse   | warn     | §10.1    |
| `EntityForeignWorld`    | An `Entity` from world A is passed to world B (reserved for post-MVP multi-world)         | Refuse   | error    | §10.1    |

### 10.2 Type-registry / archetype arms (§3.2, §3.4, §3.11)

| Arm                     | Trigger                                                                                  | Recovery | Severity | SPEC ref |
|-------------------------|------------------------------------------------------------------------------------------|----------|----------|----------|
| `TypeUnregistered`      | A public-API call names a `TypeId` not present in the sealed registry                     | Refuse   | error    | §10.1    |
| `TypeRegistryClosed`    | `TypeRegistry::register_type` called after `seal()` (i.e., post-`World::create`)         | Refuse   | error    | §10.1    |

### 10.3 Hierarchy arm (§3.5)

| Arm                     | Trigger                                                                                  | Recovery | Severity | SPEC ref |
|-------------------------|------------------------------------------------------------------------------------------|----------|----------|----------|
| `HierarchyCycle`        | `set_component(child, ChildOfTypeId, parent_bytes)` where the parent walk-up reaches `child`, OR walk-up exceeds 256 levels | Refuse | error | §10.1    |

### 10.4 Command-buffer arm (§3.10)

| Arm                     | Trigger                                                                                  | Recovery | Severity | SPEC ref |
|-------------------------|------------------------------------------------------------------------------------------|----------|----------|----------|
| `CommandBufferOverflow` | `CommandBuffer::add_component / set_component` would push payload beyond the 64 KiB arena cap | Refuse | warn | §10.1 |

### 10.5 Allocator arm (engine-wide)

| Arm                     | Trigger                                                                                  | Recovery | Severity | SPEC ref |
|-------------------------|------------------------------------------------------------------------------------------|----------|----------|----------|
| `OutOfBudget`           | A `World`-allocated growth (entity slots, archetype storage, query cache) would exceed the `core` cell's 64 MiB ceiling under `GLIBRE_ALLOC_STRICT=1` | Refuse | error (debug); warn-once-per-frame (shipping) | §10.2 #6 |

### 10.6 Hot-reload arms `World` participates in

These are emitted by the barrier (sibling spike #703) but the
detection points live in `World` boundaries that the barrier calls.
Listed for completeness; the authoritative description is SPEC §8.7.

| Arm                                          | `World` detection point                                              | SPEC ref       |
|----------------------------------------------|---------------------------------------------------------------------|----------------|
| `PluginManifestInvalid` (under `HotReload`)  | Q's component set drops a `TypeId` P registered                      | §8.4 step 2.2 |
| `PluginAbiHashMismatch` (under `HotReload`)  | Q's `ComponentDecl.schema_hash` drift on an existing FQN              | §8.4 step 2.2 |
| `SchemaMigrationFailed` (under `HotReload`)  | A migration body returns `unexpected` while walking `World` rows      | §8.5 step 3   |

### 10.7 Caller-side recovery posture

Per SPEC §10.2 #5, callers handle each arm at exactly one boundary.
For `World` arms specifically:

- **Plugin systems** — propagate (`return std::unexpected{err}`) up to
  the schedule's per-system error wrapper, which logs once at `warn`
  (or `error` for the non-`HotReload` arms) and continues to the
  next system. A failed system body does not abort the frame.
- **`FrameLoop::tick`** — handle + log; never propagates a `World`
  error to the runtime entry point.
- **Editor UI** — converts to ImGui-displayable text via the
  error-model's structured-fields formatter; never silently drops.

### 10.8 Determinism obligation

Every non-`HotReload` arm above must fire byte-equal across runs and
hosts on byte-equal input (PHILOSOPHY §7, SPEC §10.2 #7). The §11
golden-snapshot tests assert this on the error stream alongside the
world-state stream.

## 11. Test Plan

Unit + integration tests required to validate the §1–§10 invariants.
Catch2 test names are stable; the `[STORY]` parent issue (SPEC §11)
that exercises the path through manual + E2E is named in the right
column where one exists.

### 11.1 Unit tests (one Catch2 case per row)

Lives under `tests/core/world/`.

| Test name                                                | What it asserts                                                                            | §-ref          | Story |
|----------------------------------------------------------|--------------------------------------------------------------------------------------------|----------------|-------|
| `world: spawn_returns_unique_entity`                     | `spawn` increments live count; two spawns return distinct `Entity` bits.                    | §3.3, §4.3 inv 1 | #320  |
| `world: despawn_advances_generation`                     | Despawn → re-allocate at same index → original `Entity` resolves to `EntityStale`.          | §3.3, §4.3 inv 1 | #320  |
| `world: archetype_creation_is_set_keyed`                 | `set_component(e, A); set_component(e, B)` lands in the same archetype as `set_component(e, B); set_component(e, A)`. | §3.2 inv 1     | #322  |
| `world: chunk_alignment_64`                              | `alignof(Chunk) == 64`; first chunk's `storage` is also 64-aligned.                         | §3.2 inv 3     | #322  |
| `world: chunk_capacity_correct_for_row_stride`           | A 64-byte row stride yields capacity = (16 KiB − header) / 64.                              | §3.2 inv 2     | #322  |
| `world: swap_remove_preserves_density`                    | Despawn one of three rows → row 2 occupies row 0's slot; reverse map is consistent.         | §3.2 inv 4–5   | #322  |
| `world: archetype_graph_o1_amortized`                     | First add of T to archetype A computes the destination; second add is a single hash lookup.| §3.4           | #324  |
| `world: hierarchy_refuses_cycle`                          | `set_component(child, ChildOf, child)` → `HierarchyCycle`.                                   | §3.5, §4.1 inv 2 | #338  |
| `world: hierarchy_refuses_depth_overflow`                 | A 257-deep `ChildOf` chain → `HierarchyCycle`.                                              | §3.5           | #338  |
| `core/lifecycle_hook: on_add_fires_after_archetype_transition` | `set_component(e, T, b)` fires `OnAdd` exactly once when `T` was absent (plan #579). | §3.6, §4.1 inv 7 | #329  |
| `world: lifecycle_on_set_fires_only_when_present`         | `set_component(e, T, b)` fires `OnSet` (not `OnAdd`) when `T` was already present.          | §3.6, §4.1 inv 7 | #329  |
| `world: lifecycle_on_remove_fires_in_column_order`         | `despawn(e)` fires `OnRemove` for each component in archetype-column order.                 | §3.6, §4.1 inv 7 | #329  |
| `world: query_with_filter_excludes_archetypes`            | A query with `Without<T>` returns no rows from archetypes containing T.                     | §3.7, R-1.1.18 | #326  |
| `world: query_changed_filter_uses_chunk_tick`             | A chunk whose `last_modified[col] <= changed_since` is wholesale skipped; a chunk above is iterated. | §3.7, §3.8, R-1.1.23 | #326  |
| `world: query_caches_matched_archetype_set`                | After compile, two iterations match the same archetype set without re-bloom-filter walks.   | §3.7, R-1.1.18 | #326  |
| `world: query_on_new_archetype_appends_match`             | Spawn into a new archetype; an existing query matching it sees the row on next iter.        | §3.7, R-1.1.41 | #326  |
| `world: change_tick_monotonic`                             | `current_tick` only ever increases; an explicit decrement via debug API aborts.             | §3.8, §4.1 inv 4 | #337  |
| `world: change_tick_advances_on_set`                       | Two `set_component` calls produce distinct `last_change_tick` values.                       | §3.8           | #337  |
| `world: resource_typed_singleton_round_trip`               | `insert<T>(bytes)` then `get<T>()` returns the same bytes; `get_mut<T>` bumps the tick.     | §3.9, R-1.1.24 | #335  |
| `world: type_registry_closed_after_create`                 | `World::create` seals; subsequent `register_type` → `TypeRegistryClosed`.                    | §3.11, §4.9 inv 1 | #347  |
| `world: type_registry_unique_type_ids`                     | Codegen-asserted at build; runtime asserts no duplicate ids in the sealed registry.          | §3.11, §4.9 inv 2 | #347  |
| `world: command_buffer_records_then_replays_in_insertion_order` | Two `add_component` calls replay in the order they were appended.                       | §3.10, §4.8 inv 2 | #328  |
| `world: command_buffer_overflow_refuses`                   | A 65th 1-KiB `add_component` → `CommandBufferOverflow`; the 64 KiB cap holds.                | §3.10, §4.8 inv 4 | #328  |
| `world: command_buffer_no_self_read`                        | A system that records an `add_component` does not see the component until the next sync point. | §3.10, §4.8 inv 3 | #328  |
| `world: get_component_typed_returns_stale_on_despawn`      | `despawn(e); get_component(e, T)` → `EntityStale` on next call.                              | §3.3, §4.3 inv 1 | #320  |
| `world: error_arms_byte_equal_across_runs`                 | The error stream from a fixed input trace is byte-equal across two consecutive runs.         | §10.8           | (no story; CI invariant) |

### 11.2 Integration tests

Lives under `tests/core/world/integration/`. Drives the world through
multi-frame sequences via the `Schedule` test harness (sibling spike
#701 contributes the harness; this design names the cases).

| Test name                                                | Scenario                                                                                  | §-ref         |
|----------------------------------------------------------|-------------------------------------------------------------------------------------------|---------------|
| `world: deterministic_world_snapshot_across_runs`        | S1 fixture; 100 frames; a serializing visitor (test-only) emits a byte stream; two runs equal. | §10.8, PHILOSOPHY §7 |
| `world: cross_frame_changetick_filter`                    | Frame N writes T; frame N+1's `Changed<T>` query observes; frame N+2's does not.          | §3.7, §3.8    |
| `world: hot_reload_world_state_survives_no_schema_bump`  | S1 + `bad-abi-hash` plugin reload → reload refuses; `World` is bytewise unchanged.         | §8.1, §8.3    |
| `world: hot_reload_world_state_migrates_v1_to_v2`        | S1 + `v1-to-v2-migration` plugin reload → archetype rows are migrated in place; `Entity` bits survive; new schema queryable on next frame. | §8.2, §8.5 |
| `world: hot_reload_refuses_dropped_component_type`        | S1 + Q-drops-T plugin reload → reload refuses with `PluginManifestInvalid` under `HotReload`; world unchanged. | §8.3 row 2.2 |
| `world: query_caches_persist_across_hot_reload`           | A pre-reload query keeps matching post-reload (assuming no schema drift on its types).      | §8.1 row "Query*" |
| `world: bench_archetype_iteration_under_s1`               | The §9.5 `BENCHMARK_CELL` perf assert.                                                    | §9.5          |

### 11.3 Property-based tests

Lives under `tests/core/world/property/`. Use Catch2 `GENERATE` for
fuzz-style coverage.

| Test name                                                | Property                                                                                  | §-ref |
|----------------------------------------------------------|-------------------------------------------------------------------------------------------|-------|
| `world: spawn_despawn_random_walk_no_leak`               | 10 k random spawn/despawn ops; final live count matches the running count; no slot leaks. | §3.3  |
| `world: random_component_add_remove_terminates_in_archetype` | Random sequences of `set_component`/`remove_component` always land in some valid archetype; no cycles in the graph traversal. | §3.2, §3.4 |
| `world: query_filter_set_algebra`                         | For random `(reads, writes, without)` triples, the matched archetype set equals the hand-computed set algebra. | §3.7 |

### 11.4 What this design does **not** test

- **Multi-thread access** — single-thread sim in MVP (SPEC §6.10).
  When per-system parallelism lands, ThreadSanitizer + the access-set
  DAG (sibling spike #701) cover the new surface.
- **Multi-world** — refused in MVP (R-1.1.35). The
  `EntityForeignWorld` arm has a unit test that asserts the refusal
  shape, but the multi-world boundary itself is post-MVP.
- **Sparse / shared / buffer / enableable storage** — refused in MVP
  (§2.2). When any of these reactivate, the new storage shape gets
  its own unit tests at that time.
- **Sort / aspect / variable / parallel-iteration query terms** —
  refused in MVP (§2.5).
- **Plugin loading semantics** — owned by sibling spike #702 / #703;
  this design only contributes the `World`-side hot-reload assertions
  in §11.2.

### 11.5 Acceptance-criteria mapping

The `[STORY]` issues already named in SPEC §11 that this design's
tests close (one Catch2 case per story; the §11.1–§11.3 tables above
list them per story column):

- #320 `[STORY] spawn-despawn-entity-lifecycle`
- #322 `[STORY] archetype-soa-chunked-storage`
- #324 `[STORY] component-add-remove-archetype-migration`
- #326 `[STORY] queries-with-without-changed`
- #328 `[STORY] command-buffer-deferred-mutation`
- #329 `[STORY] component-lifecycle-hooks`
- #335 `[STORY] typed-singleton-resources`
- #337 `[STORY] changetick-monotonic-clock`
- #338 `[STORY] childof-relationship-forest`
- #347 `[STORY] type-registry-immutable-after-init`

The remaining stories from SPEC §11 (#331, #333, #340, #343, #344,
#345, #346, #348) belong to sibling aggregates and are out of scope
for this design.

## 12. Open Questions

Each `[OPEN]` is the trigger for a follow-up `close-*-open-questions`
spike (per the workflow); resolution amends the matching SPEC section
in place. SPEC #492–#499 already cover several of these — those are
named here so the cross-reference is explicit.

- **[OPEN]** Should `Chunk` size be a per-archetype tunable (heuristic
  on per-row stride) instead of a fixed 16 KiB? Re-derives R-1.1.1's
  8–64 KiB tunable. Tracked by SPEC #494.
- **[OPEN]** Should sparse-set storage (R-1.1.3) reactivate when an
  MVP component demonstrates ≥10× archetype churn? Trigger fixture
  TBD when the first churn-heavy use case lands.
- **[OPEN]** Should shared-component storage (R-1.1.7) reactivate when
  an MVP plugin (likely render) demonstrates the duplication cost?
  Trigger fixture TBD.
- **[OPEN]** Should `DynamicBuffer<T>` (R-1.1.8) reactivate, or do
  plugins continue modelling per-entity collections via
  `AssetHandle<T>`? Trigger: a real-world workload that an
  `AssetHandle` indirection mis-fits.
- **[OPEN]** Should `#[enableable]` toggling (R-1.1.9) reactivate? The
  archetype-move cost is the alternative; trigger when the move cost
  is measured as load-bearing on an MVP workload.
- **[OPEN]** Per-system parallelism seam: does the `glibre-foryc` thunk
  emit a single fork-join wrapper, or does the schedule own a separate
  `ParallelCompiledPhase`? When the parallelism plan opens, decide.
  Tracked by SPEC #496.
- **[OPEN]** AoSoA tiled chunks (R-1.1.40) — reactivates if a `core`-
  owned phase-5 transform walk measures as SIMD-bound on S1.
- **[OPEN]** `Option<T>` and `Added<T>` query terms (R-1.1.18) —
  reactivate if a render or gameplay plugin measurably needs them.
- **[OPEN]** Multi-world (R-1.1.35) — reactivates when a second use
  case (rollback netcode or editor preview world) actually exists.
- **[OPEN]** Cleanup components (R-1.1.13) — reactivates if a real
  resource-teardown surface needs synchronous despawn delay (the
  current answer is the resolving plugin owns its own teardown via
  `OnRemove` hook).
- **[OPEN]** Migration of `core`-owned types during hot-reload — SPEC
  §4 cross-aggregate invariant 5 deferred this; tracked by SPEC #492.
- **[OPEN]** `core::Error::OutOfBudget` payload — should it carry the
  primitive (e.g. `ContextTag::core / EntitySlots`) so consumers can
  pinpoint the busted sub-share? Resolves alongside `perf-budget.md`
  Open Question #5.

Resolution of any `[OPEN]` lands the decision into
`reviews/decisions/` (when cross-aggregate) or amends §3 / §9 / §11
in place (when local to `World`); per the workflow no `[OPEN]` is
discharged silently.

## 13. Plan Backlog Delta

This table records only the `[PLAN]` issues added **after** the
primary per-aggregate backlog already cited in the SPEC
(#557, #562, #565, #567, #572, #574, #577, #579, #589, #591,
#597, #601, #923–#933). It is not a complete cross-reference of
all per-§ owners; use the issue tracker label `domain:core` +
`type:plan` for a full view.

| Plan | Scope                                                                               | Design ref            |
|------|-------------------------------------------------------------------------------------|-----------------------|
| #935 | `world: hierarchy_refuses_depth_overflow` unit test                                 | §3.5, §11.1           |
| #936 | Lifecycle hook present/absent + column-order discrimination tests                   | §3.6, §11.1           |
| #937 | World migration row-addressing API for HotReloadBarrier (per-row byte span exposure) | §8.2 #1–3, §3.2      |
| #938 | Archetype-iter L1-D miss-rate alarm (≤5%) on archetype iteration hot loop (core)    | §5.3, §9.5            |
| #939 | `ResourceSlot::last_modified` tick + `Changed<Res<T>>` readback                     | §3.9, §3.7, §3.8      |
| #940 | `EntityForeignWorld` refusal-shape contract (post-MVP arm placeholder)              | §10.1, §11.4          |
| #941 | `ChangeTick` relaxed-atomic upgrade-path scaffold (`TickStorage` alias)             | §6.4                  |
| #942 | Archetype forward/reverse map debug invariant cross-check                           | §3.2 #5, §5.3         |
| #944 | macOS PMC sampler helper (`pmc_sampler.{hpp,cpp}`) + CI artifact-upload gate (infra) | §5.3, §9.5            |

Authorship rule: any further `[PLAN]` decomposing this design (e.g.
once an `[OPEN]` resolves) appends to this table in the same PR
that opens the plan.
