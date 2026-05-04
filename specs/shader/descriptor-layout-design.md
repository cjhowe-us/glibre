# shader — Detailed Design: descriptor-layout aggregate

> Detailed design for the `DescriptorLayout` value object declared in
> `specs/shader/SPEC.md` §4.5. Refines §4.5, §4.8 inv. 4, §5
> (`DescriptorLayout` / `DescriptorTable` / `RootSignatureSchema` /
> `StaticSampler` stubs), §6.3 (`descriptor_layout.cpp` /
> `frequency_tagger.hpp`), §7.1 / §7.4 rule 3 (regenerate-not-migrate),
> §8.4 refusal case 3, §9.3 row `DescriptorLayout`, §10.2 rows
> `DescriptorFrequencyAmbiguous` / `DescriptorFrequencyMissing`, all
> in place. Cites `reviews/decisions/error-model.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/fory-codegen.md`,
> `reviews/decisions/frame-phases.md`. Style sibling:
> `specs/data/schema-registry-design.md`.
>
> Refs: spike #753 — `[SPIKE] design-shader-descriptor-layout-detailed`.
> Parent sub-epic #744. Sibling task-breakdown spike blocked-by this
> deliverable. Does not introduce new public surface beyond
> `specs/shader/SPEC.md` §5; deviations from that surface or the cited
> records would require an amendment spike, not an in-place edit.

## 1. Purpose

`DescriptorLayout` is the per-`(Backend, PermutationKey)` value object
that translates the canonical `ReflectionBlob` produced by §4.4 into
the **backend-neutral resource binding contract** that the Metal
backend (#762) and PSO cache (#766) consume to build native pipeline
state. Concretely it answers, for one shader artifact, four
questions:

1. **Which binding goes in which frequency group?** Every reflected
   `BindingSlot` (`ConstantBuffer`, `SampledImage`, `StorageImage`,
   `Sampler`, `StructuredBuffer`, `RWStructuredBuffer`,
   `AccelerationStructure`, `PushConstant`) is tagged with exactly
   one `DescriptorFrequencyGroup` ∈ `{PerFrame, PerPass, PerMaterial,
   PerDraw}` and lands in exactly one of the four
   `DescriptorTable`s (§5).
2. **Which argument-buffer slot does it occupy?** Within each
   frequency group the layout assigns a deterministic, dense slot
   index ordered by `(register_space, register_index, stage_mask)`
   (§4.5 inv. 3). On Metal that ordinal becomes the
   `[[id(N)]]` index inside that group's argument buffer; on D3D12
   (post-MVP) it becomes a root-table slot; on Vulkan (post-MVP) a
   binding within a descriptor set per group.
3. **How are static samplers, push constants, and vertex inputs
   lowered?** Static samplers are extracted into a side table
   addressed by ordinal (Metal: a sampler argument table; D3D12:
   immutable samplers in the root sig). Push constants are lowered
   onto Metal as a dedicated `PerDraw` argument-buffer slot (Metal
   has no native push-constant primitive); D3D12 keeps them as
   root constants. Vertex IO is normalized into a backend-neutral
   `VertexIOLayout` (location, format, stage mask) that the Metal
   backend turns into an `MTLVertexDescriptor` and Vulkan / D3D12
   into their respective IA descriptions.
4. **Is the contract internally consistent?** The layout enforces
   eight validation rules (§3.5) that catch ambiguous frequency
   tags, duplicate slots, push-constant overflow, sampler-table
   overflow, vertex-attribute aliasing, RT payload size violations,
   stage-mask inconsistencies, and material-parameter-block
   shape mismatches **before** the layout is ever cached or
   consumed by render.

The aggregate's one responsibility — **derive the backend-neutral
resource binding table for one `(Backend, PermutationKey)` from a
canonical `ReflectionBlob` once at cook time, cache it inside the
`ShaderArtifact`, and validate every invariant before publish** — is
sharp. If the binding-record shape, the frequency-group projection,
the slot assignment rule, the static-sampler / push-constant lowering
policy, the vertex-attribute mapping, the validation set, or the
serialized record layout changes, this design changes. Anything
else is out of scope.

What the descriptor-layout aggregate **explicitly refuses to own**:

- **Pipeline state object construction.** PSO creation (Metal:
  `MTLRenderPipelineState`; D3D12: `ID3D12PipelineState`; Vulkan:
  `VkPipeline`) belongs to `render` (`specs/shader/SPEC.md` §3
  refusal 4, §4.5 "does not create PSOs", §4.8 inv. 1; render
  consumes the layout via #762 and #766). The fence is sharp:
  `DescriptorLayout` is data; PSO is a backend handle.
- **Render-graph topology and pass scheduling.** Frequency groups
  are a *layout* primitive; their *runtime binding cadence* (when
  `PerFrame` arg buffers are written, when `PerPass` is rebound)
  belongs to `render`'s render-graph (`specs/shader/SPEC.md` §3
  refusal 1).
- **GPU memory allocation for descriptor heaps / argument buffers.**
  `DescriptorLayout` is a schema, not a buffer
  (`specs/shader/SPEC.md` §4.5 "Does not allocate GPU memory";
  perf-budget.md §"GPU memory is render-owned"). Render owns every
  byte of descriptor-resident GPU memory under
  `ContextTag::render`.
- **Reflection extraction.** `ReflectionBlob` is the §4.4 aggregate
  emitted by slangc's native reflection API in the same subprocess
  invocation as the bytecode (§4.8 inv. 2). The descriptor-layout
  aggregate is a *consumer* of `ReflectionBlob`; it never re-parses
  bytecode and never invokes slangc.
- **Permutation enumeration.** The 4-axis `PermutationKey` (§4.2)
  and its codegen-time enumeration table belong to the
  permutation-key aggregate (#747). `DescriptorLayout` takes a
  resolved `(Backend, PermutationKey)` as input identity; it does
  not enumerate keys.
- **Cache storage.** `ShaderCache` (§4.6) owns the CAS path layout
  and idempotent put/get; `DescriptorLayout` rides inside a
  `ShaderArtifactRecord` (§7.1) but does not key the cache itself.
- **Runtime binding state caches.** Per-frame binding caches and
  push-constant caches live in `render` (§3 cited GR-2 / GR-4
  research); the layout is the schema they bind against, not the
  cache itself.
- **Slang authoring conventions.** The `[register(...)]` /
  `[frequency(...)]` annotations the slangc reflection emits are
  authored by shader programmers and ingested by §6.3's
  `frequency_tagger.hpp`. `DescriptorLayout` consumes the tagged
  output; it does not parse Slang attributes itself.
- **Mid-frame mutation.** Layouts are derived once per
  `(Backend, PermutationKey)` offline, embedded in
  `ShaderArtifact`, and serialized into the cache. Runtime is
  **table lookup, never re-derivation** (§4.5 inv. 1, §4.8 inv. 4).
  Hot-reload re-derives alongside reflection on shader rebuild
  (§8 below); shipping has no derivation path linked at all
  (§4.3 inv. 3).

The SRP boundary is sharp by construction: descriptor-layout
catalogs the reflected resource bindings projected onto a
deterministic four-table partition; the §4.4 aggregate produces the
canonical reflection record; the §4.6 aggregate stores artifacts;
the render context turns the table into native pipeline state. Four
reasons to change → four designs. This document covers the third.

## 2. Requirements coverage

This section verifies that every harmonius MVP-scope requirement /
design clause about a descriptor-layout-shaped contract is either
covered by the design below or explicitly refused with rationale.
Inputs (research only — every conclusion re-derived against
PHILOSOPHY and the cited decision records per
`specs/shader/SPEC.md` §3):

- `harmonius/docs/requirements/rendering/gpu-abstraction-layer.md`
  R-2.1.16 / R-2.1.17 / R-2.1.18 — four-frequency descriptor groups,
  descriptor-layout inference from reflection, structured errors at
  every public boundary.
- `harmonius/docs/requirements/rendering/gpu-abstraction.md` GR-2 /
  GR-4 — state-cache responsibilities (binding caches, push-constant
  caches) live in render, not shader.
- `harmonius/docs/design/rendering/render-pipeline.md` §"Descriptor
  Layout Inference" + §RF-9 — descriptor layout inferred from
  slangc reflection once and cached; hot-reload re-runs reflection
  on new bytecode so layout stays in sync.
- `harmonius/docs/design/rendering/pipeline-state-cache.md` R-2.3.9.2
  / R-2.3.9.8 — descriptor layout inferred once from reflection and
  cached; PSO key composes shader hash with device fingerprint.
- `reviews/decisions/error-model.md` §"Decision" #1 — every public
  boundary returns `std::expected<T, glibre::Error>`.
- `reviews/decisions/fory-codegen.md` §"middleman dylib exposes" —
  the on-disk record set the layout serializes into.

| Harmonius clause                                                                                | Glibre disposition                                                                                                                                                                                                                                                                                                                                                                                              |
|-------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-2.1.16** — Four descriptor-frequency groups: `PerFrame`, `PerPass`, `PerMaterial`, `PerDraw` | **Covered.** Adopted verbatim per `specs/shader/SPEC.md` §3 collapse 4; `RootSignatureSchema` (§5) carries one `DescriptorTable` per group; §3.3 below pins the projection rule and §3.5 the partition invariants.                                                                                                                                                                                              |
| **R-2.1.17** — Descriptor layout inferred from reflection (no hand-authored binding tables)     | **Covered.** `DescriptorLayout::derive(const ReflectionBlob&)` is the sole construction path (§5; §3.2 below). No public constructor takes anything else; hand-authored binding tables are physically unreachable.                                                                                                                                                                                              |
| **R-2.1.18** — Structured errors at every public boundary; no exception unwinding across the ABI | **Covered.** `derive` returns `std::expected<DescriptorLayout, shader::Error>`; the four descriptor-layout-specific error arms (`DescriptorFrequencyAmbiguous`, `DescriptorFrequencyMissing`, plus two new arms `BindingOverflow` and `IncompatibleVertexLayout` introduced in §10 below) are added under sub-epic #69's §5 amendment plan; until then they map onto the closest existing arm per §10.3.        |
| **GR-2 / GR-4** — Binding caches and push-constant caches live in `render`, not in `shader`     | **Refused** — adopted as a refusal in `specs/shader/SPEC.md` §3 refusal 4 / §4.8 inv. 1. The descriptor-layout aggregate stops at the *schema*; per-frame binding state is a render concern.                                                                                                                                                                                                                    |
| **render-pipeline.md §"Descriptor Layout Inference"** — Layout derived once and cached         | **Covered.** §4.5 inv. 1 ("static per (Backend, PermutationKey); computed once offline; re-derivation at runtime is forbidden") + §4.8 inv. 4 ("runtime descriptor selection is a table lookup, never a re-derivation"). §3.6 below pins the cache-keying rule.                                                                                                                                                |
| **render-pipeline.md §RF-9** — Hot-reload re-runs reflection on new bytecode so layout stays in sync | **Covered (re-derive, not migrate).** §7.4 rule 3 says `DescriptorLayoutRecord` is regenerated, not migrated; §8 below is the descriptor-layout-side projection of the §8 source-edit hot-reload contract.                                                                                                                                                                                                  |
| **pipeline-state-cache.md R-2.3.9.2 / R-2.3.9.8** — PSO key composes shader hash with device fingerprint; descriptor layout is a cached reflection-derived input | **Refused (PSO cache lives in `render`)** — `specs/shader/SPEC.md` §3 refusal 4. The descriptor-layout aggregate is *one input* to that PSO key (`ShaderHash` already covers the layout because the layout is a pure function of the reflection that was hashed alongside the bytecode); the PSO cache itself is render's #766. |
| Implicit across rendering reqs — Each backend rolls its own native binding scheme              | **Collapsed.** N backend-specific harmonius binding-table designs → 1 backend-neutral `RootSignatureSchema` (§5) the Metal backend (#762) projects onto argument buffers and the future D3D12 / Vulkan backends project onto root sigs / pipeline layouts. The schema is the single shared input; backend-side translators are sibling consumers, not owners of the schema. |
| Implicit — Vertex IO is reflection-derived, not shader-author-asserted                          | **Covered.** `VertexIOLayout` is part of `ReflectionBlob` (§4.4) and is forwarded by the descriptor-layout into `RootSignatureSchema` after the §3.4 normalization pass. No public constructor admits hand-authored vertex layouts.                                                                                                                                                                            |

Glibre-native requirements added beyond harmonius:

- **Push-constant lowering rule (Metal MVP target).** Metal has no
  native push-constant primitive; harmonius assumed a generic
  abstraction. Glibre's MVP target is Metal-first, so the
  descriptor-layout aggregate **lowers every `PushConstantRange`
  onto a synthetic `PerDraw` argument-buffer slot at slot ordinal
  zero** (§3.4 below) and forwards the original range list in
  `RootSignatureSchema::push_constants` so post-MVP D3D12 / Vulkan
  backends can recover the native push-constant view. The two are
  redundant by design: the synthetic slot is the runtime path, the
  range list is the post-MVP-portability record.
- **Static-sampler limit.** Metal 4 caps argument-table samplers at
  16 per stage on the Apple Silicon baseline; the descriptor-layout
  aggregate refuses derivation that exceeds the limit with
  `Error::SamplerLimitExceeded` (new §10 arm tracked under sub-epic
  #69; until landing, mapped to `DescriptorFrequencyAmbiguous`'s
  closest-fit failure path).
- **Push-constant size ceiling.** The aggregate caps the sum of
  push-constant range sizes at 128 bytes (Metal-equivalent of the
  D3D12 root-constant 256 dword soft cap, halved to leave headroom
  for the synthetic `PerDraw` slot's other contents). Excess
  refuses with `Error::PushConstantTooLarge` (new §10 arm; same
  sub-epic #69 amendment).
- **Vertex-attribute compatibility check.** The aggregate computes
  a backend-neutral `vertex_layout_hash` (BLAKE3 over the sorted
  `(location, semantic, semantic_index, format_code, stage_mask)`
  tuples) which the `geometry` context (#115's vertex-stream
  catalog) can compare against its mesh layouts before draw. If
  the aggregate detects a per-stage `location` collision, the
  derivation refuses with `Error::IncompatibleVertexLayout` (new
  §10 arm).
- **Binding-overflow refusal.** Metal 4's per-stage argument-buffer
  cap is 31 entries (Apple Silicon baseline); the aggregate
  enforces a per-frequency-group ceiling of 31 bindings and
  refuses with `Error::BindingOverflow` (new §10 arm) when
  exceeded.

Net result: every harmonius MVP-scope clause about descriptor-layout
inference is either covered (R-2.1.16 / R-2.1.17 / R-2.1.18,
render-pipeline §"Descriptor Layout Inference", §RF-9), refused
with explicit rationale and a redirect to the owning context (GR-2 /
GR-4, pipeline-state-cache.md), or collapsed under PHILOSOPHY §10
(N backend binding-table designs → 1 backend-neutral schema). The
four glibre-native additions (push-constant lowering, sampler
ceiling, push-constant size ceiling, binding-overflow ceiling) are
the load-bearing platform-specific constraints that harmonius's
backend-agnostic prose left implicit.

## 3. Detailed model

### 3.1 Aggregate composition

```text
DescriptorLayout (value object, owned by ShaderArtifact)
└── RootSignatureSchema schema_                             (the four-table partition + side tables)
    ├── DescriptorTable per_frame                            (slots in PerFrame group, sorted)
    ├── DescriptorTable per_pass                             (slots in PerPass group, sorted)
    ├── DescriptorTable per_material                         (slots in PerMaterial group, sorted)
    ├── DescriptorTable per_draw                             (slots in PerDraw group, sorted)
    ├── eastl::vector<PushConstantRange> push_constants     (forwarded for post-MVP backends)
    └── eastl::vector<StaticSampler>     static_samplers     (sorted by (register_space, register_index))

DescriptorTable                                             (per-frequency, value object)
└── eastl::vector<BindingSlot> slots                         (sorted by (register_space, register_index, stage_mask))

BindingSlot (POD, declared in SPEC §5, embedded in tables)
├── BindingKind              kind                            (CB, SampledImage, StorageImage, Sampler, …)
├── std::uint32_t            register_space                  (Slang [register(..., space=N)] axis)
├── std::uint32_t            register_index                  (Slang [register(tN)] / [register(uN)] axis)
├── std::uint32_t            array_size                      (1 for scalar bindings; >1 for arrays)
├── StageMask                stages                          (bitset over the six Stage enumerators)
├── DescriptorFrequencyGroup frequency                       (one of four groups; tagged by §6.3)
└── eastl::string            name                            (debug label; never load-bearing)

StaticSampler (POD, side-table entry)
├── std::uint32_t register_space
├── std::uint32_t register_index
├── std::uint32_t descriptor_code                            (backend-neutral sampler descriptor ordinal)
└── StageMask     stages

PushConstantRange (POD, side-table entry)
├── std::uint32_t offset
├── std::uint32_t size
└── StageMask     stages
```

Container choice rationale (per PHILOSOPHY §11 / EASTL replaces
`std::` containers):

- **`eastl::vector<BindingSlot>` per `DescriptorTable`** — sorted
  ascending by `(register_space, register_index, stage_mask)` per
  §4.5 inv. 3. Slot ordinal **= vector index**: this is the
  identity that backends translate into Metal `[[id(N)]]` argument
  buffer indices, D3D12 root-table descriptor offsets, and Vulkan
  `VkDescriptorSetLayoutBinding::binding` numbers. The vector is
  the dense slot table; no separate `slot_index` field is stored
  because the index is the position. Allocation tagged
  `glibre::PerContextAllocator{ContextTag::shader}`.
- **`eastl::vector<StaticSampler>` `static_samplers`** — sorted
  ascending by `(register_space, register_index)`. Position is
  the sampler ordinal that backends translate into the static
  sampler descriptor index.
- **`eastl::vector<PushConstantRange>` `push_constants`** — order
  preserved from reflection (Slang already emits in declaration
  order). The synthetic `PerDraw` slot 0 (§3.4 push-constant
  lowering) is emitted as the first entry of `per_draw.slots`
  with kind `PushConstant` and a sentinel `register_space = 0xFF,
  register_index = 0xFF` so backends can detect-and-skip the
  forward-compatibility duplicate.
- **No hash-map containers** in the layout. All containers are
  vectors with deterministic ordering keys (§4.5 inv. 3); no
  iteration order from a hash container may leak into any field
  (consistent with §7.3 rule 2). This is what makes structural
  equality (`operator==` on `DescriptorLayout`, §5) byte-stable
  across hosts and runs.
- **Value semantics throughout.** `DescriptorLayout` is `final`,
  copyable, and `operator==`-comparable. Two layouts derived from
  byte-equal `ReflectionBlob`s compare equal regardless of host,
  toolchain, or construction order.

The whole aggregate is trivially-copyable wherever the `eastl`
containers it nests permit; the layout is **value-shaped, not
handle-shaped** because the §4.5 invariant 1 makes it static per
`(Backend, PermutationKey)`.

### 3.2 Construction path: `DescriptorLayout::derive(const ReflectionBlob&)`

The single public construction path (§5) is a pure function of the
reflection record. The signature:

```cpp
[[nodiscard]] static auto derive(const ReflectionBlob&) noexcept
    -> std::expected<DescriptorLayout, shader::Error>;
```

The implementation is staged to keep each pass single-responsibility
(SRP fence between passes; failure of any pass is observable as a
typed `shader::Error`):

```text
ReflectionBlob (input)
    │
    ▼
[Pass 1] Pre-condition check                     ── inv. 4 (every binding pre-tagged) verified
    │                                               failure: DescriptorFrequencyMissing (no binding lacks a tag)
    ▼
[Pass 2] Partition into four frequency groups    ── walk reflection.bindings; bucket by .frequency
    │                                               failure: DescriptorFrequencyAmbiguous (duplicate-tag detected; defense-in-depth)
    ▼
[Pass 3] Static-sampler extraction               ── pull BindingKind::Sampler entries that are flagged immutable
    │                                               failure: SamplerLimitExceeded (>16 per stage; new §10 arm)
    ▼
[Pass 4] Push-constant lowering                  ── synthesize PerDraw slot 0 + carry side-table list
    │                                               failure: PushConstantTooLarge (>128 bytes total; new §10 arm)
    ▼
[Pass 5] Vertex-IO normalization                 ── compute vertex_layout_hash; check stage / location uniqueness
    │                                               failure: IncompatibleVertexLayout (per-stage location collision; new §10 arm)
    ▼
[Pass 6] Per-table sort                          ── sort by (register_space, register_index, stage_mask) ascending
    │                                               infallible
    ▼
[Pass 7] Per-table cap check                     ── len(slots) ≤ 31 per group (Metal 4 baseline)
    │                                               failure: BindingOverflow (new §10 arm)
    ▼
[Pass 8] Validation & invariant check            ── §3.5 rules 1..8 (partition completeness, no-collision, …)
    │                                               failure: any of the eight error arms above (whichever the rule maps to)
    ▼
DescriptorLayout (output)
```

Each pass is a free function in `descriptor_layout.cpp` (§6.3) that
takes the in-progress builder by `&` and returns
`std::expected<void, shader::Error>`. The `derive` function is the
top-level driver that runs the eight passes in order, short-circuits
on the first `unexpected`, and wraps the final builder into a
`DescriptorLayout` value via a private constructor.

The driver is **purely functional**: same `ReflectionBlob` input
always produces the same `std::expected<DescriptorLayout, Error>`
output, byte-equal across runs, hosts, and toolchains. This is the
property that makes §4.5 inv. 1 ("static per
(Backend, PermutationKey); structurally equal across runs")
constructively true rather than aspirational.

### 3.3 Frequency-group projection

The §4.4 reflection record arrives **already frequency-tagged** by
the §6.3 `frequency_tagger.hpp/.cpp` pass that sits between the
slangc-reflection ingester and the descriptor-layout derivation
(`specs/shader/SPEC.md` §6.3 row `frequency_tagger.hpp/.cpp`).
The §4.4 invariant 3 makes this an aggregate-construction
postcondition: "Every reflected resource binding is annotated with
exactly one `DescriptorFrequencyGroup`; unassigned bindings cause
construction to fail with `Error::DescriptorFrequencyMissing`."

The descriptor-layout aggregate **does not re-classify** bindings.
Pass 2 above is a partition by an already-set tag, not a
reclassification:

```text
for slot in reflection.bindings:
    match slot.frequency:
        PerFrame    -> per_frame.slots.push(slot)
        PerPass     -> per_pass.slots.push(slot)
        PerMaterial -> per_material.slots.push(slot)
        PerDraw     -> per_draw.slots.push(slot)
```

The Slang authoring conventions that drive the tagger
(`[register(..., space=N)]` plus explicit `[frequency(...)]`
annotations) are pinned in `specs/shader/SPEC.md` §6.3 and are
**not** re-litigated here. The descriptor-layout aggregate's
contract is: "given a fully-tagged blob, produce a partitioned
layout"; if the tagger is wrong the failure surfaces as
`DescriptorFrequencyAmbiguous` (multiple tags collapsed onto one
slot) or `DescriptorFrequencyMissing` (Pass 1 catches an
unassigned slot).

The four groups carry distinct binding cadences that informed the
group choice (§3 collapse 4); the descriptor-layout aggregate is
oblivious to those cadences — it produces the schema; render
honors the cadence at bind time.

### 3.4 Backend lowering rules (Metal-first, post-MVP-portable)

This pass converts the backend-neutral schema into the four pieces
the Metal backend (#762) needs to call `MTLArgumentDescriptor` and
`MTLArgumentEncoder`:

#### 3.4.1 Argument-buffer slot assignment

For each of the four `DescriptorTable`s, the slot ordinal is
`vector index` after Pass 6 sort. Mapping to Metal `[[id(N)]]`:

```text
RootSignatureSchema.per_frame.slots[i]    ──► Metal arg-buffer 0, [[id(i)]]
RootSignatureSchema.per_pass.slots[i]     ──► Metal arg-buffer 1, [[id(i)]]
RootSignatureSchema.per_material.slots[i] ──► Metal arg-buffer 2, [[id(i)]]
RootSignatureSchema.per_draw.slots[i]     ──► Metal arg-buffer 3, [[id(i)]]
```

Argument buffer index per frequency is fixed (`PerFrame=0`,
`PerPass=1`, `PerMaterial=2`, `PerDraw=3`) — this is part of the
contract the Metal backend (#762) reads, not a render-graph runtime
choice. Stage masking (e.g. a binding visible to `Vertex` only)
is recorded on the slot and consumed by Metal's
`argumentBufferIndex` only at PSO build, not at layout-derive.

#### 3.4.2 Sampler binding

Static samplers (samplers that are immutable across draws — depth-
compare, shadow-comparison, anisotropic-trilinear, etc.) are pulled
out of the four frequency tables in Pass 3 and placed in the side
`RootSignatureSchema::static_samplers` list. They are addressed as
follows:

```text
RootSignatureSchema.static_samplers[i] ──► Metal sampler-arg-table, ordinal i
```

Dynamic samplers (samplers that vary per material — texture-bound
sampler states authored by an artist) remain `BindingKind::Sampler`
slots inside their frequency table (typically `PerMaterial`) and
follow the §3.4.1 argument-buffer rule. The §3.5 rule 5 partition
ensures no sampler is in both lists.

The 16-per-stage cap (§2 above) is checked at Pass 3; over-budget
input refuses with `SamplerLimitExceeded` and yields no layout.

#### 3.4.3 Vertex-attribute mapping

The reflection's `VertexIOLayout` is a list of `VertexInputElement`
records carrying `(semantic, semantic_index, location, format_code)`.
Pass 5 normalizes:

```cpp
// Conceptual lowering — the public type stays as VertexIOLayout
// declared in §5; this code is sketched to show how the Metal
// backend uses it.
struct VertexAttributeMapping {
    eastl::vector<VertexInputElement> elements;  // sorted by (location, semantic_index)
    eastl::array<std::byte, 32>       layout_hash;  // BLAKE3 of normalized tuples
    StageMask                          stage_visibility;  // union of stages reading any element
};
```

The `VertexInputElement::format_code` enum is a backend-neutral
ordinal whose mapping to Metal pixel formats / D3D12 DXGI formats
/ Vulkan VkFormats is owned by the per-backend translator (#762
for Metal). The aggregate stores the ordinal and **does not**
resolve it.

The layout hash (§2 above) is computed at Pass 5 and recorded on
the `RootSignatureSchema` as a `vertex_layout_hash` field
(addition to §5; tracked under sub-epic #69 amendment plan as
the in-place §5 amendment that lands `BindingOverflow` /
`IncompatibleVertexLayout` / `SamplerLimitExceeded` /
`PushConstantTooLarge` on the public boundary). Until the
amendment lands, the layout hash is computed and cached on
descriptor-layout's private side and exposed only to the
geometry context's compatibility check via a private helper.

#### 3.4.4 Push-constant lowering (Metal-first)

Metal has no native push-constant primitive. Pass 4 lowers
push-constants two ways simultaneously:

1. **Synthetic `PerDraw` argument-buffer slot 0.** A single
   `BindingSlot` with `kind = PushConstant`, `register_space =
   0xFF`, `register_index = 0xFF` (sentinel space/index that
   never collides with a real Slang register), `array_size = 1`,
   `stages = union of every PushConstantRange's stages`, and
   `frequency = PerDraw`. This is the runtime path: the Metal
   backend reads the slot as a regular argument-buffer entry
   pointing to a `setBytes:`-style transient allocation, sized
   to `Σ size of every PushConstantRange`.
2. **Side-list `RootSignatureSchema::push_constants`.** The
   forwarded original list, untouched. This is the
   post-MVP-portability path: the future D3D12 backend reads it
   as root-32-bit-constants; the future Vulkan backend reads it
   as a `VkPushConstantRange` array.

The two are by-design redundant. Backends use whichever matches
their native primitive; the `DescriptorLayout` does not pick. The
total size cap (128 bytes) is enforced at Pass 4; over-budget
input refuses with `PushConstantTooLarge`.

The synthetic slot is always at `per_draw.slots[0]` after Pass 6
because:

- `register_space = 0xFF, register_index = 0xFF` sorts last by
  numeric ordering.
- However, the §3 collapse-4 commit (the spec freezing slot 0 as
  the push-constant slot) **prepends** the synthetic slot before
  the sort to guarantee it stays at index 0.

Pass 6's sort is therefore stable and applied to `slots[1..]`
only when a synthetic slot is present. This is the single
deviation from "always sort by register tuple"; it is documented
here and unit-tested at §11.

### 3.5 Validation rules

Eight invariants the layout enforces. Each maps to one §10 error
arm; failures refuse derivation atomically (no partial layout is
ever returned).

| # | Rule | Trigger | §10 arm |
|---|------|---------|---------|
| 1 | **Every binding has exactly one frequency tag.** | A `BindingSlot` arrives at Pass 1 with no `DescriptorFrequencyGroup` resolution after the §6.3 tagger ran. | `DescriptorFrequencyMissing` |
| 2 | **No binding has multiple frequency tags.** | A `BindingSlot` arrives at Pass 2 with a tag that conflicts with a duplicate slot for the same `(register_space, register_index, stage_mask)` triple. Defense-in-depth: the §6.3 tagger should already have caught this. | `DescriptorFrequencyAmbiguous` |
| 3 | **Per-frequency slot uniqueness.** Within one `DescriptorTable`, `(register_space, register_index, stage_mask)` is unique. | Two bindings share the triple inside the same group after Pass 2. | `DescriptorFrequencyAmbiguous` |
| 4 | **Per-frequency slot count ≤ 31.** Metal 4 argument-buffer per-stage cap (Apple Silicon baseline). | `len(table.slots) > 31` for any `table ∈ {per_frame, per_pass, per_material, per_draw}` after Pass 6. | `BindingOverflow` |
| 5 | **Sampler partition completeness.** A `BindingKind::Sampler` slot is in **exactly one** of `per_*.slots` (dynamic) or `static_samplers` (immutable), never both, never neither. | A sampler slot falls through both classifiers, or a slot tagged immutable also appears in the frequency table. | `DescriptorFrequencyAmbiguous` |
| 6 | **Static-sampler ≤ 16 per stage.** Metal 4 argument-table cap. | A `StageMask::bits` count of static samplers > 16 for any single stage. | `SamplerLimitExceeded` |
| 7 | **Push-constant total size ≤ 128 bytes.** | `Σ pc.size ∀ pc ∈ push_constants > 128`. | `PushConstantTooLarge` |
| 8 | **Vertex-input location uniqueness per stage.** Within one `Stage`, no two `VertexInputElement`s share the same `location`. | Pass 5 detects a per-stage `location` collision in the reflection's vertex-input list. | `IncompatibleVertexLayout` |

Five additional structural invariants are enforced by construction
(no separate rule needed):

- **Partition completeness.** Every `BindingSlot` in
  `reflection.bindings` lands in exactly one `DescriptorTable` —
  Pass 2's match statement is exhaustive over the
  `DescriptorFrequencyGroup` enum's four arms; a missing tag is
  rule 1 above.
- **Sort stability.** Pass 6 uses `eastl::stable_sort`, so equal
  `(register_space, register_index, stage_mask)` triples (which
  rule 3 forbids anyway) would preserve insertion order — but
  rule 3 makes the case impossible.
- **Synthetic push-constant slot positioning.** §3.4.4 above
  guarantees the synthetic slot is at `per_draw.slots[0]` when
  push constants exist; otherwise `per_draw.slots[0]` is the
  numerically-lowest real binding.
- **Static-sampler ordering.** Pass 6 sorts `static_samplers` by
  `(register_space, register_index)`; ordinal == vector index.
- **Push-constant range list ordering.** Preserved from
  reflection; the `frequency_tagger.hpp/.cpp` pass already emits
  in declaration order (`specs/shader/SPEC.md` §6.3).

### 3.6 Identity, equality, and cache keying

The layout's identity is **structural**, not nominal:

```cpp
friend bool operator==(const DescriptorLayout&, const DescriptorLayout&) noexcept = default;
```

Two layouts are equal iff their `RootSignatureSchema`s are
byte-equal. Because every container is sorted by spec-frozen keys
(§3.5 + §4.5 inv. 3), structural equality is well-defined and
byte-stable across hosts and runs.

The layout is **not** a cache key on its own. The cache key is
`ShaderHash` (§2 of the SPEC, §4.6 inv. 1), which is the BLAKE3 of
`(preprocessed source ∪ resolved PermutationKey ∪ canonical
compile flags ∪ CompileTarget)`. Because the layout is a pure
function of the reflection, which is a pure function of the
bytecode, which is a pure function of those four hash inputs,
**byte-equal `ShaderHash` ⇒ byte-equal `DescriptorLayout`**. The
layout rides inside `ShaderArtifactRecord` (§7.1 below) and is
re-derived only when the hash changes.

This is the property that makes §4.5 inv. 1 ("computed once
offline; re-derivation at runtime is forbidden") observable: the
runtime never re-derives because every cached `ShaderArtifact`
already carries its layout, and every layout lookup is a direct
field read off the artifact, not a `derive(reflection)` call.

### 3.7 Per-`(Backend, PermutationKey)` parameterization

Although `DescriptorLayout::derive` takes only a `ReflectionBlob`,
the §4.5 invariant 1 insists the layout is "static per
`(Backend, PermutationKey)`". The two-axis identity is preserved
by the production path:

- **Backend** axis: the §4.4 reflection blob is produced by the
  `IShaderBackend::reflect(...)` call (§4.7), which is keyed by
  the backend that emitted the bytecode. Different backends could
  in principle produce different reflection blobs from the same
  source (different register-space conventions, different format
  ordinals); the descriptor-layout aggregate's purity then makes
  the per-backend layout deterministic.
- **PermutationKey** axis: the `ReflectionBlob` is paired with the
  bytecode that was emitted for one specific `PermutationKey` in
  the §4.3 compile call. The descriptor-layout's purity makes the
  per-key layout deterministic.

The aggregate therefore does not need backend or key as explicit
inputs to `derive` — the reflection blob carries the relevant
identity by virtue of how it was produced. This collapses two
construction parameters into one, satisfying PHILOSOPHY §10
(Occam's razor: "Two collapsing requirements become one
primitive").

## 4. Public surface

### 4.1 Public types (locked from `specs/shader/SPEC.md` §5)

The public surface is exported by the `shader` plugin dylib through
the `glibre::shader` namespace declared in
`specs/shader/SPEC.md` §5:

```cpp
// shader/include/glibre/shader/shader.hpp — locked.
namespace glibre::shader {

enum class DescriptorFrequencyGroup : std::uint8_t {
    PerFrame, PerPass, PerMaterial, PerDraw,
};

enum class BindingKind : std::uint8_t {
    ConstantBuffer, SampledImage, StorageImage, Sampler,
    StructuredBuffer, RWStructuredBuffer, AccelerationStructure,
    PushConstant,
};

struct StageMask { std::uint8_t bits{0}; /* … */ };

struct BindingSlot {
    BindingKind              kind;
    std::uint32_t            register_space;
    std::uint32_t            register_index;
    std::uint32_t            array_size;
    StageMask                stages;
    DescriptorFrequencyGroup frequency;
    eastl::string            name;
    /* … */
};

struct VertexInputElement { /* semantic / location / format_code */ };
struct VertexIOLayout     { eastl::vector<VertexInputElement> elements; };
struct PushConstantRange  { std::uint32_t offset; std::uint32_t size; StageMask stages; };

struct DescriptorTable {
    eastl::vector<BindingSlot> slots;
    /* operator== defaulted */
};

struct StaticSampler { /* register_space, register_index, descriptor_code, stages */ };

struct RootSignatureSchema {
    DescriptorTable                  per_frame;
    DescriptorTable                  per_pass;
    DescriptorTable                  per_material;
    DescriptorTable                  per_draw;
    eastl::vector<PushConstantRange> push_constants;
    eastl::vector<StaticSampler>     static_samplers;
    /* operator== defaulted */
};

class DescriptorLayout {
public:
    [[nodiscard]] static auto derive(const ReflectionBlob&) noexcept
        -> std::expected<DescriptorLayout, shader::Error>;

    [[nodiscard]] auto table(DescriptorFrequencyGroup) const noexcept
        -> const DescriptorTable&;

    [[nodiscard]] auto schema() const noexcept
        -> const RootSignatureSchema&;

    friend bool operator==(const DescriptorLayout&, const DescriptorLayout&) noexcept = default;

private:
    DescriptorLayout() = default;
    RootSignatureSchema schema_{};
};

}  // namespace glibre::shader
```

The class declaration is exactly what `specs/shader/SPEC.md` §5
publishes; this design specifies semantics, validation rules, and
serialization without widening the surface.

### 4.2 `DescriptorLayout` operations

| Operation               | Signature                                                                | Cost          | Discipline                                                |
|-------------------------|--------------------------------------------------------------------------|---------------|-----------------------------------------------------------|
| `derive`                | `(const ReflectionBlob&) -> std::expected<DescriptorLayout, shader::Error>` | `O(N log N)` cold (N = bindings + samplers + vertex elements) | Cook-time / hot-reload only; never per-frame.    |
| `table`                 | `(DescriptorFrequencyGroup) -> const DescriptorTable&`                   | `O(1)`        | Read-only field accessor.                                 |
| `schema`                | `() -> const RootSignatureSchema&`                                       | `O(1)`        | Read-only field accessor.                                 |
| `operator==`            | `(const DescriptorLayout&, const DescriptorLayout&) -> bool`             | `O(N)`        | Element-wise compare; deterministic per §3.6.             |

`std::expected<T, shader::Error>` applies only to `derive`. The
accessors (`table`, `schema`) cannot fail — they expose existing
fields whose construction was already validated. Per
`reviews/decisions/error-model.md` §"Decision" #3 every public
boundary in the shader context is `noexcept`; the accessors return
`const &` and `derive` returns `std::expected`, so no exception
crosses the ABI.

`derive`'s `shader::Error` is one of the eight arms in §3.5 (Pass
1..8 failures); no other arm reaches the descriptor-layout call
site.

### 4.3 No reflective surface, no mutation

The aggregate exposes **no** mutation API: no `add_binding(...)`,
no `set_static_sampler(...)`, no `merge(other)`. The only path to
a non-default `DescriptorLayout` is `derive(reflection)`. This is
the structural enforcement of §4.5 inv. 1 ("computed once;
re-derivation at runtime is forbidden") at the public boundary —
even editor / dev-build code cannot mutate the schema after
derive.

The aggregate exposes **no** reflective API on its bindings
either: no `find_binding(name)`, no
`get_field_descriptor(slot, "x")`. PHILOSOPHY §6 forbids runtime
reflection in shipping; the editor's debug overlay walks
`schema().per_frame.slots` etc. as a passive enumeration, not a
dispatch surface. The `BindingSlot::name` field is debug-only and
"never load-bearing" (§5 of the SPEC).

### 4.4 ABI shape across the plugin boundary

Per PHILOSOPHY §11 and `reviews/decisions/plugin-abi.md`, public
plugin ABI surfaces never expose `eastl::` containers — they
cross the boundary as POD spans / handles. The
descriptor-layout aggregate sits inside `shader.dylib` (a plugin),
not in the middleman `glibre-types.dylib`. Cross-plugin consumers
(render's #762 Metal backend, render's #766 PSO cache) reach the
layout through the `data` middleman's
`glibre::types::shader::DescriptorLayoutRecord` (the Fory-codegen
POD form, §7 below), **not** through `glibre::shader::DescriptorLayout`
directly. The `shader::DescriptorLayout` C++ type is internal to
the `shader` plugin's process-local consumers (the cooker, the
cache loader, the editor's shader inspector); cross-plugin handoff
goes through the serialized record.

This matches the SPEC's existing contract: the on-disk
`ShaderArtifactRecord` (§7.1) carries `DescriptorLayoutRecord`,
and render binds against the record-shape. The C++ type and the
record-shape are 1:1; there is no information loss across the
serialization boundary.

## 5. Hot / cold path split

The descriptor-layout aggregate's interaction with every other
engine subsystem is **cold-path** by design:

| Path  | Caller                                                  | Trigger                                  | Frequency                                  | Budget                                      |
|-------|---------------------------------------------------------|------------------------------------------|--------------------------------------------|---------------------------------------------|
| Cold  | `CompilationPipeline` (§4.3) post-reflect step         | Cooker walk per `(PermutationKey, target)` | Once per artifact at cook                  | ≤ 0.50 ms wall-clock per derive (§9 below)  |
| Cold  | Hot-reload re-derive (§8 below)                        | Editor / dev-build source edit           | Once per affected `(PermutationKey, target)` | ≤ 0.50 ms wall-clock per derive            |
| Cold  | `ShaderArtifact` deserialize (`ShaderCache::lookup`)   | First lookup of an artifact per session  | Once per resident artifact per session     | The deserialize is the cost; no derive runs |
| Cold  | Editor binding inspector (`SchemaBrowserPanel`-equiv)  | Editor opens shader inspector            | Per artifact, per inspect open             | `O(N)` walk; no derive                      |
| Hot   | (none — never on the every-frame path)                 | n/a                                      | n/a                                        | n/a                                         |

The fast-path in the engine **does not derive descriptor layouts
every frame**. Render's #762 Metal backend reads the cached
`DescriptorLayout` field off `ShaderArtifact` at PSO build (a
cold event keyed on `ShaderHash`), translates it into Metal
`MTLArgumentEncoder` instances **once** per PSO, caches the
encoder in the #766 `PSOCache`, and reuses it across every draw
that binds the PSO. Per-frame binding work in render is
"resolve PSO → run pre-recorded encoder" — the descriptor layout
is the input to that resolution, never re-evaluated per draw.

The pattern (**derive at cook + hot-reload; runtime reads
cached field on artifact**) is what makes the §9 per-frame cell
**zero**:

```cpp
// render/internal Metal backend sketch — not part of shader's surface.
// Conceptual; lives in #762.
namespace glibre::render::metal {

struct CachedArgumentEncoder {
    NS::SharedPtr<MTL::ArgumentEncoder> per_frame_encoder;
    NS::SharedPtr<MTL::ArgumentEncoder> per_pass_encoder;
    NS::SharedPtr<MTL::ArgumentEncoder> per_material_encoder;
    NS::SharedPtr<MTL::ArgumentEncoder> per_draw_encoder;
};

auto build_pso(const glibre::shader::ShaderArtifact& artifact) noexcept
    -> std::expected<PSOEntry, render::Error>
{
    // One-shot translation of the descriptor layout into Metal encoders.
    // The layout is already-derived; this function is also cold-path
    // (PSO build), invoked from PSOCache miss.
    const auto& schema = artifact.descriptor_layout.schema();
    /* … translate per_frame.slots → MTLArgumentDescriptor[] … */
}

}  // namespace glibre::render::metal
```

**Pointer-stability rules** (the seam that makes the cache safe):

1. **`DescriptorLayout` instances are immutable post-derive.**
   The only mutation path was `derive`'s private builder; once
   `derive` returns `expected<DescriptorLayout, Error>`, the
   value is frozen. `const DescriptorLayout&` references handed
   to render (via `ShaderArtifact::descriptor_layout`) are stable
   for the artifact's lifetime in the cache.
2. **`ShaderArtifact` instances are pinned by `ShaderCache`.** The
   §4.6 cache holds `ShaderArtifact` records by hash, idempotent
   on insert (§4.6 inv. 1). Render's #766 `PSOCache` caches a
   pointer to `ShaderArtifact` (or its hash + a `lookup` thunk);
   the artifact's `descriptor_layout` field is therefore stable
   for the cache instance's lifetime.
3. **Hot-reload publishes a *new* `ShaderArtifact` at a new
   `ShaderHash` (§8 below); old layouts remain reachable
   through the old hash.** The PSOCache evicts on
   `ShaderArtifactReplaced` (§8.5 of the SPEC) and rebuilds
   against the new layout via the new hash. The §3.6 identity
   rule guarantees the new layout is structurally equal to a
   re-derive of the new reflection.

The split makes the descriptor-layout's per-frame budget cell
(§9) **zero** on the steady-state hot path, and bounded by
`O(N log N)` on the cold-path derive paths (N = total bindings +
samplers + vertex-input elements per artifact).

## 6. Concurrency

The descriptor-layout aggregate is a **pure transformation, thread-
safe by construction**. There is no shared mutable state — `derive`
is a pure function, `DescriptorLayout` instances are immutable
after construction, and the cache that holds them
(`ShaderCache`, §4.6) is governed by `core`'s plugin-loader
concurrency rules (§4.6 not the layout itself).

### 6.1 Threading rules

1. **`DescriptorLayout::derive` is reentrant from any thread.**
   It reads its `ReflectionBlob` argument by `const &` and
   produces a fresh `DescriptorLayout` value with no shared
   state. The cooker (which runs on a worker pool — see §6.5
   below) calls `derive` in parallel for distinct artifacts
   without locking.
2. **`DescriptorLayout` instance accessors (`table`, `schema`,
   `operator==`) are pure reads** with no synchronization. Once
   `derive` returns, the value is immutable; copies are
   independent values.
3. **No lock, no atomic, no fence** lives inside the
   descriptor-layout aggregate. The aggregate has no internal
   mutable state to protect.
4. **Cross-thread handoff to `ShaderCache`** is governed by the
   cache's invariant — `ShaderCache::insert` is idempotent
   (§4.6 inv. 1) and serialized by the cache's own lock (an
   implementation concern of §4.6's task-breakdown, not this
   aggregate). The descriptor-layout aggregate hands off a
   value; how the cache stores it is the cache's problem.
5. **Cross-thread handoff to render (#762, #766)** is
   value-shaped: render reads `ShaderArtifact::descriptor_layout`
   by `const &` after `ShaderCache::get` returns a `const
   ShaderArtifact*`. The pointer-stability rules in §5 above
   make the read race-free.

### 6.2 Cooker parallelism

The cooker's per-artifact work (`compile → reflect → derive →
insert`, §6.4 of the SPEC) parallelizes naturally because
`derive` is pure and `ReflectionBlob` is a value. The
cooker's worker pool's saturation strategy is owned by the
`shader-cook-time-budget` spike (§9.2 of the SPEC), not by
this aggregate. From the descriptor-layout aggregate's vantage,
"N workers calling `derive` in parallel" is structurally the
same as "1 worker calling `derive` N times sequentially" — the
output is bit-equal in either case.

### 6.3 No frame-phase dependency

The descriptor-layout aggregate participates in **no** frame
phase (`reviews/decisions/frame-phases.md` table; §9.1 of the
SPEC: "Phase ownership: none"). Concurrency rules from
`hot-reload-protocol.md` apply only to the §4.6 cache and the
core plugin loader; the layout itself is never mutated at a
frame boundary because it is not mutated at all after
`derive`.

## 7. Persistence + ABI

The descriptor-layout aggregate persists exclusively as a sub-
record of `ShaderArtifactRecord` via Apache Fory schemas under
the `data` middleman dylib (`reviews/decisions/fory-codegen.md`).
The schema is locked by `specs/shader/SPEC.md` §7.1 and §7.4
rule 3 (regenerate-not-migrate). This design refines those
sections in place; nothing here widens or contradicts them.

### 7.1 Persistent record set

| Record | Schema path | Role |
|--------|-------------|------|
| `DescriptorLayoutRecord` | `data/schemas/shader/DescriptorLayoutRecord.fory` (embedded by tag inclusion in `ShaderArtifactRecord.fory` per §7.1 of SPEC) | The full layout: four `DescriptorTableRecord`s + `PushConstantRangeRecord` list + `StaticSamplerRecord` list. |
| `DescriptorTableRecord` | embedded sub-schema in `DescriptorLayoutRecord` | One frequency group's slot list. |
| `BindingSlotRecord` | embedded sub-schema (shared with `ReflectionRecord` per §7.1 of SPEC) | Per-binding POD; mirrors §5 `BindingSlot`. |
| `PushConstantRangeRecord` | embedded sub-schema (shared) | Per-range POD. |
| `StaticSamplerRecord` | embedded sub-schema | Per-sampler POD. |

### 7.2 Schema sketch (refines `specs/shader/SPEC.md` §7.2)

The format follows the canonical sketch in
`reviews/decisions/fory-codegen.md`. Tags are immutable once
shipped; removal of a field marks the tag `reserved`. Built-in
scalar names compile to the audited
`glibre/types/_builtins.hpp` set.

```fory
schema glibre.shader.DescriptorLayoutRecord {
  version  1
  since    "0.1.0"

  field per_frame       : glibre.shader.DescriptorTableRecord     tag 1 since 1
  field per_pass        : glibre.shader.DescriptorTableRecord     tag 2 since 1
  field per_material    : glibre.shader.DescriptorTableRecord     tag 3 since 1
  field per_draw        : glibre.shader.DescriptorTableRecord     tag 4 since 1
  field push_constants  : list<glibre.shader.PushConstantRangeRecord> tag 5 since 1   // sorted by reflection declaration order
  field static_samplers : list<glibre.shader.StaticSamplerRecord> tag 6 since 1   // sorted by (register_space, register_index)
  field vertex_layout_hash : bytes32                              tag 7 since 1   // BLAKE3 of normalized (location, semantic, semantic_index, format_code, stage_mask) tuples
}

schema glibre.shader.DescriptorTableRecord {
  version  1
  since    "0.1.0"

  field slots : list<glibre.shader.BindingSlotRecord> tag 1 since 1   // sorted by (register_space, register_index, stage_mask)
}

schema glibre.shader.BindingSlotRecord {
  version  1
  since    "0.1.0"

  field kind            : u8                              tag 1 since 1   // BindingKind enum ordinal
  field register_space  : u32                             tag 2 since 1
  field register_index  : u32                             tag 3 since 1
  field array_size      : u32                             tag 4 since 1
  field stages          : u8                              tag 5 since 1   // StageMask bits
  field frequency       : u8                              tag 6 since 1   // DescriptorFrequencyGroup enum ordinal
  field name            : string                          tag 7 since 1   // debug; never load-bearing
}

schema glibre.shader.PushConstantRangeRecord {
  version  1
  since    "0.1.0"

  field offset : u32 tag 1 since 1
  field size   : u32 tag 2 since 1
  field stages : u8  tag 3 since 1
}

schema glibre.shader.StaticSamplerRecord {
  version  1
  since    "0.1.0"

  field register_space  : u32 tag 1 since 1
  field register_index  : u32 tag 2 since 1
  field descriptor_code : u32 tag 3 since 1
  field stages          : u8  tag 4 since 1
}
```

The `vertex_layout_hash` field at tag 7 is the new addition this
design specifies (the SPEC §7.2 sketch named the surrounding
`DescriptorLayoutRecord` schema without listing the hash field;
the SPEC's schema author noted that "the schema lives at the
record level"). The hash is computed at Pass 5 (§3.4.3) and
serialized verbatim. The §5 amendment plan under sub-epic #69
that lands `BindingOverflow` / `IncompatibleVertexLayout` /
`SamplerLimitExceeded` / `PushConstantTooLarge` on the public
boundary also lands the matching `vertex_layout_hash` accessor on
`RootSignatureSchema`; until then the field is computed and
serialized but only readable on the deserialized record (which
is what the geometry context's compatibility check needs anyway).

### 7.3 Determinism + serialization rules (refines §7.3)

1. **Sort orders are spec-frozen.** `DescriptorTableRecord.slots`
   is sorted ascending by `(register_space, register_index,
   stage_mask)` (§4.5 inv. 3). `static_samplers` ascending by
   `(register_space, register_index)`. `push_constants`
   preserves reflection declaration order. The synthetic
   push-constant slot (§3.4.4) is at `per_draw.slots[0]` always.
2. **No iterator-order leak.** All containers are vectors; no
   hash-map iteration order may leak into any field
   (consistent with `specs/shader/SPEC.md` §7.3 rule 2).
3. **Round-trip equality.** For every record type,
   `Fory::deserialize ∘ Fory::serialize` is the identity on the
   in-memory §5 type. Catch2 golden round-trip tests at the
   plan level (§11 below).
4. **Vertex-layout hash determinism.** BLAKE3 is computed over
   the normalized tuples in spec-frozen order (sort by
   `(location, semantic_index, semantic, format_code,
   stage_mask)`); the hash is byte-stable across hosts and runs
   per the determinism contract.

### 7.4 ABI hash composition

Per `reviews/decisions/fory-codegen.md` §"ABI Hash Function" and
§"middleman dylib exposes" #3, the schema's contribution to
`glibre_types_abi_hash()` is:

```
glibre.shader.DescriptorLayoutRecord || ":" || version_le || ":" || schema_source_blake3
```

concatenated with every other shader-context schema's analogous
line, in canonical FQN code-point order, BLAKE3-d, hex-
encoded. The five sub-schemas above (`DescriptorLayoutRecord`,
`DescriptorTableRecord`, `BindingSlotRecord`,
`PushConstantRangeRecord`, `StaticSamplerRecord`) each contribute
their own line. Tag-additive evolution (e.g. adding
`vertex_layout_hash` at tag 7 with `since 1`) bumps the
`schema_source_blake3` of `DescriptorLayoutRecord` and therefore
bumps the engine-wide `glibre_types_abi_hash`; old plugins
compiled against the previous hash refuse to load
(`core::Error::PluginAbiHashMismatch`, plugin-abi.md
§"Failure Modes" row 4).

The descriptor-layout records do **not** introduce new
`extern "C"` symbols beyond what fory-codegen.md
§"middleman dylib exposes" #4 already pins
(`glibre_types_serialize_<fqn>`,
`glibre_types_deserialize_<fqn>`,
`glibre_types_register_migration_<fqn>`); the codegen tool
emits one trampoline per record type as part of its generated
TUs, not as a hand-authored aggregate concern.

### 7.5 Migration rules (refines §7.4 rule 3)

`DescriptorLayoutRecord` is **regenerated, not migrated**. A
schema bump on this record always re-derives layouts from the
surviving `ReflectionRecord` rather than transforming old
layouts in-place. The reflection record is the source of truth
(§4.5 inv. 1); a stale layout encountered at load time is
rebuilt by calling `DescriptorLayout::derive(reflection)` and
the result replaces the on-disk record on the next cook.

The mechanical rule:

1. **Loader detects schema-version mismatch on
   `DescriptorLayoutRecord`** during `ShaderCache` open
   (`specs/shader/SPEC.md` §6.5).
2. **Loader does NOT call a `migrate_DescriptorLayoutRecord_vN_to_vNplus1`
   function.** No such function is ever provided. The §7.4 rule
   is constructive: re-derive from reflection.
3. **In dev / editor builds, the loader invokes
   `DescriptorLayout::derive(artifact.reflection)`** to produce a
   fresh layout, replaces the in-memory artifact's layout field,
   and queues a cooker rewrite of the on-disk record.
4. **In shipping builds, the loader returns
   `Error::CacheIntegrity`** because the cooked archive is
   expected to match the runtime's schema version exactly
   (§4.6 inv. 2 — runtime is read-only; the cooker is the sole
   writer). A schema-version skew between the cooked archive
   and the shipping runtime is a build-system bug, not a
   recoverable state.

This aligns with the SPEC's existing §7.4 rule 3 verbatim and
with `reviews/decisions/hot-reload-protocol.md` §"Refusal Cases"
case 2 (`SchemaMigrationFailed` is reserved for record types
that *do* provide migrations; record types with the
"regenerate" policy never reach that arm).

### 7.6 Schema versioning rules

- **Adding a new field** (e.g. tag 8 = `additional_metadata`):
  additive at `since N+1` with a deterministic synthesised
  default for older payloads. Bumps `DescriptorLayoutRecord`'s
  `schema_source_blake3`; therefore bumps
  `glibre_types_abi_hash`. Old plugins refuse to load. New
  plugins decode old payloads (missing field → default).
- **Removing a field**: marks the tag `reserved`; the codegen
  tool refuses to reuse the tag. Bumps the major schema version
  per `reviews/decisions/fory-codegen.md` §"ABI Stability Rules"
  #3.
- **Re-typing a field**: same as removing + adding; bumps major
  version.
- **Changing the sort order of any list**: a determinism-
  contract change. Bumps a major version because every existing
  cooked archive is layout-incompatible. Triggers a cooker re-
  walk of every project's permutation set.

The `vertex_layout_hash` field added at tag 7 in this design is
treated as `since 1` — the field is being introduced as part of
this design's first landing of the schema, before any payload
ships, so no migration is needed. After the first ship, the
above rules apply.

## 8. Hot-reload integration

The descriptor-layout aggregate is **re-derived alongside
reflection on shader rebuild**. This is the SPEC §8 source-edit
hot-reload contract's projection onto the descriptor-layout
aggregate; nothing here overrides §8. The aggregate's hot-reload
behavior is mechanical and follows from §3.6's identity rule.

### 8.1 Trigger

The descriptor-layout aggregate is re-derived in exactly two
cases:

1. **§8.1 source edit (editor / dev builds).** A `.slang`
   translation unit (or a file in its include closure) changes;
   the affected `(PermutationKey, target)` set is recompiled by
   the §6.2 driver, which re-runs reflection (§6.3) and re-
   derives the descriptor layout (§6.3 row
   `descriptor_layout.cpp`) before publishing the new
   `ShaderArtifact` to the cache (§4.6).
2. **§7.5 schema-version skew (dev / editor builds only).** A
   `DescriptorLayoutRecord` whose serialized `schema_version` is
   older than the runtime's current version is found at
   `ShaderCache::open`; the loader re-derives from the
   surviving reflection per §7.5 above and queues a cooker
   rewrite.

The plugin `.dylib` swap path is **not** a descriptor-layout re-
derive trigger (this matches §8.1 of the SPEC: "The plugin
`.dylib` swap path is not a `shader` hot-reload trigger"). The
§4.5 inv. 1 + §4.8 inv. 4 contract is preserved: runtime never
re-derives.

### 8.2 What survives the re-derive

Per §8.2 of the SPEC ("§8.2 What survives the swap"):

1. **`DescriptorLayout` instances for unaffected permutations
   survive bit-equal.** §3.6's identity rule: byte-equal
   `ShaderHash` ⇒ byte-equal `DescriptorLayout`. An edit that
   doesn't change a permutation's preprocessed source closure
   leaves its hash, its bytecode, its reflection, and its
   layout all bit-equal.
2. **`DescriptorLayout` instances for affected permutations are
   recompiled, not migrated.** The new artifact lives at a new
   `ShaderHash`; the new layout is derived from the new
   reflection. The old layout remains in CAS as part of an
   orphaned blob until cooker GC.
3. **The §6.3 `descriptor_layout.cpp` derivation pipeline
   itself** is part of the `shader.dylib`. If that code
   changes, the change rides on the engine-wide plugin-reload
   protocol (`hot-reload-protocol.md` Mode A), not on the
   source-edit path. After such a swap, the cooker re-walks
   the permutation set and re-derives every layout — this is
   the "schema bumped" case but applied to derivation logic
   rather than schema shape.
4. **`vertex_layout_hash`** (the new tag-7 field, §7.2) is
   recomputed deterministically from the new reflection; the
   same identity rule (§3.6) applies — byte-equal reflection
   ⇒ byte-equal hash.

### 8.3 No `migrate(...)` function

Per §8.3 of the SPEC ("`migrate(...)`: not applicable") and
§7.5 above, the descriptor-layout aggregate **does not provide
a `migrate_DescriptorLayoutRecord_vN_to_vNplus1` function**.
The contract collapses to:

- Persistent records are migrated only when their *schema*
  version bumps; for `DescriptorLayoutRecord`, the policy is
  "regenerate, don't migrate" (§7.5).
- In-memory state to carry across the swap is empty by
  construction. `DescriptorLayout` instances are immutable
  values cached on `ShaderArtifact`; downstream consumers
  (render's #762, #766) hold them by `ShaderHash`. The swap
  replaces the affected hash set; no live `DescriptorLayout`
  instance needs field-by-field reshape.
- The cooker rewrites stale records in dev / editor builds.
  Shipping refuses with `CacheIntegrity` (§7.5 step 4) because
  shipping's cache is read-only by §4.6 inv. 2.

### 8.4 Refusal cases

Re-derive can fail at any of the eight `derive` passes (§3.2,
§3.5). The failure surfaces as the corresponding `shader::Error`
arm; the SPEC §8.4 refusal case 3
("Reflection or descriptor-layout failure on the new bytecode")
catches it: the new artifact never reaches `ShaderCache::insert`,
the prior CAS entry remains live, no `ShaderArtifactReplaced`
event is published.

The refusal contract:

1. **Pass 1 / Pass 2 / Pass 3 / Pass 4 / Pass 5 / Pass 7 / Pass
   8 failure on the affected permutation** — refuse publish for
   that single permutation. The cooker continues with other
   permutations in the affected set (per §8 of the SPEC's
   "per-permutation atomicity, not phase-wide" pattern).
2. **All other affected permutations succeed** — `ShaderCache::
   insert` runs for each of them; `ShaderArtifactReplaced`
   carries only the successful subset; the failed permutation's
   prior hash continues to resolve through `ShaderCache::get`.
3. **Render's #766 PSOCache reaction** — eviction is keyed on
   `affected_old_hashes ∩ successful_new_hashes`; PSOs whose
   permutation refused never get evicted (their old hash is
   still live).

This is the §8.4 refusal contract verbatim, scoped to
descriptor-layout-derive failures. The aggregate adds no new
refusal mechanics beyond what the SPEC already specifies.

### 8.5 Observer notification — passive

The descriptor-layout aggregate publishes **no** observer-bus
events of its own. The §8.5 SPEC event
`glibre::types::shader::ShaderArtifactReplaced` is published by
the `shader` plugin's cache-insert wrapper (§6.4 of the SPEC),
not by the descriptor-layout aggregate. The layout rides inside
each replaced artifact; its presence on the new artifact's
`descriptor_layout` field is the substrate render observers use
to rebind PSOs.

### 8.6 Test hooks

Per §8.6 of the SPEC, `inject_source_diff` and
`recompile_affected` are guarded by `#if defined(GLIBRE_E2E)` and
drive source-change hot-reload deterministically. The
descriptor-layout aggregate exposes no additional test hooks; the
SPEC §11 test plan (`descriptor_layout_derive_partitions_bindings_by_frequency`
and the new tests proposed in §11 below) exercises `derive`
directly with synthesized `ReflectionBlob` fixtures, which is
sufficient to cover every pass and every refusal case without
running the full hot-reload protocol.

## 9. Performance

### 9.1 Quoted from `reviews/decisions/perf-budget.md` and `specs/shader/SPEC.md` §9

| Cell                | Value          | Source                                                                |
|---------------------|----------------|-----------------------------------------------------------------------|
| CPU sim (ms / frame)    | **0.00**   | `perf-budget.md` table row `shader`                                    |
| CPU submit (ms / frame) | **0.00**   | `perf-budget.md` table row `shader`                                    |
| GPU (ms / frame)        | **n/a**    | `shader` runs no GPU work                                              |
| Heap ceiling (context)  | **32 MiB** | `perf-budget.md` allocator-rules contract for `ContextTag::shader`     |
| Phase ownership         | **none**   | `shader` owns no frame phase (1–9)                                     |

The descriptor-layout aggregate participates in the §9.3 row
`DescriptorLayout (§4.5)` of the SPEC: **"derived once per
`(Backend, PermutationKey)` offline; runtime is table lookup;
0.000 ms per frame; counted under ReflectionBlob's 4 MiB"**. This
is reaffirmed and refined here.

### 9.2 Per-aggregate breakdown (refines SPEC §9.3)

| Path                                                                | CPU                                  | Heap                            | Notes                                                                                                  |
|---------------------------------------------------------------------|--------------------------------------|---------------------------------|--------------------------------------------------------------------------------------------------------|
| Per-frame steady state (render / PSO bind / draw)                  | **0.000 ms**                          | 0 MiB (counted under reflection's 4 MiB) | Render reads `ShaderArtifact::descriptor_layout` by `const &`; no derive runs.                |
| Per-program cold derive (cooker / hot-reload re-derive)            | **≤ 0.50 ms wall-clock per derive** on M1 firestorm | ≤ 4 KiB transient | Pass 1..8 are O(N log N) where N = bindings + samplers + vertex elements. Typical N ≈ 30 (≈ 12 bindings + 4 samplers + 8 vertex elements + 6 push-constant ranges); Pass 6's stable_sort is the dominant cost; even at N = 256 the wall-clock stays well under 1 ms. |
| Per-program serialize (cooker → CAS write)                          | **≤ 0.10 ms per artifact**           | bounded by record size (≤ 4 KiB typical) | Fory codegen-emitted serialize for `DescriptorLayoutRecord`.                                  |
| Per-program deserialize (cache load at session init or PSO build)  | **≤ 0.10 ms per artifact**           | bounded by record size           | Fory codegen-emitted deserialize.                                                              |
| Editor inspector walk                                               | **O(N) per inspect-open**             | n/a (shares cache memory)        | Walk `schema().per_*.slots`; no allocation; never on hot path.                                 |

The 0.50 ms cold-derive wall-clock is the per-program transform
budget the issue body mentions ("performance: per-program
transform wall-time budget (cold)"). It is not a CI gate on its
own — the engine-wide cook-time CI gate
(`shader-cook-time-budget` spike, §9.2 of the SPEC) absorbs
descriptor-layout-derive into the total cook wall-clock per
artifact. The ≤ 0.50 ms ceiling is recorded here as a
descriptor-layout-aggregate contract so a regression caused by a
new validation pass (e.g. someone adding an O(N²) check) is
catchable in isolation by the §11 plan-level Catch2 `BENCHMARK`
asserting ≤ 0.50 ms on the synthetic `ReflectionBlob` fixture.

### 9.3 Allocator rules

The descriptor-layout aggregate tags every allocation with
`ContextTag::shader` through `glibre::PerContextAllocator`
(perf-budget.md "Allocator Rules"). The 32 MiB shader-context
ceiling already accounts for resident `ReflectionBlob` records
(SPEC §9.4 row "ReflectionBlob records held by resident
artifacts: 4 MiB; resident, counted") — and "DescriptorLayout
projections of resident reflections" is explicitly noted as
"within the 4 MiB above" in §9.4. The descriptor-layout
aggregate therefore introduces **no new heap pool**; its
resident bytes are counted under the reflection pool.

Cold-path scratch (the Pass 1..8 builder's working storage) is
arena-tagged transient per Allocator Rule #4 of perf-budget.md
("Each context owns a per-frame transient arena (drained at
phase 9) that does **not** count against the cell ceiling").
In the descriptor-layout aggregate's case, the "frame" is
"per-derive call" — the arena is reset at the end of each
`derive` invocation.

Strict-mode (`GLIBRE_ALLOC_STRICT=1`) returns
`std::unexpected{core::Error::OutOfBudget}` if a
`shader`-tagged allocation would push live bytes over 32 MiB
during cold derive; this is a defense-in-depth check unlikely
to ever fire (per-derive heap < 4 KiB).

### 9.4 Cold-start cost

The §9.5 SPEC cold-start cost
(`ShaderLibrary` open + manifest decode ≤ 50 ms) absorbs the
descriptor-layout aggregate's deserialize cost: each resident
artifact's `DescriptorLayoutRecord` is decoded as part of
loading its `ShaderArtifactRecord`. The 50 ms ceiling is
project-wide, not per-artifact; for an MVP-scale archive of
~10 k resident artifacts and an avg ≤ 0.10 ms deserialize per
artifact, descriptor-layout decode contributes ≤ ~1 second of
cold-start budget — most of which is overlapped with disk I/O
inside the Fory loader. The ≤ 0.10 ms per-artifact ceiling is
a per-record contract that ensures the global 50 ms holds in
the worst case.

### 9.5 CI gate

The §9.6 SPEC CI gate adds three shader-specific assertions
(per-frame 0 ms, heap ≤ 32 MiB, cache hit-rate ≥ 99 %). The
descriptor-layout aggregate adds two plan-level Catch2
`BENCHMARK` assertions on top of those (these are unit-test
gates, not workflow gates):

1. **Per-program cold derive ≤ 0.50 ms.** Catch2 `BENCHMARK` in
   `tests/shader/descriptor_layout/perf/derive_bench.cpp` runs
   `DescriptorLayout::derive` against a fixture `ReflectionBlob`
   sized to N = 256 (a worst-case-large permutation) and
   asserts the median wall-clock < 0.50 ms on the M1 baseline.
2. **Per-program serialize / deserialize round-trip ≤ 0.20 ms.**
   `BENCHMARK` covering the Fory round-trip on the same fixture;
   asserts median < 0.20 ms.

These benchmarks are required by `perf-budget.md` §"CI Gate
Spec" #1 ("Every context's SPEC §9 lists at least one micro-
benchmark"); the descriptor-layout aggregate's contribution to
the shader context's overall cook-time budget is recorded
separately under the `shader-cook-time-budget` spike's
`shader-cook-time-budget.yml` workflow.

## 10. Failure modes

The descriptor-layout aggregate maps onto eight `shader::Error`
arms across two categories: **already in `specs/shader/SPEC.md`
§5** (two arms) and **proposed for sub-epic #69's §5 amendment
plan** (four new arms, plus two reused for defense-in-depth).

### 10.1 Severity vocabulary (per `specs/shader/SPEC.md` §10.1)

| Severity | Meaning                                                                 | Cache state after | Observer-bus event |
|----------|-------------------------------------------------------------------------|-------------------|--------------------|
| `refuse` | Derive is rejected before any layout is published.                       | unchanged          | none — `render` keeps binding the prior artifact (§8.4). |

All descriptor-layout-aggregate failures are `refuse`-severity.
The aggregate has no `fallback` or `fatal` arms because the
aggregate is purely transformational — there is no "previous
good descriptor layout" the aggregate falls back to (the prior
layout lives in the prior `ShaderArtifact` in the cache, which
is the cache's responsibility, not this aggregate's), and there
is no invariant whose violation cannot be recovered by re-
cooking from corrected source.

### 10.2 Per-enumerator contract

| Enumerator                                              | Status in §5 | Trigger | Recovery | Severity |
|---------------------------------------------------------|--------------|---------|----------|----------|
| **`DescriptorFrequencyMissing`**                        | already in §5 | Pass 1: a `BindingSlot` arrives without any `DescriptorFrequencyGroup` resolution after the §6.3 tagger ran. | refuse derive; affected permutation's prior CAS entry remains live (§8.4 case 3). | `refuse` |
| **`DescriptorFrequencyAmbiguous`**                      | already in §5 | Pass 2 / Pass 5 (rule 5): a binding is tagged with multiple frequencies, or a sampler appears in both dynamic and immutable classifiers. | refuse derive; same path as above. | `refuse` |
| **`BindingOverflow`** (new arm, sub-epic #69 amendment) | proposed §5 add | Pass 7: `len(table.slots) > 31` for any frequency group (Metal 4 cap). Until the §5 amendment lands, the cooker maps this onto `DescriptorFrequencyAmbiguous` (closest-fit `refuse` arm) so the closed-sum guarantee at the public boundary is preserved. The temporary mapping is unit-tested and deleted when the amendment lands. | refuse derive. | `refuse` |
| **`IncompatibleVertexLayout`** (new arm, sub-epic #69 amendment) | proposed §5 add | Pass 5: per-stage `location` collision in vertex inputs. Temporary mapping per above. | refuse derive. | `refuse` |
| **`SamplerLimitExceeded`** (new arm, sub-epic #69 amendment) | proposed §5 add | Pass 3 / Pass 6 (rule 6): `> 16` static samplers per stage. Temporary mapping per above. | refuse derive. | `refuse` |
| **`PushConstantTooLarge`** (new arm, sub-epic #69 amendment) | proposed §5 add | Pass 4 (rule 7): push-constant total size > 128 bytes. Temporary mapping per above. | refuse derive. | `refuse` |

The four "proposed §5 add" arms are filed alongside spike #79's
`ArtifactSizeExceeded` arm (`specs/shader/SPEC.md` §10.2.1) under
the same sub-epic #69 amendment plan. Until that plan lands the
§5 amendment, the cooker / driver code that detects each
condition **must** map onto the closest-fit existing arm
(`DescriptorFrequencyAmbiguous` for the four arms above) so the
closed-sum guarantee at the public boundary is never violated.
This temporary mapping is unit-tested and deleted when the §5
amendment lands.

### 10.3 Cross-arm refusal interactions

A single `derive` call can encounter exactly one error arm — the
first failure short-circuits the rest of the pipeline (§3.2's
"runs the eight passes in order, short-circuits on the first
`unexpected`"). A `BindingOverflow` is therefore not stacked with
a co-occurring `IncompatibleVertexLayout`; whichever pass detects
its condition first wins, and the §11 unit tests fix the order
deterministically (the order is the §3.2 driver's pass order).

### 10.4 Cross-references

- Closed enum source-of-truth: `specs/shader/SPEC.md` §5 (the
  §10 table here tracks the §5 declaration; both are amended
  together when a new arm lands).
- Hot-reload refusal projection: §8.4 of the SPEC (refusal case
  3 catches every descriptor-layout-derive failure).
- Engine-wide error policy: `reviews/decisions/error-model.md`
  (per-context enums roll into `glibre::Error` variant; logging
  via `glibre::log_error`).
- Sub-epic #69 amendment plan: lands the four new arms above on
  the §5 public boundary alongside spike #79's
  `ArtifactSizeExceeded`.

## 11. Test plan

The descriptor-layout aggregate's test plan composes of:
**(1) eight unit tests covering each `derive` pass / refusal case**;
**(2) three serialization round-trip tests**;
**(3) two performance benchmarks**;
**(4) one integration test covering the full shader pipeline
#745 → #747 → #751 → #753**;
**(5) one cross-context test covering descriptor-layout's seam
into the Metal backend (#762)**.

All tests are Catch2 (`reviews/decisions/error-model.md` cited
the `Catch2::BENCHMARK` block; PHILOSOPHY §4 makes "tests come
from stories"). Test files live under
`tests/shader/descriptor_layout/`.

### 11.1 Unit tests — `derive` passes

| Catch2 test name                                                                        | Pass exercised | Fixture / synthesized input                                                                                          | Expected result                                                                  |
|-----------------------------------------------------------------------------------------|----------------|----------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------|
| `descriptor_layout_derive_partitions_bindings_by_frequency`                             | Passes 1..8 happy path | `ReflectionBlob` with 12 bindings spanning all four frequency groups + 4 static samplers + 6 push-constant ranges + 8 vertex inputs. | Returns `DescriptorLayout` whose `schema()` matches the golden record. |
| `descriptor_layout_derive_rejects_unfrequency_tagged_binding`                            | Pass 1         | Binding with no frequency tag set.                                                                                   | Returns `unexpected(DescriptorFrequencyMissing)`.                                |
| `descriptor_layout_derive_rejects_ambiguous_frequency_tag`                               | Pass 2         | Two bindings with same `(register_space, register_index, stage_mask)` triple in different frequency groups.          | Returns `unexpected(DescriptorFrequencyAmbiguous)`.                              |
| `descriptor_layout_derive_rejects_sampler_in_both_static_and_dynamic`                    | Pass 3 + Pass 5 (rule 5) | Sampler tagged immutable and also placed in `PerMaterial` table.                                                | Returns `unexpected(DescriptorFrequencyAmbiguous)`.                              |
| `descriptor_layout_derive_rejects_too_many_static_samplers`                              | Pass 3 (rule 6) | 17 static samplers all visible to `Pixel` stage.                                                                    | Returns `unexpected(SamplerLimitExceeded)` (or the temporary `DescriptorFrequencyAmbiguous` mapping until §5 amendment lands). |
| `descriptor_layout_derive_rejects_push_constant_size_overflow`                           | Pass 4 (rule 7) | Push-constant ranges summing to 192 bytes.                                                                          | Returns `unexpected(PushConstantTooLarge)` (with temporary mapping fallback).    |
| `descriptor_layout_derive_lowers_push_constants_into_per_draw_slot_zero`                 | Pass 4 happy path | 2 push-constant ranges, total 64 bytes.                                                                            | `schema().per_draw.slots[0]` carries `kind=PushConstant`, `register_space=0xFF`, `register_index=0xFF`; `schema().push_constants` preserves the original ranges. |
| `descriptor_layout_derive_rejects_vertex_location_collision_per_stage`                   | Pass 5 (rule 8) | Two `VertexInputElement`s with same `location`, both visible to `Vertex` stage.                                     | Returns `unexpected(IncompatibleVertexLayout)` (with temporary mapping fallback). |
| `descriptor_layout_derive_assigns_argument_buffer_slot_index_by_register_tuple`          | Pass 6         | 5 bindings in `PerMaterial` group with shuffled `(register_space, register_index)` order.                           | `schema().per_material.slots` is sorted; slot ordinals match `(0, 1, 2, 3, 4)` mapping to sorted register tuples. |
| `descriptor_layout_derive_rejects_too_many_bindings_in_one_frequency_group`              | Pass 7 (rule 4) | 32 bindings in `PerDraw` group.                                                                                     | Returns `unexpected(BindingOverflow)` (with temporary mapping fallback).         |
| `descriptor_layout_derive_assigns_static_sampler_ordinal_by_register_tuple`              | Pass 6 (sampler side table) | 4 static samplers with shuffled `(register_space, register_index)`.                                          | `schema().static_samplers` sorted; ordinals (0..3) match the sorted tuples.      |
| `descriptor_layout_derive_computes_stable_vertex_layout_hash`                            | Pass 5 hash    | Two reflection inputs differing only in vector container insertion order; same logical vertex elements.            | Both yield byte-equal `DescriptorLayoutRecord.vertex_layout_hash`.               |

### 11.2 Unit tests — equality + identity

| Catch2 test name                                                              | Property                                                                                                                                  |
|-------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------|
| `descriptor_layout_equal_for_byte_equal_reflection`                           | §3.6 identity rule: byte-equal `ReflectionBlob` ⇒ byte-equal `DescriptorLayout`.                                                          |
| `descriptor_layout_inequal_for_different_register_tuples`                     | A reflection differing in one `(register_space, register_index)` tuple yields a distinct layout.                                          |
| `descriptor_layout_table_accessor_returns_partition_invariant`                | `table(group)` returns the same reference as the corresponding `schema().per_*` field; iteration is consistent.                           |

### 11.3 Serialization round-trip tests

| Catch2 test name                                                              | Property                                                                                                                                  |
|-------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------|
| `descriptor_layout_record_roundtrip_byte_equal`                               | `Fory::deserialize(Fory::serialize(layout)) == layout` byte-equal across hosts and runs (§7.3 rule 3).                                     |
| `descriptor_layout_record_roundtrip_preserves_vertex_layout_hash`             | The recomputed `vertex_layout_hash` on the deserialized record equals the original.                                                       |
| `descriptor_layout_record_schema_abi_hash_changes_on_field_addition`          | Adding a tag-8 field at `since 2` bumps `glibre_types_abi_hash`; old plugins refuse to load (`PluginAbiHashMismatch`).                    |

### 11.4 Performance benchmarks

| Catch2 test name                                                              | Assertion                                                                                                                                  |
|-------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------|
| `descriptor_layout_derive_under_half_ms_at_n_256`                              | Median wall-clock < 0.50 ms on the M1 baseline for a `ReflectionBlob` with N = 256 bindings + samplers + vertex elements.                  |
| `descriptor_layout_record_serialize_deserialize_under_two_hundred_us_at_n_256` | Median round-trip wall-clock < 0.20 ms on the M1 baseline.                                                                                |

### 11.5 Integration test — full pipeline #745 → #747 → #751 → #753

| Catch2 test name                                                              | Pipeline path                                                                                                                              |
|-------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------|
| `shader_pipeline_full_round_trip_yields_consistent_descriptor_layout`         | (1) Open a fixture `.slang` source via `ShaderSource::open` (#745). (2) Resolve a `PermutationKey` via the codegen-emitted enumerator (#747). (3) Compile + reflect via `IShaderBackend::compile` + `reflect` (#751 reflection-aggregate path). (4) Derive `DescriptorLayout` (this aggregate, #753). Assert: every binding has exactly one frequency tag; the `vertex_layout_hash` matches a recorded golden; the `RootSignatureSchema` is byte-equal to the recorded golden. |

This test is `#if !GLIBRE_SHIPPING`-guarded because it links the
`shader.dylib`'s offline frontend (compile path); shipping
binaries cannot run it (§4.3 inv. 3).

### 11.6 Cross-context test — Metal backend seam (#762)

| Catch2 test name                                                              | Assertion                                                                                                                                 |
|-------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------|
| `metal_backend_translates_descriptor_layout_to_argument_buffer_descriptors`   | Given a fixture `DescriptorLayout`, the #762 Metal backend's translator produces an `MTLArgumentDescriptor[]` whose count matches `schema().per_*.slots.size()` for each group; ordinals match vector indices; static samplers map onto a separate table. (Test owned by render's #762 plan but referenced here so the seam stays exercised.) |

The cross-context test is filed in `tests/render/metal/` under
the #762 plan, not in `tests/shader/`; it is referenced here so
the plan-level review keeps the seam in mind.

### 11.7 Test fixtures

A single fixture file
`tests/shader/descriptor_layout/fixtures/standard_pbr.json`
holds a hand-authored `ReflectionBlob` representing a typical
`Standard` shading-model + `Forward` render-path + `Desktop`
LOD-tier permutation. The fixture is JSON-backed (loaded into
the in-memory `ReflectionBlob` via a small test helper) for
human readability; it is **not** Fory-serialized — that path is
exercised by the §11.3 round-trip tests, not by fixture loading.

A second fixture
`tests/shader/descriptor_layout/fixtures/oversized_n256.json`
provides a synthetic N = 256 reflection for the §11.4 benchmarks.

## 12. Open questions

Tracked for resolution during implementation (each becomes a
`type:spike` if it cannot be absorbed by an existing section):

- [OPEN] **Should `RootSignatureSchema` expose
  `vertex_layout_hash` on the public §5 surface?** This design
  computes and serializes the hash but the public C++ accessor
  is deferred until the sub-epic #69 amendment plan lands the
  four new error arms. The geometry context's vertex-stream
  catalog (#115) is the consumer that needs the hash; resolving
  this question requires checking whether geometry can read
  the deserialized record directly (preferred) or needs the
  hash via the in-memory aggregate (which would force the §5
  amendment). Owner: sub-epic #69 amendment plan.
- [OPEN] **Should the four new error arms (`BindingOverflow`,
  `IncompatibleVertexLayout`, `SamplerLimitExceeded`,
  `PushConstantTooLarge`) land in §5 in a single amendment with
  spike #79's `ArtifactSizeExceeded`, or as separate
  amendments?** A single amendment is cheaper to review; separate
  amendments are easier to revert. Owner: sub-epic #69
  amendment plan.
- [OPEN] **Is the 31-binding-per-frequency cap a per-stage cap
  or a total cap?** Metal 4's argument-buffer cap on Apple
  Silicon is 31 entries per *argument buffer*; an argument
  buffer is per-stage in the simplest schema but can be shared
  across stages with stage-mask qualifiers. The conservative
  reading (per-stage, 31 max) is what this design uses; the
  liberal reading (per-argument-buffer regardless of stages)
  could double the effective cap when bindings are visible to
  exactly one stage. Resolution: a one-day measurement spike
  against Metal 4 sample code on the M1 baseline. Owner: filed
  as a follow-up spike under sub-epic #744 if the conservative
  cap turns out to be over-tight in practice.
- [OPEN] **Is the 128-byte push-constant ceiling Metal-tight
  or D3D12-tight?** Metal has no native push-constant primitive,
  so the ceiling is purely a glibre-imposed soft cap on the
  synthetic `PerDraw` slot 0. D3D12's root-32-bit-constants cap
  is 256 dwords (1024 bytes) — far higher. The 128-byte choice
  leaves headroom for the rest of the `PerDraw` argument
  buffer's contents. If post-MVP profiling shows shaders
  routinely want more, the ceiling is bumped via amendment.
  Owner: post-MVP profiling, not blocking MVP.
- [OPEN] **Should the synthetic push-constant slot's
  `register_space = 0xFF, register_index = 0xFF` sentinel be
  replaced with a strongly-typed `BindingKind::PushConstant` +
  a separate `is_synthetic` bool?** The sentinel approach is
  simple and matches existing reflection-emitter conventions;
  the strongly-typed approach is more self-documenting but
  requires an additional `BindingSlot` field that breaks the
  POD shape. Owner: §5 amendment plan; lean toward keeping the
  sentinel.
- [OPEN] **What's the right home for the `vertex_layout_hash`
  cross-check against geometry's mesh-layout catalog (#115)?**
  Two options: (a) descriptor-layout exposes the hash and
  geometry reads it; (b) a separate mesh-shader-compatibility
  aggregate cross-checks at draw build. Option (a) is simpler;
  option (b) is more SRP-pure. Owner: defer to whichever spike
  lands the geometry vertex-stream catalog (#115); the hash is
  always computed and serialized either way.
- [OPEN] **Does the descriptor-layout aggregate need to
  surface a `Capabilities`-style refusal arm when the active
  `IShaderBackend::capabilities()` doesn't advertise a feature
  the layout requires (e.g. `AccelerationStructure` binding kind
  but `capabilities().ray_tracing == false`)?** The §5
  `CapabilityNotSupported` arm exists at the `compile` level
  (§10.2 of the SPEC); the layout could add a defense-in-depth
  re-check. Resolution: probably yes, but as a Pass-9
  capability-validation pass introduced by a follow-up plan;
  not in MVP scope. Owner: sub-epic #744 follow-up.
