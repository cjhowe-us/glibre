# render — Detailed Design: hzb-cull aggregate

> Detailed design for the `HZB` + meshlet/instance occlusion-cull
> aggregate cluster declared in `specs/render/SPEC.md` §4.1.9 and the
> phase-6 `cull/meshlet_cull.cpp` body referenced in §6.2.1 step 2.1
> and §6.5 step 1. Refines §4.1.9, §5 (`HZB` surface + `HZBHandle`),
> §6.2.1 step 2.1 (phase-6 occlusion cull), §6.2.2 step 1.5
> (`passes/hzb_build.cpp` HZB-build pass body), §6.5 step 1+4
> (mesh-shader path interlock), §9.2 (phase-6 CPU breakdown — meshlet
> cull line), §9.4 (GPU `meshlet-cull` slice + folded HZB-build),
> §10 (`MeshletCullDispatchFailed`, `HeapOutOfMemory`,
> `ResourceResidencyExceeded`), and §11 acceptance story #388 in
> place. Cites `reviews/decisions/error-model.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/fory-codegen.md`, and
> `reviews/decisions/frame-phases.md`. Adds **one ABI-add to
> `specs/render/SPEC.md` §5**: `HZB::handle(ViewHandle) const noexcept`
> (read-only pyramid-handle getter, required by `DiagnosticOverlay` #774;
> see §4 and §10.1). All other deviations from the cited records require
> an amendment spike, not an in-place edit. Resolves
> `[SPIKE] design-render-hzb-cull-detailed` (#770). Sibling
> `[SPIKE] task-breakdown-render-hzb-cull-detailed` (#771) is unblocked
> by this design.

## 1. Purpose

The hzb-cull aggregate is render's per-frame, per-`View` GPU
occlusion-culling machinery. Per `specs/render/SPEC.md` §4.1.9 and
the phase-ownership rows of §6.2 / §9.2 / §9.4, it owns:

1. The **`HZB`** entity — a persistent, per-`View` hierarchical-Z
   pyramid (mip chain, min-reduce, reverse-Z). Sized to the active
   render extent; triple-buffered so frame N's build does not race
   frame N+1's read (§4.1.9 invariant 1).
2. The **HZB-build kernel** — a Metal 4 compute kernel run inside
   `passes/hzb_build.cpp` (§6.2.2 step 1.5, §6.5 step 4) that reduces
   this frame's gbuffer depth into next frame's HZB pyramid. Single
   declared `Pass` on `Queue::Compute`; one barrier-emit point.
3. The **meshlet/instance occlusion-cull kernel** — a Metal 4 compute
   kernel run inside `cull/meshlet_cull.cpp` (§6.2.1 step 2.1,
   §6.5 step 1) that reads last frame's `HZB` plus the per-`View`
   instance + meshlet bounds list and emits the meshlet survivor set
   keyed by material into compact GPU-side `IndirectDrawBuffer`s
   (§2 ubiquitous-language `IndirectDrawBuffer`).
4. The **indirect-args output** — a per-`View` ring of
   `MTLDrawIndirectArguments`-shaped (or `MTLDrawIndexedPatch` for
   tessellated paths) compaction buffers material-grouped per
   `SortKey`, consumed by the mesh-shader gbuffer dispatch and by
   the shadow-cascade and reflection-probe gbuffer passes.
5. The **per-`View` `HZBHandle` surface** — the §5
   `HZB::ensure(ViewHandle, HZBDesc)` + `HZB::invalidate(ViewHandle)`
   contract. The aggregate is the only writer to the `HZB` table;
   reads cross plug-in boundaries only via the typed handle.

This aggregate **refuses to own**:

- **Pass topology.** The render-graph DAG, the `Pass` registration
  order, the alias planner, the structural-hash cache key, and the
  barrier emitter belong to the render-graph aggregate (#760,
  `specs/render/render-graph-design.md`). hzb-cull plugs in by
  registering one phase-6 CPU body (`cull/meshlet_cull.cpp`) and one
  phase-7 compute pass (`passes/hzb_build.cpp`); both are routed by
  the graph layer, not authored against it.
- **Pass *bodies* outside its own.** The mesh-shader gbuffer pass
  (#764, `passes/gbuffer.cpp`), the deferred-lighting pass
  (#764), the cluster-light-cull pass (#764, `passes/cluster_cull.cpp`,
  state in `ClusterCullState` §4.1.10) — all separate aggregates. They
  consume hzb-cull's indirect-args output by `IndirectDrawBuffer`
  handle; they never reach into hzb-cull's internals.
- **PSO compilation or residency.** The two compute kernels
  (HZB-build, meshlet-cull) are compiled offline by `shader`'s cook
  per PHILOSOPHY §6 and resident in `PSOCache` (#766, §4.1.7).
  hzb-cull dispatches via `PSOHandle` only.
- **Scene structure.** The per-`View` instance list, the meshlet
  bounds, and the per-mesh metadata (`MaterialHandle`, `SortKey`)
  are produced by the render-frame extract aggregate (#772) and
  delivered through the immutable `RenderFrame` snapshot per
  `SPEC.md` §4.1.1. hzb-cull reads them; it never mutates ECS storage
  (`SPEC.md` §4.2 invariant 6).
- **Depth-prepass body.** Glibre runs no separate depth prepass —
  the gbuffer pass writes depth in one mesh-shader dispatch
  (`SPEC.md` §4.2 invariant 5; collapse decision in §3.2 #2). The
  HZB-build kernel reads that depth attachment as the input to
  next frame's pyramid; it does not author a prepass.
- **Cluster-light cull.** `ClusterCullState` (`SPEC.md` §4.1.10) is
  the **light**-cluster cull; the `cluster_cull.cpp` pass it owns
  builds the `LightCluster` froxel grid for deferred lighting. It is
  a separate aggregate (#764). The two share the word "cluster" and
  nothing else; meshlet/instance cluster cull (geometry, emits draw
  args) lives here.
- **Frustum-only fast paths.** Frustum + normal-cone cull is part of
  the same kernel as HZB occlusion cull (§3.4 below) because the
  three checks share inputs (the per-meshlet bounds + the camera
  state) and have one reason to change ("the visibility test"). A
  separate frustum-cull pass would be SRP-violating duplication.
- **Sort-key generation.** The packed 64-bit `SortKey` is computed
  inside `cull/sort.cpp` (§6.2.1 step 2.3) — adjacent CPU work in
  the same phase 6, but a different aggregate (the extract /
  RenderFrame aggregate, #772). hzb-cull consumes the survivor set
  + sort key column; it does not produce it.

The aggregate is the **single answer** to "did this meshlet survive
visibility this frame, and where do its draw-args land?" — frustum +
normal cone + HZB occlusion + indirect-args compaction in one
kernel, plus the HZB pyramid build that prepares next frame's
input.

## 2. Requirements coverage

This section verifies that every harmonius MVP-scope requirement /
design clause about HZB and meshlet/cluster geometry cull is either
covered by the design below or explicitly refused with rationale.
Inputs (research only, re-derived per PHILOSOPHY §"How harmonius is
used"):

- `harmonius/docs/requirements/rendering/core-rendering.md`
  (R-2.3.2 frustum cull, R-2.3.3 normal-cone cull, R-2.3.4 two-phase
  HZB occlusion cull, R-2.3.6 indirect-draw compaction by material).
- `harmonius/docs/requirements/rendering/scene-rendering-pipeline.md`
  (R-2.10.4a multi-view shared cull, R-2.3.14 mesh-shader dispatch
  with indirect-draw fallback).
- `harmonius/docs/design/rendering/meshlets.md`
  § "Architecture", § "Data Flow" (per-meshlet bounds sphere +
  normal cone — input shape only; the cook is `geometry`'s).
- `harmonius/docs/design/rendering/render-pipeline.md`
  § "Two-Phase HZB", § "Meshlet Cull" (algorithmic shape only).
- `harmonius/docs/design/rendering/rendering-core.md`
  § "Architecture", § "Data Flow" (cull-pass placement in the frame).

| Harmonius clause                                                                                               | Glibre disposition                                                                                                                                                                                                                                                                                              |
|----------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-2.3.2** — Per-meshlet GPU frustum cull against the six camera planes; off-screen meshlets dropped.          | **Covered.** §3.4 step 1: the `meshlet_cull` kernel evaluates the six camera planes against the per-meshlet bounds sphere (`MeshletAsset.bounds` from geometry's cook) before the HZB sample. Failure mask = "outside any plane".                                                                                |
| **R-2.3.3** — Per-meshlet normal-cone (back-face) cull rejecting fully-back-facing meshlets.                    | **Covered.** §3.4 step 2: same kernel evaluates the meshlet `cone(apex, axis, half_angle)` against the camera-to-bounds vector; meshlets whose cone is fully back-facing relative to the view are masked. Same dispatch, same survivor mask.                                                                    |
| **R-2.3.4** — Two-phase HZB occlusion cull: phase-1 against last-frame HZB, phase-2 re-test against this-frame HZB to recover disocclusions. | **Refused, **collapsed to single-phase against last-frame HZB**.** §3.4 step 3 + §3.5 collapse: glibre runs **one** HZB occlusion test per meshlet, against the **previous** frame's HZB. Disocclusion artefacts are absorbed by the temporal AA history (`SPEC.md` §6.2.2 step 1.10). Justification: the second phase costs one full HZB rebuild + one rasterisation re-issue mid-frame, which would split the §6.5 mesh-shader gbuffer into two declared passes (`SPEC.md` §4.2 invariant 5 forbids splitting). The MVP scene cap (1 character + 200 props, S1) makes disocclusion pop visually negligible against the cost. Story #388 names the test that locks the single-phase choice; if pop becomes visible we re-spike, not re-design in place. Listed in §12 below. |
| **R-2.3.6** — Indirect-draw compaction grouped by material (one indirect buffer slice per material).             | **Covered.** §3.4 step 4 + §3.6: the kernel writes per-meshlet draw-args into per-material slots inside one `IndirectDrawBuffer` ring; material grouping is the second sort key (after the phase bucket from `cull/sort.cpp`). The mesh-shader gbuffer dispatch reads one slice per material PSO change.       |
| **R-2.10.4a** — Multi-view shared cull where possible (one cull dispatch can serve multiple views with overlapping frusta). | **Refused for MVP / collapsed.** §3.7: each `View` runs its own cull dispatch. The four MVP view kinds (main, shadow cascade, reflection probe, viewmodel) have non-overlapping frusta in S1; the per-view kernel is ≤0.5 ms GPU on M1 (§9.4 `meshlet-cull` slice = 0.5 ms total budget). Sharing across views adds bookkeeping (per-view membership masks in the survivor set, per-view sort-key fix-up) that costs more than it saves at MVP scale. Re-spike if shadow cascades dominate the slice. Listed in §12. |
| **R-2.3.14** — Mesh-shader dispatch with indirect-draw fallback for hosts without `MeshShaders` capability.       | **Covered indirectly.** §3.8: hzb-cull's output is shape-compatible with both paths. The kernel writes `MTLDrawIndirectArguments` plus a `MTLDrawIndexedIndirectArguments` shadow slot; the fallback gbuffer pass (vertex + amplification stage; `SPEC.md` §6.5, story #387) reads the shadow slot. Glibre selects at graph-build time per `Capability::MeshShaders`; hzb-cull's emit code is identical between paths. |
| Harmonius design — Per-meshlet bounds sphere + normal cone live in the meshlet header buffer (geometry-cooked).  | **Adopted as input shape only.** §3.3: hzb-cull reads `Meshlet.bounds_sphere` (16 B) + `Meshlet.normal_cone(apex, axis, half_angle)` (16 B) from the geometry-cooked meshlet header; no glibre-side production. The 64-byte meshlet header layout (harmonius `meshlets.md` § "GPU Buffer Layout") is owned by `geometry`'s spec, not this aggregate. |
| Harmonius design — Cull pass writes the GPU "indirect-draw buffer" + a survivor count atomic.                    | **Covered.** §3.4 step 4: the kernel uses one `atomic_uint` counter per material slot to compact draw-args into the `IndirectDrawBuffer` ring; overflow trips `MeshletCullDispatchFailed → IndirectArgsOverflow` payload (§10 below).                                                                            |
| Harmonius design — HZB built from depth via min-reduce mip chain.                                                | **Covered.** §3.5: `passes/hzb_build.cpp` runs a min-reduce (reverse-Z so `min` = farthest occluder; `SPEC.md` §4.1.9 invariant 3) compute kernel mip-by-mip. Pyramid extent rounded to power-of-two with conservative `ceil_log2` mip count.                                                                   |
| Harmonius design — HZB sized to render extent at init; rebuilt on extent change.                                 | **Covered.** §3.6 + §8: `HZB::ensure(view, desc)` is the only construction point; if `HZBDesc.{width, height, mips}` differs from the live storage, the aggregate destroys the previous allocation and reallocates from `resources/persistent.cpp` (#772). `HZB::invalidate(view)` forces the next frame's `ensure` to rebuild. Hot-reload (§8) calls `invalidate` on plug-in swap because plug-in `.text` may have changed the kernel's mip-level convention. |
| Harmonius design — Per-cascade HZBs for shadow-map cull.                                                         | **Covered.** §3.6: each `View` has its own `HZB` (§4.1.9 invariant 2). Shadow-cascade `View`s instantiate one `HZB` per cascade; reflection-probe and viewmodel `View`s likewise. No cross-view aliasing. The `HZB` table is keyed on `ViewHandle`.                                                              |
| Harmonius design — HZB GPU resource format `R32Float` mip0, `R32Float` rest.                                     | **Covered.** §3.5: pyramid mips are `MTLPixelFormatR32Float` (single-channel float, reverse-Z compatible). Storage class `MTLStorageModePrivate`. Allocator routes through render's persistent heap (`SPEC.md` §9.5 "persistent textures + buffers" row, sub-row "HZB pyramids"). |
| Harmonius design — Hi-Z occlusion test samples N×N footprint of the meshlet's screen-space AABB at the appropriate mip. | **Covered.** §3.4 step 3.2: footprint = bounds sphere projected to screen-space → `(x_min, y_min, x_max, y_max)`; mip selection = `ceil(log2(max(width_px, height_px)))` so the conservative test reads at most a 2×2 footprint per meshlet at the chosen mip. One `min`-fetch + one reverse-Z compare. |
| Harmonius design — Phase-1 HZB cull happens in a compute pre-pass before the gbuffer.                            | **Refused / re-routed.** Glibre runs the cull on the **CPU/GPU seam in phase 6** (§6.2.1 step 2.1) so the resulting `IndirectDrawBuffer` is part of the immutable `RenderFrame` consumed by phase 7. The cull *kernel* is GPU compute (Metal 4 compute), but its dispatch lives in phase 6's `cull-extract` because the survivor set is an *input* to the graph-build step (it determines the gbuffer pass's `IndirectDrawBuffer` binding). Phase 6 is the only place ECS storage is read; the cull dispatch is the last GPU work that may consume ECS-derived inputs (§4.2 invariant 6). The harmonius "compute pre-pass before gbuffer" shape is a phase-7 affair; we collapse it into phase 6 because the alternative would force an extra graph-builder round-trip per `View`. Story #388 covers this. |
| Harmonius design — Two-phase HZB rebuild between phases.                                                          | **Refused, see R-2.3.4 row.** One HZB build per frame, after gbuffer.                                                                                                                                                                                                                                          |
| Harmonius design — HZB visualised in the diagnostic overlay.                                                     | **Refused at this aggregate; routed to `DiagnosticOverlay` (#774).** hzb-cull exposes `HZB::handle(view)` (ABI add to the §5 stub; see §4) so the overlay can sample the pyramid; the overlay rendering is not this aggregate's concern.                                                              |
| Harmonius design — Per-meshlet visibility persisted between frames (Hi-Z hierarchy with feedback).               | **Refused for MVP.** No persistent per-meshlet visibility cache; each frame's cull is fresh. The 0.5 ms GPU slice (§9.4 `meshlet-cull`) absorbs ~3.2k meshlets (S1) without amortisation. A feedback cache would add a frame of latency to every cull decision and complicate hot-reload (the cache would need migration). Listed in §12. |

Net result: every R-2.3.* and R-2.10.4a requirement and every
HZB-cull-relevant design clause is either implemented as specified
below or explicitly refused with rationale. The two-phase HZB and
multi-view shared cull collapses are the load-bearing refusals; both
are `[OPEN]` in §12 with named re-spike triggers.

## 3. Detailed model

### 3.1 The aggregate cluster

```
HZB                   (per (View, FrameCounter) — persistent storage)
├── pyramids_       : eastl::vector<HZBPyramid>     (one per ViewHandle)
│                     ├── mip0..mipN textures      (R32Float, private)
│                     ├── extent                   (width, height)
│                     ├── mip_count                (ceil_log2(max(w,h)))
│                     └── ring_slot                (triple-buffer index)
├── allocator_      : PersistentResourceAllocator& (§4.1.4 borrow)
└── pso_handles_    : { hzb_build_kernel, meshlet_cull_kernel }
                                                   (PSOCache borrows)

MeshletCullDispatch  (per (View, FrameCounter), phase-6 transient)
├── input_views_    : eastl::span<const Meshlet>   (RenderFrame borrow)
├── input_camera_   : CameraView                   (RenderFrame borrow)
├── input_hzb_      : HZBHandle                    (last frame's pyramid)
├── output_buffers_ : IndirectDrawBufferSet        (per-material slots)
├── output_count_   : eastl::span<atomic_uint>     (one per slot)
└── kernel_         : ComputeKernelHandle          (PSOCache borrow)

HZBBuildPass         (per (View, FrameCounter), phase-7 transient)
├── input_depth_    : VirtualResourceHandle        (gbuffer depth borrow)
├── output_pyramid_ : HZBHandle                    (this view's pyramid)
└── kernel_         : ComputeKernelHandle          (PSOCache borrow)
```

`HZB` is the *persistent* aggregate (§4.1.9 invariant 2) — it owns
the per-view pyramid storage that survives across frames in the
triple-buffer ring. The two transient companions
(`MeshletCullDispatch`, `HZBBuildPass`) are *per-frame value
objects* that hold the current frame's input/output bindings; they
are constructed by the phase-6 / phase-7 entry points respectively
and destroyed at phase exit. The split is the §4.1.9 "persistent
across frames, transient across views" invariant lifted into source
shape: the long-lived data structure has one lifetime, the per-frame
dispatch parameters have another, and the kernel handle lives in
`PSOCache` for the process lifetime.

### 3.2 Per-`View` instantiation

`HZB::ensure(view, desc)` is idempotent and creates pyramid storage
on first call per `(view, desc)` tuple. Sequence on first call for
a new `View`:

1. Compute `mip_count = ceil_log2(max(desc.width, desc.height))`.
   Clamp at 14 (the `MTLTexture` mip-level cap on Apple Silicon
   relevant to MVP extents — 16384 px would be 14 mips).
2. Allocate three `MTLTexture` mip chains from the persistent
   allocator (`resources/persistent.cpp`) — three because the
   triple-buffered ring serves the read/write race noted in
   `SPEC.md` §4.1.9 ("triple-buffered so frame N's HZB-build does
   not race frame N+1's HZB-read"). All three mips are
   `MTLPixelFormatR32Float`, `MTLStorageModePrivate`,
   `MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite`.
3. Bind the mip chain to render's residency set
   (`metal/residency.cpp`). The pyramid is a hot resource and stays
   resident.
4. Insert the entry into `pyramids_` keyed on `ViewHandle`, return
   `HZBHandle`.

Subsequent `ensure(view, desc)` calls with matching `desc` return
the cached `HZBHandle` (`O(1)`). A mismatched `desc` triggers
`invalidate(view)` + reallocation; the new pyramid's first frame
has no prior-frame data, so the cull kernel reads the "no occlusion
data" sentinel and the survivor set degenerates to "frustum + cone
cull only" for one frame. This is acceptable on extent change
(window resize, dynamic-resolution flip — `SPEC.md` §3.2 collapse
#5/#10): one frame of slightly conservative culling is invisible.

`HZB::invalidate(view)` clears the pyramid's data without freeing
storage; the next frame's cull kernel reads the sentinel and the
HZB-build pass overwrites the buffer normally.

Multi-view fan-out instantiates one `HZBHandle` per `View`. Per
SPEC §4.1.9 invariant 2 ("transient across views") the pyramids
never alias even when extents match — each cascade / probe / view
owns its storage. Total residency cost is the sum across `View`s
(MVP cap: main 1080p + 4 shadow cascades 1024² + 1 probe 256² + 1
viewmodel 1080p ≈ 7 MiB persistent, well inside the §9.5
"persistent textures + buffers" row).

### 3.3 Inputs from `RenderFrame`

Phase 6 has the immutable `RenderFrame` in hand when the
`MeshletCullDispatch` constructs. The inputs the cull kernel reads
(all by GPU buffer reference; no CPU copy):

| Input                                | Shape                                                  | Source                                                                                       |
|--------------------------------------|--------------------------------------------------------|----------------------------------------------------------------------------------------------|
| `meshlets[]`                         | `MeshletHeader[]` (64 B per meshlet, geometry-cooked)  | `RenderFrame::meshlets(view)` — the per-`View` survivor set after frustum-only meshlet selection inside the geometry plug-in's phase-6 systems (§9.2 row "Geometry meshlet-selection systems"). hzb-cull adds frustum-plane + cone + HZB on top. |
| `instance_transforms[]`              | `float4x4[]` packed                                    | `RenderFrame::instances(view)` — written by the extract aggregate (#772). One transform per renderable.                                                                                       |
| `meshlet_to_instance[]`              | `uint32[]`                                             | Indirection so one mesh asset can be drawn at multiple instances; written by extract.                                                                                          |
| `material_index[]`                   | `uint16[]` (per-meshlet)                               | Material slot for the survivor → one of the `IndirectDrawBuffer` slots. Mirrors the §6.5 step-3 "material-grouped compaction".                                                  |
| `camera_view`                        | `{ frustum_planes[6], view, proj, jitter }`            | `RenderFrame::view(handle)`.                                                                                                                                                     |
| `prev_hzb_handle`                    | `HZBHandle`                                            | Last frame's `HZB::handle(view)`. On the first frame after `invalidate`, this is the sentinel "no occlusion data" handle (a 1×1 pyramid of `+inf` reverse-Z; equivalent to "everything is potentially visible"). |

The kernel does **not** read ECS storage. Every input is a GPU
buffer or texture handle resolved at `RenderFrame` build time.

### 3.4 The `meshlet_cull` kernel

One Metal 4 compute kernel — `shaders/cull/meshlet_cull.slang`,
compiled by `shader`'s cook into the `PSOCache` (#766). One thread
per meshlet; threadgroup size 64 (one wavefront on Apple Silicon).
Per-thread sequence:

#### Step 1 — Frustum cull

Read `meshlet.bounds_sphere = {center.xyz, radius}` (16 B). For each
of the six frustum planes from `camera_view.frustum_planes`, compute
`signed_distance = dot(plane.xyz, transform * center) - plane.w`.
If `signed_distance < -radius` for any plane, the meshlet is
outside that plane → cull.

The `transform` is `instance_transforms[meshlet_to_instance[i]]`.
Instance transforms are `float4x4` so the bounds-sphere center is
read once and projected once per kernel invocation (the radius is
scale-correct only for uniform-scale instances; non-uniform scale
falls back to the conservative AABB stored alongside in
`meshlet.bounds_aabb`, also 32 B in the meshlet header — geometry's
cook supplies both shapes).

#### Step 2 — Normal-cone cull

Read `meshlet.normal_cone = {apex.xyz, axis.xyz, cos_half_angle}`
(16 B). `axis` is **outward-facing** (points away from the meshlet's
surface toward the outside of the cone, matching the geometry
`geometry`'s cook convention for the meshlet header layout). The
cone is back-facing from camera position `cam.xyz` when
`dot(axis, normalize(cam - apex)) < -cos_half_angle`. If fully
back-facing (the strict `<` inequality), cull.

Cone cull is skipped for two-sided materials. The "two-sided" bit
lives in `material_index[i]` high bit (the material slot index is
15 bits, leaving one bit for the flag — agreed with the material
plug-in's slot layout, ticket #780 follow-up; current kernel
masks the flag and skips cone cull when set). For MVP materials
(opaque + alpha-test + standard transparent), only foliage flags
two-sided.

#### Step 3 — HZB occlusion cull

If steps 1+2 passed, project the bounds sphere to screen-space:

1. Transform the bounds sphere to clip-space:
   `clip = proj * view * transform * center`.
2. Compute `ndc = clip.xyz / clip.w`.
3. Compute screen AABB: project the four extreme points
   `center ± radius * {right, up}` to clip-space and reduce to a
   `(x_min, y_min, x_max, y_max, z_max_ndc)` in NDC. In reverse-Z,
   **`z_max_ndc` is the NDC-Z of the sphere's closest face**
   (closest-to-camera = largest NDC-Z value in reverse-Z). This is
   the conservative operand: if even the front face is occluded, the
   whole meshlet is occluded.
4. Compute `extent_px = max((x_max - x_min) * width, (y_max - y_min) * height)`.
5. Compute `mip = clamp(ceil(log2(extent_px)), 0, mip_count - 1)`
   so that the screen AABB samples a 2×2 footprint at the chosen
   mip.
6. Sample the prev-frame HZB at the chosen mip — exactly four
   `texture.read(uint2)` calls at the four corners of the AABB.
   The pyramid mip stores `min` of the four mip-`mip-1` parents,
   and the cull test is `min(four_samples) > z_max_ndc` where `>`
   is reverse-Z's "behind-the-occluder" comparison (`SPEC.md` §4.1.9
   invariant 3): the occluder's HZB depth (closest occluder surface,
   larger NDC-Z) exceeds the meshlet's closest face. If the four
   samples agree the meshlet's closest face is behind the occluder,
   cull the entire meshlet.

If the meshlet's projected sphere straddles the near plane
(`clip.w` <= near-plane epsilon), skip the occlusion test — the
sphere is partially behind the eye and the conservative answer is
"keep". Frustum cull (step 1) caught everything fully behind the
near plane already.

#### Step 4 — Compaction emit

Survivors from steps 1-3 emit a `MTLDrawIndirectArguments` record
into the `IndirectDrawBuffer` slot keyed by
`material_slot = material_index[i] & 0x7FFF`:

```hlsl
const uint mat   = material_index[i] & 0x7FFFu;
const uint slot  = atomic_fetch_add_explicit(
                     &output_count[mat], 1u,
                     memory_order_relaxed);
if (slot >= output_capacity[mat]) {
    atomic_fetch_or_explicit(&overflow_flag, 1u, memory_order_relaxed);
    return;
}
output_buffers[mat][slot] = MTLDrawIndirectArguments {
    .vertex_count   = meshlet.triangle_count * 3,
    .instance_count = 1,
    .vertex_start   = meshlet.vertex_start,
    .instance_start = meshlet_to_instance[i],
};
```

The `overflow_flag` is a single `atomic_uint` per dispatch, read
back at the next CPU sync point and translated to
`MeshletCullDispatchFailed` with the
`IndirectArgsOverflow` payload (§10 below).

The compaction is per-material, per-meshlet. Each surviving meshlet
emits one `MTLDrawIndirectArguments` record with `instance_count = 1`
because the survivor count atomic is per-meshlet. Instance fan-out
(collapsing multiple instances of the same mesh into a single record
with `instance_count = N`) is a §12 follow-up.

### 3.5 The `hzb_build` kernel

One Metal 4 compute kernel — `shaders/cull/hzb_build.slang`,
compiled by `shader`'s cook. Mip-chain build via repeated min-reduce.

#### Mip 0 — copy-with-min from gbuffer depth

The gbuffer pass writes a `D32Float` depth attachment. Mip 0 of
the HZB is `R32Float` and is filled by sampling the depth and
writing the value verbatim. (Conservative: if the gbuffer
multisample-resolved or wrote MSAA, mip 0 takes the per-pixel
`min` across samples; reverse-Z `min` = farthest occluder.)

The MVP gbuffer is single-sample (`SPEC.md` §6.5 step 3, no MSAA),
so the mip-0 build is a straight copy.

#### Mips 1..N — 2×2 min reduce

For each subsequent mip `k`:

```
threadgroup size = 8x8 = 64
each thread reads four texels at (2x, 2y), (2x+1, 2y), (2x, 2y+1), (2x+1, 2y+1)
of mip k-1 and writes min(four) to (x, y) of mip k.
```

When the previous mip's extent is odd, the last row/column gets
`+inf` (reverse-Z's "behind everything") padding by clamping the
read coords; the `min` still produces the correct conservative
value.

The kernel dispatch count per mip is
`ceil(extent_k / 8) * ceil(extent_k / 8)`. Total mips ≤ 14;
total dispatches ≤ 14 (one per mip).

#### Reverse-Z convention

Per `SPEC.md` §4.1.9 invariant 3, all HZB ops use the same reverse-Z
convention as the gbuffer. The convention lives in one header:
`include/glibre/render/internal/reverse_z.hpp`, exposing
`ReverseZ::kFar = 0.0f` and `ReverseZ::kNear = 1.0f`. The HZB stores
**raw** depth values (not transformed); the cull kernel's
"behind-the-occluder" comparison is `z_max_ndc > hzb_min` in
reverse-Z (where `z_max_ndc` is the sphere's closest face — largest
NDC-Z — and `hzb_min` is the HZB sample's `min` of four texels at
the chosen mip). Both kernels include the same header; no per-pass
override (`SPEC.md` §4.1.9 invariant 3 explicitly).

### 3.6 Resource model

The aggregate manages two distinct GPU resource shapes:

#### HZB pyramid (persistent)

- **Lifetime:** plugin-instance lifetime (`SPEC.md` §4.1.9
  invariant: persistent across frames).
- **Triple-buffered:** three rotating mip chains per view;
  frame N writes ring slot N % 3, reads ring slot (N - 1) % 3.
  Slot N - 2 is the buffer the GPU may still be reading (frame
  N - 1's HZB-build is in flight from the previous submit).
- **Allocator:** `resources/persistent.cpp` (`SPEC.md` §4.1.4
  invariant), `ContextTag::render`, sub-row "HZB pyramids" of
  the `SPEC.md` §9.5 "persistent textures + buffers" 128 MiB row.
- **Storage:** `MTLPixelFormatR32Float`, `MTLStorageModePrivate`.
- **Access:** `ShaderRead | ShaderWrite`. Build kernel writes mip
  k from mip k-1; cull kernel reads mip k. No `RenderTarget`
  usage — the pyramid is compute-only.

#### IndirectDrawBuffer ring (per-frame transient)

- **Lifetime:** one frame; declared as a `Transient`
  `VirtualResource` to the graph builder so the alias planner
  (`SPEC.md` §4.1.4) may overlap its storage with other
  transients.
- **Shape:** one `MTLDrawIndirectArguments` (16 B per record)
  array per material slot, plus one `atomic_uint` survivor count
  per slot. Capacity per slot is fixed at init from
  `RenderSettings.per_view_draw_budget` divided across material
  slots; default at MVP is 4096 records per slot, 256 slots →
  ≤16 MiB per `View` per frame.
- **Allocator:** `resources/transient_pool.cpp` (`SPEC.md` §4.1.4)
  via the graph builder's `declare_transient` (the cull aggregate
  declares the `VirtualResource` from its phase-6 dispatch
  registration; the planner maps it to a transient slot).
- **Sentinel for empty slot:** `survivor_count = 0` ⇒ the
  gbuffer pass's indirect dispatch for that material draws zero
  primitives. No CPU readback needed.

#### Sentinel "no occlusion data" pyramid

- **Lifetime:** singleton, plugin-instance lifetime.
- **Shape:** 1×1 `R32Float` texture filled with the reverse-Z
  "behind everything" value (`+inf` rasterised as
  `0x7F800000`).
- **Used by:** the cull kernel on the first frame after
  `HZB::invalidate(view)` — sampling this sentinel returns the
  most-distant value, so the occlusion test never rejects, and
  the cull degenerates to frustum + cone only.

### 3.7 Per-`View` semantics

Each `View` has independent state. Four MVP view kinds:

| `View` kind            | HZB extent             | Cull dispatch                              | Indirect-args output                                |
|------------------------|------------------------|--------------------------------------------|-----------------------------------------------------|
| **Main**               | 1920×1080 (S1)         | One per main view per frame.               | Consumed by `passes/gbuffer.cpp` (story #386).      |
| **Shadow cascade** ×4  | 1024×1024 per cascade  | One per cascade per frame; same kernel.    | Consumed by the shadow-cascade gbuffer pass (post-MVP shadow surface; today goes through deferred-lighting's shadow-tier path). |
| **Reflection probe**   | 256×256                | One per probe per probe-update frame.      | Consumed by the reflection-probe gbuffer pass (post-MVP). |
| **Viewmodel**          | 1920×1080 (matches main, separate `HZB`) | One per frame. | Consumed by the viewmodel gbuffer pass (`SPEC.md` §3.1 first-person row). |

The four view kinds share the same cull and HZB-build kernels; they
differ only in the per-`View` parameters (frustum planes, extent,
HZB pyramid). MVP cull-budget (§9 below) is sized for one main +
four cascades + one probe + one viewmodel = seven dispatches per
frame at the GPU ceiling.

Multi-view shared cull is a §12 open question. In MVP each `View`
runs independently.

### 3.8 Mesh-shader fallback shape compatibility

When `Capability::MeshShaders` is absent (story #387), the gbuffer
pass falls back to vertex + amplification stage. The cull aggregate
emits **shape-compatible** indirect args:

- **Mesh-shader path:** `output_buffers[mat]` is read directly by
  `MTLDispatchMeshThreadgroupsIndirect`; each meshlet record is
  one threadgroup dispatch.
- **Fallback path:** `output_buffers[mat]` is read by
  `drawIndirect:indirectBuffer:`; each meshlet record is treated
  as a draw call via `MTLDrawIndexedIndirectArguments` (the
  capacity is tagged at init for the fallback shape, 20 B per
  record vs 16 B; the kernel writes the indexed shape, the
  mesh-shader path ignores the trailing index-count slot).

The capacity reserved at init is the union (20 B per record across
both paths) so the kernel emit code is identical and the graph
builder picks the correct read view at compile time
(`Capability::MeshShaders` predicate on the gbuffer pass).

### 3.9 Aggregate composition

```
HZB         (persistent, per (Plugin, View))
├── pyramids_[ViewHandle] : HZBPyramid (R32Float mip chain × 3 ring)
├── allocator_            : PersistentResourceAllocator&
└── pso_handles_          : { build_kernel, cull_kernel } (PSOCache)

MeshletCullDispatch (per (View, FrameCounter), phase-6 transient)
├── input_meshlets_       : eastl::span<const MeshletHeader>
├── input_camera_         : CameraView
├── input_prev_hzb_       : HZBHandle (or sentinel)
├── output_indirect_      : IndirectDrawBufferSet (transient VR)
└── kernel_dispatch_      : ComputeDispatchDesc

HZBBuildPass        (per (View, FrameCounter), phase-7 transient)
├── input_depth_          : VirtualResourceHandle (gbuffer depth)
├── output_pyramid_       : HZBHandle (this view's slot)
└── kernel_dispatch_      : ComputeDispatchDesc[mip_count]
```

The aggregate's interface to the rest of the render plug-in:

- **Inputs:** `RenderFrame&` (read), `MetalDevice&` (queue + caps),
  `PSOHandle`s vended by `PSOCache`, `ViewHandle`s for per-view
  storage lookup. The cull dispatch additionally reads the prev
  frame's `HZBHandle`.
- **Outputs:** `IndirectDrawBuffer` virtual resource (per-frame
  transient) consumed by the gbuffer pass; `HZB` storage write
  consumed by *next* frame's cull dispatch.

Nothing else crosses the boundary. The `HZBHandle` is the single
typed seam exposed in §5; `HZB::ensure` / `HZB::invalidate` are the
only mutation entry points; `HZB::handle(ViewHandle)` (ABI add; §4)
is the only read entry point available outside render-internal pass
code. The cull and build kernels are accessible only to the render
plug-in's pass-body code; external plug-ins do not register cull or
HZB-build passes.

## 4. Public surface

The public surface is frozen in `specs/render/SPEC.md` §5. This
design adds **one new method** to the §5 `HZB` stub: a read-only
`handle(ViewHandle)` getter required so that `DiagnosticOverlay`
(#774) can sample the HZB pyramid without reaching into
render-internal storage. This is an **ABI add** (noted in §10.1's
audit column per the §5 rule: new methods require an amendment to
the §5 stub and must not remove or reorder existing declarations).
For convenience, the load-bearing declarations (§5 verbatim plus
the ABI-add getter):

```cpp
namespace glibre::render {

struct HZBDesc {
    std::uint32_t width  = 0u;
    std::uint32_t height = 0u;
    std::uint32_t mips   = 0u;  // 0 ⇒ ceil_log2(max(width, height))
};

class HZB {
public:
    [[nodiscard]] Result<HZBHandle> ensure(ViewHandle, HZBDesc) noexcept;
    void                            invalidate(ViewHandle) noexcept;

    // ABI add (this design) — read-only handle accessor for
    // DiagnosticOverlay (#774) and other render-internal readers.
    // Returns an empty optional when the view has no live pyramid
    // (not yet ensure'd, or after invalidate before next ensure).
    [[nodiscard]] eastl::optional<HZBHandle>
                                    handle(ViewHandle) const noexcept;

protected:
    HZB()  noexcept = default;
    ~HZB() = default;
    HZB(const HZB&)            = delete;
    HZB& operator=(const HZB&) = delete;
};

// HZBHandle is a Handle<tags::hzb> generational handle; declared
// in §5 of SPEC.md alongside the other render handle aliases.

} // namespace glibre::render
```

Surface invariants this design imposes on top of the §5 stub:

1. **No exceptions cross the boundary.** `ensure` is `noexcept` and
   returns `glibre::Result<HZBHandle>` per `error-model.md`.
   `invalidate` cannot fail (an unknown `ViewHandle` is a no-op);
   it is `noexcept` and returns `void`.
2. **`HZBDesc::mips == 0` selects auto.** A caller passing zero
   means "use `ceil_log2(max(width, height))`"; any non-zero value
   is honoured up to the 14-mip cap and rejected with
   `HeapOutOfMemory` if it exceeds the cap or is not in
   `[1, ceil_log2(max(width,height))]`.
3. **`HZBHandle` is generational.** Per the `SPEC.md` §5 invariant
   ("Resource handles are 64-bit generational `Handle<Tag>` values
   with no payload pointers; this avoids ABI fixup on hot-reload").
   `invalidate(view)` does **not** bump the generation — it clears
   the data in-place, keeping handles valid. A *destroy*
   (e.g. `View` removal) is a separate render-internal API
   reachable through the `Resource` aggregate (#772) and *does*
   bump the generation.
4. **`HZB` is non-copyable, non-movable.** Per the §5 deleted
   ctor/op=. The class is a render-internal singleton accessed
   through the `MetalDevice` plug-in surface; pass-bodies obtain
   the live reference through their `Bindings` argument.
5. **Multi-thread access is undefined.** `ensure` and `invalidate`
   run on the builder thread; the cull and build kernels are
   GPU-side and do not call `HZB` methods. Pass-body code outside
   the cull aggregate must not call `ensure` or `invalidate` —
   doing so would race the per-frame ring rotation (§3.6 ring slot
   selection).
6. **`HZBHandle` is consumed read-only.** The graph builder's
   `declare_persistent` registers the HZB pyramid as a persistent
   `VirtualResource`; pass declarations carry `(handle, AccessKind::Read)`
   for the cull kernel and `(handle, AccessKind::Write)` for the
   build kernel. The compiler enforces no two passes claim a write
   on the same `HZBHandle` per frame (`SPEC.md` §4.1.4 invariant).

Error arms emitted directly by this aggregate's public surface and
its internal kernels (per `SPEC.md` §10.1):

- `render::Error::HeapOutOfMemory` — `HZB::ensure` cannot
  allocate the persistent mip chain (§9.5 row "persistent textures
  + buffers" exhausted).
- `render::Error::ResourceResidencyExceeded` — same, but the
  failure was budgetary rather than driver-side.
- `render::Error::MeshletCullDispatchFailed` — cull kernel
  encounters a Metal 4 `MTLCommandEncoderError` *or* the
  `IndirectArgsOverflow` flag was set by the kernel
  (§3.4 step 4).
- `render::Error::MeshShaderCapabilityMissing` — only at init /
  hot-reload register if the cull kernel cannot run on the active
  device (the kernel has no fallback; the gbuffer pass has the
  fallback per §3.8). Routed up via `Capability` predicate
  evaluation in the graph builder.

The aggregate does **not** emit `RenderGraphCycle`,
`PassUnsupportedConfig`, `BarrierConflict` — those are graph-layer
arms (#760). It does **not** emit `PsoCompileFailed` — that is the
PSO cache's arm (#766). It does **not** emit `DeviceLost`,
`QueueSubmitFailed`, `FenceTimeout` — those are device + queue
arms (#762).

## 5. Hot/cold path split

### 5.1 Hot path — phase-6 cull dispatch + phase-7 HZB build (per frame, per `View`)

Both kernels are **hot per frame, per `View`**. They run inside
phases 6 and 7 respectively and contribute directly to the §9.4
GPU `meshlet-cull` slice (0.5 ms ceiling, includes the folded HZB
build) and the §9.2 phase-6 row "frustum + HZB cull" (0.20 ms CPU
ceiling — that 0.20 ms is the CPU-side dispatch + parameter pack
cost; the GPU work itself is in §9.4).

#### 5.1.1 Phase-6 CPU side

| Step                                            | Cap        | Cost model                                                                                                |
|-------------------------------------------------|------------|-----------------------------------------------------------------------------------------------------------|
| `cull/meshlet_cull.cpp` — collect inputs        | 0.05 ms    | Read `RenderFrame::meshlets(view)` span, `instance_transforms`, `material_index`. Pointer + size only.    |
| Parameter-pack the kernel arg buffer            | 0.05 ms    | One `eastl::array<KernelArg, N>` write through the per-frame ring; ≤8 args (camera, hzb, depth, output).  |
| Dispatch the cull kernel                        | 0.05 ms    | One `MTL::ComputeCommandEncoder::dispatchThreadgroups` call with thread count `ceil(meshlet_count/64)`.    |
| Insert read-after-write fence on `HZB`          | 0.02 ms    | One `MTLSharedEvent` wait against the prev frame's HZB-build signal (§6 below).                            |
| Driver-thread reserve                           | 0.03 ms    | Absorbs first-frame warm + capability re-probe on hot-reload.                                              |
| **Phase-6 CPU subtotal (this aggregate)**       | **0.20 ms**| Matches `SPEC.md` §9.2 row "frustum + HZB cull".                                                            |

The CPU-side work is **driver-thread only**; no per-pass workers.
The kernel itself runs on the GPU compute queue.

#### 5.1.2 Phase-7 CPU side

| Step                                            | Cap        | Cost model                                                                                                |
|-------------------------------------------------|------------|-----------------------------------------------------------------------------------------------------------|
| `passes/hzb_build.cpp::register`                | 0.01 ms    | One `add_compute_pass` call with read=gbuffer-depth, write=hzb-pyramid.                                    |
| Per-mip dispatch param-pack                     | 0.02 ms    | ≤14 mips × `KernelArg` write. Per-frame ring; resolved in one `Bindings` blob.                              |
| Per-mip `dispatchThreadgroups` calls            | 0.04 ms    | ≤14 calls; each is a Metal 4 compute encoder call with constant args.                                      |
| Insert write fence (`HZB`-build → next-frame cull) | 0.01 ms | One `MTLSharedEvent` signal at end of HZB-build pass.                                                       |
| **Phase-7 CPU subtotal (this aggregate)**       | **0.08 ms**| Inside the §9.3 phase-7 row "Per-pass `execute()` recording" 0.50 ms allocation.                            |

#### 5.1.3 GPU side (folded into §9.4 `meshlet-cull` slice)

| Kernel                                          | GPU ms     | Cost model                                                                                                |
|-------------------------------------------------|------------|-----------------------------------------------------------------------------------------------------------|
| Meshlet cull (S1: ~3.2k meshlets)               | ~0.15 ms   | One thread per meshlet; bounds-sphere transform + 6 plane tests + cone test + HZB sample at chosen mip.    |
| HZB build (S1: 1080p mip chain → 14 mips)       | ~0.20 ms   | Mip 0 copy (~0.05 ms) + 13 min-reduce dispatches (~0.15 ms cumulative).                                    |
| Cluster-light cull on compute queue overlap     | (not ours) | Folded into §9.4 `cluster_cull` line; runs in parallel.                                                    |
| **Aggregate GPU subtotal**                      | **0.35 ms**| Inside the §9.4 `meshlet-cull` slice 0.5 ms cap; remaining 0.15 ms covers the cluster-light-cull overlap.  |

The 0.5 ms `meshlet-cull` row in `SPEC.md` §9.4 covers both the
cluster-light cull (`ClusterCullState`, #4.1.10) and the
meshlet/instance cluster cull (this aggregate). They share the
budget because they run in parallel on the compute queue and one
is a strict superset of the other in wall-clock dominance (the
meshlet cull is the larger slice by ~2×). Both fit inside 0.5 ms
on M1 baseline.

### 5.2 Cold path — `HZB::ensure` (per `View`, per resize)

`HZB::ensure(view, desc)` is **cold per resize**, not per frame.
Steady-state calls hit the cached `HZBHandle` in `O(1)`. The
allocation path (§3.2) runs only on first creation, dynamic-resolution
flip, or hot-reload re-probe — all rare events.

### 5.3 Why the split matters

GPU-side drift trips the §9.4 `meshlet-cull` 0.5 ms cap; CPU-side
drift trips the §9.2 phase-6 0.20 ms slice. The split is
deliberately not data-driven: the meshlet-cull kernel has one
threadgroup size (64), one survivor-count atomic per material slot,
one HZB sample per meshlet. The HZB-build kernel has one mip-chain
walk pattern. Per `SPEC.md` §4.1.10 invariant 2 (lifted to this
aggregate by parallel reasoning), shaders never branch on tier in
the hot path; tier-driven differences (e.g. lower shadow-cascade
resolution) flow through the per-`View` `HZBDesc` chosen at
`ensure` time, not through kernel switches.

## 6. Concurrency

### 6.1 Builder thread (one)

`HZB::ensure` and `HZB::invalidate` run on the **render builder
thread** (§6.3 of `SPEC.md`). The pyramid table is single-threaded;
no locks. Multi-view fan-out is sequential on the builder thread —
six `ensure` calls for the seven MVP `View`s + viewmodel reuse.

### 6.2 GPU side — single-shot dispatch into render-graph

Both kernels are **GPU-side parallel** (per their threadgroup
counts) but **CPU-side single-shot**: the phase-6 cull dispatch
records one `dispatchThreadgroups` and returns; the phase-7
HZB-build pass records ≤14 dispatches sequentially in one compute
encoder and returns.

The cull kernel runs on `Queue::Compute` per the graph builder's
queue assignment (§6.2.2 step 1.5 names HZB-build on
`Queue::Compute`; the cull dispatch is inside the phase-6
`cull-extract` step which runs on the driver thread but submits to
`Queue::Compute` via the same queue handle the graph layer
publishes for compute work). Per-pass workers do not interact —
the cull dispatch is a phase-6 driver-thread submit, completing
before phase 7 begins per `frame-phases.md` ordering.

The HZB-build pass runs on `Queue::Compute` inside phase 7 per
the graph builder's standard topology (`SPEC.md` §6.2.2 step 1.5).
The compute-queue worker (one of three per-pass workers, §6.3 of
`SPEC.md`) records the ≤14 mip dispatches sequentially.

### 6.3 Cross-frame fences

The `HZB` is the single resource crossing frame boundaries inside
this aggregate. Two fences:

1. **Frame N's cull-dispatch waits on frame N-1's HZB-build
   completion** (`MTLSharedEvent` signalled at end of frame N-1's
   `passes/hzb_build.cpp`; cull dispatch on frame N's compute queue
   does `encodeWaitForEvent`). This serialises across frames so the
   triple-buffer ring's read slot is GPU-coherent.
2. **Frame N's HZB-build signals at end** for frame N+1's cull
   dispatch.

The events are allocated from `metal/fence.cpp`'s per-frame pool
(`SPEC.md` §3.7 reference). The aggregate does not own the event
pool.

### 6.4 Determinism

Determinism requirements (PHILOSOPHY §7) for this aggregate:

1. **Cull is order-independent.** The kernel is one thread per
   meshlet; the only cross-thread interaction is the per-material
   atomic survivor-count counter. Two runs with byte-equal inputs
   produce byte-equal `output_buffers` *contents*, but the
   `output_buffers` *order within a slot* depends on
   atomic-fetch-add ordering across threadgroups (Metal 4 makes no
   ordering guarantees there). The downstream gbuffer pass treats
   the slot as an unordered batch — material grouping is the only
   ordering constraint. The per-meshlet survivor *set* (the bitset
   of which meshlets survived) is deterministic; the *order* in
   the output buffer is not. Replay determinism therefore covers
   "did this meshlet survive?" but not "at what slot index did it
   land?".
2. **HZB-build is fully deterministic.** Mip-k-from-mip-(k-1)
   `min` reduce has no atomic interaction; same input → byte-equal
   output mip texture. The triple-buffer ring slot index is a
   pure function of `frame_counter % 3`.
3. **No clock or random reads.** Neither kernel reads wall clock
   or PRNG; every input is in the bound argument buffer.

Replay determinism is a property of the survivor *set* (sorted by
the downstream `cull/sort.cpp` in §6.2.1 step 2.3, which is
deterministic — `SPEC.md` §3.2 collapse #6 + the radix's
deterministic key ordering).

## 7. Persistence + ABI

The hzb-cull aggregate persists **nothing**. Per
`SPEC.md` §7 ("Persistence & Schemas") the §5 surface contributes
no Fory schemas, and the aggregate's internal state has no on-disk
shape:

- The `HZB` pyramid is GPU-side `MTLTexture` storage; it does not
  serialise. Frame-N+1's cull works against frame-N's pyramid only
  while the process is live; on hot-reload (§8) the pyramid is
  invalidated and rebuilt next frame.
- `IndirectDrawBuffer` is per-frame transient; no on-disk shape.
- The cull kernel and HZB-build kernel are *PSO records* owned by
  `PSOCache` (#766); their on-disk archive is `shader`'s cook
  output, not this aggregate's.
- `HZBDesc` and `HZBHandle` are POD value types; they pass through
  the `RenderSettings` snapshot (when extent changes) but the
  snapshot is owned by `RenderSettings` Fory schema (§7.1.1), not
  this aggregate.

There is no Fory schema for any hzb-cull type; the codegen pipeline
does not iterate this aggregate. The `glibre-types.dylib` middleman
hash is unaffected by hzb-cull-aggregate changes (per
`fory-codegen.md` §"Middleman dylib").

ABI consequences (per `plugin-abi.md`):

- The §5 public surface (`HZB::ensure`, `HZB::invalidate`,
  `HZBHandle`, `HZBDesc`) is the entire ABI seam. Adding a method
  is a render-plugin ABI bump.
- The cull kernel's argument-buffer layout is **render-internal**;
  the kernel is invoked only by render-plug-in pass-body code, and
  the layout matches the Slang source compiled to the cached PSO.
  Plug-in authors outside render do not invoke the cull kernel.
- The `IndirectDrawBuffer` shape (`MTLDrawIndirectArguments` plus
  the trailing index slot for the fallback path; §3.8) is
  **render-internal**; the gbuffer pass and the fallback gbuffer
  pass are the only consumers and live inside the same plug-in.
  No external plug-in reads this buffer directly.
- The four error arms this aggregate emits (`HeapOutOfMemory`,
  `ResourceResidencyExceeded`, `MeshletCullDispatchFailed`,
  `MeshShaderCapabilityMissing`) are part of `render::Error`'s
  closed sum (`SPEC.md` §10.1); none are added by this design
  (all are already in §10.1 as either current §5 enum entries or
  "ABI add" rows planned by §10).

The aggregate consumes no Fory types; therefore no migration
bodies are required (`fory-codegen.md` §"Migration Mechanic" is
not exercised here).

## 8. Hot-reload

Hot-reload of the render plug-in follows the engine-wide protocol
(`hot-reload-protocol.md` drain → swap → migrate → resume). The
hzb-cull aggregate's contribution is fully covered by `SPEC.md` §8
"Hot-Reload Contract"; this section restates the aggregate-specific
points and binds them to the §3 algorithms.

### 8.1 What survives swap

**Nothing of the cull dispatch state.** Both transient companions
(`MeshletCullDispatch`, `HZBBuildPass`) live for one frame; phase
6/7 retire them before phase 8 begins.

**The `HZB` pyramid storage *survives*** in the sense that the
underlying `MTLTexture` allocations belong to the persistent
resource aggregate (#772, `resources/persistent.cpp`) and are
preserved across the swap per `SPEC.md` §8.2 row "Persistent
`Resource`s (HZB, history color, shadow atlas, ring buffers)".
However:

### 8.2 Why the data is dropped on swap (HZB rebuilt)

Even though storage is preserved, the **data inside the HZB is
invalidated** on plug-in swap:

1. The new plug-in's `meshlet_cull.slang` may have changed
   (different mip-level convention, different reverse-Z epsilon,
   different bounds-sphere field offset). Reading the prev plug-in's
   HZB with the new kernel risks subtly wrong cull (false-negative
   occlusion → over-cull, visible).
2. The new plug-in's `hzb_build.slang` may have changed mip-reduce
   convention.

So the aggregate's `migrate(...)` body (called by the engine-wide
hot-reload protocol; `SPEC.md` §8.3) calls
`HZB::invalidate(view)` for every active `View`. The next frame's
cull kernel reads the sentinel "no occlusion data" pyramid (§3.6)
and the cull degenerates to frustum + cone for one frame. The
next frame's HZB-build pass writes the new-plug-in pyramid; from
frame two onward, occlusion cull is fully online again.

The one-frame degenerated cull is permitted by `perf-budget.md`'s
hot-reload-frame budget (§9 below; the §9.4 `meshlet-cull` GPU
slice is 0.5 ms — losing the HZB step on one frame *reduces* the
slice by ~0.05 ms because the kernel skips the HZB sample).

### 8.3 `migrate(...)` body

Pseudo-code for the migration step the new plug-in's
`render_migrate` calls into:

```cpp
Result<void> migrate_hzb_cull(HZB& target, const HZB& source) noexcept {
    // Source = the outgoing plugin's HZB instance (about to dlclose).
    // Target = the new plugin's HZB instance.
    for (auto view : active_views) {
        const auto desc = source.desc(view);
        const auto rebuild = target.ensure(view, desc);
        if (!rebuild) return std::unexpected{rebuild.error()};
        target.invalidate(view);  // force fresh next-frame data.
    }
    return {};
}
```

The `migrate` body is on the critical path of phase 8 and is
budgeted under render's 0.40 ms phase-8 slice (`perf-budget.md`
§"Pipelined Frame Timing", row 8). The work is `O(views) ≤ 7`,
each `ensure` is `O(1)` cache hit (storage already exists), each
`invalidate` is a single zero-fill on the GPU side scheduled
asynchronously on the compute queue. CPU-side cost ≤ 0.02 ms; GPU
zero-fill ≤ 0.10 ms scheduled but overlapped with the standard
phase-8 work.

### 8.4 Refusal cases

The hzb-cull aggregate contributes one refusal cause:

- **`render::Error::HeapOutOfMemory`** raised at the first
  post-resume frame's `HZB::ensure`. Indicates the new plug-in's
  `HZBDesc` (e.g. a higher mip count from a settings change) cannot
  fit inside the persistent-resource budget. This rolls up to
  `core::Error::HotReloadRefused` per `SPEC.md` §8.4; the previous
  plug-in stays live. Test fixture:
  `tests/render/failure/resource_alloc_lower_tier.cpp`
  (`SPEC.md` §10.3 row).

`MeshShaderCapabilityMissing` is also a hot-reload refusal cause
when the new plug-in registers a HZB-cull pass that requires a
capability the current device lacks (e.g. RT-augmented cull, a
post-MVP plan); for MVP the kernel does not require any capability
beyond the baseline `Compute`, so this arm is dormant in MVP.

### 8.5 Editor live-reload of cull kernel source

Out of MVP scope. A per-shader live reload (e.g. editing
`meshlet_cull.slang` and pushing through Slang) would re-emit the
PSO record only; `PSOCache` (#766) is the owner. The aggregate
sees a new `PSOHandle` on the next frame but does not need to
rebuild the HZB pyramid storage. Listed in `hot-reload-protocol.md`
open question #5.

## 9. Performance

This section refines `SPEC.md` §9.2 (phase-6 CPU breakdown — the
"frustum + HZB cull" row) and §9.4 (GPU `meshlet-cull` slice) for
the hzb-cull aggregate. Every number is a ceiling, not a
steady-state expectation; drift trips the per-PR `perf-budget.yml`
gate (`perf-budget.md` §"CI Gate Spec").

### 9.1 Phase-6 CPU budget (per `View`, every frame)

Quotes `SPEC.md` §9.2 row "frustum + HZB cull": **0.20 ms** per
frame, S1, p99. Decomposed in §5.1.1 above. Multi-view (≤7 active
`View`s in MVP cap, §3.7) shares the same 0.20 ms budget because
the per-view kernel is a single dispatch per view — not seven
copies of the cap; the cap is **total**. Per-view dispatch cost
is amortised through one parameter-pack per view sharing one ring
slot.

### 9.2 Phase-7 CPU budget (HZB-build registration + dispatch)

Quotes `SPEC.md` §9.3 row "Per-pass `execute()` recording" — the
HZB-build pass shares the 0.50 ms phase-7 dispatch allowance with
the other ten MVP passes. Per §5.1.2 the aggregate's slice is
0.08 ms inside that 0.50 ms total — well-budgeted.

### 9.3 GPU budget (folded into `meshlet-cull` slice)

Quotes `SPEC.md` §9.4 row `meshlet-cull` (0.5 ms) and the
"HZB build, TLAS rebuild-or-refit, AO" folded row (no explicit
ceiling; folded under `shadow-rt` + `cluster-cull` queue
overlap). This aggregate's total GPU contribution:

| Kernel                                          | GPU ms     | Where it lives                                                                                            |
|-------------------------------------------------|------------|-----------------------------------------------------------------------------------------------------------|
| Meshlet cull (per `View`)                       | ~0.15 ms (S1, main view); ≤0.30 ms across 7 views | §9.4 `meshlet-cull` row; on `Queue::Compute` parallel with `cluster_cull`. |
| HZB build (per `View`, ≤14 mips)                 | ~0.20 ms (S1, main view); ≤0.30 ms across 7 views | §9.4 "HZB build, … folded" row; on `Queue::Compute` after gbuffer.        |
| **Aggregate GPU subtotal (S1, main only)**      | **~0.35 ms**| Inside the 0.5 ms `meshlet-cull` cap; the cluster-light-cull contribution shares the remainder.            |
| **Aggregate GPU subtotal (7 views)**            | **~0.60 ms**| Compute queue overlaps with graphics queue's gbuffer + transparent + post (5.0 ms); critical-path-invisible. |

The 7-view aggregate exceeds 0.5 ms on the compute queue but
remains invisible at the §9.4 wall-clock total because the
graphics queue is the long pole (5.0 ms) and the compute queue's
seven-view cull + build fits well inside that wall-clock. The
budget gate measures GPU wall-clock per `meshlet-cull` named slot;
seven `View`s emit seven distinct timestamp pairs (one per
dispatch). The per-`View` cap is 0.07 ms (S1 main); the total
across all views is bounded by 0.5 ms wall-clock through compute
queue serialisation.

### 9.4 Memory budget (persistent + transient)

| Storage                                                                                       | Lifetime              | Allocator                                                                       | Cap (S1 + 7 views)    |
|-----------------------------------------------------------------------------------------------|-----------------------|---------------------------------------------------------------------------------|-----------------------|
| HZB pyramid (per `View`, triple-buffered)                                                     | Persistent            | `resources/persistent.cpp`, `ContextTag::render`, sub-row "HZB pyramids"        | ≤21 MiB (7 views × 3 ring × ~1 MiB)  |
| `IndirectDrawBuffer` ring (per `View`, per frame)                                              | Transient             | `resources/transient_pool.cpp` via graph builder's `declare_transient`          | ≤7 × 16 MiB = 112 MiB |
| Sentinel "no occlusion data" pyramid                                                          | Plugin-instance       | `resources/persistent.cpp`                                                       | <4 KiB                 |
| Per-frame kernel-arg ring                                                                     | Per-frame             | `resources/ring_buffer.cpp` (per-frame ring already accounted in §9.5 "GPU resource handles" 16 MiB row of `SPEC.md`) | <1 MiB |

The 21 MiB persistent cost docks under `SPEC.md` §9.5 row
"persistent textures + buffers" 128 MiB, comfortably. The 112 MiB
transient cost docks under `SPEC.md` §9.5 row "Transient pool"
256 MiB. Both fit; the per-`View` `IndirectDrawBuffer` capacity
(16 MiB per view, supporting up to 4096 records × 256 material
slots × 16 B per record) is the only headroom-eating row, and the
alias-planner colouring (§4.1.5 invariant 2 — ≥40% recovery on
S1) cuts the effective residency to ≤67 MiB at S1 because
shadow-cascade indirect-args buffers can alias with main-view
post-pass scratch (§4.1.4 invariant 2).

Strict-mode enforcement (`GLIBRE_ALLOC_STRICT=1`) catches drift
above these caps and returns `core::Error::OutOfBudget`, mapped to
`render::Error::ResourceResidencyExceeded` at the call site
(`SPEC.md` §9.5.1 rule 2).

### 9.5 Cited acceptance benchmarks

Story #388 ("two-phase HZB occlusion cull (read N-1, write N)") —
note: glibre runs **single-phase** per the §2 collapse — carries
the per-`View` cull benchmark. Performance-relevant benchmarks
(named under `tests/render/perf/`):

| Benchmark name                                                       | Measures                                                                           | Ceiling   |
|----------------------------------------------------------------------|------------------------------------------------------------------------------------|-----------|
| `BENCHMARK("hzb-cull dispatch S1, p99 GPU")`                         | GPU wall-clock of the cull kernel for the main view, S1.                          | ≤ 0.15 ms |
| `BENCHMARK("hzb-build dispatch S1, p99 GPU")`                        | GPU wall-clock of the HZB-build kernel for the main view, S1.                     | ≤ 0.20 ms |
| `BENCHMARK("hzb-cull seven-view aggregate, p99 GPU")`                | Sum across all active views' cull dispatches.                                      | ≤ 0.30 ms (compute queue, parallel-overlap-bound) |
| `BENCHMARK("phase-6 frustum+HZB cull CPU, S1, p99")`                 | CPU wall-clock between phase-6 cull entry and dispatch return.                    | ≤ 0.20 ms |
| `BENCHMARK("HZB pyramid residency, S1, strict-mode")`                | Live bytes on `ContextTag::render` HZB pyramid sub-row.                            | ≤ 21 MiB  |

The first three roll up into the §9.4 `meshlet-cull` wall-clock
row; the fourth rolls up into the §9.2 phase-6 row; the fifth
into the §9.5 persistent-textures row.

### 9.6 Cross-references

- `SPEC.md` §9.2 — phase-6 CPU breakdown (this aggregate's row is
  "frustum + HZB cull" = 0.20 ms).
- `SPEC.md` §9.3 — phase-7 CPU breakdown (HZB-build dispatch
  registration + recording inside the 0.50 ms per-pass record
  allowance).
- `SPEC.md` §9.4 — GPU breakdown (this aggregate folds into
  `meshlet-cull` 0.5 ms slice + the "HZB build … folded" row).
- `SPEC.md` §9.5 — heap composition (HZB persistent ≤21 MiB inside
  the 128 MiB persistent row; `IndirectDrawBuffer` transient
  ≤112 MiB inside the 256 MiB transient pool).
- `SPEC.md` §9.6.1 — per-pass GPU timestamp queries (this
  aggregate's `meshlet-cull` and HZB-build slots are named).
- `perf-budget.md` §"Pipelined Frame Timing" — render's 1.40 ms
  submit slot; this aggregate's 0.08 ms phase-7 share.

## 10. Failure modes

The closed enumeration of `render::Error` arms emitted by this
aggregate (subset of `SPEC.md` §10.1; the variants are already in
the §5 stub or queued as ABI-add rows per §10.1's audit):

| `render::Error` arm              | §10.1 row             | Trigger                                                                                                                                   | Recovery        | Severity | Test fixture                                             |
|----------------------------------|-----------------------|-------------------------------------------------------------------------------------------------------------------------------------------|-----------------|----------|----------------------------------------------------------|
| `HeapOutOfMemory`                | `ResourceAllocFailed` | `HZB::ensure` cannot allocate the persistent mip chain (§3.2). Driver-side `MTLHeap::newTextureWithDescriptor:offset:` returns `nil`.       | `lower-tier`    | `warn`   | `tests/render/failure/resource_alloc_lower_tier.cpp`     |
| `ResourceResidencyExceeded`      | `ResourceResidencyExceeded` | `HZB::ensure` would push the `ContextTag::render` HZB sub-row over its share of §9.5 persistent-textures (21 MiB cap, §9.4 above).      | `lower-tier`    | `warn`   | `tests/render/failure/residency_exceeded_lower_tier.cpp` |
| `MeshletCullDispatchFailed`      | `MeshletCullDispatchFailed` (ABI add) | Cull kernel dispatch returns Metal 4 `MTLCommandEncoderError`, *or* the per-dispatch `IndirectArgsOverflow` flag is set on CPU readback at next frame's phase-6 entry (§3.4 step 4). | `disable-feature` (kernel error → `MeshShaders` capability cleared, fallback path takes over per §3.8); `lower-tier` (overflow → drop draw budget per `RenderSettings.per_view_draw_budget`). | `warn`   | `tests/render/failure/meshlet_dispatch_disable.cpp` (kernel) and `tests/render/failure/indirect_args_overflow_lower_tier.cpp` (overflow; ABI-add row) |
| `MeshShaderCapabilityMissing`    | `MeshShaderCapabilityMissing` | Init or hot-reload register: the cull kernel's `Capability::MeshShaders` declared dependency is unsatisfied. Note: in MVP the cull kernel itself does **not** require `MeshShaders` (the gbuffer pass does); this arm is reserved for post-MVP cull paths that fuse mesh-shader emit. | `lower-tier` (init) / `disable-feature` (hot-reload). | `warn`   | `tests/render/failure/mesh_shader_missing.cpp`            |

Cross-cutting notes (per `SPEC.md` §10.2):

- **`MeshletCullDispatchFailed` is the load-bearing arm.** Kernel
  errors are exceedingly rare (driver-side); overflow is the
  realistic trigger and exists because the per-material slot
  capacity is fixed at init from `RenderSettings.per_view_draw_budget`.
  When the overflow flag is set, the next frame's cull dispatch
  receives a reduced `per_view_draw_budget` (the lower-tier
  recovery shrinks the budget by 25% per overflow event, cumulative,
  with a floor at 25% of the initial budget); a survivor of the
  reduction is the budget-aware cull aggregate (#762; `cull/budget.cpp`)
  which trims by `PassPriority` *before* the cull dispatch is even
  queued. The `IndirectArgsOverflow` payload is therefore the
  signal hzb-cull sends *upstream* to budget; the recovery happens
  in budget-cull, not here.
- **`ResourceResidencyExceeded` is *budgetary*, not driver.** It
  fires only when strict-mode CI catches drift; production builds
  log `warn` and continue, with the editor's perf HUD surfacing the
  overshoot. Lower-tier recovery (one tier down) shrinks the
  per-`View` HZB extent (e.g. 1080p → 720p) which cascades the
  pyramid mip count down (one fewer mip), reducing residency by
  ~25% per tier step.
- **`MeshShaderCapabilityMissing` is dormant in MVP.** The MVP
  cull kernel runs on the baseline compute capability; this arm
  is wired so a post-MVP "fused mesh-shader emit" cull path can
  declare the capability and fail closed. Listed in §12.
- **Severity escalation under hot-reload.** When raised inside the
  first post-resume `HZB::ensure`, `HeapOutOfMemory` /
  `ResourceResidencyExceeded` log at `warn` and roll up under
  `core::Error::HotReloadRefused` (`SPEC.md` §8.4); the previous
  plug-in keeps running. At engine startup (no prior-good plug-in),
  they log at `error` and the engine aborts (`SPEC.md` §10.2
  `abort-engine`).

The aggregate does **not** emit graph-layer arms
(`RenderGraphCycle`, `BarrierConflict`, `PassUnsupportedConfig`);
those belong to the render-graph aggregate (#760). It does **not**
emit pipeline-state arms (`PsoCompileFailed`, `ShaderModuleLoadFailed`);
those belong to `PSOCache` (#766) and the device boot path (#762).
It does **not** emit device, swapchain, or fence arms
(`MetalDeviceUnavailable`, `SwapchainAcquireFailed`,
`PresentTimeout`, `GpuTimeout`, `GpuFault`); those belong to the
device + queue aggregate (#762) and the platform fence-wait
(§10.4 of `SPEC.md`).

## 11. Test plan

Tests are split between Catch2 unit tests under
`tests/render/cull/` and `tests/render/hzb/`, and integration tests
under `tests/render/integration/` that exercise the kernels against
a real `MetalDevice` from the platform fixture
(`platform/test/MetalDeviceFixture.hpp`, peer aggregate). Every
test names a §10 row, a §3 algorithm step, a §SPEC §11 acceptance
story, or a §9 benchmark.

### 11.1 Unit tests — frustum + cone math

| TC ID                                                 | Trigger                                                                                  | Expectation                                                                | §-link        | Story  |
|-------------------------------------------------------|------------------------------------------------------------------------------------------|----------------------------------------------------------------------------|---------------|--------|
| `cull_math/frustum_inside_keeps`                      | Bounds sphere fully inside the six camera planes.                                        | Cull bit = 0 (keep).                                                       | §3.4 step 1   | #388   |
| `cull_math/frustum_outside_culls`                     | Bounds sphere fully outside the left plane.                                              | Cull bit = 1 (cull).                                                       | §3.4 step 1   | #388   |
| `cull_math/frustum_straddle_keeps`                    | Bounds sphere straddles the near plane.                                                  | Cull bit = 0 (conservative keep).                                          | §3.4 step 1   | #388   |
| `cull_math/cone_facing_keeps`                         | Normal-cone fully facing camera.                                                         | Cull bit = 0 (keep).                                                       | §3.4 step 2   | #388   |
| `cull_math/cone_back_culls`                           | Normal-cone fully back-facing camera (sphere behind a flat surface).                    | Cull bit = 1 (cull).                                                       | §3.4 step 2   | #388   |
| `cull_math/cone_two_sided_skips`                      | Material flags two-sided; cone is back-facing.                                           | Cull bit = 0 (keep, cone test skipped).                                    | §3.4 step 2   | #388   |
| `cull_math/instance_transform_applied`                | Bounds sphere's center is in local space; instance transform translates outside frustum. | Cull bit = 1 (cull, transform-aware).                                      | §3.4 step 1   | #388   |

### 11.2 Unit tests — HZB occlusion math

| TC ID                                                 | Trigger                                                                                  | Expectation                                                                | §-link        | Story  |
|-------------------------------------------------------|------------------------------------------------------------------------------------------|----------------------------------------------------------------------------|---------------|--------|
| `hzb_math/sample_mip_choice`                          | Bounds projects to a 8×8 px screen AABB.                                                 | Mip = 3 (`ceil(log2(8))`); 2×2 footprint.                                  | §3.4 step 3.5 | #388   |
| `hzb_math/sample_min_reduce`                          | HZB pyramid mip 1 is `min` of mip 0's 2×2 children.                                      | Sample at mip 1 returns `min` of four children.                            | §3.5          | #388   |
| `hzb_math/reverse_z_compare`                          | `z_max_ndc = 0.7` (sphere's closest face in reverse-Z), HZB sample = 0.6 (closer occluder = larger NDC-Z). | `z_max_ndc < hzb_min` ⇒ keep (front face is in front of occluder). | §3.4 step 3.6 | #388   |
| `hzb_math/reverse_z_compare_culled`                   | `z_max_ndc = 0.5` (sphere's closest face), HZB sample = 0.6 (occluder is closer than sphere's front face). | `z_max_ndc > hzb_min` ⇒ cull (entire sphere behind occluder).   | §3.4 step 3.6 | #388   |
| `hzb_math/sphere_endpoint_closest_face`               | Sphere with center NDC-Z = 0.6, radius = 0.1 (reverse-Z). `z_max_ndc = 0.7` (closest face); `z_min_ndc = 0.5` (farthest face). HZB sample = 0.65. | Cull test uses `z_max_ndc = 0.7 > 0.65` ⇒ keep (front face is not behind occluder). Using `z_min_ndc = 0.5` instead would produce a false cull. | §3.4 step 3.3 | #388   |
| `hzb_math/sentinel_keeps_all`                         | Prev HZB is the sentinel "no occlusion data" pyramid.                                    | Cull bit = 0 for all meshlets (pyramid samples to `+inf`).                 | §3.6          | #388   |
| `hzb_math/near_plane_skip_test`                       | Bounds sphere `clip.w` < near-plane epsilon.                                             | HZB step skipped; frustum + cone result determines keep.                   | §3.4 step 3   | #388   |

### 11.3 Unit tests — `HZB::ensure` / `invalidate`

| TC ID                                                 | Trigger                                                                                  | Expectation                                                                | §-link        | Story  |
|-------------------------------------------------------|------------------------------------------------------------------------------------------|----------------------------------------------------------------------------|---------------|--------|
| `hzb_lifecycle/ensure_first_call_allocates`           | Fresh `HZB`; `ensure(view, {1920, 1080, 0})`.                                            | Returns `HZBHandle`; mip count = 11; ring slots allocated.                 | §3.2          | #388   |
| `hzb_lifecycle/ensure_idempotent`                     | Two `ensure(view, desc)` calls with matching `desc`.                                     | Returns the same `HZBHandle` (cache hit).                                  | §3.2          | #388   |
| `hzb_lifecycle/ensure_resize_triggers_realloc`        | `ensure(view, {1920, 1080, 0})` then `ensure(view, {1280, 720, 0})`.                     | Returns a new `HZBHandle`; old storage destroyed.                          | §3.2          | #388   |
| `hzb_lifecycle/ensure_invalid_mips_rejected`          | `ensure(view, {1920, 1080, 99})` (mips above cap).                                       | Returns `unexpected(HeapOutOfMemory)`.                                     | §4 invariant 2| #388   |
| `hzb_lifecycle/invalidate_clears_data_keeps_handle`   | `invalidate(view)` then `ensure(view, desc)`.                                            | Same `HZBHandle`; data cleared.                                            | §3.2 + §4 inv 3| #388   |
| `hzb_lifecycle/invalidate_unknown_view_noop`          | `invalidate(view)` for a `View` never `ensure`d.                                         | Returns `void`; no error logged.                                           | §4 invariant 1| #388   |
| `hzb_lifecycle/multi_view_independent`                | Two `View`s `ensure`d with different extents.                                            | Two distinct `HZBHandle`s; storage not aliased.                            | §3.6 + §4.1.9 inv 2| #388 |

### 11.4 Unit tests — Indirect-args compaction

| TC ID                                                 | Trigger                                                                                  | Expectation                                                                | §-link        | Story  |
|-------------------------------------------------------|------------------------------------------------------------------------------------------|----------------------------------------------------------------------------|---------------|--------|
| `cull_emit/single_material_compaction`                | 100 meshlets, all material slot 0; all survive frustum + cone + HZB.                     | `output_buffers[0]` has 100 records; `output_count[0] == 100`.             | §3.4 step 4   | #388   |
| `cull_emit/multi_material_grouped`                    | 100 meshlets across 4 material slots, evenly split.                                      | Each `output_buffers[m]` has 25 records.                                   | §3.4 step 4   | #388   |
| `cull_emit/overflow_sets_flag`                        | 5000 meshlets, material slot 0 capacity = 4096.                                          | `overflow_flag == 1`; `output_count[0] == 4096`; later 904 dropped.        | §3.4 step 4   | #388   |
| `cull_emit/instance_count_default`                    | One meshlet survives.                                                                    | `output_buffers[mat][0].instance_count == 1`.                              | §3.4 step 4   | #388   |
| `cull_emit/two_sided_flag_round_trip`                 | Material index high bit set; cone is back-facing.                                        | Survivor emitted (cone test skipped).                                      | §3.4 step 2   | #388   |

### 11.5 Integration tests — real `MetalDevice` (Apple-Silicon CI)

| TC ID                                                 | Trigger                                                                                  | Expectation                                                                | §-link        | Story  |
|-------------------------------------------------------|------------------------------------------------------------------------------------------|----------------------------------------------------------------------------|---------------|--------|
| `cull_integration/golden_survivor_set_S1`             | S1 fixture (1 character + 200 props), main view, prev HZB primed from frame 0.            | Survivor bitset matches the recorded golden bitset (byte-equal).           | §3.4          | #388   |
| `cull_integration/disocclusion_recovers_next_frame`   | Move camera so 100 occluded meshlets become disoccluded between frames N and N+1.        | Frame N+1's survivors include all 100 disoccluded meshlets (frustum + cone bring them in; HZB no longer rejects). | §3.4 + §3.5 | #388 |
| `cull_integration/hzb_build_pyramid_min_property`     | Render S1 frame; sample HZB mip K and verify it is the `min` of mip K-1's 2×2 footprint. | All mip levels satisfy the min property byte-equal.                        | §3.5          | #388   |
| `cull_integration/triple_buffer_no_frame_race`        | Run 60 frames; assert no GPU-side hazard via Metal 4 `MTLDebugLayer` validation.         | Zero validation errors.                                                    | §6.3          | #388   |
| `cull_integration/multi_view_seven_views`             | Main + 4 cascade + 1 probe + 1 viewmodel; one frame.                                     | Seven cull dispatches recorded; seven HZB-build dispatches recorded.       | §3.7          | #401 (parent epic; this aggregate's contribution) |
| `cull_integration/sentinel_first_frame_post_invalidate`| `invalidate(view)` then render one frame.                                                 | Cull kernel reads sentinel pyramid; survivor set = frustum + cone result.  | §3.6 + §8.2   | #388   |
| `cull_integration/extent_change_one_conservative_frame`| `ensure(view, new_extent)` mid-process; render two frames.                                | Frame 1's cull = frustum + cone (sentinel); frame 2's cull = full HZB-aware. | §3.2        | #388   |

### 11.6 Performance benchmarks (under `tests/render/perf/`)

The benchmarks named in §9.5 above. Each runs the S1 fixture from
`e2e/perf/`. The asserts are the `time <= cell_budget_ms` form of
`perf-budget.md` §"CI Gate Spec" item 1.

### 11.7 Failure-mode fixtures (cited from §10)

The four files cited in the §10 table:

- `tests/render/failure/resource_alloc_lower_tier.cpp`
- `tests/render/failure/residency_exceeded_lower_tier.cpp`
- `tests/render/failure/meshlet_dispatch_disable.cpp`
- `tests/render/failure/indirect_args_overflow_lower_tier.cpp` (ABI-add row)
- `tests/render/failure/mesh_shader_missing.cpp`

Each reproduces the §10.3 trigger and asserts the recovery action
(matching `SPEC.md` §10.2 ladder entries).

## 12. Open questions

- `[OPEN]` Two-phase HZB recovery (R-2.3.4) — glibre runs single-phase
  cull against last-frame HZB. If MVP playtest shows visible
  disocclusion pop on fast camera translation, re-spike the
  two-phase shape; the cost is one extra HZB rebuild + one
  rasterisation re-issue mid-frame, which would split the
  `SPEC.md` §4.2 invariant 5 atomic gbuffer pass. **Resolution
  gate:** spike #770-followup, opened by render-domain owner if
  the §11.5 `disocclusion_recovers_next_frame` test fails to
  match harmonius's "no pop-in" verification on a designated
  S1+camera-trajectory fixture.
- `[OPEN]` Multi-view shared cull (R-2.10.4a) — MVP runs one cull
  dispatch per `View`. If the seven-view aggregate (§9.3) becomes
  the dominant slice as shadow cascades grow, re-spike a
  per-meshlet bitset of `View` membership shared across overlapping
  frusta. **Resolution gate:** §11.6 `BENCHMARK("hzb-cull
  seven-view aggregate, p99 GPU")` exceeds 0.30 ms on the
  shadow-cascade-heavy fixture for two consecutive nightlies.
- `[OPEN]` Per-meshlet visibility feedback cache — harmonius's
  per-meshlet visibility cache between frames was refused for MVP
  to avoid migration complexity. **Resolution gate:** post-MVP
  perf-amendment spike if the cull GPU slice exceeds 0.5 ms on a
  high-density scene (>10k meshlets).
- `[OPEN]` Instance batching inside `IndirectDrawBuffer` — current
  MVP emits `instance_count = 1` per meshlet record. Instance
  fan-out (multiple instances of the same mesh collapsed to one
  indirect-args record) would shrink the buffer footprint and the
  encoder time. **Resolution gate:** post-MVP plan if the §9.4
  transient-pool 256 MiB row trends above 75% on the four-view
  cap.
- `[OPEN]` Two-sided material bit position — currently the high bit
  of `material_index[]` (`& 0x7FFF` mask). The material-plug-in
  ticket (#780) may relocate this to a separate `material_flags[]`
  span so the material slot index reclaims 16 bits. **Resolution
  gate:** material plug-in design spike (#780) seam decision.
- `[OPEN]` Fused mesh-shader emit cull — a post-MVP path where the
  cull kernel directly issues mesh-shader threadgroup dispatches
  (Metal 4 `setComputePipelineState` + `dispatchMeshThreadgroups`
  fusion) skipping the `IndirectDrawBuffer` round-trip. Would
  require `Capability::MeshShaders` declared on the cull pass
  (the `MeshShaderCapabilityMissing` arm sits dormant for this
  reason today). **Resolution gate:** post-MVP perf amendment if
  the per-view cull → gbuffer fence becomes the long pole.
- `[OPEN]` HZB-build mip count above 14 — Apple Silicon's 14-mip
  cap forces large-extent `View`s (e.g. 4K resolution post-MVP)
  to drop conservativeness in the deepest mip. **Resolution
  gate:** post-MVP if 4K render extents land; current MVP cap is
  1080p main + 1024² cascades (max 11 mips on main, 10 on
  cascades) so the 14-mip cap is pure headroom today.
- `[OPEN]` `IndirectArgsOverflow` recovery cadence — current
  design shrinks `RenderSettings.per_view_draw_budget` by 25% per
  overflow, with a 25% floor. The aggregate sends the signal but
  the recovery actuator lives in budget-cull (#762,
  `cull/budget.cpp`). The exact cadence and floor warrant a
  joint spike with the budget-cull design once that lands.
  **Resolution gate:** budget-cull design spike (followup to
  #770-equivalent in budget-cull's task breakdown).
