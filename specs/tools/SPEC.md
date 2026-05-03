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

Harmonius requirement IDs / file paths cited as research input. Note any
collapse decisions (multiple harmonius concepts → one glibre primitive).

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
