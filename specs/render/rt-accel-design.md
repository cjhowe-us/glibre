# render — Detailed Design: rt-accel aggregate

> Detailed design for the `RTAccelStructures` aggregate declared in
> `specs/render/SPEC.md` §4.1.8. Refines §4.1.8 (composition + four
> public-boundary invariants), §4.2 invariant 4 (BLAS-refit precedes
> TLAS-build), §5 (`RTAccelStructures` surface — `register_blas`,
> `ensure_tlas`, `submit_blas_refit`), §6.1 (`render/src/rt/` module
> layout: `blas_registry`, `tlas`, `refit_scheduler`), §6.2.2 step 1.1 /
> 1.2 (BLAS-refit pass + TLAS-build pass in phase 7), §6.4 (hybrid-RT
> path), §7.1 (no Fory schemas — accel structs are GPU-resident,
> non-persistent), §8.2 row "RTAccelStructures" (BLAS imports survive,
> TLAS contents transient), §9.4.1 (BLAS refit accounted inside
> `shadow-rt` slice), §9.5 row "RT acceleration structures" (48 MiB),
> and §10 (`Error::BlasUnavailable`, `Error::TlasBuildFailed`) in
> place. Cites `reviews/decisions/error-model.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/fory-codegen.md`, and
> `reviews/decisions/frame-phases.md`. Sibling-style references:
> `specs/render/render-graph-design.md`,
> `specs/shader/descriptor-layout-design.md`. Introduces no new public
> surface beyond `specs/render/SPEC.md` §5; deviation from the cited
> records requires an amendment spike, not an in-place edit. Resolves
> `[SPIKE] design-render-rt-accel-detailed` (#768).

## 1. Purpose

The rt-accel aggregate is render's owner of GPU-resident ray-tracing
acceleration structures inside phase 7. Per `specs/render/SPEC.md`
§4.1.8 it owns:

1. **The BLAS registry** — a render-side handle table that imports
   BLAS handles vended by `geometry`'s cook job (§3.3 routes BLAS
   construction to `geometry`; render imports the resulting
   `MTL::AccelerationStructure*` as a read-only handle). Each entry
   tracks `(BLASHandle, source mesh GpuId, content version, dynamic
   bit, last-refit frame)`.
2. **The per-`View` TLAS lifecycle** — exactly one TLAS per `View`,
   re-emitted (rebuild or refit) every frame inside phase 7. The
   TLAS is a persistent `Resource` (the buffer + accel-struct object
   live across frames; the contents do not — §4.1.8 invariant 4).
3. **The refit scheduler** — reads the immutable `RenderFrame`
   visible-set, intersects with the registry's dynamic bit, and
   selects the BLAS subset to refit this frame. The scheduler's
   output is consumed by `passes/blas_refit.cpp` and
   `passes/tlas_build.cpp` declared in `specs/render/SPEC.md` §6.2.2
   step 1.1 / 1.2.
4. **The Metal acceleration-structure command surface** — Metal 4
   `MTLAccelerationStructureCommandEncoder` (modern API) with the
   accel-struct objects allocated against the
   `RTAccelStructures` 48 MiB row of the heap composition
   (`SPEC.md` §9.5). MPS-based acceleration structures are
   explicitly **rejected** (see §3.7 below) — the Metal 4 native
   path is the only MVP backend.
5. **The scratch-buffer pool** — render-owned transient scratch
   sized from `MTLAccelerationStructureSizes` queries; recycled per
   frame against the alias planner. Scratch is *not* the same
   allocation as the BLAS update slot or the TLAS instance buffer.
6. **The cross-queue fence seam** — render's TLAS-build pass
   declares an explicit read-after-write on the BLAS-refit pass
   outputs; the render-graph compile pipeline emits the
   `MTLSharedEvent` signal+wait pair (`render-graph-design.md`
   §3.4 step 4.3). rt-accel does **not** emit fences itself; it
   declares the data dependency in the graph.

This aggregate **refuses to own**:

- **BLAS construction.** Static-mesh BLAS bytecode is built by
  `geometry`'s cook job per `SPEC.md` §3.3 (R-2.5.1 routed to
  `geometry`). For dynamic meshes (skinned / deformable) the BLAS
  *bytes* are also created at first import by `geometry`; render
  only **refits** them in-place per the Metal 4 update-in-place
  contract (§4.1.8 invariant 3: "render writes only to the BLAS
  update slot"). A BLAS that arrives from `geometry` is a
  read-only import to render; the aggregate never authors BLAS
  geometry primitives.
- **Ray-tracing pass bodies.** The trace dispatches
  (`passes/shadow_rt.cpp`, `passes/ao_rt.cpp`, the inline ray
  query inside `passes/lighting.cpp`) own the actual ray
  generation, denoise hooks, and accumulation logic; they consume
  a `TLASHandle` from `ensure_tlas` and never touch BLAS storage.
  Pass bodies are scoped to ticket #764. The graph layer's
  `add_rt_pass` helper takes the typed `TLASHandle`
  (`render-graph-design.md` §3.7) — that helper is the only seam
  pass bodies use.
- **HZB / cull / extract.** Visible-set membership is computed in
  phase 6 by `cull/meshlet_cull.cpp` and recorded in
  `RenderFrame`; rt-accel reads the visible-set, never recomputes
  it. The HZB and the cluster-cull state are scoped to ticket
  #770 (`SPEC.md` §4.1.9 / §4.1.10).
- **PSO / pipeline-state authoring.** The compute kernels that
  drive a refit (Metal 4 selects the kernel from the descriptor;
  there is no application-supplied refit shader in MVP) are
  driver-owned. Any future custom `MTLComputePipelineState` for
  RT-related auxiliary work would live in `PSOCache` (#766), not
  here.
- **Capability gating.** `Capability::HardwareRayTrace` is
  probed once by `platform` and stored in the `CapabilityMask`
  (§7.1.3). `add_rt_pass` is hard-rejected by the graph layer
  when the capability is absent (`render-graph-design.md` §10
  row `CapabilityNotSupported`). rt-accel's surface (`ensure_tlas`,
  `submit_blas_refit`) presupposes the capability is present;
  callers that bypass the graph hard-reject see
  `render::Error::CapabilityNotSupported` as a precondition
  failure (§10 below).
- **Fory schemas / disk persistence.** Acceleration structures
  are GPU-resident, runtime-only, and rebuilt on load
  (`SPEC.md` §7.1: "render does not own shader bytecode
  persistence"; §7.3 listed cache exclusions). `geometry`
  serialises BLAS *source* (meshlet vertex / index streams) per
  R-2.4.5 and reconstructs the accel-struct on import; render's
  on-disk surface for rt-accel is **empty**.

## 2. Requirements coverage

This section verifies that every harmonius MVP-scope requirement /
design clause about ray-tracing acceleration structures is either
covered by the design below or explicitly refused with rationale.
Inputs (research only, re-derived per PHILOSOPHY §"How harmonius is
used"):

- `harmonius/docs/requirements/rendering/advanced-rendering.md`
  (R-2.5.1 — BLAS build + post-build compaction, per-frame TLAS
  rebuild/refit; R-2.5.7 — opacity micromaps + SER; the rest of
  R-2.5.* covers RT *consumers* — reflections, DDGI, surfels,
  path tracing — which are pass bodies, scoped to #764 / post-MVP).
- `harmonius/docs/requirements/rendering/meshlets.md` R-2.4.6
  (BLAS built from the same vertex/index buffers used by mesh
  shaders).
- `harmonius/docs/design/rendering/render-pipeline.md` §"RF-2 Add
  ray tracing management API" (`create_blas`, `create_tlas`,
  `build_acceleration_structure`, `trace_rays`).
- `harmonius/docs/design/rendering/meshlets.md` §"BLAS parity"
  (BLAS = rasterized geometry; one cooked artefact, two consumers).

| Harmonius clause                                                                                  | Glibre disposition                                                                                                                                                                                                                                                                                                       |
|---------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-2.5.1a** — Build BLAS from meshlet geometry with post-build compaction.                         | **Refused at this aggregate; routed to `geometry`.** `SPEC.md` §3.3 places BLAS construction + compaction at cook time inside `geometry`; rt-accel imports the cooked artefact as a read-only handle (§4.1.8 invariant 3). Compaction is `geometry`'s problem — render's perf-budget §9.5 reserves only 48 MiB GPU residency.|
| **R-2.5.1b** — Rebuild or refit the TLAS each frame for dynamic scenes.                              | **Covered.** §3.4 below: `tlas/policy.cpp` decides per-frame between full rebuild and refit from the structural diff of consecutive `RenderFrame`s; both paths complete inside phase 7 before the first RT trace pass (`SPEC.md` §6.4 step 2; story #391).                                                              |
| **R-2.5.1c** — Per-frame TLAS update ensures dynamic objects are correctly intersected.             | **Covered.** §3.6 below: every BLAS whose `dynamic` bit is set and whose source mesh is in the visible-set is refit before the TLAS is built; the graph's read-after-write edge (§4.2 invariant 4) guarantees ordering. Story #391 + integration test `rt_accel_integration/blas_refit_before_tlas_build`.                |
| **R-2.5.1d** — Verify compaction reduces BLAS size by ≥30 % (verification clause).                  | **Refused at this aggregate.** Compaction is `geometry`'s cook output; the ≥30 % gate is asserted in `geometry`'s SPEC §11. Render verifies only that imported BLAS handles fit under the 48 MiB residency row (§9 below).                                                                                                |
| **R-2.4.6** — BLAS built from the same vertex/index buffers used by mesh shaders (rasterization parity). | **Covered, via import contract.** rt-accel's `register_blas(BLASHandle, version)` accepts a handle whose backing accel-struct was built by `geometry` from the canonical meshlet vertex/index streams. The `(GpuId, version)` pair (§3.3) is the parity check: the BLAS version must equal the meshlet version, or the import is refused. |
| Harmonius design — `GpuDevice::create_blas / create_tlas` API on `GpuDevice`.                       | **Refused / re-located.** glibre splits cook (geometry) from runtime (render): geometry's cook owns `create_blas` (post-MVP API not in render's §5 surface); render owns `register_blas` (import) + `ensure_tlas` (per-`View`) + `submit_blas_refit` (per-frame). The harmonius single-surface `GpuDevice` is rejected per `SPEC.md` §3.2 collapse #1 (no `IDevice` abstraction). |
| Harmonius design — `CommandBuffer::build_acceleration_structure(tlas, instances)`.                  | **Covered, with a re-shape.** rt-accel does not expose a public `build_acceleration_structure` command method; instead, the graph's TLAS-build pass body (`passes/tlas_build.cpp`) is the only call site, and it records via `MTLAccelerationStructureCommandEncoder` against the rt-accel-owned scratch + instance buffer (§3.5). The instance list is derived inside `tlas/policy.cpp` from `RenderFrame`, not provided by callers.                |
| Harmonius design — `CommandBuffer::trace_rays`.                                                     | **Refused at this aggregate; routed to pass bodies.** Trace dispatch is `passes/shadow_rt.cpp` / `passes/ao_rt.cpp` / inline ray query in `passes/lighting.cpp` (#764). rt-accel hands them a `TLASHandle` and steps out.                                                                                                |
| Harmonius design — Per-frame TLAS instance buffer carries `BlasInstance { transform, mask, hit_index, blas }`. | **Covered.** §3.3 below: glibre's `TLASInstance` carries `(transform_3x4, instance_id, instance_mask, instance_contribution_to_hit_group_index, BLASHandle)` — a one-to-one mapping onto Metal 4's `MTLAccelerationStructureUserIDInstanceDescriptor` (§3.5). The `instance_mask` is the visibility-mask bit set used by ray queries to filter passes (§4.4 below). |
| **R-2.5.7** — Opacity micromaps, shader execution reordering (SER).                                 | **Refused for MVP; deferred.** Capability-gated (`Capability::HardwareRayTrace` covers basic ray trace; OMM + SER would need `Capability::OpacityMicromap` + `Capability::ShaderExecutionReordering`, which are post-MVP additions to the §5 capability flags). `SPEC.md` §11 stories cover only basic RT shadow / AO / ray query; OMM / SER land alongside the post-MVP RT-reflections pass.                                                                                       |
| Harmonius design — `BarrierType::AccelerationStructureBuild` first-class queue type.                | **Refused, collapsed.** glibre uses `Queue::Compute` for both TLAS build and BLAS refit (`SPEC.md` §6.2.2 step 1.1 / 1.2); the queue partition is three-wide (Graphics / Compute / Copy) per `SPEC.md` §3.2 collapse #4. A dedicated AS-build queue would be a fourth queue type with no Apple Silicon hardware backing — Metal 4 schedules AS-build commands on the standard compute queue.                                                                  |
| Harmonius design — Acceleration structure rebuild on every spatial-index BVH refit (over-cost).      | **Refused.** harmonius's `spatial-index.md` proposes BVH refits driving accel-struct refits one-to-one. glibre decouples: spatial index is `core`'s responsibility for visibility / culling (phase 6); accel-struct refit is render's per-frame work driven by the visible-set (§3.6). They share no fence.                                                          |
| Harmonius design — MPSAccelerationStructure path.                                                   | **Refused, hard.** MPS RT is the legacy Metal Performance Shaders API, deprecated in favour of Metal 4 native `MTLAccelerationStructure` since macOS 13. glibre's macOS 26 baseline (PHILOSOPHY) makes MPS strictly worse: extra dependency, no SER hooks, no opacity micromap path, no inline ray query parity. The Metal 4 native path is the only MVP backend; the issue brief admits MPS as one *option* but the design selects MTL4 native (§3.7).        |
| Harmonius design — RT pipeline state objects + shader binding tables.                                | **Refused for MVP.** glibre uses Metal 4 inline ray query (`SPEC.md` §6.4 step 3); no SBT, no RT PSOs, no `MTLRayTracingPipelineState`. RT-PSOs would be a future PSOCache extension when path-tracing reference (post-MVP) lands.                                                                                                                |
| Harmonius design — BLAS LRU + streaming (large-world residency).                                    | **Refused for MVP; deferred.** R-2.4.* virtualised geometry residency is `content`'s aggregate; the BLAS handles cycle in/out alongside their source meshlets. rt-accel's registry only marks what `content` evicts; the eviction policy is `content`'s. Listed in §12 below.                                                                                       |
| Harmonius design — Compaction at cook time + at runtime.                                            | **Refused, deferred.** Cook-time compaction is `geometry`'s job (R-2.5.1a above). Runtime compaction (Metal 4 supports `copyAndCompactAccelerationStructure`) is post-MVP — measured BLAS overhead is well under 48 MiB cap on S1; no compaction-driven amendment needed yet.                                                                                       |
| Harmonius design — Visibility mask per BLAS / per ray pass.                                          | **Covered.** §4.4 below: 8-bit `instance_mask` per `TLASInstance` carries the per-ray-pass filter (shadow / AO / reflection / GI). The mask is set when the instance is built; ray queries pass the matching `intersection_mask` argument to the inline ray query op. Story #390 / #392 exercise the shadow + AO masks.                                                                |
| Harmonius design — Per-instance hit-group index for material binding.                                | **Refused, collapsed.** With inline ray query (no SBT) the hit-group index degenerates: the ray query returns instance/primitive IDs and the consumer indexes a flat material table itself. Storing `instance_contribution_to_hit_group_index` is a Metal 4 instance descriptor field that we set to the BLAS's slot index in the TLAS — used only for instance discovery from the hit, not for RT-PSO dispatch. |

Net result: every MVP-scope rt-accel requirement is either covered
below or explicitly refused with rationale. R-2.5.1's BLAS-build half
is collapsed to `geometry` (cook), the TLAS-build/refit half stays in
render. The harmonius "build-and-trace in one CommandBuffer surface"
is split along the bounded-context seam: imports flow in via a typed
handle, refit + TLAS lifecycle live here, trace dispatches live in
pass bodies.

## 3. Detailed model

### 3.1 The aggregate cluster

```
RTAccelStructures (singleton inside the render plugin)
├── blas_registry_  : BLASRegistry          (handle table + version + dynamic bit)
├── tlas_           : eastl::hash_map<ViewHandle, PerViewTLAS>
├── refit_scheduler_: RefitScheduler        (reads RenderFrame, emits refit job list)
├── scratch_pool_   : ScratchBufferPool     (Metal-heap-backed; recycled per frame)
├── instance_buf_   : RingBuffer<TLASInstance>  (3-frame-in-flight ring; CPU-write, GPU-read)
├── sizes_cache_    : SizesCache            (BlasHandle → MTLAccelerationStructureSizes)
└── caps_           : CapabilitySet         (snapshot at register; HardwareRayTrace required)

PerViewTLAS (one per View)
├── tlas_           : MTL::AccelerationStructure*  (persistent buffer; transient contents)
├── last_build_     : BuildKind { Rebuild | Refit }     (for §3.4 policy)
├── last_member_set_hash_ : std::uint64_t              (for membership-churn detection)
├── last_frame_     : std::uint64_t                    (for diagnostic + debug overlay)
└── instance_count_ : std::uint32_t                    (≤ TLAS_INSTANCE_CAP)

BLASRegistryEntry (one per imported BLAS)
├── handle_         : BLASHandle             (generational, 64-bit)
├── source_gpu_id_  : glibre::types::GpuId   (key into geometry's mesh table)
├── version_        : std::uint64_t          (matches geometry's content version)
├── as_object_      : MTL::AccelerationStructure*  (read-only by render)
├── update_size_    : std::uint64_t          (for refit scratch sizing)
├── dynamic_        : bool                   (skinned / deformable; refit-eligible)
└── last_refit_frame_: std::uint64_t         (for "stale BLAS" debug diagnostics)
```

`RTAccelStructures`, `BLASRegistry`, `RefitScheduler`, `PerViewTLAS`,
and `ScratchBufferPool` are sibling components inside this detailed
design; `SPEC.md` §4.1.8 names them as one aggregate
(`RTAccelStructures`) because their invariants are inseparable
(BLAS-refit-precedes-TLAS-build, scratch-recycled-per-frame, one-TLAS-
per-`View`). The split above is purely an internal-architecture
decomposition, not an ABI seam — the only public surface is the §5
methods on `RTAccelStructures`.

### 3.2 Per-`View` instantiation

`ensure_tlas(ViewHandle, const RenderFrame&)` is the per-frame entry
point. Sequence (`SPEC.md` §6.2.2 step 1.2; integrates with the
graph builder):

1. Look up `views_[ViewHandle]`. Miss → allocate a new `PerViewTLAS`
   from render's persistent heap (48 MiB row, §9.5). The TLAS
   accel-struct object is sized for `TLAS_INSTANCE_CAP = 4096` MVP
   instances (§4.5 below); over-cap views see
   `render::Error::TlasBuildFailed` (instance-cap arm, §10).
2. Compute the visible-set instance list for this view by walking
   `RenderFrame::views()[i].instances()` (the SoA proxy emitted in
   phase 6). Each visible instance is a tuple
   `(GpuId, GlobalTransform, material_layer_mask)`; the registry
   resolves `GpuId → BLASHandle`; missing handles trigger
   `Error::BlasUnavailable` (§10).
3. Hash the instance list's `(BLASHandle, instance_mask)` pairs into
   `member_set_hash`. Compare to `last_member_set_hash_` —
   identical → `BuildKind::Refit` candidate; differ →
   `BuildKind::Rebuild` (see §3.4).
4. Write the per-instance `TLASInstance` records into the next slot
   of the `instance_buf_` ring (CPU-write, no allocation; ring is
   sized 3× the cap × `sizeof(TLASInstance) ≈ 64` = ~768 KiB total,
   fits inside the 48 MiB row).
5. Return the stable `TLASHandle` for this `(View, FrameCounter)`
   pair. The graph's TLAS-build pass body is the one that issues
   the actual Metal 4 build/refit command, *not* `ensure_tlas`
   itself — the call is a CPU-side prepare, not a GPU command (§5
   below; `SPEC.md` §6.4 step 2).

Multi-view fan-out: `≤4` views in MVP (`SPEC.md` §9.3); the
`tlas_` map is sized for that. Each view owns its TLAS; reflection-
probe and shadow views (post-MVP cascade fanout) get their own
entries (`SPEC.md` §4.1.8 invariant — one TLAS per `View`).

### 3.3 BLAS registry lifecycle

`register_blas(BLASHandle, std::uint64_t version)` is the import
seam. `geometry` calls it during asset publish (`SPEC.md` §3.3
"render consumes immutable mesh / BLAS handles") through the engine
registry's typed seam:

1. The handle's `MTL::AccelerationStructure*` payload is resolved
   from the engine-wide `glibre::types::GpuTable` (the middleman
   type owns the device-pointer translation; rt-accel never holds
   the raw Metal pointer outside its own scope).
2. Render queries `MTLDevice::accelerationStructureSizes(descriptor)`
   for the BLAS descriptor that produced the handle (cached by
   `geometry` and forwarded as part of the import payload, so
   render does not re-author the descriptor). The result —
   `accelerationStructureSize`, `buildScratchSize`, `refitScratchSize`
   — is stored in `sizes_cache_`. **Refit scratch ≤ build scratch**
   is a Metal 4 invariant we rely on for §3.5 sizing.
3. The entry is appended to the registry; the handle becomes the
   stable identity used by the `add_rt_pass` family.
4. `dynamic_` is set from `geometry`'s descriptor: dynamic =
   skinned / deformable / morph-target meshes; static = everything
   else. The split is published by `geometry` once at cook;
   render does not heuristically re-classify.

Re-registration with a higher `version` (geometry re-cooked the
mesh, e.g. asset hot-reload from `content` watcher) replaces the
entry atomically: the `as_object_` slot rebinds; the
`source_gpu_id_` is unchanged; the version monotonically advances.
A re-register with the *same* version is a no-op (idempotent per
plugin-abi.md). A re-register with a *lower* version is rejected
(`Error::ResourceImportRefused`).

Eviction: `geometry` / `content` may evict a BLAS when its source
mesh streams out (post-MVP). The registry exposes
`unregister_blas(BLASHandle)` (internal, not in §5 — the public
`register_blas` rolls eviction into a re-register with `version=0`
indicating absence). In MVP no eviction path is exercised; a
pinned BLAS lives until shutdown.

### 3.4 TLAS build-vs-refit policy

The choice between `BuildKind::Rebuild` (full
`MTL::AccelerationStructureCommandEncoder::build`) and
`BuildKind::Refit` (`refit`) is a **compile-time decision per
frame** based on the structural diff of consecutive `RenderFrame`s
(`SPEC.md` §4.1.8 invariant 2; `SPEC.md` §6.4 step 2).

`tlas/policy.cpp` decides as follows (executed inside
`ensure_tlas`):

1. **Membership churn** — if `member_set_hash` from §3.2 step 3
   differs from `last_member_set_hash_`, force `Rebuild`. Adding
   or removing instances cannot be expressed as a refit (Metal 4
   refit requires identical instance count + identical
   `BLASHandle` per slot; only transforms / masks may change).
2. **Slot-stability** — if `member_set_hash` matches but the
   ordering changed (instance index churn), force `Rebuild`. The
   ring's `instance_buf_` writer guarantees stable slots when
   `RenderFrame`'s extract preserves ECS-iteration order; a
   re-sort triggers a rebuild.
3. **Refit budget** — if both above match, count the BLAS subset
   that needs refit (dynamic + visible). If that count exceeds
   `REFIT_MEMBERSHIP_THRESHOLD = 384` BLAS per frame on S1 (M1
   8-core baseline, fits inside the 0.3 ms refit slice §9 below),
   force `Rebuild` — at high churn the refit's per-instance cost
   exceeds the rebuild's amortised cost. (Apple Metal 4
   benchmarks place the crossover near 30 % membership change;
   this 384-cap is a conservative MVP gate, refined under
   §12 OPEN.)
4. **Otherwise** — `Refit`. The TLAS-build pass body emits a
   `refitAccelerationStructure(tlas, descriptor, scratch)` call.

The decision is recorded in `last_build_` and consumed by the
graph compiler at the `add_rt_pass`-equivalent
`add_compute_pass(passes/tlas_build.cpp)` declaration: the pass
body branches on `BuildKind` to emit the matching encoder call.
The graph layer sees one pass declaration regardless; the
structural-hash key (`render-graph-design.md` §3.8) does *not*
include `BuildKind` because both shapes share identical access
sets — the hash is invariant to the rebuild/refit choice.

### 3.5 Scratch buffer pool

Metal 4 acceleration-structure builds and refits require a scratch
buffer at least as large as the maximum
`MTLAccelerationStructureSizes::buildScratchSize` (rebuild path) or
`refitScratchSize` (refit path) over the active set
(BLAS subset + TLAS).

Render owns one `ScratchBufferPool` backed by render's **transient
pool** alias slot (§9.5 transient row; §9.3 below). Scratch is a
per-frame-transient allocation — it is **not** placed in the 48 MiB
persistent heap row. Sizing:

```
scratch_pool_size = max(
    sum(blas_refit_scratch[i] for i in dynamic_visible_subset),
    tlas_build_scratch
) + 4 KiB alignment slack.
```

For MVP S1 (1 character ≈ 6 dynamic clusters, 200 props static, 8
lights): dynamic-visible BLAS subset is ~6 entries; per-BLAS refit
scratch is ≤512 KiB on M1; total ≤ 4 MiB. TLAS build scratch for
4096 instances is ≤ 4 MiB. The pool ceiling is set at **8 MiB**,
reserved as a transient alias slot inside the 256 MiB transient pool
(§9.5 transient row). The 48 MiB persistent row is therefore not
charged for scratch; it covers only the persistent structures (~43 MiB
for TLAS accel-struct, instance ring, imported BLAS residency, and
fragmentation slack).

Recycling: scratch is drained at phase 9 along with the rest of the
transient pool; the alias planner colours the RT-scratch slot with
other single-frame scratch. Allocations on frame N+1 see fresh slots.

### 3.6 Refit scheduler

`refit_scheduler.cpp` runs inside the `ensure_tlas` flow, before the
graph builder registers `passes/blas_refit.cpp`. The scheduler
emits an array of `BLASRefitJob { BLASHandle, scratch_offset,
new_vertex_buffer_view }`:

1. Walk `RenderFrame::views()[i].instances()` for the calling
   view; record the *unique* `BLASHandle` set (§3.3 registry
   lookup).
2. Filter to entries with `dynamic_ == true` AND `last_refit_frame_
   < current_frame_counter` (idempotent — a BLAS already refit by
   another view this frame is skipped; the scheduler is per-frame,
   not per-view).
3. Walk the deduplicated subset; for each entry, consult `geometry`
   via the `RenderFrame`'s skinning-output buffer view (the SoA
   field `RenderProxy::skin_output_view`) for the new vertex data.
   The view is a `(MTLBuffer*, offset, stride)` triple; refit reads
   the new vertices in-place.
4. Allocate a scratch slice from the pool (§3.5); record the
   offset.
5. Append the `BLASRefitJob` to the per-frame list. The
   `submit_blas_refit` public entry (§5) accepts one job at a time
   for fine-grained instrumentation; internally `passes/blas_refit.cpp`
   loops the list inside one encoder.

The scheduler is **deterministic** — same input visible-set yields
the same job order, byte-equal scratch offsets, byte-equal
`new_vertex_buffer_view` triples. Determinism is asserted by
`tests/render/rt_accel/refit_scheduler_deterministic.cpp` per
PHILOSOPHY §7.

### 3.7 Backend choice — Metal 4 native, not MPS

The issue brief admits two options: `MPSAccelerationStructure`
(legacy Metal Performance Shaders) or `MTLAccelerationStructure`
(Metal 4 native). The design selects **Metal 4 native** for MVP
and explicitly refuses MPS. Rationale:

- **Capability parity.** MPS predates Metal 4's inline ray query
  (`MTLRayQuery`); using MPS would force every consumer pass to
  emit a separate ray-trace dispatch via `MPSRayIntersector`,
  doubling the surface area of the trace path. Inline ray query
  inside compute / mesh shaders (`SPEC.md` §6.4 step 3) is the
  MVP-required path; only Metal 4 native accel structs feed it.
- **Feature ceiling.** Opacity micromaps, shader execution
  reordering, world-space motion blur intersection — all
  Metal 4-only features. MPS does not expose them and never will
  (it is in maintenance mode since macOS 13).
- **Single dependency.** MPS would add a separate Apple framework
  to the runtime link list (`Metal.framework` already required
  via `metal-cpp`). PHILOSOPHY §"Tech Stack (locked)" minimises
  framework count; one framework is one framework.
- **Hot-reload simplicity.** `MTL::AccelerationStructure*` is a
  Metal-owned object whose lifetime is governed by the Metal
  ARC-equivalent reference count; it survives a render-plugin
  swap as long as the device handle survives (`SPEC.md` §8.2 row
  "RTAccelStructures"). MPS objects carry framework-internal
  state that complicates the swap.
- **Determinism.** Metal 4's accel-struct build is deterministic
  for a fixed descriptor + device; MPS's internal SAH heuristics
  are documented as implementation-defined.

The metal-cpp seam `MTL::AccelerationStructure`,
`MTL::AccelerationStructureCommandEncoder`, and
`MTL::AccelerationStructureDescriptor` are the only headers
included by `rt/`. **No Obj-C++** anywhere in `rt/` (PHILOSOPHY
§"Don'ts"); the metal-cpp wrapper handles the bridging.

### 3.8 Aggregate composition

```
namespace glibre::render::rt {

class RTAccelStructures {
public:
    // §5 surface — see specs/render/SPEC.md §5 lines 1310–1326.
    Result<void>     register_blas(BLASHandle, std::uint64_t version) noexcept;
    Result<TLASHandle> ensure_tlas(ViewHandle, const RenderFrame&) noexcept;
    Result<void>     submit_blas_refit(MetalCommandBuffer&, BLASHandle) noexcept;

    // Internal seams used by passes/blas_refit.cpp + passes/tlas_build.cpp.
    [[nodiscard]] eastl::span<const BLASRefitJob>
        pending_refit_jobs(ViewHandle) const noexcept;
    [[nodiscard]] BuildKind
        last_build_kind(ViewHandle) const noexcept;
    [[nodiscard]] eastl::span<const TLASInstance>
        instance_buffer_view(ViewHandle) const noexcept;
    [[nodiscard]] MTL::Buffer*
        scratch_slice(std::uint64_t job_id) const noexcept;
    [[nodiscard]] MTL::AccelerationStructure*
        tlas_object(ViewHandle) const noexcept;
    [[nodiscard]] MTL::AccelerationStructure*
        blas_object(BLASHandle) const noexcept;

private:
    BLASRegistry        blas_registry_;
    eastl::hash_map<ViewHandle, PerViewTLAS> tlas_;
    RefitScheduler      refit_scheduler_;
    ScratchBufferPool   scratch_pool_;
    RingBuffer<TLASInstance> instance_buf_;
    SizesCache          sizes_cache_;
    CapabilitySet       caps_;
};

}  // namespace glibre::render::rt
```

The internal seam methods are not in `SPEC.md` §5 (they accept
`MTL::*` types and live behind the public-header opaque). They are
called only from `passes/blas_refit.cpp` and
`passes/tlas_build.cpp` inside the same plugin; cross-plugin
callers see only the three §5 methods.

## 4. Public surface

The §5 surface is the contract; this section refines the semantics
of each method without amending the header.

### 4.1 `register_blas(BLASHandle, std::uint64_t version)`

Called by `geometry`'s asset-publish path (typically inside
`content`'s loader). Preconditions:

- The `BLASHandle` is a generational handle vended by `geometry`'s
  cook (§3.3). The handle's payload (resolved through the engine's
  `glibre::types::GpuTable`) is a non-null
  `MTL::AccelerationStructure*` sized to the descriptor `geometry`
  authored.
- `version` is monotonically non-decreasing over re-registers of
  the same `(handle.index)` (the index half is reused; the
  generation half ticks on cooks).

Postconditions:

- The registry contains an entry; subsequent `ensure_tlas` calls
  may resolve the handle.
- `Capability::HardwareRayTrace` is required at register time;
  absence returns
  `Result<void>{std::unexpect, render::Error::CapabilityNotSupported}`
  per `SPEC.md` §10 row `CapabilityNotSupported`.

Failure modes:

| Error                              | Trigger                                                     |
|------------------------------------|-------------------------------------------------------------|
| `CapabilityNotSupported`           | Host lacks `HardwareRayTrace`.                              |
| `ResourceImportRefused`            | `version` lower than current; or `BLASHandle` resolves to null payload. |
| `HeapOutOfMemory`                  | Registry over its 16 MiB GPU-resource-handles slice (§9.5). |

### 4.2 `ensure_tlas(ViewHandle, const RenderFrame&)`

Called once per `View` per frame, inside phase 7 by the graph
builder (immediately after `RenderGraph::begin`, before
`add_compute_pass(passes/tlas_build.cpp)`). Preconditions:

- The `RenderFrame` is the immutable phase-6 output (`SPEC.md`
  §4.1.1 invariant 1). Calling on a non-frozen frame is a
  programming error caught by debug assert.
- The `ViewHandle` is registered with the renderer (`ensure_tlas`
  on an unknown view returns `Error::ResourceImportRefused`).

Postconditions:

- A `TLASHandle` is returned that is valid for the remainder of
  this frame's phase 7.
- `pending_refit_jobs(view)` returns the BLAS refit subset for
  this frame.
- `last_build_kind(view)` is set to either `Rebuild` or `Refit`.
- The instance-buffer ring slot for this `(view, frame)` is
  populated.

Failure modes:

| Error                              | Trigger                                                     |
|------------------------------------|-------------------------------------------------------------|
| `BlasUnavailable`                  | Visible-set references a `GpuId` whose BLAS is not registered (mesh streamed out, eviction race, version mismatch). |
| `TlasBuildFailed` (instance-cap arm) | Visible-instance count > `TLAS_INSTANCE_CAP = 4096`. Maps to `SPEC.md` §10 row `TlasBuildFailed`. |
| `TlasBuildFailed` (scratch arm)    | Required refit + build scratch > 8 MiB. Maps to `TlasBuildFailed`. |
| `ResourceImportRefused`            | Unknown `ViewHandle`.                                       |

`ensure_tlas` does **not** issue any GPU command. It is a
CPU-only prepare; the actual encoder calls live in
`passes/tlas_build.cpp` and `passes/blas_refit.cpp`.

### 4.3 `submit_blas_refit(MetalCommandBuffer&, BLASHandle)`

Called by `passes/blas_refit.cpp` inside its `execute()` lambda
(`SPEC.md` §6.2.2 step 1.1). Preconditions:

- The command buffer's queue role is `Queue::Compute`. A
  `Queue::Graphics` buffer is a programming error; debug build
  asserts; release returns `Error::PassUnsupportedConfig`
  (matches `render-graph-design.md` §10 row `queue_purity_runtime_check`).
- The `BLASHandle` was returned by an earlier
  `pending_refit_jobs(view)` call this frame.

Postconditions:

- A Metal 4 `MTLAccelerationStructureCommandEncoder::refit(...)`
  call is recorded into the command buffer, writing to the BLAS's
  update slot (§4.1.8 invariant 3) and consuming a scratch slice.
- `BLASRegistryEntry::last_refit_frame_` is updated.

Failure modes:

| Error                              | Trigger                                                     |
|------------------------------------|-------------------------------------------------------------|
| `PassUnsupportedConfig`            | Wrong queue role.                                           |
| `BlasUnavailable`                  | Handle no longer registered (geometry evicted between schedule and submit). |

A Metal 4 encoder fault during refit surfaces as
`render::Error::TlasBuildFailed` (driver arm) to the next-frame
TLAS-build consumer, per §10. `submit_blas_refit` itself does not
return a refit-specific error arm — there is no `RefitFailed`
enumerator in `render::Error` (`SPEC.md` §5 has only `TlasBuildFailed`
and `BlasUnavailable` for accel-struct operations).

### 4.4 Visibility mask layout

The 8-bit `instance_mask` field of `TLASInstance` is partitioned
across the MVP RT consumer set:

| Bit | Pass consumer (file)                                                        | Story |
|-----|------------------------------------------------------------------------------|-------|
| 0   | `passes/shadow_rt.cpp` — RT shadow trace.                                    | #390  |
| 1   | `passes/ao_rt.cpp` — RT AO trace.                                            | #390  |
| 2   | `passes/lighting.cpp` — inline ray query for shadow primary fallback.        | #390  |
| 3   | (reserved) reflections — post-MVP RT-reflections pass.                       | —     |
| 4   | (reserved) GI — post-MVP DDGI / surfel.                                      | —     |
| 5–7 | (reserved) — future RT consumers.                                            | —     |

The mask is set when `TLASInstance` is built from the `RenderProxy`
material flags; an instance whose material participates in
shadows + AO sets bits 0 + 1 + 2; a transparent / particle
instance may opt out of shadows by leaving bit 0 clear. The mask
layout is render-internal (no Fory schema, no public ABI) — adding
a bit is a render-plugin internal change.

### 4.5 TLAS instance cap + sizing

`TLAS_INSTANCE_CAP = 4096` is the MVP per-`View` ceiling. Sizing
rationale:

- S1 baseline = 200 props + 1 character (~10 instances after
  skinning split) = ~210 instances. 4096 leaves 19× headroom for
  larger MVP scenes.
- The TLAS accel-struct buffer for 4096 instances is ~1 MiB on M1
  (Metal 4's `MTL_INSTANCE_DESCRIPTOR_SIZE` is 64 bytes; 4096 × 64
  = 256 KiB raw + acceleration-structure overhead ≈ 1 MiB).
- Going above the cap returns `render::Error::TlasBuildFailed`
  (instance-cap arm, §10). Recovery is `lower-tier` (drop visible
  instances at the cull aggregate's budget pass).

Post-MVP raise to 16 384 is a perf-budget amendment, not an SRP
re-think.

## 5. Hot/cold path split

### 5.1 Cold path — at asset / plugin load

Triggered by `geometry`'s asset-publish (initial cook,
`content`'s on-disk → in-memory transition, hot-reload of an
asset). Frequency: O(asset count) at startup (~1k cycles each on
M1); rare per frame.

Work:
- `register_blas` per imported BLAS — one entry insert + one
  `MTLAccelerationStructureSizes` query (driver round-trip,
  ~10 µs each).
- Sizes cache populated; `dynamic_` bit set from the descriptor
  payload.
- No GPU work (the build itself happened inside `geometry`'s
  cook); no scratch allocation.

Cost ceiling: the entire registry import for the MVP S1 mesh set
(201 BLAS) completes inside `content`'s phase-1 / 2 budget,
< 10 ms wall-clock, off the frame critical path. No render-side
allocation budget impact.

### 5.2 Hot path — per frame, inside phase 7

Triggered by `ensure_tlas` and `submit_blas_refit`. Frequency:
once per `View` per frame for `ensure_tlas`; once per dynamic-
visible BLAS per frame for `submit_blas_refit`.

Work — `ensure_tlas` (CPU-only):
- Visible-set walk, registry lookup per instance: O(visible
  instances) × ≤200 ns lookup ≈ 0.04 ms for S1 (~210 instances).
- `member_set_hash` compute: O(visible instances) hash;
  ~0.01 ms.
- Build-vs-refit decision: O(1) compare.
- Refit job list: O(dynamic visible BLAS) ≈ ~0.005 ms.
- Instance-buffer ring write: O(visible instances) × 64 B = ~13 KiB
  copy, ~0.002 ms.

Total `ensure_tlas` CPU cost on S1: **≤ 0.06 ms** per view.
Folded inside the 0.10 ms `graph/builder.cpp` register cell of
`SPEC.md` §9.3.

Work — `submit_blas_refit` (one Metal command per call):
- Argument-buffer bind: ~1 µs (resolved offsets pre-computed).
- Encoder `refit(...)` call: ~2 µs CPU; the GPU work happens
  asynchronously.
- Cost folded into `passes/blas_refit.cpp`'s per-pass record
  budget (≤3 µs from `render-graph-design.md` §9.2).

Work — TLAS build/refit (issued by `passes/tlas_build.cpp`, not
by rt-accel directly):
- One `MTLAccelerationStructureCommandEncoder::build` or `refit`
  call.
- CPU recording cost: ~5 µs.
- GPU cost: folded into `shadow-rt` slice (`SPEC.md` §9.4.1).

### 5.3 Why the split matters

The cold path is amortised across asset load; the hot path is the
per-frame recurring work. The two never share state mutation —
`register_blas` may run concurrently with `ensure_tlas` for a
*different* `(GpuId, frame)` only because the registry is
externally synchronised by the loader (asset publish completes
before the next phase-6 visible-set extracts the new BLAS). The
phase-6→phase-7 seam delivers a frozen `RenderFrame` that already
references registered BLAS or none at all.

## 6. Concurrency

### 6.1 Threading model

rt-accel inherits the render plugin's thread-role triad
(`SPEC.md` §6.3):

- **Graph builder thread (one).** Owns `ensure_tlas` calls and
  the registry-read half of `register_blas`. The `tlas_` map +
  the `instance_buf_` ring are mutated only on this thread inside
  phase 7. No locks needed.
- **Per-pass GPU encoding workers (≤3).** `passes/blas_refit.cpp`
  records on the compute-queue worker; it calls
  `submit_blas_refit` (read-only against the registry) and the
  internal `scratch_slice` / `blas_object` accessors (read-only
  views into rt-accel state). `passes/tlas_build.cpp` similarly.
- **Driver thread.** Submits the recorded buffers; never touches
  rt-accel state directly.

### 6.2 CPU-side parallelism

`ensure_tlas` is single-threaded by virtue of running on the graph
builder thread. The dominant cost (visible-set walk, registry
lookup, hash) is well inside the per-frame budget; parallelising
across views would save fractions of a millisecond and add a join
fence, net negative.

If a future scene drives `ensure_tlas` cost above ≤0.10 ms per
view, parallelising the **registry-lookup phase** (step 1 of §3.2)
across a 2-worker fork-join is the natural extension; the hash
+ ring write must remain on the builder thread for ordering.
Listed in §12 OPEN.

### 6.3 GPU-side parallelism

BLAS refits and the TLAS build all live on `Queue::Compute`. The
graph compiler emits **no** intra-queue parallelism — the
compute queue is sequential per Metal 4 contract. The
cross-queue overlap (`Queue::Compute` running BLAS refit +
TLAS build + cluster cull while `Queue::Graphics` runs gbuffer)
is what gives the §9.4 wall-clock breakdown its slack;
cross-queue fences are emitted by the graph compiler from the
declared `(reads, writes)` sets (`render-graph-design.md`
§3.4 step 4.3), not by rt-accel.

### 6.4 `register_blas` from a non-render thread

`geometry`'s asset publish runs on the loader thread (phase 1 /
2). `register_blas` is called from there, *not* from the render
plugin's thread pool. Cross-thread safety:

1. The registry's `eastl::vector<BLASRegistryEntry>` is guarded
   by an `eastl::shared_mutex` (write-locked during register;
   read-locked during the per-frame builder lookup). Writers
   are rare (asset publish); readers are frequent (builder).
   `eastl::shared_mutex` is the only synchronisation primitive
   in rt-accel.
2. The publish happens *before* phase 6 of the frame in which
   the new mesh becomes visible (phase ordering enforced by
   `core`'s frame-phases contract per
   `reviews/decisions/frame-phases.md`). The render thread
   never observes a half-written registry entry.
3. Hot-reload: `register_blas` is called inside
   `glibre_plugin_register` (phase 8) before any phase-6 work
   resumes; identical contract.

### 6.5 Determinism

Same `RenderFrame` + same registry → same `TLASHandle` payload
+ same instance buffer bytes + same refit job order. The
`member_set_hash` is computed with a fixed seed
(`PHILOSOPHY §7`). The instance-buffer ring write order is
fixed by visible-set iteration order, which is canonical per
PHILOSOPHY §7 ("Determinism by default — fixed container iteration
order"). A formal decision record for EASTL canonical-iteration-order
is flagged in §12 OPEN below.
Determinism asserted by `tests/render/rt_accel/determinism.cpp`.

## 7. Persistence + ABI

### 7.1 What is persisted

**Nothing on disk.** Acceleration structures are GPU-resident
runtime state, never serialised (`SPEC.md` §7.3 — "what is NOT
persisted" includes RT scratch, TLAS contents, BLAS instance
buffers). The `geometry` aggregate cooks the BLAS *source*
(meshlet vertex/index streams per R-2.4.5) into the asset
archive; rt-accel reconstructs the accel-struct on import via
`register_blas` at every process start.

| Artefact                          | Persistence path                       | Reasoning                                                                                                                                              |
|-----------------------------------|----------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------|
| BLAS bytes (GPU)                  | None — rebuilt by `geometry` at load  | Cook output is the meshlet source; GPU build at load is fast (≤10 ms full S1 set) and avoids cross-driver-version incompatibility.                     |
| TLAS accel-struct (GPU)           | None — built every frame              | Per-frame artefact; `SPEC.md` §4.1.8 invariant 4 ("contents transient").                                                                                |
| Instance buffers, scratch         | None — per-frame transient ring       | §3.5; recycled by alias planner.                                                                                                                       |
| BLAS registry (CPU)               | None — rebuilt at process start       | Register entries are runtime-only; the publish flow is identical at first start and at hot-reload (§8 below).                                          |

No `.fory` schema is authored under `data/schemas/render/` for
rt-accel. The aggregate's `SPEC.md` §7 contribution is the empty
set.

### 7.2 Cross-process / cross-version contract

There is no cross-process contract because there is no on-disk
artefact. The cross-version concern is **plugin ABI**:

- The §5 public surface (`register_blas`, `ensure_tlas`,
  `submit_blas_refit`) is part of the render plugin's exported
  ABI. Any change is a render-plugin ABI bump per
  `reviews/decisions/plugin-abi.md`.
- `BLASHandle` and `TLASHandle` are middleman types
  (`glibre.types.render.GpuId`-keyed); their ABI is owned by
  `glibre-types`, not by render. A type-side change forces
  the protocol's standard re-validate flow.
- `TLAS_INSTANCE_CAP`, `REFIT_MEMBERSHIP_THRESHOLD`, and the
  scratch pool ceiling are **internal constants**, not ABI.
  Changing them is a render-plugin internal change with no ABI
  impact.

### 7.3 `glibre-types` interactions

Two middleman types that rt-accel reads:

- `glibre::types::GpuId` — the stable identity that
  `register_blas` / `ensure_tlas` use to resolve a mesh's BLAS.
  Render never invents a `GpuId`; it consumes them from
  `RenderFrame::instances()`.
- `glibre::types::CapabilityMask` (§7.1.3) — the capability
  snapshot consulted at `ensure_tlas` entry. The check
  `caps_.supports(Capability::HardwareRayTrace)` is called once;
  failure short-circuits to `Error::CapabilityNotSupported`.

No new middleman type is authored by rt-accel.

## 8. Hot-reload

### 8.1 What rt-accel drops on swap

The render plugin's hot-reload follows `SPEC.md` §8 (drain → swap
→ migrate → resume). rt-accel-specific behaviour at each step:

**Drain** (phase 7 of the last pre-swap frame already returned;
the transient pool is drained per `SPEC.md` §8.1):
- The current frame's TLAS contents are gone (transient).
- The current frame's refit job list is gone (transient).
- Scratch slices are recycled.

**Swap** (the protocol's bytes-survive rule):
- **BLAS imports** survive — they are read-only handles into
  Metal-owned objects; the device pointer is stable across the
  plugin swap because the device is owned by `platform`
  (`SPEC.md` §3.3 "Window, surface, …" routed to platform).
- **Per-`View` TLAS accel-struct objects** survive — they are
  persistent buffers in render's heap, not destroyed by
  `glibre_plugin_drain`.
- **Registry table contents** survive — the table is keyed on
  `GpuId` (middleman type); the new plugin's
  `glibre_plugin_register` repopulates the table from the
  surviving asset table without re-querying Metal.

**Migrate** — none. rt-accel has no Fory schema, so the
protocol's per-row migrate step is a no-op.

**Resume**:
1. The new plugin's `glibre_plugin_register` re-runs the publish
   flow for every BLAS handle in the surviving asset table — but
   because the `MTL::AccelerationStructure*` payloads survived
   the swap, the per-handle work is just a registry insert (no
   `MTLAccelerationStructureSizes` driver round-trip; the sizes
   cache survived as part of the data structures).
2. The `tlas_` map is re-populated lazily on the first
   `ensure_tlas` call per `View`; the surviving accel-struct
   object is rebound. No GPU work happens during register.

### 8.2 What the protocol forbids rt-accel from doing

- **No mid-frame refit**. A reload arriving mid-frame is queued
  to the next phase 8 (`SPEC.md` §8.4 row "Mid-frame reload");
  rt-accel never observes a partial refit.
- **No cross-plugin Metal pointer re-acquire**. The
  `MTL::AccelerationStructure*` survives because metal-cpp holds
  the reference count; a defensive re-acquire (e.g. a fresh
  `MTLDevice::makeAccelerationStructure(descriptor)`) would
  duplicate the GPU allocation and breach the 48 MiB row.
- **No silent drop of BLAS imports**. If the new plugin's
  manifest declares a different `BLASHandle` ABI (impossible
  in MVP — the type is middleman-owned), the protocol's
  manifest-subset check refuses the swap before
  `glibre_plugin_register` runs. rt-accel itself never authors
  a refusal of this kind.

### 8.3 Refusal cases

rt-accel contributes one inner cause to render's hot-reload
refusal vocabulary (`SPEC.md` §8.4 table):

| Inner cause                       | Detected by                                                                                  | What the operator does                                                                                              |
|-----------------------------------|----------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------|
| `BlasUnavailable` during register | `register_blas` resolves a `BLASHandle` whose `MTL::AccelerationStructure*` payload is null. | Inspect `geometry`'s cook output; the BLAS may have been evicted by `content` between the cook publish and the swap. Resolution: re-cook + restart. |

Wrapped under `core::Error::PluginInitFailed` per protocol
§"Refusal Cases".

### 8.4 Test hooks

The hot-reload trace fixture (`SPEC.md` §8.6) is the umbrella
test; rt-accel's contribution is a fixture-pair under
`tests/e2e/render/hot_reload/rt_accel_v1_v1_identity.cpp`:

- `v1` registers 32 BLAS, runs 8 frames with RT shadows on,
  swaps to `v1-byte-identical`.
- The first post-reload frame's command buffer (frame K/2 + 1)
  must be byte-equal to the reference trace.
- The TLAS build/refit decision (`Rebuild` vs `Refit`) on the
  first post-reload frame must match the reference (forced
  `Rebuild` because `last_member_set_hash_` is reset by the
  swap; this is an expected one-shot rebuild and is asserted as
  such).

A second fixture (`rt_accel_v1_v2_add_dynamic_blas.cpp`)
exercises the new-BLAS path: the new plugin registers 33 BLAS
(one new dynamic mesh); the post-reload trace asserts the new
BLAS shows up in the next frame's refit job list.

## 9. Performance

This section refines `SPEC.md` §9.4.1 (BLAS refit accounted inside
`shadow-rt` slice) and §9.5 (48 MiB GPU residency row) for the
rt-accel aggregate. Every number is a ceiling, not a steady-state
expectation; drift trips the per-PR `perf-budget.yml` gate
(`perf-budget.md` §"CI Gate Spec").

### 9.1 CPU cost (per frame, S1 fixture)

| Step                                                     | Ceiling    | Cost model                                                                                                       |
|----------------------------------------------------------|------------|------------------------------------------------------------------------------------------------------------------|
| `ensure_tlas` per `View` — visible-set walk + registry lookup | 0.04 ms    | ≤210 instances × ≤200 ns lookup. Memory: per-frame arena, no heap.                                                |
| `ensure_tlas` — `member_set_hash` compute                | 0.01 ms    | xxh3 over (BLASHandle, mask) pairs.                                                                              |
| `ensure_tlas` — refit job list build                     | 0.005 ms   | Filter + scratch alloc; ≤6 dynamic-visible BLAS on S1.                                                            |
| `ensure_tlas` — instance-buffer ring write               | 0.002 ms   | ≤210 × 64 B = ~13 KiB memcpy.                                                                                    |
| `submit_blas_refit` per BLAS — encoder bind + refit call | 3 µs       | Per Metal-cpp call.                                                                                              |
| **rt-accel CPU subtotal per `View`, S1**                 | **≤ 0.06 ms** | Folded into `SPEC.md` §9.3 0.10 ms `graph/builder.cpp` row + 0.50 ms per-pass record row.                          |

### 9.2 GPU cost (per frame, S1 fixture)

`SPEC.md` §9.4.1 fixes the rt-accel GPU contribution as **~0.3 ms
folded inside the 1.5 ms `shadow-rt` slice**. Decomposition (S1):

| Step                                  | GPU ms (S1) | Notes                                                                                          |
|---------------------------------------|-------------|------------------------------------------------------------------------------------------------|
| `passes/blas_refit.cpp` — refit calls | 0.20 ms     | ~6 dynamic-visible BLAS × ~30 µs each on M1 (skinned character + a few dynamic props).         |
| `passes/tlas_build.cpp` — refit path  | 0.05 ms     | ~210 instances; refit dominant case.                                                            |
| `passes/tlas_build.cpp` — rebuild path | 0.10 ms     | Membership churn; rare on S1 (per §3.4 step 1).                                                |
| **GPU subtotal (refit-dominant)**     | **0.25 ms** | Inside the 0.30 ms reservation.                                                                |
| **GPU subtotal (rebuild-dominant)**   | **0.30 ms** | At the §9.4.1 ceiling.                                                                          |

If `shadow-rt`'s 1.5 ms ceiling is ever pressured by RT-shadow
trace-cost growth, the rt-accel half is the candidate to extract
into a dedicated `rt-accel-build` row — that is a perf-budget
amendment, not a re-design.

### 9.3 Memory ceilings

The rt-accel aggregate's persistent storage budget is the **48 MiB
"RT acceleration structures"** row of `SPEC.md` §9.5, decomposed.
Refit + build scratch is **transient** (§3.5) and is accounted in the
256 MiB transient pool separately — it does **not** appear in this
48 MiB subtotal.

| Sub-row                                | Ceiling   | Lifetime         | Allocator                                     |
|----------------------------------------|-----------|------------------|-----------------------------------------------|
| Per-`View` TLAS accel-struct           | ~1 MiB ×4 = 4 MiB | Persistent  | `glibre::PerContextAllocator(render)`, persistent slot. |
| Imported BLAS GPU residency            | ≤ 30 MiB | Persistent       | Tagged `render` per Allocator Rule 5; bytes are render-owned, source is `geometry`. |
| TLAS instance ring (3-frame-in-flight) | 768 KiB  | Persistent       | Render's CPU-write ring buffer.               |
| Sizes cache + registry table           | 256 KiB  | Persistent       | Standard CPU heap (counted in 16 MiB GPU-resource-handles row of §9.5, *not* in this 48 MiB row). |
| Reserve / fragmentation slack          | ~8 MiB   | Persistent       | Heap allocator's natural fragmentation.       |
| **Subtotal (persistent row)**          | **≤ 43 MiB** | —             | Under the 48 MiB §9.5 row; ~5 MiB headroom.   |

Transient (not in 48 MiB row):

| Sub-row                                | Ceiling   | Lifetime            | Allocator                                    |
|----------------------------------------|-----------|---------------------|----------------------------------------------|
| Refit + build scratch pool             | 8 MiB    | Per-frame transient  | Transient pool alias slot (§9.5 transient row; 256 MiB pool). |

Strict-mode enforcement (`GLIBRE_ALLOC_STRICT=1`) catches drift
above these caps and returns `core::Error::OutOfBudget`, mapped
to `render::Error::ResourceResidencyExceeded` at the call site
(`SPEC.md` §9.5.1 rule 2). Specifically, `register_blas` over
the 30 MiB BLAS-residency sub-row maps directly to
`HeapOutOfMemory` per §10 below.

### 9.4 Cited acceptance benchmarks

Stories #390, #391, #392 from `SPEC.md` §11 each carry a Catch2
fixture under `tests/render/`. The performance-relevant
benchmarks (named under `tests/render/perf/`):

| Benchmark name                                          | Measures                                                          | Ceiling       |
|---------------------------------------------------------|-------------------------------------------------------------------|---------------|
| `BENCHMARK("ensure_tlas, S1 main view, p99")`           | Wall time of one `ensure_tlas` call                               | ≤ 0.06 ms     |
| `BENCHMARK("blas refit GPU, S1, p99")`                  | GPU wall time of all `submit_blas_refit` calls in the frame       | ≤ 0.20 ms     |
| `BENCHMARK("tlas build/refit GPU, S1, p99")`            | GPU wall time of `passes/tlas_build.cpp`                           | ≤ 0.10 ms     |
| `BENCHMARK("rt-accel residency, S1")`                   | Live bytes on `ContextTag::render` attributable to rt-accel        | ≤ 48 MiB      |
| `BENCHMARK("blas refit determinism, S1")`               | Same `RenderFrame` → same refit job order                          | byte-equal    |
| `BENCHMARK("tlas build kind stability, S1")`            | Stable scene → consecutive frames are `Refit` (not `Rebuild`)      | true          |

The first three roll up under the `phase-7 render-submit driver,
S1, p99` and `shadow-rt GPU` slots of `SPEC.md` §9.6.1 / §9.6.2;
the rest are rt-accel-aggregate-owned gates.

### 9.5 Cross-references

- `SPEC.md` §9.4.1 — `shadow-rt` slice covers BLAS refit + TLAS
  build (combined ceiling 1.5 ms; rt-accel's share is ~0.3 ms).
- `SPEC.md` §9.5 — 48 MiB heap row; this aggregate's budget.
- `SPEC.md` §9.6.1 — per-pass GPU timestamp queries (the
  `shadow-rt` slot includes rt-accel's GPU work).
- `perf-budget.md` §"Pipelined Frame Timing" — render's 1.40 ms
  submit slot; this aggregate's contribution is the sum of §9.1.

## 10. Failure modes

The closed enumeration of `render::Error` arms emitted by this
aggregate (subset of `SPEC.md` §10.1 publication; §5 stub already
declares `BlasUnavailable` and `TlasBuildFailed`; the additional
arms are existing §5 enumerators repurposed for rt-accel call
sites):

| `render::Error` arm              | §10.1 row             | Trigger                                                                                                                                   | Recovery        | Severity | Test fixture                                             |
|----------------------------------|-----------------------|-------------------------------------------------------------------------------------------------------------------------------------------|-----------------|----------|----------------------------------------------------------|
| `BlasUnavailable`                | `BlasUnavailable`     | `ensure_tlas` finds a visible `GpuId` whose BLAS is not registered (eviction race, version mismatch, `register_blas` failed earlier), or `submit_blas_refit` finds the handle gone between schedule and submit. | `lower-tier` (drop the offending instance from the visible-set; `cull/budget.cpp` re-runs next frame). | `warn`   | `tests/render/rt_accel/blas_unavailable.cpp`            |
| `TlasBuildFailed` (instance cap) | `TlasBuildFailed`     | `ensure_tlas` sees visible-instance count > 4096.                                                                                          | `lower-tier`    | `warn`   | `tests/render/rt_accel/tlas_instance_cap.cpp`           |
| `TlasBuildFailed` (scratch)      | `TlasBuildFailed`     | `ensure_tlas` would need scratch > 8 MiB pool ceiling.                                                                                     | `lower-tier`    | `warn`   | `tests/render/rt_accel/tlas_scratch_exhausted.cpp`      |
| `TlasBuildFailed` (driver)       | `TlasBuildFailed`     | Metal 4 returns a build error from `MTLAccelerationStructureCommandEncoder::build` (driver fault).                                          | `abort-frame` (debug) / `abort-engine` (release CI). | `error`  | `tests/render/rt_accel/tlas_driver_fault.cpp`           |
| `CapabilityNotSupported`         | (composite §10.3)     | `register_blas` or `ensure_tlas` invoked on a host without `Capability::HardwareRayTrace`.                                                 | `disable-feature` (graph elides RT passes per `render-graph-design.md` §3.3). | `warn`   | `tests/render/rt_accel/capability_missing.cpp`          |
| `ResourceImportRefused`          | `ResourceImportRefused` | `register_blas` with a `BLASHandle` whose payload is null, or with a lower `version` than current.                                       | `abort-engine` (programming error) / `lower-tier` (eviction race). | `error`  | `tests/render/rt_accel/import_refused.cpp`              |
| `HeapOutOfMemory`                | `HeapOutOfMemory`     | BLAS GPU residency over 30 MiB sub-row (§9.3); strict-mode allocator returns `core::Error::OutOfBudget`, mapped here.                      | `lower-tier`    | `warn`   | `tests/render/rt_accel/blas_residency_exhausted.cpp`    |
| `PassUnsupportedConfig`          | (composite §10.3)     | `submit_blas_refit` invoked on a non-Compute command buffer.                                                                                | `abort-engine` (programming error). | `error`  | `tests/render/rt_accel/refit_wrong_queue.cpp`           |

Cross-cutting notes (per `SPEC.md` §10.2):

- **`BlasUnavailable` is the canonical eviction-race arm.** When
  `content` evicts a streamed mesh between phase 6 (visible-set
  extract) and phase 7 (TLAS build), the offending instance
  appears in the visible-set without a registered BLAS. Recovery
  is `lower-tier`: the instance is dropped from the TLAS build
  *for this frame* (the graph compiler tolerates a smaller
  instance count); next frame's `cull/budget.cpp` excludes the
  evicted mesh from the visible-set.
- **`TlasBuildFailed` (driver) is hard.** A driver fault during
  Metal 4 accel-struct build escalates to GPU fault per
  `SPEC.md` §10.4 (`abort-engine` + `hot-reload-restart` with
  diag capture). This is the only rt-accel arm that triggers
  the engine-wide fault path.
- **`CapabilityNotSupported` is normally pre-empted by graph-
  layer elision.** `add_rt_pass` on a non-RT host returns
  `CapabilityNotSupported` *before* it reaches rt-accel
  (`render-graph-design.md` §10). rt-accel's defensive check is
  there for direct callers (post-MVP plugin paths bypassing the
  graph helper) and for hot-reload mismatches.
- **`ResourceImportRefused` (programming-error class) vs. (eviction-
  race class)** — the call-site mapping decides. The eviction-race
  class is rare and recoverable; the programming-error class
  (e.g. null payload from `geometry`) is `abort-engine`.
- **No `RefitFailed` arm.** Metal 4's refit either succeeds or
  fails the same way build does — there is no refit-only error
  surface. A refit failure surfaces as
  `TlasBuildFailed (driver)` because the next-frame consumer
  observes a broken acceleration-structure object; the failure
  is escalated by the consumer (TLAS-build pass), not by
  `submit_blas_refit` itself.

The aggregate does **not** emit graph-layer arms
(`RenderGraphCycle`, `BarrierConflict`, `PassDeclaredUseUnused`);
those belong to render-graph (#760). It does **not** emit
device or queue arms (`DeviceLost`, `QueueSubmitFailed`,
`FenceTimeout`); those belong to `MetalDevice` (#762). It does
**not** emit pipeline arms; rt-accel does not author a PSO.

## 11. Test plan

Tests are split between Catch2 unit tests under
`tests/render/rt_accel/` and integration tests under
`tests/render/integration/` that exercise rt-accel against a
real `MetalDevice` from the platform fixture
(`platform/test/MetalDeviceFixture.hpp`, peer aggregate). Every
test names a §10 row, a §3 / §4 algorithm step, a `SPEC.md` §11
acceptance story, or a §9 benchmark.

### 11.1 Unit tests — registry

| TC ID                                                 | Trigger                                                                  | Expectation                                                                | §-link        | Story  |
|-------------------------------------------------------|--------------------------------------------------------------------------|----------------------------------------------------------------------------|---------------|--------|
| `rt_accel/register_blas_records_entry`                | `register_blas(handle, version=1)`.                                      | Entry visible to subsequent `ensure_tlas` lookups.                          | §3.3, §4.1   | #391   |
| `rt_accel/register_blas_idempotent_same_version`      | Two registers with `version=1`.                                          | Second is no-op; entry unchanged.                                           | §3.3          | #391   |
| `rt_accel/register_blas_higher_version_replaces`      | Register `v=1`, then `v=2`.                                              | Entry payload + version updated atomically.                                 | §3.3          | #391   |
| `rt_accel/register_blas_lower_version_refuses`        | Register `v=2`, then `v=1`.                                              | Returns `unexpected(ResourceImportRefused)`.                                | §3.3, §10    | #391   |
| `rt_accel/register_blas_null_payload_refuses`         | Handle resolves to null `MTL::AccelerationStructure*`.                   | Returns `unexpected(ResourceImportRefused)`.                                | §3.3, §10    | #391   |
| `rt_accel/register_blas_capability_missing_refuses`   | `Capability::HardwareRayTrace` absent.                                   | Returns `unexpected(CapabilityNotSupported)`.                               | §4.1, §10    | #392   |
| `rt_accel/register_blas_residency_exhausted`          | 30 MiB sub-row pressured; one more import.                               | Returns `unexpected(HeapOutOfMemory)`.                                      | §9.3, §10    | #402   |
| `rt_accel/register_blas_dynamic_bit_from_descriptor`  | Two registers with `dynamic=true` and `dynamic=false`.                   | Bits surface correctly via internal accessor.                               | §3.3          | #391   |

### 11.2 Unit tests — `ensure_tlas`

| TC ID                                                 | Trigger                                                                  | Expectation                                                                | §-link        | Story  |
|-------------------------------------------------------|--------------------------------------------------------------------------|----------------------------------------------------------------------------|---------------|--------|
| `rt_accel/ensure_tlas_first_frame_rebuilds`           | First-ever `ensure_tlas` for a `View`.                                   | `last_build_kind == Rebuild`; instance buffer populated.                    | §3.4 step 1   | #391   |
| `rt_accel/ensure_tlas_stable_scene_refits`            | Two consecutive frames with identical visible-set.                       | Frame 2 reports `last_build_kind == Refit`.                                | §3.4 step 4   | #391   |
| `rt_accel/ensure_tlas_membership_churn_rebuilds`      | Visible-set adds one instance frame N→N+1.                               | Frame N+1 reports `Rebuild`.                                                | §3.4 step 1   | #391   |
| `rt_accel/ensure_tlas_slot_reorder_rebuilds`          | Same set, different ECS iteration order.                                 | `Rebuild`.                                                                  | §3.4 step 2   | #391   |
| `rt_accel/ensure_tlas_high_dynamic_churn_rebuilds`    | >384 dynamic-visible BLAS in one frame.                                  | `Rebuild` (refit budget exceeded).                                          | §3.4 step 3   | #391   |
| `rt_accel/ensure_tlas_unknown_view_refuses`           | `ensure_tlas(unregistered_view)`.                                        | Returns `unexpected(ResourceImportRefused)`.                                | §4.2          | #391   |
| `rt_accel/ensure_tlas_blas_unavailable`               | Visible instance references unregistered BLAS.                           | Returns `unexpected(BlasUnavailable)`.                                      | §4.2, §10    | #391   |
| `rt_accel/ensure_tlas_instance_cap_exceeded`          | Visible-instance count > 4096.                                           | Returns `unexpected(TlasBuildFailed)` (instance-cap arm).                   | §4.5, §10    | #402   |
| `rt_accel/ensure_tlas_scratch_exhausted`              | Pathological refit set requires > 8 MiB scratch.                         | Returns `unexpected(TlasBuildFailed)` (scratch arm).                        | §3.5, §10    | #402   |
| `rt_accel/ensure_tlas_member_set_hash_deterministic`  | Same input twice.                                                        | Identical `member_set_hash` (PHILOSOPHY §7).                                | §3.2 step 3   | #391   |

### 11.3 Unit tests — refit scheduler

| TC ID                                                 | Trigger                                                                  | Expectation                                                                | §-link        | Story  |
|-------------------------------------------------------|--------------------------------------------------------------------------|----------------------------------------------------------------------------|---------------|--------|
| `rt_accel/refit_scheduler_dynamic_only`               | Mixed visible-set (dynamic + static).                                    | Refit job list contains only dynamic-bit entries.                           | §3.6 step 2   | #391   |
| `rt_accel/refit_scheduler_dedupe_across_views`        | Two views see the same dynamic BLAS.                                     | Refit appears once in the per-frame list (dedup by `last_refit_frame_`).    | §3.6 step 2   | #391   |
| `rt_accel/refit_scheduler_scratch_offsets_packed`     | 6 dynamic BLAS with varying refit-scratch sizes.                         | Offsets are non-overlapping, sum ≤ pool ceiling.                            | §3.5, §3.6   | #391   |
| `rt_accel/refit_scheduler_deterministic`              | Two runs of the same `RenderFrame`.                                      | Identical job order + identical scratch offsets.                            | §3.6, §6.5   | #391   |

### 11.4 Unit tests — `submit_blas_refit`

| TC ID                                                 | Trigger                                                                  | Expectation                                                                | §-link        | Story  |
|-------------------------------------------------------|--------------------------------------------------------------------------|----------------------------------------------------------------------------|---------------|--------|
| `rt_accel/submit_records_metal_refit_call`            | `submit_blas_refit(compute_cb, blas)` with valid pre-scheduled job.      | Mock command buffer records one `refitAccelerationStructure` call.         | §4.3          | #391   |
| `rt_accel/submit_wrong_queue_refuses`                 | `submit_blas_refit(graphics_cb, blas)`.                                  | Returns `unexpected(PassUnsupportedConfig)`.                               | §4.3, §10    | #391   |
| `rt_accel/submit_unknown_blas_refuses`                | Handle unregistered between schedule and submit.                         | Returns `unexpected(BlasUnavailable)`.                                      | §4.3, §10    | #391   |
| `rt_accel/submit_updates_last_refit_frame`            | Successful submit.                                                       | `BLASRegistryEntry::last_refit_frame_` advances.                           | §3.6, §4.3   | #391   |

### 11.5 Integration tests — real `MetalDevice` (Apple-Silicon CI)

| TC ID                                                 | Trigger                                                                  | Expectation                                                                | §-link        | Story  |
|-------------------------------------------------------|--------------------------------------------------------------------------|----------------------------------------------------------------------------|---------------|--------|
| `rt_accel_integration/blas_round_trip`                | Cook a 1k-tri mesh BLAS via `geometry`, register, query sizes.            | `accelerationStructureSize` ≥ 0; `refitScratchSize` ≤ `buildScratchSize`.   | §3.3          | #391   |
| `rt_accel_integration/tlas_build_and_trace`           | Build TLAS for one BLAS instance; trace one ray via inline ray query.     | Ray hit reported with correct instance ID.                                  | §3.4, §4.4   | #391   |
| `rt_accel_integration/blas_refit_before_tlas_build`   | Schedule one BLAS refit + one TLAS rebuild on the compute queue.          | Cross-pass fence emitted; RT shadow trace observes refit BLAS.              | §3.4, §4.2 inv 4 | #391   |
| `rt_accel_integration/blas_refit_changes_geometry`    | Refit a BLAS with new vertex data; trace before + after.                  | Hit position differs (refit took effect).                                   | §3.6, §4.3   | #391   |
| `rt_accel_integration/multi_view_two_tlas`            | Two `View`s; each `ensure_tlas` returns a distinct `TLASHandle`.          | Both TLAS objects valid; two trace passes succeed.                          | §3.2          | #391   |
| `rt_accel_integration/capability_fallback`            | Run on a synthetic capability set without `HardwareRayTrace`.             | `add_rt_pass` elides; `lighting.cpp` falls back to PCSS shadow.             | §10           | #392   |
| `BENCHMARK("ensure_tlas, S1, p99")`                   | S1 fixture, 600 frames, p99 of `ensure_tlas`.                            | ≤ 0.06 ms.                                                                   | §9.4          | (perf) |
| `BENCHMARK("blas refit GPU, S1, p99")`                | S1 fixture, GPU timestamp around all `submit_blas_refit` calls.           | ≤ 0.20 ms.                                                                   | §9.4          | #391   |
| `BENCHMARK("tlas build GPU, S1, p99")`                | S1 fixture, GPU timestamp around `passes/tlas_build.cpp`.                 | ≤ 0.10 ms.                                                                   | §9.4          | #391   |
| `BENCHMARK("rt-accel residency, S1")`                 | Live bytes on rt-accel-tagged sub-rows.                                  | ≤ 48 MiB.                                                                    | §9.4          | (perf) |

### 11.6 Coverage matrix

- Every §10 row has at least one fixture (§11.1 / §11.2 / §11.3 /
  §11.4 named).
- Every §3 / §4 algorithm step has at least one unit test
  (§3.2 → `ensure_tlas_member_set_hash_deterministic`; §3.3 →
  registry suite; §3.4 → `ensure_tlas` build-vs-refit suite;
  §3.5 → `refit_scheduler_scratch_offsets_packed` +
  `tlas_scratch_exhausted`; §3.6 → refit-scheduler suite;
  §3.7 backend choice → integration `tlas_build_and_trace`).
- Every `SPEC.md` §11 story whose acceptance criterion includes
  the rt-accel aggregate (#390, #391, #392) is named in at
  least one TC ID's "Story" column.
- Every §9.4 benchmark is named in §11.5.

The integration tests gate on the **platform-fixture-required**
label; they run on the macOS-26 / M1 CI runner only. Unit tests
run on every PR (no Metal device required; the
`MetalCommandBuffer` is mocked through the wrapper's debug seam).

## 12. Open questions

- **[OPEN]** `REFIT_MEMBERSHIP_THRESHOLD` calibration. §3.4 step 3
  picks 384 dynamic-visible BLAS as the refit-vs-rebuild crossover.
  The number is from a coarse Metal-4 benchmark on M1; the actual
  crossover depends on per-BLAS geometry size and instance-mask
  density. Resolve by measurement during the §11.5 benchmark
  bring-up; if the measured crossover is materially different, the
  amendment goes to `tlas/policy.cpp` (not to `SPEC.md` §4.1.8 —
  this is an internal threshold).
- **[OPEN]** Parallelising `ensure_tlas` across views. §6.2 keeps
  the call single-threaded; if multi-view scenes pressure the
  per-frame budget, fork-joining the registry-lookup phase across
  2 workers is the natural extension. Defer until a measured
  scene exceeds ≤0.10 ms per `ensure_tlas`.
- **[OPEN]** Runtime BLAS compaction. Metal 4 supports
  `copyAndCompactAccelerationStructure`. Cook-time compaction is
  `geometry`'s job; runtime re-compaction (after a long-running
  scene fragments BLAS storage) is a post-MVP perf-budget
  amendment. The §9.3 30 MiB sub-row has no compaction reserve.
- **[OPEN]** Dedicated `rt-accel-build` GPU budget row. §9 folds
  rt-accel's GPU work inside the 1.5 ms `shadow-rt` slice. If
  RT-shadow trace cost grows (more trace samples, more denoise
  iterations) and pressures the slice, extracting rt-accel into
  its own row is a perf-budget amendment per
  `perf-budget.md` §Consequences.
- **[OPEN]** Streaming / eviction protocol. §3.3 mentions
  `unregister_blas` as an internal API for `geometry` /
  `content` to call when a mesh streams out. The full protocol
  (eviction race detection, post-eviction visible-set repair) is
  scoped to the streaming-residency design (`content` aggregate,
  not in MVP). MVP scope is "no eviction"; the registry is
  monotonic.
- **[OPEN]** Visibility-mask layout for post-MVP RT consumers.
  §4.4 reserves bits 3–7 for reflections / DDGI / surfels / future
  consumers. The actual bit assignment lands when the post-MVP
  pass is designed; the layout in §4.4 is non-binding for those
  bits. No ABI risk — the mask is render-internal.
- **[OPEN]** OMM + SER capability flags. §2 refuses R-2.5.7 for
  MVP; the §5 `Capability` enum has no `OpacityMicromap` or
  `ShaderExecutionReordering` flags yet. Adding them is a
  capability-flag enum extension when the post-MVP RT-reflections
  / path-tracer pass lands.
- **[OPEN]** TLAS-instance ABI freeze. §3.3 fixes
  `TLASInstance` as a 64-byte record matching Metal 4's
  `MTLAccelerationStructureUserIDInstanceDescriptor`. If a
  future Metal 4 SDK changes the descriptor layout (Apple has
  done this twice between major macOS releases), rt-accel's
  internal record needs a re-pack. The change would be a
  render-plugin internal ABI bump if `TLASInstance` ever
  becomes ABI-visible (currently it is not — the field is
  internal-only).
- **[OPEN]** Determinism canonical-iteration decision record. §6.5
  grounds the refit-scheduler's deterministic job order in
  PHILOSOPHY §7 ("fixed container iteration order"). A formal
  `reviews/decisions/determinism-canonical-iteration.md` decision
  record spelling out which EASTL containers guarantee ordered
  iteration and how that invariant is enforced in tests is required
  before rt-accel's determinism tests are authoritative. Track via
  a `[SPIKE] iterate-render-determinism-canonical-iteration` issue.
