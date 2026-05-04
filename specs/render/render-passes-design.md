# Render-Passes Detailed Design

> Detailed design for the `render` context's render-passes aggregate
> (`specs/render/SPEC.md` §4.1.3 `Pass`, §4.2 cross-aggregate invariants
> 5–6, §5 `PassExecuteFn` / `GraphBuilder::add_*_pass`, §6.2.2 frame
> phase 7 pass list, §9.4 per-pass GPU breakdown, §10 failure modes).
> This design refines the SPEC's closed catalog of MVP render passes —
> the per-pass bodies that turn a compiled `ExecutionPlan`'s declarations
> into recorded Metal 4 command-encoder calls. All conclusions
> independently re-derived; harmonius `docs/design/rendering/` cited as
> research input only (§3.2 collapse #2).
>
> Refs: spike #764 — `[SPIKE] design-render-render-passes-detailed`.
> Parent #759. Sibling task-breakdown spike blocked-by this deliverable.

## 1. Purpose

The render-passes aggregate is the closed catalog of per-pass bodies
inside the render plugin. Each pass owns one declared `(reads, writes,
queue, capability predicate, execute lambda)` tuple and one Metal 4
command-encoder recording responsibility. The aggregate's single
reason to change is **what a single MVP render pass does** — adding a
denoise step inside `shadow_rt`, swapping the deferred-lighting body
to read a different cluster buffer layout, replacing the AA chain
inside `aa_upscale`, etc.

The catalog is fixed at MVP to the eleven pass classes locked in
`specs/render/SPEC.md` §6.2.2 step 1 (the GraphBuilder's per-`View`
registration sequence): **`blas_refit`, `tlas_build`, `cluster_cull`,
`gbuffer`, `hzb_build`, `shadow_rt`, `ao_rt`, `lighting`,
`transparent_forward`, `post`, `aa_upscale`, `present`**. The issue
brief's "depth-prepass / GBuffer-fill / lighting / post / present"
shorthand maps onto this catalog one-to-many: the depth-prepass
collapses into the `gbuffer` pass (SPEC §4.2 invariant 5 — gbuffer
MRT + visID + velocity + depth are written by **one** mesh-shader
dispatch as a single atomic `Pass`); the GBuffer fill is `gbuffer`;
lighting is `cluster_cull` + `shadow_rt` + `ao_rt` + `lighting` +
`transparent_forward`; post is `post` + `aa_upscale`; present is
`present`. The catalog is closed: adding a twelfth pass class is a
SPEC §6.2.2 amendment, not an in-place edit.

What this aggregate explicitly **refuses** to own:

- **Graph topology, declaration ordering, alias planning, barrier
  emission, queue assignment, structural-hash cache.** Owned by the
  `RenderGraph` / `ExecutionPlan` aggregate (SPEC §4.1.2, §4.1.5;
  spike #760 detailed design). Passes declare; the graph compiles.
- **PSO compilation, residency, eviction.** Owned by `PSOCache`
  (SPEC §4.1.7; spike #766). Passes consume `PSOHandle`s by lookup
  through the cache; no pass invokes a compiler.
- **`MetalDevice` / `MetalQueue` / `MetalCommandBuffer` lifecycles,
  heap allocator, residency-set attachment, fence emission.** Owned
  by the Metal-backend aggregate (SPEC §4.1.6; spike #762). Passes
  receive a `MetalCommandBuffer&` from the plan recorder and never
  acquire one themselves.
- **BLAS construction, BLAS storage layout, vertex / index / meshlet
  cooking, mesh-import.** Owned by `geometry` and `content` (SPEC
  §3.3). The `blas_refit` pass writes only into render-owned scratch
  + the BLAS's update slot per the Metal 4 RT API (SPEC §4.1.8
  invariant 3); BLAS-static memory is read-only.
- **TLAS storage, instance-buffer layout, BLAS-handle registry, TLAS
  rebuild-vs-refit decision.** Owned by `RTAccelStructures` (SPEC
  §4.1.8; spike #768). Passes consume `TLASHandle` and the resolved
  decision; they do not pick.
- **Frustum / occlusion / meshlet-cull selection, `RenderFrame`
  extract, sort-key composition, per-view layer mask.** Owned by the
  cull-extract aggregate inside phase 6 (SPEC §6.2.1; spike #770).
  Phase-7 passes consume `RenderFrame`'s already-finalised SoA spans
  and never re-cull.
- **Shader bytecode, Slang authoring, AIR/metallib emit, descriptor
  layout derivation.** Owned by `shader` (`specs/shader/SPEC.md`).
  Passes consume `PSOKey` halves through `PSOCache::get`.
- **Material parameter buffer schema, bindless material indexing,
  material-graph authoring.** Owned by the future `material` plugin
  (SPEC §3.3). Passes bind `MaterialHandle` indices opaquely.
- **VFX particle / cloth / fluid simulation.** Owned by the future
  `vfx` plugin (SPEC §3.3). Passes consume already-resident GPU
  buffers under the standard `VirtualResource` borrow.
- **Frame schedule, ECS storage, hot-reload protocol.** Owned by
  `core` (SPEC §3.3). Passes are entered exclusively from phase 7
  via the plan recorder.
- **Window, surface, drawable lifetime, `CAMetalDisplayLink` pacing.**
  Owned by `platform` (SPEC §3.3). The `present` pass calls
  `MetalCommandBuffer::present_drawable(...)` once and signals
  `PresentFence`; pacing is phase 9.

The aggregate's SRP boundary is sharp: if the per-pass declared access
set, the per-pass execute lambda body, the per-pass queue affinity,
the per-pass capability predicate, or the per-pass barrier-emit point
changes, this design changes. Anything else is out of scope.

## 2. Requirements Coverage

Mapping of harmonius `docs/design/rendering/render-pipeline.md`,
`docs/design/rendering/meshlets.md`, `docs/design/rendering/render-effects.md`,
`docs/design/rendering/rendering-core.md`, and the
`docs/requirements/rendering/*.md` per-pass requirements onto the
glibre MVP per-pass catalog. Every entry independently re-derived;
harmonius is research input only (PHILOSOPHY §"How harmonius is used",
SPEC §3).

| Harmonius element | Glibre disposition (MVP) | Coverage site |
|-------------------|--------------------------|---------------|
| Separate Z-prepass + opaque base + GBuffer fill in three passes (`design/rendering/render-pipeline.md` § "GBuffer Pass") | **Refused — collapsed.** | One `gbuffer` pass writes 4 MRT + visID + velocity + depth in one mesh-shader dispatch (SPEC §4.2 invariant 5). Atomic side-effect set §3.4 below. |
| `OpaquePass` and `TransparentPass` as separate aggregate roots | **Refused — collapsed.** | Two pass classes, one aggregate. Both consume the same `LightCluster` (SPEC §3.2 collapse #3). §3.5 below. |
| `ShadowPass` per-light + `CSMPass` (cascades) + `RTShadowPass` parallel implementations (`requirements/rendering/lighting.md` R-2.4.6..R-2.4.13) | **Partial — collapsed.** | One `shadow_rt` pass for hybrid-RT shadow primary rays + denoise hook (SPEC §3.2 collapse #2, §6.4 step 3). PCF / PCSS fallbacks live as `Capability::HardwareRayTrace`-gated branches inside `lighting`'s deferred body (SPEC §10.3 row `RtCapabilityMissing`); no separate `ShadowPass` aggregate. |
| Forward+ tiled vs deferred clustered as parallel paths | **Refused — collapsed.** | One `cluster_cull` (compute) feeds both `lighting` (deferred) and `transparent_forward`. SPEC §3.2 collapse #3. |
| `OcclusionCullPass` as a separate render-graph node read by phase 6 (`design/rendering/meshlets.md` § "Two-Phase Cull") | **Partial — split between phases.** | Phase-6 cull-extract reads last-frame's HZB inside `cull/meshlet_cull.cpp` (not a render-pass; phase 6 has no graph). Phase-7 owns the **build** of next frame's HZB through `hzb_build`. §3.6 below. |
| Compute-rasterised hair / strand / BVH-traverse render passes (`design/rendering/character-rendering.md` R-2.8.3, `design/rendering/render-effects.md` § "VFX") | **Refused.** | VFX is a separate plugin (SPEC §3.3). Render consumes GPU buffers / meshes through standard `Pass` nodes in MVP — no dedicated compute-rasteriser pass class. |
| Volumetric / fog / cloud passes (`requirements/rendering/environment.md`) | **Refused.** | Environment plugin (SPEC §3.3). Post-MVP slot in the catalog when the plugin lands; not in MVP. |
| Per-effect post-FX passes (bloom, DOF, motion-blur, exposure histogram, tonemap, grade, film-grain, vignette, panini, cavity, Dolby Vision) (`requirements/rendering/post-processing.md` R-2.9.1..R-2.9.14) | **Covered — collapsed.** | One `post` pass body executes the per-effect chain ordered by `RenderSettings`; per-effect kernels are dispatch sub-steps inside the pass, not separate `Pass` nodes (SPEC §3.2 collapse #6). §3.10 below. |
| TAA / FXAA / SMAA / TSR / DLSS / FSR / XeSS / vendor-upscaler permutations (`requirements/rendering/anti-aliasing-upscaling.md` R-2.6.1..R-2.6.9) | **Covered — collapsed.** | One `aa_upscale` pass class; the body is selected at graph-build time from `RenderSettings.aa_mode` + `RenderSettings.upscaler`. Selection is graph-topology, not in-shader branching (SPEC §3.2 collapse #5). §3.11 below. |
| `BLASBuildPass` per dynamic mesh + central `TLASBuildPass` (`requirements/rendering/advanced-rendering.md` R-2.5.1..R-2.5.10) | **Covered.** | Two pass classes — `blas_refit` (Compute) and `tlas_build` (Compute) — wired with explicit read-after-write per SPEC §4.2 invariant 4. §3.7 + §3.8 below. |
| Capture-to-texture / minimap / probe-render passes as a separate render-graph fork | **Covered — multi-view.** | The same eleven pass classes instantiate per `View` (SPEC §3.2 collapse #7); no extra pass class. The `present` pass is no-op for offscreen views — `View::is_offscreen() ⇒ blit-to-imported-texture`. §3.13 below. |
| Diagnostic / debug overlay as its own pass (`requirements/rendering/scene-rendering-pipeline.md`) | **Refused — debug-only.** | `DiagnosticOverlay` (SPEC §3.2 collapse #10) is build-time-gated and registers a debug-only pass slot adjacent to `present`. Out of MVP shipping link (SPEC §6.5 mirrors shader's exclusion pattern); detail in §8.4. |
| 2D / UI as a separate raster engine (`design/rendering/2d.md`) | **Covered — multi-view.** | The same catalog instantiates with a different `RenderSettings` (no shadows, no RT, simplified post). Sprite/UI authoring is `tools` + `content`. No new pass class. |
| Stylised / NPR / character outline as a separate post chain | **Refused — execution-only.** | Style execution slots into `post` as additional kernels selected by `RenderSettings`; style **authoring** is `material` + `tools` (SPEC §3.3). |

Glibre-native requirements added beyond harmonius:

- **One pass class = one file = one declared body.** §6.1 module
  layout (SPEC `passes/<name>.{hpp,cpp}`) is the SRP enforcement
  mechanism. Adding a new pass class is one file under `passes/`;
  removing one is the inverse. No multi-pass files, no
  configuration-driven pass instantiation.
- **Pass interface = `(declare(builder) → execute(encoder, bindings))`
  C++23 concept.** §4.2 below pins the concept; every pass class
  models it with no virtuals (PHILOSOPHY §6 "no runtime reflection").
- **Atomic side-effect set per pass.** SPEC §4.1.3 invariant 3 plus
  §4.2 invariant 5 (gbuffer atomicity) are enforced by the graph
  compiler + a debug-build encoder hook. §3.4 + §3.5 below.
- **Per-pass GPU timestamp at encoder begin / end.** Every pass body
  inserts the paired timestamp via `MTLCounterSampleBuffer` per SPEC
  §9.6.1. The mechanism is a one-line helper invoked in the pass's
  execute prologue / epilogue — no per-pass branching. §9 below.
- **Capability-gated absent, not branched.** A pass whose
  `Capability` predicate evaluates false at graph-build time is
  **absent** from the node list, not present-and-skipped (SPEC
  §4.1.2 invariant 3). This is what keeps shader hot paths free of
  capability bits. §3.3 below.
- **No allocations on the hot path.** Pass execute bodies never
  allocate (SPEC §4.1.3 invariant 4). Ring-buffer slices and
  transient placements come pre-resolved through the `Bindings`
  struct. §5 + §9 below.

## 3. Detailed Model

### 3.1 Catalog roster

The aggregate exposes exactly eleven pass classes inside the render
plugin. Each is a separate translation unit under
`render/src/passes/<name>.{hpp,cpp}` (SPEC §6.1 module layout). The
catalog roster, in graph-build order (SPEC §6.2.2 step 1):

| # | Pass class            | File                     | Queue     | Capability gate                     | Atomic write set                                                        |
|---|-----------------------|--------------------------|-----------|--------------------------------------|-------------------------------------------------------------------------|
| 1 | `blas_refit`          | `passes/blas_refit.cpp`  | Compute   | `HardwareRayTrace`                   | BLAS update slots for visible-LOD0 dynamic clusters.                    |
| 2 | `tlas_build`          | `passes/tlas_build.cpp`  | Compute   | `HardwareRayTrace`                   | Per-`View` TLAS storage; instance buffer.                               |
| 3 | `cluster_cull`        | `passes/cluster_cull.cpp`| Compute   | (none — mandatory)                   | `LightCluster` (compacted index buffer + atomic counters).              |
| 4 | `gbuffer`             | `passes/gbuffer.cpp`     | Graphics  | `MeshShaders` (or fallback variant)  | GBuffer MRT (4) + visibilityID + velocity + depth — **one** dispatch.   |
| 5 | `hzb_build`           | `passes/hzb_build.cpp`   | Compute   | (none — mandatory)                   | Next-frame HZB pyramid (mip-chain min/max).                             |
| 6 | `shadow_rt`           | `passes/shadow_rt.cpp`   | Compute   | `HardwareRayTrace` + `RayQuery`      | Shadow-trace + denoise outputs (per-view denoised shadow-mask).         |
| 7 | `ao_rt`               | `passes/ao_rt.cpp`       | Compute   | `HardwareRayTrace` + `RayQuery`      | RT-AO trace + denoise outputs (per-view AO buffer).                     |
| 8 | `lighting`            | `passes/lighting.cpp`    | Compute   | (none — mandatory; tier-gated body)  | Deferred lighting accumulation (HDR scene color).                       |
| 9 | `transparent_forward` | `passes/transparent_forward.cpp` | Graphics | (none — mandatory)            | HDR scene color (alpha-blended) + per-view OIT slot if enabled.         |
| 10| `post`                | `passes/post.cpp`        | Graphics  | (none — mandatory; tier-gated body)  | Post-LDR/HDR target (bloom / DOF / motion / tonemap / grade chain).     |
| 11| `aa_upscale`          | `passes/aa_upscale.cpp`  | Graphics  | (none; variant absent if MetalFx/Vendor missing) | Final-resolution color target (post-AA, post-upscale).            |
| 12| `present`             | `passes/present.cpp`     | Graphics  | (none — mandatory)                   | Swapchain drawable; signals `PresentFence`.                             |

(`hzb_build` runs **after** `gbuffer` because it consumes the gbuffer's
depth output to produce next frame's HZB; phase 6 of frame N+1 then
reads it. SPEC §4.1.9 invariant 1 — two-phase symmetry.)

`aa_upscale` is one pass class with four bodies (`Off`, `Fxaa`,
`Smaa`, `Taa`, `TemporalSuper`); the graph builder selects the body
at build time from `RenderSettings.aa_mode` (§3.11). `post` is one
pass class with a chain of compute kernels selected from
`RenderSettings`; the chain is composed at build time from a fixed
table (§3.10) and the resulting `execute` lambda invokes only
selected kernels.

### 3.2 Pass record types

Each pass class is a value type with a stable shape (no virtuals):

```cpp
struct PassRecord {
    eastl::string_view  name;            // debug-only; stable per pass class
    Queue               queue;           // Graphics / Compute / Copy (SPEC §5)
    PassPriority        priority;        // budget-cull priority (SPEC §5)
    Capability          requires_caps;   // built-time gate (SPEC §5 PassDesc)
    PassExecuteFn       execute;         // typed lambda (SPEC §5)
};
```

The eleven entries in §3.1 each hand-author one PassRecord factory
under `passes/<name>.cpp`:

```cpp
// e.g. passes/gbuffer.cpp
PassRecord make_gbuffer_pass(const RenderFrame&, ViewHandle,
                             const RenderSettings&) noexcept;
```

The factory composes the typed access set (`reads`, `writes` spans
of `GraphBuilder::ResourceAccess`) and the execute lambda; it does
**not** mutate the graph. The graph builder calls
`add_raster_pass` / `add_compute_pass` / `add_rt_pass` (SPEC §5) with
the factory's record. Two records of the same pass class for two
different `View`s are independent values; aliasing across views is
forbidden (SPEC §4.1.10 invariant 1 — same rule lifted to all per-view
state).

### 3.3 Capability gating (build-time, never run-time)

Each pass's `requires_caps` is a `Capability` mask (SPEC §5). The
graph builder evaluates the mask against `MetalDevice::capabilities()`
**at `compile()` time**:

- If every required bit is set, the pass joins the node list with its
  full body.
- If any bit is missing, the pass either (a) registers a fallback
  variant under the same pass-class name (e.g. `gbuffer` mesh-shader
  → `gbuffer` vertex+amplification fallback when `MeshShaders` is
  absent, SPEC §10.3 row `MeshShaderCapabilityMissing`), or (b) is
  absent from the node list (e.g. `shadow_rt` and `ao_rt` are absent
  when `HardwareRayTrace` is missing; the deferred lighting body
  reads PCF/PCSS fallbacks already inside its tier-gated branches per
  SPEC §6.4 step 3).

This is the SPEC §3.2 collapse #5 (one `RenderSettings` enum, no
shader-side branching) and SPEC §4.1.2 invariant 3 (capability-gated
predicate at build) realised at the per-pass layer. The fallback
variants are themselves PassRecord factories; the per-pass file
hosts both.

The capability mask also drives MVP refusal cases: a `RenderSettings`
configuration that demands `RayQuery` on a host without it is
detected at the pass-record factory and surfaces as
`render::Error::CapabilityNotSupported` from
`GraphBuilder::add_*_pass`. This re-uses SPEC §10.3 rows
`MeshShaderCapabilityMissing` / `RtCapabilityMissing` and does not
introduce a new error variant.

### 3.4 `gbuffer` — single mesh-shader dispatch (atomic write set)

The single most load-bearing invariant in the catalog. SPEC §4.2
invariant 5 makes the gbuffer pass write four MRT attachments
(albedo+metallic, normal+roughness, motion / velocity, visibilityID)
plus depth in **one** mesh-shader dispatch declared as **one** `Pass`.
This collapses harmonius's separate Z-prepass + opaque base + GBuffer
into one node and lets the alias planner promote depth + visID +
gbuffer to disjoint physical allocations safely:

- **`reads`.** `RenderFrame.proxy_soa.opaque_indirect_buffer` (SPEC
  §2 `IndirectDrawBuffer`), `RenderFrame.material_table` (opaque
  bindless table from the material plugin), the per-`View`
  `cluster_cull` output (`LightCluster`, declared as a transient).
- **`writes`.** Five `VirtualResource`s declared transient by the
  factory:
  1. `gbuffer_albedo_metallic` (`ColorAttachment`,
     `ResourceFormat::Rgba8Unorm`).
  2. `gbuffer_normal_roughness` (`ColorAttachment`,
     `ResourceFormat::Rgba16Float`).
  3. `gbuffer_motion` (`ColorAttachment`,
     `ResourceFormat::Rgba16Float`).
  4. `gbuffer_visibility_id` (`StorageTexture`, `ResourceFormat::R32Uint`).
  5. `gbuffer_depth` (`DepthAttachment`,
     `ResourceFormat::Depth32Float`, reverse-Z; SPEC §4.1.9 invariant 3).
- **`execute`.** Opens **one** `MTL::RenderCommandEncoder`, sets
  the gbuffer PSO from `PSOCache::get(PSOKey{shader_hash,
  state_hash})`, binds the per-frame / per-pass / per-material /
  per-draw argument-buffer groups (SPEC §3.2 collapse #8), and
  dispatches `drawMeshThreadgroupsWithIndirectBuffer:` once per
  material group from the `IndirectDrawBuffer` (SPEC §6.5 step 3).
  No second encoder, no encoder-end-then-restart inside the body.

The graph compiler refuses any `gbuffer` factory that declares fewer
than five writes or uses two encoders; the refusal returns
`render::Error::PassUnsupportedConfig` (SPEC §10 closed sum). A
debug-build hook on `MetalCommandBuffer` (SPEC §4.1.6 invariant 3)
asserts encoder-discipline at runtime; shipping builds rely on the
build-time refusal.

The mesh-shader fallback (vertex + amplification when `MeshShaders`
is absent) preserves the same atomic-write-set invariant: the
fallback factory still declares the same five writes and still
records on a single encoder. Only the dispatch shape changes
(`drawIndexedPrimitives` indirect from a CPU-amplification stage).
The fallback's pass-record factory lives in the same file.

### 3.5 `lighting` and `transparent_forward` — shared `LightCluster`

Both passes consume the `LightCluster` produced by `cluster_cull`
(SPEC §3.2 collapse #3). The deferred `lighting` pass is compute;
the `transparent_forward` pass is graphics. Their declared accesses
differ in the rest of the input set:

- **`lighting`.** Reads gbuffer-MRT (5), `LightCluster`, RT-shadow
  output (when present), RT-AO output (when present), shadow-atlas
  CSM (when `ShadowTier::Pcf` / `Pcss`); writes HDR scene color
  (transient `ColorAttachment`-equivalent storage texture). Compute
  queue.
- **`transparent_forward`.** Reads HDR scene color (after lighting),
  `LightCluster`, `RenderFrame.proxy_soa.translucent_indirect_buffer`,
  material table; writes HDR scene color (alpha-blended, in-place).
  Graphics queue.

Both factories declare every read/write up-front per SPEC §4.1.3
invariant 1 ("Declared = used"). The deferred body's tier branches
(PCF / PCSS / RT) are graph-shape-driven, not in-shader: the
`lighting` factory under `ShadowTier::Pcf` declares a read on
`shadow_atlas_csm_cascades`; under `ShadowTier::RayTraced` it
declares a read on `shadow_rt_output` and no atlas read; the
`shadow_rt` pass is absent from the node list under `Pcf`. PHILOSOPHY
§6 (no runtime reflection / no shader-side capability branching)
realised per-pass.

Inline ray query inside `lighting` (SPEC §6.4 step 3) is gated on
`Capability::RayQuery`; the factory adds `tlas` to its read set when
the bit is present and reads only static shadow data otherwise. The
`tlas` declaration uses `GraphBuilder::add_rt_pass` instead of
`add_compute_pass` (SPEC §5) so the compiler emits the BLAS-refit →
TLAS-build → lighting fence chain (SPEC §4.2 invariant 4).

### 3.6 `hzb_build` — phase-7 write of next frame's HZB

`hzb_build` is the only pass whose write target is a **persistent**
resource (the per-`View` HZB pyramid; SPEC §4.1.4 + §4.1.9). All
other writes in the catalog are transient. The factory declares:

- **`reads`.** `gbuffer_depth` (transient produced by `gbuffer`).
- **`writes`.** `hzb_pyramid` (persistent imported via
  `HZB::ensure(view, desc)`; lifetime ≠ aliasable per SPEC §4.1.4
  invariant 2).
- **`execute`.** Records a compute kernel that writes mip 0 from
  the gbuffer depth, then dispatches the mip-chain reduction. One
  `MTL::ComputeCommandEncoder`; one PSO from `PSOCache`; no second
  encoder. Reverse-Z convention shared with gbuffer (SPEC §4.1.9
  invariant 3) — the kernel is a single-source-of-truth header.

The persistent target is **double-buffered** at the resource layer:
phase 6 of frame N reads slot `N-1`, phase 7 writes slot `N`. The
`HZB` aggregate (`§4.1.9`) owns the swap; the pass factory only
declares a write into the slot that `HZB::write_target(view)` returns.

### 3.7 `blas_refit` — write only into update slots

The factory walks `RenderFrame.dynamic_blas_set` (extracted by phase
6) and emits one `MTL::AccelerationStructureCommandEncoder` refit
per visible-LOD0 dynamic cluster set (SPEC §6.2.2 step 1.1). Per
SPEC §4.1.8 invariant 3 the pass writes only into render-owned
scratch + the BLAS's update slot per the Metal 4 RT API; BLAS-static
memory is read-only (geometry-cooked).

- **`reads`.** `RenderFrame.dynamic_vertex_streams` (imported,
  geometry-vended), the BLAS handles being refit (imported,
  geometry-vended).
- **`writes`.** Per-BLAS update-slot scratch buffers (transient,
  one per refit; alias-eligible across non-overlapping refits).
- **`execute`.** Opens **one** acceleration-structure encoder,
  records `refitAccelerationStructure:descriptor:scratchBuffer:` per
  BLAS, ends the encoder. Compute queue. No `MTLBuffer` allocation
  — scratch comes from the bindings struct.

If `Capability::HardwareRayTrace` is absent, the pass is absent from
the node list (capability-gated; §3.3 above); `tlas_build` is also
absent; `lighting` factory selects the non-RT shadow / AO branches.

### 3.8 `tlas_build` — rebuild-or-refit decided at build time

Per SPEC §4.1.8 invariant 2 the choice between rebuild and refit is
compile-time, not runtime. The factory inspects
`RenderFrame.tlas_diff(view)` (a value computed by phase 6 from
consecutive frames' visible-set memberships) and selects one of two
factory variants:

- **`tlas_build_rebuild`.** Membership churn over threshold.
  Declares a write of the full `tlas_storage` and a read of
  `dynamic_blas_set` (already refit by `blas_refit`).
  `buildAccelerationStructure:descriptor:scratchBuffer:` records once.
- **`tlas_build_refit`.** Membership stable, only transforms
  changed. Declares a refit-shape write (same target, smaller scratch).
  `refitAccelerationStructure:` records once.

Both variants live in the same file. Both declare a read-after-write
on `blas_refit`'s outputs (SPEC §4.2 invariant 4); the compiler emits
the cross-queue fence even though both passes share `Queue::Compute`
(intra-queue ordering is plan-driven). The fence is part of the plan,
not the pass body.

### 3.9 `cluster_cull` — persistent-thread compute

The factory binds the persistent `ClusterCullState` storage owned by
the aggregate `§4.1.10` (compute kernel handle, per-view froxel
descriptor, persistent scratch buffers + atomic counters). Per SPEC
§4.1.10 invariant 3 the only mutation point is the atomic-counter
write inside the kernel; consumers read-only. The factory:

- **`reads`.** `RenderFrame.lights[view]` (visible light set),
  `RenderFrame.camera[view]` (frustum + near/far), the persistent
  scratch buffers (imported borrow).
- **`writes`.** `light_cluster_indices` (transient,
  `ResourceUsage::StorageBuffer`), `light_cluster_counts`
  (transient, `ResourceUsage::StorageBuffer`).
- **`execute`.** One `MTL::ComputeCommandEncoder`, one
  `dispatchThreadgroups:` for the persistent-thread kernel. Workgroup
  count is fixed at init from `QualityTier` (SPEC §4.1.10 invariant
  2); no hot-path branching.

### 3.10 `post` — chain composed at build time

`post` is a single pass class with a chain of GPU kernels selected
from `RenderSettings`. The factory composes the chain from a fixed
table:

| RenderSettings predicate          | Kernel selected               | Order |
|-----------------------------------|-------------------------------|-------|
| `bloom_enable`                    | `bloom_downsample` + `bloom_upsample` | 1 |
| `dof_enable`                      | `dof_circle_of_confusion` + `dof_blur`| 2 |
| `motion_blur_enable`              | `motion_blur_per_object`      | 3     |
| (always)                          | `auto_exposure_histogram`     | 4     |
| (always)                          | `tonemap_aces` (or `dolby_vision_pq` if `hdr_output`) | 5 |
| `grade_enable`                    | `grade_lut3d`                 | 6     |
| `film_grain_enable`               | `film_grain`                  | 7     |
| `vignette_enable`                 | `vignette`                    | 8     |
| `chromatic_aberration_enable`     | `chromatic_aberration`        | 9     |
| `panini_enable`                   | `panini_projection`           | 10    |

Each kernel is a `PSOHandle` lookup against `PSOCache`; the execute
lambda dispatches each in order on **one** compute or graphics
encoder (the table fixes per-kernel queue). Kernels not selected do
not dispatch; the encoder is opened once and closed once. Per SPEC
§3.2 collapse #6, this is "one post graph segment", not ten `Pass`
nodes.

The kernel table itself is locked in `passes/post.cpp` and is the
only place ordering between post effects lives. Adding a kernel is
one row in the table + one PSO entry in the shader plugin's manifest;
the graph never sees the kernels individually.

### 3.11 `aa_upscale` — variant selected at build time

Per SPEC §3.2 collapse #5, `aa_upscale` has one body per
`AntiAliasMode × UpscalerMode` combination; the factory selects one
at graph-build time from `RenderSettings`. Variant set:

| `aa_mode`        | `upscaler`         | Body (one execute lambda)                                           |
|------------------|--------------------|---------------------------------------------------------------------|
| `Off`            | `Off`              | passthrough blit (one encoder, no kernel).                          |
| `Fxaa`           | `Off`              | `fxaa` compute kernel, single dispatch.                             |
| `Smaa`           | `Off`              | `smaa_edge` + `smaa_blend` + `smaa_neighborhood` (three dispatches).|
| `Taa`            | `Off`              | `taa_history_blend` (reads persistent history color).               |
| `Taa`            | `BuiltinFallback`  | `taa_history_blend` + render-owned bilinear upscale.                |
| `Taa`            | `MetalFx`          | `taa_history_blend` + `MTLFXTemporalScaler` upscale.                |
| `TemporalSuper`  | `MetalFx` (default)| `MTLFXTemporalScaler` (single Metal-vended call).                   |
| `TemporalSuper`  | `Vendor`           | DLSS / FSR / XeSS via vendor ABI (capability-gated; out of MVP).    |

The pass factory picks one body and produces one PassRecord. No
runtime branching inside the body; the graph topology is the
selection mechanism. `Vendor` upscalers are present-and-skipped at
MVP (capability-absent ⇒ pass demoted to `BuiltinFallback`).

History color (the persistent texture TAA needs) is declared as a
persistent imported resource in the factory's `reads` set; phase 6
of frame N+1 sees the new history color produced this frame.

### 3.12 `present` — one drawable acquire, one fence signal

Final pass; SPEC §6.2.2 step 1.11. Per SPEC §3.3, `platform` owns the
swapchain + drawable lifetime; render's `present` pass calls into the
Metal-backend wrapper to acquire the drawable, blit the final color
target, and signal `PresentFence`.

- **`reads`.** `final_color` (transient produced by `aa_upscale`).
- **`writes`.** `swapchain_drawable` (imported via
  `MetalDevice::next_drawable_handle()`; declared with
  `ResourceUsage::PresentTarget`).
- **`execute`.** Opens a `MTL::BlitCommandEncoder`, blits
  `final_color` to the drawable, ends the encoder; calls
  `presentDrawable:` on the command buffer; signals `PresentFence`.
  Single encoder, single command, single fence.

Failure to acquire the drawable returns
`render::Error::SwapchainAcquireFailed` (SPEC §10.3 row); the pass
factory's `execute` lambda surfaces the error through the
`Result<void>` return per SPEC §5 `PassExecuteFn` signature. The
plan recorder treats this as `abort-frame` per SPEC §10.2.

For offscreen `View`s (capture-to-texture, reflection probe), the
`present` factory substitutes a blit-to-imported-texture body; no
drawable acquire, no fence signal. The factory selects the body from
`View::is_offscreen()`.

### 3.13 Multi-view fan-out

Per SPEC §3.2 collapse #7 the same eleven-pass catalog instantiates
once per `View`. The graph builder (SPEC §4.1.2) calls each pass
factory once per `(View, FrameCounter)` pair. Pass-class identity is
shared across views; the PassRecord values are not. Cross-view
sharing happens at the resource layer (e.g. `cluster_cull` results
are per-view; `hzb_pyramid` is per-view; `tlas_storage` is per-view
per SPEC §4.1.10 + §4.1.9 + §4.1.8 invariants).

The `present` pass collapses for the multi-view case as described in
§3.12 (offscreen views skip drawable acquire). Other than `present`,
no pass class behaves differently across views.

### 3.14 Pass declaration / execute lifetime

```text
phase-6 cull-extract    ─► RenderFrame finalised
                                     │
phase-7 entry                        │
                                     ▼
graph builder thread:
  for each View v:
    for each pass class P in §3.1 order:
      record  = P::make_record(RenderFrame, v, RenderSettings)
        │
        │  PassRecord = (name, queue, priority, requires_caps, execute)
        ▼
      builder.add_*_pass(record.desc, reads, writes, record.execute)
                                     │
  graph.compile() ────────────────► ExecutionPlan
                                     │
per-pass encoder workers:
  for each pass in plan.ordered_list:
    encoder = command_buffer.open(pass.queue)
    bindings = plan.bindings_for(pass)
    timestamp(encoder, begin)
    pass.execute(encoder, bindings)        // ← per-pass body lives here
    timestamp(encoder, end)
    encoder.end()
                                     │
plan recorder:
  emit barriers per plan.barrier_set
  signal cross-queue fences
  commit per queue
                                     ▼
phase-7 exit  ─► PresentFence signalled
```

The `PassRecord` value lives only inside the graph (destroyed when the
graph is destroyed at frame retire, SPEC §4.1.2 lifetime). The
execute lambda captures only POD or `eastl::span` slices into the
`RenderFrame` (SPEC §4.1.3). No owning heap allocations cross the
build → execute seam.

## 4. Public Surface

### 4.1 No new types beyond SPEC §5

The aggregate adds no new public types. The render plugin's public
header (SPEC §5) already publishes `Pass`, `PassDesc`, `PassPriority`,
`PassExecuteFn`, `Bindings`, `Queue`, `AccessKind`, `ResourceUsage`,
`ResourceFormat`, `ResourceLifetime`, `ResourceDesc`, `Capability`,
`CapabilitySet`, `RenderSettings`, `MaterialHandle`, `MeshHandle`,
`ViewHandle`, `BLASHandle`, `TLASHandle`, `HZBHandle`,
`ClusterCullStateHandle`, and the `GraphBuilder::add_raster_pass`,
`add_compute_pass`, `add_rt_pass` entry points. The eleven pass
classes consume that surface verbatim.

The pass-class **factories** (`make_<name>_pass`) are private to the
render dylib — they live under `render/src/passes/<name>.{hpp,cpp}`
and are called only by `graph/builder.cpp` (SPEC §6.1). No symbol
they emit crosses the plugin ABI.

### 4.2 Pass interface — C++23 concept

The pass interface is a C++23 concept satisfied by every pass-class
factory. No virtuals; no inheritance.

```cpp
// render/src/passes/pass_concept.hpp (private to the render dylib)
#pragma once
#include <glibre/render/render.hpp>          // §5 public header

namespace glibre::render::passes {

template <class Factory>
concept RenderPassFactory = requires(
    Factory                       f,
    GraphBuilder&                 b,
    const RenderFrame&            rf,
    ViewHandle                    v,
    const RenderSettings&         rs,
    const CapabilitySet&          caps) {

    // declare(builder, ...) registers the pass with the graph; returns
    // Result<void> per SPEC §5 — failure ⇒ render::Error.
    { f.declare(b, rf, v, rs, caps) } noexcept
        -> std::same_as<Result<void>>;

    // (Optional) name() is a debug-only string view; required for
    // diagnostic overlay registration. Stable per pass class.
    { f.name() } noexcept -> std::same_as<eastl::string_view>;
};

}  // namespace glibre::render::passes
```

Each pass file (`passes/<name>.cpp`) defines a stateless factory
struct (e.g. `struct GBufferPass { ... };`) whose `declare` method
composes the typed reads/writes spans, builds the execute lambda
internally, and calls `b.add_raster_pass(...)` / `add_compute_pass`
/ `add_rt_pass`. The lambda captures only POD data plus references
into the const `RenderFrame`.

```cpp
// passes/gbuffer.hpp (sketch — body in gbuffer.cpp)
struct GBufferPass {
    [[nodiscard]] static constexpr eastl::string_view name() noexcept {
        return "gbuffer";
    }

    [[nodiscard]] Result<void>
    declare(GraphBuilder&, const RenderFrame&, ViewHandle,
            const RenderSettings&, const CapabilitySet&) noexcept;
};
static_assert(passes::RenderPassFactory<GBufferPass>);
```

The `static_assert` next to each factory enforces the concept at
compile time; any factory that fails it fails the build under
`clang++ -std=c++23 -Werror -Wall -Wextra -Wpedantic`.

### 4.3 Builder-side dispatch

`graph/builder.cpp` (SPEC §6.1) calls each pass-class factory in the
fixed §3.1 order:

```cpp
// graph/builder.cpp (sketch)
Result<void>
register_mvp_passes(GraphBuilder& b, const RenderFrame& rf, ViewHandle v,
                    const RenderSettings& rs, const CapabilitySet& caps) {
  return BlasRefitPass{}.declare(b, rf, v, rs, caps)
    .and_then([&] { return TlasBuildPass{}.declare(b, rf, v, rs, caps); })
    .and_then([&] { return ClusterCullPass{}.declare(b, rf, v, rs, caps); })
    .and_then([&] { return GBufferPass{}.declare(b, rf, v, rs, caps); })
    .and_then([&] { return HzbBuildPass{}.declare(b, rf, v, rs, caps); })
    .and_then([&] { return ShadowRtPass{}.declare(b, rf, v, rs, caps); })
    .and_then([&] { return AoRtPass{}.declare(b, rf, v, rs, caps); })
    .and_then([&] { return LightingPass{}.declare(b, rf, v, rs, caps); })
    .and_then([&] {
        return TransparentForwardPass{}.declare(b, rf, v, rs, caps);
    })
    .and_then([&] { return PostPass{}.declare(b, rf, v, rs, caps); })
    .and_then([&] { return AaUpscalePass{}.declare(b, rf, v, rs, caps); })
    .and_then([&] { return PresentPass{}.declare(b, rf, v, rs, caps); });
}
```

The chain uses `std::expected`'s monadic `and_then` per
`reviews/decisions/error-model.md` §"Composition Rules" item 3. Any
factory's failure short-circuits the chain; the partial graph is
discarded by `RenderGraph::compile()` per SPEC §4.2 invariant 1.

### 4.4 No exception path

Every pass factory is `noexcept`; every execute lambda is `noexcept`.
The render plugin compiles with `-fno-exceptions` per
`reviews/decisions/error-model.md` §"Decision" item 3. Failure
crosses the boundary as `render::Error` only, threaded through
`Result<void>` returns at every `add_*_pass` and at every execute.

### 4.5 Bindings struct (opaque, SPEC §5)

The `Bindings` struct is opaque at the public ABI (SPEC §5) and
internal at the implementation. Per-pass bodies recover typed views
from it through helper accessors that live in
`render/src/graph/pass.hpp`:

```cpp
// graph/pass.hpp (private)
namespace glibre::render::detail {

[[nodiscard]] eastl::span<const std::byte>
constant_buffer(const Bindings&, std::uint32_t slot) noexcept;

[[nodiscard]] PSOHandle pso(const Bindings&, std::uint32_t slot) noexcept;

[[nodiscard]] PhysicalAllocHandle
physical_for(const Bindings&, VirtualResourceHandle) noexcept;

}  // namespace glibre::render::detail
```

Pass execute bodies call into these accessors; they never read raw
heap addresses. `physical_for` is the only allowed mapping from a
declared VirtualResourceHandle to its plan-resolved physical
allocation, and it asserts in debug builds that the handle is in the
pass's declared access set (SPEC §4.1.3 invariant 1; §10 below).

## 5. Hot / Cold Path Split

The aggregate's hot/cold split is unambiguous:

| Path  | Surface                                    | When                          | Rule                                  |
|-------|--------------------------------------------|-------------------------------|---------------------------------------|
| Cold  | `Factory::declare` (per-pass record build) | once per `(View, FrameCounter)` per pass class | Per-frame; no GPU calls; allowed to allocate from per-frame arena. |
| Hot   | `PassRecord::execute` (per-pass encoder)   | once per draw / dispatch / blit | Records only; **no allocations**; **no ECS reads**; **no logging on success**. |

**Cold** path is per-frame. Each `Factory::declare` call:

- Resolves a small set of `VirtualResourceHandle`s through
  `GraphBuilder::declare_transient` / `declare_persistent` /
  `declare_imported` (SPEC §5).
- Composes a typed `eastl::span<const ResourceAccess>` for reads
  and writes.
- Builds the execute lambda by capturing `eastl::span` slices into
  the `RenderFrame`'s SoA columns plus a few PODs (camera, jitter,
  `RenderSettings` snapshot).
- Calls `add_*_pass` once.

Cost: 11 pass classes × 1 View (S1) ≈ 11 `add_*_pass` calls per frame.
Combined cold-path cost is the §9.3 row "register per-`View` passes"
= 0.10 ms; this is render's whole-graph build CPU cost.

**Hot** path is per-draw. The plan recorder invokes each pass's
execute lambda once with `MetalCommandBuffer&` and the resolved
`Bindings&`. The lambda:

- May open one Metal encoder (graphics / compute / blit / accel-
  structure / RT) at the queue declared by the PassRecord.
- May issue draw / dispatch / blit / RT-trace commands.
- May read pre-resolved bindings through the `detail::*` accessors.
- May insert one paired GPU timestamp at encoder begin / end (§9.6.1).
- **May not** allocate, log on success (debug overlay aside),
  enter the ECS, mutate the `RenderFrame`, or open more than one
  encoder (SPEC §4.1.3 invariants 2-4; §4.1.6 invariant 3).

The split is enforced by debug-build hooks on `MetalCommandBuffer`
(SPEC §4.1.6 invariant 3 and §4.1.3 invariant 4); shipping builds
rely on the build-time concept check (`RenderPassFactory`) plus the
plan compiler's declared-access enforcement (SPEC §4.2 invariant 1).

`RenderGraph::compile()` is between cold and hot — the compiler walks
the declared edges, builds the topological order, runs the alias
planner, emits barriers, and returns the cached `ExecutionPlan`
(SPEC §6.1 `graph/compile.cpp`; §6.2.2 step 2). Compile is owned by
the graph-aggregate sibling design (#760); pass bodies are not
involved.

## 6. Concurrency

Pass bodies are designed for the SPEC §6.3 three-thread-role model.

### 6.1 Cold-side: graph builder thread (one)

Per-pass factory `declare` methods run sequentially on the graph
builder thread (SPEC §6.3). The thread is pinned and owns the
graph's per-frame arena; no cross-thread state is touched. Multi-view
fan-out reuses the same thread sequentially per `View` (SPEC §6.3 +
§3.2 collapse #7). Eleven `declare` calls per `View` × ≤ 4 views
(MVP ceiling) = ≤ 44 calls per frame; bounded.

### 6.2 Hot-side: per-pass encoder workers (≤ three)

Once the `ExecutionPlan` exists, per-pass execute lambdas are
recorded in parallel into per-queue `MetalCommandBuffer`s. The plan's
queue assignment partitions the eleven pass classes into independent
record streams (SPEC §6.3); per the §3.1 catalog, the partition is:

| Worker (queue) | Passes recorded                                                                                |
|----------------|------------------------------------------------------------------------------------------------|
| Graphics       | `gbuffer`, `transparent_forward`, `post`, `aa_upscale`, `present`                              |
| Compute        | `blas_refit`, `tlas_build`, `cluster_cull`, `hzb_build`, `shadow_rt`, `ao_rt`, `lighting`       |
| Copy           | (none in MVP; reserved for post-MVP asynchronous staging)                                       |

Inside one queue, passes record sequentially in plan order on one
worker. Across queues, the three workers record concurrently. The
inter-queue ordering (e.g. `gbuffer` → `lighting`) is mediated by
plan-emitted fences (SPEC §4.1.5 + §4.2 invariant 4); pass bodies do
not synchronise.

Worker count is bounded by queue count (three in MVP; SPEC §6.3); no
further scaling — Metal command-buffer recording is not the
bottleneck (SPEC §6.3).

### 6.3 No cross-pass mutex

Pass bodies do not share mutable state. Reads of the `RenderFrame`,
`ExecutionPlan`, plan-resolved `Bindings`, `PSOCache`, persistent
`Resource`s, and `RenderSettings` are all `const` from the worker's
perspective. The `PSOCache::get` path is internally lock-free for
hits and uses a single backend mutex for misses (covered by SPEC
§4.1.7 detailed design, spike #766) — pass bodies see neither.

### 6.4 No re-entrancy

A worker that has opened an encoder for `Pass A` does not call
another pass's execute body before closing the encoder. This is
structural — the plan recorder is a single sequential loop per
worker — not a runtime invariant. No callback mechanism re-enters
the body.

### 6.5 Frame-phase ownership

Pass bodies own no frame phase. They run **inside** phase 7 under
the plan recorder (SPEC §6.2.2 step 3). They never run on the
driver thread directly: the driver enters phase 7, kicks the three
encoder workers, and waits on the slowest queue's submit-fence
(SPEC §9.3 + §6.3). The driver's own time inside phase 7 is the
0.50 ms "Per-pass `execute()` recording (driver-side dispatch)"
slot in §9.3 — this is the time the driver waits on worker
completion, not the time the pass bodies record.

## 7. Persistence + ABI

### 7.1 Passes persist nothing

The aggregate owns no on-disk state. Per SPEC §7 ("Persistence's
schemas") render's persistent surface is intentionally narrow:
`RenderSettings`, `PSOCacheRecord`, the GPU-timestamp ring (debug-
gated). Pass bodies neither read nor write any of those; they consume
`RenderSettings` (SPEC §5 value type) by const reference inside
`declare`, and they emit per-pass timestamps that the diagnostic
aggregate (`§3.2 collapse #10`) is responsible for ringing.

`PassRecord` itself lives in the graph's per-frame arena (SPEC
§4.1.2 lifetime); destroyed at frame retire. The execute lambda is
captured by the PassRecord and lives in the same arena. Nothing in
the aggregate survives a frame.

### 7.2 No middleman growth

The aggregate adds nothing to `glibre-types.dylib`
(`reviews/decisions/fory-codegen.md`). The data middleman boundary
sees only SPEC §7's existing render schemas. The
`RenderPassFactory` concept is a private template in the render
dylib; it does not cross the plugin ABI seam
(`reviews/decisions/plugin-abi.md`).

### 7.3 Pass-list ABI surface (private)

`graph/builder.cpp`'s `register_mvp_passes` (§4.3) is a private
function inside the render dylib. The eleven factory `declare`
methods are also private. The only public ABI surface this
aggregate touches is the existing SPEC §5 `GraphBuilder::add_*_pass`
+ `PassExecuteFn` declarations.

The pass-class catalog (§3.1 the eleven names) is therefore not part
of the plugin manifest's `passes:` list (`reviews/decisions/plugin-abi.md`
§"Plugin Manifest Schema" `PassDecl`); that list is reserved for
**plugin-registered** passes from non-render plugins (e.g. a future
`vfx` plugin registering a particle-draw pass) and is empty for the
render plugin itself. Render's eleven passes are baked into
`graph/builder.cpp`.

### 7.4 No serialised render-graph files

PHILOSOPHY's anti-pattern explicitly forbids serialised render-graph
files. The render-passes aggregate respects this: graph topology is
C++ code in `graph/builder.cpp`; the `ExecutionPlan` cache is an
in-process structural-hash cache (SPEC §4.1.5) that does not persist
across process lifetimes; pass bodies are C++ functions in
`passes/<name>.cpp`. No `.fory`, no `.json`, no editor-exported pass
list.

The diagnostic overlay (`§3.2 collapse #10`) visualises the live
graph + per-pass GPU timing, but the visualisation reads in-process
state and is debug-only (build-time gated; SPEC §6.5).

## 8. Hot-Reload

### 8.1 What survives across a render-plugin reload

Per `reviews/decisions/hot-reload-protocol.md` §"State Survival
Rules" the survival rule is mechanical: state with a `.fory` schema
survives; everything else is rebuilt. For the render-passes
aggregate:

| State                                       | Survival | Reason                                                                     |
|---------------------------------------------|----------|----------------------------------------------------------------------------|
| The eleven pass-class factories             | rebuilt  | C++ symbols inside the render dylib; the new dylib redefines them.         |
| `PassRecord` instances                      | rebuilt  | Per-frame arena values; never persist anyway.                              |
| Execute lambdas                             | rebuilt  | Captured in `PassRecord`s; per-frame.                                      |
| `RenderGraph` + `ExecutionPlan` + cache     | rebuilt  | Per-frame; cache is in-memory structural hash (SPEC §4.1.5).               |
| Transient `Resource`s                       | rebuilt  | Per-frame by definition (SPEC §4.1.4 invariant 1).                         |
| Persistent `Resource`s with `.fory` schemas | survive  | E.g. `RenderSettings` (SPEC §7.1.1), `PSOCacheRecord` (SPEC §7.1.2). The new plugin re-imports them through the plugin loader's drain → swap → migrate → resume sequence. |
| GPU resources (HZB, ShadowAtlas, history color, ring buffers, TransientPool heaps) | rebuilt | The outgoing plugin's `glibre_plugin_drain` releases all GPU handles; the incoming plugin re-acquires equivalents in `glibre_plugin_register` (`hot-reload-protocol.md` §"Step 1 — Drain"). |
| `PSOCache` resident pipelines               | rebuilt  | The new plugin re-warms from the shader plugin's manifest at register; in-flight shipping use is bounded by `PSOCache::invalidate_by_shader_hash` per SPEC §4.1.7 invariant 4. |
| Capability set                              | rebuilt  | Re-probed at register against the host (SPEC §10.3 row `RtCapabilityMissing` recovery — fresh probe). |

The new render plugin's `glibre_plugin_register` walks the same
eleven-pass catalog and produces the same eleven factories. The
graph builder's `register_mvp_passes` order is fixed in the **new**
dylib's `graph/builder.cpp` (PHILOSOPHY: "Render graph is C++ code");
it does not depend on any pre-reload state.

### 8.2 No `migrate` function for pass bodies

Pass bodies emit no persistent on-disk records (§7.1). The
`migrate_<Type>_vN_to_vNplus1` family
(`hot-reload-protocol.md` §"Migrate Function Contract") applies only
to persistent component / singleton schemas with a `.fory` source.
The render-passes aggregate has none. A schema bump on
`RenderSettings` / `PSOCacheRecord` invokes the SPEC §7 migration
path; the pass bodies simply consume the migrated value in the
post-reload frame.

### 8.3 First-frame post-reload

The first frame after a render-plugin reload sees:

- A fresh `MetalDevice` (or, more commonly, the same device handle
  re-imported through the new dylib's init).
- A fresh `PSOCache` warming from the shader manifest at register
  time (SPEC §4.1.7); cache hits start cold.
- No history color → TAA falls back to its first-frame body inside
  `aa_upscale` (handled by the existing factory's variant; not a
  reload-specific code path).
- No HZB → phase 6 of the first post-reload frame skips occlusion
  cull (frustum cull only); `hzb_build` populates the persistent HZB
  for the next frame.
- No prior `ExecutionPlan` cache → first post-reload frame's
  `compile()` is a cache miss (SPEC §9.3 reserve absorbs this — one
  cache miss per `View` per reload is bounded).

These are the same behaviours as engine cold-start; reload is
indistinguishable from cold-start at the pass-body level.

### 8.4 Refusal cases (pass-side)

The plugin loader's refusal cases (`reviews/decisions/hot-reload-protocol.md`
§"Refusal Cases" + `plugin-abi.md` §"Failure Modes → core::Error")
do not directly propagate from pass bodies — the bodies do not run
during the loader sequence. The pass-aggregate's contribution to
hot-reload refusal is indirect:

- **Capability mismatch.** If the new plugin's pass-record factories
  declare a `Capability` (`HardwareRayTrace`, `MeshShaders`,
  `MetalFx`) that the host does not advertise, the first post-reload
  graph compile returns `render::Error::CapabilityNotSupported`
  (SPEC §10.3 rows `RtCapabilityMissing` /
  `MeshShaderCapabilityMissing`). Recovery is `lower-tier` per SPEC
  §10.2 — the next compile uses the fallback factories. No reload
  refusal at the loader layer; the failure is in-frame and survivable.
- **PSO miss with unknown `shader_hash`.** Surfaces as
  `render::Error::PipelineCompileFailed` from `PSOCache::get`
  (SPEC §4.1.7 invariant 2); recovery is `lower-tier`. No loader-layer
  refusal.
- **Shader-module load failure.** Surfaces as
  `render::Error::ShaderModuleLoadFailed` (SPEC §10.3 row); during
  hot-reload register the recovery is `lower-tier` (SPEC §10.3 row
  Recovery column). The pass aggregate does not handle this — the
  Metal-backend aggregate does, before any pass factory runs.

### 8.5 Test hooks

`hot-reload-protocol.md` §"Test Hooks" exposes
`enqueue_hot_reload(plugin_fqn, replacement_dylib_path)` under
`#if defined(GLIBRE_E2E)`. The pass aggregate's reload test is the
SPEC §11 user story #395 ("PSO cache invalidation by shader_hash on
hot-reload"); E2E plan:

1. `enqueue_hot_reload("glibre.render", new_dylib_path)`.
2. Wait for `HotReloadCompleted` event.
3. Assert frame counter advances by 1 between request and completion.
4. Assert the next compiled `ExecutionPlan` from the new dylib
   declares the same eleven pass classes in the same order
   (§3.1 / §4.3 catalog + order).
5. Assert per-pass GPU timing returns to within § 9.4 ceilings within
   3 frames after reload.

The test does not cover step 5 in this design's scope (it belongs
to the SPEC §11 #398 GPU-fault-restart story), but the catalog
stability assertion (step 4) is the pass-aggregate's reload
acceptance.

## 9. Performance

### 9.1 Per-pass CPU cost (cold path, declare)

Per §3.14 + §4.3, each `Factory::declare` call composes one
PassRecord and calls one `add_*_pass`. Cost decomposition (per call,
typical S1):

| Step                                 | Budget   | Notes                                                                            |
|--------------------------------------|----------|----------------------------------------------------------------------------------|
| Compose `reads`/`writes` spans       | ≤ 1 µs   | ≤ 16 entries each; `eastl::array`-backed, allocated in per-frame arena.           |
| Build execute lambda (capture POD)   | ≤ 2 µs   | Captures ≤ 8 `eastl::span` slices + ≤ 4 PODs.                                     |
| `GraphBuilder::add_*_pass` call      | ≤ 5 µs   | Validation (declared-set closure, queue purity) + node insertion.                |
| **Per-factory call**                 | **≤ 8 µs**| Bound for all eleven classes uniformly.                                          |
| 11 calls × 1 View (S1)               | **≤ 0.09 ms** | Sums into SPEC §9.3 row "register per-`View` passes" (0.10 ms ceiling).      |

The 0.09 ms is the catalog's worst-case CPU cost on the graph
builder thread per frame for the S1 fixture. The 0.10 ms cell ceiling
in SPEC §9.3 covers up to 4 views (MVP ceiling) — `4 × 0.09 = 0.36`,
which exceeds the 0.10 ms cell. The cell ceiling is dominated by
single-view S1 because multi-view fan-out occurs only in editor /
capture frames; the steady-state production case is one view, and
the ceiling is set against that. The reserve in SPEC §9.3 absorbs
multi-view spikes (cache-miss compile is 0.10 ms reserve; multi-view
register is part of the same reserve allocation by the spike that
amends it).

### 9.2 Per-pass GPU cost (hot path)

Each pass's GPU slice is fixed in SPEC §9.4 and re-listed here as
contractual ceilings the pass body must respect:

| Pass                  | Queue     | GPU ms ceiling (S1, M1) | Source                |
|-----------------------|-----------|--------------------------|-----------------------|
| `cluster_cull`        | Compute   | 0.5                      | SPEC §9.4 `meshlet-cull` |
| `gbuffer`             | Graphics  | 2.5                      | SPEC §9.4 `gbuffer-mesh` |
| `shadow_rt` (incl. `blas_refit` + `tlas_build`) | Compute | 1.5 | SPEC §9.4 `shadow-rt` (§9.4.1 funds refit inside) |
| `ao_rt`               | Compute   | (folded into `shadow_rt`)| SPEC §9.4 final row     |
| `hzb_build`           | Compute   | (folded into `shadow_rt`)| SPEC §9.4 final row     |
| `lighting`            | Compute   | 1.5                      | SPEC §9.4 `lighting-deferred` |
| `transparent_forward` | Graphics  | 0.5                      | SPEC §9.4 `transparent-forward` |
| `post`                | Graphics  | 1.0                      | SPEC §9.4 `post`         |
| `aa_upscale`          | Graphics  | (absorbed into `post`)   | SPEC §9.4 `post` row note |
| `present`             | Graphics  | 0.5                      | SPEC §9.4 `present`      |
| **GPU subtotal (wall-clock)** | **8.0 ms** |                | SPEC §9.4 Phase-7 GPU cell |

The pass body's responsibility is to dispatch within its slice
ceiling under S1; perf-budget gate enforcement is via the SPEC §9.6
Catch2 `BENCHMARK` blocks (per-pass timestamps from
`MTLCounterSampleBuffer`, SPEC §9.6.1). A pass that overshoots its
slice on the S1 fixture fails the gate and triggers
`render::Error::ResourceResidencyExceeded` if the cause is
heap-driven, or a perf-budget amendment spike if the cause is
algorithmic.

### 9.3 GPU timestamp insertion (per pass, mandatory)

Every execute lambda inserts a paired GPU timestamp at encoder
begin / end, per SPEC §9.6.1. The mechanism is a `RAII` guard:

```cpp
// graph/timing.hpp (private)
struct PassTimingGuard {
    PassTimingGuard(MetalCommandBuffer& cb, eastl::string_view name) noexcept;
    ~PassTimingGuard() noexcept;  // emits the end-timestamp + ring write.
};
```

Each pass body opens the guard at the encoder's first record point:

```cpp
[[maybe_unused]] PassTimingGuard t{cb, "gbuffer"};
```

The guard routes through `MTLCounterSampleBuffer` per SPEC §9.6.1
when `Capability::TimestampQueries` is set; on hosts without it
the guard is a compile-time no-op (zero codegen). The MVP M1 baseline
carries the capability; the no-op path exists for future low-end
host targets.

### 9.4 No allocations in execute (strict-mode CI)

Per SPEC §4.1.3 invariant 4 + perf-budget Allocator Rules item 4
(transient arena exemption), pass bodies never allocate from the
global allocator. Strict-mode (`GLIBRE_ALLOC_STRICT=1`, perf-budget
Allocator Rule 2) asserts zero `ContextTag::render` allocations
between pass `execute` entry and exit. CI gate row from SPEC §9.6.2
"transient-pool drained at phase 9, strict" covers this; this
aggregate's contribution is the per-pass leak guard.

A pass body that needs a small per-record buffer (e.g. an indirect-
arg buffer's transient header) requests it through
`Bindings::ring_slice(...)` (SPEC §5 ring-buffer surface; sibling
design under spike #762 details the API). Ring slices are pre-
allocated by the plan recorder before `execute` is invoked.

### 9.5 No frame-budget contribution beyond §9.4 / §9.3

Pass bodies' contribution to render's per-frame budget is exactly
the sum in §9.2 (GPU) and the §9.1 cold-path slice (CPU). They do
not contribute to phase 6 (cull-extract) or phase 9 (present
pacing). The Metal-driver-side jitter on submit (SPEC §9.3 reserve)
is not attributed to pass bodies — it is queue-level, not
encoder-level.

## 10. Failure Modes

The pass aggregate operates inside the SPEC §10 closed sum
(eighteen variants). Pass-body failures and per-pass declarations
contribute to a subset; this section binds each contribution to its
trigger and recovery.

### 10.1 Pass-emitted `render::Error` arms

| Variant (SPEC §10.1)       | Trigger from this aggregate                                                                                                                                                | Recovery (SPEC §10.2) |
|----------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------------------|
| `PassUnsupportedConfig`    | A factory declares an access set that violates SPEC §4.1.3 (e.g. `gbuffer` declares fewer than five writes, or two encoders); SPEC §4.2 invariant 5 enforced at compile.   | `abort-engine`        |
| `PassUndeclaredAccess`     | An execute lambda touches a `VirtualResourceHandle` not in its declared `reads`/`writes` (debug-build assertion via `detail::physical_for`; release-build returns error).  | `abort-frame` (debug) / `abort-engine` (release CI gate) |
| `PassDeclaredUseUnused`    | A factory declares a read/write that the execute lambda never touches; SPEC §4.1.3 invariant 1 enforced at compile (lambda body inspected by debug-build hook).            | `abort-engine`        |
| `BarrierConflict`          | Indirect — a pass-class factory declares an access set that the alias planner cannot satisfy together with another pass's set. Surfaces from `RenderGraph::compile()`.     | `abort-engine`        |
| `RenderGraphCycle`         | Indirect — a factory's declared edges form a cycle with another pass's edges. Surfaces from `RenderGraph::compile()`.                                                       | `abort-engine`        |
| `PipelineCompileFailed`    | An execute lambda calls `PSOCache::get(key)` and the cache cannot resolve the `(shader_hash, state_hash)` pair. The pass body returns the error through `Result<void>`.   | `lower-tier`          |
| `PassExecuteFailed` (NEW — `PassExecuteFailed`) | An execute lambda returns `Result<void>` failure for a reason not covered above (e.g. mid-record `MTLCommandBufferError`, indirect-arg buffer overflow). **Note**: this variant does not yet exist in the SPEC §10.1 closed sum; **adding it is an ABI bump**. The MVP solution uses `MeshletCullDispatchFailed` for cluster-cull-specific failures and `QueueSubmitFailed` for general encoder failures, leaving `PassExecuteFailed` as an [OPEN] question (§12). | `abort-frame` (typical) |
| `MeshletCullDispatchFailed`| `cluster_cull` execute body catches an indirect-arg buffer overflow (SPEC §10.3 row).                                                                                       | `disable-feature` (drops `MeshShaders` capability for the process) |
| `BlasUnavailable` / `BlasBuildFailed` | `blas_refit` execute body catches a Metal RT-encoder error.                                                                                                    | `disable-feature`     |
| `TlasBuildFailed`          | `tlas_build` execute body catches a Metal RT-encoder error.                                                                                                                | `disable-feature`     |
| `SwapchainAcquireFailed`   | `present` execute body's drawable acquire returns `nil` past timeout.                                                                                                       | `abort-frame`         |
| `QueueSubmitFailed`        | Indirect — pass body records succeed but `[MTLCommandQueue commit]` fails. Not a pass-body failure; included here so reviewers see the seam.                               | `abort-frame`         |
| `CapabilityNotSupported`   | A factory's `requires_caps` exceeds the live `CapabilitySet`. Surfaces from `add_*_pass` at declare time.                                                                  | `lower-tier` (init) / `disable-feature` (post-reload register)  |

The `PassExecuteFailed` row above is the one ABI-add this aggregate
proposes: **leave it as an [OPEN] question** (§12) until the first
real-world pass body needs an arm not already covered by
`MeshletCullDispatchFailed`, `BlasBuildFailed`, `TlasBuildFailed`,
`QueueSubmitFailed`, or `PipelineCompileFailed`. For MVP the existing
arms suffice; if the spike that lands the per-pass error mapping
discovers a gap, the closed sum bumps then.

### 10.2 Refusal vs frame-skip seam

Per SPEC §10.2 the recovery ladder for pass-body failures is:

- **Build-time refusal** (`PassUnsupportedConfig`,
  `PassUndeclaredAccess`, `PassDeclaredUseUnused`,
  `BarrierConflict`, `RenderGraphCycle`) → `abort-engine`. The
  graph never produces a plan; the failing frame is dropped; the
  engine shuts down. These are programmer errors caught early.
- **Resource-level refusal** (`HeapOutOfMemory`,
  `ResourceResidencyExceeded`) → `lower-tier`. The next compile
  uses the lower tier's reduced-size targets.
- **Capability-level refusal** (`CapabilityNotSupported`) →
  `lower-tier` at init / `disable-feature` post-reload. Pass class
  is rebuilt without the capability or omitted.
- **Per-frame failure** (`SwapchainAcquireFailed`,
  `QueueSubmitFailed`, `MeshletCullDispatchFailed`,
  `BlasBuildFailed`, `TlasBuildFailed`, `PipelineCompileFailed`,
  `GpuTimeout`) → either `abort-frame` (drawable/submit) or
  `disable-feature` (RT/MeshShader path) or `lower-tier` (PSO
  miss). The frame is dropped; the prior frame is re-presented;
  next frame proceeds.
- **GPU fault** (`GpuFault`) → `hot-reload-restart` per SPEC §10.4.
  Pass bodies do not detect the fault directly; `platform`'s
  phase-9 fence wait does. The pass aggregate is rebuilt by the
  reload mechanism (§8 above).

### 10.3 Logging discipline

Per `reviews/decisions/error-model.md` §"Logging / Telemetry":

- **`error` severity**: `PassUnsupportedConfig`, `BarrierConflict`,
  `RenderGraphCycle`, `PassDeclaredUseUnused`,
  `PassUndeclaredAccess` (release-build only).
- **`warn` severity**: `PipelineCompileFailed`,
  `MeshletCullDispatchFailed`, `BlasBuildFailed`,
  `TlasBuildFailed`, `SwapchainAcquireFailed`,
  `QueueSubmitFailed`, `CapabilityNotSupported`, `GpuTimeout`.
- **`info` severity** (debug overlay only): per-pass GPU timing
  rows; absent in release builds.

Every log line carries `ErrorContext` per `error-model.md`:
`(pass_class, view_handle hex, frame_counter, pso_key hex when
applicable)`. Pass bodies do not log on success — the diagnostic
overlay reads the GPU-timestamp ring directly (SPEC §9.6.1).

## 11. Test Plan

All tests are Catch2; sources under `tests/render/`. Each
`type:plan` issue downstream of this design owns one or more named
tests below; the SPEC §11 acceptance criteria (`specs/render/SPEC.md`
§11) lists the user-story-level tests this aggregate is on the
hook for (#386, #387, #389, #390, #391, #393, #396).

### 11.1 Unit tests (mocked encoder, `tests/render/passes/`)

The `MetalCommandBuffer` is replaced with a mock
(`MockMetalCommandBuffer`) that records every encoder open / record /
close call and exposes the recorded sequence to the test. The
`PSOCache`, `RTAccelStructures`, `HZB`, `ClusterCullState`, and
`TransientPool` are also mocked through their existing protocol
boundaries (each sibling aggregate's design provides the mocks).
This isolates each pass body from the rest of the render plugin.

| Test name                                                    | Pass class           | Asserts                                                                                                |
|--------------------------------------------------------------|----------------------|--------------------------------------------------------------------------------------------------------|
| `gbuffer_pass_declares_five_writes_atomically`               | `gbuffer`            | The pass declares exactly five writes (4 MRT + visID + depth) on one encoder; SPEC §4.2 invariant 5.   |
| `gbuffer_pass_rejects_split_encoder_attempt`                 | `gbuffer`            | A factory variant that opens two encoders is rejected by `add_raster_pass`; `PassUnsupportedConfig`.   |
| `gbuffer_pass_falls_back_when_meshshaders_absent`            | `gbuffer`            | Capability set without `MeshShaders` causes the factory to register the vertex+amplification fallback. |
| `cluster_cull_pass_declares_persistent_thread_dispatch`      | `cluster_cull`       | Dispatch group count fixed at init from `QualityTier`; no hot-path branching.                          |
| `lighting_pass_reads_light_cluster_under_pcf`                | `lighting`           | `ShadowTier::Pcf` factory declares `shadow_atlas_csm_cascades` read; no `shadow_rt_output` read.       |
| `lighting_pass_reads_shadow_rt_under_raytraced`              | `lighting`           | `ShadowTier::RayTraced` factory declares `shadow_rt_output` + `tlas` reads; uses `add_rt_pass`.         |
| `transparent_forward_reads_same_light_cluster`               | `transparent_forward`| Both `lighting` and `transparent_forward` declare reads on the same `LightCluster` resource handle.    |
| `hzb_build_writes_persistent_pyramid`                        | `hzb_build`          | Write target is persistent (`HZB::write_target(view)`), not transient.                                 |
| `blas_refit_writes_only_update_slots`                        | `blas_refit`         | Pass writes only into refit scratch + BLAS update slot; never to BLAS-static memory (SPEC §4.1.8 inv 3).|
| `tlas_build_selects_rebuild_or_refit_at_factory`             | `tlas_build`         | Factory selects rebuild when `tlas_diff(view).membership_churn > threshold`; refit otherwise.          |
| `tlas_build_declares_raw_on_blas_refit`                      | `tlas_build`         | Read-after-write on `blas_refit` outputs; SPEC §4.2 invariant 4.                                       |
| `post_pass_chain_composed_from_render_settings`              | `post`               | Disabled `bloom_enable` removes the bloom kernels from the dispatch sequence; remaining order unchanged.|
| `post_pass_uses_single_encoder`                              | `post`               | All selected kernels record on one encoder; SPEC §4.1.3 invariant 2 (queue purity) + invariant 3 atomic.|
| `aa_upscale_pass_selects_variant_at_build`                   | `aa_upscale`         | Each `(aa_mode, upscaler)` cell selects the matching body; eight variant assertions.                   |
| `aa_upscale_falls_back_when_metalfx_absent`                  | `aa_upscale`         | `MetalFx` capability absent ⇒ factory selects `BuiltinFallback` upscale.                                |
| `present_pass_acquires_drawable_and_signals_fence`           | `present`            | One blit, one `presentDrawable:`, one `PresentFence` signal; main view path.                           |
| `present_pass_offscreen_skips_drawable`                      | `present`            | Offscreen view → blit-to-imported-texture; no drawable acquire; no fence signal.                        |
| `present_pass_returns_swapchain_acquire_failed_on_nil`       | `present`            | Mock `next_drawable_handle()` returns invalid handle ⇒ `Result<void>` carries `SwapchainAcquireFailed`.|
| `pass_concept_satisfied_by_every_factory`                    | (all 11)             | Compile-time `static_assert(passes::RenderPassFactory<T>)` for every factory struct.                   |
| `pass_execute_does_not_allocate`                             | (all 11)             | Strict-mode trace: zero `ContextTag::render` allocations between execute entry and exit.               |
| `pass_capability_predicate_evaluated_at_build`               | (all 11)             | Mock `CapabilitySet` with bits cleared causes guarded passes to be absent from the node list.          |
| `pass_timestamps_paired_at_encoder_boundary`                 | (all 11)             | Every execute opens `PassTimingGuard`; mock `MTLCounterSampleBuffer` observes paired writes.           |
| `pass_factory_returns_capability_not_supported_when_required`| `shadow_rt`, `ao_rt` | `HardwareRayTrace` absent + factory declares it required ⇒ `add_rt_pass` returns `CapabilityNotSupported`. |
| `pass_undeclared_access_caught_in_debug`                     | (all 11)             | Mock encoder records access to a handle not in declared set ⇒ `PassUndeclaredAccess`.                  |

### 11.2 Integration tests (real device, `tests/render/integration/`)

Real `MetalDevice` (or platform fixture's headless metal-cpp device,
SPEC §6.6 cross-platform readiness; the platform fixture lives under
`tests/render/integration/fixtures/`). These tests gate on the host
having a Metal 4 device.

| Test name                                                       | Scope                    | Asserts                                                                                                |
|-----------------------------------------------------------------|--------------------------|--------------------------------------------------------------------------------------------------------|
| `mvp_frame_records_eleven_passes_in_order`                      | end-to-end S1 fixture    | One frame compiles a graph with all eleven pass classes for the main view in §3.1 catalog order.       |
| `gbuffer_pass_writes_atomic_set_real_device`                    | `gbuffer` real           | Mesh-shader dispatch produces non-zero output in all five MRT attachments; visibility ID matches index.|
| `gbuffer_fallback_real_device`                                  | `gbuffer` fallback       | Vertex+amplification path produces equivalent outputs on a host without `MeshShaders` (capability mock).|
| `lighting_pass_combines_gbuffer_and_clusters`                   | `lighting` real          | Output HDR scene color is non-zero when 8 lights illuminate the S1 scene; `LightCluster` consumed.     |
| `shadow_rt_traces_against_tlas`                                 | `shadow_rt` real         | RT shadow output is non-zero where the S1 character casts a shadow on the floor; uses `RayQuery`.      |
| `blas_refit_precedes_tlas_build_real_device`                    | `blas_refit` + `tlas_build` real | Per-pass GPU timestamps confirm refit completes before TLAS build for the dynamic character.   |
| `post_pass_chain_runs_in_order`                                 | `post` real              | Bloom + tonemap + grade chain produces expected output on a fixture HDR target.                        |
| `aa_upscale_taa_history_blend_real_device`                      | `aa_upscale` real        | TAA body reads persistent history color and produces blended output frame-over-frame.                  |
| `present_pass_signals_fence_real_device`                        | `present` real           | `PresentFence.value` advances by 1 per frame; `platform`'s phase 9 observes the fence.                 |
| `pass_aggregate_acceptance_user_story_386`                      | SPEC §11 #386            | Mesh-shader gbuffer atomicity user-story; full S1 frame, all five outputs valid.                       |
| `pass_aggregate_acceptance_user_story_387`                      | SPEC §11 #387            | Vertex+amplification fallback user-story.                                                              |
| `pass_aggregate_acceptance_user_story_389`                      | SPEC §11 #389            | Persistent-thread cluster cull + deferred lighting parity.                                              |
| `pass_aggregate_acceptance_user_story_390`                      | SPEC §11 #390            | Hybrid-RT shadow with PCSS fallback when `RayQuery` is forced off.                                     |
| `pass_aggregate_acceptance_user_story_391`                      | SPEC §11 #391            | BLAS refit precedes TLAS build every frame.                                                            |
| `pass_aggregate_acceptance_user_story_393`                      | SPEC §11 #393            | `PsoCompileFailed` triggers `lower-tier` recovery; next frame's lighting body reads a coarser PSO.     |
| `pass_aggregate_acceptance_user_story_396`                      | SPEC §11 #396            | Present pass acquires drawable and signals fence within budget.                                        |
| `multi_view_fan_out_two_views`                                  | multi-view               | Editor + main view both compile the eleven-pass catalog; per-view resources distinct.                  |
| `pass_aggregate_hot_reload_catalog_stable`                      | hot-reload               | After `enqueue_hot_reload("glibre.render", ...)`, the next frame's plan declares the same eleven passes.|

### 11.3 Performance microbenchmarks (`tests/render/perf/`)

Per SPEC §9.6.2; this aggregate's contribution to the gate.

| Benchmark name                                                       | Asserts                                                                                                |
|----------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------|
| `BENCHMARK("pass-factory declare under 8us, S1, p99")`               | Each of the eleven `Factory::declare` calls completes ≤ 8 µs on M1.                                    |
| `BENCHMARK("eleven-pass register total under 0.09ms, S1, p99")`      | Sum of the eleven calls ≤ 0.09 ms; sums into SPEC §9.3 ceiling (0.10 ms).                              |
| `BENCHMARK("per-pass GPU within slice, S1, p99")`                    | Each pass's `MTLCounterSampleBuffer`-measured GPU slice ≤ its §9.4 ceiling.                             |
| `BENCHMARK("pass execute zero allocations, strict")`                 | `GLIBRE_ALLOC_STRICT=1`; 600 frames; zero `ContextTag::render` allocations between execute entry/exit. |
| `BENCHMARK("post chain min/max kernel-count, S1, p99")`              | Empty `RenderSettings` (no post effects) ⇒ post pass has zero dispatches and ≤ 5 µs CPU; full settings ⇒ ten dispatches and ≤ 100 µs CPU. |

### 11.4 E2E coverage

The SPEC §11 acceptance-criterion E2E traces (#386, #387, #389,
#390, #391, #393, #396, #401) cover this aggregate through the
full frame loop. The spike delivers no E2E test of its own (per
CLAUDE.md "Tests by type" — spike → deliverable doc). Once the
user-story leaves run, their E2E `.glibre-trace` files under
`tests/e2e/render/` will exercise the catalog.

## 12. Open Questions

- [OPEN] **`PassExecuteFailed` ABI add.** §10.1 row 7 proposes a
  generic per-pass execute-failure arm. The MVP solution leans on
  `MeshletCullDispatchFailed`, `BlasBuildFailed`, `TlasBuildFailed`,
  `QueueSubmitFailed`, `PipelineCompileFailed`, and
  `SwapchainAcquireFailed` for specific pass-body failures. Whether
  the closed sum needs a generic `PassExecuteFailed` arm depends on
  whether a future pass body discovers a failure mode none of the
  six existing arms cover. Owner: `task-breakdown-render-render-passes`
  spike. Resolution gate: first user story or plan that surfaces a
  pass-body failure not in the six existing arms.

- [OPEN] **Per-pass timestamp opt-out for the `present` pass on
  hosts without `Capability::TimestampQueries`.** SPEC §9.6.1 already
  specifies the fallback (per-pass slots not enforced; gate falls
  back to per-frame total). The aggregate's question is whether the
  `present` pass should still emit a host-side wall-clock timestamp
  for the editor's HUD even when `MTLCounterSampleBuffer` is
  unavailable. Owner: tools / editor design. Resolution gate: when
  the editor's perf HUD lands on a non-MVP host.

- [OPEN] **Catalog extensibility seam.** §3.1 + §7.3 fix the eleven
  pass classes inside `graph/builder.cpp` and assert that
  non-render plugins (vfx, material) cannot register new pass
  classes via `plugin-abi.md`'s `PassDecl` list. The first
  external-plugin pass-class registration (post-MVP, when `vfx`
  lands) will need to amend `register_mvp_passes` to include
  registered passes after `transparent_forward` (or another
  catalog-class-specific slot). The exact insertion-order policy is
  undecided. Owner: `vfx` plugin spike (post-MVP). Resolution gate:
  first plugin that declares a `PassDecl` against the render dylib.

- [OPEN] **`post`-chain order amendment policy.** §3.10 fixes the
  ten-kernel order. The first artist-driven request to reorder
  (e.g. grade before tonemap for a stylised look) will need a SPEC
  §6.2.2 amendment plus a `RenderSettings` schema bump. Whether the
  bump is one new field per kernel position or a single ordering
  enum is undecided. Owner: `material` plugin design (which owns the
  authoring surface for grade kernels per SPEC §3.3). Resolution
  gate: first user-story that needs a non-default order.

- [OPEN] **TAA history-color survival across hot-reload.** §8.3
  asserts the first post-reload frame falls back to the first-frame
  TAA body (no history). For long-lived editor sessions this could
  produce a one-frame visual hitch on every plugin reload. Whether
  history color should be promoted to a persistent `.fory` schema
  (so it survives reload per `hot-reload-protocol.md` §"State
  Survival Rules") or remain a transient (current MVP) is undecided.
  Promoting it touches SPEC §7. Owner: `task-breakdown-render-render-passes`
  spike. Resolution gate: first hot-reload UX measurement that flags
  the hitch.

- [OPEN] **Compute-queue worker scaling.** §6.2 limits the encoder
  worker pool to one worker per queue (three total). On M1 the
  compute queue records seven of the eleven pass classes; the
  driver's wait-on-slowest-worker path (SPEC §9.3 row "Per-pass
  `execute()` recording") could become a tail-latency bottleneck if
  per-pass record cost grows post-MVP. Whether to split the compute
  queue's recording across two workers (parallelising independent
  passes inside one queue) is undecided. Owner: render-graph
  detailed design (#760) — the queue assignment lives there, not
  here. Resolution gate: first §9.3 budget overrun attributable to
  per-pass recording on the compute queue.
