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
