# 30 — Authoring and No-Code

Cites the thesis in [`00-vision.md`](00-vision.md), the contexts in
[`10-bounded-contexts.md`](10-bounded-contexts.md), the integration shape in
[`15-context-map.md`](15-context-map.md), the substrate stances in
[`20-platform-strategy.md`](20-platform-strategy.md), and the runtime architecture in
[`40-runtime-architecture.md`](40-runtime-architecture.md).

This pillar pins the authoring philosophy that turns the vision into something a maker can actually
sit down in front of. It covers what no-code means at the level of daily authoring, the universal
logic-graph stance, how authoring ergonomics balance for makers versus contributors, the AI-assist
posture, and the connection between authoring acts and the artifacts the runtime consumes.

## How to read this pillar

Decisions are presented as **stances**, in the same shape as
[`40-runtime-architecture.md`](40-runtime-architecture.md). *Committed* stances may be depended on
by the rest of the design set; *deferred* stances await an upstream resolution or a committed scope;
*anticipated* stances are likely shapes called out so later pillars don't preclude them. Failure
modes are named per Murphy.

## What no-code means at authoring time

**Committed.**

For the maker, the day-to-day act of building a game in glibre is composition: arranging,
parameterizing, and connecting typed primitives in visual editors. The maker reads no source code,
writes no source code, and is never asked to drop down a layer to "fix this in code."

This is a stronger claim than "visual scripting is supported." It is the *only* authoring surface
the engine exposes to makers. There is no parallel text-language surface that a maker is expected to
learn for the parts the visual surface doesn't reach yet; if a capability is shipped, it is
reachable from the visual surface (per [`00-vision.md`](00-vision.md)).

**Failure mode absorbed.** Without this commitment the engine slides into "no-code for the easy
half, learn-this-language for the hard half." Two surfaces means two communities, two documentation
efforts, and a quiet pressure to make the visual surface "good enough" rather than total. Holding
the line keeps the engine's investment concentrated on one surface.

## Composition is the substrate of authored behavior

**Committed.**

Every piece of authored *behavior* — gameplay logic, material parameter behavior, audio behavior,
animation control, effect parameters, tool automation — is a graph in the Composition context (per
[`10-bounded-contexts.md`](10-bounded-contexts.md)). Composition is not one editor among many; it is
the substrate the maker's behavior lives in, surfaced by different palettes in different contexts.

**Why one substrate, not many.** A visual scripting layer specialized per subsystem (a "gameplay
graph", a "material graph", an "audio graph") looks superficially right but costs the engine three
times the design budget, three times the documentation, and trains the maker that
*moving an idea between subsystems is a language barrier*. One substrate, specialized at the edges
by what nodes are available, gives the maker one model and the engine one set of tools (debugger,
profiler, inspector, hot-reload) that applies everywhere.

**What is specialized per context.** The *node palette*. Rendering contributes material and shader
nodes; Simulation contributes physics, AI, animation nodes; Content contributes asset reference
nodes; Runtime contributes lifecycle, input, and timing nodes. The contribution mechanism is the
anti-corruption layer at S-3 in [`15-context-map.md`](15-context-map.md): each context's nodes adapt
to Composition's generic typed-node model.

**Failure mode absorbed.** A maker who learns Composition once carries that knowledge into every
authoring task. Subsystem boundaries do not become language boundaries.

## Graphs compile, they do not interpret

**Committed.**

A graph is not interpreted at runtime; it is compiled to a native artifact ahead of time. This is
the lowering decision committed in [`20-platform-strategy.md`](20-platform-strategy.md) and the
runtime contract committed in [`40-runtime-architecture.md`](40-runtime-architecture.md). The
toolchain is the engine's problem; the *experience* is the maker's.

**What this buys the maker.**

- Authored behavior runs at native speed; the maker is not trading authoring ergonomics
  for runtime ergonomics.
- Type errors and broken connections are diagnostics surfaced at edit time and at bake
  time, not silent corruption at runtime.
- Deterministic behavior is achievable on every supported platform because the *same
  graph* lowers through the *same intermediate representation* on every platform.

**What this costs the maker.**

- There is a bake step between editing and seeing the result. Live preview and hot-reload
  (per [`40-runtime-architecture.md`](40-runtime-architecture.md)) keep this bake step
  small and frequent rather than rare and disruptive, but it is real.

**Failure mode absorbed.** The maker is never told "the game runs slowly because your scripts are
interpreted; rewrite the slow parts in code." Performance is a property of the graph the maker
authored, not of which authoring surface they used.

## Live preview is the authoring loop

**Committed.**

Whenever a change can be shown to the maker without restarting a play session, it is. The authoring
loop is *make a change, see the change*, not *make a change, build, run, see the change*. The
hot-reload channels in [`40-runtime-architecture.md`](40-runtime-architecture.md) carry this for
assets, graphs, and shaders; this pillar commits to *using* them by default. The trigger flows over
S-15 in [`15-context-map.md`](15-context-map.md) from authoring into the in-editor play session.

**Maker-facing iOS deficit.** On a project that targets iOS exclusively, graph hot-reload is not
available in the shipped game (per [`40-runtime-architecture.md`](40-runtime-architecture.md)). The
in-editor play session, which always runs on a JIT-host authoring platform, still hot-reloads graphs
— so the maker's authoring loop is intact. Communicating this difference at the right moment is an
authoring-experience commitment owned by this pillar; the specific maker-facing surface for it is
deferred to a committed scope that promises iOS-only shipping.

**Failure mode absorbed.** A maker who has to wait on a build for every iteration learns to make
changes in batches, which kills the tight authoring loop the vision is built on. Making live preview
the default keeps the loop tight whether the maker remembers to ask for it or not.

## Maker ergonomics dominate; contributor ergonomics adapt

**Committed.**

When an authoring decision trades off between *maker convenience* and
*engine contributor convenience*, the maker wins. This is the standing priority across the entire
authoring surface, applied any time the two pull in different directions.

**Why.** Makers are the primary audience; engine contributors are not. Optimizing for the
contributor produces an engine that contributors enjoy and makers leave.

**Failure mode absorbed.** Every accumulated small contributor-convenience decision is a small
maker-confusion decision. Naming the priority means the engine has a way to notice when it has
drifted.

## Configuration is property-on-a-primitive

**Committed.**

Every property a primitive has — an authored scene object, a prefab, a graph node, a material, an
animation state, an importer setting — is exposed in an inspector inside the editor. There is no
sidecar configuration file the maker is expected to open separately, no build-pipeline config the
maker is expected to author, no per-target settings buried behind a text editor.

**Why.** Configuration in a sidecar file is configuration the maker has to know exists. A maker who
edits a primitive expects every knob to be on the primitive; an engine that hides knobs in `.config`
files re-introduces "drop down a layer to do the real work" — the exact failure mode the no-code
stance refuses.

**Failure mode absorbed.** The maker is not told "and now open this file in your editor" anywhere in
the authoring loop.

## Fragments are the reuse primitive

**Committed.**

A named, parameterized subgraph (a **fragment**, per
[`10-bounded-contexts.md`](10-bounded-contexts.md)) is the only mechanism by which a maker extracts
and reuses logic. Inheritance, mixins, traits, and other code-language reuse metaphors are not
exposed at the authoring surface; reuse is composition, not subtyping.

**Why.** Subtyping is a code-language concept and brings code-language pathologies (diamond
inheritance, fragile base classes, virtual dispatch surprises) that no-code makers should not need
to reason about. Composition by named fragment matches the way the maker already thinks about
reusing behavior: "wrap this bit and use it twice."

**Failure mode absorbed.** A maker who builds a complex hierarchy of fragments produces a graph that
another maker can read straightforwardly. The reuse mechanism does not become a hiding mechanism.

## AI-assist is an authoring accelerator, not an authoring surface

**Committed.**

AI-assist runs inside the editor as an accelerator: it helps the maker do what they were going to do
anyway, faster and with more confidence. It is not a separate authoring surface. The maker still
composes graphs; AI-assist makes that composing quicker.

**What this rules out.**

- **Generate-game-from-prompt.** Not shipped. The maker composes; the engine does not
  generate games on the maker's behalf, even when the output would be editable graphs.
  An accelerator that bypasses the act of composing is no longer an accelerator.
- **Opaque AI output.** Anything AI-assist produces lands in the project as primitives
  the maker can inspect, modify, and remove. Output the maker cannot edit is not shipped.
- **Runtime AI features as a hidden engine capability.** If the shipped game uses AI
  inference, that is a *node library* the maker composes with, not a built-in feature
  acting silently behind the scenes.

**Anticipated.** AI providers, privacy posture, and cost-allocation model are tactical. These
decisions land in [`80-distribution-and-extensibility.md`](80-distribution-and-extensibility.md)
when that pillar is written.

**Failure mode absorbed.** The maker is never in a position where the AI produced something they
cannot understand, modify, or remove. AI-assist accelerates authoring; it does not replace it, and
it does not produce content the maker has to take on faith.

## Editor-time discoverability matters as much as runtime correctness

**Committed.**

Every primitive ships with the search hooks, tags, examples, and inline documentation a maker needs
to find it. A primitive without those is treated as incomplete in the same sense that an
inaccessible primitive is incomplete (per the vision in [`00-vision.md`](00-vision.md)).

**Why.** A capability the maker can't find is a capability the maker doesn't have. The engine's
surface area is wide; without aggressive discoverability the maker is asked to know what they don't
know.

**Failure mode absorbed.** Adding a primitive does not become a marketing exercise. The primitive's
metadata is the marketing.

## What this pillar deliberately does not decide

- **The visual style of the editor, the keymap, or theme.** Tactical; not a strategic
  commitment.
- **The node-graph editing UX in detail.** Tactical; the *commitment* to a coherent
  graph surface is here, the form is downstream.
- **Which AI providers integrate, and how.** Tactical; these decisions land in
  [`80-distribution-and-extensibility.md`](80-distribution-and-extensibility.md) when
  that pillar is written.
- **The graph IR or codegen toolchain.** Owned by Composition's tactical design; the
  *stance* (AOT, no JIT, one IR) is in
  [`20-platform-strategy.md`](20-platform-strategy.md).
- **How fragments are versioned across project history.** Owned by Content (for
  versioning) and Authoring (for surfacing); deferred to those pillars.

## Risks deferred to `90`

- **Single substrate cost.** Insisting on one logic substrate for gameplay, materials,
  audio, animation, and tooling means every subsystem's authoring surface depends on
  Composition working. A Composition regression hits everything at once.
- **Discoverability scales sublinearly with palette size.** A growing palette without
  growing discoverability investment becomes a directory of options the maker cannot
  navigate.
- **AI-assist drift.** "Generate graphs from natural language" is a tempting feature; it
  contradicts the commitment that the maker authors and AI accelerates. Holding the line
  is a discipline concern.
- **Maker-vs-contributor tension** is not a one-time decision. The engine will
  repeatedly face moments where contributor convenience and maker convenience pull in
  opposite directions; named priority gives the engine a way to notice and choose.
- **The bake step's UX cost.** Compiled graphs are an engine-level win and a maker-level
  cost. Live preview hides the cost most of the time; the cases where it doesn't (a
  first build, a clean reload, a target switch) need explicit attention.
- **Fragment versioning across a project's history.** A graph that worked at version N of
  a fragment may not work at N+1; the fragment's parameter shape can change. Without a
  versioning answer, maker projects rot when shared fragments evolve.
