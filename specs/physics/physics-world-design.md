# physics — Detailed Design: physics-world aggregate

> Per-aggregate detailed design for the `PhysicsWorld` + `PhysicsConfig`
> + `Accumulator` cluster declared in `specs/physics/SPEC.md` §4.1.1,
> §4.1.2, §4.1.3, with public surface frozen in §5 and frame
> integration locked in §6.2 (`world/phase3_driver.cpp`). Refines those
> sections in place; adds no new public surface beyond §5. Cites
> `reviews/decisions/error-model.md`,
> `reviews/decisions/perf-budget.md`,
> `reviews/decisions/plugin-abi.md`,
> `reviews/decisions/hot-reload-protocol.md`,
> `reviews/decisions/frame-phases.md`,
> `reviews/decisions/fory-codegen.md`. Deviations from the cited
> records require an amendment spike, not an in-place edit.
>
> Sibling aggregates (`Substep`, `RigidBody`/`BodyId`, `Collider`/
> `ShapeHandle`/`ShapeBlob`, `Joint` family, `ContactManifold` /
> `Trigger` events, `PhysicsQueries`, `BroadphaseLayer`,
> `PhysicsSnapshot`, `JoltMiddleman`, `Sleeping`/`Island`, `CCD`) are
> deliberately **out of scope here**; this design treats them as
> opaque seams that `PhysicsWorld` brokers across. Each has its own
> sibling design spike under sub-epic #791. Where this design names a
> sibling seam, it cites the SPEC invariant the seam upholds, not the
> sibling's internal mechanics.
>
> Harmonius prior art (`harmonius/docs/requirements/physics/
> rigid-body-dynamics.md` R-4.1.1 .. R-4.1.20 + R-4.1.NF1 .. R-4.1.NF4;
> `harmonius/docs/design/physics/foundation.md` § "Architecture" /
> "Substep Pipeline Sequence" / "Fixed-Timestep Accumulator" /
> "Frame Budget Integration") is research input only — every
> conclusion below is independently re-derived per `PHILOSOPHY.md` §
> "How harmonius is used".
>
> Refs: spike #792 — `[SPIKE] design-physics-physics-world-detailed`.
> Parent: #791 (sub-epic — Detailed Designs — physics). Sibling
> `[SPIKE] task-breakdown-physics-physics-world-detailed` is blocked
> by this deliverable.

## 1. Purpose

The `PhysicsWorld` aggregate is the **root entity** of the physics
context (SPEC §4.1.1). Its single responsibility is **owning the
per-`ecs::World` Jolt instance and brokering frame phase 3 end-to-end**
— constructing the `JPH::PhysicsSystem` from a frozen `PhysicsConfig`,
driving the fixed-timestep `Accumulator` that converts wall-clock
frame `dt` into a deterministic count of substeps, dispatching each
substep's ECS↔Jolt mirror barriers + `JoltMiddleman::step`, and
exposing the resolved siblings (`PhysicsQueries`, `PhysicsSnapshot`,
shape table, joint registry, body allocator, contact-event drain) to
callers behind one opaque facade. Concretely the cluster owns:

1. **The `PhysicsWorld` root entity (SPEC §4.1.1)** — one Jolt
   `PhysicsSystem` per ECS `World`; the only place a Jolt body /
   constraint / contact pair lives in-process; the lifetime owner of
   every Jolt-internal allocation, every Jolt worker thread, the
   shape-table index, the joint registry, the contact-event drain
   buffers, and the `JoltMiddleman` handle through which every
   Jolt-derived type crosses the plugin ABI.
2. **The `PhysicsConfig` value object (SPEC §4.1.2)** — the
   init-time-immutable knob set captured by value at world
   construction: gravity, fixed substep `dt`, substep cap (`4`),
   solver `velocity_iters` / `position_iters`, `warm_start_factor`,
   sleep thresholds, broadphase layer mapping, layer-pair interaction
   matrix, RNG seed, world budgets (`max_bodies` / `max_shapes` /
   `max_constraints` / `max_contacts`), CCD enable bit, and the
   `determinism_gate` knob. Every tunable that affects the stepping
   math lives here — per-body knobs (motion type, mass, damping, CCD
   flag, sleeping flag) live on `RigidBody`, per-shape knobs (layer,
   density, material) live on `Collider`. The `content_hash` field is
   BLAKE3 over the canonicalised serialised bytes (SPEC §7.1.1) and is
   the dispatch key the snapshot's `physics_config_hash` (§7.1.4)
   compares against.
3. **The `Accumulator` value object (SPEC §4.1.3)** — the per-frame
   fixed-timestep clock. Holds `carry_seconds : f32`, `tick_count :
   u64`, and a borrow of its owning `PhysicsConfig.fixed_dt`. The
   draining loop (`acc += core_dt` then while `acc >= dt &&
   substeps_done < SUBSTEP_CAP_4`) is the **only** path that calls
   into `JoltMiddleman::step`; it is a §4.2 invariant 1 enforcement
   point. The carry is preserved across frames so byte-equal trace
   replay re-derives the substep count even when host wall-clock dt
   varies frame-to-frame (PHILOSOPHY §7).

This cluster **refuses to own**:

- **Substep work** — what one substep actually does (entry barrier,
  Jolt step call, exit barrier, contact-listener drain) is the
  `Substep` value object's responsibility (SPEC §4.1.4, sibling spike
  for the substep / mirror seam). `PhysicsWorld` brokers the call but
  authors no per-step math. The phase-3 driver (`world/phase3_driver
  .cpp`, SPEC §6.2) is the dispatch glue, not the math.
- **Body / collider lifecycle internals** — `add_body` / `remove_body`
  / `intern_shape` / `release_shape` / `intern_material` cross the
  facade as forwarding calls; the deterministic `BodyId` allocator
  (SPEC §4.1.5b), the shape-table refcount management (SPEC §4.1.6),
  and the per-body Jolt mirror live in sibling aggregates. This
  design specifies **what** the facade exposes; sibling designs
  specify **how** the underlying tables work.
- **Joint topology + constraint build** — `add_joint` / `remove_joint`
  cross the facade; the joint-kind dispatch, constraint construction,
  and break-threshold check live in `joints/` (SPEC §6.1). Same
  pattern as bodies.
- **Spatial query semantics** — `PhysicsQueries::raycast` /
  `shape_cast` / `sphere_overlap` / `capsule_sweep` / `closest_point`
  bodies live in `queries/physics_queries.cpp` (SPEC §6.1, §4.1.10).
  `PhysicsWorld::queries()` returns the resolved aggregate by
  reference; the cluster owns no query math.
- **Contact / trigger / joint-broken event payloads** — the
  `ContactListener` adapter that drains Jolt's `ContactConstraintManager`
  into ECS event buffers lives behind the `JoltMiddleman` seam
  (`middleman/contact_listener.cpp`, SPEC §6.1, §4.1.13). The cluster
  guarantees **when** the drain runs (substep exit, before phase 3
  returns; SPEC §4.1.8 invariant 2) but never the marshalling.
- **Snapshot codec mechanics** — the `PhysicsSnapshot` schema, the
  Fory writer / reader, and the determinism contract over its bytes
  live in `snapshot/` (SPEC §4.1.12, §7.1.4). `PhysicsWorld::snapshot`
  / `restore` cross the facade as forwarding calls; the cluster never
  encodes a body row itself.
- **Jolt header inclusion** — the `<Jolt/...>` headers live behind
  exactly one TU (`middleman/jolt_middleman.cpp`; SPEC §6.1 module
  rule 1, §4.1.13 invariant 1). The cluster's TUs include only the §5
  facade; the `JoltMiddleman` reference threaded through every
  forwarding call is the seam.
- **Frame schedule, ECS storage, plugin loader, hot-reload state
  machine** — owned by `core` (`reviews/decisions/frame-phases.md`,
  `reviews/decisions/hot-reload-protocol.md`,
  `reviews/decisions/plugin-abi.md`). The cluster registers no
  systems into other phases (SPEC §9.1) and exposes exactly one
  reload-time entry point (`PhysicsWorld::on_plugin_reload`, §5) that
  the loader calls during phase 8.
- **GPU work, asset bytes, scripting intents** — refused per SPEC §1
  + §3.3. The cluster reads `ExternalForce` / `ExternalTorque` /
  kinematic transform overrides at substep entry and writes
  `Velocity` / `AngularVelocity` / position back at substep exit; it
  authors no encoder, no asset blob, no script binding.

The cluster's SRP boundary is sharp: **the only reason
`PhysicsWorld`, `PhysicsConfig`, or `Accumulator` would change is a
change to "how the per-`ecs::World` Jolt instance is owned, configured,
or clocked"**. Anything else — what a substep does, what a body looks
like, what a shape is, what a query returns, what gets serialised —
routes to the sibling aggregate that owns it.

## 2. Requirements coverage

Mapping of harmonius `R-4.1.*` clauses (`rigid-body-dynamics.md`) onto
MVP coverage in this aggregate. Every entry is independently
re-derived; coverage sites refer to sections of `specs/physics/SPEC.md`
and to the design sections below. Clauses owned by sibling aggregates
within the physics context are listed as **Routed (sibling)** with the
SPEC §4.1.* row that owns them; clauses outside MVP scope are listed
as **Refused** with the §3.3 routing target the SPEC already records.

### 2.1 Aggregate-owned clauses

| Harmonius clause                                                                                                                                                       | Disposition                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
|------------------------------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **R-4.1.1** symplectic Euler with fixed-timestep accumulator producing bit-identical results across platforms                                                          | **Covered.** §3.3 `Accumulator` model + §6 concurrency contract + §3.5 determinism cell. Glibre re-derives "fixed-timestep accumulator" as a single `Accumulator` value object owning the `carry_seconds` / `tick_count` pair (§3.3); the integrator is **Jolt's**, not glibre's (SPEC §3.2 collapse #1) — Jolt's `cross_platform_deterministic` mode is the symplectic-Euler equivalent (`SetDeterministicSimulation(true)`, §6.1 rule 1). Bit-identical across hosts is the §3.5 determinism cell + the §11 byte-equal acceptance test. |
| **R-4.1.2** configurable substep count via `PhysicsConfig` resource and per-entity `SubstepOverride`                                                                   | **Covered (PhysicsConfig only) + Refused (per-entity).** `PhysicsConfig.max_substeps = 4` and `fixed_dt` are init-time-immutable (§3.2 invariant 1). Per-entity `SubstepOverride` is **refused** per SPEC §3.2 collapse #2 — forking determinism. The hard cap is the §3.3 invariant "Bounded catch-up": four substeps per frame, residual carry dropped past the cap with `Warning::AccumulatorClamped`.                                                           |
| Harmonius design — `FixedTimestep` accumulator (`accumulate` / `consume` / `alpha`)                                                                                    | **Covered.** §3.3 `Accumulator` model. `consume` is the substep loop body (§3.4 step 4); `alpha` is exposed via `Accumulator::carry()` for render's interpolation (read by phase 6 per `frame-phases.md`).                                                                                                                                                                                                                                                                                                                                                        |
| Harmonius design — fixed-timestep with bounded catch-up (max ticks per frame) preventing spiral of death                                                                | **Covered.** §3.3 invariant "Bounded catch-up" — hard cap at four substeps, residual carry **dropped** (carry reset to zero), `Warning::AccumulatorClamped` logged. The simulation never busy-waits inside phase 3 (PHILOSOPHY §7 — determinism over wall-clock fidelity under stall).                                                                                                                                                                                                              |
| Harmonius design — accumulator carry preserved across frames                                                                                                            | **Covered.** §3.3 invariant "Carry preserved" — the residual `acc < dt` carries into the next frame verbatim. Persisted into `PhysicsSnapshot.accumulator_carry` (SPEC §7.1.4 tag 4) so a replay restart resumes with the same remainder.                                                                                                                                                                                                                                                              |
| Harmonius design — per-world physics configuration (`PhysicsConfig` resource)                                                                                           | **Covered.** §3.2 `PhysicsConfig` model. One `PhysicsConfig` per `PhysicsWorld`, captured by value at init, immutable for the world's lifetime. Re-tuning is a fresh-world event (§3.2 invariant "Init-time-immutable").                                                                                                                                                                                                                                                                              |
| Harmonius design — `PhysicsWorld` per ECS `World` (one Jolt instance per scene)                                                                                         | **Covered.** §3.1 composition + §3.4 lifecycle. SPEC §3.2 collapse #8: one `PhysicsWorld` per `ecs::World`, period. Multi-zone / multi-planet / 2D-vs-3D topology is post-MVP; the §5 `create` signature already takes `ecs::World&` so additional worlds are additional construct calls, not a new topology.                                                                                                                                                          |

### 2.2 Sibling-aggregate clauses (routed within the physics context)

These clauses are MVP-scope but their reason-to-change is owned by a
sibling aggregate. Each routes to the SPEC §4.1.* row that owns it;
the corresponding sibling design spike fills in the body. Listed here
so the cluster's seam shape is exhaustive.

| Harmonius clause                                                                                                                          | Routed to (sibling SPEC row)                                                              |
|-------------------------------------------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------|
| **R-4.1.3** impulse-based contact resolution with `PhysicsMaterial` restitution + friction                                                | §4.1.4 `Substep` + §4.1.6 `Collider` + §4.1.8 `ContactManifold`                           |
| **R-4.1.4** CCD swept-volume time-of-impact for tunneling-prone bodies                                                                    | §4.1.15 `CCD` flag + §4.1.4 `Substep` (Jolt-internal swept narrowphase)                   |
| **R-4.1.5** simulation islands via union-find, parallel solve                                                                             | §4.1.14 `Island` (read-only diagnostic surface; Jolt owns the union-find)                 |
| **R-4.1.6** sleep at rest, wake on external force / torque / new contact                                                                  | §4.1.14 `Sleeping` + §4.1.5 `RigidBody` (sleep marker on the entity)                      |
| Harmonius design — broadphase + narrowphase + integrator + solver                                                                          | §4.1.4 `Substep` (`JoltMiddleman::step` — the entire kernel collapses to one Jolt call)   |
| Harmonius design — `RigidBody` / `Velocity` / `AngularVelocity` / `ExternalForce` / `ExternalTorque` ECS components                       | §4.1.5 `RigidBody` (§5 declares the components; ECS↔Jolt mirror is sibling design)        |
| Harmonius design — `Collider` + collision-shape taxonomy (sphere / box / capsule / convex / mesh / heightfield / compound)                | §4.1.6 `Collider` / `ShapeHandle` / `ShapeBlob`                                           |
| Harmonius design — joint family (`Fixed` / `Revolute` / `Prismatic` / `Distance` / `Generic6Dof`)                                          | §4.1.7 `Joint` (sealed sum mirroring Jolt's family per SPEC §3.2 collapse #1)             |
| Harmonius design — `CollisionStarted` / `CollisionPersisted` / `CollisionEnded` / `TriggerEnter` / `TriggerStay` / `TriggerExit` events    | §4.1.8 `ContactManifold` / `ContactEvent`, §4.1.9 `Trigger` / `TriggerEvent`              |
| Harmonius design — spatial-query surface (`RayCast` / `ShapeCast` / `Overlap` / closest-point)                                            | §4.1.10 `PhysicsQueries`                                                                  |
| Harmonius design — `PhysicsSnapshot` Fory schema for replay                                                                                | §4.1.12 `PhysicsSnapshot` + SPEC §7.1.4                                                   |
| Harmonius design — `JoltMiddleman` ABI seam                                                                                                | §4.1.13 `JoltMiddleman`                                                                   |
| **R-4.1.NF3** byte-identical results across hosts                                                                                          | §6 concurrency rules + §3.5 determinism cell + §11 acceptance fixture                     |

### 2.3 Refused clauses (out of MVP scope)

Refused per SPEC §3.3; the §3.3 routing target the SPEC already
records is repeated here for the cluster's surface.

| Harmonius clause                                                                                                                                | Routing target                                                                  |
|-------------------------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------|
| **R-4.1.7** cross-streaming-zone migration preserving momentum + contact state                                                                  | Post-MVP world-streaming context; orthogonal to the §1 single-world topology    |
| **R-4.1.8** kinematic character controller (ground detection, slope sliding, step climbing, moving platforms, coyote time)                      | Post-MVP `character` plugin; consumes `PhysicsQueries` shape casts              |
| **R-4.1.9** moving platforms with one-way filtering                                                                                              | Post-MVP `character` plugin                                                     |
| **R-4.1.10** character ground smoothing on tessellated terrain                                                                                   | Post-MVP `character` plugin                                                     |
| **R-4.1.11** gyroscopic torque (`tau = omega × (I * omega)`) under a `gyroscopic` flag                                                            | Reachable as a Jolt-feature follow-up; not in MVP §1                            |
| **R-4.1.12** rolling friction torque on `PhysicsMaterial`                                                                                         | Same — not in MVP §1                                                            |
| **R-4.1.13** directional friction with primary axis + lateral coefficient                                                                          | Same — not in MVP §1                                                            |
| **R-4.1.14** per-world `GravityMode` (`Uniform` / `Radial` / custom function)                                                                     | Deferred; MVP `PhysicsConfig.gravity` is a fixed `Vec3`                         |
| **R-4.1.15** multi-planet worlds with universe-level Euclidean transforms + cross-world joint break                                               | Post-MVP world-streaming + multi-world; orthogonal to §1                        |
| **R-4.1.16** 2D rigid-body mode with scalar inertia + 2D collider shapes + separate 2D BVH + 2-DoF solver                                          | Post-MVP `physics-2d` (parallel `PhysicsWorld` over Jolt 2D primitives)         |
| **R-4.1.17** wall sliding with wall-friction + wall-angle threshold                                                                               | Post-MVP `character`                                                            |
| **R-4.1.18** multi-jump + wall jump + jump buffer                                                                                                  | Post-MVP `character`                                                            |
| **R-4.1.19** crouching with ceiling-clearance shape cast                                                                                           | Post-MVP `character`                                                            |
| **R-4.1.20** push forces from character controller to dynamic bodies                                                                              | Post-MVP `character`                                                            |
| **R-4.1.NF1** 2 000 active bodies × 4 substeps within 4 ms on min-spec                                                                            | **Reframed in §9.** The MVP S1 fixture is ~30 active bodies + 200 mostly-sleeping props (`perf-budget.md` §"Justification Per Cell"); the cell budgets 2.00 ms across two-substep upper bound. The 2 000-body figure is post-MVP scaling; revisit when an MVP workload demonstrates active-body counts above S1's. |
| **R-4.1.NF2** ≤ 256 bytes per active rigid body (component memory layout)                                                                          | **Refused as an MVP gate.** Replaced by the §9 heap cell (`Jolt body + constraint pools`, 64 MiB sub-share inside the 128 MiB context cell); per-body byte budgeting reintroduces a second source of truth. The component-byte layout is sibling-aggregate territory (§4.1.5 `RigidBody`). |
| **R-4.1.NF4** ≤ 0.1 ms per character controller, 200 simultaneous                                                                                  | Post-MVP `character`; consumes the `PhysicsQueries` 0.20 ms reserve in §9.2     |

### 2.4 Coverage summary

Of the 24 harmonius `R-4.1.*` clauses (R-4.1.1 .. R-4.1.20 + four
non-functional rows):

- **2 covered** as `PhysicsWorld` / `PhysicsConfig` / `Accumulator`
  design here (R-4.1.1 and R-4.1.2 PhysicsConfig half).
- **6 routed to sibling aggregates** within the physics context (the
  R-4.1.3 / R-4.1.4 / R-4.1.5 / R-4.1.6 dynamics row + the harmonius
  design rows for `RigidBody` ECS components, joints, contact events,
  spatial queries, snapshot, middleman).
- **16 refused with rationale** — 14 deferred post-MVP (the character
  / multi-world / 2D / advanced-friction surfaces; R-4.1.NF1 / NF4
  scaling targets), 2 reframed against the §9 cell (R-4.1.NF1 active
  body count, R-4.1.NF2 per-body memory budget).

This list is closed; PRs adding any of the refused clauses to the
`physics-world` cluster should be rejected and routed to the listed
owner.

## 3. Detailed model

The model below is the implementer's authority for the
`world/physics_world.cpp`, `world/physics_config.cpp`,
`world/accumulator.cpp`, and `world/phase3_driver.cpp` TUs (SPEC
§6.1). Each subsection owns one of the three primitives the cluster
composes; the SPEC §4.1.* invariants the primitive enforces are cited
per subsection.

### 3.1 Composition and module boundary

```text
physics/src/world/                       (private headers; not on plugin include path)
├── physics_world.{hpp,cpp}              (§3.4  ─ PhysicsWorld root entity; facade)
├── physics_config.{hpp,cpp}             (§3.2  ─ PhysicsConfig view + content_hash compute)
├── accumulator.{hpp,cpp}                (§3.3  ─ Fixed-dt clock; phase-3 entry / exit)
└── phase3_driver.{hpp,cpp}              (§3.4  ─ The phase-3 body — substep loop dispatch)

physics/include/glibre/physics/
└── physics.hpp                          (§5 facade header; SPEC §5 — locked surface)
```

`physics_world.hpp` is the only header outside `physics/src/world/`
permitted to include `physics_config.hpp` / `accumulator.hpp` /
`phase3_driver.hpp`. The CMake visibility rule cited by SPEC §6.1
module rule 3 enforces this: a sibling module's `.cpp` may include
its own `.hpp`s and the §5 facade only — cross-module reach-throughs
are forbidden, and the seam is `world/physics_world.hpp`.

The four TUs decompose by SRP (PHILOSOPHY §1):

- **`physics_config.cpp`** — one reason to change: the deterministic
  knob set + the `content_hash` compute (BLAKE3 over canonicalised
  bytes; SPEC §7.1.1, §3.2 invariant "Init-time-immutable").
- **`accumulator.cpp`** — one reason to change: how wall-clock dt
  becomes a deterministic substep count (SPEC §4.1.3).
- **`phase3_driver.cpp`** — one reason to change: how substeps are
  ordered + dispatched inside phase 3 (SPEC §6.2). Drives the
  accumulator and brokers each substep's barrier work; never authors
  per-substep math.
- **`physics_world.cpp`** — one reason to change: the facade's
  composition + lifecycle (`create`, destructor, hot-reload entry
  point, sibling-aggregate accessor wiring).

A second TU per primitive is forbidden; one reason to change → one
TU. The §5 facade lives in a single header for the same reason
(SPEC §5 preamble — the contract is one document, not many).

### 3.2 `PhysicsConfig` (§4.1.2; covers R-4.1.2 PhysicsConfig half)

A flat record captured by value at world construction. Every field is
init-time-immutable (§3.2 invariant 1); a runtime tuning request is a
fresh-world event, not an in-place mutation (SPEC §3.2 collapse #2).

```cpp
// physics/src/world/physics_config.hpp — internal view of the §5 PhysicsConfig
namespace glibre::physics::detail {

struct PhysicsConfigView {
    // ---- Stepping math (deterministic core) --------------------------
    Vec3            gravity;                    // §5: PhysicsConfig.gravity
    float           fixed_dt;                   // §5: PhysicsConfig.fixed_dt; bit-exact f32
    std::uint8_t    max_substeps;               // §5: PhysicsConfig.max_substeps; cap = 4
    std::uint8_t    velocity_iters;             // §5: PhysicsConfig.velocity_iters
    std::uint8_t    position_iters;             // §5: PhysicsConfig.position_iters
    float           warm_start_factor;          // §5: PhysicsConfig.warm_start_factor
    bool            ccd_enabled;                // §5: PhysicsConfig.ccd_enabled
    std::uint64_t   rng_seed;                   // §5: PhysicsConfig.rng_seed

    // ---- Sleep thresholds (§4.1.14) ----------------------------------
    float           sleep_linear_speed;
    float           sleep_angular_speed;
    std::uint16_t   sleep_frame_count;

    // ---- World budgets (§4.1.2 composition) --------------------------
    std::uint32_t   max_bodies;
    std::uint32_t   max_shapes;
    std::uint32_t   max_constraints;
    std::uint32_t   max_contacts;

    // ---- Layer filter (§4.1.2 invariant 4, §4.1.11) ------------------
    eastl::shared_ptr<LayerFilter> layer_filter; // opaque body in physics dylib

    // ---- Determinism gate (§10.3 CI-vs-shipping split) ---------------
    DeterminismGate determinism_gate;            // Hard | SoftWarn

    // ---- Content hash (§4.1.2 invariant 2; §7.1.1) -------------------
    std::uint64_t   content_hash;                // BLAKE3 over canonicalised bytes
};

}  // namespace
```

Composition rationale (one row per field cluster):

1. **Stepping math.** `gravity`, `fixed_dt`, `max_substeps`,
   `velocity_iters`, `position_iters`, `warm_start_factor`,
   `ccd_enabled`, `rng_seed` are the "what advances the sim clock"
   knobs; a single primitive (the `Accumulator` + Jolt's solver
   config) consumes all of them. SPEC §3.2 collapse #2 collapsed
   harmonius's selectable integrators / per-tier iteration counts /
   `SolverConfig` / per-platform LOD into this one record.
2. **Sleep thresholds.** Per-world thresholds; per-body overrides are
   refused (SPEC §3.2 collapse #6). The trio is consumed by Jolt's
   sleep system at world construction and never re-read at runtime.
3. **World budgets.** Sized at world init; the §5 `BudgetExceeded`
   arm fires when an `add_body` / `add_collider` / `add_joint` /
   `intern_shape` would overflow the cap (SPEC §10.1 row
   "BudgetExceeded"). Budget changes are fresh-world events.
4. **Layer filter.** Owns the `CollisionLayer → BroadphaseLayer`
   mapping table + the layer-pair interaction matrix. The §5
   `LayerFilter::validate()` totality check is the §4.1.2 invariant 4
   enforcement point — a partial matrix returns `ConfigInvalid` at
   `PhysicsWorld::create`.
5. **Determinism gate.** Two-value enum `Hard` / `SoftWarn`; default
   `Hard` under `-DGLIBRE_DETERMINISM_GATE=hard` (set by the CI
   workflow), `SoftWarn` otherwise. SPEC §10.3 documents the
   CI-vs-shipping severity split. The knob is part of the
   `content_hash` (§7.1.1), so flipping it mid-trace is a fresh-world
   event — the gate setting is part of the determinism contract.
6. **`content_hash`.** BLAKE3 over the canonicalised serialised bytes
   of every preceding field (SPEC §7.1.1 invariant 3 — `content_hash`
   is computed, not stored in the `.fory` record). Computed once at
   `PhysicsConfig` construction by `physics_config.cpp`'s
   `compute_content_hash(...)` helper; cached in the `View` thereafter.
   Used by `PhysicsSnapshot.physics_config_hash` (SPEC §7.1.4 invariant
   3) for cross-build snapshot-restore validation.

Public-boundary invariants (SPEC §4.1.2; restated for the implementer):

1. **Init-time-immutable** — no public API mutates a `PhysicsConfig`
   after `PhysicsWorld::create` returns. The runtime view above is
   `const`-stamped from the value-captured `PhysicsConfig` parameter
   to `create`; the parameter itself is captured by value to prevent
   aliasing into caller storage.
2. **Deterministic by construction** — every field that influences
   stepping math is fixed-point or bit-exact float (`f32`); host-tier
   branches and platform intrinsics are absent (PHILOSOPHY §6 + §7,
   SPEC §6.4 rule 3). The `content_hash` BLAKE3 covers exactly those
   bytes; two builds writing the same `PhysicsConfig` produce
   byte-equal `content_hash` on every supported host.
3. **Single source of global knobs** — every init-time-immutable
   simulation knob lives here; per-body knobs (motion type, mass,
   damping, sleeping flag, CCD flag) live on `RigidBody` (§4.1.5);
   per-shape knobs (layer, density, material) live on `Collider`
   (§4.1.6). No knob is duplicated. SPEC §3.2 collapse #6.
4. **Layer matrix is total** — `LayerFilter::validate()` walks the
   mapping at world init; an incomplete matrix returns
   `physics::Error::ConfigInvalid` and the world is **refused** (no
   half-built `PhysicsWorld` is published).

### 3.3 `Accumulator` (§4.1.3; covers R-4.1.1 fixed-step, harmonius FixedTimestep)

The fixed-timestep clock that converts wall-clock frame `dt` into a
deterministic count of substeps. One per `PhysicsWorld`; lives inside
the world for its entire lifetime; serialised into `PhysicsSnapshot`
so a replay restart resumes with the same remainder (SPEC §4.1.3
identity & lifetime, §4.1.12 / §7.1.4 schema tag 4).

```cpp
// physics/src/world/accumulator.hpp — internal view of the §5 Accumulator
namespace glibre::physics::detail {

class AccumulatorImpl {
public:
    explicit AccumulatorImpl(float fixed_dt) noexcept
        : fixed_dt_{fixed_dt}, carry_{0.0f}, tick_count_{0u} {}

    // Drive: returns AdvanceReport { substeps_run, substeps_dropped, carry_seconds }.
    [[nodiscard]] AdvanceReport advance(float real_dt,
                                        std::uint8_t max_substeps) noexcept;

    [[nodiscard]] float          carry()      const noexcept { return carry_; }
    [[nodiscard]] std::uint64_t  tick_count() const noexcept { return tick_count_; }

    // Snapshot / restore — used by §3.4 PhysicsWorld::snapshot/restore.
    [[nodiscard]] float          snapshot_carry() const noexcept { return carry_; }
    [[nodiscard]] std::uint64_t  snapshot_tick()  const noexcept { return tick_count_; }
    void                         reseed(float carry,
                                        std::uint64_t tick_count) noexcept;

private:
    float          fixed_dt_;     // immutable; mirror of PhysicsConfig.fixed_dt
    float          carry_;        // [0, fixed_dt) at every public-boundary exit
    std::uint64_t  tick_count_;   // monotonic; only `advance` increments
};

}  // namespace
```

The `advance` body — the **only** loop that calls
`JoltMiddleman::step` (§3.4 step 4):

```text
AccumulatorImpl::advance(real_dt, max_substeps):
  carry_ += real_dt
  AdvanceReport r{ .substeps_run = 0, .substeps_dropped = 0 }
  while carry_ >= fixed_dt_ AND r.substeps_run < max_substeps:
    // Each iteration: §3.4 step 4 dispatches one substep.
    // The driver, NOT the accumulator, calls JoltMiddleman::step.
    yield substep slot                      // §3.4 step 4 hooks here
    carry_  -= fixed_dt_
    tick_count_ += 1
    r.substeps_run += 1
  if carry_ >= fixed_dt_:                   // residual past the cap
    // Bounded catch-up: drop residual carry, log Warning::AccumulatorClamped.
    r.substeps_dropped = floor(carry_ / fixed_dt_)
    carry_ = 0.0f
  r.carry_seconds = carry_
  return r
```

Public-boundary invariants (SPEC §4.1.3; restated for the implementer):

1. **Single owner of phase-3 advancement** — only `AccumulatorImpl::
   advance` advances the simulation clock. No plugin, gameplay
   system, or test harness may call into Jolt's `Step` outside this
   loop (SPEC §3.2 collapse #2). The `JoltMiddleman::step` function
   is package-private (CMake visibility) so that even a buggy sibling
   TU cannot call it directly.
2. **Bounded catch-up** — the substep loop is hard-capped at four
   substeps per frame (`max_substeps = 4` from `PhysicsConfig`).
   Residual carry above that is **dropped** (`carry_ = 0.0f`), and a
   `physics::Warning::AccumulatorClamped` is logged via the engine's
   `glibre::log_error` helper at `warn` level (SPEC §10.1). The
   simulation never falls behind by more than four substeps' worth
   of wall-clock work, and phase 3 never busy-waits (PHILOSOPHY §7 —
   determinism over wall-clock fidelity under stall).
3. **Carry preserved** — whatever `acc < fixed_dt_` remains after the
   draining loop is preserved verbatim into the next frame. It is
   the determinism unit that makes spike-induced frame-rate variation
   re-converge to the same trajectory across hosts.
4. **No re-entry** — phase 3 is a single barrier per
   `frame-phases.md`; the accumulator's draining loop is the only
   loop that re-enters Jolt's `Step` within a frame, and it never
   re-enters phases 2–5 as a sub-graph (SPEC §3.2 collapse #2,
   `frame-phases.md` open question 2).
5. **Snapshot round-trip preserves both carry and tick** —
   `snapshot_carry()` + `snapshot_tick()` are written into
   `PhysicsSnapshot.accumulator_carry` (tag 4) + `world_tick` (tag 3)
   at drain (SPEC §7.1.4); `reseed(carry, tick)` restores both at
   resume (§3.4 step "Restore"). Two snapshots taken at the same
   logical tick on different hosts are byte-equal at the accumulator
   columns (SPEC §6.4 rule 4 — `f32` carry serialised via
   `std::bit_cast<u32>`; `u64` tick serialised verbatim).

The `tick_count_` field is monotonic — the only path that increments
it is `advance`'s substep-loop body. `reseed` overwrites both fields
to their pre-snapshot values during hot-reload restore (§3.4 step
"Restore"); this is the **only** non-monotonic write, and it is
permitted because the post-restore tick count is exactly the
pre-snapshot tick count (the snapshot bytes encode it). A test-only
`force_reset()` is **refused** — there is no MVP fixture that needs
to reset the clock without a fresh world.

### 3.4 `PhysicsWorld` root entity (§4.1.1; covers harmonius "PhysicsWorld per scene")

The aggregate-root facade. Composes `PhysicsConfig` (§3.2) +
`Accumulator` (§3.3) + every sibling-aggregate seam into the §5
public surface. One `PhysicsWorld` per `ecs::World`; created at world
init from a frozen `PhysicsConfig`, destroyed at world teardown.

#### 3.4.1 Composition (one Jolt instance + sibling-aggregate seams)

```cpp
// physics/src/world/physics_world.hpp — internal composition
namespace glibre::physics::detail {

class PhysicsWorldImpl {
public:
    [[nodiscard]] static Result<eastl::unique_ptr<PhysicsWorldImpl>>
        create(ecs::World&, PhysicsConfig config) noexcept;
    ~PhysicsWorldImpl();

    // Phase-3 entry — drives the accumulator + dispatches substeps.
    [[nodiscard]] Result<AdvanceReport> advance(float real_dt) noexcept;

    // Sibling-aggregate accessors (forward to opaque tables).
    [[nodiscard]] PhysicsQueries&         queries()        noexcept;
    [[nodiscard]] AccumulatorImpl&        accumulator()    noexcept;
    [[nodiscard]] const PhysicsConfigView& config()        const noexcept;
    [[nodiscard]] std::uint64_t           world_tick()    const noexcept;

    // Body / collider / joint / shape / material lifecycle — forward to siblings.
    [[nodiscard]] Result<BodyId>     add_body(ecs::Entity, const RigidBody&, const Collider&) noexcept;
    [[nodiscard]] Result<void>       remove_body(BodyId) noexcept;
    [[nodiscard]] Result<JointId>    add_joint(const JointEndpoints&,
                                               const JointLimits*,
                                               const JointMotor*,
                                               const JointBreakThreshold*) noexcept;
    [[nodiscard]] Result<void>       remove_joint(JointId) noexcept;
    [[nodiscard]] Result<ShapeHandle> intern_shape(const ShapeBlob&) noexcept;
    [[nodiscard]] Result<void>       release_shape(ShapeHandle) noexcept;
    [[nodiscard]] Result<MaterialId> intern_material(const PhysicsMaterial&) noexcept;

    // Snapshot / restore — forward to PhysicsSnapshot codec sibling.
    [[nodiscard]] Result<eastl::unique_ptr<PhysicsSnapshot>> snapshot() const noexcept;
    [[nodiscard]] Result<void>                               restore(const PhysicsSnapshot&) noexcept;

    // Hot-reload — see §8.
    [[nodiscard]] Result<void> on_plugin_reload(const JoltMiddleman&) noexcept;

private:
    explicit PhysicsWorldImpl(ecs::World&,
                              PhysicsConfigView,
                              const JoltMiddleman&) noexcept;

    // Owned in declaration order = teardown order (reverse).
    ecs::World&                          ecs_world_;          // borrow; lifetime ≥ this
    const JoltMiddleman&                 middleman_;          // borrow; process-resident
    PhysicsConfigView                    config_;             // by value; immutable
    AccumulatorImpl                      accumulator_;
    eastl::unique_ptr<JoltSystemHandle>  jolt_system_;        // opaque pointer to Jolt PhysicsSystem
    eastl::unique_ptr<ShapeTable>        shape_table_;        // §4.1.6 sibling
    eastl::unique_ptr<MaterialTable>     material_table_;     // PhysicsMaterial sibling
    eastl::unique_ptr<BodyIdAllocator>   body_id_allocator_;  // §4.1.5b sibling
    eastl::unique_ptr<JointRegistry>     joint_registry_;     // §4.1.7 sibling
    eastl::unique_ptr<ContactDrain>      contact_drain_;      // §4.1.8 sibling
    eastl::unique_ptr<PhysicsQueries>    queries_;            // §4.1.10 sibling
};

}  // namespace
```

Field-by-field rationale (declaration order is teardown-reverse —
PHILOSOPHY §1 + RAII):

1. **`ecs_world_` (borrow).** The host ECS world; lifetime ≥
   `PhysicsWorldImpl`. Used by the §3.6 ECS↔Jolt mirror barriers to
   read the `RigidBody` / `Collider` / `Joint` archetypes and write
   back `Velocity` / `AngularVelocity` / `Sleeping` markers.
   Borrowed, not owned — the world is the caller's, and a destroy of
   the ECS world before the `PhysicsWorld` is a contract violation
   the loader catches at phase 8.
2. **`middleman_` (borrow).** The single Jolt-derived ABI seam (SPEC
   §4.1.13). Process-resident; `JoltMiddleman::load_from_engine()`
   returns a `const JoltMiddleman*` borrowed from the engine.
   Borrowed, not owned — the middleman dylib outlives every physics
   plugin instance per PHILOSOPHY §9 and `hot-reload-protocol.md`.
3. **`config_` (by value).** `PhysicsConfigView` — see §3.2.
   Captured by value at `create`; immutable.
4. **`accumulator_` (by value).** `AccumulatorImpl` — see §3.3. One
   per world; lifetime = world's lifetime.
5. **`jolt_system_` (owned, opaque).** Handle to the Jolt
   `PhysicsSystem`. The pointer is owned through a sibling-typed
   handle (`JoltSystemHandle`) whose definition lives behind the
   `JoltMiddleman` seam — this TU never includes `<Jolt/...>` (SPEC
   §6.1 module rule 1, §4.1.13 invariant 1). The handle's destructor
   is the middleman's `destroy_world(...)` call and runs before the
   sibling tables' destructors so Jolt's body / constraint manager
   can release pointers into them.
6. **`shape_table_`, `material_table_`, `body_id_allocator_`,
   `joint_registry_`, `contact_drain_`, `queries_`.** Sibling
   aggregates owned by this facade. Each has its own design spike
   under #791; this design treats each as opaque. The order is
   important: shapes + materials come up before bodies (a body needs
   its shape table to add a collider), bodies come up before joints
   (a joint needs its endpoint bodies), contact drain comes up after
   bodies + joints (its listener registers against Jolt's contact
   manager which knows the body table), queries come up last (the
   query surface is a thin shim over Jolt's broadphase, populated
   after every body / shape / material is in place).
7. **No back-pointer to `PhysicsConfig` from the accumulator.** The
   accumulator stores `fixed_dt` by value at construction (§3.3
   field `fixed_dt_`); SPEC §3.2 invariant 1 makes the source
   immutable, so the by-value capture is byte-equal to the live
   config and safer than a borrow that the destruction order has to
   track.

#### 3.4.2 `create` — fail-fast world construction

`PhysicsWorld::create(ecs::World&, PhysicsConfig)` is the **only**
public entry point that allocates Jolt state. Failure modes are
total at construction; a half-built world is never published.

```text
PhysicsWorldImpl::create(ecs_world, config):
  // Step 1 — Validate PhysicsConfig totality.
  if config.fixed_dt <= 0 OR
     config.max_substeps == 0 OR
     config.budgets.max_bodies == 0 OR
     ... (every field's pre-condition; see §10):
    return std::unexpected{ Error::ConfigInvalid }
  if config.layer_filter == nullptr OR
     config.layer_filter->validate() returns unexpected:
    return std::unexpected{ Error::ConfigInvalid }      // §4.1.2 inv 4

  // Step 2 — Resolve JoltMiddleman.
  middleman <- JoltMiddleman::load_from_engine()
  if middleman is unexpected:
    return std::unexpected{ Error::JoltMiddlemanUnavailable }
  if middleman->require_hash(host_abi_hash) returns unexpected:
    return std::unexpected{ Error::JoltMiddlemanHashMismatch }   // §4.1.13 inv 2

  // Step 3 — Compute PhysicsConfigView.content_hash.
  view <- PhysicsConfigView::from(config)
  view.content_hash <- BLAKE3(canonicalise(view))                // §7.1.1 inv 3

  // Step 4 — Allocate sibling tables (each can fail with BudgetExceeded).
  shape_table       <- ShapeTable::create(view.budgets.max_shapes)
  material_table    <- MaterialTable::create()
  body_id_allocator <- BodyIdAllocator::create(view.budgets.max_bodies)
  joint_registry    <- JointRegistry::create(view.budgets.max_constraints)
  contact_drain     <- ContactDrain::create(view.budgets.max_contacts)

  // Step 5 — Construct Jolt PhysicsSystem through the middleman.
  jolt_system <- middleman->create_world(view, *body_id_allocator, *contact_drain)
  if jolt_system is unexpected:
    // Roll back sibling allocations; PhysicsSystem creation can fail
    // on Jolt-internal allocation hooks busting ContextTag::physics.
    return std::unexpected{ ... }                                // forwarded error

  // Step 6 — Construct PhysicsQueries against the live broadphase.
  queries <- PhysicsQueries::create(*jolt_system, *middleman)

  // Step 7 — Publish.
  return eastl::make_unique<PhysicsWorldImpl>(
    ecs_world, view, *middleman,
    std::move(accumulator{view.fixed_dt}),
    std::move(jolt_system), std::move(shape_table), ...
  )
```

Public-boundary invariants on `create` (SPEC §4.1.1; restated):

1. **Owns frame phase 3 entirely** — once `create` returns, every
   byte of work that advances the simulation lives inside phase 3 of
   the owning `World`'s frame loop (`reviews/decisions/frame-phases
   .md`). The phase-3 driver (§3.4.4 below) is the only consumer of
   `PhysicsWorld::advance`; sibling contexts' phase bodies cannot
   reach into Jolt.
2. **One Jolt `PhysicsSystem` per world, period.** The world holds
   no second simulation kernel and exposes no second Jolt instance.
   A second `create` against the same `ecs::World` returns
   `Error::WorldAlreadyInitialised` (SPEC §10.1).
3. **No exception path.** `create` is `noexcept`; every fallible
   step returns `Result<T>` and propagates via the monadic chain.
   Exceptions thrown by Jolt during `create_world` are caught at
   the `JoltMiddleman` ingress (`middleman/exception_wrapper.cpp`,
   SPEC §6.1 module rule 2) and translated into `physics::Error`
   arms before crossing the seam (SPEC §4.1.13 invariant 3).
4. **ECS↔Jolt mirror is one-way per substep** — set up at
   construction; the driver enforces the rule per substep (§3.4.4).

#### 3.4.3 `advance` — phase-3 entry point

The single per-frame call that drives the whole cluster. Called
exactly once per frame by the phase-3 driver (`world/phase3_driver.cpp`,
SPEC §6.2). Walks the accumulator's draining loop and, for each
substep, dispatches the §3.6 mirror-barrier sequence + the
`JoltMiddleman::step` call.

```text
PhysicsWorldImpl::advance(real_dt):
  if not in phase 3:                        // §4.1.1 inv 1; §10.1 row
    return std::unexpected{ Error::StepCalledOutsidePhase3 }

  // Substep loop — see §3.4.4 driver for the per-substep body.
  report <- AdvanceReport{ substeps_run = 0, substeps_dropped = 0, carry_seconds = 0 }
  accumulator_.carry_ += real_dt
  while accumulator_.carry_ >= config_.fixed_dt
    AND report.substeps_run < config_.max_substeps:
      // step_one is the §3.4.4 driver body — never another caller.
      step_result <- step_one()             // = entry barrier + middleman.step + exit barrier
      if step_result is unexpected:
        // Determinism / numerical-instability arms abort the substep before commit.
        // Carry is NOT decremented; tick is NOT advanced.
        return std::unexpected{ step_result.error() }   // §10.1 NumericalInstabilityDetected etc.
      accumulator_.carry_       -= config_.fixed_dt
      accumulator_.tick_count_  += 1
      report.substeps_run       += 1
  if accumulator_.carry_ >= config_.fixed_dt:
    report.substeps_dropped = floor(accumulator_.carry_ / config_.fixed_dt)
    accumulator_.carry_     = 0.0f
    log Warning::AccumulatorClamped { real_dt, dropped = report.substeps_dropped }
    if PhysicsConfig.determinism_gate == DeterminismGate::Hard:
      return std::unexpected{ Error::AccumulatorClampExceeded }   // §10.1; CI promotes
  report.carry_seconds = accumulator_.carry_
  return report
```

The phase-3 guard at the top is the §10.1 `StepCalledOutsidePhase3`
detection point. The frame-phase context is read from `core`'s
`FrameLoop` thread-local (a single `enum class Phase` set by the
loop driver at phase entry / cleared at phase exit; see SPEC §6.2 +
`reviews/decisions/frame-phases.md`); `PhysicsWorld` does not own
the guard, but it consults it.

The accumulator-clamp branch is the §10.1 `AccumulatorClampExceeded`
detection point. Under `DeterminismGate::SoftWarn` (default in
shipping) the call returns the report with `substeps_dropped > 0`
and the warning logged; under `Hard` (default in CI) it returns
the typed error so the determinism-gate fixture catches a sustained
spike. Both modes drop the residual carry — the determinism trade
is the same; only the surface differs (SPEC §10.3).

Every `step_one` failure (numerical instability, mirror inversion,
determinism check) **rolls back** the substep before returning
(§3.4.4 step 6). The accumulator carry / tick are not advanced,
so the next call to `advance` re-attempts the same substep with
the same inputs — typical recovery is "operator fixes the gameplay
input that produced the NaN; the next frame catches up".

#### 3.4.4 Phase-3 driver substep body

The driver is `world/phase3_driver.cpp` (SPEC §6.1, §6.2). Its body
is one `step_one()` call per substep slot in the accumulator's loop;
each call is the SPEC §6.2 five-step pipeline. The driver authors
no per-step math — it sequences the barrier work plus the
`JoltMiddleman::step` call, and returns the substep's `Result<void>`.

```text
PhysicsWorldImpl::step_one():
  // Step 1 — Entry barrier (§3.6.1 below; sibling RigidBody/Joint design owns details).
  // Walks the RigidBody archetype in BodyId-ascending order, drains
  // ExternalForce/ExternalTorque, applies kinematic transform overrides,
  // applies joint motor targets. Sets the entry-barrier flag for §10
  // SubstepEcsCommitInverted detection.
  set_substep_entry_barrier_flag()
  drain_external_force_torque(*body_id_allocator_, *middleman_, ecs_world_)
  apply_kinematic_overrides(*body_id_allocator_, *middleman_, ecs_world_)
  apply_joint_motor_targets(*joint_registry_, *middleman_, ecs_world_)
  clear_substep_entry_barrier_flag()

  // Step 2 — Jolt step (the only middleman.step call site).
  set_substep_in_flight_flag()
  jolt_step_result <- middleman_->step(
    *jolt_system_,
    config_.fixed_dt,
    config_.velocity_iters,
    config_.position_iters,
    /* job_system = */ deterministic_pool_.get())
  clear_substep_in_flight_flag()
  if jolt_step_result is unexpected:
    // Translated from any thrown JPH::* exception by exception_wrapper.cpp.
    return std::unexpected{ jolt_step_result.error() }

  // Step 3 — Determinism / numerical-instability barrier
  // (§3.5; §6.4 rule 4 IEEE-754 bit-equal storage).
  nan_check <- check_no_nan_inf_in_jolt_outputs(*jolt_system_, *middleman_)
  if nan_check is unexpected:
    rollback_jolt_step(*jolt_system_, *middleman_)        // restore pre-step state
    return std::unexpected{ Error::NumericalInstabilityDetected }

  // Step 4 — Exit barrier (§3.6.2 below).
  // Walks the same archetype in the same order; writes back Velocity /
  // AngularVelocity / RigidBody.position/.rotation; updates Sleeping markers;
  // drains contact-listener pairs into ECS event buffers.
  set_substep_exit_barrier_flag()
  write_back_kinematic_state(*body_id_allocator_, *middleman_, ecs_world_)
  update_sleeping_markers(*body_id_allocator_, *middleman_, ecs_world_)
  contact_drain_->drain(*middleman_, ecs_world_)
  evaluate_joint_break_thresholds(*joint_registry_, *middleman_, ecs_world_)
  clear_substep_exit_barrier_flag()

  // Step 5 — Optional determinism check (Hard mode).
  if config_.determinism_gate == DeterminismGate::Hard:
    iter_hash <- compute_post_step_iteration_hash(*body_id_allocator_, ecs_world_)
    if iter_hash != expected_iter_hash():
      return std::unexpected{ Error::DeterminismCheckFailed }

  return {}
```

Properties this driver guarantees (SPEC §4.1.4 invariants 1–4 +
§4.2 invariants 1–3 + §6.3 mirror seam):

1. **One Jolt `Step` per substep.** Step 2 is the only call to
   `middleman_->step` in the codebase; sibling TUs never reach it.
2. **ECS commit ordering.** Step 1 (entry) commits ECS-side writes
   into Jolt; step 4 (exit) commits Jolt-side writes back. Step 2
   sits between; mid-substep cross-traffic is forbidden. The
   barrier-flag instrumentation (`set_substep_in_flight_flag`)
   detects accidental cross-traffic via debug assertions in the
   sibling mirror seams.
3. **External-force drain is total.** Step 1's
   `drain_external_force_torque` is one of the call sites that the
   sibling `RigidBody` design enforces totality on (zero out after
   read; SPEC §4.1.4 invariant 3). The driver does not author the
   drain math; it merely calls into the sibling.
4. **Deterministic given (config, body set, input).** The §3.5
   determinism cell + the §6 concurrency rules + the per-step NaN /
   iteration-hash gates make the substep a pure function of its
   inputs. Two `step_one()` calls with byte-equal inputs produce
   byte-equal outputs and byte-equal `tick_count` advancement on
   every supported host (PHILOSOPHY §7, SPEC §6.4).
5. **Single rollback point.** Steps 3 + 5 are the determinism /
   numerical gates; on failure they roll back step 2's Jolt mutation
   via `rollback_jolt_step` (sibling middleman API; conceptually
   "restore the per-body kinematic state captured before step 2").
   The accumulator carry / tick are advanced **only** by `advance`'s
   loop, which checks the `step_one` return — so a rolled-back
   substep is invisible to the accumulator.

The pre-step rollback hook (`rollback_jolt_step`) is the §3.5 cell
that buys the §10.1 `NumericalInstabilityDetected` /
`DeterminismCheckFailed` recovery contract. Concretely: the
sibling `JoltMiddleman` design owns a per-substep "kinematic
shadow" (positions / rotations / velocities of every active body
captured at step 1 entry; one buffer reused across substeps,
re-allocated only when the body count grows). On step-3 / step-5
failure the shadow is restored over Jolt's body table; this is
cheap (`O(active_bodies)` memcpy) and the only rollback path the
cluster needs.

### 3.5 Determinism cell (anchors §6.4 in the driver body)

The §4.2 invariant 3 promise — byte-equal snapshots across hosts and
runs — is upheld by four mechanical rules SPEC §6.4 fixes. This
cluster owns four enforcement points; each is one TU + one
build-time assertion:

| Rule (SPEC §6.4)                                  | Enforcement point in this cluster                                                          |
|---------------------------------------------------|---------------------------------------------------------------------------------------------|
| **R1 — Deterministic Jolt config**                | `physics_world.cpp::create` step 5: `middleman_->create_world(view, ...)` calls `JPH::PhysicsSystem::SetDeterministicSimulation(true)` and pins `JobSystemSingleThreaded` (SPEC §6.4 rule 1). The view's `velocity_iters` / `position_iters` / `warm_start_factor` / sleep thresholds / layer matrix / RNG seed are forwarded verbatim; no Jolt knob defaults are used. |
| **R2 — Fixed iteration order**                    | `phase3_driver.cpp::step_one` steps 1 + 4: every archetype walk sorts a `BodyId` / `JointId` scratch span ascending before walking (sibling `RigidBody`/`Joint` design owns the sort body; this cluster threads the same comparator). The contact drain in step 4 sorts pairs by `(BodyId-low, BodyId-high)` (SPEC §4.1.8 invariant 1). |
| **R3 — No host float intrinsics**                 | `physics_config.cpp` + `accumulator.cpp` + `phase3_driver.cpp` include only `<cmath>` (no `<immintrin.h>` / `<arm_neon.h>`). The CMake pre-build check (SPEC §6.4 rule 3) re-asserts. The integration math performed outside Jolt — the accumulator carry, the external-force drain summation, the sleep-frame counter increment — is `<cmath>`-only. |
| **R4 — IEEE-754 bit-equal storage**               | `physics_config.cpp::compute_content_hash` + `accumulator.cpp::snapshot_carry` + the §3.4 snapshot/restore forwards: every `f32` / `f64` field crosses the snapshot or per-substep barriers via `std::bit_cast<u32>(x)` / `std::bit_cast<u64>(x)`. NaN payloads round-trip unchanged; the runtime never produces NaN (§3.4.4 step 3 enforces) and the codec preserves whatever pattern arrives. |

Together these rules + the §3.4.4 step-3 NaN gate + the §3.4.4
step-5 iteration-hash gate make the §11 byte-equal snapshot
acceptance test (`tests/physics/world/byte_equal_snapshot_at_tick_1000`)
pass on the M1-arm64 + macOS-x64 host pair. A failure at any of
the four rules surfaces as `DeterminismCheckFailed` at the
determinism-gate fixture before reaching the snapshot bus (SPEC
§8.6 round-trip test).

The **~0.10 ms determinism premium** the cell pays is folded into
the §9.2 sub-budget (SPEC §9.5 — ~0.05 ms for the
`BodyId`-ascending sorts, ~0.05 ms for `JobSystemSingleThreaded`).
A future spike that proposes parallel Jolt dispatch must amend
SPEC §6.4 rule 1, not §9.

### 3.6 ECS↔Jolt mirror — one-way per substep

The mirror seam is the boundary at which ECS archetype storage
crosses into Jolt's body / constraint tables (SPEC §6.3). This
cluster brokers the two barriers; sibling aggregates own the
per-row body. Two directions, two barriers, one substep boundary
— SPEC §4.2 invariant 2 made mechanical.

#### 3.6.1 Entry barrier (substep entry, before `middleman_->step`)

```text
// Sibling RigidBody/Joint designs own the per-row body.
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

Cluster's role: **call** the entry barrier from `step_one` step 1
(§3.4.4); set the `entry_barrier_flag` for the SPEC §10
`SubstepEcsCommitInverted` detection. The per-row marshalling
lives in the `bodies/` and `joints/` modules; this design treats
those as opaque seams.

#### 3.6.2 Exit barrier (substep exit, after `middleman_->step`)

```text
For body in RigidBody archetype, sorted ascending by BodyId:
  JoltMiddleman::get_linear_velocity(BodyId)  -> Velocity.value
  JoltMiddleman::get_angular_velocity(BodyId) -> AngularVelocity.value
  JoltMiddleman::get_position(BodyId)         -> RigidBody.position
  JoltMiddleman::get_rotation(BodyId)         -> RigidBody.rotation
  JoltMiddleman::is_sleeping(BodyId)          -> Sleeping marker (add/remove)
For pair in JoltMiddleman::drain_contact_pairs():
  emit CollisionStarted | CollisionPersisted | CollisionEnded
  (sorted by (BodyId-low, BodyId-high) — §4.2 invariant 8)
For pair in JoltMiddleman::drain_trigger_pairs():
  emit TriggerEnter | TriggerStay | TriggerExit
For joint in JointBreakThreshold archetype:
  if JoltMiddleman::accumulated_impulse_exceeds(JointId, threshold):
    emit JointBrokenEvent; despawn joint entity
```

Cluster's role: **call** the exit barrier from `step_one` step 4;
set the `exit_barrier_flag`. Same module split.

Three properties this seam guarantees (SPEC §6.3; restated):

1. **No mid-substep cross-traffic.** The two barriers are the only
   legal transit points. The driver flags (`entry_barrier_flag`,
   `in_flight_flag`, `exit_barrier_flag`) are read by debug
   assertions in the sibling mirror seams; a violation surfaces as
   `Error::SubstepEcsCommitInverted` (SPEC §10.1).
2. **Iteration order is `BodyId` / `JointId` ascending.** Not
   archetype-chunk order, not Jolt's internal pair-list order, not
   wall-clock contact-listener fire order. The handle is the
   determinism key (SPEC §4.1.5b invariant 1, §4.2 invariant 8).
3. **Drain is total.** `ExternalForce` / `ExternalTorque` reset to
   zero at substep entry; the contact / trigger pair lists drain
   into the per-frame event buffer at substep exit and Jolt's
   listener queue is emptied. Phase 3's final state at exit has
   zero queued physics work.

## 4. Public surface

The §5 facade (SPEC §5; locked) is the contract every caller
compiles against. This section maps the SPEC §5 declarations onto
the §3 internal model and pins three usage rules the SPEC §5
preamble does not state in one place. **No new public surface is
introduced here**; deviations would require a SPEC §5 amendment
spike, not an in-place edit.

### 4.1 SPEC §5 → §3 mapping (cluster slice)

| SPEC §5 declaration                     | §3 internal owner                            | Notes                                                                           |
|-----------------------------------------|----------------------------------------------|---------------------------------------------------------------------------------|
| `class PhysicsWorld`                    | §3.4 `PhysicsWorldImpl`                      | Opaque facade; layout owned in `physics.dylib`. Caller manipulates via methods only. |
| `PhysicsWorld::create`                  | §3.4.2                                       | The only public entry that allocates Jolt state. Returns `Result<eastl::unique_ptr<PhysicsWorld>>`. |
| `PhysicsWorld::advance`                 | §3.4.3                                       | Phase-3 entry point; §10 phase guard.                                           |
| `PhysicsWorld::add_body` / `remove_body` | sibling `bodies/` aggregate                  | Forwards through the facade; cluster brokers but does not author the body table. |
| `PhysicsWorld::add_joint` / `remove_joint`| sibling `joints/` aggregate                  | Forwards.                                                                       |
| `PhysicsWorld::intern_shape` / `release_shape` | sibling `shapes/` aggregate            | Forwards.                                                                       |
| `PhysicsWorld::intern_material`         | sibling `bodies/material_table.cpp`          | Forwards.                                                                       |
| `PhysicsWorld::queries()`               | sibling `queries/` aggregate                 | Returns reference to resolved `PhysicsQueries`.                                 |
| `PhysicsWorld::accumulator()`           | §3.3 `AccumulatorImpl`                       | Returns reference; callers may read `carry()` / `tick_count()` outside phase 3. |
| `PhysicsWorld::config()`                | §3.2 `PhysicsConfigView`                     | Returns `const&` to the immutable view.                                         |
| `PhysicsWorld::world_tick()`            | §3.3 `accumulator_.tick_count()`             | Convenience accessor; identical to `accumulator().tick_count()`.                |
| `PhysicsWorld::snapshot` / `restore`    | sibling `snapshot/` aggregate                | Forwards. `snapshot()` walks every sibling table; `restore()` is the reverse.    |
| `PhysicsWorld::on_plugin_reload`        | §8 of this design                            | Hot-reload entry; called by `core` loader during phase 8.                       |
| `class PhysicsConfig` (struct)          | §3.2 `PhysicsConfigView` (internal)          | Public is the struct; internal is the validated view.                            |
| `class Accumulator`                     | §3.3 `AccumulatorImpl`                       | Public is the opaque class with `advance` / `carry` / `tick_count`; internal is the impl. |
| `class LayerFilter`                     | sibling `bodies/broadphase_layer.cpp`        | Public is the opaque builder; internal lives in the broadphase-layer aggregate (sibling spike). |

### 4.2 Three cluster-specific usage rules

1. **`advance` is called exactly once per phase 3 entry per
   `PhysicsWorld`.** The `core` `FrameLoop` driver invokes it via
   the registered phase-3 system (the cluster registers a single
   system into phase 3 via the manifest at `glibre_plugin_register`
   time; see §6 below). A second `advance` in the same phase 3 is
   a programming error and surfaces as
   `Error::StepCalledOutsidePhase3` (the second call's phase guard
   reads "exit-of-phase-3" because the first call already set the
   driver's per-frame done flag).
2. **`accumulator()` and `config()` are read-only outside phase 3.**
   Phase 6 (`render` cull-extract) reads `Accumulator::carry()` for
   interpolation alpha (`reviews/decisions/frame-phases.md` notes;
   SPEC §6.5 seam #1); phase 5 (`core` transform propagation) does
   not read either; phase 1 (`platform` input) does not read either.
   The const accessor is the only entry point during those phases;
   mutating either through the facade is a compile error (the
   `Accumulator` class has no public mutator).
3. **`snapshot()` is callable in phases 5+ (post-step) only.** The
   snapshot codec walks every sibling table (body / joint / shape);
   walking during phase 3 would observe partially-updated state and
   the §6.4 rule 2 sort is not in flight. The cluster's phase guard
   (the same flag `step_one` consults) refuses snapshot calls during
   phase 3 with `Error::StepCalledOutsidePhase3` — SPEC §10.1 names
   this arm for `step`, but the same guard catches `snapshot` /
   `restore` from inside the same phase. (Snapshot during phase 8 —
   the hot-reload drain — is permitted; the loader holds exclusive
   ownership and the per-substep mirror is settled.)

## 5. Hot/cold path split

The cluster is touched once per frame on the game-loop driver thread
(SPEC §6.6). The critical hot path is `PhysicsWorld::advance` →
`step_one` → `middleman_->step` (§3.4.3, §3.4.4); the cold paths are
construction (`create`), reload (`on_plugin_reload`), and the
config-hash compute (`compute_content_hash`).

### 5.1 Hot fields (touched per substep, in inner loops)

Every field below appears on the path from `advance` through the
`step_one` body. They live within the first cache line of their
owning struct (`alignas(64)`) and are read-only during the substep
where possible.

| Owner                     | Field                                           | Why hot                                                                       |
|---------------------------|-------------------------------------------------|-------------------------------------------------------------------------------|
| `AccumulatorImpl`         | `carry_` (`f32`)                                | Read + decremented per substep.                                                |
| `AccumulatorImpl`         | `fixed_dt_` (`f32`)                             | Loop bound for the substep loop.                                              |
| `AccumulatorImpl`         | `tick_count_` (`u64`)                           | Incremented per substep.                                                      |
| `PhysicsConfigView`       | `velocity_iters`, `position_iters` (`u8`)       | Forwarded to `middleman_->step` per substep.                                  |
| `PhysicsConfigView`       | `max_substeps` (`u8`)                           | Loop bound.                                                                   |
| `PhysicsConfigView`       | `determinism_gate` (`enum u8`)                  | Branch selector for the §3.4.4 step-5 hash gate.                              |
| `PhysicsWorldImpl`        | `jolt_system_` (pointer)                        | First arg to `middleman_->step`.                                              |
| `PhysicsWorldImpl`        | `entry_barrier_flag`, `in_flight_flag`, `exit_barrier_flag` (`u8` triple) | Set / cleared per substep barrier; read by debug assertions in sibling mirror seams. |

The `entry_barrier_flag` triple is packed into one `u32` aligned
field (`u8 × 3` + `u8 padding`) so reading the flag triple is one
load. The triple lives in the first cache line of `PhysicsWorldImpl`,
ahead of the cold sibling-aggregate pointers.

### 5.2 Cold fields (touched at construction, reload, snapshot)

| Owner                     | Field                                                          | Why cold                                                       |
|---------------------------|----------------------------------------------------------------|----------------------------------------------------------------|
| `PhysicsConfigView`       | `gravity` (`Vec3`)                                             | Read once into Jolt at `create`; never per substep.            |
| `PhysicsConfigView`       | `sleep_*`, `warm_start_factor`, `ccd_enabled`, `rng_seed`       | Read once into Jolt at `create`; never per substep.            |
| `PhysicsConfigView`       | `budgets` (four `u32`)                                          | Read once into sibling-table sizing at `create`.               |
| `PhysicsConfigView`       | `layer_filter` (shared_ptr to opaque)                          | Read once into Jolt's broadphase at `create`.                  |
| `PhysicsConfigView`       | `content_hash` (`u64`)                                         | Computed once at `create`; read at snapshot / restore.         |
| `PhysicsWorldImpl`        | `ecs_world_`, `middleman_` (borrows)                            | Read at `create` and at every barrier; the borrow lookup is one load and is treated as cold relative to the sibling table walks. |
| `PhysicsWorldImpl`        | `shape_table_`, `material_table_`, `body_id_allocator_`, `joint_registry_`, `contact_drain_`, `queries_` (unique_ptr) | Read at sibling-aggregate accessor calls; not per-substep. |

### 5.3 Layout enforcement

`PhysicsWorldImpl`'s hot prefix fits in a single 64-byte cache line:

```cpp
static_assert(offsetof(PhysicsWorldImpl, accumulator_) +
              sizeof(AccumulatorImpl) <= 64,
              "PhysicsWorldImpl hot prefix must fit in one cache line");
```

`alignas(64)` is required on `PhysicsWorldImpl` and `AccumulatorImpl`;
the build asserts both. The cold pointers are placed after the
accumulator block; their lookup is one indirection per substep
(once at entry barrier, once at exit barrier) and is masked by the
per-row work the sibling aggregates do.

The §11 perf test (`tests/physics/world/bench_advance_two_substep`)
samples the L1-D miss rate on the hot path under the S1 fixture
(SPEC §9.6.3) and asserts < 5%; drift is the §9 alarm.

## 6. Concurrency

MVP runs every system on the **game-loop driver thread** (SPEC §6.6;
`reviews/decisions/perf-budget.md` Pipelined Frame Timing). The
cluster's concurrency surface is exhaustively small.

### 6.1 Phase-by-phase admissibility

| Phase | Cluster ops admitted in MVP                                                                                                       |
|-------|------------------------------------------------------------------------------------------------------------------------------------|
| 1 Input        | `accumulator().carry()` read-only (rare; render's interpolation alpha is read here when render registers a phase-1 prefetch). `queries().raycast(...)` etc. read-only against the previous frame's terminal state (§4.1.10 invariant 1). |
| 2 Logic        | Reserved slot; deferred body in MVP. Future gameplay-plugin systems will write `ExternalForce` / `ExternalTorque` / kinematic transform overrides into ECS storages here, ahead of phase 3's entry barrier. |
| 3 PhysicsFixed | **`PhysicsWorld::advance`** is the body. Sole writer of physics state for the frame. The driver runs the §3.4.4 substep loop end to end; no other system in phase 3 touches `PhysicsWorld` (the cluster registers exactly one system into phase 3 via the manifest, and the manifest's `reads` / `writes` access set is the union of the §3.6 mirror barriers). |
| 4 Animation    | Reserved slot; deferred body in MVP.                                                                                                |
| 5 Transform    | `accumulator().carry()` read-only (interpolation alpha for `core`'s transform propagation; rarely consumed). No `PhysicsWorld` writes.|
| 6 CullExtract  | `accumulator().carry()` read-only (render's interpolation alpha). `queries()` read-only against this frame's post-step state.       |
| 7 RenderSubmit | No `PhysicsWorld` reads.                                                                                                          |
| 8 HotReload    | Cluster is quiescent; only `PhysicsWorld::on_plugin_reload` (§8) is called by the loader, and only when this plugin is the swap target. |
| 9 Present      | No `PhysicsWorld` reads.                                                                                                          |

### 6.2 Read-only operations

May run in any phase 1, 5, 6 (not 3 — the phase guard refuses).
Read-only against `PhysicsWorld` storage; no exclusive lock required.

- `PhysicsWorld::config()`
- `PhysicsWorld::world_tick()`
- `PhysicsWorld::accumulator()` (returns `Accumulator&`; callers may
  read `carry()` / `tick_count()`).
- `PhysicsWorld::queries()` then any of the four query methods on the
  returned aggregate (§4.1.10).

### 6.3 Read-write operations

Run only in phase 3 (the cluster's owned phase) or phase 8 (the
loader's owned barrier).

- `PhysicsWorld::advance` — the only phase-3-owning write path.
  Internally calls every sibling aggregate's mirror-barrier hook;
  no sibling aggregate's mutate-from-outside-phase-3 path exists
  (the manifest's access set forbids it; SPEC §6.6).
- `PhysicsWorld::add_body` / `remove_body` / `add_joint` /
  `remove_joint` / `intern_shape` / `release_shape` /
  `intern_material` — write paths against the sibling tables.
  **Permitted in phase 1 (input), phase 2 (logic), phase 3 (the
  cluster's own substep entry), and phase 8 (hot-reload restore)**;
  forbidden in phases 4–7, 9 because those phases' read paths would
  observe partial state.
  The cluster brokers via the facade; sibling tables enforce the
  phase-and-thread admissibility internally (the sibling `bodies/`,
  `joints/`, `shapes/` designs own the per-call guard).
- `PhysicsWorld::snapshot` / `restore` — read-only phase-3 (snapshot
  during phase 3 is refused; see §4.2 rule 3) and write-only at
  phase-8 restore (the loader's hot-reload pathway). The exception
  is `snapshot()` during phase 8 drain (cluster captures at
  `glibre_plugin_drain`; SPEC §8.3.1) — permitted because the
  loader holds the exclusive lock.

### 6.4 Memory ordering

Every public method on `PhysicsWorld` and `Accumulator` is `noexcept`
and assumes single-threaded access (the game-loop driver thread).
`PhysicsConfig` is `Vec3`-aligned but otherwise plain; the
`content_hash` BLAKE3 compute runs at construction-only and is
single-threaded.

Two relaxed-atomic exceptions for post-MVP forward compatibility:

1. **`accumulator_.tick_count_` reads** — phase 6 (`render`) reads
   `tick_count()` for trace metadata; phase 3 increments. MVP
   single-thread sim collapses the read to a plain `u64` load;
   post-MVP per-system parallelism (when render moves off the
   driver thread) will upgrade the field to `std::atomic<u64>` with
   relaxed ordering. The cluster reserves the upgrade path now (the
   `AccumulatorImpl::tick_count()` accessor signature is unchanged
   on upgrade).
2. **`PhysicsWorldImpl::entry_barrier_flag` triple** — debug-build
   assertions in sibling mirror seams read the flag triple; the
   driver writes it. MVP collapses to plain reads/writes; if a
   future post-MVP parallel mirror seam reads from a worker thread,
   the triple becomes `std::atomic<u32>` with relaxed ordering. No
   call-site change is required.

### 6.5 Jolt threadpool seam

SPEC §6.4 rule 1 + §6.6 fix the Jolt `JobSystem` to either
`JobSystemSingleThreaded` or a deterministically-seeded
`JobSystemThreadPool` whose dispatch order is keyed off
`PhysicsConfig.content_hash`. MVP picks `JobSystemSingleThreaded`
unconditionally; the threadpool is constructed at `create` step 5
inside the middleman, owned by the `JoltSystemHandle`, and torn
down by `glibre_plugin_drain` (SPEC §8.2 row "Per-plugin worker
thread pool"). The cluster never reaches into the pool directly —
it threads the handle through `middleman_->step` and trusts the
middleman's contract.

The MVP "single-threaded" choice is the §3.5 R1 cell (~0.05 ms of
the §9.5 determinism premium); a future spike that proposes a
deterministically-seeded multi-thread pool must amend SPEC §6.4
rule 1, not §9.

### 6.6 Determinism guarantees

The cluster contributes three guarantees to the §4.2 invariant 3
"byte-equal across hosts" promise:

1. **`PhysicsConfig.content_hash` is byte-equal across hosts.**
   §3.5 R3 (`<cmath>`-only) + §3.5 R4 (`std::bit_cast` for `f32` /
   `f64`) make BLAKE3 over the canonicalised view bytes identical
   on every host running the same `PhysicsConfig`.
2. **`Accumulator` carry / tick are byte-equal across hosts** when
   the input `real_dt` sequence is byte-equal. The arithmetic is
   `f32` IEEE-754 (`<cmath>`), no fast-math, no intrinsics; carry
   serialises via `std::bit_cast<u32>`.
3. **`step_one` substep is byte-equal across hosts** — combined
   contribution of §3.5 R1 (deterministic Jolt config) + R2 (fixed
   iteration order) + the §3.4.4 step-3 NaN gate. The cluster does
   not author the per-row math; it threads the determinism cell
   through the substep dispatch.

## 7. Persistence + ABI

The cluster's persistence surface is split between **physics-owned**
schemas (`PhysicsConfigRecord`, `PhysicsSnapshot`'s accumulator
columns) and **sibling-owned** schemas (`ShapeBlobRecord`,
`JointDescriptorRecord`, `PhysicsSnapshot`'s body / joint columns).
Authority is SPEC §7; this section documents only the cluster's
slice.

### 7.1 Schemas this cluster owns

#### 7.1.1 `PhysicsConfigRecord` (SPEC §7.1.1)

The frozen deterministic configuration record. Authored under
`data/schemas/physics/PhysicsConfigRecord.fory`; FQN
`glibre.physics.PhysicsConfigRecord`. Field roster + invariants are
SPEC §7.1.1; no field is added or removed by this design.

The cluster's role: **read** the record at `PhysicsWorld::create`
(via the value-typed `PhysicsConfig` parameter, populated by the
`data` codec), **compute** the `content_hash` (BLAKE3 over the
canonicalised serialised bytes; SPEC §7.1.1 invariant 3), and
**cache** the hash on the `PhysicsConfigView`. The record bytes
themselves are owned by `data`; the cluster never writes them.

A schema-version bump (SPEC §7.2.1 `PhysicsConfigRecord` migration
path) is handled by the standard additive-defaulted-field pure
migration. The cluster ships no migration body for v1 — there is no
v1→v2 chain in MVP. When v2 lands the migration body lives at
`physics/src/migrations/migrate_PhysicsConfigRecord_v1_to_v2.cpp`
and is the standard pure migrate signature
(`reviews/decisions/hot-reload-protocol.md` §"Migrate Function
Contract").

#### 7.1.2 `PhysicsSnapshot` accumulator + header columns (SPEC §7.1.4)

The snapshot codec is the sibling `snapshot/` aggregate's
responsibility (SPEC §4.1.12, §7.1.4). The cluster contributes
three columns to the schema:

| `PhysicsSnapshot` field           | Source in this cluster                                       |
|-----------------------------------|--------------------------------------------------------------|
| `SnapshotHeader.world_tick`       | `accumulator_.tick_count()`                                  |
| `SnapshotHeader.accumulator_carry`| `accumulator_.snapshot_carry()` (`std::bit_cast<u32>(carry_)`)|
| `SnapshotHeader.physics_config_hash` | `config_.content_hash`                                     |

These three are read at `PhysicsWorld::snapshot` (the cluster passes
them to the sibling codec); they are written at
`PhysicsWorld::restore` via `accumulator_.reseed(carry, tick)` +
`config_.content_hash` comparison (`SnapshotSchemaMismatch` if not
equal; SPEC §7.1.4 invariant 3).

The other columns (`body_*`, `joint_*`, `body_shape_blob_hashes`)
are owned by the sibling aggregates and the codec — this design
treats them as opaque carrier bytes.

### 7.2 ABI hash sources this cluster participates in

The cluster is a host-side consumer of `glibre_types_abi_hash` from
`glibre-types.dylib` (per `fory-codegen.md` and `plugin-abi.md`);
the hash is computed over the **schema sources** plugins declare,
not over `PhysicsWorld` itself. The cluster does not contribute
bytes to the ABI hash directly. ABI hash sources the cluster
**cares about**, listed for the implementer's plan:

1. **`PhysicsConfigRecord` schema source**
   (`data/schemas/physics/PhysicsConfigRecord.fory`) — owned by the
   `data` context; one entry in the `glibre_types_abi_hash` digest
   input.
2. **`PhysicsSnapshot` schema source**
   (`data/schemas/physics/PhysicsSnapshot.fory`) — same.
3. **`PluginManifest.components` family** (SPEC §7.1; `plugin-abi.md`
   §"Plugin Manifest Schema") — owned by `core` / sibling spike
   #702; the cluster reads `ComponentDecl.fqn` /
   `ComponentDecl.schema_hash` at `glibre_plugin_register` time to
   populate sibling tables (the bodies / joints / shapes / queries /
   contact-drain modules).
4. **`JoltMiddleman` ABI hash** — owned by the `JoltMiddleman`
   sibling design (SPEC §4.1.13). The middleman exports
   `glibre_jolt_abi_hash() -> const char*` — distinct from
   `glibre_types_abi_hash` because the middleman ships
   Jolt-version-specific types (`BodyId`, `ConstraintRef`, opaque
   `Shape*` token, `ContactManifold` plain-data layout). The
   cluster's `create` step 2 calls `JoltMiddleman::require_hash` to
   gate against drift.

The cluster exports no symbols of its own that contribute to either
hash; its ABI shape is the SPEC §5 facade, not a Fory-versioned
schema. Changes to the §5 facade bump `physics.dylib`'s SemVer (per
`plugin-abi.md` §"Versioning Rules" axis 3, `PluginManifest.version`),
not the ABI hash.

### 7.3 What survives a session boundary (read: nothing in process)

The cluster is **runtime-only** at the process boundary. Process
restart reconstructs every byte from `data` 's schemas: the
`PhysicsConfigRecord` is loaded from disk into the `PhysicsConfig`
parameter, every `ShapeBlobRecord` referenced by surviving
`Collider`s is re-interned, and the `PhysicsSnapshot` (if any) is
loaded and restored.

There is no "physics-world snapshot" persisted to disk by the
cluster itself; world snapshots are the `data` / `content`
context's concern. The replay-determinism requirement (PHILOSOPHY
§7) is met by re-running the deterministic system schedule from a
known input trace, not by loading a `PhysicsWorld` blob.

The session-boundary case `PhysicsWorld` **does** participate in is
**hot-reload across the swap** — but that is a phase-8 in-process
event, not a process-boundary event. See §8.

### 7.4 Jolt as middleman seam — no Jolt types cross

SPEC §4.1.13 invariant 1 forbids Jolt-derived types from crossing
the plugin ABI except via `JoltMiddleman`. The cluster honours this
mechanically:

1. **`physics_world.cpp` includes zero Jolt headers.** The
   `<Jolt/...>` headers are reachable from exactly one TU
   (`middleman/jolt_middleman.cpp`; SPEC §6.1 module rule 1). The
   pre-build CMake check (a `clang -E` scan that fails the build on
   any other TU referencing `Jolt/`) is the enforcement.
2. **`JoltSystemHandle` is opaque to this cluster.** The handle's
   destructor lives in the middleman; the cluster's
   `PhysicsWorldImpl` destructor calls `middleman_->destroy_world(
   *jolt_system_)` and the middleman performs the Jolt-side teardown.
3. **`Accumulator` and `PhysicsConfig` carry no Jolt types.** Both
   are pure C++23 structures; no Jolt typedef leaks into either's
   layout. The §6.5 thread pool handle is owned inside
   `JoltSystemHandle`, behind the seam.
4. **The §5 facade `#error`-guards Jolt header inclusion.** The
   include guard at the top of `physics/include/glibre/physics/
   physics.hpp` (SPEC §5 preamble) forbids any caller TU that has
   already included a Jolt header from including the facade. This
   is the symmetric refusal for the consumer side.

A schema bump that adds a Jolt-derived type to the cluster's ABI
**is not possible** in MVP — the §5 facade is locked, and any new
public type would route through the `JoltMiddleman` seam.
Post-MVP additions (a new joint kind, a new shape variant) bump
the `JoltMiddleman` ABI hash and are gated by it; the cluster's
own ABI is unchanged.

## 8. Hot-reload

Hot-reload semantics for the physics plugin are owned by SPEC §8;
this section states what **the physics-world cluster** must hold
steady across the swap, what `migrate(...)` requires of the
cluster, and the refusal cases this design contributes. Engine-wide
concerns (per-plugin atomicity, observer bus event shapes, error
wrapping rules, the `enqueue_hot_reload` E2E hook) are not
re-stated here — see SPEC §8 + `reviews/decisions/hot-reload-
protocol.md`.

### 8.1 What survives the swap

Per SPEC §8.1 (hot-reload point — phase 8, never mid-frame) +
SPEC §8.2 (survival inventory), specialised to this cluster's
primitives:

| Primitive (§3 ref)                  | Survives swap? | Mechanism                                                                                                                                                 |
|-------------------------------------|----------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------|
| `PhysicsConfigView` bytes (§3.2)    | **Yes (re-derived)**  | The bytes are rebuilt by the new plugin from the surviving `PhysicsConfigRecord`. The `content_hash` is recomputed by the new plugin's `compute_content_hash` and **must equal** the snapshot's `physics_config_hash` (SPEC §7.1.4 invariant 3); mismatch → §10 `SnapshotSchemaMismatch`. |
| `AccumulatorImpl.carry_`            | **Yes (snapshot)**    | Restored from `PhysicsSnapshot.accumulator_carry` (tag 4) via `accumulator_.reseed(carry, tick)` (SPEC §7.1.4 schema). Bit-equal because `std::bit_cast<u32>` is the codec rule (SPEC §6.4 R4). |
| `AccumulatorImpl.tick_count_`       | **Yes (snapshot)**    | Restored from `PhysicsSnapshot.world_tick` (tag 3) via `reseed`. Bit-equal as a `u64`.                                                                    |
| `AccumulatorImpl.fixed_dt_`         | **Yes (re-derived)**  | Mirror of `PhysicsConfigView.fixed_dt`; rebuilt at `create` from the surviving `PhysicsConfigRecord`. Bit-equal with the pre-swap value because the record is unchanged. |
| Jolt `PhysicsSystem` handle         | **No**                | Private to the outgoing plugin's image; rebuilt by the new plugin's `create` from the same `PhysicsConfig` budgets + content hash.                        |
| Jolt body / constraint / contact pools | **No**             | Same — Jolt-internal state. Rebuilt against the surviving `RigidBody` / `Joint` ECS storages via `add_body` / `add_joint` replay (sibling design).         |
| Per-substep "kinematic shadow" buffer (§3.4.4) | **No**     | Substep-local; reborn each substep. No survival contract.                                                                                                  |
| Phase-3 driver thread context flags (`entry_barrier_flag`, etc.) | **No** | Phase 8 entry guarantees no substep is in flight (SPEC §8.1 — phase 3 already returned for frame N); flags are re-initialised by the new plugin. |
| Determinism gate (`config_.determinism_gate`) | **Yes (re-derived)** | Mirror of `PhysicsConfigRecord.determinism_gate` (SPEC §7.1.1 since-bumped field, §10.7 OQ-2). Rebuilt by the new plugin. |

The mechanical rule: **`PhysicsConfigView` re-derives from the
surviving record; `AccumulatorImpl` carry / tick re-hydrate from
the carrier `PhysicsSnapshot`; everything else (Jolt-internal
state) rebuilds from scratch.** This matches SPEC §8.2 row
"`Accumulator`" + row "`PhysicsConfig` runtime view" exactly.

### 8.2 What `migrate(...)` must do

The cluster authors **no per-row migration body** for its own
schemas in MVP — there is no v1→v2 chain pending for
`PhysicsConfigRecord` or for the accumulator columns of
`PhysicsSnapshot`. Sibling aggregates own the body / joint /
shape migrations (SPEC §7.2.4).

What this design contributes is the **drain + resume bodies**
for the cluster's primitives, called by the loader at SPEC §8.3.1
(drain) and SPEC §8.3.2 (resume).

#### 8.2.1 Drain body — `glibre_plugin_drain` cluster contribution

Called by the loader at the start of phase 8 (SPEC §8.3.1). The
cluster's slice runs as part of the plugin-wide drain:

```text
PhysicsWorldImpl::on_drain():
  // Step 1 — Pre-snapshot phase guard.
  // Phase 3 already returned for frame N (SPEC §8.1 #1); accumulator
  // carry is at most fixed_dt - epsilon; tick_count is the post-step
  // value of frame N's last substep.
  assert phase != Phase::PhysicsFixed
  assert no entry/exit barrier flags set

  // Step 2 — Hand off accumulator state to the snapshot codec.
  // The sibling snapshot/ aggregate's PhysicsWorld::snapshot() walks
  // every sibling table; this cluster's contribution is three header
  // columns: world_tick, accumulator_carry, physics_config_hash.
  // (Body / joint / shape columns come from sibling tables.)
  snapshot <- self.snapshot()                              // §3.4 forward
  if snapshot is unexpected:
    return std::unexpected{ snapshot.error() }
  // Loader's per-phase migration arena keeps the bytes alive through
  // swap + migrate + resume (SPEC §8.3.1).
  loader_arena.put(snapshot->to_bytes())

  // Step 3 — No per-cluster teardown.
  // The Jolt pool teardown happens at PhysicsWorldImpl::~PhysicsWorldImpl()
  // when the outgoing plugin's image unloads (the unique_ptr destructors
  // run in declaration-order-reverse: queries first, then contact_drain,
  // joint_registry, body_id_allocator, material_table, shape_table,
  // jolt_system, accumulator, config).
  return {}
```

The cluster's drain has no Jolt-side work — every Jolt allocation
is owned by the sibling tables (which run their own drain bodies)
and by the `JoltSystemHandle` (whose destructor at the unique_ptr
teardown point releases Jolt's `PhysicsSystem`). The cluster only
captures the snapshot.

#### 8.2.2 Resume body — `glibre_plugin_register` cluster contribution

Called by the loader at SPEC §8.3.2 step 4. The cluster's slice runs
after sibling tables are reconstructed (shape table re-interned,
body allocator rebuilt, joint registry empty):

```text
PhysicsWorldImpl::on_resume(loader_arena):
  // Step 1 — JoltMiddleman ABI gate.
  // SPEC §8.3.2 step 1 + §4.1.13 invariant 2.
  if middleman_.require_hash(host_abi_hash) is unexpected:
    return std::unexpected{ Error::JoltMiddlemanHashMismatch }

  // Step 2 — Reconstruct PhysicsWorld from the surviving record.
  // (Record bytes survive in glibre-types middleman storage.)
  surviving_record <- loader_arena.get<PhysicsConfigRecord>()
  config           <- PhysicsConfig::from_record(surviving_record)
  config_view      <- PhysicsConfigView::from(config)
  config_view.content_hash <- compute_content_hash(config_view)

  // Step 3 — Rebuild sibling-table-owned Jolt state.
  // (PhysicsWorldImpl::create's step 4-5-6 path; entirely under sibling control.)
  jolt_system, shape_table, ... <- create_jolt_state(config_view)

  // Step 4 — Restore accumulator from snapshot header.
  snapshot_bytes <- loader_arena.get<PhysicsSnapshot::bytes>()
  snapshot       <- PhysicsSnapshot::from_bytes(snapshot_bytes)
  if snapshot is unexpected:
    return std::unexpected{ Error::SnapshotDeserialiseFailed }

  // Snapshot consistency gate — the §10 SnapshotSchemaMismatch arm.
  if snapshot->header().physics_config_hash != config_view.content_hash:
    return std::unexpected{ Error::SnapshotSchemaMismatch }
  if snapshot->header().middleman_abi_hash != middleman_.info().abi_hash:
    return std::unexpected{ Error::JoltMiddlemanHashMismatch }

  // Step 5 — Reseed accumulator + delegate body/joint restore to siblings.
  accumulator_.reseed(snapshot->header().accumulator_carry,
                      snapshot->header().world_tick)
  // Sibling snapshot/snapshot_restore.cpp walks body / joint columns
  // (SPEC §8.3.2 step 4); this cluster does not author those walks.
  delegate_to_snapshot_restore(*snapshot)

  // Step 6 — Re-link triggers, contact listener, queries (SPEC §8.3.2 step 5).
  // Sibling-aggregate territory; the cluster only constructs the
  // PhysicsQueries facade (§3.4.1 step 6) which the siblings populate.

  return {}
```

The cluster's resume body is bounded by the four steps above; the
total work is **O(1) cluster-side** (config rebuild, accumulator
reseed, two hash checks) plus **O(snapshot bytes) sibling-side**
(decode, body / joint walk). SPEC §8.3.2 §"total work" budget is
preserved.

### 8.3 Refusal cases this cluster contributes

`PhysicsWorld` raises these arms during drain / resume; per SPEC
§8.4 they roll up under `core::Error::HotReloadRefused` with one of
the protocol's existing inner causes (`PluginAbiHashMismatch`,
`PluginInitFailed`, `SchemaMigrationFailed`).

| Detection point                                                                                | `physics::Error` arm                | `core::Error` wrapper                         | SPEC §10.1 row |
|-----------------------------------------------------------------------------------------------|-------------------------------------|------------------------------------------------|----------------|
| `JoltMiddleman::require_hash` differs at resume step 1 (or pre-flight at create)              | `JoltMiddlemanHashMismatch`         | `HotReloadRefused { PluginAbiHashMismatch }`  | SPEC §10.1     |
| `SnapshotHeader.physics_config_hash` differs from recomputed `PhysicsConfigView.content_hash` | `SnapshotSchemaMismatch`            | `HotReloadRefused { PluginInitFailed }`        | SPEC §10.1     |
| `PhysicsSnapshot::from_bytes` fails (truncated, version-tag-corrupt, schema-incompatible)     | `SnapshotDeserialiseFailed`         | `HotReloadRefused { SchemaMigrationFailed }`   | SPEC §10.1     |
| `PhysicsConfig::from_record` fails (record schema bumped past reader-side current; no chain)  | `HotReloadStateUnmigratable`        | `HotReloadRefused { SchemaMigrationFailed }`   | SPEC §10.1     |

`PhysicsWorld` does **not** raise:

- `JoltMiddlemanUnavailable` — the loader checked the middleman's
  presence at engine init; this case fires at process startup, not
  at hot-reload. Listed for completeness; SPEC §10.1 documents the
  row.
- `ShapeBlobMissing` — the body / shape sibling design owns this
  row at SPEC §8.4 row 4; the cluster forwards but does not detect.
- Sibling-aggregate refusals (`JointEndpointInvalid` etc. at
  restore time) — those originate inside sibling resume bodies and
  this cluster forwards them through.

The `physics::Error::HotReloadStateUnmigratable` arm is the
cluster-side surface of SPEC §8.4 row 2 (snapshot's
`schema_version` greater than the new plugin's reader-side current
version, no migration chain); the detection actually happens in
`PhysicsConfig::from_record` (the record's `since` field exceeds
the build's reader-side support). The cluster-side arm is the
typed surface; the protocol-level inner cause is
`SchemaMigrationFailed`.

### 8.4 Self-reload refusal

`physics.dylib` is hot-reloadable (this is the §8 acceptance test).
`core` is not (SPEC §3.3, hot-reload-protocol §"Open Questions" #2).
`physics_world.cpp` therefore never participates as the *outgoing*
plugin in a `core` swap; if the engine is restarted, the cluster is
constructed fresh from `data`'s `PhysicsConfigRecord`. This is the
process-boundary path; nothing this design contributes participates
in self-reload.

A `request_reload(plugin_fqn = "glibre.physics")` against the
`physics-world` cluster goes through the standard SPEC §8.3
drain → swap → migrate → resume; the `JoltMiddleman` ABI gate
(§8.3 row 1) catches version drift; the previous-good plugin keeps
ticking on refusal (SPEC §8.4 row 1).

## 9. Performance

Authority: `reviews/decisions/perf-budget.md` Per-Context Budget
Table assigns physics the cell **2.00 ms CPU sim + 0.00 ms CPU
submit + n/a GPU + 128 MiB heap**. The SPEC §9.2 per-stage split
gives the **Jolt step + ECS↔Jolt mirror + queries** trio its
sub-budgets; this design refines the **Accumulator + PhysicsWorld
facade overhead** within the existing rows so the §11
`BENCHMARK_CELL` `physics/world: phase3_total_two_substep` (SPEC
§9.6.1) has a well-defined target.

This design does **not widen** any cell or sub-budget; deviations
require a `perf-budget.md` amendment spike, not an in-place edit.

### 9.1 Cited cells (verbatim from `perf-budget.md` and SPEC §9)

| Axis              | Budget          | Source                                                    |
|-------------------|-----------------|-----------------------------------------------------------|
| CPU sim           | **2.00 ms**     | `perf-budget.md` Per-Context Budget Table — physics row   |
| CPU submit        | 0.00 ms         | physics records no GPU work                                |
| GPU               | n/a             | physics owns no Metal heaps or encoders (SPEC §3.3)        |
| Heap ceiling      | **128 MiB**     | `perf-budget.md` Per-Context Budget Table — physics row   |
| Phase ownership   | **3**           | `frame-phases.md` — physics-fixed; sole owner             |

### 9.2 Cluster sub-budget within the §9.2 stage rows

The §9.2 stage rows decompose physics's 2.00 ms cell across three
stages — Jolt step (1.50 ms), ECS↔Jolt mirror (0.30 ms), queries
(0.20 ms). The cluster's own contribution (driver overhead +
accumulator drive + facade dispatch) is bounded as follows; the
numbers fit **inside** the existing rows, not as a fourth row.

| Cluster slice                                              | Inside row             | CPU ms (sim) | Dominant operation                                                                                       |
|------------------------------------------------------------|------------------------|--------------|-----------------------------------------------------------------------------------------------------------|
| `Accumulator::advance` loop arithmetic                     | ECS↔Jolt mirror (0.30) | < 0.005      | Two `f32` add/subtract + a `u64` increment per substep × at most 4 substeps = O(40 ns).                  |
| `PhysicsWorldImpl::step_one` dispatch overhead             | ECS↔Jolt mirror (0.30) | < 0.010      | Three barrier-flag updates per substep + one virtual-function-equivalent call into the middleman; pre-amortised by §5 sub-budget. |
| `PhysicsWorldImpl::advance` phase-guard read               | (any)                  | < 0.001      | One thread-local read.                                                                                   |
| `step_one` step-3 NaN check                                | Jolt step (1.50)       | < 0.020      | One vectorisable scan over body kinematic outputs (~30 active bodies × 4 `f32` = ~120 IEEE-754 isnan calls; SIMD-friendly under `<cmath>`). |
| `step_one` step-5 iteration-hash compute (Hard mode only)  | Jolt step (1.50)       | < 0.030      | One BLAKE3-128 over the post-step body kinematic columns (~30 × 24 B = 720 B); only runs under `Hard`. Soft default skips. |
| `compute_content_hash` (`create` only)                     | (cold path)            | < 0.100      | Once per world-create; off the per-frame hot path; not counted against the cell.                          |
| `Accumulator::reseed` (`restore` only)                     | (cold path)            | < 0.001      | Once per snapshot restore.                                                                               |
| **Sub-total inside §9.2 rows**                             |                        | **< 0.066**  | Steady-state two-substep frame, default `SoftWarn`. Comfortably fits inside the existing 1.80 ms (Jolt + mirror) headroom. |

The slice-by-slice numbers above are upper bounds estimated against
the M1 firestorm baseline (`perf-budget.md` Justification Per Cell
"M1 firestorm at 3.2 GHz"). The actual per-slice cost is well below
the row ceilings — the cluster is dominated by sibling-aggregate
work (the Jolt step itself and the mirror barrier walks), not by
its own overhead. The §11 `BENCHMARK_CELL` enforces the row
totals; the slice numbers are advisory.

### 9.3 Heap composition inside the 128 MiB ceiling

The cluster's resident allocations under `ContextTag::physics`
contribute to the §9.3 pool decomposition (SPEC §9.3). Cluster's
slice:

| Pool (SPEC §9.3 row)                | Cluster's contribution                                                     |
|-------------------------------------|----------------------------------------------------------------------------|
| Jolt body + constraint pools (64 MiB) | `JoltSystemHandle` and the Jolt `PhysicsSystem` it owns; threadpool state.|
| `ShapeBlob` hash table (32 MiB)      | None — owned by sibling `shapes/`.                                         |
| Snapshot scratch arena (16 MiB)      | None — owned by sibling `snapshot/`.                                       |
| Query result buffers (8 MiB)         | None — owned by sibling `queries/`.                                        |
| Contact event ring (8 MiB)           | None — owned by sibling `contact/`.                                        |

The cluster's **non-Jolt** resident bytes are dominated by:

- `PhysicsConfigView` (~1 KiB; one struct, mostly POD).
- `AccumulatorImpl` (~32 B; three fields).
- `PhysicsWorldImpl` itself (~256 B with the cold pointer block).

These fit inside the Jolt-pool 64 MiB sub-share trivially. There
is no separate cluster sub-pool; allocations stamp
`ContextTag::physics` and are accounted in the row.

### 9.4 Allocator rules

Per SPEC §9.4 every allocation under `physics/src/world/**` is
stamped with `ContextTag::physics` at the allocator-handle level
(`perf-budget.md` Allocator Rule #1). The handle is supplied by the
cluster's `glibre_plugin_register` (§8.2.2) from `core`'s plugin
loader; module call sites are tag-free. The cluster makes only
three distinct allocations:

1. **`PhysicsWorldImpl` itself** — one `eastl::make_unique` at
   `create` step 7. ~256 B.
2. **`PhysicsConfig.layer_filter`** — one `eastl::make_shared` at
   the §5 `LayerFilter::create` call (sibling-side; cluster only
   stores the shared_ptr). KiB-scale for the layer-pair matrix.
3. **Sibling-table allocations** — five `eastl::make_unique` calls
   in `create` step 4. Each is tagged inside the sibling's TU; the
   cluster does not allocate the table contents.

Under `GLIBRE_ALLOC_STRICT=1`, an allocation that would push live
`ContextTag::physics` bytes above 128 MiB returns
`Result<>` with `core::Error::OutOfBudget` (`perf-budget.md`
Allocator Rule #2). The cluster's `create` propagates via the
monadic chain; failure to handle aborts with the diagnostic dump.

### 9.5 GPU and submit halves

The cluster consumes **no** GPU budget and **no** CPU submit
budget. Phase 3 finishes before phase 6's `RenderFrame` extract
begins; the cluster writes no GPU resources of its own. Render
reads body transforms from the post-phase-5 `GlobalTransform` ECS
column (SPEC §6.5 seam #1), not from a physics-side encoder.

### 9.6 CI gate hooks the cluster owns

Per SPEC §9.6, the per-context CI gate requires per-row
benchmarks. The cluster contributes (one Catch2 case per row;
PR fails on any breach):

| Stage                                   | `BENCHMARK_CELL` test name                                              | CPU ceiling | Source                                       |
|-----------------------------------------|--------------------------------------------------------------------------|-------------|----------------------------------------------|
| Cluster-side `advance` overhead          | `physics/world: advance_dispatch_overhead_two_substep`                  | 0.05 ms     | §9.2 slice "step_one dispatch overhead"      |
| Accumulator drive                        | `physics/world: accumulator_advance_four_substep_loop`                  | 0.01 ms     | §9.2 slice "Accumulator::advance"            |
| Phase-3 driver total (composite)         | `physics/world: phase3_total_two_substep` (SPEC §9.6.1, mandated)       | 2.00 ms     | sums to the cell                              |
| 60 fps S1 sample-scene fixture           | `physics/world: s1_sample_scene_60fps_600frames` (SPEC §9.6.3)          | p99 ≤ 2.00  | end-to-end                                   |

The composite `physics/world: phase3_total_two_substep` is the
SPEC §9.6.1 mandated row; the two cluster-specific rows are added
so a regression that puts 0.5 ms of overhead into the dispatch
without changing the Jolt step is caught at the cluster ceiling.
All three rows live under `tests/physics/world/`.

## 10. Failure modes

The cluster raises a closed subset of the `physics::Error` enum
(SPEC §5; SPEC §10.1 documents each arm authoritatively). Each arm
below names the trigger condition specific to **`PhysicsWorld` /
`PhysicsConfig` / `Accumulator`**, the recovery posture, the log
severity, and the SPEC §10.1 row that owns the arm. This design
adds the per-primitive trigger detail the implementer needs.

The arms are split by cluster primitive (§3 references). All arms
are documented authoritatively in SPEC §10.1; this section refines
the per-primitive triggers and routes.

### 10.1 `PhysicsWorld` lifecycle arms (§3.4)

| Arm                              | Trigger (in this cluster)                                                                      | Recovery        | Severity (default) | SPEC ref      |
|----------------------------------|------------------------------------------------------------------------------------------------|-----------------|--------------------|---------------|
| `ConfigInvalid`                  | `create` step 1 — `fixed_dt <= 0`, `max_substeps == 0`, any budget == 0, or `LayerFilter::validate()` returned unexpected. | Refuse + caller fixes record | `error` | SPEC §10.1     |
| `WorldNotInitialised`            | Any non-`create` entry point invoked on a default-constructed `PhysicsWorld` handle (programming error). | Refuse | `error` | SPEC §10.1     |
| `WorldAlreadyInitialised`        | Second `create` against the same `ecs::World` (single-owner; §3.4 invariant 2).                | Refuse — previous-good world unaffected | `warn` | SPEC §10.1 |
| `BudgetExceeded`                 | Sibling-table allocation at `create` step 4 fails (e.g. `BodyIdAllocator::create(max_bodies)` busts `ContextTag::physics`). | Refuse — caller raises budget on fresh world | `error` | SPEC §10.1 |
| `JoltMiddlemanUnavailable`       | `create` step 2 — `JoltMiddleman::load_from_engine()` returned unexpected (middleman dylib not loaded). | Abort cluster init; engine boots without physics | `error` | SPEC §10.1 |
| `JoltMiddlemanHashMismatch`      | `create` step 2 — `middleman_->require_hash(host_abi_hash)` returned unexpected. Also at `on_resume` step 1 (hot-reload). | Refuse — operator rebuilds physics plugin | `warn` | SPEC §10.1 |

### 10.2 `Accumulator` arms (§3.3)

The accumulator has no public `Result<T>` surface — its state
mutations are §3.4-internal to `PhysicsWorld::advance`. Failures
surface through `PhysicsWorld::advance`'s arms.

| Arm                              | Trigger (in this cluster)                                                                      | Recovery        | Severity (default) | SPEC ref      |
|----------------------------------|------------------------------------------------------------------------------------------------|-----------------|--------------------|---------------|
| `StepCalledOutsidePhase3`        | `advance` phase guard observed `current_phase != PhysicsFixed` (also fires for `snapshot` / `restore` outside phase 5+). | Refuse — caller routes through `FrameLoop` | `error` | SPEC §10.1 |
| `AccumulatorClampExceeded`       | `advance` substep loop reached `report.substeps_dropped > 0` **and** `config.determinism_gate == Hard`. | Clamp under `SoftWarn`; CI promotes to error under `Hard` | `warn` (default) / `error` (Hard) | SPEC §10.1 |

Under `SoftWarn` (default in shipping), the clamp is a `Warning::
AccumulatorClamped` log line plus a populated `AdvanceReport`; the
typed `Error` arm fires only under `Hard`. This is the SPEC §10.3
CI-vs-shipping split.

### 10.3 `PhysicsWorld::step_one` arms (§3.4.4)

| Arm                              | Trigger (in this cluster)                                                                      | Recovery        | Severity (default) | SPEC ref      |
|----------------------------------|------------------------------------------------------------------------------------------------|-----------------|--------------------|---------------|
| `SubstepEcsCommitInverted`       | A sibling mirror seam (debug-build assertion) detected ECS↔Jolt cross-traffic between the entry and exit barriers. The driver flags catch it; the typed arm fires when promoted under `Hard`. | Abort substep — no commit | `error` | SPEC §10.1 |
| `NumericalInstabilityDetected`   | `step_one` step-3 NaN check found a NaN / inf in Jolt's post-step body / joint outputs (position / orientation / velocity / impulse). | Abort substep — `rollback_jolt_step` restores pre-step kinematic shadow | `error` (always) | SPEC §10.1 |
| `DeterminismCheckFailed`         | `step_one` step-5 iteration-hash compare (under `Hard`) found a divergence from the expected hash (mid-step iteration order corruption). | Abort substep — rollback as above; CI fails the run | `error` (Hard) / `warn` (SoftWarn) | SPEC §10.1 |

`NumericalInstabilityDetected` is **never downgraded** — a NaN in
the simulation state is a content / gameplay bug operators must see
even in shipping (SPEC §10.3 row).

### 10.4 Snapshot arms (§3.4 forwards)

| Arm                              | Trigger (in this cluster)                                                                      | Recovery        | Severity (default) | SPEC ref      |
|----------------------------------|------------------------------------------------------------------------------------------------|-----------------|--------------------|---------------|
| `SnapshotSchemaMismatch`         | `restore` (or `on_resume` step 4) found `snapshot.physics_config_hash != config_view.content_hash`. | Refuse restore — caller restores compatible snapshot | `warn` (hot-reload) / `error` (CI determinism gate) | SPEC §10.1 |
| `SnapshotDeserialiseFailed`      | `PhysicsSnapshot::from_bytes` returned unexpected from inside `restore`. | Refuse restore — caller treats as unrecoverable | `error` | SPEC §10.1 |
| `HotReloadStateUnmigratable`     | `PhysicsConfig::from_record` (hot-reload resume) found the surviving record's `since` exceeds the build's reader-side support. | Refuse swap — operator authors migration body | `warn` | SPEC §10.1 |

### 10.5 Routed arms (cluster does not detect)

The cluster forwards these arms from sibling aggregates. Listed for
completeness (SPEC §10.2 documents the per-aggregate detection
points):

- `BodyNotFound`, `BodyMotionTypeImmutable`, `BodyStillReferencedByJoint`,
  `ColliderShapeRequired`, `ShapeBlobMalformed`,
  `ShapeBlobVersionUnsupported`, `ShapeHandleStale`, `ShapeBlobMissing`
  — sibling `bodies/` + `shapes/`.
- `JointEndpointInvalid`, `JointDanglingEndpoint`, `JointKindUnsupported`,
  `JointBroken` — sibling `joints/`.
- `QueryDuringStep`, `QueryFilterInvalid` — sibling `queries/`.
- `SnapshotBodyIdUnresolved` — sibling `snapshot/snapshot_restore.cpp`.

These cross the §5 facade as forwarding errors; the cluster does
not author the detection.

### 10.6 Caller-side recovery posture

Per SPEC §10.2 / §10.4, callers handle each arm at exactly one
boundary. For cluster arms specifically:

- **`core` `FrameLoop`** — `advance` errors (`StepCalledOutsidePhase3`,
  `AccumulatorClampExceeded` under `Hard`, `NumericalInstabilityDetected`,
  `DeterminismCheckFailed`, `SubstepEcsCommitInverted`) propagate up
  to the schedule's per-system error wrapper, which logs once at the
  arm's severity and continues to the next phase. A failed substep
  does not abort the frame — phase 4+ proceed.
- **Plugin authors** (gameplay systems calling `add_body` etc.) —
  forwarded arms (`BodyNotFound` etc.) handled per the sibling's
  surface; the cluster propagates verbatim.
- **Hot-reload loader** (during phase 8) — the four cluster-contributed
  refusal arms (§8.3) wrap into `core::Error::HotReloadRefused` with
  the matching inner cause. Refusal logs at `warn` per the protocol;
  the previous-good plugin keeps stepping.

### 10.7 Determinism obligation

Every non-hot-reload arm above must fire **byte-equal across runs**
on byte-equal inputs (PHILOSOPHY §7). Specifically: the
`AccumulatorClampExceeded` clamp threshold compares against
`config.fixed_dt` which is bit-exact across hosts (SPEC §6.4 R4);
the `NumericalInstabilityDetected` detection is `<cmath>::isnan`
which is portable; the `DeterminismCheckFailed` iteration hash is
BLAKE3 over `std::bit_cast` bytes which is portable. The §11
acceptance test `physics/world: error_arms_byte_equal_across_runs`
asserts the error stream from a fixed input trace is byte-equal
across two consecutive runs.

## 11. Test plan

Unit + integration + perf tests required to validate the §1–§10
invariants. Catch2 test names are stable; the SPEC §11 `[STORY]`
parent issue is named in the right column where one exists.

### 11.1 Unit tests (one Catch2 case per row)

Lives under `tests/physics/world/`.

| Test name                                                       | What it asserts                                                                                                              | §-ref          | Story |
|-----------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------|----------------|-------|
| `physics/world: create_returns_unique_world`                     | `PhysicsWorld::create` returns a unique `eastl::unique_ptr<PhysicsWorld>`; second `create` against the same `ecs::World` → `WorldAlreadyInitialised`. | §3.4.2, §4.1.1 | #421  |
| `physics/world: create_refuses_invalid_config`                   | `fixed_dt = 0` → `ConfigInvalid`; `max_substeps = 0` → `ConfigInvalid`; partial `LayerFilter` → `ConfigInvalid`.             | §3.4.2 step 1, §4.1.2 inv 4 | #421 |
| `physics/world: create_refuses_missing_middleman`                | `JoltMiddleman::load_from_engine` mocked-unavailable → `JoltMiddlemanUnavailable`.                                            | §3.4.2 step 2  | #421  |
| `physics/world: create_refuses_middleman_hash_mismatch`          | Middleman-mock returning a different ABI hash from the host's → `JoltMiddlemanHashMismatch`.                                  | §3.4.2 step 2, §4.1.13 inv 2 | #421 |
| `physics/world: create_computes_content_hash_byte_equal`         | Two `PhysicsWorld::create`s with byte-equal `PhysicsConfig` produce byte-equal `config().content_hash`.                       | §3.5 R4, §7.1.1 inv 3 | #423 |
| `physics/world: accumulator_advance_drains_substeps`             | `accumulator.advance(2 * fixed_dt)` returns `AdvanceReport { substeps_run = 2, substeps_dropped = 0, carry = 0 }`.            | §3.3, §4.1.3 inv 1 | #425 |
| `physics/world: accumulator_advance_clamps_at_four_substeps`     | `advance(8 * fixed_dt)` returns `substeps_run = 4, substeps_dropped = 4, carry = 0`; `Warning::AccumulatorClamped` logged.   | §3.3, §4.1.3 inv 2 | #425 |
| `physics/world: accumulator_carry_preserved_across_advance`      | `advance(0.5 * fixed_dt)` then `advance(0.5 * fixed_dt + epsilon)` runs exactly one substep; carry preserved verbatim across calls. | §3.3 inv 3 | #425 |
| `physics/world: accumulator_clamp_under_hard_gate_returns_error` | `advance(8 * fixed_dt)` with `determinism_gate = Hard` returns `AccumulatorClampExceeded`; under `SoftWarn` returns the report. | §3.3 inv 2, §10.1 row | #425 |
| `physics/world: accumulator_reseed_round_trip`                   | `reseed(carry, tick)` then read-back of `carry()` / `tick_count()` returns the same bit-pattern (`std::bit_cast<u32>`).      | §3.3 inv 5, §3.5 R4 | #448  |
| `physics/world: advance_refuses_outside_phase3`                  | `advance` called outside `Phase::PhysicsFixed` → `StepCalledOutsidePhase3`.                                                  | §3.4.3, §10.1 | #427  |
| `physics/world: advance_serializes_two_substeps_byte_equal`      | Two consecutive `advance(fixed_dt)` runs against a fixed body set produce byte-equal post-step snapshot bytes.                | §3.4.3, §3.5 cell, §11 #423 | #423 |
| `physics/world: step_one_rolls_back_on_nan`                      | A test-only `JoltMiddleman::step` that injects NaN into a body's velocity → `step_one` returns `NumericalInstabilityDetected`; pre-step kinematic shadow restored. | §3.4.4 step 3, §10.1 | (no story; CI invariant) |
| `physics/world: step_one_detects_iteration_hash_drift_under_hard`| A test-only mid-step iteration-order corruption (sibling-mock) → under `Hard`, `step_one` returns `DeterminismCheckFailed`; under `SoftWarn`, commits with warn. | §3.4.4 step 5, §10.3 | (no story; CI invariant) |
| `physics/world: substep_ecs_commit_inverted_aborts`              | A test-only sibling that writes ECS during the in-flight flag → `SubstepEcsCommitInverted`; substep aborts.                  | §3.6, §10.1   | (no story; CI invariant) |
| `physics/world: world_tick_increments_per_substep`               | `world_tick()` advances by `report.substeps_run` after `advance`; never advances on rolled-back substep.                     | §3.3, §3.4.4  | #423  |
| `physics/world: snapshot_round_trip_carry_and_tick`              | `snapshot()` → `restore()` round-trip preserves `carry()` and `tick_count()` byte-equal.                                     | §3.4 forwards, §3.3 inv 5 | #448 |
| `physics/world: restore_refuses_config_hash_drift`               | `restore` with snapshot whose `physics_config_hash` ≠ live `config().content_hash` → `SnapshotSchemaMismatch`.                | §8.2.2 step 4, §10.1 | #448 |
| `physics/world: restore_refuses_corrupt_bytes`                   | `restore` with truncated snapshot bytes → `SnapshotDeserialiseFailed`.                                                       | §8.2.2 step 4, §10.1 | #448 |
| `physics/world: facade_methods_are_noexcept`                     | Compile-time check: every public method on `PhysicsWorld` / `Accumulator` has `noexcept` in its signature.                   | §6.4, §4.1.1 inv 3 | (no story; ABI invariant) |
| `physics/world: error_arms_byte_equal_across_runs`               | The error stream from a fixed input trace is byte-equal across two consecutive runs (PHILOSOPHY §7).                          | §10.7         | (no story; CI invariant) |

### 11.2 Integration tests

Lives under `tests/physics/world/integration/`. Drives the cluster
through multi-frame sequences via the `core` `FrameLoop` test harness.

| Test name                                                                | Scenario                                                                                                                                | §-ref         | Story  |
|--------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------|---------------|--------|
| `physics/world: byte_equal_snapshot_at_tick_1000_macos_arm64_vs_x64`     | S1 fixture (1 char + 200 props + 8 lights); 1000 frames; snapshot at tick 1000 on M1-arm64 vs macOS-x64 hosts is byte-equal.            | §3.5 cell, §6.6 | #423  |
| `physics/world: hot_reload_preserves_carry_and_tick`                     | Tower fixture (50 boxes, 5 joints, 2 triggers); reload at frame 8; post-reload `accumulator.carry()` / `tick_count()` byte-equal to pre-reload. | §8.2, SPEC §8.6 | #449 |
| `physics/world: hot_reload_refuses_config_hash_drift`                    | Reload `physics-v2` with the `PhysicsConfigRecord` mutated; `HotReloadRefused { PluginInitFailed { SnapshotSchemaMismatch } }`; previous plugin keeps stepping. | §8.3, SPEC §8.6 | #449 |
| `physics/world: hot_reload_refuses_middleman_hash_mismatch`              | Reload `physics-v2-bad-abi`; `HotReloadRefused { PluginAbiHashMismatch }` mapping to `JoltMiddlemanHashMismatch`.                       | §8.3, SPEC §8.6 | #449 |
| `physics/world: phase_guard_refuses_advance_during_phase_5`              | A test system in phase 5 calls `physics_world->advance(dt)` → `StepCalledOutsidePhase3`; the real phase-3 system runs unaffected.       | §3.4.3, §6.1  | #427  |
| `physics/world: accumulator_clamp_logged_but_simulation_continues`       | Inject 0.5-second wall-clock spike (`real_dt = 30 * fixed_dt`); under `SoftWarn`, four substeps run, residual carry dropped, `Warning::AccumulatorClamped` logged, simulation resumes deterministic at next frame. | §3.3, §10.1 | #425 |
| `physics/world: bench_phase3_two_substep_under_s1`                       | The §9.6 `BENCHMARK_CELL` perf assert (2.00 ms ceiling).                                                                                | §9.6.1        | (CI)   |
| `physics/world: bench_advance_dispatch_overhead`                         | Cluster-side overhead `BENCHMARK_CELL` (0.05 ms ceiling).                                                                               | §9.6 row 1    | (CI)   |
| `physics/world: bench_accumulator_drive`                                 | Accumulator `BENCHMARK_CELL` (0.01 ms ceiling).                                                                                          | §9.6 row 2    | (CI)   |

### 11.3 Property-based tests

Lives under `tests/physics/world/property/`. Use Catch2 `GENERATE`
for fuzz-style coverage.

| Test name                                                          | Property                                                                                                                            | §-ref |
|--------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------|-------|
| `physics/world: accumulator_advance_total_property`                | For random `real_dt > 0` sequences, `Σ substeps_run * fixed_dt + final_carry == Σ real_dt` modulo clamp drops.                       | §3.3  |
| `physics/world: accumulator_clamp_drop_total_property`             | For random spike sequences, post-clamp `tick_count` advances exactly `report.substeps_run` per call (never more, never less).        | §3.3  |
| `physics/world: content_hash_stability_under_field_permutation`    | Two `PhysicsConfig`s differing only by struct-field-init-order (same byte content) produce the same `content_hash`.                  | §3.5 R4 |
| `physics/world: snapshot_round_trip_random_carry`                  | For random `(carry, tick_count)` reseed values, `snapshot()` then `restore()` returns the same bit-pattern.                          | §3.3 inv 5 |

### 11.4 What this design does **not** test

- **Sibling-aggregate seams** — body / joint / shape / query /
  contact / snapshot mechanics. Each has its own design spike under
  #791; this cluster only tests its own facade brokering.
- **Multi-thread access** — single-thread sim in MVP (SPEC §6.6).
  When per-system parallelism lands, ThreadSanitizer + the access-set
  DAG cover the new surface.
- **Multi-world** — refused in MVP (SPEC §3.2 collapse #8). The
  `WorldAlreadyInitialised` arm has a unit test that asserts the
  refusal shape, but the multi-world boundary itself is post-MVP.
- **Gravity modes** — refused in MVP (R-4.1.14, §2.3). When
  `GravityMode` reactivates, new unit tests gate the per-mode
  integration behaviour.
- **Per-entity `SubstepOverride`** — refused in MVP (R-4.1.2 second
  half, §2.1). The MVP test surface guards exactly the "single
  substep cap, per-world" behaviour.
- **GPU / Metal interaction** — refused (SPEC §1, §3.3). No tests.

### 11.5 Acceptance-criteria mapping

The `[STORY]` issues already named in SPEC §11 that this design's
tests close (one Catch2 case per story; the §11.1–§11.3 tables list
them per story column):

- #421 `physics: PhysicsWorld init creates one Jolt instance per ECS world`
- #423 `physics: deterministic phase-3 step byte-equal across hosts`
- #425 `physics: fixed-timestep accumulator with bounded catch-up + carry preserved`
- #427 `physics: refuse step calls outside phase 3`
- #448 `physics: PhysicsSnapshot save/restore round-trip byte-equal` (cluster slice — accumulator carry/tick + config hash)
- #449 `physics: hot-reload preserves world state across snapshot at phase 8` (cluster slice — accumulator + config + middleman gate)

The remaining stories from SPEC §11 (#429, #431, #434, #435, #437,
#439, #441, #443, #445, #451) belong to sibling aggregates and are
out of scope for this design.

## 12. Open questions

Each `[OPEN]` is a follow-up amendment trigger; resolution amends
the matching SPEC section in place, not this design. Per the
PHILOSOPHY §3 + workflow rule, no `[OPEN]` is discharged silently.

- **[OPEN]** Should `Accumulator::carry_` be `f64` instead of `f32`?
  R-4.1.NF3 cross-platform byte-equality is tight at `f32` (`x86-64`
  vs `arm64` differ on `fma` and on `cmath::sin/cos` rounding under
  fast-math, which we already disable; the `f32` carry is fine in
  isolation). But a frame-rate trace that runs for hours accumulates
  `f32` rounding noise into the `tick_count` advancement when
  `real_dt` is near `fixed_dt - epsilon`. Re-evaluate when an MVP
  test surface measures the rounding-noise budget; resolution amends
  SPEC §4.1.3 + §7.1.4 in place. Tracked by §3.3.
- **[OPEN]** Should `PhysicsConfig.determinism_gate` ship at `Hard`
  default in **all** builds (not just CI)? PHILOSOPHY §7 says yes;
  operational reality (heterogeneous hardware in shipping) says
  `SoftWarn`. SPEC §10.3 resolves at the documented two-value shape;
  re-open if a downstream consumer (lock-step multiplayer, replay
  competitive mode) demonstrates that `SoftWarn` shipping is
  load-bearing. Tracked by SPEC §10.7.
- **[OPEN]** Should the `step_one` step-3 NaN gate also check for
  `±Inf`? `<cmath>::isfinite` would catch both; current text says
  "NaN or infinity" but the implementation should be `!isfinite(x)`
  not just `isnan(x)`. Frozen at `!isfinite` until the
  implementation plan that introduces `step_one` lands; the
  test-only NaN-injection fixture uses `1.0f / 0.0f` (positive Inf)
  to guarantee the gate catches both. Tracked by §3.4.4 step 3.
- **[OPEN]** Should `step_one` step-5 iteration-hash compute use
  BLAKE3-128 (28 bytes hash) or BLAKE3-64 (8 bytes)? BLAKE3-128 has
  more collision-resistance margin but costs ~30 ns more per
  substep on the M1 baseline. Frozen at BLAKE3-128 (the §9.2 0.030
  ms slice budgets it); re-evaluate if the determinism-gate fixture
  measures the hash cost as load-bearing. Tracked by §3.4.4 step 5.
- **[OPEN]** Should `PhysicsWorldImpl::on_plugin_reload` (§5,
  hot-reload entry) be a free function instead of a method? Free
  function would be more idiomatic for the loader's vtable swap
  (the method's `this` pointer is the very thing being swapped).
  The §5 facade currently declares it as a method on `PhysicsWorld`;
  the implementation calls into a free `on_resume(...)` body inside
  the new plugin's image. Frozen at the §5 method shape; the
  free-function refactor is a §5 amendment, not a §8 amendment.
- **[OPEN]** Should the cluster expose a read-only
  `PhysicsWorld::current_substep_index() -> u8` for the `tools`
  profiler? Currently the substep loop is opaque; profiler wants
  per-substep markers. The `world_tick()` accessor exposes the
  sum-of-substeps-ever, not the per-frame index. Re-evaluate when
  the `tools` profiler design lands; the answer is likely yes via
  a thread-local read. Tracked by §3.4.3.

Resolution of any `[OPEN]` lands the decision into
`reviews/decisions/` (when cross-aggregate) or amends SPEC §3 / §4 /
§9 in place (when local to this cluster); per the workflow no
`[OPEN]` is discharged silently.
