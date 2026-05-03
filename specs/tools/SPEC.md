# Tools Spec

## 1. Purpose

The `tools` context owns one responsibility: the **in-process editor
shell** that lets a human author and replay a glibre project. Concretely
that is the Dear ImGui-driven editor application — its `EditorHost` (one
ECS world dedicated to editor-only state, kept rigorously disjoint from
the running game world it embeds), the dockable panel layout and named
layout profiles, the **scene tree** browser, the **transform gizmo**
(translate / rotate / scale, world / local / parent frames, axis and
plane constraints, configurable snap), the **inspector** auto-generated
from Fory reflection on the type registry, the **asset browser** that
walks the project tree and previews handles surfaced by `content`, the
**play / pause / step** toolbar that toggles ECS scheduling on the
embedded world, the per-action **command stack** (undo / redo with
transactional grouping), and the **trace recorder** that captures
input + assertion ops to a `.glibre-trace` file consumed by the E2E
suite. Tools renders its UI through the engine's Metal 4 backend by
emitting Dear ImGui draw lists into the `RenderFrame` extract that
`render` already consumes (no second renderer); graph-style editors
(logic, material, animation, behavior tree, state machine, render-graph
visualiser) are deferred post-MVP and will use `imgui-node-editor` as
sub-panels under this same shell. Tools **refuses** to own anything
outside that seam: it does not own rendering primitives, swapchains or
GPU resources (those are `render`); it does not own HLSL or material
graph compilation (`shader` and the future `material` plugin); it does
not own simulation — neither physics, animation runtime, nor any other
sim system (`physics`, `sim`, future per-domain plugins); it does not
own domain assets, gameplay logic, ability data, loot tables, quest
graphs, or any other game-framework concept (those live in the future
`game-framework` plugin and are merely **inspected** by tools through
Fory-reflected views); it does not own file I/O policy, asset baking,
LFS, sparse checkout, content-defined chunking, or the project-on-disk
schema (`content` is the sole owner of the project tree and bundle
format); it does not own version control, cloud build, deployment,
shared-cache, marketplace, mod packaging, server infrastructure, AI
backends, voice STT, screen capture, multi-user CRDT collaboration, VR
runtime, or remote streaming — every one of those harmonius requirement
clusters is a separate post-MVP concern outside the MVP tools surface
(and where any survive at all, they live behind their own contexts:
`platform-services`, `cloud`, `collab`, `xr`, etc.). Tools is a faithful
**author-and-replay surface** over the engine; it is the one consumer
of editor-only ECS state and the one producer of `.glibre-trace`
fixtures, and it owns no second tier of domain logic. Per SRP every
reason tools has to change must trace back to one of those listed
responsibilities; anything else routes to the owning context.

The Occam collapse from `PHILOSOPHY.md` applies aggressively here:
harmonius split the editor across `editor-framework`, `editor-plugins`,
`level-editor`, `world-building`, `specialized-editors` (12+ domain
sub-editors), `material-editor`, `animation-editor`, `logic-graph`,
`profiling-tools`, `version-control`, `localization-editor`,
`asset-store`, `vr-editor`, `remote-editing`, `ai-assistant`, `launcher`,
`deployment`, `cloud-build`, `shared-cache`, `documentation`,
`mod-support`, `server-infrastructure`, `ai-cloud-backend`, and
`ai-governance` — twenty-four files. Glibre fuses the **editor shell
itself** (panels, gizmo, inspector, scene tree, asset browser,
play/pause, undo, trace recorder) into one MVP `tools` primitive and
explicitly defers every other concern to its own context (or to
post-MVP) rather than authoring twenty-four sibling sub-modules under
`tools`. The reflection-driven inspector, in particular, replaces every
hand-tailored "ability editor / loot table editor / quest graph editor /
equipment stat editor / economy editor" because every game-framework
type is registered with the type registry and surfaces an inspector for
free.

## 2. Ubiquitous Language

Terms used unchanged in code (identifiers, file names, comments).

| Term | Meaning |
|------|---------|
| `EditorHost` | The in-process editor application. Owns one editor-only ECS `World` and embeds the running game `World`; toggles its scheduling via play/pause. |
| `EditorWorld` | The dedicated ECS world holding editor-only state (selection, gizmo state, layout, undo stack, trace recorder); never serialised into shipping builds. |
| `GameWorld` | The hosted engine world the editor inspects; play-mode ticks its scheduler, edit-mode freezes it. The two worlds share no archetypes. |
| `EditorMode` | Closed sum: `Edit`, `Play`, `Paused`, `Step`, `Recording`. Drives whether the `GameWorld` advances and whether `TraceRecorder` captures ops. |
| `Panel` | One dockable Dear ImGui window the shell hosts (Scene, Inspector, Assets, Console, Profiler, Viewport, …). Identified by a stable string id used in layouts. |
| `Layout` | A serialised dock arrangement: panel positions, sizes, splits, tabs, floats. Versioned JSON; the shell ships defaults and the user names + saves variants. |
| `LayoutProfile` | A named `Layout` (e.g. `default`, `level`, `inspect`); switching is instant and lossless. Profiles persist in user prefs alongside the project. |
| `Viewport` | A `Panel` that draws the engine-rendered `GameWorld` into a render target the editor blits into Dear ImGui via an opaque texture handle from `render`. |
| `SceneTree` | The hierarchy panel listing the `GameWorld`'s scene-graph entities; row click drives `Selection` mutations. |
| `Selection` | The set of currently selected entity ids in the `EditorWorld`; emits `SelectionChanged` on every mutation; consumed by the gizmo and inspector. |
| `Inspector` | The panel that, given the current `Selection`, renders one `InspectorView` per component using Fory-reflected field metadata. |
| `InspectorView` | The auto-generated Dear ImGui form for one component type; produces `EditCommand`s instead of mutating components directly. |
| `ReflectedField` | One row in an `InspectorView`: name + type + read/write closure derived from the type registry's Fory descriptor. |
| `Gizmo` | The on-viewport widget the user manipulates to author transforms; closed sum: `Translate`, `Rotate`, `Scale`. |
| `GizmoFrame` | Reference frame for gizmo-emitted deltas: `World`, `Local`, `Parent`. |
| `GizmoConstraint` | Active axis or plane lock applied to the gizmo: `X`, `Y`, `Z`, `XY`, `XZ`, `YZ`, `Free`. |
| `Snap` | Quantisation rule applied to gizmo output: position step (metres), rotation step (degrees), scale step. Off, single-stage, or per-axis. |
| `AssetBrowser` | The panel that walks the project tree (paths surfaced by `content`), shows handle thumbnails, and drag-drops `AssetHandle`s onto `Inspector` slots and the `Viewport`. |
| `AssetThumbnail` | Cached preview image for one `AssetHandle`; a `render` capture-to-texture pass produces it; the editor only stores and displays. |
| `EditCommand` | One reversible operation against the `GameWorld` (component edit, entity add/remove, parent change, asset slot bind). Carries `apply()` + `undo()`. |
| `CommandStack` | The undo/redo stack of `EditCommand`s in the `EditorWorld`; supports transactional grouping (begin/commit/abort) so multi-entity edits are atomic. |
| `Transaction` | A grouped sequence of `EditCommand`s pushed atomically onto the `CommandStack`; one user-visible undo step. |
| `Toolbar` | The top-of-shell control strip: play / pause / step buttons, mode indicators, snap toggles, gizmo mode, recording status. |
| `PlayPauseStep` | The toolbar trio that drives `EditorMode` transitions on the `GameWorld`'s frame loop. |
| `TraceOp` | One entry in a `.glibre-trace`: typed input event, scheduler tick, or assertion (`expect:component:value`). |
| `TraceRecorder` | The MVP service that, in `Recording` mode, captures all input + selected assertions into a `.glibre-trace` file under `tests/e2e/`. |
| `TraceFile` | The on-disk artifact (`.glibre-trace`) — Fory-archived sequence of `TraceOp`s, mmap-replayable by the E2E runner. |
| `Console` | The log/error panel rendering messages from the engine's diagnostics sink; read-only in MVP. |
| `Profiler` | The panel rendering the engine's GPU timestamp + per-system CPU timing readback; consumes data only, owns no measurement. |
| `Shortcuts` | The keymap binding actions to chords (`Ctrl+Z`, `G`, `R`, `S`, `Space`, …); per-user override layers over a default ring. |
| `EditorEvent` | The typed sum the shell publishes to itself: `SelectionChanged`, `ModeChanged`, `LayoutSwitched`, `TraceStarted`, `TraceStopped`, `CommandPushed`, `CommandUndone`. |
| `ToolsError` | Closed sum of typed failures (`LayoutLoadFailed`, `TraceWriteFailed`, `InspectorUnknownType`, `CommandConflict`, `Refused`). No exceptions cross the boundary. |

## 3. Derived From

Harmonius tooling prior art was mined as **research input only**; every
conclusion below is independently re-derived against `PHILOSOPHY.md`
(SOLID, SRP, plugin-only growth, static codegen, no runtime reflection
in shipping builds, hot-reload at frame boundaries, Occam's razor at
every decision), the engine-wide nine-phase frame schedule
(`reviews/decisions/frame-phases.md`), the Fory codegen pipeline
(`reviews/decisions/fory-codegen.md`), and the §1/§2 commitments above.
Cited paths live under `/Users/cjhowe/Code/harmonius/docs/`. Per
PHILOSOPHY §10 every multi-source concept is collapsed into the
smallest glibre primitive that still satisfies SRP; per PHILOSOPHY §3
any concern that does not trace back to "let a human author and replay
a glibre project through the in-process editor shell" is refused and
routed to the owning context.

### 3.1 Cited harmonius sources

The harmonius tooling cluster is split across **24 requirement files**
(`requirements/tools/*.md`, R-15.1 .. R-15.24) and **22 design files**
(`design/tools/*.md`). The MVP keeps the editor-shell core and routes
or defers everything else; the table below records what was mined and
what was used.

| Cluster | Harmonius file(s) | Used for |
|---------|-------------------|----------|
| Editor framework (kept, **MVP core**) | `requirements/tools/editor-framework.md` (R-15.1.1 .. R-15.1.18), `design/tools/editor-core.md` (§ "Editor framework", "Plugin architecture") | Dockable `Panel` + `Layout` + `LayoutProfile` (R-15.1.1, R-15.1.17), one or more `Viewport` panels with independent cameras (R-15.1.2 — MVP ships **one** viewport, multi-viewport post-MVP), command-pattern `EditCommand` + `Transaction` (R-15.1.3), `Selection` model (R-15.1.4 — MVP ships **click-pick + marquee**, lasso post-MVP), translate / rotate / scale `Gizmo` with `World`/`Local`/`Parent` `GizmoFrame` and `GizmoConstraint` axis/plane locks and `Snap` (R-15.1.5), preference store with versioned JSON (R-15.1.7 — MVP scope = layout profiles only), separate `EditorWorld` ECS isolated from `GameWorld` (R-15.1.11), and idle-frame skip (R-15.1.13, R-15.1.18). |
| Selection model (kept) | `design/tools/selection-model.md` (entire) | `Selection` resource lives in `EditorWorld`; `SelectionChanged` event drives `Gizmo` and `Inspector`; gizmo position derived from selection-aggregate transform; sub-object picking (vertex / edge / face) routed to the future `mesh-edit` plugin, **not MVP**. |
| Undo / redo (kept) | `design/tools/undo-redo.md` (entire) | `EditCommand` interface (`apply`, `undo`, optional `coalesce`, byte budget), `CommandStack` per `EditorWorld`, `Transaction` grouping, ≤ 50 ms apply/undo target, in-memory budget cap with oldest eviction, selection-coupled commands (apply/undo restore selection). Persistent on-disk history and collaborative sync are **deferred** post-MVP. |
| Specialized inspectors (collapsed) | `requirements/tools/specialized-editors.md` (R-15.21.1 entity inspector + R-15.21.5 .. R-15.21.7 quest / loot / ability) | Confirms the entity inspector with searchable component palette + hierarchy nav + undo integration as a first-class shell panel. Quest / loot / ability and other "domain editor" surfaces collapse into the reflection-driven `Inspector` (one `InspectorView` per registered component type, generated from Fory descriptors); see Occam #2. |
| Asset browser (kept, thin) | `requirements/tools/level-editor.md` (R-15.2.1 drag-drop placement, R-15.2.5 spline distribution UX implies asset-handle drop) | `AssetBrowser` panel walks paths surfaced by `content`, drag-drops `AssetHandle`s onto `Inspector` slots and the `Viewport`, displays `AssetThumbnail`s captured by `render`. The non-asset-browser pieces of `level-editor.md` (CSG primitives, terrain sculpt, spline mesh distribution, vegetation paint) are **refused** (see §3.3). |
| Play / pause / step (kept) | `requirements/tools/editor-framework.md` (R-15.1.11 — editor world distinct from game world, implying mode toggle), `design/tools/editor-core.md` ("game-mode toggle") | The `PlayPauseStep` toolbar trio drives `EditorMode` transitions (`Edit`, `Play`, `Paused`, `Step`) on the embedded `GameWorld`'s scheduler. Single-step advances exactly one frame; the `EditorWorld` keeps ticking so the shell remains responsive. |
| Trace recorder (kept) | `requirements/tools/ai-assistant.md` R-15.9.6 (headless editor API + UI automation primitives + CI integration), `design/tools/team-tools.md` (replay / observation hooks), engine-side `specs/e2e/SPEC.md` `.glibre-trace` shape | `TraceRecorder` captures input + scheduler tick + assertion `TraceOp`s into a `.glibre-trace` `TraceFile` consumed by the E2E runner. The MVP collapse: harmonius mixed automation, replay, AI tool-invocation, and headless CI into one R-15.9.6; glibre keeps **only** the deterministic trace recording + replay seam — the AI-driven half is refused (see §3.3). |
| Profiler / console (kept, read-only) | `requirements/tools/profiling-tools.md` (R-15.5.1 .. R-15.5.6), `design/tools/profiler.md` (entire) | `Profiler` panel renders the engine's GPU timestamp ring (already shaped by render R-2.1.12) and per-system CPU timing readback. `Console` panel renders the diagnostics-sink log. Tools owns **display only**; measurement / overhead / telemetry storage all live in `core` and `render`. Network profiling, leak-detection snapshots, and remote-profiling-over-QUIC are **deferred** post-MVP. |
| Plugin / hot-reload integration (kept, thin) | `requirements/tools/editor-plugins.md` (R-15.20.1 .. R-15.20.7), `design/tools/editor-core.md` (§ "Plugin architecture") | Tools is a `.dylib` plugin like every other domain (PHILOSOPHY §3); panels register via the engine's plugin manifest, not a tools-specific extension API. Custom-widget registration for plugin-supplied component types comes free from the Fory-reflected inspector (R-15.20.2). Hot-reload preservation of panel state + undo history follows the engine-wide protocol (`reviews/decisions/hot-reload-protocol.md`); no second hot-reload mechanism in tools (R-15.20.4). Marketplace publication, no-code plugin authoring, and dependency-graph resolution are **refused** (see §3.3). |

Inputs read but **not** adopted as tools responsibilities (collapsed
into other primitives or refused entirely — see §3.2 / §3.3): every
graph-editor cluster (`logic-graph.md` R-15.8 universal graph runtime,
`material-editor.md` R-15.3 shader-graph authoring, `animation-editor.md`
R-15.4 timeline + curve + state-machine + blend-space, the
graph-shaped half of `specialized-editors.md` R-15.21.2 .. R-15.21.4
animation-graph / behavior-tree / state-machine, `visual-editors.md`
the entire design file), the world-authoring cluster (`level-editor.md`
R-15.2 CSG / spline-mesh / terrain-paint, `world-building.md` R-15.6
sculpt / erosion / water / vegetation / probe placement,
`level-world.md` design), the team / collaboration cluster
(`version-control.md` R-15.10, `remote-editing.md` R-15.12,
`scene-versioning.md` design, `team-tools.md` design), the cloud /
infrastructure cluster (`cloud-build.md` R-15.24, `shared-cache.md`
R-15.11, `server-infrastructure.md` R-15.18, `build-deploy.md` design,
`deployment.md` R-15.14, `launcher.md` R-15.15), the marketplace /
mod cluster (`asset-store.md` R-15.17, `mod-support.md` R-15.16,
`plugin-marketplace.md` design), the AI cluster (`ai-assistant.md`
R-15.9 voice + LLM-tool-call, `ai-cloud-backend.md` R-15.23,
`ai-governance.md` R-15.7), the localization / docs cluster
(`localization-editor.md` R-15.13, `documentation.md` R-15.19,
`content-services.md` design), and the XR / immersive cluster
(`vr-editor.md` R-15.22, R-15.1.9 VR-mode subset of editor-framework).

### 3.2 Occam collapses (multiple harmonius concepts → one glibre primitive)

1. **Twenty-four harmonius tooling sub-files → one `EditorHost`
   primitive with eight named sub-parts.** Harmonius shipped 24
   independent requirement clusters under `tools/`, each with its own
   panel, command vocabulary, and lifecycle: `editor-framework`,
   `editor-plugins`, `level-editor`, `world-building`,
   `specialized-editors` (folding 12+ domain sub-editors of its own),
   `material-editor`, `animation-editor`, `logic-graph`,
   `profiling-tools`, `version-control`, `localization-editor`,
   `asset-store`, `vr-editor`, `remote-editing`, `ai-assistant`,
   `launcher`, `deployment`, `cloud-build`, `shared-cache`,
   `documentation`, `mod-support`, `server-infrastructure`,
   `ai-cloud-backend`, `ai-governance`. Glibre fuses the **shell
   itself** into one `EditorHost` MVP primitive with eight named
   sub-parts: dockable `Panel`/`Layout` system, `SceneTree`, `Gizmo`,
   reflection-driven `Inspector`, `AssetBrowser`, `PlayPauseStep`
   toolbar, `CommandStack` (undo / redo / `Transaction`), and
   `TraceRecorder`. Every other cluster either collapses into one of
   those eight (see #2 below), routes to a different context (§3.3),
   or defers post-MVP. Justification: SOLID-SRP + PHILOSOPHY §10 — one
   reason to change the editor shell is "the shell's panel /
   inspector / gizmo / replay seam evolves"; everything else has a
   different reason-to-change and lives elsewhere.

2. **All twelve "domain editors" → one Fory-reflected `Inspector`.**
   Harmonius `requirements/tools/specialized-editors.md` (R-15.21.1
   .. R-15.21.7) and the per-domain editors (`material-editor.md`
   R-15.3.4 parameter inspector, `animation-editor.md` R-15.4.x track
   parameter panels, the loot / ability / quest / equipment / economy
   editors implied by R-15.21.5 .. R-15.21.7) all reduce in glibre
   to **one** `Inspector` panel populated by `InspectorView`s
   auto-generated from the type registry's Fory descriptors. Every
   game-framework type (ability, loot table, quest node, stat block,
   economy entry, dialogue line, behaviour-tree node parameters,
   animation-state condition expression) is a registered component
   type whose fields surface for free as `ReflectedField`s in an
   `InspectorView` — no hand-tailored editor per domain. Justification:
   PHILOSOPHY §6 (static codegen, zero runtime reflection in shipping
   builds — Fory codegen runs at build time per
   `reviews/decisions/fory-codegen.md`, the editor consumes the
   generated descriptors) + §10 (Occam) — one inspector implementation
   replaces twelve.

3. **Translate / rotate / scale gizmos → one `Gizmo` closed sum.**
   Harmonius R-15.1.5 named translate, rotate, and scale as separate
   tool features each with their own snap, frame, and constraint
   semantics. Glibre collapses these to one `Gizmo` value type
   (`Translate | Rotate | Scale`) parameterised by `GizmoFrame`
   (`World | Local | Parent`), `GizmoConstraint`
   (`X | Y | Z | XY | XZ | YZ | Free`), and `Snap` (position step,
   rotation step, scale step). One on-viewport widget, one drag-loop
   state machine, one set of `EditCommand`s emitted into the
   `CommandStack`. Justification: SOLID-SRP — the shape of every
   gizmo interaction is the same delta + frame + constraint + snap
   pipeline; three separate implementations would share 90% of code.

4. **`SelectionState`/`SelectionChanged`/marquee/lasso/sub-object →
   one `Selection` value type + one event.** Harmonius
   `selection-model.md` enumerated per-world selection state, marquee
   (R-15.1.4.5), lasso (R-15.1.4.6), gizmo coupling (R-15.1.4.7), and
   selection-changed events (R-15.1.4.8) as separate features. Glibre
   collapses them to one `Selection` value (set of entity ids) in the
   `EditorWorld` and one `EditorEvent::SelectionChanged`. Marquee
   click-pick is MVP; lasso and sub-object (vertex / edge / face)
   selection are **deferred** to a future `mesh-edit` plugin and do
   not exist on the tools MVP surface. Justification: PHILOSOPHY §5
   (greatly reduced MVP scope) — one event consumer (gizmo +
   inspector) needs one event producer.

5. **Layout + multi-monitor + per-DPI + idle-skip + frosted-glass →
   one versioned `Layout` JSON + one render-graph dependency.**
   Harmonius R-15.1.1, R-15.1.13 .. R-15.1.18 enumerated dock layouts,
   per-monitor DPI, multi-monitor span, dirty-region partial redraw,
   panel-occlusion masking, and frosted-glass blur as separate shell
   features. Glibre collapses to one `LayoutProfile` (versioned JSON
   per R-15.1.7) plus one observation: tools renders through the
   engine's existing Metal 4 backend by emitting Dear ImGui draw
   lists into the `RenderFrame` (`render` already owns the scheduler,
   the present pass, dirty extract, and any post-FX); idle-skip,
   partial redraw, occlusion-mask, and panel-blur are not new features
   in tools — they are render-side optimisations that fall out of
   the existing render-graph contract. Justification: SOLID-SRP +
   PHILOSOPHY §3 (plugin-only growth) — adding a second renderer or
   second hot-loop to support these knobs would violate "minimal
   core, plugin-only growth" without adding any tools-distinct
   capability.

6. **`EditorCommand` + `Transaction` + `UndoStack` + on-disk history
   + collaborative-undo-sync → one `CommandStack` + one
   `Transaction` grouper.** Harmonius `undo-redo.md` enumerated eight
   concerns (trait, stack, transactions, memory budget, on-disk
   mirror, latency target, selection coupling, collaborative sync).
   Glibre keeps the first four as the MVP `CommandStack` + `Transaction`
   API; on-disk history mirroring and collaborative undo sync are
   refused (the latter is a `collab` concern, the former a post-MVP
   convenience). Latency target (≤ 50 ms apply / undo) survives as a
   §9 perf budget item. Selection coupling is automatic because
   `EditCommand`s carry pre / post `Selection` snapshots. Justification:
   PHILOSOPHY §10 — eight features collapse into one stack + one
   grouper at MVP; the others re-enter post-MVP without redesigning
   the core.

7. **Headless editor + UI automation + AI tool-invocation + concurrent
   isolated agents + CI integration (R-15.9.6) → one `TraceRecorder`
   + one `TraceFile`.** Harmonius mixed automation, replay, AI
   tool-invocation, and CI in one requirement. Glibre keeps **only**
   the deterministic trace seam: `TraceRecorder` captures input +
   scheduler tick + assertion `TraceOp`s into `.glibre-trace` files
   (Fory-archived); the E2E runner (`specs/e2e/SPEC.md`) replays them
   under the same scheduler. AI-driven editor automation, voice-STT,
   LLM-tool-call exposure of editor actions, and concurrent isolated
   agent worlds are **refused** (§3.3) — they are post-MVP and would
   live behind a separate `ai-assist` plugin authored against the
   same `TraceOp` vocabulary. Justification: PHILOSOPHY §5 (greatly
   reduced MVP scope) + §10 — the deterministic-replay seam is the
   minimum that closes the §1 responsibility "one producer of
   `.glibre-trace` fixtures"; everything beyond it is additive and
   plugin-shaped.

8. **Profiler + console + stat overlay + remote-profiling +
   network-profiler + leak-snapshot → one read-only `Profiler` panel
   + one read-only `Console` panel.** Harmonius
   `requirements/tools/profiling-tools.md` (R-15.5.1 .. R-15.5.6) and
   `design/tools/profiler.md` enumerated CPU flame graph, GPU
   per-pass timing + occupancy, memory treemap with call-stack capture,
   leak-detection snapshots, network bandwidth + packet inspector,
   stat overlays on viewport, CSV recording, and remote-profiling
   over QUIC. Glibre collapses MVP to two read-only panels:
   `Profiler` (renders the GPU timestamp ring + per-system CPU timing
   readback already produced by `core` and `render`) and `Console`
   (renders the diagnostics-sink log already produced by `core`).
   Tools owns **display only**; instrumentation, overhead budgets,
   leak heuristics, network telemetry, and remote streaming are all
   refused (§3.3). Justification: SOLID-SRP — "measure" and
   "visualise" are different reasons-to-change; the measurement seam
   already exists in `core` + `render`, so the tools side is one
   reader, not a parallel pipeline.

9. **R-15.1.11 EventBridge + R-15.1.8 plugin API + R-15.20.4
   plugin-hot-reload-state-preservation → engine-wide ECS event bus
   + engine-wide plugin manifest + engine-wide hot-reload protocol.**
   Harmonius wanted a tools-specific EventBridge between editor and
   game worlds, a tools-specific stable-ABI plugin API, and a
   tools-specific reflection-based hot-reload-state-preservation
   mechanism. Glibre uses **the engine's** ECS event bus, **the
   engine's** plugin manifest (PHILOSOPHY §3 — every domain is a
   plugin), and **the engine's** hot-reload protocol
   (`reviews/decisions/hot-reload-protocol.md`); tools is one
   plugin among many and inherits all three for free. The `EditorWorld`
   isolation requirement (R-15.1.11) becomes a value: tools owns one
   `World` instance configured at plugin init, distinct from any
   game `World` registered later. Justification: PHILOSOPHY §3 +
   §8 — duplicating these mechanisms inside tools would be a
   per-domain reinvention, exactly the anti-pattern the philosophy
   rejects.

### 3.3 Refusals (routed to other contexts or deferred post-MVP)

Glibre's tools plugin does **not** own any of the following, even
though harmonius collected them under "editor / tools / cloud /
collab". Each routes to the owning context per §1, or is explicitly
deferred behind PHILOSOPHY §5 (greatly reduced MVP scope).

| Harmonius surface | Cited file(s) | Routed to / deferred |
|-------------------|----------------|----------------------|
| Logic / gameplay graph editor (typed visual programming, AOT compile, multi-frame coroutines, validation, debug step-through) | `requirements/tools/logic-graph.md` (R-15.8.1 .. R-15.8.13), `design/tools/visual-editors.md` (Logic Graph Runtime sections) | **Refused** (post-MVP). Glibre uses C++23/26 plugins as the gameplay-authoring substrate (PHILOSOPHY §6 — static codegen, zero runtime reflection in shipping). When/if a logic graph re-enters, it returns as its own `logic-graph` plugin that hosts its node-editor sub-panels under the tools MVP shell using `imgui-node-editor`; tools merely supplies the dock slot. |
| Shader / material graph editor (typed pin DAG, real-time preview, function subgraphs, parameter inspector, instance variation) | `requirements/tools/material-editor.md` (R-15.3.1 .. R-15.3.6), `requirements/tools/logic-graph.md` R-15.8.5 (shader graph variant), `design/tools/visual-editors.md` (Shader Graph sections) | **Refused** (post-MVP). HLSL authoring + AIR / metallib compilation lives in the `shader` plugin; the eventual material-graph authoring UI lives in the future `material` plugin and re-uses `imgui-node-editor` as a sub-panel under the tools shell. |
| Animation editor (timeline, curve editor, skeleton viewer, blend space, animation state machine, retargeting UX) | `requirements/tools/animation-editor.md` (R-15.4.1 .. R-15.4.x), `design/tools/visual-editors.md` (animation sections), `requirements/tools/specialized-editors.md` R-15.21.2 .. R-15.21.4 | **Refused** (post-MVP). Animation runtime + retargeting are owned by the future `animation` plugin (a `sim` peer); its authoring UI re-enters as an animation-plugin sub-panel under the tools shell. |
| Behaviour tree / quest graph / state-machine / dialogue / ability-combo graph editors | `requirements/tools/specialized-editors.md` R-15.21.3 .. R-15.21.7, `design/tools/visual-editors.md` (state-machine + behavior-tree + quest sections) | **Refused** (post-MVP). The execution runtimes for these are gameplay-framework concerns, not tools concerns; their graph editors re-enter as `game-framework` plugin sub-panels. The data fields that those graphs reference are already inspectable via the Fory-reflected `Inspector` (Occam #2). |
| Level-editor world-authoring tools (CSG additive / subtractive primitives + boolean ops, terrain sculpt brushes, hydraulic / thermal erosion, terrain material painting, spline-mesh distribution, foliage / vegetation paint, biome rules, water bodies, light / reflection probe placement) | `requirements/tools/level-editor.md` (R-15.2.1 entity-placement / drag-drop is **kept** in the asset browser; R-15.2.2 .. R-15.2.7 are refused), `requirements/tools/world-building.md` (R-15.6.1 .. R-15.6.x), `design/tools/level-world.md` (entire) | **Refused** (post-MVP). World-authoring is the future `world-edit` plugin (a `tools`-shaped peer that loads its own panels under the same shell). The MVP tools shell only provides drag-and-drop of `AssetHandle`s onto entity inspectors and the viewport — no brushes, no procedural rules, no terrain. |
| Visual render-graph editor / pipeline configuration | `requirements/tools/logic-graph.md` R-15.8.6 | **Refused** (forever). PHILOSOPHY anti-pattern §3 explicitly rejects "serialized render-graph files. Render graph is C++ code, visualized live by the editor." Glibre's tools shell exposes a **read-only** render-graph diagnostic overlay (sourced from render's existing `DiagnosticOverlay` per render §3 Occam #10); it never authors graph topology. |
| Version control UI (Git client, LFS auto-tracking, structural three-way merge driver, branch-per-feature workflow, presence indicators, sparse checkout, file lock / unlock) | `requirements/tools/version-control.md` (R-15.10.1 .. R-15.10.6), `design/tools/scene-versioning.md` (entire) | **Refused** (post-MVP, then routes to a `vc-ui` plugin). Version control as a workflow lives outside the engine; the merge-driver concern is handled by `data` (Fory-aware `SceneDiff` over registered component types) when it lands. The MVP shell does not expose Git. |
| Real-time collaborative editing (CRDT scene sync, per-user undo, presence indicators, headless GPU servers, follow / observe mode) | `requirements/tools/remote-editing.md` (R-15.12.1 .. R-15.12.7), `design/tools/team-tools.md` (entire) | **Refused** (post-MVP). Routes to a future `collab` context with its own protocol (CRDT layer sits below the tools shell — collab merges into the same `EditorWorld` ECS and tools observes via the same `EditorEvent` stream). The MVP tools shell is single-user. |
| Cloud build / shared build cache / signing / OCI containers / self-hosted CI / build-farm / S3 artifact storage / CDN | `requirements/tools/cloud-build.md` (R-15.24.1 .. R-15.24.x), `requirements/tools/shared-cache.md` (R-15.11.1 .. R-15.11.x), `requirements/tools/server-infrastructure.md` (R-15.18.1 .. R-15.18.x), `design/tools/build-deploy.md` (entire) | **Refused** (post-MVP). Routes to a future `cloud` context. Local cooks and local builds run from the host shell + CMake / `xcrun`; tools merely surfaces a "rebuild" button that shells out, not a cloud client. |
| Deployment (platform packaging, deploy-to-device, certification compliance, code signing, native installers, DLC / asset bundle packaging, delta patches, store distribution) | `requirements/tools/deployment.md` (R-15.14.1 .. R-15.14.x), `design/tools/build-deploy.md` (build-deploy section) | **Refused** (post-MVP). Routes to a future `platform-services` plugin per OS target. The MVP tools shell does not deploy; `platform` already covers swapchain / windowing / input / pacing per `specs/platform/SPEC.md`. |
| Engine launcher (multi-version install, project browser, genre templates, file-association handler, synced preferences, multi-account, collab-setup wizard) | `requirements/tools/launcher.md` (R-15.15.1 .. R-15.15.x) | **Refused** (post-MVP). Launcher is a separate executable, not part of the editor shell; routes to a future `launcher` context (or stays manual). The MVP shell is invoked directly on a project directory passed at startup. |
| Asset marketplace (in-editor browser, ratings / reviews / curation, publisher dashboards, regional pricing, automated compatibility CI, revenue sharing) | `requirements/tools/asset-store.md` (R-15.17.1 .. R-15.17.x), `design/tools/plugin-marketplace.md` (entire) | **Refused** (post-MVP). Routes to a future `marketplace` context; the MVP shell installs no remote content. The `AssetBrowser` walks **only** the local project tree surfaced by `content`. |
| Mod support (mod SDK, asset-type / node-access / memory / entity budgets, signed mod bundles, sandboxed ECS partitions, Workshop integration, moderation dashboard) | `requirements/tools/mod-support.md` (R-15.16.1 .. R-15.16.x) | **Refused** (post-MVP). Routes to a future `mod` context built on top of the marketplace once both exist. |
| AI assistant (voice STT, LLM tool-invocation of every editor action, ghost-node graph suggestions, AI texture / mesh / level / dialogue generation, persistent chat panel, per-project provider selection, cost dashboard) | `requirements/tools/ai-assistant.md` R-15.9.1 .. R-15.9.5 (R-15.9.6 trace-recorder half is **kept** per Occam #7), `requirements/tools/ai-cloud-backend.md` (R-15.23.1 .. R-15.23.x), `requirements/tools/ai-governance.md` (R-15.7.1 .. R-15.7.x) | **Refused** (post-MVP). Routes to a future `ai-assist` plugin authored against the same `TraceOp` vocabulary as the trace recorder; tools merely supplies the dock slot and read-only chat panel when/if it lands. |
| VR / immersive editor (stereoscopic editor mode, head + 6-DoF motion-controller input, optical hand tracking, spatial UI / radial menus, collaborator avatars + spatial audio, follow mode) | `requirements/tools/vr-editor.md` (R-15.22.1 .. R-15.22.x), `requirements/tools/editor-framework.md` R-15.1.9 | **Refused** (post-MVP). Routes to a future `xr` context; the MVP shell is desktop + Metal 4 only. |
| Localization editor (string-table CRUD, ICU pluralisation, CSV / XLIFF import / export, translation memory, locale-preview validation, RTL / pseudo-loc, TMS integration) | `requirements/tools/localization-editor.md` (R-15.13.1 .. R-15.13.x), `design/tools/content-services.md` (localization section) | **Refused** (post-MVP). Routes to a future `localization` plugin; MVP project authoring is in one locale. |
| Documentation / learning (auto-generated API reference, in-editor tutorials with overlays, embedded video player, contextual F1 help, sample-project gallery, doc-example CI) | `requirements/tools/documentation.md` (R-15.19.1 .. R-15.19.x), `design/tools/content-services.md` (documentation section) | **Refused** (post-MVP). Routes to an out-of-engine documentation pipeline; the MVP shell offers no in-editor help system. |
| Profiler measurement / overhead budget / network telemetry / leak detection / remote profiling | `requirements/tools/profiling-tools.md` R-15.5.1 (overhead budget), R-15.5.3 (call-stack capture), R-15.5.4 (leak snapshots), R-15.5.5 (network), R-15.5.7 (remote-profiling-over-QUIC) | **Routed.** Measurement + budgets live in `core` (CPU instrumentation) and `render` (GPU timestamp ring per render-spec §3 Occam #10). Network telemetry, leak detection, and remote-profiling-over-QUIC are post-MVP. Tools owns **display only**, per Occam #8. |
| Plugin marketplace / dependency-graph resolver / no-code plugin authoring | `requirements/tools/editor-plugins.md` R-15.20.5 .. R-15.20.7, `design/tools/plugin-marketplace.md` (entire) | **Refused.** The engine plugin manifest + dylib hash + load-time refusal are owned by `core` per PHILOSOPHY §3 + §9; tools is one plugin loaded via that mechanism, not the loader itself. Marketplace publication routes to the future `marketplace` context; no-code plugin authoring is refused (tools edits Fory-described data, not engine plugins). |
| Engine-wide event bus, plugin manifest, hot-reload protocol, ECS scheduler | `requirements/tools/editor-framework.md` R-15.1.8 + R-15.1.11, `requirements/tools/editor-plugins.md` R-15.20.4 | **Routed to `core`.** Tools registers itself + its panels via the engine plugin manifest, publishes / subscribes via the engine's typed `EditorEvent` bus, and inherits the engine-wide hot-reload protocol per `reviews/decisions/hot-reload-protocol.md`. No tools-specific EventBridge, plugin loader, or hot-reload mechanism. |
| Asset baking / cooking / thumbnails capture / project-on-disk schema / sparse checkout / content-defined chunking / LFS | implied across `requirements/tools/level-editor.md` (asset import), `requirements/tools/asset-store.md`, `requirements/tools/cloud-build.md`, `requirements/tools/shared-cache.md` | **Routed to `content`.** The `AssetBrowser` walks **paths surfaced by `content`** and renders thumbnails captured by `render`'s capture-to-texture path; tools owns no I/O policy and no on-disk schema. |
| Window / swapchain / input pump / multi-monitor / DPI / frame pacing | `requirements/tools/editor-framework.md` R-15.1.16 (multi-monitor + DPI), R-15.1.18 (idle-mode CPU/GPU drop) | **Routed to `platform`.** Tools rides on `platform`'s window + input + pacing per `specs/platform/SPEC.md`; the multi-monitor / DPI / idle-mode behaviours are platform-side responsibilities. |

These refusals are the application of PHILOSOPHY §1 (SOLID-SRP) + §3
(minimal core, plugin-only growth) + §5 (greatly reduced MVP scope) +
§10 (Occam) to the harmonius "tools" umbrella: anything whose
reason-to-change does not collapse to "the in-process editor shell —
panels, tree, gizmo, inspector, asset browser, play / pause, undo,
trace recorder — evolves" lives in another plugin or returns
post-MVP, never as a sibling sub-module under `tools`.

## 4. Aggregates & Invariants

- Aggregate / entity / value object.
- Invariants that must hold at every public API boundary.

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
