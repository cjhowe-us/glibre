# Render Spec

## 1. Purpose

The `render` context owns one responsibility: turning a frame-N
`RenderFrame` extract into a presented Metal 4 frame on screen. That
covers building the per-frame **render graph** (declarative pass
builder, capability gating, transient-resource alias planner,
barrier/queue scheduler, plan compiler), executing it on the Metal 4
device (mesh-shader gbuffer, hybrid-RT shadows + AO + reflections,
clustered light culling, deferred lighting, transparent forward, post
chain, anti-aliasing/upscaling, present), owning the per-frame GPU
resource lifecycle (PSO cache, descriptor/argument-buffer organisation,
ring-buffered constant/instance/indirect buffers, gbuffer + transient
pool, swapchain integration), and providing the render-side debug
overlay + GPU-timing readback used by the editor and CI gates. Render
owns frame-phase 6 (`cull-extract` — building the immutable
`RenderFrame` from ECS) and frame-phase 7 (`render-submit` — recording
and submitting the Metal 4 command buffer); it consumes inputs from the
ECS via the snapshot bus and signals the present fence consumed by
`platform`'s phase 9. Render refuses to own anything outside that seam.
HLSL→AIR/metallib compilation belongs to `shader`; material graph
authoring, codegen and the bindless material parameter schema belong to
the future `material` plugin (consumed via opaque material handles);
VFX particle/cloth/fluid simulation belongs to the future `vfx` plugin
(render only consumes the resulting GPU-resident buffers and meshes);
mesh authoring, meshlet builds, BLAS construction at cook time, vertex
streaming, and Draco decode belong to `geometry` and `content` (render
consumes immutable handles only); the engine frame schedule and ECS
runtime belong to `core`; window, surface and input belong to
`platform`. Render is the only consumer of the GPU device, but it owns
no second tier of domain logic — it is a faithful executor of one frame
per tick. Per SRP every reason render has to change must trace back to
one of those listed responsibilities; anything else routes to the owning
context.

## 2. Ubiquitous Language

Terms used unchanged in code (identifiers, file names, comments).

| Term | Meaning |
|------|---------|
| `RenderFrame` | Immutable snapshot built in phase 6: visible-set, draw cmds, lights, camera(s), interp alpha, view list, sort keys; the only input to phase 7. |
| `RenderGraph` | C++-coded DAG of `Pass` nodes built each frame by the `GraphBuilder`; never serialised to disk (PHILOSOPHY anti-pattern). |
| `GraphBuilder` | Fluent API plugins use to declare passes, their resource reads/writes, queue affinity, and capability requirements. |
| `Pass` | One scheduled unit (mesh-shader draw, compute dispatch, blit, RT trace, present); declares its access set; opaque to neighbours. |
| `PassPriority` | Numeric priority used by the cost-aware budget culler when historical GPU timing exceeds the frame budget. |
| `ExecutionPlan` | Compiled output of the graph: ordered pass list + barrier set + alias plan + queue assignment, cached until the pass set or capabilities change. |
| `VirtualResource` | Logical handle (format + dims + usage) declared in the graph; the alias planner maps it to a `PhysicalAllocation` at compile time. |
| `PhysicalAllocation` | Backing memory region for one or more lifetime-disjoint `VirtualResource`s; placed in heaps owned by the render allocator. |
| `AliasPlan` | Interference-graph colouring result mapping virtual resources to physical allocations; minimum-VRAM solution per frame. |
| `Barrier` | Render-graph-emitted Metal 4 barrier (memory + execution); the planner emits the minimum split-aware set between passes. |
| `Queue` | Metal command queue role: `Graphics`, `Compute`, `Copy`. The plan assigns each pass exactly one queue; cross-queue order is fence-driven. |
| `View` | One camera-like consumer of the graph: main, shadow cascade, reflection probe, viewmodel, capture-to-texture. Multi-view shares cull where possible. |
| `RenderLayer` | 32-bit bitmask filtering visibility; an entity is visible to a view only when masks overlap. |
| `SortKey` | Packed 64-bit draw-list key (translucency, phase, pipeline, material, quantised depth) sorted by single-pass radix. |
| `DrawCmd` | One element of a draw list — pipeline, mesh handle, instance range, material index, sort key. |
| `IndirectDrawBuffer` | GPU-side, material-grouped, compaction output of opaque survivors; consumed by mesh-shader indirect dispatch. |
| `MeshShaderDispatch` | Phase-7 path that turns surviving meshlets into rasterised geometry via Metal 4 mesh shaders. |
| `GBuffer` | MRT layout written by the gbuffer pass (albedo+metallic, normal+roughness, motion, depth). |
| `LightCluster` | Compute-built per-froxel light list consumed by the deferred lighting pass. |
| `ShadowAtlas` | Tiled shadow target. CSM cascades live here; sized by quality tier. |
| `RTAccelStructures` | The TLAS built/refit each frame from `geometry`-owned BLAS handles; consumed by RT shadow / AO / reflection passes. |
| `HZB` | Hierarchical Z-buffer used by the two-phase occlusion culler in phase 6. |
| `RenderProxy` | SoA, GPU-shape mirror of an ECS renderable extracted in phase 6; only the GPU-needed fields. |
| `PSO` | Pipeline state object. Compiled offline by `shader` cook; render owns the residency cache and binding. |
| `ArgumentBuffer` | Metal 4 bindless table; render organises bindings into per-frame / per-pass / per-material / per-draw groups. |
| `MaterialHandle` | Opaque index into the material plugin's parameter table; render binds it bindlessly and never inspects its contents. |
| `RingBuffer` | Per-frame-in-flight CPU-write / GPU-read region for instance, uniform, and indirect buffers; sized to the frames-in-flight count. |
| `TransientPool` | Pool of placed allocations the alias planner draws from; drains and rebuilds on plan recompile. |
| `Swapchain` | The Metal `CAMetalLayer` drawable cycle; render acquires inside the `present` pass, signals to `platform` for phase 9. |
| `PresentFence` | GPU/CPU fence flagged when frame N's command buffer is enqueued; consumed by phase 9 to drive `CAMetalDisplayLink` pacing. |
| `QualityTier` | Init-time platform tier (`Mobile` / `Switch` / `Desktop` / `HighEnd`) selecting per-effect parameters with no hot-path branching. |
| `RenderSettings` | Resource holding per-view feature flags: AA mode, upscaler, shadow tier, AO tier, RT enable, dynamic-resolution bounds. |
| `DiagnosticOverlay` | Debug-only DAG visualiser + per-pass GPU-timer readout; compile-time gated out of shipping builds. |
| `GpuTimestamp` | Per-pass timing point read back one frame later; feeds the pass-budget culler and the editor profiler. |
| `DynamicResolution` | Closed-loop scaler adjusting internal render resolution within configured bounds to hit a target GPU budget. |

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
