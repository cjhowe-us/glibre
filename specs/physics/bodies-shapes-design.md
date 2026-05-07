# physics — Detailed Design: bodies-shapes aggregate

> Per-aggregate detailed design for the `RigidBody` + `BodyId` +
> `Collider` + `ShapeHandle` + `ShapeBlob` + `Sleeping` + `CCD` cluster
> declared in `specs/physics/SPEC.md` §4.1.5, §4.1.5b, §4.1.6, §4.1.14,
> §4.1.15, with public surface frozen in §5 and frame integration locked
> in §6.2 / §6.3 (`bodies/rigid_body.cpp` + `shapes/shape_table.cpp`).
> Refines those sections in place; adds no new public surface beyond §5.
> Cites `reviews/decisions/error-model.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/frame-phases.md`,
> `reviews/decisions/fory-codegen.md`. Deviations from the cited
> records require an amendment spike, not an in-place edit.
>
> Sibling aggregates inside the physics context — `PhysicsWorld` /
> `PhysicsConfig` / `Accumulator` (covered by
> `specs/physics/physics-world-design.md`), `Substep`, `Joint`,
> `ContactManifold` / `Trigger`, `PhysicsQueries`, `BroadphaseLayer`,
> `PhysicsSnapshot`, `JoltMiddleman` — are deliberately **out of scope
> here**; this design treats them as opaque seams that the bodies-shapes
> cluster brokers across. Where this design names a sibling seam, it
> cites the SPEC invariant the seam upholds, not the sibling's internal
> mechanics.
>
> Harmonius prior art (`harmonius/docs/requirements/physics/
> rigid-body-dynamics.md` R-4.1.* + `harmonius/docs/requirements/physics/
> collision-detection.md` R-4.2.* + `harmonius/docs/design/physics/
> foundation.md` § "ECS Component Map" / "Architecture" / "Collision
> Shapes") is **research input only** — every conclusion below is
> independently re-derived per `PHILOSOPHY.md` § "How harmonius is used".
>
> Refs: spike #794 — `[SPIKE] design-physics-bodies-shapes-detailed`.
> Parent: #791 (sub-epic — Detailed Designs — physics). Sibling
> `[SPIKE] task-breakdown-physics-bodies-shapes-detailed` is blocked by
> this deliverable.

## 1. Purpose

The bodies-shapes cluster is the **per-entity body+shape mirror** of
the physics context (SPEC §4.1.5, §4.1.6). Its single composed
responsibility is **owning the per-`ecs::Entity` rigid-body kinematic
state, the deterministic 32-bit `BodyId` that names the body inside
Jolt's body table, the per-instance `Collider` metadata that links the
body to its collision geometry, and the content-hashed immutable
`ShapeBlob` table whose handles every collider keys into**. The
cluster is what `PhysicsWorld::add_body` / `remove_body` /
`intern_shape` / `release_shape` / `intern_material` actually do
once the facade forwards. Concretely the cluster owns:

1. **The `RigidBody` ECS component (SPEC §4.1.5)** — the per-entity
   kinematic+dynamic state: `MotionType` (`Static` / `Kinematic` /
   `Dynamic`), mass, inertia diagonal (or auto-derived flag), linear +
   angular damping, the `BodyId` payload, the CCD bit, and the
   sleeping bit. POD-like aggregate stored in core's archetype
   storage; identity is the owning `ecs::Entity`. Companion components
   `Velocity`, `AngularVelocity`, `ExternalForce`, `ExternalTorque`,
   `Sleeping` are also owned at this seam — the cluster is the only
   site that reads / writes them at substep barriers.
2. **The `BodyId` value object + allocator (SPEC §4.1.5b)** — the
   stable 32-bit handle into Jolt's body table, plus the per-world
   deterministic allocator that issues it. Allocation order is fixed
   by ECS materialisation order (PHILOSOPHY §7); the allocator is the
   cell that turns "this entity gained a `RigidBody`" into a
   reload-stable identity (§4.1.5b invariant 1).
3. **The `Collider` ECS component (SPEC §4.1.6)** — the per-instance
   shape mirror: one `ShapeHandle`, an offset transform from body to
   shape, a `CollisionLayer`, a `MaterialId`, an optional `Trigger`
   marker, and a per-instance density override. The component never
   owns shape bytes (§4.1.6 invariant 3); it owns the handle plus the
   per-instance metadata.
4. **The `ShapeHandle` opaque handle + shape table (SPEC §4.1.6)** —
   the reference-counted runtime handle, the per-world content-hash
   map from `ShapeBlobRecord.content_hash` to a single table row, and
   the resolved Jolt `Shape*` pointer that lives behind the handle.
   Two `Collider`s referencing the same content hash share one row
   (§4.1.6 invariant 1); refcount drops free the row.
5. **The `ShapeBlob` value object (SPEC §4.1.6, §7.1.2)** — the
   versioned, immutable byte payload (primitive parameters, baked
   convex hull, baked triangle mesh, baked heightfield, baked
   compound) consumed by the shape table at first reference. Authored
   by `content` / `geometry` at cook time (§3.3); physics never bakes
   at runtime (§7.1.2 invariant 3).
6. **The `Sleeping` marker + wake-state ledger (SPEC §4.1.14)** — the
   per-body bit that says "Jolt is excluding this body from the active
   set"; the per-world add/remove walk that mirrors Jolt's
   `Body::IsActive()` into the ECS marker; the per-body sleep-frame
   counter that survives across snapshots.
7. **The `CCD` flag (SPEC §4.1.15)** — the per-body bit that opts the
   body into Jolt's swept narrowphase (linear-cast continuous
   collision detection); read once per substep at the entry barrier
   when the cluster forwards Jolt's `BodyCreationSettings`.

This cluster **refuses to own**:

- **The Jolt `PhysicsSystem` instance, the fixed-timestep
  accumulator, and the phase-3 driver** — owned by the
  `physics-world` cluster (`specs/physics/physics-world-design.md`).
  The cluster is called *by* the phase-3 driver at substep barriers;
  it never enters `JoltMiddleman::step`.
- **The substep math** (broadphase, narrowphase, constraint solve,
  contact resolution, integration, CCD swept TOI). All of it lives
  inside `JoltMiddleman::step`, behind the §4.1.13 ABI seam. Per
  SPEC §3.2 collapse #1 the cluster is a **kinematic mirror**, not a
  solver author. CCD's per-body flag is wired at body create-time;
  the swept-volume math is Jolt's.
- **Joints + constraint topology** — owned by the `joints/` sibling
  aggregate (`specs/physics/SPEC.md` §4.1.7). The cluster knows that
  a joint references its bodies (§4.1.7 invariant 1) and refuses
  body removal while a joint endpoint is live (`physics::Error::
  BodyStillReferencedByJoint`, §10.1), but never authors constraint
  bytes itself. Joint-restore at hot-reload runs **after** the
  cluster's body-restore so the endpoints are already live.
- **Contact / trigger / joint-broken events** — owned by the
  `contact/` sibling aggregate (SPEC §4.1.8 / §4.1.9). The cluster
  honours the `Trigger` marker as a per-Collider bit (§4.1.6
  invariant 4) but the listener adapter that drains contact pairs
  into ECS event buffers is a sibling.
- **Spatial queries** — owned by the `queries/` sibling aggregate
  (SPEC §4.1.10). The cluster's `ShapeHandle` and `BodyId` types
  cross the queries surface as `QueryHit` payload fields, but the
  queries surface itself is not authored here.
- **The `PhysicsSnapshot` codec** — owned by the `snapshot/` sibling
  aggregate (SPEC §4.1.12, §7.1.4). The cluster reads / writes the
  per-body and per-shape **columns** of the snapshot (§7.1.4 schema
  tags 6–14) but the writer / reader plumbing is a sibling.
- **`PhysicsMaterial` asset bytes** — owned at the asset layer by
  `data` / `content` (§3.3). The cluster carries `MaterialId`
  references on `Collider`; bytes never enter the cluster.
- **The `BroadphaseLayer` interaction matrix and `LayerFilter`** —
  owned by the `physics-world` cluster's `PhysicsConfig` (§4.1.2
  collapse #6). The cluster reads `Collider::layer` (a
  `CollisionLayer` index) at body create-time and forwards it
  through the JoltMiddleman; the per-pair interaction matrix is a
  config seam, not a per-body seam.
- **Jolt header inclusion.** `<Jolt/...>` headers live behind exactly
  one TU (`middleman/jolt_middleman.cpp`; SPEC §6.1 module rule 1,
  §4.1.13 invariant 1). The cluster's TUs include only the §5
  facade and the `JoltMiddleman` reference threaded through every
  forwarding call.
- **GPU work, asset bytes, scripting intents** — refused per SPEC
  §1 + §3.3.

The cluster's SRP boundary is sharp: **the only reason `RigidBody`,
`BodyId`, `Collider`, `ShapeHandle`, `ShapeBlob`, `Sleeping`, or
`CCD` would change is a change to "what kinematic state a body has,
how it is named, what shape it presents, and whether it is asleep
or swept-cast"**. Anything else — what one substep computes, what a
joint binds, what a query returns, what gets serialised across
hosts — routes to the sibling aggregate that owns it.

## 2. Requirements coverage

Mapping of harmonius `R-4.1.*` (`rigid-body-dynamics.md`) and
`R-4.2.*` (`collision-detection.md`) clauses onto MVP coverage in
this aggregate. Every entry is independently re-derived; coverage
sites refer to sections of `specs/physics/SPEC.md` and to the
design sections below. Clauses owned by sibling aggregates **inside
the physics context** are listed as **Routed (sibling)** with the
SPEC §4.1.* row that owns them; clauses outside MVP scope are listed
as **Refused** with the §3.3 routing target the SPEC already records.

### 2.1 Aggregate-owned clauses

| Harmonius clause                                                                                                                         | Disposition                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
|------------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Harmonius design — `RigidBody` ECS component (`MotionType` / mass / inertia / damping / sleeping / CCD bit)                              | **Covered.** §3.2 `RigidBody` model. Per §4.1.5 the component is the per-entity kinematic+dynamic mirror; the `BodyId` payload (§3.3) names the Jolt body; the CCD + sleeping bits dispatch to the corresponding Jolt opt-in. Foundation harmonius split mass/inertia/velocity into separate components; glibre **collapses** velocity/angular-velocity to **companion** components on the same archetype (§3.2 collapse) — one row per simulating entity, not seven.            |
| Harmonius design — `Velocity` / `AngularVelocity` / `ExternalForce` / `ExternalTorque` companion components                               | **Covered.** §3.2 + §3.6 entry/exit barrier walks. Companions are the only legal carriers for sim inputs (force/torque) and outputs (velocity/angular velocity); bodies/rigid_body.cpp drains them at substep entry (§3.6.1) and writes them at substep exit (§3.6.2).                                                                                                                                                                                                            |
| Harmonius design — `MotionType` taxonomy (`Static` / `Kinematic` / `Dynamic`)                                                            | **Covered.** §3.2 `RigidBody.motion_type`; §3.4 invariant "MotionType is pinned" (§4.1.5b invariant 3) — switching motion type means despawn + re-add.                                                                                                                                                                                                                                                                                                                            |
| Harmonius design — Stable allocator producing reload-stable BodyId values                                                                | **Covered.** §3.3 `BodyIdAllocator` model + §3.3.2 free-list policy. Allocation is `O(1)` and deterministic over ECS materialisation order; reload re-issues the same handle to the same entity (§4.1.5b invariant 1, §4.2 invariant 5).                                                                                                                                                                                                                                          |
| **R-4.1.6** sleep at rest + wake on external force / torque / new contact                                                                 | **Covered.** §3.7 `Sleeping` model. Jolt owns the wake decision (it tracks linear/angular speed against `PhysicsConfig.sleep_*` thresholds; harmonius §"Sleep System"); the cluster mirrors `Body::IsActive()` into the `Sleeping` marker at substep exit (§3.6.2). The per-body `sleep_frames` counter survives snapshots (§7.1.4 tag 12).                                                                                                                                       |
| **R-4.1.4** swept-volume CCD opt-in for tunneling-prone bodies                                                                            | **Covered (per-body bit only) + Routed (swept narrowphase).** The cluster owns the per-body `RigidBody.ccd` bit and forwards it to Jolt's `BodyCreationSettings` at create-time (§3.4 step 4; SPEC §4.1.15). The swept TOI math itself is Jolt-internal, behind the §4.1.13 seam — covered by the `Substep` sibling.                                                                                                                                                              |
| Harmonius design — `Collider` ECS component (shape + offset + layer + material + trigger flag)                                            | **Covered.** §3.5 `Collider` model. Per-instance metadata only (§4.1.6 invariant 3); the shape bytes live on the `ShapeBlobRecord`, the handle is the seam.                                                                                                                                                                                                                                                                                                                       |
| **R-4.2.3** primitive shapes — sphere / box / capsule + convex hull, with primitive fast paths                                            | **Covered.** §3.8.1 `Sphere`, §3.8.2 `Box`, §3.8.3 `Capsule`, §3.8.4 `ConvexHull` rows. Each `ShapeKind` resolves at intern time to the corresponding Jolt `Shape` subclass via the middleman; the "fast path" claim is satisfied because Jolt 2025 ships specialised primitive-vs-primitive overlap routines selected by Jolt's narrowphase dispatcher — the cluster does not author the dispatch table.                                                                          |
| **R-4.2.4** triangle-mesh + heightfield shapes with per-triangle material indices                                                         | **Covered.** §3.8.5 `TriangleMesh` row + §3.8.6 `Heightfield` row. The §7.1.2 `mesh_materials` column carries one `MaterialId` ordinal per triangle; the cluster passes the array verbatim to Jolt's `MeshShapeSettings::mPerTriangleUserData` analog inside the middleman.                                                                                                                                                                                                       |
| **R-4.2.5** compound colliders combining multiple primitives with per-child layers + materials                                            | **Covered.** §3.8.7 `Compound` row. A compound `ShapeBlobRecord` references its children by `content_hash` (§7.1.2 invariant 4); the shape table resolves each child to a `ShapeHandle` at parent-load time. Per-child layers + materials are recorded on the **parent** `Collider` for MVP; per-sub-shape-id metadata (e.g. per-vehicle-wheel surface tag) is the post-MVP `sub_shape_metadata` extension noted in §12. Adding a kind is the §7.2.2 schema bump.                  |
| **R-4.2.9** `PhysicsMaterial` references with friction / restitution / density / combine modes                                            | **Covered.** §3.5 `Collider.material` field carries one `MaterialId`; `PhysicsMaterial` bytes live at the asset layer (§3.3) and the cluster never decodes them. Per-triangle-on-mesh material indices (R-4.2.4) ride the `mesh_materials` ordinal column on the shape blob.                                                                                                                                                                                                       |
| Harmonius design — content-hashed immutable shape sharing across colliders                                                                | **Covered.** §3.9 `ShapeTable` model. `ShapeBlobRecord.content_hash` (BLAKE3 over canonicalised bytes per §7.1.2 invariant 1) is the table key; two colliders referencing the same hash share one row. `ShapeHandle` is reference-counted; refcount-zero rows drop the resolved Jolt `Shape*`. Story #451 closes on this row.                                                                                                                                                       |
| Harmonius design — runtime kinematic→dynamic split (Static immutable, Kinematic script-driven, Dynamic solver-driven)                    | **Covered.** §3.2 invariants for each `MotionType`. Static carries zero velocity, ignores external force; Kinematic is integrated by ECS-side script writing position + velocity targets; Dynamic is solver-driven (§4.1.5b invariant 4). The mirror barriers (§3.6) honour these by branching on `motion_type` per body row.                                                                                                                                                     |
| Harmonius design — Static body BLAS handoff to render via shared content hash                                                             | **Covered.** §3.10 cross-context handoff. The cluster computes the body's `ShapeBlob.content_hash` at create-time so render's `RTAccelStructures` aggregate (`specs/render/SPEC.md` §4.1.8) can key its BLAS imports on the same hash; the seam itself is render-owned but the cluster's contract is "the hash that physics holds is the hash render sees" (§4.1.6 invariant 1, SPEC §6.5 seam #3).                                                                                |

### 2.2 Sibling-aggregate clauses (routed within the physics context)

These clauses are MVP-scope but their reason-to-change is owned by a
sibling aggregate. Each routes to the SPEC §4.1.* row that owns it;
the corresponding sibling design spike fills in the body. Listed
here so the cluster's seam shape is exhaustive.

| Harmonius clause                                                                                                                          | Routed to (sibling SPEC row)                                                              |
|-------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------|
| **R-4.1.1** symplectic Euler with fixed-timestep accumulator                                                                              | §4.1.1 `PhysicsWorld` + §4.1.3 `Accumulator` (covered by `physics-world-design.md`)       |
| **R-4.1.2** configurable substep count (`PhysicsConfig` half)                                                                             | §4.1.2 `PhysicsConfig` (`physics-world-design.md`)                                        |
| **R-4.1.3** impulse-based contact resolution + restitution + friction combine                                                              | §4.1.4 `Substep` + §4.1.8 `ContactManifold` (sibling spikes)                              |
| **R-4.1.5** simulation islands via union-find + parallel solve                                                                            | §4.1.4 `Substep` (Jolt-internal — the cluster reads only the `Sleeping` mirror surface)   |
| **R-4.2.1** broadphase via shared BVH + `BroadphasePairs` resource                                                                        | §4.1.4 `Substep` + §4.1.10 `PhysicsQueries` (Jolt's broadphase, behind the middleman)     |
| **R-4.2.2** narrowphase contact generation via GJK / EPA / SAT                                                                            | §4.1.4 `Substep` (Jolt-internal narrowphase)                                              |
| **R-4.2.6** collision filtering via `CollisionLayers` bitmasks                                                                            | §4.1.2 `PhysicsConfig` + §4.1.11 `BroadphaseLayer` (`physics-world-design.md`)            |
| **R-4.2.7** `CollisionStarted` / `Persisted` / `Ended` events with same-frame delivery                                                    | §4.1.8 `ContactManifold` / `ContactEvent` (sibling spike)                                 |
| **R-4.2.8** trigger volumes with `TriggerEnter` / `Stay` / `Exit` events                                                                  | §4.1.9 `Trigger` / `TriggerEvent` (sibling spike). The per-Collider `Trigger` marker bit is **owned here**; the listener drain that emits the events is sibling. |
| **R-4.1.NF1** 2 000 active rigid bodies + 4 substeps within 4 ms                                                                          | **Reframed in §9 + `physics-world-design.md` §9.1.** The MVP S1 fixture is ~30 active bodies + 200 mostly-sleeping props (`perf-budget.md`); the 2 000-body figure is post-MVP scaling. |
| **R-4.1.NF2** ≤ 256 bytes per active rigid body                                                                                            | **Reframed against §9.3 heap cells**, not enforced as a per-body byte gate (`physics-world-design.md` §2.3). The component-byte count is a §3.2 hot-prefix budget, not a global ceiling. |
| **R-4.1.NF3** byte-identical results across hosts                                                                                          | §6.4 determinism guards + §6.6 single-thread sim (`physics-world-design.md`). The cluster's contribution — `BodyId`-ascending iteration order at every barrier — is documented in §6.4 here. |

### 2.3 Refused clauses (out of MVP scope)

Refused per SPEC §3.3; the §3.3 routing target the SPEC already
records is repeated here for the cluster's surface.

| Harmonius clause                                                                                                                                | Routing target                                                                  |
|-------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------|
| **R-4.1.7** cross-streaming-zone migration preserving momentum + contact state                                                                  | Post-MVP world-streaming context                                                |
| **R-4.1.8** kinematic character controller (ground detection / slope sliding / step climbing / coyote time)                                     | Post-MVP `character` plugin; consumes `PhysicsQueries` shape casts              |
| **R-4.1.9** moving platforms with one-way filtering                                                                                              | Post-MVP `character` plugin                                                     |
| **R-4.1.10** character ground smoothing on tessellated terrain                                                                                   | Post-MVP `character` plugin                                                     |
| **R-4.1.11** gyroscopic torque under a per-body `gyroscopic` flag                                                                                 | Reachable as a Jolt-feature follow-up; refused per SPEC §3.3                    |
| **R-4.1.12** rolling friction torque on `PhysicsMaterial`                                                                                         | Same — refused per SPEC §3.3                                                    |
| **R-4.1.13** directional friction (primary axis + lateral coefficient)                                                                            | Same — refused per SPEC §3.3                                                    |
| **R-4.1.16** 2D rigid-body mode (scalar inertia, 2D shapes, 2-DoF solver)                                                                          | Post-MVP `physics-2d` (parallel `PhysicsWorld`); also refuses 2D shape kinds    |
| **R-4.1.17** wall sliding with wall-friction + wall-angle threshold                                                                               | Post-MVP `character`                                                            |
| **R-4.1.18** multi-jump + wall jump + jump buffer                                                                                                  | Post-MVP `character`                                                            |
| **R-4.1.19** crouching with ceiling-clearance shape cast                                                                                           | Post-MVP `character`                                                            |
| **R-4.1.20** push forces from character controller to dynamic bodies                                                                              | Post-MVP `character`                                                            |
| **R-4.2.10** voxel-chunk triangle-mesh collider regeneration on terrain edit                                                                      | Post-MVP `voxel-terrain` (cooked output crosses as `ShapeBlob`; physics never re-bakes) |
| **R-4.2.11** offline V-HACD compound decomposition at asset import                                                                                 | `tools` + `content` / `geometry` cook-time bake. Output is `Compound` `ShapeBlob` (§3.8.7). |
| **R-4.2.12** runtime quickhull from arbitrary vertex sets                                                                                          | Post-MVP `destruction` plugin; output is a fresh `ConvexHull` `ShapeBlob`        |
| **R-4.2.13** editor-configurable 32 × 32 layer interaction matrix authoring UX                                                                     | `tools` (editor authoring); the **decoded matrix** ships in `PhysicsConfigRecord` and is owned by `physics-world` |
| Soft-body / cloth / fluid / destruction shape kinds                                                                                                | Post-MVP plugins; not in `ShapeKind` sealed sum                                 |
| `ShapeBlob` runtime baking (V-HACD / quickhull / heightfield resampling)                                                                            | Refused at `intern_shape` with `ShapeBlobMalformed` per §7.1.2 invariant 3       |

### 2.4 Coverage summary

Of the 24 harmonius `R-4.1.*` clauses + 16 `R-4.2.*` clauses + the
listed harmonius design clauses (~13 cross-cutting design rows
between the two requirement files):

- **15 covered** as bodies-shapes design here (the `RigidBody` /
  `BodyId` allocator / `Collider` / `Sleeping` / `CCD` / shape-table
  / shape-kind rows + their harmonius design counterparts).
- **11 routed to sibling aggregates** within the physics context
  (the broadphase / narrowphase / solver / island / contact-event /
  trigger-event / accumulator / config rows; R-4.1.NF3 + R-4.2.NF1
  / NF2 / NF3 cross-cutting non-functionals).
- **17 refused with rationale** — 14 deferred post-MVP (the
  character / multi-world / 2D / advanced-friction / runtime-bake
  surfaces), 3 reframed against §9 heap cells (R-4.1.NF1 active body
  count, R-4.1.NF2 per-body memory, R-4.2.NF1 broadphase entity
  count).

This list is closed; PRs adding any of the refused clauses to the
`bodies-shapes` cluster should be rejected and routed to the listed
owner.

## 3. Detailed model

The model below is the implementer's authority for the
`bodies/rigid_body.{hpp,cpp}`, `bodies/body_id_allocator.{hpp,cpp}`,
`bodies/sleeping.{hpp,cpp}`,
`shapes/shape_table.{hpp,cpp}`, and `shapes/shape_blob.{hpp,cpp}` TUs
(SPEC §6.1). Each subsection owns one of the five primitives the
cluster composes; the SPEC §4.1.* invariants the primitive enforces
are cited per subsection. The CCD opt-in (§3.4) is a static helper
inside `rigid_body.cpp`, not a separate TU (§3.1 SRP rationale).

### 3.1 Composition and module boundary

```text
physics/src/bodies/                                (private headers; not on plugin include path)
├── rigid_body.{hpp,cpp}                           (§3.2 + §3.4 ─ RigidBody mirror; entry/exit barrier walks; CCD bit forwarded as static helper at create-time)
├── body_id_allocator.{hpp,cpp}                    (§3.3  ─ Deterministic 32-bit BodyId allocator)
└── sleeping.{hpp,cpp}                             (§3.7  ─ Sleeping marker mirror + sleep-frame counter)

physics/src/shapes/                                (private headers; not on plugin include path)
├── shape_table.{hpp,cpp}                          (§3.9  ─ Content-hash table; ShapeHandle refcount; resolved Jolt Shape*)
└── shape_blob.{hpp,cpp}                           (§3.8  ─ Decode dispatcher per ShapeKind; produces Jolt Shape*)

> **Sibling reference (not owned by this cluster):** `physics/src/world/broadphase_layer.{hpp,cpp}`
> is owned by the `physics-world` cluster (`physics-world-design.md` §3.1). This cluster
> consumes `CollisionLayer` values and the `LayerFilter` abstraction exclusively through the
> §5 public facade; it does not declare or define broadphase-layer logic.

physics/include/glibre/physics/
└── physics.hpp                                    (§5 facade header; SPEC §5 — locked surface)
```

`world/physics_world.hpp` is the single dependency hub (SPEC §6.1
module rule 3); it owns `unique_ptr` references to the
`BodyIdAllocator`, the `ShapeTable`, and the bodies-side mirror
helpers, and is the only header outside `physics/src/bodies/` and
`physics/src/shapes/` permitted to include the headers above. The
CMake visibility rule cited by SPEC §6.1 module rule 3 enforces
this: a sibling module's `.cpp` may include its own `.hpp`s and the
§5 facade only — cross-module reach-throughs are forbidden, and the
seam is `world/physics_world.hpp`.

The five TUs decompose by SRP (PHILOSOPHY §1):

- **`rigid_body.cpp`** — one reason to change: how ECS RigidBody +
  companion-component bytes mirror to / from Jolt at substep
  barriers, including the per-body CCD opt-in forwarded as a static
  helper `apply_motion_quality` at create-time (§4.1.5 + §4.1.15 +
  §4.2 invariant 2). The CCD bit's reason-to-change ("how
  `RigidBody.ccd` feeds into `BodyCreationSettings::mMotionQuality`")
  is the same reason the body-creation path would change; a separate
  TU for a one-line branch would violate PHILOSOPHY §1 by introducing
  a seam without a second reason-to-change.
- **`body_id_allocator.cpp`** — one reason to change: how a stable
  32-bit handle is issued per ECS materialisation order (§4.1.5b
  invariant 1).
- **`sleeping.cpp`** — one reason to change: how Jolt's
  `Body::IsActive()` mirrors into the `Sleeping` ECS marker and
  how the per-body `sleep_frames` counter advances (§4.1.14).
- **`shape_table.cpp`** — one reason to change: how content-hashed
  `ShapeBlobRecord`s share a Jolt `Shape*` pointer across multiple
  `Collider`s (§4.1.6 invariant 1 + §7.1.2 invariant 5).
- **`shape_blob.cpp`** — one reason to change: how a decoded
  `ShapeBlobRecord` dispatches by `ShapeKind` to the corresponding
  Jolt `Shape` constructor (§4.1.6 invariant 2 + §7.1.2 sealed sum).

A second TU per primitive is forbidden; one reason to change → one
TU. The `Collider` component is a POD aggregate (§5) whose mirror
work is part of `rigid_body.cpp` (mirror of `RigidBody` includes
mirror of its `Collider`); it does not have its own TU because its
"reason to change" is the same as the body's mirror. The CCD flag
(§3.4) follows the same reasoning: `apply_motion_quality` is a
static file-scope helper inside `rigid_body.cpp`, not a separate TU.

### 3.2 `RigidBody` (§4.1.5; covers harmonius RigidBody + Velocity + AngularVelocity + ExternalForce + ExternalTorque)

A POD ECS component whose layout is locked at SPEC §5. The cluster's
**internal view** decorates the public bytes with the per-substep
caches needed by the entry/exit barriers (§3.6); the cache fields
live on a sibling per-archetype scratch buffer, **not** on the
component itself (the component's bytes survive across reload by
§8.2 row "RigidBody"; cache bytes do not).

```cpp
// physics/src/bodies/rigid_body.hpp — internal view of the §5 RigidBody
namespace glibre::physics::detail {

struct RigidBodyView {
    // ---- Public bytes (§5 RigidBody; middleman ECS storage) ----
    BodyId      body_id;               // §5: RigidBody.body_id (the §3.3 allocator output)
    MotionType  motion_type;           // §5: RigidBody.motion_type (pinned at create; §4.1.5b inv 3)
    float       mass;                  // §5: RigidBody.mass (kg; > 0 unless Static)
    Vec3        inertia_diagonal;      // §5: RigidBody.inertia_diagonal (kg·m²; principal axes only)
    bool        auto_inertia;          // §5: RigidBody.auto_inertia (compute from shape + density at create)
    float       linear_damping;        // §5: RigidBody.linear_damping (1/s)
    float       angular_damping;       // §5: RigidBody.angular_damping (1/s)
    bool        ccd;                   // §5: RigidBody.ccd (§3.4)
    bool        sleeping;              // §5: RigidBody.sleeping (mirror of Jolt Body::IsActive(); §3.7)

    // ---- Per-substep mirror cache (NOT part of the public component) ----
    // Lives on a per-archetype scratch row keyed by BodyId; rebuilt at
    // every entry barrier, drained at every exit barrier. Survives
    // neither the snapshot nor a hot-reload (§8.2 row "ContactManifold"
    // analog).
    Vec3        cached_position;       // last committed Jolt position; written at exit barrier
    Quat        cached_rotation;       // last committed Jolt rotation; written at exit barrier
    Vec3        cached_linear_velocity;
    Vec3        cached_angular_velocity;
};

}  // namespace glibre::physics::detail
```

**Invariants** (§4.1.5 / §4.1.5b authoritative):

1. **One `RigidBody` per simulating ECS entity.** Component-add at the
   ECS layer is the trigger for `BodyIdAllocator::allocate(entity)`
   (§3.3); component-remove is the trigger for
   `JoltMiddleman::remove_body(body_id)` plus
   `BodyIdAllocator::release(body_id)`. Cross-world component moves
   are forbidden (the `BodyId` would be cross-world; §4.1.5b
   invariant 2). The cluster honours core's archetype-move barrier:
   if an entity is moved between worlds, the source-world `RigidBody`
   is despawned (releasing its `BodyId`) and the destination-world
   reads back a fresh allocation.
2. **`MotionType` is pinned for the body's lifetime.** Mutating
   `motion_type` post-create returns
   `physics::Error::BodyMotionTypeImmutable` (§4.1.5b invariant 3,
   §10.1). The wire shape — same Jolt `BodyID` for two distinct
   `MotionType`s — is never produced; switching motion type means
   despawn + re-add, which yields a new `BodyId`.
3. **Static bodies carry zero velocity.** `MotionType::Static` rows
   ignore `ExternalForce` / `ExternalTorque` and have
   `Velocity == AngularVelocity == 0` at every barrier. The entry
   barrier short-circuits Static rows (§3.6.1 step "skip Static");
   the exit barrier writes back zero so a system that read `Velocity`
   from a Static body sees zero unconditionally.
4. **Kinematic bodies are integrated by ECS-side script.** The entry
   barrier reads `RigidBody`-row position + `Velocity` from ECS and
   pushes them into Jolt; Jolt does **not** integrate Kinematic rows
   under solver impulse (§4.1.5b invariant 4). The exit barrier
   writes `Velocity` and `AngularVelocity` back so contact events
   (§4.1.8) carry the correct relative velocity, but never overwrites
   the row's position — the script remains authoritative.
5. **Auto-inertia derives at create-time only.** When
   `auto_inertia == true`, the cluster reads the body's first
   `Collider`, looks up its `MaterialId` (or its density override),
   and asks the middleman to compute the inertia tensor from the
   shape's volume + density (Jolt's
   `MassProperties::ScaleToMass` analog). The result is written into
   `inertia_diagonal` once and never recomputed; subsequent shape
   swaps require a despawn + re-add (matching the `MotionType` rule
   in invariant 2). The §4.1.5 record stores `auto_inertia` as a bit
   so a snapshot restore re-runs the derive only if the surviving
   record's bit is set.
6. **Hot-prefix for the per-substep mirror.** The first 64 B of the
   `RigidBody` ECS row hold the hot fields (`body_id`, `motion_type`,
   `linear_damping`, `angular_damping`, `ccd`, `sleeping`, plus 4 B of
   reserved padding). The cold prefix (`mass`, `inertia_diagonal`,
   `auto_inertia`) follows; cold fields are read at create-time only.
   `static_assert(offsetof(RigidBody, mass) >= 16)` pins the layout.

### 3.3 `BodyId` + `BodyIdAllocator` (§4.1.5b; covers reload-stable handle allocation)

The `BodyId` is the 32-bit `Handle<tags::body>` declared in §5; the
allocator is the per-`PhysicsWorld` cell that issues it. Allocation
order is determined entirely by ECS materialisation order (§4.1.5b
invariant 1, PHILOSOPHY §7); the allocator never reads wall-clock
time, host thread identity, allocator address, or any source of
host-side variance.

```cpp
// physics/src/bodies/body_id_allocator.hpp
namespace glibre::physics::detail {

class BodyIdAllocator {
public:
    [[nodiscard]] static Result<eastl::unique_ptr<BodyIdAllocator>>
        create(std::uint32_t max_bodies) noexcept;

    // Allocate a fresh BodyId. Order is the order in which `allocate`
    // is called; the caller (rigid_body.cpp::on_component_add) is
    // serialised by the ECS-archetype-add barrier so the call order
    // mirrors ECS materialisation order.
    [[nodiscard]] Result<BodyId>
        allocate(ecs::Entity owner) noexcept;

    // Release a BodyId. The released id rejoins the free list and
    // is reused on the next allocate (§3.3.2 free-list policy).
    [[nodiscard]] Result<void>
        release(BodyId) noexcept;

    // Cross-reload restore — replays `allocate(entity_i)` for every
    // surviving entity in BodyId-ascending order so the post-reload
    // allocator state is byte-equal to the pre-reload state.
    [[nodiscard]] Result<void>
        restore_from_snapshot(eastl::span<const SnapshotBody>) noexcept;

    [[nodiscard]] std::uint32_t in_use() const noexcept;
    [[nodiscard]] std::uint32_t high_water() const noexcept;

    ~BodyIdAllocator();
    BodyIdAllocator(const BodyIdAllocator&)            = delete;
    BodyIdAllocator& operator=(const BodyIdAllocator&) = delete;

private:
    BodyIdAllocator() noexcept;

    std::uint32_t                  next_id_ = 1u;   // 0 reserved as the "invalid" sentinel.
    std::uint32_t                  max_     = 0u;   // PhysicsConfig::WorldBudgets::max_bodies.
    eastl::vector<std::uint32_t>   free_list_;      // BodyId-ascending; sorted before pop (§3.3.2).
    eastl::vector<ecs::Entity>     id_to_entity_;   // index = (BodyId.raw() - 1); zero ⇒ free.
};

}  // namespace glibre::physics::detail
```

**Invariants** (§4.1.5b authoritative):

1. **`BodyId == 0` is the invalid sentinel.** Default-constructed
   `BodyId{}` has `raw() == 0`; `valid()` returns false. The
   allocator never returns `BodyId{0}`; the first id is `BodyId{1}`.
2. **Allocation order = ECS materialisation order.** The allocator
   itself does not see ECS-archetype timing — `rigid_body.cpp`'s
   component-add hook calls `allocate` in archetype-iteration order
   (which core guarantees deterministic per PHILOSOPHY §7). The
   allocator's job is to be a pure function of "the sequence of
   `allocate` / `release` calls"; that sequence is the wire-level
   determinism input.
3. **Free-list reuse is `BodyId`-ascending.** Released ids enter the
   free list; the next `allocate` pops the **smallest available**
   `BodyId` (§3.3.2 below). This reproduces the same `BodyId` for the
   same entity on a second pass through the same allocate / release
   sequence, including across hot-reload (§4.1.5b invariant 1, §4.2
   invariant 5).
4. **Bounded by `PhysicsConfig::max_bodies`.** Allocation past the
   ceiling returns `physics::Error::BudgetExceeded` (§10.1). The
   ceiling is read once at `create`; live mutation is forbidden
   (§4.1.2 invariant 1).
5. **`in_use()` is restorable from the snapshot's `body_ids` column.**
   The snapshot codec (§4.1.12) emits `body_ids` as a parallel-list
   column in `BodyId`-ascending order (§7.1.4 invariant 5); restore
   replays the same order so the allocator's `next_id_` and
   `free_list_` state at the end of restore is byte-equal to
   pre-snapshot.

#### 3.3.1 Allocator structure

`id_to_entity_[bodyid - 1] == ecs::Entity{}` means the id is on the
free list; non-zero entity means the id is in use by that entity.
The allocator stores no separate "is allocated" bitmap — the entity
slot is the discriminator. `next_id_` is the high-water mark; ids
above it have never been allocated. Allocation chooses
`min(free_list_.front_after_sort(), next_id_)`:

```cpp
Result<BodyId> BodyIdAllocator::allocate(ecs::Entity owner) noexcept {
    if (in_use() >= max_) {
        return std::unexpected{physics::Error::BudgetExceeded};
    }
    std::uint32_t raw{};
    if (!free_list_.empty()) {
        // Sort once-per-call to keep determinism; cost amortises over
        // the per-substep walk (§9.2 mirror row 0.30 ms cell).
        eastl::sort(free_list_.begin(), free_list_.end());
        raw = free_list_.front();
        free_list_.erase(free_list_.begin());
    } else {
        raw = next_id_++;
    }
    id_to_entity_[raw - 1] = owner;  // index 0..max_-1; raw 1..max_.
    return BodyId{raw};
}
```

Sort-on-pop is `O(n log n)` per allocate; for the MVP S1 fixture
(~30 active bodies + episodic spawns) the free-list size is bounded
and the sort cost is below the §9 mirror row's precision floor.
Post-MVP, when active body counts grow, the §3.3.2 alternative
free-list policies become relevant; for MVP the simple form suffices.

#### 3.3.2 Free-list policy alternatives (deliberation)

Three candidate policies for the free list. The chosen policy is
**(A) sort-on-pop**; the alternatives are recorded so a future
amendment knows what was rejected and why.

- **(A) sort-on-pop, ascending.** Chosen. Each `allocate` pays
  `O(n log n)` for the sort over the free list; pops are then `O(n)`
  for the erase from the front. Determinism: trivially stable —
  output is a pure function of the input call sequence. Cost: bounded
  by §9 mirror row.
- **(B) keep the free list sorted on insert.** Insert-sorted
  `O(n)` per release; `O(1)` pop. Same determinism guarantee. Saves
  a sort per allocate at the cost of a sort per release. **Rejected**
  because release is at least as common as allocate (every spawn /
  despawn pair is one of each), and the §9 cell budget already
  accommodates sort-on-pop; trading a sort-on-allocate for a
  sort-on-release does not change the cell totals but distributes
  cost into the despawn path that is also serialised against the
  ECS archetype barrier. Sort-on-pop keeps despawn cheap, which
  matters for the death-stacking-pile case (R-4.2.NF1 reframed —
  despawn floods are the realistic stress).
- **(C) min-heap free list.** `O(log n)` per allocate + release.
  Determinism is preserved because heap order is `O(log n)` per
  operation against a deterministic sequence. **Rejected** because
  `eastl::priority_queue` adds a dependency on a specific compare
  shape (the §11 acceptance test for stable `BodyId` would have to
  pin the compare's tiebreak), and the constant factor on the
  S1 fixture is worse than the sort-on-pop case for n ≤ 30. Revisit
  for post-MVP active-body counts above ~512 where the `O(n log n)`
  per allocate becomes load-bearing.

The policy choice **does not** affect the on-the-wire `BodyId`
values — all three policies produce the same id sequence given the
same allocate / release sequence — so the choice is local to the
cluster and not a SPEC-level concern.

### 3.4 `CCD` flag (§4.1.15; covers R-4.1.4 swept TOI opt-in)

The `CCD` bit on `RigidBody` is the per-body opt-in for Jolt's
swept-narrowphase. The cluster's responsibility is **forwarding**
the bit at body-create time, not authoring the swept math (which
lives in Jolt's narrowphase, behind the §4.1.13 seam).

```cpp
// physics/src/bodies/rigid_body.cpp — static helper (§3.1 folded into rigid_body.cpp)
static void apply_motion_quality(BodyId body_id,
                                 const RigidBody& row,
                                 JoltMiddleman& mm) noexcept {
    const auto quality =
        row.ccd ? MotionQuality::LinearCast
                : MotionQuality::Discrete;
    mm.set_motion_quality(body_id, quality);
}
```

**Invariants** (§4.1.15 authoritative):

1. **Jolt owns the swept TOI math.** The cluster forwards the bit
   only; it never constructs a swept volume, never scans the
   broadphase for tunneling candidates, never authors a TOI
   integration step. R-4.1.4 verification ("Fire a bullet at maximum
   speed through a thin wall. Assert a hit is detected.") passes
   when the bit is forwarded correctly and Jolt's deterministic-mode
   swept narrowphase is configured per `physics-world-design.md`
   §6.1 rule 1.
2. **CCD opt-in is per-body, not per-shape.** `MotionQuality` is a
   property of `BodyCreationSettings`, not of `Shape`. The same
   `ShapeHandle` may be referenced by a CCD body and a non-CCD body;
   the shape table (§3.9) does not branch on the bit.
3. **CCD requires `MotionType::Dynamic`.** Static bodies do not move;
   Kinematic bodies are integrated by ECS-side script and Jolt does
   not run swept narrowphase against them. The cluster ignores the
   `ccd` bit on Static / Kinematic rows at create-time (the
   middleman's `set_motion_quality` is a no-op in those cases per
   Jolt's contract); a debug-build assertion fires if a
   Static / Kinematic row has `ccd == true` and the operator misread
   the §3.2 invariant 4 contract.
4. **CCD cost lives inside the §9.2 Jolt-step row, not here.**
   Per-frame swept TOI cost is Jolt's; the cluster's per-body
   forwarding cost is `O(1)` and amortised against `add_body` (§9.4
   sub-budget for the cluster's own dispatch overhead is below the
   precision floor of the gate).

### 3.5 `Collider` (§4.1.6; covers per-instance shape mirror)

A POD ECS component whose layout is locked at SPEC §5. Per
§4.1.6 invariant 3 the component is a **thin mirror**: it owns a
`ShapeHandle` plus the per-instance metadata (offset, layer,
material, trigger flag) and never owns shape bytes. Mutating the
shape means replacing the `ShapeHandle`, not editing the blob.

```cpp
// physics/src/bodies/collider_view.hpp — internal view of the §5 Collider
namespace glibre::physics::detail {

struct ColliderView {
    ShapeHandle    shape;             // §5: Collider.shape  (the §3.9 table key)
    Vec3           offset_position;   // §5: Collider.offset_position (body-frame → shape-frame)
    Quat           offset_rotation;   // §5: Collider.offset_rotation
    CollisionLayer layer;             // §5: Collider.layer  (resolved through PhysicsConfig.layer_filter)
    MaterialId     material;          // §5: Collider.material (asset-side; physics carries the ref)
    bool           is_trigger;        // §5: Collider.is_trigger (§4.1.6 invariant 4)
    float          density_override;  // §5: Collider.density_override (0 ⇒ inherit material)
};

}  // namespace glibre::physics::detail
```

**Invariants** (§4.1.6 authoritative):

1. **One `Collider` per body.** MVP supports one collider per
   `RigidBody` entity. Multi-collider bodies (vehicle chassis +
   wheels, ragdoll limbs) are expressed as a **single** compound
   `ShapeBlob` referenced by a single `Collider` (§3.8.7); the
   compound's children carry the per-sub-shape geometry. Per-child
   layer + material overrides are post-MVP (§12 OQ-1).
2. **Trigger flag is shape-side, not body-side.** Whether a contact
   pair receives solver impulse or only emits trigger events
   (§4.1.9) is determined by `Collider::is_trigger` plus the
   layer-pair interaction matrix in `PhysicsConfig` (§4.1.6
   invariant 4). Per-frame mutation is forbidden — flipping the bit
   at runtime requires a despawn + re-add of the entire `Collider`
   (post-MVP "trigger pulse" use cases dock against the
   `physics-world` `LayerFilter` rather than mutating the bit).
3. **Layer / material / offset are mutable across spawns.** Unlike
   `MotionType` (§3.2 invariant 2), `Collider`'s `layer`, `material`,
   `offset_position`, `offset_rotation` are mutable on the live
   component and the mirror writes the new values into Jolt at the
   next entry barrier. Mutation crosses one substep boundary to
   commit (§4.1.7 invariant 2 analog). The cluster's mirror reads
   them every entry barrier; cost is bounded by §9 mirror row.
4. **`density_override == 0` means "inherit from material".** The
   auto-inertia path (§3.2 invariant 5) reads the override first;
   if zero it asks the asset layer for `PhysicsMaterial::density`.
   Negative values return `physics::Error::ConfigInvalid`; the
   middleman never sees a negative density.
5. **The `ShapeHandle` is the only escape hatch to shape bytes.**
   Components hold the handle; components never hold the resolved
   Jolt `Shape*` pointer. The pointer is rebuilt during hot-reload
   resume (§8 row 5); the handle's payload is a 32-bit table index.

### 3.6 ECS↔Jolt mirror — entry / exit barriers (§4.1.4 + §4.2 invariant 2)

The mirror seam between core's archetype storage and Jolt's body
table is implemented in `bodies/rigid_body.cpp` and called only by
`world/phase3_driver.cpp` (which the `physics-world` design owns).
Two directions, two barriers, one substep boundary — §4.2 invariant
2 made mechanical, in the bodies-shapes slice.

#### 3.6.1 Entry barrier (substep entry, before `JoltMiddleman::step`)

```text
For body in RigidBody archetype, sorted ascending by BodyId:
  if body.motion_type == Static:
    continue                         # no force / torque / kinematic mirror
  ExternalForce.value     -> JoltMiddleman::add_force(BodyId, Vec3)
  ExternalTorque.value    -> JoltMiddleman::add_torque(BodyId, Vec3)
  ExternalForce.value      = 0       # drain — §4.1.4 inv 3
  ExternalTorque.value     = 0
  if body.motion_type == Kinematic:
    GlobalTransform.position -> JoltMiddleman::set_position(BodyId)
    GlobalTransform.rotation -> JoltMiddleman::set_rotation(BodyId)
    Velocity.v               -> JoltMiddleman::set_linear_velocity(BodyId)
    AngularVelocity.w        -> JoltMiddleman::set_angular_velocity(BodyId)
```

Per `physics-world-design.md` §3.6.1 the cluster contributes the body
walk; joints + queries + contact subscribers do not run inside this
barrier. The walk is sorted by `BodyId` ascending using a per-substep
arena scratch span (§3.4.4 step 1 reuse); the sort cost is the §9
determinism premium (`physics-world-design.md` §9.5 row 1).

#### 3.6.2 Exit barrier (substep exit, after `JoltMiddleman::step`)

```text
For body in RigidBody archetype, sorted ascending by BodyId:
  if body.motion_type == Static:
    Velocity.v               = 0     # invariant: Static carries zero velocity
    AngularVelocity.w        = 0
    continue
  JoltMiddleman::get_linear_velocity(BodyId)  -> Velocity.v
  JoltMiddleman::get_angular_velocity(BodyId) -> AngularVelocity.w
  if body.motion_type == Dynamic:
    JoltMiddleman::get_position(BodyId)         -> scratch[body_id].cached_position
    JoltMiddleman::get_rotation(BodyId)         -> scratch[body_id].cached_rotation
  is_active <- JoltMiddleman::is_sleeping(BodyId)
  body.sleeping <- !is_active
  if !is_active and not Sleeping marker present:
    add Sleeping marker; reset sleep_frames
  else if is_active and Sleeping marker present:
    remove Sleeping marker
  else if !is_active:
    sleep_frames += 1               # advance the §3.7 counter
```

Three properties this seam guarantees:

1. **No mid-substep cross-traffic.** The two barriers above are the
   only legal transit points; a debug-build assertion fires if any
   ECS read or write happens between them (§4.1.4 invariant 2,
   `physics::Error::SubstepEcsCommitInverted`). The
   `entry_barrier_flag` / `in_flight_flag` / `exit_barrier_flag`
   triple owned by the `physics-world` driver is what the assertion
   reads (`physics-world-design.md` §5.1).
2. **Iteration order is `BodyId` ascending.** Not archetype-chunk
   order, not Jolt's internal pair-list order, not wall-clock
   contact-listener fire order. The handle is the determinism key
   (§4.1.5b invariant 1 + §4.2 invariant 8); a host that re-orders
   the walk reproduces the same snapshot bytes only by accident.
3. **Drain is total.** `ExternalForce` / `ExternalTorque` reset to
   zero at substep entry (§4.1.4 invariant 3); the post-step cache
   on the per-archetype scratch is overwritten in place at every
   substep; nothing leaks across substep boundaries. The `Sleeping`
   marker change is committed inside the exit barrier so phase 5+
   readers see the post-step ground truth.

### 3.7 `Sleeping` marker + sleep-frame counter (§4.1.14; covers R-4.1.6)

Jolt owns the wake decision (it tracks linear / angular speed
against `PhysicsConfig.sleep_*` thresholds; harmonius design
"Sleep System"). The cluster's responsibility is twofold:

1. **Mirror Jolt's `IsActive()` into the ECS `Sleeping` marker.**
   The exit barrier (§3.6.2) reads `JoltMiddleman::is_sleeping` per
   body and adds / removes the `Sleeping` marker. The marker is a
   **tag** ECS component (zero-byte payload, §5: `struct Sleeping {};`);
   its presence is the wire-level signal.
2. **Track per-body `sleep_frames` for snapshot.** A 16-bit counter
   on the per-archetype scratch row (§3.2 cache fields are extended
   with `sleep_frames` in the bodies module) advances by 1 every
   exit-barrier whose `is_sleeping == true`, resets to 0 when the
   body wakes. The counter is captured into
   `PhysicsSnapshot.body_sleep_frames` (§7.1.4 tag 12) so a snapshot
   restore re-derives the same wake trajectory bit-for-bit.

**Invariants** (§4.1.14 authoritative):

1. **Wake decision is Jolt's.** The cluster never decides "this body
   should sleep"; it observes. Sleep thresholds (`PhysicsConfig.
   sleep_linear_speed`, `sleep_angular_speed`, `sleep_frame_count`)
   are the only inputs; they live on the immutable
   `PhysicsConfig` (§4.1.2 invariant 1) and the `physics-world`
   cluster authors them at world create.
2. **Marker presence is the public signal.** ECS systems query for
   `Sleeping` via core's `Query<&RigidBody, Without<Sleeping>>` to
   iterate active bodies. The marker is the **only** public
   sleep-state surface; the `RigidBody.sleeping` boolean is the
   **mirror cache** that survives across snapshots
   (`SnapshotBody.sleeping` tag 13) and is the discriminator for the
   add / remove decision in §3.6.2.
3. **`sleep_frames` is sleep-time-since-last-wake, not total
   sleep-time.** Wake events reset the counter to zero. Snapshots
   carry the post-step value at capture time (§4.1.12 invariant 5
   ordering).
4. **Wake events are not ECS events.** The cluster does not emit
   "WakeEnter / WakeExit" events; the marker presence change is the
   signal. (Contact events that imply a wake — `CollisionStarted`
   on a previously-sleeping pair — are the contact aggregate's
   responsibility, §4.1.8.)

### 3.8 `ShapeBlob` decode dispatcher (§4.1.6 + §7.1.2; covers R-4.2.3 / R-4.2.4 / R-4.2.5)

The `ShapeBlob` is the cooked, content-hashed, immutable bytes
authored at cook time and consumed at first reference inside the
`PhysicsWorld`'s shape table. Per §7.1.2 the wire schema is a sealed
sum over seven `ShapeKind` ordinals (Sphere, Box, Capsule,
ConvexHull, TriangleMesh, Heightfield, Compound). The cluster's
`shapes/shape_blob.cpp` is the decode dispatcher: it reads the
already-Fory-decoded `ShapeBlobRecord`, dispatches by `kind`, and
asks the middleman to construct the corresponding Jolt `Shape`.

```cpp
// physics/src/shapes/shape_blob.cpp — pseudocode for the dispatcher
Result<JoltShapePtr> decode_to_jolt(const ShapeBlobRecord& rec,
                                    JoltMiddleman& mm,
                                    const ShapeTable& table /* for Compound child resolve */) noexcept {
    if (rec.content_hash != recompute_blake3(rec)) {
        return std::unexpected{physics::Error::ShapeBlobMalformed};
    }
    switch (static_cast<ShapeKind>(rec.kind)) {
        case ShapeKind::Sphere:       return decode_sphere(rec, mm);
        case ShapeKind::Box:          return decode_box(rec, mm);
        case ShapeKind::Capsule:      return decode_capsule(rec, mm);
        case ShapeKind::ConvexHull:   return decode_convex_hull(rec, mm);
        case ShapeKind::TriangleMesh: return decode_triangle_mesh(rec, mm);
        case ShapeKind::Heightfield:  return decode_heightfield(rec, mm);
        case ShapeKind::Compound:     return decode_compound(rec, mm, table);
    }
    return std::unexpected{physics::Error::ShapeBlobMalformed};  // unknown ordinal
}
```

#### 3.8.1 `Sphere` (kind 0)

- **Tags read.** `tag 3` (`sphere_radius`).
- **Validation.** `radius > 0`; non-NaN; non-Inf. Failure ⇒
  `ShapeBlobMalformed`.
- **Jolt mapping.** `JPH::SphereShapeSettings(radius).Create()`.
- **Auto-inertia hook.** Volume = `(4/3) π r³`; mass = volume *
  density; principal inertia diagonal = `(2/5) m r²` on each axis.
- **Memory cost.** Constant — Jolt's `SphereShape` is a single-cache-
  line struct.

#### 3.8.2 `Box` (kind 1)

- **Tags read.** `tag 4` (`box_half_extents`).
- **Validation.** Each component `> 0`; all finite. Failure ⇒
  `ShapeBlobMalformed`.
- **Jolt mapping.** `JPH::BoxShapeSettings(half_extents).Create()`.
- **Auto-inertia hook.** Volume = `8 hx hy hz`; mass = volume *
  density; inertia diagonal computed by Jolt's `MassProperties::
  SetMassAndInertiaOfSolidBox`.
- **Memory cost.** Constant; Jolt stores three `f32` half-extents.

#### 3.8.3 `Capsule` (kind 2)

- **Tags read.** `tag 5` (`capsule_radius`), `tag 6`
  (`capsule_half_height`).
- **Validation.** Both `> 0`; both finite. Failure ⇒
  `ShapeBlobMalformed`.
- **Jolt mapping.** `JPH::CapsuleShapeSettings(half_height,
  radius).Create()`.
- **Auto-inertia hook.** Cylinder + two hemispheres; Jolt computes
  the diagonal via `MassProperties::SetMassAndInertiaOfCapsule`.
- **Memory cost.** Constant.

#### 3.8.4 `ConvexHull` (kind 3)

- **Tags read.** `tag 7` (`hull_vertices`), `tag 8`
  (`hull_plane_indices`).
- **Validation.** `len(hull_vertices) >= 4`; every vertex finite;
  the cook tool's pre-baked plane indices satisfy Jolt's
  `ConvexHullBuilder::Result::Success` shape (validated by Jolt
  during construction; failures bubble out as
  `ShapeBlobMalformed`). The cluster does **not** re-derive the
  hull at runtime — quickhull is post-MVP (§2.3 row R-4.2.12).
- **Jolt mapping.**
  `JPH::ConvexHullShapeSettings(vertices, max_convex_radius=0.05f)
  .Create()`. The `max_convex_radius` knob is pinned at 0.05 m
  (Jolt's default) — making it per-blob is post-MVP (§12 OQ-2).
- **Auto-inertia hook.** Jolt computes mass / centre-of-mass /
  inertia from the hull's tetrahedral decomposition.
- **Memory cost.** `O(n)` in vertex count; Jolt stores compressed
  vertices + face plane equations.

#### 3.8.5 `TriangleMesh` (kind 4; covers R-4.2.4 with per-triangle materials)

- **Tags read.** `tag 9` (`mesh_vertices`), `tag 10`
  (`mesh_indices`), `tag 11` (`mesh_materials`).
- **Validation.** `len(mesh_indices) % 3 == 0`; every index `<
  len(mesh_vertices)`; either `len(mesh_materials) == 0` (uniform
  material from the `Collider`) or `len(mesh_materials) ==
  len(mesh_indices) / 3` (per-triangle material). Failure ⇒
  `ShapeBlobMalformed`.
- **Jolt mapping.**
  `JPH::MeshShapeSettings(triangles, materials).Create()` where
  `triangles` is built from indices+vertices and the per-triangle
  user-data carries the `MaterialId` ordinal. Jolt builds an
  internal axis-aligned-box-tree at construction time; the build
  cost is amortised against the cook step (the triangle data is
  already meshlet-clustered by `geometry`, so the tree is not the
  bottleneck).
- **Restrictions.** Triangle meshes may attach only to
  `MotionType::Static` bodies (Jolt 2025 contract — dynamic
  triangle-mesh bodies are not deterministic across hosts under
  the cross-platform-determinism mode that
  `physics-world-design.md` §6.1 rule 1 pins). Attaching a
  TriangleMesh to a Dynamic body returns
  `physics::Error::ConfigInvalid` at `add_body` time.
- **Auto-inertia hook.** N/A — Static bodies have undefined inertia
  (Jolt skips the integration).
- **Memory cost.** `O(n)` in index count; `geometry`'s meshlet
  layout keeps the per-MB-of-mesh cost predictable.

#### 3.8.6 `Heightfield` (kind 5; covers R-4.2.4 heightfield half)

- **Tags read.** `tag 12` (`heightfield_extent_x`), `tag 13`
  (`heightfield_extent_z`), `tag 14` (`heightfield_scale`),
  `tag 15` (`heightfield_samples`).
- **Validation.** `extent_x * extent_z == len(heightfield_samples)`;
  `extent_x, extent_z > 0`; samples finite. Failure ⇒
  `ShapeBlobMalformed`.
- **Jolt mapping.** `JPH::HeightFieldShapeSettings(samples,
  extent_x, extent_z, scale).Create()`. Per-cell material indices
  are post-MVP — the MVP heightfield uses one material from the
  `Collider`.
- **Restrictions.** Same Static-only restriction as TriangleMesh.
- **Memory cost.** `O(extent_x * extent_z)` in float samples; Jolt's
  internal tree is an additional `~1.5x` overhead.

#### 3.8.7 `Compound` (kind 6; covers R-4.2.5)

- **Tags read.** `tag 16` (`compound_child_hashes`), `tag 17`
  (`compound_child_positions`), `tag 18` (`compound_child_rotations`).
- **Validation.** All three lists same length, length `>= 1`; every
  child hash resolves in the per-world `ShapeTable` (§3.9). A child
  hash whose ShapeBlob is not in the table at the time of compound
  decode returns `physics::Error::ShapeBlobMissing` (§7.1.2
  invariant 4 — children resolve at load, not at decode).
- **Jolt mapping.** `JPH::StaticCompoundShapeSettings` with each
  child's `(ShapeRefC, position, rotation)`. The compound is
  immutable post-construction (Jolt's `MutableCompoundShape` is
  refused — runtime topology mutation is post-MVP `destruction`,
  §2.3 row "soft-body / cloth").
- **Recursive compounds.** Forbidden in MVP — a compound's children
  must be primitive / hull / mesh / heightfield kinds, not other
  compounds. This collapses §4.1.6 invariant 5 into a single
  load-time check (children-of-Compound check at the dispatcher).
  Post-MVP nested compounds dock against an OQ in §12.
- **Memory cost.** `O(n)` in child count plus the children's own
  costs (which are already in the table).

#### 3.8.8 Decode is total per kind

The §7.1.2 sealed sum (`ShapeKind` ordinals 0..6) closes the dispatch.
A reader observing an out-of-range ordinal returns
`physics::Error::ShapeBlobMalformed` (§10.1 row); the future-build
case (a newer cook ships an ordinal this build does not know) is the
same arm — the cluster does not differentiate "unknown kind" from
"corrupted bytes". The §7.2.2 schema-version migration path covers
the future-build case explicitly via `ShapeBlobVersionUnsupported`
(§10.1) when the `ShapeBlobRecord.schema_version` is the newer-build
discriminator.

### 3.9 `ShapeTable` (§4.1.6; covers content-hashed shape sharing)

Per-`PhysicsWorld` content-hash table mapping
`ShapeBlobRecord.content_hash` → one row holding the resolved Jolt
`Shape*` pointer plus the refcount. The handle the cluster returns
(`ShapeHandle`) is a 32-bit index into this table; two `Collider`s
referencing the same content hash share one row (§4.1.6 invariant 1,
§7.1.2 invariant 5).

```cpp
// physics/src/shapes/shape_table.hpp — internal view
namespace glibre::physics::detail {

struct ShapeTableRow {
    std::uint64_t  content_hash    = 0u;   // §7.1.2 invariant 1; primary key
    std::uint16_t  schema_version  = 0u;   // §7.1.2 schema version at decode
    std::uint8_t   kind            = 0u;   // ShapeKind ordinal
    std::uint8_t   _reserved       = 0u;
    std::uint32_t  refcount        = 0u;   // §4.1.6 invariant 1
    JoltShapePtr   resolved        = {};   // private to the outgoing dylib's image (§8 row 6)
};

class ShapeTable {
public:
    [[nodiscard]] static Result<eastl::unique_ptr<ShapeTable>>
        create(std::uint32_t max_shapes) noexcept;

    [[nodiscard]] Result<ShapeHandle>
        intern(const ShapeBlob& blob, JoltMiddleman& mm) noexcept;

    [[nodiscard]] Result<void>
        release(ShapeHandle) noexcept;

    [[nodiscard]] Result<JoltShapePtr>
        resolve(ShapeHandle) const noexcept;

    [[nodiscard]] std::uint64_t
        content_hash_of(ShapeHandle) const noexcept;

    // Cross-reload — re-intern surviving body_shape_blob_hashes from
    // the carrier snapshot; rebuilds the resolved Jolt Shape* per row.
    [[nodiscard]] Result<void>
        restore_from_snapshot(eastl::span<const std::uint64_t> body_shape_blob_hashes,
                              JoltMiddleman& mm) noexcept;

    ~ShapeTable();
    ShapeTable(const ShapeTable&)            = delete;
    ShapeTable& operator=(const ShapeTable&) = delete;

private:
    ShapeTable() noexcept;

    std::uint32_t                                        max_     = 0u;
    eastl::vector<ShapeTableRow>                         rows_;        // index = ShapeHandle.raw() - 1
    eastl::vector<std::uint32_t>                         free_list_;   // sort-on-pop, same policy as BodyIdAllocator (§3.3.2)
    eastl::hash_map<std::uint64_t, std::uint32_t>        by_hash_;     // content_hash → ShapeHandle.raw()
};

}  // namespace glibre::physics::detail
```

**Invariants** (§4.1.6 authoritative):

1. **Content-hash is the primary key.** `intern(blob)` first looks
   up `by_hash_`; if present, increments the row's refcount and
   returns the existing handle. If absent, decodes the blob (§3.8),
   allocates a row, stores the resolved `Shape*` pointer, and
   inserts. Two distinct hashes never share a row (§4.1.6 invariant
   1, §7.1.2 invariant 5).
2. **Refcount drops free the row.** `release(handle)` decrements;
   refcount → 0 destroys the resolved `Shape*` (Jolt's `ShapeRefC`
   destructor analog through the middleman) and erases the
   `by_hash_` entry. The row's index stays in `rows_` but is added
   to `free_list_` so the next intern can reuse it (matching the
   BodyId allocator's sort-on-pop policy in §3.3.2).
3. **Resolved `Shape*` is private to the outgoing dylib.** The
   pointer never crosses the §4.1.13 ABI seam (§4.1.6 invariant 1 +
   §8.2 row "ShapeHandle resolved Jolt `Shape*`"). Hot-reload
   resume (§8) walks every row whose refcount is non-zero and
   re-invokes the §3.8 dispatcher to rebuild the pointer; the
   `content_hash` + `kind` + `schema_version` columns survive (they
   are middleman-typed bytes inside the snapshot's
   `body_shape_blob_hashes` column and the surviving
   `ShapeBlobRecord` table).
4. **Stale handles fail loudly.** `resolve(handle)` returns
   `physics::Error::ShapeHandleStale` when the row's refcount is
   zero, even if the row index has not been reused (§10.1 row).
   This catches the "cached handle across world destroy / recreate"
   bug shape.
5. **Bounded by `PhysicsConfig::max_shapes`.** Insert past the
   ceiling returns `physics::Error::BudgetExceeded` (§10.1).

### 3.10 Cross-context handoffs

Bodies-shapes is one slice of the physics plugin; the four
cross-context seams it honours, in addition to the
`physics-world` cluster's broker contract:

1. **`render` reads body transforms via core's ECS components — not
   via this cluster.** Phase 5 (`core` transform propagation) lifts
   the `RigidBody` row's position / rotation into the entity's
   `GlobalTransform` (§3.2 invariants); phase 6 (`render`
   `cull-extract`) reads `GlobalTransform` + `PreviousGlobalTransform`
   like any other entity's transform. The cluster never exposes a
   "give me a body's transform" entry point; the seam is core's
   component, not this cluster's surface (`physics-world-design.md`
   §6.5 seam #1).
2. **`render` BLAS handoff via shape `content_hash`.** Static
   `RigidBody` entities with TriangleMesh / Heightfield colliders
   carry a `ShapeHandle` whose underlying `ShapeBlob.content_hash`
   matches the visual mesh's BLAS key in `geometry`'s asset bundle
   (`specs/render/SPEC.md` §4.1.8 + §3.10 above). The cluster's
   contract is "the hash physics holds is the hash render sees";
   the BLAS bytes themselves are render-owned and rebuilt in
   render's domain on the `PhysicsWorldReplaced` event
   (`physics-world-design.md` §8.5).
3. **`content` / `geometry` cooks the `ShapeBlob` bytes.** Per §3.3
   refusal "Shape authoring, mesh-collider authoring, heightfield
   authoring", the cluster never re-bakes shapes. A `ShapeBlob`
   whose `kind` is one of {ConvexHull, TriangleMesh, Heightfield,
   Compound} arrives at `intern_shape` as cooked output; if the
   bytes would require runtime baking the dispatcher returns
   `physics::Error::ShapeBlobMalformed` (§7.1.2 invariant 3).
4. **`tools` (editor) writes `RigidBody` / `Collider` components.**
   Editor authoring of physics state happens in phase 1 or phase 2
   (per `frame-phases.md` open question 3); the cluster's mirror
   (§3.6) reads the resulting components at the next phase 3 entry
   barrier without distinction between "editor wrote" and "gameplay
   wrote" — the seam is the ECS write, not the authoring path.

## 4. Public surface

The §5 facade (SPEC §5; locked) is the contract every caller compiles
against. This section maps the SPEC §5 declarations onto the §3
internal model and pins three usage rules the SPEC §5 preamble does
not state in one place. **No new public surface is introduced here**;
deviations would require a SPEC §5 amendment spike, not an in-place
edit.

### 4.1 SPEC §5 → §3 mapping (cluster slice)

| SPEC §5 declaration                                            | §3 internal owner                                          | Notes                                                                                                       |
|----------------------------------------------------------------|------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------|
| `struct RigidBody`                                             | §3.2 `RigidBodyView`                                       | POD ECS component. Layout-locked; the cluster's mirror cache lives off-component.                            |
| `struct Collider`                                              | §3.5 `ColliderView`                                        | POD ECS component. One per body in MVP (§3.5 invariant 1).                                                  |
| `struct Trigger`                                               | §3.5 + sibling `contact/`                                  | Marker-tag component; flips `Collider::is_trigger` semantics. Listener drain is sibling.                    |
| `struct Velocity` / `AngularVelocity` / `ExternalForce` / `ExternalTorque` | §3.6 mirror barriers                            | POD companion components; written at entry / read at exit.                                                  |
| `struct Sleeping`                                              | §3.7 `Sleeping` mirror                                     | Marker-tag component; presence is the public sleep signal.                                                  |
| `using BodyId = Handle<tags::body, std::uint32_t>`             | §3.3 `BodyIdAllocator`                                     | 32-bit handle; `BodyId{0}` is the invalid sentinel (§3.3 invariant 1).                                      |
| `using ShapeHandle = Handle<tags::shape, std::uint32_t>`       | §3.9 `ShapeTable`                                          | 32-bit handle; `ShapeHandle{0}` is invalid; refcount-zero handle reuse is `ShapeHandleStale`.                |
| `using MaterialId = Handle<tags::material, std::uint32_t>`     | sibling `physics-world` cluster (`PhysicsConfig` + material-table TU) | Material-asset references on `Collider`; the table is per-world alongside `PhysicsConfig`; this cluster carries only the 32-bit ordinal on `Collider`. |
| `using CollisionLayer = Handle<tags::collision_layer, ...>`    | sibling `physics-world` `LayerFilter`                      | The cluster carries the layer ordinal on `Collider`; the matrix lives on `PhysicsConfig`.                   |
| `enum class MotionType`                                        | §3.2                                                       | Sealed three-value sum; pinned at create.                                                                   |
| `struct ShapeBlob`                                             | §3.8 dispatcher                                            | The public POD; the bytes are decoded into the §3.9 table.                                                  |
| `struct PhysicsMaterial`                                       | sibling `physics-world` cluster                            | Asset-side type; decoded and interned by the `physics-world` material-table TU. This cluster carries only the `MaterialId` reference on `Collider`. |
| `PhysicsWorld::add_body(Entity, const RigidBody&, const Collider&)` | §3.3 `allocate` + §3.9 `intern` (lazy via Collider.shape) | Forwards through the `physics-world` facade; the cluster authors the bytes inside.                          |
| `PhysicsWorld::remove_body(BodyId)`                            | §3.3 `release` + §3.6 mirror cleanup                       | Refuses if a joint endpoint is live (sibling joint registry's check; arm `BodyStillReferencedByJoint`).      |
| `PhysicsWorld::intern_shape(const ShapeBlob&)`                 | §3.9 `intern`                                              | The shape-table entry; refcount-incremented on duplicate hashes.                                            |
| `PhysicsWorld::release_shape(ShapeHandle)`                     | §3.9 `release`                                             | Refcount-decremented; row freed when refcount → 0.                                                          |
| `PhysicsWorld::intern_material(const PhysicsMaterial&)`        | sibling `physics-world` cluster                            | Material-table entry owned by the `physics-world` cluster alongside `PhysicsConfig`; not authored by this cluster. |

### 4.2 Three cluster-specific usage rules

1. **`add_body` wires `Collider.shape` lazily; the caller must intern
   the shape first.** The §5 facade's `add_body(entity, rigid_body,
   collider)` accepts a `Collider` whose `shape` field is already a
   live `ShapeHandle` (§3.9). Internally `add_body` does **not**
   intern from a fresh `ShapeBlob`; the caller calls
   `PhysicsWorld::intern_shape(blob)` first, stores the returned
   handle on the `Collider` POD, and then calls `add_body`. This
   keeps the body-add path single-responsibility (it does not mix
   shape decode with body allocation) and matches the SPEC §5
   signature — a `Collider`'s `shape` field is a `ShapeHandle`, not
   a `ShapeBlob`.
2. **`remove_body` is refused while a joint endpoint is live.** Per
   §3.5 invariant + §4.1.7 invariant 1, body removal returns
   `physics::Error::BodyStillReferencedByJoint` if any joint still
   references the body. The cluster does not consult the joint
   registry directly — the `physics-world` facade asks the
   `joints/` aggregate first, and only on a clear answer does the
   bodies-shapes mirror release the body. The result is one error
   arm at one detection point; the cluster authors the cleanup
   (`BodyIdAllocator::release` + `JoltMiddleman::remove_body` +
   per-archetype scratch row drop) but not the gating.
3. **`Collider`'s `shape` field is mutable post-create; `RigidBody`'s
   `motion_type` is not.** Per §3.5 invariant 3 vs. §3.2 invariant
   2: an entity may swap its collider's shape (re-intern + write
   the new handle into the component) and the next entry barrier
   pushes it to Jolt; an entity may **not** flip its `MotionType`.
   Mass / inertia / damping are similarly mutable (the entry
   barrier reads them per substep) but `auto_inertia` re-derive
   only fires at create-time (§3.2 invariant 5).

## 5. Hot/cold path split

The cluster is touched once per substep on the game-loop driver
thread (`physics-world-design.md` §6) plus episodically at body /
shape spawn / despawn time. The critical hot path is the per-substep
mirror walk (`bodies/rigid_body.cpp::commit_ecs_to_jolt` /
`commit_jolt_to_ecs`); the cold paths are body lifecycle (`add_body`
/ `remove_body`), shape lifecycle (`intern_shape` / `release_shape`),
and the auto-inertia derive (one-shot at body create).

### 5.1 Hot fields (touched per substep, in inner loops)

Every field below appears on the path from the entry barrier through
the exit barrier. They live within the first cache line of their
owning struct (`alignas(64)`) and are read-mostly during the substep.

| Owner                              | Field                                                     | Why hot                                                                                                |
|------------------------------------|-----------------------------------------------------------|---------------------------------------------------------------------------------------------------------|
| `RigidBody` (ECS column)           | `body_id` (`u32`), `motion_type` (`u8`)                   | Read at every entry / exit barrier; both fields the discriminator for the per-row branch.              |
| `RigidBody` (ECS column)           | `linear_damping`, `angular_damping` (`f32 × 2`)           | Forwarded to Jolt every entry barrier (Jolt accepts the values per substep).                            |
| `RigidBody` (ECS column)           | `ccd` (`bool`)                                            | Read once per substep on the entry barrier's debug-build assertion that Static / Kinematic don't carry CCD. |
| `RigidBody` (ECS column)           | `sleeping` (`bool`)                                       | Read at the exit barrier when deciding `Sleeping` marker add / remove.                                  |
| `Velocity` / `AngularVelocity` (ECS columns) | `v` (`Vec3`), `w` (`Vec3`)                          | Drained per substep entry (Kinematic case); written per substep exit.                                   |
| `ExternalForce` / `ExternalTorque` (ECS columns) | `force`, `torque` (`Vec3 × 2`)                  | Drained-and-zeroed per substep entry; never read at exit.                                               |
| `Collider` (ECS column)            | `shape` (`ShapeHandle`), `is_trigger` (`bool`)            | Read at body-add to wire the Jolt shape; read at every entry barrier's debug assertion that the resolved Jolt `Shape*` pointer matches the handle's row. |
| `BodyIdAllocator`                  | `next_id_`, `free_list_.size()` (`u32 × 2`)               | Read-only per substep; written only at body-add / body-remove (cold path).                              |
| `ShapeTable`                       | `rows_[i].resolved` (`JoltShapePtr`)                      | Read once per body at body-add; cached on the per-archetype scratch (§3.2) so the per-substep barrier reads the cached pointer, not the table. |

The body's hot prefix fits in a single 64 B cache line:

```cpp
static_assert(offsetof(RigidBody, mass) >= 16,
              "RigidBody hot prefix must precede the cold (mass / inertia) suffix");
static_assert(sizeof(RigidBody) <= 64,
              "RigidBody must fit in one cache line (perf-budget physics-row, §9.3 Jolt body slice)");
```

### 5.2 Cold fields (touched at construction, reload, snapshot)

| Owner                              | Field                                                     | Why cold                                                                                                |
|------------------------------------|-----------------------------------------------------------|---------------------------------------------------------------------------------------------------------|
| `RigidBody` (ECS column)           | `mass`, `inertia_diagonal`, `auto_inertia`                | Read once at body-add (`auto_inertia` triggers a one-shot derive); written once at create; per-substep barriers do not read these. |
| `Collider` (ECS column)            | `offset_position`, `offset_rotation`, `density_override`  | Read at body-add (Jolt builds the `BodyCreationSettings` from these); never per-substep.                |
| `Collider` (ECS column)            | `layer`, `material`                                       | Read at body-add (Jolt's `BodyCreationSettings::mObjectLayer` + per-shape material map); per-substep barriers do not read. |
| `BodyIdAllocator`                  | `id_to_entity_`, `free_list_` (the vectors themselves)    | Mutated at allocate / release; per-substep barriers do not touch.                                       |
| `ShapeTable`                       | `rows_[i].content_hash`, `kind`, `schema_version`, `refcount` | Read at intern / release / restore; per-substep barriers do not read.                              |
| `ShapeTable`                       | `by_hash_`                                                | Hash-table lookup at intern only.                                                                       |
| Sleep counter (per-archetype scratch) | `sleep_frames` (`u16`)                                  | Written per-exit-barrier when sleeping; read at snapshot capture only (the snapshot row's value is what survives, not the in-memory counter). |

### 5.3 Layout enforcement

Build-time `static_assert`s on the public component layouts:

```cpp
// physics/include/glibre/physics/physics.hpp — already part of §5 facade
static_assert(sizeof(RigidBody)        <= 64, "RigidBody hot fits one line");
static_assert(sizeof(Collider)         <= 64, "Collider hot fits one line");
static_assert(sizeof(Velocity)         == 12, "Velocity is one Vec3");        // 3 × f32
static_assert(sizeof(AngularVelocity)  == 12, "AngularVelocity is one Vec3");
static_assert(sizeof(ExternalForce)    == 12, "ExternalForce is one Vec3");
static_assert(sizeof(ExternalTorque)   == 12, "ExternalTorque is one Vec3");
static_assert(sizeof(Sleeping)         <= 1,  "Sleeping is a marker tag");
static_assert(sizeof(Trigger)          <= 1,  "Trigger is a marker tag");
```

The `<= 1` for marker tags accommodates the EBO-friendly empty
struct shape (`sizeof(empty struct) == 1` per the standard); the ECS
archetype storage strips the byte for tag components per core's
sparse-set rule (`specs/core/SPEC.md`).

## 6. Concurrency

MVP runs every system on the **game-loop driver thread** (SPEC §6.6;
`reviews/decisions/perf-budget.md` Pipelined Frame Timing). The
cluster's concurrency surface is exhaustively small.

### 6.1 Phase-by-phase admissibility

| Phase | Cluster ops admitted in MVP                                                                                                       |
|-------|------------------------------------------------------------------------------------------------------------------------------------|
| 1 Input        | `add_body` / `remove_body` / `intern_shape` / `release_shape` (input maps to gameplay-spawn intents). Read-only `Collider.shape` resolution via `ShapeTable::resolve`.            |
| 2 Logic        | Reserved slot; deferred body in MVP. Future gameplay-plugin systems will write `ExternalForce` / `ExternalTorque` / kinematic targets and may spawn / despawn bodies here.       |
| 3 PhysicsFixed | **Mirror barriers run inside the `physics-world` driver's substep loop** (§3.6). The cluster authors the per-body walks; admission is implicit (the driver is the only legal caller). `add_body` / `remove_body` are admitted at substep entry **before** the entry barrier and at substep exit **after** the exit barrier (the driver brackets them); admission inside the in-flight window is refused with `physics::Error::SubstepEcsCommitInverted`. |
| 4 Animation    | Reserved slot; deferred body in MVP.                                                                                                |
| 5 Transform    | Read-only `Collider.shape` / `ShapeTable::resolve` via core's transform propagation when consumers need shape AABBs (rare; cull-extract usually keys on `GlobalTransform`).   |
| 6 CullExtract  | Read-only `ShapeTable::content_hash_of` (render uses the hash for BLAS keying — `physics-world-design.md` §6.5 seam #3).            |
| 7 RenderSubmit | No cluster reads.                                                                                                                  |
| 8 HotReload    | Cluster is the **author** of restore-time `add_body` / `intern_shape` calls during `glibre_plugin_register` (§8). The loader holds exclusive ownership; no other system runs.   |
| 9 Present      | No cluster reads.                                                                                                                  |

### 6.2 Read-only operations

May run in any phase 1, 5, 6 (and 8 under loader exclusivity).
Read-only against shape table + body allocator state; no exclusive
lock required.

- `ShapeTable::resolve(handle)` — returns the resolved Jolt
  `Shape*` (and the §10 `ShapeHandleStale` arm if refcount is zero).
- `ShapeTable::content_hash_of(handle)` — returns the row's
  `content_hash`; primary use is render's BLAS keying.
- `BodyIdAllocator::in_use()`, `high_water()` — diagnostic; the
  editor / profiler reads them.

### 6.3 Read-write operations

Run only in phases 1, 2, 3 (substep entry/exit windows only), and 8
(hot-reload restore). Forbidden in phases 4–7 / 9 because those
phases' read paths would observe partial state.

- `PhysicsWorld::add_body` — `BodyIdAllocator::allocate` +
  middleman `create_body` + per-archetype scratch insert + entry
  barrier's first commit happens at the next substep entry.
- `PhysicsWorld::remove_body` — middleman `remove_body` +
  `BodyIdAllocator::release` + per-archetype scratch erase. Refused
  by sibling joint registry first if any endpoint is live.
- `PhysicsWorld::intern_shape` — `ShapeTable::intern` +
  per-`ShapeKind` decode dispatcher + middleman `Shape` create.
  `O(1)` for cache-hit (existing hash); `O(decode time)` for new
  hashes — the decode itself is bounded by the `ShapeBlob`'s data
  size and runs at most once per unique hash.
- `PhysicsWorld::release_shape` — `ShapeTable::release` plus, on
  refcount → 0, middleman shape destroy.

The cluster maintains no internal worker thread (Jolt's `JobSystem`
is owned by `physics-world` per `physics-world-design.md` §6.5);
all state is driver-thread-local in MVP.

### 6.4 Memory ordering

Every public method on `PhysicsWorld` (the bodies-shapes slice) is
`noexcept` and assumes single-threaded access (the game-loop driver
thread; SPEC §6.6). The `ShapeTable::resolve` accessor is
`const` but not internally synchronised; cross-thread reads of the
shape table from a future post-MVP parallel mirror seam will
require either the relaxed-atomic-pointer upgrade path the
`physics-world` design reserves (`physics-world-design.md` §6.4
note 2) or a per-row read-write split (deferred to the post-MVP
parallelism spike).

The `BodyIdAllocator::allocate` / `release` calls update two
`eastl::vector`s and one `eastl::hash_map` (in the shape table)
non-atomically; this is safe because phase 3 substep barriers
serialise all access through the driver thread.

### 6.5 Determinism guarantees

The cluster contributes three guarantees to the §4.2 invariant 3
"byte-equal across hosts" promise:

1. **`BodyId` allocation order is wire-deterministic.** The
   allocator's output is a pure function of the call sequence
   (§3.3 invariant 2); the call sequence is determined by ECS
   materialisation order (which core guarantees per PHILOSOPHY
   §7). Two hosts running the same trace produce the same
   `BodyId` for the same entity.
2. **Free-list pop is `BodyId`-ascending.** §3.3.2 sort-on-pop
   makes id reuse deterministic across hosts. A trace whose
   `release` calls arrive in different orders on two hosts (which
   should not happen if the trace is well-formed) would diverge —
   this is what the §11 acceptance test for #434 (BodyId stable
   across hosts) checks.
3. **Shape-table iteration order is `content_hash`-ascending.**
   When the §3.9 table walks all rows for snapshot-time iteration
   (`body_shape_blob_hashes` capture per §7.1.4 invariant 5), the
   walk sorts by `content_hash` before emitting. The hash is
   itself a 64-bit BLAKE3 prefix — deterministic by construction.

The §6 mirror barriers' `BodyId`-ascending walk (§3.6) is the
fourth determinism cell; it lives on the `physics-world` driver,
not in this cluster, but the cluster's contribution is the
per-archetype scratch span the driver sorts.

## 7. Persistence + ABI

The cluster's persistence surface is split between **physics-owned**
schemas (`ShapeBlobRecord`; the body / shape columns of
`PhysicsSnapshot`) and **sibling-owned** schemas (`PhysicsConfigRecord`,
`JointDescriptorRecord`, the joint columns of `PhysicsSnapshot`).
Authority is SPEC §7; this section documents only the cluster's
slice.

### 7.1 Schemas this cluster owns

#### 7.1.1 `ShapeBlobRecord` (SPEC §7.1.2)

The cooked collision-geometry payload. Authored under
`data/schemas/physics/ShapeBlobRecord.fory`; FQN
`glibre.physics.ShapeBlobRecord`. Field roster + invariants are SPEC
§7.1.2; no field is added or removed by this design.

The cluster's role:

1. **Read** the record at `intern_shape` (§3.8 dispatcher), via the
   value-typed `ShapeBlob` parameter populated by the `data` codec.
2. **Validate** the `content_hash` against a recompute (§7.1.2
   invariant 1); mismatch ⇒ `physics::Error::ShapeBlobMalformed`.
3. **Dispatch** by `kind` (§7.1.2 sealed sum) to the per-kind decode
   path (§3.8.1–§3.8.7) and produce a Jolt `Shape*` via the
   middleman.
4. **Insert** the resolved pointer into the shape table (§3.9) keyed
   by `content_hash`; refcount-incremented on duplicate hashes.
5. **Author** the migration body for any `vN → vN+1` schema bump
   under `physics/src/migrations/migrate_ShapeBlobRecord_v<N>_to_v<N+1>.cpp`,
   per `reviews/decisions/hot-reload-protocol.md` §"Migrate Function
   Contract" + SPEC §7.2.2. The cluster ships **no** migration body
   for v1 — there is no v1→v2 chain in MVP.

#### 7.1.2 `PhysicsSnapshot` body + shape columns (SPEC §7.1.4)

The snapshot codec is the sibling `snapshot/` aggregate's
responsibility (SPEC §4.1.12, §7.1.4). The cluster contributes nine
columns to the schema:

| `PhysicsSnapshot` field            | Source in this cluster                                                | Notes                                                                |
|------------------------------------|-----------------------------------------------------------------------|----------------------------------------------------------------------|
| `body_ids` (tag 6)                 | `BodyIdAllocator::in_use_ids()` walked ascending                       | The capture order; restore replays in the same order.                |
| `body_motion_types` (tag 7)        | `RigidBody.motion_type` per body                                      | `u8` ordinal of the §5 sealed sum.                                   |
| `body_positions` (tag 8)           | per-archetype scratch `cached_position` per body (written by exit barrier, §3.6.2) | `vec3f`; `std::bit_cast<u32>` per coordinate (SPEC §6.4 R4).          |
| `body_rotations` (tag 9)           | per-archetype scratch `cached_rotation` per body                      | `quatf`; bit-exact same way.                                          |
| `body_linear_velocities` (tag 10)  | `Velocity.v` per body                                                 | Same.                                                                 |
| `body_angular_velocities` (tag 11) | `AngularVelocity.w` per body                                          | Same.                                                                 |
| `body_sleep_frames` (tag 12)       | `sleep_frames` from per-archetype scratch (§3.7)                      | `u16`; reset on wake.                                                 |
| `body_sleeping_flags` (tag 13)     | `RigidBody.sleeping` per body                                         | Mirror of Jolt `IsActive()`.                                          |
| `body_shape_blob_hashes` (tag 14)  | `ShapeTable::content_hash_of(Collider.shape)` per body                | The shape-table-key column; references shape bytes in the asset bundle. |

These columns are **read** at `PhysicsWorld::snapshot()` (the
cluster passes them to the sibling codec); they are **written** at
`PhysicsWorld::restore()` via `add_body(...)` replay in `body_ids`
ascending order (§3.6 + §3.9 restore paths). Capture order is
`BodyId` ascending (§3.3 invariant 5 + §7.1.4 invariant 5); restore
replays in the same order so the post-restore allocator state is
byte-equal.

The other columns (`SnapshotHeader.*`, `joint_*`) are owned by the
sibling aggregates and the codec — this design treats them as opaque
carrier bytes.

### 7.2 Migration rules

The cluster authors the migration bodies for `ShapeBlobRecord` and
contributes to the `PhysicsSnapshot` body / shape column migrations.
Per `reviews/decisions/fory-codegen.md` §"Migration Mechanic" and
SPEC §7.2 the bodies are pure functions taking `(const T_vN&,
T_vNplus1&, glibre::Arena&)` and returning `Result<void>`.

#### 7.2.1 `ShapeBlobRecord` `vN → vN+1`

Three sub-cases (SPEC §7.2.2):

- **Adding a parameter to an existing kind.** Append-only at a new
  tag with `since N+1` and a default; codegen emits the identity
  migration with default-fill (`fory-codegen.md` §"Migration
  Mechanic"). No cluster body required.
- **Adding a new `ShapeKind` ordinal.** Append-only. The cluster
  ships a new decode case under §3.8 (per-kind row); the migration
  body is identity (older blobs do not carry the new kind).
- **Removing a kind.** Treated as breaking. The cluster ships the
  migration body that either re-maps to a still-shipped kind
  (lossy, total) or returns `physics::Error::ShapeBlobVersionUnsupported`
  for blobs carrying the removed kind. The decision is per-bump.

For MVP the cluster ships **no** migration bodies — the schema is
v1 with seven kinds, and no bump is on the roadmap.

#### 7.2.2 `PhysicsSnapshot` body / shape column `vN → vN+1`

Two sub-cases (SPEC §7.2.4):

- **Adding a per-body column** (e.g. a future `body_external_force`
  column to capture force accumulation across a sub-stepped frame).
  Append-only at a new tag with `since N+1` and a default
  `list<T>` zero-fill; codegen emits the identity migration that
  fills the new column to length matching `body_ids`. No cluster
  body required beyond a new `RigidBody` field if the column needs
  one.
- **Changing a column's element type.** Treated as breaking.
  The cluster authors the body under
  `physics/src/migrations/migrate_PhysicsSnapshot_v<N>_to_v<N+1>.cpp`
  and the corresponding §7.2.4 invariants (bit-exact contract,
  `f32 → f64` allowed, `f64 → f32` forbidden).

### 7.3 ABI hash sources this cluster participates in

The cluster contributes to **two** parts of the
`glibre_types_abi_hash`:

1. **`ShapeBlobRecord` source hash.** The `data/schemas/physics/
   ShapeBlobRecord.fory` file's BLAKE3 (per
   `reviews/decisions/fory-codegen.md` §"ABI Hash Function") is one
   entry in the per-schema hash list. Adding / removing / reordering
   a field in the schema bumps this entry, which bumps the global
   hash. The cluster's invariant: **the schema source bytes are
   the only place the cluster's `ShapeBlobRecord` shape is
   declared**. There is no parallel C++-only definition that could
   drift.
2. **`PhysicsSnapshot` body / shape column hashes.** Same mechanism;
   the cluster's nine columns are declared in
   `data/schemas/physics/PhysicsSnapshot.fory` (sibling
   `snapshot/` cluster's file), and the per-column BLAKE3 entries
   contribute to the global hash.

The cluster does **not** own the `glibre_types_abi_hash` exposure
itself (that is the `glibre-types.dylib` middleman per
`fory-codegen.md`); it owns only the source-of-truth schema bytes
that go into the hash.

### 7.4 What the cluster does NOT persist

Per SPEC §7.3, listed for clarity:

| Artefact                                               | Why not persisted                                                                                                  |
|--------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------|
| `ShapeHandle` (the runtime handle)                     | Per-world refcount; resolved from `ShapeBlobRecord.content_hash` at table load (§3.9 invariant 3, SPEC §7.1.2 invariant 5). |
| Resolved Jolt `Shape*` pointer                          | Owned by the outgoing dylib's image; rebuilt at hot-reload (§3.9 invariant 3, §8 row 5).                            |
| `BodyIdAllocator` private state (`next_id_`, `free_list_`) | Re-derived from the snapshot's `body_ids` column (§3.3 invariant 5).                                              |
| Per-archetype mirror cache (cached position / rotation / velocity) | Rebuilt at the next entry/exit barrier; substep-local (§3.2 cache fields).                                |
| `Sleeping` marker bytes                                | Zero-byte tag component; the bit lives on `RigidBody.sleeping`, which IS persisted (snapshot tag 13).               |
| `Trigger` marker bytes                                 | Zero-byte tag component; the bit lives on `Collider.is_trigger`, which IS persisted on the `Collider` ECS row.      |
| `MaterialId` runtime table                             | The asset-side bytes live in `data` / `content` (§3.3); the cluster carries only the 32-bit ordinal.                |
| Per-substep `BodyId` scratch span                      | Phase-3 transient arena; drains by phase 9 (`perf-budget.md` Allocator Rule #4).                                    |

These appear in the persistence surface only as **identifiers**
(`BodyId`, `ShapeHandle`, `MaterialId`, `content_hash`) referenced
from §7.1.

### 7.5 Jolt as middleman seam — no Jolt types cross

Per §4.1.13 invariant 1 + SPEC §6.1 module rule 1, no Jolt
header is reachable from `physics/src/bodies/**` or
`physics/src/shapes/**`. The cluster's `JoltShapePtr` typedef
(§3.9) is `void*`-equivalent at the public-internal seam; the
real `JPH::Shape*` lives behind `middleman/jolt_middleman.cpp`
which is the only TU compiled with Jolt headers in scope. Every
forwarding call (`mm.add_force(BodyId, Vec3)`, `mm.create_shape_*`)
crosses the middleman seam; the cluster's TUs see only the
middleman's interface.

## 8. Hot-reload

Hot-reload semantics for the physics plugin are owned by SPEC §8;
this section states what **the bodies-shapes cluster** must hold
steady across the swap, what `migrate(...)` requires of the cluster,
and the refusal cases this design contributes. Engine-wide concerns
(per-plugin atomicity, observer bus event shapes, error wrapping
rules, the `enqueue_hot_reload` E2E hook) are not re-stated here —
see SPEC §8 + `reviews/decisions/hot-reload-protocol.md`.

### 8.1 What survives the swap

Per SPEC §8.1 (hot-reload point — phase 8, never mid-frame) + SPEC
§8.2 (survival inventory), specialised to this cluster's primitives:

| Primitive (§3 ref)                          | Survives swap? | Mechanism                                                                                                                                                 |
|---------------------------------------------|----------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------|
| `RigidBody` ECS component bytes (§3.2)      | **Yes** (middleman ECS storage) | The component is middleman-typed (`glibre.types.physics.RigidBody`); the loader's archetype-storage swap preserves the bytes verbatim. Position / rotation columns survive on the **mirror cache** which is captured into `PhysicsSnapshot.body_positions` / `body_rotations` at drain time, not on the row itself.       |
| `Velocity` / `AngularVelocity` companion bytes | **Yes** (middleman ECS storage) | Same. The values are also captured into the snapshot's `body_linear_velocities` / `body_angular_velocities` columns so the post-resume mirror has the same starting values.       |
| `ExternalForce` / `ExternalTorque` companion bytes | **Yes** (middleman ECS storage) but **always zero at phase 8 entry** | Per SPEC §8.1 #2 the entry barrier drained them in frame N's last substep; bytes survive but are zero. The new plugin reads zero at its first entry barrier and the trace continues unchanged.       |
| `Sleeping` marker presence (§3.7)           | **Yes** (middleman ECS storage) | Tag component; presence bit travels with the entity.                                                                                                                                                          |
| `Trigger` marker on `Collider` (§3.5)       | **Yes** (middleman ECS storage) | `Collider.is_trigger` byte is part of the surviving component row.                                                                                                                                            |
| `BodyId` value (§3.3)                        | **Yes (re-issued)** | Bytes survive on the `RigidBody.body_id` field in middleman ECS storage. The new plugin's allocator reissues the same id by replaying `allocate(entity)` in `body_ids`-ascending order from the carrier snapshot — §4.1.5b invariant 1 promises the issued id matches.                                  |
| `BodyIdAllocator` private state              | **No**          | Rebuilt by `restore_from_snapshot` (§3.3 method) by replaying the snapshot's `body_ids` ascending walk. Post-restore `next_id_` and `free_list_` are byte-equal to pre-drain.                                  |
| `Collider` ECS component bytes (§3.5)        | **Yes** (middleman ECS storage) | `shape` ordinal, `offset_*`, `layer`, `material`, `is_trigger`, `density_override` survive verbatim. The handle's resolved Jolt `Shape*` does not.                                                            |
| `ShapeHandle` value (§3.9)                   | **Yes** (middleman ECS storage on the `Collider` row) | The 32-bit row index survives; the new plugin's shape table is rebuilt (see below) so the row at the same index after restore points at an equivalent Jolt `Shape*` pointer.                                  |
| `ShapeTable.rows_[i].resolved` (Jolt `Shape*`) | **No**         | Pointers into Jolt's `Shape` table belong to the outgoing plugin's image. Rebuilt by `ShapeTable::restore_from_snapshot` by re-interning every distinct `content_hash` referenced from the snapshot's `body_shape_blob_hashes`. (§3.9 invariant 3.)                                                       |
| `ShapeTable.rows_[i].content_hash` / `kind` / `schema_version` / `refcount` | **Yes (re-derived)** | Recomputed by the new plugin from the surviving `ShapeBlobRecord`s during restore. Bit-equal because the records survive verbatim and the BLAKE3 recompute is deterministic (§7.1.2 invariant 1, SPEC §6.4 R3 + R4).                       |
| `ShapeBlobRecord` bytes (§7.1.1)             | **Yes** (`.fory` schema, middleman) | Asset-bundle bytes survive in `data` / `content`; physics consumes them at restore.                                                                                                                            |
| `MaterialId` reference + asset bytes         | **Yes** (asset layer)              | The bytes live in `data` / `content`; the cluster only carries the ordinal.                                                                                                                                   |
| Per-archetype mirror cache (cached position / rotation / velocity) | **No**         | Rebuilt at the first post-resume entry barrier (§3.6.1) once Jolt's body table is repopulated.                                                                                                                  |
| Per-body `sleep_frames` counter (§3.7)       | **Yes (snapshot)** | Captured into `PhysicsSnapshot.body_sleep_frames` (tag 12) at drain; restored verbatim during resume.                                                                                                          |
| Auto-inertia derived `inertia_diagonal` (§3.2 invariant 5) | **Yes** (already on `RigidBody` row) | The derive happened at body-create; the result is on the surviving `RigidBody.inertia_diagonal` field. The new plugin does **not** re-derive on resume — the surviving diagonal is canonical.                |

The mechanical rule: **anything middleman-typed survives; anything
Jolt-internal (resolved `Shape*`, Jolt body / constraint / contact
pools, Jolt's `JobSystem`) does not and is rebuilt from surviving
state**.

### 8.2 What `migrate(...)` must do

The cluster authors **two** classes of migration body when its
schemas bump:

1. **`ShapeBlobRecord` `vN → vN+1`** under
   `physics/src/migrations/migrate_ShapeBlobRecord_v<N>_to_v<N+1>.cpp`,
   per §7.2.1.
2. **`PhysicsSnapshot` body / shape column `vN → vN+1`** under
   `physics/src/migrations/migrate_PhysicsSnapshot_body_columns_v<N>_to_v<N+1>.cpp`,
   per §7.2.2. (The sibling `snapshot/` cluster owns the header /
   joint columns.)

Both bodies are the standard pure migrate signature
(`reviews/decisions/hot-reload-protocol.md` §"Migrate Function
Contract"). In MVP no bump is shipped; bodies arrive when v2 lands.

The cluster also contributes the **drain + resume bodies** for the
shape table and body allocator, called by the loader at SPEC §8.3.1
(drain) and SPEC §8.3.2 step 4 (resume).

#### 8.2.1 Drain body — bodies-shapes cluster contribution

Called by the loader at SPEC §8.3.1. The cluster's slice runs as
part of the plugin-wide drain:

```text
bodies_shapes::on_drain(snapshot& s):
  // The sibling snapshot/ aggregate's PhysicsWorld::snapshot() walks
  // every body row in BodyId-ascending order; this cluster's
  // contribution is filling the nine body / shape columns (§7.1.2):
  //
  //   body_ids                <- BodyIdAllocator::in_use_ids() ascending
  //   body_motion_types       <- RigidBody.motion_type per body
  //   body_positions          <- per-archetype scratch cached_position per body
  //   body_rotations          <- per-archetype scratch cached_rotation per body
  //   body_linear_velocities  <- Velocity.v per body
  //   body_angular_velocities <- AngularVelocity.w per body
  //   body_sleep_frames       <- per-archetype scratch sleep_frames per body
  //   body_sleeping_flags     <- RigidBody.sleeping per body
  //   body_shape_blob_hashes  <- ShapeTable::content_hash_of(Collider.shape) per body
  //
  // No Jolt-side teardown happens here; the JoltSystemHandle's
  // destructor (owned by physics-world) frees Jolt's Shape table when
  // the outgoing dylib unloads. The cluster's contribution is
  // captured-bytes-into-the-snapshot only.
  return {}
```

#### 8.2.2 Resume body — bodies-shapes cluster contribution

Called by the loader at SPEC §8.3.2 step 4 (after middleman ABI
gate, world rebuild, and shape re-intern by step 3). The cluster's
slice runs in two passes:

```text
bodies_shapes::on_resume(snapshot const& s, JoltMiddleman& mm,
                         ShapeTable& shape_table,
                         BodyIdAllocator& allocator):

  // Step 4.A — Re-intern every distinct ShapeBlob hash referenced
  // from body_shape_blob_hashes. The snapshot's column is in
  // BodyId-ascending order (§7.1.4 invariant 5); we deduplicate to
  // a sorted set first so re-intern order is content_hash-ascending
  // (§6.5 determinism rule 3).
  //
  // (SPEC §8.3.2 step 3 already did this for the sibling-table call;
  // the cluster's role here is the per-body shape lookup that
  // populates Collider.shape for the surviving Collider components.)
  for hash in unique_sorted(s.body_shape_blob_hashes):
    if not shape_table.has_hash(hash):
      blob <- data.load_shape_blob_record(hash)             // asset layer
      if blob is unexpected:
        return std::unexpected{ Error::ShapeBlobMissing }
      handle <- shape_table.intern(blob, mm)
      if handle is unexpected:
        return std::unexpected{ handle.error() }

  // Step 4.B — Replay add_body in BodyId-ascending order. The
  // snapshot's body_ids column is the determinism input (§3.3
  // invariant 5); allocator.restore_from_snapshot replays the
  // ascending walk so each id is reissued to the correct entity.
  if allocator.restore_from_snapshot(s.body_rows()) is unexpected:
    return std::unexpected{ Error::BudgetExceeded }

  for i in 0 .. len(s.body_ids):
    bid     <- BodyId{ s.body_ids[i] }
    entity  <- core.entity_for_body(bid)                    // middleman ECS
    if entity is invalid:
      return std::unexpected{ Error::SnapshotBodyIdUnresolved }
    rb      <- core.read_component<RigidBody>(entity)       // surviving bytes
    col     <- core.read_component<Collider>(entity)        // surviving bytes
    shape   <- shape_table.resolve(col.shape)
    if shape is unexpected:
      return std::unexpected{ Error::ShapeHandleStale }

    // Reconstruct Jolt body from surviving record + snapshot column.
    settings <- JoltBodyCreationSettings{
      position:       s.body_positions[i],
      rotation:       s.body_rotations[i],
      motion_type:    s.body_motion_types[i],
      shape:          shape,
      collider:       col,
      mass:           rb.mass,
      inertia:        rb.inertia_diagonal,
      damping:        (rb.linear_damping, rb.angular_damping),
      ccd:            rb.ccd,                                // §3.4
    }
    if mm.create_body(bid, settings) is unexpected:
      return std::unexpected{ Error::BudgetExceeded }
    mm.set_linear_velocity(bid,  s.body_linear_velocities[i])
    mm.set_angular_velocity(bid, s.body_angular_velocities[i])
    if s.body_sleeping_flags[i]:
      mm.put_to_sleep(bid)
    // sleep_frames is per-archetype scratch; re-seeded:
    archetype_scratch[bid].sleep_frames = s.body_sleep_frames[i]

  // Step 4.C — Re-link triggers. The Collider.is_trigger bit
  // survived; the cluster reads it back during create_body above
  // and the middleman wires Jolt's overlap-only contact path. No
  // separate trigger-relink walk is required.

  return {}
```

The total work is bounded by **O(unique shape hashes) `intern`
calls + O(bodies) `add_body` calls + O(bodies) cache-row writes**,
plus a single `BodyIdAllocator::restore_from_snapshot` call. SPEC
§8.3.2 §"total work" bound is preserved.

### 8.3 Refusal cases this cluster contributes

The cluster raises these arms during drain / resume; per SPEC §8.4
they roll up under `core::Error::HotReloadRefused` with one of the
protocol's existing inner causes (`PluginAbiHashMismatch`,
`PluginInitFailed`, `SchemaMigrationFailed`).

| Detection point                                                                              | `physics::Error` arm        | `core::Error` wrapper                          | SPEC §10.1 row |
|---------------------------------------------------------------------------------------------|-----------------------------|------------------------------------------------|----------------|
| `body_shape_blob_hashes` entry not resolvable in surviving `ShapeBlobRecord` table          | `ShapeBlobMissing`          | `HotReloadRefused { PluginInitFailed }`        | SPEC §10.1     |
| Surviving `ShapeBlobRecord` decodes cleanly but its `schema_version > current_reader`, no chain | `ShapeBlobVersionUnsupported`| `HotReloadRefused { SchemaMigrationFailed }`   | SPEC §10.1     |
| Surviving `ShapeBlobRecord` fails the `content_hash` recompute (corruption)                 | `ShapeBlobMalformed`        | `HotReloadRefused { PluginInitFailed }`        | SPEC §10.1     |
| Snapshot's `body_ids` exceeds `PhysicsConfig.max_bodies` or shape count exceeds `max_shapes` | `BudgetExceeded`            | `HotReloadRefused { PluginInitFailed }`        | SPEC §10.1     |
| Snapshot row's `BodyId` does not resolve to a live ECS entity in the surviving world        | `SnapshotBodyIdUnresolved`  | `HotReloadRefused { PluginInitFailed }`        | SPEC §10.1     |
| `ShapeHandle` row at the snapshot's column index has refcount zero after restore replay     | `ShapeHandleStale`          | `HotReloadRefused { PluginInitFailed }`        | SPEC §10.1     |

The cluster does **not** raise:

- `JoltMiddlemanHashMismatch` — the `physics-world` cluster handles
  the middleman ABI gate at resume step 1
  (`physics-world-design.md` §8.3 row 1); the bodies-shapes cluster
  runs after the gate already passed.
- `SnapshotSchemaMismatch` / `SnapshotDeserialiseFailed` — owned by
  the `snapshot/` and `physics-world` clusters
  (`physics-world-design.md` §8.3 rows 2–3); the bodies-shapes
  cluster sees an already-decoded snapshot.
- `BodyMotionTypeImmutable` at restore — the snapshot column carries
  the body's motion type directly; the new plugin's `add_body` uses
  it verbatim. There is no "old type vs new type" mismatch path
  during resume.

### 8.4 Self-reload refusal

`physics.dylib` is hot-reloadable; `core` and `glibre-types.dylib`
are not (SPEC §3.3, hot-reload-protocol §"Open Questions" #2). The
bodies-shapes cluster therefore only participates as the *outgoing*
or *incoming* plugin in physics swaps; if `glibre-types` itself
ever changes (which would force a `JoltMiddleman` ABI hash change
per §4.1.13), the cluster's refusal is wrapped by the
`physics-world` cluster's `JoltMiddlemanHashMismatch` at resume
step 1 and the bodies-shapes resume body never runs.

### 8.5 Render BLAS observer

Per `physics-world-design.md` §8.5, render's `RTAccelStructures`
aggregate subscribes to `PhysicsWorldReplaced` and walks every
restored static body to invalidate / rebuild BLAS for any whose
position / rotation differ from its recorded transform. The
bodies-shapes cluster's contribution is the `body_positions` /
`body_rotations` columns in the carrier snapshot — the round-trip
identity case (post-resume positions byte-equal to pre-drain) means
zero BLAS entries are queued, which is the §11 acceptance check.

## 9. Performance

Authority: `reviews/decisions/perf-budget.md` Per-Context Budget Table
assigns physics the cell **2.00 ms CPU sim + 0.00 ms CPU submit + n/a
GPU + 128 MiB heap**. The SPEC §9.2 per-stage split gives the
**ECS↔Jolt mirror** stage 0.30 ms; this design refines the
**per-body / per-shape composition** of that stage so the §11
`BENCHMARK_CELL` `physics/bodies: ecs_jolt_mirror_two_substep` (SPEC
§9.6.1) has a well-defined target.

This design does **not widen** any cell or sub-budget; deviations
require a `perf-budget.md` amendment spike, not an in-place edit.

### 9.1 Cited cells (verbatim from `perf-budget.md` and SPEC §9)

| Axis                  | Budget          | Source                                                    |
|-----------------------|-----------------|-----------------------------------------------------------|
| CPU sim (cluster)     | **0.30 ms**     | SPEC §9.2 — ECS↔Jolt mirror stage row                     |
| CPU submit            | 0.00 ms         | physics records no GPU work                                |
| GPU                   | n/a             | physics owns no Metal heaps or encoders (SPEC §3.3)        |
| Heap (cluster slice)  | **~48 MiB + 32 MiB = 80 MiB** | SPEC §9.3 — Jolt body half (~48 of 64 MiB) + ShapeBlob hash table 32 MiB |
| Phase ownership       | (within phase 3) | the `physics-world` cluster owns phase 3; this cluster contributes the per-body / per-shape walks inside |

### 9.2 Cluster sub-budget within the §9.2 ECS↔Jolt mirror row

The §9.2 mirror row decomposes into two barriers per substep × two
substeps × ~30 active bodies (S1 fixture). The cluster's
contribution per primitive is bounded as follows; the numbers fit
**inside** the existing 0.30 ms row, not as a fourth row.

| Cluster slice                                        | Per substep | Per frame (2 substeps × 2 barriers) | Dominant operation                                                                                       |
|------------------------------------------------------|-------------|--------------------------------------|-----------------------------------------------------------------------------------------------------------|
| `BodyId`-ascending sort over scratch span            | < 0.005 ms  | < 0.020 ms                           | `eastl::sort` over ~30 `u32`; `O(n log n)` with `n ≤ 30`.                                                |
| Entry barrier — `ExternalForce` / `Torque` drain     | < 0.010 ms  | < 0.020 ms                           | `Vec3` add into Jolt's body-force accumulator + zeroing; one cache line per body.                       |
| Entry barrier — Kinematic position / rotation set    | < 0.005 ms  | < 0.010 ms                           | One `Mat4` worth of writes per Kinematic body; few in S1 (~1 character).                                  |
| Exit barrier — velocity / position / sleep mirror    | < 0.020 ms  | < 0.040 ms                           | `Vec3 × 2` reads + `Vec3 × 2` writes per body + sleep-bit branch.                                          |
| `Sleeping` marker add / remove                       | < 0.005 ms  | < 0.010 ms                           | One `add_component` / `remove_component` call per wake-state change; rare in S1 steady-state.            |
| `sleep_frames` counter advance                       | < 0.001 ms  | < 0.005 ms                           | One `u16++` per sleeping body.                                                                            |
| **Cluster sub-total per frame**                      |             | **< 0.105 ms**                       | Steady-state two-substep frame, S1 fixture. Comfortably fits inside the 0.30 ms mirror row.              |

The remaining ~0.20 ms inside the 0.30 ms row covers the joints
mirror walk (sibling), the contact-event drain (sibling), and the
trigger-event drain (sibling); the bodies-shapes contribution is
the rate-limiting term but not the dominant one.

### 9.3 Heap composition inside the 128 MiB ceiling

The cluster's resident allocations under `ContextTag::physics`
contribute to the SPEC §9.3 pool decomposition. Cluster's slice:

| Pool (SPEC §9.3 row)                | Cluster's contribution                                                     |
|-------------------------------------|----------------------------------------------------------------------------|
| Jolt body + constraint pools (64 MiB) | The body half: Jolt's `BodyManager` + `BodyCreationSettings` cache + per-body activation tracking. Sized for `PhysicsConfig::max_bodies = 1024`; ~48 MiB at the upper bound. The constraint half is the joints sibling. |
| `ShapeBlob` hash table (32 MiB)      | `ShapeTable.rows_` (struct array indexed by `ShapeHandle.raw() - 1`) + `by_hash_` hash-map + the resolved Jolt `Shape*` payloads (Jolt's `SphereShape`, `BoxShape`, `MeshShape`, etc.). Sized for ~256 unique shapes × ~128 KiB average payload (heightfields + meshes dominating). |
| Snapshot scratch arena (16 MiB)      | None — owned by sibling `snapshot/`.                                         |
| Query result buffers (8 MiB)         | None — owned by sibling `queries/`.                                          |
| Contact event ring (8 MiB)           | None — owned by sibling `contact/`.                                          |

The cluster's per-archetype mirror cache (§3.2) lives on the
**phase-3 transient arena** (4 MiB ceiling, exempt from the 128 MiB
cell per `perf-budget.md` Allocator Rule #4). The arena drains by
phase 9; cluster cache rows are reborn at every phase 3 entry barrier.

### 9.4 Allocator rules

Per SPEC §9.4 every allocation under `physics/src/bodies/**` and
`physics/src/shapes/**` is stamped with `ContextTag::physics` at
the allocator-handle level (`perf-budget.md` Allocator Rule #1).
The cluster makes only four distinct allocations:

1. **`BodyIdAllocator` itself** — one `eastl::make_unique` at
   `PhysicsWorld::create` (broker through `physics-world` cluster).
   ~24 B + the `id_to_entity_` vector's bytes (~4 MiB at
   `max_bodies = 1024`).
2. **`ShapeTable` itself** — one `eastl::make_unique` at the same
   broker. ~32 B + the `rows_` vector's bytes (~32 MiB at
   `max_shapes = 256` with average row size).
3. **Per-`intern_shape` Jolt `Shape*`** — one allocation per unique
   `content_hash` that crosses the table. Tagged inside the
   middleman per SPEC §9.4 rule 5 ("Jolt's allocator is wrapped");
   counts against the 64 MiB Jolt-pool sub-share.
4. **Per-archetype mirror cache row** — one transient-arena
   allocation per body at phase 3 entry; drains at phase 9. Counts
   against the 4 MiB transient arena, not the 128 MiB cell.

Under `GLIBRE_ALLOC_STRICT=1`, an allocation that would push live
`ContextTag::physics` bytes above 128 MiB returns
`Result<>` with `core::Error::OutOfBudget` (`perf-budget.md`
Allocator Rule #2). The cluster's `intern_shape` and `add_body`
propagate via the monadic chain; failure to handle aborts with the
diagnostic dump.

### 9.5 GPU and submit halves

The cluster consumes **no** GPU budget and **no** CPU submit
budget. Phase 3 finishes before phase 6's `RenderFrame` extract
begins; the cluster writes no GPU resources. Render reads body
transforms from the post-phase-5 `GlobalTransform` ECS column
(§3.10 seam), not from a cluster-side encoder. The `content_hash`
read for BLAS keying (§3.10 seam #2) is in phase 6 but is a
cache-cold `u64` load, well below the precision floor.

### 9.6 CI gate hooks the cluster owns

Per SPEC §9.6, the per-context CI gate requires per-row benchmarks.
The cluster contributes (one Catch2 case per row; PR fails on any
breach):

| Stage                                                           | `BENCHMARK_CELL` test name                                          | CPU ceiling | Source                                       |
|-----------------------------------------------------------------|----------------------------------------------------------------------|-------------|----------------------------------------------|
| Cluster-side mirror walk (composite — already in SPEC §9.6.1)   | `physics/bodies: ecs_jolt_mirror_two_substep`                       | 0.30 ms     | SPEC §9.6.1 mandated row                      |
| `BodyIdAllocator::allocate` + `release` round-trip               | `physics/bodies: body_id_allocator_alloc_release_round_trip`        | 0.001 ms    | §3.3 free-list policy                        |
| `ShapeTable::intern` cache-hit (existing hash)                  | `physics/shapes: shape_table_intern_cache_hit`                      | 0.005 ms    | §3.9 invariant 1                             |
| `ShapeTable::intern` cold (new hash, decode dispatcher)         | `physics/shapes: shape_table_intern_cold_box_kind_3`                | 0.020 ms    | §3.8.2 + §3.9                                |
| Per-archetype mirror cache rebuild at phase-3 entry              | `physics/bodies: archetype_scratch_rebuild_60_bodies`                | 0.010 ms    | §3.2 cache fields + §3.6.1                   |

These rows live under `tests/physics/bodies/` and
`tests/physics/shapes/`. The composite row (`physics/bodies:
ecs_jolt_mirror_two_substep`) is the SPEC §9.6.1 mandated row; the
four cluster-specific rows are added so a regression that puts 0.05
ms of overhead into one path without changing the total is caught
at the cluster ceiling.

The §9.6.2 heap-residency assertions the cluster owns:

| Pool                       | Heap-residency test name                              | Ceiling   |
|----------------------------|--------------------------------------------------------|-----------|
| Jolt body pool half        | `physics/bodies: heap_jolt_body_pool`                 | 48 MiB    |
| `ShapeBlob` hash table     | `physics/shapes: heap_shape_table` (SPEC §9.6.2)      | 32 MiB    |

The `physics/shapes: heap_shape_table` row is SPEC-mandated; the
`physics/bodies: heap_jolt_body_pool` row is added so the cluster's
share of the 64 MiB Jolt-pool ceiling is independently tracked
(joints + contact pools share the other 16 MiB).

## 10. Failure modes

The cluster raises a closed subset of the `physics::Error` enum
(SPEC §5; SPEC §10.1 documents each arm authoritatively). Each arm
below names the trigger condition specific to **`RigidBody` /
`BodyId` / `Collider` / `ShapeHandle` / `ShapeBlob` / `Sleeping` /
`CCD`**, the recovery posture, the log severity, and the SPEC §10.1
row that owns the arm. This design adds the per-primitive trigger
detail the implementer needs.

The arms are split by cluster primitive (§3 references). All arms
are documented authoritatively in SPEC §10.1; this section refines
the per-primitive triggers and routes.

### 10.1 `RigidBody` / `BodyId` lifecycle arms (§3.2, §3.3)

| Arm                              | Trigger (in this cluster)                                                                              | Recovery        | Severity (default) | SPEC ref      |
|----------------------------------|---------------------------------------------------------------------------------------------------------|-----------------|--------------------|---------------|
| `BodyNotFound`                   | `BodyId` resolves outside its world: stale handle (body despawned mid-frame and the call lands in phase 8), cross-world `BodyId`, zero-init `BodyId{0}` reaching a body op (§3.3 invariant 1). | Caller treats as despawn signal | `info` | SPEC §10.1 |
| `BodyMotionTypeImmutable`        | `RigidBody.motion_type` mutated post-create (§3.2 invariant 2). Detected by the entry barrier's per-row consistency check.                | Caller despawns + re-adds | `warn` | SPEC §10.1 |
| `BodyStillReferencedByJoint`     | `remove_body` called while a live `Joint` references the body as endpoint A or B (§3.5 + §4.1.7 inv 4). Detected before any cluster-side state mutation. | Caller removes joints first  | `warn` | SPEC §10.1 |
| `BudgetExceeded`                 | `add_body` past `PhysicsConfig::max_bodies` (§3.3 invariant 4). Surfaces from `BodyIdAllocator::allocate`. Also surfaces during hot-reload restore if the snapshot's body count exceeds the new world's `max_bodies` (§8.3).                         | Caller raises budget on fresh world | `error` | SPEC §10.1 |

### 10.2 `Collider` / `ShapeHandle` / `ShapeBlob` arms (§3.5, §3.8, §3.9)

| Arm                              | Trigger (in this cluster)                                                                              | Recovery        | Severity (default) | SPEC ref      |
|----------------------------------|---------------------------------------------------------------------------------------------------------|-----------------|--------------------|---------------|
| `ColliderShapeRequired`          | `add_body` called with `Collider.shape == ShapeHandle{}` (default-init handle) or a refcount-zero handle. Detected at `add_body` before any Jolt body allocation (§3.5).                | Caller calls `intern_shape` first | `error` | SPEC §10.1 |
| `ShapeBlobMalformed`             | `intern_shape(blob)` failed `content_hash` recompute (§7.1.2 invariant 1), unknown `kind` ordinal (§3.8.8), or per-kind validation rejected the bytes (negative radius, non-finite vertex, mesh index out of range, etc.). | Caller treats as missing-asset; refused. | `error` | SPEC §10.1 |
| `ShapeBlobVersionUnsupported`    | `intern_shape(blob)` decoded cleanly but the `ShapeBlobRecord.schema_version > current_reader` and no migration chain registered (§7.2.1).                            | Operator authors the missing migration body or re-cooks the asset | `error` | SPEC §10.1 |
| `ShapeHandleStale`               | A `ShapeHandle` whose row's refcount is zero is reused (§3.9 invariant 4). Detected at `ShapeTable::resolve` and at `add_body` time.                | Caller re-interns from the underlying blob | `warn` | SPEC §10.1 |
| `ShapeBlobMissing`               | Hot-reload restore: a `body_shape_blob_hashes` entry has no matching `ShapeBlobRecord` in the surviving asset bundle (§8.3 row 4). | Operator restores the dropped record / re-cooks / accepts fresh world | `warn` | SPEC §10.1 |
| `BudgetExceeded`                 | `intern_shape` past `PhysicsConfig::max_shapes` (§3.9 invariant 5).                            | Caller raises budget on fresh world | `error` | SPEC §10.1 |

### 10.3 Substep-mirror arms (§3.6)

| Arm                              | Trigger (in this cluster)                                                                              | Recovery        | Severity (default) | SPEC ref      |
|----------------------------------|---------------------------------------------------------------------------------------------------------|-----------------|--------------------|---------------|
| `SubstepEcsCommitInverted`       | A debug-build assertion in the entry / exit barrier walk detected ECS↔Jolt cross-traffic between the two barriers. The `physics-world` driver's flag triple is what the assertion reads (`physics-world-design.md` §5.1); the typed arm fires when promoted under `Hard`. | Programming error in a §3.6 mirror seam refactor; abort substep | `error` | SPEC §10.1 |

### 10.4 Snapshot / restore arms (§7.1.2, §8)

| Arm                              | Trigger (in this cluster)                                                                              | Recovery        | Severity (default) | SPEC ref      |
|----------------------------------|---------------------------------------------------------------------------------------------------------|-----------------|--------------------|---------------|
| `SnapshotBodyIdUnresolved`       | Snapshot's `body_ids[i]` does not resolve to a live ECS entity in the surviving world. Detected during §8.2.2 step 4.B. | Operator captures snapshot from a clean world or accepts fresh world | `error` | SPEC §10.1 |

### 10.5 Routed arms (cluster does not detect)

The cluster forwards these arms from sibling aggregates. Listed for
completeness:

- `JointEndpointInvalid`, `JointDanglingEndpoint`, `JointKindUnsupported`,
  `JointBroken` — sibling `joints/` (the cluster sees
  `BodyStillReferencedByJoint` in the symmetric remove-body path).
- `QueryDuringStep`, `QueryFilterInvalid` — sibling `queries/`.
- `ConfigInvalid`, `WorldNotInitialised`, `WorldAlreadyInitialised`,
  `JoltMiddlemanHashMismatch`, `JoltMiddlemanUnavailable`,
  `SnapshotSchemaMismatch`, `SnapshotDeserialiseFailed`,
  `HotReloadStateUnmigratable`, `StepCalledOutsidePhase3`,
  `AccumulatorClampExceeded`, `NumericalInstabilityDetected`,
  `DeterminismCheckFailed` — sibling `physics-world` cluster
  (`physics-world-design.md` §10).

These cross the §5 facade as forwarding errors; the cluster does
not author the detection.

### 10.6 Caller-side recovery posture

Per SPEC §10.2 / §10.4, callers handle each arm at exactly one
boundary. For cluster arms specifically:

- **Gameplay / scripting plugins** — `add_body`, `remove_body`,
  `intern_shape`, `release_shape` errors propagate up to the
  schedule's per-system error wrapper, which logs once at the
  arm's severity and the calling system continues. A failed
  `add_body` does not abort the frame; the entity is left without
  a body.
- **Editor tools** — same propagation, plus the editor's
  inspector surfaces the failure as a content-pipeline drift
  marker on the affected entity.
- **Hot-reload loader** — the six cluster-contributed refusal
  arms (§8.3) wrap into `core::Error::HotReloadRefused` with the
  matching inner cause. Refusal logs at `warn` per the protocol;
  the previous-good plugin keeps stepping.

### 10.7 Determinism obligation

Every non-hot-reload arm above must fire **byte-equal across runs**
on byte-equal inputs (PHILOSOPHY §7). Specifically: the
`BudgetExceeded` threshold compares against `PhysicsConfig::max_*`
which is bit-exact across hosts (SPEC §6.4 R4); the
`ShapeBlobMalformed` `content_hash` recompute is BLAKE3 which is
portable; `BodyMotionTypeImmutable` detection is a `u8` compare on
the surviving `RigidBody` row. The §11 acceptance test
`physics/bodies: error_arms_byte_equal_across_runs` asserts the
error stream from a fixed input trace is byte-equal across two
consecutive runs.

## 11. Test plan

Unit + integration + perf tests required to validate the §1–§10
invariants. Catch2 test names are stable; the SPEC §11 `[STORY]`
parent issue is named in the right column where one exists.

### 11.1 Unit tests (one Catch2 case per row)

Lives under `tests/physics/bodies/` and `tests/physics/shapes/`.

| Test name                                                                | What it asserts                                                                                                                            | §-ref          | Story |
|--------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------|----------------|-------|
| `physics/bodies: add_body_allocates_body_id`                             | `add_body(entity, RigidBody, Collider)` returns a `BodyId` whose `valid()` is true and whose `raw()` is the next id from the allocator.   | §3.3, §4.1.5 | #429  |
| `physics/bodies: add_body_refuses_collider_without_shape`                | `add_body` with `Collider.shape == ShapeHandle{}` → `ColliderShapeRequired`.                                                              | §3.5, §10.1   | #429  |
| `physics/bodies: add_body_simulates_at_next_substep`                     | After `add_body`, the next phase-3 entry barrier reads the body's `ExternalForce`, the next exit barrier writes `Velocity`.               | §3.6, §4.1.5b | #429  |
| `physics/bodies: remove_body_releases_body_id`                           | `remove_body` then `add_body` reissues the same `BodyId` (free-list reuse, sort-on-pop).                                                  | §3.3, §3.3.2 | #431  |
| `physics/bodies: remove_body_refuses_with_live_joint`                    | `remove_body` with a live joint endpoint → `BodyStillReferencedByJoint`; body remains in Jolt.                                            | §3.5, §10.1, §4.1.7 inv 1 | #431 |
| `physics/bodies: motion_type_is_immutable`                               | Mutating `RigidBody.motion_type` post-create → `BodyMotionTypeImmutable` at next substep entry; type unchanged.                            | §3.2, §10.1   | (no story; CI invariant) |
| `physics/bodies: static_body_carries_zero_velocity`                      | Reading `Velocity` / `AngularVelocity` on a `MotionType::Static` body returns zero across ten substeps even when `ExternalForce` is set. | §3.2 invariant 3 | (CI) |
| `physics/bodies: kinematic_body_position_authored_by_ecs`                | Kinematic body whose ECS-side script writes `GlobalTransform` per frame → entry barrier forwards it to Jolt; exit-barrier-written `scratch[body_id].cached_position` matches at every substep. | §3.2 invariant 4, §3.6.1 | (CI) |
| `physics/bodies: auto_inertia_derives_at_create`                         | `RigidBody.auto_inertia = true` + `Collider` referencing a `Sphere` → `inertia_diagonal` post-create equals `(2/5) m r²` per axis.       | §3.2 invariant 5 | (CI) |
| `physics/bodies: body_id_allocation_order_is_ecs_materialisation_order`  | Spawn entities A, B, C in that order; assert `BodyId(A).raw() < BodyId(B).raw() < BodyId(C).raw()` regardless of host.                    | §3.3 invariant 2 | #434 |
| `physics/bodies: body_id_free_list_pop_is_ascending`                     | Spawn IDs 1..10; release 5 then 3 then 7; next allocate returns 3 (smallest in free list).                                                | §3.3.2        | #434  |
| `physics/bodies: body_id_zero_is_invalid_sentinel`                       | `BodyId{}.valid() == false`; `BodyId{0}.raw() == 0`; allocator never returns id 0.                                                        | §3.3 invariant 1 | (CI) |
| `physics/bodies: body_id_budget_exceeded`                                | `max_bodies = 4`; `add_body` × 5 → fifth call returns `BudgetExceeded`.                                                                    | §3.3 invariant 4, §10.1 | (CI) |
| `physics/bodies: ccd_bit_forwards_to_jolt_motion_quality`                | `RigidBody.ccd = true` on a Dynamic body → middleman's `set_motion_quality` is called with `LinearCast`; bit ignored on Static.            | §3.4          | (CI)  |
| `physics/bodies: sleeping_marker_mirrors_jolt_is_active`                 | After ten frames at rest under `sleep_*` thresholds, the entity has a `Sleeping` marker; applying force removes it within one frame.       | §3.7, §4.1.6 (R-4.1.6) | (CI) |
| `physics/bodies: sleep_frames_resets_on_wake`                            | Sleep for 100 frames (counter = 100); wake by `ExternalForce`; counter resets to 0; sleep again, counter restarts.                        | §3.7          | (CI)  |
| `physics/shapes: intern_shape_returns_handle_for_sphere`                 | `intern_shape(Sphere{radius=1})` returns a valid `ShapeHandle`; `resolve` returns a non-null Jolt `Shape*`.                              | §3.8.1, §3.9   | #451  |
| `physics/shapes: intern_shape_dedupes_by_content_hash`                   | Two `intern_shape` calls with byte-equal `Sphere` blobs return the same `ShapeHandle`; refcount = 2.                                       | §3.9 invariant 1, §4.1.6 inv 1 | #451 |
| `physics/shapes: release_shape_decrements_refcount`                      | `intern_shape × 2` then `release_shape × 1` keeps the row alive; second release drops the row.                                            | §3.9 invariant 2 | #451 |
| `physics/shapes: intern_shape_refuses_malformed_content_hash`            | `intern_shape` with corrupted content_hash byte → `ShapeBlobMalformed`.                                                                   | §3.8.8, §10.1 | (CI)  |
| `physics/shapes: intern_shape_refuses_unknown_kind_ordinal`              | `intern_shape` with `kind = 99` → `ShapeBlobMalformed`.                                                                                     | §3.8.8, §10.1 | (CI)  |
| `physics/shapes: intern_shape_refuses_dynamic_triangle_mesh`             | `add_body(Dynamic, Collider{TriangleMesh})` → `ConfigInvalid` (per §3.8.5 restriction).                                                    | §3.8.5        | (CI)  |
| `physics/shapes: intern_shape_compound_resolves_children_at_load`        | `Compound` blob whose child hash is in the table → resolves; whose child hash is missing → `ShapeBlobMissing`.                              | §3.8.7, §7.1.2 inv 4 | (CI) |
| `physics/shapes: intern_shape_refuses_recursive_compound`                | `Compound` whose child is itself a `Compound` → `ShapeBlobMalformed` at decode (§3.8.7 restriction).                                       | §3.8.7        | (CI)  |
| `physics/shapes: shape_handle_zero_is_invalid_sentinel`                  | `ShapeHandle{}.valid() == false`; resolve returns `ShapeHandleStale`.                                                                       | §3.9          | (CI)  |
| `physics/shapes: shape_handle_stale_after_release_zero`                  | `intern × 1` then `release × 1` then `resolve` → `ShapeHandleStale`.                                                                        | §3.9 invariant 4, §10.1 | (CI) |
| `physics/shapes: shape_table_budget_exceeded`                            | `max_shapes = 4`; `intern_shape × 5` distinct hashes → fifth call returns `BudgetExceeded`.                                                | §3.9 invariant 5, §10.1 | (CI) |
| `physics/bodies: collider_layer_and_material_are_mutable`                | Mutate `Collider.layer` / `Collider.material` on a live entity → next entry barrier writes the new values into Jolt; pre-mutation values discarded. | §3.5 invariant 3 | (CI) |
| `physics/bodies: collider_is_trigger_immutable_per_substep`              | Mutating `Collider.is_trigger` mid-frame → debug-build assertion fires; `Trigger` events for the pair only change after a despawn + re-add. | §3.5 invariant 2 | (CI) |
| `physics/bodies: error_arms_byte_equal_across_runs`                      | The error stream from a fixed input trace is byte-equal across two consecutive runs (PHILOSOPHY §7).                                       | §10.7         | (CI)  |

### 11.2 Integration tests

Lives under `tests/physics/bodies/integration/` and
`tests/physics/shapes/integration/`. Drives the cluster through
multi-frame sequences via the `core` `FrameLoop` test harness.

| Test name                                                                | Scenario                                                                                                                                | §-ref         | Story  |
|--------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------|---------------|--------|
| `physics/bodies: body_id_stable_across_macos_arm64_vs_x64`               | Spawn 50 entities in deterministic ECS order on M1-arm64 and macOS-x64; assert `BodyId` for entity-i matches across hosts.              | §3.3, §6.5    | #434   |
| `physics/bodies: body_id_stable_across_hot_reload_round_trip`            | Tower fixture (50 boxes); reload at frame 8; assert post-reload `BodyId` for entity-i equals pre-reload.                                | §3.3, §8      | #434, #449 |
| `physics/shapes: shape_blob_dedup_under_concurrent_intern`               | Spawn 200 props all referencing the same `Sphere` content hash; assert `ShapeTable.high_water_rows == 1`.                                 | §3.9, §4.1.6 inv 1 | #451 |
| `physics/shapes: shape_blob_compound_dedup_across_parents`               | Two compound blobs whose child sets overlap; assert overlapping children share rows; refcounts sum correctly.                            | §3.8.7, §3.9  | #451   |
| `physics/bodies: hot_reload_round_trip_byte_equal_for_50_bodies`         | 50 bodies with mixed motion types; reload at frame 8; assert post-resume body positions / velocities / sleep state byte-equal pre-drain. | §8.2, SPEC §8.6 | #449  |
| `physics/bodies: hot_reload_refuses_missing_shape_blob`                  | Reload with one `ShapeBlobRecord` removed from the asset bundle; `HotReloadRefused { PluginInitFailed { ShapeBlobMissing } }`.            | §8.3, SPEC §8.6 | #449  |
| `physics/bodies: hot_reload_refuses_corrupt_shape_blob`                  | Reload with one `ShapeBlobRecord` whose content_hash is corrupted; `HotReloadRefused { PluginInitFailed { ShapeBlobMalformed } }`.        | §8.3          | #449   |
| `physics/bodies: ecs_jolt_mirror_two_substep`                            | The §9.6 `BENCHMARK_CELL` perf assert (0.30 ms ceiling).                                                                                | §9.6          | (CI)   |
| `physics/bodies: body_id_allocator_alloc_release_round_trip`             | The cluster `BENCHMARK_CELL` perf assert (0.001 ms ceiling).                                                                            | §9.6          | (CI)   |
| `physics/shapes: shape_table_intern_cache_hit`                           | The cluster `BENCHMARK_CELL` perf assert (0.005 ms ceiling).                                                                            | §9.6          | (CI)   |
| `physics/shapes: shape_table_intern_cold_box_kind_3`                     | The cluster `BENCHMARK_CELL` perf assert (0.020 ms ceiling).                                                                            | §9.6          | (CI)   |
| `physics/bodies: archetype_scratch_rebuild_60_bodies`                    | The cluster `BENCHMARK_CELL` perf assert (0.010 ms ceiling).                                                                            | §9.6          | (CI)   |

### 11.3 Property-based tests

Lives under `tests/physics/bodies/property/` and
`tests/physics/shapes/property/`. Use Catch2 `GENERATE` for
fuzz-style coverage.

| Test name                                                                | Property                                                                                                                            | §-ref |
|--------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------|-------|
| `physics/bodies: body_id_allocation_pure_function_of_call_sequence`      | For random allocate/release sequences, replaying the sequence twice yields byte-equal id outputs.                                   | §3.3  |
| `physics/bodies: body_id_free_list_invariant_under_release_order`        | Releasing the same set of ids in any order yields the same allocator state after sort-on-pop.                                        | §3.3.2 |
| `physics/shapes: shape_table_intern_idempotent_under_duplicate_hashes`   | Random sequence of `intern(blob_i)` calls with `i` from a small set; assert refcount per row equals the count of inserts for that hash. | §3.9 |
| `physics/shapes: shape_blob_content_hash_stable_under_field_zero_fill`    | Two `ShapeBlobRecord`s differing only in zero-filled unused fields produce the same `content_hash`.                                  | §7.1.2 inv 2, §3.8 |
| `physics/bodies: snapshot_round_trip_per_body_columns`                    | For random body sets, capture → restore round-trip preserves every column byte-equal.                                                | §3.6, §7.1.2 |

### 11.4 What this design does **not** test

- **Sibling-aggregate seams** — joint, contact, query, snapshot,
  middleman, accumulator. Each has its own design spike under
  #791; this cluster only tests its own facade brokering.
- **Multi-thread access** — single-thread sim in MVP (SPEC §6.6).
  When per-system parallelism lands, ThreadSanitizer + the
  access-set DAG cover the new surface.
- **Multi-body-per-entity** — refused in MVP (§3.5 invariant 1).
  Compound shapes provide the multi-collider use case via a single
  `Collider` entry.
- **Runtime baking** — refused (§2.3 row R-4.2.12). No tests.
- **Soft-body / cloth / fluid shape kinds** — refused (§2.3). No
  tests.
- **GPU / Metal interaction** — refused (SPEC §1, §3.3). No tests.

### 11.5 Acceptance-criteria mapping

The `[STORY]` issues already named in SPEC §11 that this design's
tests close (one Catch2 case per story):

- #429 `physics: insert RigidBody allocates BodyId, body simulates next substep`
- #431 `physics: remove RigidBody destroys Jolt body, refuses if joint references it`
- #434 `physics: BodyId stable across hosts and across hot-reload`
- #449 `physics: hot-reload preserves world state across snapshot at phase 8` (cluster slice — body / shape replay)
- #451 `physics: ShapeBlob deduplicated by content hash, refcounted ShapeHandle`

The remaining stories from SPEC §11 (#421, #423, #425, #427, #435,
#437, #439, #441, #443, #445, #448) belong to sibling aggregates and
are out of scope for this design.

## 12. Open questions

Each `[OPEN]` is a follow-up amendment trigger; resolution amends
the matching SPEC section in place, not this design. Per the
PHILOSOPHY §3 + workflow rule, no `[OPEN]` is discharged silently.

- **[OPEN]** Should `Collider` carry per-sub-shape metadata
  (per-child layer / material / friction-direction overrides on
  `Compound` shapes) for vehicle wheels and ragdoll limbs? MVP
  ships one layer + one material per `Collider`; per-sub-shape
  metadata reactivates when the post-MVP `vehicle` or `character`
  plugins demonstrate the load-bearing case. Tracked by §3.5
  invariant 1 + §3.8.7 row "Compound".
- **[OPEN]** Should `ConvexHull`'s `max_convex_radius` (Jolt's
  hull-shrink knob) be a per-blob field instead of the
  cluster-pinned 0.05 m? Per-blob would let high-precision physics
  (fast-rolling marbles) ship with a smaller radius and
  low-precision physics (rubble) ship with a larger one. The
  trade-off is one new field on `ShapeBlobRecord` (a §7.2.2
  schema bump) for sub-millimetre-tolerance scenes that may not
  appear in MVP. Tracked by §3.8.4.
- **[OPEN]** Should the free-list policy upgrade to a min-heap
  (§3.3.2 alt C) when active body counts exceed ~512? The
  sort-on-pop cost crosses the §9.2 mirror-row precision floor
  somewhere around `n = 512`; revisit when an MVP fixture
  demonstrates active-body counts above S1's. Tracked by §3.3.2.
- **[OPEN]** Should `BodyIdAllocator` reseed `next_id_` from
  `max(in_use ids) + 1` at hot-reload restore, or re-derive it by
  re-running the full allocate/release replay? Current text says
  "re-run the replay" (§8.2.2 step 4.B); the alternative would skip
  the replay but must prove equivalence with the original
  sort-on-pop policy. Tracked by §3.3 invariant 5 + §8.2.2.
- **[OPEN]** Should the per-archetype mirror cache (§3.2 cache
  fields) be moved off the cluster's transient arena onto the ECS
  archetype storage as a "hot prefix" of the `RigidBody` row? The
  benefit is one fewer cache-miss per substep barrier; the cost is
  a SPEC §5 layout change (`RigidBody`'s on-the-wire shape grows).
  Frozen at the transient-arena shape until the §11 `BENCHMARK_CELL`
  measures the L1-D miss rate as load-bearing. Tracked by §3.2
  invariant 6 + §5.1.
- **[OPEN]** Should the `Trigger` marker carry a `TriggerKind`
  ordinal (Static / Persistent / OneShot) authored on the ECS
  component, or remain a zero-byte tag whose lifetime semantics
  ride entirely on the layer-pair interaction matrix? Harmonius
  R-4.2.8 names "one-shot, persistent, and filtered modes"; MVP
  collapses these onto the layer matrix. Re-evaluate if a gameplay
  story demonstrates a zero-shot trigger that the layer matrix
  cannot express. Tracked by §3.5 + sibling `contact/`.
- **[OPEN]** Should `ShapeBlobRecord` grow an `optional` per-cell
  material array on `Heightfield` (§3.8.6), parallel to the
  per-triangle materials on `TriangleMesh`? The MVP heightfield
  uses one material from the `Collider`; voxel-terrain (R-4.2.10
  refused) would benefit from per-cell. Tracked by §3.8.6 +
  §7.2.1.

- **[OPEN — SPEC AMENDMENT REQUIRED]** `BodyStillReferencedByJoint`
  (SPEC §5 body/collider section, this design §10.1) and
  `JointDanglingEndpoint` (SPEC §4.1.7 invariant 1 prose, SPEC §5 joints
  section) both describe the "body removed while a joint references it"
  scenario. The current design uses `BodyStillReferencedByJoint`
  consistently (bodies-cluster perspective: the body is the refused
  actor). `JointDanglingEndpoint` is listed in §10.5 as a routed
  sibling arm (joints-cluster detection path). The SPEC §4.1.7
  invariant 1 prose must be amended to name `BodyStillReferencedByJoint`
  at the `remove_body` call site, with a note that
  `JointDanglingEndpoint` is reserved for the symmetric stale-`BodyId`-
  on-a-live-joint check (joints aggregate, not bodies aggregate). Tracked
  by `[SPIKE] iterate-physics-error-arm-joint-body-reconciliation` (see
  §12 spike reference). This is a blocker for the sibling `joints/`
  design spike to produce a consistent §10 table.

Resolution of any `[OPEN]` lands the decision into
`reviews/decisions/` (when cross-aggregate) or amends SPEC §3 / §4 /
§9 in place (when local to this cluster); per the workflow no
`[OPEN]` is discharged silently.
