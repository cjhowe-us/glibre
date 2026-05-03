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

Harmonius physics prior art was mined as **research input only**; every
conclusion below is independently re-derived against `PHILOSOPHY.md`
(SOLID, SRP, plugin-only growth, determinism by default, static codegen,
no runtime reflection in shipping builds), the engine-wide nine-phase
frame schedule (`reviews/decisions/frame-phases.md`), and the §1/§2
commitments above. Cited paths live under
`/Users/cjhowe/Code/harmonius/docs/`. Per PHILOSOPHY §10 every
multi-source concept is collapsed into the smallest glibre primitive
that still satisfies SRP; per PHILOSOPHY §3 any concern that does not
trace back to "produce a deterministic Jolt-backed rigid-body step
inside frame phase 3" is refused and routed to the owning context.

### 3.1 Cited harmonius sources

| Cluster | Harmonius file(s) | Used for |
|---------|-------------------|----------|
| Rigid-body dynamics | `requirements/physics/rigid-body-dynamics.md` (R-4.1.1 .. R-4.1.10, R-4.1.NF1 .. R-4.1.NF3), `design/physics/foundation.md` (§ "Architecture", "ECS Component Map", "Substep Pipeline Sequence", "Integration System", "Sleep System", "CCD") | Fixed-timestep accumulator + substeps, per-world `PhysicsConfig`, motion-type taxonomy, sleeping thresholds, CCD as a body-flag, ECS↔physics mirror shape (`RigidBody`, `Velocity`, `AngularVelocity`, `ExternalForce`, `ExternalTorque`), deterministic seeding, cross-platform byte-equality requirement (R-4.1.NF3). |
| Collision detection | `requirements/physics/collision-detection.md` (R-4.2.1 .. R-4.2.9, R-4.2.NF1 .. R-4.2.NF3), `design/physics/foundation.md` (§ "Broadphase", "Narrowphase", "Collision Events", "Trigger Volumes") | Broadphase + narrowphase as one stage owned by Jolt, primitive + convex + mesh + heightfield + compound shape taxonomy, `CollisionLayer` bitmask + interaction matrix, per-triangle materials, contact-event lifecycle (`CollisionStarted` / `Persisted` / `Ended`), trigger lifecycle (`TriggerEnter` / `Stay` / `Exit`), `PhysicsMaterial` with friction/restitution/density/combine modes, same-frame event delivery (R-4.2.NF3). |
| Constraints & joints | `requirements/physics/constraints-and-joints.md` (R-4.3.1 .. R-4.3.6, R-4.3.9, R-4.3.NF1 .. R-4.3.NF3), `design/physics/constraints.md` (§ "Architecture", "Joint Types", "Warm Starting") | Joint taxonomy (`Fixed`, `Revolute`, `Prismatic`, `Distance`, `Generic6Dof`), `JointMotor` and `JointLimits` as optional components, `JointBreakThreshold` + `JointBroken` event, warm-start factor as a `PhysicsConfig` knob, joint-as-ECS-entity model, deterministic-given-ordering solver requirement. |
| Spatial queries | `requirements/physics/spatial-queries.md` (R-4.4.1 .. R-4.4.8), `design/physics/advanced.md` (§ "Spatial Queries") | `PhysicsQueries` ECS resource (R-4.4.7), `RayCast` / `ShapeCast` / `Overlap` / closest-point surface, oriented shape casts (R-4.4.8), `QueryFilter` combining layer mask + ECS-component requirements + custom predicate, plain-data `QueryHit` rows, shared broadphase between phase-3 stepping and queries. |
| Frame integration | `design/physics/foundation.md` (§ "Substep Pipeline Sequence", "Architecture") | The "physics owns one phase, all stages run inside it" shape; no cross-phase reads of solver-internal state; all events emitted at substep boundaries before the phase exits. |

Inputs read but **not** adopted as physics responsibilities (see §3.3):
the cloth/soft-body XPBD surface
(`requirements/physics/soft-body-and-cloth.md`,
`design/physics/advanced.md` cloth half), the SPH/FLIP/Eulerian fluid
surface (`requirements/physics/fluid-simulation.md`), the
Voronoi/V-HACD/SDF destruction surface
(`requirements/physics/destruction-and-fracture.md`,
`design/physics/advanced.md` destruction half), the Pacejka/track/hover
vehicle surface (`requirements/physics/vehicle-physics.md`), the
character-controller surface
(`requirements/physics/rigid-body-dynamics.md` R-4.1.8 .. R-4.1.10,
R-4.1.17 .. R-4.1.20), the gyroscopic/rolling/directional friction
extensions (R-4.1.11 .. R-4.1.13), the per-world / multi-planet gravity
modes (R-4.1.14 .. R-4.1.15), the 2D physics mode (R-4.1.16), the
ragdoll / limb severance / prosthetic surface (R-4.3.4, R-4.3.7,
R-4.3.8, R-4.3.10 .. R-4.3.12), the V-HACD bake (R-4.2.11), the runtime
quickhull (R-4.2.12), and the editor 32×32 layer-matrix authoring UX
(R-4.2.13 — physics consumes the baked matrix only).

### 3.2 Occam collapses (multiple harmonius concepts → one glibre primitive)

1. **Many specialised physics subsystems → one Jolt-backed
   `PhysicsWorld` + middleman dylib.** Harmonius shipped a hand-rolled
   stack: bespoke broadphase BVH (R-4.2.1), bespoke narrowphase
   (GJK / EPA / SAT, R-4.2.2), bespoke island builder (R-4.1.5), bespoke
   integrator (symplectic Euler, R-4.1.1), bespoke contact solver
   (R-4.1.3), bespoke CCD (R-4.1.4), bespoke sleep system (R-4.1.6), and
   bespoke constraint solver (sequential-impulse + TGS, R-4.3.6) split
   across `harmonius_physics::foundation` and
   `harmonius_physics::constraints`. Glibre collapses the entire
   simulation kernel to a single primitive: **one `PhysicsWorld` per ECS
   `World`, backed by one Jolt `PhysicsSystem`**, with the Jolt-derived
   ABI types carried by the `JoltMiddleman` dylib (PHILOSOPHY §9). Glibre
   owns the ECS↔Jolt mirror (§2 component glossary), the deterministic
   `PhysicsConfig`, the `Accumulator`, the spatial-query surface, and the
   shape-blob loader; Jolt owns broadphase, narrowphase, integration,
   solving, CCD, sleeping, and island parallelism. There is no second
   physics kernel. Justification: SRP — one reason to change "physics
   simulation math" is "Jolt evolves"; that lives behind one seam, not
   eight. PHILOSOPHY §5 (greatly reduced MVP scope) and §10 (Occam) close
   the case: re-implementing Jolt's eight subsystems is the maximally
   non-minimal alternative.

2. **Multiple solver / integrator / timestep options → one deterministic
   fixed-timestep accumulator owning frame phase 3.** Harmonius required
   selectable integrators (symplectic Euler / Verlet, F-4.1.1),
   selectable solvers (sequential-impulse vs. TGS via `SolverConfig`,
   R-4.3.6), tunable warm-start factor (R-4.3.10), per-entity substep
   overrides (R-4.1.2), per-platform iteration counts, and a separate
   `FixedUpdate` schedule that could re-enter mid-frame (R-4.2.10). Glibre
   collapses **every** timing and solver knob into a single
   `PhysicsConfig` resource fixed at world init, consumed by exactly one
   actor — the `Accumulator` — which owns frame phase 3 end to end per
   `reviews/decisions/frame-phases.md`. The accumulator advances by the
   `core` frame `dt`, drains whole substeps in a deterministic loop, and
   carries the remainder across frames. There is no per-entity substep
   override (rejected: it forks determinism), no runtime solver swap
   (rejected: cross-host byte-equality demands one path), and no
   schedule re-entry (rejected: phase 3 is a single barrier per
   `frame-phases.md`). Open question 2 of `frame-phases.md` is dormant
   for the same reason — physics rate equals render rate at MVP and the
   accumulator handles spikes via its remainder. Justification:
   PHILOSOPHY §7 (determinism by default — physics + ECS world snapshots
   byte-equal across hosts and runs); SRP — one reason to change "what
   advances the sim clock" lives in one struct.

3. **Hand-rolled BVH + per-domain spatial indices → Jolt's broadphase as
   the single spatial truth, exposed through one `PhysicsQueries`
   resource.** Harmonius required a "shared BVH spatial index" used by
   physics, AI, navigation, rendering, and scripting (R-4.2.1, R-4.4.1
   .. R-4.4.7). Glibre keeps the unification but inverts the ownership:
   the only spatial structure physics owns is **Jolt's own broadphase**,
   and the only public surface over it is the `PhysicsQueries` ECS
   resource (R-4.4.7) returning plain-data `QueryHit` rows
   (`RayCast` / `ShapeCast` / `Overlap` / closest-point, with `QueryFilter`
   combining layer mask + ECS predicate + callback per R-4.4.6). Render
   does not query the physics broadphase (it owns its own HZB / cull in
   `render` per the render spec); navigation will own its own nav BVH
   when it lands. Cross-domain "shared spatial index" is rejected as a
   premature abstraction (PHILOSOPHY anti-pattern: cross-domain
   abstractions invented before two concrete users exist). Justification:
   SRP — `physics` owns one spatial structure (Jolt's broadphase) and
   one query API.

4. **Many shape kinds (primitive / convex / mesh / heightfield / compound
   / 2D primitives / SDF voxel / quickhull / V-HACD) → one
   `ShapeHandle` over a deterministic `ShapeBlob` table.** Harmonius
   enumerated box / sphere / capsule / convex-hull / triangle-mesh /
   heightfield / compound (R-4.2.3 .. R-4.2.5), 2D circle / rectangle /
   capsule2d / convex-polygon / edge / chain (R-4.1.16), runtime
   quickhull (R-4.2.12), V-HACD (R-4.2.11), SDF voxel (R-4.6.8 .. R-4.6.11)
   — each as a separate authoring + runtime path. Glibre collapses the
   runtime side to a single opaque `ShapeHandle` referencing one row of a
   shape table; rows are populated from `ShapeBlob` bytes produced by
   `content` / `geometry` at cook time and consumed by physics at load
   (the bake pipelines are refused, see §3.3). The handle is
   reference-counted, deterministic in allocation order, and carries no
   knowledge of shape kind — Jolt resolves the underlying `Shape`
   subclass internally. 2D primitives, SDF voxel collision, and runtime
   hull generation are post-MVP and re-enter as additional `ShapeBlob`
   variants without changing the handle surface. Justification:
   PHILOSOPHY §10 (Occam) — one handle, one table, one cook-side
   producer.

5. **Forward-+-deferred event paths → one substep-boundary event drain.**
   Harmonius required `CollisionStarted` / `Persisted` / `Ended` (R-4.2.7),
   `TriggerEnter` / `Stay` / `Exit` (R-4.2.8), `JointBroken` (R-4.3.3),
   `JointSevered` (R-4.3.7), and same-frame delivery (R-4.2.NF3) and
   spread emission across an `EventBus`, the `IslandBuilder`, the
   `ContactListener`, and the joint solver. Glibre collapses every
   physics-emitted event to a single rule: **events are written to ECS
   event-component buffers at substep exit, before phase 3 returns**;
   downstream phases (5+) read them in the same frame. There is one
   listener (Jolt's `ContactListener`, wrapped behind the middleman) and
   one drain point. `JointSevered` collapses into `JointBroken` plus a
   spawn done by the gameplay/destruction layer when those land — the
   physics responsibility ends at `JointBroken`. Justification: SRP —
   one reason to change "when do physics events become visible" lives in
   one place, the substep-boundary drain.

6. **Many `Solver*` / `Substep*` / `Island*` / `Sleep*` / `CCD*`
   resources → fields of one `PhysicsConfig`.** Harmonius scattered
   `SolverConfig` (R-4.3.6, R-4.3.10), substep count + per-entity
   override (R-4.1.2), island-parallel toggle (R-4.1.5), sleep
   thresholds (R-4.1.6), CCD enable + thin-wall margin (R-4.1.4),
   warm-start factor (R-4.3.10), and the broadphase-layer matrix
   (R-4.2.13) across multiple ECS resources. Glibre collapses every
   init-time-immutable knob into one `PhysicsConfig` resource (§2);
   the per-entity overrides and runtime mutations are rejected for
   determinism reasons (collapse 2). Per-body knobs that **must** be
   per-body (motion type, mass, inertia, damping, sleeping flag, CCD
   flag) live on `RigidBody` itself; per-shape knobs (layer, density,
   material) live on `Collider`. Justification: SRP — one resource for
   "global deterministic config" is the cleanest seam; per-entity knobs
   live on entity components.

7. **Many "platform tier" knobs → init-time selection only.** Harmonius
   sprinkled per-platform LOD (R-4.3.10 .. R-4.3.12 ragdoll/Verlet
   fallback, R-4.6.7 debris LOD, R-4.7.2 cloth caps, R-4.8.1 SPH caps,
   R-4.5.2 Pacejka fallback, etc.) across every advanced surface. The
   advanced surfaces are refused (§3.3); for the surfaces that remain,
   any tier-driven knob (substep count, solver iters, CCD enable, sleep
   thresholds, broadphase layer matrix) is selected at world init from
   `PhysicsConfig` and never branched on at runtime in the hot path,
   matching PHILOSOPHY §6 (zero runtime reflection in shipping builds).

8. **Multiple "physics world" topologies (per-zone, per-planet, 2D-side)
   → one `PhysicsWorld` per ECS `World`.** Harmonius required cross-zone
   migration preserving momentum (R-4.1.7), per-planet world isolation
   (R-4.1.15), and a 2D-vs-3D coexistence in the same world (R-4.1.16).
   Glibre collapses topology to: **one `PhysicsWorld` per ECS `World`,
   period.** Multi-zone streaming is delegated to whichever context
   owns world streaming (post-MVP); cross-world entity migration is the
   moving plugin's responsibility and falls under hot-reload semantics
   (§8). 2D physics is refused at MVP (§3.3). Justification: SRP +
   PHILOSOPHY §5 — one world topology is the minimum that closes §1 on
   day one.

9. **Persisted snapshot / replay / rollback surfaces → one
   `PhysicsSnapshot` Fory schema.** Harmonius scattered determinism
   verification (R-4.1.NF3 byte-equal across platforms), replay tooling,
   server-authoritative reconciliation hooks, and rollback netcode
   touchpoints across the foundation and constraints docs. Glibre
   collapses every "the simulation state, persisted" need into one
   `PhysicsSnapshot` artifact (§2): a Fory-serialised, `BodyId`-keyed
   dump of body kinematic + sleep + accumulated-impulse state. It is
   the byte-equality unit for the determinism gate, the replay record
   for the editor, and the post-MVP rollback handle. There is one
   schema, owned by `physics`, coordinated with `data` per the
   per-context SDLC. Justification: SRP — one reason to change "what
   counts as the physics state" lives in one schema.

### 3.3 Refusals (routed to other contexts)

Glibre's physics plugin does **not** own any of the following, even
though harmonius collected them under "physics". Each routes per §1:

| Harmonius surface | Cited file(s) | Routed to |
|-------------------|----------------|-----------|
| Soft body and cloth (XPBD, ClothSimulation, ClothAttachment, self-collision, two-way coupling, wind field, tearing, cloth LOD) | `requirements/physics/soft-body-and-cloth.md` R-4.7.1 .. R-4.7.7, `design/physics/advanced.md` (cloth half) | Deferred plugin (post-MVP `cloth` / `vfx`). Physics MVP does not own XPBD; the rigid-body kernel does not link a cloth solver. |
| Fluid simulation (SPH, FLIP/PIC, Eulerian grid, surface reconstruction, WaterSurface, buoyancy, two-way fluid-rigid coupling) | `requirements/physics/fluid-simulation.md` R-4.8.1 .. R-4.8.7 | Deferred plugin (post-MVP `fluid` / `vfx`). Physics MVP applies no buoyancy or drag forces from fluid volumes. GPU compute paths route through `render` / `vfx` (§1). |
| Destruction and fracture (Voronoi bake, pre-fractured DCC import, runtime fragment spawn, DamageHealth progressive damage, stress propagation, debris LOD, voxel SDF subtraction) | `requirements/physics/destruction-and-fracture.md` R-4.6.1 .. R-4.6.11, `design/physics/advanced.md` (destruction half) | Deferred plugin (post-MVP `destruction`). Physics consumes already-baked compound `ShapeBlob`s and emits `JointBroken`; everything above (fragment spawn, mass distribution, debris pooling) is the destruction layer. Voronoi / V-HACD / SDF bakes are `tools` + `content` / `geometry` cook responsibilities. |
| Vehicle physics (suspension, Pacejka tires, drivetrain, anti-roll, tracked vehicles, hover repulsors, replication) | `requirements/physics/vehicle-physics.md` R-4.5.1 .. R-4.5.7, `design/physics/advanced.md` (vehicle half) | Deferred plugin (post-MVP `vehicle`). Physics provides only the rigid-body + joint primitives; vehicle archetypes and Pacejka curves live elsewhere. |
| Kinematic character controller (ground detection, slope sliding, step climbing, moving platforms, coyote-time, wall sliding, multi-jump / wall-jump / jump buffer, crouching, push forces) | `requirements/physics/rigid-body-dynamics.md` R-4.1.8 .. R-4.1.10, R-4.1.17 .. R-4.1.20 | Deferred plugin (post-MVP `character` or gameplay framework). Physics exposes shape casts and overlap queries (`PhysicsQueries`); the controller composing those into ground / wall / step logic is gameplay, not physics. |
| Animation, skeletal pose evaluation, IK, ragdoll activation from animation, ragdoll LOD, limb severance and prosthetic re-attachment | `requirements/physics/constraints-and-joints.md` R-4.3.4, R-4.3.7, R-4.3.8, R-4.3.10 .. R-4.3.12, `design/physics/constraints.md` (ragdoll half), `design/physics/advanced.md` (ragdoll LOD half) | Deferred plugin (post-MVP `animation`). Phase 4 is reserved per `reviews/decisions/frame-phases.md`. Physics provides joints + `JointBroken`; ragdoll authoring, blend, and severed-limb spawning live in animation/gameplay. |
| Navigation and pathfinding | implied by R-4.4.x query consumers | Deferred plugin (post-MVP `navigation`). Will own its own nav BVH; physics queries are not the nav backbone (collapse 3). |
| Gyroscopic torque, rolling friction, directional friction (`friction_direction` / `lateral_friction`) | `requirements/physics/rigid-body-dynamics.md` R-4.1.11 .. R-4.1.13 | Deferred. Reachable as Jolt-feature follow-ups; not in MVP §1. |
| Per-world / multi-planet gravity modes (`Uniform` / `Radial` / custom function), inter-planetary world migration | `requirements/physics/rigid-body-dynamics.md` R-4.1.14 .. R-4.1.15 | Deferred. MVP `PhysicsConfig.gravity` is a fixed `Vec3` per world; multi-planet topology is post-MVP and orthogonal to the §1 responsibility. |
| 2D rigid-body mode (scalar inertia, 2D shapes, separate 2D BVH, 2 linear + 1 angular DoF solver) | `requirements/physics/rigid-body-dynamics.md` R-4.1.16 | Deferred. Possible post-MVP via a parallel `PhysicsWorld` over Jolt's 2D primitives or a sibling 2D plugin; not part of MVP §1. |
| V-HACD bake, offline convex decomposition, runtime quickhull, voxel-chunk collider regeneration, editor 32×32 layer-matrix authoring UX | `requirements/physics/collision-detection.md` R-4.2.10 .. R-4.2.13 | `tools` (editor authoring UX) + `content` / `geometry` (cook-time bake). Physics consumes the baked compound `ShapeBlob` and the baked layer matrix; it does not perform decomposition. |
| Shape authoring, mesh-collider authoring, heightfield authoring | `requirements/physics/collision-detection.md` (authoring half of R-4.2.4 .. R-4.2.5) | `tools` + `content` / `geometry`. Physics is a consumer of `ShapeBlob` bytes only. |
| Engine frame schedule, ECS runtime, plugin loader, hot-reload protocol | `design/physics/foundation.md` (§ "Architecture", "Substep Pipeline Sequence") | `core`. Physics owns only phase 3's body and registers no systems into other phases (§1, `reviews/decisions/frame-phases.md`). |
| Asset bytes, Fory codegen pipeline, on-disk schema versioning | implied across `requirements/physics/*` (every "asset" reference) | `data`. Physics declares `PhysicsConfig`, `PhysicsMaterial`, and `PhysicsSnapshot` schemas; `data` codegens the serialiser and migration table. |
| Window, input, swapchain, frame pacing | none direct in harmonius physics; explicit per `frame-phases.md` phases 1 and 9 | `platform`. |
| Visual scripting / gameplay intents | implied by phase 2 in `frame-phases.md` | Deferred plugin (post-MVP `logic`). Physics drains `ExternalForce` / `ExternalTorque` written by phase-2 systems and zeros them at substep exit; it does not own the scripting surface. |

These refusals are the application of PHILOSOPHY §3 (minimal core,
plugin-only growth) + §1 (SRP) + §5 (greatly reduced MVP scope) to the
harmonius "physics" umbrella: anything whose reason-to-change does not
collapse to "produce a deterministic Jolt-backed rigid-body step inside
frame phase 3" lives in another plugin or another context.

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
