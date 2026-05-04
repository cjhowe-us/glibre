# core — Detailed Design: schedule + frame-loop aggregate

> Detailed design for the `Schedule` / `Phase` / `FrameLoop` aggregate
> declared in `specs/core/SPEC.md` §4.4. Refines §4.4, §5.6, §5.7, §6.4,
> §6.5, §9.3, §10.1 in place; cites
> `reviews/decisions/frame-phases.md`,
> `reviews/decisions/error-model.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/plugin-abi.md`, and
> `reviews/decisions/fory-codegen.md`. Does not introduce new public
> surface beyond the §5 stub; deviations from the cited records would
> require an amendment spike, not an in-place edit.

## 1. Purpose

The schedule + frame-loop aggregate is the engine's deterministic
ordering machinery. Per `core` SPEC §4.4 it owns:

1. The **`Phase` enum** — the closed numeric ordering 1..=9 that every
   plugin's systems target. The phase table itself is decided in
   `frame-phases.md`; this aggregate hosts the table but does not
   relitigate it.
2. The **`Schedule`** — a per-phase access-set DAG built from every
   loaded plugin's `SystemDecl` (manifest field; plugin-abi.md §"Plugin
   Manifest Schema"). Compiled once per `(systems × types)` set into
   a per-phase `CompiledPhase`; reused across frames until plugin
   load/unload invalidates it.
3. The **`FrameLoop`** — the per-`World` driver that walks the nine
   phases in strict numeric order, dispatches each phase's
   `CompiledPhase`, drives the fixed-step accumulator that feeds
   physics-fixed (phase 3), and increments the world `ChangeTick` at
   the close of phase 9 (present).
4. The **timing-budget invariants** — each phase's run-time obeys the
   per-context cells in `perf-budget.md`; the schedule is the place
   that enforces this in CI via `BENCHMARK_CELL` blocks (§9.5 of
   `core/SPEC.md`).

This aggregate **refuses to own**:

- The bodies of phases owned by other contexts (phase 1 platform, 3
  physics, 6/7 render). It hosts the slot, validates the systems
  registered into it, and dispatches — it does not contain their
  logic.
- The hot-reload state machine inside phase 8. That belongs to
  `HotReloadBarrier` (§4.6 of `core/SPEC.md`,
  `reviews/decisions/hot-reload-protocol.md`); the FrameLoop only
  invokes `barrier.step(...)` between phases 7 and 9.
- The render-graph DAG. Render's `RenderFrame` extract sits inside
  phases 6 and 7 as a `PassDecl` registered through the manifest;
  its execution semantics belong to `render`.
- Conditional / exclusive-world systems. Per §3.2 collapse #2 of
  `core/SPEC.md` MVP rejects run-criteria; the schedule is a pure
  access-set DAG.
- Custom user-defined phases. `Phase` is a closed enum. Adding a
  phase is a `frame-phases.md` amendment spike, not a runtime API.

## 2. Requirements coverage

This section verifies that every harmonius MVP-scope requirement /
design clause about the schedule + frame-loop is either covered by
the design below or explicitly refused with rationale. Inputs:

- `harmonius/docs/requirements/core-runtime/game-loop.md` (R-1.11.1
  through R-1.11.10).
- `harmonius/docs/design/core-runtime/game-loop.md` (Phase ordering,
  thread roles, `CompiledFrame`, `RenderFrame`, `FixedTimestep`,
  `SpscQueue`, `TripleBuffer`, `GameStateManager`, `GameModeManager`).

| Harmonius clause                                                                 | Glibre disposition                                                                                                                                                                           |
|----------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-1.11.1** — 8-phase pipeline (Input, Network Rx, Sim, AI, Physics, Animation, FrameSnapshot, FrameEnd) with deterministic ordering | **Covered + collapsed.** Re-derived as the 9-phase ordering in `frame-phases.md` (Input, Logic, PhysicsFixed, Animation, Transform, CullExtract, RenderSubmit, HotReload, Present). Reasons: hot-reload promoted from "outside the loop" to phase 8 (PHILOSOPHY §8); transform propagation explicit at phase 5; sim-snapshot split into CullExtract / RenderSubmit since one consumer (`RenderFrame`). Network Rx not a slot in MVP; rides phase 2 (logic) when networking lands. AI similarly rides phase 2. Hard-coded numeric `Phase` enum; no data-driven phase identity (one reason to change per SRP). |
| **R-1.11.2** — Three thread roles (main / workers / render)                      | **Refused for MVP, deferred.** MVP is single-threaded on the game-loop driver; `core/SPEC.md` §6.10 commits to per-system parallelism as a pure-additive change keyed off the existing `(reads, writes)` collection (per-system parallelism plan #496 deferred). Rationale: PHILOSOPHY §7 (determinism by default) is trivially satisfied with a single thread; the schedule data is already structured to admit fork-join when the parallelism plan opens. |
| **R-1.11.3** — Immutable `RenderFrame` snapshot consumed by render thread        | **Covered, ownership delegated.** The snapshot lives in render context (phases 6+7 owned by `render`); `core` owns the seam — phase 6 produces the immutable extract, phase 7 records command buffers from it. This design specifies the per-phase invariant ("phase 6 publishes a finalised RenderFrame; phase 7's reads are forbidden against ECS"). The snapshot's *data shape* belongs to the render SPEC. |
| **R-1.11.4** — Lock-free triple buffer for game-loop → render thread             | **Refused for MVP.** Single-threaded loop has no producer/consumer split. The frame-phases.md "one-frame pipeline preserved" rule (Phase 7 enqueues to GPU, Phase 9 presents prior work) gives the same overlap without needing a triple buffer; the GPU runs frame N concurrently with CPU sim of frame N+1. When per-system parallelism lands and a separate render thread re-emerges, a triple buffer between phase 6 (extract) and phase 7 (submit) is the natural seam — listed in §12 (open questions). |
| **R-1.11.5** — Lock-free SPSC queue for OS events to game-loop                   | **Owned by `platform`, not `core`.** Phase 1 (input) is platform's slot; the platform plugin maintains the SPSC queue and drains it inside its phase-1 system. `core`'s contract is that phase-1 exit publishes `Input`/`ActionEvent` components into the ECS; the queue itself is platform-private. |
| **R-1.11.6** — Game state machine with `request_transition()` at sync points + mode graph | **Refused for MVP, deferred.** State / mode are gameplay concerns; phase 2 (logic) is reserved for them with an empty body in MVP. When the gameplay/scripting plugin lands, transitions become a system inside phase 2 reading a "pending transition" component and writing it at phase boundary. No `core`-owned state-machine API in MVP; the §5 stub does not expose one. |
| **R-1.11.7** — Pipelined rendering one frame behind the game loop                | **Covered.** `frame-phases.md` "Phase Table" line for phase 7 + §"Notes on the ordering choice": phase 7 enqueues command buffers and returns; the GPU executes frame N's commands concurrently with the CPU's sim of frame N+1; phase 9 of frame N presents the work submitted in phase 7. The pipeline is implicit in phase ordering, not an explicit data structure. |
| **R-1.11.8** — Compile schedule into `CompiledFrame`, recompile on plugin load/unload | **Covered.** Per-phase `CompiledPhase` (this design's name; SPEC §6.4 uses "compiled order array"). Compiles once per `(systems × types)` set; recompiles only on hot-reload barrier success or initial plugin load. Per-frame execution is `O(systems)` walk of a precomputed flat array. Phase identity itself is not data-driven; only the systems inside each phase are (see §3 below). |
| **R-1.11.9** — User-defined custom phases                                        | **Refused.** `Phase` is a closed enum (`core/SPEC.md` §5.3). PHILOSOPHY §10 (Occam's razor): the nine slots cover every MVP responsibility once; a tenth requires a `frame-phases.md` amendment spike. Reserved phase 2 (logic) and phase 4 (animation) absorb every plausible MVP-near user-defined need. |
| **R-1.11.10** — Platform-specific frame pacing (VSync / CAMetalDisplayLink / VR reprojection) | **Covered, ownership delegated.** Phase 9 (present) is platform-owned; pacing is platform's job. `core`'s `FrameLoop::tick(delta_seconds)` is driven from the platform plugin's pacing source (`CAMetalDisplayLink` callback on macOS); `core` provides the accumulator + phase walker, not the pacing source. |
| Harmonius design — `FixedTimestep` accumulator (`accumulate`/`consume`/`alpha`)  | **Covered.** The fixed-step accumulator lives inside `FrameLoop` (§4 of this design). `tick(dt)` accumulates; phase 3 consumes one tick (or more, capped by `max_ticks_per_frame` to prevent the spiral of death); `alpha()` is exposed to render context for interpolation between sim ticks (consumed in phase 6). |
| Harmonius design — `PhaseBody` enum (`Systems` / `RenderGraph` / `Task` / `SubGraph` / `Barrier`) | **Refused / collapsed.** Glibre's phases are not polymorphic. Each phase is one of two shapes: (a) "systems DAG" for phases 1–7 and 9 — handled by `CompiledPhase`; (b) "barrier step" for phase 8 — handled by `HotReloadBarrier::step` invoked directly from the FrameLoop body (`core/SPEC.md` §6.5). No `SubGraph` (physics substeps live inside phase 3, not as a nested graph); no `Task` (one-off jobs are systems with empty access sets). |
| Harmonius design — `CompileError { CyclicDependency, AccessConflict, MissingPhase, PhaseConflict }` | **Covered.** Maps to `core::Error::SystemScheduleCycle`, `core::Error::ScheduleAccessConflict`, `core::Error::FramePhaseMisordered` (debug-build only), no `PhaseConflict` analogue (each phase has one owning context per `frame-phases.md`; this is enforced at manifest-validation time, not at schedule-compile time, since plugin-abi.md §"Plugin Manifest Schema" already records `phase: u8`). |
| Harmonius design — `GameLoopGraph::add_dependency(before, after)` between phases  | **Refused.** Inter-phase ordering is fixed by the numeric `Phase` enum; no runtime API to reorder. Intra-phase ordering is via `SystemDecl.after` / `before` (manifest-declared, plugin-abi.md). One reason to change per SRP. |
| Harmonius design — `RenderFrame` field set                                       | **Owned elsewhere.** Not `core`'s data shape. |

Net result: every R-1.11.* requirement is either implemented as
designed below or explicitly refused with rationale; the harmonius
8-phase pipeline collapses cleanly into the glibre 9-phase one;
threading and snapshots are deferred without blocking determinism.

## 3. Detailed model

### 3.1 The nine phases (authoritative table from `frame-phases.md`)

| # | Name           | Owner    | Body shape           | Per-phase invariant (entry → exit)                                                                                                                                  |
|---|----------------|----------|----------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 1 | `Input`        | platform | systems DAG          | All input for tick T is materialised in ECS; no further raw events appended this frame. Reads OS event queue; writes `Input`/`ActionEvent` components.              |
| 2 | `Logic`        | gameplay (deferred — empty body in MVP) | systems DAG | Intents for this tick are written; world state past this point is read-only for sim consumers in phase 3.                              |
| 3 | `PhysicsFixed` | physics  | systems DAG (substepped via accumulator) | Physics tick(s) for this frame complete; accumulator advanced; deterministic byte-equal world snapshot.                                |
| 4 | `Animation`    | animation (deferred — empty body in MVP) | systems DAG | All animation outputs that feed transform propagation are written; bodies untouched after this point.                                  |
| 5 | `Transform`    | core     | systems DAG          | Every entity's `GlobalTransform` reflects this frame's sim+anim writes; hierarchy is consistent (no cycle); `PreviousGlobalTransform` shadowed for interpolation.   |
| 6 | `CullExtract`  | render   | systems DAG          | A finalised `RenderFrame` snapshot exists; no further ECS reads are required by the render half this frame.                                                          |
| 7 | `RenderSubmit` | render   | systems DAG          | Command buffer for frame N is enqueued to the GPU; sim-side reads of `RenderFrame` are done.                                                                          |
| 8 | `HotReload`    | core     | **barrier step** (not a systems DAG) | All loaded plugins satisfy ABI hash; component storages migrated; refusal cases logged and old plugin kept. Body = `HotReloadBarrier::step` (`core/SPEC.md` §6.7). |
| 9 | `Present`      | platform | systems DAG          | Frame N is on screen; world `ChangeTick` incremented; phase 1 of frame N+1 may begin.                                                                                |

Numeric ordering is **load-bearing**: phase N completes before phase
N+1 begins; no phase observes writes from a later phase of the same
frame. Violation is a build-time refusal (manifest validation:
`SystemDecl.phase ∉ 1..9` is rejected at load) and a debug-build
runtime assertion `core::Error::FramePhaseMisordered`
(`core/SPEC.md` §4.4 invariant 1).

Phases 2 and 4 are **reserved slots**: in MVP their `CompiledPhase`
is empty, so the FrameLoop's per-phase dispatch is a no-op; the slot
exists in the schedule from day one so neighbouring phases are not
renumbered when the deferred contexts ship.

### 3.2 Schedule compilation (per phase, runs nine times)

Inputs:

- The union of every loaded plugin's `SystemDecl` from
  `manifest.systems` (plugin-abi.md §"Plugin Manifest Schema"). Each
  carries `(name, phase, reads, writes, after, before)`.
- The `TypeRegistry` (`core/SPEC.md` §4.9) for resolving every entry
  in `reads` / `writes` to a stable `TypeId`.

For each phase number `p ∈ 1..=9`:

1. **Filter**: collect every `SystemDecl` with `decl.phase == p`. Skip
   the phase if the set is empty (deferred phases 2/4 in MVP, plus
   any user setup with no phase-N work).
2. **Build directed graph `G`** on system names. Add edge `A → B`
   ("A runs before B") when:
   - `A.writes ∩ B.reads ≠ ∅` (read-after-write hazard), OR
   - `A.writes ∩ B.writes ≠ ∅` (write-after-write hazard; tiebreak by
     deterministic name order; see step 4), OR
   - `A.name ∈ B.after` (manifest-declared explicit ordering), OR
   - `B.name ∈ A.before` (manifest-declared explicit ordering).
3. **Cycle detection**: topologically sort `G` (Kahn's algorithm).
   A cycle yields `core::Error::SystemScheduleCycle`; the loader
   rolls back the offending plugin's registration per
   `plugin-abi.md` §"Loader Sequence" step 10 (and per
   `core/SPEC.md` §4.4 invariant). Within a hot-reload transaction
   (§5 below), the rollback fires `HotReloadRefusedEvent` with the
   wrapping `core::Error::HotReload` umbrella per `core/SPEC.md`
   §10.1's row.
4. **Deterministic tiebreak**: when two systems have no edge between
   them (orderings either way are conflict-free), the topological
   sort breaks ties by lexicographic order of fully-qualified name
   (`core/SPEC.md` §4.4 invariant 5; PHILOSOPHY §7's fixed iteration
   order). This makes the compiled schedule byte-stable across
   hosts and runs given the same plugin set.
5. **Access-conflict refusal**: if at any point in the topological
   walk two systems `A`, `B` are eligible to run concurrently
   (no edge in either direction) **and** their access sets conflict
   (`A.writes ∩ B.writes ≠ ∅` or `A.writes ∩ B.reads ≠ ∅`), the
   schedule refuses compilation with
   `core::Error::ScheduleAccessConflict`. In MVP single-thread mode
   this is a vacuous check (one system runs at a time, every edge
   is realised by the deterministic order); the check is retained
   so the manifest validates against the post-MVP per-system
   parallel dispatcher without re-spec'ing the schedule. (Plan #496
   converts the check into a parallel-eligible-set partitioning.)
6. **Emit `CompiledPhase`**: a flat `eastl::vector<SystemThunk>`
   in topological order. Each `SystemThunk` is a codegen-emitted
   trampoline that resolves component column pointers from
   `TypeId`s once at compile time and calls the plugin's `SystemFn`
   (`core/SPEC.md` §5.4) with a stack-built `SystemContext`.

The build runs **at process startup** (after every plugin's
`glibre_plugin_register` returns) and **on every successful
hot-reload transaction** (`core/SPEC.md` §6.5; phase 8 success).
Build complexity is `O(systems²)` worst-case for the access-set
intersection sweep; the system count is bounded by plugin count ×
tens, and the build runs only at hot-reload (not per frame). The
fall-back to per-`(reads,writes)`-set bucketing (`O(systems)`
amortized) is gated on spike #493 (`core/SPEC.md` §6.11; §12 of
this design).

### 3.3 The frame loop body

`FrameLoop::tick(double delta_seconds)` drives one displayed frame.
Pseudo-code (faithful to `core/SPEC.md` §6.5; this design adds the
per-step contracts):

```cpp
glibre::Result<void> FrameLoop::tick(double dt) noexcept {
    accumulator_ += dt;

    // Phase 1: input
    run_phase_(Phase::Input,         /*allow_substeps=*/false);

    // Phase 2: logic (empty body in MVP)
    run_phase_(Phase::Logic,         /*allow_substeps=*/false);

    // Phase 3: physics-fixed — driven from the accumulator
    std::uint32_t ticks = consume_fixed_steps_();
    for (std::uint32_t i = 0; i < ticks; ++i) {
        run_phase_(Phase::PhysicsFixed, /*allow_substeps=*/true);
    }
    interp_alpha_ = static_cast<float>(accumulator_ / tick_seconds_);

    // Phase 4: animation (empty body in MVP)
    run_phase_(Phase::Animation,     /*allow_substeps=*/false);

    // Phase 5: transform (core-owned propagation)
    run_phase_(Phase::Transform,     /*allow_substeps=*/false);

    // Phase 6: cull-extract (render-owned; produces RenderFrame)
    run_phase_(Phase::CullExtract,   /*allow_substeps=*/false);

    // Phase 7: render-submit (render-owned; enqueues to GPU)
    run_phase_(Phase::RenderSubmit,  /*allow_substeps=*/false);

    // Phase 8: hot-reload (barrier step; not a systems DAG)
    if (auto r = barrier_->step(); !r) {
        // log + continue; refusal leaves prior plugin live.
    }

    // Phase 9: present (platform-owned; advances the world tick)
    run_phase_(Phase::Present,       /*allow_substeps=*/false);
    world_->advance_change_tick();
    ++frame_index_;

    return {};
}
```

`run_phase_(p, allow_substeps)` does:

1. Fire `phase_hooks_[p].on_enter` if registered (read-only hook;
   never mutates world per `core/SPEC.md` §5.7).
2. Walk `compiled_[p]` in order; for each `SystemThunk` build the
   per-system `SystemContext` (binds the world, the system's
   `CommandBuffer`, the current `ChangeTick`, the phase) and invoke
   the body. After the body returns, flush the system's
   `CommandBuffer` if non-empty (per `core/SPEC.md` §4.8 #3 — flush
   at the next phase boundary; the per-system flush is the safe
   conservative point, though MVP could batch all flushes at phase
   exit. See §3.5 below for the exact rule.).
3. Fire `phase_hooks_[p].on_exit`.

The `allow_substeps` flag is informational; it documents that phase
3 is the only phase invoked more than once per displayed frame.
Plan-level systems may inspect `SystemContext::is_substep()` (a
post-MVP addition; not in §5 stub) when needed.

### 3.4 Fixed-step accumulator (drives phase 3)

The accumulator is owned by `FrameLoop`; phase 3's invariant
("Physics tick(s) for this frame complete; accumulator advanced;
deterministic byte-equal world snapshot") demands a deterministic
sub-step count.

State:

- `tick_seconds_ : double` — set at FrameLoop construction; default
  `1.0/60.0` per the `core/SPEC.md` §9 reference scenario.
- `accumulator_ : double` — accumulates `dt` between displayed
  frames; consumed in fixed-size chunks by phase 3.
- `max_ticks_per_frame_ : u32` — cap to prevent the spiral of death
  on a stalled frame. Default `4` (chosen so a one-frame stall up to
  ~67 ms never produces a runaway sub-step burst).
- `interp_alpha_ : float` — fractional position inside the current
  fixed step; published to phases 4–6 via `SystemContext`. Range
  `[0.0, 1.0)`.

`consume_fixed_steps_()` returns the integer number of physics
sub-steps to run this frame, then subtracts that many `tick_seconds_`
from the accumulator. The remainder feeds `interp_alpha_`. If
`accumulator_ / tick_seconds_ ≥ max_ticks_per_frame_`, the loop
caps the count and *discards* the excess accumulation (deliberate
spiral-of-death cure: reproducible behavior at the cost of slow
motion under sustained stall — acceptable for MVP since the
`perf-budget.md` headroom rule keeps this case off the critical
path).

The accumulator starts at zero; the first call to `tick(dt)` runs
zero physics sub-steps if `dt < tick_seconds_`. This matches
harmonius `FixedTimestep::consume`'s semantics and is the
reference for TC-1.1.2.* tests in §11.

### 3.5 `CommandBuffer` flush points

Per `core/SPEC.md` §4.8 invariant 3, deferred mutations recorded by
a system into its `CommandBuffer` become visible only at the next
phase boundary. The schedule's flush points:

- **Per-system flush at phase exit** (MVP rule): inside
  `run_phase_`, after every system in `compiled_[p]` has run, the
  loop drains every system's `CommandBuffer` *in topological order*
  before exiting the phase. Drain order matches system execution
  order so that a write recorded by system A is observable to a
  read by a later system in phase `p+1` deterministically.
- **No mid-phase flush**: a system reading a component within phase
  `p` does not see another system's deferred mutation from the same
  phase, even if A executed before B in `compiled_[p]`. This is the
  "phase boundary == sync point" invariant from `frame-phases.md`
  §"Notes on the ordering choice".

Flushes that fail (`core::Error::CommandBufferOverflow`) are logged
at `warn` and the buffer is cleared (`core/SPEC.md` §10.1 row); the
phase exit proceeds. The system that overflowed is the one named in
the structured log entry's `error.detail`. CI catches this via the
`core/command-buffer: per_system_arena_drain` `BENCHMARK_CELL`
(§9.5 of `core/SPEC.md`).

### 3.6 Tick advance and frame index

The world `ChangeTick` (`core/SPEC.md` §4.4 + change-detection
contract from harmonius's design) increments **exactly once per
displayed frame**, at the end of phase 9 (present). Reasons for
choosing phase 9:

- Phase 8 (hot-reload) is the last point at which any system code
  could run for frame N; if the tick advanced before phase 8, a
  reload's `glibre_plugin_register` would observe `current_tick == N+1`
  for a frame still being assembled.
- Advancing at phase 9's exit means systems in phase 1 of frame N+1
  observe `current_tick == N+1` and `last_run_tick` snapshots from
  frame N as `N`, so `Changed<T>` queries see exactly the writes
  performed during frame N (the harmonius change-detection
  semantics, re-derived).

`frame_index_` increments alongside the tick. It is exposed via
`FrameLoop::frame_index()` (`core/SPEC.md` §5.7) and used by
profiling traces, golden-snapshot fixtures, and the `RenderFrame`
extract's `frame_index` field (owned by render). The increment is
not user-visible elsewhere; it is information, not contract.

### 3.7 Aggregate composition

```
FrameLoop (per-World driver)
├── Schedule
│   └── CompiledPhase[1..=9]
│       └── SystemThunk[]                  ← topologically sorted
├── HotReloadBarrier&                      ← invoked between phases 7 and 9
├── World&                                 ← target of every system
└── PhaseHooks[1..=9]                      ← optional observer hooks
```

`Schedule` and `FrameLoop` are sibling aggregates inside this
detailed design; `core/SPEC.md` §4.4 names them as one
"System / Schedule / Phase / FrameLoop" aggregate because their
invariants are inseparable (a phase boundary in `FrameLoop` is the
sync point for `Schedule`-recorded `CommandBuffer`s; a schedule
recompile invalidates `FrameLoop`'s `compiled_` array). The split
above is purely an internal-architecture decomposition, not an ABI
seam — both expose their public surface through the §5 stub.

## 4. Public surface

The public surface is frozen in `core/SPEC.md` §5; this design does
not add new types or signatures. For convenience, the load-bearing
declarations (verbatim from §5):

```cpp
namespace glibre::core {

// 5.3 — closed phase enum (frame-phases.md).
enum class Phase : std::uint8_t {
    Input         = 1,  Logic       = 2,  PhysicsFixed = 3,
    Animation     = 4,  Transform   = 5,  CullExtract  = 6,
    RenderSubmit  = 7,  HotReload   = 8,  Present      = 9,
};

// 5.6 — system registration via access set + manifest declaration.
struct AccessSet {
    eastl::span<const TypeId> reads{};
    eastl::span<const TypeId> writes{};
    eastl::span<const TypeId> without{};
};

struct SystemDesc {
    eastl::string_view name{};       // FQN — deterministic tiebreaker.
    Phase            phase{};
    AccessSet        access{};
    SystemFn         body{nullptr};
    eastl::span<const eastl::string_view> after{};
    eastl::span<const eastl::string_view> before{};
};

class Schedule {
public:
    [[nodiscard]] Result<SystemId> register_system(const SystemDesc&) noexcept;
    [[nodiscard]] Result<void>     unregister_system(SystemId) noexcept;
    [[nodiscard]] Result<void>     compile() noexcept;
};

// 5.7 — per-World driver.
using PhaseHookFn = void (*)(World& world, Phase phase) noexcept;

struct PhaseHooks {
    PhaseHookFn on_enter{nullptr};
    PhaseHookFn on_exit{nullptr};
};

class FrameLoop {
public:
    [[nodiscard]] static Result<FrameLoop*>
    create(World& world, Schedule& schedule) noexcept;
    static void destroy(FrameLoop* loop) noexcept;

    [[nodiscard]] Result<void>   tick(double delta_seconds) noexcept;
    [[nodiscard]] Result<void>   set_phase_hooks(Phase, PhaseHooks) noexcept;

    [[nodiscard]] double         accumulator() const noexcept;
    [[nodiscard]] std::uint64_t  frame_index() const noexcept;
};

} // namespace glibre::core
```

Surface invariants this design imposes on top of the §5 stub:

1. **No exceptions cross the boundary.** Every method is `noexcept`;
   every fallible method returns `glibre::Result<T>` =
   `std::expected<T, glibre::Error>` per `error-model.md`. Plugin
   `SystemFn` bodies are also `noexcept` (signature in §5.4 of
   `core/SPEC.md`); a system that needs to fail returns the error
   through its `CommandBuffer` or via a component write, not through
   a thrown exception.
2. **`SystemDesc::name` must be a fully-qualified compile-time
   string** (e.g. `"glibre.core.transform.propagate"`). The
   manifest's `SystemDecl.name` (plugin-abi.md) provides this; the
   string lifetime is the plugin's `.rodata` section so the borrow
   in `std::string_view` is stable across the plugin's lifetime.
3. **`SystemFn` must not capture state**. It is a free function
   pointer; per `core/SPEC.md` §5.4 closures are out of scope so
   plugin systems remain ABI-stable across reloads.
4. **`PhaseHookFn` callbacks must not mutate world state.** They
   are observer hooks (profiler ticks, e2e trace markers). The
   `World&` is a non-const reference solely so hooks may read
   resource accessors that themselves are non-const (a quirk of
   `World`'s API surface); writing through the reference inside a
   hook is undefined behavior caught by the §6.10 access-token
   check in debug builds.
5. **`FrameLoop::tick` is single-threaded by contract.** Per
   `core/SPEC.md` §6.10 the MVP runs every system on the game-loop
   driver thread; calling `tick` concurrently from two threads is
   undefined behavior. Plan #496 will introduce a worker pool
   without changing this signature.
6. **No public `Phase` insertion API.** The enum is closed. Adding
   a tenth phase is a `frame-phases.md` amendment spike whose
   output edits this design and the §5 stub in lockstep.

Error arms emitted directly by this aggregate's public surface
(per `core/SPEC.md` §10.1):

- `core::Error::ScheduleAccessConflict` — `Schedule::compile()`.
- `core::Error::SystemScheduleCycle` — `Schedule::compile()`,
  `Schedule::register_system` (when register triggers a recompile
  that finds a cycle).
- `core::Error::FramePhaseMisordered` — debug-build assertion in
  `FrameLoop::tick`; logged at `error` and aborts.
- `core::Error::CommandBufferOverflow` — bubble-up from per-system
  flush (recovery: log+continue per §3.5 above).

## 5. Hot/cold path split

### 5.1 Hot path (every frame)

The hot path is everything `FrameLoop::tick` invokes. Per
`perf-budget.md` Per-Context Budget Table, `core`'s share is
**0.45 ms CPU combined (sim + submit)**; the schedule + frame-loop
aggregate consumes ~0.20 ms of that budget (`core/SPEC.md` §9.3:
`Schedule` ~0.10 ms + `FrameLoop` ~0.05 ms + `CommandBuffer` pool
~0.10 ms across all contexts; aggregate is annotated `~0.50 ms`
slack-inclusive). Hot-path operations:

1. **Phase walker dispatch** — the nine-entry `switch` over
   `Phase`. Compiles to a function-pointer table (one entry per
   phase 1..=9). Per phase, the inlined `run_phase_` calls into
   the compiled order array.
2. **`CompiledPhase` walk** — flat `eastl::vector<SystemThunk>`
   iteration. Each thunk is a codegen-emitted call into the
   plugin's `SystemFn` with a stack-built `SystemContext`. No
   virtual calls; no `eastl::variant` visiting; no allocations.
3. **`SystemContext` build** — a stack-only struct binding
   `(World&, CommandBuffer&, ChangeTick, Phase)`. Construction is
   four pointer/scalar copies.
4. **`CommandBuffer` flush at phase exit** — per `core/SPEC.md`
   §3.5 the schedule drains every system's `CommandBuffer` in
   topological order. Each drain is a linear walk over the arena;
   `O(recorded_count)`.
5. **Phase 8 idle path** — `barrier.step()` is a single relaxed
   atomic load on `pending_` when no reload is queued
   (`hot-reload-protocol.md` §"Decision"; sub-microsecond).
6. **Tick advance** — `ChangeTick` increment + frame counter
   bump at phase 9 exit (~handful of cycles).

### 5.2 Cold path (rare events)

Cold-path operations do not run in the per-frame hot loop and are
budgeted under `perf-budget.md`'s per-reload one-shot ceiling
(0.40 ms phase 8 budget for hot-reload frames):

1. **`Schedule::register_system` / `unregister_system`** — called
   only from `glibre_plugin_register` during plugin load (process
   start) or from the hot-reload barrier's resume step. Mutates
   the registered-system table; defers the recompile to the
   `compile()` call that follows.
2. **`Schedule::compile`** — runs nine times (once per phase) at
   process start and on every successful hot-reload transaction.
   `O(systems²)` worst-case access-set intersection; bounded by
   total registered system count. Spike #493 owns the fall-back
   trigger to per-`(reads, writes)`-set bucketing.
3. **`set_phase_hooks`** — observer-hook registration; called
   from tools/editor/e2e startup, not during a frame.
4. **`barrier.step()` reload-frame path** — the four-step
   drain → swap → migrate → resume sequence (`hot-reload-
   protocol.md`). Up to 0.40 ms one-shot per reload; not
   counted against the steady-state CPU budget per
   `perf-budget.md` §"Pipelined Frame Timing".

### 5.3 Why the split matters

The hot-path budget (~0.20 ms of `core`'s 0.45 ms cell) is paid
every frame at 60 Hz; drift here trips the per-PR
`perf-budget.yml` p99 gate (`perf-budget.md` §"CI Gate Spec" rule
2). The cold-path budget is paid only on plugin load/reload
(seconds-apart in dev workflows, never in shipping). The split is
deliberately not data-driven: phase identity is a hard-coded
`switch` (no runtime lookup); compiled order is a precomputed flat
array (no per-frame DAG walk); the only `std::atomic` on the hot
path is the hot-reload pending counter, which is single-load
relaxed.

## 6. Concurrency

### 6.1 MVP — single-threaded by contract

Per `core/SPEC.md` §6.10 every system runs on the game-loop driver
thread. `FrameLoop::tick` is called from one thread; the schedule
walks `CompiledPhase` sequentially; per-system `CommandBuffer`s are
drained in topological order at phase exit. Concurrency model:

- **One writer per `World`.** The driver thread holds the
  effective exclusive write lock for the duration of `tick()`.
  Other threads (the platform plugin's OS event-loop thread, the
  GPU completion thread that signals the swapchain) communicate
  with the driver via SPSC queues drained inside their
  context-owned phases (phase 1 input drain; phase 9 swapchain
  signal). Those queues are platform-private; `core` does not
  expose a queue API.
- **One reader per ECS chunk.** While a phase is in flight, at
  most one system reads or writes any given `(Archetype, Chunk,
  Column)` triple (`core/SPEC.md` §4.11 invariant 5: "Single-writer
  per chunk"). Enforced by the `Schedule`'s access-set DAG (§3.2
  step 5); single-thread MVP makes this vacuous (one system runs
  at a time) but the manifest-level access set is collected so
  the post-MVP parallel dispatcher inherits the rule.
- **Phase boundaries are sync points.** A system's writes are
  visible to phase `p+1` readers exactly via the per-system
  `CommandBuffer` flush at phase exit (§3.5). No cross-phase
  borrow leaks across the boundary; `SystemContext` is destroyed
  at the system body's return.
- **Hot-reload phase 8 is single-threaded by contract.** The
  loader runs on the game-loop thread; observer notifications are
  synchronous on that same thread (`hot-reload-protocol.md`
  §"Decision"). The `World` exclusive write lock during phase 8
  is documentation, not contention, in MVP — no system runs.

### 6.2 Post-MVP — per-system parallelism (spike #496)

The schedule data already collects `(reads, writes)` per system; a
fork-join thread pool fanning out independent DAG nodes per phase
with a fence at the phase exit drops in without re-spec'ing the
schedule shape. The codegen step that today emits per-system
thunks (`core/SPEC.md` §6.4) will tomorrow emit a parallel-dispatch
wrapper. Public §5 surface does not change.

When parallelism lands:

- **Data-race avoidance via ECS chunk locks.** Each archetype
  chunk gets a per-chunk exclusive-or-shared advisory lock,
  acquired by the dispatcher (not by user code) before scheduling
  a system that reads/writes any column in the chunk. The
  `Schedule`'s precomputed access-set partition tells the
  dispatcher which chunks each system touches; conflicts are
  pre-resolved by the topological sort, so locks are uncontended
  in the steady state. (Lock primitives: `eastl::shared_mutex`
  per chunk; lock acquisition is integrated into the
  `SystemThunk`'s codegen wrapper, not exposed to plugin code.)
- **Single-writer-per-chunk holds**. The DAG ensures that two
  systems whose write sets intersect any same `(Archetype, Chunk,
  Column)` triple never run concurrently; `Schedule::compile`
  refuses such a partition with `ScheduleAccessConflict`.
- **Phase exit fence.** Every system in phase `p` joins before
  any system in phase `p+1` dispatches. This preserves the
  per-phase invariants in the §3.1 table.
- **`CommandBuffer` flush ordering** is preserved: at phase exit
  the dispatcher drains each system's buffer in the precomputed
  topological order (the same order MVP uses), so concurrent
  recording does not change the visible flush sequence.
- **Determinism preserved.** Lexicographic tiebreak (§3.2 step 4)
  fixes the topological order across hosts; the phase-exit fence
  removes parallelism's apparent nondeterminism from the
  cross-phase observable state.

The transition from MVP single-thread to post-MVP parallel does
not require any §5 ABI change. It is a pure-additive change to
the dispatcher inside `core/src/schedule/`.

### 6.3 Cross-thread state (out of scope for this aggregate)

Some state is shared across threads outside the aggregate's
ownership and is documented for completeness:

- **Platform → game-loop SPSC** — owned by `platform`. Drained
  inside phase 1.
- **GPU completion → game-loop signal** — owned by `platform` /
  `render`. Consumed inside phase 9.
- **Hot-reload watcher → game-loop request** — owned by tooling.
  Posts via `HotReloadBarrier::request_reload`, which appends to
  a request queue read by `barrier.step()` at phase 8. The queue
  itself is `eastl::vector` under a coarse mutex (request rate is
  human-timescale; contention is irrelevant). Per-aggregate
  budget under `core/SPEC.md` §9.3 `HotReloadBarrier` row.

This aggregate's public surface (§5) is single-threaded; cross-
thread plumbing is handled exclusively by the listed peers.

## 7. Persistence + ABI

### 7.1 What is persisted (almost nothing)

Most schedule + frame-loop state is **transient** and lives only for
the duration of a process run:

- `CompiledPhase[]` — rebuilt on every plugin load/unload event.
  Never serialized.
- `SystemThunk` jump tables — codegen-emitted constants in the
  middleman or plugin `.rodata`. Never serialized.
- `accumulator_`, `interp_alpha_`, `frame_index_` — runtime
  scalars, reset on FrameLoop construction.
- Per-system `CommandBuffer` arenas — per-frame transient (drain
  + reset at phase exit per §3.5).
- `pending_requests_` (the hot-reload queue this aggregate
  forwards into) — per-process.

None of the above has a `.fory` schema. None survives a process
restart. None requires a Fory-codegen migration.

### 7.2 What crosses the plugin ABI

Three load-bearing items cross the plugin ABI through this
aggregate:

1. **`SystemDecl` (in `manifest.systems`)** — the manifest field
   declared by every plugin per `plugin-abi.md` §"Plugin Manifest
   Schema". Crosses via the Fory-serialized manifest blob baked
   into the plugin's `.rodata`. Schema:
   ```fory
   schema glibre.core.SystemDecl {
     version 1
     field name      : string         tag 1 since 1
     field phase     : u8             tag 2 since 1
     field reads     : list<string>   tag 3 since 1
     field writes    : list<string>   tag 4 since 1
     field after     : list<string>   tag 5 since 1
     field before    : list<string>   tag 6 since 1
   }
   ```
   The schema is owned by `plugin-abi.md`, not redefined here;
   any change to it bumps the global ABI hash via
   `glibre_types_abi_hash` (`fory-codegen.md`).
2. **`Phase` enum value (`SystemDecl.phase`)** — `u8` 1..=9 from
   `frame-phases.md`. Stable across the engine's lifetime;
   renumbering is a `frame-phases.md` amendment spike that bumps
   the ABI hash.
3. **`SystemFn` function pointer** — exported by the plugin and
   pointed-to by the loader's `SystemThunk` table. ABI-stable in
   the C-ABI sense (free function, no captures, `noexcept`); the
   pointer's lifetime is the plugin dylib's lifetime (i.e.,
   invalidated by hot-reload). The loader does not serialize
   function pointers; they are re-resolved on every plugin load
   from `manifest.systems[i].name` plus the plugin's
   `glibre_plugin_register` registration calls.

No schedule state crosses a hot-reload barrier directly; the
barrier rebuilds the schedule from the post-swap union of plugin
`SystemDecl`s. State **observed** to survive (per
`hot-reload-protocol.md` §"State Survival Rules") is the world
data the schedule operates on, not the schedule itself.

### 7.3 ABI hash dependency

This aggregate contributes to the global `glibre_types_abi_hash`
via:

- `SystemDecl` schema (above) — any field-level change bumps the
  hash (`fory-codegen.md` §"ABI Hash Function").
- `PluginManifest.systems` field (tag 6 in `plugin-abi.md`) — any
  type change bumps the hash.

Layout changes to in-process types (`SystemDesc`, `AccessSet`,
`PhaseHooks`, the `CompiledPhase` shape) do **not** bump the hash;
they are private to the engine's `core` library and not crossed by
the plugin ABI. They are gated by the engine's own SemVer
(`PluginManifest.min_engine_version`; `plugin-abi.md`
§"Versioning Rules").

## 8. Hot-reload

The schedule + frame-loop aggregate is a **consumer** of the
hot-reload barrier, not a participant in the four-step state
machine. Its hot-reload contract:

### 8.1 What survives

Nothing the aggregate owns directly. The schedule is rebuilt; the
frame loop's runtime scalars (`accumulator_`, `frame_index_`,
`interp_alpha_`) carry through unchanged because they are owned by
core and never crossed the swap; the per-system `CommandBuffer`s
are drained as part of step 1 (drain) per
`hot-reload-protocol.md`'s drain contract.

### 8.2 What is rebuilt

- **`CompiledPhase[1..=9]`**. After step 4 (resume) of every
  per-plugin transaction in a phase-8 run, the union of every
  loaded plugin's `SystemDecl` set has changed (Q replaced P; Q
  may add/drop systems relative to P). The schedule's compile
  step (§3.2) re-runs nine times. If any phase yields
  `core::Error::SystemScheduleCycle` or
  `core::Error::ScheduleAccessConflict`, the loader rolls back the
  offending plugin's registration per `plugin-abi.md` §"Loader
  Sequence" step 10 and the previous-good `CompiledPhase` is
  restored. (`core/SPEC.md` §6.4 algorithm step 3.)

  **Rebuild trigger** is simple: any successful
  `glibre_plugin_register` call in a hot-reload step 4
  (`hot-reload-protocol.md` §"Step 4 — Resume"). Spike #493
  resolves whether to rebuild lazily (only the affected phase) or
  eagerly (all nine); MVP rebuilds all nine eagerly because the
  per-phase build is `O(systems²)` over the *phase's* system
  count, and phase counts in MVP are small (typically
  `< 50 systems / phase`).

- **`SystemThunk` tables**. Pointers to `SystemFn`s in the
  outgoing plugin become invalid the moment `dlclose` runs in
  step 2 (swap). The thunk table is rebuilt as part of the
  `CompiledPhase` rebuild; no stale pointers persist past phase
  8's exit because the schedule recompile completes inside step 4
  (resume) of `barrier.step()`, before phase 9 of the same
  displayed frame begins.

### 8.3 Triggers refusal

The schedule + frame-loop aggregate raises **two** refusal arms
during a hot-reload transaction:

1. `core::Error::SystemScheduleCycle` — the post-swap union of
   every plugin's `(after, before)` declarations introduces a
   cycle. Wrapped under the `core::Error::HotReload` umbrella per
   `core/SPEC.md` §10.1; `HotReloadRefusedEvent` fires; the
   transaction rolls back per `hot-reload-protocol.md` §"Failure
   & Rollback" step-3 / step-4 path.
2. `core::Error::ScheduleAccessConflict` — same trigger shape;
   two systems (one from Q, one from another loaded plugin) have
   conflicting access sets that no DAG ordering resolves. Same
   rollback discipline.

Both refusals leave the previous-good plugin live and the
previous-good `CompiledPhase` array in place; the next frame
ticks unmodified. Operator action is required to fix the
manifest (rename a system, narrow its access set, add an
explicit `after` / `before` edge); the next reload retries.

### 8.4 Schedule rebuild trigger (cross-references spike #493)

Spike #493 (`[SPIKE] schedule-rebuild-bucketed-O-systems-trigger`)
is **dependency-only** for this aggregate; it resolves §6.11 of
`core/SPEC.md` (the hot-loop big-O fall-back) but does not block
this design. Resolution dictates whether MVP's eager
all-phase rebuild upgrades to a per-`(reads, writes)`-bucketed
build before plan-level implementation lands. If #493 lands first
and recommends the bucketed build, the §3.2 algorithm gets the
upgrade in-place and this design is amended in lockstep. If MVP
implementation lands first, the eager `O(systems²)` build is
shipped and #493's bench measures it on real workloads.

This dependency is **not** a blocker for opening the implementation
plan: the §3.2 algorithm is correct and complete on its own; #493
strictly improves the constant factor.

## 9. Performance

### 9.1 Per-phase CPU/GPU budget cells (cited from `perf-budget.md`)

The schedule + frame-loop aggregate's contribution to per-phase
budgets, per `perf-budget.md` §"Pipelined Frame Timing":

| Phase | Schedule + FrameLoop's contribution                                         | Phase total (engine-wide, owning context)                       |
|-------|-----------------------------------------------------------------------------|-----------------------------------------------------------------|
| 1     | dispatch overhead only (~few µs)                                            | 0.10 ms — owner: platform                                       |
| 2     | 0 ms — empty body in MVP                                                    | 0.00 ms — owner: gameplay (deferred)                            |
| 3     | dispatch overhead × `ticks` (substep loop)                                  | 2.00 ms — owner: physics                                        |
| 4     | 0 ms — empty body in MVP                                                    | 0.00 ms — owner: animation (deferred)                           |
| 5     | dispatch overhead + transform-propagation system body                       | 0.30 ms — owner: core (this design's slice)                     |
| 6     | dispatch overhead only                                                      | 1.20 ms — owners: render (0.10) + geometry (0.30) + tools (0.80)|
| 7     | dispatch overhead only                                                      | 1.80 ms — owners: render (1.40) + geometry (0.20) + tools (0.20)|
| 8     | barrier.step() invocation; idle = sub-µs; reload = ≤0.40 ms one-shot         | <0.10 ms steady-state — owner: core (HotReloadBarrier; not this aggregate) |
| 9     | dispatch overhead + tick advance + frame counter bump                       | 0.05 ms — owner: platform                                       |

This aggregate's per-frame budget (the §9.3 `core/SPEC.md` rows
`Schedule + FrameLoop + CommandBuffer pool`) sums to approximately
0.25 ms (0.10 + 0.05 + 0.10), all sim-half. The `CommandBuffer`
arena drains are amortized across every phase that has registered
systems; phase 8 has no systems and contributes zero `CommandBuffer`
work.

### 9.2 Hot-loop ceiling ~0.5 ms (shared with §6.11)

`core/SPEC.md` §6.11 names the phase ordering hot loop with a
**~0.5 ms ceiling** for `core`'s share. This aggregate is the
owner of that ceiling. Drift trips the per-PR `perf-budget.yml`
p99 gate (`perf-budget.md` §"CI Gate Spec" rule 2):
`p50 CPU (sim+submit) > 8.05 ms` or `p99 > 13.67 ms` fails the PR.

### 9.3 `BENCHMARK_CELL` blocks (cited from `core/SPEC.md` §9.5)

| `BENCHMARK_CELL` test name                          | CPU ceiling | Heap ceiling | Aggregate                    |
|-----------------------------------------------------|-------------|--------------|------------------------------|
| `core/schedule: phase_ordering_hot_loop`            | 0.10 ms     | 4 MiB        | `Schedule`                   |
| `core/frame: dispatch_overhead`                     | 0.05 ms     | 4 MiB        | `FrameLoop`                  |
| `core/command-buffer: per_system_arena_drain`       | 0.10 ms     | 4 MiB        | `CommandBuffer` pool         |

These three blocks live under `tests/core/schedule/` and
`tests/core/frame/`; the §11 test plan below lists them as MUST
exist before a plan PR may merge.

### 9.4 Budget enforcement points

The schedule + frame-loop aggregate enforces timing budgets at
three points:

1. **Build time**: `Schedule::compile` budget — see §6.11 of
   `core/SPEC.md` for the hot-reload-frame ceiling
   (≤0.40 ms one-shot for the eager all-phase rebuild). Plan
   #493's bench resolves the trigger to the bucketed
   amortization.
2. **Per-frame steady state**: `BENCHMARK_CELL` blocks above;
   asserted under the S1 fixture
   (`perf-budget.md` §"Justification Per Cell").
3. **Allocator ceiling**: every allocation in `core/src/schedule/`
   and `core/src/frame/` carries `ContextTag::core` per
   `perf-budget.md` Allocator Rule #1; debug builds return
   `core::Error::OutOfBudget` on overshoot
   (`core/SPEC.md` §9.4 rule 2).

## 10. Failure modes

The closed enumeration of `core::Error` arms emitted by this
aggregate (subset of `core/SPEC.md` §10.1):

| `core::Error` arm                | Trigger                                                                                                       | Recovery   | Severity                      | Observer event                |
|----------------------------------|---------------------------------------------------------------------------------------------------------------|------------|-------------------------------|-------------------------------|
| `ScheduleAccessConflict`         | Two systems' `(reads, writes)` overlap such that no DAG ordering is conflict-free (§3.2 step 5).               | Refuse / Rollback (in hot-reload) | `error` (load) / `warn` (hot-reload) | none / `HotReloadRefusedEvent` |
| `SystemScheduleCycle`            | `(after, before)` declarations form a cycle in the post-union graph (§3.2 step 3).                              | Refuse / Rollback (in hot-reload) | `error` (load) / `warn` (hot-reload) | none / `HotReloadRefusedEvent` |
| `FramePhaseMisordered`           | Debug-build assertion: a phase observed a write from a later phase of the same frame (§3.1 invariant).         | Abort      | `error`                       | none                          |
| `CommandBufferOverflow`          | Per-system `CommandBuffer` flush at phase exit drained more bytes than the arena cap (§3.5).                   | Refuse (clear + log) | `warn`                  | none                          |

Cross-cutting notes (per `core/SPEC.md` §10.2):

- **Severity escalation under hot-reload.** When raised inside
  `barrier.step()`'s rebuild path (rebuild fired by step 4
  resume), `ScheduleAccessConflict` and `SystemScheduleCycle` log
  at `warn` and roll up under the `core::Error::HotReload`
  umbrella with `HotReloadRefusedEvent`. When raised at process
  start during the initial `Schedule::compile()`, both log at
  `error` (no prior-good schedule to fall back on; the engine
  fails to start).
- **`FramePhaseMisordered` is debug-only.** Per `core/SPEC.md`
  §4.4 invariant 1 the violation is undefined behavior in
  shipping; debug builds detect via the per-thread access-token
  check (§6.10 of `core/SPEC.md`) and abort. CI shipping-build
  fixtures do not exercise this arm.
- **`CommandBufferOverflow` is not fatal.** The flush failure
  drops the buffer's contents (the system's deferred mutations
  for that frame are lost); the next frame's `CommandBuffer` is
  fresh. Plugin authors are responsible for sizing their arena
  caps via `SystemDesc` (post-MVP; MVP uses a uniform default).
  Counter increments are surfaced in the editor's perf HUD
  (post-MVP).

The aggregate does **not** emit `HotReload*` arms directly; those
are owned by `HotReloadBarrier`. It does **not** emit allocator
arms (`OutOfBudget`); those propagate up from
`PerContextAllocator` (`core/SPEC.md` §10.2 #6).

## 11. Test plan

Tests are split between Catch2 unit tests (per
`tests/core/schedule/` + `tests/core/frame/`) and Catch2 + golden
fixture integration tests under `tests/core/integration/`. Every
test names a §10 row, a §3 algorithm step, or a §5 surface
contract.

### 11.1 Unit tests — `Schedule`

| TC ID                                       | Trigger                                                                  | Expectation                                                              | §-link        |
|---------------------------------------------|--------------------------------------------------------------------------|--------------------------------------------------------------------------|---------------|
| `core/schedule/build/empty_phase_compiles`  | Compile a `Schedule` with zero registered systems in a given phase.       | Produces an empty `CompiledPhase`; `compile()` returns `Result<void>` ok. | §3.2 step 1   |
| `core/schedule/build/single_system`         | Register one system in phase 5 with reads = `{Transform}`, writes = `{}`.| `compile()` ok; phase 5's `CompiledPhase` has one entry.                  | §3.2 step 6   |
| `core/schedule/build/raw_hazard_orders`     | Two systems, A writes `Foo`, B reads `Foo`, no explicit `after`/`before`.| Topological order is `A → B`.                                             | §3.2 step 2   |
| `core/schedule/build/waw_hazard_orders`     | Two systems, A and B both write `Foo`.                                   | Topological order is alphabetic by FQN (deterministic tiebreak).          | §3.2 step 4   |
| `core/schedule/build/explicit_after_overrides`| A.after = `[B]`, no read/write hazard.                                  | Topological order is `B → A`.                                             | §3.2 step 2   |
| `core/schedule/build/cycle_refuses`         | A.after = `[B]`, B.after = `[A]`.                                        | `compile()` returns `unexpected(SystemScheduleCycle)`.                    | §3.2 step 3   |
| `core/schedule/build/access_conflict_refuses`| In a hypothetical parallel build, A and B both write `Foo` with no edge.| `compile()` returns `unexpected(ScheduleAccessConflict)` (vacuous in MVP single-thread; assert via test seam exposed only under `GLIBRE_E2E`). | §3.2 step 5 |
| `core/schedule/build/recompile_idempotent`  | Compile, then compile again with no registration changes.                | Second compile returns ok with no observable side effect.                 | §3.2 step 6   |
| `core/schedule/build/recompile_invalidates` | Compile, register a new system, compile.                                 | New system appears in its phase's `CompiledPhase`.                        | §3.2 step 6   |
| `core/schedule/build/phase_out_of_range`    | `SystemDesc.phase = 0` or `= 10`.                                        | `register_system` returns `unexpected(FramePhaseMisordered)` (debug) / refuses with that arm. | §3.1 |
| `core/schedule: phase_ordering_hot_loop`    | `BENCHMARK_CELL` over a 50-system synthetic workload.                    | Mean time < 0.10 ms; p99 < 0.20 ms; resident heap < 4 MiB.                | §9.3          |

### 11.2 Unit tests — `FrameLoop`

| TC ID                                            | Trigger                                                                | Expectation                                                              | §-link        |
|--------------------------------------------------|------------------------------------------------------------------------|--------------------------------------------------------------------------|---------------|
| `core/frame/tick/calls_phases_in_order`          | Register an `on_enter` hook on every phase that records the phase id.  | Hook record is `[1,2,3,4,5,6,7,8,9]` after one `tick()` call.            | §3.3          |
| `core/frame/tick/empty_phases_skipped`           | Phases 2 and 4 have no systems registered.                             | Phase 2 and 4 hooks fire; their `CompiledPhase` walk is a no-op.         | §3.3          |
| `core/frame/tick/zero_dt_no_substeps`            | `tick(0.0)` with empty accumulator.                                    | Phase 3 invoked zero times.                                              | §3.4          |
| `core/frame/tick/one_substep`                    | `tick(1/60.0)` once.                                                   | Phase 3 invoked exactly once.                                            | §3.4          |
| `core/frame/tick/multi_substep`                  | `tick(2.5/60.0)` once.                                                 | Phase 3 invoked twice; remainder feeds `interp_alpha_ ≈ 0.5`.            | §3.4          |
| `core/frame/tick/spiral_of_death_capped`         | `tick(10.0)` once with `max_ticks_per_frame_ = 4`.                     | Phase 3 invoked exactly 4 times; remainder of accumulator discarded.     | §3.4          |
| `core/frame/tick/alpha_zero_at_boundary`         | `tick(1/60.0)` then read `interp_alpha`.                               | `interp_alpha == 0.0` (within float epsilon).                            | §3.4          |
| `core/frame/tick/alpha_half_at_half_step`        | `tick(1.5/60.0)`.                                                      | `interp_alpha == 0.5`.                                                   | §3.4          |
| `core/frame/tick/change_tick_increments_at_phase_9`| Read `World::current_tick` from on_exit hook on each phase.            | Tick observed `T` for phases 1..=8 of frame N; `T+1` from phase 1 of N+1.| §3.6          |
| `core/frame/tick/frame_index_increments`         | Two consecutive `tick()` calls.                                        | `frame_index()` advances by 2.                                           | §3.6          |
| `core/frame/tick/phase_hooks_dont_mutate`        | Register a hook that attempts a write through `World&`.                | Debug build aborts with `FramePhaseMisordered`-style access-token assert.| §4 invariant 4|
| `core/frame: dispatch_overhead`                  | `BENCHMARK_CELL` over an empty schedule (only platform/core systems).  | Mean time < 0.05 ms; resident heap < 4 MiB.                              | §9.3          |

### 11.3 Unit tests — `CommandBuffer` flush integration

| TC ID                                                    | Trigger                                                                | Expectation                                                              | §-link        |
|----------------------------------------------------------|------------------------------------------------------------------------|--------------------------------------------------------------------------|---------------|
| `core/command-buffer/flush/recorded_at_phase_exit`       | System A in phase 1 records a `set_component`; system B in phase 2 reads it. | B observes A's write on the same `tick()` call.                          | §3.5          |
| `core/command-buffer/flush/not_observable_intra_phase`   | System A and B both in phase 1; A records a write, B reads.            | B does not observe A's write within the same phase (per §3.5).           | §3.5          |
| `core/command-buffer/flush/topological_drain_order`      | Three systems in one phase; all three record writes to the same component. | Final value is the one written by the last topologically-ordered system.| §3.5          |
| `core/command-buffer/overflow/clears_and_logs`           | A system overflows its `CommandBuffer` arena.                          | Flush returns `CommandBufferOverflow`; arena is reset; next frame ok.    | §10           |
| `core/command-buffer: per_system_arena_drain`            | `BENCHMARK_CELL` over 100 systems each recording 32 mutations.         | Mean drain time < 0.10 ms; resident heap < 4 MiB.                        | §9.3          |

### 11.4 Integration tests

| TC ID                                                | Trigger                                                                 | Expectation                                                              | §-link        |
|------------------------------------------------------|-------------------------------------------------------------------------|--------------------------------------------------------------------------|---------------|
| `core/integration/frame/full_nine_phase_dispatch`    | S1-fixture-style world with platform / core systems registered; 600 ticks. | Every phase fires every frame in numeric order; `frame_index` advances 600. | §3.3          |
| `core/integration/schedule/hot_reload_recompiles`    | Trigger a hot-reload via `glibre::core::test::enqueue_hot_reload`; the new plugin's manifest adds a phase-5 system. | Post-reload `CompiledPhase[5]` includes the new system; no other phase mutated; tick continues. | §8.2 |
| `core/integration/schedule/hot_reload_cycle_refuses` | Reload a plugin whose manifest introduces `(after, before)` cycle.       | `barrier.step()` returns refusal; `HotReloadRefusedEvent` fires with `core::Error::HotReload` umbrella + inner `SystemScheduleCycle`; previous schedule stays live. | §8.3 |
| `core/integration/golden/error_stream_byte_equal`    | Drive each non-hot-reload §10 row's trigger; capture the structured error stream. | Stream is byte-equal across two runs (deterministic-replay obligation).  | §10.2 #7      |

### 11.5 Coverage matrix

Every §10 row has at least one test (Refuse-arms named in §11.1 –
§11.3; integration arms named in §11.4). Every §3 algorithm step
has at least one unit test. Every `BENCHMARK_CELL` from §9.3 is
named in §11.1 / §11.2 / §11.3. The §11.4 hot-reload tests
exercise the dependency on `HotReloadBarrier` (`core/SPEC.md`
§8.9 test hooks) without re-spec'ing the barrier itself.

## 12. Open questions

- **[OPEN]** Schedule rebuild trigger to bucketed `O(systems)`
  amortized build. Spike #493
  (`[SPIKE] schedule-rebuild-bucketed-O-systems-trigger`) owns
  this; this design ships with the eager `O(systems²)`
  all-phase rebuild on every hot-reload success and amends
  in-place when #493 lands. Risk: a high-system-count plugin set
  could push phase 8 above the 0.40 ms reload-frame budget; spike
  #493's micro-bench is the early-warning gate.
- **[OPEN]** Per-system parallelism seam — does `glibre-foryc`
  emit a single fork-join wrapper per `SystemThunk`, or does the
  schedule own a separate `ParallelCompiledPhase` shape? Spike
  #496 (`[SPIKE] per-system-parallelism-seam-foryc-vs-schedule`)
  resolves; this design ships single-thread MVP and admits the
  parallelism plan as pure-additive (§6.2).
- **[OPEN]** Phase-exit `CommandBuffer` flush vs per-system
  flush. §3.5 commits MVP to per-system drain at phase exit in
  topological order; an alternative is a single batched drain at
  phase exit. Resolve at the implementation plan that lands the
  command-buffer integration; both shapes preserve the §10
  failure semantics, so this is a perf decision (cache locality)
  not a contract decision.
- **[OPEN]** Triple-buffer between phase 6 (extract) and phase 7
  (submit) when post-MVP per-system parallelism + a separate
  render thread land. The harmonius `TripleBuffer<T>` design is
  the obvious shape; the question is whether this aggregate or
  the render context owns the buffer. Provisional answer: the
  buffer is render-private (the extract / submit seam belongs to
  render); core's contract remains "phase 6 publishes a finalised
  RenderFrame; phase 7 reads it." Confirm at the render-thread
  spike.
- **[OPEN]** `max_ticks_per_frame_` default. §3.4 picks `4`; this
  is reasonable for the S1 / 60 Hz reference scenario but
  arbitrary. A future variable-rate game-mode story may want
  per-game configuration; defer until a game mode that needs it
  exists. Listed as `frame-phases.md` open question #2 already.
- **[OPEN]** `SystemContext::is_substep()` accessor — useful for
  systems that need to distinguish the first substep of phase 3
  from later substeps. Not in §5 stub; add only when a concrete
  user (likely the physics SPEC) requests it. Not blocking.
- **[OPEN]** `PhaseHookFn` parameter shape — is `World&` the
  right surface, or should hooks receive a smaller observer-
  scoped type that statically forbids mutation? §4 invariant 4
  defines the expectation but enforcement is debug-build only.
  Tighten to a `WorldRO` view post-MVP if drift is observed.
