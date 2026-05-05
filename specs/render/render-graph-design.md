# render — Detailed Design: render-graph aggregate

> Detailed design for the `RenderGraph` / `GraphBuilder` / `Pass` /
> `ExecutionPlan` aggregate cluster declared in
> `specs/render/SPEC.md` §4.1.2 / §4.1.3 / §4.1.4 / §4.1.5. Refines
> §4.1.x, §5 (`GraphBuilder` + `RenderGraph` + `ExecutionPlan` surface),
> §6.2.2 (phase 7 build/compile/record), §6.3 (concurrency), §9.3
> (CPU phase-7 budget), §10 (graph-build / barrier / cycle errors), and
> §11 (acceptance stories #380 #381 #382 #383 #384 #385 #386 #391 #392 #396 #399 #401 #402)
> in place. Cites `reviews/decisions/error-model.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/fory-codegen.md`, and
> `reviews/decisions/frame-phases.md`. Introduces no new public surface
> beyond `specs/render/SPEC.md` §5; deviation from the cited records
> requires an amendment spike, not an in-place edit. Resolves
> `[SPIKE] design-render-render-graph-detailed` (#760).

## 1. Purpose

The render-graph aggregate is render's per-frame, per-`View` DAG
machinery. Per `specs/render/SPEC.md` §4.1.2 / §4.1.5 it owns:

1. The **`RenderGraph`** — an in-memory DAG of `Pass` nodes built
   in C++ each frame from a frozen `RenderFrame`. The graph is
   **never serialised** (PHILOSOPHY anti-pattern "serialised render-
   graph files"); it is C++ code visualised live by the editor.
2. The **`GraphBuilder`** — fluent, single-thread surface that
   plugins use to declare passes, their typed
   `(VirtualResource, AccessKind)` reads / writes, queue affinity,
   capability predicate, and `execute()` lambda. The builder is the
   only writer to the graph node list.
3. The **`Pass`** node — typed access-set declaration plus an
   `execute(MetalCommandBuffer&, const Bindings&)` lambda. Owns
   no resources; references them by `VirtualResourceHandle`.
4. The **`ExecutionPlan`** — the compiled output of one
   `RenderGraph::compile()` call: ordered pass list, barrier set,
   alias plan, queue assignment, binding-table offsets. Cached
   across frames keyed by structural hash of (pass set, capability
   set, view topology).
5. The **graph compile pipeline** — topological sort, capability
   gating, alias-plan colouring, minimum split-aware Metal 4 barrier
   emission, queue assignment, structural-hash keying.
6. The **plan record seam** — `ExecutionPlan::record_into(MetalCommandBuffer&)`
   is the only entry the per-pass workers call during the recording
   half of phase 7.

This aggregate **refuses to own**:

- **Pass bodies.** The `execute()` lambdas live in `passes/*.cpp`;
  their algorithmic content (mesh-shader gbuffer, deferred lighting,
  hybrid-RT shadow trace, post chain, …) is owned by individual
  pass-design tickets (#762 backend, #764 pass bodies, others under
  parent epic). The graph layer routes the lambda; it does not
  author it.
- **PSO compilation or residency.** The `PSOCache` aggregate
  (`SPEC.md` §4.1.7, design ticket #766) owns
  `(shader_hash, state_hash) → MTL::RenderPipelineState`. The graph
  layer references PSOs via `PSOHandle` only.
- **Metal device / queue lifecycle.** `MetalDevice`, `MetalQueue`,
  `MetalCommandBuffer` (`SPEC.md` §4.1.6) own the metal-cpp
  wrappers; the graph compiler emits *what* a pass records and
  *which* queue it lands on, not the wrapper construction.
- **Cull or extract.** Phase 6 produces the `RenderFrame` snapshot
  the graph builder reads; the graph never reaches back into ECS
  storage (`SPEC.md` §4.2 invariant 6). Specific algorithmic
  culling (frustum, HZB occlusion, meshlet cull, budget cull) is
  the cull aggregate (#770).
- **RT acceleration structures** (`SPEC.md` §4.1.8, ticket #768).
  The graph admits an `add_rt_pass` helper that consumes a
  `TLASHandle`; lifetime, refit, and rebuild policy belong to
  `RTAccelStructures`.
- **Persistent resources** (HZB, shadow atlas, history color,
  ring buffers). They appear in the graph only as
  `declare_persistent` / `declare_imported` references; their
  storage is owned by the resource aggregate (#772). The graph
  does manage *transient* virtual resources via the alias planner.
- **Frame schedule.** Phase 7 entry / exit is owned by `core` per
  `frame-phases.md`; the graph layer is invoked inside phase 7 and
  must complete before `core`'s phase-7 exit guarantee fires.
- **Graph serialisation.** Disk persistence of any DAG shape, plan,
  or barrier set is forbidden (PHILOSOPHY anti-pattern). The only
  cross-frame survivor is the in-memory `ExecutionPlan` cache, keyed
  by structural hash (a `u64`, not a file).

## 2. Requirements coverage

This section verifies that every harmonius MVP-scope requirement /
design clause about the render graph is either covered by the design
below or explicitly refused with rationale. Inputs (research only,
re-derived per PHILOSOPHY §"How harmonius is used"):

- `harmonius/docs/requirements/rendering/render-graph.md`
  (R-2.2.1 .. R-2.2.13, R-2.2.3a).
- `harmonius/docs/design/rendering/render-pipeline.md`
  § "Render Graph", § "Task Graph Integration".
- `harmonius/docs/design/rendering/rendering-core.md`
  § "Architecture", § "Data Flow".
- `harmonius/docs/design/rendering/pipeline-state-cache.md`
  (graph-side seam only; cache itself is #766).

| Harmonius clause                                                                                  | Glibre disposition                                                                                                                                                                                                                                                                                              |
|---------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-2.2.1** — Declarative pass authoring with read/write resource declarations.                    | **Covered.** §3.1 below: `GraphBuilder::add_raster_pass` / `add_compute_pass` / `add_rt_pass` take `(reads, writes)` spans of `(VirtualResourceHandle, AccessKind)` and an `execute` lambda. Declared = used is enforced (§3.6).                                                                                |
| **R-2.2.2** — Acyclic graph; cycle detection refuses compile.                                      | **Covered.** §3.4 step 1: Kahn's topological sort during `compile()`; cycle → `render::Error::RenderGraphCycle` (`SPEC.md` §10.1 row `GraphCycle`). No partial plan exposed (`SPEC.md` §4.1.2 invariant 1).                                                                                                      |
| **R-2.2.3 / R-2.2.3a** — Virtual resources with lifetime-disjoint aliasing onto physical heap.     | **Covered.** §3.5: alias-planner takes the topologically ordered pass list, computes `(first_write, last_read)` lifetimes per `VirtualResource`, builds the interference graph, colours it, places each colour on a `TransientPool` placement slot. Persistent + imported resources skip aliasing (§3.5 step 0). |
| **R-2.2.4** — Minimum split-aware barriers between dependent passes.                                | **Covered.** §3.4 step 4: barrier emitter walks adjacent edges in the compiled order, emits the minimum split memory + execution barrier per Metal 4's `MTLBarrierScope` API; redundant barriers refused at compile time (`SPEC.md` §4.1.5 invariant 1, story #383).                                            |
| **R-2.2.5** — Multi-queue scheduling (Graphics / Compute / Copy) with auto-fence emission.          | **Covered.** §3.4 step 5 + §3.7: the queue assignment is read directly off `Pass::queue` (declared via `PassDesc.queue`); cross-queue edges emit `MTLSharedEvent` fences. Story #385.                                                                                                                            |
| **R-2.2.6** — Cost-aware budget culling driven by historical GPU timing.                            | **Refused at this aggregate; routed to cull aggregate.** The graph admits `PassPriority` per `Pass` and respects it during build-time elision (§3.3); the *historical-timing-driven cost cull* lives in phase-6 `cull/budget.cpp` (`SPEC.md` §6.2.1 step 2.2; story #400 in cull #770). The graph's only role is to elide passes the cull aggregate decided to drop. |
| **R-2.2.7** — Multi-view fan-out from one extract; shared culling where possible.                   | **Covered.** §3.2: one `RenderGraph` is built per `View` from the same immutable `RenderFrame`; the builder thread reuses its compile cache across views with matching pass sets. Multi-view compile-cache hit avoids redundant topo sort (§3.8). Story #401.                                                  |
| **R-2.2.8** — Graph caching: same pass set + capability set + view topology → same compiled plan.   | **Covered.** §3.8: `ExecutionPlan` cache keyed by `structural_hash(pass_set, CapabilitySet, ViewHandle, RenderSettings_struct_hash)`; cache hit skips topo / colour / barrier emit and re-binds only. `SPEC.md` §4.1.5 invariant 3, story #384.                                                                  |
| **R-2.2.9** — Capability-gated pass elision; passes guarded by absent capability are absent.        | **Covered.** §3.3: each `PassDesc.requires_caps` is evaluated at *build time* against the live `CapabilitySet`; failing predicates drop the pass from the node list before topo sort. Shaders never branch on capability (PHILOSOPHY §6, `SPEC.md` §3.2 collapse #5/#9). Story #381.                              |
| **R-2.2.10** — Imported resources (swapchain, externally-vended buffers) treated as read-borrows.   | **Covered.** §3.1 + §3.5 step 0: `declare_imported(desc, PhysicalAllocHandle)` records an external borrow; the graph never co-owns. Write capability is opt-in per importer (`SPEC.md` §4.1.4 invariant 3). Swapchain `present` pass is the canonical write-imported (§3.7).                                     |
| **R-2.2.11** — Diagnostic overlay visualises the live graph + per-pass GPU timings.                 | **Refused at this aggregate; routed to diagnostic aggregate.** The graph layer exposes `ExecutionPlan::pass_count()` / `barrier_count()` / `structural_hash()` accessors (`SPEC.md` §5 already publishes these); the *overlay rendering* is `DiagnosticOverlay` (#774). The graph just produces the queryable artefact. |
| **R-2.2.12 / R-2.2.13** — Task-graph integration + per-pass-thread submit.                          | **Refused at this aggregate; routed to core+render concurrency.** The build half is single-threaded on the builder thread (§4 below); the recording half is per-queue, three-worker max (`SPEC.md` §6.3, §4 below). No task-graph engine — the topology is predetermined by `(queue, plan order)`.              |
| Harmonius design — `GraphBuilder.add_pass(name, queue, accesses, execute)` fluent API.             | **Covered.** `SPEC.md` §5 publishes `add_raster_pass` / `add_compute_pass` / `add_rt_pass` with this exact shape. Three flavours (instead of one polymorphic `add_pass`) are SRP: the RT flavour takes `TLASHandle` as a typed parameter; the others do not.                                                       |
| Harmonius design — `RenderGraph::compile()` returns an immutable `CompiledGraph`.                  | **Covered.** `SPEC.md` §5: `compile()` returns `Result<const ExecutionPlan*>`. Plan immutability after publish is `SPEC.md` §4.1.5 invariant; this design adds the structural-hash cache key.                                                                                                                  |
| Harmonius design — Graph diff between frames to skip recompiles.                                    | **Refused / collapsed.** Diff tracking is replaced by structural-hash equality (§3.8): a stable `u64` from the pass set + capabilities + view + settings; equal hash = cache hit, no diff walk. Two passes' "structurally identical" check via the hash is `O(1)` per frame; the diff-walk would be `O(passes)`. |
| Harmonius design — Resource virtualisation with explicit lifetime ranges in the graph.            | **Covered.** §3.5: lifetime is computed at compile time from `(first declared write, last declared read)` over the topological order; authors do not name it manually. Fewer fields to keep correct → fewer ways to be wrong.                                                                                  |
| Harmonius design — Plan persisted to disk between sessions.                                        | **Refused, hard.** PHILOSOPHY anti-pattern "serialised render-graph files". Plan lives in memory only; the structural-hash cache survives one process. No `.fory` schema. `SPEC.md` §5 already publishes "Serialised schemas (Fory) — None at this layer."                                                       |
| Harmonius design — Mid-frame graph rebuilding (push a pass, recompile, continue).                  | **Refused.** Phase 7 graph build is one-shot per `View` per frame (`SPEC.md` §4.1.2 invariant 4). Editor "live edit a pass" goes through the hot-reload path (§8 below); no mid-frame mutation of an in-flight graph.                                                                                          |
| Harmonius design — Backend-agnostic graph (Metal / Vulkan / D3D12).                                 | **Refused at this aggregate; deferred.** MVP is Metal 4 only (`SPEC.md` §3.2 collapse #1). The graph layer is *backend-neutral by construction* (`SPEC.md` §6.6) — `Pass::execute` takes `MetalCommandBuffer&` as the only seam, but the graph algorithms operate on declared accesses, not on Metal types. A future Vulkan plugin reuses the algorithms verbatim with its own command-buffer wrapper. |
| Harmonius design — Pass groups / sub-graphs for compositional authoring.                            | **Refused for MVP.** Pass set is flat per `View`; groups would be a nice-to-have for editor visualisation but the eleven MVP passes (`SPEC.md` §6.2.2 step 1) do not warrant it. Listed in §12 below.                                                                                                            |
| Harmonius design — Resource transient pool sized by max-residency profiling.                        | **Covered, deferred body.** The pool size cap (256 MiB, `SPEC.md` §9.5 transient row) is fixed at init from the perf budget; alias-planner colouring drives the *recovery rate* (≥40% on S1, story #382). Profiling-driven re-sizing is a post-MVP perf-budget amendment, not graph-layer work.                  |

Net result: every R-2.2.* requirement and every render-graph-relevant
design clause is either implemented as specified below or explicitly
refused with rationale. The "many specialised graph runtimes" of
harmonius collapse into one declarative C++ DAG with one Metal 4
backend, per `SPEC.md` §3.2 collapse #1.

## 3. Detailed model

### 3.1 The aggregate cluster

```
RenderGraph (per (View, FrameCounter) pair)
├── nodes_      : eastl::vector<Pass>            (in declaration order)
├── edges_      : eastl::vector<Edge>            (read-after-write, derived)
├── resources_  : VirtualResourceTable           (Transient/Persistent/Imported)
├── view_       : ViewHandle                     (back-pointer to RenderFrame view)
├── capabilities_ : CapabilitySet                (snapshot at begin())
└── compiled_   : const ExecutionPlan*           (cache pointer; nullable)

GraphBuilder (fluent, single-thread, lifetime ⊆ RenderGraph::begin / compile)
├── graph_      : RenderGraph*                   (non-owning)
└── live_resources_ : eastl::vector<...>         (for declared = used check)

ExecutionPlan (cached value object, key = structural_hash)
├── ordered_passes_ : eastl::vector<PassRef>     (post-topo, queue-grouped)
├── barriers_       : eastl::vector<Barrier>     (split-aware, minimum)
├── alias_plan_     : AliasPlan                  (VR → PhysicalAlloc slot)
├── queue_assignment_ : eastl::vector<Queue>     (per pass)
├── bindings_       : ArgumentBufferTables       (per-frame/pass/material/draw)
└── structural_hash_ : std::uint64_t             (cache key)
```

`RenderGraph` and `GraphBuilder` are sibling aggregates inside this
detailed design; `SPEC.md` §4.1.2 names them as one aggregate
(`RenderGraph`) because their invariants are inseparable: the builder
is a write-handle to the graph, valid only between `begin()` and
`compile()`. The split above is purely an internal-architecture
decomposition, not an ABI seam — both expose their public surface
through `SPEC.md` §5.

### 3.2 Per-`View` instantiation (multi-view fan-out)

Phase 7 entry runs once per displayed frame. Inside, the builder
thread iterates over `RenderFrame::views()` and instantiates one
`RenderGraph` per `View`. Sequence (`SPEC.md` §6.2.2 step 1):

1. `RenderGraph::create(MetalDevice&, ViewHandle)` allocates the
   graph object from render's per-frame arena. One graph per
   `(View, FrameCounter)` pair.
2. `RenderGraph::begin(const RenderFrame&)` snapshots the live
   `CapabilitySet` and returns a `GraphBuilder`. The snapshot is
   captured at begin so a hot-reload at frame N+1 cannot perturb
   frame N's compile.
3. `passes/*.cpp` register their `Pass`es by invoking
   `GraphBuilder::add_*_pass(...)` in the fixed MVP order
   (`SPEC.md` §6.2.2 step 1.1 .. 1.11). The order is hard-coded
   in `graph/builder.cpp`; passes are not data-driven (PHILOSOPHY
   §6: codegen-everywhere; render's MVP ships eleven passes, not
   a runtime DAG-author surface).
4. `RenderGraph::compile(CapabilitySet)` returns the
   `ExecutionPlan*` (cache hit or fresh compile).
5. `ExecutionPlan::record_into(MetalCommandBuffer&)` is invoked
   per per-queue worker (§4 below) to record commands.
6. The graph object is destroyed at phase-7 exit
   (`SPEC.md` §6.2.2 step 4). The plan stays in the cache.

Multi-view fan-out (split-screen, VR stereo, shadow-cascade views,
reflection probes, viewmodel) reuses this loop unchanged
(`SPEC.md` §3.2 collapse #7). The compile cache amortises across
views: when split-screen builds two graphs whose pass sets +
capabilities + render-settings hash equal, the second view's
`compile()` is a hash lookup + binding rebind (§3.8).

### 3.3 Capability gating (build-time pass elision)

Each `Pass` carries a `Capability requires_caps` predicate. During
`add_*_pass`, the builder evaluates:

```cpp
if (!capabilities_.supports(desc.requires_caps)) {
    // Pass is dropped from the node list silently.
    // Logged at debug level for the diagnostic overlay.
    return Result<void>{};
}
```

Effects:

1. The dropped pass never enters `nodes_`; topo sort, alias plan,
   barrier emit do not see it.
2. Downstream passes whose access set referenced a `VirtualResource`
   only the dropped pass produced fail with
   `render::Error::PassUndeclaredAccess` (`SPEC.md` §10.1 row
   `GraphResourceUnknown`) at the consumer's `add_*_pass` call.
   This is intentional: the capability fallback is the responsibility
   of the consumer's design (e.g. `RayQuery → GTAO/PCSS` per
   `SPEC.md` §10.3 row `MeshShaderCapabilityMissing`); silently
   dropping the consumer would mask the fallback obligation.
3. The capability snapshot is taken at `begin()`; subsequent flips
   in the same frame have no effect. Capability-fallback recovery
   (`SPEC.md` §10.2 `disable-feature`) flips a bit and the *next*
   frame's `begin()` observes the new set.

Story #381 covers this; the test fixture in §11 below names
`graph_build/capability_gated_elision`.

### 3.4 The compile pipeline

`RenderGraph::compile()` runs five steps in order. The pipeline is
total: success returns `const ExecutionPlan*`; failure returns
exactly one `render::Error` per `SPEC.md` §10.1
(`RenderGraphCycle`, `PassUnsupportedConfig`, `PassDeclaredUseUnused`,
`PassUndeclaredAccess`, `BarrierConflict`,
`ResourceResidencyExceeded`). No partial plan is observable.

#### Step 1 — Topological sort (`graph/compile.cpp`)

Inputs: `nodes_` (post-capability-gating); `edges_` derived from
declared access pairs (writer → reader) under the read-after-write
rule (and write-after-write, broken by declaration order +
deterministic FQN tiebreak as in `specs/core/schedule-frame-design.md`
§3.2 step 4). The edges are *derived*, not author-supplied; the
declared accesses are the only input the author writes.

Algorithm: Kahn's topological sort. A cycle returns
`render::Error::RenderGraphCycle`; `SPEC.md` §10 maps this to the
`abort-engine` recovery (a graph cycle is structurally invalid; no
fallback rescues a malformed graph, `SPEC.md` §10.3 `GraphCycle`
row).

Deterministic ordering: ties broken by declaration order (the order
passes were registered with the builder). Declaration order is
already deterministic — the MVP pass list is hard-coded
(`SPEC.md` §6.2.2 step 1) — so the tiebreak is byte-stable.

#### Step 2 — Capability + access-set validation

For every `Pass`:

1. Reject if `desc.requires_caps` evaluates false against the live
   `CapabilitySet` and the pass *did* enter the node list (only
   reachable when the capability flipped between `add_pass` and
   `compile`; protective check) → `PassUnsupportedConfig`.
2. Reject if the pass declared a read or write to a
   `VirtualResourceHandle` that does not exist in `resources_` →
   `PassUndeclaredAccess` (`SPEC.md` §10.1 row
   `GraphResourceUnknown`).
3. Reject if a pass declared `Queue::Compute` and used a graphics-
   only access (e.g. `ColorAttachment` write on a Compute pass) →
   `PassUnsupportedConfig`. Symmetric for `Queue::Graphics` +
   storage-buffer write through a non-encoder API.
4. Reject if two writes to the same `VirtualResource` come from two
   passes whose ordering edge is undecidable (e.g. neither is in
   the other's transitive closure) → `BarrierConflict`. This is the
   "WAW with no ordering" case; the author must add an explicit
   `after`/`before`-style edge by routing through an explicit
   read-after-write declaration.

Stories #381 / #383 / #385 cover this layer.

#### Step 3 — Alias plan (`resources/alias_planner.cpp`, see §3.5)

Computes `(first_write, last_read)` lifetimes per
`Transient` `VirtualResource`, builds the interference graph,
colours it (greedy by descending size), maps each colour to a
`TransientPool` slot. Returns the `AliasPlan` value object.
Failure (transient pool exhausted) →
`render::Error::TransientPoolExhausted` (`SPEC.md` §5
`render::Error` enum). The §10 row maps this to
`ResourceResidencyExceeded → lower-tier` recovery.

#### Step 4 — Barrier emission

For every edge `A → B` in the topologically-sorted plan, the
emitter computes the minimum split-aware Metal 4 barrier set:

1. **Memory scope:** the union of the resource access flags
   inferred from `A`'s declared writes and `B`'s declared reads
   (e.g. `MTLBarrierScopeRenderTargets` for an MRT write read by
   a sampled-texture consumer; `MTLBarrierScopeBuffers` for a
   compute storage-buffer write read by an indirect-draw consumer).
2. **Execution stage:** computed from the queue + encoder type at
   each end. Within one queue and contiguous encoder, an
   `MTLBarrierScope` suffices; across encoders within a queue, an
   end-encoder + begin-encoder pair carries the dependency
   implicitly and the emitter records only the memory half.
3. **Cross-queue:** edges spanning Graphics ↔ Compute or
   Compute ↔ Copy emit a `MTLSharedEvent` signal+wait pair.
   `metal/fence.cpp` allocates the event from a per-frame pool;
   `ExecutionPlan` owns the event indices.
4. **Split-aware:** when the same producer feeds multiple consumers
   that themselves are unordered (parallel reads), the emitter
   coalesces the barriers into one signal + one wait fan-out
   rather than N pairs (Metal 4's `encodeWaitForEvent:` is fan-out
   safe). This is what makes the count "minimum" in story #383.
5. **Redundant-barrier rejection:** when a barrier the emitter
   plans to insert is already implied by an earlier barrier in the
   same queue (same scope, same producer/consumer), the emitter
   omits it and records the omission count for the
   `barrier-count` golden test (`SPEC.md` §4.1.5 invariant 1).

Failure (barrier semantics genuinely unsatisfiable, e.g. a
write-after-write across queues with no mediating fence opportunity)
→ `render::Error::BarrierConflict`. `SPEC.md` §10 maps this to
`abort-engine`; the test fixture is
`tests/render/failure/barrier_violation.cpp` (`SPEC.md` §10.3 row).

#### Step 5 — Queue assignment + binding tables

1. Read `Pass::queue` from each pass's `PassDesc.queue`. The
   compiler does *not* re-assign queues; the author chose. Group
   passes into three queue buckets (Graphics / Compute / Copy)
   while preserving topological order within each bucket.
2. Build the four argument-buffer frequency tables (per-frame,
   per-pass, per-material, per-draw) by walking each pass's
   bindings and emitting offsets into the per-frame argument-
   buffer ring. This step lives in
   `resources/argument_buffer.cpp` (`SPEC.md` §3.2 collapse #8);
   the graph layer just calls into it.
3. Compute `structural_hash_` (§3.8) and either insert the
   `ExecutionPlan` into the cache (cache miss) or rebind only
   (cache hit, in which case steps 1–4 above were skipped).

#### Compile complexity

| Step | Complexity              | Notes                                                                  |
|------|-------------------------|------------------------------------------------------------------------|
| 1    | `O(P + E)`              | P = pass count (≤16 MVP); E = derived edges (≤4P).                     |
| 2    | `O(P · A)`              | A = avg access-set size per pass (≤8 MVP).                              |
| 3    | `O(R²)` worst-case      | R = transient virtual resource count (≤32 MVP); colouring is greedy.    |
| 4    | `O(E)`                  | One barrier compute per edge.                                          |
| 5    | `O(P · A)`              | Binding-table linear walk; structural hash in `O(P)`.                  |

The MVP working set (P ≤ 16, R ≤ 32) puts the worst-case compile
under 50 µs on M1 firestorm; cache-hit path is one `u64` lookup
plus a binding-table rebind (`O(P · A)`). §9 below pegs the budget.

### 3.5 Resource model + alias planner

Three resource roles compose with `VirtualResource` per
`SPEC.md` §4.1.4. The graph builder admits each through a distinct
`declare_*` call so the role is type-checked at the call site:

```cpp
auto vr_gbuffer  = builder.declare_transient(gbuffer_desc);   // pool-aliased
auto vr_history  = builder.declare_persistent(history_desc);  // long-lived
auto vr_drawable = builder.declare_imported(drawable_desc, swapchain_alloc); // borrow
```

#### Step 0 — Resource role partition

- **Transient.** Eligible for aliasing inside this frame. Lifetime
  starts at the first declared write and ends at the last declared
  read, both bounded by the topological order.
- **Persistent.** Outlives the frame. Skipped by the alias planner;
  storage owned by `resources/persistent.cpp` (HZB pyramid, history
  color, ring-buffered constant heaps, shadow atlas storage,
  cluster-cull scratch). `SPEC.md` §4.1.4 invariant.
- **Imported.** Borrowed; not eligible for aliasing. Lifetime
  enforced by the importer (swapchain drawable, geometry-vended
  BLAS, vfx-vended particle buffers).

The alias planner operates only on transient virtual resources.

#### Lifetime computation

For every transient `VirtualResource` `r`:

```
first_write(r) = min { i | nodes_[i].writes contains r }
last_read(r)   = max { i | nodes_[i].reads  contains r,
                              or i = first_write(r) if no reads }
lifetime(r)    = [first_write(r), last_read(r)]
```

Indices `i` are the post-topological positions. Two transient
resources `r₁`, `r₂` *interfere* iff `lifetime(r₁) ∩ lifetime(r₂) ≠ ∅`
*and* their physical descriptors are compatible for placement
(matching format class + matching extent class — Metal 4 placement
heaps require this for `MTLHeap` aliasing).

#### Interference graph + colouring

Interference forms an undirected graph. The planner runs a greedy
colouring sorted by descending allocation size (largest first), so
big targets dock onto distinct slots and smaller siblings tuck in.
The colouring is deterministic (size tiebreak by debug-name
lexicographic order; PHILOSOPHY §7).

Each colour maps to one `PhysicalAllocation` slot drawn from the
`TransientPool`. The planner asserts the running total against the
256 MiB transient row from `SPEC.md` §9.5; overflow returns
`render::Error::TransientPoolExhausted` (rolled into `SPEC.md` §10
`ResourceResidencyExceeded`, `lower-tier` recovery).

#### Recovery target

Story #382 names a ≥40% recovery rate against the naive sum-of-
sizes baseline on the S1 fixture. The benchmark that asserts this
is `tests/render/perf/transient_alias_recovery.cpp`
(§11 below). Recovery below 40% on S1 trips the `perf:headroom-low`
tripwire (`SPEC.md` §9.6.3) — a sign the alias model is regressing,
not a hard fail.

#### Integration with persistent + imported

The graph compiler walks all three resource roles when emitting
barriers (transient ↔ persistent reads/writes are honoured) but
only transients participate in the alias plan. Imported resources
borrowed read-only require no fence (the importer guarantees);
imported writable resources (the swapchain drawable inside the
`present` pass) emit a normal barrier from the producer to the
present-encoder boundary.

### 3.6 Pass invariants and `declared = used` enforcement

`SPEC.md` §4.1.3 invariants 1–4 govern every `Pass`. The graph
layer enforces them at three points:

1. **Compile time:** `add_*_pass` records the declared access set
   into `Pass::reads_` / `writes_`. The compiler validates each
   element against `resources_` (§3.4 step 2); undeclared resource
   handles fail.
2. **Recording time (debug builds):** the `MetalCommandBuffer`
   wrapper carries an instrumentation hook that observes every
   resource bind and asserts membership in the active pass's
   declared set. A violation logs `render::Error::PassUndeclaredAccess`
   and aborts the recording in debug builds; shipping builds elide
   the check (zero hot-path cost, `SPEC.md` §4.1.3 invariant 1).
3. **Recording time (all builds):** the queue-purity check —
   `MetalCommandBuffer::queue_role()` must equal the pass's
   `PassDesc.queue`. Wrong queue → recording fails;
   `render::Error::PassUnsupportedConfig`. This catches "compute
   pass tried to open a render encoder" by the wrapper itself
   (`SPEC.md` §4.1.6 invariant 3 + §4.1.3 invariant 2).

The `declared = used` enforcement is the contract that makes alias
planning sound: if the pass body touches a resource it did not
declare, the planner may have aliased it to memory the body
mutates, and a later pass reading the original VR sees garbage.
Catching this in debug-build CI is the first line of defence.

### 3.7 Special pass shapes

Three pass shapes deserve named treatment because their invariants
are load-bearing and tested against named acceptance stories.

#### Mesh-shader gbuffer (story #386)

`SPEC.md` §4.2 invariant 5: the mesh-shader dispatch writes
gbuffer MRT (4 attachments) + visibilityID + velocity + depth in
**one** declared `Pass` with one barrier-emit point. The graph
layer enforces this by:

1. Refusing any `add_raster_pass` whose declared writes include
   a strict subset of the gbuffer MRT set (without all four
   attachments + visID + velocity + depth, the pass cannot be
   the gbuffer pass; the writer chose the wrong pass shape).
   Concretely: a constexpr `kGBufferWriteSet` in
   `passes/gbuffer.cpp` is compared via `eastl::ranges` for
   superset membership at compile time of the pass declaration.
2. Routing the barrier emitter (§3.4 step 4) so a single
   producer→consumer edge from gbuffer to its consumers
   (HZB-build, deferred-lighting, RT shadow, post) coalesces into
   one barrier on the graphics queue + one cross-queue fence
   into the compute queue's lighting consumer.

#### TLAS-build follows BLAS-refit (story #391)

`SPEC.md` §4.2 invariant 4: BLAS refit precedes TLAS build, every
frame. The graph layer enforces by:

1. The TLAS-build pass declares an explicit read-after-write on
   the BLAS-refit pass's output `VirtualResource` (the refit
   scratch buffer); the §3.4 step-1 topo sort orders BLAS-refit
   before TLAS-build.
2. Both passes are on `Queue::Compute`, so the barrier emitter
   yields a same-queue execution barrier (no fence needed).
3. RT consumers (`shadow_rt`, `ao_rt`, `lighting`'s ray query)
   declare a read on the TLAS handle vended by the build pass;
   the topo order places them after TLAS-build. RT-consumer ↔
   gbuffer cross-queue fences (Compute ↔ Graphics) are handled by
   the emitter naturally (§3.4 step 4 case 3).

#### Present pass writes the swapchain imported resource (story #396)

The `present` pass declares a write to the `imported` swapchain
drawable resource. `SPEC.md` §4.1.4 invariant 3 requires that the
importer publishes write capability for this case; the swapchain
importer (`platform/swapchain.cpp` peer; not this aggregate) does
so. The barrier emitter places a render-target → drawable
transition barrier at the pass entry; the post-pass fence is the
`PresentFence` consumed by phase 9.

The present pass is the only pass whose `execute()` lambda may
acquire the next drawable (`CAMetalLayer::nextDrawable`); the
acquire-failure path is `render::Error::SwapchainAcquireFailed`
(`SPEC.md` §10.3 row). The graph layer is unaware of the acquire
mechanics — they live in the pass body.

### 3.8 Structural hash + plan cache

`structural_hash_(plan) =
    blake3_64(
        for each pass in plan order:
            (pass.name_hash, pass.queue, pass.requires_caps,
             pass.priority, hash(pass.reads), hash(pass.writes)),
        capabilities_.flags,
        view_handle.raw(),
        render_settings_struct_hash
    )`

Properties:

1. Two graphs with the same pass set in the same order on the
   same view with the same capability and settings hash to the
   same `u64`. This is the cache-hit signal.
2. The hash includes neither pass-body identity (the lambda
   capture) nor any ECS state. A rebuild triggered by a different
   `RenderFrame` *content* (e.g. different visible-set size)
   does not invalidate the plan — the binding-table refill is
   keyed off `RenderFrame` deltas separately. This is what makes
   the cache stable frame-to-frame.
3. The hash includes `render_settings_struct_hash` (a `u64`
   computed over the serialised `RenderSettings` struct snapshot)
   so an `aa_mode` change or a `shadow_tier` flip invalidates
   the plan even though the pass-set may look textually identical
   at the topology level (the graph-builder's pass-set may differ
   because the variant-selecting predicates fired differently;
   the hash captures that automatically).

Cache eviction: LRU keyed on last-frame-touched. Cap: 16 plans
(plenty for one main + four shadow cascades + one reflection
probe + viewmodel = 7 active views, doubled for hot-spare).
Eviction is a cold-path event; plans rarely turn over in MVP.

The cache lives inside `RenderGraph`'s owning module
(`graph/execution_plan.cpp`), one per render plugin instance.
The cache is *not* persisted (PHILOSOPHY anti-pattern, §1 / §7);
process restart rebuilds.

### 3.9 Aggregate composition

```
RenderGraph  (per (View, FrameCounter))
├── GraphBuilder            ← write-handle, lifetime ⊆ begin/compile
├── Pass[]                  ← node list, post-capability-gating
├── VirtualResourceTable    ← Transient/Persistent/Imported entries
├── ExecutionPlanCache&     ← shared across views (1:N: graphs:cache)
└── compile() entry         ← runs §3.4 steps 1..5

ExecutionPlan  (cached value, 1:N: cache:graphs)
├── ordered_passes: PassRef[]
├── barriers:       Barrier[]    ← split-aware Metal 4 emit
├── alias_plan:     AliasPlan     ← VR → physical slot
├── queue_groups:   3 buckets     ← Graphics/Compute/Copy
└── bindings:       4 freq-groups ← per-frame/pass/material/draw
```

The graph layer's interface to the rest of the render plugin is:

- **Inputs:** `RenderFrame&` (read), `MetalDevice&` (queues +
  caps + transient pool), `RenderSettings`-derived predicates,
  `PSOHandle`s vended by the cache, `BLASHandle` /
  `TLASHandle` from `RTAccelStructures`, `HZBHandle` from cull,
  `ClusterCullStateHandle` from cluster-cull state.
- **Outputs:** `const ExecutionPlan*` to per-pass workers.
  Workers consume it read-only via `record_into(MetalCommandBuffer&)`.

Nothing else crosses the boundary. The lambda capture of
`Pass::execute` is the only way pass-body code reaches the graph;
the lambda is opaque to the compiler and never inspected.

## 4. Public surface

The public surface is defined in `specs/render/SPEC.md` §5 and is the
canonical, authoritative source of record for all type names,
signatures, and declaration order. This design document adds no new
types or signatures beyond §5; any divergence between this document
and `SPEC.md` §5 must be resolved in favour of the SPEC. Readers
requiring the full declaration set should consult
`specs/render/SPEC.md` §5 directly (search for `class GraphBuilder`,
`class RenderGraph`, `class ExecutionPlan`, `struct PassDesc`,
`PassExecuteFn`, `struct ResourceAccess`).

Surface invariants this design imposes on top of the §5 stub:

1. **No exceptions cross the boundary.** Every method is `noexcept`;
   every fallible method returns `glibre::Result<T>` per
   `error-model.md`. `PassExecuteFn` lambdas are also `noexcept` by
   convention; failure is signalled through the `Result<void>` return.
2. **`PassDesc::name` is a stable string view.** Lifetime is the
   plugin's `.rodata` for compile-time strings; the graph stores it
   by reference for diagnostic / structural-hash purposes (the hash
   uses a 64-bit deterministic hash of the bytes, not the pointer).
3. **`PassExecuteFn` is `eastl::function`.** The use of EASTL's
   `function` (not `std::function`) follows PHILOSOPHY §11; it
   allows configurable allocator-by-value and the inline-storage
   sweet spot for typical small captures (<= 32 bytes covers the
   MVP pass bodies). Captures larger than the inline buffer fall
   back to the per-frame arena allocator (no heap).
4. **`GraphBuilder` is non-copyable, non-movable.** It is a
   write-handle vended by `RenderGraph::begin()`; storing it
   beyond the `compile()` call is undefined behavior. `SPEC.md` §5
   already publishes the deleted ctor/op=.
5. **`ExecutionPlan*` returned by `compile()` is non-owning.** The
   pointer addresses storage inside the plan cache; it is valid
   until the next `compile()` call on the same `RenderGraph`
   evicts it (LRU). Callers do not `delete` it.
6. **Multi-thread access to one `RenderGraph` is undefined.**
   `GraphBuilder`, the node list, the resource table, and
   `compile()` are all single-thread by contract (the builder
   thread; §6 below). `record_into` on the resulting plan *is*
   thread-safe-for-read across worker threads (immutable post-
   compile).

Error arms emitted directly by this aggregate's public surface
(per `SPEC.md` §10.1):

- `render::Error::RenderGraphCycle` — `compile()` step 1 (topo).
- `render::Error::PassUnsupportedConfig` — `compile()` step 2
  (queue/access mismatch); also `add_*_pass` for malformed input.
- `render::Error::PassDeclaredUseUnused` — debug-build assertion
  (a declared resource was never read or written).
- `render::Error::PassUndeclaredAccess` — `compile()` step 2
  + record-time wrapper hook; also `add_*_pass` for an unknown
  `VirtualResourceHandle`.
- `render::Error::BarrierConflict` — `compile()` step 4.
- `render::Error::TransientPoolExhausted` — `compile()` step 3
  (alias planner). Rolled up into `ResourceResidencyExceeded`
  for §10's recovery ladder.
- `render::Error::CapabilityNotSupported` — `add_rt_pass` when
  `Capability::HardwareRayTrace` is not in the live `CapabilitySet`
  (the only `add_*_pass` that hard-rejects rather than silently
  eliding; RT is the only MVP pass shape with no fallback inside
  the same flavour).

## 5. Hot/cold path split

### 5.1 Cold path — graph build + compile (per frame, per `View`)

The build half of phase 7 (`SPEC.md` §6.2.2 step 1 + step 2) is
**cold relative to per-pass workers** but **per-frame**. Per
`SPEC.md` §9.3 the driver-thread budget is:

| Step                                                    | Cap        |
|---------------------------------------------------------|------------|
| `graph/builder.cpp` — register per-`View` passes        | 0.10 ms    |
| `graph/compile.cpp` — topo / colour / barrier / queue / bind | 0.20 ms |

The 0.20 ms budget covers both cache-miss (full §3.4 steps 1–5) and
cache-hit (hash + rebind) paths. Cache-hit dominates frame-to-frame
once the ExecutionPlan stabilises (typically after the second frame
of a stable scene).

Operations on this path:

1. **Per-pass `add_*_pass` calls.** Eleven calls per `View` in MVP
   (`SPEC.md` §6.2.2 step 1), each `O(A)` access-set size. Total
   ~88 access entries, ~few µs.
2. **`compile()` cache lookup.** Compute structural hash; one
   `eastl::hash_map` lookup. Cache hit returns
   `const ExecutionPlan*` immediately; rebinding the per-pass
   binding tables walks each pass's access set linearly (~few
   µs total).
3. **Cache miss compile.** §3.4 steps 1–5; ~50 µs on M1 firestorm
   for the MVP working set. Happens on first frame, on capability
   change, on `RenderSettings` change.
4. **Plan publish.** Pointer write into the cache + LRU update.
   ~handful of cycles.

### 5.2 Hot path — per-pass `record_into` (per pass, per worker)

The recording half is **hot per-pass per-worker**. Three workers
record concurrently into per-queue command buffers (§6 below).
Per-pass cost is dominated by the pass body, not by the graph
layer; the graph contributes only:

1. Per-pass barrier emit (a few `MTL::CommandBuffer::encodeBarrier*`
   calls per edge; pre-resolved to constants by `compile()`).
2. Argument-buffer rebind at pass entry (the four frequency tables
   are pre-resolved offsets; binding is `O(A)` API calls).
3. Per-pass GPU timestamp insert at encoder begin / end
   (`SPEC.md` §9.6.1; debug build only feeds the diag overlay,
   shipping always feeds the perf gate when capability supports).

The graph layer adds **zero allocations** on this path — every
slice the lambda needs is pre-resolved into the `Bindings` struct
at compile time. This is what makes pass bodies allocation-free
(`SPEC.md` §4.1.3 invariant 4).

### 5.3 Why the split matters

Cold-path drift trips the `phase-7 render-submit driver, S1, p99`
benchmark (`SPEC.md` §9.6.2); hot-path drift trips per-pass GPU
ceilings (§9.6.1) and ultimately the 8.0 ms GPU total. The split
is deliberately not data-driven: the pass set is hard-coded, the
compile cache is in-memory only, the structural hash is a single
`u64` lookup per frame. The only `std::atomic` on the hot path is
the barrier-count instrumentation counter (debug builds), which
relaxes-stores once per pass; shipping builds elide it.

## 6. Concurrency

### 6.1 Builder thread (one)

Per `SPEC.md` §6.3, the graph build + compile runs on a single
**builder thread** dedicated to render. It owns:

- The per-frame `RenderGraph` arena.
- `graph/builder.cpp`, `graph/compile.cpp`,
  `resources/alias_planner.cpp`, the `ExecutionPlan` cache, the
  argument-buffer frequency-group binder.

Multi-view fan-out (split-screen, VR, shadow cascades) is
sequential on the builder thread; compile cost is dominated by
hash + cache lookup, not by topo sort, so sequential is the right
default. Single-thread also makes the structural hash deterministic
across runs (the iteration order of the resource table is fixed by
declaration order, not by a thread-race-influenced map probe).

### 6.2 Per-pass GPU encoding workers (≤3)

Once `ExecutionPlan` exists, per-pass `execute()` lambdas may be
recorded in parallel into per-queue `MetalCommandBuffer`s. The
plan's queue assignment partitions passes into independent record
streams:

- One Graphics worker records the Graphics-queue passes in plan
  order.
- One Compute worker records the Compute-queue passes in plan
  order.
- One Copy worker records the Copy-queue passes (mostly idle in
  MVP; the copy queue exists for swapchain blits + future
  streaming).

Worker count is bounded by the queue count (three in MVP); no
further scaling — Metal command-buffer recording is not the
bottleneck (`SPEC.md` §6.3). Workers communicate exclusively via
the read-only plan and per-queue command-buffer handles; no
cross-thread mutex acquisitions inside phase 7 once the plan is
built.

The graph layer is the **only writer** to `RenderGraph` and the
plan cache. Workers read the plan as `const`. The compile cache is
mutated only on the builder thread between `compile()` calls;
workers never touch it.

### 6.3 Driver thread

The driver thread (`core`'s frame-loop driver) initiates phase 7
on the render side, hands `RenderFrame&` to the builder thread,
and submits the recorded command buffers to `MetalQueue`s in plan
queue order. It then signals `PresentFence` and returns.

Frame N's GPU execution overlaps frame N+1's simulation per
`frame-phases.md` "one-frame pipeline" (`SPEC.md` §6.3 last
paragraph). The graph layer participates by ensuring the plan is
immutable post-publish — frame N+1's builder may read a previous
plan from the cache while frame N's plan is still in flight.

### 6.4 Determinism

Determinism requirements (PHILOSOPHY §7) for this aggregate:

1. **Structural hash is deterministic.** Same pass set + capabilities
   + view + settings hash to the same `u64` byte-equal across hosts.
   Two hosts running the same fixture compile to byte-equal plans.
2. **Topological tiebreak is deterministic.** Tied edges break by
   declaration order; declaration order is the hard-coded MVP pass
   list (`SPEC.md` §6.2.2 step 1). Two runs produce identical
   ordered pass arrays.
3. **Alias-planner colouring is deterministic.** Greedy-by-size
   with lexicographic name tiebreak; same input → same colour
   assignment (§3.5).
4. **No clock or random reads.** The compile pipeline does not
   call any wall clock or PRNG; all inputs are functions of the
   declared graph.

Replay determinism is therefore a property of the compile pipeline:
re-running phase 7 with byte-equal inputs produces a byte-equal
plan.

## 7. Persistence + ABI

The render-graph aggregate persists **nothing**. Per
`SPEC.md` §5 "Serialised schemas (Fory) — None at this layer" and
PHILOSOPHY anti-pattern "Serialised render-graph files":

- `RenderGraph` lives in render's per-frame arena; destroyed at
  phase-7 exit.
- `ExecutionPlan` lives in an in-memory LRU cache keyed by
  structural hash; the cache does not survive a process restart.
- `Pass` is an in-memory node; the lambda is a C++ closure with no
  on-disk shape.
- `VirtualResource` / `PhysicalAllocation` / `Barrier` /
  `AliasPlan` / argument-buffer-table offsets are all in-memory
  only.

There is no Fory schema for any graph type; the codegen pipeline
does not iterate this aggregate. The `glibre-types.dylib` middleman
hash is unaffected by graph-layer changes.

ABI consequences:

- The §5 public surface is the entire ABI seam. The eight error
  enumerators this aggregate emits are part of `render::Error`'s
  closed sum (`SPEC.md` §10.1); adding a ninth is a render-plugin
  ABI bump (`error-model.md` §"Composition Rules" item 5).
- `PassExecuteFn`'s use of `eastl::function` does **not** cross the
  plugin ABI boundary: the lambda is stored inside the render
  plugin, registered by render-plugin pass-body code, and consumed
  by render-plugin compile/record code. Plugin authors outside
  render do not register graph passes — they would route through
  the render plugin's higher-level seams (e.g. `vfx` publishes a
  GPU-resident buffer, render owns the pass that draws it; ticket
  #768 follow-up).
- The `ExecutionPlan*` return from `compile()` is a forward-
  declared opaque pointer (`SPEC.md` §5); its layout is render-
  internal. Callers — only `core` and render-plugin internals —
  use it via the published methods.

The graph aggregate consumes no Fory types; therefore no migration
bodies are required (`fory-codegen.md` §"Migration Mechanic" is
not exercised here).

## 8. Hot-reload

Hot-reload of the render plugin follows the engine-wide protocol
(`hot-reload-protocol.md` drain → swap → migrate → resume). The
graph aggregate's contribution is fully covered by `SPEC.md`
§8 "Hot-Reload Contract"; this section restates the graph-specific
points and binds them to the §3.4 / §3.8 algorithms.

### 8.1 What the graph aggregate drops on swap

Everything. The graph aggregate is exclusively transient state per
§7; nothing survives the plugin reload directly:

- The `RenderGraph` instance for the in-flight frame is destroyed
  before phase 8 begins (per `SPEC.md` §8.1: reload happens at
  phase 8, after phase 7's per-frame graphs have been retired).
- The `ExecutionPlan` cache is dropped wholesale. Plans are
  in-memory only and the new plugin instance gets a fresh cache.
- `Pass` lambdas live inside the outgoing dylib; they cannot
  survive the swap by definition (the code address would be
  invalid post-`dlclose`).
- `VirtualResource` table entries are dropped; persistent and
  imported VR storage is preserved by the `Resource` aggregate
  (`SPEC.md` §8.2 "Survival inventory") but not by this layer.

### 8.2 What survives, by reference

The graph layer references several survivors (`SPEC.md` §8.2):

| Survivor                          | Owner aggregate          | Graph-layer dependency                                                  |
|-----------------------------------|--------------------------|-------------------------------------------------------------------------|
| `PSOCache` entries (per `PSOKey`) | `PSOCache` (#766)        | Re-fetched via `PSOHandle` on the first frame post-resume.              |
| Persistent `Resource`s (HZB, history color, shadow atlas, ring buffers) | `Resource` (#772) | Re-declared via `declare_persistent` on the first frame post-resume; physical allocations are preserved. |
| BLAS / TLAS handles               | `RTAccelStructures` (#768) | Re-fetched via `BLASHandle` / `TLASHandle`; TLAS rebuilt on first post-resume frame anyway. |
| `MetalDevice` / queues            | `MetalDevice` (`SPEC.md` §4.1.6) | Re-acquired in `glibre_plugin_register`.                                |
| `RenderSettings` / `CapabilityMask` | data-Fory persistent  | Re-loaded by render-plugin init; graph reads via the `CapabilitySet` it gets handed. |

The new plugin instance's first-frame compile is a guaranteed
cache miss (the cache is empty); the second frame onward hits the
cache. The one-frame stall is permitted by `perf-budget.md`'s
hot-reload-frame budget (≤0.40 ms phase 8 + the next-frame compile
inside phase 7's 1.0 ms ceiling, with the 0.10 ms reserve absorbing
the cache-miss compile per `SPEC.md` §9.3).

### 8.3 `migrate(...)` for the graph aggregate

The graph aggregate has **no `migrate(...)` body**. It owns no
persistent data; nothing crosses the swap. Per
`hot-reload-protocol.md` §"State Survival Rules" — "if it has a
`.fory` schema, it survives; otherwise, it does not" — and the
graph types have no `.fory` schemas (§7), they do not appear in
the migration table.

The render plugin's `migrate(...)` body (defined in `SPEC.md`
§8.3) handles `RenderSettings` / `PSOCacheRecord` / `CapabilityMask`
migrations; none of those are graph-aggregate types.

### 8.4 Refusal cases

The graph aggregate contributes one refusal cause:

- **`render::Error::RenderGraphCycle`** raised at the first
  post-resume frame's `compile()`. Indicates the new plugin
  registered a pass-set that introduces a cycle. This rolls up to
  `core::Error::HotReloadRefused` per `SPEC.md` §8.4; the previous
  plugin stays live. Test fixture: `tests/render/failure/graph_cycle.cpp`
  (`SPEC.md` §10.3 row).

Other graph-related errors (`PassUnsupportedConfig`,
`BarrierConflict`, `TransientPoolExhausted`) at the first
post-resume frame trigger the §10 recovery ladder
(`abort-engine` for graph-shape errors; `lower-tier` for resource
exhaustion); they are not hot-reload-specific refusals and the
new plugin remains loaded.

### 8.5 Editor live-reload of a single pass body

Out of MVP scope; listed in `hot-reload-protocol.md` open question
#5 ("Editor-driven partial reload"). The MVP path is full-plugin
reload via the same dylib mechanism. The structural-hash key
(§3.8) ensures that a single-pass change at the source level
forces a cache miss on the next frame, so even partial reloads
recompile cleanly when the path lands.

## 9. Performance

This section refines `SPEC.md` §9.3 (phase-7 driver-thread CPU
budget) for the graph aggregate. Every number is a ceiling, not a
steady-state expectation; drift trips the per-PR `perf-budget.yml`
gate (`perf-budget.md` §"CI Gate Spec").

### 9.1 Build cost (per `View`, every frame)

| Step                                                     | Ceiling    | Cost model                                                                                                       |
|----------------------------------------------------------|------------|------------------------------------------------------------------------------------------------------------------|
| `add_*_pass` × 11 (MVP pass list)                        | 0.05 ms    | Eleven fluent calls; each is `O(A)` in access-set size (≤8 entries). Memory: per-frame arena, no heap.           |
| `RenderGraph::compile` cache lookup (cache hit)          | 0.02 ms    | Structural hash (`O(P · A)` ≈ 88 entries) + one `eastl::hash_map` probe + binding-table rebind.                  |
| `RenderGraph::compile` cache miss — full §3.4 pipeline   | 0.15 ms    | Topo sort (≤16P, ≤4P edges) + capability/access validation + alias plan (≤32R, R²) + barrier emit (≤4P) + queue + binding tables. |
| **Build subtotal — cache hit** (steady state)            | **0.07 ms**| Steady-state expectation after the second frame of a stable scene; well inside the §SPEC §9.3 0.20 ms compile budget. |
| **Build subtotal — cache miss** (one-shot)               | **0.20 ms**| Triggered on first frame, capability change, or settings change. Inside the 0.20 ms compile budget; phase-7 0.10 ms reserve absorbs the one-shot. |

Multi-view fan-out: `≤4` views in the MVP ceiling
(`SPEC.md` §9.3); the build cost is per-view but the cache hit
rate is high (split-screen views share pass sets), so the second
view onward is dominated by the lookup path.

### 9.2 Record cost (per pass, per worker)

| Operation                                   | Ceiling per pass | Cost model                                                            |
|---------------------------------------------|------------------|-----------------------------------------------------------------------|
| Barrier insert at pass entry                | <1 µs            | One or two `MTL::CommandBuffer::encodeBarrier*` calls per edge.       |
| Argument-buffer bind                        | ~1 µs            | Four frequency tables, pre-resolved offsets; ≤8 binds per pass.       |
| GPU timestamp insert (begin + end)          | <1 µs            | Two `MTLCounterSampleBuffer::sampleCounters` calls; gated by capability. |
| Pass body (lambda)                          | not graph-owned  | See per-pass design tickets (#762, #764, …).                            |

The graph layer's record-time contribution is bounded by ≤3 µs
per pass; over 11 passes that is ≤33 µs ≈ 0.03 ms — well inside
the per-pass record allowance under the 0.50 ms "per-pass record
(driver-side dispatch)" line of `SPEC.md` §9.3 (which budgets
the dispatch-and-wait work, not the recording itself).

### 9.3 Memory ceilings

The graph aggregate's storage budget is part of the 16 MiB
"GPU resource handles" row in `SPEC.md` §9.5 (it stores Handle<Tag>
values and the per-frame node + edge + access arrays). The arena
discipline:

| Storage                                   | Lifetime                          | Allocator                              |
|-------------------------------------------|-----------------------------------|----------------------------------------|
| `RenderGraph::nodes_` / `edges_` / `resources_` | Per-frame                  | `glibre::PerContextAllocator(render)`, transient arena slot, ≤256 KiB per `View`. |
| `ExecutionPlan` cache entries             | Process-lifetime, LRU-bounded     | Same allocator, persistent slot, ≤512 KiB total (16 plans × ~32 KiB each). |
| `Bindings` for each pass                  | Per-frame                         | Per-frame ring; resolved offsets, no heap. |

Strict-mode enforcement (`GLIBRE_ALLOC_STRICT=1`) catches drift
above these caps and returns `core::Error::OutOfBudget`, mapped to
`render::Error::ResourceResidencyExceeded` at the call site
(`SPEC.md` §9.5.1 rule 2).

### 9.4 Cited acceptance benchmarks

Stories #380, #381, #382, #383, #384, #385, #401, #402 from
`SPEC.md` §11 each carry a Catch2 fixture under `tests/render/`.
The performance-relevant benchmarks (named under
`tests/render/perf/`):

| Benchmark name                                      | Measures                                                | Ceiling   |
|-----------------------------------------------------|---------------------------------------------------------|-----------|
| `BENCHMARK("graph build, S1 main view, p99")`       | `add_*_pass × 11` + cache hit `compile()` wall time     | ≤ 0.07 ms |
| `BENCHMARK("graph compile cache miss, S1, p99")`    | First-frame `compile()` wall time (full §3.4 pipeline)  | ≤ 0.20 ms |
| `BENCHMARK("transient alias recovery, S1")`         | Recovery rate vs naive sum-of-sizes                     | ≥ 40 %    |
| `BENCHMARK("barrier count, S1, golden")`            | `ExecutionPlan::barrier_count()` against recorded value | byte-equal|
| `BENCHMARK("structural hash determinism, S1")`      | Same input → same `u64` across two runs                 | byte-equal|

The first two roll up into the `phase-7 render-submit driver, S1, p99`
benchmark (`SPEC.md` §9.6.2 row); the rest are graph-aggregate-owned
gates.

### 9.5 Cross-references

- `SPEC.md` §9.3 — phase-7 CPU breakdown (this aggregate's
  contribution is the 0.10 + 0.20 ms `builder` + `compile` rows).
- `SPEC.md` §9.5 — heap composition (transient pool 256 MiB +
  GPU resource handles 16 MiB).
- `SPEC.md` §9.6.1 — per-pass GPU timestamp queries.
- `perf-budget.md` §"Pipelined Frame Timing" — render's 1.40 ms
  submit slot; this aggregate's 0.30 ms builder+compile share.

## 10. Failure modes

The closed enumeration of `render::Error` arms emitted by this
aggregate (subset of `SPEC.md` §10.1 publication; the variants are
already in the §5 stub or queued as ABI-add rows per §10.1's
audit):

| `render::Error` arm              | §10.1 row             | Trigger                                                                                                                                   | Recovery        | Severity | Test fixture                                             |
|----------------------------------|-----------------------|-------------------------------------------------------------------------------------------------------------------------------------------|-----------------|----------|----------------------------------------------------------|
| `RenderGraphCycle`               | `GraphCycle`          | §3.4 step 1 detects a cycle in the topo sort.                                                                                              | `abort-engine`  | `error`  | `tests/render/failure/graph_cycle.cpp`                  |
| `PassUnsupportedConfig`          | (composite §10.3)     | §3.4 step 2: a pass declared an access incompatible with its queue, or a `Pass` predicate failed at compile time.                          | `lower-tier` (compile-time, settings change) / `abort-engine` (structural). | `error` / `warn` | `tests/render/failure/pass_unsupported_config.cpp` |
| `PassDeclaredUseUnused`          | (debug-build only)    | §3.6 invariant: a declared resource was never read or written. Asserted in debug; warned in shipping.                                     | none (debug abort) | `warn` | `tests/render/failure/declared_use_unused.cpp`        |
| `PassUndeclaredAccess`           | `GraphResourceUnknown`| §3.4 step 2 + record-time wrapper hook: a pass body recorded access to an undeclared resource.                                            | `abort-frame` (debug) / `abort-engine` (release CI gate). | `error`  | `tests/render/failure/graph_resource_unknown.cpp`     |
| `BarrierConflict`                | `BarrierViolation`    | §3.4 step 4: writer→reader pair the planner cannot satisfy (e.g. WAW on aliased subresource without `declared_use`).                       | `abort-engine`  | `error`  | `tests/render/failure/barrier_violation.cpp`           |
| `TransientPoolExhausted`         | `ResourceResidencyExceeded` (component failure) | §3.5 alias planner total exceeds 256 MiB transient row.                                                                       | `lower-tier`    | `warn`   | `tests/render/failure/resource_alloc_lower_tier.cpp`   |
| `ResourceResidencyExceeded`      | `ResourceResidencyExceeded` | §3.5 alias planner peaks above 512 MiB ceiling (when summed with persistent + RT; rare, only when settings push tier high). | `lower-tier` | `warn` | `tests/render/failure/residency_exceeded_lower_tier.cpp` |
| `CapabilityNotSupported`         | (composite §10.3)     | `add_rt_pass` called when `Capability::HardwareRayTrace` is absent.                                                                       | `disable-feature` | `warn`   | `tests/render/failure/rt_capability_missing.cpp`       |

Cross-cutting notes (per `SPEC.md` §10.2):

- **Severity escalation under hot-reload.** When raised inside the
  first post-resume `compile()`, `RenderGraphCycle` /
  `BarrierConflict` log at `warn` and roll up under
  `core::Error::HotReloadRefused` (`SPEC.md` §8.4); the previous
  plugin keeps running. At engine startup (no prior-good plugin),
  they log at `error` and the engine aborts (`SPEC.md` §10.2
  `abort-engine`).
- **`PassDeclaredUseUnused` is debug-only.** §3.6 instrumentation
  hook on `MetalCommandBuffer`; shipping builds elide.
  Non-fatal; surfaced via the diagnostic overlay.
- **`TransientPoolExhausted` → `ResourceResidencyExceeded`.** The
  alias planner returns the lower-level enum; the call site
  (typically `compile()`) translates it to the §10.1 row that
  triggers `lower-tier` recovery. The recovery flow is in the
  per-context error handler (`render::Error` consumer in render
  plugin's frame loop), not in this aggregate.
- **`CapabilityNotSupported` is the only graph-layer hard reject.**
  Other capability-gated passes silently elide (§3.3); RT is
  hard-rejected because there is no fallback inside `add_rt_pass`'s
  shape — the consumer is expected to register a non-RT alternative
  via its capability predicate (`SPEC.md` §10.3 row
  `RtCapabilityMissing`, recovery `disable-feature`).

The aggregate does **not** emit pipeline-state arms
(`PipelineCompileFailed`, `PipelineCacheMiss`); those belong to
`PSOCache` (#766). It does **not** emit device or queue arms
(`DeviceLost`, `QueueSubmitFailed`, `FenceTimeout`); those belong
to `MetalDevice` and the metal-cpp wrapper layer (#762).

## 11. Test plan

Tests are split between Catch2 unit tests under
`tests/render/graph/` and integration tests under
`tests/render/integration/` that exercise the graph against a real
`MetalDevice` from the platform fixture
(`platform/test/MetalDeviceFixture.hpp`, peer aggregate). Every
test names a §10 row, a §3 algorithm step, a §SPEC §11 acceptance
story, or a §9 benchmark.

### 11.1 Unit tests — `GraphBuilder`

| TC ID                                                 | Trigger                                                                  | Expectation                                                                | §-link        | Story  |
|-------------------------------------------------------|--------------------------------------------------------------------------|----------------------------------------------------------------------------|---------------|--------|
| `graph_builder/declare_transient_unique`              | Two `declare_transient` calls with the same desc.                        | Two distinct `VirtualResourceHandle`s.                                     | §3.5          | #380   |
| `graph_builder/declare_persistent_passes_through`     | `declare_persistent` with a known `ResourceDesc`.                        | Returns a `VirtualResourceHandle` distinct from any transient.             | §3.5          | #380   |
| `graph_builder/declare_imported_borrows_alloc`        | `declare_imported(desc, alloc)` followed by a write declaration.         | Compile rejects unless importer published write capability (mock).         | §3.5 step 0   | #380   |
| `graph_builder/add_raster_pass_records_accesses`      | One raster pass with reads = `[A]`, writes = `[B]`.                       | `RenderGraph::compile()` succeeds; plan has one pass, no barriers.         | §3.1, §3.4    | #380   |
| `graph_builder/add_compute_pass_queue_purity`         | Compute pass declares a write to a `ColorAttachment` resource.            | `compile()` returns `unexpected(PassUnsupportedConfig)`.                  | §3.4 step 2.3 | #385   |
| `graph_builder/add_rt_pass_capability_required`       | `add_rt_pass` on a `CapabilitySet` without `HardwareRayTrace`.            | Returns `unexpected(CapabilityNotSupported)`.                              | §3.3          | #392   |
| `graph_builder/access_unknown_handle_refuses`         | Pass declares a read on an unknown `VirtualResourceHandle`.               | `compile()` returns `unexpected(PassUndeclaredAccess)`.                    | §3.4 step 2.2 | #380   |

### 11.2 Unit tests — `RenderGraph::compile`

| TC ID                                                 | Trigger                                                                  | Expectation                                                                | §-link        | Story  |
|-------------------------------------------------------|--------------------------------------------------------------------------|----------------------------------------------------------------------------|---------------|--------|
| `graph_compile/empty_compiles`                        | A graph with zero passes registered.                                     | `compile()` succeeds; `pass_count() == 0`, `barrier_count() == 0`.         | §3.4          | #380   |
| `graph_compile/single_pass_no_barriers`               | One raster pass with no inter-pass edges.                                | `pass_count() == 1`, `barrier_count() == 0`.                                | §3.4 step 4   | #383   |
| `graph_compile/raw_hazard_emits_barrier`              | A writes `Foo`; B reads `Foo`.                                           | `barrier_count() == 1`; barrier scope = `MTLBarrierScopeRenderTargets`.    | §3.4 step 4   | #383   |
| `graph_compile/cross_queue_emits_fence`               | A on `Compute` writes `Foo`; B on `Graphics` reads `Foo`.                 | One `MTLSharedEvent` signal+wait pair recorded in plan.                    | §3.4 step 4.3 | #385   |
| `graph_compile/barrier_minimum_split_aware`           | One producer + two unordered parallel consumers reading.                  | One coalesced barrier (single signal + fan-out wait), not two pairs.        | §3.4 step 4.4 | #383   |
| `graph_compile/cycle_refuses`                         | A writes `X` reads `Y`; B writes `Y` reads `X`.                          | `compile()` returns `unexpected(RenderGraphCycle)`.                        | §3.4 step 1   | #380   |
| `graph_compile/capability_gated_elision`              | Pass registered with `requires_caps = MeshShaders`; cap absent.           | Pass dropped from `nodes_`; `pass_count()` reflects elision.                | §3.3          | #381   |
| `graph_compile/barrier_count_golden`                  | The MVP S1 graph, 11 passes.                                              | `barrier_count()` is byte-equal to the recorded golden value.              | §3.4 step 4.5 | #383   |
| `graph_compile/structural_hash_determinism`           | Compile the S1 graph twice on the same fixture.                           | `structural_hash()` returns the identical `u64`.                           | §3.8          | #384   |
| `graph_compile/cache_hit_skips_emit`                  | Compile twice with no input change; observe the second compile's path.    | Second `compile()` returns the same `ExecutionPlan*` (cache hit).          | §3.8          | #384   |
| `graph_compile/cache_miss_on_settings_change`         | Compile, flip `RenderSettings.aa_mode`, compile again.                    | Second `compile()` returns a *different* plan pointer; full pipeline ran.  | §3.8          | #384   |
| `graph_compile/multi_view_shares_cache`               | Build two graphs with identical pass sets on two `View`s; compile both.   | Second view's `compile()` is a cache hit (same hash).                      | §3.8, §3.2    | #401   |

### 11.3 Unit tests — Alias planner

| TC ID                                                 | Trigger                                                                  | Expectation                                                                | §-link        | Story  |
|-------------------------------------------------------|--------------------------------------------------------------------------|----------------------------------------------------------------------------|---------------|--------|
| `graph_alias/disjoint_lifetimes_share_slot`           | Two transients, lifetimes [0,1] and [2,3], same size class.               | Same `PhysicalAllocHandle`.                                                | §3.5          | #382   |
| `graph_alias/overlapping_lifetimes_distinct_slots`    | Two transients, lifetimes [0,2] and [1,3].                                | Distinct `PhysicalAllocHandle`s.                                           | §3.5          | #382   |
| `graph_alias/persistent_skips_aliasing`               | Persistent VR with same lifetime as a transient.                          | Persistent allocated separately; never appears in alias plan.              | §3.5 step 0   | #382   |
| `graph_alias/imported_skips_aliasing`                 | Imported VR adjacent to a transient.                                     | Imported's `PhysicalAllocHandle` matches the importer-supplied value.      | §3.5 step 0   | #382   |
| `graph_alias/exhaustion_returns_typed_error`          | Size sum forces the planner above 256 MiB.                                | `compile()` returns `unexpected(TransientPoolExhausted)`.                  | §3.5          | #402   |
| `graph_alias/colouring_deterministic`                 | Two runs of the same graph.                                              | Identical colour assignment (PHILOSOPHY §7).                                | §3.5, §6.4    | #382   |
| `BENCHMARK("transient alias recovery, S1")`           | The MVP S1 graph (~32 transient VRs).                                    | Total slot bytes ≤ 60 % of naive sum-of-sizes.                              | §9.4          | #382   |

### 11.4 Unit tests — `ExecutionPlan::record_into`

| TC ID                                                 | Trigger                                                                  | Expectation                                                                | §-link        | Story  |
|-------------------------------------------------------|--------------------------------------------------------------------------|----------------------------------------------------------------------------|---------------|--------|
| `plan_record/sequential_per_queue`                    | A plan with 3 graphics passes; record into one Graphics command buffer.   | `execute()` lambdas invoked in plan order.                                 | §3.4 step 5   | #380   |
| `plan_record/declared_only_check_debug`               | Lambda accesses a resource not in its declared set; debug build.         | `MetalCommandBuffer` instrumentation aborts; `PassUndeclaredAccess` logged.| §3.6          | #380   |
| `plan_record/queue_purity_runtime_check`              | Lambda on Compute pass tries to open a render encoder.                   | Wrapper rejects; `PassUnsupportedConfig` returned.                          | §3.6          | #385   |
| `plan_record/parallel_per_queue_record`               | Plan with passes on Graphics + Compute; two workers record concurrently. | No data races (TSan); fences emitted between queues.                        | §6.2          | #385   |
| `plan_record/timestamps_inserted_at_boundaries`       | Plan with capability `TimestampQueries`; record one pass.                 | Two `MTLCounterSampleBuffer::sampleCounters` calls observed (begin + end).  | §9.2          | #399   |

### 11.5 Integration tests — real `MetalDevice` (Apple-Silicon CI)

| TC ID                                                 | Trigger                                                                  | Expectation                                                                | §-link        | Story  |
|-------------------------------------------------------|--------------------------------------------------------------------------|----------------------------------------------------------------------------|---------------|--------|
| `graph_integration/mvp_pass_set_compiles`             | Build the 11-pass MVP graph against a real M1 `MetalDevice`.              | `compile()` succeeds; recorded buffer commits without driver error.         | §3.2, §3.4    | #380   |
| `graph_integration/multi_view_split_screen_compiles`  | Two `View`s with the MVP pass set + identical settings.                  | Both compile, second is a cache hit; both buffers commit.                   | §3.2, §3.8    | #401   |
| `graph_integration/blas_refit_before_tlas_build`      | Plan with BLAS-refit + TLAS-build + RT-shadow passes.                    | Inserted edge enforces order; cross-pass fence emitted; RT pass observes built TLAS. | §3.7        | #391   |
| `graph_integration/gbuffer_atomic_writes`             | Plan with the mesh-shader gbuffer pass.                                  | One barrier-emit point; downstream consumers see all four MRT + visID + velocity + depth. | §3.7      | #386   |
| `graph_integration/present_signals_fence`             | Plan with the `present` pass; record + commit one frame.                  | `PresentFence::value` advances; consumer (mock platform) observes the signal.| §3.7        | #396   |
| `BENCHMARK("graph build, S1 main view, p99")`         | S1 fixture, 600 frames, p99 of `add_*_pass × 11 + compile()` (cache hit). | ≤ 0.07 ms.                                                                  | §9.4          | (perf) |
| `BENCHMARK("graph compile cache miss, S1, p99")`      | S1 fixture, force a cache miss each frame.                                | ≤ 0.20 ms.                                                                  | §9.4          | (perf) |
| `BENCHMARK("barrier count, S1, golden")`              | S1 graph; capture `barrier_count()`.                                     | Byte-equal to recorded golden.                                              | §9.4          | #383   |

### 11.6 Coverage matrix

- Every §10 row has at least one fixture (§11.1 / §11.2 / §11.3 /
  §11.4 named).
- Every §3 algorithm step has at least one unit test
  (§3.3 → `capability_gated_elision`; §3.4 step 1 → `cycle_refuses`;
  step 2 → `unsupported_config` + `unknown_handle`; step 3 →
  alias-planner suite; step 4 → `barrier_*` suite; step 5 →
  `cross_queue_*` + record_into).
- Every §SPEC §11 story whose acceptance criterion includes the
  graph aggregate (#380, #381, #382, #383, #384, #385, #386, #391,
  #392, #396, #399, #401, #402) is named in at least one TC ID's "Story"
  column. (#399 "diagnostic overlay / per-pass GPU timing" is included
  because its acceptance criterion exercises the graph aggregate's
  timestamp-insertion boundary in `ExecutionPlan::record_into`; see
  TC `plan_record/timestamps_inserted_at_boundaries` in §11.4.)
- Every §9.4 benchmark is named in §11.5.

The integration tests gate on the **platform-fixture-required**
label; they run on the macOS-26 / M1 CI runner only. Unit tests
run on every PR (no Metal device required; the
`MetalCommandBuffer` is mocked through the wrapper's debug seam).

## 12. Open questions

- **[OPEN]** Pass groups / sub-graphs for editor visualisation.
  §2 refused for MVP. The editor's diagnostic overlay (#774) may
  later request grouped rendering of the DAG (e.g. "RT cluster",
  "post chain") for legibility. If so, groups are a *visualisation
  attribute* not a compile-input — the compiler still sees the
  flat pass set. Resolve at the diagnostic-overlay design ticket.
- **[OPEN]** Per-pass argument-buffer rebind cost optimisation.
  §5.2 attributes ~1 µs per pass to argument-buffer bind; on M1
  measured this is closer to 300 ns. The benchmark named in §9.4
  is sized for the ceiling, but the per-frame budget has slack.
  Whether to coalesce per-frame bindings across passes is a future
  perf-budget amendment; the graph layer does not change shape
  either way.
- **[OPEN]** `eastl::function` storage size for `PassExecuteFn`.
  §4 invariant 3 picks an inline-storage budget of 32 bytes. The
  MVP pass bodies' captures are well under this (most are 8–16
  bytes: a few pointers + a `RenderFrame` reference). If a future
  pass body needs a larger capture, the fall-back to per-frame
  arena is correct (no heap) but adds one indirection. Tighten the
  budget if profile shows the indirection costs more than 100 ns
  per pass. Defer until the per-pass design tickets land.
- **[OPEN]** Multi-view compile-cache eviction policy. §3.8 picks
  LRU with a 16-plan cap. Post-MVP scenes with many shadow
  cascades or capture views may want a larger cache or a different
  eviction (e.g. priority-weighted by `PassPriority::Mandatory`).
  Resolve when a multi-view scene exceeds the 16-plan cap in
  practice; not blocking MVP.
- **[OPEN]** Cross-queue fence pool sizing. §3.4 step 4.3 emits
  `MTLSharedEvent` signal+wait pairs for cross-queue edges; the
  pool capacity is set at init from the perf budget. The MVP graph
  has at most ~3 cross-queue edges per frame (compute → graphics
  ×2 + graphics → compute ×1) so the pool is generously sized;
  post-MVP graphs may pressure this. Resolve at the perf-budget
  amendment that lands the post-MVP pass additions.
- **[OPEN]** Resource-residency-exceeded recovery path. §10's
  `lower-tier` recovery flips `RenderSettings.quality_tier` and
  recompiles. The graph aggregate is unaware of `RenderSettings`
  beyond the structural-hash input; the recovery handler that
  edits `RenderSettings` lives one layer up (in render's frame
  loop). Confirm the seam at the render-plugin entry-point design
  ticket; the graph layer's contract is "compile fails →
  Result<unexpected>" and that is ABI-stable.
- **[OPEN]** Editor live-pass-source reload. Listed in §8.5;
  out of MVP scope. The structural-hash key (§3.8) supports it
  cleanly when the path lands — a source change to a pass body
  invalidates `pass.name_hash` (assuming the editor regenerates
  the name on edit) and the next frame's `compile()` is a cache
  miss. No graph-layer redesign required.
