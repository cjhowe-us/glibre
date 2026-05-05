# render — Detailed Design: render-frame-extract aggregate

> Detailed design for the **render-frame-extract** aggregate in the
> `render` context. Refines `specs/render/SPEC.md` §4.1.1 (`RenderFrame`),
> §4.2 invariant 6 (the ECS↔GPU seam), §5 (`RenderFrame` opaque + entry
> points), §6.2.1 (phase-6 `cull-extract` body), §9.2 (CPU phase-6
> budget), §10 (extract-side `render::Error` triggers), §11 (acceptance
> stories that flow through the snapshot). Cites
> `reviews/decisions/error-model.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/fory-codegen.md`, and
> `reviews/decisions/frame-phases.md` (rows 6 and 7). Introduces no new
> public surface beyond `specs/render/SPEC.md` §5; deviations require an
> amendment spike, not an in-place edit. Resolves
> `[SPIKE] design-render-render-frame-extract-detailed` (#772). Sibling
> task-breakdown spike is blocked by this deliverable.

## 1. Purpose

The render-frame-extract aggregate is render's **sim → render hand-off**:
the phase-6 walk that reads the ECS and produces the immutable
`RenderFrame` snapshot consumed by phase 7. It owns:

1. The **`RenderFrame` snapshot record** — a frozen, self-contained,
   per-`(World, FrameCounter)` value built in render's per-frame arena.
   Lifetime starts at phase-6 entry; ends at phase-7 exit
   (`SPEC.md` §4.1.1 invariants 1, 2, 4).
2. The **per-archetype extract walk** — the read-only traversal of the
   ECS that materialises every byte the snapshot needs (renderable SoA
   columns, lights, cameras, render-layer masks, render-settings
   snapshot, interpolation `alpha`, frame counter `N`).
3. The **double-buffer / triple-buffer slot machinery** — a fixed pool
   of `RenderFrame` slots cycled by a generational `FrameHandle` so
   frame N+1's extract can begin while phase 7 of frame N is still
   recording (`SPEC.md` §4.1.1 invariant 4; `SPEC.md` §4.2 invariant 8).
4. The **transient extract arena** — a per-frame-slot bump allocator
   tagged `ContextTag::render` against the perf-budget transient-arena
   exemption (`reviews/decisions/perf-budget.md` Allocator Rule 4).
5. The **snapshot-bus emit point** — render's only output from phase 6:
   a `const RenderFrame&` delivered to phase 7 in the same dylib via
   the snapshot bus (`SPEC.md` §6.2.1 step 3; §5 entry takes a
   `const RenderFrame&`).
6. The **failure surface for the walk** — the typed `render::Error`
   variants that can fire while reading ECS storage and writing the
   snapshot (`TransientPoolExhausted`, `ResourceImportRefused`,
   `PassUnsupportedConfig` mapped to `SPEC.md` §10's existing closed
   sum; no new variants).

This aggregate **refuses to own**:

- **Simulation.** ECS storage, fixed-tick advancement, and component
  authoring belong to `core` and to the gameplay-side plugins. Extract
  is a *consumer* of immutable ECS queries (`SPEC.md` §4.2 invariant 6:
  no aggregate inside phase 7 calls back into ECS systems; extract is
  the only render-side reader, and only inside phase 6).
- **Graph topology.** `RenderGraph` and `GraphBuilder` (`SPEC.md`
  §4.1.2; design ticket #760) consume the snapshot but do not
  participate in its construction. Phase-6 returns a finished snapshot;
  phase-7 reads it `const`.
- **Pass bodies.** `passes/*.cpp` (#764) read the snapshot's spans
  through their `Bindings`; they do not extend the snapshot's record
  taxonomy.
- **Culling algorithms.** Frustum cull, HZB occlusion cull, meshlet
  cull, cost-aware budget cull live in the **cull aggregate** (#770,
  files `cull/meshlet_cull.cpp`, `cull/budget.cpp`, `cull/sort.cpp`,
  `cull/hzb.cpp`). The extract walk hands cull a populated proxy SoA
  and accepts a culled survivor span back; it does not implement the
  cull predicates. The boundary is one function call per `View` from
  `cull/extract.cpp` into `cull/meshlet_cull.cpp` (`SPEC.md` §6.2.1
  step 2.1).
- **Sort-key construction.** `SortKey` packing and the single-pass
  radix run in `cull/sort.cpp` (#770) over the survivor span; extract
  reserves the column slot in the snapshot but does not author its
  bytes.
- **GPU residency / mesh / material handle resolution.** Extract
  carries opaque `MeshHandle` / `MaterialHandle` / `BLASHandle` /
  `RenderLayerMask` / `ViewHandle` values forward by index; the
  resolution to GPU-resident pipeline state, mesh streams, and BLAS
  blobs happens in phase 7 against `PSOCache` (#766),
  `RTAccelStructures` (#768), and the resource aggregate (#762,
  `resources/transient_pool.cpp`). Extract refuses to dereference any
  GPU-side pointer (`SPEC.md` §4.1.1 composition: "no GPU handles
  inside `RenderFrame` itself").
- **Persistent storage.** `RenderFrame` is never serialised
  (PHILOSOPHY anti-pattern); no Fory schema lives at this layer
  (`SPEC.md` §5 "Serialised schemas (Fory). None at this layer.").
  Cross-frame survival is handled exclusively by the persistent
  `Resource`s aggregate (`SPEC.md` §4.1.4) — not by the snapshot.
- **Multi-context snapshot fan-out.** The frame-phases record poses an
  open question (`reviews/decisions/frame-phases.md` Q4) about
  generalising `RenderFrame` to a `FrameSnapshot` once an audio
  plugin lands. That generalisation is post-MVP; the MVP `RenderFrame`
  carries only render-relevant extracts.

If the way the extract walk reads ECS archetypes, the way the proxy SoA
columns are laid out, the way the slot pool is rotated, the way the
per-frame arena is sized, or the way the snapshot is published to phase
7 changes, this design changes. Anything else is out of scope.

## 2. Requirements coverage

This section verifies that every harmonius MVP-scope requirement /
design clause about the **scene-rendering pipeline's extract half** is
either covered by the design below or explicitly refused with rationale.
Inputs (research only, re-derived per PHILOSOPHY §"How harmonius is
used"):

- `harmonius/docs/requirements/rendering/scene-rendering-pipeline.md`
  (R-2.10.1 .. R-2.10.9, R-2.10.4a, R-2.3.14, R-2.4.24; only the
  extract-relevant subset — sort, batch, draw-list authoring belong
  to cull / graph aggregates).
- `harmonius/docs/design/rendering/rendering-core.md`
  § "Architecture", § "Extract-Prepare-Render Pipeline",
  § "Transform Interpolation", § "RenderFrame Cross-Reference".
- `harmonius/docs/design/rendering/render-pipeline.md`
  § "Frame Synchronization" (triple-buffer producer/consumer between
  game worker threads and render thread).
- `harmonius/docs/design/rendering/camera-rendering.md`
  § "Architecture", § "Render Layer Masking" (32-bit mask).

| Harmonius clause                                                                                  | Glibre disposition                                                                                                                                                                                                                                                                                              |
|---------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-2.10.1** — Extract visible ECS entities into a renderer-owned snapshot each frame on a dedicated thread using immutable queries. | **Covered, refined.** §3.1 / §3.4: `cull/extract.cpp` is the only render-side reader of ECS storage, runs on the driver thread inside phase 6 (a single sequential body, not a separate worker pool — `SPEC.md` §6.2.1, §6.3 driver-thread only). Immutable queries are enforced by the C++ overload set (`World::view_const<...>()`); §6 below details the read-only contract. The snapshot is render-owned (lives in render's per-frame arena). |
| **R-2.10.2** — Render proxies in a flat SoA layout holding only GPU-needed data, with dirty-flag-driven incremental updates reducing per-frame bandwidth from O(N) to O(changed). | **Covered (SoA), refused (dirty-flag incremental).** §3.2 below: the renderable proxy SoA is the canonical layout (separate columns for transform, mesh handle, material handle, layer mask, AABB, meshlet-bounds offset). **Dirty-flag incremental upload is refused for MVP** because (a) the snapshot is rebuilt fresh every frame from immutable queries — the "diff" path would require a parallel persistent `RenderWorld`, which `SPEC.md` §4.2 invariant 6 forbids ("`RenderFrame` is the only ECS↔GPU seam"); and (b) the S1 fixture's 200 props × ~16 meshlets ≈ 3.2k proxies fits comfortably in the 0.20 ms `meshlet_cull` slice and the 0.03 ms snapshot-finalise slice (`SPEC.md` §9.2). The bandwidth claim ("O(changed)") is *re-derived as O(visible)* via cull, which is monotone tighter than O(N) for the MVP scene shapes. Listed in §12 below as a post-MVP perf amendment if S1+ scenes break the budget. |
| **R-2.10.3** — Each active view registered with projection, view matrix, viewport rect, quality tier; multi-view from one snapshot. | **Covered.** §3.3 / §3.5: the snapshot's `view_table_` column carries one `ExtractedView` row per active `View`, including the camera + jitter, projection, viewport, and 32-bit `RenderLayerMask`. Multi-view fan-out (split-screen, VR stereo, shadow cascades, reflection probes, viewmodel) all read the same snapshot in phase 7 (`SPEC.md` §3.2 collapse #7); the extract walk runs once per frame and produces per-view culled spans by indexing the renderable proxy SoA by `ViewHandle`. |
| **R-2.10.4 / R-2.10.4a** — Per-view draw lists sorted by material/mesh/state with packed 64-bit `SortKey`; single-pass radix sort. | **Refused at this aggregate; routed to cull aggregate (#770).** Extract reserves the per-view `SortKey` column slot in the snapshot but does not author its bits. `cull/sort.cpp` packs the key and runs the radix; `cull/budget.cpp` runs the cost-aware survivor trim. Extract's role is the column allocation (`eastl::span<std::uint64_t>` reserved out of the arena) and the survivor-set hand-off; the bytes are filled by cull. This is the SRP boundary that drove the spike split: **proxy authoring (extract) ≠ proxy ordering (cull)**. |
| **R-2.10.5** — Bindless per-draw material parameters via descriptor indices in a per-instance buffer. | **Covered (handle path), routed (binding).** Extract carries a `MaterialHandle` per renderable in the SoA (one column). The bindless argument-buffer assembly is `resources/argument_buffer.cpp` in the resources aggregate (#762) and runs in phase 7's compile half; extract refuses to touch argument buffers (no GPU-side mutation in phase 6 per `SPEC.md` §6.2.1: "no GPU calls yet"). |
| **R-2.10.6** — Immediate-mode debug drawing API + diagnostic render modes; compile-time gated out of shipping. | **Refused at this aggregate; routed to `DiagnosticOverlay` (#774, `graph/diagnostic.cpp`).** Extract neither queries nor publishes debug primitives. The overlay reads the snapshot read-only at phase-7 record time, the same way every other consumer does; debug primitives live in tools / editor extracts that piggyback on the same snapshot bus when those plugins ship (post-MVP). |
| **R-2.10.7** — 32-bit render-layer bitmask filtering renderables, cameras, and lights at extract time. | **Covered.** §3.5 below: the snapshot carries one `RenderLayerMask` per `ExtractedView` and per renderable proxy / `ExtractedLight`. Per-view culled spans are computed from the AND of the view mask with each candidate's mask; the bit-test happens during the extract walk (cheap; one `u32 & u32`) so cull does not have to re-walk the full population per view. |
| **R-2.10.8** — Transform interpolation using `alpha = (elapsed - last_fixed_tick) / fixed_dt`. | **Covered.** §3.5: the snapshot carries one `float alpha` (computed once per frame by core's frame-loop and broadcast through the snapshot's header). Extract reads `GlobalTransform` *and* `PreviousGlobalTransform` and writes `lerp(prev, curr, alpha)` into the proxy SoA's transform column (and a velocity column derived as `curr - prev` per frame, fed to the gbuffer's velocity MRT in phase 7 by `passes/gbuffer.cpp`). The `alpha` value is also written verbatim into the snapshot header so passes that need both endpoints (motion blur in `passes/post.cpp`) can re-derive. |
| **R-2.10.9** — Ring-buffer per-frame GPU resources indexed by frame-in-flight. | **Refused at this aggregate; routed to resources (#762, `resources/ring_buffer.cpp`).** Extract owns the **CPU-side** triple-buffered slot pool for the `RenderFrame` snapshot itself (§3.6 below); the **GPU-side** ring buffers (instance buffer, uniform buffer, indirect-arg buffer) are the resources aggregate's responsibility. The two ring schedules are independent — render's CPU snapshot pool is 3 deep (pipelined-frame model in `reviews/decisions/perf-budget.md`); the GPU rings are 3 deep too, but their indexing is `frame_counter % 3` driven by phase 7. |
| **R-2.4.24** — Light probes + reflection probes as ECS entities with refresh modes. | **Covered (handle pass-through), out of scope (refresh).** Extract emits a `ProbeHandle` column for any probe-tagged ECS entity into the snapshot's lights / probes table; the *refresh policy* (baked / on-load / periodic / on-change) lives in the lighting / probe sub-aggregate (post-MVP — `SPEC.md` §3.3 routes IBL probes to a future ticket). For MVP, probes are static handles in the snapshot; their cubemaps are persistent `Resource`s consumed read-only by `passes/lighting.cpp`. |
| **R-2.3.14** — Mesh-shader dispatch for surviving meshlets, indirect-draw fallback. | **Refused at this aggregate; routed to gbuffer pass (#764, `passes/gbuffer.cpp`) + cull (#770, `cull/meshlet_cull.cpp`).** Extract carries the meshlet-bounds offsets into the proxy SoA; `cull/meshlet_cull.cpp` walks them in phase 6 and `passes/gbuffer.cpp` dispatches mesh shaders in phase 7. The fallback path is a graph-build-time predicate elision (`Capability::MeshShaders` cleared → graph builder substitutes the vertex+amplification fallback pass; `SPEC.md` §10.3 row `MeshletCullDispatchFailed`). Extract neither chooses nor knows which path runs. |
| Harmonius design — Triple-buffered producer/consumer between game worker threads and render thread (`render-pipeline.md` §"Frame Synchronization"). | **Covered, refined.** §3.6 below: render's snapshot pool is 3 deep, generationally indexed by `FrameHandle`. **Producer/consumer thread topology is refused as described** (harmonius spec splits "game worker threads" from a "render thread"); glibre's frame-phases record places phase 6 on the *driver thread*, not on a worker pool (`SPEC.md` §6.3, §6.2.1; `frame-phases.md` row 6 "owner: render"). The triple-buffer slot machinery is identical in spirit (3 slots, atomic swap, freshest-wins on consumer acquire); the threading model collapses to "phase 6 writes one slot, phase 7 of the previous frame reads another, phase 9 reads the third" — driven by frame-phase ownership rather than free-running threads. The "stale-fame replay" failure mode survives: if phase 6 of frame N+1 falls behind, phase 7 of frame N can still complete unaffected because its slot is pinned (`SPEC.md` §4.1.1 invariant 4). |
| Harmonius design — `ProxyStore` persistent across frames with dirty-flag incremental updates. | **Refused / collapsed.** Glibre has no `ProxyStore` aggregate; the snapshot **is** the store, and it is rebuilt every frame in the per-frame arena. The "incremental update" optimisation is replaced by aggressive cull (`cull/meshlet_cull.cpp`) so the work that would be incremental in harmonius is simply elided in glibre. SRP justification: a persistent `ProxyStore` across frames is a second source of truth (ECS is the first), and the §4.2 invariant 6 forbids second sources. |
| Harmonius design — `RenderFrame` defined by `core-runtime/game-loop.md`, consumed by render. | **Refused / re-routed.** Glibre's `RenderFrame` is **render-owned** (`SPEC.md` §4.1.1: "allocated in render's per-frame arena at phase 6 entry"). Core does not define the type; render does (`SPEC.md` §5 publishes the opaque). The snapshot bus is a `core` infrastructure (it is just the cross-phase reference passing inside the same process); the *type* it carries is render's. This re-routing is the §4.2 invariant 6 in force: render owns the seam. |
| Harmonius design — Extract on a dedicated thread; simulation advances concurrently. | **Refused, replaced by frame-phase ordering.** `frame-phases.md` row 6 places extract sequentially after phase 5 and before phase 7. Concurrency between sim and render is achieved by the **one-frame pipeline model** (`reviews/decisions/perf-budget.md` §"Pipelined-frame timing model"): frame N+1's phases 1..5 run while frame N's GPU executes (frame N's phase 7 has already submitted). There is no free-running extract worker; phase 6 runs once per tick on the driver thread and exits before phase 7 starts (within the same frame's CPU window). The CPU budget admits this: 0.5 ms phase 6 + 1.0 ms phase 7 = 1.5 ms total (`SPEC.md` §9.1, §9.2, §9.3). |
| Harmonius design — `MaterialManager` on render thread owns `Material` instances; extract reads them. | **Routed.** `MaterialHandle` is a 64-bit generational handle (`SPEC.md` §5 `Handle<tags::material>`); the *table* mapping handles to GPU-side material parameter blocks lives in `material` plugin (`SPEC.md` §3.3 cited refusal). Extract carries the handle; phase 7's `passes/lighting.cpp` and `passes/transparent_forward.cpp` resolve it through bindless argument buffers (`resources/argument_buffer.cpp`). |
| Harmonius design — Per-archetype ECS query parallelism in extract. | **Covered, structurally.** §4 below: the extract walk is logically a span of disjoint per-archetype reads (renderable archetype, light archetype, camera archetype, probe archetype). The driver-thread sequential loop is what the budget bakes in; **per-archetype parallelism is admitted as a future intra-phase-6 optimisation** if profiling shows the 0.20 ms meshlet-cull slice or the 0.03 ms finalise slice over budget. The structure (read-only on each archetype, write to disjoint SoA columns) supports parallel-for over archetypes without locks; the budget is not yet starved enough to require it. Listed in §12. |

Net result: every R-2.10.* and adjacent extract-relevant requirement /
design clause is either implemented as specified below or explicitly
refused with rationale. The harmonius "extract → ProxyStore → prepare →
render" four-stage pipeline collapses into two glibre frame phases (6
and 7) with one snapshot type at the seam, per `SPEC.md` §3.2 collapse
#4 ("one structure crosses the ECS↔GPU seam, not many").

## 3. Detailed model

### 3.1 The aggregate cluster

```
RenderFrame slot pool                     (process-lifetime; size = 3)
└── eastl::array<RenderFrameSlot, 3>      (triple-buffered)
    └── RenderFrameSlot
        ├── arena_           : TransientArena      (per-slot bump allocator)
        ├── frame_           : RenderFrame         (the snapshot; arena-allocated)
        ├── pin_count_       : std::atomic<u32>    (observers; SPEC §8.5)
        └── generation_      : std::uint32_t       (FrameHandle generation)

RenderFrame                                  (per (World, FrameCounter); arena-allocated)
├── header_              : RenderFrameHeader      (frame_counter, alpha, settings hash)
├── views_               : eastl::span<const ExtractedView>
├── renderables_         : RenderableProxySoA     (per-renderable columns, see §3.2)
├── lights_              : eastl::span<const ExtractedLight>
├── cameras_             : eastl::span<const ExtractedCamera>      (one per ExtractedView)
├── render_settings_     : RenderSettings         (snapshot of §5 RenderSettings)
├── render_layer_table_  : eastl::span<const RenderLayerMask>      (per ExtractedView)
└── per_view_visible_    : eastl::span<const PerViewVisibleSpan>   (filled by cull)

RenderableProxySoA                                (parallel-array layout in arena)
├── interp_transform_    : eastl::span<glm::mat4>            (lerp(prev, curr, alpha))
├── velocity_            : eastl::span<glm::mat4>            (curr - prev for motion vec)
├── world_aabb_          : eastl::span<AABB>                 (consumed by cull)
├── meshlet_offset_      : eastl::span<std::uint32_t>        (offset into geometry's table)
├── meshlet_count_       : eastl::span<std::uint32_t>
├── mesh_handle_         : eastl::span<MeshHandle>
├── material_handle_     : eastl::span<MaterialHandle>
├── render_layer_        : eastl::span<RenderLayerMask>
├── sort_key_slot_       : eastl::span<std::uint64_t>        (reserved; filled by cull/sort.cpp)
└── count_               : std::uint32_t

ExtractedView                                     (one per active View)
├── view_handle_         : ViewHandle
├── camera_              : ExtractedCamera        (proj, view, jitter)
├── viewport_            : Viewport               (origin + extent)
├── layer_mask_          : RenderLayerMask
└── settings_subset_     : RenderSettingsViewBits (per-view tier overrides)

ExtractedLight  { handle, kind, world_pos, color, intensity, range, layer_mask, shadow_tier }
ExtractedCamera { proj, view, prev_view, jitter, near, far, aperture, sensor }
```

The slot pool is process-lifetime; each `RenderFrameSlot` holds one
arena (256 KiB sized in §9 below) plus the snapshot it produces. Slots
rotate generationally per frame (§3.6 below). The `RenderFrame`
aggregate published to `SPEC.md` §5 is the **frozen view** of one slot's
contents; callers see only the public methods (`frame_counter()`,
`views()`, `pin()`).

The internal split — slot pool + arena + snapshot — is purely an
implementation decomposition, not an ABI seam. `SPEC.md` §4.1.1 names
`RenderFrame` as one aggregate because the slot machinery and the
snapshot share invariants (immutability post-build, self-contained,
bounded, stable under one-frame pipelining). The split here lets the
extract walk write into the arena without violating the immutability
contract: the snapshot is mutable until phase-6 exit (`finalise()`
flips a one-bit guard), and `const` thereafter.

### 3.2 Extracted record taxonomy

The snapshot carries six MVP record kinds; **the list is closed** for
MVP. Adding a kind is a §3.2 amendment plus a `SPEC.md` §4.1.1
composition update.

#### 3.2.1 `RenderableProxy` (SoA)

The hot record. One row per visible mesh-shaped instance at the
post-cull boundary. SoA layout per `SPEC.md` §3.2 collapse #4 (proxy
SoA mirrors GPU shape) and the harmonius R-2.10.2 "GPU-needed only"
rule. Columns are listed above; cardinality is the cull survivor count
per frame (S1: ≤3 200 entries × visibility ≈ ≤2k after cull). Each
column is a contiguous span allocated up-front from the arena; the
extract walk fills row `i` across all columns before moving to row
`i+1`, so cache locality is row-wise during the walk and column-wise
during the cull walk that consumes it.

The columns admitted into MVP are exactly those phase 7 needs:

- `interp_transform_` (mat4) — already-interpolated world matrix; no
  prev/curr split exposed to phase 7.
- `velocity_` (mat4) — `curr - prev` matrix, written to the velocity
  MRT by `passes/gbuffer.cpp`. Stored as a full mat4 to avoid a
  per-pixel reconstruction; the bytes cost is tolerable at S1 scale.
- `world_aabb_` (`AABB { glm::vec3 min, glm::vec3 max }`) — read by
  `cull/meshlet_cull.cpp`. Computed at extract from
  `LocalAABB ⊗ interp_transform_`.
- `meshlet_offset_`, `meshlet_count_` (u32, u32) — index into
  geometry's meshlet table. Resolved to the GPU-resident meshlet stream
  by phase 7.
- `mesh_handle_`, `material_handle_` (`Handle<...>`, 64-bit) —
  pass-through.
- `render_layer_` (`RenderLayerMask`, 32-bit) — per-renderable.
- `sort_key_slot_` (u64) — **reserved by extract, filled by cull**.

Refused columns (post-MVP, listed in §12): per-instance custom
parameters, vertex skinning weights, per-meshlet visibility flags
(extract emits the offset/count; meshlet-level visibility is computed
by `cull/meshlet_cull.cpp` against the meshlet stream), morph-target
indices.

#### 3.2.2 `ExtractedLight`

One row per ECS-resident light entity. Columns: `LightHandle`, `kind`
(`Directional / Point / Spot / Probe`), `world_pos` (vec3), `dir` (vec3,
`Directional / Spot`), `color` (vec3), `intensity` (float), `range`
(float), `inner / outer` (float, `Spot`), `layer_mask`
(`RenderLayerMask`), `shadow_tier` (`ShadowTier`). Cardinality bounded
by `RenderSettings.per_view_draw_budget` × 0.1 (S1: ≤8 dynamic lights
on the budget row).

Shadow-tier per-light is the per-light override of the global setting;
the cluster-light cull pass (`passes/cluster_cull.cpp`) reads it during
phase 7 (`SPEC.md` §6.2.2 step 1.3). Probe lights (R-2.4.24) are
included as `kind == Probe`; their cubemap handle is carried in a
`ProbeHandle` field (overloaded into `LightHandle` in the union arm).

#### 3.2.3 `ExtractedCamera` (one per `ExtractedView`)

Columns: `proj` (mat4, projection), `view` (mat4), `prev_view` (mat4,
for TAA jitter and motion vec stability), `jitter` (vec2, sub-pixel
offset for TAA — `passes/aa_upscale.cpp`), `near` (float), `far`
(float), `aperture` (float, DOF), `sensor` (vec2, DOF). The
`prev_view` is read from `PreviousGlobalTransform` on the camera entity
identically to renderables.

#### 3.2.4 `ExtractedView` (header per active view)

Columns: `view_handle_`, `camera_` (embedded `ExtractedCamera`),
`viewport_` (origin + extent), `layer_mask_` (`RenderLayerMask`),
`settings_subset_` (`RenderSettingsViewBits` — per-view overrides of
shadow tier, AA mode, draw budget). Multi-view fan-out emits one row
per active view from the same extract walk (split-screen × 2 = 2 rows;
shadow cascades = 4 rows; reflection probes = up to N rows; viewmodel
= 1 row).

#### 3.2.5 `RenderSettings` snapshot

The `RenderSettings` POD declared at `SPEC.md` §5 is copied by value
into the snapshot header. This is the hot-reload-stable subset
(§7 below): identical bytes across the swap because the type is owned
by `glibre-types` middleman; render reads them from `core`'s registry
at phase 6 entry.

#### 3.2.6 `PerViewVisibleSpan` (filled by cull)

One row per `(View, PassPhase)` pair. Columns: `view_index`,
`pass_phase` (one of `Opaque / AlphaTested / Translucent / Shadow / 2D
/ Capture`), `first_proxy_index`, `count`. **Reserved by extract,
filled by cull/budget.cpp + cull/sort.cpp**. Extract allocates the row
slots from the arena; cull writes the indices and counts after sorting
the survivor span. Phase-7 pass bodies index through these spans into
the renderable SoA.

### 3.3 Per-archetype walk

The extract walk is a fixed sequence of read-only ECS queries followed
by a per-record materialisation into the arena. All queries are
`World::view_const<...>()` overloads (the const-only version of the
ECS view; the type system refuses any mutating return). Sequence:

1. **Header.** Allocate the `RenderFrameHeader` from the arena. Stamp
   `frame_counter`, `alpha = (now - last_fixed_tick) / fixed_dt`,
   `world_id`. Copy `RenderSettings` snapshot. (~5 µs.)

2. **Views.** Walk the camera + viewport ECS archetype:
   `view_const<CameraComponent, GlobalTransform, PreviousGlobalTransform,
   ViewportComponent, RenderLayerMaskComponent>()`. For each row, build
   one `ExtractedView` + embedded `ExtractedCamera` (with interpolated
   view, prev_view, projection, jitter sourced from `RenderSettings`).
   The result is written contiguously into the snapshot's `views_`
   span. Cardinality at S1: 1 (one player camera). (~5 µs.)

3. **Renderables.** Walk the renderable archetype:
   `view_const<MeshComponent, MaterialComponent, GlobalTransform,
   PreviousGlobalTransform, LocalAABBComponent, RenderLayerMaskComponent>()`.
   Pre-allocate the SoA columns up-front against the archetype's
   row count (the upper bound; cull will compact). For each row,
   compute `interp_transform = lerp(prev, curr, alpha)`, `velocity =
   curr - prev`, `world_aabb = LocalAABB ⊗ interp_transform`, and
   write all eight columns at row index. Cardinality at S1: 200 × ~16
   meshlets ≈ 3 200 input proxies. (~150 µs — the dominant cost; the
   per-row `lerp + AABB transform` is SIMD-vectorisable, the budget
   in §9.2 has reserved.)

4. **Lights.** Walk the light archetype:
   `view_const<LightComponent, GlobalTransform, RenderLayerMaskComponent>()`.
   Cardinality at S1: ≤8 dynamic + N static (lights are ECS entities,
   §1.3 R-2.4.24). For each row, write one `ExtractedLight` row. (~5 µs.)

5. **Probes.** Walk the probe archetype (`ProbeComponent`,
   `GlobalTransform`). Cardinality MVP-bounded; emitted into the lights
   span with `kind == Probe`. (~2 µs.)

6. **Per-view cull hand-off.** For each `ExtractedView`, hand the
   renderable SoA + `view.layer_mask` to `cull/meshlet_cull.cpp` (§3
   refusal #4). Cull returns a survivor span into the SoA's row-index
   space; extract reserves the `PerViewVisibleSpan` row slot and fills
   `first_proxy_index / count` after cull returns. (Per-view cull cost
   is `cull`'s budget, not extract's.)

7. **Finalise.** Flip the slot's `frozen_` bit; publish the slot's
   `FrameHandle` onto the snapshot bus. Phase 6 exits; phase 7 takes
   the `const RenderFrame&`. (~3 µs.)

The per-step microbudgets above are the **walk's portion** of the 0.5
ms CPU phase-6 ceiling (`SPEC.md` §9.2). The 0.20 ms `cull/meshlet_cull`
slice and the 0.10 ms `cull/sort` slice are **cull's portion**, not
extract's. Extract's own budget at S1 is (5 + 5 + 150 + 5 + 2 + 3) ≈
170 µs ≈ 0.17 ms. The remaining slack inside phase 6's 0.5 ms ceiling
goes to cull (`SPEC.md` §9.2 row breakdown: extract walk ≈ 0.05 + cull
0.20 + budget 0.05 + sort 0.10 + finalise 0.03 = 0.43 ms; reserve 0.07
ms). The "extract walk" line in `SPEC.md` §9.2 is the §3.3 sequence's
0.05 ms slot — the non-cull-non-sort portion: header + views + lights
+ probes + finalise; the renderables walk (the 150 µs of step 3) is
folded into the same line because it is SIMD-vectorisable and dominated
by the meshlet-cull slice that runs over the same rows immediately
after.

### 3.4 Extract is read-only on the world

The extract walk uses **only** `World::view_const<...>()` overloads.
Compile-time enforcement: the const overload returns a tuple of
`const T&` references; any attempt to construct a `view<...>` (mutable)
inside the extract translation unit is a build error gated by an
`#error` in `cull/extract.cpp`'s prelude:

```cpp
#define GLIBRE_RENDER_EXTRACT_TU 1
#include "core/world.hpp"  // tag-dispatched: defines view_const only
                           // when GLIBRE_RENDER_EXTRACT_TU is set.
```

(The exact mechanism is the same `static_assert`-on-tag pattern used
elsewhere in the engine; the tag is a translation-unit-local define,
not a global flag — other render TUs may freely use mutable views
against the world the way that `tools` and `editor` do.)

Run-time enforcement: the ECS storage's component archetype tables are
accessed via a `const ArchetypeTable&` reference. The `view_const`
overload returns iterators that yield `const Components&...`; mutating
through one is a `const`-correctness violation, caught at compile time.

This composes with `SPEC.md` §4.2 invariant 6 ("`RenderFrame` is the
only ECS↔GPU seam"). Inside phase 6 render reads the ECS through
extract; outside phase 6 (i.e. in phase 7's pass bodies) render must
not call back. The compile-time gate is one obvious place to put the
contract.

### 3.5 Self-containment guarantee

Every byte phase 7 reads from the snapshot is reachable from the
`RenderFrame` root pointer (`SPEC.md` §4.1.1 invariant 2). Concretely:

- **No ECS pointers escape into the snapshot.** Component references
  read during the walk are dereferenced into the arena
  (transforms / AABBs / handles / masks copied by value) before the
  walk advances. The arena's bytes are owned by render's per-frame
  arena allocator, not by ECS storage.
- **No GPU handles are dereferenced.** `MeshHandle`, `MaterialHandle`,
  `BLASHandle`, `ProbeHandle`, `RenderLayerMask` are 64-bit POD values
  per `SPEC.md` §5; extract carries them as integers. Phase 7 resolves
  the integers against the residency tables (`PSOCache`,
  `RTAccelStructures`, `material` plugin's table).
- **No sibling-context bus access from phase 7.** Phase 7 reads the
  snapshot only; if a pass needs additional per-frame data (e.g.
  vfx-vended particle buffers), the data was extracted into the
  snapshot in phase 6 by a *registered extract hook* (post-MVP — the
  MVP record taxonomy is the closed §3.2 list).

The S1 fixture's snapshot at peak occupancy: `header` (96 B) + `views`
(1 × 256 B = 256 B) + `lights` (8 × 96 B = 768 B) + `cameras` (folded
into views, 0 B extra) + `renderables` SoA (3 200 rows × ~256 B/row =
820 KiB) + `per_view_visible` (1 view × 6 phases × 16 B = 96 B) ≈
**~821 KiB peak**. The per-slot arena budget is 1 MiB (§9 below);
slack of ~200 KiB absorbs growth headroom.

### 3.6 Slot pool — triple-buffered, generational

The slot pool is a fixed `eastl::array<RenderFrameSlot, 3>` rotated
generationally. At any frame `N`, the slots' roles are:

```
Slot index       Generation         Role at frame N
─────────────────────────────────────────────────────────────
(N + 0) mod 3    G                  WRITE — phase 6 of frame N
(N − 1) mod 3    G − 1              READ  — phase 7 / phase 9 of frame N − 1 (still in flight)
(N − 2) mod 3    G − 2              FREE  — retired; arena reset; ready for frame N + 1
```

`FrameHandle` (`SPEC.md` §5 `Handle<tags::frame>`) packs (24-bit
generation, 40-bit slot index). In the 3-slot MVP pool, only slot
indices 0, 1, and 2 are used; bits [2..39] of the slot field are
always 0. The generation bumps on each acquisition; a stale handle
references a generation that has been overwritten and `valid()` returns
false against the slot's current generation.

Acquisition path (`cull/extract.cpp`):

```cpp
auto acquire_slot(SlotPool& pool, std::uint64_t frame_counter)
    -> Result<RenderFrameSlot*>;
```

The function selects the slot whose role is `FREE` (the `(N-2) mod 3`
slot) and bumps its generation. If no slot is free — i.e. phase 9 of
frame `N-2` has not retired yet because the GPU is wedged — the
function returns `render::Error::FenceTimeout`, mapped per §10 below
to `abort-frame` (the previous frame is re-presented; the next frame
retries). This is the phase-6 leg of the `GpuTimeout` /
`PresentTimeout` recovery path (`SPEC.md` §10.3).

Retire path (`SPEC.md` §6.2.2 step 4 + frame-phases row 9): when
phase 9 finishes presenting frame `N-1`, the slot for `(N-1) mod 3`
flips from `READ` to `FREE`. Reset is a single `arena_.reset()` call —
O(1) bump-pointer rewind, not byte-zeroing.

Pin counts (`SPEC.md` §8.5): observers (editor, e2e harness, tools
profiler) may pin a `RenderFrame` for diagnostic capture. A non-zero
`pin_count_` keeps the slot in `READ` role; phase 6 selects the next
free slot or returns starvation. The pin path is the §8 hot-reload
deferral mechanism; it does **not** widen the slot pool to 4. Three is
the contractual maximum, and the test fixture
`tests/render/extract/pin_then_starve.cpp` asserts that two
simultaneous pins force phase 6 to defer for one frame.

### 3.7 Snapshot bus

The "snapshot bus" is shorthand for **`core`'s phase-6→phase-7 reference
hand-off**. It is not a separate data structure; it is the ordinary
inter-phase argument-passing inside `core`'s frame loop:

```
phase 6 returns Result<FrameHandle>            (render's exit)
phase 7 entry receives FrameHandle             (render's entry, second)
phase 7 dereferences FrameHandle to const RenderFrame&  (the snapshot)
```

The dereference is a single table lookup into the slot pool with the
generation check. The cost is ~5 ns; it is folded into the 0.10 ms
`graph/builder.cpp` register cost in `SPEC.md` §9.3.

`SPEC.md` §6.2.1 step 3 names this the "snapshot bus delivers a
`const RenderFrame&` to phase 7". The §5 entry `submit_frame(MetalDevice&,
const RenderFrame&)` is the public C++ surface; internally the
`FrameHandle` resolution happens in `core`'s frame-loop driver,
not in render's plugin.

## 4. Public surface

This aggregate **introduces no new symbols** beyond `SPEC.md` §5. The
header stub already publishes:

```cpp
class RenderFrame {
public:
    [[nodiscard]] std::uint64_t                 frame_counter() const noexcept;
    [[nodiscard]] eastl::span<const ViewHandle> views()         const noexcept;
    // (pin/unpin RAII guard documented in §8.5; not yet in §5 stub.)

    ~RenderFrame();
    RenderFrame(const RenderFrame&)            = delete;
    RenderFrame& operator=(const RenderFrame&) = delete;

protected:
    RenderFrame() noexcept;
};

[[nodiscard]] Result<PresentFence>
    submit_frame(MetalDevice&, const RenderFrame&) noexcept;
```

The opaque is enough for callers; phase-7 internals dereference the
snapshot through implementation-side accessors that live in
`render/src/frame/render_frame.hpp` (a private header consumed only by
sibling render TUs). The private accessors are *not* an ABI seam; they
are render-internal.

The §5 stub does not yet name a `RenderFrame::pin() noexcept`
RAII-guard surface; `SPEC.md` §8.5 ("Observers — external systems
holding `RenderFrame`") describes the semantics and notes the symbol is
"published in §5". This design **adds the pin/unpin pair to §5 in the
same ABI bump that lands the snapshot machinery** (the four "ABI add"
rows in `SPEC.md` §10.1 are the precedent — §5 acquires deltas in
controlled bumps). The proposed additions:

```cpp
class RenderFrame {
public:
    // ... existing surface ...

    // RAII pin: prevents the slot from retiring at phase-9 while the
    // guard is alive. Honoured by render's hot-reload protocol per §8.5
    // (deferral, not refusal). Held by editor / e2e harness / profiler.
    class [[nodiscard]] PinGuard {
    public:
        PinGuard(PinGuard&&) noexcept;
        ~PinGuard();
        PinGuard(const PinGuard&)            = delete;
        PinGuard& operator=(const PinGuard&) = delete;
    private:
        explicit PinGuard(const RenderFrame*) noexcept;
        friend class RenderFrame;
        const RenderFrame* frame_ = nullptr;
    };

    [[nodiscard]] PinGuard pin() const noexcept;
    [[nodiscard]] std::uint32_t pin_count() const noexcept;  // diagnostic only
};
```

Adding these is the §5 ABI bump that lands alongside this design's
implementation. No other render aggregate needs new public symbols.

The plan-leaf author for this aggregate may **not** add public symbols
beyond the pin pair without a separate amendment spike.

## 5. Hot / cold path split

| Path                       | Frequency                    | Owner                                                                                                                                                           |
|----------------------------|------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Snapshot construction (extract walk) | **Hot — every frame, phase 6.** | §3.3 above. Driver thread, sequential. Read-only on ECS; write-only into the arena. Budget ≈ 0.17 ms at S1 (`SPEC.md` §9.2 line "extract walk + finalise" rolled up). |
| Slot acquisition + retire  | Hot — twice per frame.       | §3.6 above. Two atomic operations per frame (acquire = generation bump; retire = arena reset signal). Cost ~50 ns total.                                        |
| Pin / unpin                | **Cold — observer-driven.** Editor / e2e / tools open and close pins around a frame inspection. Not on the per-frame hot path. | §3.6, §4 above. Atomic increment / decrement on `pin_count_`.                                                                                                   |
| Per-archetype subscription | **Cold — init / hot-reload register.** Render's `glibre_plugin_register` calls `World::subscribe_archetype<...>()` once per archetype the extract walk reads. The subscription installs the const-iterator factory in the ECS query cache so step 3.3 can iterate without re-walking the archetype table from scratch. | `SPEC.md` §8.3 + §6.1 (`plugin.cpp` register body).                                                                                                              |
| Arena resize policy        | Cold — process init.         | §9 below. The 1 MiB-per-slot arena is sized at process init from `RenderSettings.per_view_draw_budget × 4`. Out-of-arena returns `render::Error::TransientPoolExhausted` per §10 below. |
| Schema migration of the snapshot bytes | **N/A — never serialised.** | §7 below. The snapshot has no on-disk form; PHILOSOPHY anti-pattern.                                                                                            |

The split rule (`SPEC.md` §6.1): one file per reason-to-change. Extract
ships as `render/src/cull/extract.cpp` (the walk, hot) +
`render/src/frame/render_frame.cpp` (the slot pool + arena, hot) +
`render/src/frame/render_frame_observer.cpp` (the pin RAII guard,
cold). The subscription-time work lives in `render/src/plugin.cpp`'s
register hook.

## 6. Concurrency

### 6.1 Driver thread, sequential body

Phase 6 runs on the **driver thread only** (`SPEC.md` §6.3:
"Phase 6 is driver-thread only"). The extract walk is a single
sequential function body inside `cull/extract.cpp`. There are no
worker threads, no thread pools, no fan-out inside phase 6.

Concurrency between sim and render is *across* phase boundaries
(pipelined-frame model from `reviews/decisions/perf-budget.md`):

```
Frame N+1 phases 1..5  ─── driver thread (sim) ───── 7.5 ms budget
Frame N   phase 7     ─── driver + 3 encoder workers ── 1.0 ms driver-side
Frame N   GPU         ─── overlaps frame N+1 sim ───── 8.0 ms
```

Phase 6 of frame N+1 sits between sim (phases 1..5) and the launch of
phase 7 of frame N+1. It does not run concurrently with sim of frame
N+1 (sim has finished); it does not run concurrently with phase 7 of
frame N (phase 7 of frame N has already submitted, by the pipelined
model). The extract walk is therefore a sequential body that briefly
holds the driver thread and produces the snapshot in 0.5 ms.

### 6.2 Read-only on the world; parallel-safe by construction

Although the body is sequential in MVP, it is **read-only on every ECS
storage it touches** (§3.4 above), and writes to **disjoint SoA
columns** in the arena (different rows = different memory, never
shared). The walk could be parallelised over archetypes (one task per
archetype) without locks: each archetype writes a different region of
the arena (renderable SoA is a different column block from the
`ExtractedLight` span, which is different from the `ExtractedView`
span). An archetype's rows are written sequentially within one task.

Per-row parallelism inside the renderable archetype (the dominant
cost) is also lock-free — each row writes a different SoA-column slot.
A `parallel_for` over rows in step 3.3.3 with the bare driver-thread
body factored as a single-pass functor is a **future amendment**: it is
not in MVP because the 0.17 ms walk fits the budget at S1, but the
structure admits it (§12 open question).

### 6.3 Pin / unpin atomicity

`pin_count_` is `std::atomic<u32>`. `pin()` is a single
`fetch_add(1, std::memory_order_acq_rel)`; `~PinGuard()` is a single
`fetch_sub(1, std::memory_order_acq_rel)`. The retire path (`SPEC.md`
§6.2.2 step 4) reads the count under `memory_order_acquire`; if
non-zero, retire defers the `arena_.reset()` until the count reaches
zero.

The pin contract is **frame-scoped only**. An observer that pins a
frame N snapshot must release the pin before phase 9 of frame N+2 (=
when slot `N mod 3` would otherwise become `FREE` for frame N+3). A
pin that lives longer than that causes the slot-starvation condition:
`acquire_slot` returns `render::Error::FenceTimeout` on the next
acquisition — listed as a refusal under `SPEC.md` §10 `abort-frame`
and tested by `tests/render/extract/pin_overlong_starvation.cpp`.

## 7. Persistence + ABI

### 7.1 The snapshot persists nothing

`SPEC.md` §5 already publishes "Serialised schemas (Fory). None at
this layer. `RenderFrame` lives in the per-frame arena and is never
serialised (PHILOSOPHY anti-pattern 'serialised render-graph
files')." This design honours that verbatim.

Concretely:

- No `RenderFrame*.fory` schema. The fory codegen
  (`reviews/decisions/fory-codegen.md`) does not enumerate any
  render-frame-extract type because no type lives across the phase 7→9
  boundary. Codegen for the snapshot's record types is unnecessary —
  they are POD assembled in-process from middleman types
  (`RenderSettings`, `MaterialHandle`, `MeshHandle`, etc.) that already
  have schemas.
- No on-disk artefact. The diagnostic capture path (`SPEC.md` §10.4
  `GpuFault`) writes a `FaultDiag` blob (a different middleman type),
  not the snapshot.
- No editor / e2e capture format. When the editor pins a snapshot
  for inspection, it walks the snapshot in-process through the §4 pin
  guard; nothing is serialised.

### 7.2 ABI surface

The render-frame-extract aggregate's ABI is exactly the §4 surface
(`RenderFrame` opaque + `submit_frame` entry + the `PinGuard` RAII
pair). The plugin manifest exposes it through render's existing
manifest entries:

- `RenderFrame` is referenced by handle (`Handle<tags::frame>`); the
  handle is a 64-bit POD with no dylib-resident pointers. ABI-stable
  across reload (no fix-up needed).
- The opaque class layout is dylib-private; callers see only the
  forward declaration in `glibre/render/render.hpp`. ABI-stable: a
  layout change inside the dylib does not affect callers.
- The `PinGuard` is a 16-byte POD wrapping a `const RenderFrame*`
  (8 B) + a generational tag (8 B) for safety. A cross-reload-pinned
  guard is invalidated at the swap (per §8.5); no UB risk.

The plugin manifest's `glibre_plugin_register` entry adds:

```yaml
contributes:
  systems:
    - phase: 6  # cull-extract
      fqn: glibre.render.cull.extract
      function: cull_extract_run
  archetypes:
    - glibre.render.renderable
    - glibre.render.light
    - glibre.render.probe
    - glibre.render.camera_view
```

The four archetypes are *subscribed*, not *owned*: render reads them
const-only in phase 6. Ownership stays with the gameplay-side plugin
that authors them (typically `core` in MVP).

### 7.3 Allocator tagging

Per `reviews/decisions/perf-budget.md` Allocator Rule 4 (transient
arena exemption), the per-slot arena is **exempt from the 512 MiB
ceiling**. The arena drains at phase 9 of the *next-but-one* frame
(slot `(N) mod 3` is reset at frame N+2's phase 9 entry, when it flips
from `READ` to `FREE`). The drain is a `bump_pointer = base`
operation; no per-allocation deallocation runs. Strict-mode CI
(`GLIBRE_ALLOC_STRICT=1`) asserts that the arena is empty at phase 9
entry of frame N+2 (`SPEC.md` §9.6.2 benchmark
`transient-pool drained at phase 9, strict`).

The 1 MiB-per-slot arena × 3 slots = **3 MiB** sit under
`ContextTag::render` (CPU side, not GPU). Three megabytes is small
relative to the 512 MiB cell ceiling and is **inside** the cell's
"GPU resource handles" + "PSO cache" rows — no separate row needed
because the arena is drain-or-leak transient, fully exempted by Rule 4.

## 8. Hot-reload

### 8.1 Reload point — phase 8, never mid-frame

Per `SPEC.md` §8.1 and `reviews/decisions/hot-reload-protocol.md` step
1, render's reload barrier is phase 8. At phase 8 entry, the
render-frame-extract aggregate's state is:

1. **No extract walk is running.** Phase 6 of frame N has finished
   (the snapshot is built and consumed); phase 6 of frame N+1 has not
   yet started (phase 8 sits before phase 6 of the next frame). The
   driver thread is not inside `cull_extract_run`.
2. **All `RenderFrame` slots in `READ` role have been retired.** Phase
   7 of frame N has submitted; the snapshot for frame N is no longer
   the active read target. Slot `N mod 3` is in `FREE` role; slot
   `(N-1) mod 3` is in `FREE` role; slot `(N-2) mod 3` was already
   `FREE`. (Pinned slots defer reload — §8.5 below.)
3. **The arena is empty in every slot.** Each of the three arenas was
   reset at its respective phase-9 retire step. Total live bytes
   under `ContextTag::render`'s arena tag: 0.

These three conditions match `SPEC.md` §8.1 ("`RenderFrame` for frame
N is destroyed") with the additional precision that the slot pool
itself survives the swap (its bytes are render-private but no reload
attempts to mutate them).

### 8.2 Survival inventory

Mapped against `SPEC.md` §8.2's table:

| State                                | Persistence path | Survives swap? | Reasoning                                                                                                                                                                                                                                                       |
|--------------------------------------|------------------|----------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `RenderFrame` slot pool (3 slots)    | None             | **Yes.**       | Lives in render-plugin private memory. The slot table's keys are slot indices (0/1/2) — stable middleman values, not pointers. Bytes survive because the loader holds exclusive ownership during phase 8 and the new plugin's `glibre_plugin_register` rebinds the slot-pool factory pointer (analogous to §8.3.1 "swap the render-graph builder"). |
| Per-slot arena (3 × 1 MiB)           | None             | **Yes (empty).** | Arena bytes survive but are empty at phase 8 (every slot has retired). The new plugin's first phase-6 reuses the existing arenas — they are bump-pointer allocators, no internal state survives a reset.                                                       |
| `FrameHandle` generations            | None             | **Yes.**       | Generations are 24-bit counters. They survive across the swap because they are stored in slot bytes (not in dylib code). A stale `FrameHandle` from before the swap remains stale after; a fresh `FrameHandle` issued by the new plugin is post-swap-valid.    |
| Pin guards held by observers (§8.5)  | None             | **Yes (deferred).** | If a pin is held at phase 8 entry, the reload defers (§8.5 below), not refuses. The guard's underlying pointer + generation tag remain valid because the slot pool survives. |
| Extract walk subscription registrations (4 archetypes) | None | Re-registered on swap. | The subscriptions are dropped by the old plugin's `glibre_plugin_drain` (§8.3 below) and re-installed by the new plugin's register. The ECS storage's archetype tables are owned by `core` and are unaffected. |
| `RenderFrameSlot::frame_counter_`    | None             | **Yes.**       | A `u64` per slot; it survives because the slot's bytes survive. The next phase 6 stamps the next counter; no resync required.                                                                                                                                  |

The rule mechanically applied: every "Yes" row has bytes owned by
middleman types or by render-private memory that the swap mechanically
preserves; no row has a `.fory` schema; no row would survive bytewise
across a hot-reload that *targeted* render-frame-extract specifically
(this aggregate is one piece of the render plugin, not a separately
swappable artefact). The whole render plugin reloads as one unit.

### 8.3 `glibre_plugin_drain` — render's responsibilities

Per `hot-reload-protocol.md` step 1, the outgoing plugin's
`glibre_plugin_drain(World&)` runs synchronously on the driver thread
at phase 8. For the render-frame-extract aggregate, drain does:

1. Walk the slot pool. For each slot in `READ` role, read
   `pin_count_` atomically (no blocking wait — the driver thread must
   not spin or sleep; see `reviews/decisions/hot-reload-protocol.md §"Step 1 — Drain"`). If any slot carries a
   non-zero pin count, drain **returns immediately** with
   `core::Error::HotReloadRefused` (the one-frame deferral path
   described in §8.5 below). The loader's `pending_reloads` counter is
   not decremented; the request re-queues for the next phase 8. No
   slot is retired and no subscription is released in the deferred
   case — steps 2 and 3 are skipped until pin_count_ reaches zero on a
   subsequent attempt.
2. For each `READ`-role slot whose `pin_count_` is zero, retire it
   (arena reset, role flip to `FREE`).
3. Release the four archetype subscriptions: call
   `World::unsubscribe_archetype<...>()` for renderable, light, probe,
   and camera_view archetypes. The ECS retains the archetype storage
   itself; only the iterator factory is dropped.

What drain does **not** do:

- Does **not** free the slot pool memory. The pool's bytes survive
  the swap; the new plugin reuses them.
- Does **not** zero the arenas. They are already empty (every slot
  was retired in step 2).
- Does **not** publish any final snapshot. Phase 8 is the gap between
  phase 7 of frame N and phase 6 of frame N+1; there is no snapshot
  to publish.

### 8.4 `glibre_plugin_register` — the new plugin's resume responsibilities

Per `hot-reload-protocol.md` step 4 (resume) and `SPEC.md` §8.3.1, the
new plugin's `glibre_plugin_register(World&, allocator)`:

1. **Re-registers the four archetype subscriptions**:
   `World::subscribe_archetype_const<MeshComponent, MaterialComponent,
   GlobalTransform, PreviousGlobalTransform, LocalAABBComponent,
   RenderLayerMaskComponent>()`, plus the light, probe, camera_view
   archetype subscriptions.
2. **Rebinds the slot-pool factory pointer**: the new plugin's image
   exports the post-swap entry point for `cull_extract_run`. The
   render registry's slot-pool entry is patched atomically (single
   store under loader exclusive ownership).
3. **Does not pre-walk the world.** Per
   `hot-reload-protocol.md` §"Decision" ("no system bodies run in
   phase 8"), register does not invoke `cull_extract_run`. The first
   walk happens at the next phase 6 of the next frame.

### 8.5 Pinned-frame reload deferral

`SPEC.md` §8.5 specifies that a non-zero pin count defers (not refuses)
the reload. Render-frame-extract is the aggregate that owns the pin
state. The contract:

1. At phase 8 entry, drain reads `pin_count_` for every slot. A
   non-zero count anywhere triggers a one-frame deferral: drain returns
   `core::Error::HotReloadRefused` (the flat enumerator for a reload
   that was refused — here used to signal the deferred state before
   the 3-attempt escalation; see `SPEC.md` §8.4 and
   `reviews/decisions/error-model.md` §Type Sketch for the flat
   `core::Error` enum — there is no nested `PinDeferred` variant).
2. The loader's `pending_reloads` counter is **not** decremented; the
   request stays in the queue. The next phase 8 (one frame later)
   re-attempts.
3. The `DiagnosticOverlay` surfaces the deferral as a `progress` line
   ("hot-reload deferred — pinned frames: 1"), not a `warn`. The
   editor's perf HUD shows the same.
4. If the pin is held for more than 3 consecutive phase-8 attempts,
   the loader escalates to `core::Error::HotReloadRefused` proper
   (per the protocol's drain-timeout rule). At that point, render's
   reload is refused and the prior plugin remains live; the operator
   must close whichever observer is holding the pin. The 3-attempt
   threshold is loader-side policy: the loader tracks the attempt
   count externally in its per-request state; drain always returns
   the same `core::Error::HotReloadRefused` enumerator on deferral
   and on final refusal — the loader uses its own counter to
   discriminate deferral (re-queue) from refusal (drop request).

The pin-deferral path is exercised by the test fixture
`tests/render/extract/pin_defers_reload.cpp` (§11 below).

### 8.6 Carrying-state-empty guarantee

The §1 declaration "snapshot rebuilt fresh next frame; carrying-state-
empty" means: at phase 8 exit, the new plugin's slot pool contains zero
live snapshot data. The next phase-6 walk (next frame) writes a fresh
snapshot from scratch into one of the three slots; phase 7 reads it.
Cross-reload determinism is therefore trivial: there is no carrying
state to mismatch.

This is the strongest possible hot-reload contract for an aggregate.
It is what `SPEC.md` §4.1.1 invariant ("destroyed at phase 7 exit") and
`SPEC.md` §8.1 ("phase 8 sees no live snapshot") together imply when
specialised to render-frame-extract.

## 9. Performance

### 9.1 Cell — extract's portion of phase 6

`SPEC.md` §9.2 publishes the phase-6 CPU breakdown. Render-frame-extract
owns the **non-cull-non-sort portion** of that 0.5 ms ceiling — i.e.
the sum of:

| Step (§3.3 reference)                                | Ceiling (S1, p99) | Notes                                                                                                            |
|------------------------------------------------------|-------------------|------------------------------------------------------------------------------------------------------------------|
| Slot acquisition + arena reset (§3.6)                | 0.02 ms           | Generation bump + bump-pointer rewind. (`SPEC.md` §9.2 line "open `RenderFrame` slot".)                          |
| Header + `RenderSettings` snapshot (§3.3 step 1)     | 0.005 ms          | One `memcpy` of the settings POD + counter / alpha stamps.                                                       |
| Views walk (§3.3 step 2)                             | 0.005 ms          | One row at S1; budget admits up to 4 rows for split-screen / VR.                                                 |
| Renderables walk (§3.3 step 3)                       | 0.15 ms           | The dominant slice. ~3 200 input rows × (lerp transform + AABB transform + 8 column writes).                      |
| Lights walk (§3.3 step 4)                            | 0.005 ms          | ≤8 rows at S1.                                                                                                   |
| Probes walk (§3.3 step 5)                            | 0.002 ms          | Bounded by MVP scene cap.                                                                                        |
| Per-view cull hand-off (§3.3 step 6)                 | (cull's slice)    | The cull `meshlet_cull` + `budget` + `sort` slices are budgeted under `cull` (`SPEC.md` §9.2 cull-aggregate row). Not counted in extract's column. |
| Finalise + snapshot bus emit (§3.3 step 7)           | 0.003 ms          | Frozen-bit flip + `FrameHandle` publish. (`SPEC.md` §9.2 line "RenderFrame finalise".)                            |
| **Render-frame-extract subtotal**                    | **~0.20 ms**      | At S1, p99. Inside phase 6's 0.5 ms ceiling.                                                                      |

The `SPEC.md` §9.2 breakdown lists `cull/extract.cpp` as a 0.02 ms
"open RenderFrame slot" line plus a 0.03 ms "RenderFrame finalise"
line; the **renderables walk in §3.3 step 3** is folded into the same
0.20 ms `cull/meshlet_cull.cpp` line in §9.2 because the loop is
fused at MVP — extract writes the SoA row and meshlet-cull reads it
back immediately; the implementation runs as one tight loop body
spanning both files, with cache-resident rows.

The "fold" is a budget-allocation choice, not an SRP violation: the
**source files** are split (extract authors columns; meshlet-cull
filters them) and reviewable independently; the **runtime cost** is
charged to the meshlet-cull line because it dominates the per-row work
and the two read/write the same cache line. If profiling shows the
walk dominates the cull cost (post-MVP scenes with low meshlet-cull
rejection rate), the budget rows split.

### 9.2 Per-context heap cell for the snapshot arena

Per §7.3 above, the **3 × 1 MiB slot arenas** sit under
`ContextTag::render`'s **CPU-side** budget — they are tagged because
the allocation path goes through `glibre::PerContextAllocator`, but
they are **exempt** from the 512 MiB cell ceiling per Allocator Rule 4
(transient arena exemption). The 3 MiB total is also negligibly small
relative to the cell.

Sizing rationale (S1):

```
peak snapshot ≈ header (96 B)
              + views (4 × 256 B)               = 1 KiB
              + lights (8 × 96 B)               = 768 B
              + cameras (folded into views)
              + renderables SoA
                (3 200 rows × ~256 B/row)       = 820 KiB
              + per-view-visible
                (4 views × 6 phases × 16 B)     = 384 B
              + slack for arena alignment        = 200 KiB
              ─────────────────────────────────
              total                             ≈ 1 MiB per slot
```

The 256 B/row figure for the renderable SoA is a row-major upper bound
(in practice the SoA columns are summed: 64 + 64 + 24 + 4 + 4 + 8 + 8
+ 4 + 8 ≈ 188 B/row plus alignment padding). The 1 MiB ceiling has
~200 KiB headroom. If profiling at scale shows the headroom shrinking,
the next perf-budget amendment widens the per-slot arena to 2 MiB
(× 3 slots = 6 MiB total — still negligible against 512 MiB cell).
**Out-of-arena returns `render::Error::TransientPoolExhausted`** per §10
below; tested by `tests/render/extract/arena_exhausted_lower_tier.cpp`.

`reviews/decisions/perf-budget.md` Allocator Rule 4 ("transient arena
exemption — drains at phase 9") is honoured: each arena resets at the
phase-9 retire step. CI runs the diagnostic build under
`GLIBRE_ALLOC_STRICT=1` and the gate
`BENCHMARK("transient-pool drained at phase 9, strict")` (`SPEC.md`
§9.6.2) catches any leak. The `extract` walk's TU is one of the call
sites that tag-stamps; the extract-arena allocator is constructed at
plugin register from the parent allocator handle (`SPEC.md` §9.5.1
Allocator Rule 1).

### 9.3 CI gate — extract-specific

`SPEC.md` §9.6 already names the phase-6 / phase-7 / heap / drain
benchmarks. Render-frame-extract adds **no new gate row**; its work is
inside the existing `BENCHMARK("phase-6 cull-extract S1, p99")`. The
acceptance criteria in §11 below add three Catch2 unit tests under
`tests/render/extract/` that assert the per-record-kind extraction
correctness without piggybacking on the phase-6 benchmark.

## 10. Failure modes

Every fallible operation in the extract walk returns
`std::expected<T, glibre::Error>` per
`reviews/decisions/error-model.md`. The closed sum is `render::Error`
(`SPEC.md` §10) plus three `core::Error` arms reachable from extract.
This aggregate **adds no new `render::Error` variants**; the existing
sum carries the load.

### 10.1 Variant-by-variant

| Condition                              | Trigger (extract walk)                                                                                              | §10 routing in `SPEC.md`                       | Recovery (per `SPEC.md` §10.2)                          | Test fixture (under `tests/render/extract/`)  |
|----------------------------------------|---------------------------------------------------------------------------------------------------------------------|-----------------------------------------------|---------------------------------------------------------|-----------------------------------------------|
| `ArenaExhausted`                       | `RenderFrameSlot::arena_.allocate(N)` returns `nullptr` because the bump pointer would exceed the per-slot 1 MiB limit. The renderable SoA is the dominant consumer; a scene whose visible-set or meshlet-bounds count exceeds the §9.2 sizing trips this. | Maps to **`render::Error::TransientPoolExhausted`** (existing `SPEC.md` §10 enum). Different aggregate (resources/transient_pool, the GPU-side pool); same enumerator because the arena is the CPU-side mirror of that resource role. The trigger column in `SPEC.md` §10.3 row `ResourceAllocFailed` is widened to include the snapshot arena via this design. | `lower-tier` — drop `RenderSettings.quality_tier` one step; the next frame's per_view_draw_budget shrinks; cull retains fewer rows; arena fits. | `arena_exhausted_lower_tier.cpp`              |
| `MissingComponent`                     | Walk step 3 encounters an entity in the renderable archetype whose `MaterialComponent` is the sentinel "unset" handle (e.g. material was hot-unloaded mid-frame, before phase 8). | Maps to **`render::Error::PassUnsupportedConfig`** (existing). The column write would otherwise emit a default handle; we refuse and return the error so the missing component is loud, not silent. | `abort-frame` — skip phase 7 for this frame; previous frame is re-presented; the next frame retries (the reload will have completed by then or the entity will have been respawned). | `missing_component_abort_frame.cpp`           |
| `BadHandle`                            | A `MeshHandle` / `MaterialHandle` / `BLASHandle` read from a component fails the generation check against the sibling-context table (e.g. a mesh handle whose generation was bumped by `geometry`'s hot-reload, but the renderable component still references the old generation). | Maps to **`render::Error::ResourceImportRefused`** (existing — semantic match: "an imported resource was refused"). | `abort-frame` — the next frame's extract walks fresh handles. The component author (gameplay-side) is responsible for fixing the stale handle; render does not silently default it. | `bad_handle_abort_frame.cpp`                  |
| `FrameSlotStarvation`                  | `acquire_slot` finds no slot in `FREE` role (all three are `READ` because phase 9 of frames N-1 and N-2 are still pending — i.e. the GPU is wedged or pins are held). | Maps to **`render::Error::FenceTimeout`** (existing) when the cause is GPU-side; or to **`core::Error::HotReloadRefused`** (existing) when the cause is observer pin overrun (§8.5). | `abort-frame` (GPU cause) or pin-deferral (observer cause). | `pin_overlong_starvation.cpp`, `slot_starvation_gpu_wedge.cpp` |
| `ECS access during phase 7 (compile-time refused)` | Any TU outside the extract TU that includes `core/world.hpp` after `GLIBRE_RENDER_EXTRACT_TU` is unset attempts `view_const` and fails to link or compile. | Compile-time rejection — never reaches `render::Error`. The contract is checked by the §11 unit test that compiles a synthetic extract-outside-phase-6 TU and asserts the build fails. | n/a — compile-time. | `phase7_world_access_compile_fail.cpp` (negative compile test) |
| `OutOfBudget` (allocator-side, `core::Error`) | The per-context allocator's strict-mode check fails before `arena_.allocate` is reached, because `ContextTag::render`'s live bytes already exceed 512 MiB. The arena exemption applies to the *arena's* allocations, not to whatever else is over budget. | Maps via `SPEC.md` §9.5.1 Allocator Rule 2 to **`render::Error::ResourceResidencyExceeded`**. | `lower-tier`. | (covered by the existing `residency_exceeded_lower_tier.cpp` fixture; extract is a downstream consumer, not the trigger.) |

The five extract-specific triggers above all map to existing
`render::Error` variants. **No ABI bump is needed** for extract failure
modes; the bump for the §4 pin/unpin pair is the only one this aggregate
introduces.

### 10.2 Severity and logging

All extract failures are logged once per occurrence at the severity
prescribed by `SPEC.md` §10.3 for the mapped variant. The structured
log fields render-frame-extract adds beyond the engine-wide schema:

- `frame_counter` — the snapshot's `frame_counter_`.
- `slot_index` — 0 / 1 / 2.
- `archetype` — `renderable / light / probe / camera_view`, the walk
  step that triggered.
- `entity_id` — the ECS entity for `MissingComponent` / `BadHandle`
  rows; `n/a` for arena / starvation.

The logger is `spdlog` per `reviews/decisions/error-model.md`
§"Logging / Telemetry"; the structured fields ride on
`spdlog::source_loc` plus the standard `glibre::log_error` shape. The
`DiagnosticOverlay` (`SPEC.md` §3.2 collapse #10) mirrors these fields
in-engine for the editor.

## 11. Test plan

Per the spike instructions, this aggregate ships unit and integration
tests against the §3 design. The plan-leaf author for the
implementation ticket binds these names to Catch2 files under
`tests/render/extract/`. **No tests are authored by this spike** (per
PHILOSOPHY: spike → deliverable doc, no tests); the names below are
the contract handed to the leaf author.

### 11.1 Unit — per-record extraction

| Test name                                              | Asserts                                                                                                                              | §3 reference     |
|--------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------|------------------|
| `extract_header_stamps_counter_alpha_settings`         | A populated `World` with `RenderSettings` and `frame_counter = 42` produces a snapshot whose header carries the same triple.         | §3.3 step 1      |
| `extract_view_camera_jitter_packed`                    | A camera ECS entity with `CameraComponent { proj, view }` + `PreviousGlobalTransform` produces an `ExtractedView` whose `prev_view` matches and whose jitter matches `RenderSettings.aa_mode == Taa` jitter sequence at the given frame counter. | §3.3 step 2      |
| `extract_renderable_lerp_transform_velocity`           | A renderable with prev / curr transforms and `alpha = 0.5` produces an interpolated transform = midpoint and a velocity = curr − prev. | §3.2.1, §3.3 step 3 |
| `extract_renderable_world_aabb`                        | A renderable with a `LocalAABB { -1..1 }^3` and a translation transform of `(10, 0, 0)` produces a world AABB centred at `(10, 0, 0)`. | §3.2.1, §3.3 step 3 |
| `extract_renderable_handles_passthrough`               | `MeshHandle / MaterialHandle / RenderLayerMask` columns equal the source components by 64-bit / 32-bit byte equality.                | §3.2.1           |
| `extract_renderable_soa_layout_contiguous`             | The eight SoA columns are contiguous spans (no padding between rows of the same column); cross-column adjacency is unspecified but each column's stride equals `sizeof(T)`. | §3.2.1           |
| `extract_lights_kind_classification`                   | A directional, point, spot, and probe entity each produces an `ExtractedLight` row with the correct `kind` enumerator and the requisite fields populated. | §3.2.2           |
| `extract_probes_routed_via_lights_span`                | A probe entity appears in the `lights_` span with `kind == Probe`; its cubemap handle round-trips through the `LightHandle` union. | §3.2.2, §3.3 step 5 |
| `extract_render_layer_mask_filters_per_view`           | A renderable on layer `bit 2` and a view on layer `bit 3` produce a `PerViewVisibleSpan` whose count is zero; flipping the view to `bit 2` produces a count of one. | §3.5             |
| `extract_views_walk_multi_split_screen`                | Two `ExtractedView` rows for split-screen viewports produce two rows in `views_` with disjoint `viewport_` rects and identical SoA backing. | §3.3 step 2      |
| `extract_arena_within_one_mib_at_s1`                   | The S1 fixture's snapshot arena footprint is < 900 KiB (headroom check against the 1 MiB cap). | §9.2             |
| `extract_arena_exhausted_returns_typed_error`          | An adversarial fixture (1 MiB renderables) makes the arena fail; the walk returns `render::Error::TransientPoolExhausted`. | §10              |
| `extract_missing_component_returns_typed_error`        | An entity in the renderable archetype with a sentinel `MaterialComponent` triggers `render::Error::PassUnsupportedConfig`. | §10              |
| `extract_bad_handle_returns_typed_error`               | A renderable with a stale-generation `MeshHandle` triggers `render::Error::ResourceImportRefused`. | §10              |
| `extract_view_const_only_compile_fail` (negative)      | A synthetic TU that includes `core/world.hpp` without `GLIBRE_RENDER_EXTRACT_TU` and attempts a mutable view fails to compile. | §3.4             |

### 11.2 Integration — full-frame extract

| Test name                                              | Asserts                                                                                                                              | §3 reference     |
|--------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------|------------------|
| `frame_extract_s1_under_budget`                        | The §11.1 S1 fixture's full extract walk + cull hand-off completes inside 0.5 ms (p99). (Folds into `BENCHMARK("phase-6 cull-extract S1, p99")` from `SPEC.md` §9.6.2.) | §9.1             |
| `frame_extract_snapshot_immutable_post_finalise`       | After `cull_extract_run` returns, an attempt to write into the snapshot's SoA columns is a compile error (the public surface returns `const RenderFrame&`). | §3.1, `SPEC.md` §4.1.1 invariant 1 |
| `frame_extract_self_contained_no_ecs_pointers`         | An adversarial harness destroys the source `World` after extract returns; phase 7 reads the snapshot through `submit_frame` without dereferencing destroyed memory. (ASan is the verifier.) | §3.5             |
| `frame_extract_triple_buffer_pipelining`               | A 3-frame run with phase 7 deliberately delayed produces three slots in distinct roles at the moment phase 6 of frame 4 begins; phase 6 of frame 4 acquires a `FREE` slot. | §3.6             |
| `frame_extract_slot_starvation_returns_error`          | A 4-frame run with all three slots held in `READ` (forced via observer pins) makes phase 6 of frame 4 return `render::Error::FenceTimeout` (the slot-starvation condition). | §3.6, §10        |
| `frame_extract_pin_defers_reload`                      | An observer pin held across phase 8 makes the loader defer the reload one frame; `DiagnosticOverlay` shows the deferral; releasing the pin lets the next phase 8 succeed. | §8.5             |
| `frame_extract_pin_overlong_refuses_reload`            | A pin held across 4 phase 8s makes the 4th attempt return `core::Error::HotReloadRefused`; the prior plugin remains live. | §8.5             |
| `frame_extract_hot_reload_carrying_state_empty`        | After a hot-reload swap, the next phase-6 walk produces a snapshot whose `frame_counter` increments by exactly 1 (no double-count, no skip), and whose contents match what a non-reloaded walk would produce. | §8.6             |
| `frame_extract_arena_drained_at_phase_9`               | A 5-frame run in `GLIBRE_ALLOC_STRICT=1` mode produces zero arena leaks across all three slots at every phase-9 entry. (Folds into `BENCHMARK("transient-pool drained at phase 9, strict")` from `SPEC.md` §9.6.2.) | §7.3, §9.2       |
| `frame_extract_render_settings_snapshot_stable_within_frame` | A reload of `RenderSettings` at phase 5 of frame N is *not* observed by phase 6 of frame N (the snapshot reads the value that was live at phase 5 entry); phase 6 of frame N+1 observes the new value. | §3.2.5, `SPEC.md` §8.2 |

### 11.3 Acceptance — story binding

The aggregate's leaf-tested user stories are the extract-touching subset
of `SPEC.md` §11 acceptance criteria. The leaf author binds:

- `Phase 6 cull-extract within 0.5 ms` — `frame_extract_s1_under_budget`.
- `Render layer mask filters per view` — `extract_render_layer_mask_filters_per_view`.
- `Transform interpolation produces smooth motion` —
  `extract_renderable_lerp_transform_velocity` plus an E2E motion-smoothness
  trace (owned by the user-story test plan, not by this design).
- `Multi-view from one snapshot` —
  `extract_views_walk_multi_split_screen` plus the integration test
  `frame_extract_triple_buffer_pipelining`.
- `Hot-reload preserves frame-counter monotonicity` —
  `frame_extract_hot_reload_carrying_state_empty`.

The story-driven E2E traces (`SPEC.md` §11) are authored by the
user-story test-plan tickets (separate plan leaves under the parent
sub-epic), not here.

## 12. Open questions

- [OPEN] **Per-archetype parallel extract walk.** §6.2 admits a future
  `parallel_for` over archetypes / over rows. Decision gate: profiling
  the §11.2 `frame_extract_s1_under_budget` benchmark on a post-MVP
  scene with > 10× S1's renderable count. If extract's slice exceeds
  0.30 ms (60 % of the 0.5 ms phase-6 ceiling), the leaf author lands
  the parallel-for variant. Owner: render-aggregate maintainer.
- [OPEN] **Dirty-flag incremental upload.** §2's refusal-with-rationale
  for R-2.10.2's "O(changed)" claim hinges on the budget being
  comfortable. If a post-MVP scene routinely puts extract over budget
  *and* parallel walks do not rescue it, the next perf-budget
  amendment may introduce a persistent `RenderWorld` mirror with
  dirty-flag tracking — but only if the alternative is breaking the
  cell. Owner: perf-budget review iteration.
- [OPEN] **`FrameSnapshot` generalisation across contexts.** Per
  `reviews/decisions/frame-phases.md` Q4 ("does `RenderFrame`
  generalise to a `FrameSnapshot` carrying both visual and audio
  extracts when the audio plugin lands?"). Resolution gate: when the
  audio plugin's first cull/extract spike opens, the question is
  forwarded to that ticket. Render's MVP does not pre-design for it.
  Owner: audio plugin's first design spike.
- [OPEN] **Probe extract refresh policy.** §3.2.2 / §2's R-2.4.24 row
  routes probe refresh modes (baked / on-load / periodic / on-change)
  to a future lighting / probe sub-aggregate. The hand-off: extract
  emits a `kind == Probe` row with `ProbeHandle`; the lighting plugin
  decides whether the probe needs re-shading in this frame. Resolution
  gate: post-MVP probe ticket. Owner: lighting plugin.
- [OPEN] **Per-view custom extract hooks.** A post-MVP plugin (vfx,
  particles) may want to extract its own per-frame data into the
  snapshot. The current §3.2 record taxonomy is closed for MVP. The
  hook mechanism — likely `World::register_extract_hook(phase=6, fn)`
  — is unspecified. Resolution gate: the first post-MVP plugin that
  needs it (vfx is the likely candidate); the spike for that plugin
  designs the hook. Owner: vfx plugin's first design spike.
- [OPEN] **Render-layer mask widening to 64-bit.** R-2.10.7 fixes the
  mask at 32 bits. If post-MVP editor / debug overlays exhaust the
  bits, the mask widens to 64. The change is mechanical (every
  `RenderLayerMask` row is 32 bits today; widening is a `glibre-types`
  middleman bump plus a fory schema migration). Resolution gate:
  editor's first pass that proves it needs > 32 layers. Owner: editor
  plugin's first design spike.
- [OPEN] **SoA column padding policy.** §3.2.1's "256 B/row upper
  bound" assumes a column-major-friendly layout; the actual stride
  depends on the SIMD alignment policy of the meshlet-cull kernel.
  Resolution gate: the cull aggregate's detailed design (#770) names
  the SIMD policy; this aggregate revisits the per-slot arena sizing
  if the policy bumps stride beyond ~256 B/row. Owner: cull aggregate
  design ticket.
