# physics — Detailed Design: joints aggregate

> Per-aggregate detailed design for the `Joint` family declared in
> `specs/physics/SPEC.md` §4.1.7, with public surface frozen in §5
> (`JointEndpoints`, `JointLimits`, `JointMotor`, `JointBreakThreshold`,
> `JointBrokenEvent`, `JointId`, `JointKind`, `PhysicsWorld::add_joint` /
> `remove_joint`) and persistence locked in §7.1.3 / §7.1.4. Refines
> those sections in place; adds no new public surface beyond §5. Cites
> `reviews/decisions/error-model.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/frame-phases.md`,
> `reviews/decisions/fory-codegen.md`. Deviations from the cited
> records require an amendment spike, not an in-place edit.
>
> Sibling aggregates inside the physics context — `PhysicsWorld` /
> `PhysicsConfig` / `Accumulator` (covered by
> `specs/physics/physics-world-design.md`); `RigidBody` / `BodyId` /
> `Collider` / `ShapeHandle` / `ShapeBlob` / `Sleeping` / `CCD`
> (covered by `specs/physics/bodies-shapes-design.md`); `Substep`,
> `ContactManifold` / `Trigger`, `PhysicsQueries`, `BroadphaseLayer`,
> `PhysicsSnapshot`, `JoltMiddleman` — are deliberately **out of scope
> here**; this design treats them as opaque seams that the joints
> cluster brokers across. Where this design names a sibling seam, it
> cites the SPEC invariant the seam upholds, not the sibling's internal
> mechanics. In particular, `BodyId` allocation, `RigidBody` lifecycle,
> and the `BodyStillReferencedByJoint` refusal at `remove_body` time
> are owned by the bodies-shapes cluster (`bodies-shapes-design.md`
> §10.1, §4.1.7 inv 1); the joints cluster only authors the symmetric
> stale-endpoint detection at `add_joint` time and the per-joint
> entries the bodies cluster's gate consults.
>
> Harmonius prior art (`harmonius/docs/requirements/physics/
> constraints-and-joints.md` R-4.3.1 .. R-4.3.12 + R-4.3.NF1 ..
> R-4.3.NF3; `harmonius/docs/design/physics/constraints.md` § "Joint
> Components" / "Constraint Solver Pipeline" / "Warm Starting" /
> "Breakable Joints") is **research input only** — every conclusion
> below is independently re-derived per `PHILOSOPHY.md` § "How
> harmonius is used".
>
> Refs: spike #796 — `[SPIKE] design-physics-joints-detailed`.
> Parent: #791 (sub-epic — Detailed Designs — physics). Sibling
> `[SPIKE] task-breakdown-physics-joints-detailed` is blocked by
> this deliverable. Spike #908 — `[SPIKE] iterate-physics-error-arm-
> joint-body-reconciliation` — **resolved** 2026-05-09 (commit
> `865405e3`); the `BodyStillReferencedByJoint` ↔ `JointDanglingEndpoint`
> arm split is now captured in
> `reviews/decisions/physics-error-arm-joint-body-reconciliation.md`
> and reflected in §3.4, §10.1, §10.5, §11.1, and §12 of this design.

## 1. Purpose

The joints cluster is the **per-`ecs::Entity` constraint mirror** of
the physics context (SPEC §4.1.7). Its single composed responsibility
is **owning the per-joint-entity constraint topology, the deterministic
32-bit `JointId` that names the constraint inside Jolt's constraint
table, the per-joint kind / endpoint / anchor-frame configuration,
the optional `JointLimits` / `JointMotor` / `JointBreakThreshold`
companion components, and the lifecycle that emits `JointBrokenEvent`
and despawns the entity when the break threshold trips**. The cluster
is what `PhysicsWorld::add_joint` / `remove_joint` actually do once
the facade forwards. Concretely the cluster owns:

1. **The `JointEndpoints` ECS component (SPEC §4.1.7 + §5)** — the
   per-entity constraint header: the `JointId` payload, the
   `JointKind` ordinal (`Point` / `Hinge` / `Slider` / `Cone` /
   `Distance` / `SwingTwist`), the two `BodyId` endpoints, and the two
   `JointFrame` anchors (one per body, in body-local space). POD-like
   aggregate stored in core's archetype storage; identity is the
   owning `ecs::Entity`. The joint **is** an ECS entity (§4.1.7
   invariant 1) — bodies hold no list of joints; constraints exist as
   a separate archetype.
2. **The `JointLimits` companion component (SPEC §5)** — optional
   axis-agnostic bounded range component (`lower` / `upper` /
   `swing_y` / `swing_z` / `twist_low` / `twist_high`). Semantics
   depend on `JointKind`: `Hinge` reads `lower` / `upper` as twist
   limits, `Slider` reads them as linear limits, `Cone` reads
   `swing_y` / `swing_z`, `SwingTwist` reads all six, `Point` and
   `Distance` ignore. Absence means unbounded (§4.1.7 invariant 2).
3. **The `JointMotor` companion component (SPEC §5)** — optional
   powered-drive component (`enabled` / `target_value` / `max_force` /
   `damping`). Semantics depend on `JointKind`: `Hinge` drives the
   twist axis, `Slider` drives the prismatic axis, `SwingTwist`
   drives the twist axis, others reject the motor at admission.
   Absence means passive (§4.1.7 invariant 2).
4. **The `JointBreakThreshold` companion component (SPEC §5)** —
   optional break-cap component (`max_force` / `max_torque`). When
   Jolt's accumulated impulse on the constraint exceeds the cap the
   cluster despawns the joint entity and emits `JointBrokenEvent`
   (§4.1.7 invariant 4). Absence means unbreakable.
5. **The `JointId` value object + allocator** — the stable 32-bit
   handle into Jolt's constraint table, plus the per-`PhysicsWorld`
   deterministic allocator that issues it. Allocation order is fixed
   by ECS materialisation order of the joint entity (PHILOSOPHY §7);
   the allocator is the cell that turns "this entity gained a
   `JointEndpoints` component" into a reload-stable identity (§4.2
   invariant 5 by extension to the joint axis — the body-id stability
   pattern applied to constraints).
6. **The `JointKind` decode dispatcher** — the per-kind path that
   reads a `JointEndpoints` row plus its optional companions and asks
   the middleman to construct the corresponding Jolt constraint
   (`PointConstraint` / `HingeConstraint` / `SliderConstraint` /
   `ConeConstraint` / `DistanceConstraint` / `SwingTwistConstraint`).
   One file per kind under `joints/joint_kinds/` (SPEC §6.1); the
   dispatcher is total over the sealed sum.
7. **The break-threshold check + lifecycle hook** — the per-substep
   exit-barrier walk over the `JointBreakThreshold` archetype that
   reads Jolt's accumulated impulse per constraint and, on threshold
   trip, emits `JointBrokenEvent` into the per-frame ECS event buffer
   and despawns the joint entity (§4.1.7 invariant 4). The only
   physics-emitted lifecycle event the cluster owns.
8. **The motor-target entry walk** — the per-substep entry-barrier
   walk over the `JointMotor` archetype that pushes
   `JointMotor.target_value` into Jolt's per-constraint motor target
   slot before the Jolt step. Symmetric to the bodies cluster's
   `ExternalForce` drain at substep entry.
9. **The `JointDescriptorRecord` decode hook** — the cluster's role
   at `add_joint` for both fresh-spawn and hot-reload-restore: read
   the surviving (or fresh) descriptor record (SPEC §7.1.3), validate
   the kind / endpoints / anchors / companions, and forward the
   build to the middleman.

This cluster **refuses to own**:

- **The Jolt `PhysicsSystem` instance, the fixed-timestep accumulator,
  the phase-3 driver, and the substep barrier ordering** — owned by
  the `physics-world` cluster (`physics-world-design.md`). The cluster
  is called *by* the phase-3 driver at substep barriers; it never
  enters `JoltMiddleman::step` and never authors the substep loop.
- **The substep math** (broadphase, narrowphase, constraint solve,
  contact resolution, integration, warm-start blending, joint Jacobian
  construction). All of it lives inside `JoltMiddleman::step`, behind
  the §4.1.13 ABI seam. Per SPEC §3.2 collapse #1 the cluster is a
  **constraint-topology mirror**, not a solver author. The
  warm-start factor lives on `PhysicsConfig` (§4.1.7 invariant 3 +
  §3.2 collapse #2); per-joint solver-iteration overrides are
  refused for determinism. Constraint Jacobians, effective-mass
  computation, and accumulated-impulse storage are Jolt-internal
  state and never cross the §4.1.13 seam.
- **`RigidBody` / `BodyId` lifecycle and the `remove_body` refusal
  path** — owned by the bodies-shapes cluster
  (`bodies-shapes-design.md`). The joints cluster maintains a
  `BodyId → joints` reverse index (§3.5 below) so the bodies-shapes
  `remove_body` path can ask "are any joints still referencing this
  body?" and receive `BodyStillReferencedByJoint` if so (SPEC §10.1).
  The joints cluster owns the index; it does **not** own the
  refusal arm itself — that lives at the bodies-shapes
  `remove_body` call site (`bodies-shapes-design.md` §10.1 row).
- **`Collider` / `ShapeHandle` / `ShapeBlob` lifecycle** — owned by
  the bodies-shapes cluster. Joints reference bodies, not shapes;
  the cluster never reads `Collider` rows.
- **Contact / trigger event payloads** — owned by the `contact/`
  sibling aggregate (SPEC §4.1.8 / §4.1.9). The cluster's only event
  is `JointBrokenEvent`; contact-vs-joint pair interactions are
  Jolt-internal (Jolt's island builder couples contacts and
  constraints transparently).
- **Spatial queries** — owned by the `queries/` sibling aggregate
  (SPEC §4.1.10). The cluster's `JointId` does not appear on any
  query surface.
- **The `PhysicsSnapshot` codec** — owned by the `snapshot/` sibling
  aggregate (SPEC §4.1.12, §7.1.4). The cluster reads / writes the
  per-joint **columns** of the snapshot (§7.1.4 schema tags 15–20)
  but the writer / reader plumbing is a sibling.
- **`PhysicsMaterial`** — joints carry no material reference;
  contact / friction surfaces live on `Collider` (bodies-shapes
  cluster). A joint is purely a topology constraint between two
  bodies; the surface response of the bodies' contacts is unrelated.
- **Ragdoll authoring, severance, prosthetic re-attachment, ropes,
  chains, ragdoll LOD, Verlet fallback** — refused per SPEC §3.3.
  R-4.3.4 (ragdoll skeletons), R-4.3.5 (rope / chain authoring),
  R-4.3.7 (limb severance + `JointSevered` event), R-4.3.8
  (prosthetic re-attachment), R-4.3.10–R-4.3.12 (ragdoll LOD,
  per-platform bone caps, Verlet chain fallback) all route to
  post-MVP `animation` / `character` / `destruction` plugins. The
  cluster's responsibility ends at the six MVP `JointKind`
  primitives + `JointBrokenEvent`; ragdoll skeleton spawn is the
  caller's composition of N joints, not a cluster surface.
- **Solver selection (SI vs TGS)** — refused. R-4.3.6's
  `SolverConfig.solver_type` is collapsed by SPEC §3.2 collapse #2
  into "Jolt's deterministic solver, period". The cluster carries no
  per-joint or per-world solver knob; warm-start factor lives on
  `PhysicsConfig` (§4.1.2 collapse #6).
- **`WarmStartData` ECS component** — refused. R-4.3.9's per-joint
  `WarmStartData` (accumulated impulse cached on an ECS row) is
  collapsed into Jolt-internal state behind the §4.1.13 seam.
  Persisted accumulated impulses cross the swap via the snapshot's
  `joint_normal_impulses` / `joint_friction_impulses` columns
  (§7.1.4) — the cluster carries no ECS-side cache.
- **Jolt header inclusion.** `<Jolt/...>` headers live behind exactly
  one TU (`middleman/jolt_middleman.cpp`; SPEC §6.1 module rule 1,
  §4.1.13 invariant 1). The cluster's TUs include only the §5 facade
  and the `JoltMiddleman` reference threaded through every forwarding
  call.
- **GPU work, asset bytes, scripting intents** — refused per SPEC §1
  + §3.3.

The cluster's SRP boundary is sharp: **the only reason `JointEndpoints`,
`JointLimits`, `JointMotor`, `JointBreakThreshold`, `JointId`,
`JointKind`, or `JointBrokenEvent` would change is a change to "what
constraint binds two bodies, what bounds / drives / breaks it, and how
that lifecycle becomes ECS-visible"**. Anything else — what one
substep computes, what a body looks like, what a query returns, what
gets serialised across hosts, what surface response a contact carries
— routes to the sibling aggregate that owns it.

## 2. Requirements coverage

Mapping of harmonius `R-4.3.*` clauses (`constraints-and-joints.md`)
onto MVP coverage in this aggregate. Every entry is independently
re-derived; coverage sites refer to sections of `specs/physics/SPEC.md`
and to the design sections below. Clauses owned by sibling aggregates
**inside the physics context** are listed as **Routed (sibling)** with
the SPEC §4.1.* row that owns them; clauses outside MVP scope are
listed as **Refused** with the §3.3 routing target the SPEC already
records.

### 2.1 Aggregate-owned clauses

| Harmonius clause                                                                                                          | Disposition                                                                                                                                                                                                                                                                                                                                                                                                                                          |
|---------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-4.3.1** revolute / prismatic / fixed / distance joints as ECS entities + 6-DOF + cone-twist extensions                | **Covered (six-kind sealed sum) + Refused (spring + raw 6-DOF).** §3.4 `JointKind` dispatcher covers `Point` (≡ harmonius "fixed-translation 3-DoF"), `Hinge` (≡ revolute), `Slider` (≡ prismatic), `Cone` (≡ swing-only cone-twist subset), `Distance` (≡ distance constraint), `SwingTwist` (≡ full cone-twist with twist axis). The "Fixed (6-DoF lock)" harmonius case is collapsed onto Jolt's `FixedConstraint` which the cluster surfaces via the `SwingTwist` kind with all four limits at zero (§3.4 invariant 4 — caller pattern). The harmonius "Spring" and "Generic6Dof" kinds are **refused** at MVP (§2.3); the six MVP kinds cover the §11 acceptance stories #435 / #437. |
| **R-4.3.2** optional `JointMotor` and `JointLimits` companion components                                                  | **Covered.** §3.6 `JointMotor` model + §3.7 `JointLimits` model. Both are POD ECS companion components on the joint entity; absence means passive / unbounded (§4.1.7 invariant 2). The kind-dispatcher reads the bits at create-time and forwards to the corresponding Jolt setter (`HingeConstraint::SetMotorState` etc.).                                                                                                                          |
| **R-4.3.3** breakable joints with force / torque thresholds emitting `JointBroken` events                                  | **Covered.** §3.8 `JointBreakThreshold` model + §3.9 break-detection walk. The walk runs at substep exit (§3.9.1); on threshold trip the cluster emits `JointBrokenEvent` into the per-frame ECS event buffer and despawns the joint entity (§4.1.7 invariant 4, §4.2 invariant 8 — same-frame delivery). Story #435 closes on this row.                                                                                                          |
| **R-4.3.9** warm-start cached impulses                                                                                     | **Covered (snapshot path) + Refused (ECS component).** Per-substep warm-start state is Jolt-internal (§3.2 collapse #1 collapses harmonius `WarmStartData` into Jolt's internal state). Cross-substep persistence happens via the `joint_normal_impulses` / `joint_friction_impulses` columns of `PhysicsSnapshot` (§7.1.4 schema tags 19–20); the cluster reads them at restore (§7.2 below) and forwards to Jolt's warm-start hook through the middleman. The harmonius `WarmStartData` ECS component is **refused** — same data, one carrier, on the snapshot.                                            |
| Harmonius design — Joint = ECS entity (not a body component)                                                              | **Covered.** §3.2 `JointEndpoints` + §3.5 reverse index. SPEC §4.1.7 invariant 1 makes "the joint is an entity, not a list-on-a-body" load-bearing; the cluster's reverse `BodyId → std::pmr::vector<JointId>` index is the only allowed lookup path and is used exclusively by the bodies-shapes cluster's `remove_body` gate.                                                                                                                          |
| Harmonius design — Joint anchor frames in each body's local space                                                         | **Covered.** §3.2 `JointEndpoints.frame_a` / `frame_b` (each a `JointFrame { Vec3 position, Quat rotation }`). Per §7.1.3 invariant 4 the anchor quaternions must be unit-normalised at decode; non-unit returns `physics::Error::ConfigInvalid` at admission.                                                                                                                                                                                       |
| Harmonius design — `JointBroken` event carries breaking force and breaking torque magnitudes                              | **Covered.** §5 `JointBrokenEvent { joint, kind, applied_force, applied_torque }`. The cluster reads Jolt's accumulated normal / friction impulses on the constraint at the substep exit, divides by `fixed_dt` to yield force / torque, populates the event payload (§3.9.2). Same-frame delivery follows §4.2 invariant 8.                                                                                                                         |

### 2.2 Sibling-aggregate clauses (routed within the physics context)

These clauses are MVP-scope but their reason-to-change is owned by a
sibling aggregate. Each routes to the SPEC §4.1.* row that owns it;
the corresponding sibling design spike fills in the body. Listed here
so the cluster's seam shape is exhaustive.

| Harmonius clause                                                                                                          | Routed to (sibling SPEC row)                                                                  |
|---------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------|
| **R-4.3.6** sequential-impulse + TGS solvers selectable via `SolverConfig`                                                | §4.1.4 `Substep` (Jolt-internal, deterministic mode pinned by `physics-world-design.md` §6.4 R1) — collapsed by SPEC §3.2 collapse #2 |
| **R-4.3.10** per-platform warm-start factor on `SolverConfig`                                                             | §4.1.2 `PhysicsConfig.warm_start_factor` (`physics-world-design.md` §3.2)                     |
| **R-4.3.NF1** 5 000 constraint rows / ms (500 joints in 4 ms)                                                             | **Reframed against §9.** SPEC §9.2 row "Jolt step" (1.50 ms / two-substep upper bound) + §9.3 row "Jolt body + constraint pools" (64 MiB) absorb the joint-solve cost; 500 joints is post-MVP scaling against the S1 fixture (~5–8 joints in the ragdoll fixture). |
| **R-4.3.NF3** 32-segment chain stability (positional drift < 1 mm over 60 s)                                              | Sibling `Substep` (Jolt's solver convergence + the deterministic-mode contract); the cluster's contribution is correct anchor frames at admission and faithful break-threshold reporting. Chain spawning itself is post-MVP (§2.3 row R-4.3.5). |
| Harmonius design — accumulated impulse persisted across substeps for warm-start                                            | §4.1.12 `PhysicsSnapshot` columns 19–20 (`joint_normal_impulses` / `joint_friction_impulses`); the cluster fills them at capture, reads them at restore. |
| Harmonius design — Jolt constraint lifecycle (build / solve / release)                                                    | §4.1.13 `JoltMiddleman` (the constraint build / step / destroy entry points on the middleman seam) |

### 2.3 Refused clauses (out of MVP scope)

Refused per SPEC §3.3; the §3.3 routing target the SPEC already
records is repeated here for the cluster's surface.

| Harmonius clause                                                                                                                                | Routing target                                                                  |
|-------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------|
| **R-4.3.1** "Spring" `JointType` (stiffness + damping + rest-length elastic connection)                                                          | Post-MVP. Jolt's `DistanceConstraint` already supports a spring-mode (`SetLimitsSpringSettings`); when a story demonstrates load-bearing spring-vs-hard-distance distinction, a future kind ordinal docks against §3.4. The MVP `Distance` kind is the rigid form. |
| **R-4.3.1** "Generic6Dof" `JointType` (per-axis Locked / Limited / Free on all 6 DOF)                                                            | Post-MVP. Jolt's `SixDOFConstraint` exists; a future kind ordinal docks against §3.4 with a richer `JointLimits` payload (per-axis enum + per-axis ranges). The MVP `SwingTwist` kind covers the ragdoll-friendly subset. |
| **R-4.3.4** Ragdoll configuration from skeleton assets with anatomically plausible limits + per-platform bone caps                                | Post-MVP `animation` plugin; phase 4 reserved per `frame-phases.md`. The cluster provides the joint primitives (`SwingTwist` for shoulders / hips); ragdoll authoring composes N joints from skeleton bone metadata, owned by `animation`. |
| **R-4.3.5** Ropes and chains as linked joint entity sequences with configurable segment count + stiffness + per-platform caps                     | Post-MVP `gameplay` / `vfx` plugin. The cluster provides `Distance` joint primitives; the chain-as-entity-list authoring is the caller's composition, not a cluster surface. |
| **R-4.3.7** Limb severance via per-joint damage thresholds, spawning severed limbs as ragdoll entities + emitting `JointSevered` events           | Post-MVP `destruction` plugin (SPEC §3.3 row "Destruction"). The cluster's `JointBrokenEvent` is the upstream signal; severed-limb spawn + `JointSevered` event are the destruction layer's responsibility. SPEC §3.2 collapse #5 — single physics event family. |
| **R-4.3.8** Prosthetic / replacement limb attachment to severed sockets at runtime, re-establishing physics constraints + updating skeleton hierarchy | Post-MVP `animation` + `character`. The cluster's `add_joint` is the attach primitive; socket / hierarchy updates live in animation. |
| **R-4.3.10** Ragdoll LOD tiers (full solve → reduced bone count → animation-driven blend) based on camera distance                                 | Post-MVP `animation` + `tools` (LOD authoring); the cluster has no per-camera-distance branch. |
| **R-4.3.11** `RagdollLod` per-tier joint solve / bone count                                                                                       | Post-MVP `animation` (same).                                                    |
| **R-4.3.12** Position-based Verlet integration fallback for distant chain joints (skip the constraint solver)                                     | Post-MVP. The MVP cluster sends every joint through Jolt's solver; no Verlet fallback path. |
| **R-4.3.NF2** Ragdoll activation < 0.5 ms / ragdoll, 8 simultaneous activations / frame                                                            | Post-MVP `animation`; the cluster's per-joint admission cost (§9.2) bounds activation but ragdoll-as-N-joints performance is post-MVP scaling. |
| Harmonius `WarmStartData` ECS component                                                                                                            | Refused per SPEC §3.2 collapse — Jolt-internal warm-start state, persisted only via `PhysicsSnapshot` columns 19–20. |
| Harmonius `SolverConfig.solver_type = SequentialImpulse \| TemporalGaussSeidel`                                                                    | Refused per SPEC §3.2 collapse #2 — one deterministic solver, no runtime swap. |

### 2.4 Coverage summary

Of the 15 harmonius `R-4.3.*` clauses (R-4.3.1 .. R-4.3.12 + three
non-functional rows) plus ~6 cross-cutting harmonius design rows
(`Joint` ECS shape, anchor frames, motor / limit composition,
breakable joints, `JointBroken` event, warm-start persistence):

- **6 covered** as joints design here (the six-kind sealed sum +
  motor / limits companions + break threshold + entity-not-component
  rule + anchor-frame rule + `JointBroken` event payload).
- **3 routed to sibling aggregates** within the physics context (the
  R-4.3.6 / R-4.3.10 solver-config rows + R-4.3.NF3 chain stability
  + the warm-start persistence path).
- **9 refused with rationale** — 7 deferred post-MVP (Spring +
  Generic6Dof joint kinds, ragdoll authoring, rope / chain authoring,
  limb severance, prosthetic re-attachment, ragdoll LOD tiers, Verlet
  fallback), 2 collapsed by SPEC §3.2 (`WarmStartData` ECS component,
  `SolverConfig` solver-type knob).

This list is closed; PRs adding any of the refused clauses to the
`joints/` cluster should be rejected and routed to the listed owner.

## 3. Detailed model

The model below is the implementer's authority for the
`joints/joint.{hpp,cpp}`, `joints/joint_break.{hpp,cpp}`, and
`joints/joint_kinds/*.cpp` TUs (SPEC §6.1). Each subsection owns
one of the cluster's primitives; the SPEC §4.1.7 invariants the
primitive enforces are cited per subsection.

### 3.1 Composition and module boundary

```text
physics/src/joints/                                (private headers; not on plugin include path)
├── joint.{hpp,cpp}                                (§3.2 + §3.4 ─ JointEndpoints mirror; kind dispatcher; reverse index)
├── joint_break.{hpp,cpp}                          (§3.8 + §3.9  ─ JointBreakThreshold check + JointBrokenEvent emit + entity despawn)
└── joint_kinds/                                   (§3.4         ─ One TU per JointKind variant)
    ├── point.cpp                                  (§3.4.1 — JPH::PointConstraint)
    ├── hinge.cpp                                  (§3.4.2 — JPH::HingeConstraint)
    ├── slider.cpp                                 (§3.4.3 — JPH::SliderConstraint)
    ├── cone.cpp                                   (§3.4.4 — JPH::ConeConstraint)
    ├── distance.cpp                               (§3.4.5 — JPH::DistanceConstraint)
    └── swing_twist.cpp                            (§3.4.6 — JPH::SwingTwistConstraint)

physics/include/glibre/physics/
└── physics.hpp                                    (§5 facade header; SPEC §5 — locked surface)
```

`world/physics_world.hpp` is the single dependency hub (SPEC §6.1
module rule 3); it owns the `unique_ptr` reference to the
`JointRegistry` (§3.5 below) and is the only header outside
`physics/src/joints/` permitted to include the headers above. The
CMake visibility rule cited by SPEC §6.1 module rule 3 enforces this:
a sibling module's `.cpp` may include its own `.hpp`s and the §5
facade only — cross-module reach-throughs are forbidden, and the seam
is `world/physics_world.hpp`.

The TU decomposition by SRP (PHILOSOPHY §1):

- **`joint.cpp`** — one reason to change: how a `JointEndpoints` ECS
  row plus its optional companions becomes a Jolt constraint at
  `add_joint`, and how the `BodyId → joints` reverse index is
  maintained (§3.5). Owns the kind dispatch (one switch over
  `JointKind`); the per-kind body lives in the kind-specific TU.
- **`joint_break.cpp`** — one reason to change: how Jolt's
  accumulated impulse-per-constraint is read at substep exit and
  compared against the `JointBreakThreshold` companion (§3.9). On
  trip, emits the event and despawns the entity. The break logic is
  separate from `joint.cpp` because its reason-to-change is solver
  output → ECS event, distinct from "how the constraint is built".
- **`joint_kinds/<kind>.cpp` (one TU per kind)** — one reason to
  change per kind: how a specific Jolt constraint subclass is
  configured from the `JointEndpoints` row + companions. A new kind
  is one new TU + one new switch arm; an existing kind's tuning
  change is one TU edit. PHILOSOPHY §1: each kind's reason-to-change
  is the corresponding Jolt constraint's API contract, distinct
  across kinds.

A second TU per primitive is forbidden; one reason to change → one
TU. The `JointLimits` / `JointMotor` / `JointBreakThreshold`
companions are POD aggregates (§5) whose mirror work is part of
`joint.cpp` (the kind dispatcher reads them); they do not have their
own TUs because their "reason to change" rides with the corresponding
kind's TU (the per-kind constraint setter is what consumes them).

### 3.2 `JointEndpoints` (§4.1.7; covers harmonius `Joint` ECS shape + anchor frames)

A POD ECS component whose layout is locked at SPEC §5. Per §4.1.7
invariant 1 the joint **is** an entity bearing this component; bodies
hold no list of joints. The cluster's **internal view** decorates the
public bytes with the resolved Jolt `ConstraintRef` + the per-substep
break-check cache; the resolved ref lives behind the `JoltMiddleman`
seam (§4.1.13 invariant 1) and never crosses the public boundary.

```cpp
// physics/src/joints/joint.hpp — internal view of the §5 JointEndpoints
namespace glibre::physics::detail {

struct JointEndpointsView {
    // ---- Public bytes (§5 JointEndpoints; middleman ECS storage) ----
    JointId    joint_id;             // §5: JointEndpoints.joint_id (the §3.5 allocator output)
    JointKind  kind;                 // §5: JointEndpoints.kind (pinned at create; §4.1.7 inv 1)
    BodyId     body_a;               // §5: JointEndpoints.body_a
    BodyId     body_b;               // §5: JointEndpoints.body_b
    JointFrame frame_a;              // §5: JointEndpoints.frame_a (anchor in body_a local space)
    JointFrame frame_b;              // §5: JointEndpoints.frame_b (anchor in body_b local space)

    // ---- Per-joint mirror cache (NOT part of the public component) ----
    // Lives on the per-archetype scratch row keyed by JointId; rebuilt
    // at admission, drained at every substep exit. Survives neither
    // the snapshot nor a hot-reload (§8 — the ConstraintRef is
    // image-private and rebuilt at resume from the surviving record).
    JoltConstraintRef cached_ref;        // resolved Jolt constraint pointer (opaque type)
    float             accum_normal_impulse;   // last-substep Jolt impulse (for break check)
    float             accum_friction_impulse; // last-substep Jolt impulse
    bool              broken;            // sticky bit set when threshold trips, cleared at despawn
};

}  // namespace glibre::physics::detail
```

**Invariants** (§4.1.7 authoritative):

1. **One `JointEndpoints` per joint ECS entity.** Component-add at the
   ECS layer is the trigger for `JointIdAllocator::allocate(entity)`
   (§3.5); component-remove is the trigger for
   `JoltMiddleman::remove_constraint(joint_id)` plus
   `JointIdAllocator::release(joint_id)` plus the reverse-index
   eviction. Cross-world component moves are forbidden (the `JointId`
   would be cross-world, mirroring the §4.1.5b invariant 2 body case);
   the cluster honours core's archetype-move barrier the same way the
   bodies-shapes cluster does.
2. **`JointKind` is pinned for the joint's lifetime.** Mutating
   `kind` post-create returns `physics::Error::JointKindUnsupported`
   (§4.1.7 invariant 1, §10.1) — the wire shape (same Jolt
   `ConstraintRef` for two distinct kinds) is never produced;
   switching kind means despawn + re-add, which yields a new
   `JointId`. Mirrors the `MotionType` pinning rule on `RigidBody`
   (`bodies-shapes-design.md` §3.2 invariant 2).
3. **Endpoints are resolvable at admission.** `body_a` and `body_b`
   must resolve to live `BodyId`s in the world's `BodyIdAllocator`
   table at `add_joint` time. The cluster calls
   `body_id_allocator.is_live(body_id)` for each endpoint before
   forwarding to the middleman. Per
   `reviews/decisions/physics-error-arm-joint-body-reconciliation.md`
   Alt C, two distinct arms apply:
   - `physics::Error::JointEndpointInvalid` (`error`) — authored-garbage
     handle: zero-init, cross-world, or a handle that was never live in
     this world. Programming bug; caller must fix the code path that
     produced the bad handle.
   - `physics::Error::JointDanglingEndpoint` (`warn`) — the endpoint
     `BodyId` was once valid but the body was despawned between
     author-time and `add_joint` commit (deferred-command timing race).
     Caller re-fetches the live `BodyId` and re-issues.
   Cross-world `BodyId`s are detected via the per-world allocator (a
   cross-world id is "not live in this world" and was never live in
   this world → `JointEndpointInvalid`).
4. **Anchor quaternions are unit-normalised.** `frame_a.rotation` and
   `frame_b.rotation` MUST satisfy `|q| ∈ [1 − 1e−6, 1 + 1e−6]` at
   admission; non-unit returns `physics::Error::ConfigInvalid` (§7.1.3
   invariant 4). The cluster does not silently re-normalise — a
   non-unit quaternion is a content-pipeline bug, surfaced loudly.
5. **`body_a != body_b`.** A joint binding a body to itself is
   refused with `physics::Error::JointEndpointInvalid` at admission;
   self-joints are not in any of the six MVP kinds' contract.
6. **Hot prefix for the per-joint mirror.** The first 32 B of the
   `JointEndpoints` ECS row hold `joint_id` + `kind` + 1 B reserved +
   `body_a` + `body_b` + 16 B of anchor data; the cold suffix
   (`frame_a` rotation, `frame_b` position + rotation) follows.
   `static_assert(sizeof(JointEndpoints) <= 64)` pins the layout to
   one cache line so the per-substep break-check walk (§3.9.1) is
   cache-friendly.

### 3.3 `JointFrame` value object (§5)

A POD `{ Vec3 position, Quat rotation }` declared in §5 (`struct
JointFrame`); the per-body-local-space anchor that defines the
constraint's origin and orientation. Two per joint: one in `body_a`'s
frame, one in `body_b`'s frame.

The cluster's role:

1. **Validate at admission.** Quaternion unit-normalisation per
   §3.2 invariant 4. Position fields are unconstrained (any finite
   `Vec3` is admissible — the constraint's geometry is the
   gameplay's choice).
2. **Forward verbatim to Jolt.** The kind dispatcher copies
   `frame_a.position` / `frame_a.rotation` / `frame_b.position` /
   `frame_b.rotation` into Jolt's per-kind setting struct (e.g.
   `HingeConstraintSettings::mPoint1` / `mHingeAxis1` /
   `mNormalAxis1` / mirror for body B). The mapping per kind is
   documented per-kind in §3.4.
3. **Bit-exact round-trip.** Per `physics-world-design.md` §3.5 R4
   the float bytes round-trip via `std::bit_cast<u32>` so two hosts
   building the same joint from the same descriptor record produce
   byte-equal Jolt constraint impulse trajectories (§7.1.3
   invariant 5). The cluster never re-normalises, re-projects, or
   re-orthogonalises the frames — they are the deterministic input.

### 3.4 `JointKind` decode dispatcher (§4.1.7; covers R-4.3.1 six MVP kinds)

The `JointKind` is the closed sealed sum declared in §5 over six
ordinals (`Point` / `Hinge` / `Slider` / `Cone` / `Distance` /
`SwingTwist`). The cluster's `joint.cpp::add_joint` is the dispatcher:
it reads the `JointEndpoints` row plus its optional companions,
dispatches by `kind`, and asks the middleman to construct the
corresponding Jolt constraint subclass.

```cpp
// physics/src/joints/joint.cpp — pseudocode for the dispatcher
Result<JoltConstraintRef> add_joint_to_jolt(const JointEndpointsView& ep,
                                            const JointLimits*         limits,
                                            const JointMotor*          motor,
                                            const JointBreakThreshold* brk,
                                            JoltMiddleman& mm,
                                            const BodyIdAllocator& bodies) noexcept {
    // Step 1 — Endpoint validity (§3.2 invariant 3).
    // Three operationally distinct arms per reviews/decisions/physics-error-arm-joint-body-reconciliation.md Alt C:
    //   JointEndpointInvalid  (error)  — authored-garbage handle: zero-init, cross-world, or never-live.
    //   JointDanglingEndpoint (warn)   — once-valid handle whose body was despawned (timing race).
    //   Self-loop check fires JointEndpointInvalid regardless (programming bug, not a timing race).
    const bool a_was_ever_live = bodies.was_ever_live(ep.body_a);
    const bool b_was_ever_live = bodies.was_ever_live(ep.body_b);
    if (!bodies.is_live(ep.body_a)) {
        return std::unexpected{
            a_was_ever_live
                ? physics::Error::JointDanglingEndpoint   // body existed, then was despawned — timing race
                : physics::Error::JointEndpointInvalid    // garbage handle: zero-init / cross-world / never-live
        };
    }
    if (!bodies.is_live(ep.body_b)) {
        return std::unexpected{
            b_was_ever_live
                ? physics::Error::JointDanglingEndpoint
                : physics::Error::JointEndpointInvalid
        };
    }
    if (ep.body_a == ep.body_b) {
        return std::unexpected{physics::Error::JointEndpointInvalid};
    }

    // Step 2 — Anchor frame validity (§3.2 invariant 4).
    if (!is_unit_quat(ep.frame_a.rotation) ||
        !is_unit_quat(ep.frame_b.rotation)) {
        return std::unexpected{physics::Error::ConfigInvalid};
    }

    // Step 3 — Companion compatibility (§3.6 / §3.7).
    if (motor != nullptr && motor->enabled && !kind_supports_motor(ep.kind)) {
        return std::unexpected{physics::Error::ConfigInvalid};
    }

    // Step 4 — Dispatch to per-kind decode.
    switch (ep.kind) {
        case JointKind::Point:      return decode_point(ep, limits, motor, brk, mm);
        case JointKind::Hinge:      return decode_hinge(ep, limits, motor, brk, mm);
        case JointKind::Slider:     return decode_slider(ep, limits, motor, brk, mm);
        case JointKind::Cone:       return decode_cone(ep, limits, motor, brk, mm);
        case JointKind::Distance:   return decode_distance(ep, limits, motor, brk, mm);
        case JointKind::SwingTwist: return decode_swing_twist(ep, limits, motor, brk, mm);
    }
    return std::unexpected{physics::Error::JointKindUnsupported};  // unknown ordinal
}
```

**Invariants** (§4.1.7 authoritative):

1. **Sealed-sum dispatch is total.** A reader observing an
   out-of-range ordinal returns `physics::Error::JointKindUnsupported`
   (§10.1 row); the future-build case (a newer cook ships an ordinal
   this build does not know) is the same arm. The §7.2.3 schema-version
   migration path covers the future-build case explicitly via
   `HotReloadStateUnmigratable` (§10.1) when the
   `JointDescriptorRecord.schema_version` is the newer-build
   discriminator.
2. **Per-kind decode is total over its companion combinations.**
   Each per-kind TU handles all four combinations of `(limits?,
   motor?, break?)` where each is a present / absent bit. Absent
   companions skip the corresponding Jolt setter; present companions
   call the corresponding Jolt setter exactly once at admission.
3. **No runtime kind swap.** Per §3.2 invariant 2, kind is pinned at
   create. The dispatcher never observes "kind changed" — it would
   be a programming error caught upstream by the bodies-shapes-style
   archetype-move barrier.
4. **Fixed (6-DoF lock) joint via SwingTwist with zeroed limits.**
   Harmonius's `Fixed` joint kind (R-4.3.1) is expressed in MVP as a
   `SwingTwist` joint with `JointLimits { swing_y = 0, swing_z = 0,
   twist_low = 0, twist_high = 0 }`; Jolt's `SwingTwistConstraint`
   degenerates correctly into a 6-DoF fixed weld in that limit. Adding
   a dedicated `Fixed` kind ordinal is post-MVP if a perf measurement
   shows the SwingTwist degeneration as load-bearing.

#### 3.4.1 `Point` (kind 0)

- **Tags read.** `body_a`, `body_b`, `frame_a.position`,
  `frame_b.position`. (The point constraint ignores the rotation
  components of the anchor frames — it locks position only.)
- **Validation.** None beyond the dispatcher's pre-flight.
- **Companion compatibility.** `JointLimits` ignored; `JointMotor`
  rejected (`ConfigInvalid` if `motor->enabled`); `JointBreakThreshold`
  honoured.
- **Jolt mapping.** `JPH::PointConstraintSettings` with `mPoint1 =
  frame_a.position`, `mPoint2 = frame_b.position`, `mSpace =
  EConstraintSpace::LocalToBodyCOM`. The middleman's
  `create_point_constraint` returns the `JoltConstraintRef`.
- **Solver shape (informational).** 3 linear constraint rows; no
  angular rows (§"Constraint Row Counts per Joint Type" in harmonius
  prior art, re-derived against Jolt 2025).
- **Memory cost (Jolt-internal).** Constant per joint; Jolt's
  `PointConstraint` is a single-cache-line struct.

#### 3.4.2 `Hinge` (kind 1)

- **Tags read.** All of `JointEndpoints` plus `JointLimits.lower`,
  `JointLimits.upper` (twist bounds in radians) when limits present;
  `JointMotor.target_value` (target angular velocity in rad/s),
  `JointMotor.max_force` (max torque in N·m), `JointMotor.damping`
  when motor present.
- **Validation.** When limits present: `lower <= upper`; both finite.
  Failure → `ConfigInvalid`.
- **Jolt mapping.** `JPH::HingeConstraintSettings`:
  - `mPoint1 = frame_a.position`, `mPoint2 = frame_b.position`.
  - `mHingeAxis1 = frame_a.rotation * Vec3::Z`, `mHingeAxis2 =
    frame_b.rotation * Vec3::Z`. (The cluster's anchor-frame
    convention is "the joint's primary axis is the frame's local
    +Z"; documented here so per-kind adapters share the rule.)
  - `mNormalAxis1 = frame_a.rotation * Vec3::X`, `mNormalAxis2 =
    frame_b.rotation * Vec3::X`. (Reference axis perpendicular to
    the hinge axis; defines the zero-twist orientation.)
  - When limits present: `mLimitsMin = JointLimits.lower`,
    `mLimitsMax = JointLimits.upper`.
  - When motor present: post-construction
    `HingeConstraint::SetMotorState(EMotorState::Velocity)` +
    `SetTargetAngularVelocity(JointMotor.target_value)` +
    `GetMotorSettings().mMaxTorqueLimit = JointMotor.max_force`.
- **Solver shape.** 5 constraint rows (3 linear + 2 angular); +1 row
  per active limit; +1 row per active motor.

#### 3.4.3 `Slider` (kind 2)

- **Tags read.** All of `JointEndpoints` plus `JointLimits.lower`,
  `JointLimits.upper` (linear bounds in metres) when limits present;
  `JointMotor.target_value` (target linear velocity in m/s),
  `JointMotor.max_force` (max force in N), `JointMotor.damping` when
  motor present.
- **Validation.** When limits present: `lower <= upper`; both finite.
  Failure → `ConfigInvalid`.
- **Jolt mapping.** `JPH::SliderConstraintSettings`:
  - `mPoint1 = frame_a.position`, `mPoint2 = frame_b.position`.
  - `mSliderAxis1 = frame_a.rotation * Vec3::X`, `mSliderAxis2 =
    frame_b.rotation * Vec3::X`. (Convention: prismatic axis is the
    frame's local +X; symmetric to Hinge's +Z choice for the
    rotational primary axis.)
  - `mNormalAxis1 = frame_a.rotation * Vec3::Y`, `mNormalAxis2 =
    frame_b.rotation * Vec3::Y`.
  - When limits present: `mLimitsMin = JointLimits.lower`,
    `mLimitsMax = JointLimits.upper`.
  - When motor present: post-construction
    `SliderConstraint::SetMotorState(EMotorState::Velocity)` +
    `SetTargetVelocity(JointMotor.target_value)` +
    `GetMotorSettings().mMaxForceLimit = JointMotor.max_force`.
- **Solver shape.** 5 constraint rows (2 linear + 3 angular); +1 row
  per active limit; +1 row per active motor.

#### 3.4.4 `Cone` (kind 3)

- **Tags read.** All of `JointEndpoints` plus `JointLimits.swing_y`
  (half-angle of the cone in radians) when limits present.
- **Validation.** When limits present: `0 <= swing_y <= π`; finite.
  Failure → `ConfigInvalid`.
- **Companion compatibility.** `JointMotor` rejected
  (`ConfigInvalid`); cones do not drive (use `SwingTwist` for the
  driven case). `JointBreakThreshold` honoured.
- **Jolt mapping.** `JPH::ConeConstraintSettings`:
  - `mPoint1 = frame_a.position`, `mPoint2 = frame_b.position`.
  - `mTwistAxis1 = frame_a.rotation * Vec3::Z`, `mTwistAxis2 =
    frame_b.rotation * Vec3::Z`. (The cone's central axis is the
    frame's local +Z, matching the Hinge convention.)
  - `mHalfConeAngle = JointLimits.swing_y` (or π/2 if absent — the
    "unbounded swing" case is meaningless for a cone, so absence of
    `JointLimits` for a `Cone` joint returns `ConfigInvalid` at
    admission).
- **Solver shape.** 3 linear rows + 1 angular row when at limit.

#### 3.4.5 `Distance` (kind 4)

- **Tags read.** All of `JointEndpoints` plus `JointLimits.lower` /
  `JointLimits.upper` (min / max distance in metres) when limits
  present; the unbounded case (no `JointLimits`) uses the body-frame
  separation at admission as the rigid distance.
- **Validation.** When limits present: `0 <= lower <= upper`; finite.
  Failure → `ConfigInvalid`.
- **Companion compatibility.** `JointMotor` rejected
  (`ConfigInvalid`); distance constraints do not drive.
  `JointBreakThreshold` honoured.
- **Jolt mapping.** `JPH::DistanceConstraintSettings`:
  - `mPoint1 = frame_a.position`, `mPoint2 = frame_b.position`.
  - When limits present: `mMinDistance = JointLimits.lower`,
    `mMaxDistance = JointLimits.upper`.
  - When limits absent: `mMinDistance = mMaxDistance =`
    `||world(body_a, frame_a.position) - world(body_b,
    frame_b.position)||` at admission (the rigid-distance case).
- **Solver shape.** 1 linear row.

#### 3.4.6 `SwingTwist` (kind 5)

- **Tags read.** All of `JointEndpoints` plus `JointLimits.swing_y`,
  `JointLimits.swing_z`, `JointLimits.twist_low`,
  `JointLimits.twist_high` when limits present;
  `JointMotor.target_value` (target twist angular velocity in
  rad/s), `JointMotor.max_force` (max torque in N·m) when motor
  present.
- **Validation.** When limits present: `0 <= swing_y <= π`,
  `0 <= swing_z <= π`, `twist_low <= twist_high`, all finite.
  Failure → `ConfigInvalid`.
- **Jolt mapping.** `JPH::SwingTwistConstraintSettings`:
  - `mPosition1 = frame_a.position`, `mPosition2 = frame_b.position`.
  - `mTwistAxis1 = frame_a.rotation * Vec3::Z`, `mTwistAxis2 =
    frame_b.rotation * Vec3::Z` (twist axis = local +Z, matching
    Hinge / Cone).
  - `mPlaneAxis1 = frame_a.rotation * Vec3::X`, `mPlaneAxis2 =
    frame_b.rotation * Vec3::X` (plane reference for swing decomposition).
  - When limits present: `mNormalHalfConeAngle = swing_y`,
    `mPlaneHalfConeAngle = swing_z`, `mTwistMinAngle = twist_low`,
    `mTwistMaxAngle = twist_high`.
  - When motor present: post-construction
    `SwingTwistConstraint::SetTwistMotorState(EMotorState::Velocity)`
    + `SetTargetAngularVelocityCS(...)` + max torque setter.
- **Solver shape.** 3 linear rows + up to 3 angular rows when at
  limit + 1 row per active twist motor.

### 3.5 `JointId` + `JointIdAllocator` + reverse index (§4.1.7)

The `JointId` is the 32-bit `Handle<tags::joint>` declared in §5; the
allocator is the per-`PhysicsWorld` cell that issues it, structurally
identical to the `BodyIdAllocator` (`bodies-shapes-design.md` §3.3).
Allocation order is determined entirely by ECS materialisation order
of the joint entity (§4.2 invariant 5 by extension); the allocator
never reads wall-clock time, host thread identity, or allocator
address.

The cluster also owns a **reverse index** `BodyId →
std::pmr::vector<JointId>` keyed by the joint's two endpoints; it is
the only consultation path for the bodies-shapes cluster's
`remove_body` gate (§4.1.7 invariant 1, §10.1
`BodyStillReferencedByJoint`).

```cpp
// physics/src/joints/joint.hpp (excerpt)
namespace glibre::physics::detail {

class JointIdAllocator {
public:
    [[nodiscard]] static Result<std::unique_ptr<JointIdAllocator>>
        create(std::uint32_t max_constraints) noexcept;

    // Allocate a fresh JointId. Order is the order in which `allocate`
    // is called; the caller (joint.cpp::on_component_add) is serialised
    // by the ECS-archetype-add barrier so the call order mirrors ECS
    // materialisation order.
    [[nodiscard]] Result<JointId>
        allocate(ecs::Entity owner) noexcept;

    [[nodiscard]] Result<void>
        release(JointId) noexcept;

    [[nodiscard]] bool
        is_live(JointId) const noexcept;

    // Cross-reload restore — replays `allocate(entity_i)` for every
    // surviving joint entity in JointId-ascending order so the
    // post-reload allocator state is byte-equal to pre-drain.
    [[nodiscard]] Result<void>
        restore_from_snapshot(std::span<const SnapshotJoint>) noexcept;

private:
    JointIdAllocator() noexcept;
    std::uint32_t                       max_     = 0u;
    std::uint32_t                       next_id_ = 1u;     // 0 reserved as invalid sentinel
    std::pmr::vector<std::uint32_t>     free_list_;        // sort-on-pop, same policy as BodyIdAllocator
    std::pmr::vector<ecs::Entity>       id_to_entity_;     // index = JointId.raw() - 1
};

class JointReverseIndex {
public:
    [[nodiscard]] static std::unique_ptr<JointReverseIndex>
        create(std::uint32_t max_bodies, std::uint32_t max_constraints) noexcept;

    // Called by joint.cpp at add_joint after JointIdAllocator::allocate
    // and Jolt constraint creation.
    void
        record(JointId, BodyId body_a, BodyId body_b) noexcept;

    // Called by joint.cpp at remove_joint and at break-trip (§3.9.2).
    void
        evict(JointId, BodyId body_a, BodyId body_b) noexcept;

    // The bodies-shapes cluster's remove_body gate consults this.
    [[nodiscard]] bool
        any_joint_references(BodyId) const noexcept;

    // Diagnostic — joint count per body (used by §11 acceptance + the
    // editor's inspector).
    [[nodiscard]] std::uint32_t
        joint_count(BodyId) const noexcept;

private:
    JointReverseIndex() noexcept;
    // Keyed by BodyId.raw(); each cell is a small inline-storage vector
    // (glibre::inplace_vector<JointId, 4> — std::inplace_vector polyfill,
    // core/include/glibre/compat/inplace_vector.hpp — P0843R14, C++26)
    // since most bodies hold 0–4 joints (ragdoll knee = 1 hinge;
    // shoulder = 1 swing-twist; vehicle wheel = 1 hinge; chain segment =
    // 2 distance). Overflow-on-push_back → grows via the outer PMR vector;
    // see reviews/decisions/eastl-removal.md §"eastl::fixed_vector".
    std::pmr::vector<glibre::inplace_vector<JointId, 4>> by_body_;
};

}  // namespace glibre::physics::detail
```

**Invariants** (`JointId` allocator + reverse index):

1. **`JointId{0}` is the invalid sentinel.** `JointId{}.valid() ==
   false`; `JointId{0}.raw() == 0`; the allocator never returns id 0.
   Mirrors the `BodyId{0}` rule (`bodies-shapes-design.md` §3.3
   invariant 1).
2. **Allocation order = ECS materialisation order.** Two hosts
   running the same trace produce the same `JointId` for the same
   joint entity (§4.2 invariant 5 extended to constraints). The
   `next_id_` field is a pure function of the call sequence.
3. **Free-list pop is `JointId`-ascending.** `release` adds to
   `free_list_` (sorted on insert); `allocate` pops the smallest.
   Identical to the bodies-shapes free-list policy
   (`bodies-shapes-design.md` §3.3.2).
4. **Bounded by `PhysicsConfig::max_constraints`.** Allocate past the
   ceiling returns `physics::Error::BudgetExceeded` (§10.1).
5. **Reverse index is total over live joints.** Every `JointId`
   issued by `allocate` and not yet `release`d appears in
   `by_body_[body_a]` AND `by_body_[body_b]`. Eviction at
   `remove_joint` and at break-trip (§3.9.2) removes from both
   cells. The cluster runs a debug-build invariant check that the
   sum-of-cell-sizes equals `2 * live_joint_count` at every public
   boundary.
6. **`any_joint_references(body_id)` is `O(1)`.** Reads
   `by_body_[body_id.raw()].empty()`. The bodies-shapes
   `remove_body` gate calls it once per remove; cost is one cache
   line read. The cluster does not iterate the joint set — it only
   asks "is the cell empty".
7. **Restore from snapshot replays in `JointId`-ascending order.**
   Snapshot's `joint_ids` column is the determinism input (§7.1.4
   invariant 5); `restore_from_snapshot` walks it ascending so each
   id is reissued to the correct entity and the post-reload reverse
   index is byte-equal to pre-drain.

### 3.6 `JointMotor` (§5; covers R-4.3.2 motor half)

A POD ECS companion component whose layout is locked at SPEC §5
(`{ bool enabled; float target_value; float max_force; float
damping; }`). Per §4.1.7 invariant 2 the component is **optional**:
absence means the joint is passive.

The cluster's role:

1. **Read at admission (cold path).** When `JointMotor` is present
   and `enabled == true`, the per-kind decoder (§3.4) calls the
   corresponding Jolt motor setter once at admission.
2. **Re-read at substep entry barrier (hot path).** The
   `JointMotor` archetype is walked in `JointId`-ascending order at
   substep entry; for each entry whose `enabled` bit is true and
   whose `target_value` differs from the cached last-pushed value,
   the cluster calls `JoltMiddleman::set_motor_target(joint_id,
   target_value)` to update Jolt's per-constraint motor target slot.
   The walk is part of the §3.10 entry-barrier contribution; cost is
   bounded by the §9.2 ECS↔Jolt mirror row.
3. **Reject mid-substep mutation.** A test that mutates
   `target_value` between the entry barrier and the exit barrier
   (the `in_flight_flag` window) trips `physics::Error::
   SubstepEcsCommitInverted` (`physics-world-design.md` §3.4.4 step
   5). The flag triple owned by the `physics-world` driver catches
   the violation.

**Invariants** (§4.1.7 authoritative):

1. **Companion presence is the discriminator.** Whether the joint
   has a motor is determined by the `JointMotor` ECS component's
   presence on the joint entity (`Query<&JointEndpoints, &JointMotor>`).
   `enabled = false` means "the component is present but the motor
   is currently passive" — the kind decoder still wires the motor
   slot at admission so flipping `enabled` to true mid-run takes
   effect at the next entry barrier.
2. **Adding / removing the companion mid-run is permitted.**
   §4.1.7 invariant 2 ("Adding a companion mid-run is permitted; it
   crosses one substep boundary to commit"). The ECS-side
   `add_component` / `remove_component` is observed at the next
   entry barrier; the kind decoder re-reads the motor slot and
   forwards to Jolt. The kind itself remains pinned (§3.2 invariant
   2) — only the companion bits change.
3. **Kind compatibility is checked at admission.** `Point` / `Cone`
   / `Distance` reject `JointMotor` with `ConfigInvalid` (§3.4.1 /
   §3.4.4 / §3.4.5). `Hinge` / `Slider` / `SwingTwist` accept; the
   per-kind decoder owns the wiring.
4. **No per-motor solver overrides.** `damping` is forwarded to
   Jolt's motor settings as the motor's stiffness damping; the
   global `PhysicsConfig.warm_start_factor` (§4.1.2) governs the
   solver's warm-start blending across all joints. Per-motor
   warm-start factors are refused for determinism (§3.2 collapse #2).

### 3.7 `JointLimits` (§5; covers R-4.3.2 limits half)

A POD ECS companion component whose layout is locked at SPEC §5
(`{ float lower, upper, swing_y, swing_z, twist_low, twist_high; }`).
Per §4.1.7 invariant 2 the component is **optional**: absence means
the joint is unbounded (where the kind admits it; `Cone` rejects
absence per §3.4.4).

The cluster's role:

1. **Read at admission (cold path).** The per-kind decoder reads the
   subset of fields the kind cares about (§3.4 per-kind rows). Fields
   the kind ignores are not validated.
2. **No re-read at substep barriers.** Limits are immutable across
   substeps in MVP. A gameplay system that mutates `JointLimits`
   mid-run will not see the change applied until the joint is
   despawned and re-added — the cluster does not push limit changes
   through `JoltMiddleman::set_motor_target`-style updates because
   Jolt's limit setters are admission-time only on most constraint
   types and the cross-substep update would require a per-frame walk
   that does not fit the §9 budget. This is a deliberate restriction;
   it can be relaxed post-MVP via a `JoltMiddleman::set_joint_limits`
   addition gated by the §9.2 ECS↔Jolt mirror row.
3. **Mutating `JointLimits` mid-run is silently ignored.** The §11
   acceptance test asserts this: a mutation crosses no error arm
   (the data is well-formed) but does not affect simulation behaviour
   until the next despawn + re-add. A debug-build telemetry counter
   tracks "limit mutations observed but not applied" so the editor
   can surface the divergence.

**Invariants** (§4.1.7 authoritative):

1. **Per-kind subset rule.** Each kind reads only its subset
   (`Hinge` / `Slider`: `lower` / `upper`; `Cone`: `swing_y`;
   `SwingTwist`: all four swing/twist fields; `Point` / `Distance`:
   `lower` / `upper` for Distance, none for Point). Codegen / the
   per-kind decoder enforces; out-of-subset fields default to zero
   in the ECS row (since the struct is POD).
2. **Range validation per kind.** Each per-kind decoder validates
   the kind's subset (e.g. `lower <= upper` for Hinge / Slider /
   Distance; `0 <= swing_y <= π` for Cone / SwingTwist); failures
   return `ConfigInvalid` (§10.1).
3. **Bit-exact storage.** All six fields cross the snapshot via
   `std::bit_cast<u32>` per §7.1.3 invariant 5; two hosts authoring
   the same `JointLimits` produce byte-equal Jolt impulse trajectories.

### 3.8 `JointBreakThreshold` (§5; covers R-4.3.3 break thresholds)

A POD ECS companion component whose layout is locked at SPEC §5
(`{ float max_force, max_torque; }`). Per §4.1.7 invariant 2 the
component is **optional**: absence means the joint is unbreakable.

The cluster's role:

1. **Read at admission (cold path).** When present, the
   `JointBreakThreshold` is forwarded to Jolt at constraint
   construction via Jolt's `Constraint::SetEnabled(true)` plus a
   middleman-side `set_break_force(joint_id, max_force, max_torque)`
   call — the actual break-detection runs in glibre's substep-exit
   walk (§3.9), not Jolt's internal `Constraint::OnFailure` callback,
   because Jolt's callback fires on the worker thread and would
   require synchronisation with the ECS-side despawn we want to keep
   single-threaded (§6.4).
2. **Read at substep exit (hot path).** The `JointBreakThreshold`
   archetype is walked in `JointId`-ascending order at substep exit;
   for each entry the cluster reads Jolt's accumulated normal /
   friction impulses for the constraint, divides by `fixed_dt` to
   yield force / torque magnitudes, compares against `max_force` /
   `max_torque`, and on threshold trip emits `JointBrokenEvent` and
   despawns the joint entity (§3.9 below).

**Invariants** (§4.1.7 invariant 4):

1. **The trip signal is `JointBrokenEvent`.** Single physics-emitted
   joint lifecycle event; severance / fragment spawn / prosthetic
   re-attachment are post-MVP plugin work (§2.3). The cluster's
   responsibility ends at the event emission and the entity despawn.
2. **Same-frame delivery.** The event is written to the per-frame
   ECS event buffer at substep exit and is visible to phases 5+
   inside the same frame (§4.2 invariant 8). The buffer is reclaimed
   at end of phase 8.
3. **Despawn is total.** A broken joint's `JointEndpoints` row is
   removed from ECS storage (the entity is despawned), the
   `JointId` is released (§3.5), the reverse index is evicted
   (§3.5 invariant 5), and the Jolt constraint is destroyed via
   `JoltMiddleman::remove_constraint(joint_id)`. No partial state
   survives; subsequent reads of the joint entity return "entity
   does not exist" through core's ECS surface.
4. **Mutation of a broken joint is refused.** A `set_joint_motor` /
   `set_joint_limits` call against a `JointId` that has tripped its
   threshold returns `physics::Error::JointBroken` (§10.1). The
   sticky `broken` bit on the per-archetype scratch (§3.2 cache
   field) is the discriminator; it is set at break-trip time and
   read by any subsequent mutate path before the despawn drains the
   row at end-of-substep.

### 3.9 Break-detection walk (§3.8 enforcement)

The walk is implemented in `joints/joint_break.cpp` and called only
by `world/phase3_driver.cpp` at substep exit (§3.10.2 below). It is
the authority for `JointBrokenEvent` emission.

#### 3.9.1 Per-substep walk

```text
For joint in JointBreakThreshold archetype, sorted ascending by JointId:
  if joint.broken:
    continue                                      # already trip-marked; despawn handled below
  scratch <- per-archetype scratch row keyed by joint_id
  scratch.accum_normal_impulse   <- JoltMiddleman::accumulated_normal_impulse(joint_id)
  scratch.accum_friction_impulse <- JoltMiddleman::accumulated_friction_impulse(joint_id)
  applied_force  <- abs(scratch.accum_normal_impulse) / fixed_dt
  applied_torque <- abs(scratch.accum_friction_impulse) / fixed_dt
  thresh <- read JointBreakThreshold component on joint entity
  if applied_force > thresh.max_force OR applied_torque > thresh.max_torque:
    emit JointBrokenEvent {
      joint:          joint_id,
      kind:           JointEndpoints.kind,
      applied_force:  applied_force,
      applied_torque: applied_torque,
    }
    scratch.broken = true                         # sticky bit; gate further mutations
```

After the walk, a second pass collects every joint whose `broken` bit
is set this substep and despawns the entities + releases the
`JointId`s + evicts from the reverse index + calls
`JoltMiddleman::remove_constraint(joint_id)`. Despawn is deferred to
the second pass so that the first-pass walk is read-mostly over the
archetype (no concurrent component removals).

#### 3.9.2 Despawn ordering invariant

The despawn pass runs **after** the contact-event drain (`physics-
world-design.md` §3.4.4 step 4: "drain Jolt's `ContactListener`
adapter into the per-frame ECS event buffers") so that contact events
involving a broken joint's endpoints are emitted with the
constraint still in Jolt's island; the despawn happens at the very
end of the exit barrier, after all event drains. This matches §4.2
invariant 8 — `JointBrokenEvent` is delivered same-frame alongside
the contact / trigger events; phases 5+ may observe the despawned
joint entity and the `JointBrokenEvent` together.

The reverse index eviction (§3.5 invariant 5) happens in the same
despawn pass; the bodies-shapes `remove_body` gate that ran earlier
in the same substep would not see the joint as live (it ran at the
substep-entry window, before the break-detection walk).

#### 3.9.3 Determinism

Per §6.4 R2 (`physics-world-design.md` §3.5) the walk sorts by
`JointId` ascending before reading Jolt's accumulated impulses. The
emit order of `JointBrokenEvent`s in the per-frame buffer is
`JointId`-ascending, which makes the event stream byte-equal across
hosts (§4.2 invariant 8). The accumulated-impulse values themselves
are bit-exact across hosts because Jolt's deterministic-mode
contract pins them (`physics-world-design.md` §3.5 R1).

### 3.10 ECS↔Jolt mirror — entry / exit barrier contribution (§4.2 inv 2)

The mirror seam between core's archetype storage and Jolt's
constraint table is implemented in `joints/joint.cpp` and called only
by `world/phase3_driver.cpp` (which the `physics-world` design owns).
The cluster contributes two walks: one at substep entry (motor
targets), one at substep exit (break-detection). Both are part of
the `physics-world-design.md` §3.6 mirror seam contract; the cluster
authors the per-row body, the driver schedules the walks.

#### 3.10.1 Entry barrier — motor targets

```text
For joint in JointMotor archetype where enabled, sorted ascending by JointId:
  if scratch[joint_id].cached_target != JointMotor.target_value:
    JoltMiddleman::set_motor_target(joint_id, JointMotor.target_value)
    scratch[joint_id].cached_target = JointMotor.target_value
```

Per `physics-world-design.md` §3.6.1 the cluster contributes the
joint walk; the body force / torque drain is the bodies-shapes
contribution. The walk is sorted by `JointId` ascending using a
per-substep arena scratch span (§3.4.4 step 1 reuse).

#### 3.10.2 Exit barrier — break detection

```text
For joint in JointBreakThreshold archetype, sorted ascending by JointId:
  (the §3.9.1 walk body)
```

Per `physics-world-design.md` §3.6.2 the cluster contributes the
break-detection walk; the body velocity / position writeback is the
bodies-shapes contribution. The despawn-pass runs at the very end of
the exit barrier (§3.9.2 ordering invariant).

Three properties this seam guarantees:

1. **No mid-substep cross-traffic.** The two walks above are the
   only legal transit points; a debug-build assertion fires if any
   ECS read or write happens between them (§4.1.4 invariant 2,
   `physics::Error::SubstepEcsCommitInverted`). The
   `entry_barrier_flag` / `in_flight_flag` / `exit_barrier_flag`
   triple owned by the `physics-world` driver is what the assertion
   reads (`physics-world-design.md` §5.1).
2. **Iteration order is `JointId` ascending.** Not archetype-chunk
   order, not Jolt's internal constraint-list order. The handle is
   the determinism key (§4.2 invariant 5 + §4.2 invariant 8); a host
   that re-orders the walk reproduces the same snapshot bytes only
   by accident. The sort is part of the §6.4 R2 fixed-iteration-order
   determinism guard (`physics-world-design.md` §3.5 R2).
3. **Drain is total.** The motor-target cache on the per-archetype
   scratch is overwritten in place at every substep entry; the
   accumulated-impulse cache is overwritten at every substep exit;
   nothing leaks across substep boundaries. The break-detection
   despawn pass commits before phase 3 returns so phase 5+ readers
   see the post-step ground truth (broken joints gone, events
   emitted).

### 3.11 Cross-context handoffs

Joints is one slice of the physics plugin; the cluster honours these
cross-context seams in addition to the `physics-world` cluster's
broker contract:

1. **Bodies-shapes consults the reverse index, not the cluster
   directly.** Per §3.5 invariant 6, the bodies-shapes
   `remove_body(BodyId)` path calls `JointReverseIndex::
   any_joint_references(body_id)` and returns
   `physics::Error::BodyStillReferencedByJoint`
   (`bodies-shapes-design.md` §10.1) if the cell is non-empty. The
   joints cluster owns the index but does not author the refusal
   arm; the index is a `const` query surface from the bodies cluster's
   perspective.
2. **The `physics-world` driver schedules both walks.** Per
   `physics-world-design.md` §3.4.4, the driver calls the
   joints-cluster motor-target walk at step 1 (entry barrier, after
   bodies' force / torque drain) and the break-detection walk at
   step 4 (exit barrier, after bodies' velocity / position
   writeback, before contact-event drain). The cluster's role is the
   walk body; the driver owns the ordering.
3. **The contact aggregate observes nothing from the cluster.** The
   `contact/` sibling drains Jolt's `ContactListener` for contact
   and trigger pairs; joint pairs do not appear there. Jolt's island
   builder couples bodies via constraints transparently — a joint
   "binds" two bodies into the same island so contact-vs-joint pair
   interactions surface as ordinary contacts on the bodies, not as a
   joint-side event.
4. **The snapshot codec walks the joint columns.** Per §7.1.4 schema
   tags 15–20 the cluster contributes six columns; the codec is
   sibling. The snapshot bytes are the only physics-side
   cross-process / cross-reload carrier for joint state (§3.2
   collapse #9). Authoring of the joint topology at fresh world spawn
   is via the `JointDescriptorRecord` schema (§7.1.3); the cluster
   reads it at `add_joint` for both fresh-spawn and hot-reload-restore.
5. **`tools` (editor) writes `JointEndpoints` + companion components.**
   Editor authoring of joint state happens in phase 1 or phase 2
   (per `frame-phases.md` open question 3); the cluster's mirror
   reads the resulting components at the next phase 3 entry barrier
   without distinction between "editor wrote" and "gameplay wrote"
   — the seam is the ECS write, not the authoring path. Body
   despawn while the editor is mid-edit on a joint endpoint is gated
   by the §3.5 reverse index (the editor sees the
   `BodyStillReferencedByJoint` arm just like a gameplay system would).


## 4. Public surface

The §5 facade (SPEC §5; locked) is the contract every caller compiles
against. This section maps the SPEC §5 declarations onto the §3
internal model and pins three usage rules the SPEC §5 preamble does
not state in one place. **No new public surface is introduced here**;
deviations would require a SPEC §5 amendment spike, not an in-place
edit.

### 4.1 SPEC §5 → §3 mapping (cluster slice)

| SPEC §5 declaration                                            | §3 internal owner                            | Notes                                                                                                              |
|----------------------------------------------------------------|----------------------------------------------|--------------------------------------------------------------------------------------------------------------------|
| `struct JointEndpoints`                                        | §3.2 `JointEndpointsView`                    | POD ECS component. Layout-locked; the cluster's mirror cache (`cached_ref`, `accum_*_impulse`, `broken`) lives off-component on the per-archetype scratch (`bodies-shapes-design.md` §3.2 cache pattern). |
| `struct JointFrame`                                            | §3.3                                         | POD value type embedded in `JointEndpoints`. Quaternion unit-norm validated at admission (§3.2 invariant 4).        |
| `struct JointLimits`                                           | §3.7                                         | POD ECS companion component; optional. Per-kind subset rule (§3.7 invariant 1).                                     |
| `struct JointMotor`                                            | §3.6                                         | POD ECS companion component; optional. Kind compatibility checked at admission (§3.6 invariant 3).                  |
| `struct JointBreakThreshold`                                   | §3.8                                         | POD ECS companion component; optional. Triggers `JointBrokenEvent` at substep exit (§3.9).                          |
| `struct JointBrokenEvent`                                      | §3.9                                         | Substep-exit lifecycle event written into the per-frame ECS event buffer. The cluster's only physics-emitted event. |
| `enum class JointKind`                                         | §3.4                                         | Sealed six-value sum (Point / Hinge / Slider / Cone / Distance / SwingTwist). Pinned at create.                     |
| `using JointId = Handle<tags::joint, std::uint32_t>`           | §3.5 `JointIdAllocator`                      | 32-bit handle; `JointId{0}` is the invalid sentinel.                                                                |
| `PhysicsWorld::add_joint(...)`                                 | §3.4 dispatcher + §3.5 allocate + §3.5 reverse-index record | Forwards through the `physics-world` facade; the cluster authors the bytes inside.                                  |
| `PhysicsWorld::remove_joint(JointId)`                          | §3.5 release + reverse-index evict + middleman remove_constraint | Total over a live `JointId`; refused on stale handle with `BodyNotFound` (sibling `physics-world` arm). |

### 4.2 Three cluster-specific usage rules

1. **`add_joint` requires both endpoint bodies to be live.** The §5
   facade's `add_joint(JointEndpoints, JointLimits*, JointMotor*,
   JointBreakThreshold*)` accepts a `JointEndpoints` whose `body_a` /
   `body_b` are already live `BodyId`s. The cluster does **not**
   spawn or look up bodies — the caller calls `PhysicsWorld::add_body`
   first for each endpoint, stores the returned `BodyId` on
   `JointEndpoints.body_a` / `body_b`, and then calls `add_joint`.
   This keeps the joint-add path single-responsibility (it does not
   mix body lifecycle with constraint allocation) and matches the
   SPEC §5 signature — `JointEndpoints.body_a` is a `BodyId`, not a
   `RigidBody`.
2. **`remove_body` is refused while a joint endpoint is live; the
   caller must `remove_joint` first.** Per §4.1.7 invariant 1 +
   §3.5 invariant 6, the bodies-shapes `remove_body(BodyId)` path
   consults the reverse index (`JointReverseIndex::
   any_joint_references`) and returns
   `physics::Error::BodyStillReferencedByJoint`
   (`bodies-shapes-design.md` §10.1) if any joint still references
   the body. The caller's expected pattern is "iterate joints
   referencing the body, `remove_joint` each, then `remove_body`".
   The cluster does not provide an "iterate joints by body" public
   surface — the reverse index is `internal-only`; callers must
   track joint identity themselves at the gameplay layer.
3. **Companion components may be added / removed mid-run; kind may
   not.** Per §4.1.7 invariant 2 the optional companions
   (`JointLimits` / `JointMotor` / `JointBreakThreshold`) may be
   added or removed on the live joint entity; the change crosses
   one substep boundary to commit (the cluster's entry-barrier walk
   re-reads the motor and break states; limits are admission-only
   per §3.7 restriction). The kind itself is pinned at create
   (§3.2 invariant 2) — a `JointKind` change requires despawn +
   re-add, which yields a new `JointId`.

## 5. Hot/cold path split

The cluster is touched once per substep on the game-loop driver
thread (`physics-world-design.md` §6) plus episodically at joint
spawn / despawn time. The critical hot path is the per-substep
mirror walks (`joints/joint.cpp::push_motor_targets` /
`joints/joint_break.cpp::check_thresholds`); the cold paths are
joint lifecycle (`add_joint` / `remove_joint`) and the per-kind
decode dispatch (`joints/joint_kinds/*.cpp::decode_*`).

### 5.1 Hot fields (touched per substep, in inner loops)

Every field below appears on the path through the entry-barrier
motor walk and the exit-barrier break walk. They live within the
first cache line of their owning struct (`alignas(64)`) and are
read-mostly during the substep.

| Owner                                | Field                                                  | Why hot                                                                                                |
|--------------------------------------|--------------------------------------------------------|---------------------------------------------------------------------------------------------------------|
| `JointEndpoints` (ECS column)        | `joint_id` (`u32`), `kind` (`u8`)                      | Read at every entry / exit walk; both fields the discriminator for per-row branches.                    |
| `JointEndpoints` (ECS column)        | `body_a`, `body_b` (`u32 × 2`)                         | Read at admission and at break-trip (for the `BrokenEvent` payload + reverse-index evict).             |
| `JointMotor` (ECS column)            | `enabled` (`bool`), `target_value` (`f32`)             | Read at every entry barrier; cache-compared against last pushed value before forwarding to Jolt.        |
| `JointBreakThreshold` (ECS column)   | `max_force`, `max_torque` (`f32 × 2`)                  | Read at every exit barrier; compared against accumulated impulses.                                     |
| Per-archetype scratch                | `cached_target` (`f32`)                                | Read+written at every entry barrier (motor walk).                                                       |
| Per-archetype scratch                | `accum_normal_impulse`, `accum_friction_impulse` (`f32 × 2`) | Written at every exit barrier from Jolt; read in the same walk for the threshold compare.        |
| Per-archetype scratch                | `broken` (`bool`)                                      | Read at every exit barrier (skip-already-broken fast path); written at trip; read by post-walk despawn pass. |
| `JointReverseIndex`                  | `by_body_[body_id].empty()`                            | Read by the bodies-shapes `remove_body` gate; one `inplace_vector::empty()` per remove.            |

The `JointEndpoints` hot prefix (32 B) plus `JointMotor` /
`JointBreakThreshold` (16 B / 8 B respectively) all fit comfortably
in one cache line; the per-archetype scratch row's `accum_*` +
`broken` triple is `12 B` aligned-packed.

### 5.2 Cold fields (touched at construction, reload, snapshot)

| Owner                                | Field                                                  | Why cold                                                                                                |
|--------------------------------------|--------------------------------------------------------|---------------------------------------------------------------------------------------------------------|
| `JointEndpoints` (ECS column)        | `frame_a`, `frame_b` (`JointFrame × 2`)                | Read once at admission (the per-kind decoder builds Jolt's settings struct from them); per-substep walks do not read these. |
| `JointLimits` (ECS column)           | All six `f32` fields                                    | Read once at admission; per-substep walks do not read (limits are admission-only per §3.7 restriction). |
| `JointMotor` (ECS column)            | `max_force`, `damping`                                  | Read once at admission (Jolt's motor settings absorb them); per-substep walks read only `enabled` + `target_value`. |
| `JointIdAllocator`                   | `next_id_`, `id_to_entity_`, `free_list_`              | Mutated at allocate / release; per-substep walks do not touch.                                          |
| Per-archetype scratch                | `cached_ref` (Jolt constraint pointer)                 | Read at admission and at break-trip (for `JoltMiddleman::remove_constraint`); not per-substep.          |
| `JointReverseIndex`                  | `by_body_[body_id]` (the `fixed_vector` itself)        | Mutated at admission / remove / break-trip; the bodies-shapes gate reads `empty()` only (a one-load fast path). |

### 5.3 Layout enforcement

Build-time `static_assert`s on the public component layouts:

```cpp
// physics/include/glibre/physics/physics.hpp — already part of §5 facade
static_assert(sizeof(JointEndpoints)         <= 64, "JointEndpoints fits one cache line");
static_assert(offsetof(JointEndpoints, frame_a) >= 16,
              "JointEndpoints hot prefix (joint_id/kind/body_a/body_b) precedes the cold (frame) suffix");
static_assert(sizeof(JointLimits)            <= 32, "JointLimits is 6 × f32 + 8 B reserved padding");
static_assert(sizeof(JointMotor)             <= 16, "JointMotor fits the entry-barrier hot prefix");
static_assert(sizeof(JointBreakThreshold)    <= 8,  "JointBreakThreshold is 2 × f32");
static_assert(sizeof(JointBrokenEvent)       <= 16, "JointBrokenEvent fits one half cache line");
```

The reverse index's per-cell `glibre::inplace_vector<JointId, 4>` (the
`std::inplace_vector` P0843R14 polyfill from
`core/include/glibre/compat/inplace_vector.hpp`) is sized at 4 inline
`u32`s (16 B inline storage + 4 B size field = 20 B per cell, padded to
24 B by alignment); `max_bodies = 1024` → ~24 KiB index footprint.
Overflow beyond 4 elements promotes the cell value into the outer
`std::pmr::vector` element; this is permitted but rare under the MVP S1
fixture (~5–8 joints total in the ragdoll fixture, never > 4 per body).
Per `reviews/decisions/eastl-removal.md` §"eastl::fixed_vector": use the
polyfill until libc++ ships `std::inplace_vector` natively.

## 6. Concurrency

MVP runs every system on the **game-loop driver thread** (SPEC §6.6;
`reviews/decisions/perf-budget.md` Pipelined Frame Timing). The
cluster's concurrency surface is exhaustively small.

### 6.1 Phase-by-phase admissibility

| Phase | Cluster ops admitted in MVP                                                                                                       |
|-------|------------------------------------------------------------------------------------------------------------------------------------|
| 1 Input        | `add_joint` / `remove_joint` (input maps to gameplay-spawn intents, e.g. editor authoring). Read-only `JointReverseIndex::joint_count` for diagnostics. |
| 2 Logic        | Reserved slot; deferred body in MVP. Future gameplay-plugin systems will write `JointMotor.target_value` and may spawn / despawn joints here, ahead of phase 3's entry barrier. |
| 3 PhysicsFixed | **Mirror walks run inside the `physics-world` driver's substep loop** (§3.10). The cluster authors the per-joint walks; admission is implicit (the driver is the only legal caller). `add_joint` / `remove_joint` are admitted at substep entry **before** the entry barrier and at substep exit **after** the exit barrier (the driver brackets them); admission inside the in-flight window is refused with `physics::Error::SubstepEcsCommitInverted`. The break-detection despawn pass runs at the very end of the exit barrier (§3.9.2). |
| 4 Animation    | Reserved slot; deferred body in MVP.                                                                                                |
| 5 Transform    | No cluster reads.                                                                                                                  |
| 6 CullExtract  | No cluster reads.                                                                                                                  |
| 7 RenderSubmit | No cluster reads.                                                                                                                  |
| 8 HotReload    | Cluster is the **author** of restore-time `add_joint` calls during `glibre_plugin_register` (§8). The loader holds exclusive ownership; no other system runs.   |
| 9 Present      | No cluster reads.                                                                                                                  |

### 6.2 Read-only operations

May run in any phase 1, 2, 5, 6, 7, 9 (and 8 under loader exclusivity).
Read-only against the joint registry + reverse index; no exclusive
lock required.

- `JointReverseIndex::any_joint_references(body_id)` — used by the
  bodies-shapes `remove_body` gate; one `empty()` read per call.
- `JointReverseIndex::joint_count(body_id)` — diagnostic; the
  editor / profiler reads it.
- `JointIdAllocator::is_live(joint_id)` — used by sibling validation
  paths (e.g. the snapshot restore replay's per-row sanity check).

### 6.3 Read-write operations

Run only in phases 1, 2, 3 (substep entry / exit windows only), and
8 (hot-reload restore). Forbidden in phases 4–7 / 9 because those
phases' read paths would observe partial state.

- `PhysicsWorld::add_joint` — endpoint validity check + dispatcher
  (§3.4) + `JointIdAllocator::allocate` + middleman `create_constraint`
  + reverse-index `record` + per-archetype scratch insert. The first
  motor commit happens at the next substep entry; the first
  break-check happens at the next substep exit.
- `PhysicsWorld::remove_joint` — middleman `remove_constraint` +
  `JointIdAllocator::release` + reverse-index `evict` +
  per-archetype scratch erase. Total over a live `JointId`.
- Break-trip despawn (§3.9.2) — same as `remove_joint` but
  triggered by the cluster, not the caller; the
  `JointBrokenEvent` is emitted before the despawn so subscribers
  see the event with the entity still nominally addressable inside
  the same substep.

The cluster maintains no internal worker thread; all state is
driver-thread-local in MVP.

### 6.4 Memory ordering

Every public method on `PhysicsWorld` (the joints slice) is
`noexcept` and assumes single-threaded access (the game-loop driver
thread; SPEC §6.6). The reverse index's `by_body_` vector is mutated
only on the driver thread; cross-thread reads from a future
post-MVP parallel mirror seam will require either the
relaxed-atomic-pointer upgrade path (`physics-world-design.md` §6.4
note 2) or a per-cell read-write split (deferred).

The `JointIdAllocator::allocate` / `release` calls update two
`std::pmr::vector`s non-atomically; safe because phase 3 substep
barriers serialise all access through the driver thread.

### 6.5 Determinism guarantees

The cluster contributes four guarantees to the §4.2 invariant 3
"byte-equal across hosts" promise:

1. **`JointId` allocation order is wire-deterministic.** The
   allocator's output is a pure function of the call sequence
   (§3.5 invariant 2); the call sequence is determined by ECS
   materialisation order (which core guarantees per PHILOSOPHY §7).
   Two hosts running the same trace produce the same `JointId` for
   the same joint entity.
2. **Free-list pop is `JointId`-ascending.** §3.5 invariant 3
   sort-on-pop makes id reuse deterministic across hosts.
3. **Mirror walks are `JointId`-ascending.** Both the entry-barrier
   motor walk (§3.10.1) and the exit-barrier break walk (§3.10.2)
   sort by `JointId` ascending before walking; the despawn pass at
   §3.9.2 walks the trip set in `JointId`-ascending order so the
   `JointBrokenEvent` stream is byte-equal across hosts (§4.2
   invariant 8).
4. **Anchor frames are bit-exact across hosts.** Per §3.2 invariant
   4 + §7.1.3 invariant 5, the quaternion + position fields cross
   the `JointDescriptorRecord` and `PhysicsSnapshot` schemas via
   `std::bit_cast<u32>`; two hosts authoring the same descriptor
   produce byte-equal Jolt impulse trajectories.

The §6 mirror barriers' `BodyId`-ascending walk is the bodies-shapes
contribution; the joints-cluster contribution is the
`JointId`-ascending walk + the reverse-index reads in
`BodyId`-ascending order (the bodies-shapes gate reads it that way
already).

## 7. Persistence + ABI

The cluster's persistence surface is split between **physics-owned**
schemas (`JointDescriptorRecord`; the joint columns of
`PhysicsSnapshot`) and **sibling-owned** schemas
(`PhysicsConfigRecord`, `ShapeBlobRecord`, the body / shape columns
of `PhysicsSnapshot`). Authority is SPEC §7; this section documents
only the cluster's slice.

### 7.1 Schemas this cluster owns

#### 7.1.1 `JointDescriptorRecord` (SPEC §7.1.3)

The constraint topology + tuning record. Authored under
`data/schemas/physics/JointDescriptorRecord.fory`; FQN
`glibre.physics.JointDescriptorRecord`. Field roster + invariants are
SPEC §7.1.3; no field is added or removed by this design.

The cluster's role:

1. **Read** the record at `add_joint` (§3.4 dispatcher), via the
   value-typed `JointEndpoints` + companion-component arguments
   populated by the `data` codec or by gameplay code.
2. **Validate** the kind ordinal (§7.1.3 invariant 1), the endpoints
   resolve in the world (§7.1.3 invariant 2), and the anchor
   quaternions are unit-normalised (§7.1.3 invariant 4); failures
   return the corresponding `physics::Error` arm.
3. **Dispatch** by `kind` (§7.1.3 sealed sum) to the per-kind decode
   path (§3.4.1–§3.4.6) and produce a Jolt constraint via the
   middleman.
4. **Insert** the resolved `cached_ref` into the per-archetype
   scratch keyed by the freshly-allocated `JointId`; record the
   `(joint_id, body_a, body_b)` triple in the reverse index (§3.5).
5. **Author** the migration body for any `vN → vN+1` schema bump
   under `physics/src/migrations/migrate_JointDescriptorRecord_v<N>_to_v<N+1>.cpp`,
   per `reviews/decisions/hot-reload-protocol.md` §"Migrate Function
   Contract" + SPEC §7.2.3. The cluster ships **no** migration body
   for v1 — there is no v1→v2 chain in MVP.

#### 7.1.2 `PhysicsSnapshot` joint columns (SPEC §7.1.4)

The snapshot codec is the sibling `snapshot/` aggregate's
responsibility (SPEC §4.1.12, §7.1.4). The cluster contributes six
columns to the schema:

| `PhysicsSnapshot` field             | Source in this cluster                                                  | Notes                                                                |
|-------------------------------------|-------------------------------------------------------------------------|----------------------------------------------------------------------|
| `joint_ids` (tag 15)                | `JointIdAllocator::in_use_ids()` walked ascending                       | The capture order; restore replays in the same order.                |
| `joint_kinds` (tag 16)              | `JointEndpoints.kind` per joint                                         | `u8` ordinal of the §5 sealed sum.                                   |
| `joint_body_a` (tag 17)             | `JointEndpoints.body_a` per joint                                       | `u32` `BodyId` ordinal.                                              |
| `joint_body_b` (tag 18)             | `JointEndpoints.body_b` per joint                                       | `u32` `BodyId` ordinal.                                              |
| `joint_normal_impulses` (tag 19)    | per-archetype scratch `accum_normal_impulse` per joint                  | `f32`; `std::bit_cast<u32>` per the §7 determinism contract.         |
| `joint_friction_impulses` (tag 20)  | per-archetype scratch `accum_friction_impulse` per joint                | `f32`; same.                                                          |

These columns are **read** at `PhysicsWorld::snapshot()` (the
cluster passes them to the sibling codec); they are **written** at
`PhysicsWorld::restore()` via `add_joint(...)` replay in `joint_ids`
ascending order (§3.4 + §3.5 restore paths). Capture order is
`JointId` ascending (§3.5 invariant 3 + §7.1.4 invariant 5);
restore replays in the same order so the post-restore allocator state
is byte-equal.

The other columns (`SnapshotHeader.*`, `body_*`,
`body_shape_blob_hashes`) are owned by sibling aggregates and the
codec — this design treats them as opaque carrier bytes.

The schema does **not** carry the joint's anchor frames, kind-specific
limits, motor configuration, or break threshold — those persist via
the surviving `JointDescriptorRecord` ECS-component bytes (the kind /
endpoints / anchors are middleman-typed component bytes that survive
across reload per SPEC §8.2 row "`Joint` ECS entity"). The snapshot
columns carry only the **runtime** state that cannot be re-derived
from the descriptor: the accumulated impulses needed for warm-start
continuity across the reload boundary.

### 7.2 Migration rules

The cluster authors the migration bodies for `JointDescriptorRecord`
and contributes to the `PhysicsSnapshot` joint column migrations.
Per `reviews/decisions/fory-codegen.md` §"Migration Mechanic" and
SPEC §7.2 the bodies are pure functions taking `(const T_vN&,
T_vNplus1&, glibre::Arena&)` and returning `Result<void>`.

#### 7.2.1 `JointDescriptorRecord` `vN → vN+1`

Three sub-cases (SPEC §7.2.3):

- **Adding a new `JointKind` ordinal** (post-MVP `Spring` or
  `Generic6Dof`). Append-only at the ordinal end. The cluster ships
  a new decode case under §3.4 (per-kind row) plus a new TU under
  `joints/joint_kinds/<new_kind>.cpp`. The migration body is identity
  (older descriptors do not carry the new kind).
- **Adding a companion** (e.g. a future `has_drive_curve` bit plus
  payload). Same additive pattern: one new `has_*` bool tag at
  default false plus the payload tags at zero defaults. The cluster
  ships the corresponding entry-barrier walk extension; older
  descriptors decode to `has_drive_curve = false` and the world
  skips the companion (§4.1.7 invariant 2).
- **Removing a kind.** Treated as breaking. The cluster ships the
  migration body that either re-maps to a still-shipped kind
  (lossy, total — e.g. `Cone` → `SwingTwist` with zeroed
  twist-limits) or returns `physics::Error::JointKindUnsupported`
  for descriptors carrying the removed kind. The decision is
  per-bump.

For MVP the cluster ships **no** migration bodies — the schema is v1
with six kinds, and no bump is on the roadmap.

#### 7.2.2 `PhysicsSnapshot` joint column `vN → vN+1`

Three sub-cases (SPEC §7.2.4):

- **Adding a per-joint column.** Append a new `list<T>` field at a
  new tag with `since N+1` and a default; codegen emits the identity
  migration that fills the new column with the default at length
  matching `joint_ids`. The §7.1.4 invariant 5 column-length check
  enforces the fill.
- **Removing a column.** Tag reserved; never reused. The migration
  body drops the column from older snapshots (the dispatcher reads
  the old column, ignores it, and writes a snapshot without it).
  The cluster ships the body when the column is one it owns
  (tags 15–20).
- **Changing element type.** Treated as breaking. Migration body
  converts under the §7 bit-exact contract; narrowing (`f64 → f32`)
  is forbidden inside MVP (§7.2.4 case 6).

For MVP no bump is shipped; bodies arrive when v2 lands.

### 7.3 ABI hash sources this cluster participates in

The cluster is a host-side consumer of `glibre_types_abi_hash` from
`glibre-types.dylib` (per `fory-codegen.md` and `plugin-abi.md`); the
hash is computed over the **schema sources** plugins declare, not over
`Joint` itself. The cluster contributes bytes to the ABI hash via:

1. **`JointDescriptorRecord` schema source**
   (`data/schemas/physics/JointDescriptorRecord.fory`) — owned by the
   `data` context; one entry in the `glibre_types_abi_hash` digest
   input.
2. **`PhysicsSnapshot` joint columns** (tags 15–20 on the snapshot
   schema) — same.
3. **`JoltMiddleman` ABI hash** — owned by the `JoltMiddleman` sibling
   design (SPEC §4.1.13). The middleman's
   `glibre_jolt_abi_hash` exports cover the constraint-creation entry
   points (`create_point_constraint`, `create_hinge_constraint`,
   `create_slider_constraint`, `create_cone_constraint`,
   `create_distance_constraint`, `create_swing_twist_constraint`,
   `remove_constraint`, `set_motor_target`,
   `accumulated_normal_impulse`, `accumulated_friction_impulse`); a
   change to any of those bumps the hash. The cluster's `add_joint`
   path consults `JoltMiddleman::require_hash` indirectly through
   `PhysicsWorld::create`'s gate (`physics-world-design.md` §3.4.2
   step 2).

The cluster exports no public symbols of its own that contribute to
either hash; its ABI shape is the SPEC §5 facade.

### 7.4 Jolt as middleman seam — no Jolt types cross

SPEC §4.1.13 invariant 1 forbids Jolt-derived types from crossing the
plugin ABI except via `JoltMiddleman`. The cluster honours this
mechanically:

1. **`joints/joint.cpp` and the per-kind TUs include zero Jolt
   headers.** The `<Jolt/...>` headers are reachable from exactly one
   TU (`middleman/jolt_middleman.cpp`; SPEC §6.1 module rule 1). The
   pre-build CMake check (a `clang -E` scan that fails the build on
   any other TU referencing `Jolt/`) is the enforcement.
2. **`JoltConstraintRef` is opaque to this cluster.** The ref is a
   sibling-typed handle (a `void*` token wrapped in a strong type
   under `glibre::physics::detail::JoltConstraintRef`); the cluster
   stores it on the per-archetype scratch and never dereferences. The
   middleman's `remove_constraint(joint_id)` consumes it at break-trip
   and `remove_joint`; the middleman's `accumulated_*_impulse(joint_id)`
   accessors take `JointId` (not the ref) so the per-substep walk is
   ref-free.
3. **`JointEndpoints` / `JointLimits` / `JointMotor` /
   `JointBreakThreshold` / `JointBrokenEvent` carry no Jolt types.**
   All five are pure C++23 POD structures; no Jolt typedef leaks into
   any layout. The `BodyId` and `JointId` payloads are
   middleman-typed `Handle<Tag, u32>` (SPEC §5 preamble) but carry
   only a 32-bit ordinal — no Jolt pointer.
4. **The §5 facade `#error`-guards Jolt header inclusion.** The
   include guard at the top of `physics/include/glibre/physics/
   physics.hpp` (SPEC §5 preamble) forbids any caller TU that has
   already included a Jolt header from including the facade.

A schema bump that adds a Jolt-derived type to the cluster's ABI
**is not possible** in MVP — the §5 facade is locked. Post-MVP
additions (a new joint kind, a per-axis 6-DoF mode enum) bump the
`JoltMiddleman` ABI hash and are gated by it; the cluster's own
public ABI is unchanged.

### 7.5 What the cluster does NOT persist

To make the boundary explicit (SPEC §7.3 + sibling-design pattern):

| Artefact                                  | Why not persisted                                                                                                  |
|-------------------------------------------|--------------------------------------------------------------------------------------------------------------------|
| `JointBrokenEvent` payload                | Same-frame event; consumed by phases 5+ inside the emitting frame (§4.1.7 inv 4, §4.2 inv 8). Reclaimed at end of phase 8. |
| Per-archetype scratch `cached_ref` (Jolt constraint pointer) | Image-private to the outgoing dylib; rebuilt at hot-reload resume by replaying `add_joint` (§8.2). |
| Per-archetype scratch `cached_target` (motor target cache) | Substep-local; reborn at every entry barrier.                                                       |
| Per-archetype scratch `broken` (sticky bit) | Substep-local; either drains via despawn-pass within the same substep or is reset at next substep entry (a despawned joint cannot remain in the archetype). |
| `JointReverseIndex` private state         | Per-world; rebuilt at hot-reload resume by replaying `record(joint_id, body_a, body_b)` for every surviving joint in `JointId`-ascending order. |

These appear in the persistence surface only as **identifiers**
(`JointId`, `BodyId`) referenced from §7.1.

## 8. Hot-reload

Hot-reload semantics for the physics plugin are owned by SPEC §8;
this section states what **the joints cluster** must hold steady
across the swap, what `migrate(...)` requires of the cluster, and the
refusal cases this design contributes. Engine-wide concerns
(per-plugin atomicity, observer bus event shapes, error wrapping
rules, the `enqueue_hot_reload` E2E hook) are not re-stated here —
see SPEC §8 + `reviews/decisions/hot-reload-protocol.md`.

### 8.1 What survives the swap

Per SPEC §8.1 (hot-reload point — phase 8, never mid-frame) + SPEC
§8.2 (survival inventory), specialised to this cluster's primitives:

| Primitive (§3 ref)                          | Survives swap? | Mechanism                                                                                                                                                 |
|---------------------------------------------|----------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------|
| `JointEndpoints` ECS component bytes (§3.2) | **Yes** (middleman ECS storage) | The component is middleman-typed (`glibre.types.physics.JointEndpoints`); the loader's archetype-storage swap preserves the bytes verbatim — `joint_id`, `kind`, `body_a`, `body_b`, `frame_a`, `frame_b` all intact. |
| `JointLimits` ECS component bytes (§3.7)    | **Yes** (middleman ECS storage) | Same; six `f32` fields plus presence-tag survive.                                                                                                          |
| `JointMotor` ECS component bytes (§3.6)     | **Yes** (middleman ECS storage) | Same; `enabled`, `target_value`, `max_force`, `damping` survive verbatim.                                                                                  |
| `JointBreakThreshold` ECS component bytes (§3.8) | **Yes** (middleman ECS storage) | Same; `max_force`, `max_torque` survive.                                                                                                                   |
| `JointBrokenEvent` buffer entries           | **No** — drained on swap | Per SPEC §8.2 row "JointBrokenEvent buffer", events drain at end-of-phase-8 in normal operation; the swap drains one beat earlier so no stale event leaks. Joint entities that broke in frame N are already despawned (§4.1.7 inv 4), so no re-emit is needed. |
| `JointId` value (§3.5)                       | **Yes (re-issued)** | Bytes survive on the `JointEndpoints.joint_id` field in middleman ECS storage. The new plugin's `JointIdAllocator` reissues the same id by replaying `allocate(entity)` in `JointId`-ascending order from the carrier snapshot — §4.2 invariant 5 (extended to the joint axis) promises the issued id matches. |
| `JointIdAllocator` private state             | **No**          | Rebuilt by `restore_from_snapshot` (§3.5 method) by replaying the snapshot's `joint_ids` ascending walk. Post-restore `next_id_` and `free_list_` are byte-equal to pre-drain.                                  |
| `JointReverseIndex` private state            | **No**          | Rebuilt during the resume body's per-joint replay (§8.2.2). Each `add_joint` re-records the `(joint_id, body_a, body_b)` triple; the post-resume index is byte-equal to pre-drain because the replay order is `JointId`-ascending and the descriptor bytes survive.                                  |
| Per-archetype scratch `cached_ref` (Jolt constraint pointer) | **No**         | Rebuilt by `add_joint` replay in resume; Jolt's per-image `Constraint` table is rebuilt from scratch.                                                                                                                                                                                                |
| Per-archetype scratch `accum_*_impulse` cache | **Yes (snapshot)** | Captured into `PhysicsSnapshot.joint_normal_impulses` / `joint_friction_impulses` (tags 19–20) at drain; restored at resume via `JoltMiddleman::set_warm_start_impulses(joint_id, normal, friction)` so Jolt's first post-resume substep warm-starts at byte-equal impulses. This is the **single carrier** for joint runtime state across the swap (§3.2 collapse #9). |
| Jolt-internal `Constraint` instances + island graph | **No**     | Pointers into Jolt's `Constraint` table belong to the outgoing plugin's image. Rebuilt by the new plugin's `add_joint` replay against the same `JoltMiddleman::create_<kind>_constraint` paths (§3.4); post-resume warm-start impulses are reseeded from the snapshot.                                |

The mechanical rule: **anything middleman-typed survives; anything
Jolt-internal (resolved `JoltConstraintRef`, Jolt's per-constraint
solver state, the joint section of Jolt's island graph) does not and
is rebuilt from surviving state**. The single carrier across the swap
that combines surviving ECS bytes with the live Jolt residue
(accumulated impulses) is `PhysicsSnapshot` joint columns —
identical pattern to the bodies-shapes cluster (`bodies-shapes-design.md`
§8.1).

### 8.2 What `migrate(...)` must do

The cluster authors **two** classes of migration body when its
schemas bump:

1. **`JointDescriptorRecord` `vN → vN+1`** under
   `physics/src/migrations/migrate_JointDescriptorRecord_v<N>_to_v<N+1>.cpp`,
   per §7.2.1.
2. **`PhysicsSnapshot` joint column `vN → vN+1`** under
   `physics/src/migrations/migrate_PhysicsSnapshot_joint_columns_v<N>_to_v<N+1>.cpp`,
   per §7.2.2. (The sibling `snapshot/` cluster owns the header
   columns; the bodies-shapes cluster owns the body / shape
   columns.)

Both bodies are the standard pure migrate signature
(`reviews/decisions/hot-reload-protocol.md` §"Migrate Function
Contract"). In MVP no bump is shipped; bodies arrive when v2 lands.

The cluster also contributes the **drain + resume bodies** for the
joint registry, called by the loader at SPEC §8.3.1 (drain) and SPEC
§8.3.2 step 4 (resume).

#### 8.2.1 Drain body — joints cluster contribution

Called by the loader at SPEC §8.3.1. The cluster's slice runs as
part of the plugin-wide drain:

```text
joints::on_drain(snapshot& s):
  // The sibling snapshot/ aggregate's PhysicsWorld::snapshot() walks
  // every joint row in JointId-ascending order; this cluster's
  // contribution is filling the six joint columns (§7.1.2):
  //
  //   joint_ids               <- JointIdAllocator::in_use_ids() ascending
  //   joint_kinds             <- JointEndpoints.kind per joint
  //   joint_body_a            <- JointEndpoints.body_a per joint
  //   joint_body_b            <- JointEndpoints.body_b per joint
  //   joint_normal_impulses   <- per-archetype scratch accum_normal_impulse per joint
  //                              (last-substep value from the §3.10.2 walk)
  //   joint_friction_impulses <- per-archetype scratch accum_friction_impulse per joint
  //
  // No Jolt-side teardown happens here; the JoltSystemHandle's
  // destructor (owned by physics-world) frees Jolt's Constraint table
  // when the outgoing dylib unloads. The cluster's contribution is
  // captured-bytes-into-the-snapshot only.
  return {}
```

#### 8.2.2 Resume body — joints cluster contribution

Called by the loader at SPEC §8.3.2 step 4 (after middleman ABI gate,
world rebuild, shape re-intern, AND body restore — bodies must be
live before joints, since `add_joint` requires both endpoints to
resolve per §3.2 invariant 3). The cluster's slice runs in two
passes:

```text
joints::on_resume(snapshot const& s, JoltMiddleman& mm,
                  BodyIdAllocator& bodies,
                  JointIdAllocator& joints,
                  JointReverseIndex& reverse):

  // Step 4.A — Restore allocator state by replaying the JointId
  // ascending walk from the snapshot. After this call, allocator's
  // next_id_ and free_list_ are byte-equal to pre-drain.
  if joints.restore_from_snapshot(s.joint_rows()) is unexpected:
    return std::unexpected{ Error::BudgetExceeded }

  // Step 4.B — Replay add_joint in JointId-ascending order. The
  // snapshot's joint_ids column is the determinism input (§3.5
  // invariant 7); each id is reissued to the correct entity.
  for i in 0 .. len(s.joint_ids):
    jid    <- JointId{ s.joint_ids[i] }
    entity <- core.entity_for_joint(jid)                      // middleman ECS
    if entity is invalid:
      return std::unexpected{ Error::SnapshotJointIdUnresolved }
    ep     <- core.read_component<JointEndpoints>(entity)     // surviving bytes
    limits <- core.try_read_component<JointLimits>(entity)    // optional
    motor  <- core.try_read_component<JointMotor>(entity)     // optional
    brk    <- core.try_read_component<JointBreakThreshold>(entity) // optional

    // Endpoint resolution gate — §3.2 invariant 3 + §10.1.
    if not bodies.is_live(ep.body_a) or not bodies.is_live(ep.body_b):
      return std::unexpected{ Error::JointEndpointInvalid }

    // Reconstruct Jolt constraint via §3.4 dispatcher.
    cref <- add_joint_to_jolt(ep, limits, motor, brk, mm, bodies)
    if cref is unexpected:
      return std::unexpected{ cref.error() }

    // Re-seed Jolt's warm-start with the surviving accumulated impulses
    // so the first post-resume substep produces byte-equal solver
    // outputs (§4.2 invariant 3, §7.1.4 invariant 1).
    mm.set_warm_start_impulses(jid,
                                s.joint_normal_impulses[i],
                                s.joint_friction_impulses[i])

    // Re-record reverse-index entry (§3.5 invariant 5).
    reverse.record(jid, ep.body_a, ep.body_b)

    // Re-seed per-archetype scratch with the same impulses for the
    // first break-detection walk (§3.10.2) to compute correctly.
    archetype_scratch[jid].cached_ref               = cref
    archetype_scratch[jid].accum_normal_impulse     = s.joint_normal_impulses[i]
    archetype_scratch[jid].accum_friction_impulse   = s.joint_friction_impulses[i]
    archetype_scratch[jid].broken                   = false

  return {}
```

The total work is bounded by **O(joints) `add_joint` replays + O(joints)
warm-start reseeds + O(joints) reverse-index records**, plus a single
`JointIdAllocator::restore_from_snapshot` call. SPEC §8.3.2
§"total work" bound is preserved.

The resume body runs **after** the bodies-shapes resume body
(`bodies-shapes-design.md` §8.2.2) — body endpoints must be live
before a joint can resolve them. The loader's per-cluster
ordering matches the live-world dependency graph (shapes → bodies →
joints → contact / queries).

### 8.3 Refusal cases this cluster contributes

The cluster raises these arms during drain / resume; per SPEC §8.4
they roll up under `core::Error::HotReloadRefused` with one of the
protocol's existing inner causes (`PluginAbiHashMismatch`,
`PluginInitFailed`, `SchemaMigrationFailed`).

| Detection point                                                                                | `physics::Error` arm                | `core::Error` wrapper                          | SPEC §10.1 row |
|-----------------------------------------------------------------------------------------------|-------------------------------------|------------------------------------------------|----------------|
| Snapshot's `joint_ids` exceeds `PhysicsConfig.max_constraints`                                | `BudgetExceeded`                    | `HotReloadRefused { PluginInitFailed }`        | SPEC §10.1     |
| Snapshot row's `body_a` / `body_b` no longer resolves to a live `BodyId` after body-restore   | `JointEndpointInvalid`              | `HotReloadRefused { PluginInitFailed }`        | SPEC §10.1     |
| Snapshot row's `kind` is an ordinal absent from this build's sealed sum (newer-cook descriptor)| `JointKindUnsupported`              | `HotReloadRefused { SchemaMigrationFailed }`   | SPEC §10.1     |
| Snapshot row's `joint_id` does not resolve to a live ECS entity in the surviving world        | `SnapshotJointIdUnresolved` (analogous to `SnapshotBodyIdUnresolved`; see §12 OQ — pending SPEC §10.1 amendment) | `HotReloadRefused { PluginInitFailed }`        | (pending SPEC §10.1 amendment)     |

The cluster does **not** raise:

- `JoltMiddlemanHashMismatch` — handled by the `physics-world`
  cluster at resume step 1 (`physics-world-design.md` §8.3 row 1);
  the joints cluster runs after the gate has passed.
- `SnapshotSchemaMismatch` / `SnapshotDeserialiseFailed` — owned by
  the `snapshot/` and `physics-world` clusters; the joints cluster
  sees an already-decoded snapshot.
- `BodyStillReferencedByJoint` — this arm fires at the **bodies-shapes**
  `remove_body` call site, not at any joints-cluster path. The
  joints cluster owns the reverse index that the gate consults but
  the refusal itself is attributed to the bodies cluster
  (`bodies-shapes-design.md` §10.1). See §10 + §12 below for the
  reconciliation tracked by spike #908.

### 8.4 Self-reload refusal

`physics.dylib` is hot-reloadable; `core` and `glibre-types.dylib`
are not (SPEC §3.3, hot-reload-protocol §"Open Questions" #2). The
joints cluster therefore only participates as the *outgoing* or
*incoming* plugin in physics swaps; if `glibre-types` itself ever
changes (which would force a `JoltMiddleman` ABI hash change per
§4.1.13), the cluster's refusal is wrapped by the `physics-world`
cluster's `JoltMiddlemanHashMismatch` at resume step 1 and the
joints resume body never runs.

### 8.5 Render BLAS observer

Joints contribute nothing to the `PhysicsWorldReplaced` event arm
(`physics-world-design.md` §8.5) beyond the `joints_restored` count
the event payload carries (a derived value from the cluster's
`JointIdAllocator::live_count()` after resume). Render's BLAS
invalidation is keyed on body transforms, not joint topology;
the joints cluster has no direct subscriber.


## 9. Performance

Authority: `reviews/decisions/perf-budget.md` Per-Context Budget Table
assigns physics the cell **2.00 ms CPU sim + 0.00 ms CPU submit + n/a
GPU + 128 MiB heap**. The SPEC §9.2 per-stage split gives the **Jolt
step + ECS↔Jolt mirror + queries** trio its sub-budgets; this design
refines the **per-joint composition** of those rows so the §11
`BENCHMARK_CELL`s have well-defined targets.

This design does **not widen** any cell or sub-budget; deviations
require a `perf-budget.md` amendment spike, not an in-place edit.

### 9.1 Cited cells (verbatim from `perf-budget.md` and SPEC §9)

| Axis                  | Budget          | Source                                                    |
|-----------------------|-----------------|-----------------------------------------------------------|
| CPU sim (cluster contributions) | inside §9.2 rows | SPEC §9.2 — Jolt step (1.50 ms) absorbs joint-solve cost; ECS↔Jolt mirror (0.30 ms) absorbs the per-substep motor + break walks |
| CPU submit            | 0.00 ms         | physics records no GPU work                                |
| GPU                   | n/a             | physics owns no Metal heaps or encoders (SPEC §3.3)        |
| Heap (cluster slice)  | **~16 MiB** of the 64 MiB Jolt body+constraint pool | SPEC §9.3 — Jolt constraint half (~16 of 64 MiB; bodies take ~48 of 64 per `bodies-shapes-design.md` §9.3) |
| Phase ownership       | (within phase 3) | the `physics-world` cluster owns phase 3; this cluster contributes the per-joint walks inside |

### 9.2 Cluster sub-budget within the §9.2 ECS↔Jolt mirror + Jolt-step rows

The §9.2 mirror row decomposes into two barriers per substep × two
substeps × ~30 active bodies (S1 fixture). For joints the S1 fixture
(`physics-world-design.md` §9 + `bodies-shapes-design.md` §9) carries
**~5–8 joints** (a 5-joint ragdoll fixture used by §11 + the §11
acceptance fixtures cited in SPEC §8.6 — "Tower fixture (50 boxes,
5 joints, 2 triggers)"). The cluster's contribution per primitive is
bounded as follows; the numbers fit **inside** the existing rows.

| Cluster slice                                              | Inside row                | Per substep | Per frame (2 substeps × 2 barriers) | Dominant operation                                                                                       |
|------------------------------------------------------------|---------------------------|-------------|--------------------------------------|-----------------------------------------------------------------------------------------------------------|
| `JointId`-ascending sort over scratch span (entry + exit)  | ECS↔Jolt mirror (0.30 ms) | < 0.005 ms  | < 0.020 ms                           | `std::ranges::sort` over ~8 `u32`; `O(n log n)` with `n ≤ 8`. Reuses the same per-substep arena pattern as the bodies sort. Per `reviews/decisions/eastl-removal.md` R4 — range algorithms replace pre-C++20 iterator forms. |
| Entry barrier — motor target push                          | ECS↔Jolt mirror (0.30 ms) | < 0.005 ms  | < 0.010 ms                           | Per-`JointMotor`-row enabled-bit + cache compare + middleman `set_motor_target` if dirty; few in S1 (~1–2 motors). |
| Exit barrier — accumulated-impulse read                    | ECS↔Jolt mirror (0.30 ms) | < 0.005 ms  | < 0.010 ms                           | One middleman call per `JointBreakThreshold`-archetype row; 2 × `f32` read per call.                     |
| Exit barrier — break-threshold compare                     | ECS↔Jolt mirror (0.30 ms) | < 0.001 ms  | < 0.005 ms                           | One `f32` divide + two `f32` compares per row; no Jolt traffic.                                           |
| Break-trip despawn pass (rare)                             | ECS↔Jolt mirror (0.30 ms) | < 0.005 ms  | < 0.005 ms (steady-state ~0)         | Triggered on threshold trip only; per-trip cost is one middleman `remove_constraint` + one ECS despawn + one reverse-index evict + one `JointId` release. Not in steady-state. |
| Joint Jolt-solve cost (per joint, all rows × all iterations) | Jolt step (1.50 ms)     | (Jolt-internal) | per `physics-world-design.md` §9.5 | The cluster does not author the solver; cost is folded into the 1.50 ms Jolt-step row. R-4.3.NF1's "5 000 rows / ms" maps to the §11 fixture: ~8 joints × ~5 rows / joint × 10 iterations = ~400 row-solves / substep, well below the 7 500-row capacity. |
| **Cluster sub-total per frame (mirror row)**               |                           |             | **< 0.050 ms**                       | Steady-state two-substep frame, S1 fixture. Comfortably fits inside the 0.30 ms mirror row.              |

The bodies-shapes cluster's contribution (§9.2 in
`bodies-shapes-design.md`) is < 0.105 ms; the joints cluster adds <
0.050 ms; the contact + trigger drain (sibling `contact/`) absorbs
the remaining ~0.145 ms inside the 0.30 ms mirror row. The cluster
is one rate-limiting term but not the dominant one.

### 9.3 Heap composition inside the 128 MiB ceiling

The cluster's resident allocations under `ContextTag::physics`
contribute to the SPEC §9.3 pool decomposition. Cluster's slice:

| Pool (SPEC §9.3 row)                     | Cluster's contribution                                                     |
|------------------------------------------|----------------------------------------------------------------------------|
| Jolt body + constraint pools (64 MiB)    | The constraint half: Jolt's `ContactConstraintManager`'s constraint subset + per-joint accumulated-impulse caches + the `JoltConstraintRef` table. Sized for `PhysicsConfig::max_constraints = 256`; ~16 MiB at the upper bound. The body half is the bodies-shapes contribution. |
| `ShapeBlob` hash table (32 MiB)          | None — owned by sibling `shapes/`.                                          |
| Snapshot scratch arena (16 MiB)          | None — owned by sibling `snapshot/`.                                        |
| Query result buffers (8 MiB)             | None — owned by sibling `queries/`.                                         |
| Contact event ring (8 MiB)               | The cluster's `JointBrokenEvent` entries ride the same ring (the ring is a shared per-frame ECS event buffer per SPEC §9.3); typical break-event count is 0–1 per frame, well below the ring's headroom. |
| **Joint registry + reverse index** (in-line within the 64 MiB Jolt-pool sub-share) | `JointIdAllocator` (~24 B + `id_to_entity_` vector ~1 KiB at `max_constraints = 256`) + `JointReverseIndex.by_body_` (~24 KiB at `max_bodies = 1024` × 24 B per cell) — all under the 16 MiB constraint-pool sub-share. |

The cluster's per-joint mirror cache (§3.2) lives on the **phase-3
transient arena** (4 MiB ceiling, exempt from the 128 MiB cell per
`perf-budget.md` Allocator Rule #4). The arena drains by phase 9;
cluster cache rows are reborn at every phase 3 entry barrier — same
pattern as the bodies-shapes cluster.

### 9.4 Allocator rules

Per SPEC §9.4 every allocation under `physics/src/joints/**` is
stamped with `ContextTag::physics` at the allocator-handle level
(`perf-budget.md` Allocator Rule #1). The cluster makes only three
distinct allocations:

1. **`JointIdAllocator` itself** — one `std::make_unique` at
   `PhysicsWorld::create` (broker through `physics-world` cluster).
   ~24 B + the `id_to_entity_` vector's bytes. The `std::pmr::vector`
   fields inside the allocator carry `&physics_mr_` (the
   `glibre::PerContextAllocatorResource` for `ContextTag::physics`),
   forwarded at construction per `reviews/decisions/eastl-removal.md`
   §3 lifetime contract. See PHILOSOPHY.md §11 superseded notice.
2. **`JointReverseIndex` itself** — one `std::make_unique` at the
   same broker. ~24 B + the `by_body_` vector at
   `max_bodies × sizeof(glibre::inplace_vector<JointId, 4>)`. Same PMR
   resource threading.
3. **Per-`add_joint` Jolt `Constraint*`** — one allocation per joint
   that crosses the table. Tagged inside the middleman per SPEC §9.4
   rule 5 ("Jolt's allocator is wrapped"); counts against the 16 MiB
   constraint-pool sub-share.

Reverse-index spillover (a cell whose live joint count exceeds 4)
promotes the cell to a heap-grown `std::pmr::vector<JointId>` element; this is rare under the
MVP S1 fixture (~5–8 joints total, never > 4 per body) but is a
permitted allocation.

Under `GLIBRE_ALLOC_STRICT=1`, an allocation that would push live
`ContextTag::physics` bytes above 128 MiB returns `Result<>` with
`core::Error::OutOfBudget`. The cluster's `add_joint` propagates via
the monadic chain; failure to handle aborts with the diagnostic dump.

### 9.5 GPU and submit halves

The cluster consumes **no** GPU budget and **no** CPU submit budget.
Phase 3 finishes before phase 6's `RenderFrame` extract begins; the
cluster writes no GPU resources of its own. The break-detection
event surface (`JointBrokenEvent`) is consumed by post-MVP
gameplay / destruction layers in phase 5+ via the standard ECS
event-component reader path; render does not subscribe.

### 9.6 CI gate hooks the cluster owns

Per SPEC §9.6, the per-context CI gate requires per-row benchmarks.
The cluster contributes (one Catch2 case per row; PR fails on any
breach):

| Stage                                                           | `BENCHMARK_CELL` test name                                          | CPU ceiling | Source                                       |
|-----------------------------------------------------------------|----------------------------------------------------------------------|-------------|----------------------------------------------|
| Cluster-side mirror walk (motor + break composite)              | `physics/joints: ecs_jolt_mirror_two_substep`                       | 0.05 ms     | §9.2 cluster sub-total                        |
| `add_joint` for each kind (admission cost)                      | `physics/joints: add_joint_each_kind`                               | 0.020 ms / kind | §3.4 per-kind decoder                       |
| `JointIdAllocator::allocate` + `release` round-trip             | `physics/joints: joint_id_allocator_alloc_release_round_trip`       | 0.001 ms    | §3.5 free-list policy                        |
| `JointReverseIndex::any_joint_references` lookup                | `physics/joints: reverse_index_any_joint_references_lookup`         | 0.0005 ms   | §3.5 invariant 6 (one `empty()` read)        |
| Break-trip despawn pass (single joint)                          | `physics/joints: break_trip_despawn_single_joint`                   | 0.010 ms    | §3.9.2                                       |
| Per-archetype joint scratch rebuild at phase-3 entry            | `physics/joints: archetype_scratch_rebuild_8_joints`                | 0.005 ms    | §3.2 cache fields + §3.10.1                  |

These rows live under `tests/physics/joints/`. The cluster does not
own the SPEC §9.6.1 mandated row directly — that row
(`physics/world: phase3_total_two_substep`) is the composite; the
cluster's contribution rolls up into it.

The §9.6.2 heap-residency assertions the cluster owns:

| Pool                       | Heap-residency test name                              | Ceiling   |
|----------------------------|--------------------------------------------------------|-----------|
| Jolt constraint pool half  | `physics/joints: heap_jolt_constraint_pool`           | 16 MiB    |
| Joint registry + reverse index | `physics/joints: heap_registry_and_reverse_index`  | 64 KiB    |

The constraint-pool ceiling is added so the cluster's share of the
64 MiB Jolt-pool sub-share is independently tracked (the bodies-pool
half is `bodies-shapes-design.md` §9.6's `physics/bodies:
heap_jolt_body_pool`). The registry-and-index row is added because
the reverse-index footprint scales with `max_bodies` — a future
sensor-pack scenario with ~10⁵ bodies + ~10² joints could push the
index past 1 MiB and is worth tracking.

## 10. Failure modes

The cluster raises a closed subset of the `physics::Error` enum
(SPEC §5; SPEC §10.1 documents each arm authoritatively). Each arm
below names the trigger condition specific to **`JointEndpoints` /
`JointId` / `JointKind` / `JointLimits` / `JointMotor` /
`JointBreakThreshold`**, the recovery posture, the log severity, and
the SPEC §10.1 row that owns the arm. This design adds the
per-primitive trigger detail the implementer needs.

The arms are split by cluster primitive (§3 references). All arms
are documented authoritatively in SPEC §10.1; this section refines
the per-primitive triggers and routes.

### 10.1 `Joint` lifecycle arms (§3.2, §3.5)

| Arm                              | Trigger (in this cluster)                                                                              | Recovery        | Severity (default) | SPEC ref      |
|----------------------------------|---------------------------------------------------------------------------------------------------------|-----------------|--------------------|---------------|
| `JointEndpointInvalid`           | `add_joint` called with `body_a` / `body_b` as an authored-garbage handle: zero-init, cross-world, or a handle that was never live in this world. Also fires when `body_a == body_b` (self-loop). Detected by §3.4 dispatcher step 1 (the `was_ever_live` branch) before any Jolt allocation. Per `reviews/decisions/physics-error-arm-joint-body-reconciliation.md` Alt C — "authored-garbage handle" case only; the timing-race case routes to `JointDanglingEndpoint` below. | Caller fixes the gameplay-code path that produced the bad handle (re-orders body / joint spawn, or validates handle provenance before `add_joint`). Physics **refuses** the joint add; no Jolt constraint is allocated. | `error` | SPEC §10.1 |
| `JointDanglingEndpoint`          | `add_joint` called with `body_a` / `body_b` where the endpoint `BodyId` was once valid in this world but the body has been despawned between author-time and `add_joint` commit (deferred-command timing race). The handle is non-zero and same-world but no longer resolves in `BodyIdAllocator::is_live`. Detected by §3.4 dispatcher step 1 (the `was_ever_live && !is_live` branch) before any Jolt allocation. Per Alt C — canonical construction site for this arm is `add_joint` (joints cluster); see reconciliation record. | Caller re-fetches the live endpoint `BodyId` and re-issues `add_joint`. Physics **refuses** the joint add; no Jolt constraint is allocated. | `warn` | SPEC §10.1 |
| `JointKindUnsupported`           | `add_joint` called with a `JointKind` ordinal absent from this build's sealed sum (post-MVP `Spring` / `Generic6Dof` requested by a newer authoring tool); also surfaces from snapshot restore against an older build (§8.3 row 3). Detected by §3.4 dispatcher's switch's default arm. | Operator rebuilds the physics plugin with the missing joint kind compiled in (an ABI bump, PHILOSOPHY §9). Physics **refuses** the joint add or the snapshot restore. | `error` | SPEC §10.1 |
| `JointBroken`                    | Mutation attempted on a joint whose `broken` sticky bit is set (the threshold tripped earlier in this substep but the despawn pass has not yet drained the row). Surfaces from `set_joint_motor`, `set_joint_limits`, and friends. Detected by §3.8 invariant 4. | Caller checks `is_joint_broken(jid)` before mutating, or removes the broken joint and adds a fresh one. Physics **refuses** the mutation; the broken joint stays broken. | `info` | SPEC §10.1 |
| `BudgetExceeded`                 | `add_joint` past `PhysicsConfig::max_constraints` (§3.5 invariant 4). Surfaces from `JointIdAllocator::allocate`. Also surfaces during hot-reload restore if the snapshot's joint count exceeds the new world's `max_constraints` (§8.2.2). | Caller raises budget on fresh world; physics refuses. | `error` | SPEC §10.1 |

### 10.2 `JointEndpoints` companion-validity arms (§3.2 invariant 4, §3.6, §3.7)

| Arm                              | Trigger (in this cluster)                                                                              | Recovery        | Severity (default) | SPEC ref      |
|----------------------------------|---------------------------------------------------------------------------------------------------------|-----------------|--------------------|---------------|
| `ConfigInvalid`                  | (a) `JointFrame::rotation` not unit-normalised at admission (`|q| ∉ [1 − 1e−6, 1 + 1e−6]`; §3.2 invariant 4). (b) `JointMotor.enabled = true` on a kind that rejects motors (Point / Cone / Distance; §3.6 invariant 3). (c) Per-kind range invalid: `JointLimits.lower > upper` for Hinge / Slider / Distance, `swing_y` outside `[0, π]` for Cone / SwingTwist, `twist_low > twist_high` for SwingTwist (§3.4 per-kind rows). (d) `JointKind::Cone` admitted without `JointLimits` (a cone joint requires its half-angle; §3.4.4 admission gate). (e) `Collider.density_override < 0` cross-fired into joint admission (rare; only when the auto-inertia derive runs synchronously with joint add — unlikely path, listed for completeness). | Caller fixes the corresponding companion / anchor field. Physics **refuses** the joint add; no Jolt constraint is allocated. | `error` | SPEC §10.1 |

### 10.3 Substep-mirror arms (§3.10)

| Arm                              | Trigger (in this cluster)                                                                              | Recovery        | Severity (default) | SPEC ref      |
|----------------------------------|---------------------------------------------------------------------------------------------------------|-----------------|--------------------|---------------|
| `SubstepEcsCommitInverted`       | A debug-build assertion in the entry / exit barrier walk detected ECS↔Jolt cross-traffic between the two barriers. The `physics-world` driver's flag triple is what the assertion reads (`physics-world-design.md` §5.1); the typed arm fires when promoted under `Hard`. Joints contribute the motor-target walk (entry) and the break-detection walk (exit); a violation in either fires the same arm. | Programming error in a §3.10 mirror seam refactor; abort substep | `error` | SPEC §10.1 |

### 10.4 Snapshot / restore arms (§7.1.2, §8)

| Arm                              | Trigger (in this cluster)                                                                              | Recovery        | Severity (default) | SPEC ref      |
|----------------------------------|---------------------------------------------------------------------------------------------------------|-----------------|--------------------|---------------|
| `SnapshotJointIdUnresolved`      | Snapshot's `joint_ids[i]` does not resolve to a live ECS entity in the surviving world (the joint entity was despawned outside physics's despawn pass, e.g. by an editor undo crossing the swap). Detected during §8.2.2 step 4.B. **Pending SPEC §10.1 amendment** — the bodies-cluster analog `SnapshotBodyIdUnresolved` exists; the joint analog must be added to the §5 enum + SPEC §10.1 in the same implementation plan that introduces `physics/include/glibre/physics/error.hpp`. The §8.3 row above flags the same dependency. | Operator captures snapshot from a clean world or accepts fresh world. Physics refuses the resume. | `error` (CI Hard) / `warn` (shipping SoftWarn) | (pending SPEC §10.1 amendment; tracked in §12) |
| `JointEndpointInvalid` (restore-time variant) | Snapshot row's `body_a` / `body_b` does not resolve to a live `BodyId` after the bodies-shapes resume body completed (the body was lost across the swap; e.g. snapshot referenced a body whose `RigidBody` component was removed before drain captured the snapshot). Detected during §8.2.2 step 4.B. | Same as fresh-spawn `JointEndpointInvalid`; the operator restores from a snapshot whose joint endpoints all resolve. | `error` | SPEC §10.1 |

### 10.5 Routed arms (cluster does not detect)

The cluster forwards these arms from sibling aggregates. Listed for
completeness:

- `BodyNotFound`, `BodyMotionTypeImmutable`, `BodyStillReferencedByJoint`,
  `ColliderShapeRequired`, `ShapeBlobMalformed`,
  `ShapeBlobVersionUnsupported`, `ShapeHandleStale`, `ShapeBlobMissing` —
  sibling `bodies/` + `shapes/`.
- `QueryDuringStep`, `QueryFilterInvalid` — sibling `queries/`.
- `ConfigInvalid` (world-init form), `WorldNotInitialised`,
  `WorldAlreadyInitialised`, `JoltMiddlemanHashMismatch`,
  `JoltMiddlemanUnavailable`, `SnapshotSchemaMismatch`,
  `SnapshotDeserialiseFailed`, `HotReloadStateUnmigratable`,
  `StepCalledOutsidePhase3`, `AccumulatorClampExceeded`,
  `NumericalInstabilityDetected`, `DeterminismCheckFailed` — sibling
  `physics-world` cluster (`physics-world-design.md` §10).

Particularly load-bearing: **`BodyStillReferencedByJoint` is the
bodies-cluster's arm, not this cluster's arm.** Per the resolved spike
#908 (`reviews/decisions/physics-error-arm-joint-body-reconciliation.md`
Alt C, merged 2026-05-09), the bodies-shapes `remove_body(BodyId)` path
consults this cluster's `JointReverseIndex::any_joint_references(body_id)`
and emits `BodyStillReferencedByJoint` on its own behalf
(`bodies-shapes-design.md` §10.1). The joints cluster owns the index
plus **three** `add_joint`-site arms with distinct severities and
recovery rungs:

| Arm                       | Ownership      | Canonical construction site  | Recovery rung                              | Severity |
|---------------------------|----------------|------------------------------|--------------------------------------------|----------|
| `JointEndpointInvalid`    | joints cluster | `add_joint` (§3.4 step 1)    | Fix authored garbage handle                | `error`  |
| `JointDanglingEndpoint`   | joints cluster | `add_joint` (§3.4 step 1)    | Re-fetch live `BodyId` and re-issue        | `warn`   |
| `BodyStillReferencedByJoint` | bodies cluster (routed) | `remove_body` (bodies cluster) | Remove joint(s) first, then remove body | `warn` |

Three arms, three detection sites, three operationally distinct
recovery rungs — per the Alt C resolution. `BodyStillReferencedByJoint`
is listed here only as a routed-from-sibling arm; it is emitted and
owned by the bodies aggregate exclusively.

### 10.6 Caller-side recovery posture

Per SPEC §10.2 / §10.4, callers handle each arm at exactly one
boundary. For cluster arms specifically:

- **Gameplay / scripting plugins** — `add_joint`, `remove_joint`
  errors propagate up to the schedule's per-system error wrapper,
  which logs once at the arm's severity and the calling system
  continues. A failed `add_joint` does not abort the frame; the
  joint entity is left without a Jolt constraint (gameplay code is
  expected to despawn the entity in a follow-up tick if the joint
  cannot recover).
- **Editor tools** — same propagation, plus the editor's inspector
  surfaces the failure as a content-pipeline drift marker on the
  affected joint entity. The `BodyStillReferencedByJoint` arm
  surfacing on a `remove_body` action presents in the editor as
  "this body has joints; remove the joints first" with a list link
  populated by the editor calling `JointReverseIndex::joint_count`.
- **Hot-reload loader** — the four cluster-contributed refusal arms
  (§8.3) wrap into `core::Error::HotReloadRefused` with the matching
  inner cause. Refusal logs at `warn` per the protocol; the
  previous-good plugin keeps stepping.

### 10.7 Determinism obligation

Every non-hot-reload arm above must fire **byte-equal across runs**
on byte-equal inputs (PHILOSOPHY §7). Specifically: the
`BudgetExceeded` threshold compares against
`PhysicsConfig::max_constraints` which is bit-exact across hosts
(SPEC §6.4 R4); the `JointEndpointInvalid` detection is a
`BodyIdAllocator::is_live` query with deterministic state; the
quaternion unit-norm check uses `<cmath>::fabs` which is portable;
`JointKindUnsupported` is a `u8` enum compare. The §11 acceptance
test `physics/joints: error_arms_byte_equal_across_runs` asserts
the error stream from a fixed input trace is byte-equal across two
consecutive runs.

## 11. Test plan

Unit + integration + perf tests required to validate the §1–§10
invariants. Catch2 test names are stable; the SPEC §11 `[STORY]`
parent issue is named in the right column where one exists.

### 11.1 Unit tests (one Catch2 case per row)

Lives under `tests/physics/joints/`.

| Test name                                                                        | What it asserts                                                                                                                                | §-ref          | Story |
|----------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------|----------------|-------|
| `physics/joints: add_joint_allocates_joint_id`                                   | `add_joint(...)` returns a `JointId` whose `valid()` is true and whose `raw()` is the next id from the allocator.                              | §3.5, §4.1.7   | #435  |
| `physics/joints: add_joint_each_kind_constructs_jolt_constraint`                 | For each of Point / Hinge / Slider / Cone / Distance / SwingTwist, `add_joint` constructs a Jolt constraint via the corresponding decoder.     | §3.4.1–§3.4.6  | #435  |
| `physics/joints: add_joint_refuses_dead_endpoint`                                | `add_joint` with `body_a` from a body that was once live but has since been despawned → `JointDanglingEndpoint` (`warn`). Distinct from the garbage-handle case: the `BodyId` was once valid in this world. Per Alt C (`reviews/decisions/physics-error-arm-joint-body-reconciliation.md`). | §3.2 inv 3, §10.1 | #437 |
| `physics/joints: add_joint_refuses_self_loop`                                    | `add_joint` with `body_a == body_b` → `JointEndpointInvalid` (`error`). Programming bug; not a timing race.                                    | §3.2 inv 5, §10.1 | #437 |
| `physics/joints: add_joint_refuses_cross_world_endpoint`                         | `add_joint` with `body_a` from world A used against world B → `JointEndpointInvalid` (`error`). The per-world allocator does not recognise the id (never-live in this world — authored-garbage case). | §3.2 inv 3, §10.1 | #437 |
| `physics/joints: add_joint_refuses_zero_init_endpoint`                           | `add_joint` with `body_a = BodyId{}` (zero-init / null sentinel) → `JointEndpointInvalid` (`error`). Authored-garbage case; `was_ever_live` is false. | §3.2 inv 3, §10.1 | #437 |
| `physics/joints: add_joint_refuses_non_unit_anchor_quaternion`                   | `add_joint` with `frame_a.rotation = Quat{0.5, 0, 0, 0.5}` (`|q| ≈ 0.707`) → `ConfigInvalid`.                                                  | §3.2 inv 4, §7.1.3 inv 4, §10.1 | (no story; CI invariant) |
| `physics/joints: add_joint_refuses_motor_on_point`                               | `add_joint(Point, ..., motor.enabled = true)` → `ConfigInvalid` (Point joints reject motors per §3.4.1).                                       | §3.4.1, §3.6 inv 3 | (CI) |
| `physics/joints: add_joint_refuses_motor_on_cone`                                | `add_joint(Cone, ..., motor.enabled = true)` → `ConfigInvalid` (Cone joints reject motors per §3.4.4).                                          | §3.4.4         | (CI) |
| `physics/joints: add_joint_refuses_motor_on_distance`                            | `add_joint(Distance, ..., motor.enabled = true)` → `ConfigInvalid`.                                                                            | §3.4.5         | (CI) |
| `physics/joints: add_joint_accepts_motor_on_hinge_slider_swingtwist`             | `add_joint(Hinge, ...)` / `Slider` / `SwingTwist` with motor enabled succeeds; the per-kind decoder calls `JoltMiddleman::set_motor_target` once at admission. | §3.4.2 / §3.4.3 / §3.4.6, §3.6 | (CI) |
| `physics/joints: add_joint_refuses_cone_without_limits`                          | `add_joint(Cone, ..., limits = nullptr)` → `ConfigInvalid` (Cone requires its half-angle).                                                      | §3.4.4         | (CI)  |
| `physics/joints: add_joint_refuses_inverted_hinge_limits`                        | `add_joint(Hinge, ..., limits.lower = 1.0, limits.upper = -1.0)` → `ConfigInvalid` (`lower > upper`).                                          | §3.4.2, §10.1 | (CI)  |
| `physics/joints: add_joint_refuses_swing_y_out_of_range`                         | `add_joint(Cone, ..., limits.swing_y = 4.0)` (> π) → `ConfigInvalid`.                                                                          | §3.4.4         | (CI)  |
| `physics/joints: add_joint_refuses_unknown_kind_ordinal`                         | `add_joint` with `kind = JointKind{99}` → `JointKindUnsupported`.                                                                              | §3.4 inv 1, §10.1 | (CI) |
| `physics/joints: kind_is_immutable`                                              | Mutating `JointEndpoints.kind` post-create has no effect at the next entry barrier; the Jolt constraint kind is unchanged.                      | §3.2 inv 2     | (CI)  |
| `physics/joints: remove_joint_releases_joint_id`                                  | `remove_joint` then `add_joint` reissues the same `JointId` (free-list reuse, sort-on-pop).                                                    | §3.5 inv 3      | #435  |
| `physics/joints: joint_id_zero_is_invalid_sentinel`                              | `JointId{}.valid() == false`; `JointId{0}.raw() == 0`; allocator never returns id 0.                                                          | §3.5 inv 1      | (CI)  |
| `physics/joints: joint_id_allocation_order_is_ecs_materialisation_order`         | Spawn joint entities A, B, C in that order; assert `JointId(A).raw() < JointId(B).raw() < JointId(C).raw()` regardless of host.                | §3.5 inv 2      | #435  |
| `physics/joints: joint_id_free_list_pop_is_ascending`                             | Spawn ids 1..10; release 5 then 3 then 7; next allocate returns 3 (smallest in free list).                                                     | §3.5 inv 3      | (CI)  |
| `physics/joints: joint_id_budget_exceeded`                                       | `max_constraints = 4`; `add_joint × 5` → fifth call returns `BudgetExceeded`.                                                                  | §3.5 inv 4, §10.1 | (CI) |
| `physics/joints: reverse_index_records_on_add`                                   | After `add_joint(j, body_a, body_b)`, `reverse.any_joint_references(body_a) == true` AND `reverse.any_joint_references(body_b) == true`.       | §3.5 inv 5     | #437  |
| `physics/joints: reverse_index_evicts_on_remove`                                 | After `remove_joint(j)`, `reverse.any_joint_references(body_a)` returns false (assuming j was the only joint).                                  | §3.5 inv 5     | #437  |
| `physics/joints: reverse_index_evicts_on_break_trip`                             | After break-trip despawn, `reverse.any_joint_references(body_a)` returns false.                                                                | §3.9.2, §3.5 inv 5 | (CI) |
| `physics/joints: reverse_index_count_is_total_over_live_joints`                  | For random sequences of `add_joint` / `remove_joint`, `Σ reverse.joint_count(b) == 2 × live_joint_count` after every operation.                | §3.5 inv 5     | (CI)  |
| `physics/joints: motor_target_pushed_at_entry_barrier`                           | Set `JointMotor.target_value` then advance; the next entry barrier calls `JoltMiddleman::set_motor_target` once with the new value.            | §3.6, §3.10.1  | #435  |
| `physics/joints: motor_target_cache_skips_unchanged`                             | Two consecutive substeps with the same `target_value` → only the first calls `set_motor_target`; the second skips (cache hit).                  | §3.6, §3.10.1  | (CI)  |
| `physics/joints: motor_companion_addable_mid_run`                                | A joint without `JointMotor` runs passive; adding the component mid-run takes effect at the next entry barrier (§4.1.7 inv 2).                  | §3.6 inv 2     | (CI)  |
| `physics/joints: limit_companion_immutable_mid_run`                              | Mutating `JointLimits` mid-run has no effect on solver behaviour until despawn + re-add; a debug-build telemetry counter increments.            | §3.7 inv 2     | (CI)  |
| `physics/joints: break_threshold_trips_emits_event`                              | Apply force exceeding `JointBreakThreshold.max_force` to a hinge; the next exit barrier emits `JointBrokenEvent` with `applied_force > max_force`. | §3.9, §4.1.7 inv 4 | #435  |
| `physics/joints: break_threshold_trip_despawns_entity`                           | After break-trip, the joint entity does not exist (`core.is_entity_alive(joint_entity) == false`).                                              | §3.9.2, §3.8 inv 3 | (CI) |
| `physics/joints: break_threshold_trip_releases_joint_id`                         | After break-trip, the `JointId` rejoins the free list (next `add_joint` reuses it under sort-on-pop).                                            | §3.9.2, §3.5    | (CI)  |
| `physics/joints: broken_joint_mutation_refused`                                  | Calling `set_joint_motor` on a `JointId` whose `broken` bit is set (between trip and despawn-pass drain) → `JointBroken`.                       | §3.8 inv 4, §10.1 | (CI) |
| `physics/joints: broken_event_payload_carries_force_and_torque`                  | The emitted `JointBrokenEvent.applied_force` and `applied_torque` equal `|accum_normal_impulse| / fixed_dt` and `|accum_friction_impulse| / fixed_dt` respectively. | §3.9.1         | (CI)  |
| `physics/joints: error_arms_byte_equal_across_runs`                              | The error stream from a fixed input trace is byte-equal across two consecutive runs (PHILOSOPHY §7).                                            | §10.7         | (CI)  |

### 11.2 Integration tests

Lives under `tests/physics/joints/integration/`. Drives the cluster
through multi-frame sequences via the `core` `FrameLoop` test harness.

| Test name                                                                | Scenario                                                                                                                                | §-ref         | Story  |
|--------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------|---------------|--------|
| `physics/joints: hinge_two_box_swing_under_gravity`                      | Two-box hinge fixture (one static, one dynamic); gravity drives the dynamic box to swing; assert post-substep angles converge to a stable steady-state within 60 frames. | §3.4.2        | #435   |
| `physics/joints: distance_chain_5_segments_hangs_under_gravity`          | 5-segment distance-joint chain; segments hang under gravity; assert chain endpoint position stable to 0.5 mm over 600 frames.            | §3.4.5        | #435   |
| `physics/joints: ragdoll_5_swing_twist_joints_settle`                    | 5-joint ragdoll fixture (shoulders + hips + neck = 5 SwingTwist constraints); released from a posed state; assert ragdoll settles within 120 frames with no NaN. | §3.4.6        | #435   |
| `physics/joints: hinge_motor_drives_to_target_velocity`                  | Hinge with `JointMotor.target_value = 5.0`; assert the dynamic body's angular velocity around the hinge axis converges to 5.0 ± 0.1 within 30 frames. | §3.4.2 motor   | #435   |
| `physics/joints: break_threshold_trips_under_load`                       | Distance joint with `max_force = 10 N`; apply 20 N to the dynamic body; assert `JointBrokenEvent` fires within one substep, entity is despawned. | §3.9, §4.1.7 inv 4 | #435 |
| `physics/joints: remove_body_blocked_by_live_joint`                      | Add a hinge between body A and body B; call `remove_body(A)`; assert `BodyStillReferencedByJoint` returned (the bodies-shapes side); the hinge persists. | §3.5 inv 6, `bodies-shapes-design.md` §10.1 | #431, #437 |
| `physics/joints: remove_body_succeeds_after_remove_joint`                | Add a hinge; `remove_joint`; `remove_body`; assert both succeed. The reverse index correctly evicts.                                    | §3.5 inv 5    | #431, #437 |
| `physics/joints: joint_id_stable_across_macos_arm64_vs_x64`              | Spawn a 5-joint ragdoll fixture on M1-arm64 and macOS-x64; assert `JointId` for joint-i matches across hosts.                            | §3.5 inv 2, `physics-world-design.md` §3.5 cell | #423   |
| `physics/joints: joint_id_stable_across_hot_reload_round_trip`           | Tower fixture (50 boxes + 5 ragdoll joints + 2 triggers); reload at frame 8; assert post-reload `JointId` for joint-i equals pre-reload. Per SPEC §8.6. | §8, SPEC §8.6 | #449   |
| `physics/joints: hot_reload_round_trip_byte_equal_for_5_joints`          | 5-joint ragdoll; reload at frame 8; assert post-resume warm-start impulses byte-equal pre-drain (the snapshot's `joint_normal_impulses` / `joint_friction_impulses` columns round-trip).       | §8.2, SPEC §8.6 | #449  |
| `physics/joints: hot_reload_refuses_unsupported_kind`                    | Build `physics-v2` with `JointKind::Spring = 6`; ship a snapshot containing a kind-6 row; resume against `physics-v1` (which knows only ordinals 0..5) → `HotReloadStateUnmigratable` mapping to `JointKindUnsupported`. | §8.3, SPEC §8.6 | #449  |
| `physics/joints: hot_reload_refuses_dangling_endpoint`                   | Snapshot has `joint_body_a[0] = 99` but body 99 is despawned before resume; `HotReloadRefused { PluginInitFailed { JointEndpointInvalid } }`. | §8.3          | #449   |
| `physics/joints: bench_ecs_jolt_mirror_two_substep`                      | The §9.6 `BENCHMARK_CELL` perf assert (0.05 ms ceiling for the cluster's mirror walk).                                                   | §9.6          | (CI)   |
| `physics/joints: bench_add_joint_each_kind`                              | The §9.6 `BENCHMARK_CELL` perf assert (0.020 ms / kind admission cost).                                                                  | §9.6          | (CI)   |
| `physics/joints: bench_break_trip_despawn_single_joint`                   | The §9.6 `BENCHMARK_CELL` perf assert (0.010 ms break-trip despawn cost).                                                                | §9.6          | (CI)   |

### 11.3 Property-based tests

Lives under `tests/physics/joints/property/`. Use Catch2 `GENERATE`
for fuzz-style coverage.

| Test name                                                                | Property                                                                                                                            | §-ref |
|--------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------|-------|
| `physics/joints: joint_id_allocation_pure_function_of_call_sequence`     | For random allocate/release sequences, replaying the sequence twice yields byte-equal id outputs.                                   | §3.5  |
| `physics/joints: reverse_index_invariant_under_random_add_remove`        | For random `add_joint` / `remove_joint` sequences, `Σ reverse.joint_count(b) == 2 × live_joint_count` after every operation.        | §3.5 inv 5 |
| `physics/joints: descriptor_round_trip_byte_equal_under_kind_enumeration`| For each of the six kinds, encode + decode a `JointDescriptorRecord` with random valid limits / motor / break and assert byte-equality. | §7.1.1 |
| `physics/joints: anchor_quaternion_unit_norm_check_total_under_random_q` | For random quaternions, the unit-norm check (§3.2 inv 4) accepts those in `[1 − 1e−6, 1 + 1e−6]` and rejects all others.            | §3.2 inv 4 |
| `physics/joints: snapshot_round_trip_per_joint_columns`                  | For random joint sets (mix of kinds, with / without companions), capture → restore round-trip preserves every column byte-equal.    | §3.10, §7.1.2 |

### 11.4 What this design does **not** test

- **Sibling-aggregate seams** — body / shape / contact / query /
  snapshot / accumulator / middleman mechanics. Each has its own
  design spike under #791; this cluster only tests its own facade
  brokering.
- **The bodies-cluster `BodyStillReferencedByJoint` arm** — that
  test is owned by `bodies-shapes-design.md` §11. The joints
  cluster's contribution is tested at the integration level via
  `remove_body_blocked_by_live_joint`, but the unit-level arm
  detection is the bodies cluster's responsibility (§3.5 +
  §10.5 — two arms, two detection sites).
- **Multi-thread access** — single-thread sim in MVP (SPEC §6.6).
  When per-system parallelism lands, ThreadSanitizer + the access-set
  DAG cover the new surface.
- **Ragdoll authoring / chain authoring** — refused in MVP (§2.3
  rows R-4.3.4 / R-4.3.5). The 5-joint ragdoll integration fixture
  is constructed by hand-coded test setup, not by a ragdoll-from-skeleton
  authoring tool; the latter is post-MVP `animation`.
- **Spring + Generic6Dof joint kinds** — refused in MVP (§2.3 rows
  R-4.3.1 sub-cases). When they reactivate, new unit + integration
  tests gate the per-kind behaviour.
- **Per-platform LOD / Verlet fallback / per-platform bone caps** —
  refused (§2.3 rows R-4.3.10 / R-4.3.11 / R-4.3.12).
- **Solver selection (SI vs TGS) + SolverConfig surface** — refused
  (§2.3 row R-4.3.6).
- **GPU / Metal interaction** — refused (SPEC §1, §3.3). No tests.

### 11.5 Acceptance-criteria mapping

The `[STORY]` issues already named in SPEC §11 that this design's
tests close (one Catch2 case per story; the §11.1–§11.3 tables list
them per story column):

- #435 `physics: create Joint entity materialises Jolt constraint with limits/motor/break`
- #437 `physics: refuse joints with invalid or cross-world endpoints`
- #449 `physics: hot-reload preserves world state across snapshot at phase 8` (cluster slice — joint replay + warm-start round-trip)
- #423 `physics: deterministic phase-3 step byte-equal across hosts` (cluster slice — `JointId` stability across hosts)

The remaining stories from SPEC §11 (#421, #425, #427, #429, #431,
#434, #439, #441, #443, #445, #448, #451) belong to sibling
aggregates and are out of scope for this design — though #431
("remove RigidBody destroys Jolt body, refuses if joint references
it") consumes the joints cluster's reverse index (§3.5 inv 6) at the
integration test level (`remove_body_blocked_by_live_joint`); the
joints contribution is the index correctness, not the refusal arm
itself.

## 12. Open questions

Each `[OPEN]` is a follow-up amendment trigger; resolution amends
the matching SPEC section in place, not this design. Per the
PHILOSOPHY §3 + workflow rule, no `[OPEN]` is discharged silently.

- **[RESOLVED — 2026-05-09]** `BodyStillReferencedByJoint` vs
  `JointDanglingEndpoint` arm split — spike #908 resolved by
  `reviews/decisions/physics-error-arm-joint-body-reconciliation.md`
  (commit `865405e3`, merged to `main` 2026-05-09). **Alternative C
  adopted:** three operationally distinct arms with one canonical
  construction site each, disambiguated by the owning aggregate's
  reason-to-change:
  - `BodyStillReferencedByJoint` (`warn`) — bodies cluster,
    `remove_body` call site. Body cannot be destroyed while a live
    joint holds it; caller removes the joint first.
  - `JointDanglingEndpoint` (`warn`) — joints cluster, `add_joint`
    call site. Endpoint `BodyId` was once valid but the body was
    despawned between author-time and commit (timing race); caller
    re-fetches live `BodyId` and re-issues.
  - `JointEndpointInvalid` (`error`) — joints cluster, `add_joint`
    call site. Authored-garbage handle (zero-init, cross-world, or
    never-live); programming bug; caller fixes the code path.
  This design's §3.4 dispatcher, §10.1 arm table, §10.5 prose, and
  §11.1 test rows are updated to reflect Alt C. The SPEC §4.1.7
  invariant 1 prose and SPEC §10.1 arm contracts were amended in the
  same merge (see decision record §"Spec edits delivered alongside
  this decision"). No further amendment is required by this design.

- **[OPEN — SPEC AMENDMENT REQUIRED]** `SnapshotJointIdUnresolved`
  (referenced in §10.4 above) is the joint-side analog of
  `SnapshotBodyIdUnresolved` (SPEC §5 enum + SPEC §10.1). The
  joint analog must be added to the §5 `physics::Error` enum + a
  new SPEC §10.1 row in the same implementation plan that
  introduces `physics/include/glibre/physics/error.hpp` (per SPEC
  §10.7 "added by §10" pattern). The detection point is
  §8.2.2 step 4.B; severity is `error` under CI `Hard`, `warn` under
  shipping `SoftWarn` (matching the bodies analog). Until the
  amendment lands, the §10.4 row carries the "(pending SPEC §10.1
  amendment)" marker and the §8.3 refusal table forwards under
  `JointEndpointInvalid` as a fallback. Tracked by the same `error.hpp`
  implementation plan as the spike #908 resolution above.

- **[OPEN]** Should `JointBreakThreshold` distinguish "force-only" /
  "torque-only" / "either" trip semantics? Currently §3.8 uses an
  OR-relation (`applied_force > max_force OR applied_torque >
  max_torque`); a future fixture might want AND-relation (e.g. a
  ragdoll knee that breaks only on combined twist + pull). The
  expressivity gap can be filled by setting one limit to `+inf`
  (the OR with inf is satisfied trivially), but a dedicated
  `BreakMode { ForceOnly, TorqueOnly, Either, Both }` enum on
  `JointBreakThreshold` is more readable. Frozen at the OR
  semantics until an MVP fixture demonstrates the AND case as
  load-bearing. Tracked by §3.8 + §3.9.1.

- **[OPEN]** Should `JointMotor` carry a `MotorMode { Velocity,
  Position }` enum (harmonius prior art's per-mode dispatch) instead
  of the velocity-only MVP shape? Position-mode would drive a PD
  controller toward `target_position` rather than `target_velocity`;
  Jolt's `HingeConstraint::SetMotorState(EMotorState::Position)`
  supports it. MVP collapses to velocity-only because the §11
  acceptance fixtures only exercise velocity-driven motors; a
  position-mode addition is a §5 amendment plus a §3.6 invariant
  bump. Tracked by §3.6.

- **[OPEN]** Should the cluster expose `set_joint_motor(JointId, ...)`
  / `set_joint_limits(JointId, ...)` direct mutators on the §5
  facade, or keep mutation strictly through ECS-component writes?
  The current design favours the latter (gameplay code writes
  `JointMotor.target_value`; the entry barrier picks it up). A
  direct mutator would let an editor toggle a single joint's motor
  without touching ECS, but it duplicates the ECS-component path
  and is the kind of "second seam" PHILOSOPHY §1 rejects. Frozen at
  ECS-only mutation; reopen if an editor UX story demonstrates the
  direct mutator as load-bearing.

- **[OPEN]** Should `JointLimits` mid-run mutation propagate to
  Jolt without a despawn + re-add? §3.7 invariant 2 currently
  silently ignores limit mutations until the joint is re-added;
  the cost of pushing them every entry barrier (one
  `JoltMiddleman::set_<kind>_limits` call per `JointLimits`-archetype
  row) is bounded by the §9.2 row but introduces a new middleman
  ABI surface (six per-kind setters). Frozen at the silent-ignore
  semantics until a gameplay fixture demonstrates limit-tweening as
  load-bearing. The debug-build telemetry counter ("limit mutations
  observed but not applied") is the tripwire.

- **[OPEN]** Should the `Cone` joint kind degenerate to `SwingTwist`
  with `swing_z = 0` / `twist_low = twist_high = 0`? Currently
  §3.4.4 declares `Cone` as a separate kind backed by Jolt's
  `ConeConstraint`; the alternative is to retire `ConeConstraint`
  and express the cone as a `SwingTwist` with a degenerate twist /
  one-axis swing. The trade-off is one fewer kind ordinal vs. a
  more expensive solve for what could be a simpler constraint. Per
  Jolt 2025 the `ConeConstraint` is genuinely lighter (3 + 1 rows
  vs SwingTwist's 3 + up to 3 rows); the kind separation is kept.
  Tracked by §3.4.4 + §3.4.6.

- **[OPEN]** Should `JointReverseIndex.by_body_` sized inline
  capacity be raised from 4 to 8? Ragdoll bodies that participate
  in multiple joints (a torso with 4 limb-joint endpoints + 1
  neck-joint endpoint = 5 joints / body) overflow the inline 4 and
  spill to heap. The cost is +8 B per cell (24 B → 32 B per
  `fixed_vector<JointId, 8>`); at `max_bodies = 1024` that is +8 KiB
  index footprint. Frozen at 4 until a §11 fixture measures the
  spillover rate as load-bearing. Tracked by §5.3.

Resolution of any `[OPEN]` lands the decision into
`reviews/decisions/` (when cross-aggregate) or amends SPEC §3 / §4 /
§5 / §10 in place (when local to this cluster); per the workflow no
`[OPEN]` is discharged silently.
