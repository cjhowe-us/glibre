# Geometry Spec

## 1. Purpose

The `geometry` context owns one responsibility: turning authored static
meshes into the cooked, runtime-resident artefacts that downstream
contexts consume by opaque handle. That covers the **cook-time pipeline**
(meshoptimizer-driven vertex-cache + overdraw + fetch optimisation,
meshlet decomposition with bounding spheres and normal cones, LOD chain
extraction, simplification-driven cluster DAG construction for
virtualised-geometry future use, screen-space-error metadata, BLAS recipe
emission per static mesh, Draco mesh(let) compression of the cooked
streams), the **on-disk container** (`MeshletPak` format — header,
cluster DAG, LOD bands, per-meshlet bounds + cone + SSE record, BLAS
recipe blob, Draco-compressed vertex/index/attribute streams, residency
hints), and the **runtime decode + residency hooks** (Draco decode into
GPU-resident vertex/index/attribute buffers, page-level residency policy
that the I/O scheduler drives, immutable `MeshHandle` /
`MeshletGroupHandle` issuance, BLAS-recipe materialisation into the
buffers the render context consumes when it builds the TLAS, LOD-band
selection rules used by the render extract phase). Geometry's outputs
are immutable per build: a shipped `MeshletPak` is byte-equal across
hosts (PHILOSOPHY §7) and only its residency state mutates at runtime.
Geometry refuses to own anything outside that seam. Frame submission,
command-buffer recording, mesh-shader dispatch, TLAS build/refit, and
the render graph belong to `render` (geometry only supplies the BLAS
recipe and the GPU-resident buffers behind the handles render binds);
HLSL → AIR/metallib compilation, PSO authoring, and shader permutation
belong to `shader`; skeletal deformation, blend shapes, IK, and any
pose-driven mesh mutation belong to the deferred `animation` plugin
(geometry meshes are pose-rigid; the animation plugin will write into a
separate skinning buffer, not into geometry's immutable streams); cloth,
fluid, particles, and any GPU-simulation-driven geometry belong to the
future `vfx` plugin; on-disk asset I/O scheduling, async page loads,
decompression worker pools, and streaming-budget arbitration belong to
`content` and `platform` (geometry declares which pages it would like
resident; the scheduler decides when); material parameter tables and
material-graph codegen belong to the future `material` plugin (geometry
meshes carry only an opaque `MaterialHandle` slot per submesh); CPU-side
collision shapes and the physics broadphase belong to `physics` (a
heightfield's collision twin is authored by `terrain` / `physics`, not
by this context); terrain heightfield tiles, voxel SDF volumes, ocean
clipmaps, sky domes, foliage billboards, procedural placement, and any
runtime-procedural mesh authoring are deferred to dedicated post-MVP
contexts (`terrain`, `water`, `sky`, `foliage`, `pcg`) and must not be
assumed primitives of this context. Geometry is the sole consumer of
meshoptimizer + Draco at cook time and the sole emitter of the
`MeshletPak` runtime format; per SRP every reason geometry has to change
must trace back to one of those listed responsibilities — anything else
routes to the owning context.

## 2. Ubiquitous Language

Terms used unchanged in code (identifiers, file names, comments).

| Term | Meaning |
|------|---------|
| `MeshSource` | Cook-time input: the authored static mesh (positions, indices, attribute streams, submesh ranges, material slot ids) before any optimisation. |
| `OptimisedMesh` | Post-meshoptimizer mesh: vertex-cache reordered, overdraw-optimised, fetch-optimised; the input to meshlet build. |
| `Meshlet` | Atomic geometry unit: ~64 vertices / ~124 triangles, with a bounding sphere, a normal cone, and a screen-space-error bound. The cluster-culler's quantum. |
| `MeshletGroup` | Set of meshlets that simplify together as one DAG node; a group's children are the next-finer cluster set; cuts of the DAG that respect group edges yield watertight meshes. |
| `ClusterDAG` | Acyclic graph of `MeshletGroup` nodes encoding the full LOD chain; a virtualised-geometry-friendly representation. Stored once per `MeshSource`, never rebuilt at runtime. |
| `LODBand` | A discrete tier (0 = finest) within `ClusterDAG`; render selects per-frame using `ScreenSpaceError` and `BoundingCone` against the active view. |
| `ScreenSpaceError` | Per-`MeshletGroup` projected error in pixels at a reference distance; the LOD-band selector compares this against the view's pixel threshold. |
| `BoundingCone` | Per-`Meshlet` orientation cone (axis + half-angle) used by the cluster culler for backface rejection. |
| `BoundingSphere` | Per-`Meshlet` and per-`MeshletGroup` minimum sphere; consumed by frustum and occlusion culling on the render side. |
| `BLASRecipe` | Cook-time blob describing how the runtime should build a Metal acceleration-structure BLAS for one `MeshSource` (geometry descriptors, flags, LOD-band selection); no GPU calls live here. |
| `MeshletPak` | The cooked on-disk container: header + `ClusterDAG` + per-band index streams + per-meshlet bounds/cone/SSE + `BLASRecipe` + Draco-compressed vertex/index/attribute streams + page table. The single artefact `geometry` ships. |
| `PakPage` | Fixed-size, individually-streamable region of a `MeshletPak` (one or more `MeshletGroup`s' compressed streams); the unit the scheduler loads / evicts. |
| `PakHeader` | Format-versioned record at the front of a `MeshletPak`: format hash, glibre engine version, page-table offset, residency hints, content hash. Refusal cases for hot-reload key off this. |
| `DracoStream` | One Draco-compressed payload inside a `MeshletPak` (positions, indices, normals, tangents, UVs, colours per attribute set). |
| `DecodePool` | Runtime-sized pool of Draco decode scratch buffers; sized from the pak header at startup, never resized mid-frame. |
| `MeshHandle` | Opaque, stable, immutable u32 (with generation) identifying one cooked `MeshSource`'s `MeshletPak`; the only thing render and other consumers ever see. |
| `MeshletGroupHandle` | Opaque handle to one cluster-DAG node within a `MeshHandle`'s pak; render's LOD selector resolves to this granularity. |
| `MaterialSlot` | Per-submesh opaque `MaterialHandle` index baked into the pak; geometry never inspects the slot's contents. |
| `ResidencyState` | Per-`PakPage` enum (`NotResident` / `Pending` / `Resident` / `Evicting`) maintained by the runtime decode pipeline; reads are lock-free. |
| `ResidencyHint` | Cook-time tag on a `PakPage` (e.g. always-resident bind-pose root, distance-bucket, hot-pose) the scheduler weights when arbitrating budget. |
| `GpuMeshBuffers` | The set of post-decode, GPU-resident buffers (vertex / index / per-attribute) backing one `MeshHandle`'s currently-resident bands; render binds them bindlessly via `MaterialHandle`. |
| `GeometryRegistry` | Runtime aggregate that owns the `MeshHandle` table, the `ResidencyState` table, and the `GpuMeshBuffers` allocator; the public boundary every other context calls into. |
| `CookManifest` | Per-`MeshSource` build-side record naming the source asset, the pak path, the format hash, and the cook-time options used; gated by content hash for incremental cooks. |
| `MeshoptStage` | The cook-time meshoptimizer step: vertex-cache reorder + overdraw optimisation + vertex-fetch optimisation + LOD-chain simplify; deterministic given input. |
| `MeshletBuildStage` | Cook-time stage producing `Meshlet` and `MeshletGroup` records (bounds, cone, SSE) from `OptimisedMesh`. |
| `DracoEncodeStage` | Cook-time stage compressing each `MeshletGroup`'s streams into `DracoStream`s; ships the decoder configuration in `PakHeader`. |
| `BLASRecipeStage` | Cook-time stage emitting the `BLASRecipe` blob (one per `MeshSource`); never builds the BLAS itself. |
| `PakWriter` | Cook-time emitter that serialises stage outputs into a `MeshletPak`; deterministic byte order. |
| `PakReader` | Runtime decoder front-end: maps a `MeshletPak`, validates `PakHeader`, exposes `PakPage` iterators to the decode pipeline. |
| `FormatHash` | Stable hash of the `MeshletPak` schema; runtime refuses to load paks whose hash mismatches the engine's compiled-in value. |

## 3. Derived From

Harmonius prior art was mined as **research input only** (PHILOSOPHY § "How
harmonius is used"). Every conclusion below is independently re-derived
against `PHILOSOPHY.md` (SOLID, SRP first; cohesion AND completeness;
plugin-only growth; static codegen everywhere; deterministic byte-equal
artefacts; Occam's razor at every collapse) and the §1 / §2 commitments
above. Cited paths live under `/Users/cjhowe/Code/harmonius/docs/`. Per
PHILOSOPHY §10 every multi-source concept is collapsed to the smallest
glibre primitive that still satisfies SRP; per PHILOSOPHY §3 every concern
that does not trace back to "turn an authored static mesh into a cooked,
runtime-resident `MeshletPak`" is refused and routed to the owning
context. Harmonius's geometry corpus is dominated by domain-content
generators (terrain, water, sky, foliage, procedural generation) that
glibre `geometry` explicitly does **not** own — only the meshlet-pipeline
requirement family (R-3.1.x) supplies MVP-shaped material; everything
else surfaces here as a refusal trail so the boundary stays load-bearing.

### 3.1 Cited harmonius sources

Requirements (`/Users/cjhowe/Code/harmonius/docs/requirements/geometry/`):

| File | IDs read | MVP relevance | What we mined |
|------|----------|---------------|---------------|
| `meshlet-pipeline.md` | R-3.1.1, R-3.1.5, R-3.1.6, R-3.1.NF1..NF3 | **Yes — the only MVP requirement family in this corpus.** | Meshlet decomposition shape (~64 vertices / ~124 triangles, per-meshlet bounding sphere + normal cone + screen-space-error bound, DAG hierarchy with watertight cut invariant); per-`MeshletGroup` projected SSE driving LOD-band selection; fixed-size streamable pages with priority hints; performance-budget envelopes (cull / raster / shade timings) used in §9. |
| `meshlet-pipeline.md` | R-3.1.2, R-3.1.3, R-3.1.4, R-3.1.7 | **No — routed to `render`.** | Two-phase HZB occlusion, task/mesh-shader culling pipeline, mesh-shader fallback, visibility buffer — these are *frame-time render passes*, not cook-time geometry primitives. Cited so the §3.3 refusal trail is explicit. |
| `terrain.md` | R-3.2.1 .. R-3.2.19, R-3.2.NF1..NF2 | **No — deferred to post-MVP `terrain` plugin.** | Heightfield tile streaming, virtual-texture clipmap, CDLOD geometry rings, hole masks, splatmap layering, decoupled collision LOD, 64-bit world coords + camera-relative f32 (this lives in `core` / `render` not `geometry`), portal interiors, sparse-octree SDF voxels, hybrid heightmap+voxel, planetary spheres, runtime voxel editing, voxel streaming, multi-planet coordinate space, cube-sphere face seams, 2D tilemap chunking + auto-tiling + per-chunk colliders. None of these author **immutable cooked static meshes**; all of them are runtime / authoring engines for their own domain. |
| `water.md` | R-3.4.1 .. R-3.4.8 | **No — deferred to post-MVP `water` plugin.** | FFT ocean displacement, shoreline depth blending, underwater volumetric mode, caustics projection, Fresnel reflection / refraction, flow-map rivers, dynamic foam, camera-centred clipmap mesh rings. Water *meshes* are simulation-driven, not cooked from authored static input — wrong context. |
| `sky-atmosphere.md` | R-3.5.1 .. R-3.5.7 | **No — deferred to post-MVP `sky` plugin.** | Analytical sky model, multi-scattering atmosphere LUTs, ray-marched volumetric clouds + cloud shadow maps, dynamic time-of-day, celestial bodies, environment-cubemap IBL capture. No `MeshSource` in sight. |
| `foliage.md` | R-3.3.1 .. R-3.3.9 | **Partially — see §3.3.** | GPU-driven instanced rendering, density-map rule placement, billboard / impostor LOD with crossfade, GPU vertex-shader wind from a wind field, character-vegetation interaction buffers, procedural grass blade meshing, dedicated tree shading, dense-foliage meshlet path through cluster DAG with masked blend + opacity micromasks (R-3.3.8), bone-chain wind preserving cluster AABBs (R-3.3.9). The cluster-DAG path (R-3.3.8) confirms our cooked artefact must already accept masked-blend submeshes; foliage *placement, wind, grass authoring, billboard generation* refuse to `foliage` (post-MVP). |
| `procedural-generation.md` | R-3.6.1 .. R-3.6.18 | **No — deferred to post-MVP `pcg` plugin.** | PCG visual-graph runtime, terrain stamps, biome distribution, road / building / WFC / modular-assembly, runtime + GPU generation, planetary / stellar / galactic generation, AI-driven authoring, GIS import. PCG outputs may *feed* the geometry cooker upstream, but PCG itself owns no piece of the cooked-mesh boundary. |

Designs (`/Users/cjhowe/Code/harmonius/docs/design/geometry/`):

| File | What we mined |
|------|---------------|
| `world-geometry.md` § "Meshlet Pipeline", § "Meshlet Offline Baking Pipeline" (lines ~158–177, ~360–384), § "Meshlet Types" (~lines 483–600), "API Design — Meshlet" sections (~lines 1041–1265) | Cook-stage pipeline shape (Import → LOD chain → Partition into meshlets → DAG build → Cache opt) and the meshoptimizer-FFI delegation pattern; ~64 KiB page size as the streaming unit; per-meshlet `BoundingSphere`, `NormalCone`, `lod_error`; `MeshletDAGNode` / `MeshletPage` / `MeshletMesh` shape; the GPU instance record; readback ring for streaming feedback; meshlet-error enumeration. |
| `world-geometry.md` § "Cross-Cutting Dependencies" (~lines 134–149), § "RF-1: Replace all Tokio/async with platform-native I/O", § "RF-2: Remove all Reflect derives", § "RF-5: Consolidate rendering features to render designs" | The fact that **streaming I/O, async runtime, render-graph passes, and PSO authoring already belong to peer contexts** in harmonius itself; glibre keeps the same seam. RF-2 confirms zero runtime reflection in shipping is a re-derived requirement, matching PHILOSOPHY §6. |
| `world-geometry.md` § "RF-6: Dense foliage via meshlet cluster LOD", § "RF-9: Bone-chain wind preserving cluster AABBs" | Cooked geometry must support masked-blend submeshes and opacity micromasks so the same `MeshletPak` format serves both opaque static meshes and the future foliage cluster path; cluster AABBs must remain stable frame-to-frame, which means cook-time bounds are authored against the *bind pose* — animation never mutates the cooked stream (matches §1 refusal). |
| `procedural-generation.md` (entire file) | Read to confirm there is no cook-time geometry concern in PCG that the geometry cooker should adopt; PCG is upstream content-authoring only. No glibre primitives derived. |

Cross-context cooking inputs (read for boundary alignment, not adopted as
geometry responsibilities):

- `docs/design/integration/asset-pipeline-rendering.md` § IR-5.2.5 (line
  ~41), § "MeshProcessor" (~line 380), § "Tooling" (~line 841) —
  confirms the cook-side invocation of `meshopt_buildMeshlets` lives in
  the asset pipeline's worker pool. Glibre routes the worker-pool side
  of this to `content` (§3.3 refusal); the glibre `geometry` context
  owns the pure meshlet build / DAG / Draco-encode logic that the
  workers call into.
- `docs/design/rendering/meshlets.md` § "BLAS build from meshlets"
  (R-2.4.6 lines ~18–53), § "BLAS section" (line ~593) — confirms BLAS
  is built from the same vertex / index buffers as the meshlet streams.
  Glibre cooks a **`BLASRecipe`** describing how `render` should build
  the BLAS; the actual GPU acceleration-structure call belongs to
  `render` per §1.
- `docs/design/rendering/render-effects.md` § "BLAS build from meshlets
  with compaction" (F-2.5.1 lines ~54, ~588–602) — confirms TLAS / BLAS
  *runtime* lifecycle is `render`'s; cook-time recipe authoring is
  ours.
- `docs/design/content-pipeline/asset-processing.md` § "meshoptimizer"
  (~line 153, ~1094, RF-7 line ~1307) — confirms meshoptimizer is the
  one library glibre cooks against; harmonius's late RF-7 toggle
  between the C library and the `meshopt` Rust crate is moot for
  glibre (we call meshoptimizer directly from C++).

Files **read but rejected as inputs to `geometry`** (each routed in §3.3):
`design/geometry/procedural-generation.md`, `requirements/geometry/terrain.md`,
`requirements/geometry/water.md`, `requirements/geometry/sky-atmosphere.md`,
`requirements/geometry/foliage.md` (placement / wind / billboard / grass
parts), `requirements/geometry/procedural-generation.md`.

### 3.2 Occam collapses (multiple harmonius concepts → one glibre primitive)

Per PHILOSOPHY §10 every collapse is recorded with the harmonius concepts
on the left, the single glibre primitive on the right, and the SOLID
rationale that licensed it.

1. **Separate "mesh import", "meshlet build", and "virtualized-geometry
   page streaming" subsystems → one cooked-asset pipeline producing one
   `MeshletPak`.** Harmonius split the static-mesh path across at least
   three cooperating modules: a generic mesh-import / `MeshAsset` path
   (`design/integration/asset-pipeline-rendering.md` § "MeshProcessor"
   line ~380; `design/rendering/meshlets.md` lines ~16–53), a
   `MeshletBuilder` that produces a `MeshletAsset` separately
   (`design/rendering/meshlets.md` § "Architecture", "Pipeline" —
   defining `Meshlet`, `LodGroup`, `MeshletAsset`), and a
   virtualized-geometry residency / page system in
   `design/geometry/world-geometry.md` § "Virtual geometry streaming"
   (~lines 170, 360–384, 483–600) — three asset shapes (`MeshAsset`,
   `MeshletAsset`, `MeshletPage`) with overlapping but non-identical
   schemas. **Glibre collapses to one cook-time pipeline** (`MeshSource`
   → `MeshoptStage` → `MeshletBuildStage` → `DracoEncodeStage` +
   `BLASRecipeStage` → `PakWriter` → `MeshletPak`) emitting one
   on-disk artefact (`MeshletPak`, §2). There is no parallel
   `MeshAsset` shipping format — every cooked mesh is a `MeshletPak`,
   even meshes whose DAG is a single LOD band. Justification: SRP —
   one reason for `geometry` to change is "the cooked artefact format
   evolves"; that evolves in one place, behind one `FormatHash`.
   Cohesion-and-completeness (PHILOSOPHY §2): the cooker cannot ship
   half-a-format and defer the rest; `MeshletPak` is complete on day
   one and post-MVP foliage / virtualized-geometry features are
   additions to *this* schema, not a new one.

2. **Three cook-time tool stacks (meshoptimizer for clustering / LOD,
   bespoke compression, ad-hoc per-attribute encoders) → meshoptimizer
   + Draco, exclusively.** Harmonius cooked with meshoptimizer
   (`design/integration/asset-pipeline-rendering.md` § IR-5.2.5;
   `design/content-pipeline/asset-processing.md` § "meshoptimizer"
   line ~153) but never settled on a compression story (the only
   compression discussion is heightfield LZ4 in `terrain.md` R-3.2.NF1,
   which is **not** static-mesh data). Glibre fixes the cook-time
   library set: **meshoptimizer** for vertex-cache reorder + overdraw
   optimisation + vertex-fetch optimisation + LOD-chain simplify +
   meshlet partitioning + meshlet-bound + normal-cone generation
   (`MeshoptStage`, `MeshletBuildStage` in §2), and **Draco** for the
   per-`MeshletGroup` per-stream compressor (`DracoEncodeStage` in §2).
   No third encoder is admitted; no per-attribute bespoke quantiser is
   authored in glibre — Draco's documented quantisation is the contract.
   Justification: SRP — the geometry context owns one cook-time
   external-library seam, and that seam is two libraries with one
   purpose each. PHILOSOPHY §1 (SRP first) and §10 (Occam) — picking
   two well-scoped libraries beats authoring our own.

3. **Three runtime "mesh consumer" shapes
   (`MeshAsset` / `MeshletAsset` / `MeshletPage`) → one `MeshletPak` +
   one `PakPage` streaming unit + opaque `MeshHandle` /
   `MeshletGroupHandle`.** Harmonius
   `design/geometry/world-geometry.md` § "Meshlet Types" (~lines
   483–600) and `design/rendering/meshlets.md` exposed `MeshletAsset`,
   `MeshletMesh`, `MeshletMeshComponent`, `MeshletPage`,
   `GpuMeshletInstance`, and a separate `MeshletDAGNode` as runtime
   types every consumer touched. Glibre collapses every public-facing
   geometry runtime type to **two opaque handles** (`MeshHandle`,
   `MeshletGroupHandle`, §2) plus the registry (`GeometryRegistry`)
   that owns the table; everything else lives behind that seam.
   `PakPage` is the single streaming unit `content` schedules and the
   single residency-state target; render never sees `Meshlet` records
   directly, it sees the bindless `GpuMeshBuffers` the registry hands
   it once a `MeshletGroupHandle` becomes resident. Justification:
   PHILOSOPHY §3 (plugin-only growth, opaque handles at every plugin
   boundary) + §6 (zero runtime reflection) — opaque handles eliminate
   the cross-plugin schema surface that would otherwise force every
   consumer to track a copy of our internal layout.

4. **Mesh-builder → cluster-builder → DAG-builder → simplifier →
   page-packer → BLAS-builder → uploader (seven harmonius cook-stage
   modules) → five named glibre cook stages with deterministic
   output.** Harmonius
   `design/geometry/world-geometry.md` § "Meshlet Offline Baking
   Pipeline" (~lines 360–384, table at ~line 376) and
   `design/integration/asset-pipeline-rendering.md` listed Import,
   Simplify, LOD Chain, Partition, Bounds, DAG Build, Cache Opt as
   separate boxes. Glibre collapses these into the five §2 stages:
   `MeshoptStage` (cache + overdraw + fetch reorder + LOD-chain
   simplify), `MeshletBuildStage` (partition + bounds + cone + SSE +
   DAG link), `DracoEncodeStage` (compress per-group streams),
   `BLASRecipeStage` (emit recipe blob), `PakWriter` (deterministic
   serialise to disk). Each stage is one SRP-bounded class with
   deterministic byte-equal output across hosts (PHILOSOPHY §7).
   Justification: SRP plus PHILOSOPHY §2 — small bounded contexts,
   each complete within its scope; one cook stage = one reason to
   change.

5. **Runtime "build BLAS / refit TLAS" + cook-time "describe BLAS
   inputs" → one `BLASRecipe` blob baked into the pak.** Harmonius
   `design/rendering/render-effects.md` § "BLAS build from meshlets"
   (F-2.5.1, R-2.5.1) and `design/rendering/meshlets.md` §
   "BLAS section" (line ~593) put both ends of the BLAS lifecycle in
   `render`. Glibre splits them: cook-time recipe authoring is
   `geometry`'s `BLASRecipeStage` (`BLASRecipe` blob in `MeshletPak`,
   §2), runtime `MTLAccelerationStructure` build / refit / TLAS
   assembly is `render`'s. The recipe is the single declarative
   contract — render reads it, never asks geometry runtime questions.
   Justification: SRP — one reason to change "what BLAS inputs look
   like" routes to `geometry`; one reason to change "how the GPU
   builds an AS" routes to `render`. PHILOSOPHY §1 + §3 (plugin-only
   growth at the GPU boundary).

6. **Streaming I/O scheduler + decompression worker pool +
   `Tokio`-style async runtime + decode buffers (four harmonius
   layers) → declarative `ResidencyHint` + lock-free
   `ResidencyState` table + sized `DecodePool`, with the *scheduler
   itself* refused.** Harmonius
   `design/geometry/world-geometry.md` § RF-1 (lines ~2229–2240) had
   already begun the move from Tokio to platform-native I/O — glibre
   completes the move and refuses the scheduler entirely. Geometry
   declares **what** it wants resident (`ResidencyHint` baked at cook,
   `ResidencyState` read by render) and runs decode against a
   pre-sized scratch pool (`DecodePool`); **when** pages load and how
   the budget is arbitrated belongs to `content` / `platform`.
   Justification: SRP — one reason for `geometry` to change is "the
   pak format / decode pipeline evolves"; budget arbitration evolves
   for entirely different reasons (platform memory tiers, install
   strategies, network installs) and lives elsewhere. PHILOSOPHY §3
   (plugin-only growth).

7. **Per-stream attribute formats fanned across `Meshlet`,
   `MeshletAsset`, `MeshletMesh`, `MeshletMeshComponent`,
   `GpuMeshletInstance`, `TerrainMesh`, `FoliageCluster` (seven
   harmonius shapes carrying their own attribute layouts) → one
   `MaterialSlot` + one `GpuMeshBuffers` set per `MeshHandle`.** All
   the harmonius types in the §"Meshlet Types" / §"Foliage Types" /
   §"Terrain Types" blocks of `world-geometry.md` (lines ~483, ~602,
   ~790) carry overlapping descriptions of what positions / indices /
   normals / tangents / UVs / colours / material indices look like.
   Glibre stores those decisions exactly **once** — in the
   `MeshletPak` schema and the `GpuMeshBuffers` runtime layout — and
   exposes only `MaterialSlot` (an opaque `MaterialHandle` index) at
   the public boundary. Justification: SRP — one reason for the
   attribute layout to change routes to one schema. PHILOSOPHY §10
   (Occam) — `MaterialHandle` is owned by the future `material`
   plugin; geometry refuses to know what's behind that index.

### 3.3 Refusals (routed to other contexts)

Glibre's `geometry` plugin does **not** own any of the following, even
though harmonius collected them under "geometry". Each routes to the
owning context per §1, and the corresponding harmonius requirement IDs
are **out of scope** for this spec.

| Harmonius surface | Cited file(s) / IDs | Routed to |
|-------------------|---------------------|-----------|
| Two-phase HZB occlusion culling, task / mesh-shader cluster + triangle culling, mesh-shader-fallback compute compaction + multi-draw-indirect, visibility-buffer raster + deferred fullscreen material pass | `requirements/geometry/meshlet-pipeline.md` R-3.1.2, R-3.1.3, R-3.1.4, R-3.1.7 | `render`. Geometry only supplies the cooked streams + `BLASRecipe` + per-meshlet bounds / cone / SSE that the cull / raster / shade passes consume. |
| `MTLAccelerationStructure` build / refit, TLAS assembly + per-frame refit, BLAS compaction, ray-traced shadows / AO / reflections | `requirements/geometry/meshlet-pipeline.md` (none directly), `design/rendering/render-effects.md` F-2.5.1 / R-2.5.1, `design/rendering/meshlets.md` BLAS section | `render`. Geometry ships `BLASRecipe`; render builds the AS. |
| HLSL → AIR / metallib compilation, PSO authoring, shader-permutation cook, descriptor-frequency-group binders | (no direct harmonius geometry IDs; cited via `world-geometry.md` § RF-5) | `shader` plugin. Geometry never compiles or authors a shader. |
| Material-graph authoring, material-codegen, custom material functions, the bindless material parameter buffer schema | (cited via `requirements/foliage.md` shading rules; harmonius treats material as a separate plugin) | `material` plugin (deferred). Geometry meshes carry an opaque `MaterialHandle` slot per submesh; geometry never inspects it. |
| Skeletal deformation, blend shapes, IK, pose-driven mesh mutation, bone-chain foliage wind preserving cluster AABBs (R-3.3.9 *animation half*) | `requirements/geometry/foliage.md` R-3.3.9 (animation half), `design/animation/skeletal.md` § "Bone chain for Nanite-style foliage" (line ~1867) | `animation` plugin (deferred). Geometry meshes are pose-rigid; the animation plugin will write into a separate skinning buffer, never into our cooked streams. The cooked stream's *bind-pose* bounds remain stable, which is exactly what RF-9 asks for. |
| GPU-driven foliage instancing + compute culling, density-map / rule procedural placement, billboard / impostor LOD with crossfade, GPU vertex-shader wind from a wind field, character-vegetation interaction buffer, dense-foliage cluster-DAG path with masked blend + opacity micromasks (R-3.3.8 *placement / runtime half*), procedural grass blade meshing, dedicated tree-shading pipeline with subsurface leaf transmission | `requirements/geometry/foliage.md` R-3.3.1 .. R-3.3.9, `design/geometry/world-geometry.md` § "Foliage" (line ~74), § "Foliage Types" (line ~790) | `foliage` plugin (deferred post-MVP). The cooked-pak format already accommodates masked-blend submeshes and opacity micromasks (R-3.3.8 *format half*) so foliage can ride the existing `MeshletPak`; placement, wind, grass, and impostor authoring are not geometry concerns. |
| Heightfield tile streaming, virtual-texture clipmap, CDLOD geometry rings, per-tile hole masks, splatmap layering, tile collision derived from the heightfield, portal interiors, sparse-octree SDF voxels with material IDs, hybrid heightmap+voxel resolver, planetary spheres + radial gravity, runtime voxel editing + incremental re-mesh + serialized delta logs, voxel streaming + RLE compression, multi-planet coordinate space + cube-sphere face seams, 2D tilemap chunks + auto-tiling + per-chunk colliders | `requirements/geometry/terrain.md` R-3.2.1 .. R-3.2.19, R-3.2.NF1..NF2 | `terrain` plugin (deferred post-MVP). Terrain authors its own collision shape (with `physics`), its own LOD (clipmap or voxel), its own streaming unit, and its own materials; it does not ship `MeshletPak`s. |
| FFT ocean displacement on GPU compute, shoreline depth blending + foam, underwater volumetric fog + Beer-Lambert + caustics, Fresnel reflection + refraction, flow-map rivers, dynamic foam, camera-centred clipmap mesh rings | `requirements/geometry/water.md` R-3.4.1 .. R-3.4.8 | `water` plugin (deferred post-MVP). Water meshes are simulation outputs, not authored static meshes. |
| Analytical procedural sky model, multi-scattering atmosphere LUTs + aerial perspective, ray-marched volumetric clouds + temporal reprojection, cloud shadow map, dynamic time-of-day astronomical arcs, celestial bodies, environment-cubemap IBL capture | `requirements/geometry/sky-atmosphere.md` R-3.5.1 .. R-3.5.7 | `sky` plugin (deferred post-MVP). Atmospheric LUTs and procedural cubemaps are not cooked static meshes. |
| PCG visual-graph runtime, deterministic point generation / filtering / transformation, terrain stamps + biome distribution, spline roads + buildings + WFC + modular assembly, GPU + chunk-based runtime generation, planet- / star-system- / galaxy-scale generation, AI-driven authoring, GIS / OSM import, mineralogy distributions | `requirements/geometry/procedural-generation.md` R-3.6.1 .. R-3.6.18, `design/geometry/procedural-generation.md` (entire) | `pcg` plugin (deferred post-MVP). PCG outputs may *feed* a geometry cook upstream, but PCG does not own any portion of the cooked-mesh boundary. |
| Streaming I/O scheduler, async page loads, decompression worker pool, streaming-budget arbitration, mod / patch / install-time pak placement | `design/geometry/world-geometry.md` § RF-1, § "Cross-Cutting Dependencies" (lines ~134–149); `design/integration/asset-pipeline-rendering.md` § "Tooling" (line ~841) | `content` + `platform`. Geometry declares `ResidencyHint` and exposes lock-free `ResidencyState`; everything about *when* pages load lives outside this context. |
| CPU-side collision shapes, physics broadphase, heightfield collision, voxel collision, NavMesh invalidation on voxel edit | `requirements/geometry/terrain.md` R-3.2.6, R-3.2.13; `design/physics/foundation.md` § "BLAS / collision twin" (line ~551) | `physics` (and `terrain` for the heightfield twin). Geometry is render-side only. |
| Render-graph topology, frame submission, command-buffer recording, mesh-shader dispatch, indirect-draw fallback path | `design/geometry/world-geometry.md` § "GPU-Driven Culling and Rendering" (line ~385) | `render`. Geometry does not enqueue draws. |
| Tools-side mesh import UI, content browser previews, in-editor LOD authoring, asset cooking job orchestration | `design/integration/asset-pipeline-rendering.md`, `design/content-pipeline/asset-processing.md` | `tools` + `content`. Geometry is the cook-stage **library**, not the orchestration shell. |

These refusals are PHILOSOPHY §1 (SRP) + §3 (minimal core, plugin-only
growth) + §5 (greatly reduced MVP scope) applied to the harmonius
"geometry" umbrella: any concern whose reason-to-change does not
collapse to "cook a static mesh into a `MeshletPak`, decode it back at
runtime behind opaque handles" lives in another plugin.

## 4. Aggregates & Invariants

- Aggregate / entity / value object.
- Invariants that must hold at every public API boundary.

## 5. Public Interface

```cpp
// header-only stub goes here
```

Event types, serialized schemas (Fory), error types.

## 6. Internal Architecture

Non-binding sketch for implementers.

## 7. Persistence & Schemas

Fory schemas. Migration rules.

## 8. Hot-Reload Contract

What survives swap, what `migrate(...)` must do, what triggers refusal.

## 9. Performance Budget

Cycles / frame, memory ceiling, allocation rules.

## 10. Failure Modes & Error Model

Typed errors. Recovery.

## 11. Acceptance Criteria

GitHub `type:user-story` issues this spec closes:

- #TBD — `<title>`

Each must have a Catch2 test by name.

## 12. Open Questions

- Owner / resolution gate.
