# Decision Record — `ResourceResidencyExceeded` SRP Split

## Status

Accepted (spike #874, parent epic #83 render).
Resolves the §10.1 self-flag in
`specs/render/render-resources-design.md` line 1262 ("Two construction
sites for one variant is an SRP violation that must be resolved in the
implementation plan"). Inputs to a follow-up `type:plan` issue that
performs the §5 enum extension and the C++ construction-site refactor.

## Context

`render::Error::ResourceResidencyExceeded` is currently constructed at
two semantically distinct sites inside the render plugin dylib:

1. `resources/transient_pool.cpp::TransientPool::peak_residency_check` —
   emitted when the alias planner's peak-residency footprint for any
   transient sub-pool exceeds the heap budget, or the aggregate
   per-frame footprint exceeds the 512 MiB cell ceiling
   (`render-resources-design.md` §3.5, §3.12 row "Per-frame total
   residency > 512 MiB"; SPEC §9.5).
2. `resources/sampler_cache.cpp::SamplerCache::get_or_create` —
   emitted when the closed sampler cache (§3.13, cap = 16) is full and
   a new `SamplerDesc` is requested.

`render-resources-design.md` §10.1 fixes the SRP rule for this layer:

> Single construction site per variant is the SRP test: if a future
> change must construct one of these errors from a second site, the
> SRP boundary is being violated and a refactor is required.

The two-site state already violates that rule. The §10.1 self-note
attempted to defer the resolution to "the §5 amendment for
`StaleResourceHandle` / `ResourceRoleMismatch`," but that amendment
landed via PR #872 without resolving this. Round-1 review of #872
re-flagged it (LOW) and opened spike #874.

The two sites are not just notation: their recovery paths and frame
phases are structurally distinct.

| Field                   | Transient-pool site                          | Sampler-cache site                                      |
|-------------------------|----------------------------------------------|---------------------------------------------------------|
| Frame phase (§6.2)      | Phase 6 graph compile                         | Init / hot-reload register (cold path; §3.13)           |
| Trigger frequency       | Quality-tier-driven (residency under load)    | Configuration-driven (sampler set exceeds static cap)   |
| Recovery (§10.2 ladder) | `lower-tier`                                  | `abort-engine` at init / `lower-tier` at hot-reload     |
| Severity                | `warn`                                        | `error`                                                  |
| Test fixture            | `residency_exceeded_lower_tier.cpp`           | `sampler_over_cap.cpp`                                   |
| Telemetry signal class  | "engine is hot, demote tier"                  | "shader / material set is misconfigured, fix build"      |

The recovery row in `render-resources-design.md` §10 currently encodes
both into one row with a forked recovery (`lower-tier (case a)` /
`abort-engine` at init + `lower-tier` at hot-reload `(case b)`) and a
forked severity (`warn (a)`; `error (b)`). The fork is the smoking gun:
the §10.2 closed recovery ladder is supposed to be dispatchable on
`error.code` alone, and a single variant cannot dispatch to two
ladder rungs.

Two distinct *reasons to change* are stacked on one variant. By
PHILOSOPHY §1 (SOLID — SRP first; "Split when two reasons to change
appear") the variant must split.

## Alternatives considered

### A. Split into two variants

`ResourceResidencyExceeded` retains its meaning (transient-pool /
aggregate residency, the §10.3 row's primary trigger). A new §5
enumerator `SamplerCapExceeded` is added for the sampler-cache case.

- §5 enum surface grows by one (`render::Error::SamplerCapExceeded`).
- §5 ABI bump cost is folded into the already-planned
  `StaleResourceHandle` / `ResourceRoleMismatch` ABI bump cluster
  (zero incremental ABI bump — a single bump pays for all four
  enumerators).
- Each variant has exactly one construction site, one trigger, one
  recovery, one severity, one test fixture. The §10.2 closed ladder
  becomes dispatchable on `error.code` for both sites.
- §10.3 (SPEC) gets a new per-variant row for `SamplerCapExceeded`;
  the existing `ResourceResidencyExceeded` row is left untouched
  (its trigger language already names only the transient-pool /
  512 MiB case).
- `render-resources-design.md` §3.12, §3.13, §10 (failure-mode table),
  and §10.1 update to attribute each variant to its single site.

### B. Single shared helper `residency_exceeded(kind, detail)`

A free function in the render-resources aggregate that constructs the
`ResourceResidencyExceeded` arm with a `ResidencyKind { TransientPool,
SamplerCache }` payload encoded into `ErrorContext.detail` (or a new
side-channel field). Both `transient_pool.cpp` and `sampler_cache.cpp`
call the helper; the helper is the lone construction site, satisfying
§10.1's literal text.

- No §5 ABI bump; one variant.
- Recovery routing must inspect `kind` (or `detail`) to pick a
  ladder rung, breaking the §10.2 contract that recovery is
  dispatchable on `error.code` alone.
- `error-model.md` line 100 binds `ErrorContext.detail` as "optional
  human hint, never load-bearing"; making it dispatch-bearing
  contradicts the engine-wide error contract.
- Telemetry dashboards keyed off `error.code` (per error-model.md
  §"Logging / Telemetry") still cannot distinguish "engine is hot"
  from "shader set misconfigured."
- The two reasons-to-change move from "two construction sites" into
  "one helper that knows two construction reasons" — SRP is moved,
  not resolved.

### C. Status quo + `__FILE__` / `__LINE__` disambiguation

Leave one variant; rely on `ErrorContext.file` / `line` already
captured at construction to identify which site emitted the error.

- Zero ABI / spec change.
- Same `error.detail`-is-load-bearing problem as B, except now it is
  source-location-bearing — even more fragile, since refactors that
  move construction sites silently break recovery routing and
  telemetry classification.
- Leaves the explicit §10.1 SRP rule unresolved as documented
  technical debt.

## Decision

**Adopt Alternative A: split into `ResourceResidencyExceeded` (kept,
transient-pool / aggregate residency) + `SamplerCapExceeded` (new,
sampler-cache over-cap).**

The split:

1. Adds `render::Error::SamplerCapExceeded` to `specs/render/SPEC.md`
   §5's `render::Error` enum and to §10.1's closed-sum table as a
   new row.
2. Adds a `SamplerCapExceeded` row to SPEC §10.3 with its own
   trigger / recovery / severity / capability-fallback / test-fixture
   columns, drawn directly from §10.2's closed ladder.
3. Folds the ABI bump into the already-planned
   `StaleResourceHandle` / `ResourceRoleMismatch` cluster ("ABI add"
   rows in §10.1). Zero incremental ABI bump cost; the ABI hash
   advances exactly once for all four enumerators.
4. Updates `specs/render/render-resources-design.md` §3.12, §3.13,
   §10 (failure-mode table), and §10.1 (construction-site rule) so
   each variant resolves to one construction site.

`ResourceResidencyExceeded` keeps its name. The transient-pool case is
the primary trigger named in the existing SPEC §10.3 row, in user
story #402 ("ResourceResidencyExceeded triggers lower-tier recovery"),
and in the existing test fixture
`tests/render/resources/residency_exceeded_lower_tier.cpp`. Holding
the name minimizes diff and keeps the user-story / test-fixture
language unchanged.

## Rationale

- **SOLID / SRP first (PHILOSOPHY §1).** Two reasons-to-change
  → split. The two sites have independent recovery rungs (§10.2),
  independent severity, independent frame phases, and independent
  operational meaning; they are two errors, not one.
- **Closed recovery ladder dispatchability (§10.2).** Recovery
  must be a function of `error.code` alone. Alternatives B and C
  smuggle dispatch-relevant data into `ErrorContext.detail` /
  `file` / `line`, contradicting `error-model.md` line 100.
- **Telemetry stays unambiguous.** `glibre::log_error` formats
  `error.code` as the structured dispatch field; one variant per
  semantic class keeps dashboards meaningful.
- **ABI cost is already paid.** The
  `StaleResourceHandle` / `ResourceRoleMismatch` ABI bump (per
  §10.1's "ABI add" rows + `error-model.md` Composition Rule 5) is
  the next planned render-plugin hash advance. Adding
  `SamplerCapExceeded` to that bump is free.
- **Honors §10.1 verbatim.** The SRP rule the design itself adopted
  is preserved for the future. The alternative paths weaken that
  rule by example.

## Consequences

Positive:

- Per-variant recovery dispatch becomes mechanical: switch on the
  enum, pick a ladder rung. No payload inspection.
- Telemetry / log structured fields are unambiguous per variant.
- §10.1's SRP-by-construction-site invariant becomes a real rule
  the test suite can `static_assert` against the §5 header in
  `tests/render/spec_§5_§10_consistency.cpp` (existing harness).
- Sampler-cache misconfiguration becomes a structurally first-class
  failure mode; future work that data-drives the sampler set
  (post-MVP `material` plugin authoring surface, §3.13 Open
  Question 5) has a clean error to extend.

Negative / accepted costs:

- One additional `render::Error` enumerator (25 → 26).
- A new test fixture `tests/render/resources/sampler_over_cap.cpp`
  must be authored alongside the implementation plan PR. The
  fixture is already named in `render-resources-design.md` §10's
  failure-mode table, so this is a re-statement, not a new ask.
- §5 ABI hash advances along with the existing planned bump.
  Plugin authors recompiling against the new hash see all four new
  enumerators at once (`StaleResourceHandle`,
  `ResourceRoleMismatch`, `MeshletCullDispatchFailed` /
  `BlasBuildFailed` / `GpuFault` / `SamplerCapExceeded` — the
  precise set is defined by the ABI bump's PR).

## Out-of-scope follow-ups (not resolved by this spike)

These are flagged for completeness so a future review pass picks
them up; they are independent SRP questions that would not fit one
session.

1. **§11.1 unit test "slot table — capacity overflow returns
   `ResourceResidencyExceeded`."** **Resolved by spike #904**
   (`reviews/decisions/slot-table-overflow-error.md`). The §11.1
   test name was wrong; the correct variant is a new §5 enumerator
   `render::Error::SlotTableExhausted` riding the same ABI bump
   cluster as `SamplerCapExceeded`. Re-folding slot-table overflow
   into `ResourceResidencyExceeded` (or into `HeapOutOfMemory`)
   would re-violate the §10.1 single-construction-site SRP rule
   this spike installed; instead, slot-table overflow gets its own
   variant with one construction site at
   `resources/handle_table.cpp::SlotTable::alloc` and recovery
   `lower-tier`. See `reviews/decisions/slot-table-overflow-error.md`
   for the full alternatives walk and refutation.
2. **`core::Error::OutOfBudget` translation under
   `GLIBRE_ALLOC_STRICT=1`.** SPEC §6 ("Strict-mode enforcement"
   line 2629) and `render-resources-design.md` §11.6 ("strict-mode
   allocator returns OutOfBudget") describe a translation seam that
   maps `core::Error::OutOfBudget` to
   `render::Error::ResourceResidencyExceeded` at the render
   boundary. This is a *translation*, not a construction site —
   the inbound `core::Error` is reshaped into the equivalent
   `render::Error` at the per-allocator wrapper that called into
   core. Per `error-model.md` Composition Rule 2 ("the mapping is
   local, explicit, and unit-tested"), this seam is correct; it is
   not a §10.1 violation. Documented here so the implementation
   plan does not accidentally treat it as a new construction site.

## Spec edits delivered alongside this decision

- `specs/render/SPEC.md` §5 — add `SamplerCapExceeded` enumerator
  to the `render::Error` enum body.
- `specs/render/SPEC.md` §10.1 — increment closed-sum row count and
  enumerator count; add row for `SamplerCapExceeded`; update the
  "ABI add" cumulative-diff comment.
- `specs/render/SPEC.md` §10.3 — add per-variant row for
  `SamplerCapExceeded`.
- `specs/render/render-resources-design.md` §3.12 — split the
  failure-mode-mapping table row.
- `specs/render/render-resources-design.md` §3.13 — replace the
  "returns `ResourceResidencyExceeded`" sentence.
- `specs/render/render-resources-design.md` §10 — split the §10
  failure-mode table row; rewrite the §10.1 construction-site list
  so each variant attributes to one site; remove the SRP-violation
  self-note.

## Follow-up plan

A `type:plan` issue parented to epic #83 will perform:

- The C++ enum extension in the render plugin's public header.
- The construction-site refactor:
  `transient_pool.cpp::peak_residency_check` keeps emitting
  `ResourceResidencyExceeded`; `sampler_cache.cpp::get_or_create`
  switches to `SamplerCapExceeded`.
- The new Catch2 test
  `tests/render/resources/sampler_over_cap.cpp` (named in §10).
- The ABI hash bump alongside the existing
  `StaleResourceHandle` / `ResourceRoleMismatch` cluster.
- The `tests/render/spec_§5_§10_consistency.cpp`
  `static_assert` extension to cover the new enumerator.

That plan is a separate leaf; it is not part of this spike.

## Cross-references

- `specs/render/SPEC.md` §5, §10.1, §10.2, §10.3.
- `specs/render/render-resources-design.md` §3.5, §3.12, §3.13,
  §10, §10.1.
- `reviews/decisions/error-model.md` §"Composition Rules" item 5
  (ABI bump on enum extension), §"Logging / Telemetry"
  (`error.code` is the dispatch field), `ErrorContext.detail`
  contract.
- `PHILOSOPHY.md` §1 (SOLID / SRP first), §10 (Occam's razor — the
  reverse direction: when one primitive carries two requirements,
  split).
- Spike #874; parent epic #83; flagged-by PR #872.
