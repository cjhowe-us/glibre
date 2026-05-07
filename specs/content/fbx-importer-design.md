# content — Detailed Design: fbx-importer aggregate

> Detailed design for the `FbxImporter` aggregate (cook-time entity)
> declared in `specs/content/SPEC.md` §4.1.2, with public surface
> locked in §5.4 and lifecycle / scratch / SDK seams pinned in §6.1
> (`importers/fbx_importer.{hpp,cpp}`), §6.2 stage 3, §6.5 (build-time
> cut row), §7.1.2 (`importer_version` participation in `CookKey`),
> §8.5 Class B (importer / cook-step refusal classes), §9.3.1
> (off-thread soft-ceiling scratch arena), §10.1 (`ImporterError::*`
> per-arm contract), and §10.3 (`-fexceptions` carve-out + SDK-exception
> translation table). Refines those sections in place; introduces no
> new public surface beyond `specs/content/SPEC.md` §5.4. Deviations
> from the cited records require an amendment spike, not an in-place
> edit.
>
> Cites `reviews/decisions/error-model.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/fory-codegen.md`,
> `reviews/decisions/frame-phases.md`. No public surface is introduced
> beyond the §5.4 stub locked in `specs/content/SPEC.md`; the §5.4
> normalize-parameter vocabulary refinement below is documented as a
> pending §5.4 amendment (§12, OQ-1) and routed through a follow-up
> task-breakdown spike, not a unilateral header edit.
>
> Harmonius prior art (`harmonius/docs/requirements/content-pipeline/
> asset-import.md`, R-12.1.1 / R-12.1.2 / R-12.1.3 / R-12.1.4 /
> R-12.1.5) cited as research input only — every conclusion below was
> independently re-derived per `PHILOSOPHY.md` §"How harmonius is
> used". No glTF / Alembic / USD coverage; the §3.2 collapse #1
> commitment to one mesh path (FBX SDK only) is preserved, not
> revisited.
>
> Refs: spike #807 — `[SPIKE] design-content-fbx-importer-detailed`.
> Parent: #806 (sub-epic — Detailed Designs — content). Sibling
> `[SPIKE] task-breakdown-content-fbx-importer-detailed` is blocked by
> this deliverable.

## 1. Purpose

The `fbx-importer` aggregate is the single component in the content
context permitted to link the Autodesk FBX SDK and to call into its
C++ surface. Its one responsibility is **decoding one
`SourceAsset { kind = Mesh, format = Fbx }` value object into the
normalized in-memory bytes of a `glibre.content.MeshArtifact`
precursor**, packaged as `eastl::span<const std::byte>` allocated
inside a caller-supplied `ImporterArena`, with every SDK-thrown
exception translated at first ingress into a typed
`ImporterError::*` arm and every allocation routed through the
importer's per-cook arena (PHILOSOPHY §11). Concretely the aggregate
owns:

1. The **`FbxImporter` final entity** (SPEC §4.1.2) — the closed-sum
   member that serves `SourceKind::Mesh`, constructed at cook-session
   start through `FbxImporter::create()` (SPEC §5.4) and destroyed at
   session end. One `FbxImporter` instance per importer worker thread
   in the cook worker pool (§6.5 below); the FBX SDK's `FbxManager`
   is **not** thread-safe across `FbxScene` traversal, so the design
   pins a `FbxManager`-per-thread topology rather than a process-wide
   singleton.
2. The **first-ingress SDK seam** — every Autodesk FBX SDK call site
   in the engine lives inside `plugins/content/cook/import/
   fbx_importer.cpp` (SPEC §10.3, the unique `-fexceptions` carve-out
   for FBX). The seam catches `FbxStatus`-typed soft failures, raw
   `std::exception` derivatives from the SDK's libxml2 / zlib internal
   paths, and the unbounded `std::bad_alloc` case, and translates each
   into a typed `ImporterError::*` arm with a structured-log
   `error.detail` prefix per the §10.3 classification table.
3. The **scene-graph normalize pipeline** — the deterministic walk
   over `FbxScene` → `FbxNode` → `FbxMesh` / `FbxSkeleton` /
   `FbxCluster` that produces the `MeshArtifact` precursor's
   in-memory layout (vertex streams in engine-canonical layout, index
   streams, skeleton + scene-hierarchy nodes, per-vertex skinning
   weights, animation curves *deferred* per §3.6, materials *delegated
   to texture importer* per §3.6). The walk is pure-of-effect outside
   the per-cook arena (SPEC §4.1.2 inv #3) and respects the
   `CancellationToken` polled at every node boundary (SPEC §4.1.2
   inv #5).
4. The **`importer_version` digest** — a 32-byte BLAKE3 over the
   importer's compiled-in identity (FBX SDK version string + major
   normalize-params-vocabulary version + post-process options
   vocabulary), embedded as a string literal in the importer's
   translation unit at build time and returned through
   `Importer::version()` (SPEC §5.4). The digest is one of the five
   ingredients of `CookKey` (SPEC §4.1.3 component #2) and the rule
   that bumping the SDK forces re-cook of every dependent.
5. The **per-cook arena lifetime** — `ImporterArena` (the typed alias
   over `glibre::PerContextAllocator` per `perf-budget.md` Allocator
   Rule 1, SPEC §9.3.1) holds every byte the importer allocates: the
   FBX SDK's internal allocations (re-routed through the SDK's
   `FbxMemoryAllocator` hook), the normalized vertex / index streams,
   the skeleton table, the scene-hierarchy node array, and any
   transient scratch the normalize pipeline needs. The arena drains
   once `CookSession`'s end-of-session rollback / publish path runs
   (SPEC §4.1.9 inv #1, #5) and never bleeds into the runtime heap.

This aggregate **refuses to own**:

- **Mesh-internal geometry processing** — meshlet partitioning,
  vertex-cache reorder, automatic LOD chain authoring, BLAS
  construction, lightmap UV unwrapping, mikktspace tangent
  re-derivation. SPEC §3.3 routes these to the `geometry` plugin;
  the cook-step orchestrator (§6.2 stage 3) calls into geometry's
  cook-step API after the importer returns its precursor. The
  importer hands geometry the canonical streams and walks away.
- **CAS write** — `CAS::put(CookedAsset)` (SPEC §4.1.5, §5.7) is the
  cook-step's stage 5 (SPEC §6.2). The importer does not touch
  `cooked/<prefix>/<hash>` and never sees a `ContentHash`.
- **Manifest publish** — `Manifest::publish` (SPEC §4.1.6) is the
  cook-session's end-of-session step (SPEC §6.2 manifest-publish
  paragraph). The importer does not see an `AssetId` outside the
  `SourceAsset`'s identity-on-cook context; it never resolves
  `AssetId → ContentHash`.
- **Cook-key derivation** — `make_cook_key` (SPEC §5.5, §4.1.3) is
  authored in `cook/cook_key.cpp` and consumes the importer's
  `version()` value as one of five ingredients. The importer does
  not author the digest; it only contributes its compiled-in
  identity. This is the SRP split that keeps "what counts as the
  same cook" (`CookKey`) separate from "the SDK seam"
  (`FbxImporter`).
- **Fory serialization of the precursor** — the importer returns
  pre-Fory bytes (an in-memory layout the next cook-step stage
  Fory-encodes per SPEC §6.2 stage 4). Fory codegen and the
  `glibre-types.dylib` middleman are owned by `data`
  (`reviews/decisions/fory-codegen.md`); the importer is a producer
  of bytes that the next stage Fory-encodes. The §3.4 below pins
  the precursor's **in-memory** layout — distinct from the Fory
  envelope shape that lands in the CAS.
- **Source-file I/O mechanics** — the importer reads through
  `platform::FileIo` via the `SourceAsset.path` resolved by
  `cook/session.cpp` stage 1 (SPEC §6.2). The watcher seam
  (SPEC §4.1.10), the FSEvents callback, and the OS-level `read(2)`
  mechanics live in `platform`. The importer does not touch any
  path other than what the cook session hands it through
  `SourceAsset`.
- **Source debounce / dedup** — the watcher's debounce window
  (SPEC §4.1.10 inv #2) collapses rapid resaves into one
  `RecookRequest` before any importer runs. The importer never
  sees back-to-back imports of the same `(path, source_hash)` pair
  inside one cook session.
- **Audio source decode** — explicitly refused per SPEC §3.3
  (R-12.1.3 → `audio` plugin). The importer rejects any non-`Mesh`
  `SourceKind` at construction (the closed-sum dispatch in
  `importers/dispatch.cpp` makes this a compile-time-exhaustive
  branch; SPEC §4.1.2 inv #1).
- **Texture / image / font ingest** — those are the
  `FreeImageImporter` and `FreeTypeImporter` siblings (SPEC §4.1.2,
  detailed designs are sibling spikes #810 / #816). The FBX path
  may reference embedded texture sub-assets (§3.6 below) but
  delegates their decode to `FreeImageImporter` rather than
  inlining the decode here.
- **Schema authoring and codegen** — `glibre.content.MeshArtifact`
  the Fory schema is authored under `data/schemas/content/
  MeshArtifact.fory` per `reviews/decisions/fory-codegen.md`. The
  importer is a producer of bytes conforming to that schema; it
  never defines the schema.
- **Plugin loader / hot-reload bodies** — `core::PluginLoader` and
  the manifest-pointer flip protocol (SPEC §8.2) are the loader's;
  the importer participates as a build-time-only TU under
  `tools/glibre-cook` (SPEC §6.1 lifecycle seam, SPEC §6.5
  shipping-cuts table) and ships **no runtime symbol** in
  `glibre-content.dylib`.
- **Obj-C / Obj-C++ glue** — CLAUDE.md "No Obj-C++ in engine code".
  The Autodesk FBX SDK is plain C++ (linked from Autodesk's
  pre-built shared library); no `.mm` translation unit appears in
  this aggregate's source.

The aggregate's SRP boundary is sharp: if the FBX SDK version moves,
if the normalize-params vocabulary mutates (axis-convention canonical
form, unit-scale conversion rule, UV-layer selection policy), or if
the per-vertex skinning encoding changes, **this design changes**.
Anything else — manifest layout, cook-session orchestration, residency
policy, Fory schema body, geometry meshlet algorithm — is out of scope.

## 2. Requirements coverage

Mapping of harmonius asset-import requirements
(`harmonius/docs/requirements/content-pipeline/asset-import.md`,
R-12.1.1 .. R-12.1.5) onto MVP coverage in this aggregate. Every
entry is independently re-derived; coverage sites refer to sections
of `specs/content/SPEC.md` and to the design sections below.

| Harmonius clause                                                                              | Glibre disposition (MVP)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
|-----------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-12.1.1** native binary format ingestion with magic / version / hash validation            | **Refused for the FBX importer; collapsed.** R-12.1.1 names a glibre-native pre-cooked binary format (DCC plugin export → engine ingest). Glibre §3.2 collapse #1 explicitly removes that intermediate format: the FBX SDK ingests `.fbx` directly. The validation contract R-12.1.1 names — magic / version / hash — is preserved on the **`SourceAsset → CookedAsset`** seam: SPEC §4.1.1 inv #3 (`source_hash = BLAKE3(bytes)`), §4.1.4 inv #1 (`content_hash = BLAKE3(payload)`), and SPEC §4.1.2 inv #2 (SDK reports `MagicMismatch` / `UnsupportedVersion` at first ingress; §3.7 step 1 below). The "DCC plugin native export" surface re-enters post-MVP behind the same `Importer` seam if needed; the FBX importer covers MVP. |
| **R-12.1.2** texture source import (PNG / JPEG / EXR / HDR / TIFF) with sRGB / linear decode | **Out of scope (sibling).** Texture decode is the `FreeImageImporter` aggregate (sibling spike #810). The FBX importer **does not** decode embedded texture sub-assets; it emits the texture's source-tree path (resolved per §3.6 below) into the `MeshArtifact` precursor's `material_refs` field, and the cook-session orchestrator schedules the texture cook as a separate `RecookRequest` driven by the `DependencyEdge` graph (SPEC §4.1.10 inv #4).                                                                                                                                          |
| **R-12.1.3** audio source import (WAV / FLAC / Ogg Vorbis)                                    | **Refused (out of context).** SPEC §3.3 routes audio source decode to the `audio` plugin. The FBX importer's `SourceKind` is closed-sum on `Mesh`; the dispatch table (SPEC §6.1 `importers/dispatch.cpp`) compile-time-rejects any other kind.                                                                                                                                                                                                                                                                |
| **R-12.1.4** schema validation; errors include source path + byte offset + fix suggestion     | **Covered, scoped.** Path is the structured-log field `source_path` (SPEC §10.5); byte offset is recovered from the FBX SDK's `FbxStatus` line/column when available and embedded in `error.detail` (§3.7 step 2 below); fix-suggestion is the per-arm "Operator action" column from SPEC §10.1 (Class B refusal) — re-export at supported version, restore source under `assets/source/`, break dependency cycle, etc. The structured-log handler formats these uniformly (§3.7 step 5 below).             |
| **R-12.1.5** parallel batch import with progress + cancellation + rollback                    | **Covered, partitioned.** Parallelism and progress tracking are the `CookSession` aggregate's concerns (SPEC §4.1.9, §6.2): the worker pool runs one `FbxImporter::import_one` invocation per scheduling slot; `CookSession::commit()` returns a `CookReport { cooks_executed, cache_hits }` (SPEC §5.12). Cancellation is observed at every `FbxNode` traversal boundary in the importer (§3.7 step 4 below) per SPEC §4.1.2 inv #5; rollback semantics live with the session (SPEC §4.1.9 inv #1).      |

Glibre-native requirements added beyond harmonius:

- **Per-cook arena allocation discipline** (PHILOSOPHY §11; SPEC
  §9.3.1; `perf-budget.md` Allocator Rule #1). Every byte the
  importer allocates — including the FBX SDK's internal allocations
  rerouted via `FbxMemoryAllocator` (§3.5 step 2 below) — comes from
  the per-cook arena tagged `ContextTag::content`. The runtime heap
  is never touched by importer code paths; raw `new` / `malloc` is
  rejected by `-Wglibre-no-raw-alloc`. The 64 MiB soft sub-ceiling
  (SPEC §9.3.1) bounds the largest single FBX import.
- **Single `-fexceptions` carve-out, narrow boundary** (SPEC §10.3,
  `error-model.md` Decision rule 3 + Consequences last bullet). The
  carve-out is `plugins/content/cook/import/fbx_importer.cpp` only;
  the corresponding header `fbx_importer.hpp` and every other TU in
  the engine compile with `-fno-exceptions`. The `try / catch` block
  inside `import_one` is the **single** translation site (§3.7
  step 6 below); no exception ever crosses the importer's public
  boundary (SPEC §4.1.2 inv #2).
- **Deterministic normalize pipeline** (PHILOSOPHY §7,
  cross-aggregate invariant SPEC §4.2 #1). Two cooks of the same
  source bytes with the same `importer_version` and the same
  `NormalizeParams` produce **byte-equal** precursor bytes — and
  hence byte-equal cooked `MeshArtifact` payloads, byte-equal
  `ContentHash`. The normalize pipeline is straight-line code with
  no platform intrinsics, no `std::unordered_*` (replaced by
  `eastl::vector_map` for deterministic iteration order), no PRNG,
  no clock reads, no thread-local caches. §3.7 step 3 below pins
  the determinism rule per-stage.
- **Importer is stateless across cooks; per-cook arena is the only
  state** (SPEC §4.1.2 identity rule). The `FbxImporter::Impl`
  pimpl carries only the `FbxManager*` + `FbxIOSettings*` pair
  (constructed at `create()` time, owned for the importer's
  lifetime); every `import_one` call resets its scratch view of the
  arena and produces a fresh precursor. No memoization, no caches
  keyed off prior cooks.
- **Cancellation is polled, not preemptive** (SPEC §4.1.2 inv #5).
  The importer polls `CancellationToken::is_cancelled()` at every
  `FbxNode` boundary (the natural traversal granularity). Bound:
  ≤ ~1 ms latency to observe cancellation on an MVP-scale FBX
  (~200 nodes per ~5 MiB FBX, S3 fixture; §9 below).
- **No animation-curve / morph-target ingest at MVP** (§3.6 refusal
  list below; §12 OQ-2). FBX scenes carry `FbxAnimStack` /
  `FbxAnimLayer` / `FbxBlendShape` data; MVP imports the bind-pose
  skeleton + skinning weights only. Animation curves enter post-MVP
  through a `data/schemas/content/AnimationClipArtifact.fory`
  authored under a future spike behind the same importer seam.

Coverage rule: every harmonius MVP-scope requirement above either
lands in this design (with a coverage site) or is refused with a
one-line rationale routed to the owning context. No silent drops.

## 3. Detailed model

### 3.1 Aggregate composition

```text
FbxImporter  (final, SPEC §4.1.2; cook-time-only entity)
├── kind_              SourceKind::Mesh                  (closed-sum tag; immutable)
├── version_           ImporterVersion                   (compiled-in BLAKE3; §3.2 below)
└── impl_              Impl*                             (pimpl; arena-allocated by create())

Impl  (private; lives in fbx_importer.cpp)
├── manager_           FbxManager*                       (Autodesk root; one per worker thread)
├── io_settings_       FbxIOSettings*                    (configured at create())
├── memory_allocator_  FbxMemoryAllocator                (routes SDK allocs to per-cook arena)
├── progress_callback_ FbxProgress                       (cancellation-polling shim, §3.7 step 4)
└── version_string_    eastl::string_view                (e.g. "FBX SDK 2025.0/normalize-v1/post-v1";
                                                          the source from which version_ is BLAKE3'd)
```

Per-call scratch (lives entirely inside the supplied `ImporterArena`,
SPEC §5.4):

```text
import_one(...) scratch
├── scene_              FbxScene*                  (allocated via memory_allocator_)
├── fbx_importer_       FbxImporter*               (the SDK's loader, NOT to be confused with our
│                                                   `FbxImporter` aggregate; documented in §3.5)
├── nodes_              eastl::vector<NodeRecord, ImporterArenaAllocator>
├── meshes_             eastl::vector<MeshRecord, ImporterArenaAllocator>
├── skeleton_           eastl::vector<BoneRecord, ImporterArenaAllocator>
├── vertex_streams_     eastl::vector<VertexStream, ImporterArenaAllocator>
├── index_streams_      eastl::vector<IndexStream, ImporterArenaAllocator>
├── material_refs_      eastl::vector<MaterialRef, ImporterArenaAllocator>
└── precursor_bytes_    eastl::span<std::byte>     (final emit; pointer into arena returned to caller)
```

The `FbxImporter` aggregate is the cook-time entity; the §5.4 stub is
the only public surface; everything in the boxes above is private to
the implementation file group `plugins/content/cook/import/`. SPEC
§6.1 already names the file split (`fbx_importer.{hpp,cpp}`,
`dispatch.{hpp,cpp}`, `version.{hpp,cpp}`).

`ImporterArenaAllocator` is the EASTL-conformant adaptor over
`glibre::PerContextAllocator` exposing `ContextTag::content` to
EASTL's allocator-by-value contract. The adaptor's body is a simple
forward-call shim authored in `core/include/glibre/alloc.hpp`
(per `perf-budget.md` Allocator Rules header); the importer pulls
it in via the `ImporterArena&` reference handed to `import_one`.

### 3.2 `ImporterVersion` — the compiled-in importer identity

`ImporterVersion` (SPEC §5.4) is a 32-byte BLAKE3 over a canonical
**version string** baked into the importer's translation unit at
build time. The string is the canonical concatenation of three
components (length-prefixed, separated by `':'`):

```text
"glibre.content.fbx_importer" ":"
SDK_TAG     ":"     // e.g. "fbx-sdk-2025.0"          — the Autodesk FBX SDK version linked.
NORMALIZE_TAG ":"   // e.g. "normalize-v1"           — the §3.4 NormalizeParams vocabulary version.
POST_TAG            // e.g. "post-v1"                — reserved for future post-process stages
                                                       (e.g. tangent-recompute toggle); MVP = "post-v0".
```

Concrete example:

```text
glibre.content.fbx_importer:fbx-sdk-2025.0:normalize-v1:post-v0
```

The string is recovered at code-review time from the importer's
header (one constant per release). The BLAKE3 digest is computed at
**codegen time** by a small build-system step that reads the constant
and emits a `version.cpp` carrying the 32-byte hex literal; the
importer's `Importer::version()` returns the digest. Bumping the SDK
version, the normalize-params vocabulary version, or the post-process
options vocabulary version changes the digest (and only those changes
do).

The digest's role:

1. **Cache-key ingredient (SPEC §4.1.3 component #2; §7.1.2).** The
   `CookKey` digest depends on `importer_version`; bumping it forces
   re-cook of every dependent on next session per SPEC §7.2.2.
2. **Hot-reload coexistence (§8 below).** The build-time-only nature
   of the importer means a runtime hot-reload of `glibre.content`
   never moves the importer's code. The digest moves only on the
   release boundary that ships a new cook tool — at which point the
   §7.2.2 full-re-cook rule applies.
3. **Telemetry (§10.5 below).** The digest is logged with every
   importer error so post-mortem triage can identify which SDK / params
   vocabulary version produced the failure.

Why three components and not more:

- **Why include the SDK tag.** Autodesk's FBX SDK changes parser
  behavior across versions (e.g. animation-curve key interpolation
  changed between 2024 and 2025). Bumping the SDK is a re-cook
  trigger.
- **Why include `normalize-vN`.** A change to the axis convention,
  unit-scale handling, or skinning-encoding vocabulary changes every
  cook output. Re-cook trigger.
- **Why include `post-vN`.** Reserved; MVP = `post-v0`. The slot
  exists so a future add-on (e.g. mikktspace tangent re-derivation
  toggle) can mark its bumps without reaching into the SDK or the
  normalize component.
- **Why not include compiler / clang-version / libc++ flags.**
  PHILOSOPHY §7 (determinism) is the engine-wide commitment; the
  build system is responsible for byte-deterministic codegen across
  the supported toolchain matrix. Including the compiler hash would
  re-cook on every patch upgrade with zero semantic change — wrong
  trade-off; consistent with `fory-codegen.md` ABI-hash component
  list (schema sources only, no compiler identity).

### 3.3 `SourceAsset` reception — the dispatch contract

`FbxImporter::import_one` (SPEC §5.4) receives the `SourceAsset` by
const reference. The dispatch contract is:

1. **Closed-sum dispatch.** `importers/dispatch.cpp` (SPEC §6.1)
   compile-time-routes `SourceKind::Mesh + MeshFormat::Fbx` to
   `FbxImporter::import_one`. Other kinds are unreachable here (the
   match is exhaustive at compile time per SPEC §4.1.2 inv #1).
2. **Path-scope re-validation.** The cook session already validated
   the path (SPEC §4.1.1 inv #1) when constructing the `SourceAsset`;
   the importer additionally asserts that `SourceAsset.path` is
   non-empty and that `SourceAsset.format.mesh == MeshFormat::Fbx`
   under `GLIBRE_DEBUG`-only `assert`. In release the assertion is
   elided — the dispatch table is compile-time-exhaustive.
3. **`source_hash` is opaque.** The importer treats
   `SourceAsset.source_hash` as a 32-byte tag carried for telemetry
   (§10.5 below); it does not re-hash the file contents. Re-hashing
   would defeat the cook session's stage-1 hash capture (SPEC §6.2)
   and cost wall-clock proportional to the source size.
4. **`NormalizeParams.canonical_bytes` carries the per-cook
   parameter set.** The bytes are the **canonical, sorted-key,
   length-prefixed** serialization of the NormalizeParams record
   defined in §3.4 below. The importer typed-decodes the bytes via
   a generated header `glibre/content/fbx_normalize_params.hpp`
   (codegen rule §6 below); it does NOT re-canonicalize on the
   cook path.

### 3.4 `NormalizeParams` for FBX — the parameter vocabulary

`NormalizeParams.canonical_bytes` (SPEC §5.4) carries the per-cook
parameter set for the FBX importer. The vocabulary fields:

| Field                   | Type    | MVP default            | Effect                                                                                                                    |
|-------------------------|---------|------------------------|---------------------------------------------------------------------------------------------------------------------------|
| `vocab_version`         | `u32`   | `1`                    | The vocabulary version. Component of `importer_version`'s `NORMALIZE_TAG` per §3.2. Bump on every breaking schema move.    |
| `axis_up`               | `enum`  | `YUp`                  | Engine-canonical up axis. Closed sum {`YUp`, `ZUp`}. The importer rotates the scene if the FBX file declares a different axis (`FbxAxisSystem`). |
| `axis_forward`          | `enum`  | `NegativeZ`            | Closed sum {`NegativeZ`, `PositiveZ`, `NegativeX`, `PositiveX`}. Combined with `axis_up` defines the engine's RH frame.  |
| `unit_scale_meters`     | `f32`   | `1.0`                  | Multiplier from FBX scene units to engine meters. Imported by reading the scene's `FbxSystemUnit`; the field overrides for unusual sources. |
| `uv_layer_diffuse`      | `u8`    | `0`                    | Index of the UV layer to emit as the primary texture-coordinate stream. FBX permits up to 8 layers; MVP uses 0.            |
| `uv_layer_lightmap`     | `u8`    | `1`                    | Index of the UV layer to emit as the secondary (lightmap) stream. `255` = "no lightmap layer; emit zero stream".          |
| `skin_max_influences`   | `u8`    | `4`                    | Per-vertex skin influence cap. Closed sum {`4`, `8`}. MVP = 4 (matches the engine's vertex layout's 4-weight slot).        |
| `skin_normalize_weights`| `bool`  | `true`                 | Re-normalize the per-vertex skinning weights so they sum to `1.0` after capping. False = preserve raw FBX weights.        |
| `triangulate`           | `bool`  | `true`                 | Triangulate quads / n-gons via FBX SDK's `FbxGeometryConverter::Triangulate`. False = refuse non-triangle meshes.          |
| `recompute_normals`     | `enum`  | `IfMissing`            | Closed sum {`IfMissing`, `Always`, `Never`}. `IfMissing` = use FBX-baked normals when present, else compute via SDK.       |
| `recompute_tangents`    | `enum`  | `Never`                | Closed sum {`Never`, `MikkTSpace`}. MVP = `Never` (tangent-space generation is the geometry plugin's concern, §3.6).       |
| `weld_position_epsilon` | `f32`   | `0.0`                  | Position-weld epsilon in meters. `0.0` = no welding; positive value = SDK's `FbxGeometryConverter::Recover…WeldVertices`.   |
| `import_animations`     | `bool`  | `false`                | MVP = `false` (animation-curve ingest deferred per §3.6, §12 OQ-2). Reserved field for the post-MVP path.                  |
| `import_morph_targets`  | `bool`  | `false`                | MVP = `false`. Reserved field; same path as `import_animations`.                                                          |

The canonical-bytes encoding is **sorted-key, length-prefixed**:
each field name is encoded as a `u8` length followed by ASCII bytes,
followed by a `u8` typetag, followed by the canonical bytes of the
value. Boolean → `u8`; enums → `u8`; `u32`/`f32` → little-endian. The
field set is sorted lexicographically by name before encoding so two
serializations of the same logical record produce byte-equal canonical
bytes (SPEC §4.1.3 inv #2). The encoding is generated by a small
codegen rule in `tools/glibre-cook/CMakeLists.txt` from a manifest
`plugins/content/cook/import/fbx_normalize_params.fory` (the `data`
plugin's Fory grammar; the codegen tool already knows how to produce
canonical-byte serializers per `fory-codegen.md` §"Migration
Mechanic"). The importer typed-decodes via the generated header.

Why these fields and not others:

- **Why explicit axis convention.** Different DCCs export with
  different axis defaults (Maya y-up RH, 3dsMax z-up, Blender y-up
  with z-forward). FBX records the source's axis system in
  `FbxAxisSystem`; without re-orientation, geometry would not match
  the engine's coordinate-system contract. The two-field shape
  (`up` + `forward`) covers every practical RH convention; the
  alternative (single enum over the 24 possible axis triplets) hides
  the structure.
- **Why two UV layers.** The MVP material model needs base texture
  coords + lightmap UVs (a long-term hint from the geometry plugin's
  rasterized-and-RT-equivalent commitment, `geometry/SPEC.md`). Three
  or more layers re-enter post-MVP behind the same vocabulary by
  bumping `vocab_version`.
- **Why 4-weight skin cap.** The engine's vertex layout has a 4×u8
  bone-index + 4×f32 weight slot per vertex. 8-weight is reserved
  (closed-sum admits it) for post-MVP characters that need higher
  fidelity at facial / cloth deformation joints; bumping the cap is
  a `vocab_version` bump.
- **Why MVP refuses animation / morph imports.** The
  `MeshArtifact` schema (SPEC §5.2, §7) carries no animation
  fields at MVP. Adding them requires authoring
  `data/schemas/content/AnimationClipArtifact.fory` and a separate
  `Importer` dispatch row (animation as its own SourceKind, or a
  sub-precursor inside the mesh path). Both options re-enter
  post-MVP per §12 OQ-2.

### 3.5 SDK seam — `FbxManager`, `FbxIOSettings`, `FbxScene`, `FbxImporter`

The FBX SDK's class topology and lifetime contracts dictate the
importer's structure:

```text
FbxManager     (root; allocates everything; one per thread is the
                only thread-safe topology — Autodesk docs explicit on
                this. Live for the FbxImporter aggregate's entire
                lifetime.)
├── FbxIOSettings   (one per FbxManager; controls importer flags;
│                    configured at FbxImporter::create() time —
│                    triangulate, materials, animations, etc.)
├── FbxScene        (per-cook; allocated per import_one call;
│                    populated by FbxImporter::Import; destroyed at
│                    end of import_one. Memory comes from the
│                    importer's per-cook arena via the SDK's
│                    FbxMemoryAllocator hook.)
└── FbxImporter     (the SDK's importer object — distinct from our
                     glibre::content::FbxImporter aggregate. To
                     disambiguate, the SDK type is referenced as
                     ::fbxsdk::FbxImporter throughout the
                     implementation; our aggregate stays in
                     glibre::content::FbxImporter.)
```

**Disambiguation rule.** In `fbx_importer.cpp` the qualified name
`::fbxsdk::FbxImporter` always names the SDK class; the unqualified
name `FbxImporter` (in our namespace) names the aggregate. The
header pulls in the SDK's headers via a private-include directive:

```cpp
// fbx_importer.cpp (private; -fexceptions on this TU only)
#include <fbxsdk.h>            // pulls FbxManager, FbxScene, ...
#include "fbx_importer.hpp"    // our aggregate; -fno-exceptions header
```

`FbxManager` allocation is **per-importer-instance** (i.e. per worker
thread). The `create()` static (SPEC §5.4) instantiates one
`FbxManager` via `FbxManager::Create()` (which throws on allocation
failure — caught at the carve-out and translated to
`ImporterError::MalformedPayload` with `error.detail = "fbx-bootstrap"`
per §3.7 step 6 below). The destructor (`~FbxImporter`) calls
`FbxManager::Destroy()` to release every SDK object the manager
allocated. Static-init ordering of the SDK's internal globals is
respected by the `FbxManager::Create()` factory pattern; no
`FbxManager` is ever taken across thread boundaries.

**Memory allocator hook.** The FBX SDK exposes
`FbxSetMemoryAllocator(const FbxMemoryAllocator*)` (a static, on the
SDK side). The importer installs a process-wide allocator at first
`create()` call that routes every SDK allocation through
`glibre::PerContextAllocator` tagged `ContextTag::content`. The
allocator records the requesting `FbxManager` pointer so the soft
sub-ceiling (SPEC §9.3.1, 64 MiB) can be enforced per-importer-cook.
The hook is set exactly once per process; subsequent `create()` calls
verify the existing hook is the engine's and skip re-installation.
This collapse pattern matches `fory-codegen.md`'s middleman: one
shared SDK allocator per process, scoped per cook by the arena's
tagging.

**Why per-thread `FbxManager`.** Autodesk's FBX SDK programmer's guide
explicitly states that `FbxManager` must not be shared across threads
during scene traversal; serializing through one manager would block
the cook worker pool. One-per-thread admits parallel cook of
independent FBX sources without lock contention. The cook worker
pool sized from `perf-budget.md`'s content cell (§9.3.1; ~4 workers)
holds 4 `FbxImporter` instances, each owning its own `FbxManager`.
Memory cost: ~few-hundred KiB per manager × 4 ≈ ~1 MiB peak from the
content tag's 256 MiB ceiling — small.

### 3.6 Sub-asset boundaries — what the FBX path includes / refuses

An FBX file can carry many sub-assets in one container: meshes,
skeletons, animations, materials, cameras, lights, textures (embedded
or referenced), morph targets. The aggregate's MVP decision per
sub-asset:

| FBX sub-asset                                     | MVP disposition                                                                                                                                                                                                                                              |
|---------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `FbxMesh` (control points, polygons, UVs)         | **Imported.** Becomes the `MeshArtifact` precursor's vertex / index streams. Triangulated per §3.4 `triangulate=true`. Multiple `FbxMesh` instances under one root produce one `MeshRecord` per instance with named identity (the FBX node name).             |
| `FbxNode` (scene-graph node + transform)          | **Imported.** Becomes the `MeshArtifact` precursor's `nodes_` array; preserves parent / child topology and local-space transforms. Engine-canonical axis convention applied per §3.4.                                                                          |
| `FbxSkeleton` (bone)                              | **Imported.** Becomes the precursor's `skeleton_` array; bind-pose only at MVP.                                                                                                                                                                                |
| `FbxCluster` (skin binding)                       | **Imported.** Skin weights resolved per `skin_max_influences` (§3.4); per-vertex `(bone_index[N], weight[N])` written into the precursor.                                                                                                                       |
| `FbxAnimStack` / `FbxAnimLayer` / `FbxAnimCurve`  | **Refused (MVP).** §3.4 `import_animations=false`. Reserved field; post-MVP behind a separate Fory schema authored under a future spike (§12 OQ-2). The SDK's animation tree is left un-traversed; the importer asserts no curves leak into the precursor.   |
| `FbxBlendShape` (morph targets)                   | **Refused (MVP).** §3.4 `import_morph_targets=false`. Same path as animations.                                                                                                                                                                                 |
| `FbxCamera` / `FbxLight`                          | **Refused.** Out of context (rendering / scene composition concerns). Path: emit a once-per-cook `info`-level log entry naming the unhandled sub-asset for tooling visibility; do not error.                                                                  |
| `FbxMaterial` / `FbxSurfaceMaterial`              | **Indirected (no decode).** The importer captures the material's name and any referenced texture file paths into the precursor's `material_refs` field. Texture decode is the `FreeImageImporter`'s job (sibling spike #810); the cook session schedules it as a sibling `RecookRequest` driven by the resulting `DependencyEdge`. Material parameter values (PBR factors, etc.) are deferred to the future `material` plugin (§12 OQ-3). |
| Embedded textures (`FbxFileTexture` with embedded media) | **Refused.** MVP requires textures to live as external files under `assets/source/` so the watcher seam (SPEC §4.1.10) can drive their hot-reload. Embedded media in FBX is rejected with `ImporterError::UnsupportedVersion` and `error.detail = "fbx-embedded-media"`. Operator action: re-export the source FBX with `FbxFileTexture::SetUseEmbedded(false)`, ship the texture as a sibling file. |
| External texture references (`FbxFileTexture` with disk path) | **Indirected.** The path is canonicalized against `assets/source/` (the source-tree root, SPEC §4.1.1 inv #1); paths outside the root are `ImporterError::SourceNotFound`. The canonical path is recorded in the precursor's `material_refs[i].texture_path` field; the cook session emits a `DependencyEdge` so a later texture change re-cooks the parent mesh's material binding. |
| `FbxLayerElementVertexColor`                      | **Imported, optional.** If present, the per-vertex color channel is emitted as a fourth vertex stream; `255`-default if absent.                                                                                                                                |
| `FbxLayerElementSmoothing` / `FbxNormal` policy   | **Imported, normalized.** Smoothing-group flags drive the SDK's per-vertex normal computation when `recompute_normals=IfMissing` and the FBX file lacks a baked normal layer. Otherwise the baked layer wins. The choice is deterministic per source bytes.    |
| `FbxLayerElementTangent` / `FbxLayerElementBinormal` | **Imported when present, never recomputed at MVP.** §3.4 `recompute_tangents=Never`. The geometry plugin owns mikktspace re-derivation if needed (per its design doc, `geometry/cluster-dag-design.md`).                                                  |

The disposition is closed: any sub-asset class not listed above is
ignored with a once-per-cook `debug`-level log entry. Adding a class
to the imported set requires bumping `vocab_version` per §3.4 and
authoring the precursor-field shape — a deliberate central edit, not
a runtime branch (PHILOSOPHY §6, §10).

### 3.7 Normalize pipeline — the seven ordered stages inside `import_one`

`FbxImporter::import_one(SourceAsset, NormalizeParams, ImporterArena&,
CancellationToken&)` runs **seven** ordered stages, each documented
inline against a SPEC invariant. Stages 1–6 run inside the
`-fexceptions` carve-out's `try` block (one block, wrapping the entire
body); stage 7 runs after the `catch` epilogue and is the SDK
teardown phase. The `try` / `catch` pair is the unique exception
translation site (SPEC §10.3).

**Stage 1 — SDK importer creation + magic / version probe**
(SPEC §4.1.2 inv #2). Allocate an `::fbxsdk::FbxImporter*` via
`::fbxsdk::FbxImporter::Create(manager_, "")`; configure
`FbxIOSettings` per `NormalizeParams`; call `Initialize(path,
-1, io_settings_)`. Failure modes:

- `Initialize` returns `false` with status `eInvalidFile` →
  `ImporterError::MagicMismatch`, `error.detail = "fbx-magic"`.
- Status `eInvalidFileVersion` → `ImporterError::UnsupportedVersion`,
  `error.detail = "fbx-version"`. The SDK's reported version pair is
  embedded in the structured-log fields as
  `error.fbx_file_version` / `error.fbx_sdk_min_version`.
- Status `eFileNotFound` → `ImporterError::SourceNotFound`,
  `error.detail = "fbx-not-found"`. (Should not normally fire — the
  cook session validates the path at stage 1; this is the
  watcher-debounce-vs-delete race carve-out.)
- Status `eFileCorrupted` → `ImporterError::MalformedPayload`,
  `error.detail = "fbx-corruption"`.

The byte-offset for R-12.1.4 errors is `FbxStatus::GetLocation()`
when the SDK reports it; otherwise zero. The structured log records
`error.byte_offset` regardless.

**Stage 2 — Scene import** (SDK side-effect; must be inside the
arena). Allocate `::fbxsdk::FbxScene::Create(manager_, "scene")`;
call `fbx_importer_->Import(scene_)`. The SDK populates the scene
graph synchronously. Failure modes match stage 1's classification
(SDK-thrown exceptions during traversal land in stage 6's `catch`).
Cancellation is **not** observable inside `Import`; the call is
atomic from our perspective — if cancellation arrived, stage 4 is
the first poll point. Bound: ~50–500 ms wall-clock for a ~5 MiB FBX
on the cook worker thread (off the game-loop driver thread per
SPEC §9.3.1).

**Stage 3 — Axis / unit / scene-pre-process normalization**
(SPEC §4.1.2 inv #3 — pure; no I/O outside arena). Apply:

- `FbxAxisSystem::ConvertScene(scene_, target)` where `target` is
  built from `NormalizeParams.axis_up` + `axis_forward`. The
  SDK's `ConvertScene` modifies the scene's root transform; the
  importer's later traversal sees an axis-converted scene.
- `FbxSystemUnit` scale conversion via
  `target_unit.ConvertScene(scene_)` where `target_unit` is built
  from `NormalizeParams.unit_scale_meters`.
- `FbxGeometryConverter::Triangulate(scene_, /*replace=*/true)` if
  `triangulate=true`; refuse non-triangle meshes otherwise with
  `ImporterError::MalformedPayload` and
  `error.detail = "fbx-non-triangle"`.

These three SDK calls are pure of effect outside `scene_` and the
arena; deterministic per source bytes (PHILOSOPHY §7) — the SDK's
implementations are documented as reproducible across runs on the
same SDK version. Cross-host determinism follows from the SDK
linking against deterministic libxml2 / zlib (the build system's
toolchain pin).

**Stage 4 — Scene-graph walk + cancellation poll**
(SPEC §4.1.2 inv #5). Recursive walk over `scene_->GetRootNode()`;
at every `FbxNode` boundary, poll `cancel.is_cancelled()`. On
observed cancellation, immediately return
`ImporterError::Cancelled` with `error.detail = "fbx-cancelled"`;
the arena cleanup is the destructor's job (the arena's lifetime is
the cook session, not `import_one`'s; SPEC §4.1.9 lifetime). Per
node:

1. Resolve the node kind (`FbxMesh`, `FbxSkeleton`, etc.) per the
   §3.6 disposition table.
2. For `FbxMesh`: extract control points (positions), index
   buffer (post-triangulation), per-vertex normals (per
   `recompute_normals` + smoothing-group resolution), per-vertex
   tangents/binormals (raw if present, never recomputed), per-vertex
   UVs (layers `uv_layer_diffuse` / `uv_layer_lightmap`), per-vertex
   colors (if present), per-vertex skin weights (resolved per
   `skin_max_influences`).
3. For `FbxSkeleton`: extract bone name, parent index, bind-pose
   transform.
4. For `FbxFileTexture`: canonicalize path against
   `assets/source/` and append a `MaterialRef` to the
   `material_refs_` array.
5. Append per-class records (`MeshRecord`, `BoneRecord`, etc.) to
   the scratch arrays.

Bound: ≤ ~200 nodes per ~5 MiB FBX (S3 fixture); total cancellation
latency ≤ ~1 ms because polling-per-node is cache-cheap (one atomic
load per node).

**Stage 5 — Determinism canonicalization**
(SPEC §4.2 inv #1; PHILOSOPHY §7). After the walk:

- Sort `nodes_` / `meshes_` / `skeleton_` / `vertex_streams_` /
  `index_streams_` / `material_refs_` arrays by stable key
  (`FbxNode` name, then position-in-parent for ties). FBX scene-graph
  iteration order is documented as preserving file order on the same
  SDK version, but the explicit sort defends against any future SDK
  change and against host-dependent traversal cost models.
- Canonicalize `MaterialRef.texture_path` to the workspace-relative
  form under `assets/source/`. Paths outside the root are
  `ImporterError::SourceNotFound` (already detected at stage 4 —
  this step is a defense-in-depth assert).
- Quantize / canonicalize floating-point fields: refuse `NaN` /
  `signaling NaN` / `denormal` per `fory-codegen.md` Open Question 5
  ("ban NaN payloads at deserialize time"). The importer extends the
  rule to **encode time** as well: a `NaN` position component is
  `ImporterError::MalformedPayload`,
  `error.detail = "fbx-nan-position"`.
- Tag-sort the per-vertex skinning weight pairs so two cooks of the
  same mesh produce identical bone-influence ordering.

**Stage 6 — Precursor emit**. Write the precursor bytes into the
arena via a generated emitter
(`glibre/content/mesh_artifact_precursor.hpp`, the Fory codegen
output's pre-Fory CPU-side struct). The emitter is a straight-line
walk over the canonicalized arrays from stage 5; no allocation outside
the arena. The precursor's CPU-side layout is **distinct** from the
Fory envelope shape (which the cook step's stage-4 produces — SPEC
§6.2 stage 4); the importer never emits Fory bytes itself.

The returned `eastl::span<const std::byte>` points into the arena;
the span is valid for the cook session's lifetime per SPEC §9.3.1
(arena drains between cooks, not between frames). The cook step's
stage-4 (`fory-serialize`) consumes the span and produces the
`CookedAsset.payload` (SPEC §4.1.4); the cook-session's stage-5
writes the payload to the CAS (SPEC §4.1.5).

**Stage 7 — SDK teardown** (after the `try` / `catch`). Call
`fbx_importer_->Destroy(true)`; call `scene_->Destroy(true)`. The
manager-owned SDK objects are released; the arena's claim on the
scene's bytes is released by the arena drain at session end. The
teardown is unconditional — runs on success and on the `catch`-side
error path — to guarantee no SDK object leaks into the next cook.

### 3.8 Concurrency model — one `FbxImporter` per worker, no shared mutable state

The aggregate is **stateless across cooks** (SPEC §4.1.2 lifetime).
The cook worker pool (§4 worker pool budget) holds N `FbxImporter`
instances, one per worker thread. Each instance owns:

- One `FbxManager` (per the SDK's thread-affinity rule, §3.5).
- One `FbxIOSettings` (immutable post-`create`).
- One pimpl `Impl*`.

There is **no shared state** across importer instances except the
process-wide `FbxMemoryAllocator` hook (§3.5), which is itself
thread-safe by construction (it routes to `glibre::
PerContextAllocator` whose tag-bookkeeping uses atomic counters per
`perf-budget.md` Allocator Rule #1).

Per-cook concurrency:

- One worker thread runs `import_one` start-to-finish; the call is
  not internally parallelized. Parallelism across multiple
  `RecookRequest`s is the `CookSession`'s concern (SPEC §4.1.9
  composition; one importer instance per worker).
- The arena handed to `import_one` is **per-call**; arenas are not
  shared across importer calls in the same worker (each session
  allocates a fresh per-cook arena and resets at end-of-session
  per SPEC §9.3.1 rule 3).

The frame-loop driver thread **never** runs `import_one`. The
`perf-budget.md` content cell's 0.50 ms is steady-state on the
driver thread (residency tickle + handle resolve only); cook work
is off-thread per SPEC §9.3.1.

### 3.9 In-memory layout of the `MeshArtifact` precursor

The precursor (the bytes `import_one` returns) is the input to
the cook step's Fory-encode stage. Its in-memory layout is **not**
the Fory envelope shape; it is a packed CPU-side struct emitted by
the codegen rule under `data/codegen-output/include/glibre/types/
content/mesh_artifact_precursor.hpp` (per `fory-codegen.md` rule —
the codegen tool emits a pre-Fory CPU struct alongside the Fory
struct for any persistent type the engine wants a non-Fory in-memory
form of). The shape:

```text
MeshArtifactPrecursor {
  vocab_version  : u32                  // copy of NormalizeParams.vocab_version; lets the cook step's
                                         // Fory encoder reject precursor / Fory-schema vocabulary mismatches.
  meshes         : list<MeshRecord>     // sorted by (node_name, position_in_parent).
  nodes          : list<NodeRecord>     // sorted by node_name; preserves parent-index references.
  skeleton       : list<BoneRecord>     // sorted by bone_name.
  material_refs  : list<MaterialRef>    // sorted by material_name.
  vertex_streams : list<VertexStream>   // dense per-vertex layouts; one per mesh; deterministic order.
  index_streams  : list<IndexStream>    // u32 indices; triangulated.
}

MeshRecord {
  node_name      : eastl::string_view  // borrowed view; backing memory is in the arena.
  vertex_stream_idx : u32              // index into precursor.vertex_streams.
  index_stream_idx  : u32              // index into precursor.index_streams.
  material_ref_indices : list<u32>     // indices into precursor.material_refs (per-submesh).
}

VertexStream {
  positions      : list<vec3f>         // engine-canonical RH y-up; meters.
  normals        : list<vec3f>         // optional (zero-length if recompute_normals=Never AND no baked).
  tangents       : list<vec3f>         // optional.
  binormals      : list<vec3f>         // optional.
  uvs_diffuse    : list<vec2f>         // empty if uv_layer_diffuse layer absent.
  uvs_lightmap   : list<vec2f>         // empty if uv_layer_lightmap == 255 OR layer absent.
  colors         : list<u32>           // RGBA8 packed; default 0xFFFFFFFF if FBX layer absent.
  skin_indices   : list<u32>           // 4× u8 packed into u32; (skin_max_influences == 8 → 2× u32 per vertex).
  skin_weights   : list<vec4f>         // (skin_max_influences == 8 → 2× vec4f per vertex).
}

IndexStream {
  indices : list<u32>                  // triangulated; 3-vertex strides.
}

NodeRecord {
  node_name        : eastl::string_view
  parent_idx       : u32  // 0xFFFFFFFF == root.
  local_translation: vec3f
  local_rotation   : quatf
  local_scale      : vec3f
  mesh_idx         : u32  // 0xFFFFFFFF if node carries no mesh.
}

BoneRecord {
  bone_name             : eastl::string_view
  parent_idx            : u32  // 0xFFFFFFFF == root bone.
  bind_pose_translation : vec3f
  bind_pose_rotation    : quatf
  bind_pose_scale       : vec3f
}

MaterialRef {
  material_name  : eastl::string_view
  texture_paths  : list<eastl::string_view>  // workspace-relative; one per texture slot.
}
```

**Why `eastl::string_view` over owning strings.** The precursor
bytes live entirely inside the arena; views into the arena are
valid for the precursor's lifetime. Owning strings would
double-allocate the same bytes. The cook step's Fory-encode stage
copies the views into the Fory envelope's owning representation
(SPEC §6.2 stage 4); after that, the arena drain reclaims the
view's backing memory.

**Why the CPU-side struct is distinct from the Fory schema.** The
Fory schema (`data/schemas/content/MeshArtifact.fory`) drives the
on-disk envelope shape — tag-sorted fields, length prefixes,
tag-numbered for ABI evolution. The CPU-side struct is the
**fast-path in-memory form** with cache-coherent layouts (every
`list<T>` is an `eastl::vector<T, ImporterArenaAllocator>` of
contiguous bytes). Fory's encoding step is a deterministic transform
between the two; the importer produces the CPU-side form because
that is what the geometry plugin's cook-step API consumes (per
geometry's `cluster-dag-design.md` cook seam).

Field alignment: every numeric field is naturally aligned (vec3f at
4-byte alignment, quatf at 4-byte alignment, u32 at 4-byte). The
generated emitter pads to the natural alignment; no bit-packed
fields. This keeps the precursor cache-friendly for the geometry
plugin's downstream meshlet partition / LOD pass.

### 3.10 Lifetime — `create` / per-call / `~FbxImporter`

The lifetime contract:

```text
CookSession::begin(...)               (SPEC §5.12; cook session opens)
  ↓
  for each worker thread W in pool:
    FbxImporter::create()             (SPEC §5.4; once per worker)
      ↓ allocates FbxManager, FbxIOSettings, installs FbxMemoryAllocator
        if first call in process.
  ↓
  for each RecookRequest assigned to W (one at a time):
    FbxImporter::import_one(           (SPEC §5.4; many calls per importer)
      src, params, arena, cancel)
      ↓ stages 1..7 above; arena scratch for the call.
  ↓
  ~FbxImporter()                       (cook session ends or worker retires)
    ↓ FbxManager::Destroy() frees every SDK object.
```

`create` returns `Result<eastl::unique_ptr<FbxImporter>>` (SPEC §5.4
verbatim). The smart pointer is an `eastl::unique_ptr` per
PHILOSOPHY §11 (no `std::unique_ptr` in engine code); the deleter
is the standard EASTL deleter that calls `~FbxImporter`.

The `import_one` call is **synchronous** — returns when the
precursor is emitted or an error fires. No async future, no
coroutine resumption (PHILOSOPHY: no coroutines in engine code).

The `~FbxImporter` body:

1. Destroy the SDK importer (`fbx_importer_->Destroy(true)`),
   defensive — `import_one`'s stage 7 already did this on every
   path, so this is a no-op on the happy path; but if `~FbxImporter`
   runs while an `import_one` is "in flight" (cancellation cleanup
   path or surprising session abort), the Destroy here drains any
   leaked SDK state.
2. Destroy the scene (defensive; same reasoning).
3. Destroy the `FbxIOSettings`.
4. Destroy the `FbxManager` — releases every remaining SDK
   allocation back through the `FbxMemoryAllocator` hook, returning
   bytes to the per-cook arena (which is itself drained at session
   end; SPEC §9.3.1 rule 3).
5. Reset the pimpl.

The destructor is `noexcept` (PHILOSOPHY: every public destructor in
engine code is noexcept; the carve-out is the importer's `import_one`
body, not its destructor). Any SDK-thrown exception during `Destroy`
calls is logged at `error` and swallowed; the destructor must not
escape exceptions.

## 4. Public surface

The §5.4 stub is the only public C++23 header surface the importer
exports. Reproduced here for cross-reference; **this design does not
modify the stub**.

```cpp
// SPDX-License-Identifier: Apache-2.0
// content/include/glibre/content/importer.hpp — fbx-importer slice.
//
// Locked in specs/content/SPEC.md §5.4. -fno-exceptions header;
// the implementation TU (fbx_importer.cpp) carries the unique
// -fexceptions carve-out per §10.3 / reviews/decisions/error-model.md
// §"Decision" rule 3.

namespace glibre::content {

class FbxImporter final : public Importer {
public:
    // Allocates the SDK manager + io_settings; installs the
    // FbxMemoryAllocator hook on first call in the process. Per-thread
    // instance; not safe to share across worker threads (FBX SDK
    // thread-affinity rule, §3.5).
    [[nodiscard]] static auto create() noexcept
        -> Result<eastl::unique_ptr<FbxImporter>>;

    // Read the mesh source, normalize per `params`, emit the
    // `glibre.content.MeshArtifact` precursor bytes into `out_arena`.
    // SDK exceptions translated at the carve-out boundary into
    // `ImporterError::*` arms; never escape.
    //
    // Cancellation (§3.7 stage 4) polled at every FbxNode boundary;
    // observed cancellation returns `ImporterError::Cancelled`
    // promptly (§4.1.2 inv #5).
    //
    // The returned span points into `out_arena` and is valid for
    // the arena's lifetime (cook session; §9.3.1 rule 3).
    [[nodiscard]] auto import_one(const SourceAsset&        src,
                                  const NormalizeParams&    params,
                                  ImporterArena&            out_arena,
                                  const CancellationToken&  cancel) noexcept
        -> Result<eastl::span<const std::byte>>;

    ~FbxImporter();

private:
    FbxImporter() noexcept;
    struct Impl;
    Impl* impl_{nullptr};
};

}  // namespace glibre::content
```

Pending §5.4 amendment (out-of-scope for this design; routed to
follow-up §12 OQ-1):

- `NormalizeParams.canonical_bytes` (SPEC §5.4) is currently a
  generic `eastl::span<const std::byte>` blob. The §3.4 vocabulary
  defines the typed schema the bytes carry. A future amendment may
  add a typed `NormalizeParamsView` adaptor that decodes the bytes
  into the §3.4 fields, exported through the same header. The body
  of the typed view lives inside `data/codegen-output/include/
  glibre/types/content/fbx_normalize_params.hpp` (Fory-codegenerated)
  per `fory-codegen.md`; this design pins the vocabulary contract
  without importing the typed view into the §5.4 header (which would
  pull a Fory-codegen dependency into a header consumed by the
  runtime that does not need it).

The `Importer` base trait (SPEC §5.4) is a non-virtual base. The
closed sum admits exactly one implementation per `SourceKind` (§3.2
collapse #1). Generic cook-step code references `Importer&` and
dispatches through the free function `dispatch_import(...)` defined
in `importers/dispatch.cpp` (SPEC §6.1) — a compile-time switch on
`Importer::kind()` matched by `SourceKind`. There is no virtual
dispatch table.

`Importer::version()` returns the 32-byte `ImporterVersion` digest
(§3.2 above). Generic code consumes it without knowing the FBX
internals.

No second public header is added. Internal helpers
(`FbxMemoryAllocator` hook, the canonicalization tables, the §3.7
stage emitters) live behind the implementation TU's translation-unit
boundary.

## 5. Hot/cold path split

`fbx-importer` is a **build-time-only** aggregate. SPEC §6.5 names
the cuts: `importers/fbx_importer.{hpp,cpp}` is shipped only into
the `tools/glibre-cook` host executable, **not** into
`glibre-content.dylib`. The runtime cannot link the importer; calling
into `FbxImporter::*` from a shipping build is a link-time error
(undefined symbol), not a runtime branch. Every section below is the
cook-tool's profile; the runtime profile elides the entire aggregate.

### 5.1 Hot path — within one `import_one` call

Stages 1–7 (§3.7) form the import call's hot path. Stage-by-stage
profile:

| Stage              | Hot/cold | Bound (S3 = ~5 MiB FBX, M1 firestorm) | Dominant cost                                                       |
|--------------------|----------|---------------------------------------|---------------------------------------------------------------------|
| 1 SDK init         | Cold     | ≤ 5 ms                                 | One-time; `FbxImporter::Initialize` parses FBX header.               |
| 2 Scene import     | **Hot**  | ~ 50–500 ms                            | SDK scene-graph build; libxml2 parse; allocations through arena.     |
| 3 Axis/unit pre-process | Hot | ~ 5 ms                                 | `FbxAxisSystem::ConvertScene`, `Triangulate`.                        |
| 4 Walk + cancel poll | **Hot** | ~ 20–100 ms                          | Walk over ~200 nodes per S3; per-vertex skinning resolve.            |
| 5 Determinism canonicalization | Hot | ~ 10 ms                       | Sorts, NaN refusal, per-vertex skin tag-sort.                        |
| 6 Precursor emit   | Hot      | ~ 5 ms                                 | Straight-line memcpy into arena.                                     |
| 7 SDK teardown     | Cold     | ≤ 1 ms                                 | `Destroy` calls.                                                     |

S3 wall-clock totals to ~100–600 ms per cook on the worker thread —
**off the game-loop driver thread**. The 0.50 ms content cell
(`perf-budget.md`) is **not** charged for this; the 64 MiB importer
soft sub-ceiling (SPEC §9.3.1) is the relevant gate.

The CPU profile is dominated by stage 2 (SDK scene-graph build)
and stage 4 (scene-graph walk). Stage 2 is library-bound (Autodesk
implementation; we do not optimize here). Stage 4 is bounded by
the per-node walk plus per-vertex skinning resolution; both are
SIMD-friendly straight-line code.

### 5.2 Cold path — `create` and `~FbxImporter`

`create` runs once per worker thread per cook session opening
(§3.10). The first `create` in a process additionally installs the
process-wide `FbxMemoryAllocator` hook. Costs:

- `FbxManager::Create()`: ~few hundred μs of SDK init.
- `FbxIOSettings::Create(manager_, IOSROOT)`: <100 μs.
- Allocator hook install (first call only): one atomic store +
  bookkeeping.

Total: ≤ 5 ms first call, ≤ 1 ms subsequent calls. Cook sessions
typically begin once per editor save burst; this is amortised to
zero in steady state.

`~FbxImporter` runs once per worker per session close (§3.10). Cost:

- `FbxImporter::Destroy(true)` on the SDK importer (no-op on happy
  path; defensive on abort): ≤ 100 μs.
- `FbxManager::Destroy()`: ≤ few ms (releases all manager-owned
  allocations through the arena).

These are the only paths touched outside `import_one`; both run off
the game-loop driver thread (cook worker pool). The runtime never
runs them.

### 5.3 No runtime hot path

The runtime (`glibre-content.dylib`) exposes the §5 surface but
**never calls** `FbxImporter::*`. SPEC §6.5 cut row makes this a
build-time guarantee; SPEC §10.3's `-fexceptions` carve-out is
build-time-only (the runtime TUs all build with `-fno-exceptions`).
Any future runtime path that wanted to ingest FBX would re-introduce
SDK linkage into the runtime dylib — explicitly refused by SPEC
§3.3 (the runtime is a producer of cooked bytes, not a consumer of
sources). The §3.3 + §6.5 commitments together ensure the importer's
cost is bounded to the cook tool's profile.

## 6. Concurrency

### 6.1 Frame phase ownership

The FBX importer **does not run on the game-loop driver thread**.
SPEC §9.3.1 ("Off-thread soft ceiling — importer scratch arena")
pins this: cook orchestration runs entirely on the **importer worker
pool** outside the frame loop. The phase-by-phase contribution:

| Phase | Importer activity                | Notes                                                          |
|-------|----------------------------------|----------------------------------------------------------------|
| 1     | None                             | Watcher events translated by `WatchEdge` (SPEC §4.1.10) on the driver thread; no importer work yet. |
| 2     | None                             |                                                                |
| 3     | None                             |                                                                |
| 4     | None                             |                                                                |
| 5     | None                             |                                                                |
| 6     | None                             |                                                                |
| 7     | None                             |                                                                |
| 8     | None *directly*. Manifest swap happens here (SPEC §8.2 step 4); the importer's outputs (CookedAssets in CAS, manifest staging blob) were produced asynchronously and parked on the cook session's queue. | The cook session's `commit()` returns a parked publish; the loader's phase-8 step performs the actual `rename(2)` (SPEC §6.2 manifest-publish paragraph). The importer is not involved in the swap itself. |
| 9     | None                             |                                                                |

The importer's runtime cost on the driver thread is **literally
zero**; SPEC §9.3.2 ("Hot-path operations — exhaustive list") confirms
this — the three driver-thread hot operations (residency-state
evaluation, hot-reload check, AssetHandle resolution) do not touch
the importer.

### 6.2 Worker pool topology

The cook worker pool is owned by `cook/worker_pool.cpp` (SPEC §6.1).
Topology:

- **Pool size**: 4 workers at MVP. Sized from `perf-budget.md`'s
  `content` row "one-shot import work is off-thread"; the precise
  count is `cores − 1` clamped to `[2, 4]` so the cook tool does not
  starve the editor's UI thread on a 4-core CI host. Final number
  is the implementation plan's call (per `task-breakdown-content-
  fbx-importer-detailed`); this design pins the **shape** (one
  `FbxImporter` per worker) and the **upper bound** (≤ 4) for the
  perf budget §9.3.1's 64 MiB soft sub-ceiling.
- **One `FbxImporter` per worker**, allocated at session start
  (§3.10), destroyed at session end. The aggregate is **per-thread,
  not shared**.
- **One `ImporterArena` per worker per cook**: the cook worker's
  per-call arena handed into `import_one` (§3.10).
- **No work-stealing across workers' `import_one` calls.** The cook
  session schedules one `RecookRequest` per worker slot at a time;
  contention is only at the queue boundary (`cook/topology.cpp`'s
  topo-sort, SPEC §4.1.9 inv #3).

The pool is **persistent** across cook sessions inside one process
lifetime (the cook tool runs many sessions per editor invocation).
Workers retire only at process shutdown or `glibre_plugin_drain` for
a content-plugin reload (SPEC §8.3 row "In-flight RecookRequest
queues / CookSession worker pools" — destroyed by drain, re-spawned
by register).

### 6.3 Allocators

Per `perf-budget.md` Allocator Rule #1 + SPEC §9.3.1:

- **Per-context tag**. Every importer allocation is stamped with
  `ContextTag::content`. The `FbxMemoryAllocator` hook (§3.5) routes
  every SDK allocation through `glibre::PerContextAllocator` tagged
  `ContextTag::content`. EASTL containers used by the importer
  (`eastl::vector<T, ImporterArenaAllocator>`) carry the same tag
  through their allocator type.
- **Soft 64 MiB sub-ceiling**. Per SPEC §9.3.1 rule 2; the cook
  scratch arena breaches the ceiling at `warn` level in shipping
  builds and `OutOfBudget` in `GLIBRE_ALLOC_STRICT=1` builds. Single
  ~5 MiB FBX with skinning + LOD precursor allocates ~30–50 MiB
  arena peak; 64 MiB headroom is the smallest gate that admits S3
  without surprise refusals. A larger source (~50 MiB) will trip
  the soft warn — the operator is expected to split the source
  before commit.
- **Multi-frame, off-thread arena lifetime**. Per SPEC §9.3.1 rule
  3, the arena drains between cooks (at session end) — **not**
  between frames. The transient-arena exemption (Allocator Rule 4)
  does **not** apply. The 64 MiB counts against the 256 MiB
  `content` heap cell (Allocator Rule 1).
- **No raw `new` / `malloc`**. Per `perf-budget.md` Allocator Rules
  header + SPEC §9.4 rule 7. The build rejects raw allocations in
  `plugins/content/cook/import/`. The FBX SDK's vendor-internal
  `malloc` is the third-party-wrap exemption (§4.1.2 inv #3), routed
  through the `FbxMemoryAllocator` hook; this is the only third-party
  allocator carve-out in the engine.

### 6.4 Cancellation propagation

SPEC §4.1.2 inv #5 + SPEC §4.1.9 inv #5 require cancellation to
propagate within ~one frame's wall-clock. The mechanics:

1. `CookSession::cancel()` sets the session's `CancellationToken`
   to `cancelled = true` (atomic store, relaxed).
2. The worker thread's `import_one` polls `cancel.is_cancelled()`
   at every `FbxNode` boundary (§3.7 stage 4). Polling cost: one
   `memory_order_acquire` atomic load per node, ~few ns; a ~200-node
   FBX adds ~1 μs of polling overhead total — invisible.
3. Observed cancellation returns `ImporterError::Cancelled` from
   `import_one` immediately; the worker thread proceeds to the
   `~FbxImporter` path (or to its next dequeue if the importer
   instance is reused).
4. The arena drain (at session abort) reclaims any in-flight
   allocations.

Bound: ≤ ~1 ms wall-clock from `cancel()` to `import_one` returning
on a ~200-node FBX. Stage 2 (SDK scene-import) is the lone
non-cancellable point — it is atomic from our perspective. The
cancellation latency is therefore bounded by stage 2's bound (~50–500
ms for S3), which exceeds one frame; this is the operational reality
the spike #146 brief calls out and is consistent with SPEC §9.3.1's
"the cook is multi-frame" statement.

### 6.5 Off-thread invariants

Two invariants govern the off-thread topology:

1. **No importer call ever runs synchronously on the driver thread.**
   The cook session's `begin` / `enqueue` / `commit` / `cancel`
   methods may be called from the driver thread (editor flow, watch
   event flow), but they enqueue work; the actual `import_one`
   dispatch runs on the worker pool. SPEC §4.2 inv #8.
2. **The driver thread never holds a reference to importer state
   that requires synchronization with a worker.** The cook session
   parks results on a queue; the driver thread polls the queue at
   phase 8 entry (loader's drain step) for staged manifest blobs.
   The queue's SPSC discipline matches the `WatchEdge` SPSC pattern
   (`platform/file-watcher-design.md` §3.2): one producer (the
   worker pool aggregate), one consumer (the loader's phase-8
   reader). No locks on the driver-thread side.

These invariants are what permit the §9.3.2 driver-thread hot-path
list to remain a closed set excluding any importer activity.

## 7. Persistence + ABI

The fbx-importer aggregate **does not directly persist anything**.
Its outputs feed downstream stages (Fory-encode → CAS write →
manifest publish) per SPEC §6.2. This section pins the four
ABI / persistence seams the importer **participates in** but does
**not own**.

### 7.1 Importer outputs that flow into persistence

The importer emits an in-memory `MeshArtifactPrecursor` (§3.9). The
cook step's stage 4 (`fory-serialize`, SPEC §6.2) consumes the
precursor and produces the on-disk `glibre.content.MeshArtifact`
Fory payload. The precursor → Fory transform is **owned by `data`**,
authored under `data/codegen-output/include/glibre/types/content/`
per `fory-codegen.md`. The importer does not author the schema, the
encoder, or the migration table.

The Fory-encoded bytes → `ContentHash` mapping is `seal_cooked_asset`
(SPEC §5.6) — owned by the cook step, not the importer.

The `(AssetId, CookKey, ContentHash)` triple → manifest publish is
the cook session's end-of-session step (SPEC §6.2 manifest-publish
paragraph) — owned by the cook session.

The importer's persistence surface is therefore: **precursor bytes
in the arena, span returned to caller**. Nothing more.

### 7.2 `importer_version` participation in `CookKey`

Per SPEC §4.1.3 component #2 and §7.1.2, the `importer_version`
digest is one of five ingredients of `CookKey.digest`. The
importer's contract:

1. **Compiled-in.** The 32-byte digest is a compile-time constant
   embedded in the importer's TU (§3.2 above).
2. **Returned through `Importer::version()`.** The cook step's
   stage 2 (`cook/cook_key.cpp`, SPEC §6.2) calls
   `importer.version()` and feeds the bytes into the BLAKE3
   accumulator at component #2 (length-prefixed).
3. **Bumping forces re-cook.** Per SPEC §7.2.2, a `CookKey` schema
   bump invalidates every recorded key; bumping `importer_version`
   has the same effect. This is the contract the §3.2 collapse
   (no compiler / OS in the digest) is designed to deliver — one
   re-cook trigger per intended behavior change.

The CAS ABI is not perturbed: the cooked artifacts under
`cooked/<prefix>/<hash>` are addressed by `ContentHash`, not by
`CookKey`; old hashes survive a re-cook (SPEC §4.1.5 inv #2;
§7.2.2 last paragraph).

### 7.3 FBX SDK version is the OS-side dependency

The Autodesk FBX SDK is the importer's only third-party SDK
dependency. Its version is the `SDK_TAG` component of
`importer_version` (§3.2). Build-system note:

- The cook tool links `libfbxsdk.dylib` from the Autodesk
  distribution (vcpkg-pinned per `task-breakdown-data-plugin`'s
  parallel pattern; the FBX SDK has no upstream vcpkg port at this
  spike's time, requiring an overlay port — a follow-up plan, not
  this design's concern).
- The runtime dylib (`glibre-content.dylib`) does **not** link the
  SDK. SPEC §6.5 cut.
- The SDK version is recorded in three places: (a) the `vcpkg.json`
  manifest at the cook tool target, (b) the
  `plugins/content/cook/import/fbx_importer.cpp` constant string used
  to derive `importer_version` (§3.2), and (c) the cook session's
  `CookKey.downstream_versions` list (SPEC §4.1.3 component #5; the
  `(tool_name, tool_version)` pair). The three must agree; the build
  system asserts this at codegen time.

The SDK's binary ABI is the OS-side dependency the importer hides
from the rest of the engine. A SDK SONAME bump triggers a re-cook
(via `importer_version` advancing) and a manifest re-publish (next
session). The runtime never observes the SDK's symbols directly.

### 7.4 No new Fory schemas authored here

Per the §1 refusal list, the importer authors no Fory schema. The
schemas it produces bytes for / against are all owned by `data`:

| Schema                                | FQN                                | Owned by | Importer role                                |
|---------------------------------------|------------------------------------|----------|----------------------------------------------|
| `MeshArtifact.fory`                   | `glibre.content.MeshArtifact`      | `data`   | Producer of precursor bytes.                 |
| `Manifest.fory`                       | `glibre.content.Manifest`          | `data`   | None (manifest is the cook session's).       |
| `ManifestEntry.fory`                  | `glibre.content.ManifestEntry`     | `data`   | None (cook session writes; importer doesn't).|
| `CookKey.fory`                        | `glibre.content.CookKey`           | `data`   | Contributes `importer_version` field only.   |
| `DependencyEdge.fory`                 | `glibre.content.DependencyEdge`    | `data`   | Producer of edges (texture-path → mesh).     |

The texture-path → mesh `DependencyEdge`s are emitted by the cook
session at stage 6 (`stage_table.cpp`, SPEC §6.2), driven by the
importer's `material_refs` field. The importer surfaces the
dependency; the cook session writes the edge. SRP: importer surfaces,
session persists.

### 7.5 `NormalizeParams` is not directly persisted

The §3.4 vocabulary's canonical-bytes encoding is hashed into
`CookKey.normalize_params_hash` (SPEC §7.1.2 component #4). The bytes
themselves are **not** persisted — the manifest holds only the hash
(SPEC §7.1.2 audit-only field set). This matches `fory-codegen.md`'s
ABI principle: the manifest records hashes, not raw inputs;
diagnostic recoverability of the inputs is `cook_key`'s audit
contract (§7.1.2).

A future spike that wants the raw inputs persistent (e.g. for
"explain why this asset re-cooked" tooling) would author a
`data/schemas/content/CookInputProvenance.fory` schema — out of
scope at MVP.

### 7.6 Plugin ABI seam — none crossed by importer

The importer is build-time-only (SPEC §6.5). The runtime plugin ABI
hash (`glibre_types_abi_hash`, `plugin-abi.md`) does not change with
importer-only edits unless the importer's edits also bump a Fory
schema in `data/schemas/`. The §3.2 `importer_version` digest is
**internal to content's cook pipeline**; it does not appear in the
plugin manifest's `abi_hash` field (`plugin-abi.md` schema). A
runtime hot-reload of `glibre.content` (SPEC §8 plugin-code reload)
sees no importer activity — the runtime cuts (§5.3) elide the SDK
linkage entirely.

The cook tool's own re-link with a new FBX SDK version is **not** a
runtime hot-reload; it is a workspace re-tooling event. Operators
distribute new cook-tool builds out-of-band; the runtime is unaware.

## 8. Hot-reload

The aggregate is build-time-only (SPEC §6.5); the runtime never
loads it. Hot-reload contributes through two indirect paths.

### 8.1 Manifest-swap path (the dominant content hot-reload)

When a `.fbx` source changes on disk, the watcher delivers a
`FileEvent` (SPEC §4.1.10) → `WatchEdge::translate` produces
`RecookRequest`s → `CookSession::commit()` runs the §3.7 pipeline
on the worker pool → CAS write → manifest staging → loader's phase-8
publish (SPEC §8.2 steps 1–5).

The importer's role in this path is exactly stages 1–7 of §3.7; no
hot-reload-specific code appears here. The importer is a **producer
of bytes**; the publish that makes the bytes hot-reload-visible is
the cook session's + loader's job.

What survives the manifest swap (per SPEC §8.3 row table) on the
importer's side:

- **`FbxImporter` instances**: do not survive (cook session worker
  pool destroyed by the swap if the swap is a content-plugin reload;
  on a manifest-only swap, they live across because no plugin code
  moves).
- **`FbxManager` per-worker state**: same as above.
- **In-flight `import_one` precursor bytes**: live in the per-cook
  arena which is itself bounded by the cook session's lifetime — a
  manifest-only swap that happens between cook sessions sees no
  in-flight arena.

A manifest-only swap that lands while a cook session is mid-flight
is **safe** by construction: the importer's outputs are parked in
the staging table (SPEC §6.2 stage 6), never observed by the
runtime until the staged blob is `rename(2)`'d at the next phase 8.
The phase-8 swap publishes only fully-staged sessions (SPEC §4.1.9
inv #1, all-or-nothing). Mid-cook concurrency is therefore irrelevant
to the swap's atomicity.

### 8.2 Plugin-code reload path (rare)

If `glibre.content.dylib` itself swaps (SPEC §8.3 plugin-code reload
path), the engine-wide protocol (`hot-reload-protocol.md`) runs:
drain → swap → migrate → resume.

**Does the FBX importer participate?** Only indirectly:

1. **`drain` step (SPEC §8.3 row "In-flight RecookRequest queues /
   CookSession worker pools").** The current cook session's worker
   pool is destroyed; in-flight `import_one` calls observe
   cancellation via the `CancellationToken` (§3.7 stage 4) and
   return `ImporterError::Cancelled`. The arena is drained as part
   of session abort.
2. **`swap` step.** No importer code is loaded into the runtime
   (SPEC §6.5). The runtime swap does not move SDK symbols. The
   importer is unaffected.
3. **`migrate` step.** The importer authors no migration body
   (§7.4); migrations on the manifest layer (`ManifestEntry`,
   `CookKey`, `DependencyEdge`) are §8.4 of SPEC. The importer is
   unaffected.
4. **`register` step (SPEC §8.4 last paragraph).** The new content
   plugin's register re-spawns the cook worker pool. New
   `FbxImporter` instances are constructed by the new pool; old
   instances (destroyed in drain) leave no residue.

The importer's plugin-code reload contract is therefore: **no body
runs across the reload**. The §3.10 `~FbxImporter` runs at drain;
new instances start fresh at register. SDK state (the `FbxManager`
allocator hook) is process-wide and persists across the reload —
the hook is installed once per process (§3.5), not per importer.

### 8.3 Refusal cases (importer-side)

The importer contributes the eight refusal causes already pinned in
SPEC §8.5 Class B:

| Refusal cause                                       | Stage triggering | Returned arm                                     |
|-----------------------------------------------------|------------------|--------------------------------------------------|
| Path escapes `assets/source/`                        | 1                | `ImporterError::SourceNotFound`                  |
| File missing at SDK open time                        | 1                | `ImporterError::SourceNotFound`                  |
| FBX SDK reports `eInvalidFile` (magic mismatch)      | 1                | `ImporterError::MagicMismatch`                   |
| FBX SDK reports `eInvalidFileVersion`                | 1                | `ImporterError::UnsupportedVersion`              |
| FBX SDK reports `eFileCorrupted`                     | 1, 2             | `ImporterError::MalformedPayload` (`fbx-corruption`) |
| Triangulation refused (non-triangle, `triangulate=false`) | 3            | `ImporterError::MalformedPayload` (`fbx-non-triangle`) |
| Embedded media in `FbxFileTexture`                   | 4                | `ImporterError::UnsupportedVersion` (`fbx-embedded-media`) |
| `NaN` / `denormal` floats in vertex stream           | 5                | `ImporterError::MalformedPayload` (`fbx-nan-position`) |
| Cancellation observed                                | 4                | `ImporterError::Cancelled`                       |

All eight surface as `CookOutcome::RolledBack` from the cook session
(SPEC §4.1.9 inv #1). The prior manifest snapshot remains active per
SPEC §8.5 Class B. Operator action is the §10.1 per-arm column ("Re-
export at supported version", "Restore source", "Re-export with
embedded=false", "Inspect source for NaN authoring", etc.).

The importer never refuses **the swap itself** — that is the
plugin-code reload's domain. Class A (cooker version mismatch,
expected re-cook trigger) is a normal §7.2.2 path; the importer
participates by emitting a fresh `importer_version` ingredient that
mismatches the recorded `CookKey` (§3.2 + §7.2 above). No specific
importer arm fires; the cook session simply re-runs every dependent.

### 8.4 What `migrate(...)` body the importer owns

**None.** The importer authors no Fory schema (§7.4) and contributes
no migration body. The schema-migration plumbing (`ManifestEntry`
additive, `CookKey` re-cook, `DependencyEdge` additive) lives in
SPEC §7.2 / §8.4 and is owned at the manifest aggregate's seam.

## 9. Performance

### 9.1 Cell allocation (citation)

`reviews/decisions/perf-budget.md` Per-Context Budget Table assigns
`content` the cell **0.20 ms CPU sim + 0.00 ms CPU submit + 256 MiB
heap**, with phase ownership "residency / streaming; one-shot import
work is off-thread". SPEC §9.1 refines the gate threshold to **0.50
ms** absorbing one-shot-import-handoff variance. The fbx-importer
aggregate's contribution to that cell:

- **Driver-thread CPU**: **0 ms steady-state, 0 ms peak** (SPEC §9.3
  row `Importer (§4.1.2)`). The aggregate runs entirely off the
  driver thread (SPEC §9.3.1, §6.1 above). The cell is unaffected.
- **Off-thread CPU**: bounded by the cook worker pool's wall-clock
  per cook (~100–600 ms per ~5 MiB FBX, S3 fixture; §5.1 above). The
  perf-budget gate does **not** measure this — `perf-budget.md`'s
  S3 fixture asserts "the **driver thread**'s p99 phase-1 + phase-9
  cost stays inside the 0.50 ms content cell" *while* an import
  runs. The importer's own latency is bounded operationally
  (acceptable on dev workflows; CI gates triage in §9.4 below).
- **Heap (driver-side)**: 0 MiB (SPEC §9.3 row `Importer` /
  `0 MiB`).
- **Heap (off-thread soft sub-ceiling)**: **64 MiB** (SPEC §9.3.1).
  The aggregate's per-cook arena lives here; the breach is `warn`
  in shipping, `OutOfBudget` in `GLIBRE_ALLOC_STRICT=1`.

### 9.2 Per-stage budget

Per §5.1 above, restated as a budget table with the relevant gate:

| Stage                        | CPU bound (S3, M1 firestorm) | Heap bound (per cook) | Gate                          |
|------------------------------|------------------------------|-----------------------|-------------------------------|
| 1 SDK init                   | ≤ 5 ms                       | ≤ 1 MiB               | Off-thread; not gate-measured |
| 2 Scene import               | ~ 50–500 ms                  | ~ 30–50 MiB peak      | §9.3.1 soft 64 MiB           |
| 3 Axis/unit pre-process      | ~ 5 ms                       | ≤ 1 MiB               | Off-thread; not gate-measured |
| 4 Walk + cancel poll         | ~ 20–100 ms                  | ~ 10–20 MiB           | Cancel ≤ 1 ms latency         |
| 5 Determinism canonicalization | ~ 10 ms                    | ≤ 5 MiB               | Determinism golden            |
| 6 Precursor emit             | ~ 5 ms                       | ~ 5 MiB               | Per-call returned span        |
| 7 SDK teardown               | ≤ 1 ms                       | (frees)               | Off-thread; not gate-measured |
| **per-cook total**           | **~ 100–600 ms**             | **~ 30–50 MiB peak**  | §9.3.1 soft ceiling 64 MiB   |

The per-cook bounds are **operational targets**, not perf-gate
asserts — the perf gate measures driver-thread cost, not worker-
thread wall-clock. A pathologically-slow FBX (large in vertex count,
deeply nested skeleton) extends stage 2 / stage 4 wall-clock without
breaking any gate; the operator observes it as "the cook took longer
than expected" via the cook tool's progress UI (out of scope; future
editor concern).

### 9.3 CI gate hooks

Per `perf-budget.md` §"CI Gate Spec" + SPEC §9.5:

1. **`content/import: scratch_arena_off_thread_residency`**
   (SPEC §9.5 row 4). Asserts the per-cook arena's peak resident
   bytes ≤ 64 MiB on the S3 fixture (~5 MiB FBX). PR fails if
   exceeded.
2. **`content/import: s3_off_thread_no_driver_spike`** (SPEC §9.5
   end-to-end gate). Drives the cook worker pool with the S3
   fixture (synthetic ~5 MiB FBX) for 600 frames while S1 plays
   on the driver thread; asserts the driver thread's p99 phase-1
   + phase-9 cost stays inside the 0.50 ms content cell. PR fails
   if exceeded. This is the gate that proves the FBX importer
   does not bleed onto the game loop.
3. **`content/import: fbx_determinism_golden`** (cross-cite to
   §11 below). Microbenchmark + golden test: cook the same FBX
   bytes twice (different runs of the cook tool), assert the
   resulting precursor bytes are byte-equal. This is the
   PHILOSOPHY §7 cross-host determinism gate applied to the FBX
   path.
4. **`content/import: cancellation_latency`** (cross-cite §11).
   Asserts that `import_one` returns `ImporterError::Cancelled`
   within ≤ 5 ms of `cancel()` being called on a fixture mid-walk.
   The 5 ms bound exceeds the ~1 ms theoretical bound from §6.4 to
   admit CI noise.

### 9.4 What is not gated

The per-cook wall-clock (~100–600 ms) is **not** gated. Reasons:

- It is wall-clock dominated by the FBX SDK, which we do not
  optimize.
- It varies across hosts (CI vs developer machine); a tight gate
  would flap.
- It is operationally visible to the developer (cook tool's
  progress UI) and to the spike #146 brief's S3 fixture.

A future tightening could land if a regression spike opens; not
this design's concern.

### 9.5 Driver-thread invariant

The single perf invariant the FBX importer must hold:

> **Zero importer activity on the game-loop driver thread,
> regardless of how many FBX cooks are in flight on the worker pool.**

This is asserted by the §9.3 gate #2 (`s3_off_thread_no_driver_spike`)
and structurally by SPEC §9.3.2's exhaustive driver-thread hot-path
list excluding the importer. Any future change that would invoke
`import_one` from the driver thread is rejected at SPEC §9.3.2 and
SPEC §6.5 cut review.

## 10. Failure modes

The importer's failure surface is the closed sum
`glibre::content::ImporterError` declared in SPEC §5.3 and §10.1.
The §10.1 per-arm contract is the source of truth; this section
restates the importer-specific arms with implementation-grain
detail and pins the SDK-exception → arm mapping at the carve-out
(SPEC §10.3).

### 10.1 Importer-emitted arms (subset of `ImporterError`)

The fbx-importer aggregate emits exactly the six `ImporterError`
arms below — the others (`MissingDependency`) are emitted by the
manifest-publish path (SPEC §10.1) and are not owned here.

| Arm                                | Emit site (stage)            | `error.detail` prefixes                           | Severity (SPEC §10.5) |
|------------------------------------|------------------------------|---------------------------------------------------|------------------------|
| `ImporterError::SourceNotFound`    | §3.7 stage 1; stage 4 (texture path) | `"fbx-not-found"`, `"fbx-texture-not-in-source"` | `warn`                |
| `ImporterError::MagicMismatch`     | §3.7 stage 1                 | `"fbx-magic"`                                     | `warn`                |
| `ImporterError::UnsupportedVersion`| §3.7 stage 1; stage 4 (embedded media) | `"fbx-version"`, `"fbx-embedded-media"`        | `warn`                |
| `ImporterError::MalformedPayload`  | §3.7 stage 1, 2, 3, 5; carve-out catch-all | `"fbx-corruption"`, `"fbx-non-triangle"`, `"fbx-nan-position"`, `"fbx-bootstrap"`, `"fbx-unclassified"` | `warn` for corruption / non-triangle / nan; `error` for `"fbx-bootstrap"` |
| `ImporterError::Cancelled`         | §3.7 stage 4                 | `"fbx-cancelled"`                                 | `debug`               |
| `ImporterError::MissingDependency` | (never emitted by importer; manifest-side only) | n/a                                  | (n/a)                 |

The structured-log fields the importer attaches (per SPEC §10.5):

- `asset_id`: the `AssetId` the cook is on (from the cook session's
  scheduling).
- `source_path`: `SourceAsset.path` (workspace-relative).
- `importer_kind`: literal `"fbx"`.
- `importer_version`: 64-char BLAKE3 hex of `version_` (§3.2).
- `cook_session_id`: opaque from the cook session.
- `error.detail`: per the prefix table above.
- (For `UnsupportedVersion`) `error.fbx_file_version` /
  `error.fbx_sdk_min_version`: the SDK's reported version pair.
- (For `MalformedPayload`) `error.byte_offset`: the SDK's reported
  parse offset, or zero.
- (For `MalformedPayload(fbx-nan-position)`)
  `error.vertex_index` / `error.mesh_node_name`: the offending
  vertex's location.

### 10.2 SDK-exception translation table

Pinned at the §3.7 stage-6 `try` / `catch` boundary; matches SPEC
§10.3's classification table verbatim with one-cause-per-row precision:

| SDK exception class                                                       | Translated arm                              | `error.detail` prefix      | Notes                                                                                  |
|---------------------------------------------------------------------------|---------------------------------------------|----------------------------|----------------------------------------------------------------------------------------|
| FBX SDK `FbxStatus::eInvalidFile`                                         | `ImporterError::MagicMismatch`              | `"fbx-magic"`              | SDK's `Initialize` returns false with this status; the carve-out wraps via `FbxStatus`. |
| FBX SDK `FbxStatus::eInvalidFileVersion`                                  | `ImporterError::UnsupportedVersion`         | `"fbx-version"`            | `error.fbx_file_version` field captured.                                                |
| FBX SDK `FbxStatus::eFileCorrupted`                                       | `ImporterError::MalformedPayload`           | `"fbx-corruption"`         | Either `Initialize`-time or `Import`-time corruption; same arm.                         |
| FBX SDK `FbxStatus::eFileNotFound`                                        | `ImporterError::SourceNotFound`             | `"fbx-not-found"`          | Race carve-out (file deleted between watch fan-out and SDK open).                       |
| `std::bad_alloc` from any SDK call                                        | terminate (§10.3 `std::bad_alloc` row; SPEC §10.7 OQ-2 resolved → terminate) | n/a              | The per-cook arena is sized to fit the §9.3.1 ceiling; bad_alloc inside it is a defect. |
| Any other `std::exception` derivative                                     | `ImporterError::MalformedPayload`           | `"fbx-unclassified"`       | The catch-all arm. The exception's `what()` is logged at `warn` level for triage.       |
| Any non-`std::exception` thrown object                                    | terminate                                    | n/a                        | Non-`std::exception` cannot be classified; the carve-out's `catch(...)` terminates.    |

The catch-all `MalformedPayload(fbx-unclassified)` arm exists because
the FBX SDK does not document a complete exception taxonomy; the
catch-all preserves the §3.2 collapse #1 commitment ("symptom-by-arm,
not SDK-by-arm") even when the SDK throws something the table did
not anticipate.

### 10.3 Exception scope — the unique `-fexceptions` carve-out

Per `error-model.md` §"Decision" rule 3 + §"Consequences" last
bullet, the FBX importer's implementation TU is one of three
files in the engine that compile with `-fexceptions`:

```
plugins/content/cook/import/fbx_importer.cpp     # this design
plugins/content/cook/import/freeimage_importer.cpp # sibling spike #810
plugins/content/cook/import/freetype_importer.cpp  # sibling spike #816
```

The header `fbx_importer.hpp` and **every other TU in the engine**
compiles with `-fno-exceptions`. The build system enforces this via
a per-TU compile-flag override in the cook tool's CMakeLists; raw
`add_executable(glibre-cook ...)` configures `-fno-exceptions`
globally and the override is per-source `set_source_files_properties`.

The `try` / `catch` block inside `import_one` is the **single**
translation site. The catch arms:

```cpp
// Sketch — final shape lives in fbx_importer.cpp.
auto FbxImporter::import_one(const SourceAsset& src,
                             const NormalizeParams& params,
                             ImporterArena& out_arena,
                             const CancellationToken& cancel) noexcept
    -> Result<eastl::span<const std::byte>>
{
    try {
        // Stages 1..6 inline.
        return emit_precursor(...);
    }
    catch (const ::fbxsdk::FbxStatus& st) {
        return classify_fbx_status(st);  // §10.2 table
    }
    catch (std::bad_alloc&) {
        std::terminate();                 // §10.2 row 5
    }
    catch (const std::exception& e) {
        return std::unexpected{ Error{
            ImporterError::MalformedPayload,
            ErrorContext{ __FILE__, __LINE__, "fbx-unclassified" } }};
    }
    catch (...) {
        std::terminate();                 // §10.2 row 7
    }
    // Stage 7 (SDK teardown) is in a finally-equivalent guard
    // (RAII helper) so it runs on every path including the catches.
}
```

The RAII guard uses the `eastl::scoped_exit` pattern (or a
hand-written equivalent) to avoid C++23's `std::scope_exit` (not yet
in the engine's libc++ baseline). The guard body invokes
`fbx_importer_->Destroy(true)` and `scene_->Destroy(true)` per §3.7
stage 7.

### 10.4 Recovery posture

Per SPEC §10.4 recovery-posture map, every importer arm falls into
**Refuse**:

| Arm                        | Posture | Mechanism                                              |
|----------------------------|---------|--------------------------------------------------------|
| `SourceNotFound`           | Refuse  | Cook step rolls back; prior manifest stays active.    |
| `MagicMismatch`            | Refuse  | Same.                                                  |
| `UnsupportedVersion`       | Refuse  | Same. Operator re-exports from DCC.                    |
| `MalformedPayload`         | Refuse  | Same. `error.detail` discriminates sub-cause.          |
| `Cancelled`                | Refuse (no-op) | No automatic retry; operator re-triggers.       |

There is **no Re-cook** posture from the importer side — re-cook is
the manifest-publish path's response to `MissingDependency` (which
the importer does not emit) or `HashNotInCas` (residency-side, not
importer-side). The importer is purely a producer of bytes that
either succeed or refuse with a typed arm.

### 10.5 What the importer does NOT emit

To make the boundary explicit:

- **`ImporterError::MissingDependency`** is emitted by the
  manifest-publish path (`cook/publish.cpp`, SPEC §6.2 manifest-
  publish paragraph) when the bottom-up dependency walk finds an
  edge to a child whose `AssetId` is not in the active manifest.
  The importer surfaces dependencies (texture paths in
  `material_refs`); it does not detect missing children.
- **`ResidencyError::*`** (SPEC §5.3) are emitted by the runtime's
  residency manager (SPEC §4.1.7), not by the importer. The cook
  tool may emit `ResidencyError::IoFailure` from `cas/atomic_write.cpp`
  during stage 5 of the cook step (CAS write), but that is the cook
  step's emit site, not the importer's.
- **`core::Error::*`** arms (`PluginAbiHashMismatch`,
  `SchemaMigrationFailed`, etc.) are loader-side and only relevant
  to the runtime plugin reload (§8.2 above). The importer never
  emits these.

## 11. Test plan

The importer's tests live under `tests/content/import/fbx/`. Five
classes of tests cover the §3 / §6 / §10 contracts; each Catch2
case name below is the gate-asserted name.

### 11.1 Unit tests (Catch2)

**`content/import/fbx: dispatch_routes_mesh_fbx_to_fbx_importer`**.
Asserts that `dispatch_import(SourceAsset{kind=Mesh, format=Fbx})`
routes to `FbxImporter::import_one`. Exercises SPEC §4.1.2 inv #1
(closed-sum dispatch) and SPEC §6.1 `importers/dispatch.cpp`.

**`content/import/fbx: rejects_path_outside_source_root`**.
Asserts that an FBX whose internal `FbxFileTexture` references a
path outside `assets/source/` returns
`ImporterError::SourceNotFound`. Exercises SPEC §4.1.1 inv #1 and
§3.6 above.

**`content/import/fbx: rejects_embedded_media`**. Asserts that an
FBX with `FbxFileTexture::UseEmbedded()=true` returns
`ImporterError::UnsupportedVersion` with
`error.detail = "fbx-embedded-media"`. Exercises §3.6.

**`content/import/fbx: rejects_non_triangle_when_triangulate_false`**.
Asserts that an FBX with quads + `NormalizeParams.triangulate=false`
returns `ImporterError::MalformedPayload` with
`error.detail = "fbx-non-triangle"`. Exercises §3.4 + §3.7 stage 3.

**`content/import/fbx: rejects_nan_vertex_position`**. Asserts
that a hand-authored FBX with a `NaN` position component returns
`ImporterError::MalformedPayload` with
`error.detail = "fbx-nan-position"`. Exercises §3.7 stage 5.

**`content/import/fbx: cancellation_observed_within_5ms`**. Drives
a synthetic ~200-node FBX through `import_one` while a sibling
thread calls `cancel()` mid-walk; asserts return value is
`ImporterError::Cancelled` within 5 ms wall-clock. Exercises SPEC
§4.1.2 inv #5 and §6.4.

**`content/import/fbx: produces_byte_equal_precursor_across_runs`**.
Cooks the same FBX bytes twice in two fresh cook tool processes;
asserts the returned precursor bytes are byte-equal. Exercises
PHILOSOPHY §7 + SPEC §4.2 inv #1 + §3.7 stage 5 (determinism
canonicalization).

**`content/import/fbx: importer_version_changes_on_sdk_bump`**.
Asserts that `Importer::version()` returns a different digest
when the build is configured with a different `SDK_TAG`. Exercises
§3.2.

**`content/import/fbx: scratch_arena_residency_under_64mib_on_s3`**.
Cooks the S3 fixture (~5 MiB FBX with skinning) and asserts peak
arena residency ≤ 64 MiB. Exercises SPEC §9.3.1 + §9.2 above.

**`content/import/fbx: skin_weights_normalize_to_one_when_enabled`**.
Asserts that with `NormalizeParams.skin_normalize_weights=true`,
every per-vertex weight tuple sums to `1.0` ± `1e-6` after the
4-influence cap. Exercises §3.4 + §3.7 stage 4.

**`content/import/fbx: axis_conversion_y_up_to_z_up_round_trip`**.
Cooks a y-up-source FBX with `axis_up=ZUp`, asserts the resulting
precursor's vertex positions match a known-good reference. Cooks
the result back through z-up→y-up; asserts round-trip equality.
Exercises §3.4 + §3.7 stage 3.

**`content/import/fbx: rejects_fbx_version_below_min`**. Asserts
an FBX 6.x file returns `ImporterError::UnsupportedVersion` with
`error.detail = "fbx-version"`. Exercises §3.7 stage 1.

**`content/import/fbx: emits_dependency_edge_per_texture_ref`**.
Cooks an FBX referencing two external textures; asserts the
returned `material_refs` array contains two `MaterialRef` records
with workspace-relative texture paths. Verifies the edges the cook
session uses to schedule sibling texture cooks. Exercises §3.6
+ §7.4.

**`content/import/fbx: per_thread_fbx_manager_no_shared_state`**.
Spawns 4 worker threads each running `import_one` on the same
fixture; asserts every worker's precursor is byte-equal and no
data-race / double-allocation triggers TSAN / ASAN. Exercises
§3.5 + §6.2.

### 11.2 SDK-exception translation tests

**`content/import/fbx: translates_eInvalidFile_to_magic_mismatch`**.
Forces the SDK to throw via a non-FBX byte stream renamed `.fbx`;
asserts arm + `error.detail = "fbx-magic"`. Exercises §10.2 row 1.

**`content/import/fbx: translates_eInvalidFileVersion_to_unsupported`**.
Same pattern, FBX 6.0 fixture; arm + `error.detail = "fbx-version"`.
Exercises §10.2 row 2.

**`content/import/fbx: translates_eFileCorrupted_to_malformed`**.
Truncated FBX; arm + `error.detail = "fbx-corruption"`. Exercises
§10.2 row 3.

**`content/import/fbx: translates_eFileNotFound_to_source_not_found`**.
Path-deleted-mid-cook fixture; arm + `error.detail =
"fbx-not-found"`. Exercises §10.2 row 4.

**`content/import/fbx: terminates_on_bad_alloc_inside_arena`**.
Forces a `bad_alloc` by setting the arena ceiling to 1 KiB and
cooking S3; asserts process termination via `std::terminate`.
Exercises §10.2 row 5 + SPEC §10.7 OQ-2.

**`content/import/fbx: catches_unclassified_std_exception`**. Throws
a custom `std::runtime_error` from a fixture-injected SDK callback;
asserts arm + `error.detail = "fbx-unclassified"`. Exercises §10.2
row 6.

**`content/import/fbx: terminates_on_non_std_exception`**. Throws
an `int` from a fixture-injected callback; asserts process
termination. Exercises §10.2 row 7.

### 11.3 Integration tests with cook session

**`content/cook: fbx_import_then_cas_write_then_manifest_publish`**.
End-to-end: cook one FBX through `CookSession::commit()`; asserts
(a) cook returns `CookOutcome::Published`, (b) one CAS file appears
under `cooked/<prefix>/<hash>`, (c) the manifest entry resolves the
asset's `AssetId` to that hash. Exercises SPEC §6.2 stages 1–6 with
the FBX importer in the dispatch slot.

**`content/cook: fbx_import_rolls_back_on_corruption`**. Cooks a
corrupt FBX; asserts (a) cook returns `CookOutcome::RolledBack`,
(b) no CAS file is published (the temp file may exist but the
final path does not), (c) the prior manifest snapshot remains
active. Exercises SPEC §4.1.9 inv #1 + §10.4 + §10.1
`MalformedPayload`.

### 11.4 Determinism gates (PHILOSOPHY §7)

**`content/import/fbx: byte_equal_across_macos_and_linux_ci`**.
Same FBX cooked on macOS and Linux CI; asserts byte-equal precursor
bytes (Linux CI uses the SDK linked against the same vcpkg-pinned
versions). Exercises §3.7 stage 5 + the FBX SDK's deterministic
codegen claim. The Linux side is for cross-host validation only;
the runtime is macOS-first.

**`content/import/fbx: byte_equal_across_two_runs_same_host`**.
Same as the unit case `produces_byte_equal_precursor_across_runs`,
elevated to the determinism gate so a regression flips the
`perf:headroom-low`-equivalent label per `perf-budget.md` §"CI Gate
Spec" rule 5.

### 11.5 Off-thread soundness

**`content/import: s3_off_thread_no_driver_spike`** (SPEC §9.5
end-to-end gate, restated here). The integration gate that proves
the importer does not bleed onto the driver thread; exercised by
running 600 frames of S1 with an active S3 cook on the worker pool.
PR fails if driver-thread p99 phase-1 + phase-9 cost exceeds 0.50
ms.

This is the load-bearing gate against the §9.5 driver-thread
invariant.

## 12. Open questions

Three open questions remain at this design pass; each names the gate
that re-opens it. None block this design's PR.

**OQ-1. `NormalizeParams` typed view in §5.4 header** —
The §3.4 vocabulary defines a typed schema the canonical-bytes
encode; the §5.4 header currently exposes only the opaque
`canonical_bytes` span. A typed `NormalizeParamsView` adaptor
would give callers (the cook session, future tooling) a checked
decode path without re-implementing the canonical reader. The
adaptor's body lives in the codegen output (`fory-codegen.md`);
exposing it through the §5.4 header pulls a Fory-codegen
dependency into a header consumed by the runtime. Resolution
trigger: a second consumer needs typed access (e.g. an editor's
"per-cook params inspector"). Until then, the cook session decodes
the bytes through the codegen header directly, and the §5.4
header stays Fory-free. **Owner**: planning spike for editor
content-pipeline UI; **gate**: `task-breakdown-content-fbx-importer-
detailed` follow-up does not author the typed view.

**OQ-2. Animation curves and morph targets** —
MVP refuses both per §3.4 / §3.6. Re-entry path: a future spike
authors `data/schemas/content/AnimationClipArtifact.fory` and
`MorphTargetArtifact.fory`; the FBX importer gains two more
sub-precursor emitters at §3.7 stage 4 / 5; the
`MeshArtifactPrecursor` schema bumps to include optional
references to those artifacts (or they become independent
`AssetId`s with `DependencyEdge` linkage). The path does not
require a second `Importer` aggregate — it extends this one
under a `vocab_version` bump. **Owner**: animation-system epic
(post-MVP); **gate**: animation epic landing.

**OQ-3. Material parameter values (PBR factors, etc.)** —
§3.6 surfaces material **references** but not parameter values
(roughness, metallic, normal-strength, etc.). The future
`material` plugin owns the parameter-value schema; the importer
will gain a `material_params` field at that point. Re-entry
trigger: `material` plugin epic lands. Until then, MVP pipelines
that need parameter values author them in a separate sidecar
file (out of FBX), driven by a sibling importer (TBD). **Owner**:
`material` plugin epic (post-MVP); **gate**: material plugin
landing.

These three OQs do **not** become spec residue — they are
deliberate post-MVP routing per PHILOSOPHY §5 (greatly reduced
MVP scope) + §10 (Occam — collapse in MVP, expand only on a
second consumer). Each carries its own re-opening gate; none of
them are content's residue (§12 SPEC), they are this design's
residue against the post-MVP epic landing.
