# content — Detailed Design: font-importer aggregate

> Detailed design for the `FreeTypeImporter` aggregate (cook-time
> entity) declared in `specs/content/SPEC.md` §4.1.2, with public
> surface locked in §5.4 and lifecycle / scratch / SDK seams pinned
> in §6.1 (`importers/freetype_importer.{hpp,cpp}`), §6.2 stage 3,
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
> `specs/content/fbx-importer-design.md` (sibling, merged via PR #905)
> and `specs/content/texture-importer-design.md` (sibling, PR #909):
> per-thread / TLS allocator dispatch, `importer_version` digest
> contributing to `CookKey`, build-time-only seam at a single
> `-fexceptions` carve-out TU, `GLIBRE_DEFER` stage-9 cleanup,
> normalize-then-canonicalize-then-emit pipeline. **Unlike** the
> texture importer (which had no allocator hook to FreeImage's
> internals), FreeType *does* expose a per-`FT_Library` `FT_Memory`
> hook, so this importer routes every byte FreeType allocates
> through the per-cook arena via per-thread library construction —
> closer to the FBX importer's `FbxSetMemoryAllocator` pattern, but
> at per-thread granularity instead of process-global because
> `FT_Library` itself is not thread-safe. Cross-links upcoming
> `specs/content/cas-store-design.md` (#813) and
> `specs/content/manifest-design.md` (#815) at the seams the importer
> participates in; both are dependencies of the eventual cook-step
> integration plan, not of this design.
>
> Harmonius prior art (`harmonius/docs/requirements/content-pipeline/
> asset-import.md`, R-12.1.1 / R-12.1.4 / R-12.1.5) cited as research
> input only — every conclusion below was independently re-derived
> per `PHILOSOPHY.md` §"How harmonius is used". Harmonius surfaced no
> font-specific clauses under R-12.1; the requirement to ship fonts at
> MVP comes from glibre's own `tools/glibre-editor` shell + future UI
> plugin needs (SPEC §3.2 collapse #1 last paragraph). Adjacent
> harmonius UI clauses (R-10.4.2 MSDF text glyphs, R-10.2.1 HarfBuzz
> rich-text shaping) are read for context only — they are render /
> ui domain concerns; this design refuses both.
>
> Refs: spike #811 — `[SPIKE] design-content-font-importer-detailed`.
> Parent: #806 (sub-epic — Detailed Designs — content). Sibling
> `[SPIKE] task-breakdown-content-font-importer-detailed` is blocked
> by this deliverable.

## 1. Purpose

The `font-importer` aggregate is the single component in the content
context permitted to link the FreeType library and to call into its
C surface. Its one responsibility is **decoding one
`SourceAsset { kind = Font, format ∈ {Ttf, Otf} }` value object into
the normalized in-memory bytes of a `glibre.content.FontArtifact`
precursor**, packaged as `eastl::span<const std::byte>` allocated
inside a caller-supplied `ImporterArena`, with every FreeType-reported
error (whether returned `FT_Error` integer or — exceedingly rare —
C++ exception leaked from a downstream codec the build links) caught
at first ingress and translated into a typed `ImporterError::*` arm,
and every byte the importer claims as its own allocated through the
per-cook arena (PHILOSOPHY §11). Concretely the aggregate owns:

1. The **`FreeTypeImporter` final entity** (SPEC §4.1.2) — the
   closed-sum member that serves `SourceKind::Font`, constructed
   at cook-session start through `FreeTypeImporter::create()` (SPEC
   §5.4) and destroyed at session end. One `FreeTypeImporter`
   instance per importer worker thread in the cook worker pool
   (§6.5 below); FreeType documents `FT_Library` as **not
   thread-safe** for concurrent face access from multiple threads,
   so the design pins one `FT_Library` per worker thread (each
   constructed from its own per-thread `FT_Memory` hook routed at
   the per-cook arena via TLS), not a process-wide singleton.
2. The **first-ingress codec seam** — every FreeType C-API call
   site in the engine lives inside `plugins/content/cook/import/
   freetype_importer.cpp` (SPEC §10.3, the unique `-fexceptions`
   carve-out for FreeType). FreeType is a C library and its public
   API returns `FT_Error` integers rather than throwing; the seam
   classifies every non-zero `FT_Error` into a typed `ImporterError::*`
   arm with a structured-log `error.detail` prefix per the §10.3
   classification table. The `try` / `catch` block exists for the
   rare exception that may leak from FreeType's optional WOFF2
   Brotli decompression path (`brotli` is C, but its build links
   `libstdc++` on some toolchains and the `std::bad_alloc` case is
   the only documented exception class that may surface) and for
   the `std::bad_alloc` defence-in-depth exit path.
3. The **decode → metric extraction → atlas-bake pipeline** — the
   deterministic walk over `FT_Face` that produces the
   `FontArtifact` precursor's in-memory layout: per-glyph metrics
   table (advance, bearing, bounding box, kerning pairs) plus a
   single CPU-side bitmap **or** SDF atlas image at a fixed pixel
   size, packed by a small deterministic skyline / next-fit-decreasing
   bin packer (§3.7 stage 6). The walk is pure-of-effect outside
   the per-cook arena (SPEC §4.1.2 inv #3) and respects the
   `CancellationToken` polled at every glyph load boundary (SPEC
   §4.1.2 inv #5).
4. The **`importer_version` digest** — a 32-byte BLAKE3 over the
   importer's compiled-in identity (FreeType library version +
   linked Brotli / zlib codec versions if WOFF/WOFF2 is enabled +
   normalize-params-vocabulary version + post-process / atlas
   options vocabulary), embedded as a string literal in the
   importer's translation unit at build time and returned through
   `Importer::version()` (SPEC §5.4). The digest is one of the
   five ingredients of `CookKey` (SPEC §4.1.3 component #2) and
   the rule that bumping the FreeType build (or any of its linked
   sub-codecs at MVP scope) forces re-cook of every dependent font.
5. The **per-cook arena lifetime** — `ImporterArena` (the typed
   alias over `glibre::PerContextAllocator` per `perf-budget.md`
   Allocator Rule 1, SPEC §9.3.1) holds every byte the importer
   declares as its own: the source-file buffer, FreeType's own
   internal allocations (re-routed through the per-thread
   `FT_Memory` hook installed at `FT_New_Library` time — §3.5
   below), the normalized glyph metrics table, the atlas bitmap
   buffer, and the precursor's emitter scratch. The arena drains
   once `CookSession`'s end-of-session rollback / publish path runs
   (SPEC §4.1.9 inv #1, #5) and never bleeds into the runtime heap.

This aggregate **refuses to own**:

- **Text shaping** — selecting glyph indices from a Unicode
  codepoint sequence according to OpenType GSUB/GPOS lookups,
  ligature substitution, contextual alternates, mark positioning,
  Indic / Arabic / Hangul cluster shaping. SPEC §3.3 routes complex
  shaping to a future UI plugin via HarfBuzz, *not* to the content
  context. The font importer emits **per-glyph** metrics keyed by
  glyph index, plus a Unicode-codepoint→glyph-index lookup table
  for the Basic Multilingual Plane subset the cook covers (§3.4
  `glyph_subset` field); HarfBuzz consumes the same `FT_Face` at
  runtime in the future UI plugin's path, but that path links
  FreeType independently — the cook artifact does not encode
  shaping state. Adjacent harmonius clause R-10.2.1 (HarfBuzz-
  compatible shaping) is explicitly an `ui` / `tools` domain
  concern, not content's.
- **Glyph layout / paragraph breaking** — line-breaking
  (Unicode UAX #14), bidi (UAX #9), text justification, hyphenation,
  per-line rasterization, run segmentation. All belong to the
  future UI plugin or to a `tools` text-edit subsystem; the
  content-cooked atlas is layout-agnostic (per-glyph; no
  pre-laid-out text runs). Adjacent harmonius clauses R-10.2.1
  (BiDi) and R-10.2.2 (multi-line input) are routed accordingly.
- **GPU upload, descriptor binding, residency, on-demand glyph
  rasterization at runtime** — SPEC §3.3 routes upload to `render`,
  residency to the residency manager (§4.1.7). The importer never
  sees a `MTLDevice*`, never allocates a Metal heap, never emits a
  descriptor handle, and never runs on the runtime side at all
  (§5 cuts). The atlas image emitted is consumed by `render`'s
  font subsystem (post-MVP) for upload; `render` decides texture
  format selection (R8 vs RG8 for SDF; A8 for bitmap; mip
  selection), upload cadence, and descriptor binding. The atlas
  is **CPU-side** at this layer.
- **Runtime glyph rasterization (i.e. live glyph cache)** — at
  MVP the cook bakes a fixed-subset atlas at one fixed pixel size;
  a later editor / UI workflow that demands arbitrary-size or
  arbitrary-codepoint glyphs at runtime would require either
  multiple cooked atlas variants or a runtime FreeType linkage
  inside `render` / `ui`. The latter path is **explicitly refused
  here** — adding live rasterization to the runtime would make
  FreeType part of the runtime ABI; SPEC §6.5 cut keeps that off
  the table. Re-entry path is post-MVP via either (a) cooking
  multiple sizes per font at the cook step (vocabulary bump on
  §3.4 `pixel_sizes`), or (b) a separate `ui` plugin with its own
  FreeType link line — content does not budget for either at MVP.
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
  (`FreeTypeImporter`).
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
  `SourceAsset`. (FreeType's `FT_New_Face(path)` path is bypassed;
  the importer always uses `FT_New_Memory_Face` over an
  arena-resident byte buffer, per §3.5 below.)
- **Source debounce / dedup** — the watcher's debounce window
  (SPEC §4.1.10 inv #2) collapses rapid resaves into one
  `RecookRequest` before any importer runs. The importer never
  sees back-to-back imports of the same `(path, source_hash)`
  pair inside one cook session.
- **Mesh / Texture ingest** — those are the `FbxImporter` and
  `FreeImageImporter` siblings (SPEC §4.1.2, sibling spikes #807 /
  #809; designs landed in `fbx-importer-design.md` and
  `texture-importer-design.md`). The font path is leaf-only at MVP
  (no embedded textures, no font-references-mesh edges); SPEC
  §4.1.2 inv #1 closed-sum dispatch routes its kind here, and the
  importer does not delegate cross-kind.
- **WOFF / WOFF2 / TTC / variable-font / color-font containers** —
  WOFF / WOFF2 (web-optimised wrappers around SFNT) are
  **explicitly refused** at MVP per §3.2 collapse #1; the §3.4
  vocabulary's closed-sum format set is `{Ttf, Otf}` only. WOFF /
  WOFF2 re-enter post-MVP behind the same seam by bumping
  `vocab_version` and adding format rows (FreeType supports both
  with a Brotli build dependency on WOFF2). TrueType Collection
  (TTC, multi-face containers) are also refused at MVP — the
  importer treats one `SourceAsset` as exactly one face. Variable
  fonts (OpenType `fvar`/`gvar`/`HVAR`/`MVAR` axes) are refused —
  the importer ignores axis tags and uses the default instance.
  Color fonts (`COLR`/`CPAL`, `CBDT`/`CBLC`, `sbix`, `SVG`) are
  refused — color glyphs are flattened to greyscale by FreeType's
  default rendering and the alpha is preserved; emoji and bitmap
  color tables are skipped at MVP. Each refusal is a deliberate
  central edit — adding any of these requires a `vocab_version`
  bump per §3.4 plus a row addition to §3.6.
- **Schema authoring and codegen** — `glibre.content.FontArtifact`
  the Fory schema is authored under `data/schemas/content/
  FontArtifact.fory` per `reviews/decisions/fory-codegen.md`. The
  importer is a producer of bytes conforming to that schema;
  it never defines the schema.
- **Plugin loader / hot-reload bodies** — `core::PluginLoader` and
  the manifest-pointer flip protocol (SPEC §8.2) are the loader's;
  the importer participates as a build-time-only TU under
  `tools/glibre-cook` (SPEC §6.1 lifecycle seam, SPEC §6.5
  shipping-cuts table) and ships **no runtime symbol** in
  `glibre-content.dylib`.
- **Obj-C / Obj-C++ glue** — CLAUDE.md "No Obj-C++ in engine code".
  FreeType is plain C linked from the vcpkg-pinned distribution;
  no `.mm` translation unit appears in this aggregate's source.

The aggregate's SRP boundary is sharp: if the FreeType version
moves, if any of its linked codec versions move (Brotli / zlib
when WOFF2 lands post-MVP — at MVP neither is linked), if the
normalize-params vocabulary mutates (atlas pixel size, glyph
subset, render mode selection, padding rule, bin-pack heuristic),
or if the per-glyph metric encoding changes, **this design
changes**. Anything else — manifest layout, cook-session
orchestration, residency policy, Fory schema body, GPU format
selection, runtime glyph cache — is out of scope.

## 2. Requirements coverage

Mapping of harmonius asset-import requirements
(`harmonius/docs/requirements/content-pipeline/asset-import.md`,
R-12.1.1 .. R-12.1.5) onto MVP coverage in this aggregate. Every
entry is independently re-derived; coverage sites refer to sections
of `specs/content/SPEC.md` and to the design sections below.
Harmonius surfaced **no font-specific clauses under R-12.1**; the
font path's existence at MVP is glibre's own decision (SPEC §3.2
collapse #1 last paragraph: editor + future UI plugin need at least
one font path). The requirement-coverage rows below therefore
reflect the *general* asset-import obligations as they apply to a
font source; format-specific font requirements (R-10.2.1 / R-10.4.2
HarfBuzz / MSDF in the UI domain) are explicitly refused per §1
and re-listed in §3.6.

| Harmonius clause                                                                              | Glibre disposition (MVP)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
|-----------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-12.1.1** native binary format ingestion with magic / version / hash validation            | **Refused for the font importer; collapsed.** R-12.1.1 names a glibre-native pre-cooked binary format. SPEC §3.2 collapse #1 explicitly removes that intermediate format: FreeType ingests source files (TTF / OTF) directly. The validation contract R-12.1.1 names — magic / version / hash — is preserved on the `SourceAsset → CookedAsset` seam: SPEC §4.1.1 inv #3 (`source_hash = BLAKE3(bytes)`), §4.1.4 inv #1 (`content_hash = BLAKE3(payload)`), and SPEC §4.1.2 inv #2 (FreeType reports `FT_Err_Unknown_File_Format` / `FT_Err_Invalid_Version` at first ingress; §3.7 step 1 below). |
| **R-12.1.2** texture source import (PNG / JPEG / EXR / HDR / TIFF) with sRGB / linear decode | **Out of scope (sibling).** Texture decode is the `FreeImageImporter` aggregate (sibling spike #809; design in `texture-importer-design.md`). The font path emits a CPU-side **bitmap atlas image** as part of the precursor (§3.9 below); that image is **not** routed through the texture importer — it is emitted as raw bytes that `render`'s font subsystem (post-MVP) consumes for direct upload. Embedding a texture inside the font precursor avoids the cross-importer dependency edge that would otherwise force a per-font dependency on the texture importer's vocabulary. |
| **R-12.1.3** audio source import (WAV / FLAC / Ogg Vorbis)                                    | **Refused (out of context).** SPEC §3.3 routes audio source decode to the `audio` plugin. The font importer's `SourceKind` is closed-sum on `Font`; the dispatch table (SPEC §6.1 `importers/dispatch.cpp`) compile-time-rejects any other kind.                                                                                                                                                                                                                                                                |
| **R-12.1.4** schema validation; errors include source path + byte offset + fix suggestion     | **Covered, scoped.** Path is the structured-log field `source_path` (SPEC §10.5); byte offset is recovered from FreeType's `FT_Error` taxonomy when the underlying error names a position (SFNT table-offset errors carry the offending table tag, glyph-outline errors carry the glyph index — neither is a literal byte offset, so the importer maps to `error.table_tag` / `error.glyph_index` structured fields and leaves `error.byte_offset` zero, per §10.1 fields list); fix-suggestion is the per-arm "Operator action" column from SPEC §10.1 (Class B refusal) — re-export at supported version, regenerate the broken outline, ship a single-face TTC, etc. The structured-log handler formats these uniformly (§3.7 step 9 below). |
| **R-12.1.5** parallel batch import with progress + cancellation + rollback                    | **Covered, partitioned.** Parallelism and progress tracking are the `CookSession` aggregate's concerns (SPEC §4.1.9, §6.2): the worker pool runs one `FreeTypeImporter::import_one` invocation per scheduling slot; `CookSession::commit()` returns a `CookReport { cooks_executed, cache_hits }` (SPEC §5.12). Cancellation is observed at every per-glyph load boundary in the importer (§3.7 step 5 below) per SPEC §4.1.2 inv #5; rollback semantics live with the session (SPEC §4.1.9 inv #1).            |

Glibre-native requirements added beyond harmonius:

- **Per-cook arena allocation discipline** (PHILOSOPHY §11; SPEC
  §9.3.1; `perf-budget.md` Allocator Rule #1). Every byte the
  importer declares as its own — including FreeType's own internal
  allocations (re-routed via the per-thread `FT_Memory` hook
  installed at `FT_New_Library` time, §3.5 below) — comes from
  the per-cook arena tagged `ContextTag::content`. The runtime
  heap is never touched by importer code paths; raw `new` /
  `malloc` is rejected by `-Wglibre-no-raw-alloc`. The 64 MiB
  soft sub-ceiling (SPEC §9.3.1) bounds the largest single font
  import (a 4096×4096 8-bit SDF atlas at MVP scope — ~16 MiB
  alone — fits comfortably; even 8192×8192 one-byte atlases
  with extended subsets stay below the ceiling).
- **Single `-fexceptions` carve-out, narrow boundary** (SPEC
  §10.3, `error-model.md` Decision rule 3 + Consequences last
  bullet). The carve-out is `plugins/content/cook/import/
  freetype_importer.cpp` only; the corresponding header
  `freetype_importer.hpp` and every other TU in the engine
  compile with `-fno-exceptions`. The `try / catch` block inside
  `import_one` is the **single** translation site (§3.7 step 9
  below); no exception ever crosses the importer's public
  boundary (SPEC §4.1.2 inv #2). FreeType's C ABI does not throw,
  so the carve-out exists primarily as defence-in-depth for
  `std::bad_alloc` and for any future Brotli / zlib / SVG
  internals that may surface a C++ exception once WOFF2 / SVG
  color-font support lands post-MVP.
- **Deterministic decode pipeline** (PHILOSOPHY §7,
  cross-aggregate invariant SPEC §4.2 #1). Two cooks of the same
  source bytes with the same `importer_version` and the same
  `NormalizeParams` produce **byte-equal** precursor bytes — and
  hence byte-equal cooked `FontArtifact` payloads, byte-equal
  `ContentHash`. The decode + atlas-bake pipeline is straight-line
  code with no platform intrinsics, no `std::unordered_*`, no PRNG,
  no clock reads, no thread-local caches *that survive a call*
  (the `tl_active_arena` TLS slot, §3.5, lives only for the
  duration of one `import_one` call). §3.7 step 7 below pins
  the determinism rule per-stage; §3.5 pins
  `<cfenv>` rounding-mode `FE_TONEAREST` at the `import_one`
  entry to defend SDF / float-sub-pixel-position bit patterns
  across hosts (FreeType's SDF module computes `float`-range
  distance fields whose exact bit pattern depends on rounding).
- **Importer is stateless across cooks; per-cook arena is the
  only state** (SPEC §4.1.2 identity rule). The
  `FreeTypeImporter::Impl` pimpl carries only the per-thread
  `FT_Library` + the `FT_Memory` struct + the per-thread
  cancellation-flag pointer + the alloc-exhausted atomic
  (constructed at `create()` time, owned for the importer's
  lifetime); every `import_one` call resets its scratch view of
  the arena and produces a fresh precursor. No memoization, no
  caches keyed off prior cooks.
- **Cancellation is polled, not preemptive** (SPEC §4.1.2 inv #5).
  The importer polls `CancellationToken::is_cancelled()` at every
  per-glyph load boundary inside the §3.7 stage-5 glyph loop.
  Bound: ≤ ~5 ms latency to observe cancellation on a 256-glyph
  ASCII subset (the natural granularity is one
  `FT_Load_Glyph` + `FT_Render_Glyph` pair per glyph, ~few
  hundred μs each on M1 firestorm; 256 polls fire across the
  loop).
- **Atlas size budget** (§3.4 + §9 below). The MVP atlas is
  **2048×2048 single-channel** (4 MiB) by default with a hard
  cap at **4096×4096** (16 MiB). Sources whose subset exceeds
  the cap return `ImporterError::MalformedPayload` with
  `error.detail = "freetype-glyph-atlas-overflow"` and surface
  to the operator as "split the subset or shrink `pixel_size`."
  The cap matches the engine's MVP atlas-page convention from
  the harmonius UI prior art (R-10.4.4) for cross-system
  consistency.

Coverage rule: every harmonius MVP-scope requirement above either
lands in this design (with a coverage site) or is refused with a
one-line rationale routed to the owning context. No silent drops.

## 3. Detailed model

### 3.1 Aggregate composition

```text
FreeTypeImporter  (final, SPEC §4.1.2; cook-time-only entity)
├── kind_              SourceKind::Font                  (closed-sum tag; immutable)
├── version_           ImporterVersion                   (compiled-in BLAKE3; §3.2 below)
└── impl_              Impl*                             (pimpl; arena-allocated by create())

Impl  (private; lives in freetype_importer.cpp)
├── library_           FT_Library                        (per-thread FT_Library; FT_New_Library
│                                                         + FT_Add_Default_Modules at create())
├── memory_            FT_Memory                         (the FT_Memory hook struct; alloc/realloc/free
│                                                         dispatch through tl_active_arena, §3.5)
├── memory_user_       MemoryUser                        (the FT_Memory user blob: Impl* back-ref
│                                                         so the alloc/free callbacks can find Impl)
├── alloc_exhausted_   std::atomic<bool>                 (set by FT_Memory hook on null-return;
│                                                         checked before every FreeType entry point per §6.3;
│                                                         std::atomic is a retained std:: utility per
│                                                         PHILOSOPHY §11)
└── version_string_    eastl::string_view                (e.g. "FreeType 2.13.3/normalize-v1/atlas-v1";
                                                          the source from which version_ is BLAKE3'd)
```

The `Impl` carries one `FT_Library` per worker thread; FreeType
documents `FT_Library` as **not safe for concurrent face access**
across threads, so the per-thread topology mirrors the FBX
importer's `FbxManager`-per-thread pattern (per `fbx-importer-
design.md` §3.1) rather than the FreeImage importer's process-
global init (per `texture-importer-design.md` §3.5). Unlike the FBX
SDK, FreeType has no static globals that need a process-wide
init/deinit refcount — each `FT_Library` is independent, and
`FT_New_Library` + `FT_Done_Library` are pure per-instance
operations.

Per-call scratch (lives entirely inside the supplied `ImporterArena`,
SPEC §5.4):

```text
import_one(...) scratch
├── source_buffer_     eastl::vector<std::byte, ImporterArenaAllocator>  (raw source bytes; loaded once via
│                                                                          platform::FileIo, fed to FreeType
│                                                                          via FT_New_Memory_Face)
├── face_              FT_Face                            (FreeType's per-face handle; allocated via
│                                                          FT_New_Memory_Face — bytes route through
│                                                          memory_; lifetime bounded by FT_Done_Face at stage 9)
├── glyph_slots_       eastl::vector<GlyphSlot, ImporterArenaAllocator>  (per-glyph metric record;
│                                                                          one row per loaded glyph index)
├── atlas_buffer_      eastl::vector<std::byte, ImporterArenaAllocator>  (final canonical atlas image;
│                                                                          size = atlas_w * atlas_h * 1 byte)
├── packer_state_      SkylinePacker                      (deterministic skyline / NFD bin packer;
│                                                          straight-line code, no allocation
│                                                          beyond a per-row "fences" eastl::vector)
├── kerning_pairs_     eastl::vector<KerningPair, ImporterArenaAllocator>  (legacy kern-table pairs only;
│                                                                            empty if FT_HAS_KERNING is false)
├── codepoint_map_     eastl::vector<CodepointMapEntry, ImporterArenaAllocator>  (Unicode codepoint
│                                                                                  → glyph index; sorted)
└── precursor_bytes_   eastl::span<std::byte>             (final emit; pointer into arena returned to caller)
```

The `FreeTypeImporter` aggregate is the cook-time entity; the §5.4
stub is the only public surface; everything in the boxes above is
private to the implementation file group `plugins/content/cook/import/`.
SPEC §6.1 already names the file split (`freetype_importer.{hpp,cpp}`,
`dispatch.{hpp,cpp}`, `version.{hpp,cpp}`).

`ImporterArenaAllocator` is the EASTL-conformant adaptor over
`glibre::PerContextAllocator` exposing `ContextTag::content` to
EASTL's allocator-by-value contract. The adaptor's body is a simple
forward-call shim authored in `core/include/glibre/alloc.hpp` (per
`perf-budget.md` Allocator Rules header) and shared with
`fbx-importer-design.md` §3.1 / `texture-importer-design.md` §3.1;
the font importer pulls it in via the `ImporterArena&` reference
handed to `import_one`.

### 3.2 `ImporterVersion` — the compiled-in importer identity

`ImporterVersion` (SPEC §5.4) is a 32-byte BLAKE3 over a canonical
**version string** baked into the importer's translation unit at
build time. The string is the canonical concatenation of four
components (length-prefixed, separated by `':'`):

```text
"glibre.content.freetype_importer" ":"
FREETYPE_TAG  ":"   // e.g. "freetype-2.13.3"          — the vcpkg-pinned FreeType release.
NORMALIZE_TAG ":"   // e.g. "normalize-v1"             — the §3.4 NormalizeParams vocabulary version.
ATLAS_TAG           // e.g. "atlas-v1"                 — the §3.7 atlas-bake / packer / SDF
                                                         configuration vocabulary version.
```

Concrete example:

```text
glibre.content.freetype_importer:freetype-2.13.3:normalize-v1:atlas-v1
```

The string is recovered at code-review time from the importer's
header (one constant per release). The BLAKE3 digest is computed at
**codegen time** by a small build-system step that reads the constant
and emits a `version.cpp` carrying the 32-byte hex literal; the
importer's `Importer::version()` returns the digest. Bumping the
FreeType version, the normalize-params vocabulary version, or the
atlas-bake vocabulary version changes the digest (and only those
changes do).

The digest's role:

1. **Cache-key ingredient (SPEC §4.1.3 component #2; §7.1.2).** The
   `CookKey` digest depends on `importer_version`; bumping it
   forces re-cook of every dependent font on next session per
   SPEC §7.2.2.
2. **Hot-reload coexistence (§8 below).** The build-time-only
   nature of the importer means a runtime hot-reload of
   `glibre.content` never moves the importer's code. The digest
   moves only on the release boundary that ships a new cook tool —
   at which point the §7.2.2 full-re-cook rule applies for fonts
   only (digest change is per-importer; mesh / texture cooks are
   unaffected unless their importer_version also moves).
3. **Telemetry (§10.5 below).** The digest is logged with every
   importer error so post-mortem triage can identify which
   FreeType / params vocabulary / atlas vocabulary version
   produced the failure.

Why four components and not more / fewer:

- **Why include `FREETYPE_TAG`.** FreeType's SFNT parser, glyph
  hinting (the autohinter changed materially across 2.10→2.11),
  CFF/CFF2 outline interpreter, and SDF module
  (introduced in 2.11, refined in 2.12) all change behaviour
  across releases. A patch upgrade can shift autohinted glyph
  byte patterns; a minor upgrade can change SDF distance
  computation. Bumping FreeType is a re-cook trigger.
- **Why include `NORMALIZE_TAG`.** A change to the §3.4 vocabulary
  (e.g. promoting the default atlas size from 2048 to 4096, or
  changing the codepoint subset from BMP-ASCII to BMP-Latin-Extended)
  changes every font cook output. Re-cook trigger.
- **Why include `ATLAS_TAG`.** Reserved separately from
  `NORMALIZE_TAG` because the **atlas packing algorithm** and
  the **render-mode dispatch** (bitmap vs SDF vs both, §3.7)
  evolve on a different cadence from the parameter vocabulary.
  Switching the bin packer from skyline to MaxRects, or
  switching SDF generation from FreeType's built-in `sdf` module
  to an externally-baked MSDF library, would bump `ATLAS_TAG`
  while leaving `NORMALIZE_TAG` intact.
- **Why no Brotli / zlib at MVP.** Both are linked by FreeType
  only when WOFF / WOFF2 support is configured. MVP refuses both
  formats per §1; the build's vcpkg manifest pins FreeType
  *without* those features. When WOFF2 lands post-MVP the digest
  acquires `BROTLI_TAG` and `ZLIB_TAG` components in line with
  the texture importer's `LIBPNG_TAG` / `LIBJPEG_TAG` discipline.
- **Why not include compiler / clang-version / libc++ flags.**
  Same rationale as `fbx-importer-design.md` §3.2 and
  `texture-importer-design.md` §3.2: PHILOSOPHY §7 (determinism)
  is the engine-wide commitment; the build system is responsible
  for byte-deterministic codegen across the supported toolchain
  matrix. Including the compiler hash would re-cook on every
  patch upgrade with zero semantic change — wrong trade-off;
  consistent with `fory-codegen.md` ABI-hash component list
  (schema sources only, no compiler identity).

### 3.3 `SourceAsset` reception — the dispatch contract

`FreeTypeImporter::import_one` (SPEC §5.4) receives the
`SourceAsset` by const reference. The dispatch contract is:

1. **Closed-sum dispatch.** `importers/dispatch.cpp` (SPEC §6.1)
   compile-time-routes `SourceKind::Font + FontFormat::{Ttf, Otf}`
   to `FreeTypeImporter::import_one`. Other kinds are unreachable
   here (the match is exhaustive at compile time per SPEC §4.1.2
   inv #1).
2. **Path-scope re-validation.** The cook session already validated
   the path (SPEC §4.1.1 inv #1) when constructing the
   `SourceAsset`; the importer additionally asserts that
   `SourceAsset.path` is non-empty and that
   `SourceAsset.format.font` is one of the two admitted
   `FontFormat` enumerators under `GLIBRE_DEBUG`-only `assert`.
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
   `glibre/content/freetype_normalize_params.hpp` (codegen rule
   §6 below); it does NOT re-canonicalize on the cook path.

### 3.4 `NormalizeParams` for fonts — the parameter vocabulary

`NormalizeParams.canonical_bytes` (SPEC §5.4) carries the per-cook
parameter set for the font importer. The vocabulary fields:

| Field                       | Type    | MVP default            | Effect                                                                                                                                                              |
|-----------------------------|---------|------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `vocab_version`             | `u32`   | `1`                    | The vocabulary version. Component of `importer_version`'s `NORMALIZE_TAG` per §3.2. Bump on every breaking schema move.                                              |
| `pixel_size`                | `u16`   | `48`                   | Pixel size for the bake (FreeType `FT_Set_Pixel_Sizes`). The atlas is generated at this single size. Closed sum at MVP via min/max admissibility: `[8, 256]`. Sources outside the range fail at stage 4 with `ImporterError::MalformedPayload(freetype-pixel-size-out-of-range)`. |
| `glyph_subset`              | `enum`  | `BasicLatin`           | Closed sum {`BasicLatin`, `LatinPlusLatin1`, `BmpAscii`, `Custom`}. `BasicLatin` = U+0020..U+007E (95 glyphs). `LatinPlusLatin1` = adds U+00A0..U+00FF (160 glyphs). `BmpAscii` = `BasicLatin` plus a fixed 256-codepoint set (digits punctuation common diacritics — full table in §3.6). `Custom` = consult `glyph_subset_custom_codepoints` (next field). |
| `glyph_subset_custom_codepoints` | `bytes` | `{}`                  | When `glyph_subset == Custom`, this is the canonical (length-prefixed, sorted-by-codepoint) byte serialization of the set of `u32` Unicode codepoints to bake. Refusal: any codepoint above `U+FFFF` fails at stage 4 with `ImporterError::MalformedPayload(freetype-codepoint-out-of-bmp)`. Set must be non-empty when `Custom`. |
| `render_mode`               | `enum`  | `Sdf`                  | Closed sum {`Bitmap`, `Sdf`}. `Bitmap` uses FreeType's grayscale rasterizer (`FT_RENDER_MODE_NORMAL`, 8-bit alpha-coverage glyphs). `Sdf` uses FreeType's `FT_RENDER_MODE_SDF` (built-in `sdf` module, available since FreeType 2.11; produces 8-bit signed distance fields, used by render's MSDF-equivalent quad shader). MSDF specifically is **not** in scope at MVP — `Sdf` covers the `R-10.4.2` adjacent harmonius use-case at the closest available FreeType-only render mode. |
| `sdf_spread`                | `u8`    | `8`                    | Pixel spread (`FT_PROPERTY_SET("sdf", "spread", spread)`) for `render_mode = Sdf`. Range `[2, 32]`; outside range fails at stage 4. Ignored when `render_mode = Bitmap`. |
| `atlas_max_dim`             | `u16`   | `2048`                 | Maximum atlas dimension (square). Allowed values: `{1024, 2048, 4096}`. Subsets that do not pack within `atlas_max_dim × atlas_max_dim` at the configured `pixel_size + sdf_spread + atlas_padding` produce `ImporterError::MalformedPayload(freetype-glyph-atlas-overflow)` at stage 7. |
| `atlas_padding`             | `u8`    | `2`                    | Pixels of padding around each glyph in the atlas (separates glyphs to avoid sampler bleed at runtime). Range `[0, 16]`. The packer adds `2 * atlas_padding` to each glyph's box before placement. |
| `hinting`                   | `enum`  | `None`                 | Closed sum {`None`, `Light`, `Normal`}. `None` = `FT_LOAD_NO_HINTING` (preserves outline at any sub-pixel resolution; correct for SDF baking and for upscaled bitmap text — autohinter outputs are otherwise non-deterministic across linker patches). `Light` / `Normal` route through FreeType's hinting pipeline; included for operators who specifically need bitmap-aligned glyphs. **MVP default is `None`** because SDF is the default render mode and hinting is meaningless for SDF (the outline is what gets distance-fielded, regardless of hinted bitmap shape). |
| `include_kerning`           | `bool`  | `true`                 | Whether to extract the legacy `kern` table pairs from `FT_HAS_KERNING`. Modern OpenType fonts use GPOS instead (which is HarfBuzz domain — not surfaced here). Set to `false` for fonts where `kern` is known absent / wrong. |
| `pack_strategy`             | `enum`  | `Skyline`              | Closed sum {`Skyline`, `NextFitDecreasing`}. `Skyline` is the deterministic skyline packer described in §3.7 stage 7. `NextFitDecreasing` sorts glyphs by area descending and walks rows; smaller atlas waste on heterogeneous subsets but slightly more wall-clock. MVP = `Skyline`. |

The canonical-bytes encoding is **sorted-key, length-prefixed**:
each field name is encoded as a `u8` length followed by ASCII bytes,
followed by a `u8` typetag, followed by the canonical bytes of the
value. Boolean → `u8`; enums → `u8`; `u16`/`u32` → little-endian.
For the `bytes`-typed `glyph_subset_custom_codepoints` field, the
encoding is `u32-le` count followed by `count × u32-le` codepoint
values **sorted ascending** (sort defends `CookKey` determinism per
SPEC §4.1.3 inv #2). The field set is sorted lexicographically by
name before encoding so two serializations of the same logical
record produce byte-equal canonical bytes (SPEC §4.1.3 inv #2). The
encoding is generated by a small codegen rule in
`tools/glibre-cook/CMakeLists.txt` from a manifest
`plugins/content/cook/import/freetype_normalize_params.fory` (the
`data` plugin's Fory grammar; the codegen tool already knows how to
produce canonical-byte serializers per `fory-codegen.md`
§"Migration Mechanic"). The importer typed-decodes via the
generated header.

Why these fields and not others:

- **Why a single `pixel_size` rather than a list.** MVP commits to
  one atlas at one size per font cook; a multi-size cook would
  require either multiple atlases per font (the precursor's
  `pixels` field becoming a `list<atlas>` with a parallel
  `pixel_sizes` list, doubling the schema) or one atlas containing
  multiple sizes (which negates the §1 `pixel_size` invariant and
  forces the packer to handle non-uniform glyph dimensions). Both
  options re-enter post-MVP per §12 OQ-2.
- **Why explicit `glyph_subset` enum rather than always `Custom`.**
  The three named subsets cover the vast majority of editor /
  text-prompt use cases at MVP and serialize compactly. `Custom`
  covers the rest. Hard-coding the subsets removes per-cook
  variance for the common path and makes the `BasicLatin` /
  `LatinPlusLatin1` / `BmpAscii` cohorts share a `CookKey`
  ingredient (deduplicating cache hits across many fonts that
  bake the same subset).
- **Why `Sdf` as default `render_mode`.** SDF text upscales
  cleanly to arbitrary resolutions without re-rasterization, which
  is the harmonius R-10.4.2 use-case (5000+ visible glyphs, sharp
  at 100%-300% scale) most easily met from a single cooked atlas.
  Bitmap-mode is included for the corner case where authoring tools
  expect pixel-aligned glyph rasters (e.g. a UI mockup baked at
  the exact display DPI).
- **Why no MSDF.** MSDF (multi-channel SDF) is not in FreeType's
  built-in module set. Adding MSDF would require linking either
  the `msdfgen` library or a custom RGB-channel SDF emitter; both
  re-enter post-MVP per §12 OQ-3 if a render-side requirement
  drives it.
- **Why `atlas_max_dim` capped at 4096.** Texture-axis sampling
  precision on M-series GPUs degrades visibly above 4096 (the
  hardware sampler's address mantissa narrows past that point);
  also matches the engine-wide UI atlas page convention from
  harmonius R-10.4.4. Larger atlases re-enter post-MVP through
  multi-page atlas support, not by widening this single field.
- **Why include `kerning` at MVP.** The legacy `kern` table is the
  one OpenType structure FreeType exposes through `FT_Get_Kerning`
  without HarfBuzz. Including it covers simple LTR Latin text
  rendering (the editor's primary path) without pulling HarfBuzz
  into content. Modern fonts that use GPOS will report empty kern
  pairs, which is correct — the future UI plugin handles GPOS via
  HarfBuzz at runtime.
- **Why `Skyline` default `pack_strategy`.** Skyline is the
  deterministic baseline; produces byte-equal output for the same
  input regardless of host (PHILOSOPHY §7). NFD is included as a
  closed-sum alternative for cases where the resulting atlas
  utilization is materially worse with skyline (rare for
  homogeneous-pixel-size subsets).

### 3.5 SDK seam — `FT_Library`, `FT_Memory`, `FT_Face`, the `FT_New_Memory_Face` ingest

FreeType's class topology and lifetime contracts dictate the
importer's structure. **Notable convergence with
`fbx-importer-design.md` §3.5**: FreeType *does* expose a custom
allocator hook (the `FT_Memory` struct passed at `FT_New_Library`
time), so unlike the texture importer (which had no equivalent for
FreeImage), the font importer routes every FreeType-internal
allocation through the per-cook arena. **Notable divergence from
the FBX importer**: the hook is **per-`FT_Library`** (so per-thread)
rather than process-global, because each worker thread owns its
own `FT_Library`.

```text
FT_New_Library / FT_Done_Library     (per-FT_Library; called once per worker thread
                                      from FreeTypeImporter::create() / ~FreeTypeImporter().)

FT_Library     (per-thread root; carries the FT_Memory hook + the registered modules.
                FreeType documents per-FT_Library use across threads as unsafe; per-thread
                ownership matches Autodesk-style per-FbxManager isolation. Live for the
                FreeTypeImporter aggregate's entire lifetime.)
└── FT_Face   (per-cook; allocated per import_one call via FT_New_Memory_Face;
               populated by FreeType's SFNT parser; destroyed at end of import_one
               via FT_Done_Face. Bytes route through the FT_Memory hook into the
               per-cook arena.)

FT_Memory  (the memory hook struct: alloc/realloc/free callbacks + a `user` pointer
            that holds an Impl* so the callbacks can find tl_active_arena.)
```

**Per-thread `FT_Library` allocation.** The `create()` static
(SPEC §5.4) builds an `FT_Memory` struct whose callbacks dispatch
through the `tl_active_arena` TLS slot below, then constructs the
library via `FT_New_Library(memory_, &library_)` followed by
`FT_Add_Default_Modules(library_)` (loads the SFNT, TrueType, CFF,
auto-hinter, smooth-rasterizer, and SDF modules; per the FreeType
docs the modules-set is what gives `FT_RENDER_MODE_SDF` its body —
without `FT_Add_Default_Modules` the SDF module is absent). Per
`FT_PROPERTY_SET("sdf", "spread", N)`, the SDF spread is set from
`NormalizeParams.sdf_spread` at the same step. The destructor
(`~FreeTypeImporter`) calls `FT_Done_Library(library_)` to release
every library-owned resource.

**`FT_Memory` allocator hook.** FreeType allocates everything
through the hook installed at library construction. The hook's
callback signatures are:

```c
typedef void* (*FT_Alloc_Func)(FT_Memory memory, long size);
typedef void  (*FT_Free_Func) (FT_Memory memory, void* block);
typedef void* (*FT_Realloc_Func)(FT_Memory memory, long cur_size,
                                 long new_size, void* block);
```

The `FT_Memory` struct's `user` field is the `Impl*` back-reference
the callbacks use to find the importer state (specifically the
`alloc_exhausted_` flag). The callbacks themselves dispatch through
the same `tl_active_arena` / `tl_active_impl` TLS-pointer pattern
the FBX importer uses (per `fbx-importer-design.md` §3.5):

```cpp
// freetype_importer.cpp (translation-unit-local; no external linkage)
thread_local ImporterArena*       tl_active_arena = nullptr;
thread_local FreeTypeImporterImpl* tl_active_impl  = nullptr;

void* ft_alloc(FT_Memory /*mem*/, long size) {
    if (tl_active_arena == nullptr) {
        // Spurious call outside an active import_one; FreeType never calls
        // alloc when no FT_Library operation is in flight, but defensively
        // return null so a defect surfaces immediately rather than silently
        // routing to a global heap.
        return nullptr;
    }
    auto bytes = tl_active_arena->allocate(static_cast<std::size_t>(size),
                                           alignof(std::max_align_t));
    if (bytes == nullptr) {
        if (tl_active_impl) {
            tl_active_impl->alloc_exhausted_.store(true,
                                                   std::memory_order_release);
        }
        return nullptr;
    }
    return bytes;
}

void ft_free(FT_Memory /*mem*/, void* block) {
    // The arena is bump-allocated and drains at session end (SPEC §9.3.1
    // rule 3); per-call `free` is a no-op. This is the standard pattern
    // for arena-backed allocators across the codebase.
    (void)block;
}

void* ft_realloc(FT_Memory mem, long cur_size, long new_size, void* block) {
    // Standard arena-realloc: if the block is the most-recent allocation,
    // grow in place; otherwise allocate fresh + memcpy old bytes (note
    // FT_Realloc_Func contract: "in case of error the old block must
    // still be available", which our null-on-failure path honours).
    auto* fresh = static_cast<std::byte*>(ft_alloc(mem, new_size));
    if (fresh == nullptr) {
        return nullptr;  // FreeType's caller is responsible for keeping
                         // the old block; arena's bump pointer is unchanged.
    }
    if (block != nullptr && cur_size > 0) {
        eastl::memcpy(fresh, block,
                      static_cast<std::size_t>(eastl::min(cur_size, new_size)));
    }
    // Old block stays where it is in the arena — wasted bytes accepted as
    // arena-realloc fragmentation; bounded by the §9.3.1 64 MiB ceiling.
    return fresh;
}
```

Before the first FreeType call in `import_one` (stage 1, source
buffer load), the worker sets:

```cpp
tl_active_arena = &arena;
tl_active_impl  = impl_;
```

The hook's allocator dispatches to the per-call arena;
`alloc_exhausted_` is written by the hook's null-return path and
read by the same worker thread's `import_one`; no cross-thread
access to the flag occurs. Stage 9 teardown (always, via the
`GLIBRE_DEFER` guard) clears both pointers back to `nullptr`. A
null `tl_active_arena` in the callback indicates a spurious
FreeType call outside of an active cook — the callback returns
`nullptr` immediately (treated as a defect path; FreeType has no
documented external-thread-driven internal calls, so this should
be unreachable in practice, but the defensive nullptr-return
matches the FBX importer's pattern).

This design is safe under the four-worker parallel topology
because each worker thread has its own TLS slot and its own
`FT_Library`; no synchronization between workers is needed.

**Why per-thread `FT_Library`.** FreeType's documentation does not
explicitly call out thread safety, but the source code uses
internal mutexes only for the (optional) thread-safe-cache module
which we do not enable; per-`FT_Library` access from multiple
threads to the same library is **not safe** for face creation +
glyph rendering (the SFNT module's per-face state would race).
One library per worker thread is the supported topology — same
shape as the FBX importer's `FbxManager`-per-thread (per
`fbx-importer-design.md` §3.5). The cook worker pool sized from
`perf-budget.md`'s content cell (§9.3.1; ~4 workers) holds 4
`FreeTypeImporter` instances, each owning its own `FT_Library`.
Memory cost: ~few-hundred KiB per library × 4 ≈ ~1 MiB peak from
the content tag's 256 MiB ceiling — small.

**Source bytes via `FT_New_Memory_Face`.** The importer never
calls `FT_New_Face(path, ...)`. Instead the source bytes are read
once at stage 1 via `platform::FileIo` into the arena's
`source_buffer_`, and the importer hands FreeType the buffer
through `FT_New_Memory_Face(library_, source_buffer_.data(),
source_buffer_.size(), 0 /*face_index*/, &face_)`. This is the
same source-byte-routing pattern as the texture importer's
`FreeImageIO` shim (per `texture-importer-design.md` §3.5), but
FreeType's API takes a flat byte view directly so no `FreeImageIO`-
equivalent shim is needed.

The `face_index = 0` argument selects the first face in the file.
TTC (TrueType Collection) files contain multiple faces; per §1
this design refuses TTC at MVP: stage 4 below probes
`FT_FACE_FLAG_VARIATION` and `face_->num_faces > 1` and emits
`ImporterError::UnsupportedVersion` with `error.detail =
"freetype-ttc-multiface"` if either is set.

**Render-mode dispatch table — the §3.4 `render_mode` rule.** The
deterministic mapping from `(render_mode, glyph_subset, hinting)`
to per-glyph `FT_Load_Glyph` flags + `FT_Render_Mode` selection:

| `render_mode` | `hinting` | `FT_Load_*` flags                                          | `FT_Render_Mode`         | Output buffer format             |
|---------------|-----------|-------------------------------------------------------------|--------------------------|----------------------------------|
| `Bitmap`      | `None`    | `FT_LOAD_NO_HINTING \| FT_LOAD_NO_AUTOHINT \| FT_LOAD_RENDER` | `FT_RENDER_MODE_NORMAL`  | `FT_PIXEL_MODE_GRAY` (8-bit)     |
| `Bitmap`      | `Light`   | `FT_LOAD_TARGET_LIGHT \| FT_LOAD_RENDER`                    | `FT_RENDER_MODE_NORMAL`  | `FT_PIXEL_MODE_GRAY` (8-bit)     |
| `Bitmap`      | `Normal`  | `FT_LOAD_TARGET_NORMAL \| FT_LOAD_RENDER`                   | `FT_RENDER_MODE_NORMAL`  | `FT_PIXEL_MODE_GRAY` (8-bit)     |
| `Sdf`         | `None`    | `FT_LOAD_NO_HINTING \| FT_LOAD_NO_AUTOHINT`                 | `FT_RENDER_MODE_SDF`     | `FT_PIXEL_MODE_GRAY` (8-bit SDF) |
| `Sdf`         | `Light`   | `FT_LOAD_TARGET_LIGHT`                                       | `FT_RENDER_MODE_SDF`     | `FT_PIXEL_MODE_GRAY` (8-bit SDF) |
| `Sdf`         | `Normal`  | `FT_LOAD_TARGET_NORMAL`                                      | `FT_RENDER_MODE_SDF`     | `FT_PIXEL_MODE_GRAY` (8-bit SDF) |

FreeType's SDF module emits 8-bit pixels where pixel value `128`
denotes the contour, `<128` is outside, `>128` is inside, and the
contour-relative distance scales linearly with the configured
`spread` (`FT_PROPERTY_SET("sdf", "spread", spread)`, set at
`create()` time from `NormalizeParams.sdf_spread`). The atlas
buffer stores these bytes verbatim; downstream `render` consumes
the 8-bit format directly with a sampler-derived screen-space
distance threshold.

The MVP default `(render_mode = Sdf, hinting = None)` matches
the bottom-left row: outline-preserving SDF that upscales cleanly
without rasterizer-aligned bitmap drift. PHILOSOPHY §7 determinism
holds because SDF distance computation is documented as
implementation-deterministic per the FreeType release pinned by
`importer_version` (the `<cfenv>` `FE_TONEAREST` rounding mode is
set at `import_one` entry to defend against host-specific FPU
rounding; same as the texture importer's HDR / EXR path per
`texture-importer-design.md` §3.5).

### 3.6 Sub-asset / format-feature boundaries — what the font path includes / refuses

A font source file can carry many ancillary features in one
container: multiple faces (TTC), variable-font axes, color glyph
tables, embedded bitmap strikes, complex shaping tables, hinting
programs, embedded SVG outlines, embedded ICC profiles, metadata
strings (postscript names, copyright, version). The aggregate's
MVP decision per feature:

| Feature                                                | MVP disposition                                                                                                                                                                                                                                              |
|--------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Single `FT_Face` outlines (`glyf` / `CFF` / `CFF2`)    | **Imported.** The primary glyph outlines are loaded per glyph via `FT_Load_Glyph` and rasterized to the §3.5 render-mode dispatch table.                                                                                                                       |
| Per-glyph metrics (advance, bearing, bbox)             | **Imported.** Extracted via `FT_Glyph_Metrics` after each `FT_Load_Glyph`; 26.6 fixed-point converted to floating-point pixels at canonicalization (§3.7 stage 7).                                                                                              |
| Legacy `kern` table                                    | **Imported when present** if `NormalizeParams.include_kerning = true` and `FT_HAS_KERNING(face_) != 0`. Pairs extracted via `FT_Get_Kerning` for every (lhs, rhs) cross-product within the loaded glyph subset; sorted by `(lhs, rhs)` for determinism.       |
| OpenType GPOS / GSUB tables                            | **Refused.** Modern positional / substitution shaping is HarfBuzz domain (future UI plugin / runtime). The cook artifact carries no GPOS-derived data; consumers that need GPOS link HarfBuzz separately at runtime.                                          |
| Variable font axes (`fvar` / `gvar` / `HVAR` / `MVAR`) | **Refused.** The default variation instance is used implicitly by FreeType when no `FT_Set_Var_Design_Coordinates` is called; the importer never calls that API. Files with `FT_FACE_FLAG_VARIATION` set return `ImporterError::UnsupportedVersion(freetype-variable-font)`. |
| TrueType Collection (TTC) — multiple faces             | **Refused.** Files with `face_->num_faces > 1` return `ImporterError::UnsupportedVersion(freetype-ttc-multiface)`. Operator action: re-export single-face TTF/OTF, or split the TTC.                                                                            |
| Color tables (`COLR` / `CPAL`, `CBDT`/`CBLC`, `sbix`, `SVG`) | **Refused.** Color glyph rendering is post-MVP; the importer skips color tables entirely. FreeType's default `FT_Render_Glyph` flattens to greyscale, which is sufficient for the MVP black-box-text editor use case.                                  |
| Embedded bitmap strikes (`EBDT` / `EBLC`, `bdat` / `bloc`) | **Refused.** Same path as color tables — bitmap strikes are not loaded; the outline path is used at the configured `pixel_size`.                                                                                                                            |
| Hinting program (TrueType bytecode interpreter)        | **Conditionally honored.** When `hinting = Light` / `Normal`, FreeType's TT bytecode interpreter runs (assuming the engine's FreeType build was configured with TT_CONFIG_OPTION_BYTECODE_INTERPRETER, which is the default). When `hinting = None` (MVP default), the interpreter is bypassed via `FT_LOAD_NO_HINTING`. |
| Embedded SVG glyphs (OpenType `SVG` table)             | **Refused.** SVG glyph rendering requires linking an SVG renderer (FreeType's `ot-svg` hook); MVP does not configure one. Codepoints whose primary glyph is SVG-only would render as missing-glyph; the importer maps them to glyph index 0 (`.notdef`) per FreeType's behaviour. |
| WOFF / WOFF2 wrapping                                  | **Refused.** §3.4 `FontFormat` closed-sum is `{Ttf, Otf}` only. `.woff` / `.woff2` extensions never reach the importer (rejected at `SourceAsset::from_path`); the dispatch is exhaustive on `FontFormat`.                                                  |
| Embedded ICC profiles                                  | **Refused.** Color management is not relevant to a greyscale-glyph atlas; ICC tags are ignored at decode time.                                                                                                                                                |
| Metadata strings (Postscript name, copyright, license) | **Imported partially.** Only the Postscript name (`FT_Get_Postscript_Name`) is recorded in the precursor's `metadata.postscript_name` field, for telemetry / debugging. Copyright / license / vendor strings are deliberately **not** persisted — those are governance concerns for a future asset-license enforcement spike, not a content-cook concern. |
| BMP-only codepoint range (`U+0000..U+FFFF`)            | **Imported.** All MVP `glyph_subset` values stay within the BMP; SMP-and-above codepoints (emoji range, CJK-extended ranges) are out of scope. `Custom` subsets containing codepoints `> U+FFFF` fail at stage 4 with `ImporterError::MalformedPayload(freetype-codepoint-out-of-bmp)`. |
| Glyph index 0 (`.notdef`)                              | **Imported.** The `.notdef` glyph (always glyph 0, the missing-glyph rectangle) is always included in the cooked subset regardless of which Unicode codepoints are loaded; consumers use it as the fallback for unmapped codepoints at runtime.                |

**MVP `BmpAscii` codepoint set** (the §3.4 `glyph_subset = BmpAscii`
covers): `BasicLatin` (U+0020..U+007E, 95 chars) + `LatinPlusLatin1`
extras (U+00A0..U+00FF, 96 chars) + a curated set of common
diacritic / quote / punctuation codepoints (U+2010 hyphen, U+2013
en-dash, U+2014 em-dash, U+2018..U+201D smart quotes, U+2026
ellipsis, U+20AC euro, U+00A9 copyright, U+00AE registered, U+2122
trademark) — total ~205 glyphs once the source maps codepoints to
glyph indices. The exact codepoint list is generated alongside the
`importer_version` digest at codegen time so it is machine-checkable
against the §3.4 vocabulary (§3.2 — bumping the codepoint set bumps
`NORMALIZE_TAG`).

The disposition is closed: any feature class not listed above is
ignored with a once-per-cook `debug`-level log entry. Adding a
feature to the imported set requires bumping `vocab_version` per
§3.4 and authoring the precursor-field shape — a deliberate
central edit, not a runtime branch (PHILOSOPHY §6, §10).

### 3.7 Import pipeline — the nine ordered stages inside `import_one`

`FreeTypeImporter::import_one(SourceAsset, NormalizeParams,
ImporterArena&, CancellationToken&)` runs **nine** ordered stages,
each documented inline against a SPEC invariant. Stages 1–8 run
inside the `-fexceptions` carve-out's `try` block (one block,
wrapping the entire body); stage 9 runs after the `catch` epilogue
and is the FreeType teardown phase. The `try` / `catch` pair is
the unique exception translation site (SPEC §10.3).

**Stage 1 — Source buffer load + format probe**
(SPEC §4.1.2 inv #2). Set the `tl_active_arena` / `tl_active_impl`
TLS slots (§3.5). Set `<cfenv>` rounding mode to `FE_TONEAREST`
via `std::fesetround(FE_TONEAREST)` (deferred-restore via a
`GLIBRE_DEFER` guard). Read the source bytes via
`platform::FileIo::read_into(arena, src.path)` into
`source_buffer_` (the arena-tagged byte buffer). Dispatch failure
modes:

- `platform::FileIo::read_into` returns `platform::Error::IoFailure`
  → translated to `ImporterError::SourceNotFound` (file missing,
  watcher race) or `ImporterError::MalformedPayload` (with
  `error.detail = "platform-io"`) per the `error.detail` discriminator
  on `platform::Error::IoFailure`.

The byte-offset for R-12.1.4 errors is zero at this stage (the
file has not been parsed yet); offset becomes meaningful at
stage 2 onward (FreeType's error taxonomy carries no literal byte
offsets, but it does carry table tags / glyph indices that the
structured-log handler emits in lieu of `byte_offset`).

**Stage 2 — `FT_New_Memory_Face` decode**
(FreeType side-effect; FT_Face allocations route through the
`FT_Memory` hook into the arena per §3.5). Invoke
`FT_New_Memory_Face(library_, source_buffer_.data(),
source_buffer_.size(), /*face_index=*/0, &face_)`. The
`face_index = 0` is fixed; multi-face TTC is refused at stage 4.
Failure modes (every `FT_Error` returned non-zero is classified
via the §10.2 translation table):

- `FT_Err_Unknown_File_Format` → `ImporterError::MagicMismatch`,
  `error.detail = "freetype-magic"`.
- `FT_Err_Invalid_Version` (file version unsupported by the
  linked SFNT/CFF parser) →
  `ImporterError::UnsupportedVersion`, `error.detail =
  "freetype-version"`.
- `FT_Err_Invalid_File_Format` (truncated SFNT directory,
  missing required tables) → `ImporterError::MalformedPayload`,
  `error.detail = "freetype-corruption"`.
- `FT_Err_Invalid_Table` / `FT_Err_Table_Missing` →
  `ImporterError::MalformedPayload`, `error.detail =
  "freetype-corruption"`, `error.table_tag` field set from
  the FT_Error context when available (FreeType's
  `FT_Error_String` provides the human-readable form;
  programmatic table-tag recovery is best-effort via the
  high-level FT_Error code).
- Any unclassified non-zero `FT_Error` →
  `ImporterError::MalformedPayload`, `error.detail =
  "freetype-unclassified"`.

The `tl_active_impl->alloc_exhausted_` flag is checked
immediately after the call; if set, returns
`ImporterError::MalformedPayload(freetype-alloc-exhausted)`
(see §6.3). `face_` is captured in a `GLIBRE_DEFER` guard for
stage-9 cleanup.

**Stage 3 — Set pixel size**
(FreeType side-effect; pure of effect outside `face_`). Invoke
`FT_Set_Pixel_Sizes(face_, /*pixel_width=*/params.pixel_size,
/*pixel_height=*/params.pixel_size)`. Square pixels at MVP; the
non-square case (mixed DPI, anamorphic baking) is a post-MVP
vocabulary addition. Failure: `FT_Err_Invalid_Pixel_Size` (rare
— file admits no scalable size at the requested resolution; some
bitmap-only fonts may surface this) →
`ImporterError::MalformedPayload(freetype-invalid-pixel-size)`.
For SDF mode, also call `FT_Property_Set(library_, "sdf",
"spread", &spread)` where `spread` is `params.sdf_spread`
(a `unsigned int` — FreeType's property is a uint).

**Stage 4 — Refuse-if-unsupported probe** (the §3.6 refusal
gate). Read the face flags and reject:

- `face_->num_faces > 1` → `ImporterError::UnsupportedVersion(
  freetype-ttc-multiface)`.
- `(face_->face_flags & FT_FACE_FLAG_VARIATION) != 0` →
  `ImporterError::UnsupportedVersion(freetype-variable-font)`.
- For `Custom` subset: every codepoint in
  `params.glyph_subset_custom_codepoints` must be `<= 0xFFFF`;
  a codepoint above the BMP →
  `ImporterError::MalformedPayload(freetype-codepoint-out-of-bmp)`.

**Stage 5 — Glyph loading + cancellation poll + per-glyph metric
extraction** (SPEC §4.1.2 inv #5; §4.1.2 inv #3 — pure outside
arena). Resolve the codepoint subset to a glyph-index list:

1. Iterate the §3.4 codepoint subset (`BasicLatin` /
   `LatinPlusLatin1` / `BmpAscii` / `Custom`).
2. For each codepoint `cp`: `glyph_idx = FT_Get_Char_Index(face_,
   cp)`. `glyph_idx == 0` (no glyph for this codepoint) is
   recorded but not an error — the `.notdef` fallback at runtime
   handles missing glyphs. Build the `codepoint_map_` entry
   `(cp, glyph_idx)`.
3. Collect the unique glyph indices into a sorted set (so two
   codepoints mapping to the same glyph produce one row in the
   atlas); always include `0` (`.notdef`).
4. For each unique glyph index `g` (in ascending order):
   - Poll `cancel.is_cancelled()` — observed cancellation
     returns `ImporterError::Cancelled(freetype-cancelled)`
     immediately. Polling cost: one atomic load per glyph,
     bounded by the subset size (~200 glyphs at MVP); ~200 ns.
   - `FT_Load_Glyph(face_, g, load_flags)` per the §3.5 table
     (load_flags depends on `render_mode` × `hinting`). Failure
     modes: `FT_Err_Invalid_Outline` →
     `ImporterError::MalformedPayload(freetype-corruption)`;
     `FT_Err_Out_Of_Memory` (only fires when the FT_Memory
     hook returned null) → `MalformedPayload(freetype-alloc-
     exhausted)`.
   - `FT_Render_Glyph(face_->glyph, FT_RENDER_MODE_NORMAL or
     FT_RENDER_MODE_SDF)` per the §3.5 table. Same failure
     classification.
   - Extract `face_->glyph->metrics` (advance, bearing,
     bounding box — all in 26.6 fixed-point) and push a
     `GlyphSlot` row into `glyph_slots_`:
     ```text
     GlyphSlot {
       glyph_idx        : u32
       advance_x_26_6   : i32   // FT 26.6 fixed-point.
       bearing_x_26_6   : i32
       bearing_y_26_6   : i32
       bbox_w_pixels    : u16   // post-rendering bitmap width.
       bbox_h_pixels    : u16
       bitmap_data_ptr  : const FT_Byte*  // points into face_->glyph->bitmap.buffer
                                          // — valid until next FT_Load_Glyph; copied to
                                          // atlas_buffer_ during stage 7 immediately.
       bitmap_pitch     : i32
     }
     ```
     The `bitmap_data_ptr` is FreeType-owned and lives until the
     next `FT_Load_Glyph` overwrites it; the §3.7 stage-7 packer
     consumes it immediately during the same loop iteration so
     the pointer is always live at copy time.

Per-glyph budget on M1 firestorm: ~50–500 μs per glyph for SDF
mode (dominated by the SDF distance-field computation; spread=8
on a 48-px bake takes ~150 μs typical); ~10–50 μs for bitmap
mode. A 256-glyph subset SDF bake totals ~25–125 ms wall-clock,
comfortably off the driver thread per §9.

**Stage 6 — Optional kerning extraction** (skipped if
`include_kerning = false` or `FT_HAS_KERNING(face_) == 0`). For
every ordered glyph-index pair `(lhs, rhs)` in the loaded
subset's cross-product:

```cpp
FT_Vector v;
if (FT_Get_Kerning(face_, lhs, rhs, FT_KERNING_DEFAULT, &v) == 0
    && (v.x != 0 || v.y != 0)) {
    kerning_pairs_.push_back({lhs, rhs, v.x, v.y});
}
```

The pairs are sorted by `(lhs, rhs)` ascending at stage 8 for
deterministic encoding. Cross-product cost: O(N²) where N is
the loaded subset size — at MVP ~200 glyphs ⇒ ~40k probes; each
probe is a hash-table lookup inside FreeType's kerning cache,
~few hundred ns per probe; total ~10 ms wall-clock. Most
modern fonts have empty `kern` tables (GPOS replaces kerning),
so `kerning_pairs_` typically has zero rows.

**Stage 7 — Atlas pack + bitmap copy-out + canonicalization**
(SPEC §4.2 inv #1; PHILOSOPHY §7). The dominant stage. The
deterministic skyline packer:

1. Sort `glyph_slots_` ascending by glyph index (already sorted
   by stage 5's loop order; this is a defensive re-sort).
2. Compute padded glyph dimensions: `padded_w = bbox_w + 2 *
   atlas_padding`, `padded_h = bbox_h + 2 * atlas_padding`.
3. Initialize a skyline of one segment at height 0 spanning
   the full atlas width (`atlas_max_dim` from §3.4).
4. For each glyph in iteration order (the `pack_strategy`
   modulates this — `Skyline` keeps the glyph-index order; NFD
   sorts by `padded_h * padded_w` descending):
   - Walk the skyline left-to-right. Find the leftmost column
     `x` where placing the glyph (width `padded_w`) on top of
     the current skyline yields the smallest resulting top
     height `y_new`.
   - If `y_new + padded_h > atlas_max_dim`: refusal.
     Atlas overflow → `ImporterError::MalformedPayload(
     freetype-glyph-atlas-overflow)`.
   - Place the glyph at `(x + atlas_padding, y_new + atlas_padding)`;
     update the skyline to reflect the new top.
5. Compute the actual atlas height as `max(y_new + padded_h)`
   over all glyphs; round up to the next power of two
   (`atlas_h = next_pow2(actual_h)`); set `atlas_w =
   atlas_max_dim`.
6. Allocate `atlas_buffer_` in the arena sized to `atlas_w *
   atlas_h * 1` (single-byte 8-bit grayscale; one byte
   regardless of `Bitmap` or `Sdf` mode). Zero-initialize the
   buffer.
7. For each placed glyph: `memcpy` the FreeType-owned bitmap
   bytes from `face_->glyph->bitmap.buffer` (after re-loading
   the glyph at stage 5's pointer; the loop is structured so
   the bitmap is copied during the same iteration as
   `FT_Load_Glyph` to avoid re-loading) row-by-row at row stride
   `bitmap_pitch` into the atlas at the placed position. The
   copy is `bbox_h` rows × `bbox_w` bytes per row; row stride
   in the atlas is `atlas_w`.
8. Update each glyph's `GlyphSlot` with the placed `(u, v)` and
   `(u_size, v_size)` in atlas pixels for stage 8's metric
   emit.

The skyline packer is straight-line code with O(N × W) worst-case
where N is the glyph count and W is the skyline segment count
(bounded by N). Total cost on the worker thread: ≤ ~1 ms for
~200 glyphs at MVP. The `atlas_buffer_` allocation is the
arena's largest single allocation (16 MiB at the 4096×4096 cap;
typically 2–4 MiB at 2048×2048 with the BasicLatin subset);
in `GLIBRE_ALLOC_STRICT=1` mode it is the gate for the §9.3.1
64 MiB soft sub-ceiling.

Byte-determinism: the packer is deterministic given identical
input order (glyph index ascending) and identical glyph
dimensions; identical source bytes + identical
`NormalizeParams` produce byte-equal `atlas_buffer_` contents
(PHILOSOPHY §7).

**Stage 8 — Determinism canonicalization**
(SPEC §4.2 inv #1). After the bake:

- Sort `kerning_pairs_` ascending by `(lhs, rhs)`. (Already
  sorted by stage 6 loop order; defensive re-sort for the rare
  case where stage 6 visits in a non-sorted order.)
- Sort `codepoint_map_` ascending by codepoint. (Same defensive
  re-sort.)
- Convert per-glyph 26.6 fixed-point metrics to canonical
  `f32` pixel values (`x_pixels = x_26_6 / 64.0f`); store
  with `FE_TONEAREST` rounding (set at stage 1 entry).
- Reject any `f32` metric that is `NaN` or `signaling NaN`
  (matches the texture importer's `ban_nan` policy per
  `texture-importer-design.md` §3.4 / §3.7 stage 5; matches
  `fory-codegen.md` Open Question 5). NaN in a glyph metric
  indicates a corrupt source; emit
  `ImporterError::MalformedPayload(freetype-nan-metric)` with
  `error.glyph_index` set to the offending glyph.

**Stage 9 — Precursor emit** (§3.9 below). Write the precursor
bytes into the arena via a generated emitter
(`glibre/types/content/font_artifact_precursor.hpp`, the Fory
codegen output's pre-Fory CPU-side struct — generated header
path per `fory-codegen.md` §Pipeline into
`${CMAKE_BINARY_DIR}/generated/glibre-types/include/glibre/types/<ctx>/`).
The emitter is a straight-line walk over `glyph_slots_`,
`atlas_buffer_`, `kerning_pairs_`, `codepoint_map_`, and the
`metadata` record; no allocation outside the arena. The
precursor's CPU-side layout is **distinct** from the Fory
envelope shape (which the cook step's stage-4 produces — SPEC
§6.2 stage 4); the importer never emits Fory bytes itself.

The returned `eastl::span<const std::byte>` points into the arena;
the span is valid for the cook session's lifetime per SPEC §9.3.1
(arena drains between cooks, not between frames). The cook step's
stage-4 (`fory-serialize`) consumes the span and produces the
`CookedAsset.payload` (SPEC §4.1.4); the cook-session's stage-5
writes the payload to the CAS (SPEC §4.1.5; cross-link to
`cas-store-design.md` #813 once that design lands).

**Stage 10 (post-`try`/`catch`) — FreeType teardown.**
`FT_Done_Face(face_)` via `GLIBRE_DEFER`. The arena owns
FreeType's internal allocations through the `FT_Memory` hook;
the teardown returns those bytes to the arena's free list (a
no-op for bump arenas — bytes are reclaimed at session end). The
TLS slots `tl_active_arena` / `tl_active_impl` are cleared via
their own `GLIBRE_DEFER` guards. The `<cfenv>` rounding mode is
restored. This step runs unconditionally — on success and on every
`catch`-side error path — to guarantee no FT_Face leaks into the
next cook.

### 3.8 Concurrency model — per-thread `FT_Library`, no shared mutable state

The aggregate is **stateless across cooks** (SPEC §4.1.2 lifetime).
The cook worker pool (§4 worker pool budget) holds N
`FreeTypeImporter` instances, one per worker thread. Each
instance owns:

- One `FT_Library` (per FreeType's per-library thread-affinity
  expectation; §3.5).
- One `FT_Memory` struct (per-instance).
- One pimpl `Impl*`.

There is **no shared state** across importer instances. Unlike the
texture importer (which shares the FreeImage process-global
init state and the global error-callback) and unlike the FBX
importer (which shares the `FbxSetMemoryAllocator` process-wide
hook), the font importer's per-thread `FT_Library` topology means
**no process-wide initialization is required**. Each `create()`
call constructs an isolated library; each `~FreeTypeImporter()`
tears one down. This is the simplest concurrency posture of the
three importers.

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

### 3.9 In-memory layout of the `FontArtifact` precursor

The precursor (the bytes `import_one` returns) is the input to
the cook step's Fory-encode stage. Its in-memory layout is **not**
the Fory envelope shape; it is a packed CPU-side struct emitted
by the codegen rule under
`${CMAKE_BINARY_DIR}/generated/glibre-types/include/glibre/types/content/font_artifact_precursor.hpp`
(per `fory-codegen.md` §Pipeline — the codegen tool emits a
pre-Fory CPU struct alongside the Fory struct for any persistent
type the engine wants a non-Fory in-memory form of). The shape:

```text
FontArtifactPrecursor {
  vocab_version  : u32                  // copy of NormalizeParams.vocab_version; lets the cook step's
                                         // Fory encoder reject precursor / Fory-schema vocabulary mismatches.
  metadata       : FontMetadata         // see below.
  glyphs         : list<GlyphRecord>    // sorted ascending by glyph_index; one row per loaded glyph.
  codepoint_map  : list<CodepointMapEntry>  // sorted ascending by codepoint.
  kerning_pairs  : list<KerningPair>    // sorted ascending by (lhs, rhs); empty if include_kerning = false
                                         //   or if FT_HAS_KERNING is false.
  atlas          : AtlasImage           // see below.
}

FontMetadata {
  pixel_size            : u16            // copy of NormalizeParams.pixel_size.
  ascender_pixels       : f32            // face_->size->metrics.ascender / 64.0
  descender_pixels      : f32            // face_->size->metrics.descender / 64.0 (negative).
  line_height_pixels    : f32            // face_->size->metrics.height / 64.0 (canonical line gap).
  units_per_em          : u16            // face_->units_per_EM (raw font-design-units density; for telemetry).
  postscript_name       : eastl::string_view  // FT_Get_Postscript_Name; borrowed view into arena.
  render_mode           : enum { Bitmap, Sdf }
  hinting               : enum { None, Light, Normal }
  has_kerning_kern_table: bool           // false when source lacks `kern` table OR include_kerning=false.
  glyph_count           : u32            // glyphs.size(); convenience for downstream consumers.
}

GlyphRecord {
  glyph_index        : u32
  advance_pixels     : f32      // metrics.horiAdvance / 64.0
  bearing_x_pixels   : f32      // metrics.horiBearingX / 64.0
  bearing_y_pixels   : f32      // metrics.horiBearingY / 64.0 (positive: above baseline).
  bbox_w_pixels      : u16      // post-rendering bitmap width.
  bbox_h_pixels      : u16
  atlas_u_pixels     : u16      // x of glyph upper-left in the atlas.
  atlas_v_pixels     : u16      // y of glyph upper-left in the atlas.
}

CodepointMapEntry {
  codepoint   : u32   // Unicode codepoint; ≤ 0xFFFF at MVP.
  glyph_index : u32   // 0 = .notdef (no glyph for this codepoint in the source).
}

KerningPair {
  lhs        : u32   // glyph index of left-hand glyph.
  rhs        : u32   // glyph index of right-hand glyph.
  delta_x_26_6 : i32  // 26.6 fixed-point as FreeType returns it; render's text rasterizer divides by 64 at use.
  delta_y_26_6 : i32  // typically 0 for Latin scripts; preserved for completeness.
}

AtlasImage {
  width_pixels  : u16   // = atlas_w (a power of two ≤ atlas_max_dim).
  height_pixels : u16   // = atlas_h (a power of two ≥ packed_height).
  pixel_format  : enum { GrayBitmap, GraySdf }   // matches metadata.render_mode discriminant.
  pixels        : eastl::span<const std::byte>   // width * height bytes; row-major top-down.
}
```

**Why a single mip-0 atlas.** The precursor carries one canonical
atlas at one pixel size at MVP per §3.6 (no mip generation, no
multi-size bake). Mip-chain authoring is `render`'s job (mip
selection often fused with sampler-derived blur threshold); a
multi-size bake would re-enter post-MVP per §12 OQ-2.

**Why no Fory schema escape.** The CPU-side struct is the
**fast-path in-memory form** with cache-coherent layouts (`glyphs`
and `codepoint_map` are contiguous `eastl::vector<T,
ImporterArenaAllocator>` of fixed-stride records; `atlas.pixels`
is a contiguous `eastl::span<const std::byte>` inside the arena).
Fory's encoding step is a deterministic transform between this
form and the `glibre.content.FontArtifact` schema's tag-sorted
on-disk envelope; the importer produces the CPU-side form because
that is what the cook step's stage-4 expects (per `cook_step.cpp`'s
common interface across Mesh / Texture / Font precursors).

**Why `eastl::string_view` for `postscript_name`.** The precursor
bytes live entirely inside the arena; views into the arena are
valid for the precursor's lifetime. Owning strings would
double-allocate the same bytes. The cook step's Fory-encode stage
copies the views into the Fory envelope's owning representation
(SPEC §6.2 stage 4); after that, the arena drain reclaims the
view's backing memory. Same convention as the FBX importer's
node / bone / material name views (`fbx-importer-design.md` §3.9).

**Why kerning deltas in 26.6 fixed-point rather than `f32`.** The
text rasterizer at runtime accumulates kerning deltas before
rounding; preserving fixed-point precision matches how rendering
tools historically expect kerning to feed into glyph layout
arithmetic. The `i32` storage is also smaller than `f32` for the
same effective range. Conversion to `f32` happens at the render
seam (out of scope here). Field alignment: every numeric field is
naturally aligned (`u16` at 2-byte alignment, `u32`/`f32` at
4-byte alignment). The generated emitter pads to natural
alignment; no bit-packed fields. This keeps the precursor
cache-friendly for the future render-side consumer.

### 3.10 Lifetime — `create` / per-call / `~FreeTypeImporter`

The lifetime contract:

```text
CookSession::begin(...)                  (SPEC §5.12; cook session opens)
  ↓
  for each worker thread W in pool:
    FreeTypeImporter::create()           (SPEC §5.4; once per worker)
      ↓ FT_New_Library(memory_, &library_) + FT_Add_Default_Modules(library_)
        + FT_Property_Set("sdf", "spread", default-spread); allocates Impl pimpl.
  ↓
  for each RecookRequest assigned to W (one at a time):
    FreeTypeImporter::import_one(        (SPEC §5.4; many calls per importer)
      src, params, arena, cancel)
      ↓ stages 1..9 above; arena scratch for the call;
        FT_Face via FT_New_Memory_Face routed through FT_Memory hook;
        FT_Done_Face at stage 10 via GLIBRE_DEFER.
  ↓
  ~FreeTypeImporter()                     (cook session ends or worker retires)
    ↓ FT_Done_Library(library_); releases Impl pimpl.
```

`create` returns `Result<eastl::unique_ptr<FreeTypeImporter>>`
(SPEC §5.4 verbatim). The smart pointer is an `eastl::unique_ptr`
per PHILOSOPHY §11 (no `std::unique_ptr` in engine code); the
deleter is the standard EASTL deleter that calls
`~FreeTypeImporter`.

The `import_one` call is **synchronous** — returns when the
precursor is emitted or an error fires. No async future, no
coroutine resumption (PHILOSOPHY: no coroutines in engine code).

The `~FreeTypeImporter` body:

1. `FT_Done_Library(library_)` — releases every library-owned
   resource through the `FT_Memory` hook; bytes return to the
   per-cook arena (which is itself drained at session end; SPEC
   §9.3.1 rule 3). FreeType's docs say `FT_Done_Library` does not
   throw; even so the destructor wraps it in a `try` block per
   the standard noexcept-destructor pattern (catch-all silently
   logs at `error` and swallows; PHILOSOPHY: every public
   destructor is noexcept).
2. Reset the pimpl.

The destructor is `noexcept` (PHILOSOPHY: every public destructor
in engine code is noexcept; the carve-out is the importer's
`import_one` body, not its destructor).

## 4. Public surface

The §5.4 stub is the only public C++23 header surface the importer
exports. Reproduced here for cross-reference; **this design does
not modify the stub**.

```cpp
// SPDX-License-Identifier: Apache-2.0
// content/include/glibre/content/importer.hpp — font-importer slice.
//
// Locked in specs/content/SPEC.md §5.4. -fno-exceptions header;
// the implementation TU (freetype_importer.cpp) carries the
// unique -fexceptions carve-out per §10.3 / reviews/decisions/error-model.md
// §"Decision" rule 3.

namespace glibre::content {

class FreeTypeImporter final : public Importer {
public:
    // Allocates a per-thread FT_Library configured with the
    // engine's per-cook-arena FT_Memory hook; FT_Add_Default_Modules
    // brings in SFNT/TT/CFF/auto-hinter/smooth-rasterizer/SDF.
    // Per-thread instance; not safe to share across worker threads
    // (FreeType per-library thread-affinity rule, §3.5).
    [[nodiscard]] static auto create() noexcept
        -> Result<eastl::unique_ptr<FreeTypeImporter>>;

    // Read the font source, normalize per `params`, emit the
    // `glibre.content.FontArtifact` precursor bytes into `out_arena`.
    // FreeType errors translated at the carve-out boundary into
    // `ImporterError::*` arms; never escape.
    //
    // Cancellation (§3.7 stage 5) polled at every per-glyph load
    // boundary; observed cancellation returns `ImporterError::Cancelled`
    // promptly (§4.1.2 inv #5).
    //
    // The returned span points into `out_arena` and is valid for
    // the arena's lifetime (cook session; §9.3.1 rule 3).
    [[nodiscard]] auto import_one(const SourceAsset&        src,
                                  const NormalizeParams&    params,
                                  ImporterArena&            out_arena,
                                  const CancellationToken&  cancel) noexcept
        -> Result<eastl::span<const std::byte>>;

    ~FreeTypeImporter();

private:
    FreeTypeImporter() noexcept;
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
  add a typed `NormalizeParamsView` adaptor (shared with the FBX /
  Texture importers) that decodes the bytes into the per-importer
  fields, exported through the same header. The body of the typed
  view lives inside
  `${CMAKE_BINARY_DIR}/generated/glibre-types/include/glibre/types/content/freetype_normalize_params.hpp`
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
the FreeType internals.

No second public header is added. Internal helpers (the
`FT_Memory` hook callbacks, the `tl_active_arena` TLS slot, the
skyline packer, the §3.7 stage emitters) live behind the
implementation TU's translation-unit boundary.

## 5. Hot/cold path split

`font-importer` is a **build-time-only** aggregate. SPEC §6.5
names the cuts: `importers/freetype_importer.{hpp,cpp}` is shipped
only into the `tools/glibre-cook` host executable, **not** into
`glibre-content.dylib`. The runtime cannot link the importer;
calling into `FreeTypeImporter::*` from a shipping build is a
link-time error (undefined symbol), not a runtime branch. Every
section below is the cook-tool's profile; the runtime profile
elides the entire aggregate.

### 5.1 Hot path — within one `import_one` call

Stages 1–10 (§3.7) form the import call's hot path. Stage-by-stage
profile for the design's reference workload (a `BasicLatin`-subset
SDF bake at 48 px on a 2048×2048 atlas with `sdf_spread=8`; M1
firestorm; ~95 glyphs):

| Stage              | Hot/cold | Bound (BasicLatin SDF, M1 firestorm) | Dominant cost                                                       |
|--------------------|----------|---------------------------------------|---------------------------------------------------------------------|
| 1 Source load + setup | Cold  | ≤ 5 ms                                 | platform::FileIo::read_into into arena; TLS + cfenv setup.           |
| 2 FT_New_Memory_Face | Cold  | ~ 1–10 ms                              | SFNT directory parse; CFF / glyf table indexing.                     |
| 3 FT_Set_Pixel_Sizes | Cold  | ≤ 1 ms                                 | One library call; updates internal scaler.                           |
| 4 Refuse-if-unsupported probe | Cold | ≤ 100 μs                       | A few flag checks.                                                    |
| 5 Glyph load + cancel poll + metric extract | **Hot** | ~ 25–125 ms     | N × (FT_Load_Glyph + FT_Render_Glyph + metric copy). SDF dominates.  |
| 6 Kerning extraction | Hot     | 0–10 ms                                | N² FT_Get_Kerning probes; zero on GPOS-only fonts.                   |
| 7 Atlas pack + bitmap copy-out | **Hot** | ~ 1–5 ms                       | Skyline pack + N × bitmap memcpy; arena allocation.                  |
| 8 Determinism canonicalization | Hot | ≤ 2 ms                            | Sort + 26.6→f32 conversion + ban_nan scan.                           |
| 9 Precursor emit   | Hot      | ~ 1 ms                                 | Straight-line memcpy + metadata write into arena.                    |
| 10 FreeType teardown | Cold   | ≤ 1 ms                                 | FT_Done_Face; FT_Done_Library at session end (cold).                 |

BasicLatin-SDF totals to ~30–155 ms per cook on the worker thread —
**off the game-loop driver thread**. The 0.50 ms content cell
(`perf-budget.md`) is **not** charged for this; the 64 MiB
importer soft sub-ceiling (SPEC §9.3.1) is the relevant gate.

The CPU profile is dominated by stage 5 (FreeType glyph load +
SDF render) — particularly SDF distance-field computation, which
is the single most expensive operation per glyph (FreeType's
`sdf` module's `bsdf` algorithm scales with `bbox_w × bbox_h ×
spread²`). Stage 7 (atlas pack + bitmap copy-out) is bounded by
arena memcpy bandwidth (~16 MiB/s on the 4096×4096 cap; ≪ DRAM
bandwidth, so well under 1 ms).

For larger subsets (e.g. `BmpAscii` with ~205 glyphs at 64 px
SDF), stage 5 scales roughly linearly to ~100–300 ms. A 4096×4096
atlas at SDF mode 64 px does not breach the 64 MiB soft sub-
ceiling (atlas ≈ 16 MiB; FreeType library state ≈ ~few MiB; source
file ≈ ~few MiB; total typical ≈ ~25 MiB peak). Pathological
subsets (CJK ranges, SMP emoji) are out of scope at MVP — see §3.4
refusal of codepoints above U+FFFF.

### 5.2 Cold path — `create` and `~FreeTypeImporter`

`create` runs once per worker thread per cook session opening
(§3.10). Costs:

- `FT_New_Library` + `FT_Add_Default_Modules`: ~1–2 ms (module
  table init).
- `FT_Property_Set("sdf", "spread", ...)`: <100 μs.
- Pimpl alloc through arena: <100 μs.

Total: ~3 ms per worker per session. Cook sessions typically
begin once per editor save burst; this is amortised to zero in
steady state.

`~FreeTypeImporter` runs once per worker per session close
(§3.10). Cost:

- `FT_Done_Library` (releases plugin dispatch tables + module
  state): ~1 ms.

These are the only paths touched outside `import_one`; both run
off the game-loop driver thread (cook worker pool). The runtime
never runs them.

### 5.3 No runtime hot path

The runtime (`glibre-content.dylib`) exposes the §5 surface but
**never calls** `FreeTypeImporter::*`. SPEC §6.5 cut row makes
this a build-time guarantee; SPEC §10.3's `-fexceptions` carve-out
is build-time-only (the runtime TUs all build with
`-fno-exceptions`). Any future runtime path that wanted to ingest
font sources or rasterize glyphs on demand would re-introduce
FreeType linkage into the runtime dylib — explicitly refused by
SPEC §3.3 and by §1 above (the runtime is a producer of
`FontArtifact` consumers, not a consumer of sources). The §3.3 +
§6.5 commitments together ensure the importer's cost is bounded
to the cook tool's profile.

## 6. Concurrency

### 6.1 Frame phase ownership

The font importer **does not run on the game-loop driver thread**.
SPEC §9.3.1 ("Off-thread soft ceiling — importer scratch arena")
pins this: cook orchestration runs entirely on the **importer
worker pool** outside the frame loop. The phase-by-phase
contribution:

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
FreeType). The font importer's per-worker contribution:

- **Pool size**: 4 workers at MVP (per `fbx-importer-design.md`
  §6.2 / `texture-importer-design.md` §6.2 — same pool). Sized
  from `perf-budget.md`'s `content` row "one-shot import work is
  off-thread"; the precise count is `cores − 1` clamped to
  `[2, 4]`. Final number is the implementation plan's call (per
  `task-breakdown-content-font-importer-detailed`); this design
  pins the **shape** (one `FreeTypeImporter` per worker) and the
  **upper bound** (≤ 4) for the perf budget §9.3.1's 64 MiB soft
  sub-ceiling.
- **One `FreeTypeImporter` per worker**, allocated at session
  start (§3.10), destroyed at session end. The aggregate is
  **per-thread**; the `FT_Library` is per-instance (and therefore
  per-thread), with no shared global state. This is the simplest
  concurrency posture of the three importers.
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

- **Per-context tag**. Every importer allocation is stamped with
  `ContextTag::content`. The `FT_Memory` hook (§3.5) routes every
  FreeType allocation through `glibre::PerContextAllocator`
  tagged `ContextTag::content`. EASTL containers used by the
  importer (`eastl::vector<T, ImporterArenaAllocator>`) carry the
  same tag through their allocator type.
- **Soft 64 MiB sub-ceiling**. Per SPEC §9.3.1 rule 2; the cook
  scratch arena breaches the ceiling at `warn` level in shipping
  builds and `OutOfBudget` in `GLIBRE_ALLOC_STRICT=1` builds.
  Joint accounting: arena-tagged bytes ONLY (FreeType allocs
  through the hook, so they are arena-tagged — unlike the texture
  importer which had to do RSS-delta sampling around FreeImage's
  internal mallocs). A `BasicLatin` SDF bake at 2048×2048 typical
  arena peak ≈ ~25 MiB (4 MiB atlas + ~5 MiB FreeType library/face
  internals + ~5 MiB source bytes for a typical TTF + ~5 MiB
  packer / metric / kerning scratch) — well under the 64 MiB
  ceiling. A `BmpAscii` 4096×4096 cap bake peaks ≈ ~30 MiB —
  still well under. Pathological large subsets (out of MVP scope)
  would be the only way to breach.
- **Multi-frame, off-thread arena lifetime**. Per SPEC §9.3.1
  rule 3, the arena drains between cooks (at session end) —
  **not** between frames. The transient-arena exemption
  (Allocator Rule 4) does **not** apply. The 64 MiB counts
  against the 256 MiB `content` heap cell (Allocator Rule 1).
- **No raw `new` / `malloc`**. Per `perf-budget.md` Allocator
  Rules header + SPEC §9.4 rule 7. The build rejects raw
  allocations in `plugins/content/cook/import/`. FreeType's
  vendor-internal `malloc` is the third-party-wrap exemption
  (§4.1.2 inv #3), routed through the `FT_Memory` hook; this is
  one of two third-party allocator carve-outs in the engine
  (the other being the FBX SDK's `FbxSetMemoryAllocator`).
- **Arena-allocation null-return protocol (`GLIBRE_ALLOC_STRICT=1`)**.
  When `PerContextAllocator` refuses an allocation in strict mode
  (`OutOfBudget`), the `FT_Memory` hook records the refusal in
  the atomic `alloc_exhausted_` flag on `Impl` and returns
  `nullptr` to FreeType. FreeType propagates the failure as
  `FT_Err_Out_Of_Memory`; the §10.2 translation table maps that
  to `ImporterError::MalformedPayload(freetype-alloc-exhausted)`
  immediately. **Before any FreeType call that may trigger the
  hook**, `import_one` checks the atomic flag and returns the
  same arm — preventing FreeType from observing a stale `OK` from
  a prior call after the arena has been exhausted. In practice,
  the 64 MiB soft sub-ceiling (§9.3.1) is sized well above the
  observed peak (~25–30 MiB), so the null path is a defect guard
  rather than a routine production path. The
  `GLIBRE_ALLOC_STRICT=1` CI gate (`perf-budget.md` CI Gate §3)
  validates the ceiling on the reference fixtures.

### 6.4 Cancellation propagation

SPEC §4.1.2 inv #5 + SPEC §4.1.9 inv #5 require cancellation to
propagate within ~one frame's wall-clock. The mechanics:

1. `CookSession::cancel()` sets the session's `CancellationToken`
   to `cancelled = true` (atomic store, relaxed).
2. The worker thread's `import_one` polls
   `cancel.is_cancelled()` once per glyph in stage 5 (§3.7).
   Polling cost: one `memory_order_acquire` atomic load per
   glyph, ~few ns; a 256-glyph subset adds ~256 ns total —
   invisible.
3. Observed cancellation returns `ImporterError::Cancelled` from
   `import_one` immediately; the worker thread proceeds to the
   `~FreeTypeImporter` path (or to its next dequeue if the
   importer instance is reused).
4. The arena drain (at session abort) reclaims any in-flight
   allocations.

Bound: ≤ ~5 ms wall-clock from `cancel()` to `import_one`
returning. Stage 1 (source buffer load), stage 2 (`FT_New_Memory_Face`)
and stage 7 (atlas pack — internal to a single batched scan
without per-glyph polling once stage 5 finishes) are the
non-cancellable points; each is bounded individually well under
the 5 ms target. The glyph-loop in stage 5 is the dominant cost
(~25–125 ms) and is fully cancellable; observed cancellation in
stage 5 returns within one glyph's processing time (~few hundred
μs SDF, a few μs bitmap).

### 6.5 Off-thread invariants

Two invariants govern the off-thread topology (shared with
`fbx-importer-design.md` §6.5 / `texture-importer-design.md`
§6.5):

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

The font-importer aggregate **does not directly persist anything**.
Its outputs feed downstream stages (Fory-encode → CAS write →
manifest publish) per SPEC §6.2. This section pins the four
ABI / persistence seams the importer **participates in** but does
**not own**.

### 7.1 Importer outputs that flow into persistence

The importer emits an in-memory `FontArtifactPrecursor` (§3.9).
The cook step's stage 4 (`fory-serialize`, SPEC §6.2) consumes
the precursor and produces the on-disk
`glibre.content.FontArtifact` Fory payload. The precursor →
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
   `importer_version` has the same effect. The font importer's
   digest tracks the FreeType version verbatim, so a FreeType
   patch upgrade re-cooks every dependent font; mesh / texture
   cooks are unaffected unless their importer_version also moves
   (per the per-importer narrowing pattern from
   `texture-importer-design.md` §7.2).

The CAS ABI is not perturbed: the cooked artifacts under
`cooked/<prefix>/<hash>` are addressed by `ContentHash`, not by
`CookKey`; old hashes survive a re-cook (SPEC §4.1.5 inv #2;
§7.2.2 last paragraph). The cross-link to
`cas-store-design.md` (#813) detailed-design will pin the CAS
side of this seam; the importer requires only that the cook
session writes **before** publishing, which `cas-store-design.md`
will commit to.

### 7.3 FreeType — the OS-side dependency

FreeType is the importer's only third-party SDK dependency at
MVP. Its version is the `FREETYPE_TAG` component of
`importer_version` (§3.2). Build-system note:

- The cook tool links `libfreetype.dylib` from the vcpkg-pinned
  distribution per CLAUDE.md "Tech Stack (locked)". FreeType's
  vcpkg port (`freetype`) is configured **without** the
  Brotli / WOFF2 / zlib / ICU / SVG / HarfBuzz feature gates at
  MVP — the only enabled features are the SFNT / TrueType / CFF
  parsers and the `sdf` / `smooth` rasterizer modules.
- The runtime dylib (`glibre-content.dylib`) does **not** link
  FreeType. SPEC §6.5 cut.
- The FreeType version is recorded in three places: (a) the
  `vcpkg.json` manifest at the cook tool target, (b) the
  `plugins/content/cook/import/freetype_importer.cpp` constant
  string used to derive `importer_version` (§3.2), and (c) the
  cook session's `CookKey.downstream_versions` list (SPEC
  §4.1.3 component #5; the `(tool_name, tool_version)` pair). A
  build-time codegen step asserts (a) and (b) agree (the
  FreeType `FT_Library_Version` runtime check at first
  `create()` provides a fourth confirmation). The four must
  agree.

The FreeType binary ABI is the OS-side dependency the importer
hides from the rest of the engine. A FreeType SONAME bump
triggers a re-cook (via `importer_version` advancing) and a
manifest re-publish (next session). The runtime never observes
FreeType's symbols directly.

### 7.4 No new Fory schemas authored here

Per the §1 refusal list, the importer authors no Fory schema.
The schemas it produces bytes for / against are all owned by
`data`:

| Schema                                | FQN                                | Owned by | Importer role                                |
|---------------------------------------|------------------------------------|----------|----------------------------------------------|
| `FontArtifact.fory`                   | `glibre.content.FontArtifact`      | `data`   | Producer of precursor bytes.                 |
| `Manifest.fory`                       | `glibre.content.Manifest`          | `data`   | None (manifest is the cook session's).       |
| `ManifestEntry.fory`                  | `glibre.content.ManifestEntry`     | `data`   | None (cook session writes; importer doesn't).|
| `CookKey.fory`                        | `glibre.content.CookKey`           | `data`   | Contributes `importer_version` field only.   |
| `DependencyEdge.fory`                 | `glibre.content.DependencyEdge`    | `data`   | None at MVP (font sources have no transitive children; TTF/OTF are leaf assets in the §4.1.10 dependency graph). |

The font importer is a **leaf** in the dependency graph at MVP.
Like the texture importer (`texture-importer-design.md` §7.4),
fonts have no embedded sub-asset references that require
external resolution: TTF/OTF tables are entirely self-contained.
This simplifies the cook session's stage-6 edge-emission for
font cooks: zero edges per font cook.

If a future format with cross-asset references lands (e.g. a
font-pack referencing external SDF atlas variants, or a SVG
color-glyph font referencing external SVG primitives), it would
re-enter post-MVP under a `vocab_version` bump and add
external-reference-shaped edges at that time.

### 7.5 `NormalizeParams` is not directly persisted

The §3.4 vocabulary's canonical-bytes encoding is hashed into
`CookKey.normalize_params_hash` (SPEC §7.1.2 component #4). The
bytes themselves are **not** persisted — the manifest holds only
the hash (SPEC §7.1.2 audit-only field set). This matches
`fory-codegen.md`'s ABI principle: the manifest records hashes,
not raw inputs. Same convention as the FBX / Texture importers.

### 7.6 Plugin ABI seam — none crossed by importer

The importer is build-time-only (SPEC §6.5). The runtime plugin
ABI hash (`glibre_types_abi_hash`, `plugin-abi.md`) does not
change with importer-only edits unless the importer's edits also
bump a Fory schema in `data/schemas/`. The §3.2
`importer_version` digest is **internal to content's cook
pipeline**; it does not appear in the plugin manifest's
`abi_hash` field (`plugin-abi.md` schema). A runtime hot-reload
of `glibre.content` (SPEC §8 plugin-code reload) sees no
importer activity — the runtime cuts (§5.3) elide the FreeType
linkage entirely.

The cook tool's own re-link with a new FreeType version is
**not** a runtime hot-reload; it is a workspace re-tooling event.
Operators distribute new cook-tool builds out-of-band; the
runtime is unaware.

## 8. Hot-reload

The aggregate is build-time-only (SPEC §6.5); the runtime never
loads it. Hot-reload contributes through two indirect paths.

### 8.1 Manifest-swap path (the dominant content hot-reload)

When a `.ttf` / `.otf` source changes on disk, the watcher
delivers a `FileEvent` (SPEC §4.1.10) → `WatchEdge::translate`
produces `RecookRequest`s → `CookSession::commit()` runs the
§3.7 pipeline on the worker pool → CAS write → manifest staging
→ loader's phase-8 publish (SPEC §8.2 steps 1–5).

The importer's role in this path is exactly stages 1–10 of §3.7;
no hot-reload-specific code appears here. The importer is a
**producer of bytes**; the publish that makes the bytes
hot-reload-visible is the cook session's + loader's job.

What survives the manifest swap (per SPEC §8.3 row table) on
the importer's side:

- **`FreeTypeImporter` instances**: do not survive (cook session
  worker pool destroyed by the swap if the swap is a
  content-plugin reload; on a manifest-only swap, they live
  across because no plugin code moves).
- **FreeType library state**: per-instance (per worker thread);
  there is no process-global FreeType state to survive (unlike
  FreeImage's `FreeImage_Initialise` and FBX SDK's
  `FbxSetMemoryAllocator` hook, both of which are process-wide).
  The simplest survival contract of the three importers.
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

**Does the font importer participate?** Only indirectly:

1. **`drain` step (SPEC §8.3 row "In-flight RecookRequest queues
   / CookSession worker pools").** The current cook session's
   worker pool is destroyed; in-flight `import_one` calls
   observe cancellation via the `CancellationToken` (§3.7
   stage 5) and return `ImporterError::Cancelled`. The arena
   is drained as part of session abort. Each
   `~FreeTypeImporter()` calls `FT_Done_Library` for its
   per-thread library.
2. **`swap` step.** No importer code is loaded into the runtime
   (SPEC §6.5). The runtime swap does not move FreeType
   symbols. The importer is unaffected.
3. **`migrate` step.** The importer authors no migration body
   (§7.4); migrations on the manifest layer (`ManifestEntry`,
   `CookKey`, `DependencyEdge`) are §8.4 of SPEC. The importer
   is unaffected.
4. **`register` step (SPEC §8.4 last paragraph).** The new
   content plugin's register re-spawns the cook worker pool.
   New `FreeTypeImporter` instances are constructed by the new
   pool; old instances (destroyed in drain) leave no residue.
   Each new instance constructs its own fresh `FT_Library`;
   there is no process-global state to re-initialize.

The importer's plugin-code reload contract is therefore: **no
body runs across the reload**. The §3.10 `~FreeTypeImporter`
runs at drain; new instances start fresh at register. There is
no process-global FreeType state to coordinate (unlike
FreeImage / FBX SDK). This is the simplest reload contract of
the three importers.

### 8.3 Refusal cases (importer-side)

The importer contributes the seven refusal causes already
pinned in SPEC §8.5 Class B:

| Refusal cause                                              | Stage triggering | Returned arm                                                      |
|------------------------------------------------------------|------------------|-------------------------------------------------------------------|
| Path escapes `assets/source/`                               | 1                | `ImporterError::SourceNotFound`                                   |
| File missing at `platform::FileIo::read_into` time          | 1                | `ImporterError::SourceNotFound`                                   |
| FreeType reports `FT_Err_Unknown_File_Format`               | 2                | `ImporterError::MagicMismatch` (`freetype-magic`)                 |
| FreeType reports `FT_Err_Invalid_Version`                   | 2                | `ImporterError::UnsupportedVersion` (`freetype-version`)          |
| Multi-face TTC / variable-font axes                         | 4                | `ImporterError::UnsupportedVersion` (`freetype-{ttc-multiface, variable-font}`) |
| FreeType reports glyph / table corruption                   | 2 / 5            | `ImporterError::MalformedPayload` (`freetype-corruption`)         |
| Atlas overflow at the configured `atlas_max_dim`            | 7                | `ImporterError::MalformedPayload` (`freetype-glyph-atlas-overflow`) |
| Cancellation observed                                       | 5                | `ImporterError::Cancelled`                                        |

All surface as `CookOutcome::RolledBack` from the cook session
(SPEC §4.1.9 inv #1). The prior manifest snapshot remains active
per SPEC §8.5 Class B. Operator action is the §10.1 per-arm
column ("Re-export at supported version", "Restore source",
"Split TTC into single-face files", "Reduce subset / increase
`atlas_max_dim` / shrink `pixel_size`", etc.).

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
one-shot-import-handoff variance. The font-importer
aggregate's contribution to that cell:

- **Driver-thread CPU**: **0 ms steady-state, 0 ms peak** (SPEC
  §9.3 row `Importer (§4.1.2)`). The aggregate runs entirely
  off the driver thread (SPEC §9.3.1, §6.1 above). The cell is
  unaffected.
- **Off-thread CPU**: bounded by the cook worker pool's
  wall-clock per cook (~30–155 ms per BasicLatin SDF bake; §5.1
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
  `GLIBRE_ALLOC_STRICT=1`. Joint accounting is arena-tagged
  bytes only (no RSS-delta needed; FreeType's allocations
  route through the hook into the arena).

### 9.2 Per-stage budget

Per §5.1 above, restated as a budget table with the relevant
gate (BasicLatin SDF reference workload at 48 px on 2048×2048):

| Stage                        | CPU bound (BasicLatin SDF, M1 firestorm) | Heap bound (per cook) | Gate                          |
|------------------------------|-------------------------------------------|-----------------------|-------------------------------|
| 1 Source load + setup        | ≤ 5 ms                                     | ~5 MiB (source bytes; arena-tagged) | Off-thread; not gate-measured |
| 2 FT_New_Memory_Face         | ~ 1–10 ms                                  | ~5 MiB (FreeType library/face state; arena-tagged via FT_Memory hook) | Off-thread; not gate-measured |
| 3 FT_Set_Pixel_Sizes         | ≤ 1 ms                                     | < 1 KiB                                | Off-thread; not gate-measured |
| 4 Refuse-if-unsupported probe| ≤ 100 μs                                   | < 1 KiB                                | Off-thread; not gate-measured |
| 5 Glyph load + cancel poll   | ~ 25–125 ms                                | ~5 MiB (glyph_slots_, scratch bitmaps via FT_Memory) | Cancel ≤ 5 ms latency |
| 6 Kerning extraction         | 0–10 ms                                    | < 1 MiB                                | Off-thread; not gate-measured |
| 7 Atlas pack + bitmap copy-out | ~ 1–5 ms                                | ~4 MiB (atlas_buffer_; arena-tagged)   | Determinism golden            |
| 8 Determinism canonicalization| ≤ 2 ms                                    | < 1 MiB                                | Determinism golden            |
| 9 Precursor emit             | ~ 1 ms                                     | ~ 1 MiB                                | Per-call returned span        |
| 10 FreeType teardown         | ≤ 1 ms                                     | (arena-frees ~5 MiB face state)        | Off-thread; not gate-measured |
| **per-cook total**           | **~ 30–155 ms**                            | **~ 25 MiB peak**                      | §9.3.1 soft ceiling 64 MiB   |

For larger subsets (e.g. `BmpAscii` at 64 px SDF on the 4096×4096
cap), stage 5 dominates and scales roughly linearly with the
glyph count. A 200-glyph 64-px SDF bake on the 4096×4096 cap
peaks at ~30 MiB arena (16 MiB atlas + ~14 MiB FreeType internals
+ scratch), still well under the 64 MiB soft sub-ceiling. Wall-
clock is bounded by the SDF rendering cost (~150 μs/glyph at
spread=8, 64 px → ~30 ms total for stage 5).

The per-cook bounds are **operational targets**, not perf-gate
asserts — the perf gate measures driver-thread cost, not
worker-thread wall-clock. A pathologically-slow font (large
custom subset, large `pixel_size`) extends stage 5 wall-clock
without breaking any gate; the operator observes it as "the cook
took longer than expected" via the cook tool's progress UI (out
of scope; future editor concern).

### 9.3 CI gate hooks

Per `perf-budget.md` §"CI Gate Spec" + SPEC §9.5:

1. **`content/import: scratch_arena_off_thread_residency`**
   (SPEC §9.5 row 4). Asserts the per-cook arena's peak
   resident bytes ≤ 64 MiB on the font-importer reference
   fixture (BasicLatin SDF at 48 px / 2048×2048; ~25 MiB peak).
   PR fails if exceeded. Shared with the FBX / Texture importers'
   gates (per `fbx-importer-design.md` §9.3 #1 /
   `texture-importer-design.md` §9.3 #1) — all three write into
   the same `BENCHMARK_CELL` slot; the joint test exercises
   each importer's worst-case fixture.
2. **`content/import: s3_off_thread_no_driver_spike`** (SPEC
   §9.5 end-to-end gate). Drives the cook worker pool with the
   font-importer reference fixture (BasicLatin SDF at 48 px) for
   600 frames while S1 plays on the driver thread; asserts the
   driver thread's p99 phase-1 + phase-9 cost stays inside the
   0.50 ms content cell. PR fails if exceeded. This is the gate
   that proves the font importer does not bleed onto the game
   loop. Shared with the FBX / Texture importers (the test
   fixture exercises whichever importer the workload triggers).
3. **`content/import: font_determinism_golden`** (cross-cite
   to §11 below). Microbenchmark + golden test: cook the same
   font bytes twice (different runs of the cook tool), assert
   the resulting precursor bytes are byte-equal. This is the
   PHILOSOPHY §7 cross-host determinism gate applied to the
   font path.
4. **`content/import: font_cancellation_latency`** (cross-cite
   §11). Asserts that `import_one` returns
   `ImporterError::Cancelled` within ≤ 10 ms of `cancel()`
   being called on a BasicLatin SDF mid-bake. The 10 ms bound
   exceeds the ~5 ms theoretical bound from §6.4 to admit CI
   noise.

### 9.4 What is not gated

The per-cook wall-clock (~30–155 ms) is **not** gated. Reasons:

- It is wall-clock dominated by FreeType's glyph load + SDF
  render path, which we do not optimize.
- It varies across hosts (CI vs developer machine) and across
  font + subset + render-mode combinations; a tight gate would
  flap on any source larger or smaller than the reference
  fixture.
- It is operationally visible to the developer (cook tool's
  progress UI) and to the spike #146 brief's S3 fixture (which
  already exercises the importers off-thread invariant).

A future tightening could land if a regression spike opens; not
this design's concern.

### 9.5 Driver-thread invariant

The single perf invariant the font importer must hold:

> **Zero importer activity on the game-loop driver thread,
> regardless of how many font cooks are in flight on the worker
> pool.**

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
implementation-grain detail and pins the FreeType-error → arm
mapping at the carve-out (SPEC §10.3).

**Cross-boundary wrapping chain** (per `error-model.md`
§Composition Rules 1–2, mirroring `fbx-importer-design.md`
§10 / `texture-importer-design.md` §10): `ImporterError` values
are leaves of the content context's error sum. At the `content`
plugin's public boundary, the `Result<eastl::span<const std::byte>>`
returned by `import_one` carries a `glibre::Error` whose variant
holds a `glibre::content::Error` (the
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

The font-importer aggregate emits exactly the five
`ImporterError` arms below — `MissingDependency` is emitted by
the manifest-publish path (SPEC §10.1) and is not owned here
(fonts are leaf assets; §7.4).

| Arm                                | Emit site (stage)            | `error.detail` prefixes                                                                                                                                          | Severity (SPEC §10.5) |
|------------------------------------|------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------|
| `ImporterError::SourceNotFound`    | §3.7 stage 1                 | `"freetype-not-found"`, `"platform-io-missing"`                                                                                                                   | `warn`                |
| `ImporterError::MagicMismatch`     | §3.7 stage 2                 | `"freetype-magic"`                                                                                                                                                | `warn`                |
| `ImporterError::UnsupportedVersion`| §3.7 stages 2, 4             | `"freetype-version"`, `"freetype-ttc-multiface"`, `"freetype-variable-font"`                                                                                      | `warn`                |
| `ImporterError::MalformedPayload`  | §3.7 stages 2, 3, 4, 5, 7, 8; carve-out catch-all | `"freetype-corruption"`, `"freetype-invalid-pixel-size"`, `"freetype-codepoint-out-of-bmp"`, `"freetype-pixel-size-out-of-range"`, `"freetype-glyph-atlas-overflow"`, `"freetype-nan-metric"`, `"freetype-alloc-exhausted"`, `"freetype-unclassified"`, `"platform-io"` | `warn` for corruption / glyph atlas / nan; `error` for `"freetype-alloc-exhausted"`, `"platform-io"` |
| `ImporterError::Cancelled`         | §3.7 stage 5                 | `"freetype-cancelled"`                                                                                                                                            | `debug`               |
| `ImporterError::MissingDependency` | (never emitted by font importer; manifest-side only) | n/a                                                                                                                                  | (n/a)                 |

The structured-log fields the importer attaches (per SPEC §10.5):

- `asset_id`: the `AssetId` the cook is on (from the cook
  session's scheduling).
- `source_path`: `SourceAsset.path` (workspace-relative).
- `importer_kind`: literal `"freetype"`.
- `importer_version`: 64-char BLAKE3 hex of `version_` (§3.2).
- `cook_session_id`: opaque from the cook session.
- `error.detail`: per the prefix table above.
- (For `MalformedPayload(freetype-corruption)`)
  `error.table_tag`: a 4-character tag (`"glyf"`, `"head"`,
  `"name"`, etc.) when FreeType's error names a specific SFNT
  table; empty otherwise. (FreeType's high-level `FT_Error`
  enum does not carry the offending tag programmatically — the
  importer recovers it from FreeType's debug-log output when
  available; otherwise empty. The structured log records the
  field regardless.)
- (For `MalformedPayload(freetype-corruption)` from the glyph
  loop)
  `error.glyph_index`: the offending glyph's index.
- (For `MalformedPayload(freetype-nan-metric)`)
  `error.glyph_index` / `error.metric_field`: the offending
  glyph + which `f32` field carried NaN.
- (For `MalformedPayload(freetype-glyph-atlas-overflow)`)
  `error.atlas_dim_required`: the `(width, height)` the packer
  determined was needed but exceeded `atlas_max_dim`.
- (For `MalformedPayload(freetype-codepoint-out-of-bmp)`)
  `error.codepoint`: the offending codepoint value.
- (For `UnsupportedVersion(freetype-ttc-multiface)`)
  `error.num_faces`: the number of faces FreeType reported.

### 10.2 FreeType-error translation table

FreeType is a C library and its public API returns `FT_Error`
integer codes rather than throwing. The carve-out's `try` block
exists primarily for `std::bad_alloc` defence-in-depth and for
the rare exception that may leak from internal codec code (none
linked at MVP — Brotli / zlib / SVG hooks are all disabled).

Rows 1-9 cover **`FT_Error` return-value classification** —
read on every FreeType call. Rows 10-12 cover thrown exceptions
inside the `try` block.

Matches SPEC §10.3's classification table verbatim with
one-cause-per-row precision:

| Codec error source                                                            | Translated arm                                | `error.detail` prefix                | Notes                                                                                       |
|-------------------------------------------------------------------------------|-----------------------------------------------|--------------------------------------|---------------------------------------------------------------------------------------------|
| `FT_Err_Unknown_File_Format` from `FT_New_Memory_Face`                        | `ImporterError::MagicMismatch`                | `"freetype-magic"`                   | First-byte check fails to match any registered SFNT/CFF format.                              |
| `FT_Err_Invalid_Version` from any FreeType call                               | `ImporterError::UnsupportedVersion`           | `"freetype-version"`                 | SFNT major/minor version unsupported by the linked SFNT module.                              |
| `FT_Err_Invalid_File_Format` / `FT_Err_Invalid_Stream_Operation`              | `ImporterError::MalformedPayload`             | `"freetype-corruption"`              | Truncated SFNT directory or invalid table layout.                                            |
| `FT_Err_Table_Missing` / `FT_Err_Invalid_Table` (stage 2)                     | `ImporterError::MalformedPayload`             | `"freetype-corruption"`              | Required SFNT table absent or corrupt; `error.table_tag` set when recoverable.               |
| `FT_Err_Invalid_Outline` / `FT_Err_Invalid_Glyph_Index` (stage 5)             | `ImporterError::MalformedPayload`             | `"freetype-corruption"`              | Glyph outline corrupt; `error.glyph_index` set.                                              |
| `FT_Err_Out_Of_Memory` from any FreeType call                                 | `ImporterError::MalformedPayload`             | `"freetype-alloc-exhausted"`         | Triggered by the FT_Memory hook returning null on arena exhaustion (§6.3).                   |
| `FT_Err_Invalid_Pixel_Size` from `FT_Set_Pixel_Sizes` (stage 3)               | `ImporterError::MalformedPayload`             | `"freetype-invalid-pixel-size"`      | The font admits no scalable size at the requested pixel size (rare; bitmap-only fonts).      |
| Multi-face TTC detected at stage 4 (`face_->num_faces > 1`)                   | `ImporterError::UnsupportedVersion`           | `"freetype-ttc-multiface"`           | `error.num_faces` set.                                                                        |
| Variable-font axes detected at stage 4 (`FT_FACE_FLAG_VARIATION` set)         | `ImporterError::UnsupportedVersion`           | `"freetype-variable-font"`           | The default instance is implicit; we refuse rather than silently use it (deterministic intent). |
| Codepoint above `U+FFFF` at stage 4 (`Custom` subset only)                    | `ImporterError::MalformedPayload`             | `"freetype-codepoint-out-of-bmp"`    | `error.codepoint` set.                                                                        |
| Atlas pack overflow at stage 7                                                | `ImporterError::MalformedPayload`             | `"freetype-glyph-atlas-overflow"`    | `error.atlas_dim_required` set.                                                               |
| `NaN` metric detected at stage 8                                              | `ImporterError::MalformedPayload`             | `"freetype-nan-metric"`              | `error.glyph_index` / `error.metric_field` set.                                              |
| `std::bad_alloc` thrown from any FreeType-internal codec                      | terminate (§10.3 `std::bad_alloc` row; SPEC §10.7 OQ-2 resolved → terminate) | n/a                                  | The per-cook arena is sized to fit the §9.3.1 ceiling; bad_alloc inside it is a defect.     |
| Any other `std::exception` derivative (rare; only via post-MVP Brotli / SVG)  | `ImporterError::MalformedPayload`             | `"freetype-unclassified"`            | The catch-all arm. The exception's `what()` is logged at `warn` level for triage.            |
| Any non-`std::exception` thrown object                                        | terminate                                      | n/a                                  | Non-`std::exception` cannot be classified; the carve-out's `catch(...)` terminates.        |
| Any unclassified non-zero `FT_Error` return                                   | `ImporterError::MalformedPayload`             | `"freetype-unclassified"`            | Default for any `FT_Error` not matching the rows above; covers FreeType's broad error set.   |

The catch-all `MalformedPayload(freetype-unclassified)` arm
exists because FreeType's error code taxonomy has many specific
codes (over 200) and the table above covers only the
operationally-distinguishable cohort; the catch-all preserves
the §3.2 collapse #1 commitment ("symptom-by-arm, not
codec-by-arm") even when an `FT_Error` does not map cleanly to
one of the named arms.

### 10.3 Exception scope — the unique `-fexceptions` carve-out

Per `error-model.md` §"Decision" rule 3 + §"Consequences" last
bullet, the font importer's implementation TU is one of three
files in the engine that compile with `-fexceptions` (matching
SPEC §10.3 verbatim):

```
plugins/content/cook/import/fbx_importer.cpp        # sibling spike #807
plugins/content/cook/import/freeimage_importer.cpp  # sibling spike #809
plugins/content/cook/import/freetype_importer.cpp   # this design
```

The header `freetype_importer.hpp` and **every other TU in the
engine** compiles with `-fno-exceptions`. The build system
enforces this via a per-TU compile-flag override in the cook
tool's CMakeLists; raw `add_executable(glibre-cook ...)`
configures `-fno-exceptions` globally and the override is
per-source `set_source_files_properties`.

The `try` / `catch` block inside `import_one` is the **single**
translation site. The catch arms:

```cpp
// Sketch — final shape lives in freetype_importer.cpp.
auto FreeTypeImporter::import_one(const SourceAsset& src,
                                  const NormalizeParams& params,
                                  ImporterArena& out_arena,
                                  const CancellationToken& cancel) noexcept
    -> Result<eastl::span<const std::byte>>
{
    // Stages 1..9 inline; FT_Error returns drained per §10.2 inside the
    // straight-line code path (no exceptions from FreeType's C ABI).
    try {
        return emit_precursor(...);
    }
    catch (std::bad_alloc&) {
        std::terminate();                 // §10.2 row 13
    }
    catch (const std::exception& e) {
        return std::unexpected{ Error{
            ImporterError::MalformedPayload,
            ErrorContext{ __FILE__, __LINE__, "freetype-unclassified" } }};
    }
    catch (...) {
        std::terminate();                 // §10.2 row 15
    }
    // Stage 10 (FT_Done_Face) is in a finally-equivalent guard
    // (RAII helper) so it runs on every path including the catches.
}
```

The RAII guard uses the `GLIBRE_DEFER` macro authored in
`core/include/glibre/defer.hpp` — the same macro
`fbx-importer-design.md` §10.3 / `texture-importer-design.md`
§10.3 introduce. This design **does not re-author** `defer.hpp`;
it depends on it. The guard body invokes `FT_Done_Face(face_)`
per §3.7 stage 10. Example usage in `import_one` (capturing
the FT_Face pointer by value for scope independence):

```cpp
auto _ft_cleanup = GLIBRE_DEFER(
    [face = face_]() noexcept {
        if (face) FT_Done_Face(face);
    }
);
```

The guard also clears the TLS slots (§3.5) so a spurious
allocator-hook call after `import_one` returns sees null
sentinels and short-circuits:

```cpp
auto _tls_cleanup = GLIBRE_DEFER(
    []() noexcept {
        tl_active_arena = nullptr;
        tl_active_impl  = nullptr;
    }
);
```

A third guard restores the `<cfenv>` rounding mode:

```cpp
auto _fenv_cleanup = GLIBRE_DEFER(
    [prior = std::fegetround()]() noexcept {
        std::fesetround(prior);
    }
);
```

`GLIBRE_DEFER` is preferred over a `std::function`-based guard
(PHILOSOPHY §11 bans `std::function` outside of EASTL; the
lambda + template parameter approach has zero overhead at the
call site).

### 10.4 Recovery posture

Per SPEC §10.4 recovery-posture map, every importer arm falls
into **Refuse**:

| Arm                        | Posture        | Mechanism                                              |
|----------------------------|----------------|--------------------------------------------------------|
| `SourceNotFound`           | Refuse         | Cook step rolls back; prior manifest stays active.    |
| `MagicMismatch`            | Refuse         | Same.                                                  |
| `UnsupportedVersion`       | Refuse         | Same. Operator re-exports from authoring tool with supported version / single-face / non-variable instance. |
| `MalformedPayload`         | Refuse         | Same. `error.detail` discriminates sub-cause.          |
| `Cancelled`                | Refuse (no-op) | No automatic retry; operator re-triggers.              |

There is **no Re-cook** posture from the importer side — re-cook
is the manifest-publish path's response to `MissingDependency`
(which the font importer does not emit; §7.4 — fonts are leaf
assets) or `HashNotInCas` (residency-side, not importer-side).
The importer is purely a producer of bytes that either succeed
or refuse with a typed arm.

### 10.5 What the importer does NOT emit

To make the boundary explicit:

- **`ImporterError::MissingDependency`** is emitted by the
  manifest-publish path (`cook/publish.cpp`, SPEC §6.2 manifest-
  publish paragraph) when the bottom-up dependency walk finds
  an edge to a child whose `AssetId` is not in the active
  manifest. The font importer surfaces no dependencies (§7.4);
  it never contributes to a missing-dependency event.
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

The importer's tests live under `tests/content/import/font/`.
Six classes of tests cover the §3 / §6 / §10 contracts; each
Catch2 case name below is the gate-asserted name.

### 11.1 Unit tests (Catch2)

**`content/import/font: dispatch_routes_font_to_freetype`**.
Asserts that
`dispatch_import(SourceAsset{kind=Font, format=Ttf})` and
`format=Otf` both route to `FreeTypeImporter::import_one`.
Exercises SPEC §4.1.2 inv #1 (closed-sum dispatch) and SPEC
§6.1 `importers/dispatch.cpp`.

**`content/import/font: rejects_path_outside_source_root`**.
Asserts that a font whose `SourceAsset.path` resolves outside
`assets/source/` returns `ImporterError::SourceNotFound`.
Exercises SPEC §4.1.1 inv #1.

**`content/import/font: rejects_unknown_magic`**. Asserts that
a non-font byte stream renamed `.ttf` returns
`ImporterError::MagicMismatch` with
`error.detail = "freetype-magic"`. Exercises §3.7 stage 2.

**`content/import/font: rejects_invalid_version`**. Asserts that
a hand-crafted SFNT with an invalid version prefix returns
`ImporterError::UnsupportedVersion` with
`error.detail = "freetype-version"`. Exercises §3.7 stage 2.

**`content/import/font: rejects_ttc_multiface`**. Asserts that
a TrueType Collection with two faces returns
`ImporterError::UnsupportedVersion` with
`error.detail = "freetype-ttc-multiface"` and
`error.num_faces = 2`. Exercises §3.6 + §3.7 stage 4.

**`content/import/font: rejects_variable_font`**. Asserts that
a variable font (e.g. an `fvar`-bearing font like Recursive)
returns `ImporterError::UnsupportedVersion` with
`error.detail = "freetype-variable-font"`. Exercises §3.6 + §3.7
stage 4.

**`content/import/font: rejects_codepoint_out_of_bmp`**.
Asserts that a `Custom` subset containing `U+1F600` (an emoji
codepoint above U+FFFF) returns
`ImporterError::MalformedPayload` with
`error.detail = "freetype-codepoint-out-of-bmp"` and
`error.codepoint = 0x1F600`. Exercises §3.4 + §3.7 stage 4.

**`content/import/font: rejects_corrupt_glyph_outline`**.
Hand-corrupts a `glyf` table entry to make `FT_Load_Glyph`
return `FT_Err_Invalid_Outline`; asserts arm +
`error.detail = "freetype-corruption"` plus `error.glyph_index`
set. Exercises §3.7 stage 5.

**`content/import/font: rejects_atlas_overflow`**.
Cooks a `BasicLatin` subset at an extreme `pixel_size = 256`
on a `atlas_max_dim = 1024` configuration so the packer cannot
fit; asserts arm + `error.detail = "freetype-glyph-atlas-overflow"`
plus `error.atlas_dim_required` set. Exercises §3.7 stage 7.

**`content/import/font: rejects_nan_metric`**. Hand-corrupts a
font's `head` table to a value that produces NaN in the f32
ascender; asserts arm + `error.detail = "freetype-nan-metric"`
plus `error.glyph_index` / `error.metric_field` set. Exercises
§3.7 stage 8.

**`content/import/font: cancellation_observed_within_10ms`**.
Drives a `BasicLatin` SDF bake through `import_one` while a
sibling thread calls `cancel()` mid-glyph-loop; asserts return
value is `ImporterError::Cancelled` within 10 ms wall-clock.
Exercises SPEC §4.1.2 inv #5 and §6.4.

**`content/import/font: produces_byte_equal_precursor_across_runs`**.
Cooks the same font bytes twice in two fresh cook tool
processes; asserts the returned precursor bytes are byte-equal.
Exercises PHILOSOPHY §7 + SPEC §4.2 inv #1 + §3.7 stage 7
(determinism canonicalization). One case per format
(`ttf_byte_equal`, `otf_byte_equal`).

**`content/import/font: importer_version_changes_on_freetype_bump`**.
Asserts that `Importer::version()` returns a different digest
when the build is configured with a different `FREETYPE_TAG`
or any of `NORMALIZE_TAG` / `ATLAS_TAG` components. Exercises §3.2.

**`content/import/font: scratch_arena_residency_under_64mib_on_basiclatin_sdf`**.
Cooks the BasicLatin SDF reference fixture at 48 px / 2048×2048
and asserts arena peak residency ≤ 64 MiB. Exercises SPEC §9.3.1
+ §9.2 above.

**`content/import/font: glyph_metrics_match_freetype_reference`**.
Cooks a hand-authored TTF with known per-glyph advance + bearing
+ bbox values; asserts the precursor's `GlyphRecord` rows match
the reference values within a tolerance of `1.0/64.0` (one 26.6
fixed-point unit converted to f32 pixels). Exercises §3.4 + §3.7
stage 5 metric extraction.

**`content/import/font: sdf_atlas_distance_field_matches_reference`**.
Cooks a BasicLatin SDF fixture at a fixed reference configuration;
asserts the resulting atlas pixel buffer matches a known-good
reference (cooked once on a reference machine, checked into the
test fixtures). Exercises §3.5 SDF render-mode dispatch + §3.7
stage 7 atlas pack.

**`content/import/font: bitmap_atlas_pixel_format_matches_reference`**.
Same as above but with `render_mode = Bitmap`; asserts the atlas
bytes are the FreeType `FT_PIXEL_MODE_GRAY` raster output
verbatim. Exercises §3.5 row 1.

**`content/import/font: kern_table_pairs_extracted_when_present`**.
Cooks a font that ships a non-empty `kern` table (e.g. an old
Helvetica variant); asserts `kerning_pairs_` is non-empty and
the (lhs, rhs, delta) tuples match `FT_Get_Kerning` output for
the loaded subset. Exercises §3.4 + §3.7 stage 6.

**`content/import/font: kerning_disabled_when_include_kerning_false`**.
Same fixture; asserts `kerning_pairs_` is empty when
`NormalizeParams.include_kerning = false`. Exercises §3.4.

**`content/import/font: codepoint_map_sorted_ascending`**.
Cooks a `BmpAscii` subset; asserts the `codepoint_map_` is
sorted strictly ascending by codepoint and contains exactly the
expected codepoints. Exercises §3.7 stage 8.

**`content/import/font: glyph_subset_basic_latin_count_95`**.
Cooks a `BasicLatin` subset; asserts exactly 95 codepoint-map
entries (96 if a font does not provide a glyph for one of
U+0020..U+007E, in which case that codepoint maps to glyph 0
but the entry still appears). The exact number depends on
fixture font's coverage; the test checks invariant per the §3.4
table. Exercises §3.4 / §3.6.

**`content/import/font: per_thread_freetype_no_shared_state`**.
Spawns 4 worker threads each running `import_one` on the same
font fixture; asserts every worker's precursor is byte-equal
and no data-race / double-allocation triggers TSAN / ASAN.
Exercises §3.5 + §6.2.

**`content/import/font: ft_memory_hook_routes_to_arena`**.
Verifies via instrumented allocator that every byte FreeType
allocates during `import_one` is tagged `ContextTag::content`
and lands in the per-cook arena (no default heap allocations).
Exercises §3.5 + §6.3.

### 11.2 FreeType error translation tests

**`content/import/font: translates_unknown_file_format_to_magic`**.
Hand-corrupts a TTF magic prefix; asserts arm +
`error.detail = "freetype-magic"` plus `error.codec = "freetype"`.
Exercises §10.2 row 1.

**`content/import/font: translates_invalid_version_to_unsupported`**.
Hand-crafts an SFNT with an invalid version field; asserts arm +
`error.detail = "freetype-version"`. Exercises §10.2 row 2.

**`content/import/font: translates_corruption_to_malformed`**.
Truncates a TTF mid-`glyf`-table; asserts arm + `error.detail =
"freetype-corruption"`. Exercises §10.2 rows 3–5.

**`content/import/font: translates_alloc_exhausted_to_malformed`**.
Forces FreeType allocation refusal by setting the arena ceiling
to 1 KiB and cooking a typical font fixture; asserts arm +
`error.detail = "freetype-alloc-exhausted"`. Exercises §10.2
row 6.

**`content/import/font: translates_invalid_pixel_size_to_malformed`**.
Cooks a bitmap-only font with `pixel_size = 999`; asserts arm +
`error.detail = "freetype-invalid-pixel-size"`. Exercises §10.2
row 7.

**`content/import/font: terminates_on_bad_alloc`**.
Forces a `bad_alloc` via a fixture-injected hook (rare path —
the FT_Memory hook usually intercepts arena exhaustion before
`bad_alloc` can fire); asserts process termination via
`std::terminate`. Exercises §10.2 row 13 + SPEC §10.7 OQ-2.

**`content/import/font: catches_unclassified_std_exception`**.
Throws a custom `std::runtime_error` from a fixture-injected
FreeType callback hook; asserts arm + `error.detail =
"freetype-unclassified"`. Exercises §10.2 row 14.

**`content/import/font: terminates_on_non_std_exception`**.
Throws an `int` from a fixture-injected callback; asserts
process termination. Exercises §10.2 row 15.

### 11.3 Format-coverage tests

The two admitted formats each get a round-trip test that asserts
the canonical metrics + atlas layout match the §3.5 / §3.7
specification for a known fixture font.

**`content/import/font: ttf_basic_latin_sdf_canonical_layout`**.
Cooks a TTF (e.g. DejaVu Sans) with `BasicLatin` SDF at 48 px /
2048×2048; asserts metadata fields match the source's authored
values, `glyph_count` matches the subset, `atlas.pixel_format =
GraySdf`, and `atlas.width / height` are powers-of-two ≤ 2048.

**`content/import/font: otf_basic_latin_sdf_canonical_layout`**.
Same shape with an OTF source (CFF outlines) — asserts the CFF
parser produces canonically-equivalent metrics + atlas to the
TTF analog above.

**`content/import/font: ttf_basic_latin_bitmap_canonical_layout`**.
Cooks the TTF in `Bitmap` mode; asserts `atlas.pixel_format =
GrayBitmap` and the atlas pixel value distribution matches
FreeType's `FT_PIXEL_MODE_GRAY` reference (8-bit alpha-coverage).

**`content/import/font: ttf_latin_plus_latin1_canonical_layout`**.
Cooks the TTF with `glyph_subset = LatinPlusLatin1`; asserts
the codepoint count is the expected ~191 codepoints (BasicLatin
+ Latin-1 supplement) and every codepoint resolves to a non-zero
glyph index for a typical Latin font.

**`content/import/font: ttf_custom_subset_canonical_layout`**.
Cooks the TTF with `glyph_subset = Custom` and a hand-authored
codepoint list; asserts the `codepoint_map_` matches the input
list exactly and the atlas contains exactly those glyphs (plus
`.notdef`).

### 11.4 Integration tests with cook session

**`content/cook: font_import_then_cas_write_then_manifest_publish`**.
End-to-end: cook one TTF through `CookSession::commit()`;
asserts (a) cook returns `CookOutcome::Published`, (b) one
CAS file appears under `cooked/<prefix>/<hash>`, (c) the
manifest entry resolves the asset's `AssetId` to that hash,
(d) the manifest emits zero `DependencyEdge`s for the font
(fonts are leaf assets per §7.4). Exercises SPEC §6.2 stages
1–6 with the font importer in the dispatch slot. Cross-link:
`cas-store-design.md` (#813) will pin the CAS-side details
once its design lands.

**`content/cook: font_import_rolls_back_on_corruption`**.
Cooks a corrupt TTF; asserts (a) cook returns
`CookOutcome::RolledBack`, (b) no CAS file is published (the
temp file may exist but the final path does not), (c) the
prior manifest snapshot remains active. Exercises SPEC §4.1.9
inv #1 + §10.4 + §10.1 `MalformedPayload`.

### 11.5 Determinism gates (PHILOSOPHY §7)

**`content/import/font: byte_equal_across_macos_and_linux_ci`**
*(deferred — conditional on prerequisites)*. Same font cooked
on macOS and Linux CI; asserts byte-equal precursor bytes
(Linux CI uses FreeType linked against the same vcpkg-pinned
version). Deferred until the project's Linux CI runner is
provisioned (matching `fbx-importer-design.md` §11.4 /
`texture-importer-design.md` §11.5 which have the same
prerequisite). Skipped in CI until then; replaced by
`byte_equal_across_two_runs_same_host`.

**`content/import/font: byte_equal_across_two_runs_same_host`**.
Same as the unit case `produces_byte_equal_precursor_across_runs`,
elevated to the determinism gate so a regression flips the
`perf:headroom-low`-equivalent label per `perf-budget.md`
§"CI Gate Spec" rule 5.

### 11.6 Off-thread soundness

**`content/import: s3_off_thread_no_driver_spike`** (SPEC §9.5
end-to-end gate, restated here). The integration gate that
proves the importer does not bleed onto the driver thread;
exercised by running 600 frames of S1 with an active BasicLatin
SDF font cook on the worker pool. PR fails if driver-thread p99
phase-1 + phase-9 cost exceeds 0.50 ms. Shared with
`fbx-importer-design.md` §11.5 / `texture-importer-design.md`
§11.6 — all three importers feed the same end-to-end gate; the
test fixture exercises whichever importer the workload triggers.

This is the load-bearing gate against the §9.5 driver-thread
invariant.

## 12. Open questions

Three open questions remain at this design pass; each names
the gate that re-opens it. None block this design's PR.

**OQ-1. `NormalizeParams` typed view in §5.4 header** — Same
as `fbx-importer-design.md` §12 OQ-1 / `texture-importer-design.md`
§12 OQ-1, applied to the font importer. The §3.4 vocabulary
defines a typed schema the canonical-bytes encode; the §5.4
header currently exposes only the opaque `canonical_bytes` span.
A typed `NormalizeParamsView` adaptor would give callers (the
cook session, future tooling) a checked decode path without
re-implementing the canonical reader. The adaptor's body lives
in the codegen output (`fory-codegen.md`); exposing it through
the §5.4 header pulls a Fory-codegen dependency into a header
consumed by the runtime. Resolution trigger: a second consumer
needs typed access (e.g. an editor's "per-cook params
inspector"). Until then, the cook session decodes the bytes
through the codegen header directly, and the §5.4 header stays
Fory-free. The font importer's resolution is bundled with the
FBX / texture importers' identical OQ. **Owner**: planning
spike for editor content-pipeline UI; **gate**:
`task-breakdown-content-font-importer-detailed` follow-up does
not author the typed view.

**OQ-2. Multi-size atlas / runtime size variance** — MVP refuses
per §3.4 (single `pixel_size`) / §3.6. Re-entry path: a future
spike admits multiple `pixel_size` values per font cook, either
as multiple atlases per cooked font (the precursor's `atlas`
field becoming a `list<atlas>` with a parallel `pixel_sizes`
list) or as one atlas containing multiple sizes (negating the
§1 single-size invariant and requiring a non-uniform-glyph
packer). The current refusal commits to **fixed-size cooking**
at MVP because `render`'s SDF text pipeline upscales/downscales
from a single size, which is the harmonius R-10.4.2 use-case
most easily met from a single cooked atlas. Re-opening this OQ
is the trigger to revisit that boundary. The path does not
require a second `Importer` aggregate — it extends this one
under a `vocab_version` bump. **Owner**: future UI plugin's
text-rendering epic; **gate**: that epic's landing.

**OQ-3. MSDF / multi-channel SDF support** — MVP refuses per
§3.4 (`render_mode ∈ {Bitmap, Sdf}`). MSDF (multi-channel SDF;
sharper corners than single-channel SDF, used by harmonius
R-10.4.2's "MSDF textures with subpixel positioning") is not in
FreeType's built-in module set; supporting it would require
linking either the `msdfgen` library or implementing an
RGB-channel SDF emitter in-house. The path does not require a
second `Importer` aggregate — it extends this one under a
`vocab_version` bump and a new `render_mode = Msdf` enumerator,
with the linked third-party library version contributing to
`importer_version` as a new tag. The current refusal commits to
**single-channel SDF** at MVP because FreeType ships SDF
out-of-the-box and msdfgen is a non-trivial linkage decision
(LGPL licensing, no vcpkg port at this spike's time). **Owner**:
render plugin's text-rendering epic; **gate**: a render-side
requirement that demands MSDF specifically (not just SDF).

**OQ-4. WOFF / WOFF2 / variable fonts / TTC support** — MVP
refuses per §1 / §3.4 / §3.6. Re-entry path: post-MVP
configuration of FreeType with the corresponding feature gates
(Brotli for WOFF2, fvar/MM module for variable fonts, TTC face
selection via `face_index`); each format addition is a
deliberate central edit per §3.4 vocabulary bump. The path does
not require a second `Importer` aggregate — it extends this one
under a `vocab_version` bump. **Owner**: future UI / asset-
import-coverage epic; **gate**: that epic's landing.

These four OQs do **not** become spec residue — they are
deliberate post-MVP routing per PHILOSOPHY §5 (greatly reduced
MVP scope) + §10 (Occam — collapse in MVP, expand only on a
second consumer). Each carries its own re-opening gate; none
of them are content's residue (§12 SPEC), they are this
aggregate's deferred work routed to its owning consumer.
