# content — Detailed Design: texture-importer aggregate

> Detailed design for the `FreeImageImporter` aggregate (cook-time
> entity) declared in `specs/content/SPEC.md` §4.1.2, with public
> surface locked in §5.4 and lifecycle / scratch / SDK seams pinned
> in §6.1 (`importers/freeimage_importer.{hpp,cpp}`), §6.2 stage 3,
> §6.5 (build-time cut row), §7.1.2 (`importer_version` participation
> in `CookKey`), §8.5 Class B (importer / cook-step refusal classes),
> §9.3.1 (off-thread soft-ceiling scratch arena), §10.1 (`ImporterError::*`
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
> Mirrors structural conventions established by
> `specs/content/fbx-importer-design.md` (sibling, merged via PR #905):
> per-thread / TLS allocator dispatch, `importer_version` digest
> contributing to `CookKey`, build-time-only seam at a single
> `-fexceptions` carve-out TU, `GLIBRE_DEFER` stage-7 cleanup,
> normalize-then-canonicalize-then-emit pipeline. **Diverges where
> FreeImage's API differs from the FBX SDK's** — most notably FreeImage
> exposes no `FbxSetMemoryAllocator`-equivalent hook (§3.5 below); the
> arena-tagging scheme works via copy-out at stage 6 rather than
> allocator-hook redirection. Cross-links upcoming
> `specs/content/font-importer-design.md` (#811) and
> `specs/content/cas-store-design.md` (#813) at the seams the importer
> participates in.
>
> Harmonius prior art (`harmonius/docs/requirements/content-pipeline/
> asset-import.md`, R-12.1.1 / R-12.1.2 / R-12.1.4 / R-12.1.5) cited
> as research input only — every conclusion below was independently
> re-derived per `PHILOSOPHY.md` §"How harmonius is used". No KTX2 /
> Basis / DDS coverage; the §3.2 collapse #1 commitment to one texture
> path (FreeImage only) is preserved, not revisited.
>
> Refs: spike #809 — `[SPIKE] design-content-texture-importer-detailed`.
> Parent: #806 (sub-epic — Detailed Designs — content). Sibling
> `[SPIKE] task-breakdown-content-texture-importer-detailed` is
> blocked by this deliverable.

## 1. Purpose

The `texture-importer` aggregate is the single component in the
content context permitted to link the FreeImage codec library and
to call into its C surface. Its one responsibility is **decoding
one `SourceAsset { kind = Texture, format ∈ {Png, Jpeg, Exr, Hdr,
Tiff} }` value object into the normalized in-memory bytes of a
`glibre.content.TextureArtifact` precursor**, packaged as
`eastl::span<const std::byte>` allocated inside a caller-supplied
`ImporterArena`, with every FreeImage-thrown C++ exception or
error-callback invocation translated at first ingress into a typed
`ImporterError::*` arm and every byte the importer claims as its own
allocated through the per-cook arena (PHILOSOPHY §11). Concretely the
aggregate owns:

1. The **`FreeImageImporter` final entity** (SPEC §4.1.2) — the
   closed-sum member that serves `SourceKind::Texture`, constructed
   at cook-session start through `FreeImageImporter::create()` (SPEC
   §5.4) and destroyed at session end. One `FreeImageImporter`
   instance per importer worker thread in the cook worker pool (§6.5
   below); FreeImage's per-bitmap decode is documented thread-safe
   once the library is initialized, but the **decoder error-callback
   sink is process-global**, so the design pins one library-init
   per process plus per-thread TLS dispatch of the error sink rather
   than a process-wide singleton importer.
2. The **first-ingress codec seam** — every FreeImage C-API call
   site in the engine lives inside `plugins/content/cook/import/
   freeimage_importer.cpp` (SPEC §10.3, the unique `-fexceptions`
   carve-out for FreeImage). The seam catches the C-callback-driven
   error notifications from FreeImage's plugin decoders (PNG / JPEG
   / TIFF / EXR / HDR), the rare `std::exception` derivative thrown
   by libpng / libjpeg-turbo / libtiff / OpenEXR internals when the
   FreeImage build links exception-aware variants, and the
   unbounded `std::bad_alloc` case, and translates each into a typed
   `ImporterError::*` arm with a structured-log `error.detail`
   prefix per the §10.3 classification table.
3. The **decode → normalize pipeline** — the deterministic walk
   over FreeImage's `FIBITMAP*` that produces the `TextureArtifact`
   precursor's in-memory layout (canonical pixel bytes in one of
   four engine layouts: `RGBA8_sRGB`, `RGBA8_Linear`, `RGBA16F_Linear`,
   `RGBA32F_Linear`; mip-zero only at MVP; per-pixel color-space
   tagged from the source format and the `color_space_hint`
   `NormalizeParams` field; usage-hint pass-through). The walk is
   pure-of-effect outside the per-cook arena (SPEC §4.1.2 inv #3)
   and respects the `CancellationToken` polled at every per-row
   strip-decode boundary (SPEC §4.1.2 inv #5).
4. The **`importer_version` digest** — a 32-byte BLAKE3 over the
   importer's compiled-in identity (FreeImage version string +
   linked codec set + major normalize-params-vocabulary version +
   post-process options vocabulary), embedded as a string literal
   in the importer's translation unit at build time and returned
   through `Importer::version()` (SPEC §5.4). The digest is one of
   the five ingredients of `CookKey` (SPEC §4.1.3 component #2)
   and is the rule that bumping the FreeImage build (or any of its
   linked codecs — libpng / libjpeg-turbo / libtiff / OpenEXR) forces
   re-cook of every dependent texture.
5. The **per-cook arena lifetime** — `ImporterArena` (the typed
   alias over `glibre::PerContextAllocator` per `perf-budget.md`
   Allocator Rule 1, SPEC §9.3.1) holds every byte the importer
   declares as its own: the canonical pixel buffer, the metadata
   record, and the precursor's emitter scratch. **FreeImage's own
   internal `malloc`-driven allocations are not arena-tagged** —
   FreeImage's API does not expose an `FbxSetMemoryAllocator`-style
   replaceable allocator (§3.5 below); those bytes live in the
   process's default heap for the strict-bounded lifetime
   `FreeImage_Load → emit copy-out → FreeImage_Unload` and are
   accounted as untagged transient overhead measured by the
   §9.3.1 soft sub-ceiling's RSS-delta gate, not by tag-counted
   heap. The arena drains once `CookSession`'s end-of-session
   rollback / publish path runs (SPEC §4.1.9 inv #1, #5) and never
   bleeds into the runtime heap.

This aggregate **refuses to own**:

- **GPU-format transcoding** — BC7 / BC6H / ASTC / ETC2 block
  compression, BCn signedness selection, hardware-format probing
  and fallback, descriptor-heap updates, sparse-binding tables.
  SPEC §3.3 routes every such concern to the `render` plugin; the
  importer hands `render` a CPU-side canonical pixel layout (`RGBA8`
  or `RGBA16F` or `RGBA32F`) and walks away. The §3.4 vocabulary
  closes this boundary — there is no `target_gpu_format`
  `NormalizeParams` field.
- **Mipmap-chain authoring** — automatic 2× downsample chains,
  Kaiser / box / Mitchell-Netravali filter selection, gamma-aware
  downsample, anisotropic mip generation, sRGB-correct downsample
  shaping. MVP imports **mip 0 only**; mip-chain construction is
  routed to `render` (which already owns format selection and may
  build mips alongside BCn compression). Mip-chain authoring
  re-enters post-MVP behind the same `Importer` seam without
  changing the cook pipeline shape (§12 OQ-2).
- **GPU upload, descriptor binding, residency, sparse binding** —
  SPEC §3.3 routes these to `render` and to the residency manager
  (§4.1.7). The importer never sees a `MTLDevice*`, never allocates
  a Metal heap, and never emits a descriptor handle.
- **CAS write** — `CAS::put(CookedAsset)` (SPEC §4.1.5, §5.7) is
  the cook-step's stage 5 (SPEC §6.2). The importer does not touch
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
  same cook" (`CookKey`) separate from "the codec seam"
  (`FreeImageImporter`).
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
  `cook/session.cpp` stage 1 (SPEC §6.2). The watcher seam (SPEC
  §4.1.10), the FSEvents callback, and the OS-level `read(2)`
  mechanics live in `platform`. The importer does not touch any
  path other than what the cook session hands it through
  `SourceAsset`. (FreeImage's `FreeImage_Load(filename)` path is
  bypassed; the importer always uses `FreeImage_LoadFromHandle`
  with a `FreeImageIO` struct routed through `platform::FileIo`,
  per §3.5 below.)
- **Source debounce / dedup** — the watcher's debounce window
  (SPEC §4.1.10 inv #2) collapses rapid resaves into one
  `RecookRequest` before any importer runs. The importer never
  sees back-to-back imports of the same `(path, source_hash)`
  pair inside one cook session.
- **Mesh / Font ingest** — those are the `FbxImporter` and
  `FreeTypeImporter` siblings (SPEC §4.1.2, detailed designs are
  sibling spikes #807 / #811). The texture path may be referenced
  by mesh imports (FBX `FbxFileTexture` paths surface in
  `MaterialRef.texture_paths`; see `fbx-importer-design.md` §3.6)
  but the importer dispatch is exhaustive on `SourceKind` per SPEC
  §4.1.2 inv #1; no cross-kind delegation happens inside this
  aggregate.
- **KTX2 / Basis / DDS / pre-compressed containers** — explicitly
  refused per §3.2 collapse #1 and SPEC §3.3 (KTX2 specifically
  rerouted to post-MVP behind the same seam). Embedded mipmap
  chains inside KTX2 / DDS would force the importer to surface
  pre-compressed payloads to `render`, which would re-introduce
  the GPU-format authoring concern §3.3 collapses out. Re-entry
  path is post-MVP behind the same `FreeImageImporter` seam by
  bumping `vocab_version` and adding rows to the §3.4 format
  table — not by adding a second importer.
- **Schema authoring and codegen** — `glibre.content.TextureArtifact`
  the Fory schema is authored under `data/schemas/content/
  TextureArtifact.fory` per `reviews/decisions/fory-codegen.md`.
  The importer is a producer of bytes conforming to that schema;
  it never defines the schema.
- **Plugin loader / hot-reload bodies** — `core::PluginLoader` and
  the manifest-pointer flip protocol (SPEC §8.2) are the loader's;
  the importer participates as a build-time-only TU under
  `tools/glibre-cook` (SPEC §6.1 lifecycle seam, SPEC §6.5
  shipping-cuts table) and ships **no runtime symbol** in
  `glibre-content.dylib`.
- **Obj-C / Obj-C++ glue** — CLAUDE.md "No Obj-C++ in engine code".
  FreeImage is plain C linked from the vcpkg-pinned distribution;
  no `.mm` translation unit appears in this aggregate's source.

The aggregate's SRP boundary is sharp: if the FreeImage version
moves, if any of its linked codec versions move (libpng, libjpeg-turbo,
libtiff, OpenEXR), if the normalize-params vocabulary mutates
(canonical-pixel-layout selection rule, color-space promotion policy,
HDR clamping rule, alpha-premultiplication), or if the per-pixel
canonicalization changes, **this design changes**. Anything else —
manifest layout, cook-session orchestration, residency policy, Fory
schema body, GPU format selection, mip-chain authoring — is out of
scope.

## 2. Requirements coverage

Mapping of harmonius asset-import requirements
(`harmonius/docs/requirements/content-pipeline/asset-import.md`,
R-12.1.1 .. R-12.1.5) onto MVP coverage in this aggregate. Every
entry is independently re-derived; coverage sites refer to sections
of `specs/content/SPEC.md` and to the design sections below.

| Harmonius clause                                                                              | Glibre disposition (MVP)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
|-----------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-12.1.1** native binary format ingestion with magic / version / hash validation            | **Refused for the texture importer; collapsed.** R-12.1.1 names a glibre-native pre-cooked binary format. SPEC §3.2 collapse #1 explicitly removes that intermediate format: FreeImage ingests source files (PNG / JPEG / EXR / HDR / TIFF) directly. The validation contract R-12.1.1 names — magic / version / hash — is preserved on the `SourceAsset → CookedAsset` seam: SPEC §4.1.1 inv #3 (`source_hash = BLAKE3(bytes)`), §4.1.4 inv #1 (`content_hash = BLAKE3(payload)`), and SPEC §4.1.2 inv #2 (FreeImage reports `FIF_UNKNOWN` / decoder-error at first ingress; §3.7 step 1 below). Pre-cooked KTX2 / DDS surfaces re-enter post-MVP behind the same `Importer` seam (§3.6 below). |
| **R-12.1.2** texture source import (PNG / JPEG / EXR / HDR / TIFF) with sRGB / linear decode | **Covered, scoped.** This is the importer's core mandate. The five formats land at the FreeImage decoder via `FreeImage_LoadFromHandle` (§3.5 below). Color-space classification follows the fixed §3.4 rule: PNG and JPEG default to `sRGB` decode; EXR, HDR, and TIFF default to `Linear` decode (TIFF defaulted to `Linear` because TIFF carries no implicit sRGB convention; the §3.4 `color_space_hint` field overrides per-asset for the rare authoring case where a TIFF was authored as sRGB). GPU-format compression (BCn / ASTC / ETC2) and texture-compression-pipeline upload are **routed to `render`** per SPEC §3.3 — the importer does not feed a "compression pipeline"; it emits canonical pixel bytes that `render` then compresses + uploads. |
| **R-12.1.3** audio source import (WAV / FLAC / Ogg Vorbis)                                    | **Refused (out of context).** SPEC §3.3 routes audio source decode to the `audio` plugin. The texture importer's `SourceKind` is closed-sum on `Texture`; the dispatch table (SPEC §6.1 `importers/dispatch.cpp`) compile-time-rejects any other kind.                                                                                                                                                                                                                                                          |
| **R-12.1.4** schema validation; errors include source path + byte offset + fix suggestion     | **Covered, scoped.** Path is the structured-log field `source_path` (SPEC §10.5); byte offset is recovered from FreeImage's plugin error-callback message when available (libpng emits chunk-offset on CRC mismatch; libjpeg-turbo emits scan-line offset on truncation; libtiff emits IFD offset; OpenEXR emits scanline-block offset; HDR / Radiance emits header-line offset) and embedded in `error.detail` (§3.7 step 2 below); fix-suggestion is the per-arm "Operator action" column from SPEC §10.1 (Class B refusal) — re-export at supported codec-version, restore source under `assets/source/`, repair sRGB chunk, etc. The structured-log handler formats these uniformly (§3.7 step 5 below). |
| **R-12.1.5** parallel batch import with progress + cancellation + rollback                    | **Covered, partitioned.** Parallelism and progress tracking are the `CookSession` aggregate's concerns (SPEC §4.1.9, §6.2): the worker pool runs one `FreeImageImporter::import_one` invocation per scheduling slot; `CookSession::commit()` returns a `CookReport { cooks_executed, cache_hits }` (SPEC §5.12). Cancellation is observed at every strip-decode boundary in the importer (§3.7 step 4 below) per SPEC §4.1.2 inv #5; rollback semantics live with the session (SPEC §4.1.9 inv #1).            |

Glibre-native requirements added beyond harmonius:

- **Per-cook arena allocation discipline** (PHILOSOPHY §11; SPEC
  §9.3.1; `perf-budget.md` Allocator Rule #1). Every byte the
  importer **declares as its own** — the canonical-pixel buffer
  emitted at stage 6, the metadata record, the per-strip
  intermediate decode buffers (§3.5 below) — comes from the
  per-cook arena tagged `ContextTag::content`. The runtime heap
  is never touched by importer code paths; raw `new` / `malloc`
  is rejected by `-Wglibre-no-raw-alloc`. **FreeImage's own
  internal allocations are an exception** documented in §3.5: the
  FreeImage C API does not expose a replaceable allocator, so
  `FreeImage_LoadFromHandle`'s FIBITMAP allocations live in the
  process default heap with a strict-bounded lifetime
  (`FreeImage_LoadFromHandle → copy into arena → FreeImage_Unload`),
  inside one TU. The 64 MiB soft sub-ceiling (SPEC §9.3.1) bounds
  the largest single texture import as `tag-counted arena bytes
  + RSS-delta of FreeImage's transient FIBITMAP`.
- **Single `-fexceptions` carve-out, narrow boundary** (SPEC
  §10.3, `error-model.md` Decision rule 3 + Consequences last
  bullet). The carve-out is `plugins/content/cook/import/
  freeimage_importer.cpp` only; the corresponding header
  `freeimage_importer.hpp` and every other TU in the engine
  compile with `-fno-exceptions`. The `try / catch` block inside
  `import_one` is the **single** translation site (§3.7 step 6
  below); no exception ever crosses the importer's public boundary
  (SPEC §4.1.2 inv #2).
- **Deterministic normalize pipeline** (PHILOSOPHY §7,
  cross-aggregate invariant SPEC §4.2 #1). Two cooks of the same
  source bytes with the same `importer_version` and the same
  `NormalizeParams` produce **byte-equal** precursor bytes — and
  hence byte-equal cooked `TextureArtifact` payloads, byte-equal
  `ContentHash`. The normalize pipeline is straight-line code with
  no platform intrinsics, no `std::unordered_*`, no PRNG, no
  clock reads, no thread-local caches. §3.7 step 5 below pins
  the determinism rule per-stage. **Floating-point determinism
  caveat**: HDR / EXR decode produces `f32` / `f16` pixel values
  whose bit pattern depends on FreeImage's linked codec
  version's IEEE-754 rounding mode; the §3.5 below pins
  `<cfenv>` rounding-mode `FE_TONEAREST` at the `import_one`
  entry to defend cross-host determinism — this is one of the
  rare cases where `std::expected`-friendly imperative state
  manipulation is permitted (PHILOSOPHY §11 retains `<cfenv>` as
  a `std::` utility EASTL does not own).
- **Importer is stateless across cooks; per-cook arena is the
  only state** (SPEC §4.1.2 identity rule). The
  `FreeImageImporter::Impl` pimpl carries only the per-thread
  cancellation-flag pointer + the alloc-exhausted atomic
  (constructed at `create()` time, owned for the importer's
  lifetime); every `import_one` call resets its scratch view of
  the arena and produces a fresh precursor. No memoization, no
  caches keyed off prior cooks.
- **Cancellation is polled, not preemptive** (SPEC §4.1.2 inv #5).
  The importer polls `CancellationToken::is_cancelled()` at every
  strip-decode boundary inside its `FreeImageIO::read_proc` shim
  (the natural granularity FreeImage's plugin decoders give
  us — typically every ~64 KiB read on PNG / TIFF, every scanline
  pair on libjpeg-turbo, every scanline-block on OpenEXR). Bound:
  ≤ ~1 ms latency to observe cancellation on an MVP-scale 4K
  RGBA8 PNG (~16 MiB decoded; the strip-decode polls fire at
  least 16 times during decode).
- **No mip-chain ingest at MVP** (§3.6 refusal list below; §12
  OQ-2). The MVP `TextureArtifact` precursor carries only mip 0;
  mip-chain construction is `render`'s job (mip selection is
  per-format, often fused with BCn block compression). Bumping
  the §3.4 `vocab_version` admits mip-1+ in a future spike
  without adding a second importer.
- **No animated / multi-frame texture ingest at MVP**. APNG
  frames in PNG, multi-page TIFF, and EXR multipart files are
  refused with `ImporterError::UnsupportedVersion` and
  `error.detail = "freeimage-multipage"`. Per-pixel canonicalize
  applies only to the first / primary frame.

Coverage rule: every harmonius MVP-scope requirement above either
lands in this design (with a coverage site) or is refused with a
one-line rationale routed to the owning context. No silent drops.

## 3. Detailed model

### 3.1 Aggregate composition

```text
FreeImageImporter  (final, SPEC §4.1.2; cook-time-only entity)
├── kind_              SourceKind::Texture                (closed-sum tag; immutable)
├── version_           ImporterVersion                    (compiled-in BLAKE3; §3.2 below)
└── impl_              Impl*                              (pimpl; arena-allocated by create())

Impl  (private; lives in freeimage_importer.cpp)
├── version_string_    eastl::string_view                  (e.g. "FreeImage 3.19.0/libpng-1.6.40/libjpeg-turbo-3.0.1/...";
│                                                          the source from which version_ is BLAKE3'd.
│                                                          Borrows from a `static constexpr char[]` literal
│                                                          in freeimage_importer.cpp (static storage duration =
│                                                          process lifetime); MUST NOT borrow from stack or
│                                                          arena allocation — implementer note: the backing
│                                                          must be a string literal or a `static const`
│                                                          variable with file scope)
└── alloc_exhausted_   std::atomic<bool>                   (set ONLY during an active FreeImage_LoadFromHandle
                                                            call (stage 2) when a PerContextAllocator refusal
                                                            occurs mid-decode; read_proc checks this flag and
                                                            short-reads to abort the in-flight decode per §6.3.
                                                            Has no role for stage-1 (pre-LoadFromHandle early
                                                            return) or stage-5/6 (post-decode direct Result<T>
                                                            return).  std::atomic is a retained std:: utility
                                                            per PHILOSOPHY §11)
```

The `Impl` is deliberately small. Unlike the FBX SDK's
`FbxManager*`-per-thread pattern (see `fbx-importer-design.md` §3.1),
**FreeImage carries no per-thread "manager" object**: the library is
process-globally initialized once by `FreeImage_Initialise` (§3.5
below) and per-thread state lives only in TLS slots the importer
itself manages. The pimpl therefore holds just identity (the version
string for digest derivation) and the alloc-exhausted flag.

Per-call scratch (lives entirely inside the supplied `ImporterArena`,
SPEC §5.4):

```text
import_one(...) scratch
├── source_buffer_     eastl::vector<std::byte, ImporterArenaAllocator>   (raw source bytes; loaded once via
│                                                                           platform::FileIo, fed to FreeImage
│                                                                           via FreeImage_LoadFromHandle)
├── fi_bitmap_         FIBITMAP*                            (FreeImage opaque handle; lives in default heap,
│                                                            NOT in arena — see §3.5; lifetime bounded by
│                                                            FreeImage_Load → emit copy → FreeImage_Unload)
├── canonical_pixels_  eastl::vector<std::byte, ImporterArenaAllocator>   (final canonical pixel buffer;
│                                                                           memcpy'd / converted from fi_bitmap_)
├── metadata_          TextureMetadata                      (extracted at stage 4; pixel format, color space,
│                                                            usage hint, dimensions, alpha channel info)
└── precursor_bytes_   eastl::span<std::byte>               (final emit; pointer into arena returned to caller)
```

The `FreeImageImporter` aggregate is the cook-time entity; the §5.4
stub is the only public surface; everything in the boxes above is
private to the implementation file group `plugins/content/cook/import/`.
SPEC §6.1 already names the file split (`freeimage_importer.{hpp,cpp}`,
`dispatch.{hpp,cpp}`, `version.{hpp,cpp}`).

`ImporterArenaAllocator` is the EASTL-conformant adaptor over
`glibre::PerContextAllocator` exposing `ContextTag::content` to
EASTL's allocator-by-value contract. The adaptor's body is a simple
forward-call shim authored in `core/include/glibre/alloc.hpp` (per
`perf-budget.md` Allocator Rules header) and shared with
`fbx-importer-design.md` §3.1; the texture importer pulls it in via
the `ImporterArena&` reference handed to `import_one`.

### 3.2 `ImporterVersion` — the compiled-in importer identity

`ImporterVersion` (SPEC §5.4) is a 32-byte BLAKE3 over a canonical
**version string** baked into the importer's translation unit at
build time. The string is the canonical concatenation of seven
components (length-prefixed, separated by `':'`):

```text
"glibre.content.freeimage_importer" ":"
FREEIMAGE_TAG  ":"  // e.g. "freeimage-3.19.0"          — the vcpkg-pinned FreeImage release.
LIBPNG_TAG     ":"  // e.g. "libpng-1.6.40"             — the PNG codec FreeImage links.
LIBJPEG_TAG    ":"  // e.g. "libjpeg-turbo-3.0.1"       — the JPEG codec.
LIBTIFF_TAG    ":"  // e.g. "libtiff-4.6.0"             — the TIFF codec.
OPENEXR_TAG    ":"  // e.g. "openexr-3.2.1"             — the EXR codec.
NORMALIZE_TAG  ":"  // e.g. "normalize-v1"              — the §3.4 NormalizeParams vocabulary version.
POST_TAG            // e.g. "post-v0"                   — reserved for future post-process stages
                                                          (e.g. SDF generation toggle); MVP = "post-v0".
```

Concrete example:

```text
glibre.content.freeimage_importer:freeimage-3.19.0:libpng-1.6.40:libjpeg-turbo-3.0.1:libtiff-4.6.0:openexr-3.2.1:normalize-v1:post-v0
```

The string is recovered at code-review time from the importer's
header (one constant per release). The BLAKE3 digest is computed at
**codegen time** by a small build-system step that reads the constant
and emits a `version.cpp` carrying the 32-byte hex literal; the
importer's `Importer::version()` returns the digest. Bumping the
FreeImage version, any of its linked codec versions, the
normalize-params vocabulary version, or the post-process options
vocabulary version changes the digest (and only those changes do).

The digest's role:

1. **Cache-key ingredient (SPEC §4.1.3 component #2; §7.1.2).** The
   `CookKey` digest depends on `importer_version`; bumping it
   forces re-cook of every dependent texture on next session per
   SPEC §7.2.2.
2. **Hot-reload coexistence (§8 below).** The build-time-only
   nature of the importer means a runtime hot-reload of
   `glibre.content` never moves the importer's code. The digest
   moves only on the release boundary that ships a new cook tool —
   at which point the §7.2.2 full-re-cook rule applies for textures
   only (digest change is per-importer; mesh / font cooks are
   unaffected unless their importer_version also moves).
3. **Telemetry (§10.5 below).** The digest is logged with every
   importer error so post-mortem triage can identify which
   FreeImage / codec / params vocabulary version produced the
   failure. Per-codec attribution is preserved structurally:
   `importer_kind = "freeimage"` plus the digest's pre-image string
   embeds each linked codec's version, so a regression caused by a
   libpng bump is identifiable from the digest alone.

Why seven components and not fewer:

- **Why include each linked codec separately.** FreeImage is a
  thin wrapper over libpng / libjpeg-turbo / libtiff / OpenEXR /
  the Radiance HDR loader. A bug fix in libjpeg-turbo (e.g.
  upstream's 3.0.0 → 3.0.1 fix to a chroma-subsample rounding
  edge case) changes JPEG decode output bit-for-bit; if the
  digest only tracked the FreeImage version, the cook cache
  would silently serve stale bytes. Per-codec tagging makes the
  re-cook trigger precise.
- **Why include `normalize-vN`.** A change to the canonical-pixel
  layout selection rule (e.g. promoting EXR's half-float decode
  from `RGBA16F` to `RGBA32F` for higher precision) changes every
  EXR cook output. Re-cook trigger.
- **Why include `post-vN`.** Reserved; MVP = `post-v0`. The slot
  exists so a future add-on (e.g. SDF generation toggle, or a
  premultiplied-alpha canonicalization toggle) can mark its
  bumps without reaching into the FreeImage or normalize
  components.
- **Why not include compiler / clang-version / libc++ flags.**
  Same rationale as `fbx-importer-design.md` §3.2: PHILOSOPHY §7
  (determinism) is the engine-wide commitment; the build system
  is responsible for byte-deterministic codegen across the
  supported toolchain matrix. Including the compiler hash would
  re-cook on every patch upgrade with zero semantic change —
  wrong trade-off; consistent with `fory-codegen.md` ABI-hash
  component list (schema sources only, no compiler identity).

### 3.3 `SourceAsset` reception — the dispatch contract

`FreeImageImporter::import_one` (SPEC §5.4) receives the
`SourceAsset` by const reference. The dispatch contract is:

1. **Closed-sum dispatch.** `importers/dispatch.cpp` (SPEC §6.1)
   compile-time-routes `SourceKind::Texture + TextureFormat::{Png,
   Jpeg, Exr, Hdr, Tiff}` to `FreeImageImporter::import_one`.
   Other kinds are unreachable here (the match is exhaustive at
   compile time per SPEC §4.1.2 inv #1).
2. **Path-scope re-validation.** The cook session already validated
   the path (SPEC §4.1.1 inv #1) when constructing the
   `SourceAsset`; the importer additionally asserts that
   `SourceAsset.path` is non-empty and that
   `SourceAsset.format.texture` is one of the five admitted
   `TextureFormat` enumerators under `GLIBRE_DEBUG`-only `assert`.
   In release the assertion is elided — the dispatch table is
   compile-time-exhaustive.
3. **`source_hash` is opaque.** The importer treats
   `SourceAsset.source_hash` as a 32-byte tag carried for telemetry
   (§10.5 below); it does not re-hash the file contents. Re-hashing
   would defeat the cook session's stage-1 hash capture (SPEC §6.2)
   and cost wall-clock proportional to the source size.
4. **`NormalizeParams.canonical_bytes` carries the per-cook
   parameter set.** The bytes are the **canonical, sorted-key,
   length-prefixed** serialization of the NormalizeParams record
   defined in §3.4 below. The importer typed-decodes the bytes
   via a generated header
   `glibre/content/freeimage_normalize_params.hpp` (codegen rule
   §6 below); it does NOT re-canonicalize on the cook path.

### 3.4 `NormalizeParams` for textures — the parameter vocabulary

`NormalizeParams.canonical_bytes` (SPEC §5.4) carries the per-cook
parameter set for the texture importer. The vocabulary fields:

| Field                       | Type    | MVP default            | Effect                                                                                                                                                              |
|-----------------------------|---------|------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `vocab_version`             | `u32`   | `1`                    | The vocabulary version. Component of `importer_version`'s `NORMALIZE_TAG` per §3.2. Bump on every breaking schema move.                                              |
| `color_space_hint`          | `enum`  | `Auto`                 | Closed sum {`Auto`, `ForceSRGB`, `ForceLinear`}. `Auto` follows the per-format default (PNG/JPEG → sRGB; EXR/HDR/TIFF → Linear). Override is for the rare case where authoring choice differs from format convention. |
| `usage_hint`                | `enum`  | `ColorMap`             | Closed sum {`ColorMap`, `NormalMap`, `RoughnessMetallicMap`, `HeightMap`, `MaskMap`, `Hdr`}. Pass-through into `TextureMetadata.usage_hint`; the importer does not apply hint-conditional decode logic at MVP. |
| `output_layout`             | `enum`  | `Auto`                 | Closed sum {`Auto`, `RGBA8`, `RGBA16F`, `RGBA32F`}. `Auto` selects per the §3.5 layout-selection table (PNG/JPEG/8-bit-TIFF → `RGBA8`; HDR/16-bit-TIFF/half-float-EXR → `RGBA16F`; full-float-EXR/32-bit-TIFF → `RGBA32F`). Forcing a layout downcasts (with clamp) or upcasts (with zero-extend) accordingly. |
| `alpha_premultiply`         | `bool`  | `false`                | If `true`, post-decode multiply RGB channels by alpha. Required by some renderer paths; opt-in here so the importer does not silently mutate authored values.       |
| `flip_vertical`             | `bool`  | `false`                | If `true`, vertically flip the decoded scanlines. Some tooling chains (e.g. OpenGL legacy convention) require origin-bottom-left; the engine's render seam is origin-top-left so MVP default is `false`. |
| `hdr_clamp_max`             | `f32`   | `+infinity`            | If finite, clamp HDR / EXR / float-TIFF channels to `[0, hdr_clamp_max]`. `+infinity` (default) preserves authored HDR values; finite values defeat physically-implausible authoring (e.g. NaN-shifted bloom artifacts). |
| `srgb_decode_mode`          | `enum`  | `IcChain`              | Closed sum {`IcChain`, `Approximate`}. `IcChain` uses the linked codec's IC-published curve (libpng's sRGB chunk-aware decode for PNG; libjpeg-turbo's color-space-aware decode for JPEG). `Approximate` uses a fixed gamma-2.2 curve as a fallback; flagged at `vocab_version` upgrade time. MVP = `IcChain`. |
| `discard_alpha_if_opaque`   | `bool`  | `false`                | If `true` and every alpha channel sample is `255` (8-bit) or `1.0` (float), strip the alpha channel and emit a 3-channel canonical layout. MVP = `false` (opt-in; the cook is faster but bandwidth differences are negligible at canonical-layout granularity since `render` re-packs into BCn / ASTC / ETC2 anyway). |
| `ban_nan`                   | `bool`  | `true`                 | Refuse decode if any pixel sample is `NaN` or `signaling NaN` (HDR / EXR / float-TIFF). Default `true` defends downstream determinism and matches `fory-codegen.md` Open Question 5. False = preserve raw bits. |
| `import_mip_chain`          | `bool`  | `false`                | MVP = `false` (mip-chain ingest deferred per §3.6, §12 OQ-2). Reserved field for the post-MVP path.                                                                 |
| `import_animated_frames`    | `bool`  | `false`                | MVP = `false` (animated PNG / multi-page TIFF / EXR multipart deferred per §3.6, §12 OQ-3). Reserved field.                                                          |

The canonical-bytes encoding is **sorted-key, length-prefixed**:
each field name is encoded as a `u8` length followed by ASCII bytes,
followed by a `u8` typetag, followed by the canonical bytes of the
value. Boolean → `u8`; enums → `u8`; `u32`/`f32` → little-endian. The
field set is sorted lexicographically by name before encoding so two
serializations of the same logical record produce byte-equal canonical
bytes (SPEC §4.1.3 inv #2). The encoding is generated by a small
codegen rule in `tools/glibre-cook/CMakeLists.txt` from a manifest
`plugins/content/cook/import/freeimage_normalize_params.fory` (the
`data` plugin's Fory grammar; the codegen tool already knows how to
produce canonical-byte serializers per `fory-codegen.md` §"Migration
Mechanic"). The importer typed-decodes via the generated header.

Why these fields and not others:

- **Why explicit `color_space_hint` rather than purely
  format-driven.** TIFF specifically has no implicit sRGB / linear
  convention; a single TIFF authored as either is equally common
  in art pipelines. Forcing a single default would silently
  mis-decode either one cohort. The two-axis shape (`Auto` → format
  default; `Force*` → override) admits both authoring conventions
  without per-asset metadata files.
- **Why no `target_gpu_format`.** SPEC §3.3 routes GPU-format
  decisions to `render`. Including a `target_gpu_format` field
  here would entangle the importer with the renderer's residency
  / format-probe layer. The four canonical CPU-side layouts
  (`RGBA8`, `RGBA16F`, `RGBA32F`, plus the `Auto` selection rule)
  are sufficient as the precursor's CPU-side shape; `render` does
  the format selection.
- **Why `ban_nan` defaults to true.** The deterministic-cook
  invariant (PHILOSOPHY §7, SPEC §4.2 inv #1) requires the same
  source bytes + same params to produce byte-equal cooked
  bytes; `NaN` payloads have ambiguous bit patterns (signaling
  vs. quiet, payload bits) that depend on the codec's float
  rounding mode. Refusing them at decode time matches the
  `fory-codegen.md` canonical-floats rule and prevents
  `ContentHash` non-determinism caused solely by `NaN` payload
  bit drift.
- **Why MVP refuses mip-chain / animated imports.** The
  `TextureArtifact` schema (SPEC §5.2, §7) carries no mip-chain
  array and no per-frame array at MVP. Adding either requires
  authoring `data/schemas/content/TextureArtifact.fory` extensions
  (or splitting `TextureArtifactAnimated.fory` etc.) and
  refining the precursor's in-memory layout. Both options
  re-enter post-MVP per §12 OQ-2 / OQ-3.

### 3.5 Codec seam — `FIBITMAP*`, `FreeImageIO`, the absent allocator hook

FreeImage's class topology and lifetime contracts dictate the
importer's structure. **Notable divergence from
`fbx-importer-design.md` §3.5**: FreeImage exposes no
`FbxSetMemoryAllocator`-equivalent API. This forces a different
arena-tagging discipline (copy-out at stage 6) than the FBX
importer's allocator-hook routing.

```text
FreeImage_Initialise / FreeImage_DeInitialise   (process-global; called once per
                                                  process from FreeImageImporter::create()'s
                                                  first invocation, paired with
                                                  std::atomic_flag-guarded refcount; see below)

FIBITMAP*  (per-decode opaque handle; allocated by FreeImage in the
            process default heap via its private `malloc` — NOT in our arena;
            destroyed by FreeImage_Unload after stage-6 copy-out.)

FreeImageIO  (custom I/O struct passed to FreeImage_LoadFromHandle.
              read_proc / write_proc / seek_proc / tell_proc shims route
              every byte FreeImage reads through the importer's
              source_buffer_ — which IS in the arena.)
```

**Process-global library init / deinit.** FreeImage 3.x is a C
library with documented one-time `FreeImage_Initialise(BOOL
load_local_plugins_only)` and `FreeImage_DeInitialise()` calls. The
init enumerates plugins (one per supported format), sets the global
output-message handler, and creates the codec dispatch table. The
deinit tears these down. Both must be called exactly once per
process per FreeImage's documentation. The importer enforces this
via a `std::atomic_int` refcount in `freeimage_importer.cpp`:

```cpp
// freeimage_importer.cpp (translation-unit-local; no external linkage)
std::atomic<int> tu_init_refcount{0};

// In FreeImageImporter::create():
if (tu_init_refcount.fetch_add(1, std::memory_order_acq_rel) == 0) {
    FreeImage_Initialise(FALSE);
    FreeImage_SetOutputMessage(&error_callback_dispatch);
}
// In ~FreeImageImporter():
if (tu_init_refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    FreeImage_DeInitialise();
}
```

The first `create()` per process initializes the library; the last
`~FreeImageImporter()` deinitializes it. Per-thread `create()` calls
between those two see only an atomic increment. This pattern is
safe because the cook worker pool's lifetime is bounded by
`CookSession::begin` / `commit`'s outer scope, and the worker pool
holds the importer instances. `std::atomic` is one of the retained
`std::` utilities per PHILOSOPHY §11.

**No `FbxSetMemoryAllocator` analog.** FreeImage 3.x does not
expose a public memory-allocator hook. The library's internal
allocations (FIBITMAP buffers, plugin scratch, IFD tables for
TIFF, scanline decode buffers for PNG / JPEG / EXR / HDR) all
go through its private `malloc / free` calls and cannot be
redirected to a per-cook arena via a hook. The importer's
arena-tagging discipline therefore differs from `fbx-importer-
design.md` §3.5:

1. **Arena holds the source buffer + the canonical pixel buffer +
   the metadata record.** Every byte the importer **declares as
   its own** lives in the arena. The arena's tag-counted bytes
   are bounded by `2 × decoded_pixel_byte_count + small overhead`
   (one copy of the source bytes, one copy of the canonical
   pixel buffer); for a 4K RGBA8 PNG that is ~50 MiB tag-counted.
2. **FIBITMAP lives in the default heap, not the arena.** Its
   bytes are NOT counted against the §9.3.1 64 MiB tag-counted
   sub-ceiling. They are bounded by lifetime: `FreeImage_LoadFromHandle`
   allocates the FIBITMAP at stage 2; stage 6 reads its pixels via
   `FreeImage_GetBits` and `memcpy`s them into the arena's
   `canonical_pixels_` buffer (after applying the §3.5 layout
   conversions); `FreeImage_Unload` releases the FIBITMAP at stage
   7. The default-heap RSS therefore peaks at ~`decoded_pixel_byte_count`
   plus FreeImage's plugin scratch (libtiff and OpenEXR
   in particular allocate intermediate IFD / part tables
   approximately equal to ~10 % of the decoded payload).
3. **The §9.3.1 64 MiB soft sub-ceiling is the joint gate.** It
   is enforced as `tag-counted-bytes + freeimage-rss-delta ≤ 64
   MiB`, where the RSS delta is sampled before/after each
   `FreeImage_LoadFromHandle` call by the strict-mode build's
   allocator-tracking shim. The shim is not a true allocator
   hook (we cannot intercept FreeImage's `malloc` directly) but
   it samples `mach_task_basic_info` resident-byte deltas around
   the FreeImage call to produce the joint accounting; this is
   the same RSS-delta technique `cas/mmap_view.cpp` uses for
   memory-mapped CAS reads (SPEC §9.4 rule 5). In shipping
   builds the RSS-delta sampling is replaced by a
   FreeImage-internal heap-watermark estimate of `~1.1 ×
   decoded_pixel_byte_count` (the MVP fixture's empirically
   measured upper bound across the five formats); shipping
   shoulds the soft warn but does not abort.

**`FreeImageIO` shim — source bytes via `platform::FileIo`.** The
importer never calls `FreeImage_Load(filename, ...)`. Instead the
source bytes are read once at stage 1 via `platform::FileIo` into
the arena's `source_buffer_`, and the importer hands FreeImage a
custom `FreeImageIO` struct whose `read_proc` reads from
`source_buffer_`:

```cpp
// freeimage_importer.cpp (sketch)
struct ArenaSourceCursor {
    eastl::span<const std::byte> bytes;
    std::size_t                  cursor{0};
    const CancellationToken*     cancel{nullptr};   // §3.7 stage 4 polling site
    Impl*                        impl{nullptr};     // for alloc_exhausted_ check
};

unsigned read_proc(void* dst, unsigned size, unsigned count, fi_handle handle) {
    auto* c = static_cast<ArenaSourceCursor*>(handle);
    // §3.7 stage 4: cancellation poll at every strip-decode boundary.
    if (c->cancel && c->cancel->is_cancelled()) {
        return 0;  // FreeImage interprets short-read as decode error;
                   // import_one detects via classify_freeimage_status() at the
                   // entry point and emits ImporterError::Cancelled.
    }
    if (c->impl && c->impl->alloc_exhausted_.load(std::memory_order_acquire)) {
        return 0;  // arena exhaustion shadow path; same short-read trick.
    }
    const std::size_t want = static_cast<std::size_t>(size) * count;
    const std::size_t avail = c->bytes.size() - c->cursor;
    const std::size_t copy  = (avail < want) ? avail : want;
    eastl::memcpy(dst, c->bytes.data() + c->cursor, copy);
    c->cursor += copy;
    return static_cast<unsigned>(copy / size);
}

// seek_proc / tell_proc are straight-line cursor manipulation.
// write_proc returns 0 (we never write through this handle).
```

The cancellation poll lives inside `read_proc` because it is the
**only function the importer can hook into FreeImage's plugin
decoders' inner loop** without modifying FreeImage. PNG / TIFF /
JPEG / EXR / HDR decoders all read in chunks (libpng's row-block
read, libtiff's strip read, libjpeg-turbo's scan-line read,
OpenEXR's chunk read, HDR's RLE-decoded scanline read); each
chunk fires `read_proc`. The polling cost per call is one atomic
load (~few ns); a 4K RGBA8 PNG's decode triggers ~64 `read_proc`
calls (decoded in 64-row strips per libpng's default IDAT chunking),
adding ~1 μs of polling overhead total — invisible.

**Per-thread error-callback dispatch — `thread_local` pinning.**
FreeImage's `FreeImage_SetOutputMessage(FreeImage_OutputMessageFunction
omf)` registers a single process-wide callback fired on every
plugin decoder error. The signature is
`void (*)(FREE_IMAGE_FORMAT fif, const char* message)`. To route
per-cook errors to the owning worker thread's `import_one` call,
the importer maintains a TLS pointer to the active per-cook error
record:

```cpp
// freeimage_importer.cpp (translation-unit-local; no external linkage)
struct PerCookError {
    // detail_storage owns the copied-out message bytes (see error_callback_dispatch below).
    // detail is a view into detail_storage; they are always kept in sync.
    // Lifetime invariant: storage is reset to empty at the top of every import_one call
    // (before any FreeImage entry point) so no cross-cook residue survives.
    eastl::fixed_string<char, 512> detail_storage{};
    eastl::string_view             detail{};      // view into detail_storage; empty == no error
    FREE_IMAGE_FORMAT              fif{FIF_UNKNOWN};
};

thread_local PerCookError tl_active_error{};
thread_local Impl*        tl_active_impl{nullptr};

void error_callback_dispatch(FREE_IMAGE_FORMAT fif, const char* message) {
    if (tl_active_impl == nullptr) {
        // Spurious callback outside an active cook (defect path; should not
        // happen because no FreeImage call runs without an active import_one).
        // Best-effort: log at debug, do nothing.
        return;
    }
    // Pin the lifetime invariant: this callback fires *within* a single
    // FreeImage entry point (FreeImage_LoadFromHandle, FreeImage_GetFileType*,
    // etc.) on the current worker thread.  The per-thread single-call contract
    // guarantees exactly one entry point is active per thread at any time; a
    // second codec callback cannot fire until the first entry point returns
    // (libpng emits chunk-level callbacks sequentially, not concurrently).
    // Therefore the LAST callback before classify_freeimage_status() is called
    // is the authoritative error; any earlier callback on the same call is a
    // warning that the final-error overwrites — the "last-error-wins" semantic.
    // To make this safe even if FreeImage's message buffer were ever aliased
    // across callbacks, we copy the message bytes into a stack-local
    // fixed_string before storing.  The fixed_string is then moved into
    // tl_active_error.detail_storage and the view points into that storage,
    // giving an arena-independent, callback-scoped lifetime.
    //
    // NOTE: tl_active_error.detail_storage must be sized to cover realistic
    // codec messages.  libpng's longest message is under 256 bytes; 512 is
    // the chosen bound with truncation (suffix "…") if exceeded.
    {
        eastl::fixed_string<char, 512> tmp;
        if (message) {
            const auto len = eastl::CharStrlen(message);
            if (len <= 511) {
                tmp.assign(message, len);
            } else {
                tmp.assign(message, 511);
                tmp += '\x85'; // ELLIPSIS (0x85 single-byte stand-in; detail is debug text)
            }
        }
        tl_active_error.fif            = fif;
        tl_active_error.detail_storage = eastl::move(tmp);
        tl_active_error.detail         = eastl::string_view{tl_active_error.detail_storage.data(),
                                                             tl_active_error.detail_storage.size()};
    }
}
```

Before the first FreeImage call in `import_one` (stage 1, source
buffer load), the worker sets:

```cpp
tl_active_error = PerCookError{};   // clear previous cook's residue
tl_active_impl  = impl_;
```

After every FreeImage call that may fire the callback, the worker
checks `tl_active_error.detail.empty()` to detect a soft failure
and dispatches via `classify_freeimage_status()` (§3.7 step 6
below). Stage 7 teardown (always, via the `GLIBRE_DEFER` guard)
clears `tl_active_impl` back to `nullptr`. A null
`tl_active_impl` in the callback indicates a spurious FreeImage
call outside of an active cook; the callback short-circuits.

This design is safe under the four-worker parallel topology
because each worker thread has its own TLS slot; no
synchronization between workers is needed for the error-dispatch
path. The single process-wide callback is registered exactly once
per process (refcount-guarded above).

**Layout-selection table — the §3.4 `output_layout = Auto` rule.**
The deterministic mapping from `(TextureFormat, FIBITMAP_TYPE,
bits-per-pixel, color-space)` to the chosen canonical output:

| Source format | FIBITMAP type           | bpp source | `Auto` chooses        | Notes                                                                            |
|---------------|-------------------------|------------|-----------------------|----------------------------------------------------------------------------------|
| PNG (8-bit)   | `FIT_BITMAP`            | 24 / 32    | `RGBA8_sRGB`          | sRGB always (color_space_hint Auto → ForceSRGB-equivalent); alpha if 32 bpp.     |
| PNG (16-bit)  | `FIT_RGBA16`            | 64         | `RGBA16F_Linear`      | **Precision note**: FreeImage `FIT_RGBA16` stores each channel as a uint16 [0, 65535]; `RGBA16F` is IEEE-754 half-float (~10.9 bits effective mantissa). Values above 2048/channel are rounded on promotion — up to ~0.024% relative error at the top of range. Accepted for MVP: the output-layout set is the canonical GPU-upload path and f16 is the smallest layout that supports HDR ranges. If lossless 16-bit integer round-trip is required (e.g. 16-bit normal-map synthesis), force `output_layout = RGBA32F` in `NormalizeParams`; this is an explicit caller override, not an Auto-path concern. Escalated to OQ-1 for post-MVP reconsideration of an `RGBA16_unorm` layout variant. |
| JPEG          | `FIT_BITMAP`            | 24         | `RGBA8_sRGB`          | sRGB; alpha forced to 255 (JPEG has no alpha channel).                           |
| TIFF (8-bit)  | `FIT_BITMAP`            | 24 / 32    | `RGBA8_Linear`        | Linear by default per §3.4 rule; alpha if 32 bpp; `color_space_hint` overrides.  |
| TIFF (16-bit) | `FIT_RGBA16`            | 64         | `RGBA16F_Linear`      | Promoted to half-float (same precision-loss caveat as 16-bit PNG above: u16→f16 rounds values above 2048/channel; force `output_layout = RGBA32F` for lossless round-trip). |
| TIFF (32-bit f32) | `FIT_RGBAF`         | 128        | `RGBA32F_Linear`      | Preserved precision.                                                              |
| EXR (half)    | `FIT_RGBAF` (half-tag)  | 64         | `RGBA16F_Linear`      | Half-float passes through bit-for-bit (modulo `ban_nan`).                        |
| EXR (full)    | `FIT_RGBAF`             | 128        | `RGBA32F_Linear`      | Full-float passes through.                                                       |
| HDR / RGBE    | `FIT_RGBF`              | 96         | `RGBA32F_Linear`      | RGBE expanded to 4-channel f32; alpha forced to 1.0 (HDR has no alpha).          |

`output_layout = Auto` selects per the table above. Forcing a
non-`Auto` layout in `NormalizeParams` performs the conversion at
stage 5 (canonicalization): `RGBA32F → RGBA16F` clamps + rounds
under `FE_TONEAREST`; `RGBA16F → RGBA8` clamps to `[0, 1]` and
quantizes; `RGBA8 → RGBA16F` zero-extends; etc. Conversions are
straight-line, deterministic per source bytes, and pure of effect
outside the arena.

### 3.6 Sub-asset / format-feature boundaries — what the texture path includes / refuses

A texture source file can carry many ancillary features in one
container: mip chains, animated frames (APNG, multi-page TIFF, EXR
multipart), embedded ICC profiles, alpha channels, color-management
metadata, multi-layer (EXR layers / TIFF photoshop-style layers),
geographic metadata (TIFF GeoTIFF), thumbnails. The aggregate's
MVP decision per feature:

| Feature                                                | MVP disposition                                                                                                                                                                                                                                              |
|--------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Mip 0 (the primary image)                              | **Imported.** Becomes the precursor's canonical pixel buffer. Layout selected per §3.5 table.                                                                                                                                                                  |
| Mip 1 .. mip N (embedded mip chain in PNG / TIFF / KTX2-not-supported) | **Refused (MVP).** §3.4 `import_mip_chain=false`. Reserved field; mip-chain authoring is `render`'s job (often fused with BCn block compression). Re-entry path is post-MVP per §12 OQ-2 — bumping `vocab_version` admits mip-1+ inside the same precursor without a new importer. |
| Animated PNG (APNG)                                    | **Refused.** First frame only. Multi-frame APNG returns `ImporterError::UnsupportedVersion` with `error.detail = "freeimage-apng-multiframe"`.                                                                                                                |
| Multi-page TIFF                                        | **Refused.** Page 0 only. Multi-page TIFF returns `ImporterError::UnsupportedVersion` with `error.detail = "freeimage-tiff-multipage"`.                                                                                                                       |
| EXR multipart                                          | **Refused.** First part only. Multipart returns `ImporterError::UnsupportedVersion` with `error.detail = "freeimage-exr-multipart"`.                                                                                                                          |
| Embedded ICC profile                                   | **Refused (with warning).** ICC color-management is not an MVP color-space concern; the §3.4 `color_space_hint` selects the canonical layout's color space, and ICC overrides are explicitly out of scope. The importer logs at `info`-level the presence of an embedded ICC for tooling visibility but does not honor it. |
| Alpha channel                                          | **Imported.** RGBA8 / RGBA16F / RGBA32F per §3.5 layout. `discard_alpha_if_opaque=true` strips it post-decode if every sample is opaque.                                                                                                                       |
| EXR layers / TIFF photoshop layers                     | **Refused.** Primary RGBA composite only. Layer-aware decode is out of MVP scope.                                                                                                                                                                              |
| TIFF GeoTIFF tags                                      | **Refused.** Geographic metadata is irrelevant to a game-engine texture. Tag presence is logged at `debug`; pixels are imported unaffected.                                                                                                                    |
| Embedded thumbnails (JPEG EXIF, TIFF SubIFD)           | **Refused.** Skipped at decode time; not surfaced.                                                                                                                                                                                                             |
| RGB without alpha source                               | **Imported, alpha synthesized.** PNG RGB / JPEG / HDR / RGB-only EXR-or-TIFF emit a 4-channel canonical layout with alpha forced to `255` (`RGBA8`) or `1.0` (`RGBA16F` / `RGBA32F`). The §3.4 `discard_alpha_if_opaque` opt-in toggles re-stripping. |
| Indexed / palettized PNG / TIFF                        | **Imported via FreeImage's automatic conversion.** FreeImage's `FreeImage_ConvertTo32Bits` un-paletizes during decode; the canonical layout receives the post-conversion bytes.                                                                                |
| Grayscale (1 / 8 / 16-bit)                             | **Imported via FreeImage's automatic conversion.** Channel-replicated to RGB with full alpha; for `usage_hint = HeightMap` / `MaskMap` workflows the consumer (typically `render`) reads only the R channel.                                                   |
| HDR / Radiance RLE-encoded RGBE                        | **Imported.** RGBE → RGBA32F decode happens inside FreeImage's HDR plugin; canonical layout = `RGBA32F_Linear`.                                                                                                                                                |
| EXR DWAA / DWAB compression                            | **Imported (depending on FreeImage build).** The §3.2 importer-version digest tracks OpenEXR's linked version; if the linked OpenEXR build was configured without DWAA/B, an unsupported file returns `ImporterError::UnsupportedVersion` with `error.detail = "freeimage-exr-dwa-codec"`. |
| TIFF JPEG-in-TIFF compression                          | **Imported (depending on FreeImage build).** Same dispatch as DWAA — the importer-version digest's libtiff component tracks the codec set.                                                                                                                     |
| Pre-compressed BCn / ASTC / ETC2 (DDS / KTX2)          | **Refused.** Path: route to `render` post-MVP. The §3.4 vocabulary's closed-sum format set excludes `Dds` / `Ktx2`; adding either is a deliberate central edit — a `vocab_version` bump and a row addition.                                                  |

The disposition is closed: any feature class not listed above is
ignored with a once-per-cook `debug`-level log entry. Adding a
feature to the imported set requires bumping `vocab_version` per
§3.4 and authoring the precursor-field shape — a deliberate
central edit, not a runtime branch (PHILOSOPHY §6, §10).

### 3.7 Normalize pipeline — the seven ordered stages inside `import_one`

`FreeImageImporter::import_one(SourceAsset, NormalizeParams,
ImporterArena&, CancellationToken&)` runs **seven** ordered stages,
each documented inline against a SPEC invariant. Stages 1–6 run
inside the `-fexceptions` carve-out's `try` block (one block,
wrapping the entire body); stage 7 runs after the `catch` epilogue
and is the FreeImage teardown phase. The `try` / `catch` pair is
the unique exception translation site (SPEC §10.3).

**Stage 1 — Source buffer load + format probe**
(SPEC §4.1.2 inv #2). Read the source bytes via
`platform::FileIo::read_into(arena, src.path)` into
`source_buffer_` (the arena-tagged byte buffer). Wrap the buffer
into an `ArenaSourceCursor` and a `FreeImageIO` struct (§3.5).
Probe the format via `FreeImage_GetFileTypeFromHandle(io, handle,
0)`. Dispatch failure modes:

- `FreeImage_GetFileTypeFromHandle` returns `FIF_UNKNOWN` →
  `ImporterError::MagicMismatch`, `error.detail = "freeimage-magic"`.
- The probed `FREE_IMAGE_FORMAT` does not match `src.format.texture`
  (e.g. file says PNG but the dispatch table routed via the `.tif`
  extension) → `ImporterError::MagicMismatch`,
  `error.detail = "freeimage-format-mismatch"`. The importer favours
  the on-disk format over the extension; the cook session's stage-1
  scan step is what registered the extension in `SourceAsset.format`.
- `platform::FileIo::read_into` returns `platform::Error::IoFailure`
  → translated to `ImporterError::SourceNotFound` (file missing,
  watcher race) or `ImporterError::MalformedPayload` (with
  `error.detail = "platform-io"`) per the `error.detail` discriminator
  on `platform::Error::IoFailure`.

The byte-offset for R-12.1.4 errors is zero at this stage (the
file has not been parsed yet); offset becomes meaningful at stage
2 onward.

**Stage 2 — `FreeImage_LoadFromHandle` decode**
(FreeImage side-effect; FIBITMAP allocations live in the default
heap per §3.5). Invoke
`FreeImage_LoadFromHandle(fif, &io, handle, decode_flags)` where
`decode_flags` is built from `NormalizeParams`:

- `JPEG_ACCURATE` for JPEG when `srgb_decode_mode = IcChain`
  (selects libjpeg-turbo's accurate IDCT);
- `EXR_NONE` for EXR (we want the raw channel data; layer / part
  selection is refused);
- `TIFF_NONE` for TIFF (we want the bytes as-stored);
- `PNG_DEFAULT` (no special flags); etc.

The decode is synchronous. Failure modes:

- Returns `nullptr` and the `tl_active_error.detail` was populated
  via the error callback → classify per the `tl_active_error.fif`:
  - libpng error (e.g. `"libpng error: bad CRC"`) →
    `ImporterError::MalformedPayload`,
    `error.detail = "freeimage-png-corruption"`,
    `error.byte_offset` parsed from the libpng message when available.
  - libjpeg-turbo error (e.g. `"Premature end of JPEG file"`) →
    `ImporterError::MalformedPayload`,
    `error.detail = "freeimage-jpeg-truncation"`.
  - libtiff error → `error.detail = "freeimage-tiff-corruption"` /
    `"freeimage-tiff-multipage"` (if message indicates multi-page
    rejection per §3.6).
  - OpenEXR error → `error.detail = "freeimage-exr-corruption"` /
    `"freeimage-exr-dwa-codec"` / `"freeimage-exr-multipart"`.
  - HDR error → `error.detail = "freeimage-hdr-malformed"`.
- Returns `nullptr` and `tl_active_error.detail` is empty
  (FreeImage internal failure with no diagnostic) →
  `ImporterError::MalformedPayload`,
  `error.detail = "freeimage-unclassified"`.
- Cancellation observed (`tl_active_error.detail` was populated
  with the read_proc-driven short-read; cancellation token also
  set) → `ImporterError::Cancelled`,
  `error.detail = "freeimage-cancelled"`.

The byte-offset (`error.byte_offset`) is parsed from the codec
message when available; the §3.5 callback record stores the
unstructured `message`, and `classify_freeimage_status()` runs a
small set of regex / prefix matches per known codec to extract
offsets. When no offset is recoverable the field is zero. The
structured log records `error.byte_offset` regardless.

Bound: ~10–200 ms wall-clock for a 4K RGBA8 PNG on the cook worker
thread (off the game-loop driver thread per SPEC §9.3.1). HDR / EXR
decodes are typically faster (the codecs are simpler than libpng's
zlib pipeline).

**Stage 3 — Pre-decode normalization** (SPEC §4.1.2 inv #3 — pure;
no I/O outside arena). Apply FreeImage post-load conversions before
the canonical-layout copy-out at stage 5:

- If the decoded `FIBITMAP_TYPE` is paletted / indexed
  (`FIT_BITMAP` with bpp ≤ 8), convert via
  `FreeImage_ConvertTo32Bits` → 32-bit RGBA. The conversion is
  deterministic per source bytes and runs in FreeImage's default
  heap; the resulting FIBITMAP replaces `fi_bitmap_` and the prior
  is `Unload`ed.
- If the decoded `FIBITMAP_TYPE` is grayscale, channel-replicate
  via `FreeImage_ConvertToType(FIT_BITMAP)` then
  `FreeImage_ConvertTo32Bits`. Same heap discipline.
- If `flip_vertical = true`, `FreeImage_FlipVertical(fi_bitmap_)`
  in place. The MVP default is `false`; this branch is for
  legacy-OpenGL-origin tooling chains.

These three FreeImage calls are pure of effect outside `fi_bitmap_`
and the arena; deterministic per source bytes (PHILOSOPHY §7).
The `<cfenv>` rounding mode is set to `FE_TONEAREST` at the
`import_one` entry to defend half-float / float conversions
across hosts (HDR / EXR / float-TIFF paths).

**Stage 4 — Metadata extraction + cancellation poll** (SPEC §4.1.2
inv #5). Extract the `TextureMetadata` record:

- `width` / `height` from `FreeImage_GetWidth` /
  `FreeImage_GetHeight`.
- `pixel_layout` from the §3.5 layout-selection table given
  `(TextureFormat, FIBITMAP_TYPE, bpp, color-space)`.
- `color_space` from the §3.4 `color_space_hint` (Auto → format
  default; ForceSRGB / ForceLinear → as named).
- `usage_hint` pass-through from `NormalizeParams.usage_hint`.
- `has_alpha` from `FreeImage_IsTransparent` /
  `FreeImage_GetTransparencyCount`.
- `dpi_x` / `dpi_y` from `FreeImage_GetDotsPerMeterX/Y` (recorded
  for tooling but not used downstream by `render` at MVP).

Poll `cancel.is_cancelled()` once before stage 5. Observed
cancellation returns `ImporterError::Cancelled`. (The decoder's
cancellation poll is inside `read_proc` per §3.5; this is the
stage-4 spot for cancellations triggered between decode completion
and copy-out.)

**Stage 5 — Canonicalization + copy-out + determinism**
(SPEC §4.2 inv #1; PHILOSOPHY §7). The hot stage. After this
runs, the FIBITMAP is no longer needed; stage 6's emit is a
pure-arena memcpy.

1. **Allocate `canonical_pixels_` in the arena** sized to
   `width × height × bytes_per_canonical_pixel` (4 / 8 / 16 for
   `RGBA8` / `RGBA16F` / `RGBA32F`). This is the arena's largest
   single allocation; in `GLIBRE_ALLOC_STRICT=1` mode it is the
   gate for the §9.3.1 64 MiB soft sub-ceiling.
2. **Per-row copy-out from `fi_bitmap_` to `canonical_pixels_`.**
   FreeImage's `FreeImage_GetBits` returns a pointer to the
   **bottom-left** pixel of the FIBITMAP under FreeImage's internal
   bottom-up storage convention (used for PNG, JPEG, TIFF); EXR is
   stored top-down and requires no reversal. `FreeImage_GetPitch`
   gives the row stride in bytes.

   Row-orientation correction to engine-canonical top-down:

   ```
   // bottom_up_formats: PNG, JPEG, TIFF (FreeImage 3.x internal convention)
   // top_down_formats:  EXR (inherits OpenEXR's scanline order)
   const bool needs_flip = is_bottom_up(fi_image_type_);   // true for PNG/JPEG/TIFF
   const uint8_t* src_base = FreeImage_GetBits(fi_bitmap_);
   const uint32_t pitch    = FreeImage_GetPitch(fi_bitmap_);
   uint8_t*       dst_row  = canonical_pixels_.data();

   for (uint32_t row = 0; row < height_; ++row) {
       const uint32_t src_row_index = needs_flip ? (height_ - 1 - row) : row;
       const uint8_t* src_row = src_base + src_row_index * pitch;
       eastl::copy_n(src_row, row_bytes, dst_row);
       dst_row += row_bytes;
   }
   ```

   Using an **asymmetric test fixture** (e.g. a gradient that is visibly
   different when flipped) is mandatory to catch orientation bugs; see
   `png_8bit_rgba_canonical_layout_row_order` in §11.3 below.
3. **Layout conversion when `output_layout != Auto`.** If the
   forced layout differs from the §3.5-selected layout, run the
   per-pixel conversion (`f32 → f16` rounds under `FE_TONEAREST`;
   `f16 → u8` clamps to `[0, 1]` and quantizes via
   `(u8)(clamp(x, 0, 1) * 255 + 0.5)`; `u8 → f16` zero-extends
   then divides by 255).
4. **`alpha_premultiply`.** If `true`, multiply RGB channels by
   alpha in canonical-layout precision. For `RGBA16F` / `RGBA32F`
   this is straight-line floating multiplies; for `RGBA8` the
   division-by-255 is fused into a 256-entry LUT for
   determinism (no platform-specific `__builtin_*` ops).
5. **`hdr_clamp_max`.** If finite, clamp every channel of every
   pixel to `[0, hdr_clamp_max]`.
6. **`ban_nan` enforcement.** If `true` (default), scan every
   float pixel for `NaN` / `signaling NaN` (`std::isnan`); abort
   with `ImporterError::MalformedPayload`,
   `error.detail = "freeimage-nan-pixel"`, and
   `error.pixel_index` / `error.channel_index` recording the
   first offending sample.
7. **`discard_alpha_if_opaque`.** If `true` and every alpha
   sample is fully opaque (`255` for `RGBA8`, `1.0` for
   `RGBA16F` / `RGBA32F`), strip the alpha channel and emit a
   3-channel canonical layout. The metadata's `has_alpha` flag
   flips accordingly.

The canonicalization is straight-line code per pixel; total cost
is bounded by `width × height × ~10 ops/pixel` (RGBA32F worst
case). For a 4K RGBA8 image (~16 M pixels) at ~1 cycle/op on
M1 firestorm at ~3.2 GHz, the stage runs in ~50 ms — comfortably
under the worker-thread wall-clock budget.

**Stage 6 — Precursor emit** (SPEC §3.9 below). Write the
precursor bytes into the arena via a generated emitter
(`glibre/types/content/texture_artifact_precursor.hpp`, the Fory
codegen output's pre-Fory CPU-side struct — generated header path
per `fory-codegen.md` §Pipeline into
`${CMAKE_BINARY_DIR}/generated/glibre-types/include/glibre/types/<ctx>/`).
The emitter is a straight-line walk over `canonical_pixels_` +
`metadata_`; no allocation outside the arena. The precursor's
CPU-side layout is **distinct** from the Fory envelope shape
(which the cook step's stage-4 produces — SPEC §6.2 stage 4); the
importer never emits Fory bytes itself.

The returned `eastl::span<const std::byte>` points into the arena;
the span is valid for the cook session's lifetime per SPEC §9.3.1
(arena drains between cooks, not between frames). The cook step's
stage-4 (`fory-serialize`) consumes the span and produces the
`CookedAsset.payload` (SPEC §4.1.4); the cook-session's stage-5
writes the payload to the CAS (SPEC §4.1.5; cross-link to
`cas-store-design.md` #813 once that design lands).

**Stage 7 — FreeImage teardown** (after the `try` / `catch`).
`FreeImage_Unload(fi_bitmap_)`. The default-heap bytes are
released back to the process allocator. This is the operation
the §6.5 `task-breakdown` follow-up names as "the deferred-cleanup
seam"; it runs unconditionally via `GLIBRE_DEFER` (§10.3) — on
success and on every `catch`-side error path — to guarantee no
FIBITMAP leaks into the next cook.

### 3.8 Concurrency model — process-global init, per-thread decode

The aggregate is **stateless across cooks** (SPEC §4.1.2 lifetime).
The cook worker pool (§4 worker pool budget) holds N
`FreeImageImporter` instances, one per worker thread. Each
instance owns:

- A reference to the process-global FreeImage library (not
  exclusive; the library is initialized once per process via
  the §3.5 atomic refcount).
- A pimpl `Impl*` with `alloc_exhausted_` + `version_string_`.

There is **shared global state** across importer instances inside
one process:

1. The FreeImage library itself (initialized via
   `FreeImage_Initialise`; documentation states post-init
   per-bitmap decode is thread-safe — multiple
   `FreeImage_LoadFromHandle` calls on different `FIBITMAP*`
   from different threads do not contend).
2. The process-wide error-callback registered by
   `FreeImage_SetOutputMessage`. The callback dispatches per-
   thread via the `tl_active_error` / `tl_active_impl` TLS
   slots (§3.5).

Per-cook concurrency:

- One worker thread runs `import_one` start-to-finish; the call
  is not internally parallelized. Parallelism across multiple
  `RecookRequest`s is the `CookSession`'s concern (SPEC §4.1.9
  composition; one importer instance per worker).
- The arena handed to `import_one` is **per-call**; arenas are
  not shared across importer calls in the same worker (each
  session allocates a fresh per-cook arena and resets at
  end-of-session per SPEC §9.3.1 rule 3).

The frame-loop driver thread **never** runs `import_one`. The
`perf-budget.md` content cell's 0.50 ms is steady-state on the
driver thread (residency tickle + handle resolve only); cook
work is off-thread per SPEC §9.3.1.

### 3.9 In-memory layout of the `TextureArtifact` precursor

The precursor (the bytes `import_one` returns) is the input to the
cook step's Fory-encode stage. Its in-memory layout is **not** the
Fory envelope shape; it is a packed CPU-side struct emitted by the
codegen rule under
`${CMAKE_BINARY_DIR}/generated/glibre-types/include/glibre/types/content/texture_artifact_precursor.hpp`
(per `fory-codegen.md` §Pipeline — the codegen tool emits a
pre-Fory CPU struct alongside the Fory struct for any persistent
type the engine wants a non-Fory in-memory form of). The shape:

```text
TextureArtifactPrecursor {
  vocab_version  : u32                  // copy of NormalizeParams.vocab_version; lets the cook step's
                                         // Fory encoder reject precursor / Fory-schema vocabulary mismatches.
  metadata       : TextureMetadata      // see below.
  pixels         : eastl::span<const std::byte>  // canonical pixel buffer; len == metadata.width *
                                                  // metadata.height * bytes_per_pixel; row-major top-down.
}

TextureMetadata {
  width                : u32             // pixels.
  height               : u32             // pixels.
  pixel_layout         : enum { Rgba8_sRGB, Rgba8_Linear, Rgba16f_Linear, Rgba32f_Linear,
                                 Rgb8_sRGB, Rgb8_Linear, Rgb16f_Linear, Rgb32f_Linear }
                                          // 3-channel variants appear only when discard_alpha_if_opaque
                                          // stripped alpha; default is the 4-channel form.
  color_space          : enum { sRGB, Linear }
  usage_hint           : enum { ColorMap, NormalMap, RoughnessMetallicMap, HeightMap, MaskMap, Hdr }
  has_alpha            : bool            // false if discard_alpha_if_opaque stripped alpha.
  dpi_x                : f32             // FreeImage_GetDotsPerMeterX → DPI conversion.
  dpi_y                : f32             // FreeImage_GetDotsPerMeterY → DPI conversion.
  source_format        : enum { Png, Jpeg, Exr, Hdr, Tiff }
                                          // recorded for telemetry; `render` does not branch on it.
  bytes_per_pixel      : u8              // 4 / 6 / 8 / 12 / 16 — derived from pixel_layout.
}
```

**Why a single mip-0 buffer.** The precursor carries one canonical
pixel buffer at MVP per §3.6 (`import_mip_chain=false`). Bumping
the §3.4 `vocab_version` to admit mip chains would extend the
shape to `pixels: list<eastl::span<const std::byte>>` indexed by
mip level, with `metadata` extended with a `mip_count` field. The
shape change is post-MVP per §12 OQ-2.

**Why no Fory schema escape.** The CPU-side struct is the
**fast-path in-memory form** with cache-coherent layouts (the
pixel buffer is a contiguous `eastl::span<const std::byte>`
inside the arena). Fory's encoding step is a deterministic
transform between this form and the `glibre.content.TextureArtifact`
schema's tag-sorted on-disk envelope; the importer produces the
CPU-side form because that is what the cook step's stage-4 expects
(per `cook_step.cpp`'s common interface across Mesh / Texture /
Font precursors).

**Why metadata is all integral / enum.** The MVP `TextureMetadata`
fields are all integral or enum with explicit byte sizes. Floating
fields (`dpi_x`, `dpi_y`) are pass-through values from FreeImage;
they are not load-bearing for `render` selection logic and are
present only for tooling round-tripping. No `eastl::string_view`
fields exist in the metadata — the precursor carries no
human-readable text; identity comes from the `AssetId` upstream.

### 3.10 Lifetime — `create` / per-call / `~FreeImageImporter`

The lifetime contract:

```text
CookSession::begin(...)               (SPEC §5.12; cook session opens)
  ↓
  for each worker thread W in pool:
    FreeImageImporter::create()       (SPEC §5.4; once per worker)
      ↓ atomic-refcount-guarded FreeImage_Initialise on first call
        in process; sets error-callback; allocates Impl pimpl.
  ↓
  for each RecookRequest assigned to W (one at a time):
    FreeImageImporter::import_one(    (SPEC §5.4; many calls per importer)
      src, params, arena, cancel)
      ↓ stages 1..7 above; arena scratch for the call;
        FIBITMAP in default heap with FreeImage_Unload at stage 7.
  ↓
  ~FreeImageImporter()                (cook session ends or worker retires)
    ↓ atomic-refcount-guarded FreeImage_DeInitialise on last
      ~FreeImageImporter in process; releases Impl pimpl.
```

`create` returns `Result<eastl::unique_ptr<FreeImageImporter>>`
(SPEC §5.4 verbatim). The smart pointer is an `eastl::unique_ptr`
per PHILOSOPHY §11 (no `std::unique_ptr` in engine code); the
deleter is the standard EASTL deleter that calls
`~FreeImageImporter`.

The `import_one` call is **synchronous** — returns when the
precursor is emitted or an error fires. No async future, no
coroutine resumption (PHILOSOPHY: no coroutines in engine code).

The `~FreeImageImporter` body:

1. Decrement the process-wide library refcount; if it reaches
   zero, call `FreeImage_DeInitialise` (releases plugin
   dispatch tables; matches the `FreeImage_Initialise` paired
   with the atomic refcount).
2. Reset the pimpl.

The destructor is `noexcept` (PHILOSOPHY: every public destructor
in engine code is noexcept; the carve-out is the importer's
`import_one` body, not its destructor). Any FreeImage-thrown
exception during `DeInitialise` (rare; the docs say it does not
throw, but if a third-party plugin's deinit throws) is logged at
`error` and swallowed; the destructor must not escape exceptions.

## 4. Public surface

The §5.4 stub is the only public C++23 header surface the importer
exports. Reproduced here for cross-reference; **this design does
not modify the stub**.

```cpp
// SPDX-License-Identifier: Apache-2.0
// content/include/glibre/content/importer.hpp — texture-importer slice.
//
// Locked in specs/content/SPEC.md §5.4. -fno-exceptions header;
// the implementation TU (freeimage_importer.cpp) carries the
// unique -fexceptions carve-out per §10.3 / reviews/decisions/error-model.md
// §"Decision" rule 3.

namespace glibre::content {

class FreeImageImporter final : public Importer {
public:
    // Initialises FreeImage's plugin dispatch on first call in the
    // process (atomic-refcount-guarded; idempotent across worker
    // threads). Per-thread instance; FreeImage's per-bitmap decode
    // is thread-safe but the error-callback sink is process-global,
    // so the implementation routes per-thread errors via TLS (§3.5).
    [[nodiscard]] static auto create() noexcept
        -> Result<eastl::unique_ptr<FreeImageImporter>>;

    // Read the texture source, normalize per `params`, emit the
    // `glibre.content.TextureArtifact` precursor bytes into `out_arena`.
    // FreeImage exceptions (rare; libpng / libjpeg-turbo / libtiff /
    // OpenEXR / Radiance HDR error callbacks) translated at the carve-out
    // boundary into `ImporterError::*` arms; never escape.
    //
    // Cancellation (§3.7 stages 2 + 4) polled at every strip-decode
    // boundary inside FreeImageIO::read_proc; observed cancellation
    // returns `ImporterError::Cancelled` promptly (§4.1.2 inv #5).
    //
    // The returned span points into `out_arena` and is valid for
    // the arena's lifetime (cook session; §9.3.1 rule 3).
    [[nodiscard]] auto import_one(const SourceAsset&        src,
                                  const NormalizeParams&    params,
                                  ImporterArena&            out_arena,
                                  const CancellationToken&  cancel) noexcept
        -> Result<eastl::span<const std::byte>>;

    ~FreeImageImporter();

private:
    FreeImageImporter() noexcept;
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
  add a typed `NormalizeParamsView` adaptor (shared with the FBX
  / Font importers) that decodes the bytes into the per-importer
  fields, exported through the same header. The body of the typed
  view lives inside
  `${CMAKE_BINARY_DIR}/generated/glibre-types/include/glibre/types/content/freeimage_normalize_params.hpp`
  (Fory-codegenerated, per `fory-codegen.md` §Pipeline); this
  design pins the vocabulary contract without importing the typed
  view into the §5.4 header (which would pull a Fory-codegen
  dependency into a header consumed by the runtime that does not
  need it).

The `Importer` base trait (SPEC §5.4) is a non-virtual base. The
closed sum admits exactly one implementation per `SourceKind`
(§3.2 collapse #1). Generic cook-step code references `Importer&`
and dispatches through the free function `dispatch_import(...)`
defined in `importers/dispatch.cpp` (SPEC §6.1) — a compile-time
switch on `Importer::kind()` matched by `SourceKind`. There is
no virtual dispatch table.

`Importer::version()` returns the 32-byte `ImporterVersion`
digest (§3.2 above). Generic code consumes it without knowing
the FreeImage internals.

No second public header is added. Internal helpers (the
`FreeImageIO` shim, the `tl_active_error` TLS slot, the
canonicalization tables, the §3.7 stage emitters) live behind
the implementation TU's translation-unit boundary.

## 5. Hot/cold path split

`texture-importer` is a **build-time-only** aggregate. SPEC §6.5
names the cuts: `importers/freeimage_importer.{hpp,cpp}` is shipped
only into the `tools/glibre-cook` host executable, **not** into
`glibre-content.dylib`. The runtime cannot link the importer;
calling into `FreeImageImporter::*` from a shipping build is a
link-time error (undefined symbol), not a runtime branch. Every
section below is the cook-tool's profile; the runtime profile
elides the entire aggregate.

### 5.1 Hot path — within one `import_one` call

Stages 1–7 (§3.7) form the import call's hot path. Stage-by-stage
profile for the design's reference workload (a 4K RGBA8 PNG
~16 MiB decoded; M1 firestorm):

| Stage              | Hot/cold | Bound (4K PNG, M1 firestorm)  | Dominant cost                                                       |
|--------------------|----------|-------------------------------|---------------------------------------------------------------------|
| 1 Source load + probe | Cold | ≤ 5 ms                         | platform::FileIo::read_into into arena; `FreeImage_GetFileTypeFromHandle`. |
| 2 FreeImage decode | **Hot**  | ~ 30–200 ms                    | libpng zlib / libjpeg-turbo IDCT / libtiff predictor / OpenEXR DWA / HDR RLE. |
| 3 Pre-decode normalize | Hot  | ~ 1–10 ms                      | `ConvertTo32Bits` for paletized / grayscale; `FlipVertical` if forced. |
| 4 Metadata + cancel poll | Hot | ≤ 1 ms                       | Metadata getters; one cancellation atomic load.                      |
| 5 Canonicalization | **Hot**  | ~ 20–80 ms                     | Per-row / per-pixel copy + format conversion + clamp + ban_nan scan. |
| 6 Precursor emit   | Hot      | ~ 5 ms                         | Straight-line memcpy + metadata write into arena.                    |
| 7 FreeImage teardown | Cold   | ≤ 1 ms                         | `FreeImage_Unload` releases the FIBITMAP back to default heap.       |

4K RGBA8 PNG totals to ~60–300 ms per cook on the worker thread —
**off the game-loop driver thread**. The 0.50 ms content cell
(`perf-budget.md`) is **not** charged for this; the 64 MiB
importer soft sub-ceiling (SPEC §9.3.1) is the relevant gate.

The CPU profile is dominated by stage 2 (FreeImage decode) and
stage 5 (canonicalization). Stage 2 is library-bound (we do not
optimize libpng / libjpeg-turbo / libtiff / OpenEXR / HDR
internals). Stage 5 is straight-line per-pixel code; the 4K
RGBA8 case (16 M pixels) is bounded by memory bandwidth on the
copy-out (16 MiB read + 16 MiB write at ~200 GiB/s system
bandwidth ≈ 0.2 ms theoretical; the ~50 ms actual reflects the
ban-NaN scan + clamp + per-pixel format checks dominating).

For HDR inputs (a 4K RGBA32F EXR — ~256 MiB decoded — would
exceed the §9.3.1 64 MiB soft ceiling), the cook tool's progress
UI surfaces the soft-ceiling warn. In practice MVP textures
fit comfortably inside the ceiling at 4K resolution and below.

### 5.2 Cold path — `create` and `~FreeImageImporter`

`create` runs once per worker thread per cook session opening
(§3.10). The first `create` in a process additionally calls
`FreeImage_Initialise(FALSE)` (registers built-in plugins; sets
the global error-callback). Costs:

- `FreeImage_Initialise` (first call only): ~few ms (plugin
  registry build).
- Subsequent atomic refcount increment: <1 μs.

Total: ≤ 5 ms first call, ≤ 1 ms subsequent calls. Cook sessions
typically begin once per editor save burst; this is amortised to
zero in steady state.

`~FreeImageImporter` runs once per worker per session close
(§3.10). Cost:

- `FreeImage_DeInitialise` (last call only): ~few ms (releases
  plugin dispatch tables).
- Subsequent atomic refcount decrement: <1 μs.

These are the only paths touched outside `import_one`; both run
off the game-loop driver thread (cook worker pool). The runtime
never runs them.

### 5.3 No runtime hot path

The runtime (`glibre-content.dylib`) exposes the §5 surface but
**never calls** `FreeImageImporter::*`. SPEC §6.5 cut row makes
this a build-time guarantee; SPEC §10.3's `-fexceptions` carve-out
is build-time-only (the runtime TUs all build with
`-fno-exceptions`). Any future runtime path that wanted to
ingest source images would re-introduce FreeImage linkage into
the runtime dylib — explicitly refused by SPEC §3.3 (the runtime
is a producer of `TextureArtifact` consumers, not a consumer of
sources). The §3.3 + §6.5 commitments together ensure the
importer's cost is bounded to the cook tool's profile.

## 6. Concurrency

### 6.1 Frame phase ownership

The texture importer **does not run on the game-loop driver
thread**. SPEC §9.3.1 ("Off-thread soft ceiling — importer
scratch arena") pins this: cook orchestration runs entirely on
the **importer worker pool** outside the frame loop. The
phase-by-phase contribution:

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
zero**; SPEC §9.3.2 ("Hot-path operations — exhaustive list")
confirms this — the three driver-thread hot operations
(residency-state evaluation, hot-reload check, AssetHandle
resolution) do not touch the importer.

### 6.2 Worker pool topology

The cook worker pool is owned by `cook/worker_pool.cpp` (SPEC
§6.1) and **shared across all three importers** (FBX, FreeImage,
FreeType). The texture importer's per-worker contribution:

- **Pool size**: 4 workers at MVP (per `fbx-importer-design.md`
  §6.2 — same pool). Sized from `perf-budget.md`'s `content`
  row "one-shot import work is off-thread"; the precise count
  is `cores − 1` clamped to `[2, 4]`. Final number is the
  implementation plan's call (per `task-breakdown-content-
  texture-importer-detailed`); this design pins the **shape**
  (one `FreeImageImporter` per worker) and the **upper bound**
  (≤ 4) for the perf budget §9.3.1's 64 MiB soft sub-ceiling.
- **One `FreeImageImporter` per worker**, allocated at session
  start (§3.10), destroyed at session end. The aggregate is
  **per-thread**; the FreeImage library itself is process-global
  and shared across workers.
- **One `ImporterArena` per worker per cook**: the cook worker's
  per-call arena handed into `import_one` (§3.10).
- **No work-stealing across workers' `import_one` calls.** The
  cook session schedules one `RecookRequest` per worker slot at
  a time; contention is only at the queue boundary
  (`cook/topology.cpp`'s topo-sort, SPEC §4.1.9 inv #3).

The pool is **persistent** across cook sessions inside one
process lifetime (the cook tool runs many sessions per editor
invocation). Workers retire only at process shutdown or
`glibre_plugin_drain` for a content-plugin reload (SPEC §8.3
row "In-flight RecookRequest queues / CookSession worker pools"
— destroyed by drain, re-spawned by register).

### 6.3 Allocators

Per `perf-budget.md` Allocator Rule #1 + SPEC §9.3.1:

- **Per-context tag**. Every importer-declared allocation is
  stamped with `ContextTag::content`. The arena (`source_buffer_`,
  `canonical_pixels_`, `metadata_`, `precursor_bytes_`) routes
  through `glibre::PerContextAllocator`. EASTL containers used
  by the importer (`eastl::vector<T, ImporterArenaAllocator>`)
  carry the same tag through their allocator type.
- **FreeImage's internal allocations are NOT arena-tagged** —
  per §3.5, FreeImage exposes no allocator hook. Its FIBITMAP
  bytes live in the process default heap with strict-bounded
  lifetime (`FreeImage_LoadFromHandle → stage-6 copy → FreeImage_Unload`).
  Accounting in `GLIBRE_ALLOC_STRICT=1` mode uses an RSS-delta
  sample around the FreeImage call (§3.5); the joint
  `tag-counted + RSS-delta ≤ 64 MiB` gate is checked in §9.3
  below.
- **Soft 64 MiB sub-ceiling**. Per SPEC §9.3.1 rule 2; the cook
  scratch arena breaches the ceiling at `warn` level in shipping
  builds and `OutOfBudget` in `GLIBRE_ALLOC_STRICT=1` builds.
  Joint accounting is `arena_tag_bytes + freeimage_rss_delta`.
  A 4K RGBA8 PNG cooks at ~50 MiB tag-counted (16 MiB source +
  16 MiB FIBITMAP-equivalent FRSS + 16 MiB canonical) — fits;
  a 4K RGBA32F EXR (~64 MiB source + ~256 MiB decoded) **breaches
  the soft ceiling** and surfaces the warn. Operator action: split
  the source, accept the warn (shipping), or bump the §3.4
  `output_layout` to a smaller-precision target.
- **Multi-frame, off-thread arena lifetime**. Per SPEC §9.3.1
  rule 3, the arena drains between cooks (at session end) —
  **not** between frames. The transient-arena exemption
  (Allocator Rule 4) does **not** apply. The 64 MiB counts
  against the 256 MiB `content` heap cell (Allocator Rule 1).
- **No raw `new` / `malloc`**. Per `perf-budget.md` Allocator
  Rules header + SPEC §9.4 rule 7. The build rejects raw
  allocations in `plugins/content/cook/import/`. FreeImage's
  vendor-internal `malloc` is the third-party-wrap exemption
  (§4.1.2 inv #3); unlike FBX SDK there is no allocator hook
  available, so the exemption is widened to "FreeImage's own
  internal allocations during a `LoadFromHandle` call" — the
  exemption is documented in this design.
- **Arena-allocation null-return protocol (`GLIBRE_ALLOC_STRICT=1`)**.
  Allocation failures are caught at three distinct points, each with a
  different propagation path:

  - **Stage 1 (source-buffer load) — pre-FreeImage early return.**
    If the `source_buffer_` arena allocation fails before
    `FreeImage_LoadFromHandle` is called, `import_one` returns an
    `ImporterError::MalformedPayload` with
    `error.detail = "freeimage-alloc-exhausted"` immediately.
    No `FreeImage_LoadFromHandle` is ever called; the `alloc_exhausted_`
    flag is irrelevant for this path.

  - **Stage 2 (in-flight FreeImage decode via `FreeImage_LoadFromHandle`)
    — `alloc_exhausted_` + `read_proc` short-read.**
    This is the only stage in which an arena allocation failure can
    occur *while* a `FreeImage_LoadFromHandle` call is in progress
    (e.g. if a codec's internal strip-allocation triggers a
    `PerContextAllocator` refusal before the decode finishes).
    The importer sets `alloc_exhausted_` to `true`; the `read_proc`
    callback checks the flag on every invocation and returns a
    short-read (0 bytes) to abort the in-flight decode.
    `classify_freeimage_status()` translates the resulting decode failure
    to `ImporterError::MalformedPayload` with
    `error.detail = "freeimage-alloc-exhausted"`.
    **This short-read trick is scoped to stage 2 only** — it cannot fire
    after `FreeImage_LoadFromHandle` returns because `read_proc` is only
    invoked during an active `LoadFromHandle` call.

  - **Stage 5 (`canonical_pixels_` allocation) and stage 6 (precursor
    emit) — direct `Result<T>` early return.**
    By stages 5 and 6 the `FIBITMAP` has already been fully decoded and
    `FreeImage_LoadFromHandle` has returned.  There is no in-flight
    FreeImage entry point whose `read_proc` could fire.  Failures here
    are caught at the `Result<T>` allocation-check call site and returned
    directly as `ImporterError::MalformedPayload` with
    `error.detail = "freeimage-alloc-exhausted"`.  The
    `alloc_exhausted_` flag plays no role for stage 5 or stage 6.

  In practice, the 64 MiB soft sub-ceiling (§9.3.1) is sized for
  typical 4K textures (~50 MiB peak); the null path is a defect guard
  rather than a routine production path.  The `GLIBRE_ALLOC_STRICT=1` CI
  gate (`perf-budget.md` CI Gate §3) validates the ceiling on the
  reference fixtures.

### 6.4 Cancellation propagation

SPEC §4.1.2 inv #5 + SPEC §4.1.9 inv #5 require cancellation to
propagate within ~one frame's wall-clock. The mechanics:

1. `CookSession::cancel()` sets the session's `CancellationToken`
   to `cancelled = true` (atomic store, relaxed).
2. The worker thread's `import_one` polls
   `cancel.is_cancelled()` at every `read_proc` invocation
   (§3.5) and once at stage 4 (§3.7). Polling cost: one
   `memory_order_acquire` atomic load per invocation, ~few ns;
   a 4K PNG decode triggers ~64 `read_proc` calls (libpng's
   default IDAT chunking adds ~64 strip reads per 4K PNG),
   adding ~1 μs of polling overhead total — invisible.
3. Observed cancellation returns short-read from `read_proc`,
   FreeImage's plugin decoder reports decode failure, the
   importer's `classify_freeimage_status()` distinguishes
   cancellation from corruption via the `cancel.is_cancelled()`
   re-check, and `import_one` returns `ImporterError::Cancelled`
   immediately. The worker thread proceeds to the
   `~FreeImageImporter` path (or to its next dequeue if the
   importer instance is reused).
4. The arena drain (at session abort) reclaims any in-flight
   allocations.

Bound: ≤ ~5 ms wall-clock from `cancel()` to `import_one`
returning on a 4K PNG. Stage 1 (source buffer load via
`platform::FileIo`) and stage 5 (canonicalization) are the lone
non-strip-poll points — stage 1 is bounded by the source size
read time (~1 ms for 16 MiB at typical M1 SSD bandwidth), stage 5
runs to completion (~20–80 ms) before the metadata-stage poll
fires. The 5 ms bound is the design target; the §11 test gate
admits 10 ms to absorb CI noise.

### 6.5 Off-thread invariants

Two invariants govern the off-thread topology (shared with
`fbx-importer-design.md` §6.5):

1. **No importer call ever runs synchronously on the driver
   thread.** The cook session's `begin` / `enqueue` / `commit`
   / `cancel` methods may be called from the driver thread
   (editor flow, watch event flow), but they enqueue work; the
   actual `import_one` dispatch runs on the worker pool. SPEC
   §4.2 inv #8.
2. **The driver thread never holds a reference to importer
   state that requires synchronization with a worker.** The
   cook session parks results on a queue; the driver thread
   polls the queue at phase 8 entry (loader's drain step) for
   staged manifest blobs. The queue's SPSC discipline matches
   the `WatchEdge` SPSC pattern (`platform/file-watcher-design.md`
   §3.2): one producer (the worker pool aggregate), one
   consumer (the loader's phase-8 reader). No locks on the
   driver-thread side.

These invariants are what permit the §9.3.2 driver-thread
hot-path list to remain a closed set excluding any importer
activity.

## 7. Persistence + ABI

The texture-importer aggregate **does not directly persist
anything**. Its outputs feed downstream stages (Fory-encode →
CAS write → manifest publish) per SPEC §6.2. This section pins
the four ABI / persistence seams the importer **participates in**
but does **not own**.

### 7.1 Importer outputs that flow into persistence

The importer emits an in-memory `TextureArtifactPrecursor` (§3.9).
The cook step's stage 4 (`fory-serialize`, SPEC §6.2) consumes
the precursor and produces the on-disk
`glibre.content.TextureArtifact` Fory payload. The precursor →
Fory transform is **owned by `data`**, authored under
`${CMAKE_BINARY_DIR}/generated/glibre-types/include/glibre/types/content/`
per `fory-codegen.md` §Pipeline. The importer does not author the
schema, the encoder, or the migration table.

The Fory-encoded bytes → `ContentHash` mapping is
`seal_cooked_asset` (SPEC §5.6) — owned by the cook step, not
the importer.

The `(AssetId, CookKey, ContentHash)` triple → manifest publish
is the cook session's end-of-session step (SPEC §6.2
manifest-publish paragraph) — owned by the cook session.

The importer's persistence surface is therefore: **precursor
bytes in the arena, span returned to caller**. Nothing more.

### 7.2 `importer_version` participation in `CookKey`

Per SPEC §4.1.3 component #2 and §7.1.2, the `importer_version`
digest is one of five ingredients of `CookKey.digest`. The
importer's contract:

1. **Compiled-in.** The 32-byte digest is a compile-time
   constant embedded in the importer's TU (§3.2 above).
2. **Returned through `Importer::version()`.** The cook step's
   stage 2 (`cook/cook_key.cpp`, SPEC §6.2) calls
   `importer.version()` and feeds the bytes into the BLAKE3
   accumulator at component #2 (length-prefixed).
3. **Bumping forces re-cook.** Per SPEC §7.2.2, a `CookKey`
   schema bump invalidates every recorded key; bumping
   `importer_version` has the same effect. Notable: the
   texture importer's digest tracks **each linked codec
   separately** (libpng / libjpeg-turbo / libtiff / OpenEXR;
   §3.2 above), so a libpng-only patch upgrade re-cooks PNG
   textures while leaving JPEG / TIFF / EXR / HDR cooks
   unchanged at the digest level. The cook session, however,
   does not pre-filter by source format — it re-cooks every
   asset whose recorded `CookKey` mismatches; the cache-hit
   path (SPEC §6.2 stage 2) still runs per-asset, so
   format-specific re-cook narrowing is an effect of the
   per-asset `CookKey` recomputation, not of importer-side
   bookkeeping.

The CAS ABI is not perturbed: the cooked artifacts under
`cooked/<prefix>/<hash>` are addressed by `ContentHash`, not by
`CookKey`; old hashes survive a re-cook (SPEC §4.1.5 inv #2;
§7.2.2 last paragraph). The cross-link to
`cas-store-design.md` (#813) detailed-design will pin the CAS
side of this seam; the importer requires only that the cook
session writes **before** publishing, which `cas-store-design.md`
will commit to.

### 7.3 FreeImage + linked codecs — the OS-side dependency

FreeImage is the importer's only third-party SDK dependency
**at the API surface**, but it transitively links libpng,
libjpeg-turbo, libtiff, OpenEXR (with Imath), and the Radiance
HDR loader. Their versions are the `LIBPNG_TAG` / `LIBJPEG_TAG`
/ `LIBTIFF_TAG` / `OPENEXR_TAG` components of `importer_version`
(§3.2). Build-system note:

- The cook tool links FreeImage from the vcpkg-pinned
  distribution per CLAUDE.md "Tech Stack (locked)". FreeImage
  3.19.x has a vcpkg port (the standard `freeimage` port, with
  feature gates for each codec); the linked codec versions
  surface through `FreeImage_GetVersion` and per-codec
  `*_GetLibVersion` functions.
- The runtime dylib (`glibre-content.dylib`) does **not** link
  FreeImage. SPEC §6.5 cut.
- The codec versions are recorded in five places: (a) the
  `vcpkg.json` manifest at the cook tool target, (b) the
  `plugins/content/cook/import/freeimage_importer.cpp` constant
  string used to derive `importer_version` (§3.2), (c) the
  cook session's `CookKey.downstream_versions` list (SPEC
  §4.1.3 component #5; the `(tool_name, tool_version)` pair),
  (d) the per-codec `*_GetLibVersion` function call at first
  `create()` (validates that the build system's pinned versions
  match the linked binary), and (e) a build-time codegen step
  that asserts (b) and (d) agree. The five must agree; the
  build system asserts this at codegen time.

The codecs' binary ABIs are the OS-side dependency the importer
hides from the rest of the engine. A codec SONAME bump triggers
a re-cook (via `importer_version` advancing) and a manifest
re-publish (next session). The runtime never observes the
codecs' symbols directly.

### 7.4 No new Fory schemas authored here

Per the §1 refusal list, the importer authors no Fory schema.
The schemas it produces bytes for / against are all owned by
`data`:

| Schema                                | FQN                                | Owned by | Importer role                                |
|---------------------------------------|------------------------------------|----------|----------------------------------------------|
| `TextureArtifact.fory`                | `glibre.content.TextureArtifact`   | `data`   | Producer of precursor bytes.                 |
| `Manifest.fory`                       | `glibre.content.Manifest`          | `data`   | None (manifest is the cook session's).       |
| `ManifestEntry.fory`                  | `glibre.content.ManifestEntry`     | `data`   | None (cook session writes; importer doesn't).|
| `CookKey.fory`                        | `glibre.content.CookKey`           | `data`   | Contributes `importer_version` field only.   |
| `DependencyEdge.fory`                 | `glibre.content.DependencyEdge`    | `data`   | None at MVP (texture sources have no transitive children; PNG / JPEG / EXR / HDR / TIFF are leaf assets in the §4.1.10 dependency graph). |

The texture importer is a **leaf** in the dependency graph at
MVP. Unlike `fbx-importer-design.md` §7.4 (which surfaces
texture-path → mesh `DependencyEdge`s), the texture importer
imports nothing transitively — there are no embedded sub-asset
references in the five MVP texture formats that require external
resolution. EXR's optional embedded LUT and ICC profiles are
refused per §3.6. This simplifies the cook session's stage-6
edge-emission for texture cooks: zero edges per texture cook.

If a future format with cross-asset references lands (e.g. KTX2
with a sibling color-management profile), it would re-enter
post-MVP under a `vocab_version` bump and add `material_refs`-shaped
edges at that time.

### 7.5 `NormalizeParams` is not directly persisted

The §3.4 vocabulary's canonical-bytes encoding is hashed into
`CookKey.normalize_params_hash` (SPEC §7.1.2 component #4). The
bytes themselves are **not** persisted — the manifest holds only
the hash (SPEC §7.1.2 audit-only field set). This matches
`fory-codegen.md`'s ABI principle: the manifest records hashes,
not raw inputs.

### 7.6 Plugin ABI seam — none crossed by importer

The importer is build-time-only (SPEC §6.5). The runtime plugin
ABI hash (`glibre_types_abi_hash`, `plugin-abi.md`) does not
change with importer-only edits unless the importer's edits also
bump a Fory schema in `data/schemas/`. The §3.2
`importer_version` digest is **internal to content's cook
pipeline**; it does not appear in the plugin manifest's
`abi_hash` field (`plugin-abi.md` schema). A runtime hot-reload
of `glibre.content` (SPEC §8 plugin-code reload) sees no
importer activity — the runtime cuts (§5.3) elide the FreeImage
linkage entirely.

The cook tool's own re-link with a new FreeImage / codec
version is **not** a runtime hot-reload; it is a workspace
re-tooling event. Operators distribute new cook-tool builds
out-of-band; the runtime is unaware.

## 8. Hot-reload

The aggregate is build-time-only (SPEC §6.5); the runtime never
loads it. Hot-reload contributes through two indirect paths.

### 8.1 Manifest-swap path (the dominant content hot-reload)

When a `.png` / `.jpg` / `.jpeg` / `.exr` / `.hdr` / `.tif` /
`.tiff` source changes on disk, the watcher delivers a
`FileEvent` (SPEC §4.1.10) → `WatchEdge::translate` produces
`RecookRequest`s → `CookSession::commit()` runs the §3.7
pipeline on the worker pool → CAS write → manifest staging →
loader's phase-8 publish (SPEC §8.2 steps 1–5).

The importer's role in this path is exactly stages 1–7 of §3.7;
no hot-reload-specific code appears here. The importer is a
**producer of bytes**; the publish that makes the bytes
hot-reload-visible is the cook session's + loader's job.

What survives the manifest swap (per SPEC §8.3 row table) on
the importer's side:

- **`FreeImageImporter` instances**: do not survive (cook session
  worker pool destroyed by the swap if the swap is a
  content-plugin reload; on a manifest-only swap, they live across
  because no plugin code moves).
- **FreeImage process-global init state**: process-wide; survives
  every manifest-only swap. Survives plugin-code reloads only if
  the new plugin re-references the library (the atomic refcount
  dance in §3.5 handles re-init naturally).
- **In-flight `import_one` precursor bytes**: live in the
  per-cook arena which is itself bounded by the cook session's
  lifetime — a manifest-only swap that happens between cook
  sessions sees no in-flight arena.

A manifest-only swap that lands while a cook session is
mid-flight is **safe** by construction: the importer's outputs
are parked in the staging table (SPEC §6.2 stage 6), never
observed by the runtime until the staged blob is `rename(2)`'d
at the next phase 8. The phase-8 swap publishes only fully-staged
sessions (SPEC §4.1.9 inv #1, all-or-nothing). Mid-cook
concurrency is therefore irrelevant to the swap's atomicity.

### 8.2 Plugin-code reload path (rare)

If `glibre.content.dylib` itself swaps (SPEC §8.3 plugin-code
reload path), the engine-wide protocol
(`hot-reload-protocol.md`) runs: drain → swap → migrate → resume.

**Does the texture importer participate?** Only indirectly:

1. **`drain` step (SPEC §8.3 row "In-flight RecookRequest queues
   / CookSession worker pools").** The current cook session's
   worker pool is destroyed; in-flight `import_one` calls
   observe cancellation via the `CancellationToken` (§3.7
   stage 4) and return `ImporterError::Cancelled`. The arena
   is drained as part of session abort.
2. **`swap` step.** No importer code is loaded into the runtime
   (SPEC §6.5). The runtime swap does not move FreeImage
   symbols. The importer is unaffected.
3. **`migrate` step.** The importer authors no migration body
   (§7.4); migrations on the manifest layer (`ManifestEntry`,
   `CookKey`, `DependencyEdge`) are §8.4 of SPEC. The importer
   is unaffected.
4. **`register` step (SPEC §8.4 last paragraph).** The new
   content plugin's register re-spawns the cook worker pool.
   New `FreeImageImporter` instances are constructed by the new
   pool; old instances (destroyed in drain) leave no residue.

The importer's plugin-code reload contract is therefore: **no
body runs across the reload**. The §3.10 `~FreeImageImporter`
runs at drain; new instances start fresh at register. The
process-global FreeImage library state (`FreeImage_Initialise`'d
plugin dispatch + `FreeImage_SetOutputMessage` callback)
persists across the reload only if some `FreeImageImporter`
instance was alive across the swap; the §3.5 atomic refcount
otherwise re-initializes the library on the first `create()`
inside the new plugin. Both paths converge on the same
post-init state.

### 8.3 Refusal cases (importer-side)

The importer contributes the eight refusal causes already
pinned in SPEC §8.5 Class B:

| Refusal cause                                              | Stage triggering | Returned arm                                                      |
|------------------------------------------------------------|------------------|-------------------------------------------------------------------|
| Path escapes `assets/source/`                               | 1                | `ImporterError::SourceNotFound`                                   |
| File missing at `platform::FileIo::read_into` time          | 1                | `ImporterError::SourceNotFound`                                   |
| FreeImage reports `FIF_UNKNOWN` (magic mismatch)            | 1                | `ImporterError::MagicMismatch` (`freeimage-magic`)                |
| Probed FIF disagrees with `SourceAsset.format.texture`      | 1                | `ImporterError::MagicMismatch` (`freeimage-format-mismatch`)      |
| Codec-version unsupported (DWAA / JPEG-in-TIFF / APNG / multi-page TIFF / EXR multipart) | 2 | `ImporterError::UnsupportedVersion` (`freeimage-{exr-dwa-codec, tiff-jpeg-codec, apng-multiframe, tiff-multipage, exr-multipart}`) |
| libpng / libjpeg-turbo / libtiff / OpenEXR / HDR corruption | 2                | `ImporterError::MalformedPayload` (`freeimage-{png,jpeg,tiff,exr,hdr}-corruption`) |
| `NaN` / `signaling NaN` in HDR / EXR / float-TIFF channel  | 5                | `ImporterError::MalformedPayload` (`freeimage-nan-pixel`)         |
| Cancellation observed                                       | 2 / 4            | `ImporterError::Cancelled`                                        |

All eight surface as `CookOutcome::RolledBack` from the cook
session (SPEC §4.1.9 inv #1). The prior manifest snapshot
remains active per SPEC §8.5 Class B. Operator action is the
§10.1 per-arm column ("Re-export at supported codec-version",
"Restore source", "Repair sRGB chunk", "Inspect source for NaN
authoring", etc.).

The importer never refuses **the swap itself** — that is the
plugin-code reload's domain. Class A (cooker version mismatch,
expected re-cook trigger) is a normal §7.2.2 path; the importer
participates by emitting a fresh `importer_version` ingredient
that mismatches the recorded `CookKey` (§3.2 + §7.2 above). No
specific importer arm fires; the cook session simply re-runs
every dependent.

### 8.4 What `migrate(...)` body the importer owns

**None.** The importer authors no Fory schema (§7.4) and
contributes no migration body. The schema-migration plumbing
(`ManifestEntry` additive, `CookKey` re-cook, `DependencyEdge`
additive) lives in SPEC §7.2 / §8.4 and is owned at the
manifest aggregate's seam.

## 9. Performance

### 9.1 Cell allocation (citation)

`reviews/decisions/perf-budget.md` Per-Context Budget Table
assigns `content` the cell **0.20 ms CPU sim + 0.00 ms CPU
submit + 256 MiB heap**, with phase ownership "residency /
streaming; one-shot import work is off-thread". SPEC §9.1
refines the gate threshold to **0.50 ms** absorbing
one-shot-import-handoff variance. The texture-importer
aggregate's contribution to that cell:

- **Driver-thread CPU**: **0 ms steady-state, 0 ms peak** (SPEC
  §9.3 row `Importer (§4.1.2)`). The aggregate runs entirely
  off the driver thread (SPEC §9.3.1, §6.1 above). The cell is
  unaffected.
- **Off-thread CPU**: bounded by the cook worker pool's
  wall-clock per cook (~60–300 ms per 4K RGBA8 PNG; §5.1
  above). The perf-budget gate does **not** measure this —
  `perf-budget.md`'s S3 fixture asserts "the **driver
  thread**'s p99 phase-1 + phase-9 cost stays inside the 0.50
  ms content cell" *while* an import runs. The importer's own
  latency is bounded operationally (acceptable on dev workflows;
  CI gates triage in §9.3 below).
- **Heap (driver-side)**: 0 MiB (SPEC §9.3 row `Importer` /
  `0 MiB`).
- **Heap (off-thread soft sub-ceiling)**: **64 MiB** (SPEC
  §9.3.1). The aggregate's per-cook arena lives here; the
  breach is `warn` in shipping, `OutOfBudget` in
  `GLIBRE_ALLOC_STRICT=1`. Joint accounting per §6.3:
  `arena_tag_bytes + freeimage_rss_delta`.

### 9.2 Per-stage budget

Per §5.1 above, restated as a budget table with the relevant
gate (4K RGBA8 PNG reference workload):

| Stage                        | CPU bound (4K RGBA8 PNG, M1 firestorm) | Heap bound (per cook) | Gate                          |
|------------------------------|----------------------------------------|-----------------------|-------------------------------|
| 1 Source load + probe        | ≤ 5 ms                                  | ~16 MiB (source bytes; arena-tagged) | Off-thread; not gate-measured |
| 2 FreeImage decode           | ~ 30–200 ms                             | ~16 MiB (FIBITMAP; default heap, RSS-delta) | §9.3.1 soft 64 MiB joint accounting |
| 3 Pre-decode normalize       | ~ 1–10 ms                               | ≤ 1 MiB                                | Off-thread; not gate-measured |
| 4 Metadata + cancel poll     | ≤ 1 ms                                  | < 1 KiB                                | Cancel ≤ 5 ms latency         |
| 5 Canonicalization           | ~ 20–80 ms                              | ~16 MiB (canonical_pixels_; arena)     | Determinism golden            |
| 6 Precursor emit             | ~ 5 ms                                  | ~ 5 MiB                                | Per-call returned span        |
| 7 FreeImage teardown         | ≤ 1 ms                                  | (frees ~16 MiB FIBITMAP)               | Off-thread; not gate-measured |
| **per-cook total**           | **~ 60–300 ms**                         | **~ 50 MiB peak (joint)**              | §9.3.1 soft ceiling 64 MiB   |

For a 4K RGBA32F EXR (~64 MiB source, ~256 MiB decoded), stage 2
drives the joint accounting above the 64 MiB soft sub-ceiling and
fires the warn. This is the design's deliberate operational
admission: very-large HDR sources are post-MVP-friendly with the
soft ceiling visible in the cook tool's progress UI; a pathological
case (32K HDR EXR, ~hundreds of MiB) is accepted as a "split the
source before commit" operator call.

The per-cook bounds are **operational targets**, not perf-gate
asserts — the perf gate measures driver-thread cost, not
worker-thread wall-clock. A pathologically-slow texture (32K
RGBA32F EXR, deeply compressed PNG with ~zlib max-effort) extends
stage 2 / stage 5 wall-clock without breaking any gate; the
operator observes it as "the cook took longer than expected" via
the cook tool's progress UI (out of scope; future editor concern).

### 9.3 CI gate hooks

Per `perf-budget.md` §"CI Gate Spec" + SPEC §9.5:

1. **`content/import: scratch_arena_off_thread_residency`**
   (SPEC §9.5 row 4). Asserts the per-cook arena's peak
   resident bytes ≤ 64 MiB on the texture-importer reference
   fixture (4K RGBA8 PNG, ~16 M pixels; ~50 MiB joint peak).
   PR fails if exceeded. Shared with the FBX importer's gate
   (per `fbx-importer-design.md` §9.3 #1) — both write into
   the same `BENCHMARK_CELL` slot; the joint test exercises
   each importer's worst-case fixture.
2. **`content/import: s3_off_thread_no_driver_spike`** (SPEC
   §9.5 end-to-end gate). Drives the cook worker pool with
   the texture-importer reference fixture (4K RGBA8 PNG) for
   600 frames while S1 plays on the driver thread; asserts
   the driver thread's p99 phase-1 + phase-9 cost stays
   inside the 0.50 ms content cell. PR fails if exceeded.
   This is the gate that proves the texture importer does not
   bleed onto the game loop.
3. **`content/import: texture_determinism_golden`** (cross-cite
   to §11 below). Microbenchmark + golden test: cook the
   same texture bytes twice (different runs of the cook tool),
   assert the resulting precursor bytes are byte-equal. This
   is the PHILOSOPHY §7 cross-host determinism gate applied
   to the texture path.
4. **`content/import: texture_cancellation_latency`**
   (cross-cite §11). Asserts that `import_one` returns
   `ImporterError::Cancelled` within ≤ 10 ms of `cancel()`
   being called on a 4K PNG mid-decode. The 10 ms bound
   exceeds the ~5 ms theoretical bound from §6.4 to admit CI
   noise.

### 9.4 What is not gated

The per-cook wall-clock (~60–300 ms) is **not** gated. Reasons:

- It is wall-clock dominated by the FreeImage codec
  (libpng's zlib pipeline, libjpeg-turbo's IDCT, libtiff's
  predictor unwind, OpenEXR's DWA decode), which we do not
  optimize.
- It varies across hosts (CI vs developer machine) and across
  source size; a tight gate would flap on any source larger
  or smaller than the reference fixture.
- It is operationally visible to the developer (cook tool's
  progress UI) and to the spike #146 brief's S3 fixture (which
  exercises an FBX, not a texture, but the off-thread
  invariant is the load-bearing gate).

A future tightening could land if a regression spike opens; not
this design's concern.

### 9.5 Driver-thread invariant

The single perf invariant the texture importer must hold:

> **Zero importer activity on the game-loop driver thread,
> regardless of how many texture cooks are in flight on the
> worker pool.**

This is asserted by the §9.3 gate #2
(`s3_off_thread_no_driver_spike`) and structurally by SPEC
§9.3.2's exhaustive driver-thread hot-path list excluding the
importer. Any future change that would invoke `import_one` from
the driver thread is rejected at SPEC §9.3.2 and SPEC §6.5 cut
review.

## 10. Failure modes

The importer's failure surface is the closed sum
`glibre::content::ImporterError` declared in SPEC §5.3 and
§10.1. The §10.1 per-arm contract is the source of truth; this
section restates the importer-specific arms with
implementation-grain detail and pins the codec-error → arm
mapping at the carve-out (SPEC §10.3).

**Cross-boundary wrapping chain** (per `error-model.md`
§Composition Rules 1–2, mirroring `fbx-importer-design.md`
§10): `ImporterError` values are leaves of the content
context's error sum. At the `content` plugin's public boundary,
the `Result<eastl::span<const std::byte>>` returned by
`import_one` carries a `glibre::Error` whose variant holds a
`glibre::content::Error` (the
`eastl::variant<ImporterError, ResidencyError>` declared in
SPEC §5.3). The `Result<T>` alias in §5.4 is
`std::expected<T, glibre::Error>` per `error-model.md`; the
wrapping happens at the cook session's boundary (SPEC §6.2),
not inside `import_one` itself — the importer returns the inner
arm value and the cook session wraps it for callers outside the
content context. This means `ImporterError` is internal to
`content`; external callers see only the `glibre::Error` union
arm.

### 10.1 Importer-emitted arms (subset of `ImporterError`)

The texture-importer aggregate emits exactly the five
`ImporterError` arms below — `MissingDependency` is emitted by
the manifest-publish path (SPEC §10.1) and is not owned here
(textures are leaf assets; §7.4).

| Arm                                | Emit site (stage)            | `error.detail` prefixes                                                                                                                                          | Severity (SPEC §10.5) |
|------------------------------------|------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------|
| `ImporterError::SourceNotFound`    | §3.7 stage 1                 | `"freeimage-not-found"`, `"platform-io-missing"`                                                                                                                  | `warn`                |
| `ImporterError::MagicMismatch`     | §3.7 stage 1                 | `"freeimage-magic"`, `"freeimage-format-mismatch"`                                                                                                                | `warn`                |
| `ImporterError::UnsupportedVersion`| §3.7 stage 2                 | `"freeimage-{exr-dwa-codec, tiff-jpeg-codec, apng-multiframe, tiff-multipage, exr-multipart}"`                                                                     | `warn`                |
| `ImporterError::MalformedPayload`  | §3.7 stages 1, 2, 5; carve-out catch-all | `"freeimage-{png,jpeg,tiff,exr,hdr}-corruption"`, `"freeimage-nan-pixel"`, `"freeimage-alloc-exhausted"`, `"freeimage-unclassified"`, `"platform-io"`            | `warn` for corruption / nan; `error` for `"freeimage-alloc-exhausted"`, `"platform-io"` |
| `ImporterError::Cancelled`         | §3.7 stages 2, 4             | `"freeimage-cancelled"`                                                                                                                                          | `debug`               |
| `ImporterError::MissingDependency` | (never emitted by texture importer; manifest-side only) | n/a                                                                                                                                  | (n/a)                 |

The structured-log fields the importer attaches (per SPEC
§10.5):

- `asset_id`: the `AssetId` the cook is on (from the cook
  session's scheduling).
- `source_path`: `SourceAsset.path` (workspace-relative).
- `importer_kind`: literal `"freeimage"`.
- `importer_version`: 64-char BLAKE3 hex of `version_` (§3.2).
- `cook_session_id`: opaque from the cook session.
- `error.detail`: per the prefix table above.
- (For all FreeImage-codec-attributable arms)
  `error.codec`: literal `"libpng"` / `"libjpeg-turbo"` /
  `"libtiff"` / `"openexr"` / `"hdr"`. Required so telemetry
  can spot per-codec regressions even though the
  `ImporterError` enum does not discriminate by codec (per
  SPEC §10.3 closed-sum rule).
- (For `MalformedPayload`) `error.byte_offset`: the codec's
  reported parse offset, parsed from the FreeImage callback
  message when available, else zero.
- (For `MalformedPayload(freeimage-nan-pixel)`)
  `error.pixel_index` / `error.channel_index`: the offending
  pixel's location.

### 10.2 Codec error-callback translation table

FreeImage 3.x reports plugin-decoder errors via the global
`FreeImage_OutputMessageFunction` callback set with
`FreeImage_SetOutputMessage`. There are **no exception classes**
thrown by FreeImage's C surface (the library is a C ABI); rare
exceptions can leak from the linked C++-internal codec
implementations (OpenEXR's `IEX_*` exceptions, libtiff's
`std::exception`-derived TIFFFatalError on certain builds). The
carve-out's `try` block catches both the C-callback-driven
errors (read after each FreeImage call) and any thrown
exceptions in the same single block.

Rows 1-7 cover **C-callback-reported errors** — read by
inspecting `tl_active_error.detail` after each FreeImage call
that may fire the callback. Rows 8-9 cover thrown exceptions
inside the `try` block.

Matches SPEC §10.3's classification table verbatim with
one-cause-per-row precision:

| Codec error source                                                            | Translated arm                                | `error.detail` prefix                | Notes                                                                                       |
|-------------------------------------------------------------------------------|-----------------------------------------------|--------------------------------------|---------------------------------------------------------------------------------------------|
| FreeImage `FIF_UNKNOWN` from `FreeImage_GetFileTypeFromHandle` (return-value)  | `ImporterError::MagicMismatch`                | `"freeimage-magic"`                  | `GetFileTypeFromHandle` returns sentinel; no callback fires; classify on return value.       |
| Probed FIF disagrees with `SourceAsset.format.texture` (return-value)          | `ImporterError::MagicMismatch`                | `"freeimage-format-mismatch"`        | Caught at stage 1's post-probe check; preserves SPEC §3.2's "extension is hint, not truth". |
| libpng error callback fires (CRC, IDAT chunk, sRGB chunk)                      | `ImporterError::MalformedPayload`             | `"freeimage-png-corruption"`         | `error.byte_offset` parsed from `"libpng error: ... offset X"` when present.                |
| libjpeg-turbo error callback (truncation, scan-line failure)                   | `ImporterError::MalformedPayload`             | `"freeimage-jpeg-truncation"`        | Subsumes "Premature end of JPEG file" / "Bogus huffman table" / etc.                         |
| libtiff error callback (corruption, multi-page rejection)                      | `ImporterError::MalformedPayload` / `UnsupportedVersion` | `"freeimage-tiff-corruption"` / `"freeimage-tiff-multipage"` | Multi-page detection via probe of `TIFFNumberOfDirectories` post-load (defensive). |
| OpenEXR exception or callback (corruption, multipart, DWA-codec absent)        | `ImporterError::MalformedPayload` / `UnsupportedVersion` | `"freeimage-exr-corruption"` / `"freeimage-exr-multipart"` / `"freeimage-exr-dwa-codec"` | Caught both via `tl_active_error` and exception path — OpenEXR is C++ internally and may throw. |
| HDR / Radiance loader error                                                    | `ImporterError::MalformedPayload`             | `"freeimage-hdr-malformed"`          | The Radiance HDR plugin uses simple text-header parsing; truncated headers fire the callback. |
| `std::bad_alloc` thrown from any FreeImage codec                               | terminate (§10.3 `std::bad_alloc` row; SPEC §10.7 'bad_alloc from importer SDKs' resolved → terminate) | n/a                  | The per-cook arena is sized to fit the §9.3.1 ceiling; bad_alloc inside it is a defect.     |
| Any other `std::exception` derivative thrown from FreeImage / codec code       | `ImporterError::MalformedPayload`             | `"freeimage-unclassified"`           | The catch-all arm. The exception's `what()` is logged at `warn` level for triage.            |
| Any non-`std::exception` thrown object                                          | terminate                                      | n/a                                  | Non-`std::exception` cannot be classified; the carve-out's `catch(...)` terminates.        |

The catch-all `MalformedPayload(freeimage-unclassified)` arm
exists because FreeImage's C callback does not document a
complete error-message taxonomy; the catch-all preserves the
§3.2 collapse #1 commitment ("symptom-by-arm, not codec-by-arm")
even when a codec emits a callback message the table did not
anticipate.

### 10.3 Exception scope — the unique `-fexceptions` carve-out

Per `error-model.md` §"Decision" rule 3 + §"Consequences" last
bullet, the texture importer's implementation TU is one of three
files in the engine that compile with `-fexceptions` (matching
SPEC §10.3 verbatim):

```
plugins/content/cook/import/fbx_importer.cpp        # sibling spike #807
plugins/content/cook/import/freeimage_importer.cpp  # this design
plugins/content/cook/import/freetype_importer.cpp   # sibling spike #811
```

The header `freeimage_importer.hpp` and **every other TU in the
engine** compiles with `-fno-exceptions`. The build system
enforces this via a per-TU compile-flag override in the cook
tool's CMakeLists; raw `add_executable(glibre-cook ...)`
configures `-fno-exceptions` globally and the override is
per-source `set_source_files_properties`.

The `try` / `catch` block inside `import_one` is the **single**
translation site. The catch arms:

```cpp
// Sketch — final shape lives in freeimage_importer.cpp.
auto FreeImageImporter::import_one(const SourceAsset& src,
                                   const NormalizeParams& params,
                                   ImporterArena& out_arena,
                                   const CancellationToken& cancel) noexcept
    -> Result<eastl::span<const std::byte>>
{
    // The C-callback's fired errors are read from tl_active_error.detail
    // after each FreeImage call; classify_freeimage_status(...) translates
    // into the §10.2 ImporterError arm immediately.
    // No catch arm for "FreeImage_GetLastError" exists — FreeImage has no such
    // function; the callback IS the error surface for the C side.
    try {
        // Stages 1..6 inline; FreeImage callback drained per §10.2.
        return emit_precursor(...);
    }
    catch (std::bad_alloc&) {
        std::terminate();                 // §10.2 row 8
    }
    catch (const std::exception& e) {
        return std::unexpected{ Error{
            ImporterError::MalformedPayload,
            ErrorContext{ __FILE__, __LINE__, "freeimage-unclassified" } }};
    }
    catch (...) {
        std::terminate();                 // §10.2 row 10
    }
    // Stage 7 (FreeImage teardown) is in a finally-equivalent guard
    // (RAII helper) so it runs on every path including the catches.
}
```

The RAII guard uses the `GLIBRE_DEFER` macro authored in
`core/include/glibre/defer.hpp` — the same macro
`fbx-importer-design.md` §10.3 introduces (and which the FBX
importer's task-breakdown follow-up will author). This design
**does not re-author** `defer.hpp`; it depends on it. The
guard body invokes `FreeImage_Unload(fi_bitmap_)` per §3.7
stage 7. Example usage in `import_one` (capturing the FIBITMAP
pointer by value for scope independence):

```cpp
auto _fi_cleanup = GLIBRE_DEFER(
    [fi_bitmap = fi_bitmap_]() noexcept {
        if (fi_bitmap) FreeImage_Unload(fi_bitmap);
    }
);
```

The guard also clears `tl_active_impl` (§3.5) so a spurious
callback after `import_one` returns sees the null guard and
short-circuits:

```cpp
auto _tls_cleanup = GLIBRE_DEFER(
    []() noexcept { tl_active_impl = nullptr; }
);
```

`GLIBRE_DEFER` is preferred over a `std::function`-based guard
(PHILOSOPHY §11 bans `std::function` outside of EASTL; the lambda
+ template parameter approach has zero overhead at the call site).
Authoring `defer.hpp` is a prerequisite shared with
`fbx-importer-design.md` §10.3; the implementation plan produced
by either task-breakdown spike provides it once.

### 10.4 Recovery posture

Per SPEC §10.4 recovery-posture map, every importer arm falls
into **Refuse**:

| Arm                        | Posture        | Mechanism                                              |
|----------------------------|----------------|--------------------------------------------------------|
| `SourceNotFound`           | Refuse         | Cook step rolls back; prior manifest stays active.    |
| `MagicMismatch`            | Refuse         | Same.                                                  |
| `UnsupportedVersion`       | Refuse         | Same. Operator re-exports from authoring tool with supported codec / non-multipart layout. |
| `MalformedPayload`         | Refuse         | Same. `error.detail` discriminates sub-cause.          |
| `Cancelled`                | Refuse (no-op) | No automatic retry; operator re-triggers.              |

There is **no Re-cook** posture from the importer side — re-cook
is the manifest-publish path's response to `MissingDependency`
(which the texture importer does not emit; §7.4 — textures are
leaf assets) or `HashNotInCas` (residency-side, not
importer-side). The importer is purely a producer of bytes that
either succeed or refuse with a typed arm.

### 10.5 What the importer does NOT emit

To make the boundary explicit:

- **`ImporterError::MissingDependency`** is emitted by the
  manifest-publish path (`cook/publish.cpp`, SPEC §6.2 manifest-
  publish paragraph) when the bottom-up dependency walk finds an
  edge to a child whose `AssetId` is not in the active manifest.
  The texture importer surfaces no dependencies (§7.4); it never
  contributes to a missing-dependency event.
- **`ResidencyError::*`** (SPEC §5.3) are emitted by the
  runtime's residency manager (SPEC §4.1.7), not by the importer.
  The cook tool may emit `ResidencyError::IoFailure` from
  `cas/atomic_write.cpp` during stage 5 of the cook step (CAS
  write), but that is the cook step's emit site, not the
  importer's.
- **`core::Error::*`** arms (`PluginAbiHashMismatch`,
  `SchemaMigrationFailed`, etc.) are loader-side and only
  relevant to the runtime plugin reload (§8.2 above). The
  importer never emits these.

## 11. Test plan

The importer's tests live under `tests/content/import/texture/`.
Five classes of tests cover the §3 / §6 / §10 contracts; each
Catch2 case name below is the gate-asserted name.

### 11.1 Unit tests (Catch2)

**`content/import/texture: dispatch_routes_texture_to_freeimage`**.
Asserts that
`dispatch_import(SourceAsset{kind=Texture, format=Png})` and the
four sibling format enumerators all route to
`FreeImageImporter::import_one`. Exercises SPEC §4.1.2 inv #1
(closed-sum dispatch) and SPEC §6.1 `importers/dispatch.cpp`.

**`content/import/texture: rejects_path_outside_source_root`**.
Asserts that a texture whose `SourceAsset.path` resolves outside
`assets/source/` returns `ImporterError::SourceNotFound`. Exercises
SPEC §4.1.1 inv #1.

**`content/import/texture: rejects_unknown_magic`**. Asserts that
a non-image byte stream renamed `.png` returns
`ImporterError::MagicMismatch` with
`error.detail = "freeimage-magic"`. Exercises §3.7 stage 1.

**`content/import/texture: rejects_format_mismatch_extension_vs_magic`**.
Asserts that a JPEG renamed to `.png` returns
`ImporterError::MagicMismatch` with
`error.detail = "freeimage-format-mismatch"`. Exercises §3.7 stage
1's post-probe check.

**`content/import/texture: rejects_apng_multiframe`**. Asserts
that a multi-frame APNG returns
`ImporterError::UnsupportedVersion` with
`error.detail = "freeimage-apng-multiframe"`. Exercises §3.6.

**`content/import/texture: rejects_tiff_multipage`**. Asserts
that a multi-page TIFF returns
`ImporterError::UnsupportedVersion` with
`error.detail = "freeimage-tiff-multipage"`. Exercises §3.6.

**`content/import/texture: rejects_exr_multipart`**. Asserts that
a multipart EXR returns `ImporterError::UnsupportedVersion` with
`error.detail = "freeimage-exr-multipart"`. Exercises §3.6.

**`content/import/texture: rejects_nan_pixel_in_hdr`**. Asserts
that a hand-authored HDR / EXR with a `NaN` channel returns
`ImporterError::MalformedPayload` with
`error.detail = "freeimage-nan-pixel"`, plus
`error.pixel_index` / `error.channel_index` set. Exercises §3.7
stage 5.

**`content/import/texture: cancellation_observed_within_10ms`**.
Drives a 4K RGBA8 PNG through `import_one` while a sibling
thread calls `cancel()` mid-decode; asserts return value is
`ImporterError::Cancelled` within 10 ms wall-clock. Exercises
SPEC §4.1.2 inv #5 and §6.4.

**`content/import/texture: produces_byte_equal_precursor_across_runs`**.
Cooks the same texture bytes twice in two fresh cook tool
processes; asserts the returned precursor bytes are byte-equal.
Exercises PHILOSOPHY §7 + SPEC §4.2 inv #1 + §3.7 stage 5
(determinism canonicalization). One case per format
(`png_byte_equal`, `jpeg_byte_equal`, `exr_byte_equal`,
`hdr_byte_equal`, `tiff_byte_equal`).

**`content/import/texture: importer_version_changes_on_freeimage_bump`**.
Asserts that `Importer::version()` returns a different digest
when the build is configured with a different `FREEIMAGE_TAG`
or any of the four `LIBPNG_TAG` / `LIBJPEG_TAG` / `LIBTIFF_TAG`
/ `OPENEXR_TAG` components. Exercises §3.2.

**`content/import/texture: scratch_arena_residency_under_64mib_on_4k_png`**.
Cooks the 4K RGBA8 PNG reference fixture and asserts joint
peak residency (`arena_tag_bytes + freeimage_rss_delta`) ≤ 64
MiB. Exercises SPEC §9.3.1 + §9.2 above.

**`content/import/texture: srgb_decode_matches_libpng_curve`**.
Cooks a hand-authored sRGB PNG with known tristimulus values;
asserts the decoded canonical `RGBA8_sRGB` values match the
libpng-published sRGB-curve reference (per the linked
libpng version's IC chain). Exercises §3.4
`srgb_decode_mode = IcChain` and §3.5 layout-selection table
row 1.

**`content/import/texture: hdr_clamp_max_clamps_channels`**.
Cooks an HDR fixture with a known peak-luminance pixel; asserts
the clamped channel matches `min(channel, hdr_clamp_max)` after
`NormalizeParams.hdr_clamp_max = 8.0`. Exercises §3.4 +
§3.7 stage 5.

**`content/import/texture: alpha_premultiply_applies_to_rgb`**.
Cooks an authored RGBA8 PNG with a known per-pixel `(R, G, B, A)`
tuple; asserts the canonical pixel buffer carries
`(R * A / 255, G * A / 255, B * A / 255, A)` after
`alpha_premultiply = true`. Exercises §3.4 + §3.7 stage 5
(LUT-driven divide).

**`content/import/texture: discard_alpha_if_opaque_strips_channel`**.
Cooks an opaque RGBA PNG (every alpha == 255) with
`discard_alpha_if_opaque = true`; asserts the precursor's
`pixel_layout` is the 3-channel variant
(`Rgb8_sRGB`) and `metadata.has_alpha == false`. Exercises §3.4
+ §3.7 stage 5.

**`content/import/texture: layout_force_downcast_rgba32f_to_rgba8`**.
Cooks an EXR with `output_layout = RGBA8`; asserts the
canonical buffer is u8 with `clamp(x, 0, 1) * 255` quantization.
Exercises §3.4 + §3.5 layout-conversion rule.

**`content/import/texture: per_thread_freeimage_no_shared_state`**.
Spawns 4 worker threads each running `import_one` on the same
texture fixture; asserts every worker's precursor is byte-equal
and no data-race / double-allocation triggers TSAN / ASAN.
Exercises §3.5 + §6.2.

**`content/import/texture: process_global_init_refcounted_safely`**.
Spawns 4 workers, each doing `create()` / `~FreeImageImporter()`
in interleaved order; asserts `FreeImage_Initialise` is called
exactly once and `FreeImage_DeInitialise` exactly once across
the test, via instrumented hooks. Exercises §3.5 atomic
refcount.

### 11.2 Codec error-callback translation tests

**`content/import/texture: translates_libpng_crc_error_to_malformed`**.
Hand-corrupts a PNG IDAT chunk's CRC; asserts arm +
`error.detail = "freeimage-png-corruption"` plus `error.codec =
"libpng"`. Exercises §10.2 row 3.

**`content/import/texture: two_consecutive_callbacks_last_error_wins`**.
Uses a fixture-injected FreeImage error-output function to fire
two callbacks in sequence on the same thread within a single
`FreeImage_LoadFromHandle` call (simulating libpng emitting a
chunk-level warning followed by a fatal CRC error). Asserts that
`tl_active_error.detail` after `classify_freeimage_status()` contains
the *second* (last) callback's message, not the first. Verifies the
last-error-wins semantic documented in `error_callback_dispatch` and
that no use-after-free or dangling-view occurs (run under ASAN).
Exercises §3.5 error-dispatch + PerCookError lifetime invariant.

**`content/import/texture: translates_libjpeg_truncation_to_malformed`**.
Truncates a JPEG mid-scan; asserts arm + `error.detail =
"freeimage-jpeg-truncation"` plus `error.codec = "libjpeg-turbo"`.
Exercises §10.2 row 4.

**`content/import/texture: translates_libtiff_corruption_to_malformed`**.
Corrupts a TIFF IFD offset; asserts arm + `error.detail =
"freeimage-tiff-corruption"` plus `error.codec = "libtiff"`.
Exercises §10.2 row 5.

**`content/import/texture: translates_openexr_corruption_to_malformed`**.
Corrupts an EXR scanline-block compressed payload; asserts arm +
`error.detail = "freeimage-exr-corruption"` plus
`error.codec = "openexr"`. Exercises §10.2 row 6.

**`content/import/texture: translates_hdr_malformed_to_malformed`**.
Corrupts a Radiance HDR header line; asserts arm + `error.detail
= "freeimage-hdr-malformed"` plus `error.codec = "hdr"`.
Exercises §10.2 row 7.

**`content/import/texture: terminates_on_bad_alloc_inside_arena`**.
Forces a `bad_alloc` by setting the arena ceiling to 1 KiB and
cooking the 4K PNG fixture; asserts process termination via
`std::terminate`. Exercises §10.2 row 8 + SPEC §10.7 OQ-2.

**`content/import/texture: catches_unclassified_std_exception`**.
Throws a custom `std::runtime_error` from a fixture-injected
FreeImage callback hook; asserts arm + `error.detail =
"freeimage-unclassified"`. Exercises §10.2 row 9.

**`content/import/texture: terminates_on_non_std_exception`**.
Throws an `int` from a fixture-injected callback; asserts
process termination. Exercises §10.2 row 10.

### 11.3 Format-coverage tests

The five admitted formats each get a round-trip test that asserts
the canonical pixel layout is the §3.5 table's expected layout,
the metadata fields match the source's authored values, and the
precursor's pixel buffer matches a known-good reference (cooked
once on a reference machine, checked into the test fixtures).

**`content/import/texture: png_8bit_rgba_canonical_layout`**.
Layout = `RGBA8_sRGB`; metadata.color_space = `sRGB`. §3.5 row 1.

**`content/import/texture: png_8bit_rgba_canonical_layout_row_order`**.
Uses an asymmetric test fixture: a 4×2 PNG where the top row is solid
red and the bottom row is solid blue (visually distinct when flipped).
Asserts that after cook the first canonical row is red (RGBA = [255, 0, 0, 255])
and the second is blue ([0, 0, 255, 255]), confirming the bottom-up
FreeImage convention is correctly inverted to engine-canonical top-down.
Run under ASAN. Exercises §3.7 stage 5 row-orientation reversal.

**`content/import/texture: png_16bit_rgba_canonical_layout`**.
Layout = `RGBA16F_Linear`. §3.5 row 2. Includes an assertion that
a known channel value above 2048 (e.g. raw uint16 = 4096) is
rounded to the nearest f16 representable value under `FE_TONEAREST`,
confirming the documented u16→f16 precision-loss behavior rather
than silently producing a wrong bit pattern.

**`content/import/texture: jpeg_canonical_layout`**.
Layout = `RGBA8_sRGB`; alpha forced to 255. §3.5 row 3.

**`content/import/texture: tiff_8bit_canonical_layout`**.
Layout = `RGBA8_Linear`; default `Auto` color_space = Linear.
§3.5 row 4.

**`content/import/texture: tiff_16bit_canonical_layout`**.
Layout = `RGBA16F_Linear`. §3.5 row 5.

**`content/import/texture: tiff_f32_canonical_layout`**.
Layout = `RGBA32F_Linear`. §3.5 row 6.

**`content/import/texture: exr_half_canonical_layout`**.
Layout = `RGBA16F_Linear`; bit-for-bit pass-through (modulo
`ban_nan`). §3.5 row 7.

**`content/import/texture: exr_full_canonical_layout`**.
Layout = `RGBA32F_Linear`. §3.5 row 8.

**`content/import/texture: hdr_canonical_layout`**.
Layout = `RGBA32F_Linear`; alpha forced to 1.0. §3.5 row 9.

**`content/import/texture: paletted_png_unpaletizes_via_freeimage`**.
A 4-bit paletted PNG cooks into `RGBA8_sRGB` after FreeImage's
`ConvertTo32Bits` runs at stage 3. Exercises §3.6 indexed-PNG
disposition + §3.7 stage 3.

**`content/import/texture: grayscale_replicates_to_rgb`**.
A 1-bit / 8-bit / 16-bit grayscale PNG / TIFF cooks into the
expected canonical layout with channel-replicated RGB and
opaque alpha. Exercises §3.6 grayscale disposition.

### 11.4 Integration tests with cook session

**`content/cook: texture_import_then_cas_write_then_manifest_publish`**.
End-to-end: cook one PNG through `CookSession::commit()`;
asserts (a) cook returns `CookOutcome::Published`, (b) one
CAS file appears under `cooked/<prefix>/<hash>`, (c) the
manifest entry resolves the asset's `AssetId` to that hash, (d)
the manifest emits zero `DependencyEdge`s for the texture
(textures are leaf assets per §7.4). Exercises SPEC §6.2 stages
1–6 with the texture importer in the dispatch slot. Cross-link:
`cas-store-design.md` (#813) will pin the CAS-side details once
its design lands.

**`content/cook: texture_import_rolls_back_on_corruption`**.
Cooks a corrupt PNG; asserts (a) cook returns
`CookOutcome::RolledBack`, (b) no CAS file is published (the
temp file may exist but the final path does not), (c) the
prior manifest snapshot remains active. Exercises SPEC §4.1.9
inv #1 + §10.4 + §10.1 `MalformedPayload`.

### 11.5 Determinism gates (PHILOSOPHY §7)

**`content/import/texture: byte_equal_across_macos_and_linux_ci`**
*(deferred — conditional on prerequisites)*. Same texture
cooked on macOS and Linux CI; asserts byte-equal precursor
bytes (Linux CI uses FreeImage linked against the same
vcpkg-pinned codec versions). Deferred until the project's
Linux CI runner is provisioned (matching
`fbx-importer-design.md` §11.4 which has the same prerequisite).
Skipped in CI until then; replaced by
`byte_equal_across_two_runs_same_host`.

**`content/import/texture: byte_equal_across_two_runs_same_host`**.
Same as the unit case `produces_byte_equal_precursor_across_runs`,
elevated to the determinism gate so a regression flips the
`perf:headroom-low`-equivalent label per `perf-budget.md`
§"CI Gate Spec" rule 5.

### 11.6 Off-thread soundness

**`content/import: s3_off_thread_no_driver_spike`** (SPEC §9.5
end-to-end gate, restated here). The integration gate that
proves the importer does not bleed onto the driver thread;
exercised by running 600 frames of S1 with an active 4K PNG
texture cook on the worker pool. PR fails if driver-thread p99
phase-1 + phase-9 cost exceeds 0.50 ms. Shared with
`fbx-importer-design.md` §11.5 — both importers feed the same
end-to-end gate; the test fixture exercises whichever importer
the workload triggers.

This is the load-bearing gate against the §9.5 driver-thread
invariant.

## 12. Open questions

Three open questions remain at this design pass; each names
the gate that re-opens it. None block this design's PR.

**OQ-1. `NormalizeParams` typed view in §5.4 header** — Same
as `fbx-importer-design.md` §12 OQ-1, applied to the texture
importer. The §3.4 vocabulary defines a typed schema the
canonical-bytes encode; the §5.4 header currently exposes only
the opaque `canonical_bytes` span. A typed `NormalizeParamsView`
adaptor would give callers (the cook session, future tooling) a
checked decode path without re-implementing the canonical
reader. The adaptor's body lives in the codegen output
(`fory-codegen.md`); exposing it through the §5.4 header pulls
a Fory-codegen dependency into a header consumed by the
runtime. Resolution trigger: a second consumer needs typed
access (e.g. an editor's "per-cook params inspector"). Until
then, the cook session decodes the bytes through the codegen
header directly, and the §5.4 header stays Fory-free. The
texture importer's resolution is bundled with the FBX
importer's identical OQ. **Owner**: planning spike for editor
content-pipeline UI; **gate**:
`task-breakdown-content-texture-importer-detailed` follow-up
does not author the typed view.

**OQ-2. Mip chain ingest** — MVP refuses per §3.4 / §3.6.
Re-entry path: a future spike admits mip-1+ in the
`TextureArtifact` precursor's pixel-buffer list, bumps the
§3.4 `vocab_version`, and decides whether mip-chain
*construction* (Kaiser / box / Mitchell-Netravali filter) is
the importer's or `render`'s job. The current refusal commits
to **`render` owns mip-chain construction** (mip selection is
fused with BCn block compression in `render`'s upload seam);
re-opening this OQ is the trigger to revisit that boundary.
The path does not require a second `Importer` aggregate — it
extends this one under a `vocab_version` bump. **Owner**:
`render` plugin's texture-upload epic; **gate**: that epic's
landing.

**OQ-3. Animated PNG / multi-page TIFF / EXR multipart ingest**
— MVP refuses per §3.4 / §3.6. Re-entry path: a future spike
authors `data/schemas/content/AnimatedTextureArtifact.fory`
and / or `MultiLayerTextureArtifact.fory`; the texture
importer gains two more sub-precursor emitters at §3.7 stage
4 / 5; the `TextureArtifactPrecursor` schema bumps to include
optional references to those artifacts (or they become
independent `AssetId`s with `DependencyEdge` linkage). The
path does not require a second `Importer` aggregate — it
extends this one under a `vocab_version` bump. **Owner**:
animation-system epic and / or post-MVP UI epic; **gate**:
either epic landing.

These three OQs do **not** become spec residue — they are
deliberate post-MVP routing per PHILOSOPHY §5 (greatly reduced
MVP scope) + §10 (Occam — collapse in MVP, expand only on a
second consumer). Each carries its own re-opening gate; none
of them are content's residue (§12 SPEC), they are this
design's residue against the post-MVP epic landing.
