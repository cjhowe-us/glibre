# Physics Spec

## 1. Purpose

The `physics` context owns one responsibility: producing a deterministic,
bit-identical rigid-body simulation step inside the engine's frame loop.
Concretely, that is owning **frame phase 3** (`physics-fixed`) end to
end — driving a fixed-timestep accumulator over a Jolt-backed
`PhysicsWorld`, performing broadphase, narrowphase, constraint solve
and contact resolution for the substeps owed this frame, and exposing
the resulting body state plus contact and trigger events back to ECS
as plain components. It owns the ECS-facing translation layer that
mirrors `RigidBody`, `Collider`, joint and material components into
Jolt body/shape/constraint handles, the deterministic configuration
that makes that mirror reproducible across hosts and runs (substep
count, solver iterations, warm-start factor, sleep thresholds, fixed
seed, fixed iteration order, stable body-id allocation), the spatial
query surface (`RayCast`, `ShapeCast`, `Overlap`, closest-point) that
gameplay and other plugins call against the same Jolt broadphase, and
the on-disk physics-tuning schemas (`PhysicsConfig`, `PhysicsMaterial`)
the `data` context serialises. Physics refuses to own anything outside
that seam. It does **not** own animation or skeletal pose evaluation
(deferred — phase 4 is reserved for the future `animation` plugin),
navigation or pathfinding (a separate context when it lands),
collision-shape authoring UX or V-HACD bake pipelines (those belong to
`tools` and `content`/`geometry` at cook time; physics consumes baked
shape blobs only), GPU compute work of any kind (no fluids, no GPU
broadphase, no GPU cloth — soft-body, cloth, fluid and destruction
contexts are all deferred and route GPU paths through `render` /
`vfx`), the frame schedule and ECS runtime themselves (those are
`core`), the asset bytes and Fory codegen pipeline (`data`), gameplay
intents and visual scripting (the deferred phase-2 `logic` plugin), or
window/input/swapchain (`platform`). Per SRP, every reason physics has
to change must trace back to phase-3 stepping, the ECS↔Jolt mirror,
deterministic config, the query API, or the tuning schemas; anything
else routes to the owning context.

## 2. Ubiquitous Language

Terms used unchanged in code (identifiers, file names, comments).

| Term | Meaning |
|------|---------|
| `PhysicsWorld` | One Jolt `PhysicsSystem` instance owned by the plugin: bodies, broadphase, constraint set, contact listener, query interface. One per ECS `World`. |
| `PhysicsConfig` | ECS resource holding the deterministic config: gravity, fixed `dt`, `substep_count`, solver `velocity_iters` / `position_iters`, `warm_start_factor`, sleep thresholds, broadphase layer matrix, RNG seed. Init-time-immutable for a given run. |
| `Accumulator` | Per-`World` time accumulator advanced in phase 3 by the `core` frame `dt`; consumes whole substeps until the remainder is below `dt`. Carry preserved across frames. |
| `Substep` | One fixed `dt` Jolt step inside phase 3: integrate → broadphase → narrowphase → solve → contacts. The unit of determinism. |
| `RigidBody` | ECS component declaring participation in simulation: motion type (`Static`/`Kinematic`/`Dynamic`), mass, inertia, linear+angular damping, sleeping flag. Mirrors a Jolt `BodyID`. |
| `Velocity` / `AngularVelocity` | ECS components carrying linear and angular velocity respectively; written by the solver each substep, read by gameplay. |
| `ExternalForce` / `ExternalTorque` | Per-frame accumulator components plugins write into; physics drains them at substep entry and zeros them at substep exit. |
| `MotionType` | Enum (`Static` immovable, `Kinematic` script-driven, `Dynamic` solver-driven); fixes which Jolt body kind the mirror creates. |
| `Collider` | ECS component referencing one immutable `ShapeHandle` plus offset transform, density, and `CollisionLayer`. The shape blob is owned upstream. |
| `ShapeHandle` | Opaque handle into the physics-owned shape table; backs a Jolt `Shape` (sphere, box, capsule, convex hull, compound, mesh). Reference-counted. |
| `ShapeBlob` | Serialised, versioned bytes describing a shape (primitive params or baked convex/mesh); produced by `content`/`geometry` at cook, consumed by physics at load. |
| `BodyId` | Stable 32-bit physics handle (Jolt `BodyID`); maps 1:1 to a Jolt body. Allocation order is deterministic for replay. |
| `CollisionLayer` | 32-bit layer index per body/shape. The layer-pair interaction matrix (`PhysicsConfig`) encodes which layers collide and which only emit triggers. |
| `PhysicsMaterial` | Asset describing surface response: friction, restitution, density, friction/restitution combine modes. Indexed by `Collider` and per-triangle on mesh shapes. |
| `Joint` | ECS entity carrying a constraint between two `BodyId`s: `Fixed`, `Revolute`, `Prismatic`, `Distance`, `Generic6Dof`. Mirrors a Jolt `Constraint`. |
| `JointLimits` / `JointMotor` | Optional ECS components on a `Joint` entity declaring bounded ranges and powered drive targets respectively. |
| `JointBreakThreshold` | Optional component on a `Joint` declaring force/torque limits past which the joint despawns and emits `JointBroken`. |
| `Trigger` | Marker component flagging a `Collider` as no-response; overlap drives `TriggerEnter` / `TriggerStay` / `TriggerExit` events instead of contact impulses. |
| `ContactManifold` | Per-pair component (or event payload) carrying contact points, normals, separations, and per-point accumulated impulses; written by narrowphase, consumed by gameplay. |
| `CollisionStarted` / `CollisionPersisted` / `CollisionEnded` | ECS event components emitted at substep boundaries describing pair lifecycle. Same-frame delivery — phase 3 emits, phase 5+ may read. |
| `TriggerEnter` / `TriggerStay` / `TriggerExit` | ECS event components for trigger-volume lifecycle, mirrored from Jolt's contact listener with the contact-response bit cleared. |
| `Sleeping` | Marker component added when a body's island has been at rest below the `PhysicsConfig` thresholds for the configured frame count; removed on external force, torque, or new contact. |
| `Island` | Jolt-internal connected component of bodies coupled by contacts or constraints; physics exposes membership read-only for diagnostics. |
| `CCD` | Continuous-collision flag on a `RigidBody`; selects swept-volume narrowphase to prevent tunneling for fast movers. |
| `PhysicsQueries` | ECS resource exposing the synchronous query API (`ray_cast`, `shape_cast`, `overlap`, `closest_point`) backed by the same Jolt broadphase used in phase 3. |
| `QueryFilter` | Predicate combining `CollisionLayer` mask, ECS-component requirements, and an optional callback; passed to every `PhysicsQueries` call. |
| `QueryHit` | Plain-data result row (entity, point, normal, distance, layer, material) returned from a query; never references Jolt internals across the ABI. |
| `BroadphaseLayer` | Coarse Jolt broadphase bucket; `PhysicsConfig` maps each `CollisionLayer` to a `BroadphaseLayer` for fast-static-vs-dynamic culling. |
| `PhysicsSnapshot` | Fory-serialised snapshot of `BodyId`-keyed body state used for golden-trace replay and cross-host determinism gates; the byte-equality unit. |
| `JoltMiddleman` | The middleman dylib carrying the Jolt-derived ABI types both `physics` and the engine link against; ABI hash gates plugin load (PHILOSOPHY §9). |

## 3. Derived From

Harmonius requirement IDs / file paths cited as research input. Note any
collapse decisions (multiple harmonius concepts → one glibre primitive).

## 4. Aggregates & Invariants

- Aggregate / entity / value object.
- Invariants that must hold at every public API boundary.

## 5. Public Interface

```cpp
// header-only stub goes here
```

Event types, serialized schemas (Fory), error types.

## 6. Internal Architecture

Non-binding sketch for implementers.

## 7. Persistence & Schemas

Fory schemas. Migration rules.

## 8. Hot-Reload Contract

What survives swap, what `migrate(...)` must do, what triggers refusal.

## 9. Performance Budget

Cycles / frame, memory ceiling, allocation rules.

## 10. Failure Modes & Error Model

Typed errors. Recovery.

## 11. Acceptance Criteria

GitHub `type:user-story` issues this spec closes:

- #TBD — `<title>`

Each must have a Catch2 test by name.

## 12. Open Questions

- Owner / resolution gate.
