# Decision Record — Performance Budget

## Status

Accepted (spike #10, refs sub-epic Epic #E0/SubE B, epic #2). Inputs to
every per-context SPEC §9 (perf budget) and to the CI gate that
enforces frame-time totals.

## Context

Glibre targets 60 fps on macOS 26 / Apple Silicon M1 baseline with a
Metal 4 mesh-shader gbuffer + hybrid-RT shadow pipeline (PHILOSOPHY,
ROADMAP, frame-phases.md). 60 fps == 16.67 ms wall-clock per
displayed frame on the game-loop driver thread. Frame-phases.md fixes
nine ordered phases per frame and notes that submission of frame N
overlaps simulation of frame N+1; the budget therefore distinguishes
**CPU sim work** (phases 1-5, on the critical path of the next
displayed frame) from **CPU submit work** (phases 6-7, overlapped
with the GPU executing the prior frame) from **GPU work** (phase 7
output, executed on the GPU concurrently with sim).

The plan sketch ("D. Performance Budget Frame") supplied an initial
allocation. This record locks the per-context cells, justifies each
cell against MVP scenarios (sample scene, hot-reload edit, asset
import), specifies the pipelined-frame timing model, and defines:

1. The CI gate that fails a PR whose perf changes break the frame target.
2. The allocator rules (per-context heap ceilings) the core memory
   subsystem enforces.

The 16.67 ms target must be met with **>= 1.5 ms headroom** (constraint
from the issue body) so that variability (thermal throttling, OS jitter,
spurious GC-style pauses inside Jolt or Metal drivers) does not push
the frame across the deadline.

## Decision

The per-context CPU/GPU/heap budget below is the engine-wide
contract for MVP. Every context's SPEC §9 quotes its row and may not
exceed it without a perf-budget amendment spike. The headroom row is
explicitly reserved unallocated capacity, not slack to be spent silently.

### Per-Context Budget Table

| Context     | CPU ms (sim)  | CPU ms (submit) | GPU ms | Heap ceiling | Phase ownership / participation                                 |
|-------------|---------------|-----------------|--------|--------------|-----------------------------------------------------------------|
| core        | 0.40          | 0.05            | -      | 64 MiB       | owns 5 (transform), 8 (hot-reload); systems in 1, 9             |
| platform    | 0.20          | 0.05            | -      | 16 MiB       | owns 1 (input), 9 (present)                                     |
| data        | 0.15          | 0.00            | -      | 32 MiB       | persistence spine; per-frame work is migration only             |
| shader      | 0.00          | 0.00            | -      | 32 MiB       | offline cook; 0 ms per frame in shipping build                  |
| render      | 0.10          | 1.40            | 8.0    | 512 MiB      | owns 6 (cull-extract), 7 (render-submit)                        |
| geometry    | 0.30          | 0.20            | -      | 256 MiB      | meshlet selection inside phase 6; BLAS refits inside phase 7    |
| physics     | 2.00          | 0.00            | -      | 128 MiB      | owns 3 (physics-fixed)                                          |
| content     | 0.20          | 0.00            | -      | 256 MiB      | residency / streaming; one-shot import work is off-thread       |
| tools       | 0.80          | 0.20            | 0.5    | 256 MiB      | gizmo + inspector systems in 1; ImGui draw inside phase 7       |
| e2e         | n/a           | n/a             | n/a    | n/a          | test-only context; budget irrelevant in shipping build          |
| **subtotal**| **4.15**      | **1.90**        | **8.5**| 1552 MiB     |                                                                 |
| **headroom**| **1.50**      | **0.50**        | **0.5**| -            | reserved unallocated; CI gate fails if eaten                    |
| **frame**   | **5.65**      | **2.40**        | **9.0**| -            | sim+submit on CPU = **8.05 ms**; GPU concurrent                 |

CPU sim + CPU submit = 8.05 ms wall-clock per frame. GPU runs
concurrently and finishes within 9.0 ms; presentation pacing in phase
9 absorbs sub-millisecond skew. Frame-budget invariant:
**`max(cpu_sim + cpu_submit, gpu) <= 16.67 - 1.5 = 15.17 ms`**, met
with substantial margin (8.05 ms CPU and 9.0 ms GPU); the 1.5 ms
headroom is real, not arithmetic-only.

## Justification Per Cell

Each cell is justified against the three MVP scenarios:
**(S1)** sample scene = 1 character + 200 props + 8 dynamic lights at
1920x1080; **(S2)** hot-reload edit = swap one plugin dylib at frame
boundary; **(S3)** asset import = drag-drop one FBX (~5 MiB) while
S1 plays.

- **core / 0.40 sim + 0.05 submit / 64 MiB.** Phase 5 transform
  propagation dominates: ~2k entities, dirty-set sweep is a linear
  walk over packed `LocalTransform` storage; on M1 firestorm at 3.2
  GHz, 2k SIMD mat4 multiplies + cache-coherent writes are
  comfortably <0.3 ms (S1). Phase 8 idle case (S1, S3) is <0.1 ms;
  S2 hot-reload migrates one plugin's component storage and is
  budgeted to 0.4 ms but excluded from steady-state (it occurs once
  per reload, not every frame). 0.05 ms submit-side covers world
  tick advance + frame-stat counter writes. 64 MiB heap holds the
  ECS archetype tables for MVP entity counts (estimated 8-16 KiB
  per archetype x ~256 archetypes max + handle tables).
- **platform / 0.20 sim + 0.05 submit / 16 MiB.** SDL3 event drain
  + action-event mapping in phase 1 is dozens of events/frame max
  (S1, S2, S3 all bounded by human input rate). Phase 9 present
  (drawable acquire + present + CAMetalDisplayLink callback
  trampoline) is <0.05 ms; the wait for next drawable does not count
  against CPU since it overlaps with GPU. 16 MiB covers SDL3 internal
  state + window-state singleton + small per-frame action ring buffer.
- **data / 0.15 sim / 32 MiB.** Per-frame work is migration-step
  drains and Fory-handle bookkeeping; steady-state under S1 is near
  zero. 0.15 ms reserved so a schema-migration that lands on a
  hot-reload frame (S2) does not blow out the budget. 32 MiB holds
  middleman type tables + the active migration scratch arena.
- **shader / 0.0 / 32 MiB.** Shading is fully offline-cooked
  (PHILOSOPHY #6, codegen-everywhere). Shipping builds load PSO
  blobs from disk during init; no per-frame compile. 32 MiB caches
  resident PSO blobs for MVP material set.
- **render / 0.10 sim + 1.40 submit / 8.0 GPU / 512 MiB.** Sim-side
  0.10 ms covers `RenderFrame` extract handoff + double-buffer pointer
  swap. Phase 7 record budget 1.40 ms: ~3k draws via mesh-shader
  indirect on M1 is ~0.5 us/draw on the encoder (Apple's published
  encoder cost) ⇒ ~1.5 ms; we cap at 1.40 ms by leaning on Metal 4
  argument-buffer reuse. GPU 8.0 ms covers the gbuffer + hybrid-RT
  shadow + lighting passes for S1; M1 8-core GPU at ~2.6 TFLOPs hits
  this with 30-40% of peak occupancy on the published Metal 4
  mesh-shader benchmarks (Apple WWDC 2024/25 figures applied to the
  M1 baseline, validated against 2025 Apple sample scenes). 512 MiB
  heap covers gbuffer (4 MRT @ 1080p ~64 MiB), shadow atlases, BVH
  scratch, transient resource pool, and PSO/argument-buffer cache.
- **geometry / 0.30 sim + 0.20 submit / 256 MiB.** Sim-side: per-frame
  meshlet visibility scoring + LOD selection inside phase 6 (~200
  props x ~16 meshlets each = 3.2k culls; SIMD bound, <0.3 ms).
  Submit-side: BLAS refits for the dynamic character + any rigid
  bodies that moved this frame (Metal 4 `accelerationStructure`
  refit is GPU-resident; CPU-side cost is descriptor packing). 256
  MiB covers vendor-cooked vertex/index/meshlet streams + BLAS/TLAS
  storage for the resident set.
- **physics / 2.00 sim / 128 MiB.** Jolt at 60 Hz fixed-step with S1
  (200 props, mostly sleeping; ~30 active rigid bodies + 1 character)
  is 0.5-1.2 ms per substep on M1 firestorm per the Jolt 2025
  benchmarks (Apple Silicon track). 2.00 ms reserves headroom for
  one sub-stepped frame (accumulator ran two ticks) without missing
  the deadline. 128 MiB holds Jolt broadphase + body manager + island
  graph + contact cache for the MVP ceiling of ~1k bodies.
- **content / 0.20 sim / 256 MiB.** Per-frame: residency tickle
  (handle reference-count sweep) + asset-handle resolution; all O(n
  changed) and bounded. Heavy work — FBX parse (S3), Draco decode,
  texture upload — runs on the import worker thread off the game
  loop; the 0.20 ms covers the **handoff** (CAS lookup, residency
  table update) when the worker signals completion. 256 MiB is the
  CPU-side residency cache; GPU-side memory is render's 512 MiB.
- **tools / 0.80 sim + 0.20 submit / 0.5 GPU / 256 MiB.** Editor
  shell systems run inside phase 1 (gizmo manipulation, inspector
  edits) before any sim consumer reads (frame-phases Open Q #3).
  ImGui-Metal-4 draw is recorded inside phase 7. 0.80 ms sim covers
  scene-tree refresh + inspector binding diff; 0.20 ms submit + 0.5
  ms GPU cover ImGui's draw call (typical complex editor frame is
  ~1k ImGui draws ⇒ <0.5 ms on M1). 256 MiB heap: editor textures,
  font atlases, undo-redo ring buffer.
- **e2e / n/a.** Tests substitute their own `InputDriver` and
  assertion ops; budget is whatever the test fixture allows. Not a
  shipping-build cost.
- **headroom / 1.50 sim + 0.50 submit + 0.5 GPU.** Reserved. Eating
  into headroom is a CI failure (see CI Gate Spec). Reasons it must
  exist: (a) thermal throttling on sustained M1 loads, (b)
  Metal/IOAccelerator driver-side jitter that is not under our
  control, (c) future deferred contexts (animation, audio, AI,
  networking) need a place to dock without immediately re-budgeting.

## Pipelined Frame Timing

Frame N's wall-clock from the perspective of the game-loop driver
thread:

```
phase  1 input         platform   0.10 ms  ┐
phase  2 logic         (deferred) 0.00 ms  │ CPU sim half
phase  3 physics-fixed physics    2.00 ms  │ (critical path of
phase  4 animation     (deferred) 0.00 ms  │  frame N+1's display)
phase  5 transform     core       0.30 ms  ┘  subtotal: 2.40 ms sim
                                            ── headroom feeds here

phase  6 cull-extract  render     0.10 ms  ┐
                       geometry   0.30 ms  │ CPU submit half
                       tools      0.80 ms  │ (overlapped with
phase  7 render-submit render     1.40 ms  │  GPU executing N-1)
                       geometry   0.20 ms  │  subtotal: 2.90 ms
                       tools      0.20 ms  ┘  (incl. tools sim slot)

phase  8 hot-reload    core       <0.10 ms steady-state; up to
                                  ~0.40 ms one-shot on a reload frame
phase  9 present       platform   0.05 ms  (drawable wait overlapped)

GPU concurrent: render 8.0 + tools 0.5 = 8.5 ms (frame N rendering
                while CPU does N+1 sim).
```

CPU wall-clock per frame on the driver thread, steady-state:
~2.40 + 2.90 + 0.05 = **5.35 ms** (well inside 16.67 ms; the
remaining slack absorbs the 1.5 ms headroom plus future deferred
contexts). GPU wall-clock: 8.5 ms. The pipeline is GPU-bound by
design at MVP, leaving CPU room for the deferred phases (animation,
logic, audio) when they land.

The hot-reload frame (S2) is allowed up to 0.40 ms in phase 8 (one
plugin storage migration) and that is the one frame per reload that
is permitted to skirt the headroom; CI gate has a separate threshold
for "reload frame" samples.

## CI Gate Spec

A new workflow `perf-budget.yml` (to be authored under the
`task-breakdown-error-perf` spike) runs on every PR touching code in
any of the ten MVP context directories and on `main` post-merge.

Gate logic (rejects PR on any failure):

1. **Per-context unit perf tests.** Every context's SPEC §9 lists at
   least one micro-benchmark (Catch2 `BENCHMARK` block) that exercises
   its phase work under the S1 fixture. The benchmark asserts
   `time <= cell_budget_ms`. PR fails if any assert fails.
2. **End-to-end frame timing.** A nightly e2e run of the sample scene
   (S1) for 600 frames captures per-frame CPU sim, CPU submit, and
   GPU times via `MTLCounterSampleBuffer` + frame-marker
   instrumentation. PR fails if:
   - p50 CPU (sim+submit) > 8.05 ms, or
   - p99 CPU (sim+submit) > 13.67 ms (= 16.67 - 3.0 jitter), or
   - p50 GPU > 9.0 ms, or
   - any frame's `cpu_sim+cpu_submit` exceeds the headroom-included
     ceiling 9.55 ms (= 8.05 + 1.5).
3. **Heap ceiling enforcement.** A diagnostic build runs the same e2e
   scene with the per-context allocator (Allocator Rules below) in
   strict mode. PR fails if any context's resident heap exceeds its
   cell ceiling. Migrations and import scratch are permitted to use a
   transient arena that does not count against the ceiling, provided
   the arena drains by phase 9 of the same frame.
4. **Hot-reload frame budget.** A scripted reload (S2) runs on the
   nightly job; the reload-frame phase 8 cost must be <= 0.40 ms. PR
   fails if exceeded.
5. **Headroom regression alarm.** If p50 CPU or GPU drops within 0.5
   ms of the ceiling for two consecutive nightlies, the gate posts a
   warning comment on the next PR and labels it `perf:headroom-low`.
   This does not block merge; it is a tripwire to open a perf-budget
   amendment spike before the budget is actually broken.

The fixtures (S1 sample scene, S2 reload script, S3 import asset)
live under `e2e/perf/` and are versioned alongside the gate. A
fixture change requires a perf-budget amendment spike.

## Allocator Rules

`core` exposes a `glibre::PerContextAllocator` (header in
`core/include/glibre/alloc.hpp`, implemented under `core/src/alloc/`).
Plugins request memory through this allocator only; raw `new`/`malloc`
in plugin code is rejected by the build via a `-Wglibre-no-raw-alloc`
warning-as-error compile flag (clang custom plugin, decided in the
implementation plan, not here).

1. **Per-context tag.** Every allocation carries a `ContextTag`
   (`core`, `platform`, `data`, `shader`, `render`, `geometry`,
   `physics`, `content`, `tools`). Tag is supplied by the caller via
   the allocator handle obtained at `glibre_plugin_register` time;
   the registration code stamps the tag into the handle so plugin
   call sites are tag-free.
2. **Hard ceiling in diagnostic / debug builds.** When
   `GLIBRE_ALLOC_STRICT=1`, the allocator tracks live bytes per
   `ContextTag` and returns
   `std::unexpected{core::Error::OutOfBudget}` (see error-model.md)
   when an allocation would push the tag over its cell ceiling.
   Plugin code that fails to handle the `Result<T>` aborts with a
   diagnostic dump.
3. **Soft warning in shipping builds.** Shipping builds do not return
   `OutOfBudget`; instead, exceeding the ceiling logs a `warn` once
   per-tag-per-frame to `spdlog` and increments a frame-stat counter
   that the editor surfaces in the perf HUD. This preserves
   release-build robustness while keeping the budget visible.
4. **Transient arena exemption.** Each context owns a per-frame
   transient arena (drained at phase 9) that does **not** count
   against the cell ceiling. Drain failure (allocations leaking past
   phase 9) is a `core::Error::OutOfBudget` arm with a "leak" detail
   string; CI gate has a debug-build assertion.
5. **GPU memory is render-owned.** All Metal heaps are allocated
   under the `render` tag regardless of which context requested the
   resource (geometry's vertex buffers, tools' ImGui textures). The
   render context's 512 MiB ceiling is GPU-side, accounted in MTLHeap
   residency. CPU-side staging uploads use the requesting context's
   tag for their CPU shadow only.
6. **Hot-reload migration arena.** Phase 8's drain → swap → migrate
   path uses a dedicated migration arena owned by `core`. Its
   ceiling (built into core's 64 MiB) is 16 MiB; exceeding it refuses
   the reload (`core::Error::SchemaMigrationFailed`) and keeps the
   prior plugin live (per error-model.md and frame-phases.md).

## Rationale

- **Per-context budget over per-phase budget.** Frame-phases.md
  notes the average per-phase budget is ~1.85 ms but two phases
  (physics, render-submit) are expected to dominate. A per-context
  budget admits this asymmetry honestly: physics gets 2.0 ms, render
  gets 8.5 ms total (1.4 CPU submit + 8.0 GPU + 0.1 sim), shader
  gets zero. Budgeting per phase would force artificial fairness
  that the workload does not have.
- **Sim/submit split.** Because frame N+1 simulation overlaps frame
  N GPU execution, a single "CPU ms" column would mislead. Splitting
  shows that the critical CPU path of the next displayed frame is
  only 2.40 ms steady-state — leaving substantial CPU room for
  deferred contexts (logic, animation, audio, AI, networking) which
  is the whole point of reserving phases 2 and 4 in frame-phases.
- **>= 1.5 ms headroom locked.** Constraint from the issue, also
  derived independently: M1 thermal throttling under sustained load
  drops effective frequency by ~10-15%, which on an 8 ms CPU budget
  is ~1.0-1.2 ms of jitter. 1.5 ms is the smallest safe margin that
  survives both thermal jitter and Metal-driver tail latency we have
  seen in macOS 15 dev-tools captures. We do not have direct macOS
  26 numbers; if M1 thermal headroom on macOS 26 differs materially,
  this record is the right amendment point.
- **Heap ceilings as enforced contracts, not advice.** The
  Allocator Rules section makes the heap budget a build-and-CI
  contract, not a SPEC convention. Without enforcement the per-cell
  numbers drift and we get the harmonius prior-art outcome (budgets
  in docs, allocations everywhere). Tagged allocation + strict mode
  in CI is the smallest enforcement that catches drift early.
- **Shader = 0 ms / frame.** Codegen-everywhere (PHILOSOPHY #6)
  means the shader context's runtime contribution is loading PSO
  blobs at init and zero per-frame. Recording it as 0 in the table
  keeps that decision honest; if any per-frame shader work appears,
  it is a SPEC violation and a perf-budget amendment.
- **e2e excluded.** Test scaffolding's perf budget is whatever the
  fixture's deadline is; encoding a number here would create a
  fictional ceiling for shipping builds where e2e is not present.

## Consequences

- Every per-context SPEC §9 must quote its row's CPU-sim, CPU-submit,
  GPU, and heap numbers verbatim, plus list its phase ownership
  consistent with frame-phases.md. SPEC §9 then refines with a
  per-system breakdown that sums into the cell.
- The `task-breakdown-error-perf` spike must produce `type:plan`
  issues for: (a) `glibre::PerContextAllocator` implementation,
  (b) `perf-budget.yml` CI workflow, (c) the S1/S2/S3 fixtures,
  (d) the strict-mode build flag and the Catch2 `BENCHMARK`
  scaffolding for SPEC §9 micro-benchmarks.
- The error-model variant must include `core::Error::OutOfBudget`;
  it already does (error-model.md Type Sketch line for `core::Error`).
  No amendment to error-model needed.
- Adding a deferred context (animation, audio, AI, networking) to
  shipping consumes headroom. That context's introduction spike
  must amend this record by allocating it a row out of headroom,
  not by squeezing existing contexts.
- The 8.05 ms CPU steady-state means MVP has substantial room to
  absorb the deferred contexts: animation ~1.0 ms, audio ~0.3 ms,
  scripting/logic ~0.5 ms, AI ~0.5 ms, networking ~0.3 ms = 2.6 ms
  combined still leaves 5.5 ms wall-clock under 16.67 ms.

## Open Questions

1. Are macOS 26 / M1 thermal-throttling numbers materially different
   from the macOS 15 baseline measurements that informed the 1.5 ms
   headroom? Resolution: a one-day measurement spike opened against
   #E0 once the platform context lands a runnable harness.
2. Does the `tools` context need a separate ceiling for "play-mode"
   vs "edit-mode" frames? Edit-mode tolerates larger latencies and
   could permit a wider gizmo budget. Provisional answer: keep one
   row, treat edit-mode as out-of-scope for the gate; revisit when
   tools spec lands.
3. Is the GPU-memory-is-render-owned rule sustainable when content's
   streaming residency wants to track GPU-resident-vs-CPU-resident
   per asset? Likely yes (render exposes a tagged sub-allocator
   keyed on asset handle), but defer to the content/render seam
   spike.
4. Should the Catch2 `BENCHMARK` cell-budget assertion run on every
   PR or only nightly? Per-PR is most protective but adds CI
   minutes; defer the cadence decision to the workflow-authoring plan.
5. The `OutOfBudget` arm is currently `core::Error`. Should it carry
   a `ContextTag` payload so consumers can see which context busted
   the budget? Likely yes, but the payload mechanism interacts with
   `glibre::Error`'s variant design (error-model.md). Resolve when
   the alloc plan lands.
