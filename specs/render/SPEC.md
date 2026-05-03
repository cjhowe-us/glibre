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
