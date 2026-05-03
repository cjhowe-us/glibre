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

The `shader` context is decomposed into seven aggregates. Each owns
one responsibility (SRP) and exposes value objects that downstream
consumers (`render`, `material`) can hold without reaching into the
aggregate's internals. Cross-aggregate references travel only through
the value objects listed under "Owns" / "Exposes"; no aggregate
mutates another's state.

```text
   ShaderSource ─┐
                 ▼
      PermutationKey ──► CompilationPipeline ──► ReflectionBlob
                             │            ╲             │
                             ▼             ▼            ▼
                       ShaderArtifact   ShaderCache   DescriptorLayout
                             │            (CAS)            │
                             └─────► IShaderBackend ◄──────┘
```

### 4.1 `ShaderSource` (aggregate root, entity)

**Responsibility.** Own one HLSL translation unit on disk and the set
of stage-tagged entry points it exposes. Validates that the file
parses, that every entry point carries exactly one stage attribute
(`[shader("vertex")]`, `[shader("pixel")]`, …), and that the file's
preprocessor closure is finite and reproducible. Does **not** compile;
does **not** know about permutations.

- **Identity.** Stable `SourceId` derived from the project-relative
  source path; survives moves only via explicit rename, never silently.
- **Owns.** The HLSL byte stream, the include-graph closure (paths +
  content hashes), and the entry-point manifest (name + stage).
- **Exposes.** `SourceId`, `EntryPoint { name, Stage }`,
  `PreprocessedSource` (post-include byte stream + total content hash).
- **SRP.** "Validate and present an authored HLSL translation unit."
  Anything that consumes the unit (compile, reflect, hash for cache)
  belongs to a different aggregate.

**Invariants.**

1. Every emitted `EntryPoint` has exactly one stage attribute.
2. Two `ShaderSource` instances with byte-equal preprocessed content
   and byte-equal include-graph closure produce equal `PreprocessedSource`.
3. The include-graph closure is acyclic and resolves entirely under the
   project source root; absolute or upward-escaping includes are
   rejected at construction time with `shader::Error::IncludeEscape`.

### 4.2 `PermutationKey` (value object)

**Responsibility.** Encode the 4-tuple
`(ShadingModel, FeatureSet, RenderPath, LODTier)` from §2 as a single
trivially-copyable value with a deterministic packed encoding suitable
for hashing, ordering, and codegen-table generation. Does **not** know
about HLSL, DXC, or any backend.

- **Identity.** Pure value semantics; equality is bitwise on the
  packed encoding.
- **Owns.** The 4-axis packed bits, an `is_well_formed()` predicate,
  and the canonical `to_bytes()` / `from_bytes()` round-trip used by
  both the codegen tables and the cache key.
- **Exposes.** `PermutationKey`, `PermutationIndex` (dense codegen
  ordinal across the enumerable cross-product), and the closed
  enumerator types `ShadingModel`, `FeatureSet`, `RenderPath`, `LODTier`.
- **SRP.** "Name and order one point in the permutation cross-product."
  Enumeration of which keys are *materialized* into a build belongs to
  the build-time permutation enumerator (§6); not this aggregate.

**Invariants.**

1. The packed encoding is total, injective, and bit-stable across
   hosts and runs (determinism — PHILOSOPHY §7).
2. Iteration order over the cross-product is fixed and matches the
   tuple field order `(ShadingModel, FeatureSet, RenderPath, LODTier)`,
   ascending; no implementation-defined hash ordering may leak.
3. Every closed enum has a documented cardinality; `FeatureSet` is a
   bitfield whose bit positions are fixed in the public header.
   Adding an enumerator or feature bit is a spec amendment, not an
   in-place edit.

### 4.3 `CompilationPipeline` (aggregate, service-shaped)

**Responsibility.** Drive one `(ShaderSource, PermutationKey,
CompileTarget)` through the appropriate subprocess chain
(`DXC` for `DXIL` / `SPIRV`; `metal-shaderconverter` consuming `DXIL`
for `MetalLib`) and return a sealed `ShaderArtifact`. Owns invocation,
argument construction, sandbox controls, stdout/stderr capture, and
exit-code translation into `shader::Error`. Does **not** persist
artifacts; does **not** reflect; does **not** link.

- **Identity.** Stateless; one instance per offline build.
- **Owns.** The DXC subprocess descriptor (binary path, argv template,
  per-target flags) and the `metal-shaderconverter` subprocess
  descriptor; the compile-flag canonicalizer used by the cache key.
- **Exposes.** `compile(ShaderSource, PermutationKey, CompileTarget) →
  Result<ShaderArtifact>` and the typed flag struct that feeds it.
- **SRP.** "Run the offline compiler chain, deterministically."
  Caching, reflection, and linking are explicitly elsewhere.

**Invariants.**

1. Both compilers are invoked **only** as subprocesses; in-process
   linkage of DXC or `metal-shaderconverter` is forbidden (Occam
   collapse §3.3).
2. `MetalLib` artifacts are produced by lowering the **same** `DXIL`
   bytecode that the SPIR-V/DXIL targets share; no parallel HLSL→MSL
   path exists. (Reinforces invariant 2 in §4.4.)
3. The pipeline is a no-op symbol in shipping builds: the entire
   `CompilationPipeline` translation unit is excluded from the
   shipping `shader` plugin via `#if !GLIBRE_SHIPPING` at the plugin
   manifest layer. Shipping builds NEVER invoke DXC.
4. Compile flags are canonicalized (sorted, deduplicated) before they
   feed the cache key, so semantically-equal invocations hash equally.

### 4.4 `ReflectionBlob` (value object, aggregate boundary)

**Responsibility.** Carry the structured metadata extracted from a
single compiled bytecode container — entry points, register bindings,
descriptor frequency tags, vertex IO layout, push-constant ranges,
sampler bindings, RT payload sizes, specialization constant slots.
Constructed exclusively by reflecting **DXIL**; SPIR-V is allowed as a
secondary source only when DXIL is unavailable for a given target.
Does **not** persist; does **not** know about backend descriptor
heaps.

- **Identity.** Value semantics; equality is structural over its
  fields. A `ReflectionBlob` is paired 1:1 with its source
  `ShaderArtifact`.
- **Owns.** All reflected metadata and the `Reflector` strategy
  (DXIL-first; SPIR-V fallback) that produced it.
- **Exposes.** `ReflectionBlob`, the descriptor-binding records
  tagged with `DescriptorFrequencyGroup`, the `MaterialParameterBlock`
  layout, and the `VertexIOLayout` used by `render` validation.
- **SRP.** "Translate compiled bytecode into a canonical metadata
  record." Mapping that record onto a backend-specific descriptor
  layout belongs to §4.5.

**Invariants.**

1. A `ReflectionBlob` is produced from DXIL (preferred) or from SPIR-V
   (fallback) — **never** from MSL or `metallib`. The Metal
   descriptor schema is derived **only** via the upstream DXIL
   reflection that `metal-shaderconverter` preserves; re-reflecting
   `metallib` is a spec violation. (Occam collapse §3.4.)
2. Reflecting the same bytecode container twice yields a structurally
   equal `ReflectionBlob` (deterministic; no compiler-stamp drift).
3. Every reflected resource binding is annotated with exactly one
   `DescriptorFrequencyGroup` (`PerFrame | PerPass | PerMaterial |
   PerDraw`); unassigned bindings cause construction to fail with
   `shader::Error::DescriptorFrequencyAmbiguous`.

### 4.5 `DescriptorLayout` (value object)

**Responsibility.** Project a `ReflectionBlob` onto a backend-neutral
descriptor schema partitioned into the four frequency groups
(`PerFrame`, `PerPass`, `PerMaterial`, `PerDraw`). Provides the input
that `render` consumes to build D3D12 root signatures, Vulkan pipeline
layouts, and Metal argument-buffer schemas. Does **not** allocate GPU
memory; does **not** create PSOs.

- **Identity.** Value semantics; structurally equal layouts compare
  equal regardless of construction site.
- **Owns.** The four frequency tables (each a fixed-order list of
  `BindingSlot { kind, register/space/index, array_size, stage_mask }`)
  and the static-sampler set.
- **Exposes.** `DescriptorLayout`, `BindingSlot`, `RootSignatureSchema`
  (the unified backend-neutral form `render` consumes).
- **SRP.** "Derive the descriptor contract for one
  `(Backend, PermutationKey)`." Backend-native object construction
  belongs to `render`.

**Invariants.**

1. A `DescriptorLayout` is **static per `(Backend, PermutationKey)`**:
   given the same backend and the same permutation key, the derived
   layout is structurally equal across runs and across the entire
   project's PSOs. Layouts are computed **once** offline and cached;
   re-derivation at runtime is forbidden.
2. The four frequency tables partition the binding set: every
   `BindingSlot` appears in exactly one table; the tables are
   disjoint and complete.
3. Within a frequency table, slots are ordered deterministically
   (ascending by `(register space, register index, stage mask)`); no
   set / map iteration order may leak.

### 4.6 `ShaderCache` (aggregate, repository-shaped)

**Responsibility.** Persist `ShaderArtifact` records (bytecode +
`ReflectionBlob` + `DescriptorLayout`) in a content-addressable on-disk
store keyed by `ShaderHash` from §2. Provides idempotent put/get and
the cooked `ShaderLibrary` archive layout. Does **not** compile;
does **not** reflect.

- **Identity.** One cache instance per project; the cache root path
  is its identity.
- **Owns.** The CAS directory layout, the `ShaderHash` → blob index,
  the `ShaderLibrary` archive packer, and the integrity verifier.
- **Exposes.** `ShaderCache::lookup(ShaderHash) → optional<ShaderArtifact>`,
  `ShaderCache::insert(ShaderArtifact) → Result<void>`, and
  `ShaderLibrary` (cooked, read-only handle for shipping).
- **SRP.** "Store and retrieve content-addressed shader artifacts."
  Decisions about which `(Permutation, Target)` records to populate
  come from the permutation enumerator; not this aggregate.

**Invariants.**

1. The cache is **content-addressable**: `ShaderHash` is the BLAKE3
   of `(preprocessed source ∪ resolved PermutationKey ∪ canonical
   compile flags ∪ CompileTarget)` (§2). `insert` of an existing key
   is a no-op; values are immutable once stored.
2. `lookup` is the **sole** read path in shipping builds; the
   shipping `ShaderLibrary` is opened read-only, and every PSO load
   resolves through `ShaderHash` — no fallback to compilation.
3. A cooked `ShaderLibrary` archive contains exactly the artifacts
   reachable from the project's enumerated `PermutationKey` set; orphan
   blobs and missing references both fail the cooker
   (`shader::Error::CacheIntegrity`).

### 4.7 `IShaderBackend` (plugin trait, aggregate boundary)

**Responsibility.** The narrow seam through which the rest of the
engine talks to a shader source-language family. Today the only
implementation is the HLSL backend wrapping DXC + `metal-shaderconverter`.
Defines four operations and nothing else.

- **Identity.** One implementing plugin per source-language family;
  selected at offline-build configuration time.
- **Owns.** The trait declaration; concrete backends live in plugin
  dylibs.
- **Exposes.** Four operations:

  | Op             | Signature                                                                   |
  |----------------|-----------------------------------------------------------------------------|
  | `compile`      | `(ShaderSource, PermutationKey, CompileTarget) → Result<ShaderArtifact>`    |
  | `reflect`      | `(ShaderArtifact) → Result<ReflectionBlob>`                                 |
  | `link`         | `(span<ShaderArtifact>) → Result<LinkedModule>` (SPIR-V spec-const baking)  |
  | `capabilities` | `() → Capabilities` (mesh shaders, RT, work graphs, wave intrinsics, fp16) |

- **SRP.** "Define the contract a source-language backend must
  satisfy." Implementation, caching, and runtime use are not part of
  the trait.

**Invariants.**

1. `capabilities()` is a **pure** offline query; no implementation
   may evaluate it per-frame or hold mutable state across calls.
2. Backends compose via the four-operation surface only; engine code
   never reaches into a backend's internals (no friend access, no
   sibling-context downcasts).
3. `compile` and `reflect` are **deterministic functions** of their
   inputs in the engine's value-equality sense: byte-equal inputs
   produce structurally-equal outputs, modulo subprocess
   non-determinism that the canonicalization layer (§4.3) elides.

### 4.8 Cross-aggregate invariants

These four invariants are universal contracts that bind multiple
aggregates and are exposed at the public API boundary of the `shader`
context:

1. **One PSO ↔ one PermutationKey.** Every PSO that `render`
   constructs from a `shader`-context artifact maps to **exactly one**
   `PermutationKey`. There is no n:1, no 1:n, and no fallback
   permutation. (Enforced by `ShaderArtifact` carrying its
   `PermutationKey` as part of its identity.)
2. **DXIL is the reflection pivot.** A `ReflectionBlob` for a
   `metallib` artifact is **always** derived via DXIL during the
   offline lowering; `metallib` is never re-reflected from MSL.
   (§4.4 invariant 1; §3 collapse 4.)
3. **Shipping never compiles.** `CompilationPipeline` is excluded
   from shipping builds; the only runtime read path is
   `ShaderCache::lookup` against a cooked `ShaderLibrary`.
   (§4.3 invariant 3; §4.6 invariant 2; §3 refusal 3.)
4. **Static descriptor layouts.** `DescriptorLayout` is determined
   once per `(Backend, PermutationKey)` offline; runtime descriptor
   selection is a table lookup, never a re-derivation.
   (§4.5 invariant 1.)

## 5. Public Interface

The header below is the compileable stub of the `shader` context's public
boundary. It compiles cleanly with `clang++ -std=c++23 -fsyntax-only`,
both with `-DGLIBRE_SHIPPING=0` (default; tooling builds) and
`-DGLIBRE_SHIPPING=1` (the entire `compile()` virtual is `#if`-guarded
out so shipping plugins never link DXC subprocess code — §4.3 invariant
3, §4.8 cross-aggregate invariant 3).

The error type follows `reviews/decisions/error-model.md`: a closed
`enum class shader::Error : std::uint16_t` returned through
`std::expected<T, shader::Error>` at every boundary. The engine-wide
`glibre::Error` variant alias will append this enum when the shader
plugin lands; that registration is a `core` change, not a `shader`
change.

```cpp
// shader/include/glibre/shader/shader.hpp
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef GLIBRE_SHIPPING
#define GLIBRE_SHIPPING 0
#endif

namespace glibre::shader {

// -------- Error closed sum (enumerator list pinned by SPEC §10) ----------

enum class Error : std::uint16_t {
    SourceNotFound,
    SourceParseFailed,
    IncludeEscape,
    IncludeCycle,
    EntryPointMissing,
    EntryPointStageAmbiguous,
    PermutationKeyMalformed,
    PermutationKeyOutOfRange,
    CompilerInvocationFailed,
    CompilerExitNonZero,
    CompilerTimedOut,
    UnsupportedTarget,
    DxilEmissionFailed,
    SpirvEmissionFailed,
    MetalLibLoweringFailed,
    ReflectionExtractionFailed,
    DescriptorFrequencyAmbiguous,
    DescriptorFrequencyMissing,
    LinkFailed,
    SpecializationConstantMissing,
    CacheLookupMiss,
    CacheCorrupt,
    CacheIntegrity,
    CacheReadOnlyViolation,
    CapabilityNotSupported,
    ShippingCompilationAttempted,
};

// -------- Closed enums for the 4-axis permutation key (§4.2) -------------

enum class ShadingModel : std::uint8_t {
    Standard, Skin, Hair, Cloth, Foliage, Eye, Water, ClearCoat,
};
inline constexpr std::size_t kShadingModelCount = 8;

enum class FeatureBit : std::uint8_t {
    Skinned        = 0,
    MotionVectors  = 1,
    AlphaTest      = 2,
    Decal          = 3,
    VirtualTexture = 4,
    RT             = 5,
};
inline constexpr std::size_t kFeatureBitCount = 6;

class FeatureSet {
public:
    constexpr FeatureSet() noexcept = default;
    constexpr explicit FeatureSet(std::uint16_t bits) noexcept : bits_{bits} {}

    constexpr bool test(FeatureBit b) const noexcept {
        return (bits_ & (std::uint16_t{1} << static_cast<std::uint8_t>(b))) != 0;
    }
    constexpr void set(FeatureBit b) noexcept {
        bits_ |= static_cast<std::uint16_t>(std::uint16_t{1} << static_cast<std::uint8_t>(b));
    }
    constexpr std::uint16_t bits() const noexcept { return bits_; }

    friend constexpr bool operator==(FeatureSet, FeatureSet) noexcept = default;

private:
    std::uint16_t bits_{0};
};

enum class RenderPath : std::uint8_t {
    Forward, Deferred, DepthOnly, Shadow, Velocity, Probe,
};
inline constexpr std::size_t kRenderPathCount = 6;

enum class LODTier : std::uint8_t { Mobile, Switch, Desktop, HighEnd };
inline constexpr std::size_t kLODTierCount = 4;

// -------- PermutationKey (value object, §4.2) ----------------------------

struct PermutationKey {
    ShadingModel shading_model{ShadingModel::Standard};
    FeatureSet   features{};
    RenderPath   render_path{RenderPath::Forward};
    LODTier      lod_tier{LODTier::Desktop};

    friend constexpr bool operator==(PermutationKey, PermutationKey) noexcept = default;

    // Total, injective, bit-stable encoding (§4.2 invariant 1).
    using PackedBytes = std::array<std::byte, 6>;
    PackedBytes to_bytes() const noexcept;
    static std::expected<PermutationKey, Error> from_bytes(const PackedBytes&) noexcept;

    bool is_well_formed() const noexcept;
};

// Dense codegen ordinal across the enumerable cross-product (§4.2).
struct PermutationIndex {
    std::uint32_t value{0};
    friend constexpr bool operator==(PermutationIndex, PermutationIndex) noexcept = default;
};

// -------- Shader stages, targets, content hash ---------------------------

enum class Stage : std::uint8_t {
    Vertex, Pixel, Compute, Mesh, Amplification, Library,
};

enum class CompileTarget : std::uint8_t { DXIL, SPIRV, MetalLib };

// BLAKE3 of (preprocessed source ∪ resolved key ∪ canonical flags ∪ target).
struct ShaderHash {
    std::array<std::byte, 32> bytes{};
    friend constexpr bool operator==(ShaderHash, ShaderHash) noexcept = default;
};

// -------- ShaderSource (aggregate root, §4.1) ----------------------------

struct SourceId {
    std::string project_relative_path;
    friend bool operator==(const SourceId&, const SourceId&) noexcept = default;
};

struct EntryPoint {
    std::string name;
    Stage       stage{Stage::Vertex};
    friend bool operator==(const EntryPoint&, const EntryPoint&) noexcept = default;
};

struct IncludeNode {
    std::string project_relative_path;
    ShaderHash  content_hash{};
};

struct PreprocessedSource {
    std::vector<std::byte>   bytes;            // post-include byte stream
    std::vector<IncludeNode> include_closure;  // ordered, acyclic, project-rooted
    ShaderHash               total_hash{};
};

class ShaderSource {
public:
    static std::expected<ShaderSource, Error>
    open(const std::filesystem::path& project_root,
         const std::filesystem::path& project_relative);

    const SourceId&             id() const noexcept;
    std::span<const EntryPoint> entry_points() const noexcept;
    const PreprocessedSource&   preprocessed() const noexcept;

private:
    ShaderSource() = default;

    SourceId                id_{};
    std::vector<EntryPoint> entry_points_{};
    PreprocessedSource      preprocessed_{};
};

// -------- ReflectionBlob (value object, §4.4) ----------------------------

enum class DescriptorFrequencyGroup : std::uint8_t {
    PerFrame, PerPass, PerMaterial, PerDraw,
};

enum class BindingKind : std::uint8_t {
    ConstantBuffer,
    SampledImage,
    StorageImage,
    Sampler,
    StructuredBuffer,
    RWStructuredBuffer,
    AccelerationStructure,
    PushConstant,
};

struct StageMask {
    std::uint8_t bits{0};
    friend constexpr bool operator==(StageMask, StageMask) noexcept = default;
};

struct BindingSlot {
    BindingKind              kind{BindingKind::ConstantBuffer};
    std::uint32_t            register_space{0};
    std::uint32_t            register_index{0};
    std::uint32_t            array_size{1};
    StageMask                stages{};
    DescriptorFrequencyGroup frequency{DescriptorFrequencyGroup::PerDraw};
    std::string              name;

    friend bool operator==(const BindingSlot&, const BindingSlot&) noexcept = default;
};

struct VertexInputElement {
    std::string   semantic;
    std::uint32_t semantic_index{0};
    std::uint32_t location{0};
    std::uint32_t format_code{0};   // backend-neutral format ordinal
};

struct VertexIOLayout {
    std::vector<VertexInputElement> elements;
};

struct PushConstantRange {
    std::uint32_t offset{0};
    std::uint32_t size{0};
    StageMask     stages{};
    friend constexpr bool operator==(PushConstantRange, PushConstantRange) noexcept = default;
};

struct MaterialParameterBlock {
    std::string              name;
    std::uint32_t            size_bytes{0};
    std::vector<BindingSlot> members;  // members reflected as named scalar/vector slots
};

struct SpecializationConstantSlot {
    std::string   name;
    std::uint32_t id{0};
    std::uint32_t size_bytes{0};
};

struct ReflectionBlob {
    std::vector<EntryPoint>                 entry_points;
    std::vector<BindingSlot>                bindings;        // one frequency tag each
    VertexIOLayout                          vertex_io;
    std::vector<PushConstantRange>          push_constants;
    MaterialParameterBlock                  material_parameters;
    std::vector<SpecializationConstantSlot> spec_constants;
    std::uint32_t                           rt_payload_bytes{0};
};

// -------- DescriptorLayout (value object, §4.5) --------------------------

struct DescriptorTable {
    // Ordered by (register_space, register_index, stage_mask) — §4.5 inv 3.
    std::vector<BindingSlot> slots;

    friend bool operator==(const DescriptorTable&, const DescriptorTable&) noexcept = default;
};

struct StaticSampler {
    std::uint32_t register_space{0};
    std::uint32_t register_index{0};
    std::uint32_t descriptor_code{0};   // backend-neutral sampler descriptor ordinal
    StageMask     stages{};
    friend constexpr bool operator==(StaticSampler, StaticSampler) noexcept = default;
};

struct RootSignatureSchema {
    DescriptorTable                per_frame;
    DescriptorTable                per_pass;
    DescriptorTable                per_material;
    DescriptorTable                per_draw;
    std::vector<PushConstantRange> push_constants;
    std::vector<StaticSampler>     static_samplers;

    friend bool operator==(const RootSignatureSchema&, const RootSignatureSchema&) noexcept = default;
};

class DescriptorLayout {
public:
    static std::expected<DescriptorLayout, Error>
    derive(const ReflectionBlob&);

    const DescriptorTable&     table(DescriptorFrequencyGroup) const noexcept;
    const RootSignatureSchema& schema() const noexcept;

    friend bool operator==(const DescriptorLayout&, const DescriptorLayout&) noexcept = default;

private:
    DescriptorLayout() = default;
    RootSignatureSchema schema_{};
};

// -------- Shader artifact + linked module (§4.3 outputs, §4.7 surface) ---

struct ShaderArtifact {
    PermutationKey         key{};
    CompileTarget          target{CompileTarget::DXIL};
    ShaderHash             hash{};
    std::vector<std::byte> bytecode;
    ReflectionBlob         reflection;
    DescriptorLayout       descriptor_layout;
};

struct LinkedModule {
    std::vector<std::byte> bytecode;
    ReflectionBlob         reflection;
};

// -------- ShaderCache: CAS, lookup-only in shipping (§4.6) ---------------

class ShaderCache {
public:
    static std::expected<ShaderCache, Error>
    open(const std::filesystem::path& cache_root, bool read_only);

    // Non-owning pointer into the cache; nullptr on miss. Sole runtime
    // read path in shipping (§4.6 invariant 2, §4.8 invariant 3).
    const ShaderArtifact* get(const ShaderHash&) const noexcept;

    // Idempotent: insert of an existing hash is a no-op (§4.6 invariant 1).
    std::expected<void, Error> insert(ShaderArtifact);

    // Cooked, read-only shipping handle.
    class Library;
    std::expected<Library, Error>
    cook(std::span<const PermutationKey> enumerated) const;

private:
    ShaderCache() = default;
    std::filesystem::path root_{};
    bool                  read_only_{true};
};

class ShaderCache::Library {
public:
    const ShaderArtifact* get(const ShaderHash&) const noexcept;

private:
    Library() = default;
    friend class ShaderCache;
};

// -------- Capabilities (offline query, §4.7) -----------------------------

struct Capabilities {
    bool mesh_shaders{false};
    bool ray_tracing{false};
    bool work_graphs{false};
    bool wave_intrinsics{false};
    bool fp16{false};
};

// -------- IShaderBackend trait (§4.7) ------------------------------------
//
// The four-operation surface through which the engine talks to a shader
// source-language family. `compile()` is excluded from shipping builds:
// the shipping `shader` plugin never invokes DXC or metal-shaderconverter
// (§4.3 invariant 3, §4.8 invariant 3).

class IShaderBackend {
public:
    virtual ~IShaderBackend() = default;

#if !GLIBRE_SHIPPING
    // DXC + metal-shaderconverter subprocess driver. Excluded from shipping.
    virtual std::expected<ShaderArtifact, Error>
    compile(const ShaderSource&, const PermutationKey&, CompileTarget) = 0;
#endif

    virtual std::expected<ReflectionBlob, Error>
    reflect(const ShaderArtifact&) = 0;

    virtual std::expected<LinkedModule, Error>
    link(std::span<const ShaderArtifact>) = 0;

    virtual Capabilities capabilities() const noexcept = 0;
};

// -------- Stable string mapping for structured logs (error-model.md) ----

constexpr std::string_view to_string(Error e) noexcept {
    switch (e) {
        case Error::SourceNotFound:                return "SourceNotFound";
        case Error::SourceParseFailed:             return "SourceParseFailed";
        case Error::IncludeEscape:                 return "IncludeEscape";
        case Error::IncludeCycle:                  return "IncludeCycle";
        case Error::EntryPointMissing:             return "EntryPointMissing";
        case Error::EntryPointStageAmbiguous:      return "EntryPointStageAmbiguous";
        case Error::PermutationKeyMalformed:       return "PermutationKeyMalformed";
        case Error::PermutationKeyOutOfRange:      return "PermutationKeyOutOfRange";
        case Error::CompilerInvocationFailed:      return "CompilerInvocationFailed";
        case Error::CompilerExitNonZero:           return "CompilerExitNonZero";
        case Error::CompilerTimedOut:              return "CompilerTimedOut";
        case Error::UnsupportedTarget:             return "UnsupportedTarget";
        case Error::DxilEmissionFailed:            return "DxilEmissionFailed";
        case Error::SpirvEmissionFailed:           return "SpirvEmissionFailed";
        case Error::MetalLibLoweringFailed:        return "MetalLibLoweringFailed";
        case Error::ReflectionExtractionFailed:    return "ReflectionExtractionFailed";
        case Error::DescriptorFrequencyAmbiguous:  return "DescriptorFrequencyAmbiguous";
        case Error::DescriptorFrequencyMissing:    return "DescriptorFrequencyMissing";
        case Error::LinkFailed:                    return "LinkFailed";
        case Error::SpecializationConstantMissing: return "SpecializationConstantMissing";
        case Error::CacheLookupMiss:               return "CacheLookupMiss";
        case Error::CacheCorrupt:                  return "CacheCorrupt";
        case Error::CacheIntegrity:                return "CacheIntegrity";
        case Error::CacheReadOnlyViolation:        return "CacheReadOnlyViolation";
        case Error::CapabilityNotSupported:        return "CapabilityNotSupported";
        case Error::ShippingCompilationAttempted:  return "ShippingCompilationAttempted";
    }
    return "Unknown";
}

}  // namespace glibre::shader
```

**Boundary types at a glance.**

| Type / function                     | Aggregate | Role at the boundary                       |
|-------------------------------------|-----------|--------------------------------------------|
| `ShaderSource` + `EntryPoint` + `PreprocessedSource` | §4.1 | HLSL translation unit + stage manifest. |
| `PermutationKey` + `PermutationIndex` + 4 axis enums | §4.2 | The closed 4-tuple cache / codegen key.  |
| `ShaderArtifact` + `ShaderHash` + `CompileTarget`    | §4.3 | Sealed compile output, content-addressed. |
| `ReflectionBlob` + `DescriptorFrequencyGroup` + `BindingSlot` + `VertexIOLayout` + `PushConstantRange` + `MaterialParameterBlock` + `SpecializationConstantSlot` | §4.4 | Canonical bytecode metadata. |
| `DescriptorLayout` + `DescriptorTable` + `RootSignatureSchema` + `StaticSampler` | §4.5 | Backend-neutral descriptor schema, 4 frequency groups. |
| `ShaderCache` + `ShaderCache::Library`               | §4.6 | CAS-keyed artifact store; lookup-only in shipping. |
| `IShaderBackend` + `Capabilities` + `LinkedModule`   | §4.7 | Plugin trait; `compile()` `#if`-guarded out of shipping. |
| `Error` + `to_string(Error)`                         | §10  | Closed enum returned via `std::expected`.  |

**Events.** None at this layer. `shader` is an offline producer; runtime
events (e.g. `ShaderLibraryLoaded`) belong to `render` when it consumes
the cooked library.

**Fory schemas.** Defined in §7. The on-disk records that mirror the
public types above (`ShaderArtifact`, `ReflectionBlob`,
`DescriptorLayout`, `PermutationKey`) get versioned Fory schemas; the
in-memory C++ types declared here remain the canonical source of
truth that those schemas serialize to.

**Compileability.** The block above is verified with
`clang++ -std=c++23 -fsyntax-only` under both `-DGLIBRE_SHIPPING=0` and
`-DGLIBRE_SHIPPING=1`. The shipping configuration drops the
`compile()` virtual from the trait, satisfying the §4.3 / §4.8 invariant
that shipping plugins never link DXC subprocess code.

## 6. Internal Architecture

Non-binding sketch for implementers.

## 7. Persistence & Schemas

The `shader` context persists three on-disk record types via Apache Fory
schemas under the `data` middleman dylib (`reviews/decisions/fory-codegen.md`).
Schemas live at `data/schemas/shader/<Type>.fory`; codegen emits
`glibre::types::shader::*` POD-like structs that the in-memory C++ types
in §5 serialize into. **HLSL source is never persisted** — it stays
versioned in the project repo. The cache is content-addressable; the
manifest is a single sidecar pointing into it; reflection rides inside
each artifact record.

### 7.1 Persistent record set

| Record | Schema path | Role |
|--------|-------------|------|
| `ShaderArtifactRecord` | `data/schemas/shader/ShaderArtifactRecord.fory` | One CAS blob per `(PermutationKey, source-hash, CompileTarget)`: bytecode bytes + embedded `ReflectionRecord` + derived `DescriptorLayoutRecord` + identity envelope. |
| `ShaderCacheManifest` | `data/schemas/shader/ShaderCacheManifest.fory` | Sidecar enumerating every `ShaderArtifactRecord` reachable from a project's resolved permutation set. Used to cook the shipping `ShaderLibrary` (§4.6). |
| `ReflectionRecord` | `data/schemas/shader/ReflectionRecord.fory` | Standalone Fory schema for the `ReflectionBlob` value object (§4.4). Embedded by value inside `ShaderArtifactRecord`; also serializable on its own for editor / IPC consumers. |

Three supporting value-schemas live alongside the records and are
embedded by tag inclusion (no separate files on disk; they are reused
across the records above):

| Sub-schema | Embedded in | Role |
|------------|-------------|------|
| `PermutationKeyRecord` | `ShaderArtifactRecord`, `ShaderCacheManifest` | 4-axis packed key (§4.2). Bit-stable `to_bytes()` / `from_bytes()` round-trip. |
| `DescriptorLayoutRecord` | `ShaderArtifactRecord` | Backend-neutral 4-frequency-group descriptor schema (§4.5). |
| `BindingSlotRecord`, `VertexIOLayoutRecord`, `PushConstantRangeRecord`, `MaterialParameterBlockRecord`, `SpecializationConstantSlotRecord`, `StaticSamplerRecord`, `EntryPointRecord` | `ReflectionRecord`, `DescriptorLayoutRecord` | Element-of-vector value records mirroring §5 boundary structs. |

### 7.2 Schema sketches

The format follows the canonical sketch in
`reviews/decisions/fory-codegen.md`. Tags are immutable once shipped;
removal of a field marks the tag `reserved`. Built-in scalar names
(`u8`, `u16`, `u32`, `bytes`, `string`, `list<T>`, `option<T>`) compile
to the audited `glibre/types/_builtins.hpp` set.

`data/schemas/shader/ShaderArtifactRecord.fory`:

```fory
schema glibre.shader.ShaderArtifactRecord {
  version  1
  since    "0.1.0"

  field key             : glibre.shader.PermutationKeyRecord  tag 1 since 1
  field target          : u8                                  tag 2 since 1   // CompileTarget enum ordinal
  field source_hash     : bytes32                             tag 3 since 1   // BLAKE3 of preprocessed source
  field flags_hash      : bytes32                             tag 4 since 1   // BLAKE3 of canonical compile-flag list
  field artifact_hash   : bytes32                             tag 5 since 1   // ShaderHash from §2 (CAS key)
  field bytecode        : bytes                               tag 6 since 1   // DXIL / SPIR-V / metallib bytes
  field reflection      : glibre.shader.ReflectionRecord      tag 7 since 1
  field descriptors     : glibre.shader.DescriptorLayoutRecord tag 8 since 1
  field producer_label  : string                              tag 9 since 1   // backend identity (e.g. "hlsl-dxc-1.7")
}
```

`data/schemas/shader/ShaderCacheManifest.fory`:

```fory
schema glibre.shader.ShaderCacheManifest {
  version  1
  since    "0.1.0"

  field cache_root_relative : string                                   tag 1 since 1
  field schema_abi_hash     : bytes32                                  tag 2 since 1   // glibre_types_abi_hash() at cook time
  field entries             : list<glibre.shader.ShaderManifestEntry>  tag 3 since 1   // sorted ascending by artifact_hash
  field enumerated_keys     : list<glibre.shader.PermutationKeyRecord> tag 4 since 1   // sorted ascending by packed key bytes
}

schema glibre.shader.ShaderManifestEntry {
  version  1
  since    "0.1.0"

  field artifact_hash : bytes32                              tag 1 since 1   // CAS lookup key
  field key           : glibre.shader.PermutationKeyRecord   tag 2 since 1
  field target        : u8                                   tag 3 since 1
  field byte_size     : u32                                  tag 4 since 1
}
```

`data/schemas/shader/ReflectionRecord.fory`:

```fory
schema glibre.shader.ReflectionRecord {
  version  1
  since    "0.1.0"

  field entry_points         : list<glibre.shader.EntryPointRecord>          tag 1 since 1
  field bindings             : list<glibre.shader.BindingSlotRecord>         tag 2 since 1   // every slot frequency-tagged
  field vertex_io            : glibre.shader.VertexIOLayoutRecord            tag 3 since 1
  field push_constants       : list<glibre.shader.PushConstantRangeRecord>   tag 4 since 1
  field material_parameters  : glibre.shader.MaterialParameterBlockRecord    tag 5 since 1
  field spec_constants       : list<glibre.shader.SpecializationConstantSlotRecord> tag 6 since 1
  field rt_payload_bytes     : u32                                           tag 7 since 1
}
```

### 7.3 Determinism + serialization rules

1. **Cache key = artifact identity.** `artifact_hash` is the BLAKE3 of
   `(source_hash ∪ key.to_bytes() ∪ flags_hash ∪ target_byte)` (§2,
   §4.6 invariant 1). The manifest stores it; the CAS file path is
   derived from it. Insert is idempotent.
2. **Sort orders are spec-frozen.** `ShaderCacheManifest.entries` is
   sorted ascending by `artifact_hash`; `enumerated_keys` ascending by
   the bit-stable packed bytes of `PermutationKeyRecord` (§4.2 invariant
   2). `ReflectionRecord.bindings` is sorted by
   `(register_space, register_index, stage_mask)` (§4.5 invariant 3).
   `DescriptorLayoutRecord` tables likewise. No iterator-order from a
   hash container may leak into any field.
3. **HLSL source never enters a record.** Only the BLAKE3 `source_hash`
   appears. The `data/schemas/shader/` files are the only artifacts
   that ship; the HLSL itself is repo-versioned (§3 collapse 1, §4.6
   invariant 2).
4. **Manifest stamps the middleman ABI.** `schema_abi_hash` is the
   value `glibre_types_abi_hash()` returns at cook time. The shader
   loader refuses a manifest whose hash differs from the host's
   (`Error::CacheIntegrity`); together with the dynamic linker's SONAME
   check this catches both schema-additive and schema-breaking drift.
5. **`metallib` records reuse the upstream DXIL reflection.** A
   `ShaderArtifactRecord` whose `target == MetalLib` carries the
   `ReflectionRecord` derived from the DXIL the lowering consumed
   (§4.4 invariant 1, §4.8 invariant 2). Never re-reflected from MSL.
6. **Round-trip equality.** For every record type, Fory deserialize ∘
   serialize is the identity on the in-memory §5 type (golden tests at
   the plan level).

### 7.4 Migration rules

Migration follows the engine-wide pattern in `reviews/decisions/fory-codegen.md`:
each record carries `current_version`; vN→vN+1 transforms are pure
functions provided by the `shader` context and registered through the
codegen-emitted dispatcher. The shader-specific rules:

1. **`PermutationKeyRecord` axis growth is additive.** Adding a new
   `FeatureBit`, `ShadingModel`, `RenderPath`, or `LODTier` enumerator
   bumps the axis cardinality but not the schema's tag layout — the
   packed encoding (§4.2 invariant 1) reserves headroom inside its
   existing field widths. Older payloads decode unchanged: missing
   feature bits are zero, older enumerators retain their ordinal.
   Hosts encountering an unknown enumerator while loading a newer
   manifest fail with `Error::PermutationKeyOutOfRange` rather than
   silently widening (refusal-driven hot reload, PHILOSOPHY §8).
2. **`ReflectionRecord` extensions are additive.** New reflected
   metadata (e.g. work-graph node descriptors, future RT payload
   layouts) must be appended at a fresh tag with `since N+1` and a
   deterministic synthesised default for older payloads. Removing or
   re-typing a field requires a major version bump and a
   `migrate_ReflectionRecord_vN_to_vNplus1` provider in
   `glibre::shader::migrate`.
3. **`DescriptorLayoutRecord` is regenerated, not migrated.** A
   layout-record schema bump always re-derives layouts from the
   surviving `ReflectionRecord` rather than transforming old layouts
   in-place. The reflection record is the source of truth (§4.5
   invariant 1); a stale layout encountered at load time is rebuilt
   by calling `DescriptorLayout::derive(reflection)` and the result
   replaces the on-disk record on the next cook.
4. **`ShaderCacheManifest` is rebuildable, not migrated.** The
   manifest is a derived index over the CAS — a version skew bumps the
   cooker, which re-walks the resolved permutation set and writes a
   fresh manifest. No vN→vN+1 transform is provided; instead the
   cooker runs against the new schema and emits a new manifest.
   Stale manifests are detected by `schema_abi_hash` mismatch and
   rejected with `Error::CacheIntegrity`.
5. **`ShaderArtifactRecord` envelope migrations are full-record.** A
   bump in this schema's version (e.g. adding a new identity field to
   the envelope) provides a `migrate_ShaderArtifactRecord_vN_to_vNplus1`
   transform that operates on the envelope only; bytecode bytes are
   never decoded or rewritten by a migration. If a migration would
   require touching `bytecode`, the design choice is to recompile from
   source (re-running `CompilationPipeline` against the recorded
   `source_hash`) rather than transform bytes.
6. **Hot-reload integration.** At the frame-boundary swap (PHILOSOPHY
   §8) the `shader` plugin loader resolves each persistent
   `ShaderArtifactRecord` through the migration chain into the new
   middleman ABI; failures surface as `Error::SchemaMigrationFailure`
   propagated up through `Error::CacheIntegrity`, and the swap is
   refused (no half-migrated cache state).
7. **Migration testing.** Each shipped `vN_to_vNplus1` transform owns a
   Catch2 golden under `tests/shader/persistence/` that loads a frozen
   `vN` payload, runs the migration, and asserts byte-equal
   re-serialization at `vN+1`. Goldens are committed alongside the
   migration provider.

### 7.5 Cooked `ShaderLibrary` layout

The shipping `ShaderLibrary` (§4.6, §5) is the on-disk projection of a
`ShaderCacheManifest` plus the CAS blobs it references:

```text
<library_root>/
    manifest.fory                 # ShaderCacheManifest, single file
    artifacts/<aa>/<bb>/<hash>    # ShaderArtifactRecord blobs, CAS
                                  #   <aa><bb> = first 4 hex chars of artifact_hash
                                  #   <hash>   = full hex artifact_hash
```

Read-only at runtime in shipping (§4.8 invariant 3); the cooker is the
sole writer, and it runs strictly offline through
`ShaderCache::cook(span<const PermutationKey>)`. Orphan blobs (CAS file
not referenced by manifest) and dangling references (manifest entry
without CAS file) both fail integrity verification with
`Error::CacheIntegrity` (§4.6 invariant 3).

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
