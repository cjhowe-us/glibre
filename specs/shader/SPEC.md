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
