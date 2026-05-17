# 60 — Content Pipeline

Cites the thesis in [`00-vision.md`](00-vision.md), the contexts in
[`10-bounded-contexts.md`](10-bounded-contexts.md), the integration shape in
[`15-context-map.md`](15-context-map.md), the substrate stances in
[`20-platform-strategy.md`](20-platform-strategy.md), the authoring philosophy in
[`30-authoring-and-no-code.md`](30-authoring-and-no-code.md), and the runtime architecture in
[`40-runtime-architecture.md`](40-runtime-architecture.md).

This pillar pins how authored content moves from a source the maker dragged into the project to a
baked artifact the running runtime consumes — and how live edits propagate back into a running
editor session. It covers the source-of-truth model, the bake contract, hot-reload semantics,
importer extensibility, version-control posture, and the boundary between pipeline staging and
runtime loading. No file formats, no library names, no on-disk layouts.

## How to read this pillar

Decisions are presented as **stances** with *committed* / *deferred* / *anticipated* tags, in the
same shape as [`40-runtime-architecture.md`](40-runtime-architecture.md). Failure modes are named
per Murphy.

## Source asset and baked artifact are different things

**Committed.**

A **source asset** is the human-edited input — the file produced by an external tool, the state the
maker can re-edit later. A **baked artifact** is the runtime-ready output of processing that source
for a specific target platform (per [`10-bounded-contexts.md`](10-bounded-contexts.md)). The two are
never confused: the source is in the project; the artifact is in the build cache.

**Why.** A maker who edits an asset wants to keep editing the same way they were before; they should
never be asked to "open the baked version." A runtime that consumes assets wants something fast to
load and right-sized for the target platform; it should never be asked to interpret an artist's
authoring-format quirks. Keeping the two strictly separated gives each side the form it actually
needs.

**Failure mode absorbed.** Bake outputs do not contaminate the project the maker is editing; project
edits do not corrupt artifacts the runtime depends on. A bake-cache deletion is recoverable
(rebuild); a source-asset deletion is not (lost work). The asymmetry of the two failure modes is
honored by the two stores.

## The Asset DB owns identity

**Committed.**

Every source asset has a stable identity managed by the **Asset DB** (per
[`10-bounded-contexts.md`](10-bounded-contexts.md)). References between assets, between scenes and
assets, and between graphs and assets resolve through the Asset DB; nothing in the project
references an asset by filesystem path.

**Why.** Filesystem paths are not identity: a maker who renames a folder breaks hard-coded paths but
should not break references. The Asset DB's identity model decouples "what an asset is" from "where
the file currently sits."

**Anticipated.** The shape of identity (numeric, hash-derived, content-addressable, maker-chosen) is
tactical; this pillar commits to *having an identity layer*, not to its form.

**Failure mode absorbed.** A reorganized project does not propagate breakage; a moved asset is still
itself.

## Bake is build-time, never runtime

**Committed.**

The transformation from source asset to baked artifact happens at build time, in the content
pipeline. The shipped runtime never invokes an importer or a bake step. During an authoring session
the *editor* re-bakes on source changes — the editor is a distinct OS process from the shipped
runtime (per [`40-runtime-architecture.md`](40-runtime-architecture.md)), so a re-bake there is not
an exception to the runtime's bake-free contract; it is a different process entirely. The re-baked
artifact reaches the in-editor play session through the hot-reload contract in `40` (S-4 and S-15 in
[`15-context-map.md`](15-context-map.md)).

**Why.** Importers and bakers are heavy: they call out to platform-native compilers (shaders), they
decompress and recompress textures, they convert mesh topologies. Doing this at runtime would bloat
the shipped runtime with toolchain dependencies and would tax every game startup. Doing it at build
time pays the cost once.

**Failure mode absorbed.** A shipped game does not need a shader compiler, an importer plugin, or a
build-pipeline configuration on the player's machine.

## The bake is reproducible and per-target

**Committed.**

A bake is a pure function of: source asset, importer settings, target platform, and the versions of
the importer and the relevant tools. Given the same inputs *and a deterministic tool configuration*,
the bake produces the same artifact. Different target platforms bake to different artifacts from the
same source.

**Why.** Reproducibility is what makes the bake cache trustworthy and what makes the bake diff-able
across versions of the engine. Per-target bake is what makes glibre's "authored once, ships
everywhere" promise actually true: the maker doesn't write a per-platform material variant; the bake
produces them.

**Anticipated.** Cross-platform determinism in the *bake step itself* (not just the output) is
desirable but not committed; some tools used in the bake are non-deterministic in subtle ways. The
runtime determinism stance in [`40-runtime-architecture.md`](40-runtime-architecture.md) is a
separate question.

**Failure mode absorbed.** Given the same inputs and a deterministic tool configuration, the same
artifact is produced. Establishing the deterministic configuration in practice (across CI
environments, across maker machines) is contingent on the tool-nondeterminism enforcement tracked in
[`90-risks-and-open-questions.md`](90-risks-and-open-questions.md); the stance commits to the
property, not yet to the operational answer.

## Graph artifacts share the bake pipeline

**Committed.**

Composition's lowering — graph → executable artifact, per
[`20-platform-strategy.md`](20-platform-strategy.md) — runs as a bake step inside this pipeline,
alongside texture compression, shader compilation, and mesh staging. Composition owns the
*lowering contract* (the transformation from graph to executable artifact; the toolchain and
intermediate representation are tactical details downstream of that contract, per
[`10-bounded-contexts.md`](10-bounded-contexts.md)). The content pipeline owns the *coordination* of
that lowering: invocation, caching, per-target bake, hot-reload event emission. The artifact
Composition produces — which satisfies the contract it publishes toward Runtime at S-2 in
[`15-context-map.md`](15-context-map.md) — is what the pipeline receives; what the pipeline stages
for Runtime, Rendering, or Distribution flows out through S-4, S-5, S-12, and S-13.

**Why.** Graph artifacts are first-class baked output. Treating them as parallel-to or outside the
pipeline would mean two separate caching, hot-reload, and CI-integration stories — one for graphs,
one for everything else — which contradicts the single-substrate spirit of
[`30-authoring-and-no-code.md`](30-authoring-and-no-code.md) and the content-context responsibility
list in [`10-bounded-contexts.md`](10-bounded-contexts.md).

**Failure mode absorbed.** Graph hot-reload, graph bake caching, graph dependency invalidation, and
graph CI integration share the same infrastructure as every other baked asset; the engine does not
maintain two pipelines for the same job.

## Hot-reload at edit time, snapshot-boundary at runtime

**Committed.**

When a source asset changes during an editor session, Content re-bakes the affected artifacts and
emits a hot-reload event. Three consumers receive it (seams from
[`15-context-map.md`](15-context-map.md)):

- **Authoring** (S-14) refreshes any open inspectors, asset browsers, and editing
  surfaces.
- **Rendering** (S-5) re-establishes GPU state from the new render artifact when a
  material, shader, or texture re-bake completes.
- **The in-editor play session's runtime** (S-4) applies the new asset artifact at a
  snapshot boundary, never mid-tick, per
  [`40-runtime-architecture.md`](40-runtime-architecture.md).
- **The runtime, via the graph hot-reload channel** (S-2), receives a freshly lowered
  graph artifact when a re-bake of a graph completes. This is the Graph hot-reload
  channel committed in [`40-runtime-architecture.md`](40-runtime-architecture.md);
  per the "Graph artifacts share the bake pipeline" stance above, the pipeline emits
  the hot-reload event the same way it does for asset re-bakes.

**Why.** The four consumers have different correctness contracts. Authoring wants to show the maker
the newest information as soon as it's available; Rendering wants to re-establish its GPU state
without tearing the frame; the runtime wants to swap asset artifacts without tearing the simulation;
the graph channel wants to retire stale executable code only when no caller is active. Splitting the
seams is what makes all four correct.

**Deferred.** What happens to in-flight references held by other systems when an artifact is
hot-swapped — animation state pointing at a re-baked skeleton, audio mixer pointing at a re-baked
clip — is a per-asset-kind concern and is open in
[`90-risks-and-open-questions.md`](90-risks-and-open-questions.md).

**Failure mode absorbed.** The maker doesn't have to choose between an up-to-date editor view and a
stable running session: both are kept consistent, on their own terms.

## Importers are extensions, not built-ins

**Committed.**

The engine ships with importers for the formats committed scopes require, but the *mechanism* by
which an importer exists is the same whether the importer is built-in or contributed by a community
extension. There is no "first-class formats" tier that third-party importers can't reach.

**Why.** A first-class/second-class importer split would tell the maker "we trust the engine team's
importers more than yours," which contradicts the maker-ergonomics-dominate stance (per
[`30-authoring-and-no-code.md`](30-authoring-and-no-code.md)). It would also give the engine a
backdoor for tactical shortcuts that community importers can't use, which calcifies the engine.

**Anticipated.** The packaging and distribution of importer extensions are owned by
[`80-distribution-and-extensibility.md`](80-distribution-and-extensibility.md).

**Failure mode absorbed.** New source formats are not engine-team gating questions. Anyone can write
an importer; everyone uses the same hosting mechanism.

## Source assets live in version control; bake artifacts do not

**Committed.**

The project's source assets are intended to be checked into version control alongside the project's
graphs, scenes, and configuration. Baked artifacts are derived data; they live in a build cache that
is regenerable and is not version-controlled.

**Why.** Two operationally important properties: (a) a fresh clone of a project produces a buildable
workspace without committing megabytes of derived bytes, and (b) the bake cache can be cleared
without losing maker work. Mixing the two violates both properties.

**Deferred.** Whether very large source binaries (large textures, long audio, dense meshes) ship
through the same version-control mechanism as graphs and scenes, or through a side-channel optimized
for binary data, is a tactical decision that depends on committed-scope project sizes; tracked in
[`90-risks-and-open-questions.md`](90-risks-and-open-questions.md).

**Failure mode absorbed.** Cloning a project is a known, fast, repeatable act. Losing the bake cache
is a recoverable inconvenience, never lost work.

## Streaming and loading are runtime concerns; staging is the pipeline's

**Committed shape; specific mechanics deferred.**

What the runtime loads at startup, what it streams later, and what it discards under memory pressure
are Runtime's decisions (per [`10-bounded-contexts.md`](10-bounded-contexts.md) and
[`40-runtime-architecture.md`](40-runtime-architecture.md)). The content pipeline's responsibility
is to *stage* artifacts in a form that enables those decisions: chunking that allows partial loads,
dependency metadata that allows correct prefetch, size information that allows budgeting.

**Why.** A pipeline that pre-decides how the runtime should load is a pipeline that constrains every
future runtime change. Keeping the pipeline focused on staging and the runtime focused on loading
lets each evolve independently.

**Failure mode absorbed.** A new loading strategy in the runtime (background streaming, on-demand
loading, mip-streaming) does not require a pipeline change; the staging shape is sufficient.

## What this pillar deliberately does not decide

- **Source formats.** Which specific source formats the engine accepts is determined by
  the committed scope that needs them. The pipeline supports any format an importer
  exists for; the list is not a strategic commitment.
- **Artifact formats.** The on-disk shape of baked artifacts is tactical and owned by
  per-asset-kind design.
- **Cache layout and invalidation.** Tactical; the pipeline's *behavior under invalidation*
  is strategic, the storage form is not.
- **Version-control vendor.** The pipeline assumes a generic version-control system; it
  does not name one. Recommended setups are out of scope.
- **Asset DB on-disk representation.** Identity is the strategic commitment; how the DB
  is stored is tactical.
- **Build/CI orchestration.** Owned by
  [`80-distribution-and-extensibility.md`](80-distribution-and-extensibility.md).

## Risks deferred to `90`

- **Large-binary version control.** Source meshes, textures, and audio can be enormous;
  conventional version control struggles with them. The pipeline's assumption that
  sources live in VC will hit a wall on real projects without a side-channel.
- **Hot-reload of long-lived references.** Animation state, audio buffers, GPU resource
  handles — anything that holds onto an artifact past the next snapshot boundary — needs
  a per-kind hot-swap design. The contract is named in `40`; the per-kind designs are
  open.
- **Bake nondeterminism.** Some tools used in the bake (shader compilers, texture
  encoders) have nondeterminism modes that can produce subtly different output run to
  run. Hitting the reproducibility commitment requires choosing deterministic settings
  consistently, which is an enforcement concern, not a design one.
- **Importer extension surface.** A growing third-party importer ecosystem creates a wide
  surface for project-state assumptions to leak in. Versioning the importer interface
  is a concern that grows with adoption.
- **Asset identity persistence across renames and merges.** Maintaining identity through
  refactoring of a project's filesystem layout is straightforward; maintaining it across
  branch merges where two makers have renamed the same asset differently is not. Tracked
  in the deferred set.
- **CI/no-display bake.** Per the platform-strategy risk register (per
  [`20-platform-strategy.md`](20-platform-strategy.md)), shader and graph compilation
  can fail under sandboxed CI. The pipeline's posture on headless CI is the
  authoritative place to resolve that risk; this pillar names it, does not solve it.
