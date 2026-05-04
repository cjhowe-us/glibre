# shader — Detailed Design: shader-reflection aggregate

> Detailed design for the `ReflectionBlob` value object and its
> producing ingester (the `reflection/` subtree of the `shader`
> plugin) declared in `specs/shader/SPEC.md` §4.4 and §6.3. Refines
> §4.4 (aggregate boundary, three invariants), §4.7 (`reflect`
> operation on `IShaderBackend`), §4.8 (cross-aggregate invariant
> 2 — reflection paired with bytecode), §5 (the public `ReflectionBlob`
> struct + `BindingSlot` / `VertexIOLayout` / `PushConstantRange` /
> `MaterialParameterBlock` / `SpecializationConstantSlot` / `EntryPoint`
> records), §6.3 (the slangc-reflection ingester pipeline), §7.1 + §7.2
> + §7.4 (`ReflectionRecord` Fory schema, embedded in
> `ShaderArtifactRecord`), §8.2 (cache survival rules), §8.4 (refusal
> case 3), §9.3 (per-aggregate breakdown row), §10.2 (the four
> reflection arms `ReflectionExtractionFailed`,
> `DescriptorFrequencyAmbiguous`, `DescriptorFrequencyMissing`,
> `EntryPointMissing`) of the SPEC in place. Cites
> `reviews/decisions/error-model.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/fory-codegen.md`, and
> `reviews/decisions/frame-phases.md`. Sibling siblings on `main`:
> `specs/data/reflection-blob-design.md` (a different subject — the
> editor descriptor table for Fory schemas; SRP fence in §1 below),
> `specs/data/schema-registry-design.md`. All conclusions independently
> re-derived; harmonius prior art (`harmonius/docs/requirements/
> rendering/gpu-abstraction-layer.md` R-2.1.16/.17/.18,
> `harmonius/docs/design/rendering/render-pipeline.md` §"Descriptor
> Layout Inference" + §RF-9, `harmonius/docs/design/rendering/
> pipeline-state-cache.md` R-2.3.9.2/.8) cited as research input
> only — every clause evaluated against PHILOSOPHY §6 (no runtime
> reflection in shipping builds — narrowed for this aggregate per
> SPEC §6.5: the *ingester* ships, but it never re-extracts; it
> only round-trips a previously-cooked `ReflectionRecord`) and §7
> (determinism) before being kept, narrowed, or refused.

Refs: spike #751 — `[SPIKE] design-shader-shader-reflection-detailed`.
Parent sub-epic #744. Sibling task-breakdown spike blocked-by this
deliverable. Feeds descriptor-layout #753 + the render-graph PSO
assembly story.

## 1. Purpose

The `shader-reflection` aggregate is the **build-time / editor-time
ingester** for one compiled Slang program. Its single responsibility
is to **translate the slangc-emitted native reflection record paired
with the bytecode of one `ShaderArtifact` into a normalized,
deterministic, POD-shaped `ReflectionBlob` keyed by
`(ShaderHash, PermutationKey)`** (SPEC §4.4 inv. 1, §4.8 inv. 2).

Concretely the aggregate owns, per artifact:

1. The list of `EntryPoint` records (name + stage), one per
   `[shader("...")]`-tagged Slang function in the compiled module
   (§3.1).
2. The flat list of `BindingSlot` records — every reflected resource
   binding (constant buffer, sampled image, storage image, sampler,
   structured buffer, RW structured buffer, acceleration structure,
   push constant) — each tagged with exactly one
   `DescriptorFrequencyGroup` (§3.2, §3.3).
3. The `VertexIOLayout` — the semantic + format-code list for every
   vertex-stage input element (§3.4).
4. The `PushConstantRange` list — `(offset, size, stage_mask)` rows
   for every push-constant block declared by any entry point
   (§3.5).
5. The `MaterialParameterBlock` — the named scalar/vector member
   list of the `MaterialParameters` cbuffer the engine standardizes
   on (§3.6).
6. The `SpecializationConstantSlot` list — every
   `[SpecializationConstant]`-marked Slang constant the entry-point
   set links against (§3.7).
7. The `rt_payload_bytes` scalar — the largest ray-tracing payload
   size required by any RT entry point in the module (§3.8).

The aggregate's output is a value object: structurally-equal byte
input produces structurally-equal `ReflectionBlob`, modulo only the
canonicalization elided by the `slangc_argv_builder` upstream
(SPEC §4.4 inv. 2; this design § 4.3).

What the aggregate **explicitly refuses to own**:

- **Bytecode-container parsing.** Per SPEC §4.4 inv. 1 and §3
  collapse 4, the reflection record is a paired output of one slangc
  subprocess invocation, not a re-derivation from `metallib` /
  `DXIL` bytes. The ingester reads slangc's native reflection
  artifact (a JSON document the driver §6.2 captures alongside the
  bytecode); it never opens a `metallib` container or a `DXIL` blob.
  A future "round-trip from bytecode alone" path is refused (§12
  [OPEN] #1 — explicitly out of MVP scope).
- **Backend-native descriptor-table layout.** Projecting a
  `ReflectionBlob` onto the four-frequency `RootSignatureSchema`
  the engine consumes belongs to `DescriptorLayout::derive` (SPEC
  §4.5; sibling design `specs/shader/descriptor-layout-design.md`,
  spike #753 — blocked-by this deliverable). The ingester emits
  `BindingSlot` records sorted by `(register_space, register_index,
  stage_mask)` (per §4.5 inv. 3); it does **not** partition them.
  The fence is sharp: descriptor-layout consumes a `ReflectionBlob`,
  it does not extend one.
- **PSO assembly.** Building Metal `MTLRenderPipelineState` /
  `MTLComputePipelineState` (or the future D3D12/Vulkan equivalents)
  from `(bytecode, ReflectionBlob, DescriptorLayout, render-state-
  hash)` is `render`'s territory (`specs/render/SPEC.md` §4.1.7
  `PSOCache`). The reflection aggregate is one of the inputs to
  that build; it owns no PSO state.
- **Runtime reflection.** Per PHILOSOPHY §6 and SPEC §4.8 inv. 3,
  the engine ships only pre-cooked artifacts in shipping builds.
  The ingester *ships* (SPEC §6.5: `reflection/` is included in
  the shipping plugin) so that the (rare) editor / asset-pipe build
  embedded in a non-shipping configuration can re-ingest from a
  bundled artifact, **but the shipping read path never invokes the
  ingester** — it deserializes a pre-cooked `ReflectionRecord` from
  the CAS via `glibre-types.dylib`'s Fory machinery (§6 below).
  Live extraction at runtime is refused.
- **Slang source parsing.** The ingester's input is the slangc-
  produced reflection record; it never re-parses `.slang` source
  text. Source-level concerns (entry-point scanning, include closure)
  belong to `ShaderSource` (SPEC §4.1).
- **Compile-flag canonicalization.** The slangc argv normalization
  that feeds the cache key (SPEC §4.3 inv. 4) belongs to
  `slangc_argv_builder` in `backend/slang/`. The ingester reads
  whatever reflection record slangc produces under those canonical
  flags; it does not influence them.
- **Slang-schema-registry-style FQN catalog.** The sibling design
  `specs/data/reflection-blob-design.md` defines an editor-only
  descriptor-table over Fory schemas. That `ReflectionBlob` is a
  `data` aggregate keyed by `(SchemaId, SchemaVersion)`; it has
  zero overlap with the shader `ReflectionBlob` defined here, which
  is keyed by `(ShaderHash, PermutationKey)` and produced by slangc.
  The two share a name across bounded contexts because each is the
  natural English term inside its own context — neither is
  type-equivalent, neither imports the other's header, and their
  Fory schemas live under disjoint namespaces (`glibre.data.*`
  vs. `glibre.shader.*`).
- **`MaterialParameterBlock` content-type validation.** The
  ingester normalizes the `MaterialParameters` cbuffer's reflected
  scalar/vector members into `BindingSlot`-shaped rows (§3.6); it
  does **not** validate that the engine-side `Material` struct
  binds compatible C++ types. That cross-context check belongs to
  the `material` context's authoring pipeline.
- **Ray-tracing payload struct layout.** Only the maximum payload
  byte count is reflected (§3.8); the per-field layout of the
  payload struct is not extracted in MVP. The render-side RT
  pipeline builder allocates the conservative `rt_payload_bytes`
  upper bound; per-field reflection of payloads is post-MVP
  (§12 [OPEN] #2).

The SRP boundary is sharp by construction: if the **shape of the
slangc reflection record changes** (new binding kinds slangc starts
emitting, an upgrade in the JSON schema slangc writes, a frequency-
annotation grammar revision), this design changes. If anything else
changes — Slang source grammar, slangc CLI flags, descriptor-table
projection rules, PSO cache keys, render-graph topology — this design
does not.

## 2. Requirements coverage

Mapping of harmonius requirements / design clauses cited in SPEC §3
and SPEC §4.4 / §4.8 invariants to glibre MVP coverage. Every entry
is independently re-derived against PHILOSOPHY §6 (no runtime
reflection in shipping) and §7 (determinism). The table is the
contract: every harmonius clause and every SPEC invariant either
lands in this design or is refused with a one-line rationale.

| Source                                                                                             | Glibre disposition (MVP)            | Coverage site                                                                                                                                                                                                                                                            |
|----------------------------------------------------------------------------------------------------|-------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Harmonius `gpu-abstraction-layer.md` R-2.1.16 — four descriptor-frequency groups                   | **Re-derived (verbatim)**           | §3.3 — `DescriptorFrequencyGroup` enum keeps `PerFrame / PerPass / PerMaterial / PerDraw` unchanged; tagger consumes Slang `[frequency(...)]` annotations and falls back to register-space conventions (§3.3 deterministic mapping table).                              |
| Harmonius `gpu-abstraction-layer.md` R-2.1.17 — Slang → metallib via slangc CLI subprocess         | **Inherited (upstream of this aggregate)** | The compilation pipeline (SPEC §4.3) owns invocation; this aggregate consumes the slangc-emitted reflection JSON. SPEC §4.4 inv. 1, §4.8 inv. 2 hardened: ingester only accepts records *paired* with a `ShaderArtifact` from the same subprocess invocation.            |
| Harmonius `gpu-abstraction-layer.md` R-2.1.18 — structured errors at every public boundary         | **Covered**                         | §4 surface returns `std::expected<ReflectionBlob, shader::Error>`; the four reflection-relevant arms (`ReflectionExtractionFailed`, `DescriptorFrequencyAmbiguous`, `DescriptorFrequencyMissing`, `EntryPointMissing`) are pinned by SPEC §10.2 and elaborated in §10. |
| Harmonius `gpu-abstraction.md` GR-2 / GR-4 — render owns push-constant + binding caches            | **Refused (correctly — render's job)** | This aggregate emits `PushConstantRange` and `BindingSlot` records; it does not cache writes. SPEC §3 refusal 4 + §4.4 SRP confirm the boundary.                                                                                                                          |
| Harmonius `render-pipeline.md` §"Descriptor Layout Inference" — descriptor layout from reflection  | **Re-derived (split)**              | The *inference rule* (frequency tagger) is owned here (§3.3); the *projection onto a four-frequency table* is owned by `DescriptorLayout::derive` (SPEC §4.5; sibling spike #753).                                                                                       |
| Harmonius `render-pipeline.md` §RF-9 — hot-reload re-runs reflection on new bytecode               | **Covered**                         | §8 — the reflection re-extraction trigger is the source-change reload (SPEC §8.1); the affected `(PermutationKey, target)` set is recompiled and the ingester runs once per affected artifact.                                                                            |
| Harmonius `pipeline-state-cache.md` R-2.3.9.2 / R-2.3.9.8 — descriptor layout inferred *once* and cached | **Covered**                         | §5 hot/cold split — extraction is build-time only; runtime path deserializes a pre-cooked `ReflectionRecord` from the CAS. SPEC §4.5 inv. 1 forbids runtime re-derivation; this design strengthens the rule for the ingester upstream.                                |
| Harmonius `pipeline-state-cache.md` R-2.3.9.2 — PSO key composes shader hash + device fingerprint  | **Refused (PSO is render's domain)** | The reflection aggregate emits inputs; the PSO key composition lives in `render::PSOCache` (`specs/render/SPEC.md` §4.1.7).                                                                                                                                              |
| SPEC §4.4 inv. 1 — `ReflectionBlob` produced **only** from slangc native reflection paired with bytecode | **Covered**                         | §4 public surface only accepts `(const ShaderArtifact&)`, whose construction is gated by the §6.2 driver. §11.1 unit test `reflection_ingester_refuses_unpaired_input` enforces.                                                                                          |
| SPEC §4.4 inv. 2 — same bytecode → structurally-equal `ReflectionBlob` (deterministic)             | **Covered**                         | §3.9 ordering rules; §4.3 canonicalization + sort steps; §11.1 round-trip test `reflection_blob_is_structurally_equal_for_same_bytecode`.                                                                                                                              |
| SPEC §4.4 inv. 3 — every binding tagged with exactly one `DescriptorFrequencyGroup`                | **Covered**                         | §3.3 tagger; §10 refusal arms `DescriptorFrequencyAmbiguous` / `DescriptorFrequencyMissing`; §11.1 unit tests `reflection_ingester_rejects_multi_frequency_binding` and `reflection_ingester_rejects_untagged_binding`.                                                  |
| SPEC §4.7 op `reflect` — `(ShaderArtifact) → Result<ReflectionBlob>` on `IShaderBackend`           | **Covered**                         | §4 public surface; the `SlangBackend::reflect` impl delegates to the `reflection/` ingester; build-time path only (the trait method survives in shipping per SPEC §5, but the shipping ingester only re-extracts from a paired artifact never produced in shipping). |
| SPEC §4.8 inv. 2 — reflection paired with bytecode at slangc invocation time                       | **Covered**                         | §4.2 contract: ingester input includes a paired `(bytecode_hash, reflection_json_bytes)` pair both produced by one `glibre-shadercc` invocation; mismatched pair → `ReflectionExtractionFailed{detail="unpaired"}`.                                                       |
| SPEC §5 `ReflectionBlob` struct                                                                    | **Refined**                         | §3.10 restates the struct verbatim, pins per-field provenance rules (which slangc reflection field maps to which §5 column), and adds the ingester-internal `ExtractionContext` POD.                                                                                    |
| SPEC §5 `BindingSlot` / `VertexInputElement` / `PushConstantRange` / `MaterialParameterBlock` / `SpecializationConstantSlot` / `EntryPoint` | **Refined**                         | §3.2 / §3.4 / §3.5 / §3.6 / §3.7 / §3.1 — each gets a one-paragraph provenance + ordering rule.                                                                                                                                                                          |
| SPEC §6.3 reflection ingester pipeline                                                             | **Refined**                         | §4 — module composition: `slangc_reflection_ingester.cpp` (raw record reader) → `frequency_tagger.cpp` (DescriptorFrequencyGroup assignment) → ingester returns `ReflectionBlob`. The `descriptor_layout.cpp` step listed in SPEC §6.3 belongs to spike #753. |
| SPEC §7.1 `ReflectionRecord` schema                                                                | **Covered**                         | §7.2 — the Fory schema embeds-by-value into `ShaderArtifactRecord`; tag layout pinned in `data/schemas/shader/ReflectionRecord.fory` per SPEC §7.2.                                                                                                                       |
| SPEC §7.4 rules 2 + 6 — additive evolution of `ReflectionRecord`; schema migrations at hot-reload  | **Covered**                         | §7.3 — additive-only fields land at fresh tags `since N+1` with deterministic synth defaults; migration providers live in `glibre::shader::migrate::ReflectionRecord_vN_to_vNplus1` per `fory-codegen.md` §"Migration Mechanic".                                          |
| SPEC §8.2 rule 1 — unaffected `ReflectionBlob`s survive bit-equal across source-change reload      | **Covered**                         | §8.1 — survival is by content-addressed CAS; the ingester is invoked only on the *affected* `(PermutationKey, target)` set.                                                                                                                                              |
| SPEC §8.4 refusal case 3 — reflection / descriptor-layout failure leaves prior artifact bound      | **Covered**                         | §10 — every reflection refusal arm (`ReflectionExtractionFailed`, `DescriptorFrequencyAmbiguous`, `DescriptorFrequencyMissing`) refuses publish; `ShaderCache::insert` is not called; prior CAS entry remains live.                                                       |
| SPEC §9.3 row "ReflectionBlob"                                                                     | **Covered**                         | §9 — runtime hot-path cost is **0 ms / frame**; cold-start cost is bounded as the ingester does not run in shipping; build-time wall-clock budget per program is in §9.2.                                                                                                |
| SPEC §10.2 four reflection-relevant arms                                                           | **Covered**                         | §10 — full per-arm contract (trigger, recovery, severity, ingester-internal detection point).                                                                                                                                                                            |
| `reviews/decisions/error-model.md` `std::expected<T, glibre::Error>` boundary                      | **Covered**                         | §4 surface; the `shader::Error` enum surface rolls into `glibre::Error` via the variant alias; ingester is `noexcept` and returns `std::expected<ReflectionBlob, shader::Error>` directly.                                                                                |
| `reviews/decisions/perf-budget.md` `shader` row 0 ms / frame, 32 MiB heap                          | **Covered**                         | §9.1 — per-frame contribution is 0 ms (ingester not on the hot path); the 4 MiB sub-pool of the 32 MiB tag is the resident `ReflectionBlob` set's accounting cell.                                                                                                       |
| `reviews/decisions/plugin-abi.md` middleman-only ABI surface                                       | **Covered**                         | §7.4 — the `ReflectionRecord` Fory schema is a `glibre.shader.*` schema in `glibre-types.dylib`; the ABI hash absorbs its source bytes per the standard rule.                                                                                                            |
| `reviews/decisions/hot-reload-protocol.md` plugin reload protocol                                  | **Covered (orthogonal)**            | §8 — shader-source reload (SPEC §8) is the trigger that re-runs the ingester; plugin-loader reload (`hot-reload-protocol.md`) re-runs the ingester only if the `shader.dylib` itself reloaded with a code change to `reflection/`.                                       |
| `reviews/decisions/fory-codegen.md` migration mechanic                                             | **Covered**                         | §7.3 — additive-only `ReflectionRecord` evolution; per-bump migration provider follows the `migrate_<Type>_vN_to_vNplus1` shape verbatim.                                                                                                                                |
| `reviews/decisions/frame-phases.md` phase ownership                                                | **Refused (none)**                  | The `shader-reflection` aggregate owns no frame phase. Per SPEC §9.1 phase-ownership row, `shader` participates in zero of the nine phases; this aggregate is build-time only.                                                                                          |

Coverage rule: every harmonius clause and every SPEC invariant either
lands in this design or is refused with a one-line rationale. No
silent drops.

Glibre-native obligations beyond harmonius:

- **Pairing oracle.** Harmonius's reflection clauses left the
  pairing-with-bytecode rule implicit. Glibre makes it a first-class
  invariant: the ingester refuses any record whose paired
  `bytecode_hash` does not byte-equal the `ShaderArtifact::hash`'s
  source-side input (SPEC §4.4 inv. 1, §4.8 inv. 2; this design
  §4.2). Detected without running slangc; surfaced as
  `ReflectionExtractionFailed`.
- **Deterministic ordering at every level.** Harmonius implied
  ordered tables; this design pins three sort orders (entry points
  by `(stage, name)`; `BindingSlot`s by `(register_space,
  register_index, stage_mask)`; spec constants by `id`) so
  ingesting the same record twice yields a byte-equal Fory payload
  (§3.9; §4.3 canonical-walk rule).
- **Slang `[frequency(...)]` annotation as the primary tag source.**
  The frequency tagger (§3.3) consumes an *engine-standardized*
  Slang attribute the editor enforces at authoring time, falling
  back to register-space conventions only for legacy / external
  Slang modules (a deterministic, table-driven fallback — never a
  heuristic).
- **`MaterialParameters` cbuffer is a recognized standard.** The
  Slang authoring contract names exactly one cbuffer
  `MaterialParameters` whose reflected member layout is folded into
  the `MaterialParameterBlock` value (§3.6). Other cbuffers reflect
  as ordinary `BindingSlot` rows; no other cbuffer is silently
  promoted to the parameter-block slot. Documented as a Slang
  authoring convention in this design's glossary so the constraint
  is checkable at codegen time.

## 3. Detailed model

### 3.1 `EntryPoint` records

The ingester walks slangc's reflection record to find every function
the slangc front-end identified as an entry point (`[shader(...)]`-
tagged) and emits one `EntryPoint` per record:

```cpp
namespace glibre::shader {

struct EntryPoint {
    eastl::string name;        // Slang function name, UTF-8
    Stage         stage{Stage::Vertex};
    // (no payload beyond name + stage; equality is structural)
};

}  // namespace glibre::shader
```

Provenance: `slang::IEntryPoint::getName()` →
`EntryPoint::name`; the `[shader(...)]` tag string maps onto the
`Stage` enum via the table in §3.10. The list is sorted ascending by
`(stage_ordinal, name)` so two structurally-equal modules produce
byte-equal `EntryPoint` arrays (§3.9 invariant).

If the artifact's cached `EntryPoint` set (carried on
`ShaderArtifact::reflection.entry_points`) differs from the set the
ingester re-derives — for example, slangc re-wrote a stage tag
between invocations — the ingester refuses with
`EntryPointMissing` (one of the two `EntryPointMissing` triggers
listed in §10).

### 3.2 `BindingSlot` records

Every reflected resource binding lifts into a `BindingSlot`:

```cpp
struct BindingSlot {
    BindingKind              kind{BindingKind::ConstantBuffer};
    std::uint32_t            register_space{0};
    std::uint32_t            register_index{0};
    std::uint32_t            array_size{1};
    StageMask                stages{};         // bitfield over Stage enum
    DescriptorFrequencyGroup frequency{DescriptorFrequencyGroup::PerDraw};
    eastl::string              name;
};
```

`BindingKind` is a closed sum the ingester switches on (SPEC §5):

| BindingKind            | Slang reflection category source                                          |
|------------------------|---------------------------------------------------------------------------|
| `ConstantBuffer`       | `slang::TypeReflection::Kind::ConstantBuffer`                             |
| `SampledImage`         | `slang::TypeReflection::Kind::Resource` with shape `Texture*` + read      |
| `StorageImage`         | `slang::TypeReflection::Kind::Resource` with shape `Texture*` + read/write |
| `Sampler`              | `slang::TypeReflection::Kind::SamplerState`                               |
| `StructuredBuffer`     | `slang::TypeReflection::Kind::Resource` shape `StructuredBuffer`, read    |
| `RWStructuredBuffer`   | `slang::TypeReflection::Kind::Resource` shape `StructuredBuffer`, RW      |
| `AccelerationStructure`| `slang::TypeReflection::Kind::Resource` shape `AccelerationStructure`     |
| `PushConstant`         | `slang::TypeReflection::ParameterCategory::PushConstantBuffer`            |

Anything that doesn't fall into the table above is the `TypeUnsupported`
condition: the ingester refuses with `ReflectionExtractionFailed{detail=
"unsupported_kind:<slangkind>"}` (§10). This is intentional — adding a
kind requires a SPEC §5 amendment plus a sibling design refresh
(§12 [OPEN] #3).

`register_space` / `register_index` come from
`slang::VariableLayoutReflection::getBindingSpace()` and
`getBindingIndex()`. `array_size` is `getElementCount()` for array
parameters; scalar parameters get `array_size = 1` (never zero —
zero-element arrays in Slang surface as an extraction failure).

`stages` is a `u8` bitfield with one bit per `Stage` enumerator
(SPEC §5):

| Stage          | Bit |
|----------------|-----|
| Vertex         | 0   |
| Pixel          | 1   |
| Compute        | 2   |
| Mesh           | 3   |
| Amplification  | 4   |
| Library        | 5   |

A binding shared across multiple entry points has multiple bits set;
a binding used only by `vertex` is `0b000001`. The bit-packing is
fixed (not derived from the enum value at runtime) so the on-disk
`bytes`-size is bit-stable across hosts.

The duplicate-binding rule: two `BindingSlot` rows that compare
equal under `(kind, register_space, register_index, name)` but
differ in `stages` are **merged** by OR-ing their `stages` masks;
the ingester emits one row. Two rows that compare equal under
`(register_space, register_index)` but differ in any other field
are the `BindingDuplicated` condition: the ingester refuses with
`ReflectionExtractionFailed{detail="duplicate_binding:space=N,index=M"}`
(§10).

### 3.3 Frequency tagging — `frequency_tagger`

Every `BindingSlot` must end up with **exactly one**
`DescriptorFrequencyGroup` (SPEC §4.4 inv. 3). The tagger is a
deterministic three-stage pipeline; each stage is a pure function
of its input.

**Stage A — explicit Slang annotation (`[frequency(...)]`).** The
engine standardizes one Slang attribute the editor enforces at
authoring time:

```slang
[frequency("PerMaterial")]
ConstantBuffer<MaterialParameters> g_material;
```

Recognized values (closed): `"PerFrame"`, `"PerPass"`, `"PerMaterial"`,
`"PerDraw"`. Anything else → `DescriptorFrequencyAmbiguous` (§10).
The tagger reads the attribute via `slang::VariableLayoutReflection::
findUserAttributeByName("frequency")`. If the attribute is present,
the binding's frequency is fixed; the next two stages are skipped
for that row.

**Stage B — `register_space` convention fallback.** For modules
without explicit annotations (legacy / external Slang), the tagger
maps `register_space` to `DescriptorFrequencyGroup`:

| `register_space` | DescriptorFrequencyGroup |
|------------------|--------------------------|
| 0                | `PerFrame`               |
| 1                | `PerPass`                |
| 2                | `PerMaterial`            |
| 3                | `PerDraw`                |
| ≥ 4              | `DescriptorFrequencyAmbiguous` |

The 0..=3 mapping is the engine-wide convention pinned in the
authoring guidelines. `register_space ≥ 4` is reserved for backend-
extension use; a binding declared there without an explicit
`[frequency(...)]` is ambiguous by definition.

**Stage C — refusal.** Any binding that exits stages A and B without
a frequency tag is `DescriptorFrequencyMissing` (§10). This is
distinct from "ambiguous" (multiple candidate values resolved during
stage A's attribute parse): "missing" means *no* signal, "ambiguous"
means *contradictory* signals.

Stages A and B share one canonical decision table; the table is a
`constexpr` array in `frequency_tagger.cpp`. There is no heuristic
fallback; there is no per-binding override file.

**Push-constant frequency.** `BindingKind::PushConstant` rows skip
the tagger entirely and are forced to `PerDraw`. SPEC §4.5 partition
inv. 2 still holds — push constants live in the `PerDraw` table by
construction. Push-constant range mechanics are §3.5 below.

**`MaterialParameters` cbuffer.** A `ConstantBuffer<MaterialParameters>`
binding whose declared variable name is exactly `g_material` (the
engine's Slang authoring convention) is forced to `PerMaterial`
regardless of stage A / stage B output, *and* its members are folded
into `MaterialParameterBlock` (§3.6). A non-standard name on the
`MaterialParameters` type triggers no special handling; it reflects
as an ordinary cbuffer binding.

### 3.4 `VertexIOLayout`

The vertex stage's input semantics list:

```cpp
struct VertexInputElement {
    eastl::string semantic;        // e.g. "POSITION", "TEXCOORD"
    std::uint32_t semantic_index{0};
    std::uint32_t location{0};
    std::uint32_t format_code{0};   // backend-neutral format ordinal
};

struct VertexIOLayout {
    eastl::vector<VertexInputElement> elements;
};
```

Provenance: walks the vertex `[shader("vertex")]` entry point's
parameter list. Each parameter with a Slang semantic annotation
(`POSITION`, `NORMAL`, `TEXCOORDn`, `COLORn`, `TANGENT`, `BITANGENT`,
`PSIZE`, custom `SV_*` semantics) lifts into a `VertexInputElement`.
Compute / mesh / amplification / library entry points contribute
nothing to `VertexIOLayout`; their `VertexIOLayout::elements` is
empty (a module with only compute entry points has an empty
`vertex_io`).

`format_code` is a backend-neutral ordinal pinned in
`shader/include/glibre/shader/_format_codes.hpp` (a `constexpr
eastl::array` that the ingester switches on by Slang scalar/vector type
+ component count). The mapping table is closed (~32 entries
covering the formats slangc actually emits); unmapped Slang scalar
types fail with `ReflectionExtractionFailed{detail=
"unsupported_format:<slang_type>"}`.

`location` comes from
`slang::VariableLayoutReflection::getBindingIndex()` for the vertex-
input parameter category; `semantic_index` is the trailing integer
of the Slang semantic name (`TEXCOORD3` → `("TEXCOORD", 3)`).

Sort order: ascending by `(location, semantic, semantic_index)`.
Ties on `(location, semantic)` are an extraction error
(`ReflectionExtractionFailed{detail="vertex_input_collision"}`).

There is exactly one vertex entry point per `ShaderArtifact` in MVP
(SPEC §4.1 inv. 1: each entry point has exactly one stage tag, and
the cooker invokes one `(ShaderSource, PermutationKey, CompileTarget)`
per artifact); a module with two `[shader("vertex")]` functions
would already have failed at compile time as
`EntryPointStageAmbiguous`. The ingester defends in depth: it
refuses with `EntryPointMissing` (multi-vertex variant) if the
reflection record carries two vertex entry points.

### 3.5 `PushConstantRange` list

Push-constant blocks reflect as one `PushConstantRange` per
contiguous byte range:

```cpp
struct PushConstantRange {
    std::uint32_t offset{0};
    std::uint32_t size{0};
    StageMask     stages{};
};
```

Provenance:
`slang::VariableLayoutReflection::getOffset(SLANG_PARAMETER_CATEGORY_PUSH_CONSTANT_BUFFER)`
gives `offset`; `slang::TypeLayoutReflection::getSize(...)` gives
`size`. `stages` is the bit-OR of every entry point that references
the block — same merge rule as `BindingSlot::stages` (§3.2).

Sort order: ascending by `(offset, size)`. Two ranges that overlap
(`r1.offset < r2.offset + r2.size && r2.offset < r1.offset + r1.size`)
are an extraction error
(`ReflectionExtractionFailed{detail="push_constant_overlap"}`).

The Metal 4 backend uses argument buffers for what other backends
call push constants; the ingester normalizes Slang's
`SLANG_PARAMETER_CATEGORY_PUSH_CONSTANT_BUFFER` category onto this
shape regardless of backend so the `PushConstantRange` list is
backend-neutral. The backend-specific binding (Metal: argument
buffer slot; D3D12: root constants) is `render`'s problem.

### 3.6 `MaterialParameterBlock`

A single named cbuffer the engine standardizes on:

```cpp
struct MaterialParameterBlock {
    eastl::string              name;          // always "MaterialParameters"
    std::uint32_t            size_bytes{0}; // total cbuffer footprint
    eastl::vector<BindingSlot> members;       // members reflected as named slots
};
```

Triggered by a `ConstantBuffer<MaterialParameters>` parameter named
`g_material` (§3.3 standard). The ingester walks the Slang struct
type's field list and emits one `BindingSlot` per scalar / vector
member. Each member's `register_space` is set to
`std::numeric_limits<std::uint32_t>::max()` (a sentinel: members
have no register binding of their own; they live inside the parent
cbuffer's binding slot), `register_index` is the byte offset of
the member, `array_size` is the member's element count, `stages`
is the parent cbuffer's `stages`, and `frequency` is forced to
`PerMaterial`.

A module with no `MaterialParameters` cbuffer emits an empty
`MaterialParameterBlock` (`name == "MaterialParameters"`,
`size_bytes == 0`, `members` empty) — the field is required for
schema stability (SPEC §7.2 `ReflectionRecord` tag 5) but its
content is optional content-wise.

A module with a `MaterialParameters` cbuffer whose declared variable
name is *not* `g_material` is treated as an ordinary cbuffer
(per §3.3) and `MaterialParameterBlock` stays empty. This is by
design: the engine's Slang authoring convention is the only path
into the parameter-block slot.

### 3.7 `SpecializationConstantSlot` list

Slang specialization constants reflect as:

```cpp
struct SpecializationConstantSlot {
    eastl::string   name;
    std::uint32_t id{0};
    std::uint32_t size_bytes{0};
};
```

Provenance: every Slang `[SpecializationConstant(id=N)]` declaration
that the entry-point set links against. `id` is the integer
parameter; `size_bytes` is `slang::TypeLayoutReflection::getSize(...)`
on the declared type.

Sort order: ascending by `id`. Two distinct `name`s sharing one
`id` is `ReflectionExtractionFailed{detail="spec_const_id_collision"}`.

Specialization constants the link step references but the reflection
record does not declare are `SpecializationConstantMissing`
(SPEC §10.2 enumerator) — surfaced by the **link** path, not by
the ingester. The ingester emits the declaration list verbatim;
the link step's job is to bind values.

### 3.8 `rt_payload_bytes`

A single `u32` carrying the maximum ray-tracing payload size in
bytes across every RT-capable entry point in the module. Computed
as `max(0, max_over_RT_entry_points(slang_rt_payload_size))`. A
module with no RT entry points emits `rt_payload_bytes = 0` (§7.2
default).

Per-field reflection of the RT payload struct itself is **not** in
MVP (§12 [OPEN] #2). The render-side RT pipeline allocates a
conservative buffer of `rt_payload_bytes`; per-field decoding is
done in shader code, not by the ingester.

### 3.9 Determinism — the canonical walk

The ingester is a pure function of its input (the slangc reflection
JSON bytes) modulo a one-line canonicalization step. Concretely, two
ingester runs over the same input bytes produce byte-equal output
*after* the following ordering rules apply:

| Vector / list                              | Sort key                                            |
|--------------------------------------------|-----------------------------------------------------|
| `ReflectionBlob::entry_points`             | ascending `(stage_ordinal, name)`                   |
| `ReflectionBlob::bindings`                 | ascending `(register_space, register_index, stage_mask)` |
| `ReflectionBlob::push_constants`           | ascending `(offset, size)`                          |
| `ReflectionBlob::spec_constants`           | ascending `id`                                      |
| `MaterialParameterBlock::members`          | ascending `register_index` (the byte offset)        |
| `VertexIOLayout::elements`                 | ascending `(location, semantic, semantic_index)`    |

Sorts are stable; on equal keys, input order wins (which is itself
deterministic because slangc emits a canonical traversal order, but
the ingester does not depend on that — the explicit sort keys above
are the contract).

Strings are compared **byte-wise as UTF-8** (`memcmp` semantics);
no locale-aware ordering is consulted. Numbers compare as unsigned
integers. The canonical walk is the only post-processing step
between "raw slangc reflection" and "byte-stable
`ReflectionRecord`".

### 3.10 The aggregate root struct

`ReflectionBlob` is the value object the rest of the engine sees
(SPEC §5; copied here for one-stop reference):

```cpp
struct ReflectionBlob {
    eastl::vector<EntryPoint>                 entry_points;
    eastl::vector<BindingSlot>                bindings;        // tag-1 frequency
    VertexIOLayout                          vertex_io;
    eastl::vector<PushConstantRange>          push_constants;
    MaterialParameterBlock                  material_parameters;
    eastl::vector<SpecializationConstantSlot> spec_constants;
    std::uint32_t                           rt_payload_bytes{0};
};
```

The struct is a POD aggregate (SPEC §5; `eastl::vector` and
`eastl::string` are not strictly trivially-copyable, but the
**Fory-serialized `ReflectionRecord`** *is* the byte-stable form —
the in-memory C++ value is allocator-tagged
`ContextTag::shader` and lives only in editor / cook builds; the
shipping read path holds either a const reference into a memory-
mapped record or a deserialized `ReflectionBlob` whose lifetime is
the resident `ShaderArtifact`'s).

The `Stage` enum table (cross-referenced by §3.1):

| Slang `[shader(...)]` tag | `Stage` enumerator |
|---------------------------|--------------------|
| `"vertex"`                | `Vertex`           |
| `"pixel"` / `"fragment"`  | `Pixel`            |
| `"compute"`               | `Compute`          |
| `"mesh"`                  | `Mesh`             |
| `"amplification"`         | `Amplification`    |
| `"raygeneration"` / `"closesthit"` / `"anyhit"` / `"miss"` / `"intersection"` / `"callable"` | `Library` |

The Library tag covers every RT stage in MVP — render-graph
configuration distinguishes the specific RT stage downstream.
Distinct `Stage` arms for RT shaders are post-MVP (§12 [OPEN] #4).

## 4. Public surface

The reflection aggregate's **only** public symbol is the `reflect`
operation on `IShaderBackend` (SPEC §4.7, §5):

```cpp
namespace glibre::shader {

class IShaderBackend {
public:
    // (other operations elided; see SPEC §5 for the full trait)

    [[nodiscard]] virtual std::expected<ReflectionBlob, Error>
    reflect(const ShaderArtifact&) = 0;
};

}  // namespace glibre::shader
```

`SlangBackend::reflect` (in `backend/slang/slang_backend.cpp`)
delegates to the ingester:

```cpp
namespace glibre::shader::reflection {

// Pure function. Not part of the public boundary; internal to the
// shader plugin. Called from SlangBackend::reflect and from the
// hot-reload path's recompile_affected helper.
[[nodiscard]] std::expected<ReflectionBlob, Error>
ingest(const ShaderArtifact& artifact) noexcept;

}  // namespace glibre::shader::reflection
```

Boundary rules:

1. **`std::expected` only.** Both the public `reflect` virtual and
   the internal `ingest` return `std::expected<ReflectionBlob,
   shader::Error>`. No exceptions; no out-parameters; no
   sentinel values. (`error-model.md`.)
2. **`noexcept` ingester.** The ingester is a pure POD-walking
   function with bounded allocation (a single `eastl::vector` per
   output column, sized from the slangc record). It catches no
   exceptions because none of its inputs throw — the slangc
   reflection JSON parser is itself an in-tree allocator-aware
   parser (`tools/shadercc/` ships the JSON producer, the shader
   plugin's `cache/blake3.hpp/.cpp` siblings ship the consumer; the
   parser is `noexcept`).
3. **Input-pairing contract.** The ingester accepts only a
   `const ShaderArtifact&` whose `bytecode_hash` (a derived prefix
   of `ShaderHash`) equals the `bytecode_hash` field embedded in
   the `slangc-reflection` record. Mismatch → `ReflectionExtractionFailed`
   with `detail="unpaired_bytecode_hash"`. This is the §4.4 inv. 1
   defense-in-depth (SPEC §4.8 inv. 2); it makes "reflection paired
   with bytecode" a checkable invariant rather than a build-time
   convention.
4. **No POD across the plugin ABI.** `ReflectionBlob` carries
   `eastl::vector` / `eastl::string` and so cannot cross the C-ABI
   plugin boundary. The render-side consumer
   (`render::PSOCache` build path) reaches `ReflectionBlob` records
   *through `glibre-types.dylib`* — i.e. by deserializing the
   `ReflectionRecord` Fory schema (§7) from CAS bytes into a fresh
   in-memory `ReflectionBlob` allocated under the consumer's tag.
   The shader plugin never hands a `ReflectionBlob*` to render
   directly. (`plugin-abi.md` §"plugins must not link
   `glibre-core` or any other plugin"; in-tree code never crosses
   plugin dylibs by raw pointer.)
5. **No virtual `ingest`.** The ingester is a free function in
   `glibre::shader::reflection`; it is **not** a virtual method on
   any plugin trait. Different source-language families (a future
   non-Slang backend, §12 [OPEN] #5) would ship their own
   ingester *and* their own backend-specific reflection record;
   they would not implement `glibre::shader::reflection::ingest`
   itself.

### 4.1 Internal module composition

The `reflection/` subtree (SPEC §6.1) decomposes:

```text
plugins/shader/src/reflection/
    slangc_reflection_ingester.hpp/.cpp
        // Reads the JSON bytes that glibre-shadercc emits paired
        // with the bytecode (§6.2 driver contract). Lifts each entry
        // point, binding, vertex-input element, push-constant range,
        // sampler binding, and RT payload size into the
        // canonical BindingSlot-shaped temporaries.
    frequency_tagger.hpp/.cpp
        // Maps each BindingSlot to one DescriptorFrequencyGroup.
        // Three-stage pipeline (§3.3); refuses ambiguity / missing.
    reflection_blob.cpp
        // The SRP-bounded entry: reads slangc record, runs ingester,
        // runs frequency tagger, applies the canonical sort (§3.9),
        // returns a ReflectionBlob.
```

`descriptor_layout.cpp` (SPEC §6.1) is **not** part of this
aggregate; it consumes a `ReflectionBlob` and emits a
`DescriptorLayout`, owned by sibling design #753.

### 4.2 Pairing contract — `ShaderArtifact` shape

The ingester's input is a `ShaderArtifact` (SPEC §5):

```cpp
struct ShaderArtifact {
    PermutationKey         key{};
    CompileTarget          target{CompileTarget::MetalLib};
    ShaderHash             hash{};
    eastl::vector<std::byte> bytecode;
    ReflectionBlob         reflection;       // *populated by* ingester
    DescriptorLayout       descriptor_layout;
};
```

Construction order (cooker §6.4 step 2):

1. `IShaderBackend::compile(source, k, t)` → returns
   `ShaderArtifact{key=k, target=t, hash=h, bytecode=b,
   reflection={}, descriptor_layout={}}` along with a side-channel
   `slangc_reflection_json` bytestring captured by the
   `glibre-shadercc` driver (§6.2).
2. `IShaderBackend::reflect(artifact)` → ingester runs over the
   side-channel JSON, producing the `ReflectionBlob`, which is
   assigned into `artifact.reflection`.
3. `DescriptorLayout::derive(artifact.reflection)` → produces the
   `DescriptorLayout`, assigned into `artifact.descriptor_layout`.
4. `ShaderCache::insert(artifact)` → the now-fully-populated
   artifact is serialized into `ShaderArtifactRecord` (SPEC §7.1)
   and committed to CAS.

The "side-channel JSON" is not a public type; it lives strictly
inside the `shader` plugin between steps 1 and 2. The ingester is
the only consumer.

### 4.3 Ingester invariants on the public surface

1. **Determinism.** `ingest(a) == ingest(a)` byte-equal for any `a`
   whose `bytecode_hash` and side-channel JSON are byte-equal.
   Tested by `reflection_blob_is_structurally_equal_for_same_bytecode`
   (§11).
2. **Totality on valid input.** Any input that passes the §4.2
   pairing oracle and represents a Slang program slangc accepted
   produces a `ReflectionBlob`; refusal is reserved for genuine
   structural defects (`ReflectionExtractionFailed`,
   `DescriptorFrequencyAmbiguous`, `DescriptorFrequencyMissing`,
   `EntryPointMissing`).
3. **Pure.** No I/O, no global state, no clock, no PRNG. Allocates
   only through `glibre::PerContextAllocator{ContextTag::shader}`.
4. **Bounded allocation.** Per-call peak heap is `O(record_size)`;
   the JSON parser is streaming so peak input residency is also
   bounded. §9 budget gives a hard ceiling.
5. **No reach into sibling aggregates.** The ingester reads only
   `ShaderArtifact` and the side-channel JSON; it does not call
   `ShaderCache::lookup`, does not build a `DescriptorLayout`, does
   not consult `ShaderSource`, and does not invoke the slangc
   subprocess.

## 5. Hot/cold path split

The aggregate has exactly one runtime profile in shipping (cold,
zero work) and one in editor / cook (warm, build-time). Per
PHILOSOPHY §6 and SPEC §4.8 inv. 3, runtime reflection extraction
is forbidden in shipping; this design's shipping reach is bounded
to a Fory-deserialize over a CAS-resident byte buffer.

| Phase                                  | Build profile          | Cost                                                  | Path                                                                  |
|----------------------------------------|------------------------|-------------------------------------------------------|-----------------------------------------------------------------------|
| Cold (shipping startup)                | shipping               | ~50 ms wall-clock total for `ShaderLibrary` open (§9.5 of SPEC) | `ShaderCache::open` mmaps `manifest.fory`; `ReflectionRecord` not parsed eagerly |
| Cold (shipping per-artifact resolve)   | shipping               | ~5 µs per `ShaderCache::get` (§9.3 of SPEC)            | Fory-deserialize `ReflectionRecord` into `ReflectionBlob` on demand    |
| Warm steady-state (shipping per-frame) | shipping               | **0 ms** (§9.1 of SPEC)                                 | No ingester, no deserialize; `render::PSOCache` absorbs queries        |
| Cold (cook / editor)                   | tools / dev            | budget §9.2: ≤ 5 ms per artifact (typical: ~0.5 ms)    | `ingest(artifact)` runs once per cooked permutation                    |
| Hot-reload trigger (editor)            | dev                    | re-runs cooker for affected set; budget §9.2 applies    | `inject_source_diff` → recompile → reflect → derive → insert            |

Hot-path freedom rules (steady-state):

1. **No ingester invocation in shipping.** SPEC §6.5 includes
   `reflection/` in the shipping plugin as defense-in-depth so an
   editor build embedded in a non-shipping host can re-ingest, but
   the shipping `runtime` binary's `IShaderBackend::reflect` is never
   called: the runtime read path is `ShaderCache::Library::get` →
   pre-cooked `ShaderArtifactRecord` (which contains a pre-cooked
   `ReflectionRecord` already). A shipping caller that reaches
   `IShaderBackend::reflect` is itself a defect (PHILOSOPHY §6
   violation); the call still works (the ingester is in the link),
   but the per-frame budget gate (SPEC §9.6 #1) catches the
   regression.
2. **No global state.** The ingester carries no module-scope
   variables; it is invoked through the `IShaderBackend` trait
   instance whose only state is the slangc binary path
   (build-time only). Multiple programs can be ingested in
   parallel (§6).
3. **No frame-phase ownership.** `shader-reflection` participates in
   none of the nine frame phases (`reviews/decisions/frame-phases.md`
   `shader` row). The aggregate's only timing reference is build-time
   wall clock.

The runtime `ReflectionBlob` access pattern in editor / dev builds:
on a hot-reload (SPEC §8), the `inject_source_diff` test hook (or
the dev-watcher) drives `recompile_affected`, which runs the
ingester once per affected `(PermutationKey, target)`; the resulting
`ReflectionBlob` is committed to CAS via `ShaderCache::insert`,
publishing a `ShaderArtifactReplaced` event. Subscribers
(`render::PSOCache`) then deserialize the new `ReflectionRecord`
through `glibre-types.dylib`'s codegen-emitted reader on cache miss.
The ingester does **not** publish `ReflectionBlob` instances
directly; everything crosses the plugin boundary as Fory-serialized
bytes.

## 6. Concurrency

The ingester is a **pure function over a `const ShaderArtifact&`
plus the side-channel JSON bytes**. Concurrency rules:

1. **Re-entrant per artifact.** Two threads invoking
   `glibre::shader::reflection::ingest(a1)` and
   `ingest(a2)` over distinct artifacts share no state. Both calls
   complete independently. The cooker (§6.4) parallelizes the
   permutation walk across `glibre-shadercc` subprocess workers;
   each worker runs the ingester on its own artifact serially with
   its own compile.
2. **No shared mutable state.** The frequency-tagger's lookup
   table (§3.3 stage B) is a `constexpr` array; the format-code
   table (§3.4) is a `constexpr` array; the `Stage` mapping
   (§3.10) is a `constexpr` array. No singletons, no caches.
3. **Allocator thread-safety.** Allocation is through
   `glibre::PerContextAllocator{ContextTag::shader}`. The
   per-context allocator is documented in
   `reviews/decisions/perf-budget.md` "Allocator Rules" #1; it is
   thread-safe under the standard glibre allocator rules
   (`PerContextAllocator` operations carry their own internal
   synchronization or use thread-local arenas — implementation
   detail of the alloc plan, not this design).
4. **No locks.** The ingester takes no mutex; the slangc reflection
   JSON parser is allocator-aware and stack-only for parse state.
5. **Pairing oracle is local.** §4.2's `bytecode_hash` check is a
   memcmp against a local field; it does not consult any shared
   table.

The cooker's parallel saturation budget (M1 firestorm/icestorm
cores) is tracked under the `shader-cook-time-budget` spike (SPEC
§9.2); this design only commits that the ingester is parallelism-
safe per artifact.

The hot-reload path runs the ingester serially over the affected
set inside `recompile_affected` (SPEC §8.6). Parallelizing the
hot-reload ingest is post-MVP (§12 [OPEN] #6).

The `IShaderBackend::reflect` virtual itself is invoked from one
thread at a time per artifact (the cooker worker); cross-thread
sharing of a `ReflectionBlob` after construction is fine because
the value is immutable (`const`-aliased post-return; the `ingest`
call is the only writer of the value's storage, and it owns it
exclusively until the move-out).

## 7. Persistence + ABI

### 7.1 Persistence boundary

The reflection aggregate **does not persist** in its own right; it
is persisted as a value member of `ShaderArtifactRecord` (SPEC §7.1)
through the standalone Fory schema `ReflectionRecord` (SPEC §7.2,
`data/schemas/shader/ReflectionRecord.fory`). The on-disk path is:

```text
ShaderArtifactRecord (SPEC §7.2)
├── key             : PermutationKeyRecord  tag 1
├── target          : u8                    tag 2
├── source_hash     : bytes32               tag 3
├── flags_hash      : bytes32               tag 4
├── artifact_hash   : bytes32               tag 5
├── bytecode        : bytes                 tag 6
├── reflection      : ReflectionRecord      tag 7  ← this aggregate's slot
├── descriptors     : DescriptorLayoutRecord tag 8
└── producer_label  : string                tag 9
```

The `ReflectionRecord` schema (SPEC §7.2 verbatim, repeated for
SRP-locality):

```fory
schema glibre.shader.ReflectionRecord {
  version  1
  since    "0.1.0"

  field entry_points         : list<glibre.shader.EntryPointRecord>          tag 1 since 1
  field bindings             : list<glibre.shader.BindingSlotRecord>         tag 2 since 1
  field vertex_io            : glibre.shader.VertexIOLayoutRecord            tag 3 since 1
  field push_constants       : list<glibre.shader.PushConstantRangeRecord>   tag 4 since 1
  field material_parameters  : glibre.shader.MaterialParameterBlockRecord    tag 5 since 1
  field spec_constants       : list<glibre.shader.SpecializationConstantSlotRecord> tag 6 since 1
  field rt_payload_bytes     : u32                                           tag 7 since 1
}
```

Sub-schemas (`EntryPointRecord`, `BindingSlotRecord`,
`VertexIOLayoutRecord`, `PushConstantRangeRecord`,
`MaterialParameterBlockRecord`,
`SpecializationConstantSlotRecord`) are by-value embeddings; their
tag layouts are listed in SPEC §7.1 and re-emitted by codegen
without further customization here.

### 7.2 Serialize / deserialize round-trip

`glibre-foryc` emits, for every `glibre.shader.*` schema, the typed
trampolines `serialize` / `deserialize` per
`reviews/decisions/fory-codegen.md`. The reflection aggregate piggy-
backs on this; it does **not** ship its own serialize / deserialize
code path. The in-memory `ReflectionBlob` (SPEC §5) and the
generated `glibre::types::shader::ReflectionRecord` POD are
field-equivalent under the codegen's tag-sorted layout rule
(`fory-codegen.md` §"ABI Stability Rules" #1):

```cpp
namespace glibre::shader::reflection::persist {

// Convert the in-memory aggregate value into the codegen middleman
// POD. Layout-equivalent under the tag-sorted rule; this is a
// memberwise copy emitted by codegen, not an ad-hoc transform.
glibre::types::shader::ReflectionRecord
to_record(const ReflectionBlob&);

// Inverse. Allocates EASTL strings/vectors under
// PerContextAllocator{ContextTag::shader}.
ReflectionBlob
from_record(const glibre::types::shader::ReflectionRecord&);

}  // namespace glibre::shader::reflection::persist
```

Round-trip equality (`from_record(to_record(b)) == b` for any well-
formed `b`) is the §11 `reflection_record_round_trip_is_identity`
test. The §3.9 canonical walk guarantees byte-equal Fory output for
byte-equal input.

### 7.3 Migration (additive evolution)

`ReflectionRecord` evolves under the engine-wide additive-only
rule (`fory-codegen.md` §"Migration Mechanic"):

1. **Additive fields land at fresh tags `since N+1`.** Example:
   adding RT payload per-field reflection (§12 [OPEN] #2) would
   add tag 8 `rt_payload_layout : list<RtPayloadFieldRecord> tag 8
   since 2` with deterministic synth default `[]` for older
   payloads.
2. **Removing or re-typing a field requires a major version bump
   plus a `migrate_ReflectionRecord_vN_to_vNplus1` provider** (per
   `fory-codegen.md` §"Migration Mechanic"). Removed fields are
   marked `reserved` rather than reusing tag numbers.
3. **Additive enumerator growth on `BindingKind` / `Stage`.** A new
   binding kind (e.g. a future Slang resource type) bumps the
   enum's cardinality but keeps the tag layout. Older payloads
   decode unchanged because the existing enumerators retain their
   ordinal; newer payloads encountered by older hosts return
   `Error::ReflectionExtractionFailed{detail="unsupported_kind:
   <new_kind>"}` (refusal-driven, PHILOSOPHY §8).
4. **No automatic cross-version transformation of bytes.**
   `ShaderArtifactRecord.bytecode` is never reshaped by a
   reflection-record migration; if a migration would require
   touching `bytecode`, the design choice is to recompile from
   source (SPEC §7.4 rule 5).
5. **Hot-reload-time refusal.** A `ReflectionRecord` whose stored
   version has no chain to the host's current version causes
   `core::Error::SchemaMigrationFailed` per
   `hot-reload-protocol.md`; the cooked archive is rejected with
   `shader::Error::CacheIntegrity` (SPEC §10 mapping).

Every shipped `vN_to_vNplus1` transform owns a Catch2 golden under
`tests/shader/persistence/`; goldens are committed alongside the
migration provider (§11).

### 7.4 ABI hash composition

`ReflectionRecord`'s schema source bytes (the
`data/schemas/shader/ReflectionRecord.fory` file) feed into the
single blake3 digest the middleman exposes via
`glibre_types_abi_hash()` (SPEC `plugin-abi.md` §"ABI Hash
Function" #1). Specifically:

1. `glibre-foryc` blake3-hashes the canonical bytes of
   `ReflectionRecord.fory` (SPEC §7.3 canonicalization rule).
2. The resulting `(fqn, version_le, schema_source_blake3)` triple
   for `glibre.shader.ReflectionRecord` joins the sorted catalog.
3. The catalog is hashed once into `glibre_types_abi_hash`; every
   plugin compiled against a host with a different
   `ReflectionRecord` schema source hash refuses to load with
   `core::Error::PluginAbiHashMismatch` (`plugin-abi.md`
   §"Plugin Manifest Schema").
4. **A `ReflectionRecord` schema bump is therefore an ABI bump.**
   Adding tag 8 (additive) keeps SONAME stable but bumps
   `glibre_types_abi_hash`; removing a tag (breaking) requires a
   SONAME bump per `plugin-abi.md` §"Versioning Rules" #2.

The reflection aggregate contributes no ABI surface beyond what
`ReflectionRecord`'s schema source provides. The in-memory
`ReflectionBlob` is *not* ABI: it is an internal type of the
shader plugin, not crossed by any plugin boundary (§4 rule 4).

### 7.5 Manifest contract

The `ShaderCacheManifest`'s `schema_abi_hash` field (SPEC §7.2)
captures `glibre_types_abi_hash()` at cook time. A manifest whose
`schema_abi_hash` differs from the host's current
`glibre_types_abi_hash()` is rejected with
`shader::Error::CacheIntegrity` at `ShaderCache::open`. Together
with the dynamic linker's SONAME check, this catches both
schema-additive (hash differs, SONAME stable) and schema-breaking
(SONAME differs) drift. The reflection aggregate inherits this
gate without contributing additional bytes.

## 8. Hot-reload

### 8.1 Refresh trigger

The reflection aggregate has exactly one re-extraction trigger,
inherited from SPEC §8: a Slang source change observed by the
editor's filesystem watcher (or by the `inject_source_diff` test
hook) on a file in some `ShaderSource`'s preprocessed include
closure. The watcher recomputes `PreprocessedSource.total_hash`;
on hash change, the affected
`(ShaderSource, PermutationKey, CompileTarget)` set is recompiled,
and the ingester runs once per affected artifact:

```text
inject_source_diff(source_id, new_bytes) ──► recompile_affected:
    for each (key, target) in affected_set:
        artifact := IShaderBackend::compile(source, key, target)
        artifact.reflection := IShaderBackend::reflect(artifact)   ← ingester
        artifact.descriptor_layout := DescriptorLayout::derive(artifact.reflection)
        ShaderCache::insert(artifact)
    publish ShaderArtifactReplaced{
        affected_old_hashes, affected_new_hashes, affected_permutations, target}
```

Unaffected permutations (those whose preprocessed closure does not
include the modified source) survive bit-equal (SPEC §8.2 rule 1):
the ingester is **not** invoked for them, their cached
`ReflectionBlob` remains the live one, and `render::PSOCache` does
not evict the corresponding PSO.

### 8.2 What survives

Per SPEC §8.2, applied to this aggregate:

| State                                    | Survival                                                              |
|------------------------------------------|-----------------------------------------------------------------------|
| `ReflectionBlob` of unaffected artifacts | bit-equal across reload (CAS file unchanged, `ReflectionRecord` byte-equal) |
| `ReflectionBlob` of affected artifacts   | replaced by re-extraction; old blob orphaned in CAS until cooker GC   |
| Frequency-tagger constexpr tables        | unchanged (compile-time data; survives any reload)                    |
| `ReflectionRecord` Fory schema           | unchanged (a schema bump is an engine-wide event, not a source-edit)  |
| `glibre_types_abi_hash`                  | unchanged (a schema bump path is the only one that changes it)        |

The reflection aggregate has no "live" in-memory state across the
shader-source-reload swap because:

1. The shader plugin's `.dylib` does not reload on a source edit
   (SPEC §8.1: source reload is content-only, not loader-driven).
2. The ingester is stateless (§6 rules 1-3).
3. Resident `ReflectionBlob` instances inside `ShaderArtifact`
   values held by `render::PSOCache` are immutable; they get
   evicted-and-replaced by hash, not migrated in place
   (SPEC §8.2 rule 5).

### 8.3 Refusal during hot-reload

A re-extraction that fails (any of `ReflectionExtractionFailed`,
`DescriptorFrequencyAmbiguous`, `DescriptorFrequencyMissing`,
`EntryPointMissing`) leaves the prior cached `ReflectionRecord`
authoritative (SPEC §8.4 refusal case 3); the new artifact is
**not** inserted into CAS, and `ShaderArtifactReplaced` is **not**
published. `render::PSOCache` therefore continues to bind the prior
`ReflectionBlob` deterministically, and the editor surfaces the
refusal via the structured `warn` log per
`reviews/decisions/error-model.md` §"Logging / Telemetry".

### 8.4 No `migrate(...)` for source-change

Per SPEC §8.3, source-change reload does not call any `migrate(...)`
function. The reflection aggregate has no `migrate_ReflectionBlob`
hook; the only migration path is the engine-wide schema-version
bump (§7.3) that ships
`migrate_ReflectionRecord_vN_to_vNplus1` providers when the
`ReflectionRecord` schema itself evolves.

### 8.5 Plugin-loader reload (orthogonal)

When the `shader.dylib` reloads under the engine-wide loader
protocol (`hot-reload-protocol.md`) — i.e. because the
`reflection/` source code itself changed — the four-step state
machine (drain → swap → migrate → resume) runs. The reflection
aggregate's contribution:

- **Drain**: nothing to drain. The ingester holds no in-flight
  state across calls (§6); any in-flight `ingest()` invocation
  finishes before drain returns (the cooker worker thread completes
  its current artifact).
- **Swap**: the new `shader.dylib` brings a new ingester
  implementation; the old one is unloaded. CAS-resident
  `ReflectionRecord` bytes are not affected (they live in
  `glibre-types.dylib`'s namespace, not in `shader.dylib`).
- **Migrate**: only triggered if `ReflectionRecord`'s schema
  bumped (which would also have bumped
  `glibre_types_abi_hash()`); the migration runs through
  `glibre-types.dylib`'s migration table per
  `fory-codegen.md` §"Migration Mechanic". The shader plugin
  contributes the migration provider's body; the loader invokes it.
- **Resume**: the new shader plugin's `glibre_plugin_register`
  re-acquires no ingester state (there is none). Subsequent
  `IShaderBackend::reflect` calls use the new ingester.

## 9. Performance

### 9.1 Runtime per-frame budget (engine contract)

Per `reviews/decisions/perf-budget.md` `shader` row and SPEC §9.1:

| Cell                          | Value     | Source                               |
|-------------------------------|-----------|--------------------------------------|
| CPU sim per frame             | **0.00 ms** | `perf-budget.md` `shader` row        |
| CPU submit per frame          | **0.00 ms** | `perf-budget.md` `shader` row        |
| GPU per frame                 | n/a       | reflection aggregate runs no GPU work |
| Aggregate hot-set heap        | **4 MiB** | sub-pool of SPEC §9.4 (under `ContextTag::shader`'s 32 MiB ceiling) |
| Frame phase                   | **none**  | aggregate participates in zero phases |

The 4 MiB cell is the resident `ReflectionBlob` set's accounting
slot (SPEC §9.3 row "ReflectionBlob"). Per §5 hot/cold split, the
ingester is not on the per-frame hot path; the 0 ms / frame cost is
by construction.

### 9.2 Build-time wall-clock per program (cold)

Cook-time budgets are explicitly out of the frame-budget contract
(SPEC §9.2) and tracked under `shader-cook-time-budget`. This
design pins the per-program ingest budget for that spike's cook-
time gates:

| Ingest stage                                               | Budget per program (M1 firestorm) | Rationale                                                                 |
|------------------------------------------------------------|-----------------------------------|---------------------------------------------------------------------------|
| `slangc_reflection_ingester` JSON parse                     | ≤ 2 ms                            | Streaming parser; typical reflection record is ~2 KiB JSON for a 16-binding program. |
| `frequency_tagger` three-stage walk                         | ≤ 0.5 ms                          | Pure constexpr-table lookup; O(N) over bindings.                          |
| `MaterialParameterBlock` member fold (§3.6)                 | ≤ 0.5 ms                          | Per-member loop; bounded by cbuffer's declared field count.               |
| Canonical sort + serialize to `ReflectionRecord`            | ≤ 1 ms                            | EASTL sort; six small vectors.                                            |
| `Fory::serialize<ReflectionRecord>` (downstream of ingest)  | ≤ 1 ms                            | Codegen-emitted; documented for SPEC §9.2 cooker accounting.              |
| **Total per program (cold)**                                | **≤ 5 ms**                        | Sums to the cooker's per-artifact ingest budget; CI gate in §11.          |

A typical program (~16 bindings, ~3 entry points, ~1
`MaterialParameters` cbuffer with ~12 members, ~2 push-constant
ranges, ~4 spec constants) finishes under ~0.5 ms in practice; the
5 ms ceiling is the strict-mode upper bound a perf-regression CI
gate fails on.

The cooker parallelizes across artifacts, not within one ingest
call (§6); a 10k-artifact cook on a 10-core M1 saturates at
~10k × 0.5 ms / 10 ≈ 0.5 s of pure ingest wall-clock — folded into
the larger cook-time budget under the `shader-cook-time-budget`
spike.

### 9.3 Heap accounting

The reflection aggregate's allocator-tagged footprint:

| Pool                                                        | Ceiling | Class             |
|-------------------------------------------------------------|---------|-------------------|
| Resident `ReflectionBlob` set held by `ShaderCache` hot set  | 4 MiB   | resident, counted under `ContextTag::shader` |
| Per-call ingest peak (one program, eviction on return)      | bounded by record size; ≤ 64 KiB typical | transient arena, **not counted** per `perf-budget.md` Allocator Rule §4 |
| `slangc_reflection_ingester` JSON parser scratch            | bounded; ≤ 64 KiB | transient arena, not counted |
| `frequency_tagger` lookup tables                            | 0       | `constexpr` static data, no heap allocation |
| Cooker scratch (cook-only)                                  | shipping-excluded; counted under the editor's tag in dev builds | n/a |

Strict-mode (`GLIBRE_ALLOC_STRICT=1`) fires
`std::unexpected{core::Error::OutOfBudget}` if a `shader`-tagged
allocation pushes the live total over 32 MiB; the 4 MiB sub-pool is
documented for SPEC §11 accounting only and is not separately
strictly enforced by the allocator (the `shader` ceiling is one
number).

### 9.4 Cold-start cost

The reflection aggregate's cold-start cost is **zero** in shipping:
`ReflectionRecord` is not eagerly deserialized at
`ShaderCache::open` (SPEC §9.5). Per-artifact deserialize cost is
folded into `ShaderCache::get`'s ~5 µs (SPEC §9.3); the ~5 µs is
charged to `render`'s CPU-submit budget (SPEC §9.3 row "(External)
PSO queries"), not to `shader`.

### 9.5 CI gate contributions

The reflection aggregate contributes one cook-time gate to the
`shader-cook-time-budget` spike's CI workflow:

- **Per-program ingest budget.** Catch2 `BENCHMARK` block in
  `tests/shader/perf/reflection_ingest_budget.cpp` over a
  reference program (the engine's S1 sample-scene material
  shaders) asserts `ingest(artifact) <= 5 ms` on the M1 baseline.
  Failure fails the PR; threshold is the §9.2 strict-mode upper
  bound.

The aggregate contributes nothing to the per-frame
`perf-budget.yml` gate beyond what SPEC §9.6 already enforces
(zero CPU sim, zero CPU submit under `ContextTag::shader`).

## 10. Failure modes

The reflection aggregate's failure surface is the four enumerators
already pinned by SPEC §10.2: **`ReflectionExtractionFailed`,
`DescriptorFrequencyAmbiguous`, `DescriptorFrequencyMissing`,
`EntryPointMissing`**. Every refusal in this aggregate maps to
exactly one of these. The aggregate adds **no new enumerators**
beyond SPEC §5; SPEC §10's closed-sum guarantee at the public
boundary is preserved.

The spike's brief lists candidate names `BindingDuplicated`,
`TypeUnsupported`, but SPEC §5 / §10 do not declare them as
distinct arms; per SPEC §10's closed-sum rule, these conditions
**fold into `ReflectionExtractionFailed`** with structured `detail`
fields. This design pins the `detail` strings used so log /
telemetry consumers can dispatch on them deterministically.

| Trigger                                                       | Enumerator                       | `detail` (when arm is `ReflectionExtractionFailed`) | Severity (§10.1 of SPEC) | Detection point                                        |
|---------------------------------------------------------------|----------------------------------|----------------------------------------------------|--------------------------|--------------------------------------------------------|
| slangc reflection JSON parse failure (malformed bytes)         | `ReflectionExtractionFailed`     | `"json_parse_failed:<offset>"`                     | `refuse`                 | `slangc_reflection_ingester` JSON parser              |
| Reflection record's `bytecode_hash` ≠ artifact's `bytecode_hash` | `ReflectionExtractionFailed`     | `"unpaired_bytecode_hash"`                         | `refuse`                 | §4.2 pairing oracle, before ingest                    |
| Slang `BindingKind` outside the §3.2 closed sum                | `ReflectionExtractionFailed`     | `"unsupported_kind:<slang_kind_name>"`             | `refuse`                 | `BindingSlot` lift loop                                |
| Two bindings differ in `(kind, name)` but share `(register_space, register_index)` | `ReflectionExtractionFailed` | `"duplicate_binding:space=N,index=M"`              | `refuse`                 | `BindingSlot` post-lift dedupe                         |
| Vertex element `format_code` not in §3.4 mapping table         | `ReflectionExtractionFailed`     | `"unsupported_format:<slang_type_name>"`           | `refuse`                 | `VertexIOLayout` lift                                  |
| Two vertex elements share `(location, semantic, semantic_index)` | `ReflectionExtractionFailed`     | `"vertex_input_collision"`                         | `refuse`                 | `VertexIOLayout` lift                                  |
| Two `PushConstantRange`s overlap                               | `ReflectionExtractionFailed`     | `"push_constant_overlap"`                          | `refuse`                 | `PushConstantRange` lift                               |
| Two `SpecializationConstantSlot`s share `id`                   | `ReflectionExtractionFailed`     | `"spec_const_id_collision"`                        | `refuse`                 | `SpecializationConstantSlot` lift                      |
| Slang `[frequency(...)]` value not in `{PerFrame, PerPass, PerMaterial, PerDraw}` | `DescriptorFrequencyAmbiguous`   | n/a (dedicated arm)                                | `refuse`                 | `frequency_tagger` stage A                            |
| `register_space ≥ 4` and no `[frequency(...)]`                 | `DescriptorFrequencyAmbiguous`   | n/a                                                | `refuse`                 | `frequency_tagger` stage B                            |
| Binding survives stages A + B without a frequency               | `DescriptorFrequencyMissing`     | n/a                                                | `refuse`                 | `frequency_tagger` stage C                            |
| Reflection lists an entry point absent from the artifact's `ShaderSource` entry-point manifest | `EntryPointMissing` | n/a                                                | `refuse`                 | `EntryPoint` lift, post-collation                     |
| Reflection lists two `[shader("vertex")]` functions             | `EntryPointMissing`              | n/a                                                | `refuse`                 | `VertexIOLayout` lift                                  |

Refusal rules:

1. **Refuse-publish, never partial.** A refusal at any detection
   point aborts the ingest call before the `ReflectionBlob` is
   returned. The cooker's `ShaderCache::insert` is **not** invoked
   for the in-flight artifact; the prior CAS entry remains live
   (SPEC §8.4 refusal case 3).
2. **No retry inside the aggregate.** The §10 of SPEC bans silent
   retry; the ingester does not re-parse, does not re-extract with
   relaxed rules, does not fall back to a "best-effort"
   `ReflectionBlob`. Retries, if any, happen at the cooker /
   editor layer.
3. **Structured logging at the boundary.** Every refusal is logged
   exactly once at the boundary where it is *handled* — i.e. by
   the cooker (cook path) or the editor (hot-reload path) — per
   `error-model.md` §"Logging / Telemetry". The ingester itself
   does not log; it returns `std::unexpected`.
4. **Severity is `refuse` for every aggregate-internal refusal.**
   Per SPEC §10.1, all four enumerators are `refuse`-class; the
   prior cache state remains live; no observer-bus event is
   emitted. None of the four are `fatal` (the closed-sum is
   recoverable by re-cook from source).
5. **No mapping of these refusals onto `core::Error`.** The
   reflection aggregate's refusals are `shader::Error` values; the
   engine-wide `glibre::Error` variant rolls up `shader::Error`
   per `error-model.md` Composition Rule #1 without re-wrapping.

The spike brief names `core::Error` / `shader::Error` as the
target arm; this design routes every reflection failure through
`shader::Error`. The `core::Error` is reserved for cross-cutting
hot-reload refusals (`HotReloadRefused` wrapping a
`shader::Error::ReflectionExtractionFailed` cause), which is
itself owned by the loader, not the reflection aggregate.

## 11. Test plan

All tests are Catch2 (`TEST_CASE` / `BENCHMARK`) and live under
`tests/shader/reflection/`. Each test name maps to one acceptance
artifact; SPEC §11 acceptance criteria are extended with these
plan-level unit names so the SPEC's `type:user-story` test
inventory remains authoritative.

### 11.1 Unit — record normalization

| Test name (Catch2 `TEST_CASE`)                                          | Asserts                                                                                                  |
|-------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------|
| `reflection_blob_extracts_canonical_metadata_from_slangc`               | (SPEC §11 #332) Ingester over the S1-fixture program produces the golden `ReflectionBlob`; field-by-field. |
| `reflection_blob_is_structurally_equal_for_same_bytecode`               | Two ingester runs over byte-equal slangc records produce byte-equal `ReflectionRecord`s (§3.9).          |
| `reflection_ingester_refuses_unpaired_input`                            | §4.2 pairing oracle: a `ShaderArtifact` whose `bytecode_hash` ≠ record's paired hash → `ReflectionExtractionFailed{detail="unpaired_bytecode_hash"}`. |
| `reflection_ingester_rejects_unsupported_kind`                          | A synthetic record with a `BindingKind` outside §3.2 closed sum → `ReflectionExtractionFailed{detail="unsupported_kind:<name>"}`. |
| `reflection_ingester_rejects_duplicate_binding`                         | Two bindings differing in `(kind, name)` but sharing `(register_space, register_index)` → `ReflectionExtractionFailed{detail="duplicate_binding:space=N,index=M"}`. |
| `reflection_ingester_merges_cross_stage_bindings`                       | Two bindings with identical `(kind, register_space, register_index, name)` and disjoint `stages` → one row, OR-ed `stages` (§3.2 merge rule).   |
| `reflection_ingester_rejects_overlapping_push_constants`                | Two `PushConstantRange`s with overlapping byte windows → `ReflectionExtractionFailed{detail="push_constant_overlap"}`. |
| `reflection_ingester_rejects_collision_in_vertex_layout`                | Two `VertexInputElement`s with equal `(location, semantic, semantic_index)` → `ReflectionExtractionFailed{detail="vertex_input_collision"}`. |
| `reflection_ingester_rejects_unsupported_vertex_format`                 | A vertex parameter's Slang scalar/vector type not in §3.4 table → `ReflectionExtractionFailed{detail="unsupported_format:<name>"}`. |
| `reflection_ingester_rejects_spec_const_id_collision`                   | Two `SpecializationConstantSlot`s sharing `id` → `ReflectionExtractionFailed{detail="spec_const_id_collision"}`. |
| `reflection_ingester_rejects_two_vertex_entry_points`                   | Reflection lists two `[shader("vertex")]` functions → `EntryPointMissing` (multi-vertex variant).        |
| `reflection_ingester_rejects_entry_point_absent_from_source`            | Reflection's entry-point name absent from `ShaderArtifact::reflection.entry_points` (the source-side manifest) → `EntryPointMissing`. |
| `frequency_tagger_resolves_explicit_attribute`                          | Stage A: `[frequency("PerMaterial")]` annotation pins frequency.                                          |
| `frequency_tagger_resolves_register_space_convention`                   | Stage B: register space 0..=3 maps deterministically.                                                    |
| `frequency_tagger_rejects_unknown_attribute_value`                      | Stage A with `[frequency("Bogus")]` → `DescriptorFrequencyAmbiguous`.                                     |
| `frequency_tagger_rejects_register_space_overflow`                      | Register space ≥ 4 with no annotation → `DescriptorFrequencyAmbiguous`.                                   |
| `frequency_tagger_reports_missing_frequency`                            | Binding survives stages A + B → `DescriptorFrequencyMissing`.                                             |
| `frequency_tagger_forces_push_constant_to_per_draw`                     | `BindingKind::PushConstant` always tagged `PerDraw` regardless of annotation.                              |
| `frequency_tagger_promotes_g_material_to_per_material`                  | `ConstantBuffer<MaterialParameters> g_material` always tagged `PerMaterial`.                              |
| `material_parameter_block_folds_g_material_members`                     | §3.6 fold: `g_material`'s declared scalar/vector members appear in `MaterialParameterBlock::members` with `frequency = PerMaterial`. |
| `material_parameter_block_empty_when_no_g_material`                     | A module without `g_material` → empty `MaterialParameterBlock`.                                            |
| `entry_point_sort_is_canonical`                                         | `entry_points` sorted ascending by `(stage_ordinal, name)`.                                                |
| `binding_sort_is_canonical`                                             | `bindings` sorted ascending by `(register_space, register_index, stage_mask)`.                              |
| `push_constant_sort_is_canonical`                                       | `push_constants` sorted ascending by `(offset, size)`.                                                     |
| `spec_const_sort_is_canonical`                                          | `spec_constants` sorted ascending by `id`.                                                                 |
| `vertex_io_sort_is_canonical`                                           | `vertex_io.elements` sorted ascending by `(location, semantic, semantic_index)`.                            |

### 11.2 Unit — Fory persistence round-trip

| Test name                                                             | Asserts                                                                                                  |
|-----------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------|
| `reflection_record_round_trip_is_identity`                            | `from_record(to_record(blob)) == blob` for the S1-fixture program (§7.2).                                |
| `reflection_record_serialize_is_byte_stable`                          | Two `Fory::serialize<ReflectionRecord>(rec)` calls over the same record produce byte-equal output.        |
| `reflection_record_v1_golden_round_trip_is_byte_equal`                | Frozen v1 payload at `tests/shader/persistence/golden_v1.foryblob` decodes, re-serializes byte-equal.    |
| `reflection_record_unknown_kind_round_trip_refuses`                   | A v2 payload (synthetic; introduces an additive `BindingKind` arm) decoded by a v1 host → `ReflectionExtractionFailed{detail="unsupported_kind:..."}` (§7.3 rule 3). |

### 11.3 Integration — full extraction over real Slang programs

| Test name                                                             | Fixture                                                                                                  |
|-----------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------|
| `reflection_full_extraction_over_s1_material_set`                     | The S1 sample-scene material shader set (vertex + pixel + compute permutations) is compiled by `glibre-shadercc` and ingested; resulting `ReflectionBlob` matches a per-program golden. |
| `reflection_full_extraction_over_mesh_shader_program`                 | A `[shader("mesh")]` + `[shader("amplification")]` Slang program ingests; `entry_points` carries both stages; `vertex_io.elements` is empty (mesh shaders have no vertex IO). |
| `reflection_full_extraction_over_compute_only_program`                | A compute-only program ingests; `vertex_io.elements` empty; `bindings` populated.                         |
| `reflection_full_extraction_over_rt_program`                          | An RT program (`raygeneration` + `closesthit` + `miss`) ingests; `entry_points` all carry `Stage::Library`; `rt_payload_bytes` reflects the largest payload. |
| `reflection_pairing_oracle_catches_swapped_artifact`                  | Two artifacts swapped (artifact A's record paired with artifact B's bytecode hash) → `ReflectionExtractionFailed{detail="unpaired_bytecode_hash"}`. |
| `reflection_hot_reload_re_extracts_only_affected_set`                 | `inject_source_diff(foo.slang, ...)` re-runs the ingester only for permutations whose preprocessed closure includes `foo.slang`; unaffected permutations' `ReflectionBlob`s survive bit-equal (SPEC §8.2 rule 1).  |
| `reflection_hot_reload_refusal_leaves_prior_blob_live`                | A re-extraction that fails (synthetic frequency-ambiguity) → no `ShaderArtifactReplaced` published; `ShaderCache::get(prior_hash).reflection` byte-equal to the pre-failure blob. |

### 11.4 Performance

| Benchmark name (Catch2 `BENCHMARK`)                                    | Asserts                                                                                                  |
|------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------|
| `reflection_ingest_budget_per_program`                                 | §9.2 — `ingest(artifact) ≤ 5 ms` on the M1 baseline over the reference program; PR fails on regression.   |
| `reflection_record_serialize_budget`                                   | §9.2 — `Fory::serialize<ReflectionRecord>(rec) ≤ 1 ms` per program.                                       |

### 11.5 Test plan rules

1. **Coverage.** Every refusal arm in §10's table has at least one
   unit test; every §3 ordering rule has at least one unit test.
2. **Goldens are bytes, not prose.** `reflection_full_extraction_*`
   tests compare the Fory-serialized `ReflectionRecord` byte-for-
   byte against a `tests/shader/reflection/goldens/<name>.foryblob`
   file checked into the repo. Updates to a golden are PR-reviewable
   as a binary diff.
3. **No flakes.** Every test is deterministic; no test depends on
   slangc subprocess ordering, file-system enumeration order, or
   thread scheduling.
4. **CI integration.** Tests run under `tests/shader/`'s Catch2
   harness; per-PR for unit tests, per-PR for integration tests
   that hit `glibre-shadercc`. The cook-time budget benchmarks run
   nightly per `perf-budget.yml`.

## 12. Open questions

- `[OPEN]` #1 — **Round-trip-from-bytecode-alone.** A future post-MVP
  story may want to reconstruct a `ReflectionBlob` from a stored
  `metallib` (or `DXIL`) blob *without* its paired slangc reflection
  record (e.g. for migrating from a third-party-cooked archive). MVP
  refuses this (§1, §4.2 pairing oracle). When the need surfaces,
  the resolution gate is a new spike under sub-epic #744 that picks
  a backend-specific bytecode parser; it would not extend this
  aggregate but ship as a sibling. Owner: shader sub-epic lead.
- `[OPEN]` #2 — **Per-field RT payload reflection.** §3.8 emits only
  `rt_payload_bytes`. Per-field decoding of the payload struct
  (offset / size / type per field) is post-MVP; it requires a new
  `RtPayloadFieldRecord` sub-schema and an additive tag 8 on
  `ReflectionRecord` (§7.3). Owner: render context lead — surfaces
  when the first RT story past hybrid-shadows lands.
- `[OPEN]` #3 — **`BindingKind` enumerator growth.** Adding a new
  binding kind (e.g. a future Slang resource type, or a Metal-
  specific `BindlessTexture`) requires a SPEC §5 amendment plus a
  refresh of §3.2's closed-sum table here. The growth path is the
  standard additive enumerator rule (§7.3 rule 3); the spike that
  ships the new kind owns the amendment. Owner: shader sub-epic
  lead.
- `[OPEN]` #4 — **Distinct `Stage` arms for individual RT shaders.**
  §3.10 collapses `raygeneration` / `closesthit` / `anyhit` /
  `miss` / `intersection` / `callable` into `Stage::Library`. A
  future render-graph need to dispatch on the specific RT stage
  would split the enum; that is a SPEC §5 amendment + a §3.10 table
  refresh + an additive `ReflectionRecord` schema bump (per-entry-
  point `rt_stage : u8`). Owner: render context lead.
- `[OPEN]` #5 — **Non-Slang backend story.** SPEC §3 collapse 1 pins
  Slang as the sole shader source language for MVP. A future non-
  Slang backend (e.g. an MSL-direct authoring path for a small fast-
  iteration build) would ship its own ingester and its own
  reflection-record schema; this aggregate's design does not
  generalize. Spike trigger: a `shader-multi-backend` story under
  sub-epic #744. Owner: shader sub-epic lead.
- `[OPEN]` #6 — **Parallel hot-reload ingest.** §6 confirms the
  ingester is parallelism-safe per artifact, but the `recompile_
  affected` path in SPEC §8.6 runs sequentially. Parallelizing
  recompile-and-reflect across the affected set (likely via a small
  per-cook thread pool) is post-MVP; the gate is whether real
  hot-reload latencies on M1 with the reference S1 fixture exceed
  editor-tolerance budgets. Owner: tools / editor lead.
- `[OPEN]` #7 — **`detail` string stability.** §10's `ReflectionExtractionFailed`
  arm carries a structured `detail` field (`"unsupported_kind:...",
  "duplicate_binding:..."`, etc.). This design pins the strings used,
  but they are not part of the §5 closed-sum guarantee — log /
  telemetry consumers that want to dispatch on `detail` need a
  stable contract. Resolution gate: the `task-breakdown-error-perf`
  spike's logging plan, which audits `ErrorContext::detail` shape
  engine-wide. Until then, treat `detail` strings as unstable
  human-readable hints. Owner: error-model decision-record owner.
