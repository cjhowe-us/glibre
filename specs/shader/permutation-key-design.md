# `shader` — `PermutationKey` Detailed Design

> Detailed design for the **permutation-key** aggregate in the `shader`
> context (SPEC §4.2, §5 lines 523–589, §7.1 sub-schema row, §10.2 rows
> `PermutationKeyMalformed` / `PermutationKeyOutOfRange`). This document
> refines the SPEC's invariants into an implementation-shaped contract.
> It is the deliverable of spike #747 and the input to the sibling
> task-breakdown spike that decomposes implementation work into
> `type:plan` issues.
>
> Authority: this design refines `specs/shader/SPEC.md` §4.2, §5 lines
> 523–589, §7.1, §7.2 (`PermutationKeyRecord`), §7.3 #2, §7.4 #1, §9
> (entries marked `PermutationKey`), and §10.2 rows
> `PermutationKeyMalformed` / `PermutationKeyOutOfRange`. Anything not
> addressed here defers to those sections; anything that appears to
> contradict them is a defect in this document.

---

## 1. Purpose

The permutation-key aggregate owns **the rules for naming one point in
the closed 4-axis shader specialization cross-product** as a single
trivially-copyable value object with a deterministic packed encoding
suitable for hashing, ordering, and codegen-table generation. Its sole
responsibility is to **canonicalize the 4-tuple `(ShadingModel,
FeatureSet, RenderPath, LODTier)` into a stable byte-key and to
serve equality and ordering at hash-table speed**.

It refuses to own:

- **Source ingestion.** The Slang translation unit, include closure,
  and entry-point manifest belong to `ShaderSource` (SPEC §4.1, spike
  #745). The permutation-key references the ingested source only via
  the BLAKE3 `source_hash` that the `ShaderHash` envelope carries;
  the key itself does not store source bytes, paths, or entry-point
  names.
- **Compilation.** Driving slangc, building flag lists, and emitting
  bytecode belong to `CompilationPipeline` (SPEC §4.3, spike #749).
  The key is a *direction* the pipeline consumes; it never invokes,
  spawns, or canonicalizes flags.
- **Cache lookup and CAS storage.** Content-addressable lookup and
  the cooked `ShaderLibrary` belong to `ShaderCache` (SPEC §4.6,
  spike #755). The key is the **permutation half** of a `ShaderHash`;
  the other halves (source-hash, flags-hash, target byte) are joined
  by the cache when it derives the artifact identity. The key is
  *not* a `ShaderHash`.
- **Reflection and descriptor layout.** A `ReflectionBlob` (SPEC §4.4)
  and `DescriptorLayout` (SPEC §4.5) are derived per
  `(Backend, PermutationKey)`, but the key holds none of their
  bytes — it is an input to derivation, not a record of the result.
- **Permutation enumeration policy.** Which keys a project
  *materializes* into its build is the offline permutation
  enumerator's concern (SPEC §6, sub-epic #70). The key knows the
  cross-product cardinality (`kShadingModelCount × 2^kFeatureBitCount
  × kRenderPathCount × kLODTierCount`) but does not enumerate it.
- **Capability validation.** Whether a given backend (`IShaderBackend`,
  SPEC §4.7) advertises mesh shaders / RT / fp16 etc. is a
  `Capabilities` query. The key carries no capability bits; mismatch
  is detected at compile time with `Error::CapabilityNotSupported`,
  not at key-construction time.

The design lives entirely inside the `shader/permutation/` sub-module.
The public ABI surface it exports is the four closed enums
(`ShadingModel`, `FeatureBit`, `RenderPath`, `LODTier`), the
`FeatureSet` bitfield, the `PermutationKey` POD struct, the
`PermutationIndex` dense ordinal, and the `to_bytes` /
`from_bytes` / `is_well_formed` operations declared in SPEC §5.

---

## 2. Requirements Coverage

Harmonius prior art (`harmonius/docs/design/rendering/shader-variants.md`,
`shader-variants-test-cases.md`) is treated as **research input only**
(PHILOSOPHY "How harmonius is used", AGENTS.md). Conclusions are
re-derived against glibre's invariants.

The table below enumerates every harmonius requirement / design
clause that touches the permutation-key space and routes it to one of:
**covered** (with this doc's section), **collapsed** (folded into a
single glibre primitive), or **refused** (out of MVP scope with
rationale rooted in glibre SPEC §3 collapses or §4.2 invariants).

| Harmonius clause                                                                                            | glibre disposition                                                                                                                               | Where    |
|-------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------|----------|
| `R-2.3.10.1` — Shaders defined as material × feature × pass × LOD                                           | **Covered.** Re-derived as the closed 4-tuple `(ShadingModel, FeatureSet, RenderPath, LODTier)` (SPEC §2, §4.2).                                 | §3       |
| `R-2.3.10.2` — `PermutationKey` is hashable, serializable, and stable                                       | **Covered.** `to_bytes()` produces a 6-byte total-injective bit-stable encoding; serialization rides through `PermutationKeyRecord` (§7).        | §3, §7   |
| `R-2.3.10.3` — Per-mesh max variant count = 64; total per-project = 4096                                    | **Refused for the key.** Cross-product budgets are an enumerator-policy concern (sub-epic #70), not a key-aggregate concern. The key knows total cardinality (§3.4) but does not enforce caps. | n/a (out of scope) |
| `R-2.3.10.4` — Asset processing precompiles the tracked variant set                                         | **Refused for the key.** Cooker concern (`ShaderCache::cook`, SPEC §4.6 / §6.4); the key is the *input*, not the policy.                          | n/a      |
| `R-2.3.10.5` — Untracked variants compile on demand, recorded in usage metrics                              | **Refused.** glibre forbids on-demand runtime compile (SPEC §4.6 inv 2, §4.8 inv 3, §10.3). The key has no on-demand fallback path.               | n/a      |
| `R-2.3.10.6` — Hot reload of compiled variants                                                              | **Collapsed.** Hot-reload survival follows from the key being immutable POD: keys carried as state survive plugin swap byte-equal (§8). No per-key hot-swap state machine. | §8       |
| `R-2.3.10.7` — CLI report of variant counts, uncompiled misses, and bundle sizes                            | **Refused.** CLI / metrics live in tools; the key exposes `PermutationIndex` and total cardinality so tools can iterate, but does not own reporting. | n/a      |
| `PermutationKey { shading_model, features, render_path, lod }` (Rust prior art, `shader-variants.md` §"Core Types") | **Covered.** Re-derived in C++23 as a POD struct (SPEC §5 lines 569–583). Field order matches harmonius for cross-walk readability; bit packing is glibre's. | §3       |
| `ShaderFeatures` 32-bit bitset with 16 used bits (`shader-variants.md` §"ShaderFeatures Bitset")            | **Collapsed.** glibre's `FeatureSet` is 16 bits with 6 currently-defined positions (SPEC §5 lines 530–557). The harmonius set (skinning, morph_targets, vertex_color, normal_map, alpha_cutout, alpha_blend, two_sided, parallax, decal, emissive, clear_coat, anisotropic, refraction, motion_vectors, velocity, stereo) collapses onto SPEC §5's tighter MVP set; growth is additive (§7 / SPEC §7.4 #1). | §3.2     |
| `PermutationKey::content_hash() → BLAKE3` (`shader-variants.md` §"Core Types")                              | **Collapsed and re-scoped.** The key itself does **not** hash. The BLAKE3 digest that identifies a *cache entry* is `ShaderHash`, computed by `ShaderCache` over `(source_hash ∪ key.to_bytes() ∪ flags_hash ∪ target_byte)` (SPEC §2, §4.6 inv 1, §7.3 #1). The key contributes its 6 packed bytes; nothing more. | §3.5, §7 |
| `ShadingModel { Pbr, Skin, Hair, Cloth, Foliage, Eye, Water, ClearCoat, ... }` (`shader-variants.md` class diagram) | **Covered.** Re-derived as the SPEC §5 enum `ShadingModel { Standard, Skin, Hair, Cloth, Foliage, Eye, Water, ClearCoat }` (cardinality 8). Harmonius's `Pbr` collapses into glibre's `Standard`. | §3.2     |
| `RenderPath { Forward, Deferred, ShadowMap, Velocity, ... }` (`shader-variants.md`)                         | **Covered.** Re-derived as SPEC §5 `RenderPath { Forward, Deferred, DepthOnly, Shadow, Velocity, Probe }` (cardinality 6). `ShadowMap` collapses into `Shadow`; `Probe` is glibre-new for IBL bake passes. | §3.2     |
| `LodLevel { High, Medium, Low }` (`shader-variants.md`)                                                     | **Covered and renamed.** SPEC §5 `LODTier { Mobile, Switch, Desktop, HighEnd }` (cardinality 4). The semantics shift from "geometry LOD" to "platform/perf tier"; this is intentional — geometry LOD is `geometry`'s concern, not `shader`'s. | §3.2     |
| `VariantBudget` per-mesh / per-shading-model / per-render-path / per-project caps                           | **Refused for the key.** Enumerator-policy concern (SPEC §6 sub-epic #70). The key serves as the iteration coordinate; budget enforcement is a wrapper at the cooker level. | n/a      |
| `PermutationKey::to_defines() → Vec<&str>` (`shader-variants.md` §"On-Demand Compiler")                     | **Refused at the key boundary.** Translating a `FeatureSet` into Slang `#define` strings is the `CompilationPipeline`'s flag canonicalizer (SPEC §4.3 inv 4); the key is target-language-agnostic. | n/a      |
| `dxc_profile()` mapping shading-model → HLSL profile string                                                 | **Refused.** `CompileTarget` is a separate axis on `ShaderHash`, not on the key (SPEC §5 line 598). Profile selection lives in the backend. | n/a      |
| `VariantIndex sorted ascending by PermutationKey` (`shader-variants.md` class diagram)                      | **Covered.** SPEC §4.2 invariant 2 fixes ascending iteration order; this design pins the comparator (§3.6).                                       | §3.6     |
| Generation/version field on the key for hot-reload skew detection                                           | **Refused.** Hot-reload skew is detected via `schema_abi_hash` on the manifest (SPEC §7.3 #4); the key carries no version. Adding one would couple per-key state to the hot-reload protocol, violating SPEC §4.2 SRP.               | n/a      |

**Coverage attestation.** Every harmonius clause that lands in MVP scope
for the permutation-key aggregate is covered above; every refusal
carries an SRP-grounded rationale rooted in SPEC §3 (Occam collapses),
§4.2 (key-aggregate invariants), or §4.6 / §4.8 (cache and shipping
invariants that forbid on-demand compile). No clause is silently
dropped.

---

## 3. Detailed Model

### 3.1 The aggregate at a glance

A `PermutationKey` is a **trivially-copyable value object** of fixed
size. It carries four scalar/bitfield axes, exposes a 6-byte canonical
serialization, and supports equality, total ordering, and dense-ordinal
codegen indexing. It is **not** a class with construction validation
that may fail: any `PermutationKey` value the code holds is
*structurally* well-formed (its enumerators are in declared range);
ill-formed bytes are rejected at the `from_bytes` boundary, never as a
held value.

Sizing and alignment (SPEC §5, PermutationKey POD struct, §4.2 invariant 1):

```
sizeof(PermutationKey)  = 6 bytes
alignof(PermutationKey) = 2 bytes  (driven by FeatureSet's uint16_t member)

Field                       Type            Size      Offset   Notes
shading_model               ShadingModel    1 byte    0
(implicit padding)          —               1 byte    1        inserted by compiler for uint16_t align
features                    FeatureSet      2 bytes   2
render_path                 RenderPath      1 byte    4
lod_tier                    LODTier         1 byte    5
                                            -----
                                            6 bytes (actual struct layout)
```

**Important:** `FeatureSet` wraps a `std::uint16_t`, which requires 2-byte alignment.
The compiler therefore inserts an implicit 1-byte padding slot at offset 1, making
`offsetof(PermutationKey, features) == 2`, not 1. `sizeof(PermutationKey) == 6` and
`alignof(PermutationKey) == 2`.

`to_bytes()` explicitly encodes each field into the `PackedBytes` wire format (§3.5)
and does **not** `memcpy` the struct directly. Any code that reads the wire format
via a raw `memcpy` of the in-memory struct is a defect — the struct layout and the
wire layout are deliberately decoupled so that compiler padding never silently corrupts
wire data. The unit suite asserts `static_assert(offsetof(PermutationKey, features) == 2)`
to lock down the actual struct layout and catch any future reordering.

Trivially copyable per `std::is_trivially_copyable_v<PermutationKey>`;
this is asserted by a Catch2 static_assert in the unit suite.

### 3.2 The four axes — closed enums and bit assignments

Every axis is a **closed enum** (or, for `FeatureSet`, a closed
bitfield over a fixed bit-position assignment). Closed means: every
value the type can hold has a documented meaning at this revision of
the SPEC; adding an enumerator or bit is a SPEC amendment, not an
in-place edit (SPEC §4.2 invariant 3). The bit positions are the
public ABI; renumbering breaks every persisted key on disk.

#### 3.2.1 `ShadingModel` — `enum class : std::uint8_t`, cardinality 8

| Ordinal | Enumerator   | Semantics                                             |
|---------|--------------|-------------------------------------------------------|
| 0       | `Standard`   | PBR specular-glossiness / metal-rough; default        |
| 1       | `Skin`       | Subsurface scattering for skin                        |
| 2       | `Hair`       | Anisotropic strand model                              |
| 3       | `Cloth`      | Charlie-sheen / fuzz BRDF                             |
| 4       | `Foliage`    | Two-sided thin translucent                            |
| 5       | `Eye`        | Refractive iris + cornea                              |
| 6       | `Water`      | Refraction + caustics + foam                          |
| 7       | `ClearCoat`  | Dual-lobe specular over base                          |

`kShadingModelCount = 8`. Range check (§3.7): byte value must satisfy
`v < 8`.

#### 3.2.2 `FeatureBit` and `FeatureSet` — `std::uint16_t` over 6 named bits

| Bit | Position | Enumerator      | Semantics                                  |
|-----|----------|-----------------|--------------------------------------------|
| 0   | `1 << 0` | `Skinned`       | Hardware skinning vertex path enabled      |
| 1   | `1 << 1` | `MotionVectors` | Per-vertex previous-frame position output  |
| 2   | `1 << 2` | `AlphaTest`     | Discard via cutoff threshold               |
| 3   | `1 << 3` | `Decal`         | Decal-only path (no base material)         |
| 4   | `1 << 4` | `VirtualTexture`| Tile-based virtual texture sampling        |
| 5   | `1 << 5` | `RT`            | Hit-shader / any-hit / closest-hit path    |

`kFeatureBitCount = 6`. The unused upper 10 bits are **reserved for
future feature additions** under SPEC §7.4 #1 (additive-only growth)
and **must be zero** in any well-formed `FeatureSet` at this revision.
A non-zero reserved bit in `from_bytes` input is rejected with
`Error::PermutationKeyMalformed` (§3.7); this is the load-bearing
forward-compatibility seam (older runtimes refuse newer keys cleanly,
as opposed to silently widening — PHILOSOPHY §8 refusal-driven
hot-reload, SPEC §7.4 #1).

`FeatureSet` storage is `std::uint16_t` (SPEC §5 line 543); 2 bytes,
little-endian on disk (§3.5).

`FeatureSet` operations: `test`, `set`, `bits()`, equality (SPEC §5
lines 545–553). Notably absent from the public surface:

- **No `clear(FeatureBit)`** — removing a bit on a held key is a
  builder-side operation that lives in the offline permutation
  enumerator, not the value object.
- **No union / intersection** — `FeatureSet` is *not* a set algebra
  type for runtime use; it is a packed coordinate. Algebra over
  feature bits at the cooker layer constructs new keys explicitly.
- **No `to_defines()`** — the mapping from feature bits to Slang
  `#define` tokens lives in `CompilationPipeline` (SPEC §4.3
  invariant 4), not in the key.

#### 3.2.3 `RenderPath` — `enum class : std::uint8_t`, cardinality 6

| Ordinal | Enumerator  | Semantics                                                |
|---------|-------------|----------------------------------------------------------|
| 0       | `Forward`   | Forward-shaded color pass                                |
| 1       | `Deferred`  | GBuffer fill pass                                        |
| 2       | `DepthOnly` | Pre-pass / Z-prepass                                     |
| 3       | `Shadow`    | Shadow-map render (cascade or atlas)                     |
| 4       | `Velocity`  | Motion-vector render target                              |
| 5       | `Probe`     | IBL / radiance probe bake pass                           |

`kRenderPathCount = 6`. Range check: `v < 6`.

#### 3.2.4 `LODTier` — `enum class : std::uint8_t`, cardinality 4

| Ordinal | Enumerator | Semantics                                            |
|---------|------------|------------------------------------------------------|
| 0       | `Mobile`   | iOS / iPad-class GPUs                                |
| 1       | `Switch`   | Nintendo Switch (post-MVP target)                    |
| 2       | `Desktop`  | macOS Apple Silicon baseline (the MVP default)       |
| 3       | `HighEnd`  | Discrete-class desktop / workstation                 |

`kLODTierCount = 4`. Range check: `v < 4`.

### 3.3 Cross-product cardinality

`kPermutationCrossProductCardinality
   = kShadingModelCount
   × 2^kFeatureBitCount
   × kRenderPathCount
   × kLODTierCount
   = 8 × 64 × 6 × 4
   = 12 288`

This is the count of **structurally well-formed** keys; it is the
upper bound on `PermutationIndex::value`. It is not the count of
materialized variants in any given project — that is the enumerator's
filter (sub-epic #70). The constant is exposed at compile time for
codegen-table sizing:

```cpp
inline constexpr std::uint32_t kPermutationCrossProductCardinality = 12288;
```

### 3.4 `PermutationIndex` — dense ordinal across the cross-product

A `PermutationIndex` is the bijective dense ordinal of a structurally
well-formed `PermutationKey` under the canonical iteration order
(§3.6). Its sole purpose is **codegen-table indexing**: per-key tables
emitted at offline build time (e.g. `PermutationKey →
SpecializationConstantBlock`) are flat arrays of size
`kPermutationCrossProductCardinality`, and the index is the slot.

```cpp
struct PermutationIndex {
    std::uint32_t value{0};
    friend constexpr bool operator==(PermutationIndex,
                                     PermutationIndex) noexcept = default;
};
```

The codec is total and deterministic:

```
value = ((shading_model_ord  * 2^kFeatureBitCount  + features.bits())
                              * kRenderPathCount   + render_path_ord)
                              * kLODTierCount      + lod_tier_ord
```

i.e. axes are nested little-endian-of-axes:
`(((sm * 64 + features) * 6) + path) * 4 + lod`.

The inverse (`PermutationIndex → PermutationKey`) is provided by the
codec implementation (§3.6) and used by codegen-table walkers; it
fails with `Error::PermutationKeyOutOfRange` if `value >=
kPermutationCrossProductCardinality` (SPEC §10.2 row
`PermutationKeyOutOfRange`, severity `fatal`).

### 3.5 Canonical byte layout — `to_bytes` / `from_bytes`

The 6-byte `PackedBytes = eastl::array<std::byte, 6>` encoding is the
**single source of truth** for on-disk and on-wire permutation-key
identity. It is what `PermutationKeyRecord` (SPEC §7.2) carries via
its `bytes6` field; it is what `ShaderHash` mixes into the cache key
(SPEC §2, §4.6 invariant 1, §7.3 #1); it is what equality and
ordering on the value object are defined to be byte-identical to.

Layout (offsets in bytes, little-endian for multi-byte fields):

```
Offset  Bytes  Field            Encoding
0       1      shading_model    uint8 ordinal (0..7)
1       2      features         uint16 little-endian (low byte first)
3       1      render_path      uint8 ordinal (0..5)
4       1      lod_tier         uint8 ordinal (0..3)
5       1      RESERVED         must be 0x00 in well-formed bytes
        ----
        6 bytes total
```

Why 6 bytes (not 5):

1. **Word-pad determinism.** Six bytes is divisible by no awkward
   factor; downstream BLAKE3 mixing in `ShaderHash` consumes whole
   bytes only and is insensitive to alignment, so we choose the
   round size that leaves an explicit reserved byte.
2. **Forward extension headroom without re-laying-out.** The
   reserved byte is the first place an additive future axis lands
   (e.g. a hypothetical `MaterialFamily` axis); it is not a
   functional field today and **must be zero** at this revision.
   A non-zero reserved byte in `from_bytes` input is rejected with
   `Error::PermutationKeyMalformed` (§3.7) — same forward-compat
   contract as reserved `FeatureSet` bits.
3. **Stable across architectures.** Little-endian for the 16-bit
   field is fixed by spec; we never emit native-endian bytes.

`to_bytes` is `noexcept` and infallible (any held `PermutationKey`
value is, by class invariant, well-formed; encoding cannot fail):

```cpp
PermutationKey::PackedBytes PermutationKey::to_bytes() const noexcept {
    PackedBytes out{};
    out[0] = std::byte{static_cast<std::uint8_t>(shading_model)};
    const auto bits = features.bits();
    out[1] = std::byte{static_cast<std::uint8_t>(bits & 0xFFu)};
    out[2] = std::byte{static_cast<std::uint8_t>((bits >> 8) & 0xFFu)};
    out[3] = std::byte{static_cast<std::uint8_t>(render_path)};
    out[4] = std::byte{static_cast<std::uint8_t>(lod_tier)};
    out[5] = std::byte{0};   // RESERVED
    return out;
}
```

`from_bytes` is `noexcept` and *fallible*; it is the only validation
seam in the aggregate:

```cpp
std::expected<PermutationKey, Error>
PermutationKey::from_bytes(const PackedBytes& in) noexcept {
    const auto sm   = static_cast<std::uint8_t>(in[0]);
    const auto fLo  = static_cast<std::uint8_t>(in[1]);
    const auto fHi  = static_cast<std::uint8_t>(in[2]);
    const auto rp   = static_cast<std::uint8_t>(in[3]);
    const auto lod  = static_cast<std::uint8_t>(in[4]);
    const auto resv = static_cast<std::uint8_t>(in[5]);

    const std::uint16_t bits =
        static_cast<std::uint16_t>(fLo) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(fHi) << 8);

    if (sm   >= kShadingModelCount)                           // §3.2.1
        return std::unexpected(Error::PermutationKeyMalformed);
    if ((bits & ~kFeatureBitMask) != 0)                       // §3.2.2 reserved bits
        return std::unexpected(Error::PermutationKeyMalformed);
    if (rp   >= kRenderPathCount)                             // §3.2.3
        return std::unexpected(Error::PermutationKeyMalformed);
    if (lod  >= kLODTierCount)                                // §3.2.4
        return std::unexpected(Error::PermutationKeyMalformed);
    if (resv != 0)                                            // §3.5 reserved byte
        return std::unexpected(Error::PermutationKeyMalformed);

    PermutationKey k{};
    k.shading_model = static_cast<ShadingModel>(sm);
    k.features      = FeatureSet{bits};
    k.render_path   = static_cast<RenderPath>(rp);
    k.lod_tier      = static_cast<LODTier>(lod);
    return k;
}

inline constexpr std::uint16_t kFeatureBitMask =
    (std::uint16_t{1} << kFeatureBitCount) - 1u;   // = 0x003F at this rev
```

Round-trip equality (SPEC §4.2 invariant 1, §7.3 #6):

```
∀ k ∈ PermutationKey : from_bytes(to_bytes(k)) == k
∀ b ∈ well-formed PackedBytes : to_bytes(*from_bytes(b)) == b
```

Both directions are unit-tested as goldens (§11).

**Error-boundary scope for `from_bytes`.** `from_bytes` is a **within-plugin** call.
It is called only by the `shader` plugin's own serialization layer (e.g. loading
`PermutationKeyRecord` from Fory-serialized disk bytes) and by the cooker tool,
which links against the same plugin library. It is **never** exposed directly across
the plugin C ABI boundary. The return type `std::expected<PermutationKey, shader::Error>`
therefore does not violate error-model.md §Decision item 1, which requires
`glibre::Error` only at the *engine-wide plugin boundary* (i.e. the public C ABI
surface in the middleman dylib). At that crossing the caller's adapter is responsible
for wrapping `shader::Error` into the `glibre::Error` variant (error-model.md
§Composition Rules item 1). No raw `shader::Error` value ever crosses the C ABI
directly; the adaptation is documented in §4's "Errors returned at this surface" table
and repeated here for clarity: any future caller that invokes `from_bytes` from outside
the shader plugin must route through the `glibre::Error` adapter, not call
`from_bytes` through a C ABI export.

### 3.6 Equality, ordering, and iteration

- **Equality** on `PermutationKey` is defaulted (`= default`,
  SPEC §5 line 575). Because the struct is trivially copyable and has
  no padding-relevant alternation, default equality is field-by-field
  scalar equality, which equals byte-equality on
  `to_bytes()`-output. This identity is asserted in unit tests:
  `(a == b) ⇔ (a.to_bytes() == b.to_bytes())`.
- **Total ordering** is *not* exposed via `operator<=>` on the public
  type; instead the spec-frozen comparator over `to_bytes()` lives as
  a free function `permutation_key_byte_less` and is the comparator
  the cooker / manifest serializer consume:

  ```cpp
  inline bool permutation_key_byte_less(
      const PermutationKey& a, const PermutationKey& b) noexcept {
      return a.to_bytes() < b.to_bytes();   // lexicographic on 6 bytes
  }
  ```

  This is identical to the canonical iteration order (§3.4) for
  structurally well-formed keys: nesting `(sm, features, path, lod)`
  with little-endian features matches the byte order
  `[sm | f_lo | f_hi | path | lod | 0]` lexicographically, **as long
  as the features bits stay within `[0, 64)`**. At revisions where
  `kFeatureBitCount > 8`, the index nesting and the byte order will
  begin to diverge for in-axis adjacency; the comparator is fixed
  (byte order) and the index is the secondary view. This divergence
  is documented for the day a 9th feature bit lands; today the two
  orders coincide.

**Warning — bit-count growth boundary.** The implementation MUST include
the following guard so that any future addition of a 9th feature bit
produces a hard build failure rather than a silent behavioural change in
the comparator / cooker:

  ```cpp
  // In the .cpp implementation file, near permutation_key_byte_less:
  static_assert(kFeatureBitCount <= 8,
      "byte-order and mixed-radix index-order diverge when kFeatureBitCount > 8; "
      "update permutation_key_byte_less, the index codec, and any cooker code "
      "that assumed the two orders are equivalent before removing this guard.");
  ```

  When this assert trips, the implementer must:
  - (a) verify whether the mixed-radix index formula in §3.4 must change;
  - (b) update `permutation_key_byte_comparator_matches_lex_order` in the
    unit suite to reflect the diverged ordering;
  - (c) audit any cooker code that assumed byte order and index order are
    equivalent and update accordingly.

  This guard is tracked as §12 OPEN item "static_assert bit-count guard".

- **Hashing.** `PermutationKey` is *not* a key in a `std::unordered_*`
  container in engine code. EASTL's hash containers in the cooker
  hash the 6 bytes via a small inline mixer (FNV-1a or xxhash;
  decided by sub-epic #70 plan); the public surface exposes
  `to_bytes()` and lets callers hash bytes themselves. No
  `std::hash` specialization is shipped in the public header (it
  would force a hash-policy choice on every consumer).

### 3.7 `is_well_formed` — held-value structural check

```cpp
bool PermutationKey::is_well_formed() const noexcept {
    return static_cast<std::uint8_t>(shading_model) < kShadingModelCount
        && static_cast<std::uint8_t>(render_path)   < kRenderPathCount
        && static_cast<std::uint8_t>(lod_tier)      < kLODTierCount
        && (features.bits() & ~kFeatureBitMask) == 0;
}
```

A held `PermutationKey` is *expected* to satisfy `is_well_formed()`
unconditionally — every code path that produces one (default
construction, brace-initialization with literal enumerators,
`from_bytes` success) does. The predicate exists for two narrow
purposes:

1. **Defensive guard** at the `compile()` entry inside
   `CompilationPipeline` (SPEC §4.3): callers should call
   `is_well_formed()` defensively at the earliest point they hold a
   key produced by an untrusted source (e.g. a key deserialized from
   an external buffer, or constructed via `static_cast` of arbitrary
   ordinals from a memory-corrupted or codegen-table-walked path).
   Exactly how and where `CompilationPipeline` does this is an
   implementation detail owned by issue #749.
2. **Bridging across the C ABI.** A plugin trait method that takes
   a `PermutationKey` by value receives it across the C ABI of the
   plugin dylib; the receiving side checks `is_well_formed()` to
   refuse silently-garbage-initialized inputs from a misbehaving
   caller before any further work.

It is *not* the validation seam used by `from_bytes`: that one
returns a typed `expected` and is the authoritative parse path.

### 3.8 The five things a key is **not**

Re-stating §1 in operational form, in case an implementer hits a fork:

1. A key is **not** a `ShaderHash`. Mixing a key into the BLAKE3 of
   an artifact identity is `ShaderCache`'s job (SPEC §4.6 inv 1,
   §7.3 #1). The key is one of four inputs to that hash; the others
   are `source_hash`, `flags_hash`, and the `target_byte`.
2. A key is **not** a Slang `#define` set. The translation from
   `FeatureSet` bits to compiler defines lives in `CompilationPipeline`
   (SPEC §4.3 inv 4).
3. A key is **not** a `CompileTarget`. `MetalLib` vs `DXIL` is a
   separate axis on `ShaderHash`, not on the key.
4. A key is **not** an entry-point reference. Entry-point names live
   on `ShaderSource::EntryPoint` (SPEC §4.1, §5 lines 613–617). The
   same key compiles against many entry points within a source.
5. A key is **not** capability-aware. Whether the active backend
   supports the requested feature combination is checked at compile
   time against `IShaderBackend::capabilities()` (SPEC §4.7), not at
   key construction.

---

## 4. Public Surface

The full public ABI of the permutation-key aggregate is the slice of
`shader/include/glibre/shader/shader.hpp` (SPEC §5) reproduced and
annotated below. Nothing outside the shader public header crosses the
plugin ABI for this aggregate; there are no internal-only headers
re-exported.

```cpp
// shader/include/glibre/shader/shader.hpp  — permutation-key slice

namespace glibre::shader {

// ---- 4.1 Closed enums (§3.2) -------------------------------------------

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
inline constexpr std::uint16_t kFeatureBitMask =
    static_cast<std::uint16_t>((std::uint16_t{1} << kFeatureBitCount) - 1u);

class FeatureSet {
public:
    constexpr FeatureSet() noexcept = default;
    constexpr explicit FeatureSet(std::uint16_t bits) noexcept : bits_{bits} {}

    constexpr bool         test(FeatureBit b) const noexcept;
    constexpr void         set(FeatureBit b)       noexcept;
    constexpr std::uint16_t bits()                 const noexcept { return bits_; }

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

// ---- 4.2 PermutationKey value object (§3.1, §3.5, §3.7) ----------------

struct PermutationKey {
    ShadingModel shading_model{ShadingModel::Standard};
    FeatureSet   features{};
    RenderPath   render_path{RenderPath::Forward};
    LODTier      lod_tier{LODTier::Desktop};

    friend constexpr bool operator==(PermutationKey, PermutationKey) noexcept = default;

    using PackedBytes = eastl::array<std::byte, 6>;
    PackedBytes                       to_bytes() const noexcept;
    static std::expected<PermutationKey, Error>
                                      from_bytes(const PackedBytes&) noexcept;

    bool                              is_well_formed() const noexcept;
};

static_assert(std::is_trivially_copyable_v<PermutationKey>);
static_assert(sizeof(PermutationKey) <= 8);   // no surprise bloat

// ---- 4.3 PermutationIndex dense codegen ordinal (§3.4) -----------------

struct PermutationIndex {
    std::uint32_t value{0};
    friend constexpr bool operator==(PermutationIndex, PermutationIndex) noexcept = default;
};

inline constexpr std::uint32_t kPermutationCrossProductCardinality =
    static_cast<std::uint32_t>(kShadingModelCount)
  * (1u << kFeatureBitCount)
  * static_cast<std::uint32_t>(kRenderPathCount)
  * static_cast<std::uint32_t>(kLODTierCount);   // = 12288

// Bijection (§3.4). Inverse fails with PermutationKeyOutOfRange.
PermutationIndex                     to_index(const PermutationKey&)             noexcept;
std::expected<PermutationKey, Error> from_index(const PermutationIndex&)         noexcept;

// ---- 4.4 Spec-frozen byte comparator (§3.6) ----------------------------

bool permutation_key_byte_less(const PermutationKey&,
                               const PermutationKey&) noexcept;

}  // namespace glibre::shader
```

**Boundary types at a glance.**

| Type / function                              | Role at the boundary                                  |
|----------------------------------------------|-------------------------------------------------------|
| `ShadingModel`, `RenderPath`, `LODTier`      | Closed enum axes (§3.2.1, §3.2.3, §3.2.4).            |
| `FeatureBit` + `FeatureSet`                  | Closed bitfield axis (§3.2.2).                        |
| `kShadingModelCount`, `kFeatureBitCount`,    | Cardinality constants used by codegen-table walkers.  |
| `kRenderPathCount`, `kLODTierCount`,         |                                                       |
| `kPermutationCrossProductCardinality`        |                                                       |
| `kFeatureBitMask`                            | Reserved-bit guard for §3.5 / §3.7 validation.        |
| `PermutationKey`                             | The 4-axis value object (§3.1).                       |
| `PermutationKey::to_bytes()`                 | Canonical 6-byte encoding (§3.5).                     |
| `PermutationKey::from_bytes()`               | Validation seam (§3.5, §3.7) — only fallible op.      |
| `PermutationKey::is_well_formed()`           | Defensive structural predicate (§3.7).                |
| `PermutationIndex`                           | Dense codegen ordinal (§3.4).                         |
| `to_index`, `from_index`                     | Bijection across well-formed cross-product (§3.4).    |
| `permutation_key_byte_less`                  | Spec-frozen comparator (§3.6).                        |

**Errors returned at this surface.** Exactly two enumerators from the
SPEC §5 closed sum are reachable from this aggregate:

| Enumerator                       | Surfaces from                                          | Severity (§10.1) |
|----------------------------------|--------------------------------------------------------|------------------|
| `Error::PermutationKeyMalformed` | `from_bytes` (range / reserved-bit / reserved-byte).   | `refuse`         |
| `Error::PermutationKeyOutOfRange`| `from_index` (`value >= kPermutationCrossProductCardinality`). | `fatal`  |

Both wrap at the engine boundary into `glibre::Error` via the
`shader::Error` arm of the `glibre::Error::Variant` (error-model.md
§Composition Rules, item 1; SPEC §5 line 469). Plain `T` (or `void`)
returns are reserved for infallible operations:
`to_bytes`, `to_index`, `is_well_formed`, equality, the comparator,
and every `FeatureSet` op are infallible by construction.

**No exceptions across ABI.** Every operation listed is `noexcept`.
The plugin compiles with `-fno-exceptions` (error-model.md
§Decision item 3); a hypothetical implementation bug that calls
into an exception-throwing third-party library inside this aggregate
is a defect and is caught by Catch2 `--allow-running-no-tests` and
ASan / UBSan in CI (perf-budget.md §CI Gate Spec).

**No Fory dependency in the public header.** The header pulls only
`<EASTL/array.h>`, `<cstddef>`, `<cstdint>`, `<expected>`,
`<EASTL/string_view.h>` (transitively, for `Error::to_string`).
Persistence rides through `PermutationKeyRecord` (§7), which is a
codegen-emitted POD wrapper, *not* the public type.

---

## 5. Hot / Cold Path Split

The permutation-key aggregate is unusual among `shader`-context
aggregates: it is the **only one with a hot-path-adjacent operation**
in MVP. SPEC §9.1 places the entire `shader` context at 0 ms per
shipping frame (offline cook), but the *render* context at hot-path
times must compare and look up cache entries by key as part of PSO
binding (SPEC §4.8 invariant 1 — one PSO ↔ one key). That comparison
is what this aggregate's hot-path-adjacent surface serves.

The split:

| Operation                     | Path      | Frequency                          | Budget                            |
|-------------------------------|-----------|------------------------------------|-----------------------------------|
| `to_bytes()`                  | cold      | offline cook only (cooker, editor) | unbudgeted (off-frame)            |
| `from_bytes()`                | cold      | offline cook + library load init   | unbudgeted (off-frame)            |
| `is_well_formed()`            | cold      | defensive at `compile()` entry     | unbudgeted (off-frame)            |
| `to_index()` / `from_index()` | cold      | codegen-table walk at cook time    | unbudgeted (off-frame)            |
| `permutation_key_byte_less`   | cold      | manifest sort / cook walk          | unbudgeted (off-frame)            |
| `operator==(PermutationKey, PermutationKey)`  | **hot-adjacent** | per-PSO bind in `render` (phase 7) | a few ns inline in the caller     |
| `FeatureSet::test/set/bits`   | mixed     | cold for cook; hot inline only if `render` reads bits to dispatch within a draw | hot-path-adjacent with no allocation |

The "hot-adjacent" classification means: the operation is inlined into
the caller in `render`, where its time is accounted against
`render`'s phase-7 budget (1.40 ms; perf-budget.md §Per-Context Budget
Table). The `shader` context's own per-frame budget remains 0
(SPEC §9.1 row `shader`).

Why equality is hot-adjacent and not cold:

- `render` indexes its in-memory PSO table by `(PermutationKey,
  CompileTarget)` to bind a pipeline state for a draw; the index
  build is offline (load-time; SPEC §4.5 invariant 1, §4.6 invariant
  2), but the *lookup* — comparing a draw-time desired key against
  the bound key — runs per draw. With ~3k draws/frame (perf-budget.md
  S1 scenario), even a 50 ns key-compare costs 150 µs/frame; pinning
  the compare to a literal 4-byte struct equality (§3.1) gives us
  single-digit ns inlined.

The cold-path operations get their own budget cell at cook time
(§9.1 of this design); they never appear in a per-frame budget.

**No allocator on either side.** Every operation in the public surface
is allocator-free; `to_bytes` returns by value into the caller's
storage, `from_bytes` constructs into the `expected`'s value slot, the
`FeatureSet` ops mutate a `uint16_t` in place. The aggregate has zero
heap presence (perf-budget.md §Allocator Rules).

---

## 6. Concurrency

The permutation-key aggregate is **trivially thread-safe by absence
of mutable state**.

1. **Immutable POD.** A `PermutationKey` is a 4-axis trivially
   copyable struct with public default-equality. It is pass-by-value
   everywhere; nothing in the public surface takes a non-const
   reference (SPEC §5 lines 569–583 confirms). There is no instance
   shared across threads except through user-level sharing of a
   value, which requires no synchronization (read-only access to a
   POD is race-free).
2. **No global state in the aggregate.** No singletons, no thread-
   local caches, no global enumerator tables initialized at
   `dlopen` time. The codegen-cardinality constants
   (`kShadingModelCount` etc.) are `inline constexpr std::size_t`
   and live in the read-only segment of the dylib; they are
   initialized at link time and never mutated.
3. **Hashing is the caller's concern.** Because the aggregate ships
   no `std::hash` specialization (§3.6), there is no potential for
   the cooker's hash containers to share concurrency-relevant state
   with the aggregate. The cooker chooses its own hashing policy and
   owns its own thread-safety guarantees.
4. **No reentrancy traps.** Every function in the public surface is
   pure: same input → same output, no I/O, no clock reads, no
   randomness, no allocation. They are reentrant trivially.
5. **Hot-reload concurrency.** The hot-reload barrier (frame-phases.md
   §Phase 8, hot-reload-protocol.md §Step 2) executes the plugin
   swap on the main thread with all other threads halted at
   barrier-entry. A `PermutationKey` value carried as state through
   a swap (§8) is read on no other thread during the swap window;
   thread-safety inside the aggregate is irrelevant to the swap
   window's safety, which is established by the protocol, not by
   per-aggregate locking.

**No locks.** Implementations of any operation in this design must
not take a lock. CI gating (a `clang-tidy` rule under sub-epic #70's
plan) forbids `std::mutex`, `std::shared_mutex`, `std::atomic`, and
`std::call_once` references inside `shader/permutation/`. The
EASTL-equivalent forms are likewise disallowed.

---

## 7. Persistence + ABI

The permutation-key persists through the `data` middleman dylib's
Fory codegen pipeline (`reviews/decisions/fory-codegen.md`,
SPEC §7). The on-disk record is a POD wrapper around the 6-byte
canonical encoding from §3.5; the in-memory `PermutationKey`
serializes into and out of it.

### 7.1 Schema — `PermutationKeyRecord`

`data/schemas/shader/PermutationKeyRecord.fory` (sub-schema embedded
by tag inclusion in `ShaderArtifactRecord` and `ShaderCacheManifest`,
SPEC §7.1, §7.2):

```fory
schema glibre.shader.PermutationKeyRecord {
  version  1
  since    "0.1.0"

  // The single source-of-truth field: the 6-byte canonical encoding
  // produced by PermutationKey::to_bytes (§3.5). On-disk parsers
  // never decompose it into per-axis fields; they round-trip through
  // PermutationKey::from_bytes to enforce range checks at deserialize
  // time.
  field bytes : bytes6  tag 1 since 1
}
```

**One field, by design.** The schema deliberately stores the packed
encoding rather than four separate enum-ordinal fields, for three
reasons:

1. **Schema-version boundedness.** Adding a new feature bit, a new
   `ShadingModel`, or a new axis is a *content* extension within the
   reserved space (§3.5), not a *schema* extension. The Fory schema
   stays at `version 1` across multi-year axis growth; no migration
   transform is needed for additive changes within reserved bits or
   reserved bytes (SPEC §7.4 #1).
2. **Bit-equal round-trip.** The payload that lands in
   `ShaderHash` BLAKE3 input (SPEC §7.3 #1) is byte-identical to
   the bytes the schema stored, with no Fory-side re-encoding to
   reason about.
3. **Cross-language compatibility.** The data plugin's middleman is
   a C ABI (`reviews/decisions/plugin-abi.md`); a single
   fixed-width byte field is the cleanest possible cross-ABI shape.

### 7.2 ABI stability guarantees

The permutation-key aggregate is part of the `shader` plugin's
public ABI surface; its stability rules sit on top of the
plugin-abi.md and fory-codegen.md ABI rules:

1. **Enumerator ordinals are immutable.** Once an enumerator has a
   non-zero CI build that ships with it, its ordinal is part of the
   ABI forever. New enumerators are appended at the next free
   ordinal; existing ones are never reordered or repurposed.
   Renaming an enumerator at the C++ level keeps its ordinal; the
   stable string mapping (`to_string` in SPEC §5) tracks the
   enum-rename so logs stay stable.
2. **`FeatureBit` positions are immutable.** Bit position 0 is
   `Skinned` forever; new bits land at positions ≥ 6 only. A bit
   *removal* requires a permutation-key schema major-version bump
   and a `migrate_PermutationKeyRecord_v1_to_v2` provider; this is
   not in MVP scope and there is no removal scheduled (SPEC §7.4
   #1).
3. **Reserved space rules.**
   - The 10 reserved bits of `FeatureSet` (positions 6–15) **must
     be zero** at this revision; non-zero on disk → reject with
     `Error::PermutationKeyMalformed` (§3.5 / §3.7).
   - The 1 reserved byte at offset 5 of `PackedBytes` **must be
     zero** at this revision; non-zero on disk → reject with
     `Error::PermutationKeyMalformed` (§3.5).
   - These guards are the forward-compatibility seam: an older
     runtime reading a newer cooked library that uses one of those
     bits will refuse the manifest at `from_bytes` and the
     containing manifest will be rejected with
     `Error::CacheIntegrity` (SPEC §7.3 #4, §10.2 row
     `CacheIntegrity`). This is the **refusal-driven** path
     (PHILOSOPHY §8), not silent widening.
4. **`schema_abi_hash` participation.** The middleman computes
   `glibre_types_abi_hash()` over every codegen-emitted POD,
   including `PermutationKeyRecord` (fory-codegen.md §ABI Stability
   Rules). A schema-major bump or any tag-layout change rotates the
   ABI hash; `ShaderCacheManifest.schema_abi_hash` then no longer
   matches, and the loader rejects the manifest (SPEC §7.3 #4).
5. **Endianness pinned to little-endian.** `to_bytes` emits the
   16-bit `FeatureSet` field little-endian regardless of host
   endianness. macOS Apple Silicon (the MVP target) is LE; the
   fixing applies even there for bit-exact determinism on a
   hypothetical BE port.

### 7.3 Migration

Per SPEC §7.4 #1 and fory-codegen.md §Migration Mechanic, additive
axis growth needs **no migration transform**. The `vN → vN+1`
machinery applies only when:

- A new axis is added that *cannot* fit in reserved bits or the
  reserved byte (would force a `bytes6 → bytes8` field bump);
- An enumerator is removed (very unlikely; would force a record
  filter across the cooked library);
- The semantics of an existing enumerator change (banned by ABI
  rule #1).

If/when such a migration ships, it carries a Catch2 golden under
`tests/shader/persistence/permutation_key_migration_v{N}_to_v{N+1}/`
that loads a frozen `vN` payload and asserts byte-equal
re-serialization at `vN+1` (SPEC §7.4 #7). No such migration is
scheduled at this revision; the §7.1 schema sits at version 1.

### 7.4 Logging round-trip

`PermutationKey` formats into `spdlog` structured logs through the
engine-wide `glibre::log_error` helper (error-model.md §Logging /
Telemetry). The mapping the helper uses is:

```
permutation_key.shading_model = "Standard"
permutation_key.features      = "0x0021"   // hex of bits()
permutation_key.render_path   = "Forward"
permutation_key.lod_tier      = "Desktop"
permutation_key.bytes         = "00 21 00 00 02 00"
```

The stable string mapping for each enum (SPEC §5
`shader::to_string(Error)` is the precedent; per-axis equivalents are
emitted by the same hand-written `to_string(ShadingModel)` etc. at
plan time, not by `magic_enum`; error-model.md Open Question 1 tilts
toward hand-written for ABI predictability). The bytes field is the
authoritative round-tripable form, included in every log line that
mentions a key so post-mortem analysis can re-construct the value.

---

## 8. Hot-Reload

The permutation-key participates in hot-reload as a **carrying-state
value object** (hot-reload-protocol.md §State Survival Rules,
SPEC §8.2). Concretely:

1. **Keys survive plugin swap byte-equal.** A `PermutationKey`
   stored in another plugin's component storage (e.g. on a `Material`
   component in `render` that names the active permutation) survives
   a `shader` plugin swap with no transformation. The bit layout
   of `PermutationKey` is part of the `shader` plugin's ABI, but
   because the layout is fixed (4 enum bytes + 1 reserved padding;
   §3.1), swapping in a `shader.dylib` with the same `schema_abi_hash`
   keeps every held key valid.
2. **Re-validation on swap.** The hot-reload-protocol.md §Step 2
   swap step computes the incoming dylib's `glibre_types_abi_hash()`
   and compares it against the in-memory hash of the outgoing dylib
   (plugin-abi.md §Decision item 5). On match, all carried
   `PermutationKey` values are presumed valid and used as-is. On
   mismatch, the swap is *refused* with
   `core::Error::SchemaMigrationFailed` (hot-reload-protocol.md
   §Refusal Cases); the prior `shader.dylib` keeps running; carried
   keys remain bound to the prior dylib's enumerator semantics. This
   is the load-bearing forward-compatibility guarantee for keys
   under hot-reload.
3. **`migrate(...)` for the aggregate is a no-op.** The
   `migrate_PermutationKeyRecord_v1_to_v2` slot in the codegen
   dispatcher is empty at this revision (§7.3). Hot-reload
   re-validation never runs a transform on a held key; either the
   ABI matches (zero work) or the swap is refused.
4. **No observer-bus event.** The §8 hot-reload event surface for
   `shader` is `ShaderArtifactReplaced` (SPEC §8.5), which fires
   per-artifact, not per-key. A held `PermutationKey` does not
   trigger any event of its own when a plugin reloads; it is a
   passive coordinate.
5. **Refusal cases that can land here.**
   - Plugin-ABI hash mismatch (handled at the protocol layer; no
     key-side action required).
   - The new `shader.dylib` removes an enumerator a held key
     references — refused as ABI mismatch (rule §7.2 #1 forbids
     removal unless the schema also bumps; the schema bump rotates
     `schema_abi_hash`, so the protocol catches it).
   - The new `shader.dylib` adds a feature bit that a held key
     does not set — *no refusal*; older keys are zero-extended in
     the unused bit positions, still well-formed by §3.7.
6. **What the resolving plugin does.** The plugin holding a
   `PermutationKey` on one of its components (typically `render`'s
   `MaterialBinding`) rebinds the PSO table after the swap (SPEC
   §8.5 publishes `ShaderArtifactReplaced`); the key itself does not
   require any plugin-side action, because its bits do not change
   under a successful swap.

**No participation in `HotReloadBarrier::drain_in_flight`.** A held
`PermutationKey` is not a live request, a deferred message, or a
ref-counted resource; it has no in-flight state that needs to drain
(hot-reload-protocol.md §Step 1 — Drain).

---

## 9. Performance

The permutation-key aggregate's performance contract has two cells.
The **per-frame** cell is zero (the aggregate has no per-frame work
in the `shader` context's row in perf-budget.md §Per-Context Budget
Table). The **cook-time** cell is bounded by SPEC §9.2 (cook-time
budgets are out of frame-budget scope) but quoted here for the
implementer's reference.

### 9.1 Budget cells

| Operation                        | Path           | Wall-clock target | Allocator | Asserted by                                      |
|----------------------------------|----------------|-------------------|-----------|--------------------------------------------------|
| `to_bytes`                       | cold (cook)    | < 50 ns           | none      | unit micro-bench `bench_permutation_key_to_bytes` |
| `from_bytes` (success)           | cold (cook)    | < 80 ns           | none      | unit micro-bench `bench_permutation_key_from_bytes` |
| `from_bytes` (failure)           | cold (cook)    | < 80 ns           | none      | same; failure path measured separately            |
| `to_index`                       | cold (cook)    | < 30 ns           | none      | unit micro-bench `bench_permutation_key_to_index` |
| `from_index` (success)           | cold (cook)    | < 50 ns           | none      | unit micro-bench `bench_permutation_key_from_index` |
| `permutation_key_byte_less`      | cold (cook)    | < 20 ns           | none      | covered by the sort micro-bench                   |
| `is_well_formed`                 | mixed          | < 5 ns inline     | none      | static-call inlined; verified by `-fsave-optimization-record` artifact in CI |
| `operator==(PermutationKey, .)`  | hot-adjacent   | < 3 ns inline     | none      | covered by the `render` PSO-bind micro-bench (sub-epic #70 plan) |
| `FeatureSet::{test,set,bits}`    | hot-adjacent   | < 1 ns inline     | none      | constexpr; verified at compile time              |
| Construction (default / brace)   | hot-adjacent   | < 1 ns inline     | none      | trivially copyable; static_assert in §4 / §11    |

The numbers are wall-clock targets on the M1 firestorm core at
3.2 GHz (perf-budget.md baseline). Each is calibrated to leave at
least 10× margin under the engine-wide CPU-sim and CPU-submit
budgets.

### 9.2 Allocator rules

The aggregate is **strictly allocator-free** (perf-budget.md
§Allocator Rules row "ContextTag::shader → no per-frame allocation").
Every operation in §4 is a pure transform over scalar / fixed-array
data; there is no `eastl::vector` growth, no `eastl::string` copy,
no implicit heap usage. CI enforces this by:

- **`-fno-rtti -fno-exceptions`** on the `shader` plugin (set in the
  context's CMakeLists; perf-budget.md §CI Gate Spec).
- **clang-tidy** rule `glibre-no-allocations-in-permutation-key`
  (added by sub-epic #70 plan): forbids `new`, `delete`,
  `eastl::vector::reserve`, `eastl::string::reserve`,
  `std::pmr::*` references in `shader/src/permutation/`.

### 9.3 Cold-start cost

There is none. The aggregate has no construction cost beyond the
zero-init of a 6-byte struct; no global tables to populate at
`dlopen` time; no thread-local caches to seed.

### 9.4 CI gate

Two CI gates protect the budgets:

1. **Catch2 micro-benchmarks** under `tests/shader/permutation/bench/`,
   one per row in §9.1, run by `glibre-perf-harness` on the
   reference hardware. Failure to meet the target with 10×
   margin fails the gate.
2. **Codegen-table walk benchmark.** A single benchmark walks all
   12 288 well-formed permutations with `to_index` / `from_index`
   round-trips and asserts the wall-clock total is under 1 ms.
   This catches regressions that escape the per-op micro-benches
   (e.g. the inverse codec drifting toward a div-and-mod chain
   that the compiler stops folding).

### 9.5 Cross-references

- SPEC §9.2 (cook-time budgets out of frame-budget scope).
- perf-budget.md §Per-Context Budget Table row `shader`
  (0 ms / frame).
- perf-budget.md §Allocator Rules.
- frame-phases.md §Phase 7 (where the hot-adjacent operations are
  accounted to `render`'s budget, not `shader`'s).

---

## 10. Failure Modes

The aggregate surfaces exactly two `shader::Error` enumerators
(SPEC §5; this design's §4 boundary table). The mapping below pins
each to its trigger, the recovery the caller must execute, and the
SPEC §10.1 severity class.

| Enumerator                          | Trigger                                                                                       | Recovery                                                                             | Severity (§10.1) |
|-------------------------------------|-----------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------|------------------|
| `Error::PermutationKeyMalformed`    | `from_bytes` rejects: enumerator out of declared range; reserved bit non-zero; reserved byte non-zero (§3.5, §3.7). | refuse decode; the cache entry whose payload produced the bytes is quarantined and treated as `CacheCorrupt` at the cache layer (SPEC §10.2 row). | `refuse`         |
| `Error::PermutationKeyOutOfRange`   | `from_index` is invoked with `value >= kPermutationCrossProductCardinality` (§3.4).            | refuse compile; indicates codegen-table drift between the build that emitted the index and the build consuming it. SPEC §10.2 marks this `fatal`. | `fatal`          |

**Failure modes the §1 instructions name that are NOT in this
aggregate.** The executor instructions list four candidate failure
modes for "the permutation key concept": *BadEntryPoint*,
*DefineDuplicate*, *CapabilityUnknown*, *TargetUnsupported*. Per
SPEC §4.2 SRP and §1 of this design, the aggregate refuses ownership
of all four. Their actual homes:

| Instruction-named mode | Maps to SPEC §5 enumerator      | Owning aggregate (SPEC §)     |
|------------------------|---------------------------------|-------------------------------|
| BadEntryPoint          | `EntryPointMissing` /           | `ShaderSource` (§4.1) /       |
|                        | `EntryPointStageAmbiguous`      | `CompilationPipeline` (§4.3)  |
| DefineDuplicate        | (subsumed by) `CompilerInvocationFailed` / `CompilerExitNonZero` | `CompilationPipeline` flag canonicalizer (§4.3 inv 4) |
| CapabilityUnknown      | `CapabilityNotSupported`        | `IShaderBackend` (§4.7)       |
| TargetUnsupported      | `UnsupportedTarget`             | `CompilationPipeline` (§4.3) / `IShaderBackend::capabilities()` (§4.7) |

The permutation-key aggregate **does not detect, raise, or surface
any of those four**; it is a dumb coordinate. Detection and
recovery for them belong to the named owners above. This is the SRP
boundary. The instruction-list is treated as a
prompt-time hint that anchors the right sub-epic; the SPEC is
authoritative.

**Recovery summary.** Both aggregate-owned enumerators are
recoverable in the SPEC §10.1 sense:

- `PermutationKeyMalformed` is a `refuse` — the caller (`ShaderCache`
  on a manifest load, or `CompilationPipeline` on a key arriving
  from a malformed index) returns it through `std::expected` and
  the build halts on the failed entry; the rest of the build
  continues.
- `PermutationKeyOutOfRange` is a `fatal` — codegen-table drift
  cannot be recovered in-process; the cooker re-runs from clean
  and the offending build artifact is discarded.

Neither is a hot-reload refusal arm directly; both surface upstream
into `core::Error::SchemaMigrationFailed` if they fire during a
swap (hot-reload-protocol.md §Refusal Cases via SPEC §8.4).

**No silent retry.** The aggregate executes no retry loop on either
enumerator. A `from_bytes` failure is final; a `from_index` failure
is final. Retries (if any) happen at the cooker / loader layer that
called us, never inside the aggregate (SPEC §10 §10.1 contract).

---

## 11. Test Plan

Every test is Catch2 (`reviews/decisions/error-model.md` chooses
Catch2; perf-budget.md §CI Gate Spec wires it). Tests live under
`tests/shader/permutation/` in three buckets: unit (canonicalization
goldens), integration (cache-key round-trip with `ShaderCache`), and
benchmark (the §9 budget gates). All test names are stable contract
strings — they are referenced by the acceptance criteria for SPEC
user story #323 (SPEC §11; matched here for cross-walk).

### 11.1 Unit suite — canonicalization & well-formedness

Location: `tests/shader/permutation/test_permutation_key_unit.cpp`.

| Catch2 test name                                                            | What it asserts                                                                 |
|-----------------------------------------------------------------------------|---------------------------------------------------------------------------------|
| `permutation_key_is_trivially_copyable`                                     | `static_assert(std::is_trivially_copyable_v<PermutationKey>)` and `sizeof <= 8`. |
| `permutation_key_default_construction_is_well_formed`                       | Default-constructed key satisfies `is_well_formed()` and round-trips.            |
| `permutation_key_to_bytes_is_total_and_deterministic`                       | For all 12 288 well-formed cross-product keys, `to_bytes()` produces 12 288 distinct 6-byte sequences (total injectivity). |
| `permutation_key_from_bytes_round_trip`                                     | `from_bytes(to_bytes(k)) == k` for all well-formed `k`.                          |
| `permutation_key_from_bytes_rejects_out_of_range_shading_model`             | Bytes with `byte[0] == 8 .. 0xFF` → `Error::PermutationKeyMalformed`.            |
| `permutation_key_from_bytes_rejects_out_of_range_render_path`               | Bytes with `byte[3] == 6 .. 0xFF` → `Error::PermutationKeyMalformed`.            |
| `permutation_key_from_bytes_rejects_out_of_range_lod_tier`                  | Bytes with `byte[4] == 4 .. 0xFF` → `Error::PermutationKeyMalformed`.            |
| `permutation_key_from_bytes_rejects_reserved_feature_bits`                  | Bytes with any bit in `byte[1..2]` outside `kFeatureBitMask` → malformed.        |
| `permutation_key_from_bytes_rejects_reserved_padding_byte`                  | Bytes with `byte[5] != 0` → malformed.                                           |
| `permutation_key_byte_layout_golden`                                        | Hard-coded golden: `(Skin, Skinned|MotionVectors, Deferred, HighEnd)` encodes to the exact 6 bytes `01 03 00 01 03 00`. Frozen for ABI. |
| `permutation_key_equality_equals_byte_equality`                             | For 1 024 random well-formed pairs, `(a == b) ⇔ (a.to_bytes() == b.to_bytes())`. |
| `permutation_key_byte_comparator_matches_lex_order`                         | `permutation_key_byte_less` is a strict total order and matches lexicographic 6-byte order. |
| `permutation_index_to_index_is_bijection`                                   | For all 12 288 well-formed keys, `to_index` produces 12 288 distinct ordinals in `[0, 12288)`. |
| `permutation_index_from_index_round_trip`                                   | `from_index(to_index(k)) == k` for all well-formed `k`.                          |
| `permutation_index_from_index_rejects_overflow`                             | `from_index({12288})` → `Error::PermutationKeyOutOfRange`; same for `UINT32_MAX`. |
| `permutation_index_iteration_order_matches_canonical`                       | Iteration `to_index → from_index` for `i = 0..12287` produces keys in the SPEC §4.2 inv 2 order: `(sm, features, path, lod)` ascending. |
| `permutation_key_is_well_formed_predicate_truth_table`                      | `is_well_formed()` returns `false` for hand-crafted `static_cast`-malformed keys (one per axis).|
| `feature_set_test_set_bits_round_trip`                                      | `FeatureSet::test(b)` agrees with `bits()`; `set` is idempotent.                 |

### 11.2 Integration suite — cache-key round-trip with `ShaderCache`

Location: `tests/shader/permutation/test_permutation_key_cache_integration.cpp`.

These integration tests live in this aggregate's test directory but
exercise the seam between `PermutationKey` and `ShaderCache` (sub-epic
#70 spike #755). They depend on the `ShaderCache` MVP being landed; if
this design lands first (which is the expected order; spike #755 is
sibling), the tests are gated by `[!hide]` until #755 closes.

| Catch2 test name                                                            | What it asserts                                                                                |
|-----------------------------------------------------------------------------|------------------------------------------------------------------------------------------------|
| `permutation_key_round_trips_through_shader_cache_manifest`                 | A `PermutationKey` written into a `ShaderCacheManifest` via Fory and read back is byte-equal to the source key. |
| `permutation_key_contributes_six_bytes_to_shader_hash`                      | The `ShaderHash` BLAKE3 input has the key's `to_bytes()` at the documented offset (SPEC §7.3 #1) and changes when any axis changes. |
| `permutation_key_change_invalidates_shader_hash`                            | For each axis, mutating that axis in a held key produces a different `ShaderHash` (sensitivity). |
| `permutation_key_with_reserved_bit_set_fails_manifest_load`                 | Hand-crafted manifest payload with a `FeatureSet` reserved bit set is rejected with `Error::CacheIntegrity` (the loader bubbles `PermutationKeyMalformed` up). |
| `permutation_key_manifest_sort_is_byte_lex`                                 | `ShaderCacheManifest.enumerated_keys` produced from a shuffled input is sorted ascending by `permutation_key_byte_less`. |
| `permutation_key_survives_plugin_swap_byte_equal`                           | Held `PermutationKey` on a `render` `MaterialBinding` survives a `shader.dylib` swap with no transformation; bytes are identical pre- and post-swap. **Migration note:** this test exercises a cross-plugin hot-reload contract (render `MaterialBinding` + `shader.dylib` swap) and is a stub here until a dedicated cross-plugin harness exists; intended destination is `tests/cross-plugin/` once that harness lands. |

### 11.3 Benchmark suite — §9 budget gates

Location: `tests/shader/permutation/bench_permutation_key.cpp`.

| Catch2 benchmark name                                                       | What it measures                                                                 |
|-----------------------------------------------------------------------------|----------------------------------------------------------------------------------|
| `bench_permutation_key_to_bytes`                                            | Wall-clock per call; target < 50 ns (§9.1).                                      |
| `bench_permutation_key_from_bytes_success`                                  | Wall-clock per call (well-formed input); target < 80 ns.                         |
| `bench_permutation_key_from_bytes_failure`                                  | Wall-clock per call (rejected input); target < 80 ns.                            |
| `bench_permutation_key_to_index`                                            | Wall-clock per call; target < 30 ns.                                             |
| `bench_permutation_key_from_index_success`                                  | Wall-clock per call (in-range); target < 50 ns.                                  |
| `bench_permutation_key_byte_less_sort_12288`                                | Time to sort 12 288 keys via `permutation_key_byte_less`; target < 1 ms.         |
| `bench_permutation_key_full_cross_product_walk`                             | Walk all 12 288 well-formed permutations with `to_index` / `from_index`; total < 1 ms (§9.4). |
| `bench_permutation_key_equality_inline_no_alloc`                            | Equality compare in a tight loop produces zero allocations under the asan/jemalloc-stats harness. |

### 11.4 Story link

SPEC §11 names this aggregate's user story as #323 with Catch2
contract `permutation_key_encoding_is_total_injective_and_bit_stable`.
That contract is the conjunction of the unit tests
`permutation_key_to_bytes_is_total_and_deterministic`,
`permutation_key_from_bytes_round_trip`, and
`permutation_key_byte_layout_golden`; the acceptance harness for
#323 runs all three in series and reports a single pass/fail. The
implementation plan that closes #323 wires this aggregation explicitly.

---

## 12. Open Questions

- [OPEN] **`std::hash<PermutationKey>` specialization** — should the
  aggregate ship one for ergonomics with `std::unordered_*` /
  `eastl::hash_*`, accepting the policy lock-in (FNV-1a vs xxhash
  vs Fibonacci of a `uint64_t` view of the bytes), or stay
  policy-free? Resolution gate: sub-epic #70 plan that lands
  `ShaderCache::lookup` (#755). Tilt: stay policy-free; cooker
  hashes the bytes itself.
- [OPEN] **`source_location` in `from_bytes` failure path** —
  error-model.md Open Question 3 hedges on `std::source_location`
  vs manual `__FILE__`/`__LINE__` for `ErrorContext`. Resolution
  gate: when `core/error.hpp` lands. Tilt: `source_location` once
  release-build overhead is verified; until then this aggregate
  passes the default `ErrorContext{}`.
- [OPEN] **Hand-written vs `magic_enum` for axis stable strings** —
  inherits error-model.md Open Question 1 (`magic_enum` vs
  hand-written `to_string`). Resolution gate: same as above. Tilt:
  hand-written for ABI predictability (§7.4 logging contract).
- [OPEN] **Per-mesh feature-bit caps** — harmonius `R-2.3.10.3`
  imposes per-mesh / per-shading-model / per-render-path / per-
  project caps that this aggregate refuses to enforce (§2 row).
  Sub-epic #70 plan must decide whether the cooker enforces the
  budgets or not, and where the failure surfaces. Resolution gate:
  the enumeration plan in sub-epic #70.
- [OPEN] **Reserved-byte assignment ahead of need** — the
  `PackedBytes` reserved byte at offset 5 is ear-marked for an
  additive future axis (§3.5), but no concrete axis is on the
  roadmap. If a `MaterialFamily` axis or `BlendingTier` axis
  emerges, that sub-epic owns layout assignment. Resolution gate:
  any future sub-epic that proposes a 5th axis files a SPEC
  amendment that pins offset 5's bit / byte breakdown.
- [OPEN] **Forward-compatibility test for an unknown 7th
  feature bit** — should the unit suite include a "hypothetical
  future bit" test that hand-crafts a `FeatureSet` with bit 6 set
  and asserts `from_bytes` rejects with
  `Error::PermutationKeyMalformed`? The suite does have
  `permutation_key_from_bytes_rejects_reserved_feature_bits`
  (§11.1) covering this generically; whether to add a named
  golden anchored at bit 6 specifically is a style call.
  Resolution gate: code review of the implementation plan.
- [OPEN] **`static_assert(kFeatureBitCount <= 8)` bit-count guard** —
  the §3.6 warning section mandates a `static_assert` in the
  implementation file guarding the invariant that byte order and
  mixed-radix index order coincide only while `kFeatureBitCount <= 8`.
  This open item tracks the concrete steps required when that guard
  trips: (a) verify the mixed-radix index formula (§3.4); (b) update
  `permutation_key_byte_comparator_matches_lex_order` in the unit
  suite; (c) audit cooker code that assumed the two orders are
  equivalent. Resolution gate: whichever implementation plan
  introduces the 9th feature bit must close this item first.

---

## Cross-References

- **SPEC sections this design refines.** `specs/shader/SPEC.md`
  §4.2 (aggregate definition), §5 lines 523–589 (public surface),
  §7.1 / §7.2 (`PermutationKeyRecord`), §7.3 #1, §7.3 #6, §7.4 #1,
  §9 (per-frame budget cell), §10.2 rows `PermutationKeyMalformed`
  / `PermutationKeyOutOfRange`, §11 user story #323.
- **Engine-wide decisions consumed.**
  `reviews/decisions/error-model.md` (closed-sum errors via
  `std::expected<T, glibre::Error>`),
  `reviews/decisions/perf-budget.md` (allocator rules; per-context
  zero-frame budget for `shader`),
  `reviews/decisions/plugin-abi.md` (middleman ABI hash, `dlopen`
  symbol stability),
  `reviews/decisions/hot-reload-protocol.md` (Step-2 ABI hash check;
  no per-key migration),
  `reviews/decisions/fory-codegen.md` (Fory schema for
  `PermutationKeyRecord`; `glibre_types_abi_hash`),
  `reviews/decisions/frame-phases.md` (hot-adjacent equality
  accounted to `render` phase 7).
- **Sibling aggregates this design depends on or seams against.**
  `ShaderSource` (SPEC §4.1, spike #745 — owns source ingestion;
  the key references its `source_hash` only),
  `CompilationPipeline` (SPEC §4.3, spike #749 — consumes the key
  as one of three compile inputs; owns flag canonicalization and
  define translation),
  `ShaderCache` (SPEC §4.6, spike #755 — combines the key into
  `ShaderHash` and stores the resulting CAS entry; owns persistence
  of the full artifact tuple).
- **Harmonius prior art (research input only).**
  `harmonius/docs/design/rendering/shader-variants.md`,
  `harmonius/docs/design/rendering/shader-variants-test-cases.md`.
  Every clause is routed in §2; every refusal carries a
  glibre-side rationale.
