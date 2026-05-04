# Shader-Backend Detailed Design

> Detailed design for the `shader` context's `IShaderBackend` aggregate
> and its sole MVP implementation, the **Slang+Metal backend** (SPEC
> §4.7, §5 lines 825–849, §6.1 `backend/`, §6.2 driver flow). The
> aggregate's role at the public boundary is to define and implement the
> four-operation seam — `compile`, `reflect`, `link`, `capabilities` —
> through which the rest of the engine consumes one shader source
> language family. In MVP that family is Slang; the implementation
> wraps the offline `glibre-shadercc` driver (the `slangc` subprocess
> hop pinned in SPEC §6.2) for compile / reflect / link, and binds the
> resulting cached `ShaderArtifact` records into Metal 4
> `MTL::Library` / `MTL::Function` handles via metal-cpp so that
> `render`'s `PSOCache` (`specs/render/SPEC.md` §4.1.7) can build
> pipeline-state objects without ever touching slangc, Objective-C, or
> raw bytecode containers.
>
> Authority: this document refines `specs/shader/SPEC.md` §4.7, §4.8
> (cross-aggregate invariants), §5 (lines 815–849 + the `IShaderBackend`
> trait + `Capabilities` + `LinkedModule`), §6.1 `backend/`, §6.2
> (`glibre-shadercc` driver flow), §6.5 (shipping cut), §8.4 refusal
> cases, §9.3 (per-aggregate budget for `IShaderBackend`), and §10.2
> rows `CompilerInvocationFailed`, `CompilerExitNonZero`,
> `CompilerTimedOut`, `UnsupportedTarget`, `MetalLibEmitFailed`,
> `LinkFailed`, `SpecializationConstantMissing`, `CapabilityNotSupported`,
> `ShippingCompilationAttempted`. Anything not addressed here defers to
> those sections; anything that appears to contradict them is a defect
> in this document.

Refs: spike #757 — `[SPIKE] design-shader-shader-backend-detailed`.
Parent sub-epic #744 (`[SUB-EPIC] Detailed Designs — shader`). Sibling
task-breakdown spike blocked-by this deliverable.

---

## 1. Purpose

The shader-backend aggregate owns **the narrow plugin-trait seam
through which the engine talks to one shader source-language family**,
and the sole MVP implementation of that seam: a Slang backend that
spawns the `glibre-shadercc` driver subprocess (offline) and that
materializes the resulting cached `ShaderArtifact` records into
Metal 4 `MTL::Library` / `MTL::Function` handles via metal-cpp
(offline + dev / editor; the cooked artifact bytes themselves ride
into shipping through `ShaderCache::Library`).

The aggregate is two responsibilities held in deliberate tension by
SRP, joined only because they share a single seam:

1. **Trait surface.** Define the `IShaderBackend` v-table the engine
   compiles against — `compile`, `reflect`, `link`, `capabilities` —
   with shipping-build link exclusion of the `compile` virtual
   (SPEC §4.3 inv 3, §4.8 inv 3). The trait is the entire ABI the
   `shader` plugin exposes for the cooker, the editor adapter, and
   the `render`-side PSO builder.
2. **Slang+Metal implementation.** Provide one concrete v-table —
   `SlangMetalBackend` — that fans out to four sub-services:
   - `compile` → spawns `glibre-shadercc` (one slangc subprocess hop)
     to emit a `metallib` payload + paired native reflection JSON;
     forwards reflection bytes to the §4.4 ingester; embeds the
     resulting `ReflectionBlob` and the §4.5 `DescriptorLayout` into
     a sealed `ShaderArtifact`.
   - `reflect` → re-runs the §4.4 ingester against an
     already-cached artifact's reflection blob bytes (idempotent;
     caller is the cooker rebuilding a cache after a schema bump,
     not a per-frame path).
   - `link` → emits a single combined `LinkedModule` from a span of
     compatible `ShaderArtifact`s, baking specialization constants
     where Metal 4 admits it and otherwise refusing with
     `Error::SpecializationConstantMissing` /
     `Error::LinkFailed`.
   - `capabilities` → returns the static, offline-pinned capability
     descriptor for the active backend (mesh shaders, RT, work
     graphs, wave intrinsics, fp16). Pure function; no runtime
     query against the Metal device.
3. **Library materialization.** A separate, narrow service —
   `MetalLibraryLoader` — that takes a sealed `ShaderArtifact`
   (already in the cache, byte-validated by BLAKE3) plus a
   `metal_cpp::MTL::Device*` borrowed from `render`'s `MetalDevice`
   (`specs/render/SPEC.md` §4.1.6) and returns an opaque
   `MetalLibraryHandle` carrying:
   - one `MTL::Library*` constructed via
     `MTL::Device::newLibrary(dispatch_data, NS::Error**)` over the
     artifact's `bytecode` span, **never** via Slang source or MSL
     text — shipping has no slangc and no MSL;
   - one `MTL::Function*` per `EntryPoint` in the artifact, looked
     up by name through `MTL::Library::newFunction(NS::String*)`.

This third service is the load-bearing addition this design pins
beyond what SPEC §5 already declares: SPEC §5's `IShaderBackend`
returns `ShaderArtifact` value-objects whose `bytecode` is opaque to
the `shader` context; the **materialization seam** turns those bytes
into the GPU-driver-resident objects that `render`'s `PSOCache`
consumes when building `MTL::RenderPipelineState`. Without it,
`render` would have to either (a) call metal-cpp's library-create API
itself — pulling slangc-aware reflection knowledge into `render` — or
(b) re-read the cache on the render thread, duplicating the
shader-context BLAKE3 lookup. Both violate SRP. The materialization
seam belongs in `shader/backend/` because it pairs the artifact with
the same `ReflectionBlob` (entry-point names, stage tags) the
backend produced — the binding from "blob of bytes" to "callable
function handle" is a `shader`-context concern.

What `IShaderBackend` and the materialization seam **explicitly refuse
to own**:

- **PSO objects, render-graph passes, command encoders.** The
  `MTL::RenderPipelineState` / `MTL::ComputePipelineState` objects
  built from `(MetalLibraryHandle, render-state-hash)` belong to
  `render`'s `PSOCache` (`specs/render/SPEC.md` §4.1.7, sub-epic
  #766). The shader-backend hands `render` opaque library + function
  handles and stops at the v-table boundary.
- **The Metal device.** `MetalDevice` ownership and lifetime live in
  `render` (`specs/render/SPEC.md` §4.1.6, §4.8 invariant cited).
  The shader-backend takes a borrowed `MTL::Device*` per call and
  never retains it past the call. The shader plugin does not
  `dlopen` metal-cpp; metal-cpp linkage stays inside the
  `backend/slang/` translation unit, which is shipping-excluded for
  the slangc half and shipping-included only for the loader half.
- **Source ingestion.** `ShaderSource` (SPEC §4.1, sibling
  `shader-source-design.md`, spike #745) reads `.slang` files,
  expands `#include`, scans entry-point attributes. The backend
  consumes a `const ShaderSource&` only.
- **Permutation enumeration / encoding.** `PermutationKey`
  (SPEC §4.2, sibling `permutation-key-design.md`, spike #747)
  defines the 4-axis closed sum. The backend consumes a
  `const PermutationKey&` and never enumerates the cross-product.
- **Compilation pipeline orchestration.** `CompilationPipeline`
  (SPEC §4.3, sibling `compilation-pipeline-design.md`, spike #749)
  drives the `glibre-shadercc` driver invocation, builds the
  canonicalized argv, captures stdout/stderr, and translates exit
  codes onto `shader::Error`. The backend's `compile` *delegates* to
  the pipeline; it does not duplicate driver-spawning logic.
- **Reflection ingestion internals.** The §4.4 ingester
  (`reflection/slangc_reflection_ingester.cpp`, sibling spike #751)
  parses slangc native reflection JSON. The backend forwards the
  raw reflection bytes from the driver to the ingester and pairs
  the result with the bytecode in one `ShaderArtifact`.
- **Descriptor-layout derivation.** `DescriptorLayout::derive`
  (SPEC §4.5, sibling `descriptor-layout-design.md`, spike #753)
  projects a `ReflectionBlob` onto the four-frequency tables. The
  backend invokes `derive` once per artifact and embeds the result.
- **Cache lookup / insertion.** The `ShaderCache` (SPEC §4.6,
  sibling `shader-cache-design.md`, spike #755) owns put / get
  against the CAS. The backend's `compile` returns a fresh
  `ShaderArtifact`; the cooker (sole writer) is the call site that
  inserts.
- **Backend selection across source-language families.** Today
  there is one source-language family (Slang). A second family
  (e.g. a future raw-MSL backend, hypothetical only) would arrive
  as a second `IShaderBackend` implementation; the trait dispatcher
  would select between them. MVP has no dispatcher because there
  is one impl. The trait surface admits the second impl without
  changing the surface.
- **Capability negotiation policy.** The cooker pre-walks the
  enumerated permutation set against `capabilities()` to refuse
  unsupported permutations early (`Error::CapabilityNotSupported`,
  SPEC §10.2). That filtering policy is the cooker's; the backend
  only reports the static descriptor.
- **Runtime compilation in shipping.** The entire `compile()`
  virtual is `#if !GLIBRE_SHIPPING` in SPEC §5 (lines 836–840);
  the slangc subprocess driver is not bundled into shipping
  (SPEC §6.5). The materialization seam *survives* the shipping
  cut because shipping must still load `MTL::Library` from cached
  bytes; nothing in materialization references slangc.

The aggregate's SRP boundary is sharp: if the four-op trait surface
changes, this design changes; if the Slang+Metal impl's spawn /
ingest / pair / materialize chain changes, this design changes;
anything else is out of scope.

---

## 2. Requirements Coverage

Mapping of harmonius requirements to MVP refusal-or-coverage. Every
entry is independently re-derived (PHILOSOPHY §"How harmonius is
used"); harmonius is research input only. The cited harmonius file is
`docs/requirements/rendering/gpu-abstraction-layer.md` (R-2.1.x), with
cross-walks into `docs/design/rendering/render-pipeline.md` and
`docs/design/rendering/pipeline-state-cache.md`.

| Harmonius clause                                                                                   | Glibre disposition (MVP)                      | Coverage site                                                                                                                                  |
|----------------------------------------------------------------------------------------------------|-----------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------|
| `R-2.1.1` — top-level GPU device / command-buffer / PSO trait with associated types                | **Refused (lives in `render`)**               | The shader-backend is the **shader-source-language** trait, not the GPU-device trait. `MetalDevice` / `MetalQueue` / `MetalCommandBuffer` belong to `render` (`specs/render/SPEC.md` §4.1.6). |
| `R-2.1.4` — Metal backend implemented via objc2-metal (Rust prior art)                             | **Collapsed to metal-cpp; Slang seam owns library creation** | §3.5 below. Metal API access is **metal-cpp** (`MTL::Device::newLibrary` / `MTL::Library::newFunction`). No Obj-C++ in engine code (`CLAUDE.md`, PHILOSOPHY §"How harmonius is used"). The seam from cached bytecode to `MTL::Library` / `MTL::Function` lives in `shader/backend/slang/metal_library_loader.cpp`. |
| `R-2.1.16` — descriptor bindings in four frequency groups, mapped to backend-native mechanisms     | **Refused at this aggregate; covered by §4.5**| `DescriptorLayout` (SPEC §4.5) owns the four-frequency partition. The backend embeds a `DescriptorLayout` in every `ShaderArtifact` it returns; the engine-native mapping (Metal argument-buffer schema) is built by `render` from the embedded `RootSignatureSchema`. |
| `R-2.1.17` — compile HLSL → DXIL / SPIR-V / metallib via dxc + metal-shaderconverter subprocesses; **no runtime shader compilation** | **Re-derived: Slang → metallib via slangc; no runtime compile in shipping** | §3.2 and §3.6 below. One subprocess hop (`glibre-shadercc` → `slangc`); shipping link excludes the subprocess driver entirely (SPEC §4.3 inv 3, §4.8 inv 3, §6.5). |
| `R-2.1.18` — structured error types at every public boundary                                       | **Covered**                                   | §4.1 surface returns `std::expected<T, glibre::shader::Error>`; `reviews/decisions/error-model.md`; SPEC §10.2 enumerator-by-enumerator pinning. |
| `R-2.1.11` — feature emulation layer (mesh-shader / RT fallbacks)                                  | **Refused for MVP**                           | §3.3 below. `Capabilities` reports static caps; the cooker filters unsupported permutations against caps. No emulation path in the backend; the M1 baseline supports mesh shaders + RT natively (PHILOSOPHY "macOS 26 / Apple Silicon baseline"). |
| `R-2.1.10` — GPU work graphs (D3D12 native, emulated on Metal/Vulkan)                              | **Refused for MVP, capability flag reserved** | §3.3 below. `Capabilities::work_graphs == false` for MVP Slang+Metal backend. Post-MVP work-graph support arrives as a capability flip + slangc target flag, not a new trait.                                                                                  |
| `design/rendering/render-pipeline.md` § "Shader Compilation Pipeline" — single Slang front-end, no transpile chain | **Covered**                                   | §3.1 below. One `IShaderBackend` impl; one `glibre-shadercc` driver hop; `metallib` (MVP) and `DXIL` (post-MVP) emitted directly. |
| `design/rendering/pipeline-state-cache.md` R-2.3.9.2 — PSO key composes shader hash with device fingerprint | **Refused at this aggregate (lives in `render`)** | The `(shader_hash, state_hash)` PSO key is render's (`specs/render/SPEC.md` §4.1.7). The backend produces the *bytes* and the *handle*; PSO assembly composes them with render-state.                                                                          |
| `design/rendering/render-pipeline.md` § "Descriptor Layout Inference" — descriptor layout inferred once and cached | **Covered**                                   | The backend embeds a derived `DescriptorLayout` in every `ShaderArtifact`. Re-derivation at runtime is forbidden (SPEC §4.5 inv 1, §4.8 inv 4). |
| `design/rendering/render-pipeline.md` § RF-9 — re-run reflection on new bytecode after hot-reload  | **Covered (compile-side); reflection-side delegated** | §8 below. After a Slang source change the backend's `compile` re-runs the driver and pairs new bytecode with new reflection in one subprocess hop (SPEC §4.4 inv 1); the actual ingestion call lives in `reflection/` per §4.4. |
| `requirements/rendering/gpu-abstraction-layer.md` R-2.1.3 — PSO validated at creation              | **Refused at this aggregate (lives in `render`)** | PSO creation lives in `render`'s `PSOCache::insert` path, not the shader-backend. The backend's contribution is "produce a valid `MTL::Library` + `MTL::Function` set"; `render` validates the PSO-state composition.                                          |
| `design/rendering/pipeline-state-cache.md` R-2.3.9.8 — descriptor layout inferred from slangc reflection once | **Covered (offline, in cache)**               | `DescriptorLayout::derive(reflection)` runs once per `(PermutationKey, target)` inside `compile`; the result is embedded in the `ShaderArtifactRecord` (SPEC §7.2). Runtime is a table lookup.                                                                  |

Glibre-native requirements added beyond harmonius:

- **Materialization seam.** Harmonius treated "compiled bytecode →
  `MTL::Library`" as render's job; glibre re-derives that the seam
  belongs in `shader/backend/` because the `EntryPoint` names
  (the keys for `MTL::Library::newFunction`) are produced by the
  same Slang front-end that emitted the bytecode. Coupling the two
  in `render` would replicate `EntryPoint` knowledge across context
  boundaries; SRP forbids this. Re-derived in §3.5.
- **No Obj-C++ in engine code.** Harmonius was Rust-with-objc2-metal;
  glibre is C++23 with metal-cpp (PHILOSOPHY "macOS-first.
  Plugin-based. SDL3 + Metal 4 (via metal-cpp)"). The
  `MetalLibraryLoader` is pure C++23 over metal-cpp's
  `MTL::Device` / `MTL::Library` / `MTL::Function` headers. The
  bridging from `dispatch_data_t` to `MTL::Library` uses metal-cpp's
  `dispatch_data_create` wrapper (`Foundation/NSData.hpp` interop);
  no `.mm` files, no Objective-C runtime code in the `shader`
  plugin.
- **Capabilities are pure, offline, and pinned per backend instance.**
  The `Capabilities` struct (SPEC §5 lines 817–823) is constructed
  at backend-instance construction and never queried per-frame
  (SPEC §4.7 inv 1). Re-derivation rules: `Capabilities` is a
  property of the backend implementation + slangc target version,
  not of any specific Metal device. The cooker bakes capability
  decisions into the resolved permutation set offline; runtime never
  re-queries.
- **`std::expected<T, glibre::shader::Error>` at every fallible
  boundary.** Per `reviews/decisions/error-model.md`. The trait's
  four virtuals all return `std::expected`; the `MetalLibraryLoader`
  surface likewise. No exceptions cross the v-table; metal-cpp's
  `NS::Error*`-out-parameter pattern is the only exception-equivalent
  the loader translates into the closed enum (§10).
- **The compile virtual is link-stripped in shipping.** Per SPEC §5
  the entire `compile` virtual is `#if !GLIBRE_SHIPPING`-guarded; in
  shipping the v-table is three slots, not four. Plugin ABI hash
  recomputation triggered by this conditional is acceptable because
  shipping never loads a tooling plugin (PHILOSOPHY §3 plugin-ABI
  decision).

**Coverage attestation.** Every harmonius clause that lands in MVP
scope for the shader-backend aggregate is either covered above or
explicitly refused with a rationale rooted in glibre SPEC §3 (Occam
collapses), §4.7 (trait invariants), §4.8 (cross-aggregate
invariants), §6.5 (shipping cut), or `specs/render/SPEC.md` §4.1.6 /
§4.1.7 (render's MetalDevice and PSOCache). No clause is silently
dropped.

---

## 3. Detailed Model

### 3.1 Aggregate composition

```text
IShaderBackend (trait, abstract)
└── SlangMetalBackend (impl, single MVP instance)
    ├── compile/         — delegates to CompilationPipeline (#749)
    │     └── builds ShaderArtifact { bytecode, ReflectionBlob,
    │                                 DescriptorLayout }
    ├── reflect/         — delegates to slangc reflection ingester (#751)
    ├── link/            — combined-module link via slangc spec-const bake
    ├── capabilities/    — static descriptor (§3.3)
    └── MetalLibraryLoader (separate service, narrow surface)
          ├── load_library(MTL::Device*, ShaderArtifact) → MetalLibraryHandle
          └── lookup_function(MetalLibraryHandle, EntryPoint) → MTL::Function*
```

`IShaderBackend` is a v-table abstract base with four pure virtuals
(SPEC §5 lines 832–849). Concrete instances are constructed by the
plugin's `glibre_plugin_register` entry point and registered into the
engine's backend registry by name (`"slang-metal"` in MVP). The trait
is plugin-internal: no `extern "C"` boundary crosses the v-table — the
`shader` plugin's own translation units construct, register, and
invoke the trait. The seam lives in
`plugins/shader/include/glibre/shader/shader.hpp` (the §5 public
header) so the cooker, the editor adapter, and the `render`-side
loader-shim can include it.

`SlangMetalBackend` is the sole MVP impl. It holds **no mutable
state** between calls — `capabilities()` returns a const-ref to a
process-static struct populated at construction; `compile`, `reflect`,
and `link` are all pure functions of their inputs (modulo the
deterministic subprocess they spawn). The implementation pipeline
inside one `compile` call is:

```text
SlangMetalBackend::compile(source, key, target)
  └── 1. validate(target ∈ capabilities)               → UnsupportedTarget /
                                                          CapabilityNotSupported
      └── 2. CompilationPipeline::run(source, key, target)
                                                        // sibling spike #749:
                                                        //   spawn glibre-shadercc,
                                                        //   capture stdout (bytecode +
                                                        //                  reflection bytes)
                                                        //   capture stderr (Error JSON)
          └── 3. ReflectionBlob::ingest(reflection_bytes)     // sibling #751
              └── 4. DescriptorLayout::derive(reflection)     // sibling #753
                  └── 5. seal ShaderArtifact{ key, target,
                                              hash, bytecode,
                                              reflection,
                                              descriptor_layout }
```

`MetalLibraryLoader::load_library` is a separate concern, called by
the `render`-side loader shim that lives at
`plugins/shader/src/backend/slang/metal_library_loader.cpp` and is
linked into the shipping plugin (the slangc subprocess code is not).
It runs:

```text
MetalLibraryLoader::load_library(device, artifact)
  └── 1. validate(artifact.target == CompileTarget::MetalLib)
                                                        → UnsupportedTarget
      └── 2. dispatch_data := dispatch_data_create(
                                  artifact.bytecode.data(),
                                  artifact.bytecode.size(),
                                  nullptr,
                                  DISPATCH_DATA_DESTRUCTOR_DEFAULT)
          └── 3. NS::Error* err = nullptr
              └── 4. lib := device->newLibrary(dispatch_data, &err)
                                                        → MetalLibEmitFailed
                                                          (translates NS::Error)
                  └── 5. dispatch_release(dispatch_data)
                      └── 6. return MetalLibraryHandle{ lib, artifact.reflection }
```

`MetalLibraryLoader::lookup_function` is a pure call into
`MTL::Library::newFunction(NS::String*)` over an `EntryPoint::name`;
it wraps the metal-cpp call, retains the returned `MTL::Function*`
through metal-cpp's reference-counted handle, and reports
`Error::FunctionMissing` (a new `shader::Error` arm — see §10.5
amendment note) when the entry point is not present in the library.

### 3.2 The trait's four operations

| Op             | Signature (locked to SPEC §5)                                                | Hot/cold | Shipping linkage                                |
|----------------|------------------------------------------------------------------------------|----------|-------------------------------------------------|
| `compile`      | `(const ShaderSource&, const PermutationKey&, CompileTarget) → Result<ShaderArtifact>` | cold (offline only) | **excluded** (`#if !GLIBRE_SHIPPING`) |
| `reflect`      | `(const ShaderArtifact&) → Result<ReflectionBlob>`                            | cold     | included (cooker may re-ingest after schema bump) |
| `link`         | `(eastl::span<const ShaderArtifact>) → Result<LinkedModule>`                  | cold     | included (post-MVP path; in MVP returns the trivial single-module identity for spans of size 1, refuses with `LinkFailed` for spans of size ≥ 2 if specialization-constants conflict) |
| `capabilities` | `() → Capabilities` (`noexcept`, no `Result` — pure offline query)             | cold     | included (queried at backend construction)      |

The four ops compose only through the trait surface (SPEC §4.7 inv 2).
Cross-call state is forbidden: each call is a pure function of its
inputs, modulo the pinned-deterministic subprocess `compile` spawns.

`compile`'s deterministic-subprocess clause: per
`compilation-pipeline-design.md` (sibling #749) the
`glibre-shadercc` driver runs slangc with reproducibility flags
pinned in the driver (`-Zsb`-disabled timestamps, fixed `-O3`,
`-Qstrip_debug`); two `compile` calls with byte-equal inputs produce
byte-equal artifacts modulo the subprocess sandbox elision (SPEC §4.7
inv 3). `reflect` and `link` carry no subprocess at all; both are
pure C++ over the artifact's bytes.

### 3.3 `Capabilities` — static descriptor

The `Capabilities` struct (SPEC §5 lines 817–823) is constructed at
`SlangMetalBackend` construction and never mutates. Its five flags
in MVP:

| Flag                         | MVP value (Apple Silicon M1 baseline) | Justification                                                                                            |
|------------------------------|----------------------------------------|----------------------------------------------------------------------------------------------------------|
| `mesh_shaders`               | `true`                                 | M1 + Metal 4 supports mesh shaders natively (PHILOSOPHY "macOS 26 / Apple Silicon baseline"). slangc emits `[shader("mesh")]` → `MTL::FunctionType::Mesh` directly. |
| `ray_tracing`                | `true`                                 | M1 + Metal 4 supports hybrid RT pipelines. slangc emits `library` stage → `MTL::FunctionType::Intersection` etc. |
| `work_graphs`                | `false`                                | Metal 4 has no work-graph primitive in MVP scope. Reserved for post-MVP capability flip + slangc target flag. |
| `wave_intrinsics`            | `true`                                 | M1 SIMD-group intrinsics (`simd_*`) map to Slang's wave intrinsics; slangc emits the Metal-native form. |
| `fp16`                       | `true`                                 | M1 supports half-precision arithmetic; slangc emits `half` types directly to MSL.                       |

The struct is a value object (POD-like, trivially copyable, 5 bytes
of `bool`). The `capabilities()` virtual returns it by value (cheap).
Hot-path use is forbidden by SPEC §4.7 inv 1; the cooker queries it
once during the §6.4 walk to refuse capability-incompatible
permutations early with `Error::CapabilityNotSupported`.

A second backend instance (post-MVP, e.g. a Slang+DXIL backend for
Windows) would publish a different `Capabilities` value at its
construction time; `Capabilities` is a property of the
`(SourceLanguageFamily, CompileTarget, slangc-version)` triple, not
of the GPU device.

### 3.4 `compile` operation — driver delegation + sealing

`compile` is a thin orchestrator over four pure subroutines, each
owned by a sibling aggregate. The backend's responsibility is the
*sequencing* and the *sealing*: it never duplicates a sibling's
internals.

```text
compile(source, key, target):
    1. capability_check(target, key.required_features) → UnsupportedTarget /
                                                          CapabilityNotSupported
    2. raw := CompilationPipeline::run(source, key, target)   // #749
       // raw == { bytecode_bytes,
       //          reflection_bytes,           // slangc native JSON
       //          flags_hash,
       //          source_hash }               // for ShaderHash composition
    3. reflection := reflection::ingest(raw.reflection_bytes)  // #751
    4. layout := DescriptorLayout::derive(reflection)          // #753
    5. artifact_hash := ShaderHash::compose(
                            raw.source_hash,
                            key.to_bytes(),
                            raw.flags_hash,
                            target_byte(target))
                                                               // composer in #755
    6. seal ShaderArtifact{
           .key                  = key,
           .target               = target,
           .hash                 = artifact_hash,
           .bytecode             = move(raw.bytecode_bytes),
           .reflection           = move(reflection),
           .descriptor_layout    = move(layout),
       }
    7. return artifact   // caller (cooker) inserts into ShaderCache
```

Notes:

- **Step 1 is defense-in-depth.** The cooker pre-filters; the
  capability check inside `compile` catches the case where a test
  harness or editor reload synthesises a `(key, target)` pair the
  cooker didn't pre-validate. Refusing here keeps the trait surface
  total over its declared inputs.
- **Step 2's `raw` envelope is `CompilationPipeline`'s output type.**
  This design does not redeclare it; the sibling spike #749 owns the
  schema. The backend treats it as opaque: bytecode bytes, reflection
  bytes, two hashes.
- **Step 3 / step 4 are sibling-aggregate calls.** They run on the
  calling thread (the cooker's worker thread). They allocate against
  `ContextTag::shader` per `reviews/decisions/perf-budget.md`.
- **Step 5's hash composition is `ShaderCache`'s.** This design
  documents the inputs but the actual BLAKE3 composition lives in
  `cache/blake3.cpp` (§6.4 / §7.3 of SPEC). Two `compile` calls
  with byte-equal inputs produce byte-equal `artifact_hash` values
  — the determinism property §4.8 inv 2 demands.
- **Step 6's `move` uses are ownership transfers.** The bytecode
  vector is moved, never copied; the reflection blob is moved; the
  layout is moved. The sealed `ShaderArtifact` is the sole owner of
  these bytes.

### 3.5 Library materialization — `MetalLibraryLoader`

This is the load-bearing service that joins **already-cooked
artifact bytes** to **GPU-driver-resident objects**. Its surface is
two operations:

```cpp
namespace glibre::shader::backend::metal {

// Opaque RAII handle to one MTL::Library + cached entry-point lookups.
// The reflection blob is borrowed for the lifetime of the handle so
// callers can resolve EntryPoint -> MTL::Function without re-parsing.
class MetalLibraryHandle {
public:
    MetalLibraryHandle(MetalLibraryHandle&&) noexcept;
    MetalLibraryHandle& operator=(MetalLibraryHandle&&) noexcept;
    MetalLibraryHandle(const MetalLibraryHandle&)            = delete;
    MetalLibraryHandle& operator=(const MetalLibraryHandle&) = delete;
    ~MetalLibraryHandle();

    // Borrowed; lives as long as the handle.
    [[nodiscard]] MTL::Library*          library()    const noexcept;
    [[nodiscard]] const ReflectionBlob&  reflection() const noexcept;

private:
    friend class MetalLibraryLoader;
    MetalLibraryHandle(MTL::Library*, const ReflectionBlob*) noexcept;
    MTL::Library*          lib_{};        // metal-cpp ref-counted; release in ~
    const ReflectionBlob*  reflection_{}; // borrowed from the artifact
};

class MetalLibraryLoader {
public:
    // Construct a Metal MTL::Library from a sealed ShaderArtifact.
    // Refuses if artifact.target != CompileTarget::MetalLib.
    // Refuses with MetalLibraryCreateFailed if metal-cpp's
    // newLibrary returns null / NS::Error*.
    [[nodiscard]] static std::expected<MetalLibraryHandle, glibre::shader::Error>
    load_library(MTL::Device* device, const ShaderArtifact& artifact) noexcept;

    // Resolve one entry point -> MTL::Function. Reflection's
    // EntryPoint manifest is the source of truth for legitimate
    // names; lookup of a name not in the manifest is a programmer
    // error (Error::FunctionMissing).
    [[nodiscard]] static std::expected<MTL::Function*, glibre::shader::Error>
    lookup_function(const MetalLibraryHandle& handle,
                    const EntryPoint&         entry) noexcept;
};

}  // namespace glibre::shader::backend::metal
```

The implementation is short and metal-cpp-only:

```text
load_library(device, artifact):
    1. if artifact.target != CompileTarget::MetalLib: → UnsupportedTarget
    2. NS::AutoreleasePool pool;                       // metal-cpp idiom; scoped
    3. dd := dispatch_data_create(
                 artifact.bytecode.data(),
                 artifact.bytecode.size(),
                 nullptr,
                 DISPATCH_DATA_DESTRUCTOR_DEFAULT)     // metal-cpp wraps libdispatch
    4. NS::Error* err = nullptr
    5. lib := device->newLibrary(dd, &err)
    6. dispatch_release(dd)                            // dd retains its own ref to bytes
    7. if !lib:
           translate(err) → MetalLibraryCreateFailed
                            (capture err->localizedDescription() into ErrorContext::detail)
           return unexpected(MetalLibraryCreateFailed)
    8. return MetalLibraryHandle{ lib, &artifact.reflection }

lookup_function(handle, entry):
    1. NS::AutoreleasePool pool;
    2. name := NS::String::string(entry.name.c_str(), NS::UTF8StringEncoding)
    3. fn := handle.library()->newFunction(name)
    4. if !fn: → FunctionMissing (detail = entry.name)
    5. return fn   // caller takes ownership of the metal-cpp ref
```

Notes:

- **metal-cpp ownership rules.** `MTL::Device::newLibrary` returns a
  retained reference (the `new` prefix in metal-cpp follows the
  Cocoa naming rule — caller owns one ref). `MetalLibraryHandle`'s
  destructor calls `lib_->release()`. `lookup_function` returns a
  retained `MTL::Function*` that the caller (`render`) is
  responsible for releasing; `render`'s `PSOCache` holds these refs
  inside its `MTL::RenderPipelineDescriptor` /
  `MTL::ComputePipelineDescriptor` builds.
- **`NS::AutoreleasePool` scope.** metal-cpp's helper objects
  (`NS::String`, transient `NS::Error*`) are autoreleased; the
  scope ensures no leaks across the call boundary. The autorelease
  pool is the only Foundation primitive the loader touches; no
  Objective-C messaging outside metal-cpp's inline wrappers.
- **No source path is read.** The loader takes the artifact's
  `bytecode` as a span and never re-reads `.slang` files or any MSL
  text. This is what lets the loader survive the shipping cut: it
  has no slangc, no MSL parser, no source-language code-path.
- **`dispatch_data` retention.** `dispatch_data_create`'s
  `DISPATCH_DATA_DESTRUCTOR_DEFAULT` flag tells libdispatch to copy
  the input bytes into its own buffer; the artifact's `bytecode`
  vector may be moved or freed after step 6 returns. We pay the
  copy cost once at library creation; the resulting `MTL::Library`
  is then resident in driver memory and the source bytes can be
  released. This is intentional: the cache's `ShaderArtifact`
  hot-set is bounded (24 MiB in `ContextTag::shader`,
  `perf-budget.md`); driver-resident `MTL::Library` objects live
  under `ContextTag::render`'s 512 MiB ceiling.
- **No fallback path.** If `newLibrary` returns `null` we refuse
  with `MetalLibraryCreateFailed` and surface metal-cpp's
  `NS::Error*` text into `ErrorContext::detail`. There is no
  attempt to retry with different flags, no recompile-from-source
  fallback (shipping has no slangc, dev/editor surfaces the error
  in the editor diagnostics overlay).

### 3.6 `reflect` and `link` operations

Both operations are thin pure functions; both are linked into the
shipping plugin (unlike `compile`).

**`reflect(artifact)`** — re-runs the §4.4 reflection ingester
against an already-stored artifact's reflection bytes. The use case
is post-schema-bump rebuilding (`SPEC §7.4` rule 2): when the
`ReflectionRecord` schema gains a new field, the cooker re-walks
every artifact and calls `reflect` to materialise the in-memory
`ReflectionBlob` against the new schema. The operation never
re-spawns slangc and never re-parses `bytecode`. Delegates to the
sibling §4.4 ingester (spike #751); the backend just forwards the
artifact's reflection bytes.

**`link(span<const ShaderArtifact>)`** — spec-constant baking when
the target supports it (Slang+Metal: yes, via slangc's `-link`
mode). The MVP behaviour:

| Span size | Behaviour                                                                                                                         |
|-----------|-----------------------------------------------------------------------------------------------------------------------------------|
| 0         | Refuse with `LinkFailed` (degenerate input)                                                                                       |
| 1         | Trivial identity: copy the artifact's bytecode + reflection into a `LinkedModule`. No subprocess spawn.                            |
| ≥ 2       | Full link path: re-spawn `glibre-shadercc` in `--link` mode with the N artifact bytecode buffers as inputs; produce one combined `metallib` + reflection. Refuse with `Error::SpecializationConstantMissing` when an unbound spec-constant is referenced; refuse with `Error::LinkFailed` for general slangc-link errors. |

The link path is **shipping-included for size 0/1** (trivial copy)
and **shipping-excluded for size ≥ 2** (subprocess required). MVP
is dominated by single-module artifacts; size ≥ 2 is reserved for
future material-shader composition where the cooker links a base
permutation with a material-graph-emitted Slang module. The
shipping cut is enforced by the same `#if !GLIBRE_SHIPPING` guard
the `compile` virtual carries; the trivial-size paths live behind a
`size <= 1` branch that compiles unconditionally.

### 3.7 Backend registry and lifetime

The `shader` plugin registers exactly one `IShaderBackend`
implementation in MVP, named `"slang-metal"`, into a process-wide
backend registry owned by the plugin's `glibre_plugin_register`
callback. The registry is a small `eastl::vector_map<eastl::string,
eastl::unique_ptr<IShaderBackend>>` — no concurrent mutation past
plugin init.

The cooker, the editor adapter, and the `render`-side loader shim all
acquire the backend by name through a single accessor:

```cpp
namespace glibre::shader {
[[nodiscard]] std::expected<IShaderBackend*, Error>
get_backend(eastl::string_view name) noexcept;
}
```

Failure modes:

| Error                                  | Trigger                                                                                                |
|----------------------------------------|--------------------------------------------------------------------------------------------------------|
| `Error::CapabilityNotSupported`        | Backend named `name` not registered (defense-in-depth: no other arm fits an unknown source-language family). |

Lifetime: the backend instance is constructed in
`glibre_plugin_register` and destroyed in
`glibre_plugin_unregister`. The plugin-ABI decision
(`reviews/decisions/plugin-abi.md`) makes register / unregister the
sole lifecycle hooks; no per-frame construction. The
`SlangMetalBackend` instance itself is `~32 B` (the `Capabilities`
struct + a process-static config pointer); allocation happens once
per plugin load.

---

## 4. Public Surface

The §5 surface in `specs/shader/SPEC.md` is authoritative. This
section restates the subset owned by the shader-backend aggregate
with per-method behaviour annotations and pre-/post-conditions, and
adds the `MetalLibraryLoader` surface that this design introduces.

### 4.1 Trait surface (locked from SPEC §5)

```cpp
namespace glibre::shader {

class IShaderBackend {
public:
    virtual ~IShaderBackend() = default;

#if !GLIBRE_SHIPPING
    // Shipping-excluded: slangc subprocess. The plugin's
    // CompilationPipeline (#749) is the sole call site.
    virtual std::expected<ShaderArtifact, Error>
    compile(const ShaderSource&, const PermutationKey&, CompileTarget) = 0;
#endif

    // Re-ingest an already-cached artifact's reflection blob bytes
    // against the current schema. Idempotent. Linked in shipping.
    virtual std::expected<ReflectionBlob, Error>
    reflect(const ShaderArtifact&) = 0;

    // Combine N compatible artifacts into one LinkedModule (spec-const
    // bake when target supports it). Span-size ≤ 1 path is trivial
    // and shipping-included; span-size ≥ 2 path uses slangc and is
    // shipping-excluded.
    virtual std::expected<LinkedModule, Error>
    link(eastl::span<const ShaderArtifact>) = 0;

    // Static offline query. Pure; no Result wrapping (cannot fail
    // post-construction). Caller must not invoke per-frame
    // (§4.7 inv 1).
    virtual Capabilities capabilities() const noexcept = 0;
};

}  // namespace glibre::shader
```

Surface rules (re-derived against
`reviews/decisions/error-model.md`):

- **`std::expected<T, shader::Error>` at every fallible boundary.**
  `shader::Error` is the closed enum (SPEC §5 lines 494–521); it
  composes into engine-wide `glibre::Error` only at the cross-context
  seam, not inside the trait.
- **No exceptions cross the trait surface.** The aggregate compiles
  with `-fno-exceptions` (per error-model.md). metal-cpp internal
  paths that may throw are not used (metal-cpp's surface returns
  `NS::Error*` out-parameters; we translate them).
- **No `World&` / `Registry&` / logger references on the trait.**
  The trait is invoked from offline tooling (cooker) and from the
  shipping render path (via `MetalLibraryLoader`); neither wants
  ECS surface area.
- **`compile` is `#if !GLIBRE_SHIPPING`** at the v-table level. A
  shipping caller cannot reference the symbol at compile time. This
  is the load-bearing link-time enforcement of SPEC §4.3 inv 3 and
  §4.8 inv 3.

### 4.2 `MetalLibraryLoader` surface (this design's addition)

The loader is shipping-linked because shipping must materialise
cached `metallib` bytes into `MTL::Library` objects on every
PSO-cache miss. The header lives at
`plugins/shader/include/glibre/shader/backend/metal/library_loader.hpp`
and re-exports the §3.5 sketch verbatim. It is included by:

- the `shader` plugin's `backend/slang/metal_library_loader.cpp`
  (the implementation TU),
- `render`'s PSO-builder code (`specs/render/SPEC.md` §4.1.7) — the
  *only* `render`-side include of a `shader`-context private header,
  permitted because the loader is the seam between the two contexts.

The loader's surface ABI:

| Type / function                        | Aggregate | Role at the boundary                                  |
|----------------------------------------|-----------|-------------------------------------------------------|
| `MetalLibraryLoader::load_library`     | §4.7 (this) | Materialise cached bytes into `MTL::Library`.         |
| `MetalLibraryLoader::lookup_function`  | §4.7 (this) | Resolve `EntryPoint::name` to `MTL::Function*`.       |
| `MetalLibraryHandle` (RAII)            | §4.7 (this) | Owning wrapper around `MTL::Library*` + reflection ref. |

Borrow rules:

- `load_library` takes a borrowed `MTL::Device*` and a borrowed
  `const ShaderArtifact&`. The handle it returns retains its own
  `MTL::Library*` ref; the caller may release the artifact after
  the call (the bytes were copied into `dispatch_data` and then
  into the driver's library object).
- `lookup_function` returns a retained `MTL::Function*` the caller
  owns; metal-cpp's reference-counting rules apply.
  `MetalLibraryHandle` does **not** cache the returned functions —
  caching is the `PSOCache`'s concern.
- `MetalLibraryHandle` is move-only; the `MTL::Library*` is a single
  ref the destructor releases. Copying is forbidden because it
  would multiply the ref count without a clear ownership contract.

### 4.3 ABI surface

The `IShaderBackend` v-table is **plugin-internal**: the `shader`
plugin declares it and instantiates one impl. Cross-plugin calls
into the trait would violate `reviews/decisions/plugin-abi.md`
("each plugin links only `glibre-types.dylib`, not other plugins").
Cross-context callers (cooker in `tools`, render-side loader shim in
`render`) reach the trait through an in-process function pointer
exposed by the `shader` plugin's registration callback:

```cpp
// glibre-types middleman: a POD function-pointer table, byte-stable
// across plugin versions, exposed by the shader plugin to other
// plugins via the type registry. The cooker reads this table at
// register-time; render reads it once during init.
struct ShaderBackendVTable {
    std::expected<ShaderArtifact, Error> (*compile)(
        IShaderBackend*, const ShaderSource&, const PermutationKey&,
        CompileTarget) noexcept;        // shipping: nullptr
    std::expected<ReflectionBlob, Error> (*reflect)(
        IShaderBackend*, const ShaderArtifact&) noexcept;
    std::expected<LinkedModule, Error> (*link)(
        IShaderBackend*, eastl::span<const ShaderArtifact>) noexcept;
    Capabilities (*capabilities)(const IShaderBackend*) noexcept;
};
```

Per `reviews/decisions/plugin-abi.md`'s "Public plugin ABI surfaces
never expose `std::` containers or `eastl::` containers" rule, the
table crosses through `eastl::span` / POD value-types only. The
table is registered into `glibre-types.dylib` as a middleman type
so its layout is hash-stable across plugin reloads
(§3 collapse / `fory-codegen.md`).

The `MetalLibraryLoader` API does **not** cross the plugin ABI: it
is invoked by the `render`-side PSO builder which is itself in the
`render` plugin, and the call goes through an in-process function-
pointer table registered into `glibre-types.dylib` the same way as
the trait v-table above. metal-cpp types (`MTL::Device*`,
`MTL::Library*`, `MTL::Function*`) are opaque pointers across the
ABI; both contexts compile against the same metal-cpp headers
(`vcpkg manifest mode`, single SDK version pinned in
`vcpkg.json`).

---

## 5. Hot/Cold Path Split

Both halves of the aggregate are **cold paths**. The full split:

| Path | Trigger                                                        | Frequency                                    | Budget                                            |
|------|----------------------------------------------------------------|----------------------------------------------|---------------------------------------------------|
| Cold | `IShaderBackend::compile` from cooker                          | Once per `(source × key × target)` per cook   | <100 ms per artifact (subprocess + ingest + derive); cook-time, not frame-budget (§9.1)                          |
| Cold | `IShaderBackend::reflect` from cooker (post-schema-bump rebuild) | Once per cached artifact during one rebuild  | <0.5 ms per artifact (§9.1)                       |
| Cold | `IShaderBackend::link` from cooker (size 0/1 trivial)          | Once per artifact with linked module         | <0.1 ms (no subprocess)                           |
| Cold | `IShaderBackend::link` from cooker (size ≥ 2)                  | Once per linked artifact set                  | <50 ms per link (subprocess; cook-time)           |
| Cold | `IShaderBackend::capabilities` from cooker                     | Once at cooker startup                        | <0.001 ms (struct copy)                           |
| Cold | `IShaderBackend::capabilities` from `render` PSO-build init   | Once at render-init                           | <0.001 ms                                         |
| Cold | `MetalLibraryLoader::load_library` from `render`'s PSOCache    | Once per **cache miss** (level-stream / first-use) | <2 ms per library: dispatch_data build + metal-cpp newLibrary; charged against `render`'s CPU-submit cell, not `shader`'s 0 ms cell (§9.3)        |
| Cold | `MetalLibraryLoader::lookup_function` from `render`'s PSO build | Once per entry point per PSO build            | <0.05 ms per function (string interning + metal-cpp newFunction); same charging as above                                                            |
| Hot  | Runtime steady-state                                           | n/a                                          | **0 ms** — no `IShaderBackend` virtual fires per frame; PSOCache hits absorb the load (`specs/render/SPEC.md` §4.1.7) |

The `IShaderBackend` virtuals are categorically cold: `compile`,
`reflect`, `link` are cooker-only; `capabilities` is queried once.
The `MetalLibraryLoader` is also cold from `render`'s vantage —
PSOCache misses are level-stream / first-use events, not per-frame
(`specs/render/SPEC.md` §4.1.7 inv 4 hot-reload-safe + LRU eviction
budget).

The cold/hot split is enforced by the build-system gating in SPEC
§6.1 and §6.5: `backend/slang/` is shipping-excluded for the slangc
subprocess code (the entire `compile` virtual + the size ≥ 2 link
path) and shipping-included for the `MetalLibraryLoader` (which has
no slangc dependency). PHILOSOPHY §6 ("zero runtime reflection in
shipping") is preserved: the shipping `reflect` virtual reads stored
reflection bytes; it never runs slangc.

The implication for SPEC §9.1's `shader = 0.00 ms / frame` cell is
that the shader-backend contributes **zero** in shipping by absence
of frame-aligned calls. `MetalLibraryLoader::load_library` is non-
zero, but it is charged against `render`'s 1.40 ms CPU-submit cell
on cache miss (the same charging the §9.3 row "(External) PSO
queries" pins), not against `shader`'s 0 ms cell.

---

## 6. Concurrency

The shader-backend aggregate has three concurrency profiles, one
per surface.

### 6.1 `IShaderBackend` virtuals are pure functions

`compile`, `reflect`, `link`, and `capabilities` are pure on their
inputs (modulo subprocess side-effects pinned by SPEC §4.7 inv 3).
They hold no shared mutable state. Two consequences:

1. **Cross-call concurrency.** The cooker may invoke `compile` on N
   `(source, key, target)` triples in parallel from N worker threads
   (cooker thread pool, bounded by build configuration). No locks,
   no mutex, no shared scratch. Per-call allocator state under
   `ContextTag::shader` is per-thread (transient arenas drain at
   call return, `reviews/decisions/perf-budget.md` Allocator Rules
   §4).
2. **Subprocess sandbox per call.** Each `compile` spawns its own
   `glibre-shadercc` subprocess (sibling spike #749 owns the spawn
   arithmetic). Subprocesses do not share state; the kernel
   guarantees process isolation.

The cooker's parallelism is bounded by `min(core_count, 16)` in
MVP; the shader-backend has no opinion about the bound — it admits
any concurrency the cooker chooses.

### 6.2 `MetalLibraryLoader::load_library` thread-safety

`MTL::Device::newLibrary(dispatch_data, NS::Error**)` is documented
as **thread-safe** by Apple ("Each `MTLDevice` instance is
thread-safe"; Metal Programming Guide). Specifically:

- Multiple threads may invoke `newLibrary` concurrently against the
  same `MTL::Device*` without external synchronization.
- The returned `MTL::Library*` is itself thread-safe for read-only
  operations including `newFunction`.
- metal-cpp's wrappers do not add their own locks; thread-safety is
  inherited verbatim from the underlying Metal API.

The `MetalLibraryLoader` therefore needs no internal mutex. Concrete
threading rules:

1. **Multiple loaders may run concurrently against one device.**
   `render`'s PSO build threads (today: a single render thread; in
   MVP no parallel PSO build) may call `load_library` from
   different threads without external locks.
2. **`MetalLibraryHandle` is move-only and not shared.** Two
   threads cannot share a handle without a lock; the handle's
   destructor runs `lib_->release()` which is idempotent only with
   a single owner.
3. **`lookup_function` is thread-safe per Metal docs.** Two threads
   may call `lookup_function` against the same `MetalLibraryHandle`
   concurrently; `MTL::Library::newFunction` is documented as
   thread-safe.
4. **`dispatch_data_create` is thread-safe.** libdispatch's data
   constructor is well-defined under concurrent invocation.
5. **`NS::AutoreleasePool` is thread-local.** Each call constructs
   its own scoped pool; pools never leak across threads.

In MVP `render` runs PSO builds on a single render thread, so the
above is a defense-in-depth contract; post-MVP parallel PSO build
plans (post-MVP) inherit the contract without modification.

### 6.3 `Capabilities` and the registry

The backend registry (§3.7) is mutated **only** during plugin
register / unregister, which run on the loader thread during phase
8 (`reviews/decisions/frame-phases.md`). Past plugin init, the
registry is read-only; concurrent reads from any thread are
race-free. The `Capabilities` struct is process-static and POD;
reads from any thread are race-free.

### 6.4 Shipping vs tooling concurrency

Shipping has no `compile` / `link-size-≥2` paths; the only
shader-backend code that runs in shipping is the `MetalLibraryLoader`
+ `reflect` + `link-size-≤1` paths. All four are pure functions over
already-cached bytes; concurrency is whatever `render`'s PSO builder
chooses. Tooling (cooker / editor) gets the full set; concurrency is
the cooker's thread pool.

---

## 7. Persistence + ABI

### 7.1 The shader-backend persists nothing

The aggregate's outputs are either consumed in-process
(`Capabilities`, `MetalLibraryHandle`, `MTL::Function*`) or handed
off to a sibling aggregate that does the persisting:

- `compile` returns a `ShaderArtifact` value the cooker hands to
  `ShaderCache::insert` (SPEC §4.6, sibling #755). The cache owns
  the on-disk schema (`data/schemas/shader/ShaderArtifactRecord.fory`,
  SPEC §7.2).
- `reflect` returns an in-memory `ReflectionBlob`; persistence is
  the cooker's via the same artifact-record path.
- `link` returns an in-memory `LinkedModule`; the linked-module
  variant of the cache is post-MVP and routed through the same
  cache aggregate.
- `MetalLibraryHandle` is **never persisted**: `MTL::Library*`
  lives in GPU driver memory, is process-local, and is rebuilt from
  cached bytes at every process start. There is no on-disk shape
  for an `MTL::Library` object outside the driver's own pipeline-
  cache (`MTLBinaryArchive`, post-MVP — owned by `render`, not
  shader).

### 7.2 The trait v-table is the cross-aggregate ABI

Two consumers reach the trait:

1. **Cooker** (`tools/cook/`, eventually `tools` plugin) — reads the
   `ShaderBackendVTable` from `glibre-types.dylib`'s middleman
   type registry, invokes `compile` / `reflect` / `link` /
   `capabilities` to produce the artifacts that go into
   `ShaderCache::insert`.
2. **`render` plugin** — reads the same v-table once during
   `glibre_plugin_register` to discover capability flags (used to
   refuse incompatible PSO requests early), and reads
   `MetalLibraryLoader`'s function-pointer table (registered into
   `glibre-types.dylib` the same way) on every PSOCache miss to
   materialise libraries.

Both consumers receive the v-table by value from
`glibre-types.dylib`'s registry; the table layout is hash-stable
across plugin reloads (§3 collapse, `fory-codegen.md`).

### 7.3 `MetalLibraryHandle` lifetime versus reload

`MetalLibraryHandle` instances are owned by `render`'s PSOCache
(`specs/render/SPEC.md` §4.1.7). On a `shader`-driven hot-reload
(SPEC §8) the new artifact bytes produce a new `ShaderHash`; the
`PSOCache` evicts the old PSO entries and triggers a new
`load_library` call against the new bytes. The old
`MetalLibraryHandle` is destroyed when its last `MTL::Function*`
referrer (a PSO state object) retires. There is no
`MetalLibraryHandle` survival across reload by design; both halves
(library bytes, function handles) are derivative of the artifact
bytes that just changed.

### 7.4 Plugin-ABI surface

The `IShaderBackend` trait is **plugin-internal** and crosses the
ABI as a POD function-pointer table per `plugin-abi.md`. The
function-pointer table layout:

```cpp
namespace glibre::types::shader {

// Middleman ABI: stable across plugin reloads. Schema-versioned
// per fory-codegen.md (declarations live in
// data/schemas/shader/BackendVTable.fory; this is the C++
// projection).
struct BackendVTable {
    // 4-byte tag identifying the source-language family.
    std::uint32_t backend_tag;          // 0x'slng' for "slang-metal" MVP

    // shipping: compile_fn = nullptr (link-time absence)
    std::expected<glibre::shader::ShaderArtifact, glibre::shader::Error>
        (*compile_fn)(
            void* impl,
            const glibre::shader::ShaderSource&,
            const glibre::shader::PermutationKey&,
            glibre::shader::CompileTarget) noexcept;

    std::expected<glibre::shader::ReflectionBlob, glibre::shader::Error>
        (*reflect_fn)(
            void* impl,
            const glibre::shader::ShaderArtifact&) noexcept;

    std::expected<glibre::shader::LinkedModule, glibre::shader::Error>
        (*link_fn)(
            void* impl,
            eastl::span<const glibre::shader::ShaderArtifact>) noexcept;

    glibre::shader::Capabilities (*capabilities_fn)(
            const void* impl) noexcept;

    void* impl;     // opaque handle to the SlangMetalBackend instance
};

}  // namespace glibre::types::shader
```

The `MetalLibraryLoader` exposes a similar table:

```cpp
namespace glibre::types::shader {

struct MetalLibraryLoaderVTable {
    std::uint32_t loader_tag;           // 0x'mtll'

    std::expected<glibre::shader::backend::metal::MetalLibraryHandle,
                  glibre::shader::Error>
        (*load_library_fn)(
            void*                                       device_opaque,
            const glibre::shader::ShaderArtifact&) noexcept;

    // MTL::Function* opaqued to a void* across the ABI; render
    // re-types it on its side via the same metal-cpp headers.
    std::expected<void*, glibre::shader::Error>
        (*lookup_function_fn)(
            const glibre::shader::backend::metal::MetalLibraryHandle&,
            const glibre::shader::EntryPoint&) noexcept;
};

}  // namespace glibre::types::shader
```

Both tables are registered into `glibre-types.dylib` by the
`shader` plugin's `glibre_plugin_register`. Hash-bumping rules: a
new entry in the table = `ShaderBackendRecord` schema bump =
middleman ABI hash bump = plugin reload required (`plugin-abi.md`
versioning rules). MVP locks both tables at the shape above.

---

## 8. Hot-Reload Integration

The shader-backend has two hot-reload-relevant surfaces; each follows
a different protocol.

### 8.1 Source-driven reload (the SPEC §8 path)

Per SPEC §8, a Slang source change re-runs `compile` for the
affected `(source, key, target)` set. The shader-backend's role is
to accept the re-invocation deterministically: each re-invocation is
an independent pure function call, and two re-invocations against
byte-equal `ShaderSource` inputs produce byte-equal `ShaderArtifact`
outputs (§4.7 inv 3). The backend itself has nothing to "reload" —
it is stateless per call.

The `MetalLibraryLoader` sees the consequence of the source change
through `render`'s PSOCache invalidation:

1. `shader` cooker's hot-reload tick re-runs `compile` and inserts
   new `ShaderArtifact` records under new `ShaderHash` values.
2. `shader` publishes `ShaderArtifactReplaced` (SPEC §8.5) on the
   observer bus.
3. `render`'s PSOCache subscriber (§specs/render/SPEC.md §4.1.7 inv
   4 hot-reload-safe) marks the affected PSO entries invalid,
   destroys their `MetalLibraryHandle`s (which calls
   `MTL::Library::release()`), and triggers fresh `load_library`
   calls against the new artifact bytes on the next PSO request.
4. The old `MTL::Library*` lingers in driver memory until the last
   in-flight frame referencing it retires — Metal's reference
   counting + the GPU's command-buffer lifetime guarantee no use-
   after-release. PSOCache's "Hot-reload-safe" invariant
   (`specs/render/SPEC.md` §4.1.7 inv 4) pins this contract.

The shader-backend's own contribution to the protocol is:

- **`compile` is idempotent on byte-equal inputs.** A reload tick
  that fires twice against the same source bytes produces the same
  `ShaderArtifact` twice; the cache's idempotent `insert` (SPEC
  §4.6 inv 1) drops the second.
- **`MetalLibraryLoader::load_library` is idempotent on byte-equal
  inputs.** The same artifact bytes produce a structurally
  equivalent `MTL::Library` (different `MTL::Library*` pointer
  values; same library shape, same exported function set). The
  caller (`render` PSOCache) never holds two handles to the same
  artifact concurrently — eviction is by `ShaderHash` change.

### 8.2 Plugin-dylib reload (the engine-wide path)

When the `shader` plugin's own `.dylib` reloads (per
`reviews/decisions/hot-reload-protocol.md` — e.g., the slangc argv
builder changed, the reflection ingester changed, or the
`MetalLibraryLoader` itself changed), the engine-wide loader
protocol applies:

1. **Step 1 — Drain.** `glibre_plugin_drain` is the only callback
   the shader plugin owns past register. It releases any cached
   `Capabilities` query result and flushes the backend registry's
   `unique_ptr<IShaderBackend>` to `nullptr`. Note: the
   `MetalLibraryHandle`s in `render`'s PSOCache are **not** owned
   by the shader plugin — they are owned by `render`. The drain
   does not invalidate them.
2. **Step 2 — Swap.** The new `shader.dylib`'s vtable replaces the
   old. The new `IShaderBackend` instance is constructed during
   the new plugin's `register`.
3. **Step 3 — Migrate.** Schema-driven only; SPEC §8.3 says no
   migrate function fires for a source-only edit. A backend-code
   reload likewise does not migrate any cached artifact bytes
   (those are content-keyed and survive bit-equal).
4. **Step 4 — Resume.** The new plugin re-registers its
   `ShaderBackendVTable` and `MetalLibraryLoaderVTable` into
   `glibre-types.dylib`; subscribers (the cooker, `render`) read
   the new tables on their next call.

Survival across the dylib swap:

| State                                           | Owner        | Survival                                                                         |
|-------------------------------------------------|--------------|----------------------------------------------------------------------------------|
| `IShaderBackend` instance                       | shader plugin | Re-constructed by new plugin's `register`. No cross-swap state.                  |
| `Capabilities` value                            | static const | Re-published by new plugin's `register`. May change values across versions; cooker re-queries on next walk. |
| Backend v-table function pointers               | middleman     | Re-registered by new plugin's `register`. Hash-checked by loader (§7.4).         |
| `MetalLibraryHandle` instances in `render` PSOCache | render        | Survive bit-equal — they hold direct `MTL::Library*` refs, not function pointers into the shader plugin. The shader-plugin code that built them is gone, but the runtime behaviour of `MTL::Library*::newFunction` is owned by metal-cpp + the Metal driver, which survive the swap. |
| `ShaderCache` records on disk                   | shader plugin (write side) / shipping plugin (read side) | Bit-equal across swap (content-addressed). |

The third row is load-bearing: it is the property that lets a
`shader.dylib` reload happen *without* requiring `render` to evict
every PSO. The `MTL::Library` and `MTL::Function` objects in the
PSOCache are GPU-driver-resident entities whose lifetime is owned
by metal-cpp's reference count, not by the `shader` plugin's TU.
They survive the dylib swap.

### 8.3 Refusal cases on reload

A backend-driven reload may refuse for two new reasons beyond the
SPEC §8.4 refusal cases (which are source-driven):

1. **`Error::CapabilityNotSupported` mid-cook.** The new plugin's
   `Capabilities` differ from the old (e.g., `mesh_shaders` flipped
   to `false`); a cook in progress hits a permutation that the new
   capabilities don't admit. Resolution: the cooker re-queries
   capabilities at re-registration and re-walks the resolved
   permutation set, refusing newly-incompatible permutations with
   `Error::CapabilityNotSupported` (SPEC §10.2). The PR's pre-cooked
   archive is the live artifact source until a fresh cook completes.
2. **`Error::MetalLibraryCreateFailed` on first post-reload load.**
   metal-cpp / Metal driver behaviour changed across versions and
   `newLibrary` rejects bytes that the prior library accepted. The
   reload itself succeeds; the next PSO build attempt against the
   affected library fails. `render`'s PSOCache surfaces the failure
   (`render::Error::PipelineCompileFailed` wrapping the inner
   `shader::Error`); the prior PSO continues to bind. Resolution
   path is to re-cook against the new metal-cpp version.

Neither refusal terminates the engine; the loader logs at `warn`
level per `error-model.md`.

---

## 9. Performance

### 9.1 Per-call wall-time budgets (cold)

All shader-backend calls are cold-path. Targeted on the M1 baseline.

| Op                                          | Budget (cold)  | Notes                                                                 |
|---------------------------------------------|----------------|-----------------------------------------------------------------------|
| `IShaderBackend::compile` (one artifact)    | <100 ms / artifact  | Dominated by `glibre-shadercc` subprocess (sibling #749 budget); ingest + derive add <2 ms each. Cook-time, not frame-budget. |
| `IShaderBackend::reflect` (re-ingest)       | <0.5 ms / artifact  | Pure JSON ingest of ~32 KiB of reflection bytes; no subprocess.       |
| `IShaderBackend::link` size 0/1             | <0.1 ms             | Trivial copy / refuse.                                                |
| `IShaderBackend::link` size ≥ 2             | <50 ms / link       | slangc subprocess in `--link` mode; cook-time.                        |
| `IShaderBackend::capabilities`              | <0.001 ms           | Struct copy of 5 booleans + tag.                                      |
| `MetalLibraryLoader::load_library`          | <2 ms / library     | `dispatch_data_create` (memcpy of ≤256 KiB metallib) + `MTL::Device::newLibrary` (driver-side library decode). |
| `MetalLibraryLoader::lookup_function`       | <0.05 ms / function | metal-cpp string-from-utf8 + `MTL::Library::newFunction` (driver hash lookup). |

The `compile` budget per artifact (100 ms) is the cooker-side
budget; a project with 1k artifacts cooked single-threaded would
take 100 s. Cooker parallelism (§6.1) reduces wall-clock to ~12 s on
M1 firestorm. Per
`reviews/decisions/perf-budget.md` cook-time budgets are **out of
frame-budget scope**; no per-frame budget cell is consumed by
`compile`.

The `MetalLibraryLoader::load_library` 2 ms budget is the load-
bearing constraint for `render`'s PSOCache cold-build path. Per
`specs/render/SPEC.md` §9 the render cell allocates 1.40 ms of
CPU-submit work per frame; a single PSO miss must complete the
library load + function resolution + PSO build in well under 1.40 ms.
The 2 ms budget here is the upper bound on the shader-backend's
contribution; a typical metallib (≤32 KiB MVP) loads in <0.5 ms.

### 9.2 Memory budget

Per-instance and per-call memory bounds.

| Surface                                  | Bound (MVP)                  |
|------------------------------------------|------------------------------|
| `SlangMetalBackend` instance              | ~32 B (Capabilities + config ptr) |
| `Capabilities`                            | 8 B (5 bools + 3 padding)    |
| Per-`compile` transient arena             | ~512 KiB (subprocess scratch + ingest scratch); drains at call return |
| Per-`compile` returned `ShaderArtifact`   | ~256 KiB (bytecode ≤256 KiB + reflection ≤32 KiB + descriptor layout ≤4 KiB) |
| `MetalLibraryHandle`                      | ~16 B (one ptr + one ptr) — the `MTL::Library*` itself is driver-side, accounted under `ContextTag::render` |
| Per-`load_library` transient arena        | ~256 KiB (dispatch_data copy buffer); released after `newLibrary` retains its own copy |

Cook-time concurrency: with N parallel `compile` invocations, peak
heap is `N × (512 KiB + 256 KiB) ≈ N × 768 KiB`. For N = 16 (cooker
default), peak is ~12 MiB — well inside the 32 MiB
`ContextTag::shader` ceiling (`reviews/decisions/perf-budget.md`).

`MetalLibraryHandle` instances are charged against `ContextTag::render`
(the holder), not `ContextTag::shader` (the producer). The 16 B
holder cost is trivial; the driver-resident `MTL::Library` bytes
(typically ≤256 KiB per library) live in the Metal driver's address
space and are accounted under `render`'s 512 MiB GPU heap ceiling
per `perf-budget.md` Allocator Rule §5.

### 9.3 Allocator integration

Both halves of the aggregate allocate against `ContextTag::shader`
when they live in the `shader` plugin. The exception:
`MetalLibraryHandle::library_` is a `MTL::Library*` whose underlying
bytes live in driver memory under `ContextTag::render`'s tag. The
shader plugin allocates only the holder pointer + the reflection
back-pointer (16 B); the holder's destructor calls
`lib_->release()` which returns the driver-side memory to Metal's
heap.

Per-call transient arenas drain at call return per `perf-budget.md`
Allocator Rule §4. The cooker's drain point is "after
`ShaderCache::insert` returns" (sibling design #755); the
`MetalLibraryLoader`'s drain point is "after `load_library` /
`lookup_function` returns to the caller". No allocation crosses
`compile` / `load_library` boundaries beyond the explicit owned
return values.

### 9.4 Init / cold-start contribution

In shipping, the shader-backend contributes **zero** to engine init:

- The `IShaderBackend` instance is constructed by the plugin's
  `glibre_plugin_register` callback during phase 8 of the first
  frame (`reviews/decisions/frame-phases.md`); construction is
  ~32 B + 8 B static-data publishing (sub-microsecond).
- `MetalLibraryLoader` has no state; "init" is just header
  inclusion.
- The first `MetalLibraryLoader::load_library` call happens on the
  first PSOCache miss, which is during `render`'s warmer step in
  the first level-stream — not in the shader plugin's init path.

In editor / dev builds, the cooker's first-walk time is bounded by
§9.1's `compile` budget × project artifact count, parallelised. This
is the editor's startup-cook cost, not a per-frame cost.

---

## 10. Failure Modes

The SPEC §10.2 enumeration is authoritative. This section pins the
arms attributable to the shader-backend aggregate, their per-arm
operator actions, and one new arm this design proposes (§10.5
amendment note).

### 10.1 Backend-emitted `shader::Error` arms

| Arm                              | Operation        | Trigger                                                                                                                                                | Operator action                                                                                                                                              | Severity (SPEC §10.1) |
|----------------------------------|------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------|-----------------------|
| `CompilerInvocationFailed`       | `compile`        | `glibre-shadercc` driver could not be spawned (binary missing, sandbox profile rejected, executable bit missing).                                       | Verify the driver binary's presence under `tools/shadercc/`; verify the `sandbox-exec` profile; rerun.                                                       | `refuse`              |
| `CompilerExitNonZero`            | `compile`        | The driver exited non-zero with structured `shader::Error` JSON on stderr (sibling #749 driver envelope).                                              | Read the captured stderr (Slang diagnostic); fix the source.                                                                                                  | `refuse`              |
| `CompilerTimedOut`               | `compile`        | Driver subprocess exceeded the per-invocation wall-clock budget (cook-time budget pinned in driver).                                                   | Re-queue the job at the cooker's layer; do not retry inside `compile` (`§10.1` no silent retry).                                                              | `refuse`              |
| `UnsupportedTarget`              | `compile`, `load_library` | The slangc backend does not support the requested `CompileTarget` (e.g. `DXIL` before post-MVP), or `MetalLibraryLoader::load_library` was called with a non-`MetalLib` artifact. | Adjust the target selection in the cooker / loader call site; capability-table mismatch is a build-time wiring bug.                                          | `refuse`              |
| `MetalLibEmitFailed`             | `compile`        | slangc emitted no metallib, or emitted one whose container shape failed driver validation.                                                            | Re-author the source; the prior metallib (if any) remains the live entry.                                                                                    | `refuse`              |
| `LinkFailed`                     | `link`           | slangc link rejected a spec-constant bake; entry-point set incoherent; specialization conflict across modules.                                        | Inspect the link argv (sibling #749) and the constituent modules' reflection records; spec-const declarations must agree across linked modules.              | `refuse`              |
| `SpecializationConstantMissing`  | `link`           | A spec-constant referenced by the entry-point set is not bound at link time.                                                                          | Bind the spec-constant in the calling cooker code; the missing name is in the error detail.                                                                 | `refuse`              |
| `CapabilityNotSupported`         | `compile`, `get_backend` | `(key, target)` requires a capability the backend does not advertise; or `get_backend` was called with an unknown name.                              | The cooker's permutation enumeration table is out of sync with `Capabilities`; re-walk after the next backend-capabilities query.                            | `refuse`              |
| `ShippingCompilationAttempted`   | `compile` (link-stripped) | A shipping-process code path tried to dispatch through the (excluded) `compile` virtual. Only reachable if a test harness was mistakenly enabled in shipping (SPEC §6.5, §10.3). | Rebuild the shipping binary without the offending test harness; this is a build-system bug.                                                                  | `fatal`               |
| **`MetalLibraryCreateFailed`** *(this design's amendment)* | `MetalLibraryLoader::load_library` | `MTL::Device::newLibrary(dispatch_data, NS::Error**)` returned `null`; metal-cpp's `NS::Error*` text is captured into `ErrorContext::detail`. | Re-cook against the current metal-cpp + slangc versions; re-build the shipping binary if metal-cpp was bumped.                                                | `refuse`              |
| **`FunctionMissing`** *(this design's amendment)* | `MetalLibraryLoader::lookup_function` | `MTL::Library::newFunction` returned `null` for the requested entry-point name; the name was in the artifact's reflection but not in the library. Indicates a slangc emission bug or a stale reflection record.    | Re-cook the artifact; the artifact's reflection and bytecode must agree on entry-point names (SPEC §4.4 inv 1, "reflection paired with bytecode").              | `refuse`              |

### 10.2 Refuse semantics

Every refusal is "atomic": no partial artifact is published. For
`compile` failures, the in-flight `ShaderArtifact` is dropped before
return; `ShaderCache::insert` is never called. For `load_library`
failures, no `MetalLibraryHandle` is constructed; the caller's prior
handle (if any) remains live, and `render`'s PSOCache continues to
bind the prior PSO.

This matches SPEC §8.4 refusal cases 2–4 (compile-side, reflection-
side, cache-integrity-side); `MetalLibraryCreateFailed` and
`FunctionMissing` extend the refusal vocabulary to the
materialization seam.

### 10.3 Logging

Per `reviews/decisions/error-model.md`, every refusal logs **once at
the boundary that handles it**. The handling boundary for
shader-backend errors is:

- **Cooker** for `compile` / `reflect` / `link` / `capabilities`
  errors — `error` level with the offending
  `(SourceId, PermutationKey, CompileTarget)` attached.
- **`render` PSO builder** for `MetalLibraryCreateFailed` /
  `FunctionMissing` — `error` level with the offending
  `(ShaderHash, EntryPoint name)` attached, wrapped under
  `render::Error::PipelineCompileFailed` (the inner `shader::Error`
  rides in `ErrorContext::detail`).
- **Editor diagnostic UI** for hot-reload-driven failures — the
  same `Error` arm surfaces in the viewport overlay (matching
  harmonius R-12.4.3's expectation for source-edit reload).

The trait virtuals themselves do not call `spdlog`; they return
the error and let the caller's boundary log it. This preserves the
"one log per error" rule (error-model.md §"Logging / Telemetry"
#1).

### 10.4 Cross-references

- Closed enum source-of-truth: SPEC §5 (`enum class shader::Error`).
- Hot-reload refusal projection: SPEC §8.4 cases 2–4 (this
  aggregate's `compile`-side arms; case 1 covers
  `ShippingCompilationAttempted`).
- Engine-wide error policy: `reviews/decisions/error-model.md`.
- Sibling reflection / descriptor / cache refusal: SPEC §10.2 rows
  attributable to `ReflectionBlob` / `DescriptorLayout` /
  `ShaderCache`.

### 10.5 Amendment to SPEC §5 (proposed)

This design proposes adding two new enumerators to the closed
`enum class shader::Error` declared in SPEC §5 (lines 494–521):

- **`MetalLibraryCreateFailed`** — `MTL::Device::newLibrary`
  returned null; the artifact's bytecode could not be materialised
  into a Metal library on the active device. Recovery: refuse the
  PSO build; surface the `NS::Error*` description.
- **`FunctionMissing`** — `MTL::Library::newFunction` returned null
  for an entry-point name listed in the artifact's reflection.
  Recovery: refuse the PSO build; the artifact's reflection vs.
  bytecode pairing was violated.

Both arms are `refuse` severity. They are **not** present in §5 as
of the spec revision this design refines; they must be added in the
same follow-up plan that lands the §5 amendment for sibling spike
#79's `ArtifactSizeExceeded` (SPEC §10.2.1). Until that amendment
lands, the implementer must temporarily map both conditions onto
`Error::MetalLibEmitFailed` (closest-fit `refuse` arm) so the
closed-sum guarantee at the public boundary is never violated; this
mapping is unit-tested and removed when the new arms land.

The amendment is filed under sub-epic #69 (the SPEC-§5 amendment
sub-epic) in the same PR that lands `MetalLibraryLoader`. This
spike (#757) does **not** mutate §5 by construction — design spikes
do not amend the canonical enum.

---

## 11. Test Plan

### 11.1 Unit tests (Catch2, `tests/shader/backend/`)

Each row pins a §10 refusal arm or an SPEC §4.7 invariant. The
harness uses **mocked metal-cpp** for the loader-side tests
(headers replaced by an in-tree fake that records calls and returns
configurable `MTL::Library*` / `MTL::Function*` values) and a
**fixture-built `glibre-shadercc` binary** for the trait-side tests
(prebuilt under `tests/shader/fixtures/shadercc/`; deterministic
byte content; checked-in goldens for the resulting `metallib`
hashes).

| Test name                                                          | Drives arm / property                                             | Fixture                                                              |
|--------------------------------------------------------------------|-------------------------------------------------------------------|----------------------------------------------------------------------|
| `backend.capabilities_returns_pinned_struct`                       | (positive; §3.3)                                                  | Construct `SlangMetalBackend`, assert capability bits match the §3.3 table. |
| `backend.capabilities_is_pure_no_per_frame_query`                  | (positive; §4.7 inv 1)                                            | Two calls return byte-equal struct; mark function `noexcept` at type level; static-asserts. |
| `backend.compile_emits_artifact_with_paired_reflection`            | (positive; §4.7 inv 3, §4.4 inv 1)                                | Fixture Slang TU + key + `MetalLib` target; assert artifact's reflection is paired with bytecode. |
| `backend.compile_is_deterministic_across_runs`                     | (positive; §4.7 inv 3)                                            | Run `compile` 10 times against the same inputs; assert byte-equal `bytecode` and structural-equal `ReflectionBlob`. |
| `backend.compile_refuses_unsupported_target`                       | `UnsupportedTarget`                                               | Call `compile` with `CompileTarget::DXIL` (post-MVP target on MVP backend). |
| `backend.compile_refuses_capability_mismatch`                      | `CapabilityNotSupported`                                          | Construct a `PermutationKey` with `FeatureBit::RT` set against a backend whose `Capabilities::ray_tracing == false` (override via test seam). |
| `backend.compile_refuses_subprocess_spawn_failure`                 | `CompilerInvocationFailed`                                        | Set `glibre-shadercc` path to a missing file; assert error.          |
| `backend.compile_refuses_subprocess_nonzero_exit`                  | `CompilerExitNonZero`                                             | Fixture Slang TU with a deliberate Slang syntax error; assert stderr forwarded into error detail. |
| `backend.compile_refuses_subprocess_timeout`                       | `CompilerTimedOut`                                                | Fixture driver with a sleep loop; assert wall-clock budget enforced. |
| `backend.compile_refuses_metallib_emission_failure`                | `MetalLibEmitFailed`                                              | Fixture driver returning empty bytecode; assert refuse.              |
| `backend.reflect_re_ingests_idempotently`                          | (positive; §4.4 inv 2)                                            | Call `reflect` twice; assert structurally-equal `ReflectionBlob`.    |
| `backend.link_size_zero_refuses`                                   | `LinkFailed`                                                       | Empty span input.                                                    |
| `backend.link_size_one_is_trivial_identity`                        | (positive; §3.6)                                                  | Single-element span; assert no subprocess spawn (test seam counts spawns); assert bytecode bit-equal. |
| `backend.link_size_two_uses_subprocess`                            | (positive; §3.6)                                                  | Two compatible artifacts; assert one subprocess spawn; assert linked bytecode. |
| `backend.link_refuses_specialization_constant_missing`             | `SpecializationConstantMissing`                                   | Two artifacts with an unbound spec-constant; assert refuse.          |
| `backend.link_refuses_general_link_error`                          | `LinkFailed`                                                       | Two artifacts with conflicting entry-point sets.                     |
| `backend.shipping_compile_link_excluded`                           | `ShippingCompilationAttempted` (link-time)                        | Compile with `-DGLIBRE_SHIPPING=1`; assert `IShaderBackend::compile` symbol is absent. |
| `backend.shipping_load_library_linked`                             | (positive; §6.5 surviving cut)                                    | Compile with `-DGLIBRE_SHIPPING=1`; assert `MetalLibraryLoader::load_library` symbol is present. |
| `loader.load_library_succeeds_with_valid_metallib`                 | (positive; §3.5)                                                  | Mock metal-cpp returns a non-null `MTL::Library*`; assert handle is constructed and reflection is borrowed correctly. |
| `loader.load_library_refuses_non_metallib_target`                  | `UnsupportedTarget`                                               | Artifact with `target == CompileTarget::DXIL`.                       |
| `loader.load_library_refuses_metal_create_failure`                 | `MetalLibraryCreateFailed` (mapped to `MetalLibEmitFailed` until §10.5 amendment lands) | Mock metal-cpp returns null + populates `NS::Error*`; assert error captures the description. |
| `loader.load_library_releases_on_destruction`                      | (positive; §6.2 RAII)                                             | Mock counts `release` calls; assert exactly one `release` per `MetalLibraryHandle` destruction. |
| `loader.lookup_function_succeeds_for_listed_entry_point`           | (positive; §3.5)                                                  | Mock returns a non-null `MTL::Function*`; assert pointer returned. |
| `loader.lookup_function_refuses_missing_entry_point`               | `FunctionMissing` (mapped to `MetalLibEmitFailed` until §10.5 amendment lands) | Mock returns null for an entry-point name not in the library; assert error detail = name. |
| `loader.thread_safe_concurrent_load_library`                       | (positive; §6.2)                                                  | Spawn 8 threads each calling `load_library` against the same mock device; assert no data race (TSAN-clean). |

### 11.2 Integration tests (Catch2, `tests/shader/integration/`)

- `integration.compile_then_load_library_round_trip` — author a
  Slang TU, run `compile` against a real `glibre-shadercc` driver,
  then `load_library` against a real Metal device acquired via the
  platform fixture (`tests/platform/metal_device_fixture.cpp` —
  `MTL::CreateSystemDefaultDevice`); assert
  `MetalLibraryHandle::library() != nullptr`, assert every entry
  point in the artifact's reflection resolves through
  `lookup_function`. Skip on hosts without a Metal device (CI
  matrix gates Metal-fixture tests behind `MACOS_HAS_METAL=1`).
- `integration.cooker_walk_with_real_backend` — fixture project
  with 8 `.slang` TUs and ~100 enumerated permutations; cooker
  invokes `compile` on each in parallel; assert all artifacts
  inserted into a fixture cache; assert
  `Capabilities::ray_tracing` is queried exactly once at the
  cooker's startup.
- `integration.hot_reload_artifact_replaced_drives_load_library` —
  combine #719's `FileWatcher` + the `shader` plugin's hot-reload
  adapter + a real Metal device fixture; modify a TU; assert
  `ShaderArtifactReplaced` is published; assert the next
  `load_library` against the affected hash returns a fresh
  `MTL::Library*` (different pointer value); assert the prior
  library's `release()` was called when the prior PSOCache entry
  was evicted.
- `integration.shipping_link_strips_compile` — link a fixture
  shipping plugin with `-DGLIBRE_SHIPPING=1`; assert `nm -D
  shader.dylib` does not list `IShaderBackend::compile`'s symbol;
  assert `MetalLibraryLoader::load_library` is present.

### 11.3 Performance microbenchmarks

Catch2 `BENCHMARK` blocks under `tests/shader/perf/backend_bench.cpp`:

- `bench.compile_typical_shader` — Slang TU with 2 entry points,
  256 KiB total post-include bytes, 8 includes; assert <100 ms
  wall-clock (§9.1 cook-time budget).
- `bench.load_library_typical_metallib` — 256 KiB metallib; assert
  <2 ms wall-clock (§9.1).
- `bench.lookup_function_x100` — load a 256 KiB metallib once; call
  `lookup_function` 100 times against distinct entry points; assert
  <5 ms total (=<0.05 ms per function, §9.1).
- `bench.capabilities_x10000` — 10k calls to `capabilities()`;
  assert <10 µs total (=<0.001 ms per call, §9.1).

CI gates per `reviews/decisions/perf-budget.md` §"CI Gate Spec" #1:
any benchmark exceeding its budget fails the PR. Cook-time budgets
are not per-frame but the same gate harness asserts them.

### 11.4 E2E coverage

The E2E trace under `tests/e2e/shader/backend/` ships three fixture
projects:

- `slang-metal-happy-path` — 4 `.slang` TUs, ~40 permutations,
  cooker walks; runtime PSO build via `MetalLibraryLoader` for all
  artifacts; record golden frame; replay must be byte-equal.
- `metal-library-create-failure` — fixture artifact with
  deliberately-corrupt bytecode bytes; assert `load_library`
  refuses with `MetalLibraryCreateFailed` (mapped to
  `MetalLibEmitFailed` until amendment); assert PSOCache surfaces
  `render::Error::PipelineCompileFailed` wrapping the inner; assert
  prior PSO continues to bind.
- `entry-point-rename-hot-reload` — author a Slang TU; cook;
  rename one entry point in the source; trigger reload; assert
  `lookup_function` for the **old** name surfaces `FunctionMissing`
  (mapped to `MetalLibEmitFailed` until amendment) on the new
  artifact; assert `lookup_function` for the new name succeeds;
  assert affected PSOs are rebuilt.

Each fixture has a recorded `.glibre-trace` golden; CI replays
assert byte-equal trace output across runs (deterministic-replay
obligation, PHILOSOPHY §7).

---

## 12. Open Questions

- **[OPEN] `MetalLibraryCreateFailed` and `FunctionMissing`
  enumerator amendment.** §10.5 proposes adding two new
  `shader::Error` arms. Both must land in the same plan as sibling
  spike #79's `ArtifactSizeExceeded`; until then, the implementer
  maps both conditions onto `MetalLibEmitFailed`. Decide whether to
  fast-track the amendment plan or accept the mapping for the first
  implementation iteration.

- **[OPEN] `MTLBinaryArchive` integration.** Metal 4 supports
  `MTLBinaryArchive` for caching pipeline-state objects on disk
  (`MTL::Device::newBinaryArchive`). Per
  `specs/render/SPEC.md` §7.2.2 the PSO archive is render's
  concern, not shader's; but the seam between `MetalLibraryHandle`
  and `MTLBinaryArchive::addRenderPipelineFunctions` could plausibly
  belong to either context. MVP defers; revisit once the
  PSO-archive plan lands.

- **[OPEN] Multiple `IShaderBackend` implementations dispatcher.**
  MVP has one impl (`SlangMetalBackend`). A future second impl
  (e.g. Slang+DXIL for Windows) will need a dispatcher. The naive
  approach is `get_backend(name)` with a string name; the typed
  alternative is a source-language tag enum. Defer until a second
  impl arrives; either approach is admissible under the trait.

- **[OPEN] `link` size ≥ 2 driver flag schema.** Sibling #749 owns
  the `glibre-shadercc` argv; this design's `link` op assumes a
  `--link` mode that takes N input metallibs and produces one
  output. Confirm the driver argv shape during #749's
  implementation; if `--link` requires staging-file inputs (rather
  than stdin-piped binary streams), the link op's transient arena
  budget grows.

- **[OPEN] `Capabilities` per-device vs per-backend.** The struct
  is currently per-backend (a property of the slangc + metal-cpp +
  shipping-target triple). On Apple Silicon the M1 / M2 / M3 / M4
  chips all expose mesh-shaders / RT / fp16 in MVP scope, so a
  single per-backend struct suffices. If a future iOS target
  (pre-M-series A-chip) lacks one of these, the struct must
  bifurcate per-device. Defer; iOS is post-MVP.

- **[OPEN] `MetalLibraryLoader` cross-context include.**
  `render`'s PSO builder includes
  `glibre/shader/backend/metal/library_loader.hpp` directly. This
  is the only `render`-side include of a `shader`-context private
  header. The decision-record should pin whether this is permitted
  or whether the include should be re-exported through
  `glibre-types.dylib`'s middleman layer. Lean toward the direct
  include (simpler, narrower surface), but verify with the
  plugin-ABI plan when it lands.

- **[OPEN] `dispatch_data` zero-copy path.**
  `dispatch_data_create` with `DISPATCH_DATA_DESTRUCTOR_DEFAULT`
  copies the bytecode bytes. A custom destructor that takes
  ownership of the artifact's `bytecode` vector would avoid the
  copy but couples library-handle lifetime to artifact-vector
  lifetime in a way that complicates the cache's eviction story.
  Defer to perf-tuning; the 256 KiB memcpy at <2 ms / library is
  comfortably inside budget.
