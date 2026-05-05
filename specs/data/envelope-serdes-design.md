# Envelope-Serdes Detailed Design

> Detailed design for the `data` context's envelope-serdes aggregate
> (`Envelope` / `EnvelopeHeader`, SPEC §4.8). Refines §5 §envelope.hpp,
> §7.2.4, and §7.5 of `specs/data/SPEC.md` against the
> `reviews/decisions/{fory-codegen,error-model,plugin-abi,
> hot-reload-protocol,perf-budget,frame-phases}.md` decision records.
> All conclusions independently re-derived; harmonius prior art
> (`harmonius/docs/design/core-runtime/reflection-serialization.md`
> §"Binary Serialization", `BinaryHeader`) cited as research input
> only.

Refs: spike #736 — `[SPIKE] design-data-envelope-serdes-detailed`.
Parent sub-epic #729. Sibling task-breakdown spike blocked-by this
deliverable.

## 1. Purpose

The envelope-serdes aggregate is the **single boundary** at which
every persisted Fory blob crosses the wire. Its one responsibility
is to **encode and decode the fixed-width envelope header that wraps
every payload** and to **dispatch the wrapped payload by version to
the migration dispatcher (§4.7) when the inbound version is older
than the live registry's current version**.

Concretely, `Envelope<T>` is the only call site at which:

1. Magic bytes are written and verified.
2. The schema FQN id and version of a payload are read or written.
3. The `source-blake3` content-addressing digest is stamped or
   checked.
4. The payload's byte length is recorded and bounded against the
   inbound span.
5. Per-version dispatch is performed: equal-version → direct decode;
   older-version → `MigrationDispatcher::dispatch` (§6.3); newer-than-
   host → typed refusal.

What the aggregate explicitly **refuses** to own:

- **Raw Fory codecs.** Per-type `serialize_<fqn>` /
  `deserialize_<fqn>` bodies are emitted by `glibre-foryc`
  (`fory-codegen.md` §"Decision" #4) and exposed through the
  `RegistryEntry::{serialize,deserialize}` trampolines (SPEC §5
  §registry.hpp). The envelope is the **outer** wrapper around those
  codecs; their byte format is owned by Fory.
- **Schema registry.** Lookup by `SchemaId` is owned by
  `SchemaRegistry::lookup` (SPEC §4.5; sibling spike #730). The
  envelope reads the registry **read-only** at deserialize time and
  never mutates it.
- **Migration logic bodies.** Per-step `MigrationFn` bodies live in
  the originating context (`fory-codegen.md` §"Migration Mechanic";
  sibling spike #738). The envelope only **dispatches** to the
  composed chain held by the registry entry; it never composes,
  validates, or executes a migration step itself.
- **CRC / checksum trailing.** A per-payload CRC is **not** in MVP
  scope — `source-blake3` already content-addresses the schema; an
  end-to-end CRC over the payload bytes is a post-MVP question
  (§12 [OPEN] #2). The envelope reserves the bit pattern.
- **Frame-phase scheduling.** Per `frame-phases.md`, the envelope
  has no phase ownership; encode/decode runs on whatever thread the
  caller is on, gated by §6.

The aggregate's SRP boundary is sharp: if the envelope's byte layout,
the magic value, the FQN-id encoding, the source-blake3 stamping
rule, the version-dispatch decision tree, the endianness rule, the
alignment rule, or the truncation-detection algorithm changes, this
design changes. Anything else is out of scope.

## 2. Requirements Coverage

Mapping of harmonius requirements
(`harmonius/docs/design/core-runtime/reflection-serialization.md`
§"Binary Serialization", `BinaryHeader`) and SPEC §4.8 / §7.2.4 /
§7.5 invariants to glibre MVP coverage. Every entry is independently
re-derived.

| Source                                                   | Glibre disposition (MVP) | Coverage site                                                                          |
|----------------------------------------------------------|--------------------------|----------------------------------------------------------------------------------------|
| Harmonius `BinaryHeader.magic = b"HSER"`                 | **Re-derived**           | §3.1 — magic = `b"GLEN"` (Glibre ENvelope); rationale in §3.1.                         |
| Harmonius `BinaryHeader.header_version` (u8 + 3B pad)    | **Refused**              | Replaced by build-time meta-schema bootstrap (SPEC §7.6); single live envelope shape per `glibre-foryc` release. |
| Harmonius `BinaryHeader.type_id_hash : u64`              | **Re-derived (narrowed)**| §3.2 — `schema_fqn_id : u32` keyed against the `SchemaRegistry`'s static FQN table; 32 bits suffice for MVP type counts (≤ 65k schemas, §3.2). |
| Harmonius `BinaryHeader.schema_version : SchemaVersion`  | **Covered**              | §3.1 — `version : u32` little-endian (SPEC §4.8 inv. 3).                               |
| Harmonius `BinaryHeader.payload_len : u64`               | **Re-derived (narrowed)**| §3.1 — `payload_len : u32`; rationale in §3.1 (max 4 GiB per blob, single-pass).       |
| Harmonius "rkyv outer envelope" (zero-copy access)       | **Refused**              | Fory is the codec (`fory-codegen.md`); zero-copy is post-MVP (§12 [OPEN] #4).          |
| Harmonius `MigrationRegistry` runtime dispatch           | **Covered (delegated)**  | Owned by `MigrationDispatcher` (sibling #738); envelope only **routes** by version (§3.4). |
| SPEC §4.8 inv. 1 (self-describing)                       | **Covered**              | §3.1 layout — every dispatch input is in the header bytes.                              |
| SPEC §4.8 inv. 2 (fixed-prefix, peekable)                | **Covered**              | §3.1 — header is exactly 48 bytes; `peek_header` returns without consuming the payload (§4.2). |
| SPEC §4.8 inv. 3 (little-endian)                         | **Covered**              | §3.1 — every multi-byte field is LE; §3.5 endianness rule.                              |
| SPEC §4.8 inv. 4 (round-trip identity)                   | **Covered**              | §3.6 round-trip rule; §11 unit + integration tests.                                     |
| SPEC §4.8 inv. 5 (version-driven dispatch)               | **Covered**              | §3.4 dispatch decision tree.                                                            |
| SPEC §7.2.4 (Fory-defined byte shape)                    | **Re-derived (refined)** | This design refines §7.2.4 by replacing `fqn:string` with `schema_fqn_id:u32` + adding `source_hash:32B` and `magic:4B`. Divergence flagged as §12 [OPEN] #1; SPEC §7.2.4 amendment proposed. |
| SPEC §7.5 (per-version goldens)                          | **Covered**              | §11 test plan — golden harness per schema and per version.                              |
| SPEC §10 `EnvelopeTruncated`, `DeserializeError`, `SchemaUnknown`, `MigrationStepMissing` | **Covered** | §10 failure modes table; payload field population per SPEC §10.2.                       |
| `error-model.md` `std::expected<T, glibre::Error>` boundary | **Covered**           | §4 surface — every public function returns `std::expected`; no exceptions cross.        |
| `plugin-abi.md` `glibre_types_serialize_<fqn>` C trampolines | **Covered**            | §3.3 — `Envelope<T>::serialize` / `deserialize` are typed wrappers over those trampolines. |
| `hot-reload-protocol.md` "Step 3 — Migrate" reuse        | **Covered**              | §8 — same code path runs cold deserialize and hot-reload migrate; envelope is unaware of the phase. |
| `perf-budget.md` `data` row 0.20 ms / 32 MiB             | **Covered**              | §9 — per-call wall-time per-KB bound; 16 MiB scratch sub-ceiling.                       |
| `frame-phases.md` phase ownership                        | **Refused (none)**       | The envelope owns no phase; it is a synchronous codec called from any phase the caller is on (§6). |

Coverage rule: every harmonius clause and every SPEC invariant either
lands in this design or is refused with a one-line rationale. No
silent drops.

Glibre-native obligations beyond harmonius:

- **Magic bytes are non-optional.** Harmonius's `BinaryHeader` had
  magic; rkyv's archive format does not. We retain magic
  unconditionally because it is the cheapest possible defence
  against passing the wrong byte buffer to `deserialize` (§3.5
  rationale).
- **Source-blake3 is in the header, not the payload.** Harmonius
  carried no per-payload schema content-hash on the wire. We add
  it: it lets a reader detect "schema FQN matches but its `.fory`
  source has drifted since this byte was stamped" — a subtle
  failure rkyv hides because rkyv's layout-validation is structural,
  not source-equivalence.
- **No header_version field.** Harmonius reserved one byte for
  in-process header evolution. Glibre's meta-schema bootstrap rule
  (SPEC §7.6) collapses this onto a `glibre-foryc` release boundary:
  one envelope shape per release, the AbiHash gate forces every
  plugin to rebuild on bump. The `flags` u32 in §3.1 is reserved for
  future bit-additive extensions that do not change layout.

## 3. Detailed Model

### 3.1 Envelope byte layout

The envelope is a **fixed 48-byte little-endian header** followed by
a Fory-encoded payload of `payload_len` bytes. The header layout is
authored *not* through a `.fory` schema — per SPEC §7.2.4 the header
is "Fory-defined" and rides Fory's own version handling — but its
observable byte shape is pinned here so readers and writers across
the engine and the cooker agree byte-for-byte.

```text
offset  size  field            description
------  ----  ---------------  ----------------------------------------
  0      4    magic            ASCII bytes 'G','L','E','N' (0x47 4C 45 4E)
  4      4    schema_fqn_id    u32 LE — index into SchemaRegistry's
                               FQN-sorted entries() span (§3.2)
  8      4    version          u32 LE — SchemaVersion of the payload
                               (SPEC §4.1 inv. 2)
 12     32    source_hash      Blake3-256 of the canonicalized .fory
                               source (SPEC §7.3) at the time of write;
                               equals SchemaRegistry's recorded hash for
                               (schema_fqn_id, version) on a same-build
                               round-trip
 44      4    payload_len      u32 LE — Fory-encoded body size in bytes,
                               not including the 48-byte header
------  ----  ---------------  ----------------------------------------
 48     payload_len  payload   Fory-encoded body of the Generated Type
                               at SchemaVersion `version`.
```

Total wire size: `48 + payload_len` bytes. No trailing CRC, no
trailing pad, no length prefix outside `payload_len`.

**Constants.**

| Symbol                          | Value                                                          |
|---------------------------------|----------------------------------------------------------------|
| `kEnvelopeMagic`                | `{ 0x47, 0x4C, 0x45, 0x4E }` — ASCII `"GLEN"`                  |
| `kEnvelopeHeaderSize`           | `48` (bytes, exact)                                            |
| `kSourceHashSize`               | `32` (bytes — Blake3-256)                                      |
| `kMaxPayloadBytes`              | `4'294'967'295` (u32 max)                                      |
| `kEnvelopeHeaderAlignment`      | `4` bytes (every multi-byte field is 4-byte aligned in-buffer) |

**Why `"GLEN"`.** Four ASCII bytes that are (a) printable, so a hex
dump is human-recognizable, (b) distinct from harmonius's `"HSER"`
to avoid accidental cross-engine load (the byte streams must never
mix), (c) distinct from any common archive magic (`PK`, `7z`, `BZ`,
ELF `\x7FELF`, Mach-O `\xFE\xED\xFA\xCE`), so a naive `file(1)` does
not misidentify the byte run.

**Why u32 `payload_len`.** The 4 GiB cap is the largest single asset
the engine commits to round-tripping in one envelope; assets beyond
this size split at the content layer (post-MVP — `content/` SPEC).
Narrowing from harmonius's u64 saves 4 bytes per envelope and aligns
with the §9 per-call budget (per-KB encode/decode time linear in
`payload_len`).

**Why u32 `schema_fqn_id` (vs harmonius's u64 `type_id_hash`).** The
registry's `entries()` span (SPEC §4.5; FQN-sorted, populated at
static-init) is the canonical FQN universe for any one middleman
build. A u32 index is sufficient (the engine's MVP schema set is
≤ ~10⁴; even at 10⁶ schemas u32 suffices). The id is **build-local**:
two middleman builds with non-identical schema sets do not share id
namespaces. The `source_hash` field is the cross-build invariant —
see §3.2 for the discriminator pair.

**Why `source_hash` in the header (vs harmonius's omission).** The
hash is the wire-level proof that the bytes were stamped against a
specific `.fory` source. Without it, two builds that happen to share
`schema_fqn_id == K` but disagree on the schema's content (e.g. one
inserted a field at a tag the other has not declared) would silently
mis-decode. With it, a reader detects the drift and refuses with
`SourceHashMismatch` (§10 arm 10).

### 3.2 The `(schema_fqn_id, source_hash)` identity pair

`SchemaRegistry::entries()` returns the FQN-sorted span of
`RegistryEntry`s populated at static-init by
`_registry.cpp` (SPEC §6.1.3). The envelope encodes the **entry's
ordinal index** in that span as `schema_fqn_id`. Two reader rules
make this safe across builds:

1. **Same-build round-trip.** A reader operating against the same
   middleman build that wrote the bytes resolves
   `entry := registry.entries()[schema_fqn_id]` and verifies
   `entry.source_hash == header.source_hash`. Mismatch is impossible
   in this case (the hash is taken from the same registry); the
   check is a defensive assertion in debug builds and elided in
   shipping.
2. **Cross-build read.** A reader operating against a different
   middleman build (e.g. loading a save file from a previous engine
   release) cannot trust the `schema_fqn_id` ordinal because the
   FQN-sorted list may have grown or shrunk. The reader **falls
   back to source-hash lookup**: a secondary registry index keyed by
   `source_hash → RegistryEntry*` is built at static-init alongside
   the primary FQN-sorted span (SPEC §4.5 inv. 3 already permits
   secondary indices). If the source-hash is present in the live
   registry, decode proceeds against that entry; if absent,
   `SourceHashMismatch` (§10).

The two-mode behavior requires no flag bit (the trigger
is structural: the reader compares `schema_fqn_id` against
`registry.entries().size()` and against `entry.source_hash`).
Default behavior is "use the ordinal; verify the hash"; the fallback
fires only on hash-disagreement.

This collapses two failure modes (forged FQN ordinal, schema source
drift) onto one decision and one error arm
(`SourceHashMismatch`).

### 3.3 The `Envelope<T>` typed wrapper

```cpp
// data/runtime/include/glibre/types/envelope.hpp — public.
namespace glibre::types {

// 48-byte fixed header — see §3.1 layout. Trivially copyable, POD-
// friendly. Never holds owning storage.
struct EnvelopeHeader {
    eastl::array<std::byte, 4> magic{};               // = kEnvelopeMagic (PHILOSOPHY §11)
    std::uint32_t              schema_fqn_id{0};
    SchemaVersion              version{0};             // alias for u32
    SchemaSourceHash           source_hash{};          // 32 bytes
    std::uint32_t              payload_len{0};
};
static_assert(sizeof(EnvelopeHeader) == 48);
static_assert(std::is_trivially_copyable_v<EnvelopeHeader>);
static_assert(alignof(EnvelopeHeader) <= 4);

// Per-FQN typed wrapper. Specializations are emitted by glibre-foryc
// in <glibre/types/<ctx>/<Type>.hpp>; primary template left undefined
// (link-time error if no schema for T).
template <class T>
struct Envelope {
    // Encode `value` into `dst` starting at offset 0. Writes the
    // 48-byte header, then invokes the codegen-emitted Fory body
    // serializer at offset 48. Returns the total bytes written
    // (== 48 + payload_len).
    //
    // Failure modes: BufferTooSmall (§10).
    [[nodiscard]] static auto serialize(
        const T&             value,
        std::span<std::byte> dst
    ) noexcept -> std::expected<std::size_t, data::Error>;

    // Decode `src`. Reads the 48-byte header, dispatches by version
    // (§3.4), invokes the registry's deserialize trampoline (§3.3)
    // and the migration dispatcher (§6.3) as needed. Output value is
    // stored in `out`.
    //
    // Failure modes: BadMagic (discriminator only — surfaces as
    // EnvelopeTruncated arm 9), EnvelopeTruncated, SchemaUnknown,
    // VersionUnsupported, PayloadTruncated (discriminator only —
    // surfaces as EnvelopeTruncated arm 9), DeserializeError,
    // SchemaMigrationFailure, MigrationStepMissing (§10).
    [[nodiscard]] static auto deserialize(
        std::span<const std::byte> src,
        T*                          out
    ) noexcept -> std::expected<void, data::Error>;

    // Compute the exact wire byte count for `value` without
    // performing the serialize. Used by callers sizing pre-allocated
    // buffers (e.g. content/ asset writers). O(value-size) — same
    // walk Fory does, no memcpy.
    [[nodiscard]] static auto size_bytes(const T& value) noexcept
        -> std::expected<std::size_t, data::Error>;
};

// Type-erased peek. Returns the parsed header without consuming the
// payload. Used by save-file readers to triage an unknown FQN
// before allocating a full T.
[[nodiscard]] auto peek_header(std::span<const std::byte> src) noexcept
    -> std::expected<EnvelopeHeader, data::Error>;

}  // namespace glibre::types
```

**Deserialize signature — out-param vs value-return (SPEC §5 amendment).** The
SPEC §5 stub declares `deserialize(src) -> std::expected<T, data::Error>` (value
return). This design changes the signature to `deserialize(src, T* out) ->
std::expected<void, data::Error>` (out-param). The rationale:

1. **No copy/move of T at the call site.** Generated types can be large (e.g.
   meshes, animation tables); a value-return form forces a move of T through
   `std::expected<T, ...>`, which cannot be elided when the expected is
   conditionally returned. The out-param form writes `T` in-place with no
   temporary.
2. **Caller-controlled lifetime.** Save-file loaders pre-allocate the target
   value slot; the out-param form writes directly into it without routing
   through `std::expected`'s internal storage.
3. **Consistent failure isolation.** The out-param is left unmodified on any
   failure (§3.4 decision tree); this invariant is easier to audit in the
   out-param form than in the value-return form where the caller must discard
   the entire expected on error.

The `error-model.md` §"Composition Rules" #3 mandates `std::expected` for the
return type but does not prescribe whether T is the value or void — both forms
satisfy the rule. See §12 [OPEN] #7 for the formal SPEC §5 amendment tracking
this signature change; the amendment ships with the first implementation PR
that introduces `data/runtime/src/envelope.cpp`.

Borrow rules:

- `serialize` writes only into `dst`; the input `value` is borrowed
  read-only. No allocation.
- `deserialize` reads only from `src`; `out` is written exactly
  once on success. On failure `out` is left **unmodified** (the
  caller's prior contents persist) — no partial decode escapes. The
  single legal exception is intermediate scratch the migration
  dispatcher writes through `Arena&`; that storage is reset between
  steps and never aliases `out`.
- `peek_header` allocates nothing; the returned `EnvelopeHeader` is
  a value (48 bytes copied out of `src`).

### 3.4 Version-dispatch decision tree

`Envelope<T>::deserialize` runs the following decision tree, in
order, on every call. Each step is constant-time except step 5,
whose cost is proportional to `entry.version - header.version` (i.e.
the migration chain length).

```text
1. BoundsCheck(src.size() >= kEnvelopeHeaderSize)
   └── false → EnvelopeTruncated{ at.offset = src.size() }

2. ReadHeader(src[0..48]) into hdr
   └── hdr.magic != kEnvelopeMagic → BadMagic{ at.offset = 0 }

3. BoundsCheck(48 + hdr.payload_len <= src.size())
   └── false → PayloadTruncated{ at.offset = src.size() }

4. RegistryLookup:
   if hdr.schema_fqn_id < registry.entries().size():
       entry := registry.entries()[hdr.schema_fqn_id]
       if entry.source_hash != hdr.source_hash:
           goto FallbackBySourceHash
   else:
       goto FallbackBySourceHash

   FallbackBySourceHash:
       entry := registry.lookup_by_source_hash(hdr.source_hash)
       if entry == nullptr → SchemaUnknown{ at.offset = 0, at.schema = empty, at.version = hdr.version }
       // SchemaUnknown (arm 6): hash-fallback miss means "schema not found by any identity" —
       // semantically identical to the ordinal-miss case above. SourceHashMismatch (arm 10)
       // is reserved for the Mode-A barrier-diff site at phase-8 step 2 (see §10 and §12 [OPEN] #9).

5. VersionDispatch:
   if hdr.version > entry.version → VersionUnsupported (newer-than-host)
   if hdr.version == entry.version:
       call entry.deserialize(src.subspan(48, hdr.payload_len), out)
       └── Fory body returned err → DeserializeError{ at.offset = 48 + err.byte_offset }
   if hdr.version < entry.version:
       call MigrationDispatcher::dispatch(entry, hdr.version, src.subspan(48, hdr.payload_len), out, arena)
       └── chain step missing → MigrationStepMissing{ step_from = ?, step_to = ? }
       └── chain step refused → SchemaMigrationFailure{ step_from, step_to }

6. Success: return {}
```

**Decision-tree invariants.**

- The magic check (step 2) is strictly before the registry lookup.
  Garbage bytes cannot reach the registry.
- The bounds check (step 3) is strictly before the deserialize call.
  Fory's body decoder is never given a span longer than
  `payload_len` even if the surrounding `src` carries trailing data
  (e.g. a save-file packed with multiple envelopes back-to-back).
- The source-hash fallback (step 4) is invisible to callers — they
  observe either a successful decode or one of the §10 typed
  failures. The decision between "ordinal lookup" and "hash lookup"
  is internal.
- The migration dispatcher (step 5 older-version arm) **is the same
  code path** the hot-reload barrier invokes (`hot-reload-protocol.md`
  §"Step 3 — Migrate"); the envelope is unaware of which caller is
  driving it. See §8.

### 3.5 Endianness, alignment, padding

**Endianness.** Every multi-byte field in the header is **little-
endian**, unconditionally, on every host. PHILOSOPHY §7 mandates
byte-equal snapshots across hosts, and the engine baseline is Apple
Silicon (LE-native), so the rule is "store LE; on a hypothetical BE
host the load/store path performs an unconditional byte swap." The
implementation uses `std::byteswap` (C++23) wrapped in a
`load_le` / `store_le` helper; on LE hosts the swap is a no-op
inlined to a register move.

The Fory-encoded payload (offset 48 onwards) is governed by Fory's
own endianness rule (`fory-codegen.md` §"Decision" #4); per the
codegen contract Fory emits LE on every host so the whole envelope
is LE-uniform.

**Alignment.** The `EnvelopeHeader` struct is 4-byte aligned in C++
(every field is `u32` or smaller, plus a 32-byte `array`). In the
wire image, the 48-byte header is *naturally* aligned to a 4-byte
boundary if the buffer's start is 4-byte aligned. Callers pass byte
spans to `serialize` / `deserialize`; the envelope never assumes
buffer alignment — it `memcpy`s the field bytes into a local
`EnvelopeHeader` and reads from there (cheap on Apple Silicon; one
unaligned 48-byte memcpy per call, ~3 ns).

**Padding.** None. The 48-byte header packs without internal
padding because every field's natural alignment ≤ 4 and the field
order respects 4-byte boundaries:

```text
[ 0..4)   magic           (4B aligned to 4)
[ 4..8)   schema_fqn_id   (4B aligned to 4)
[ 8..12)  version         (4B aligned to 4)
[12..44)  source_hash     (32B aligned to 4 — array of bytes)
[44..48)  payload_len     (4B aligned to 4)
```

The `source_hash` field is byte-aligned (it is `eastl::array<std::
byte, 32>`); 4-byte alignment within the buffer is sufficient.

**Reserved bytes.** None in the 48-byte header. In MVP all 32 bits of
`payload_len` are length: `kMaxPayloadBytes = 4'294'967'295` (u32 max)
is a live, enforceable bound with no encoding-invisible sub-cap. Any
future flag-bit scheme (e.g. "payload is compressed") would require
adding a *new field* to the 48-byte layout, which is a major-version
bump per §7.1. No bits are pre-reserved inside `payload_len` for MVP
— see §12 [OPEN] #3 for the post-MVP flag design option.

### 3.6 Round-trip rule (what serialize then deserialize guarantees)

For every type `T` with a registered `Schema` and every value
`t : T` constructible by the schema's deterministic constructor
recipe (SPEC §7.5):

```text
forall t : T,
  let bytes := Envelope<T>::serialize(t, dst).value()
  let t'    := { Envelope<T>::deserialize(bytes, &out); out }
  ⇒ bytes byte-equal a checked-in golden
  ∧ t' == t (POD equality, tag-sorted comparison)
  ∧ Envelope<T>::serialize(t', dst').value() byte-equal bytes
```

Key consequences:

- **Round-trip is bit-deterministic.** Two hosts encoding the same
  `t` produce the same bytes (PHILOSOPHY §7).
- **Round-trip preserves source-hash.** A same-build encode then
  decode yields a `t'` whose subsequent encode embeds the same
  `source_hash` (the registry's recorded hash for the type at its
  current version).
- **Round-trip across versions composes through migration.** A `vN`
  payload deserialized into a `vM` value then re-serialized produces
  a `vM`-stamped envelope. The `vN` bytes are not preserved (this
  is by design — once migrated, the engine-side authoritative form
  is `vM`).

The §11 unit-test plan exercises every clause; the §11 integration
test (full asset persistence + reload) drives the whole pipeline.

### 3.7 Aggregate composition

```text
Envelope<T>  (per-type; codegen-specialized in glibre-foryc)
   │
   ├── EnvelopeHeader  (48B POD; serialize/deserialize bytes)
   │
   ├── SchemaRegistry  (read-only; lookup by ordinal or source_hash)
   │      └─ owned by data/runtime/src/schema_registry.cpp
   │
   ├── MigrationDispatcher  (older-version path only)
   │      └─ owned by data/runtime/src/migration_dispatcher.cpp
   │
   └── glibre_types_serialize_<fqn> / glibre_types_deserialize_<fqn>
          └─ codegen-emitted extern "C" trampolines (fory-codegen.md)
```

The envelope **owns** only:

- The 48-byte header serialize / deserialize (§3.1).
- The decision tree (§3.4).
- The endian-pin (§3.5).

Everything else is delegated through the registry and dispatcher.

## 4. Public Surface

The §5 §envelope.hpp stub from `specs/data/SPEC.md` is authoritative
in shape; this section restates it with per-method behavioural
annotations and the design's refinements (the SPEC's
`EnvelopeHeader` is updated structurally per §3.1; flagged in §12
[OPEN] #1 as a SPEC §7.2.4 amendment).

### 4.1 Types

```cpp
namespace glibre::types {

using SchemaVersion    = std::uint32_t;
using SchemaSourceHash = eastl::array<std::byte, 32>;  // Blake3-256 (PHILOSOPHY §11)

struct EnvelopeHeader {
    eastl::array<std::byte, 4> magic{};               // PHILOSOPHY §11 — eastl:: for fixed-width byte containers
    std::uint32_t              schema_fqn_id{0};
    SchemaVersion              version{0};
    SchemaSourceHash           source_hash{};
    std::uint32_t              payload_len{0};

    constexpr bool operator==(const EnvelopeHeader&) const noexcept = default;
};

}  // namespace glibre::types
```

Field semantics: see §3.1.

### 4.2 Operations

```cpp
namespace glibre::types {

template <class T>
struct Envelope {
    [[nodiscard]] static auto serialize(
        const T&             value,
        std::span<std::byte> dst
    ) noexcept -> std::expected<std::size_t, data::Error>;

    [[nodiscard]] static auto deserialize(
        std::span<const std::byte> src,
        T*                          out
    ) noexcept -> std::expected<void, data::Error>;

    [[nodiscard]] static auto size_bytes(const T& value) noexcept
        -> std::expected<std::size_t, data::Error>;
};

[[nodiscard]] auto peek_header(std::span<const std::byte> src) noexcept
    -> std::expected<EnvelopeHeader, data::Error>;

[[nodiscard]] auto serialize_header(
    const EnvelopeHeader& hdr,
    std::span<std::byte>  dst
) noexcept -> std::expected<std::size_t, data::Error>;

[[nodiscard]] auto deserialize_header(
    std::span<const std::byte> src,
    EnvelopeHeader*             out
) noexcept -> std::expected<std::size_t, data::Error>;

}  // namespace glibre::types
```

Per-method contract:

- **`serialize<T>`** — Pre-allocated `dst` (caller-sized via
  `size_bytes`); writes 48 + `payload_len` bytes; returns the byte
  count. On `dst.size() < required`, returns
  `BufferTooSmall` with `at.offset = dst.size()`. Pure function;
  no allocation; no I/O. Calls
  `glibre_types_serialize_<fqn>` for the body. Wall-time: O(payload
  size); see §9.
- **`deserialize<T>`** — Runs §3.4 decision tree. Returns `{}` on
  success; `out` is written. On any §10 failure `out` is unmodified.
  Pure function; allocates only inside the migration arena (when the
  older-version path fires). Calls
  `glibre_types_deserialize_<fqn>` for the body and
  `MigrationDispatcher::dispatch` for older versions.
- **`size_bytes<T>`** — Computes the exact wire size without
  serializing. Walks `value` once at Fory's body layer to total the
  variable-length field sizes (strings, lists, bytes), adds 48.
  Returns `data::Error::BufferTooSmall` only if the body walk would
  itself overflow u32 (single-asset hard cap, §3.1). No allocation.
- **`peek_header`** — Reads the 48-byte header only; does not call
  the registry, does not validate `schema_fqn_id`. Returns the parsed
  header on `BadMagic` / `EnvelopeTruncated`-only checks (the caller
  may then triage). Used by save-file readers and the editor's asset
  inspector.
- **`serialize_header` / `deserialize_header`** — Type-erased
  header-only forms exported for tooling (the cooker, the editor
  inspector, e2e harness). Behave like the typed forms restricted
  to the 48-byte prefix.

### 4.3 Public ABI surface (extern "C")

The envelope is consumed across the dylib boundary only through the
codegen-emitted typed trampolines (`fory-codegen.md` §"Decision" #4):

```cpp
extern "C" {
// Per-FQN, codegen-emitted; one pair per registered schema.
glibre::types::data::Error glibre_types_serialize_<fqn>(
    const void* value, std::byte* dst, std::size_t dst_len, std::size_t* out_written) noexcept;
glibre::types::data::Error glibre_types_deserialize_<fqn>(
    const std::byte* src, std::size_t src_len, void* out_value) noexcept;
}
```

These trampolines **already** handle the envelope (the codegen
inlines the `Envelope<T>::serialize` / `deserialize` body into the
generated `<Type>.cpp`) — i.e. plugins do **not** call `Envelope<T>`
directly across the dylib boundary; they call the typed extern "C"
trampolines, which call `Envelope<T>` internally (one TU inside
`glibre-types.dylib`).

The extern "C" surface returns `data::Error` as a plain enum (per
SPEC §5 the C-ABI boundary stays free of `std::expected`); the C++
wrapper macro lifts it to `Result<void>` for callers.

Surface rules:

- `noexcept` on every export. Per `error-model.md` `-fno-exceptions`
  is engine-wide; no exception crosses any envelope boundary.
- POD spans only (`const std::byte*` + `std::size_t`, never
  `std::span` or `eastl::span`) per PHILOSOPHY §11.
- The host-side typed wrapper `Envelope<T>` is consumed inside
  `glibre-types.dylib` only; plugins consume the extern "C"
  trampolines.

## 5. Hot / Cold Path Split

The envelope is called **per-asset-load** at *cold* moments (engine
startup, scene load, hot-reload migration) and **per-frame** at
*warm* moments (editor time-rewind scrubber, snapshot capture,
save-on-checkpoint). It is **never** on the steady-state per-frame
hot path (PHILOSOPHY §6 — the hot path uses ECS storage, not Fory
wire form).

The codec internally splits into a hot inner loop (the magic-and-
header memcpy) and a cold dispatcher (the migration walk). The
split is documented here so the per-aggregate budget in §9 sums
correctly.

| Path | Trigger | Frequency | Budget |
|------|---------|-----------|--------|
| Cold | `Envelope<T>::deserialize` engine startup (asset cook ingest) | Once per asset per session | 0–N seconds; not in steady-state frame budget |
| Cold | `Envelope<T>::serialize` save-file write | One per checkpoint event | Same |
| Cold | `Envelope<T>::deserialize` hot-reload migrate frame | Per migrated row, on reload frames only | ≤ 0.40 ms phase-8 envelope (§9.2) |
| Warm | `peek_header` editor inspector | Per inspector refresh, ≤ 100 calls/frame | < 0.05 ms total; 48-byte memcpy + magic compare |
| Warm | `Envelope<T>::deserialize` editor time-rewind | Per scrub-step | ≤ 0.10 ms total per frame across all sites (§9.3) |
| Hot  | (none in shipping)  | — | The steady-state frame loop performs zero envelope ops on the per-frame hot path. |

**Within the codec:**

- **Hot inner.** Magic check, header memcpy (48 B), bounds check,
  registry-by-ordinal lookup. ~50 ns wall-time, allocation-free.
  This is the cost every `peek_header` and every same-version
  `deserialize` pays unconditionally.
- **Cold dispatcher.** Source-hash fallback search, migration-chain
  walk, arena reset. Fires only on cross-build read or older-version
  payload. Wall-time proportional to `entry.version - hdr.version`;
  arena scratch per migration step (§6.3 of `MigrationDispatcher`
  design).

The hot inner is intentionally branch-light: the magic check is one
4-byte compare, the bounds check is two `<=`, the ordinal lookup is
a single span index. Branch-prediction-friendly under the same-build
case (the modal case in shipping).

## 6. Concurrency

`Envelope<T>::serialize` and `Envelope<T>::deserialize` are **pure
functions** of their inputs. They:

1. Read only `value` (serialize) or `src` (deserialize).
2. Write only `dst` (serialize) or `out` (deserialize).
3. Read the immutable `SchemaRegistry::instance()` (SPEC §4.5 inv. 4
   — read-only after static-init).
4. Write only the caller-supplied `Arena&` (deserialize, older-
   version path; SPEC §4.6 inv. 2).
5. Do not log (logging is the handler's responsibility per
   `error-model.md` §"Logging / Telemetry"; the envelope only
   constructs and returns `Error` values).
6. Do not touch the filesystem, network, clock, or RNG.
7. Do not mutate any global state.

**Thread-safety guarantees:**

- Two threads may concurrently call `Envelope<T>::serialize` /
  `deserialize` against disjoint `dst` / `src` / `out` / `arena`
  arguments with **no synchronization**. The shared
  `SchemaRegistry` is read-only.
- The `Arena&` is **not** shared across threads. Each caller passes
  its own arena; `MigrationDispatcher` writes only into that arena
  (SPEC §6.3 "the dispatcher itself reads only the registry it
  holds by `const&`, never mutates the registry").
- `peek_header` is trivially thread-safe (it reads only the
  argument span; no registry access).

**Phase ownership (per `frame-phases.md`).** None. The envelope is
synchronous and runs on whatever thread the caller is on. Hot-reload
invocations (§8) run on the loader thread inside phase 8, gated by
the barrier's exclusive lock; that lock is the loader's, not the
envelope's.

**No suspension, no continuations, no callbacks across threads.**
The envelope is a leaf primitive.

## 7. Persistence + ABI

The envelope **is** the canonical persisted format. Every byte the
engine commits to long-term storage — save files, asset cook output,
world snapshots, hot-reload pre-migration buffers — opens with the
48-byte envelope header.

### 7.1 Wire-format stability

**Any change to the envelope's byte layout (§3.1) is a major engine
version bump.**

This collapses the bootstrap rule (SPEC §7.6) onto the envelope:
the `EnvelopeHeader` is one of the four meta-schemas (alongside
`SchemaSourceRecord`, `MigrationTableRecord`, `AbiHashManifest`)
that **cannot** evolve through the in-process `MigrationChain`.
Layout changes therefore route through `glibre-foryc` release-time
migration, not runtime migration:

1. Add a field, change a field's width, reorder fields, change the
   magic value, or change the endianness rule → `glibre-foryc` major
   version bump → middleman SONAME bump (`fory-codegen.md` §"ABI
   Stability Rules" #5) → every plugin rebuilds against the new
   middleman → `glibre_types_abi_hash` is fresh → loader refuses any
   stale plugin (SPEC §10.2 `AbiHashMismatch`).
2. Future flag-field addition (a new `flags : u32` at offset 48,
   per §12 [OPEN] #3) → header grows from 48 to 52 bytes →
   `glibre-foryc` major version bump → middleman SONAME bump →
   every plugin rebuilds. No bit-level sub-reservation exists in
   MVP; all 32 bits of `payload_len` are payload length (§3.5).

There is no in-process "envelope v1 reader" in a build that emits
"envelope v2". The single-live-version rule (SPEC §7.6 #3) applies.

### 7.2 ABI surface

The envelope's ABI surface is *transitive* through the codegen-
emitted extern "C" trampolines (§4.3). Plugins do not link against
the C++ template `Envelope<T>` directly; they link against
`glibre_types_serialize_<fqn>` / `glibre_types_deserialize_<fqn>`
which live in `glibre-types.dylib`. The middleman dylib is the only
ABI surface plugins see (SPEC §4.10 inv. 4).

The `ContextTag::data` allocator handle (`perf-budget.md`
§"Allocator Rules") tags every envelope allocation. The envelope
itself allocates **only** inside the caller-supplied arena
(deserialize, older-version path); it does not allocate during
serialize.

### 7.3 Cross-build read invariants

A reader built against middleman build B reading bytes written by
middleman build A:

- **Magic must match** (the constant is fixed across builds; a
  difference means a non-glibre buffer or a major-version-different
  glibre).
- **Source-hash must resolve** in B's registry (either by ordinal
  or by source-hash fallback, §3.2). If A's `schema_fqn_id` does
  not exist in B (the schema was removed) or B's schema for the
  same FQN has a different `source_hash` (the schema's bytes
  drifted), the reader refuses with `SourceHashMismatch`.
- **Version must be ≤ B's current** (older versions migrate;
  newer-than-host refuses with `VersionUnsupported`).

The envelope is therefore **forward-compatible across A → B for
schemas B still recognizes** (B applies migrations); it is **not**
backward-compatible (B does not produce A-readable bytes once B's
envelope or schema versions exceed A's).

### 7.4 Save-file packing

Save files pack multiple envelopes back-to-back: the cooker writes
`[envelope_1][envelope_2]…[envelope_N]` into one file. Readers
iterate by walking `48 + payload_len` bytes per envelope until the
file ends. This is purely a content-context concern (its `specs/
content/SPEC.md` § will pin the indexing); the envelope itself does
not own multi-envelope framing.

## 8. Hot-Reload Contract

The envelope serdes is **invoked by but does not own** the hot-
reload state machine. Per SPEC §8.2, the data context's hot-reload
entry point is `glibre::types::data::migrate(...)`, which walks the
world's surviving storage rows and invokes
`MigrationDispatcher::dispatch` on each row whose stored
`SchemaVersion` differs from the live registry's current version.

**The envelope's role is exclusively the deserialize path.**
Specifically, `migrate(...)` deserializes each row's stored bytes
through `Envelope<T>::deserialize` (or the type-erased trampoline)
which routes to the migration dispatcher when `header.version <
entry.version`. The same code path runs cold deserialize and hot-
reload migrate — the envelope is *unaware* of which caller is
driving it (SPEC §6.3 final paragraph).

Codecs themselves do not hot-reload. Concretely:

- **The 48-byte envelope layout is fixed across one process
  lifetime.** Per §7.1, layout changes are major-version bumps that
  require process restart. A hot-reload that tried to swap the
  layout (e.g. a Mode-B middleman swap that introduced a new
  `EnvelopeHeader v2`) would refuse at the §8.3 gate-3 meta-schema
  bootstrap rule.
- **Per-FQN `Envelope<T>` specializations live in the codegen-
  emitted `<Type>.cpp` translation units inside
  `glibre-types.dylib`.** Hot-reload reloads *plugins*, not the
  middleman; the envelope specializations are unaffected by Mode-A
  reload (the common case).
- **Mode-A reload's effect on the envelope is observed indirectly
  through `SchemaRegistry`.** A reloaded plugin may register new
  schemas (extending the FQN-sorted span) or bump existing
  `SchemaVersion`s. The envelope's next-deserialize call sees the
  updated registry and dispatches accordingly. Pre-reload bytes
  whose `version` is now older route through the migration
  dispatcher; pre-reload bytes whose `schema_fqn_id` ordinal moved
  (because Q inserted a new FQN sorting before it) route through
  the source-hash fallback and resolve to the same entry.
- **Payload routing follows the registry diff, not an envelope
  cache.** The envelope holds no per-FQN state outside the `<Type>.
  cpp` static functions; there is nothing to invalidate on reload.

**Refusal cases the envelope contributes to.** Per SPEC §8.4 the
data context types two refusal arms:
`Error::AbiHashMismatch` (loader-detected, not envelope) and
`Error::SchemaMigrationFailure` (envelope-detected when the
dispatch reaches an unsupported step). The envelope additionally
surfaces the §10 arms `BadMagic`, `EnvelopeTruncated`,
`SchemaUnknown`, `SourceHashMismatch`, `VersionUnsupported`,
`PayloadTruncated`, `DeserializeError` to the migrate caller; per
SPEC §8.4 these collapse onto `core::Error::SchemaMigrationFailed`
at the loader's wrap point.

**Per-row rollback discipline.** Per SPEC §8.4 the envelope
guarantees that on any failure during a hot-reload row's
deserialize, the `out` value is unmodified and the arena is reset.
The migrate caller's per-row rollback is therefore complete by the
time the envelope returns the error.

## 9. Performance

The envelope-serdes aggregate is the only `data` aggregate with
measurable per-frame cost (SPEC §9.3). Its per-call wall-time is
linear in `payload_len`; its heap footprint is bounded by the per-
context arena.

### 9.1 Per-call wall-time budget

Per-call CPU cost target on the engine baseline (Apple Silicon M-
series, libc++, clang ≥ 21, release build with `-O2 -fno-exceptions
-fno-rtti`):

| Call             | Fixed cost (header) | Per-KB cost (payload) | Worst-case budget                                |
|------------------|---------------------|------------------------|---------------------------------------------------|
| `peek_header`    | ≤ 50 ns             | 0                      | 100 calls/frame × 50 ns = 5 µs                    |
| `serialize<T>` (same-build, current version) | ≤ 100 ns            | ≤ 200 ns/KB           | 100 KB asset → ≤ 20 µs                            |
| `deserialize<T>` (same-build, current version) | ≤ 100 ns            | ≤ 250 ns/KB           | 100 KB asset → ≤ 25 µs                            |
| `deserialize<T>` (older-version, single migration step) | ≤ 100 ns + 50 µs (one chain step) | ≤ 250 ns/KB        | per `migration_dispatch_v_minus_1.bench.cpp` (SPEC §9.4 #3) |
| `deserialize<T>` (cross-build, source-hash fallback) | ≤ 1 µs (hash table lookup on registry size ~10⁴) | ≤ 250 ns/KB | rare; bounded by registry size                    |

The fixed-cost (header-only) cost is what the SPEC §9.4 #1
`envelope_round_trip_s1.bench.cpp` benchmark gates: the per-frame
0.10 ms cap on all envelope ops in the S1 fixture sums to ≤ 1000
calls × 100 ns = 100 µs = 0.10 ms.

### 9.2 Phase-8 contribution

On a hot-reload frame (S2 fixture) the envelope contributes **at
most 0.40 ms** (SPEC §9.3 — the migration handoff cost bound).
Decomposing the budget at the worst-case per-call cost:

- Per migrating row: 100 ns (header) + 50 µs (one migration step
  at `migration_dispatch_v_minus_1.bench.cpp` ceiling) + body
  at 250 ns/KB × 0.5 KB ≈ 50.225 µs/row.
- 0.40 ms ÷ 50.225 µs/row ≈ **7.97 rows** → the S2 fixture
  row-count bound within the 0.40 ms envelope budget is **≤ 8
  migrating rows per hot-reload frame**.

The S2 fixture in `perf-budget.md` represents a single one-version
migration on a small, bounded component set; production reloads
must stay within this ≤ 8 migrating-row bound per migration step.
If a plugin upgrade touches more rows, the loader must defer
migration to a background pass outside phase 8 (see §12 [OPEN] #5
for the outstanding question on representativeness). The
benchmark fixture runs at the per-call ceiling; same-version
deserialize rows (the common case) are not counted against the
migration budget.

### 9.3 Per-context heap cell

Per `perf-budget.md` `data` row 32 MiB and SPEC §9.2:

- **`Envelope` scratch — 16 MiB sub-ceiling.** Per-payload arena
  used by the migration dispatcher's older-version path. Reset
  between rows during phase-8 migrate; reset between calls outside
  phase 8 (the caller passes the arena and is responsible for the
  reset cadence — typically per-asset-load).
- **No per-frame transient.** The same-version path (the modal
  case) allocates zero bytes. Header memcpy is on-stack.

Allocation rules (per `perf-budget.md` §"Allocator Rules"):

1. Zero allocation in `serialize`, `peek_header`, `size_bytes`,
   `serialize_header`, `deserialize_header`. These are pure
   functions over their inputs.
2. Allocation in `deserialize` is bounded by the migration arena
   (the older-version path); the same-version path allocates zero
   bytes.
3. Allocations leaking past the arena's reset are an `OutOfBudget`
   "leak" arm (SPEC §10 — the data context handles this through
   `MigrationDispatcher`'s arena discipline, not the envelope's).
4. The envelope never allocates from the per-frame transient or
   the per-context heap directly; only arenas the caller supplies.

### 9.4 CI gates

The envelope-serdes contributes to four required CI benchmarks
(SPEC §9.4 enumeration):

1. **`envelope_round_trip_s1.bench.cpp`** — serializes then
   deserializes one of every persistent aggregate type registered
   for S1, asserting wall-clock total ≤ 0.10 ms per 1000-iteration
   window.
2. **`schema_registry_lookup.bench.cpp`** — exercises ordinal
   lookup and source-hash-fallback lookup; both ≤ 10 ns and ≤ 1 µs
   respectively, per §9.1.
3. **`migration_dispatch_v_minus_1.bench.cpp`** — single-step
   migration via the envelope's older-version path; ≤ 50 µs per
   call.
4. **Heap ceiling** — `GLIBRE_ALLOC_STRICT=1` build asserts ≤ 16
   MiB resident across the arena under `data`'s `ContextTag` during
   the round-trip benchmarks.

## 10. Failure Modes

The envelope-serdes failure surface maps to the `data::Error` closed
sum whose authoritative owner is `specs/data/data-error-design.md`
(PR #838, three-round reviewed). That design's §3.1.1 Occam-collapse
audit resolves which envelope failure conditions map to which existing
arms; this table uses that resolution. No new `ErrorTag` arms are
added by envelope-serdes (see §12 [OPEN] #1 for the full disposition
record).

| Arm                       | Trigger                                                                                                       | Detection point (§3.4 step) | Payload fields populated                                       | Recovery        | Severity | core::Error mapping             |
|---------------------------|---------------------------------------------------------------------------------------------------------------|------------------------------|----------------------------------------------------------------|------------------|----------|---------------------------------|
| `BadMagic` (discriminator only — maps to `EnvelopeTruncated` arm 9) | `header.magic != kEnvelopeMagic`                                                                              | step 2                       | `at.offset = 0`                                                | refuse decode    | error    | `EnvelopeTruncated` (arm 9, per `data-error-design.md` §3.1.1 Occam-collapse) |
| `EnvelopeTruncated`       | `src.size() < 48`                                                                                              | step 1                       | `at.offset = src.size()`                                       | refuse decode    | error    | none — passed through (already in SPEC §10, arm 9) |
| `PayloadTruncated` (discriminator only — maps to `EnvelopeTruncated` arm 9) | `48 + header.payload_len > src.size()`                                                                         | step 3                       | `at.offset = src.size()`, `at.schema = header_decoded`         | refuse decode    | error    | `EnvelopeTruncated` (arm 9, per `data-error-design.md` §3.1.1 Occam-collapse) |
| `SchemaUnknown`           | (a) `schema_fqn_id` ordinal exceeds registry size **and** source-hash fallback returns null; OR (b) ordinal is in-range but `entry.source_hash != header.source_hash` **and** source-hash fallback also returns null — i.e. the schema is not findable by any identity in the live registry. Both sub-cases collapse onto arm 6 (`SchemaUnknown`): from the envelope's POV, "hash-fallback miss" is semantically identical to "ordinal miss" — the schema was not found. | step 4 (ordinal-miss path or FallbackBySourceHash path) | `at.offset = 0`, `at.schema = empty`, `at.version = header.version` | refuse decode    | error    | none — passed through (already in SPEC §10) |
| `SourceHashMismatch`      | Mode-A barrier diff at phase-8 step 2: the hot-reload barrier detects that a live plugin's exported `source_hash` for a FQN differs from the registry's recorded hash — i.e. a `.fory` source was modified without a version bump. This arm is **not** emitted by `Envelope<T>::deserialize` (see §12 [OPEN] #9 for the gate condition). Detection is in the loader, not the envelope. `host_hash` and `plugin_hash` are mandatory per SPEC §10.1 and `data-error-design.md` §3.3. | phase-8 step 2 (barrier-diff, loader-detected) | `step_schema` (drifted FQN), `step_from = 0`, `step_to = 0`, `host_hash = live_entry.source_hash_hex`, `plugin_hash = hdr.source_hash_hex` | refuse load (hot-reload) | error    | none — passed through (arm 10 in SPEC §10) |
| `VersionUnsupported`      | `header.version > entry.version` (newer-than-host)                                                            | step 5                       | `at.schema = entry.fqn`, `at.version = header.version`         | refuse decode    | error    | none — passed through; sub-case of `DeserializeError` per SPEC §10 row "DeserializeError" — kept distinct here for log clarity |
| `MigrationStepMissing`    | Older-version path: chain has no `(N → N+1)` step at the inbound version                                      | step 5 (older-version arm)   | `step_schema`, `step_from = header.version`, `step_to = step_from + 1` | refuse decode    | error    | wrapped to `core::Error::SchemaMigrationFailed` at hot-reload (already in SPEC §10) |
| `SchemaMigrationFailure`  | Older-version path: a `MigrationFn` body returned `unexpected`                                                | step 5 (older-version arm)   | `step_schema`, `step_from`, `step_to`                          | refuse decode (cold) / refuse load (hot-reload) | error / warn | wrapped to `core::Error::SchemaMigrationFailed` (already in SPEC §10) |
| `DeserializeError`        | Fory body decoder refused (tag-type mismatch, range check, nested type refusal) — i.e. envelope OK, body bad | step 5 (same-version arm) — body call returned `unexpected` | `at.schema`, `at.version`, `at.offset = 48 + body_offset`      | refuse decode    | error    | none — passed through (already in SPEC §10) |
| `BufferTooSmall` (deferred — no ErrorTag arm assigned) | `serialize`: `dst.size() < 48 + computed_payload_len`                                                          | serialize precheck           | `at.offset = dst.size()`                                       | refuse encode    | error    | deferred per `data-error-design.md` §12 [OPEN]; serialize-path will return a `DeserializeError`-shaped tag once a concrete caller requires typed dispatch |

**Recovery vocabulary** (mirrors SPEC §10.2):

- **refuse decode** — `deserialize` returns `unexpected(...)`; `out`
  unmodified; arena reset; world untouched. Caller decides drop /
  default / escalate.
- **refuse encode** — `serialize` returns `unexpected(...)`; `dst`
  partially written but logically discarded. Caller resizes and
  retries.
- **refuse load** — under `migrate(...)` only: per-row rollback
  per SPEC §8.4, loader un-swaps vtable.

**Severity** mirrors `error-model.md` §"Logging / Telemetry":

- **error** for cold deserialize; logged at the *handler* (never
  inside the envelope).
- **warn** for hot-reload refusals; logged by the loader.

**Detection sequencing.** The `BadMagic` check fires before any
registry access. Garbage bytes (e.g. a buffer pointing at an
ELF file by accident) cannot reach the registry-lookup code path.

**Payload field population (per SPEC §10.1 closed sum).** Each arm
populates exactly the fields the table notes. Arms that do not use
a particular field leave it default-constructed; the
`glibre::log_error` handler omits unset key/value pairs from
`spdlog` output. The `at.offset` field is always measured from the
start of the inbound `src` (i.e. byte 0 of the envelope header).

**Logging discipline.** The envelope itself **never** logs (per
SPEC §10.4 — "the data context never logs from inside
`Envelope<T>::deserialize`"). The handler — which is either
`migrate(...)` (hot-reload), the asset cooker (cold ingest), or the
calling context's deserialize site — calls
`glibre::log_error(err, level)` exactly once per refusal.

## 11. Test Plan

Acceptance is a Catch2 test suite under `tests/data/envelope/` plus
an integration suite under `tests/data/integration/` plus the
golden harness already required by SPEC §11 (issues #367, #368).
The unit tests are owned by the per-issue `type:plan` issues that
this design feeds.

### 11.1 Unit tests

Round-trip every envelope variant (one Catch2 case per fixture):

| Test                                                | Asserts                                                                                                |
|-----------------------------------------------------|--------------------------------------------------------------------------------------------------------|
| `envelope_header_layout`                            | `sizeof(EnvelopeHeader) == 48`; field offsets per §3.1; `is_trivially_copyable_v == true`              |
| `envelope_round_trip_pod_minimal`                   | Serialize a fixed `Transform` v3 instance; bytes byte-equal a checked-in golden; deserialize back; equal |
| `envelope_round_trip_string_field`                  | Round-trip a schema with a variable-length `string` field; verify `payload_len` matches the encoded body size |
| `envelope_round_trip_list_field`                    | Round-trip a schema with `list<u32>` of varying lengths; verify byte-equal across hosts                |
| `envelope_round_trip_nested_generated_type`         | Round-trip a schema referencing another generated type (FQN); both source-hashes correct               |
| `envelope_peek_header`                              | Parse a valid envelope's header without consuming the payload; field-by-field equal                    |
| `envelope_size_bytes_matches_serialize`             | `size_bytes(t)` equals `serialize(t).value()` on every fixture                                         |
| `envelope_endianness_pin`                           | Bytes for the `Transform` v3 fixture begin with `0x47 0x4C 0x45 0x4E …` regardless of host endianness  |

Malformed-input tests (one Catch2 case per arm in §10):

| Test                                                | Asserts                                                                                                |
|-----------------------------------------------------|--------------------------------------------------------------------------------------------------------|
| `envelope_bad_magic`                                | A buffer beginning with `0x00 0x00 0x00 0x00` returns `EnvelopeTruncated{at.offset = 0}` (per `data-error-design.md` §3.1.1 Occam-collapse; `BadMagic` is a discriminator string in §10 prose, not an `ErrorTag` arm) |
| `envelope_truncated_short_header`                   | A 47-byte buffer returns `EnvelopeTruncated{at.offset = 47}`                                           |
| `envelope_payload_truncated`                        | A header claiming `payload_len = 1024` followed by 100 bytes returns `EnvelopeTruncated{at.offset = 148, at.schema = header_decoded}` (per `data-error-design.md` §3.1.1 Occam-collapse; `PayloadTruncated` is a discriminator string in §10 prose, not an `ErrorTag` arm) |
| `envelope_schema_unknown_ordinal`                   | A header with `schema_fqn_id = 9999` (out of range) and an unknown `source_hash` returns `SchemaUnknown` |
| `envelope_schema_unknown_hash_fallback`             | A header with a known ordinal but a synthetic `source_hash` (hash-fallback also misses) returns `SchemaUnknown` (arm 6) — per §10 HIGH-1 remap: hash-fallback miss collapses onto SchemaUnknown, not SourceHashMismatch |
| `envelope_version_unsupported_newer_than_host`      | A header with `version = entry.version + 1` returns `VersionUnsupported`                               |
| `envelope_migration_step_missing`                   | A header with `version = N` against a registry whose chain starts at `(N+1 → N+2)` returns `MigrationStepMissing{step_from = N, step_to = N+1}` |
| `envelope_migration_step_failed`                    | A registered migration `force_migration_failure(N → N+1)` (SPEC §8.6) yields `SchemaMigrationFailure{step_schema, step_from, step_to}` |
| `envelope_deserialize_body_error`                   | A valid header followed by a Fory body with a tag-type mismatch returns `DeserializeError{at.offset = 48 + body_offset}` |
| `envelope_buffer_too_small_serialize`               | `serialize(t, dst)` with `dst.size() < required` returns `ErrorTag::DeserializeError` with discriminator `at.kind == "buffer_too_small"` (per §10 `BufferTooSmall` deferred row: no distinct arm is assigned; serialize-path refusal uses `DeserializeError`-shaped carrier per `data-error-design.md` §12 [OPEN]) |
| `envelope_out_unmodified_on_failure`                | On every refusal arm, `out` retains its pre-call value (random initialised, asserted byte-equal)        |

Determinism (PHILOSOPHY §7):

| Test                                                | Asserts                                                                                                |
|-----------------------------------------------------|--------------------------------------------------------------------------------------------------------|
| `envelope_round_trip_byte_equal_across_hosts`       | A fixture serialized on host A produces bytes byte-equal a serialization on host B (CI matrix runner)  |
| `envelope_serialize_pure`                           | Two consecutive `serialize(t)` calls produce identical bytes                                           |
| `envelope_deserialize_pure`                         | Two consecutive `deserialize(bytes)` calls produce identical `out` values                              |

Concurrency:

| Test                                                | Asserts                                                                                                |
|-----------------------------------------------------|--------------------------------------------------------------------------------------------------------|
| `envelope_thread_safe_disjoint_args`                | 8 threads each serializing a distinct `t` into a distinct `dst` produce 8 valid envelopes concurrently |
| `envelope_thread_safe_shared_registry`              | 8 threads each `peek_header`-ing the same buffer return identical headers                              |

### 11.2 Integration tests

| Test                                                      | Asserts                                                                                                |
|-----------------------------------------------------------|--------------------------------------------------------------------------------------------------------|
| `envelope_full_asset_round_trip`                          | End-to-end: ECS world → `migrate(...)` snapshot → file write → file read → world re-load → byte-equal |
| `envelope_hot_reload_v_minus_1_migration`                 | Driven through `bump_schema_version` (SPEC §8.6); pre-bump `vN` payloads deserialize via envelope through the dispatcher and produce `vN+1` shaped values |
| `envelope_hot_reload_force_failure_rollback`              | `force_migration_failure(fqn, N, N+1)` triggers `SchemaMigrationFailure` from inside the envelope; per-row rollback leaves the world byte-identical |
| `envelope_save_file_pack_unpack`                          | Cooker writes 100 envelopes back-to-back; reader walks `48 + payload_len` per envelope; all round-trip equal |
| `envelope_cross_build_source_hash_fallback`               | Build A writes a payload with `schema_fqn_id = 5`; build B (which inserted a new FQN sorting before it) reads via source-hash fallback and decodes successfully |

### 11.3 Performance gates

The four CI benchmarks listed in §9.4 (and SPEC §9.4) run on every
PR touching `data/runtime/src/envelope.cpp`. Benchmark thresholds
fail the build per `perf-budget.md` "perf-budget.yml" workflow.

### 11.4 Trace instrumentation

The `e2e` context's `.glibre-trace` format records envelope
encode/decode events on hot-reload frames (per SPEC §11 acceptance
criteria #371, #372). Trace points:

- `envelope.serialize.begin` / `end` with `(fqn, version, bytes)`.
- `envelope.deserialize.begin` / `end` with `(fqn, version_in,
  version_out, bytes, used_fallback, used_migration)`.

Trace points are compiled out in shipping (`GLIBRE_TRACE = 0`) and
on by default in editor / e2e profiles. Their cost (≤ 50 ns per
trace point — `tools/SPEC.md` §10) is accounted under the `tools`
context, not `data`.

## 12. Open Questions

- [OPEN] **#1 — SPEC §7.2.4 / §10 amendment scope.** This design
  refines the SPEC's envelope shape from the `(fqn:string,
  version:u32, payload_length:u32, flags:u32)` Fory-defined
  layout to the explicit 48-byte fixed-width
  `(magic, schema_fqn_id, version, source_hash, payload_len)`
  layout (§3.1). It also adds arms to `data::Error` and
  re-classifies one existing arm. The amendment ships with the
  first plan PR that introduces `data/runtime/src/envelope.cpp`.
  Owner: data sub-epic #729 next plan iteration.

  **Structural delta — `EnvelopeHeader` (SPEC §5 vs design §3.3):**
  The SPEC §5 header stub (`specs/data/SPEC.md` lines 724–729) must
  be updated to reflect the following five changes:
  1. `magic` field added — `eastl::array<std::byte, 4>` (4 bytes;
     holds `kEnvelopeMagic`; validated in §3.4 step 2).
  2. `SchemaId schema{}` → `std::uint32_t schema_fqn_id{0}` — field
     renamed **and** type changed (string-view borrow → registry
     ordinal; saves 16 bytes of pointer-size on the stack).
  3. `source_hash` field added — `SchemaSourceHash` (32-byte
     BLAKE3-256 truncated to 32 B; §3.1 field-5).
  4. `flags` field removed — `std::uint32_t flags{0}` is gone; no
     in-band flag bits in this revision (see §12 [OPEN] #3).
  5. `payload_length` → `payload_len` rename — snake_case consistency
     (no type change; remains `std::uint32_t`).

  **`data::Error` closed-sum dispositions for §10 failure arms**
  (revised per R3 review — adopting `data-error-design.md` §3.1.1
  Occam-collapse; the R2 dispositions proposing tags 11, 12, and 13
  are hereby superseded):

  Authoritative source: `specs/data/data-error-design.md` (PR #838,
  three-round reviewed, R3 0H 0M). That design's §1 declares it the
  owner of the `ErrorTag` closed sum. Its §3.1.1 Occam-collapse audit
  explicitly resolves the four envelope-serdes failure conditions:

  - `BadMagic` — **collapsed onto `EnvelopeTruncated` (arm 9)** per
    `data-error-design.md` §3.1.1: "a wrong magic prefix is physically
    an early-truncation-shaped failure from the consumer's perspective."
    Same recovery (refuse decode), same payload (`at.offset = 0`).
    `BadMagic` survives as a discriminator label in §10 prose and in
    structured-log `detail` fields; it is **not** a distinct
    `ErrorTag` arm. No new tag is added.

  - `PayloadTruncated` — **collapsed onto `EnvelopeTruncated` (arm 9)**
    per `data-error-design.md` §3.1.1: "the two are distinguishable
    only by `at.schema`-known vs `at.schema`-default; the recovery is
    identical; the structured log carrier preserves the distinction in
    the `detail` field for operators." `PayloadTruncated` survives as a
    discriminator label in §10 prose; it is **not** a distinct
    `ErrorTag` arm. No new tag is added.

  - `VersionUnsupported` — **sub-case of `DeserializeError` (tag 3).**
    Already consistent with `data-error-design.md` §3.1.1 (unchanged
    from R2). No new tag is added.

  - `BufferTooSmall` — **deferred** per `data-error-design.md` §12
    [OPEN] and §3.1.1. Serialize-path refusal currently returns a
    `DeserializeError`-shaped carrier with `at.offset = dst.size()`.
    Promotion to a first-class `ErrorTag` arm is deferred until a
    concrete caller requires typed dispatch. No new tag is added.

  **Consequence for the SPEC §10.1 amendment scope:** No new
  `ErrorTag` arms are introduced by envelope-serdes. The §10.1
  enum extension (arms 11, 12, 13) previously listed in this section
  is **withdrawn**. The only SPEC amendments required by this design
  are the five structural-delta `EnvelopeHeader` changes listed
  above.

  **Implementation note for plan authors:** Tests that assert
  `EnvelopeTruncated` (arm 9) for bad-magic and payload-truncated
  triggers are correct. Test names (`envelope_bad_magic`,
  `envelope_payload_truncated`) describe the trigger condition and
  may be kept; the asserted `ErrorTag` arm is `EnvelopeTruncated`
  in both cases (see §11.1).

- [OPEN] **#2 — Per-payload CRC.** The envelope reserves no CRC
  field; integrity is currently delegated to the storage layer
  (filesystem checksums, Mach-O fat-binary hash, network transport
  TLS) plus the `source_hash` for schema identity. A CRC-32C over
  `[envelope header || payload]` would catch silent bit-flip in
  cold storage at the cost of 4 bytes per envelope and ~1 GB/s
  CRC throughput. Decision deferred to a post-MVP `content/`
  spike that owns long-lived save-file resilience.

- [OPEN] **#3 — Future flag field.** §3.5 removes the in-band high-bit
  reservation from `payload_len` (resolved per r1 followup review: the
  reservation was inconsistent with `kMaxPayloadBytes = u32 max` and
  would have silently violated the protocol for payloads above 256 MiB).
  If a future MVP/post-MVP need arises for per-envelope flags (e.g.
  "payload is compressed", "payload is encrypted"), the correct
  mechanism is to add a dedicated `flags : u32` field to the header
  layout — a 4-byte extension that bumps the header from 48 to 52
  bytes, triggering a `glibre-foryc` major-version bump (§7.1). Until
  two concrete users demand this, defer per PHILOSOPHY §10. Owner:
  post-MVP storage spike.

- [OPEN] **#4 — Zero-copy deserialize.** Harmonius's rkyv-based
  prior art supports zero-copy archive access (`access_archived`).
  Glibre's MVP does not, because Fory's body format is not laid
  out for direct memory-mapping. A future spike may add an
  `Envelope<T>::view` returning a `T::Archived` reference into
  `src` for read-only scenarios (asset streaming, time-rewind
  scrubber). Defer until two concrete users demand it
  (PHILOSOPHY's "two concrete users" rule).

- [OPEN] **#5 — Phase-8 budget headroom under realistic schema-
  version gaps.** §9.2's worst-case decomposition shows that 200
  rows × 50 µs/step = 10 ms per migration step, which exceeds the
  0.40 ms phase-8 envelope. The S2 fixture in `perf-budget.md`
  assumes a single one-version migration on a small fraction of
  rows; the fixture's representativeness for production reload
  shapes is an open question. Sibling spike `task-breakdown-data-
  envelope-serdes-detailed` is asked to shape a §9 stress-test
  suite that pins the realistic shape and either confirms the
  budget or amends `perf-budget.md`.

- [OPEN] **#6 — Per-build registry-source-hash secondary index
  cost.** §3.2's source-hash fallback requires a secondary index
  `source_hash → RegistryEntry*` populated at static-init. For ~10⁴
  schemas this is a ~640 KiB table (32 B key + 8 B pointer per
  entry, with ~3× overhead for an open-addressing hash). The 8 MiB
  `SchemaRegistry` heap sub-ceiling (SPEC §9.2) absorbs it
  comfortably. The question is whether the index should be `eastl::
  hash_map` (constant-time amortised) or a sorted-array binary
  search (deterministic, matches the FQN-sorted primary). Decision
  deferred to the implementation plan.

- [OPEN] **#7 — SPEC §5 `deserialize` signature amendment.**
  §3.3 and §4.2 adopt `Envelope<T>::deserialize(src, T* out) ->
  std::expected<void, data::Error>` (out-param form) instead of the
  SPEC §5 stub's `deserialize(src) -> std::expected<T, data::Error>`
  (value-return form). The justification is in §3.3 (no move of T,
  caller-controlled lifetime, auditable failure isolation). The
  amendment must be landed in the SPEC §5 header stub in the same
  implementation PR that introduces `data/runtime/src/envelope.cpp`.
  Owner: data sub-epic #729 first plan PR.

- [OPEN] **#8 — SPEC §5 free-function amendment: `peek_header`,
  `serialize_header`, `deserialize_header`.** Design §4.2 exports
  three publicly-callable free functions in `namespace glibre::types`:

  ```cpp
  [[nodiscard]] auto peek_header(
      std::span<const std::byte> src
  ) noexcept -> std::expected<EnvelopeHeader, data::Error>;

  [[nodiscard]] auto serialize_header(
      const EnvelopeHeader& hdr,
      std::span<std::byte>  dst
  ) noexcept -> std::expected<std::size_t, data::Error>;

  [[nodiscard]] auto deserialize_header(
      std::span<const std::byte> src,
      EnvelopeHeader*             out
  ) noexcept -> std::expected<std::size_t, data::Error>;
  ```

  These three signatures are absent from the current SPEC §5 header
  stub (`specs/data/SPEC.md`, `envelope.hpp` section, lines 720–750)
  and must be added there before any implementation PR.

  **Concrete users (PHILOSOPHY §10 two-concrete-users rule):**

  1. `tools/glibre-cook` — the asset cooker writes Envelope blobs to
     disk back-to-back (see §11.2 test `envelope_save_file_pack_unpack`).
     The cooker calls `serialize_header` directly when it needs to patch
     a header field (e.g. `payload_len`) after body serialization without
     re-running `Envelope<T>::serialize` from scratch.

  2. `tools/glibre-editor` inspector panel — parses envelope headers
     from on-disk save files for offline schema inspection (displays FQN,
     version, source hash in the asset browser). The inspector calls
     `peek_header` to read the header non-destructively and
     `deserialize_header` when it needs the returned byte-count to walk
     the blob stream (multiple envelopes packed back-to-back).

  **Blocking condition:** This amendment is blocking the implementation
  PR that introduces `data/runtime/src/envelope.cpp`. The PR must add
  all three signatures to `include/glibre/types/envelope.hpp` and update
  the SPEC §5 stub in the same commit. Owner: data sub-epic #729 next
  plan iteration (same PR as [OPEN] #1 and [OPEN] #7).

- [OPEN] **#9 — `data-error-design.md` §3.2 trigger-table row for
  `SchemaUnknown` deserialize-time site.**

  The HIGH-1 resolution in this design (PR #894 R1 review) remaps the
  `Envelope<T>::deserialize` step-4 FallbackBySourceHash miss from
  `SourceHashMismatch` (arm 10) to `SchemaUnknown` (arm 6). The
  rationale: from the envelope's POV, a hash-fallback miss is
  semantically identical to an ordinal miss — the schema was not found
  by any identity in the live registry. Both collapse onto arm 6.

  **Gate condition:** `data-error-design.md` §3.2 (the per-source
  trigger-table, which is the authoritative cross-aggregate audit for
  every `ErrorTag` arm's detection sites) must be amended to add a row
  mapping:

  ```
  (envelope-serdes / deserialize / step-4 FallbackBySourceHash miss)
      → SchemaUnknown (arm 6)
  ```

  This amendment must land before the first `envelope.cpp`
  implementation PR can close. Until it does, the trigger-table in
  `data-error-design.md` §3.2 is incomplete for the envelope-serdes
  aggregate.

  **Why `SourceHashMismatch` (arm 10) is NOT used here:** SPEC §10.2
  pins arm 10 to "Mode-A barrier diff at phase-8 step 2" exclusively.
  Adding a second trigger site (deserialize-time) would require amending
  the §3.2 trigger table — and that amendment itself is the gate. The
  correct gate artifact is a §3.2 row for SchemaUnknown (arm 6) at the
  deserialize step-4 site, not a widening of arm 10.

  Owner: data sub-epic #729, next plan iteration (same PR as [OPEN] #1,
  #7, and #8).
