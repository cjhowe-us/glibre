# Meshlet-Pak Detailed Design

> Detailed design for the `geometry` context's **meshlet-pak**
> aggregate (`MeshletPak` / `PakHeader` / `PakReader` / `PakWriter`,
> `specs/geometry/SPEC.md` §4.1.7, §4.1.7.1, §4.1.7.2, §4.1.11, §6.3.1,
> §7.2). Refines the on-disk byte format (§7.2.1–§7.2.3), the writer
> emission seam (§6.2 step 7), and the reader load + validation seam
> (§6.3.1) against
> `reviews/decisions/{error-model,perf-budget,plugin-abi,
> hot-reload-protocol,fory-codegen,frame-phases}.md`. All conclusions
> independently re-derived; harmonius prior art
> (`harmonius/docs/design/rendering/meshlets.md` "Architecture",
> "Pipeline", "BLAS section";
> `harmonius/docs/design/geometry/world-geometry.md` "Meshlet Offline
> Baking Pipeline", "Meshlet Types") cited as research input only.

Refs: spike #777 — `[SPIKE] design-geometry-meshlet-pak-detailed`.
Parent sub-epic #776. Sibling task-breakdown spike blocked-by this
deliverable.

## 1. Purpose

The meshlet-pak aggregate is the **single seam** at which cooked
static-mesh geometry crosses the wire. Its one responsibility is to
**emit and consume the immutable on-disk byte container that holds
one cooked `MeshSource`'s meshoptimizer-output meshlet groups,
Draco-compressed vertex/index/attribute streams, per-meshlet bounds
and cones, and the LOD0 `BLASRecipe` blob** — and to do so
**byte-deterministically across hosts**, gated by a single
content-of-schema hash (`FormatHash`) at load time.

Concretely, the aggregate owns three call sites and three only:

1. The **byte format spec** (§3 below) — the fixed-width
   little-endian header at offset 0, the regions it points at
   (cluster-DAG bytes, BLAS-recipe blob, page array, page table),
   and the per-page sub-format (page header + Draco-compressed
   per-attribute streams + CRC32C trailer).
2. The **writer** (`PakWriter`, cook-time-only behind
   `GLIBRE_GEOMETRY_COOK`) — turns staged `MeshletGroup` /
   `DracoStream` / `BLASRecipe` records into one `.glibre-pak` file
   with bit-equal output across hosts.
3. The **reader** (`PakReader`, runtime, cold path) — `mmap`s the
   file, validates the header in a fixed eight-step order
   (`SPEC.md` §7.2.2 "Reader-side validation order"), and exposes
   `PakPage` byte spans + per-page `ResidencyHint` reads to the
   geometry decode pipeline.

What the aggregate explicitly **refuses** to own:

- **Cluster-DAG construction.** The DAG topology, watertight-cut
  bookkeeping, and SSE monotonicity are owned by the cluster-DAG
  aggregate (sibling spike #779). The pak persists an already-built
  DAG; it does not author one.
- **The cook pipeline.** Meshopt stages, meshlet build, Draco
  encode, BLAS recipe authoring, and cook-incremental gating are
  owned by the cook-pipeline aggregate (sibling spike #781). The
  writer is invoked *by* that pipeline as its terminal step
  (`SPEC.md` §6.2 step 7).
- **The decode pool.** Draco decode scratch arenas, worker threads,
  and slot recycling are owned by the decode-pool aggregate
  (sibling spike #785). The reader hands page byte spans to the
  pool; it never decodes.
- **Residency.** `ResidencyState` table writes, scheduler
  arbitration, and `GpuMeshBuffers` materialisation are owned by
  the residency aggregate (sibling spike #787). The reader exposes
  cook-time-baked `ResidencyHint` bytes only.
- **The runtime registry.** `MeshHandle` issuance, generational
  slots, lock-free read surface, and phase-7 mutation point are
  owned by the registry aggregate (sibling spike #789). The reader
  is constructed *by* the registry at `register_mesh` time
  (`SPEC.md` §6.3.1).
- **The BLAS build itself.** The blob is declarative
  (`SPEC.md` §4.1.7.2 invariant 2); render's
  `RTAccelStructures` consumes it. The pak refuses to enqueue
  GPU work (`SPEC.md` §4.2 invariant 10).
- **Fory.** `MeshletPak` is **not Fory-serialised** (`SPEC.md`
  §7.2.4); its sibling Fory side-records (`CookManifest`,
  `BLASRecipeRecord`, `MeshSourceMetadata`, `PakHeaderRecord`)
  are tooling-only and ride the data-context registry.

The aggregate's SRP boundary is sharp: if the magic, the field
table of `PakHeader`, the cluster-DAG byte layout, the page
sub-format, the `BLASRecipe` binary descriptor schema, the
endianness rule, the alignment rule, the eight-step validation
order, the `FormatHash` derivation rule, or the per-page CRC32C
algorithm changes, this design changes. Anything else routes
elsewhere.

## 2. Requirements Coverage

Mapping of harmonius prior-art clauses
(`harmonius/docs/design/rendering/meshlets.md`,
`harmonius/docs/design/geometry/world-geometry.md`) and SPEC §4.1.7 /
§4.1.7.1 / §4.1.7.2 / §4.1.11 / §7.2 invariants to glibre MVP
coverage. Every entry is independently re-derived.

| Source                                                                                | Glibre disposition (MVP) | Coverage site                                                                                                                                                                                                       |
|---------------------------------------------------------------------------------------|--------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Harmonius `meshlets.md` "Architecture" — `Meshlet` / `LodGroup` / `MeshletAsset` separation | **Refused (collapsed)**  | One `MeshletPak` per `MeshSource` (`SPEC.md` §3.2 collapse #1, §4.1.7 invariant 3); no parallel `MeshAsset` shipping format. The pak's region table covers all three harmonius types in one file.                  |
| Harmonius `meshlets.md` "Pipeline" — meshopt build → DAG → page-pack → BLAS            | **Re-derived**           | §3.1 file layout matches the pipeline's terminal stage; the pak ships the post-pipeline byte image. Pipeline stage residency is owned by sibling #781; this design owns the **emission** of the pipeline's outputs. |
| Harmonius `world-geometry.md` "Meshlet Offline Baking Pipeline" 64 KiB page size      | **Re-derived**           | §3.4 page sub-format default `target_page_size_bytes = 64 KiB` (matches `SPEC.md` §5 `PakWriterOptions`); per-pak override permitted up to `max_page_size_bytes = 256 KiB`.                                          |
| Harmonius `world-geometry.md` "Meshlet Types" `MeshletPage` shape                      | **Re-derived (refined)** | §3.4 — page header carries the page-format-version byte, the contained `MeshletGroup` index list, the per-stream `DracoStream` offsets, and a CRC32C trailer (`SPEC.md` §4.1.6 invariants).                          |
| Harmonius `world-geometry.md` `MeshletDAGNode` runtime type                            | **Refused (opaque)**     | DAG bytes ship inside the pak's `cluster-DAG region` (§3.3); never crosses the plugin boundary as a record (`SPEC.md` §4.2 invariant 9). `PakReader` exposes the byte span only; consumers go through the registry. |
| Harmonius `world-geometry.md` "BLAS section" — BLAS built from same vertex/index streams | **Re-derived**           | §3.5 — `BLASRecipe` blob is declarative, references LOD0 cluster band only (`SPEC.md` §4.1.7.2 invariant 1, §4.2 invariant 3). Render builds the AS; geometry never enqueues GPU work.                              |
| Harmonius `world-geometry.md` § RF-1 (Tokio/async removal)                             | **Adopted**              | Pak load is synchronous on the registry's caller thread; no async runtime, no fibers (`SPEC.md` §6.4). Off-frame work is the decode pool's; the pak is mmap-only.                                                   |
| `SPEC.md` §4.1.7 inv. 1 (`FormatHash` per cooked pak)                                  | **Covered**              | §3.2.4 derivation; §4.2 reader validation step 2; §3.2.3 layout; §11 unit tests.                                                                                                                                    |
| `SPEC.md` §4.1.7 inv. 2 (byte-equal across hosts)                                      | **Covered**              | §3.6 determinism rule; §4.1 writer determinism contract; §11 cross-host golden harness.                                                                                                                             |
| `SPEC.md` §4.1.7 inv. 3 (single `MeshSource` per pak)                                  | **Covered**              | §3.2.1 magic + header layout; the writer's public surface (§4.1) takes one source per call.                                                                                                                         |
| `SPEC.md` §4.1.7 inv. 4 (header validated before any payload read)                     | **Covered**              | §4.2 reader-side eight-step gate; §4.1.11 `PakReader` invariant 1.                                                                                                                                                  |
| `SPEC.md` §4.1.7.1 inv. 1 (magic + version checked first)                              | **Covered**              | §4.2 step 1.                                                                                                                                                                                                        |
| `SPEC.md` §4.1.7.1 inv. 2 (all offsets in-range)                                       | **Covered**              | §4.2 steps 4–6.                                                                                                                                                                                                     |
| `SPEC.md` §4.1.7.1 inv. 3 (fixed layout)                                               | **Covered**              | §3.2 byte table — no optional fields, no variable-length prefix; layout drift folds into `FormatHash` (§3.7).                                                                                                       |
| `SPEC.md` §4.1.7.2 inv. 1 (LOD0-only)                                                  | **Covered**              | §3.5 binary descriptor schema; writer rejects non-LOD0 descriptor input with `geometry::Error::BLASRecipeInvalid`.                                                                                                  |
| `SPEC.md` §4.1.7.2 inv. 2 (no GPU calls)                                               | **Covered**              | §3.5 — `BLASRecipe` is bytes only; no symbol from `Metal/MTLDevice` is reachable from `pak/` source.                                                                                                                |
| `SPEC.md` §4.1.7.2 inv. 3 (deterministic ordering)                                     | **Covered**              | §3.5 — descriptors emitted in `group_index` ascending order; writer asserts.                                                                                                                                        |
| `SPEC.md` §4.1.6 inv. 1–4 (page invariants)                                            | **Covered**              | §3.4 page sub-format; §4.2 step 6 page-table range check; §4.3.5 reader page-byte access; §11 tests.                                                                                                                |
| `SPEC.md` §4.1.11 inv. 1–4 (`PakReader`)                                               | **Covered**              | §4.2 reader public surface; §4.3 reader internal ordering; §6 concurrency rules.                                                                                                                                    |
| `SPEC.md` §4.2 inv. 1 (single `FormatHash` schema-drift gate)                          | **Covered**              | §3.7 derivation rule; §4.2 step 2; §8 hot-reload contract reuses the same gate.                                                                                                                                     |
| `SPEC.md` §4.2 inv. 4 (meshlet ≤ 64 vert / ≤ 124 prim caps frozen)                     | **Covered**              | §3.7 — caps are inputs to `FormatHash`; §4.2 step 7 defence-in-depth `u8` range check.                                                                                                                              |
| `SPEC.md` §7.2.4 (out-of-band binary, not Fory)                                        | **Covered**              | §3.8 — design choice restated; mmap addressability rule and Fory-tag drift refusal explicit.                                                                                                                        |
| `SPEC.md` §7.3.2 (FormatHash invalidation, no migration)                               | **Covered**              | §8.1 — pak hot-reload reuses §7.3.2's "invalidate, never migrate" rule verbatim; refusal is `PakFormatHashMismatch`.                                                                                                |
| `error-model.md` `std::expected<T, glibre::Error>` boundary                            | **Covered**              | §4 surface — every public function returns `Result<T>`; no exceptions cross the plugin or the cook-driver boundary.                                                                                                 |
| `plugin-abi.md` middleman-types release-time stability                                 | **Covered**              | §7.2 — `FormatHash` lives in the same generated header tree as `glibre_types_abi_hash`; both bump on any release, but they are **independent gates** (`SPEC.md` §7.2.3 property 5, §7.5 cross-context obligations). |
| `hot-reload-protocol.md` "drain → swap → migrate → resume"                             | **Covered**              | §8 — pak hot-reload rides the protocol with `FormatHash` as the sole geometry-specific refusal arm; engine machinery unchanged.                                                                                     |
| `fory-codegen.md` middleman build-time generation                                      | **Refused (out-of-band)**| `MeshletPak` bytes are not Fory; sibling Fory records (`CookManifest`, `BLASRecipeRecord`, `MeshSourceMetadata`, `PakHeaderRecord`) ride the standard codegen (`SPEC.md` §7.1).                                     |
| `perf-budget.md` geometry row 0.30 + 0.20 ms / 256 MiB                                  | **Partially covered**    | §9 — pak load time-per-MB ceiling on the cold path; pak mmap is exempt from the 256 MiB heap (`SPEC.md` §9.5 "Pak mmap exempt"); the writer runs in `glibre-meshcc` and is bounded separately.                      |
| `frame-phases.md` phase ownership                                                      | **Refused (none)**       | The pak owns no phase. Reader construction runs at `register_mesh` time (any phase); writer runs in the cook driver process. (`SPEC.md` §4.2 invariant 8 places the runtime aggregate's phase-7 ownership elsewhere.) |

Coverage rule: every harmonius clause and every SPEC invariant either
lands in this design or is refused with a one-line rationale. No
silent drops.

Glibre-native obligations beyond harmonius:

- **Magic + version is a `u32 + u16×3`, not a single 64-bit word.**
  Harmonius's `meshlets.md` did not pin a magic byte sequence; we
  add one explicitly (`"GLPK"` ASCII, four bytes) so a hex dump
  identifies the file unambiguously, and so a stray bundle whose
  bytes happen to start with the same Draco container header
  cannot be mis-routed to `PakReader`.
- **`FormatHash` is a u64 truncation of Blake3-256, not a string.**
  Mirrors `data/SPEC.md` §7.3 and `render/SPEC.md` §7.1.2's
  `archive_blob_blake3` rule. A u64 is the cheapest possible exact
  equality check at load time; the source-of-truth full hash lives
  in `<pak>.cookmanifest.fory`'s `format_hash` field for tooling
  cross-reference.
- **Per-page CRC32C is mandatory.** Pak content lives on disk for
  the lifetime of an installed asset; bit-rot on a network-mounted
  dev volume is the documented failure shape. CRC32C (Castagnoli,
  hardware-accelerated on M1+ via `__crc32cd` intrinsics) is the
  cheapest integrity gate that catches single-bit flips without
  invoking Blake3 per page.
- **Header includes the per-attribute decode-pool scratch maxima.**
  `SPEC.md` §4.1.12 invariant 1 forbids mid-frame pool resize; the
  registry refuses pak load when the live pool cannot accommodate
  the pak's declared maxima. Carrying the maxima in the header
  (rather than recomputing them from the pages) is what makes the
  refusal an O(1) check at `register_mesh` time.

## 3. Detailed Model

### 3.1 File layout (logical regions, byte offsets)

The pak is one flat file, mmap-friendly, little-endian throughout
(`SPEC.md` §7.2.1). Region offsets are absolute from byte 0 and are
recorded in `PakHeader`. The header sits at offset 0 unconditionally;
every other region's offset is a header field.

```text
File layout (offsets are absolute, byte units, little-endian):

  0x0000          PakHeader                    (fixed-size, see §3.2)
  hdr_end ------> ClusterDAG bytes             (linearised group / meshlet
                                                 tables; opaque blob §3.3)
  dag_end ------> BLASRecipe blob              (binary §3.5; LOD0 cover only)
  blas_end -----> Page[0] bytes                +
  page_1 -------> Page[1] bytes                |   §3.4 page sub-format;
  ...                                          |   each page self-describing,
  page_N-1 -----> Page[N-1] bytes              |   CRC32C trailer mandatory.
  pages_end ----> PageTable[page_count]        (page_index -> { byte_offset,
                                                                byte_length };
                                                 §3.6)
  EOF
```

`hdr_end == PakHeader.header_byte_length`. `dag_end ==
PakHeader.cluster_dag_offset + PakHeader.cluster_dag_length`.
`blas_end == PakHeader.blas_recipe_offset +
PakHeader.blas_recipe_length`. The page region runs contiguously from
`blas_end` to `PakHeader.page_table_offset`; `pages_end ==
PakHeader.page_table_offset`. `EOF == page_table_offset + page_count
* sizeof(PageTableEntry)`.

The writer emits regions in this exact order (§4.1.4); the reader
does not depend on contiguity beyond what `PakHeader`'s offsets
declare, so the format admits future-additive regions inserted
between any two named ones (each addition would bump `FormatHash`
per §3.7 — the format is **not** silently extensible at runtime).

**Constants.**

| Symbol                        | Value                                                               |
|-------------------------------|---------------------------------------------------------------------|
| `kPakMagic`                   | `{ 0x47, 0x4C, 0x50, 0x4B }` — ASCII `"GLPK"`                       |
| `kPakHeaderFixedSize`         | `0x60` (96 bytes; the part of `PakHeader` before the variable tail) |
| `kPakAttributeKindCount`      | `9` (matches `SPEC.md` §5 `kAttributeKindCount`)                    |
| `kPageHeaderSize`             | `16` (bytes; §3.4)                                                  |
| `kPageCrcSize`                | `4` (bytes; CRC32C trailer; §3.4)                                   |
| `kPageDefaultByteSize`        | `65 536` (bytes; `SPEC.md` §5 `target_page_size_bytes`)             |
| `kPageMaxByteSize`            | `262 144` (bytes; `SPEC.md` §5 `max_page_size_bytes`)               |
| `kPakMinAlignment`            | `16` (bytes; every region start is 16-byte aligned)                 |
| `kPakReservedTailPad`         | up to 15 bytes; brings the next region start to `kPakMinAlignment`  |

Why **`"GLPK"`**. Four ASCII bytes that are (a) printable so a hex
dump of byte 0 is human-recognisable, (b) distinct from the
`data` envelope's `"GLEN"` (envelope-serdes-design.md §3.1) so the
two byte streams cannot be cross-routed, and (c) distinct from
common archive magics (`PK`, `7z`, `BZ`, ELF, Mach-O, glTF `glTF`,
Draco's own container) so `file(1)` does not mis-identify a pak.

Why **fixed alignment to 16 bytes** between regions. SIMD reads of
the cluster-DAG group records and Draco's stream prologue both
prefer 16-byte alignment; the writer pads the tail of each region to
satisfy this without ever growing the previous region's logical
length. Pad bytes are zero (`SPEC.md` §4.1.7 invariant 2 byte-equal
determinism).

### 3.2 `PakHeader` byte layout

`PakHeader` (`SPEC.md` §4.1.7.1) is a fixed-layout, little-endian
record split into a **fixed prefix** (96 bytes — the part whose
offset is known statically) and a **variable tail** (per-attribute
scratch maxima array + per-page residency-hint table). The fixed
prefix is observable at any point post-construction through
`PakHeader::fixed_prefix_view()` (§4.2); the tail is gated on the
prefix's `header_byte_length` field.

```text
Offset   Size   Field                          Notes
------   ----   -----                          -----
0x0000   4      magic                          ASCII "GLPK" (kPakMagic)
0x0004   2      pak_version_major              u16  schema version (§3.7)
0x0006   2      pak_version_minor              u16
0x0008   2      pak_version_patch              u16
0x000A   2      reserved_align0                u16 = 0
0x000C   4      header_byte_length             u32  total header length, incl. tail
0x0010   8      format_hash                    u64  FormatHash (§3.7)
0x0018   8      source_content_hash            u64  blake3_64 of MeshSource (§4.1.1)
0x0020   2      glibre_engine_version_major    u16
0x0022   2      glibre_engine_version_minor    u16
0x0024   2      glibre_engine_version_patch    u16
0x0026   2      reserved_align1                u16 = 0
0x0028   8      cluster_dag_offset             u64
0x0030   8      cluster_dag_length             u64
0x0038   8      blas_recipe_offset             u64
0x0040   8      blas_recipe_length             u64
0x0048   8      page_table_offset              u64
0x0050   4      page_count                     u32
0x0054   1      lod_band_count                 u8
0x0055   1      draco_profile                  u8   DracoQuantisationProfile
0x0056   1      meshlet_max_vertices           u8   = 64 (frozen, §3.7)
0x0057   1      meshlet_max_triangles          u8   = 124 (frozen, §3.7)
0x0058   4      meshlet_group_count            u32
0x005C   4      reserved_align2                u32 = 0
                                                   --- end of fixed prefix ---
0x0060   N×4    decode_pool_scratch_max[N]     u32 per AttributeKind (§3.2.5)
                                                   (N = kPakAttributeKindCount = 9)
0x0084   M×1    residency_hint_table[M]        u8 per page; M = page_count (§3.2.6)
         pad    align to 16-byte boundary       zero pad (§3.1)
   header_byte_length ↑
```

The fixed prefix is exactly `0x60 = 96` bytes (`kPakHeaderFixedSize`).
The tail's length is `9 × 4 + page_count × 1 + pad`, computed once
and recorded in `header_byte_length` so readers can advance past the
header in one read without re-deriving the math (§4.2 step 3 uses
`header_byte_length` as its bound check input).

#### 3.2.1 `magic`, version triple

`magic` is the four bytes `0x47 0x4C 0x50 0x4B`. The reader's first
gate is exact equality (`SPEC.md` §4.1.7.1 invariant 1, §7.2.2 step
1).

`pak_version_{major,minor,patch}` is the **schema** version of the
format itself, *not* the engine version (which is recorded
separately at `0x0020`). MVP ships at `1.0.0`; bumps follow §3.7.

#### 3.2.2 `header_byte_length`

`u32` little-endian. Recorded at write time once the variable tail
size is known. The reader uses it for two purposes: (a) bound check
on every later region offset (`SPEC.md` §7.2.2 step 3), and (b) skip
past the entire header to the cluster-DAG region without recomputing
tail math. The value is always 16-byte aligned (§3.1).

#### 3.2.3 `format_hash`

`u64` little-endian. The first 8 bytes of the Blake3-256 of the
canonical schema-defining bytes (§3.7). The reader's second gate is
exact equality against the engine's compiled-in `FormatHash`
(`SPEC.md` §4.1.7 invariant 1, §4.2 invariant 1, §7.2.2 step 2). A
mismatch refuses the pak with `geometry::Error::PakFormatHashMismatch`
(§5).

#### 3.2.4 `source_content_hash`

`u64` little-endian. The first 8 bytes of the Blake3-256 of the
authored `MeshSource`'s canonical bytes (positions, indices,
attribute streams, submesh ranges, material slots — `SPEC.md`
§4.1.1). Used for two purposes: (a) the cooker's incremental-cook
gate (`SPEC.md` §4.1.8 invariant 1), and (b) the pak hot-reload
trigger (§8 below) — a content-hash change in the file watcher's
view is what prompts a reload request.

#### 3.2.5 `decode_pool_scratch_max[]`

Array of `kPakAttributeKindCount = 9` `u32` little-endian values,
one per `AttributeKind` (`SPEC.md` §5: Position, Index, Normal,
Tangent, UV0, UV1, Color, JointIndices, JointWeights). Each entry
is the maximum *decoded* byte count any single page in the pak will
produce for that attribute kind (writer derives per-page from
Draco's `DecoderBuffer::decoded_size_estimate()` and reduces by max
across pages). The runtime `DecodePool` (sibling #785) consults
these values at `register_mesh` time and refuses load if the pool
cannot accommodate them (`SPEC.md` §4.1.12 invariant 1; refusal arm
`geometry::Error::DecodePoolUndersized`).

Entries for unused attribute kinds are `0`. `JointIndices` and
`JointWeights` are typically `0` for static-mesh paks; they are
present in the array layout regardless so the schema is stable
across mesh authoring profiles (§3.7 — `kPakAttributeKindCount` is
an input to `FormatHash`).

#### 3.2.6 `residency_hint_table[]`

Array of `page_count` `u8` values, one per `PakPage`, each holding a
`ResidencyHint` enumerator (`SPEC.md` §5). The hint is **cook-time
baked and read-only at runtime** (`SPEC.md` §4.1.13 invariant 4). It
is the scheduler's input weighting — the hint never overrides
runtime state; it only informs prioritisation (`SPEC.md` §4.1.13
invariants).

The table is a `bytes` blob (one byte per page) so the reader can
expose it to the scheduler as a single span without per-entry
endianness work.

### 3.3 Cluster-DAG region byte layout

The cluster-DAG region is a `cluster_dag_length`-byte span starting
at `cluster_dag_offset`. Its internal byte schema is owned by the
sibling **cluster-DAG aggregate** (#779) — this design pins only
the *seam* (offset, length, alignment, byte-equality contract); the
record schemas inside the blob are detailed in that aggregate's
design.

The seam contract:

1. **Self-contained.** The cluster-DAG region is decoded without
   reading bytes outside its declared `[cluster_dag_offset,
   cluster_dag_offset + cluster_dag_length)` span (`SPEC.md`
   §7.2.1 byte-layout discipline).
2. **Folded into `FormatHash`.** Any change to the region's
   internal schema (group record fields, meshlet record fields,
   parent/child edge encoding, watertight-cut bookkeeping shape)
   bumps `FormatHash` via the
   `cluster_dag_layout_canonical` input listed in §3.7 (matches
   `SPEC.md` §7.2.3 input list).
3. **16-byte aligned start.** `cluster_dag_offset %
   kPakMinAlignment == 0` (§3.1).
4. **Length includes inner pad.** `cluster_dag_length` is the byte
   length of the writer-emitted bytes including any internal
   alignment pad the cluster-DAG record schema demands; the next
   region's offset is `cluster_dag_offset + cluster_dag_length`,
   which the writer may then pad to 16 bytes before emitting the
   BLAS-recipe blob (the writer's own pad, not the DAG region's).

`PakReader` exposes the region as an opaque `eastl::span<const
std::byte>` (§4.2 `cluster_dag_bytes()`); decoding the bytes is the
cluster-DAG aggregate's responsibility. No `MeshletGroup` /
`Meshlet` record ever crosses this design's public boundary
(`SPEC.md` §4.2 invariant 9).

### 3.4 Page sub-format

Every `PakPage` is a self-describing byte run. The page region is a
sequence of `page_count` pages laid out contiguously between
`blas_recipe_offset + blas_recipe_length` (with inter-region pad)
and `page_table_offset`. Each page begins on a 16-byte boundary
(§3.1).

Per-page byte layout:

```text
Offset (relative to page start)  Size   Field
-------------------------------  ----   -----
0x0000                           1      page_format_version (u8 = 1)
0x0001                           1      reserved_align0     (u8 = 0)
0x0002                           2      group_count         (u16 LE)
0x0004                           4      payload_byte_length (u32 LE; bytes
                                        from page start to CRC trailer,
                                        i.e. page_byte_length - kPageCrcSize)
0x0008                           4      stream_count        (u32 LE; one per
                                        contained group × kPakAttributeKindCount,
                                        excluding zero-length entries;
                                        must satisfy stream_count ≤ group_count
                                        × kPakAttributeKindCount — enforced by
                                        page_streams() before constructing the
                                        StreamTableEntry span)
0x000C                           4      reserved_align1     (u32 = 0)
                                        --- 16-byte page header end ---
0x0010                           G×4    group_index[G]      (u32 LE per
                                        contained MeshletGroup; G = group_count)
                                 pad    zero-pad to align to 8 bytes
                                        (pad = align8(G×4) - G×4; 0 or 4 bytes)
   stream_table_offset ↑ = 0x0010 + align8(G×4)
                                 S×20   StreamTableEntry[S] (S = stream_count;
                                        20-byte record §3.4.1)
   draco_streams_offset ↑
                                 raw    Draco-compressed bytes (one stream per
                                        StreamTableEntry; entries' offsets land
                                        inside this region)
                                 pad    zero-pad to align CRC32C to 4 bytes
   crc_offset ↑ == page_byte_length - kPageCrcSize
                                 4      crc32c              (u32 LE; CRC32C
                                        of bytes [0, crc_offset))
   page_byte_length ↑
```

`page_byte_length` is recorded in the page table (§3.6); the page
itself records `payload_byte_length = page_byte_length - 4` so the
CRC trailer is observable without reading the table. A page header
that declares `payload_byte_length + kPageCrcSize > page_byte_length`
refuses with `geometry::Error::PakPageIntegrityFailed` (§5; `SPEC.md`
§4.1.6 invariant 3).

#### 3.4.1 `StreamTableEntry` byte layout

Per-stream record inside the page (one per `(MeshletGroup,
AttributeKind)` Draco stream actually present):

```text
Offset (rel.)  Size   Field
-------------  ----   -----
0x00           4      group_index_local       (u32 LE; index into the page's
                                               group_index[] table, NOT the
                                               pak-wide group index)
0x04           1      attribute_kind          (u8; AttributeKind enumerator)
0x05           1      draco_profile_id        (u8; index into PakHeader's
                                               DracoQuantisationProfile)
0x06           2      reserved_align          (u16 = 0)
0x08           4      draco_byte_offset       (u32 LE; relative to page start,
                                               into the page's draco_streams
                                               region)
0x0C           4      compressed_byte_length  (u32 LE; byte length of this
                                               stream's Draco-compressed payload;
                                               must satisfy draco_byte_offset +
                                               compressed_byte_length ≤
                                               payload_byte_length)
0x10           4      decoded_byte_length     (u32 LE; bytes the decoder is
                                               expected to write; ≤ the matching
                                               PakHeader.decode_pool_scratch_max[
                                               attribute_kind])
```

`compressed_byte_length` is required for two purposes: (a) the reader
uses `draco_byte_offset + compressed_byte_length` to bound-check that
the stream lies entirely within the page's payload before returning a
byte span to the decode pool (§4.2.3); (b) the decode pool uses it to
slice the exact compressed byte range from the mmap without scanning
for a delimiter. A stream whose `draco_byte_offset + compressed_byte_length
> payload_byte_length` is rejected by `page_stream_bytes()` with
`geometry::Error::PakHeaderOffsetOutOfRange`.

`decoded_byte_length` is the decoder's pre-allocated scratch slot
size (`SPEC.md` §4.1.12). The pool's slot is sized to the maximum
across pages (§3.2.5); per-stream `decoded_byte_length` ≤ that
maximum. A stream whose `decoded_byte_length` exceeds the pool's
matching scratch capacity returns `geometry::Error::DecodePoolOverflow`
at decode time (`SPEC.md` §5; expected to be unreachable on
post-`accommodate(...)` paks, kept as a defence-in-depth arm).

#### 3.4.2 Whole-group containment

A single `MeshletGroup`'s clusters reside entirely inside one
`PakPage` (`SPEC.md` §4.1.6 invariant 1). The writer enforces this
by partitioning groups across pages such that no group crosses a
page boundary; the reader observes it implicitly (every entry in
`group_index[]` resolves to a group whose every Draco stream is in
the same page's `StreamTableEntry[]`). Splits across pages refuse
cook with `geometry::Error::PakPageOversize` (§5).

#### 3.4.3 Page CRC32C

The trailer is one `u32` LE: CRC32C (Castagnoli polynomial
`0x1EDC6F41`) computed over the bytes `[page_start, page_start +
payload_byte_length)`. Castagnoli is hardware-accelerated on Apple
Silicon via `__crc32cd` / `__crc32cw` intrinsics; the writer reaches
the throughput ceiling listed in §9.

Verification runs at decode time (`SPEC.md` §4.1.6 invariant 3,
§6.3.2 step 3). A mismatch refuses the page with
`geometry::Error::PakPageIntegrityFailed` (§5); the residency
manager's downgrade loop (`SPEC.md` §10.4.1) routes the affected
page to `NotResident` and falls back to a coarser LOD band.

### 3.5 `BLASRecipe` blob byte layout

The BLAS-recipe region is a `blas_recipe_length`-byte span starting
at `blas_recipe_offset`. Its bytes are the declarative input to
render's `RTAccelStructures` BLAS build (`SPEC.md` §4.1.7.2; §4.2
invariant 3 — LOD0 cluster band only).

```text
Offset (rel. to blob start)  Size   Field
---------------------------  ----   -----
0x0000                        4      blob_format_version     (u32 = 1)
0x0004                        4      lod0_descriptor_count   (u32 LE)
0x0008                        2      blas_build_flags        (u16 LE; bitmask
                                     over BLASBuildFlags from SPEC §5)
0x000A                        2      reserved_align0         (u16 = 0)
0x000C                        4      reserved_align1         (u32 = 0)
                                     --- 16-byte blob header end ---
0x0010                        D×64   BLASGeometryDescriptor[D]
                                     (D = lod0_descriptor_count; §3.5.1)
   recipe_byte_length ↑
```

#### 3.5.1 `BLASGeometryDescriptor` byte layout

Per-LOD0-`MeshletGroup` descriptor, one per LOD0 group (matches
`SPEC.md` §5 `BLASGeometryDescriptor`):

```text
Offset (rel.)  Size   Field
-------------  ----   -----
0x00           4      group_index_pak       (u32 LE; pak-wide MeshletGroup
                                             index — must reference an LOD0
                                             group; writer asserts)
0x04           4      vertex_byte_offset    (u32 LE; into the matching page's
                                             decoded vertex stream — render
                                             resolves at BLAS build time
                                             against the GpuBufferHandle the
                                             registry hands it)
0x08           4      vertex_count          (u32 LE)
0x0C           4      vertex_stride_bytes   (u32 LE)
0x10           4      index_byte_offset     (u32 LE; into decoded index
                                             stream)
0x14           4      index_count           (u32 LE)
0x18           1      index_format          (u8; 0 = u32, 1 = u16; closed-sum)
0x19           1      vertex_format         (u8; closed-sum: pos-only / pos
                                             +normal / pos+normal+uv / etc.)
0x1A           2      reserved_align        (u16 = 0)
0x1C           4      material_slot_index   (u32 LE; bindless MaterialHandle
                                             index — opaque, geometry never
                                             inspects)
0x20           4      triangle_count        (u32 LE; computed = index_count / 3)
0x24           28     reserved_for_future   (28 zero bytes; brings record
                                             to 64 bytes; bumps FormatHash
                                             when re-purposed)
```

Total per-descriptor size: 64 bytes (a single cache line on M1 +
M2). The 28-byte reserved tail is intentional headroom that **does
not** admit silent extensibility: any field added in the reserved
range bumps `FormatHash` (§3.7), so two builds with different
field allocations cannot mis-decode each other's recipes.

#### 3.5.2 Determinism + ordering

The writer emits descriptors sorted by ascending `group_index_pak`
(`SPEC.md` §4.1.7.2 invariant 3). The blob's bytes are byte-equal
across hosts given the same inputs (`SPEC.md` §4.1.7 invariant 2;
§3.6 below). Out-of-order descriptors at write time refuse with
`geometry::Error::BLASRecipeInvalid` (cook-time arm; §5).

### 3.6 Page table

The page table is a fixed-stride array of `page_count`
`PageTableEntry` records, located at `page_table_offset` (last
region; `EOF == page_table_offset + page_count * 16`).

```text
Offset (rel.)  Size   Field
-------------  ----   -----
0x00           8      page_byte_offset      (u64 LE; absolute, from file start)
0x08           4      page_byte_length      (u32 LE; including CRC trailer)
0x0C           4      reserved_align        (u32 = 0)
                                            --- 16-byte record ---
```

Per-record size: 16 bytes (`sizeof(PageTableEntry)` is a constant the
reader uses for bound-check arithmetic in §4.2 step 6). The table is
the index `content`'s scheduler walks when issuing residency
requests; entries are accessed by `page_index ∈ [0, page_count)`.

The table sits at the file's tail rather than next to the header
because (a) the page bodies between the header and the table can be
written first by the cooker without knowing the final
`page_byte_offset` until pad/alignment is resolved, and (b) the OS
pre-fault behaviour for mmap files prefers contiguous early-file
reads over mid-file table lookups; the table is touched once at
load time when the registry seeds residency state, not in the hot
decode loop (which addresses by `page_byte_offset` recorded once).

### 3.7 `FormatHash` derivation

`FormatHash` is the schema-only content hash that gates pak
loadability. It is computed at **`glibre-foryc` build time** from
the canonical bytes of the schema-defining inputs. The runtime
embeds the value in the generated header
`build/generated/glibre-types/include/glibre/types/geometry/_format_hash.hpp`
(`SPEC.md` §7.3.2 consequence 2); the writer reads the same constant
and stamps it into the pak; the reader compares (§4.2 step 2).

```text
FormatHash := blake3_64(canonical_bytes(
  "GLPK"                                 // ASCII magic, 4 bytes
  pak_version_major                      // u16 LE
  pak_version_minor                      // u16 LE
  pak_version_patch                      // u16 LE

  pak_header_field_table_canonical       // UTF-8: each field's
                                          // (name, c++ type, byte offset)
                                          // sorted by offset, comma-separated;
                                          // covers fixed prefix + tail layout
                                          // shape (variable lengths replaced
                                          // by their type signature, e.g.
                                          // "decode_pool_scratch_max:u32×N")

  cluster_dag_layout_canonical           // UTF-8: cluster-DAG record schemas,
                                          // emitted by sibling #779;
                                          // included verbatim here

  blas_recipe_layout_canonical           // UTF-8: blob header schema +
                                          // BLASGeometryDescriptor field table
                                          // (§3.5.1), sorted

  page_layout_canonical                  // UTF-8: page header schema +
                                          // StreamTableEntry field table
                                          // (§3.4.1), sorted

  page_table_entry_layout_canonical      // UTF-8: PageTableEntry field table
                                          // (§3.6), sorted

  meshlet_max_vertices                   // u8 = 64 (frozen, SPEC §4.2 inv. 4)
  meshlet_max_triangles                  // u8 = 124 (frozen)

  draco_quantisation_profile_table       // UTF-8: closed-sum enum table for
                                          // DracoQuantisationProfile, sorted

  attribute_kind_table                   // UTF-8: closed-sum enum table for
                                          // AttributeKind, sorted

  blas_build_flags_table                 // UTF-8: closed-sum bitmask values
                                          // for BLASBuildFlags, sorted

  crc32_polynomial                       // u32 = 0x1EDC6F41 (Castagnoli)
))[:8]                                    // first 8 bytes, u64 LE
```

Properties (mirrors `SPEC.md` §7.2.3):

1. **Schema-only, payload-blind.** Two paks with different mesh
   data but identical schemas have identical `FormatHash`.
2. **Frozen-cap input.** Bumping either meshlet cap bumps
   `FormatHash` (`SPEC.md` §7.3.4 rule 1).
3. **Closed-sum enums fold in.** Adding a `DracoQuantisationProfile`
   or `AttributeKind` enumerator bumps `FormatHash`.
4. **Cluster-DAG and BLAS-recipe schemas fold in.** Sibling
   aggregates (#779, #783 — task-breakdown for cluster-DAG /
   BLAS-recipe) cannot evolve their byte schemas without bumping
   `FormatHash` here; the inputs are emitted by their respective
   designs and concatenated by `glibre-foryc` per a pinned source
   order.
5. **64-bit truncation.** Acceptable because the value is gated by
   exact equality, not collision-resistance against an adversary
   (schema authors are first-party).
6. **Independent of `glibre_types_abi_hash`.** The two coexist;
   `CookManifest` records both (§7.1 of `SPEC.md`); the pak header
   records only `FormatHash` because runtime pak-load gates only
   on pak schema. (`SPEC.md` §7.5 cross-context obligation rule 4.)

`FormatHash` is u64 truncated; the full Blake3-256 lives in
`<pak>.cookmanifest.fory`'s `format_hash_full` field (sibling
spike #781 ratifies the manifest layout) for tooling cross-reference
during diagnostic comparisons.

### 3.8 Why not Fory

Restated in this design's voice (matches `SPEC.md` §7.2.4):

1. **mmap addressability.** The reader maps the pak read-only and
   addresses the cluster-DAG, BLAS-recipe, and page regions by raw
   byte offset (§3.1, §4.3). Fory's tagged variable-length encoding
   would force a parse pass before any field is addressable;
   defeats mmap.
2. **Byte-equal determinism.** Fory's tag-skipping admits multiple
   byte-equivalent forms; the pak's contract is single-canonical
   bytes (`SPEC.md` §4.1.7 invariant 2, PHILOSOPHY §7).
3. **Refusal-not-default.** Fory's `since` clause synthesises
   defaults at deserialise time; the runtime must refuse loads of
   unknown schemas, not synthesise them. `FormatHash` mismatch is
   the only acceptable drift signal, and it is binary.
4. **Decoder ownership.** Domain-specialised decoders (Draco for
   streams, hand-rolled u32/u8 for tables, CRC32C intrinsics for
   page integrity) cannot ride Fory's reflection-style codecs
   without pulling them onto the runtime hot path.

Fory's role for the pak ecosystem is therefore confined to the
side-records that *describe* the pak (`CookManifest`,
`BLASRecipeRecord`, `MeshSourceMetadata`, `PakHeaderRecord`) — see
`SPEC.md` §7.1 for those schemas; they are not part of this design.

## 4. Public Surface

The aggregate's public C++ surface is the writer (cook-time only,
behind `GLIBRE_GEOMETRY_COOK`) and the reader (runtime), both
declared in the geometry §5 header (`SPEC.md` lines 1502–1526 for
the reader; the writer is the cook-time `pak::write_pak` / `pak::
inspect_pak` entry points complementing `cook::cook_mesh`). This
section refines the §5 declarations with the byte-format-specific
contract every implementer must obey. No new types are introduced;
the closed sum `geometry::Error` is unchanged.

Every fallible operation returns `glibre::Result<T> = std::expected<T,
glibre::Error>` per `reviews/decisions/error-model.md`. No exception
crosses the plugin or the cook-driver boundary. POD spans
(`eastl::span<const std::byte>`, `eastl::span<std::byte>`) carry
buffers across the seam; the aggregate never owns heap allocations
outside its constructor.

### 4.1 Writer — `PakWriter` (cook-time only)

#### 4.1.1 Inputs

The writer takes the **already-staged outputs** of the cook pipeline
(sibling #781) and emits one pak. It does **not** drive
meshoptimizer or Draco; those are stages the cook pipeline runs
upstream.

```cpp
#if defined(GLIBRE_GEOMETRY_COOK)
namespace glibre::geometry::cook::pak {

// PakWriter's narrowed projection of the cook pipeline's internal group
// record (#781).  Only the four fields the writer actually consumes are
// present here; bounds, cone, SSE, meshlet ranges, and DAG adjacency lists
// live exclusively in the cook pipeline's own group record and are never
// passed across the writer boundary.
struct StagedMeshletGroup {
    std::uint32_t                   group_index;     // pak-wide
    std::uint32_t                   page_index;      // owning PakPage
    LODBand                         band;            // used for LOD0 cross-check (§4.1.3 step 6)
    eastl::array<std::uint32_t, kAttributeKindCount> draco_decoded_byte_length{};
    // decode-pool-max derivation (design §4.1.3 step 4)
};

struct StagedDracoStream {
    std::uint32_t                   group_index;
    AttributeKind                   kind;
    DracoQuantisationProfile        profile;
    eastl::span<const std::byte>    compressed_bytes;
    std::uint32_t                   decoded_byte_length;
};

// Callers must resolve MaterialHandle → slot index before populating this
// struct; the writer writes material_slot_index directly as a u32 LE into
// the binary BLAS descriptor (§3.5.1) without any handle accessor call.
struct StagedBLASGeometryDescriptor {
    std::uint32_t                   group_index;         // LOD0 only
    std::uint32_t                   vertex_byte_offset;
    std::uint32_t                   vertex_count;
    std::uint32_t                   vertex_stride_bytes;
    std::uint32_t                   index_byte_offset;
    std::uint32_t                   index_count;
    std::uint8_t                    index_format;        // 0=u32, 1=u16
    std::uint8_t                    vertex_format;
    std::uint32_t                   material_slot_index; // resolved from MaterialHandle by caller; written as u32 LE (§3.5.1)
};

struct StagedPak {
    // Header inputs (§3.2)
    std::uint64_t                                          source_content_hash;
    std::uint16_t                                          engine_version_major;
    std::uint16_t                                          engine_version_minor;
    std::uint16_t                                          engine_version_patch;
    DracoQuantisationProfile                               draco_profile;
    std::uint8_t                                           lod_band_count;
    eastl::array<std::uint32_t, kAttributeKindCount>       decode_pool_scratch_max;
    eastl::span<const ResidencyHint>                       residency_hints;     // size == page_count

    // Cluster DAG (§3.3) — opaque blob from sibling #779
    eastl::span<const std::byte>                           cluster_dag_bytes;

    // BLAS recipe (§3.5)
    BLASBuildFlags                                         blas_flags;
    eastl::span<const StagedBLASGeometryDescriptor>        blas_descriptors;    // sorted by group_index

    // Pages (§3.4) — one entry per pak page
    eastl::span<const StagedMeshletGroup>                  groups;
    eastl::span<const StagedDracoStream>                   streams;
    std::uint32_t                                          page_count;
    std::uint32_t                                          target_page_size_bytes;  // ≤ kPageMaxByteSize
};

struct WrittenPak {
    std::uint64_t  byte_length;
    FormatHash     format_hash;
    std::uint64_t  source_content_hash;
};

[[nodiscard]] glibre::Result<WrittenPak>
    write_pak(const StagedPak& staged,
              eastl::string_view output_path) noexcept;

[[nodiscard]] glibre::Result<std::uint64_t>
    estimate_pak_byte_length(const StagedPak& staged) noexcept;

}  // namespace glibre::geometry::cook::pak
#endif  // GLIBRE_GEOMETRY_COOK
```

`write_pak(...)` is the writer's single entry point and the **only**
function in the geometry codebase that emits `MeshletPak` bytes. It
is not on the runtime hot path; the runtime dylib does not link it
(`SPEC.md` §6.5 shipping cuts).

`estimate_pak_byte_length(...)` is provided for the cook driver's
disk-space pre-allocation step; it is the same arithmetic the writer
performs internally (region offsets + tail pad + page table) and is
exposed because the cook driver wants the answer before the pak
file is opened.

#### 4.1.2 Determinism contract

`write_pak` is **byte-deterministic**: equal `StagedPak` inputs
produce byte-equal output files on every supported host (`SPEC.md`
§4.1.7 invariant 2, PHILOSOPHY §7). Determinism rests on:

1. **Fixed iteration order.** Groups iterated by ascending
   `group_index`; streams iterated by `(group_index ascending,
   AttributeKind ascending)`; descriptors iterated by ascending
   `group_index_pak`.
2. **Zero-padded alignment.** All region pads, the variable-tail
   pad, and per-page CRC alignment are filled with `0x00` only.
3. **No floating-point reduction.** The writer does no
   `f32`-aggregating arithmetic; bounds and SSE values are copied
   from staged records (the upstream pipeline owns the
   determinism of those values per sibling #781).
4. **Single-pass write.** The writer emits regions in §3.1 order
   in one forward pass; no in-place rewrites of earlier offsets
   except for the header's `header_byte_length`,
   `cluster_dag_offset`, etc. fields, which are computed before
   the header is written (§4.1.3).
5. **Endianness.** Every multi-byte field is `to_le_bytes`-cast
   regardless of host endianness; macOS ARM64 is little-endian, so
   in practice the cast is a no-op, but the writer obeys the rule
   defensively.

Determinism is verified by the cross-host golden harness in §11;
violation refuses CI gate (`SPEC.md` §4.1.7 invariant 2 is a
contractual property, not a quality-of-implementation goal).

#### 4.1.3 Emission order

Single forward pass over the file:

1. **Reserve header.** Allocate `kPakHeaderFixedSize +
   tail_byte_length` bytes at offset 0; do not write fields yet.
   Compute `header_byte_length` (with align-to-16 pad), record it
   for step 6.
2. **Compute region offsets and lengths.**
   - `cluster_dag_offset = header_byte_length` (already 16-aligned).
   - `cluster_dag_length = staged.cluster_dag_bytes.size()`.
   - `blas_recipe_offset = align16(cluster_dag_offset +
     cluster_dag_length)`.
   - `blas_recipe_length = 16 + 64 * staged.blas_descriptors.size()`.
   - `pages_offset = align16(blas_recipe_offset +
     blas_recipe_length)`.
   - For each page, compute `page_byte_offset` (16-aligned) and
     `page_byte_length` from staged streams (page header + group
     index table + stream table + Draco bytes + pad + 4-byte CRC).
   - `page_table_offset = align16(pages_offset + Σ page_byte_length
     + per-page pads)`.
   - Verify every page's `page_byte_length ≤ kPageMaxByteSize` else
     refuse with `geometry::Error::PakPageOversize`.
3. **Compute `FormatHash`.** Read the constant from the generated
   header (§3.7); stamp it.
4. **Compute `decode_pool_scratch_max[]`.** Per
   `AttributeKind`, take the max across `staged.streams` of
   `decoded_byte_length`. Verify each entry equals
   `staged.decode_pool_scratch_max[kind]` else refuse with
   `geometry::Error::CookManifestMissingField` (the staged input
   under-described the pool requirement).
5. **Emit cluster-DAG region.** Copy
   `staged.cluster_dag_bytes` verbatim to `cluster_dag_offset`;
   pad to 16 bytes.
6. **Emit BLAS-recipe region.** Write blob header (§3.5), then
   each descriptor in sorted order; verify all `group_index`
   reference an LOD0 group else refuse with
   `geometry::Error::BLASRecipeInvalid`. Write
   `descriptor.material_slot_index` verbatim as u32 LE into the
   `material_slot_index` field of the binary record (§3.5.1); the
   caller has already resolved any `MaterialHandle` to its slot index
   before staging (see `StagedBLASGeometryDescriptor` comment).
7. **Emit pages.** For each page in ascending `page_index`:
   - Reserve page header.
   - Write `group_index[]` from groups in this page; if `G` is odd,
     append 4 zero bytes so `StreamTableEntry[]` starts at
     `0x0010 + align8(G×4)` (8-byte aligned).
   - Write `StreamTableEntry[]` for streams in this page.
   - Append Draco bytes for each stream in `(group_index_local
     ascending, AttributeKind ascending)` order.
   - Compute CRC32C over `[page_start, page_start +
     payload_byte_length)`.
   - Write CRC trailer; pad to 16 bytes.
   - Patch page header (`page_format_version`, `group_count`,
     `payload_byte_length`, `stream_count`).
8. **Emit page table.** Sequentially write
   `PageTableEntry[page_count]` (offsets and lengths from step 2 /
   step 7).
9. **Patch header.** Seek to offset 0; write `PakHeader` fixed
   prefix + tail (per-attribute scratch maxima + residency hints +
   pad).
10. **Flush + fsync.** Close the file with deterministic flush; the
    write is observable atomically only after the cook driver
    renames the temp file to its final path (cooker policy; the
    writer uses temp-then-rename per
    `tools/glibre-meshcc/cli.cpp`'s discipline).

The writer makes one allocation: a contiguous in-memory staging
buffer sized to `estimate_pak_byte_length(...)`. The buffer is
written to disk in one `write` call (or in chunked writes if the
buffer exceeds 16 MiB, with deterministic chunk boundaries on
16-byte alignment). No allocations happen inside the per-page or
per-stream loops; the staging buffer is the cooker's only hot heap.

#### 4.1.4 Failure modes (writer arms)

The writer surfaces the cook-time arms of `geometry::Error`
(`SPEC.md` §10.1 cook-time set):

- `PakPageOversize` — any page's computed byte length exceeds
  `kPageMaxByteSize` (step 2).
- `BLASRecipeInvalid` — descriptor list non-sorted, references a
  non-LOD0 group, or fails the LOD0-cover check (step 6).
- `CookManifestMissingField` — staged inputs under-describe the
  decode-pool maxima (step 4).
- `PakWriterIoFailed` — host filesystem write failure (step 10
  flush, or any chunked write).
- All other cook-time arms (`MeshletOversize`, `ClusterDAGCycle`,
  `MeshSourceInvalidTopology`, etc.) surface upstream of the
  writer; they cannot fire here by construction.

`glibre-meshcc` maps these to process exit codes (`SPEC.md` §10.7).

### 4.2 Reader — `PakReader` (runtime, cold path)

The reader is the §5-declared `class PakReader` (`SPEC.md` lines
1511–1526), refined here with byte-format-specific contract.
Construction is private; the registry's `register_mesh` is the
single factory.

```cpp
namespace glibre::geometry {

// Refined factory and accessors complementing SPEC §5 declaration.
class PakReader {
public:
    [[nodiscard]] static glibre::Result<eastl::unique_ptr<PakReader>>
        create_from_mapping(eastl::span<const std::byte> pak_bytes,
                            eastl::string_view           pak_path) noexcept;

    [[nodiscard]] glibre::Result<PakHeaderInfo>          header()              const noexcept;
    [[nodiscard]] eastl::span<const std::byte>           cluster_dag_bytes()   const noexcept;
    [[nodiscard]] eastl::span<const std::byte>           blas_recipe_bytes()   const noexcept;
    [[nodiscard]] std::uint32_t                          page_count()          const noexcept;
    [[nodiscard]] glibre::Result<ResidencyHint>
        page_hint(std::uint32_t page_index) const noexcept;
    [[nodiscard]] glibre::Result<eastl::span<const std::byte>>
        page_bytes(std::uint32_t page_index) const noexcept;
    [[nodiscard]] glibre::Result<eastl::span<const StreamTableEntry>>
        page_streams(std::uint32_t page_index) const noexcept;
    [[nodiscard]] glibre::Result<eastl::span<const std::byte>>
        page_stream_bytes(std::uint32_t page_index,
                          std::uint32_t stream_index) const noexcept;

    ~PakReader();
    PakReader(const PakReader&)            = delete;
    PakReader& operator=(const PakReader&) = delete;
private:
    PakReader() noexcept;
};

}  // namespace glibre::geometry
```

The factory takes the registry-provided memory mapping (the
registry owns the mmap; the reader is a non-owning view + a
validated `PakHeader`). The `pak_path` is carried for diagnostic
payloads (every `geometry::Error` arm here records it).

#### 4.2.1 Header validation — the eight-step gate

Construction performs exactly eight validation steps in fixed
order (`SPEC.md` §7.2.2 reader-side validation order, restated and
expanded here):

| Step | Check                                                                                                | Refusal arm                                                  |
|------|------------------------------------------------------------------------------------------------------|--------------------------------------------------------------|
| 1    | `pak_bytes.size() >= kPakHeaderFixedSize` AND magic bytes [0:4] == `kPakMagic`                       | `PakHeaderMagicMismatch`                                     |
| 2    | `header.format_hash == kCompiledFormatHash`                                                          | `PakFormatHashMismatch`                                      |
| 3    | `header.header_byte_length` ≤ `pak_bytes.size()` AND ≥ `kPakHeaderFixedSize` AND 16-aligned          | `PakHeaderOffsetOutOfRange`                                  |
| 4    | `header.cluster_dag_offset + header.cluster_dag_length` ≤ `pak_bytes.size()` AND offset 16-aligned   | `PakHeaderOffsetOutOfRange`                                  |
| 5    | `header.blas_recipe_offset + header.blas_recipe_length` ≤ `pak_bytes.size()` AND offset 16-aligned    | `PakHeaderOffsetOutOfRange`                                  |
| 6    | `header.page_table_offset + header.page_count * sizeof(PageTableEntry)` ≤ `pak_bytes.size()`         | `PakHeaderOffsetOutOfRange`                                  |
| 7    | `header.meshlet_max_vertices == 64` AND `header.meshlet_max_triangles == 124`                        | `PakFormatHashMismatch` (defence-in-depth — caps fold into hash) |
| 8    | `header.draco_profile != Reserved` AND resolves inside live closed-sum                               | `DracoProfileUnknown`                                        |

Each step short-circuits on first failure; the arm is reported
through the `Result<T>` return; the partially-constructed reader is
discarded; the caller's mmap is unchanged. After all eight pass,
the reader caches: header view, cluster-DAG span, BLAS-recipe span,
page-table span, residency-hint span. No further validation runs at
runtime — the per-page CRC32C check is the decode-time gate, not a
construction-time gate (§4.2.3).

#### 4.2.2 Lock-free reads

Every `const`-qualified accessor reads only the immutable mmap and
the cached spans (`SPEC.md` §4.1.11 invariant 4). No mutex is taken;
no shared mutable state lives inside `PakReader`. Concurrent reads
from the registry's phase-7 mutation point and the decode pool's
worker threads are correct without coordination (`SPEC.md` §6.4
concurrency model).

#### 4.2.3 Per-page byte access

`page_bytes(page_index)` returns the full byte span for one page,
including header and CRC trailer. The page-table lookup is an O(1)
array index; range check returns
`geometry::Error::PakHeaderOffsetOutOfRange` for out-of-range.

`page_streams(page_index)` parses the page header + stream table
in-place (no allocation; the returned span aliases the mmap) and
returns the `StreamTableEntry` span for the page. Before constructing
the span, the accessor asserts that the page header's `stream_count`
satisfies the upper-bound contract: `stream_count ≤ group_count *
kPakAttributeKindCount`. A page whose `stream_count` exceeds this
bound is rejected with `PakHeaderOffsetOutOfRange` (a crafted page
with an oversize `stream_count` would otherwise let the stream-table
span stride past the page payload). Per-page parsing runs in
O(group_count + stream_count) time, ≤ a few hundred nanoseconds for
typical S1 pages (§9 perf budget).

`page_stream_bytes(page_index, stream_index)` resolves a single
Draco-compressed stream's byte range; this is the input the decode
pool's worker reads. Range check returns
`PakHeaderOffsetOutOfRange` if the stream entry's `draco_byte_offset
+ compressed_byte_length` exceeds the page's `payload_byte_length`.
The same upper-bound contract (`stream_count ≤ group_count *
kPakAttributeKindCount`) applies here via the validated span from
`page_streams()`.

CRC32C verification is **not** performed on these accessors (cold
path); it runs at decode time inside `DecodePool::decode(...)`
(sibling #785, `SPEC.md` §6.3.2 step 3) so the cost is paid once
per residency promotion, not on every byte access. A reader-only
`verify_page_crc(page_index)` accessor is reserved for the editor's
"validate pak" path (out of MVP scope; `[OPEN]` §12 #3).

#### 4.2.4 Failure modes (reader arms)

Runtime arms surfaced (`SPEC.md` §10.1 runtime set):

- `PakHeaderMagicMismatch` — step 1.
- `PakFormatHashMismatch` — step 2 / step 7.
- `PakHeaderOffsetOutOfRange` — steps 3–6, plus per-accessor range
  checks.
- `PakReaderUnvalidated` — payload-touching accessor called before
  successful construction; by `SPEC.md` §4.1.11 invariant 1 this is
  a programming error (the factory returns either a fully validated
  reader or none). Listed as the contract guard arm.
- `PakIoFailed` — the mmap surfaced an OS-level read fault (mmap
  unavailability is rare on shipping builds; reachable on dev
  network mounts). Translated from the platform layer's `IoFailure`
  arm at the reader's ingress per `error-model.md` composition
  rule 2; raw `OsCode` does not cross.
- `DracoProfileUnknown` — step 8.

The reader does **not** surface `PakPageIntegrityFailed` — that arm
is the decode pool's (`SPEC.md` §10.3.3 `decode` row), fired when
the per-page CRC is computed and disagrees.

### 4.3 Tooling — `inspect_pak`

The cook-time `cook::inspect_pak` entry point (`SPEC.md` §5 lines
1812–1813) reads a pak's `PakHeader` without a full mmap: it takes
a `eastl::span<const std::byte>` over the file's first
`kPakHeaderFixedSize + decode_pool_scratch_max_byte_length +
residency_hint_byte_length` bytes and validates steps 1–7 of
§4.2.1 against the prefix span. Used by:

1. The build farm's determinism gate (cook on host A, cook on host
   B, byte-compare — `inspect_pak` confirms both files declare the
   same `FormatHash` before the byte-equality check is meaningful).
2. The editor's content browser (preview the `PakHeaderRecord`
   without opening the full mmap).
3. CI's "pak repository scan" (visit every pak under
   `assets/cooked/`, confirm `format_hash == kCompiledFormatHash`,
   fail the build on any mismatch — the same rule
   `SPEC.md` §7.3.2 consequence 4 demands).

`inspect_pak` returns `PakHeaderInfo` (`SPEC.md` §5 line 1502); the
prefix span discipline keeps the call cheap (≤ 1 KiB read for the
typical pak) and side-effect-free.

## 5. Hot/Cold Path Split

The aggregate is a **build-time emitter** + **load-time validator**
+ **cold-path byte accessor**. It is not on the runtime per-frame
hot path; render's LOD-band selector and the decode pool's worker
threads consume byte spans the reader exposes, but the spans
themselves are immutable mmap views with no per-frame mutation.

| Path     | What runs                                                                                                  | Where                                                                                                | When                                                                                  |
|----------|------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------|
| Hot      | (none owned by this aggregate)                                                                             | n/a                                                                                                  | Per-frame phases 6 / 7 are owned by the runtime registry (sibling #789), not the pak. |
| Cold     | Reader factory (`create_from_mapping`) + 8-step validation                                                 | `runtime/registry.cpp` calling `pak/reader.cpp` at `register_mesh` time (`SPEC.md` §6.3.1)           | Once per `MeshHandle` registration; once per pak hot-reload swap (§8).                |
| Cold     | Page byte-span lookups (`page_bytes`, `page_streams`, `page_stream_bytes`)                                  | `runtime/decode_worker.cpp` driving the decode pool (sibling #785)                                   | Once per residency promotion; off-frame on a worker thread.                           |
| Build    | Writer (`write_pak`)                                                                                       | `tools/glibre-meshcc` cook driver process (`SPEC.md` §6.2 step 7)                                     | Once per cook invocation per mesh; never inside the engine runtime.                   |
| Tooling  | `inspect_pak`                                                                                              | Editor / build farm / CI                                                                              | On demand; not in the runtime dylib's link set.                                       |

The split means:

- **The runtime dylib does not link the writer.** `GLIBRE_GEOMETRY_COOK`
  guards `write_pak` and `estimate_pak_byte_length`; the runtime
  build's link set excludes meshoptimizer + Draco encoders + the
  writer's staging buffer code (`SPEC.md` §6.5 shipping cuts).
- **The runtime hot path never touches a pak byte uncached.**
  Render's LOD-band selector consults the registry's `MeshletGroupView`
  records (`SPEC.md` §4.1.10 / §5), which the registry materialises
  from cluster-DAG bytes at `register_mesh` time and caches in
  `runtime/handle_table.cpp`'s slot record. The reader is a
  cold-path provider, not a per-frame consumer.
- **Pak mmap is exempt from the heap budget.** `SPEC.md` §9.5
  declares the read-only mapping is OS-paged and not heap-tagged
  (`MAP_PRIVATE | PROT_READ`). The 256 MiB ceiling covers the
  registry's CPU shadow, not the pak bytes themselves.

The cooker side (writer) is bounded by §9 throughput; the runtime
side (reader) by §9 wall-time per pak. Both are off the per-frame
hot path; conflating them would force the reader's mmap setup onto
the driver thread (rejected — `SPEC.md` §6.3.1 places `register_mesh`
on the caller's phase, typically phase 1 / 8 in editor, never inside
phase 6 / 7).

## 6. Concurrency

Two independent concurrency contexts; they do not share threads.

### 6.1 Writer (cook-time, host-only)

Single-threaded per pak. The cook driver is one process per worker;
the build farm parallelises across meshes by spawning multiple
`glibre-meshcc` processes (`SPEC.md` §6.4 cook-side rule). Inside
one cook job:

- No fibers, no thread pool inside the writer.
- No shared mutable state with concurrently-cooking processes
  (output paths are per-mesh; the temp-file dance is per-process).
- `write_pak` makes one staging-buffer allocation, one or more
  `write` syscalls, one `close`. The cook driver's `rename` is the
  atomicity boundary; a partially-written temp file is never
  observed by the runtime.

### 6.2 Reader (runtime, in-engine)

**Read-only after construction.** Once the eight-step validation
gate passes, the reader's spans are immutable for the reader's
lifetime. Concurrent readers from any thread are correct without
locking (`SPEC.md` §4.1.11 invariant 4).

Construction is single-threaded (the registry's `register_mesh`
serialises pak loads through its phase-7 mutation point per
`SPEC.md` §6.4 runtime-side rule). The mmap call itself is cheap on
macOS / APFS (≤ 100 µs for a 64 MiB pak per §9); no need for a
separate I/O thread.

The reader **does not own** the file mapping. The registry owns the
mmap (so unmapping is sequenced with handle teardown); the reader
borrows a `eastl::span<const std::byte>`. Hot-reload's `migrate`
body (§8 below) destroys the old reader before the registry unmaps
the old file, preventing dangling spans.

### 6.3 Decode pool boundary

The decode pool's worker threads (sibling #785) call
`PakReader::page_stream_bytes(...)` to obtain the Draco-compressed
input. Multiple workers may call into one reader simultaneously;
the calls are pure reads of the immutable mmap, so contention is
zero.

The decode pool also CRC32C-verifies the page bytes the reader
returned (`SPEC.md` §6.3.2 step 3). This means the reader's
accessors return *unvalidated* page bytes — the integrity check is
deliberately deferred to decode time so cold accesses for tooling
(`inspect_pak`, editor previews) skip it. Promoting the check to
construction-time would O(N) the load and is rejected.

## 7. Persistence + ABI

The pak **is** the canonical persisted format for meshlet content
(`SPEC.md` §7.2). It rides two coordinated stability gates,
detailed below.

### 7.1 The `FormatHash` gate

`FormatHash` (§3.7) is the schema-only content hash. Stability
discipline:

1. **Bumps trigger re-cook.** Any change to a §3.7 input bumps
   `FormatHash`; the runtime refuses every pak whose hash mismatches
   (`SPEC.md` §7.3.2 "invalidate, never migrate").
2. **`glibre-foryc` emits the constant.** The build system regenerates
   `_format_hash.hpp` on every release; runtime, cooker, and editor
   read the same constant, so partial-build drift cannot occur
   (`SPEC.md` §7.3.2 consequence 2).
3. **CI pak-repo scan.** Every pak under `assets/cooked/` is scanned
   on `main`; any pak with a mismatching hash fails the build,
   forcing the change author to re-cook before merge (`SPEC.md`
   §7.3.2 consequence 4).
4. **No in-process migration.** There is no `migrate_pak_v<N>_to_v<N+1>`
   table, no per-version chain. The only recovery from a hash bump
   is a fresh cook from `MeshSource`. (Mirrors render's `PSOCacheRecord`
   "invalidate, never migrate" rule, `specs/render/SPEC.md` §7.2.2.)

### 7.2 The middleman ABI gate

`glibre_types_abi_hash` (`reviews/decisions/fory-codegen.md` "ABI
Stability Rules") gates the middleman dylib's load. The pak's
relationship to it:

1. **Independent gates.** A `FormatHash` bump does **not** force a
   middleman ABI bump; a middleman ABI bump does **not** force a
   `FormatHash` bump (`SPEC.md` §7.5 rule 4). Two independent
   surfaces, two independent rebuilds.
2. **Cooker records both.** `<pak>.cookmanifest.fory` records both
   `format_hash` (this design's gate) and `foryc_abi_hash` (the
   middleman's gate; `SPEC.md` §7.1.1 invariant 1). Either mismatch
   forces a re-cook on the cooker's incremental gate; the pak's
   own header carries `FormatHash` only because the runtime's
   pak-load gate is `FormatHash`-only (the middleman gate fires
   earlier, at plugin load time).
3. **Tag namespace alignment.** `MeshHandle` and `MaterialHandle`
   tag types (`SPEC.md` §5 `tags::mesh`, `tags::material`) match
   render's tag namespace (`SPEC.md` §4.2 cross-aggregate
   invariant 9), so the middleman dylib's serialisation of these
   handles is bit-stable across geometry / render boundary
   crossings without translation.

### 7.3 Pak-format ABI composition

`FormatHash`'s composition (§3.7) is the design-time deliverable
this section pins. The composition rules:

1. **Order matters.** The `glibre-foryc` source-emit order for
   `FormatHash`'s inputs is:
   `magic → version → header_table → cluster_dag_layout →
   blas_recipe_layout → page_layout → page_table_layout →
   meshlet_caps → draco_profile_table → attribute_kind_table →
   blas_build_flags_table → crc32_polynomial`. Re-ordering bumps
   `FormatHash` even though the same inputs are present.
2. **Canonical UTF-8 form.** Each layout-table input is emitted
   as `name:type@offset`, comma-separated, sorted by `offset`
   ascending; whitespace is exactly one space after each colon
   and one space after each comma. The `glibre-foryc`
   pretty-printer enforces this; a deviation produces a different
   `FormatHash` and therefore a different pak.
3. **Closed-sum enums.** `DracoQuantisationProfile`,
   `AttributeKind`, and `BLASBuildFlags` are emitted as
   `name=value` pairs sorted by `value`. Adding an enumerator
   appends one line, bumps `FormatHash`.
4. **Crc32 polynomial.** `0x1EDC6F41` (Castagnoli) is part of the
   hash; switching CRC algorithms (e.g. to xxHash3) bumps
   `FormatHash` even if every other byte is unchanged.

### 7.4 Side-records ride Fory

The four sibling Fory side-records (`SPEC.md` §7.1) are
**not** part of this design. They live at
`data/schemas/geometry/{CookManifest,BLASRecipeRecord,
MeshSourceMetadata,PakHeaderRecord}.fory` and ride the standard
data-context migration rules (`specs/data/SPEC.md` §7.4). Their
relationship to the pak is:

- The pak references them only by **content hash equality**
  (`source_content_hash`, `format_hash`); no Fory codec ever
  decodes a pak byte. (`SPEC.md` §7.2.4 closes the loop.)
- The records reference the pak by **path**; the runtime never
  reads `CookManifest`, only the cooker / editor do.
- A side-record absent at load time does not gate the pak load
  (`SPEC.md` §7.1.3 invariant 1 for `MeshSourceMetadata`; analogous
  for `BLASRecipeRecord`); the editor surfaces a "missing
  side-record" warning but rendering continues.

## 8. Hot-Reload

Pak hot-reload is fully specified in `SPEC.md` §8. This design
contributes the **byte-format-specific refresh path** the protocol
machinery routes through. The trigger, observer schema, and four
fixture pairs are all in `SPEC.md` §8.1–§8.6 and are not restated.

### 8.1 Refresh path (writer → cook → file → reader)

A pak hot-reload is a five-stage pipeline; this design owns stages
3 and 4. Stages 1, 2, and 5 are owned by sibling aggregates
(content's filesystem watcher, the cook driver, the runtime
registry):

```text
Stage  Owner               What runs                                                Trigger
-----  ------------------  -------------------------------------------------------  ---------------------------------------
  1    content (#719)      filesystem watcher detects MeshSource change              fs notify on art/<mesh>.fbx
  2    cook (#781)         cook pipeline re-runs (meshopt → meshlet → draco → BLAS)  CookManifest content_hash mismatch
  3    pak (THIS)          PakWriter emits new <mesh>.glibre-pak (temp + rename)     stage 2 produces StagedPak
  4    pak (THIS)          watcher posts PakReloadRequest with new path              renamed file's content_hash differs
                           (registry receives at phase 8, calls reload_pak which
                            constructs new PakReader and validates 8-step gate)
  5    registry (#789)     handle re-resolution + GpuMeshBuffers replacement +       reader validation succeeded
                           BLASRecipe re-publish + MeshReplaced event                (or refusal: previous pak survives)
```

The pak's contribution is **the byte-layout integrity** across the
swap: the writer's determinism contract (§4.1.2) means stage 3
produces byte-equal output for byte-equal staged inputs (so
reproducible cooks across hosts confirm the swap is valid before
the watcher trips); the reader's 8-step gate (§4.2.1) is what fires
the §8.2 refusal arm in `SPEC.md`. There is no in-place mutation;
the old pak file remains on disk (under its old path) until
content's watcher logs the renamed-away artifact.

### 8.2 Watcher coupling (sibling #719)

Content's watcher detects pak file renames and posts a
`PakReloadRequest` (`SPEC.md` §8.1). The pak aggregate's contract
to the watcher:

1. **Path-keyed.** The watcher passes a path; the registry resolves
   it to a `MeshHandle` via the registry's `pak_path → MeshHandle`
   table. The pak does not own that table; sibling #789 does.
2. **Content-hash gated.** The watcher computes Blake3-64 over the
   new file's bytes and compares against the registry's recorded
   `source_content_hash`; a match means the file was rewritten
   without semantic change (e.g. a touch + save with no edit) and
   the watcher silently drops the request. This is the watcher's
   policy, not the pak's; the pak's `source_content_hash` field is
   the input, not the gate.
3. **Atomic rename.** The cooker writes
   `<mesh>.glibre-pak.tmp.<pid>`, fsyncs, and renames to
   `<mesh>.glibre-pak`; the watcher trips on the rename, never on
   the partial write. The pak format does not need to defend
   against partial files because the rename is the atomicity
   boundary; nevertheless §4.2 step 3 (`header_byte_length ≤
   mapped_size`) catches any pathology from a half-written file
   that somehow reached the runtime (e.g. APFS snapshot edge
   cases) and refuses with `PakHeaderOffsetOutOfRange`.

### 8.3 Refusal continuation

The §8.2 refusal arm in `SPEC.md` is `PakFormatHashMismatch`. This
design's contribution: the new pak's bytes remain on disk, untouched,
under the same path; the watcher does not retry; the operator's
recovery is a rebuild that bumps `FormatHash` to match the running
engine (or a re-cook against the engine's current `FormatHash`).
The previous pak's bytes — already mmapped by the still-live
`PakReader` — continue to back the live `MeshHandle` (the registry's
roll-back rule, `SPEC.md` §8.4.1).

A refused swap leaves no orphan: the candidate `PakReader`'s
construction failed, so no mmap was retained beyond the failed
factory call; the file mapping the candidate held briefly is
released by the `Result<T>`'s destructor.

## 9. Performance

Numbers are contractual ceilings, not steady-state expectations. The
pak aggregate's perf is split between (a) runtime read wall-time per
pak (cold path), and (b) cooker write throughput (off-engine).
Geometry's row in `reviews/decisions/perf-budget.md` (0.30 ms phase-6
+ 0.20 ms phase-7 + 256 MiB) is owned by the runtime registry, not
this aggregate; the pak contributes to the registry's load-time
slice (counted under register_mesh's amortised cost across the
session, not against a per-frame ceiling).

### 9.1 Reader — load wall-time per pak

| Slot                                     | Ceiling                  | Cost model                                                                                                                                |
|------------------------------------------|--------------------------|-------------------------------------------------------------------------------------------------------------------------------------------|
| `mmap` + 8-step header validation        | **≤ 0.5 ms per pak**     | One `mmap` syscall + 96-byte header read + 7 arithmetic checks. M1 + APFS measured baseline ≤ 100 µs for typical paks; ceiling provides headroom for cold-cache disk fetches. |
| `cluster_dag_bytes()` / `blas_recipe_bytes()` accessor | **≤ 100 ns**             | Pure span return from cached fields populated at construction.                                                                            |
| `page_bytes(page_index)` accessor        | **≤ 200 ns**             | Page-table O(1) array index + range check + span construction.                                                                            |
| `page_streams(page_index)` accessor      | **≤ 1 µs**               | Page header parse + stream-table span construction; one cache line for header, one for table prefix.                                      |
| `page_stream_bytes(page_index, stream_index)` accessor | **≤ 100 ns**             | Stream-table O(1) + range check + span return.                                                                                            |
| `page_hint(page_index)` accessor         | **≤ 50 ns**              | Hint-table O(1) byte read.                                                                                                                |
| Per-page CRC32C verification (decode-time) — **warm cache** | **≤ 10 µs / 16 KiB** | `__crc32cd` intrinsics on M1; 2 GB/s sustained throughput at warm-cache rates; 16 KiB / 2 GB/s = ~8 µs; ceiling of 10 µs adds ~25 % headroom for pipeline stalls and sub-16-KiB alignment rounding. |
| Per-page CRC32C verification (decode-time) — **full 64 KiB, warm cache** | **≤ 40 µs** | 64 KiB / 2 GB/s = ~32 µs at hardware ceiling; 40 µs ceiling adds 25 % headroom for pipeline stalls. Cold-cache cost (~30 µs additional for a fresh 64 KiB cold DRAM fetch) is folded into the decode-pool's 5 ms p99 cluster-decode latency (`SPEC.md` §9.4) and not tracked here. |

The 0.5 ms-per-pak load ceiling × 200 props in the S1 fixture =
100 ms total registry-load time at startup. The engine's startup
budget (`reviews/decisions/perf-budget.md` allocator section,
load-time amortisation) absorbs this comfortably; a per-frame
amortisation across N startup frames is the registry's policy, not
the pak's.

### 9.2 Writer — cook throughput

The writer runs in `glibre-meshcc`, off the engine's perf budget.
The throughput contract is for build-farm sizing:

| Slot                                     | Ceiling                  | Cost model                                                                                                                                |
|------------------------------------------|--------------------------|-------------------------------------------------------------------------------------------------------------------------------------------|
| Pak write throughput (writer alone)      | **≥ 200 MiB/s sustained**| Single-threaded; one staging buffer; one `write` syscall. M1 baseline ≥ 1 GiB/s for raw bytes; the 200 MiB/s ceiling assumes typical mesh sizes (≤ 64 MiB) where syscall overhead and CRC32C computation dominate. |
| Per-page CRC32C (writer-side)            | **≥ 2 GB/s sustained**   | Hardware-accelerated `__crc32cd` intrinsic; matches the runtime ceiling on the same M1.                                                   |
| Total cook time per S1 mesh (incl. all upstream stages) | **≤ 5 s p99**           | Cook driver budget (sibling #781); the writer's slice is ≤ 5 % of total cook time per typical mesh. Listed for context, not enforced here. |

Build-farm scaling: with `min(8, hwconcurrency)` parallel cook
processes and 200 MiB/s per process, the farm cooks ≥ 1.6 GiB/s of
mesh data — comfortably above MVP content scale (sibling spike
#781 sizes the content pipeline against a 50 GiB MVP corpus,
implying ≤ 30 s full re-cook on a 16-core M2 dev box, which is the
build-farm SLA).

### 9.3 Heap / mmap accounting

| Sub-budget                       | Ceiling      | Notes                                                                                                                                                                            |
|----------------------------------|--------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Reader heap (`PakReader` per pak) | **≤ 256 B**  | Cached spans + path string; no payload bytes copied.                                                                                                                              |
| Pak mmap (per pak)               | **exempt**   | `MAP_PRIVATE | PROT_READ`; OS-paged; not heap-tagged (`SPEC.md` §9.5 row 1). HUD reports the resident-set fraction for visibility.                                                |
| Writer staging buffer            | **= estimated pak byte length, single allocation per call** | Released on return. The cook driver's heap is sized by the build farm orchestrator, not by the runtime budget; conservative ceiling = 256 MiB per cook process. |

### 9.4 CI gates

The pak aggregate plugs into the engine's perf gate
(`reviews/decisions/perf-budget.md` §"CI Gate Spec"):

| Catch2 benchmark / CI gate                                    | Measures                                                                                                       | Asserts            |
|---------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------|--------------------|
| `BENCHMARK("PakReader cold load, S1, p99")`                   | `mmap` + 8-step validation wall-time per pak; warm-disk; cold-CPU-cache.                                       | ≤ 0.5 ms           |
| `BENCHMARK("PakReader page accessors, S1, p99")`              | `page_streams + page_stream_bytes` round-trip per page; warm caches.                                           | ≤ 1 µs             |
| `BENCHMARK("PakWriter throughput, S1 mesh, 64 MiB output")`    | Bytes / second from `write_pak` entry to file `close`; staging buffer alloc included.                          | ≥ 200 MiB/s        |
| `BENCHMARK("Pak CRC32C throughput")`                          | Bytes / second through `__crc32cd` for a 1 MiB buffer; M1 baseline.                                            | ≥ 2 GB/s           |
| `BENCHMARK("Pak determinism: cross-host byte-equal, S1")`     | Cook on host A and host B in CI matrix; byte-compare outputs.                                                  | byte_equal         |
| `BENCHMARK("Pak repo scan: every assets/cooked/*.glibre-pak loads")` | `inspect_pak` over every shipped pak; assert `format_hash == kCompiledFormatHash`.                       | all hashes match   |

## 10. Failure Modes

The aggregate contributes the following arms to `geometry::Error`
(closed-sum declared in `SPEC.md` §5; this section restates only
the arms whose seam is owned by this design — none are added).

### 10.1 Runtime arms (reader)

| Arm                            | Trigger (this aggregate's seam)                                                                                  | Recovery (this aggregate's contract)                                                                            | Routes to (`SPEC.md`)                                       |
|--------------------------------|------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------|
| `PakHeaderMagicMismatch`       | §4.2 step 1 fails — first four bytes ≠ `kPakMagic`. Wrong file, wrong format, or truncation inside magic.        | Refuse load; `Result<T>` returns this arm; mmap discarded; no mesh handle issued.                                | §10.2 `PakHeaderMagicMismatch`; §8 hot-reload refusal arm.  |
| `PakFormatHashMismatch`        | §4.2 step 2 (or step 7 defence-in-depth) fails — `header.format_hash` ≠ `kCompiledFormatHash` or caps drift.      | Refuse load. No migration path (§7.1 rule 4). Operator re-cooks against the live engine.                         | §10.2 `PakFormatHashMismatch`; §8.2 sole hot-reload gate.   |
| `PakHeaderOffsetOutOfRange`    | Any of §4.2 steps 3–6 fails, or any per-accessor range check (page index out of bounds; declared offset overruns mmap). | Refuse load (construction-time) or refuse the specific accessor (runtime). Caller handles via residency-downgrade loop. | §10.2 `PakHeaderOffsetOutOfRange`.                          |
| `PakReaderUnvalidated`         | Public payload-touching accessor called on a reader whose factory failed; programming error per `SPEC.md` §4.1.11 invariant 1. | CI promotes to build failure (`SPEC.md` §10.2 row).                                                              | §10.2 `PakReaderUnvalidated`.                               |
| `PakIoFailed`                  | OS-level read fault on the mmap (typically EIO on dev network mounts).                                           | Refuse load (or evict if already loaded); registry invalidates affected `MeshHandle`.                            | §10.2 `PakIoFailed`.                                        |
| `PakPageIntegrityFailed`       | Deferred to decode pool (sibling #785) — fired when CRC32C disagrees. Surface here only as a contract reference.  | Decode pool refuses page; residency downgrade kicks in.                                                          | §10.2 `PakPageIntegrityFailed`; §10.4.1 downgrade loop.     |
| `DracoProfileUnknown`          | §4.2 step 8 fails — header's `draco_profile` is `Reserved` or outside the live closed sum.                       | Refuse load. Editor surfaces "asset out of date" banner; cook against current engine.                            | §10.2 (re-derived from `SPEC.md` §10.1 cook-time set).      |

### 10.2 Cook-time arms (writer)

| Arm                            | Trigger (this aggregate's seam)                                                            | Recovery                                                                                            | Routes to (`SPEC.md`)                                  |
|--------------------------------|--------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------|--------------------------------------------------------|
| `PakWriterIoFailed`            | Step 10 flush / chunked write surfaced a host filesystem failure (out of disk, EROFS, etc.).| Cooker exits with code `20` (`SPEC.md` §10.7 mapping).                                              | §10.7 row 6.                                           |
| `PakPageOversize`              | Step 2 detected a page whose computed byte length exceeds `kPageMaxByteSize`.              | Cooker exits with code `20`; the cook pipeline is told to re-page (sibling #781).                    | §10.7 row 6.                                           |
| `BLASRecipeInvalid`            | Step 6 detected a non-LOD0 descriptor or out-of-order list.                                 | Cooker exits with code `13`.                                                                         | §10.7 row 4.                                           |
| `CookManifestMissingField`     | Step 4 detected an under-described decode-pool maxima staged input.                        | Cooker exits with code `21`.                                                                         | §10.7 row 7.                                           |

### 10.3 Refusals (out of scope — routed)

- **Cluster-DAG schema validity.** Owned by sibling #779; the
  writer accepts the DAG bytes verbatim as a span and the reader
  exposes them as a span. A malformed DAG that decodes incorrectly
  inside sibling #779 does not return a pak-aggregate arm.
- **Decode integrity.** `DracoDecodeFailed`, `DecodePoolBusy`,
  `DecodePoolUndersized`, `DecodePoolOverflow` are owned by
  sibling #785; the pak surfaces only the inputs (compressed
  bytes, profile id, expected decoded byte length).
- **Residency state.** `ResidencyTransitionIllegal`,
  `ResidencyHintImmutable`, `PageNotResident` are owned by
  sibling #787; the pak's `residency_hint_table[]` is read-only
  input, not state.
- **Handle lifecycle.** `MeshHandleStale`, `MeshletGroupHandleStale`,
  `MeshHandleNotFound`, `MeshAlreadyRegistered` are owned by
  sibling #789; the pak is opaque to handle identity.
- **GPU upload.** `GpuUploadRefused`, `GpuBufferAllocFailed` are
  owned by render's `RTAccelStructures` peer (`specs/render/SPEC.md`
  §4.1.8); the pak refuses to invoke a GPU API.

## 11. Test Plan

Unit + integration tests covering the writer / reader round-trip,
malformed inputs, and cross-host byte-equality. All tests live under
`tests/geometry/pak/` (Catch2). Names below are the canonical Catch2
test names referenced by `SPEC.md` §11 acceptance criteria — this
section refines them with the specific assertions every test must
make.

### 11.1 Round-trip tests

| Test name                                              | Asserts                                                                                                           |
|--------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------|
| `pak_round_trip_empty_mesh`                            | A `StagedPak` with one LOD0 group and one page round-trips; reader returns identical header, DAG span, recipe span, page bytes. |
| `pak_round_trip_multi_band_multi_page`                 | A 5-LOD-band, 200-group, 16-page mesh round-trips byte-equal; every group's stream bytes recovered byte-equal.    |
| `pak_round_trip_residency_hints_preserved`             | All eight `ResidencyHint` enumerators ride the writer / reader unchanged.                                         |
| `pak_round_trip_decode_pool_scratch_max_correct`       | Header's `decode_pool_scratch_max[]` matches the staged input maxima for every `AttributeKind`.                   |
| `pak_round_trip_blas_recipe_descriptors_sorted`        | Reader's `blas_recipe_bytes()` parses to descriptors in `group_index_pak` ascending order regardless of staged input order. |
| `pak_round_trip_page_table_offsets_align16`            | Every `PageTableEntry.page_byte_offset % 16 == 0`; every region offset 16-aligned.                                |

### 11.2 Malformed input tests

| Test name                                              | Asserts                                                                                                           |
|--------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------|
| `pak_bad_magic`                                        | A buffer with first 4 bytes `0x00 0x00 0x00 0x00` returns `PakHeaderMagicMismatch` from §4.2 step 1.              |
| `pak_format_hash_mismatch_first_load`                  | A pak with stub `format_hash` returns `PakFormatHashMismatch` from §4.2 step 2.                                   |
| `pak_format_hash_mismatch_via_meshlet_caps`            | A pak with `meshlet_max_vertices = 32` (cap drift) returns `PakFormatHashMismatch` from §4.2 step 7 even when step 2 was bypassed via stub. |
| `pak_truncated_header`                                 | A `kPakHeaderFixedSize - 1`-byte buffer returns `PakHeaderMagicMismatch` (size short-circuit) or `PakHeaderOffsetOutOfRange` (step 3) depending on truncation point. |
| `pak_cluster_dag_offset_overruns`                      | A pak whose `cluster_dag_offset + cluster_dag_length > pak_bytes.size()` returns `PakHeaderOffsetOutOfRange` from §4.2 step 4. |
| `pak_blas_recipe_offset_overruns`                      | Symmetric for §4.2 step 5.                                                                                        |
| `pak_page_table_overruns`                              | Symmetric for §4.2 step 6.                                                                                        |
| `pak_draco_profile_reserved`                           | A pak with `draco_profile == Reserved` returns `DracoProfileUnknown` from §4.2 step 8.                            |
| `pak_page_index_out_of_range`                          | `page_bytes(page_count)` returns `PakHeaderOffsetOutOfRange`.                                                     |
| `pak_stream_index_out_of_range`                        | `page_stream_bytes(p, stream_count(p))` returns `PakHeaderOffsetOutOfRange`.                                      |
| `pak_unvalidated_payload_access`                       | Calling `page_bytes` on a reader whose factory returned an error returns `PakReaderUnvalidated` (debug-only contract guard). |

### 11.3 Determinism / cross-host tests

| Test name                                              | Asserts                                                                                                           |
|--------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------|
| `pak_writer_byte_equal_same_input`                     | Two `write_pak` calls with the same `StagedPak` produce byte-equal files.                                          |
| `pak_writer_byte_equal_across_hosts`                   | CI matrix runner: cook on macOS-14-ARM and macOS-15-ARM; assert byte-compare. Gated on §9.4 `BENCHMARK("Pak determinism: cross-host byte-equal, S1")`. |
| `pak_writer_zero_pad_only`                             | Every region pad and the variable-tail pad are zero-byte-only; a corruption tool that writes `0xFF` into pad ranges fails the byte-equal test. |
| `pak_format_hash_constant_matches_glibre_foryc`        | The compiled `kCompiledFormatHash` from `_format_hash.hpp` equals the value `glibre-foryc` emits when run against the live schema set. |

### 11.4 Golden-file harness

Per `specs/data/SPEC.md` §7.5's pattern, the pak format ships a
golden-file harness under `tests/data/paks/golden/v1/`:

| Fixture                                          | Purpose                                                                                                            |
|--------------------------------------------------|--------------------------------------------------------------------------------------------------------------------|
| `cornell-mesh-v1.pak`                            | Hand-curated 1-prop fixture; reproducible cook from `tests/data/paks/cornell-mesh-source.fbx`. The reference for hot-reload tests (`SPEC.md` §8.6) and for cross-host byte-equality.   |
| `cornell-mesh-v1.pak.cookmanifest.fory`          | Sibling Fory side-record (`SPEC.md` §7.1.1); records the `format_hash`, `source_content_hash`, cook options.                                                                          |
| `cornell-mesh-v1.pak.expected-pakheaderinfo.fory`| Hand-authored `PakHeaderRecord` (`SPEC.md` §7.2.2 twin) golden against which `inspect_pak`'s output is compared.                                                                       |

The harness's CI workflow (`pak-golden.yml` under
`.github/workflows/`):

1. Re-runs `glibre-meshcc cornell-mesh-source.fbx --out
   golden-out/cornell-mesh-v1.pak`.
2. Asserts `golden-out/cornell-mesh-v1.pak` byte-equals
   `tests/data/paks/golden/v1/cornell-mesh-v1.pak`.
3. Asserts `inspect_pak(golden-out/cornell-mesh-v1.pak)` returns a
   `PakHeaderInfo` that matches `expected-pakheaderinfo.fory` on
   every field.

A failure of (2) means the writer's determinism has regressed
(possibly across a clang version bump that re-ordered something the
compiler shouldn't have). The change author responds either by
identifying and fixing the non-determinism, or by amending the
golden + bumping the pak version triple if the change is
intentional.

### 11.5 Concurrency tests

| Test name                                              | Asserts                                                                                                           |
|--------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------|
| `pak_reader_concurrent_page_bytes_threadsafe`          | 8 threads each call `page_bytes(...)` against the same `PakReader` for 1k iterations; assert byte-equal returns and no data races (TSAN-clean). |
| `pak_reader_concurrent_page_streams_threadsafe`        | Symmetric for `page_streams`.                                                                                     |
| `pak_reader_no_mutex_held`                             | TSAN trace asserts no mutex acquisition inside any reader accessor (`SPEC.md` §6.4 lock-free contract).            |

### 11.6 Performance benchmarks

Listed in §9.4; each `BENCHMARK` assertion is a Catch2 case under
`tests/geometry/perf/pak_*` and rides the per-PR perf gate.

## 12. Open Questions

- [OPEN] **#1 — Per-page hash algorithm choice.** This design pins
  CRC32C (Castagnoli, polynomial `0x1EDC6F41`) for per-page integrity
  on the strength of M1's `__crc32cd` intrinsic and CRC32C's
  bit-flip-detection sufficiency for storage media. xxHash3 has
  higher throughput on x86_64 but slightly higher latency on M1; on
  Apple Silicon, CRC32C is the right pick. The question reopens if
  the engine ever ships on a non-Apple-Silicon platform — the gate
  is the platform-targeting spike, not a geometry-side decision.
  Owner: deferred until a non-Apple platform target is admitted.

- [OPEN] **#2 — Truncation-resilient mid-page resync.** The current
  per-page CRC catches whole-page bit-flips; a partial mid-page
  truncation (e.g. SIGKILL during a writer's chunked write that
  somehow survives the temp-then-rename atomicity boundary) would
  produce a page whose declared `payload_byte_length` exceeds the
  remaining mmap. §4.2 step 6 catches the file-level overrun; the
  per-page accessor catches the page-level overrun via the same
  range check. No mid-page resync is provided — a corrupted page is
  refused, not partially recovered. The question reopens if a
  shipping deployment scenario ever requires mid-pak salvage of
  partial bytes; current MVP rebuild-from-source is sufficient.
  Owner: deferred.

- [OPEN] **#3 — Editor "validate pak" path.** A
  `PakReader::verify_all_pages()` accessor that runs CRC32C across
  every page would let the editor surface a "this asset has bit-rot"
  banner before any draw. Current MVP defers verification to decode
  time (cold path); the editor path is post-MVP. Owner: editor
  sub-epic when the content browser's "asset diagnostics" view
  lands; the accessor's signature is reserved here so its addition
  is additive (does not bump `FormatHash`).

- [OPEN] **#4 — Pak format version bump cadence.** The current
  design ships at `pak_version = 1.0.0`. Rules for minor / patch
  bumps inside a single `FormatHash` (i.e. cosmetic schema notes
  that do not change byte layout — e.g. adding a comment in
  `pak_header_field_table_canonical`) are not yet specified.
  Sibling spike `task-breakdown-geometry-meshlet-pak-detailed`
  decides whether to allow comment-only bumps or freeze the version
  triple to `FormatHash` lockstep. Owner: sibling task-breakdown.

- [OPEN] **#5 — Reserved-bytes audit.** The `BLASGeometryDescriptor`
  reserves 28 bytes (§3.5.1) and `PageHeader` reserves 5 bytes
  across two `reserved_align*` fields (§3.4). The values are zero-
  filled and any future field allocation bumps `FormatHash`. The
  question is whether the reserves should be smaller (saving disk
  space at the cost of more `FormatHash` bumps) or larger (cheaper
  future evolution at the cost of disk size). Current sizes are
  driven by cache-line alignment (BLAS descriptor = 64 bytes = one
  cache line) and 16-byte alignment (page header). Owner: revisited
  if a concrete future field needs more than the current reserve.

- [OPEN] **#6 — Cross-pak determinism under clang bumps.** The
  writer's determinism is tested across hosts at one clang version
  (CI matrix). A clang upgrade that changes the std::sort
  tie-breaking inside `glibre-foryc`'s canonical-bytes emitter
  could in principle bump `FormatHash` without a SPEC change.
  `glibre-foryc` is required to use stable-sort with explicit
  comparators for every tie-break; this question reopens if any
  step in the toolchain admits an unstable sort. Owner: build
  toolchain spike.
