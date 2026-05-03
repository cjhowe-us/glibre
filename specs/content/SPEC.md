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

The header below is the §5 deliverable: every symbol that crosses the
content plugin's public boundary, declared in one C++23 header and
verified compileable with
`clang++ -std=c++23 -fsyntax-only -Wall -Wextra -Wpedantic` (also
clean under `-fno-exceptions`, the engine-wide default per
`reviews/decisions/error-model.md` §Decision #3). Bodies live inside the
content dylib; this header is the contract every caller (core, render,
future game-framework, editor / tools) compiles against. The full
surface is presented as one translation unit so reviewers see the whole
seam at once; in the real tree it splits across
`content/include/glibre/content/{error,identity,source,importer,cookkey,cooked,cas,manifest,residency,handle,watch,session}.hpp`
with section banners matching the `5.1`..`5.12` numbering used inline.

Cross-context invariants enforced by this surface:

- Every fallible operation returns `glibre::Result<T>` =
  `std::expected<T, glibre::Error>` per
  `reviews/decisions/error-model.md`. `content::Error` is a `std::variant`
  over the two closed sums declared in §2 (`ImporterError`,
  `ResidencyError`); `core` rolls this into the engine-wide
  `glibre::Error` arm when the content plugin lands.
- Aggregates listed in §4 (`SourceAsset`, importers, `CAS`, `Manifest`,
  `ResidencyManager`, `CookSession`) are forward-declared classes whose
  layout is owned inside the plugin. Callers manipulate them only
  through the methods exposed below; copy / assign are deleted on
  every aggregate root, mirroring the `unique`-resource contract used
  by `render` / `platform` / `data`.
- Cooked-byte addressing exposes only `ContentHash` (BLAKE3 of the
  Fory payload) and `AssetId` (stable logical name). No raw paths
  ever leak to the runtime — `AssetHandle<T>` is opaque
  (§4.2 cross-aggregate invariant #6).
- `AssetHandle<T>` is type-keyed per artifact class via the closed
  `tags::{mesh, texture, font}` set; a wrong-class manifest entry is a
  typed `MalformedPayload` rather than a runtime branch
  (§4.1.8 invariant 3).
- Importer SDK exceptions never escape the public boundary: each
  importer is the unique `-fexceptions` carve-out per
  `reviews/decisions/error-model.md`; thrown exceptions translate at
  first ingress into `ImporterError::*` arms (§4.1.2 invariant 2).
- The atomic-write / atomic-publish protocol (§4.2 cross-aggregate
  invariants #2, #3) is enforced inside `CAS::put` and `Manifest`'s
  internal publish path; callers never see a partial / interleaved
  snapshot.

The header forward-declares its sibling-context dependencies
(`glibre::Error`, `glibre::Result<T>`, `glibre::platform::FileEvent`)
behind feature macros so this stub compiles in isolation; the
implementation translation units include the real headers.

```cpp
// SPDX-License-Identifier: Apache-2.0
// glibre — content plugin public interface (header-only stub).
//
// This file is the §5 deliverable of `specs/content/SPEC.md`. It declares
// every symbol that crosses the content plugin's public boundary. The
// bodies live inside the content dylib; this header is the contract every
// caller (core, render, future game-framework, editor / tools) compiles
// against.
//
// Cross-context invariants embedded here:
//   * Every fallible call returns `glibre::Result<T>` per
//     `reviews/decisions/error-model.md`. `-fno-exceptions` is enforced
//     globally; the importer carve-outs translate at first ingress
//     (§4.1.2 invariant 2).
//   * Aggregates listed in §4 (`CAS`, `Manifest`, `ResidencyManager`,
//     `CookSession`, importers) are forward-declared classes whose
//     layout is owned inside the plugin. Callers manipulate them only
//     through the methods exposed below.
//   * Cooked-byte addressing exposes only `ContentHash` (BLAKE3 of the
//     Fory payload) and `AssetId` (stable logical name). No raw paths
//     leak to the runtime — `AssetHandle<T>` is opaque (§4.2 invariant 6).
//   * `AssetHandle<T>` is type-keyed per artifact class (`Mesh`,
//     `Texture`, `Font`); a wrong-class manifest entry is a typed
//     `MalformedPayload` rather than a runtime branch (§4.1.8 invariant 3).
//
// Verified compileable with
// `clang++ -std=c++23 -fsyntax-only -Wall -Wextra -Wpedantic`.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <variant>

// -----------------------------------------------------------------------
// Stand-in declarations from sibling contexts. The real definitions live
// in `core/include/glibre/error.hpp`,
// `platform/include/glibre/platform/platform.hpp`, and the Fory-generated
// headers under `data/schemas/content/`. This header forward-declares
// them so the stub compiles in isolation; the implementation .cpp files
// include the real headers, not these stubs.
// -----------------------------------------------------------------------

#if !defined(GLIBRE_HAVE_CORE_ERROR)
namespace glibre {

struct ErrorContext {
    std::string_view file{};
    int              line{0};
    std::string_view detail{};
};

class Error {
public:
    using Variant = std::variant<int /* per-context Error enums appended in core */>;

    template <class E>
    constexpr Error(E e, ErrorContext ctx = {}) noexcept
        : variant_{static_cast<int>(e)}, ctx_{ctx} {}

    constexpr const Variant&      code()  const noexcept { return variant_; }
    constexpr const ErrorContext& where() const noexcept { return ctx_; }

private:
    Variant      variant_;
    ErrorContext ctx_;
};

template <class T>
using Result = std::expected<T, Error>;

}  // namespace glibre
#endif  // GLIBRE_HAVE_CORE_ERROR

#if !defined(GLIBRE_HAVE_PLATFORM_FILEEVENT)
namespace glibre::platform {

class CanonicalPath;  // workspace-relative canonical path; owned by platform.

namespace file_event {
struct Created  { /* opaque */ };
struct Modified { /* opaque */ };
struct Deleted  { /* opaque */ };
struct Renamed  { /* opaque */ };
}  // namespace file_event

using FileEvent = std::variant<
    file_event::Created,
    file_event::Modified,
    file_event::Deleted,
    file_event::Renamed>;

}  // namespace glibre::platform
#endif  // GLIBRE_HAVE_PLATFORM_FILEEVENT

namespace glibre::content {

// -----------------------------------------------------------------------
// 5.1 Closed sums of typed failures (§4 + §10).
//
// `ImporterError` captures every cook-time failure that the per-`SourceKind`
// importer can raise; `ResidencyError` captures every runtime failure
// surfaced by `ResidencyManager` and `AssetHandle`. The two are joined
// into the `content::Error` variant — the single arm contributed by this
// context to the engine-wide `glibre::Error` per
// `reviews/decisions/error-model.md` §"Composition Rules" #1.
// -----------------------------------------------------------------------

enum class ImporterError : std::uint16_t {
    SourceNotFound,        // §4.1.1 inv #1 — path escapes assets/source/ root.
    MagicMismatch,         // §4.1.2 inv #2 — SDK reports format prefix mismatch.
    UnsupportedVersion,    // §4.1.2 inv #2 — format known, version not supported.
    MalformedPayload,      // §4.1.4 inv #2; §4.1.6 inv #4; §4.1.8 inv #3 — bad bytes / wrong class / cycle.
    MissingDependency,     // §4.1.6 inv #4 — referenced child asset unresolvable.
    Cancelled,             // §4.1.2 inv #5 — CookSession cancellation observed.
};

enum class ResidencyError : std::uint16_t {
    HashNotInCas,          // §4.1.7 inv #5; §4.1.8 inv #5 — ContentHash absent under cooked/.
    ManifestStale,         // §4.1.6 inv #2; §4.1.8 inv #4 — handle resolved against superseded snapshot.
    BudgetExceeded,        // §4.1.7 inv #1 — load deadline elapsed without admission.
    IoFailure,             // §4.1.7 inv #5 — mmap/open returned an OS error.
};

// `Error` is the single content-internal sum. Each arm carries one of the
// two closed enums declared above; `core` rolls this into `glibre::Error`'s
// variant when the content plugin lands.
using Error = std::variant<ImporterError, ResidencyError>;

// Convenience: every public fallible function in this surface returns Result<T>.
template <class T>
using Result = ::glibre::Result<T>;

// -----------------------------------------------------------------------
// 5.2 Identity primitives (§2 ubiquitous language; §4.1.1 / §4.1.4 / §4.1.6).
// -----------------------------------------------------------------------

// 32-byte BLAKE3 digest. The only key under which cooked artifacts are
// addressed inside the CAS (§4.1.5 inv #1) and the cache key over which
// the cook pipeline deduplicates (§4.1.3 inv #3).
struct ContentHash {
    std::array<std::byte, 32> bytes{};
    [[nodiscard]] friend constexpr bool operator==(ContentHash, ContentHash) noexcept = default;
};

// 32-byte BLAKE3 of the canonical concatenation of (source_hash,
// importer_version, normalize_params, processing_params,
// downstream_tool_versions). Identical key ⇒ cache hit ⇒ no re-cook.
// Distinct from `ContentHash` (which addresses cooked bytes); the
// `Manifest` records both so a future scan can decide cache-hit-vs-recook
// without re-deriving every ingredient (§4.1.6 composition).
struct CookKey {
    std::array<std::byte, 32> bytes{};
    [[nodiscard]] friend constexpr bool operator==(CookKey, CookKey) noexcept = default;
};

// Stable logical identity for an asset across versions —
// `glibre.<ctx>.<slug>` (e.g. `glibre.scene.hangar.mesh.crate-a`).
// Storage is owned by the manifest's per-asset interning table; the
// view is borrow-only. A default-constructed `AssetId` is the empty /
// invalid identity (returned by `AssetHandle{}.asset_id()`).
class AssetId {
public:
    constexpr AssetId() noexcept = default;

    [[nodiscard]] static auto from_string(std::string_view s) noexcept
        -> Result<AssetId>;

    [[nodiscard]] auto view()    const noexcept -> std::string_view { return view_; }
    [[nodiscard]] auto is_empty() const noexcept -> bool { return view_.empty(); }

    [[nodiscard]] friend constexpr bool operator==(const AssetId&, const AssetId&) noexcept = default;

private:
    constexpr explicit AssetId(std::string_view v) noexcept : view_{v} {}
    std::string_view view_{};
};

// Compile-time-stable enumeration of the artifact classes content owns
// (§3.2 collapse #1; §4.1.2). Adding a class is a deliberate central edit.
enum class SourceKind : std::uint8_t {
    Mesh,     // FBX (Autodesk FBX SDK)
    Texture,  // PNG / JPEG / EXR / HDR / TIFF (FreeImage)
    Font,     // TTF / OTF (FreeType)
};

// Per-`SourceKind` concrete container/format. Drives importer dispatch;
// cooked outputs do not retain it (§4.1.1 composition).
enum class MeshFormat    : std::uint8_t { Fbx };
enum class TextureFormat : std::uint8_t { Png, Jpeg, Exr, Hdr, Tiff };
enum class FontFormat    : std::uint8_t { Ttf, Otf };

// -----------------------------------------------------------------------
// 5.3 SourceAsset — artist-authored input file (§4.1.1).
//
// Constructors refuse any path that escapes the workspace `assets/source/`
// root (no `..`, no absolute paths, no symlink escape) per §4.1.1 inv #1.
// `source_hash` is BLAKE3 of the byte sequence as read; identical bytes
// on disk yield identical `source_hash` (§4.1.1 inv #3).
// -----------------------------------------------------------------------

struct SourceAsset {
    // Workspace-relative path under `assets/source/...`. Stored as a
    // borrowed view; backing memory is owned by the cook session arena.
    std::string_view path{};

    SourceKind       kind{SourceKind::Mesh};
    // The concrete container/format. Use `mesh_format` / `texture_format` /
    // `font_format` according to `kind`; the others are unspecified.
    union FormatTag {
        MeshFormat    mesh;
        TextureFormat texture;
        FontFormat    font;
        constexpr FormatTag() noexcept : mesh{MeshFormat::Fbx} {}
    } format{};

    // BLAKE3 of the raw on-disk bytes captured at scan/watcher delivery.
    std::array<std::byte, 32> source_hash{};

    // Construct from a workspace-relative path. Refuses out-of-root /
    // unknown-extension inputs with `ImporterError::SourceNotFound` /
    // `MalformedPayload` lifted into `glibre::Error`.
    [[nodiscard]] static auto from_path(std::string_view workspace_relative) noexcept
        -> Result<SourceAsset>;
};

// -----------------------------------------------------------------------
// 5.4 Importer trait + closed-sum concrete importers (§4.1.2).
//
// `Importer` is a non-virtual base — the closed sum admits exactly one
// concrete importer per `SourceKind` (`FbxImporter`, `FreeImageImporter`,
// `FreeTypeImporter`). Dispatch is exhaustive at compile time;
// `dispatch(...)` returns the bytes of the per-class normalized
// representation (the cook step's `fory-serialize` stage consumes this
// to produce `CookedAsset::payload`).
// SDK exceptions never escape: each importer is the unique
// `-fexceptions` carve-out per the error-model decision; thrown
// exceptions are caught at first ingress and translated into
// `ImporterError::*` (§4.1.2 inv #2).
// -----------------------------------------------------------------------

// Per-source-kind normalize parameter set. Canonical (sorted-key,
// length-prefixed) byte serialization participates in `CookKey` (§4.1.3).
struct NormalizeParams {
    // Opaque blob; the canonical serialization is computed by the cook
    // step and fed into `CookKey` directly. Content does not interpret
    // these fields here — each importer pulls a typed view inside its
    // own translation unit.
    std::span<const std::byte> canonical_bytes{};
};

// Each importer carries an immutable `importer_version` — a BLAKE3 over
// (SDK version, normalize-params vocabulary, post-process options
// vocabulary). One ingredient of `CookKey` (§4.1.3 component #2).
struct ImporterVersion {
    std::array<std::byte, 32> bytes{};
    [[nodiscard]] friend constexpr bool operator==(ImporterVersion, ImporterVersion) noexcept = default;
};

// Per-cook arena owned by the importer. Importer bodies allocate only
// here; callers see only the resulting normalized byte view.
class ImporterArena;

// Cancellation token observed by every in-flight import (§4.1.2 inv #5).
class CancellationToken;

// Base trait — non-virtual; the concrete importers below are the closed
// sum. The base exists so generic cook-step code can refer to `Importer&`
// and dispatch through a free function `dispatch_import(...)` that
// pattern-matches on `kind()`.
class Importer {
public:
    [[nodiscard]] auto kind()    const noexcept -> SourceKind      { return kind_; }
    [[nodiscard]] auto version() const noexcept -> ImporterVersion { return version_; }

    Importer(const Importer&)            = delete;
    Importer& operator=(const Importer&) = delete;

protected:
    constexpr Importer(SourceKind k, ImporterVersion v) noexcept
        : kind_{k}, version_{v} {}
    ~Importer() = default;

    SourceKind      kind_;
    ImporterVersion version_;
};

// FBX SDK ingress — the only mesh path for MVP (§3.2 collapse #1).
class FbxImporter final : public Importer {
public:
    [[nodiscard]] static auto create() noexcept -> Result<std::unique_ptr<FbxImporter>>;

    // Reads the mesh source asset, normalizes vertex/index streams and
    // skeleton/scene hierarchy, delegates meshlet / LOD / BLAS work to
    // `geometry`, and returns the bytes of the
    // `glibre.content.MeshArtifact` precursor in `out_arena`.
    [[nodiscard]] auto import_one(const SourceAsset&        src,
                                  const NormalizeParams&    params,
                                  ImporterArena&            out_arena,
                                  const CancellationToken&  cancel) noexcept
        -> Result<std::span<const std::byte>>;

    ~FbxImporter();

private:
    FbxImporter() noexcept;
    struct Impl;
    Impl* impl_{nullptr};
};

// FreeImage ingress — every texture format MVP cares about (§3.2 collapse #1).
class FreeImageImporter final : public Importer {
public:
    [[nodiscard]] static auto create() noexcept -> Result<std::unique_ptr<FreeImageImporter>>;

    // Decodes the texture source asset into the engine's canonical pixel
    // layout plus extracted metadata; returns the bytes of the
    // `glibre.content.TextureArtifact` precursor in `out_arena`.
    [[nodiscard]] auto import_one(const SourceAsset&        src,
                                  const NormalizeParams&    params,
                                  ImporterArena&            out_arena,
                                  const CancellationToken&  cancel) noexcept
        -> Result<std::span<const std::byte>>;

    ~FreeImageImporter();

private:
    FreeImageImporter() noexcept;
    struct Impl;
    Impl* impl_{nullptr};
};

// FreeType ingress — TTF/OTF glyph metrics + atlas baking (§3.2 collapse #1).
class FreeTypeImporter final : public Importer {
public:
    [[nodiscard]] static auto create() noexcept -> Result<std::unique_ptr<FreeTypeImporter>>;

    // Extracts glyph metrics + a baked SDF/bitmap atlas; returns the bytes
    // of the `glibre.content.FontArtifact` precursor in `out_arena`.
    [[nodiscard]] auto import_one(const SourceAsset&        src,
                                  const NormalizeParams&    params,
                                  ImporterArena&            out_arena,
                                  const CancellationToken&  cancel) noexcept
        -> Result<std::span<const std::byte>>;

    ~FreeTypeImporter();

private:
    FreeTypeImporter() noexcept;
    struct Impl;
    Impl* impl_{nullptr};
};

// -----------------------------------------------------------------------
// 5.5 CookKey (§4.1.3) — pure function of declared inputs.
// -----------------------------------------------------------------------

// Canonical serialization of cook-step processing parameters
// (e.g. `geometry`'s meshlet/LOD configuration as supplied to the cook).
struct ProcessingParams {
    std::span<const std::byte> canonical_bytes{};
};

// Sorted tuple of (tool_name, tool_version) pairs the cook transitively
// invokes (§4.1.3 component #5). Canonical sort order is lexicographic
// over `tool_name`; identical tool_name/tool_version pair occurrences are
// deduplicated before hashing.
struct ToolVersionPair {
    std::string_view tool_name;
    std::string_view tool_version;
};

// Construct a `CookKey` from its declared inputs (§4.1.3 inv #1, #2).
// Length-prefixed concatenation makes the digest unambiguous; the
// function rejects denormalized representations (e.g. NaN floats inside
// `params.canonical_bytes`) before hashing rather than producing a
// distinct key for one logical input.
[[nodiscard]] auto make_cook_key(const std::array<std::byte, 32>&  source_hash,
                                 ImporterVersion                    importer_version,
                                 const NormalizeParams&             normalize_params,
                                 const ProcessingParams&            processing_params,
                                 std::span<const ToolVersionPair>   downstream_tool_versions) noexcept
    -> Result<CookKey>;

// -----------------------------------------------------------------------
// 5.6 CookedAsset — Fory-serialized artifact (§4.1.4).
//
// `payload` conforms to one of the three content-owned schema FQNs at its
// declared `SchemaVersion`: `glibre.content.MeshArtifact`,
// `glibre.content.TextureArtifact`, `glibre.content.FontArtifact`.
// `content_hash = BLAKE3(payload)` is computed exactly once at the
// cook-step boundary; thereafter the artifact is addressed by it
// (§4.1.4 inv #1, #3).
// -----------------------------------------------------------------------

// The Fory FQN of the artifact class. Aliased here for readability;
// `data` owns the schema-id type per `reviews/decisions/fory-codegen.md`.
struct ArtifactSchemaId {
    std::string_view fqn{};   // e.g. "glibre.content.MeshArtifact"
    std::uint32_t    version{0};
};

struct CookedAsset {
    // The Fory-serialized bytes of one of the per-class artifact schemas.
    // Storage is owned by the cook session's staging arena until the CAS
    // write commits.
    std::span<const std::byte> payload{};

    ArtifactSchemaId schema{};

    // BLAKE3(payload). Computed by `seal` (below); never set by callers.
    ContentHash content_hash{};
};

// Compute `content_hash`, validate `schema` against the registry, and
// produce a `CookedAsset` ready for CAS write. Wrong FQN / wrong version
// is `ImporterError::MalformedPayload` (§4.1.4 inv #2).
[[nodiscard]] auto seal_cooked_asset(std::span<const std::byte> payload,
                                     ArtifactSchemaId           schema) noexcept
    -> Result<CookedAsset>;

// -----------------------------------------------------------------------
// 5.7 CAS — content-addressable store (§4.1.5).
//
// One CAS per workspace. Files at `cooked/<prefix>/<hash>` are written
// via the atomic temp-then-`rename(2)` protocol (§4.1.5 inv #3),
// mmap-readable once committed (§4.1.5 inv #5), and append-only at the
// file layer (§4.1.5 inv #2). Hash collisions at BLAKE3 strength are
// treated as cryptographically impossible.
// -----------------------------------------------------------------------

struct CasConfig {
    // Workspace-rooted absolute path to `cooked/`. Validated at create()
    // time against `platform::CanonicalPath`.
    std::string_view cooked_root{};
};

// Read view of a CAS-resident artifact: an mmap region surfaced as a
// const byte span. Lifetime is bounded by the residency mapping that
// produced it (§4.1.7 inv #5).
struct CasReadView {
    std::span<const std::byte> bytes{};
};

class CAS {
public:
    [[nodiscard]] static auto create(const CasConfig&) noexcept
        -> Result<std::unique_ptr<CAS>>;

    // Atomic write: the bytes land at a collision-free temp path, are
    // fsynced, then `rename(2)`d onto `cooked/<prefix>/<hash>`. Idempotent
    // if the final path already exists with the same bytes
    // (deduplication; §4.1.5 inv #4).
    [[nodiscard]] auto put(const CookedAsset&) noexcept -> Result<void>;

    // Open and mmap the artifact addressed by `hash`. The returned view
    // is read-only; the underlying mapping is owned by the residency
    // manager (which is the only mmap-region holder per §4.1.5).
    [[nodiscard]] auto get(ContentHash hash) noexcept -> Result<CasReadView>;

    // Cheap probe used by the cook session to decide cache-hit-vs-recook
    // without opening / mmap'ing the file.
    [[nodiscard]] auto contains(ContentHash hash) const noexcept -> bool;

    ~CAS();
    CAS(const CAS&)            = delete;
    CAS& operator=(const CAS&) = delete;

protected:
    CAS() noexcept;
};

// -----------------------------------------------------------------------
// 5.8 Manifest — `AssetId → ContentHash + dependencies` (§4.1.6).
//
// Persisted as `cooked/_manifest.fory` (Fory schema
// `glibre.content.Manifest`); published atomically per `CookSession` via
// temp-write + `rename(2)` (§4.1.6 inv #2). The runtime resolves
// `AssetId → ContentHash` only through the active snapshot (§4.1.6 inv #1).
// -----------------------------------------------------------------------

// One edge in the dependency graph (§2): re-cooking the child invalidates
// the parent. The closure is computed at publish time (§4.1.6 inv #4).
struct DependencyEdge {
    // Canonical form: the parent depends on either another asset
    // (referenced by its stable `AssetId`) or directly on a source-tree
    // path (workspace-relative under `assets/source/`).
    AssetId          parent;
    AssetId          child_asset_id;        // empty if the child is a source path
    std::string_view child_source_path;     // empty if the child is an asset
};

struct ManifestEntry {
    AssetId                          asset_id;
    ContentHash                      content_hash{};
    CookKey                          cook_key{};
    std::span<const DependencyEdge>  edges{};
};

class Manifest {
public:
    // Open / validate the persistent manifest at workspace root. Missing
    // file ⇒ empty manifest (first run); malformed payload ⇒
    // `ImporterError::MalformedPayload`.
    [[nodiscard]] static auto open(std::string_view workspace_root) noexcept
        -> Result<std::unique_ptr<Manifest>>;

    // Resolve a logical name to a current cooked content hash.
    // Stale snapshot (asset_id no longer current) ⇒
    // `ResidencyError::ManifestStale`.
    [[nodiscard]] auto resolve(AssetId asset_id) const noexcept
        -> Result<ContentHash>;

    // Watcher fan-out: every asset whose recook is invalidated by a
    // change to `asset_id` (transitive through the dependency graph).
    // Bounded by the manifest's transitive dependents (§4.1.10 inv #4).
    [[nodiscard]] auto dependents_of(AssetId asset_id) const noexcept
        -> Result<std::span<const AssetId>>;

    // Lookup the per-entry record (asset_id, hash, key, edges) used by
    // the editor / cook session. Missing entry ⇒ `ManifestStale`.
    [[nodiscard]] auto entry(AssetId asset_id) const noexcept
        -> Result<ManifestEntry>;

    ~Manifest();
    Manifest(const Manifest&)            = delete;
    Manifest& operator=(const Manifest&) = delete;

protected:
    Manifest() noexcept;
};

// -----------------------------------------------------------------------
// 5.9 ResidencyManager (§4.1.7).
//
// One per process. Owns the in-RAM working set; defends `MemoryBudget`;
// performs LRU + screen-coverage priority eviction. Mmap regions are the
// manager's exclusive responsibility — the CAS itself is stateless across
// reads (§4.1.5 / §4.1.7 inv #5).
// -----------------------------------------------------------------------

// Configured RAM ceiling the manager defends (§4.1.7 inv #1).
struct MemoryBudget {
    std::uint64_t bytes{0};
};

// Per-handle hint supplied by the consumer (typically `render` from a
// per-view screen-coverage estimate). Larger ⇒ higher priority ⇒ later
// eviction. Treated as an opaque non-negative scalar.
struct ScreenCoverage {
    float value{0.0f};
};

// One unit of work consumed by the manager's I/O lane (§2 ubiquitous
// language). `deadline_ns` is a monotonic-clock deadline; missing it
// surfaces as `ResidencyError::BudgetExceeded`.
struct LoadRequest {
    ContentHash    content_hash{};
    ScreenCoverage screen_coverage{};
    std::uint64_t  deadline_ns{0};
};

// Closed sum of states one residency slot can be in (§4.1.7 inv #2).
// The state machine is total and only originates inside the manager.
enum class Residency : std::uint8_t {
    Unloaded,
    Pending,
    Resident,
    Evicting,
};

// Generation-tagged residency slot index (§4.1.8 composition). The
// manager increments `generation` on hot-reload swap so a stale handle's
// dereference fails with `ResidencyError::ManifestStale` rather than UB.
struct ResidencySlot {
    std::uint64_t index{0};
    std::uint64_t generation{0};
    [[nodiscard]] friend constexpr bool operator==(ResidencySlot, ResidencySlot) noexcept = default;
};

class ResidencyManager {
public:
    [[nodiscard]] static auto create(const MemoryBudget&, CAS&) noexcept
        -> Result<std::unique_ptr<ResidencyManager>>;

    // Enqueue an async load. Admission may trigger progressive eviction
    // before the new mapping is admitted (§4.1.7 inv #1). Failure modes:
    // `HashNotInCas`, `IoFailure`, `BudgetExceeded`.
    [[nodiscard]] auto load(const LoadRequest&) noexcept
        -> Result<ResidencySlot>;

    // Drop a previously-acquired slot. Decrements the residency
    // ref-count; the slot becomes eviction-eligible when it reaches zero
    // (§4.1.7 inv #3).
    void release(ResidencySlot slot) noexcept;

    // Inspect the current residency state for a slot. Total over all
    // slot indices the manager has ever vended; stale generations
    // surface as `Unloaded`.
    [[nodiscard]] auto residency_of(ResidencySlot slot) const noexcept
        -> Residency;

    // Resolve a Resident slot to its mmap-backed byte view. Called by
    // `AssetHandle<T>::view()`; non-Resident states surface as the
    // appropriate `ResidencyError` (§4.1.7 inv #5; §4.1.8 inv #2).
    [[nodiscard]] auto view_of(ResidencySlot slot) const noexcept
        -> Result<std::span<const std::byte>>;

    ~ResidencyManager();
    ResidencyManager(const ResidencyManager&)            = delete;
    ResidencyManager& operator=(const ResidencyManager&) = delete;

protected:
    ResidencyManager() noexcept;
};

// -----------------------------------------------------------------------
// 5.10 AssetHandle<T> (§4.1.8).
//
// Type-keyed per artifact class (`Mesh`, `Texture`, `Font`); a wrong-class
// manifest entry surfaces as `ImporterError::MalformedPayload` rather
// than presenting a wrong-class blob (§4.1.8 inv #3). Construction
// resolves through the active `Manifest` snapshot, increments the
// residency ref-count via `ResidencyManager`, and stamps the slot
// generation observed at acquire (§4.1.8 composition). Generation
// tagging makes use-after-swap a `ResidencyError::ManifestStale`.
// -----------------------------------------------------------------------

// Closed sum of artifact classes the runtime can hold. One alias per
// cooked-artifact schema (`glibre.content.MeshArtifact` etc.).
namespace tags {
struct mesh    {};
struct texture {};
struct font    {};
}  // namespace tags

template <class Tag>
class AssetHandle {
public:
    constexpr AssetHandle() noexcept = default;

    // Acquire by stable logical name. Walks the active manifest for the
    // current `ContentHash`, asks the residency manager for a slot, and
    // bumps the ref-count. Failure modes:
    // `ResidencyError::ManifestStale` (no entry), `HashNotInCas`,
    // `BudgetExceeded`, `IoFailure`, `ImporterError::MalformedPayload`
    // (wrong artifact class for `Tag`).
    [[nodiscard]] static auto acquire(AssetId          asset_id,
                                      ScreenCoverage   coverage,
                                      const Manifest&  manifest,
                                      ResidencyManager& residency) noexcept
        -> Result<AssetHandle>;

    // Stable byte view of the current cooked payload while the handle's
    // ref is non-zero; the slot cannot be evicted (§4.1.7 inv #3 ⇒
    // §4.1.8 inv #2). Hot-reload swap is observed on the next call once
    // the new hash is `Resident` (§4.1.8 inv #4).
    [[nodiscard]] auto view() const noexcept
        -> Result<std::span<const std::byte>>;

    [[nodiscard]] auto kind()       const noexcept -> SourceKind;
    [[nodiscard]] auto asset_id()   const noexcept -> AssetId       { return asset_id_; }
    [[nodiscard]] auto valid()      const noexcept -> bool          { return residency_ != nullptr; }

    // Rule-of-Five: copy / move bump / drop the residency ref-count.
    AssetHandle(const AssetHandle&) noexcept;
    AssetHandle& operator=(const AssetHandle&) noexcept;
    AssetHandle(AssetHandle&&) noexcept;
    AssetHandle& operator=(AssetHandle&&) noexcept;
    ~AssetHandle();

    [[nodiscard]] friend constexpr bool operator==(const AssetHandle& a,
                                                   const AssetHandle& b) noexcept {
        return a.asset_id_ == b.asset_id_ && a.slot_ == b.slot_;
    }

    // Hashable: two handles with equal (asset_id, slot) hash equally.
    [[nodiscard]] auto hash() const noexcept -> std::uint64_t;

private:
    AssetId           asset_id_{};                  // empty AssetId == default Empty handle.
    ResidencySlot     slot_{};
    ContentHash       content_hash_at_acquire_{};
    ResidencyManager* residency_{nullptr};
};

using MeshHandle    = AssetHandle<tags::mesh>;
using TextureHandle = AssetHandle<tags::texture>;
using FontHandle    = AssetHandle<tags::font>;

// -----------------------------------------------------------------------
// 5.11 RecookRequest + WatchEdge (§4.1.10).
//
// `WatchEdge` is the subscription content registers with `platform`'s
// file watcher (§3.3); the watcher itself, debounce, and OS event
// coalescing live in `platform`. A `FileEvent` delivered through the
// platform watcher is translated by content into one or more
// `RecookRequest` value objects via the manifest's `DependencyEdge`
// graph (§4.1.10 inv #2).
// -----------------------------------------------------------------------

enum class RecookReason : std::uint8_t {
    SourceChanged,        // a watched source file changed on disk.
    DependentRecook,      // a parent in the DependencyEdge graph re-cooked.
    ManualEditorCommand,  // editor-initiated recook of a specific asset.
};

struct RecookRequest {
    AssetId      asset_id;
    RecookReason reason{RecookReason::SourceChanged};
};

// Content-side debounce / dedup policy paired with each subscription.
// Pure data — `platform` reads the values verbatim; content does not
// debounce here (the watcher does, per §3.3).
struct DebouncePolicy {
    std::uint32_t coalesce_ms{50};
    bool          dedup_consecutive_writes{true};
};

class WatchEdge {
public:
    // Register a subscription with `platform`'s file watcher. Duplicate
    // subscriptions for the same path collapse at registration time
    // (§4.1.10 inv #1).
    [[nodiscard]] static auto subscribe(std::string_view source_subtree,
                                        DebouncePolicy   policy) noexcept
        -> Result<WatchEdge>;

    [[nodiscard]] auto path()           const noexcept -> std::string_view { return path_; }
    [[nodiscard]] auto debounce()       const noexcept -> DebouncePolicy   { return policy_; }

    // Translate one platform-delivered `FileEvent` into the set of
    // `RecookRequest`s implied by the active manifest snapshot.
    // Pure of source-tree I/O (§4.1.10 inv #3).
    [[nodiscard]] auto translate(const ::glibre::platform::FileEvent& ev,
                                 const Manifest&                       manifest,
                                 std::span<RecookRequest>              out) const noexcept
        -> Result<std::size_t>;

    WatchEdge(WatchEdge&&) noexcept;
    WatchEdge& operator=(WatchEdge&&) noexcept;
    WatchEdge(const WatchEdge&)            = delete;
    WatchEdge& operator=(const WatchEdge&) = delete;
    ~WatchEdge();

private:
    WatchEdge() noexcept = default;

    std::string_view path_{};
    DebouncePolicy   policy_{};
    struct Impl;
    Impl* impl_{nullptr};
};

// -----------------------------------------------------------------------
// 5.12 CookSession — bounded run that publishes one manifest update (§4.1.9).
//
// Sessions can be triggered by editor command (full / partial cook), by
// initial workspace scan, or by a `WatchEdge` firing. A session that
// fails any constituent cook step rolls back its staging table; the
// active `Manifest` remains the prior snapshot (§4.1.9 inv #1).
// CAS-write strictly precedes manifest-publish (§4.2 cross-aggregate
// invariant #2).
// -----------------------------------------------------------------------

// Outcome of a session's end-of-session publish.
enum class CookOutcome : std::uint8_t {
    Published,  // every staged asset committed; new manifest is active.
    NoChange,   // every request was a cache hit; manifest unchanged.
    RolledBack, // a cook step failed; prior manifest remains active.
    Cancelled,  // cancellation observed before publish.
};

struct CookReport {
    CookOutcome   outcome{CookOutcome::NoChange};
    std::uint32_t cooks_executed{0};
    std::uint32_t cache_hits{0};
};

class CookSession {
public:
    [[nodiscard]] static auto begin(Manifest&         manifest,
                                    CAS&              cas,
                                    ResidencyManager& residency) noexcept
        -> Result<std::unique_ptr<CookSession>>;

    // Enqueue a recook request. Duplicate `AssetId`s within a session
    // collapse to one cook step (§4.1.9 inv #4).
    [[nodiscard]] auto enqueue(RecookRequest) noexcept -> Result<void>;

    // Run all queued cooks in dependency order (children before parents),
    // CAS-write each `CookedAsset`, then atomically publish the new
    // manifest snapshot. The session is consumed; calling further
    // methods on it is a defect.
    [[nodiscard]] auto commit() noexcept -> Result<CookReport>;

    // Cancel an in-flight session. Workers observe the token and return
    // `ImporterError::Cancelled`; the active manifest is unchanged.
    void cancel() noexcept;

    ~CookSession();
    CookSession(const CookSession&)            = delete;
    CookSession& operator=(const CookSession&) = delete;

protected:
    CookSession() noexcept;
};

}  // namespace glibre::content
```

### 5.1 Event types

Content emits no ECS-bus events at MVP; the in-process event-shaped
surface it produces is the `CookOutcome` value returned by
`CookSession::commit()` (success / no-change / rollback / cancelled),
plus the `ResidencyManager`'s state-machine transitions
(`Unloaded → Pending → Resident → Evicting → Unloaded`) which are
internal and are not exported as wire events. The `WatchEdge` does
not own any event delivery — it consumes
`glibre::platform::FileEvent` (§4.1.10) and produces
`RecookRequest` value objects which feed back into a `CookSession`.
External observers (editor / tooling) read state by querying
`Manifest::entry(...)` and `Manifest::resolve(...)`; there is no
publish-subscribe seam at the content boundary.

### 5.2 Serialized schemas (Fory)

Content is a producer of bytes conforming to schemas authored under
`data/schemas/content/` per `reviews/decisions/fory-codegen.md`; it
never defines a schema (§3.3). The schemas this surface depends on,
listed for cross-reference:

- `data/schemas/content/MeshArtifact.fory` — FQN
  `glibre.content.MeshArtifact`. Cooked output of the mesh path
  (§4.1.4 composition); consumed by `geometry` for downstream
  meshlet / LOD / BLAS construction inside the cook step.
- `data/schemas/content/TextureArtifact.fory` — FQN
  `glibre.content.TextureArtifact`. Cooked output of the texture
  path; consumed by `render`'s upload seam (which owns format
  selection / descriptor binding).
- `data/schemas/content/FontArtifact.fory` — FQN
  `glibre.content.FontArtifact`. Cooked output of the font path;
  consumed by the editor and any future UI plugin.
- `data/schemas/content/Manifest.fory` — FQN
  `glibre.content.Manifest`. The persisted `AssetId → ContentHash +
  CookKey + DependencyEdge*` table written to `cooked/_manifest.fory`
  via the atomic-publish protocol (§4.1.6 inv #2). Schema authoring
  and version-N→N+1 migration plumbing live in `data`; the migration
  *body* is owned here per `reviews/decisions/fory-codegen.md`
  §"Migration Mechanic". Migration rules are filled in §7 and §8.

The schemas above are referenced through the `ArtifactSchemaId` /
`Envelope<T>` surface declared in `data`'s §5; this header does not
re-declare them.

### 5.3 Error types

The closed sum is `glibre::content::Error` declared above —
`std::variant<ImporterError, ResidencyError>`. Each enumerator maps to
one §4 invariant:

| Enumerator                          | Raised when                                                    | Origin                          |
|-------------------------------------|----------------------------------------------------------------|---------------------------------|
| `ImporterError::SourceNotFound`     | path escapes `assets/source/` root or file is missing          | §4.1.1 inv #1                   |
| `ImporterError::MagicMismatch`      | importer SDK reports format-prefix mismatch                    | §4.1.2 inv #2                   |
| `ImporterError::UnsupportedVersion` | format known but file version not supported                    | §4.1.2 inv #2                   |
| `ImporterError::MalformedPayload`   | bad cook output / wrong artifact class / dependency cycle      | §4.1.4 inv #2; §4.1.6 inv #4    |
| `ImporterError::MissingDependency`  | child asset referenced by a manifest entry cannot be resolved  | §4.1.6 inv #4                   |
| `ImporterError::Cancelled`          | `CookSession` cancellation token observed mid-import           | §4.1.2 inv #5; §4.1.9 inv #5    |
| `ResidencyError::HashNotInCas`      | `ContentHash` referenced by manifest is missing from `cooked/` | §4.1.7 inv #5; §4.1.8 inv #5    |
| `ResidencyError::ManifestStale`     | handle resolved against superseded manifest snapshot           | §4.1.6 inv #2; §4.1.8 inv #4    |
| `ResidencyError::BudgetExceeded`    | `LoadRequest` deadline elapsed without admission               | §4.1.7 inv #1                   |
| `ResidencyError::IoFailure`         | `mmap`/`open` returned an OS error                             | §4.1.7 inv #5                   |

`core` rolls `glibre::content::Error` into the engine-wide
`glibre::Error` variant when the content plugin lands; that
registration is a `core` change, not a `content` change. Composition
across boundaries follows
`reviews/decisions/error-model.md` §"Composition Rules" #2 — the call
site that crosses into `content` from a sibling context translates the
inner enumerator into its own context's enum, never auto-upcasts.

Verification: the stub above compiles cleanly with
`clang++ -std=c++23 -fsyntax-only -Wall -Wextra -Wpedantic`, also
under `-fno-exceptions`, on libc++ as shipped with macOS Homebrew
LLVM (clang version 19+).

## 6. Internal Architecture

Non-binding sketch for implementers. Section §4 pins the *what* (the
ten SRP-bounded aggregates and their cross-aggregate invariants); §5
pins the *contract surface* (the C++23 header `glibre-content`
exports); §7 pins the *persistent schemas* (the three Fory types
under `data/schemas/content/`); §8 pins the *hot-reload contract*
(manifest-swap and plugin-code paths bound to phase 8). This section
sketches the *how* — the module layout, the cook pipeline shape, the
residency worker topology, the watcher bridge, and the
shipping-vs-build-time split — that together produce the §5 surface
while preserving the §4 / §7 / §8 invariants.

The split below is binding only insofar as the public-surface
guarantees in §5 and the persistence guarantees in §7 require it;
private internals (file names within a module, function signatures
that never cross a header boundary, algorithmic choices inside a
single TU) remain implementer's choice. Where this section names a
directory or symbol, that name is the one the rest of the spec, the
decision records, and the issue tracker will use; no synonyms.

### 6.1 Module layout

The content context's implementation splits along two orthogonal
seams: a **lifecycle seam** between code that runs only at build /
edit time (importers + cook orchestration) and code that ships in
the runtime dylib (the read-side: CAS, manifest, residency, watch),
and an **aggregate seam** that lifts the §4 roster directly into
directories, one reason-to-change per module. The lifecycle seam is
the source of the shipping-cuts rule (§6.5); the aggregate seam is
the source of the SRP layout below.

The build system materialises the lifecycle seam by producing two
artifacts from one source tree:

1. `tools/glibre-cook` — a host executable that links the importer
   modules, the `cook/` orchestrator, and writes into a workspace's
   `cooked/` CAS + manifest. **Not shipped.** Runs on developer
   machines and CI cookers only.
2. `glibre-content.dylib` — the runtime plugin loaded by `core`.
   Ships only the read-side modules (`cas/`, `manifest/`,
   `residency/`, `watch/`, plus the §5 public header surface).
   Importer modules and cook orchestrator code are excluded by the
   build profile (§6.5).

Both artifacts share `content/include/glibre/content/` (the §5
deliverable) and the Fory-codegen'd headers under
`data/codegen-output/include/glibre/types/content/` (per
`reviews/decisions/fory-codegen.md`).

```
content/
  include/glibre/content/        # §5 surface (compiles standalone).
    content.hpp                  # Public banner; pulls in the per-aggregate headers.
    error.hpp                    # ImporterError / ResidencyError / Error variant (§5.1).
    identity.hpp                 # AssetId, ContentHash, CookKey-as-digest (§5.2).
    source.hpp                   # SourceAsset, SourceKind, SourceFormat (§5.3).
    importer.hpp                 # ImporterCapabilities + dispatch (§5.4); host-only impl.
    cooked.hpp                   # CookedAsset / artifact-tag aliases (§5.5).
    cas.hpp                      # CAS handle + put/get methods (§5.6).
    manifest.hpp                 # Manifest snapshot + resolve / dependents_of (§5.7).
    residency.hpp                # ResidencyManager + LoadRequest (§5.8).
    handle.hpp                   # AssetHandle<T> + tags::{mesh,texture,font} (§5.9).
    watch.hpp                    # WatchEdge + RecookRequest + reason discriminant (§5.10).
    session.hpp                  # CookSession + CookOutcome (§5.11; host-only impl).
  src/
    importers/                   # Aggregate §4.1.2; build-time only (§6.5 cut).
      fbx_importer.{hpp,cpp}     # FBX SDK wrap; -fexceptions carve-out per §4.1.2 inv #2.
      freeimage_importer.{hpp,cpp}  # PNG/JPEG/EXR/HDR/TIFF decode; -fexceptions carve-out.
      freetype_importer.{hpp,cpp}   # TTF/OTF glyph + atlas bake; -fexceptions carve-out.
      dispatch.{hpp,cpp}         # SourceKind → Importer routing.
      version.{hpp,cpp}          # Per-importer compiled-identity hash (§4.1.3 component 2).
    cook/                        # Aggregate §4.1.9; build-time only (§6.5 cut).
      session.{hpp,cpp}          # CookSession orchestrator (§4.1.9 invariants 1–5).
      cook_key.{hpp,cpp}         # CookKey hashing (§4.1.3); BLAKE3 over the 5 components.
      cook_step.{hpp,cpp}        # Per-step body: import → normalize → fory → CAS → stage.
      worker_pool.{hpp,cpp}      # Bounded-parallelism job runner; cancellation token.
      topology.{hpp,cpp}         # Dependency-edge topo sort (§4.1.9 inv #3).
      stage_table.{hpp,cpp}      # (AssetId, CookKey, ContentHash) staging table.
      publish.{hpp,cpp}          # End-of-session staged-blob fsync + temp-write protocol.
    cas/                         # Aggregate §4.1.5; runtime + build-time.
      cas.{hpp,cpp}              # Configured cooked/ root + put/get surface.
      atomic_write.{hpp,cpp}     # Temp-write + fsync + rename(2) primitive (§4.1.5 inv #3).
      mmap_view.{hpp,cpp}        # Read-only mmap helper consumed by residency.
      fanout.{hpp,cpp}           # cooked/<prefix>/<hash> path layout (256-way fan-out).
    manifest/                    # Aggregate §4.1.6; runtime + build-time.
      snapshot.{hpp,cpp}         # Active Manifest snapshot; resolve / dependents_of.
      publish.{hpp,cpp}          # Atomic publish protocol — temp file + rename(2).
      pointer.{hpp,cpp}          # In-RAM active-snapshot pointer + relaxed-store flip.
      dependency_graph.{hpp,cpp} # DependencyEdge graph + transitive-closure / cycle check.
      schema_io.{hpp,cpp}        # Fory read/write of cooked/_manifest.fory (§7).
    residency/                   # Aggregate §4.1.7; runtime + build-time.
      manager.{hpp,cpp}          # ResidencyManager facade + state machine arbitration.
      slot.{hpp,cpp}             # Per-ContentHash slot: state + ref-count + mmap region.
      load_queue.{hpp,cpp}       # Bounded LoadRequest queue (priority-ordered).
      io_lane.{hpp,cpp}          # Background worker(s) draining the queue; off-frame.
      eviction.{hpp,cpp}         # (screen_coverage, LRU) eviction policy.
      budget.{hpp,cpp}           # MemoryBudget accounting; admission control.
      handle.{hpp,cpp}           # AssetHandle<T> body — view() resolves through manager.
    watch/                       # Aggregate §4.1.10; runtime + build-time.
      watch_edge.{hpp,cpp}       # WatchEdge subscription registry + dedup.
      translator.{hpp,cpp}       # FileEvent → Set<RecookRequest> via dependency graph.
      bridge.{hpp,cpp}           # Adapter to platform::FileWatcher; subscribe / unsubscribe.
      reason.{hpp,cpp}           # RecookReason discriminant.
    plugin.{hpp,cpp}             # Plugin entry: init / drain / migrate / register / shutdown.
```

The split is the §4 aggregate roster lifted directly into
directories. Each directory owns one reason to change. Adding a new
source kind adds one importer file under `importers/` (and a row in
`dispatch.cpp`); swapping the eviction policy touches
`residency/eviction.cpp` only; tightening the manifest publish
protocol touches `manifest/publish.cpp` only.

`importers/` and `cook/` link the third-party SDKs (FBX SDK,
FreeImage, FreeType) and the `geometry` plugin's cook-step API as
build-time-only dependencies; neither symbol set is reachable from
the runtime `.dylib` (§6.5).

### 6.2 Cook pipeline (build-time data flow)

`tools/glibre-cook` drives a six-stage pipeline per `RecookRequest`;
the stages are sequential, each stage's output is the input to the
next, and the pipeline is a pure function of its declared inputs
(§4.1.3 inv #1, §4.1.4 inv #1).

```
SourceAsset → Importer → normalize → fory-serialize
                                          ↓
                         (CAS write: temp + fsync + rename(2))
                                          ↓
                              (stage in CookSession table)
                                          ↓
                       (end-of-session manifest publish: temp + rename(2))
```

**Stage 1 — Source resolution.** `cook/session.cpp` consumes a
`RecookRequest`, looks up the `AssetId`'s `ManifestEntry` in the
prior snapshot for the recorded `cook_key.digest` (cache-hit
candidate, §4.1.3 inv #3), and reads the source bytes through
`platform::FileIo`. The `SourceAsset` value object is constructed
with `path`, `kind`, `format`, and `source_hash = BLAKE3(bytes)`
(§4.1.1 inv #3). Failures here lift to
`ImporterError::SourceNotFound`.

**Stage 2 — CookKey derivation.** `cook/cook_key.cpp` concatenates
the five components from §4.1.3 (length-prefixed,
deterministically-canonicalised) and BLAKE3-digests them. If the
result equals the prior manifest entry's recorded `cook_key.digest`
**and** the recorded `content_hash` resolves to a CAS file present
on disk, the stage short-circuits to "cache hit, no re-cook"
(§4.1.3 inv #3). Otherwise the request proceeds to stage 3.

**Stage 3 — Importer dispatch + normalize.** `cook/cook_step.cpp`
dispatches via `importers/dispatch.cpp` to one of `FbxImporter`,
`FreeImageImporter`, `FreeTypeImporter`. Each importer wraps its
SDK at the unique `-fexceptions` carve-out (§4.1.2 inv #2) and
yields a normalized in-memory artifact precursor (`MeshArtifact`
draft, `TextureArtifact` draft, `FontArtifact` draft). Mesh-internal
processing (meshlet partitioning, LOD chains, BLAS construction) is
delegated to the `geometry` plugin from inside the cook step
(§3.3) — content does not own those algorithms.

**Stage 4 — Fory serialise.** The normalized draft is encoded
through the codegen'd `Envelope<T>::serialize` from
`data/codegen-output/include/glibre/types/content/<Type>.hpp`
(`reviews/decisions/fory-codegen.md`). The encoded bytes are the
`CookedAsset.payload`; `content_hash = BLAKE3(payload)` is computed
exactly once at the moment the bytes leave the encoder (§4.1.4
inv #1). Schema-conformance is asserted at this step (§4.1.4 inv #2);
malformed encodings lift to `ImporterError::MalformedPayload`.

**Stage 5 — CAS write.** `cas/atomic_write.cpp` writes the bytes to
`cooked/<prefix>/<hash>.tmp.<pid>.<rand>`, fsyncs, then `rename(2)`s
onto `cooked/<prefix>/<hash>` (§4.1.5 inv #3). The `rename(2)` is
the atomic publication boundary at the file layer; concurrent
writes of the same hash collapse to one stored file by §4.1.5
inv #4. I/O failures lift to `ResidencyError::IoFailure`.

**Stage 6 — Stage triple.** `cook/stage_table.cpp` records the
`(AssetId, CookKey, ContentHash)` triple plus the post-cook
`Set<DependencyEdge>` into the session's staging table. Cooks
within a session execute children-before-parents (§4.1.9 inv #3);
parents wait on the staging table for their children's triples
before their own stage 3 runs.

**Manifest publish (end-of-session).** When every queued
`RecookRequest` has either cache-hit or written its triple into
the staging table, `cook/publish.cpp` builds the new manifest
blob (every recooked entry re-mapped, every unchanged entry
byte-equal, every dependency edge updated, cycle-freedom asserted
per §4.1.6 inv #4), serialises it through Fory into a temp file
next to `cooked/_manifest.fory`, fsyncs the temp file, and
**hands ownership of the temp path + final-path target to the
loader's phase-8 barrier** for the actual `rename(2)` flip
(§8.2 step 4). `CookOutcome::Published` is returned to the
session's caller; the in-RAM active-manifest pointer flip is
deferred to the next phase 8 (§4.2 inv #8). Failure of any cook
step rolls back the entire staging table and leaves the prior
manifest snapshot active (§4.1.9 inv #1) — no half-published
state.

### 6.3 Residency worker (runtime data flow)

`residency/manager.cpp` owns the in-RAM working set. The worker
topology is bounded by two rules: I/O is off-frame (§4.2 inv #8),
and the budget ceiling is hard (§4.1.7 inv #1). The flow is:

1. **Handle access requests bytes.** `AssetHandle<T>::view()`
   resolves through the active `Manifest` snapshot to a
   `ContentHash`, then asks the manager for the corresponding slot
   (`residency/slot.cpp`).
2. **Slot state branch.** If the slot is `Resident` and ref-count
   was non-zero on entry, `view()` returns the live
   `std::span<const std::byte>` immediately — no I/O, no eviction
   pressure. If `Unloaded` or `Pending`, the manager enqueues a
   `LoadRequest{content_hash, screen_coverage, deadline}` onto
   `residency/load_queue.cpp` and returns a pending sentinel; the
   handle re-tries on the next frame's extract pass.
3. **I/O lane drains the queue.** A small worker pool
   (`residency/io_lane.cpp`, sized by
   `reviews/decisions/perf-budget.md`'s content cell) services
   `LoadRequest`s in priority order. For each request the lane:
   - looks up the CAS file at `cooked/<prefix>/<hash>` via
     `cas/fanout.cpp`;
   - opens read-only, mmap's the bytes via `cas/mmap_view.cpp`;
   - admits the new mapping into the resident set, gated by
     `residency/budget.cpp` — if admission would exceed
     `MemoryBudget`, eviction (step 4) runs first;
   - transitions the slot `Pending → Resident` and signals any
     waiting `view()` calls (next-frame poll).
4. **Eviction under pressure.** `residency/eviction.cpp`
   selects the lowest-priority `Resident` slot whose ref-count
   is zero (`(screen_coverage, LRU)` ordering, §4.1.7 inv #4).
   Selected slots transition `Resident → Evicting → Unloaded`;
   their mmap region is unmapped and the byte ceiling is
   credited. Slots with non-zero ref-counts are excluded
   regardless of priority (§4.1.7 inv #3 — what makes
   `view()` safe). If no evictable slot can free enough room,
   the load request transitions to `Pending` and waits;
   `ResidencyError::BudgetExceeded` surfaces only when the
   request's deadline elapses with no admission (§4.1.7 inv #1).
5. **Hot-reload coexistence.** When the `Manifest` re-points
   an `AssetId` from `H_old` to `H_new`, both slots can coexist
   in the residency table (§4.1.7 inv #6, §8.3). The prior
   slot is held until its outstanding handle ref-count
   reaches zero, then drains via the standard graph; the new
   slot is admitted by the next handle access through the
   above flow.

The I/O lane and the eviction policy run on background threads
scheduled by `core` outside the frame loop (§4.2 inv #8). The
on-frame surface is exclusively `view()` reads of already-`Resident`
slots; any frame-phase that depends on a fresh load tolerates a
one-frame retry (extract systems use a sentinel + carry-over
pattern owned by their respective contexts).

### 6.4 Hot-reload bridge (`watch/` → `cook/` → manifest swap)

`watch/` is the seam that turns `platform`'s file watcher firings
into work for the cook pipeline. Three files cooperate:

1. **`watch/bridge.cpp`** — adapter to `platform::FileWatcher`.
   Subscribes one watch handle per source-tree subscription
   recorded in `WatchEdge` (§4.1.10 inv #1). Receives
   `platform::FileEvent` deliveries on the platform watcher's
   thread; forwards the path + event kind into the translator
   without performing any FS I/O of its own (§4.1.10 inv #3).
2. **`watch/translator.cpp`** — the `FileEvent → Set<RecookRequest>`
   translator. Reads only the active `Manifest`'s in-RAM
   `DependencyEdge` graph (§4.1.10 inv #3); for the changed source
   path, walks transitive dependents (`SourceChanged` for the leaf,
   `DependentRecook` for each parent, §4.1.10 composition) and
   emits one `RecookRequest` per affected `AssetId`. Bounded by
   the dependency graph's transitive closure (§4.1.10 inv #4).
3. **`watch/watch_edge.cpp`** — the registry. Deduplicates
   subscriptions at registration; tracks the
   `(path, debounce_policy)` identity. The actual debounce window
   is owned by `platform`'s watcher (§3.3); content carries only
   the policy descriptor.

Translated `RecookRequest`s are enqueued onto a recook channel
consumed by `cook/session.cpp`; one `CookSession` is begun per
batch (the session's batching policy is owned by the
orchestrator — typical heuristic: open a session on first request,
close it after a debounce window or once the queue drains, never
keep an unbounded open session). The session runs the pipeline of
§6.2 and produces a staged manifest blob; the actual swap is
deferred to the next phase 8 (§8.2 step 4).

The bridge is symmetric on tear-down: when the content plugin
drains (§8 plugin-code reload), every `WatchEdge` subscription is
unsubscribed via the bridge and re-registered after the new
plugin's `glibre_plugin_register` completes. The platform watcher
is unaware of the plugin swap; only the bridge's subscription
identities move.

### 6.5 Shipping cuts (build-time vs runtime)

The build profile partitions content's source tree into two
disjoint sets along the lifecycle seam introduced in §6.1:

| Module                       | `tools/glibre-cook` | `glibre-content.dylib` |
|------------------------------|---------------------|------------------------|
| `importers/fbx_importer.*`   | Yes                 | **No** (cut)           |
| `importers/freeimage_importer.*` | Yes             | **No** (cut)           |
| `importers/freetype_importer.*`  | Yes             | **No** (cut)           |
| `importers/dispatch.*`       | Yes                 | **No** (cut)           |
| `importers/version.*`        | Yes                 | **No** (cut)           |
| `cook/session.*`             | Yes                 | **No** (cut)           |
| `cook/cook_key.*`            | Yes                 | **No** (cut)           |
| `cook/cook_step.*`           | Yes                 | **No** (cut)           |
| `cook/worker_pool.*`         | Yes                 | **No** (cut)           |
| `cook/topology.*`            | Yes                 | **No** (cut)           |
| `cook/stage_table.*`         | Yes                 | **No** (cut)           |
| `cook/publish.*`             | Yes                 | **No** (cut)           |
| `cas/*`                      | Yes                 | Yes                    |
| `manifest/*`                 | Yes                 | Yes                    |
| `residency/*`                | No                  | Yes                    |
| `watch/*`                    | No                  | Yes                    |
| `plugin.*`                   | No                  | Yes                    |
| `include/glibre/content/*`   | Yes (compile-only)  | Yes                    |

The `importers/` and `cook/` cut is mechanical: those modules are
the only sites that link FBX SDK / FreeImage / FreeType / the
`geometry` plugin's cook-step API, and the runtime never invokes
them. The §5 header surface includes their forward declarations
(`CookSession`, `Importer`, `SourceAsset`) so callers compile
uniformly, but the runtime profile elides every TU under those
two directories from the `glibre-content.dylib` link line. Calling
any cook-side API from a shipping build is a link-time error
(undefined symbol), not a runtime branch.

`residency/` and `watch/` are runtime-only by symmetry: the cook
tool never resolves an `AssetHandle` and never subscribes to a
file watcher; those modules are elided from the `glibre-cook`
link line. `cas/` and `manifest/` link into both — the cook tool
writes them, the runtime reads them.

The `plugin.cpp` entry implements `glibre_plugin_init`,
`glibre_plugin_drain`, `glibre_plugin_migrate`,
`glibre_plugin_register`, and `glibre_plugin_shutdown` per the
engine plugin ABI (`reviews/decisions/plugin-abi.md`). It is
runtime-only; the cook tool drives the pipeline directly and
needs no plugin entry. Its body is mechanical: spin up the
worker pool used by `residency/io_lane.cpp` on init, tear it
down on drain, hand the survival inventory of §8.3 across the
swap on migrate, re-register `WatchEdge` subscriptions on
register, and free the residency table on shutdown.

The split mechanically realises PHILOSOPHY §3 (minimal core,
plugin-only growth — and within a plugin, the read side is the
shipping payload) and the §1 commitment that "the runtime needs
an immutable, hashed, residency-managed view of artist work" —
the runtime ships only what it needs to read, not what produced
the bytes.

## 7. Persistence & Schemas

Content's persistence surface is intentionally split into two layers
that this section pins separately:

1. The **manifest layer** — small, structured, schema-evolved Fory
   records that map `AssetId → ContentHash + CookKey + dependency
   edges`. This is the only thing the content context owns *as a
   schemaful artefact*. Three persistent types live here, all under
   `data/schemas/content/<Type>.fory` per
   `reviews/decisions/fory-codegen.md`: `ManifestEntry`, `CookKey`,
   `DependencyEdge`. Their FQNs are `glibre.content.<Type>`.
2. The **CAS payload layer** — the cooked `MeshArtifact` /
   `TextureArtifact` / `FontArtifact` blobs under `cooked/<prefix>/<hash>`.
   The payload schemas are listed in §5.2 for cross-reference; the
   manifest layer references those payloads only by `ContentHash`. The
   cooked bytes themselves are **not manifest-managed at field level**
   — they are opaque to the manifest, addressed by their BLAKE3 digest,
   and never patched in place (§4.1.4 inv #3, §4.1.5 inv #2).

The `Manifest` aggregate (§4.1.6) is itself the on-disk file
`cooked/_manifest.fory`; its top-level encoding is a list of
`ManifestEntry` records. There is no separate `Manifest`-the-record
schema beyond `list<glibre.content.ManifestEntry>` plus the standard
`EnvelopeHeader` (data SPEC §7.2.4).

Schemas are authored as `.fory` text files using the grammar in data
SPEC §7.1, generated into the `glibre-types` middleman dylib by
`glibre-foryc`, and ship with at least one Catch2 round-trip test
under `tests/data/schemas/content/<Type>.cpp` per data SPEC §7.5.

### 7.1 Persistent types

#### 7.1.1 `ManifestEntry` — one row of the asset table

**File:** `data/schemas/content/ManifestEntry.fory`
**FQN:** `glibre.content.ManifestEntry`
**Lifetime scope:** per-workspace; written by the `CookSession`
end-of-session publish (§4.1.9 inv #1); read at runtime through the
active `Manifest` snapshot (§4.1.6 inv #1) and at startup by the cook
to decide cache-hit-vs-recook (§4.1.3 inv #3).

```fory
schema glibre.content.ManifestEntry {
  version 1
  since   "0.1.0"

  field asset_id     : string                          tag 1 since 1
  field content_hash : bytes                           tag 2 since 1
  field cook_key     : glibre.content.CookKey          tag 3 since 1
  field edges        : list<glibre.content.DependencyEdge>
                                                       tag 4 since 1
}
```

- `asset_id` is the stable logical identity from §2 / §4.1.6 inv #5,
  matching `glibre.<ctx>.<slug>`. Validated at parse time against the
  same regex `Foryc` uses for built-in `Ident` strings; malformed
  values become `ImporterError::MalformedPayload`.
- `content_hash` is exactly 32 bytes (BLAKE3-256 of the cooked
  payload, §4.1.4 inv #1). Shorter or longer sequences are
  `ImporterError::MalformedPayload`. The hash is opaque here — its
  agreement with the addressed file at `cooked/<prefix>/<hash>` is a
  publish-time invariant of the `CookSession` (§4.1.9 inv #2), not a
  field-level constraint.
- `cook_key` is the recorded cache key from the cook that produced
  `content_hash` (§4.1.6 composition; §4.1.3 inv #1). Recording the
  key — rather than re-deriving its ingredients on every scan —
  collapses the bottom-up incremental check to a single BLAKE3
  comparison.
- `edges` is the per-asset dependency set from §4.1.6 composition.
  Sorted by `(child_kind, child_id_or_path)` ascending at serialise
  time so two semantically-equal manifests round-trip byte-equal
  (deterministic encoding per PHILOSOPHY §7).

**Invariants** (echoing §4.1.6 where the manifest aggregate enforces
them):

1. **Internally consistent at publish.** Every `ManifestEntry` whose
   `content_hash` is published must have its CAS file durable on disk
   first (§4.1.6 inv #3, §4.1.9 inv #2). The schema does not encode
   this — it is a publish-protocol obligation — but the round-trip
   test fixture asserts it for the per-version goldens.
2. **Atomic publish.** A persisted manifest payload is either the
   complete updated table (every entry re-mapped) or absent; no
   partial write is ever observable at the `cooked/_manifest.fory`
   path (§4.1.6 inv #2, §4.1.9 inv #1). Enforced by the temp-write +
   `rename(2)` protocol; the schema is unaware of the file system.
3. **Cycle-free `edges` closure.** The union of every entry's `edges`
   forms a DAG; cycles are rejected at publish with
   `ImporterError::MalformedPayload` (§4.1.6 inv #4). The schema
   permits any `DependencyEdge` list; cycle detection is a publish-
   time invariant of the `Manifest` aggregate, not a field-level
   constraint.

#### 7.1.2 `CookKey` — canonicalised cache key

**File:** `data/schemas/content/CookKey.fory`
**FQN:** `glibre.content.CookKey`
**Lifetime scope:** embedded inside `ManifestEntry`; never persisted
standalone. The schema exists so the canonicalised input set the
digest is computed *over* is recoverable from the manifest at
diagnostic time (cache misses, audit trails, importer regressions),
without re-running the cook.

```fory
schema glibre.content.CookKey {
  version 1
  since   "0.1.0"

  field digest                   : bytes  tag 1 since 1
  field source_hash              : bytes  tag 2 since 1
  field importer_version         : bytes  tag 3 since 1
  field normalize_params_hash    : bytes  tag 4 since 1
  field processing_params_hash   : bytes  tag 5 since 1
  field downstream_versions_hash : bytes  tag 6 since 1
}
```

- `digest` is the 32-byte BLAKE3-256 cache key from §4.1.3 — the
  length-prefixed concatenation of the five component hashes below.
  Two `CookKey`s with the same `digest` are interchangeable for
  cache-hit purposes (§4.1.3 inv #3).
- `source_hash` (32 bytes) is `BLAKE3(SourceAsset bytes)` (§4.1.1 /
  §4.1.3 inv #1).
- `importer_version` (32 bytes) is `BLAKE3(dispatched importer's
  compiled identity)` (§4.1.2 inv #4 / §4.1.3 inv #1).
- `normalize_params_hash` (32 bytes) is `BLAKE3(canonical
  serialisation of the per-source-kind normalise parameter set
  applied)`. Canonical serialisation here means sorted-key,
  length-prefixed bytes per §4.1.3 inv #2.
- `processing_params_hash` (32 bytes) is `BLAKE3(canonical
  serialisation of cook-step processing parameters)` (§4.1.3 inv #1
  component 4).
- `downstream_versions_hash` (32 bytes) is `BLAKE3(sorted-tuple
  serialisation of every transitively-invoked tool's
  (name, version))`, including `geometry` plugin version,
  `glibre-foryc` version, and the `glibre-types` ABI hash from
  data SPEC §4.4.

**Invariants:**

1. **Digest is reproducible.** Recomputing `BLAKE3` over the
   length-prefixed concatenation of the five component hashes, in
   the order declared above, MUST yield `digest` exactly. The
   round-trip test fixture asserts this for every shipped golden;
   this is the schema-level enforcement of §4.1.3 inv #1 (pure
   function of declared inputs).
2. **All hashes are exactly 32 bytes.** Any other length is
   `ImporterError::MalformedPayload` at deserialise time, before any
   higher layer sees the value.
3. **Canonical-input audit only.** The component hashes are recorded
   for diagnostic recoverability (cache-miss audit, importer
   regression triage); the runtime cache-hit path keys only on
   `digest` (§4.1.3 inv #3). No code path re-derives `digest` from
   the components at runtime — that is a build-time assertion of
   the schema's golden tests.
4. **Versioning is a key change, not a key annotation** (§4.1.3
   inv #4). Bumping the schema does not migrate the recorded keys;
   see §7.2.2.

#### 7.1.3 `DependencyEdge` — one edge of the bottom-up invalidation graph

**File:** `data/schemas/content/DependencyEdge.fory`
**FQN:** `glibre.content.DependencyEdge`
**Lifetime scope:** embedded inside `ManifestEntry.edges`; never
persisted standalone. The persisted form mirrors the in-memory
`DependencyEdge` struct in §5.1 / §5.2 with the discriminant made
explicit so the on-disk encoding is unambiguous.

```fory
schema glibre.content.DependencyEdge {
  version 1
  since   "0.1.0"

  field parent     : string         tag 1 since 1
  field child_kind : u8             tag 2 since 1
  field child      : string         tag 3 since 1
}
```

- `parent` is the dependent's `AssetId` (the asset that must be
  re-cooked when `child` changes). Same validation as
  `ManifestEntry.asset_id`.
- `child_kind` is a closed-sum discriminant over the two child
  variants from §4.1.6 / §5.2:
  - `0` = `Asset` — `child` is another `AssetId`.
  - `1` = `SourcePath` — `child` is a workspace-relative path under
    `assets/source/...`.
  Any other value is `ImporterError::MalformedPayload` at
  deserialise time. Treating the discriminant as a `u8` rather than
  a Fory `enum` is intentional: future kinds (e.g.
  `ExternalToolVersion`, `EnvironmentInput`) extend the closed sum
  via new positions per §7.2.3 without moving any tag — see the
  migration rules below.
- `child` is the textual identity matching `child_kind`: an
  `AssetId` regex when `child_kind == 0`, or a workspace-relative
  path when `child_kind == 1`. The `SourcePath` variant is sorted
  by NFC-normalised Unicode code-point order at canonicalisation
  time so identical edge sets serialise byte-equal.

**Invariants:**

1. **Closed discriminant.** The set of recognised `child_kind`
   values is fixed by the running middleman dylib's schema version;
   unknown values cannot be silently tolerated (PHILOSOPHY §6 — no
   reflection-driven runtime branches). Decoding a value the dylib
   does not recognise is `ImporterError::MalformedPayload`.
2. **Path discipline.** When `child_kind == SourcePath`, `child`
   MUST start with `assets/source/` and contain no `..` segments;
   otherwise `ImporterError::SourceNotFound` (matches §4.1.1 inv #1
   path-scope rule). Validated at deserialise time.
3. **Cycle-freedom is the manifest's invariant.** A
   `DependencyEdge` in isolation cannot encode a cycle; cycle
   detection runs across the union of every published
   `ManifestEntry.edges` set (§4.1.6 inv #4). The edge schema does
   not constrain global topology.

### 7.2 Migration rules

Per `reviews/decisions/fory-codegen.md` §"Migration Mechanic" and
data SPEC §7.4, every schema-version bump generates a dispatcher
hookup; content owns the migration *bodies*, `data` owns the
plumbing. The three rules below differ from the typical
`data/schemas/<ctx>/<Type>.fory` pattern because the manifest layer
sits directly in front of an opaque content-hashed cache that is
itself an authoritative re-cook source.

#### 7.2.1 `ManifestEntry` — additive variants only

The manifest schema is **additive-only** within MVP and the
foreseeable post-MVP horizon. Two flavours of additive change are
recognised:

1. **Append a new defaulted field at a new tag.** Example: a
   future `last_published_at_unix_ms : u64 default 0` appended at
   tag 5. Codegen synthesises the default at deserialise time per
   data SPEC §7.4 rule #6; no migration body is required. Fory's
   `since` clause makes older payloads load cleanly into newer
   readers; older readers see Fory's "unknown trailing tag" path
   and skip the bytes per Fory's wire format.
2. **Add a new `DependencyEdge` kind.** A new `child_kind` variant
   (e.g. `ExternalToolVersion`) is an additive variant of the
   `DependencyEdge` schema — see §7.2.3. The `ManifestEntry`
   schema itself does not change.

Anything else — renaming a field, changing an existing field's
type, removing a field, or changing the meaning of a recorded
`content_hash` — is treated as **breaking** and falls under the
re-cook rule in §7.2.4. Migrating individual `ManifestEntry`
records across a breaking schema bump is explicitly **not
supported**.

Round-trip golden contract: for every shipped schema version `N`,
`tests/data/schemas/content/ManifestEntry.cpp` includes a recorded
`vN` payload golden and asserts the v`N` → v(current) chain
produces a v(current)-byte-equal payload (data SPEC §7.5).

#### 7.2.2 `CookKey` — schema-bump = full re-cook

`CookKey`'s `digest` is computed over a length-prefixed
concatenation of five component hashes (§7.1.2 / §4.1.3). Any
change to the `CookKey` schema — new component hash, removed
component hash, reordering, or width change — **alters the digest
algorithm itself**. The digest of a v1 `CookKey` is therefore
*never* equal to the digest of an equivalent v2 `CookKey`, even
when every input is byte-identical.

Consequence: a `CookKey` schema bump invalidates **every recorded
key** in the manifest, which by definition forces a re-cook of
**every asset** on the next `CookSession`. There is no live
migration of recorded `CookKey` records — the manifest is treated
as cold cache and fully rebuilt.

This rule is enforced in code by **omitting** any `migration`
clause from `CookKey.fory` entirely. The Fory codegen tool refuses
to emit a migration dispatcher for a type that declares no
migration providers (data SPEC §7.4 rule #5 mandates complete
coverage; absence of the clause is a load-bearing signal here per
data SPEC §7.6's parallel pattern for meta-schemas, lifted at
codegen-time for `CookKey` only). Future schema bumps therefore
force the author to either author an explicit migration (rejected
by review per this rule) or accept full re-cook (the only allowed
path).

Operationally: a `CookKey` schema bump rides the same release
boundary as a `glibre-types` ABI hash bump; the next workspace
open after the upgrade observes a manifest whose every entry's
`cook_key.digest` mismatches the freshly-computed key for its
asset, falls through to the cook path for every asset, and
publishes a fresh manifest. The CAS contents already on disk
(§4.1.5) survive — they are addressed by `ContentHash`, not by
`CookKey` — so the re-cook collapses to deduplicating writes for
any asset whose cooked bytes happen to round-trip byte-equal.

#### 7.2.3 `DependencyEdge` — additive variants on `child_kind`

Adding a new dependency kind extends the closed sum on
`child_kind` (§7.1.3). The migration rule is:

1. **Allocating a new `child_kind` value.** Reserve the next
   unused position in the discriminant; do **not** move existing
   positions. Existing edges loaded from a prior payload retain
   their original `child_kind`; the schema bump is additive
   because no tag changes and no field type changes (the
   discriminant remains a `u8`). Codegen treats this as a
   no-op migration (identity mapping) and does not require a
   provider body.
2. **Authoring the new variant's interpretation.** The owning
   site that emits edges with the new `child_kind` (the cook
   step, the watcher fan-out, etc.) is updated in lockstep with
   the schema bump; older `glibre-types.dylib` readers will refuse
   such payloads with `ImporterError::MalformedPayload` per §7.1.3
   inv #1, which is the desired behaviour (they cannot interpret
   the kind correctly anyway).
3. **Removing or repurposing a `child_kind` value.** Forbidden.
   Discriminant positions are immutable once shipped; deprecated
   kinds are left in place and the cook simply stops emitting them.
   This mirrors the data SPEC §7 reserved-tag rule applied to
   discriminant positions.

There is no migration body for any `DependencyEdge` change; the
schema is structurally additive by design. `tests/data/schemas/
content/DependencyEdge.cpp` carries one round-trip golden per
shipped `child_kind` value.

#### 7.2.4 Why no live migration of manifest entries

The `CookKey` schema **absorbs the full identity of every cook
input** (§4.1.3 inv #1, §7.1.2 inv #1) — including the
`glibre-types` ABI hash, every importer's compiled identity, every
downstream tool version, and the canonicalised normalize /
processing parameter sets. Any breaking change to *any* of those
inputs — by definition — alters every shipped asset's `CookKey`
digest, regardless of whether the manifest schema itself moved.

Consequence: a "live migration of manifest entries" — rewriting
recorded `(content_hash, cook_key)` pairs in place to track a new
schema or new tool version — is never the right operation. The
correct operation is always: invalidate the affected entries,
re-run the cook, re-publish. The cook is the authoritative
fallback; the manifest is an optimisation on top of it.

Therefore content's persistent surface is split between (a) the
narrow additive-only manifest schema in §7.1, which never needs
migration bodies inside the MVP horizon, and (b) the opaque CAS
in §7.3, which does not migrate at the field level by
construction.

### 7.3 The CAS payload layer (not manifest-managed)

The cooked artifacts under `cooked/<prefix>/<hash>` — Fory-encoded
`MeshArtifact` / `TextureArtifact` / `FontArtifact` payloads —
are referenced from `ManifestEntry.content_hash` but are
**opaque to the manifest layer**. Schema-level reasoning about
those payloads belongs to their per-artefact §7.x cells (cited in
§5.2): the manifest only sees their 32-byte digest.

Two consequences:

1. **CAS contents are not manifest-managed at field level.** A
   schema bump on `MeshArtifact` does not perturb the manifest
   schema. The bump invalidates the affected `cook_key.digest`s
   (because the Fory schema version participates in
   `glibre-types`'s ABI hash, which feeds
   `cook_key.downstream_versions_hash` — §7.1.2), the next
   `CookSession` re-cooks the affected assets, and the manifest
   is republished with new `(content_hash, cook_key)` pairs.
   The manifest schema, the meta-schema layer, and the data
   spine all stand still.
2. **The CAS itself is append-only and not migrated.** Cooked
   files at `cooked/<prefix>/<hash>` are immutable bytes
   (§4.1.4 inv #3, §4.1.5 inv #2). A schema bump on a payload
   type produces *new* hashes; the prior hashes' files remain on
   disk until post-MVP garbage collection (§4.1.5 inv #2 / §3.2
   collapse #2). There is no in-place rewrite path and no
   migration dispatcher for cooked bytes at the file layer.

### 7.4 What is NOT persisted

To make the boundary explicit (in line with §5 and the §3.3
refusals):

| Artefact          | Why not persisted                                                                                     |
|-------------------|-------------------------------------------------------------------------------------------------------|
| `Residency` state | Process-local; rebuilt on next run from `Manifest` + CAS lookups (§4.1.7 lifetime).                   |
| mmap regions      | Owned by the `ResidencyManager`; never serialised (§4.1.7 inv #5).                                    |
| `LoadRequest`     | Ephemeral I/O-lane unit; never crosses process boundaries.                                            |
| `ScreenCoverage`  | Per-frame hint from `render`; never persisted.                                                        |
| `WatchEdge`       | Subscription state owned by `platform`'s watcher; rebuilt at startup from the `DependencyEdge` graph. |
| `RecookRequest`   | Bounded to a single `CookSession`'s lifetime (§4.1.9 entity lifetime).                                |
| `CookSession`     | Bounded run; identity is timestamp + counter, debug-only (§4.1.9 lifetime).                           |
| `AssetHandle<T>`  | Process-local refcount; never serialised (§4.1.8 lifetime).                                           |
| `CookedAsset`     | Bytes only — addressed by `ContentHash`, opaque to the manifest (§7.3).                               |
| Importer config   | Per-`SourceKind` parameter sets feed `cook_key.normalize_params_hash` as bytes; the structured config is owned by each importer's source tree, not the manifest. |

These appear in the persistence surface only as *identifiers*
(`AssetId`, `ContentHash`, `CookKey.digest`) referenced from §7.1,
never as byte payloads.

## 8. Hot-Reload Contract

This section specialises the engine-wide hot-reload protocol
(`reviews/decisions/hot-reload-protocol.md` — drain → swap → migrate →
resume) to **content**, whose primary hot-reload event is **not** a
plugin `.dylib` swap but a **source-asset file change** that re-cooks
one or more `AssetId`s and atomically replaces the active `Manifest`
snapshot. This path fires far more often in development than the
plugin-code path: every save in a DCC, every paint stroke that lands
through the watcher, every editor "reimport" command. Content's
contract therefore has two layers, both anchored to the engine's
phase-8 frame-boundary barrier:

1. **Manifest-swap (the dominant path)** — a `CookSession` produced
   new `(AssetId → ContentHash)` rows; the loader's barrier publishes
   them via the existing atomic-`rename(2)` protocol (§4.1.6 inv #2);
   `AssetHandle<T>` consumers transparently re-resolve on their next
   `view()` (§4.1.8 inv #4). No vtables move. No middleman ABI hash
   changes.
2. **Plugin-code reload (the rare path)** — the `glibre.content`
   plugin dylib itself swaps under the engine-wide protocol (drain →
   swap → migrate → resume). Engine-wide concerns (per-plugin
   atomicity, observer bus event shapes, error wrapping rules, the
   `enqueue_hot_reload` E2E hook) are not re-stated; see the protocol
   record. Content adds the four pluggable points the protocol leaves
   to each plugin: drain side-effects, survival inventory, migrate
   body, and register-time rehydration.

The §3.2 collapse #3 ("multiple hot-reload pipelines → one re-cook +
atomic handle swap") is realised by §8: the manifest-swap path is the
single mechanism through which textures, meshes, and fonts hot-reload
at the content boundary. Domain semantics of swap (descriptor-heap
updates, PSO swap, shader-permutation invalidation, logic-graph state
preservation) are routed to the owning context (§3.3) and are not
content's concern; content's concern ends at "the active manifest now
maps `AssetId` to a new `ContentHash`, and `view()` returns the new
bytes."

### 8.1 Reload point — phase 8, never mid-frame

The engine schedule (`reviews/decisions/frame-phases.md`) places the
hot-reload barrier at phase 8, between `render-submit` (phase 7) and
`present` (phase 9). Both content paths are anchored to that one
slot and refuse any other.

At phase 8 entry, content's in-flight state is:

1. **No `AssetHandle::view()` is being dereferenced.** All ECS
   systems that consume cooked bytes finished writing their per-frame
   extracts in phases 1–6; phase 7 already returned. Phase 8 sees no
   live `view()` call and no `LoadRequest` mid-flight on the synchronous
   path (§4.2 invariant 8 anchors content's I/O lane to background
   threads off the frame loop; the manifest pointer is read on-frame
   but only at extract time, never under the loader's exclusive
   ownership).
2. **The active `Manifest` snapshot pointer is stable.** The
   in-RAM swap of the manifest pointer (§4.1.6 inv #2) is itself the
   atomic publish — a single relaxed store under the loader's
   exclusive phase-8 ownership — and is invisible to phases 1–7 of
   frame N because they have already completed.
3. **No `CookSession` mutates the active snapshot mid-frame.**
   `CookSession::commit()` runs entirely off the frame loop on a
   background thread (§4.2 inv #8); it stages results into a worker-
   private table and parks them on a queue. The queue is drained
   exactly at phase 8 entry by the loader, which then executes the
   manifest-swap path described in §8.3.1. A `CookSession` whose
   `commit()` returns at any other phase does **not** publish — its
   `CookOutcome` value is captured, but the manifest pointer flip is
   deferred to the next phase 8.
4. **`ResidencyManager` ref-count tables are stable.** I/O-lane
   operations on the residency state machine (§4.1.7 inv #2) are
   suspended between phase 7 exit and phase 9 entry by the engine's
   exclusive-ownership rule; ref-count adjustments queued during
   phases 1–7 have all been applied. (Ref-count *reads* during phase
   8 are safe; the loader is single-threaded inside the barrier.)

These four conditions are content's half of the protocol's "drain"
postcondition (protocol §"Step 1 — Drain"). The
`glibre_plugin_drain` body therefore has nothing to flush on the
runtime side — its only work is the worker-pool teardown described
in §8.3.

**Mid-frame reload is refused.** Any `CookSession::commit()` that
completes during phases 1–7 enqueues its staged results; the
manifest pointer flip is consumed only at phase 8 entry. Inside
phase 8, content does not yield to the I/O lane or to recook
workers — the loader holds exclusive ownership for the duration of
drain → swap → migrate → resume per protocol §"Decision". A
runtime caller that observes a partially-swapped manifest pointer
is treated as a contract violation by the loader, not a refusal —
content's spec contributes no new refusal arm here, but states the
invariant explicitly so consumers cannot expect mid-frame swap
semantics.

### 8.2 Manifest-swap path (the dominant hot-reload)

The vast majority of content hot-reloads are **manifest-only** — no
plugin code moves, no ABI hash changes, no migrate functions run.
The trigger chain is mechanical:

1. **`platform` watcher fires.** A `FileEvent` for a path under
   `assets/source/` is delivered through the `WatchEdge` subscription
   (§4.1.10 composition).
2. **Content translates to recook requests.** The `WatchEdge`
   translator reads the active manifest's in-RAM `DependencyEdge`
   graph and produces a `Set<RecookRequest>` whose closure is
   bounded by the transitive dependents of the changed source
   (§4.1.10 inv #4). The set may include multiple `AssetId`s when a
   leaf source has fan-out (e.g. a normal-map source feeding two
   materials); each `RecookRequest` carries a `SourceChanged` /
   `DependentRecook` reason discriminant for telemetry.
3. **`CookSession` runs the requests.** A session is begun
   (`CookSession::begin`), the requests are enqueued
   (`CookSession::enqueue`), and `commit()` runs the cooks in
   topological order (children before parents, §4.1.9 inv #3). Each
   cook step:
   - computes the `CookKey` digest (§4.1.3) from the new source
     bytes plus the unchanged importer / params / downstream tool
     versions;
   - on cache hit (digest matches the manifest's recorded
     `cook_key.digest` and the corresponding `ContentHash` is
     present in the CAS), skips the cook entirely (§4.1.3 inv #3);
   - on cache miss, dispatches the importer (§4.1.2), produces a
     new `CookedAsset`, computes its `ContentHash`
     (`BLAKE3(payload)`, §4.1.4 inv #1), writes the bytes to the
     CAS via the temp-write + `rename(2)` protocol (§4.1.5 inv #3),
     and stages a new `(AssetId, CookKey, ContentHash)` triple.
4. **Atomic manifest publish at phase 8.** When `commit()` returns
   `CookOutcome::Published`, the staged manifest blob has been
   written to a temp path and fsync'd, but the canonical
   `cooked/_manifest.fory` rename is **deferred** to the next phase
   8 (§4.2 inv #8: "the only on-frame artifact is the in-RAM
   manifest swap"). At phase 8 entry the loader executes:
   - `rename(2)` of the staged manifest blob onto the canonical
     path — atomic at the filesystem layer (§4.1.6 inv #2);
   - relaxed-store of the in-RAM active-manifest pointer to the new
     snapshot — atomic at the runtime layer (§4.2 inv #5);
   - emission of one `AssetReloaded` event per affected `AssetId`
     (§8.5).
5. **`AssetHandle<T>` consumers re-resolve transparently.** On the
   next `view()` after phase 8 exit, each handle's `asset_id`
   resolves through the new manifest snapshot to the new
   `ContentHash`. Because handle identity is `(asset_id, slot
   generation, content_hash_at_acquire)` (§4.1.8 lifetime), the
   handle observes the change as a generation bump and a fresh byte
   span; consumers that have cached a `view()` span from a prior
   frame must re-call `view()` (§4.1.8 inv #4).

This path is **the** content hot-reload. Steps 1–4 produce no
changes to plugin code, vtables, or middleman ABI hashes; they are
not a "reload" in the protocol's drain → swap → migrate → resume
sense. They are a **manifest pointer flip** under the engine's
existing phase-8 ownership, sharing the loader's barrier but not
the protocol's machinery.

### 8.3 Survival across the manifest swap

The engine-wide survival rule is mechanical: **state with a
`.fory` schema in `glibre-types.dylib` survives across the swap;
state without one does not** (protocol §"State Survival Rules";
PHILOSOPHY collapse: one check, not a per-aggregate manifest).
Content owns three persistent fory-schema'd types
(§7.1.1 `ManifestEntry`, §7.1.2 `CookKey`, §7.1.3 `DependencyEdge`)
and a collection of host-side runtime state. The table classifies
every content aggregate against that rule and adds the
content-specific reasoning for each survival decision.

| Content-owned state                                                          | Persistence path                | Survives swap? | Reasoning                                                                                                                                                                                                                                                                                                                                  |
|------------------------------------------------------------------------------|---------------------------------|----------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `ManifestEntry` rows (§7.1.1)                                                | `.fory` schema, middleman       | Yes — the active manifest snapshot **is the swap**. Old rows for re-cooked `AssetId`s are replaced by new rows in the same atomic publish; rows for unchanged `AssetId`s are byte-equal in the new snapshot (deterministic encoding, §4.1.6 inv #6 / §7.1.1 round-trip golden). Per-row migration runs only when the `ManifestEntry` schema itself bumped (additive-only, §7.2.1) — the manifest-swap path does not invoke migrate functions. |
| `CookKey` records (§7.1.2)                                                   | `.fory` schema, middleman       | Yes — embedded inside `ManifestEntry`. A re-cooked entry carries a freshly-derived `cook_key`; an unchanged entry retains the prior key byte-equal. Schema bumps on `CookKey` itself force full re-cook (§7.2.2) — never a live migration of recorded keys. |
| `DependencyEdge` lists (§7.1.3)                                              | `.fory` schema, middleman       | Yes — embedded inside `ManifestEntry.edges`. The new manifest snapshot's edges reflect the post-cook dependency graph; cycle-freedom is asserted at publish (§4.1.6 inv #4). Schema bumps on `DependencyEdge` are additive on `child_kind` (§7.2.3) and require no migration body. |
| **CAS files at `cooked/<prefix>/<hash>` (§4.1.5)**                           | Append-only filesystem          | **Yes — never rewritten in place.** Old `ContentHash` files remain on disk after the manifest pointer moves to a new hash; they are addressed by their digest, not by `AssetId`, so re-pointing the manifest does not orphan their bytes for handles that captured them. **Garbage collection of orphaned hashes is post-MVP** (§4.1.5 inv #2); the swap leaves orphaned bytes on disk to be reclaimed later. |
| **Active `Manifest` pointer (§4.1.6)**                                       | In-RAM only; on-disk `cooked/_manifest.fory` is the persistent backing | Yes — pointer flip is **the** swap. The prior snapshot remains live for any thread that captured it before the flip (§4.1.6 inv #2: "Concurrent runtime queries see exactly one of the two snapshots"). Outstanding handles whose prior `view()` returned bytes from the prior snapshot keep those bytes valid until they re-resolve. |
| **`AssetHandle<T>` instances (§4.1.8)**                                      | None (in-process)               | **Yes — handle identity survives**. A handle's `asset_id` is stable across cook revisions (§4.1.8 composition); the handle's `slot.generation` increments on each manifest swap that re-pointed its `AssetId`, so the next `view()` re-resolves through the new snapshot to a fresh `(slot, content_hash)` tuple. **Generation tagging makes use-after-swap a `ResidencyError::ManifestStale`, never UB** (§4.1.8 inv #4). |
| `ResidencyManager` ref-count table (§4.1.7)                                  | None (in-process)               | Yes. The table is keyed by `ContentHash`; when the manifest re-points an `AssetId` from `H_old` to `H_new`, both slots can coexist — the prior slot is held until its outstanding handle ref-count reaches zero, then naturally drains (§4.1.7 inv #6). The new slot is admitted via the standard `LoadRequest` path on the next handle access. The swap **adjusts ref-counts only by the rules of slot transitions**, not by any out-of-band mutation. |
| `ResidencyManager` mmap regions (§4.1.7 inv #5)                              | None (in-process)               | Yes for held mappings (refcount > 0); released for evicted mappings via the standard `Resident → Evicting → Unloaded` graph. Mapping bytes themselves are owned by the kernel; the manager only holds the descriptor (§4.1.7 composition). |
| `MemoryBudget` ceiling (§4.1.7)                                              | Configured (process arg)        | Yes — set once at content init from `perf-budget.md`; not perturbed by any swap. A swap that would temporarily double-pin (old hash held by extant handles + new hash being admitted) MUST still respect the ceiling: progressive eviction runs first, deferring the new admission per §4.1.7 inv #1 if no slot can free room. |
| `WatchEdge` subscriptions (§4.1.10)                                          | None (rebuilt at startup)       | No — owned by `platform`'s watcher (§3.3). Content's translator state (the in-RAM mapping from path → `Set<AssetId>`) is rebuilt from the `DependencyEdge` graph on startup and is otherwise stable across swaps; a manifest swap that changes the dependency graph updates the translator's in-RAM cache as part of the publish protocol. |
| In-flight `RecookRequest` queues / `CookSession` worker pools                | None                            | No — destroyed by content's `glibre_plugin_drain`, re-spawned by the new plugin's `glibre_plugin_register` (plugin-code reload only; the manifest-swap path does not touch them). |
| `SourceAsset` debounced cache, `Importer` per-cook arenas                    | None                            | No — stateless across cook sessions (§4.1.1, §4.1.2 lifetime). No survival concern. |

The rule mechanically applied: every row marked "Yes" has either a
`.fory` schema or is owned by `core` / `platform` / `data` /
`glibre-types`; every "No" row is private to content with no
on-disk format and no migration contract — exactly what
PHILOSOPHY §3 + protocol §"State Survival Rules" require.

The dominant manifest-swap path touches only the first six rows;
the plugin-code reload path additionally tears down and re-spawns
the last two row groups.

### 8.4 `migrate(...)` body — content's responsibilities

Two `migrate(...)` surfaces exist; both follow the protocol's pure
migrate signature (`hot-reload-protocol.md` §"Migrate Function
Contract").

**Manifest-row re-resolution (manifest-swap path).** This is **not**
a protocol-level migrate function — no schema version moves and no
ABI hash changes. It is content's per-row republish action, executed
inside the loader's phase-8 ownership immediately after the manifest
pointer flip:

1. For each `AssetId` whose new `ManifestEntry.content_hash` differs
   from the prior snapshot's `content_hash`, content walks the
   `ResidencyManager` table and, if the `AssetId` has any
   outstanding `AssetHandle` references:
   - Bumps the handle's `slot.generation` so the next `view()`
     re-resolves (§4.1.8 inv #4); the prior slot is **not**
     evicted — it is held until its ref-count drains naturally
     (§4.1.7 inv #6).
   - Issues a `LoadRequest` for the new `ContentHash` if it is
     not already `Resident`. The request flows through the standard
     priority queue (§4.1.7 inv #4); the manifest swap does **not**
     bypass the eviction policy or the `MemoryBudget` ceiling.
2. For each `AssetId` whose `ContentHash` is unchanged in the new
   snapshot (cache-hit cooks, untouched siblings of a leaf change),
   no work is required — the old slot, the old generation, and the
   old `view()` bytes remain valid.
3. Content emits one `AssetReloaded` event per affected
   `AssetId` (those whose `ContentHash` actually changed) carrying
   `{asset_id, old_content_hash, new_content_hash}` on the engine's
   observer bus (§8.5). Consumers (render, geometry, audio, editor)
   subscribe and react synchronously inside phase 8 per the
   protocol's atomicity rule.

**Persistent-type schema migration (plugin-code reload path).** The
protocol's step 3 (`migrate`) runs pure per-row functions for every
persistent-component-type schema bump on the engine's behalf.
Content owns three of those bodies, all governed by §7.2:

- `ManifestEntry` migrations (§7.2.1) — additive-only; codegen
  synthesises defaults at deserialise time per data SPEC §7.4 rule
  #6. **No body** is required at MVP horizon. Schema bumps that
  append a defaulted field (e.g. `last_published_at_unix_ms : u64
  default 0` at tag 5) flow through the engine's standard
  additive-defaulted-field migration with no per-row code.
- `CookKey` migrations (§7.2.2) — **forbidden by construction**.
  `CookKey.fory` ships with no `migration` clause; the codegen tool
  refuses to emit a dispatcher (data SPEC §7.4 rule #5 / §7.6
  parallel pattern). A schema bump on `CookKey` is therefore a full
  re-cook on next workspace open: the manifest is treated as cold
  cache and rebuilt by the next `CookSession`. **No live migration
  of `CookKey` records is permitted**, ever. The CAS contents survive
  the re-cook because they are addressed by `ContentHash`, not by
  `CookKey` (§4.1.5 inv #2).
- `DependencyEdge` migrations (§7.2.3) — additive variants on
  `child_kind` only; codegen treats this as an identity mapping.
  **No body** is required.

The plugin-code reload path therefore runs **zero** content-authored
migrate functions across MVP. Any future schema bump that would
require a body is rejected at review under §7.2.4 — the correct
operation is always invalidate-and-recook, never live-rewrite. The
contract is testable: `tests/data/schemas/content/<Type>.cpp` round-
trip goldens (§7.1.1, §7.1.2, §7.1.3 round-trip contracts) cover
every shipped schema version.

What content's `glibre_plugin_register` (resume step) **does** do
on a plugin-code reload:

1. **Re-acquires the active `Manifest` snapshot pointer** from the
   middleman registry. The pointer survives the swap (§8.3 row 5);
   the new plugin reads it and seeds its own in-RAM dispatch tables
   from the snapshot's contents.
2. **Re-spawns the cook worker pool** sized from `perf-budget.md`'s
   content-context cell. The pool's previous lifetime ended in
   `glibre_plugin_drain`.
3. **Re-registers the `WatchEdge` subscriptions** with `platform`'s
   watcher by walking the active manifest's `DependencyEdge` graph
   (§4.1.10 inv #1). Subscriptions are idempotent; re-subscribing
   to a path already watched is a no-op at the platform layer.
4. **Re-binds the `ResidencyManager` I/O lane** to the engine's
   background-thread scheduler. Outstanding `Resident` slots and
   their mmap regions survive (§8.3) and are re-attached to the
   resumed I/O lane without re-mapping.
5. **Does not rebuild `Manifest` snapshots, does not re-cook, does
   not invalidate the CAS.** All persistent state survives by
   construction; the resume step is bounded by O(active subscriptions
   + worker pool size), which is sub-millisecond in practice.

The total work in content's resume step is therefore bounded by
**O(subscriptions) re-registrations + O(workers) thread spawns +
zero CAS or manifest churn**, fitting the protocol's "reload path
bounded by drain + swap + Σ migrate + register" budget
(`hot-reload-protocol.md` §Consequences).

### 8.5 Refusal cases (content-specific)

Content contributes no new umbrella refusal arm; every refusal is
expressed as the engine-wide `core::Error::HotReloadRefused` with a
nested cause chosen from the protocol's existing arms, **or** as a
local `glibre::content::Error` returned from the manifest-swap path
which is not a protocol-level refusal but a publish failure (the
prior manifest snapshot remains active, identical to the protocol's
"prior plugin remains live" semantics).

Three classes of refusal exist; each maps to a documented inner
cause and a specified operator action.

**Class A — Cooker version mismatch (manifest-swap path, refused as
publish failure).** Detected during `CookSession::commit()` when the
`CookKey.digest` for any input changes due to a non-source ingredient
moving (importer rebuild, `glibre-foryc` bump, downstream tool
version change, `glibre-types` ABI hash bump per §3.2 collapse #2 /
§7.1.2 component 6). This is **not a refusal** — it is the **expected
re-cook trigger** (§7.2.2 / §7.2.4). Every recorded `cook_key.digest`
mismatches the freshly-computed key for its asset; the session
re-cooks every dependent and publishes a fresh manifest. The
engine-wide protocol is not invoked. Operator action: none — the
re-cook is automatic and may take longer than usual on the first
session after the version bump.

**Class B — Importer / cook step failure (manifest-swap path,
refused as publish failure).** Detected during `CookSession::commit()`
on any of the §4.1.2 / §4.1.4 / §4.1.6 invariants. The session
returns `CookOutcome::RolledBack` and the prior manifest snapshot
remains active (§4.1.9 inv #1). The four cases:

| Refusal cause                                                                          | Detected by                                                                                                       | Returned error                                              | Operator action                                                                                  |
|----------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------|--------------------------------------------------------------------------------------------------|
| Importer cannot read source (path escape, missing file)                                | `Importer::ingest()` per §4.1.2 inv #1, lifted from `SourceAsset` constructor (§4.1.1 inv #1).                    | `ImporterError::SourceNotFound`                             | Restore the source file to a path under `assets/source/`; re-trigger the watch event.            |
| Importer SDK rejects the source (magic mismatch, unsupported version, malformed)       | Importer's `-fexceptions` carve-out (§4.1.2 inv #2); SDK exception is translated at first ingress.                | `ImporterError::MagicMismatch` / `UnsupportedVersion` / `MalformedPayload` | Re-export the source from the DCC at a supported version; verify the file is not corrupt.       |
| Cook output fails Fory schema validation (wrong artifact class FQN, version not in registry) | `CookSession::commit()` validates each staged `CookedAsset.payload` against `data`'s `glibre-types` registry per §4.1.4 inv #2. | `ImporterError::MalformedPayload`                           | This is a producer-side defect — file a bug against the cook step's owning context; the cook is rejected and the prior manifest remains. |
| Dependency cycle detected at publish                                                   | Manifest publish protocol per §4.1.6 inv #4 / §4.1.10 inv #4.                                                     | `ImporterError::MalformedPayload`                           | Resolve the cycle in the source tree (e.g. break a circular material reference); re-trigger the cook. |

In all four Class-B cases, **the prior `CookedAsset` and its
`ContentHash` remain referenced** by the active manifest. The
in-flight session's CAS writes (if any) succeeded onto the file
system but are **never published** — the manifest pointer never
moves. Orphaned CAS bytes await post-MVP garbage collection
(§4.1.5 inv #2). The diagnostic surface logs each refusal once at
`warn` level (mirroring the engine protocol's refusal logging
discipline) with structured fields `asset_id`, `source_path`,
`importer_kind`, `cause`.

**Class C — Plugin-code reload refusal.** Detected during the
engine-wide protocol's steps 2–4 when the `glibre.content` dylib
itself is being swapped. Three cases roll up to the protocol's three
named arms:

| Plugin-code refusal cause                                       | Detected at protocol step               | Inner-error arm                                | What the operator must do                                                                                  |
|-----------------------------------------------------------------|-----------------------------------------|------------------------------------------------|------------------------------------------------------------------------------------------------------------|
| New plugin's `glibre_types_abi_hash()` ≠ host                   | Step 2.1 (engine-wide ABI gate)          | `core::Error::PluginAbiHashMismatch`           | Rebuild the plugin against the current `glibre-types.dylib`. PHILOSOPHY §9 case.                           |
| New plugin's manifest declares a `(fqn, schema_version)` set that drops a content-owned persistent type the prior plugin registered | Step 2.2 (engine-wide manifest gate)     | `core::Error::HotReloadRefused` with cause `core::Error::SchemaMigrationFailed` | Restore the dropped type or accept a fresh workspace (post-MVP); content's manifest layer is additive-only (§7.2.1) so this case is unexpected. |
| New plugin's `glibre_plugin_register` returns an error (worker-pool spin-up failure, watcher re-registration failure)                  | Step 4.1 (engine-wide register gate)     | `core::Error::PluginInitFailed` carrying `content::Error`   | Inspect the plugin's structured log; resolve the inner cause; re-attempt. The prior plugin remains live. |

Each refusal is logged exactly once at `warn` level (protocol
§"Refusal Cases") with the structured fields `plugin_fqn=glibre.content`,
`attempted_dylib_path`, `host_abi_hash`, `plugin_abi_hash`, and the
inner cause's enumerator name.

### 8.6 Observer notification — `AssetReloaded`

`AssetReloaded` is content's single hot-reload event type, emitted
**per affected `AssetId`** during the manifest-swap path (§8.4). It
is the seam through which sibling contexts (render, geometry,
audio, editor) react to a content change without polling the
manifest themselves. Its shape:

```cpp
namespace glibre::content {

struct AssetReloaded {
    AssetId     asset_id;          // §2 — `glibre.<ctx>.<slug>`
    ContentHash old_content_hash;  // 32-byte BLAKE3, the prior bytes
    ContentHash new_content_hash;  // 32-byte BLAKE3, the new bytes
    // No SchemaVersion field — manifest swap never moves schema versions
    // (§8.4 "manifest-row re-resolution"). Schema bumps go through the
    // plugin-code reload path and surface as the engine's standard
    // `HotReloadCompleted { migrated_types }` instead.
};

}
```

The event is itself a middleman type
(`glibre.types.content.AssetReloaded`, layout owned by data SPEC
§7), so its byte layout survives any plugin-code reload (engine
protocol §"Observer Notification" mirror).

**Emission contract:**

1. **One event per `AssetId` whose `ContentHash` actually changed
   in the published manifest.** Cache-hit cooks (digest match,
   `ContentHash` unchanged) emit no event — the bytes are
   byte-equal to what consumers already see.
2. **Synchronous emission inside phase 8.** Subscribers are called
   on the loader thread, after the manifest pointer flip and
   before phase 9 begins (mirrors engine protocol §"Observer
   Notification" atomicity). Subscribers see a fully-swapped
   manifest snapshot; they never observe a half-swapped state.
3. **Subscriber failure is non-fatal.** A subscriber that returns
   an error is logged at `warn` level with `subscriber_fqn`,
   `asset_id`, `cause`; the manifest swap proceeds and other
   subscribers are still notified. (Rationale: the manifest
   pointer is already flipped; refusing the swap retroactively is
   not coherent. The dev-time editor / e2e harness path is
   tolerant of subscriber bugs by design.)
4. **No `AssetReloaded` is emitted on the plugin-code reload path.**
   That path uses the engine's `HotReloadCompleted { migrated_types }`
   event (protocol §"Observer Notification") which already carries
   the schema-bump information. Emitting `AssetReloaded` for every
   active `AssetId` on a plugin-code reload would be redundant —
   the manifest pointer **survives** unchanged through the
   plugin-code swap (§8.3 row 5), so no `ContentHash` mapping
   moves.

Consumer responsibilities (each owning context's hot-reload §8 cell
spells these out concretely; this is the cross-reference):

- **`render`** (specs/render/SPEC.md §8.5): treats `AssetReloaded`
  on a `glibre.<ctx>.<slug>.texture` `AssetId` as a descriptor-heap
  rebind trigger; the new `ContentHash` resolves to new pixel bytes
  on the next `RenderFrame` extract.
- **`geometry`**: re-issues meshlet / BLAS imports for affected
  meshes; existing GPU resources are torn down on the next phase 6.
- **`audio`** (post-MVP): re-decodes the affected `AssetId` into
  the mixer's sample bank.
- **`editor`** (post-MVP): refreshes its asset-browser thumbnail
  and any open inspector for the affected `AssetId`.

The event types added by content — exactly one
(`AssetReloaded`) — piggyback on the engine bus. No second event is
permitted; new observability needs flow into existing arms or
graduate to a SPEC bump.

### 8.7 Test hooks — file-touch + manual recook + replay assertion

Content's hot-reload contract is verified end-to-end by three
fixture layers under `tests/content/hot_reload/`. All fixtures use
the loader's existing `enqueue_hot_reload` E2E entry point (protocol
§"Test Hooks") only for the plugin-code reload path; the manifest-
swap path uses **content-private E2E hooks** described below, which
trigger the same in-process state machine without touching the
filesystem watcher.

**Content-private E2E hooks (`#if defined(GLIBRE_E2E)`):**

```cpp
namespace glibre::content::test {

// Drops a synthesized FileEvent into the WatchEdge translator
// without touching the filesystem. The event is treated as if it
// came from `platform`'s watcher; the next CookSession picks it up.
RecookRequestId enqueue_synthetic_file_event(
    std::filesystem::path source_path,
    std::span<const std::byte> new_source_bytes
) noexcept;

// Forces a CookSession to commit at the next phase 8 entry.
// Used to deterministically interleave a recook with a frame.
[[nodiscard]] auto force_commit_at_next_phase8(
    CookSessionId session
) noexcept -> Result<CookOutcome>;

// Blocks the calling thread until the referenced recook has
// reached terminal state (Published / RolledBack / Cancelled).
// Used by E2E goldens; never linked into runtime.
[[nodiscard]] auto await_recook(RecookRequestId) noexcept
    -> Result<CookReport>;

}
```

**Three test scenarios:**

1. **File-touch happy path** (`Hot-reload re-cooks on source change`).
   Fixture writes a synthetic `.png` source under
   `tests/data/content/source/`, opens a workspace, and runs the
   engine for `K = 8` frames. At frame `K/2` the harness calls
   `enqueue_synthetic_file_event` with new bytes, then
   `force_commit_at_next_phase8`. Assertions:
   - `CookSession::commit()` returns `CookOutcome::Published`;
   - the active `Manifest` snapshot at frame `K/2 + 1` resolves
     the affected `AssetId` to the new `ContentHash`;
   - exactly one `AssetReloaded` event was emitted with
     `{old_content_hash, new_content_hash}` matching the cook
     output;
   - the `AssetHandle<Texture>` held by the harness's render-stub
     observes the new bytes on its next `view()` (the prior
     `view()` span is still valid because the prior slot is held
     until ref-count drains, §4.1.7 inv #6);
   - the `ResidencyManager` ref-count for the prior `ContentHash`
     is exactly 1 immediately after the swap (the harness's still-
     captured prior `view()`), and drops to 0 once the harness
     drops its captured span;
   - the prior CAS file at `cooked/<prefix>/<old_hash>` remains
     on disk (no in-place rewrite, no GC at MVP).

2. **Manual recook + replay assertion** (`Hot-reload preserves
   manifest determinism`). Fixture cooks a deterministic source
   asset, captures the resulting manifest blob bytes (after
   `rename(2)`) as a golden, then replays the same cook from a
   fresh workspace and asserts the manifest blob is byte-equal to
   the golden. This exercises §4.1.4 inv #1 (`(source_bytes,
   cooker_version) → content_hash` is total) end-to-end through
   the publish protocol; if the manifest layer or any importer
   leaks non-determinism, the golden mismatch surfaces immediately.

3. **Cooker version mismatch → re-cook all dependents** (`Hot-reload
   re-cooks on cooker version bump`). Fixture cooks a source under
   importer-version-A, persists the manifest, then re-opens the
   workspace under importer-version-B (simulated by bumping
   `importer_version` in the fixture importer's compiled identity).
   Assertions:
   - the next `CookSession` re-cooks **every** asset whose
     `cook_key.digest` mismatched the freshly-computed key (the
     entire fixture's dependency graph in this scenario);
   - the published manifest's `cook_key` records reflect
     importer-version-B for every entry;
   - one `AssetReloaded` event fires per re-cooked `AssetId` whose
     resulting `ContentHash` actually changed (cache-hit cooks
     under the new key, where the new bytes happen to round-trip
     byte-equal, emit no event);
   - the prior CAS files survive (no GC).

A fourth fixture exercises **importer failure**:
(`Hot-reload refuses on malformed source`). The harness writes a
malformed `.fbx` source byte sequence; `enqueue_synthetic_file_event`
fires; the next `CookSession::commit()` returns
`CookOutcome::RolledBack` carrying `ImporterError::MalformedPayload`;
the active manifest is unchanged; no `AssetReloaded` is emitted; the
prior `CookedAsset` and its `ContentHash` remain referenced by the
active manifest (§8.5 Class B). The diagnostic log records exactly
one `warn`-level entry with the structured refusal fields.

A fifth fixture (`Hot-reload accepts plugin-code swap`) drives the
engine-wide protocol path: `enqueue_hot_reload("glibre.content",
tests/e2e/plugins/content-v2.dylib)` with v2 byte-identical to v1
(modulo build timestamp). Assertions:
- `HotReloadCompleted { migrated_types: [] }` fires exactly once
  (no schema change);
- the active `Manifest` pointer survives unchanged through the
  swap (§8.3 row 5);
- no `AssetReloaded` events are emitted (manifest mappings
  unchanged, §8.6 emission contract item 4);
- the worker pool's thread count after the swap matches the
  pre-swap count (re-spawned by `glibre_plugin_register`).

All five scenarios run inside a single CI job using the in-process
trigger; no real filesystem watcher is involved (mirrors protocol
§"Test Hooks"). The Catch2 case names listed above are the exact
acceptance criteria entries in §11.

### 8.8 Cross-references

- Engine protocol: `reviews/decisions/hot-reload-protocol.md`
  (drain → swap → migrate → resume; refusal arms; observer bus;
  E2E hook).
- Frame slot: `reviews/decisions/frame-phases.md` (phase 8 entry /
  exit guarantees; `CookSession::commit()` parking off-frame).
- Persistence rules invoked: §7.1.1 / §7.2.1 (`ManifestEntry`
  additive-only), §7.1.2 / §7.2.2 (`CookKey` schema-bump = full
  re-cook), §7.1.3 / §7.2.3 (`DependencyEdge` additive variants),
  §7.2.4 (no live migration of manifest entries — re-cook is the
  authoritative fallback).
- Aggregates touched: §4.1.1 `SourceAsset` (file-touch trigger),
  §4.1.4 `CookedAsset` (immutable byte payload), §4.1.5 `CAS`
  (append-only, never rewritten), §4.1.6 `Manifest` (atomic
  pointer flip = the swap), §4.1.7 `ResidencyManager` (ref-count
  drain across swap), §4.1.8 `AssetHandle<T>` (transparent
  re-resolve via generation tag), §4.1.9 `CookSession` (commit
  parked until phase 8), §4.1.10 `WatchEdge` / `RecookRequest`
  (pure file-event → recook fan-out), §4.2 invariant 5
  (hot-reload = re-cook + atomic Manifest swap).
- Errors used: `ImporterError::SourceNotFound`,
  `ImporterError::MagicMismatch`, `ImporterError::UnsupportedVersion`,
  `ImporterError::MalformedPayload`, `ImporterError::Cancelled`,
  `ResidencyError::ManifestStale` (§5.3), each surfacing through
  `CookOutcome::RolledBack` for the manifest-swap path or wrapped
  by `core::Error::PluginInitFailed` for the plugin-code reload
  path per protocol §"Refusal Cases".
- Sibling §8 cells that consume `AssetReloaded`: render §8.5
  (descriptor-heap rebind), geometry §8 (BLAS re-import), audio
  §8 (post-MVP — sample-bank re-decode), editor §8 (post-MVP —
  inspector refresh).

## 9. Performance Budget

### 9.1 Engine-Wide Allocation (Citation)

`reviews/decisions/perf-budget.md` Per-Context Budget Table assigns
`content` the cell **0.20 ms CPU sim + 0.00 ms CPU submit + n/a GPU
+ 256 MiB heap**, with phase ownership "residency / streaming;
one-shot import work is off-thread". The 16.67 ms / 60 fps wall-clock
target is met with ≥1.5 ms reserved headroom (`perf-budget.md`
§"Decision"); content's row is part of the **8.05 ms CPU steady-state**
that constitutes the sim-plus-submit half of every frame on the
game-loop driver thread. This SPEC §9 is the per-aggregate refinement
of that cell — it MUST sum into the cited row and MUST NOT silently
expand it. Any cell-level amendment requires a perf-budget amendment
spike per `perf-budget.md` §"Consequences".

The issue brief for spike #146 sets the per-aggregate sub-budgets
against an MVP-refined CPU ceiling of **0.50 ms** that absorbs the
0.20 ms `perf-budget.md` `content` sim slot plus 0.30 ms drawn from
the engine 1.5 ms sim-half headroom row. The decision-record cell
itself is unchanged at 0.20 ms in the engine-wide table; the 0.50 ms
ceiling here is the **gate threshold** that includes one-shot
variance and the import-handoff S3 path so the gate does not flap on
runs where the import worker signals completion on the same frame as
a manifest swap. The headroom draw is documented here per
`perf-budget.md` §"CI Gate Spec" rule 5 (headroom regression alarm)
and is the only non-trivial deviation §9 takes from the engine table.

### 9.2 Cell Summary

The content context's full cell, per the §9.1 refinement:

| Axis              | Budget       | Source                                      |
|-------------------|--------------|---------------------------------------------|
| CPU (sim)         | **0.50 ms**  | §9.1 refinement (= 0.20 ms `content` row + 0.30 ms drawn from sim-half headroom for import-handoff variance) |
| CPU (submit)      | 0.00 ms      | `perf-budget.md` Per-Context Budget Table (no submit-half work)     |
| CPU (combined)    | **0.50 ms**  | sum, used as the §9.3 sub-budget ceiling    |
| GPU               | n/a          | content owns no rendering work; GPU residency for cooked artifacts is render-tagged per `perf-budget.md` Allocator Rule 5 |
| Heap ceiling      | **256 MiB**  | `perf-budget.md` Per-Context Budget Table   |
| Phase ownership   | (none)       | content registers no phase-owned systems; participates as systems-in-phase |
| Phase systems     | 1, 9         | residency tickle + handle resolve in phase 1; transient-arena drain in phase 9 |

The cell is sized against **(S1)** = 1 character + 200 props + 8
dynamic lights at 1920x1080 on M1 8-core GPU baseline, plus the
**(S3)** asset-import scenario (one ~5 MiB FBX drag-drop while S1
plays) exercising the off-thread import path. Content owns no
per-frame phase (`perf-budget.md` §"Pipelined Frame Timing") — its
work is systems-in-phase: residency state evaluation and
`AssetHandle` resolution in phase 1 (input/scan), transient drain in
phase 9 (present). Bulk import / cook is **off-thread** (the import
worker pool); the 0.50 ms cell pays only for the runtime handoff
seam (CAS lookup, residency-table update on worker completion) per
`perf-budget.md` §"Justification Per Cell" (`content` row).

The combined-CPU 0.50 ms ceiling is the figure the §9.3 per-aggregate
rows sum into. The sim-only nature of the cell means every aggregate
in §9.3 is annotated `sim`; no submit-half work exists for content
in shipping builds.

### 9.3 Per-Aggregate Sub-Budgets

Sub-budgets refine the §9.2 cell across the ten content-owned
aggregates from §4.1. Each row lists CPU ms (sim half), heap
allocation under the 256 MiB ceiling, and the dominant operation that
the budget pays for. The CI gate (§9.5) attaches one
`BENCHMARK_CELL(...)` block per aggregate that asserts steady-state
CPU time is `<= cpu_ms` under the S1 fixture defined in
`perf-budget.md` §"Justification Per Cell". Hot-path activity is
limited to three operations: residency-state evaluation, hot-reload
manifest check, and `AssetHandle` resolution — every other aggregate
is dormant on the per-frame critical path.

| Aggregate (§4.1 ref)                | CPU ms  | Half | Heap     | Dominant operation                                                                |
|-------------------------------------|---------|------|----------|-----------------------------------------------------------------------------------|
| `SourceAsset` (§4.1.1)              | 0       | sim  | 0 MiB    | cook-only value object; never resident at runtime                                 |
| `Importer` (§4.1.2)                 | 0       | sim  | 0 MiB    | cook-only entity; lives off-thread in importer worker arena (counted §9.3.1)      |
| `CookKey` (§4.1.3)                  | 0       | sim  | 0 MiB    | cook-only value; ephemeral inside `CookSession` job table                         |
| `CookedAsset` (§4.1.4)              | 0       | sim  | 0 MiB    | mmap-only at runtime; bytes counted as CAS hot-set, not heap (Allocator note §9.4)|
| `CAS` (§4.1.5)                      | 0       | sim  | **160 MiB**| mmap-based; CAS hot-set residency. **mmap entries do not count against heap** per §9.4 rule 5; the 160 MiB is the resident-page ceiling tracked separately |
| `Manifest` (§4.1.6)                 | ~0.05   | sim  | 16 MiB   | O(1) hash lookup `AssetId → ContentHash` on the handle-resolve path               |
| `ResidencyManager` (§4.1.7)         | ~0.20   | sim  | 16 MiB   | residency-state evaluation: LRU + screen-coverage priority sweep over working set |
| `AssetHandle` table (§4.1.8)        | ~0.05   | sim  | 0 MiB    | O(1) handle resolution; the slot table itself is core-owned (`core` SPEC §9.3 row `AssetHandle table` / 8 MiB) — content's column counts only the per-handle resolve work, not the table storage |
| `CookSession` (§4.1.9)              | 0       | sim  | (off-thread)| importer-worker scratch arena; counted under §9.3.1 below as the soft 64 MiB    |
| `WatchEdge` / `RecookRequest` (§4.1.10) | 0   | sim  | 0 MiB    | drained at phase 1; **0 ms on the hot path** — translation runs only on `FileEvent` arrival, gated by phase 1 watcher seam (§4.1.10 invariant 3) |
| **per-aggregate runtime total**     | **0.30**| —    | **192 MiB**| sums against §9.2 combined-CPU 0.50 ms (~0.20 ms slack absorbs variance, §9.3.2)  |

Plus the off-thread soft-ceiling row (counted separately per §9.3.1):

| Off-thread aggregate                | CPU ms  | Half | Heap     | Dominant operation                                                                |
|-------------------------------------|---------|------|----------|-----------------------------------------------------------------------------------|
| Importer / `CookSession` scratch arena | (off-thread) | n/a | **64 MiB** | per-cook arena: FBX SDK / FreeImage / FreeType decode buffers. Off the game-loop driver thread; counted as a **soft ceiling** against the 256 MiB heap cell (§9.4 rule 4) |

Heap composition exact: 16 (Manifest) + 16 (Residency) + 64 (importer
soft ceiling) + 160 (CAS hot-set, mmap-tracked) = **256 MiB** = the
cell heap ceiling.

Notes per row, indexed by aggregate:

- **`SourceAsset` / `Importer` / `CookKey` / `CookedAsset` (0 / 0
  MiB).** These four aggregates are cook-only on the runtime side: the
  runtime never holds a `SourceAsset` (§4.1.1 composition), an
  `Importer` is constructed at cook-session start and destroyed at
  session end on the importer worker pool (§4.1.2 identity), a
  `CookKey` lives in a `CookSession`'s job table (§4.1.3 identity),
  and `CookedAsset` bytes are reached only through the residency
  manager's mmap mapping (§4.1.7 invariant 5; §4.1.4 distinction). No
  per-frame CPU cost and no CPU-side heap on the runtime.
- **`CAS` (0 / 160 MiB).** The CAS aggregate is stateless across reads
  (§4.1.5 invariant 5); per-frame CPU is zero. The 160 MiB is the
  resident-page ceiling for the **CAS hot-set** — the cooked artifact
  bytes the kernel keeps mapped under the residency manager's mmap
  regions for the steady-state working set (S1: ~200 props + 1
  character + 8 light textures + UI / font atlases). This is **not**
  heap-allocated bytes; per `perf-budget.md` Allocator Rule 4 + the
  mmap-only invariants (§4.1.5 invariant 5; §4.1.7 invariant 5), mmap
  entries are tracked separately and **do not count** against the
  per-context allocator's tagged heap. The 160 MiB is enforced as a
  **soft ceiling** in shipping builds (§9.4 rule 5) and asserted by a
  resident-set-size benchmark in CI (§9.5).
- **`Manifest` (~0.05 ms / 16 MiB).** The manifest's runtime hot path
  is the `resolve(AssetId) → ContentHash` query on every
  `AssetHandle::view()` cache miss / hot-reload generation bump
  (§4.1.6 query API; §4.1.8 invariant 4). The query is an O(1) hash
  lookup against the in-RAM snapshot. ~0.05 ms covers the per-frame
  amortised cost across the resident handle set (S1 ≈ ~256 active
  handles × ~200 ns per O(1) probe + the once-per-frame snapshot
  pointer load). 16 MiB holds the in-RAM manifest snapshot for the
  MVP asset count ceiling (~10k `(AssetId, ContentHash, CookKey,
  Set<DependencyEdge>)` rows × ~1.5 KiB amortised per Fory-deserialized
  `ManifestEntry` + dependency-edge sub-graph). Atomic publish swap
  (§4.1.6 invariant 2) replaces the snapshot pointer at phase 8 — the
  16 MiB allows two snapshots resident simultaneously during the
  hot-reload swap window so the prior snapshot's reads drain naturally
  (§4.1.7 invariant 6).
- **`ResidencyManager` (~0.20 ms / 16 MiB).** The dominant runtime
  cost. ~0.20 ms covers the per-frame **residency-state evaluation**:
  scan the resident set for LRU stamp updates (every `view()` access
  bumps the stamp) + screen-coverage priority recompute (input from
  `render`'s extract path per §4.1.7 composition) + eviction
  candidate selection. Sized for the S1 working set of ~256 resident
  artifacts: SIMD-bound priority sort + bounded-degree pending-queue
  drain. The evaluation never exceeds 0.2 ms because the priority
  sort runs on a fixed-cap candidate ring (§4.1.7 invariant 4); over-
  budget configurations refuse admission via
  `ResidencyError::BudgetExceeded` (§4.1.7 invariant 1) rather than
  bleeding into adjacent contexts. 16 MiB holds the residency table
  per `ContentHash` (~10k slots × ~1 KiB metadata: state enum, mmap
  region pointer, ref-count, LRU stamp, screen-coverage scalar) plus
  the priority-ordered `LoadRequest` queue (§4.1.7 composition). The
  CAS hot-set's mmap bytes (160 MiB) are owned by the residency
  manager's mappings but accounted under the CAS row above.
- **`AssetHandle` table (~0.05 ms / 0 MiB content-tagged).** O(1)
  handle resolution: `asset_id → manifest hash lookup → residency
  slot probe + generation-tag check` (§4.1.8 composition). ~0.05 ms
  covers the per-frame amortised cost across the resident handle set
  (S1 ≈ ~256 handle resolutions per frame, ~200 ns each). The handle
  slot table itself is **core-owned**: it lives under `core`'s
  `AssetHandle table` row (`core` SPEC §9.3 — 8 MiB / 0.05 ms
  `core/asset` benchmark cell). Content's column here counts only the
  per-handle resolve work, not the table storage. The split keeps the
  ABI seam (`core::AssetHandle<T>` is a core primitive per
  PHILOSOPHY §3) cleanly attributed to `core` while content owns the
  resolution semantics. No double-counting: core asserts table heap;
  content asserts resolve CPU.
- **`CookSession` (off-thread / scratch in §9.3.1).** Cook orchestration
  runs entirely off the game-loop driver thread on the importer worker
  pool (§4.1.9 composition; `perf-budget.md` `content` row "one-shot
  import work is off-thread"). Per-frame CPU on the driver thread is
  zero. The session's staging table + worker arena live in the
  off-thread soft-ceiling row (§9.3.1 / 64 MiB).
- **`WatchEdge` / `RecookRequest` (0 / 0 MiB).** **0 ms on the hot
  path.** Watcher event translation runs only on `FileEvent` arrival
  from `platform`'s file watcher (§4.1.10 composition; §3.3 platform
  seam). Events are drained at phase 1 (input phase, the platform
  watcher's scheduled drain point), translated against the manifest's
  in-RAM dependency graph (§4.1.10 invariant 3), and enqueued on the
  off-thread importer pool — no FS operations on the driver thread,
  no allocation per non-event frame. Steady-state S1 produces zero
  watcher events per frame (no edits in flight) so the cost is
  literally zero. The hot-reload frame budget (§9.5 + `perf-budget.md`
  §"CI Gate Spec" rule 4) gates the rare event-frame separately.

The per-aggregate runtime CPU total (~0.30 ms) is below the §9.2
combined-CPU 0.50 ms ceiling by ~0.20 ms; that slack is intentional
(§9.3.2) — it absorbs variance in residency-evaluation cost when the
S3 import-completion handoff lands on the same frame as a hot-reload
manifest swap, without spilling into another context's budget.

#### 9.3.1 Off-thread soft ceiling — importer scratch arena

Bulk import / cook (§4.1.2 importers, §4.1.9 cook session) runs on a
**separate worker pool**, not on the game-loop driver thread. Per
`perf-budget.md` §"Justification Per Cell" (`content` row): "Heavy
work — FBX parse (S3), Draco decode, texture upload — runs on the
import worker thread off the game loop." Therefore:

1. The 0.50 ms CPU cell does **not** charge for cook-step CPU time on
   the worker pool. The driver thread is the perf gate's measurement
   surface (§9.5). The S3 fixture exercises this: a ~5 MiB FBX
   drag-drop while S1 plays must not push the **driver thread** out
   of its 0.50 ms cell, regardless of how long the worker takes to
   decode the FBX.
2. The cook-step **scratch arena** (FBX SDK decode buffers, FreeImage
   bitmap scratch, FreeType glyph atlas staging) is allocated under
   `ContextTag::content` per `perf-budget.md` Allocator Rule 1, but
   tracked as a **soft ceiling** of **64 MiB**. "Soft" means: the
   ceiling is enforced in `GLIBRE_ALLOC_STRICT=1` builds (per
   Allocator Rule 2) but its breach in shipping builds emits the
   once-per-frame `spdlog` warn (Allocator Rule 3) without aborting
   the cook. The rationale: a single ~5 MiB FBX with skinning + LOD
   chains can transiently allocate ~30-50 MiB of arena for normalize
   + meshlet partition + BLAS construction; 64 MiB is the smallest
   soft ceiling that admits S3 without surprise refusals while still
   catching unbounded growth (e.g. an FBX importer leak).
3. The arena is **drained between cooks**, not between frames:
   `CookSession`'s end-of-session rollback / publish (§4.1.9
   invariants 1, 5) frees the arena. It is not a per-frame transient
   arena (`perf-budget.md` Allocator Rule 4 transient-arena
   exemption) because the cook is multi-frame; the off-thread soft
   ceiling is the right enforcement model.
4. The off-thread arena counts against the `content` 256 MiB heap
   cell (Allocator Rule 1 tag stamping). The cell composition (§9.4)
   reflects this: 16 (Manifest) + 16 (Residency) + 64 (importer
   soft) + 160 (CAS hot-set, mmap-tracked) = 256 MiB exactly.

#### 9.3.2 Hot-path operations — exhaustive list

To make the hot path explicit and gate-able, the **complete**
per-frame work content does on the game-loop driver thread is:

1. **Residency state evaluation** (`ResidencyManager`, ~0.20 ms) —
   priority sort + LRU sweep + admission/eviction decisions over the
   resident set.
2. **Hot-reload check** (Manifest snapshot pointer load, ~ns scale
   absorbed in `Manifest` 0.05 ms) — every `AssetHandle::view()`
   reads the active snapshot pointer and re-resolves on generation
   bump (§4.1.8 invariant 4); the once-per-frame snapshot pointer
   load is constant-time.
3. **`AssetHandle` resolution** (~0.05 ms amortised) — O(1) per call;
   the per-frame total scales with `view()` call frequency from
   `render` extract + system reads. Bounded by the resident handle
   set under S1 (~256 calls / frame).

Anything else (cook orchestration, FBX parse, texture decode,
manifest publish, watcher-event translation) runs **off the driver
thread** or **only on watcher-event frames** (which are rare and
gated by §9.5 rule 4, not the steady-state cell).

### 9.4 Allocator Rules

`content` enforces its 256 MiB ceiling — and the per-aggregate
sub-shares in §9.3 — through `glibre::PerContextAllocator`, the
allocator declared in `perf-budget.md` §"Allocator Rules". The
contract `content` SPEC §9 imposes:

1. **Per-context tag (`ContextTag::content`).** Every allocation made
   by any module under `content/src/**` is stamped with
   `ContextTag::content` at the allocator-handle level
   (`perf-budget.md` Allocator Rule 1). This includes the off-thread
   importer worker pool's scratch arena (§9.3.1) — the worker pool
   uses content's allocator handle, not a per-thread default.
2. **Hard ceiling in diagnostic / debug builds.** When
   `GLIBRE_ALLOC_STRICT=1` (debug + diagnostic presets), an
   allocation that would push live `ContextTag::content` bytes above
   256 MiB returns
   `std::unexpected{core::Error::OutOfBudget}` (`perf-budget.md`
   Allocator Rule 2). Every content call site that allocates uses
   the `Result<T>` form (§5) and propagates the error.
3. **Soft warning in shipping builds.** Shipping builds log a `warn`
   once per-tag-per-frame to `spdlog` and increment a frame-stat
   counter on overshoot (`perf-budget.md` Allocator Rule 3).
4. **Importer scratch arena is soft, not transient.** The 64 MiB
   importer arena (§9.3.1) is **not** a `perf-budget.md` Allocator
   Rule 4 transient-arena exemption (those drain at phase 9 and do
   not count against the cell). Cook arenas are multi-frame and
   off-thread; they count against the 256 MiB cell with the
   per-aggregate breakdown in §9.3.1 enforced as a soft sub-ceiling
   inside the hard cell ceiling.
5. **CAS mmap entries do not count against the heap.** Per §4.1.5
   invariant 5 + §4.1.7 invariant 5, the CAS reads cooked bytes via
   `mmap(2)` read-only mappings owned by the residency manager.
   These mmap regions are **not heap allocations** — they are
   page-cache-backed virtual memory views. `glibre::PerContextAllocator`
   does not count them against `ContextTag::content`'s 256 MiB
   ceiling. Instead the **CAS hot-set** (160 MiB, §9.3 row) is
   tracked separately via a resident-set-size accounting hook
   (RSS-delta sampled at phase 9) and enforced as a **soft ceiling**
   in shipping builds (Allocator Rule 3 semantics). The strict-mode
   build asserts the soft ceiling at the same threshold; overshoot
   triggers the `OutOfBudget` arm with an "mmap" detail string. This
   split matches the `render` precedent for GPU memory
   (`perf-budget.md` Allocator Rule 5): residency-tracked,
   not-allocator-tracked.
6. **Manifest snapshot double-buffer.** The 16 MiB Manifest row
   admits two snapshots resident simultaneously during a hot-reload
   manifest swap (§4.1.6 invariant 2; §4.1.7 invariant 6). Both
   snapshots count against the `content` tag; the 16 MiB ceiling is
   sized so the steady-state single-snapshot foot is ~8 MiB and the
   peak two-snapshot foot is ~16 MiB. Drain of the prior snapshot
   happens once its referrers' generation bumps complete (§4.1.8
   invariant 4); the `Manifest`-allocated bytes are released on the
   next allocator service after the prior snapshot's last reader
   exits.
7. **No raw `new` / `malloc` in `content/`.** Per the
   `-Wglibre-no-raw-alloc` clang custom-warning-as-error documented
   in `perf-budget.md` Allocator Rules header, all dynamic
   allocations in `content/src/**` MUST go through
   `PerContextAllocator`. The build rejects raw `new` / `malloc`.
   Vendored SDK arenas (FBX SDK / FreeImage / FreeType) allocate
   through allocator hooks that route to the importer scratch arena
   in §9.3.1; raw vendor `malloc` is the third-party-wrap exemption
   covered at the importer ingress per §4.1.2 invariant 3
   (allocations only inside the per-cook arena).

### 9.5 CI Gate — `BENCHMARK_CELL` Per Aggregate

The per-context CI gate (`perf-budget.yml`, scoped under the
`task-breakdown-error-perf` follow-up spike) requires each per-context
SPEC §9 to declare at least one `BENCHMARK_CELL(...)` block per
aggregate that has non-zero hot-path activity (`perf-budget.md`
§"CI Gate Spec" rule 1). The macro expands to a Catch2 `BENCHMARK`
body that:

1. constructs the §9.3 row's S1-derived fixture for the aggregate,
2. measures wall-clock time over a steady-state sample,
3. asserts `time <= cell_budget_ms` (the row's CPU ms cell),
4. records resident-byte usage for the aggregate's
   `ContextTag::content` sub-share and asserts `<= heap_budget_mib`
   (the row's heap cell).

Mandated `BENCHMARK_CELL` blocks for `content` (PR fails if any
assert fails — `perf-budget.md` §"CI Gate Spec" rule 1):

| Aggregate                | `BENCHMARK_CELL` test name                              | CPU ceiling | Heap ceiling          |
|--------------------------|---------------------------------------------------------|-------------|-----------------------|
| `Manifest`               | `content/manifest: o1_hash_lookup_latency`              | 0.05 ms     | 16 MiB                |
| `ResidencyManager`       | `content/residency: state_evaluation_lru_priority_sweep`| 0.20 ms     | 16 MiB                |
| `AssetHandle` resolve    | `content/handle: o1_resolve_generation_check`           | 0.05 ms     | n/a (core-owned table)|
| Importer scratch arena   | `content/importer: scratch_arena_off_thread_residency`  | n/a (off-thread) | 64 MiB (soft)    |
| CAS hot-set              | `content/cas: hot_set_resident_pages`                   | n/a         | 160 MiB (soft, RSS)   |

In addition to the per-aggregate cells, content owns two engine-wide
gate hooks called out by the spike #146 brief:

1. **Residency churn benchmark.** A microbenchmark (test name
   `content/residency: churn_under_pressure`) drives the
   `ResidencyManager` with a synthetic load that requests admissions
   exceeding the `MemoryBudget`, forcing eviction-driven churn. The
   gate asserts: (a) churn-frame CPU stays inside the 0.20 ms
   `ResidencyManager` cell, (b) no
   `ResidencyError::BudgetExceeded` is silently swallowed, (c) the
   eviction priority is `(screen_coverage, LRU)`-correct per §4.1.7
   invariant 4 (validated against a known-good ordering oracle). PR
   fails on any of (a)/(b)/(c). This is the **residency churn
   benchmark** required by the perf-budget gate spec for content.
2. **Manifest lookup latency.** A microbenchmark (test name
   `content/manifest: o1_hash_lookup_latency`, the same as the
   per-aggregate row above) measures p50 / p99 latency of
   `Manifest::resolve(AssetId)` over a manifest of ~10k entries
   (MVP ceiling). The gate asserts p99 ≤ 0.05 ms / call; sustained
   regression flips the `perf:headroom-low` label per
   `perf-budget.md` §"CI Gate Spec" rule 5. This is the **manifest
   lookup latency** gate required by the perf-budget gate spec for
   content.

Hot-reload-frame variance is exercised by the engine-wide
**hot-reload frame budget** gate (`perf-budget.md` §"CI Gate Spec"
rule 4) which scripts an S2 reload and asserts phase 8 cost ≤ 0.40 ms;
content's manifest-swap cost participates in that aggregate gate via
§4.1.6's atomic-publish path and §4.1.7 invariant 6's drain
semantics. The S2 fixture is engine-wide; content's contribution is
asserted as "manifest snapshot pointer swap is constant-time" inside
the S2 script's phase 8 measurement.

S3 (asset-import while S1 plays) is exercised by an end-to-end
**off-thread soundness** gate (`content/import:
s3_off_thread_no_driver_spike`): the test runs S1 for 600 frames
while triggering a synthetic ~5 MiB FBX import on a worker thread and
asserts the **driver thread**'s p99 phase-1 + phase-9 cost stays
inside the 0.50 ms content cell. This is the gate that proves
importers do not bleed onto the game loop.

## 10. Failure Modes & Error Model

Content's failure surface is the closed sum `glibre::content::Error`
declared in §5.3 — `std::variant<ImporterError, ResidencyError>` — and
every public boundary in §5 returns
`Result<T> = std::expected<T, glibre::Error>` per
`reviews/decisions/error-model.md`. §10 fills four slots that §5.3
left implicit:

1. **Per-arm contract** — for each `ImporterError::*` and
   `ResidencyError::*` enumerator: trigger, recovery posture (re-cook
   / evict / refuse / fallback), and log severity.
2. **Importer SDK translation** — the rule that thrown exceptions
   from the FBX SDK, FreeImage, and FreeType are caught at the
   importer's first ingress and translated into `ImporterError::*`,
   plus the precise list of `-fexceptions` carve-outs the build is
   allowed to apply.
3. **Recovery shapes** — what re-cook, eviction, manifest-rollback,
   and atomic-write retry actually do at the §6.2 / §6.3 / §8.2 data
   flows; cross-references to §8.5 for hot-reload refusal cases.
4. **Logging severity table** — the consolidated mapping
   `glibre::log_error` uses when it formats a `content::Error` into
   spdlog.

§10 introduces no new public types. The §5.3 table is the source of
truth for the variant shape; §10 does not propose new arms. Three
§5.3 arms (`ImporterError::MagicMismatch`, `UnsupportedVersion`,
`MalformedPayload`) jointly cover the failure space the issue
brief named "FbxFailed / FreeImageFailed / FreeTypeFailed" — they
discriminate by *symptom* (which §4.1.2 invariant the SDK
violated), not by *which SDK threw*; the SDK identity rides in
the structured log fields and the `ErrorContext::detail` string,
not in the enumerator. This honours the closed-sum rule of §3.2
collapse #1: adding per-SDK arms would re-introduce the registry
explosion §3.2 collapsed.

### 10.1 `ImporterError` — per-arm contract

Trigger / Recovery / Severity for each `ImporterError::*` enumerator.
"Recovery" names what the *operator* or the *cook session* may do;
the importer itself never auto-retries across the boundary
(§4.1.2 inv #2 — exceptions never escape; §4.1.2 inv #3 — pure of
effect outside its arena).

#### `ImporterError::SourceNotFound`

- **Trigger.** The path declared on the `SourceAsset` either escapes
  the `assets/source/` root (§4.1.1 inv #1) or no longer exists at
  the moment `Importer::ingest()` opens it (file deleted, moved, or
  renamed between watch-edge fan-out and cook). Also raised when an
  FBX-internal sidecar reference (e.g. embedded texture override
  path) escapes the source root after the FBX SDK resolves it.
- **Recovery.** **Refuse + rollback.** The `CookSession` rolls back
  per §4.1.9 inv #1; the prior manifest snapshot remains active
  per §8.5 Class B. Operator restores the source under
  `assets/source/`; the next `WatchEdge` event re-triggers the cook.
  No automatic retry — the watcher's debounce already deduplicates
  rapid restore-then-rename sequences (§4.1.10 inv #2).
- **Severity.** `warn` at the handling boundary. The prior manifest
  is unchanged so the runtime continues; the editor surfaces the
  rejection in its asset-tree status badge.

#### `ImporterError::MagicMismatch`

- **Trigger.** The SDK reports format-prefix mismatch — the file
  passed magic-byte sniffing for one `SourceKind` (e.g. extension
  said `.fbx`) but the SDK rejected the actual prefix. Concretely:
  FBX SDK's `FbxImporter::Initialize` returns
  `eInvalidFileVersion`/`eInvalidFile` with a magic-related status;
  FreeImage's `FreeImage_GetFileType` returns `FIF_UNKNOWN` or a
  format that disagrees with the resolved `SourceKind`; FreeType's
  `FT_New_Face` returns `FT_Err_Unknown_File_Format`. Each is
  translated at the importer's `-fexceptions` ingress point per
  §4.1.2 inv #2.
- **Recovery.** **Refuse + rollback.** The cook step rejects the
  source; the prior manifest remains. Operator re-exports the
  source from the DCC at the expected format, or moves the file to
  the correct extension.
- **Severity.** `warn`. The artist's intent is recoverable; the
  runtime is not at risk.

#### `ImporterError::UnsupportedVersion`

- **Trigger.** The SDK accepts the magic but rejects the file's
  declared version: FBX file version older than the SDK's minimum
  (e.g. FBX < 7.0 ASCII) or newer than the SDK's bundled compatibility
  table; FreeImage codec version unsupported (rare — FreeImage
  versions image *codecs*, not files, so this fires on EXR multipart
  / DWAA-compressed variants the linked FreeImage build was not
  configured for); FreeType reports an unrecognized SFNT or CFF
  table version (`FT_Err_Invalid_Version`).
- **Recovery.** **Refuse + rollback.** Operator re-exports at a
  supported version. The `importer_version` ingredient of `CookKey`
  (§4.1.3 component #2) ensures that bumping the importer's bundled
  SDK version automatically forces re-cook of every dependent on
  the next session, so this arm narrows over time as importers are
  upgraded — but never silently, per §4.1.2 inv #4.
- **Severity.** `warn`.

#### `ImporterError::MalformedPayload`

- **Trigger.** Three failure shapes collapse here per §4.1.4 inv #2
  and §4.1.6 inv #4:
  1. **SDK-detected corruption.** FBX SDK throws on a truncated
     stream or a node-graph cycle inside the file; FreeImage's
     decoder reports a checksum / chunk-size mismatch on PNG /
     JPEG / EXR; FreeType reports
     `FT_Err_Invalid_Table` / `Invalid_Outline` on a corrupt glyph.
     All three flow through the importer's `-fexceptions` ingress.
  2. **Cooker-output schema-validation failure.** The cooked
     `CookedAsset.payload` is a Fory blob whose declared
     `(fqn, schema_version)` is not in the `glibre-types` registry
     — i.e. the cook step produced bytes claiming to be an
     artifact class the runtime does not know how to deserialize.
     §4.1.4 invariant 2; raised by `CookSession::commit()`.
  3. **Dependency cycle at publish.** The bottom-up
     invalidation graph (§4.1.6 inv #4) forms a cycle when the
     manifest is being assembled — typically a circular material
     reference or a self-referential FBX skeleton sub-asset.
     Manifest publish refuses to commit a cyclic graph; §4.1.10
     inv #4.
- **Recovery.** **Refuse + rollback.** All three sub-causes leave
  the prior manifest snapshot active per §8.5 Class B. The
  structured log distinguishes the sub-cause via `error.detail`:
  `"sdk-corruption"`, `"schema-fqn-unknown"`, or `"dependency-cycle"`.
  Operator action differs per sub-cause: re-export the source
  (corruption); file a bug against the cook step's owning context
  (schema — this is a producer-side defect); break the circular
  reference in the source tree (cycle).
- **Severity.** `error` for the schema-validation sub-cause (it is
  always a defect in producer code); `warn` for the corruption and
  cycle sub-causes (artist-correctable).

#### `ImporterError::MissingDependency`

- **Trigger.** A child asset referenced by a manifest entry's
  `DependencyEdge*` cannot be resolved: the child's `AssetId` is not
  in the active manifest, or its `ContentHash` does not appear in
  the CAS at lookup time. Distinct from `ResidencyError::HashNotInCas`
  in that this is detected during *cook-time* dependency walk, not
  runtime residency load. §4.1.6 inv #4.
- **Recovery.** **Re-cook the child.** The `CookSession` enqueues a
  recook for the missing child if its source still exists; the
  parent's cook is deferred until the child publishes. If the
  child's source was deleted, the parent's cook fails with this
  arm and the operator either restores the child source or breaks
  the parent's reference. The session as a whole rolls back if
  recovery is not possible inside the same session.
- **Severity.** `warn` when the child can be re-cooked in the same
  session; `error` when the child's source is missing and the
  parent must be edited.

#### `ImporterError::Cancelled`

- **Trigger.** A `CookSession`'s cancellation token was observed by
  the importer mid-import, or by the cook step between import and
  Fory-serialize. §4.1.2 inv #5; §4.1.9 inv #5.
- **Recovery.** **No-op.** Cancellation is the operator's explicit
  intent (typically: editor stop button, workspace close). The
  in-flight cook step returns this arm; the session reports
  `CookOutcome::Cancelled`; the prior manifest remains active.
  No automatic retry; the operator re-triggers the session if
  desired.
- **Severity.** `debug`. Routine; cancellation is not a failure
  signal at the engine level.

### 10.2 `ResidencyError` — per-arm contract

Trigger / Recovery / Severity for each `ResidencyError::*` enumerator.
The residency manager owns runtime-side recovery; cook-time recovery
belongs to `ImporterError` (§10.1).

#### `ResidencyError::HashNotInCas`

- **Trigger.** A `LoadRequest` for `ContentHash H` reaches the I/O
  lane and `cooked/<prefix>/<H>` does not exist on disk: the
  manifest claims the hash is current, but the CAS layer does not
  hold the bytes. This is the runtime symptom the issue brief named
  "AssetMissing" *and* the symptom of "CasIntegrityFailed" — the
  closed sum collapses both because residency cannot distinguish
  "never written" from "written and corrupted on disk" without
  re-hashing, and re-hashing is a cook-time concern (§4.1.5
  inv #4 — hash collisions are cryptographically impossible by
  construction; a payload mismatch implies corruption *of the
  filesystem*, which is an OS-level event, not a content invariant).
  Causes in practice: (a) the CAS file was deleted out from under
  the workspace; (b) the workspace was moved between hosts and
  `cooked/` was not copied; (c) filesystem-level corruption ate
  the file.
- **Recovery.** **Refuse + re-cook.** The residency manager
  surfaces this arm and fails the `LoadRequest` (the slot returns
  to `Unloaded`). The next `CookSession` re-cooks every asset
  whose `ContentHash` is not present in the CAS; the manifest
  publishes a fresh entry only if the re-cook succeeds. Until then
  the consumer (typically `render`) falls back to its own
  default-asset path per the engine-wide composition rule
  (`reviews/decisions/error-model.md` §"Composition Rules" #2).
  The handle remains valid — see §4.1.8 — but `view()` will
  return this arm until the re-cook completes.
- **Severity.** `error`. A hash-not-in-CAS at runtime always
  indicates either a workspace move bug or filesystem-level
  damage; it is operator-actionable and never routine.

#### `ResidencyError::ManifestStale`

- **Trigger.** A handle resolved against a manifest snapshot that
  has since been superseded by a `CookSession::commit()` publish.
  §4.1.6 inv #2; §4.1.8 inv #4. Concretely: the consumer cached
  a `ContentHash` from `Manifest::resolve(asset_id)` at frame N
  and called back at frame N+k after a hot-reload; the snapshot
  the consumer was reading no longer matches the active manifest.
  This is the runtime symptom the issue brief named "HandleStale".
- **Recovery.** **Refresh + retry.** The handle is still valid
  (handles are hash-keyed, not snapshot-keyed); the consumer
  re-resolves through `AssetHandle::view()` which transparently
  picks up the new `ContentHash`. The residency manager keeps
  the prior `Resident` slot live as long as outstanding handle
  refs > 0 (§4.1.7 inv #6) so the consumer never sees torn bytes
  during the swap. Returning this arm is the contract for
  "you used a stale snapshot; re-resolve and continue."
- **Severity.** `info`. Routine during hot-reload; warn-level
  noise would drown legitimate failures.

#### `ResidencyError::BudgetExceeded`

- **Trigger.** A `LoadRequest`'s deadline elapsed without admission:
  the residency budget (§4.1.7 inv #1) was full, every resident
  artifact had outstanding handle refs (so eviction could not free
  room — §4.1.7 inv #3), and the request's deadline reached zero
  while still in `Pending`. The §9 content cell's budget is the
  hard ceiling.
- **Recovery.** **Backpressure + retry next frame.** The consumer
  drops the request and surfaces the failure to its own domain.
  Render's typical response: keep the prior LOD resident, defer
  the upgrade to the next frame, log at `warn`. Editor's typical
  response: surface "RAM ceiling reached" in the asset browser
  and let the operator unload an open scene. The residency
  manager itself never auto-retries; backpressure is the consumer's
  to apply per the priority weight it supplied
  (§4.1.7 invariant 4 — `screen_coverage` is opaque to residency).
- **Severity.** `warn`. Repeated within a single second escalates
  to `error` via the §10.4 escalation rule.

#### `ResidencyError::IoFailure`

- **Trigger.** An OS-level I/O error during the residency manager's
  load path: `mmap` returned an error, `open` failed for a reason
  other than ENOENT (which would surface as `HashNotInCas`), or
  `fstat` reported a size that disagrees with the manifest's
  expected payload length. Wraps the typed `platform::Error::IoFailure`
  arm per the cross-context composition rule
  (`reviews/decisions/error-model.md` §"Composition Rules" #2 —
  content translates platform's error into its own enum at the
  ingress site, never auto-upcasts). Also the residency-side carrier
  for the issue brief's "AtomicWriteFailed" — the runtime cannot
  distinguish "rename interrupted" from "open after rename failed"
  without re-reading the CAS, so both surface here, and the
  diagnostic prefix in `error.detail` (`"open"`, `"mmap"`,
  `"size-mismatch"`) discriminates for telemetry. Note that the
  *cook-time* atomic-write protocol (§4.1.5 inv #3) is implemented
  inside the CAS aggregate and reports failures up to the
  `CookSession`, which surfaces them as `ImporterError::MalformedPayload`
  with `error.detail = "atomic-write-failed"` — the runtime never
  observes a half-written CAS file because `rename(2)` is the
  publication boundary (§4.1.5 inv #3) and the manifest cannot
  reference a hash whose CAS write never committed.
- **Recovery.** **Refuse.** The `LoadRequest` fails; the slot
  returns to `Unloaded`. The consumer falls back as in
  `BudgetExceeded`. Persistent IO failures imply hardware /
  workspace-mount problems and require operator intervention;
  no automatic retry inside content (the platform context's I/O
  thread already applied its single bounded EINTR retry per
  `specs/platform/SPEC.md` §10.2.1 before content sees the arm).
- **Severity.** `error`. OS-level I/O failure is never routine.

### 10.3 Importer SDK translation — `-fexceptions` carve-outs

The importer aggregates (§4.1.2) are the **only** translation units in
the engine that compile with `-fexceptions`. Everything else compiles
with `-fno-exceptions` per `reviews/decisions/error-model.md`
§"Decision" rule 3. The carve-out is bounded to three files exactly:

```
plugins/content/cook/import/
    fbx_importer.cpp            # FBX SDK: -fexceptions
    freeimage_importer.cpp      # FreeImage: -fexceptions
    freetype_importer.cpp       # FreeType: -fexceptions
```

The corresponding headers (`fbx_importer.hpp`, etc.) compile with
`-fno-exceptions` like the rest of the engine; only the implementation
TUs that *call into* the SDK carry the carve-out. This matches §6.1's
module layout and is enforced by the build system, not by convention.

#### Translation contract

Each importer's first-ingress entry point (`Importer::ingest(...)`)
wraps every SDK call in a `try` block whose `catch` arms perform the
classification table below. The translated `ImporterError` is
returned as `std::unexpected{Error{ImporterError::*, ErrorContext}}`;
no exception ever crosses the importer's public boundary
(§4.1.2 inv #2).

| SDK exception class                                       | Translated arm                              | `error.detail` prefix |
|-----------------------------------------------------------|---------------------------------------------|-----------------------|
| FBX SDK `FbxStatus::eInvalidFile`                         | `ImporterError::MagicMismatch`              | `"fbx-magic"`         |
| FBX SDK `FbxStatus::eInvalidFileVersion`                  | `ImporterError::UnsupportedVersion`         | `"fbx-version"`       |
| FBX SDK `FbxStatus::eFileCorrupted`, exceptions during traversal | `ImporterError::MalformedPayload`    | `"fbx-corruption"`    |
| FBX SDK `FbxStatus::eFileNotFound`                        | `ImporterError::SourceNotFound`             | `"fbx-not-found"`     |
| FreeImage `FIF_UNKNOWN` from `FreeImage_GetFileType`      | `ImporterError::MagicMismatch`              | `"freeimage-magic"`   |
| FreeImage decoder error callback fired                    | `ImporterError::MalformedPayload`           | `"freeimage-corruption"` |
| FreeImage codec-version unsupported (build-time absent)   | `ImporterError::UnsupportedVersion`         | `"freeimage-codec"`   |
| FreeType `FT_Err_Unknown_File_Format`                     | `ImporterError::MagicMismatch`              | `"freetype-magic"`    |
| FreeType `FT_Err_Invalid_Version`                         | `ImporterError::UnsupportedVersion`         | `"freetype-version"`  |
| FreeType `FT_Err_Invalid_Table` / `Invalid_Outline` / `Invalid_File_Format` | `ImporterError::MalformedPayload` | `"freetype-corruption"` |
| `std::bad_alloc` from any SDK                             | terminate (see note below)                  | n/a                   |
| Any other SDK-thrown exception or status                  | `ImporterError::MalformedPayload`           | `"unclassified"`      |

The `std::bad_alloc` case is **not** translated — the importer's
per-cook arena (§4.1.2 inv #3) is sized to fit the largest source
the budget cell allows; a `bad_alloc` from inside that arena is a
build-system / configuration defect and the process is terminated
with a structured log. This mirrors the editor-UI carve-out's
deferred decision in `reviews/decisions/error-model.md` open
question 4 — content commits to terminate, not to map; the gate
that would re-open this (a `core::Error::OutOfMemory` arm landing
when the editor module lands) is recorded against §10.7's second
entry, not as a content-owned §12 question.

#### Why the SDK identity does not ride in the enum

The closed-sum rule of §3.2 collapse #1 forbids per-SDK arms
(`FbxFailed`, `FreeImageFailed`, `FreeTypeFailed`). The §5.3 enum
discriminates by *symptom*, not by *which SDK threw*: a corrupt PNG
and a corrupt FBX both produce `MalformedPayload` because the
recovery posture (refuse + rollback + operator re-export) is
identical. SDK identity rides in the structured-log fields
(`importer_kind`, `error.detail`) where it serves telemetry and
debugging without inflating the variant arity. Adding per-SDK arms
would re-introduce the registry explosion §3.2 collapsed and
require a `core::Error` variant edit per importer family — the
opposite of what the closed sum is for.

### 10.4 Recovery shape summary

Mapping of every §10.1 / §10.2 arm to one of the four recovery
postures:

| Recovery posture        | Arms                                                                                       | Mechanism                                              |
|-------------------------|--------------------------------------------------------------------------------------------|--------------------------------------------------------|
| **Re-cook**             | `MissingDependency`, `HashNotInCas`                                                        | Next `CookSession` enqueues the affected `AssetId`s; manifest publishes when cook succeeds (§6.2). |
| **Evict**               | (no direct arm; budget pressure handled inside `BudgetExceeded`'s eviction loop, §4.1.7 inv #1) | Lowest-priority resident artifact moves to `Evicting`; new mapping admitted; if no evictable artifact exists, the request returns `BudgetExceeded`. |
| **Refuse**              | `SourceNotFound`, `MagicMismatch`, `UnsupportedVersion`, `MalformedPayload`, `IoFailure`, `Cancelled` | Cook step rolls back per §8.5 Class B; runtime load path returns the arm; prior manifest stays active. |
| **Fallback** (consumer) | `BudgetExceeded`, `ManifestStale`, `HashNotInCas` (consumer-side)                          | Consumer (render / editor) keeps the prior asset visible, retries on the next frame or after the next manifest publish. |

The "evict" posture appears as a *recovery mechanism inside the
residency manager*, not as a directly-returned arm — eviction is
the manager's response to budget pressure *before* it surfaces
`BudgetExceeded`. This is consistent with §4.1.7 inv #1 (hard
ceiling, no silent overshoot) and §3.2 collapse #2 (one residency
seam, not separate "eviction" + "load" surfaces).

The "AtomicWriteFailed" symptom the issue brief named is structurally
absent from the runtime path because the §4.1.5 atomic-write
protocol guarantees the manifest can never reference a hash whose
CAS write did not commit. Cook-time atomic-write failures surface
as `ImporterError::MalformedPayload` with `error.detail =
"atomic-write-failed"` (§10.1, MalformedPayload sub-cause #2 path);
the manifest publish is refused, the prior snapshot remains active.
Runtime-side atomic-write failures are impossible by construction
(§4.1.5 inv #2 — append-only at the file layer; reads see either
the prior bytes or the new bytes, never an in-progress prefix).

The "ManifestCorrupted" symptom is similarly absent as a runtime
arm: a manifest snapshot is constructed from a Fory-deserialized
buffer that fails the `(fqn, schema_version)` registry check at
load time and is rejected by `data`'s schema layer, surfacing as
`data::Error::SchemaMigrationFailed` per the cross-context
composition rule. Content's only owned manifest-corruption path is
the publish-time integrity check, which surfaces as
`ImporterError::MalformedPayload` with `error.detail =
"manifest-publish-cycle"` (the §10.1 MalformedPayload sub-cause #3
"dependency-cycle" case).

### 10.5 Logging severity table (consolidated)

The §10.1 / §10.2 per-arm severities, restated as the table the
`glibre::log_error` helper (per `reviews/decisions/error-model.md`
§"Logging / Telemetry") uses when it formats a `content::Error`
into spdlog. Content never logs at the *raise* site; logging is
the *handler's* responsibility per the same decision document.

| Arm                                              | Default severity | Notes                                                                       |
|--------------------------------------------------|------------------|-----------------------------------------------------------------------------|
| `ImporterError::SourceNotFound`                  | `warn`           | Operator-actionable; restore source under `assets/source/`.                 |
| `ImporterError::MagicMismatch`                   | `warn`           | Re-export from DCC; not runtime-fatal.                                      |
| `ImporterError::UnsupportedVersion`              | `warn`           | Bumping importer's bundled SDK forces re-cook automatically.                |
| `ImporterError::MalformedPayload` (sub: `sdk-corruption`)  | `warn`  | Artist-correctable.                                                         |
| `ImporterError::MalformedPayload` (sub: `schema-fqn-unknown`) | `error` | Producer-side defect; file a bug.                                          |
| `ImporterError::MalformedPayload` (sub: `dependency-cycle`)   | `warn`  | Break the cycle in the source tree.                                        |
| `ImporterError::MalformedPayload` (sub: `atomic-write-failed`) | `error` | Filesystem-level event; investigate disk / mount.                          |
| `ImporterError::MissingDependency` (child re-cookable)        | `warn`  | Same-session recovery via dependency-walk.                                  |
| `ImporterError::MissingDependency` (child source missing)     | `error` | Operator must edit the parent or restore the child source.                 |
| `ImporterError::Cancelled`                       | `debug`          | Routine; explicit operator intent.                                          |
| `ResidencyError::HashNotInCas`                   | `error`          | Workspace-move or filesystem-damage signal; re-cook on next session.        |
| `ResidencyError::ManifestStale`                  | `info`           | Routine during hot-reload; consumer re-resolves.                            |
| `ResidencyError::BudgetExceeded`                 | `warn`           | Escalates to `error` on rapid recurrence (>3 within 1 s).                   |
| `ResidencyError::IoFailure`                      | `error`          | OS-level I/O is never routine; platform's bounded EINTR retry already ran. |

Structured fields attached at the handling site (in addition to the
engine-wide `error.tag` / `error.code` / `error.file` / `error.line`
from `reviews/decisions/error-model.md` §"Logging / Telemetry"):

- `asset_id` — the `AssetId` the operation was on, when known.
- `content_hash` — the `ContentHash` (BLAKE3 hex), when residency-side.
- `source_path` — the `SourceAsset.path` (relative to
  `assets/source/`), when importer-side.
- `importer_kind` — `"fbx"` / `"freeimage"` / `"freetype"`, when
  importer-side. **Required** for §10.3 SDK-attributable arms so
  telemetry can spot per-SDK regression patterns even though the
  enum does not discriminate by SDK.
- `cook_session_id` — the `CookSession`'s opaque id, when cook-time.

### 10.6 Cross-references (out of §10 scope)

- **Hot-reload refusal classes.** `core::Error::PluginAbiHashMismatch`,
  `PluginInitFailed`, `SchemaMigrationFailed`, `HotReloadRefused`
  surface during the `glibre.content` plugin's own dylib swap.
  Owned by `specs/core/SPEC.md`; documented at the content seam in
  §8.5 Class C. Content contributes nothing new to those arms.
- **Schema migration.** `data::Error::SchemaMigrationFailed` covers
  Fory-serialized payloads whose `(fqn, schema_version)` is no
  longer in the registry — including the manifest itself when read
  from disk at workspace open. Owned by `specs/data/SPEC.md`.
  Content's `MalformedPayload (schema-fqn-unknown)` sub-cause is
  the *cook-time producer-side* mirror; both surface for the same
  underlying problem (a producer wrote bytes claiming to be a
  schema the runtime cannot deserialize) but at different sides
  of the persistence boundary.
- **Platform I/O.** `platform::Error::IoFailure` from `mmap`,
  `open`, `rename`, file-watcher syscalls. Owned by
  `specs/platform/SPEC.md` §10. Content translates into its own
  `ResidencyError::IoFailure` at the residency manager's I/O lane
  (per `reviews/decisions/error-model.md` §"Composition Rules" #2);
  the `OsCode` from platform rides in `error.detail` for
  telemetry, never branched on.
- **Render device-lost on swap.** `render::Error::DeviceLost`
  during a manifest swap that touches GPU-resident state. Owned by
  `specs/render/SPEC.md`. Content's manifest publish completes at
  the CPU layer; render's GPU handle re-resolution is its concern.

### 10.7 Open questions — resolved

All four questions resolve in place against an existing external gate;
§12 holds no content-owned residue. Each entry below names the gate
and the trigger that re-opens the question.

- **Per-SDK escalation telemetry.** Resolved by deferral to the
  second-consumer rule (PHILOSOPHY principle 10, Occam's razor;
  §4.7 inv #1 — closed sum, deliberate central edit). The §10.5
  `importer_kind` log field is sufficient until a second consumer
  needs to discriminate per SDK; promotion to a typed
  `ImporterError` payload is a deliberate §5.3 enum amendment, not
  a silent enrichment. Trigger: a second consumer (production
  telemetry retention SLA, per-SDK regression watch) lands and
  needs to branch on `importer_kind`.
- **`bad_alloc` from importer SDKs.** Resolved as terminate per
  §10.3 (the `std::bad_alloc` row of the §10.3 classification
  table) — content commits to terminate; the per-cook arena
  (§4.1.2 inv #3) is sized to fit the budget cell, so a `bad_alloc`
  inside it is a build-system / configuration defect, not a runtime
  recovery situation. Trigger: `reviews/decisions/error-model.md`
  open question 4 (editor module landing + cross-cutting OOM
  policy) flips to a `core::Error::OutOfMemory` arm; if it does,
  content follows the central decision rather than re-deriving.
- **Cancellation as `info` vs `debug`.** Resolved as a single
  `ImporterError::Cancelled` arm (§5.3) plus differentiated log
  levels at the call site (editor stop-button → `info`; watcher
  debounce → `debug`). The §10.5 structured fields already carry
  `importer_kind` and the originating cause, so the operational
  distinction is recoverable from telemetry without inflating the
  closed sum. Trigger: the same second-consumer rule as OQ #1 —
  if a third caller needs to branch on cancellation cause (not
  just log it differently), split into `Cancelled` / `Superseded`
  in a deliberate §5.3 amendment.
- **Re-cook escalation on `HashNotInCas` storm.** Resolved at the
  per-request granularity per §10.1 — content surfaces one
  `ResidencyError::HashNotInCas` per request at `error` level and
  does not aggregate. A consolidating storm-detector (e.g. drop
  the `cooked/` directory → suppress N duplicate `error` lines into
  one `warn` summary until next session re-cook) is observability
  policy, not content policy, and lives in the post-MVP `obs`
  domain. Trigger: the `obs` domain spec lands; until then, content
  does not collapse arms because the §4.7 inv #1 closed-sum rule
  forbids inventing arms speculatively.

## 11. Acceptance Criteria

GitHub `type:user-story` issues this spec closes (drafted under
spike #148, parent sub-epic #138):

- #404 — content: FbxImporter ingests `.fbx` → `MeshArtifact`
  precursor (no SDK leak) — exercises §4.1.2 `FbxImporter` and the
  `-fexceptions` carve-out per the error-model decision.
- #405 — content: `ImageImporter` (FreeImage) decodes
  PNG / JPEG / EXR / HDR / TIFF — exercises §4.1.2 `ImageImporter`
  and the §3.3 refusal to author GPU-format compression.
- #406 — content: `FontImporter` (FreeType) bakes glyph metrics +
  SDF atlas — exercises §4.1.2 `FontImporter` and the §2
  `FontArtifact` shape.
- #407 — content: `CookKey` is a pure function of declared inputs,
  canonical + length-prefixed BLAKE3 — exercises §4.1.3 invariants
  1-4 and PHILOSOPHY §7 cross-host determinism.
- #408 — content: `CAS::commit` lands cooked bytes via tmp + fsync
  + `rename(2)` — exercises §4.1.5 invariants 2-6 (atomic publish,
  dedup, mmap-readable, path-scoped).
- #409 — content: `Manifest` atomic publish — all-or-nothing
  manifest swap — exercises §4.1.6 invariants 1-4 and §4.1.9
  invariant 2 (CAS-write-before-manifest-publish).
- #410 — content: `ResidencyManager` defends `MemoryBudget` via
  `(screen_coverage, LRU)` priority eviction — exercises §4.1.7
  invariants 1-5.
- #411 — content: hot-reload re-cook on source save lands at the
  frame-8 boundary in ≤ 500ms — exercises §4.1.7 invariant 6,
  §4.1.10 invariants 1-4, and PHILOSOPHY §8.
- #412 — content: `DependencyEdge` fan-out drives bottom-up
  topological re-cook — exercises §4.1.6 invariant 4, §4.1.10
  invariants 2-4, and §4.1.9 invariant 3.
- #413 — content: `AssetHandle<T>::view()` resolves stable bytes
  through `Manifest → CAS → ResidencyManager` — exercises §4.1.8
  invariants 1-5 and §4.1.7 invariants 3, 6.
- #414 — content: `CookSession` parallel topological orchestration
  with all-or-rollback publish — exercises §4.1.9 invariants 1-5.
- #415 — content: in-flight imports honor cancellation within one
  frame — exercises §4.1.2 invariant 5 and §4.1.9 invariant 5.
- #416 — content: importer faults surface as typed
  `ImporterError::*` with path + offset — exercises §4.1.2
  invariant 2 and the §2 `ImporterError` closed sum.
- #417 — content: `Manifest::resolve` returns wait-free typed
  result against publish race — exercises §4.1.6 invariants 1, 2, 5.
- #418 — content: `Residency` state-machine totality
  (`Unloaded → Pending → Resident → Evicting`) — exercises §4.1.7
  invariants 2, 5, 6 and the §2 `Residency` closed sum.
- #419 — content: `WatchEdge` debounces atomic-save bursts into
  one `RecookRequest` per `AssetId` — exercises §4.1.10 invariants
  1-4.

Each must have a Catch2 test by name.

## 12. Open Questions

All questions identified during the §1–§11 design passes resolved in
place under spike #149; this section holds no content-owned residue.
Resolution records and the gates that re-open each decision live in
§10.7 (four entries — per-SDK escalation telemetry, importer-SDK
`bad_alloc`, cancellation log-level, `HashNotInCas` storm). Forward-
pointers from §10.3 prose route to §10.7 rather than here.
