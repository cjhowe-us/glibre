# Core Spec

## 1. Purpose

The `core` context owns one responsibility: hosting the engine-level
runtime that every plugin links against. Concretely, that is the
codegen-driven archetype ECS (world, entities, components, archetype
storage, queries, scheduler, change ticks), the plugin loader with its
ABI-hash gate and frame-boundary hot-reload barrier, the engine-wide
frame loop and frame-phase ordering, and the registries plugins share
across the ABI seam — the type registry and the asset handle table.
Core refuses to own anything outside that seam: no domain logic
(render, physics, audio, scripting, animation, AI, scene, networking,
serialization formats, editor UI — all live in plugin `.dylib`s), no
I/O (filesystem, sockets, windowing, input devices), no GPU work
(devices, queues, command buffers, shaders), and no audio devices.
Core also refuses to be a thin wrapper around a third-party ECS;
glibre owns its archetype layout end-to-end via codegen, so `entt`
and equivalents are explicitly out of scope. Per SRP, every reason
core has to change must trace back to one of those listed
responsibilities; anything else is a plugin concern.

## 2. Ubiquitous Language

Terms used unchanged in code (identifiers, file names, comments).

| Term | Meaning |
|------|---------|
| `World` | One ECS instance: archetype storage + entity allocator + resource map + change-tick clock. |
| `Entity` | 64-bit generational handle (index + generation); the only stable cross-frame reference to a row. |
| `Component` | Plain-data type registered with the type registry; identifies a column in archetype storage. |
| `Archetype` | Set of component types defining one storage table; entities sharing the set live together. |
| `Chunk` | Cache-aligned, fixed-size slab inside an archetype holding contiguous SoA columns. |
| `Query` | Compiled descriptor — With/Without/Changed terms — resolving to an iterable archetype set. |
| `System` | Function with declared read/write access; scheduled by the engine, never called directly. |
| `Schedule` | DAG of systems built from access sets; compiled once into a `CompiledFrame`. |
| `Phase` | Named ordering bucket inside the frame (Input, Simulation, Physics, Render, …). |
| `FrameLoop` | The engine-wide driver that advances `Phase`s in order each tick. |
| `ChangeTick` | Monotonic counter advanced on mutable access, the basis for `Changed` filters. |
| `CommandBuffer` | Per-system deferred mutation log applied at sync points in deterministic order. |
| `Resource` | Typed singleton owned by a `World`, accessed through the same scheduler discipline. |
| `Plugin` | A `.dylib` registering components, systems, and resources via the plugin trait. |
| `PluginLoader` | The core service that discovers, validates, loads, and unloads plugins. |
| `AbiHash` | Compile-time hash of the middleman `.dylib` ABI; load is refused on mismatch. |
| `HotReloadBarrier` | Frame-boundary gate (drain → swap → migrate → resume); reload never crosses a frame. |
| `TypeRegistry` | Lock-free, init-time-immutable map from `TypeId` to descriptor (size, align, drop, layout). |
| `AssetHandle` | Stable opaque handle into the engine-wide asset table; resolution is plugin-defined. |
| `Middleman` | The single shared `.dylib` carrying codegen'd type layouts both engine and plugins link against. |

## 3. Derived From

Harmonius prior art is treated as input for fresh research, not as
authority (PHILOSOPHY §"How harmonius is used"). The citations below
identify the harmonius requirement IDs and design files we read while
re-deriving the `core` boundary; every conclusion was independently
justified against PHILOSOPHY §1–§10 and the engine-wide decision records
(`reviews/decisions/frame-phases.md`, `reviews/decisions/error-model.md`).
Glibre does not preserve harmonius decisions; it re-uses harmonius only
as a research starting point for the small set of responsibilities the
`core` context owns.

### 3.1 Harmonius Inputs

Per-area research inputs, listed with the glibre `core` concept they
informed.

| Area / harmonius source | Requirement IDs read | Design files read | Glibre `core` concept(s) informed |
|---|---|---|---|
| Entity-component-system | R-1.1.1 — R-1.1.41 (`requirements/core-runtime/entity-component-system.md`) | `design/core-runtime/ecs.md`, `design/core-runtime/change-detection.md` | `World`, `Entity`, `Component`, `Archetype`, `Chunk`, `Query`, `System`, `Schedule`, `ChangeTick`, `CommandBuffer`, `Resource` |
| Game loop | R-1.11.1 — R-1.11.10 (`requirements/core-runtime/game-loop.md`) | `design/core-runtime/game-loop.md` | `FrameLoop`, `Phase`, `CompiledFrame` (compiled schedule) |
| Plugin system | R-1.6.1 — R-1.6.10 (`requirements/core-runtime/plugin-system.md`) | `design/core-runtime/events-plugins.md`, `design/core-runtime/hot-reload-protocol.md` | `Plugin`, `PluginLoader`, `AbiHash`, `HotReloadBarrier`, `Middleman` |
| Reflection / type system | R-1.3.1 — R-1.3.2 (`requirements/core-runtime/reflection-and-type-system.md`) | `design/core-runtime/reflection-serialization.md` (registry portions only) | `TypeRegistry` (size / align / drop / layout descriptors only) |
| Memory management | R-1.7.5, R-1.7.6 (`requirements/core-runtime/memory-management.md`) | `design/core-runtime/primitives.md` (handle / slot-map portions) | `AssetHandle`, generational-index handle convention |
| Events / messaging | R-1.5.4, R-1.5.5, R-1.5.13 (`requirements/core-runtime/events-and-messaging.md`) | `design/core-runtime/events-plugins.md` (observer + command-buffer portions) | `CommandBuffer` semantics; lifecycle observer hook contract on `World` |
| ID conventions | F-1.10.1 — F-1.10.5 (`design/core-runtime/ids.md`) | `design/core-runtime/ids.md` | `Entity` 64-bit generational handle layout; ID stability levels for `AssetHandle` |
| Hot-reload protocol | F-1.11.1 — F-1.11.6 (`design/core-runtime/hot-reload-protocol.md`) | `design/core-runtime/hot-reload-protocol.md` | `HotReloadBarrier` drain → swap → migrate → resume contract; ABI-hash refusal |
| Error model (input only) | (no harmonius requirement; `design/core-runtime/error.md`) | `design/core-runtime/error.md` | confirmed `core::Error` enum scope per `reviews/decisions/error-model.md` |

Specifically NOT read as inputs to `core` (routed elsewhere — see §3.3):
`async-io.md`, `serialization.md`, `scene-and-transforms.md`,
`spatial-indexing.md`, `algorithms.md` (gameplay primitives portion),
`memory-async-io.md`, `console-variables.md`, `graph-runtime.md`,
`io.md`, `primitives.md` (container portion), `scene-transforms.md`.

### 3.2 Occam Collapses

Every harmonius requirement that survived re-derivation collapsed to a
strictly smaller set of glibre primitives. Each collapse below names
the harmonius concepts on the left, the single glibre primitive on the
right, and the SOLID / determinism rationale that licensed the
collapse.

1. **Eight harmonius frame phases + the implicit reload-outside-the-loop
   model → one nine-phase `FrameLoop` with the hot-reload barrier as
   phase 8.** Inputs: R-1.11.1 (8-phase pipeline: Input, Network Rx,
   Simulation, AI, Physics, Animation, Frame Snapshot, Frame End) plus
   R-1.6.5 / F-1.11.1 (hot-reload as a separate concept). Glibre
   collapses both into one numbered `Phase` enum owned by `core`,
   ordering documented in `reviews/decisions/frame-phases.md`. The
   barrier is named at exactly one slot (phase 8), not "somewhere on
   the side." Rationale: PHILOSOPHY §8 (hot-reload at frame
   boundaries) demands a single position; SRP says one schedule, one
   reason to change. Audio / AI / network become systems inside
   existing phases rather than top-level slots, since their data
   ride the same snapshot bus.

2. **R-1.11.8 `CompiledFrame` + R-1.1.26 schedule DAG + R-1.1.27
   hierarchical system groups + R-1.1.28 run criteria + R-1.1.30
   exclusive systems → one `Schedule` compiled to one `CompiledFrame`
   per frame-phase DAG.** Glibre keeps DAG-from-access-sets and
   compile-once-reuse; it rejects nested phase hierarchies with custom
   ordering operators and per-system run criteria as runtime concerns
   that belong in the gameplay/scripting plugin's logic phase, not in
   `core`. Rationale: SRP — `core` owns ordering, plugins own
   conditional execution. Determinism is preserved by the access-set
   DAG; conditional run logic is leaked complexity.

3. **R-1.1.1 (archetype tables), R-1.1.7 (shared components), R-1.1.40
   (AoSoA tiled chunks), R-1.1.41 (compiled query plans with bloom
   filters), and the harmonius "compile-time + runtime registration"
   split → one codegen-driven archetype layout owned end-to-end by
   `core`.** Glibre runs the codegen pipeline at build time; the
   middleman `.dylib` carries layouts; runtime "register a new type"
   is refused. Rationale: PHILOSOPHY §6 (zero runtime reflection in
   shipping builds) plus §3 (engine owns its archetype layout
   end-to-end). The harmonius mix of static + dynamic registration is
   one boundary too many.

4. **R-1.1.5 (component derive + dynamic register API), R-1.3.1
   (10,000 runtime types, dynamic registration), R-1.3.3 (path-based
   property access), R-1.3.4 (collection reflection), R-1.3.5
   (`DynamicValue`), R-1.3.6 (attribute system), R-1.3.7 (registry
   trait impls), R-1.3.8 — R-1.3.11 (`Reflect` trait, sub-traits,
   `FromReflect`) → one immutable-after-init `TypeRegistry` storing
   only `{TypeId → {size, align, drop, layout}}`.** Everything reflective
   beyond size/align/drop/layout (path access, dynamic value,
   reflect trait, attributes) is not a `core` concern. Rationale:
   PHILOSOPHY §6 forbids runtime reflection for shipping; the editor
   and serializer plugins consume static codegen output, not a
   reflection API. `core` exposes the minimum the ABI seam needs.

5. **R-1.6.1 (plugin trait + dependency declaration), R-1.6.3 / R-1.6.4
   (dependency validation + topological order), R-1.6.5 / R-1.6.6
   (hot-reload + state migration), R-1.6.7 (ABI hash gate), R-1.6.9
   (middleman `.dylib`), F-1.11.1 — F-1.11.6 (hot-reload protocol)
   → one `PluginLoader` + one `HotReloadBarrier` + one ABI-hash check.**
   Glibre requires `AbiHash` matched against the middleman dylib and
   refuses load on mismatch (PHILOSOPHY §9). The barrier is the
   drain → swap → migrate → resume sequence at phase 8. Plugin groups
   (R-1.6.2) and capability advertisement (R-1.6.8) are deferred —
   they are configuration, not core. Rationale: SRP — one loader
   class, one refusal policy, one frame-position; the loader is a
   primitive, capability brokering is a higher-level concern.

6. **R-1.1.12 (entity = 32-bit index + 32-bit generation, 4 M
   entities/world), R-1.7.5 (generational handles), R-1.7.6 (slot map),
   F-1.10.2 (ID taxonomy across `Entity`, `ComponentId`, `AssetId`,
   `GraphInstanceId`, `NodeId`, `BoneIndex`, `VoiceId`,
   `NetworkEntityId`) → one `Entity` (64-bit generational, the only
   stable cross-frame reference into a `World`) + one `AssetHandle`
   (opaque generational handle into the engine-wide asset table).**
   Other ID kinds (`NetworkEntityId`, `GraphInstanceId`, `NodeId`,
   `BoneIndex`, `VoiceId`) are owned by their consuming context, not
   by `core`. Rationale: SRP — `core` defines only the IDs whose
   meaning is shared across the ABI seam (entities, asset handles,
   `TypeId`); domain IDs belong to the domain that defines them.

7. **R-1.1.14 (entity names + path lookup), R-1.1.15 — R-1.1.17
   (relationship pairs, properties, `ChildOf`), R-1.1.37 — R-1.1.38
   (entity templates / prototypes), R-1.1.39 (state machine
   components), R-1.1.31 — R-1.1.32 (observers + entity events with
   propagation), R-1.5.11 (capture/bubble propagation),
   `scene-and-transforms.md` (R-1.2.*) → routed out of `core` to
   `scene` and `gameplay/scripting` contexts.** `core` only owns the
   primitive lifecycle hook surface (the OnAdd / OnRemove / OnSet
   shape, since it touches archetype storage at the storage layer).
   Higher-level event semantics (capture/bubble, hierarchy
   propagation, named-path lookup, prototype IsA inheritance) live in
   plugin contexts that own scene structure. Rationale: SRP — `core`
   refuses scene-graph and gameplay semantics; this is the strongest
   refusal in the spec.

8. **R-1.5.4 + R-1.5.5 + R-1.1.33 + R-1.1.34 (observers + per-system
   command buffers + multi-thread shared command buffers with sort
   keys) → one `CommandBuffer` per system + deterministic sync-point
   ordering.** Glibre keeps the deferred-mutation-with-deterministic-
   replay primitive (it is the only way parallel systems compose under
   PHILOSOPHY §7 determinism) and the observer hook lifecycle, but
   defers reactive-query subscription (R-1.5.6) and bridge channels
   between worlds (R-1.5.9) as concerns that belong with the
   gameplay/scripting context. Rationale: SRP and determinism — the
   minimal kernel ships the smallest mutation primitive that can
   compose into anything richer, and richer event routing is built on
   top by other contexts.

### 3.3 Refused Harmonius Requirements (Routed Elsewhere or Deferred)

The following harmonius requirements appeared in the core-runtime
requirements/design tree but are **out of scope for the `core` context**.
The right column states the new owner or "deferred / post-MVP." This
list is an explicit refusal: `core` will not implement these and pull
requests adding them to `core` should be rejected.

| Harmonius requirement / area | Refusal — routed to |
|---|---|
| R-1.2.1 — R-1.2.14 (scene hierarchy, transforms, dirty tracking, propagation, 2D transforms, previous-frame transforms, scene spawning, scene-instance tracking, ordered child insertion) | `scene` context (separate plugin); `core` owns only the `LocalTransform → GlobalTransform` propagation phase 5 slot, not the data model. |
| R-1.4.1 — R-1.4.15 (binary / text serialization, schema versioning, migration, mixed format, git-friendly text, codegen serialization) | `data` context. `core` only declares the `Fory` codegen pipeline is consumed at component registration time; the serializer itself is not in `core`. |
| R-1.8.1 — R-1.8.18 (Tokio-based async I/O facade, file / network / audio / vectored I/O, priority scheduling, cancellation, buffer pools, VFS, GPU DMA, file metadata) | `platform` context. `core` does no I/O. |
| R-1.9.1 — R-1.9.20 (BVH, uniform grid / octree, 2D BVH, layer masks, frustum culling, AoI, fattened AABBs, double-buffered rebuild, any-hit ray) | `spatial` context (separate plugin). |
| R-1.10.1 — R-1.10.11 (GPU grid upload, graph compilation to shader bytecode, deterministic RNG, condition-expression trees, frame-budgeted work queue, falloff curves, platform tier, compression codec, decaying values, weighted lookup, conditional graphs) | Distributed to `render` (R-1.10.1 — R-1.10.2), `gameplay` (R-1.10.4, R-1.10.6, R-1.10.9 — R-1.10.11), `data` / asset baking (R-1.10.8), `platform` (R-1.10.7), and an engine-wide `rng` primitive (R-1.10.3). None of these are owned by `core`. |
| R-1.7.1 — R-1.7.4 (bump arena, nested arenas, fixed-size pool), R-1.7.7 (per-subsystem memory budgets), R-1.7.8 — R-1.7.9 (allocation profiling), R-1.7.10 (arbitrary-precision numerics), R-1.7.11 (per-worker arenas) | Engine-wide `memory` primitive shared across contexts; not part of `core`'s public ABI surface. `core` does declare the `AssetHandle` and per-`World` resource map (R-1.7.7's tag system maps to context-owned budgets, not core enforcement). |
| R-1.5.1 — R-1.5.3 (typed event channels with double buffering, persistent event streams), R-1.5.6 — R-1.5.7 (reactive query subscription overhead bound), R-1.5.8 (typed singleton resources are kept; ECS resource map is core), R-1.5.9 (cross-world event bridge), R-1.5.10 (flood detection), R-1.5.11 (capture / bubble propagation), R-1.5.12 (stream overflow detection) | `events` plugin (gameplay-tier event bus); `core` retains only the lifecycle observer (R-1.5.4) and command-buffer primitive (R-1.5.5 / R-1.1.33 / R-1.1.34). Reactive subscription performance bounds become a plugin concern. |
| R-1.1.31 — R-1.1.32 (observers with multi-term query matching, entity-targeted events, propagation along `ChildOf`) | The lifecycle hook *shape* (callback at storage-mutation point) stays in `core`; the multi-term-query observer evaluator and event-propagation routing move to the `events` plugin. |
| R-1.1.35 — R-1.1.36 (multiple worlds, world flags Game / Editor / Server / Shadow, entity migration between worlds) | Deferred. MVP runs one `World`; multi-world is post-MVP and will be a `core` extension when a second use case (rollback netcode or editor preview world) actually exists, not before. |
| R-1.1.37 — R-1.1.39 (entity templates, state-machine components) | `gameplay` / `scripting` context. Template inheritance and OnEnter / OnExit / OnTransition are higher-level abstractions over the lifecycle-hook primitive `core` already owns. |
| R-1.3.3 — R-1.3.11 (path-based property access, `DynamicValue`, attribute system, `Reflect` trait, sub-traits, `FromReflect`) | `editor` and `data` contexts. `core` exposes only the type-descriptor record (size / align / drop / layout); reflective features are a plugin layer that consumes static codegen output. |
| R-1.6.2 (plugin groups), R-1.6.8 (capability advertisement) | `tools` / config layer. `core` loads individual plugins and matches ABI hashes; group registration and capability brokering are configuration not present in the kernel. |
| R-1.6.10 (static linking for shipping) | Build-system concern (CMake presets in `cmake/`), not core. |
| Console variables (`design/core-runtime/console-variables.md`) | `tools` / config layer. Not core. |
| Graph runtime (`design/core-runtime/graph-runtime.md`, F-1.15.*) | `scripting` / `render` (material graphs) / `vfx` / `animation` / `ai` per graph kind. Glibre rejects a single shared graph runtime as a cross-domain abstraction without two concrete users sharing one IR (PHILOSOPHY anti-pattern #1). |
| I/O request protocol (`design/core-runtime/io.md`) | `platform` context. |
| Primitives (`design/core-runtime/primitives.md`) — `Handle`, `HandleMap`, `SortedVecMap`, `RingBuffer`, `DirtyRegionSet`, `DispatchTable`, `DeterministicRng`, `SmallVec`, `FixedBitSet` | Engine-wide `primitives` library, vendored / std where possible; not specifically a `core`-context export. The `Entity` / `AssetHandle` generational-index *shape* is core; the generic `Handle<T>` template is a shared utility. |
| Memory async I/O (`design/core-runtime/memory-async-io.md`) | `platform` + engine-wide `memory` primitives. Not core. |
| Reflection-serialization (`design/core-runtime/reflection-serialization.md`) — beyond size/align/drop/layout | `data` (Fory codegen consumer) + `editor` (inspector). Not core. |
| Algorithms test-cases set (cross-domain primitives) | Distributed to consuming domain contexts. Not core. |
| Spatial-index design (BVH + 2D BVH + grid + octree, double-buffered rebuild) | `spatial` context. |

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
