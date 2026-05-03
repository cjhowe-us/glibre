# Decision Record: Frame Phases

## Status

Accepted (spike #6, refs sub-epic #3, epic #2).

## Context

The MVP engine targets 60 fps on macOS, single-player, with a mesh-shader
gbuffer + hybrid-RT shadow pipeline and plugin-only domain growth. Every
non-core domain ships as a `.dylib` and may be hot-reloaded; reload must
occur at a frame boundary, never mid-frame (PHILOSOPHY §8). The frame
schedule must:

1. Cover MVP needs (input, fixed-step physics, transform propagation,
   render submission, present) end-to-end.
2. Leave **named, reserved slots** for deferred contexts
   (animation, audio, AI, networking) so the ordering does not break
   when post-MVP plugins land. The slots are present in the schedule
   from day one but their bodies are no-ops in MVP.
3. Place the hot-reload barrier at exactly one position per frame, with
   a clear "what is in flight when we reload" answer.
4. Permit one-frame pipelining between simulation and GPU submission
   without sacrificing determinism of the simulation half.

The plan sketch (`new-no-code-game-engine-golden-thompson.md` §A) listed
9 phases with the hot-reload barrier between submit and present.
Harmonius prior art used 8 phases and placed reload concerns outside the
loop entirely; that is rejected because PHILOSOPHY §8 makes hot-reload a
first-class frame-boundary step. This record locks the 9-phase ordering,
assigns each phase to exactly one owning context (SRP), and specifies
read/write authority and exit guarantees so per-context SPEC §6 entries
can be authored without re-litigating ordering.

## Decision

Nine fixed phases per frame, executed in strict numeric order on the
game-loop driver thread. Phase 8 is the hot-reload barrier and is the
**only** point in the frame at which plugin `.dylib`s may be swapped.
Phase 9 owns presentation; the GPU submission of frame N overlaps the
simulation of frame N+1 (one-frame pipeline), but every phase boundary
within a single frame is a hard barrier — no phase may observe writes
from a later phase of the same frame.

Each phase has exactly one owning context. Other plugins may register
**systems** that run inside a phase, but the phase itself — its entry
guarantees, its exit guarantees, and the body's scheduling — is owned
solely by that context. This preserves SRP: a phase has one reason to
change, located in one bounded context.

Deferred contexts (animation, audio, AI, networking) own reserved
phases or sub-slots from day one. In MVP their phase bodies are
empty — the slot exists, the systems list is empty, the exit guarantee
trivially holds. Post-MVP plugins fill the slot without disturbing
neighbours.

## Phase Table

| # | Name             | Owning context | What runs                                                                                              | Allowed reads                                                                  | Allowed writes                                                                          | Exit guarantee                                                                                               |
|---|------------------|----------------|--------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------|
| 1 | input            | `platform`     | Drain SDL3 SPSC event queue; map raw input to action events; pump window/lifecycle events.             | OS event queue; previous frame `Input` components.                             | `Input` / `ActionEvent` components; window-state singleton.                              | All input for tick T is materialised in ECS; no further raw events appended this frame.                      |
| 2 | logic            | (deferred — gameplay/scripting plugin; reserved slot, MVP body empty) | Visual-script / game-framework systems; reads input, schedules intents.                                | `Input`, world state (read-mostly), gameplay components.                       | Gameplay intent components, transient script state.                                      | Intents for this tick are written; world state past this point is read-only for sim consumers in phase 3.    |
| 3 | physics-fixed    | `physics`      | Jolt fixed-timestep substeps with deterministic config; broadphase, solve, contacts.                   | Transforms, rigid-body components, collision shapes, intents from phase 2.     | Rigid-body velocities/positions, contact events.                                         | Physics tick(s) for this frame complete; accumulator advanced; deterministic byte-equal world snapshot.      |
| 4 | animation        | (deferred — animation plugin; reserved slot, MVP body empty)         | Skeletal pose evaluation, IK, blendshapes; in MVP nothing runs.                                        | Physics-resolved transforms, animation graphs, skeletal data.                  | Pose components, derived bone matrices.                                                  | All animation outputs that feed transform propagation are written; bodies untouched after this point.        |
| 5 | transform        | `core`         | Hierarchical `LocalTransform → GlobalTransform` propagation; previous-transform shadowing for interp.  | Local transforms, parent pointers, dirty flags.                                | `GlobalTransform`, `PreviousGlobalTransform`, dirty-set reset.                           | Every entity's `GlobalTransform` reflects this frame's sim+anim writes; hierarchy is consistent.             |
| 6 | cull-extract     | `render`       | Frustum + occlusion cull; meshlet selection; build immutable `RenderFrame` extract (no GPU calls yet). | `GlobalTransform`, `PreviousGlobalTransform`, mesh/material/light components.  | An owned `RenderFrame` snapshot (visible-set, draw cmds, lights, camera, interp alpha). | A finalised `RenderFrame` exists; no further ECS reads are required by the render half this frame.           |
| 7 | render-submit    | `render`       | Record Metal command buffers from `RenderFrame`; mesh-shader gbuffer + hybrid-RT shadow + lighting.    | `RenderFrame` (immutable), GPU resource handles, shader middleman types.       | Metal command buffers, transient GPU resources; signals submit-fence.                    | Command buffer for frame N is enqueued to the GPU; sim-side reads of `RenderFrame` are done.                 |
| 8 | **hot-reload**   | `core`         | Drain → swap → migrate → resume. ABI hash gated. Idle if no reload requested.                          | `PluginRegistry`, pending reload requests, on-disk `.dylib` headers.           | Plugin vtables, type registry, migrated component storages.                              | All loaded plugins satisfy ABI hash; component storages migrated; refusal cases logged and old plugin kept.  |
| 9 | present          | `platform`     | Acquire next drawable, present prior submit, run frame pacing (CAMetalDisplayLink), advance world tick.| Submit-fence, swapchain state, frame-stat counters.                            | Swapchain front buffer; world `ChangeTick` increment; frame counter.                     | Frame N is on screen; tick incremented; phase 1 of frame N+1 may begin.                                      |

Notes on the ordering choice:

- **Hot-reload at slot 8, after submit and before present.** Submit
  records command buffers but does not block on GPU completion;
  presentation happens after reload. This means a reload that swaps the
  render plugin still affects only frame N+1 onward — frame N's command
  buffer was recorded by the pre-reload code and is presented as-is.
  No half-recorded command buffer survives a swap.
- **`logic` (phase 2) and `animation` (phase 4) are reserved slots.**
  Their owning contexts are deferred (post-MVP). The phase exists in the
  schedule with an empty body; no neighbour is rewritten when the
  context lands.
- **Audio, AI, networking** are not separate top-level phases in MVP.
  They will register as systems inside existing phases when their
  contexts land: networking-receive and AI inside phase 2 (logic),
  networking-send and audio-mix inside phase 6 (cull-extract) so their
  outputs ride the same `RenderFrame`-equivalent snapshot. If a future
  spike shows that any of them needs hard isolation, it gets a
  `Custom(N)` slot allocated between numbered phases — never inserted
  ahead of the existing nine.
- **`core` owns phases 5 and 8 only.** Render owns 6 and 7 because
  cull-extract and render-submit share the `RenderFrame` data structure
  and there is no second consumer; SRP says one context, not two.

## Rationale

- **SRP per phase.** One owning context per phase is the cleanest seam:
  the SPEC §6 ("frame integration") block of each context lists
  exactly the phase(s) it owns and the systems it registers into other
  phases. Cross-context arguments about "who runs first" become
  arguments about ordering within a phase, owned by that phase's
  context — not a meta-debate.
- **Slot for deferred contexts.** Reserving phases 2 and 4 from day
  one removes the only realistic reason to renumber phases later. The
  alternative — adding phases when the plugin lands — would force
  every existing phase ID to shift, breaking persisted profiler traces
  and replay tooling.
- **Hot-reload after submit.** Placing the barrier between submit and
  present makes reload semantics trivially stateable: "the currently
  presented frame was rendered by the previous code; the next frame
  will be rendered by the new code." Placing it earlier (e.g. between
  cull and submit) would require reasoning about partially-built
  command buffers; placing it later (after present) would block the
  next frame's input from beginning while the swap completes.
- **One-frame pipeline preserved.** Phase 7 enqueues to the GPU and
  returns; phase 9 presents prior work. Sim of frame N+1 may begin
  while GPU executes frame N. Determinism is preserved because the
  simulation half (phases 1–5) sees no GPU side effects.
- **Occam's razor.** Nine phases is the minimum that names every
  MVP-or-near-MVP responsibility once. Audio/AI/network do not get
  their own slots because two collapsing requirements
  (network-tx and snapshot-build) become one primitive
  (`RenderFrame` extract on a generic snapshot bus) when revisited.

## Consequences

- Every per-context SPEC §6 must declare: which phase(s) it owns, which
  phases it registers systems into, and what its system access set is
  (read/write components, world singletons). The phase table above is
  the authority on the read/write columns.
- Profiler traces, replay records, and e2e golden-snapshot fixtures
  may persist phase IDs as `u8` 1..=9 with no risk of renumbering.
- The plugin loader's reload protocol (decision record
  `hot-reload.md`, to be authored under #7) only needs to consider
  state at one frame position — phase 8 — which simplifies the
  drain/swap/migrate/resume state machine.
- A context that wants to do work outside any of these nine phases
  must request a `Custom(N)` insertion via a future spike; this is
  intentionally friction-laden.
- Performance budget allocation (plan §D) is per-context, but the
  per-phase CPU budget is bounded by 16.67 ms / 9 ≈ 1.85 ms average.
  Phases 3 (physics) and 7 (render-submit) are expected to dominate;
  phase 8 must complete in <0.1 ms when no reload is pending.

## Open Questions

1. Phase 8 (hot-reload) when no reload is pending — should it still
   incur a barrier (cache flush, fence) or be a true no-op?
   Resolution deferred to spike #7 (research-hot-reload).
2. Should phase 3 (physics-fixed) run more than once per visible frame
   when the accumulator carries multiple ticks, or should the schedule
   re-enter phases 2–5 as a sub-graph? The MVP physics rate equals the
   render rate (60 Hz), so this question is dormant; revisit when a
   variable-rate game mode is proposed.
3. Where does editor-tools work (gizmo manipulation, inspector edits)
   slot in? Provisional answer: tools writes happen in phase 1
   (input-equivalent) before any sim consumer reads. Confirmed by a
   future tools spike.
4. When the audio plugin lands, does `RenderFrame` generalise to a
   `FrameSnapshot` carrying both visual and audio extracts, or do the
   two snapshots travel on independent triple buffers? Defer to the
   audio context spike.
5. The hot-reload barrier currently runs on the game-loop thread. If
   migration becomes expensive (large component storages), should it
   move to a worker with a fence into phase 9? Deferred to spike #7.
