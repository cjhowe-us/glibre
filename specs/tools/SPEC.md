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

This section enumerates the aggregates, entities, and value objects the
`tools` context owns, the invariants that must hold at every public API
boundary, and the SRP justification for each. The set is closed:
nothing outside this list is owned by `tools` (per §1 and the refusals
in §3.3); every Occam collapse cited from §3.2 is honoured by collapsing
into one named aggregate rather than authoring sibling sub-modules.
Each aggregate is the smallest unit that preserves an invariant cluster
atomically; every public API call enters and exits with all invariants
holding. Failures are signalled through `std::expected<T, glibre::Error>`
per `reviews/decisions/error-model.md`; the `tools::Error` arms named in
§2 are surfaced alongside each invariant cluster.

The roster is grouped by responsibility: the host root (§4.1), the
panel/layout substrate (§4.2), scene navigation + selection (§4.3), the
reflection-driven inspector (§4.4), the on-viewport gizmo (§4.5), the
asset browser (§4.6), the undo/redo command stack (§4.7), the toolbar
play/pause/step controls (§4.8), and the trace recorder (§4.9). The
cross-aggregate invariants that span those seams are collected in §4.10.

### 4.1 `EditorHost` — root of the in-process editor shell (aggregate root)

**Reason to change:** the boundary between editor-only ECS state and the
embedded `GameWorld` it inspects (§3.2 collapse #1 — twenty-four
harmonius tooling sub-files collapse into this one root with eight
named sub-parts). Distinct from any single panel's body, any single
inspector view, or the trace recorder's wire format.

**Composition.** Owns exactly two ECS `World` instances:

- The **`EditorWorld`** — a dedicated `core::World` configured at
  plugin init holding only editor-only resources (`Layout`,
  `LayoutProfile` slot map, `Selection`, `CommandStack`, `Gizmo` state,
  `TraceRecorder`, `Toolbar` state, `Shortcuts` keymap). Never
  serialised into shipping builds; never participates in the
  game-world frame schedule.
- The embedded **`GameWorld`** — a borrowed reference to the
  engine's game `core::World` (the one that ships in the runtime).
  `EditorHost` toggles its scheduling via `EditorMode` transitions
  but never registers editor-only components onto it.

Plus the closed-sum `EditorMode` resource (`Edit | Play | Paused | Step
| Recording`), the `EditorEvent` typed bus (republished from `core`'s
event bus, not a second bus per §3.2 collapse #9), and a back-pointer to
`render`'s `RenderFrame` extract slot for the per-frame Dear ImGui draw
list emit.

**Identity & lifetime.** One `EditorHost` per editor-process invocation.
Constructed once at `glibre_plugin_register` time; destroyed at plugin
unload. Survives hot-reload of any other plugin per the engine-wide
hot-reload protocol; its own dylib swap follows `core`'s
drain → swap → migrate → resume cycle.

**Public-boundary invariants.**

1. **World disjointness.** `EditorWorld` and the embedded `GameWorld`
   share no `Archetype`, no `Resource<T>` slot, and no `Entity`
   namespace; an `Entity` minted in one rejects with
   `core::Error::EntityForeignWorld` if passed to the other. No
   editor-only component type ever appears in the game world's type
   registry.
2. **Mode-gated scheduling.** The embedded `GameWorld`'s `FrameLoop`
   advances only when `EditorMode ∈ { Play, Step, Recording }`; in
   `Edit | Paused` modes its scheduler does not tick (the editor
   world keeps ticking so the shell stays responsive). `Step`
   advances exactly one game-world frame and then transitions to
   `Paused` (single-frame quantum honoured by phase 9, per
   `reviews/decisions/frame-phases.md`).
3. **Mode-transition atomicity.** `EditorMode` changes only at the
   phase-9 boundary of the editor world's frame; no game-world phase
   observes a mode change mid-frame. Transitions emit
   `EditorEvent::ModeChanged` synchronously after the boundary.
4. **One `RenderFrame` consumer.** `EditorHost` emits Dear ImGui draw
   lists into the `render` context's per-frame extract slot exactly
   once per editor frame; no second renderer or second extract path
   exists. The editor never owns swapchains, command buffers, or
   PSOs (refused by §3.3 → routed to `render` / `platform`).
5. **No engine-wide singletons inside the host.** `EditorHost` never
   holds a static reference to any non-editor service; every
   collaboration with `core`, `render`, `content`, `platform`, or
   `data` flows through their public boundaries by handle.

**SRP justification — single reason to change:** the rules that govern
how the editor shell co-exists with the embedded game world. If world
disjointness, mode gating, or the single-extract contract changes,
`EditorHost` changes; nothing else does. Every other aggregate in this
roster has its own narrower reason-to-change.

### 4.2 `Layout` / `LayoutProfile` / `Panel` / `Viewport` (aggregate)

**Reason to change:** the dock-arrangement substrate — how panels are
positioned, named, persisted, and reflowed (§3.2 collapse #5 collapses
dock layouts, multi-monitor, per-DPI, idle-skip, partial redraw, and
frosted-glass into one versioned JSON `Layout` plus existing render
optimisations). Distinct from any specific panel's body content, which
each owns its own narrower aggregate (§4.3–§4.9).

**Composition.** A `Layout` is a value object: a versioned JSON
descriptor naming dock splits, panel positions, sizes, tab groups,
floating windows, and active tabs, keyed by stable `Panel` ids. A
`LayoutProfile` is a named `Layout` (e.g. `default`, `level`,
`inspect`); the host owns a slot map of profiles in `EditorWorld`.
A `Panel` is one dockable Dear ImGui window identified by a stable
string id, registered into the host at construction time with its
draw-callback closure. A `Viewport` is the special `Panel` subtype
that draws the engine-rendered `GameWorld` into a render-target the
editor blits via an opaque texture handle from `render` (multi-viewport
deferred per §3.1, MVP ships exactly one).

**Identity & lifetime.** `LayoutProfile` instances persist across
sessions in the user-prefs JSON adjacent to the project; `Layout`
values are loaded into memory on profile activation. `Panel`
registrations live for the lifetime of the registering plugin (panels
survive hot-reload of *other* plugins; a panel from a swapped-out
plugin is unregistered during the drain phase per §8). Only one
`LayoutProfile` is active at a time; switching is atomic and lossless.

**Public-boundary invariants.**

1. **Stable `Panel` ids.** Each `Panel` registers a
   compile-time-stable string id; ids are unique within the host.
   Re-registering an id is rejected with
   `tools::Error::CommandConflict` (a panel-id collision is
   structurally identical to a command conflict — same closed-sum
   refusal arm). Layouts reference panels by id; an unknown id
   inside a loaded `Layout` is rejected with
   `tools::Error::LayoutLoadFailed` and the previous active layout
   is preserved.
2. **Versioned JSON, monotonic schema.** `Layout`'s on-disk schema
   carries a `schema_version` integer; loaders accept the current
   version and any older version reachable by the migration table
   in `data`. An unknown future version refuses load; a malformed
   document refuses load. No partial layout is ever applied.
3. **Lossless profile switch.** Activating a different
   `LayoutProfile` either fully applies its `Layout` (every panel
   visible per the layout, every dock split materialised) or
   leaves the previous layout intact and returns
   `tools::Error::LayoutLoadFailed`; no half-applied dock state
   is observable.
4. **One `Viewport` panel in MVP.** Exactly one `Viewport` is
   registered; multi-viewport with independent cameras is deferred
   to a post-MVP plan and refuses to register a second viewport
   in MVP with `tools::Error::Refused`.
5. **Draw lists routed through `render`.** Every `Panel`'s draw
   callback emits Dear ImGui geometry into the host-owned draw-list
   buffer that flows into `render`'s `RenderFrame` extract; no
   panel directly touches Metal, swapchains, or GPU resources
   (PHILOSOPHY §3 — refused by §3.3, routed to `render`).

**SRP justification:** the rules of how panels persist and reflow.
Changes to dock-layout JSON shape, profile-switch semantics, or panel
registration discipline move this aggregate; everything else has a
distinct reason to change.

### 4.3 `SceneTree` + `Selection` (aggregate)

**Reason to change:** how the user navigates the `GameWorld`'s
hierarchy and which entities the inspector and gizmo will operate on
(§3.2 collapse #4 — selection state, marquee, lasso, gizmo coupling,
and selection-changed events collapse into one `Selection` value plus
one event). Distinct from how the inspector renders (§4.4) or how the
gizmo authors transforms (§4.5).

**Composition.** `SceneTree` is the panel that walks the embedded
`GameWorld`'s `ChildOf` forest (using `core`'s parent/child relationship
index) and renders one row per entity; selection is *not* owned here —
clicking a row mutates `Selection`. `Selection` is a resource owned by
`EditorWorld`: a deterministically-ordered set of `Entity` ids
referencing the `GameWorld`. Every mutation publishes
`EditorEvent::SelectionChanged` synchronously. MVP scope: click-pick +
marquee; lasso + sub-object (vertex/edge/face) selection are routed to
the future `mesh-edit` plugin per §3.3.

**Identity & lifetime.** `SceneTree` is one of the registered `Panel`s
(§4.2). `Selection` lives across editor frames as a single resource in
`EditorWorld`; it survives hot-reload of plugins other than `tools`.
Pre/post snapshots of `Selection` are captured by every `EditCommand`
(§4.7) so undo/redo restore selection automatically.

**Public-boundary invariants.**

1. **`Selection` references the `GameWorld` only.** Every `Entity` in
   `Selection` resolves through the `GameWorld`'s entity allocator;
   stale handles (entity despawned by play-mode logic) are scrubbed
   at the next selection-event boundary and emit
   `SelectionChanged`. No `EditorWorld` entities ever appear in
   `Selection`.
2. **Deterministic order.** Iteration order of `Selection` is
   insertion-stable per session; the inspector and gizmo see the
   same ordering on every read (PHILOSOPHY §7). Cross-session
   ordering is not promised — selection does not persist to disk.
3. **Single producer.** Only `SceneTree` row clicks, viewport-pick
   clicks, marquee-rect commits, and explicit `EditCommand` undo /
   redo restoration mutate `Selection`. Every other path is
   read-only.
4. **Event coalescing.** Multi-entity marquee commits publish
   exactly one `SelectionChanged` event after the rectangle is
   released, not one per added entity; consumers see one
   coherent snapshot.
5. **Drag-and-drop reparenting refused here.** Re-parenting an
   entity in the tree pushes an `EditCommand` (§4.7); `SceneTree`
   never mutates `ChildOf` directly, preserving undo correctness.

**SRP justification:** the rules of "what is selected and how does
the rest of the editor know". The shape of the navigation row (icons,
filters, search) is presentational and lives inside the panel
implementation; selection semantics live here.

### 4.4 `Inspector` + `InspectorView` + `ReflectedField` (aggregate)

**Reason to change:** how the `Selection`'s components surface as
editable forms — the reflection-driven mapping from Fory descriptors
to Dear ImGui rows (§3.2 collapse #2 — twelve harmonius "domain
editors" collapse into this one aggregate). Distinct from how edits
become reversible operations (§4.7) and from the type registry itself
(owned by `core`).

**Composition.** `Inspector` is the registered panel that, given the
current `Selection`, walks the components present on each selected
entity and instantiates one `InspectorView` per `(Entity, ComponentType)`
pair. `InspectorView` is a value object derived from the type
registry's Fory descriptor for one component type: a list of
`ReflectedField` entries plus a Dear ImGui draw closure. A
`ReflectedField` is one row: name, type tag, read-closure (returns the
current value via the Fory descriptor's `read_field(blob, offset)`),
and write-closure (produces an `EditCommand` rather than mutating the
component directly). Reads flow through a `ReflectionBlob` — an
opaque, immutable byte view obtained from the registry — and never
through raw component pointers.

**Identity & lifetime.** `InspectorView` instances are constructed per
panel-frame from cached Fory descriptors; descriptors themselves are
codegen-emitted at build time (PHILOSOPHY §6) and live in static
storage. `Inspector` itself is one `Panel`.

**Public-boundary invariants.**

1. **Read via `ReflectionBlob`, never raw pointers.** Every
   `ReflectedField::read()` resolves through the registry's
   `ReflectionBlob` accessor; the inspector aggregate has no
   `T*`-typed references to `GameWorld` storage. Type-mismatch on
   blob access is a build-time error in the codegen.
2. **Writes produce `EditCommand`s, never direct mutations.** Every
   `ReflectedField::write(...)` returns an `EditCommand` value;
   committing it to `CommandStack` is the only path that touches
   `GameWorld` storage. The inspector itself never calls
   `world.set<T>(...)`.
3. **Closed type-registry domain.** A `ReflectedField` exists only
   for a `(ComponentType, FieldName)` pair the type registry
   knows; an unknown component type or field returns
   `tools::Error::InspectorUnknownType` and the row is omitted
   (no half-rendered form).
4. **Deterministic field order.** Fields are rendered in the order
   declared by the Fory descriptor; that order is itself a
   compile-time constant per `reviews/decisions/fory-codegen.md`.
   No runtime sort.
5. **Plugin-supplied custom widgets opt-in.** A plugin may register
   a custom widget for a `(ComponentType, FieldName)` pair via the
   engine plugin manifest; absent that registration, the default
   reflection-driven row is used. Custom widgets still emit
   `EditCommand`s — no widget bypasses the command stack.

**SRP justification:** the rules of mapping reflected types to
editable rows. If Fory descriptor shape changes, the read-closure
shape changes, or the inspector grows new default widget kinds, this
aggregate changes; everything else does not.

### 4.5 `Gizmo` + `GizmoFrame` + `GizmoConstraint` + `Snap` (aggregate)

**Reason to change:** how on-viewport authoring of transforms turns
into reversible delta operations (§3.2 collapse #3 — translate /
rotate / scale gizmos collapse into one closed sum parameterised by
frame, constraint, and snap). Distinct from the inspector (§4.4) and
from the command stack (§4.7).

**Composition.** `Gizmo` is the closed sum
(`Translate | Rotate | Scale`) with one drag-loop state machine and
one widget-render path. `GizmoFrame` is the closed sum of reference
frames (`World | Local | Parent`). `GizmoConstraint` is the closed
sum of axis / plane locks (`X | Y | Z | XY | XZ | YZ | Free`).
`Snap` is the value object holding the active quantisation rule —
position step in metres, rotation step in degrees, scale step — with
states `Off | SinglePerAxis | UniformPerAxis`. The aggregate owns
the drag-state resource in `EditorWorld` (anchor, current delta,
hover-axis), the per-`Selection` aggregate-transform centroid used
as the gizmo origin, and the toolbar bindings that mutate the
configuration resources.

**Identity & lifetime.** Drag state lives only between drag-begin
and drag-commit; configuration state (`GizmoFrame`, `GizmoConstraint`,
`Snap`) persists across editor sessions in user prefs. The gizmo's
on-viewport widget renders inside the `Viewport` panel (§4.2).

**Public-boundary invariants.**

1. **Drag-commit produces one `EditCommand`.** A drag-loop opens at
   pointer-down, accumulates deltas while pointer is held, and
   commits exactly one `EditCommand` (or one `Transaction` if the
   selection is multi-entity) at pointer-up. An aborted drag
   (escape key, focus loss) discards the in-progress delta and
   commits nothing.
2. **Snap quantises after frame transform.** The deltas the gizmo
   computes in its `GizmoFrame`'s local axes are snapped *after*
   the frame transform is applied; consumers never see an
   un-snapped intermediate when `Snap ≠ Off`.
3. **Constraint masks the delta source, not the result.**
   `GizmoConstraint = X | XY | …` masks which input dimensions
   reach the delta computation; the resulting transform delta
   leaves un-constrained axes byte-identical to the pre-drag
   transform. No sub-millimetre noise on locked axes.
4. **One drag at a time.** The drag-loop state machine is a
   single-track resource; concurrent drag attempts (e.g. two
   pointers in a future XR mode) are refused at the input
   boundary with `tools::Error::Refused`.
5. **Read-only outside drag.** When no drag is in progress the
   gizmo aggregate emits no `EditCommand`s and mutates no
   `GameWorld` state; widget hover and axis-highlight live
   entirely inside `EditorWorld`.

**SRP justification:** the rules of "user gestures over the
viewport become atomic transform deltas". If the gizmo's widget
geometry, frame semantics, constraint interpretation, or snap
quantisation changes, this aggregate changes; nothing else does.

### 4.6 `AssetBrowser` + `AssetThumbnail` (aggregate)

**Reason to change:** how the local project tree surfaces as
drag-droppable handles inside the shell (§3.2 collapse #1 sub-part —
the editor's read-only window onto `content`'s on-disk layout).
Distinct from `content`'s I/O policy or `render`'s capture-to-texture
pass — both refused by §3.3.

**Composition.** `AssetBrowser` is the registered panel walking the
paths surfaced by `content`'s public listing API; it never opens
files directly. `AssetThumbnail` is a value object: a cached preview
texture handle (rendered by `render`'s capture-to-texture path,
borrowed as an opaque texture id) plus the thumbnail's source-asset
hash so cache invalidation is cheap. The aggregate owns the
in-memory thumbnail LRU keyed by `AssetHandle`, the active filter /
search state inside `EditorWorld`, and the drag-payload type
(`AssetHandle` value passed via Dear ImGui drag-drop) consumed by
`Inspector` slots and the `Viewport`.

**Identity & lifetime.** Thumbnails live in the LRU until evicted
under memory pressure (per the §9 budget cell); they are *not*
persisted to disk by tools (caching policy is `content`'s concern
when/if added). Drag-payloads exist only between drag-start and
drop-or-cancel.

**Public-boundary invariants.**

1. **Read-only over the project tree.** `AssetBrowser` issues only
   listing and metadata queries to `content`; it never writes,
   imports, bakes, or mutates assets on disk. File I/O policy is
   refused per §3.3 → `content`.
2. **Drop produces an `EditCommand`.** Dropping an `AssetHandle`
   onto an `Inspector` slot produces an `EditCommand` whose
   `apply()` binds the handle to the target `ReflectedField` and
   whose `undo()` restores the previous handle. Dropping onto the
   `Viewport` produces a "spawn entity with `MeshHandle = …`"
   `EditCommand`. No drop ever mutates `GameWorld` directly.
3. **Thumbnails are display-only.** `AssetThumbnail` carries an
   opaque texture id (vended by `render`); the editor never
   inspects the underlying GPU texture and never re-encodes the
   image. Capture is requested by handle; the producer (render's
   capture-to-texture path) owns memory and lifetime.
4. **Local tree only.** Listings come from `content`'s
   *project-local* surface; remote stores, marketplaces, asset
   bundles, mod stores, and download caches are refused per §3.3.
5. **Eviction never invalidates a drag in progress.** A drag with
   a live `AssetHandle` payload pins its thumbnail's
   metadata-row in the LRU until the drag concludes; evicting
   thumbnails mid-drag is rejected.

**SRP justification:** the rules of how local assets become
drag-droppable handles in the shell. If the listing protocol, the
thumbnail-cache eviction policy, or the drop-payload type changes,
this aggregate changes; nothing else does.

### 4.7 `EditCommand` + `CommandStack` + `Transaction` (aggregate)

**Reason to change:** how reversible operations against the
`GameWorld` are recorded, grouped, and replayed (§3.2 collapse #6 —
eight harmonius undo/redo concerns collapse into one stack + one
grouper at MVP). Distinct from any specific edit's payload (each
`EditCommand` subtype is a value type carrying its own apply/undo
closures).

**Composition.** `EditCommand` is a value object carrying `apply()`
and `undo()` closures, an optional `coalesce(other)` predicate, a
byte-count estimate (for the in-memory budget), pre/post `Selection`
snapshots, and a typed payload (component edit / entity add / entity
remove / parent change / asset slot bind). `CommandStack` is the
resource in `EditorWorld` holding two contiguous arrays — the undo
stack and the redo stack — plus the byte-budget cap and the eviction
policy (oldest-first when over budget). `Transaction` is the grouping
RAII handle: `begin()` opens a transaction, every push during its
lifetime appends to a private buffer, `commit()` atomically promotes
the buffer to one stack entry, `abort()` discards. Multi-entity edits
emitted by `Gizmo` drag-commits or `Inspector` multi-select writes
flow through one `Transaction`.

**Identity & lifetime.** `CommandStack` lives across the editor
session (and survives hot-reload of every plugin including `tools`
itself per §8). On-disk persistence of history is deferred per
§3.2 collapse #6. `Transaction` exists only between `begin()` and
`commit()` / `abort()`.

**Public-boundary invariants.**

1. **Monotonic with O(1) undo / redo.** `CommandStack` is a
   contiguous array; pushing, undoing, and redoing the top entry
   are O(1) amortised. The "monotonic" property: at any instant
   the stack is fully ordered by push-time and partitioned into
   `undo[0..top]` and `redo[top..end]`; pushing a new command
   when redo is non-empty truncates redo (no branching history at
   MVP per §3.2 collapse #6).
2. **`apply()` and `undo()` are inverses.** For every committed
   `EditCommand` `c`, applying `c.undo()` immediately after
   `c.apply()` returns the `(GameWorld, Selection)` pair to a
   state byte-equal to its pre-`apply` snapshot, modulo unrelated
   concurrent edits (refused: there are no concurrent edits — see
   invariant 4). Violation refuses commit with
   `tools::Error::CommandConflict`.
3. **`Transaction` atomicity.** A `Transaction` either commits all
   contained `EditCommand`s atomically (one user-visible undo
   step) or commits none of them on `abort()`. A transaction in
   flight refuses any direct push to the stack (push-while-grouped
   routes to the transaction buffer).
4. **Single-writer to `GameWorld` storage.** Only `CommandStack`'s
   apply-path mutates `GameWorld` storage; every mutation source
   in tools (gizmo drag-commit, inspector write, asset-drop,
   scene-tree reparent) flows through here. Concurrent applies
   are refused — the stack mutates serially within phase 5
   (transform) of the editor world (consistent with `core`'s
   single-writer-per-chunk invariant § 4.1.5).
5. **Selection coupling.** Every `EditCommand` carries pre-/post-
   `Selection` snapshots; `apply()` restores `post`, `undo()`
   restores `pre`, and either path emits exactly one
   `EditorEvent::SelectionChanged` if the snapshots differ.
6. **Byte-budget bounded.** When pushing a new command would
   exceed the in-memory budget cell from §9, the oldest entries
   are evicted (FIFO from the bottom of the undo stack); evictions
   are deterministic and never silently drop redo entries (which
   are truncated only by an explicit new push, per invariant 1).
7. **Latency target.** Apply / undo of a single `EditCommand`
   completes in ≤ 50 ms on the §9 reference target; this is a
   first-class budget assertion enforced by perf tests.

**SRP justification:** the rules of "how reversible operations are
recorded, grouped, and replayed". Changes to apply/undo discipline,
transaction grouping, eviction policy, or selection coupling move
this aggregate; nothing else does.

### 4.8 `Toolbar` + `PlayPauseStep` (aggregate)

**Reason to change:** how the top-of-shell control strip drives
`EditorMode` transitions on the embedded `GameWorld` and surfaces
mode-affecting toggles (snap, gizmo mode, recording status). Distinct
from any single subsystem the buttons control — the toolbar is the
*input* surface, not the implementation.

**Composition.** `Toolbar` is the registered panel hosting a fixed
set of controls: the `PlayPauseStep` trio (Play / Pause / Step
buttons), gizmo-mode selector (`Translate | Rotate | Scale`),
gizmo-frame selector (`World | Local | Parent`), snap toggles
(position, rotation, scale), and recording-status indicator. The
controls bind to resources owned by other aggregates — `EditorMode`
on the host (§4.1), gizmo configuration (§4.5), `TraceRecorder` state
(§4.9) — and produce `EditorEvent`s on every change. `PlayPauseStep`
is the named trio whose three actions are the only legitimate way to
transition `EditorMode` from outside the trace recorder.

**Identity & lifetime.** One `Toolbar` per `EditorHost`. Survives
profile switches (the toolbar is not a `Layout`-positioned panel —
its shell-anchored slot is fixed in MVP).

**Public-boundary invariants.**

1. **Mode transitions exclusively via `PlayPauseStep` or
   `TraceRecorder`.** No other code path mutates `EditorMode`;
   programmatic mode changes from tests or plugins go through the
   same controls (or through `TraceRecorder` for recording-mode
   entry / exit). Direct writes to the resource are refused.
2. **Single-step is exactly one game-world frame.** Pressing Step
   while in `Paused` transitions to `Step`, advances the embedded
   `GameWorld` by exactly one phase-9 boundary, and transitions
   back to `Paused`. Step is a no-op outside `Paused`.
3. **Buttons are read-only over the systems they reflect.** A
   toolbar button surfaces state; it does not own state. The
   gizmo-mode buttons read and mutate `Gizmo`'s configuration
   resource (§4.5), they do not store gizmo mode locally.
4. **Recording-status indicator is read-only.** The status
   indicator reads `TraceRecorder`'s state (§4.9); it never
   starts or stops recording itself — that is the recorder's
   own action surface.

**SRP justification:** the rules of how toolbar input maps to mode
transitions and configuration writes. If the trio's action set,
button layout, or mode-transition semantics changes, this aggregate
changes; the systems on the other side of those buttons do not.

### 4.9 `TraceRecorder` + `TraceFile` (aggregate)

**Reason to change:** how the editor captures a deterministic record
of input + scheduler ticks + assertions into a `.glibre-trace` file
the E2E runner can replay (§3.2 collapse #7 — automation, replay, AI
tool-invocation, and CI integration collapse into this one
deterministic seam; AI-driven editor automation refused per §3.3).
Distinct from how the E2E runner *replays* a trace (owned by
`specs/e2e/SPEC.md`).

**Composition.** `TraceRecorder` is the resource in `EditorWorld`
holding the active recording's writer state: the open `TraceFile`
handle, the append-only `TraceOp` ring buffer, the assertion
templates the user has configured for capture, and the recording's
start tick. `TraceOp` is the closed sum of typed entries: input
event (mouse / keyboard / gamepad), scheduler tick boundary,
assertion (`expect:component:value`), `Selection` mutation,
`EditCommand` push. `TraceFile` is the on-disk artifact — a
Fory-archived sequence of `TraceOp`s under `tests/e2e/` whose schema
is owned jointly with `specs/e2e/SPEC.md`. The aggregate owns no
read path: replay is the E2E runner's responsibility.

**Identity & lifetime.** Recordings exist only when
`EditorMode == Recording`. The `TraceFile` is opened on
mode-transition into `Recording`, written through the recording, and
closed on transition out. Aborted recordings (process crash) leave a
truncated but well-formed prefix file (per the Fory append-streaming
contract — final commit is the only header write).

**Public-boundary invariants.**

1. **Non-perturbing capture.** `TraceRecorder` is a write-only
   side channel: capturing a `TraceOp` may not mutate any system
   the editor observes. No game-world state, no editor-world
   non-recorder state, no scheduler ordering, and no input event
   is altered by the act of recording. Disable / enable
   recording is byte-equal in the recorded execution path
   (consistent with PHILOSOPHY §7 — determinism by default).
2. **Append-only `TraceFile`.** The file is opened with append
   semantics; in-flight writes never seek backwards. Truncation
   of an existing file at recording-start is the one allowed
   non-append act.
3. **Bounded write latency.** Each `TraceOp` capture serialises
   in ≤ 100 µs on the §9 target; over-budget writes drop the
   recording with `tools::Error::TraceWriteFailed` and the
   editor exits `Recording` mode (per invariant 1, dropping a
   recording does not perturb the surrounding execution).
4. **Schema versioned.** `TraceFile` carries the `.glibre-trace`
   schema version negotiated with `specs/e2e/SPEC.md`; older
   recordings replay through the data-context migration table.
5. **Recording boundary is the only `EditorMode` writer outside
   the toolbar.** Entering / exiting `Recording` mode is the
   recorder's privilege; the toolbar surfaces a button whose
   action delegates to the recorder rather than mutating
   `EditorMode` directly.

**SRP justification:** the rules of capturing a deterministic,
non-perturbing record of editor activity into a replayable artifact.
If the `TraceOp` vocabulary, write discipline, or file schema
evolves, this aggregate changes; the replay runner and the editor
shell do not.

### 4.10 Cross-aggregate invariants

Invariants that span more than one aggregate and must hold at every
public boundary at the seams between them:

1. **Editor world disjoint from `GameWorld`, always.** No aggregate
   above ever stores an `Entity` from one world inside a resource
   on the other world; no editor-only component type ever enters
   the game world's type registry. Cross-world `Entity` passes are
   rejected at each aggregate's public boundary
   (`core::Error::EntityForeignWorld`).
2. **Play-mode toggles `GameWorld` scheduling without touching
   editor world scheduling.** `EditorMode ∈ { Edit, Paused }`
   freezes the game world's `FrameLoop`; the editor world keeps
   ticking so the shell stays responsive. `EditorMode = Step`
   advances the game world by exactly one frame and reverts to
   `Paused`. `EditorMode = Recording` keeps the game world
   ticking while `TraceRecorder` captures (§4.9 invariant 1
   guarantees this is non-perturbing).
3. **One edit pipeline.** Every mutation of `GameWorld` storage
   from inside tools flows through `CommandStack` (§4.7) — gizmo
   drag-commits, inspector writes, asset-drops, scene-tree
   reparents all produce `EditCommand`s. No aggregate has a
   parallel write path; `tools::Error::CommandConflict` is the
   refusal arm when this is violated.
4. **Inspector reads via `ReflectionBlob`, never raw pointers.**
   Across the inspector → registry → component-storage seam, no
   raw `T*` typed reference to game-world component data ever
   crosses an aggregate boundary. `ReflectionBlob` is the only
   read path (§4.4 invariant 1).
5. **Trace recording is non-perturbing.** Across recorder / event
   bus / scheduler / input pump, capturing a `TraceOp` does not
   alter any observed system's state, ordering, or timing — the
   recorder is a write-only side channel (§4.9 invariant 1).
6. **CommandStack monotonic with O(1) undo.** Across stack /
   transaction / event-bus seams, undo / redo are O(1) and the
   stack's partition into `undo` and `redo` halves is fully
   ordered at every instant (§4.7 invariant 1).
7. **One `RenderFrame` extract.** Across `EditorHost` / `Panel` /
   `Viewport` / `render` seam, the editor emits exactly one Dear
   ImGui draw-list extract per editor frame into `render`'s
   `RenderFrame` slot. No second extract path or second renderer
   exists (§4.1 invariant 4 / §4.2 invariant 5).
8. **Per-context error model honoured.** Every aggregate's public
   fallible operation returns `glibre::Result<T, glibre::Error>`
   per `reviews/decisions/error-model.md`; tools' arms live in
   the `tools::Error` enum named in §2 and are the only
   tools-internal error surface.
9. **Hot-reload survival.** Across hot-reload of any plugin
   *other* than tools itself, `Layout` profiles, `Selection`,
   `CommandStack`, `Gizmo` configuration, `Toolbar` state, and
   active `TraceRecorder` recordings all survive the swap per the
   engine-wide hot-reload protocol (`reviews/decisions/hot-reload-protocol.md`).
   Tools' own dylib swap follows the same protocol; details
   live in §8.
10. **Frame-phase ownership.** Tools registers no phase of its
    own; its work runs inside phases owned by other contexts
    (input → core's input phase, draw-list emit → render phase 6
    extract, mode transitions → editor world phase 9). Tools
    never mutates `GameWorld` outside the moments when its
    `CompiledFrame` permits writes.

## 5. Public Interface

The header stub below is the §5 deliverable: every symbol that crosses
the `tools` plugin's public boundary, declared in one C++23 header and
verified clean under
`clang++ -std=c++23 -fsyntax-only -Wall -Wextra -Wpedantic`. Bodies live
inside the tools dylib; this header is the contract every caller (the
embedded `core::World`, `render`'s extract slot, `content`'s listing
surface, `data`'s `ReflectionBlob`, the E2E runner that consumes
`.glibre-trace` files) compiles against. Cross-context invariants
embedded here:

- Every fallible operation returns `glibre::Result<T>` per
  `reviews/decisions/error-model.md`. The tools-internal `Error` enum
  is the closed sum named in §2 / §4 and rolled into `glibre::Error`'s
  variant by `core` once the data context lands the central registry.
- Aggregates listed in §4 (`EditorHost`, `Layout` /
  `LayoutProfile` / `Panel` / `Viewport`, `SceneTree` / `Selection`,
  `Inspector` / `InspectorView` / `ReflectedField`, `Gizmo` /
  `GizmoFrame` / `GizmoConstraint` / `Snap`, `AssetBrowser` /
  `AssetThumbnail`, `EditCommand` / `CommandStack` / `Transaction`,
  `Toolbar` / `PlayPauseStep`, `TraceRecorder` / `TraceFile`) are
  forward-declared classes whose layout is owned inside the plugin.
  Callers manipulate them only through the methods exposed below.
- The reflection-driven `Inspector` reads component bytes solely via
  the data context's `ReflectionBlob` (§4.4 inv. 1 / §4.10 inv. 4); no
  raw `T*` references to game-world storage cross this header.
- `EditCommand` is a sealed sum over a small closed payload set
  (component edit / entity add / entity remove / parent change / asset
  slot bind); `CommandStack` is the single edit pipeline (§4.10 inv. 3)
  and the only writer to game-world storage from inside tools.
- `GizmoFrame` and `GizmoConstraint` are closed enums; widening them is
  an ABI bump.
- `TraceRecorder` is a write-only side channel — its surface exposes
  no read path (§4.9 inv. 1, §4.10 inv. 5); replay is owned by
  `specs/e2e/SPEC.md`.

The header has no Fory-serialised event types in MVP — the editor
republishes through `core`'s typed `EditorEvent` bus rather than a
second bus (§3.2 collapse #9), and the only persistent on-disk artifact
the editor authors is the `.glibre-trace` `TraceFile` (schema co-owned
with `specs/e2e/SPEC.md`) plus the user's `LayoutProfile` JSON whose
versioned schema is migrated by `data`.

```cpp
// SPDX-License-Identifier: Apache-2.0
// glibre — tools plugin public interface (header-only stub).
//
// This file is the §5 deliverable of `specs/tools/SPEC.md`. It declares
// every symbol crossing the tools plugin's public boundary. The bodies
// live inside the tools dylib; this header is the contract every caller
// (core / render / content / data / platform / e2e) compiles against.
//
// Cross-context invariants embedded here:
//   * Every fallible call returns `glibre::Result<T>` per
//     `reviews/decisions/error-model.md`. `-fno-exceptions` is enforced
//     globally (the editor's ImGui interop carve-out converts at the
//     module boundary before crossing this header).
//   * Aggregates are opaque — `EditorHost`, `Layout`, `Panel`,
//     `Viewport`, `Inspector`, `Gizmo`, `AssetBrowser`, `CommandStack`,
//     `Transaction`, `Toolbar`, `TraceRecorder`, `TraceFile` are
//     forward-declared classes whose layout is owned inside the plugin.
//   * Inspector reads flow through the data context's `ReflectionBlob`;
//     this header never exposes raw component pointers.
//   * `EditCommand` is a sealed sum (closed payload variant) and the
//     single edit pipeline into game-world storage from inside tools.
//   * `GizmoFrame` / `GizmoConstraint` / `EditorMode` / `tools::Error`
//     are closed enums — widening any of them is an ABI bump.
//   * `TraceRecorder` is write-only; replay is owned by `specs/e2e`.
//
// This stub compiles standalone with
// `clang++ -std=c++23 -fsyntax-only -Wall -Wextra -Wpedantic`.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// ---------------------------------------------------------------------------
// Stand-in declarations from sibling contexts. The real definitions live
// in `core/include/glibre/error.hpp`, `core/include/glibre/core/world.hpp`,
// `data/include/glibre/types/reflection.hpp`, etc.; this header forward-
// declares them so the stub compiles in isolation. The implementation .cpp
// files include the real headers, not these stubs.
// ---------------------------------------------------------------------------

#if !defined(GLIBRE_HAVE_CORE_ERROR)
namespace glibre {

namespace core {
enum class Error : std::uint16_t {
    EntityStale,
    EntityForeignWorld,
    HierarchyCycle,
    TypeUnregistered,
    TypeRegistryClosed,
    ScheduleAccessConflict,
    SystemScheduleCycle,
    FramePhaseMisordered,
    AssetStale,
    CommandBufferOverflow,
    PluginAbiHashMismatch,
    PluginInitFailed,
    HotReload,
    SchemaMigrationFailed,
    OutOfBudget,
};
}  // namespace core

struct ErrorContext {
    std::string_view file{};
    int              line{0};
    std::string_view detail{};
};

class Error {
public:
    using Variant = std::variant<core::Error /*, tools::Error inserted in core */>;

    template <class E>
    constexpr Error(E e, ErrorContext ctx = {}) noexcept
        : variant_{e}, ctx_{ctx} {}

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

#if !defined(GLIBRE_HAVE_CORE_WORLD)
namespace glibre::core {

// Opaque ECS world handle. The editor borrows two: the dedicated
// EditorWorld (constructed at plugin init) and the embedded GameWorld
// (registered by the host runtime). Both are forward-declared; this
// header touches them only by reference.
class World;

// Opaque per-world entity handle. The (index, generation) split is
// private to core; tools compares and stores by value. Cross-world
// passes are rejected at every public boundary with
// `core::Error::EntityForeignWorld`.
struct Entity {
    std::uint64_t bits{0};
    friend constexpr bool operator==(Entity, Entity) noexcept = default;
};

// Codegen-emitted stable component identifier. Listed by the type
// registry; tools never invents new TypeId values.
struct TypeId {
    std::uint64_t value{0};
    friend constexpr bool operator==(TypeId, TypeId) noexcept = default;
};

}  // namespace glibre::core
#endif  // GLIBRE_HAVE_CORE_WORLD

#if !defined(GLIBRE_HAVE_DATA_REFLECTION)
namespace glibre::types {

// Subset of `data`'s reflection surface (specs/data/SPEC.md §5)
// reached by the inspector. Read-only; codegen-emitted; stripped to
// nullptr in shipping builds.
struct SchemaId {
    std::string_view fqn{};
    constexpr bool operator==(const SchemaId&) const noexcept = default;
};

using SchemaVersion = std::uint32_t;

struct ReflectionField {
    std::string_view name{};
    std::uint16_t    tag{0};
    std::string_view type_name{};   // builtin or another FQN
    SchemaVersion    since{0};
};

struct ReflectionBlob {
    SchemaId                          schema{};
    SchemaVersion                     version{0};
    std::span<const ReflectionField>  fields{};   // tag-sorted ascending
};

}  // namespace glibre::types
#endif  // GLIBRE_HAVE_DATA_REFLECTION

#if !defined(GLIBRE_HAVE_PLATFORM_INPUT)
namespace glibre::platform {
// Forward-declared opaque InputEvent. The real definition lives in
// `specs/platform/SPEC.md` §5 as a closed `std::variant` over
// KeyDown / KeyUp / MouseMove / MouseButtonEv / Wheel / TextInput /
// GamepadAxisEv / GamepadBtnEv. Tools consumes by const reference.
struct InputEvent;
}  // namespace glibre::platform
#endif  // GLIBRE_HAVE_PLATFORM_INPUT

#if !defined(GLIBRE_HAVE_RENDER_FRAME)
namespace glibre::render {
// Forward-declared opaque RenderFrame and TextureId. RenderFrame is the
// per-frame extract owned by render (specs/render/SPEC.md §4.1.1);
// tools emits Dear ImGui draw lists into it once per editor frame.
// `TextureId` is the borrowed opaque texture handle that backs a
// Viewport blit and an AssetThumbnail.
class RenderFrame;
struct TextureId {
    std::uint64_t bits{0};
    friend constexpr bool operator==(TextureId, TextureId) noexcept = default;
};
}  // namespace glibre::render
#endif  // GLIBRE_HAVE_RENDER_FRAME

#if !defined(GLIBRE_HAVE_CONTENT_ASSET)
namespace glibre::content {
// Type-erased asset handle. The real `AssetHandle<T>` template lives in
// `specs/content/SPEC.md` §5; the editor stores type-erased bits in the
// drag-payload + thumbnail LRU and rebinds the typed view at the
// `Inspector` slot only when an `EditCommand` applies.
struct AssetHandle {
    std::uint64_t bits{0};
    friend constexpr bool operator==(AssetHandle, AssetHandle) noexcept = default;
};
}  // namespace glibre::content
#endif  // GLIBRE_HAVE_CONTENT_ASSET

namespace glibre::tools {

// ---------------------------------------------------------------------------
// 5.1 tools::Error — closed sum of every failure mode at a public
//     tools boundary (named in §2 and rolled up in §10). Adding a
//     variant is an ABI bump per error-model.md §"Composition Rules" #5.
// ---------------------------------------------------------------------------

enum class Error : std::uint16_t {
    LayoutLoadFailed,        // §4.2 inv. 2 / inv. 3 — malformed JSON, unknown panel id, partial apply.
    TraceWriteFailed,        // §4.9 inv. 3      — over-budget or backing-store error.
    InspectorUnknownType,    // §4.4 inv. 3      — registry has no descriptor for the (Type, Field).
    CommandConflict,         // §4.7 inv. 2 / §4.2 inv. 1 — apply/undo not inverse, or panel-id collision.
    Refused,                 // catch-all closed-sum refusal arm — concurrent drag, second viewport, mode-change races.
};

[[nodiscard]] constexpr std::string_view to_string(Error e) noexcept;

// ---------------------------------------------------------------------------
// 5.2 EditorMode — closed sum (§4.1 inv. 2, §4.10 inv. 2). Drives whether
//     the embedded GameWorld's FrameLoop advances and whether
//     TraceRecorder captures ops. `Step` is a one-shot mode that
//     advances exactly one game-world frame and reverts to `Paused`.
// ---------------------------------------------------------------------------

enum class EditorMode : std::uint8_t {
    Edit,
    Play,
    Paused,
    Step,
    Recording,
};

[[nodiscard]] constexpr std::string_view to_string(EditorMode m) noexcept;

// ---------------------------------------------------------------------------
// 5.3 Stable string id type for Panels and LayoutProfiles.
//
// Panel ids and profile names are compile-time-stable tokens; the host
// stores them by value. Equality is byte-equality of the string view.
// ---------------------------------------------------------------------------

struct PanelId {
    std::string_view value{};
    friend constexpr bool operator==(const PanelId&, const PanelId&) noexcept = default;
};

struct LayoutProfileName {
    std::string_view value{};
    friend constexpr bool operator==(const LayoutProfileName&, const LayoutProfileName&) noexcept = default;
};

// ---------------------------------------------------------------------------
// 5.4 Layout / LayoutProfile / Panel / Viewport (§4.2).
//
// `Layout` is the versioned-JSON dock-arrangement value object; the
// concrete shape (splits, sizes, tab groups, floats) is owned inside
// the plugin. Public callers carry it as opaque bytes — the editor
// host loads, saves, and switches profiles via the methods below.
// ---------------------------------------------------------------------------

class Layout;          // opaque value; serialised as versioned JSON
class LayoutProfile;   // opaque named-Layout slot owned by EditorWorld
class Panel;           // opaque base for all dockable panels
class Viewport;        // opaque Viewport — the one panel that blits a
                       // render-target via a TextureId from `render`
                       // (multi-viewport refused in MVP per §4.2 inv. 4)

struct PanelDesc {
    PanelId          id{};
    std::string_view title{};
    bool             closeable{true};
    bool             dockable{true};
};

// The draw callback is called once per editor frame at extract time
// (phase 6). It emits Dear ImGui geometry into the host-owned
// draw-list buffer routed to `render::RenderFrame`; it is forbidden to
// allocate, mutate game-world storage, or hold engine-wide singletons.
// Returning an error aborts the panel for this frame; the host logs.
using PanelDrawFn = std::function<glibre::Result<void>() /* noexcept */>;

// ---------------------------------------------------------------------------
// 5.5 Selection (§4.3) — deterministically-ordered set of GameWorld
//     entity ids. Single producer (SceneTree, viewport-pick, marquee
//     commit, EditCommand undo/redo restoration); deterministic
//     iteration; never persists to disk. `SelectionChanged` is
//     published synchronously on every mutation that changes the set.
// ---------------------------------------------------------------------------

class Selection {
public:
    [[nodiscard]] std::span<const glibre::core::Entity> entities() const noexcept;
    [[nodiscard]] std::size_t                           size()     const noexcept;
    [[nodiscard]] bool                                  empty()    const noexcept;
    [[nodiscard]] bool                                  contains(glibre::core::Entity) const noexcept;

    // Hashed snapshot for change-detection / coalescing; stable per
    // session (insertion-stable iteration, §4.3 inv. 2).
    [[nodiscard]] std::uint64_t snapshot_hash() const noexcept;

    Selection(const Selection&)            = delete;
    Selection& operator=(const Selection&) = delete;

protected:
    Selection() noexcept = default;
    ~Selection()         = default;
};

// ---------------------------------------------------------------------------
// 5.6 SceneTree (§4.3 panel side). Walks the embedded GameWorld's
//     ChildOf forest and surfaces row clicks as Selection mutations and
//     parent-change EditCommands; it never mutates ChildOf directly
//     (§4.3 inv. 5) and never owns Selection (§4.3 inv. 3).
// ---------------------------------------------------------------------------

class SceneTree {
public:
    // Re-walk the embedded GameWorld's hierarchy from the root forest;
    // idempotent. Called once per panel-frame when the tree is dirty.
    [[nodiscard]] glibre::Result<void> rebuild() noexcept;

    SceneTree(const SceneTree&)            = delete;
    SceneTree& operator=(const SceneTree&) = delete;

protected:
    SceneTree() noexcept = default;
    ~SceneTree()         = default;
};

// ---------------------------------------------------------------------------
// 5.7 Inspector / InspectorView / ReflectedField (§4.4).
//
// Reads flow through `glibre::types::ReflectionBlob`; writes return
// EditCommand values rather than mutating component storage. A
// ReflectedField with no registry descriptor refuses with
// `tools::Error::InspectorUnknownType` and the row is omitted.
// ---------------------------------------------------------------------------

class EditCommand;           // declared in §5.10
class InspectorView;
class ReflectedField;

class Inspector {
public:
    // Re-build the per-Selection view set. Called once per panel-frame.
    [[nodiscard]] glibre::Result<void> refresh(const Selection&) noexcept;

    // Number of (Entity, ComponentType) views currently bound. Used by
    // tests and by the panel-header summary row.
    [[nodiscard]] std::size_t view_count() const noexcept;

    Inspector(const Inspector&)            = delete;
    Inspector& operator=(const Inspector&) = delete;

protected:
    Inspector() noexcept = default;
    ~Inspector()         = default;
};

class InspectorView {
public:
    [[nodiscard]] glibre::core::Entity                   entity()        const noexcept;
    [[nodiscard]] glibre::core::TypeId                   component()     const noexcept;
    [[nodiscard]] const glibre::types::ReflectionBlob*   reflection()    const noexcept;
    [[nodiscard]] std::span<const ReflectedField>        fields()        const noexcept;

    InspectorView(const InspectorView&)            = delete;
    InspectorView& operator=(const InspectorView&) = delete;

protected:
    InspectorView() noexcept = default;
    ~InspectorView()         = default;
};

// One row inside an InspectorView. The read closure resolves through
// the registry's ReflectionBlob accessor; the write closure produces
// an EditCommand value rather than mutating the component directly.
class ReflectedField {
public:
    [[nodiscard]] std::string_view                     name()      const noexcept;
    [[nodiscard]] std::string_view                     type_name() const noexcept;
    [[nodiscard]] std::uint16_t                        tag()       const noexcept;

    // Read the current value as an opaque byte view through the
    // registry's ReflectionBlob accessor; the bytes are owned by the
    // game-world component storage and outlive the call only until the
    // next phase-5 boundary.
    [[nodiscard]] glibre::Result<std::span<const std::byte>>
        read() const noexcept;

    // Produce an EditCommand that, when applied, writes `bytes` into
    // the field. The command flows through CommandStack (§4.7); the
    // inspector itself never mutates component storage.
    [[nodiscard]] glibre::Result<EditCommand>
        write(std::span<const std::byte> bytes) const noexcept;

    ReflectedField(const ReflectedField&)            = delete;
    ReflectedField& operator=(const ReflectedField&) = delete;

protected:
    ReflectedField() noexcept = default;
    ~ReflectedField()         = default;
};

// ---------------------------------------------------------------------------
// 5.8 Gizmo / GizmoFrame / GizmoConstraint / Snap (§4.5).
//
// Closed sums for mode, frame, and constraint. Drag-commit produces
// exactly one EditCommand (or one Transaction, multi-entity); aborted
// drags discard. Snap quantises after frame transform; constraint
// masks input dimensions, leaving locked axes byte-identical.
// ---------------------------------------------------------------------------

enum class Gizmo : std::uint8_t {
    Translate,
    Rotate,
    Scale,
};

enum class GizmoFrame : std::uint8_t {
    World,
    Local,
    Parent,
};

enum class GizmoConstraint : std::uint8_t {
    Free,
    X,
    Y,
    Z,
    XY,
    XZ,
    YZ,
};

[[nodiscard]] constexpr std::string_view to_string(Gizmo)           noexcept;
[[nodiscard]] constexpr std::string_view to_string(GizmoFrame)      noexcept;
[[nodiscard]] constexpr std::string_view to_string(GizmoConstraint) noexcept;

// Snap quantisation rule. `mode` selects the active state; the
// per-axis fields are read only when `mode == UniformPerAxis` /
// `SinglePerAxis`. Steps are in metres / degrees / dimensionless
// scale-factor; zero step disables that axis.
struct Snap {
    enum class Mode : std::uint8_t {
        Off,
        SinglePerAxis,
        UniformPerAxis,
    };

    Mode  mode{Mode::Off};
    float position_step_metres{0.0f};
    float rotation_step_degrees{0.0f};
    float scale_step{0.0f};

    friend constexpr bool operator==(const Snap&, const Snap&) noexcept = default;
};

class GizmoController {
public:
    // Active mode / frame / constraint / snap. These are configuration
    // resources persisted in user prefs (§4.5).
    [[nodiscard]] Gizmo            mode()        const noexcept;
    [[nodiscard]] GizmoFrame       frame()       const noexcept;
    [[nodiscard]] GizmoConstraint  constraint()  const noexcept;
    [[nodiscard]] Snap             snap()        const noexcept;

    [[nodiscard]] glibre::Result<void> set_mode(Gizmo)                 noexcept;
    [[nodiscard]] glibre::Result<void> set_frame(GizmoFrame)           noexcept;
    [[nodiscard]] glibre::Result<void> set_constraint(GizmoConstraint) noexcept;
    [[nodiscard]] glibre::Result<void> set_snap(Snap)                  noexcept;

    // True iff a drag is in flight; concurrent drag attempts return
    // `tools::Error::Refused` (§4.5 inv. 4).
    [[nodiscard]] bool dragging() const noexcept;

    GizmoController(const GizmoController&)            = delete;
    GizmoController& operator=(const GizmoController&) = delete;

protected:
    GizmoController() noexcept = default;
    ~GizmoController()         = default;
};

// ---------------------------------------------------------------------------
// 5.9 AssetBrowser / AssetThumbnail (§4.6).
//
// Read-only over the project tree. Drops produce EditCommand values.
// Thumbnails carry a borrowed render::TextureId and a source-asset
// hash for cheap cache invalidation; eviction never invalidates a
// drag in progress (§4.6 inv. 5).
// ---------------------------------------------------------------------------

struct AssetThumbnail {
    glibre::content::AssetHandle source{};
    glibre::render::TextureId    texture{};
    std::uint64_t                source_hash{0};
    std::uint16_t                width{0};
    std::uint16_t                height{0};
};

class AssetBrowser {
public:
    // Read-only listing of paths surfaced by `content` rooted at
    // `subpath`. Returns a borrowed view valid until the next refresh.
    [[nodiscard]] glibre::Result<std::span<const glibre::content::AssetHandle>>
        list(std::string_view subpath) const noexcept;

    // Look up the cached thumbnail for `handle`; returns nullptr if
    // not yet captured. Capture is scheduled by the editor and
    // performed by render's capture-to-texture path.
    [[nodiscard]] const AssetThumbnail*
        thumbnail_for(glibre::content::AssetHandle handle) const noexcept;

    // Pin / unpin a thumbnail entry (§4.6 inv. 5 — drag in flight).
    // Pinned entries are never evicted by LRU pressure.
    [[nodiscard]] glibre::Result<void>
        pin(glibre::content::AssetHandle handle) noexcept;
    [[nodiscard]] glibre::Result<void>
        unpin(glibre::content::AssetHandle handle) noexcept;

    AssetBrowser(const AssetBrowser&)            = delete;
    AssetBrowser& operator=(const AssetBrowser&) = delete;

protected:
    AssetBrowser() noexcept = default;
    ~AssetBrowser()         = default;
};

// ---------------------------------------------------------------------------
// 5.10 EditCommand / CommandStack / Transaction (§4.7).
//
// EditCommand is a sealed sum over the closed payload set named in §4.7
// Composition; CommandStack is the single edit pipeline (§4.10 inv. 3)
// and the only writer to GameWorld storage from inside tools.
// `apply` and `undo` are inverse functions of the (GameWorld, Selection)
// pair (§4.7 inv. 2); violation refuses commit with CommandConflict.
// ---------------------------------------------------------------------------

namespace edit {

// One byte-buffer payload + the type id it targets — used by
// `ComponentEdit`, `AssetSlotBind`, and any future field-level
// EditCommand subtype that carries Fory-encoded bytes.
struct ComponentEdit {
    glibre::core::Entity   entity{};
    glibre::core::TypeId   component{};
    std::vector<std::byte> previous_bytes{};
    std::vector<std::byte> next_bytes{};
};

struct EntityAddComponent {
    glibre::core::TypeId   component{};
    std::vector<std::byte> bytes{};
};

struct EntityAdd {
    glibre::core::Entity            parent{};   // optional; bits == 0 means root
    std::vector<EntityAddComponent> components{};
};

struct EntityRemove {
    glibre::core::Entity entity{};
};

struct ParentChange {
    glibre::core::Entity child{};
    glibre::core::Entity old_parent{};   // bits == 0 means was root
    glibre::core::Entity new_parent{};   // bits == 0 means becomes root
};

struct AssetSlotBind {
    glibre::core::Entity         entity{};
    glibre::core::TypeId         component{};
    std::uint16_t                field_tag{0};
    glibre::content::AssetHandle previous{};
    glibre::content::AssetHandle next{};
};

}  // namespace edit

using EditCommandPayload = std::variant<
    edit::ComponentEdit,
    edit::EntityAdd,
    edit::EntityRemove,
    edit::ParentChange,
    edit::AssetSlotBind>;

// Pre/post Selection snapshot — captured by every EditCommand so undo
// and redo restore selection deterministically (§4.7 inv. 5).
struct SelectionSnapshot {
    std::uint64_t                                hash{0};
    std::vector<glibre::core::Entity>            entities{};
};

class EditCommand {
public:
    [[nodiscard]] const EditCommandPayload&    payload()        const noexcept;
    [[nodiscard]] std::size_t                  byte_estimate()  const noexcept;
    [[nodiscard]] const SelectionSnapshot&     pre_selection()  const noexcept;
    [[nodiscard]] const SelectionSnapshot&     post_selection() const noexcept;

    // Optional coalesce predicate — when present, the stack may merge
    // `*this` with a successor command of the same shape (e.g.
    // sliding the gizmo continuously). Predicate must be pure and
    // deterministic.
    [[nodiscard]] bool coalesces_with(const EditCommand& next) const noexcept;

    EditCommand(const EditCommand&)            = default;
    EditCommand& operator=(const EditCommand&) = default;
    EditCommand(EditCommand&&) noexcept            = default;
    EditCommand& operator=(EditCommand&&) noexcept = default;
    ~EditCommand()                                 = default;

protected:
    EditCommand() noexcept = default;
};

class CommandStack;

// RAII grouping handle (§4.7 inv. 3). begin() opens a transaction;
// every push during its lifetime appends to a private buffer;
// commit() atomically promotes the buffer to one stack entry; abort()
// discards. A transaction in flight refuses any direct push to the
// stack (push-while-grouped routes to the transaction buffer).
class Transaction {
public:
    [[nodiscard]] glibre::Result<void> push(EditCommand cmd) noexcept;
    [[nodiscard]] glibre::Result<void> commit()              noexcept;
    void                               abort()               noexcept;

    [[nodiscard]] bool active() const noexcept;

    Transaction(const Transaction&)            = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) noexcept;
    Transaction& operator=(Transaction&&) noexcept;
    ~Transaction();

protected:
    Transaction() noexcept = default;
    friend class CommandStack;
};

class CommandStack {
public:
    // Push, undo, redo are O(1) amortised on the contiguous stack.
    [[nodiscard]] glibre::Result<void> push(EditCommand cmd) noexcept;
    [[nodiscard]] glibre::Result<void> undo()                noexcept;
    [[nodiscard]] glibre::Result<void> redo()                noexcept;

    // Open a transaction; while one is active, push() routes into it.
    [[nodiscard]] glibre::Result<Transaction> begin_transaction() noexcept;

    [[nodiscard]] std::size_t   undo_depth()    const noexcept;
    [[nodiscard]] std::size_t   redo_depth()    const noexcept;
    [[nodiscard]] std::size_t   bytes_in_use()  const noexcept;
    [[nodiscard]] std::size_t   byte_budget()   const noexcept;

    CommandStack(const CommandStack&)            = delete;
    CommandStack& operator=(const CommandStack&) = delete;

protected:
    CommandStack() noexcept = default;
    ~CommandStack()         = default;
};

// ---------------------------------------------------------------------------
// 5.11 Toolbar / PlayPauseStep (§4.8).
//
// PlayPauseStep is the only legitimate EditorMode writer outside the
// trace recorder. The toolbar reads-and-mutates Gizmo configuration and
// reads TraceRecorder state; it never owns either.
// ---------------------------------------------------------------------------

class Toolbar {
public:
    [[nodiscard]] glibre::Result<void> press_play()  noexcept;
    [[nodiscard]] glibre::Result<void> press_pause() noexcept;
    // Step is a no-op outside Paused (§4.8 inv. 2).
    [[nodiscard]] glibre::Result<void> press_step()  noexcept;

    // Read-only mode mirror.
    [[nodiscard]] EditorMode current_mode() const noexcept;

    // Gizmo configuration shortcuts — delegate to GizmoController.
    [[nodiscard]] glibre::Result<void> set_gizmo_mode(Gizmo)             noexcept;
    [[nodiscard]] glibre::Result<void> set_gizmo_frame(GizmoFrame)       noexcept;
    [[nodiscard]] glibre::Result<void> set_snap(Snap)                    noexcept;

    Toolbar(const Toolbar&)            = delete;
    Toolbar& operator=(const Toolbar&) = delete;

protected:
    Toolbar() noexcept = default;
    ~Toolbar()         = default;
};

// ---------------------------------------------------------------------------
// 5.12 TraceRecorder / TraceFile (§4.9).
//
// Write-only side channel. Capture is non-perturbing (§4.9 inv. 1).
// Replay is owned by `specs/e2e/SPEC.md` — this header exposes no
// read path. `TraceFile` is the on-disk artifact; tools authors it,
// the E2E runner consumes it.
// ---------------------------------------------------------------------------

namespace trace {

struct InputEventOp {
    std::uint64_t                       tick{0};
    const glibre::platform::InputEvent* event{nullptr};   // borrowed
};

struct SchedulerTickOp {
    std::uint64_t tick{0};
};

struct AssertionOp {
    std::uint64_t              tick{0};
    glibre::core::Entity       entity{};
    glibre::core::TypeId       component{};
    std::uint16_t              field_tag{0};
    std::span<const std::byte> expected_bytes{};
};

struct SelectionOp {
    std::uint64_t                         tick{0};
    std::span<const glibre::core::Entity> entities{};
};

struct CommandPushOp {
    std::uint64_t      tick{0};
    const EditCommand* command{nullptr};   // borrowed
};

}  // namespace trace

using TraceOp = std::variant<
    trace::InputEventOp,
    trace::SchedulerTickOp,
    trace::AssertionOp,
    trace::SelectionOp,
    trace::CommandPushOp>;

// Append-only writer interface — opening a TraceFile truncates an
// existing file; subsequent writes are append-only and never seek
// backwards (§4.9 inv. 2). Bounded write latency per op is enforced
// at runtime; over-budget writes return TraceWriteFailed and the
// recorder exits Recording mode (§4.9 inv. 3).
class TraceFile {
public:
    // Open a new recording; truncates if `path` already exists.
    [[nodiscard]] static glibre::Result<std::unique_ptr<TraceFile>>
        open_for_write(std::string_view path) noexcept;

    // Append a single op. Serialisation is Fory-archived; schema
    // version is co-owned with `specs/e2e/SPEC.md`.
    [[nodiscard]] glibre::Result<void> append(const TraceOp& op) noexcept;

    // Flush + close. Aborted recordings (process crash before close)
    // leave a truncated but well-formed prefix file.
    [[nodiscard]] glibre::Result<void> close() noexcept;

    [[nodiscard]] std::string_view path()    const noexcept;
    [[nodiscard]] std::uint64_t   op_count() const noexcept;

    TraceFile(const TraceFile&)            = delete;
    TraceFile& operator=(const TraceFile&) = delete;
    TraceFile(TraceFile&&) noexcept;
    TraceFile& operator=(TraceFile&&) noexcept;
    virtual ~TraceFile();

protected:
    TraceFile() noexcept = default;
};

class TraceRecorder {
public:
    // True iff EditorMode == Recording and a TraceFile is open.
    [[nodiscard]] bool recording() const noexcept;

    // Begin a recording; writes a fresh TraceFile prefix and
    // transitions EditorMode → Recording (§4.9 inv. 5 — the recorder
    // is the only EditorMode writer outside the toolbar).
    [[nodiscard]] glibre::Result<void> begin(std::string_view path) noexcept;

    // End a recording; closes the TraceFile and transitions back to
    // the previous EditorMode (typically Edit).
    [[nodiscard]] glibre::Result<void> end() noexcept;

    // Configure which assertion templates to capture. Capture is
    // non-perturbing (§4.9 inv. 1) — assertion templates are
    // evaluated against ReflectionBlob views, never against raw
    // pointers.
    struct AssertionTemplate {
        glibre::core::Entity entity{};
        glibre::core::TypeId component{};
        std::uint16_t        field_tag{0};
    };
    [[nodiscard]] glibre::Result<void>
        set_assertion_templates(std::span<const AssertionTemplate>) noexcept;

    TraceRecorder(const TraceRecorder&)            = delete;
    TraceRecorder& operator=(const TraceRecorder&) = delete;

protected:
    TraceRecorder() noexcept = default;
    ~TraceRecorder()         = default;
};

// ---------------------------------------------------------------------------
// 5.13 EditorEvent — typed sum republished through `core`'s ECS event
//      bus (§3.2 collapse #9 — tools never owns a second bus).
//      Listed here so plugin subscribers compile against one closed sum.
// ---------------------------------------------------------------------------

namespace events {

struct SelectionChanged { std::uint64_t snapshot_hash{0}; };
struct ModeChanged      { EditorMode previous{EditorMode::Edit}; EditorMode current{EditorMode::Edit}; };
struct LayoutSwitched   { LayoutProfileName previous{}; LayoutProfileName current{}; };
struct TraceStarted     { std::string_view path{}; };
struct TraceStopped     { std::string_view path{}; std::uint64_t op_count{0}; };
struct CommandPushed    { std::size_t undo_depth{0}; std::size_t redo_depth{0}; };
struct CommandUndone    { std::size_t undo_depth{0}; std::size_t redo_depth{0}; };

}  // namespace events

using EditorEvent = std::variant<
    events::SelectionChanged,
    events::ModeChanged,
    events::LayoutSwitched,
    events::TraceStarted,
    events::TraceStopped,
    events::CommandPushed,
    events::CommandUndone>;

// ---------------------------------------------------------------------------
// 5.14 EditorHost — aggregate root (§4.1).
//
// Owns the disjoint EditorWorld and a borrowed reference to the embedded
// GameWorld. Toggles GameWorld scheduling through EditorMode transitions
// at the phase-9 boundary (§4.1 inv. 3); never registers editor-only
// components onto the GameWorld.
// ---------------------------------------------------------------------------

struct EditorHostDesc {
    // Borrowed reference to the engine's game World. Tools never
    // takes ownership; lifetime is managed by the host runtime.
    glibre::core::World* game_world{nullptr};

    // Optional path to the LayoutProfile JSON the editor loads at
    // startup; empty = ship default.
    std::string_view startup_profile_path{};
};

class EditorHost {
public:
    // Construct the host. Spawns the dedicated EditorWorld and binds
    // `game_world`. Registers the default Panel set (Scene, Inspector,
    // Assets, Console, Profiler, Viewport, Toolbar). Returns a host
    // handle whose lifetime spans plugin load → unload.
    [[nodiscard]] static glibre::Result<std::unique_ptr<EditorHost>>
        create(const EditorHostDesc& desc) noexcept;

    // World accessors. The EditorWorld is owned by the host and
    // distinct from the game world (§4.1 inv. 1). Cross-world
    // Entity passes are rejected with `core::Error::EntityForeignWorld`.
    [[nodiscard]] glibre::core::World&       editor_world()       noexcept;
    [[nodiscard]] const glibre::core::World& editor_world() const noexcept;

    [[nodiscard]] glibre::core::World&       game_world()       noexcept;
    [[nodiscard]] const glibre::core::World& game_world() const noexcept;

    // Mode mirror — write-side lives on Toolbar / TraceRecorder
    // (§4.10 inv. 2). Reads here are O(1).
    [[nodiscard]] EditorMode mode() const noexcept;

    // Sub-aggregate accessors — all references are stable across
    // hot-reload of every plugin other than tools itself (§4.10 inv. 9).
    [[nodiscard]] Selection&        selection()        noexcept;
    [[nodiscard]] SceneTree&        scene_tree()       noexcept;
    [[nodiscard]] Inspector&        inspector()        noexcept;
    [[nodiscard]] GizmoController&  gizmo()            noexcept;
    [[nodiscard]] AssetBrowser&     asset_browser()    noexcept;
    [[nodiscard]] CommandStack&     command_stack()    noexcept;
    [[nodiscard]] Toolbar&          toolbar()          noexcept;
    [[nodiscard]] TraceRecorder&    trace_recorder()   noexcept;

    // Panel registration — every dockable Panel registers a stable id
    // and a draw closure; re-registering a known id refuses with
    // `tools::Error::CommandConflict` (§4.2 inv. 1).
    [[nodiscard]] glibre::Result<void>
        register_panel(const PanelDesc& desc, PanelDrawFn draw) noexcept;

    [[nodiscard]] glibre::Result<void>
        unregister_panel(PanelId id) noexcept;

    // LayoutProfile activation — atomic and lossless. On success the
    // `LayoutSwitched` event is emitted; on failure the previous
    // active layout is preserved and `LayoutLoadFailed` is returned
    // (§4.2 inv. 3).
    [[nodiscard]] glibre::Result<void>
        activate_profile(LayoutProfileName name) noexcept;

    [[nodiscard]] glibre::Result<void>
        save_profile(LayoutProfileName name, std::string_view path) noexcept;

    [[nodiscard]] glibre::Result<void>
        load_profile(LayoutProfileName name, std::string_view path) noexcept;

    // Per-frame entry. Called by the engine plugin manifest at the
    // editor world's phase boundaries; it drains the input pump,
    // runs panel-frame draw closures, emits Dear ImGui draw lists into
    // `render::RenderFrame` exactly once (§4.1 inv. 4 / §4.10 inv. 7),
    // and applies any phase-9 EditorMode transition.
    [[nodiscard]] glibre::Result<void>
        tick(glibre::render::RenderFrame& extract,
             std::span<const glibre::platform::InputEvent> input) noexcept;

    EditorHost(const EditorHost&)            = delete;
    EditorHost& operator=(const EditorHost&) = delete;
    virtual ~EditorHost();

protected:
    EditorHost() noexcept = default;
};

}  // namespace glibre::tools
```

### 5.1 Events

The editor publishes the closed `EditorEvent` sum declared above
through `core`'s ECS event bus — there is no second bus
(§3.2 collapse #9). Subscribers consume the sum by `std::visit` and
match exhaustively; adding a variant is an ABI bump per
`reviews/decisions/error-model.md` §"Composition Rules" #5. Tools is a
producer of `EditorEvent`; its only consumers in MVP are tools itself
(panels reading `SelectionChanged` / `ModeChanged`) and the E2E runner
(which observes `TraceStarted` / `TraceStopped` to align replay with
recording).

### 5.2 Serialised schemas (Fory)

The editor authors two persistent on-disk artifacts; both go through
`data`'s Fory codegen pipeline (`reviews/decisions/fory-codegen.md`).

- `data/schemas/tools/LayoutProfile.fory` — the versioned dock-arrangement
  JSON described in §4.2. Schema version is monotonic; loaders accept
  the current version and any older version reachable through `data`'s
  migration table. Malformed or unknown-future-version documents refuse
  load with `tools::Error::LayoutLoadFailed` and the previous active
  layout is preserved (§4.2 inv. 2 / inv. 3).
- `data/schemas/tools/TraceFile.fory` — the `.glibre-trace` artifact
  declared in §4.9. Schema is co-owned with `specs/e2e/SPEC.md` and
  versioned identically; older recordings replay through the same
  migration table. The on-disk shape is an append-only sequence of
  Fory-archived `TraceOp` envelopes prefixed by one schema-version
  header (the only non-append write, on `open_for_write`).

The `EditorEvent` sum is not persisted; it lives only on `core`'s
in-memory event bus per PHILOSOPHY anti-pattern §"serialised event
streams are not engine artifacts". `Selection` is also non-persistent
across sessions (§4.3 inv. 2). `EditCommand` payloads carry typed
component bytes that are themselves Fory-encoded by the originating
context's schema; tools never invents a parallel encoding.

### 5.3 Error types

The closed sum is `glibre::tools::Error` declared above. Each arm maps
to one §4 invariant:

| Arm                     | Raised when                                                                         | Origin                            |
|-------------------------|-------------------------------------------------------------------------------------|-----------------------------------|
| `LayoutLoadFailed`      | malformed JSON, unknown panel id, schema version unreachable, partial profile apply | §4.2 inv. 2 / inv. 3              |
| `TraceWriteFailed`      | `TraceFile::append` exceeds the §9 latency budget or the backing store errors       | §4.9 inv. 3                       |
| `InspectorUnknownType`  | `Inspector::refresh` finds a `(Type, Field)` with no registry descriptor            | §4.4 inv. 3                       |
| `CommandConflict`       | `apply` / `undo` not inverse, or panel-id collision on register                     | §4.2 inv. 1, §4.7 inv. 2          |
| `Refused`               | concurrent gizmo drag, second viewport register, mode-change race                   | §4.2 inv. 4, §4.5 inv. 4, §4.10   |

These arms wrap into `glibre::Error` via the central registry in
`core` per `reviews/decisions/error-model.md` §"Composition Rules" #2;
no other context's error enum nests inside `tools::Error`. Cross-context
failures (e.g. `core::Error::EntityForeignWorld` from a stale Selection
entity) are translated at the call site that crosses the boundary —
the inspector / scene-tree / gizmo paths each map inner enumerators
into their own `Error` arm where the §4 invariants demand it, and pass
through the inner `glibre::Error` value otherwise.

Verification: the stub above compiles clean under
`clang++ -std=c++23 -fsyntax-only -Wall -Wextra -Wpedantic` on the
toolchain documented in `reviews/decisions/fory-codegen.md` §"Open
Questions" #4 (libc++ as shipped with macOS Xcode 15 / Homebrew-LLVM).

## 6. Internal Architecture

Non-binding sketch for implementers.

## 7. Persistence & Schemas

Tools' persistence surface is intentionally narrow. The shell holds a
great deal of in-memory state (`Selection`, `CommandStack`, `Gizmo`
configuration, `Toolbar` mode mirror, `AssetThumbnail` LRU,
`ReflectionBlob` views, `EditorEvent` republish queue) but **persists
only what survives across editor-process lifetimes**: layout profiles,
the project-rooted scene reference document, the per-session command
journal envelope, and the user keymap. Per-frame artefacts and live
ECS data are runtime-only and never serialised by tools.

Tools **never invents a parallel encoding** for game-world component
bytes. `EditCommand` payloads, scene component slots, and inspector
field reads all flow through the originating context's Fory schemas
(via `data`'s middleman dylib per `reviews/decisions/fory-codegen.md`).
The schemas below are the *envelopes* tools owns — the structural
spine that wraps opaque, plugin-owned payload bytes.

All schemas are authored as `data/schemas/tools/<Type>.fory` files per
`reviews/decisions/fory-codegen.md` and compile into the `glibre-types`
middleman dylib. FQNs are `glibre.tools.<Type>`. Each schema ships
with at least one Catch2 round-trip test under
`tests/data/schemas/tools/<Type>.cpp` per the data SPEC §7 mandate.
Cross-context: `TraceFile` (§4.9) is **owned by `specs/e2e/SPEC.md`**;
its `.fory` schema lives at `data/schemas/e2e/TraceFile.fory` and is
not enumerated here. Tools writes `TraceOp` envelopes through the e2e
schema; this section enumerates only schemas tools authors.

### 7.1 Persistent types

#### 7.1.1 `LayoutProfile` — named dock arrangement

**File:** `data/schemas/tools/LayoutProfile.fory`
**FQN:** `glibre.tools.LayoutProfile`
**Lifetime scope:** per-user, per-project; written by the editor on
profile save, read at editor startup and on profile-switch (§4.2).
Stored under `<project-root>/.glibre/tools/layouts/<profile_name>.fory`.

```fory
schema glibre.tools.LayoutProfile {
  version  1
  since    "0.1.0"

  field profile_name   : string             tag 1 since 1
  field schema_version : u32                tag 2 since 1
  field root_split     : DockSplit          tag 3 since 1
  field floats         : list<FloatingPanel> tag 4 since 1
  field active_tabs    : list<ActiveTab>    tag 5 since 1
  field active_viewport_id : string         tag 6 since 1   default ""
}

schema glibre.tools.DockSplit {
  version 1
  since   "0.1.0"

  field axis     : u8           tag 1 since 1                  // 0=horiz, 1=vert, 2=leaf
  field ratio    : f32          tag 2 since 1   default 0.5
  field children : list<DockSplit> tag 3 since 1
  field panel_id : string       tag 4 since 1   default ""     // populated only on leaves
}

schema glibre.tools.FloatingPanel {
  version 1
  since   "0.1.0"

  field panel_id : string tag 1 since 1
  field x        : f32    tag 2 since 1
  field y        : f32    tag 3 since 1
  field width    : f32    tag 4 since 1
  field height   : f32    tag 5 since 1
}

schema glibre.tools.ActiveTab {
  version 1
  since   "0.1.0"

  field dock_path : string tag 1 since 1                     // canonical "/0/1/2" descent
  field panel_id  : string tag 2 since 1
}
```

**Invariants** (echo §4.2 invariants 1-3):

1. **Stable panel-id closure.** Every `panel_id` referenced (in
   `DockSplit` leaves, `FloatingPanel` entries, `ActiveTab` entries,
   and `active_viewport_id`) resolves to a panel registered in the
   current host's panel registry at load time. Unknown ids refuse
   load with `tools::Error::LayoutLoadFailed` and the prior active
   layout is preserved (§4.2 inv. 1).
2. **Versioned, monotonic.** `schema_version` mirrors the schema's
   own `version` field for forward-compat handshake. Loaders accept
   the current version and any older version reachable through the
   `data` migration table; an unknown future version refuses load
   (§4.2 inv. 2).
3. **No partial apply.** The loader either materialises every dock
   split, every floating panel, every active-tab record, and the
   viewport selection wholesale, or it leaves the previous active
   layout intact and returns `tools::Error::LayoutLoadFailed`
   (§4.2 inv. 3). Round-trip golden tests assert byte equality of
   `(load(write(p)) == p)` for every shipped profile fixture.
4. **DockSplit is a closed-recursive value.** A leaf (`axis == 2`)
   carries a non-empty `panel_id` and an empty `children` list; an
   internal node carries an empty `panel_id` and a non-empty
   `children` list. Mixed nodes refuse decode with
   `tools::Error::LayoutLoadFailed`. The recursion terminates by
   construction at a host-configured depth cap (mirrors the §9
   layout depth budget cell).
5. **Per-user, per-project; not transferred via game-state save.**
   `LayoutProfile` files live alongside the project on the local
   filesystem; they are not shipped inside `Scene` and never enter
   the `glibre_types_abi_hash` payload digest (the schema does
   contribute to `glibre_types_abi_hash` per §4.4 of the data spec —
   the *files* do not).

#### 7.1.2 `Scene` — tools-side scene reference document

**File:** `data/schemas/tools/Scene.fory`
**FQN:** `glibre.tools.Scene`
**Lifetime scope:** per-project, per-named-scene; written on scene
save from the toolbar, read at scene-open. Stored under
`<project-root>/scenes/<scene_name>.fory`.

The `Scene` is the **tools-side root document** that anchors a named
ECS world snapshot. **It does not own component payloads.** Game-world
ECS component bytes live in plugin-owned schemas under
`data/schemas/<owning-context>/<Type>.fory` (e.g. `core::Transform`,
`render::MeshHandle`, `physics::RigidBody`); `Scene` persists only the
**structural references** (entity ids, parent links, component-slot
references by `(TypeId, payload-bytes)` pair) plus the editor-side
metadata that the shell needs to re-open the scene in the same visual
state (last selection, last camera, last layout-profile binding).

The split is load-bearing: every domain plugin retains authority over
its own component encoding (PHILOSOPHY §3 — minimal core, plugin-only
growth), and tools' `Scene` document is a thin index that points at
those payloads. `Scene` is therefore an envelope, not a serializer.

```fory
schema glibre.tools.Scene {
  version  1
  since    "0.1.0"

  field scene_name        : string             tag 1 since 1
  field schema_version    : u32                tag 2 since 1
  field game_types_abi    : bytes              tag 3 since 1   // 32-byte blake3
  field entities          : list<EntityRecord> tag 4 since 1
  field root_entity_ids   : list<u64>          tag 5 since 1
  field saved_selection   : list<u64>          tag 6 since 1
  field saved_layout_ref  : string             tag 7 since 1   default ""
  field saved_viewport    : ViewportPose       tag 8 since 1
}

schema glibre.tools.EntityRecord {
  version 1
  since   "0.1.0"

  field entity_id      : u64                  tag 1 since 1
  field parent_id      : u64                  tag 2 since 1   default 0   // 0 == root
  field child_order    : u32                  tag 3 since 1
  field components     : list<ComponentSlot>  tag 4 since 1
  field bookmark_label : string               tag 5 since 1   default ""
}

schema glibre.tools.ComponentSlot {
  version 1
  since   "0.1.0"

  field type_id        : u64    tag 1 since 1
  field payload_schema : string tag 2 since 1                // FQN of the plugin-owned schema
  field payload_version : u32   tag 3 since 1
  field payload_bytes  : bytes  tag 4 since 1                // opaque to tools
}

schema glibre.tools.ViewportPose {
  version 1
  since   "0.1.0"

  field eye_x   : f32 tag 1 since 1
  field eye_y   : f32 tag 2 since 1
  field eye_z   : f32 tag 3 since 1
  field pitch   : f32 tag 4 since 1
  field yaw     : f32 tag 5 since 1
  field fov_deg : f32 tag 6 since 1   default 60.0
}

```

**Invariants:**

1. **Structural references only — payloads are opaque.** Tools never
   decodes `payload_bytes`. On scene-open, tools enumerates each
   `ComponentSlot`, looks up `(payload_schema, payload_version)` in
   the `data` `SchemaRegistry`, and dispatches the bytes through the
   middleman's per-type `deserialize_<fqn>` entry point (which may
   migrate). Unknown `payload_schema` returns
   `tools::Error::InspectorUnknownType` (the same closed-sum arm
   §4.4 inv. 3 already names) and the slot is dropped from the
   loaded scene with a diagnostics-sink warning; the scene still
   loads with the remaining slots present. This is **not** a
   half-applied scene — entities with at least one missing slot are
   loaded; `LoadFailed` is reserved for envelope-level corruption.
2. **`game_types_abi` is advisory provenance, not a load gate.**
   The hash records the middleman ABI used at write time. A mismatch
   at load is **not** a refusal — the per-slot `(payload_schema,
   payload_version)` migration path in invariant 1 already handles
   schema evolution. The hash exists for diagnostics-sink reporting
   and for the e2e trace replay seam (§4.9) which uses byte-equal
   provenance to assert deterministic replay.
3. **Selection persistence is best-effort.** `saved_selection` lists
   entity ids that may or may not still resolve in the current load
   (a plugin upgrade may have removed an entity). Unresolvable ids
   are dropped silently and a single `EditorEvent::SelectionChanged`
   fires after load (§4.3 inv. 1 — stale handles scrub at the next
   selection-event boundary). `saved_selection` is **not** the same
   as live cross-session selection; the §4.3 inv. 2 promise that
   selection does not persist holds for the run-to-run editor
   resource. `Scene` carries it explicitly as one document field for
   user convenience, not as a `Selection` resource snapshot.
4. **`saved_layout_ref` is a `LayoutProfile` name, not an inline
   layout.** A `Scene` references a layout profile by name (defined
   in §7.1.1); the empty string means "use the user's current
   active profile". Inline layout payload inside `Scene` is refused
   by codegen — the two schemas compose by reference, not by
   embedding, so layout edits and scene edits stay independent edit
   trails (§4.10 inv. 3 — one edit pipeline, scoped per aggregate).
5. **Per-project scope; cross-machine portable.** A `Scene` file is
   meaningful when paired with the same set of plugin dylibs (which
   own the payload schemas). Moving a scene to a host with a
   different middleman ABI hash is supported through the per-slot
   migration path; moving it to a host missing a payload's owning
   plugin yields per-slot `InspectorUnknownType` warnings per
   invariant 1.
6. **No engine resource pointers.** `Scene` carries no `RenderProxy`,
   no `World*`, no GPU handle — those are runtime-only (§3.3 → render).
   Every reference is by stable id (`Entity::bits`, `TypeId::value`,
   string `panel_id`).

#### 7.1.3 `CommandJournal` — opaque command-payload envelope

**File:** `data/schemas/tools/CommandJournal.fory`
**FQN:** `glibre.tools.CommandJournal`
**Lifetime scope:** per-session (in-memory) and per-recording (when
the `TraceRecorder` is active). The `CommandStack`'s in-memory
representation persists across plugin hot-reload of `tools` itself
(§4.10 inv. 9) by serialising through this envelope. **On-disk
persistence of the full journal is deferred post-MVP** per §3.2
collapse #6; the schema is defined now so:

- (a) `TraceRecorder` (§4.9) can capture `EditCommand` push events as
  `CommandJournalEntry` records inside the e2e trace stream.
- (b) The hot-reload path drains and re-hydrates the `CommandStack`
  through the envelope without inventing an ad-hoc wire format
  (`reviews/decisions/hot-reload-protocol.md`).
- (c) The post-MVP on-disk-history feature flips one boolean and
  reuses this schema unchanged.

```fory
schema glibre.tools.CommandJournal {
  version  1
  since    "0.1.0"

  field session_id  : u64                          tag 1 since 1
  field cursor      : u32                          tag 2 since 1
  field undo_depth  : u32                          tag 3 since 1
  field redo_depth  : u32                          tag 4 since 1
  field byte_budget : u64                          tag 5 since 1
  field entries     : list<CommandJournalEntry>    tag 6 since 1
}

schema glibre.tools.CommandJournalEntry {
  version 1
  since   "0.1.0"

  field sequence       : u64               tag 1 since 1
  field group_id       : u64               tag 2 since 1   default 0   // 0 == not grouped
  field kind           : u8                tag 3 since 1               // CommandKind discriminant
  field payload_bytes  : bytes             tag 4 since 1               // Fory-encoded payload, kind-determined
  field pre_selection  : SelectionSnapshot tag 5 since 1
  field post_selection : SelectionSnapshot tag 6 since 1
  field byte_estimate  : u64               tag 7 since 1
  field timestamp_ns   : u64               tag 8 since 1
}

schema glibre.tools.SelectionSnapshot {
  version 1
  since   "0.1.0"

  field hash     : u64       tag 1 since 1
  field entities : list<u64> tag 2 since 1                            // Entity::bits values
}
```

**`CommandKind` (closed sum, mirrors §5 `EditCommandPayload`).**
The `kind` byte is a stable u8 discriminant matching the variant order
of the `EditCommandPayload` `std::variant` declared in §5.10:

| `kind` value | Variant                  | `payload_bytes` schema FQN                  |
|--------------|--------------------------|---------------------------------------------|
| 0            | `edit::ComponentEdit`    | `glibre.tools.ComponentEditPayload`         |
| 1            | `edit::EntityAdd`        | `glibre.tools.EntityAddPayload`             |
| 2            | `edit::EntityRemove`     | `glibre.tools.EntityRemovePayload`          |
| 3            | `edit::ParentChange`     | `glibre.tools.ParentChangePayload`          |
| 4            | `edit::AssetSlotBind`    | `glibre.tools.AssetSlotBindPayload`         |

Each `<Variant>Payload` is its own `data/schemas/tools/<Variant>Payload.fory`
(elided here for brevity; their shape mirrors the §5.10 structs by
field). Tools owns the **envelope** schema; the `*Payload` schemas wrap
plugin-owned component byte spans (`previous_bytes` / `next_bytes`
inside `ComponentEditPayload`, etc.). The component bytes themselves
are opaque to tools and decoded only by the originating context's
schema at apply / undo time.

**Invariants:**

1. **Closed-sum `kind`.** `kind` values outside the table above
   refuse decode with `tools::Error::CommandConflict` (the same arm
   §4.7 inv. 2 already names — apply / undo non-inverse and
   structural malformation share a refusal arm). Adding a sixth
   variant requires a coordinated codegen bump (see §7.2.3).
2. **Opaque payload bytes; tools never decodes.** Tools knows the
   payload's schema FQN and version (via the kind table) but decodes
   bytes only at `apply()` / `undo()` time inside the receiving
   context's territory. A `CommandJournalEntry` whose
   `payload_bytes` cannot be decoded by the receiving context
   surfaces as `core::Error::SchemaMigrationFailed` from the
   `data` middleman, which tools translates into
   `tools::Error::CommandConflict` at the stack boundary (§4.7
   inv. 2 — apply / undo not inverse implies pre-state cannot be
   reconstructed).
3. **Sequence is monotonic per session.** `sequence` strictly
   increases with push order; `cursor` partitions
   `entries[..cursor)` (the undo half) from `entries[cursor..)`
   (the redo half) per §4.7 inv. 1. Decode rejects out-of-order
   `sequence` values.
4. **Group membership encodes `Transaction`.** Entries sharing a
   non-zero `group_id` form one atomic undo step (§4.7 inv. 3); the
   stack treats them as a single user-visible operation. A
   `group_id == 0` entry is ungrouped. Mixed grouped /ungrouped
   contiguity is allowed; sparse `group_id` values are allowed
   (gaps from coalesce eviction).
5. **`byte_estimate` is advisory, not a checksum.** It mirrors the
   in-memory `EditCommand::byte_estimate()` for budget-cap accounting
   only. It is **not** validated against `payload_bytes.size()` at
   decode (the relationship is lossy because Fory's tagged-binary
   encoding adds per-field overhead the in-memory estimate ignores).
6. **`SelectionSnapshot` round-trips Selection's `hash + entities`
   pair byte-equal.** `hash` is the deterministic order-stable hash
   of `entities` (§4.3 inv. 2). Decode rejects entries whose
   computed hash mismatches the stored hash with
   `tools::Error::CommandConflict` — the snapshot is then untrustable
   for `apply()` / `undo()` selection restoration.
7. **Not on disk in MVP.** No editor binary in MVP writes a
   `CommandJournal` file to disk. The schema's only live readers are
   (a) the e2e `TraceRecorder` (§4.9) — which writes
   `CommandJournalEntry` records inside the `.glibre-trace` stream,
   not standalone files — and (b) the hot-reload barrier (§8) which
   serialises the in-memory journal to a heap arena for swap
   survival. Post-MVP enables a `<project-root>/.glibre/tools/journal/`
   directory; the schema does not change when that flag flips.

#### 7.1.4 `Shortcuts` — keymap binding actions to chords

**File:** `data/schemas/tools/Shortcuts.fory`
**FQN:** `glibre.tools.Shortcuts`
**Lifetime scope:** per-user, project-agnostic by default with
optional project-level overrides. Stored under
`<user-data>/glibre/tools/shortcuts.fory` (default ring) and
`<project-root>/.glibre/tools/shortcuts.fory` (project override).
Read at editor startup; written on user-binding edit.

```fory
schema glibre.tools.Shortcuts {
  version  1
  since    "0.1.0"

  field schema_version : u32                tag 1 since 1
  field profile_name   : string             tag 2 since 1   default "default"
  field bindings       : list<KeyBinding>   tag 3 since 1
  field disabled       : list<string>       tag 4 since 1               // action ids whose default is suppressed
}

schema glibre.tools.KeyBinding {
  version 1
  since   "0.1.0"

  field action_id : string tag 1 since 1                              // e.g. "tools.command.undo"
  field chord     : Chord  tag 2 since 1
}

schema glibre.tools.Chord {
  version 1
  since   "0.1.0"

  field modifier_mask : u8     tag 1 since 1                          // bit-OR of Modifier flags
  field key           : u32    tag 2 since 1                          // platform::Key ordinal
  field repeats       : bool   tag 3 since 1   default true
}
```

**`Modifier` flags:** `Ctrl=1, Shift=2, Alt=4, Cmd=8` (bit positions
mirror `glibre::platform::Modifier`; bit additions are append-only —
see §7.2.4 below).

**Invariants:**

1. **Action id closure.** Every `action_id` in `bindings` resolves to
   an action registered with the host's action registry at load
   time. Unknown ids are dropped from the loaded keymap with a
   diagnostics-sink warning; the load itself does not fail (a
   stale binding is a soft failure — the user can still use the
   editor). This is **not** the same closure rule as `LayoutProfile`
   panel ids (§7.1.1 inv. 1) where unknown ids fail the load: a
   shortcut with no action is a no-op; a layout with an unknown
   panel breaks the dock graph.
2. **Chord uniqueness.** Within one `Shortcuts` document, no two
   `KeyBinding` entries share the same `(modifier_mask, key)` pair.
   Duplicate chords refuse decode with
   `tools::Error::CommandConflict` (a chord collision is structurally
   identical to a panel-id collision — same closed-sum refusal arm,
   per §4.2 inv. 1).
3. **Override layering.** When a project-level `shortcuts.fory`
   exists, it layers over the user-default ring: project bindings
   replace the user default for the same `action_id`; project
   `disabled` entries suppress the user default. The merged result
   is the live keymap; neither file is mutated by the merge.
4. **Closed-sum modifier set.** `modifier_mask` bits beyond the four
   defined flags refuse decode (`tools::Error::CommandConflict`).
   Adding a new modifier (e.g. a future `Hyper`) is an additive
   schema change per §7.2.4.
5. **Versioned, monotonic.** `schema_version` mirrors the schema's
   own `version` field; loaders accept the current version and any
   older version reachable through the `data` migration table; an
   unknown future version refuses load with
   `tools::Error::LayoutLoadFailed` (the same arm
   `LayoutProfile` uses; both are versioned-config-load failures).

### 7.2 Migration rules

Per `reviews/decisions/fory-codegen.md` §"Migration Mechanic", every
schema-version bump emits a generated dispatcher hookup. Tools owns
the migration *bodies* for the types above; their *plumbing* is
generated. Migration bodies live under
`src/tools/migrations/<type>_v<N>_to_v<N+1>.cpp`.

#### 7.2.1 `LayoutProfile` migrations — additive only

Layout profiles follow the **additive defaulted-field** pattern:

1. **N → N+1 adds a field at a new tag.** Default value defined in
   the schema; codegen synthesises the default at deserialise time
   when the payload omits the new field (Fory's `since` clause). No
   migration body required; the codegen tool emits
   `migrate_LayoutProfile_v<N>_to_v<N+1>` as the identity mapping
   with default-fill. Examples that fit this rule: a future
   `accent_color`, `panel_pinned`, `tab_overflow_strategy`.
2. **N → N+1 adds a new variant to `DockSplit::axis` or any other
   embedded closed-sum byte.** Treated as additive when the new
   variant has a representable default that older code can ignore
   (typically not the case — closed-sum bumps usually need a body).
   When a body is required: the migration synthesises the variant
   into a representable older shape (e.g. a new `axis` variant
   collapses to a horizontal split) and the previous behaviour is
   preserved.
3. **N → N+1 changes the meaning of an existing field.** Treated as
   breaking. The schema bumps to a new major; tools commits to
   **no breaking layout changes inside MVP**. The migration body
   lives in `src/tools/migrations/layout_profile_v<N>_to_v<N+1>.cpp`
   and is reviewed against §4.2 inv. 3 (no half-applied profile).
4. **N → N+1 removes a field.** Tag becomes `reserved`; never
   reused. Codegen rejects reuse at generation time per data SPEC
   §7. The migration body discards the value with a diagnostic
   warning; downstream code re-derives any dependent state from the
   surviving fields.

Round-trip golden test contract (mandatory):
`tests/data/schemas/tools/LayoutProfile.cpp` includes a recorded
`vN` payload for every shipped schema version `N` and asserts
`migrate(vN) == defaults_for_v_current()` modulo the explicitly-set
fields in the recorded payload, plus the byte-equal round-trip
`load(write(p)) == p` for every fixture profile.

#### 7.2.2 `Scene` migrations — additive envelope, payload migration delegated

`Scene` evolution follows two independent rules because the schema is
an envelope:

1. **Envelope-level changes (the `Scene` and `EntityRecord` schemas
   themselves).** Additive defaulted-field pattern, identical to
   §7.2.1 case 1. Examples: a future `layer_id` on `EntityRecord`,
   a future `comment` on `Scene`. The migration body, if any, is
   the identity-with-default-fill the codegen tool emits.
2. **Per-slot payload changes (the schemas referenced by
   `payload_schema`).** **Not tools' concern.** Each plugin owns its
   own component schema's migration body in
   `src/<context>/migrations/<Type>_v<N>_to_v<N+1>.cpp`. Tools'
   per-slot decode loop calls the `data` middleman's
   `deserialize_<fqn>` entry point, which dispatches through the
   plugin-owned migration chain. Tools sees only the loaded T value
   or the migration error; it does not author migration bodies for
   non-tools schemas. This is the load-bearing boundary the user
   brief calls out: "ECS data lives in plugin-owned schemas; tools
   persists structural references + selection state".
3. **`game_types_abi` mismatch handling.** Per §7.1.2 inv. 2, an
   ABI hash mismatch between scene and host is an advisory log,
   not a refusal — the per-slot migration chain (rule 2) is the
   load gate, not the envelope hash. Migration bodies therefore
   never read `game_types_abi`.

Round-trip golden test contract:
`tests/data/schemas/tools/Scene.cpp` includes a recorded `vN` envelope
*plus* a fixture set of plugin-owned payload schemas pinned at known
versions; the test asserts the envelope round-trips byte-equal and
that the per-slot decode yields the expected (component, value) pairs.
Plugin-owned migration tests live under
`tests/data/schemas/<ctx>/<Type>.cpp` per §7 of each owning context.

#### 7.2.3 `CommandJournal` migrations — additive envelope, additive variant set

`CommandJournal` follows the same envelope / payload split as `Scene`,
plus a third rule for the closed `CommandKind` discriminant:

1. **Envelope-level additive bumps.** Identical to §7.2.1 case 1.
2. **Per-payload-schema bumps (the `*Payload` schemas).** Tools owns
   these payload schemas because they wrap tools' edit-command
   variants — but the **inner component bytes** carried inside
   (e.g. `ComponentEditPayload::previous_bytes`) are still opaque
   plugin payloads and migrate through their owning context's chain.
   The `*Payload` envelope itself uses additive defaulted fields.
3. **Adding a new `CommandKind` variant (sixth and beyond).**
   Closed-sum schema bump: the discriminant byte is **append-only**
   in value (new variant gets `kind = 5`, never reuses 0..4); a new
   `data/schemas/tools/<NewVariant>Payload.fory` ships alongside;
   the §5.10 `EditCommandPayload` `std::variant` gains a
   corresponding alternative in the same authoring change so
   `kind` byte ↔ variant index stays 1:1. The codegen tool refuses
   to emit a `CommandKind` table whose discriminant assignment does
   not match the §5.10 variant order; this catches forgot-to-bump
   regressions at build time.

Round-trip golden test contract:
`tests/data/schemas/tools/CommandJournal.cpp` records one fixture
journal per shipped schema version with at least one entry per
`CommandKind` variant; the test asserts byte-equal round-trip and
that the discriminant-table covers every variant of the §5.10
`EditCommandPayload` variant alternative set (a one-line
`static_assert` in the test guards future drift).

#### 7.2.4 `Shortcuts` migrations — additive bindings, append-only modifier bits

1. **Adding a new `action_id` to the default ring.** Pure code-side
   change; no schema bump. The new binding ships as a static default;
   user-saved keymaps without the binding inherit the default at
   load time (action-id closure rule §7.1.4 inv. 1 already drops
   unknown ids gracefully — the *opposite* direction, missing
   defaults, is just inheritance).
2. **Adding a new `Modifier` flag bit.** Bit positions are
   append-only (§7.1.4 inv. 4); the new bit reserves the next free
   position in the `modifier_mask` u8 and rebuilds the dylib. The
   `.fory` schema is unchanged. Older keymaps read on a newer build
   present a zero in the new bit position; the gating rule treats
   that as "modifier not held" — the safe default. Promoting
   `modifier_mask` from `u8` to `u16` is a breaking schema bump
   (new tag, defaulted to zero, old field marked reserved); the
   migration body lives in
   `src/tools/migrations/shortcuts_modifier_widen_v<N>_to_v<N+1>.cpp`.
3. **Adding a new field on `Chord` (e.g. `os_only : u8` to scope a
   chord to a specific OS).** Additive defaulted-field pattern,
   identical to §7.2.1 case 1.
4. **Override layer evolution.** The `disabled` list is a value
   type — appending entries is forward- and backward-compatible.
   Removing the project-level override file falls back to the
   user-default ring per §7.1.4 inv. 3.

Round-trip golden test contract:
`tests/data/schemas/tools/Shortcuts.cpp` includes a recorded `vN`
keymap fixture with one binding per built-in `action_id`, asserts
byte-equal round-trip, and asserts that loading a `vN` fixture on
the current build maps every action that exists in both versions
(unknown-action drops are tested in a sibling case).

### 7.3 What is NOT persisted

To make the boundary explicit (in line with §5 "Serialised schemas":
Fory exhausts the persistent surface — every other tools-owned
artefact is runtime-only):

| Artefact                 | Why not persisted                                                                                                           |
|--------------------------|-----------------------------------------------------------------------------------------------------------------------------|
| `EditorEvent` bus        | Republished from `core`'s event bus per §3.2 collapse #9; in-memory only.                                                   |
| `Selection` resource     | Cross-session not promised (§4.3 inv. 2). `Scene::saved_selection` is an explicit one-shot doc-field, not a `Selection` mirror. |
| `CommandStack` (live)    | In-memory only; serialised through `CommandJournal` only at hot-reload barrier and trace capture, never as a standalone file in MVP (§7.1.3 inv. 7). |
| `Gizmo` drag state       | Per-frame transient; commits land as `EditCommand`s on the stack (§4.5 inv. 1).                                             |
| `Toolbar` mode mirror    | Reads from `EditorMode` (§4.8 inv. 3); never owns state.                                                                    |
| `AssetThumbnail` LRU     | Display-only cache; `content` owns asset persistence (§4.6 inv. 3).                                                         |
| `ReflectionBlob` views   | Read-through immutable views over `data`'s registry; no copy, no persistence (§4.4 inv. 1).                                 |
| `TraceRecorder` (live)   | Resource lifecycle is recording-bounded (§4.9); the *output* `.glibre-trace` is owned by `specs/e2e/SPEC.md`, **not tools**. |
| `EditorMode` resource    | Per-session value; resets to `Edit` on startup.                                                                             |
| Per-frame Dear ImGui draw lists | Extract slot in `render::RenderFrame`; one frame's lifetime (§4.1 inv. 4).                                            |
| Profiler / Console output | Read-through over `core` and `render` measurement; tools owns display only (§3.2 collapse #8).                              |

These appear in the persistence surface only as **identifiers** —
`Entity::bits` from `Scene::EntityRecord`, `TypeId::value` from
`ComponentSlot::type_id`, panel id strings from `LayoutProfile` —
never as byte payloads. Cross-context: `TraceFile` (`.glibre-trace`)
is **owned by `specs/e2e/SPEC.md`** and cited from §4.9 only as the
artefact tools writes through; its schema and migration story live in
the e2e spec.

## 8. Hot-Reload Contract

This section specialises the engine-wide hot-reload protocol
(`reviews/decisions/hot-reload-protocol.md` — drain → swap → migrate →
resume) to the **tools plugin**. It defines exactly which tools-owned
state survives a swap, what `migrate(...)` must do, and which conditions
cause tools' reload attempt to be refused with the engine's standard
`core::Error::HotReloadRefused` arm. Engine-wide concerns (per-plugin
atomicity, the four-step state machine, observer-bus event shapes, the
three umbrella refusal arms, the `enqueue_hot_reload` E2E hook) are not
re-stated here — see the protocol record. Tools fills only the four
pluggable points the protocol leaves to each plugin: drain side-effects,
survival inventory, migrate body, and register-time rehydration.

Tools is unusual among glibre plugins because it observes **two**
hot-reload trigger classes that demand different responses:

1. **Tools-plugin self-reload** — the editor `.dylib` itself swaps at
   phase 8. **Rare in practice** (the running editor ordinarily restarts
   when its own image changes), but the protocol still covers it for
   determinism and for in-process E2E swaps. The tools shell's panel
   draw closures, gizmo body, inspector form code, and trace recorder
   wire-format reside in the tools dylib; the swap repoints those
   without losing user state.
2. **Game-plugin reload** (e.g. `render`, `physics`, gameplay-framework
   plugins) — **the more common case** in dev workflows. The tools
   image is unchanged, but the type registry, asset graph, and entity
   identities the editor inspects shift underneath it. Tools must
   *reseat its handles* across the swap rather than swap its own code.

Both cases run inside the same protocol; what differs is which of the
four steps does meaningful work for tools and which is a no-op. The
sub-sections below state both responsibilities side-by-side.

### 8.1 Reload point — phase 8, never mid-frame

The engine schedule (`reviews/decisions/frame-phases.md`) places the
hot-reload barrier at phase 8, **after `render-submit` (phase 7) and
before `present` (phase 9)**. Tools' reload protocol is anchored to
that one slot and refuses any other.

At phase 8 entry, tools' in-flight state is:

1. **No Dear ImGui draw list is being recorded.** Phase 6 of the
   current frame already extracted tools' draw lists into `render`'s
   `RenderFrame` slot (§4.1 inv. 4 / §4.10 inv. 7); phase 7 already
   submitted. No panel `draw()` callback is on any thread — tools'
   work for frame N is done.
2. **No `EditCommand` is mid-apply.** `CommandStack::apply` /
   `undo` runs only inside the `EditorWorld`'s phase 5 (§4.7 inv. 4
   single-writer); that phase has retired for the editor world's
   frame N before phase 8 begins. The stack's invariants 1–7 hold at
   the barrier.
3. **No `Transaction` is open.** The protocol refuses to begin
   drain while any `Transaction` (§4.7) is in flight; the loader's
   pending-reload counter is consumed only when the editor world's
   transaction depth is zero. (In normal frame execution the depth
   resets to zero at every phase 5 exit; an open transaction at
   phase 8 entry is a contract violation, not a refusal.)
4. **No `Selection` mutation is in flight.** Selection mutates
   only through `CommandStack` apply/undo or through scene-tree
   click handlers in the editor world's phase 1 input pump (§4.3
   inv. 1); both have retired before phase 8.

These conditions are tools' half of the protocol's "drain"
postcondition (protocol §"Step 1 — Drain"). Tools'
`glibre_plugin_drain` body therefore has nothing to flush from the
draw / apply paths; its work is the journal-snapshot + observer
notification described in §8.3.

**Mid-frame reload is refused.** Any reload request that arrives during
phases 1–7 (of either the editor world or the game world) is queued,
never applied; the loader's `pending_reloads` counter is consumed only
at phase 8 entry per protocol step 1. The editor world's frame loop
yields exclusive ownership to the loader during phase 8; no panel
`draw()`, no `EditCommand::apply`, and no `Selection` mutation runs
while drain → swap → migrate → resume executes.

### 8.2 Survival inventory

The engine-wide survival rule is mechanical: **state with a `.fory`
schema in `glibre-types.dylib` survives across the swap; state without
one does not** (protocol §"State Survival Rules"; PHILOSOPHY collapse:
one check, not a per-aggregate manifest). Tools owns four persistent
fory-schema'd types (§7.1.1 `LayoutProfile`, §7.1.2 `Scene`, §7.1.3
`CommandJournal`, §7.1.4 `Shortcuts`) and a collection of in-memory
runtime state. The table below classifies every tools-owned aggregate
against that rule and adds the tools-specific reasoning per-row.

| Tools-owned state                                                                  | Persistence path             | Survives swap? | Reasoning                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
|------------------------------------------------------------------------------------|------------------------------|----------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `LayoutProfile` slot map (§4.2 / §7.1.1)                                           | `.fory` schema, middleman    | Yes — bytes are owned by `glibre-types`; tools only reads them. The active profile name + the on-disk profiles persist; the live `Layout` value object is destroyed in drain and rebuilt in resume by re-loading from the surviving `LayoutProfile` (§8.3.1).                                                                                                                                                                                                  |
| `Selection` resource (§4.3)                                                        | None — runtime resource over middleman ids | Yes for the *id set*; live indices are re-resolved on resume. `Selection::entities` is a `list<u64>` of `Entity::bits` values that live in `glibre-types.core` and are stable across plugin reloads (§4.3 inv. 1). After a game-plugin reload, ids that no longer resolve in the new game world are scrubbed and `EditorEvent::SelectionRevalidated` is emitted (§8.3.2 / §8.5).                                                                                |
| `CommandStack` undo + redo arrays (§4.7)                                           | `.fory` schema via §7.1.3 envelope | Yes via journal snapshot. The live `EditCommand` array is serialised through `CommandJournal` (§7.1.3 inv. 7) into a loader-owned arena at drain entry; resume rebuilds the array from the snapshot. **Closed-sum `kind` invariant guarantees replayability**: §7.1.3 inv. 1 / §7.2.3 require the variant set to be **strictly additive** — old commands serialised before the swap remain decodable by every future tools image.                                |
| `Transaction` in-flight buffer                                                     | None                         | N/A — no active transaction permitted at phase 8 entry (§8.1 condition 3). The buffer is empty by construction.                                                                                                                                                                                                                                                                                                                                              |
| `Shortcuts` keymap (§4.8 / §7.1.4)                                                 | `.fory` schema, middleman    | Yes — bytes owned by `glibre-types`; the live keymap is rebuilt in resume from the surviving record.                                                                                                                                                                                                                                                                                                                                                          |
| `Scene` reference document (§7.1.2)                                                | `.fory` schema, middleman    | Yes — owned by `glibre-types`; tools only reads / writes through the schema. The on-disk doc is unaffected by either reload class.                                                                                                                                                                                                                                                                                                                            |
| `EditorMode` resource                                                              | None                         | Yes — value preserved across both reload classes; the mode value is a u8 enum mirrored into `glibre-types.tools` (§4.10 inv. 2). Tools-plugin self-reload while in `Recording` mode is **refused** under §8.4 (a recording is "user state in flight"); other modes survive verbatim.                                                                                                                                                                            |
| `TraceRecorder` live state (§4.9)                                                  | None                         | Yes for the *recording cursor*; **paused across the reload window** per §8.6. The recorder's `op_count` and current file handle are mirrored into a middleman-typed `TraceRecorderResume` value at drain; resume re-opens the file in append mode and resumes capture at frame N+1. The reload window emits no `TraceOp`s (§4.9 inv. 1 non-perturbing — the swap is not an editor action).                                                                       |
| Floating `PanelHost` instances (§4.2)                                              | None                         | **No** for tools-plugin self-reload — dropped in drain, re-created in resume from the active `LayoutProfile`. Yes for game-plugin reload — panel registrations belong to tools' image, which is unchanged.                                                                                                                                                                                                                                                       |
| `Panel` registrations from tools' own image                                        | None                         | No for tools-plugin self-reload (registry slots cleared in drain, re-populated in resume); yes for game-plugin reload.                                                                                                                                                                                                                                                                                                                                          |
| `Panel` registrations from *other* plugins (e.g. a future game-framework's debug panel) | None                    | Yes for tools-plugin self-reload (the registering plugin's image is unchanged; tools' resume re-applies the layout against the surviving registration list); transparently re-registered by the reloading plugin's own resume for game-plugin reload.                                                                                                                                                                                                          |
| `Inspector` `InspectorView` cache + `ReflectedField` closures (§4.4)               | None                         | **No** for both reload classes — every view holds closures over the type registry's Fory descriptors; those descriptors are owned by `data` middleman but the *closure code* lives in tools' image (self-reload) or in the reloaded plugin's image (game-plugin reload, since field descriptors come from per-context schemas). Resume invalidates the cache and rebuilds lazily on next `Selection` read (§8.3.3).                                            |
| `Gizmo` configuration (`GizmoFrame`, `GizmoConstraint`, `Snap`)                    | None — middleman value types | Yes — values mirrored into `glibre-types.tools.GizmoConfig`; the tab values survive both reload classes. The drag-state ephemeral data (current axis-hover, drag origin) is dropped on swap and reset to neutral on resume.                                                                                                                                                                                                                                       |
| `AssetThumbnail` LRU cache (§4.6)                                                  | None                         | No for both reload classes — display cache only (§7 non-persistent table). Resume rebuilds on demand from `content` and `render`.                                                                                                                                                                                                                                                                                                                              |
| `EditorEvent` republish queue                                                      | None                         | Drained before phase 8 (synchronous bus); phase 8 publishes `LayoutChanged` and `SelectionRevalidated` (§8.5) atomically.                                                                                                                                                                                                                                                                                                                                       |
| Per-plugin worker thread pools (e.g. asset thumbnail decoder)                      | None                         | No for tools-plugin self-reload — destroyed by tools' `glibre_plugin_drain`, re-spawned by the new image's `glibre_plugin_register`. Yes for game-plugin reload (tools' threads are unaffected).                                                                                                                                                                                                                                                              |
| Profiler / Console rolling buffers                                                 | None — debug-gated           | Reset on tools-plugin self-reload; preserved on game-plugin reload. Per `frame-phases.md` §Notes, debug surfaces are not contractual.                                                                                                                                                                                                                                                                                                                          |

The rule mechanically applied: every "Yes" row has a `.fory` schema or
references middleman-typed bytes; every "No" row is private to the
reloading plugin's image. The split between **tools-plugin self-reload**
and **game-plugin reload** is decided per-row by which image owns the
underlying code, not by per-aggregate opt-in.

### 8.3 `migrate(...)` body — tools' responsibilities

The protocol's `migrate` step (protocol §"Step 3 — Migrate") runs *pure*
per-row migrate functions for every persistent-component-type schema
bump. Tools owns four such bodies (§7.2.1 `LayoutProfile`, §7.2.2
`Scene`, §7.2.3 `CommandJournal`, §7.2.4 `Shortcuts` — all additive in
MVP). Those functions are the standard pure migrate signature; nothing
here changes them.

What this section adds is the **tools-plugin-specific portion of step
4 (resume)** — the work the new image's `glibre_plugin_register` must
do to repoint live runtime state at the new code while reusing the
surviving bytes. Three pointer fix-ups matter; which run depends on
the reload class.

#### 8.3.1 Tools-plugin self-reload — drop floating PanelHosts; re-create from LayoutProfile

Runs only when **tools' own image** is the swapping plugin.

The drain step (§"Step 1") releases:

1. Every floating `PanelHost` window (the OS-window children of the
   shell that hold tear-off panels). Their byte representations live
   in tools' image; their position / size are already mirrored into
   the active `LayoutProfile`'s `floats` field (§7.1.1) — losing the
   live host objects loses no user state.
2. Every panel `draw()` closure pointer in the panel registry.
3. The thumbnail-decoder thread pool (§4.6).
4. The `InspectorView` cache (its closures point at tools-image lambdas).

The resume step (§"Step 4") rebuilds:

1. Re-registers tools' default `Panel` set (Scene, Inspector,
   Assets, Console, Profiler, Viewport, Toolbar) into the host's
   panel registry. Re-registration of the same `panel_id` is
   idempotent per protocol step 4.1.
2. Re-loads the active `LayoutProfile` from the surviving slot map
   (§7.1.1) and re-applies it: every `DockSplit` leaf, every
   `FloatingPanel` (recreating one OS-window child per entry), and
   every `ActiveTab` is materialised against the freshly-registered
   panel ids. Failure of this re-apply is the §8.4 unsaved-edits
   refusal: any panel id that no longer resolves drops to the
   protocol's standard `core::Error::HotReloadRefused` with cause
   `tools::Error::LayoutLoadFailed` (§4.2 inv. 1).
3. Spins the thumbnail-decoder thread pool back up.
4. The `InspectorView` cache stays empty; views rebuild lazily on
   the next `Selection` read.

`EditorEvent::LayoutChanged` (§8.5) fires exactly once at the end of
this fix-up sequence, before the protocol's `HotReloadCompleted`
event, so observers see a fully-laid-out shell.

#### 8.3.2 Game-plugin reload — invalidate Inspector views; refresh Selection target ids

Runs when **any plugin other than tools** swaps. Tools' image is
unchanged; the engine's protocol calls tools' (already-loaded)
post-swap callback to reseat handles.

Tools registers a `core::HotReloadObserver` callback (the engine
exposes the same observer bus that emits `HotReloadStarted` /
`HotReloadCompleted`, see protocol §"Observer Notification") that:

1. **Invalidates the `InspectorView` cache** for every cached view
   whose component type's schema FQN is owned by the reloading
   plugin. The cache is keyed by `(Entity, ComponentType)`; the
   invalidation walks the cache once and drops matching entries.
   The next `Inspector` draw pass rebuilds them lazily against the
   reloaded plugin's new `ReflectionBlob` descriptors.
2. **Re-resolves `Selection` target ids.** Every `Entity::bits` in
   `Selection::entities` is queried against the post-reload game
   world's entity allocator (the allocator survives the reload —
   middleman-typed; the entries inside it may have been migrated
   per the reloaded plugin's persistent-component schema bump).
   Stale ids — entities the reload's own migrate functions may have
   despawned — are scrubbed; survivors are kept in their original
   deterministic order (§4.3 inv. 2). If any id was scrubbed,
   `SelectionRevalidated` (§8.5) is emitted exactly once, carrying
   the pre / post selection hashes and the count of scrubbed ids.
3. **Refreshes any cached `ReflectionBlob` views** held by open
   panels (Inspector, Console with object-pretty-print). The blobs
   themselves come from `data`'s middleman registry; what tools
   caches is the per-component pointer. Pointers from the reloaded
   plugin's image are repointed; pointers from un-affected plugins
   are untouched.
4. **Does not touch `CommandStack`**. Past commands are replayable
   by construction (§8.2 row: closed-sum `kind`, additive variant
   set). `payload_bytes` are decoded by the receiving context at
   `apply()` time, so a reload that bumps a payload schema's
   version is handled by the *data* middleman's migration table,
   not by tools — tools sees no difference. (A payload schema bump
   without a migrate function would surface as
   `tools::Error::CommandConflict` per §7.1.3 inv. 2 *only when the
   user actually tries to undo across the bump*; the swap itself
   does not validate every payload.)

This callback runs synchronously on the loader thread, between
protocol steps 4.2 and 4.3 (so subscribers see fully-resolved
selection state at `HotReloadCompleted` time). Total work is
**O(distinct cached `InspectorView`s) + O(|Selection|) + O(open
inspector panel rows)** — well inside the protocol's one-frame
stall budget (`hot-reload-protocol.md` §Consequences).

#### 8.3.3 Inspector lazy rebuild — common to both reload classes

The `InspectorView` cache rebuilds on demand from the post-reload
type registry the next time the Inspector panel draws. The first
`Inspector::draw` after a swap walks the current `Selection`,
queries `data`'s `ReflectionBlob` for each `(Entity, ComponentType)`
pair, and constructs a fresh `InspectorView`. Cache misses are
bounded by `|Selection| × distinct ComponentType count`; for the
MVP `≤ 64` selected entities and `≤ 32` component types per entity
target, the rebuild fits in a single editor frame. Rebuild failures
(unknown component type) emit `tools::Error::InspectorUnknownType`
per §4.4 inv. 5 and the row is omitted; the error is *not*
escalated to a hot-reload refusal (§8.4 — refusal cases are
narrowly scoped).

The total work in tools' resume step is therefore bounded by:

- Self-reload: **O(panels in active LayoutProfile) panel
  re-registrations + O(thread pool size) thread spawns + zero
  `EditCommand` re-applies** (the journal snapshot is restored as
  bytes, not replayed).
- Game-plugin reload: **O(cached InspectorViews) invalidations +
  O(|Selection|) id queries + zero panel churn**.

Both stay inside the protocol's "reload path bounded by drain +
swap + Σ migrate + register" budget.

### 8.4 Refusal cases (tools-specific)

Tools contributes no new umbrella refusal arm; every refusal is
expressed as the engine-wide `core::Error::HotReloadRefused` with a
nested cause chosen from the protocol's existing arms. Tools introduces
two **inner causes** that the loader sees only because tools is the
plugin being reloaded (self-reload case) or because tools' observer
callback raises them (game-plugin reload case). Both roll up to
`core::Error::PluginInitFailed` per protocol §"Refusal Cases" item 3
unless noted otherwise.

| Tools refusal cause                                            | Reload class triggering it       | Detected by                                                                                                              | Inner-error arm                                                | What the operator must do                                                                                                                                                                                                                                                                |
|----------------------------------------------------------------|----------------------------------|--------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Unsaved edits in flight at swap entry**                      | Tools-plugin self-reload only.   | Tools' `glibre_plugin_drain` reads the `CommandStack`'s undo-depth and the active-recording flag (`TraceRecorder::is_recording()`). Either non-zero → refusal. | `tools::Error::Refused` wrapped under `core::Error::HotReloadRefused` (direct, not under `PluginInitFailed` — refusal happens before swap). | Operator-facing prompt: "Save / discard / continue editing." Saving routes through the normal `Scene` write path; discarding clears the `CommandStack`; continuing leaves the request queued for the next phase 8 at which both flags are zero. The prior tools image keeps running unmodified. |
| **Active `Transaction` open at phase 8 entry**                 | Both classes (loader-detected, listed for completeness). | Loader (not tools). Tools' contract refuses to begin drain while transaction depth > 0; the loader treats this as a queued request, never an error. | Deferred — the request will be honoured at the next phase 8 with depth zero (typically the next frame's phase 8 since transactions span ≤ one editor frame per §4.7 inv. 3). | None — the request will be honoured automatically.                                                                                                                                                                                                                                       |
| **Layout re-apply fails after self-reload**                    | Tools-plugin self-reload only.   | Tools' `glibre_plugin_register` calls `LayoutProfile::activate` on the surviving active-profile name; an unknown panel id (e.g. a panel from an extension that *also* swapped and dropped a registration) raises `tools::Error::LayoutLoadFailed`. | `tools::Error::LayoutLoadFailed` wrapped under `core::Error::PluginInitFailed`. | Inspect which `panel_id` is missing in the active `LayoutProfile`; either restore the dropping plugin or switch to the `default` `LayoutProfile`. The reload is refused; the prior tools image keeps running.                                                                              |
| **Closed-sum `kind` violation in surviving CommandJournal**    | Tools-plugin self-reload only.   | Tools' `glibre_plugin_register` decodes the loader's `CommandJournal` snapshot; a `kind` value outside the §7.1.3 closed sum surfaces from the data middleman. By construction (§7.2.3 — variant set is strictly additive across MVP), this only fires on a downgrade or on a corrupted snapshot. | `tools::Error::CommandConflict` wrapped under `core::Error::PluginInitFailed`. | Either upgrade the tools image to a version that recognises the journal's variant set, or accept the loss by discarding the snapshot (the default behaviour is to refuse, preserving user history).                                                                                       |

Each refusal is logged exactly once at `warn` level (protocol §"Refusal
Cases") with structured fields: `plugin_fqn=glibre.tools`,
`attempted_dylib_path`, `host_abi_hash`, `plugin_abi_hash`, and the
inner cause's enumerator name. The unsaved-edits case additionally
surfaces an operator-facing prompt in the editor shell — the only path
in §8 that touches the user directly.

**Important non-refusal:** a game-plugin reload that scrubs entries
from `Selection` (§8.3.2) is **not** a refusal. The id-revalidation
behaviour is part of the contract; the editor surfaces the change as a
`SelectionRevalidated` event (§8.5), and the user observes the
selection set shrink. Refusing the swap because some selected entity
no longer exists would block legitimate dev workflows where iterating
on game logic regularly despawns and respawns entities.

### 8.5 Observers — `LayoutChanged`, `SelectionRevalidated`

The closed `EditorEvent` sum (§5.13) gains exactly two arms the
hot-reload contract introduces. They piggyback on `core`'s typed event
bus per §3.2 collapse #9 — tools never owns a second bus.

```
events::LayoutChanged          { LayoutProfileName active{}; std::uint32_t panels_re_registered{0}; };
events::SelectionRevalidated   { std::uint64_t pre_hash{0}; std::uint64_t post_hash{0}; std::uint32_t scrubbed_count{0}; };
```

Both are middleman-typed (`glibre.types.tools.HotReloadEvent` with
arms `LayoutChanged`, `SelectionRevalidated`) so their wire layouts
survive any tools-plugin reload. Their wire schemas live alongside
§7.1's persistent types and follow the same additive-variant rule
(§7.2.3); a fifth arm or new field requires a coordinated `EditorEvent`
codegen bump.

**Emission rules:**

1. **`LayoutChanged`** fires exactly once **per tools-plugin
   self-reload**, at the end of §8.3.1's resume sequence (after every
   panel is re-registered and the active layout has materialised),
   *before* the protocol's `HotReloadCompleted` event. It does **not**
   fire on game-plugin reload (no layout work happens). It also fires
   on user-driven `LayoutProfile` switches per §4.2 — the reload event
   is one source among others; subscribers cannot distinguish
   "user switched layout" from "tools just self-reloaded" except by
   observing whether `HotReloadCompleted` follows.
2. **`SelectionRevalidated`** fires exactly once **per game-plugin
   reload** that scrubs at least one id, at the end of §8.3.2's
   target-id refresh, before the protocol's `HotReloadCompleted`
   event. It does **not** fire on tools-plugin self-reload (selection
   bytes survive verbatim). `pre_hash` is the §4.3-inv-2 hash of
   `Selection` before re-resolution; `post_hash` is the hash after.
   `scrubbed_count == 0` cases are silenced (no event emitted) so
   subscribers can use the event's mere arrival as a signal that
   their cached selection-derived state is stale.

**Observer atomicity** is the protocol's §"Observer Notification"
guarantee: subscribers are called synchronously on the loader thread,
between protocol steps 4.2 and 4.3 of *the swapping plugin's*
transaction. Subscribers see a fully-swapped, fully-migrated world
when they receive these events; they never observe a half-swapped
state. This is the contract that lets the editor's diagnostic
overlay update without races and lets the e2e harness capture
deterministic post-reload assertions (§8.6).

The two new arms are the *only* tools-side observability additions
across the hot-reload boundary. New observability needs flow into
existing `EditorEvent` arms or graduate to a SPEC bump per §3.2
collapse #9.

### 8.6 Test hooks — trace recorder paused during reload window

Tools' reload contract is verified via the loader's existing
`enqueue_hot_reload` E2E entry point (protocol §"Test Hooks"). Tools
adds **one test-hook discipline** that no other plugin needs: the
trace recorder is paused for the duration of the reload window and
resumes capture at the next frame.

**Pause / resume rule.** When a hot-reload request is enqueued for any
plugin (including tools itself) and the editor is in `Recording` mode
(§4.9 inv. 1), the `TraceRecorder` writes one `TraceOp` of kind
`HotReloadBoundary { plugin_fqn, frame_counter }` and **stops capture
immediately**. Capture resumes at frame N+1's phase 1 input pump entry,
*after* the protocol's `HotReloadCompleted` (or `HotReloadRefused`)
event has been published. The pause covers the entire phase 8 window:
no `TraceOp` is recorded for events that originate inside drain, swap,
migrate, or resume; the only record of the window is the single
boundary op. (Rationale: the swap is not an editor action; recording
its internals would couple the trace format to the loader's internal
event vocabulary, violating §4.9 inv. 1's "non-perturbing" rule and
§4.10 inv. 5.)

If the reload is refused, the next captured `TraceOp` (frame N+1's
input op) is unaffected by the refusal — the recorder treats the
boundary op as a marker, not a transaction. If the reload succeeds, the
post-reload frame's first ops are captured against the new plugin
image; replay against a recording that crossed a reload window must
re-trigger the same reload at the same frame (`enqueue_hot_reload`
called in trace replay against the same `plugin_fqn` at the recorded
`frame_counter`) for the trace to remain deterministic. This is the
e2e-context contract surfaced here only as a tools-side requirement;
the replay machinery lives in `specs/e2e/SPEC.md`.

**Test scenarios.** The CI matrix (Catch2 cases listed in §11)
exercises three end-to-end fixtures running inside a single
deterministic frame loop using the in-process `enqueue_hot_reload`
trigger:

1. **Tools-plugin self-reload — happy path.** A fixture pair
   `tools-v1` → `tools-v1-rebuilt` (byte-identical except for the
   embedded build timestamp) runs the editor for `K = 16` editor
   frames, triggers a self-reload at frame 8 with no unsaved edits
   and no active recording, and asserts:
   - the active `LayoutProfile` is byte-equal pre / post;
   - every panel from frame 7 still resolves at frame 9;
   - `Selection`, `CommandStack`, `Gizmo` configuration, and
     `Shortcuts` are byte-equal pre / post;
   - `EditorEvent::LayoutChanged` fires exactly once at frame 8;
   - `SelectionRevalidated` does *not* fire;
   - `HotReloadCompleted` fires exactly once with
     `migrated_types = []`.
2. **Tools-plugin self-reload — refusal on unsaved edits.** Same
   fixture pair, but with the `CommandStack` carrying one undone
   `EditCommand` at frame 8. Asserts `HotReloadRefused { cause:
   tools::Error::Refused }` fires; the prior tools image keeps
   ticking through frame 16; `LayoutChanged` does *not* fire.
3. **Game-plugin reload — selection revalidation.** A fixture pair
   `game-v1` → `game-v2-despawn-half` (the v2 plugin's migrate
   function despawns every odd-indexed entity) runs the editor with
   `Selection` populated by 8 entities at frame 8. The reload
   triggers tools' game-plugin observer; the trace asserts:
   - `Selection` shrinks from 8 ids to 4 (the even ones);
   - `SelectionRevalidated` fires exactly once with
     `scrubbed_count == 4` and `pre_hash != post_hash`;
   - The Inspector panel's view cache is invalidated for every
     scrubbed entity; the panel re-draws cleanly at frame 9 against
     the survivors;
   - `LayoutChanged` does *not* fire (no layout work);
   - `HotReloadCompleted` fires exactly once with the v2 plugin's
     migrated types;
   - The `TraceRecorder`, if in `Recording` mode, captures one
     `HotReloadBoundary` op at frame 8 and resumes normal capture
     at frame 9.

All three scenarios run inside a single CI job using the in-process
trigger; no filesystem watcher is involved (protocol §"Test Hooks").
The Catch2 cases are listed in §11 acceptance criteria as
`Tools self-reload preserves user state`, `Tools self-reload refuses
unsaved edits`, `Game-plugin reload revalidates Selection`.

### 8.7 Cross-references

- Engine protocol: `reviews/decisions/hot-reload-protocol.md`
  (drain → swap → migrate → resume; refusal arms; observer bus;
  E2E hook). Tools adds nothing to that machinery.
- Frame slot: `reviews/decisions/frame-phases.md` (phase 8 entry /
  exit guarantees; one-frame pipeline preserved).
- Render pilot pattern: `specs/render/SPEC.md` §8 (parallel
  structure: reload point, survival inventory, migrate body,
  refusal cases, observers, test hooks).
- Persistence rules invoked: §7.1.1 / §7.2.1 (`LayoutProfile`
  additive), §7.1.2 / §7.2.2 (`Scene` additive), §7.1.3 / §7.2.3
  (`CommandJournal` closed-sum `kind` strictly additive — the
  property that makes old commands replayable across every future
  tools image), §7.1.4 / §7.2.4 (`Shortcuts` additive).
- Aggregates touched: §4.1 `EditorHost` (drain / resume orchestration),
  §4.2 `Layout` / `LayoutProfile` / `Panel` (re-apply on self-reload),
  §4.3 `SceneTree` / `Selection` (re-resolve on game-plugin reload),
  §4.4 `Inspector` / `InspectorView` (cache invalidation on both
  classes), §4.7 `EditCommand` / `CommandStack` (journal snapshot
  survival; closed-sum replayability), §4.9 `TraceRecorder`
  (paused during reload window), §4.10 inv. 9 (this section's
  body), §4.10 inv. 10 (frame-phase ownership preserved).
- Errors used: `tools::Error::Refused` (unsaved-edits refusal),
  `tools::Error::LayoutLoadFailed` (layout re-apply failure),
  `tools::Error::CommandConflict` (closed-sum `kind` violation in
  surviving journal), each wrapped by either
  `core::Error::HotReloadRefused` (drain-time refusal) or
  `core::Error::PluginInitFailed` (resume-time refusal) per protocol
  §"Refusal Cases".
- New `EditorEvent` arms: `LayoutChanged`, `SelectionRevalidated`
  (§8.5 — middleman-typed; §5.13 closed sum widened by these two arms
  only).

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
