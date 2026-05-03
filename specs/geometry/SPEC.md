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

Harmonius requirement IDs / file paths cited as research input. Note any
collapse decisions (multiple harmonius concepts → one glibre primitive).

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
