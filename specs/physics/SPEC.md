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

This section enumerates physics's aggregates, entities, and value
objects, and the invariants every public boundary must hold. Aggregates
are listed in data-flow order (config → mirror in → step → mirror out →
expose). Each aggregate owns one dimension of "produce a deterministic
Jolt-backed rigid-body step inside frame phase 3"; per PHILOSOPHY §1
(SRP) an aggregate is admitted to this list only when its single
reason-to-change does not collapse into another's. Where two harmonius
primitives reduce to one glibre primitive the collapse is cited from
§3.2; cross-context concerns (frame schedule, ECS runtime, asset bytes,
shape bake, scripting, rendering) are explicitly delegated and never
re-asserted here (§3.3).

### 4.1 Aggregate roster

#### 4.1.1 `PhysicsWorld` — root entity owning one Jolt instance (aggregate root)

**Reason to change:** what physics owns inside an ECS `World` — one
Jolt `PhysicsSystem` and the seam around it. Distinct from the
deterministic configuration that drives it (§4.1.2) and from the
clock that advances it (§4.1.3).

**Composition.** Owns exactly one Jolt `PhysicsSystem` instance, the
broadphase + narrowphase + constraint set + contact listener that come
with it, the `BodyId` allocator, the `ShapeBlob` / `ShapeHandle` table
(§4.1.6), the active `Joint` registry (§4.1.7), the contact-event
drain buffers (§4.1.8), and the `JoltMiddleman` (§4.1.13) handle
through which every Jolt type crosses the plugin ABI. Holds
back-pointers to `PhysicsConfig` (§4.1.2), `Accumulator` (§4.1.3), and
`PhysicsQueries` (§4.1.10). Holds **no** ECS-component pointers — the
ECS↔Jolt mirror is performed at substep entry / exit by the world
itself (§3.2 collapse #1).

**Identity & lifetime.** One `PhysicsWorld` per ECS `World` (§3.2
collapse #8); created at world init from a frozen `PhysicsConfig`,
destroyed at world teardown. Survives hot-reload of the physics plugin
when the `JoltMiddleman` ABI hash matches (§8); refused otherwise per
`reviews/decisions/error-model.md`.

**Public-boundary invariants.**

1. **Owns frame phase 3 entirely.** Every byte of work that advances
   the simulation lives inside phase 3 of the owning `World`'s frame
   loop (`reviews/decisions/frame-phases.md`). No phase-3 work runs
   outside phase 3; no phase ≠ 3 calls into Jolt's stepping API.
2. **One Jolt `PhysicsSystem` per world, period.** The world holds
   no second simulation kernel and exposes no second Jolt instance.
   Multi-zone / multi-planet / 2D-vs-3D topology is post-MVP and
   re-enters as additional `PhysicsWorld` instances on additional
   `World`s (§3.2 collapse #8).
3. **No exception path.** Every fallible public method returns
   `glibre::Result<T>` per `reviews/decisions/error-model.md`;
   exceptions thrown by Jolt are caught at the `JoltMiddleman` ingress
   and translated into `physics::Error` arms before crossing the ABI.
4. **ECS↔Jolt mirror is one-way per substep.** ECS-side writes
   (`ExternalForce`, `ExternalTorque`, kinematic transform overrides,
   joint motor targets) are committed into Jolt at substep entry;
   Jolt-side writes (`Velocity`, `AngularVelocity`, `GlobalTransform`
   inputs, contact events, sleeping flags) are committed back to ECS
   at substep exit. There is no mid-substep cross-traffic (§3.2
   collapse #5).

#### 4.1.2 `PhysicsConfig` — frozen deterministic configuration (value object)

**Reason to change:** the deterministic knob set that makes the
simulation reproducible across hosts and runs. Distinct from the
per-body knobs on `RigidBody` (§4.1.5) and the per-collider knobs on
`Collider` (§4.1.6).

**Composition.** A flat record holding gravity (`Vec3`), fixed
substep `dt`, `substep_count` per frame, solver `velocity_iters` and
`position_iters`, `warm_start_factor`, linear + angular sleep
thresholds and frame counts, CCD enable bit, the `BroadphaseLayer`
mapping table (`CollisionLayer` → `BroadphaseLayer`), the layer-pair
interaction matrix (collide / trigger-only / ignore), the RNG seed,
and the maximum bodies / shapes / contacts / constraints budgets the
Jolt `PhysicsSystem` is sized against. Authored as a Fory-serialised
asset by `data` (§7) and consumed at world init.

**Identity & lifetime.** One `PhysicsConfig` per `PhysicsWorld`,
captured by value at init, **immutable for the lifetime of the
world**. A new config means a new world.

**Public-boundary invariants.**

1. **Init-time-immutable.** No public API mutates a `PhysicsConfig`
   after `PhysicsWorld` construction. Runtime tuning is rejected
   (§3.2 collapse #2) — re-tuning is "build a new world".
2. **Deterministic by construction.** Every field that influences the
   stepping math (substep `dt`, iteration counts, warm-start factor,
   sleep thresholds, layer matrix, RNG seed) is fixed-point or
   bit-exact float; `PhysicsConfig` carries no host-specific
   intrinsics path or platform-tier branch (PHILOSOPHY §6, §7).
3. **Single source of global knobs.** Every init-time-immutable
   simulation knob (collapse #6) lives on `PhysicsConfig`; per-body
   knobs (motion type, mass, damping, sleeping flag, CCD flag) live
   on `RigidBody` (§4.1.5); per-shape knobs (layer, density,
   material) live on `Collider` (§4.1.6). No knob is duplicated.
4. **Layer matrix is total.** The `BroadphaseLayer` mapping covers
   every defined `CollisionLayer`; the layer-pair interaction matrix
   is fully specified (collide / trigger-only / ignore for every
   ordered pair). An incomplete matrix at world init returns
   `physics::Error::ConfigInvalid`.

#### 4.1.3 `Accumulator` — fixed-timestep clock for phase 3 (value object)

**Reason to change:** how wall-clock frame `dt` is converted into a
deterministic count of fixed substeps. Distinct from what a substep
does (§4.1.4) and from the config it consumes (§4.1.2).

**Composition.** Holds the carried remainder (`f32` seconds), the
last-frame stamp, and a pre-resolved pointer to its owning world's
`PhysicsConfig.dt`. Drained once per frame at phase-3 entry: `acc +=
core_dt`, then while `acc >= dt && substeps_done < SUBSTEP_CAP_4`,
runs one `Substep` (§4.1.4) and decrements `acc` by `dt`. Carry is
preserved across frames so byte-equal trace replay can re-derive the
substep count.

**Identity & lifetime.** One `Accumulator` per `PhysicsWorld`; lives
inside the world for its entire lifetime. Serialised into
`PhysicsSnapshot` (§4.1.12) so a replay restart resumes with the same
remainder.

**Public-boundary invariants.**

1. **Single owner of phase-3 advancement.** Only the `Accumulator`
   advances the simulation clock; no plugin, gameplay system, or test
   harness may call into Jolt's `Step` outside the accumulator's
   draining loop (§3.2 collapse #2).
2. **Bounded catch-up.** The substep loop is hard-capped at four
   substeps per frame; remainder above that is **dropped** (carry
   reset to zero) and a `physics::Warning::AccumulatorClamped` is
   logged. The simulation never falls behind by more than four
   substeps' worth of wall-clock work, and frame-loop pacing never
   busy-waits inside phase 3 (PHILOSOPHY §7 — determinism over
   wall-clock fidelity under a stall).
3. **Carry preserved.** Whatever `acc` remains under `dt` after the
   draining loop is preserved verbatim into the next frame; it is the
   determinism unit that makes spike-induced frame-rate variation
   re-converge to the same trajectory across hosts.
4. **No re-entry.** Phase 3 is a single barrier per
   `frame-phases.md`; the accumulator's draining loop is the only
   loop that re-enters Jolt's `Step` within a frame, and it never
   re-enters phases 2–5 as a sub-graph.

#### 4.1.4 `Substep` — one fixed `dt` Jolt step (value object)

**Reason to change:** what counts as one deterministic step (the unit
of byte-equality across hosts). Distinct from how many of them run
this frame (§4.1.3).

**Composition.** Not a stored object — a logical pipeline executed by
`PhysicsWorld::step_one(...)` per Jolt step: (1) **commit ECS → Jolt**
(drain `ExternalForce` / `ExternalTorque`, apply kinematic overrides,
apply joint motor targets, zero the per-frame accumulator
components), (2) Jolt's broadphase + narrowphase + constraint solve +
contact resolution + integration (one call into Jolt — see §3.2
collapse #1), (3) **commit Jolt → ECS** (write `Velocity` /
`AngularVelocity` / position back into ECS, drain Jolt's contact
listener into the contact-event buffers (§4.1.8), update `Sleeping`
markers (§4.1.14), update `Island` membership read-only).

**Identity & lifetime.** Stack-resident; lives only for the duration
of one Jolt `Step` call. The number of substeps run in a given frame
is determined by the `Accumulator` (§4.1.3) and saved into
`PhysicsSnapshot` (§4.1.12) for trace replay.

**Public-boundary invariants.**

1. **One Jolt `Step` per substep.** A substep maps 1:1 to one Jolt
   `Step`; there is no partial step, no nested step, no re-entry.
2. **ECS commit ordering.** ECS writes commit at substep **entry**;
   Jolt writes commit at substep **exit**. Mid-substep cross-traffic
   is forbidden (§3.2 collapse #5). Violations are caught by a debug
   instrumentation hook that asserts no ECS↔Jolt traffic between the
   two commit barriers.
3. **External-force drain is total.** `ExternalForce` and
   `ExternalTorque` components are read in full at substep entry and
   reset to zero at substep exit; a value left over the boundary is
   a programming error and a debug-build assertion fires.
4. **Deterministic given (config, body set, input).** Two substeps
   with byte-equal `PhysicsConfig`, byte-equal body / collider /
   joint state, and byte-equal `ExternalForce` / `ExternalTorque` /
   kinematic overrides produce byte-equal post-substep state. No
   field on any aggregate observed by a substep depends on host
   thread count, allocator address, or wall-clock time
   (PHILOSOPHY §7, R-4.1.NF3).

#### 4.1.5 `RigidBody` — per-entity body component + Jolt mirror (entity)

**Reason to change:** per-body kinematic and dynamic state; the
ECS-side projection of one Jolt body. Distinct from the shape it
collides with (§4.1.6) and from joints binding it (§4.1.7).

**Composition.** ECS component carrying `MotionType`
(`Static` / `Kinematic` / `Dynamic`), mass, inertia tensor (or auto-derived
flag), linear + angular damping, the `BodyId` (§4.1.5b) of its Jolt
mirror, the CCD flag, the sleeping flag (§4.1.14), and back-references
to its `Velocity` / `AngularVelocity` companion components.
`ExternalForce` and `ExternalTorque` are co-located on the same
entity as accumulator components; physics drains them on the substep
boundary (§4.1.4 invariant 3). Per-body knobs that **must** be
per-body live here; everything else lives in `PhysicsConfig` (§3.2
collapse #6).

**Identity & lifetime.** One `RigidBody` component per simulating
entity; created when the entity gains the component, removed when
the component is dropped. Creation allocates a Jolt `BodyID` via
the world's deterministic allocator; removal frees it. Survives
hot-reload of the physics plugin (§8).

##### 4.1.5b `BodyId` — stable handle into Jolt's body table (value object)

**Composition.** A 32-bit stable handle (Jolt `BodyID`) issued by
`PhysicsWorld`'s deterministic allocator. Allocation order is fixed
by the order in which `RigidBody` components materialise in the
world (the ECS materialisation order is itself deterministic —
PHILOSOPHY §7).

**Public-boundary invariants on `RigidBody` + `BodyId`.**

1. **`BodyId` is stable across reload.** Allocation order is a
   function of ECS body insertion order, not of host-thread
   scheduling or allocator address. After a hot-reload swap the same
   ECS entities resolve to the same `BodyId` values; persisted
   replays that key on `BodyId` re-bind without rewriting (§3.2
   collapse #9, PHILOSOPHY §8).
2. **One `BodyId` per `RigidBody`.** A `RigidBody` component holds
   exactly one `BodyId`; a `BodyId` resolves to exactly one ECS
   entity inside its owning world. Cross-world `BodyId` lookups
   return `physics::Error::BodyNotFound`.
3. **`MotionType` fixes Jolt body kind.** A body's `MotionType` is
   established at `RigidBody` creation and pinned for that
   `BodyId`'s lifetime; switching motion type means destroying the
   `RigidBody` and re-adding it (which allocates a new `BodyId`).
4. **Static + Kinematic invariants.** `Static` bodies carry zero
   `Velocity` / `AngularVelocity` and ignore `ExternalForce` /
   `ExternalTorque`; `Kinematic` bodies are integrated by the
   ECS-side script writing transforms + linear/angular targets and
   are immune to solver impulse. Violating either is a debug-build
   assertion.

#### 4.1.6 `Collider` / `ShapeHandle` / `ShapeBlob` — collision geometry mirror (entity + value object + value object)

**Reason to change:** what shape an entity collides with and how
that shape's bytes are sourced. Distinct from body dynamics
(§4.1.5).

**Composition.**

- **`Collider`** — ECS component carrying one `ShapeHandle`, an
  offset transform from body to shape, a density override (or
  default-from-`PhysicsMaterial` flag), the `CollisionLayer`, the
  `PhysicsMaterial` reference, and an optional `Trigger` marker (see
  §4.1.9). Per-shape knobs that must be per-shape live here;
  everything else lives in `PhysicsConfig` (§3.2 collapse #6).
- **`ShapeHandle`** — opaque, reference-counted handle into the
  world's shape table. Carries a `ShapeBlob` content hash and a
  resolved Jolt `Shape` pointer. The handle itself is the only thing
  that crosses the plugin ABI; Jolt `Shape` pointers never escape
  the middleman seam.
- **`ShapeBlob`** — versioned, immutable byte payload (primitive
  parameters, baked convex hull, baked triangle mesh, baked
  heightfield, baked compound) authored by `content` / `geometry` at
  cook time (§3.3). Identified by content hash; never re-bakes at
  runtime.

**Identity & lifetime.** A `Collider` lives on its entity; its
`ShapeHandle` is reference-counted by `PhysicsWorld`'s shape table.
A `ShapeBlob` is loaded into the table at first reference and
unloaded when its refcount drops to zero. Multiple `Collider`s
referencing the same `ShapeBlob` content hash share one row of the
table.

**Public-boundary invariants.**

1. **`ShapeBlob`s are shared by hash.** A given content hash maps to
   exactly one row of the shape table for the lifetime of the world;
   a second reference resolves to the same `ShapeHandle`. Two
   distinct hashes never share a row (§3.2 collapse #4).
2. **Shape kind invisible at the seam.** `ShapeHandle` carries no
   kind tag visible to ECS code; primitive vs. convex vs. mesh vs.
   compound is resolved internally by Jolt. Adding a new kind (2D
   primitives, SDF voxel, runtime quickhull — all post-MVP per
   §3.3) introduces a new `ShapeBlob` variant without changing the
   handle surface.
3. **`Collider` is a thin mirror.** The component never owns shape
   bytes; it owns one `ShapeHandle` plus the per-instance metadata
   (offset, layer, material, trigger flag). Mutating the shape means
   replacing the `ShapeHandle`, not editing the blob.
4. **Trigger flag is shape-side.** Whether a contact pair receives
   solver impulse or only emits trigger events is determined by the
   `Trigger` marker on `Collider` plus the layer-pair interaction
   matrix in `PhysicsConfig`; per-frame mutation is forbidden.
5. **Compound shapes are pre-baked.** Multi-part shapes (vehicle
   chassis, fractured rubble) ship as a single compound `ShapeBlob`
   produced by `content` / `geometry`'s V-HACD / authoring path
   (§3.3); physics never decomposes at runtime.

#### 4.1.7 `Joint` — constraint between two bodies (entity)

**Reason to change:** which constraint topology binds two bodies and
its tuning. Distinct from the bodies it binds (§4.1.5).

**Composition.** ECS entity (a joint **is** an entity, not a
component-on-a-body — collapse-aligned with harmonius and SRP) bearing
a `Joint` component declaring the joint kind (`Fixed` / `Revolute` /
`Prismatic` / `Distance` / `Generic6Dof`), the two `BodyId` endpoints,
the anchor frame on each body, a Jolt `ConstraintRef` produced by the
mirror, and optional companion components: `JointLimits` (bounded
angular / linear ranges), `JointMotor` (powered drive target), and
`JointBreakThreshold` (force / torque limit triggering despawn). The
joint kind set is fixed; specialised joint surfaces (ragdoll, severable
limbs) are post-MVP plugin work (§3.3).

**Identity & lifetime.** One Jolt constraint per `Joint` entity;
created at component-add, destroyed at component-remove or when a
break threshold trips. On break, the entity is despawned by physics
and a `JointBroken` event is emitted (§4.1.8).

**Public-boundary invariants.**

1. **Joint = ECS entity, not a body component.** Despawning a joint
   despawns its entity; a body holds no list of joints, only Jolt
   resolves connectivity. Removing a body that is still referenced
   by a joint returns `physics::Error::JointDanglingEndpoint` and
   refuses the body removal until the joint is despawned first.
2. **Companion components are optional.** Absence of `JointLimits`
   means unbounded; absence of `JointMotor` means passive; absence
   of `JointBreakThreshold` means unbreakable. Adding a companion
   mid-run is permitted (it crosses one substep boundary to commit);
   the kind itself is fixed at creation.
3. **Warm-start is config-global.** The warm-start factor lives in
   `PhysicsConfig` (§3.2 collapse #2), not on the joint; per-joint
   solver overrides are rejected for determinism.
4. **`JointBroken` is the only physics-emitted lifecycle event.**
   Severance, fragment spawn, prosthetic re-attachment are post-MVP
   plugin work (§3.3); physics's responsibility ends at emitting
   `JointBroken` on threshold trip and despawning the joint entity
   (§3.2 collapse #5).

#### 4.1.8 `ContactManifold` / `ContactEvent` — contact data + lifecycle (value object + value object)

**Reason to change:** what contact information crosses the seam and
how its lifecycle is observed. Distinct from triggers (§4.1.9).

**Composition.**

- **`ContactManifold`** — per-pair value object carrying contact
  points (positions in world space), per-point separations, the
  contact normal, the `PhysicsMaterial` at each side, and per-point
  accumulated normal + friction impulses. Written by Jolt's
  narrowphase, mirrored across the `JoltMiddleman` seam as plain
  data, attached as the payload of `CollisionPersisted` /
  `CollisionStarted` events.
- **`CollisionStarted` / `CollisionPersisted` / `CollisionEnded`**
  — ECS event components written into per-pair event buffers by
  Jolt's `ContactListener` adapter at substep exit (§3.2 collapse
  #5). Each event carries the two `BodyId` endpoints and (for
  `Started` / `Persisted`) the `ContactManifold`.

**Identity & lifetime.** A `ContactManifold` is reborn each
substep; lifetime ends at the next substep that retires the pair.
Events live in a per-frame ECS buffer and are reclaimed at end of
phase 8 (after downstream phases 5+ have read them inside the
same frame).

**Public-boundary invariants.**

1. **Same-frame delivery.** Events emitted by phase 3 are visible to
   phases 5+ inside the same frame (R-4.2.NF3); no event survives
   into the next frame's read window. (§3.2 collapse #5.)
2. **Substep-boundary drain.** Contact events are written at substep
   **exit**, after Jolt's solver has produced final manifolds. No
   event is written mid-substep, no event is written outside phase
   3.
3. **Plain-data payload.** `ContactManifold` carries no Jolt-internal
   pointers; the only handles inside it are `BodyId` (§4.1.5b) and
   `PhysicsMaterial` references. Cross-ABI traffic is by value.
4. **Lifecycle is monotone.** A pair transitions
   `Started → Persisted* → Ended`; no `Persisted` without prior
   `Started`, no `Ended` without prior `Started`. Misorderings are
   debug-build assertions on the listener adapter.

#### 4.1.9 `Trigger` / `TriggerEvent` — overlap-only volumes + lifecycle (value object + value object)

**Reason to change:** how no-response collision volumes are flagged
and how their lifecycle becomes ECS visible. Distinct from
contact-emitting colliders (§4.1.8).

**Composition.**

- **`Trigger`** — marker component on a `Collider` (§4.1.6) clearing
  the contact-response bit. The pair still goes through narrowphase
  (so overlaps are detected) but the solver applies no impulse.
- **`TriggerEnter` / `TriggerStay` / `TriggerExit`** — ECS event
  components written into per-pair buffers by the same listener
  adapter that emits contact events, with the response bit cleared.

**Identity & lifetime.** Same as contact events (§4.1.8). The
`Trigger` marker lives on the `Collider`'s entity for the entity's
lifetime.

**Public-boundary invariants.**

1. **Trigger pairs never receive solver impulse.** A `Trigger`-marked
   collider plus any other collider in a layer-pair flagged
   `trigger-only` in `PhysicsConfig` produces lifecycle events but
   zero contact impulse. Mutating "is this a trigger?" mid-frame is
   forbidden (§4.1.6 invariant 4).
2. **Same-frame delivery.** `TriggerEnter` / `TriggerStay` /
   `TriggerExit` follow the same substep-exit drain rule as contact
   events (§4.1.8).
3. **Lifecycle is monotone.** `Enter → Stay* → Exit`, same shape as
   contact pairs.

#### 4.1.10 `PhysicsQueries` / `QueryFilter` / `QueryHit` — synchronous spatial query surface (entity + value object + value object)

**Reason to change:** what spatial questions plugins ask of physics
and how they cross the ABI. Distinct from the broadphase that backs
them (§4.1.11).

**Composition.**

- **`PhysicsQueries`** — ECS resource (one per `PhysicsWorld`)
  exposing the synchronous query surface: `ray_cast`,
  `shape_cast` (oriented), `overlap`, `closest_point`. Backed by
  Jolt's broadphase via `JoltMiddleman` (§4.1.13) — the very same
  broadphase phase 3 stepping uses (§3.2 collapse #3).
- **`QueryFilter`** — value object combining `CollisionLayer` mask,
  ECS-component-presence requirements, and an optional callback
  predicate; passed by value to every query call.
- **`QueryHit`** — plain-data result row carrying entity,
  `BodyId`, world-space hit point, normal, distance, hit
  `CollisionLayer`, and `PhysicsMaterial` reference. Returned by
  value (or as a span); never references Jolt internals.

**Identity & lifetime.** `PhysicsQueries` lives for the lifetime of
its `PhysicsWorld`. A `QueryFilter` is constructed and consumed
at the call site. `QueryHit` rows are owned by the caller's
buffer (caller-supplied span for batched queries).

**Public-boundary invariants.**

1. **Shared broadphase.** Queries hit the same Jolt broadphase that
   phase 3 stepping uses; there is no second spatial structure
   (§3.2 collapse #3). Results reflect the post-phase-3 body state
   of the current frame for queries called in phases 5+, and the
   pre-phase-3 state for queries called in phase 1 (the only legal
   pre-step query window).
2. **Plain-data results.** `QueryHit` carries no Jolt-internal
   pointer; cross-ABI traffic is by value (§3.2 collapse #1).
3. **No physics broadphase outside this surface.** Render owns its
   own HZB / cull (`render`'s spec); navigation will own its own
   nav BVH when it lands. Cross-domain "shared spatial index" is
   refused (PHILOSOPHY anti-pattern; §3.2 collapse #3).
4. **Synchronous semantics.** Queries return inside the calling
   frame; there is no async / streaming query in MVP. A query
   issued during phase 3 is rejected with
   `physics::Error::QueryDuringStep` — phase 3 is single-owner.
5. **Filter is pure.** `QueryFilter`'s callback is read-only over
   ECS state; mutating ECS during a filter callback is undefined
   and a debug-build assertion fires.

#### 4.1.11 `BroadphaseLayer` — coarse Jolt broadphase bucket (value object)

**Reason to change:** how `CollisionLayer`s are coarsened for fast
static-vs-dynamic culling. Distinct from the per-body layer
(§4.1.5) and from the layer-pair interaction matrix (§4.1.2).

**Composition.** A small enum (typically `NonMoving` / `Moving` /
`Trigger` for MVP — exact set frozen by `PhysicsConfig`) plus the
mapping table `CollisionLayer → BroadphaseLayer` carried in
`PhysicsConfig`. The mapping is consumed by Jolt's broadphase at
world init.

**Identity & lifetime.** Compile-time-finite; lives for the
world's lifetime. Membership of a body in a broadphase layer is
fixed by its `Collider`'s `CollisionLayer` plus the config mapping;
it does not change at runtime.

**Public-boundary invariants.**

1. **Mapping is total and frozen.** Every `CollisionLayer` has a
   `BroadphaseLayer`; the mapping is captured in `PhysicsConfig`
   at world init and is immutable thereafter (§4.1.2 invariant 1).
2. **Broadphase membership follows collision layer.** A body's
   broadphase bucket is derived from its `Collider`'s
   `CollisionLayer` via the config mapping; no second membership
   path exists.

#### 4.1.12 `PhysicsSnapshot` — Fory-serialised determinism unit (value object)

**Reason to change:** what counts as the persisted physics state
(replay, golden trace, post-MVP rollback). One schema, one writer
(§3.2 collapse #9).

**Composition.** A Fory-serialised dump (schema authored by
physics, codegen by `data` per `reviews/decisions/fory-codegen.md`)
keyed by `BodyId` (§4.1.5b), carrying for each body: `MotionType`,
position, orientation, linear + angular velocity, sleep frame
counter, accumulated impulses on every joint that touches it, the
`Accumulator` carry, the active `ShapeBlob` content hashes
(reference, not bytes), and the `PhysicsConfig` content hash. The
schema is the byte-equality unit for cross-host determinism gates.

**Identity & lifetime.** Produced on demand by
`PhysicsWorld::snapshot()`; consumed by the determinism gate
(byte-compare across hosts), by golden-trace replay, and by the
post-MVP rollback path. Survives across worlds and across
hot-reload.

**Public-boundary invariants.**

1. **One schema for all "persisted physics state" needs.** Replay,
   golden-trace, post-MVP rollback all write into and read out of
   the same `PhysicsSnapshot` schema (§3.2 collapse #9).
2. **`BodyId`-keyed.** Stable across hosts and reloads (§4.1.5
   invariant 1); consumers may re-bind without rewriting payload.
3. **Byte-equal across hosts.** Two `PhysicsSnapshot`s captured at
   the same logical tick on two different hosts running with the
   same `PhysicsConfig` content hash are byte-identical
   (PHILOSOPHY §7, R-4.1.NF3). Field iteration order is fixed by
   the Fory schema, not by host hash-table order.
4. **References, not bytes.** Snapshots reference `ShapeBlob`s and
   `PhysicsMaterial`s by content hash; the bytes themselves live
   in the asset bundle and are loaded by `data` / `content` (§3.3).

#### 4.1.13 `JoltMiddleman` — ABI-gated Jolt-derived type carrier (entity)

**Reason to change:** the Jolt version glibre links against (§3.2
collapse #1). One seam, one ABI hash.

**Composition.** A middleman dylib (per PHILOSOPHY §9) carrying the
Jolt-derived ABI types both `physics` and the engine link against:
`BodyId`, `ConstraintRef`, opaque `Shape*` token, `ContactManifold`
plain-data layout, broadphase layer enum, `Step` entry point. The
middleman is the only place Jolt headers are visible across the
plugin ABI; its content hash gates plugin load.

**Identity & lifetime.** One per process, loaded at engine init,
unloaded at engine teardown. Survives `physics` plugin hot-reload
when its hash matches; reload is **refused** on hash mismatch with
`core::Error::PluginAbiHashMismatch` per
`reviews/decisions/error-model.md` (§3.2 collapse #1, PHILOSOPHY §9).

**Public-boundary invariants.**

1. **Single ABI seam for Jolt.** No physics-internal type derived
   from Jolt headers crosses the plugin boundary except via
   `JoltMiddleman`. Direct Jolt header inclusion outside
   `physics` and `JoltMiddleman` is a build error.
2. **Hash-gated load.** Refuse on hash mismatch; the previously
   loaded `physics` plugin keeps running (PHILOSOPHY §9). The
   refusal is the **only** legal failure path for hot-reload's
   ABI step.
3. **Exception ingress wrapper.** Jolt is exception-tolerant
   internally; `JoltMiddleman` is the lone wrapper compiled with
   `-fexceptions` (`reviews/decisions/error-model.md`); thrown
   exceptions translate to `physics::Error` arms before crossing
   the ABI.

#### 4.1.14 `Sleeping` / `Island` — rest-state markers (value object + value object)

**Reason to change:** how rest-state is detected and exposed for
diagnostics. Distinct from the dynamics state (§4.1.5).

**Composition.**

- **`Sleeping`** — marker component added by physics to a
  `RigidBody`'s entity when the body's island has been below the
  `PhysicsConfig` linear + angular thresholds for the configured
  frame count. Removed on external force, external torque, new
  contact, or kinematic transform override. The marker is set /
  cleared at substep exit (§4.1.4 invariant 2).
- **`Island`** — Jolt-internal connected component of bodies
  coupled by contacts or constraints. Exposed read-only via
  `RigidBody::island_id() -> u32` for diagnostics (profiler
  overlay, debug draw); never written by ECS code.

**Identity & lifetime.** `Sleeping` lives on the body's entity
between sleep-detect and wake. `Island` membership is rebuilt by
Jolt every substep; the exposed id is valid only for the current
frame (read by phases 5+ inside the same frame).

**Public-boundary invariants.**

1. **Sleep wake is total.** Any `ExternalForce` / `ExternalTorque`
   write, any incoming contact pair, or any kinematic transform
   override on a sleeping body removes the `Sleeping` marker by
   end of the next substep. No body remains sleeping while it has
   nonzero pending solver inputs.
2. **`Island` is read-only across the seam.** ECS code may read
   `island_id` for grouping / debug draw but may not write to it
   or rely on it across substep boundaries.
3. **Threshold bounded by config.** Linear and angular sleep
   thresholds and the frame count are `PhysicsConfig` fields
   (§3.2 collapse #6); per-body overrides are rejected.

#### 4.1.15 `CCD` flag — continuous-collision body bit (value object)

**Reason to change:** which bodies opt into swept-volume narrowphase
to prevent tunneling for fast movers. Distinct from the global CCD
enable on `PhysicsConfig` (§4.1.2).

**Composition.** A boolean field on `RigidBody` (§4.1.5) combined
with the global CCD enable on `PhysicsConfig`; both must be true for
Jolt to take the swept path for that body. Swept narrowphase is
opt-in per body to avoid budget blow-up on slow movers (§3.2
collapse #6 — per-body knob lives on the body).

**Public-boundary invariants.**

1. **Per-body opt-in.** A `RigidBody.ccd = true` body uses Jolt's
   swept narrowphase only when `PhysicsConfig.ccd_enabled` is also
   true; either bit false → discrete narrowphase. No global
   "always-on" CCD.
2. **No runtime swap of swept/discrete inside a substep.** The
   choice is made per body at substep entry and is stable across
   the substep; mid-substep mutation is refused.

### 4.2 Cross-aggregate invariants

Invariants that span more than one aggregate and must hold at every
public boundary at the seams between them:

1. **Phase-3 ownership is total.** Every aggregate above is mutated
   only inside frame phase 3 of the owning `World`. `RigidBody`,
   `Collider`, `Joint`, `ExternalForce`, `ExternalTorque`,
   `Velocity`, `AngularVelocity`, `Sleeping`, contact events,
   trigger events — all are written by phases that physics owns
   (phase 3) or by phases physics permits to feed it (phase 2 for
   intents, phase 1 for kinematic input). No phase ≠ 3 calls into
   Jolt's `Step`. (`reviews/decisions/frame-phases.md`,
   §4.1.1 invariant 1, §4.1.3 invariant 1.)
2. **ECS↔Jolt mirror is one-way per substep.** ECS-side writes
   commit at substep entry; Jolt-side writes commit at substep
   exit. There is no mid-substep cross-traffic. The two commit
   barriers are the only legal transit points (§4.1.1 invariant 4,
   §4.1.4 invariants 2 + 3, §3.2 collapse #5).
3. **Determinism by construction.** A frame run twice with byte-equal
   `PhysicsConfig`, byte-equal body / collider / joint set, byte-equal
   `ExternalForce` / `ExternalTorque` / kinematic overrides, and
   byte-equal `Accumulator` carry produces a byte-equal
   `PhysicsSnapshot` on every supported host (PHILOSOPHY §7,
   R-4.1.NF3). Field iteration order is fixed; allocator addresses
   never leak into the snapshot; no platform-tier branch runs in the
   stepping math (PHILOSOPHY §6).
4. **Accumulator never falls behind by more than four substeps.**
   The per-frame substep loop is hard-capped at four; remainder
   above that is dropped (carry reset to zero) and a warning is
   logged. The simulation's logical clock may diverge from
   wall-clock under a stall, but never by more than `4 * dt` of
   buffered work, and phase 3 never busy-waits (§4.1.3
   invariant 2).
5. **`BodyId` is stable across reload.** A given ECS entity that
   carries a `RigidBody` resolves to the same `BodyId` after a
   physics-plugin hot-reload swap (when the `JoltMiddleman` ABI
   hash matches). `PhysicsSnapshot`s persisted before reload re-bind
   to the post-reload world without payload rewrite (§4.1.5
   invariant 1, §4.1.12 invariant 2, §3.2 collapse #9).
6. **`ShapeBlob`s are shared by content hash.** A given content hash
   maps to one row of the world's shape table for the lifetime of
   the world; every `Collider` referencing that hash resolves to the
   same `ShapeHandle` (§4.1.6 invariant 1, §3.2 collapse #4).
7. **Single Jolt seam.** No Jolt-derived type crosses the plugin
   ABI except through `JoltMiddleman`; load is refused on ABI hash
   mismatch (PHILOSOPHY §9, §4.1.13 invariants 1 + 2). Render does
   not read physics broadphase; navigation will own its own; no
   second physics broadphase exists (§3.2 collapse #3).
8. **Same-frame event delivery.** `CollisionStarted` /
   `CollisionPersisted` / `CollisionEnded` / `TriggerEnter` /
   `TriggerStay` / `TriggerExit` / `JointBroken` events emitted at
   substep exit are visible to phases 5+ inside the same frame and
   reclaimed at end of phase 8; none survive into the next frame's
   read window (§4.1.8 invariants 1 + 2, §4.1.9 invariant 2,
   §4.1.7 invariant 4, R-4.2.NF3, §3.2 collapse #5).
9. **Spatial-query results reflect declared phase.** A query
   issued in phases 5+ returns post-phase-3 state of the current
   frame; a query issued in phase 1 returns pre-phase-3 state
   (the previous frame's terminal state); a query issued in phase
   3 is rejected with `physics::Error::QueryDuringStep`
   (§4.1.10 invariants 1 + 4).
10. **Per-context error model honoured.** Every aggregate's public
    fallible operation returns `glibre::Result<T, glibre::Error>`
    per `reviews/decisions/error-model.md`; physics's enum lives in
    the `physics::Error` arm cited there and is the only physics-
    internal error surface. No exception leaves a public boundary
    (§4.1.1 invariant 3, §4.1.13 invariant 3).

## 5. Public Interface

The header stub below is the §5 deliverable: every symbol that crosses
the physics plugin's public boundary, declared in one C++23 header and
verified via `clang++ -std=c++23 -fsyntax-only -Wall -Wextra -Wpedantic`.
Bodies live inside the physics dylib; this header is the contract every
caller (core, gameplay/logic, editor, downstream plugins) compiles
against. Cross-context invariants enforced here:

- Every fallible operation returns `glibre::Result<T>` per
  `reviews/decisions/error-model.md`. The physics-internal `Error` enum
  is the closed sum cited in §10 below; it is rolled into
  `glibre::Error`'s variant in `core` (PHILOSOPHY §1, §6).
- Aggregates listed in §4 (`PhysicsWorld`, `Accumulator`, `LayerFilter`,
  `PhysicsQueries`, `PhysicsSnapshot`, `JoltMiddleman`) are
  forward-declared classes whose layout is owned inside the plugin.
  Callers manipulate them only through the methods exposed below.
- Resource identifiers (`BodyId`, `ShapeHandle`, `JointId`, `MaterialId`,
  `CollisionLayer`) are 32-bit `Handle<Tag>` values with no payload
  pointers and a stable bit layout; identity survives a hot-reload swap
  whenever the `JoltMiddleman` ABI hash matches (PHILOSOPHY §8 + §9,
  §4.1.5 invariant 1, §4.1.13 invariants 1+2).
- The `Joint` family is a sealed sum (`JointKind::Point` / `Hinge` /
  `Slider` / `Cone` / `Distance` / `SwingTwist`) mirroring Jolt's joint
  primitive set. Adding a kind is an ABI bump: it forces a new
  `JointKind` enumerator, a new `physics::Error::JointKindUnsupported`
  rejection path is removed, and the `JoltMiddleman` ABI hash bumps
  (§4.1.7 invariant 1, §3.2 collapse #1).
- The Jolt-derived ABI is **gated by the `JoltMiddleman` seam**: this
  header pulls in zero Jolt headers, and a `#error` defends the
  invariant when downstream code accidentally includes Jolt before this
  file (§4.1.13 invariant 1).
- ECS components (`RigidBody`, `Collider`, `Trigger`, `Velocity`,
  `AngularVelocity`, `ExternalForce`, `ExternalTorque`, `Sleeping`,
  `JointEndpoints`, `JointLimits`, `JointMotor`,
  `JointBreakThreshold`) are POD-like aggregates. Storage is the ECS
  archetype that holds them; identity is the owning `ecs::Entity`.
  Physics never reaches across into ECS storage from outside the
  substep-boundary mirror (§4.1.4 invariants 2+3, §4.2 invariant 2).
- The query API (`PhysicsQueries`) returns spans of plain-data
  `QueryHit` rows into caller-supplied buffers, never references Jolt
  internals across the ABI, and is rejected with
  `physics::Error::QueryDuringStep` when invoked while phase 3 is
  in flight (§4.1.10 invariants 1+2+4, §4.2 invariant 9).

The header declares the events `CollisionStarted` / `CollisionPersisted`
/ `CollisionEnded` / `TriggerEnter` / `TriggerStay` / `TriggerExit` plus
`JointBrokenEvent` and a sealed `ContactEvent` variant. These are the
only physics-emitted events; they are written into ECS event-component
buffers at substep exit and reclaimed at end of phase 8 (§4.1.8
invariants 1+2, §4.1.9 invariant 2, §4.1.7 invariant 4, §4.2
invariant 8).

The Fory-serialised schema lives on `PhysicsSnapshot` (and its
`SnapshotHeader` / `SnapshotBody` / `SnapshotJoint` field rosters). The
schema is authored here and codegenned by `data` per
`reviews/decisions/fory-codegen.md`; physics owns the layout, `data`
owns the codec.

```cpp
// SPDX-License-Identifier: Apache-2.0
// glibre — physics plugin public interface (header-only stub).
//
// This file is the §5 deliverable of `specs/physics/SPEC.md`. It declares
// every symbol crossing the physics plugin's public boundary. The bodies
// live inside the physics dylib; this header is the contract every caller
// (core, gameplay/logic, editor, downstream plugins) compiles against.
//
// Cross-context invariants embedded here:
//   * Every fallible call returns `glibre::Result<T>` per
//     `reviews/decisions/error-model.md`. `-fno-exceptions` is enforced
//     globally; this header obeys.
//   * Aggregates are opaque — `PhysicsWorld`, `PhysicsQueries`,
//     `PhysicsSnapshot`, `LayerFilter`, `JoltMiddleman`, `Accumulator`
//     are forward-declared classes whose layout is owned inside the
//     physics dylib.
//   * The Jolt-derived ABI is gated by the `JoltMiddleman` seam: no Jolt
//     header is reachable from this file (PHILOSOPHY §9, §3.2 collapse
//     #1, §4.1.13 invariants 1+3). Including this header after a Jolt
//     header refuses to compile.
//   * Identifiers (`BodyId`, `ShapeHandle`, `JointId`, …) are
//     trivially-copyable value types with stable bit layout; no payload
//     pointers cross the seam — reload swaps preserve identity
//     (PHILOSOPHY §8).
//   * Joint family is a sealed sum (Point / Hinge / Slider / Cone /
//     Distance / SwingTwist); adding a kind is an ABI bump and a new
//     `physics::Error` variant.
//
// This stub compiles standalone with
// `clang++ -std=c++23 -fsyntax-only -Wall -Wextra -Wpedantic`.

#pragma once

#if defined(GLIBRE_PHYSICS_INCLUDES_JOLT) && !defined(GLIBRE_PHYSICS_INTERNAL)
#error "Jolt headers must not cross the physics plugin ABI; include only "    \
       "<glibre/physics/physics.hpp>. The middleman seam owns Jolt types."
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <variant>

namespace glibre {

// -----------------------------------------------------------------------
// Stand-in declarations from sibling contexts. The real definitions live
// in `core/include/glibre/error.hpp`, `core/include/glibre/math.hpp`,
// `core/include/glibre/ecs.hpp`, etc.; this header forward-declares them
// so the stub compiles in isolation. The implementation .cpp files
// include the real headers, not these stubs.
// -----------------------------------------------------------------------

#if !defined(GLIBRE_HAVE_CORE_ERROR)
namespace core {
enum class Error : std::uint16_t {
    PluginAbiHashMismatch,
    PluginInitFailed,
    SchemaMigrationFailed,
    HotReloadRefused,
    FramePhaseMisordered,
    OutOfBudget,
};
}  // namespace core

namespace physics { enum class Error : std::uint16_t; }  // declared below.

struct ErrorContext {
    std::string_view file;
    int              line  = 0;
    std::string_view detail;
};

class Error {
public:
    using Variant = std::variant<core::Error /*, physics::Error inserted in core */>;

    template <class E>
    constexpr Error(E e, ErrorContext ctx = {}) noexcept
        : variant_{e}, ctx_{ctx} {}

    constexpr const Variant&      code()  const noexcept { return variant_; }
    constexpr const ErrorContext& where() const noexcept { return ctx_; }

private:
    Variant      variant_;
    ErrorContext ctx_;
};

template <class T>
using Result = std::expected<T, Error>;
#endif  // GLIBRE_HAVE_CORE_ERROR

#if !defined(GLIBRE_HAVE_CORE_MATH)
struct Vec3 { float x = 0.0f, y = 0.0f, z = 0.0f; };
struct Quat { float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f; };
struct Mat4 { std::array<float, 16> m{}; };
#endif  // GLIBRE_HAVE_CORE_MATH

#if !defined(GLIBRE_HAVE_CORE_ECS)
namespace ecs {
struct Entity {
    std::uint64_t bits = 0u;
    [[nodiscard]] friend constexpr bool operator==(Entity, Entity) noexcept = default;
};
class World;  // archetype storage; declared in core.
}  // namespace ecs
#endif  // GLIBRE_HAVE_CORE_ECS

namespace physics {

// -----------------------------------------------------------------------
// physics::Error — closed sum of every physics-internal failure mode.
// Every public physics boundary returns Result<T> over this enum (rolled
// into glibre::Error's variant per reviews/decisions/error-model.md).
// The list is closed: adding a variant is an ABI bump and forces a
// JoltMiddleman hash bump (PHILOSOPHY §9).
// -----------------------------------------------------------------------

enum class Error : std::uint16_t {
    // Config / world lifecycle (§4.1.1, §4.1.2)
    ConfigInvalid,                  // layer matrix incomplete, dt non-positive, …
    WorldNotInitialised,            // operation before PhysicsWorld::create.
    WorldAlreadyInitialised,        // double-init.
    BudgetExceeded,                 // bodies / shapes / contacts past PhysicsConfig caps.

    // Body / collider mirror (§4.1.5, §4.1.6)
    BodyNotFound,                   // BodyId resolves outside its world.
    BodyMotionTypeImmutable,        // changing MotionType not permitted.
    BodyStillReferencedByJoint,     // remove blocked by live Joint endpoint.
    ColliderShapeRequired,          // missing ShapeHandle on add.
    ShapeBlobMalformed,             // invalid Fory bytes / unknown variant.
    ShapeBlobVersionUnsupported,    // schema-version newer than this build.
    ShapeHandleStale,               // refcount-zero handle reused.

    // Joints (§4.1.7)
    JointEndpointInvalid,           // either BodyId resolves nowhere.
    JointDanglingEndpoint,          // body remove attempted with live joint.
    JointKindUnsupported,           // post-MVP joint kind requested.
    JointBroken,                    // attempt to mutate after break.

    // Step / phase (§4.1.3, §4.1.4, §4.2)
    StepCalledOutsidePhase3,        // any caller-driven Step outside phase 3.
    QueryDuringStep,                // PhysicsQueries during an in-flight substep.
    AccumulatorClampExceeded,       // > 4 substeps owed; carry dropped.
    SubstepEcsCommitInverted,       // ECS↔Jolt mid-substep cross-traffic detected.

    // Snapshot / determinism (§4.1.12)
    SnapshotSchemaMismatch,         // PhysicsConfig content hash diverged.
    SnapshotDeserialiseFailed,      // Fory bytes invalid or truncated.
    SnapshotBodyIdUnresolved,       // persisted BodyId has no live ECS entity.

    // Hot-reload + ABI (§4.1.13, §8)
    JoltMiddlemanHashMismatch,      // refused load — previous plugin keeps running.
    JoltMiddlemanUnavailable,       // middleman dylib not loaded at engine init.
    HotReloadStateUnmigratable,     // schema bump invalidates a live world.
};

[[nodiscard]] constexpr std::string_view to_string(Error e) noexcept;

// -----------------------------------------------------------------------
// physics::Warning — non-fatal surface; logged but not returned. Listed
// here so the warning vocabulary is enumerated in one place.
// -----------------------------------------------------------------------

enum class Warning : std::uint16_t {
    AccumulatorClamped,             // §4.1.3 invariant 2 — carry dropped.
    SleepThresholdShadowed,         // per-body override ignored (collapse #6).
    BodyDespawnedDuringStep,        // entity removed mid-frame; deferred to phase 8.
};

// -----------------------------------------------------------------------
// Strong-typed handles. All trivially-copyable, all fixed bit-layout, all
// reload-stable (PHILOSOPHY §8). Equality + hashing are part of the ABI;
// they are how downstream code keys `PhysicsSnapshot` rows (§4.1.12).
// -----------------------------------------------------------------------

namespace tags {
struct body              {};
struct shape             {};
struct joint             {};
struct material          {};
struct collision_layer   {};
struct broadphase_layer  {};
}  // namespace tags

template <class Tag, class Repr = std::uint32_t>
class Handle {
public:
    using value_type = Repr;

    constexpr Handle() noexcept = default;
    explicit constexpr Handle(Repr v) noexcept : bits_{v} {}

    [[nodiscard]] constexpr Repr raw()   const noexcept { return bits_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return bits_ != Repr{0}; }

    [[nodiscard]] friend constexpr bool operator==(Handle, Handle) noexcept = default;

private:
    Repr bits_ = Repr{0};
};

// Hash specialisation contract — any std::hash<Handle<Tag,Repr>> that
// downstream code instantiates must collapse to hashing `raw()`. The
// real specialisation lives in core; this struct documents the shape.
struct HandleHashContract {
    template <class Tag, class Repr>
    [[nodiscard]] constexpr std::size_t
        operator()(Handle<Tag, Repr> h) const noexcept {
        return static_cast<std::size_t>(h.raw());
    }
};

using BodyId           = Handle<tags::body,            std::uint32_t>;
using ShapeHandle      = Handle<tags::shape,           std::uint32_t>;
using JointId          = Handle<tags::joint,           std::uint32_t>;
using MaterialId       = Handle<tags::material,        std::uint32_t>;
using CollisionLayer   = Handle<tags::collision_layer, std::uint32_t>;

// -----------------------------------------------------------------------
// BroadphaseLayer — §4.1.11. Closed enum; the concrete bucket set is
// fixed by `PhysicsConfig` at world init and never mutates.
// -----------------------------------------------------------------------

enum class BroadphaseLayer : std::uint8_t {
    NonMoving = 0,
    Moving    = 1,
    Trigger   = 2,
    // Closed list. Adding a bucket is an ABI bump (collapse #6).
};

// -----------------------------------------------------------------------
// MotionType — §4.1.5. Pinned at body creation; mutating is a recreate
// (§4.1.5 invariant 3).
// -----------------------------------------------------------------------

enum class MotionType : std::uint8_t {
    Static    = 0,  // immovable; ignores ExternalForce / ExternalTorque.
    Kinematic = 1,  // script-driven; immune to solver impulse.
    Dynamic   = 2,  // solver-driven.
};

// -----------------------------------------------------------------------
// Joint kind — sealed sum mirroring Jolt's joint family (§4.1.7).
// Adding a kind is an ABI bump (PHILOSOPHY §9).
// -----------------------------------------------------------------------

enum class JointKind : std::uint8_t {
    Point      = 0,  // 3-DoF position-fixed (Jolt PointConstraint).
    Hinge      = 1,  // 1-DoF rotational (Jolt HingeConstraint).
    Slider     = 2,  // 1-DoF prismatic (Jolt SliderConstraint).
    Cone       = 3,  // limited swing only (Jolt ConeConstraint).
    Distance   = 4,  // anchored at distance (Jolt DistanceConstraint).
    SwingTwist = 5,  // swing-cone + twist (Jolt SwingTwistConstraint).
};

// -----------------------------------------------------------------------
// PhysicsConfig — §4.1.2. Init-time-immutable; one per PhysicsWorld.
// -----------------------------------------------------------------------

struct SleepThresholds {
    float linear_speed   = 0.05f;   // m / s.
    float angular_speed  = 0.05f;   // rad / s.
    std::uint16_t frame_count = 30; // frames below thresholds before sleep.
};

struct WorldBudgets {
    std::uint32_t max_bodies      = 0u;
    std::uint32_t max_shapes      = 0u;
    std::uint32_t max_constraints = 0u;
    std::uint32_t max_contacts    = 0u;
};

// Per-pair interaction policy.
enum class LayerInteraction : std::uint8_t {
    Ignore      = 0,
    Collide     = 1,
    TriggerOnly = 2,
};

// LayerFilter — §2 ubiquitous language; mapping every CollisionLayer to
// a BroadphaseLayer plus the layer-pair interaction matrix. The opaque
// body lives in the physics dylib; callers populate via the methods.
class LayerFilter {
public:
    [[nodiscard]] static Result<std::unique_ptr<LayerFilter>>
        create(std::uint16_t layer_count) noexcept;

    [[nodiscard]] Result<void>
        set_broadphase(CollisionLayer, BroadphaseLayer) noexcept;

    [[nodiscard]] Result<void>
        set_pair(CollisionLayer a, CollisionLayer b, LayerInteraction) noexcept;

    [[nodiscard]] Result<void>
        validate() const noexcept;  // §4.1.2 invariant 4 — totality check.

    [[nodiscard]] std::uint16_t layer_count() const noexcept;

    ~LayerFilter();
    LayerFilter(const LayerFilter&)            = delete;
    LayerFilter& operator=(const LayerFilter&) = delete;

protected:
    LayerFilter() noexcept;
};

struct PhysicsConfig {
    Vec3                          gravity              = {0.0f, -9.81f, 0.0f};
    float                         fixed_dt             = 1.0f / 60.0f;
    std::uint8_t                  max_substeps         = 4u;   // §4.1.3 inv 2.
    std::uint8_t                  velocity_iters       = 10u;
    std::uint8_t                  position_iters       = 2u;
    float                         warm_start_factor    = 0.85f;
    SleepThresholds               sleep                = {};
    bool                          ccd_enabled          = true;
    std::uint64_t                 rng_seed             = 0u;
    WorldBudgets                  budgets              = {};
    std::shared_ptr<LayerFilter>  layer_filter         = {};
    // Content-hash of the canonicalised config bytes; written by `data` at
    // load and matched against persisted PhysicsSnapshot at restore time.
    std::uint64_t                 content_hash         = 0u;
};

// -----------------------------------------------------------------------
// ShapeBlob — §4.1.6. Cooked, content-hashed bytes. The handle surface
// is the only thing that crosses the seam; the bytes are owned by `data`.
// -----------------------------------------------------------------------

struct ShapeBlob {
    std::uint64_t              content_hash   = 0u;  // BLAKE3 of the cooked bytes.
    std::uint16_t              schema_version = 0u;
    std::span<const std::byte> bytes          = {};
};

// -----------------------------------------------------------------------
// PhysicsMaterial — §2 ubiquitous language. Asset-side; physics consumes
// MaterialId references via Collider.
// -----------------------------------------------------------------------

enum class CombineMode : std::uint8_t {
    Average  = 0,
    Min      = 1,
    Max      = 2,
    Multiply = 3,
};

struct PhysicsMaterial {
    float         friction               = 0.6f;
    float         restitution            = 0.0f;
    float         density                = 1000.0f;
    CombineMode   friction_combine       = CombineMode::Average;
    CombineMode   restitution_combine    = CombineMode::Max;
    std::uint64_t content_hash           = 0u;
};

// -----------------------------------------------------------------------
// RigidBody / Collider / Trigger — §4.1.5, §4.1.6, §4.1.9. ECS components
// — POD-like aggregates with `BodyId` / `ShapeHandle` payloads. Stored
// in archetype columns; identity is the owning Entity.
// -----------------------------------------------------------------------

struct RigidBody {
    BodyId       body_id          = {};
    MotionType   motion_type      = MotionType::Dynamic;
    float        mass             = 1.0f;
    Vec3         inertia_diagonal = {1.0f, 1.0f, 1.0f};
    bool         auto_inertia     = true;
    float        linear_damping   = 0.05f;
    float        angular_damping  = 0.05f;
    bool         ccd              = false;
    bool         sleeping         = false;
};

struct Collider {
    ShapeHandle    shape            = {};
    Vec3           offset_position  = {};
    Quat           offset_rotation  = {};
    CollisionLayer layer            = {};
    MaterialId     material         = {};
    bool           is_trigger       = false;
    float          density_override = 0.0f;  // 0 ⇒ use material density.
};

struct Trigger {};  // Marker tag — §4.1.9.

struct Velocity        { Vec3 v      = {}; };
struct AngularVelocity { Vec3 w      = {}; };
struct ExternalForce   { Vec3 force  = {}; };
struct ExternalTorque  { Vec3 torque = {}; };
struct Sleeping        {};  // marker; managed by physics (§4.1.14).

// -----------------------------------------------------------------------
// Joint — §4.1.7. Joint *is* an ECS entity; the `JointEndpoints` +
// optional companion components describe the constraint.
// -----------------------------------------------------------------------

struct JointFrame {
    Vec3 anchor_position = {};
    Quat anchor_rotation = {};
};

struct JointEndpoints {
    JointId    joint_id = {};
    JointKind  kind     = JointKind::Point;
    BodyId     body_a   = {};
    BodyId     body_b   = {};
    JointFrame frame_a  = {};
    JointFrame frame_b  = {};
};

struct JointLimits {
    // Axis-agnostic; semantics depend on JointKind.
    float lower      = 0.0f;
    float upper      = 0.0f;
    float swing_y    = 0.0f;  // SwingTwist / Cone — radians.
    float swing_z    = 0.0f;  // SwingTwist — radians.
    float twist_low  = 0.0f;  // SwingTwist — radians.
    float twist_high = 0.0f;  // SwingTwist — radians.
};

struct JointMotor {
    bool  enabled       = false;
    float target_value  = 0.0f;
    float max_force     = 0.0f;  // N or N·m depending on kind.
    float damping       = 0.0f;
};

struct JointBreakThreshold {
    float max_force  = 0.0f;
    float max_torque = 0.0f;
};

// -----------------------------------------------------------------------
// ContactManifold + lifecycle events — §4.1.8.
// All payloads are plain-data; no Jolt pointer crosses the seam.
// -----------------------------------------------------------------------

struct ContactPoint {
    Vec3  position_world   = {};
    float separation       = 0.0f;
    float normal_impulse   = 0.0f;
    float friction_impulse = 0.0f;
};

struct ContactManifold {
    BodyId                       body_a       = {};
    BodyId                       body_b       = {};
    Vec3                         normal_world = {};
    MaterialId                   material_a   = {};
    MaterialId                   material_b   = {};
    std::array<ContactPoint, 4>  points       = {};
    std::uint8_t                 point_count  = 0u;
};

// Substep-boundary lifecycle events — written to ECS event-component
// buffers at substep exit (§4.1.8 invariant 2).
struct CollisionStarted   { ContactManifold manifold; };
struct CollisionPersisted { ContactManifold manifold; };
struct CollisionEnded     { BodyId body_a; BodyId body_b; };

// Trigger lifecycle — §4.1.9. Same substep-exit drain rule; no impulse.
struct TriggerEnter { BodyId body_a; BodyId body_b; };
struct TriggerStay  { BodyId body_a; BodyId body_b; };
struct TriggerExit  { BodyId body_a; BodyId body_b; };

// Joint break event — §4.1.7 invariant 4. Physics's only joint event.
struct JointBrokenEvent {
    JointId   joint          = {};
    JointKind kind           = JointKind::Point;
    float     applied_force  = 0.0f;
    float     applied_torque = 0.0f;
};

// Generic ContactEvent — sealed sum of the above; downstream code may
// match on the variant for unified handling.
using ContactEvent = std::variant<
    CollisionStarted,
    CollisionPersisted,
    CollisionEnded,
    TriggerEnter,
    TriggerStay,
    TriggerExit>;

// -----------------------------------------------------------------------
// Spatial query surface — §4.1.10.
// All methods are synchronous, run outside phase 3, and return spans of
// plain-data QueryHit rows into a caller-supplied buffer.
// -----------------------------------------------------------------------

struct QueryHit {
    ecs::Entity     entity         = {};
    BodyId          body           = {};
    Vec3            point_world    = {};
    Vec3            normal_world   = {};
    float           distance       = 0.0f;
    CollisionLayer  layer          = {};
    MaterialId      material       = {};
    std::uint32_t   sub_shape_id   = 0u;  // for compound shapes.
};

// QueryFilter callback — pure (read-only over ECS, §4.1.10 inv 5).
// Returns true to keep the candidate, false to reject.
using QueryPredicateFn = std::function<bool(const QueryHit&) /* noexcept */>;

struct QueryFilter {
    std::uint64_t                layer_mask         = ~std::uint64_t{0};
    bool                         hit_triggers       = false;
    bool                         hit_static         = true;
    bool                         hit_kinematic      = true;
    bool                         hit_dynamic        = true;
    QueryPredicateFn             predicate          = {};
    std::span<const ecs::Entity> ignore_entities    = {};
};

struct ShapeCastDesc {
    ShapeHandle shape       = {};
    Vec3        origin      = {};
    Quat        orientation = {};
    Vec3        sweep       = {};   // direction × distance.
};

class PhysicsQueries {
public:
    // Ray cast — directional. The caller scales `direction` by max
    // distance. Returned hits are written into `out` (caller-owned)
    // up to its capacity; `Result` carries the populated subspan.
    [[nodiscard]] Result<std::span<QueryHit>>
        raycast(Vec3 origin,
                Vec3 direction,
                const QueryFilter&,
                std::span<QueryHit> out) const noexcept;

    // Sphere overlap — non-sweeping; returns every body whose AABB +
    // narrowphase intersects the sphere.
    [[nodiscard]] Result<std::span<QueryHit>>
        sphere_overlap(Vec3 centre,
                       float radius,
                       const QueryFilter&,
                       std::span<QueryHit> out) const noexcept;

    // Capsule sweep — oriented; the QueryHit::distance is the sweep-T
    // at first contact in [0, 1].
    [[nodiscard]] Result<std::span<QueryHit>>
        capsule_sweep(Vec3 centre_a,
                      Vec3 centre_b,
                      float radius,
                      Vec3 sweep,
                      const QueryFilter&,
                      std::span<QueryHit> out) const noexcept;

    // Generic shape cast — used by post-MVP character controllers /
    // vehicle wheels; `desc.sweep` controls direction and length.
    [[nodiscard]] Result<std::span<QueryHit>>
        shape_cast(const ShapeCastDesc&,
                   const QueryFilter&,
                   std::span<QueryHit> out) const noexcept;

    // Closest-point — single hit; `Result` carries an empty span if the
    // filter rejects every candidate.
    [[nodiscard]] Result<std::span<QueryHit>>
        closest_point(Vec3 point,
                      float max_distance,
                      const QueryFilter&,
                      std::span<QueryHit> out) const noexcept;

    ~PhysicsQueries();
    PhysicsQueries(const PhysicsQueries&)            = delete;
    PhysicsQueries& operator=(const PhysicsQueries&) = delete;

protected:
    PhysicsQueries() noexcept;
};

// -----------------------------------------------------------------------
// Accumulator — §4.1.3. Owns the substep draining loop; advances the
// simulation clock once per phase-3 entry. Opaque; no caller may step
// independently of `PhysicsWorld::advance`.
// -----------------------------------------------------------------------

struct AdvanceReport {
    std::uint8_t  substeps_run     = 0u;
    std::uint8_t  substeps_dropped = 0u;  // > 0 ⇒ AccumulatorClamped warning.
    float         carry_seconds    = 0.0f;
};

class Accumulator {
public:
    [[nodiscard]] Result<AdvanceReport> advance(float real_dt) noexcept;
    [[nodiscard]] float                  carry()      const noexcept;
    [[nodiscard]] std::uint64_t          tick_count() const noexcept;

    ~Accumulator();
    Accumulator(const Accumulator&)            = delete;
    Accumulator& operator=(const Accumulator&) = delete;

protected:
    Accumulator() noexcept;
};

// -----------------------------------------------------------------------
// PhysicsSnapshot — §4.1.12. Fory-serialised determinism unit. Schema
// authored here (the field roster); codegen owned by `data`.
// -----------------------------------------------------------------------

struct SnapshotHeader {
    std::uint64_t schema_version       = 1u;
    std::uint64_t physics_config_hash  = 0u;
    std::uint64_t world_tick           = 0u;
    float         accumulator_carry    = 0.0f;
    std::uint64_t middleman_abi_hash   = 0u;
};

struct SnapshotBody {
    BodyId        body_id          = {};
    MotionType    motion_type      = MotionType::Static;
    Vec3          position         = {};
    Quat          rotation         = {};
    Vec3          linear_velocity  = {};
    Vec3          angular_velocity = {};
    std::uint16_t sleep_frames     = 0u;
    bool          sleeping         = false;
    std::uint64_t shape_blob_hash  = 0u;
};

struct SnapshotJoint {
    JointId       joint_id                     = {};
    JointKind     kind                         = JointKind::Point;
    BodyId        body_a                       = {};
    BodyId        body_b                       = {};
    float         accumulated_normal_impulse   = 0.0f;
    float         accumulated_friction_impulse = 0.0f;
};

class PhysicsSnapshot {
public:
    [[nodiscard]] const SnapshotHeader&             header() const noexcept;
    [[nodiscard]] std::span<const SnapshotBody>     bodies() const noexcept;
    [[nodiscard]] std::span<const SnapshotJoint>    joints() const noexcept;

    // Fory bridge — `data` codegens these. Writer / reader are total:
    // success ⇒ canonical bytes / restored snapshot, failure ⇒ typed
    // error.
    [[nodiscard]] Result<std::span<const std::byte>> to_bytes() const noexcept;

    [[nodiscard]] static Result<std::unique_ptr<PhysicsSnapshot>>
        from_bytes(std::span<const std::byte>) noexcept;

    ~PhysicsSnapshot();
    PhysicsSnapshot(const PhysicsSnapshot&)            = delete;
    PhysicsSnapshot& operator=(const PhysicsSnapshot&) = delete;

protected:
    PhysicsSnapshot() noexcept;
};

// -----------------------------------------------------------------------
// JoltMiddleman — §4.1.13. The single ABI seam through which any
// Jolt-derived type may cross the plugin boundary. The header surface
// here exposes only the hash-gated load contract; concrete Jolt-derived
// payload types live behind `GLIBRE_PHYSICS_INTERNAL`.
// -----------------------------------------------------------------------

struct MiddlemanInfo {
    std::uint64_t abi_hash       = 0u;   // BLAKE3 over the canonical type list.
    std::uint32_t jolt_version   = 0u;
    std::uint16_t schema_version = 0u;
};

class JoltMiddleman {
public:
    [[nodiscard]] static Result<const JoltMiddleman*>
        load_from_engine() noexcept;

    [[nodiscard]] MiddlemanInfo info() const noexcept;

    [[nodiscard]] Result<void>
        require_hash(std::uint64_t expected_abi_hash) const noexcept;

    ~JoltMiddleman()                                = default;
    JoltMiddleman(const JoltMiddleman&)             = delete;
    JoltMiddleman& operator=(const JoltMiddleman&)  = delete;

protected:
    JoltMiddleman() noexcept = default;
};

// -----------------------------------------------------------------------
// PhysicsWorld — §4.1.1 root aggregate. One per ECS World; owns the Jolt
// instance, the shape table, the joint registry, the contact-event
// drain, the Accumulator, and the PhysicsQueries surface.
// -----------------------------------------------------------------------

class PhysicsWorld {
public:
    // Construction — fails fast on ConfigInvalid / ABI hash mismatch /
    // budget overflow. Captures `config` by value (immutable for life).
    [[nodiscard]] static Result<std::unique_ptr<PhysicsWorld>>
        create(ecs::World&, PhysicsConfig config) noexcept;

    // Phase-3 entry point — advances the accumulator and drains owed
    // substeps. Called exactly once per frame by core's phase-3 driver.
    [[nodiscard]] Result<AdvanceReport>
        advance(float real_dt) noexcept;

    // Shape table.
    [[nodiscard]] Result<ShapeHandle>
        intern_shape(const ShapeBlob&) noexcept;
    [[nodiscard]] Result<void>
        release_shape(ShapeHandle) noexcept;

    // Material table.
    [[nodiscard]] Result<MaterialId>
        intern_material(const PhysicsMaterial&) noexcept;

    // Body lifecycle. Insert / remove are deterministic (allocation
    // order = ECS materialisation order, §4.1.5 invariant 1).
    [[nodiscard]] Result<BodyId>
        add_body(ecs::Entity, const RigidBody&, const Collider&) noexcept;
    [[nodiscard]] Result<void>
        remove_body(BodyId) noexcept;

    // Joint lifecycle. Endpoints must resolve before insert; live joint
    // blocks endpoint body removal (§4.1.7 invariant 1).
    [[nodiscard]] Result<JointId>
        add_joint(const JointEndpoints&,
                  const JointLimits*         limits = nullptr,
                  const JointMotor*          motor  = nullptr,
                  const JointBreakThreshold* brk    = nullptr) noexcept;
    [[nodiscard]] Result<void>
        remove_joint(JointId) noexcept;

    // Resolved accessors.
    [[nodiscard]] PhysicsQueries&     queries()      noexcept;
    [[nodiscard]] Accumulator&        accumulator()  noexcept;
    [[nodiscard]] const PhysicsConfig& config()      const noexcept;
    [[nodiscard]] std::uint64_t       world_tick()   const noexcept;

    // Snapshot — produces an opaque PhysicsSnapshot rooted in this
    // world's tick. Restore is total: it resets the world to the
    // captured tick or returns a typed error.
    [[nodiscard]] Result<std::unique_ptr<PhysicsSnapshot>>
        snapshot() const noexcept;
    [[nodiscard]] Result<void>
        restore(const PhysicsSnapshot&) noexcept;

    // Hot-reload — swap the underlying physics dylib. Refused on ABI
    // hash mismatch (§4.1.13 invariant 2). The previously-running
    // instance keeps stepping if refused.
    [[nodiscard]] Result<void>
        on_plugin_reload(const JoltMiddleman&) noexcept;

    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&)            = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

protected:
    PhysicsWorld() noexcept;
};

}  // namespace physics
}  // namespace glibre
```

**Event types.** Physics emits seven event-component types, all written
at substep exit (§4.1.8 invariant 2, §4.1.9 invariant 2, §4.1.7
invariant 4) and reclaimed at end of phase 8 (§4.2 invariant 8):
`CollisionStarted` / `CollisionPersisted` / `CollisionEnded` /
`TriggerEnter` / `TriggerStay` / `TriggerExit` / `JointBrokenEvent`. The
six contact + trigger events compose into the `ContactEvent` sealed-sum
variant for unified consumer code; `JointBrokenEvent` stays separate as
the joint lifecycle is independent of contact lifecycle (§3.2 collapse
#5). Physics emits nothing else; all other "physics-adjacent" events
(severance, fragment spawn, prosthetic re-attachment, ragdoll
activation) are post-MVP plugin work routed away from this context per
§3.3.

**Serialised schemas (Fory).** One Fory-serialised artifact lives at
this layer — `PhysicsSnapshot`, with the `SnapshotHeader` /
`SnapshotBody` / `SnapshotJoint` field rosters declared above (§4.1.12).
The schema is authored by `physics`, codegenned by `data` per
`reviews/decisions/fory-codegen.md`. Iteration order is fixed by the
schema definition (PHILOSOPHY §7), so two snapshots captured at the
same logical tick on different hosts are byte-identical (§4.2 invariant
3, R-4.1.NF3). `PhysicsConfig` and `PhysicsMaterial` are also
Fory-serialised assets, owned at the asset layer by `data` (§3.3); the
field rosters above are the schema authority.

**Error types.** The closed sum `physics::Error` declared above lists
every failure mode at every public physics boundary. It is the §10
authority for failure-mode enumeration; new variants require an ABI
bump and a `JoltMiddleman` hash bump per
`reviews/decisions/error-model.md` and PHILOSOPHY §9. Physics also
exposes a `physics::Warning` enum for non-fatal surfaces that are
logged but not returned (clamped accumulator, body despawn during
step, shadowed sleep override).

## 6. Internal Architecture

Non-binding sketch for implementers. The §4 aggregates and the §5
public header are binding; this section sketches the *how* — the
module split inside the physics dylib, the frame-phase-3 owner that
threads them together, the ECS↔Jolt mirror data flow per substep, the
determinism guards that make the §4.2 invariants compile down to
byte-equal snapshots, and the cross-context handoffs (render BLAS
lifecycle, query span ownership, snapshot bus). Reviewers should
reject deviations only when they violate §4 invariants, the §5
header, or the per-phase ownership locked in
`reviews/decisions/frame-phases.md`.

The split is the §4 aggregate roster lifted directly into directories.
SRP rule (PHILOSOPHY §1): each module has one reason to change — its
owned aggregate's invariants. Cross-module reach-throughs are
forbidden; modules communicate either through the §5 facade or
through the world-internal seams enumerated in §6.2.

### 6.1 Module layout

The physics plugin compiles to a single `physics.dylib` (PHILOSOPHY
§3 — every domain is a plugin). Inside, source is split by SRP — one
directory per "reason to change". Public headers (the §5 deliverable)
live under `physics/include/glibre/physics/`; implementation under
`physics/src/`. The seven sub-modules below own one §4 aggregate
cluster each, plus a tiny `plugin.{hpp,cpp}` entry point for
`glibre_plugin_register` / `glibre_plugin_drain` (per
`reviews/decisions/plugin-abi.md`).

```
physics/
  include/glibre/physics/      # §5 surface (compiles standalone).
    physics.hpp                # The single header from §5.
  src/
    world/                     # Aggregates §4.1.1 + §4.1.2 + §4.1.3.
      physics_world.{hpp,cpp}  # PhysicsWorld root; owns Jolt PhysicsSystem.
      physics_config.{hpp,cpp} # Frozen config view; content_hash compute.
      accumulator.{hpp,cpp}    # Fixed-dt clock; phase-3 entry/exit.
      phase3_driver.{hpp,cpp}  # The phase-3 body — see §6.2.
    shapes/                    # Aggregate §4.1.6 + §4.1.11.
      shape_table.{hpp,cpp}    # Hash-keyed ShapeBlob table; refcount mgmt.
      shape_blob.{hpp,cpp}     # Immutable blob payload; content-hash key.
      broadphase_layer.{hpp,cpp} # Layer mapping table; world-init only.
    bodies/                    # Aggregates §4.1.5 + §4.1.5b + §4.1.14 + §4.1.15.
      rigid_body.{hpp,cpp}     # ECS↔Jolt body mirror.
      body_id_allocator.{hpp,cpp} # Deterministic 32-bit allocator.
      sleeping.{hpp,cpp}       # Sleep marker + island read-back.
      ccd.{hpp,cpp}            # Per-body swept narrowphase opt-in.
    joints/                    # Aggregate §4.1.7.
      joint.{hpp,cpp}          # Joint variant dispatch; constraint build.
      joint_kinds/             # One file per JointKind variant.
        point.cpp              # Point constraint body.
        hinge.cpp              # Hinge constraint body.
        slider.cpp             # Slider constraint body.
        cone.cpp               # Cone constraint body.
        distance.cpp           # Distance constraint body.
        swing_twist.cpp        # SwingTwist constraint body.
      joint_break.{hpp,cpp}    # Break-threshold check + entity despawn.
    queries/                   # Aggregate §4.1.10.
      physics_queries.{hpp,cpp} # Synchronous query surface.
      query_filter.{hpp,cpp}   # CollisionLayer mask + callback predicate.
      query_hit.{hpp,cpp}      # Plain-data result row layout.
    middleman/                 # Aggregate §4.1.13.
      jolt_middleman.{hpp,cpp} # The ONLY TU including <Jolt/...> headers.
      contact_listener.{hpp,cpp} # ContactListener adapter; emits events.
      exception_wrapper.{hpp,cpp} # -fexceptions ingress; translates throws.
    snapshot/                  # Aggregate §4.1.12 + §7.1.4.
      physics_snapshot.{hpp,cpp} # Serialise/deserialise; phase-8 carrier.
      snapshot_restore.{hpp,cpp} # add_body / add_joint replay loop.
    contact/                   # Aggregates §4.1.8 + §4.1.9.
      contact_manifold.{hpp,cpp} # Per-pair manifold layout.
      contact_event.{hpp,cpp}    # CollisionStarted/Persisted/Ended drain.
      trigger_event.{hpp,cpp}    # TriggerEnter/Stay/Exit drain.
    plugin.{hpp,cpp}           # Plugin entry: register / drain / migrate.
```

Module-level rules (build-system enforced):

1. **Single Jolt seam.** `<Jolt/...>` headers may be reached by
   exactly **one** translation unit: `middleman/jolt_middleman.cpp`
   (§4.1.13 invariant 1). A pre-build CMake check rejects any other
   `.cpp` whose preprocessor output contains a Jolt include path.
   The §5 header pulls in zero Jolt headers and sports the
   `#error "Jolt headers must not cross the physics plugin ABI"`
   guard already specified in §5; that guard is the symmetric
   refusal for the consumer side.
2. **Single `-fexceptions` TU.** Per
   `reviews/decisions/error-model.md`, the entire physics dylib
   compiles with `-fno-exceptions` except `middleman/jolt_middleman.cpp`
   and `middleman/exception_wrapper.cpp`, which are compiled with
   `-fexceptions` and translate any `JPH::*` throw into a closed
   `physics::Error` arm before returning across the module boundary
   (§4.1.13 invariant 3). No other physics source is permitted to
   include `<exception>` or use `try` / `throw`.
3. **No cross-module direct includes.** A module's `.cpp` may
   include its own `.hpp`s and the §5 facade only. Cross-module
   reach-throughs (e.g. `bodies/` including `shapes/shape_table.hpp`
   directly) are forbidden; the seam is `world/physics_world.hpp`,
   which owns references to all sibling tables and brokers access.
   This makes `world/` the single dependency hub and matches §4.1.1's
   "owns the broadphase + narrowphase + constraint set + the shape
   table + the joint registry + contact-event drain" composition.
4. **Plugin entry symbols only at `plugin.cpp`.** The four
   `glibre_plugin_*` extern-C entry points required by
   `reviews/decisions/plugin-abi.md` (`glibre_plugin_abi_hash`,
   `glibre_plugin_manifest`, `glibre_plugin_manifest_size`,
   `glibre_plugin_register`) plus `glibre_plugin_drain` (per protocol
   §Step 1) are defined in `plugin.cpp` only. No other `.cpp`
   exports an extern-C symbol; the dylib's symbol-visibility default
   is `hidden` with `__attribute__((visibility("default")))` applied
   only to those five.

### 6.2 Frame integration — phase 3 ownership

Physics owns exactly one phase per `reviews/decisions/frame-phases.md`:
**phase 3 — `physics-fixed`**. The phase body is implemented by
`world/phase3_driver.cpp` and is the single entry point for every
byte of stepping work in the plugin (§4.2 invariant 1).

Phase 3 entry receives the `World&` + `PhysicsWorld&` pair from
`core`'s `FrameLoop` (`specs/core/SPEC.md` §6.5). The driver is a
synchronous body on the game-loop driver thread; it does not yield,
does not poll for hot-reload, and does not call into any sibling
context's APIs (only `core::ecs` storage reads / writes through the
mirror seams below). Steps in order:

1. **Drive the `Accumulator`.** `accumulator.cpp` reads the frame
   `dt` from `core`'s `FrameClock` snapshot and increments the
   carry: `acc += core_dt`. The substep loop runs while
   `acc >= cfg.dt && substeps_done < SUBSTEP_CAP_4` (§4.1.3
   invariant 2). Each iteration calls `step_one(world, substep)`;
   the loop terminates on either condition. A clamp event
   (`AccumulatorClamped` warning, §4.1.3 invariant 2) is logged
   when the cap is hit; the residual carry is reset to zero per the
   bounded-catch-up rule.
2. **Per substep — commit ECS → Jolt (entry barrier).** The driver
   walks the middleman-typed `RigidBody` archetype in `BodyId`
   ascending order (§4.1.5b invariant 1) and, for each body:
   - drains `ExternalForce` + `ExternalTorque` into the Jolt body
     via `JoltMiddleman::add_force(...)` / `add_torque(...)`, then
     zeros the components (§4.1.4 invariant 3),
   - applies kinematic transform overrides into the Jolt body via
     `JoltMiddleman::set_position(...)` / `set_rotation(...)` for
     `MotionType::Kinematic` rows,
   - applies joint motor targets by walking the `Joint` archetype
     in `JointId` ascending order and calling
     `JoltMiddleman::set_motor_target(...)` per active motor.
   This is the ECS-side commit barrier (§4.2 invariant 2 first
   half). It is the only legal point at which ECS state crosses
   into Jolt during phase 3.
3. **Per substep — Jolt step.** The driver calls
   `JoltMiddleman::step(physics_system, cfg.dt, cfg.velocity_iters,
   cfg.position_iters, &temp_alloc, job_system)` exactly once per
   substep (§4.1.4 invariant 1). Jolt runs broadphase, narrowphase,
   constraint solve, contact resolution, and integration in one
   call. The driver does not pre-empt; the call returns when the
   substep's solver work is complete.
4. **Per substep — commit Jolt → ECS (exit barrier).** Symmetric to
   step 2: the driver walks the same `RigidBody` archetype in the
   same `BodyId` ascending order and writes back:
   - `Velocity` + `AngularVelocity` from
     `JoltMiddleman::get_linear_velocity(...)` /
     `get_angular_velocity(...)`,
   - position / rotation into the body's `RigidBody` row (which
     `core`'s phase-5 transform-propagation will lift into the
     `GlobalTransform`),
   - sleep state into the `Sleeping` marker per
     `JoltMiddleman::is_sleeping(...)` (§4.1.14 invariant 1).
   Then the driver drains Jolt's `ContactListener` adapter
   (`middleman/contact_listener.cpp`) into the per-frame ECS event
   buffers — `CollisionStarted` / `CollisionPersisted` /
   `CollisionEnded` / `TriggerEnter` / `TriggerStay` / `TriggerExit`
   — sorted by `(BodyId-low, BodyId-high)` so the order is fixed
   by entity identity, not by Jolt's internal pair-list order
   (§4.1.8 invariant 1, §4.2 invariant 8). `JointBrokenEvent`
   emission and the joint entity's despawn are handled by
   `joints/joint_break.cpp` reading break thresholds against
   accumulated solver impulses (§4.1.7 invariant 4). This is the
   Jolt-side commit barrier (§4.2 invariant 2 second half).
5. **Substep accounting.** `acc -= cfg.dt`; `substeps_done += 1`;
   `world_tick += 1`. The substep loop returns to step 1's
   condition check.

After the substep loop exits, phase 3 returns. The accumulator's
residual carry, the updated `world_tick`, and every middleman-typed
ECS row are now consistent with one logical post-step state (§4.2
invariant 3). Phase 4 (animation) is empty in MVP; phase 5 reads the
post-step transforms; phase 6 culls; phase 7 submits; phase 8 may
swap the physics dylib (§8). No physics work runs outside phase 3
(§4.1.1 invariant 1, §4.2 invariant 1).

### 6.3 ECS↔Jolt mirror — one-way per substep

The mirror seam between core's archetype storage and Jolt's body
table is implemented in `bodies/rigid_body.cpp` and called only by
`world/phase3_driver.cpp`. Two directions, two barriers, one
substep boundary — §4.2 invariant 2 made mechanical.

**ECS → Jolt** (substep entry, before `JoltMiddleman::step`):

```text
For body in RigidBody archetype, sorted ascending by BodyId:
  ExternalForce.value     -> JoltMiddleman::add_force(BodyId, Vec3)
  ExternalTorque.value    -> JoltMiddleman::add_torque(BodyId, Vec3)
  ExternalForce.value      = 0  (drain — §4.1.4 inv 3)
  ExternalTorque.value     = 0
  if MotionType::Kinematic:
    GlobalTransform.position -> JoltMiddleman::set_position(BodyId)
    GlobalTransform.rotation -> JoltMiddleman::set_rotation(BodyId)
For joint in Joint archetype, sorted ascending by JointId:
  if JointMotor present:
    JointMotor.target_velocity -> JoltMiddleman::set_motor_target(JointId)
```

**Jolt → ECS** (substep exit, after `JoltMiddleman::step`):

```text
For body in RigidBody archetype, sorted ascending by BodyId:
  JoltMiddleman::get_linear_velocity(BodyId)  -> Velocity.value
  JoltMiddleman::get_angular_velocity(BodyId) -> AngularVelocity.value
  JoltMiddleman::get_position(BodyId)         -> RigidBody.position
  JoltMiddleman::get_rotation(BodyId)         -> RigidBody.rotation
  JoltMiddleman::is_sleeping(BodyId)          -> Sleeping marker (add/remove)
For pair in JoltMiddleman::drain_contact_pairs():
  emit CollisionStarted | CollisionPersisted | CollisionEnded
  (sorted by (BodyId-low, BodyId-high) — §4.2 invariant 8 ordering)
For pair in JoltMiddleman::drain_trigger_pairs():
  emit TriggerEnter | TriggerStay | TriggerExit
For joint in JointBreakThreshold archetype:
  if JoltMiddleman::accumulated_impulse_exceeds(JointId, threshold):
    emit JointBrokenEvent; despawn joint entity
```

Three properties this seam guarantees:

1. **No mid-substep cross-traffic.** The two barriers above are the
   only legal transit points; a debug-build assertion fires if any
   ECS read or write happens between them (§4.1.4 invariant 2).
   This is what "one-way per substep" means: ECS produces inputs
   into Jolt at entry, Jolt produces outputs into ECS at exit, no
   loop in between.
2. **Iteration order is `BodyId` / `JointId` ascending.** Not
   archetype-chunk order, not Jolt's internal pair-list order, not
   wall-clock contact-listener fire order. The handle is the
   determinism key (§4.1.5b invariant 1 + §4.2 invariant 8); a host
   that re-orders the walk reproduces the same snapshot bytes only
   by accident.
3. **Drain is total.** `ExternalForce` / `ExternalTorque` reset to
   zero at substep entry (§4.1.4 invariant 3); the contact /
   trigger pair lists drain into the per-frame event buffer at
   substep exit and Jolt's listener queue is emptied. Phase 3's
   final state at exit has zero queued physics work.

### 6.4 Determinism guards

The §4.2 invariant 3 promise — byte-equal snapshots across hosts and
runs — is upheld by four mechanical rules each enforced at one site
in the codebase. They are not aspirational; they are how the snapshot
round-trip in §8.6 passes.

1. **Deterministic Jolt config.** `JoltMiddleman::create_world(cfg)`
   constructs the Jolt `PhysicsSystem` with the determinism-relevant
   knobs pinned from `PhysicsConfig` (§4.1.2): integer iteration
   counts (`velocity_iters`, `position_iters`); `warm_start_factor`;
   linear / angular sleep thresholds; layer-pair interaction matrix;
   maximum-bodies / shapes / contacts / constraints budgets. The
   middleman additionally calls
   `JPH::PhysicsSystem::SetDeterministicSimulation(true)` and
   `JPH::JobSystemSingleThreaded` (or a deterministically-seeded
   `JobSystemThreadPool` whose dispatch order is content-of-config
   keyed) so Jolt's internal scheduling does not depend on host
   thread-count. Any future Jolt config knob whose default differs
   between Jolt versions is forced to a glibre-pinned value here;
   this is the "single source of truth for deterministic Jolt
   config" SRP cell.
2. **Fixed iteration order.** Every loop that walks an ECS
   archetype in phase 3 sorts by `BodyId` or `JointId` ascending
   before walking. This includes the entry-barrier walk (§6.3),
   the exit-barrier walk (§6.3), the snapshot-capture walk
   (§7.1.4 invariant 5), the snapshot-restore walk (§8.3.2 step 4),
   the BLAS-invalidation walk during `PhysicsWorldReplaced` (§8.5),
   the `joints/joint_break.cpp` impulse-threshold walk, and the
   `queries/physics_queries.cpp` overlap-result append. The sort is
   a `std::ranges::sort` over a `std::span<BodyId>` materialised
   into a phase-3-arena scratch buffer; the arena resets at
   substep entry. Hash-table iteration order, archetype-chunk
   order, and Jolt's internal pair list never reach an output that
   crosses the substep barrier or the snapshot.
3. **No host float intrinsics.** Per PHILOSOPHY §6 + §7, no
   physics source includes `<immintrin.h>`, `<arm_neon.h>`, or any
   target-specific intrinsic header. The integration math (the
   substep math that runs outside Jolt — accumulator carry,
   external-force drain summation, sleep-frame counter increment)
   uses only `<cmath>` IEEE-754 operations. A pre-build check
   rejects any physics `.cpp` whose preprocessor output references
   intrinsic headers; the only TU exempt is
   `middleman/jolt_middleman.cpp`, which is forbidden by Jolt's
   own determinism contract from emitting non-deterministic
   intrinsics (Jolt's `cross_platform_deterministic` mode disables
   FMA, fast-math, and SIMD reduction-order tricks). The
   `-fno-fast-math -ffp-contract=off -fno-finite-math-only` compile
   flags are pinned engine-wide and re-asserted in physics's
   `CMakeLists.txt`.
4. **IEEE-754 bit-equal storage.** Every `f32` / `f64` field that
   crosses the snapshot or the per-substep barriers is stored
   without rounding, canonicalisation, or denormal-flush. The
   snapshot's serialiser uses `std::bit_cast<std::uint32_t>(x)` /
   `std::bit_cast<std::uint64_t>(x)` per the §7's "determinism
   contract" preamble; the in-memory ECS components carry the
   same bit pattern Jolt produced. NaN payloads round-trip
   unchanged. The runtime never produces a NaN (§4.1.4 invariant
   4); the codec preserves whatever pattern arrives. This is the
   bit that makes "two hosts running the same trace produce
   byte-equal snapshots" a compile-time consequence of the
   serialiser's contract rather than a runtime property to be
   tested at every field.

These four rules together are the §4.2 invariant 3 implementation;
the §11 acceptance test "byte-equal snapshot at tick 1000 across
macOS-arm64 and macOS-x64" passes only if all four hold. A failure
at any of them is caught by the determinism gate (the §8.6 round-
trip test) before it reaches the snapshot bus.

### 6.5 Cross-context handoffs

Physics is one of seven plugins; the four cross-context seams it
honours are pinned here so reviewers can see the seam shape without
chasing through the §5 surface and the §3.3 refusals.

1. **Render reads body transforms via `core`'s ECS components — not
   physics's API.** Phase 5 (`core` transform propagation) lifts the
   `RigidBody` row's position / rotation into the entity's
   `GlobalTransform` (§4.1.5 composition); phase 6 (`render`
   `cull-extract`) reads `GlobalTransform` + `PreviousGlobalTransform`
   like any other entity's transform (`specs/render/SPEC.md` §6.2.1
   step 2.i). Render does not call into `physics::*` to fetch a body
   transform; the seam is core's component, not physics's surface.
   This is the §3.3 refusal "render does not consume physics's API
   for transform read-back" made mechanical.
2. **Queries return spans into caller arenas.** `PhysicsQueries::
   ray_cast(...)`, `shape_cast(...)`, `overlap(...)`, and
   `closest_point(...)` accept a caller-supplied
   `std::span<QueryHit>` output buffer and write **at most**
   `out.size()` rows, returning the actual count (§4.1.10
   invariant 2 — plain-data results, no Jolt internals). Callers
   are expected to source the buffer from a per-context arena
   (e.g. render's per-frame arena, gameplay's per-system command
   buffer); physics never allocates query memory. The returned
   span is keyed by the caller's arena lifetime; the rows
   themselves are POD, copyable, and stable across reload (the
   `BodyId` field re-resolves under §4.2 invariant 5). A query
   issued during phase 3 returns `physics::Error::QueryDuringStep`
   without writing to the buffer (§4.1.10 invariant 4); a query
   issued in phase 1 reflects the prior frame's terminal state;
   queries in phases 5+ reflect the current frame's post-step
   state.
3. **Static BLAS lifecycle is handed off to `render`.** Cooked
   static-mesh collision shapes (§4.1.6: heightfields, baked
   triangle meshes) and their visual-mesh BLAS counterparts share
   a content-hash through `geometry`'s asset bundle (§3.3); render
   imports per-body BLAS handles via `RTAccelStructures`
   (`specs/render/SPEC.md` §4.1.8). Physics's job in this
   handoff is two-fold: (a) at body create-time, physics resolves
   the `ShapeBlob` content hash so render's BLAS importer keys on
   the same hash; (b) on hot-reload, physics emits the
   `PhysicsWorldReplaced` event (§8.5) carrying the surviving
   `BodyId` set so render's subscriber can compare each body's
   post-reload position / rotation against its recorded BLAS
   transform and rebuild any whose transform changed (`specs/
   render/SPEC.md` §4.1.8 invariant 3). Physics does not own the
   BLAS bytes, does not call the BLAS builder, and does not
   participate in the rebuild scheduling — render owns those.
4. **Snapshot is the single carrier across the swap.** Hot-reload
   (§8) keys the entire physics-side handoff on `PhysicsSnapshot`
   (§4.1.12 / §7.1.4): the outgoing plugin captures one snapshot
   in `glibre_plugin_drain` (§8.3.1), the loader keeps the bytes
   in its phase-8 migration arena (§8.2 row "PhysicsSnapshot"),
   and the incoming plugin restores them in `glibre_plugin_register`
   (§8.3.2). No second on-the-wire format is introduced for
   reload; no per-aggregate carrier exists alongside the snapshot
   (§3.2 collapse #9). The snapshot's bus subscribers (render's
   BLAS invalidator, the e2e harness's golden-trace anchor —
   §8.5) read the same bytes the §11 acceptance tests check.

### 6.6 Concurrency

Phase 3 runs entirely on the game-loop driver thread. Jolt's
internal `JobSystem` is configured per §6.4 rule 1 to be either
single-threaded or deterministically-seeded; either way, the public-
visible behaviour is "synchronous body, returns when stepping is
done". No physics worker thread outlives a phase-3 invocation
window; the worker pool is owned by `PhysicsWorld` (§4.1.1
composition) and torn down by `glibre_plugin_drain` (§8.2 row
"Per-plugin worker thread pool").

Plugins outside physics never touch a Jolt worker; the only
cross-thread synchronisation physics participates in is the
single barrier at the end of phase 3, which is the same barrier
core's `FrameLoop` (`specs/core/SPEC.md` §6.5) uses between every
phase. Per-system parallelism inside phase 3 (e.g. parallel
gather of `ExternalForce` writes) is post-MVP and would land
through the §6.4 rule 2 fixed-iteration-order seam — the sort key
remains `BodyId` ascending; only the per-row body of the loop
parallelises. The schedule-side machinery (`specs/core/SPEC.md`
§6.4) already collects the read/write sets needed to make that
seam safe; physics will register its phase-3 systems with those
declarations when the parallel-dispatch wrapper lands.

## 7. Persistence & Schemas

Physics's persistence surface is the **deterministic-replay spine**:
the configuration that drives a `PhysicsWorld`, the cooked collision
geometry blobs that populate its shape table, the constraint
descriptors that re-create joint topology, and the `PhysicsSnapshot`
that captures one tick of stepping output for byte-equal cross-host
comparison. Per-frame artefacts (`ContactManifold`, contact / trigger
events, `Island` membership, `QueryHit` rows, Jolt-internal solver
state) are runtime-only and never serialised — they are reborn each
substep (§4.1.4, §4.1.8, §4.1.9, §4.1.14) and reclaimed at end of
phase 8 (§4.2 invariant 8).

All schemas below are authored as `data/schemas/physics/<Type>.fory`
files per `reviews/decisions/fory-codegen.md` and compile into the
`glibre-types` middleman dylib. FQNs are `glibre.physics.<Type>`.
Each schema ships with at least one Catch2 round-trip test under
`tests/data/schemas/physics/<Type>.cpp` per the data SPEC §7.5
golden-roundtrip mandate.

**Determinism contract (load-bearing).** Every `f32` and `f64` field
in §7.1's schemas is serialised as the **bit-exact IEEE 754
representation** of its in-memory value: `f32` as the 32-bit
little-endian payload of `std::bit_cast<std::uint32_t>(x)`; `f64` as
the 64-bit little-endian payload of `std::bit_cast<std::uint64_t>(x)`.
No rounding, no canonicalisation, no platform-tier branch. NaN
payloads round-trip unchanged (the snapshot writer never produces a
NaN — §4.1.4 invariant 4 — but the codec contract preserves whatever
bit pattern arrives from a host's solver). This rule is the byte-
equality unit `PhysicsSnapshot` invariant 3 (§4.1.12) compiles down
to; violating it at any field decodes the whole spine into a
`physics::Error::SnapshotMismatch` at the determinism gate.

### 7.1 Persistent types

#### 7.1.1 `PhysicsConfigRecord` — frozen deterministic configuration

**File:** `data/schemas/physics/PhysicsConfigRecord.fory`
**FQN:** `glibre.physics.PhysicsConfigRecord`
**Lifetime scope:** per-world, per-glibre-version. Authored by the
editor / world-template tool, read at world init by `PhysicsWorld`
constructor (§4.1.2). Init-time-immutable for the world's lifetime
(§4.1.2 invariant 1). The `content_hash` field of the in-memory
`PhysicsConfig` (§5) is BLAKE3 over the canonicalised bytes of this
record and is the dispatch key the snapshot's `physics_config_hash`
(§4.1.12, §7.1.4) compares against.

```fory
schema glibre.physics.PhysicsConfigRecord {
  version  1
  since    "0.1.0"

  # ---- Global stepping math (§4.1.2, §4.1.3) ----
  field gravity              : vec3f tag 1  since 1
  field fixed_dt             : f32   tag 2  since 1
  field max_substeps         : u8    tag 3  since 1   default 4
  field velocity_iters       : u8    tag 4  since 1   default 10
  field position_iters       : u8    tag 5  since 1   default 2
  field warm_start_factor    : f32   tag 6  since 1   default 0.85
  field ccd_enabled          : bool  tag 7  since 1   default true
  field rng_seed             : u64   tag 8  since 1   default 0

  # ---- Sleep thresholds (§4.1.14, §5 SleepThresholds) ----
  field sleep_linear_speed   : f32   tag 9  since 1   default 0.05
  field sleep_angular_speed  : f32   tag 10 since 1   default 0.05
  field sleep_frame_count    : u16   tag 11 since 1   default 30

  # ---- World budgets (§5 WorldBudgets) ----
  field max_bodies           : u32   tag 12 since 1   default 0
  field max_shapes           : u32   tag 13 since 1   default 0
  field max_constraints      : u32   tag 14 since 1   default 0
  field max_contacts         : u32   tag 15 since 1   default 0

  # ---- Layer filter (§4.1.2 inv 4, §4.1.11) ----
  # Total mapping CollisionLayer -> BroadphaseLayer; index is the
  # CollisionLayer ordinal, value is the BroadphaseLayer enum.
  field broadphase_mapping   : list<u8>  tag 16 since 1
  # Layer-pair interaction matrix. Row-major, length = layer_count^2,
  # value = LayerInteraction enum (Ignore / Collide / TriggerOnly).
  field layer_interactions   : list<u8>  tag 17 since 1
  field layer_count          : u16       tag 18 since 1
}
```

**Invariants** (echoing §4.1.2 / §4.1.11 where the runtime aggregate
enforces them):

1. **Layer matrix is total at decode.** `len(broadphase_mapping) ==
   layer_count` and `len(layer_interactions) == layer_count *
   layer_count`. Either mismatch returns
   `physics::Error::ConfigInvalid` at deserialise time; the world
   constructor never observes a partial matrix (§4.1.2 invariant 4).
2. **Stepping fields are bit-exact.** `fixed_dt`, `warm_start_factor`,
   `gravity`, and the sleep speeds are stored per the §7
   determinism contract above. Two builds writing the same
   `PhysicsConfig` produce byte-equal records on every supported host.
3. **`content_hash` is computed, not stored.** The record carries
   no hash field; the in-memory `PhysicsConfig.content_hash` is
   computed by BLAKE3 over the canonicalised serialised bytes at
   load time. A future schema bump that adds a field bumps the hash
   automatically — there is no parallel "version of the hash" to
   keep in sync (data SPEC §4.4).
4. **No runtime tuning.** Re-tuning a world means writing a new
   record and re-creating the world (§4.1.2 invariant 1); there is
   no in-place mutation path through this schema.

#### 7.1.2 `ShapeBlobRecord` — cooked collision-geometry payload

**File:** `data/schemas/physics/ShapeBlobRecord.fory`
**FQN:** `glibre.physics.ShapeBlobRecord`
**Lifetime scope:** per-asset; produced by `content` / `geometry` at
cook time (§3.3, §4.1.6) and consumed by `PhysicsWorld`'s shape
table at first reference. Identified by `content_hash` (BLAKE3 of
the canonicalised bytes); two `Collider`s referencing the same hash
share one row of the table (§4.1.6 invariant 1, §4.2 invariant 6).

```fory
schema glibre.physics.ShapeBlobRecord {
  version  1
  since    "0.1.0"

  # Stable identifier. BLAKE3 of the canonicalised serialised bytes
  # of every other field below; computed by the cook tool, verified
  # at deserialise time. Mismatch ⇒ Error::ShapeBlobCorrupt.
  field content_hash       : u64   tag 1 since 1
  # ShapeKind ordinal (sealed sum below). One ShapeKind per record.
  field kind               : u8    tag 2 since 1

  # ---- Primitive parameters (used by Sphere / Box / Capsule) ----
  # Encoding rule: every primitive populates the fields its kind
  # references; codegen fills unreferenced fields with defaults at
  # write time. The kind-tag is the discriminator at read time.
  field sphere_radius      : f32   tag 3 since 1   default 0.0
  field box_half_extents   : vec3f tag 4 since 1   default { 0.0, 0.0, 0.0 }
  field capsule_radius     : f32   tag 5 since 1   default 0.0
  field capsule_half_height: f32   tag 6 since 1   default 0.0

  # ---- Convex hull (kind == ConvexHull) ----
  # Vertices in shape-local space. Jolt's hull builder consumes the
  # span verbatim; the cook tool guarantees vertex count >= 4 and
  # planarity-free input.
  field hull_vertices      : list<vec3f> tag 7 since 1
  # Per-vertex hull plane normal index (Jolt `HullVertex::mPlane`).
  field hull_plane_indices : list<u32>   tag 8 since 1

  # ---- Triangle mesh (kind == TriangleMesh) ----
  field mesh_vertices      : list<vec3f> tag 9  since 1
  # Triangle indices, 3 entries per face, length divisible by 3.
  field mesh_indices       : list<u32>   tag 10 since 1
  # Per-triangle MaterialId ordinal (length == len(mesh_indices)/3),
  # or empty when the shape uses a single material from `Collider`.
  field mesh_materials     : list<u32>   tag 11 since 1

  # ---- Heightfield (kind == Heightfield) ----
  field heightfield_extent_x : u32   tag 12 since 1   default 0
  field heightfield_extent_z : u32   tag 13 since 1   default 0
  field heightfield_scale    : vec3f tag 14 since 1   default { 1.0, 1.0, 1.0 }
  field heightfield_samples  : list<f32> tag 15 since 1

  # ---- Compound (kind == Compound) ----
  # Sub-shapes referenced by content_hash; the table lookup at load
  # time resolves each entry to a child ShapeHandle. The transform
  # is the sub-shape's offset within the compound's local frame.
  field compound_child_hashes     : list<u64>   tag 16 since 1
  field compound_child_positions  : list<vec3f> tag 17 since 1
  field compound_child_rotations  : list<quatf> tag 18 since 1
}
```

**`ShapeKind` sealed sum** (mirrors §4.1.6 + §5):

| Ordinal | Name           | Populated tags                              |
|---------|----------------|---------------------------------------------|
| 0       | `Sphere`       | 3                                           |
| 1       | `Box`          | 4                                           |
| 2       | `Capsule`      | 5, 6                                        |
| 3       | `ConvexHull`   | 7, 8                                        |
| 4       | `TriangleMesh` | 9, 10, 11                                   |
| 5       | `Heightfield`  | 12, 13, 14, 15                              |
| 6       | `Compound`     | 16, 17, 18                                  |

Adding a kind (post-MVP 2D primitives, SDF voxel, runtime quickhull
per §3.3) appends a new ordinal; existing ordinals are immutable
once shipped (§7.2.2 below).

**Invariants:**

1. **Self-authenticating.** `content_hash` MUST equal BLAKE3 of the
   canonicalised serialised bytes of every field tag ≥ 2 (i.e.
   excluding the hash itself). Mismatch decodes to
   `physics::Error::ShapeBlobCorrupt`; the corrupt blob is **not**
   migrated and **not** fed to Jolt.
2. **Kind-tag dispatch is total.** A reader observes only the fields
   its `kind` references; reading a field outside that set is
   undefined and a debug-build assertion fires. The cook tool
   zeroes unused fields at write time so canonicalisation is
   determinism-stable across kinds (§7.2.2 invariant 2).
3. **No runtime baking.** Convex decomposition / V-HACD / quickhull /
   heightfield resampling all live in `tools` + `content`/`geometry`
   at cook time (§3.3). Physics consumes the cooked record only;
   any record whose payload would require runtime baking decodes to
   `physics::Error::ShapeBlobCorrupt`.
4. **Compound children resolve at load time, not at decode.** A
   `Compound` record references children by `content_hash`; the
   shape table resolves each hash to a `ShapeHandle` when the
   compound is loaded into a world. A missing child hash returns
   `physics::Error::ShapeBlobMissing` at table-load time, not at
   record-deserialise time (the bytes are well-formed; the world's
   asset graph is what is incomplete).
5. **Reference-counted at the table.** The byte form makes no
   reference-count statement; refcount lives on `ShapeHandle` in
   the world's shape table (§4.1.6) and is reset to zero on every
   load. Two worlds in the same process loading the same hash
   each get one reference; the table is per-world (§4.1.1).

#### 7.1.3 `JointDescriptorRecord` — constraint topology + tuning

**File:** `data/schemas/physics/JointDescriptorRecord.fory`
**FQN:** `glibre.physics.JointDescriptorRecord`
**Lifetime scope:** per-world; one record per `Joint` ECS entity at
world spawn. Re-emitted into `PhysicsSnapshot` (§7.1.4) as the
joint roster needed to re-create constraint topology on restore.
Authored by the editor / scene-template tool, read by physics's
joint-creation hook at component-add (§4.1.7).

```fory
schema glibre.physics.JointDescriptorRecord {
  version  1
  since    "0.1.0"

  # ---- Identity (§4.1.7, §5 JointEndpoints) ----
  field joint_id           : u32   tag 1  since 1
  # JointKind ordinal — sealed sum (§5):
  #   0 Point, 1 Hinge, 2 Slider, 3 Cone,
  #   4 Distance, 5 SwingTwist.
  field kind               : u8    tag 2  since 1
  field body_a             : u32   tag 3  since 1   # BodyId ordinal.
  field body_b             : u32   tag 4  since 1   # BodyId ordinal.

  # ---- Anchor frames on each body (§5 JointFrame) ----
  field frame_a_position   : vec3f tag 5  since 1
  field frame_a_rotation   : quatf tag 6  since 1
  field frame_b_position   : vec3f tag 7  since 1
  field frame_b_rotation   : quatf tag 8  since 1

  # ---- Optional companion: limits (§5 JointLimits) ----
  field has_limits         : bool  tag 9  since 1   default false
  field limit_lower        : f32   tag 10 since 1   default 0.0
  field limit_upper        : f32   tag 11 since 1   default 0.0
  field limit_swing_y      : f32   tag 12 since 1   default 0.0
  field limit_swing_z      : f32   tag 13 since 1   default 0.0
  field limit_twist_low    : f32   tag 14 since 1   default 0.0
  field limit_twist_high   : f32   tag 15 since 1   default 0.0

  # ---- Optional companion: motor (§5 JointMotor) ----
  field has_motor          : bool  tag 16 since 1   default false
  field motor_target_value : f32   tag 17 since 1   default 0.0
  field motor_max_force    : f32   tag 18 since 1   default 0.0
  field motor_damping      : f32   tag 19 since 1   default 0.0

  # ---- Optional companion: break (§5 JointBreakThreshold) ----
  field has_break          : bool  tag 20 since 1   default false
  field break_max_force    : f32   tag 21 since 1   default 0.0
  field break_max_torque   : f32   tag 22 since 1   default 0.0
}
```

**Invariants:**

1. **Sealed-sum dispatch.** `kind` decodes to one of the six ordinals
   listed in §5; an out-of-range value returns
   `physics::Error::JointKindUnsupported` (§5 / §10), which is the
   same error a future build raises when a record authored on a
   newer schema arrives at an older middleman. Existing ordinals
   are immutable; adding a kind is a schema bump (§7.2.3 below).
2. **Endpoints exist at load time.** `body_a` and `body_b` ordinals
   resolve to live `BodyId`s in the world; an unresolved endpoint
   returns `physics::Error::JointDanglingEndpoint` (§4.1.7
   invariant 1). The error fires at **load time**, not at decode
   time — the bytes are well-formed; the world's body roster is
   what is incomplete.
3. **Companion presence is the discriminator.** The `has_*` bits
   are the only discriminators for which optional payload bytes
   are meaningful. Codegen synthesises zero defaults when a bit is
   false, and the world skips the corresponding companion-component
   add (§4.1.7 invariant 2). Two records that differ only in zeroed
   "absent" payload remain hash-equivalent because the canonicalised
   form normalises absent payloads to zero (§7.2.3 invariant 5).
4. **Frame quaternions are unit-normalised.** `frame_a_rotation`
   and `frame_b_rotation` MUST satisfy `|q| ∈ [1 - 1e-6, 1 + 1e-6]`
   at decode; non-unit quaternions return
   `physics::Error::ConfigInvalid` (§4.1.7's frame is part of the
   constraint's deterministic input).
5. **Bit-exact float tuning.** Every `f32` field on this record is
   stored per the §7 determinism contract; an authored joint
   re-spawned on a different host produces a byte-equal Jolt
   constraint impulse trajectory (§4.2 invariant 3).

#### 7.1.4 `PhysicsSnapshot` — deterministic replay artefact

**File:** `data/schemas/physics/PhysicsSnapshot.fory`
**FQN:** `glibre.physics.PhysicsSnapshot`
**Lifetime scope:** per-tick capture; produced by
`PhysicsWorld::snapshot()` (§4.1.12, §5), consumed by the
determinism gate (cross-host byte compare), by golden-trace replay,
and by the post-MVP rollback path. Survives across worlds and
across hot-reload (§4.1.12 identity & lifetime).

```fory
schema glibre.physics.PhysicsSnapshot {
  version  1
  since    "0.1.0"

  # ---- Header (§5 SnapshotHeader) ----
  # Monotone schema_version is the migration discriminator (§7.2.4).
  field schema_version       : u64   tag 1 since 1
  field physics_config_hash  : u64   tag 2 since 1
  field world_tick           : u64   tag 3 since 1
  field accumulator_carry    : f32   tag 4 since 1
  field middleman_abi_hash   : u64   tag 5 since 1

  # ---- Body roster (§5 SnapshotBody, BodyId-keyed) ----
  # Iteration order is BodyId-ascending; the writer enforces sort at
  # capture time so two hosts emit byte-equal payloads (§4.1.12 inv 3).
  field body_ids                  : list<u32>   tag 6  since 1
  field body_motion_types         : list<u8>    tag 7  since 1
  field body_positions            : list<vec3f> tag 8  since 1
  field body_rotations            : list<quatf> tag 9  since 1
  field body_linear_velocities    : list<vec3f> tag 10 since 1
  field body_angular_velocities   : list<vec3f> tag 11 since 1
  field body_sleep_frames         : list<u16>   tag 12 since 1
  field body_sleeping_flags       : list<bool>  tag 13 since 1
  # Per-body active shape ShapeBlobRecord.content_hash. References,
  # not bytes (§4.1.12 invariant 4); the bytes live in the asset
  # bundle and are loaded by `data` / `content` (§3.3).
  field body_shape_blob_hashes    : list<u64>   tag 14 since 1

  # ---- Joint roster (§5 SnapshotJoint, JointId-keyed) ----
  # Iteration order is JointId-ascending; sort enforced at capture.
  field joint_ids                 : list<u32>   tag 15 since 1
  field joint_kinds               : list<u8>    tag 16 since 1
  field joint_body_a              : list<u32>   tag 17 since 1
  field joint_body_b              : list<u32>   tag 18 since 1
  field joint_normal_impulses     : list<f32>   tag 19 since 1
  field joint_friction_impulses   : list<f32>   tag 20 since 1
}
```

**Invariants** (echoing §4.1.12 / §4.2 invariant 3):

1. **Byte-equal across hosts.** Two snapshots captured at the same
   `world_tick` on two hosts running with the same
   `physics_config_hash` are bit-identical (§4.2 invariant 3,
   R-4.1.NF3, PHILOSOPHY §7). The determinism gate compares the
   byte form; mismatch returns
   `physics::Error::SnapshotMismatch` and surfaces a host-pair
   diff in the trace report.
2. **Schema version is monotone.** `schema_version` increases by
   exactly one per shipped Fory schema bump (§7.2.4 below); readers
   refuse a higher version with
   `physics::Error::SnapshotVersionFuture` (a future build wrote a
   snapshot an older middleman cannot reconstruct). Migrations
   from `vN` to `vM > N` follow §7.2.4; there is no "skip-version"
   decode path.
3. **`physics_config_hash` is the dispatch key.** Restoring a
   snapshot into a world whose `PhysicsConfig.content_hash` differs
   returns `physics::Error::SnapshotConfigDrift`; cross-config
   restore is forbidden because `PhysicsConfig` is the only input
   that decides what counts as deterministic (§4.1.2 invariant 2).
4. **`middleman_abi_hash` gates restore.** A snapshot whose
   `middleman_abi_hash` differs from the live `JoltMiddleman` hash
   (§4.1.13, §5 `MiddlemanInfo`) is **refused at restore** with
   `physics::Error::PluginAbiHashMismatch`. Cross-Jolt-version
   replay is not supported; the determinism gate recompiles its
   trace corpus when the middleman ships a new hash (§4.2 inv 7).
5. **Parallel-list shape, not array-of-struct.** Bodies and joints
   are stored as parallel `list<T>` columns rather than an
   array-of-struct. Codegen still generates SoA C++ (§4.1.12), but
   the schema's column shape makes the migration story explicit:
   adding a per-body field is one new column at a new tag, not a
   struct-shape change. Index `i` across the body columns names
   one body; mismatched column lengths decode to
   `physics::Error::SnapshotMismatch`.
6. **No NaN / no denormal in writer output.** The writer asserts
   each `f32` payload is finite and non-denormal at capture time
   (§4.1.4 invariant 4). The reader decodes whatever bytes arrive
   verbatim per §7's determinism contract — the assertion guards
   only the producer side; corrupt inputs from a foreign host
   surface as `SnapshotMismatch` at the determinism gate.
7. **References, not bytes.** Snapshots reference `ShapeBlobRecord`
   payloads by `content_hash`; the bytes themselves live in the
   asset bundle and are loaded by `data` / `content` (§3.3,
   §4.1.12 invariant 4). A restore with an absent shape hash
   returns `physics::Error::ShapeBlobMissing` at world rebind
   time, not at decode time.

### 7.2 Migration rules

Per `reviews/decisions/fory-codegen.md` §"Migration Mechanic" and
data SPEC §7.4, every schema-version bump emits a generated
dispatcher hookup; physics owns the migration *bodies* for the
types above (under `src/physics/migrations/`), and `data` owns the
plumbing.

#### 7.2.1 `PhysicsConfigRecord` — additive defaulted-field only

The config record follows the **additive defaulted-field** pattern
(data SPEC §7.4 rule 6, render SPEC §7.2.1 case 1):

1. **`vN → vN+1` adds a field at a new tag, appended past the prior
   version's last offset.** Default value defined in the schema;
   codegen synthesises the default at deserialise time (Fory's
   `since` clause). No migration body required; codegen emits
   `migrate_PhysicsConfigRecord_v<N>_to_v<N+1>` as the identity
   mapping with default-fill (data SPEC §7.4 rule 6).
2. **`vN → vN+1` removes a field.** Tag becomes `reserved`; never
   reused (data SPEC §7.4 rule 7). The removed field's runtime
   reading code is deleted in the same release; readers of older
   payloads ignore the reserved bytes.
3. **`vN → vN+1` changes the meaning of an existing field.**
   Treated as breaking. Bump the schema and author the migration
   body under `src/physics/migrations/
   physics_config_record_v<N>_to_v<N+1>.cpp`. Round-trip golden
   under `tests/data/schemas/physics/PhysicsConfigRecord/v<N>.
   fory.bin` becomes mandatory (data SPEC §7.5).
4. **Adding a `LayerInteraction` enumerator.** Append-only at the
   ordinal end; existing ordinals are immutable. An older middleman
   reading a newer-ordinal value returns
   `physics::Error::ConfigInvalid` (the value is outside its closed
   sum). This matches the closed-sum rule render SPEC §7.1.1 uses
   for `AntiAliasMode` etc.
5. **`content_hash` consequence.** Any of the above changes the
   canonicalised bytes and therefore changes the in-memory
   `PhysicsConfig.content_hash`. A snapshot captured under the old
   hash is refused with `SnapshotConfigDrift` (§7.1.4 invariant 3)
   on a build that authors the new hash; the determinism gate
   regenerates its trace corpus when the schema bumps.

#### 7.2.2 `ShapeBlobRecord` — additive parameter struct extensions + new kinds

Shape parameter struct extensions are **additive**:

1. **Adding a parameter to an existing kind** (e.g. a third
   capsule-end radius for a future "TaperedCapsule" generalisation)
   appends a new tag past the prior version's last offset.
   Defaulted-field synthesis (Fory `since`) makes older blobs
   readable; codegen emits the identity migration with default-fill
   (data SPEC §7.4 rule 6).
2. **Adding a new `ShapeKind` ordinal.** Append-only at the ordinal
   end. New parameter fields the kind needs are appended past the
   prior last offset (rule 1). Existing kind ordinals are immutable;
   reusing an ordinal returns
   `physics::Error::ShapeBlobCorrupt` at decode (the cook tool
   would be writing a new shape against an old ordinal). An older
   middleman reading a newer-kind value returns
   `physics::Error::ShapeBlobCorrupt` — the closed-sum rule in
   §7.1.2 invariant 2 holds across versions.
3. **Removing a kind.** Tag becomes `reserved`; ordinal never
   reused. Cooked blobs of the removed kind become unreadable on
   the build that drops the kind, by design. Asset-pipeline
   migration (recook to a still-supported kind) is owned by
   `tools` + `content` (§3.3); physics does not bridge the gap.
4. **Canonicalisation across kinds.** Unused-field zero-fill is
   load-bearing for `content_hash` stability across cook-tool
   versions: a cook tool that emits a `Sphere` record must zero
   every non-sphere field. The `glibre-foryc` codegen enforces
   this at write time per the §7 determinism contract; a
   non-zero unused field returns
   `physics::Error::ShapeBlobCorrupt` at decode.
5. **No runtime ID-bake migration.** Compound records reference
   children by `content_hash`, not by table index, so reordering
   children in a future cook does not require a migration as long
   as the children themselves are still resolvable. A child whose
   hash changes is a new asset; the parent recomputes its own
   `content_hash` and ships as a new asset (§7.1.2 invariant 1).

#### 7.2.3 `JointDescriptorRecord` — new joint kinds add variants

Joint kind extensions are **additive sealed-sum bumps**:

1. **Adding a `JointKind` ordinal** (post-MVP ragdoll-friendly
   variants per §3.3). Append-only at the ordinal end; existing
   ordinals are immutable. New tuning fields the kind needs are
   appended past the prior version's last offset with `since N+1`
   and a default; codegen emits the identity migration with
   default-fill (data SPEC §7.4 rule 6).
2. **Adding a companion** (e.g. a future `has_drive_curve` bit
   plus payload). Same additive pattern: one new `has_*` bool tag
   at default false, plus the payload tags at zero defaults. Older
   records decode to `has_drive_curve = false` and zero payload;
   the world skips the companion-component add (§4.1.7
   invariant 2).
3. **Removing a kind.** Treated as breaking. Worlds shipping with
   that kind cannot load on the new build; the migration body
   under `src/physics/migrations/` is responsible for either
   re-mapping the old kind to a still-shipped one (lossy, but
   total — data SPEC §7.4 rule 3) or surfacing
   `physics::Error::JointKindUnsupported` at restore. The choice
   is per-bump in the migration body; it is not a schema-level
   decision.
4. **Tuning-field meaning change.** Same as §7.2.1 case 3: schema
   bump, migration body, golden under
   `tests/data/schemas/physics/JointDescriptorRecord/v<N>.fory.bin`.
5. **Canonicalisation across kinds.** A record's
   `content_hash`-equivalent stability comes from zero-filling
   absent companions (§7.1.3 invariant 3); writers MUST zero
   payload bytes when the corresponding `has_*` bit is false. A
   non-zero absent payload returns
   `physics::Error::ConfigInvalid` at decode.

#### 7.2.4 `PhysicsSnapshot` — monotone version, golden-replay corpus

Snapshot migrations follow data SPEC §7.4 rules 1–7 with the
following physics-specific overlay:

1. **Monotone version.** `schema_version` increases by exactly one
   per shipped bump (§7.1.4 invariant 2). The dispatcher composes
   the chain `vN → vN+1 → ... → vM` per data SPEC §7.4 rule 1; no
   skip-step decode path.
2. **Per-version golden corpus.** For every shipped `schema_version`
   `N`, `tests/data/schemas/physics/PhysicsSnapshot/v<N>.fory.bin`
   carries a recorded snapshot. The Catch2 round-trip harness:
   - decodes the `vN` golden,
   - runs every migration step `vN → vN+1 → ... → vCurrent`,
   - asserts the post-migration in-memory snapshot equals a
     matching `v<Current>.expected.fory.bin` golden, and
   - asserts re-serialising the migrated snapshot under the
     current writer reproduces `v<Current>.fory.bin` byte-for-byte.
   Failure on any step blocks the bump (data SPEC §7.5).
3. **Adding a per-body or per-joint column.** Append a new
   `list<T>` field at a new tag with `since N+1` and a default
   "all entries zero / empty"; codegen emits the identity
   migration that fills the new column with the default at length
   matching `body_ids` / `joint_ids`. The §7.1.4 invariant 5
   column-length check enforces the fill.
4. **Adding a header field.** Same additive defaulted-field
   pattern as §7.2.1 case 1; the migration body fills the new
   header field with a deterministic default (typically zero or
   the current world's value when the snapshot is restored into
   that world).
5. **Removing a column.** Tag reserved; never reused. The migration
   body is responsible for **dropping** the column from older
   snapshots (the dispatcher reads the old column, ignores it,
   and writes a snapshot without it). Subsequent goldens regenerate.
6. **Changing column-element type** (e.g. `f32 → f64` on
   `accumulator_carry`). Treated as breaking. Migration body
   converts under the §7 bit-exact contract: an `f32` value
   migrates to an `f64` value via `static_cast<double>(x)`, where
   `x` is read by `std::bit_cast<float>` on the wire bytes. The
   reverse (`f64 → f32`) is forbidden inside MVP — narrowing
   loses determinism at the migrated tick.
7. **Cross-build replay corpus.** The determinism gate's trace
   corpus is keyed on `(schema_version, physics_config_hash,
   middleman_abi_hash)`. A bump of any of those three forces a
   corpus regeneration, run by CI under the new build before the
   bump merges. A snapshot whose triple matches the corpus key
   but whose bytes differ from the recorded golden returns
   `physics::Error::SnapshotMismatch` and surfaces the diff
   (§7.1.4 invariant 1).

### 7.3 What is NOT persisted

To make the boundary explicit (in line with §5 "Serialised schemas
(Fory)" and render SPEC §7.3):

| Artefact                                  | Why not persisted                                                                                                  |
|-------------------------------------------|--------------------------------------------------------------------------------------------------------------------|
| `ContactManifold` / contact events        | Per-substep; reborn each step, reclaimed end-of-phase-8 (§4.1.8, §4.2 inv 8).                                      |
| `TriggerEnter` / `TriggerStay` / `TriggerExit` | Per-substep ECS event components; same lifetime as contact events (§4.1.9).                                  |
| `JointBrokenEvent`                        | Same-frame event; consumed by phases 5+ inside the emitting frame (§4.1.7 inv 4).                                 |
| `Island` membership                       | Rebuilt by Jolt every substep; read-only, not durable across steps (§4.1.14 inv 2).                                |
| `QueryHit` rows                           | Caller-owned span output of `PhysicsQueries`; never crosses the persistence layer (§4.1.10 inv 2).                 |
| Jolt-internal solver state                | Owned by Jolt's `PhysicsSystem`; never crosses the `JoltMiddleman` ABI as bytes (§4.1.13 inv 1).                   |
| `ShapeHandle` (the runtime handle)        | Per-world refcount; resolved from `ShapeBlobRecord.content_hash` at table load (§4.1.6 inv 1, §7.1.2 inv 5).       |
| `BodyId` allocator state                  | Stable per world; the snapshot's `body_ids` column is the authority for what is live (§4.2 inv 5, §7.1.4 inv 5).   |
| `Accumulator` private state               | Captured into `PhysicsSnapshot.accumulator_carry`; no separate persistence (§4.1.3, §7.1.4).                       |
| `PhysicsMaterial` bytes                   | Owned at the asset layer by `data` (§3.3); physics consumes `MaterialId` references only.                          |
| `PhysicsQueries` / `LayerFilter`          | Runtime objects derived from `PhysicsConfigRecord` at world init (§4.1.10, §4.1.2); not persisted independently.   |

These appear in the persistence surface only as **identifiers**
(`BodyId`, `JointId`, `MaterialId`, `content_hash`) referenced from
§7.1, never as byte payloads.

## 8. Hot-Reload Contract

This section specialises the engine-wide hot-reload protocol
(`reviews/decisions/hot-reload-protocol.md` — drain → swap → migrate →
resume) to the **physics plugin**. It defines exactly which
physics-owned state survives a `physics.dylib` swap, what
`migrate(...)` must do to re-apply `RigidBody` mass / velocity /
position bytes, rebuild `Joint` topology, and drain `ContactManifold`
queues, and which conditions cause physics's reload attempt to be
refused with the engine's standard `core::Error::HotReloadRefused`
arm. Engine-wide concerns (per-plugin atomicity, observer bus event
shapes, error wrapping rules, the `enqueue_hot_reload` E2E hook,
ABI-hash gating) are not re-stated here — see the protocol record
and the `JoltMiddleman` invariants in §4.1.13. Physics adds nothing to
that machinery; it only fills in the four pluggable points the
protocol leaves to each plugin: drain side-effects, survival
inventory, migrate body, and register-time rehydration.

### 8.1 Reload point — phase 8, never mid-frame

The engine schedule (`reviews/decisions/frame-phases.md`) places the
hot-reload barrier at phase 8, **after `render-submit` (phase 7) and
before `present` (phase 9)**. Physics's reload protocol is anchored
to that one slot and refuses any other.

At phase 8 entry, physics's in-flight state is:

1. **Phase 3 already returned for frame N.** Phase 3 owns Jolt's
   `Step` (§4.2 invariant 1); by the time phase 8 begins, every
   substep the `Accumulator` owed has been drained through Jolt's
   barrier and committed back to ECS storages (§4.1.4 invariants 2
   and 3). No solver thread is mid-iteration; no narrowphase pair is
   half-resolved.
2. **Substep ECS↔Jolt commit has flushed.** The two commit barriers
   (§4.2 invariant 2) ran inside phase 3; ECS-side intents
   (`ExternalForce`, `ExternalTorque`, kinematic transform overrides)
   were drained into Jolt at substep entry, and Jolt-side outputs
   (positions, rotations, linear / angular velocities, sleep flags)
   were written back to their middleman-typed `RigidBody` companion
   components at substep exit. Phase 8 sees no straddling write.
3. **Contact / trigger / joint-broken events for frame N are
   drainable.** Per §4.2 invariant 8, all seven event types
   (`CollisionStarted` / `CollisionPersisted` / `CollisionEnded` /
   `TriggerEnter` / `TriggerStay` / `TriggerExit` / `JointBrokenEvent`)
   are written at substep exit (phase 3) and reclaimed at end of
   phase 8 (after phases 5+ of frame N have read them). The "drained
   on swap" bullet in §8.3 below makes that reclamation the loader's
   single point of clearance for the swap, eliminating any chance a
   manifold from the old solver's coordinate space leaks into the
   new plugin's first step.
4. **Spatial query results from frame N are static.** The query
   surface is read-only after phase 3 (§4.2 invariant 9); phase 8
   makes no fresh queries and the previous frame's hits are owned
   by their callers (downstream contexts hold them by value, not by
   reference into Jolt's broadphase).

These four conditions are the physics-half of the protocol's "drain"
postcondition (protocol §"Step 1 — Drain"). Physics's
`glibre_plugin_drain` body therefore has nothing to flush from the
solver side; its work is the snapshot capture + worker-pool teardown
described in §8.3.

**Mid-frame reload is refused.** Any reload request that arrives
during phases 1–7 is queued, never applied; the loader's
`pending_reloads` counter is consumed only at phase 8 entry per
protocol step 1. Inside phase 8, physics does not yield to phase 3
or to query consumers — the loader holds exclusive ownership for the
duration of drain → swap → migrate → resume per protocol
§"Decision". A request that would force phase 3 to observe a
partially-swapped Jolt vtable is treated as a contract violation by
the loader, not a refusal — physics's spec contributes no new
refusal arm here, but states the invariant explicitly so consumers
cannot expect mid-frame swap semantics. The trigger contemplated by
§11 acceptance — a physics-plugin `.dylib` swap at frame 8 — is the
only legal entry path.

### 8.2 Survival inventory

The engine-wide survival rule is mechanical: **state with a `.fory`
schema in `glibre-types.dylib` survives across the swap; state
without one does not** (protocol §"State Survival Rules"; PHILOSOPHY
collapse: one check, not a per-aggregate manifest). Physics owns
four persistent fory-schema'd records (§7.1.1–§7.1.4) and a large
collection of host-side runtime state owned either by `physics`
itself or by `JoltMiddleman` (§4.1.13). The table below classifies
every physics aggregate against that rule and records the
physics-specific reasoning for each survival decision.

| Physics-owned state                                                                                  | Persistence path             | Survives swap? | Reasoning                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
|------------------------------------------------------------------------------------------------------|------------------------------|----------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `PhysicsConfigRecord` (§7.1.1) — frozen deterministic configuration                                  | `.fory` schema, middleman    | Yes — bytes are owned by `glibre-types`; physics only reads them. Per §7.2.1 a schema bump flows through the standard additive-defaulted-field migration. Physics's `migrate(...)` does not touch these bytes; the live `PhysicsConfig.content_hash` is recomputed by the new plugin from the surviving record (§4.1.2 invariant 2) and matched against the snapshot's `physics_config_hash` (§7.1.4 invariant 3). Mismatch is a refusal, not a silent re-derivation.                                                            |
| `ShapeBlobRecord` (§7.1.2) — cooked collision-geometry payload                                       | `.fory` schema, middleman    | Yes — blobs are content-addressed (§4.1.6 invariant 1); the shape table's keys are the immutable `content_hash` values, and the bytes themselves live in the asset bundle owned by `data` / `content` (§3.3). The new plugin re-resolves Jolt `Shape*` pointers from the surviving blobs in `register`; no re-cook happens at reload.                                                                                                                                                                                          |
| `JointDescriptorRecord` (§7.1.3) — constraint topology + tuning                                      | `.fory` schema, middleman    | Yes — joint-record bytes (kind, endpoints, anchors, limits, motor, break threshold) survive verbatim; the new plugin rebuilds Jolt `Constraint` objects from the descriptors during `register` (§8.3.2). Per §7.2.3 a schema bump flows through standard additive-sealed-sum migration; physics's `migrate(...)` body for this record is the §7.2.3 path, not the snapshot path.                                                                                                                                                |
| `PhysicsSnapshot` (§7.1.4) — deterministic replay artefact                                           | `.fory` schema, middleman    | **Yes — and it is the carrier for every per-body / per-joint runtime byte that crosses the swap.** §8.3 below specifies that the outgoing plugin captures one `PhysicsSnapshot` at drain time, the loader keeps the bytes in its phase-8 arena, and the incoming plugin restores them inside `register`. This is the only state carrier the physics protocol uses for runtime body / joint state — a deliberate single seam (§3.2 collapse #9, PHILOSOPHY §7).                                                                  |
| `RigidBody` ECS component (§4.1.5) — `MotionType`, mass, inertia, damping, `BodyId`, CCD bit         | Middleman ECS storage        | Yes — the component bytes are middleman-typed (`glibre.types.physics.RigidBody`) and live in core's archetype storage; the loader owns them. The runtime `Velocity` / `AngularVelocity` companion components survive the same way. Their values are re-applied to Jolt's body table during the snapshot restore in §8.3 (the snapshot is the single carrier; the ECS bytes are the surviving anchor that snapshot rows key to).                                                                                                  |
| `BodyId` (§4.1.5b) — stable handle into Jolt's body table                                            | Middleman value type          | Yes — **stability across reload is a §4.2 invariant 5 promise**. Allocation order is fixed by ECS body insertion order, not by Jolt's allocator address; the new plugin re-inserts bodies in the same order during `register` and the deterministic allocator yields the same 32-bit handle for the same ECS entity. Persisted snapshots that key on `BodyId` re-bind without payload rewrite (§4.1.12 invariant 2). A reload that does **not** reproduce the same `BodyId` for a given entity is a contract violation, not a refusal — the loader terminates per protocol §"Failure & Rollback". |
| `Collider` ECS component (§4.1.6) — `ShapeHandle`, offset, layer, material, trigger flag             | Middleman ECS storage        | Yes — component bytes survive in middleman storage. The held `ShapeHandle` is re-resolved against the surviving shape table during `register` (§8.3.2); the resolved Jolt `Shape*` pointer that lives inside the handle is **not** part of the middleman ABI (§4.1.13 invariant 1) and is rebuilt by the new plugin.                                                                                                                                                                                                          |
| `ShapeHandle` resolved Jolt `Shape*` pointer                                                          | None (in-process)             | No — pointers into Jolt's `Shape` table belong to the outgoing plugin's image. The new plugin rebuilds the shape table from the surviving `ShapeBlobRecord` content hashes; refcount survives because it is middleman-typed.                                                                                                                                                                                                                                                                                                  |
| `Joint` ECS entity + `Joint` / `JointLimits` / `JointMotor` / `JointBreakThreshold` components       | Middleman ECS storage        | Yes — the joint-as-entity model (§4.1.7 invariant 1) means the joint's components are in middleman storage and the entity itself is owned by core. The Jolt `ConstraintRef` held by the `Joint` component is **not** a middleman type — it is rebuilt during `register` by replaying `add_joint(...)` for every surviving joint entity in deterministic ECS order, which mirrors the post-snapshot `joint_normal_impulses` / `joint_friction_impulses` columns (§7.1.4 schema).                                                |
| `ContactManifold` per-pair payload (§4.1.8)                                                          | None (transient)              | No — manifolds are reborn each substep (§4.1.8 identity & lifetime). The pre-swap manifolds for frame N's last substep are written into the per-frame ECS event buffer and are reclaimed at end of phase 8 alongside the contact / trigger / joint-broken events. Physics's drain explicitly clears that buffer so the new plugin's first frame N+1 step starts with no stale pair history (§8.3); pairs that should be reported for frame N+1 will fire fresh `CollisionStarted` events at the next substep exit.            |
| `CollisionStarted` / `CollisionPersisted` / `CollisionEnded` event buffers                           | Middleman ECS event storage   | No — drained as part of the manifold drain above. The buffer's storage is middleman-owned, but its **contents** are not; clearing the buffer is a per-frame ECS operation that physics performs as the closing act of `glibre_plugin_drain` (§8.3). Subscribers in phases 5+ of frame N have already read them; reclamation at end of phase 8 is canonical (§4.2 invariant 8). The new plugin re-emits started events for any pair whose pre-swap state was `Persisted` because Jolt's contact listener treats the pair as fresh on reload. |
| `TriggerEnter` / `TriggerStay` / `TriggerExit` event buffers                                          | Middleman ECS event storage   | No — same reasoning as contact events. Drained on swap; the new plugin re-emits `TriggerEnter` for every overlapping trigger pair at the first post-reload substep exit. (Subscribers must be prepared for one extra `TriggerEnter` per pre-existing overlap; this is the only behavioural difference observable across reload, surfaced via the `PhysicsWorldReplaced` event in §8.5.)                                                                                                                                       |
| `JointBrokenEvent` buffer                                                                            | Middleman ECS event storage   | No — drained on swap. A joint that broke in frame N's substep is reflected in the surviving entity having been despawned; no re-emit is required because the entity is already gone (§4.1.7 invariant 1).                                                                                                                                                                                                                                                                                                                     |
| `Sleeping` / `Island` markers (§4.1.14)                                                              | Middleman ECS storage         | Yes for `Sleeping` (the marker bit travels with the body's middleman-typed `RigidBody` row); `Island` ids are recomputed by Jolt every substep so "survival" is moot. The snapshot carries `body_sleep_frames` + `body_sleeping_flags` (§7.1.4 schema), so the new plugin restores sleep state byte-for-byte.                                                                                                                                                                                                                  |
| `Accumulator` (§4.1.3) — fixed-timestep clock                                                        | Middleman value object        | Yes — `accumulator_carry` is a `f32` field on `PhysicsSnapshot` (§7.1.4 schema tag 4); restored verbatim. `world_tick` (tag 3) survives the same way. The Jolt-internal sub-tick state is reset to a fresh start because the deterministic-stepping math is stateless across substeps (§4.1.4 invariants 1 + 4).                                                                                                                                                                                                                |
| `PhysicsConfig` runtime view (§4.1.2)                                                                 | Derived from `PhysicsConfigRecord` | Yes by re-derivation. The new plugin reconstructs the runtime view from the surviving `PhysicsConfigRecord` bytes during `register`; the `content_hash` is recomputed and asserted byte-equal to the snapshot's `physics_config_hash` (§7.1.4 invariant 3) before any body is restored.                                                                                                                                                                                                                                       |
| Internal Jolt `PhysicsSystem`, `BodyManager`, `BroadPhase`, `NarrowPhase`, `ContactConstraintManager`, `IslandBuilder`, temporary allocators | None (in-process)             | No — these are private to the outgoing plugin's image. Drained via the snapshot capture, then reconstructed from scratch by the new plugin's `register` against the same `PhysicsConfig` budgets. Allocator addresses **never** appear in the snapshot (§4.1.12 invariant 3), so the rebuild does not perturb determinism.                                                                                                                                                                                                    |
| Per-plugin worker thread pool (Jolt's `JobSystem`)                                                   | None                          | No — destroyed by `glibre_plugin_drain`, re-spawned by `glibre_plugin_register` against the budgets in `PhysicsConfig` (§4.1.2). Worker count and seeded scheduling order are deterministic per `PhysicsConfig.content_hash` (PHILOSOPHY §7); a reload that re-derives identical workers yields identical task ordering.                                                                                                                                                                                                       |
| `PhysicsQueries` surface (§4.1.10)                                                                   | None — declarative only       | N/A — the query surface is a function table; the **table** survives because it is middleman-typed, but the function pointers are repointed during `register` per the protocol's vtable swap (protocol §"Step 2 — Swap"). No query state crosses frames (§4.1.10 invariant 1).                                                                                                                                                                                                                                                |
| `JoltMiddleman` itself (§4.1.13)                                                                     | None — process-resident dylib | Yes — one per process, loaded at engine init, unloaded at engine teardown (§4.1.13 identity & lifetime). It is the gate-keeper, not a payload, and physics-plugin reload is **refused** on hash mismatch (§4.1.13 invariant 2 + §8.4 below). Its bytes never change during a physics reload.                                                                                                                                                                                                                                  |

The rule mechanically applied: every row marked "Yes" has either a
`.fory` schema or is a middleman-typed ECS component / value object;
every "No" row is private to the outgoing plugin's image with no
on-disk format and is reconstructed by the incoming plugin from
surviving state — exactly what PHILOSOPHY §3 + protocol §"State
Survival Rules" require. The single carrier across the swap that
combines surviving ECS bytes with the live Jolt residue (impulses,
sleep counters, accumulator carry) is **`PhysicsSnapshot`**, the
fory-serialised type already specified at §4.1.12 / §7.1.4 — physics
introduces no new on-the-wire format for hot-reload (§3.2 collapse #9).

### 8.3 `migrate(...)` body — physics's responsibilities

The protocol's `migrate` step (protocol §"Step 3 — Migrate") runs
*pure* per-row migrate functions for every persistent-component-type
schema bump on the engine's behalf. Physics owns four of those
bodies: `PhysicsConfigRecord` (§7.2.1), `ShapeBlobRecord` (§7.2.2),
`JointDescriptorRecord` (§7.2.3), and `PhysicsSnapshot` (§7.2.4). The
function signatures are the standard pure migrate signature
(`reviews/decisions/hot-reload-protocol.md` §"Migrate Function
Contract"); nothing here changes them.

What this section adds is the **physics-plugin-specific portion of
steps 1 + 4 (drain + resume)** — the work the outgoing plugin's
`glibre_plugin_drain` and the incoming plugin's `glibre_plugin_register`
must do to capture every Jolt-internal byte that the surviving
`.fory`-typed bytes do not already cover, transport it across the
swap inside a `PhysicsSnapshot`, and rebuild Jolt's body table /
shape table / joint constraint graph against the same deterministic
inputs. Three operations matter; they happen in the order listed.

#### 8.3.1 Drain — capture `PhysicsSnapshot`, drain event buffers, tear down workers

The outgoing plugin's `glibre_plugin_drain` body, called by the
loader at the start of phase 8, performs three steps in order:

1. **Capture a `PhysicsSnapshot` of the live world.** Calls
   `PhysicsWorld::snapshot()` (§5, §4.1.12). The snapshot is the
   §7.1.4 schema: header (`schema_version`, `physics_config_hash`,
   `world_tick`, `accumulator_carry`, `middleman_abi_hash`) plus
   parallel-list body and joint columns (`body_ids`,
   `body_motion_types`, `body_positions`, `body_rotations`,
   `body_linear_velocities`, `body_angular_velocities`,
   `body_sleep_frames`, `body_sleeping_flags`,
   `body_shape_blob_hashes`, `joint_ids`, `joint_kinds`,
   `joint_body_a`, `joint_body_b`, `joint_normal_impulses`,
   `joint_friction_impulses`). Per §7.1.4 invariant 5, columns are
   parallel-list; per §4.1.12 invariant 3, iteration is
   `BodyId`-ascending and `JointId`-ascending so the bytes are
   byte-equal across hosts. The serialised buffer (`to_bytes()`) is
   handed to the loader's per-phase migration arena
   (`reviews/decisions/hot-reload-protocol.md` §Consequences); the
   arena keeps it alive through swap + migrate + resume.
2. **Drain event buffers and ContactManifold residue.** Empties the
   per-frame ECS event buffers for `CollisionStarted` /
   `CollisionPersisted` / `CollisionEnded` / `TriggerEnter` /
   `TriggerStay` / `TriggerExit` / `JointBrokenEvent`. By §4.2
   invariant 8 those buffers are cleared at end of phase 8 in
   normal operation; the drain step performs that clearance one
   beat earlier so no stale event payload from the outgoing
   plugin's coordinate space leaks into the incoming plugin's first
   substep. The drained events are not re-emitted — phases 5+ of
   frame N have already read them, and frame N+1's first substep
   will produce fresh `CollisionStarted` / `TriggerEnter` events
   for any persisting overlap (the incoming Jolt instance has no
   pair history; §8.2 row "TriggerEnter").
3. **Release Jolt-internal allocations and tear down the worker
   pool.** Frees Jolt's `PhysicsSystem`, `BodyManager`, broadphase /
   narrowphase / island / contact-constraint managers, and the
   `JobSystem` workers. The temporary allocator is reset; no Jolt
   pointer outlives drain. The middleman-typed `RigidBody` /
   `Collider` / `Joint` / `Sleeping` / `Velocity` / `AngularVelocity`
   ECS storages are untouched — they survive in core's archetype
   storage by §8.2.

After drain, the loader runs the protocol's swap step (vtable
replacement + type-registry append; protocol §"Step 2 — Swap") and
then the migrate step (per-record schema-version chains; protocol
§"Step 3 — Migrate"). Neither step touches Jolt or the snapshot bytes
themselves — they touch only the surviving `.fory`-typed records'
schema versions if any have bumped.

#### 8.3.2 Resume — rebuild PhysicsWorld + restore PhysicsSnapshot

The incoming plugin's `glibre_plugin_register` body, called by the
loader at step 4 of the protocol, performs five steps in order:

1. **Verify `JoltMiddleman` ABI hash.** Calls `JoltMiddleman::
   require_hash(host_abi_hash)` per §4.1.13 invariant 2. Mismatch
   short-circuits to refusal (§8.4 row 1). On success, the new
   plugin's symbol table now resolves every Jolt-derived public type
   through the same middleman the outgoing plugin used.
2. **Reconstruct `PhysicsWorld` from the surviving
   `PhysicsConfigRecord`.** Calls
   `PhysicsWorld::create(world, PhysicsConfig{record})` (§5).
   `PhysicsWorld::create` builds a fresh Jolt `PhysicsSystem`, sizes
   broadphase + body / contact pools from the config's caps, spawns
   the worker pool, and constructs the empty body / joint / shape
   tables. The new world's `PhysicsConfig.content_hash` is computed
   and compared against `PhysicsSnapshot.physics_config_hash`
   (header tag 2); mismatch returns
   `physics::Error::SnapshotConfigDrift` (§7.1.4 invariant 3),
   which routes to refusal §8.4 row 4.
3. **Re-intern surviving `ShapeBlobRecord`s into the shape table.**
   Walks every distinct `content_hash` referenced from
   `body_shape_blob_hashes` (snapshot tag 14) plus any
   `Collider::shape_blob_hash` cited on a surviving `Collider`
   component, calls `PhysicsWorld::intern_shape(...)` for each, and
   re-binds the resolved Jolt `Shape*` pointer back into the
   surviving `ShapeHandle`. The handle's refcount survives because
   it is middleman-typed (§8.2 row "Collider"); only the resolved
   pointer is rebuilt. Pinning is read-only against the
   `ShapeBlobRecord` table; a missing hash returns
   `physics::Error::ShapeBlobMissing` and routes to refusal §8.4
   row 4.
4. **Restore bodies + joints + accumulator from the snapshot.**
   Calls `PhysicsWorld::restore(snapshot)` (§5). The restore body
   walks `body_ids` (tag 6) in ascending order and calls
   `add_body(entity, RigidBody{...}, Collider{...})` for each;
   because allocation order is deterministic (§4.1.5 invariant 1),
   the allocator yields the same `BodyId` for the same ECS entity
   that owned it before the swap (§4.2 invariant 5). Mass / inertia
   / motion-type / damping come from the surviving middleman-typed
   `RigidBody` ECS row; position / rotation / linear-velocity /
   angular-velocity / sleep-frames / sleeping-flag come from the
   snapshot columns (tags 8–13). After every body is in place, the
   loader walks `joint_ids` (tag 15) in ascending order and calls
   `add_joint(...)` for each, sourcing kind + endpoints + anchors
   from the surviving `JointDescriptorRecord` and re-applying the
   accumulated `joint_normal_impulses` / `joint_friction_impulses`
   (tags 19–20) so the next substep's warm-start is byte-equal to
   what the outgoing plugin would have produced (§4.2 invariant 3).
   The `Accumulator` is reseeded from `accumulator_carry` (tag 4).
5. **Re-link triggers, contact listener, and query surface.** The
   incoming plugin re-installs its `ContactListener` adapter
   (§4.1.8) into Jolt's `ContactConstraintManager`, walks every
   surviving `Collider` component carrying the `Trigger` marker
   (§4.1.9) and registers it with Jolt's overlap-only contact path,
   and re-publishes the `PhysicsQueries` function pointers into the
   query-surface registry. None of this rebuilds frame state —
   triggers fire fresh `TriggerEnter` events at the next substep
   exit per §8.2.

The total work in physics's resume step is bounded by **O(bodies)
`add_body` calls + O(joints) `add_joint` calls + O(unique shape
blobs) `intern_shape` calls + O(1) world / allocator / worker-pool
rebuild + a single `PhysicsSnapshot::from_bytes` decode**, fitting
the protocol's "reload path bounded by drain + swap + Σ migrate +
register" budget (`hot-reload-protocol.md` §Consequences). A
deterministic snapshot taken before drain and a snapshot taken after
resume on the same world tick are byte-equal (§4.2 invariant 3,
§7.1.4 invariant 1) — the round-trip is the canonical correctness
proof and the §8.6 acceptance test.

### 8.4 Refusal cases (physics-specific)

Physics contributes no new umbrella refusal arm; every refusal is
expressed as the engine-wide `core::Error::HotReloadRefused` with a
nested cause chosen from the protocol's existing arms (protocol
§"Refusal Cases"). Physics does introduce four **inner cases** that
the loader sees because physics inspects the new plugin's
middleman / snapshot / shape / joint state during steps 2–4; they
are enumerated here so the test matrix and the diagnostic surface
can name them. All four roll up to `core::Error::PluginAbiHashMismatch`,
`core::Error::SchemaMigrationFailed`, or `core::Error::PluginInitFailed`
per protocol §"Refusal Cases".

| Physics refusal cause                                                                                              | Detected by                                                                                                                                                       | Inner-error arm                                                                                                                          | What the operator must do                                                                                                                                                                                                                                                                                          |
|--------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| New plugin's `JoltMiddleman` ABI hash differs from the host's `glibre_types_abi_hash` for the physics-typed surface | Protocol step 2.1 (`Q::glibre_types_abi_hash() == host_glibre_types_abi_hash`); also re-asserted by physics's `register` step 1 (`JoltMiddleman::require_hash`).   | `core::Error::HotReloadRefused` with cause `core::Error::PluginAbiHashMismatch`. Maps to `physics::Error::JoltMiddlemanHashMismatch` at the physics surface (§5 enum). | Rebuild the physics plugin against the current `JoltMiddleman` (i.e. rebuild against the same Jolt version + type list the host shipped). The previously-loaded physics plugin keeps stepping; this is the PHILOSOPHY §9 case. (§4.1.13 invariant 2.)                                                              |
| `PhysicsSnapshot.schema_version` greater than the new plugin's reader-side current version, with no migration chain | Protocol step 3.1 (`glibre-types.dylib` migration table query for `PhysicsSnapshot`). Detected before any byte is overwritten in the surviving storage.            | `core::Error::HotReloadRefused` with cause `core::Error::SchemaMigrationFailed`. Maps to `physics::Error::HotReloadStateUnmigratable` at the physics surface (§5 enum).                | Author the missing `vN → vN+1` migration body under `src/physics/migrations/` per §7.2.4, ship it in the new plugin, and re-trigger the swap. Alternatively restore from a snapshot taken at the prior schema version. The migration arena is reset before the cause is returned (no partial bytes published).    |
| `PhysicsSnapshot.physics_config_hash` mismatches the new world's recomputed `PhysicsConfig.content_hash`            | Physics's `register` step 2 (snapshot restore precondition; §7.1.4 invariant 3).                                                                                  | `core::Error::HotReloadRefused` with cause `core::Error::PluginInitFailed` carrying inner `physics::Error::SnapshotSchemaMismatch`.       | Either revert the `PhysicsConfigRecord` change (a `PhysicsConfig` change is a fresh-world event by §4.1.2 invariant 2; not a hot-reload event), or restore from a snapshot whose hash matches the new config. Cross-config restore is forbidden because `PhysicsConfig` decides what counts as deterministic.      |
| `PhysicsSnapshot` references a `body_shape_blob_hashes` entry that no longer resolves in the surviving `ShapeBlobRecord` table | Physics's `register` step 3 (`PhysicsWorld::intern_shape` against the missing hash). Also the `Collider`-bearing-entity case if a surviving collider's shape was dropped. | `core::Error::HotReloadRefused` with cause `core::Error::PluginInitFailed` carrying inner `physics::Error::ShapeBlobMissing`.             | Restore the dropped `ShapeBlobRecord` (asset-pipeline drift between cook output and the running world), or re-cook the affected body's shape, or accept a fresh world. The reload is refused; the prior physics plugin remains live.                                                                                |

Each refusal is logged exactly once at `warn` level (protocol
§"Refusal Cases") with the structured fields
`plugin_fqn=glibre.physics`, `attempted_dylib_path`, `host_abi_hash`,
`plugin_abi_hash`, the inner cause's enumerator name, and (for
snapshot-related causes) the snapshot's `world_tick` so the
diagnostic surface can correlate the refusal with the determinism
gate's trace corpus. The previously-loaded physics plugin keeps
stepping; phase 3 of frame N+1 dispatches through the same vtable
it used in frame N. No body / collider / joint state is mutated by a
refusal.

### 8.5 Observers — `PhysicsWorldReplaced` and render BLAS invalidation

Physics adds **one** plugin-specific event arm to the engine
observer bus (the same bus that carries `HotReloadStarted` /
`HotReloadCompleted` per protocol §"Observer Notification"):

```
PhysicsWorldReplaced {
    world_tick:           u64,    // §4.1.12 / §7.1.4 header tag 3
    bodies_restored:      u32,    // count of body_ids in the carrier snapshot
    joints_restored:      u32,    // count of joint_ids in the carrier snapshot
    triggers_relinked:    u32,    // count of Collider components carrying the Trigger marker
    middleman_abi_hash:   u64,    // §4.1.13 — equal across old + new by step 2.1
    snapshot_byte_size:   u64,    // size of the carrier snapshot in the migration arena
}
```

The event is middleman-typed (`glibre::types::physics::HotReloadEvent`,
arm `PhysicsWorldReplaced`) so its layout survives any future
core-runtime reload. It is published synchronously by the loader
between protocol steps 4.2 and 4.3 — i.e. after the new plugin has
restored bodies + joints + triggers and rebuilt its caches but
before phase 9 begins, matching the engine bus's atomicity guarantee
(protocol §"Observer Notification"). Subscribers see a fully-swapped,
fully-migrated world; they never observe a half-restored state.

The required subscribers and their reactions:

1. **`render` plugin — BLAS invalidation for static bodies.** Render
   imports per-body BLAS handles for static `RigidBody` entities via
   the `RTAccelStructures` aggregate (render SPEC §4.1.8); those
   imports are read-only by render but their **identity** depends on
   the static body's position / rotation matching what render
   recorded at its last extract. After `PhysicsWorldReplaced`,
   render must mark every BLAS for a body whose `BodyId` appears in
   the carrier snapshot as **invalidated for re-build at the next
   frame's phase 6** (render SPEC §4.1.8 invariant 3 forbids
   silently reusing a BLAS whose source geometry's transform
   changed). The trigger is mechanical: the render-side subscriber
   walks the surviving static-body archetype, compares each body's
   position / rotation against the BLAS's recorded transform, and
   posts the BLAS to its rebuild queue if they differ. In the
   round-trip identity case (§8.6 happy-path), all transforms are
   byte-equal and zero BLAS entries are queued — the invalidation
   is idempotent.
2. **`e2e` harness — trace-replay anchor.** The harness consumes
   `PhysicsWorldReplaced` to mark the post-swap snapshot capture
   point. The byte-equal acceptance test (§8.6) keys off the
   `world_tick` and `snapshot_byte_size` fields.
3. **`tools` profiler / `editor` (post-MVP)** — diagnostic display
   of the swap. Optional; not in MVP.

No half-swapped world is ever observable: the bus's atomicity
guarantee (protocol §"Observer Notification") means subscribers see
either a pre-swap-fully-completed-world or a post-swap-fully-restored
world. Subscribers that hold long-lived `PhysicsSnapshot` references
across reload (only the e2e harness, in MVP) keep them by value — the
snapshot is a `unique_ptr`-owned standalone byte buffer (§5
`PhysicsSnapshot::from_bytes`), not a pointer into Jolt's tables, so
the swap does not invalidate them.

No second event arm is permitted; new observability needs flow into
`PhysicsWorldReplaced` field additions or graduate to a SPEC bump
(§3.2 collapse #5 — single physics event family).

### 8.6 Test hooks — deterministic snapshot byte-equal verification

Physics's reload contract is verified end-to-end by a single test
fixture under `tests/physics/hot_reload/` that uses the loader's
existing `enqueue_hot_reload` E2E entry point (protocol §"Test
Hooks"). The fixture has three layers, each producing one Catch2
test case listed in §11 acceptance criteria.

1. **Trace capture.** A fixture plugin
   `tests/e2e/plugins/physics-v1/` runs the engine for `K = 16`
   frames against a deterministic scene (50 dynamic boxes stacked
   into a tower, 5 distance joints binding a ragdoll, 2 trigger
   volumes; fixed PRNG seed; `PhysicsConfig` from the canonical
   §4.1.2 fixture). At frame 7 (one before the swap) the harness
   captures `snapshot_pre = PhysicsWorld::snapshot()` and stores its
   byte form under `tests/data/snapshots/physics/cornell-tower-pre.
   fory.bin`. The harness records, per frame for `K` frames, the
   structured trace `(world_tick, BodyId-keyed positions, contact-
   event list, trigger-event list, JointBroken list)`; the trace is
   canonical-ordered (`reviews/decisions/determinism-canonical-
   iteration.md`) and stored at `tests/data/traces/physics/cornell-
   tower-vN.bin`.
2. **Reload trigger at frame 8.** At frame 8 (matching §11
   acceptance "trigger: physics-plugin .dylib swap at frame 8"),
   the harness calls `enqueue_hot_reload("glibre.physics",
   tests/e2e/plugins/physics-v2.dylib)`. The `v2` plugin is
   byte-identical to `v1` for the happy-path test (same Jolt
   version, same `JoltMiddleman` ABI hash, same migration table)
   but has a different `__file__` timestamp embedded so the loader
   treats it as a real swap. This exercises the **identity
   round-trip**: a swap that is semantically a no-op.
3. **Post-reload assertion.** The harness records a second trace
   for frames `8..K-1` under the new plugin and asserts:
   - `snapshot_pre.to_bytes()` (captured before swap) is byte-equal
     to `PhysicsWorld::snapshot().to_bytes()` captured on the new
     plugin at the same `world_tick = 8` (the first instruction
     the new plugin executes inside `register` after `restore`
     returns). This is the **byte-equal verification across reload**
     (§11 acceptance);
   - the per-frame trace for frames `9..K-1` is byte-equal to the
     pre-swap reference trace at the same indices (post-reload
     determinism is preserved, §4.2 invariant 5);
   - the `HotReloadCompleted` event was fired exactly once with
     `migrated_types = []` (no schema change in this scenario);
   - the `PhysicsWorldReplaced` event reports
     `bodies_restored == 50`, `joints_restored == 5`,
     `triggers_relinked == 2`, `middleman_abi_hash` unchanged;
   - the contact-event buffer was empty at the start of frame 9
     (drain reclaimed every pre-swap manifold; §8.3.1 step 2);
   - `TriggerEnter` events fired exactly once for each of the 2
     overlapping trigger pairs at frame 9's first substep exit
     (the re-emit case in §8.2 row "TriggerEnter");
   - render's BLAS rebuild queue length on the
     `PhysicsWorldReplaced` callback was zero (transforms
     byte-equal in the identity round-trip).

A second fixture pair (`physics-v1` → `physics-v2-bad-abi`)
exercises the ABI refusal path: `v2-bad-abi` is built against a
different `JoltMiddleman` hash. The post-reload trace asserts
`HotReloadRefused { cause: PluginAbiHashMismatch }` with the prior
plugin still ticking (frames `8..K-1` byte-equal to the pre-swap
reference trace).

A third fixture (`physics-v1-snapshot-vN` → `physics-v2-snapshot-vN+1`)
exercises the schema-migration refusal path: `v2` declares
`PhysicsSnapshot.schema_version = N+1` with no `vN → vN+1`
migration body shipped. The harness asserts
`HotReloadRefused { cause: SchemaMigrationFailed }` mapping to
`physics::Error::HotReloadStateUnmigratable` (§5 enum), with the
prior plugin still ticking. A companion case ships the migration
body and asserts the round-trip continues to byte-equal under the
migrated schema.

A fourth fixture (`physics-v1` → `physics-v2-good-abi`, but with
the `physics_config_hash` changed in the new plugin's record)
exercises the config-drift refusal: the harness asserts
`HotReloadRefused { cause: PluginInitFailed { inner:
SnapshotSchemaMismatch } }` and that the prior plugin keeps ticking.

All four scenarios run inside a single CI job using the in-process
trigger; no filesystem watcher is involved (protocol §"Test Hooks").
The Catch2 cases are listed in §11 acceptance criteria as
`Hot-reload preserves snapshot byte-equal`,
`Hot-reload refuses ABI hash mismatch`,
`Hot-reload refuses schema migration without body`,
`Hot-reload refuses physics-config drift`.

### 8.7 Cross-references

- Engine protocol: `reviews/decisions/hot-reload-protocol.md`
  (drain → swap → migrate → resume; refusal arms; observer bus;
  E2E hook).
- Frame slot: `reviews/decisions/frame-phases.md` (phase 8 entry /
  exit guarantees; phase 3 single-owner).
- Pilot specialisation: `specs/render/SPEC.md` §8 (the render-side
  template this section follows; inter-plugin observability via
  `PhysicsWorldReplaced` ↔ render BLAS invalidation is consistent
  with render's `RenderFrameDropPending` shape).
- Persistence rules invoked: §7.1.1 / §7.2.1
  (`PhysicsConfigRecord`), §7.1.2 / §7.2.2 (`ShapeBlobRecord`),
  §7.1.3 / §7.2.3 (`JointDescriptorRecord`), §7.1.4 / §7.2.4
  (`PhysicsSnapshot`).
- Aggregates touched: §4.1.1 `PhysicsWorld` (root rebuild),
  §4.1.2 `PhysicsConfig` (re-derived), §4.1.3 `Accumulator`
  (carry restored), §4.1.5 / §4.1.5b `RigidBody` / `BodyId`
  (snapshot restore + stable allocation), §4.1.6 `Collider` /
  `ShapeHandle` (re-intern), §4.1.7 `Joint` (topology rebuild),
  §4.1.8 `ContactManifold` (drained), §4.1.9 `Trigger` (re-linked),
  §4.1.10 `PhysicsQueries` (vtable repointed), §4.1.12
  `PhysicsSnapshot` (the carrier), §4.1.13 `JoltMiddleman` (ABI
  gate), §4.1.14 `Sleeping` / `Island` (sleep state restored),
  §4.2 invariants 1, 2, 3, 5, 7, 8 (phase-3 ownership, mirror
  one-way, determinism, BodyId stability, single Jolt seam,
  same-frame events).
- Errors used: `physics::Error::JoltMiddlemanHashMismatch`,
  `physics::Error::HotReloadStateUnmigratable`,
  `physics::Error::SnapshotSchemaMismatch`,
  `physics::Error::ShapeBlobMissing` (§5 enum), each wrapped by
  `core::Error::HotReloadRefused` per protocol §"Refusal Cases".

## 9. Performance Budget

This section quotes physics's row from the engine-wide budget locked
in `reviews/decisions/perf-budget.md`, decomposes the **2.00 ms CPU
sim** budget across the three phase-3 stages (Jolt step,
ECS↔Jolt mirror, queries), composes the **128 MiB heap** ceiling
across the five resident pools physics keeps, books the
determinism-cost premium that the §6.4 fixed-iteration-order rule
imposes, and lists the CI gate hooks physics owns. Every number is
a **contractual ceiling**, not a steady-state expectation — the
budget gate fails on any frame that exceeds the cell, any phase-3
stage slice (§9.3), or any heap pool sub-share (§9.4). The §11
acceptance criteria name the Catch2 benchmarks that enforce these
ceilings.

### 9.1 Cell — physics row

Physics's cell from `reviews/decisions/perf-budget.md` §"Per-Context
Budget Table", restated verbatim:

| Axis              | Budget       | Source                                                    |
|-------------------|--------------|-----------------------------------------------------------|
| CPU (sim)         | **2.00 ms**  | `perf-budget.md` Per-Context Budget Table — physics row   |
| CPU (submit)      | 0.00 ms      | physics records no GPU work; submit half is empty         |
| GPU               | n/a          | physics owns no Metal heaps or encoders (§3.3)            |
| Heap ceiling      | **128 MiB**  | `perf-budget.md` Per-Context Budget Table — physics row   |
| Phase ownership   | **3**        | `frame-phases.md` — `physics-fixed`; sole owner           |
| Phase systems     | none         | physics registers no systems into other phases            |

Physics is unique among MVP contexts in owning **exactly one phase
in full** (§4.1.1 invariant 1; §6.2). The 2.00 ms cell is the
entirety of physics's per-frame CPU draw on the game-loop driver
thread; no slice runs in phases 1, 2, 4, 5, 6, 7, 8, or 9. Submit
is 0.00 ms because phase 3 finishes before phase 6's `RenderFrame`
extract begins, and physics writes no GPU resources of its own —
render reads body transforms from the post-phase-5
`GlobalTransform` ECS column (§6.5 seam #1), not from a
physics-side encoder. The 128 MiB heap row is the resident
allocation under `ContextTag::physics`; it does not include
transient phase-3 arena usage, which drains by phase-9 per
`perf-budget.md` Allocator Rule #4.

The 2.00 ms cell carries headroom for one **sub-stepped frame**
(`Accumulator` ran two substeps because the prior frame slipped
~17 ms, §4.1.3 invariant 2). Steady-state at 60 Hz is one
substep/frame for ~1.0 ms; the cell budgets two substeps/frame
because the substep cap is four (§4.1.3 invariant 2) and a
two-substep frame is the realistic upper bound under the S1
fixture. Three- or four-substep frames are clamp-event territory
(§4.1.3) and the gate flags them via the
`physics::Warning::AccumulatorClamped` log line, not via the
budget; surviving the cell ceiling under clamp is intentional —
clamp drops residual carry rather than blowing the deadline.

### 9.2 Per-stage sub-budget (CPU sim)

Sub-budgets refine the 2.00 ms cell across the three phase-3
stages that the §6.2 `world/phase3_driver.cpp` body runs. Each row
lists CPU ms (within the 2.00 ms ceiling), the §4 aggregate(s) the
row covers, and the §6.2 driver step the budget pays for.
Steady-state numbers assume the S1 fixture from `perf-budget.md`
§"Justification Per Cell" (1 character + 200 props + 8 dynamic
lights at 1920x1080); the M1 firestorm-at-3.2-GHz Jolt 2025
benchmark cited in `perf-budget.md` ("0.5–1.2 ms per substep")
is the floor that the row totals sum against.

| Stage                                         | §6.2 step | §4 aggregates                                                            | CPU ms  | Dominant operation                                                                            |
|-----------------------------------------------|-----------|--------------------------------------------------------------------------|---------|-----------------------------------------------------------------------------------------------|
| **Jolt step** (broadphase + solve + contacts) | 3         | §4.1.1 `PhysicsWorld`, §4.1.4 `Substep`, §4.1.13 `JoltMiddleman`         | **1.50**| One `JoltMiddleman::step(...)` call per substep — broadphase, narrowphase, constraint solve, contact resolution, integration; budgets two substeps/frame at the upper bound. |
| **ECS↔Jolt mirror** (entry + exit barriers)   | 2, 4      | §4.1.5 `RigidBody`, §4.1.5b `BodyId`, §4.1.7 `Joint`, §4.1.14 `Sleeping` | **0.30**| `BodyId`-sorted walk over the `RigidBody` archetype: ECS→Jolt commit (force/torque drain, kinematic overrides, motor targets) and Jolt→ECS commit (velocity/position/sleep/contact-event drain) per substep. |
| **Queries** (raycast / sweep / overlap)       | n/a       | §4.1.10 `PhysicsQueries`, §4.1.11 `BroadphaseLayer`                      | **0.20**| Reserve for `ray_cast` / `shape_cast` / `overlap` / `closest_point` issued from phase 1 (pre-step) and phases 5+ (post-step). Calls during phase 3 are refused (§4.1.10 invariant 4) and pay zero. |
| **Subtotal**                                  |           |                                                                          | **2.00**|                                                                                               |

Notes per row:

- **Jolt step (1.50 ms).** Two substeps × ~0.75 ms each at the upper
  bound; one substep at steady-state. Jolt's own broadphase quality
  determines this number more than any glibre-side code; the
  middleman's only knob inside this slice is the deterministic
  `JobSystemSingleThreaded` configuration (§6.4 rule 1) which is
  the determinism cost folded into the 1.50 ms — see §9.5.
- **ECS↔Jolt mirror (0.30 ms).** Two substeps × two barriers ×
  ~0.075 ms each. The barrier cost is dominated by the
  `std::ranges::sort` over the per-substep `BodyId` scratch span
  (§6.4 rule 2) for ~30 active bodies + 1 character at S1; SIMD
  scalar copies into the Jolt body table; the contact-listener
  drain into the per-frame ECS event buffers sorted by
  `(BodyId-low, BodyId-high)` (§4.1.8 invariant 1). The 200 props
  are mostly sleeping (§4.1.14) and skipped by the active-set
  walk; only the active set crosses the barrier.
- **Queries (0.20 ms).** Reserve. The S1 fixture has no specific
  query workload; gameplay raycasts / character-controller sweeps
  / pickup-overlap probes from the post-MVP scripting / animation
  contexts dock here. A query that would exceed 0.20 ms in one
  frame is a perf-budget amendment (e.g. spatial-AI bulk overlap),
  not a silent expansion of physics's cell. Calls during phase 3
  pay zero — they are refused with `physics::Error::QueryDuringStep`
  (§4.1.10 invariant 4) and never enter this row.

The §6.2 driver loop's per-substep accounting (`acc -= cfg.dt;
substeps_done += 1; world_tick += 1`) is below the precision floor
the gate measures and is not a separate row.

### 9.3 Heap composition inside the 128 MiB ceiling

The 128 MiB ceiling decomposes across the five resident pools
physics keeps. Sub-shares are advisory at the allocator level
(`PerContextAllocator` enforces the 128 MiB cell, not per-pool
caps; same model as `core` SPEC §9.4 rule 4) and contractual at
the gate level (§9.6 declares one heap-residency assertion per
pool). Numbers are sized against the MVP ceiling of ~1k bodies
from `perf-budget.md` physics-row justification.

| Pool                                | Sub-share  | §4 aggregates / §6.1 module                                | What it holds                                                                              |
|-------------------------------------|------------|------------------------------------------------------------|--------------------------------------------------------------------------------------------|
| **Jolt body + constraint pools**    | **64 MiB** | §4.1.1 `PhysicsWorld` (Jolt-internal), §4.1.7 `Joint`      | Jolt's `BodyManager`, `ContactConstraintManager`, broadphase grid, island graph, contact cache, joint-constraint pool. Sized for ~1k bodies + ~256 constraints (`PhysicsConfig` budgets, §4.1.2 composition). |
| **`ShapeBlob` hash table**          | **32 MiB** | §4.1.6 `ShapeBlob` / `ShapeHandle`; `shapes/shape_table.cpp` | Content-hash-keyed immutable shape blobs (heightfields, baked triangle meshes, primitives + compound shape graphs). Refcount metadata; the resident set covers the loaded scene. |
| **Snapshot scratch arena**          | **16 MiB** | §4.1.12 `PhysicsSnapshot`; `snapshot/`                     | Fory codec scratch + the carry buffer used by `PhysicsWorld::snapshot()` and the `glibre_plugin_drain` capture (§8.3.1). Drains between snapshot calls; sized for the MVP body cap. |
| **Query result buffers**            | **8 MiB**  | §4.1.10 `PhysicsQueries` (caller-arena fallback)           | Reserved fall-through for callers that omit a per-context arena and rely on physics's pool. Per §6.5 seam #2 callers SHOULD bring their own arena; this row is the safety pool for plugins still being ported. |
| **Contact event ring**              | **8 MiB**  | §4.1.8 `ContactManifold` / `ContactEvent`, §4.1.9 `Trigger` | Per-frame `CollisionStarted` / `CollisionPersisted` / `CollisionEnded` / `TriggerEnter` / `TriggerStay` / `TriggerExit` event buffers drained at substep exit (§6.2 step 4). Ring sized so the MVP ceiling of ~1k bodies cannot wrap. |
| **Subtotal**                        | **128 MiB**|                                                            |                                                                                            |

Pool composition rationale:

- **Jolt body + constraint pools = half the cell.** Jolt's resident
  data structures dominate physics memory; the 64 MiB allocation is
  the floor at which Jolt's `MaxBodies = 1024` + `MaxBodyPairs =
  4096` + `MaxContactConstraints = 4096` run without overflow on
  the M1 baseline (Jolt 2025 sizing guide).
- **`ShapeBlob` hash table = quarter cell.** Content-hashed
  immutability (§4.1.6 invariant 2) means the table grows once per
  unique shape and is shared across all bodies referencing the
  same hash; 32 MiB carries the MVP shape inventory (estimated
  ~256 unique shapes × ~128 KiB average meshlet storage).
- **Snapshot scratch is per-call, not per-frame.** The 16 MiB
  arena is sized so a snapshot can be captured mid-frame
  (debug-tools / e2e-harness use case) without going through the
  transient-arena exemption — snapshots are not a
  per-frame-deterministic cost and the budget treats them as
  resident.
- **Query result buffers = 8 MiB safety pool.** Callers per §6.5
  seam #2 are expected to provide their own arena; this pool
  exists so a plugin under porting (e.g. early gameplay scripts
  before they adopt the per-system command-buffer pattern) does
  not bust the cell. The pool is sized at 8 MiB so the gate
  surfaces over-reliance — if a plugin is consuming this pool
  routinely, the porting effort is incomplete.
- **Contact event ring = 8 MiB.** Ring buffer sized so the MVP
  ceiling of ~1k bodies cannot wrap within a single frame (worst
  case: 1k bodies × 6 event types × ~16 B per event ≈ 96 KiB; the
  remaining ~8 MiB headroom is post-MVP slack for higher contact
  densities under stacking scenarios that the §11 acceptance
  tests will measure). Drain timing is enforced by §6.2 step 4 and
  the §4.1.8 invariant 1 sort key.

Transient phase-3 arena usage (the `BodyId` scratch span behind
§6.4 rule 2; the per-substep contact-event marshalling buffer)
does **not** count against the 128 MiB ceiling — it is the
per-frame transient arena exemption from `perf-budget.md`
Allocator Rule #4, drained by phase 9. The arena is sized at 4
MiB and lives under `ContextTag::physics` for tracking purposes
only.

### 9.4 Allocator rules

Physics enforces its 128 MiB ceiling — and the per-pool sub-shares
in §9.3 — through `glibre::PerContextAllocator`, the allocator
declared in `perf-budget.md` §"Allocator Rules" and implemented
under the `core` allocator plan. The contract physics SPEC §9
imposes:

1. **Per-context tag (`ContextTag::physics`).** Every allocation
   made by any module under `physics/src/**` is stamped with
   `ContextTag::physics` at the allocator-handle level
   (`perf-budget.md` Allocator Rule #1). The tag is supplied by the
   handle physics's `glibre_plugin_register` (§8.3.2) obtains from
   `core`'s plugin loader; module call sites are tag-free.
2. **Hard ceiling in diagnostic / debug builds.** When
   `GLIBRE_ALLOC_STRICT=1`, an allocation that would push live
   `ContextTag::physics` bytes above 128 MiB returns
   `std::unexpected{core::Error::OutOfBudget}` (`perf-budget.md`
   Allocator Rule #2). Physics's call sites use the `Result<T>`
   form (§4.1.1 invariant 3) and propagate the error; failure to
   handle aborts with the diagnostic dump.
3. **Soft warning in shipping builds.** Shipping builds log a
   `warn` once per-tag-per-frame to `spdlog` and increment the
   frame-stat counter on overshoot (`perf-budget.md` Allocator
   Rule #3). The editor's perf HUD (tools context) surfaces the
   counter.
4. **Per-pool sub-shares are gate-enforced, not allocator-enforced.**
   `PerContextAllocator` enforces the 128 MiB cell ceiling, not the
   §9.3 per-pool ceilings; per-pool enforcement is via the
   `BENCHMARK_CELL` heap-residency assertions (§9.6) which exercise
   the S1 fixture and record the resident bytes per pool at frame
   end. This split keeps the runtime allocator path branch-free per
   pool while still catching drift on a CI cadence.
5. **Jolt's allocator is wrapped, not bypassed.** Jolt 2025 ships
   with an injectable `JPH::Allocate` / `JPH::Free` hook;
   `middleman/jolt_middleman.cpp` (§4.1.13, the only TU that
   includes Jolt headers) installs hooks that route every Jolt
   allocation through `PerContextAllocator` under
   `ContextTag::physics`. No raw `new` / `malloc` reaches the
   process allocator from inside Jolt's code. The
   `-Wglibre-no-raw-alloc` engine-wide compile flag covers
   physics's own sources; the Jolt hook covers the third-party
   half.
6. **Phase-3 transient arena drains by phase 9.** The per-substep
   `BodyId` scratch span (§6.4 rule 2) and the contact-event
   marshalling buffer use a transient arena allocated from
   `core`'s phase-3-driver-owned arena pool, exempt from the 128
   MiB cell per `perf-budget.md` Allocator Rule #4. Drain failure
   (allocations leaking past phase 9) is a `core::Error::OutOfBudget`
   "leak" arm with a debug-build assertion.

### 9.5 Determinism cost — fixed iteration order premium

Physics deliberately spends a measurable fraction of its 2.00 ms
cell on the §6.4 rule 2 fixed-iteration-order guarantee. The
mechanical cost is the per-substep `std::ranges::sort` over the
`BodyId` (or `JointId`) scratch span before each ECS-archetype
walk in phase 3 (entry barrier, exit barrier, snapshot capture,
joint-break impulse threshold, query overlap append).

Estimated determinism premium under S1, in steady-state:

| Source                                                                               | CPU ms (sim) |
|--------------------------------------------------------------------------------------|--------------|
| `BodyId` / `JointId` ascending sort vs. archetype-chunk-natural order                | ~0.05        |
| Single-threaded `JobSystemSingleThreaded` vs. multi-threaded Jolt step               | ~0.05        |
| **Total premium**                                                                    | **~0.10**    |

This **~0.10 ms premium is accepted** per PHILOSOPHY §7
("Determinism by default. Physics + ECS world snapshots byte-equal
across hosts and runs"). It is folded into the §9.2 sub-budget
totals — it is not a separate row eating into headroom.
Specifically: the 1.50 ms Jolt-step row absorbs the ~0.05 ms
single-threaded premium (Jolt's deterministic mode disables FMA,
SIMD reduction-order tricks, and parallel constraint partitioning
per §6.4 rule 1); the 0.30 ms mirror row absorbs the ~0.05 ms
sort premium (sort cost is `O(n log n)` over ~30 active bodies +
1 character at S1, ~0.025 ms per barrier × two substeps × two
barriers = 0.10 ms upper bound, with the remainder masked by L1
cached scratch reuse across the four barriers).

A future spike that proposes parallel-dispatch Jolt for higher
body counts must amend §6.4 rule 1, not §9 — the determinism
premium is a §6.4 contract that §9 budgets against, not a §9
ceiling. Lifting the premium would mean breaking PHILOSOPHY §7,
which is out of scope for this record.

### 9.6 CI gate hooks physics owns

The per-context CI gate (`perf-budget.yml`, scoped under the
`task-breakdown-error-perf` follow-up spike) requires each
per-context SPEC §9 to declare benchmarks that exercise the row
(`perf-budget.md` §"CI Gate Spec" rule 1). Physics owns the
following hooks:

#### 9.6.1 Per-stage `BENCHMARK_CELL` blocks

One per §9.2 row (PR fails on any breach):

| Stage                | `BENCHMARK_CELL` test name                            | CPU ceiling | Source aggregate(s)                          |
|----------------------|-------------------------------------------------------|-------------|----------------------------------------------|
| Jolt step            | `physics/world: jolt_step_two_substep_upper_bound`    | 1.50 ms     | §4.1.1, §4.1.4, §4.1.13                      |
| ECS↔Jolt mirror      | `physics/bodies: ecs_jolt_mirror_two_substep`         | 0.30 ms     | §4.1.5, §4.1.5b, §4.1.7, §4.1.14             |
| Queries              | `physics/queries: raycast_sweep_overlap_reserve`      | 0.20 ms     | §4.1.10, §4.1.11                             |
| Cell total           | `physics/world: phase3_total_two_substep`             | 2.00 ms     | composite — all of §4.1                      |

Each block constructs the S1 fixture, drives phase 3 for a
steady-state sample, and asserts wall-clock time `<= cell_budget_ms`.
The composite `physics/world: phase3_total_two_substep` row exists
so a regression that rebalances time *between* stages without
changing the total is still caught at the cell ceiling.

#### 9.6.2 Per-pool heap-residency assertions

One per §9.3 row, recording resident `ContextTag::physics` bytes
attributed to that pool at the end of frame 600 of the S1 sample
run (`perf-budget.md` §"CI Gate Spec" rule 3):

| Pool                       | Heap-residency test name                           | Ceiling   |
|----------------------------|----------------------------------------------------|-----------|
| Jolt body + constraint     | `physics/world: heap_jolt_pools`                   | 64 MiB    |
| `ShapeBlob` hash table     | `physics/shapes: heap_shape_table`                 | 32 MiB    |
| Snapshot scratch arena     | `physics/snapshot: heap_scratch_arena`             | 16 MiB    |
| Query result buffers       | `physics/queries: heap_safety_pool`                |  8 MiB    |
| Contact event ring         | `physics/contact: heap_event_ring`                 |  8 MiB    |
| Cell total                 | `physics/world: heap_context_tag_total`            | 128 MiB   |

#### 9.6.3 60 fps sample-scene fixture (frame-time bench)

The end-to-end physics step-time bench runs the **S1 sample scene
fixture** (`perf-budget.md` §"Justification Per Cell" definition:
1 character + 200 props + 8 dynamic lights at 1920x1080) at the
target **60 fps** for 600 frames and records per-frame phase-3
wall-clock time via `MTLCounterSampleBuffer`-equivalent CPU
markers around the `world/phase3_driver.cpp` entry / exit. Gate
thresholds (PR fails on any breach):

- p50 phase-3 time `<= 1.20 ms` (steady-state, one substep / frame),
- p99 phase-3 time `<= 2.00 ms` (cell ceiling under the
  realistic two-substep upper bound),
- any single frame `> 2.00 ms` is a fail (no excursions tolerated;
  clamp events §4.1.3 invariant 2 are logged, not budgeted).

The fixture lives under `e2e/perf/physics/s1_sample_scene/` and is
versioned alongside the gate; a fixture change requires a
perf-budget amendment spike per `perf-budget.md` §"CI Gate Spec".

#### 9.6.4 Determinism gate is a §11 row, not a §9 row

The byte-equal snapshot round-trip across hosts (§6.4 rules 1–4;
§8.6 round-trip test) is measured by the determinism gate, **not**
by the perf-budget gate; a snapshot mismatch is a §11 acceptance
failure, not a §9 ceiling breach. The two gates are independent —
a frame can be deterministic and over-budget, or in-budget and
non-deterministic; both must pass for a PR to merge.

### 9.7 Cross-references

- `reviews/decisions/perf-budget.md` — engine-wide allocation
  cited in §9.1; allocator rules cited in §9.4; CI gate spec cited
  in §9.6.
- `reviews/decisions/frame-phases.md` — phase-3 sole ownership
  cited in §9.1 and §9.2.
- `specs/physics/SPEC.md` §4.1 (aggregate roster), §4.2 (cross-
  aggregate invariants), §6.1 (module layout), §6.2 (phase-3
  driver), §6.3 (mirror seam), §6.4 (determinism guards), §6.5
  (cross-context handoffs) — the implementation surfaces whose
  steady-state cost the §9.2 / §9.3 rows budget against.
- `specs/core/SPEC.md` §9 — the engine-wide allocator and
  per-context CI-gate plumbing physics plugs into.
- PHILOSOPHY §7 — determinism contract that §9.5's ~0.10 ms
  premium is the price of.

## 10. Failure Modes & Error Model

Physics's failure surface is the closed sum `physics::Error` declared
in §5. Every public function in §5 returns
`Result<T> = std::expected<T, glibre::Error>`, and physics contributes
exactly one arm to the engine-wide variant per
`reviews/decisions/error-model.md`. §10 fills three slots that §5
left implicit:

1. **Per-arm semantics.** For each variant: trigger, recovery
   contract, and log severity. The recovery contract names what the
   *caller* may do; physics itself never auto-retries across the
   boundary (composition rule 2 of `error-model.md`).
2. **Aggregate-by-aggregate failure surface.** Which arms each §4
   aggregate can return at which §5 entry points, plus the
   recovery shape specific to that aggregate.
3. **Determinism-gate translation.** How `NumericalInstabilityDetected`
   and `DeterminismCheckFailed` flow through the §6.4 guards and the
   §8.6 round-trip test; the **CI vs shipping** severity split that
   keeps determinism a hard gate in CI without aborting shipping
   sessions on a downgrade-able warn.

§10 introduces three new arms beyond what §5 enumerated at spec-draft
time — `QueryFilterInvalid`, `NumericalInstabilityDetected`, and
`DeterminismCheckFailed`. Adding any of them is an ABI bump per the
`physics::Error` closed-sum rule (§5 preamble, PHILOSOPHY §9). The
arms are listed here so the diagnostic surface is enumerated in one
place; the §5 enum row + the `JoltMiddleman` ABI-hash bump for the
three additions land in the follow-up implementation plan
(see §10.7).

The issue-cited shorthand `AccumulatorClampHit` is the same condition
as §5's `physics::Error::AccumulatorClampExceeded` (carry dropped past
the four-substep cap, §4.1.3 invariant 2). The shorthand
`BodyIdInvalid` is the same condition as §5's `BodyNotFound`
(`BodyId` resolves outside its world, §4.1.5b invariant 1). §10 uses
the §5 enumerator names.

### 10.1 Per-arm contract

Trigger / Recovery / Severity for every `physics::Error` arm. Severity
is the level the *handling boundary* logs at (per `error-model.md`
§"Logging / Telemetry" rule 1 — physics never logs at the raise site).
Recovery is what the caller may do; the table omits the universal
"propagate via `std::unexpected` and let the next boundary decide"
fallback.

#### `ConfigInvalid`

- **Trigger.** `PhysicsConfig` rejected at world init: layer
  interaction matrix incomplete, `dt` non-positive, broadphase-layer
  mapping missing entries, sleep thresholds negative, budget caps
  zero, `JoltMiddleman` ABI hash mismatch in the config-stamped
  field. Detected in `PhysicsWorld::create` before any Jolt allocation.
- **Recovery.** Caller fixes the `PhysicsConfigRecord`. The world
  is **refused** — no half-built `PhysicsWorld` is published.
  Returning `Result<PhysicsWorldHandle>` with `unexpected` is the
  only outcome; partial state is rolled back inside the call.
- **Severity.** `error`. Always actionable; a misconfigured
  `PhysicsConfig` cannot be recovered without operator intervention.

#### `WorldNotInitialised`

- **Trigger.** Any §5 entry point invoked before
  `PhysicsWorld::create` returned successfully (e.g. spawning a body
  on a default-constructed `PhysicsWorldHandle`).
- **Recovery.** Programming error in nearly every case. Caller
  reorders init: `PhysicsConfig` → `PhysicsWorld::create` → body /
  collider spawning. Physics **refuses** the call.
- **Severity.** `error`. CI promotes to a build failure inside test
  runs (mirrors platform §10.1 `AlreadyExists` behavior).

#### `WorldAlreadyInitialised`

- **Trigger.** `PhysicsWorld::create` called twice on the same
  `PhysicsWorldHandle` (§4.1.1 invariant 2 — single-owner). Also
  surfaces if a hot-reload swap accidentally races a second `create`
  on the carrier handle before the migration arena drains.
- **Recovery.** Programming error. Caller drops the duplicate
  `create`. The previous-good world keeps stepping; the second call
  is **refused** with no state mutation.
- **Severity.** `warn`. Not `error` because the previous-good world
  is unaffected; the duplicate is silenced cleanly.

#### `BudgetExceeded`

- **Trigger.** Body / collider / contact / constraint count exceeds
  the per-pool ceiling pinned by `PhysicsConfig` (§4.1.2,
  §9.3 ceilings). Surfaces from `PhysicsWorld::add_body`,
  `add_collider`, `add_joint`, and from the substep when Jolt's
  internal contact pool overflows.
- **Recovery.** Caller-domain decision. Content / gameplay code
  either (a) despawns lower-priority bodies, (b) raises the
  `PhysicsConfig` budget on the next world creation event (a budget
  change is a fresh-world event, §4.1.2 invariant 2), or (c)
  surfaces the failure as a content-budget violation. Physics
  **clamps** by refusing the `add_*` call — it never silently drops
  a pre-existing body to make room.
- **Severity.** `error` at the handling boundary. CI's S1 fixture
  (§9.6.3) treats a `BudgetExceeded` from steady state as a fixture
  authoring bug.

#### `BodyNotFound` (alias `BodyIdInvalid` per issue)

- **Trigger.** `BodyId` resolves outside its owning world: stale
  handle (body despawned mid-frame and the call lands in phase 8),
  cross-world `BodyId` (handle from world A used against world B),
  or zero-init `BodyId` reaching a query (§4.1.5b invariant 1).
- **Recovery.** Caller-domain decision. Gameplay code typically
  treats a stale `BodyId` as a despawn signal and skips the
  affected entity. Physics **refuses** the per-call effect — no
  Jolt mutation occurs.
- **Severity.** `info` at the handling boundary. The most common
  trigger (despawn-during-step, §4.1.14 collapse) is expected
  steady-state traffic; logging at higher levels would drown the
  legitimate failures.

#### `BodyMotionTypeImmutable`

- **Trigger.** `RigidBody::set_motion_type` (or equivalent) called
  on a body whose `MotionType` is already pinned (§4.1.5
  invariant 3). Mutating `MotionType` post-create is forbidden;
  the caller must despawn and re-add.
- **Recovery.** Programming error. Caller despawns + re-adds with
  the desired `MotionType`. Physics **refuses** the mutation; the
  Jolt body keeps its current `MotionType`.
- **Severity.** `warn`. Common during gameplay-system development;
  not `error` because the simulation continues correctly.

#### `BodyStillReferencedByJoint`

- **Trigger.** `PhysicsWorld::remove_body` called while a live
  `Joint` still references the body as endpoint A or B (§4.1.7
  invariant 4). Detected by Jolt's joint registry walk before the
  body destruction is committed.
- **Recovery.** Caller removes the offending joints first
  (`PhysicsWorld::remove_joint(jid)`) then re-issues the body
  remove. Physics **refuses** the body remove until the joint set
  is empty; this prevents dangling endpoints.
- **Severity.** `warn`. The previous-good world is unaffected; the
  caller fixes ordering and retries.

#### `ColliderShapeRequired`

- **Trigger.** `add_collider` called with no `ShapeHandle` set
  (default-init), or with a `ShapeHandle` whose refcount is zero
  (§4.1.6 invariant 2 — shape table garbage-collects refcount-zero
  entries). Surfaces from the §5 collider-builder seam.
- **Recovery.** Caller calls `PhysicsWorld::intern_shape(blob)` to
  obtain a live `ShapeHandle`, then re-issues the collider add.
  Physics **refuses** the collider add.
- **Severity.** `error`. Almost always a content-pipeline drift
  (cooked shape blob missing) and operator-actionable.

#### `ShapeBlobMalformed`

- **Trigger.** `intern_shape(blob)` called with bytes that fail
  Fory deserialisation, or a deserialised `ShapeBlobRecord` whose
  variant tag is unknown to this build (e.g. a `ConvexHull` blob
  shipped from a newer cooker than the engine). Surfaces from the
  data-context schema reader (§7.1.2).
- **Recovery.** Caller treats as missing-asset (drop / fall back to
  default cube collider). Physics **refuses** the intern; no
  partial shape is registered. Content-pipeline drift is the usual
  root cause; the asset cooker's blob version is the diagnostic
  field.
- **Severity.** `error`. CI promotes to a fixture authoring failure.

#### `ShapeBlobVersionUnsupported`

- **Trigger.** `intern_shape(blob)` deserialised cleanly but the
  `ShapeBlobRecord.schema_version` is greater than the running
  build's reader-side current version, with no migration chain
  registered (§7.2.2). Surfaces during normal world build *and*
  during hot-reload restore (§8.3.2).
- **Recovery.** Two operator paths: (a) author the missing
  `vN → vN+1` migration body under `src/physics/migrations/` and
  rebuild, (b) re-cook the asset against the current schema
  version. Physics **refuses** the intern; the previous-good world
  keeps stepping.
- **Severity.** `error`. Always operator-actionable.

#### `ShapeHandleStale`

- **Trigger.** A `ShapeHandle` whose refcount has decremented to
  zero is reused (§4.1.6 invariant 2 — handle reuse only after
  garbage collection of the refcount-zero entry). The most common
  trigger is a gameplay system that cached a `ShapeHandle` across
  a world destroy / recreate.
- **Recovery.** Caller re-interns from the underlying blob.
  Physics **refuses** the per-call effect.
- **Severity.** `warn`. Not `error` because the simulation
  continues; the stale handle is silenced cleanly.

#### `ShapeBlobMissing`

- **Trigger.** Hot-reload restore phase encounters a
  `body_shape_blob_hashes` entry in the carrier `PhysicsSnapshot`
  that no longer resolves in the surviving `ShapeBlobRecord` table
  (§8.4 row 4). Cooked-asset drift between the snapshot and the
  new plugin's shape table is the root cause.
- **Recovery.** Operator restores the dropped `ShapeBlobRecord`,
  re-cooks the affected body's shape, or accepts a fresh world
  (snapshot re-capture against the current shape table). The
  hot-reload swap is **refused** at the loader (`core::Error::
  HotReloadRefused` carrying `ShapeBlobMissing`); the previous-
  good plugin remains live (§8.4).
- **Severity.** `warn` per the §8.4 row contract — the previous-
  good plugin keeps stepping, so the failure is informational at
  the hot-reload boundary, not an `error`.

#### `JointEndpointInvalid`

- **Trigger.** `PhysicsWorld::add_joint` called with a `JointDescriptor`
  whose endpoint A or B `BodyId` does not resolve in the world
  (zero-init handle, cross-world handle, body already despawned).
  Detected before Jolt's constraint allocation.
- **Recovery.** Caller validates body endpoints (re-fetches live
  `BodyId`s) and re-issues. Physics **refuses** the joint add; no
  Jolt constraint is allocated.
- **Severity.** `error`. Almost always a content / gameplay
  ordering bug (joint authored before its endpoint bodies exist).

#### `JointDanglingEndpoint`

- **Trigger.** `remove_body` attempted with a live joint still
  referencing it (the *symmetric* case of `JointEndpointInvalid`,
  detected at body-removal time rather than joint-add time;
  §4.1.7 invariant 4). Surfaces from `PhysicsWorld::remove_body`.
- **Recovery.** Caller removes joints first, then re-issues the
  body remove. Physics **refuses** the body remove.
- **Severity.** `warn`. Previous-good world unaffected.

#### `JointKindUnsupported`

- **Trigger.** Post-MVP joint kind requested (a `JointKind`
  enumerator absent from this build's sealed sum, §4.1.7
  invariant 1). Surfaces from `add_joint` and from snapshot
  restore against an older build.
- **Recovery.** Operator-level: rebuild the physics plugin with
  the missing joint kind compiled in (an ABI bump, PHILOSOPHY §9).
  Physics **refuses** the joint add or the snapshot restore.
- **Severity.** `error`. Always operator-actionable.

#### `JointBroken`

- **Trigger.** Mutation attempted on a joint that has tripped its
  `JointBreakThreshold` (`JointBrokenEvent` already emitted,
  §4.1.7 invariant 5). Surfaces from `set_joint_motor`,
  `set_joint_limits`, and friends.
- **Recovery.** Caller checks `is_joint_broken(jid)` before mutating,
  or removes the broken joint and adds a fresh one. Physics
  **refuses** the mutation; the broken joint stays broken.
- **Severity.** `info`. Routine post-break cleanup signal; logging
  louder would drown legitimate failures.

#### `StepCalledOutsidePhase3`

- **Trigger.** Any caller-driven `Step` outside phase 3 (PHILOSOPHY
  §7 — phase 3 is the sole owner of advancement; §4.1.3 invariant 1).
  Detected by the frame-phase guard at the §5 entry point.
- **Recovery.** Programming error. Caller routes the simulation
  advance through the engine frame loop. Physics **refuses** the
  call; no Jolt step occurs.
- **Severity.** `error`. CI promotes to a frame-loop integration
  failure.

#### `QueryDuringStep`

- **Trigger.** `PhysicsQueries::ray_cast` / `shape_cast` / `overlap`
  / `closest_point` invoked while phase 3 is in flight (§4.1.10
  invariant 4). Detected by the phase guard on the queries
  aggregate.
- **Recovery.** Caller defers the query to phase 5+ (post-step
  window) or phase 1 (pre-step window). Physics **refuses** the
  query; no Jolt broadphase walk occurs.
- **Severity.** `warn`. Common during gameplay-system development;
  not `error` because the simulation continues.

#### `QueryFilterInvalid` (added by §10)

- **Trigger.** `QueryFilter` rejected at the queries entry: an
  empty `CollisionLayer` mask (would match nothing — programming
  error), a callback predicate that mutates ECS state during the
  filter walk (§4.1.10 invariant 5; debug-build assertion
  promoted to a typed arm in release), or an
  ECS-component-presence requirement that names a component
  unknown to the running build's schema. Detected before Jolt's
  broadphase is touched.
- **Recovery.** Caller fixes the filter shape. Physics **refuses**
  the query; the result span is left untouched.
- **Severity.** `error`. The "filter is pure" rule (§4.1.10
  invariant 5) is determinism-relevant — a filter that mutates ECS
  state during the broadphase walk would invalidate substep
  byte-equality (§4.2 invariant 3); we refuse loudly.

#### `AccumulatorClampExceeded` (alias `AccumulatorClampHit` per issue)

- **Trigger.** Phase-3 entry observed `acc >= 4 * dt` after the
  per-frame `acc += core_dt` step; the bounded-catch-up cap
  (§4.1.3 invariant 2) drops the carry past four substeps.
- **Recovery.** Physics **clamps** by dropping the carry past four
  substeps and continues stepping. The dropped carry is logged for
  diagnostic purposes; the simulation re-converges within the
  next few frames once the wall-clock spike subsides. Caller need
  not act unless the spike is sustained, in which case operator
  surfaces a frame-pacing investigation.
- **Severity.** `warn`. The clamp is a deliberate determinism
  trade (PHILOSOPHY §7 — determinism over wall-clock fidelity
  under stall). The non-fatal path is also surfaced as
  `physics::Warning::AccumulatorClamped` (§5) for the cases that
  do not need to abort a call; the typed `Error` arm is reserved
  for the case where a caller asked the accumulator for a guarantee
  it cannot keep (e.g. fixed-substep-replay traces with a missing
  carry frame).

#### `SubstepEcsCommitInverted`

- **Trigger.** Mid-substep ECS↔Jolt cross-traffic detected (§4.1.4
  invariant 2). The debug-build instrumentation promoted to a
  typed arm in release builds when the deterministic-mode flag is
  on (§6.4 rule 1).
- **Recovery.** Programming error in a §6.3 mirror seam refactor.
  Physics **aborts** the substep — no partial step is committed —
  and the typed arm carries the offending entity / component.
- **Severity.** `error`. Always determinism-breaking; CI promotes
  to a determinism-gate failure (§6.4 + §8.6).

#### `SnapshotSchemaMismatch`

- **Trigger.** `PhysicsSnapshot::from_bytes` deserialised cleanly
  but the snapshot's `physics_config_hash` mismatches the running
  world's recomputed `PhysicsConfig.content_hash` (§7.1.4
  invariant 3, §8.4 row 3). Cross-config restore is forbidden.
- **Recovery.** Operator either reverts the `PhysicsConfigRecord`
  change (a config change is a fresh-world event, not a
  hot-reload event, §4.1.2 invariant 2) or restores from a
  snapshot whose hash matches the new config. Physics **refuses**
  the restore; the previous-good world keeps stepping.
- **Severity.** `warn` at the hot-reload boundary (§8.4 row
  contract — the previous-good plugin is unaffected); CI promotes
  to an `error` inside the determinism-gate fixture.

#### `SnapshotDeserialiseFailed`

- **Trigger.** `PhysicsSnapshot::from_bytes` fed bytes that fail
  Fory deserialisation (truncated, version-tag-corrupt, or
  schema-incompatible past the migration table's reach). Surfaces
  from the data-context codec (§7.1.4).
- **Recovery.** Caller treats the snapshot as unrecoverable
  (re-capture from the live world, or fall back to a fresh
  world). Physics **refuses** the restore.
- **Severity.** `error`. Always actionable.

#### `SnapshotBodyIdUnresolved`

- **Trigger.** Snapshot restore observed a persisted `BodyId` that
  has no live ECS entity in the post-restore world (§7.1.4
  invariant 4 — `BodyId` ↔ `ecs::Entity` mapping is part of the
  snapshot but the entity was destroyed before restore completed).
- **Recovery.** Caller drops the affected body from the carrier
  snapshot and re-issues, or accepts the body absence (the
  per-body-fail-tolerant restore path). Physics **clamps** by
  skipping the unresolved row and continues; the dropped row
  count is logged.
- **Severity.** `warn`. The restore continues — the simulation is
  not aborted, but the diagnostic is preserved for telemetry.

#### `JoltMiddlemanHashMismatch`

- **Trigger.** Hot-reload protocol step 2.1 observed
  `Q::glibre_types_abi_hash() != host_glibre_types_abi_hash` for the
  physics-typed surface, *or* physics's `register` step 1 re-asserted
  via `JoltMiddleman::require_hash` and the new plugin's compiled-in
  hash differs from the running middleman dylib's hash (§4.1.13
  invariants 1+2, §8.4 row 1).
- **Recovery.** Operator rebuilds the physics plugin against the
  current `JoltMiddleman` (same Jolt version + type list the host
  shipped). The hot-reload swap is **refused** at the loader; the
  previously-loaded physics plugin keeps stepping (PHILOSOPHY §9 —
  refuse load on hash mismatch).
- **Severity.** `warn`. The previous-good plugin is unaffected; the
  refusal is loud-but-bounded per §8.4 / `error-model.md`
  §"Logging / Telemetry" rule 3.

#### `JoltMiddlemanUnavailable`

- **Trigger.** Engine init reached the physics plugin register step
  but `glibre-jolt-middleman.dylib` is not loaded (file missing,
  load failed earlier, or the host's plugin manifest does not
  declare the middleman dependency). Detected by the middleman
  presence check before any Jolt allocation.
- **Recovery.** Operator-level: ship the middleman dylib alongside
  the physics dylib, or fix the manifest to declare the dependency.
  Physics **aborts** its register; the engine boots without the
  physics plugin (the engine treats the missing plugin as a §6.5
  cross-context handoff failure, not a per-frame error).
- **Severity.** `error`. Always operator-actionable; the engine
  cannot run physics without the middleman.

#### `HotReloadStateUnmigratable`

- **Trigger.** Hot-reload protocol step 3.1 found
  `PhysicsSnapshot.schema_version` greater than the new plugin's
  reader-side current version, with no migration chain registered
  (§7.2.4, §8.4 row 2). Detected before any byte is overwritten in
  the surviving storage.
- **Recovery.** Operator authors the missing `vN → vN+1` migration
  body under `src/physics/migrations/`, ships it in the new plugin,
  and re-triggers the swap; *or* restores from a snapshot taken at
  the prior schema version. The migration arena is reset before
  the cause is returned. The hot-reload swap is **refused** at the
  loader; the previously-loaded physics plugin keeps stepping
  (§8.4).
- **Severity.** `warn`. Same `error-model.md` §"Logging /
  Telemetry" rule 3 contract as the other hot-reload refusals.

#### `NumericalInstabilityDetected` (added by §10)

- **Trigger.** A NaN or infinity reached the substep barrier in a
  field that crosses the §6.4 rule 4 IEEE-754 bit-equal storage
  contract: body position / orientation / velocity, contact-event
  impulse, joint motor target, or external-force drain sum. The
  runtime never *produces* NaN (§4.1.4 invariant 4); detection here
  means an external input (gameplay code, hot-reload migration,
  snapshot deserialise) introduced one. Detected by the substep
  exit barrier's bit-pattern check.
- **Recovery.** Physics **aborts** the offending substep — the
  Jolt body / joint state is rolled back to the substep entry
  snapshot, and the substep is not committed to ECS. The carrier
  trace records the offending entity / component / value for the
  determinism-gate corpus. Recovery in shipping is per-substep
  (caller's gameplay logic decides whether to despawn the body or
  freeze it); CI fails the run.
- **Severity.** `error` always at the handling boundary. Unlike
  `DeterminismCheckFailed`, this is never downgraded to `warn` in
  shipping — a NaN in the simulation state is a content / gameplay
  bug that operators must see.

#### `DeterminismCheckFailed` (added by §10)

- **Trigger.** The §6.4 + §8.6 determinism gate observed a
  divergence: post-substep snapshot bytes do not match the golden
  trace at frame `N` (§8.6 step 3 assertion), *or* the §6.4 rule 2
  fixed-iteration-order scratch span hash diverged from its
  pre-step value (mid-step iteration order corruption), *or* a
  cross-host snapshot byte-comparison at the §11 acceptance fixture
  (`macos-arm64` vs `macos-x64`) reported a mismatch.
- **Recovery.** Physics **refuses** to commit the divergent
  substep in CI builds; the simulation is rolled back to the last
  known-good snapshot and the carrier trace is preserved for the
  determinism-gate corpus. In shipping builds the **default** is
  to log at `warn` and continue (the divergence is recorded for
  telemetry, the simulation keeps stepping with the divergent
  state). The downgrade is **configurable**: shipping builds may
  opt into the CI behavior via `PhysicsConfig.determinism_gate =
  Hard` (default `SoftWarn`), in which case the substep is
  refused as in CI.
- **Severity.**
  - **CI builds (default `Hard`)** — `error`. The PR fails the
    determinism gate (§9.6.4); the substep refuses to commit.
  - **Shipping builds (default `SoftWarn`)** — `warn`. The
    substep commits with the divergent state; telemetry records
    the divergence for post-hoc analysis. Operators may flip to
    `Hard` if their build cannot tolerate diverged steady state
    (e.g. lock-step multiplayer).

  The CI vs shipping split is the only place in §10 where the
  same arm carries two severities. The split is encoded as a
  `physics::DeterminismGate { Hard, SoftWarn }` knob in
  `PhysicsConfig`, defaulted by build flavor: `Hard` under
  `-DGLIBRE_DETERMINISM_GATE=hard` (set by the CI workflow),
  `SoftWarn` otherwise. Engine code never branches on the build
  flavor directly — the knob is the only inspection point.

### 10.2 Aggregate-by-aggregate failure surface

Each §4 aggregate enumerates: which arms it can return at which §5
entry points, and the recovery shape specific to that aggregate.
Cross-aggregate recovery is forbidden — `PhysicsQueries` does not
re-translate a `PhysicsWorld` error.

#### 10.2.1 `PhysicsWorld` (§4.1.1)

| Entry point                          | Returnable arms                                                                                          | Trigger summary                                              |
|--------------------------------------|----------------------------------------------------------------------------------------------------------|--------------------------------------------------------------|
| `PhysicsWorld::create`               | `ConfigInvalid`, `WorldAlreadyInitialised`, `JoltMiddlemanUnavailable`, `JoltMiddlemanHashMismatch`      | bad config, double-init, middleman missing / hash mismatch   |
| `PhysicsWorld::add_body`             | `WorldNotInitialised`, `BudgetExceeded`, `ColliderShapeRequired`, `ShapeHandleStale`                     | budget cap, missing shape, stale handle                      |
| `PhysicsWorld::remove_body`          | `WorldNotInitialised`, `BodyNotFound`, `BodyStillReferencedByJoint`, `JointDanglingEndpoint`             | live joint endpoint, stale `BodyId`                          |
| `PhysicsWorld::set_motion_type`      | `WorldNotInitialised`, `BodyNotFound`, `BodyMotionTypeImmutable`                                         | mutation post-create forbidden                               |
| `PhysicsWorld::add_collider`         | `WorldNotInitialised`, `BodyNotFound`, `ColliderShapeRequired`, `ShapeHandleStale`, `ShapeBlobMalformed` | shape table drift                                            |
| `PhysicsWorld::add_joint`            | `WorldNotInitialised`, `JointEndpointInvalid`, `JointKindUnsupported`, `BudgetExceeded`                  | bad endpoints, post-MVP kind                                 |
| `PhysicsWorld::remove_joint`         | `WorldNotInitialised`, `BodyNotFound`                                                                    | stale `JointId`                                              |
| `PhysicsWorld::set_joint_motor`      | `WorldNotInitialised`, `BodyNotFound`, `JointBroken`                                                     | broken-joint mutation                                        |
| `PhysicsWorld::step`                 | `StepCalledOutsidePhase3`, `AccumulatorClampExceeded`, `SubstepEcsCommitInverted`, `NumericalInstabilityDetected`, `DeterminismCheckFailed` | phase guard, clamp, mirror inversion, NaN, divergence      |
| `PhysicsWorld::snapshot`             | `WorldNotInitialised`, `SnapshotSchemaMismatch`                                                          | config hash drift                                            |
| `PhysicsWorld::intern_shape`         | `WorldNotInitialised`, `ShapeBlobMalformed`, `ShapeBlobVersionUnsupported`, `BudgetExceeded`             | bad blob, version unsupported                                |

`AccumulatorClampExceeded` from `step` is the typed-arm form of
`Warning::AccumulatorClamped` — see §10.1 for the warning vs error
split. `NumericalInstabilityDetected` and `DeterminismCheckFailed`
both abort the offending substep before commit; the world keeps
stepping at the next frame.

#### 10.2.2 `Accumulator` (§4.1.3)

The accumulator does not expose a public `Result<T>` surface — its
state mutations are §6.2-internal to `PhysicsWorld::step`. Failures
surface through `PhysicsWorld::step`'s arms (`AccumulatorClampExceeded`,
`StepCalledOutsidePhase3`). Listed here for completeness.

#### 10.2.3 `PhysicsQueries` (§4.1.10)

| Entry point                          | Returnable arms                                                              | Trigger summary                                              |
|--------------------------------------|------------------------------------------------------------------------------|--------------------------------------------------------------|
| `PhysicsQueries::ray_cast`           | `WorldNotInitialised`, `QueryDuringStep`, `QueryFilterInvalid`, `BodyNotFound` | bad filter, phase-3 violation                              |
| `PhysicsQueries::shape_cast`         | `WorldNotInitialised`, `QueryDuringStep`, `QueryFilterInvalid`, `ShapeHandleStale` | bad filter, stale shape                                  |
| `PhysicsQueries::overlap`            | `WorldNotInitialised`, `QueryDuringStep`, `QueryFilterInvalid`               | bad filter                                                  |
| `PhysicsQueries::closest_point`      | `WorldNotInitialised`, `QueryDuringStep`, `QueryFilterInvalid`, `BodyNotFound` | bad filter, stale body                                    |

`QueryFilterInvalid` is the dedicated arm for the §4.1.10
invariant 5 "filter is pure" check; mutating ECS during a filter
callback is determinism-breaking and refused at the boundary.

#### 10.2.4 `PhysicsSnapshot` (§4.1.12)

| Entry point                             | Returnable arms                                                        | Trigger summary                                              |
|-----------------------------------------|------------------------------------------------------------------------|--------------------------------------------------------------|
| `PhysicsSnapshot::from_bytes`           | `SnapshotDeserialiseFailed`, `SnapshotSchemaMismatch`, `ShapeBlobMissing`, `HotReloadStateUnmigratable` | bad bytes, config drift, missing blob, schema gap   |
| `PhysicsSnapshot::to_bytes`             | total                                                                  | —                                                            |
| `PhysicsSnapshot::restore_into`         | `SnapshotBodyIdUnresolved`, `JointEndpointInvalid`, `ShapeBlobMissing`, `ShapeBlobVersionUnsupported` | per-row restore failure                              |

`SnapshotBodyIdUnresolved` is per-row and the restore continues; the
other arms are pre-flight and the restore refuses entirely.

#### 10.2.5 `JoltMiddleman` (§4.1.13)

The middleman is not a §5 public boundary — its `Result<T>` surface
is internal to `PhysicsWorld::create` and the hot-reload register
step. Failures surface through `JoltMiddlemanHashMismatch` and
`JoltMiddlemanUnavailable` on the `PhysicsWorld::create` row above.
Listed here so the §4 aggregate roster maps 1:1 to §10.

### 10.3 Determinism gate translation (CI vs shipping)

The §6.4 rules and the §8.6 round-trip test are the determinism
contract; §10.3 documents how the contract surfaces as typed arms
and how the CI vs shipping severity split is encoded.

The two arms that participate are `NumericalInstabilityDetected`
(NaN / inf reached a substep barrier) and `DeterminismCheckFailed`
(snapshot / iteration-order divergence detected by the gate). The
two arms split as follows:

| Arm                              | Detection site                           | CI behavior (`Hard`)                  | Shipping default (`SoftWarn`)         |
|----------------------------------|------------------------------------------|---------------------------------------|---------------------------------------|
| `NumericalInstabilityDetected`   | substep exit bit-pattern check (§6.4 r4) | `error`, abort substep, fail PR       | `error`, abort substep, telemetry     |
| `DeterminismCheckFailed`         | §8.6 round-trip + §6.4 r2 hash compare   | `error`, refuse commit, fail PR       | `warn`, commit divergent step, telemetry |

`NumericalInstabilityDetected` is **never** downgraded — a NaN in the
simulation state is a content / gameplay bug that operators must see
even in shipping. `DeterminismCheckFailed` is the only arm whose
severity depends on the build flavor; the `PhysicsConfig.
determinism_gate` knob (`Hard` / `SoftWarn`) selects the behavior.
The default is `Hard` under `-DGLIBRE_DETERMINISM_GATE=hard` (set by
the CI workflow `.github/workflows/determinism-gate.yml`) and
`SoftWarn` otherwise.

The split is anchored to PHILOSOPHY §7 ("determinism by default") +
the operational reality that shipping a build that aborts on a
single substep divergence would be hostile to players running
heterogeneous hardware. CI is the place where the gate must be
hard; shipping is the place where the gate may be soft. The
configurability lets a downstream that needs hard determinism in
shipping (e.g. lock-step multiplayer, replay-only competitive
modes) flip to `Hard`.

The `PhysicsConfig.determinism_gate` knob is part of the
`PhysicsConfig.content_hash` (§7.1.1), so a build that flips the
knob mid-trace gets a fresh-world event (§4.1.2 invariant 2) and
the prior trace cannot be replayed — the gate setting is part of
the determinism contract.

### 10.4 Logging severity table (consolidated)

The §10.1 per-arm severities, restated as the table the
`glibre::log_error` helper uses when it formats a `physics::Error`
into `spdlog`:

| Arm                              | Default severity                        | Notes                                                          |
|----------------------------------|-----------------------------------------|----------------------------------------------------------------|
| `ConfigInvalid`                  | `error`                                 | Always operator-actionable                                     |
| `WorldNotInitialised`            | `error`                                 | Init ordering bug                                              |
| `WorldAlreadyInitialised`        | `warn`                                  | Previous-good world unaffected                                 |
| `BudgetExceeded`                 | `error`                                 | Content-budget violation                                       |
| `BodyNotFound`                   | `info`                                  | Despawn-during-step is steady-state traffic                    |
| `BodyMotionTypeImmutable`        | `warn`                                  | Simulation continues correctly                                 |
| `BodyStillReferencedByJoint`     | `warn`                                  | Caller fixes ordering and retries                              |
| `ColliderShapeRequired`          | `error`                                 | Content-pipeline drift                                         |
| `ShapeBlobMalformed`             | `error`                                 | Content-pipeline drift                                         |
| `ShapeBlobVersionUnsupported`    | `error`                                 | Operator-actionable                                            |
| `ShapeHandleStale`               | `warn`                                  | Simulation continues                                           |
| `ShapeBlobMissing`               | `warn`                                  | §8.4 hot-reload contract — previous plugin keeps stepping      |
| `JointEndpointInvalid`           | `error`                                 | Content / gameplay ordering bug                                |
| `JointDanglingEndpoint`          | `warn`                                  | Previous-good world unaffected                                 |
| `JointKindUnsupported`           | `error`                                 | Operator-actionable                                            |
| `JointBroken`                    | `info`                                  | Routine post-break cleanup                                     |
| `StepCalledOutsidePhase3`        | `error`                                 | Frame-loop integration failure                                 |
| `QueryDuringStep`                | `warn`                                  | Simulation continues                                           |
| `QueryFilterInvalid`             | `error`                                 | Determinism-relevant; refused loudly                           |
| `AccumulatorClampExceeded`       | `warn`                                  | Determinism trade per PHILOSOPHY §7                            |
| `SubstepEcsCommitInverted`       | `error`                                 | Determinism-breaking                                           |
| `SnapshotSchemaMismatch`         | `warn` (CI: `error`)                    | §8.4 hot-reload contract                                       |
| `SnapshotDeserialiseFailed`      | `error`                                 | Always actionable                                              |
| `SnapshotBodyIdUnresolved`       | `warn`                                  | Per-row, restore continues                                     |
| `JoltMiddlemanHashMismatch`      | `warn`                                  | §8.4 hot-reload contract — previous plugin keeps stepping      |
| `JoltMiddlemanUnavailable`       | `error`                                 | Engine cannot run physics                                      |
| `HotReloadStateUnmigratable`     | `warn`                                  | §8.4 hot-reload contract                                       |
| `NumericalInstabilityDetected`   | `error` (CI + shipping)                 | Never downgraded                                               |
| `DeterminismCheckFailed`         | `error` (CI `Hard`) / `warn` (shipping `SoftWarn`) | Configurable per `PhysicsConfig.determinism_gate`     |

Physics never logs at the *raise* site; logging is the *handler's*
responsibility per `error-model.md` §"Logging / Telemetry" rule 1.
Physics's contribution is the typed arm + any structured
diagnostic (offending `BodyId`, `JointId`, `frame_tick`,
`world_tick`, `physics_config_hash`) attached to the `ErrorContext`;
the engine-wide log helper does the formatting and dispatches to
spdlog.

### 10.5 Refusals (out of §10 scope)

- **Hot-reload protocol shape.** `core::Error::HotReloadRefused`,
  `PluginAbiHashMismatch`, `PluginInitFailed`, `SchemaMigrationFailed`
  are owned by `specs/core/SPEC.md`. Physics contributes the four
  inner causes enumerated in §8.4 (which roll up to those core
  arms); §10 documents the physics-surface aliases
  (`JoltMiddlemanHashMismatch`, `HotReloadStateUnmigratable`,
  `SnapshotSchemaMismatch`, `ShapeBlobMissing`).
- **Schema migration semantics.** Owned by `specs/data/SPEC.md` §10
  + the per-record migration rules in §7.2. Physics's role stops at
  surfacing `ShapeBlobVersionUnsupported` / `HotReloadStateUnmigratable`
  / `SnapshotDeserialiseFailed` at the physics surface.
- **Render BLAS invalidation on hot-reload.** Owned by
  `specs/render/SPEC.md` §4.1.8 + §10. Physics publishes
  `PhysicsWorldReplaced` (§8.5) and stops there; render's BLAS
  reaction failures are render's failure surface.
- **Frame-phase scheduler errors.** `core::Error::FramePhaseMisordered`
  is owned by `specs/core/SPEC.md`. Physics's `StepCalledOutsidePhase3`
  is the physics-side translation at the §5 boundary.
- **Plugin-loader error model.** Owned by `specs/core/SPEC.md` and
  `reviews/decisions/plugin-loader.md`.

### 10.6 Cross-references

- `reviews/decisions/error-model.md` — `std::expected<T, glibre::Error>`
  contract that §10's per-arm rows fulfil; `-fno-exceptions` on
  engine code; `glibre::log_error` dispatch rule.
- `specs/physics/SPEC.md` §5 — the `physics::Error` enum that §10
  documents arm-by-arm; the `physics::Warning` companion enum
  cited in the `AccumulatorClampExceeded` row.
- `specs/physics/SPEC.md` §4.1.* — the per-aggregate invariants
  whose violation triggers each arm.
- `specs/physics/SPEC.md` §6.4 — the four determinism guards
  whose violation surfaces as `NumericalInstabilityDetected` /
  `DeterminismCheckFailed` / `SubstepEcsCommitInverted`.
- `specs/physics/SPEC.md` §8.4 — the four hot-reload refusal
  causes whose physics-surface aliases live in §10.1
  (`JoltMiddlemanHashMismatch`, `HotReloadStateUnmigratable`,
  `SnapshotSchemaMismatch`, `ShapeBlobMissing`).
- `specs/physics/SPEC.md` §8.6 + §9.6.4 — the determinism-gate
  fixture that consumes `DeterminismCheckFailed`.
- PHILOSOPHY §7 — determinism contract that §10.3 protects;
  PHILOSOPHY §9 — refuse-on-hash-mismatch rule that
  `JoltMiddlemanHashMismatch` enforces.

### 10.7 Open questions — resolved

The four questions originally carried into §12 each resolve to an
existing external gate or to an answer frozen until a known trigger;
§12 holds no physics-owned residue. Each entry below names the gate
that re-opens it, so a future change does not need to re-derive the
deferral.

- **Add `QueryFilterInvalid`, `NumericalInstabilityDetected`, and
  `DeterminismCheckFailed` to the §5 `physics::Error` enum.** Frozen
  until the implementation plan that introduces
  `physics/include/glibre/physics/error.hpp` lands; that plan is the
  only place an enumerator-row addition is permitted, because each
  one is an ABI bump and triggers a `JoltMiddleman` hash bump
  (§4.1.13 invariant 1; PHILOSOPHY §9). The §10.1 rows already name
  these arms as "added by §10" so consumers compile against the
  documented surface; the gate to flip from documented to enumerated
  is the same implementation plan and nothing else.
- **`PhysicsConfig.determinism_gate` knob (`Hard` / `SoftWarn`).**
  Frozen at the documented two-value shape; the §4.1.2 invariant row
  and §7.1.1 schema field land together with the
  `physics/include/glibre/physics/error.hpp` plan above (same gate),
  so the §10.3 determinism-gate split becomes testable end-to-end in
  one ABI bump rather than two. No physics-side residue independent
  of that plan.
- **`ErrorContext` payload shape — structured struct vs prose
  `detail` string.** Owned by `reviews/decisions/error-model.md`
  Open Q #3 + `core/error.hpp`. Physics arms attach `BodyId`,
  `JointId`, `frame_tick`, `world_tick`, `physics_config_hash` via
  the `ErrorContext.detail` field today; if core promotes `detail`
  to a typed payload, physics migrates each attachment site as a
  mechanical edit gated by the same ABI bump as the
  `error.hpp` plan above. No physics-side residue.
- **`magic_enum` vs hand-written `to_string` for arm names in log
  output.** Same deferral as platform §10.8: owned by
  `core/error.hpp` per `reviews/decisions/error-model.md` Open Q #1;
  physics follows whatever core picks. No physics-side residue.

## 11. Acceptance Criteria

GitHub `type:user-story` issues this spec closes (drafted under spike
#131; each carries a Catch2 test name plus the story-required E2E
`.glibre-trace`):

- #421 — physics: PhysicsWorld init creates one Jolt instance per ECS world (§4.1.1 inv 1+2; §3.2 collapse #8) — pts:3
- #423 — physics: deterministic phase-3 step byte-equal across hosts (§4.1.1 inv 1; §6.4) — pts:5
- #425 — physics: fixed-timestep accumulator with bounded catch-up + carry preserved (§4.1.3 inv 2+3) — pts:3
- #427 — physics: refuse step calls outside phase 3 (§4.1.1 inv 1; §4.1.3 inv 1) — pts:2
- #429 — physics: insert RigidBody allocates BodyId, body simulates next substep (§4.1.5; §4.1.5b inv 1+2) — pts:3
- #431 — physics: remove RigidBody destroys Jolt body, refuses if joint references it (§4.1.5; §4.1.7 inv 1) — pts:3
- #434 — physics: BodyId stable across hosts and across hot-reload (§4.1.5b inv 1; PHILOSOPHY §7+§8) — pts:5
- #435 — physics: create Joint entity materialises Jolt constraint with limits/motor/break (§4.1.7) — pts:5
- #437 — physics: refuse joints with invalid or cross-world endpoints (§4.1.7 inv 1) — pts:2
- #439 — physics: PhysicsQueries::ray_cast against shared broadphase (§4.1.10 inv 1+2+5) — pts:3
- #441 — physics: PhysicsQueries::shape_cast and overlap surfaces (§4.1.10) — pts:3
- #443 — physics: contact events drained at substep exit, same-frame visible (§4.1.8 inv 1+2+3+4) — pts:5
- #445 — physics: trigger volumes emit Enter/Stay/Exit events with no impulse (§4.1.9; §4.1.6 inv 4) — pts:3
- #448 — physics: PhysicsSnapshot save/restore round-trip byte-equal (§4.1.12; §7.1.4) — pts:5
- #449 — physics: hot-reload preserves world state across snapshot at phase 8 (§8.1, §8.3.1, §8.3.2) — pts:5
- #451 — physics: ShapeBlob deduplicated by content hash, refcounted ShapeHandle (§4.1.6 inv 1+3) — pts:3

Total: 16 stories, 58 pts roll up into sub-epic #121.

Each must have a Catch2 test by name.

## 12. Open Questions

None. The four carry-ins originally listed in §10.7 each resolved in
place against existing external gates — see that sub-section for the
trigger that re-opens each one. Per spike #132.
