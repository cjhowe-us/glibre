# 15 — Context Map

Cites the thesis in [`00-vision.md`](00-vision.md), the substrate stances in
[`20-platform-strategy.md`](20-platform-strategy.md), and the bounded contexts in
[`10-bounded-contexts.md`](10-bounded-contexts.md).

This pillar describes the *relationships* between contexts: who depends on whom, where the seams are
negotiated, and where they are not. The contexts themselves — their responsibilities and vocabulary
— live in `10`. Read this file after that one.

Integration shape is a design decision, not an accident of import order. Each pairing below is one
of: partnership, customer/supplier, conformist, anti-corruption layer (ACL), shared kernel, open
host service, or published language. Where this file picks a shape, every later pillar uses it.

## How to read this pillar

The map is presented as a set of **named seams**, each with:

- The two contexts that meet at it.
- The DDD relationship pattern that governs it.
- Direction (upstream → downstream where applicable).
- What flows across the seam.
- The failure mode the pattern is meant to absorb.

Seams are named because every later pillar will refer to them. If a relationship is not named here,
it does not yet exist as a design commitment.

## Strategic shape at a glance

- **Composition and Authoring are the core.** Every other context either supplies them or
  is surfaced through them.
- **Platform is everyone's supplier.** It is the most-depended-upon context in the map;
  it has no upstream supplier of its own and many downstream consumers.
- **Content is the next-most-depended-upon.** Both Runtime and Rendering consume from
  it; Authoring also consumes from it through a separate seam.
- **Authoring is the union of seams**, not a single seam. Every primitive-producing
  context contributes primitives into Authoring's surfaces; the integration with
  Authoring is the act of exposing that primitive.
- **Accessibility & Localization cross-cuts as a contract publisher.** Every producing
  context conforms to the semantic-metadata and string-identity contracts it publishes,
  and it has its own seam to Platform for OS assistive integration.

## Named seams

### S-1 — Authoring ← every other primitive-producing context

**Pattern.** Open host service. Authoring publishes a stable extension protocol; every other context
contributes its primitives, inspectors, palettes, and editors as conformants of that protocol.

**Direction.** Authoring is the upstream host of the protocol; contributing contexts are downstream
conformants of it. Maker-intent events (edits, selections, undo events) flow back to the
contributing context as a secondary, unidirectional notification channel; the protocol contract is
Authoring's alone.

**What flows.** Primitive definitions and their editing affordances, into Authoring; maker intent,
back out to the originating context.

**Failure mode absorbed.** Without this seam, every context would build its own editor surface and
the editor would fragment. The open host service keeps the editor coherent even as the engine grows
new contexts.

### S-2 — Composition → Runtime

**Pattern.** Published language. Composition publishes a stable artifact-loading contract; Runtime
loads what Composition produces and invokes it.

**Direction.** Composition upstream, Runtime downstream.

**What flows.** Compiled graph artifacts; metadata describing entry points, lifetimes, and tick
cadence.

**Failure mode absorbed.** Composition's internal toolchain (IR, lowering, codegen) can change
without Runtime adapting; only the published artifact-loading language is shared.

### S-3 — Composition ⇄ each context's node library

**Pattern.** Anti-corruption layer. Each context that contributes nodes (Rendering, Simulation,
Content, Runtime) publishes a node library; Composition adapts the library to its generic typed-node
model.

**Direction.** Context upstream (defines the operations), Composition downstream (presents them as
nodes).

**What flows.** Operation signatures, type information, side-effect declarations, determinism class
declarations.

**Failure mode absorbed.** Without the ACL, Composition's generic node model would be polluted with
rendering-specific or simulation-specific concepts, and every context's internal refactor would
ripple into the graph editor. The ACL means the node libraries can change shape internally while
Composition's surface stays stable.

### S-4 — Content → Runtime

**Pattern.** Published language. Content publishes a baked-artifact format; Runtime loads only that.

**Direction.** Content upstream, Runtime downstream.

**What flows.** Baked assets (meshes, textures, scenes, prefabs, configuration); the identity by
which Runtime resolves a reference to an asset; the change events that drive hot-reload at editor
time.

**Failure mode absorbed.** Importer plugins and source-format choices can churn without Runtime
caring; Runtime depends on the bake output, not the source.

### S-5 — Content → Rendering

**Pattern.** Published language (the renderer-targeted subset of S-4).

**Direction.** Content upstream, Rendering downstream.

**What flows.** Render artifacts (compiled shaders, pipeline state objects, texture data, mesh data)
in the per-platform shape Rendering expects.

**Failure mode absorbed.** The shader compiler and texture compressor are Content's problem;
Rendering trusts the artifact format and refuses to do edit-time work itself unless the maker is
authoring shaders or materials live.

### S-6 — Runtime → Simulation

**Pattern.** Customer/supplier with explicit phasing.

**Direction.** Runtime upstream (owns the tick), Simulation downstream (consumes tick budget,
produces state changes within declared phases).

**What flows.** Phase boundaries within a tick; world-state references each subsystem may read or
write in its phase; the determinism class each subsystem advertises.

**Failure mode absorbed.** Subsystem ordering becomes deterministic at the tick level without each
subsystem knowing about every other one. Inter-subsystem ordering is owned by Runtime, not
negotiated at runtime between subsystems.

### S-7 — Runtime → Rendering

**Pattern.** Customer/supplier with a read-only snapshot.

**Direction.** Runtime upstream, Rendering downstream. Runtime owns the world state (per
[`10-bounded-contexts.md`](10-bounded-contexts.md)) and is the sole producer of the snapshot
Rendering consumes; Simulation's contributions are already integrated into that world state through
S-6 before Runtime exposes the snapshot.

**What flows.** A read-only renderable view of world state, valid for the duration of one frame.
Rendering does not mutate world state.

**Failure mode absorbed.** Rendering does not race the simulation; simulation does not have to wait
for rendering. The snapshot decouples their cadences (tick rate and frame rate are independent, per
`10`).

### S-8 — Platform → everyone

**Pattern.** Conformist. The platform exposes its OS-level seams and the rest of the engine accepts
them as given.

**Direction.** Platform upstream; Runtime, Rendering, Authoring, Distribution, Content all
downstream.

**What flows.** Native windowing primitives, OS input events, filesystem access, threading
primitives, time, processes, the Vulkan rendering seam via SDL3.

**Failure mode absorbed.** No upstream context dictates platform shape; the platform seam is taken
as given and downstream contexts adapt. This is the only major place where glibre is a conformist
(rather than translating with an ACL): platform diversity is the reality we live in.

Distribution is one of these downstream conformants in a packaging-specific form: it conforms to
per-platform packaging conventions (artifact shape, signing requirements, store-metadata
expectations) rather than inventing its own. This is part of S-8, not a seam of its own.

### S-9 — Authoring → Distribution

**Pattern.** Customer/supplier.

**Direction.** Authoring upstream (the maker chooses what to ship and when), Distribution downstream
(packages it).

**What flows.** A frozen project state for a build target; the chosen build target; signing
material; store metadata.

**Failure mode absorbed.** Distribution never reads the live editor state; it always consumes a
deliberately frozen snapshot. The maker's authoring loop is decoupled from the packaging loop.

### S-10 — Accessibility & Localization → every primitive-producing context

**Pattern.** Published language. A11y & L10n publishes the semantic-metadata and string-identity
contracts as a shared language; every producing context (Authoring, Rendering, Simulation,
Composition's node libraries where they expose maker-visible names, Content where assets carry
user-visible strings) is a conformant that emits data satisfying those contracts. There is no
service call into A11y & L10n; conformance is by data shape.

**Direction.** Accessibility & Localization upstream as authority for the language; producing
contexts downstream as conformants.

**What flows.** Producing contexts emit semantic metadata on every visible or interactive primitive
and string identities for every piece of authored text, all conforming to the contracts A11y & L10n
publishes. At display time each producing context resolves the translated string or semantic
annotation it needs by reading from A11y & L10n's published catalogs and semantic tree — it pulls,
A11y & L10n does not push.

**Failure mode absorbed.** A11y & L10n cannot retrofit accessibility onto opaque primitives;
producing contexts are forced into the contract at authoring time, not after. This seam enforces the
vision's "primitive that cannot be made accessible is incomplete" commitment.

### S-11 — Accessibility & Localization → Platform

**Pattern.** Conformist (specialization of S-8).

**Direction.** Platform upstream (owns the OS assistive-technology interop seam), A11y & L10n
downstream (delivers the engine's semantic and translatable model into the shape Platform expects).

**What flows.** Engine-side semantic tree and translated strings, in the form the platform
accessibility API can consume; OS-level assistive events and locale changes coming back.

**Failure mode absorbed.** A11y & L10n cannot drive OS assistive APIs directly; Platform owns that
interop seam (per [`10-bounded-contexts.md`](10-bounded-contexts.md)). A semantic model not legible
to Platform's seam would produce a silent accessibility failure in the shipped game; making the seam
explicit prevents that.

### S-12 — Composition ⇄ Distribution (artifact-only)

**Pattern.** Published language (specialization of S-2).

**Direction.** Composition upstream, Distribution downstream.

**What flows.** The bake-artifact form Composition has already produced for Runtime; Distribution
includes it in the shipped build.

**Failure mode absorbed.** Distribution does not invoke the Composition toolchain; it ships what was
already baked. Build correctness is established at the Composition seam (S-2), not re-established at
the Distribution seam.

### S-13 — Content ⇄ Distribution (artifact-only)

**Pattern.** Published language (specialization of S-4).

**Direction.** Content upstream, Distribution downstream.

**What flows.** Baked assets selected by build target.

**Failure mode absorbed.** Same as S-12: Distribution ships what Content has already baked; it does
not re-bake.

### S-14 — Content → Authoring (queries and hot-reload)

**Pattern.** Published language.

**Direction.** Content upstream (owns the Asset DB and the hot-reload event stream), Authoring
downstream (queries the Asset DB for inspectors and asset browsers, and consumes hot-reload events
to refresh open editing surfaces).

**What flows.** Asset-identity and dependency queries from Authoring into Content; baked-asset
metadata back to Authoring; hot-reload change events pushed from Content to Authoring as re-bakes
complete.

**Failure mode absorbed.** This is a *different* seam from S-4 (Content → Runtime) even though both
are downstream of the same upstream. If Authoring resolved assets through the same contract Runtime
uses, editor-side asset resolution would diverge from runtime resolution during in-flight
hot-reloads — the editor would see new bytes the running game session has not yet adopted.
Separating the seams keeps editor responsiveness independent of runtime resolution order.

### S-15 — Authoring ⇄ Runtime (in-editor play-session control channel)

**Pattern.** Customer/supplier with a small published-language control vocabulary.

**Direction.** Authoring is the upstream party: it spawns the in-editor play session, drives its
lifecycle (start, pause, resume, stop), routes input focus, and pushes hot-reload triggers into it.
The runtime play session is the downstream party: it acknowledges control messages, surrenders or
accepts focus on request, and reports back its lifecycle state (running, paused, crashed). The
channel does **not** carry world state, asset bytes, or graph artifacts — those flow through S-4,
S-2, and S-14 directly to the runtime process.

**What flows.** Lifecycle commands (start / pause / resume / stop), focus-transfer requests,
hot-reload notifications, lifecycle status events. Nothing else.

**Failure mode absorbed.** Without this seam, every editor-to-runtime interaction during play would
have to invent its own ad-hoc protocol. Naming the channel makes it possible to reason about its
surface, version it, and stop it from accreting responsibilities that belong to the data-flow seams.

## What this map deliberately does not include

- **Inter-Simulation subsystem coupling.** Physics, AI, animation, audio mixing all
  live inside Simulation; how they communicate is a Simulation-internal concern. They
  meet the other contexts only through Simulation's published phase model (S-6).
- **The Networking context's seams.** Networking does not yet exist (see `10`); when it
  is claimed, it will gain seams to Runtime (most likely customer/supplier with a
  replication protocol), Simulation (conformist on determinism class), and Distribution
  (customer/supplier for matchmaking endpoints). The shapes are *anticipated* here so
  the existing seams do not preclude them; they are not committed.
- **The maker as a context.** The maker uses the engine but is not modeled as a
  context. Their edits flow through Authoring; their intent does not have its own
  model.

## Risks deferred to `90`

- **S-1 is wide.** The open host service Authoring publishes will end up being the
  single most widely-consumed contract in the engine. Versioning it as contexts grow
  primitives is a known load-bearing risk.
- **S-3's ACL count grows with contexts.** Every new context that contributes nodes
  adds another node-library ACL. The cost is per-context, not per-node; nonetheless,
  the model needs to stay legible as the number of contexts grows.
- **S-8 conformism vs portability.** Conformist coupling with Platform is the right
  trade for now, but it means OS-level platform changes can propagate into multiple
  downstream contexts. The substrate stance in
  [`20-platform-strategy.md`](20-platform-strategy.md) collapses graphics to a single
  Vulkan seam through SDL3, which narrows the surface that can shift; the context map
  carries that narrower cost.
- **S-10 obligates every producing context.** Treating A11y & L10n as the host of the
  semantic-metadata contract puts compliance pressure on every producing context. The
  vision commits to first-class accessibility, so the cost is justified, but it is the
  seam most likely to leak A11y/L10n concerns into producing contexts if discipline
  lapses on either side.
- **S-11 depends on platform assistive APIs.** OS-level assistive technology evolves
  independently of the engine; a Platform-level change can force A11y & L10n into a
  silent failure mode if Platform's seam to the OS shifts without notice.
- **S-14 versus S-4 divergence.** Authoring and Runtime now resolve assets through
  separate Content seams. The risk is that the two seams drift in subtle ways
  (identity, dependency, metadata) so the editor sees a different world than the running
  game session. Keeping the two seams in lock-step is a discipline concern, not a design
  one — but the discipline has to hold.
- **Anticipated Networking seams** are not yet validated. The map's deferral of
  Networking is honest, but the *shape* of its future seams is a guess; a committed
  Slice-4 scope may force-revise this map.
