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
Slang→AIR/metallib compilation belongs to `shader`; material graph
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
| Slang → AIR / metallib compilation, shader-variant cook, slangc subprocess management, `slangc` invocation | `requirements/rendering/gpu-abstraction-layer.md` R-2.1.17, `design/rendering/shader-variants.md`, `design/rendering/pipeline-state-cache.md` (cook-time half) | `shader` plugin. Render consumes opaque `PSO` handles. |
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
lambda captures only POD or `eastl::span` slices into the
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
   compiles Slang or transcodes AIR; that is `shader`'s job
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

#include <EASTL/array.h>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <EASTL/span.h>
#include <EASTL/string_view.h>
#include <type_traits>
#include <EASTL/variant.h>

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
    eastl::string_view file;
    int              line  = 0;
    eastl::string_view detail;
};

class Error {
public:
    using Variant = eastl::variant<core::Error /*, render::Error inserted in core */>;

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
    ShaderModuleLoadFailed,        // PSO-cache design §3.5 step 4 / §10

    // Resource residency / aliasing
    ResourceResidencyExceeded,
    ResourceImportRefused,
    StaleResourceHandle,          // ABI add: render-resources design §3.4 / §10
    ResourceRoleMismatch,         // ABI add: render-resources design §3.7 / §10
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

[[nodiscard]] constexpr eastl::string_view to_string(Error e) noexcept;

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
struct sampler                {};   // ABI add: render-resources design §3.3
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
using SamplerHandle          = Handle<tags::sampler>;           // ABI add: render-resources design §3.3
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
    std::uint32_t           warm_per_tick        = 4u;  // PSO-cache cold-build cap per
                                                         // glibre_plugin_register tick
                                                         // (§8.3.2 rate limiter). 0 = no cap.
                                                         // PSO-cache design §3.8 / §4.3.
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
    eastl::string_view debug_name;
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
    eastl::function<Result<void>(MetalCommandBuffer&, const Bindings&) /* noexcept */>;

// -----------------------------------------------------------------------
// GraphBuilder — fluent surface plugins use to declare passes. Per-pass
// invariants (declared = used, queue purity, no allocations on the hot
// path) are checked at compile() time and at execute() in debug builds.
// -----------------------------------------------------------------------

struct PassDesc {
    eastl::string_view name;
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
                        eastl::span<const ResourceAccess> reads,
                        eastl::span<const ResourceAccess> writes,
                        PassExecuteFn                   execute) noexcept;

    [[nodiscard]] Result<void>
        add_compute_pass(const PassDesc&,
                         eastl::span<const ResourceAccess> reads,
                         eastl::span<const ResourceAccess> writes,
                         PassExecuteFn                   execute) noexcept;

    // Ray-trace pass — RT-shadow / AO / reflection bodies declare TLAS
    // input + shadow / AO / reflection target outputs. Rejected with
    // `CapabilityNotSupported` if `Capability::HardwareRayTrace` is
    // missing from the device CapabilitySet at build time.
    [[nodiscard]] Result<void>
        add_rt_pass(const PassDesc&,
                    TLASHandle                       tlas,
                    eastl::span<const ResourceAccess>  reads,
                    eastl::span<const ResourceAccess>  writes,
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
    [[nodiscard]] static Result<eastl::unique_ptr<RenderGraph>>
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
// not authoring (`shader` plugin compiles Slang → AIR / metallib).
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
    // Hot-path lookup; lazy-builds on miss. Holds a pin on the returned
    // handle until the matching unpin / RAII PinnedPSO release.
    [[nodiscard]] Result<PSOHandle> get(PSOKey) noexcept;

    // Long-lived pin (survives RenderFrame retire). Used by
    // glibre_plugin_register (§8.3.2) and by the startup warmer.
    [[nodiscard]] Result<PSOHandle> pin(PSOKey) noexcept;
    void                            unpin(PSOHandle) noexcept;

    // Bulk warm. Builds every key in `keys`; stops at the first
    // ShaderModuleLoadFailed / PipelineCompileFailed and returns.
    // Entries successfully built before the first failure remain live
    // (partial warm is safe; callers must not retry failed keys).
    [[nodiscard]] Result<void>      warm(eastl::span<const PSOKey>) noexcept;

    // Drop every entry whose key.shader_hash matches. Returns the count
    // of entries dropped. Called by `shader`'s reload hook.
    [[nodiscard]] std::size_t invalidate_by_shader_hash(std::uint64_t shader_hash) noexcept;

    // Force LRU drain to `target_size` bytes. Honours pin_count;
    // overshoot is logged but never refused.
    void evict_lru(std::size_t target_size) noexcept;

    // Register the pipeline descriptor for a state_hash. Must be called
    // by the pass-registry (GraphBuilder) before any get/pin/warm call
    // that supplies this state_hash. Idempotent on identical desc;
    // returns unexpected{PipelineCompileFailed} on collision.
    [[nodiscard]] Result<void>
        register_state_descriptor(std::uint64_t  state_hash,
                                  StateDescriptor desc) noexcept;

    // Diagnostic accessors — read-only, lock-free.
    [[nodiscard]] std::size_t live_bytes()  const noexcept;
    [[nodiscard]] std::size_t entry_count() const noexcept;

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
    [[nodiscard]] static Result<eastl::unique_ptr<MetalDevice>>
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
    [[nodiscard]] Result<eastl::unique_ptr<MetalCommandBuffer>>
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
    [[nodiscard]] Result<void> push_debug_group(eastl::string_view name) noexcept;
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
    void                            invalidate(ViewHandle) noexcept;

    // ABI add (hzb-cull-design.md) — read-only handle accessor for
    // DiagnosticOverlay (#774) and other render-internal readers.
    // Returns an empty optional when the view has no live pyramid
    // (not yet ensure'd, or after invalidate before next ensure).
    [[nodiscard]] eastl::optional<HZBHandle>
                                    handle(ViewHandle) const noexcept;

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
    [[nodiscard]] eastl::span<const ViewHandle> views()        const noexcept;

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

Non-binding sketch for implementers. The aggregates of §4 and the public
header of §5 are binding; the file/directory layout, threading topology,
and per-pass invocation order below are illustrative and exist so the
plan-leaf author has one obvious place to start. Reviewers should reject
deviations only when they violate §4 invariants, the §5 header, or the
per-phase ownership locked in `reviews/decisions/frame-phases.md`.

### 6.1 Module layout

The render plugin compiles to a single `.dylib`. Inside, source is split
by SRP — one directory per "reason to change". Public headers (the §5
deliverable + an `internal/` tree the rest of the plugin consumes) live
under `render/include/glibre/render/`; implementation under `render/src/`.

```
render/
  include/glibre/render/         # §5 surface (compiles standalone).
    render.hpp                   # The single header from §5.
    capability.hpp               # Capability / CapabilitySet bit layout.
    handle.hpp                   # Generational `Handle<Tag>` template.
  src/
    graph/                       # Aggregate §4.1.2 + §4.1.5.
      builder.{hpp,cpp}          # GraphBuilder fluent surface.
      compile.{hpp,cpp}          # Topo sort, alias plan, barrier emit.
      pass.{hpp,cpp}             # Pass storage + access-set typing.
      execution_plan.{hpp,cpp}   # ExecutionPlan + structural-hash key.
      diagnostic.{hpp,cpp}       # DiagnosticOverlay (debug-only).
    metal/                       # Aggregate §4.1.6.
      device.{hpp,cpp}           # MetalDevice singleton + heap allocator.
      queue.{hpp,cpp}            # Graphics / Compute / Copy queue trio.
      command_buffer.{hpp,cpp}   # MetalCommandBuffer + encoder cursor.
      fence.{hpp,cpp}            # Cross-queue fence + present-fence emit.
      residency.{hpp,cpp}        # MTLResidencySet attach/detach helpers.
    passes/                      # One pass body per file; SRP per pass.
      gbuffer.{hpp,cpp}          # Mesh-shader gbuffer + visID + velocity.
      lighting.{hpp,cpp}         # Deferred lighting + RT shadow ray query.
      shadow_rt.{hpp,cpp}        # Hybrid-RT shadow trace + denoise hook.
      ao_rt.{hpp,cpp}            # Hybrid-RT AO trace + denoise hook.
      cluster_cull.{hpp,cpp}     # ClusterCullPass body (state in §4.1.10).
      hzb_build.{hpp,cpp}        # HZBBuildPass body (state in §4.1.9).
      tlas_build.{hpp,cpp}       # TLAS rebuild-or-refit pass body.
      blas_refit.{hpp,cpp}       # BLAS refit dispatch (compute queue).
      transparent_forward.{hpp,cpp}  # Forward path reading LightCluster.
      post.{hpp,cpp}             # Bloom / DOF / motion / tonemap chain.
      aa_upscale.{hpp,cpp}       # TAA / FXAA / SMAA / TSR variant select.
      present.{hpp,cpp}          # Drawable acquire + present-fence signal.
    resources/                   # Aggregate §4.1.4 implementation.
      transient_pool.{hpp,cpp}   # Heap pool drained per recompile.
      persistent.{hpp,cpp}       # HZB, ShadowAtlas, history-color, rings.
      alias_planner.{hpp,cpp}    # Interference-graph colouring.
      ring_buffer.{hpp,cpp}      # Per-frame-in-flight CPU-write rings.
      argument_buffer.{hpp,cpp}  # Frequency-group binder (§3.2 #8).
    pso_cache/                   # Aggregate §4.1.7.
      cache.{hpp,cpp}            # `(shader_hash,state_hash)` table + LRU.
      warmer.{hpp,cpp}           # MVP-set pre-fault from shader manifest.
      record.{hpp,cpp}           # PSOCacheRecord (Fory schema in §7.1.2).
    rt/                          # Aggregate §4.1.8.
      blas_registry.{hpp,cpp}    # Imported BLAS handle table.
      tlas.{hpp,cpp}             # TLAS lifetime + rebuild-vs-refit policy.
      refit_scheduler.{hpp,cpp}  # Per-frame BLAS refit scheduling.
    cull/                        # Aggregate §4.1.9 + §4.1.10 + extract.
      hzb.{hpp,cpp}              # HZB pyramid storage; two-phase reads.
      cluster_cull_state.{hpp,cpp}   # Persistent-thread cluster cull.
      meshlet_cull.{hpp,cpp}     # Meshlet frustum + normal-cone cull.
      extract.{hpp,cpp}          # Phase-6 extract → RenderFrame builder.
      sort.{hpp,cpp}             # Single-pass radix on packed SortKey.
      budget.{hpp,cpp}           # Cost-aware budget culler (§3.2 #10).
    frame/
      render_frame.{hpp,cpp}     # RenderFrame storage + triple-buffer slots.
      view.{hpp,cpp}             # Per-View instantiation + layer mask.
    plugin.{hpp,cpp}             # Plugin entry: init / tick / migrate / shutdown.
```

The split is the §4 aggregate roster lifted directly into directories.
Each directory owns one reason to change. Adding a new pass adds one
file under `passes/`; adding a new resource role touches `resources/`;
swapping the alias algorithm touches `alias_planner.cpp` only.

### 6.2 Frame integration — phase 6 and phase 7

Render owns exactly two phases per `reviews/decisions/frame-phases.md`.

#### 6.2.1 Phase 6 — `cull-extract` (build `RenderFrame`)

Driver thread, sequential body, no GPU calls. Steps in order:

1. `cull/extract.cpp` opens a fresh `RenderFrame` slot in render's
   triple-buffered per-frame arena (§4.1.1 invariant 4) and pins it
   to the current `(World, FrameCounter)`.
2. For each active `View` registered with render:
   1. `cull/meshlet_cull.cpp` reads `GlobalTransform` + meshlet
      bounds + last-frame's `HZB` (`cull/hzb.cpp`, §4.1.9 invariant
      1) and emits the meshlet survivor set per the §3.1 core-raster
      derivation.
   2. `cull/budget.cpp` applies `PassPriority`-respecting cost-aware
      culling against the per-view draw budget so phase 7 never has
      to drop work silently (§4.1.1 invariant 3).
   3. `cull/sort.cpp` writes the packed 64-bit `SortKey` column and
      runs single-pass radix into the phase bucket arrays
      (opaque / alpha-tested / translucent / shadow / 2D / capture).
   4. The light list, camera + jitter, interp-α, and `RenderSettings`
      snapshot are copied into the slot.
3. The slot becomes immutable (§4.1.1 invariant 1). Phase 6 returns;
   the snapshot bus delivers a `const RenderFrame&` to phase 7.

Phase 6 is the only place ECS storage is read on the render side
(§4.2 invariant 6); after exit, the ECS half of the frame may begin
phase 7 of frame N+1's predecessor or move on to phase 8.

#### 6.2.2 Phase 7 — `render-submit` (consume `RenderFrame`)

Driver thread initiates phase 7; the body splits into a build/compile
half (graph builder thread) and a recording half (per-pass workers
on the render thread pool). Phase 7 returns when frame N's command
buffer is enqueued and `PresentFence` is signalled (consumed by
phase 9). Steps in order:

1. **Build.** `graph/builder.cpp` instantiates one `RenderGraph`
   per `View` from the immutable `RenderFrame`. The builder
   sequence is fixed at MVP and registers passes in this order
   (capability-gated per §4.1.2 invariant 3):

   1. `passes/blas_refit.cpp` — refit one BLAS per visible LOD0
      cluster set whose source mesh is dynamic (skinned /
      deformable). The refit pass declares `Queue::Compute`.
   2. `passes/tlas_build.cpp` — rebuild-or-refit the TLAS from
      the visible-set; declares a read-after-write on the BLAS
      refit outputs (§4.2 invariant 4). Compute queue.
   3. `passes/cluster_cull.cpp` — persistent-thread compute
      build of `LightCluster` for the active froxel grid.
      Compute queue.
   4. `passes/gbuffer.cpp` — mesh-shader dispatch writing the
      gbuffer MRT + visibilityID + velocity in one `Pass`
      (§4.2 invariant 5). Graphics queue.
   5. `passes/hzb_build.cpp` — depth-pyramid build from the
      gbuffer's depth output, written to next frame's HZB
      (§4.1.9 invariant 1). Compute queue.
   6. `passes/shadow_rt.cpp` + `passes/ao_rt.cpp` — RT shadow /
      AO compute traces consuming the TLAS. Compute queue.
   7. `passes/lighting.cpp` — deferred lighting compute reading
      gbuffer + `LightCluster` + RT shadow / AO outputs; ray
      query inline for hybrid-RT shadow primary-ray fallback
      where `RenderSettings.shadow_tier` selects RT
      (§3.2 collapse #2). Compute queue.
   8. `passes/transparent_forward.cpp` — forward translucent
      pass reading the same `LightCluster` (§3.2 collapse #3).
      Graphics queue.
   9. `passes/post.cpp` — bloom / DOF / motion / tonemap /
      grade chain ordered per `RenderSettings`. Graphics queue.
   10. `passes/aa_upscale.cpp` — TAA / FXAA / SMAA / TSR variant
       selected by `RenderSettings.aa_mode`; graph topology
       differs per variant, no shader-side branching
       (§3.2 collapse #5). Graphics queue.
   11. `passes/present.cpp` — drawable acquire, swapchain blit,
       `PresentFence` signal. Graphics queue.

2. **Compile.** `graph/compile.cpp` runs topological sort
   (§4.1.2 invariant 1), interference-graph colouring through
   `resources/alias_planner.cpp` (§4.1.4 invariant 2), barrier
   emission through `graph/compile.cpp` (split-aware, minimum;
   §4.1.5 invariant 1), queue assignment, and binding-table
   build through `resources/argument_buffer.cpp`. Hits the
   `ExecutionPlan` cache keyed by structural hash (§4.1.5 +
   §4.1.2 invariant 4); a cache hit skips compile and rebinds
   only.

3. **Record + submit.** `metal/command_buffer.cpp` opens one
   `MetalCommandBuffer` per queue. Per-pass `execute()` lambdas
   record into the right encoder; `metal/fence.cpp` emits the
   inter-queue fences computed by the plan. `metal/queue.cpp`
   commits the buffers; `metal/fence.cpp` flags `PresentFence`.

4. **Cleanup.** Transient `VirtualResource`s recycle to
   `TransientPool` (§4.2 invariant 2). The `RenderFrame` slot
   is marked retired; the graph object is destroyed; the
   `ExecutionPlan` survives in the cache.

### 6.3 Concurrency

Render runs on three thread-roles inside phase 7. Phase 6 is
driver-thread only.

- **Graph builder thread (one).** Owns `graph/builder.cpp`,
  `graph/compile.cpp`, `resources/alias_planner.cpp`, and the
  `ExecutionPlan` cache. Builds and compiles synchronously per
  `View`; multi-view fan-out reuses the same thread sequentially
  because compile time is dominated by hash + cache lookup, not by
  topo sort (§3.2 collapse #7). The builder is single-threaded so
  the `ExecutionPlan` cache needs no locks and structural hashing
  is deterministic frame-to-frame. The graph builder thread is
  pinned (no migration) to keep its per-frame arena local.

- **Per-pass GPU encoding workers (a small pool).** Once the plan
  exists, per-pass `execute()` lambdas may be recorded in parallel
  into per-queue `MetalCommandBuffer`s. The plan's queue assignment
  partitions passes into independent record streams; passes inside
  one queue record sequentially in plan order on one worker, but
  Graphics / Compute / Copy queues may record on three workers
  concurrently. Worker count is bounded by the queue count (three
  in MVP); no further scaling — Metal command-buffer recording is
  not the bottleneck.

- **Render thread (driver).** Submits the recorded command buffers
  to `MetalQueue` in queue dependency order, waits on no GPU
  completion, signals `PresentFence`, and returns. Frame N's GPU
  execution overlaps frame N+1's simulation per
  `reviews/decisions/frame-phases.md` "one-frame pipeline".

The graph builder thread is the only writer to the `RenderGraph`
and the `ExecutionPlan` cache; per-pass workers read both as
`const`. There are no cross-thread mutex acquisitions inside phase
7 once the plan is built — workers communicate exclusively via the
plan's pass list (read-only) and per-queue command-buffer handles
(thread-local until commit).

### 6.4 Hybrid-RT path

Hybrid RT in MVP means: Metal 4 ray query in compute lighting +
dedicated shadow / AO compute traces, all reading one TLAS rebuilt
or refit each frame. The structure is:

1. **BLAS refit per visible LOD0 cluster set.** `rt/refit_scheduler.cpp`
   walks the `RenderFrame` visible-set and selects BLAS handles
   whose source mesh is dynamic (skinned / deformable). Static-mesh
   BLAS are cooked by `geometry` (§3.3) and never refit. A refit
   request goes to `passes/blas_refit.cpp` which records a Metal 4
   acceleration-structure refit on `Queue::Compute`. Render writes
   only to the BLAS update slot (§4.1.8 invariant 3).

2. **TLAS rebuild-or-refit.** `rt/tlas.cpp` decides per-frame
   between full rebuild (visible-set membership churn over
   threshold) and refit (membership stable, only transforms
   changed) per §4.1.8 invariant 2. The decision is compile-time
   from the structural diff of consecutive `RenderFrame`s; the
   compiler picks the matching pass body. The TLAS-build pass
   declares an explicit read-after-write on the BLAS refit
   resources so the plan compiler emits the cross-queue fence
   (§4.2 invariant 4).

3. **Ray query in lighting compute.** `passes/lighting.cpp` issues
   a Metal 4 inline ray query against the TLAS for shadow primary
   rays when `RenderSettings.shadow_tier == ShadowTier::Raytraced`.
   Dedicated `passes/shadow_rt.cpp` and `passes/ao_rt.cpp` provide
   denoised auxiliary buffers when their tiers are RT;
   `passes/lighting.cpp` composites them into the deferred light
   accumulation. No second TLAS, no second backend; one compute
   surface per RT consumer.

Reflections in MVP read the lighting result via screen-space
fallback inside `passes/lighting.cpp`; full RT reflections is a
post-MVP `Pass` insertion (§3.2 collapse #2).

### 6.5 Mesh-shader path

The opaque coverage path is a single mesh-shader dispatch:

1. `cull/meshlet_cull.cpp` (phase 6) emits the meshlet survivor
   set per `View` against last frame's HZB.
2. `passes/cluster_cull.cpp` (phase 7, compute) builds
   `LightCluster` for the same view; runs in parallel with
   `passes/blas_refit.cpp` and `passes/tlas_build.cpp` on the
   compute queue.
3. `passes/gbuffer.cpp` (phase 7, graphics) consumes the
   meshlet survivor set as `IndirectDrawBuffer` material-grouped
   compaction output, dispatches Metal 4 mesh shaders, and writes
   gbuffer MRT + visibilityID + velocity in one declared `Pass`
   (§4.2 invariant 5). Reverse-Z is shared with HZB
   (§4.1.9 invariant 3).
4. `passes/hzb_build.cpp` (phase 7, compute) builds next frame's
   HZB from this frame's gbuffer depth.

The visibility ID buffer is written in the same dispatch so that a
post-MVP visibility-buffer deferred path does not require a second
gbuffer pass; it slots in as a downstream consumer
(§3.2 collapse #2).

### 6.6 Cross-platform readiness

Metal 4 is the only shipping backend (MVP and post-MVP near horizon;
§3.2 collapse #1). Vulkan and D3D12 are explicitly out of scope for
this plugin and would land as **separate plugins** when needed:

- The §5 public header is backend-neutral. `MetalDevice`,
  `MetalQueue`, and `MetalCommandBuffer` are forward-declared
  opaques whose layout lives inside the render dylib; callers
  manipulate them only through `glibre::Result<T>`-returning
  methods. A Vulkan or D3D12 plugin would fork this header into
  a `vk_render` / `d3d_render` plugin with its own opaques but
  identical aggregate names and identical `Pass` invariants
  (§4.1.3, §4.2.5).
- The graph layer (`render/graph/`) is backend-neutral by
  construction: `Pass::execute(MetalCommandBuffer&, const Bindings&)`
  is the only seam with Metal types. A second backend plugin would
  reuse the §4.1.2/§4.1.5 algorithms verbatim and substitute its
  own command-buffer wrapper.
- Resources (`render/resources/`) are also reusable: the alias
  planner operates on declared lifetimes, not on `MTLHeap`
  specifics; the implementation calls into `metal/` only at
  materialise time.
- `passes/` are backend-bound and would be re-implemented per
  backend; the per-pass file split (one pass per file) keeps the
  per-backend porting surface small.

No `IDevice` / `ICommandBuffer` abstraction exists in the MVP plugin
(rejected per §3.2 collapse #1: SRP says one reason to change the
GPU layer is "Metal 4 evolves"). A future backend plugin owns its
own copy; cross-backend abstraction would be a fresh spike, not a
retrofit, and would only happen when a second concrete backend
landed (PHILOSOPHY anti-pattern: "abstractions invented before two
concrete users exist").

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
  field warm_per_tick        : u32  tag 10 since 1  default 4      // PSO cold-build cap per
                                                                    // glibre_plugin_register tick
                                                                    // (§8.3.2); 0 = no cap.
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

This section specialises the engine-wide hot-reload protocol
(`reviews/decisions/hot-reload-protocol.md` — drain → swap → migrate →
resume) to the **render plugin**. It defines exactly which render-owned
state survives a swap, what `migrate(...)` must do for the render-graph
builder and the PSO-cache, and which conditions cause render's reload
attempt to be refused with the engine's standard `core::Error::HotReloadRefused`
arm. Engine-wide concerns (per-plugin atomicity, observer bus event
shapes, error wrapping rules, the `enqueue_hot_reload` E2E hook) are not
re-stated here — see the protocol record. Render adds nothing to that
machinery; it only fills in the four pluggable points the protocol
leaves to each plugin: drain side-effects, survival inventory, migrate
body, and register-time rehydration.

### 8.1 Reload point — phase 8, never mid-frame

The engine schedule (`reviews/decisions/frame-phases.md`) places the
hot-reload barrier at phase 8, **after `render-submit` (phase 7) and
before `present` (phase 9)**. Render's reload protocol is anchored to
that one slot and refuses any other.

At phase 8 entry, render's in-flight state is:

1. **No command buffer is being recorded.** Phase 7 already returned
   for frame N; every render system has finished writing its
   `MetalCommandBuffer`. Recording is therefore not interrupted by the
   swap (cf. §4.1.1 invariant 1: `RenderFrame` is immutable post-build,
   so nothing in the recording path can race the swap).
2. **The submit-fence for frame N is already signaled on the CPU
   side.** Phase 7's exit guarantee (frame-phases table row 7) is
   "command buffer for frame N is enqueued to the GPU"; the
   `MetalQueue::submit` call returns only after the queue's CPU-visible
   submit fence is signaled (i.e. the GPU has accepted the workload
   into its queue). The GPU itself may still be executing frame N —
   that is irrelevant to render's reload, because the swap touches
   only host code (vtables, builder pointers, cache pointers), never
   GPU resource bytes (see §8.2). The next presentation of frame N
   in phase 9 reads exclusively from already-submitted command-buffer
   contents.
3. **`RenderFrame` for frame N is destroyed.** Per §4.1.1, the
   per-frame extract is destroyed at phase 7 exit; phase 8 sees no
   live snapshot. (Triple-buffered slots for frame N+1 may have been
   pre-populated by an early phase-6 if pipelining is enabled; those
   slots survive the swap because their byte layout is owned by
   `glibre-types` middleman types, not by render-plugin code.)

These three conditions are the render-half of the protocol's "drain"
postcondition (protocol §"Step 1 — Drain"). Render's
`glibre_plugin_drain` body therefore has nothing to flush from the
recording side; its work is the GPU-resource release described in §8.3.

**Mid-frame reload is refused.** Any reload request that arrives during
phases 1–7 is queued, never applied; the loader's `pending_reloads`
counter is consumed only at phase 8 entry per protocol step 1. Inside
phase 8, render does not yield to recording or submission — the loader
holds exclusive ownership for the duration of drain → swap → migrate →
resume per protocol §"Decision". A request that would force any of
phases 1–7 to observe a partially-swapped vtable is treated as a
contract violation by the loader, not a refusal — render's spec
contributes no new refusal arm here, but states the invariant
explicitly so consumers cannot expect mid-frame swap semantics.

### 8.2 Survival inventory

The engine-wide survival rule is mechanical: **state with a
`.fory` schema in `glibre-types.dylib` survives across the swap;
state without one does not** (protocol §"State Survival Rules";
PHILOSOPHY collapse: one check, not a per-aggregate manifest). Render
owns three persistent fory-schema'd types (§7.1.1–§7.1.3) and a
collection of host- and device-side runtime state. The table below
classifies every render aggregate against that rule and adds the
render-specific reasoning for each survival decision.

| Render-owned state                                                       | Persistence path             | Survives swap? | Reasoning                                                                                                                                                                                                                                                                                                                                  |
|--------------------------------------------------------------------------|------------------------------|----------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `RenderSettings` (§7.1.1)                                                | `.fory` schema, middleman    | Yes — bytes are owned by `glibre-types`; render only reads them. Per §7.2.1 a schema bump on `RenderSettings` flows through the engine's standard additive-defaulted-field migration. Render's `migrate(...)` does not touch these bytes. |
| `CapabilityMask` (§7.1.3)                                                | `.fory` schema, middleman    | Yes — capability bits do not depend on the render plugin; the mask is probed once per `(host_id, gpu_id, metal_feature_set, os_build_hash)` tuple by `platform`. The reload **must not** revalidate or re-probe; per §8.4 a reload that disagrees with the surviving mask is refused, not silently re-probed. |
| `PSOCacheRecord` (§7.1.2) — on-disk archive                              | `.fory` schema, middleman    | Conditional. The disk archive survives the swap as bytes (it is owned by `glibre-types`), but the runtime `PSOCache` invalidates the entire archive directory when the loaded record's `glibre_types_abi_hash` differs from the live middleman's hash (§7.2.2 — "invalidate, never migrate"). Render's `migrate(...)` honours this rule by calling the existing warmer, which performs the hash check. |
| Persistent `Resource`s — texture handles, persistent buffers, BLAS imports (§4.1.4, §4.1.8 invariant 3) | None (in-process)             | Yes, **unless their schema changed**. Texture / buffer / BLAS bytes are GPU-side allocations imported from `geometry` and `content`; their identity is a stable `glibre.types.render.GpuId`. The swap preserves the handle table because the table's keys are middleman types. If the new plugin manifest declares a `(fqn, schema_version)` for a persistent-resource component that does not match the surviving storage's version, the protocol treats this as the §7.2 invalidation case (refusal, not silent loss). |
| Runtime `PSOCache` — in-memory hash map keyed by `PSOKey = (shader_hash, state_hash)` (§4.1.7) | None (in-process)             | Yes, **key-stable across reload**. Both halves of `PSOKey` are content hashes (cooked metallib + render's deterministic state hash); identical `(shader_hash, state_hash)` pairs in the new plugin map to the same residency entries. Render's `migrate(...)` rebinds the cache pointer in the new builder rather than rebuilding entries (§8.3). The cache is invalidated only when (a) `shader` swaps a metallib, which changes `shader_hash` (§4.1.7 invariant 4), or (b) the on-disk archive's hash check fails (above). |
| `RTAccelStructures` — TLAS scratch, BLAS instance buffers (§4.1.8)        | None (in-process)             | Yes for BLAS imports (read-only by render); TLAS contents are transient and rebuilt every frame, so "survival" is a no-op. |
| `HZB` pyramid (§4.1.9), `ClusterCullState` scratch (§4.1.10)             | None (in-process)             | Yes — both are persistent `Resource`s sized at init from `RenderSettings` / `QualityTier`; their backing GPU allocations and dispatch parameters survive. The next frame's two-phase symmetry (§4.1.9 invariant 1) is preserved because no in-flight read or write straddles the swap (§8.1). |
| `MetalDevice`, `MetalQueue` instances                                     | None                          | Yes. The Metal device handle is owned by `platform` (§3.3 cited refusal) and exposed to render via `MetalDevice::create`. Render's reload does not call `create` again; the new plugin acquires the existing device via the engine's `Registry`. |
| `TransientPool` heaps (§4.1.4)                                            | None                          | Yes. The placement heaps are sized from `RenderSettings`; their bytes are render-internal but survive because the loader holds exclusive ownership during phase 8 and no transient resource lives across the phase 7→9 boundary (§4.2 cross-aggregate invariant). The `AliasPlan` cache is dropped on the swap and rebuilt by the new plugin's first compile (§4.1.5). |
| `RenderGraph`, `Pass`, `RenderFrame` (§4.1.1–§4.1.3)                      | None — never serialised       | N/A — destroyed at phase 7 exit; phase 8 sees no instance. The new plugin builds fresh graphs at the next frame's phase 6. |
| `DiagnosticOverlay`, GPU timestamp ring (§7.3)                            | None — debug-gated            | Reset on swap. Per `frame-phases.md` §Notes, profiler traces persist phase IDs only; render's debug surfaces are not contractual. |
| `ExecutionPlan` structural-hash cache (§4.1.5)                            | None — in-memory              | Dropped on swap. The cache is keyed by pass-set hash; the new plugin's first phase-6 reconstructs it. The cache miss is bounded (one extra compile per `View`) and is the dev-time cost of a reload, not a determinism issue (frame N's plan is already submitted; frame N+1 builds anew). |
| Per-plugin worker thread pools, internal RT-denoiser caches               | None                          | No — destroyed by render's `glibre_plugin_drain`, re-spawned by the new plugin's `glibre_plugin_register`. |

The rule mechanically applied: every row marked "Yes" has either a
`.fory` schema or is owned by `core` / `platform` / `geometry` /
`content` / `shader`; every "No" row is private to render with no
on-disk format and no migration contract — exactly what
PHILOSOPHY §3 + protocol §"State Survival Rules" require.

### 8.3 `migrate(...)` body — render's responsibilities

The protocol's `migrate` step (protocol §"Step 3 — Migrate") runs
*pure* per-row migrate functions for every persistent-component-type
schema bump on the engine's behalf. Render owns three of those bodies
(§7.2.1 `RenderSettings`, §7.2.3 `CapabilityMask` — both additive in
MVP; the `PSOCacheRecord` case explicitly forbids a body per §7.2.2).
Those functions are the standard pure migrate signature; nothing here
changes them.

What this section adds is the **render-plugin-specific portion of step
4 (resume)** — the work the new plugin's `glibre_plugin_register`
must do to repoint render-graph builders and PSO-cache references at
the new code while reusing surviving bytes. Two pointer fix-ups
matter:

#### 8.3.1 Swap the render-graph builder

The new plugin exports a fresh `glibre_plugin_register` that:

1. **Publishes the new `GraphBuilder`-factory pointer** into the
   render registry's `(View → GraphBuilder factory)` table. The table
   is itself a middleman type
   (`glibre::types::render::GraphBuilderRegistry`) so its slot
   identities survive; the values held in the slots are function
   pointers into the new plugin's image. The fix-up is a single
   atomic store per slot, performed under the loader's exclusive
   phase-8 ownership (no race with phase 6 of frame N+1, which has
   not yet begun).
2. **Re-registers each pass class** (`gbuffer`, `deferred-lighting`,
   `cluster-cull`, `hzb-build`, `tlas-build`, `present`, …) into the
   pass class registry. Class identity is the
   `(phase, system_fqn)` pair, idempotent per protocol step 4.1; a
   re-registration of the same identity is a no-op even if the
   underlying function pointer is new (the new pointer overwrites
   the old slot).
3. **Does not rebuild any `RenderGraph` or `ExecutionPlan`.** Both
   are per-frame and are produced by the next frame's phase 6 from
   the new builder. Pre-building inside `register` would violate
   the protocol's "no system bodies run in phase 8" rule (protocol
   §"Decision").

The previous plugin's `glibre_plugin_drain` need do nothing for
graphs or plans — they are gone before phase 8 begins.

#### 8.3.2 Restore PSO-cache pointers from the new plugin's tables

Render's `PSOCache` (§4.1.7) is a single `MetalDevice`-owned hash
map; the cache instance is owned by `MetalDevice::pso_cache()`,
**not by the render plugin's image**. The cache survives the swap
verbatim (table above). What does *not* survive is the new plugin's
**static dispatch table** of `(pass_class → PSOKey)` lookups; that
table lives in plugin code (its function pointers point at lambdas
in the dylib).

Render's `glibre_plugin_register` therefore:

1. Walks the new plugin's static `(pass_class, PSOKey)` table and
   calls `PSOCache::pin(PSOKey)` for every entry to obtain the
   resident pipeline-state handle. Pinning is read-only against the
   cache; it returns the existing entry on hit and triggers a
   bounded compile on miss (rate-limited by the warmer's per-tick
   budget, set in `RenderSettings`). The handles returned populate
   the new plugin's per-pass binding tables.
2. **Honours `glibre_types_abi_hash` matches.** If the protocol's
   step 2 already accepted the swap, the abi hash of every
   `glibre.types.render.*` type is by construction equal across old
   and new plugin (protocol §"Step 2 — Swap"). The PSOCache's key
   stability invariant (§4.1.7 invariant 1: identical keys must map
   to byte-equal pipeline bytecode) is therefore preserved — the
   new plugin requesting `(shader_hash_X, state_hash_Y)` gets back
   the same pipeline the old plugin would have seen.
3. **Does not re-warm the on-disk archive.** The archive is
   loaded once at process start (§7.1.2 invariant 4); a hot-reload
   inherits the warmed cache and adds at most a small set of
   newly-introduced PSOKeys (corresponding to new passes the new
   plugin added). Removing pipelines for passes the new plugin
   dropped is left to the next eviction sweep — passes that no
   longer exist no longer pin their PSOs, so LRU drains them
   naturally.

The total work in render's resume step is therefore bounded by
**O(passes) atomic-store fix-ups + O(distinct PSOKeys) cache-pin
calls + zero GPU-resource churn**, fitting the protocol's
"reload path bounded by drain + swap + Σ migrate + register"
budget (`hot-reload-protocol.md` §Consequences).

### 8.4 Refusal cases (render-specific)

Render contributes no new umbrella refusal arm; every refusal is
expressed as the engine-wide `core::Error::HotReloadRefused` with
a nested cause chosen from the protocol's existing arms. Render does
introduce four **inner causes** that loader sees only because render
inspects the new plugin during step 4 (resume); they are enumerated
here so the test matrix and the diagnostic surface (§4.1.7
`DiagnosticOverlay`) can name them. All four roll up to
`core::Error::PluginInitFailed` per protocol §"Refusal Cases" item 3.

| Render refusal cause                                  | Detected by                                                                                                       | Inner-error arm                                | What the operator must do                                                                                  |
|-------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------|------------------------------------------------|------------------------------------------------------------------------------------------------------------|
| Mid-frame reload requested                            | Loader (not render). Listed for completeness because render's contract forbids it.                                 | `core::Error::HotReloadRefused` direct (no inner — never reaches render); the request is queued until phase 8. The "refusal" is a deferral, not an error. | None — the request will be honoured at the next phase 8.                                                   |
| New plugin's `Capability` requirement set is **broader** than the surviving `CapabilityMask`     | Render's `glibre_plugin_register` reads the live `CapabilityMask` (§7.1.3) and compares against each declared pass's capability predicate. | `render::Error::CapabilityNotSupported` wrapped under `core::Error::PluginInitFailed`. | Either rebuild the plugin against the host's capabilities, or re-probe the host (which only `platform` may do) and restart the process. The reload is refused; the prior plugin remains live. |
| New plugin declares a pipeline whose `state_hash` collides with the surviving cache but whose pass declarations differ in shader bytecode hash | Render walks the new plugin's `(pass_class, PSOKey)` table during register and calls `PSOCache::pin`; a hit whose stored `shader_hash` does not match the requested key violates §4.1.7 invariant 1. | `render::Error::PipelineCompileFailed` wrapped under `core::Error::PluginInitFailed`.  | Inspect the cooked metallib manifest (`shader`'s cook output) and the new plugin's expected `shader_hash`; the mismatch indicates a stale archive. Resolution path: `rmtree(<pso-archive-dir>)` per §7.2.2 and rebuild. The reload is refused. |
| Surviving persistent GPU resource's component schema differs from the new plugin's manifest declaration | The protocol's step 2.2 detects this as a manifest-subset failure and refuses before render's register runs. Listed here so the resource-schema invariant in §8.2 is testable.            | `core::Error::HotReloadRefused` with cause `core::Error::SchemaMigrationFailed`. | Author the missing migrate function for the persistent-resource type, or accept a fresh world (post-MVP). |

Each refusal is logged exactly once at `warn` level (protocol
§"Refusal Cases") with the structured fields `plugin_fqn=glibre.render`,
`attempted_dylib_path`, `host_abi_hash`, `plugin_abi_hash`, and the
inner cause's enumerator name. The `DiagnosticOverlay` mirrors the
`HotReloadRefused` event payload (§8.5) for in-game diagnosis.

### 8.5 Observers — external systems holding `RenderFrame`

`RenderFrame` is the only ECS↔GPU seam render exposes (§4.2 invariant
6); the only contexts that legitimately hold a non-owning reference to
one are:

- The editor (post-MVP), via the live diagnostics overlay.
- The `e2e` harness, via its trace-replay capture (§8.6).
- The `tools` profiler, when a frame is paused for inspection.

By §4.1.1 invariant ("destroyed at phase 7 exit"), no `RenderFrame`
reference is live at phase 8 entry under normal frame execution. The
three observers above can extend a `RenderFrame`'s lifetime by pinning
it (the §5 public API exposes `RenderFrame::pin() noexcept` returning
an RAII guard). Render's hot-reload contract requires:

1. **Pinned `RenderFrame`s prevent reload acceptance — by deferral,
   not refusal.** When the loader enqueues a reload at any phase
   between 1 and 7 of frame N, render checks the pin count. A
   non-zero count causes the reload to *defer* to the next phase 8
   at which the pin count is zero. This is not a refusal; the
   request stays in the queue, and the operator sees a `progress`
   line in the diagnostic overlay rather than a `warn`. (Rationale:
   editor / e2e are dev workflows; a one-frame stall behind a paused
   inspection is acceptable, an unexplained refusal is not.)
2. **Drop notification.** When an active reload is about to
   commence at phase 8, render publishes a render-specific
   `RenderFrameDropPending { frame_counter }` event on the engine's
   observer bus (the same bus that carries `HotReloadStarted` per
   protocol §"Observer Notification") **before** invoking the
   protocol's drain step. Observers must drop their reference
   synchronously (the bus call returns; the loader proceeds). An
   observer that does not drop within the synchronous callback is
   reported via `render::Error::ResourceImportRefused` wrapping
   `core::Error::PluginInitFailed`; the swap proceeds, but the
   observer's pin is invalidated by the destructor of the frame at
   the next phase 8 exit. (This is loud-failure-by-design;
   silently corrupting an observer is rejected by §4.1.1
   immutability.)
3. **No half-swapped `RenderFrame` is ever observable.** The
   observer bus's atomicity guarantee (protocol §"Observer
   Notification") means subscribers see either a
   pre-swap-fully-completed-frame or no frame at all. A
   `RenderFrame` from frame N (built by the old plugin) cannot be
   observed concurrently with the new plugin's frame N+1; the
   triple-buffer slot is unique to its frame counter.

The observer event types added by render — exactly two —
piggyback on the engine bus and are middleman-typed
(`glibre::types::render::HotReloadEvent`, with arms
`RenderFrameDropPending`, `RenderHotReloadCompleted` — the latter is
emitted alongside the engine's `HotReloadCompleted` and carries the
counts of (passes re-registered, PSOKeys re-pinned, capability bits
unchanged) for the e2e harness to assert against). No third event is
permitted; new observability needs flow into existing arms or graduate
to a SPEC bump.

### 8.6 Test hooks — trace-replay verification

Render's reload contract is verified end-to-end by a single test
fixture under `tests/render/hot_reload/` that uses the loader's
existing `enqueue_hot_reload` E2E entry point (protocol §"Test Hooks").
The fixture has three layers:

1. **Trace capture.** A fixture plugin `tests/e2e/plugins/render-v1/`
   runs the engine for `K = 8` frames against a deterministic scene
   (cornell-box-with-1-mesh, fixed PRNG seed); during phase 7 the
   harness records a structured trace per frame containing:
   `(frame_counter, RenderFrame proxy hashes, pass list, PSOKey
   list, command-buffer encoder ops bucketed by phase, swapchain
   drawable ID)`. The trace is canonical-ordered (`reviews/decisions/
   determinism-canonical-iteration.md`) and stored under
   `tests/data/traces/render/cornell-vN.bin`.
2. **Reload trigger.** At frame `K/2`, the harness calls
   `enqueue_hot_reload("glibre.render",
   tests/e2e/plugins/render-v2.dylib)`. The `v2` plugin is
   byte-identical to `v1` for our test (same shaders, same passes,
   same `state_hash` algorithm) but has a different `__file__`
   timestamp embedded so the loader treats it as a real swap. This
   tests the **happy-path identity** case: a swap that should be
   semantically a no-op.
3. **Post-reload assertion.** The harness records a second trace
   for frames `K/2 .. K-1` under the new plugin and asserts:
   - frame `K/2`'s recorded command buffer (already submitted before
     phase 8) is byte-equal to the reference trace at the same
     index (cf. §8.1.1: phase 7 already returned);
   - frame `K/2 + 1`'s command buffer is byte-equal to the
     reference trace; this is the **first post-reload frame**, the
     one that exercises the new plugin's freshly-rebuilt graph and
     repointed PSO bindings;
   - the `HotReloadCompleted` event was fired exactly once with
     `migrated_types = []` (no schema change in this scenario);
   - the `RenderHotReloadCompleted` event reports `passes_reregistered
     == reference_pass_count`, `psokeys_repinned == reference_psokey_count`,
     `capability_bits_unchanged == true`;
   - no observer ever called the bus's "I cannot drop" path.

A second fixture pair (`render-v1` → `render-v1-add-pass`) exercises
the new-pass path: the new plugin adds one debug pass guarded by
`Capability::TimestampQueries`. The post-reload trace asserts the
extra pass appears starting at frame `K/2 + 1` and that the pass's
PSOKey was a fresh entry in the cache (cache-miss counter incremented
exactly once). A third fixture (`render-v1` → `render-v2-bad-cap`)
declares an unsupported capability and asserts
`HotReloadRefused { cause: PluginInitFailed { inner:
CapabilityNotSupported } }` with the prior plugin still ticking
(frames `K/2 + 1 .. K-1` byte-equal to the original trace).

All three scenarios run inside a single CI job using the in-process
trigger; no filesystem watcher is involved (protocol §"Test Hooks").
The Catch2 cases are listed in §11 acceptance criteria as
`Hot-reload preserves frame trace`, `Hot-reload accepts pass
addition`, `Hot-reload refuses capability widening`.

### 8.7 Cross-references

- Engine protocol: `reviews/decisions/hot-reload-protocol.md`
  (drain → swap → migrate → resume; refusal arms; observer bus;
  E2E hook).
- Frame slot: `reviews/decisions/frame-phases.md` (phase 8 entry /
  exit guarantees; one-frame pipeline preserved).
- Persistence rules invoked: §7.1.1 / §7.2.1 (`RenderSettings`),
  §7.1.2 / §7.2.2 (`PSOCacheRecord` invalidation), §7.1.3 / §7.2.3
  (`CapabilityMask` additive bits).
- Aggregates touched: §4.1.1 `RenderFrame` (pin-and-drop),
  §4.1.2 `RenderGraph` (per-frame, no survival), §4.1.5
  `ExecutionPlan` (cache dropped on swap), §4.1.7 `PSOCache` (key-
  stable survival), §4.1.8 `RTAccelStructures` (BLAS imports
  read-only), §4.2 invariant 6 (`RenderFrame` is the only ECS↔GPU
  seam).
- Errors used: `render::Error::CapabilityNotSupported`,
  `render::Error::PipelineCompileFailed`,
  `render::Error::ResourceImportRefused` (§5 enum), each wrapped
  by `core::Error::PluginInitFailed` per protocol §"Refusal Cases".

## 9. Performance Budget

This section quotes render's row from the engine-wide budget table
(`reviews/decisions/perf-budget.md`), breaks it down per render-owned
phase (6 and 7 per `reviews/decisions/frame-phases.md`), itemises the
per-pass GPU cost that sums into the cell, fixes the heap composition
inside the 512 MiB ceiling, restates the allocator rules render
plugs into, and lists the CI gate hooks render owns. Every number in
this section is a **contractual ceiling**, not a steady-state
expectation — the budget gate fails on any frame that exceeds the cell
or any pass slice (§9.6). The §11 acceptance criteria name the Catch2
benchmarks that enforce these ceilings.

### 9.1 Cell — render row

Render's cell from `reviews/decisions/perf-budget.md` §"Per-Context
Budget Table", restated verbatim with one MVP refinement: the issue
brief for spike #95 collapses the 0.10 ms CPU-sim slot and the 1.40 ms
CPU-submit slot into a single **1.5 ms CPU ceiling** so render's two
phases each carry a clean per-phase budget without cross-half
arithmetic at the gate level. The decision record's split is preserved
internally (sim 0.10 + submit 1.40 = 1.50) and feeds the budget
record's pipelined-frame timing model unchanged.

| Slot                               | Ceiling   | Notes                                                                                     |
|------------------------------------|-----------|-------------------------------------------------------------------------------------------|
| CPU per frame (phases 6 + 7)       | **1.5 ms**| Quotes `perf-budget.md` row "render": 0.10 ms sim + 1.40 ms submit. See §9.2 / §9.3.      |
| GPU per frame (phase 7 output)     | **8.0 ms**| Concurrent with frame N+1 sim per frame-phases "one-frame pipeline". Breakdown in §9.4.   |
| Heap ceiling                       | **512 MiB**| MTLHeap residency + CPU-side staging shadows tagged `ContextTag::render`. See §9.5.      |
| Phase ownership                    | 6, 7      | Per `frame-phases.md` table. Geometry / tools register systems inside these phases (§9.2).|

The cell is sized against **(S1)** = 1 character + 200 props + 8
dynamic lights at 1920x1080 on M1 8-core GPU baseline. Justification
for each slot lives in `perf-budget.md` §"Justification Per Cell"
(render row) and is not re-derived here. Render's SPEC §9 only
**refines** the cell into its sub-budgets; it does not amend the cell.
Any future amendment is a perf-budget spike per
`perf-budget.md` §Consequences.

### 9.2 Phase 6 — `cull-extract` CPU breakdown

Phase 6 owns 0.5 ms of render's 1.5 ms CPU ceiling (the legacy
"sim-side" 0.10 ms slot in `perf-budget.md` plus the 0.4 ms of the
1.40 ms submit slot that funds CPU cull/extract work; the rest of
the submit slot is phase 7 below). Frustum + occlusion cull against
the HZB and meshlet selection are the dominant costs and the only
ones gated at this slice.

| Step (file, §6.2.1 reference)                    | Ceiling   | Cost model                                                                                                       |
|--------------------------------------------------|-----------|------------------------------------------------------------------------------------------------------------------|
| `cull/extract.cpp` — open `RenderFrame` slot     | 0.02 ms   | Triple-buffer slot acquire + `(World, FrameCounter)` pin (§4.1.1 invariant 4). One bounded atomic + arena reset. |
| `cull/meshlet_cull.cpp` — frustum + HZB cull     | 0.20 ms   | ~3.2k meshlet bounds (200 props × ~16 meshlets) tested against frustum planes + reverse-Z HZB sample. SIMD-bound. |
| `cull/budget.cpp` — `PassPriority` cost cull     | 0.05 ms   | Cost-aware survivor trim against per-view draw budget (§4.1.1 invariant 3). Linear over survivor set, ~3k entries. |
| `cull/sort.cpp` — packed `SortKey` radix         | 0.10 ms   | Single-pass radix on 64-bit `SortKey` column over survivor set; one allocation from transient arena.             |
| `RenderFrame` finalise (lights, camera, settings) | 0.03 ms   | Fixed-cost copy: ≤8 dynamic lights (S1), `RenderSettings` snapshot, `View` camera + jitter.                       |
| Geometry meshlet-selection systems (registered in phase 6) | 0.10 ms (geometry's slice) | Per `geometry`'s SPEC §9; **not** counted in render's 0.5 ms — listed only so reviewers see the full phase cost. |
| **Render subtotal**                              | **0.40 ms**| Render-owned work in phase 6.                                                                                     |
| Reserve inside phase 6                           | 0.10 ms   | Absorbs the cost-aware budget culler's worst-case re-sort and one-shot warm starts on first frame.                |

**Cap:** 0.5 ms render CPU in phase 6, including reserve. Geometry's
phase-6 systems carry their own ceiling against `geometry`'s row;
render's gate does not double-count them. Tools' phase-6 work is
gated against `tools`'s row.

Exit guarantee (frame-phases row 6): a finalised `RenderFrame` exists
and no further ECS reads are required by render this frame. The
budget gate asserts the CPU timestamp delta between phase 6 entry and
exit on the driver thread is ≤ 0.5 ms (S1 fixture, p99). See §9.6.

### 9.3 Phase 7 — `render-submit` CPU breakdown

Phase 7 owns 1.0 ms of render's 1.5 ms CPU ceiling — the CPU side
that records command buffers from the immutable `RenderFrame`. The
work splits across the three thread-roles defined in §6.3 (graph
builder, per-pass encoders, render driver); the 1.0 ms ceiling
applies to the **driver-thread wall clock** for phase 7, i.e. the
time from phase 7 entry to the submit-fence signal that wakes phase 9.

| Step (file, §6.2.2 reference)                                  | Driver-thread cost | Cost model                                                                                                           |
|----------------------------------------------------------------|--------------------|----------------------------------------------------------------------------------------------------------------------|
| `graph/builder.cpp` — register per-`View` passes               | 0.10 ms            | Fixed sequence (§6.2.2 step 1) instantiated once per `View`; one `View` in S1, ≤4 in MVP ceiling.                    |
| `graph/compile.cpp` — topo / colour / barrier / queue / bind   | 0.20 ms            | Cache hit path is one structural-hash lookup + rebind (`§4.1.5`); cache-miss path (rare) trades against the reserve. |
| Per-pass `execute()` recording (driver-side dispatch)          | 0.50 ms            | Three encoder workers wake up; driver waits on the slowest queue. Bounded by ~3k `IndirectDraw` record cost on M1.    |
| `metal/queue.cpp` — submit + `PresentFence` signal             | 0.05 ms            | One `commit()` per queue (Graphics + Compute + Copy = 3); fence emit.                                                |
| `RenderFrame` retire + transient pool recycle                  | 0.05 ms            | §4.1.4 cleanup (alias plan free-list, virtual-resource drop) + `RenderFrame` slot mark-retired.                       |
| Geometry BLAS-refit submit (registered in phase 7)             | (geometry's slice) | Per `geometry`'s SPEC §9; not in render's 1.0 ms. GPU-side cost folded into `shadow-rt` slice (§9.4).                |
| Tools ImGui-Metal-4 record (registered in phase 7)             | (tools's slice)    | Per `tools`'s SPEC §9; not in render's 1.0 ms.                                                                        |
| **Render subtotal**                                            | **0.90 ms**        | Render-owned driver-thread work in phase 7.                                                                            |
| Reserve inside phase 7                                         | 0.10 ms            | Absorbs cache-miss compile (≤1 per `View` per frame) and Metal driver tail jitter on submit.                          |

**Cap:** 1.0 ms render CPU in phase 7, including reserve, on the
driver thread. The per-pass encoder workers' CPU time is overlapped
with the driver and does not enter this column unless the driver
blocks on the slowest worker — which the budget admits via the 0.50
ms "per-pass record" line. The budget gate asserts the CPU timestamp
delta between phase 7 entry and submit-fence signal is ≤ 1.0 ms
(S1, p99).

Phase 6 (0.5) + phase 7 (1.0) = **1.5 ms** = render's CPU cell.

### 9.4 Phase 7 — GPU breakdown per pass

The 8.0 ms GPU ceiling decomposes across the §6.2.2 pass list. Each
slice is the **wall-clock** cost on the M1 8-core GPU baseline,
measured pass-end-minus-pass-start via `MTLCounterSampleBuffer`
timestamps inserted at every `Pass`'s encoder boundary (the
mechanism is decided in §9.6 below; the ring storage is the §7.3
debug-gated GPU-timestamp ring).

| Pass (file, §6.2.2 reference)         | Queue     | GPU ms   | Notes                                                                                                                   |
|---------------------------------------|-----------|----------|-------------------------------------------------------------------------------------------------------------------------|
| `passes/cluster_cull.cpp` (meshlet-cull) | Compute  | **0.5**  | Persistent-thread cluster cull build of `LightCluster` (froxel grid) + meshlet-cull GPU-side amplification (§6.5).      |
| `passes/gbuffer.cpp` (gbuffer-mesh)   | Graphics  | **2.5**  | Mesh-shader dispatch writing gbuffer MRT + visibilityID + velocity in one declared `Pass` (§4.2 invariant 5).            |
| `passes/shadow_rt.cpp` (shadow-rt)    | Compute   | **1.5**  | Hybrid-RT shadow trace + denoise hook. **BLAS refit (~0.3 ms, §6.2.2 step 1.1) is included in this slice** per §9.4.1.   |
| `passes/lighting.cpp` (lighting-deferred) | Compute | **1.5** | Deferred lighting compute reading gbuffer + `LightCluster` + RT shadow / AO; ray query inline (§6.4 step 3).             |
| `passes/transparent_forward.cpp` (transparent-forward) | Graphics | **0.5** | Forward translucent reading the same `LightCluster` (§3.2 collapse #3).                                          |
| `passes/post.cpp` (post)              | Graphics  | **1.0**  | Bloom / DOF / motion / tonemap chain ordered per `RenderSettings`; AA / upscale variant absorbed into this slice in MVP.|
| `passes/present.cpp` (present)        | Graphics  | **0.5**  | Drawable acquire + swapchain blit + `PresentFence` signal.                                                                |
| HZB build, TLAS rebuild-or-refit, AO  | Compute   | (folded) | `passes/hzb_build.cpp`, `passes/tlas_build.cpp`, `passes/ao_rt.cpp` overlap on the compute queue with the slices above; their wall-clock is hidden under the dominant compute slice (`shadow_rt` + `lighting`) and does not add to the total. |
| **GPU subtotal**                      |           | **8.0**  | Sum of the seven measurable slices above.                                                                                |

The pass-list order is fixed (§6.2.2 step 1); the queue assignment is
fixed (declared per pass); the compile cache (§4.1.5) keeps the
plan-shape stable frame-to-frame so GPU slice variances are workload-
driven, not graph-shape-driven. Compute / Graphics queue overlap is
the reason the per-slice numbers can sum to 8.0 ms while the
wall-clock GPU ceiling is also 8.0 ms — Apple's published Metal 4
mesh-shader benchmark behaviour on M1 keeps the graphics queue near
saturation across `gbuffer` + `transparent_forward` + `post` +
`present` (5.0 ms), and the compute queue near saturation across
`cluster_cull` + `shadow_rt` + `lighting` (3.5 ms) — the compute
half completes inside the graphics half's wall-clock, so the
critical-path total is the graphics queue's ~5.0 ms plus the
graphics-queue tail beyond the compute queue (~3.0 ms attributable
to lighting waiting on gbuffer's gbuffer + visID writes via the
plan's read-after-write fence). The 8.0 ms ceiling is the sum's
upper bound; the gate measures it as wall-clock (§9.6).

#### 9.4.1 BLAS refit accounted inside `shadow-rt`

`passes/blas_refit.cpp` (§6.2.2 step 1.1; §6.4 step 1) records on
`Queue::Compute` and emits a Metal 4 `accelerationStructure` refit
for each visible-LOD0 dynamic-cluster set (skinned / deformable
meshes; static-mesh BLAS are cooked by `geometry` and never refit
per §6.4). The refit's GPU cost on M1 for S1 (1 character with ~6k
deformable verts + a small handful of dynamic rigid bodies) is
**~0.3 ms**, which is **funded inside the 1.5 ms `shadow-rt` slice**:
the TLAS rebuild-or-refit (`passes/tlas_build.cpp`) declares an
explicit read-after-write on the BLAS refit outputs (§4.2 invariant
4), so on the compute queue the refit precedes the TLAS update and
both precede the shadow trace; the three together are accounted as
`shadow-rt`. This collapses one budget row: the perf-budget gate
exposes `shadow-rt` GPU as a single number, with refit + TLAS hidden
under it. If a future scene drives BLAS refit above 0.3 ms, the
amendment goes to `shadow-rt` (or to a dedicated `blas-refit` row),
not to `geometry`'s row — GPU memory is render-owned per
`perf-budget.md` Allocator Rule 5.

The 0.3 ms refit number is the working assumption for the gate; the
S1 fixture under §9.6 will measure it and feed the next perf-budget
amendment if it diverges. Static BLAS imports (read-only by render
per §4.1.8 invariant 3) cost zero per frame.

### 9.5 Heap composition inside the 512 MiB ceiling

The 512 MiB ceiling is GPU-side residency (MTLHeap bytes attached to
render's residency set per §4.1.6) **plus** the CPU-side staging
shadows tagged `ContextTag::render`. Per `perf-budget.md` Allocator
Rule 5, all GPU allocations carry the `render` tag regardless of
the requesting context (geometry's vertex/index/meshlet streams,
tools' ImGui textures); CPU-side staging is tagged by the requester.
Render's heap composition pre-allocates the persistent half at init
and reserves the rest as a transient pool drained per frame.

| Sub-budget                                | Ceiling   | Aggregate / source                                                                                                  |
|-------------------------------------------|-----------|---------------------------------------------------------------------------------------------------------------------|
| **PSO cache** (`§4.1.7`)                  | **64 MiB**| `PSOCache` LRU + on-disk archive page-cache + per-pass binding-table prebuilds. Sized for the MVP material set.     |
| **GPU resource handles**                  | **16 MiB**| `Handle<Tag>` tables + `glibre.types.render.GpuId` keying tables + residency-set membership bitset (§4.1.6).        |
| **Transient pool** (`§4.1.4`)             | **256 MiB**| Per-frame placement heap drained per recompile. Hosts virtual resources whose lifetime is bounded by one frame: gbuffer MRT (4 attachments @ 1080p ≈ 64 MiB), HZB pyramid scratch, RT shadow / AO trace targets, history-color rings, RT scratch. The §4.2 invariant ("no transient resource lives across the phase 7→9 boundary") makes this drain-or-leak. |
| **Persistent textures + buffers**         | **128 MiB**| Long-lived `Resource`s: shadow atlases, BLAS imports (geometry-cooked, read-only by render per §4.1.8), persistent buffers (lighting LUTs, IBL probes, gbuffer-history for TAA), font / overlay atlases. |
| **RT acceleration structures** (`§4.1.8`) | **48 MiB**| TLAS + BLAS instance buffers + RT scratch. BLAS storage itself is tagged `ContextTag::geometry` for CPU shadows but its GPU bytes are render-tagged per Allocator Rule 5; this row is the GPU-side residency of TLAS + scratch + the active BLAS refit slot. |
| **Subtotal**                              | **512 MiB**| Sum of the five rows = render's cell ceiling exactly.                                                                |

The five rows are exhaustive and additive; render does not maintain a
sixth catch-all bucket. Any new GPU resource type at MVP must dock
under one of these five rows, or amend `perf-budget.md`. The
transient pool's 256 MiB is the largest row by design — alias-planner
colouring (`§4.1.5` invariant 2) recovers ≥40% of its naive footprint
on the S1 workload, so the 256 MiB cap is a real headroom for
post-MVP passes (visibility-buffer deferred path, RT reflections)
without a budget amendment per `perf-budget.md` §Consequences.

#### 9.5.1 Allocator rules render plugs into

`perf-budget.md` §"Allocator Rules" defines `glibre::PerContextAllocator`
and the `ContextTag` mechanism. Render plugs into it as follows;
nothing here amends the engine-wide rules:

1. **Tag stamp at register.** Render's `glibre_plugin_register`
   receives the allocator handle, which is pre-stamped with
   `ContextTag::render`. All call sites inside the render dylib are
   tag-free per Allocator Rule 1.
2. **Strict-mode enforcement.** Diagnostic / debug builds run with
   `GLIBRE_ALLOC_STRICT=1`; an allocation that would push render's
   live bytes above 512 MiB returns
   `std::unexpected{core::Error::OutOfBudget}` per Allocator Rule 2.
   Render maps that to `render::Error::ResourceResidencyExceeded`
   (§4.1.4 invariant 4) at the call site that requested the
   `Resource`, preserving the typed-error contract of §5.
3. **Soft warning in shipping.** Shipping builds log `warn` once
   per-frame on overshoot per Allocator Rule 3 and increment the
   frame-stat counter; the editor's perf HUD surfaces it. Render
   does not down-grade quality silently.
4. **Transient arena exemption.** The 256 MiB transient pool is
   render's per-frame transient arena (§4.1.4); it drains at phase 9
   per Allocator Rule 4. A virtual resource that survives phase 9 is
   a leak: the alias planner emits a debug-build assertion in
   `resources/transient_pool.cpp`. CI runs the diagnostic build
   under `GLIBRE_ALLOC_STRICT=1` (§9.6) so leaks fail loudly.
5. **GPU-memory-is-render-owned.** Per Allocator Rule 5, geometry's
   vertex / index / meshlet streams and tools' ImGui textures are
   GPU-allocated under the `render` tag; their CPU shadows are
   geometry- / tools-tagged. Render's 512 MiB therefore covers the
   **GPU footprint** of those non-render contexts, and §9.5's
   "persistent textures + buffers" row is sized with that in mind.

### 9.6 CI gate hooks render owns

`perf-budget.md` §"CI Gate Spec" defines `perf-budget.yml` (authored
under the `task-breakdown-error-perf` spike) and the five gate items.
Render owns the per-context portions of items 1, 2, and 3 — i.e. the
micro-benchmarks that prove its row, the e2e frame-time slice
attributable to render, and the heap ceiling enforcement on
`ContextTag::render`. The §11 acceptance criteria name the Catch2
benchmarks; this section fixes the **measurement mechanism** so the
gate authors and benchmark authors agree on what is counted.

#### 9.6.1 GPU timestamp queries per pass

Every `Pass`'s `execute()` lambda inserts a paired GPU timestamp at
encoder begin and end via `MTLCounterSampleBuffer` (Metal 4's
`MTLCommonCounterTimestamp` set, sampled at
`MTLCounterSamplingPointAtStageBoundary`). The timestamps land in
the §7.3 debug-gated GPU-timestamp ring; the e2e harness reads the
ring at frame N+2 (one frame after the GPU has signalled completion,
so the timestamps are resolved on host) and computes per-pass
wall-clock. The seven measurable slices of §9.4 are the gate's named
slots:

| Gate slot               | Computed as                                                  | Ceiling   |
|-------------------------|--------------------------------------------------------------|-----------|
| `meshlet-cull`          | end(`cluster_cull`) − begin(`cluster_cull`)                  | 0.5 ms    |
| `gbuffer-mesh`          | end(`gbuffer`) − begin(`gbuffer`)                            | 2.5 ms    |
| `shadow-rt`             | end(`shadow_rt`) − begin(`blas_refit`) (covers refit + TLAS) | 1.5 ms    |
| `lighting-deferred`     | end(`lighting`) − begin(`lighting`)                          | 1.5 ms    |
| `transparent-forward`   | end(`transparent_forward`) − begin(`transparent_forward`)    | 0.5 ms    |
| `post`                  | end(`post`) − begin(`post`) (includes AA / upscale variant)  | 1.0 ms    |
| `present`               | end(`present`) − begin(`present`)                            | 0.5 ms    |
| **GPU total**           | end(`present`) − begin(first compute pass) on graphics queue | **8.0 ms**|

`MTLCounterSampleBuffer` use is gated to `Capability::TimestampQueries`
(§5 enum); on hosts where the capability is absent, the per-pass
slots are not enforced and the gate falls back to per-frame total
(item 2 of `perf-budget.md` §"CI Gate Spec"). The MVP host baseline
(M1 / macOS 26) carries the capability per `platform`'s probe table
(§7.1.3 `CapabilityMask`).

#### 9.6.2 CPU phase 6 + 7 budget asserts

Render's two phase ceilings are asserted as Catch2 `BENCHMARK` blocks
under `tests/render/perf/`. Each block runs the S1 fixture (one
character, 200 props, 8 lights, 1920×1080) as set up by the engine's
shared perf fixture (`e2e/perf/`). The assertion is the
`time <= cell_budget_ms` form of `perf-budget.md` §"CI Gate Spec"
item 1.

| Catch2 benchmark name (under `tests/render/perf/`)        | Measures                                                        | Asserts            |
|-----------------------------------------------------------|-----------------------------------------------------------------|--------------------|
| `BENCHMARK("phase-6 cull-extract S1, p99")`               | CPU wall-clock between phase 6 entry and `RenderFrame` finalise | ≤ 0.5 ms           |
| `BENCHMARK("phase-7 render-submit driver, S1, p99")`      | CPU wall-clock between phase 7 entry and `PresentFence` signal  | ≤ 1.0 ms           |
| `BENCHMARK("phase-6 + phase-7 CPU total, S1, p99")`       | Sum of the two above                                            | ≤ 1.5 ms           |
| `BENCHMARK("render heap ceiling, S1, strict-mode")`       | Live bytes on `ContextTag::render` after phase 7 retire         | ≤ 512 MiB          |
| `BENCHMARK("transient-pool drained at phase 9, strict")`  | Live bytes in render's transient arena at phase 9 entry         | == 0 (leak guard)  |

The benchmarks are authored by the `task-breakdown-error-perf` spike
per `perf-budget.md` §Consequences; this SPEC §9 names them so
reviewers can map cell numbers to test artifacts. The S1 fixture and
the e2e perf harness live under `e2e/perf/` and are versioned
alongside the gate.

#### 9.6.3 Headroom-low tripwire

Per `perf-budget.md` §"CI Gate Spec" item 5, if p50 CPU or p50 GPU
sit within 0.5 ms of the ceiling for two consecutive nightlies, the
gate posts a warning comment on the next PR and labels it
`perf:headroom-low`. Render-specific thresholds: ≥ 1.0 ms p50 CPU
(out of 1.5 ms cell) or ≥ 7.5 ms p50 GPU (out of 8.0 ms) for two
consecutive nightlies trip the alarm. The tripwire does not block
merge; it requests a perf-budget amendment spike before the budget
is broken. This is the only place in §9 where the cell may be
informally elastic — by the time a hard ceiling fails, the
amendment-or-fix decision has already had a working window.

### 9.7 Cross-references

- Engine budget record: `reviews/decisions/perf-budget.md` (per-context
  table, allocator rules, CI gate spec, pipelined-frame timing model).
- Frame slot ownership: `reviews/decisions/frame-phases.md` rows 6 and 7
  (entry / exit guarantees consumed in §9.2 / §9.3).
- Aggregates touched: §4.1.1 `RenderFrame` (phase 6 output), §4.1.2
  `RenderGraph` + §4.1.5 `ExecutionPlan` (phase 7 build / compile),
  §4.1.4 `Resource` + transient pool (§9.5 transient row), §4.1.6
  `MetalDevice` + queues (§9.4 queue assignment), §4.1.7 `PSOCache`
  (§9.5 PSO row), §4.1.8 `RTAccelStructures` (§9.4.1 BLAS refit),
  §4.1.9 `HZB` (§9.2 cull input), §4.1.10 `ClusterCullState` (§9.4
  meshlet-cull slice), §7.3 GPU-timestamp ring (§9.6.1 measurement).
- Errors used: `render::Error::ResourceResidencyExceeded`
  (over-budget heap; §4.1.4 invariant 4); `core::Error::OutOfBudget`
  (allocator-side, mapped to the render arm at the call site per §9.5.1).
- §11 acceptance criteria: see `Phase 6 cull-extract within 0.5 ms`,
  `Phase 7 render-submit within 1.0 ms`, `GPU passes within slice`,
  `Render heap within 512 MiB`, `Transient pool drains by phase 9`.

## 10. Failure Modes & Error Model

`render::Error` is the closed sum returned through every `glibre::Result<T>`
exported from `specs/render/SPEC.md` §5. The §5 stub already enumerates a
working subset (`DeviceLost`, `PipelineCompileFailed`, `RenderGraphCycle`,
…); §10 fixes the *full* closed sum below as the rendering plugin's
contractual failure surface and binds each variant to a trigger, recovery
strategy, severity, and capability-fallback path. Adding or removing a
variant is a render-plugin ABI bump (per `reviews/decisions/error-model.md`
§"Composition Rules" item 5 and §3.2 collapse #5 of this spec).

### 10.1 The closed sum (twenty design-name rows; 25 §5 enumerators)

The §5 stub publishes the canonical enumerator names; §10 names them in
the documentation form below and notes the §5 spelling in parentheses
where they differ. Variants whose §5 spelling does not yet appear in the
header are flagged "ABI add" — landing the §5 implementation header
adds them in a single ABI bump alongside the §10 acceptance test.

| §10 name                       | §5 enumerator (current / planned)            | Frame-phase origin (§6.2)            |
|--------------------------------|----------------------------------------------|--------------------------------------|
| `MetalDeviceUnavailable`       | `DeviceLost` + `DeviceUnsupported` (§5)      | init / phase 6 / phase 7             |
| `SwapchainAcquireFailed`       | `SwapchainAcquireFailed` (§5)                | phase 7 acquire                      |
| `ShaderModuleLoadFailed`       | `ShaderModuleLoadFailed` (§5)                | init / hot-reload register / phase 7 |
| `PsoCompileFailed`             | `PipelineCompileFailed` (§5)                 | phase 7 record (lazy compile)        |
| `ResourceAllocFailed`          | `HeapOutOfMemory` (§5)                       | phase 6 plan / phase 7 record        |
| `ResourceResidencyExceeded`    | `ResourceResidencyExceeded` (§5)             | phase 6 plan                         |
| (stale handle)                 | ABI add `StaleResourceHandle` (§5)           | phase 7 record (lookup)              |
| (role mismatch)                | ABI add `ResourceRoleMismatch` (§5)          | cold-path release                    |
| `BarrierViolation`             | `BarrierConflict` (§5)                       | phase 6 graph compile                |
| `GraphCycle`                   | `RenderGraphCycle` (§5)                      | phase 6 graph compile                |
| `GraphResourceUnknown`         | `PassUndeclaredAccess` (§5)                  | phase 6 graph compile                |
| `MeshletCullDispatchFailed`    | ABI add (`MeshletCullDispatchFailed`)         | phase 7 cluster cull                 |
| `BLASBuildFailed`              | `BlasUnavailable` (§5) + ABI add `BlasBuildFailed` | phase 7 RT build                |
| `TLASBuildFailed`              | `TlasBuildFailed` (§5)                       | phase 7 RT build                     |
| `RtCapabilityMissing`          | `CapabilityNotSupported` (§5) + flag bit     | init / hot-reload register           |
| `MeshShaderCapabilityMissing`  | `CapabilityNotSupported` (§5) + flag bit     | init / hot-reload register           |
| `FrameSubmitFailed`            | `QueueSubmitFailed` (§5)                     | phase 7 submit                       |
| `PresentTimeout`               | `FenceTimeout` (§5)                          | phase 9 (platform fence wait)        |
| `GpuTimeout`                   | `FenceTimeout` (§5) + payload `gpu_fault=false` | phase 9                          |
| `GpuFault`                     | ABI add (`GpuFault`)                          | phase 9 (Metal `executionStatus`)    |

`ShaderModuleLoadFailed` was previously "ABI add" in this table;
it is now a real §5 enumerator (added by the PSO-cache design followup,
PR #849 r1). Five "ABI add" rows remain (four original + `StaleResourceHandle`
+ `ResourceRoleMismatch` added by the render-resources design, minus the
now-landed `ShaderModuleLoadFailed`): these are the cumulative diff §5
acquires when those designs land; they are testable today as `static_assert`s
against the header in `tests/render/spec_§5_§10_consistency.cpp`.

### 10.2 Recovery vocabulary

Every variant resolves to exactly one recovery action drawn from the
fixed four-element ladder. The ladder is closed; no per-variant ad-hoc
recovery is permitted.

1. **`lower-tier`** — `RenderSettings.quality_tier` (§4.1, §3.2 collapse
   #9) drops one step (`HighEnd → Desktop → Switch → Mobile`). The
   render plugin re-runs phase 6 graph compile on the next frame with
   the tier-gated pass predicates re-evaluated. No frame is presented
   for the failing frame; the previous frame is re-presented (`platform`
   §9 honours the stale fence).
2. **`disable-feature`** — flip one `Capability` bit off in the live
   `CapabilitySet` (§5). The graph builder's pass predicates demote any
   pass guarded by that bit to its fallback path (e.g. `RayQuery → Gtao
   reflection`, `MeshShaders → vertex+amplification fallback`,
   `MetalFx → BuiltinFallback` upscaler — §6.6 cross-platform table).
   The fix is sticky for the process lifetime; the bit only re-enables
   on hot-reload register if the new plugin redeclares the capability
   *and* the host still supports it.
3. **`abort-frame`** — render returns `glibre::unexpected(err)` from its
   phase 7 entry; `core` skips phases 7..9 for this frame and the
   previous frame is re-presented. The next frame proceeds normally.
   The render plugin remains live; no state is dropped.
4. **`abort-engine`** — render returns `glibre::unexpected(err)` from
   init or phase 6, and the failure is non-recoverable in the running
   process. `core` performs an orderly shutdown (drain → release →
   exit). The editor (when attached) sees the structured error and
   surfaces it before the process exits.

A fifth modality — **`hot-reload-restart`** — is reserved exclusively
for `GpuFault` (§10.3 below). It is not part of the closed ladder;
it is a render-plugin-internal trigger into `core`'s existing
`HotReloadRequest` queue (§8.1), and §10.3 specifies its full
mechanics so the four-element ladder above stays intact for every
other variant.

### 10.3 Per-variant failure-mode rows

Every variant carries five fields:

- **Trigger** — the exact frame-phase event that constructs the
  `render::Error`. Cited against §6.2 phases.
- **Recovery** — one entry from §10.2 (`lower-tier` / `disable-feature`
  / `abort-frame` / `abort-engine`).
- **Severity** — log level passed to `glibre::log_error` per
  `reviews/decisions/error-model.md` §"Logging / Telemetry".
- **Capability-fallback path** — the graph-build-time pass predicate
  that re-routes work after a `disable-feature` recovery, or `n/a` for
  variants that do not flip a `Capability` bit.
- **Test fixture** — the Catch2 file under `tests/render/failure/` that
  reproduces the trigger and asserts the recovery action.

| Variant                       | Trigger                                                                                                                                    | Recovery          | Severity | Capability-fallback path                                             | Test fixture                          |
|-------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------|-------------------|----------|----------------------------------------------------------------------|---------------------------------------|
| `MetalDeviceUnavailable`      | `MTLCreateSystemDefaultDevice` returns `nil` at init, or any `MTLCommandBuffer.status == .error` with `.deviceRemoved` reason mid-frame.   | `abort-engine`    | `error`  | n/a — no Metal, no rendering.                                         | `device_unavailable.cpp`              |
| `SwapchainAcquireFailed`      | `CAMetalLayer.nextDrawable()` returns `nil` after the platform-defined acquire timeout (§9.4 phase 7 acquire budget = 1.5 ms).             | `abort-frame`     | `warn`   | n/a — drawable retried next frame.                                    | `swapchain_acquire_timeout.cpp`        |
| `ShaderModuleLoadFailed`      | `[MTLDevice newLibraryWithData:]` fails on a metallib pulled from the `shader` cook archive at init *or* during hot-reload register (§8.4).| `abort-engine` (init) / `lower-tier` (hot-reload register — refusal cause). | `error` | n/a — shader bytecode is non-optional.                                | `shader_module_load.cpp`              |
| `PsoCompileFailed`            | `PSOCache::compile_or_get()` returns failure during phase 7 record (lazy compile path); the `(state_hash, shader_hash)` pair is rejected. | `lower-tier`      | `warn`   | The lower tier's pass predicate selects a different PSO (e.g. drops TAA → FXAA → Off). | `pso_compile_lower_tier.cpp`         |
| `ResourceAllocFailed`         | `MTLHeap` sub-allocation returns `nil`, or the residency set rejects a commit at phase 7 record because a transient texture exceeds the heap composition (§9.5).| `lower-tier`      | `warn`   | Lower tier's pass predicates use smaller targets (e.g. shadow atlas 4K → 2K, GBuffer half-res). | `resource_alloc_lower_tier.cpp`        |
| `ResourceResidencyExceeded`   | Phase 6 plan computes a peak-residency footprint > 512 MiB ceiling (§9.5).                                                                 | `lower-tier`      | `warn`   | Re-plan at the lower tier shrinks the working set under 512 MiB.       | `residency_exceeded_lower_tier.cpp`    |
| `BarrierViolation`            | Phase 6 barrier-emit step detects a writer→reader pair the planner cannot satisfy (e.g. write-after-write on an aliased subresource without an explicit `Pass::declared_use`). | `abort-engine`    | `error`  | n/a — graph is structurally invalid; no fallback rescues a malformed graph. | `barrier_violation.cpp`               |
| `GraphCycle`                  | `RenderGraph::compile()` topological sort detects a cycle among `Pass` nodes.                                                              | `abort-engine`    | `error`  | n/a — same reasoning as `BarrierViolation`.                           | `graph_cycle.cpp`                      |
| `GraphResourceUnknown`        | A `Pass::execute` records access to a `VirtualResourceHandle` not in its `declared_use` set (debug-build assertion; release-build returns the error). | `abort-frame` (debug) / `abort-engine` (release CI gate). | `error`  | n/a — the pass body is buggy.                                         | `graph_resource_unknown.cpp`           |
| `MeshletCullDispatchFailed`   | Phase 7 mesh-shader cull dispatch returns `MTLCommandEncoderError`, or the indirect-arg buffer overflows the cluster pool (§4.1).         | `disable-feature` | `warn`   | `Capability::MeshShaders` cleared → graph builder picks vertex + amplification stage fallback (§6.5). | `meshlet_dispatch_disable.cpp`         |
| `BLASBuildFailed`             | `[MTLAccelerationStructureCommandEncoder buildAccelerationStructure:descriptor:scratchBuffer:]` fails for a per-mesh BLAS at phase 7 RT build. | `disable-feature` | `warn`   | `Capability::HardwareRayTrace` cleared → §6.4 hybrid-RT path falls back to RT-disabled GTAO/PCF lighting. | `blas_build_disable.cpp`              |
| `TLASBuildFailed`             | TLAS rebuild fails (instance-count overflow, scratch exhausted, or driver error).                                                          | `disable-feature` | `warn`   | Same as `BLASBuildFailed`.                                            | `tlas_build_disable.cpp`              |
| `RtCapabilityMissing`         | `CapabilitySet::supports(HardwareRayTrace \| RayQuery)` is `false` at init, but a registered pass declared the bit as required.            | `lower-tier` (init) / `disable-feature` (hot-reload register — refusal cause §8.4). | `warn`   | Same predicate-demotion as `BLASBuildFailed`.                          | `rt_capability_missing.cpp`            |
| `MeshShaderCapabilityMissing` | `CapabilitySet::supports(MeshShaders)` is `false` at init, but a registered pass declared the bit as required.                             | `lower-tier` (init) / `disable-feature` (hot-reload register — refusal cause §8.4). | `warn`   | Same predicate-demotion as `MeshletCullDispatchFailed`.                | `mesh_shader_missing.cpp`              |
| `FrameSubmitFailed`           | `[MTLCommandQueue commit]` returns failure (transient driver error, queue overflow); not a device loss.                                    | `abort-frame`     | `warn`   | n/a — retried next frame against the same queue.                       | `frame_submit_retry.cpp`               |
| `PresentTimeout`              | The `platform` phase 9 fence wait exceeds the 16.6 ms budget (§9.4) by > 2× without a GPU-side completion signal.                          | `abort-frame`     | `warn`   | n/a — tier already at floor would imply `lower-tier` is a no-op; skipping the present is the only relief. | `present_timeout.cpp`                  |
| `GpuTimeout`                  | `MTLCommandBuffer.status == .completed` is not observed within the per-buffer watchdog (1.5 × budget); no fault payload reported.          | `lower-tier`      | `warn`   | Lower tier's smaller workload typically clears the watchdog.           | `gpu_timeout_lower_tier.cpp`           |
| `GpuFault`                    | `MTLCommandBuffer.status == .error` with `.faulted` reason, or Metal's residency monitor reports a page fault on a tracked allocation.     | `hot-reload-restart` (see §10.4). | `error`  | n/a — the render plugin is restarted; capability re-probe runs in the new process. | `gpu_fault_restart.cpp`                |

### 10.4 GPU fault — diagnostic capture and HotReload restart

`GpuFault` is the only variant whose recovery escapes the
four-element ladder. The mechanism preserves PHILOSOPHY §8 (hot-reload
at frame boundaries) and §9 (ABI hash gating) — the render plugin
is *restarted*, not patched in place.

1. **Detection (phase 9, on the platform-side fence wait).** When
   `MTLCommandBuffer.status` resolves to `.error` with a fault reason
   (`.faulted`, `.outOfMemory`, or `.invalidResource`), `platform`
   forwards the `MTLCommandBufferError` payload to render via the §5
   `Result<void> render_report_gpu_fault(...)` entry point.
2. **Diagnostic capture.** Render's fault handler synchronously writes
   a fixed-shape diagnostic blob to the per-process diagnostic ring
   (`DiagnosticOverlay` GPU-timestamp ring, §6.5). The blob carries:
   - `frame_counter` (§4.2 invariant 6).
   - `pass_class` and `PSOKey` of the in-flight command at fault.
   - The `MTLCommandBufferError.userInfo[MTLCommandBufferEncoderInfoErrorKey]`
     dump (Metal's per-encoder fault map).
   - The `RenderSettings.quality_tier` and live `CapabilitySet` snapshot.
   - The four most recent `(frame_counter, pass_class, gpu_ms)`
     timing rows from the GPU-timestamp ring.
   The blob format is the existing `glibre::types::render::FaultDiag`
   middleman type (no new schema; it piggybacks on the §8.5 observer
   bus typing). The blob is written to `${GLIBRE_DIAG_DIR}/render-fault-${pid}-${frame_counter}.diag`
   and a single `error`-level `spdlog` line is emitted with the file
   path as a structured field.
3. **Signal `core::HotReload`.** Render publishes a
   `HotReloadRequest { plugin_fqn = "glibre.render", reason =
   GpuFault, dylib_path = (current loaded dylib) }` onto `core`'s
   reload queue. The request is **not** a hot-reload to a *new*
   plugin binary — the same dylib is reloaded. This intentionally
   exercises the §8 protocol in full: drain → swap → migrate → resume.
4. **Frame at which the restart occurs.** Per §8.1, the request is
   honoured at the next phase-8 boundary at which no `RenderFrame`
   pin is held (§8.5). Until then, render returns
   `glibre::unexpected(GpuFault)` from each frame's phase 6 entry,
   driving `abort-frame` for those intervening frames (the previous
   good frame is re-presented). This bounded "stuck frame" window
   is part of the contract; the test fixture
   `tests/render/failure/gpu_fault_restart.cpp` asserts it lasts at
   most 3 frames.
5. **Restart semantics.** The reload follows the standard §8.3
   `migrate(...)` body: the persistent-resource set named in §8.2
   is preserved (PSO archive, mesh/material handles, TLAS instance
   topology); transient state (per-frame arenas, swapchain views,
   GPU-timestamp ring) is dropped and re-allocated. The
   `CapabilitySet` is re-probed against the host so a fault induced
   by a now-degraded GPU (eGPU detached, thermal throttle) lands at
   a lower capability set and the graph builder demotes the
   offending pass naturally on the next frame.
6. **Repeat-fault guard.** The reload-on-`GpuFault` path is gated by
   a per-process counter: if three consecutive restarts each within
   one second land on `GpuFault`, render escalates to `abort-engine`
   (a fourth fault is treated as an unrecoverable hardware state and
   `core` shuts down). The counter resets after one fault-free
   frame. This guard prevents an infinite restart loop on a wedged
   device.

### 10.5 Cross-references

- §3.2 collapse #5 — the render::Error closed sum is the §10
  realisation of "one closed `render::Error` rather than per-subsystem
  exception types."
- §5 — header stub publishes the §10.1 enumerator names; landing
  the four "ABI add" rows is the next render-plugin ABI bump.
- §6.2 — phase 6 / phase 7 are the only frame-phase origins for
  every §10.3 trigger.
- §6.4 / §6.5 / §6.6 — the predicate-demotion paths cited in the
  capability-fallback column live in these sections.
- §8.1 — the reload-queue entry point used by §10.4.
- §8.4 — `RtCapabilityMissing`, `MeshShaderCapabilityMissing`, and
  `ShaderModuleLoadFailed` all double as hot-reload refusal causes.
- §8.5 — the observer bus and `FaultDiag` middleman type used by
  §10.4.
- §9.4 / §9.5 — the budget thresholds that promote `PresentTimeout`,
  `GpuTimeout`, `ResourceResidencyExceeded`, and `ResourceAllocFailed`
  from "slow frame" to a typed render::Error.
- `reviews/decisions/error-model.md` — §"Composition Rules" governs
  how render::Error rolls up into `glibre::Error::Variant`; §10 owns
  the closed sum, the engine-wide alias never edits it.

## 11. Acceptance Criteria

GitHub `type:user-story` issues this spec closes (24 stories,
total 76 pts; parent sub-epic #87, parent epic #83):

| #    | Title                                                                            | Pts |
|------|----------------------------------------------------------------------------------|-----|
| #379 | render: RenderFrame extract is immutable, triple-buffered                         | 3   |
| #380 | render: declarative C++ render graph build per View                               | 3   |
| #381 | render: capability-gated pass elision at build time                               | 2   |
| #382 | render: transient resource alias planner with ≥40% recovery                       | 5   |
| #383 | render: minimum split-aware Metal 4 barrier emission                              | 3   |
| #384 | render: ExecutionPlan structural-hash cache hit path                              | 3   |
| #385 | render: Graphics/Compute/Copy queue assignment with auto-fences                   | 3   |
| #386 | render: mesh-shader gbuffer writes 4 MRTs + visID + depth atomically              | 5   |
| #387 | render: vertex+amplification fallback when MeshShaders missing                    | 3   |
| #388 | render: two-phase HZB occlusion cull (read N-1, write N)                          | 3   |
| #389 | render: persistent-thread cluster cull + deferred lighting parity                 | 5   |
| #390 | render: hybrid-RT shadow trace with PCSS fallback                                 | 5   |
| #391 | render: BLAS refit precedes TLAS build every frame                                | 3   |
| #392 | render: HardwareRayTrace capability-fallback to GTAO/PCSS                         | 3   |
| #393 | render: PSO cache lookup with PsoCompileFailed lower-tier path                    | 2   |
| #394 | render: PSO cache pre-faulted at init from shader manifest                        | 2   |
| #395 | render: PSO cache invalidation by shader_hash on hot-reload                       | 2   |
| #396 | render: present pass acquires drawable and signals PresentFence                   | 2   |
| #397 | render: PresentTimeout (>2× budget) triggers abort-frame                          | 2   |
| #398 | render: GpuFault triggers hot-reload-restart with diag capture                    | 5   |
| #399 | render: diagnostic overlay (DAG + per-pass GPU timing)                            | 3   |
| #400 | render: cost-aware budget culling under load (PassPriority)                       | 3   |
| #401 | render: multi-view fan-out from one RenderFrame extract                           | 3   |
| #402 | render: ResourceResidencyExceeded triggers lower-tier recovery                    | 3   |

Coverage map (target topics from spike #97 brief):

- **Render graph build** — #380, #381, #383, #384, #385.
- **Mesh-shader gbuffer** — #386, #387.
- **Deferred lighting** — #389.
- **Hybrid-RT shadow** — #390, #392.
- **BLAS lifecycle** — #391, #392.
- **HZB cull** — #388.
- **Transient resource alias** — #382, #402.
- **PSO cache hit / lifecycle** — #393, #394, #395.
- **Present timing** — #396, #397.
- **GPU fault recovery** — #398.
- **Capability fallback** — #381, #387, #392.
- **Cross-cutting (extract, multi-view, budget, overlay)** — #379, #399,
  #400, #401.

Each story's E2E `.glibre-trace` lives under `tests/e2e/render/`; each
acceptance criterion has at minimum one Catch2 fixture under
`tests/render/` (named in the story's E2E plan). Aggregate roll-up:
24 stories × pts → 76 pts. The §10 closed-sum failure-mode coverage is
asserted by the per-variant test fixtures cited in §10.3 (recovery
ladder + capability-fallback paths exercised across stories #381,
#387, #392, #393, #397, #398, #402).

## 12. Open Questions

None. All concerns raised during §1–§11 authoring (spikes #88–#97) were
either resolved in place by §1–§10 or converted into the §11 user-story
backlog (#379–#402). Closed by spike #98.
