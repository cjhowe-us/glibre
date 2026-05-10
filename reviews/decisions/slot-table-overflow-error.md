# Decision Record — Slot-Table Capacity-Overflow Error Variant

## Status

Accepted (spike #904, parent epic #83 render).
Resolves the `Out-of-scope follow-ups` item 1 in
`reviews/decisions/resourceresidency-srp.md` ("§11.1 unit test 'slot
table — capacity overflow returns `ResourceResidencyExceeded`'") and
the corresponding `render-resources-design.md` §10.1 self-note.
Sibling to spike #874 (`reviews/decisions/resourceresidency-srp.md`).

## Context

`render-resources-design.md` §11.1 names a Catch2 test:

> `slot table — capacity overflow returns ResourceResidencyExceeded`
> | Exhaust `cap`; next `alloc` returns the typed error; no slot inserted.

The test name asserts that a generic `SlotTable<T, Tag>` (the
generation-counted entity table backing every public handle in the
resources aggregate per `render-resources-design.md` §3.1, §3.4) emits
`render::Error::ResourceResidencyExceeded` when its pre-sized capacity
is exhausted.

Two adjacent contracts disagree with that assertion:

1. `render-resources-design.md` §10.1's construction-site list — the
   single-construction-site SRP rule installed by spike #874 — does
   **not** name `SlotTable::alloc` as a `ResourceResidencyExceeded`
   construction site. The only site listed for that variant is
   `resources/transient_pool.cpp::TransientPool::peak_residency_check`
   (per-frame total residency > 512 MiB).
2. `specs/render/SPEC.md` §10.3's `ResourceResidencyExceeded` row binds
   the variant to "Phase 6 plan computes a peak-residency footprint
   > 512 MiB ceiling (§9.5)." Slot-table capacity is not the
   per-frame-bytes ceiling; it is a per-table, init-sized count of
   handle slots sitting inside the §9.5 "GPU resource handles" 16 MiB
   row (`render-resources-design.md` §3.1 sizing table:
   `virtual_table_ = 1024`, `physical_table_ = 512`, `argbuf_table_ =
   256`, `ring_table_ = 4096`, `sampler_cache_ = 16`).

So the §11.1 test-as-named asks `ResourceResidencyExceeded` to do
double duty — to mean both "per-frame total residency > 512 MiB" *and*
"per-table slot cap exhausted." That is the same two-reasons-to-change
problem spike #874 just resolved for the sampler-cache arm
(`reviews/decisions/resourceresidency-srp.md` §"Decision"). Re-folding
slot-table overflow into `ResourceResidencyExceeded` would reinstall
the §10.1 violation #874 removed.

The §11.1 follow-up was carried as
`reviews/decisions/resourceresidency-srp.md` §"Out-of-scope follow-ups"
item 1 and re-flagged in PR #903 round-1 review as "the §11.1 test name
likely wrong." This spike is the resolution.

## Walking the slot-table allocation path

A slot-table `alloc()` call inserts a value into a fixed-cap entity
table. The path:

1. Caller (e.g. `GraphBuilder::declare_transient`,
   `ResourceCatalog::create_persistent_*`,
   `argbuf_binder_.acquire(...)`,
   `ring_buffer_mgr_.acquire_slice(...)`,
   `sampler_cache_.get_or_create(...)`) calls into the appropriate
   `SlotTable<T, Tag>::alloc(value)`.
2. The table consults its LIFO `free_indices_` list; if non-empty, it
   pops an index, writes the value, returns the handle.
3. If `free_indices_` is empty AND `slots_.size() < cap`, the table
   appends a new slot and returns the handle.
4. If `free_indices_` is empty AND `slots_.size() == cap`, the table
   has no free slot to vend. It **must not** push past `cap`: per
   §3.1 "capacities are sized at init from `RenderSettings` and the
   `QualityTier` baseline," and per §6.1 "the table's underlying
   `eastl::vector` does not reallocate during a frame (capacity is
   pre-sized at init, worst-case for the View)." Resizing the slot
   table mid-frame would (a) break the slot-pointer-stability
   contract that hot-path lookups rely on, and (b) violate the
   no-allocations-in-the-hot-path invariant
   (`render-resources-design.md` §5, `SPEC.md` §4.1.3 invariant 4).
5. The table returns `std::unexpected{render::Error::<variant>}`. The
   question this spike answers is **which** variant.

The slot-table cap is structurally distinct from every existing
exhaustion variant in the resources aggregate:

| Resource bound        | Footprint           | Existing variant              | §10.1 site                                                   |
|-----------------------|---------------------|--------------------------------|--------------------------------------------------------------|
| Persistent allocator  | 128 MiB GPU bytes   | `HeapOutOfMemory`              | `resources/persistent.cpp::PersistentAllocator::allocate`    |
| Transient sub-pool    | 64 / 32 / 16 MiB GPU bytes per pool | `TransientPoolExhausted`     | `resources/alias_planner.cpp::AliasPlanner::compute`         |
| Aggregate residency   | 512 MiB per `ContextTag::render`    | `ResourceResidencyExceeded`  | `resources/transient_pool.cpp::TransientPool::peak_residency_check` |
| Sampler cache         | 16 entries (closed set)             | `SamplerCapExceeded`         | `resources/sampler_cache.cpp::SamplerCache::get_or_create`   |
| Slot table per kind   | 16–4096 entries CPU-side bookkeeping | **(this spike)**            | `resources/handle_table.cpp::SlotTable::alloc`               |

Slot-table overflow is **not** a heap-bytes failure (no `MTL::Heap`
allocation is attempted), **not** an alias-planner peak (no
`AliasPlan` is consulted), **not** an aggregate-residency breach (no
`ContextTag::render` byte total is checked), and **not** a closed-set
sampler-cache cap (which is a different cache). It is a per-table
handle-slot exhaustion that triggers cold-path during graph build
when too many transient / persistent / argbuf / ring / sampler
declarations land in the same frame for the configured cap.

## Alternatives considered

### A. Repoint §11.1 to `HeapOutOfMemory` (re-use existing variant)

Change the §11.1 test name to `slot table — capacity overflow returns
HeapOutOfMemory`. Argue that "ran out of capacity in a fixed-cap
allocator-of-slots" is structurally identical to "ran out of capacity
in the persistent allocator" and the variant should be re-used.

- Zero §5 enum surface growth.
- §3.10 ring buffer manager already vends `HeapOutOfMemory` for ring
  over-quota (`render-resources-design.md` line 629). There is
  precedent for `HeapOutOfMemory` covering non-`MTL::Heap` "I ran out
  of pre-allocated capacity" cases.
- **Disqualifier — re-violates §10.1.** `HeapOutOfMemory` is named
  in §10.1 with one construction site:
  `resources/persistent.cpp::PersistentAllocator::allocate`. Adding
  `resources/handle_table.cpp::SlotTable::alloc` as a second site
  re-introduces the two-construction-sites-per-variant pattern that
  spike #874 just removed. The §10.1 SRP rule says the test
  is being written *for*: "if a future change must construct one of
  these errors from a second site, the SRP boundary is being violated
  and a refactor is required."
- (Aside: the ring-buffer's existing `HeapOutOfMemory` use is itself a
  latent §10.1 violation discovered while walking this path. It is
  out-of-scope for this spike but flagged in §"Out-of-scope follow-ups"
  below for a future leaf to resolve.)
- Recovery semantics also drift. `HeapOutOfMemory` recovery is
  "lower-tier shrinks GPU working set"; slot-table-exhausted recovery
  is "lower-tier reduces declared resource count." The two stories are
  operationally distinct in telemetry / logs. Per
  `error-model.md` §"Logging / Telemetry", `error.code` is the
  structured dispatch field; one variant per semantic class is the
  rule.

### B. Repoint §11.1 to a NEW §5 enumerator `SlotTableExhausted`

Change the §11.1 test name to `slot table — capacity overflow returns
SlotTableExhausted`. Add `render::Error::SlotTableExhausted` to
`SPEC.md` §5 alongside the existing "ABI add" cluster. Add a §10.1
design-name row and a §10.3 per-variant row.

- §5 enum surface grows by one (`render::Error::SlotTableExhausted`).
- §5 ABI bump cost is folded into the already-planned
  `StaleResourceHandle` / `ResourceRoleMismatch` /
  `MeshletCullDispatchFailed` / `BlasBuildFailed` / `GpuFault` /
  `SamplerCapExceeded` ABI bump cluster. Zero incremental ABI bump:
  the ABI hash advances exactly once for the cluster, regardless of
  whether one more enumerator rides it. Same logic spike #874 used to
  fold `SamplerCapExceeded` into the cluster
  (`reviews/decisions/resourceresidency-srp.md` §"Decision" point 3).
- Each variant has exactly one construction site, one trigger, one
  recovery, one severity, one test fixture. The §10.1 single-
  construction-site rule is honoured for every variant in the
  aggregate.
- §10.3 (SPEC) gets a new per-variant row; the existing
  `ResourceResidencyExceeded` and `HeapOutOfMemory` rows are left
  untouched (their trigger language is unchanged).
- `render-resources-design.md` §3.1, §3.4, §3.12, §10, §10.1, §11.1
  update to attribute slot-table overflow to its single site and
  variant.

### C. Repoint §11.1 to `ResourceResidencyExceeded`; add §10.1 row

Add `resources/handle_table.cpp::SlotTable::alloc` as a second
`ResourceResidencyExceeded` construction-site row in §10.1. Leave the
test name unchanged.

- Zero §5 enum surface growth.
- **Disqualifier — directly contradicts spike #874.** Re-folds
  slot-table overflow (per-table CPU-side handle cap) and aggregate
  residency (per-frame GPU-bytes ceiling > 512 MiB) into one variant.
  Recovery is the same (`lower-tier`), but severity, frame phase,
  trigger condition, and operational meaning differ. The §10.2 closed
  recovery ladder is dispatchable on `error.code` alone; one variant
  carrying two semantic classes contradicts that contract for the
  same reason `SamplerCapExceeded` had to split.
- The §11.1 test would also become misleading: `Exhaust cap; next
  alloc returns the typed error` — but the slot-table internal does
  not "compute peak-residency footprint > 512 MiB ceiling" (the §10.3
  trigger language for `ResourceResidencyExceeded`). The name asserts
  one trigger; the test asserts another.

### D. Repoint §11.1 to `TransientPoolExhausted`

Argue that slot-table exhaustion is "the alias planner can't place
any more virtual resources" and route through `TransientPoolExhausted`.

- **Disqualifier — wrong cause.** `TransientPoolExhausted`'s §10.1
  construction site is `resources/alias_planner.cpp::AliasPlanner::
  compute` — the alias planner's peak-residency check, not the
  slot-table's slot-cap check. The two paths run at different
  frame-build moments (`virtual_table_.alloc` runs during
  `declare_transient`; alias planner runs in graph compile *after*
  every declaration). Slot-table overflow happens before the alias
  planner is consulted — the planner never gets a chance to fail.
- Also fails for `physical_table_`, `argbuf_table_`, `ring_table_`,
  and `sampler_cache_` (the table on the front-line) — none of these
  are routed through the alias planner. Cross-table coverage is
  impossible under D.

## Decision

**Adopt Alternative B: repoint §11.1 to a new §5 enumerator
`render::Error::SlotTableExhausted`.**

The change set:

1. Adds `render::Error::SlotTableExhausted` to `specs/render/SPEC.md`
   §5's `render::Error` enum body. Documented as "ABI add" alongside
   `StaleResourceHandle` / `ResourceRoleMismatch` / `SamplerCapExceeded`
   / `MeshletCullDispatchFailed` / `BlasBuildFailed` / `GpuFault`
   in §10.1's closed-sum table; rides the shared upcoming ABI bump
   per `error-model.md` Composition Rule 5.
2. Adds a `SlotTableExhausted` design-name row to `SPEC.md` §10.1.
   Row count grows from twenty-one to twenty-two; §5 enumerator count
   grows from twenty-six to twenty-seven; "ABI add" row count grows
   from six to seven.
3. Adds a `SlotTableExhausted` per-variant row to `SPEC.md` §10.3
   with its trigger / recovery / severity / capability-fallback /
   test-fixture columns, drawn directly from §10.2's closed ladder.
4. Updates `specs/render/render-resources-design.md`:
   - §3.1 — slot-table sizing table footnote attributes overflow to
     `SlotTableExhausted`.
   - §3.12 — failure-mode mapping table grows by one row for
     slot-table overflow.
   - §10 — failure-mode table grows by one row for
     `SlotTableExhausted`.
   - §10.1 — construction-site list grows by one entry for
     `SlotTableExhausted` at `resources/handle_table.cpp::SlotTable::alloc`;
     the §11.1 follow-up note is rewritten to record resolution rather
     than carry the open question.
   - §10.2 — cross-references add the `SlotTableExhausted` arm.
   - §11.1 — test name corrected from
     `slot table — capacity overflow returns ResourceResidencyExceeded`
     to `slot table — capacity overflow returns SlotTableExhausted`.

`ResourceResidencyExceeded` retains its meaning and construction site
verbatim (transient-pool / aggregate-residency, single site at
`resources/transient_pool.cpp::TransientPool::peak_residency_check`).
`HeapOutOfMemory` retains its meaning and construction site verbatim
(persistent allocator best-fit failure, single site at
`resources/persistent.cpp::PersistentAllocator::allocate`).

## Rationale

- **SOLID / SRP first (PHILOSOPHY §1).** Slot-table cap exhaustion
  has its own reason-to-change distinct from heap-bytes exhaustion
  (`HeapOutOfMemory`), aggregate-residency breach
  (`ResourceResidencyExceeded`), alias-planner peak
  (`TransientPoolExhausted`), and sampler-cache over-cap
  (`SamplerCapExceeded`). Five distinct failure conditions → five
  distinct variants, each with one construction site. This is the
  same SRP logic spike #874 used to split `SamplerCapExceeded`.
- **Honors §10.1 verbatim.** The single-construction-site rule the
  design itself adopted is preserved across the entire aggregate.
  Alternatives A and C weaken the rule by example (re-introducing the
  two-sites-per-variant pattern); D mis-attributes the failure
  to a different code path.
- **Closed recovery ladder dispatchability (§10.2).** Recovery must
  be a function of `error.code` alone. One variant per semantic class
  keeps the dispatch mechanical (switch on the enum, pick a ladder
  rung). `SlotTableExhausted` recovery is `lower-tier` (lower
  QualityTier reduces the per-View transient declaration count
  proportionally; persistent slot demands shrink with reduced shadow
  atlas / HZB extent / RT TLAS instance count); severity `warn`
  matches every other tier-driven recovery in the aggregate.
- **Telemetry stays unambiguous.** `glibre::log_error` formats
  `error.code` as the structured dispatch field
  (`error-model.md` §"Logging / Telemetry"). Logging
  `SlotTableExhausted` separately from `HeapOutOfMemory` /
  `ResourceResidencyExceeded` lets the perf overlay's "GPU bytes by
  requester" view distinguish "engine ran out of GPU bytes" from
  "engine ran out of CPU-side handle slots" — operationally
  different conditions an operator must respond to differently
  (former: lower tier; latter: lower tier OR raise per-table cap in
  `RenderSettings`).
- **ABI cost is already paid.** The `StaleResourceHandle` /
  `ResourceRoleMismatch` / `SamplerCapExceeded` /
  `MeshletCullDispatchFailed` / `BlasBuildFailed` / `GpuFault` ABI
  bump cluster is the next planned render-plugin hash advance. Adding
  `SlotTableExhausted` to that cluster is free; the ABI hash advances
  once for all seven enumerators per `error-model.md` Composition
  Rule 5.
- **Occam at the right level.** Option A (re-use `HeapOutOfMemory`)
  is *fewer lines of spec change* but adds a second construction site
  for an existing variant — the §10.1 SRP rule is the harder
  invariant. Option C is *fewer lines* but re-folds two semantic
  classes into one variant — the §10.2 dispatchability rule is the
  harder invariant. Option B's "one variant per reason-to-change" is
  the smaller spec change *measured by invariants violated*: zero.
  Per PHILOSOPHY §10 ("Occam's razor at every decision. Two
  collapsing requirements become one primitive"), the correct
  reverse-direction reading is also captured: when one primitive
  carries two requirements, split.

## Consequences

Positive:

- Per-variant recovery dispatch becomes mechanical for slot-table
  overflow: switch on the enum, pick a ladder rung. No payload
  inspection.
- Telemetry / log structured fields are unambiguous per variant.
- §10.1's SRP-by-construction-site invariant becomes a real rule the
  test suite can `static_assert` against the §5 header in
  `tests/render/spec_§5_§10_consistency.cpp` (existing harness, per
  spike #874's identical logic).
- Slot-table overflow becomes a structurally first-class failure
  mode; future work that varies per-table caps based on tier (e.g.
  Mobile reducing `virtual_table_.cap` to 256) has a clean error to
  surface.
- The §11.1 unit test's expected-return-value clause is brought into
  alignment with §10.1, §10.3, and §5; the
  `tests/render/spec_§5_§10_consistency.cpp` static-assert harness
  remains a one-stop source of truth.

Negative / accepted costs:

- One additional `render::Error` enumerator (26 → 27).
- A new test fixture `tests/render/resources/slot_table_exhausted.cpp`
  (under `tests/render/resources/handle_table.cpp` per §11.1's
  Catch2-file binding — no new file is required; the existing
  `handle_table.cpp` test file already houses the §11.1 test row).
  This is a re-statement of the existing §11.1 row, not a new ask.
- §5 ABI hash advances along with the existing planned bump. Plugin
  authors recompiling against the new hash see all seven new
  enumerators at once
  (`StaleResourceHandle` / `ResourceRoleMismatch` /
  `SamplerCapExceeded` / `MeshletCullDispatchFailed` /
  `BlasBuildFailed` / `GpuFault` / `SlotTableExhausted` — the precise
  set is defined by the ABI bump's PR).

## Out-of-scope follow-ups (not resolved by this spike)

These are flagged for completeness so a future review pass picks them
up; they are independent SRP questions that would not fit one session.

1. **Ring-buffer manager's `HeapOutOfMemory` use (§3.10 line 629,
   `RingBufferManager::acquire_slice`).** The ring buffer's
   `failure_handle_for(HeapOutOfMemory)` introduces a second
   construction site for `HeapOutOfMemory` (the first is
   `resources/persistent.cpp::PersistentAllocator::allocate` per
   §10.1). Per §10.1's single-construction-site SRP rule, this is a
   latent violation. Discovered while walking the slot-table
   allocation path for this spike but out of scope; tracked as a
   follow-up spike for a future leaf. The resolution will likely
   parallel spikes #874 and #904: introduce a new §5 enumerator
   `RingBandExhausted` riding the same ABI bump cluster, with
   construction site `resources/ring_buffer.cpp::RingBufferManager::
   acquire_slice` and recovery `lower-tier`. Not blocking this PR.
   **Tracked as #934** (`[SPIKE] iterate-render-ring-buffer-heapoutofmemory-srp`,
   parented to epic #83).
2. **`tests/render/spec_§5_§10_consistency.cpp` static-assert
   harness.** The harness is named in spike #874's "Consequences"
   section and again here; it does not yet exist (it is part of the
   resources-aggregate implementation plan that will land alongside
   the ABI bump). Authoring it is a `type:plan` issue, not a spike.
   Not blocking this PR.

## Spec edits delivered alongside this decision

- `specs/render/SPEC.md` §5 — add `SlotTableExhausted` enumerator to
  the `render::Error` enum body.
- `specs/render/SPEC.md` §10.1 — add row for `SlotTableExhausted`;
  increment closed-sum row count (21 → 22) and enumerator count
  (26 → 27); update the "ABI add" cumulative-diff narrative (six →
  seven rows).
- `specs/render/SPEC.md` §10.3 — add per-variant row for
  `SlotTableExhausted`.
- `specs/render/render-resources-design.md` §3.1 — slot-table sizing
  table footnote attributes overflow to `SlotTableExhausted`.
- `specs/render/render-resources-design.md` §3.12 — add failure-mode
  mapping table row.
- `specs/render/render-resources-design.md` §10 — add failure-mode
  table row.
- `specs/render/render-resources-design.md` §10.1 — add construction-
  site entry for `SlotTableExhausted` at
  `resources/handle_table.cpp::SlotTable::alloc`; rewrite the §11.1
  follow-up note to record resolution.
- `specs/render/render-resources-design.md` §10.2 — cross-reference
  the `SlotTableExhausted` arm.
- `specs/render/render-resources-design.md` §11.1 — correct test name
  to `slot table — capacity overflow returns SlotTableExhausted`.
- `reviews/decisions/resourceresidency-srp.md` — replace the
  `Out-of-scope follow-ups` item 1 narrative with a one-line
  resolution pointer to this record.

## Follow-up plan

The same `type:plan` issue parented to epic #83 that performs the
spike #874 ABI bump will also land `SlotTableExhausted`:

- The C++ enum extension in the render plugin's public header.
- The construction-site authoring at
  `resources/handle_table.cpp::SlotTable::alloc` (the failure path
  on `slots_.size() == cap_ && free_indices_.empty()`).
- The Catch2 test row already named in §11.1 (no new file needed; the
  test lives in the existing `tests/render/resources/handle_table.cpp`).
- The ABI hash bump alongside the existing
  `StaleResourceHandle` / `ResourceRoleMismatch` /
  `SamplerCapExceeded` / `MeshletCullDispatchFailed` /
  `BlasBuildFailed` / `GpuFault` cluster.
- The `tests/render/spec_§5_§10_consistency.cpp` `static_assert`
  extension to cover the new enumerator.

That plan is a separate leaf; it is not part of this spike.

## Cross-references

- `specs/render/SPEC.md` §5, §10.1, §10.2, §10.3.
- `specs/render/render-resources-design.md` §3.1, §3.4, §3.12, §10,
  §10.1, §11.1.
- `reviews/decisions/resourceresidency-srp.md` — sibling decision
  (spike #874) that installed the §10.1 single-construction-site SRP
  rule this spike honours.
- `reviews/decisions/error-model.md` §"Composition Rules" item 5 (ABI
  bump on enum extension), §"Logging / Telemetry" (`error.code` is
  the dispatch field).
- `PHILOSOPHY.md` §1 (SOLID / SRP first), §10 (Occam's razor —
  reverse direction: when one primitive carries two requirements,
  split).
- Spike #904; parent epic #83; flagged-by PR #903 round-1 review.
