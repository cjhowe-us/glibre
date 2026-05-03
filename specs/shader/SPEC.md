# Shader Spec

## 1. Purpose

The `shader` context owns the offline pipeline that turns HLSL source
into platform-native shader bytecode plus the reflection metadata that
downstream contexts consume. Concretely it owns: HLSL authoring
conventions (Slang-compatible subset); the `IShaderBackend` plugin
interface (compile / reflect / link / capabilities); DXC invocation
producing DXIL and SPIR-V; metal-shaderconverter transpilation of DXIL
into `.metallib`; HLSL reflection extraction (entry points, resource
bindings, descriptor-frequency groups, vertex IO, push constants,
specialization constants, material parameter blocks); the permutation
key `(ShadingModel × Features × RenderPath × LOD)` and its enumeration
+ codegen; the on-disk shader cache keyed by source hash + permutation
+ target; root-signature / argument-buffer layout derived from
reflection. The context **refuses** to own: render-graph topology and
pass scheduling (lives in `render`); material-graph nodes and visual
authoring (lives in `material`); GPU resource allocation, command
encoding, PSO objects, and runtime binding (lives in `render`); mesh
data and meshlet layout (lives in `geometry`); and **all runtime
shader compilation** — shipping builds contain only precompiled
artifacts loaded by handle.

## 2. Ubiquitous Language

Terms used unchanged in code.

| Term | Meaning |
|------|---------|
| Shader Source | Versioned HLSL translation unit on disk. Slang-compatible subset; entry points tagged by stage attribute. |
| Shader Stage | One of `vertex`, `pixel`, `compute`, `mesh`, `amplification`, `library` (RT). Identified by HLSL entry-point attribute. |
| Shading Model | Closed enum naming a BSDF family (e.g. `Standard`, `Skin`, `Hair`, `Cloth`, `Foliage`, `Eye`, `Water`, `ClearCoat`). One axis of `Permutation Key`. |
| Feature Set | Bitfield of optional shader capabilities (e.g. `Skinned`, `MotionVectors`, `AlphaTest`, `Decal`, `VirtualTexture`, `RT`). One axis of `Permutation Key`. |
| Render Path | Closed enum: `Forward`, `Deferred`, `DepthOnly`, `Shadow`, `Velocity`, `Probe`. One axis of `Permutation Key`. |
| LOD Tier | Closed enum of shading-cost tiers (`Mobile`, `Switch`, `Desktop`, `HighEnd`) selected at device init, never branched per-frame. One axis of `Permutation Key`. |
| Permutation Key | The 4-tuple `(ShadingModel, FeatureSet, RenderPath, LODTier)`. Total enumerable at codegen time; used as primary key for compiled artifacts. |
| Permutation | A single resolved `Permutation Key` value; one compiled artifact per `(Permutation, target backend)`. |
| Shader Backend | Implementation of `IShaderBackend` plugin trait: `compile`, `reflect`, `link`, `capabilities`. One per source language family (HLSL today). |
| Compile Target | `DXIL`, `SPIRV`, or `MetalLib`. DXC emits the first two; metal-shaderconverter consumes DXIL to emit the third. |
| DXC | DirectX Shader Compiler subprocess. The sole HLSL → DXIL / SPIR-V producer; never linked in-process. |
| metal-shaderconverter | Apple CLI subprocess that lowers DXIL into Metal `.metallib`. Sole MSL producer; HLSL is never authored as MSL. |
| Reflection | Structured metadata extracted from compiled DXIL: entry points, register bindings, descriptor frequency groups, vertex input layout, push-constant ranges, sampler bindings, RT payload sizes. Source of truth for downstream binding code. |
| Descriptor Frequency Group | One of `PerFrame`, `PerPass`, `PerMaterial`, `PerDraw`. Reflection assigns each binding to exactly one group. Drives backend-native descriptor mapping in `render`. |
| Material Parameter Block | A reflected constant-buffer struct describing the per-instance parameter buffer that material instances upload; shared across all instances of a parent material. |
| Vertex IO Layout | Reflection-derived description of vertex stage input semantics; consumed by `render` to validate / build vertex layouts. |
| Root Signature | Reflection-derived descriptor-binding contract used by `render` to build backend-native pipeline layouts (D3D12 root sig, Vulkan pipeline layout, Metal argument-buffer schema). |
| Shader Hash | BLAKE3 of (preprocessed source ∪ resolved Permutation Key ∪ compile flags ∪ target). Cache key. |
| Shader Artifact | Tuple `(Compile Target bytecode, Reflection blob, Shader Hash)` produced for one `(Permutation, Compile Target)` and stored in the shader cache. |
| Shader Cache | Content-addressable on-disk store of `Shader Artifact` records keyed by `Shader Hash`. Read-only at runtime in shipping builds. |
| Shader Library | Cooked archive of all `Shader Artifact` records reachable from a project's permutation enumeration; the runtime asset that ships with the game. |
| `IShaderBackend` | Plugin trait: `compile(source, key, target) → Artifact`, `reflect(artifact) → Reflection`, `link(artifacts...) → LinkedModule`, `capabilities() → Caps`. |
| Capabilities | Static descriptor of what a backend supports (mesh shaders, RT pipelines, work graphs, wave intrinsics, 16-bit types). Queried offline; never per-frame. |
| Specialization Constant | Reflected named scalar baked at link time, not at HLSL compile time, when the backend supports it (SPIR-V); folded into permutation otherwise. |

## 3. Derived From

Harmonius is unreliable prior art — its files were mined as research input
only. Every conclusion below is independently re-derived against glibre's
philosophy (SOLID, SRP, codegen-everywhere, zero runtime reflection in
shipping). Citations name the harmonius source paragraph, not its
authority.

### Cited harmonius sources

| Harmonius source | Path | What we mined |
|------------------|------|---------------|
| Shader Variants Design | `docs/design/rendering/shader-variants.md` | 4-axis permutation key `(ShadingModel, ShaderFeatures, RenderPath, Lod)`; per-axis enumerations; bitset feature dimension; precompile + on-demand-compile split; usage-metric-driven precompile list; pak-bundle layout. |
| Shader Variants Test Cases | `docs/design/rendering/shader-variants-test-cases.md` | Concrete invariants (deterministic key hash, bit-stable ordering, dimension cardinalities, budget-fail-build behavior). |
| GPU Abstraction Layer Reqs | `docs/requirements/rendering/gpu-abstraction-layer.md` § R-2.1.16 / R-2.1.17 / R-2.1.18 | Four descriptor-frequency groups (PerFrame / PerPass / PerMaterial / PerDraw); HLSL → DXIL + SPIR-V + metallib via DXC and metal-shaderconverter as **CLI subprocesses**; **no runtime shader compilation in shipping**; structured errors at every public boundary. |
| GPU Runtime Reqs (GR) | `docs/requirements/rendering/gpu-abstraction.md` GR-2 / GR-4 | Confirmed that state-cache responsibilities (binding caches, push-constant caches) live in `render`, not `shader` — they consume reflection but don't produce it. |
| Render Pipeline Design | `docs/design/rendering/render-pipeline.md` § "Shader Compilation Pipeline" + § "Descriptor Layout Inference" | Single HLSL front-end; DXC produces DXIL + SPIR-V; metal-shaderconverter consumes DXIL to emit metallib (DXIL is the pivot, not a parallel back-end); descriptor layout / bindings inferred from compiled-bytecode reflection. |
| Pipeline State Cache Design | `docs/design/rendering/pipeline-state-cache.md` R-2.3.9.2 / R-2.3.9.8 | PSO key composes shader hash with device fingerprint (PSO cache lives in `render`); descriptor layout is inferred from DXIL/SPIR-V reflection **once** and cached — confirms reflection is a `shader`-context output, not a render-thread runtime job. |
| Advanced Materials Reqs | `docs/requirements/rendering/advanced-materials.md` R-2.12.9 | Custom material graphs **codegen HLSL** consumed by this same DXC pipeline; the `material` context emits HLSL into `shader`'s front door — confirms HLSL is the engine-wide source-of-truth shader language. |
| Rendering Core Design | `docs/design/rendering/rendering-core.md` Material System | `Material` references a `ShaderPermutationCache` keyed by `PermutationKey`; `ShadingModel` enum is the same axis used here. Confirms `render` and `material` both consume artifacts keyed by `PermutationKey`. |
| Render Pipeline Design — RF-9 | `docs/design/rendering/render-pipeline.md` § RF-9 | Hot-reload re-runs reflection on new bytecode so descriptor layout stays in sync. Adopted as the `shader`-context hot-reload contract obligation; the runtime PSO-invalidate side belongs to `render`. |

### Occam collapses (multiple harmonius concepts → one glibre primitive)

1. **Shader source language: many → one (HLSL via DXC).** Harmonius's
   advanced-materials text waves at HLSL as the lingua franca but its
   broader render docs and assorted prior tooling left ambiguous room
   for Slang front-ends, MSL hand-authored shaders, and direct GLSL
   paths. Glibre collapses to **HLSL only** at the source layer and a
   single tool pipeline:

   ```text
   HLSL ─► DXC ─► DXIL ─► metal-shaderconverter ─► metallib
                        │
                        └► SPIR-V (DXC --target spirv)
   ```

   Slang remains a *future research seed* via HLSL's Slang-compatible
   subset, but no Slang front-end is adopted today. MSL is never
   hand-authored. GLSL is rejected outright. Justification: SRP — one
   front-end, one error vocabulary, one reflection format. (PHILOSOPHY
   §1, §10.)

2. **Shader permutation schemes: many → one 4D key.** Harmonius
   gestured at per-feature `#ifdef` flags, `ShaderFeatures` bitsets,
   `ShadingModel` enums, render-path enums, and LOD tiers as
   independent permutation mechanisms scattered across the variant
   doc and the rendering-core material system. Glibre collapses them
   into the **single primitive** `Permutation Key = (ShadingModel,
   FeatureSet, RenderPath, LODTier)` defined in §2. All four axes are
   closed enums / bitsets known at codegen time; the cross-product is
   finite, enumerable, and the sole cache key for shader artifacts.
   Any axis growth requires a spec amendment, never an ad-hoc
   `#define`.

3. **Compiler invocation models: many → one (subprocess CLI).**
   Harmonius's GR-1 through GR-4 implied either in-process linkage or
   subprocess invocation depending on which paragraph one read.
   Glibre collapses to **subprocess CLI only** for both DXC and
   metal-shaderconverter — never linked in-process, never run on the
   render thread. Justification: SRP for plugin boundaries (no DXC
   ABI bleed into core) and determinism (subprocess sandbox + content
   hash on inputs gives reproducible artifacts).

4. **Reflection sources: many → one (DXIL/SPIR-V post-compile
   reflection).** Harmonius mentioned three reflection paths
   (DXC-produced DXIL metadata, SPIR-V cross-reflection, Metal IR
   inspection). Glibre collapses to **a single reflection pass over
   compiled bytecode** (DXIL preferred, SPIR-V as the secondary
   source) producing one canonical `Reflection` record per artifact.
   metallib is treated as terminal output, never re-reflected — its
   binding layout is derived from the upstream DXIL reflection that
   metal-shaderconverter preserves.

5. **Descriptor-frequency taxonomy: keep harmonius's four groups
   verbatim.** Harmonius R-2.1.16 named four frequency groups
   (PerFrame, PerPass, PerMaterial, PerDraw). Independently
   re-derived: any finer split overfits one backend; coarser loses
   per-material change-rate information. Adopted unchanged as the §2
   `Descriptor Frequency Group` enum. (Not a collapse so much as a
   non-rejection — recorded here for traceability.)

### Refusals (what `shader` does **not** own, despite harmonius prose)

1. **Render-graph topology and pass scheduling.** Harmonius's
   render-pipeline doc tangled shader compilation into the render
   graph. Glibre keeps `shader` purely offline: the render graph,
   barrier analysis, queue scheduling, and execution-plan compilation
   live entirely in `render`. The `shader` context's only handoff to
   `render` is the artifact + reflection blob.

2. **Material-graph nodes and visual authoring.** Harmonius
   `advanced-materials.md` R-2.12.9 lets material graphs codegen HLSL
   *into* this pipeline. That codegen lives in the `material` context
   (visual nodes, node-type registry, graph compilation to HLSL).
   `shader` consumes the resulting HLSL like any other shader source —
   it does not author or own material-graph node semantics.

3. **Runtime shader compilation in shipping.** Harmonius's
   shader-variants doc included an `OnDemandCompiler` runtime path.
   Glibre **rejects** this for shipping builds: shipping ships only
   precompiled artifacts loaded by handle, full stop. Editor / dev
   builds may invoke the same offline pipeline on save, but this is a
   tooling path, not a runtime path. Gating: `#if GLIBRE_SHIPPING` at
   plugin manifest level — the runtime DXC subprocess code is
   excluded from the shipping `shader` plugin entirely. (PHILOSOPHY
   §6: "zero runtime reflection in shipping builds" extended to zero
   runtime compilation.)

4. **PSO objects and runtime binding.** PSO creation, root-signature
   binding, and command encoding live in `render`. The `shader`
   context produces the **inputs** (bytecode + reflection-derived
   descriptor layouts and root-signature schemas); `render` builds
   PSOs from those inputs.

5. **GPU resource allocation, mesh / meshlet data.** Resource heaps,
   sub-allocation, and meshlet buffers belong to `render` and
   `geometry` respectively. `shader` never touches GPU memory.

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
