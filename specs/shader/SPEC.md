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
