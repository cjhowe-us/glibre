# Shader Spec

## 1. Purpose

The `shader` context owns the offline pipeline that turns Slang source
into platform-native shader bytecode plus the reflection metadata that
downstream contexts consume. The MVP pipeline is **Slang source →
`slangc` subprocess → `metallib`** for Apple Silicon / Metal 4; D3D12 /
Windows support is added post-MVP by invoking the same `slangc` driver
with `--target=dxil`. Concretely it owns: Slang authoring
conventions; the `IShaderBackend` plugin
interface (compile / reflect / link / capabilities); the single
`slangc` subprocess invocation that emits the platform bytecode
directly from Slang source (no transpile chain, no intermediate pivot);
Slang reflection extraction via slangc's native reflection API
(entry points, resource bindings, descriptor-frequency groups, vertex
IO, push constants, specialization constants, material parameter
blocks); the permutation
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
| Shader Source | Versioned Slang translation unit on disk. Slang-compatible subset; entry points tagged by stage attribute. |
| Shader Stage | One of `vertex`, `pixel`, `compute`, `mesh`, `amplification`, `library` (RT). Identified by Slang entry-point attribute. |
| Shading Model | Closed enum naming a BSDF family (e.g. `Standard`, `Skin`, `Hair`, `Cloth`, `Foliage`, `Eye`, `Water`, `ClearCoat`). One axis of `Permutation Key`. |
| Feature Set | Bitfield of optional shader capabilities (e.g. `Skinned`, `MotionVectors`, `AlphaTest`, `Decal`, `VirtualTexture`, `RT`). One axis of `Permutation Key`. |
| Render Path | Closed enum: `Forward`, `Deferred`, `DepthOnly`, `Shadow`, `Velocity`, `Probe`. One axis of `Permutation Key`. |
| LOD Tier | Closed enum of shading-cost tiers (`Mobile`, `Switch`, `Desktop`, `HighEnd`) selected at device init, never branched per-frame. One axis of `Permutation Key`. |
| Permutation Key | The 4-tuple `(ShadingModel, FeatureSet, RenderPath, LODTier)`. Total enumerable at codegen time; used as primary key for compiled artifacts. |
| Permutation | A single resolved `Permutation Key` value; one compiled artifact per `(Permutation, target backend)`. |
| Shader Backend | Implementation of `IShaderBackend` plugin trait: `compile`, `reflect`, `link`, `capabilities`. One per source language family (Slang today). |
| Compile Target | `MetalLib` (MVP — Apple Silicon / Metal 4) or `DXIL` (post-MVP — D3D12 / Windows). slangc emits the requested target directly from Slang source. |
| slangc | Apple/Khronos Slang shader compiler subprocess. Emits `metallib` (MVP) and `DXIL` (post-MVP) directly from Slang source. Sole bytecode producer; never linked in-process. |
| Reflection | Structured metadata produced by slangc's native reflection API, paired with the emitted bytecode in the same subprocess invocation: entry points, register bindings, descriptor frequency groups, vertex input layout, push-constant ranges, sampler bindings, RT payload sizes. Source of truth for downstream binding code. |
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
| Specialization Constant | Reflected named scalar baked at link time when slangc supports it for the target; folded into permutation otherwise. |

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
| GPU Abstraction Layer Reqs | `docs/requirements/rendering/gpu-abstraction-layer.md` § R-2.1.16 / R-2.1.17 / R-2.1.18 | Four descriptor-frequency groups (PerFrame / PerPass / PerMaterial / PerDraw); Slang → metallib via slangc as **CLI subprocess**; **no runtime shader compilation in shipping**; structured errors at every public boundary. |
| GPU Runtime Reqs (GR) | `docs/requirements/rendering/gpu-abstraction.md` GR-2 / GR-4 | Confirmed that state-cache responsibilities (binding caches, push-constant caches) live in `render`, not `shader` — they consume reflection but don't produce it. |
| Render Pipeline Design | `docs/design/rendering/render-pipeline.md` § "Shader Compilation Pipeline" + § "Descriptor Layout Inference" | Single Slang front-end; slangc emits metallib directly (MVP) and DXIL (post-MVP); no DXIL pivot; descriptor layout / bindings inferred from slangc native reflection. |
| Pipeline State Cache Design | `docs/design/rendering/pipeline-state-cache.md` R-2.3.9.2 / R-2.3.9.8 | PSO key composes shader hash with device fingerprint (PSO cache lives in `render`); descriptor layout inferred from slangc reflection **once** and cached — confirms reflection is a `shader`-context output, not a render-thread runtime job. |
| Advanced Materials Reqs | `docs/requirements/rendering/advanced-materials.md` R-2.12.9 | Custom material graphs **codegen Slang** consumed by this same slangc pipeline; the `material` context emits Slang into `shader`'s front door — confirms Slang is the engine-wide source-of-truth shader language. |
| Rendering Core Design | `docs/design/rendering/rendering-core.md` Material System | `Material` references a `ShaderPermutationCache` keyed by `PermutationKey`; `ShadingModel` enum is the same axis used here. Confirms `render` and `material` both consume artifacts keyed by `PermutationKey`. |
| Render Pipeline Design — RF-9 | `docs/design/rendering/render-pipeline.md` § RF-9 | Hot-reload re-runs reflection on new bytecode so descriptor layout stays in sync. Adopted as the `shader`-context hot-reload contract obligation; the runtime PSO-invalidate side belongs to `render`. |

### Occam collapses (multiple harmonius concepts → one glibre primitive)

1. **Shader source language: many → one (Slang via slangc).** Harmonius's
   advanced-materials text waves at Slang as the lingua franca but its
   broader render docs and assorted prior tooling left ambiguous room
   for Slang front-ends, MSL hand-authored shaders, and direct GLSL
   paths. Glibre collapses to **Slang only** at the source layer and a
   single tool pipeline:

   ```text
   Slang source ─► slangc ─► metallib   (MVP — Apple Silicon / Metal 4)

   (post-MVP, when Windows lands)
   Slang source ─► slangc ─► DXIL       (D3D12)
   ```

   slangc emits the platform bytecode directly from Slang source in a
   single subprocess hop. There is no transpile chain and no
   intermediate pivot format. MSL is never hand-authored. GLSL is
   rejected outright. Justification: SRP — one front-end, one error
   vocabulary, one reflection format. (PHILOSOPHY §1, §10.)

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
   Glibre collapses to **subprocess CLI only** for slangc — never
   linked in-process, never run on the render thread. Justification:
   SRP for plugin boundaries (no slangc ABI bleed into core) and
   determinism (subprocess sandbox + content hash on inputs gives
   reproducible artifacts).

4. **Reflection sources: many → one (slangc native reflection).**
   Harmonius mentioned three reflection paths (compiler-produced
   container metadata, cross-reflection libraries, Metal IR
   inspection). Glibre collapses to slangc's native reflection API:
   reflection is emitted alongside the bytecode in the same subprocess
   invocation. There is no bytecode-container parsing; the reflection
   record and the bytecode are paired outputs of one slangc run.

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
   `advanced-materials.md` R-2.12.9 lets material graphs codegen Slang
   *into* this pipeline. That codegen lives in the `material` context
   (visual nodes, node-type registry, graph compilation to Slang).
   `shader` consumes the resulting Slang like any other shader source —
   it does not author or own material-graph node semantics.

3. **Runtime shader compilation in shipping.** Harmonius's
   shader-variants doc included an `OnDemandCompiler` runtime path.
   Glibre **rejects** this for shipping builds: shipping ships only
   precompiled artifacts loaded by handle, full stop. Editor / dev
   builds may invoke the same offline pipeline on save, but this is a
   tooling path, not a runtime path. Gating: `#if GLIBRE_SHIPPING` at
   plugin manifest level — the runtime slangc subprocess code is
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

**Responsibility.** Own one Slang translation unit on disk and the set
of stage-tagged entry points it exposes. Validates that the file
parses, that every entry point carries exactly one stage attribute
(`[shader("vertex")]`, `[shader("pixel")]`, …), and that the file's
preprocessor closure is finite and reproducible. Does **not** compile;
does **not** know about permutations.

- **Identity.** Stable `SourceId` derived from the project-relative
  source path; survives moves only via explicit rename, never silently.
- **Owns.** The Slang byte stream, the include-graph closure (paths +
  content hashes), and the entry-point manifest (name + stage).
- **Exposes.** `SourceId`, `EntryPoint { name, Stage }`,
  `PreprocessedSource` (post-include byte stream + total content hash).
- **SRP.** "Validate and present an authored Slang translation unit."
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
about Slang, slangc, or any backend.

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
CompileTarget)` through the single `slangc` subprocess invocation that
emits the requested bytecode (`metallib` for MVP, `DXIL` post-MVP)
directly from Slang source, and return a sealed `ShaderArtifact`. Owns
invocation, argument construction, sandbox controls, stdout/stderr
capture, and exit-code translation into `shader::Error`. Does **not**
persist artifacts; does **not** reflect; does **not** link.

- **Identity.** Stateless; one instance per offline build.
- **Owns.** The slangc subprocess descriptor (binary path, argv
  template, per-target flags) and the compile-flag canonicalizer used
  by the cache key.
- **Exposes.** `compile(ShaderSource, PermutationKey, CompileTarget) →
  Result<ShaderArtifact>` and the typed flag struct that feeds it.
- **SRP.** "Run the offline compiler, deterministically."
  Caching, reflection, and linking are explicitly elsewhere.

**Invariants.**

1. slangc is invoked **only** as a subprocess; in-process linkage is
   forbidden (Occam collapse §3.3).
2. Each `(ShaderSource, PermutationKey, CompileTarget)` is produced by
   exactly one `slangc` subprocess invocation that emits the target
   bytecode directly from Slang source. There is no transpile chain
   and no parallel Slang→MSL path. (Reinforces invariant 2 in §4.4.)
3. The pipeline is a no-op symbol in shipping builds: the entire
   `CompilationPipeline` translation unit is excluded from the
   shipping `shader` plugin via `#if !GLIBRE_SHIPPING` at the plugin
   manifest layer. Shipping builds NEVER invoke slangc.
4. Compile flags are canonicalized (sorted, deduplicated) before they
   feed the cache key, so semantically-equal invocations hash equally.

### 4.4 `ReflectionBlob` (value object, aggregate boundary)

**Responsibility.** Carry the structured metadata produced by slangc's
native reflection API alongside the emitted bytecode — entry points,
register bindings, descriptor frequency tags, vertex IO layout,
push-constant ranges, sampler bindings, RT payload sizes,
specialization constant slots. Constructed exclusively by ingesting
slangc's reflection output, paired 1:1 with the bytecode from the same
subprocess invocation. Does **not** persist; does **not** know about
backend descriptor heaps.

- **Identity.** Value semantics; equality is structural over its
  fields. A `ReflectionBlob` is paired 1:1 with its source
  `ShaderArtifact`.
- **Owns.** All reflected metadata and the slangc-reflection ingester
  that produced it.
- **Exposes.** `ReflectionBlob`, the descriptor-binding records
  tagged with `DescriptorFrequencyGroup`, the `MaterialParameterBlock`
  layout, and the `VertexIOLayout` used by `render` validation.
- **SRP.** "Translate slangc's native reflection output into a
  canonical metadata record." Mapping that record onto a
  backend-specific descriptor layout belongs to §4.5.

**Invariants.**

1. A `ReflectionBlob` is produced **only** from slangc's native
   reflection output, paired with the bytecode from the same slangc
   invocation. There is no bytecode-container re-parsing; reflection
   and bytecode are joint outputs of one slangc run. (Occam collapse
   §3.4.)
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
implementation is the Slang backend wrapping the `slangc` subprocess;
it emits `metallib` (MVP) and `DXIL` (post-MVP) directly from Slang
source. Defines four operations and nothing else.

- **Identity.** One implementing plugin per source-language family;
  selected at offline-build configuration time.
- **Owns.** The trait declaration; concrete backends live in plugin
  dylibs.
- **Exposes.** Four operations:

  | Op             | Signature                                                                   |
  |----------------|-----------------------------------------------------------------------------|
  | `compile`      | `(ShaderSource, PermutationKey, CompileTarget) → Result<ShaderArtifact>`    |
  | `reflect`      | `(ShaderArtifact) → Result<ReflectionBlob>`                                 |
  | `link`         | `(span<ShaderArtifact>) → Result<LinkedModule>` (spec-const baking when the target supports it)  |
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
2. **Reflection is paired with bytecode at slangc invocation time.**
   A `ReflectionBlob` is produced by slangc's native reflection API in
   the same subprocess invocation that emitted the bytecode; bytecode
   containers are never re-parsed for reflection.
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
out so shipping plugins never link slangc subprocess code — §4.3 invariant
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

#include <EASTL/array.h>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <EASTL/span.h>
#include <EASTL/string.h>
#include <EASTL/string_view.h>
#include <EASTL/vector.h>

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
    MetalLibEmitFailed,
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
    using PackedBytes = eastl::array<std::byte, 6>;
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

// MVP target is MetalLib; DXIL added post-MVP for D3D12/Windows.
enum class CompileTarget : std::uint8_t { MetalLib, DXIL };

// BLAKE3 of (preprocessed source ∪ resolved key ∪ canonical flags ∪ target).
struct ShaderHash {
    eastl::array<std::byte, 32> bytes{};
    friend constexpr bool operator==(ShaderHash, ShaderHash) noexcept = default;
};

// -------- ShaderSource (aggregate root, §4.1) ----------------------------

struct SourceId {
    eastl::string project_relative_path;
    friend bool operator==(const SourceId&, const SourceId&) noexcept = default;
};

struct EntryPoint {
    eastl::string name;
    Stage       stage{Stage::Vertex};
    friend bool operator==(const EntryPoint&, const EntryPoint&) noexcept = default;
};

struct IncludeNode {
    eastl::string project_relative_path;
    ShaderHash  content_hash{};
};

struct PreprocessedSource {
    eastl::vector<std::byte>   bytes;            // post-include byte stream
    eastl::vector<IncludeNode> include_closure;  // ordered, acyclic, project-rooted
    ShaderHash               total_hash{};
};

class ShaderSource {
public:
    static std::expected<ShaderSource, Error>
    open(const std::filesystem::path& project_root,
         const std::filesystem::path& project_relative);

    const SourceId&             id() const noexcept;
    eastl::span<const EntryPoint> entry_points() const noexcept;
    const PreprocessedSource&   preprocessed() const noexcept;

private:
    ShaderSource() = default;

    SourceId                id_{};
    eastl::vector<EntryPoint> entry_points_{};
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
    eastl::string              name;

    friend bool operator==(const BindingSlot&, const BindingSlot&) noexcept = default;
};

struct VertexInputElement {
    eastl::string   semantic;
    std::uint32_t semantic_index{0};
    std::uint32_t location{0};
    std::uint32_t format_code{0};   // backend-neutral format ordinal
};

struct VertexIOLayout {
    eastl::vector<VertexInputElement> elements;
};

struct PushConstantRange {
    std::uint32_t offset{0};
    std::uint32_t size{0};
    StageMask     stages{};
    friend constexpr bool operator==(PushConstantRange, PushConstantRange) noexcept = default;
};

struct MaterialParameterBlock {
    eastl::string              name;
    std::uint32_t            size_bytes{0};
    eastl::vector<BindingSlot> members;  // members reflected as named scalar/vector slots
};

struct SpecializationConstantSlot {
    eastl::string   name;
    std::uint32_t id{0};
    std::uint32_t size_bytes{0};
};

struct ReflectionBlob {
    eastl::vector<EntryPoint>                 entry_points;
    eastl::vector<BindingSlot>                bindings;        // one frequency tag each
    VertexIOLayout                          vertex_io;
    eastl::vector<PushConstantRange>          push_constants;
    MaterialParameterBlock                  material_parameters;
    eastl::vector<SpecializationConstantSlot> spec_constants;
    std::uint32_t                           rt_payload_bytes{0};
};

// -------- DescriptorLayout (value object, §4.5) --------------------------

struct DescriptorTable {
    // Ordered by (register_space, register_index, stage_mask) — §4.5 inv 3.
    eastl::vector<BindingSlot> slots;

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
    eastl::vector<PushConstantRange> push_constants;
    eastl::vector<StaticSampler>     static_samplers;

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
    CompileTarget          target{CompileTarget::MetalLib};
    ShaderHash             hash{};
    eastl::vector<std::byte> bytecode;
    ReflectionBlob         reflection;
    DescriptorLayout       descriptor_layout;
};

struct LinkedModule {
    eastl::vector<std::byte> bytecode;
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
    cook(eastl::span<const PermutationKey> enumerated) const;

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
// the shipping `shader` plugin never invokes slangc or slangc
// (§4.3 invariant 3, §4.8 invariant 3).

class IShaderBackend {
public:
    virtual ~IShaderBackend() = default;

#if !GLIBRE_SHIPPING
    // slangc + slangc subprocess driver. Excluded from shipping.
    virtual std::expected<ShaderArtifact, Error>
    compile(const ShaderSource&, const PermutationKey&, CompileTarget) = 0;
#endif

    virtual std::expected<ReflectionBlob, Error>
    reflect(const ShaderArtifact&) = 0;

    virtual std::expected<LinkedModule, Error>
    link(eastl::span<const ShaderArtifact>) = 0;

    virtual Capabilities capabilities() const noexcept = 0;
};

// -------- Stable string mapping for structured logs (error-model.md) ----

constexpr eastl::string_view to_string(Error e) noexcept {
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
        case Error::MetalLibEmitFailed:            return "MetalLibEmitFailed";
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
| `ShaderSource` + `EntryPoint` + `PreprocessedSource` | §4.1 | Slang translation unit + stage manifest. |
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
that shipping plugins never link slangc subprocess code.

## 6. Internal Architecture

Non-binding sketch for implementers. Directory layout, subprocess
topology, reflection parser shape, and shipping-build exclusions —
nothing here is normative beyond what §3, §4, and §5 already pin.
The aggregates of §4 each map to exactly one subdirectory under
`shader/`; cross-aggregate traffic flows along the §4 dataflow diagram
and never around it.

### 6.1 Module layout

The `shader` plugin source tree (`plugins/shader/src/`) is partitioned
along aggregate boundaries (§4) — one directory per aggregate, no
cross-cutting helpers:

```text
plugins/shader/
    include/glibre/shader/
        shader.hpp            # The §5 public header. Sole compileable
                              #   surface; nothing else in the engine
                              #   includes a `shader` private header.
    src/
        source/               # §4.1 ShaderSource — Slang frontend
            preprocessor.hpp/.cpp     # Tokenizer + #include expander.
                                      #   Build-time only.
            include_resolver.hpp/.cpp # Project-rooted resolver. Rejects
                                      #   absolute paths and `..`-escapes
                                      #   with Error::IncludeEscape.
            entry_point_scanner.hpp/.cpp # Extracts `[shader("...")]`-tagged
                                      #   entry points; one stage tag per
                                      #   function (§4.1 invariant 1).
            shader_source.cpp         # Aggregate root: ties the three
                                      #   above into ShaderSource::open().
        permutation/          # §4.2 PermutationKey — 4-axis codec
            axes.hpp                  # Closed enums + cardinalities,
                                      #   mirrors §5 declarations.
            packed_key.hpp/.cpp       # to_bytes() / from_bytes() — bit-
                                      #   stable across hosts (§4.2
                                      #   invariant 1).
            permutation_index.hpp/.cpp # Dense ordinal across the cross-
                                      #   product; mixed-radix encode in
                                      #   tuple-field order.
            enumeration_table.hpp/.cpp # Build-time enumerator that walks
                                      #   the resolved (project-pruned)
                                      #   PermutationKey set; consumed by
                                      #   the cooker (§6.4).
        backend/              # §4.7 IShaderBackend impls
                              #   EXCLUDED from shipping (§4.3 inv 3,
                              #   §4.8 inv 3). #if !GLIBRE_SHIPPING.
            slang/                 # The sole production backend.
                slangc_argv_builder.hpp/.cpp  # Canonicalized flag list
                                              #   (sorted, deduped); feeds
                                              #   the cache key (§4.3 inv 4).
                slangc_subprocess.hpp/.cpp    # Spawns slangc, captures
                                              #   stdout/stderr/exit;
                                              #   translates non-zero exits
                                              #   into shader::Error.
                slang_backend.cpp             # IShaderBackend impl wrapping
                                              #   the slangc subprocess;
                                              #   emits metallib (MVP) and
                                              #   DXIL (post-MVP).
        reflection/           # §4.4 ReflectionBlob — slangc reflection ingester
            slangc_reflection_ingester.hpp/.cpp # Ingests slangc's native
                                      #   reflection JSON emitted alongside
                                      #   the bytecode; produces the
                                      #   canonical ReflectionBlob.
            frequency_tagger.hpp/.cpp # Maps every reflected binding to one
                                      #   DescriptorFrequencyGroup; refuses
                                      #   ambiguity with
                                      #   Error::DescriptorFrequencyAmbiguous
                                      #   (§4.4 inv 3).
            descriptor_layout.cpp     # §4.5 — projects ReflectionBlob onto
                                      #   the four-frequency-table schema.
        cache/                # §4.6 ShaderCache — CAS + manifest
            blake3.hpp/.cpp           # BLAKE3 hasher — single source of
                                      #   the engine's ShaderHash.
            cas_store.hpp/.cpp        # Content-addressable filesystem
                                      #   store: artifacts/<aa>/<bb>/<hash>
                                      #   layout (§7.5). Idempotent insert,
                                      #   immutable values.
            manifest.hpp/.cpp         # ShaderCacheManifest reader/writer
                                      #   (Fory-backed, §7.1). Sole
                                      #   sidecar; sorted by artifact_hash.
            cooker.hpp/.cpp           # Walks the permutation enumeration
                                      #   table; populates CAS; emits the
                                      #   shipping ShaderLibrary. Sole
                                      #   writer (§7.5); never invoked at
                                      #   runtime in shipping builds.
            integrity.hpp/.cpp        # Cooker-time orphan-blob and
                                      #   dangling-reference checks
                                      #   (Error::CacheIntegrity, §4.6
                                      #   inv 3).
            library.cpp               # ShaderCache::Library — the shipping
                                      #   read-only handle (§4.6 inv 2).
        backend.cpp           # IShaderBackend trait selector + plugin
                              #   manifest entry point.
```

**SRP per directory.** Each subdirectory owns exactly one §4 aggregate:
`source/` ↔ §4.1, `permutation/` ↔ §4.2, `backend/` ↔ §4.3 + §4.7,
`reflection/` ↔ §4.4 + §4.5, `cache/` ↔ §4.6. No file reaches across
directory boundaries except through the public types declared in
`include/glibre/shader/shader.hpp`. There is no cross-cutting
`shader/util/` directory — utilities (BLAKE3 hasher, subprocess
launcher) live next to their sole consumer.

**Build-system gating.** The plugin's `CMakeLists.txt` partitions
sources by shipping eligibility. Only the build system distinguishes
shipping from tooling builds; no in-tree code branches on
`GLIBRE_SHIPPING` outside the `#if`-guards of §5 and §4.3 invariant 3.

| Directory | Shipping build | Tooling / dev build |
|-----------|----------------|---------------------|
| `source/` | excluded | included |
| `permutation/` | included (codec only) | included |
| `backend/` | **excluded entirely** | included |
| `reflection/` | included | included |
| `cache/` | included (lookup + Library; cooker excluded) | included |

The shipping `shader` plugin therefore links only:
`permutation/` + `reflection/` + `cache/{blake3,cas_store,manifest,
library}.cpp`. slangc, the include resolver, and the cooker are not in
the shipping binary at any link layer (§4.3 invariant 3, §4.8 invariant
3, §3 refusal 3).

### 6.2 Subprocess flow: `glibre-shadercc`

The `slangc` subprocess is wrapped by a single offline driver
binary, **`glibre-shadercc`**, which lives under `tools/shadercc/` and
is not part of the `shader` plugin. The plugin's `backend/` code spawns
this driver as a subprocess; the driver in turn spawns `slangc`. The
engine never links slangc and never invokes it at runtime — only the
offline cooker, the editor, and tests reach it, and only via this
driver.

```text
                       (offline build / editor / tests only)

  CompilationPipeline (§4.3) ──► spawn ──► glibre-shadercc
                                                │
                                                └──► spawn ──► slangc
                                                              ( --target=metallib  (MVP)
                                                              | --target=dxil      (post-MVP) )

  Outputs ◄── stdout (bytecode bytes, length-prefixed) ──── glibre-shadercc
           ◄── stderr (structured shader::Error JSON) ────
           ◄── exit code (0 = success, 1..N = enumerated Error)
```

**Why a wrapper exists.**

1. **One subprocess hop per artifact.** The plugin spawns
   `glibre-shadercc` once per `(ShaderSource, PermutationKey,
   CompileTarget)`; the driver invokes `slangc` once with the
   target-appropriate flags and returns the bytecode plus paired
   reflection. There is no transpile chain.
2. **One canonicalized argv schema.** The driver normalizes flag order
   and quoting before invoking slangc, so the cache-key inputs (§4.3
   invariant 4) come from the driver's stable argv schema, not from
   the plugin's per-call argv builder. Two callers with semantically
   equal flag sets produce byte-equal driver argv.
3. **One error vocabulary.** The driver maps slangc exit codes onto
   the closed `shader::Error` enum (§5 / §10). The plugin parses a
   stable JSON error envelope from stderr; it never inspects
   slangc-native diagnostics directly.
4. **Sandbox policy in one place.** The driver runs each compiler
   under macOS `sandbox-exec` (or the platform-equivalent seccomp
   filter on Linux dev hosts) with read access scoped to the project
   source root and write access scoped to a per-invocation temp dir.
   Reproducibility flags (`-Qstrip_debug`, fixed `-O3`,
   `-Zsb`-disabled time stamps) are pinned in the driver, not the
   plugin (Occam collapse §3.3).

**Engine never links slangc at runtime.** The runtime read path is
`ShaderCache::Library::get(ShaderHash)` (§4.6, §4.8 invariant 3).
There is no fallback path that spawns `glibre-shadercc` from a
shipping process; the driver binary is not bundled into shipping
distributions, and the shipping plugin's `backend/` directory is not
linked in, so calling `IShaderBackend::compile` is unreachable code by
construction (§5 `#if !GLIBRE_SHIPPING` guard, §4.3 invariant 3).

**Driver location.** `tools/shadercc/` (a `tools` context plugin per
`specs/tools/SPEC.md`). The `shader` context owns the *contract* with
the driver — its argv schema, its stderr JSON envelope, its exit-code
table — but does not own the driver source. This keeps slangc
version-pinning out of the `shader` plugin's ABI surface
(PHILOSOPHY §1, §10).

### 6.3 Reflection: ingest slangc native reflection

`reflection/` does **not** parse bytecode containers. slangc emits a
native reflection record alongside each compiled artifact in the same
subprocess invocation; the §4.4 `ReflectionBlob` is built by ingesting
that record and tagging each binding with its descriptor frequency.

The ingester is small:

| Module | Owns | Produces |
|--------|------|----------|
| `slangc_reflection_ingester.hpp/.cpp` | Reads the slangc-emitted reflection JSON paired with the bytecode in the same subprocess invocation; lifts each entry-point, binding, vertex-input element, push-constant range, sampler binding, and RT payload size into the canonical `BindingSlot`-shaped temporaries. | `EntryPoint` list, untagged `BindingSlot` set, `VertexIOLayout`, `PushConstantRange` list, `rt_payload_bytes`. |
| `frequency_tagger.hpp/.cpp` | Per-binding frequency assignment from Slang `[register(..., space=N)]` conventions and from explicit `[frequency(...)]` annotations the engine standardizes. | Each `BindingSlot` annotated with exactly one `DescriptorFrequencyGroup`. |
| `descriptor_layout.cpp` | Project `ReflectionBlob` onto §4.5 four-frequency tables; sort each table by `(register_space, register_index, stage_mask)`. | `DescriptorLayout` + `RootSignatureSchema`. |

**Determinism.** The ingester walks slangc's reflection record in a
fixed canonical order; the frequency tagger is a pure function of its
inputs. Ingesting the same reflection record twice yields a
structurally-equal `ReflectionBlob` (§4.4 invariant 2).

**Pairing with bytecode.** Reflection and bytecode are joint outputs
of one slangc run; the artifact stores both as a unit (§4.4 invariant
1, §3 collapse 4). Bytecode containers are never re-parsed for
reflection.

**Ship-time presence.** `reflection/` is in the shipping plugin —
hot-reload (§8) re-runs the ingester on freshly emitted reflection
records in editor / dev builds, and the runtime descriptor-layout
consumer in `render` reaches into stored `ReflectionBlob` records via
the cache. The ingester depends on nothing outside `<cstdint>` /
`<span>` / `<vector>` plus a small JSON reader, and carries no slangc
dynamic linkage.

### 6.4 Cache: BLAKE3 keys and the cooker walk

`cache/` is the §4.6 aggregate's implementation. Two responsibilities,
two clean halves:

**Read path (shipping + tooling).**

1. `ShaderCache::open(root, read_only=true)` memory-maps
   `manifest.fory` (§7.5) and parses it with the codegen'd Fory reader
   from the `data` middleman.
2. `get(ShaderHash)` performs the BLAKE3-keyed lookup:
   a. Search the manifest's `entries` (sorted ascending by
      `artifact_hash`, §7.3) via binary search.
   b. On hit, derive the CAS file path
      `artifacts/<aa>/<bb>/<hash>` (`<aa><bb>` = first 4 hex chars of
      the BLAKE3) and memory-map the `ShaderArtifactRecord` blob.
   c. Parse into an in-memory `ShaderArtifact` value (§5).
3. Returns `nullptr` on miss — there is no compile-on-miss path in
   shipping (§4.6 invariant 2).

The shipping plugin exposes only this read path. `ShaderCache::insert`
and `ShaderCache::cook` are present on the type for tooling builds
but their implementation TUs are excluded from the shipping link
target.

**Write path (cooker; tooling only).** The cooker is the sole writer
to a `ShaderCache` (§7.5) and runs strictly offline. Its algorithm:

```text
cook(span<const PermutationKey> enumerated_keys):
    1. enumerated_keys = permutation/enumeration_table.walk(project_pruner)
       // The 4-axis cross-product is finite (§4.2). Project-level
       // pruning (e.g. "this game does not ship Skin shading on Mobile")
       // is a build-system responsibility; the table walks the resulting
       // resolved set in tuple-field order.

    2. For each key k in enumerated_keys:
       2a. For each (CompileTarget t) in {MetalLib}:    // MVP; DXIL added post-MVP
           2b. source_hash := ShaderSource::preprocessed().total_hash
           2c. flags_hash  := BLAKE3(canonical_flags(k, t))
           2d. artifact_hash := BLAKE3(source_hash || k.to_bytes() ||
                                      flags_hash || u8(t))
           2e. if cas_store.has(artifact_hash):
                   continue              // idempotent (§4.6 inv 1)
           2f. artifact := IShaderBackend::compile(source, k, t)   // §6.2
           2g. artifact.reflection := IShaderBackend::reflect(artifact) // §6.3
           2h. artifact.descriptor_layout := DescriptorLayout::derive(
                                                 artifact.reflection)  // §4.5
           2i. cas_store.insert(artifact_hash, ShaderArtifactRecord{...})

    3. manifest := ShaderCacheManifest{
           cache_root_relative: <relative path>,
           schema_abi_hash:     glibre_types_abi_hash(),
           entries:             sort_ascending(by artifact_hash, all CAS entries),
           enumerated_keys:     sort_ascending(by k.to_bytes(), enumerated_keys),
       }

    4. integrity_check(manifest, cas_store):
       - Every manifest entry has a CAS file (no dangling refs).
       - Every CAS file is referenced by the manifest (no orphan blobs).
       - Both failures raise Error::CacheIntegrity (§4.6 inv 3).

    5. atomic_write(<library_root>/manifest.fory, manifest)
```

Everything in steps 2f–2h lives behind the `#if !GLIBRE_SHIPPING`
boundary. Steps 2a–2e + 3–5 live in `cache/cooker.cpp`, which is
itself excluded from the shipping link target. The shipping plugin
therefore contains only the §6.4 read path: BLAKE3 hasher + CAS file
mapper + manifest reader + binary search.

**BLAKE3 source.** A single in-tree BLAKE3 implementation lives at
`cache/blake3.hpp/.cpp`. The same hasher is used both for cache keys
(this section) and for include-graph content hashes (§4.1) — one
hash family across the context, no SHA-/MD-/xxhash drift.

### 6.5 Shipping-build behavior — what survives the cut

Restating the §6.1 build-system table as a runtime contract:

| Surface | Shipping | Justification |
|---------|----------|----------------------------|
| `source/` (Slang frontend) | **excluded** | §3 refusal 3, §4.3 inv 3 — shipping never opens Slang. |
| `permutation/` | included | §4.2 — codec is consumed at runtime to map a `PermutationKey` to the cache lookup. Pure value math; no I/O. |
| `backend/slang/` | **excluded** | §4.3 inv 3, §4.8 inv 3 — slangc is tooling-only. The whole `IShaderBackend::compile` virtual is `#if`-guarded out of §5, so callers cannot even reference it. |
| `reflection/` | included | §6.3 — slangc-reflection ingester is tiny and dependency-free. Reads stored `ReflectionBlob` records on artifact load; no slangc linkage. |
| `cache/{blake3, cas_store, manifest, library}.cpp` | included | §4.6 inv 2 — sole runtime read path. Manifest is mmap'd, CAS files are mmap'd, BLAKE3 is the lookup-key codec. |
| `cache/cooker.cpp`, `cache/integrity.cpp` | **excluded** | Sole writer is offline (§7.5). Shipping is read-only. |
| `tools/shadercc/` (`glibre-shadercc` driver) | not bundled | §6.2 — the driver binary is a tooling artifact; it is not redistributed in shipping. |

The shipping `shader.dylib` therefore contains: §4.2 (key codec), the
slangc-reflection ingester, BLAKE3, the CAS read path, the manifest
reader, and `ShaderCache::Library`. Nothing else. PHILOSOPHY §6
("zero runtime reflection in shipping") holds for the *engine*: any
reflection that runs at shipping time is a bounded read of a
pre-cooked, content-addressed `ReflectionBlob` — never a live
ingestion — but the ingester itself ships so the (rare) editor /
asset-pipe build that wants to re-ingest reflection from a bundled
artifact still has the code.

### 6.6 Dataflow recap (§4 diagram, projected onto §6)

The §4 dataflow:

```text
ShaderSource ─► PermutationKey ─► CompilationPipeline ─► ReflectionBlob
                                       │                       │
                                       ▼                       ▼
                                ShaderArtifact ─► ShaderCache  DescriptorLayout
                                       │            (CAS)            │
                                       └────► IShaderBackend ◄───────┘
```

Maps onto §6 directories as:

```text
source/ ─► permutation/ ─► backend/ ──► reflection/
                              │              │
                              ▼              ▼
                            cache/ (cooker, write) ──► reflection/descriptor_layout
                              │                                  │
                              └────► backend/ (IShaderBackend) ◄─┘

(shipping link target = permutation/ + reflection/ + cache/{read})
```

No directory is reachable from another except along the arrows above,
and the shipping cut removes both `source/` and `backend/` without
breaking the remaining link.

## 7. Persistence & Schemas

The `shader` context persists three on-disk record types via Apache Fory
schemas under the `data` middleman dylib (`reviews/decisions/fory-codegen.md`).
Schemas live at `data/schemas/shader/<Type>.fory`; codegen emits
`glibre::types::shader::*` POD-like structs that the in-memory C++ types
in §5 serialize into. **Slang source is never persisted** — it stays
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
  field bytecode        : bytes                               tag 6 since 1   // metallib (MVP) or DXIL (post-MVP) bytes
  field reflection      : glibre.shader.ReflectionRecord      tag 7 since 1
  field descriptors     : glibre.shader.DescriptorLayoutRecord tag 8 since 1
  field producer_label  : string                              tag 9 since 1   // backend identity (e.g. "slang-slangc-1.7")
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
3. **Slang source never enters a record.** Only the BLAKE3 `source_hash`
   appears. The `data/schemas/shader/` files are the only artifacts
   that ship; the Slang itself is repo-versioned (§3 collapse 1, §4.6
   invariant 2).
4. **Manifest stamps the middleman ABI.** `schema_abi_hash` is the
   value `glibre_types_abi_hash()` returns at cook time. The shader
   loader refuses a manifest whose hash differs from the host's
   (`Error::CacheIntegrity`); together with the dynamic linker's SONAME
   check this catches both schema-additive and schema-breaking drift.
5. **Reflection is paired with bytecode at compile time.** A
   `ShaderArtifactRecord` carries the `ReflectionRecord` slangc emitted
   alongside the bytecode in the same subprocess invocation
   (§4.4 invariant 1, §4.8 invariant 2). Bytecode containers are never
   re-parsed.
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

The `shader` context's hot-reload semantics differ from the engine-wide
plugin reload protocol (`reviews/decisions/hot-reload-protocol.md`) in
one fundamental way: **the unit of reload is an Slang source file, not
the `shader` plugin `.dylib`**. The plugin loader's drain → swap →
migrate → resume state machine governs `.dylib` swaps; `shader` does
not perform a plugin self-swap as its hot-reload story. Shader source
edits bypass the loader's vtable-swap step entirely and instead drive
a content-keyed cache-invalidation event observed by `render`. This
matches harmonius RF-9's intent (re-run reflection on new bytecode so
descriptor layout stays in sync) without conflating two unrelated
reasons-to-change (PHILOSOPHY §1).

### 8.1 Trigger

Exactly one trigger fires a `shader` hot-reload event in editor / dev
builds:

1. **Slang source change.** The editor's filesystem watcher observes a
   modified `.slang` translation unit (or any file in its include
   closure, §4.1 invariant 3) under the project source root. The
   watcher calls into the `shader` plugin to re-`open()` the affected
   `ShaderSource` and recompute its `PreprocessedSource.total_hash`.
   If the new hash differs from the cached hash, the affected
   `(ShaderSource, PermutationKey, CompileTarget)` set is the
   *affected permutation set*; everything else is unaffected.

The plugin `.dylib` swap path is **not** a `shader` hot-reload trigger.
The `shader` plugin is reloaded only by the engine-wide loader protocol
when the plugin's own code (slangc argv builder, reflection extractor,
backend trait implementation) changes — that is a `core`-driven event,
not a content event, and it inherits `hot-reload-protocol.md` verbatim.

Editor-driven partial reload is the path
`hot-reload-protocol.md` Open Question 5 anticipated; this section is
its `shader`-side answer for the source-change case.

### 8.2 What survives the swap

Shader hot-reload changes content-addressed artifacts on disk; nothing
in-memory needs migration. Specifically:

1. **`ShaderCache` entries for unaffected permutations survive
   verbatim.** Cache is keyed by `ShaderHash = BLAKE3(preprocessed
   source ∪ resolved PermutationKey ∪ canonical flags ∪ target)` (§2,
   §4.6 invariant 1). An edit to `foo.slang` changes the `source_hash`
   only for `(PermutationKey, target)` pairs that include `foo.slang`
   in their preprocessed closure. All other permutations retain their
   prior `ShaderHash`, prior bytecode, prior `ReflectionBlob`, and
   prior `DescriptorLayout` — bit-equal across the swap.
2. **`ShaderCache` entries for affected permutations are not
   "migrated" — they are recompiled.** The new artifact lives at a new
   `ShaderHash`; the old artifact remains in CAS as a now-unreferenced
   blob until the next `cook()` walk garbage-collects it (cooker is
   the sole writer; runtime never deletes). `insert` is idempotent
   (§4.6 invariant 1); re-keying is by construction.
3. **`PermutationKey`, `PermutationIndex`, axis enums, and the
   permutation enumeration table survive unchanged.** The four axes
   (§4.2) are spec-frozen; a source-only edit cannot grow a new
   `ShadingModel` or `FeatureBit`. Axis growth requires a spec
   amendment (§4.2 invariant 3) which routes through the engine-wide
   plugin reload, not this path.
4. **`ShaderCacheManifest` is rebuilt, not migrated.** The manifest
   is a derived index over the CAS (§7.4 rule 4); after a source
   change the dev-build cooker re-walks the resolved permutation set
   and writes a fresh manifest with updated `artifact_hash` entries
   for the affected permutations and unchanged entries for the rest.
5. **`PSOCache` entries for affected permutations are invalidated by
   `render`.** The PSO cache is owned by `render`, not `shader`
   (§4.8 invariant 1, §3 refusal 4); `shader` publishes the affected
   `ShaderHash` set on the observer bus and `render` evicts each PSO
   whose source artifact's hash changed, then rebinds via the new
   `ShaderHash` lookup. PSO cache entries for unaffected permutations
   survive — their input `ShaderHash` is bit-equal across the swap.
6. **No GPU resources cross the swap from `shader`'s side.** `shader`
   never owns GPU memory (§3 refusal 5); resource handles live in
   `render` and are re-acquired by `render`'s observer reaction to
   the invalidation event, not by `shader`.

### 8.3 `migrate(...)`: not applicable

The `shader` context **does not provide a `migrate_<Type>_vN_to_vNplus1`
function for source-change hot-reload**. The contract collapses to:

- **Persistent on-disk records** (§7) are migrated only when a record
  *schema* version bumps; that is the engine-wide path
  (`hot-reload-protocol.md` Step 3, `fory-codegen.md` migration
  table). A source-only edit never bumps a schema version, so no
  migrate function fires.
- **In-memory state to carry across the swap is empty by
  construction.** `shader` produces sealed, immutable
  `ShaderArtifact` values; downstream consumers (`render`, `material`)
  hold them by `ShaderHash`. The swap replaces the affected hash set;
  no live `ShaderArtifact` instance needs field-by-field reshape.
- **Cache-state survival is a content-hash equality check, not a
  migration.** The loader's "if it has a `.fory` schema, it survives"
  rule (`hot-reload-protocol.md` State Survival Rules) trivially
  applies: `ShaderArtifactRecord` and `ShaderCacheManifest` survive
  by definition. Their *content* changes for affected entries; their
  *layout* does not.

A future schema bump on `ShaderArtifactRecord`, `ReflectionRecord`, or
`DescriptorLayoutRecord` invokes the migration rules already locked
in §7.4 (rules 1–5); those rules belong to the schema-evolution path,
not to source-change hot-reload, and are not duplicated here.

### 8.4 Refusal cases

A `shader` hot-reload event refuses (i.e., the new artifact is not
published; the prior cache state remains live; affected PSOs are
*not* invalidated) in exactly four cases. The first is a hard refusal
of the trigger itself in shipping; the other three are recompile-time
failures that surface to the editor and leave the previous-good
artifact bound.

1. **Shipping build refuses to invoke slangc.** In any build with
   `GLIBRE_SHIPPING == 1`, the entire `CompilationPipeline`
   translation unit is excluded (§4.3 invariant 3, §4.8 invariant 3),
   and the filesystem watcher is not registered. A reload-on-source
   request synthesised from any source (e.g., a stray dev tool
   embedded in a shipping binary) is **ignored without error**: the
   shipping `shader` plugin has no path that can serve it, and
   `IShaderBackend::compile()` itself does not exist as a virtual at
   that ABI. If a request nonetheless reaches the plugin (for
   example, via the engine-wide test harness mistakenly enabled), it
   is rejected with `Error::ShippingCompilationAttempted`.
2. **slangc subprocess failure.** The subprocess invocation fails
   (`Error::CompilerInvocationFailed`), exits non-zero
   (`Error::CompilerExitNonZero`), or times out
   (`Error::CompilerTimedOut`) — see §10 for the closed enum. The
   editor surfaces the captured stderr; the in-flight `ShaderArtifact`
   is discarded; the prior CAS entry remains the live artifact for
   that `(PermutationKey, target)`.
3. **Reflection or descriptor-layout failure on the new bytecode.**
   The new bytecode reflects but yields a `ReflectionBlob` whose bindings
   include an unassigned descriptor frequency
   (`Error::DescriptorFrequencyAmbiguous` /
   `DescriptorFrequencyMissing`, §4.4 invariant 3) or whose
   `DescriptorLayout::derive(...)` rejects the schema. The new
   artifact never reaches `ShaderCache::insert`; the old artifact
   remains live.
4. **Cache integrity violation on insert.** A `ShaderHash` collision
   with a non-byte-equal payload (theoretically impossible under
   BLAKE3 but checked) or a manifest-vs-CAS skew detected by the
   cooker (§4.6 invariant 3) yields `Error::CacheIntegrity`; the new
   artifact is dropped and the affected permutation continues to
   resolve through the prior hash.

In every refusal case the `shader` plugin emits a structured `warn`
log entry (per `reviews/decisions/error-model.md`) carrying
`source_path`, `affected_permutation_count`, and the `Error`
enumerator name; it does **not** publish an invalidation event on the
observer bus. `render`'s PSO cache therefore continues to bind the
prior artifact deterministically.

### 8.5 Observer notification — `render` invalidation and rebind

`shader` and `render` communicate via a single typed event published
on the engine-wide observer bus (the same bus used by the loader
protocol's `HotReloadCompleted`, see `hot-reload-protocol.md`
Observer Notification). The event is a middleman type so its
on-the-wire layout survives plugin swaps:

```text
glibre::types::shader::ShaderArtifactReplaced {
    source_id              : SourceId            // §5
    affected_old_hashes    : list<ShaderHash>    // bit-stable order
    affected_new_hashes    : list<ShaderHash>    // 1:1 with old, same index
    affected_permutations  : list<PermutationKey>
    target                 : CompileTarget
}
```

`render` is the sole subscriber relevant to PSO cache invalidation.
On receiving a `ShaderArtifactReplaced` event:

1. For each `affected_old_hash`, `render` looks up every PSO whose
   shader-stage input keys include that hash and marks the entry
   invalid in `PSOCache`.
2. `render` rebinds the affected pipeline state by resolving the
   matching `affected_new_hash` through `ShaderCache::get(...)` and
   reconstructing the PSO from the new `ShaderArtifact` plus the
   surviving render-side device fingerprint.
3. PSOs whose source hashes are not in the affected set survive
   bit-equal — their cache lookups continue to hit the same record.

The event is published synchronously on the editor's tool thread
after `ShaderCache::insert` succeeds for the entire affected set;
subscribers see a fully-published cache, never a half-inserted one.
This mirrors the loader protocol's "synchronous on the loader thread,
between steps 4.2 and 4.3" rule (`hot-reload-protocol.md` Observer
Notification) and gives `render` a single deterministic point at
which to re-derive PSO state.

### 8.6 Test hooks

Two `shader`-side test entry points complement the loader's
`enqueue_hot_reload` / `await_reload` pair (`hot-reload-protocol.md`
Test Hooks). Both are guarded by `#if defined(GLIBRE_E2E)` and never
linked into runtime or shipping; they let the harness drive
source-change hot-reload deterministically without touching the
filesystem watcher.

```cpp
namespace glibre::shader::test {

// Inject an Slang diff for `source_id` as if the watcher had observed
// it; new_bytes replace the file's preprocessed body. Returns the
// affected permutation set discovered by re-keying through the
// permutation enumerator.
std::expected<eastl::vector<PermutationKey>, Error>
inject_source_diff(
    const SourceId&                source_id,
    eastl::span<const std::byte>     new_preprocessed_bytes
) noexcept;

// Synchronously drive the recompile + reflect + cache-insert chain
// for the given affected set, returning the published
// ShaderArtifactReplaced payload. Used by render-side tests to
// assert deterministic PSO invalidation.
std::expected<glibre::types::shader::ShaderArtifactReplaced, Error>
recompile_affected(
    const SourceId&                       source_id,
    eastl::span<const PermutationKey>       affected,
    CompileTarget                         target
) noexcept;

}
```

The CI matrix exercises four scenarios in a single deterministic
frame each:

- **Unaffected-permutation survival.** Inject a diff to `foo.slang`;
  assert that every permutation whose preprocessed closure does
  *not* include `foo.slang` retains its prior `ShaderHash` byte-equal,
  and that `render`'s `PSOCache` does not evict the corresponding
  PSO (zero invalidations on the unaffected set).
- **Affected-permutation invalidation.** Inject a diff that touches
  N permutations; assert exactly N old hashes appear in
  `affected_old_hashes`, exactly N new hashes appear in
  `affected_new_hashes`, the lists' indices line up, and `render`'s
  observer reaction evicts exactly those N PSOs and rebinds them
  through the new hashes.
- **Each refusal case.** Drive `inject_source_diff` with synthetic
  inputs that fail at slangc, at reflection, at descriptor derivation,
  and at cache insert; assert that no `ShaderArtifactReplaced` is
  published and that the prior `ShaderHash` continues to resolve
  through `ShaderCache::get`.
- **Shipping refusal.** Compile the test binary with
  `-DGLIBRE_SHIPPING=1`; assert that
  `glibre::shader::test::inject_source_diff` is not linked and that
  any synthesised reload request returns
  `Error::ShippingCompilationAttempted` through the
  test-only error gate.

The `inject_source_diff` symbol is the same in-process trigger used
by the dev-build watcher wrapper, so testing the in-process path
covers the full source-change hot-reload state machine.

### 8.7 Cross-context summary

| Concern | Owner | Survival |
|---------|-------|----------|
| Affected `ShaderArtifact` bytecode + reflection | `shader` (CAS, recompiled) | New hash, new blob; prior blob orphaned until cooker GC. |
| Unaffected `ShaderArtifact` records | `shader` (CAS) | Bit-equal across reload (§7.3 rule 1). |
| `ShaderCacheManifest` | `shader` (rebuilt) | Re-emitted by cooker; `schema_abi_hash` unchanged. |
| Affected `PSOCache` entries | `render` | Evicted on `ShaderArtifactReplaced`; rebound to new hash. |
| Unaffected `PSOCache` entries | `render` | Survive bit-equal (input hash unchanged). |
| GPU device fingerprint, descriptor heaps, command encoders | `render` | Survive — no `shader`-side dependency changed. |
| Schema migrations on §7 records | `data` middleman + `shader` | Out of band — fires only on schema bumps, not source edits. |

## 9. Performance Budget

Quotes the `shader` row of the engine-wide budget
(`reviews/decisions/perf-budget.md`) verbatim and refines it with the
per-aggregate breakdown that sums into that row. Frame-phase ownership
is consistent with `reviews/decisions/frame-phases.md`.

### 9.1 Budget cells (engine contract)

| Cell                | Value          | Source                                                    |
|---------------------|----------------|-----------------------------------------------------------|
| CPU sim (ms / frame)    | **0.00**   | `perf-budget.md` table row `shader`                       |
| CPU submit (ms / frame) | **0.00**   | `perf-budget.md` table row `shader`                       |
| GPU (ms / frame)        | **n/a**    | `shader` runs no GPU work; `render` owns all GPU cycles   |
| Heap ceiling            | **32 MiB** | `perf-budget.md` allocator-rules contract for `ContextTag::shader` |
| Phase ownership         | **none**   | `shader` owns no frame phase (1–9); cooked artifacts only |

The per-frame cost of the `shader` context in shipping builds is
**zero by construction** (PHILOSOPHY §6 codegen-everywhere; §4.3
invariant 3 and §4.8 invariant 3 — no compilation, no reflection,
no descriptor derivation, no hashing per frame). The 32 MiB heap is
the hot-set ceiling for the resident artifacts the runtime read path
holds memory-mapped or copied; cook-time arenas do not count against
it (§9.4).

### 9.2 Cook-time budgets are out of frame-budget scope

`CompilationPipeline` (§4.3) and the cooker (`cache/cooker.cpp`) run
**only** in the offline build / editor / tests; both are excluded from
the shipping link (§6.1 build-system gating, §4.3 invariant 3). slangc
and `slangc` subprocess wall-clock per artifact, total
cook duration over a full permutation enumeration, parallel cook
saturation on M1 firestorm/icestorm cores, and editor incremental-cook
latency are tracked separately under the `shader-cook-time-budget`
spike — they are **not** an input to the 16.67 ms / 60 fps frame
contract and may not be conflated with §9.1.

The only cook-related cost that touches a running shipping process is
the cold-start cache-index load (§9.5), which is a one-time init cost,
not a per-frame cost.

### 9.3 Runtime per-aggregate breakdown

Each aggregate's per-frame contribution. The sum is the §9.1
`shader` cell — `0.00 ms` CPU, no GPU work — by construction.

| Aggregate (§4)       | Per-frame CPU work | Hot-path cost            | In-memory hot set | Notes |
|----------------------|--------------------|--------------------------|-------------------|-------|
| `ShaderSource` (§4.1)        | none in shipping (frontend excluded by §6.1) | **0.000 ms** | 0 MiB | Editor / tests only. |
| `PermutationKey` (§4.2)      | codec only when render computes a `ShaderHash`; render holds keys directly | **0.000 ms** | <0.5 MiB | Dense ordinal tables baked at codegen; no allocations on hot path. |
| `CompilationPipeline` (§4.3) | excluded entirely from shipping (§4.3 inv 3) | **0.000 ms** | 0 MiB | Subprocess driver lives in `tools/shadercc/`. |
| `ReflectionBlob` (§4.4)      | parsed once at artifact load; immutable thereafter | **0.000 ms on hot path** | 4 MiB | One blob per resident artifact; consumers (`render`) hold const refs and never re-parse. |
| `DescriptorLayout` (§4.5)    | derived once per `(Backend, PermutationKey)` offline; runtime is table lookup (§4.5 inv 1) | **0.000 ms** | (counted under ReflectionBlob's 4 MiB) | No runtime re-derivation. |
| `ShaderCache` (§4.6)         | `lookup(ShaderHash) → optional<ShaderArtifact>` queries from `render`'s PSO build / reload paths | **~0.005 ms per lookup; 0 ms on PSOCache hit** | 24 MiB CAS in-memory hot set | Lookups are O(1) on the BLAKE3-keyed index; the 5 µs amortized cost is non-zero only on PSOCache miss and is absorbed by the *render* CPU-submit budget (`render` cell §9), not by `shader`'s 0.00 ms cell. |
| `IShaderBackend` (§4.7)      | trait surface; shipping has no implementation linked | **0.000 ms** | 0 MiB | Backend impls live behind `#if !GLIBRE_SHIPPING`. |
| (External) PSO queries        | render-side `PSOCache` keyed by `(shader_hash, state_hash)` (`specs/render/SPEC.md` §4.1.7) | **0 ms on cache hit; O(1) hash-map lookup on miss** | 4 MiB | The PSOCache itself is a `render` aggregate; recorded here only so the `shader` ↔ `render` seam's runtime cost is fully accounted. Any miss-path cost is render's, not shader's. |
| **Hot-set total**             |                                                      | **0.005 ms / lookup; 0 ms / frame steady-state** | **32 MiB** (24 + 4 + 4) | Equals §9.1 CPU 0.00 + heap 32 MiB. |

The hot-path budget is **0 ms / frame** in steady state because, after
init, `render`'s `PSOCache` (specs/render/SPEC.md §4.1.7) absorbs every
PSO request, and the `shader` cache index is not consulted on the per-
frame draw path. `ShaderCache::lookup` is exercised only on (a) cold
PSO build during a load / level-stream event, or (b) a dev-build hot-
reload (§8); neither is a steady-state cost.

### 9.4 Allocator rules (per `reviews/decisions/perf-budget.md`)

The `shader` context tags every allocation with `ContextTag::shader`
through `glibre::PerContextAllocator` (perf-budget.md "Allocator
Rules"). The 32 MiB ceiling decomposes:

| Pool                                                   | Ceiling | Class          |
|--------------------------------------------------------|---------|----------------|
| `ShaderCache` in-memory hot set (CAS bytes + index)    | 24 MiB  | resident, counted |
| `ReflectionBlob` records held by resident artifacts     | 4 MiB   | resident, counted |
| `DescriptorLayout` projections of resident reflections  | (within the 4 MiB above) | resident, counted |
| Cold-start manifest scratch (decode buffer, drained by phase 9 of the first frame after init) | 4 MiB | transient arena, **not counted** against the ceiling per perf-budget.md Allocator Rule §4 |
| Cooker / slangc scratch                                 | n/a     | shipping-excluded; counted under the editor's tag in dev builds |
| GPU-side memory                                         | 0       | `shader` allocates no GPU memory; PSO bytecode residency lives under the `render` 512 MiB tag (perf-budget.md Allocator Rule §5) |

Strict-mode (`GLIBRE_ALLOC_STRICT=1`) returns
`std::unexpected{core::Error::OutOfBudget}` if a `shader`-tagged
allocation would push live bytes over 32 MiB; shipping mode logs a
once-per-frame `warn` to `spdlog` (perf-budget.md Allocator Rules §2,
§3). The transient cold-start arena is exempt provided it drains by
phase 9 of the first frame after init (Allocator Rule §4); a leak
past phase 9 is `core::Error::OutOfBudget{detail="leak"}`.

### 9.5 Cold-start cost (one-time, not in frame budget)

Engine init pays a one-shot cost to open the cooked `ShaderLibrary`
archive and rehydrate the `ShaderCache` hot-set index:

- **`ShaderLibrary` open + manifest decode** ≤ **50 ms** wall-clock on
  the M1 baseline for an MVP-sized cooked archive (~10 k artifacts
  resident; manifest is Fory-decoded once per process, §7.1).
- This cost is paid before frame 1 of the game loop starts driving and
  is **not** charged against the 16.67 ms / 60 fps frame budget
  (perf-budget.md "Pipelined Frame Timing" — init work precedes the
  driver thread's first phase-1 tick).
- Subsequent `lookup` queries are O(1) against the rehydrated index;
  there is no second-tier cold path.

The 50 ms ceiling is recorded here as a contract on `shader`'s init
contribution, not as part of the per-frame cell. A regression that
exceeds 50 ms is a `shader`-spec violation, surfaced to the
`task-breakdown-error-perf` spike's startup-time budget rather than
to `perf-budget.yml`'s frame gate.

### 9.6 CI gate

The `perf-budget.yml` workflow (perf-budget.md "CI Gate Spec") gates
PRs that touch `plugins/shader/**` on three `shader`-specific
assertions in addition to the engine-wide gates:

1. **Per-frame `shader` cost = 0.** The S1 sample-scene replay (600
   frames) must record **0.000 ms** of CPU sim and CPU submit time
   under `ContextTag::shader`. Any non-zero sample is a violation
   (Catch2 `BENCHMARK` assertion in `tests/shader/perf/`).
2. **Heap ceiling.** The same diagnostic-build replay under
   `GLIBRE_ALLOC_STRICT=1` asserts that `shader`-tagged live bytes
   never exceed 32 MiB. The transient cold-start arena (§9.4) is
   exempt only inside frame 0; thereafter the ceiling applies.
3. **Shader cache hit-rate ≥ 99 %.** Across the S1 replay's
   `ShaderCache::lookup` calls (driven by `render`'s PSOCache misses
   plus level-stream events), the hit ratio against the cooked
   `ShaderLibrary` must be ≥ 99 %. A miss in shipping is
   `shader::Error::CacheLookupMiss` (§4.6 invariant 2 forbids any
   compilation fallback); the gate fails on a single miss past the
   99 % threshold and posts the offending `(ShaderHash, source path)`
   pairs to the PR. Sub-99 % almost always indicates a stale cooked
   archive, a missing permutation in the enumeration table (§6),
   or a `state_hash` change on the render side that left the
   cooked archive behind.

The 99 % threshold (rather than 100 %) reserves a small budget for
rare engine-internal events that legitimately ask for a
not-yet-resident artifact during a level-stream transition. A 100 %
gate would flag false positives the first time a streaming chunk
loads a permutation that was correctly enumerated but not yet warmed
into the hot set.

### 9.7 Cross-references

- Engine-wide budget: `reviews/decisions/perf-budget.md`
  (`shader` row; Allocator Rules; CI Gate Spec).
- Frame-phase non-ownership: `reviews/decisions/frame-phases.md`
  (phases 1–9; `shader` participates in none).
- Render-side absorption: `specs/render/SPEC.md` §4.1.7
  (`PSOCache`); the `shader` ↔ `render` cost seam is recorded in
  §9.3 row "(External) PSO queries" so neither context double-counts.
- Hot-reload (§8): a dev-build content reload triggers a one-shot
  `ShaderCache::lookup` storm for the affected permutations; that
  storm is not steady-state and is excluded from the per-frame gate.

## 10. Failure Modes & Error Model

The `shader` context returns `std::expected<T, shader::Error>` at every
public boundary (§5; `reviews/decisions/error-model.md`). The closed
`enum class shader::Error : std::uint16_t` declared in §5 is exhaustive
for this context — every failure mode in this section maps onto exactly
one enumerator, and no enumerator is reserved for a future failure that
is not yet specified (`Result<T>` is reserved for *actual* failure modes
per error-model decision §Composition Rules, item 4).

### 10.1 Severity vocabulary

Three severities; each is a fixed contract about how the shader plugin
reacts and what the *next frame* sees. Severities are not opinions about
how loud the log line is — they are testable bindings between an error
and the cache / observer-bus state.

| Severity | Meaning | Cache state after | Observer-bus event |
|----------|---------|-------------------|--------------------|
| `refuse` | Compile is rejected before any artifact is written. The prior cache state remains live; the offending input is blamed in `spdlog` and surfaced in editor UI. | unchanged | none — `render` keeps binding the prior artifact (§8.4) |
| `fallback` | Lookup misses or yields a known-good prior; the read path serves the prior content-addressed `ShaderArtifact`. The miss is observable to callers via the `Result` return but the engine continues at full functionality. | prior entry remains authoritative | none |
| `fatal` | An invariant has been violated that cannot be recovered without re-cooking. The error is logged at `error` level and bubbles to the caller; there is no in-process retry, and shipping builds may abort the process at the editor / asset-pipe boundary. | quarantined; cooker must re-run | none — surfacing is via `Result`, not the bus |

The §8.4 *refusal-vs-publish* contract is the runtime projection of
this table: every `refuse` severity here corresponds to a hot-reload
refusal there, and vice versa.

Recovery for every entry is mechanical (`refuse compile`, `fall back
to prior cache entry`, or `fail the cook with no artifact emission`).
The §10 contract bans silent retry: the plugin does **not** re-spawn
slangc on transient subprocess failure, does **not** synthesise a "best
effort" `ReflectionBlob`, and does **not** publish a partially-cooked
artifact. Retries, when they happen, happen at the cooker / editor
layer that called us, never inside `shader`.

### 10.2 Per-enumerator contract

The table below pins every §5 `enum class shader::Error` arm to its
trigger, the recovery policy the plugin executes, and the severity
class from §10.1. Names in **bold** are the canonical §5 enumerators;
the parenthetical italic name is the alias used in spike #79's
deliverable list (kept here so reviewers can cross-walk the spike's
checklist against the spec without mutating §5).

| Enumerator (§5) | Trigger | Recovery | Severity |
|-----------------|---------|----------|----------|
| **`SourceNotFound`** | `ShaderSource::open` cannot stat the project-relative path, or the path resolves outside the project source root (§4.1 inv 3). | refuse open; caller (cooker / editor) decides whether to retry after a filesystem rename or surface to the user. | `refuse` |
| **`SourceParseFailed`** *(SourceParseError)* | slangc rejects the Slang translation unit during preprocessing or parsing — syntax error, unresolved entry-point attribute, malformed `[shader(...)]` annotation (§4.1 inv 1). | refuse compile; capture stderr verbatim into the structured error envelope (§6.2) and surface to the editor; prior artifact (if any) remains live. | `refuse` |
| **`IncludeEscape`** *(IncludeResolutionFailed, escape variant)* | An `#include` resolves outside the project source root, or to an absolute path (§4.1 inv 3). | refuse open; the include graph is never partially admitted — `ShaderSource` construction fails atomically. | `refuse` |
| **`IncludeCycle`** *(IncludeResolutionFailed, cycle variant)* | The include graph contains a cycle; closure is non-finite (§4.1 inv 3). | refuse open; report the cycle path through the structured error detail. | `refuse` |
| **`EntryPointMissing`** | `compile` is invoked for an entry point name that the preprocessed `ShaderSource` does not expose. | refuse compile; surfaced to the cooker as a build-graph wiring bug, not a shader bug. | `refuse` |
| **`EntryPointStageAmbiguous`** | An entry point carries zero or more than one `[shader(...)]` attribute (§4.1 inv 1). | refuse open. | `refuse` |
| **`PermutationKeyMalformed`** | `PermutationKey::from_bytes` rejects bytes (e.g. enumerator out of declared range) (§4.2). | refuse decode; the cache entry that produced the bytes is quarantined and treated as `CacheCorrupt`. | `refuse` |
| **`PermutationKeyOutOfRange`** *(PermutationOutOfRange)* | A `PermutationIndex` exceeds the §4.2 cardinality product. | refuse compile; indicates a codegen-table drift between the build that emitted the index and the build that consumes it. | `fatal` |
| **`CompilerInvocationFailed`** *(SlangcInvocationFailed)* | The `glibre-shadercc` driver subprocess cannot be spawned (binary missing, sandbox profile rejected, executable bit missing). | refuse compile; the artifact is not written; in-flight cook is failed — see §8.4 refusal case 2. | `refuse` |
| **`CompilerExitNonZero`** *(SlangcOutputDiagnostic)* | The driver exited non-zero with a structured `shader::Error` JSON envelope on stderr (§6.2). slangc emitted a diagnostic the driver mapped to this arm. | refuse compile; diagnostic JSON is forwarded verbatim into the editor / cooker logs; prior CAS entry remains the live artifact for that `(PermutationKey, target)`. | `refuse` |
| **`CompilerTimedOut`** *(ShaderCompileTimeout)* | The driver subprocess exceeded the per-invocation wall-clock budget (§9 cook-time budget; pinned in the driver, not overridable from the plugin). | refuse compile; **no in-process retry**; the cooker may re-queue the job at its layer, which is outside this context. | `refuse` |
| **`UnsupportedTarget`** | The slangc backend does not yet support the requested `CompileTarget` (e.g. `DXIL` before post-MVP support lands). | refuse compile; capability mismatch surfaced through `IShaderBackend::capabilities()`. | `refuse` |
| **`MetalLibEmitFailed`** *(SlangcFailed)* | slangc emitted no `metallib`, or emitted a `metallib` whose container shape failed driver validation. | refuse compile; prior `metallib` artifact (if any) remains the live entry for this permutation. | `refuse` |
| **`ReflectionExtractionFailed`** *(ReflectionParseError)* | The `reflection/` slangc-reflection ingester (§6.3) cannot ingest the reflection record paired with the bytecode — malformed JSON, unknown binding kind, truncated bind-table (§4.4). | refuse publish (the artifact is *not* inserted into the cache); prior `ReflectionBlob` for the prior artifact remains live; surfaces as §8.4 refusal case 3. | `refuse` |
| **`DescriptorFrequencyAmbiguous`** *(DescriptorLayoutInvalid, ambiguous variant)* | A binding in the new `ReflectionBlob` carries no, or multiple, `DescriptorFrequencyGroup` annotations (§4.4 inv 3). | refuse publish; same path as `ReflectionExtractionFailed`. | `refuse` |
| **`DescriptorFrequencyMissing`** *(DescriptorLayoutInvalid, missing variant)* | A binding lacks any `DescriptorFrequencyGroup` resolution after the §4.5 `DescriptorLayout::derive` pass. | refuse publish. | `refuse` |
| **`LinkFailed`** | `IShaderBackend::link` rejected a spec-constant bake — entry-point set is incoherent, or specialization constants conflict across modules (§4.7 op `link`). | refuse compile of the linked module; per-module artifacts remain valid. | `refuse` |
| **`SpecializationConstantMissing`** | A spec-constant referenced by the entry-point set is not bound at link time. | refuse link. | `refuse` |
| **`CacheLookupMiss`** *(CacheMiss)* | `ShaderCache::lookup` finds no manifest entry for the requested `ShaderHash`. **This is the success case in disguise**: it is the only `Error` arm that callers are *expected* to handle non-fatally — the cooker's response is to enqueue a compile, the runtime's response is to refuse the bind (§4.6 inv 2 — runtime is read-only). | tooling: enqueue compile through `CompilationPipeline`. shipping: refuse bind; `render` falls back to its own missing-PSO policy (§render SPEC §8). | `fallback` |
| **`CacheCorrupt`** | A CAS file under the cache root fails its BLAKE3 self-check, or a manifest entry references a hash whose CAS file is absent (§4.6 inv 3). | refuse load of the corrupt entry; the cooker is required to re-cook from source — there is no in-place repair. Prior valid entries in the same manifest remain live. | `fatal` |
| **`CacheIntegrity`** | A `ShaderHash` collision against a non-byte-equal payload during cooker insert, or a manifest-vs-CAS skew detected by the integrity walk (§4.6 inv 3). | refuse insert; the in-flight artifact is dropped; prior hash continues to resolve — see §8.4 refusal case 4. | `fatal` |
| **`CacheReadOnlyViolation`** | The shipping `shader.dylib` observed a write attempt against the cache root (§4.6 inv 2; §4.8 inv 3). | refuse the write; abort the offending caller — this is a build-system bug, never a runtime user fault. | `fatal` |
| **`CapabilityNotSupported`** | A permutation requires a capability (mesh shaders, RT, work graphs, wave intrinsics, fp16) that the active backend does not advertise via `IShaderBackend::capabilities()` (§4.7 op `capabilities`). | refuse compile; the offline permutation enumerator (§6) is responsible for not asking — surfacing this at compile is a defense-in-depth check. | `refuse` |
| **`ShippingCompilationAttempted`** *(ShippingBuildCannotCompile)* | Any code path inside a `GLIBRE_SHIPPING == 1` build invokes the (link-excluded) `IShaderBackend::compile` — only reachable if a test harness or stray dev tool is mistakenly enabled in shipping (§4.3 inv 3, §4.8 inv 3, §6.5, §8.4 refusal case 1). | refuse with this enumerator and abort the calling thread; shipping has no slangc binary, no `glibre-shadercc` driver bundled — there is no recovery path that a runtime could take. | `fatal` |

#### 10.2.1 Spike-list enumerator added to §5

Spike #79 enumerated one failure mode that is **not** present in the
§5 declaration as of this revision:

- **`ArtifactSizeExceeded`** — the driver emitted a bytecode artifact
  (`metallib` MVP, `DXIL` post-MVP) whose serialized payload exceeds the
  per-artifact ceiling pinned in §9 (cook-time budget) and §7.5 (cooked
  `ShaderLibrary` layout). Trigger: cooker insert observes
  `payload.size() > artifact_size_ceiling` after the driver returns.
  Recovery: refuse insert; treat as `refuse` severity in the §10.1
  table; emit a structured diagnostic with the offending `ShaderHash`
  and the byte count so the asset author can split the shader.
  Severity: `refuse`.

This enumerator is added to the §5 enum in the follow-up plan that
amends §5 itself; §10 cannot mutate §5 by construction (this spike is
scoped to §10). The amendment is tracked under sub-epic #69. Until
that amendment lands, the cooker that detects an oversized artifact
**must** map the condition onto `CacheIntegrity` (closest-fit `fatal`
arm) so the closed-sum guarantee at the public boundary is never
violated; this temporary mapping is unit-tested and deleted when
`ArtifactSizeExceeded` lands in §5.

### 10.3 Shipping-build refusal blanket rule

Per §4.3 inv 3, §4.8 inv 3, §6.5, and §8.4 refusal case 1, the
shipping `shader.dylib` does not link slangc and does not bundle the
`glibre-shadercc` driver. The §5 public header `#if !GLIBRE_SHIPPING`-
guards `IShaderBackend::compile` itself, so a shipping caller cannot
reference the operation at compile time.

The blanket runtime rule is therefore:

> **Any slangc invocation attempted from a
> `GLIBRE_SHIPPING == 1` process — by any code path, including test
> harnesses mistakenly enabled in shipping — fails with
> `Error::ShippingCompilationAttempted`** (§5 alias of spike #79's
> `ShippingBuildCannotCompile`). There is no fallback, no retry, and
> no degraded mode; the calling thread is aborted and the structured
> log line carries the rejected request's `(SourceId, PermutationKey,
> CompileTarget)` so the build that produced the offending binary can
> be traced.

This rule is enforceable both at link time (the symbols are absent)
and at runtime (the surviving guard returns this enumerator before any
subprocess is spawned). The §10.1 severity is `fatal`; the cache state
is unchanged because no artifact was written; no observer-bus event is
emitted because `render` should never have requested the operation in
the first place.

### 10.4 Cross-references

- Closed enum source-of-truth: §5 (the §10 table tracks the §5
  declaration; both are amended together when a new arm lands).
- Hot-reload refusal projection: §8.4 (refusal cases 1–4).
- Subprocess error envelope: §6.2 (driver maps slangc exit codes onto
  this enum).
- Reflection refusal path: §6.3 (slangc native reflection ingestion).
- Cache integrity walk: §6.4, §4.6 invariant 3.
- Shipping-cut linkage: §6.5.
- Engine-wide error policy: `reviews/decisions/error-model.md`
  (per-context enums roll into `glibre::Error` variant; logging via
  `glibre::log_error`).

## 11. Acceptance Criteria

GitHub `type:user-story` issues this spec closes:

- #319 — Author and validate an Slang translation unit
  (Catch2: `shader_source_open_validates_entry_points`)
- #321 — Resolve Slang include closure with escape and cycle detection
  (Catch2: `shader_source_include_closure_rejects_escape_and_cycle`)
- #323 — Encode and enumerate the 4-axis `PermutationKey` deterministically
  (Catch2: `permutation_key_encoding_is_total_injective_and_bit_stable`)
- #325 — Compile Slang to DXIL via slangc subprocess (post-MVP)
  (Catch2: `compilation_pipeline_emits_dxil_via_subprocess`)
- #327 — [CLOSED — SPIR-V deferred until Vulkan]
- #330 — Compile Slang to MetalLib via slangc subprocess
  (Catch2: `compilation_pipeline_emits_metallib_via_subprocess`)
- #332 — Extract canonical `ReflectionBlob` from slangc native reflection
  (Catch2: `reflection_blob_extracts_canonical_metadata_from_slangc`)
- #334 — Derive `DescriptorLayout` partitioned by frequency group
  (Catch2: `descriptor_layout_derive_partitions_bindings_by_frequency`)
- #336 — Cook the resolved permutation set into a content-addressable
  `ShaderLibrary`
  (Catch2: `shader_cache_cook_walks_permutations_idempotently`)
- #339 — Resolve a cached `ShaderArtifact` by hash without invoking
  any compiler
  (Catch2: `shader_cache_get_resolves_artifact_without_compiling`)
- #341 — Hot-reload swap on Slang source edit via
  `ShaderArtifactReplaced` event
  (Catch2: `shader_hot_reload_publishes_artifact_replaced_for_affected_set_only`)
- #342 — Refuse shader compilation in shipping builds at link and
  runtime
  (Catch2: `shader_shipping_build_refuses_compilation_at_link_and_runtime`)

Each must have a Catch2 test by name.

## 12. Open Questions

None. All design decisions for the `shader` context — ubiquitous
language (§2), aggregate boundaries and invariants (§4), the public
`IShaderBackend` interface (§5), internal architecture (§6), on-disk
schemas (§7), the hot-reload contract (§8), the performance budget
(§9), the failure-mode taxonomy (§10), and the `type:user-story`
acceptance criteria (§11) — are resolved in this spec. Cross-context
decisions consumed here (engine error model, hot-reload protocol,
performance harness, plugin ABI) are owned by their respective
records under `reviews/decisions/` and referenced in-place above.

Any decision that surfaces during implementation and cannot be
absorbed by an existing section will be opened as a new
`type:spike` under sub-epic #70 (`Acceptance + Implementation Plan
— shader`); this section is then re-opened in the same PR that
files the spike. Until that happens, downstream contexts may
treat §1–§11 as load-bearing.
