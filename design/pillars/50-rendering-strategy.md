# 50 — Rendering Strategy

Cites the thesis in [`00-vision.md`](00-vision.md), the contexts in
[`10-bounded-contexts.md`](10-bounded-contexts.md), the integration shape in
[`15-context-map.md`](15-context-map.md), the substrate stances in
[`20-platform-strategy.md`](20-platform-strategy.md), the authoring philosophy in
[`30-authoring-and-no-code.md`](30-authoring-and-no-code.md), the runtime architecture in
[`40-runtime-architecture.md`](40-runtime-architecture.md), and the content pipeline in
[`60-content-pipeline.md`](60-content-pipeline.md).

This pillar pins the rendering posture: the abstraction shape that sits above the three native
interop seams committed in `20`, the shader pipeline shape, the 2D-and-3D unification stance, the
scalability tier model, and the relationship between the renderer the player sees and the renderer
the maker previews in the editor. No GPU-API names beyond the platform-API-tier, no render-pass
enumerations, no specific feature lists.

## How to read this pillar

Decisions are presented as **stances** with *committed* / *deferred* / *anticipated* tags. Failure
modes are named per Murphy. Capability inventories (which post-processing effects, which lighting
models, which material features) are tactical and live in a downstream design owned by the Rendering
context.

## The portable layer is above the seams, not below them

**Committed.**

Glibre's renderer is a portable layer that sits *above* the three native interop seams committed in
[`20-platform-strategy.md`](20-platform-strategy.md). It does not translate draws into a synthetic
API and then have each backend adapt; it lets each platform's native graphics API speak its own
idiom while presenting a coherent rendering model at the layer above.

**Why.** A portable layer *below* the seams (the "abstract over the three native seams with one API"
model) costs a translation tax on every frame on every platform and forces every platform to support
the union of the abstraction's features. A portable layer *above* the seams keeps the per-platform
code idiomatic to that platform's API and isolates the abstraction to the parts that genuinely need
to be portable — material authoring, render-graph composition, asset format. This honors the
three-seam decision in `20` rather than fighting it.

**Failure mode absorbed.** Platform-native graphics features (the things a single platform can do
well that the others cannot) do not require a generic abstraction to be retrofitted before the
engine can use them. The portable layer is not the bottleneck for using a platform.

## One shader language at authoring time, per-platform artifacts at runtime

**Committed.**

Material and shader authoring at the contributor tier is in a single shader language (per
[`20-platform-strategy.md`](20-platform-strategy.md)'s "one shader language for all platforms"
stance). The shader pipeline compiles that source into per-platform shader artifacts at build time,
through the content pipeline (per [`60-content-pipeline.md`](60-content-pipeline.md)). The runtime
ships those artifacts and does not invoke a shader compiler.

**Why.** This is the only way to honor the no-code vision in [`00-vision.md`](00-vision.md) for the
rendering surface: the maker authors materials visually and never sees a shader language, while the
engine takes responsibility for producing the right artifact for each platform. The build-time bake
also keeps the shipped runtime free of toolchain dependencies (per
[`20-platform-strategy.md`](20-platform-strategy.md)).

**Deferred.** Whether the editor *itself* needs a runtime shader compiler for live shader editing
inside Authoring (per [`20-platform-strategy.md`](20-platform-strategy.md)'s observation that this
is the only candidate use case) is a maker-experience trade-off owned by the Authoring downstream
design and will be tracked in `90-risks-and-open-questions.md` when that file is established.

**Failure mode absorbed.** Adding a new platform target adds compiler-target work in the shader
pipeline; it does not multiply the maker's shader-authoring surface. The chosen shader language is
an external project with its own release cadence; a version-churn exposure (a breaking change in
that language) is an engine-level concern not borne by the maker, and is named again in the risks
section below.

## Materials are authored, shaders are baked

**Committed.**

The maker authors **materials**: parameter sets, node graphs, and references that the engine
combines with mesh, lighting, and post-processing state to produce a shaded surface. The maker does
not author shader source directly; shader source is what the material bake produces inside the
content pipeline, and shader artifacts are what the runtime loads.

**Why.** A maker who authors materials can compose visually (per
[`30-authoring-and-no-code.md`](30-authoring-and-no-code.md)); a maker who authors shader source is,
by definition, not a no-code authoring surface. The split keeps material authoring inside the
Composition substrate (per [`10-bounded-contexts.md`](10-bounded-contexts.md)) and keeps shader
source as an internal pipeline concern.

**Anticipated.** A contributor or advanced extension may author shader source for a custom material
node library; that is a contributor-tier authoring path, not a maker-tier one. The boundary is the
same boundary the vision draws around code-writing in general (per [`00-vision.md`](00-vision.md)).

**Failure mode absorbed.** A maker is never told "and here is the part where you write a shader."
The boundary between maker authoring and engine-side compilation is enforced at the editor surface.

## 2D and 3D share one renderer

**Committed.**

The renderer is one engine, used for both 2D and 3D rendering, with vector and sprite rendering as
maker-selectable modes inside it rather than as a parallel system. There is no "2D engine" alongside
a "3D engine"; there is one renderer that the maker drives with different primitives.

**Why.** Two renderers would mean two material systems, two shader pipelines, two camera models, two
debugging surfaces, and a maker who knows where the boundary sits. One renderer with authored 2D
modes keeps the engine's tooling, debugger, and material system coherent and lets the maker mix 2D
and 3D content in a scene without negotiating a system boundary.

**Anticipated.** Specific 2D capabilities (vector graphics, sprite atlases, 2D skeletal animation
surfaces) are downstream design owned by the Rendering context, scoped to what a committed scope
claims.

**Failure mode absorbed.** A 2D-leaning project does not pay a 3D abstraction cost it does not use.
A 3D-leaning project is never locked out of cheap 2D overlays by a system boundary.

## Rendering scales by *tier*, not by *feature*

**Committed.**

The renderer ships a small set of **scalability tiers** (low / mid / high as the working baseline;
the tier model requires at least two distinct tiers for the scalability commitment to be
non-trivial). A tier is a coherent rendering posture — lighting model, post-processing posture,
geometric complexity budget — that a project selects per build target. Within a tier, individual
features either work or they don't; makers do not assemble a tier from a feature checklist.

**Why.** A feature-checklist model lets a project ship a combination the engine has never tested. A
tier model bounds the testing matrix: the engine commits to each tier working coherently on every
committed platform.

**Anticipated.** The exact tier definitions, the feature inventory per tier, and the per-platform
tier-to-hardware mapping are tactical and owned by the Rendering context's downstream design. The
*stance* (tier model, not feature checkboxes) is committed here.

**Anticipated.** Auto-selection of a default tier from runtime hardware probe is a deferred decision
for the runtime; the renderer commits only to *honoring* a selected tier, not to selecting one.

**Failure mode absorbed.** A maker doesn't ship a combination of features the engine has never seen
working together. The engine doesn't ship a combinatorial test matrix.

## The editor and runtime share one renderer

**Committed.**

The renderer that draws the in-editor viewport is the *same* renderer that draws the shipped game.
The viewport is a hosted surface, arranged per-platform by Platform (per
[`40-runtime-architecture.md`](40-runtime-architecture.md)); the rendering code inside it is the
runtime's rendering code. The frame the renderer composes is fed by the read-only world-state
snapshot Runtime publishes at S-7, exactly as it is for a shipped game.

**Why.** Two renderers — a "preview renderer" inside the editor and a "shipped renderer" in the
runtime — would let the maker's editor experience drift from the player's experience. The vision in
[`00-vision.md`](00-vision.md) refuses that drift; "fidelity matching what the shipped runtime
produces" is a vision commitment, and the cheapest way to honor it is to use one renderer.

**Committed.** Editor-only overlays (gizmos, selection highlights, debug visualizers) live in the
*editor* process, not in the runtime process — the editor and runtime are distinct OS processes per
[`40-runtime-architecture.md`](40-runtime-architecture.md), so the runtime renderer cannot draw an
editor overlay even if asked. The editor composites overlays on top of the runtime's viewport
output. The boundary is therefore structural (the runtime has no source for editor overlays), not
just a discipline.

**Failure mode absorbed.** A bug in the renderer that shows up in a shipped game also shows up in
the editor; the maker hits it during authoring, not after release.

## Render artifacts are per-target, baked, and reproducible

**Committed.**

Render artifacts — compiled shaders, per-platform pipeline configurations, baked render data — are
produced by the content pipeline per the reproducibility commitment in
[`60-content-pipeline.md`](60-content-pipeline.md) and reach Rendering through S-5. The runtime
loads them; it does not synthesize them at startup.

**Why.** Compiling shaders on the player's machine costs first-launch time, requires a compiler on
the target, and produces nondeterministic results across hardware. Baking at build time pays the
cost once, in a controlled environment, with the reproducibility contract of `60`.

**Anticipated.** A shader artifact compiled for one platform's API is not portable to another; the
per-target bake (per `60`) is what makes "authored once, ships everywhere" true for shaders.

**Failure mode absorbed.** The player does not wait on a first-launch shader compile; the maker does
not see different shader behavior across CI and developer builds because of compiler nondeterminism.

## The render-graph composition is portable

**Committed.**

How a frame is composed — the ordering of passes, the resource lifetimes, the read/write
declarations between passes — is portable. The per-platform native API executes that composition;
the composition itself is one authored shape.

**Why.** The composition is where the renderer's design budget concentrates: it is the part of
rendering most likely to be revised as scopes commit (new lighting models, new post-processing
features). Keeping the composition portable means revisions land in one place, not three.

**Anticipated.** The composition's exact form — declarative graph, command-list builder, or some
hybrid — is tactical and owned by Rendering's downstream design.

**Failure mode absorbed.** A renderer change does not require three platform-specific edits to take
effect; the platform-specific code is the *execution* of the composition, not the composition
itself.

## Rendering is credible and modern, not the differentiator

**Committed.**

Per [`00-vision.md`](00-vision.md), rendering must be credible and modern; it is not the engine's
differentiator. The renderer ships at the bar a maker shipping a real, modern game expects to find
on a modern engine — and stops short of investing the engine's design budget on rendering features
that are not load-bearing for the no-code thesis. What that bar contains (specific lighting,
shading, post-processing capabilities) is the Rendering context's downstream capability inventory,
scoped per committed scope.

**What this rules out.** Bespoke rendering features designed to win benchmarks or to match the
marketing of competing engines. If a feature is shipped, it is shipped because a committed scope
needs it, not because rendering is the thing this engine wants to be famous for.

**Failure mode absorbed.** The engine does not over-invest in rendering at the cost of the
differentiator (the no-code path). Conversely, the engine does not under-invest in rendering to the
point that a real maker cannot ship a credible-looking game.

## What this pillar deliberately does not decide

- **The specific lighting model, post-processing inventory, or shading techniques.**
  Tactical; owned by Rendering's downstream design, scoped per committed scope.
- **The render-graph form** (declarative vs imperative, immediate vs deferred). Tactical.
- **The exact tier definitions.** Strategy commits to the tier model; the contents are
  tactical.
- **Specific GPU-API features used per seam.** Owned by Platform.
- **The shader-language feature set the maker sees.** Owned by Composition's material
  node library and the shader language's own version policy.
- **Editor overlay specifics** (gizmo style, selection visualization, debug
  visualizers). Tactical and owned by the editor downstream design.

## Risks deferred to `90`

- **Three idiomatic backends multiplies maintenance.** The portable-layer-above-the-seams
  stance is the right trade for the engine's vision but means every renderer feature
  must ship on all three seams before it counts as shipped. The cost is named in `20`
  and inherited here.
- **Tier-vs-feature tension.** A maker who wants a single feature from a higher tier
  without paying the rest of that tier's cost is a known wish; granting it would break
  the tier model. Holding the line is a discipline concern.
- **Shared-renderer drift.** The shared-renderer commitment is structurally enforced by
  the editor/runtime process split (per `40`), but a *renderer-internal* drift — features
  active in one build configuration and not another — is a discipline concern that the
  structural boundary does not catch.
- **Shader-language version churn.** The chosen shader language is an external project
  with its own release cadence; a version change is an engine-level concern, not a
  maker-level one, but its handling is named here so it cannot surprise a later slice.
- **First-launch shader compile in some forms of distribution.** Even with build-time
  bakes, some platforms may require on-device prewarm passes (driver-specific shader
  caches). The maker-facing surface for this is a downstream concern; the renderer's
  commitment to no-runtime-compile is not weakened, but the platform's prewarm is not
  glibre's compilation.
- **Material-vs-shader contributor path.** Letting contributors write shader source for
  custom material nodes is the engine's escape hatch from the no-code constraint at the
  rendering surface; without discipline, that hatch widens into a parallel authoring
  surface the vision refuses.
