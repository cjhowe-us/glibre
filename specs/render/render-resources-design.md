# render — Detailed Design: render-resources aggregate

> Detailed design for the **render-resources** aggregate (typed handles
> + lifetime management for textures, buffers, render targets, samplers,
> and argument buffers) declared in `specs/render/SPEC.md` §4.1.4
> (`Resource` — virtual + physical; transient / persistent / imported)
> and consumed by §4.1.2 (`RenderGraph`), §4.1.5 (`ExecutionPlan`),
> §4.1.6 (`MetalDevice` heap allocator), §4.1.7 (`PSOCache` argument-
> buffer binding), §4.1.8 (`RTAccelStructures` BLAS / TLAS storage),
> §4.1.9 (`HZB`) and §4.1.10 (`ClusterCullState`). Refines §4.1.4,
> §5 (`ResourceDesc` / `ResourceUsage` / `ResourceFormat` /
> `ResourceLifetime` / `Handle<Tag>` / `GraphBuilder::declare_*`),
> §6.1 `resources/` directory, §6.3 concurrency, §7.1 / §7.3 (nothing
> persists at this layer), §8.2 hot-reload survival rows for persistent
> resources / `TransientPool` heaps, §9.5 heap composition (256 MiB
> transient + 128 MiB persistent + 16 MiB handle tables + 48 MiB RT
> + 64 MiB PSO = 512 MiB ceiling), §9.5.1 allocator rules, and §10
> failure-mode rows `ResourceAllocFailed` / `ResourceResidencyExceeded`
> (§10 design-name labels; §5 enum identifiers are `HeapOutOfMemory` /
> `TransientPoolExhausted` / `ResourceResidencyExceeded`).
> Cites `reviews/decisions/error-model.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/fory-codegen.md`, and
> `reviews/decisions/frame-phases.md`. Introduces no new public surface
> beyond `specs/render/SPEC.md` §5; deviation from the cited records
> requires an amendment spike, not an in-place edit. Resolves
> `[SPIKE] design-render-render-resources-detailed` (#774).

## 1. Purpose

The render-resources aggregate is render's **typed-handle catalog +
lifetime allocator** for every GPU-side and host-side
allocation that lives inside the render plugin. Per
`specs/render/SPEC.md` §4.1.4 it owns:

1. The **`VirtualResource`** value object — a logical
   `(format, extent, mip_levels, array_layers, sample_count, usage,
   lifetime, debug_name, frame-of-birth, frame-of-last-use)` record
   referenced by `VirtualResourceHandle`. Issued by
   `GraphBuilder::declare_transient` / `declare_persistent` /
   `declare_imported`; consumed by passes' read/write declaration sets.
2. The **`PhysicalAllocation`** entity — the heap slot or
   externally-vended `MTL::Texture` / `MTL::Buffer` / sampler /
   argument-buffer that materialises a `VirtualResource`. Referenced
   by `PhysicalAllocHandle`. Owns no algorithmic policy; is purely a
   storage record + GPU-handle holder.
3. The **`TransientPool`** — render's per-frame placement heap pool.
   `MTL::Heap` instances pre-sized at init (`SPEC.md` §9.5 transient
   row = 256 MiB) into which the alias planner places lifetime-disjoint
   `VirtualResource`s as `MTL::Texture` / `MTL::Buffer` placements.
   The pool drains at the phase 7→9 boundary every frame.
4. The **persistent allocator** — the long-lived slab of `MTL::Heap`s
   carrying resources whose lifetime crosses frames: `HZB` pyramids,
   shadow atlases, history-color targets, ring-buffer storage,
   lighting LUTs, IBL probes, font / overlay atlases, RT scratch.
   Sized at init from the §9.5 persistent row (128 MiB).
5. The **imported-resource borrow registry** — opaque records that
   wrap externally-owned allocations (the swapchain `MTLDrawable`,
   `geometry`-vended BLAS, `vfx`-vended particle buffers,
   `shader`-vended PSO blobs) without taking ownership. Lifetime
   stays with the importer (`SPEC.md` §4.1.4 invariant 3).
6. The **typed-handle tables** — five generation-counted
   `(index, generation)` tables, one per public handle kind:
   `VirtualResourceHandle`, `PhysicalAllocHandle`,
   `ArgumentBufferHandle`, `RingSliceHandle`, plus the read-only
   import handles (`BLASHandle`, `HZBHandle`,
   `ClusterCullStateHandle`, `ShadowAtlasHandle`) whose tables are
   owned by their respective aggregates but whose entries route
   through this layer's catalog for lookup. Tables sit inside the
   §9.5 "GPU resource handles" 16 MiB row.
7. The **ring-buffer surface** — per-frame-in-flight CPU-write /
   GPU-read rings (constant heaps, instance buffers, indirect-arg
   buffers). Slices are addressed by `RingSliceHandle`; the slab
   itself is part of the persistent allocator.
8. The **argument-buffer frequency-group binder** — the four-level
   binding-table builder (per-frame / per-pass / per-material /
   per-draw, `SPEC.md` §3.2 collapse #8) that resolves
   `(VirtualResourceHandle, PhysicalAllocHandle)` pairs into
   `MTL::ArgumentEncoder` offsets consumed by passes' execute
   lambdas via the opaque `Bindings` struct. The binder is hot-path
   and lives behind a stable per-pass cache.

This aggregate **refuses to own**:

- **Graph topology, declared-edge set, alias-planner colouring
  algorithm, barrier emission, queue assignment, structural-hash
  cache.** Owned by the `RenderGraph` / `ExecutionPlan` aggregate
  (`SPEC.md` §4.1.2, §4.1.5; spike #760 detailed design). Resources
  feed the planner (lifetime rows + footprint estimates) and consume
  its output (`AliasPlan` mapping each `VirtualResource` to a
  `PhysicalAllocation` slot); this layer never decides
  *which colour* a virtual resource gets.
- **Pass bodies, declared access sets, `execute()` lambda
  semantics.** Owned by the pass aggregate (`SPEC.md` §4.1.3; spike
  #764). Passes call `GraphBuilder::declare_*` and reference the
  returned handles; resources never look inside an execute lambda.
- **PSO compilation, `MTL::RenderPipelineState` /
  `MTL::ComputePipelineState` residency, eviction.** Owned by
  `PSOCache` (`SPEC.md` §4.1.7; spike #766). Argument buffers built
  here are consumed by PSOs but the cache itself is sibling: the
  cache stores compiled pipeline state; the resources aggregate
  stores the data that flows *through* those pipelines.
- **`MetalDevice` / `MetalQueue` / `MetalCommandBuffer` lifecycles,
  surface attach, residency-set construction, fence emission.**
  Owned by the metal-backend aggregate (`SPEC.md` §4.1.6; spike
  #762). The resources aggregate calls `MetalDevice::heap_allocator()`
  and `MetalDevice::residency()` to acquire and attach `MTL::Heap`s
  but does not construct the device, the queues, or the residency
  set. Heap *attachment* to the residency set is a backend call;
  heap *allocation* is this layer's responsibility.
- **BLAS / TLAS construction or storage layout.** Owned by
  `RTAccelStructures` (`SPEC.md` §4.1.8; spike #768). BLAS handles
  are imports (read-only by `SPEC.md` §4.1.8 invariant 3); TLAS
  storage is a persistent `Resource` whose backing slab lives in
  this layer's persistent allocator but whose contents are managed
  by the RT aggregate.
- **HZB / `ClusterCullState` storage policy.** Owned by `HZB`
  (#769) and `ClusterCullState` (#771) respectively. Their
  *backing GPU bytes* sit in this aggregate's persistent allocator
  (per `SPEC.md` §8.2 row "HZB pyramid, ClusterCullState scratch");
  their *dispatch policy and triple-buffer cadence* live in their
  own design specs.
- **Cull, extract, sort, budget cull, RenderFrame finalisation.**
  Owned by the cull-extract aggregate inside phase 6 (`SPEC.md`
  §6.2.1; spike #770). Phase 6 reads no resources from this
  layer except indirectly through the HZB pyramid pointer (#769).
- **Asset content, vertex/index/meshlet bytes, texture pixels,
  Draco decode, FBX import.** Owned by `geometry` and `content`
  (`SPEC.md` §3.3). Resources receives already-resident GPU bytes
  via `declare_imported` and never reaches into the asset layer.
- **`RenderSettings`, `CapabilityMask`, `PSOCacheRecord` Fory
  schemas.** Owned by `SPEC.md` §7.1.1 / §7.1.3 / §7.1.2
  respectively. Resources is the aggregate that *consumes* the
  capability set when sizing the persistent slab (HZB extent,
  shadow-atlas tier) but persists nothing.
- **Frame schedule.** Phase 6 / phase 7 entry / exit are owned by
  `core` (`reviews/decisions/frame-phases.md`); resources is
  invoked inside those phases and obeys their entry / exit
  guarantees.
- **Resource serialisation.** Disk persistence of any
  `VirtualResource`, `PhysicalAllocation`, alias plan, or argument
  buffer is forbidden. This layer's only persistent artefact would
  be a heap-residency hint, which is not part of MVP. PHILOSOPHY
  anti-pattern reaffirmed.

The aggregate's SRP boundary is sharp: if **the way a render
resource is named, allocated, looked up, freed, or aliased** changes,
this design changes. Anything else is out of scope.

## 2. Requirements coverage

This section verifies that every harmonius MVP-scope requirement /
design clause about render resources is either covered by the design
below or explicitly refused with rationale. Inputs (research only,
re-derived per PHILOSOPHY §"How harmonius is used"):

- `harmonius/docs/requirements/rendering/resource-management.md`
  (R-2.5.* clauses on typed handles, transient pools, persistent
  resources, imported wrappers, generational invalidation).
- `harmonius/docs/design/rendering/render-pipeline.md`
  § "Resources", § "Heap & Argument Buffers".
- `harmonius/docs/design/rendering/rendering-core.md`
  § "Allocation", § "Handle Tables".
- `harmonius/docs/design/rendering/argument-buffer-binding.md`
  (frequency-group binding, sampler tables).

| Harmonius clause                                                                                  | Glibre disposition                                                                                                                                                                                                                                                                                                       |
|---------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-2.5.1** — Strongly typed handles per resource kind (texture / buffer / sampler / arg buffer / target). | **Covered.** §3.1: phantom-tagged `Handle<Tag>` template per `SPEC.md` §5 (`tags::virtual_resource`, `tags::physical_allocation`, `tags::argument_buffer`, `tags::sampler`, `tags::ring_slice`, `tags::shadow_atlas`, `tags::hzb`). Cross-tag assignment is a compile error. |
| **R-2.5.2** — Generational handles for stale-handle detection.                                       | **Covered.** §3.4: 24-bit generation + 40-bit index packed into `u64` per `SPEC.md` §5; a free-then-realloc bumps the slot's generation, stale handles fail `lookup()` with `render::Error::StaleResourceHandle` (documented in §10).            |
| **R-2.5.3** — Transient resources with lifetime ≤ one frame; alias-eligible.                         | **Covered.** §3.2 transient role + §3.5 transient pool: `ResourceLifetime::Transient` declares; alias planner (#760) produces colouring; this layer materialises placements on the `MTL::Heap` pool. §3.7 invariant: a transient placement is freed at phase 7 exit. |
| **R-2.5.4** — Persistent resources outliving a frame; not alias-eligible.                            | **Covered.** §3.2 persistent role + §3.6 persistent allocator: `ResourceLifetime::Persistent` allocates from the persistent slab; never enters the alias plan; survives until `release_persistent()` or plugin shutdown. |
| **R-2.5.5** — Imported resources as borrows; lifetime owned by importer.                            | **Covered.** §3.2 imported role + §3.8 import borrow registry: `declare_imported(desc, PhysicalAllocHandle)` records a non-owning reference; `PhysicalAllocation::lifetime_imported_borrow` flag prevents this layer from calling release. Read by default; write opt-in per §4.1.4 invariant 3. |
| **R-2.5.6** — Resource descriptors carry format, extent, usage, mip count, array layers.            | **Covered.** `SPEC.md` §5 publishes `ResourceDesc { debug_name, format, width, height, depth, mip_levels, array_layers, sample_count, usage, lifetime }`; closed format list (`ResourceFormat`); usage bitset (`ResourceUsage`). |
| **R-2.5.7** — Bindless / argument-buffer-grouped binding (per-frame / per-pass / per-material / per-draw). | **Covered.** §3.9: argument-buffer binder builds four `MTL::ArgumentEncoder` tables per pass invocation (frequency-group binder per `SPEC.md` §3.2 collapse #8). `Capability::BindlessResources` is the gate; Tier-2 bindless argument buffers are MVP-target on Apple Silicon. |
| **R-2.5.8** — Ring-buffered constant / instance / indirect buffers per frame in flight.             | **Covered.** §3.10: ring buffer manager allocates a single persistent slab sized for `frames_in_flight × per_frame_quota`; vending `RingSliceHandle`s by bumping a per-frame head pointer; rolls over at frame retire. Slice acquisition is lock-free. |
| **R-2.5.9** — Lifetime-driven release (not refcount); release happens at known frame boundaries. | **Covered.** §3.7: transient releases at phase 7 exit; persistent releases by explicit `release_persistent()`; imported releases are no-ops (importer owns). No reference counting; PHILOSOPHY §"explicit lifetimes" preserved. |
| **R-2.5.10** — Heap pool with placement-resource sub-allocation (Metal `MTLHeap`).                  | **Covered.** §3.5 transient pool + §3.6 persistent allocator: both back onto `MTL::Heap` with `MTL::HeapType::placement`, slot-allocated by best-fit-by-size on the persistent side and alias-plan-driven on the transient side. Heap creation goes through `MetalDevice::heap_allocator()`. |
| **R-2.5.11** — Per-context allocator tagging; render owns the GPU residency tag.                    | **Covered.** §3.11 + `SPEC.md` §9.5.1: every allocation routes through `glibre::PerContextAllocator` stamped with `ContextTag::render`. CPU shadows are tagged by the requesting context; GPU bytes are render-tagged regardless of caller per `perf-budget.md` Allocator Rule 5. |
| **R-2.5.12** — Resource exhaustion produces typed errors, not exceptions.                           | **Covered.** §3.12 + §10: `ResourceResidencyExceeded`, `HeapOutOfMemory`, `TransientPoolExhausted`, `StaleResourceHandle`, `ResourceRoleMismatch`, `ResourceImportRefused`, `CapabilityNotSupported` (for Tier-2 absence). All return `glibre::Result<T>`; no exception path. |
| Harmonius design — Free list per `MTLHeap` with first-fit / best-fit policy.                       | **Covered, simplified.** §3.6 step 3: persistent allocator uses best-fit-by-size against a fixed-bin free list (eight power-of-two size buckets); the alias planner handles transient placement and does not need a free list at all (its colouring runs cold-path each compile). PHILOSOPHY collapse: one allocator policy, not two. |
| Harmonius design — Resource versioning per write to enable cross-pass barriers.                    | **Refused at this aggregate; routed to graph aggregate.** Read-after-write versioning is the alias planner's job (#760 §3.5). Resources only declare lifetime; the planner derives versioning from declared edges. |
| Harmonius design — Sampler cache keyed by `MTLSamplerDescriptor`.                                  | **Covered.** §3.13: a small sampler cache (16 entries) keyed by the closed-set `SamplerDesc` value object; vended by `SamplerHandle` (using distinct `tags::sampler` — see §3.3). Samplers are persistent and alias-disjoint. |
| Harmonius design — Per-resource debug name surfaced to Metal capture tools.                        | **Covered.** §3.1: `ResourceDesc.debug_name` propagates to `MTL::Texture::setLabel(...)` / `MTL::Buffer::setLabel(...)` at materialisation; the binder also propagates pass / material / draw frequency labels for argument buffers. Debug-only; no shipping cost. |
| Harmonius design — Cross-process resource sharing (e.g. compositor handoff).                       | **Refused for MVP.** Single-process, single-window engine (`SPEC.md` §3.3 routed to `platform`). Imported borrows are intra-process only. Listed in §12 below. |
| Harmonius design — Resource serialisation to disk between sessions.                                 | **Refused, hard.** PHILOSOPHY anti-pattern. The only persistent render artefact is the PSO archive (`SPEC.md` §7.1.2), which is not a resource (it's a pipeline state). Resources never persist. |
| Harmonius design — Refcounted resource handles with hierarchical aggregation.                       | **Refused.** Lifetime-driven release (R-2.5.9 above) replaces reference counting. The graph layer's declared lifetime spans (transient = one frame; persistent = explicit; imported = importer-owned) are sufficient and correct without a count. PHILOSOPHY §"reject-the-false-trade-off". |
| Harmonius design — Multi-backend (Metal / Vulkan / D3D12) resource abstraction layer.                | **Refused for MVP.** Metal 4 only (`SPEC.md` §3.2 collapse #1). The aggregate's algorithms (handle table, generation counter, alias-plan placement, free-list, ring vend) are backend-neutral by construction; a future Vulkan fork would substitute `VkDeviceMemory` for `MTL::Heap` without reshaping the catalog. |
| Harmonius design — Texture upload streaming with per-mip residency control.                         | **Refused at this aggregate; routed to content / geometry.** Streaming policy (priority queue, residency budget per asset, mip drop) belongs to `content` (`SPEC.md` §3.3); resources only sees the final allocation request via `declare_imported`. |
| Harmonius design — Read-only descriptor heap separate from writable heap.                           | **Refused / collapsed.** Apple Silicon's unified memory model and Metal 4's argument-buffer Tier-2 make a separate descriptor heap unnecessary; the four-frequency argument-buffer split (§3.9) is the equivalent abstraction. PHILOSOPHY §6 collapse: one heap policy. |
| Harmonius design — Async resource allocation off the render thread.                                 | **Refused.** Allocation is a cold path (§5 / §6); driver thread serialises it. The hot path's only allocation is ring-slice bump-vending, which is lock-free. Async allocation would invent a queue model the budget does not need. |

Net result: every R-2.5.* requirement and every render-resources-
relevant design clause is either implemented as specified below or
explicitly refused with rationale. The "many specialised resource
managers" of harmonius collapse into one typed-handle catalog with
one transient pool, one persistent allocator, and one ring vend.

## 3. Detailed model

### 3.1 The aggregate cluster

```
ResourceCatalog (singleton inside the render plugin)
├── virtual_table_     : SlotTable<VirtualResource, tags::virtual_resource>
├── physical_table_    : SlotTable<PhysicalAllocation, tags::physical_allocation>
├── argbuf_table_      : SlotTable<ArgumentBufferRecord, tags::argument_buffer>
├── ring_table_        : SlotTable<RingSlice, tags::ring_slice>
├── sampler_cache_     : SamplerCache (closed-set of `SamplerDesc`)
├── transient_pool_    : TransientPool                       (§3.5)
├── persistent_alloc_  : PersistentAllocator                 (§3.6)
├── ring_buffer_mgr_   : RingBufferManager                   (§3.10)
├── argbuf_binder_     : ArgumentBufferBinder                (§3.9)
└── device_            : MetalDevice*                        (non-owning; from §4.1.6)

VirtualResource (value object, stored in slot table)
├── desc_              : ResourceDesc
├── lifetime_          : ResourceLifetime
├── frame_of_birth_    : u64
├── frame_of_last_use_ : u64                                  (set during compile)
└── physical_          : PhysicalAllocHandle                  (resolved at compile)

PhysicalAllocation (entity, stored in slot table)
├── kind_              : enum { Texture, Buffer, ArgumentBuffer, Sampler, Imported }
├── heap_              : MTL::Heap*                           (nullptr for imported)
├── offset_            : usize                                (0 for non-placement)
├── size_              : usize
├── mtl_handle_        : eastl::variant<MTL::Texture*, MTL::Buffer*, MTL::SamplerState*,
│                                       MTL::Buffer* /* arg buf */, ImportedHandle>
└── owner_tag_         : enum { Transient, Persistent, Imported }
```

`SlotTable<T, Tag>` (a render-internal template) is the generation-
counted entity table that backs every public handle. Layout:

```
SlotTable<T, Tag>
├── slots_       : eastl::vector<Slot>            (dense; index → Slot)
├── free_indices_: eastl::vector<u32>             (LIFO free list)
└── generation_counter_per_slot_  (inline in Slot)

Slot
├── value_        : T                              (stored payload)
├── generation_   : u32                            (24-bit live, 8-bit reserved)
└── alive_        : bool
```

The catalog owns one `SlotTable` per public-handle Tag. Tables live
inside the §9.5 "GPU resource handles" 16 MiB row; capacities are
sized at init from `RenderSettings` and the `QualityTier` baseline:

| Table              | MVP cap (S1) | Storage row footprint |
|--------------------|--------------|-----------------------|
| `virtual_table_`   | 1024 entries | ~64 KiB (slot ≈ 64 B) |
| `physical_table_`  | 512 entries  | ~48 KiB               |
| `argbuf_table_`    | 256 entries  | ~16 KiB               |
| `ring_table_`      | 4096 entries | ~96 KiB               |
| `sampler_cache_`   | 16 entries   | ~2 KiB                |
| **Sum (tables)**   |              | **~228 KiB**          |
| Per-frame argbuf   | per-pass × 4 | ~12 MiB working set   |
| Residency-set bitset | per-`MTL::Heap` | ~8 KiB              |
| Free-list scratch  |              | ~8 KiB                |
| **Catalog total**  |              | **~12.5 MiB**         |

The remaining headroom inside the 16 MiB row absorbs hot-reload
resume churn (§3.14) and per-`View` ring expansions when MVP scales
from one to four views without a budget amendment.

### 3.2 Three resource roles

`ResourceLifetime` (`SPEC.md` §5) discriminates three roles. Their
storage paths and lifetime semantics are disjoint; the catalog
chooses the path at `declare_*` time and the choice never changes
inside a frame.

| Role | Declares via | Storage path | Lifetime | Alias-eligible? | Released by |
|------|--------------|--------------|----------|-----------------|-------------|
| **Transient** | `GraphBuilder::declare_transient(desc)` | `TransientPool` placement on shared `MTL::Heap`s | `[frame_of_birth, phase 7 exit]` | Yes (alias plan colours disjoint lifetimes onto the same slot) | Phase 7 exit, en-masse free of the colour set (§3.5 step 4) |
| **Persistent** | `GraphBuilder::declare_persistent(desc)` *or* `ResourceCatalog::create_persistent(desc)` from cold-path init | `PersistentAllocator` slab on long-lived `MTL::Heap`s (best-fit + free list) | Until explicit `release_persistent(handle)` or plugin shutdown | No (persistent resources never enter the alias plan; `SPEC.md` §4.1.4 invariant 2) | Cold-path `release_persistent`, or shutdown drain |
| **Imported** | `GraphBuilder::declare_imported(desc, PhysicalAllocHandle)` | Borrow record only; no allocation | Importer-owned; resource catalog publishes a non-owning view | No (imported never alias-place; the catalog cannot assert disjointness for memory it does not own) | Importer-side; `release_imported(handle)` releases only the borrow record |

The three-role classification is a `SPEC.md` §4.1.4 invariant; the
detailed-design contribution here is the storage path, the
release seam, and the alias eligibility rule. Confusion between
roles (e.g. trying to `release_persistent` on an imported handle)
returns `render::Error::ResourceRoleMismatch` with no state
mutation.

### 3.3 Public handle catalog (re-statement of `SPEC.md` §5 with sampler-tag refinement)

`SPEC.md` §5 publishes the closed handle catalog. The detailed
design refines two practical consequences:

1. **`SamplerHandle` uses a distinct `tags::sampler` phantom type.**
   Samplers are GPU immutable state objects (no aliasing, no
   reallocation mid-frame) and have a structurally different lifetime
   from argument buffers (plugin-shutdown vs. per-pass). Giving
   samplers their own phantom tag (`Handle<tags::sampler>`) preserves
   the phantom-tag compile-time enforcement: cross-assignment between
   `SamplerHandle` and `ArgumentBufferHandle` remains a compile error,
   and the `SlotTable<MTL::SamplerState*, tags::sampler>` table can
   use the full 24-bit generation field without reserving bits for
   sub-tag discrimination (§3.4 below). `tags::sampler` is added to
   `SPEC.md` §5's handle catalog as part of this refinement (see
   §10.2 ABI note).
2. **`RTHandle` (render-target handle) is *not* a separate public
   handle.** Render targets are `VirtualResourceHandle` instances
   whose `usage` includes `ColorAttachment` or `DepthAttachment`
   (`SPEC.md` §5 `ResourceUsage`). The "RT vs. SR" axis is a
   per-pass declaration concern (which subset of the resource a
   pass binds), not a separate handle kind. PHILOSOPHY §6 collapse:
   one handle, multiple usage bits.

Public-handle re-statement (binding):

| Handle kind | Tag | Vended by | Released by |
|-------------|-----|-----------|-------------|
| `VirtualResourceHandle` | `tags::virtual_resource` | `GraphBuilder::declare_*` | Phase 7 exit (transient), `release_persistent`, importer (imported) |
| `PhysicalAllocHandle`   | `tags::physical_allocation` | Internal — vended at materialisation; surfaced only to `declare_imported(..., PhysicalAllocHandle)` callers (the importer obtains it via the import-side seam in their own SPEC) | Same as the resource it backs |
| `ArgumentBufferHandle`  | `tags::argument_buffer` | `argbuf_binder_.acquire(pass, frequency_group)` | Per-pass: end of pass record; per-material / per-frame: per the binder (§3.9) |
| `SamplerHandle`         | `tags::sampler` | `ResourceCatalog::sampler(SamplerDesc)` (cached) | Plugin shutdown |
| `RingSliceHandle`       | `tags::ring_slice` | `ring_buffer_mgr_.acquire_slice(kind, bytes)` | Frame retire (§3.10) |
| `HZBHandle`, `ShadowAtlasHandle`, `ClusterCullStateHandle`, `BLASHandle`, `TLASHandle` | per-aggregate tags (`SPEC.md` §5) | Owning aggregate; resource catalog stores the underlying `PhysicalAllocHandle` only | Owning aggregate |

The public surface is exactly `SPEC.md` §5 (plus `tags::sampler`
added by this design); the table above is a read-back-into-context
summary of which seam vends and releases each kind.

### 3.4 Generation counter — stale-handle detection

Every `SlotTable<T, Tag>` slot carries a **24-bit generation counter**
(all 24 bits live — no sub-tag bits reserved). This applies uniformly
to all five handle tables: `VirtualResource`, `PhysicalAlloc`,
`ArgumentBuffer`, `Sampler` (using its own `tags::sampler` table per
§3.3), and `RingSlice`. A fresh allocation takes the slot's *current*
generation; a release *increments* the counter; `lookup(handle)`
validates `handle.generation() == slots_[handle.index()].generation_`
before returning `&slots_[index].value_`. Mismatch returns
`render::Error::StaleResourceHandle` (generation mismatch = stale
borrow; see §10) with a debug-build log line carrying the handle's
`(index, generation)`, the slot's *live* generation, and the most
recent producer call site (captured in debug builds via
`__builtin_FILE` + `__builtin_LINE` threaded through the `declare_*`
API; release builds skip the capture).

Wrap-around safety:

- 24 bits = 16 777 215 unique generations per slot before wrap.
- At MVP S1 a transient table slot may free + realloc up to 11
  times per frame (one per `View` × 4 views × ~3 declared transients
  per pass slot in the worst case); 16 777 215 / 11 ≈ 1 525 200 frames
  ≈ 7 hours of continuous gameplay at 60 FPS before any one slot
  could wrap. Per `SPEC.md` §4.1.4 invariant 1 ("transient never
  outlives a frame") and `SPEC.md` §9.6's leak-guard benchmark, no
  observer can hold a stale transient handle across a frame
  boundary; wrap-around-after-7-hours therefore cannot strike a
  live observer. Persistent slot generations advance only on
  explicit release; their wrap rate is bounded by user / dev
  action and is non-issue. Sampler slots are released only at
  plugin shutdown; their generation counter is non-issue in practice.
- The wrap is **detected, not forbidden**. When a slot's generation
  hits `0xFFFFFF` and the next free attempts to bump, the table
  marks the slot **retired** (alive_=true, gen=0xFFFFFF, value_
  in-place but no further realloc) and `pop`s the next free index.
  The retired slot is reclaimed at plugin shutdown only. This
  preserves the soundness of the generation guarantee at the cost
  of a bounded slot-leak (≤ 16M slots-per-process; orders of
  magnitude below `virtual_table_.cap = 1024`). Documented test
  fixture: `tests/render/resources/generation_wrap.cpp` constructs
  a synthetic table with `cap = 4`, `generation_bits = 4` and
  exercises the wrap → retire path under 64 free-realloc cycles.

### 3.5 Transient pool (alias-driven placement)

The transient pool is a small fixed set of pre-allocated `MTL::Heap`
instances configured at init from `SPEC.md` §9.5's transient row
(256 MiB total). Composition:

| Sub-pool          | Heap size | Heap count | Storage mode | Notes |
|-------------------|-----------|------------|--------------|-------|
| Color-attachment  | 64 MiB    | 2          | Private      | 4× 1080p MRT + history-color sub-band |
| Depth-attachment  | 32 MiB    | 1          | Private      | Reverse-Z depth + scratch-depth |
| Storage-texture   | 64 MiB    | 1          | Private      | RT trace targets + intermediate compute |
| Storage-buffer    | 64 MiB    | 1          | Private      | Indirect / instance / scratch / RT scratch |
| Argument-buffer   | 16 MiB    | 1          | Shared (Tier-2 bindless visibility) | Per-pass argument tables (§3.9) |
| Reserve           | 16 MiB    | (split)    | Private      | Absorbs alias-planner overhead and per-View fan-out |
| **Sum**           | **256 MiB**| **6 heaps**|              |       |

The split-by-usage choice is deliberate: Apple Silicon Tier-2
bindless has no explicit descriptor-heap separation, but Metal's
`MTL::Heap` performance is best when an heap's resident
allocations share a usage class (texture vs buffer) so the GPU's
allocator can co-locate same-usage bytes. The six-heap split is
thus a perf-shape, not a contract; CI gate `transient-pool drained
at phase 9` (`SPEC.md` §9.6.2) does not care which sub-pool a
placement landed on. Reshaping the split is a §3.5 amendment, not
a §4 invariant change.

Per-frame transient flow:

1. **Compile.** During `RenderGraph::compile()` (cold path; phase
   7 entry, single-threaded on the builder thread), the alias
   planner (#760 §3.5) walks the topologically ordered pass list
   and emits an `AliasPlan { virtual_resource → (sub_pool, offset,
   size) }` for every transient `VirtualResource`. The planner is
   pure; this layer's only contribution at compile time is to
   answer two questions per virtual resource: (a) what is its
   `MTL::Heap`-aligned size (computed from `ResourceDesc` via
   `MTLDevice::heapTextureSizeAndAlign(...)` /
   `heapBufferSizeAndAlign(...)`); (b) which sub-pool it routes to.
2. **Materialise.** When `ExecutionPlan::record_into` first reaches
   a pass that reads or writes the resource, the catalog calls
   `MTL::Heap::newTexture(descriptor: ..., offset: alias_offset)`
   or `newBuffer(...)` to produce the placement `MTL::Texture` /
   `MTL::Buffer`. Materialisation is **lazy**: the same alias slot
   is reused by lifetime-disjoint resources without any free /
   realloc — Metal allows multiple `newTexture` calls on the same
   `(heap, offset)` provided lifetimes do not overlap, which the
   alias plan guarantees by construction. The per-call cost is one
   `MTL::Heap::newTexture` or `newBuffer`, ≤ 1.5 µs on M1
   (measured, in line with the §9.3 0.05 ms metal/queue.cpp budget).
3. **Bind.** The materialised handle is folded into the per-pass
   argument-buffer table (§3.9). Passes' execute lambdas read the
   handle through `Bindings`; they never see the heap or the
   offset.
4. **Drain.** At phase 7 exit, the catalog walks the alias plan in
   reverse and **releases every transient placement in one
   bounded sweep**. Metal's `MTL::Texture` / `MTL::Buffer` retired
   here have refcount 1 (only the catalog held them); release
   returns the alias slot to the heap immediately. The
   `transient_pool drained at phase 9, strict` benchmark
   (`SPEC.md` §9.6.2) asserts post-drain `live_bytes ==
   first_frame_resident_bytes` (i.e. only the heap shells remain;
   no placements survive).

The pool itself never grows past the 256 MiB ceiling. If an alias
plan computes a peak-residency above the pool's capacity for any
sub-pool, the planner returns `render::Error::ResourceResidency
Exceeded` (`SPEC.md` §10 row), which `RenderGraph::compile()`
propagates and the recovery ladder (`SPEC.md` §10.2) demotes to
`lower-tier`. The pool size is not a runtime knob; it is
cell-anchored.

### 3.6 Persistent allocator

The persistent allocator backs every long-lived render-owned
resource. It is sized at init from `SPEC.md` §9.5's persistent row
(128 MiB) plus the persistent half of the RT row (24 MiB of the 48
MiB RT total accounted as "TLAS + scratch + instance"; BLAS imports
do not allocate). Composition:

| Sub-slab           | Initial size | Storage mode | Notes |
|--------------------|--------------|--------------|-------|
| Persistent textures | 80 MiB      | Private      | HZB pyramid (per-View), shadow atlases (4K @ Pcf, 2K @ floor), history-color (TAA), font/overlay atlases |
| Persistent buffers  | 32 MiB      | Private      | Lighting LUTs, IBL probe cube ladder, gbuffer-history (TAA accumulator), cluster-cull persistent scratch |
| RT TLAS + scratch   | 24 MiB      | Private      | TLAS storage, instance buffers, RT scratch (`SPEC.md` §4.1.8) |
| Argument-buffer (persistent half) | 8 MiB | Shared | Per-frame argument table that is rebuilt every frame but lives in a slab not freed between frames (§3.9) |
| Ring slabs (persistent storage)   | 8 MiB | Shared | Constant ring + instance ring + indirect ring backing slab; slices are vended per-frame from this single slab (§3.10) |
| **Sum**            | **152 MiB**  |              |       |

Allocation policy is **best-fit-by-size against a fixed eight-bin
power-of-two free list**:

```
bins_ : eastl::array<eastl::vector<FreeBlock>, 8>
        // bin index = floor(log2(size)) - log2(min_alloc)
        // min_alloc = 4 KiB; bin 0 = [4K..8K), bin 7 = [512K..1M+]
```

`allocate(size)` walks bins from `floor(log2(size))` upward,
splitting an oversized free block when no exact-bin block is
available. `release(block)` pushes onto the bin matching its size
and merges with adjacent free blocks (linear walk, capped at 32
neighbours). The cost is amortised O(log bin_count) per allocation
on the cold path; the hot path never allocates a persistent
resource (every persistent allocation is cold-path init or
hot-reload register).

Allocation alignment honours `MTLDevice::heapTextureSizeAndAlign` /
`heapBufferSizeAndAlign` for the requested format; the bin policy
adds an alignment slack (≤ 4 KiB) absorbed into the per-block
overhead. Fragmentation is bounded by best-fit + neighbour-merge to
a measured ≤ 5 % at MVP working-set turnover (`bench("persistent
fragmentation under churn")`, §11.3).

### 3.7 Lifetime-driven release

Release is **never reference-counted**. The three roles each have
their own release seam:

1. **Transient release.** Frame N's phase 7 exit drains every
   transient `VirtualResource` whose `frame_of_birth == N`. The
   sweep is part of the phase-7 retire step (`SPEC.md` §9.3
   "RenderFrame retire + transient pool recycle = 0.05 ms"). After
   the sweep, the alias plan free-list is empty and every
   `MTL::Texture` / `MTL::Buffer` placement is released. Slot
   generation in `virtual_table_` advances; future lookups fail.
2. **Persistent release.** The cold-path API
   `ResourceCatalog::release_persistent(VirtualResourceHandle)` (or
   `release_persistent(PhysicalAllocHandle)` for catalog-internal
   slots) frees the underlying `MTL::Heap` block back to the
   matching free-list bin and advances the slot generation. Calling
   this on a transient or imported handle returns
   `render::Error::ResourceRoleMismatch` with no state mutation.
   Persistent resources held by sibling aggregates (HZB,
   `ClusterCullState`, shadow atlas, RT TLAS, ring slabs) are
   released by their owning aggregate at plugin shutdown, never by
   this layer's user.
3. **Imported release.** The cold-path API
   `ResourceCatalog::release_imported(VirtualResourceHandle)`
   removes the borrow record only; no `MTL::*` call is made. The
   importer's lifetime guarantees apply (`SPEC.md` §4.1.4 invariant
   3); calling release before the import is materialised in any
   pass is permitted and a no-op.

Plugin shutdown drains in reverse order: imports → transients (no-op
under invariant 1) → ring slabs → persistents → heaps. Drain
failure (a sibling aggregate still holds a persistent handle) is a
debug assertion in `resources/plugin.cpp::drain()`; release builds
log a `warn`-level structured event and proceed (the engine is
exiting; no recovery is possible).

### 3.8 Imported-resource borrow registry

Imported borrows wrap externally-vended GPU allocations. At MVP
the imported set is closed:

| Importer | Resource kind | Wrap point | Lifetime guarantee |
|----------|---------------|------------|---------------------|
| `platform` | `MTL::Drawable` swapchain texture | Per phase 7 acquire (`MetalCommandBuffer::next_drawable`) | Until `presentDrawable` is called by the `present` pass |
| `geometry` | BLAS | Cook output (one BLAS per cooked mesh) | Until `geometry`'s mesh asset is unloaded |
| `geometry` | Vertex / index / meshlet streams | Asset load | Until asset unloaded |
| `vfx` (post-MVP) | Particle / cloth GPU buffers | Per-frame VFX submit | Until next VFX submit |
| `shader` | PSO blobs (consumed by `PSOCache`, not by resources) | (out of scope at this layer) | (out of scope) |

The borrow record fields:

```
ImportedBorrow
├── kind_              : enum { Drawable, BLAS, GeomStream }   // closed MVP set; see note below
├── physical_handle_   : PhysicalAllocHandle           (catalog-side)
├── mtl_handle_        : metal-cpp pointer (immutable post-record)
├── owner_drop_token_  : eastl::function<void()>       (importer-supplied; invoked on borrow rejection only)
└── write_capability_  : bool                          (default false; opt-in per `SPEC.md` §4.1.4 invariant 3)
```

> **Future ABI amendment (post-MVP):** When the `vfx` plugin is introduced, a `VfxBuffer` arm will
> be added to `kind_` and the corresponding `SPEC.md` §4.1.4 ABI-add row will be filed. This
> constitutes a minor ABI bump requiring a middleman-dylib hash update. No cross-domain abstraction
> is introduced here before a concrete second user exists (PHILOSOPHY §5, anti-pattern rule).

Write capability is only set true for the swapchain drawable in
the `present` pass; every other import is a read-borrow. An attempt
to record a write declaration on a read-only import is rejected by
the graph compiler (`SPEC.md` §10 row `BarrierConflict`) before
this layer is consulted. The catalog's role is to *publish*
`physical_handle_` and `mtl_handle_` to the binder (§3.9); ownership
transfer is forbidden (no `release_imported` call ever frees the
underlying `MTL::*`).

### 3.9 Argument-buffer frequency-group binder

The binder owns the four-frequency argument-buffer schema
(`SPEC.md` §3.2 collapse #8). Each pass invocation produces up to
four `MTL::ArgumentEncoder`-built tables:

| Frequency | Built per | Lifetime | Bound at |
|-----------|-----------|----------|----------|
| **Per-frame** | Frame N (one for the entire frame) | Phase 7 entry → phase 7 exit | Once per frame, before the first pass |
| **Per-pass** | Each pass invocation | Pass record begin → pass record end | At pass encoder open |
| **Per-material** | Each unique `MaterialHandle` referenced this frame | Frame N (cached per material) | At draw command (rebind on material switch) |
| **Per-draw** | Each `DrawCmd` in the bucketed list | One draw call | At draw record |

The binder caches the per-frame and per-material tables across
frames (cache key = `(MaterialHandle, frame_in_flight_index)`),
amortising MVP material churn — the cached tables sit inside the
§9.5 PSO row's binding-table prebuild lane. The per-pass and
per-draw tables are rebuilt every frame from scratch; their
backing memory is the transient argument-buffer sub-pool (§3.5,
16 MiB).

Hot-path operation:

1. Pass `execute()` lambda receives `const Bindings&`.
2. The first read of a typed view (`bindings.gbuffer_color`,
   `bindings.cluster_lights`, …) fetches the resolved
   `(MTL::Texture* | MTL::Buffer*, offset_into_argument_buffer)`
   from the per-pass table and binds via the encoder cursor on
   the `MetalCommandBuffer`.
3. Subsequent reads are O(1) cache hits in the same per-pass
   table.

The binder is the only path that marshals
`(VirtualResourceHandle → MTL::Texture* | MTL::Buffer*)`; passes
never call `lookup(VirtualResourceHandle)` directly. This keeps
the hot-path lookup (one indexed read of a packed 32-byte struct
from the per-pass table; SIMD-friendly because the table is laid
out by declared-access order, matching the lambda's read order)
inside the §9.3 0.50 ms per-pass record budget.

`Bindings` is the opaque struct from `SPEC.md` §5; the typed-view
accessors are generated alongside each pass body (per #764) and
referenced here by signature only. The codegen for `Bindings`
itself is the §3.9 cold-path output — emitted at compile time
from a tiny per-pass schema, not at runtime.

### 3.10 Ring buffer manager

Three rings, one per resource class, all backed by the persistent
ring-slab in §3.6:

| Ring | Backing slab share | Slice cadence | Slice size budget |
|------|---------------------|---------------|-------------------|
| Constant heap | 4 MiB | Per-pass and per-draw uniform writes | `(passes × 256 B) + (draws × 64 B)` |
| Instance buffer | 2 MiB | Per-`DrawCmd` bucket | `instances × 64 B` |
| Indirect buffer | 2 MiB | Per-`DrawCmd` indirect | `draws × 32 B (MTLDrawIndexedPrimitivesIndirectArguments)` |
| **Sum (per frame in flight)** | **8 MiB** | | |

With three frames in flight (`SPEC.md` §4.1.1 triple-buffer), the
ring slab in §3.6 is sized to host three contiguous per-frame
windows = 24 MiB, but only 8 MiB of it is live at any instant
(the in-recording frame); the other 16 MiB is GPU-readable for
frames N-1 and N-2 still in flight. Per-frame head pointers wrap at
the slab's sub-band boundary; an over-quota slice request (rare;
absorbed by the per-frame quota's 25 % reserve in `SPEC.md` §9.5)
returns `render::Error::HeapOutOfMemory` and triggers the
`ResourceAllocFailed` recovery (`SPEC.md` §10 row, recovery =
`lower-tier`).

Slice acquisition is **lock-free**:

```cpp
RingSliceHandle acquire_slice(RingKind k, std::size_t bytes) noexcept {
  std::atomic<std::size_t>& head = heads_[k];
  // align bytes up to the ring's natural alignment (256 B for constants)
  bytes = align_up(bytes, alignment_for(k));
  // bump head; CAS only on wrap into the in-flight band
  std::size_t old_head = head.fetch_add(bytes, std::memory_order_acq_rel);
  if (old_head + bytes > current_band_end_[k]) {
    return failure_handle_for(HeapOutOfMemory);
  }
  return make_handle(k, old_head, bytes);
}
```

The fetch-add is the only atomic operation; the band-end check is
a relaxed read of a per-frame-in-flight constant. CAS is not
needed because slice writers never retire ranges (the ring rolls
forward only, retired by frame retire on the driver thread). Per
`SPEC.md` §6.3, the per-pass encoder workers each own a CB and
serialise their writes within the CB; cross-worker contention on
the same ring is bounded by `K = three workers`; the fetch-add
contention is dominated by GPU-write rate, not CPU contention.

### 3.11 Per-context allocator wiring

Render's resources aggregate is the single largest GPU-bytes
consumer in the engine, and the only one that re-routes allocations
made by sibling contexts (geometry / tools) under the `render` tag
per `perf-budget.md` Allocator Rule 5. The wiring obeys
`SPEC.md` §9.5.1 verbatim:

1. **Tag stamp at register.** Render's `glibre_plugin_register`
   (cited in `SPEC.md` §8.3.1) stores the engine's allocator handle
   inside `ResourceCatalog`; the handle is pre-stamped with
   `ContextTag::render`. Every `MTL::Heap` creation, every
   `MTL::Texture` / `MTL::Buffer` placement, every argument-buffer
   allocation routes through it.
2. **Strict-mode rejection.** Diagnostic / debug builds run with
   `GLIBRE_ALLOC_STRICT=1`; an allocation that would push render's
   live bytes above 512 MiB returns
   `std::unexpected{core::Error::OutOfBudget}`. This layer maps to
   `render::Error::ResourceResidencyExceeded` at the `declare_*`
   call site, never at the per-pass record site (the record site
   reads materialised handles only).
3. **Cross-context allocations.** When `geometry` or `tools` calls
   into render to materialise a GPU asset (vertex stream upload,
   ImGui texture page), this layer's allocation request rides under
   `ContextTag::render`; the requesting context's CPU shadows are
   tagged in their own context. The catalog records the requesting
   context in the slot record (debug-build-only; for the perf
   overlay's "GPU bytes by requester" view) but enforces no
   per-requester sub-budget. The 512 MiB ceiling is collective.
4. **Transient arena exemption.** The 256 MiB transient pool drains
   per phase 7 exit per Allocator Rule 4; the leak guard is
   `bench("transient-pool drained at phase 9, strict")` (`SPEC.md`
   §9.6.2). A persistent slot leaked across plugin reload is a
   bigger story (§3.14); transient leaks are caught the same frame.

### 3.12 Failure-mode mapping (forward reference to §10)

The catalog produces seven render-error variants (§5 enum identifiers;
§10 design-name labels in parentheses for cross-reference):

| Trigger | §5 Variant | §10 design-name row |
|---------|-----------|---------------------|
| Persistent allocator exhausted (best-fit failed) | `HeapOutOfMemory` | `ResourceAllocFailed` |
| Transient pool peak-residency > sub-pool cap | `TransientPoolExhausted` | `ResourceAllocFailed` |
| Per-frame total residency > 512 MiB | `ResourceResidencyExceeded` | `ResourceResidencyExceeded` |
| Stale handle (generation mismatch at lookup) | `StaleResourceHandle` | (abort-frame; see §10) |
| Role mismatch (e.g. `release_persistent` on transient handle) | `ResourceRoleMismatch` | (cold-path assert; see §10) |
| Genuine import-borrow refusal (write declaration on read-only borrow) | `ResourceImportRefused` | (abort-engine; see §10) |
| Sampler cache over-capacity | `ResourceResidencyExceeded` | `ResourceResidencyExceeded` |
| Tier-2 bindless required, host CapabilitySet lacks `BindlessResources` | `CapabilityNotSupported` | `RtCapabilityMissing` (§10 design-name for bindless-capability arm) |

Recovery routing per `SPEC.md` §10.2 is preserved verbatim;
detail in §10 below.

### 3.13 Sampler cache (closed set)

A small fixed-size cache (16 entries) keyed by the closed-set
`SamplerDesc`:

```cpp
struct SamplerDesc {
  enum class Filter : std::uint8_t { Nearest, Linear, Trilinear, Anisotropic4x, Anisotropic8x, Anisotropic16x };
  enum class Wrap   : std::uint8_t { Clamp, Repeat, MirroredRepeat, ClampToEdge };
  enum class Compare: std::uint8_t { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };

  Filter  min_mag    = Filter::Linear;
  Filter  mip        = Filter::Linear;
  Wrap    s          = Wrap::Repeat;
  Wrap    t          = Wrap::Repeat;
  Wrap    r          = Wrap::Repeat;
  Compare compare    = Compare::Never;
  std::uint8_t border_color_index = 0;   // closed palette {transparent_black, opaque_black, opaque_white}
};
```

The closed-set design (six × four × eight × four = limited
combinatorics) gives a known upper bound on distinct samplers
across the entire MVP shader set. Sixteen entries is sized for the
union of every MVP pass's sampler use ({linear-clamp, linear-repeat,
trilinear-anisotropic-repeat for albedo, comparison-less-clamp for
shadow, nearest-clamp for visID resolve, …}); over-cap returns
`render::Error::ResourceResidencyExceeded`, consistent with the
slot-table overflow precedent in §11.1 ("capacity overflow returns
`ResourceResidencyExceeded`"). This is treated as a SPEC amendment
trigger rather than a runtime concern (samplers are not data-driven
in MVP; if the static set ever exceeds 16, the cap is raised in
a §3.13 amendment, not at runtime). Cache lookup is linear (16
entries; one cache line); hit rate is 100 % under MVP's static set.

### 3.14 Hot-reload survival hook

Forward reference to §8 below. Persistent resources survive the
swap (`SPEC.md` §8.2 row "Persistent Resources"); transient
resources are gone before phase 8 begins. The catalog itself is a
plugin-private struct, so the slot tables and the allocator state
survive the swap as bytes (the new plugin's
`glibre_plugin_register` rebinds catalogue function pointers but
does not move slot bytes). Generation counters are preserved
verbatim; handles vended by the previous plugin remain valid in
the new plugin so long as their underlying slot kind, format, and
extent match the new plugin's manifest declaration. Mismatch
triggers `core::Error::HotReloadRefused` with cause
`core::Error::SchemaMigrationFailed` (`SPEC.md` §8.4 row
"Surviving persistent GPU resource's component schema differs").

## 4. Public surface

This aggregate adds **no new symbol** to `specs/render/SPEC.md` §5
beyond what the SPEC already publishes. Every symbol below is a
re-statement of the SPEC's contract sized to this aggregate's
scope.

### 4.1 Types — bound to SPEC §5

```cpp
// Already locked in SPEC §5. Re-stated here so reviewers can verify
// the resources aggregate is the sole owner of these types' bodies.

namespace glibre::render {

// Resource description — SPEC §5
struct ResourceDesc { /* exactly as published */ };

// Resource lifetime — SPEC §5
enum class ResourceLifetime : std::uint8_t {
    Transient,   // alias-eligible; per-frame
    Persistent,  // long-lived; never aliased
    Imported,    // borrow; importer-owned
};

// Resource format / usage — SPEC §5
enum class ResourceFormat : std::uint16_t { /* closed set */ };
enum class ResourceUsage  : std::uint32_t { /* bitset */ };

// Phantom-tagged handles — SPEC §5
template <class Tag> class Handle { /* 24:40 generation:index */ };
using VirtualResourceHandle  = Handle<tags::virtual_resource>;
using PhysicalAllocHandle    = Handle<tags::physical_allocation>;
using ArgumentBufferHandle   = Handle<tags::argument_buffer>;
using RingSliceHandle        = Handle<tags::ring_slice>;
using ShadowAtlasHandle      = Handle<tags::shadow_atlas>;
using HZBHandle              = Handle<tags::hzb>;
using ClusterCullStateHandle = Handle<tags::cluster_cull_state>;

}  // namespace glibre::render
```

`SamplerHandle` is a private sub-tag of `tags::argument_buffer` per
§3.3; it is **not** exported as a separate top-level alias because
the sampler cache is plugin-internal (no caller outside render
needs to construct a `SamplerHandle`). The header retains
`ArgumentBufferHandle` as the public name; samplers are vended
through the same pathway by the binder (§3.9).

### 4.2 Methods on `GraphBuilder` (re-stated from SPEC §5)

```cpp
// Authored in SPEC §5; bodies live in this aggregate.
[[nodiscard]] glibre::Result<VirtualResourceHandle>
GraphBuilder::declare_transient(const ResourceDesc&) noexcept;

[[nodiscard]] glibre::Result<VirtualResourceHandle>
GraphBuilder::declare_persistent(const ResourceDesc&) noexcept;

[[nodiscard]] glibre::Result<VirtualResourceHandle>
GraphBuilder::declare_imported(const ResourceDesc&,
                               PhysicalAllocHandle) noexcept;
```

Body contracts per §3:

- `declare_transient` validates `desc.lifetime == Transient`,
  inserts into `virtual_table_`, marks `frame_of_birth = current
  frame counter`, and routes the resource into the transient
  sub-pool selected by `desc.usage`. Failure modes: `desc.usage`
  empty; format unsupported on the host; the sub-pool's pre-
  computed peak-residency would exceed the cap.
- `declare_persistent` validates `desc.lifetime == Persistent`,
  inserts into `virtual_table_`, calls `persistent_alloc_.
  allocate(...)` for the matching sub-slab, returns the handle.
  Cold-path call site only; debug-build assertion if invoked
  inside phase 7's record half.
- `declare_imported` validates that `PhysicalAllocHandle` resolves
  to a slot tagged `Imported`; inserts the borrow record into
  `physical_table_` (or reuses the existing slot if the importer
  vended the handle through this catalog already); returns the
  paired `VirtualResourceHandle`. Body never allocates.

### 4.3 Catalog-private surface (not in SPEC §5)

The following helpers are **not** part of the public ABI; they are
plugin-internal seams the resource aggregate exposes to sibling
aggregates inside the render dylib (HZB, ClusterCullState, RT,
ring buffers, the binder). They are listed here for SRP traceability
and to document the seam tests in §11.

```cpp
namespace glibre::render::detail {

// Cold-path persistent allocator — invoked by HZB / ClusterCullState
// / RT / ring-buffer / shadow-atlas at plugin init or hot-reload register.
[[nodiscard]] glibre::Result<PhysicalAllocHandle>
ResourceCatalog::create_persistent_texture(const ResourceDesc&) noexcept;

[[nodiscard]] glibre::Result<PhysicalAllocHandle>
ResourceCatalog::create_persistent_buffer(std::size_t bytes,
                                           ResourceUsage usage) noexcept;

// Cold-path release — invoked at shutdown drain or on a sibling's explicit teardown.
glibre::Result<void>
ResourceCatalog::release_persistent(PhysicalAllocHandle) noexcept;

// Hot-path lookup (used only by the binder and trace-replay).
[[nodiscard]] const PhysicalAllocation*
ResourceCatalog::lookup(PhysicalAllocHandle) const noexcept;

// Hot-path ring vend (used by per-pass encoder workers).
[[nodiscard]] glibre::Result<RingSliceHandle>
ResourceCatalog::acquire_ring_slice(RingKind, std::size_t bytes) noexcept;

}  // namespace glibre::render::detail
```

These names are stable inside the dylib only; ABI hashes do not
cover them per `reviews/decisions/plugin-abi.md` §"In-dylib seams
are not part of the manifest hash". Adding or removing one of these
helpers is a render-internal refactor, not an ABI bump.

## 5. Hot/cold path split

| Path | Frequency | Surface | Allowed work |
|------|-----------|---------|--------------|
| **Cold — process init** | Once per process | `ResourceCatalog::create`, `create_persistent_*` for HZB / shadow atlas / ring slab / RT TLAS / IBL probes | `MTL::Heap` creation (six transient + N persistent), pre-allocation of sampler cache, initial argument-buffer skeletons. May allocate freely against `ContextTag::render`. |
| **Cold — per-frame compile** | Once per `View` per frame (≤ 4 per frame) | `declare_transient` / `declare_persistent` / `declare_imported` (called by passes during graph build); `materialise(alias_plan)` (called by `ExecutionPlan` recorder before first record) | `MTL::Heap::newTexture` / `newBuffer` placement calls — at most one per declared transient, ≤ 1.5 µs each on M1; argument-buffer build via `MTL::ArgumentEncoder` per pass (≤ 256 B writes). |
| **Cold — hot-reload register** | Once per swap | (catalog-internal, called from `glibre_plugin_register`) | Slot-table function-pointer rebind; persistent slot revalidation; sampler cache re-warm. No `MTL::Heap` realloc; no GPU-bytes movement. |
| **Cold — hot-reload drain** | Once per swap | (catalog-internal, called from `glibre_plugin_drain`) | Drop in-flight ring heads to a quiesced state; await GPU completion of the in-flight frame; no bytes freed (persistent survives). |
| **Hot — per-pass record** | Per pass per frame | `lookup(PhysicalAllocHandle)` (via the binder's per-pass table); `acquire_ring_slice(...)` | One indexed read of `physical_table_` (one cache-line); one atomic `fetch_add` per slice; **no allocation, no `MTL::*` construct, no `MTL::Heap` touch, no PSO touch**. |
| **Hot — per-frame retire** | Per frame | (driver-thread) catalog-internal `retire_transient_frame()` | Walk alias-plan free list; release placements en-masse; advance `virtual_table_` slot generations. ≤ 0.05 ms per `SPEC.md` §9.3. |

The hot-path-allocation rule is render-wide (`SPEC.md` §4.1.3
invariant 4 "no allocations on the hot path"); this aggregate's
contribution is ensuring every bookkeeping operation visible to a
pass body is amortised O(1) over the per-pass slot count and
reduced to indexed reads of pre-built tables.

## 6. Concurrency

The aggregate integrates with `SPEC.md` §6.3's three-thread
topology (graph builder thread + per-pass encoder workers + render
driver thread).

### 6.1 Catalog tables — single-thread on driver, read-mostly elsewhere

`virtual_table_`, `physical_table_`, `argbuf_table_`, `ring_table_`
are mutated **only on the driver thread**:

- Slot insertion (`declare_*`, `create_persistent_*`) happens
  during graph build (driver thread, single-threaded per
  `SPEC.md` §6.3 graph builder thread). The compiler is the only
  inserter at frame N; the driver thread serialises with itself.
- Slot release (`retire_transient_frame()`,
  `release_persistent(...)`) happens during phase-7 retire
  (driver thread).
- Slot lookup (`lookup(handle)`) happens on per-pass encoder
  workers during recording. Lookup is read-only against
  `physical_table_`; the table's underlying `eastl::vector` does
  not reallocate during a frame (capacity is pre-sized at init,
  worst-case for the View; an over-cap insert returns
  `ResourceResidencyExceeded` from the *driver* thread, never from
  a worker).

Acquire-release ordering across these threads is not required for
correctness because the synchronisation point is the
`ExecutionPlan::record_into` invocation: the plan is published to
the per-pass workers via release/acquire on a single atomic
`plan_ready_` flag (the §3 metal-backend cited concurrency model);
once a worker reads the plan, every preceding insert into
`physical_table_` is visible. The slot-table internal layout
(`eastl::vector<Slot>`) is therefore plain memory; no per-slot
atomics are required for the lookup path.

### 6.2 Ring vend — lock-free atomic bump

`acquire_ring_slice` is the one hot-path entry that genuinely runs
under contention (three encoder workers may race). The model is
the lock-free atomic bump from §3.10. Memory ordering:

- `head.fetch_add(bytes, memory_order_acq_rel)` provides both
  publication of the new head and visibility of prior writes by
  other workers to the slab itself (each worker writes a disjoint
  range into the slab; the fetch-add ensures the worker that
  consumed range `[old_head, old_head + bytes)` sees no later
  worker reading from the same range until the fetch-add resolves).
- Per-frame head reset on the driver thread happens under
  `memory_order_release` after the GPU has signalled completion of
  frame N-2 (the oldest in-flight); workers reading on frame N
  see the reset via the release/acquire on the next frame's
  `plan_ready_` flag.

CAS is unnecessary because the ring rolls forward only; no slot is
ever freed by a worker. Frame-retire on the driver atomic-stores
the head back to the band start; workers recording frame N+1's
passes see the new head via the next plan publication.

### 6.3 Persistent allocator — driver-thread cold-path only

Persistent allocations are cold-path; the driver thread is the
only thread that calls them. No locks are required. Hot-reload
register runs on the loader's exclusive-ownership thread, not the
driver, but the loader serialises with the driver per
`hot-reload-protocol.md` §"Step 4 — Resume" (no driver activity
during register), so the persistent allocator remains
single-threaded.

### 6.4 Sampler cache — read-only post-init

The 16-entry sampler cache is fully populated at init from the
closed sampler set; per-frame use is read-only. No locks.

### 6.5 Determinism

Two callers issuing identical `declare_*` sequences against
identical `MetalDevice` capability sets receive identical
`(index, generation)` handles and identical `MTL::Heap` placement
offsets. This is required for trace-replay (`SPEC.md` §8.6) and
asserted by `tests/render/resources/determinism_replay.cpp`. The
property follows from:

- Slot allocation is deterministic (LIFO free list; no time-based
  source).
- Generation counters advance only on release; the release order
  is determined by alias-plan output, which is deterministic
  (#760 §3.5).
- Persistent allocator best-fit is deterministic (free-list order
  is itself deterministic; ties broken by lower bin index, then
  lower address).

A non-deterministic input (e.g. ring-slice acquisitions in
worker-race order) is rejected at the determinism gate; the gate
canonicalises by serialising worker output into the trace at frame
retire (driver thread, single point).

## 7. Persistence + ABI

### 7.1 The resources aggregate persists nothing

`VirtualResource`, `PhysicalAllocation`, `ArgumentBufferRecord`,
`RingSlice`, `SamplerDesc`, the slot tables, the transient pool's
heap composition, the persistent allocator's free-list state — none
have a Fory schema; none have an on-disk artefact. They are pure
runtime state. `SPEC.md` §7.3 names every one of these in the "what
is NOT persisted" table; this aggregate's contribution is the
mechanical enforcement (`data/schemas/render/` carries no
resources-aggregate file).

The only persistent record neighbouring this aggregate is
`PSOCacheRecord` (`SPEC.md` §7.1.2), which is owned by `PSOCache`
(#766) — its archive bytes ride under the §9.5 PSO 64 MiB row, not
the resources rows. Resources reads no Fory file at runtime.

### 7.2 ABI surface

Every public type is locked in `SPEC.md` §5. The detailed-design
ABI commitments:

- `ResourceDesc`, `ResourceFormat`, `ResourceUsage`,
  `ResourceLifetime` — POD; field additions bump the ABI hash per
  `reviews/decisions/plugin-abi.md`.
- `Handle<Tag>` — fixed 64-bit layout (`SPEC.md` §5: 24:40
  generation:index); changing the split bumps the ABI.
- `GraphBuilder::declare_transient` / `declare_persistent` /
  `declare_imported` — signatures locked. Adding a fourth role is
  an ABI bump and a `SPEC.md` §4.1.4 invariant amendment.
- The `detail::` helpers (§4.3) are plugin-internal; not in the
  manifest hash.

The aggregate contributes no manifest declaration of its own
(persistent component types are zero; `SPEC.md` §7.3 is exhaustive).

### 7.3 No third-party access to `MTL::*` pointers

Imported-resource borrows (§3.8) carry `MTL::*` pointers in their
internal records; these never escape the dylib. Importers vending
a handle inject the `MTL::*` pointer through the registry-mediated
seam (their plugin's public API), and the catalog stores it
plugin-privately. Public callers see `PhysicalAllocHandle` (a
`u64`); the metal-cpp pointer is invisible.

## 8. Hot-reload

This section specialises `reviews/decisions/hot-reload-protocol.md`'s
drain → swap → migrate → resume sequence to the resources
aggregate. It is the §3.14 forward reference made concrete.

### 8.1 What survives the swap

Per `SPEC.md` §8.2's survival inventory, every persistent
resource-aggregate record survives:

| Record | Survives | Why |
|--------|----------|-----|
| `MTL::Heap`s (transient pool, persistent allocator slabs) | Yes | The heaps are owned by the catalog struct, which is plugin-private memory; the loader's exclusive phase-8 ownership prevents mutation during swap, and the `MTL::Device` reference (`SPEC.md` §8.2 row "MetalDevice") survives, so the heaps' parent device is intact. |
| `PhysicalAllocation` slot table | Yes | Slot-table bytes survive; slot generations carry over; persistent slot occupants (HZB pyramid texture, shadow atlas, history-color, RT TLAS, ring slabs) keep their `MTL::*` pointers. |
| `VirtualResource` slot table | Yes for persistent / imported entries; no for transient | Phase 8 entry guarantees no transient resource is alive (`SPEC.md` §4.1.4 invariant 1, §4.2 cross-aggregate invariant 2). The transient pool is in its drained state. |
| `ArgumentBufferRecord` slot table | Yes structurally; bodies invalidated | The records survive but their `MTL::ArgumentEncoder` references the old plugin's PSO descriptor layouts, which the new plugin must re-acquire. Resume re-builds the per-frame and per-material caches via the binder's first-frame path. |
| `RingSlice` slot table | Yes | Ring slabs survive; per-frame head pointers reset to band-start during drain. |
| `SamplerDesc` cache | Yes | Closed set; rewarms identically post-swap. The cache hits on first lookup. |
| Generation counters | Yes | Carried verbatim. Handles vended by the leaving plugin remain valid in the arriving plugin so long as their slot kind, format, and extent match the new plugin's manifest declaration. |
| Transient pool placement free list | Reset to empty | Drained at phase 7 exit before phase 8 enters; nothing to carry. |

### 8.2 `migrate(...)` body — none required

The aggregate has **no Fory schema** (§7.1). The protocol's
migrate step (`hot-reload-protocol.md` §"Step 3 — Migrate") runs no
resource-aggregate function. This is a deliberate corollary of
"resources persist nothing": persistence schema migration cannot
exist without a schema.

### 8.3 Resume — `glibre_plugin_register`

The arriving plugin's register body, scoped to the resources
aggregate (cited from `SPEC.md` §8.3, refined here):

1. **Re-attach to the surviving catalog.** `glibre_plugin_register`
   calls `registry.get<ResourceCatalog>()` and binds its function
   pointers; no slot-table bytes are touched.
2. **Validate persistent-resource schema match.** Walk the new
   plugin's manifest declaration of persistent resource kinds
   (HZB extent, shadow-atlas tier, ring slab sizes, RT TLAS cap)
   against the surviving catalog's persistent slot occupants.
   Mismatch → `core::Error::HotReloadRefused` with cause
   `core::Error::SchemaMigrationFailed` (`SPEC.md` §8.4 row).
   Match → proceed.
3. **Rewarm the argument-buffer binder.** The per-frame and
   per-material caches are dropped (their `MTL::ArgumentEncoder`
   layouts are tied to PSO descriptor sets that the arriving
   plugin's PSO cache may have re-resolved to different bytecode);
   the first frame after resume rebuilds them. The cost is a
   bounded one-frame compile — absorbed by the §9.3 0.10 ms phase
   7 reserve. No state corruption is possible because (a) the
   in-flight frame retired before phase 8 (`SPEC.md` §8.1), (b)
   the arriving plugin's first frame builds fresh argument buffers
   from scratch.
4. **Reset ring heads.** Each ring's `head_[k]` resets to the
   band-start for the in-flight frame index. The reset is a single
   atomic store; the next `acquire_ring_slice` on the new plugin
   sees the correct value via the ordinary
   `acquire_ring_slice → fetch_add` path.
5. **No `MTL::Heap` realloc, no GPU-bytes copy, no asset reload.**
   Resume's resource-aggregate cost is bounded by **O(distinct
   pass classes)** (per-pass argument-buffer rebuild on first
   frame) + **O(persistent slot count)** (schema check) + a
   constant-time ring reset. This fits the protocol's
   "register is bounded by drain + swap + Σ migrate + register"
   budget.

### 8.4 Refusal cases (resources-specific)

The aggregate contributes one new refusal cause to `SPEC.md`
§8.4's table:

| Refusal | Detected by | Inner-error arm | What the operator must do |
|---------|-------------|------------------|----------------------------|
| Surviving persistent resource's `(format, extent, mip_levels, array_layers, usage)` differs from the new plugin's manifest declaration | `glibre_plugin_register` step 2 | `core::Error::HotReloadRefused` cause `core::Error::SchemaMigrationFailed`. | Author the missing migrate function (post-MVP) or accept a fresh world. The reload is refused; the prior plugin remains live. |

This refusal is logged exactly once at `warn` level with
structured fields `plugin_fqn=glibre.render`,
`resource_kind=<HZB|ShadowAtlas|...>`, `surviving_desc_hash`,
`new_desc_hash`. The `DiagnosticOverlay` (#774 sibling) mirrors
the event payload.

### 8.5 GPU-fault restart — special path

`SPEC.md` §10.4's GPU-fault restart drains the resources aggregate
and rebuilds it as part of the same dylib reload (drain → swap →
migrate → resume). The detail: persistent slabs survive the
restart (the `MTL::Device` survives per `SPEC.md` §8.2 row, hence
its `MTL::Heap`s survive); the transient pool was already drained
at phase 7 exit; the diagnostic blob (`SPEC.md` §10.4 step 2)
captures the in-flight pass's resource view (the active `Bindings`
struct's resolved handles) so the post-restart capture's
"frame_at_fault" annotation includes which transient resources
were live at the moment of fault.

The 3-frame "stuck frame" window (`SPEC.md` §10.4 step 4) is
absorbed entirely outside this aggregate (the prior frame is
re-presented; resources do not see the stuck frames since phase 6
returns `GpuFault` early).

## 9. Performance

### 9.1 Cell — render row, resources slice

Resources is not a separate row in `SPEC.md` §9.5; it sits inside
**three** existing rows:

| §9.5 row | Resources contribution | Cap |
|----------|--------------------------|-----|
| GPU resource handles | Slot tables, residency-set bitset, free-list scratch | 16 MiB |
| Transient pool | Six `MTL::Heap` shells (256 MiB total) | 256 MiB |
| Persistent textures + buffers | Persistent allocator's slab (HZB, shadow atlas, history-color, font/overlay atlases, ring slabs, persistent argument buffers) | 128 MiB |

The other two §9.5 rows (PSO cache 64 MiB; RT structures 48 MiB)
are not resources-aggregate contributions, although the persistent
allocator backs the RT TLAS + scratch bytes inside its sub-slab.

### 9.2 Per-frame CPU budget — hot path

The aggregate's hot-path contribution is folded into `SPEC.md`
§9.3's phase-7 row:

| Phase 7 hot-path activity (resources contribution) | Per frame (S1, M1) | Counted under |
|---------------------------------------------------|---------------------|---------------|
| Per-pass `lookup(PhysicalAllocHandle)` × declared accesses | ≈ 11 passes × ~6 accesses × ~50 ns = 3.3 µs | "Per-pass execute() recording" 0.50 ms slice |
| `acquire_ring_slice` × per-pass and per-draw | ≈ 11 passes × 2 ring writes + ~3 000 draws × 1 instance write ≈ 3 022 fetch-adds × ~25 ns ≈ 76 µs | Same |
| Argument-buffer rebuild (per-pass + per-draw) | ≈ 11 passes × 256 B + 3 000 draws × 64 B = 195 KiB writes ≈ 60 µs (memcpy-bound at ~3 GB/s effective) | Same |
| Per-frame argument-buffer rebuild | 1 × 4 KiB ≈ 1 µs | Same |
| `RenderFrame` retire — `retire_transient_frame()` | ≈ 64 placement releases × ~200 ns = ~13 µs | "RenderFrame retire + transient pool recycle" 0.05 ms slice |
| **Resources hot-path subtotal** | **~150 µs (0.15 ms)** | Inside the existing 0.50 + 0.05 = 0.55 ms phase-7 hot-path slices, no amendment |

The 0.15 ms is well inside the headroom of the §9.3 reserve
(0.10 ms) plus the slice slack; no separate per-aggregate budget
amendment is required.

### 9.3 Per-frame CPU budget — cold path (per `View`)

Per-View graph build is cold path; the aggregate's contribution
sits inside `SPEC.md` §9.3's "graph/builder.cpp register" 0.10 ms +
"graph/compile.cpp" 0.20 ms slices:

| Per-View cold-path activity (resources contribution) | Per `View` (S1, M1) | Counted under |
|------------------------------------------------------|----------------------|---------------|
| `declare_transient` / `declare_persistent` / `declare_imported` × ~12 declarations | ~12 × 200 ns = 2.4 µs | "register per-View passes" 0.10 ms slice |
| Alias-plan footprint computation per virtual resource (heap size + alignment) | ~12 × `heapTextureSizeAndAlign` calls × 600 ns = 7 µs | "topo / colour / barrier / queue / bind" 0.20 ms slice |
| Materialise on first record (alias-plan placement → `MTL::Heap::newTexture`) | ~12 placements × 1.5 µs = 18 µs | "Per-pass execute() recording" 0.50 ms slice (deferred to first record) |
| **Resources cold-path subtotal per `View`** | **~28 µs (0.03 ms)** | Inside existing slices |

For four-`View` MVP fan-out, the cold path multiplies to ~0.12 ms
total, comfortably inside the cell.

### 9.4 GPU memory ceilings

The aggregate enforces three contractual ceilings; each maps to a
benchmark in `SPEC.md` §9.6.2:

| Ceiling | Measurement | Failure |
|---------|-------------|---------|
| 256 MiB transient pool | `bench("transient pool peak residency, S1, p100")` | `render::Error::TransientPoolExhausted` → `lower-tier` |
| 128 MiB persistent allocator | `bench("persistent allocator live bytes after init, S1")` | `render::Error::HeapOutOfMemory` → `lower-tier` (init) / `abort-engine` (out-of-budget at first frame) |
| 512 MiB total per `ContextTag::render` | `bench("render heap ceiling, S1, strict-mode")` (`SPEC.md` §9.6.2) | `render::Error::ResourceResidencyExceeded` → `lower-tier` |
| 0 transient bytes at phase 9 entry | `bench("transient-pool drained at phase 9, strict")` (`SPEC.md` §9.6.2) | Debug assert → CI fails |
| ≤ 5 % persistent fragmentation under churn | `bench("persistent fragmentation under churn")` (resources-only, §11.3) | Best-fit policy regression alarm |

The five gate lines are the resources aggregate's contractual
performance signature.

### 9.5 Cross-references

- `SPEC.md` §9.5 — heap composition (transient + persistent rows
  feed §9.4 above).
- `SPEC.md` §9.5.1 — allocator rules consumed verbatim by §3.11.
- `SPEC.md` §9.6.1 / §9.6.2 — CI gate definitions for resource
  budgets.
- `reviews/decisions/perf-budget.md` — Allocator Rules 1–5,
  ContextTag mechanism.
- Sibling design docs: `render-graph-design.md` §3.5 (alias
  planner consumes resource lifetimes), `metal-backend-design.md`
  §3.1 (heap allocator integration), `render-passes-design.md`
  §3 (passes consume the binder via `Bindings`).

## 10. Failure modes

The aggregate produces seven distinct errors, each rolling into
`SPEC.md` §10.1's closed sum. The three new variants
(`StaleResourceHandle`, `ResourceRoleMismatch`, and the
`ResourceResidencyExceeded` arm for sampler-cache overflow) are ABI
additions that require a `SPEC.md` §5 enum amendment and an ABI bump
per `reviews/decisions/error-model.md` Composition Rule 5.

| Render error variant | Trigger | Recovery (per §10.2) | Severity | Capability-fallback path | Test fixture |
|----------------------|---------|----------------------|----------|---------------------------|--------------|
| `HeapOutOfMemory` | Persistent-allocator best-fit failure: every bin of sufficient size is empty after merge attempts. Triggered cold-path (init or hot-reload register) or per-frame compile when a persistent resource is declared mid-frame. | `lower-tier` (re-plan at lower tier shrinks the working set; e.g. shadow atlas 4K → 2K). | `warn` | Lower tier's pass predicates select smaller persistent extents. | `tests/render/resources/heap_out_of_memory_lower_tier.cpp` |
| `TransientPoolExhausted` | Alias planner produces a peak-residency for any sub-pool exceeding its cap; triggered during graph compile, phase 7 entry. | `lower-tier`. | `warn` | Same as `HeapOutOfMemory`; lower tier shrinks gbuffer / scratch targets. | `tests/render/resources/transient_pool_exhausted.cpp` |
| `ResourceResidencyExceeded` | (a) Compile computes `total_live_bytes_after_compile > 512 MiB` (`SPEC.md` §10 row, this aggregate's primary shared trigger with `RenderGraph`); (b) sampler cache over-capacity (`sampler_cache_.size() == 16` and a new `SamplerDesc` is requested — treated as a SPEC amendment trigger in MVP; see §3.13). | `lower-tier` (case a). For case (b): `abort-engine` at init / `lower-tier` (sampler count reduction) at hot-reload; over-cap cannot arise at frame-time under MVP's static sampler set. | `warn` (a); `error` (b). | Re-plan at lower tier shrinks the working set under 512 MiB (case a only). | `tests/render/resources/residency_exceeded_lower_tier.cpp` (case a); `tests/render/resources/sampler_over_cap.cpp` (case b). |
| `StaleResourceHandle` | Generation mismatch at `SlotTable::lookup`: the handle's generation counter does not match the slot's current generation, indicating the slot was freed and reallocated since the handle was issued. Structurally distinct from a role mismatch or import refusal. | `abort-frame`. | `error` | n/a | `tests/render/resources/stale_handle.cpp` |
| `ResourceRoleMismatch` | Role mismatch on a release API call (e.g. `release_persistent` called on a transient or imported handle). Cold-path only; cannot arise on the render-thread hot path. No state mutation. | Cold-path no-op + debug assert; surfaced as `warn` in structured log. | `warn` | n/a | `tests/render/resources/role_mismatch.cpp` |
| `ResourceImportRefused` | Genuine import-borrow refusal: an imported handle is declared for write access on a resource whose borrow record was registered read-only (§3.8 invariant). Structurally distinct from a stale handle or role mismatch; this is a graph structural error. | `abort-engine` (graph is structurally invalid). | `error` | n/a — graph must be fixed. | `tests/render/resources/import_write_on_read_borrow.cpp` |
| `CapabilityNotSupported` (bindless arm) | `Capability::BindlessResources` absent at init while a registered pass declared it required. | `lower-tier` (init) / `disable-feature` (hot-reload register). | `warn` | Argument-buffer binder demotes to per-pass uniform binding without bindless visibility (§3.9 fallback path); per-frame and per-material binding tables become per-pass-direct. | `tests/render/resources/bindless_capability_missing.cpp`. |

All variants honour the §10.2 closed recovery ladder verbatim;
no aggregate-private recovery is invented.

### 10.1 Error construction site rule

Each variant is constructed at exactly **one** site inside the
dylib:

- `HeapOutOfMemory` — `resources/persistent.cpp::PersistentAllocator::allocate`.
- `TransientPoolExhausted` — `resources/alias_planner.cpp::AliasPlanner::compute` (the planner constructs the error; the catalog forwards it through `RenderGraph::compile`).
- `ResourceResidencyExceeded` — (a) `resources/transient_pool.cpp::TransientPool::peak_residency_check`; (b) `resources/sampler_cache.cpp::SamplerCache::get_or_create` (over-cap arm). Two construction sites for one variant is an SRP violation that must be resolved in the implementation plan: either split into separate variants (preferred; requires SPEC.md §5 ABI bump) or consolidate via a shared helper. Tracked as part of the §5 amendment for `StaleResourceHandle` / `ResourceRoleMismatch`.
- `StaleResourceHandle` — `resources/handle_table.cpp::SlotTable::lookup` (generation mismatch).
- `ResourceRoleMismatch` — `resources/imported.cpp::ImportRegistry::release` (wrong release API for handle's lifetime kind).
- `ResourceImportRefused` — `resources/imported.cpp::ImportRegistry::declare_write` (write-on-read-borrow).
- `CapabilityNotSupported` — `resources/argument_buffer.cpp::ArgumentBufferBinder::ensure_bindless`.

Single construction site per variant is the SRP test: if a future
change must construct one of these errors from a second site, the
SRP boundary is being violated and a refactor is required.

### 10.2 Cross-references

- `SPEC.md` §10.1 — closed sum (eighteen variants; this design adds
  `StaleResourceHandle` and `ResourceRoleMismatch` as ABI additions,
  plus adds `tags::sampler` to the §5 handle catalog).
- `SPEC.md` §10.2 — recovery ladder.
- `SPEC.md` §10.3 — per-variant rows. This design's contributions map
  to §10 design-name rows as follows:
  - `HeapOutOfMemory` + `TransientPoolExhausted` → `ResourceAllocFailed`
  - `ResourceResidencyExceeded` → `ResourceResidencyExceeded`
  - `StaleResourceHandle` + `ResourceRoleMismatch` + `ResourceImportRefused` → (resource borrow failure rows; no single §10 design-name — each has its own recovery action per §10)
  - `CapabilityNotSupported` (bindless arm) → `RtCapabilityMissing` (§10 design-name)
- `reviews/decisions/error-model.md` — Composition Rules item 5
  (closed sum extension is an ABI bump; `StaleResourceHandle` and
  `ResourceRoleMismatch` are new variants requiring an ABI bump when
  `SPEC.md` §5 is updated).

## 11. Test plan

The aggregate's tests split into unit (handle / table / allocator
arithmetic), integration (real `MTLDevice` end-to-end), and
performance (CI gate). Unit tests run in CI on every PR; integration
tests run in the macOS-26 / M1 fixture matrix nightly; perf tests
run in the perf-gate nightly.

### 11.1 Unit tests — handle / slot table

Catch2 file: `tests/render/resources/handle_table.cpp`.

| Test name | Asserts |
|-----------|---------|
| `slot table — alloc / release / realloc cycles preserve generation soundness` | After `K = 10 000` cycles, every emitted handle satisfies `generation == slot.generation` at lookup; freed handles fail. |
| `slot table — generation wrap reaches retire state at gen-cap` | With `cap = 4`, `generation_bits = 4`, the 17th realloc cycle marks the slot retired and pops the next free index. |
| `slot table — phantom-tag prevents cross-assignment at compile time` | `static_assert` on `!std::is_assignable_v<VirtualResourceHandle&, PhysicalAllocHandle>` and `!std::is_assignable_v<SamplerHandle&, ArgumentBufferHandle>` (distinct tags verify no sub-tag aliasing). |
| `slot table — capacity overflow returns ResourceResidencyExceeded` | Exhaust `cap`; next `alloc` returns the typed error; no slot inserted. |
| `slot table — release of stale handle is a no-op` | A handle whose generation mismatches the slot's current generation does not free the slot; idempotent. |

### 11.2 Unit tests — transient pool / alias plan integration

Catch2 file: `tests/render/resources/transient_pool.cpp`.

| Test name | Asserts |
|-----------|---------|
| `transient pool — six sub-pools sized exactly per §3.5` | After `init(QualityTier::Desktop)`, each `MTL::Heap` reports `usedSize == 0` and `currentAllocatedSize` matches the §3.5 table. |
| `transient pool — placement materialise / release round-trip` | `declare_transient` → first `lookup` materialises a `MTL::Texture`; phase-7 retire releases it; second declare with same desc returns a fresh `(index, generation)`. |
| `transient pool — drains to empty at phase-7 exit` | After `retire_transient_frame()`, `live_placement_count == 0` for every sub-pool. |
| `transient pool — over-cap returns TransientPoolExhausted` | Synthesise an alias plan whose color-attachment peak exceeds 64 MiB; `peak_residency_check` returns the typed error. |
| `transient pool — alias planner respects sub-pool routing` | Color targets land in color sub-pool, depth in depth, etc. |

### 11.3 Unit tests — persistent allocator

Catch2 file: `tests/render/resources/persistent_allocator.cpp`.

| Test name | Asserts |
|-----------|---------|
| `persistent allocator — best-fit picks the smallest fitting bin` | Allocate `5 MiB`; with bins `[4M..8M, 8M..16M]` populated, the 8M-bin block is split, not the 16M-bin block. |
| `persistent allocator — neighbour merge` | `release` merges adjacent free blocks; total free bytes equal sum of releases. |
| `persistent allocator — fragmentation under churn ≤ 5%` | After 1 000 alloc/release cycles drawn from a synthetic working-set distribution, `1 - largest_free_block_size / total_free_bytes <= 0.05`. |
| `persistent allocator — exhaustion returns HeapOutOfMemory` | Saturate the slab; next alloc returns the typed error. |
| `persistent allocator — alignment honours heapTextureSizeAndAlign` | Mocked alignment of 64 KiB; allocations land on 64 KiB boundaries; slack accounted in slot record. |

### 11.4 Unit tests — ring buffer manager

Catch2 file: `tests/render/resources/ring_buffer.cpp`.

| Test name | Asserts |
|-----------|---------|
| `ring buffer — fetch_add returns disjoint slices` | Three threads each call `acquire_ring_slice(Constant, 256)` 1 000 times; the 3 000 `(offset, size)` records are pairwise disjoint and within the band. |
| `ring buffer — band rollover via head reset on frame retire` | After `retire_frame_in_flight(N)`, `head_[k] == band_start[k][N+3 mod 3]`. |
| `ring buffer — over-quota returns HeapOutOfMemory` | A slice request larger than `band_end - head` returns the typed error without bumping `head`. |
| `ring buffer — alignment per kind` | Constant slices align to 256 B; instance slices to 64 B; indirect slices to 16 B. |

### 11.5 Unit tests — argument-buffer binder

Catch2 file: `tests/render/resources/argument_buffer.cpp`.

| Test name | Asserts |
|-----------|---------|
| `argbuf binder — four frequency tables built per pass` | After a pass record, four `MTL::ArgumentEncoder`-built tables exist with expected slot counts (per-frame, per-pass, per-material, per-draw). |
| `argbuf binder — per-material cache hit` | Two draws sharing `MaterialHandle` rebuild only the per-draw table; per-material is reused. |
| `argbuf binder — bindless capability fallback` | With `Capability::BindlessResources` cleared, the binder routes to per-pass-direct binding; argument tables are not built. |
| `argbuf binder — table eviction on hot-reload register` | Post-resume, the per-frame and per-material caches are dropped; the next frame rebuilds. |

### 11.6 Integration tests — real `MTLDevice` end-to-end

Catch2 file: `tests/render/resources/integration_real_device.cpp`
(macOS-26 / M1 fixture matrix; tagged `[gpu]` and skipped on hosts
without a Metal device).

| Test name | Asserts |
|-----------|---------|
| `resources e2e — single-frame transient lifecycle` | Build a synthetic graph with two transient color targets and one transient buffer; record one frame; assert (a) `MTL::Heap::usedSize` matches the alias-plan footprint mid-frame; (b) drains to zero at phase-7 exit. |
| `resources e2e — persistent HZB pyramid lives across 8 frames` | Allocate an HZB persistent texture; record 8 frames each writing its level-0 mip; assert no realloc; same `MTL::Texture*` across all frames; persistent allocator's free bytes unchanged. |
| `resources e2e — imported drawable borrow flows through present` | `next_drawable` → `declare_imported(.., write=true)` → `present` records → `presentDrawable` invoked exactly once; borrow released without `MTL::Texture` ownership transfer. |
| `resources e2e — total residency under 512 MiB on S1 fixture` | After 60 frames of S1 (one character + 200 props + 8 lights @ 1080p), `MTL::Device.currentAllocatedSize` for `ContextTag::render` ≤ 512 MiB. |
| `resources e2e — strict-mode allocator returns OutOfBudget` | Force `GLIBRE_ALLOC_STRICT=1`, allocate beyond cap; the `core::Error::OutOfBudget` arrives mapped to `render::Error::ResourceResidencyExceeded`. |
| `resources e2e — determinism replay on repeat invocation` | Two identical frames produce byte-identical alias plans, slot indices, generation counters, and `MTL::Heap` placement offsets. Inputs to `tests/render/hot_reload/trace_replay.cpp`. |

### 11.7 Hot-reload tests — survival inventory

Catch2 file: `tests/render/resources/hot_reload_survival.cpp`
(extends `tests/render/hot_reload/`, `SPEC.md` §8.6 fixture).

| Test name | Asserts |
|-----------|---------|
| `hot-reload — persistent slot table survives swap with generations preserved` | Pre-swap: `K = 64` persistent allocations; post-resume: handles still resolve; no slot generation has advanced. |
| `hot-reload — transient pool drained pre-swap` | Reload triggered at phase 8; assert `live_placement_count == 0` immediately before swap. |
| `hot-reload — schema mismatch refuses with HotReloadRefused / SchemaMigrationFailed` | New plugin's manifest declares HZB extent 4096 vs surviving 8192; resume returns the typed error; prior plugin remains live. |
| `hot-reload — argument-buffer binder caches drop and rebuild on first frame` | Post-resume frame N+1's per-pass tables are freshly built; per-material cache is empty; perf overlay shows the bounded one-frame compile. |
| `hot-reload — ring heads reset to band-start` | Post-resume, `head_[k]` equals the in-flight frame's band start; first `acquire_ring_slice` returns offset zero. |

### 11.8 Performance benchmarks

Catch2 file: `tests/render/resources/perf.cpp`. Each benchmark
asserts the §9.4 ceiling. Runs in the perf-gate nightly under
`GLIBRE_ALLOC_STRICT=1`.

| Benchmark | Asserts |
|-----------|---------|
| `BENCHMARK("ring slice acquire, single-thread, S1, p99")` | ≤ 50 ns per acquire. |
| `BENCHMARK("argument-buffer binder per-pass build, S1, p99")` | ≤ 8 µs per pass. |
| `BENCHMARK("transient pool peak residency, S1, p100")` | ≤ 256 MiB. |
| `BENCHMARK("persistent allocator live bytes after init, S1")` | ≤ 152 MiB. |
| `BENCHMARK("persistent fragmentation under churn")` | ≤ 5 %. |

These five lines are the resources aggregate's contractual perf
gate; failures are merge-blocking per `perf-budget.md` §"CI Gate
Spec". They are *additive* to the SPEC §9.6.2 heap-ceiling and
transient-drain benchmarks (not duplicative): §9.6.2 measures
end-to-end residency; this aggregate's per-mechanism benchmarks
catch regressions earlier in the stack.

### 11.9 User-story acceptance coverage

Resources directly serves three `SPEC.md` §11 stories:

- **#382** — render: transient resource alias planner with ≥ 40 %
  recovery. Resources contributes the transient pool and the
  alias-planner integration tests in §11.2.
- **#402** — render: ResourceResidencyExceeded triggers
  lower-tier recovery. Resources contributes the ceiling
  computation and the integration test in §11.6.
- **#400** — render: cost-aware budget culling under load
  (PassPriority). Resources contributes the residency
  pre-computation that feeds the cost-aware culler.

Three further stories (#388 HZB cull, #389 cluster cull, #391
BLAS refit) consume the persistent allocator transparently;
their tests live with their respective sibling aggregates.

## 12. Open questions

- `[OPEN]` **Bindless argument-buffer fallback path on bindless-
  absent hosts.** §3.9 / §10 specify the fallback, but the per-pass
  binder's "per-pass-direct" path needs concrete codegen
  (resolution gate: spike under #774 sibling that authors the
  argument-buffer binder body — pre-MVP gate). Owner: render
  team.
- `[OPEN]` **Persistent allocator bin choice 8 vs. 16.** §3.6
  fixes 8 power-of-two bins (4 KiB through 512 KiB +); MVP
  measurement may show that 16 finer bins reduce fragmentation
  below the 5 % ceiling more robustly. Resolution: re-measure
  under `bench("persistent fragmentation under churn")` after
  six MVP user stories land (#388, #389, #391, #390, #386, #401).
  No SPEC change required; this is an internal §3.6 amendment.
- `[OPEN]` **Argument-buffer per-material cache eviction policy.**
  §3.9 caches per-material tables across frames; MVP material
  count is small (~24), so no eviction is needed. Post-MVP
  expansion (visibility-buffer deferred, RT reflections) may push
  the working set beyond cache cap. Resolution: post-MVP perf
  amendment under the same `material` plugin gate cited in
  `SPEC.md` §3.3.
- `[OPEN]` **Cross-process resource sharing (compositor handoff).**
  Refused for MVP per §2 table. Resolution: post-MVP `platform`
  amendment if and when window composition graduates beyond the
  single-process model.
- `[OPEN]` **Determinism gate for ring slice ordering.** §6.5
  asserts determinism; the gate canonicalises by serialising at
  frame retire. Need to validate that the §11.6 `determinism
  replay` test catches a non-deterministic failure mode (e.g.
  three encoder workers acquiring slices in different orders
  across runs). Resolution: add a stress variant under §11.6
  with explicit thread-affinity scrambling.
- `[OPEN]` **Sampler cache cap of 16 vs. 32.** §3.13 fixes 16; MVP
  shader set fits, but a post-MVP `material` plugin authoring
  surface may need more. Resolution: bump to 32 if a CI dynamic
  count of distinct samplers exceeds 12 over a one-week nightly
  window. Internal §3.13 amendment; no SPEC change.
