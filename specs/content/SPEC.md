# Content Spec

## 1. Purpose

The `content` context owns one responsibility: turning a tree of
artist-authored **source assets** on disk into immutable **cooked
artifacts** addressed by BLAKE3 hash, then keeping the right subset of
those artifacts resident in host memory while the runtime is using
them. Concretely it owns the **cook pipeline**
(`source → normalize → fory-serialize → CAS write → manifest update`)
and its three importer front ends — the FBX SDK importer for meshes,
skeletons, and scene hierarchies (the only mesh path for MVP; no
glTF / cgltf), the FreeImage importer for textures (PNG / JPEG / EXR
/ HDR / TIFF source decode into the engine's canonical pixel layout),
and the FreeType importer for fonts (glyph metrics + atlas baking) —
together with the **content-addressable store** under
`cooked/<prefix>/<hash>` (BLAKE3-keyed, deduplicating, append-only at
the file layer), the **manifest** that maps stable logical asset IDs
to current content hashes plus their transitive cook-input dependency
set, the **residency manager** that streams cooked artifacts into RAM
on request and evicts under memory pressure (LRU with priority
weighted by screen coverage), the typed **asset handles** the rest of
the engine holds (opaque, hash-keyed, reference-counted), and the
**file watcher seam** that turns `platform`'s file-change events into
incremental re-cook + re-residency work. It refuses to own anything
outside that seam: domain semantics of any specific asset (prefab
composition, entity templates, gameplay data tables belong to a
future `game-framework` plugin), GPU upload of any kind (no
descriptor heaps, no buffer / texture creation, no sparse-binding
policy — `render` consumes byte ranges and handles opaquely), audio
playback or DSP (`audio` context owns mixing, sources, and decode
timing — content delivers cooked encoded blobs and walks away),
schema authoring or codegen (every persistent layout the cook emits
routes through `data`'s Fory pipeline per
`reviews/decisions/fory-codegen.md` — content is a producer of bytes
conforming to those schemas, never a definer of them), the
serialization runtime itself (Fory generators + the `glibre-types`
middleman live in `data`), shader compilation (`shader` owns
HLSL→AIR/metallib; content only carries the resulting bytecode blobs
as opaque payloads), mesh-internal geometry processing such as
meshlet building, BLAS construction, or LOD chain authoring
(`geometry` owns those — content delegates to geometry from inside
the cook step and stores the result), file-watching and OS-level file
IO mechanics (`platform` owns the watcher and `FileIo` primitives —
content subscribes), and any networked CDN / DLC / live-ops asset
distribution (post-MVP; a future `delivery` context). The collapse
rule from `PHILOSOPHY.md` applies: harmonius split this surface
across asset import (R-12.1), processing (R-12.2), database
(R-12.3), hot reload (R-12.4), streaming I/O (R-12.5), versioning
(R-12.7), and content plugins; glibre fuses what only differs by
stage of the same pipeline into one context whose single
justification is "the runtime needs an immutable, hashed,
residency-managed view of artist work".

## 2. Ubiquitous Language

Terms used unchanged in code (identifiers, file names, status
comments, tests).

| Term | Meaning |
|------|---------|
| `SourceAsset` | A file under `assets/source/...` authored by an artist or DCC plugin export — `.fbx`, `.png`, `.jpg`, `.exr`, `.hdr`, `.tif`, `.ttf`, `.otf`. The cook's only legal input. Never read by the runtime. |
| `SourceKind` | Closed sum: `Mesh` (FBX), `Texture` (PNG / JPEG / EXR / HDR / TIFF), `Font` (TTF / OTF). One importer per variant. |
| `Importer` | The front-end that reads one `SourceKind` into a normalized in-memory representation: `FbxImporter` (FBX SDK), `ImageImporter` (FreeImage), `FontImporter` (FreeType). |
| `CookStep` | One stage of the pipeline: `import → normalize → process → fory-serialize → cas-write → manifest-publish`. Each step is a pure function of its input set. |
| `CookKey` | BLAKE3 over `(source-content-hash, importer-version, normalize-params, processing-params, downstream-tool-versions)`. Identical key → cache hit, no re-cook. |
| `Cooked` | An immutable, Fory-serialized artifact written to the CAS. Addressed by `ContentHash`; never patched in place; supersession is by writing a new hash. |
| `ContentHash` | BLAKE3 of the cooked bytes; the only key under which `cooked/<prefix>/<hash>` files are stored (`prefix` = first 2 hex chars for filesystem fan-out). |
| `CAS` | Content-addressable store under `cooked/`. Append-only at the file layer; deduplicates identical artifacts; readable via mmap. |
| `AssetId` | Stable logical identity for an asset across versions — `glibre.<ctx>.<slug>` (e.g. `glibre.scene.hangar.mesh.crate-a`). The `Manifest` resolves this to a current `ContentHash`. |
| `Manifest` | Persistent table mapping each `AssetId` to its current `ContentHash`, the importer / cook params that produced it, and its transitive `DependencyEdge` set. The single source of truth for the runtime's cooked view. |
| `DependencyEdge` | A `(parent_asset_id, child_asset_id_or_source_path)` pair recording that re-cooking the child must invalidate the parent. Drives bottom-up incremental rebuilds. |
| `AssetHandle<T>` | Opaque, hash-keyed, reference-counted handle held by runtime code (`AssetHandle<Mesh>`, `AssetHandle<Texture>`, `AssetHandle<Font>`). Resolves to a byte view through the residency manager; never exposes raw paths. |
| `Residency` | The state of one cooked artifact in RAM: `Unloaded → Pending → Resident → Evicting → Unloaded`. Transitions are driven by handle ref-counts and memory pressure. |
| `ResidencyManager` | Component that owns the in-RAM working set of cooked artifacts: enqueues async loads from the CAS, holds mmap regions, performs LRU eviction with priority weighted by screen coverage supplied by `render`. |
| `ScreenCoverage` | Per-handle hint (`f32`, area-of-screen estimate) supplied by `render` to bias `ResidencyManager` priority — larger coverage → higher priority → later eviction. |
| `MemoryBudget` | Configured RAM ceiling the `ResidencyManager` defends. Exceeding it triggers progressive eviction in priority order; no allocations bypass it. |
| `LoadRequest` | Enqueued unit of work: `(ContentHash, ScreenCoverage, deadline)` consumed by the residency manager's I/O lane. |
| `WatchEdge` | A subscription registered with `platform`'s file watcher; a `FileEvent` on a path under `assets/source/` becomes a `RecookRequest` for the affected `AssetId` set via the `DependencyEdge` graph. |
| `RecookRequest` | A scheduled cook of one `AssetId` triggered by a `WatchEdge` firing or a manual editor command; runs through the same `CookStep` chain as a fresh cook. |
| `CookSession` | Bounded run that processes a queue of `RecookRequest`s with deduplication, parallelism across CPU cores, and a single atomic manifest publish at the end. |
| `MeshArtifact` | Cooked output of the `Mesh` path: vertex / index streams normalized to engine layouts plus skeleton + scene hierarchy nodes. Handed to `geometry` for meshlet / LOD / BLAS construction inside the cook. |
| `TextureArtifact` | Cooked output of the `Texture` path: pixel data in a canonical linear-or-sRGB layout plus extracted metadata (mip levels, color space, usage hint). GPU-format block compression (BC7 / ASTC / etc.) lives outside MVP scope. |
| `FontArtifact` | Cooked output of the `Font` path: glyph metrics table plus a baked SDF / bitmap atlas. Consumed by UI / text rendering plugins. |
| `ImporterError` | Closed sum of typed cook-time failures (`SourceNotFound`, `MagicMismatch`, `UnsupportedVersion`, `MalformedPayload`, `MissingDependency`, `Cancelled`). Lifted into `core::Error::ContentImport`. |
| `ResidencyError` | Closed sum of typed runtime failures (`HashNotInCas`, `ManifestStale`, `BudgetExceeded`, `IoFailure`). Lifted into `core::Error::ContentResidency`. |

## 3. Derived From

Harmonius content-pipeline prior art was mined as **research input only**;
every conclusion below is independently re-derived against
`PHILOSOPHY.md` (SOLID, SRP, plugin-only growth, static codegen, no runtime
reflection in shipping builds), the Apache Fory codegen + middleman dylib
decision (`reviews/decisions/fory-codegen.md`), and the §1/§2 commitments
above. Cited paths live under `/Users/cjhowe/Code/harmonius/docs/`. Per
PHILOSOPHY §10 every multi-source concept is collapsed into the smallest
glibre primitive that still satisfies SRP; per PHILOSOPHY §3 any concern
that does not trace back to "turn artist source files into hashed,
residency-managed cooked artifacts" is refused and routed to the owning
context.

### 3.1 Cited harmonius sources

| Cluster | Harmonius file(s) | Used for |
|---------|-------------------|----------|
| Asset import (front ends, validation, batch) | `requirements/content-pipeline/asset-import.md` (R-12.1.1 native ingest, R-12.1.2 texture sources, R-12.1.3 audio sources, R-12.1.4 schema validation, R-12.1.5 batch import + cancellation), `design/content-pipeline/asset-pipeline.md` (§ "Import Pipeline Flow", `ImportCoordinator`, `ImporterRegistry`) | Per-`SourceKind` importer seam, magic / version / hash validation, error reporting with path + offset, parallel batch import with rollback semantics. (Audio source decode rerouted — see §3.3.) |
| Content-addressable storage | `requirements/content-pipeline/asset-database.md` (R-12.3.1 BLAKE3-keyed CAS with dedup, R-12.3.2 metadata table, R-12.3.3 hash-keyed import cache, R-12.3.4 bottom-up dependency invalidation), `design/content-pipeline/asset-pipeline.md` (§ "ContentAddressableStore", "MetadataStore", "DependencyGraph", "ImportCache") | BLAKE3 keying, append-only file layer with dedup, `(source_hash, importer_version, params)` cache-key shape, transitive dependency invalidation, mmap-friendly storage layout. |
| Manifest / metadata (single source of truth) | `requirements/content-pipeline/asset-database.md` (R-12.3.2 metadata mapping IDs → hashes + deps, R-12.3.4 bottom-up invalidation), `design/content-pipeline/asset-pipeline.md` (§ "MetadataStore", `AssetMetadata`, `AssetId`, `ContentHash`) | The `Manifest` shape: `AssetId → ContentHash + DependencyEdge*`, atomic publish per `CookSession`, persistent across runs, queried by editor and runtime. |
| Streaming / residency | `requirements/content-pipeline/streaming-io.md` (R-12.5.6 priority-queue scheduling by screen-space size + camera distance + deadline, R-12.5.7 memory-pressure eviction with progressive quality drop), `design/content-pipeline/asset-pipeline.md` (residency manager surface implied by handle indirection) | `ResidencyManager` lifecycle (`Unloaded → Pending → Resident → Evicting`), LRU + screen-coverage priority, configured `MemoryBudget` defended by progressive eviction, `LoadRequest` shape. (VFS, GPU DirectStorage, mip-level streaming rerouted — see §3.3.) |
| File watch → re-cook | `requirements/content-pipeline/hot-reload.md` (R-12.4.1 native file-system notifications with debounce + dedup, R-12.4.2 affected-asset re-import with atomic swap, R-12.4.6 partial re-import of changed sub-assets in a composite source) | The `WatchEdge → RecookRequest` seam: a `FileEvent` from `platform`'s watcher fans out via the `DependencyEdge` graph into per-`AssetId` recooks, debounced and deduplicated; partial re-cook drives the cache's `CookKey` granularity. |
| Asset handles + atomic swap | `requirements/content-pipeline/hot-reload.md` (R-12.4.2 atomic pointer replacement behind a frame fence), `design/content-pipeline/asset-pipeline.md` (§ "Asset Handle Indirection") | `AssetHandle<T>` shape (opaque, hash-keyed, ref-counted; never exposes raw paths), generation-tagged slots, runtime swap aligned to a frame boundary supplied by `core` rather than owned here. |
| Binary asset format / persistence | `requirements/content-pipeline/asset-versioning.md` (R-12.7.1 mmap-loadable single binary format with magic + version + content hash + TOC) | The `Cooked` artifact shape: a Fory-serialized payload (per `reviews/decisions/fory-codegen.md`) addressed by `ContentHash`, mmap-readable; magic / version / TOC concerns collapse into Fory's envelope rather than a bespoke header. |

Inputs read but **not** adopted as content responsibilities (see §3.3):
audio source decode (`requirements/content-pipeline/asset-import.md`
R-12.1.3, `requirements/content-pipeline/asset-processing.md` R-12.2.6),
texture / mesh GPU-format processing
(`requirements/content-pipeline/asset-processing.md` R-12.2.1 BCn / ASTC,
R-12.2.2 LOD chains, R-12.2.3 meshlet building, R-12.2.4 vertex-cache
opt, R-12.2.5 lightmap UVs), shader-graph compilation
(`requirements/content-pipeline/asset-processing.md` R-12.2.7 graph→HLSL,
R-12.2.9 HLSL→DXIL/SPIR-V/MSL), the VFS / GPU-DirectStorage / archive
chain (`requirements/content-pipeline/streaming-io.md` R-12.5.1, R-12.5.3,
R-12.5.4 mip streaming, R-12.5.5 mesh-LOD streaming, R-12.5.8 pak
archives, R-12.5.9 LZ4 / Zstd codec selection, R-12.5.10 CDN
download-on-demand), shader / logic-graph / UI hot-reload paths
(`requirements/content-pipeline/hot-reload.md` R-12.4.3, R-12.4.4,
R-12.4.5, R-12.4.7 editor-runtime sync), structural diff / merge / Git
LFS (`requirements/content-pipeline/asset-versioning.md` R-12.7.3..
R-12.7.8), AI tagging / vector-index search / thumbnails
(`requirements/content-pipeline/asset-database.md` R-12.3.5, R-12.3.6,
R-12.3.7..R-12.3.9), content-plugin system + sandbox + manifest
(`requirements/content-pipeline/content-plugins.md` R-12.7.1..R-12.7.6).

### 3.2 Occam collapses (multiple harmonius concepts → one glibre primitive)

1. **Many source formats → three importers backed by three vendor SDKs.**
   Harmonius enumerated glTF 2.0 + Alembic for geometry / vertex-cache
   animation, PNG / JPEG / EXR / HDR / TIFF / KTX2 for textures, and
   WAV / FLAC / Ogg Vorbis for audio
   (`requirements/content-pipeline/asset-import.md` R-12.1.2, R-12.1.3;
   `design/content-pipeline/asset-pipeline.md` § "Overview" lists
   glTF 2.0, Alembic, KTX2, FLAC as parallel importers; the design doc
   even sketches USD as a future path). Glibre collapses MVP source
   ingest to **three importers backed by three vendor SDKs**: the
   Autodesk **FBX SDK** as the only mesh / skeleton / scene-hierarchy
   path (replacing glTF + Alembic + USD), **FreeImage** as the only
   texture-decode path (PNG / JPEG / EXR / HDR / TIFF; no KTX2 because
   GPU-format containers are out of scope per §3.3), and **FreeType**
   as the only font path (TTF / OTF; harmonius did not surface fonts
   distinctly, but the `tools` editor and any future UI plugin need
   them at MVP). One importer per `SourceKind` collapses harmonius'
   `ImporterRegistry`-of-N back ends into a closed sum (`Mesh`,
   `Texture`, `Font`) whose only reason to grow is a wholly new asset
   class — at which point the registry pattern reappears. Justification:
   PHILOSOPHY §5 (greatly reduced MVP scope) + §10 (Occam) — three
   vendor SDKs cover every artist authoring tool we care about for
   day-one ship; KTX2 / Alembic / glTF / USD / Ogg / FLAC re-enter
   post-MVP behind the same `Importer` seam without changing the cook
   pipeline shape.

2. **Many CAS designs → one BLAKE3-keyed `cooked/<prefix>/<hash>` store.**
   Harmonius split persistence across a content-addressable store
   (`requirements/content-pipeline/asset-database.md` R-12.3.1), a
   parallel persistent metadata DB (R-12.3.2), a separate import cache
   keyed by `(source + settings + tool version)` (R-12.3.3), pak
   archives organised by streaming region for shipping
   (`requirements/content-pipeline/streaming-io.md` R-12.5.8), CDN
   download-on-demand staging (R-12.5.10), and a structural-diff
   version store (`requirements/content-pipeline/asset-versioning.md`
   R-12.7.1, R-12.7.2). Glibre fuses all of this into **one CAS** under
   `cooked/<prefix>/<hash>` (BLAKE3 over the cooked Fory payload;
   `prefix` = first 2 hex chars for filesystem fan-out; append-only at
   the file layer; mmap-readable). The harmonius "cache" collapses into
   a CAS-hit lookup keyed by `CookKey` (BLAKE3 over `(source-content-
   hash, importer-version, normalize-params, processing-params,
   downstream-tool-versions)`); the harmonius "metadata DB" collapses
   into the `Manifest` (`AssetId → ContentHash + dependencies`); pak
   archives, CDN staging, and structural diff stores are post-MVP and
   re-enter as either additional reader views over the same CAS or as a
   future `delivery` context (§3.3). Justification: PHILOSOPHY §10
   (Occam) + §1 SRP — one reason to change persistence is "we changed
   what bytes a cook produces", and that lives behind one hash and one
   directory.

3. **Multiple hot-reload pipelines → one re-cook + atomic handle swap.**
   Harmonius required separate hot-reload paths for textures (descriptor
   heap update), meshes / materials (atomic pointer behind frame fence),
   shaders (PSO swap with viewport error overlay), logic graphs (state
   preservation when variable layout unchanged), and UI (preserve scroll
   / focus / animation)
   (`requirements/content-pipeline/hot-reload.md` R-12.4.2..R-12.4.5).
   Glibre collapses content's surface to one mechanism: a `WatchEdge`
   fires a `RecookRequest`, the cook produces a new `ContentHash`, the
   `Manifest` publishes the new hash atomically at the end of a
   `CookSession`, and any `AssetHandle<T>` resolves through the
   `ResidencyManager` to the new bytes on its next access. Domain
   semantics of swap (descriptor heap updates, PSO swap, logic-graph
   state migration, UI tree preservation) are routed to the owning
   context (`render`, `shader`, future `game-framework`, future UI
   plugin). Justification: SRP — content's reason to change hot-reload
   is "the cook produced new bytes"; everyone else's reason to change
   is their domain's resume semantics.

4. **R-12.7 namespace collision (versioning vs content-plugins) → one
   refusal, one cooked-format primitive.** Harmonius reused the R-12.7
   prefix for both `asset-versioning.md` (binary format, mmap, bundles,
   diff / merge, Git LFS) and `content-plugins.md` (plugin system,
   manifest, sandbox, packaging, dependency resolver). Glibre adopts
   only R-12.7.1 binary-format-with-magic-and-content-hash as the shape
   of `Cooked` (delegated to Fory per `reviews/decisions/fory-codegen.md`)
   and refuses everything else from both files — bundles / LZ4 / Zstd /
   structural diff / merge / Git LFS belong to a future `delivery` or
   tooling context; the entire content-plugin manifest / sandbox /
   marketplace surface belongs to a future `game-framework` plugin.
   Justification: PHILOSOPHY §3 (plugin-only growth — content does not
   decide what a "content plugin" is; that's the consumer's call) +
   PHILOSOPHY §1 SRP (one R-12.7 mining → one primitive that already
   exists via Fory).

### 3.3 Refusals (routed to other contexts)

Glibre's content plugin does **not** own any of the following, even
though harmonius collected them under "content pipeline". Each routes to
the owning context per §1:

| Harmonius surface | Cited file(s) | Routed to |
|-------------------|----------------|-----------|
| Domain semantics of any specific asset (prefab composition, entity templates, gameplay data tables, content-plugin manifests / sandboxing / marketplace packaging / dependency resolver, logic-graph hot-reload state preservation) | `requirements/content-pipeline/content-plugins.md` R-12.7.1..R-12.7.6 (entire file), `requirements/content-pipeline/hot-reload.md` R-12.4.4 (logic-graph state) | Future `game-framework` plugin. Content delivers cooked bytes addressed by hash and walks away; what those bytes mean to gameplay is not its problem. |
| GPU upload, descriptor heap updates, BCn / ASTC / ETC2 block compression, mip-level streaming with sparse binding, GPU DirectStorage / Metal I/O direct-to-GPU DMA, descriptor heap updates for texture hot-reload | `requirements/content-pipeline/asset-processing.md` R-12.2.1 (block compression), `requirements/content-pipeline/streaming-io.md` R-12.5.3 (GPU direct storage), R-12.5.4 (mip streaming + sparse binding), `requirements/content-pipeline/hot-reload.md` R-12.4.2 (descriptor-heap update half) | `render` plugin. Content emits canonical-layout pixel data and metadata; `render` decides upload cadence, format selection, descriptor binding, and sparse residency. |
| Audio source decode, encoding to runtime formats (Opus / ADPCM / PCM), DSP, mixing, sources, decode timing | `requirements/content-pipeline/asset-import.md` R-12.1.3 (audio source decode), `requirements/content-pipeline/asset-processing.md` R-12.2.6 (Opus / ADPCM / PCM encoding) | `audio` plugin. The cook delivers cooked encoded blobs (already-encoded by `audio`'s offline tool) under content-addressed storage; content does not decode WAV / FLAC / Ogg Vorbis at MVP. |
| Schema authoring, codegen, the serialization runtime itself (Fory generator + the `glibre-types` middleman dylib, ABI hash, migration registry) | Implicit across `requirements/content-pipeline/asset-versioning.md` R-12.7.1 (binary format authoring), `design/content-pipeline/asset-pipeline.md` (§ "Core Data Structures" treats schema as part of `harmonius_content`) | `data` plugin per `reviews/decisions/fory-codegen.md`. Content is a producer of bytes conforming to schemas authored in `data/schemas/<ctx>/<Type>.fory`; it never defines a schema. |
| HLSL authoring, shader-graph→HLSL emit, DXC / Metal Shader Converter invocation, PSO swap, shader hot-reload error overlay | `requirements/content-pipeline/asset-processing.md` R-12.2.7 (graph→HLSL), R-12.2.9 (HLSL→DXIL/SPIR-V/MSL), `requirements/content-pipeline/hot-reload.md` R-12.4.3 (shader hot-reload + PSO swap) | `shader` plugin. Content carries the resulting bytecode blobs as opaque payloads inside cooked artifacts. |
| Mesh-internal geometry processing: meshlet building (max-64-verts / max-124-tris partitioning), vertex-cache reordering, automatic LOD chain authoring (edge-collapse + silhouette preservation), BLAS construction / compaction, lightmap UV unwrapping | `requirements/content-pipeline/asset-processing.md` R-12.2.2 (LOD chains), R-12.2.3 (meshlets), R-12.2.4 (vertex-cache opt), R-12.2.5 (lightmap UVs) | `geometry` plugin. Content invokes `geometry` from inside the cook step (the FBX importer's normalize → process pipeline calls into geometry to produce `MeshArtifact`'s meshlet / LOD / BLAS structures) and stores the result; it never owns the algorithm. |
| File-watching mechanics (`FSEvents` / `inotify` / `ReadDirectoryChangesW` / `kqueue` adapters), debounce / dedup of raw OS events, async I/O abstraction (Tokio / `io_uring` / IOCP / GCD), `FileIo` primitives | `requirements/content-pipeline/hot-reload.md` R-12.4.1 (platform-native FS notifications), `requirements/content-pipeline/streaming-io.md` R-12.5.2 (Tokio direct I/O) | `platform` plugin. Content subscribes to a `WatchEdge` seam and consumes `FileEvent` deliveries; the watcher itself, the OS event coalescing, and the file-IO abstraction live in `platform`. |
| Networked CDN / DLC / live-ops asset distribution, pak-archive layout, LZ4 / Zstd compression codec selection at runtime, central-directory O(1) lookup, expansion-pack mounting, virtual file system unifying loose / archive / HTTP backings | `requirements/content-pipeline/streaming-io.md` R-12.5.1 (VFS), R-12.5.8 (pak archives), R-12.5.9 (per-chunk codec selection), R-12.5.10 (CDN download-on-demand) | Post-MVP. A future `delivery` context. The MVP CAS is loose `cooked/<prefix>/<hash>` files mmap'd by the residency manager; archive / VFS / CDN concerns re-enter as additional reader views over the same hashes without changing the cook pipeline. |
| Editor / asset-browser concerns: full-text + tag-based + faceted search, async thumbnail generation per asset type, AI-assisted tagging / vector-index semantic search / near-duplicate flagging, structural diff / three-way merge / Git LFS custom merge driver, spreadsheet-style data-table editor, per-type visual inspectors, bidirectional editor↔runtime sync channel | `requirements/content-pipeline/asset-database.md` R-12.3.5..R-12.3.9, `requirements/content-pipeline/asset-versioning.md` R-12.7.3..R-12.7.8, `requirements/content-pipeline/hot-reload.md` R-12.4.7 | `tools` plugin (editor concerns) and post-MVP. Content publishes the manifest and CAS as queryable inputs; the editor is free to build search / preview / diff / merge UI on top, but content does not author them. |

These refusals are the application of PHILOSOPHY §3 (minimal core,
plugin-only growth) + §1 (SRP) to the harmonius "content pipeline"
umbrella: anything whose reason-to-change does not collapse to "an
artist saved a source file; produce / publish / serve its cooked
artifact" lives in another plugin.

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
