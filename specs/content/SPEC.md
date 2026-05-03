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

This section enumerates the `content` context's aggregates, entities, and
value objects together with the invariants every public API boundary must
hold. Aggregates are listed in pipeline-and-residency order
(`SourceAsset → Importer → CookKey → CookedAsset → CAS → Manifest →
ResidencyManager → AssetHandle`); each owns one dimension of "turn
artist source files into hashed, residency-managed cooked artifacts".
Per PHILOSOPHY §1 (SRP), an aggregate is admitted to this list only when
its single reason-to-change does not collapse into another's; where two
harmonius primitives reduce to one glibre primitive the collapse is
cited from §3.2. Cross-context concerns — domain semantics of cooked
bytes, GPU upload, audio decode, schema authoring, shader compilation,
mesh-internal geometry processing, OS file-watching, networked
distribution, editor concerns — are explicitly delegated and never
re-asserted here (§3.3). All fallible operations return
`std::expected<T, glibre::Error>` per `reviews/decisions/error-model.md`;
`content::Error` (an arm of the engine-wide tagged union) is the only
content-internal error surface, populated by the `ImporterError` and
`ResidencyError` closed sums declared in §2.

### 4.1 Aggregate roster

#### 4.1.1 `SourceAsset` — artist-authored input file (value object)

**Reason to change:** what the cook treats as a legal input on disk —
the closed `SourceKind` set (FBX / image / font), the source-tree root
convention, the source-content-hash function. Distinct from any
importer's normalize behavior (§4.1.2) and from the cook key that keys
caching (§4.1.3).

**Composition.** A `SourceAsset` is a value object carrying:

- `path` — workspace-relative path under `assets/source/...` produced
  by the artist or DCC plugin export. The cook never reads files
  outside this root.
- `kind` — one of `SourceKind::{Mesh, Texture, Font}`. Resolved by
  extension at scan time (`.fbx`→`Mesh`; `.png`/`.jpg`/`.jpeg`/`.exr`/
  `.hdr`/`.tif`/`.tiff`→`Texture`; `.ttf`/`.otf`→`Font`).
- `format` — the concrete container/format inside that kind (e.g.
  `TextureFormat::Png`, `MeshFormat::Fbx`, `FontFormat::Ttf`). Drives
  importer dispatch; cooked outputs do not retain this.
- `source_hash` — BLAKE3 of the raw source bytes on disk, captured at
  the moment scan or watcher delivery sealed the file.

The runtime never holds a `SourceAsset`; only the cook does. The
runtime addresses cooked artifacts by `ContentHash` via `AssetHandle`.

**Identity & lifetime.** Identity is the `path` (workspace-relative)
plus `source_hash` pair: identical path with different bytes is a new
revision and produces a fresh `CookKey`. Lifetime is the duration of a
`CookSession`; `SourceAsset` values are recomputed (or fetched from a
debounced cache of recent `FileEvent`s) per session — never persisted
across runs because the source tree is the persistence.

**Public-boundary invariants.**

1. **Path-scoped to `assets/source/`.** Constructors refuse any path
   that escapes the configured source root (no `..`, no absolute
   paths, no symlinks that resolve outside the root). Violations
   return `ImporterError::SourceNotFound` lifted through
   `core::Error::ContentImport` rather than reading off-tree.
2. **Closed `SourceKind` × `format` table.** A `SourceAsset` cannot
   be constructed for any extension outside the table in §2; unknown
   extensions are rejected at scan time, not by the importer (an
   importer never sees a wrong-kind input). Adding a format is a
   deliberate central edit (a new `Importer` per §3.2 collapse #1).
3. **`source_hash` is BLAKE3 of the byte sequence as read.** No
   normalization, no metadata stripping; identical bytes on disk
   yield identical `source_hash`.
4. **Read-only at the cook seam.** No code path writes back to a
   `SourceAsset`'s file; the cook is a producer of cooked bytes, not
   a mutator of artist authoring (§1).

#### 4.1.2 `Importer` — front end per `SourceKind` (entity)

**Reason to change:** the SDK seam for one source-kind family — FBX
SDK API, FreeImage decoder set, FreeType glyph extraction. Distinct
from cook-key construction (§4.1.3) and from the persistent CAS layer
(§4.1.5).

**Composition.** Three closed-sum entities, one per `SourceKind`,
each wrapping exactly one vendor SDK at its first ingress per
PHILOSOPHY §3 + the error-model decision's third-party-wrap rule:

- **`FbxImporter`** — wraps the Autodesk FBX SDK; reads `Mesh`
  source assets into a normalized `MeshArtifact` precursor (vertex
  /index streams in engine-canonical layouts, skeleton + scene
  hierarchy nodes). Delegates meshlet partitioning, vertex-cache
  reorder, LOD chain authoring, BLAS construction, and lightmap UV
  unwrapping to `geometry` from inside the cook step — never owns
  those algorithms (§3.3). The only mesh path for MVP; glTF /
  Alembic / USD re-enter post-MVP behind this same seam.
- **`ImageImporter`** — wraps FreeImage; decodes `Texture` source
  assets (PNG / JPEG / EXR / HDR / TIFF) into the engine's canonical
  pixel layout plus extracted metadata (mip count, color space,
  usage hint). GPU-format block compression (BC7 / ASTC / ETC2) is
  out of scope and routed to `render` (§3.3); KTX2 containers
  re-enter post-MVP behind this seam if needed.
- **`FontImporter`** — wraps FreeType; extracts `Font` source
  assets (TTF / OTF) into glyph metrics + a baked SDF or bitmap
  atlas, packaged as `FontArtifact`.

Each importer carries an immutable `importer_version` (a BLAKE3 of
its compiled-in identity: SDK version, normalize-params version,
post-process options vocabulary). This version is one of the
ingredients of `CookKey` (§4.1.3).

**Identity & lifetime.** Each importer is a stateless engine-singleton
constructed at cook-session start and destroyed at session end; the
only state per-cook is a per-importer arena it owns. Identity is the
`SourceKind` it serves (closed sum: at most one importer per kind).

**Public-boundary invariants.**

1. **One importer per `SourceKind`.** A closed sum admits exactly one
   `FbxImporter`, one `ImageImporter`, one `FontImporter`; dispatch is
   exhaustive at compile time. Adding a new asset class re-introduces
   the registry pattern under explicit central edit (§3.2 collapse #1)
   — never via runtime registration.
2. **SDK exceptions never escape.** Each importer is the unique
   `-fexceptions` carve-out per the error-model decision: thrown
   exceptions from FBX SDK / FreeImage / FreeType are caught at the
   importer's first ingress and translated into `ImporterError::*`
   variants (`SourceNotFound`, `MagicMismatch`, `UnsupportedVersion`,
   `MalformedPayload`, `MissingDependency`, `Cancelled`). No
   exception crosses an importer's public boundary.
3. **Pure of effect outside its arena.** An importer reads the
   declared `SourceAsset.path`, calls into its SDK, allocates only
   inside its per-cook arena, and returns either a normalized
   in-memory representation or an `ImporterError`. No global state,
   no I/O outside the source path, no allocations on the runtime
   heap.
4. **`importer_version` participates in `CookKey`.** Bumping the
   SDK version or normalize-params vocabulary forces the
   downstream `CookKey` to change, which forces a re-cook on next
   touch (§4.1.3). Importers never silently change output for the
   same input.
5. **Cancellable.** Any in-flight import respects a `CookSession`
   cancellation token and returns `ImporterError::Cancelled`
   promptly; no detached threads or undrainable work.

#### 4.1.3 `CookKey` — cache key over (source + cooker version) (value object)

**Reason to change:** what counts as "the same cook" — the precise
inputs over which the cache hashes. Distinct from the cooked output
itself (§4.1.4) and from cooked-content addressing (§4.1.5).

**Composition.** A `CookKey` is a value object: a single 32-byte
BLAKE3 digest computed over the canonical concatenation of:

1. `source_hash` — BLAKE3 of the source bytes (from §4.1.1).
2. `importer_version` — BLAKE3 of the dispatched importer's compiled
   identity (from §4.1.2).
3. `normalize_params` — BLAKE3 of the canonical (sorted-key,
   length-prefixed) serialization of the per-source-kind normalize
   parameter set actually applied (axis convention, color-space
   hint, glyph-atlas size, …).
4. `processing_params` — BLAKE3 of the canonical serialization of
   any cook-step processing parameters (e.g. `geometry`'s meshlet /
   LOD configuration as supplied to the cook step).
5. `downstream_tool_versions` — BLAKE3 of the sorted tuple of
   `(tool_name, tool_version)` pairs for every tool the cook step
   transitively invokes (`geometry` plugin version, `glibre-foryc`
   version, `glibre-types` ABI hash from §3.2 collapse #2).

Concatenation is length-prefixed (`u64-le` byte length before each
component) so no two distinct input tuples can collide via boundary
ambiguity. The result is the cache key against which the CAS is
queried.

**Identity & lifetime.** A `CookKey` is an immutable 32-byte value;
two `CookKey`s with the same digest are interchangeable. Lifetime is
the duration it is held in a `CookSession`'s job table.

**Public-boundary invariants.**

1. **Pure function of declared inputs.** `CookKey` depends on
   exactly the five components above and on nothing else. Wall-clock
   time, machine identity, environment variables, and filesystem
   ordering may not affect the digest.
2. **Canonical serialization.** Each component above is fed in its
   declared canonical byte form; alternate representations of the
   same logical value (e.g. JSON-with-different-whitespace,
   floating-point with NaN payload bits) are rejected before
   hashing rather than allowed to produce two keys for one logical
   input.
3. **Identical key ⇒ cache hit (no re-cook).** The `CookSession`
   path is: compute `CookKey` → ask the `Manifest` if a current
   cook for the affected `AssetId` already used this key and the
   resulting `ContentHash` is present in the CAS; if so, skip the
   cook. Otherwise enqueue the cook. The cache hit is the *only*
   path that elides a cook — there is no second opt-out.
4. **Versioning is a key change, not a key annotation.** Bumping
   any component (SDK, params vocabulary, `geometry` plugin)
   changes the digest and forces a fresh cook; no migration path
   on the key itself.

#### 4.1.4 `CookedAsset` — Fory-serialized artifact (value object)

**Reason to change:** the layout of cooked bytes for a given asset
class — what fields the `MeshArtifact` / `TextureArtifact` /
`FontArtifact` Fory schemas encode. Distinct from the addressing /
storage layer (§4.1.5) and from the manifest (§4.1.6).

**Composition.** A `CookedAsset` is the immutable byte payload
produced at the end of a cook step's `fory-serialize` stage,
together with its derived `ContentHash`:

- `payload` — the Fory-serialized bytes of one of the per-class
  artifact schemas authored in `data/` per
  `reviews/decisions/fory-codegen.md`. Content is a producer of
  bytes conforming to those schemas, never a definer of them
  (§3.3): `glibre.content.MeshArtifact`,
  `glibre.content.TextureArtifact`, `glibre.content.FontArtifact`
  are the three Fory FQNs; the schemas live under
  `data/schemas/content/`.
- `content_hash` — `ContentHash` = BLAKE3(`payload`). Computed
  exactly once at the moment the bytes leave the cook step;
  thereafter the artifact is addressed by it.

The `payload`'s envelope (magic, version, content hash, type-of-
contents tag) is provided by Fory's encoding per §3.2 collapse #2;
content does not author a bespoke header.

**Identity & lifetime.** Identity is `content_hash`. Lifetime is
"forever once written" — `CookedAsset`s are immutable, append-only
in the CAS, and never patched in place; supersession is by writing
a new hash (a new `CookedAsset`) and re-pointing the `Manifest` at
it.

**Public-boundary invariants.**

1. **Determinism: identical `(source bytes, cooker version)` →
   identical `content_hash`.** Two cooks of the same `SourceAsset`
   with the same `importer_version`, `normalize_params`,
   `processing_params`, and `downstream_tool_versions` produce
   byte-equal `payload`, hence byte-equal `content_hash`. This is
   what makes the `CookKey → ContentHash` map well-defined and the
   CAS deduplicating. Determinism follows PHILOSOPHY §7 and is
   enforced by Fory's deterministic encoding (tag-sorted field
   layout, no platform intrinsics in the cook).
2. **Schema-conformant.** `payload` is a valid Fory encoding of
   exactly one of the three content-owned schema FQNs at its
   declared `SchemaVersion`. Any deviation (wrong FQN, version not
   present in `glibre-types` registry) is a producer-side defect,
   not a runtime branch — `CookSession` refuses to publish such an
   artifact and returns `ImporterError::MalformedPayload`.
3. **Immutable post-write.** Once `content_hash` is computed, the
   bytes are sealed; in-place modification is not supported by the
   cook surface (no API exposes a mutating reference). Re-cook
   produces a new `CookedAsset`; the prior one remains in the CAS
   until garbage collection (post-MVP) decides otherwise.
4. **No GPU resources, no audio decode, no shader bytecode
   authoring.** A `CookedAsset` carries domain-canonical data
   (CPU-side normalized pixels, vertex/index streams, glyph
   metrics + atlas) plus opaque blobs it is asked to ferry (e.g.
   shader bytecode produced by `shader`); it never authors any of
   these (§3.3).

#### 4.1.5 `CAS` — content-addressable store (aggregate root)

**Reason to change:** how cooked bytes are stored on disk under
`cooked/<prefix>/<hash>` — the directory layout, the prefix length,
the atomic-write protocol, the mmap-read protocol. Distinct from
the manifest's `AssetId → ContentHash` mapping (§4.1.6) and from
residency (§4.1.7).

**Composition.** The `CAS` aggregate root owns:

- The configured `cooked/` root directory.
- The fan-out convention: `cooked/<prefix>/<hash>` where `prefix`
  is the first 2 hex chars of the BLAKE3 digest (256-way fan-out
  to keep any one directory under filesystem limits at scale).
- The atomic-write protocol: cooked bytes land first at
  `cooked/<prefix>/<hash>.tmp.<pid>.<rand>` (or equivalent
  collision-free temp name) and are committed via
  `rename(tmp, final)` once the full payload + fsync are
  durable. The `rename(2)` is the atomic publication boundary.
- Read: cooked files are opened read-only and mmap'd; the
  residency manager (§4.1.7) holds the mappings, not the CAS
  itself, so the CAS is stateless across reads.

The CAS is the only persistence concern for cooked bytes; the
metadata DB / import cache / pak archive / CDN staging primitives
that harmonius split out collapse here per §3.2 collapse #2.

**Identity & lifetime.** One CAS per workspace; identity is the
`cooked/` root path. Lifetime spans the workspace lifetime and
survives across cook sessions and runs.

**Public-boundary invariants.**

1. **Address by `ContentHash` only.** Files under `cooked/` are
   opened, written, and addressed by their BLAKE3 hash; no logical
   names ever appear at the CAS layer. `AssetId`s live one layer
   up (in the `Manifest`).
2. **Append-only at the file layer.** A file at
   `cooked/<prefix>/<hash>` is never opened for write after its
   first `rename(2)` commit. Re-cook produces a new hash → a new
   file; supersession is mediated by the `Manifest`, not by
   in-place rewriting. Garbage collection of orphaned hashes is
   post-MVP.
3. **Atomic write via `rename(2)`.** Concurrent or interrupted
   writes never expose a partial file under `cooked/<prefix>/<hash>`:
   the temp file is fsynced, then atomically renamed onto the
   final path. Reads see either the prior bytes (if any) or the
   new bytes — never an in-progress prefix. Writes onto an existing
   final path are no-ops at the byte layer (the hash already
   matches; rename targets identical bytes), preserving
   deduplication.
4. **Deduplicating by construction.** Two callers writing the same
   `(content_hash, payload)` collapse to one stored file; the
   second writer's temp file is unlinked after its rename target
   is observed to already match. Hash collisions across distinct
   payloads at BLAKE3 strength are treated as cryptographically
   impossible; the runtime does not branch on them.
5. **Mmap-readable.** Once committed, a CAS file is safe to mmap
   read-only and treat as a stable byte view for the lifetime of
   the residency mapping; the residency manager's mappings are
   torn down before any (post-MVP) GC reclaim could remove the
   file.
6. **Path-scoped.** No path under `cooked/` is ever read or
   written outside the configured `CAS` root; the aggregate
   refuses out-of-root paths at its boundary.

#### 4.1.6 `Manifest` — `AssetId → ContentHash + dependencies` (aggregate root)

**Reason to change:** the persistent table that maps stable logical
identity (`AssetId`) to current cooked content (`ContentHash`) plus
the dependency edges that drive incremental rebuilds. Distinct from
the CAS's byte-storage seam (§4.1.5) and from residency (§4.1.7).

**Composition.** The `Manifest` aggregate root owns:

- A persistent mapping `AssetId → (ContentHash, CookKey,
  Set<DependencyEdge>)`. The `CookKey` is recorded so a future
  scan can decide cache-hit-vs-recook without re-deriving every
  ingredient; the `DependencyEdge` set records that re-cooking the
  child must invalidate the parent (bottom-up incremental
  rebuilds per §3.1 R-12.3.4).
- A persistent on-disk encoding: `Manifest` is itself a Fory-
  serialized artifact under `data/schemas/content/Manifest.fory`
  per `reviews/decisions/fory-codegen.md`, written next to the
  CAS root (e.g. `cooked/_manifest.fory`).
- An atomic-publish protocol: a `CookSession`'s end-of-session
  publish writes a fresh manifest blob to a temp path, fsyncs it,
  and `rename(2)`s it onto the canonical manifest path. No
  partial publish is ever observable.
- Query API: `resolve(AssetId) → std::expected<ContentHash,
  ResidencyError::ManifestStale>`, `dependents_of(AssetId) →
  Set<AssetId>` for the watcher fan-out.

**Identity & lifetime.** One `Manifest` per workspace; identity is
the `cooked/_manifest.fory` path. Lifetime spans the workspace and
survives across runs; in-RAM, exactly one manifest snapshot is
active at a time, swapped atomically per `CookSession` publish.

**Public-boundary invariants.**

1. **Single source of truth.** The runtime resolves `AssetId →
   ContentHash` only through the active `Manifest` snapshot; no
   other code path may map `AssetId` to bytes. Editor / cook
   tools may read previous snapshots for history queries
   (post-MVP), but the runtime does not.
2. **Atomic publish via `rename(2)`.** A `CookSession` either
   publishes its full updated manifest (every recooked
   `AssetId` re-mapped, every dependency edge updated) or none of
   it. Concurrent runtime queries see either the prior snapshot
   or the new snapshot; never an interleaved mixture.
3. **Manifest snapshot is internally consistent.** Every
   `ContentHash` referenced by a published manifest must exist in
   the CAS at the moment of publish (the CAS write happens
   *before* the manifest publish; ordering is enforced by the
   `CookSession`). Querying an active manifest can only return
   `ContentHash`es whose CAS files are durable.
4. **Bottom-up invalidation correctness.** The `DependencyEdge`
   set is closed under transitivity at publish time: if `A`
   depends on `B` and `B` depends on `C`, the manifest records an
   edge `A→B` and an edge `B→C` so a `FileEvent` on `C` correctly
   fans out to recook `B` then `A`. Cycles in the dependency
   graph are rejected at publish with
   `ImporterError::MalformedPayload` (a content-domain defect).
5. **`AssetId` namespace-scoped.** Every `AssetId` matches the
   `glibre.<ctx>.<slug>` convention from §2; collisions across
   sources are rejected at scan time, not silently merged.
6. **Schema-conformant.** The on-disk manifest is a valid Fory
   payload of the `glibre.content.Manifest` schema at its
   declared `SchemaVersion`. Migrations across versions follow
   the `reviews/decisions/fory-codegen.md` migration registry;
   content owns the migration body, `data` owns the dispatcher
   plumbing (§3.3).

#### 4.1.7 `ResidencyManager` — RAM working set (aggregate root)

**Reason to change:** how cooked artifacts are streamed into RAM,
prioritized, and evicted under memory pressure. Distinct from the
CAS's byte-storage seam (§4.1.5), from manifest publishing
(§4.1.6), and from `AssetHandle` indirection (§4.1.8).

**Composition.** The `ResidencyManager` aggregate root owns:

- A bounded `MemoryBudget` (RAM ceiling) configured from
  `reviews/decisions/perf-budget.md`'s content-context cell.
- A residency table per `ContentHash`: `Residency` ∈ `{Unloaded,
  Pending, Resident, Evicting}`. `Resident` carries the live
  mmap region + ref-count from outstanding `AssetHandle<T>`
  instances. State transitions are total and only originate
  inside the manager.
- A priority-ordered `LoadRequest` queue consumed by an I/O
  lane: each request carries `(content_hash, screen_coverage,
  deadline)`. `ScreenCoverage` is supplied by `render` via
  the handle-resolve seam; the manager treats it as an opaque
  scalar weight.
- An LRU + screen-coverage priority eviction policy: under
  pressure, the lowest-priority resident artifact (lowest
  `screen_coverage`, oldest LRU stamp, zero outstanding handle
  refs) is moved to `Evicting`, its mmap region is unmapped,
  and the slot returns to `Unloaded`.

The residency manager is the only owner of mmap regions over CAS
files; the CAS itself is stateless across reads (§4.1.5).

**Identity & lifetime.** One residency manager per process;
identity is the engine-singleton handle obtained from the
content plugin's init. Lifetime spans the process; state is
ephemeral (rebuilt on next run from `Manifest` + CAS lookups).

**Public-boundary invariants.**

1. **`MemoryBudget` is a hard ceiling.** The aggregate
   resident-artifact bytes never exceed `MemoryBudget`. Any
   `LoadRequest` that would exceed it triggers progressive
   eviction in priority order *before* the new mapping is
   admitted; if no evictable artifact can free enough room, the
   request transitions to `Pending` and waits, returning
   `ResidencyError::BudgetExceeded` only when its deadline
   elapses with no admission. No allocation ever silently
   overshoots.
2. **State machine is total and serialised.** Each
   `ContentHash` is in exactly one residency state at a time;
   transitions follow the closed graph
   `Unloaded → Pending → Resident → Evicting → Unloaded` and
   are arbitrated by the manager. No external code path may
   construct a residency state.
3. **Eviction respects outstanding handles.** A `Resident`
   artifact with a non-zero `AssetHandle` ref-count cannot be
   evicted; it is excluded from the eviction candidate set
   regardless of LRU age or screen coverage. This is what
   makes `AssetHandle::view()` safe (§4.1.8 invariant 2).
4. **Priority is `(screen_coverage, LRU)`.** Higher
   screen_coverage stays resident longer; ties are broken by
   most-recently-used. `screen_coverage` is supplied by the
   handle's consumer (typically `render`) and is treated as an
   opaque non-negative scalar.
5. **CAS reads are mmap-only on the load path.** The manager
   opens the CAS file at `cooked/<prefix>/<hash>` read-only,
   mmap's it, populates the `Resident` slot, and never copies
   the bytes. I/O failure surfaces as `ResidencyError::IoFailure`;
   missing hash surfaces as `ResidencyError::HashNotInCas`;
   stale manifest (the `ContentHash` no longer current) surfaces
   as `ResidencyError::ManifestStale`.
6. **Hot-reload safe.** When the `Manifest` publishes a new
   `ContentHash` for an `AssetId` that has resident bytes under
   the prior hash, the prior `Resident` slot is *not* immediately
   evicted; it is held until its outstanding handle ref-count
   reaches zero (each handle resolves through the manager and
   transparently picks up the new hash on its next access — see
   §4.1.8). Old residency drains naturally, no mid-frame swap.

#### 4.1.8 `AssetHandle<T>` — opaque, hash-keyed, ref-counted indirection (value object)

**Reason to change:** the runtime-facing handle shape — what
runtime code holds, how it resolves to bytes, how its ref-count
governs residency. Distinct from residency policy (§4.1.7) and
from the manifest's `AssetId → ContentHash` resolution (§4.1.6).

**Composition.** `AssetHandle<T>` is a generic value object
parameterised by the artifact class `T ∈ {Mesh, Texture, Font}`
(closed sum at compile time, one alias per cooked artifact
schema). Internally it carries:

- `asset_id` — the stable logical identity the runtime asked
  for. Resolved through the active `Manifest` snapshot to a
  `ContentHash` on access.
- `slot` — a generation-tagged residency slot index in the
  `ResidencyManager`'s table; increments on hot-reload swap so
  stale handle reads cannot accidentally bind to a recycled
  slot.
- `ref` — reference-counted membership in the resident set; the
  count is owned by `ResidencyManager`.

Public surface: `view() -> std::expected<std::span<const
std::byte>, content::Error>` (returns the cooked bytes), `kind()`,
explicit copy/move semantics that bump/drop the residency
ref-count. The handle never exposes a path, never exposes a
`ContentHash`, and is type-aliased per artifact class so a
`AssetHandle<Mesh>` cannot accidentally resolve a `Texture` blob.

**Identity & lifetime.** Identity is `(asset_id, slot.generation,
content_hash_at_acquire)`. Lifetime is governed by the holder;
construction increments the residency ref-count, destruction
decrements it. Generation tagging makes use-after-swap a
`ResidencyError::ManifestStale` rather than UB.

**Public-boundary invariants.**

1. **Opaque to consumers.** Runtime code (render, future
   game-framework, UI plugins) never observes a path or a raw
   `ContentHash` through this seam; only `AssetId` (compile-
   time-known string literal) on construction and the typed
   byte view on access.
2. **`view()` returns bytes for a `Resident` slot or an error.**
   While a handle's `ref` is non-zero, its slot cannot be
   evicted (§4.1.7 invariant 3); `view()` therefore yields a
   stable `std::span<const std::byte>` for the lifetime of the
   handle's outstanding ref. No partial / torn / mid-eviction
   view is ever exposed.
3. **Type-keyed.** `AssetHandle<T>::view()` returns bytes
   conforming to the Fory schema for `T`; resolving an
   `AssetId` whose manifest entry has the wrong artifact class
   returns `ImporterError::MalformedPayload` lifted through
   `core::Error::ContentImport`, rather than presenting a
   wrong-class blob.
4. **Hot-reload-transparent.** When `Manifest` publishes a new
   `ContentHash` for a handle's `asset_id`, subsequent `view()`
   calls observe the new bytes once the new hash is `Resident`;
   the prior hash's bytes remain valid only for handles that
   captured them (no in-place mutation of an outstanding
   `view()`). Generation tags catch any attempt to dereference
   a slot that was reused.
5. **Ref-count discipline.** Copy and move follow Rule-of-Five
   discipline; ref-count manipulations are atomic at the
   manager seam; a leaked handle is a defect that pins residency
   (no silent drop). Default-constructed handles are a distinct
   `Empty` state with no ref-count contribution; `view()` on
   `Empty` returns `ResidencyError::HashNotInCas`.

#### 4.1.9 `CookSession` — bounded run that publishes one manifest update (entity)

**Reason to change:** the orchestration policy for a batch of
cooks — parallelism, deduplication, dependency ordering, the
end-of-session atomic publish. Distinct from any individual
importer / cook step (§4.1.2, §4.1.4) and from watcher mechanics
(§4.1.10).

**Composition.** A `CookSession` aggregate root owns:

- A `Set<RecookRequest>` queue (deduplicated by `AssetId`).
- A worker pool that runs `CookStep` chains in parallel across
  CPU cores; each step is a pure function of its input set per
  §2.
- A staging table of `(AssetId, CookKey, ContentHash)` triples
  produced by completed cooks but not yet published.
- The single end-of-session manifest publish that atomically
  commits the staging table into the active `Manifest` (§4.1.6
  invariant 2).
- A cancellation token observed by every worker.

Sessions can be triggered by editor command (full or partial cook),
by initial workspace scan, or by a `WatchEdge` (§4.1.10).

**Identity & lifetime.** Identity is the session's start
timestamp + monotonic counter (debug only; no semantic
dependence). Lifetime is bounded — every session terminates with
either a successful publish or a rollback (no partial publish).

**Public-boundary invariants.**

1. **Atomic publish or no publish.** A session that fails any
   constituent cook step rolls back its staging table and leaves
   the active `Manifest` unchanged; the prior manifest snapshot
   remains current. There is no half-published state.
2. **CAS-write before manifest-publish.** Every `ContentHash` in
   the staging table has its `payload` written and committed to
   the CAS (atomic `rename(2)` per §4.1.5 invariant 3) *before*
   the manifest publish runs. Order is mandatory; the runtime
   never sees a manifest entry whose CAS file is missing.
3. **Dependency-ordered cook execution.** Cooks within a session
   execute in topological order over the `DependencyEdge` graph
   (children before parents); a `FileEvent` on a leaf source
   asset fans out to recook that leaf's parents, but the
   parents are scheduled to start only once their children's
   cooks have completed. Cycles → reject the session with
   `ImporterError::MalformedPayload`.
4. **Deduplicating.** Multiple `RecookRequest`s for the same
   `AssetId` within a session collapse to one cook step;
   identical `CookKey` against the prior manifest's recorded
   key with the same `ContentHash` already in the CAS skips
   the cook entirely (cache hit).
5. **Cancellable end-to-end.** Cancelling a session before
   publish leaves the prior manifest active and the CAS
   unchanged at the manifest-visible layer; orphaned cooked
   files (cooked but never referenced) are garbage-collectable
   post-MVP.

#### 4.1.10 `WatchEdge` / `RecookRequest` — file-watch fan-out (value objects)

**Reason to change:** the seam between `platform`'s file watcher
and content's recook scheduling. Distinct from `CookSession`
orchestration (§4.1.9) and from the importer / cook-step path
(§4.1.2, §4.1.4).

**Composition.** A `WatchEdge` is a value-object subscription
registered with `platform`'s file watcher (§3.3): a path under
`assets/source/` plus a debounce / dedup policy. A `FileEvent`
delivered through the platform watcher is translated by content
into one or more `RecookRequest` value objects, each carrying an
`AssetId` to recook plus a reason (`SourceChanged`,
`DependentRecook`, `ManualEditorCommand`).

Content owns only the translation seam — the fan-out from a path
event into the affected `AssetId` set via the `Manifest`'s
`DependencyEdge` graph. `platform` owns the OS-level watcher,
debounce, and event delivery (§3.3).

**Identity & lifetime.** A `WatchEdge` is identified by its
`(path, debounce_policy)` pair and lives for the workspace's
lifetime. A `RecookRequest` is identified by its `AssetId` plus
session counter; lifetime is one `CookSession`.

**Public-boundary invariants.**

1. **One `WatchEdge` per source-tree subscription.** Duplicate
   subscriptions for the same path collapse at registration
   time; `platform` is told once.
2. **`FileEvent → RecookRequest` is a pure function of the
   manifest snapshot.** Given the same `FileEvent` and the same
   active manifest at translation time, the resulting
   `Set<RecookRequest>` is identical. Determinism (PHILOSOPHY
   §7) holds at the watcher seam.
3. **No FS operations on the translation path.** The translator
   reads only the manifest's in-RAM `DependencyEdge` graph and
   does not stat / read / write the source tree; the `FileEvent`
   already carries the changed path. Source-byte reads happen
   in the importer (§4.1.2), not here.
4. **Bounded fan-out.** A `FileEvent` on a single source asset
   produces a `RecookRequest` set whose size is bounded by the
   manifest's transitive dependents of that asset. No fan-out
   amplification beyond the dependency graph is possible;
   pathological dependency graphs (which would manifest as
   high-degree `DependencyEdge` sets) are flagged at manifest
   publish time, not at watcher firing.

### 4.2 Cross-aggregate invariants

Invariants that span more than one aggregate and must hold at every
public boundary at the seams between them:

1. **Determinism: `(source_bytes, cooker_version) → content_hash` is a
   total function.** Two cooks of the same `SourceAsset` with the
   same `importer_version`, `normalize_params`, `processing_params`,
   and `downstream_tool_versions` produce byte-equal `CookedAsset`
   payloads, hence byte-equal `ContentHash`. This is what makes the
   `CookKey → ContentHash` map well-defined, the CAS deduplicating,
   and the `Manifest` snapshots reproducible across hosts and runs
   (PHILOSOPHY §7).
2. **CAS-write atomicity precedes manifest-publish atomicity.** The
   ordering inside a `CookSession` is rigid: every staged
   `ContentHash`'s bytes are committed to the CAS via the
   `rename(2)` protocol of §4.1.5 invariant 3 *before* the manifest
   publish runs (§4.1.6 invariant 2). The runtime never observes a
   `Manifest` entry pointing at a missing or partial CAS file.
3. **Manifest publication is atomic.** A `CookSession` either
   publishes its full updated `Manifest` (every recooked `AssetId`
   re-mapped, every `DependencyEdge` updated) or none of it.
   Concurrent runtime queries see exactly one of the two snapshots.
   Combined with invariant 2, this gives transactional
   "(CAS-write-set, manifest-publish)" semantics without any
   bespoke transaction manager.
4. **Residency never exceeds `MemoryBudget`.** The aggregate
   resident-artifact bytes held by `ResidencyManager` never exceed
   the configured ceiling; pressure triggers progressive eviction
   in `(screen_coverage, LRU)` order before any new mapping is
   admitted (§4.1.7 invariant 1). No `LoadRequest` or
   `AssetHandle::view()` path silently overshoots the budget.
5. **Hot-reload = re-cook + atomic `Manifest` swap.** Content's
   single hot-reload mechanism per §3.2 collapse #3: a `WatchEdge`
   delivers a `FileEvent`, the translator emits a
   `Set<RecookRequest>` (§4.1.10), a `CookSession` runs them in
   dependency order producing new `CookedAsset`s in the CAS
   (§4.1.4, §4.1.5), and a single atomic `Manifest` publish
   (§4.1.6) commits the new `AssetId → ContentHash` mapping.
   `AssetHandle` consumers transparently observe the new bytes on
   their next `view()` once the new hash is `Resident` (§4.1.8
   invariant 4); domain semantics of swap (descriptor-heap
   updates, PSO swap, logic-graph state preservation, UI tree
   preservation) are routed to the owning context (§3.3) and are
   not content's concern.
6. **`AssetId` is the only stable identity the runtime sees.** No
   aggregate above exposes a `path` or a raw `ContentHash` to the
   runtime; runtime code names assets by `AssetId`, holds opaque
   `AssetHandle<T>` values, and resolves through the
   `ResidencyManager` to a typed `std::span<const std::byte>`. The
   manifest is the single resolution point; the CAS is the single
   byte-storage point; residency is the single in-RAM-mapping
   point.
7. **Per-context error model honoured.** Every aggregate's public
   fallible operation returns `std::expected<T, glibre::Error>`
   per `reviews/decisions/error-model.md`; content's enumerators
   live in the `content::Error` arm via the closed sums
   `ImporterError` and `ResidencyError` declared in §2. No
   exception crosses any content public boundary; the importer
   carve-outs of §4.1.2 invariant 2 are the only `-fexceptions`
   sites and they translate at first ingress.
8. **Frame-phase ownership.** `ResidencyManager`'s I/O lane and
   eviction policy run on background threads scheduled by `core`;
   the `AssetHandle::view()` seam is callable from any frame phase
   `core` permits content access in (typically pre-render extract).
   `CookSession` work runs entirely off the frame loop; the only
   on-frame artifact is the in-RAM manifest swap, which is a
   pointer-flip aligned with `core`'s frame-boundary barrier per
   PHILOSOPHY §8. No aggregate above mutates state mid-frame.

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
