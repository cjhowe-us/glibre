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

This section enumerates geometry's aggregates, entities, and value objects
along with the invariants every public boundary must hold. Aggregates are
listed in data-flow order: cook-time inputs feed cook-time stages that
emit the immutable `MeshletPak` artefact (§4.1.1 → §4.1.8), then the
runtime aggregates own load, decode, residency, and handle issuance
(§4.1.9 → §4.1.15). Each aggregate owns one dimension of "turn an
authored static mesh into a cooked, runtime-resident `MeshletPak`,
decoded back behind opaque handles"; per PHILOSOPHY §1 (SRP), an
aggregate is admitted to this list only when its single reason-to-change
does not collapse into another's. Where multiple harmonius primitives
reduce to one glibre primitive, the collapse is cited from §3.2.
Cross-context concerns (frame submission, BLAS GPU build, shader cook,
streaming-budget arbitration, animation, materials) are explicitly
delegated and never re-asserted here (§3.3).

### 4.1 Aggregate roster

#### 4.1.1 `MeshSource` — authored static-mesh input (value object, cook-time only)

**Reason to change:** what authored data the cooker accepts (positions,
indices, attribute streams, submesh ranges, material slots). Distinct
from optimisation policy (§4.1.2) and from cluster shape (§4.1.3).

**Composition.** The pre-optimisation snapshot of one authored mesh:
position stream (`std::span<const f32>` xyz), index stream (`u32`),
optional per-vertex attribute streams (normal, tangent, UV0..UVn,
colour, vertex weights), `Submesh` ranges (each carrying an opaque
`MaterialSlot` index baked at author time), and the source-asset
content hash that `CookManifest` (§4.1.8) keys incremental cooks off.
No engine-runtime types live here — `MeshSource` is exclusively the
input that the cook-time stages consume.

**Identity & lifetime.** Constructed by the upstream `content` importer
from FBX / glTF / OBJ on the cook worker thread; lives only until the
end of the per-mesh cook job; never enters a `MeshletPak` directly.

**Public-boundary invariants.**

1. **Pose-rigid.** `MeshSource` carries the authored bind-pose vertex
   data only; no per-frame deformation, no skinning matrices. Skinning
   data (joint indices / weights) is carried as opaque attribute
   streams the future `animation` plugin owns; geometry never inspects
   it (§3.3 routing for skeletal deformation).
2. **Manifold submeshes.** Every `Submesh` index range describes a
   topologically valid triangle list (no degenerate triangles, no
   index out of range, no orphan vertices); the cooker rejects the
   mesh with `geometry::Error::MeshSourceInvalidTopology` rather than
   producing a `MeshletPak` with undefined cluster behaviour.
3. **Stable attribute set per submesh.** All vertices addressed by one
   submesh share one attribute layout; mixing layouts inside a
   submesh is rejected with `geometry::Error::AttributeLayoutMismatch`.
4. **Deterministic byte order.** Field encoding is little-endian fixed
   layout; equal-content `MeshSource`s on two hosts produce byte-equal
   bytes feeding `MeshoptStage`. PHILOSOPHY §7.

#### 4.1.2 `OptimisedMesh` — meshoptimizer-stage output (value object, cook-time only)

**Reason to change:** which meshoptimizer passes are applied and in
which order (vertex-cache reorder + overdraw + fetch + LOD-chain
simplify). Bounded; doesn't drag in cluster shape or compression.

**Composition.** Post-`MeshoptStage` reordered vertex/index streams
keyed by submesh, plus the per-LOD-chain index sets produced by
`meshopt_simplify` (one index span per LOD level, finest to coarsest).
Carries the FFI-exact return values from meshoptimizer so the
downstream `MeshletBuildStage` can reproduce them deterministically.
Does not yet carry meshlet decomposition or bounds.

**Identity & lifetime.** One per `MeshSource` per cook; lives only
until consumed by `MeshletBuildStage`; never serialised into
`MeshletPak` (the LOD chain is re-expressed as `MeshletGroup` records).

**Public-boundary invariants.**

1. **Deterministic given input.** `OptimisedMesh` is a pure function of
   `MeshSource` plus the cook-time meshoptimizer parameters baked
   into `CookManifest`; two hosts cook byte-equal output (PHILOSOPHY
   §7). The cooker uses meshoptimizer's reproducible-flag path; no
   randomised tie-breaking.
2. **Vertex-fetch + cache + overdraw all applied.** A pak whose
   header advertises `MeshoptStage` applied must have all three
   passes run; partial application is refused at cook time.
3. **LOD-chain non-empty.** The simplification chain contains at
   least LOD0 (the finest level); a zero-LOD output is rejected.

#### 4.1.3 `Meshlet` — atomic cluster (value object)

**Reason to change:** the cluster-quantum format — what fits in
`Meshlet` (vertex count, prim count, bound, cone, SSE record). One
seam.

**Composition.** Fixed-size record per cluster: an offset into the
pak's per-meshlet vertex / triangle index arrays, a vertex count
(`u8`, ≤ 64), a triangle count (`u8`, ≤ 124), the cluster's
`BoundingSphere` (`f32 cx, cy, cz, r`), the cluster's `BoundingCone`
(`f32` axis xyz + `f32` half-angle cosine), and the per-cluster
`ScreenSpaceError` bound at the reference distance.

**Identity & lifetime.** Identified by index inside its owning
`MeshletPak`'s meshlet table; the index is stable for the life of
the pak (the pak is immutable post-build, §4.1.7). Geometry never
exposes raw `Meshlet` records across the plugin boundary — only
`MeshletGroupHandle` (§4.1.10).

**Public-boundary invariants.**

1. **`vertex_count ≤ 64` and `prim_count ≤ 124`.** Hard cap matching
   meshoptimizer's `meshopt_buildMeshlets` shape (§3.1 mining of
   R-3.1.1) and Metal 4 mesh-shader payload limits. Any cluster
   exceeding either count fails cook with
   `geometry::Error::MeshletOversize`; the pak format itself
   physically cannot represent it (the count fields are `u8` with
   range checks at `PakReader` time).
2. **Bounding sphere encloses all referenced vertices.** Computed
   by meshoptimizer's `meshopt_computeMeshletBounds`; the cooker
   emits a self-test (debug build) that re-validates on a sampled
   subset.
3. **Cone half-angle bounded.** A degenerate cone (all-direction
   normal cone, `cos(θ) = -1`) is allowed (signals "do not
   backface-cull this cluster") but encoded as a single sentinel
   value so cull shaders branch deterministically.
4. **`ScreenSpaceError` non-negative.** Per-meshlet SSE values are
   bounded by the per-`MeshletGroup` SSE so render's LOD-band
   selector can rely on group-level monotonicity (§4.2 invariant 2).

#### 4.1.4 `MeshletGroup` — DAG node + LOD-band record (entity)

**Reason to change:** how clusters group into LOD-DAG nodes
(simplification policy, watertight-cut shape, per-group SSE).

**Composition.** A range of `Meshlet` indices (the cluster set
this group materialises at one LOD), the parent-group references
in the next-coarser LOD band, the child-group references in the
next-finer band, the group-level `BoundingSphere` enclosing all
constituent meshlet spheres, the per-group `ScreenSpaceError` value
the LOD-band selector compares against the view's pixel threshold,
and the `LODBand` tier (0 = finest). Watertight-cut bookkeeping —
which group edges may be crossed by a render-time LOD cut — is
encoded as a per-edge bit mask so the runtime selector can verify
its choice in O(group degree).

**Identity & lifetime.** Identified by index inside its owning
`MeshletPak`'s group table; the public-facing handle is
`MeshletGroupHandle` (§4.1.10). One `MeshletGroup` is the residency
target — `PakPage` (§4.1.6) packs whole groups, never half a
group's clusters.

**Public-boundary invariants.**

1. **Watertight cut.** Any cut of the cluster DAG that respects
   group edges (i.e. selects exactly one band per cut) yields a
   topologically watertight mesh: no T-junctions, no missing
   triangles, no double-coverage. Cooker validates by re-tessellating
   sample cuts and rejecting on watertight-failure.
2. **Per-group SSE monotonically tightens with finer LOD.** For any
   `MeshletGroup` `g` and its child `c` in `LODBand(c) = LODBand(g)
   - 1`, `SSE(c) < SSE(g)` strictly (finer = lower SSE). Equality is
   not permitted; the monotonic-tightening property is what makes
   the LOD-band selector total. Validated at cook time;
   cross-aggregate invariant repeated in §4.2.
3. **LOD0 group set covers the full mesh.** The set of LOD0
   `MeshletGroup`s collectively materialises every triangle of
   the original `MeshSource`; coarser bands are simplifications,
   never additions.
4. **Group bounds enclose all child meshlets.** The group sphere
   contains every constituent `Meshlet`'s sphere; cooker computes
   from constituents and validates.

#### 4.1.5 `ClusterDAG` — acyclic LOD chain (aggregate root, cook-time only)

**Reason to change:** the DAG topology — how groups link across
bands. Distinct from the per-group payload (§4.1.4) and from how
the DAG is laid out on disk (§4.1.6, §4.1.7).

**Composition.** The full set of `MeshletGroup` records ordered by
`LODBand` (0 = finest), the parent-edge / child-edge adjacency
lists between bands, and the LOD0-band cover (the `MeshletGroup`
indices that constitute the LOD0 cut — the single cluster band
the BLAS recipe references, §4.1.7). Stored once per `MeshSource`,
never rebuilt at runtime.

**Identity & lifetime.** Lives inside the cooker until serialised
into `MeshletPak`; the runtime equivalent is the immutable bytes
inside the pak — there is no `ClusterDAG` mutation after cook.

**Public-boundary invariants.**

1. **Acyclic.** Topological sort succeeds; no edge connects a node
   in band `k` to a node in band `k` or to a finer band. Verified
   at cook time; violation refuses pak emission with
   `geometry::Error::ClusterDAGCycle`.
2. **Edges only between adjacent bands.** A group's parents are
   exclusively in `LODBand + 1`; children exclusively in `LODBand
   - 1`. No skip-level edges.
3. **Single LOD0 cover.** The LOD0 cluster band is a single
   complete cover of the source; cooker rejects DAGs whose LOD0
   nodes do not partition the source triangle set.
4. **Coarsest band is reachable from every leaf.** Every LOD0
   group is connected to the coarsest band by a chain of
   parent edges; orphan subgraphs are refused.

#### 4.1.6 `PakPage` — fixed-size streaming unit (value object)

**Reason to change:** the streaming granularity — what the I/O
scheduler loads / evicts.

**Composition.** A fixed-size, individually-streamable region of
a `MeshletPak`: a header byte (page-format-version), a list of
the `MeshletGroup` indices wholly contained in this page, and the
Draco-compressed per-group vertex / index / attribute streams
(`DracoStream`, §4.1.6.1) for those groups. Page size is set at
cook time (default 64 KiB, mined from harmonius `world-geometry.md`
§ "Meshlet Offline Baking Pipeline" line ~360); paks may carry
mixed page sizes if the cooker chose per-tier sizing, but every
page header self-describes its size.

**Identity & lifetime.** Identified by index inside its owning
`MeshletPak`'s page table; the runtime `ResidencyState`
(§4.1.13) is keyed by `(MeshHandle, page_index)`. Pages are the
unit `content` schedules and the unit `DecodePool` operates on.

**Public-boundary invariants.**

1. **Whole-group containment.** A `MeshletGroup`'s clusters
   reside entirely inside one `PakPage`; group splits across
   pages are refused at cook time. This makes residency
   decisions group-coherent — render's LOD selector either
   sees the whole group resident or none of it.
2. **Self-describing.** Page header contains the page size, the
   list of group indices, and the per-stream `DracoStream`
   offsets relative to page start; readable without reference
   to neighbouring pages.
3. **Integrity-checked.** Every page carries a CRC32 trailer over
   its post-header bytes; `PakReader` (§4.1.11) refuses to feed
   a page with mismatched CRC into `DecodePool`, returning
   `geometry::Error::PakPageIntegrityFailed`.
4. **Sized within scheduler page-budget bounds.** Page bytes ≤
   the engine-wide upper limit declared in `CookManifest`'s
   target profile; oversize pages refuse cook.

##### 4.1.6.1 `DracoStream` — one Draco-compressed payload (value object)

**Reason to change:** Draco encoder configuration / quantisation
profile per attribute kind. Bounded.

**Composition.** A single Draco-compressed byte stream for one
attribute kind (positions / indices / normals / tangents / UVs /
colours) of one `MeshletGroup`'s worth of vertex data. Carries
the Draco quantisation profile (one of a small fixed set declared
in `PakHeader`) and the decoded byte length so `DecodePool`
(§4.1.12) can pre-size scratch buffers.

**Public-boundary invariants.**

1. **Byte-equal decode across hosts.** Decoding the same
   `DracoStream` on any supported host produces byte-equal
   vertex bytes; this is how the runtime stays deterministic
   even though it decodes lazily (PHILOSOPHY §7). Validated by
   the unit test `decompresses-byte-equal-on-multiple-hosts`
   referenced from §11.
2. **Self-describing quantisation.** The decoder reads the
   profile index from the stream header and configures Draco
   from `PakHeader`'s profile table; profile mismatch refuses
   load with `geometry::Error::DracoProfileUnknown`.
3. **No per-stream allocation in shipping.** Decode runs against
   `DecodePool` scratch buffers pre-sized at engine init; a
   stream whose decoded bytes exceed the pool slot refuses
   decode with `geometry::Error::DecodePoolOverflow` (the
   cooker's pak-level upper bound is supposed to prevent this,
   so this error is a contract-violation signal, not an
   expected runtime path).

#### 4.1.7 `MeshletPak` — cooked on-disk container (aggregate root)

**Reason to change:** the cooked-artefact format — the single seam
geometry ships across plugin and host boundaries. One reason
licenses a `FormatHash` bump.

**Composition.** A `PakHeader` (§4.1.7.1) followed by the cluster
DAG bytes (linearised group / meshlet tables), the `BLASRecipe`
blob (§4.1.7.2), the `PakPage` array, and a page table mapping
page indices to byte offsets. One `MeshletPak` is one `MeshSource`;
no pak carries two source meshes.

**Identity & lifetime.** Per cooked mesh; immutable once written by
`PakWriter`. At runtime, lifetime equals the time the pak file is
mapped into the process; eviction unmaps.

**Public-boundary invariants.**

1. **Stable `FormatHash` per cooked pak.** Every `MeshletPak`
   carries a `FormatHash` field in `PakHeader` derived from the
   pak schema version; runtime refuses to load a pak whose hash
   mismatches the engine's compiled-in value
   (`geometry::Error::PakFormatHashMismatch`). This is the
   primary refusal hook for hot-reload schema drift (§8). The
   hash is content-of-schema, not content-of-mesh — two paks
   with different mesh data but identical schema have identical
   `FormatHash`. Cross-aggregate invariant repeated in §4.2.
2. **Byte-equal across hosts.** Two cooks of the same source on
   two hosts produce byte-equal pak bytes (PHILOSOPHY §7). The
   determinism chain is `MeshSource` byte-equal → `OptimisedMesh`
   byte-equal → cluster build byte-equal → Draco encode
   byte-equal → `PakWriter` byte-equal.
3. **Single `MeshSource` per pak.** No pak multiplexes meshes;
   multi-mesh asset bundles are a `content`-context concern, not
   geometry's.
4. **Header-validated before any payload read.** `PakReader`
   refuses any payload access until `PakHeader` validation
   passes.

##### 4.1.7.1 `PakHeader` — format-versioned record (value object)

**Reason to change:** what the runtime needs to validate and
size before accessing payload (format version, hash, page-table
offset, residency hints, content hash, decode-pool sizing).

**Composition.** Fixed-layout little-endian record at offset 0 of
the pak: `FormatHash` (`u64`), glibre engine version triple
(`u16` major / minor / patch), page-table offset (`u64`),
page-count (`u32`), `BLASRecipe` offset + length (`u64` × 2),
`DecodePool` sizing requirements (per-attribute scratch-byte
maxima, `u32` array), the Draco quantisation profile table
(`u8` index → profile descriptor), the per-page `ResidencyHint`
table (one byte per page, mining harmonius's "always-resident
bind-pose root / distance-bucket / hot-pose" tags), and the
content-hash of the source `MeshSource` (`u64`) for incremental
cook.

**Public-boundary invariants.**

1. **Magic + version checked first.** `PakReader` reads the
   magic + version + `FormatHash` before any other field; an
   unknown magic refuses with
   `geometry::Error::PakHeaderMagicMismatch`.
2. **All offsets in-range.** Page-table offset and `BLASRecipe`
   offset must be inside the mapped file; refuse with
   `geometry::Error::PakHeaderOffsetOutOfRange` if not.
3. **Fixed layout.** No optional fields, no variable-length
   prefix; layout drift = `FormatHash` bump = different pak.

##### 4.1.7.2 `BLASRecipe` — declarative BLAS-build description (value object)

**Reason to change:** how render is told to build the
acceleration structure for one `MeshSource` (which clusters
participate, geometry descriptors, BLAS flags). Distinct from
the runtime act of building the BLAS, which lives in `render`.

**Composition.** Cook-time blob describing exactly the
`render`-facing inputs to one BLAS build: a list of geometry
descriptors (vertex-buffer span, index-buffer span, format) one
per LOD0 `MeshletGroup`, the Metal `MTLAccelerationStructureFlags`
the cooker decided on (typically `FastTrace`), and the
material-slot index per descriptor so the bindless lookup
matches. The recipe references the LOD0 cluster band exclusively
— no other LOD band participates in BLAS, mining harmonius
`design/rendering/render-effects.md` F-2.5.1 / R-2.5.1.

**Public-boundary invariants.**

1. **Lists exactly the LOD0 cluster band.** No coarser band
   participates in the BLAS; render's TLAS sees the finest
   geometry only. Cross-aggregate invariant repeated in §4.2.
2. **Self-contained — no GPU calls inside.** The recipe
   describes inputs as offsets into the pak's vertex/index
   bytes; geometry never invokes a GPU API. `render`'s
   `RTAccelStructures` (peer SPEC §4.1.8) reads the recipe and
   issues the `MTLAccelerationStructure` build.
3. **Deterministic ordering.** Geometry descriptors are emitted
   in stable iteration order over LOD0 groups (group index
   ascending) so identical paks build identical BLAS bytes
   given the same Metal driver.

#### 4.1.8 `CookManifest` — per-mesh build record (entity, cook-time only)

**Reason to change:** what the cooker tracks for incremental cooks
(source path, pak path, format hash, options, content hash).

**Composition.** A small record per `MeshSource` recording the
absolute source-asset path, the emitted pak path, the
`FormatHash` the cooker produced against, the cook-time options
in effect (meshoptimizer flags, Draco quantisation profile,
target page size, target `LODBand` count), and the source
content-hash gating incremental rebuild. Stored alongside the
pak (next to the .pak file) and consumed by the cook driver to
decide whether a re-cook is needed.

**Identity & lifetime.** One per pak; lifetime equals the lifetime
of the cooked artefact on disk; runtime never reads
`CookManifest`.

**Public-boundary invariants.**

1. **Incremental-cook trigger is the source content-hash.** A
   cook is skipped iff the source hash matches the manifest's
   recorded hash AND the recorded `FormatHash` matches the
   engine's compiled `FormatHash`; either mismatch forces a
   re-cook.
2. **Manifest is the cook-time mirror of `PakHeader`.** Anything
   the runtime checks against `PakHeader` has its cook-time
   counterpart in `CookManifest`; a missing manifest field
   refuses cook rather than emitting an under-specified pak.
3. **Cook-time only.** The runtime plugin does not read
   `CookManifest`; it is exclusively the cooker driver's
   bookkeeping.

#### 4.1.9 `MeshHandle` — opaque mesh identifier (value object)

**Reason to change:** the public-facing mesh identity the engine
plumbs through ECS, `RenderProxy`, `BLASRecipe` consumption, and
material binding. One seam.

**Composition.** A 64-bit packed `(u32 index, u32 generation)`:
`index` selects a row in `GeometryRegistry`'s `MeshHandle` table;
`generation` is bumped on slot reuse so stale handles compare
unequal even after the slot is recycled. No payload pointer
inside the handle — every consumer goes through `GeometryRegistry`.

**Identity & lifetime.** Issued by `GeometryRegistry::register_mesh`
when a pak is first mapped; remains stable for the life of the
mapping. Re-mapping the same pak (e.g. after a `content` reload)
issues a new handle with a bumped generation.

**Public-boundary invariants.**

1. **Stable and immutable.** A `MeshHandle` returned to a caller
   never changes meaning until its owning entry is unregistered
   (generation-bumped). Render and other consumers may cache
   `MeshHandle` values across frames freely.
2. **Comparable lock-free.** Equality and ordering are pure
   bitwise compares; no chain of heap reads. Render's
   `RenderProxy` SoA stores raw `MeshHandle`s without indirection.
3. **Stale handles fail safe.** Lookup with a generation
   mismatch returns `geometry::Error::MeshHandleStale`; consumers
   never deference a stale slot.

#### 4.1.10 `MeshletGroupHandle` — opaque LOD-node identifier (value object)

**Reason to change:** the granularity at which render's LOD-band
selector references geometry — one node of the cluster DAG.

**Composition.** A 64-bit packed `(u32 mesh_index, u32 group_index)`
or `(MeshHandle, u32 group_index)`, depending on the implementation
plan that lands; the public-boundary semantics are identical: one
handle identifies one `MeshletGroup` inside one `MeshHandle`'s pak.

**Public-boundary invariants.**

1. **Resolves to one `MeshletGroup`.** The handle is bijective with
   `(MeshHandle, group_index)` for the lifetime of the parent
   `MeshHandle`'s generation.
2. **Render-side is read-only.** Render's LOD selector resolves a
   handle to the bindless `GpuMeshBuffers` slice that backs the
   group's currently-resident pages; render never asks the
   registry to mutate a handle's residency directly — it consults
   `ResidencyState`.
3. **Group-coherent.** A handle whose backing `MeshletGroup` spans
   any non-resident `PakPage` resolves to a render-skip signal,
   not a partial buffer (§4.1.6 invariant 1 forbids the half-page
   case at cook time, so this resolves to "page not loaded yet").

#### 4.1.11 `PakReader` — runtime decode front-end (entity)

**Reason to change:** how the runtime maps and validates one pak
file before payload decode.

**Composition.** A thin `mmap` / file-mapping wrapper that holds
the validated `PakHeader` view, the page table, the `BLASRecipe`
view, and a non-owning span over the file bytes. Exposes a
`PakPage` iterator the decode pipeline drives. Does not own
heap allocations beyond the OS file mapping.

**Identity & lifetime.** One `PakReader` per loaded pak;
constructed by `GeometryRegistry::register_mesh`, destroyed when
the registry unmaps the pak.

**Public-boundary invariants.**

1. **Header-first, payload-after.** Public methods that touch
   payload are gated on `PakHeader` validation having returned
   success; calling them on an unvalidated reader returns
   `geometry::Error::PakReaderUnvalidated` (a contract-violation
   signal, not an expected runtime path).
2. **Read-only mapping.** The pak file is mapped read-only; the
   reader never writes back to the mapping.
3. **`FormatHash` enforced.** Construction refuses on
   `FormatHash` mismatch (§4.1.7 invariant 1); a reader is only
   handed out when its pak is loadable.
4. **Lock-free for residency reads.** Residency-related reads
   (page table offset lookup, `ResidencyHint` byte fetch) hit
   only the immutable header and the page table — no shared
   mutable state inside `PakReader`.

#### 4.1.12 `DecodePool` — Draco decode scratch arena (entity)

**Reason to change:** how decode scratch buffers are sized and
recycled. Bounded; doesn't drag in scheduler or render decisions.

**Composition.** A pool of pre-sized scratch buffers (one set per
attribute kind: positions / indices / normals / tangents / UVs /
colours) sized at engine init from the maximum per-attribute
decode requirement across all loaded `PakHeader`s. The pool
issues a slot to a decode worker, the worker decodes into the
slot, the decoded bytes are copied (via the platform-native
upload path) into `GpuMeshBuffers`, then the slot returns to
the pool. Sizing is **never** changed mid-frame.

**Identity & lifetime.** One pool per process, owned by
`GeometryRegistry`. Sized at init; resized only at engine
shutdown / restart. Slots are reused per-frame; the pool itself
persists across frames.

**Public-boundary invariants.**

1. **Sized at startup, never mid-frame.** `DecodePool` capacity
   is determined from the union of every loaded `PakHeader`'s
   per-attribute scratch maxima; loading a pak whose
   requirements exceed the pool refuses load with
   `geometry::Error::DecodePoolUndersized` rather than
   reallocating mid-frame. Re-sizing requires engine restart.
2. **Slot acquisition is bounded-wait.** A decode worker that
   cannot acquire a slot returns the page to the scheduler's
   pending queue with `geometry::Error::DecodePoolBusy`; no
   worker spins or allocates around the contention.
3. **Decoded bytes byte-equal across hosts.** A decode against
   a `DracoStream` with profile `P` and the cook-time profile
   table from `PakHeader` produces byte-equal vertex bytes
   regardless of host (cross-aggregate invariant repeated in
   §4.2).

#### 4.1.13 `ResidencyState` — per-page residency table (entity)

**Reason to change:** how residency is tracked across frames
between geometry, content (scheduler), and render.

**Composition.** A lock-free table keyed by `(MeshHandle,
page_index)` mapping to a `ResidencyState` enum value:
`NotResident` / `Pending` / `Resident` / `Evicting`. Reads are
lock-free atomics so render's LOD selector and content's
scheduler can both consult the table without taking locks.
Writes are serialised through `GeometryRegistry` (§4.1.14) at
phase 7 mutation points only.

**Identity & lifetime.** One table per process, owned by
`GeometryRegistry`; persists across frames; entries are added
on `register_mesh` and removed on `unregister_mesh`.

**Public-boundary invariants.**

1. **Monotonic transitions per frame.** Within a single frame, a
   page's `ResidencyState` transitions only along the legal
   chain `NotResident → Pending → Resident` or `Resident →
   Evicting → NotResident`; backwards transitions inside a
   single frame are refused. This is what makes render's
   LOD-band selector see a consistent residency view between
   phase 6 (read) and phase 7 (consume). Cross-aggregate
   invariant repeated in §4.2.
2. **Lock-free reads.** Render and content both read the table
   without locking; reads observe atomic snapshots only.
3. **Writes happen inside phase 7.** State changes are applied
   inside the geometry-owned phase-7 mutation point (the same
   phase render submits in); no other phase observes a half-
   updated transition.
4. **`ResidencyHint` does not mutate at runtime.** The hint
   byte is read from `PakHeader` and never overwritten; only
   the *state* mutates. The hint informs the scheduler's
   weighting; geometry never adjudicates budget.

#### 4.1.14 `GeometryRegistry` — runtime aggregate root (aggregate root)

**Reason to change:** the public-boundary surface every other
plugin calls into — `MeshHandle` issuance, `MeshletGroupHandle`
resolution, `ResidencyState` reads, `GpuMeshBuffers` lookup.

**Composition.** Owns the `MeshHandle` table (slot array,
generation array, backing `PakReader` per slot), the
`ResidencyState` table (§4.1.13), the `DecodePool` (§4.1.12),
the `GpuMeshBuffers` allocator (§4.1.15), and the integration
hooks `content` calls when a page completes loading. Every
public geometry API entry point routes through this aggregate.

**Identity & lifetime.** Engine-singleton; constructed during
`core`'s init phase; destroyed during shutdown. No second
instance.

**Public-boundary invariants.**

1. **Single owner of the handle table.** No other plugin holds
   write access; `MeshHandle` issuance is centralised so
   generation bumping cannot race.
2. **All public APIs return `glibre::Result<T>`.** Every
   fallible operation surfaces `geometry::Error` per
   `reviews/decisions/error-model.md`; geometry adds a single
   arm to `glibre::Error`'s variant.
3. **Writes serialised at phase 7 boundary.** Concurrent reads
   (render's LOD selector, content's scheduler) are lock-free;
   writes (residency transitions, handle issuance) happen
   inside phase 7's geometry mutation point so the post-write
   state is visible to phase-9 present and to the next frame's
   phase 6 cull-extract.
4. **Boundary opacity.** No method exposes raw `Meshlet` /
   `MeshletGroup` records to callers; everything crosses the
   plugin boundary as opaque handles or as the
   geometry-owned `GpuMeshBuffers` view (§4.1.15) bound
   bindlessly.

#### 4.1.15 `GpuMeshBuffers` — handle-only GPU buffer aggregate (entity)

**Reason to change:** how decoded vertex / index / attribute
bytes are exposed to render. Geometry stores the **handles**;
the actual `MTLBuffer` upload is a render-context concern
called via the platform-native upload path. This split keeps
geometry render-API-agnostic.

**Composition.** Per `MeshHandle` and per currently-resident
`LODBand`: a set of opaque buffer handles (one per attribute
stream + one for indices) that geometry materialises by asking
the `render`-vended buffer allocator to upload the decoded
bytes from `DecodePool` slots. Geometry retains the **handles**
(stable identifiers consumed bindlessly via `MaterialHandle`
indirection); geometry never holds an `MTLBuffer*`. The
upload itself is a one-shot copy declared by render; geometry
provides the source bytes and receives the handle.

**Identity & lifetime.** One `GpuMeshBuffers` record per
`(MeshHandle, LODBand)` band that has at least one resident
page; lifetime ends when every page in the band evicts. The
buffer-handle slot returns to render's allocator at that point.

**Public-boundary invariants.**

1. **Geometry holds handles, render owns memory.** No
   `MTLBuffer*` lives inside `GpuMeshBuffers`; geometry stores
   only the opaque handles render allocates. Geometry's
   responsibility is "what bytes go where"; render's is "where
   on the GPU".
2. **Bindless lookup-only.** Render binds the buffers
   bindlessly via `MaterialHandle` indirection; the
   `GpuMeshBuffers` record is a lookup table, not a binding
   point.
3. **Decode-once-per-band.** Multiple visible LOD-cuts that
   share a band see one resident `GpuMeshBuffers` record; no
   duplicate decode.
4. **Lifetime tied to residency.** A `GpuMeshBuffers` record
   exists iff at least one page in its band is `Resident`; the
   `ResidencyState` table is the source of truth.

### 4.2 Cross-aggregate invariants

Invariants that span more than one aggregate and must hold at every
public boundary at the seams between them:

1. **Stable `FormatHash` per cooked mesh.** Every `MeshletPak`'s
   `PakHeader.FormatHash` is derived from the pak schema version
   in effect when the pak was cooked; the runtime refuses any pak
   whose hash does not match the engine's compiled-in value
   (`geometry::Error::PakFormatHashMismatch`). This is the single
   gate for hot-reload schema drift refusal (§8) and the only
   schema-evolution mechanism geometry exposes — there is no
   migration path inside a `FormatHash` change. Changing
   `FormatHash` requires a new pak cook.

2. **`ClusterDAG` is acyclic with monotonically-tightening
   `LODBand` SSE.** Across `ClusterDAG` (§4.1.5) and the
   `MeshletGroup` records (§4.1.4) it points at, the topology is
   a DAG and the per-group `ScreenSpaceError` strictly tightens
   as `LODBand` decreases (finer = lower SSE). This is what makes
   the runtime LOD-band selector total: any view with a pixel
   threshold `T` admits exactly one cut of the DAG that respects
   group edges and yields the coarsest band whose every group has
   `SSE ≤ T`. Violation of either property refuses pak cook.

3. **`BLASRecipe` lists exactly the LOD0 cluster band.**
   `BLASRecipe` (§4.1.7.2) emits geometry descriptors for the
   LOD0 `MeshletGroup` cover only; no coarser band participates
   in any BLAS build. Render's `RTAccelStructures` (peer SPEC
   `specs/render/SPEC.md` §4.1.8) consumes the recipe under the
   same assumption — TLAS sees the finest geometry only,
   independent of per-frame LOD-band choice.

4. **Meshlet ≤ 64 vertices / ≤ 124 primitives.** Every `Meshlet`
   (§4.1.3) record packs no more than 64 unique vertices and 124
   triangles; this is the size of the Metal 4 mesh-shader payload
   and the meshoptimizer cluster default we mined from harmonius
   R-3.1.1. Cooker rejects oversize clusters with
   `geometry::Error::MeshletOversize`; the pak format physically
   cannot represent them (count fields are `u8` with range
   checks at `PakReader` time).

5. **Draco decode produces byte-equal vertex data across hosts.**
   `DracoStream` decode (§4.1.6.1) into `DecodePool` (§4.1.12)
   produces byte-equal vertex bytes on every supported host given
   the same compressed input and the same cook-time quantisation
   profile from `PakHeader`. Combined with byte-equal cooks
   (§4.1.7 invariant 2), this is what closes PHILOSOPHY §7
   (deterministic byte-equal artefacts) at the geometry boundary.
   The unit test
   `decompresses-byte-equal-on-multiple-hosts` (§11) is the
   contract test.

6. **`ResidencyState` transitions are monotonic per-frame.**
   Within a single frame, every `ResidencyState` (§4.1.13) entry
   transitions only along `NotResident → Pending → Resident` or
   `Resident → Evicting → NotResident`; no backwards transition is
   observable inside a frame. This means render's LOD-band
   selector (in phase 6) and `GpuMeshBuffers` resolver (in phase
   7) see the same residency snapshot across the same frame, even
   though the table itself is mutated by `content`'s scheduler
   between frames.

7. **Per-context error model honoured.** Every aggregate's public
   fallible operation returns `glibre::Result<T, glibre::Error>`
   per `reviews/decisions/error-model.md`; geometry's enum lives
   in the `geometry::Error` arm cited there and is the only
   geometry-internal error surface. Variants enumerated in §10
   stay in lockstep with the §4 invariant prose above.

8. **Frame-phase ownership (per `reviews/decisions/frame-phases.md`
   and `reviews/decisions/perf-budget.md`).** Cook-time aggregates
   (§4.1.1 – §4.1.8) live exclusively inside the cook driver
   process — never inside the engine runtime. Runtime aggregates
   (§4.1.9 – §4.1.15) participate in the frame as follows:
   `MeshHandle` and `MeshletGroupHandle` issuance happens at
   `register_mesh` time (any phase, but typically phase 1 / 8 in
   editor); `ResidencyState` reads happen inside phase 6
   (cull-extract reads what's resident for LOD-band selection)
   and phase 7 (render consumes resolved `GpuMeshBuffers`);
   `ResidencyState` writes and `GpuMeshBuffers` materialisation
   happen at the geometry-owned phase 7 mutation point
   (alongside `BLAS` refits per the perf-budget table). No
   geometry aggregate is mutated inside phases 1–5 or phase 9.

9. **Opaque-handle boundary.** Geometry's public API exposes only
   opaque handles (`MeshHandle`, `MeshletGroupHandle`,
   `MaterialSlot`-as-opaque-`MaterialHandle`-index) and the
   geometry-owned `GpuMeshBuffers` lookup view (consumed
   bindlessly, never inspected). No raw `Meshlet`,
   `MeshletGroup`, `PakPage`, or `DracoStream` record crosses
   the plugin boundary. This is PHILOSOPHY §3 (plugin-only
   growth, opaque handles at every plugin boundary) applied
   uniformly.

10. **Geometry never enqueues GPU work.** Geometry computes
    bytes and issues handles; render submits draws and builds
    BLAS. The `BLASRecipe` blob is the *only* artefact crossing
    that seam, and it is declarative — render reads it and
    issues its own GPU calls. This is the SRP split mined as
    §3.2 collapse #5.

## 5. Public Interface

The header stub below is the §5 deliverable: every symbol that crosses
the geometry plugin's public boundary, declared in one C++23 header and
verified via `clang++ -std=c++23 -fsyntax-only -Wall -Wextra -Wpedantic`.
Bodies live inside the geometry dylib (cook-time stages link the same
header from the cooker driver); this header is the contract every caller
(core, content, render, editor) compiles against. Cross-context
invariants enforced here:

- Every fallible operation returns `glibre::Result<T>` per
  `reviews/decisions/error-model.md`. The geometry-internal `Error` enum
  is the closed sum cited in §10 below; it is rolled into
  `glibre::Error`'s variant in `core`. `-fno-exceptions` is enforced
  globally; this header obeys.
- Aggregates listed in §4 (`MeshletPak`, `PakReader`, `DecodePool`,
  `GeometryRegistry`, `GpuMeshBuffers`) are forward-declared classes
  whose layout is owned inside the plugin. Cook-time aggregates
  (`MeshSource`, `OptimisedMesh`, `Meshlet`, `MeshletGroup`,
  `ClusterDAG`, `BLASRecipe`, `MeshletPak`, `PakPage`, `PakHeader`,
  `DracoStream`, `CookManifest`) are likewise opaque to runtime callers;
  the cooker driver consumes them through the same opaque types.
  Callers manipulate them only through the methods exposed below.
- Resource handles (`MeshHandle`, `MeshletGroupHandle`,
  `MaterialHandle`, `GpuBufferHandle`) are 64-bit generational
  `Handle<Tag>` values with no payload pointers; this avoids ABI fixup
  on hot-reload (PHILOSOPHY §8 + §9). The tag types are empty structs so
  handles addressing different aggregates are distinct types and cannot
  be cross-assigned. `MeshHandle` and `MaterialHandle` tags share names
  with the render plugin's tag namespace so the same value travels both
  contexts without translation (cross-aggregate invariant 9, §4.2).
- `BoundingSphere` / `BoundingCone` are public value types: render's
  cluster culler reads these per-`MeshletGroup` records via
  `MeshletGroupHandle` resolution. They are the only meshlet-internal
  numbers that ever cross the plugin boundary (otherwise §4.2 invariant
  9 holds — no raw `Meshlet` / `MeshletGroup` records leak).
- `ResidencyState` is a closed enum and the source of truth for
  per-page residency; reads are lock-free atomics at the implementation
  level (§4.1.13). `ResidencyHint` is cook-time-baked and never mutates
  at runtime.
- `GpuMeshBuffers` exposes only opaque `GpuBufferHandle` slots; geometry
  never holds an `MTLBuffer*`. The render plugin owns GPU memory and
  vends the upload path; geometry wires decoded bytes through it
  (§4.1.15, cross-aggregate invariant 10).
- The cook-time pipeline's stage entry points (`cook_mesh`,
  `cook_is_up_to_date`, `inspect_pak`) live behind the
  `GLIBRE_GEOMETRY_COOK` macro guard so the runtime dylib does not pull
  cook-time meshoptimizer / Draco link symbols.

The header has no event types in MVP — geometry publishes nothing back
into the ECS event bus; runtime mutation points are inside phase 7 and
the parent registry's caller drives them directly. No Fory schemas live
in this surface either: `MeshletPak` is the on-disk format and is
schema-versioned by `FormatHash` rather than serialised through Fory,
and `CookManifest` is the cooker's bookkeeping side-table (also outside
Fory). Geometry's contribution to telemetry is the structured
per-context error enum returned through `Result<T>` and consumed by
`glibre::log_error`.

```cpp
// SPDX-License-Identifier: Apache-2.0
// glibre — geometry plugin public interface (header-only stub).
//
// This file is the §5 deliverable of `specs/geometry/SPEC.md`. It declares
// every symbol crossing the geometry plugin's public boundary. The bodies
// live inside the geometry dylib; cook-time stages link the same header
// from the cooker driver, gated by GLIBRE_GEOMETRY_COOK.
//
// Cross-context invariants embedded here:
//   * Every fallible call returns `glibre::Result<T>` per
//     `reviews/decisions/error-model.md`. `-fno-exceptions` is enforced
//     globally; this header obeys.
//   * Aggregates are opaque — `MeshletPak`, `PakReader`, `DecodePool`,
//     `GeometryRegistry`, `GpuMeshBuffers`, and the cook-time stage
//     records are forward-declared classes whose layout is owned inside
//     the plugin.
//   * Resource handles are 64-bit generational `Handle<Tag>` values with
//     no payload pointers; this avoids ABI fixup on hot-reload.
//   * `MeshHandle` / `MaterialHandle` tag types match render's so the
//     same handle value travels both contexts without translation.
//   * Cook-time entry points are guarded by GLIBRE_GEOMETRY_COOK; the
//     runtime dylib never pulls meshoptimizer / Draco link symbols.
//
// This stub compiles standalone with
// `clang++ -std=c++23 -fsyntax-only -Wall -Wextra -Wpedantic`.

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

namespace glibre {

// -----------------------------------------------------------------------
// Stand-in declarations from sibling contexts. The real definitions live
// in `core/include/glibre/error.hpp`, `render/include/...`, etc.; this
// header forward-declares them so the stub compiles in isolation. The
// implementation .cpp files include the real headers, not these stubs.
// -----------------------------------------------------------------------

#if !defined(GLIBRE_HAVE_CORE_ERROR)
namespace core {
enum class Error : std::uint16_t {
    PluginAbiHashMismatch,
    PluginInitFailed,
    SchemaMigrationFailed,
    HotReloadRefused,
    FramePhaseMisordered,
    OutOfBudget,
};
}  // namespace core

struct ErrorContext {
    std::string_view file;
    int              line  = 0;
    std::string_view detail;
};

class Error {
public:
    using Variant = std::variant<core::Error /*, geometry::Error inserted in core */>;

    template <class E>
    constexpr Error(E e, ErrorContext ctx = {}) noexcept
        : variant_{e}, ctx_{ctx} {}

    constexpr const Variant&      code() const noexcept  { return variant_; }
    constexpr const ErrorContext& where() const noexcept { return ctx_; }

private:
    Variant      variant_;
    ErrorContext ctx_;
};

template <class T>
using Result = std::expected<T, Error>;
#endif  // GLIBRE_HAVE_CORE_ERROR

// -----------------------------------------------------------------------
// geometry::Error — closed sum of every geometry-internal failure mode.
// Every public geometry boundary returns Result<T> over this enum
// (rolled into glibre::Error's variant per
// reviews/decisions/error-model.md). The list is closed: adding a
// variant is an ABI bump.
// -----------------------------------------------------------------------

namespace geometry {

enum class Error : std::uint16_t {
    // Cook-time — MeshSource validation (§4.1.1)
    MeshSourceInvalidTopology,
    AttributeLayoutMismatch,
    MeshSourceEmpty,

    // Cook-time — meshoptimizer / cluster build (§4.1.2 .. §4.1.5)
    MeshoptStageFailed,
    MeshletOversize,
    MeshletBoundsInvalid,
    ClusterDAGCycle,
    LODBandSseNonMonotonic,
    LOD0CoverIncomplete,

    // Cook-time — Draco encode (§4.1.6.1)
    DracoEncodeFailed,
    DracoProfileUnknown,

    // Cook-time — BLASRecipe (§4.1.7.2)
    BLASRecipeInvalid,

    // Cook-time — pak emission (§4.1.7, §4.1.8)
    PakWriterIoFailed,
    PakPageOversize,
    CookManifestInvalid,
    CookManifestMissingField,

    // Runtime — pak load + header validation (§4.1.7, §4.1.7.1, §4.1.11)
    PakHeaderMagicMismatch,
    PakFormatHashMismatch,
    PakHeaderOffsetOutOfRange,
    PakReaderUnvalidated,
    PakIoFailed,
    PakPageIntegrityFailed,

    // Runtime — decode pool (§4.1.12)
    DecodePoolUndersized,
    DecodePoolOverflow,
    DecodePoolBusy,
    DracoDecodeFailed,

    // Runtime — handle / registry (§4.1.9, §4.1.10, §4.1.14)
    MeshHandleStale,
    MeshletGroupHandleStale,
    MeshHandleNotFound,
    MeshAlreadyRegistered,

    // Runtime — residency (§4.1.13)
    ResidencyTransitionIllegal,
    ResidencyHintImmutable,
    PageNotResident,

    // Runtime — GPU buffer materialisation (§4.1.15)
    GpuUploadRefused,
    GpuBufferAllocFailed,
};

[[nodiscard]] constexpr std::string_view to_string(Error e) noexcept;

// -----------------------------------------------------------------------
// Format / quantisation enums. Bytes-on-disk are determined by these +
// the cooker's recorded options; the runtime checks them at PakHeader
// validation time and never branches on them in the decode hot path.
// -----------------------------------------------------------------------

enum class FormatHash : std::uint64_t { Unknown = 0u };

enum class DracoQuantisationProfile : std::uint8_t {
    // Closed list owned by geometry; one entry per attribute-quantisation
    // tuple shipped at MVP. Profile 0 is reserved as "decoder rejects".
    Reserved   = 0,
    Standard   = 1,  // 14-bit positions, 10-bit normals, 12-bit UVs.
    HighFi     = 2,  // 16-bit positions, 12-bit normals, 14-bit UVs.
    LowFi      = 3,  // 11-bit positions, 8-bit normals, 10-bit UVs.
};

enum class AttributeKind : std::uint8_t {
    Position,
    Index,
    Normal,
    Tangent,
    UV0,
    UV1,
    Color,
    JointIndices,   // opaque; geometry never inspects (animation owns).
    JointWeights,   // opaque; geometry never inspects (animation owns).
};

inline constexpr std::size_t kAttributeKindCount = 9u;

// -----------------------------------------------------------------------
// Generational handles. 64 bits, packed { generation : 24, index : 40 }.
// Tag types are empty structs so handles to different aggregates are
// distinct types and cannot be cross-assigned. The `mesh` and `material`
// tags match the render plugin's tag namespace (cross-aggregate
// invariant 9, §4.2) so the same value travels both contexts.
// -----------------------------------------------------------------------

namespace tags {
struct mesh             {};
struct meshlet_group    {};
struct material         {};
struct gpu_buffer       {};
struct pak              {};
struct pak_page         {};
}  // namespace tags

template <class Tag>
class Handle {
public:
    using value_type = std::uint64_t;

    constexpr Handle() noexcept = default;
    explicit constexpr Handle(value_type v) noexcept : bits_{v} {}

    [[nodiscard]] constexpr value_type    raw()        const noexcept { return bits_; }
    [[nodiscard]] constexpr std::uint64_t index()      const noexcept { return bits_ & 0x000000FF'FFFFFFFFull; }
    [[nodiscard]] constexpr std::uint32_t generation() const noexcept { return static_cast<std::uint32_t>(bits_ >> 40); }
    [[nodiscard]] constexpr bool          valid()      const noexcept { return bits_ != 0u; }
    [[nodiscard]] friend constexpr bool operator==(Handle, Handle) noexcept = default;

private:
    value_type bits_ = 0u;
};

using MeshHandle         = Handle<tags::mesh>;
using MeshletGroupHandle = Handle<tags::meshlet_group>;
using MaterialHandle     = Handle<tags::material>;
using GpuBufferHandle    = Handle<tags::gpu_buffer>;
using PakHandle          = Handle<tags::pak>;
using PakPageHandle      = Handle<tags::pak_page>;

// Hash functors for handle keys (set/map storage in callers).
struct MeshHandleHash {
    [[nodiscard]] constexpr std::size_t operator()(MeshHandle h) const noexcept {
        return static_cast<std::size_t>(h.raw());
    }
};
struct MeshletGroupHandleHash {
    [[nodiscard]] constexpr std::size_t operator()(MeshletGroupHandle h) const noexcept {
        return static_cast<std::size_t>(h.raw());
    }
};

// -----------------------------------------------------------------------
// Bounding primitives — public value types. Emitted per-Meshlet /
// per-MeshletGroup at cook time; render's cluster culler reads them
// per-frame via MeshletGroupHandle resolution (§4.1.3 / §4.1.4 / §4.2
// invariant 9). These are the only meshlet-internal numbers that cross
// the plugin boundary; raw Meshlet records do not.
// -----------------------------------------------------------------------

struct BoundingSphere {
    float center_x = 0.0f;
    float center_y = 0.0f;
    float center_z = 0.0f;
    float radius   = 0.0f;
};

struct BoundingCone {
    // Axis is unit-length; cos_half_angle == -1.0 is the sentinel
    // meaning "do not backface-cull this cluster" (§4.1.3 invariant 3).
    float axis_x         = 0.0f;
    float axis_y         = 0.0f;
    float axis_z         = 0.0f;
    float cos_half_angle = -1.0f;
};

// -----------------------------------------------------------------------
// LOD band tier. 0 = finest. The runtime LOD-band selector compares the
// per-group ScreenSpaceError against the view's pixel threshold and
// picks the coarsest band whose every group has SSE ≤ T (§4.2 #2).
// -----------------------------------------------------------------------

enum class LODBand : std::uint8_t {
    LOD0 = 0,
    LOD1 = 1,
    LOD2 = 2,
    LOD3 = 3,
    LOD4 = 4,
    LOD5 = 5,
    LOD6 = 6,
    LOD7 = 7,
};

// -----------------------------------------------------------------------
// Residency — public to render and content. The state enum is closed;
// transitions are monotonic per-frame (§4.2 invariant 6). The hint is
// cook-time-baked into PakHeader and is read-only at runtime
// (§4.1.13 invariant 4).
// -----------------------------------------------------------------------

enum class ResidencyState : std::uint8_t {
    NotResident = 0,
    Pending     = 1,
    Resident    = 2,
    Evicting    = 3,
};

enum class ResidencyHint : std::uint8_t {
    None             = 0,
    AlwaysResident   = 1,  // bind-pose root / coarse impostor band.
    HotPose          = 2,  // expected to be resident under typical play.
    DistanceBucket0  = 3,  // 0–25m
    DistanceBucket1  = 4,  // 25–100m
    DistanceBucket2  = 5,  // 100–500m
    DistanceBucket3  = 6,  // > 500m
    OnDemand         = 7,  // load only when first observed.
};

// -----------------------------------------------------------------------
// Aggregates — opaque to the public interface. Implementations live
// inside the geometry dylib. Callers manipulate them only through the
// methods exposed here.
// -----------------------------------------------------------------------

class MeshletPak;        // §4.1.7  — cooked on-disk container (mapped at runtime).
class PakReader;         // §4.1.11 — runtime decode front-end.
class DecodePool;        // §4.1.12 — Draco decode scratch arena.
class GeometryRegistry;  // §4.1.14 — runtime aggregate root (engine singleton).
class DecodedBuffer;     // post-decode RAII view returned by DecodePool.

// Cook-time aggregates (linked only with GLIBRE_GEOMETRY_COOK defined).
class MeshSource;        // §4.1.1  — authored static-mesh input.
class OptimisedMesh;     // §4.1.2  — meshoptimizer-stage output.
class ClusterDAG;        // §4.1.5  — acyclic LOD chain.
class BLASRecipe;        // §4.1.7.2 — declarative BLAS-build description.
class CookManifest;      // §4.1.8  — per-mesh build record.

// -----------------------------------------------------------------------
// DecodedBuffer — RAII view over one DecodePool slot's bytes. Held for
// the duration of one upload; the slot returns to the pool when the
// buffer is destroyed (§4.1.12 invariant 2). Move-only.
// -----------------------------------------------------------------------

class DecodedBuffer {
public:
    [[nodiscard]] AttributeKind              kind()         const noexcept;
    [[nodiscard]] std::span<const std::byte> bytes()        const noexcept;
    [[nodiscard]] std::size_t                decoded_size() const noexcept;

    DecodedBuffer(DecodedBuffer&&) noexcept;
    DecodedBuffer& operator=(DecodedBuffer&&) noexcept;
    ~DecodedBuffer();

    DecodedBuffer(const DecodedBuffer&)            = delete;
    DecodedBuffer& operator=(const DecodedBuffer&) = delete;

protected:
    DecodedBuffer() noexcept;
};

// -----------------------------------------------------------------------
// PakReader — runtime decode front-end. Constructed via
// GeometryRegistry::register_mesh; lifetime is owned by the registry.
// Header validation (FormatHash, magic, in-range offsets) happens at
// construction; payload-touching methods are gated on validation
// success (§4.1.11 invariant 1).
// -----------------------------------------------------------------------

struct PakHeaderInfo {
    FormatHash    format_hash          = FormatHash::Unknown;
    std::uint16_t engine_version_major = 0u;
    std::uint16_t engine_version_minor = 0u;
    std::uint16_t engine_version_patch = 0u;
    std::uint32_t page_count           = 0u;
    std::uint64_t source_content_hash  = 0u;
};

class PakReader {
public:
    [[nodiscard]] Result<PakHeaderInfo> header()      const noexcept;
    [[nodiscard]] std::uint32_t         page_count()  const noexcept;
    [[nodiscard]] Result<ResidencyHint>
        page_hint(std::uint32_t page_index) const noexcept;
    [[nodiscard]] Result<std::span<const std::byte>>
        page_bytes(std::uint32_t page_index) const noexcept;

    ~PakReader();
    PakReader(const PakReader&)            = delete;
    PakReader& operator=(const PakReader&) = delete;

protected:
    PakReader() noexcept;
};

// -----------------------------------------------------------------------
// DecodePool — sized at engine init from the union of every loaded
// PakHeader's per-attribute scratch maxima. Slot acquisition is
// bounded-wait; a contention return surfaces DecodePoolBusy and the
// scheduler retries on the next frame (§4.1.12 invariant 2).
// -----------------------------------------------------------------------

struct DecodePoolDesc {
    std::array<std::size_t, kAttributeKindCount> per_attribute_max_bytes{};
    std::uint16_t slot_count_per_attribute = 0u;
};

class DecodePool {
public:
    [[nodiscard]] static Result<std::unique_ptr<DecodePool>>
        create(const DecodePoolDesc&) noexcept;

    [[nodiscard]] Result<DecodedBuffer>
        decode(const PakReader& reader,
               std::uint32_t    page_index,
               AttributeKind    kind) noexcept;

    [[nodiscard]] Result<void>
        accommodate(const PakHeaderInfo&) noexcept;  // refuses if undersized.

    ~DecodePool();
    DecodePool(const DecodePool&)            = delete;
    DecodePool& operator=(const DecodePool&) = delete;

protected:
    DecodePool() noexcept;
};

// -----------------------------------------------------------------------
// GpuMeshBuffers — handle-only view of one MeshHandle's currently-
// resident bands. Geometry stores only the opaque GpuBufferHandle slots
// allocated by the render plugin's buffer allocator; the handles are
// consumed bindlessly via MaterialHandle indirection (§4.1.15).
// -----------------------------------------------------------------------

struct GpuMeshBuffers {
    MeshHandle      mesh{};
    LODBand         band                 = LODBand::LOD0;
    GpuBufferHandle position_buffer{};
    GpuBufferHandle index_buffer{};
    GpuBufferHandle normal_buffer{};
    GpuBufferHandle tangent_buffer{};
    GpuBufferHandle uv0_buffer{};
    GpuBufferHandle uv1_buffer{};
    GpuBufferHandle color_buffer{};
    std::uint32_t   resident_group_count = 0u;
};

// -----------------------------------------------------------------------
// BLAS recipe view — read-only window into the cooked BLASRecipe blob a
// MeshHandle's pak ships. Render's RTAccelStructures consumes this when
// it builds the per-mesh BLAS (§4.1.7.2; §4.2 invariant 3 — LOD0 only).
// Geometry never invokes a GPU API; the recipe is declarative.
// -----------------------------------------------------------------------

enum class BLASGeometryFormat : std::uint8_t {
    Triangles32BitIndices,
    Triangles16BitIndices,
};

struct BLASGeometryDescriptor {
    GpuBufferHandle    vertex_buffer{};
    std::uint32_t      vertex_byte_offset = 0u;
    std::uint32_t      vertex_count       = 0u;
    std::uint32_t      vertex_stride      = 0u;
    GpuBufferHandle    index_buffer{};
    std::uint32_t      index_byte_offset  = 0u;
    std::uint32_t      index_count        = 0u;
    BLASGeometryFormat format             = BLASGeometryFormat::Triangles32BitIndices;
    MaterialHandle     material{};
};

enum class BLASBuildFlags : std::uint16_t {
    None              = 0u,
    PreferFastTrace   = 1u << 0,
    PreferFastBuild   = 1u << 1,
    AllowCompaction   = 1u << 2,
    AllowUpdate       = 1u << 3,
};
[[nodiscard]] constexpr BLASBuildFlags
    operator|(BLASBuildFlags a, BLASBuildFlags b) noexcept {
    using U = std::underlying_type_t<BLASBuildFlags>;
    return static_cast<BLASBuildFlags>(static_cast<U>(a) | static_cast<U>(b));
}

struct BLASRecipeView {
    std::span<const BLASGeometryDescriptor> geometries{};
    BLASBuildFlags                          flags = BLASBuildFlags::PreferFastTrace;
};

// -----------------------------------------------------------------------
// MeshletGroupView — public projection of one DAG node, returned by
// GeometryRegistry::resolve_group. Carries only the bounds, SSE, and
// material slot the render-side cluster culler needs; the underlying
// MeshletGroup record stays opaque (§4.2 invariant 9).
// -----------------------------------------------------------------------

struct MeshletGroupView {
    MeshletGroupHandle handle{};
    LODBand            band               = LODBand::LOD0;
    BoundingSphere     bounds{};
    BoundingCone       cone{};
    float              screen_space_error = 0.0f;
    MaterialHandle     material{};
    std::uint32_t      meshlet_count      = 0u;
    bool               fully_resident     = false;
};

// -----------------------------------------------------------------------
// Mesh registration — content's pak loader hands a memory-mapped slice
// to the registry; the registry constructs a PakReader, validates the
// header, sizes (or refuses) the DecodePool, and issues a MeshHandle.
// MeshSource metadata travels with the registration so the editor's
// content browser and telemetry can cite authoring information.
// -----------------------------------------------------------------------

struct MeshSourceMetadata {
    std::string_view source_path;     // e.g. "art/props/crate.fbx"
    std::string_view author;          // optional; logged on cook only.
    std::string_view tool_version;    // optional; cook driver tag.
};

struct MeshRegistrationDesc {
    std::span<const std::byte> pak_bytes;          // memory-mapped, read-only.
    std::string_view           pak_path;
    MeshSourceMetadata         source_metadata{};
};

// -----------------------------------------------------------------------
// GeometryRegistry — engine-singleton runtime aggregate root. All
// public boundary calls flow through here. Writes happen at the
// geometry-owned phase 7 mutation point; reads (resolve_group,
// gpu_buffers, residency) are lock-free (§4.1.14, §4.2 invariant 6).
// -----------------------------------------------------------------------

class GeometryRegistry {
public:
    [[nodiscard]] static Result<GeometryRegistry*> instance() noexcept;

    [[nodiscard]] static Result<std::unique_ptr<GeometryRegistry>>
        create(DecodePoolDesc initial_pool_desc) noexcept;

    // --- Mesh registration -------------------------------------------------
    [[nodiscard]] Result<MeshHandle>
        register_mesh(const MeshRegistrationDesc&) noexcept;

    [[nodiscard]] Result<void>
        unregister_mesh(MeshHandle) noexcept;

    [[nodiscard]] Result<PakHeaderInfo>
        pak_header(MeshHandle) const noexcept;

    [[nodiscard]] Result<BLASRecipeView>
        blas_recipe(MeshHandle) const noexcept;

    // --- LOD / cluster lookup ----------------------------------------------
    [[nodiscard]] Result<MeshletGroupHandle>
        select_lod_group(MeshHandle     mesh,
                         float          pixel_threshold,
                         BoundingSphere view_sphere) const noexcept;

    [[nodiscard]] Result<MeshletGroupView>
        resolve_group(MeshletGroupHandle) const noexcept;

    [[nodiscard]] Result<std::span<const MeshletGroupView>>
        lod0_groups(MeshHandle) const noexcept;

    // --- GPU buffers -------------------------------------------------------
    [[nodiscard]] Result<GpuMeshBuffers>
        gpu_buffers(MeshHandle, LODBand) const noexcept;

    // --- Residency control -------------------------------------------------
    [[nodiscard]] Result<ResidencyState>
        page_state(MeshHandle, std::uint32_t page_index) const noexcept;

    [[nodiscard]] Result<ResidencyHint>
        page_hint(MeshHandle, std::uint32_t page_index) const noexcept;

    [[nodiscard]] Result<void>
        request_residency(MeshHandle, std::uint32_t page_index) noexcept;

    [[nodiscard]] Result<void>
        request_eviction(MeshHandle, std::uint32_t page_index) noexcept;

    [[nodiscard]] Result<void>
        notify_page_decoded(MeshHandle               mesh,
                            std::uint32_t            page_index,
                            DecodedBuffer            position,
                            DecodedBuffer            index,
                            std::span<DecodedBuffer> attributes) noexcept;

    // --- Decode pool -------------------------------------------------------
    [[nodiscard]] DecodePool&       decode_pool() noexcept;
    [[nodiscard]] const DecodePool& decode_pool() const noexcept;

    ~GeometryRegistry();
    GeometryRegistry(const GeometryRegistry&)            = delete;
    GeometryRegistry& operator=(const GeometryRegistry&) = delete;

protected:
    GeometryRegistry() noexcept;
};

// -----------------------------------------------------------------------
// Cook-time interface — guarded by GLIBRE_GEOMETRY_COOK so the runtime
// dylib never links the meshoptimizer / Draco code paths. The cooker
// driver lives in `tools/cook/` and is the only translation unit that
// defines this macro before including this header.
// -----------------------------------------------------------------------

#if defined(GLIBRE_GEOMETRY_COOK)

namespace cook {

struct MeshoptOptions {
    bool         apply_vertex_cache_optimisation = true;
    bool         apply_overdraw_optimisation     = true;
    bool         apply_vertex_fetch_optimisation = true;
    float        overdraw_threshold              = 1.05f;
    std::uint8_t target_lod_count                = 6u;
    float        lod_error_threshold             = 0.01f;
};

struct MeshletBuildOptions {
    std::uint8_t max_vertices_per_cluster        = 64u;   // hard cap (§4.2 #4).
    std::uint8_t max_triangles_per_cluster       = 124u;  // hard cap (§4.2 #4).
    float        cluster_cone_weight             = 0.5f;
    float        screen_space_reference_distance = 1.0f;  // metres at 1080p.
};

struct DracoEncodeOptions {
    DracoQuantisationProfile profile = DracoQuantisationProfile::Standard;
    std::uint8_t             speed   = 5u;  // 0 = highest compression.
};

struct PakWriterOptions {
    std::uint32_t target_page_size_bytes = 64u * 1024u;
    std::uint32_t max_page_size_bytes    = 256u * 1024u;
    bool          enable_blas_recipe     = true;
};

struct CookOptions {
    MeshoptOptions      meshopt{};
    MeshletBuildOptions meshlet_build{};
    DracoEncodeOptions  draco{};
    PakWriterOptions    pak{};
    std::string_view    output_pak_path;
    std::string_view    manifest_path;
};

struct CookedMesh {
    std::string_view pak_path;
    FormatHash       format_hash         = FormatHash::Unknown;
    std::uint64_t    source_content_hash = 0u;
    std::uint32_t    page_count          = 0u;
    std::uint32_t    meshlet_group_count = 0u;
    std::uint8_t     lod_band_count      = 0u;
};

// One-shot cook. Source is an authored mesh wrapped in MeshSource (the
// content plugin emits one of these from FBX / glTF / OBJ); options
// drive every cook stage; output is written deterministically (§4.2 #4
// + PHILOSOPHY §7). The cooker driver consumes the result and writes
// CookManifest alongside the .pak file.
[[nodiscard]] Result<CookedMesh>
    cook_mesh(const MeshSource&         source,
              const MeshSourceMetadata& metadata,
              const CookOptions&        options) noexcept;

// Incremental-cook gate. Returns true iff the recorded manifest's
// content-hash + FormatHash match the engine's compiled hash; the
// driver skips re-cook in that case (§4.1.8 invariant 1).
[[nodiscard]] Result<bool>
    cook_is_up_to_date(std::string_view manifest_path,
                       std::uint64_t    source_content_hash,
                       FormatHash       engine_format_hash) noexcept;

// Pak inspection — used by the editor's content browser and by the
// CI determinism gate (cook on host A, cook on host B, byte-compare).
[[nodiscard]] Result<PakHeaderInfo>
    inspect_pak(std::span<const std::byte> pak_bytes) noexcept;

}  // namespace cook

#endif  // GLIBRE_GEOMETRY_COOK

}  // namespace geometry
}  // namespace glibre
```

**Event types.** Geometry publishes no events back into the ECS event
bus in MVP. Residency transitions are observed by render through
`GeometryRegistry::page_state` reads inside phase 6 / 7; the scheduler
(`content`) drives writes via `request_residency` / `request_eviction`
and `notify_page_decoded`. Hot-reload refusal events flow up through
the per-call `Result<T>` return — geometry does not own a bus channel.

**Serialised schemas (Fory).** None at this layer. `MeshletPak` is the
on-disk format and is schema-versioned by `FormatHash` rather than
serialised through Fory (§4.1.7.1). `CookManifest` lives next to each
`.pak` file as a small fixed-layout record consumed only by the cooker
driver (§4.1.8 invariant 3); it does not enter Fory either. The
runtime never reads `CookManifest`.

**Error types.** The closed sum `geometry::Error` declared above lists
every failure mode at every public geometry boundary. It is the §10
authority for failure-mode enumeration; new variants require an ABI
bump per `reviews/decisions/error-model.md`.

## 6. Internal Architecture

Non-binding sketch for implementers. The aggregates of §4 and the public
header of §5 are binding; the directory layout, host-tool topology, and
per-stage invocation order below are illustrative and exist so the
plan-leaf author has one obvious place to start. Reviewers should reject
deviations only when they violate §4 invariants, the §5 header, the
on-disk format locked in §7.2, or the hot-reload contract of §8.

### 6.1 Module layout

Geometry compiles to **two distinct artefacts** from the same source tree
— a runtime plugin `.dylib` shipped to end users and a host-only cook
driver `.bin` shipped only inside the editor / build farm. Both link the
same §5 public header; the cook side adds `-DGLIBRE_GEOMETRY_COOK` and
pulls in meshoptimizer + Draco encoders, neither of which the runtime
ever links (§5 cook-time guard, §6.5).

Source is split by SRP — one directory per "reason to change". Public
headers (the §5 deliverable + the cook-time entry points behind
`GLIBRE_GEOMETRY_COOK`) live under `geometry/include/glibre/geometry/`;
implementation under `geometry/src/`.

```
geometry/
  include/glibre/geometry/             # §5 surface (compiles standalone).
    geometry.hpp                       # The single header from §5.
    handle.hpp                         # Generational `Handle<Tag>` template.
    bounds.hpp                         # `BoundingSphere` / `BoundingCone`.
    residency.hpp                      # `ResidencyState` / `ResidencyHint`.
  src/
    pak/                               # Aggregate §4.1.7 — on-disk container.
                                       # SHIPPED (runtime + cook both link).
      header.{hpp,cpp}                 # `PakHeader` layout + magic / version.
      reader.{hpp,cpp}                 # `PakReader` — mmap, validate, iterate.
      writer.{hpp,cpp}                 # `PakWriter` — deterministic byte order.
      page_table.{hpp,cpp}             # Page index → byte-offset table.
      crc.{hpp,cpp}                    # Per-page CRC32 (§4.1.6 invariant 3).
      blas_recipe.{hpp,cpp}            # `BLASRecipe` blob layout (§4.1.7.2).
      format_hash.{hpp,cpp}            # `FormatHash` constant + schema sig.
    cook/                              # Aggregates §4.1.1 .. §4.1.8.
                                       # COOK-ONLY (excluded from runtime dylib).
      mesh_source.{hpp,cpp}            # `MeshSource` value type + topology check.
      meshopt_stage.{hpp,cpp}          # `OptimisedMesh` — meshoptimizer driver.
      meshlet_build.{hpp,cpp}          # `meshopt_buildMeshlets` + bounds + cone.
      cluster_dag.{hpp,cpp}            # `ClusterDAG` + LOD-band simplify chain.
      draco_encode.{hpp,cpp}           # Per-stream Draco encode + profile bind.
      blas_recipe_build.{hpp,cpp}      # Emit `BLASRecipe` from LOD0 cover.
      pak_emit.{hpp,cpp}               # Drive `pak/writer.cpp` from cooked state.
      cook_manifest.{hpp,cpp}          # `CookManifest` read / write / hash gate.
      cook_driver.{hpp,cpp}            # `cook_mesh` orchestrator (§5 entry).
    runtime/                           # Aggregates §4.1.9 .. §4.1.15.
                                       # SHIPPED (runtime dylib only).
      registry.{hpp,cpp}               # `GeometryRegistry` aggregate root.
      handle_table.{hpp,cpp}           # `MeshHandle` slot + generation array.
      decode_pool.{hpp,cpp}            # `DecodePool` scratch arena + workers.
      decode_worker.{hpp,cpp}          # Per-thread Draco decode loop body.
      residency_state.{hpp,cpp}        # `ResidencyState` table (lock-free).
      gpu_mesh_buffers.{hpp,cpp}       # `GpuMeshBuffers` lookup + render hook.
      page_loader.{hpp,cpp}            # Glue from `content` page-decoded → decode pool.
    plugin.{hpp,cpp}                   # Plugin entry: init / tick / migrate / shutdown.
tools/
  glibre-meshcc/                       # Host cook driver (§6.2). NOT shipped.
    main.cpp                           # CLI; loads importer; calls cook::cook_mesh.
    importer.{hpp,cpp}                 # FBX / glTF / OBJ → `MeshSource` adapter.
    cli.{hpp,cpp}                      # Arg parsing, manifest path resolution.
```

The split is the §4 aggregate roster lifted directly into directories
keyed by lifecycle: `pak/` is the on-disk seam shared between cook and
runtime, `cook/` owns every cook-time-only aggregate (§4.1.1–§4.1.8),
and `runtime/` owns every runtime-only aggregate (§4.1.9–§4.1.15). Each
directory owns one reason to change. Adding a new cook stage adds one
file under `cook/`; adding a new runtime resource role touches
`runtime/`; bumping the on-disk schema touches `pak/format_hash.cpp` and
nothing else.

### 6.2 Cook pipeline — `tools/glibre-meshcc`

Geometry's cook driver is a host-only executable, not a runtime plugin.
It links `geometry/src/cook/` + `geometry/src/pak/` + the third-party
encoders (meshoptimizer, Draco) and produces one `.glibre-pak` file plus
its sidecar `<pak>.manifest` (§4.1.8) per authored static mesh. The
runtime dylib never links any of these stages or their dependencies
(§6.5 shipping cuts).

Stage order — sequential, deterministic, single-threaded per mesh
(parallelism is across meshes, not within one):

1. **Import.** `tools/glibre-meshcc/importer.cpp` reads FBX / glTF / OBJ
   from the `content` cache and produces a `MeshSource` value
   (§4.1.1). Manifold-topology and stable-attribute-set checks
   (§4.1.1 invariants 2–3) run here; failures refuse cook with
   `geometry::Error::MeshSourceInvalidTopology` /
   `AttributeLayoutMismatch`.

2. **Meshopt stage.** `cook/meshopt_stage.cpp` runs the canonical
   meshoptimizer pass trio in fixed order — `meshopt_optimizeVertexCache`
   → `meshopt_optimizeOverdraw` → `meshopt_optimizeVertexFetch` — then
   produces the LOD-simplification chain via `meshopt_simplify` (and
   `meshopt_simplifySloppy` for the coarsest tail) per the
   `MeshoptOptions` baked into `CookOptions`. Output is `OptimisedMesh`
   (§4.1.2). Reproducible-flag path is mandatory; no randomised
   tie-breaking (§4.1.2 invariant 1, PHILOSOPHY §7).

3. **Meshlet build.** `cook/meshlet_build.cpp` calls
   `meshopt_buildMeshlets` against each LOD level's index stream with
   the §4.1.3 hard caps (`max_vertices ≤ 64`,
   `max_triangles ≤ 124`). Per-meshlet `BoundingSphere` and
   `BoundingCone` come from `meshopt_computeMeshletBounds`; per-meshlet
   `ScreenSpaceError` comes from a fixed pixel-projection formula at
   the cook-time reference distance (`MeshletBuildOptions
   .screen_space_reference_distance`). Output is the per-LOD `Meshlet`
   array.

4. **Cluster DAG.** `cook/cluster_dag.cpp` builds the cross-LOD
   `MeshletGroup` topology (§4.1.4 / §4.1.5) — groups partition each
   LOD level's meshlets, parent links connect a finer group to its
   coarser successor across band boundaries. The DAG-acyclicity,
   single-LOD0-cover, watertight-cut, and SSE-monotonicity invariants
   (§4.1.4–§4.1.5) are validated at this stage; violation refuses
   cook with the matching enum arm. The driver assembles the
   `ClusterDAG` aggregate root (§4.1.5).

5. **Draco encode.** `cook/draco_encode.cpp` walks each `MeshletGroup`
   and emits one `DracoStream` per attribute kind (positions, indices,
   normals, tangents, UVs, colours) using the `DracoQuantisationProfile`
   chosen in `DracoEncodeOptions`. The encoded bytes per group are
   buffered into a `PakPage` builder.

6. **BLAS recipe.** `cook/blas_recipe_build.cpp` walks the LOD0 cover
   and emits the declarative `BLASRecipe` blob (§4.1.7.2) — geometry
   descriptors, primitive ranges, BLAS build flags. Geometry never
   issues GPU calls; the recipe is the single seam where render reads
   and builds the actual acceleration structure (§4.2 invariant 10).

7. **Pak write.** `cook/pak_emit.cpp` drives `pak/writer.cpp` to
   serialise `PakHeader` + cluster-DAG bytes + `BLASRecipe` blob +
   `PakPage` array + page table into one `.glibre-pak` file with the
   §7.2 byte layout. `PakWriter` is byte-deterministic (PHILOSOPHY §7,
   §4.1.7 invariant 2). Per-page CRC32 trailers are emitted here
   (§4.1.6 invariant 3).

8. **Manifest write.** `cook/cook_manifest.cpp` writes the sidecar
   `<pak>.manifest` carrying source-asset path, output pak path, the
   `FormatHash` the cooker compiled against, the `CookOptions` in
   effect, and the source content hash (§4.1.8). The next cook reads
   this back and compares against the source's current content hash
   for incremental skip (§4.1.8 invariant 1).

The driver itself (`cook/cook_driver.cpp`) wires steps 1–8 behind the
public `cook::cook_mesh(...)` entry point declared in §5; the host CLI
in `tools/glibre-meshcc/main.cpp` is a thin argv → `CookOptions` shim.
Per-mesh cook is single-threaded; the build farm spawns one driver
process per worker and parallelism is at the mesh granularity. There is
no shared mutable state between concurrently-cooking meshes.

### 6.3 Runtime path — load, decode, residency

The runtime side is plugin-internal: `runtime/registry.cpp` is the
aggregate root every other plugin calls into via the §5 header, and
every other `runtime/` file is owned by it.

#### 6.3.1 Pak load (`register_mesh`)

`GeometryRegistry::register_mesh(pak_path)` is the single entry point
that turns a cooked `.glibre-pak` file into a live `MeshHandle`:

1. **mmap.** `pak/reader.cpp` opens the file read-only and maps it.
   The mapping is read-only for the life of the registration; geometry
   never writes back (§4.1.11 invariant 2).
2. **Header validation.** `pak/header.cpp` validates magic + version +
   `FormatHash` against the engine's compiled-in value (§4.1.7.1
   invariant 1, §4.1.7 invariant 1). Mismatch refuses with
   `PakHeaderMagicMismatch` / `PakFormatHashMismatch`. In-range checks
   on every offset in the header (page-table offset, `BLASRecipe`
   offset+length) gate further reads (`PakHeaderOffsetOutOfRange`).
3. **Page table read.** `pak/page_table.cpp` builds the
   `(page_index → byte_offset)` lookup from the validated header.
   The page table is part of the immutable mapping — no copy.
4. **Handle issuance.** `runtime/handle_table.cpp` allocates a slot,
   bumps generation, and returns a `MeshHandle` (§4.1.9). The slot
   record holds the owning `PakReader` and the residency-table base
   for this mesh.
5. **Residency seed.** `runtime/residency_state.cpp` initialises every
   page's `ResidencyState` to `NotResident` and copies the per-page
   `ResidencyHint` byte from `PakHeader` into the residency table for
   the scheduler's later weighting reads (§4.1.13 invariants 1, 4).
6. **DecodePool sizing check.** `runtime/decode_pool.cpp` consults the
   `PakHeader`'s per-attribute scratch maxima and refuses registration
   if the pool is too small (`DecodePoolUndersized`, §4.1.12 invariant
   1). The pool is sized at engine init; this check is read-only.

The `MeshHandle` is now stable until `unregister_mesh`. No payload bytes
have been touched.

#### 6.3.2 On-demand cluster decode (`DecodePool` worker path)

When `content`'s scheduler decides a `(MeshHandle, page_index)` should
become resident, it calls `GeometryRegistry::request_residency(mesh,
page)`. The flow:

1. **Transition `NotResident → Pending`.** The registry CASes the
   residency entry; on success it enqueues a decode job onto the
   `DecodePool`'s pending queue (§4.1.13 invariant 1, §4.1.12).
2. **Worker picks up.** A `DecodePool` worker thread (one of a small
   bounded pool, §6.4) acquires a per-attribute scratch slot. If no
   slot is free, it returns the job with `DecodePoolBusy` so the
   scheduler can re-issue (§4.1.12 invariant 2). No worker spins or
   allocates around contention.
3. **Page read.** The worker resolves the page byte range via the
   `PakReader`'s page table; reads the `PakPage` header, validates the
   per-page CRC32 (§4.1.6 invariant 3), and refuses on mismatch with
   `PakPageIntegrityFailed`.
4. **Per-stream decode.** For each `DracoStream` in the page, the
   worker decodes into the matching attribute's scratch slot using the
   `DracoQuantisationProfile` carried in the stream header. Decoded
   bytes are byte-equal across hosts (§4.1.6.1 invariant 1, §4.1.12
   invariant 3).
5. **GPU upload.** `runtime/gpu_mesh_buffers.cpp` hands the decoded
   spans to render's vended buffer allocator via the platform-native
   upload path; render returns one `GpuBufferHandle` per attribute
   stream + indices for this `(MeshHandle, LODBand)` band (§4.1.15).
   Geometry stores only the handles — no `MTLBuffer*` ever lives in
   geometry memory.
6. **Slot return.** `DecodedBuffer` destruction returns the scratch
   slot to the pool (§4.1.12 invariant 2). Worker is free to pick the
   next job.
7. **Transition `Pending → Resident`.** The registry, inside its
   phase-7 mutation point, CASes the residency entry and notifies
   `content` via `notify_page_decoded` so the scheduler can update its
   bookkeeping (§4.1.13 invariant 3, §4.2 invariant 6).

Eviction is the dual: `request_eviction` transitions `Resident →
Evicting` at the phase-7 mutation point, decrements the
`GpuMeshBuffers` band refcount, releases the buffer handles back to
render's allocator when the band's last page departs (§4.1.15
invariant 4), and finally CASes to `NotResident`.

#### 6.3.3 Render handoff (LOD-band selector hot path)

Phase 6's `cull-extract` (in render) reads `ResidencyState` per
`MeshletGroupHandle` to pick the coarsest band whose every constituent
group is `Resident`. Phase 7 then resolves each surviving handle to its
`GpuMeshBuffers` slice for bindless binding. Both reads are lock-free
atomics on the immutable registry tables (§4.1.13 invariant 2, §4.1.14
invariant 3) — no mutex acquisition on the render side.

### 6.4 Concurrency model

Geometry has two independent concurrency contexts; they do not share
threads.

- **Cook side (offline, host-only).** The cook driver is single-threaded
  per mesh; the build farm parallelises across meshes by spawning one
  `glibre-meshcc` process per worker. There is no shared mutable state
  between processes; output paths are per-mesh. meshoptimizer and Draco
  are called synchronously inside the driver. No fibers, no thread
  pool inside one cook job.

- **Runtime side (in-engine).** The decode pool owns a small bounded
  worker thread pool — sized at engine init from
  `reviews/decisions/perf-budget.md` (default = `min(4, hwconcurrency
  - render_threads - sim_threads)`; hard cap at 4 workers in MVP).
  Workers pull jobs off the residency-pending queue and decode into
  pre-allocated scratch slots; there is no work-stealing across pools
  and no recursive parallelism inside one decode job. Worker count
  never changes mid-frame (§4.1.12 invariant 1).

  Public-API reads (`page_state`, `resolve_group`, `gpu_buffers`) are
  lock-free atomics on the residency / handle tables; concurrent
  readers from render's phase 6 and content's scheduler observe atomic
  snapshots (§4.1.13 invariant 2, §4.1.14 invariant 3). Writes
  (residency transitions, handle issuance, `GpuMeshBuffers`
  materialisation) serialise through `GeometryRegistry`'s phase-7
  mutation point — exactly one writer thread, no contention with
  readers, and the post-write state is visible to the next frame's
  phase-6 cull-extract (§4.1.14 invariant 3, §4.2 invariant 8).

  No mutex is acquired on the render hot path. The decode-pool job
  queue is the only place where worker threads contend, and contention
  there is bounded by the small pool size.

### 6.5 Shipping cuts — what links into what

| Artefact                              | Links `pak/` | Links `runtime/` | Links `cook/` | Links meshoptimizer / Draco encoder | Links Draco decoder |
|---------------------------------------|:------------:|:----------------:|:-------------:|:-----------------------------------:|:-------------------:|
| Shipping runtime dylib (`geometry`)   | yes          | yes              | **no**        | **no**                              | yes                 |
| Editor build of the runtime dylib     | yes          | yes              | **no**        | **no**                              | yes                 |
| `tools/glibre-meshcc` host driver     | yes          | **no**           | yes           | yes                                 | **no**              |
| `tools/glibre-pakdump` (debug viewer) | yes          | **no**           | **no**        | **no**                              | yes (optional)      |

The cuts are enforced two ways. First, by directory: `cook/` is excluded
from the runtime dylib's CMake target. Second, by the `GLIBRE_GEOMETRY_COOK`
guard in §5 — every cook-time entry point in the public header is behind
that macro, so a runtime translation unit cannot call into `cook::*`
even if a stray include happens. This keeps the runtime dylib's link set
free of the meshoptimizer + Draco encoder symbols (the encoder side; the
decoder is needed at runtime and ships).

A consequence: a malformed pak whose header is fine but whose payload
needs encoder support to validate cannot be re-encoded inside the
runtime — the runtime refuses load and the editor's cook driver must
re-emit. This is intentional. Re-cook is the only path; runtime never
mutates a pak.

### 6.6 Cross-context handoffs

Geometry's seams to the rest of the engine, by responsibility:

- **`content` (asset scheduler).** Owns the residency budget and the
  load / evict decisions. Calls `GeometryRegistry::request_residency`
  / `request_eviction`; observes residency through `page_state` reads
  and `notify_page_decoded` callbacks. Geometry never adjudicates
  budget (§4.1.13 invariant 4).

- **`render`.** Owns GPU memory and BLAS construction. Vends a buffer
  allocator that geometry calls during decode upload (§4.1.15
  invariant 1). Reads `BLASRecipe` blobs from mapped paks during BLAS
  build (§4.2 invariant 10) — geometry never enqueues GPU work. Reads
  `MeshletGroupHandle` / `BoundingSphere` / `BoundingCone` /
  `ResidencyState` per-frame from the registry's lock-free surface;
  there is no per-frame mutex with render.

- **`core`.** Owns the plugin lifecycle (init, tick, migrate,
  shutdown), the `Result<T>` / `Error` plumbing, and the frame-phase
  ordering. `runtime/plugin.cpp` is the entry point; it constructs
  `GeometryRegistry` at init and registers the geometry phase-7
  mutation point (§4.2 invariant 8, `reviews/decisions/frame-phases.md`).

- **`animation` (post-MVP).** Reads per-vertex skinning attributes
  (`AttributeKind::JointIndices` / `JointWeights`) through `DecodedBuffer`
  views; geometry never inspects their semantics (§3.3 routing).
  Geometry's static-mesh path is unaffected by animation's presence.

- **Editor / debug.** `tools/glibre-pakdump` (post-MVP) reads paks via
  `pak/reader.cpp` and dumps human-readable summaries. It links the
  same `pak/` directory as the runtime, so any header-format drift it
  flags is also visible to the engine.

No cross-context abstraction is invented before two concrete users
exist (PHILOSOPHY anti-pattern). The above seams are the only ones
geometry exposes; everything else routes through `GeometryRegistry`'s
public surface declared in §5.

## 7. Persistence & Schemas

Geometry's persistence surface is split across two distinct media:

1. **The `MeshletPak` custom binary container** (`§4.1.7`) — the
   cooked-artefact byte stream. **Not Fory-serialised.** It is a
   fixed-layout, little-endian, `FormatHash`-gated mmap-friendly file
   format owned end-to-end by `geometry` (`§4.1.7` invariant 4 — header
   validated before any payload read). The pak's payload bytes (vertex
   streams, index streams, attribute streams, page bytes, the
   `BLASRecipe` blob, the cluster-DAG bytes) are produced by `PakWriter`
   and consumed by `PakReader` directly; Fory never touches them.
2. **A small set of Fory side-records** describing the build-time and
   tooling-facing metadata around each cooked pak. These compile into
   `glibre-types.dylib` per `reviews/decisions/fory-codegen.md` and
   are authored at `data/schemas/geometry/<Type>.fory`. FQNs are
   `glibre.geometry.<Type>`. Each schema ships with at least one
   Catch2 round-trip test under `tests/data/schemas/geometry/<Type>.cpp`
   per `specs/data/SPEC.md` §7.7.

The split is load-bearing. The pak format must be byte-equal across
hosts, mmap-friendly, and gated by a single content-of-schema hash
(`FormatHash`, `§4.1.7` invariant 1) so the runtime can refuse a pak
whose schema does not match its compiled-in expectation in O(1) before
any other byte is touched. Fory's tagged variable-length encoding
cannot satisfy those constraints — it would force a parse pass over
every page on load, defeat mmap residency, and admit the kind of
silent additive drift that `MeshletPak` precisely forbids. The Fory
side-records, by contrast, ride the same migration mechanic every
other glibre persistent type rides; they describe the pak rather than
*being* the pak.

This section pins (a) the inventory of Fory side-records (§7.1), (b)
the `MeshletPak` binary file format and the rules its `FormatHash`
encodes (§7.2), (c) migration rules for both surfaces (§7.3), and
(d) the explicit list of artefacts that are *not* persisted (§7.4).

### 7.1 Persistent Fory side-records

#### 7.1.1 `CookManifest` — per-mesh build record

**File:** `data/schemas/geometry/CookManifest.fory`
**FQN:** `glibre.geometry.CookManifest`
**Lifetime scope:** one per cooked `.pak`; written by the cook driver
alongside the pak file (`<pak-path>.manifest`); read by the cooker on
the next build to gate incremental cooks (`§4.1.8` invariant 1).
**Runtime never reads** (`§4.1.8` invariant 3).

```fory
schema glibre.geometry.CookManifest {
  version  1
  since    "0.1.0"

  // Identity & cook-incremental gating.
  field source_path          : string  tag 1  since 1
  field pak_path             : string  tag 2  since 1
  field source_content_hash  : u64     tag 3  since 1
  field format_hash          : u64     tag 4  since 1   // mirrors PakHeader.FormatHash
  field foryc_abi_hash       : u64     tag 5  since 1   // glibre_types_abi_hash at cook
  field cooker_version       : string  tag 6  since 1   // SemVer of cooker driver
  field cooked_at_unix_ms    : u64     tag 7  since 1

  // Cook-time options used (mirror of CookOptions in §5).
  field meshopt_overdraw_threshold       : f32  tag 10 since 1   default 1.05
  field meshopt_lod_error_threshold      : f32  tag 11 since 1   default 0.01
  field meshopt_target_lod_count         : u8   tag 12 since 1   default 6
  field meshopt_apply_vertex_cache_opt   : bool tag 13 since 1   default true
  field meshopt_apply_overdraw_opt       : bool tag 14 since 1   default true
  field meshopt_apply_vertex_fetch_opt   : bool tag 15 since 1   default true

  field meshlet_max_vertices             : u8   tag 20 since 1   // hard-frozen 64; see §7.3
  field meshlet_max_triangles            : u8   tag 21 since 1   // hard-frozen 124; see §7.3
  field meshlet_cone_weight              : f32  tag 22 since 1   default 0.5
  field meshlet_sse_reference_distance_m : f32  tag 23 since 1   default 1.0

  field draco_profile                    : u8   tag 30 since 1   // DracoQuantisationProfile
  field draco_speed                      : u8   tag 31 since 1   default 5

  field pak_target_page_size_bytes       : u32  tag 40 since 1   default 65536
  field pak_max_page_size_bytes          : u32  tag 41 since 1   default 262144
  field pak_enable_blas_recipe           : bool tag 42 since 1   default true

  // Cooker bookkeeping for diagnostics & roll-up reports.
  field cooked_meshlet_group_count       : u32  tag 50 since 1
  field cooked_lod_band_count            : u8   tag 51 since 1
  field cooked_page_count                : u32  tag 52 since 1
  field cooked_pak_size_bytes            : u64  tag 53 since 1
}
```

**Invariants** (echoing `§4.1.8`):

1. **Hash + format twin lock.** `(source_content_hash, format_hash,
   foryc_abi_hash)` is the cook-incremental key. A cook is skipped
   iff *all three* match the cooker's live values; any mismatch forces
   a full re-cook. Recording `foryc_abi_hash` makes the manifest
   self-invalidating across middleman bumps even when `format_hash`
   is unchanged, closing the same drift hole `PSOCacheRecord` closes
   for render (`specs/render/SPEC.md` §7.1.2 invariant 2).
2. **Frozen meshlet caps.** `meshlet_max_vertices == 64` and
   `meshlet_max_triangles == 124` (`§4.1.3` invariant 1). The fields
   are persisted for diagnostic completeness, **not** as tuneables —
   `Foryc` recognises these tags as frozen and the cooker rejects any
   loaded manifest whose values diverge (`Error::CookManifestInvalid`).
   See migration rule §7.3.4.
3. **Closed-set enums refuse unknown values.** `draco_profile` is a
   `u8` projection of `DracoQuantisationProfile` (`§5`). Loading a
   manifest whose value is outside the live closed set yields
   `Error::CookManifestInvalid` rather than silently coercing — the
   same closed-sum rule render applies to its enum-encoded `u8`s.
4. **Cook-time only.** No runtime path consumes `CookManifest`
   (`§4.1.8` invariant 3). The plugin loader does not read it; only
   the cook driver and the editor's content-browser do. Render and
   `geometry`'s runtime aggregate (`GeometryRegistry`) get every
   gating value they need from `PakHeader` (§7.2).
5. **Manifest is the cook-time mirror of `PakHeader`.** Every field
   in `PakHeader` that the runtime checks (`format_hash`,
   `source_content_hash`, the cook-time options that *parameterise*
   the pak's bytes) has a counterpart here. A manifest missing a
   field that `PakHeader` carries refuses cook with
   `Error::CookManifestMissingField` rather than emitting a pak
   under-described by its bookkeeping (`§4.1.8` invariant 2).

#### 7.1.2 `BLASRecipeRecord` — Fory projection of the pak's BLAS recipe

**File:** `data/schemas/geometry/BLASRecipeRecord.fory`
**FQN:** `glibre.geometry.BLASRecipeRecord`
**Lifetime scope:** one per cooked pak; emitted **alongside** the pak
under `<pak-path>.blas-recipe` for **editor / tooling / RT-shader
authoring** introspection. The runtime BLAS build path inside `render`
reads the recipe **from the pak's binary `BLASRecipe` block** (the
authoritative source per `§4.1.7.2`), not from this Fory side-record;
this file exists so external tools (Metal-shader authoring,
acceleration-structure profiler, content-browser preview) can read
the recipe without touching the pak's binary blob.

```fory
schema glibre.geometry.BLASRecipeRecord {
  version  1
  since    "0.1.0"

  // Identity — must match the owning pak's PakHeader.
  field pak_path                : string  tag 1 since 1
  field format_hash             : u64     tag 2 since 1
  field source_content_hash     : u64     tag 3 since 1

  // Recipe-level flags forwarded into the MTLAccelerationStructure build.
  field accel_struct_flags      : u32     tag 10 since 1   // MTLAccelerationStructureFlags
  field lod0_descriptor_count   : u32     tag 11 since 1

  // Per-LOD0-group geometry descriptors, sorted by ascending group_index.
  field descriptors             : list<BLASGeometryDescriptor>  tag 12 since 1
}

schema glibre.geometry.BLASGeometryDescriptor {
  version  1
  since    "0.1.0"

  field group_index             : u32  tag 1 since 1   // LOD0 MeshletGroup index
  field material_slot_index     : u32  tag 2 since 1   // bindless material id

  // Byte offsets into the owning pak's vertex / index regions.
  // The runtime resolves these against PakHeader-recorded base offsets;
  // the editor's recipe-viewer resolves against the same base offsets.
  field vertex_buffer_offset    : u64  tag 10 since 1
  field vertex_buffer_length    : u64  tag 11 since 1
  field vertex_stride_bytes     : u32  tag 12 since 1
  field vertex_format           : u8   tag 13 since 1   // closed-sum: pos-only / pos+normal / etc.

  field index_buffer_offset     : u64  tag 20 since 1
  field index_buffer_length     : u64  tag 21 since 1
  field index_format            : u8   tag 22 since 1   // closed-sum: u16 / u32

  field triangle_count          : u32  tag 30 since 1
}
```

**Invariants** (echoing `§4.1.7.2`):

1. **Authoritative source is the pak.** A `BLASRecipeRecord` whose
   `format_hash` or `source_content_hash` does not match the
   companion `<pak-path>` is `Error::BLASRecipeInvalid` and consumers
   discard the side-record, **not** the pak. The pak's binary
   `BLASRecipe` block remains canonical (`§4.1.7.2` invariant 1).
2. **LOD0-only.** `descriptors` lists exactly the LOD0 cluster band
   (`§4.1.7.2` invariant 1). Any descriptor whose `group_index`
   resolves to a non-LOD0 group at load time refuses with
   `Error::BLASRecipeInvalid`.
3. **Deterministic ordering.** `descriptors` is sorted by ascending
   `group_index`; out-of-order lists are `Error::BLASRecipeInvalid`
   at deserialise (`§4.1.7.2` invariant 3).
4. **Self-contained — no GPU calls.** The record describes inputs
   only. The `accel_struct_flags` value is a forward-projection of
   `MTLAccelerationStructureFlags`; render's `RTAccelStructures`
   peer reads either this side-record (tooling path) or the pak's
   binary block (runtime path) to issue the actual build
   (`§4.1.7.2` invariant 2).
5. **Closed-set enums.** `vertex_format` and `index_format` are
   closed sums; unknown values refuse, mirroring `CookManifest`
   invariant 3.

#### 7.1.3 `MeshSourceMetadata` — authoring provenance

**File:** `data/schemas/geometry/MeshSourceMetadata.fory`
**FQN:** `glibre.geometry.MeshSourceMetadata`
**Lifetime scope:** one per `MeshSource` registration; carried with
`MeshRegistrationDesc` (`§5`) into `GeometryRegistry::register_mesh`,
emitted to the editor's content browser, and logged at cook-time
into the cook-driver's audit trail. Persists separately from the pak
(under `<pak-path>.metadata`) so tooling can read it without mapping
the pak.

```fory
schema glibre.geometry.MeshSourceMetadata {
  version  1
  since    "0.1.0"

  field source_path        : string  tag 1 since 1   // e.g. "art/props/crate.fbx"
  field author             : string  tag 2 since 1   default ""
  field tool_version       : string  tag 3 since 1   default ""
  field source_content_hash : u64    tag 4 since 1   // matches PakHeader / CookManifest
  field authored_at_unix_ms : u64    tag 5 since 1   default 0
  field license_tag         : string tag 6 since 1   default ""   // free-form, content-policy
  field bounding_box_min    : vec3f  tag 10 since 1
  field bounding_box_max    : vec3f  tag 11 since 1
  field source_vertex_count : u32    tag 12 since 1
  field source_triangle_count : u32  tag 13 since 1
}
```

**Invariants:**

1. **Read-only metadata, never gating.** `MeshSourceMetadata` is
   informational. No runtime decision (residency, LOD selection,
   handle issuance) branches on its fields. The loader tolerates a
   missing or malformed metadata file by surfacing
   `Error::CookManifestMissingField` (re-used as the generic
   side-record absence arm) at the editor / telemetry boundary; the
   registry still issues a `MeshHandle`.
2. **Hash twins the pak.** `source_content_hash` MUST equal
   `PakHeader.source_content_hash` (`§4.1.7.1`). Mismatch routes the
   metadata to a per-load diagnostic warning; the warning is the
   editor's signal that source-tree drift has decoupled the
   provenance record from the cooked artefact.
3. **Bounding box is authoring-frame, not LOD-derived.** The box is
   the authored bind-pose AABB of the source geometry (`§4.1.1`
   invariant 1). It is *not* the LOD0 group cover or any cluster
   sphere — those live inside `MeshletPak` and are byte-equal across
   hosts (`§4.1.7` invariant 2). The metadata box is for the editor's
   content-browser preview only.

### 7.2 `MeshletPak` binary container — out-of-band format

`MeshletPak` (`§4.1.7`) is the cooked on-disk artefact; its byte
stream is **not Fory-serialised**. The format below is the load-bearing
contract every `PakWriter` emit and every `PakReader` map must obey;
its content-of-schema hash is `FormatHash` (`§4.1.7` invariant 1), and
the pak's loader-time validation gate is `PakHeader` (`§4.1.7.1`).

#### 7.2.1 Byte layout (logical)

The pak is a single flat file, mmap-friendly, little-endian throughout
(`§4.1.7.1` "fixed-layout little-endian"). All offsets are byte
offsets from the start of the file.

```text
File layout (offsets are absolute, byte units, little-endian):

  0x0000  PakHeader  (fixed-size; see §7.2.2)
   ...    cluster-DAG bytes        (linearised group / meshlet tables)
   ...    BLASRecipe blob          (binary; LOD0 geometry descriptors)
   ...    PakPage[0]               +-+
   ...    PakPage[1]                 |-- compressed vertex / index /
   ...    ...                        |   attribute streams; Draco-encoded
   ...    PakPage[N-1]             +-+
   ...    PageTable                  (PakHeader.page_table_offset)
                                     (page_index -> byte_offset, page_byte_length)
  EOF
```

The header sits at offset 0 unconditionally; every other region's
offset is a field of the header. `PakReader` validates the header
first (`§4.1.7` invariant 4), refusing any payload access until the
magic, version, and `FormatHash` checks pass.

#### 7.2.2 `PakHeader` byte layout

`PakHeader` (`§4.1.7.1`) is a fixed-layout, no-padding-beyond-natural,
little-endian record. Every field's offset is determined at compile
time from the schema; field order is the canonical order below
(matching the data SPEC §7's tag-sorted layout rule applied to the
binary surface).

```text
Offset  Size   Field                          Notes
------  ----   -----                          -----
0x0000  4      magic                          ASCII "GLPK"
0x0004  2      pak_version_major              u16
0x0006  2      pak_version_minor              u16
0x0008  2      pak_version_patch              u16
0x000A  2      reserved_alignment             u16   = 0
0x000C  4      header_byte_length             u32   total header size in bytes
0x0010  8      format_hash                    u64   FormatHash (§7.2.3)
0x0018  8      source_content_hash            u64   blake3 of MeshSource (§4.1.1)
0x0020  2      glibre_engine_version_major    u16
0x0022  2      glibre_engine_version_minor    u16
0x0024  2      glibre_engine_version_patch    u16
0x0026  2      reserved_align2                u16   = 0
0x0028  8      cluster_dag_offset             u64
0x0030  8      cluster_dag_length             u64
0x0038  8      blas_recipe_offset             u64
0x0040  8      blas_recipe_length             u64
0x0048  8      page_table_offset              u64
0x0050  4      page_count                     u32
0x0054  4      lod_band_count                 u8 + 3 pad
0x0058  4      meshlet_group_count            u32
0x005C  1      draco_profile                  u8    DracoQuantisationProfile
0x005D  1      meshlet_max_vertices           u8    = 64 (§7.3.4)
0x005E  1      meshlet_max_triangles          u8    = 124 (§7.3.4)
0x005F  1      reserved_align3                u8    = 0
0x0060  N*4    decode_pool_scratch_max[N]     u32 per AttributeKind (§4.1.12)
        ...    residency_hint_table[page_count]   u8 per page (§4.1.6 / harmonius hot-pose)
        ...    reserved-tail padding to align next region to 16 bytes
```

`header_byte_length` carries the pak's own knowledge of how many bytes
it occupies; readers use it to seek past the header into the
cluster-DAG region without recomputing alignments. The two
variable-length tail arrays (`decode_pool_scratch_max[]` and
`residency_hint_table[]`) sit inside the header region (everything
before the cluster-DAG bytes) so the header is one contiguous read.

##### Reader-side validation order

`PakReader` validates fields in this exact order; the first failure
short-circuits with the listed error:

1. `magic == "GLPK"` else `Error::PakHeaderMagicMismatch`.
2. `format_hash == compiled_FormatHash` else
   `Error::PakFormatHashMismatch` (`§4.1.7` invariant 1).
3. `header_byte_length <= mmap_size` else
   `Error::PakHeaderOffsetOutOfRange`.
4. `cluster_dag_offset + cluster_dag_length <= mmap_size` else
   `Error::PakHeaderOffsetOutOfRange` (`§4.1.7.1` invariant 2).
5. `blas_recipe_offset + blas_recipe_length <= mmap_size` else
   `Error::PakHeaderOffsetOutOfRange`.
6. `page_table_offset + page_count * sizeof(PageTableEntry) <=
   mmap_size` else `Error::PakHeaderOffsetOutOfRange`.
7. `meshlet_max_vertices == 64` and `meshlet_max_triangles == 124`
   else `Error::PakFormatHashMismatch` (caps are encoded into
   `FormatHash` per §7.3.4; this check is a defence-in-depth arm).
8. `draco_profile != Reserved` and resolves inside the live closed
   set else `Error::DracoProfileUnknown`.

Only after all eight checks pass does any payload access proceed
(`§4.1.7` invariant 4).

##### Twin record `glibre.geometry.PakHeaderRecord` (Fory side-record)

A Fory-encoded sibling of `PakHeader` exists at
`data/schemas/geometry/PakHeaderRecord.fory` (FQN
`glibre.geometry.PakHeaderRecord`) **strictly for tooling** — the
editor's pak inspector, the cooker's verify pass, and the
content-browser preview. The runtime never reads it; it is a
human/tool-readable snapshot of the same field set.

```fory
schema glibre.geometry.PakHeaderRecord {
  version  1
  since    "0.1.0"

  field pak_path                    : string  tag 1  since 1
  field magic                       : u32     tag 2  since 1   // "GLPK" packed
  field pak_version_major           : u16     tag 3  since 1
  field pak_version_minor           : u16     tag 4  since 1
  field pak_version_patch           : u16     tag 5  since 1
  field format_hash                 : u64     tag 6  since 1
  field source_content_hash         : u64     tag 7  since 1
  field glibre_engine_version       : string  tag 8  since 1   // "M.m.p"
  field cluster_dag_offset          : u64     tag 10 since 1
  field cluster_dag_length          : u64     tag 11 since 1
  field blas_recipe_offset          : u64     tag 12 since 1
  field blas_recipe_length          : u64     tag 13 since 1
  field page_table_offset           : u64     tag 14 since 1
  field page_count                  : u32     tag 15 since 1
  field lod_band_count              : u8      tag 16 since 1
  field meshlet_group_count         : u32     tag 17 since 1
  field draco_profile               : u8      tag 18 since 1
  field meshlet_max_vertices        : u8      tag 19 since 1
  field meshlet_max_triangles       : u8      tag 20 since 1
  field decode_pool_scratch_max     : list<u32>  tag 21 since 1
  field residency_hint_table        : bytes      tag 22 since 1
}
```

This Fory record is optional; absence does not gate runtime load. Its
presence lets `glibre-foryc-pak-inspect` (a tools-context utility,
not a `data` concern) emit a JSON dump of any pak by reading only
the side-record. It is the side-channel render's §7 calls a "Fory
side-record" — same shape, same bootstrap rule.

#### 7.2.3 `FormatHash` — content-of-schema hash

`FormatHash` (`§4.1.7` invariant 1) is computed at `glibre-foryc` time
from a deterministic byte serialisation of the pak schema's defining
inputs:

```text
FormatHash := blake3_64(canonical_bytes(
    "GLPK"                            ascii bytes, 4
    pak_version_major                 u16 LE
    pak_version_minor                 u16 LE
    pak_version_patch                 u16 LE
    pak_header_field_layout_canonical UTF-8 (the exact field-table
                                       above, sorted by offset, comma-
                                       separated `name:type@offset`)
    cluster_dag_layout_canonical      UTF-8 (group / meshlet table
                                       record schemas, sorted)
    blas_recipe_layout_canonical      UTF-8 (binary descriptor record
                                       schema, sorted)
    page_layout_canonical             UTF-8 (page-format-version, sub-
                                       record table)
    page_table_entry_layout_canonical UTF-8
    meshlet_max_vertices              u8 = 64
    meshlet_max_triangles             u8 = 124
    sort(DracoQuantisationProfile.values_canonical)
                                      UTF-8 enum table; closed-sum
    sort(AttributeKind.values_canonical)
                                      UTF-8 enum table; closed-sum
))[:8]                                truncated to first 8 bytes ->
                                      u64 little-endian
```

Properties:

1. **Schema-only, payload-blind.** `FormatHash` depends solely on the
   pak schema; two paks whose mesh data differ but whose schemas are
   identical have identical `FormatHash` (`§4.1.7` invariant 1).
2. **Frozen-cap input.** `meshlet_max_vertices = 64` and
   `meshlet_max_triangles = 124` are fixed inputs to the digest. Any
   change to either bumps `FormatHash`; see §7.3.4.
3. **Closed-sum enums fold in.** `DracoQuantisationProfile` and
   `AttributeKind` enumerator tables fold in. Adding an enumerator
   bumps `FormatHash` (a previously-unknown profile is a different
   format).
4. **64-bit truncation.** The full Blake3-256 is truncated to its
   first 8 bytes, little-endian, and embedded in `PakHeader` as a
   `u64`. The truncation is acceptable because the value is gated by
   exact equality, not by collision-resistance against an adversarial
   miner — schema authors are first-party to the build.
5. **`AbiHash` independence.** `FormatHash` is the **pak-schema**
   hash; `glibre_types_abi_hash` is the **middleman-dylib** hash
   (`reviews/decisions/fory-codegen.md`). The two coexist:
   `CookManifest` records both (§7.1.1); the pak header records only
   `FormatHash` because runtime pak-load only gates on pak-schema.
   `glibre_types_abi_hash` is checked by the plugin loader at dylib
   load time, before any pak is mapped.

#### 7.2.4 Why `MeshletPak` is not Fory

The decision is the same one render's `PSOCacheRecord` makes for its
`archive_blob` field (`specs/render/SPEC.md` §7.1.2): the *envelope*
rides Fory; the *opaque large binary* does not. For `MeshletPak`,
*everything* is the opaque binary — its bytes are mmap'd directly into
the runtime's address space and decoded by domain-specialised code
(Draco for streams, hand-rolled u32/u8 for tables). Forcing Fory's
tagged variable-length encoding on this surface would:

1. Break mmap residency (Fory's varints require a parse pass before
   any field is addressable; mmap'd pak pages are addressable by raw
   offset).
2. Defeat byte-equal-across-hosts determinism (`PHILOSOPHY` §7,
   `§4.1.7` invariant 2): Fory's tagged encoding admits multiple
   byte-equivalent forms via tag-skipping; the pak's contract is
   single-canonical bytes.
3. Admit silent additive drift: Fory's `since` clause synthesises
   defaults at deserialise time, which is exactly the wrong behaviour
   for a runtime that must refuse loads of pak schemas it does not
   understand. `FormatHash` mismatch is the only acceptable drift
   signal, and it is binary.

Fory's role for the pak is therefore confined to (a) the *manifest*
that describes the pak from outside (§7.1.1), (b) the *side-records*
that mirror the pak's read-mostly metadata (§7.1.2, §7.1.3, §7.2.2's
twin record), and (c) tooling that wants to read the pak without
linking against `geometry`.

### 7.3 Migration rules

Geometry's persistence has two distinct migration mechanisms because
it has two distinct media. Both obey the engine-wide rules in
`specs/data/SPEC.md` §7.4 where applicable; the pak-binary path adds
its own, stricter rule.

#### 7.3.1 Fory side-records — additive fields, owned migrations

`CookManifest`, `BLASRecipeRecord`, `MeshSourceMetadata`, and
`PakHeaderRecord` follow the standard glibre Fory migration pattern:

1. **Additive fields at new tags.** Adding a field at a new (unused)
   tag with a `since N+1` clause and a `default` is the preferred
   migration shape. No migration-provider body is required; codegen
   synthesises the default at deserialise time. This is the only
   migration shape geometry expects to need at MVP.
2. **Removal marks tag reserved.** A removed field becomes a
   `reserved tag N` line; the tag number is never reused
   (`specs/data/SPEC.md` §7.4 rule 7).
3. **Closed-set enum extensions force a schema bump.** Adding a new
   `DracoQuantisationProfile` enumerator, a new `vertex_format`
   value, or a new `index_format` value adds an entry to the closed
   sum; the schema bumps and a migration body lives under
   `src/geometry/migrations/<Type>_v<N>_to_v<N+1>.cpp`. The body is
   pure (`specs/data/SPEC.md` §7.4 rule 2): it remaps old
   enumerator values into the new closed set or refuses with
   `Error::SchemaMigrationFailure` for irreconcilable values.
4. **Stepwise only.** Migrations are `vN -> vN+1`; the dispatcher
   composes chains (`specs/data/SPEC.md` §7.4 rule 1).
5. **Round-trip goldens.** Per `specs/data/SPEC.md` §7.5, every
   shipped schema version `N` ships a `vN.fory.bin` golden under
   `tests/data/schemas/geometry/<Type>/v<N>.fory.bin`; the migration
   chain is exercised end-to-end against the registry rather than
   step-by-step.

Geometry commits to **no breaking schema changes** to these
side-records inside MVP. Post-MVP changes follow rules 1-5.

#### 7.3.2 `MeshletPak` binary — `FormatHash` invalidation, no migration

The pak format is **schema-versioned by `FormatHash`**, not by Fory's
runtime migration. Any change to the pak schema — new `PakHeader`
field, new region in the file layout, change to the `BLASRecipe`
binary descriptor schema, change to the page format, change to the
cluster-DAG byte layout, addition of an `AttributeKind` enumerator,
or any of the inputs to `FormatHash` listed in §7.2.3 — bumps
`FormatHash` and **the new engine refuses the old pak with
`Error::PakFormatHashMismatch`**.

There is no in-process migration path for paks. The cooker re-cooks
from `MeshSource`. This is the same rule render applies to its
`PSOCacheRecord` archive (`specs/render/SPEC.md` §7.2.2 "invalidate,
never migrate"): paks are *cache* of a deterministic cook, not
authored user-data.

Operational consequences:

1. **Cook-driver re-cook trigger.** `CookManifest.format_hash` is
   compared against the live `FormatHash` at cook-time; mismatch
   forces a re-cook (`§4.1.8` invariant 1). The cook driver's
   incremental layer treats mismatched manifests the same way it
   treats absent manifests.
2. **Build-system advertisement.** `glibre-foryc` emits the live
   `FormatHash` value into a constant header
   (`build/generated/glibre-types/include/glibre/types/geometry/_format_hash.hpp`)
   that the runtime, the cooker, and the editor all consume from a
   single source. Schema drift therefore cannot leak into a partial
   build.
3. **No partial-validity middle state.** A pak whose `FormatHash`
   mismatches is treated as absent, never as a candidate for
   field-level migration. The editor's content browser surfaces the
   mismatch as a "needs re-cook" badge; the runtime refuses load.
4. **Acceptance gate.** The acceptance criteria for any change that
   alters `FormatHash` MUST include a re-cook of every shipped MVP
   pak in `assets/cooked/` so the repository never holds a pak that
   the engine refuses on `main`.

#### 7.3.3 Cluster DAG layout — additive fields, no migration

The cluster-DAG bytes inside the pak (`§4.1.5`) are part of the pak
schema and ride §7.3.2. The DAG's evolution rule, however, is
specifically called out in this spike's brief:

1. **Layout changes are additive only at the field level.** Adding
   a per-`MeshletGroup` field (e.g. a new bounding-cone tightening
   metric, an additional SSE band) appends past the existing record
   tail; no field is reordered, no field is reinterpreted. Padding
   is allowed where ABI alignment demands it (`reviews/decisions/fory-codegen.md`
   §"ABI Stability Rules" rule 3, applied to the binary surface).
2. **Topology changes are not "additive".** Changing the watertight-cut
   bookkeeping shape (`§4.1.4` invariant 1), changing the LOD-band
   monotonicity contract (`§4.1.4` invariant 2), or changing the
   parent/child edge encoding bumps `FormatHash` and forces re-cook
   per §7.3.2. These are not additive — they alter the meaning of
   existing bytes.
3. **No in-place field-level migration.** Even when a change is
   field-additive at the DAG level, the pak's overall `FormatHash`
   bumps because the pak's inputs to §7.2.3 include
   `cluster_dag_layout_canonical`. The "additive" property here is a
   property of the *cooker's source code* (it does not need to
   re-derive the DAG from `MeshSource`; it can reuse the prior
   stage's output and only patch in the new field), not of the pak
   bytes (which still get rewritten at re-cook time).

The collapse: the pak is rewritten on every schema bump; the DAG
field set within the pak grows additively across schema versions to
keep the cooker's incremental machinery cheap.

#### 7.3.4 Frozen meshlet caps — `vertex_count <= 64`, `prim_count <= 124`

The hard caps enforced by `§4.1.3` invariant 1 are **frozen** for
the life of the engine:

1. **Encoded into `FormatHash`.** Both caps fold into the digest
   (§7.2.3 input list). Changing either cap bumps `FormatHash` and
   invalidates every shipped pak per §7.3.2.
2. **Encoded into `CookManifest` as fixed values.** The
   `meshlet_max_vertices` and `meshlet_max_triangles` fields of
   `CookManifest` (§7.1.1 tags 20-21) are persisted but locked: any
   manifest carrying values other than `64` and `124` is
   `Error::CookManifestInvalid` (§7.1.1 invariant 2). The fields
   exist for diagnostic completeness, not as tuneables.
3. **Encoded into `PakHeader` as range-checked `u8`.** `PakHeader`
   carries the caps as `u8` fields (§7.2.2 offsets `0x005D` /
   `0x005E`); the reader-side validation order checks them at step
   7. The `u8` width is itself part of the contract — a cap that
   would not fit in `u8` triggers a `FormatHash` bump regardless of
   whether the new value is "still small" (`§4.1.3` invariant 1
   "the count fields are `u8` with range checks").
4. **No graceful relaxation.** Loosening the caps post-MVP requires
   a `FormatHash` bump and a full re-cook of every pak; the
   migration path is "new format, not new field" per §7.3.2. The
   cooker's `MeshletBuildOptions` (§5) presents the values as
   `default = 64 / 124` and the cooker rejects user attempts to
   override them with `Error::MeshletOversize` at cook-time stage 3.

The collapse: the meshlet shape is the single physical-layout knob
the entire downstream pipeline is parameterised by — Metal 4
mesh-shader payload limits, `PakPage` packing density, Draco profile
tuning, BLAS descriptor sizing all key off it. Freezing it removes
an entire combinatorial axis from the format-evolution surface.

### 7.4 What is NOT persisted

To make the boundary explicit (echoing render's §7.3 table):

| Artefact                       | Why not persisted                                                                                                              |
|--------------------------------|--------------------------------------------------------------------------------------------------------------------------------|
| Cooked vertex / index bytes    | Live in `MeshletPak` binary blocks; consumed by `PakReader` directly. Not Fory-serialised by design (§7.2.4).                  |
| `DracoStream` payloads         | Pak binary blocks; decoded by `DecodePool` into transient buffers (`§4.1.12`).                                                 |
| `PakPage` bytes                | Pak binary blocks; mmap'd by `PakReader` and streamed by the residency manager (`§4.1.13`).                                    |
| `ClusterDAG` runtime form      | Reconstructed from pak bytes at register-mesh; never rebuilt at runtime (`§4.1.5`).                                            |
| `MeshSource`                   | Cook-time only (`§4.1.1`); content-context owns the source-asset import; geometry consumes a transient `MeshSource` view.      |
| `OptimisedMesh`                | Cook-time intermediate (`§4.1.2`); never enters a pak directly; not persisted.                                                 |
| `Meshlet` / `MeshletGroup`     | Persisted *inside* the pak binary, not as Fory records. Their layout is part of `FormatHash`.                                  |
| `BLASRecipe` runtime blob      | Pak binary block; the `BLASRecipeRecord` Fory side-record (§7.1.2) is the tooling projection, not the runtime input.           |
| `MeshHandle` / `MeshletGroupHandle` | Per-process opaque identifiers (`§4.1.9` / `§4.1.10`); regenerated on each register-mesh; never persisted.                |
| `DecodePool` / `ResidencyState`| Per-process runtime state (`§4.1.12` / `§4.1.13`); rebuilt from `PakHeader` on each pak map.                                   |
| `GeometryRegistry` table       | Per-process runtime aggregate (`§4.1.14`); rebuilt from registered paks at startup.                                            |
| `GpuMeshBuffers`               | Device-side mappings (`§4.1.15`); allocated by render's `Device` peer; never persisted.                                        |

These appear in the persistence surface only as **identifiers** and
**byte-offset references** (`source_content_hash`, `format_hash`,
`pak_path`, page indices, group indices) carried inside the four
Fory side-records and the pak header, never as standalone byte
payloads.

### 7.5 Cross-context obligations

Per `specs/data/SPEC.md` §7.7, this section inherits the engine-wide
schema rules. Specifically:

1. The `.fory` grammar in `specs/data/SPEC.md` §7.1 is the only
   legal syntax for files at `data/schemas/geometry/<Type>.fory`.
2. Migration providers (when needed; none at v1) obey
   `specs/data/SPEC.md` §7.4.
3. Round-trip goldens follow the harness in `specs/data/SPEC.md`
   §7.5; per-version goldens are mandatory whenever a schema's
   version exceeds 1.
4. The `MeshletPak` binary format is **out-of-band** with respect to
   the `.fory` registry; it is not enumerated in `AbiHashManifest`
   (`specs/data/SPEC.md` §7.2.3). Its evolution is gated by
   `FormatHash` per §7.3.2, not by the spine's per-schema source
   hash. A `FormatHash` bump does not automatically force an
   `AbiHashManifest` rebuild; an `AbiHashManifest` rebuild does not
   automatically force a `FormatHash` bump. The two surfaces are
   independent — the same independence render's §7.1.2 calls out
   between `glibre_types_abi_hash` and the PSO archive's
   `archive_blob_blake3`.

The collapse: domain Fory schemas migrate via the spine; the pak
migrates via `FormatHash`. Two surfaces, two gates, one cooker-driven
cache rebuild on either bump.

## 8. Hot-Reload Contract

This section specialises the engine-wide hot-reload protocol
(`reviews/decisions/hot-reload-protocol.md` — drain → swap → migrate →
resume) to the **geometry plugin and its cooked artefacts**. Geometry is
unusual among glibre's contexts because it owns *two* reload surfaces,
not one:

1. The geometry plugin `.dylib` itself, which rides the engine-wide
   protocol unchanged (drain → swap → migrate → resume at phase 8).
2. The cooked `.glibre-pak` files on disk that the running plugin maps
   into the process. These are the artefact this section targets.

The first surface adds nothing new beyond the protocol's existing
machinery — geometry's `glibre_plugin_drain` releases the `DecodePool`,
unmaps every `PakReader`, and clears the `GeometryRegistry` tables;
`glibre_plugin_register` rebuilds them from the surviving `MeshHandle`
slots' recorded `(pak_path, format_hash, content_hash)` triples in the
middleman registry table. That is the standard pattern the protocol
documents and is not re-stated here.

The **second surface — pak-file change** — is what this section
defines. A pak swap shares the protocol's four-step shape (drain → swap
→ migrate → resume) and is dispatched from inside the same phase 8
slot, but its trigger, refusal gate, and migrate body are geometry-
specific: the trigger is a content-hash change on a `.glibre-pak`
file (not a dylib), the refusal gate is `FormatHash` mismatch (not
`glibre_types_abi_hash` mismatch), the survival inventory is keyed on
`MeshHandle` identity preservation, and the migrate body re-resolves
every `MeshHandle` and `MeshletGroupHandle` to a new payload address
without changing the handle's bit-level value. Engine-wide concerns
(per-plugin atomicity, observer bus event shapes, error wrapping rules,
the `enqueue_hot_reload` E2E entry point) are not re-stated — see the
protocol record. Geometry adds nothing to that machinery; it only fills
in the four pluggable points the protocol leaves to each plugin
(drain side-effects, survival inventory, migrate body, register-time
rehydration) plus a small surface for the pak-file path so observers
(notably `render`) can react.

### 8.1 Trigger — `.glibre-pak` content-hash change

A pak hot-reload is requested when, and only when, `content`'s
filesystem-watcher (or the E2E test hook in §8.6) detects that a
`.glibre-pak` file referenced by some live `MeshHandle` has changed
**and** the new file's `PakHeader.content_hash` differs from the
mapping currently held by the registry. The watcher posts the request
to the loader's pending-reload queue; the loader consumes it at phase 8
entry per the protocol, treating geometry as the originating context.

Three observations make this trigger orthogonal to the dylib trigger:

1. **The trigger payload is a path, not a dylib.** Geometry's reload
   request carries `(MeshHandle target, std::filesystem::path
   replacement_pak_path)` — never a `.dylib` path. The loader does not
   `dlopen` anything in this branch; it routes the request to
   geometry's `GeometryRegistry::reload_pak(...)` after phase 8 entry.
   Plugin vtable swaps and pak-file swaps are independent transactions
   in the same phase per protocol §"Failure & Rollback" (per-plugin
   atomicity, here per-pak atomicity).
2. **The trigger does not require any plugin code change.** A pak that
   re-cooks because its `MeshSource` bytes changed — but whose
   `FormatHash` still matches the engine's compiled value — reloads
   with no dylib involvement. The plugin code remains stable; only
   bytes inside the mapping change.
3. **The trigger surface itself is a middleman type.**
   `glibre::types::geometry::PakReloadRequest` carries the path, the
   target `MeshHandle`, and a request id. Its layout survives a
   geometry-plugin reload (which would be a dylib reload, the first
   surface) so a queued pak reload outlives a concurrent plugin swap
   in the same phase 8.

**Mid-frame reload is refused.** A pak reload request that arrives at
any phase 1–7 of frame N is queued; the loader's `pending_reloads`
counter is consumed only at phase 8 entry per protocol §"Step 1 —
Drain". Inside phase 8, geometry does not yield to LOD-band selection
(phase 6) or `RenderProxy` consumption (phase 7) — the loader holds
exclusive ownership for the duration of drain → swap → migrate → resume
per protocol §"Decision". A request that would force any of phases
1–7 to observe a partially-swapped `MeshHandle` resolution is treated
as a contract violation by the loader, not a refusal — geometry
contributes no new refusal arm here, but states the invariant
explicitly so consumers cannot expect mid-frame swap semantics.

### 8.2 Refusal — `FormatHash` mismatch is the only gate

The pak hot-reload protocol has **exactly one geometry-specific
refusal arm**: the new pak file's `PakHeader.FormatHash` does not
match the engine's compiled-in `FormatHash`
(`build/generated/glibre-types/include/glibre/types/geometry/_format_hash.hpp`,
§7.3.2 consequence 2). On mismatch, geometry refuses the swap and the
old pak remains mapped; no migration is attempted, no `BLASRecipe` is
re-applied, no observer event is fired except the engine-wide
`HotReloadRefused`. The refusal arm is the existing
`geometry::Error::PakFormatHashMismatch` (§4.1.7 invariant 1, §10),
wrapped under `core::Error::HotReloadRefused` per protocol §"Refusal
Cases".

This is the single seam that makes pak hot-reload's contract
mechanically simple:

1. **No in-process migration of pak bytes.** §7.3.2 already records
   "invalidate, never migrate" for the pak binary. Hot-reload inherits
   that rule verbatim — there is no migration ladder, no per-version
   chain, no `migrate_pak_v<N>_to_v<N+1>` table. A `FormatHash` bump is
   semantically identical to a fresh game on this surface.
2. **Engine restart needed.** When `FormatHash` mismatches, the
   operator's only recovery path is to (a) re-cook the pak against the
   current engine (`§7.3.2` consequence 4 mandates this for any change
   on `main`), (b) restart the engine process if step (a) requires a
   plugin rebuild that the loader cannot apply mid-session. The
   refusal log surfaces both the live `FormatHash` and the rejected
   pak's `FormatHash` so the operator can act without grepping
   generated headers.
3. **Refusal is per-pak, not phase-wide.** The protocol's per-plugin
   atomicity (§"Failure & Rollback") applies per-pak here: one pak's
   `FormatHash` mismatch does not block another pak's reload in the
   same phase 8.
4. **No graceful "best-effort" path.** The engine never accepts a pak
   whose `FormatHash` differs from its own. Partial validity is
   forbidden per §7.3.2 consequence 3; this section restates the rule
   in the hot-reload context for emphasis.

The other two protocol refusal arms surface verbatim and are listed
here for completeness only — they are not geometry-specific:

| Engine arm                            | When it fires for a pak swap                                                                                        | Operator action                                                                                          |
|---------------------------------------|---------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------|
| `core::Error::PluginAbiHashMismatch`  | A *plugin*-level concept; cannot fire on a pak swap because the pak path does not invoke `dlopen`. Listed for completeness. | N/A in this branch.                                                                                      |
| `core::Error::SchemaMigrationFailed`  | A `glibre-types` middleman type referenced by the pak's accompanying `CookManifest` side-record (§7.1.1) lacks a migrate chain. | Author the missing migrate function (`§7.3.1` rule 3) or restore the prior schema; refusal until either. |
| `core::Error::PluginInitFailed`       | The replacement plugin's `register` body errored — orthogonal to the pak path; never a pak-file refusal cause.       | Read the geometry plugin's log; this is the dylib branch, not this section.                              |

### 8.3 Survival inventory

The engine-wide survival rule is mechanical: **state with a `.fory`
schema in `glibre-types.dylib` survives across the swap; state without
one does not** (protocol §"State Survival Rules"; PHILOSOPHY collapse:
one check, not a per-aggregate manifest). Pak hot-reload narrows that
rule further with a geometry-specific corollary: **opaque-handle
identity (`MeshHandle`, `MeshletGroupHandle`) survives even though the
backing pak bytes are replaced**. The handle's bit value does not
change; the registry simply re-resolves it to a new payload address.
The table below classifies every geometry aggregate against the rule.

| Geometry-owned state                                     | Persistence path                                | Survives swap? | Reasoning                                                                                                                                                                                                                                                                                                                                                                                            |
|----------------------------------------------------------|-------------------------------------------------|----------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `MeshHandle` (§4.1.9)                                    | None (in-process opaque id; recorded in middleman registry table)         | **Yes — bit-equal.** The `(u32 index, u32 generation)` pair stays the same; the registry slot is re-pointed at the new `PakReader`. Generation is **not** bumped because the handle's identity contract requires stability across pak content changes (§4.1.9 invariant 1). A new generation is reserved for explicit `unregister_mesh` followed by `register_mesh`, which is a different operation. |
| `MeshletGroupHandle` (§4.1.10)                           | None (derived from `(MeshHandle, group_index)`) | **Yes — bit-equal.** Per §4.1.10 invariant 1, the handle is bijective with `(MeshHandle, group_index)` for the lifetime of the parent's generation. Hot-reload preserves the parent's generation (above), so every group handle issued before the swap continues to resolve. The group **may have moved** in the new pak's cluster DAG; re-resolution is the migrate body's job (§8.4).               |
| `GpuMeshBuffers` (§4.1.15) — handle table                | None (per-band, per-`MeshHandle`)               | **No — replaced.** The buffer handles point at GPU memory whose contents are derived from the *old* pak's decoded bytes. Surviving them across a pak swap would expose stale geometry to render's bindless lookup. The migrate body releases every `GpuMeshBuffers` record for the affected mesh and re-allocates from `render`'s buffer allocator after the new pak's decode pool entries warm.    |
| `ResidencyState` (§4.1.13) — per-`(MeshHandle, page)`    | None (per-process)                              | **Re-evaluated.** The new pak's `PakHeader` may declare a different page count, different residency hints, or different page-byte-offsets. The migrate body drops the old pak's entries and seeds the new pak's entries at `NotResident`; `content`'s scheduler is then free to re-prioritise based on the new `ResidencyHint` table. The state's *table identity* survives; its rows are rebuilt. |
| `BLASRecipe` (§4.1.7.2) — per-`MeshHandle` blob          | Inside the new pak file                         | **Re-applied.** The recipe's bytes live in the new pak's `BLASRecipe` region (§7.2.2 layout). The migrate body publishes the new recipe to `render`'s `RTAccelStructures` peer (§4.1.8 of `specs/render/SPEC.md`) via the `MeshReplaced` event (§8.5); render then schedules a BLAS rebuild against the new geometry on its next phase-7 mutation point. Geometry never builds the BLAS itself.    |
| `CookManifest` (§7.1.1) — Fory side-record               | `.fory` schema, middleman                       | **Reloaded.** The accompanying `<pak>.cookmanifest.fory` file is re-read whenever its sibling pak reloads; the new manifest's `format_hash`, `cook_options`, and `source_content_hash` overwrite the registry's recorded values for that `MeshHandle`. If the manifest's `format_hash` field disagrees with the new pak's `PakHeader.FormatHash`, the swap is refused with `geometry::Error::CookManifestInvalid` wrapped under `core::Error::HotReloadRefused`. |
| `BLASRecipeRecord` (§7.1.2) — Fory side-record           | `.fory` schema, middleman                       | **Reloaded** alongside the pak. Tooling-only; runtime consumes the binary `BLASRecipe` blob directly. Re-read on swap purely so the editor's content browser displays the new descriptor count without restarting.                                                                                                                                                                                |
| `MeshSourceMetadata` (§7.1.3) — Fory side-record         | `.fory` schema, middleman                       | **Reloaded.** Authoring-provenance only; no runtime consumer.                                                                                                                                                                                                                                                                                                                                       |
| `PakReader` (§4.1.11)                                    | None — wraps the file mapping                   | **No — replaced.** The old `PakReader` is destroyed (its `mmap` is unmapped after the swap completes; see §8.4). A fresh `PakReader` is constructed against the new pak file. Construction performs the `FormatHash` check (§4.1.11 invariant 3) — this is where the §8.2 refusal fires.                                                                                                          |
| `DecodePool` (§4.1.12)                                   | None — pool of scratch buffers                  | **Yes, but entries invalidated.** The pool itself persists (sized at engine init from the union of every loaded pak's `PakHeader` per-attribute maxima; §4.1.12 invariant 1). Decode-cache entries currently holding *intermediate* state for the affected `MeshHandle`'s pages are invalidated by the migrate body — every in-flight decode for the old pak is cancelled before the swap. Slots return to the pool after cancellation. The **pool's capacity is unchanged**; if the new pak's per-attribute maxima exceed the existing pool budget, the swap refuses with `geometry::Error::DecodePoolUndersized` (§4.1.12 invariant 1, restated in this hot-reload context). |
| `GeometryRegistry` (§4.1.14) — handle table              | None — engine singleton                         | **Yes — table identity survives.** The `MeshHandle` table stays put; the migrate body re-points the affected slot's backing `PakReader` and clears the slot's `GpuMeshBuffers` record set. No other slots are touched.                                                                                                                                                                              |
| `ClusterDAG` runtime form (§4.1.5) — derived from pak    | None — reconstructed at register-mesh           | **No — re-derived.** The DAG is reconstructed from the new pak's bytes at the migrate step. Group-index identity is **not** preserved across re-cooks; the migrate body's re-resolution step (§8.4 step 3) walks the new DAG and re-binds each surviving `MeshletGroupHandle` to the closest matching group in the new DAG (matching key: stable `group_id` recorded in pak per §7.1.2).            |
| Per-plugin worker thread pools (decode workers)          | None                                            | No — destroyed by geometry's `glibre_plugin_drain` (the dylib branch of hot-reload, not this branch). Pak swaps do not spin down decode workers; they merely cancel in-flight decodes for the affected mesh.                                                                                                                                                                                        |

The rule mechanically applied: every "Yes — bit-equal" row sits behind
an opaque-handle boundary that the engine's middleman registry already
preserves; every "No — replaced" row is bounded by a single
`MeshHandle` so the swap is tractable in O(pages of the affected mesh)
work; every "Reloaded" row is a `.fory` side-record that rides the
engine's standard Fory reload path.

### 8.4 `migrate(...)` body — geometry's responsibilities

The protocol's `migrate` step (protocol §"Step 3 — Migrate") for a
pak swap runs entirely inside `GeometryRegistry::reload_pak(...)`,
called once per pending pak request from the loader's phase-8 driver.
The body is **pure with respect to plugin code** — it touches the
registry tables, the `DecodePool`, the `ResidencyState` table, and the
observer bus, all of which live on the host side or in middleman
types. It does not call into render's vtables (it only publishes the
`MeshReplaced` event; render's response runs in render's own resume
step or in render's next phase 7), nor into content's scheduler
(scheduler reads `ResidencyState` lock-free at phase 6 of the next
frame and re-prioritises naturally).

The body has six numbered steps, executed in order; failure at step
`K` rolls back through `K..1` and yields
`core::Error::HotReloadRefused` per protocol §"Failure & Rollback".

#### 8.4.1 Pre-swap header validation

1. Construct a candidate `PakReader` against
   `request.replacement_pak_path` — this performs the `FormatHash`
   check (§4.1.11 invariant 3, §8.2) and the magic / offset checks
   (§4.1.7.1). On any failure, refuse the swap with the underlying
   `geometry::Error::*` wrapped under `core::Error::HotReloadRefused`;
   leave the old `PakReader` mapped; emit `HotReloadRefused` with the
   inner cause.
2. Re-read the sibling `<pak>.cookmanifest.fory` file via the engine's
   Fory loader. Cross-check its `format_hash` field against the
   candidate `PakReader`'s `PakHeader.FormatHash`; mismatch refuses
   with `geometry::Error::CookManifestInvalid` (§7.1.1 invariant 2).
3. Walk the candidate pak's `PakHeader.DecodePool` sizing requirements
   and verify they fit inside the live pool's capacity (§4.1.12
   invariant 1). Failure refuses with
   `geometry::Error::DecodePoolUndersized`.

This step never mutates registry state. Postcondition: the candidate
is fully validated and the rest of the migrate body is committed to
running to completion.

#### 8.4.2 Cancel in-flight decodes for the affected mesh

1. For each `(MeshHandle, page_index)` pair in `ResidencyState` whose
   state is `Pending`, mark it `NotResident` and signal cancellation
   to the decode worker holding the slot. Workers check the
   cancellation flag at the next decode-block boundary and return the
   slot to the pool with `geometry::Error::DecodePoolBusy` (per
   §4.1.12 invariant 2 — bounded-wait, no spin).
2. Wait for cancellation to complete: this is a bounded wait because
   §4.1.12 forbids unbounded blocking; in practice each in-flight
   decode finishes its current sub-block within microseconds.
3. Postcondition: no decode worker is still reading the old
   `PakReader`'s mapped bytes for this mesh.

#### 8.4.3 Re-resolve handles to the new payload

This is the heart of the migrate body and the unique geometry
contribution. The handle's bit-level value does not change; only its
**resolution to a payload address** changes.

1. **`MeshHandle` re-resolution.** The registry slot for the affected
   `MeshHandle` is re-pointed at the new `PakReader`. The slot's
   generation is **not** bumped (§4.1.9 invariant 1: handle stability
   across pak content changes). Render's `RenderProxy` SoA may
   continue holding the same `MeshHandle` value; subsequent lookups
   resolve to the new pak.
2. **`MeshletGroupHandle` re-resolution.** Every `MeshletGroupHandle`
   issued before the swap is bijective with `(MeshHandle,
   group_index)` for the lifetime of the parent's generation
   (§4.1.10 invariant 1). Group indices may have shifted in the new
   pak's cluster DAG (because re-cook can re-order DAG nodes
   deterministically per `cluster_dag_layout_canonical`, §7.2.3). The
   re-resolution table maps each pre-swap `(group_index_old)` to its
   `(group_index_new)` by matching the stable per-group `group_id`
   recorded in `BLASRecipeRecord` (§7.1.2) and the cluster-DAG
   record (§7.1.1). Groups that have no match in the new DAG (e.g. a
   simplification stage merged the group into a coarser node) yield
   `geometry::Error::MeshletGroupHandleStale` on subsequent lookup —
   the same contract as a generation mismatch in §4.1.9 invariant 3,
   surfaced through the existing stale-handle path.
3. The mapping table is published atomically; readers (render's LOD
   selector at the next phase 6) see either the old table or the new
   table, never a partial view. Publication is one relaxed atomic
   store under the loader's exclusive phase-8 ownership.

#### 8.4.4 Replace `GpuMeshBuffers`

1. For every `(MeshHandle, LODBand)` entry in the registry's
   `GpuMeshBuffers` table for the affected mesh, return the buffer
   handles to render's allocator. The release call is the inverse of
   the per-band acquisition (§4.1.15 invariant 4 — lifetime tied to
   residency).
2. Clear the table for the affected `MeshHandle`. New
   `GpuMeshBuffers` records will be materialised lazily when the
   first page in each band reaches `Resident` again — this is the
   normal residency promotion path (§4.1.13), driven by `content`'s
   scheduler in subsequent frames.
3. Postcondition: render's bindless lookup against this `MeshHandle`
   in the next phase 7 returns the §4.1.10 invariant 3 "page not
   loaded yet" render-skip signal until the scheduler reseats pages.
   The skip is brief (≤1–2 frames at MVP scheduling rates) and
   never produces stale geometry.

#### 8.4.5 Re-evaluate `ResidencyState`

1. Drop every `ResidencyState` entry keyed `(this MeshHandle, *)`.
2. Insert fresh entries for each `page_index` in the new pak's page
   table, all initialised to `NotResident`.
3. The new pak's `ResidencyHint` byte array (§4.1.7.1, §4.1.13
   invariant 4 — hints never mutate at runtime) is read once during
   this step and stored alongside the new entries; `content`'s
   scheduler sees the updated hints on its next phase-6 read and
   re-prioritises naturally. Geometry never adjudicates the budget
   itself (§4.1.13 invariant 4).

#### 8.4.6 Re-apply `BLASRecipe` and unmap the old pak

1. Read the new pak's `BLASRecipe` blob (§4.1.7.2). Geometry does
   *not* invoke any GPU API (§4.2 invariant 10); it only stores the
   recipe inside the registry slot for `render` to consume.
2. Publish the `MeshReplaced` event to the engine observer bus
   (§8.5). The event carries the affected `MeshHandle`, the new
   pak's `FormatHash` and `content_hash`, the count of resolved
   group handles, and the count of stale group handles (§8.4.3 step
   2). Render's `RTAccelStructures` peer (§4.1.8 of the render
   SPEC) reads the recipe on its next phase-7 mutation point and
   schedules a BLAS rebuild; the prior BLAS for this mesh remains
   live until the rebuild completes (render's contract, not
   geometry's).
3. After the bus call returns synchronously (subscribers run on the
   loader thread per protocol §"Observer Notification"), unmap the
   old `PakReader`. Its file mapping is released, its decode pool
   slots have already been returned (§8.4.2), and no other reference
   exists.

The total work in geometry's pak-reload migrate body is therefore
bounded by **O(pages of affected mesh) for residency-table rebuild +
O(groups in DAG) for handle re-resolution + O(bands of affected mesh)
for `GpuMeshBuffers` release + one bus call**, fitting the protocol's
"reload path bounded by drain + swap + Σ migrate + register" budget
(`hot-reload-protocol.md` §Consequences).

### 8.5 Observers — `MeshReplaced` event

A pak swap's success is published as a single geometry-specific event
piggy-backing on the engine observer bus:

```text
MeshReplaced {
    mesh_handle:     glibre.types.geometry.MeshHandle,
    old_format_hash: u64,
    new_format_hash: u64,         // == old in non-§8.2 cases
    old_content_hash: u64,
    new_content_hash: u64,
    groups_resolved: u32,
    groups_stale:    u32,
}
```

Atomicity follows protocol §"Observer Notification": subscribers are
called synchronously by the loader on the game-loop thread, after
§8.4.6 step 1 (recipe stored) and before §8.4.6 step 3 (old mapping
unmapped). Subscribers see a fully-swapped registry and a fully-
re-resolved handle table; they never observe a half-swapped pak.

Two observer responsibilities are contractual:

1. **`render` invalidates draw lists for affected entities.** Render
   subscribes to `MeshReplaced` and walks every `RenderProxy` whose
   `MeshHandle` field matches `mesh_handle` (the SoA layout makes
   this a single linear scan keyed by the handle's `index` field).
   For each match, render marks the proxy's per-frame draw entry
   stale; the next phase 6 cull-extract rebuilds it against the new
   `MeshletGroupHandle` resolution and the next phase 7 picks up the
   replaced `GpuMeshBuffers` (rebuilt lazily by the residency
   promotion path, §8.4.4 step 2). Render's contract is documented
   in its own §8.5 (`RenderFrameDropPending` / `RenderHotReloadCompleted`
   are the dylib-reload events; `MeshReplaced` is the asset-reload
   event geometry adds). No render state survives the swap that
   would yield stale geometry — the lazy `GpuMeshBuffers`
   rebuild is the seam that closes the loop.
2. **`content` re-prioritises residency.** Content's filesystem-watcher
   *originated* the request, so it does not re-listen here; but it
   does observe the event to clear any pending pre-fetch records keyed
   on the old pak's page indices. The scheduler then schedules new
   pre-fetches against the new pak's page table on its next phase-6
   read.

The `MeshReplaced` event type is itself a middleman type
(`glibre::types::geometry::MeshReplaced`), so its layout survives any
geometry-plugin reload (the dylib branch). No third event arm is
permitted on this surface; new observability needs flow into the
existing arms or graduate to a SPEC bump.

A refusal — any of the §8.2 cases or any §8.4 step 1 failure — fires
the engine's standard `HotReloadRefused { plugin_fqn:
"glibre.geometry", cause: core::Error::* }` instead of `MeshReplaced`.
The previous mapping stays live; observers see no event for the failed
swap.

### 8.6 Test hooks — deterministic pak-swap fixture

Geometry's pak-reload contract is verified end-to-end by a single test
fixture under `tests/geometry/hot_reload/` that drives the loader's
existing `enqueue_hot_reload` E2E entry point (protocol §"Test
Hooks") via a pak-path overload. The fixture has three layers, mirroring
render's §8.6 structure to keep CI shape uniform across contexts.

1. **Trace capture.** A fixture pak `tests/data/paks/cornell-mesh-v1.pak`
   ships byte-identical from cook on every supported host (the
   determinism contract of §4.1.7 invariant 2 makes this a goldens
   commit, not a per-CI cook). The harness loads the pak, registers
   one `MeshHandle`, runs the engine for `K = 8` frames against a
   deterministic scene with `content`'s scheduler driven by a fixed
   PRNG seed, and captures a structured trace per frame:
   `(frame_counter, mesh_handle, resolved_meshlet_group_handles,
   resident_pages, gpu_mesh_buffers_handles, blas_recipe_descriptor_count)`.
   Stored canonical-ordered (per
   `reviews/decisions/determinism-canonical-iteration.md`) at
   `tests/data/traces/geometry/pak-swap-vN.bin`.
2. **Reload trigger.** At frame `K/2`, the harness calls a
   `enqueue_pak_reload(mesh_handle, "tests/data/paks/cornell-mesh-v2.pak")`
   E2E entry point (a pak-flavoured overload of
   `enqueue_hot_reload`). The `v2` pak is byte-different from `v1`
   (a single vertex moved by a known offset; the cook is
   deterministic) but its `FormatHash` is identical. This tests the
   **happy-path content-only swap**: a swap that should preserve
   handle identity and re-resolve to new payload addresses.
3. **Post-reload assertion.** The harness records a second trace for
   frames `K/2 .. K-1` against the new pak and asserts:
   - frame `K/2`'s trace entry is byte-equal to the reference at the
     same index (phase 8 publishes nothing observable in frame N per
     `hot-reload-protocol.md` §Consequences);
   - frame `K/2 + 1`'s `mesh_handle` is bit-equal to the original
     handle (handle-identity preservation, §8.3 / §8.4.3);
   - frame `K/2 + 1`'s resolved group handles are bit-equal to the
     pre-swap handles (the pak's group-id stability check, §8.4.3
     step 2);
   - the `MeshReplaced` event was fired exactly once with
     `groups_resolved == reference_group_count` and `groups_stale ==
     0`;
   - frame `K/2 + 1`'s `gpu_mesh_buffers_handles` is *different* from
     the reference (the buffers were replaced, §8.4.4) — the
     assertion is `not_equal`, with the new handles drawn from
     `render`'s allocator at the next residency promotion;
   - frame `K/2 + 2`'s rendered output (a captured framebuffer
     screenshot) is byte-equal to the reference's frame `K/2 + 2`
     captured against `v2` from a cold start; this is the seam that
     proves the swap path produces the same observable output as a
     re-run with the new pak from frame zero.

Two more fixture pairs cover the refusal cases, mirroring render's
matrix:

- `cornell-mesh-v1` → `cornell-mesh-bad-format-hash`: the v2 pak is
  cooked against a stub `FormatHash` that does not match the engine.
  Asserts `HotReloadRefused { cause: PakFormatHashMismatch }` with the
  prior pak still serving (frames `K/2 + 1 .. K-1` byte-equal to the
  original trace; `mesh_handle` continues to resolve to v1 bytes).
- `cornell-mesh-v1` → `cornell-mesh-undersized-pool`: the v2 pak's
  `PakHeader.DecodePool` sizing exceeds the live pool's capacity.
  Asserts `HotReloadRefused { cause: DecodePoolUndersized }` and the
  same continuation behaviour.

A fourth fixture exercises the group-stale path:
`cornell-mesh-v1` → `cornell-mesh-simplified-v2` (same `FormatHash`,
different DAG topology — one finer group merged into a coarser group
between cooks). Asserts `MeshReplaced` fires with `groups_stale > 0`
and the matching `MeshletGroupHandle` lookups return
`geometry::Error::MeshletGroupHandleStale` (§8.4.3 step 2). Render's
proxy update path (§8.5 observer responsibility 1) drops the affected
draw entries on the next phase 6 cull-extract; the test asserts the
draw-call count drops by exactly `groups_stale` between frames `K/2`
and `K/2 + 1`.

All four scenarios run inside a single CI job using the in-process
trigger; no filesystem watcher races are involved (the watcher path is
a thin wrapper that calls `enqueue_pak_reload`, exactly mirroring the
protocol's filesystem-watcher policy in `hot-reload-protocol.md` §Test
Hooks). The Catch2 cases are listed in §11 acceptance criteria as
`Hot-reload preserves mesh handle identity`, `Hot-reload refuses
FormatHash mismatch`, `Hot-reload refuses undersized decode pool`,
`Hot-reload surfaces stale group handles`.

### 8.7 Cross-references

- Engine protocol: `reviews/decisions/hot-reload-protocol.md`
  (drain → swap → migrate → resume; refusal arms; observer bus;
  E2E hook).
- Frame slot: `reviews/decisions/frame-phases.md` (phase 8 entry /
  exit guarantees; one-frame pipeline preserved).
- Persistence rules invoked: §7.1.1 / §7.3.1 (`CookManifest`
  side-record reload), §7.2.2 / §7.3.2 (`MeshletPak` `FormatHash`
  invalidation, no migration), §7.2.3 (`FormatHash` derivation),
  §7.3.4 (frozen meshlet caps fold into `FormatHash`).
- Aggregates touched: §4.1.7 `MeshletPak` (replaced wholesale),
  §4.1.7.1 `PakHeader` (validated on every swap), §4.1.7.2
  `BLASRecipe` (re-applied), §4.1.9 `MeshHandle` (bit-equal
  survival), §4.1.10 `MeshletGroupHandle` (re-resolved),
  §4.1.11 `PakReader` (replaced), §4.1.12 `DecodePool` (entries
  invalidated, pool unchanged), §4.1.13 `ResidencyState`
  (re-evaluated), §4.1.14 `GeometryRegistry` (slot re-pointed),
  §4.1.15 `GpuMeshBuffers` (released and lazily re-materialised),
  §4.2 invariant 1 (`FormatHash` is the sole schema-drift gate),
  §4.2 invariant 10 (geometry never enqueues GPU work — the BLAS
  rebuild is render's job).
- Errors used: `geometry::Error::PakFormatHashMismatch`,
  `geometry::Error::CookManifestInvalid`,
  `geometry::Error::DecodePoolUndersized`,
  `geometry::Error::MeshletGroupHandleStale` (§10 enum), each
  wrapped by `core::Error::HotReloadRefused` per protocol
  §"Refusal Cases".
- Peer hot-reload contracts: `specs/render/SPEC.md` §8 (render's
  own dylib-reload contract; consumes geometry's `MeshReplaced`
  event per §8.5).

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
