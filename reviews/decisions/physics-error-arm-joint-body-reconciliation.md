# Decision Record — Physics Error-Arm Reconciliation: `BodyStillReferencedByJoint` vs `JointDanglingEndpoint`

## Status

Accepted (spike #908, parent sub-epic #791 detailed designs — physics).
Resolves the contradiction between `specs/physics/SPEC.md` §4.1.7
invariant 1 (which currently names `JointDanglingEndpoint` at the
`remove_body` call site) and `specs/physics/bodies-shapes-design.md`
§10.1 (which names `BodyStillReferencedByJoint` at the same call site).
Inputs to the sibling joints-cluster detailed-design spike, which can
now author its §10 error table without further reconciliation.

Originating context: PR #907 round-1 review comment 3203489294
(MED-severity flag against `bodies-shapes-design.md` §10.1).

## Context

`physics::Error` (SPEC §5) currently enumerates two arms whose
header-stub comments overlap on the single scenario "remove the body
while a live joint references it":

```
BodyStillReferencedByJoint,     // remove blocked by live Joint endpoint.
JointDanglingEndpoint,          // body remove attempted with live joint.
```

SPEC §4.1.7 invariant 1 names `JointDanglingEndpoint` as the return
value at `PhysicsWorld::remove_body`. SPEC §10.1's `JointDanglingEndpoint`
entry says the same. SPEC §10.1's `BodyStillReferencedByJoint` entry
*also* says the same. SPEC §10.2.1's `PhysicsWorld::remove_body` row
lists *both* arms in the "Returnable arms" column.

`bodies-shapes-design.md` §10.1 (already on `main` via PR #907) names
`BodyStillReferencedByJoint` consistently for the bodies-cluster
`remove_body` refusal path, and §10.5 routes `JointDanglingEndpoint`
to the joints cluster as a sibling-detected arm. The two documents
internally agree only after this spike resolves which arm is canonical
at which site.

A third arm in the same neighbourhood, `JointEndpointInvalid`, is
documented at SPEC §10.1 as firing on "zero-init handle, cross-world
handle, body already despawned" at `add_joint` time. The "body already
despawned" clause overlaps with what `JointDanglingEndpoint` is meant
to cover; this spike disambiguates them as part of the resolution.

The state today violates two project rules:

1. **SOLID / SRP first (PHILOSOPHY §1).** Two construction sites for
   one variant means two reasons to change one symbol — the same
   anti-pattern resolved for `render::Error::ResourceResidencyExceeded`
   in `reviews/decisions/resourceresidency-srp.md`. The applicable
   rule is unchanged: each error variant has one canonical
   construction site.
2. **Closed-ladder dispatchability (`error-model.md` Composition
   Rule 2; SPEC §10.2).** Caller-side recovery dispatches on
   `error.code` alone. Two arms covering the same code path cannot
   route to two distinct ladder rungs without payload inspection,
   which `error-model.md` line 100 forbids
   (`ErrorContext.detail` is "optional human hint, never load-bearing").

## Alternatives considered

### A. Retire `JointDanglingEndpoint`; keep only `BodyStillReferencedByJoint`

One arm for the live-joint refusal at `remove_body`. The "body already
despawned" sub-case of `add_joint`-time validation collapses into
`JointEndpointInvalid`.

- Enum shrinks by one variant; one fewer ABI surface row.
- `JointEndpointInvalid` now carries two reasons-to-change: caller
  authored a garbage handle (zero / cross-world / never-existed) **and**
  caller authored a once-valid handle whose body just despawned. The
  recovery paths differ — rebuild from scratch (programming bug,
  `error` severity) vs re-fetch live `BodyId` and retry (timing race,
  `warn` severity). The SRP rule that closed
  `ResourceResidencyExceeded` (decision record
  `resourceresidency-srp.md`) refuses this collapse.
- **Refused** — relocates the SRP violation rather than resolving it.

### B. Retire `BodyStillReferencedByJoint`; keep only `JointDanglingEndpoint` for both sides

One arm spanning both the bodies-cluster `remove_body` refusal and the
joints-cluster `add_joint`-with-stale-endpoint refusal. Matches the
current SPEC §4.1.7 invariant 1 text verbatim, requiring only that
`bodies-shapes-design.md` §10.1 be rewritten.

- Two construction sites under the bodies cluster's `remove_body`
  call site **and** under the joints cluster's `add_joint` call site,
  with two distinct recovery rungs (caller removes joints first vs
  caller refreshes endpoint handle).
- Bodies-aggregate's reason-to-change (body lifecycle: §4.1.5) and
  joints-aggregate's reason-to-change (joint construction validation:
  §4.1.7) are now stacked on one variant. PHILOSOPHY §1 ("Split when
  two reasons to change appear") refuses.
- The §10.2 closed recovery ladder loses dispatchability on
  `error.code` alone — same failure mode the
  `resourceresidency-srp.md` decision rejected.
- **Refused** — same SRP problem from the opposite direction.

### C. Keep both arms with disambiguated semantics, one canonical site each

`BodyStillReferencedByJoint` is the bodies-cluster `remove_body`
refusal (the body is the refused actor; it cannot leave while a joint
holds it). `JointDanglingEndpoint` is the joints-cluster `add_joint`
refusal when an endpoint `BodyId` is stale (the body was live when
the joint was authored but became stale before the add committed —
a timing race distinct from `JointEndpointInvalid`'s zero / cross-
world programming bug). `JointEndpointInvalid` narrows to the
authored-garbage case only.

- Each variant has exactly one canonical construction site, exactly
  one trigger, exactly one recovery rung, exactly one severity, exactly
  one test fixture. SRP-clean per the
  `resourceresidency-srp.md` rule.
- §10.2 closed ladder remains dispatchable on `error.code` alone.
- Telemetry distinguishes "body lifecycle ordering bug"
  (`BodyStillReferencedByJoint`, `warn`) from "joint authored against
  stale handle" (`JointDanglingEndpoint`, `warn`) from "joint authored
  against garbage handle" (`JointEndpointInvalid`, `error`). Three
  operationally distinct conditions, three dashboard buckets.
- Zero ABI bump cost — both arms already exist in §5; this spike
  only sharpens their semantics. `JointEndpointInvalid`'s narrowing
  is a trigger-text edit, not an enumerator removal.
- **Selected.**

## Decision

**Adopt Alternative C: keep both arms; disambiguate by aggregate's
reason-to-change.**

Canonical assignment:

| Arm                            | Owning aggregate (§4)         | Canonical construction site (§5)                | Trigger                                                                                                  | Recovery                                          | Severity |
|--------------------------------|-------------------------------|-------------------------------------------------|----------------------------------------------------------------------------------------------------------|---------------------------------------------------|----------|
| `BodyStillReferencedByJoint`   | `RigidBody` / bodies cluster (§4.1.5) | `PhysicsWorld::remove_body`                    | A live `Joint` entity references the body as endpoint A or B. Detected by Jolt's joint-registry walk before any body-destruction commit. | Caller removes the joint(s) first, then re-issues the body remove. | `warn`   |
| `JointDanglingEndpoint`        | `Joint` / joints cluster (§4.1.7)     | `PhysicsWorld::add_joint`                      | An endpoint `BodyId` was once valid but the body has been despawned between author-time and add-commit (deferred-command race). The handle is non-zero and same-world but no longer resolves. | Caller re-fetches the live endpoint `BodyId` and re-issues the joint add. | `warn`   |
| `JointEndpointInvalid` (narrowed) | `Joint` / joints cluster (§4.1.7)  | `PhysicsWorld::add_joint`                      | An endpoint `BodyId` is zero-init, cross-world, or was never live in this world. Authored-garbage handle. | Caller fixes the gameplay-code path that produced the bad handle. | `error`  |

The split anchors on three independent reasons-to-change:

1. **Body lifecycle.** A body that is still referenced cannot be
   destroyed without dangling Jolt constraints. The bodies aggregate
   owns body destruction (§4.1.5 invariant 3 — destruction is monotone)
   and is the sole detector. `BodyStillReferencedByJoint` is the
   bodies-aggregate-emitted refusal.
2. **Joint construction — stale-but-once-valid handle.** A timing
   race between gameplay's despawn-body command and a deferred
   add-joint command. The joints aggregate's input validation
   (§4.1.7 invariant 1 — joint endpoints must resolve at add time)
   is the sole detector. `JointDanglingEndpoint` is the
   joints-aggregate-emitted refusal for this race.
3. **Joint construction — never-valid handle.** Programming bug:
   handle is zero-init, cross-world, or hand-fabricated. The joints
   aggregate's same input-validation seam detects, but the operational
   meaning differs: this is *never* steady-state, where (2) can be.
   `JointEndpointInvalid` is the joints-aggregate-emitted refusal for
   this case.

Three operationally distinct recovery paths → three variants. The
bodies-aggregate path stays in `BodyStillReferencedByJoint`; the
joints-aggregate paths stay in `JointDanglingEndpoint` and
`JointEndpointInvalid` and split on whether the handle was ever
valid in this world.

## Rationale

- **PHILOSOPHY §1 (SOLID / SRP first).** Each of the three arms now
  has one canonical construction site. The two-construction-sites
  test from `resourceresidency-srp.md` is satisfied without enum
  growth.
- **`error-model.md` Composition Rule 2.** Per-variant recovery
  remains dispatchable on `error.code` alone. No payload inspection,
  no `ErrorContext.detail` overload.
- **PHILOSOPHY §10 (Occam's razor).** The same primitive (one arm)
  is **not** asked to carry two requirements. Where the requirements
  diverge — bodies' lifecycle vs joints' input validation — the
  primitives are kept separate.
- **Minimal SPEC churn.** All three arms already exist in §5; this
  spike refines their per-arm contract in §10.1, the §10.2.1
  `remove_body` row, and the §4.1.7 invariant 1 text. No ABI hash
  bump, no new test fixtures beyond the ones already named in
  `bodies-shapes-design.md` §11.1 (`remove_body_refuses_with_live_joint`)
  and the future joints-cluster design.
- **Sibling joints-design unblock.** The joints-cluster detailed
  design (a sibling spike under sub-epic #791) can now author its
  §10 error table by quoting this record; the bodies-cluster design
  on `main` is already correct under this resolution.

## Consequences

Positive:

- §4.1.7 invariant 1, §5 enum comments, §10.1 per-arm contracts, and
  §10.2.1 entry-point row align under one resolution.
- Bodies-cluster design (`bodies-shapes-design.md` §10.1, §10.5)
  becomes correct as written — no edits required against the
  already-merged PR #907.
- Joints-cluster design has a one-page record to cite, no
  reconciliation work needed at design time.
- Three distinct telemetry classes — body-lifecycle ordering bug,
  joint timing race, joint authored-garbage — each routed to a
  distinct dashboard bucket.

Negative / accepted costs:

- The narrowing of `JointEndpointInvalid` (drop "body already
  despawned" from its trigger) requires a small text edit when the
  joints-cluster design lands. That edit is delegated to the
  sibling joints-design spike (out of scope here per "do not touch
  the joints detailed-design doc" in this spike's dispatch).
- §10.2.1's `remove_body` row currently lists both arms; SPEC §10.2.1
  is left unedited by this spike (its row already lists the canonical
  arm `BodyStillReferencedByJoint`; the `JointDanglingEndpoint`
  reference there is harmless because §5 still permits the
  `add_joint` site, and the bodies cluster's `remove_body` row is
  the bodies-aggregate's view). A future iteration spike (not this
  one) can tighten that row.

## Out-of-scope follow-ups (not resolved by this spike)

These are flagged for completeness so a future review pass picks
them up; they are independent SRP questions outside the one-leaf-
one-session boundary.

1. **Joints-cluster design `joint-add` error table.** The sibling
   joints-cluster detailed-design spike must author its §10 table
   citing this record:
   - `JointEndpointInvalid` — narrowed trigger (zero / cross-world
     / never-live).
   - `JointDanglingEndpoint` — once-valid, now-stale `BodyId` at
     add-commit time.
   That work is the joints-design spike's deliverable, not this one.
2. **§10.2.1 `remove_body` row tightening.** The current row lists
   both arms in "Returnable arms"; under this resolution, only
   `BodyStillReferencedByJoint` is returnable from `remove_body`.
   `JointDanglingEndpoint` belongs in the `add_joint` row (which
   currently lists `JointEndpointInvalid` only). A bookkeeping
   iteration spike can tighten both rows; the sequencing is
   intentional — the joints-cluster design lands first, then the
   table-tightening spike picks up both edits at once.
3. **Refuse-by-Jolt walk vs ECS walk.** §10.1's
   `BodyStillReferencedByJoint` trigger names "Jolt's joint registry
   walk" as the detector. If the bodies-cluster design implements
   the check via an ECS-side reverse index instead (cheaper, no
   middleman crossing), the trigger text needs a one-line edit.
   Decided in the bodies-cluster implementation plan, not here.

## Spec edits delivered alongside this decision

Applied in the same PR as this decision record:

- `specs/physics/SPEC.md` §4.1.7 invariant 1 — replace
  `physics::Error::JointDanglingEndpoint` with
  `physics::Error::BodyStillReferencedByJoint` at the `remove_body`
  call site. The invariant continues to assert that the body
  removal is **refused** until the joint is despawned first.
- `specs/physics/SPEC.md` §5 enum body — refine the per-arm
  comments so the two arms describe their canonical construction
  sites:
  - `BodyStillReferencedByJoint` — "`remove_body` refused while a
    live `Joint` endpoint references the body."
  - `JointDanglingEndpoint` — "`add_joint` refused: endpoint
    `BodyId` was once valid but is now stale (body despawned)."
- `specs/physics/SPEC.md` §10.1 — sharpen the per-arm contract
  bodies for both arms so the trigger sentences name the canonical
  construction site explicitly. `BodyStillReferencedByJoint` keeps
  its current text. `JointDanglingEndpoint`'s trigger flips from
  the (incorrect-as-written) `remove_body` site to the canonical
  `add_joint`-with-stale-endpoint site.
- `specs/physics/SPEC.md` §10 error table additions — one row per
  arm naming its canonical construction site. The §10.4 logging
  severity rows already encode the per-arm severity; only the
  per-arm contract bodies in §10.1 need the construction-site
  attribution.

The §10.2.1 entry-point row tightening (drop `JointDanglingEndpoint`
from the `remove_body` row; move it to the `add_joint` row) is left
to follow-up #2 above.

## Cross-references

- `specs/physics/SPEC.md` §4.1.7 (Joint aggregate), §5
  (`physics::Error` enum), §10.1 (per-arm contract), §10.2.1
  (`PhysicsWorld` entry-point arms), §10.4 (severity table).
- `specs/physics/bodies-shapes-design.md` §10.1, §10.5 (already on
  `main` via PR #907; no edits required by this spike).
- `reviews/decisions/error-model.md` §"Composition Rules" item 2
  (per-context translation is local + explicit), `ErrorContext.detail`
  contract (line 100, "never load-bearing").
- `reviews/decisions/resourceresidency-srp.md` (the precedent SRP
  split for the same anti-pattern in render).
- `PHILOSOPHY.md` §1 (SOLID — SRP first), §10 (Occam's razor — the
  reverse direction: when one primitive carries two requirements,
  split).
- Spike #908; parent sub-epic #791; flagged-by PR #907 review
  comment 3203489294.
