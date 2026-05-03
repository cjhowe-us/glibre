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

Harmonius rendering prior art was mined as **research input only**; every
conclusion below is independently re-derived against `PHILOSOPHY.md`
(SOLID, SRP, plugin-only growth, static codegen, no runtime reflection in
shipping builds), the engine-wide nine-phase frame schedule
(`reviews/decisions/frame-phases.md`), and the §1/§2 commitments above.
Cited paths live under `/Users/cjhowe/Code/harmonius/docs/`. Per
PHILOSOPHY §10 every multi-source concept is collapsed into the smallest
glibre primitive that still satisfies SRP; per PHILOSOPHY §3 any concern
that does not trace back to "turn a `RenderFrame` into a presented Metal 4
frame" is refused and routed to the owning context.

### 3.1 Cited harmonius sources

| Cluster | Harmonius file(s) | Used for |
|---------|-------------------|----------|
| GPU abstraction | `requirements/rendering/gpu-abstraction-layer.md` (R-2.1.1 .. R-2.1.18), `requirements/rendering/gpu-abstraction.md` (GR-1.x .. GR-4.x), `design/rendering/render-pipeline.md` (§ "GPU Abstraction Layer", "GPU Module Boundaries") | Backend dispatch model, GPU heap sub-allocator shape, redundant-state-filter idea, descriptor frequency groups, deferred timestamp readback, generational handles, structured error variants. |
| Render graph | `requirements/rendering/render-graph.md` (R-2.2.1 .. R-2.2.13, R-2.2.3a), `design/rendering/render-pipeline.md` (§ "Render Graph"), `design/rendering/rendering-core.md` (§ "Architecture", "Data Flow"), `design/rendering/pipeline-state-cache.md` | Declarative `Pass` + `GraphBuilder`, virtual-resource alias planner, minimal-barrier emission, multi-queue scheduling, graph caching, multi-view fan-out, budget-aware culling, diagnostic overlay. |
| Scene/extract pipeline | `requirements/rendering/scene-rendering-pipeline.md` (R-2.10.1 .. R-2.10.9, R-2.10.4a, R-2.3.14, R-2.4.24), `design/rendering/rendering-core.md`, `design/rendering/camera-rendering.md` (§ "Architecture", "Render Layer Masking") | `RenderFrame` extract, SoA `RenderProxy`, dirty-flag incremental updates, multi-view from one snapshot, 64-bit `SortKey`, ring-buffered per-frame resources, transform interpolation, 32-bit `RenderLayer` mask, mesh-shader dispatch with indirect-draw fallback. |
| Core raster | `requirements/rendering/core-rendering.md` (R-2.3.1 .. R-2.3.14), `design/rendering/meshlets.md` (§ "Architecture", "Data Flow"), `design/rendering/render-pipeline.md` | Meshlet GPU frustum + normal-cone cull, two-phase HZB, reverse-Z, GBuffer-deferred MRT layout, indirect-draw compaction by material, dynamic-resolution feedback loop, alpha-tested shadow participation. |
| Lighting | `requirements/rendering/lighting.md` (R-2.4.1 .. R-2.4.17) | Tiled/clustered light culling (`LightCluster`), unified light buffer for forward + deferred parity, CSM (`ShadowAtlas`), tiered soft shadows (PCF→PCSS→RT), tiered AO (SSAO→GTAO→RT), area lights / IES / IBL acceptance shape. |
| Advanced rendering / RT | `requirements/rendering/advanced-rendering.md` (R-2.5.1 .. R-2.5.10), `design/rendering/render-effects.md` | TLAS refit each frame from cooked BLAS handles (`RTAccelStructures`), hybrid-RT shadows / AO / reflections MVP set, denoising hooks, future surfaces for path-tracing reference. |
| Anti-alias / upscale | `requirements/rendering/anti-aliasing-upscaling.md` (R-2.6.1 .. R-2.6.9) | TAA + FXAA + SMAA + temporal-super-resolution slot model behind one `RenderSettings.aa_mode` enum; vendor-upscaler abstraction with built-in fallback. |
| Post chain | `requirements/rendering/post-processing.md` (R-2.9.1 .. R-2.9.14), `design/rendering/render-effects.md` (§ "Architecture") | Bloom / DOF / motion-blur / tonemap / grade / film-grain / vignette / chromatic-aberration / panini / cavity, post-process volume blending, `QualityTier`-driven init-time selection, HDR / Dolby Vision output path. |
| Per-platform integration | `requirements/rendering/first-person-rendering.md` (R-2.13.1 .. R-2.13.8), `design/rendering/camera-rendering.md` (viewmodel layer + multi-view), `design/rendering/render-styles.md` (§ "Architecture") | Viewmodel as a separate `View` with its own near-clip + post opt-in/out via stencil; multi-view from one extract. (Stylized / NPR / character-rendering details mined for shape only — see refusals.) |
| 2D/UI raster | `design/rendering/2d.md` (§ "Architecture", "API Design") | 2D path treated as another `View` + dedicated passes inside the same render graph, not a separate engine. (Sprite/UI authoring is `tools` + `content`, not render.) |

Inputs read but **not** adopted as render responsibilities (see §3.3): the
shader-variant authoring pipeline (`requirements/rendering/scene-rendering-pipeline.md`
material-graph language, `design/rendering/shader-variants.md`,
`design/rendering/pipeline-state-cache.md` cook-time pieces),
the material-codegen surface (`requirements/rendering/advanced-materials.md`,
`requirements/rendering/character-rendering.md`,
`requirements/rendering/stylized-effects.md`),
the environment / VFX simulation surface (`requirements/rendering/environment.md`),
and the meshlet **cook** (`design/rendering/meshlets.md` § "Pipeline").

### 3.2 Occam collapses (multiple harmonius concepts → one glibre primitive)

1. **Many specialised render systems → one declarative render graph +
   one Metal 4 backend.** Harmonius shipped a `harmonius_gpu` backend
   trait, a separate `harmonius_gpu_runtime` of memory / state /
   barrier / work-graph services, and `harmonius_rg` on top, plus three
   parallel platform backends (Metal/D3D12/Vulkan) per
   `requirements/rendering/gpu-abstraction-layer.md` R-2.1.4..6 and
   `requirements/rendering/gpu-abstraction.md` GR-1..GR-4. Glibre
   collapses this to a single primitive: the **`RenderGraph`** declared
   each frame by `GraphBuilder` (§2), executed on a single Metal 4
   device. There is no abstraction layer over backends — Metal 4 is the
   one shipping target — so the GR-1/GR-2/GR-4 services collapse into
   private members of the render plugin (the heap sub-allocator, the
   redundant-state filter, the barrier merger, the descriptor
   frequency-group binder) rather than a separate runtime crate.
   Justification: SRP — one reason to change the GPU layer is "Metal 4
   evolves"; that lives behind one seam, not three.

2. **Multiple raster paths → mesh-shader gbuffer + hybrid-RT shadow MVP
   set.** Harmonius enumerated forward, deferred, MSAA-forward,
   compute-rasterised hair, software-rasterised path tracer
   (`requirements/rendering/lighting.md` R-2.4.1..R-2.4.17,
   `requirements/rendering/anti-aliasing-upscaling.md` R-2.6.3,
   `requirements/rendering/advanced-rendering.md` R-2.5.5..R-2.5.10,
   `requirements/rendering/character-rendering.md` R-2.8.3) and required
   them to produce pixel-equivalent output. Glibre collapses MVP to one
   pipeline: mesh-shader gbuffer → clustered light cull → deferred
   lighting → transparent forward → hybrid-RT shadow / AO / reflection
   composite → post chain → present (frame phases 6 + 7 per
   `reviews/decisions/frame-phases.md`). Forward-only, MSAA forward,
   path-tracing reference, compute-rasterised strands, and surfel-/DDGI-
   based GI are post-MVP and re-enter as additional `Pass` nodes inside
   the same graph; they do **not** justify a second pipeline shape.
   Justification: PHILOSOPHY §5 (greatly reduced MVP scope), §10
   (Occam) — one pipeline instantiated as one `ExecutionPlan` is the
   minimum that closes the §1 responsibility on day one.

3. **Forward + deferred unified light buffer → one `LightCluster` +
   one GBuffer.** Harmonius R-2.4.1..R-2.4.2 required forward and
   deferred paths to consume the same unified light list. With the MVP
   collapsed to one deferred path the unification is trivial: a single
   compute-built `LightCluster` feeds the deferred lighting pass, and
   the transparent-forward pass reads the same cluster. No second light
   buffer.

4. **Many extract / proxy / batch / sort layers → one `RenderFrame`.**
   Harmonius scene-rendering-pipeline (R-2.10.1..R-2.10.9) split
   extraction, proxy storage, view setup, draw-list assembly, batch
   compaction, and sort into separate cooperating subsystems. Glibre
   collapses everything that crosses the ECS↔GPU seam into the single
   immutable `RenderFrame` snapshot defined in §2: one struct, one
   producer (phase 6), one consumer (phase 7). The scene-rendering bits
   that appear distinct in harmonius (proxy SoA, dirty diff, view list,
   sort keys, layer mask, interp alpha) become **named fields** of
   `RenderFrame`, not separate aggregates. Justification: SRP — one
   reason to change "what crosses the ECS/GPU seam" routes to one
   data structure.

5. **Many anti-alias / upscale paths → one `RenderSettings` enum.**
   Harmonius enumerated TAA, FXAA, SMAA, MSAA, TSR, DLSS, FSR, XeSS,
   checkerboard, frame-gen, Reflex/Anti-Lag (R-2.6.1..R-2.6.9). Glibre
   keeps every option but folds selection into a single enum on
   `RenderSettings` per view; the graph builder picks the matching pass
   chain at compile time. No runtime branching in shaders — pure
   `QualityTier` / `RenderSettings`-driven graph topology.

6. **Many post-FX kinds → one post graph segment + post-process volume
   blend.** Bloom, DOF, motion blur, exposure histogram, tonemap, grade,
   film grain, vignette, panini, cavity, custom post graphs, Dolby
   Vision (R-2.9.1..R-2.9.14) all reduce to a sequence of `Pass` nodes
   inside the render graph reading scene textures and writing the
   post-LDR/HDR target; volume blending is one read of the per-view
   post-parameter buffer, not a separate runtime.

7. **Multi-view fan-out (split-screen / VR / shadow / probe / capture /
   minimap / viewmodel) → repeated `View` instances, one graph.**
   Harmonius handled split-screen, VR stereo, shadow cascades,
   reflection probes, scene-capture cameras, and viewmodel as separate
   subsystems (R-2.2.7, R-2.10.3, R-2.13.1..R-2.13.8,
   `design/rendering/camera-rendering.md`). Glibre collapses these to
   one `RenderGraph` instantiated once per `View`; the graph compiler
   shares culling and lighting where it can (R-2.2.7 fan-out semantics)
   without a second code path.

8. **Pipeline-state-cache + descriptor-set-management →
   one `PSO` cache + one `ArgumentBuffer` frequency-group binder.**
   `design/rendering/pipeline-state-cache.md` and the GR-1/GR-2 services
   collapse into private cache members of the render plugin: the cache
   is keyed by a `PSO` identity baked at cook time by `shader`, and
   bindings are organised into the four frequency groups
   (per-frame / per-pass / per-material / per-draw) that R-2.1.16
   already named.

9. **Hundreds of "tiered" effect quality knobs → one `QualityTier` +
   one `RenderSettings`.** Harmonius scattered Mobile/Switch/Desktop/
   HighEnd selection logic across every effect doc (R-2.4.4, R-2.4.7,
   R-2.4.13, R-2.5.6, R-2.6.x, R-2.9.13, etc.). Glibre selects all
   per-effect parameters at init time from one `QualityTier` value
   and one `RenderSettings` resource; effects read those, never branch
   on capability bits in their hot path (PHILOSOPHY §6: zero runtime
   reflection / runtime-branched permutations in shipping).

10. **Diagnostic overlay + GPU-timing readback + budget-culler →
    one `DiagnosticOverlay` + one `GpuTimestamp` ring.** Harmonius split
    diagnostic visualisation (R-2.2.11), pass-cost feedback (R-2.2.6),
    and editor profiler hooks across multiple files; glibre collapses
    them to one debug-only overlay that visualises the live graph and
    one ring of per-pass `GpuTimestamp`s read back one frame later
    (R-2.1.12 already shaped this) — the same ring feeds the cost-aware
    pass-budget culler and the editor profiler.

### 3.3 Refusals (routed to other contexts)

Glibre's render plugin does **not** own any of the following, even
though harmonius collected them under "rendering". Each routes to the
owning context per §1:

| Harmonius surface | Cited file(s) | Routed to |
|-------------------|----------------|-----------|
| HLSL → AIR / metallib compilation, shader-variant cook, DXC subprocess management, `metal-shaderconverter` invocation | `requirements/rendering/gpu-abstraction-layer.md` R-2.1.17, `design/rendering/shader-variants.md`, `design/rendering/pipeline-state-cache.md` (cook-time half) | `shader` plugin. Render consumes opaque `PSO` handles. |
| Material graph authoring, material-codegen, custom material functions, the bindless material parameter buffer schema, fabric / clearcoat / SSS / hair / eye / skin / weathering / refraction / emissive surface graphs | `requirements/rendering/advanced-materials.md` R-2.12.1..R-2.12.9, `requirements/rendering/character-rendering.md` R-2.8.1..R-2.8.8, `requirements/rendering/lighting.md` R-2.4.3..R-2.4.9 (codegen aspects) | `material` plugin. Render consumes opaque `MaterialHandle` indices and binds them bindlessly without inspecting their contents. |
| Particle systems, GPU sim of cloth / fluid / hair strands, ocean FFT, volumetric clouds, weather state machine, OpenVDB volume sim, decals as a simulated system, breaking-wave deformation | `requirements/rendering/environment.md` R-2.7.1..R-2.7.9, `requirements/rendering/character-rendering.md` R-2.8.3 (compute strand sim half), `requirements/rendering/stylized-effects.md` R-2.11.x (the simulation half) | `vfx` plugin. Render consumes already-resident GPU buffers / meshes published by `vfx` and draws them via standard `Pass` nodes. |
| Mesh import, meshlet building, BLAS construction / compaction, vertex streaming, Draco decode, virtualised geometry residency, geometry LOD policy, hair-card LOD, mesh-proxy generation | `design/rendering/meshlets.md` (cook half), `requirements/rendering/advanced-rendering.md` R-2.5.1 (BLAS build + compaction at cook), `requirements/rendering/character-rendering.md` R-2.8.2 (LOD policy half) | `geometry` (cook + LOD policy) + `content` (residency, streaming). Render consumes immutable mesh / BLAS handles. |
| Stylised / NPR rendering authoring, toon ramp authoring, painterly / pixel-art parameter authoring, outline parameterisation as content data | `requirements/rendering/stylized-effects.md` R-2.11.1..R-2.11.7, `design/rendering/render-styles.md` (authoring half) | Style **execution** stays in render (just more `Pass` nodes); style **authoring** (graphs, parameters, art content) routes to `material` and `tools`. |
| Engine frame schedule, ECS runtime, hot-reload protocol, plugin registry | `design/rendering/render-pipeline.md` § "Task Graph Integration" (R-2.2.12, R-2.2.13) | `core`. Render registers systems into phases 6 and 7 per `reviews/decisions/frame-phases.md`; it does not own the schedule. |
| Window, surface, swapchain creation policy, input pump, frame pacing (`CAMetalDisplayLink`), display configuration | `requirements/rendering/scene-rendering-pipeline.md` (no direct harmonius coverage — implied by R-2.6.7..R-2.6.8 latency tooling) | `platform`. Render acquires the next drawable inside its `present` pass and signals `PresentFence`; pacing belongs to phase 9. |

These refusals are the application of PHILOSOPHY §3 (minimal core,
plugin-only growth) + §1 (SRP) to the harmonius "rendering" umbrella:
anything whose reason-to-change does not collapse to "one frame, one
graph, one Metal 4 device" lives in another plugin.

## 4. Aggregates & Invariants

This section enumerates render's aggregates, value objects, and the
invariants every public boundary must hold. Aggregates are listed in
data-flow order (extract → graph → execute → present). Each aggregate
owns one dimension of "turn a `RenderFrame` into a presented Metal 4
frame"; per PHILOSOPHY §1 (SRP), an aggregate is admitted to this list
only when its single reason-to-change does not collapse into another's.
Where two harmonius primitives reduce to one glibre primitive, the
collapse is cited from §3.2. Cross-context concerns (shaders, materials,
geometry, hot-reload, frame schedule) are explicitly delegated and never
re-asserted here (§3.3).

### 4.1 Aggregate roster

#### 4.1.1 `RenderFrame` — immutable per-frame extract (entity)

**Reason to change:** what crosses the ECS↔GPU seam (one structure,
not many; §3.2 collapse #4).

**Composition.** Owns the full immutable input that phase 7 consumes:
the active `View` list, per-view culled `RenderProxy` SoA spans, the
linearised `DrawCmd` array bucketed by phase (opaque / alpha-tested /
translucent / shadow / 2D / capture), the packed `SortKey` column, the
per-view `RenderLayer` mask, the active light list, the per-view
`RenderSettings` snapshot, the camera + jitter + interp-α, and the frame
counter `N`. No GPU handles inside `RenderFrame` itself — the proxy SoA
references opaque mesh / material / BLAS handles by index; phase 7
resolves residency.

**Identity & lifetime.** One snapshot per `(World, FrameCounter)` pair;
allocated in render's per-frame arena at phase 6 entry, destroyed at
phase 7 exit. Never persists across frames. Triple-buffered behind a
generational handle so frame N+1's extract may begin while frame N's
record is still in flight on the GPU.

**Public-boundary invariants.**

1. **Immutable post-build.** After phase 6's `cull-extract` returns,
   no field of `RenderFrame` may be written. Phase 7 takes a
   `const RenderFrame&`; any attempt to mutate is a compile error.
2. **Self-contained.** Phase 7 never reads back into the ECS for
   draw data. Every byte phase 7 needs is reachable from the
   `RenderFrame` root pointer.
3. **Bounded.** Visible-set size is capped by `RenderSettings`'s
   per-view draw budget; over-budget proxies are budget-culled in
   phase 6 (cost-aware, `PassPriority`-respecting) before snapshot
   build, never silently dropped during phase 7.
4. **Stable under one-frame pipelining.** If phase 6 of frame N+1
   begins while phase 7 of frame N is still recording, the two
   `RenderFrame` instances live in distinct triple-buffer slots; no
   field aliases.

#### 4.1.2 `RenderGraph` — DAG of `Pass` nodes built each frame (aggregate root)

**Reason to change:** graph topology — adding / removing / reordering
passes for a `View`. Distinct from "what a single pass body does"
(§4.1.3) and from "how a resource is allocated" (§4.1.4).

**Composition.** Holds the in-construction node list (each node a
`Pass`), the explicit edge set produced by `GraphBuilder`'s
read/write resource declarations, the per-`View` instantiation
cursor (multi-view fan-out reuses the same builder code per §3.2
collapse #7), and a back-pointer to the originating `RenderFrame`.
Built once per `View` per frame in ordinary C++ — never serialised
(PHILOSOPHY anti-pattern). Compilation produces an
`ExecutionPlan` (§4.1.5).

**Identity & lifetime.** One graph per `(View, FrameCounter)` pair;
the graph object is destroyed once its `ExecutionPlan` has been
recorded into a Metal command buffer. The plan is cached across
frames keyed by the structural hash of pass-set + capability set;
the graph object itself is not.

**Public-boundary invariants.**

1. **Acyclic.** The compile step performs a topological sort; any
   cycle returns `render::Error::RenderGraphCycle` (per
   `reviews/decisions/error-model.md`) and refuses to produce a
   plan. No partial plan is exposed.
2. **Closed access set.** Every `Pass` declared on the graph
   declares its complete read and write resource set up-front; the
   compiler refuses (rejects with
   `render::Error::PassUnsupportedConfig`) any pass whose
   `execute()` lambda touches a resource not in its declaration.
3. **Capability-gated.** Passes guarded by a `QualityTier` /
   `RenderSettings` predicate that evaluates false at build time
   are absent from the node list (no runtime branch in shaders;
   PHILOSOPHY §6).
4. **Build-once-per-frame.** No path mutates an in-flight graph
   after `compile()` returns; reconfiguration produces a new graph
   instance.

#### 4.1.3 `Pass` — typed read/write declaration + execute lambda (entity)

**Reason to change:** a single pass's body or its access set —
e.g. swapping the gbuffer write list, replacing the deferred
lighting body, adding a denoiser. Bounded.

**Composition.** A name (debug-only string view), a
`Queue` affinity (Graphics / Compute / Copy), a `PassPriority`,
a typed list of `(VirtualResource, AccessKind)` reads, the same
for writes, an optional capability predicate, and the
`execute(MetalCommandBuffer&, const Bindings&)` lambda the
compiler will invoke during recording. Bindings are produced
from the alias plan and the argument-buffer frequency-group
binder (§3.2 collapse #8); the lambda never sees raw heap
addresses or implicit globals.

**Identity & lifetime.** Same as the owning `RenderGraph`. The
lambda captures only POD or `std::span` slices into the
`RenderFrame`; no owning heap allocations.

**Public-boundary invariants.**

1. **Declared = used.** A pass body must touch every declared
   read/write at least once and must not touch any undeclared
   resource. Violations are caught by a debug-build
   instrumentation hook on `MetalCommandBuffer` (no runtime cost
   in shipping builds).
2. **Queue-pure.** A pass declared on `Queue::Compute` may not
   record graphics encoders; a `Queue::Graphics` pass may not
   record blit encoders. Cross-queue ordering is mediated by the
   compiler-emitted fence set, not by intra-pass code.
3. **Atomic side-effect set.** A single pass either writes its
   full declared output set or none of it (recovered on error
   via Metal command-buffer abandonment). For the gbuffer pass
   specifically, the visibilityID, motion-vector, and gbuffer
   MRT writes happen inside a single mesh-shader dispatch
   declared as one `Pass` with one barrier-emit point — see
   §4.2 invariant 5.
4. **No allocations on the hot path.** `execute()` lambdas may
   not allocate; ring-buffer slices and transient placements
   come pre-resolved from the bindings struct.

#### 4.1.4 `Resource` — virtual + physical; transient / persistent / imported

**Reason to change:** how a resource is laid out in memory or how
its lifetime is computed. Distinct from graph topology.

**Composition.** `VirtualResource` is a value object (format,
extent, sample count, usage mask, frame-of-birth, frame-of-last-use,
optional debug name). `PhysicalAllocation` is an entity owning a
heap range or a Metal `MTLTexture`/`MTLBuffer` placed on a shared
`MTLHeap`. Three resource roles compose with `VirtualResource`:

- **Transient.** Materialised by the alias planner from the
  `TransientPool`'s shared heaps; one frame's lifetime; aliases
  freely with any non-overlapping transient.
- **Persistent.** Outlives a single frame (HZB last-frame mip
  pyramid, history color for TAA, motion accumulator,
  `ShadowAtlas`, ring-buffered constant heaps). Owned by render's
  long-lived allocator; not eligible for aliasing inside
  `AliasPlan`.
- **Imported.** Externally owned (Metal-vended `MTLDrawable`
  swapchain texture, `geometry`-vended BLAS, `vfx`-vended particle
  buffers, `shader`-vended PSO blobs). The graph reads/writes via
  a borrow that does not transfer ownership; lifetime is enforced
  by the importer, not render.

**Identity & lifetime.** `VirtualResource` IDs are stable inside
a single graph compilation; `PhysicalAllocation`s persist across
frames inside the `TransientPool` heap pool but their occupants
rotate per `AliasPlan`.

**Public-boundary invariants.**

1. **Transient never outlives a frame.** No `VirtualResource`
   marked transient may be referenced after phase 7 exit of the
   frame that declared it. The graph compiler refuses any pass
   whose declared-use frame index does not equal the current
   `FrameCounter`.
2. **Lifetime-disjoint aliasing only.** The `AliasPlan` is the
   minimum-VRAM colouring of the interference graph computed
   from declared first-write / last-read frames per
   `VirtualResource`; two virtual resources share a
   `PhysicalAllocation` only when their lifetimes are proven
   disjoint at compile time.
3. **Imported resources are read-borrows by default.** A pass may
   write to an imported resource only when the importer publishes
   write capability (e.g. the swapchain target inside the
   `present` pass); render never co-owns externally vended state.
4. **Persistent budget bounded.** The aggregate persistent
   footprint is declared at init time against the per-context
   memory cell from `reviews/decisions/perf-budget.md`; exceeding
   it returns `render::Error::ResourceResidencyExceeded` rather
   than silently degrading.

#### 4.1.5 `ExecutionPlan` — compiled graph output (value object)

**Reason to change:** how a graph compiles — barrier algorithm,
topological-sort policy, queue-assignment heuristic. Decoupled
from any single `Pass` body.

**Composition.** An ordered pass list (post-topological-sort), a
`Barrier` set per inter-pass edge (memory + execution + queue
fences, computed split where Metal 4 supports it), the alias plan
mapping each `VirtualResource` to a `PhysicalAllocation` slot in
the `TransientPool`, the queue assignment per pass, and the
binding tables (per-frame / per-pass / per-material / per-draw
argument-buffer offsets). Plans are cached by the structural hash
of the source graph; cache hit = re-bind only, no recompile.

**Public-boundary invariants.**

1. **Barrier-minimal.** The barrier set emitted for a plan is the
   minimum split-aware set sufficient to honour every declared
   read-after-write and write-after-read in declaration order.
   Adding a redundant barrier is a regression caught by the
   barrier-count golden test.
2. **Alias-correct.** No two passes that race for a
   `PhysicalAllocation` may overlap in the queue-merged
   execution timeline.
3. **Plan = pure function of (pass set, capability set, view
   topology).** Two graphs producing the same triple compile to
   byte-equal plans; this is what makes the cache hash sound.

#### 4.1.6 `MetalDevice` / `MetalQueue` / `MetalCommandBuffer` — metal-cpp wrappers

**Reason to change:** Metal 4 evolves (single seam, per §3.2
collapse #1).

**Composition.** Thin RAII wrappers over `metal-cpp`
(`MTL::Device`, `MTL::CommandQueue`, `MTL::CommandBuffer`)
exposing only the surface render needs: queue acquisition,
command-buffer commit, fence signal/wait, residency-set
attachment, debug-marker push/pop. No vendor branching, no second
backend.

- **`MetalDevice`** — engine-singleton; owns the heap allocator,
  the residency set, the PSO cache (§4.1.7), and provides the
  three queue handles.
- **`MetalQueue`** — one per role (`Graphics`, `Compute`, `Copy`);
  holds a per-queue submission counter for fence ordering.
- **`MetalCommandBuffer`** — frame-scoped; carries a back-pointer
  to its owning `Queue`, the active encoder, and the
  argument-buffer binding cursor; fed to each `Pass::execute()`.

**Public-boundary invariants.**

1. **Single device per process.** Constructed once during
   `core`'s init phase; every render allocation routes through it.
2. **No exception path.** Wrapper methods that can fail return
   `glibre::Result<T>` per `reviews/decisions/error-model.md`;
   exceptions never cross the boundary.
3. **Encoder discipline.** A `MetalCommandBuffer` may have at most
   one open encoder at a time; switching encoders implicitly
   ends the previous and emits any plan-required barrier — the
   wrapper enforces this in debug builds and assumes it in
   shipping.
4. **Queue purity.** Cross-queue commands route via fences only;
   a buffer recorded on `Graphics` may not be committed to
   `Compute`.

#### 4.1.7 `PSOCache` — pipeline-state-object residency cache (entity)

**Reason to change:** how compiled pipelines are looked up,
warmed, and evicted on the device.

**Composition.** A hash table keyed by `PSOKey =
(shader_hash, state_hash)` mapping to a resident
`MTL::RenderPipelineState` / `MTL::ComputePipelineState`. The
`shader_hash` is the cook-time hash of the AIR/metallib produced
by `shader`; the `state_hash` is render's own deterministic hash
of the non-shader state half (vertex layout, blend, depth, MRT
formats, sample count, raster state). PSOs are populated lazily
at first use, with a warmer that pre-faults the MVP set during
init from a manifest emitted by `shader`'s cook.

**Public-boundary invariants.**

1. **Key = `(shader_hash, state_hash)`.** Both halves are required
   and sufficient; identical keys must map to byte-equal pipeline
   bytecode. A miss creates exactly one PSO, never two.
2. **Render owns residency, not authoring.** The cache never
   compiles HLSL or transcodes AIR; that is `shader`'s job
   (§3.3). A miss whose `shader_hash` is unknown returns
   `render::Error::PipelineCompileFailed` rather than invoking
   a compiler.
3. **Eviction is bounded.** The cache size cap comes from
   `RenderSettings`; eviction is LRU and reports back via
   `DiagnosticOverlay`. A pass that requests an evicted PSO
   re-promotes it before recording.
4. **Hot-reload-safe.** When `shader` swaps a metallib, the
   cache is invalidated by `shader_hash` change rather than by
   pointer fixup; old entries linger only until their last
   in-flight frame retires, then drop.

#### 4.1.8 `RTAccelStructures` — BLAS/TLAS lifecycle (entity)

**Reason to change:** how acceleration structures are refit
and rebuilt for ray-traced shadows / AO / reflections.

**Composition.** A registry of currently-resident BLAS handles
imported from `geometry` (one per cooked mesh; render does
**not** build BLAS — that is `geometry`'s cook job per §3.3); a
single TLAS per `View` rebuilt or refit each frame from the
visible-set extracted into `RenderFrame`; per-instance
transform + material-index buffers feeding the TLAS; and the
fence indicating BLAS-refit completion that the TLAS-build pass
waits on.

**Public-boundary invariants.**

1. **BLAS-refit before TLAS-build, every frame.** When a BLAS
   has dynamic vertex data (skinned mesh, deformable),
   render's `BLASRefitPass` records its refit on
   `Queue::Compute` and the `TLASBuildPass` declares an explicit
   read-after-write on the same resource; the compiler emits
   the cross-queue fence. No TLAS may be consumed by an RT
   pass without that fence retired.
2. **TLAS rebuilt-or-refit per frame.** The choice between
   refit (visible-set membership stable, only transforms
   changed) and full rebuild (membership churn beyond a
   threshold) is a compile-time decision based on
   `RenderFrame` deltas; either path completes inside phase 7
   before the first RT trace pass.
3. **BLAS imports are read-only.** Render never mutates BLAS
   storage; refit kernels write to a render-owned scratch
   buffer plus the BLAS's update slot per the Metal 4 RT API,
   not to BLAS-static memory.
4. **TLAS lifetime = persistent, contents = transient.** The
   TLAS buffer is a persistent `Resource`; its contents are
   regenerated each frame and never read across the frame
   boundary.

#### 4.1.9 `HZB` — hierarchical Z-buffer for two-phase occlusion (entity)

**Reason to change:** the occlusion algorithm used by phase-6
culling. Bounded; doesn't drag in lighting or RT changes.

**Composition.** A persistent depth pyramid sized to the active
view's render extent (mip-chain min/max); two backing
allocations triple-buffered so frame N's HZB-build (writing
the post-mesh-shader depth) does not race frame N+1's
HZB-read (the cull-extract phase consuming it). Owned by the
render plugin; produced by the post-gbuffer `HZBBuildPass`,
consumed by the next frame's `OcclusionCullPass`.

**Public-boundary invariants.**

1. **Two-phase symmetry.** The HZB is read in phase 6 (cull
   against last frame's HZB), then written at end of phase 7
   (after the mesh-shader gbuffer pass) for next frame's
   consumption. Phase ordering — never reversed.
2. **Persistent across frames, transient across views.** Each
   `View` has its own HZB pyramid; reflection-probe and shadow
   views own theirs; no cross-view aliasing.
3. **Reverse-Z respected.** All HZB ops use the same reverse-Z
   convention as the gbuffer pass; the convention lives in one
   header (no per-pass override), consistent with the §3.1
   core-raster derivation.

#### 4.1.10 `ClusterCullState` — persistent-thread clustered-light cull state (entity)

**Reason to change:** the clustered-light-culling algorithm —
froxel layout, persistent-thread kernel, atomic-list compaction.

**Composition.** The persistent-thread compute kernel handle
(via `PSOCache`), the per-view froxel grid descriptor (cluster
count XYZ, near/far slicing policy from `RenderSettings`),
the persistent scratch buffers (per-cluster light index list
heads, atomic counters, compacted indices) sized at init time
from `QualityTier`, and the dispatch parameters consumed by
the `ClusterCullPass`. Outputs the `LightCluster` resource
declared as a `VirtualResource` in the graph and consumed by
the deferred-lighting pass and the transparent-forward pass
(§3.2 collapse #3).

**Public-boundary invariants.**

1. **One state per `View`.** Multi-view (split-screen, VR,
   reflection probes) instantiates one `ClusterCullState`
   per `View`; cull state never aliases across views even
   when extents match.
2. **Persistent-thread invariants.** Workgroup count and
   per-thread workload are fixed at init from `QualityTier`;
   shaders never branch on tier in the hot path
   (PHILOSOPHY §6).
3. **Atomic compaction is the only mutation point.**
   `ClusterCullPass` writes the compacted index buffer with a
   single atomic-counter pass; deferred and forward consumers
   read-only.
4. **State outlives any one frame, output does not.** The
   scratch buffers are persistent; the `LightCluster` virtual
   resource feeding lighting is transient and aliases per
   `AliasPlan`.

### 4.2 Cross-aggregate invariants

Invariants that span more than one aggregate and must hold at every
public boundary at the seams between them:

1. **Graph compile is total.** `RenderGraph::compile()` either
   returns a fully-formed `ExecutionPlan` honouring barrier-
   minimality, alias-correctness, and acyclicity, or returns a
   typed `render::Error` (`RenderGraphCycle`,
   `PassUnsupportedConfig`, `ResourceResidencyExceeded`). No
   partial plan is observable outside the compiler.
2. **Transient resources never outlive a frame.** Transient
   `VirtualResource`s declared during graph build of frame N are
   destroyed (their `PhysicalAllocation` recycled into the
   `TransientPool`) at the moment phase 7 of frame N exits.
   Persistent resources never enter the alias plan.
3. **`PSOKey` is `(shader_hash, state_hash)`.** Both halves are
   required; both halves participate in the cache lookup; an
   identical key must map to byte-equal pipeline bytecode.
   Render never compiles shaders, never invents a state hash
   that does not include all PSO-relevant Metal pipeline
   descriptor fields.
4. **BLAS-refit precedes TLAS-build, every frame.** Any RT pass
   reading the TLAS sees a TLAS whose constituent BLAS
   resources have been refit (or rebuilt at cook time and not
   yet invalidated) and whose refit fence is retired. Violation
   is a compile-time error in `RenderGraph`; runtime ordering
   is enforced by Metal 4 fences emitted by the plan compiler.
5. **Mesh-shader gbuffer pass writes gbuffer + velocity +
   visibilityID atomically.** The single mesh-shader dispatch
   that produces opaque coverage writes all four MRT targets
   (albedo+metallic, normal+roughness, motion / velocity, the
   visibilityID buffer) plus depth in one declared `Pass` with
   one barrier-emit point. Splitting these writes across
   passes is rejected by the compiler so downstream consumers
   (HZB-build, deferred-lighting, RT shadow primary-ray fallback)
   never observe a half-written gbuffer.
6. **`RenderFrame` is the only ECS↔GPU seam.** No aggregate
   outside `RenderFrame` may hold a pointer to ECS storage; no
   aggregate inside phase 7 may call back into ECS systems.
7. **Per-context error model honoured.** Every aggregate's
   public fallible operation returns
   `glibre::Result<T, glibre::Error>` per
   `reviews/decisions/error-model.md`; render's enum lives in
   the `render::Error` arm cited there and is the only
   render-internal error surface.
8. **Frame-phase ownership.** The aggregates above are
   instantiated and destroyed inside the phases declared by
   `reviews/decisions/frame-phases.md`: `RenderFrame` is built
   in phase 6 and read in phase 7; `RenderGraph`,
   `ExecutionPlan`, `Pass`, transient `Resource`,
   `MetalCommandBuffer`, `RTAccelStructures` updates,
   `ClusterCullState` dispatch, and `HZB` write live inside
   phase 7; `MetalDevice`, persistent `Resource`, `PSOCache`,
   `HZB` storage, and `ClusterCullState` storage live across
   frames but are mutated only inside phase 7. No aggregate is
   mutated inside phases 1–5 or phase 9.

## 5. Public Interface

The header stub below is the §5 deliverable: every symbol that crosses
the render plugin's public boundary, declared in one C++23 header and
verified via `clang++ -std=c++23 -fsyntax-only -Wall -Wextra -Wpedantic`.
Bodies live inside the render dylib; this header is the contract every
caller (core, platform, editor, downstream plugins) compiles against.
Cross-context invariants enforced here:

- Every fallible operation returns `glibre::Result<T>` per
  `reviews/decisions/error-model.md`. The render-internal `Error` enum is
  the closed sum cited in §10 below; it is rolled into `glibre::Error`'s
  variant in `core`.
- Aggregates listed in §4 (`RenderFrame`, `RenderGraph`, `Pass`,
  `ExecutionPlan`, `MetalDevice`, `MetalQueue`, `MetalCommandBuffer`,
  `PSOCache`, `RTAccelStructures`, `HZB`, `ClusterCullState`,
  `TransientPool`, `DiagnosticOverlay`) are forward-declared classes whose
  layout is owned inside the plugin. Callers manipulate them only through
  the methods exposed below.
- Resource handles are 64-bit generational `Handle<Tag>` values with no
  payload pointers; this avoids ABI fixup on hot-reload (PHILOSOPHY §8 +
  §9). The tag types are empty structs so handles addressing different
  aggregates are distinct types and cannot be cross-assigned.
- Capability flags (`Capability`, `CapabilitySet`) are queried at init
  time and consulted by the graph builder's pass predicates; shader hot
  paths never branch on them (PHILOSOPHY §6, §3.2 collapse #5/#9).
- Per-pass execute lambdas receive only `MetalCommandBuffer&` and the
  resolved `Bindings&`; they may not allocate, may not record on the
  wrong queue, and may not touch undeclared resources (§4.1.3).

The header has no event types in MVP — render publishes nothing back into
the ECS event bus; the snapshot bus delivers `RenderFrame` by reference
and the present fence is read by `platform`'s phase 9 directly. No Fory
schemas live in this surface either: `RenderFrame` is allocated in the
per-frame arena and never serialised (PHILOSOPHY anti-pattern), and the
`ExecutionPlan` cache is keyed by structural hash, not by file. Render's
contribution to telemetry is the structured per-context error enum
returned through `Result<T>` and consumed by `glibre::log_error`.

```cpp
// SPDX-License-Identifier: Apache-2.0
// glibre — render plugin public interface (header-only stub).
//
// This file is the §5 deliverable of `specs/render/SPEC.md`. It declares
// every symbol crossing the render plugin's public boundary. The bodies
// live inside the render dylib; this header is the contract every caller
// (core, platform, editor) compiles against.
//
// Cross-context invariants embedded here:
//   * Every fallible call returns `glibre::Result<T>` per
//     `reviews/decisions/error-model.md`. `-fno-exceptions` is enforced
//     globally; this header obeys.
//   * Aggregates are opaque — `RenderFrame`, `ExecutionPlan`,
//     `MetalDevice`, `MetalQueue`, `MetalCommandBuffer`, etc. are
//     forward-declared classes whose layout is owned inside the plugin.
//   * Resource handles are 64-bit generational `Handle<Tag>` values
//     with no payload pointers; this avoids ABI fixup on hot-reload.
//   * Capability flags are compile-time-stable; gating decisions happen
//     at graph-build time, never inside shader hot paths.
//
// This stub compiles standalone with
// `clang++ -std=c++23 -fsyntax-only -Wall -Wextra -Wpedantic`.

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

namespace glibre {

// -----------------------------------------------------------------------
// Stand-in declarations from sibling contexts. The real definitions live
// in `core/include/glibre/error.hpp`, `geometry/include/...`, etc.; this
// header forward-declares them so the stub compiles in isolation. The
// implementation .cpp files include the real headers, not these stubs.
// -----------------------------------------------------------------------

#if !defined(GLIBRE_HAVE_CORE_ERROR)
namespace core {
enum class Error : std::uint16_t {
    PluginAbiHashMismatch,
    PluginInitFailed,
    SchemaMigrationFailed,
    HotReloadRefused,
    FramePhaseMisordered,
    OutOfBudget,
};
}  // namespace core

struct ErrorContext {
    std::string_view file;
    int              line  = 0;
    std::string_view detail;
};

class Error {
public:
    using Variant = std::variant<core::Error /*, render::Error inserted in core */>;

    template <class E>
    constexpr Error(E e, ErrorContext ctx = {}) noexcept
        : variant_{e}, ctx_{ctx} {}

    constexpr const Variant&      code() const noexcept  { return variant_; }
    constexpr const ErrorContext& where() const noexcept { return ctx_; }

private:
    Variant      variant_;
    ErrorContext ctx_;
};

template <class T>
using Result = std::expected<T, Error>;
#endif  // GLIBRE_HAVE_CORE_ERROR

// -----------------------------------------------------------------------
// render::Error — closed sum of every render-internal failure mode.
// Every public render boundary returns Result<T> over this enum (rolled
// into glibre::Error's variant per reviews/decisions/error-model.md).
// The list is closed: adding a variant is an ABI bump.
// -----------------------------------------------------------------------

namespace render {

enum class Error : std::uint16_t {
    // Device / queue lifecycle
    DeviceLost,
    DeviceUnsupported,
    QueueSubmitFailed,
    FenceTimeout,

    // Pipeline / PSO cache
    PipelineCompileFailed,
    PipelineCacheMiss,
    UnsupportedBackend,

    // Resource residency / aliasing
    ResourceResidencyExceeded,
    ResourceImportRefused,
    TransientPoolExhausted,
    HeapOutOfMemory,

    // Graph compile
    RenderGraphCycle,
    PassUnsupportedConfig,
    PassDeclaredUseUnused,
    PassUndeclaredAccess,
    BarrierConflict,

    // RT-accel structures
    BlasUnavailable,
    TlasBuildFailed,

    // Swapchain / present
    SwapchainAcquireFailed,
    SwapchainOutOfDate,
    PresentFailed,

    // Capability gating
    CapabilityNotSupported,
};

[[nodiscard]] constexpr std::string_view to_string(Error e) noexcept;

// -----------------------------------------------------------------------
// Capability flags — init-time queried, never branched on in shader hot
// paths (PHILOSOPHY §6). Used by the graph builder's pass predicates and
// by RenderSettings to gate optional features.
// -----------------------------------------------------------------------

enum class Capability : std::uint32_t {
    None              = 0u,
    MeshShaders       = 1u << 0,
    RayQuery          = 1u << 1,
    HardwareRayTrace  = 1u << 2,
    BindlessResources = 1u << 3,
    ResidencySets     = 1u << 4,
    TimestampQueries  = 1u << 5,
    HdrPresent        = 1u << 6,
    DolbyVision       = 1u << 7,
    VariableRate      = 1u << 8,
    MetalFx           = 1u << 9,
};

[[nodiscard]] constexpr Capability operator|(Capability a, Capability b) noexcept {
    using U = std::underlying_type_t<Capability>;
    return static_cast<Capability>(static_cast<U>(a) | static_cast<U>(b));
}
[[nodiscard]] constexpr Capability operator&(Capability a, Capability b) noexcept {
    using U = std::underlying_type_t<Capability>;
    return static_cast<Capability>(static_cast<U>(a) & static_cast<U>(b));
}
[[nodiscard]] constexpr bool has(Capability set, Capability bit) noexcept {
    using U = std::underlying_type_t<Capability>;
    return (static_cast<U>(set) & static_cast<U>(bit)) != 0u;
}

struct CapabilitySet {
    Capability flags = Capability::None;
    [[nodiscard]] constexpr bool supports(Capability c) const noexcept { return has(flags, c); }
};

// -----------------------------------------------------------------------
// Generational handles. 64 bits, packed { generation : 24, index : 40 }.
// Tag types are empty structs so handles to different aggregates are
// distinct types and cannot be cross-assigned.
// -----------------------------------------------------------------------

namespace tags {
struct mesh                   {};
struct material               {};
struct view                   {};
struct render_layer           {};
struct pipeline_state         {};
struct argument_buffer        {};
struct virtual_resource       {};
struct physical_allocation    {};
struct blas                   {};
struct tlas                   {};
struct hzb                    {};
struct cluster_cull_state     {};
struct shadow_atlas           {};
struct ring_slice             {};
struct frame                  {};
struct render_settings        {};
}  // namespace tags

template <class Tag>
class Handle {
public:
    using value_type = std::uint64_t;

    constexpr Handle() noexcept = default;
    explicit constexpr Handle(value_type v) noexcept : bits_{v} {}

    [[nodiscard]] constexpr value_type    raw()        const noexcept { return bits_; }
    [[nodiscard]] constexpr std::uint64_t index()      const noexcept { return bits_ & 0x000000FF'FFFFFFFFull; }
    [[nodiscard]] constexpr std::uint32_t generation() const noexcept { return static_cast<std::uint32_t>(bits_ >> 40); }
    [[nodiscard]] constexpr bool          valid()      const noexcept { return bits_ != 0u; }
    [[nodiscard]] friend constexpr bool operator==(Handle, Handle) noexcept = default;

private:
    value_type bits_ = 0u;
};

using MeshHandle             = Handle<tags::mesh>;
using MaterialHandle         = Handle<tags::material>;
using ViewHandle             = Handle<tags::view>;
using RenderLayerMask        = Handle<tags::render_layer>;
using PSOHandle              = Handle<tags::pipeline_state>;
using ArgumentBufferHandle   = Handle<tags::argument_buffer>;
using VirtualResourceHandle  = Handle<tags::virtual_resource>;
using PhysicalAllocHandle    = Handle<tags::physical_allocation>;
using BLASHandle             = Handle<tags::blas>;
using TLASHandle             = Handle<tags::tlas>;
using HZBHandle              = Handle<tags::hzb>;
using ClusterCullStateHandle = Handle<tags::cluster_cull_state>;
using ShadowAtlasHandle      = Handle<tags::shadow_atlas>;
using RingSliceHandle        = Handle<tags::ring_slice>;
using FrameHandle            = Handle<tags::frame>;
using RenderSettingsHandle   = Handle<tags::render_settings>;

// -----------------------------------------------------------------------
// Quality + render settings (SPEC §3.2 collapse #9).
// -----------------------------------------------------------------------

enum class QualityTier : std::uint8_t { Mobile, Switch, Desktop, HighEnd };

enum class AntiAliasMode : std::uint8_t {
    Off,
    Fxaa,
    Smaa,
    Taa,
    TemporalSuper,   // TSR / DLSS / FSR / XeSS slot — vendor selected at init.
};

enum class UpscalerMode : std::uint8_t {
    Off,
    BuiltinFallback,
    MetalFx,
    Vendor,
};

enum class ShadowTier : std::uint8_t { None, Pcf, Pcss, RayTraced };
enum class AmbientOcclusionTier : std::uint8_t { None, Ssao, Gtao, RayTraced };

struct DynamicResolutionBounds {
    float min_scale = 0.5f;
    float max_scale = 1.0f;
};

struct RenderSettings {
    AntiAliasMode           aa_mode              = AntiAliasMode::Taa;
    UpscalerMode            upscaler             = UpscalerMode::BuiltinFallback;
    ShadowTier              shadows              = ShadowTier::Pcf;
    AmbientOcclusionTier    ao                   = AmbientOcclusionTier::Ssao;
    bool                    ray_tracing_enable   = false;
    bool                    hdr_output           = false;
    DynamicResolutionBounds dynamic_resolution{};
    std::uint32_t           per_view_draw_budget = 0u;  // 0 = no budget cull.
};

// -----------------------------------------------------------------------
// Aggregates — opaque to the public interface. Implementations live
// inside the render dylib. Callers manipulate them only through the
// methods exposed here.
// -----------------------------------------------------------------------

class RenderFrame;        // §4.1.1 — immutable per-frame extract.
class RenderGraph;        // §4.1.2 — DAG of Pass nodes.
class GraphBuilder;       // §4.1.2 fluent API — declared below.
class Pass;               // §4.1.3 — typed read/write declaration + execute lambda.
class ExecutionPlan;      // §4.1.5 — compiled graph output.
class MetalDevice;        // §4.1.6 — engine-singleton metal-cpp wrapper.
class MetalQueue;         // §4.1.6 — per-role queue.
class MetalCommandBuffer; // §4.1.6 — frame-scoped command buffer.
class PSOCache;           // §4.1.7 — pipeline-state residency cache.
class RTAccelStructures;  // §4.1.8 — BLAS / TLAS lifecycle.
class HZB;                // §4.1.9 — hierarchical Z-buffer.
class ClusterCullState;   // §4.1.10 — clustered light cull state.
class TransientPool;      // §4.1.4 — placement heap pool.
class DiagnosticOverlay;  // §3.2 collapse #10 — DAG visualiser.

// -----------------------------------------------------------------------
// VirtualResource — value object describing a logical render target /
// buffer in the graph. The alias planner maps it to a PhysicalAllocation.
// -----------------------------------------------------------------------

enum class ResourceUsage : std::uint32_t {
    None             = 0u,
    SampledTexture   = 1u << 0,
    StorageTexture   = 1u << 1,
    ColorAttachment  = 1u << 2,
    DepthAttachment  = 1u << 3,
    StorageBuffer    = 1u << 4,
    UniformBuffer    = 1u << 5,
    IndirectBuffer   = 1u << 6,
    AccelStructure   = 1u << 7,
    PresentTarget    = 1u << 8,
};
[[nodiscard]] constexpr ResourceUsage operator|(ResourceUsage a, ResourceUsage b) noexcept {
    using U = std::underlying_type_t<ResourceUsage>;
    return static_cast<ResourceUsage>(static_cast<U>(a) | static_cast<U>(b));
}

enum class ResourceFormat : std::uint16_t {
    Unknown,
    Rgba8Unorm,
    Rgba16Float,
    Rgba32Float,
    Depth32Float,
    Depth24UnormStencil8,
    R32Uint,
    R16Float,
    Bgra8UnormSrgb,
    // … extended at PSO authoring time; closed list owned by render.
};

enum class ResourceLifetime : std::uint8_t {
    Transient,   // alias-eligible inside a single frame.
    Persistent,  // outlives a frame; never aliased.
    Imported,    // borrowed from another context (geometry / vfx / shader / platform).
};

struct ResourceDesc {
    std::string_view debug_name;
    ResourceFormat   format       = ResourceFormat::Unknown;
    std::uint32_t    width        = 0u;
    std::uint32_t    height       = 0u;
    std::uint32_t    depth        = 1u;
    std::uint32_t    mip_levels   = 1u;
    std::uint32_t    array_layers = 1u;
    std::uint32_t    sample_count = 1u;
    ResourceUsage    usage        = ResourceUsage::None;
    ResourceLifetime lifetime     = ResourceLifetime::Transient;
};

enum class AccessKind : std::uint8_t {
    Read,
    Write,
    ReadWrite,
};

enum class Queue : std::uint8_t {
    Graphics,
    Compute,
    Copy,
};

// -----------------------------------------------------------------------
// PassPriority — used by the cost-aware budget culler (§2 ubiquitous
// language). Passes with higher numeric priority drop first when the
// previous frame's GPU timing exceeds the configured frame budget.
// -----------------------------------------------------------------------

enum class PassPriority : std::uint16_t {
    Mandatory       = 0,
    HighQuality     = 100,
    StandardQuality = 200,
    LowQuality      = 300,
    Optional        = 400,
};

// -----------------------------------------------------------------------
// Pass execute signature — invoked by the plan recorder during phase 7.
// `Bindings` is opaque (resolved argument-buffer offsets); the lambda
// never touches raw heap addresses.
// -----------------------------------------------------------------------

struct Bindings;  // opaque; bodies recover typed views from it.

using PassExecuteFn =
    std::function<Result<void>(MetalCommandBuffer&, const Bindings&) /* noexcept */>;

// -----------------------------------------------------------------------
// GraphBuilder — fluent surface plugins use to declare passes. Per-pass
// invariants (declared = used, queue purity, no allocations on the hot
// path) are checked at compile() time and at execute() in debug builds.
// -----------------------------------------------------------------------

struct PassDesc {
    std::string_view name;
    Queue            queue         = Queue::Graphics;
    PassPriority     priority      = PassPriority::StandardQuality;
    Capability       requires_caps = Capability::None;  // unset bits ⇒ compile-time skip.
};

class GraphBuilder {
public:
    GraphBuilder() = delete;  // obtained via RenderGraph::begin(...).
    GraphBuilder(const GraphBuilder&) = delete;
    GraphBuilder& operator=(const GraphBuilder&) = delete;

    // Resource declarations — return handles consumed by add_*_pass().
    [[nodiscard]] Result<VirtualResourceHandle>
        declare_transient(const ResourceDesc&) noexcept;

    [[nodiscard]] Result<VirtualResourceHandle>
        declare_persistent(const ResourceDesc&) noexcept;

    [[nodiscard]] Result<VirtualResourceHandle>
        declare_imported(const ResourceDesc&, PhysicalAllocHandle) noexcept;

    // Pass builders. Each takes a span of (resource, access) edges plus
    // the execute lambda. The compiler enforces SPEC §4 invariants:
    // declared-set closure, queue purity, atomic side-effect set.
    struct ResourceAccess {
        VirtualResourceHandle resource;
        AccessKind            access;
    };

    [[nodiscard]] Result<void>
        add_raster_pass(const PassDesc&,
                        std::span<const ResourceAccess> reads,
                        std::span<const ResourceAccess> writes,
                        PassExecuteFn                   execute) noexcept;

    [[nodiscard]] Result<void>
        add_compute_pass(const PassDesc&,
                         std::span<const ResourceAccess> reads,
                         std::span<const ResourceAccess> writes,
                         PassExecuteFn                   execute) noexcept;

    // Ray-trace pass — RT-shadow / AO / reflection bodies declare TLAS
    // input + shadow / AO / reflection target outputs. Rejected with
    // `CapabilityNotSupported` if `Capability::HardwareRayTrace` is
    // missing from the device CapabilitySet at build time.
    [[nodiscard]] Result<void>
        add_rt_pass(const PassDesc&,
                    TLASHandle                       tlas,
                    std::span<const ResourceAccess>  reads,
                    std::span<const ResourceAccess>  writes,
                    PassExecuteFn                    execute) noexcept;

private:
    // Constructed by RenderGraph::begin(); body in render dylib.
    GraphBuilder(RenderGraph&) noexcept;
    friend class RenderGraph;
    RenderGraph* graph_ = nullptr;
};

// -----------------------------------------------------------------------
// RenderGraph — built per (View, FrameCounter) pair. begin() vends the
// fluent builder; compile() returns the cached or freshly produced
// ExecutionPlan. Compile is total: success ⇒ ExecutionPlan, failure ⇒
// typed render::Error (RenderGraphCycle / PassUnsupportedConfig /
// ResourceResidencyExceeded / BarrierConflict).
// -----------------------------------------------------------------------

class RenderGraph {
public:
    [[nodiscard]] static Result<std::unique_ptr<RenderGraph>>
        create(MetalDevice&, ViewHandle) noexcept;

    [[nodiscard]] GraphBuilder begin(const RenderFrame&) noexcept;

    [[nodiscard]] Result<const ExecutionPlan*>
        compile(CapabilitySet) noexcept;

    [[nodiscard]] ViewHandle view() const noexcept;

    ~RenderGraph();
    RenderGraph(const RenderGraph&)            = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

protected:
    RenderGraph() noexcept;
};

// -----------------------------------------------------------------------
// PSOCache — keyed by (shader_hash, state_hash). Render owns residency,
// not authoring (`shader` plugin compiles HLSL → AIR / metallib).
// -----------------------------------------------------------------------

struct PSOKey {
    std::uint64_t shader_hash = 0u;
    std::uint64_t state_hash  = 0u;

    [[nodiscard]] friend constexpr bool operator==(PSOKey, PSOKey) noexcept = default;
};

struct PSOKeyHash {
    [[nodiscard]] constexpr std::size_t operator()(PSOKey k) const noexcept {
        return std::rotl(k.shader_hash, 21) ^ k.state_hash;
    }
};

class PSOCache {
public:
    [[nodiscard]] Result<PSOHandle> get(PSOKey) noexcept;
    [[nodiscard]] Result<void>      warm(std::span<const PSOKey>) noexcept;

    void invalidate_by_shader_hash(std::uint64_t shader_hash) noexcept;
    void evict_lru(std::size_t target_size) noexcept;

protected:
    PSOCache() noexcept = default;
    ~PSOCache() = default;
    PSOCache(const PSOCache&)            = delete;
    PSOCache& operator=(const PSOCache&) = delete;
};

// -----------------------------------------------------------------------
// Metal wrappers — opaque handles into the render dylib. Real bodies
// hold `MTL::Device*` / `MTL::CommandQueue*` / `MTL::CommandBuffer*`.
// -----------------------------------------------------------------------

struct DeviceDesc {
    bool        prefer_low_power = false;
    QualityTier tier             = QualityTier::Desktop;
};

class MetalDevice {
public:
    [[nodiscard]] static Result<std::unique_ptr<MetalDevice>>
        create(const DeviceDesc&) noexcept;

    [[nodiscard]] CapabilitySet capabilities() const noexcept;
    [[nodiscard]] MetalQueue&    queue(Queue) noexcept;
    [[nodiscard]] PSOCache&      pso_cache() noexcept;
    [[nodiscard]] TransientPool& transient_pool() noexcept;

    ~MetalDevice();
    MetalDevice(const MetalDevice&)            = delete;
    MetalDevice& operator=(const MetalDevice&) = delete;

protected:
    MetalDevice() noexcept;
};

class MetalQueue {
public:
    [[nodiscard]] Result<std::unique_ptr<MetalCommandBuffer>>
        acquire_command_buffer() noexcept;

    [[nodiscard]] Result<void> submit(MetalCommandBuffer&) noexcept;

    [[nodiscard]] Queue role() const noexcept;

protected:
    MetalQueue() noexcept = default;
    ~MetalQueue()         = default;
    MetalQueue(const MetalQueue&)            = delete;
    MetalQueue& operator=(const MetalQueue&) = delete;
};

class MetalCommandBuffer {
public:
    [[nodiscard]] Result<void> push_debug_group(std::string_view name) noexcept;
    [[nodiscard]] Result<void> pop_debug_group() noexcept;

    [[nodiscard]] Queue queue_role() const noexcept;

    ~MetalCommandBuffer();
    MetalCommandBuffer(const MetalCommandBuffer&)            = delete;
    MetalCommandBuffer& operator=(const MetalCommandBuffer&) = delete;

protected:
    MetalCommandBuffer() noexcept;
};

// -----------------------------------------------------------------------
// RTAccelStructures — BLAS imports + per-View TLAS lifecycle.
// -----------------------------------------------------------------------

class RTAccelStructures {
public:
    [[nodiscard]] Result<void>
        register_blas(BLASHandle, std::uint64_t version) noexcept;

    [[nodiscard]] Result<TLASHandle>
        ensure_tlas(ViewHandle, const RenderFrame&) noexcept;

    [[nodiscard]] Result<void>
        submit_blas_refit(MetalCommandBuffer&, BLASHandle) noexcept;

protected:
    RTAccelStructures() noexcept = default;
    ~RTAccelStructures()         = default;
    RTAccelStructures(const RTAccelStructures&)            = delete;
    RTAccelStructures& operator=(const RTAccelStructures&) = delete;
};

// -----------------------------------------------------------------------
// HZB + ClusterCullState — persistent-thread compute state per View.
// -----------------------------------------------------------------------

struct HZBDesc {
    std::uint32_t width  = 0u;
    std::uint32_t height = 0u;
    std::uint32_t mips   = 0u;
};

class HZB {
public:
    [[nodiscard]] Result<HZBHandle> ensure(ViewHandle, HZBDesc) noexcept;
    void invalidate(ViewHandle) noexcept;

protected:
    HZB()  noexcept = default;
    ~HZB() = default;
    HZB(const HZB&)            = delete;
    HZB& operator=(const HZB&) = delete;
};

struct ClusterDesc {
    std::uint32_t cluster_count_x = 16u;
    std::uint32_t cluster_count_y = 9u;
    std::uint32_t cluster_count_z = 24u;
    float         near_plane      = 0.1f;
    float         far_plane       = 1000.0f;
};

class ClusterCullState {
public:
    [[nodiscard]] Result<ClusterCullStateHandle>
        ensure(ViewHandle, ClusterDesc, QualityTier) noexcept;

protected:
    ClusterCullState()  noexcept = default;
    ~ClusterCullState() = default;
    ClusterCullState(const ClusterCullState&)            = delete;
    ClusterCullState& operator=(const ClusterCullState&) = delete;
};

// -----------------------------------------------------------------------
// RenderFrame — opaque to all callers. The producer (phase 6) emits one
// via the snapshot bus; the consumer (phase 7) takes a const reference.
// -----------------------------------------------------------------------

class RenderFrame {
public:
    [[nodiscard]] std::uint64_t              frame_counter() const noexcept;
    [[nodiscard]] std::span<const ViewHandle> views()        const noexcept;

    ~RenderFrame();
    RenderFrame(const RenderFrame&)            = delete;
    RenderFrame& operator=(const RenderFrame&) = delete;

protected:
    RenderFrame() noexcept;
};

// -----------------------------------------------------------------------
// ExecutionPlan — opaque compiled graph; recorded by record_into().
// -----------------------------------------------------------------------

class ExecutionPlan {
public:
    [[nodiscard]] Result<void>
        record_into(MetalCommandBuffer&) const noexcept;

    [[nodiscard]] std::size_t   pass_count()      const noexcept;
    [[nodiscard]] std::size_t   barrier_count()   const noexcept;
    [[nodiscard]] std::uint64_t structural_hash() const noexcept;

    ~ExecutionPlan();
    ExecutionPlan(const ExecutionPlan&)            = delete;
    ExecutionPlan& operator=(const ExecutionPlan&) = delete;

protected:
    ExecutionPlan() noexcept;
};

// -----------------------------------------------------------------------
// Top-level entry — invoked from the core frame loop at phase 7. Takes
// a frozen RenderFrame; produces one submitted command buffer per View
// and signals a PresentFence consumed by `platform`'s phase 9.
// -----------------------------------------------------------------------

struct PresentFence {
    std::uint64_t value = 0u;  // monotonically increasing per-queue.
};

[[nodiscard]] Result<PresentFence>
    submit_frame(MetalDevice&, const RenderFrame&) noexcept;

}  // namespace render
}  // namespace glibre
```

**Event types.** Render publishes no events back into the ECS event bus
in MVP; the snapshot bus delivers `RenderFrame` by reference and the
present fence is read by `platform`'s phase 9 (`PresentFence` value
above). The `DiagnosticOverlay` exposes a debug-only event channel for
the editor; that surface ships behind a build-time gate and is therefore
omitted from this header.

**Serialised schemas (Fory).** None at this layer. `RenderFrame` lives
in the per-frame arena and is never serialised (PHILOSOPHY anti-pattern
"serialised render-graph files"). `ExecutionPlan`s are cached by
in-memory structural hash; they do not persist across process lifetimes.
PSO blob serialisation is owned by `shader`'s cook (§3.3).

**Error types.** The closed sum `render::Error` declared above lists
every failure mode at every public render boundary. It is the §10
authority for failure-mode enumeration; new variants require an ABI bump
per `reviews/decisions/error-model.md`.

## 6. Internal Architecture

Non-binding sketch for implementers.

## 7. Persistence & Schemas

Render's persistence surface is intentionally narrow: only **operational
state** that must survive across process lifetimes is persisted. Per-frame
artefacts (`RenderFrame`, `ExecutionPlan`, `RenderGraph`, transient
allocations, `RenderProxy` SoA, `LightCluster`, `HZB`, ring slices, GPU
timestamp rings) are runtime-only and never serialised, per the
PHILOSOPHY anti-pattern "serialised render-graph files" and the §5
"Serialised schemas (Fory) — None at this layer" boundary statement.
Render does **not** own shader bytecode persistence — that is `shader`'s
cook-time artefact (§3.3); render owns the **device-resident** pipeline
archive that warms the runtime `PSOCache`.

All schemas below are authored as `data/schemas/render/<Type>.fory`
files per `reviews/decisions/fory-codegen.md` and compile into the
`glibre-types` middleman dylib. FQNs are `glibre.render.<Type>`. Each
schema ships with at least one Catch2 round-trip test under
`tests/data/schemas/render/<Type>.cpp` per the data SPEC §7 mandate.

### 7.1 Persistent types

#### 7.1.1 `RenderSettings` — per-view feature configuration

**File:** `data/schemas/render/RenderSettings.fory`
**FQN:** `glibre.render.RenderSettings`
**Lifetime scope:** per-user, per-view; written by the editor / settings
UI, read at view-creation time inside the render plugin.

```fory
schema glibre.render.RenderSettings {
  version  1
  since    "0.1.0"

  field aa_mode              : u8   tag 1 since 1                  // AntiAliasMode
  field upscaler             : u8   tag 2 since 1                  // UpscalerMode
  field shadows              : u8   tag 3 since 1                  // ShadowTier
  field ao                   : u8   tag 4 since 1                  // AmbientOcclusionTier
  field ray_tracing_enable   : u8   tag 5 since 1   default 0
  field hdr_output           : u8   tag 6 since 1   default 0
  field dyn_res_min_scale    : f32  tag 7 since 1   default 0.5
  field dyn_res_max_scale    : f32  tag 8 since 1   default 1.0
  field per_view_draw_budget : u32  tag 9 since 1   default 0      // 0 = unbounded
}
```

The Fory enum-as-`u8` encoding is intentional: the `AntiAliasMode` /
`UpscalerMode` / `ShadowTier` / `AmbientOcclusionTier` enumerator lists
declared in §5 are **closed sums** (§10 ABI bump rule). A loaded value
outside the current closed set decodes to `Error::SchemaMigrationFailure`
rather than silently saturating to a neighbour, so settings authored by
a future build cannot smuggle unknown effect chains into an older render
plugin.

**Invariants** (echoing §4.x where the runtime aggregate enforces them):

1. `dyn_res_min_scale ∈ (0, 1]` and `dyn_res_min_scale ≤ dyn_res_max_scale ≤ 1`.
2. `ray_tracing_enable` ⇒ host capability set (§7.1.3) contains
   `HardwareRayTrace` or `RayQuery` at view creation; otherwise the
   loader downgrades to `false` and emits a `DiagnosticOverlay`
   warning. The loader **does not** fail; capability mismatches between
   the persisted settings document and the current host are an expected
   steady-state condition (e.g. moving a settings file between
   machines).
3. `upscaler == MetalFx` ⇒ host capability set contains `MetalFx`;
   otherwise downgrade to `BuiltinFallback` with the same diagnostic.
4. `(aa_mode, upscaler, shadows, ao)` combinations refused by the graph
   builder's pass predicates (§4.1.2 / §4.1.3) trigger
   `Error::PassUnsupportedConfig` at the first frame after load — not
   at deserialise time, because the graph builder is the single seam
   that knows which pass topologies are reachable on the current host.

#### 7.1.2 `PSOCacheRecord` — device-resident pipeline archive entry

**File:** `data/schemas/render/PSOCacheRecord.fory`
**FQN:** `glibre.render.PSOCacheRecord`
**Lifetime scope:** per-host, per-glibre-version, per-GPU-driver.
Written by the render plugin at clean shutdown; read at startup to
warm the runtime `PSOCache`. Stored under
`<user-data>/render/pso-archive/<host-id>/<glibre-version>/<gpu-id>/`.

**Distinction vs. `shader`'s cook output.** `shader` produces the
**source** AIR/metallib bytecode (the `shader_hash` half of `PSOKey`).
Render's `PSOCacheRecord` persists the **device-compiled** result of
binding that bytecode to a specific `(state_hash, GPU driver)` pair —
i.e. the `MTL::BinaryArchive` blob produced by Metal's pipeline
compiler. This is render's responsibility because the artefact is
device-, driver-, and OS-version-specific; `shader`'s cook is platform-
agnostic by construction (§3.3).

```fory
schema glibre.render.PSOCacheRecord {
  version  1
  since    "0.1.0"

  // Identity — must be byte-equal to the in-memory PSOKey for warm.
  field shader_hash           : u64    tag 1 since 1
  field state_hash            : u64    tag 2 since 1

  // Provenance — used to invalidate the record on host / driver change.
  field gpu_id                : u64    tag 3 since 1   // MTLDevice registryID
  field metal_feature_set     : u32    tag 4 since 1   // MTLGPUFamily ordinal
  field os_build_hash         : u64    tag 5 since 1   // hash of `kern.osversion`
  field glibre_types_abi_hash : u64    tag 6 since 1   // mirrors data §7

  // Payload — opaque MTL::BinaryArchive blob.
  field archive_blob          : bytes  tag 7 since 1
  field archive_blob_blake3   : u64    tag 8 since 1   // truncated blake3
}
```

**Invariants:**

1. **Self-authenticating payload.** `archive_blob_blake3` MUST equal the
   first 8 bytes of `blake3(archive_blob)`. Mismatch → discard the
   record; do NOT propagate as a deserialise error (corrupt PSO records
   are a steady-state condition after crashes during shutdown writes,
   not a programming defect).
2. **Provenance gating.** A record is admissible to the runtime
   `PSOCache` only when **all** of `(gpu_id, metal_feature_set,
   os_build_hash, glibre_types_abi_hash)` match the current host's
   `CapabilityMask` (§7.1.3) and the current `glibre_types_abi_hash`
   exported by the middleman dylib (`reviews/decisions/fory-codegen.md`).
   Mismatched records are **silently discarded**, never migrated. PSO
   archives are **not** migrated across glibre or driver versions; they
   are recompiled from scratch (rebuilds in seconds, vs. minutes for
   the cook).
3. **Whole-archive replacement, not field migration.** Any breaking
   schema change to `PSOCacheRecord`, any change to render's PSO
   state-hash function, any change to `shader`'s cook, any host driver
   upgrade, or any glibre middleman ABI hash bump invalidates **the
   entire archive directory** wholesale. Render's startup warmer treats
   the directory as ephemeral cache and rebuilds on miss.
4. **Eviction is filesystem-driven.** Total on-disk size is capped by
   a fixed 256 MiB budget for MVP (post-MVP wires this to a
   `RenderSettings` sibling field). Eviction uses LRU on record
   `mtime`; the warmer's miss path tolerates an absent record at any
   time without surfacing an error.

#### 7.1.3 `CapabilityMask` — per-host capability record

**File:** `data/schemas/render/CapabilityMask.fory`
**FQN:** `glibre.render.CapabilityMask`
**Lifetime scope:** per-host. Probed once at first run (or on hardware
change), stored at
`<user-data>/render/capability-mask/<host-id>.fory`. Read at startup
**before** any `RenderSettings` is loaded; produces the gating signal
used by §7.1.1 invariants 2-3 and §7.1.2 invariant 2.

```fory
schema glibre.render.CapabilityMask {
  version  1
  since    "0.1.0"

  field host_id            : u64    tag 1  since 1   // hash of (machine UUID, OS install id)
  field probed_at_unix_ms  : u64    tag 2  since 1
  field gpu_id             : u64    tag 3  since 1
  field gpu_name           : string tag 4  since 1
  field metal_feature_set  : u32    tag 5  since 1
  field os_build_hash      : u64    tag 6  since 1

  // Mirrors the `Capability` bitset (§5). Persisted as a single u32
  // so future capability bits flow in via additive bumps, not new
  // fields — see migration rules below.
  field capabilities       : u32    tag 7  since 1

  // Optional richer probes — null until §11 user stories add them.
  field max_argument_buffer_tier : u8  tag 8  since 1   default 0
  field hdr_max_nits             : u16 tag 9  since 1   default 0
  field timestamp_period_ns      : f32 tag 10 since 1   default 0.0
}
```

**Invariants:**

1. **Single-writer.** Exactly one host process writes the
   `CapabilityMask` for a given `host_id`; concurrent writes from a
   second instance refuse with `Error::CapabilityNotSupported` (post-MVP
   refines this to a file-lock seam owned by `platform`).
2. **Capability bits are append-only.** New `Capability` enumerators
   may be defined; existing bit positions are immutable once shipped
   (mirrors the `data` SPEC §7 reserved-tag rule, applied to bit
   positions inside the `capabilities` u32).
3. **Stale records are revalidated, not migrated.** When the record's
   `(gpu_id, metal_feature_set, os_build_hash)` no longer matches the
   live device, the loader treats it as absent and re-probes; it does
   not decode-and-mutate stale records. Probing is the only writer of
   capability bits.

### 7.2 Migration rules

Per `reviews/decisions/fory-codegen.md` §"Migration Mechanic", every
schema-version bump emits a generated dispatcher hookup. Render owns the
migration *bodies* for the types above; their *plumbing* is generated.

#### 7.2.1 `RenderSettings` migrations — additive only

The settings type follows the **additive defaulted-field** pattern:

1. **N → N+1 adds a field at a new tag.** Default value defined in the
   schema; codegen synthesises the default at deserialise time when the
   payload omits the new field (Fory's `since` clause). No migration
   body required; the codegen tool emits
   `migrate_RenderSettings_v<N>_to_v<N+1>` as the identity mapping with
   default-fill.
2. **N → N+1 changes the meaning of an existing enumerator.** Treated
   as breaking. Bump the schema to a new major (post-MVP — render
   commits to no breaking settings changes inside MVP). Migration body
   lives in `src/render/migrations/render_settings_v<N>_to_v<N+1>.cpp`
   and is owned by render, not by `data`.
3. **N → N+1 removes a field.** Tag becomes `reserved`; never reused.
   Codegen rejects reuse at generation time per data SPEC §7.

Round-trip golden test contract: for every shipped schema version `N`,
`tests/data/schemas/render/RenderSettings.cpp` includes a recorded `vN`
payload and asserts `migrate(vN) == defaults_for_v_current()` modulo
the explicitly-set fields in the recorded payload.

#### 7.2.2 `PSOCacheRecord` — invalidate, never migrate

Any breaking change — including a new field whose absence the runtime
cannot synthesise (e.g. a new identity component of `PSOKey`), a change
to render's `state_hash` function, a change to `shader`'s metallib
encoding, a libc++ ABI bump that perturbs `MTL::BinaryArchive` layout,
or a glibre middleman ABI hash bump — **invalidates the entire
PSO-archive directory**. Implementation: the warmer compares the loaded
record's `glibre_types_abi_hash` to the live middleman's hash; a single
mismatch at startup triggers `rmtree(<pso-archive-dir>)` and the runtime
proceeds with an empty cache. There is no partial-validity middle state.

This rule is enforced in code by **omitting** any `migration` block
from `PSOCacheRecord.fory` entirely. The Fory codegen tool refuses to
emit a migration dispatcher for a type whose schema declares no
migrations; any future schema bump therefore forces the author to
either add an explicit migration (rejected by review per this rule) or
accept the whole-archive invalidation (the only allowed path).

#### 7.2.3 `CapabilityMask` — additive bits, immutable positions

1. **Adding a `Capability` bit position.** Reserve the next free bit in
   the `capabilities` u32 in-source and rebuild the dylib; the `.fory`
   schema is unchanged. The probe writes the bit only when the current
   driver / device reports support; older masks read on a newer build
   simply present a zero in the new position, which the gating rules
   in §7.1.1 / §7.1.2 already treat as "feature unavailable".
2. **Promoting `capabilities` from u32 to u64.** Treated as a breaking
   schema bump (new tag, defaulted to zero, old field marked
   reserved). Migration body provided by render under
   `src/render/migrations/`; round-trip test is mandatory.
3. **Adding a probe field (e.g. `vrr_min_refresh_hz`).** Additive
   defaulted-field pattern, identical to §7.2.1 case 1. Default
   represents "not probed"; loader re-probes opportunistically rather
   than treating the default as a reading.

### 7.3 What is NOT persisted

To make the boundary explicit (in line with §5 "None at this layer"):

| Artefact            | Why not persisted                                                                                |
|---------------------|--------------------------------------------------------------------------------------------------|
| `RenderFrame`       | Per-frame arena snapshot; rebuilt every frame from the ECS.                                      |
| `ExecutionPlan`     | In-memory structural-hash cache; rebuilt when the pass set or capability set changes (§4.1.5).   |
| `RenderGraph`       | C++ code, not data (PHILOSOPHY anti-pattern).                                                    |
| `RenderProxy` SoA   | Lives inside `RenderFrame`'s arena.                                                              |
| `AliasPlan`         | Output of the alias planner per recompile of `ExecutionPlan`.                                    |
| `LightCluster`      | Compute-built each frame from the unified light buffer.                                          |
| `HZB`               | GPU-side per-frame depth pyramid.                                                                |
| `RingBuffer` slices | CPU-write / GPU-read; rebuilt per frame-in-flight.                                               |
| `GpuTimestamp` ring | Debug-only; one-frame latency by construction.                                                   |
| `DiagnosticOverlay` | Build-gated debug surface; never persisted.                                                      |
| Shader bytecode     | Owned by `shader`'s cook (§3.3); render consumes by hash.                                        |

These appear in the persistence surface only as **identifiers**
(`shader_hash`, `state_hash`, `gpu_id`) referenced from §7.1, never as
byte payloads.

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
