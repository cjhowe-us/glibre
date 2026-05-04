# geometry — Detailed Design: cluster-DAG aggregate

> Detailed design for the `ClusterDAG` aggregate (cook-time root) plus
> its runtime cut-selection helper declared in
> `specs/geometry/SPEC.md` §4.1.4 / §4.1.5 / §4.2 invariant 2 / §5
> `select_lod_group` / §6.3.3 / §9.2 row "`select_lod_band` per
> visible mesh" / §10.7. Refines those sections in place; introduces
> no new public surface beyond `specs/geometry/SPEC.md` §5; deviation
> from the cited records requires an amendment spike, not an in-place
> edit. Cites `reviews/decisions/error-model.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/fory-codegen.md`, and
> `reviews/decisions/frame-phases.md`. Resolves
> `[SPIKE] design-geometry-cluster-dag-detailed` (#779).

## 1. Purpose

The cluster-DAG aggregate is geometry's **hierarchical LOD topology**
for one cooked `MeshSource`. Per `SPEC.md` §4.1.5 it owns the acyclic
graph of `MeshletGroup` nodes that encodes the full LOD chain (parent
groups simplify children of the next-finer band) and the cut-selection
algorithm the runtime uses, per `View`, to pick the coarsest band whose
every group has `SSE ≤ T` (§4.2 invariant 2). Concretely the aggregate
is responsible for:

1. The **`ClusterDAG` cook-time root** (`SPEC.md` §4.1.5) — the full
   set of `MeshletGroup` records ordered by `LODBand`, the parent-edge
   / child-edge adjacency lists between bands, the LOD0 cover (the
   single cluster band the `BLASRecipe` references, §4.2 invariant 3),
   and the four DAG-shape invariants (acyclic; edges only between
   adjacent bands; single LOD0 cover; coarsest band reachable from
   every leaf).
2. The **immutable runtime form** of the same DAG, reconstructed from
   the pak's cluster-DAG bytes at `register_mesh` time (§7.4 row
   `ClusterDAG runtime form`) and never mutated thereafter.
3. The **per-`MeshletGroup` LOD-band record** (`SPEC.md` §4.1.4) —
   meshlet index range, parent / child references, group-level
   `BoundingSphere`, per-group `ScreenSpaceError`, watertight-cut
   bookkeeping (per-edge bit mask), `LODBand` tier — and the four
   group-level invariants (watertight cut; per-group SSE strictly
   tightens with finer LOD; LOD0 covers the source; group bounds
   enclose constituent meshlets).
4. The **cut-selection helper** invoked from `GeometryRegistry::
   select_lod_group(MeshHandle, pixel_threshold, view_sphere)` per
   `SPEC.md` §5. The helper walks the DAG from the root and returns
   the `MeshletGroupHandle` of the coarsest band whose every
   constituent group has `SSE ≤ T` *and* is fully resident
   (`§6.3.3`). It is the only runtime entry geometry exposes that
   touches the DAG topology.
5. The **per-view cut state** — a small structure render's
   cull-extract pass (#770) keeps per `View` to memoise the prior
   frame's cut and accelerate the current frame's selection
   (temporal coherence). The cut state is owned by render's
   per-view scratch arena, not by geometry; geometry only specifies
   its byte shape and the recurrence relation that updates it.

This aggregate **refuses to own**:

- **The pak file format and on-disk byte layout** (`SPEC.md` §4.1.7,
  §7.2). Cluster-DAG bytes are *one region* of `MeshletPak` (`§7.2.1`
  layout `cluster_dag_offset` + `cluster_dag_length`); the surrounding
  `PakHeader`, `BLASRecipe` blob, page table, and `PakPage` array are
  the meshlet-pak aggregate (#777). The DAG aggregate specifies its
  byte shape (§7 below) and stops at the region boundary.
- **The cook pipeline that builds the DAG.** `cook/cluster_dag.cpp`
  (`SPEC.md` §6.1, §6.2 step 4) is the construction logic; its
  detailed design is the cook-pipeline ticket (#781). The DAG
  aggregate fixes the *invariants* the cook stage must satisfy and
  the *byte layout* it must emit; it does not specify the
  simplification policy or the meshoptimizer driver shape.
- **The BLAS recipe.** `BLASRecipe` (`SPEC.md` §4.1.7.2) lists the
  LOD0 cover and is the only artefact crossing to render's RT path
  (§4.2 invariant 3). Recipe layout is the BLAS-recipe ticket (#783).
  The DAG aggregate hands the LOD0 cover index list to the recipe
  builder; it does not author the recipe itself.
- **The decode pool.** `DecodePool` (`SPEC.md` §4.1.12) decodes
  `DracoStream` payloads into transient buffers; its design is
  ticket #785. The DAG aggregate consults `ResidencyState` per
  group during cut selection but never enqueues a decode.
- **Residency / streaming.** `ResidencyState` (`SPEC.md` §4.1.13)
  tracks per-page state and is mutated by `content`'s scheduler;
  its design is ticket #787. The DAG aggregate reads residency
  bits *as a filter* during cut selection (a candidate band is
  admissible only if `fully_resident == true`) but never writes
  them.
- **The runtime registry root.** `GeometryRegistry` (`SPEC.md`
  §4.1.14, ticket #789) owns mesh-handle issuance, the phase-7
  mutation point, and the lock-free read surface. The DAG
  aggregate contributes the implementation of one method
  (`select_lod_group`) and the byte shape of one immutable per-mesh
  table; it does not own the registry.
- **Per-frame cull bookkeeping.** Frustum culling, HZB occlusion
  culling, draw-list compaction, and visibility-buffer raster are
  render's cull pass (#770) and consume the
  `MeshletGroupHandle` returned by `select_lod_group`. The DAG
  aggregate produces one handle per (mesh, view) pair; what render
  does with it (cone test, frustum cull, indirect dispatch) is
  outside this seam.

## 2. Requirements coverage

This section verifies that every harmonius MVP-scope requirement /
design clause about the cluster-DAG / LOD-hierarchy is either covered
by the design below or explicitly refused with rationale. Inputs
(research only, re-derived per PHILOSOPHY §"How harmonius is used"):

- `harmonius/docs/design/rendering/meshlets.md` § "Class Diagram" /
  `LodGroup` (R-2.4.3 — "`LodGroup` holds a cluster hierarchy with
  screen-space error per level") and the design's `lod_groups: Vec<LodGroup>`
  composition rule.
- `harmonius/docs/design/rendering/meshlets.md` § "Design Principles"
  items 4 ("Uniform LOD — each LOD level is a complete independent
  meshlet set") and 5 ("BLAS parity — ray-traced geometry equals
  rasterized geometry").
- `harmonius/docs/design/rendering/meshlets.md` § "Open Questions"
  item 1 ("cluster hierarchy cone-over-N vs per-meshlet cones") and
  item 3 ("when a mesh changes LOD count after hot-reload, how are
  existing render instances migrated").
- The harmonius `meshlet-pipeline.md` requirement family
  R-3.1.1 / R-3.1.5 / R-3.1.6 (cited in `SPEC.md` §3 row
  `meshlet-pipeline.md`) — DAG hierarchy with watertight-cut
  invariant; per-`MeshletGroup` projected SSE driving LOD-band
  selection.

| Harmonius clause                                                                                                | Glibre disposition                                                                                                                                                                                                                                                                                                                                                                                                                  |
|-----------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-2.4.3 / R-3.1.1** — Cluster hierarchy with per-level screen-space error.                                     | **Covered.** §3.2 / §3.3 below: `MeshletGroup` carries `LODBand` + per-group `ScreenSpaceError`; `ClusterDAG` records adjacency between bands; cut-selection (§3.6) compares per-group SSE against the view pixel threshold.                                                                                                                                                                                                       |
| **R-3.1.5** — Per-`MeshletGroup` projected SSE drives LOD-band selection.                                        | **Covered.** §3.6 algorithm: `select_lod_group` walks the DAG from root, picks the coarsest band whose every traversed group has `SSE ≤ pixel_threshold` and `fully_resident == true`. Monotonic-tightening invariant (§3.4) makes the search total.                                                                                                                                                                                |
| **R-3.1.6** — Watertight cut at every band selection.                                                            | **Covered.** §3.5: the cut algorithm respects group edges (no half-group cuts); per-edge watertight bit mask (`SPEC.md` §4.1.4 composition) is checked at cook time and consulted at runtime to validate its choice in `O(group degree)`.                                                                                                                                                                                          |
| Harmonius — "Uniform LOD — each LOD level is a complete independent meshlet set."                                | **Refused / collapsed.** Glibre uses a *cluster-DAG* not "uniform LOD per level" because uniform-LOD prevents partial-detail cuts. The DAG admits cuts that mix bands across the mesh (§3.5), giving fine detail near silhouettes and coarse detail behind. The "uniform" property is preserved as a degenerate case (every band has exactly one group) but is not the contract.                                                       |
| Harmonius — "BLAS parity — ray-traced geometry equals rasterized geometry."                                      | **Refused / re-routed.** `SPEC.md` §4.2 invariant 3 fixes BLAS at LOD0 *only*; the rasteriser may pick a coarser band per view but ray tracing always sees LOD0. This is a stronger contract than parity (which would force ray tracing to match the per-view cut, an `O(views)` BLAS rebuild per frame). LOD0-only is what makes BLAS refit affordable per `perf-budget.md` §"render" row.                                            |
| Harmonius — "Cluster hierarchy cone-over-N vs per-meshlet cones" (open Q #1).                                    | **Resolved: per-meshlet cones, plus per-group bounding-sphere only.** §3.3 below: `MeshletGroup` carries one `BoundingSphere` for frustum / occlusion (no group cone). Per-meshlet `BoundingCone` lives on `Meshlet` (`SPEC.md` §4.1.3) and feeds the cull pass (#770), not cut selection. Group-level cones would add ABI surface for an optimisation cull already does at meshlet granularity; collapsed.                              |
| Harmonius — "When a mesh changes LOD count after hot-reload, how are existing render instances migrated" (Q #3). | **Resolved.** §8 below restates the migrate body from `SPEC.md` §8.4: handle bit-identity preserved, group-index re-resolution via stable `group_id` recorded in the cluster-DAG bytes (§7.1.2 `BLASRecipeRecord` shape; the DAG record carries the same key per group). The first post-swap frame's `select_lod_group` rebuilds cut state from scratch.                                                                            |
| Harmonius — `LodGroup { level, screen_error, meshlets, bounds }` shape.                                          | **Covered with refinements.** `MeshletGroup` (§3.3) carries band, SSE, meshlet range, group sphere, plus parent/child edge lists and the watertight bit mask harmonius omits. The omission is what would have broken Q-3.1.6.                                                                                                                                                                                                       |
| Harmonius — "DAG hierarchy with watertight cut invariant."                                                       | **Covered.** §3.5 watertight-cut algorithm + §4 invariants (`SPEC.md` §4.1.4 inv 1, §4.1.5 inv 1–4) re-stated below as design contracts.                                                                                                                                                                                                                                                                                            |
| Harmonius — "DAG mutated post-import for streaming reasons."                                                     | **Refused.** `SPEC.md` §4.1.5 invariant: DAG immutable post-cook; runtime form is reconstructed from pak bytes at `register_mesh`; never rebuilt. Streaming changes residency bits (§4.1.13), not topology. PHILOSOPHY §6 (no runtime reflection) reinforces — the DAG topology is part of the cooked artefact.                                                                                                                       |
| Harmonius — "Skip-level edges across LOD bands for fast coarse fall-back."                                       | **Refused.** `SPEC.md` §4.1.5 invariant 2: edges only between adjacent bands; no skip-level edges. Skip-level fall-back is unnecessary because cut selection terminates in `O(depth)` over an 8-band tree (§3.6); skip edges would inflate the on-disk DAG and complicate the watertight-cut bit mask.                                                                                                                              |
| Harmonius — "Per-group material override."                                                                       | **Refused.** Material lives at `MeshletGroupView::material` (`SPEC.md` §5), populated by the cook pipeline from `MeshSource`'s per-submesh material slot. Per-group materials would let LOD selection accidentally swap shaders mid-mesh, breaking PSO-cache reuse (`render` SPEC §4.1.7). Geometry refuses; render asks for one material per LOD0 cover only.                                                                       |
| Harmonius — "Cluster cones aggregated upward for hierarchical back-face cull."                                   | **Refused / deferred.** Per-meshlet cones are sufficient for MVP cull (`render` cull design #770). Hierarchical cone aggregation is an optimisation that requires a per-band cone per group plus its degenerate-cone sentinel handling; the marginal cull win does not justify the invariant load. Listed in §12 below as `[OPEN]` for post-MVP only.                                                                              |
| Harmonius — "DAG persisted as separate side file for incremental updates."                                       | **Refused.** DAG bytes live inside `MeshletPak` (`SPEC.md` §7.2.1). A side file would multiply the load surface (two mmaps per mesh, two integrity checks, two `FormatHash` gates). Pak-internal placement keeps the DAG one mmap-region away from its consumers; deterministic byte-equal cooks (PHILOSOPHY §7) absorb the "incremental" property without a second file.                                                          |
| Harmonius — "Runtime DAG mutation when LOD policy changes."                                                      | **Refused, hard.** `SPEC.md` §4.1.5: never rebuilt at runtime. LOD policy is a cook-time input (`MeshletBuildOptions`); a policy change forces a re-cook (`FormatHash` is unaffected by policy values, but `source_content_hash` is not — the manifest re-cook gate triggers).                                                                                                                                                       |
| Harmonius — "DAG diff/patch protocol between sessions."                                                          | **Refused.** No "session-persistent" DAG state exists. The DAG is read-only mmap from a pak; runtime cut state is per-view scratch that does not survive frame N. Patch protocols are infeasible against a content-hash-gated immutable artefact.                                                                                                                                                                                   |
| `meshlets.md` — Q #3 "LOD count change after hot-reload migrates existing render instances."                     | **Resolved.** Stable `group_id` (a `u64` recorded inside the DAG bytes per group, §7.1) is the migration key (`SPEC.md` §8.4 step 3). Handle bit-identity preserved; the post-swap DAG may have a different shape but every surviving `MeshletGroupHandle` re-binds against `group_id`, falling back to the LOD0 cover if no match (with a one-frame downgrade per §8.5 observer responsibility 1). Story #488 in §11 below.       |

Net result: every harmonius cluster / LOD-hierarchy clause is either
implemented as specified below or explicitly refused with rationale.
The "many bespoke LOD-mesh schemes" of harmonius collapse into one
`ClusterDAG` aggregate with one cut-selection algorithm and one
LOD0-only BLAS contract per `SPEC.md` §3.2 collapse rules.

## 3. Detailed model

### 3.1 The aggregate cluster

```
ClusterDAG (one per cooked MeshSource; cook-time root)
├── groups_       : eastl::vector<MeshletGroup>      (band-major: LOD0 first, then LOD1, …)
├── band_offsets_ : eastl::array<uint32_t, 9>        (CSR-style; band k = [band_offsets_[k], band_offsets_[k+1]))
├── parent_edges_ : eastl::vector<uint32_t>          (CSR-style; per-group parent group indices)
├── parent_csr_   : eastl::vector<uint32_t>          (per-group offsets into parent_edges_)
├── child_edges_  : eastl::vector<uint32_t>          (CSR-style; per-group child group indices)
├── child_csr_    : eastl::vector<uint32_t>          (per-group offsets into child_edges_)
├── lod0_cover_   : eastl::vector<uint32_t>          (group indices comprising LOD0 cover; band_offsets_[1] entries)
└── group_ids_    : eastl::vector<uint64_t>          (stable id per group; survives re-cook for migration)

MeshletGroup (one node of the DAG; entity record inside ClusterDAG)
├── band              : LODBand                      (0 = finest; 7 = coarsest in MVP)
├── meshlet_range     : { uint32_t first, uint32_t count }   (slice into pak's meshlet table)
├── bounds            : BoundingSphere               (encloses all constituent meshlet spheres)
├── screen_space_error: float                        (projected pixel error at cook reference distance)
├── parent_count      : uint16_t                     (degree into parent_csr_; 0 at coarsest band)
├── child_count       : uint16_t                     (degree into child_csr_; 0 at LOD0)
├── watertight_mask   : uint32_t                     (per-edge bit; 1 = edge may be cut watertight)
├── material          : MaterialHandle               (LOD0 cover only carries material; coarser bands inherit)
└── reserved          : uint32_t                     (zero; reserves alignment slot for future additive field)

CutState (per (View, MeshHandle) pair; render-owned scratch)
├── frame_counter   : uint64_t                       (last frame this state was updated)
├── selected_group  : MeshletGroupHandle             (coarsest admissible group from prior frame)
├── prev_threshold  : float                          (pixel threshold used last frame)
└── stale           : bool                           (true after pak hot-reload; forces full walk next frame)
```

`ClusterDAG` is a sibling aggregate of `MeshletGroup` in the §4.1
hierarchy (`SPEC.md` §4.1.4 / §4.1.5); they live in one record set
because their invariants are inseparable. The cook-time
`ClusterDAG` C++ class is constructed by `cook/cluster_dag.cpp`
(`SPEC.md` §6.1) and serialised into `MeshletPak` via the byte layout
in §7 below; the runtime form is a `const`-pointer view over the
mmap'd pak bytes — geometry never copies the DAG into engine-owned
memory.

`CutState` is **owned by render's per-view scratch arena** (`render`
SPEC §4.1.4), not by geometry. Geometry specifies its shape and
update recurrence so render's cull pass (#770) can implement it
without inventing a private data layout. The struct is POD; ABI
exposure is via `eastl::span<const std::byte>` reads only —
geometry's public surface (`SPEC.md` §5) does not name the type.

### 3.2 Band layout — CSR for cache-friendly traversal

Groups are stored band-major (`LOD0` indices contiguous, then `LOD1`,
…) with `band_offsets_[k]` recording the first index of band `k` and
`band_offsets_[8]` recording the past-the-end index. This is a
two-line CSR scheme:

```
band_offsets_ = [0, |LOD0|, |LOD0|+|LOD1|, …, total_groups]
```

Two consequences fall out:

1. **Iterating one band is one linear scan.** `groups_[band_offsets_[k]
   .. band_offsets_[k+1])` is contiguous; the cut algorithm (§3.6)
   touches at most one band's worth of records per descent step.
2. **Group index `g` → its band `k`** is a `O(log 8)` upper-bound
   binary search over `band_offsets_`; in practice we cache the band
   in `MeshletGroup::band` so the lookup is `O(1)`.

The CSR scheme also means parent / child edges are stored as flat
`uint32_t` arrays (`parent_edges_`, `child_edges_`) with per-group
offset arrays (`parent_csr_`, `child_csr_`). No node-side pointer
indirection; one edge load is a single cache line touch given typical
fan-out (`parent_count <= 4`, `child_count <= 4` in MVP).

### 3.3 `MeshletGroup` record

`MeshletGroup` is the entity record inside `ClusterDAG`. Its public
projection across the geometry plugin boundary is `MeshletGroupView`
(`SPEC.md` §5 line 1630) — a strictly narrower type that drops the
adjacency lists and watertight mask. The internal record carries
exactly the fields cut selection and watertight validation need:

- **`band`** (`LODBand` enum, `u8`) — the tier index (0 = finest).
  Cached so `O(1)` lookup; redundant with the CSR offset but worth
  the byte for branch-free band tests.
- **`meshlet_range`** — slice into the pak's per-meshlet table for
  this group's constituent clusters. Whole-group residency
  (`SPEC.md` §4.1.6 invariant 1) means a single page-state read
  decides the whole range.
- **`bounds`** — `BoundingSphere` enclosing every constituent
  `Meshlet`'s sphere. Group-level only; per-meshlet bounds live on
  `Meshlet`.
- **`screen_space_error`** — per-group projected error at the
  cook-time reference distance (`MeshletBuildOptions
  .screen_space_reference_distance`). Strictly tightens with finer
  band per §4.1.4 invariant 2; cut selection compares against
  view's pixel threshold.
- **`parent_count` / `child_count`** — degrees into the CSR edge
  arrays. `LOD0` groups have `parent_count >= 1` and `child_count
  == 0`; coarsest-band groups have `parent_count == 0`.
- **`watertight_mask`** — `u32` bit field with one bit per outgoing
  edge (parent + child); bit `i` = 1 means the corresponding edge
  may be crossed by a runtime cut without producing a T-junction.
  Cooker computes from the simplification-driven seam analysis;
  runtime asserts `(watertight_mask & cut_edge_mask) == cut_edge_mask`
  for any cut it selects (§3.5).
- **`material`** — `MaterialHandle` (opaque, `SPEC.md` §5 line
  ~1310). Populated for LOD0 groups only; coarser-band groups
  inherit by walking up the parent chain at cook time and stamping
  the resolved handle into the record. Runtime cut selection
  ignores the material — cull pass (#770) reads it post-selection.

The record is **POD**; no virtual methods, no `eastl::function`. It
serialises to/from pak bytes by `memcpy` plus byte-order fix-up if
the host is big-endian (`SPEC.md` §7.2 "little-endian throughout"
fixes the on-disk order). Sizeof is 64 bytes on M1 baseline (one
cache line); padding is explicit (`reserved` field) so cooker
emission and runtime read see the same layout.

### 3.4 DAG-shape invariants (re-stated as design contracts)

The four DAG-level invariants from `SPEC.md` §4.1.5 plus the four
group-level invariants from `SPEC.md` §4.1.4 are the cluster-DAG's
public-boundary contracts. This design re-states them as the things
the cook stage must produce and the things the runtime may rely on:

1. **Acyclic.** Topological sort over `(parent_edges_, child_edges_)`
   succeeds with no edge cycle. Cook-time validation walks band-major
   from coarsest to finest; finding a back-edge refuses cook with
   `geometry::Error::ClusterDAGCycle` (`SPEC.md` §10.7 row).
2. **Edges only between adjacent bands.** For every edge `(u, v)`
   with `band(u) > band(v)` (parent → child), `band(u) == band(v) + 1`.
   No skip-level edges. Cook validates by linear scan.
3. **Single LOD0 cover.** `lod0_cover_` is a complete partition of
   the source triangle set; refused on cook with
   `geometry::Error::LOD0CoverIncomplete` if the union of meshlet
   ranges in the cover does not equal the full triangle count.
4. **Coarsest band reachable from every leaf.** Walking parent
   edges from any LOD0 group reaches at least one coarsest-band
   group; orphan subgraphs are refused. Cook validates via
   reverse-BFS from the coarsest band.
5. **Per-group SSE strictly tightens with finer LOD.** For every
   parent edge `(u, v)`, `SSE(v) < SSE(u)` strictly. Equality
   refused with `geometry::Error::LODBandSseNonMonotonic`. This
   makes cut selection total: for any pixel threshold `T`, exactly
   one cut respects edges and yields the coarsest band with all
   `SSE ≤ T`.
6. **Watertight cut on group edges.** The cooker re-tessellates a
   sample of admissible cuts and rejects any DAG that produces a
   T-junction or double-coverage on any sampled cut. The
   `watertight_mask` records the cooker's findings; runtime
   asserts on the chosen cut (§3.5).
7. **LOD0 group set covers the source.** Same triangle-count
   equality as #3, restated at the group invariant level.
8. **Group bounds enclose constituents.** Per `SPEC.md` §4.1.4
   invariant 4; cooker computes the minimum sphere from the
   constituent meshlet spheres (Welzl's algorithm with deterministic
   tiebreak per PHILOSOPHY §7).

The runtime trusts (1)–(8) without re-verifying, because the
`FormatHash` gate (`SPEC.md` §7.3.2) and per-page CRC32 (§4.1.6
invariant 3) together guarantee the bytes have not been tampered
with post-cook. A malformed DAG that survives both gates is a
Byzantine fault, not a domain failure.

### 3.5 Watertight cut — definition and validation

A **cut** is a function `c: SourceTriangles → MeshletGroups` such
that:

1. Every source triangle maps to exactly one group's meshlet range.
2. For every pair of adjacent triangles sharing an edge in the
   source mesh, the two groups they map to are connected in the
   DAG by a chain of parent / child edges *all of whose
   `watertight_mask` bits are set on the chain edges*.

The cut is **band-coherent** if every group it selects is at the
same `LODBand`; **band-mixed** otherwise. Both are admissible if
they satisfy (1) and (2).

The runtime cut algorithm (§3.6) emits only band-coherent cuts at
MVP — the per-view threshold maps to exactly one band by §3.4 #5
monotonicity. Band-mixed cuts (e.g. silhouette-aware "fine on
silhouette edge, coarse interior") are an explicit post-MVP open
question (§12); the watertight-mask is sized for them so the format
admits the upgrade additively (§7.3.3).

**Runtime validation.** For the chosen cut `c`, geometry asserts
that for every pair `(g_i, g_j)` of groups in the cut connected by
an edge `e`, `(watertight_mask(g_i) & bit(e)) != 0`. This is
`O(group degree)` per cut, which on the MVP fan-out cap (`parent_count
<= 4`) is at most 4 bit tests per group. The assertion fires only
in debug builds; shipping builds trust the cooker (`SPEC.md` §4.1.4
invariant 1 "cooker validates by re-tessellating sample cuts").

### 3.6 Cut-selection algorithm — `select_lod_group`

`GeometryRegistry::select_lod_group(MeshHandle, pixel_threshold,
view_sphere)` (`SPEC.md` §5 line 1689) is the single runtime entry
that touches the DAG topology. It runs **per-view per-mesh per-frame**
inside phase 6 (`SPEC.md` §9.2 row `select_lod_band per visible
mesh`) and returns the `MeshletGroupHandle` for the coarsest band
whose every constituent group has `SSE ≤ pixel_threshold` and is
fully resident.

**Algorithm.** Given `mesh`, `T = pixel_threshold`, `V = view_sphere`:

1. **Resolve mesh handle.** `O(1)` lookup into the registry's
   handle table (`SPEC.md` §6.3.3, §4.1.14 invariant 3) returns the
   `ClusterDAG*` for the pak. Failure → `MeshHandleStale` (§10).
2. **Hot-path cut-state probe.** If the per-view `CutState` for
   `(view, mesh)` is non-stale and `prev_threshold == T` and the
   prior frame's selected group still has `fully_resident == true`
   and its band's residency mask is unchanged, return the cached
   `selected_group` immediately. This is the temporal-coherence
   fast path; on stable scenes it dominates frame-to-frame
   (§5.1 below). `O(1)` work.
3. **Coarsest-first descent.** Starting from any group at the
   coarsest band (`band_offsets_[8] - 1`, walking leftward across
   the band), evaluate the **admission predicate**:

   ```
   admissible(g) ⇔ SSE(g) ≤ T  ∧  fully_resident(g) == true
   ```

   The "fully resident" test reads the residency atomic for every
   page that contains a constituent meshlet; whole-group containment
   (`SPEC.md` §4.1.6 invariant 1) means this is one atomic load
   per group.
4. **Descend on inadmissibility.** If no group at band `k` is
   admissible, descend to band `k - 1` via child edges:

   ```
   candidates_{k-1} := ⋃ { children(g) : g ∈ candidates_k, ¬admissible(g) }
   ```

   Continue until either (a) every group at the current band is
   admissible — return any one of them (the coarsest admissible
   set); or (b) `k == 0` — return the LOD0 group covering the view
   sphere (always admissible because LOD0 SSE ≤ all coarser SSE
   and the LOD0 cover is by construction `fully_resident` once
   `register_mesh` completes if the registry's residency seed
   policy demands it; see §6 below).
5. **Pick representative.** The current MVP returns *one*
   `MeshletGroupHandle` per `(mesh, view)` because cull pass (#770)
   takes one handle per visible mesh and unrolls into per-meshlet
   records on the GPU. The handle returned is the coarsest
   admissible group whose `bounds` contains the view sphere's
   centre; ties broken lexicographically on `group_id` (PHILOSOPHY
   §7 deterministic tiebreak).
6. **Update `CutState`.** Write `selected_group`, `prev_threshold`,
   `frame_counter`. Render owns the buffer; geometry only pokes
   the bytes via the registry's lock-free read surface returning a
   POD update payload.
7. **Watertight assertion (debug only).** If `GLIBRE_GEOMETRY_DEBUG`,
   verify the selected group's `watertight_mask` admits a cut that
   includes it; on failure, log
   `geometry::Error::CutInvalid` and downgrade to LOD0 (§10).

**Termination.** The descent visits each band at most once and each
group at most once per descent. Worst-case work per call:
`O(group_count_in_visited_bands)`. On the §3.4 #5 monotonicity
contract, the typical S1 case visits 1–3 bands (`SPEC.md` §9.2 row
"~200 props × ~3-5 candidate bands"). Single-mesh cost on M1
firestorm: ≤ 1 µs steady-state, ≤ 3 µs cold (§9 below).

**Determinism.** No clock or PRNG read; result is a deterministic
function of `(ClusterDAG bytes, T, view_sphere, residency mask
snapshot)`. Two runs with the same inputs return byte-equal
`MeshletGroupHandle`s (PHILOSOPHY §7).

### 3.7 LOD0-cover invariant — BLAS handoff

The LOD0 cover (`lod0_cover_`) is the single cluster band the
`BLASRecipe` references (`SPEC.md` §4.2 invariant 3). Cluster-DAG
exposes the cover via the existing `GeometryRegistry::lod0_groups`
public method (`SPEC.md` §5 line 1697); no new surface needed.
BLAS-recipe authoring (#783) walks `lod0_cover_` and emits geometry
descriptors for each group's meshlet range; geometry never builds
a BLAS itself (§4.2 invariant 10).

### 3.8 Per-view CutState recurrence

`CutState` is updated by render's cull pass (#770) using the
recurrence `select_lod_group` returns. The struct is:

```cpp
struct CutState {
    std::uint64_t      frame_counter   = 0;
    MeshletGroupHandle selected_group{};
    float              prev_threshold  = 0.0f;
    bool               stale           = true;   // forced full walk on first frame
    // 7 bytes pad to 32 bytes total
};
```

The recurrence is:

```
CutState_{n+1} = select_lod_group(mesh, T_{n+1}, V_{n+1})
                 .with_frame(frame_n+1)
                 .with_threshold(T_{n+1})
                 .with_stale(false)
```

`stale` is forced `true` on three events: (a) initial allocation;
(b) pak hot-reload publishes `MeshReplaced` (`SPEC.md` §8.5
observer responsibility 1); (c) view-handle reuse across frames.
Each forces a full walk on the next call. `CutState` lives in
render's per-view scratch arena (`render` SPEC §4.1.4); geometry
specifies the layout but never allocates it.

Concurrency: `CutState` is accessed by exactly one thread at a
time (the cull-pass worker for `View` `v`). No atomics needed; the
allocation is `View`-local.

## 4. Public surface

This aggregate introduces **no new public types or functions
beyond `specs/geometry/SPEC.md` §5**. The existing seam is:

```cpp
// SPEC §5 — already published; cluster-DAG provides the implementation.
[[nodiscard]] Result<MeshletGroupHandle>
    GeometryRegistry::select_lod_group(MeshHandle     mesh,
                                       float          pixel_threshold,
                                       BoundingSphere view_sphere) const noexcept;

[[nodiscard]] Result<MeshletGroupView>
    GeometryRegistry::resolve_group(MeshletGroupHandle) const noexcept;

[[nodiscard]] Result<eastl::span<const MeshletGroupView>>
    GeometryRegistry::lod0_groups(MeshHandle) const noexcept;
```

Failure shape (per `error-model.md` and `SPEC.md` §10):

- `select_lod_group` returns `MeshHandleStale` if the mesh handle
  is stale (post-unregister, post-reload-without-rebind);
  `MeshletGroupHandleStale` if internal cut bookkeeping fails to
  resolve (Byzantine, debug-only);
  `CutInvalid` if the watertight assertion fires (debug-only;
  shipping builds trust the cooker per §3.4).
- `resolve_group` returns `MeshletGroupHandleStale` per §10 row.
- `lod0_groups` returns `MeshHandleStale` per §10 row; the LOD0
  cover slice is otherwise total.

`MeshletGroupView` is the only public projection of a DAG node; it
strips parent/child adjacency and the watertight mask (those are
internal). The view is POD with the §3.3 fields plus
`MeshletGroupHandle` and `meshlet_count` already published in
SPEC §5 line 1630–1639.

**No exception path.** `-fno-exceptions` per
`error-model.md` §"Decision" item 3; `select_lod_group` returns
`Result<MeshletGroupHandle>` and never throws across the geometry
plugin boundary (`plugin-abi.md` §"C ABI surface" forbids C++
exceptions across the dylib seam).

**No `eastl::` containers cross the ABI.** `lod0_groups` returns
`eastl::span<const MeshletGroupView>` which is a POD `(ptr,
length)` pair (PHILOSOPHY §11 final paragraph: "Public plugin ABI
surfaces never expose `std::` containers or `eastl::` containers —
they cross the boundary as POD spans / handles only"). The span
points into the mmap'd pak region; lifetime is the
`MeshHandle`'s registration window.

**Cut-state shape is internal.** `CutState` is render-owned
scratch (§3.8); its byte shape is named in this design but it does
not appear in `SPEC.md` §5. Render's cull pass declares its own
struct that is bit-equal; the equivalence is asserted at compile
time via `static_assert(sizeof(render::CutState) ==
sizeof(geometry::detail::CutState))` in render's tree, and at run
time via a unit test fixture that round-trips a `CutState` through
both names.

## 5. Hot/cold path split

### 5.1 Hot path — `select_lod_group` per (View, Mesh) per frame

Phase 6's cull-extract calls `select_lod_group` once per visible
mesh per view (`SPEC.md` §6.3.3). Per `perf-budget.md` (geometry
row, `SPEC.md` §9.2 step `select_lod_band per visible mesh`) the
**hot-path cap is 0.15 ms total across the entire S1 visible set**
(~200 props × ~3–5 candidate bands ≤ 1k SIMD-bound comparisons).

Operations on this path:

1. **Cut-state probe.** `O(1)` read of `CutState`; cached return on
   stable scene. Dominates frame-to-frame; M1 firestorm cost
   ≤ 50 ns per call.
2. **Cache-miss descent.** §3.6 algorithm; cost is band-major
   linear scans plus per-group residency-atomic loads. M1 cost
   ≤ 1 µs per mesh on first frame; ≤ 0.3 µs on threshold-stable
   subsequent frames where the residency snapshot has changed.
3. **`MeshletGroupView` projection.** `resolve_group` is `O(1)` —
   one struct copy from the mmap'd record into the caller's
   `MeshletGroupView`. ≤ 30 ns.

**Zero allocations** on this path — `CutState` is render-owned
scratch, the DAG is a `const`-pointer view over mmap, and the
descent uses two stack-allocated `eastl::fixed_vector<uint32_t,
64>` buffers (one for the current band's candidate set, one for
the next band's). Fixed inline storage means no heap touch.

### 5.2 Cold path — DAG construction (cook stage 4)

`cook/cluster_dag.cpp` (`SPEC.md` §6.2 step 4) builds the DAG from
the meshlet records of every LOD level produced by stage 3 and
emits the §3.1 byte layout into a pak buffer. This runs **once per
mesh per cook**, off the engine entirely (host-only cook driver
`tools/glibre-meshcc`, `SPEC.md` §6.5). Cost is `O(meshlets *
log(meshlets))` for the simplification-driven hierarchy build;
target time per mesh on M1 host is ≤ 200 ms for the MVP S1
character mesh per the `perf-budget.md` cook-driver budget.

The cook path is allowed to allocate freely from the
`ContextTag::cook` arena (`perf-budget.md` Allocator Rules item 4
— transient arena exemption); it never touches engine memory.

### 5.3 Why the split matters

Hot-path drift trips `SPEC.md` §9.2 cap and ultimately the
`max(cpu_sim+cpu_submit, gpu) <= 15.17 ms` frame invariant
(`perf-budget.md` §"Decision"). Cold-path drift trips the cook
driver's per-mesh deadline only and is a build-farm issue, not a
runtime issue. The split is deliberately not data-driven: the cut
algorithm is hard-coded, the temporal-coherence cache is
view-local POD, and the descent visits at most 8 bands (the
`LODBand` enum is closed at 8 entries per `SPEC.md` §5 line 1416).
The only `std::atomic` on the hot path is the residency-state
load; shipping builds elide the watertight-debug assertion.

## 6. Concurrency

### 6.1 Cold side — cook-driver thread (one)

DAG construction is single-threaded per mesh (`SPEC.md` §6.4 cook
side). The build farm parallelises across meshes by spawning one
`glibre-meshcc` process per worker; intra-mesh DAG build is one
thread because meshoptimizer's simplification driver is itself
single-threaded and the DAG topology is `O(meshlet)` work — the
parallelism win is dominated by the inter-process axis.

### 6.2 Hot side — per-view cull worker (≤ N views)

`select_lod_group` is called from render's cull-extract phase 6
worker (`render` SPEC §6.3 cull). The number of cull workers is
bounded by the number of active views (split-screen: ≤ 4; single-
view: 1; shadow cascades: up to 4 directional + per-light face
counts). Each worker calls `select_lod_group` for its view's
visible meshes serially — no nested parallelism inside the call.

**Read-only contention.** All DAG reads are `const` against the
mmap'd region; concurrent readers from different cull workers see
byte-identical bytes with zero contention. The residency atomic
loaded inside admission tests (`SPEC.md` §4.1.13 invariant 2) is
a relaxed-load; concurrent `Pending → Resident` writes in phase 7
do not race because phase 6 reads execute before phase 7 writes
(`frame-phases.md` ordering).

**No mutex on the hot path.** Per `SPEC.md` §6.4: "No mutex is
acquired on the render hot path." `CutState` is per-(View, Mesh),
allocated in render's per-view arena, single-writer; no
synchronisation.

### 6.3 Determinism

Per PHILOSOPHY §7, the DAG aggregate is deterministic in two
distinct senses:

1. **Cook-time byte-equal.** Two cooks of the same `MeshSource`
   produce byte-equal cluster-DAG bytes inside their respective
   paks (`SPEC.md` §4.1.7 invariant 2). The build farm relies on
   this for incremental-skip across hosts.
2. **Runtime selection determinism.** Given byte-equal DAG bytes,
   byte-equal `pixel_threshold`, byte-equal `view_sphere`, and a
   byte-equal residency snapshot, `select_lod_group` returns the
   same `MeshletGroupHandle` on every host. Tiebreak in §3.6 step
   5 is lexicographic on `group_id`; `group_id` is part of the
   cooked bytes (§7.1) so it is byte-equal too.

The fixture test
`select-lod-group-byte-equal-on-multiple-hosts` (§11) is the
contract test; it runs in CI on the M1-baseline matrix and on the
e2e M1 host.

### 6.4 No re-entrancy

`select_lod_group` does not call back into `GeometryRegistry`
(other than through the residency-atomic load, which is not a
re-entrant entry). It does not call into `render`, `content`, or
any other plugin. Re-entrancy is forbidden by the §6.4 §SPEC
"single-writer phase-7 mutation point" — phase 6 reads only.

## 7. Persistence + ABI

The cluster-DAG aggregate persists **inside `MeshletPak`** at the
file region documented in `SPEC.md` §7.2.1 (`cluster_dag_offset` /
`cluster_dag_length` in `PakHeader`). It is **not Fory-serialised**
(per `SPEC.md` §7.2.4 — *"the envelope rides Fory; the opaque
large binary does not"*); the bytes are mmap'd and consumed
directly by `PakReader`.

### 7.1 On-disk byte layout

The cluster-DAG region is laid out in a fixed canonical order so
`FormatHash` (`SPEC.md` §7.2.3) can hash the layout string
`cluster_dag_layout_canonical` deterministically. All offsets are
relative to `cluster_dag_offset`; little-endian throughout per
`SPEC.md` §7.2.1.

```text
Offset  Size                     Field                      Notes
------  ----                     -----                      -----
0x0000  4                        group_count                u32 — total groups across all bands
0x0004  4                        edge_count_parent          u32 — total parent-edge entries
0x0008  4                        edge_count_child           u32 — total child-edge entries
0x000C  4                        lod0_cover_count           u32 — entries in LOD0 cover
0x0010  9 * 4                    band_offsets[9]            u32 each — CSR band boundaries
0x0034  group_count * 64         groups[]                   MeshletGroupRecord (§7.1.1)
   ...  (group_count + 1) * 4    parent_csr[]               u32 each — offset into parent_edges
   ...  edge_count_parent * 4    parent_edges[]             u32 each — group index
   ...  (group_count + 1) * 4    child_csr[]                u32 each — offset into child_edges
   ...  edge_count_child * 4     child_edges[]              u32 each — group index
   ...  lod0_cover_count * 4     lod0_cover[]               u32 each — group index
   ...  group_count * 8          group_ids[]                u64 each — stable id (§7.1.2)
   ...  pad to 16-byte alignment reserved                   zero bytes; ends region
```

`cluster_dag_length` (in `PakHeader`) equals the total bytes from
offset 0 through the trailing pad. `PakReader` validates
`cluster_dag_offset + cluster_dag_length <= mmap_size` at header
step 4 (`SPEC.md` §7.2.2 reader-side validation order); it does
*not* re-validate the internal structure — `FormatHash` is the
gate.

#### 7.1.1 `MeshletGroupRecord` — 64-byte fixed record

Each entry of `groups[]` is a 64-byte record matching the §3.3
runtime form one-to-one. Field order:

```text
Offset  Size  Field                   Notes
------  ----  -----                   -----
0x00    1     band                    u8 — LODBand enumerator
0x01    1     parent_count            u8 — degree, 0..255
0x02    1     child_count             u8 — degree, 0..255
0x03    1     reserved_align0         u8 = 0
0x04    4     meshlet_first           u32 — index into pak meshlet table
0x08    4     meshlet_count           u32 — count
0x0C    4     watertight_mask         u32 — per-edge bit field
0x10    16    bounds                  BoundingSphere (4 * f32)
0x20    4     screen_space_error      f32
0x24    4     material                MaterialHandle (u32 raw)
0x28    24    reserved_tail           zero bytes; reserves additive fields per §7.3 of SPEC
```

Sizeof = 64 bytes (one cache line on M1 baseline). The
`reserved_tail` slot is for additive growth per `SPEC.md` §7.3.3
("Layout changes are additive only at the field level"); a future
field appending here does not break older paks because every pak
schema version still bumps `FormatHash` and forces re-cook
(§7.3.2 of SPEC).

#### 7.1.2 `group_id` — stable 64-bit identifier

Each group carries a `u64 group_id` recorded at `group_ids[]`
position equal to its group index. `group_id` is stable across
re-cooks of the *same source* — it is computed from the source
triangle set comprising the group (deterministic hash over sorted
triangle indices, blake3 truncated to 64 bits, byte-equal across
hosts per PHILOSOPHY §7). Two re-cooks of the same `MeshSource`
produce identical `group_id`s for groups whose triangle membership
is unchanged.

`group_id` is the **migration key** (`SPEC.md` §8.4 step 3): when
a pak hot-reload changes DAG topology, geometry walks the new DAG
to find a group with matching `group_id` and re-binds the surviving
`MeshletGroupHandle` to it. No-match falls back to the LOD0 cover
with a one-frame downgrade (§8 below; `SPEC.md` §8.5 observer
responsibility 1).

### 7.2 No Fory schema for the DAG

DAG bytes are inside the pak's binary region, which `SPEC.md`
§7.2.4 forbids from Fory serialisation for three reasons (mmap
residency, byte-equal determinism, no silent additive drift). The
twin record `glibre.geometry.PakHeaderRecord` (`SPEC.md` §7.2.2)
exposes `cluster_dag_offset` and `cluster_dag_length` for tooling
but **not** the DAG body — tools wanting to inspect the DAG must
link `geometry/src/pak/` and read the bytes directly.

### 7.3 ABI consequences

- The §4 public surface (`select_lod_group`, `resolve_group`,
  `lod0_groups`) is the entire ABI seam this aggregate
  contributes. None of its types appear in `glibre-types.dylib`
  (`fory-codegen.md`); the middleman ABI hash is unaffected by
  cluster-DAG changes.
- The on-disk byte layout (§7.1) is part of `FormatHash`'s input
  string `cluster_dag_layout_canonical` (`SPEC.md` §7.2.3 input
  list). Any change to §7.1 — field reorder, field add, record
  size change — bumps `FormatHash` and forces re-cook of every
  pak (`SPEC.md` §7.3.2 "invalidate, never migrate").
- Adding an enumerator to `LODBand` (currently 0..7, closed
  per `SPEC.md` §5 line 1416) bumps `FormatHash` because the
  enum table is folded in (`SPEC.md` §7.2.3 input list closed-sum
  enums). MVP freezes the band count at 8.

The DAG aggregate consumes no Fory types; therefore no migration
bodies are required (`fory-codegen.md` §"Migration Mechanic" is
not exercised here).

## 8. Hot-reload

The cluster-DAG aggregate participates in pak hot-reload (`SPEC.md`
§8.1 trigger: `.glibre-pak` content-hash change). The plugin-dylib
hot-reload path (drain → swap → migrate → resume per
`hot-reload-protocol.md`) follows the engine-wide protocol; the
DAG aggregate's contribution is fully covered by `SPEC.md` §8.4
migrate body. This section restates the DAG-specific points.

### 8.1 What the DAG aggregate drops on swap

Everything mmap-backed by the outgoing pak. Per `SPEC.md` §8.4
step 2 ("unmap outgoing pak"):

- The `const`-pointer view over the outgoing pak's cluster-DAG
  region is invalidated when the pak is munmap'd.
- Per-`MeshletGroupHandle` resolution against the outgoing
  `groups_` table is no longer valid.
- The `lod0_cover_` index list resolves into an unmapped region
  and must be re-read from the new pak.

### 8.2 What survives, by reference

- **`MeshHandle` bit-identity.** Per `SPEC.md` §8.4 step 1 +
  invariant: `MeshHandle::raw()` is preserved across a pak swap.
  The slot+generation pair is stamped onto the new pak's table
  without bumping the generation.
- **`MeshletGroupHandle` bit-identity.** Same rule, harder
  contract: per `SPEC.md` §8.4 step 3, the migrate body re-binds
  every surviving `MeshletGroupHandle` to a group in the new DAG
  via `group_id` matching (§7.1.2 above). A handle whose
  `group_id` is no longer present in the new DAG enters a
  one-frame downgrade window — `select_lod_group` returns the
  LOD0-cover representative for that mesh and `MeshReplaced`
  fires so render's cull pass invalidates its `CutState` for
  every view holding this mesh.
- **`CutState`** (render-owned). The migrate body sets
  `stale = true` for every `CutState` referencing this mesh
  (`SPEC.md` §8.5 observer responsibility 1). The next frame's
  `select_lod_group` starts from a full walk — temporal-coherence
  cache is rebuilt in one frame.

### 8.3 Refusal cases

`SPEC.md` §8.2 enumerates the pak hot-reload refusal gates;
DAG-relevant ones:

- `Error::PakFormatHashMismatch` — new pak schema disagrees with
  engine; refuse swap, keep outgoing pak. `FormatHash` hash chain
  includes `cluster_dag_layout_canonical`, so any DAG byte-layout
  change is caught here.
- `Error::PakPageIntegrityFailed` — per-page CRC32 fails on the
  new pak; refuse swap. (DAG region is part of the pak header
  area, not a `PakPage`, so this gate fires for pages, not the
  DAG bytes; the DAG region is gated by `FormatHash` only.)
- `Error::ClusterDAGCycle` / `LODBandSseNonMonotonic` /
  `LOD0CoverIncomplete` — these are *cook-time* refusals; a pak
  shipped from a build farm cannot reach the runtime carrying
  them. If a malformed pak somehow survives both `FormatHash` and
  CRC32 gates, the runtime trusts the bytes (§3.4 last paragraph;
  Byzantine, debug builds detect via watertight assertion).

### 8.4 Editor live-reload of a single mesh

Editor "re-cook one mesh" (`SPEC.md` §8.6 E2E hook) feeds the same
pak hot-reload path. The DAG aggregate sees the same swap
sequence; no special editor case is required.

## 9. Performance

### 9.1 Hot-path cost (per mesh per view per frame)

Per `perf-budget.md` (geometry row: 0.30 ms sim + 0.20 ms submit
on driver thread, 256 MiB heap) and `SPEC.md` §9.2 (sub-budget
for `select_lod_band per visible mesh` = 0.15 ms total across
visible set):

| Step                                                           | Cap (per call) | Cap (S1 total)    |
|----------------------------------------------------------------|----------------|-------------------|
| Cut-state probe (cache hit)                                    | 50 ns          | 10 µs (200 props) |
| Cache miss — coarsest-first descent + admission tests          | 1 µs           | 200 µs            |
| `MeshletGroupView` projection (`resolve_group`)                | 30 ns          | 6 µs              |
| `lod0_groups` cover walk (BLAS-recipe handoff, phase 7 only)   | 100 ns / group | 50 µs (LOD0 set)  |
| **Subtotal phase 6 contribution**                              |                | **≤ 0.15 ms**     |

The 0.15 ms cap matches `SPEC.md` §9.2 row exactly. The subtotal is
within the geometry phase-6 slice (0.30 ms total; remainder spent
on residency / handle table reads in §9.2 other rows).

### 9.2 Cold-path cost (cook stage 4, off-engine)

`cook/cluster_dag.cpp` builds the DAG from stage 3's per-LOD
meshlet records. Per-mesh time on M1 host:

| Mesh size                         | Cap        |
|-----------------------------------|------------|
| MVP S1 character (~50k tris)      | ≤ 200 ms   |
| MVP S1 prop (~5k tris)            | ≤ 30 ms    |
| MVP S1 environment chunk (~200k tris) | ≤ 800 ms |

These are cook-driver times, not engine times; they do not feed
`perf-budget.md`'s 16.67 ms frame ceiling. The cook driver's
per-mesh budget feeds the build-farm SLA only.

### 9.3 Memory ceilings

The DAG region inside one pak is sized as follows (S1 character
upper bound):

```
group_count    ≤ 1024     (8 bands × ≤ 128 groups/band)
edge_count_*   ≤ 4 * group_count = 4096
lod0_cover     ≤ group_count
group_ids      = group_count

Region bytes   = 0x34 header + group_count*64 + 2*(group_count+1)*4
                 + 2*4096*4 + group_count*4 + group_count*8 + pad
               = 56 + 64K + 8K + 32K + 4K + 8K + pad
               ≤ 128 KiB per mesh.
```

`MeshletPak` is mmap'd, so the 128 KiB does not count against the
geometry context's 256 MiB heap ceiling (mmap'd bytes are
file-backed, accounted by the OS, not by the per-context allocator
per `perf-budget.md` Allocator Rules item 1).

`CutState` size: `sizeof(CutState) = 32 bytes`. Per-view per-mesh:
S1's ~200 visible meshes × 1 view × 32 bytes = 6.4 KiB per view.
This lives in render's per-view scratch arena (`render` SPEC
§4.1.4); the 256 MiB render heap (`perf-budget.md` render row)
absorbs it trivially.

### 9.4 Cited acceptance benchmarks

Per `SPEC.md` §11 row 11.5 ("LOD band selection"), story #484
asserts the 0.15 ms cap via a Catch2 `BENCHMARK` block named
`select_lod_band coarsest-resident with 0.15 ms cap`. This design
adds two more benchmark targets (§11 below): `cut-selection-cold-
walk` and `cut-selection-temporal-cache-hit`.

### 9.5 Cross-references

- `perf-budget.md` geometry row — the 0.30 ms phase-6 ceiling.
- `SPEC.md` §9.2 — the row this design refines.
- `frame-phases.md` row 6 — phase 6 owner is render; geometry's
  participation is read-only.

## 10. Failure modes

The DAG aggregate's failure surface flows through `geometry::Error`
per `error-model.md` §"Decision" item 2 and `SPEC.md` §10. This
section enumerates the DAG-specific arms. None are new; all are
already in `SPEC.md` §10.7 (cook-time roster) or §10.2 (runtime
roster), restated here against the DAG-specific triggers.

| Error arm                          | Surface (cook / runtime) | Trigger                                                                                                                               | Recovery                                                                                                                                |
|------------------------------------|--------------------------|---------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------|
| `ClusterDAGCycle`                  | Cook-time (§10.7)         | Topological sort over `(parent_edges_, child_edges_)` finds a back-edge.                                                              | Cook refuses pak emission (`tools/glibre-meshcc` exits with code 11 per §10.7.6 row); no in-process recovery — re-author or fix cooker bug. |
| `LODBandSseNonMonotonic`           | Cook-time (§10.7)         | `SSE(child) >= SSE(parent)` for any parent edge.                                                                                      | Cook refuses; same as above. May indicate meshoptimizer simplification policy bug; defer to cook ticket #781.                          |
| `LOD0CoverIncomplete`              | Cook-time (§10.7)         | LOD0 cover's union of meshlet ranges does not equal the source triangle count.                                                        | Cook refuses; same as above.                                                                                                            |
| `MeshHandleStale`                  | Runtime (§10.2)           | `select_lod_group` / `resolve_group` / `lod0_groups` called with a handle whose generation no longer matches the registry.            | Caller (render) drops the handle, refetches via `register_mesh` or treats the mesh as not-registered. Logged at `info` (not `warn`).   |
| `MeshletGroupHandleStale`          | Runtime (§10.2)           | `resolve_group` called with a group handle whose owning `MeshHandle` is no longer registered, or whose `group_id` did not survive a hot-reload re-bind. | One-frame downgrade to LOD0 cover representative; `MeshReplaced` event already fired by hot-reload pipeline; render rebuilds CutState. |
| `CutInvalid`                       | Runtime (§10.2, debug-only) | Watertight assertion in §3.6 step 7 fires: chosen cut violates `watertight_mask`.                                                     | Debug build only; logs the offending mesh + group + edge, falls back to LOD0 cover for this frame, and the test fixture fails. Shipping builds skip the assertion (cooker is the ground truth). |
| `ErrorMetricNaN` *(spike brief)*   | Cook-time → routes to `LODBandSseNonMonotonic` | `screen_space_error` is `NaN` or negative for any group at cook stage 4.                                                              | Cook refuses with `LODBandSseNonMonotonic` (NaN fails the strict `<` test). Open: §12 below considers a dedicated arm.                  |
| `DAGCorrupt` *(spike brief)*       | Runtime → routes to `PakFormatHashMismatch` (load) or `MeshletGroupHandleStale` (post-load) | Bytes inside the DAG region resolve to a structure that violates §3.4 invariants on read.                                            | The `FormatHash` gate (§7.3) catches schema-level corruption before any read; per-byte tampering survives only as Byzantine, surfaced by debug-build watertight assertion → `CutInvalid`. |

The spike brief mentions three placeholder arms (`DAGCorrupt`,
`CutInvalid`, `ErrorMetricNaN`); only `CutInvalid` becomes a
distinct arm. `DAGCorrupt` collapses into `PakFormatHashMismatch`
(`SPEC.md` §10.2) at load time and into `MeshletGroupHandleStale`
post-load; `ErrorMetricNaN` collapses into
`LODBandSseNonMonotonic` (the cook-time monotonicity test catches
NaN as a strict-`<` failure). The collapse decisions are in §3.2
of `SPEC.md` (occam's razor; one error arm per refusal gate).

**No exception path.** Every failure surfaces via
`std::expected<T, glibre::Error>` per `error-model.md` §"Decision"
item 1. `select_lod_group` is `noexcept` and returns `Result<...>`.
`-fno-exceptions` per item 3.

**No partial state.** A failed `select_lod_group` returns the
error and writes nothing into `CutState`; the prior frame's state
remains valid.

## 11. Test plan

Per `AGENTS.md` "Tests by type": `type:plan` issues for this
design carry **unit tests (Catch2)**; the user-stories cited in
`SPEC.md` §11 carry the manual + E2E coverage.

### 11.1 Unit tests — DAG construction (cook side)

- `cluster-dag-builds-from-fixture-mesh-source`: feed a synthetic
  4-band, 16-group `MeshSource`; assert `group_count`, band
  offsets, parent / child edges match the golden record.
- `cluster-dag-rejects-cycle`: hand-craft an in-memory DAG with a
  back-edge; assert `cook::cluster_dag::build` returns
  `geometry::Error::ClusterDAGCycle`.
- `cluster-dag-rejects-skip-level-edge`: parent-of-LOD0 = LOD2
  (skip-level); assert refusal (logged as cycle by the topo
  walker; or as a dedicated layout error per §3.4 #2 — the
  cooker's choice; this test pins the behaviour).
- `cluster-dag-rejects-non-monotonic-sse`: parent SSE = child SSE;
  assert `LODBandSseNonMonotonic`.
- `cluster-dag-rejects-incomplete-lod0-cover`: LOD0 missing one
  source triangle; assert `LOD0CoverIncomplete`.
- `cluster-dag-rejects-orphan-subgraph`: an LOD0 group with no
  path to the coarsest band; assert refusal.
- `cluster-dag-rejects-degenerate-bounds`: a group whose sphere
  does not enclose every constituent meshlet sphere; assert
  refusal (`SPEC.md` §10.7 — flows under `MeshletBoundsInvalid`,
  re-routed at this aggregate to whichever cook-time arm cooker
  chooses; pinned by this test).
- `cluster-dag-byte-equal-across-hosts`: cook the same
  `MeshSource` on two host configurations (M1 + M1 with different
  thread pinning); assert `cluster_dag` region of the two paks is
  byte-equal.

### 11.2 Unit tests — Cut-selection algorithm

- `select-lod-group-coarsest-admissible`: synthetic DAG, fully
  resident, varying `pixel_threshold`; assert returned handle
  resolves to the coarsest band whose every group has `SSE ≤ T`.
- `select-lod-group-residency-filter`: same DAG, all coarse-band
  pages `NotResident`; assert the descent skips coarser bands and
  returns LOD0.
- `select-lod-group-pixel-threshold-zero`: `T = 0`; assert returns
  LOD0 (only band whose every group has `SSE ≤ 0`, with strict
  monotonicity making LOD0 SSE = 0 the only feasible band).
- `select-lod-group-pixel-threshold-infinity`: `T = +∞`; assert
  returns coarsest band's representative.
- `select-lod-group-cut-state-cache-hit`: call twice with same
  inputs; assert second call hits cut-state probe (verified via
  a perf counter increment fixture).
- `select-lod-group-cut-state-stale-on-mesh-replaced`: simulate
  `MeshReplaced` event; assert next call performs full descent
  (cut-state probe miss).
- `select-lod-group-deterministic-tiebreak`: a synthetic DAG
  where two coarsest-admissible groups have the same SSE; assert
  the returned handle is the one with the lexicographically
  smaller `group_id`.
- `select-lod-group-mesh-handle-stale`: pass a handle whose
  generation is 1 less than the registry's; assert
  `MeshHandleStale`.
- `select-lod-group-byte-equal-on-multiple-hosts`: run the same
  fixture on two hosts; assert returned `MeshletGroupHandle::raw()`
  is byte-equal (PHILOSOPHY §7).

### 11.3 Unit tests — Watertight invariant

- `cut-respects-watertight-mask`: a synthetic DAG whose
  `watertight_mask` bits are set on all parent edges; assert
  every cut emitted by `select_lod_group` passes the §3.5
  validation predicate.
- `cut-debug-watertight-violation-fires`: a synthetic DAG with a
  manually-zeroed `watertight_mask` bit on a known cut edge;
  debug build asserts `CutInvalid`; shipping build (compiled
  without the assertion) returns the bad cut and the test
  detects the missing assertion at compile time via
  `static_assert(GLIBRE_GEOMETRY_DEBUG)` guard. (Two distinct
  test binaries.)

### 11.4 Unit tests — `MeshletGroupView` projection

- `resolve-group-projects-required-fields-only`: assert the
  projection drops adjacency lists and the watertight mask
  (verified by sizeof / field-count introspection on the
  internal record vs the public view).
- `resolve-group-stale-handle-error`: stale group handle ⇒
  `MeshletGroupHandleStale`.
- `lod0-groups-returns-cover-only`: assert the span's element
  count equals `lod0_cover_count` and every element has
  `band == LODBand::LOD0`.

### 11.5 Unit tests — Hot-reload re-bind

- `pak-swap-rebinds-via-group-id`: cook two paks with same
  `MeshSource` but different DAG layouts (different
  simplification seed); register pak A, get a
  `MeshletGroupHandle`, hot-reload to pak B; assert the handle
  resolves to a group with the same `group_id`.
- `pak-swap-no-match-falls-back-to-lod0`: cook pak B with a new
  group whose `group_id` is fresh; pre-swap handle has a
  `group_id` that does not appear in B; assert next
  `select_lod_group` returns LOD0-cover representative for one
  frame and `MeshReplaced` is observed.
- `pak-swap-cut-state-marked-stale`: register a fresh
  `CutState`, call `select_lod_group` once, then trigger a pak
  swap; assert the next call performs a full descent.

### 11.6 Integration tests — real cooked DAG, golden cut result

- `golden-cut-character-mesh-camera-A`: cook the S1 character
  fixture; place a fixture camera at distance D; assert
  `select_lod_group` returns the golden `MeshletGroupHandle::raw()`.
- `golden-cut-character-mesh-camera-zoomed-out`: same fixture,
  camera at 4×D; assert a coarser-band handle.
- `golden-cut-environment-chunk`: the S1 environment-chunk
  fixture under three camera angles; assert three golden
  handles.
- `cut-selection-cold-walk`: Catch2 `BENCHMARK` block; cap
  `1 µs / call` on M1 baseline (§9.1).
- `cut-selection-temporal-cache-hit`: Catch2 `BENCHMARK` block;
  cap `50 ns / call` on M1 baseline (§9.1).
- `select-lod-band-coarsest-resident-with-0.15ms-cap`: Catch2
  `BENCHMARK` over the S1 visible set; this is the test
  `SPEC.md` §11 row 11.5 / story #484 already names. This
  design adds it as a dependency assertion against the §3.6
  algorithm.

### 11.7 Coverage matrix

Every `SPEC.md` §4.1.4 / §4.1.5 invariant + §4.2 invariant 2 +
§5 surface row + §10 error arm + §11 acceptance row above maps to
at least one test in §11.1–§11.6. The mapping is:

| Spec invariant / error / story         | Test name                                                |
|----------------------------------------|----------------------------------------------------------|
| §4.1.5 inv 1 (acyclic)                 | `cluster-dag-rejects-cycle`                              |
| §4.1.5 inv 2 (adjacent-band edges)     | `cluster-dag-rejects-skip-level-edge`                    |
| §4.1.5 inv 3 (single LOD0 cover)       | `cluster-dag-rejects-incomplete-lod0-cover`              |
| §4.1.5 inv 4 (coarsest reachable)      | `cluster-dag-rejects-orphan-subgraph`                    |
| §4.1.4 inv 1 (watertight cut)          | `cut-respects-watertight-mask`                           |
| §4.1.4 inv 2 (SSE monotonicity)        | `cluster-dag-rejects-non-monotonic-sse`                  |
| §4.1.4 inv 3 (LOD0 covers source)      | `cluster-dag-rejects-incomplete-lod0-cover` (shared)     |
| §4.1.4 inv 4 (group bounds enclose)    | `cluster-dag-rejects-degenerate-bounds`                  |
| §4.2 inv 2 (DAG acyclic + monotonic)   | both above                                               |
| §5 `select_lod_group`                  | `select-lod-group-*` (eight tests)                       |
| §5 `resolve_group`                     | `resolve-group-*` (two tests)                            |
| §5 `lod0_groups`                       | `lod0-groups-returns-cover-only`                         |
| §10 `ClusterDAGCycle`                  | `cluster-dag-rejects-cycle`                              |
| §10 `LODBandSseNonMonotonic`           | `cluster-dag-rejects-non-monotonic-sse`                  |
| §10 `LOD0CoverIncomplete`              | `cluster-dag-rejects-incomplete-lod0-cover`              |
| §10 `MeshHandleStale`                  | `select-lod-group-mesh-handle-stale`                     |
| §10 `MeshletGroupHandleStale`          | `resolve-group-stale-handle-error`                       |
| §10 `CutInvalid`                       | `cut-debug-watertight-violation-fires`                   |
| Story #484                             | `select-lod-band-coarsest-resident-with-0.15ms-cap`      |
| Story #487 (handle bit-identity)       | `pak-swap-rebinds-via-group-id`                          |
| Story #488 (DAG-topology-changing pak swap) | `pak-swap-no-match-falls-back-to-lod0`              |
| Determinism (PHILOSOPHY §7)            | `cluster-dag-byte-equal-across-hosts`, `select-lod-group-byte-equal-on-multiple-hosts` |

## 12. Open questions

- [OPEN] **Band-mixed cuts for silhouette-aware LOD.** §3.5
  defines a watertight cut that admits both band-coherent and
  band-mixed shapes; §3.6 emits only band-coherent at MVP because
  the cull pass (#770) consumes one handle per (mesh, view).
  Post-MVP, render's cull pass may want a *cut tree* of handles
  (fine on silhouette edge, coarse interior). Owner: render cull
  ticket #770 follow-up. Resolution gate: when a story opens for
  silhouette-aware LOD; no MVP work.

- [OPEN] **Hierarchical bounding-cone aggregation.** §3.3 records
  a per-group `BoundingSphere` only; per-meshlet `BoundingCone`
  is on `Meshlet` and feeds cull at meshlet granularity. A
  per-group cone could amortise back-face cull but adds an
  invariant load (the degenerate-cone sentinel) and an additive
  field. Owner: render cull ticket #770. Resolution gate:
  cull-pass benchmark shows back-face cull is hot; until then,
  refused.

- [OPEN] **`group_id` collision-resistance under adversarial
  authoring.** §7.1.2 truncates blake3 to 64 bits; the migration
  key is stable across honest re-cooks but a blake3-64 collision
  is feasible against an adversarial mesh (different triangle set
  → same hash). The hot-reload migrate body would then re-bind
  to a wrong group. Probability is `2^-32` per re-cook on random
  inputs and effectively zero in practice. Resolution gate:
  open if the editor exposes mesh-import to user-authored
  binary-equivalent files; until then, accepted.

- [OPEN] **Dedicated `ErrorMetricNaN` arm.** The spike brief
  flagged `ErrorMetricNaN` as a candidate; §10 collapses it into
  `LODBandSseNonMonotonic` because the strict-`<` test catches
  NaN. If a future cook stage produces NaN at a stage *before*
  monotonicity is checked, the collapse fails. Owner: cook
  ticket #781. Resolution gate: cook stage 4 review.

- [OPEN] **`CutState` ABI ownership.** §3.8 places `CutState` in
  render's per-view scratch arena, with a compile-time
  `static_assert` on size equivalence. An alternative is to host
  `CutState` in geometry (publishing a POD type in §5). Geometry
  hosting would force every `select_lod_group` caller to allocate
  a `CutState` per (View, Mesh) pair; render hosting keeps the
  ABI surface narrower. Resolution gate: render cull ticket
  #770's view-scratch design lands.

- [OPEN] **Per-mesh band count below 8.** `LODBand` is closed at
  8 entries (`SPEC.md` §5 line 1416); a small mesh whose finest
  triangle count fits in one meshlet has `band_offsets_[1] ==
  band_offsets_[2] == … == band_offsets_[8]`. The format admits
  this (the CSR offsets just collapse) but the cook stage may
  emit a degenerate DAG with one group per band. Owner: cook
  ticket #781. Resolution gate: cook-stage review confirms the
  small-mesh path collapses gracefully, otherwise we add a
  format-level "band count override" field (which would bump
  `FormatHash` and affect every pak).
