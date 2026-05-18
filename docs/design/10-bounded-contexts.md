# 10 — Bounded Contexts

Cites the thesis in [`00-vision.md`](00-vision.md). Applies the substrate stances committed in
[`20-platform-strategy.md`](20-platform-strategy.md). This pillar names the bounded contexts glibre
is modeled around, states what each is responsible for, and pins down enough of the ubiquitous
language for the rest of the design set to use the same words for the same things.

The relationships *between* contexts — upstream/downstream, anti-corruption layers, shared kernels —
live in [`15-context-map.md`](15-context-map.md). This file describes each context in isolation.

## How to read this pillar

Each context has:

- **Purpose** — one sentence on why this context exists.
- **Responsibilities** — the work the context owns end-to-end.
- **Ubiquitous language** — the canonical terms. Every later pillar uses these terms with
  these meanings; if a term is missing here, it has not been agreed yet.
- **Out of scope** — work that looks like it might belong here but does not.

Contexts are coarse on purpose. Where a capability area (e.g. "abilities", "quests", "inventory")
looks like it deserves its own context, it almost never does — it is a domain mechanic *built from*
the primitives a context provides, not a context of its own. The strategic-vs-tactical line in DDD
is the same line as the no-code-vs-mechanic line in glibre.

## Domain classification

- **Core domain** — the parts where the engine *wins*: Composition and Authoring. The
  no-code path is what the engine differentiates on; the design budget concentrates here.
- **Supporting subdomains** — Runtime, Simulation, Rendering, Content,
  Accessibility & Localization. Necessary, deliberately modeled, but not the source of
  differentiation. Accessibility & Localization sits here despite enforcing a core
  vision commitment: the differentiating work is in Composition's and Authoring's
  no-code primitives being inherently accessible-and-translatable, not in inventing the
  a11y/l10n model itself, which is well-understood territory adapted from established
  practice.
- **Generic subdomains** — Platform, Distribution. Well-understood territory; the engine
  uses the boring solution and does not lavish DDD machinery on it.

Anything outside this list is either inside one of these contexts as a mechanic, or it is not part
of glibre.

## Authoring

**Purpose.** The editor experience: where the maker assembles, edits, and runs the game. Authoring
is the engine's maker-facing surface. The verb "compose" is reserved for the Composition context's
graph-logic meaning; Authoring assembles primitives.

**Responsibilities.**

- Scene, prefab, and project organization as the maker sees them.
- Inspectors, palettes, asset browsers, search.
- The editing surfaces for every other context's primitives (graphs for Composition,
  scenes for Runtime, materials and lights for Rendering, timelines for Simulation,
  importers for Content, accessibility properties for Accessibility & Localization,
  packaging targets for Distribution).
- Live preview hosting (the rendering surface itself is owned by Rendering; *placing it
  in the editor and feeding it the right scene* belongs here).
- Undo / redo / history for everything the maker touches.

**Ubiquitous language.**

- **Maker** — the human authoring a game in glibre. Always preferred to "user", "dev",
  or "designer", which collapse roles the engine deliberately keeps distinct.
- **Project** — the unit of work the Authoring context manages. A project produces zero
  or more *shipped builds* (defined in Distribution).
- **Scene** — the unit a maker opens, edits, and runs. Scenes contain entities and
  reference assets; their on-disk form is owned by Content.
- **Inspector** — the editing surface for a single primitive.
- **Palette** — the list of primitives the maker can add to the current surface.

**Out of scope.**

- How primitives are *executed*. Composition and Runtime own that.
- Where assets live on disk and how they are versioned. Content owns that.
- How the editor's own window is opened and how its input arrives. Platform owns that.
- The renderer the live preview uses. Rendering owns that.

## Composition

**Purpose.** The universal logic substrate the maker authors *in*. Composition is the engine's
no-code core; every behavior, every parameter relationship, every reactive piece of the game is a
graph in this context.

**Responsibilities.**

- The visual logic graph: nodes, ports, types, the editor's understanding of "what is
  legal to connect to what".
- The transformation from authored graph to an executable artifact for the target
  platform. The toolchain and intermediate representation are tactical and not owned by
  this context.
- Reusable graph fragments (named, parameterized, composable).
- Graph-level validation, type errors, and the diagnostic surface the maker reads when a
  graph is incomplete.

**Ubiquitous language.**

- **Graph** — the authored unit. A graph is *the* program the maker writes. Engines that
  use other terms (script, blueprint, behavior tree) are referring to specialized graphs;
  glibre has one graph kind, specialized by node palette and host.
- **Node** — a primitive operation in a graph. Nodes are typed, pure where possible, and
  composable.
- **Port** — a typed input or output on a node.
- **Fragment** — a named, reusable subgraph. Fragments are the maker's reuse primitive.
- **Lowering** — the transformation from graph to intermediate representation to native
  code. Always AOT; never JIT.
- **Artifact** — the native-code output of lowering, loaded by Runtime.

**Out of scope.**

- Which native code the artifact targets (Platform/Rendering decide per-target ABI).
- Where the artifact lives on disk (Content owns delivery; Distribution owns shipping).
- How the editor renders a graph (Authoring owns the surface).
- The semantics of any specific node beyond its type signature; node *libraries* belong
  to the context whose primitives they expose (rendering nodes to Rendering, simulation
  nodes to Simulation, etc.).

## Runtime

**Purpose.** The shipped game's execution environment. Runtime owns the frame, the process, and the
lifecycle.

**Responsibilities.**

- Tick / frame model, scheduling, lifecycle hooks (start, pause, resume, quit).
- Scene loading, entity instantiation, save / load of game state.
- Dispatch of OS-level input to in-game listeners (input *acquisition* belongs to
  Platform; in-game *meaning* lives here).
- Hosting compiled graph artifacts and invoking them on the right cadence.
- The runtime host process. Execution posture (and the boundaries that come with it) is
  inherited from platform strategy.

**Ubiquitous language.**

- **Tick** — one step of the simulation/game clock. glibre is explicit about tick rate
  per scene; rendering rate is independent.
- **Entity** — the in-runtime instance of an authored thing. Entities are runtime
  objects; their authoring-time identity is owned by Authoring.
- **World** — the live, mutable graph of entities and their state for one running game
  session.
- **Save** — a serialized snapshot of world state. Save format is owned by Runtime in
  collaboration with Content.

**Out of scope.**

- The subsystems that *run during* a tick (physics, AI, animation, audio mixing) —
  Simulation owns those.
- Rendering of any kind — Rendering owns the frame.
- Asset bytes on disk and streaming — Content owns those.
- Networking, multiplayer, replication — these get their own context (Networking) when
  the relevant committed scope claims them; until then, they are deferred and live as a
  named risk in [`90-risks-and-open-questions.md`](90-risks-and-open-questions.md).

## Simulation

**Purpose.** The subsystems that make the world feel alive: physics, AI, animation evaluation, audio
mixing, navigation, particle simulation. All have in common that they consume tick budget and
produce world-state changes.

**Responsibilities.**

- Physics: rigid body, soft body, cloth, fluid, destruction, character motion.
- Animation evaluation: skeletal, vertex, state machines, blends, IK.
- AI: behavior trees, planners, perception, navigation, steering, crowd.
- Audio mixing: spatialization, occlusion, dynamic mixing, HRTF.
- Particle simulation and effect graphs (visualized by Rendering but simulated here).
- The determinism contract per subsystem (see [`90`](90-risks-and-open-questions.md)).

**Ubiquitous language.**

- **Subsystem** — a Simulation participant with its own update phase and state.
- **Phase** — a slice of a tick during which a subsystem may read and write specific
  world state. Phases are explicit so multi-subsystem ordering is deterministic.
- **Determinism class** — the contract a subsystem advertises (deterministic / best-effort
  / non-deterministic). Subsystems used in a deterministic feature (rollback,
  replay, automated test) must be deterministic; the rest may be pragmatic.

**Out of scope.**

- Rendering of simulated state — Rendering consumes Simulation outputs.
- Authoring the *parameters* of simulated behavior — those are Composition graphs and
  inspectors in Authoring.
- Networking of simulated state — owned by the future Networking context.

## Rendering

**Purpose.** Turning world state into pixels, on every supported graphics API, with artifacts
authored once and compiled per platform.

**Responsibilities.**

- The Vulkan renderer that runs on the single graphics seam of
  [`20-platform-strategy.md`](20-platform-strategy.md).
- Materials, shaders, lighting, post-processing, 2D / 3D / vector rendering.
- The shader pipeline: authoring surface, per-platform compilation, and runtime artifact
  delivery.
- Viewport hosting *within* the editor (the surface placement is Authoring; what is
  drawn on it is here).
- Render-graph composition and resource lifetimes.

**Ubiquitous language.**

- **Material** — the maker-facing concept; the rendering parameters and shader behavior
  attached to a surface.
- **Shader** — the authored shader source; always in one shader language at the maker's
  level.
- **Render graph** — the runtime arrangement of render passes and resources for a frame.
- **Render artifact** — a precompiled shader, pipeline state, or other per-platform
  baked artifact loaded at runtime.

**Out of scope.**

- The simulation that produces the state being rendered — Simulation owns that.
- The Vulkan seam through SDL3 — Platform owns it.
- The on-disk format of render artifacts — Content owns delivery.

## Content

**Purpose.** Content owns the asset lifecycle — source import, transformation, runtime delivery, and
live re-propagation — for all authored media that is not a logic graph: meshes, textures, materials,
animations, audio, scenes, prefabs, and project configuration.

**Responsibilities.**

- Importers for source formats. The specific format catalog is owned by
  [`60-content-pipeline.md`](60-content-pipeline.md) and expands as committed scopes
  claim new ones.
- The asset database (identity, dependencies, search, references).
- Bake: the transformation from source to runtime-ready artifacts (compressed textures,
  per-platform shader artifacts, baked graph artifacts).
- Hot-reload: live propagation of asset edits into a running editor session.
- Asset version control posture: what is human-editable, what is generated, what is
  cached.

**Ubiquitous language.**

- **Source asset** — the human-edited input file produced by an external tool.
- **Baked artifact** — the runtime-ready output of one or more sources after bake.
- **Asset DB** — the database of identities, dependencies, and metadata.
- **Importer** — a plugin (built-in or third-party) that consumes a source format and
  emits Asset DB entries.
- **Hot-reload** — the contract by which a re-baked artifact replaces its predecessor in
  a running editor session.

**Out of scope.**

- Live network streaming to end users (Distribution).
- Per-game save data (Runtime).
- The shader compiler itself (a Rendering toolchain that Content invokes).

## Distribution

**Purpose.** Getting the maker's finished game into players' hands, and getting extensions / plugins
/ mods into the maker's project.

**Responsibilities.**

- Per-platform packaging into the platform-native artifact each committed build target
  expects.
- Signing, entitlements, store metadata.
- The plugin / mod model (in-process vs out-of-process boundaries are a Distribution +
  Platform concern; the model itself is here).
- Marketplace / extension distribution stance.
- Cloud-build posture, if/when a committed scope claims it.

**Ubiquitous language.**

- **Build target** — a (platform, configuration) tuple that produces one shipped
  artifact.
- **Shipped build** — the player-facing output of packaging a project for a build target.
- **Extension** — anything a maker installs into a project that wasn't there before
  (importer, node library, editor tool, asset pack, mod).

**Out of scope.**

- The act of *running* a shipped build — that is the player's OS, not glibre.
- The asset / graph bake — Content and Composition own those, even when Distribution
  invokes them.

## Platform

**Purpose.** The seam between glibre and the operating system. Platform owns the substrate-level
concerns where every other context would otherwise have its own per-OS shim.

**Responsibilities.**

- Native windowing through SDL3 for both editor and runtime, per
  [`20-platform-strategy.md`](20-platform-strategy.md).
- OS input acquisition (translating OS events into engine-level input events;
  *interpretation* belongs to Runtime).
- Filesystem, threading, time, processes.
- The Vulkan rendering seam, reached through SDL3 — owned at the substrate boundary,
  consumed by Rendering.
- The execution-model boundary between editor host and runtime host: ensuring no
  dependency that requires capabilities the runtime cannot support leaks across the
  boundary. (The execution-model decision itself is owned by
  [`20-platform-strategy.md`](20-platform-strategy.md).)

**Ubiquitous language.**

- **Host** — the OS-level process the engine is running in (editor host vs runtime
  host).
- **Interop seam** — the Vulkan boundary at which glibre crosses from managed to native
  code, mediated by SDL3 for windowing and surface creation and by MoltenVK on Apple
  platforms.

**Out of scope.**

- Anything that runs *above* the OS abstraction (gameplay, simulation, rendering at a
  level higher than the API seam).
- Anything Distribution owns (packaging is not platform abstraction even though it is
  per-platform).

## Accessibility & Localization

**Purpose.** Make every authored primitive accessible and translatable by construction. This is its
own context, not a checklist, because it has its own model (a semantic tree, a translatable-string
identity, a focus model) that lives alongside the visual content.

**Responsibilities.**

- The semantic / accessibility tree exposed at runtime to assistive technologies on each
  platform.
- The translatable-string identity model (a string is an identity, not its current
  text).
- Accessibility properties on every authored primitive (label, role, semantic
  description, color-independence affordances).
- Localization workflow integration in Authoring (string catalogs, pluralization,
  message context).
- Authoring-time linting: a primitive that cannot be made accessible or translatable is
  an incomplete primitive (per [`00-vision.md`](00-vision.md)).

**Ubiquitous language.**

- **Semantic node** — an entry in the runtime accessibility tree corresponding to a
  visible, interactive, or audible element.
- **String identity** — the identifier a piece of text is referenced by; the actual
  text comes from a string catalog.
- **String catalog** — the per-language collection of translated strings.
- **Accessibility property** — a piece of metadata on an authored primitive that
  contributes to the semantic node and / or to assistive rendering.

**Out of scope.**

- The platform-level assistive APIs themselves — Platform owns the interop seam.
- Visual editing of text — Authoring owns the editor surface.

## Deferred contexts

A context that does not yet exist but is named here so the rest of the design set has somewhere to
point.

- **Networking** — replication, transport, prediction, rollback, lobby, anti-cheat. Will
  become a real context when the Slice-4 committed scope claims it. Until then,
  references in other pillars point to
  [`90-risks-and-open-questions.md`](90-risks-and-open-questions.md). Runtime's tick model
  and Simulation's determinism classes are preconditions any future Networking design
  must satisfy; they are intentionally designed to be Networking-agnostic until the
  context is claimed, so a future commitment does not force a rewrite of either.
