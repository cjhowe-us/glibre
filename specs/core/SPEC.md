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

This section enumerates the aggregates, entities, and value objects the
`core` context owns, the invariants that must hold at every public API
boundary, and the SRP justification for each aggregate (the single
reason it would change). The set is closed: nothing else is owned by
`core`, per §1 and the refusals in §3.3.

The DDD aggregate boundaries below correspond to the bounded-context
seams discussed in §3.2. Each aggregate is the smallest unit that
preserves an invariant cluster atomically; every public API call enters
and exits with all invariants holding. Failures are signalled through
`std::expected<T, glibre::Error>` per the engine-wide error-model
decision record; the relevant `core::Error` arms are noted alongside
each invariant cluster.

### 4.1 `World` (aggregate root)

**Identity:** one `World` instance per ECS universe. MVP runs exactly
one (multi-world is deferred per §3.3, R-1.1.35–R-1.1.36). Composes:
`Archetype` storage table, `Entity` allocator, `Resource` map (typed
singletons), `ChangeTick` clock, codegen-emitted column descriptors
keyed by `TypeId`, parent/child relationship index, and the ABI seam
to the `TypeRegistry` and `AssetHandle` table.

**Owned entities / value objects:** `Archetype` (entity), `Entity`
(value object — opaque generational ID), `ChangeTick` (value object),
`Resource` slot map (entity collection).

**Invariants (must hold on every public boundary entry/exit):**

1. **Entity-handle validity.** Every `Entity` returned by the public
   API resolves through the generational allocator: its index is in
   range and its generation equals the slot's current generation. A
   stale handle is rejected with `core::Error::EntityStale`. No public
   API ever dereferences a handle whose generation does not match.
2. **Archetype graph acyclic for parent/child relationships.** The
   `ChildOf` relationship between entities forms a forest (each entity
   has at most one parent; cycles are refused with
   `core::Error::HierarchyCycle`). Phase 5 (`transform`, see
   `reviews/decisions/frame-phases.md`) relies on this for
   single-pass `LocalTransform → GlobalTransform` propagation.
3. **`TypeId` registered before storage allocation.** A component type
   may have a column allocated only if its `TypeId` is present in the
   `TypeRegistry` at world-creation time. Adding a type post-init is
   refused with `core::Error::TypeUnregistered` (PHILOSOPHY §6 — no
   runtime registration in shipping builds).
4. **`ChangeTick` monotonicity.** `ChangeTick` is a `u64` advanced
   exclusively at the phase-9 boundary (`present`) and on each mutable
   `Query` access; it never decreases within a process lifetime.
   `Changed<T>` filters compare ticks under this invariant.
5. **Single-writer per chunk.** While a phase is in flight, at most
   one system holds a write reference to any given `(Archetype, Chunk,
   Column)` triple. Enforced by the `Schedule`'s access-set DAG (§4.4).
6. **Resource map type-safety.** A typed singleton `Resource<T>` is
   accessed only through its `TypeId`-keyed slot. Concurrent
   read/write access to one resource within a phase is refused.
7. **Lifecycle hook firing order.** When a component is added, removed,
   or set, the corresponding `OnAdd` / `OnRemove` / `OnSet` observer
   fires exactly once per mutation, in deterministic insertion order,
   before the mutation becomes visible to subsequent queries within
   the same phase. (The lifecycle hook *shape* is core's; multi-term
   query observers are not — see §3.3, R-1.1.31–R-1.1.32.)

**SRP justification — single reason to change:** the rules of the ECS
world's storage and addressing. If archetype-row addressing, generation
counting, or change-tick semantics change, `World` changes. Anything
else (scene hierarchy data model, GPU resource lifetime, networking,
asset resolution) is owned by another context and would not move
`World`.

### 4.2 `Archetype` (entity within `World`)

**Identity:** the unordered set of `TypeId`s defining one storage
table; equal sets share one `Archetype`. Composes a list of `Chunk`s
holding cache-aligned, fixed-size SoA columns (one column per
component type in the set, plus an entity-id column).

**Invariants:**

1. **Component-set immutability.** An `Archetype`'s component set is
   fixed at creation. Adding or removing a component on an entity
   moves the row to a different `Archetype`; the set itself never
   mutates.
2. **Chunk capacity bound.** Every `Chunk` holds at most
   `Chunk::CAPACITY` rows (codegen-emitted constant). Inserting into
   a full chunk allocates a new chunk; rows are never split across
   chunks.
3. **SoA column alignment.** Each column is `alignof(T)`-aligned and
   contiguous. Codegen asserts at build time that the per-type
   `(size, align)` recorded in the `TypeRegistry` matches the column's
   offset arithmetic; mismatch fails the build.
4. **Row density.** When a row is removed, the last row in its chunk
   swaps into the freed slot (swap-remove); chunks contain no holes.
   Iteration order within a chunk is therefore stable only within one
   `Schedule` invocation, not across frames — but iteration order
   across all chunks of an `Archetype` is deterministic by chunk
   insertion order (PHILOSOPHY §7).
5. **Entity-row address validity.** The reverse map `Entity → (Archetype,
   Chunk, Row)` agrees with the forward map at every public boundary;
   swap-removes update both maps atomically within the same critical
   section.

**SRP justification:** the rules of chunked SoA storage layout. If the
chunk size, column-alignment, or swap-remove discipline changes,
`Archetype` changes. Schedule-time read/write authority lives in
`Schedule`, not here.

### 4.3 `Entity` (value object)

**Identity:** an opaque 64-bit handle: 32-bit slot index plus 32-bit
generation. Allocated by `World`'s entity allocator; only stable
cross-frame reference into a `World`'s rows.

**Invariants:**

1. **Generation-counted reuse.** When an entity is despawned its slot's
   generation increments before reuse; any prior `Entity` value for
   that slot now resolves to `core::Error::EntityStale`.
2. **Opacity.** Public APIs treat `Entity` as opaque; no public method
   exposes the (index, generation) split as load-bearing fields.
   Internal codegen may unpack for storage addressing only.
3. **No cross-`World` portability.** An `Entity` from one `World`
   passed to another `World`'s API yields
   `core::Error::EntityForeignWorld` (relevant once multi-world lands
   post-MVP; refusal contract is reserved now to keep MVP code paths
   honest).

**SRP justification:** the encoding rules of the cross-frame entity
reference. Format changes (e.g. moving to a 48-bit index for >4 M
entities) move `Entity`; nothing else does.

### 4.4 `System` / `Schedule` / `Phase` / `FrameLoop` (aggregate)

**Identity:** the engine's deterministic ordering machinery. One
`FrameLoop` per `World`, owning a `Schedule` per phase, each compiled
once into a `CompiledFrame` whose form is a DAG over `System`
nodes keyed by access sets. `Phase` is a numbered enum (1..=9) with
ordering frozen by `reviews/decisions/frame-phases.md`. A `System` is
a function with a declared read/write access set over `(TypeId,
ResourceId)`.

**Invariants:**

1. **Phase numeric order.** Phase N completes before phase N+1 begins
   within one frame. No phase observes writes from a later phase of
   the same frame. Violation is a build-time refusal — phases are not
   re-orderable at runtime — and a runtime debug-build assertion
   `core::Error::FramePhaseMisordered`.
2. **Read/write access sets non-overlapping within a phase.** Within
   one phase's `Schedule`, no two systems running concurrently have
   overlapping write sets, and no system has read access to a
   component another running system writes. The DAG compiler enforces
   this at schedule-build time; mismatch refuses compilation with
   `core::Error::ScheduleAccessConflict`.
3. **Compile-once.** A `Schedule` compiles to a `CompiledFrame` once
   per (set of registered systems × set of registered components);
   re-registration that changes the set invalidates and recompiles.
   The compiled frame is reused across every frame until invalidated.
4. **System purity at the access-set boundary.** A `System` may only
   read/write the components and resources it declared. Declared-set
   violation is undefined behaviour by ABI; debug builds detect via a
   per-thread access-token check and abort.
5. **Deterministic system ordering.** When two systems' access sets
   permit either ordering, the tiebreaker is the lexicographic order
   of their compile-time fully-qualified names (PHILOSOPHY §7 — fixed
   container iteration order).
6. **Phase ownership.** Each phase has exactly one owning context (per
   the frame-phases decision record); only that context may register
   the phase's body-level systems. Other contexts may register
   *systems* that run *inside* a phase but cannot redefine the phase
   itself. `core` owns phases 5 (`transform`) and 8 (`hot-reload`);
   ownership of other phases is delegated to other contexts via the
   plugin loader.
7. **No exclusive systems in MVP.** Per §3.2 collapse #2, run-criteria
   and exclusive-world systems are out of scope; the schedule is a
   pure access-set DAG.

**SRP justification:** the rules of frame-deterministic system
ordering. Changes to how access sets are checked, how phase boundaries
are gated, or how the DAG is compiled move `Schedule` / `FrameLoop`.
Conditional execution belongs to gameplay/scripting plugins; scene
graph order belongs to scene; render queue belongs to render.

### 4.5 `Plugin` / `PluginLoader` / `Manifest` / `AbiHash` (aggregate)

**Identity:** the aggregate that admits a `.dylib` to the engine. The
`PluginLoader` is the core service; each `Plugin` is a discovered
dylib with its `Manifest` (filename, declared dependencies, embedded
`AbiHash`, embedded schema-version table). `AbiHash` is a value
object: a blake3 hash over the concatenated, sorted schema-source
hashes of the middleman dylib (per `reviews/decisions/fory-codegen.md`).

**Invariants:**

1. **ABI-hash equality before register call.** A plugin's compiled-in
   `glibre_types_abi_hash()` must equal the host's
   `glibre_types_abi_hash()` before *any* call into the plugin's
   `glibre_plugin_register(World&, Registry&)` entry point. Mismatch
   refuses load with `core::Error::PluginAbiHashMismatch`; the dylib
   handle is dropped and no register call is issued. (PHILOSOPHY §9.)
2. **Manifest structural validity.** The `Manifest` must declare a
   plugin name, a set of required dependency names, and the embedded
   ABI hash. Missing or malformed fields refuse load with
   `core::Error::PluginManifestInvalid`.
3. **Topological dependency ordering.** Plugins load in topological
   order over the manifest's `requires` graph; cycles refuse load with
   `core::Error::PluginDependencyCycle`. A plugin cannot register
   components, systems, or resources before all its declared
   dependencies have completed `glibre_plugin_register`.
4. **Single registration window.** Plugin `glibre_plugin_register`
   runs exactly once per load, on the loader thread, before the first
   frame in which the plugin's systems may execute. Re-registration is
   refused; the plugin must be unloaded and reloaded through the
   `HotReloadBarrier` (§4.6).
5. **No plugin link to Fory.** Plugins link only `glibre-types.dylib`;
   linking Fory directly is refused at build time by the dependency
   graph (per the codegen decision record). This invariant is
   build-system-enforced, not runtime-checked, but it backs invariant
   #1 above.
6. **Capability brokering refused.** Plugin groups (R-1.6.2) and
   capability advertisement (R-1.6.8) are not part of `Manifest`;
   manifests carrying such fields are rejected as malformed.

**SRP justification:** the rules of admitting a plugin dylib across
the ABI seam. If the ABI-hash algorithm, the manifest schema, or the
load-order discipline change, `PluginLoader` changes. The contents of
what plugins do once loaded is owned by the plugin's context, not core.

### 4.6 `HotReloadBarrier` (aggregate, owned by `core`)

**Identity:** the frame-boundary gate at phase 8 that drains, swaps,
migrates, and resumes plugins. State machine: Idle → Drain → Swap →
Migrate → Resume → Idle. Composes a pending-reload request queue, a
snapshot of in-flight component storages, and the migration dispatch
table populated by `glibre-types.dylib` (per the codegen decision
record).

**Invariants:**

1. **Fires only at frame phase 8.** No reload state transition occurs
   outside phase 8. A reload requested mid-frame is queued and
   processed at the next phase-8 entry. (PHILOSOPHY §8; frame-phases
   decision record.)
2. **Drain completeness.** Before swap, every in-flight `CommandBuffer`
   has applied; every pending observer hook has fired; the world is
   quiescent (no system holds a borrow). Failure to drain within a
   bounded budget refuses the reload with
   `core::Error::HotReloadDrainTimeout` and leaves the prior plugin
   live.
3. **ABI hash recheck before swap.** Even if a plugin previously loaded
   successfully, the candidate replacement dylib's `AbiHash` is
   compared again before swap. Mismatch refuses the swap with
   `core::Error::HotReloadAbiHashMismatch`; previous plugin remains
   loaded.
4. **Migration atomicity.** Component-storage migration runs to
   completion or rolls back to the pre-swap snapshot; partial
   migration state is never observable. Failure refuses the swap with
   `core::Error::SchemaMigrationFailed` (per the error-model decision
   record); previous plugin remains loaded.
5. **No mid-frame command buffers survive a swap.** All `CommandBuffer`s
   produced by the outgoing plugin are applied or discarded before
   swap; none persist into the new plugin's first frame.
6. **Single-position barrier.** There is exactly one barrier per frame,
   at phase 8. No second hot-reload position exists; the schedule
   refuses to register one.
7. **Refusal preserves prior state.** Any refusal case (#2–#5 above)
   returns the world to its pre-barrier state byte-for-byte; the
   refused reload is logged at `warn` level (per the error-model
   decision record's logging rules) and the frame proceeds to phase 9.

**SRP justification:** the rules of frame-boundary plugin swap. If the
drain protocol, the migration runner, or the refusal-rollback
discipline change, `HotReloadBarrier` changes. The actual schema
migration *bodies* are owned by each migrating type's originating
context, not by core.

### 4.7 `AssetHandle` and the asset handle table (value object + entity)

**Identity:** the `AssetHandle` is an opaque generational handle into
the engine-wide asset table; the table itself is an entity owned by
`core`. Resolution semantics (what bytes / GPU resource a handle
denotes) are plugin-defined; the table only stores the handle slot,
its generation, and an opaque payload pointer maintained by the
resolving plugin.

**Invariants:**

1. **Generation-counted reuse.** Identical to `Entity` (§4.3): a freed
   slot's generation increments before reuse; stale handles resolve
   to `core::Error::AssetStale`.
2. **Opacity at the ABI seam.** Plugins receive `AssetHandle` as an
   opaque 64-bit value; only the resolving plugin interprets the
   payload pointer. Other plugins never dereference the payload.
3. **Handle-table singleton per process.** Exactly one asset handle
   table lives per process (not per `World`); handles are valid
   across worlds when multi-world lands. This is a deliberate seam
   different from `Entity`'s per-world scoping (per §3.2 collapse #6).
4. **No I/O at the table.** The table records handles and payloads;
   it never opens files, sockets, or GPU resources. I/O is the
   `platform` context's; resolution to bytes/GPU is the resolving
   plugin's. (PHILOSOPHY §1 — `core` refuses I/O.)

**SRP justification:** the rules of opaque cross-plugin resource
identity. If the handle encoding or generation discipline change,
`AssetHandle` changes. The meaning of the bytes a handle resolves to
is owned by whichever plugin resolves it.

### 4.8 `CommandBuffer` (value object, owned per-system)

**Identity:** a per-system deferred-mutation log. Records intended
component adds/removes/sets and entity spawns/despawns; replayed at a
sync point in deterministic order.

**Invariants:**

1. **Append-only during phase execution.** A `CommandBuffer` is
   append-only while its owning system runs; mutations recorded are
   not visible until the sync point.
2. **Deterministic replay order.** At the sync point all command
   buffers replay in lexicographic order of their owning system's
   fully-qualified name; within one buffer, in insertion order
   (PHILOSOPHY §7).
3. **No reads of own deferred writes.** A system that writes via its
   `CommandBuffer` does not see its own deferred mutations within the
   same phase; they become visible only at the next phase boundary.
4. **Bounded per-frame allocation.** Each `CommandBuffer` allocates
   into a per-frame arena; spillover beyond the arena cap refuses
   further appends with `core::Error::CommandBufferOverflow` (the
   precise budget figure is set by §9 once allocation cells are
   declared).
5. **Sync-point well-definedness.** The sync points are the phase
   boundaries enumerated in the frame-phases decision record; no other
   sync points exist. Reactive-query subscriptions and cross-world
   bridges are deferred (§3.3).

**SRP justification:** the rules of the smallest deterministic
deferred-mutation primitive. If the ordering rule or the
sync-point-set change, `CommandBuffer` changes. Richer event routing
(typed channels, capture/bubble propagation, cross-world bridges) is
explicitly the `events` plugin's, not core (§3.3, R-1.5.* refusals).

### 4.9 `TypeRegistry` (immutable-after-init lookup, owned by `core`)

**Identity:** a lock-free, init-time-immutable map from `TypeId` to a
type descriptor: `{ size, align, drop, layout }`. Populated at static
init from `glibre-types.dylib`'s codegen output.

**Invariants:**

1. **Immutable after init.** No entries may be added, removed, or
   mutated after `World` construction completes. Attempted mutation
   is refused with `core::Error::TypeRegistryClosed`.
2. **`TypeId` uniqueness.** Each registered `TypeId` corresponds to
   exactly one descriptor. Codegen asserts uniqueness at build time;
   collisions fail the build, never reach runtime.
3. **Size/align consistency.** The `(size, align)` recorded for a type
   matches its codegen-emitted column layout in every `Archetype` that
   references it (cross-checked by §4.2 invariant #3).
4. **No reflective fields.** Descriptors carry only `{ size, align,
   drop, layout }`. Path-based property access, `DynamicValue`, and
   the reflect trait are deliberately absent (per §3.2 collapse #4).

**SRP justification:** the rules of cross-ABI type identity and
storage layout descriptors. If the descriptor schema changes,
`TypeRegistry` changes. Reflective surfaces beyond size/align/drop/
layout belong to `editor` and `data`, not core (§3.3).

### 4.10 Aggregate Composition Map

```text
World (root)
├── Archetype[] (entities; chunked SoA storage)
│   └── Chunk[] (value objects; fixed-capacity SoA slabs)
├── Entity allocator (slot-map; generational)
├── Resource map (TypeId → typed singleton)
├── ChangeTick clock (value object)
├── ChildOf relationship index (forest, acyclic)
└── ABI seam to:
    ├── TypeRegistry          (process-wide, immutable-after-init)
    └── AssetHandle table     (process-wide, generational)

FrameLoop (per-World driver)
└── Schedule (per-Phase, compiled to CompiledFrame DAG)
    └── System[] (access-set-typed function nodes)

PluginLoader (process-wide service)
├── Plugin[] (dylib + Manifest + AbiHash)
└── HotReloadBarrier (frame-8 state machine)

CommandBuffer (per-system, per-frame, deferred-mutation log)
```

Aggregates do not nest each other across context seams: the
`PluginLoader` references `World` only through public APIs, never by
reaching into archetype storage. The `HotReloadBarrier` likewise
interacts with `World` only through the public boundary so its
drain/migrate state machine is a peer aggregate, not a sub-component.

### 4.11 Cross-Aggregate Invariants

These hold at the seams between aggregates and are enforced at the
public API boundary:

1. **Entity referenced from a `CommandBuffer` resolves in the post-
   replay world.** If an entity is despawned by buffer A and
   referenced by buffer B in the same sync point, deterministic replay
   order decides; B's reference resolves to
   `core::Error::EntityStale`, never to a misaddressed row.
2. **System access set ⊆ registered components ∪ registered
   resources.** A system referencing an unregistered `TypeId` refuses
   to compile into the `Schedule`.
3. **Hot-reload preserves entity identity for migrated components.**
   Across the §4.6 swap, `Entity` handles remain valid for any
   component type whose schema migration succeeded; handles for types
   whose migration was refused are unaffected (the type was rolled
   back to its prior state).
4. **`AssetHandle` payloads survive plugin reload only when the
   resolving plugin's schema migration succeeds.** A reload that
   refuses migration leaves the payload pointer intact (rollback);
   one that succeeds with a payload-shape change must register a
   migration that runs at phase 8 alongside the component-storage
   migrations.
5. **Hot-reload may not run during phase 8 itself if the migration is
   for a component type owned by a system the loader is about to
   swap.** This is a self-reference refusal: the loader detects the
   case at request time and refuses the reload with
   `core::Error::HotReloadSelfReference`. (Open question: whether
   `core`-owned types can be migrated at all during a reload is
   deferred to §12.)

## 5. Public Interface

The header-only stub below freezes the `core` ABI shape. It compiles
standalone under `clang++ -std=c++23 -fsyntax-only -Wall -Wextra
-Werror`. Every public function returns `glibre::Result<T>` (=
`std::expected<T, glibre::Error>`) per `reviews/decisions/error-model.md`;
no exceptions cross any boundary; runtime reflection is absent
(PHILOSOPHY §6). Aggregate types are forward-declared and exposed only
through opaque references, keeping the seam thin.

The stub names every surface required by §4 and the engine-wide
decision records: `World` (query, spawn/despawn, component get/set,
change-tick read), `Schedule` (system registration with `Phase` plus
read/write component sets), `FrameLoop` (phase entry/exit hooks plus
the fixed-step accumulator), `HotReloadBarrier` (`request_reload`,
`step` driven from `Phase::HotReload`), `PluginLoader` (`load`,
`unload`, `list` over manifest paths), `AssetHandle<T>` (opaque ID
template), and `CommandBuffer` (record-then-flush deferred mutations).
Typed error tags from `reviews/decisions/plugin-abi.md` and
`reviews/decisions/hot-reload-protocol.md` (`core::Error::HotReload`,
`core::Error::Plugin{...}` family, `core::Error::SchemaMigrationFailed`)
are listed as the `core::Error` enum members so per-context error
mapping is grounded.

```cpp
// specs/core/SPEC.md §5 — Public Interface (header-only stub).
// C++23. Compiles standalone with `clang++ -std=c++23 -fsyntax-only`.
// Authoritative ABI lives in glibre-core; this stub freezes the public shape.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string_view>
#include <variant>

namespace glibre {

// Forward-declared so this header stays free of context cycles. Full
// definitions live in glibre/error.hpp (per error-model.md decision
// record) and the per-context error enums.
class Error;

template <class T>
using Result = std::expected<T, Error>;

namespace core {

// ---------------------------------------------------------------------------
// 5.1 Error arms emitted by `core` (see §10 for full enumeration / recovery).
// Listed here because every signature below names them by tag.
// ---------------------------------------------------------------------------
enum class Error : std::uint16_t {
    // Entity / world invariants (§4.1, §4.3).
    EntityStale,
    EntityForeignWorld,
    HierarchyCycle,
    TypeUnregistered,
    TypeRegistryClosed,

    // Schedule / frame ordering (§4.4).
    ScheduleAccessConflict,
    SystemScheduleCycle,
    FramePhaseMisordered,

    // Asset table (§4.7).
    AssetStale,

    // Command buffer (§4.8).
    CommandBufferOverflow,

    // Plugin loader (plugin-abi.md §"Failure Modes → core::Error").
    PluginDlopenFailed,
    PluginMissingEntryPoint,
    PluginManifestInvalid,
    PluginAbiHashMismatch,
    PluginEngineTooOld,
    PluginNameCollision,
    PluginDependencyMissing,
    PluginDependencyCycle,
    PluginInitFailed,

    // Hot-reload barrier (hot-reload-protocol.md §"Refusal Cases").
    HotReload,                // Umbrella tag — "a hot-reload was refused".
    HotReloadDrainTimeout,
    HotReloadAbiHashMismatch,
    HotReloadSelfReference,
    SchemaMigrationFailed,
};

// ---------------------------------------------------------------------------
// 5.2 ID value objects.
//
// `Entity` and `AssetHandle` are opaque 64-bit generational handles
// (§4.3, §4.7). The (index, generation) split is private; public
// callers compare and pass them by value. `TypeId` and `SystemId` are
// stable codegen-emitted IDs from `glibre-types.dylib`.
// ---------------------------------------------------------------------------
struct Entity {
    std::uint64_t bits{};
    friend constexpr bool operator==(Entity, Entity) noexcept = default;
};

template <class T>
struct AssetHandle {
    std::uint64_t bits{};
    friend constexpr bool operator==(AssetHandle, AssetHandle) noexcept = default;
};

struct TypeId {
    std::uint64_t value{};
    friend constexpr bool operator==(TypeId, TypeId) noexcept = default;
};

struct SystemId {
    std::uint64_t value{};
    friend constexpr bool operator==(SystemId, SystemId) noexcept = default;
};

struct ChangeTick {
    std::uint64_t value{};
    friend constexpr bool operator==(ChangeTick, ChangeTick) noexcept = default;
    friend constexpr auto operator<=>(ChangeTick, ChangeTick) noexcept = default;
};

// ---------------------------------------------------------------------------
// 5.3 Frame phases (frame-phases.md). Numeric order is load-bearing;
// any later phase observes only writes from earlier phases of the same
// frame. The enum is an exhaustive 1..=9 list — `Custom(N)` insertions
// are out of scope (frame-phases.md §Notes).
// ---------------------------------------------------------------------------
enum class Phase : std::uint8_t {
    Input         = 1,  // platform
    Logic         = 2,  // gameplay/scripting (deferred body in MVP)
    PhysicsFixed  = 3,  // physics
    Animation     = 4,  // animation (deferred body in MVP)
    Transform     = 5,  // core (LocalTransform → GlobalTransform)
    CullExtract   = 6,  // render
    RenderSubmit  = 7,  // render
    HotReload     = 8,  // core (drain → swap → migrate → resume)
    Present       = 9,  // platform
};

// ---------------------------------------------------------------------------
// 5.4 Aggregate forward declarations. Implementations are private; the
// public API exposes references to these opaque types so the ABI seam
// stays thin (PHILOSOPHY §6 — no runtime reflection in shipping).
// ---------------------------------------------------------------------------
class World;
class Schedule;
class FrameLoop;
class HotReloadBarrier;
class PluginLoader;
class CommandBuffer;
class TypeRegistry;
class Query;          // Compiled archetype-set descriptor.
class QueryDesc;      // Builder input — Reads / Writes / Withouts / Changed.
class SystemContext;  // Per-system view supplied to `SystemFn`.

// A `SystemFn` is a free-standing function pointer; capturing closures
// are out of scope so plugin systems remain ABI-stable across reloads.
using SystemFn = void (*)(SystemContext& ctx) noexcept;

// ---------------------------------------------------------------------------
// 5.5 World — query, spawn/despawn, get/set component, change-tick read.
// (Aggregate root §4.1.) All mutating operations validate Entity
// generation and TypeRegistry membership; failures return one of the
// `core::Error` arms above. No exceptions cross this boundary.
// ---------------------------------------------------------------------------
class World {
public:
    // Lifecycle ------------------------------------------------------------
    static Result<World*> create() noexcept;
    static void           destroy(World* w) noexcept;

    World(const World&)            = delete;
    World& operator=(const World&) = delete;

    // Entities -------------------------------------------------------------
    [[nodiscard]] Result<Entity> spawn() noexcept;
    [[nodiscard]] Result<void>   despawn(Entity e) noexcept;
    [[nodiscard]] bool           is_alive(Entity e) const noexcept;

    // Components — typed surface lives in plugin-side helpers; the ABI
    // boundary is the type-erased pair below, keyed by codegen TypeId.
    [[nodiscard]] Result<void>
    set_component(Entity e, TypeId t, std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] Result<std::span<const std::byte>>
    get_component(Entity e, TypeId t) const noexcept;

    [[nodiscard]] Result<void>
    remove_component(Entity e, TypeId t) noexcept;

    // Change ticks ---------------------------------------------------------
    [[nodiscard]] ChangeTick current_tick() const noexcept;
    [[nodiscard]] Result<ChangeTick>
    last_change_tick(Entity e, TypeId t) const noexcept;

    // Queries — descriptor compiles once, query handle is opaque.
    [[nodiscard]] Result<Query*> compile_query(const QueryDesc& desc) noexcept;

    // Type registry access (immutable-after-init; §4.9).
    [[nodiscard]] const TypeRegistry& type_registry() const noexcept;

protected:
    World() noexcept;
    ~World();
};

// ---------------------------------------------------------------------------
// 5.6 Schedule — system registration with Phase + read/write component
// sets (§4.4). The DAG is compiled once into a CompiledFrame; further
// registrations invalidate and recompile.
// ---------------------------------------------------------------------------
struct AccessSet {
    std::span<const TypeId> reads{};
    std::span<const TypeId> writes{};
    std::span<const TypeId> without{};
};

struct SystemDesc {
    std::string_view name{};       // Fully-qualified, used as deterministic tiebreaker.
    Phase            phase{};
    AccessSet        access{};
    SystemFn         body{nullptr};
    std::span<const std::string_view> after{};
    std::span<const std::string_view> before{};
};

class Schedule {
public:
    [[nodiscard]] Result<SystemId> register_system(const SystemDesc& desc) noexcept;
    [[nodiscard]] Result<void>     unregister_system(SystemId id) noexcept;

    // Forces DAG (re)compilation; idempotent if no registrations changed.
    [[nodiscard]] Result<void>     compile() noexcept;

protected:
    Schedule() noexcept;
    ~Schedule();
};

// ---------------------------------------------------------------------------
// 5.7 FrameLoop — phase entry/exit hooks, accumulator (§4.4).
// One driver per World; phase ordering is the Phase enum above.
// ---------------------------------------------------------------------------
using PhaseHookFn = void (*)(World& world, Phase phase) noexcept;

struct PhaseHooks {
    PhaseHookFn on_enter{nullptr};
    PhaseHookFn on_exit{nullptr};
};

class FrameLoop {
public:
    [[nodiscard]] static Result<FrameLoop*>
    create(World& world, Schedule& schedule) noexcept;
    static void destroy(FrameLoop* loop) noexcept;

    // Advances the fixed-step accumulator by `delta_seconds`; runs as
    // many frames as the accumulator releases (frame-phases.md
    // §"One-frame pipeline preserved").
    [[nodiscard]] Result<void> tick(double delta_seconds) noexcept;

    // Per-phase observer hooks. Hooks fire on the loop thread between
    // the schedule's phase-N exit and phase-(N+1) entry; they MUST NOT
    // mutate world state — that is the schedule's job.
    [[nodiscard]] Result<void> set_phase_hooks(Phase phase, PhaseHooks hooks) noexcept;

    // Fixed-step accumulator state (read-only).
    [[nodiscard]] double      accumulator() const noexcept;
    [[nodiscard]] std::uint64_t frame_index() const noexcept;

protected:
    FrameLoop() noexcept;
    ~FrameLoop();
};

// ---------------------------------------------------------------------------
// 5.8 HotReloadBarrier — request_reload(plugin_path), step at frame 8.
// (hot-reload-protocol.md.) Requests are queued; the barrier executes
// at exactly the next Phase::HotReload boundary (PHILOSOPHY §8).
// ---------------------------------------------------------------------------
struct ReloadRequestId {
    std::uint64_t value{};
    friend constexpr bool operator==(ReloadRequestId, ReloadRequestId) noexcept = default;
};

enum class ReloadOutcome : std::uint8_t {
    Pending,
    Completed,
    Refused,
};

struct ReloadStatus {
    ReloadOutcome outcome{ReloadOutcome::Pending};
    Error         cause{Error::HotReload};   // Meaningful only when Refused.
};

class HotReloadBarrier {
public:
    // Enqueue a reload of the plugin currently registered under
    // `plugin_fqn` to point at `replacement_dylib_path`. Idempotent
    // within one frame: re-requesting the same plugin coalesces.
    [[nodiscard]] Result<ReloadRequestId>
    request_reload(std::string_view             plugin_fqn,
                   const std::filesystem::path& replacement_dylib_path) noexcept;

    // Drives the four-step state machine (drain → swap → migrate →
    // resume) for every queued request. Called once per frame from
    // the FrameLoop at Phase::HotReload entry. Returns the count of
    // requests processed (0 when the queue is empty — the no-op path).
    [[nodiscard]] Result<std::size_t> step() noexcept;

    // Polls a previously-queued request without blocking.
    [[nodiscard]] ReloadStatus status(ReloadRequestId id) const noexcept;

protected:
    HotReloadBarrier() noexcept;
    ~HotReloadBarrier();
};

// ---------------------------------------------------------------------------
// 5.9 PluginLoader — load(manifest_path), unload, list (§4.5,
// plugin-abi.md). Owns the dlopen/dlsym/manifest-parse/hash-check/
// register sequence. Hot-reload requests route through the barrier.
// ---------------------------------------------------------------------------
struct PluginId {
    std::uint64_t value{};
    friend constexpr bool operator==(PluginId, PluginId) noexcept = default;
};

struct LoadedPlugin {
    PluginId          id{};
    std::string_view  name{};        // From PluginManifest.name.
    std::string_view  abi_hash{};    // 64-char blake3 hex.
    std::string_view  dylib_path{};  // Borrowed from the loader's storage.
};

class PluginLoader {
public:
    [[nodiscard]] static Result<PluginLoader*>
    create(World& world, HotReloadBarrier& barrier) noexcept;
    static void destroy(PluginLoader* loader) noexcept;

    // Validates the manifest, gates on ABI hash, and registers the
    // plugin's components / systems / passes / panels. The plugin's
    // `glibre_plugin_register` runs synchronously on the calling
    // thread; mid-frame calls are queued and run at Phase::HotReload.
    [[nodiscard]] Result<PluginId>
    load(const std::filesystem::path& manifest_path) noexcept;

    [[nodiscard]] Result<void> unload(PluginId id) noexcept;

    // Snapshot of currently-loaded plugins (stable for the duration of
    // the call; the slice is invalidated by the next load/unload).
    [[nodiscard]] std::span<const LoadedPlugin> list() const noexcept;

protected:
    PluginLoader() noexcept;
    ~PluginLoader();
};

// ---------------------------------------------------------------------------
// 5.10 CommandBuffer — apply on flush (§4.8). One per system; recorded
// during the system body, replayed in deterministic order at the next
// sync point. Failure modes are arena overflow + entity staleness.
// ---------------------------------------------------------------------------
class CommandBuffer {
public:
    [[nodiscard]] Result<Entity> spawn() noexcept;
    [[nodiscard]] Result<void>   despawn(Entity e) noexcept;

    [[nodiscard]] Result<void>
    add_component(Entity e, TypeId t, std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] Result<void>
    remove_component(Entity e, TypeId t) noexcept;

    [[nodiscard]] Result<void>
    set_component(Entity e, TypeId t, std::span<const std::byte> bytes) noexcept;

    // Flushes recorded mutations into `world` in deterministic order.
    // Called by the schedule at sync points; plugins do not invoke
    // `flush` directly. Re-runnable (no-op when the buffer is empty).
    [[nodiscard]] Result<void> flush(World& world) noexcept;

    // Resets the per-frame arena without applying. Used by the
    // hot-reload barrier when a refusal discards in-flight work.
    void clear() noexcept;

    [[nodiscard]] std::size_t recorded_count() const noexcept;

protected:
    CommandBuffer() noexcept;
    ~CommandBuffer();
};

// ---------------------------------------------------------------------------
// 5.11 Per-system view passed to SystemFn bodies. Carries scoped
// references to the world, the system's CommandBuffer, and the
// current ChangeTick. Borrowed-only — the pointer is invalidated
// after the system body returns.
// ---------------------------------------------------------------------------
class SystemContext {
public:
    [[nodiscard]] World&         world() noexcept;
    [[nodiscard]] const World&   world() const noexcept;
    [[nodiscard]] CommandBuffer& commands() noexcept;
    [[nodiscard]] ChangeTick     tick() const noexcept;
    [[nodiscard]] Phase          phase() const noexcept;

protected:
    SystemContext() noexcept;
    ~SystemContext();
};

// ---------------------------------------------------------------------------
// 5.12 Hot-reload observer events (hot-reload-protocol.md
// §"Observer Notification"). The bus is owned by HotReloadBarrier;
// subscribers are called synchronously between barrier steps 4.2
// and 4.3, so they always see a fully-swapped world.
// ---------------------------------------------------------------------------
struct HotReloadStartedEvent {
    std::string_view plugin_fqn{};
    std::string_view old_dylib_path{};
    std::string_view new_dylib_path{};
};

struct HotReloadCompletedEvent {
    std::string_view             plugin_fqn{};
    std::string_view             old_abi_hash{};
    std::string_view             new_abi_hash{};
    std::span<const std::string_view> migrated_types{};
};

struct HotReloadRefusedEvent {
    std::string_view plugin_fqn{};
    Error            cause{Error::HotReload};
};

}  // namespace core
}  // namespace glibre
```

Event types: `HotReloadStartedEvent`, `HotReloadCompletedEvent`,
`HotReloadRefusedEvent` — emitted synchronously by `HotReloadBarrier`
between steps 4.2 and 4.3 (`reviews/decisions/hot-reload-protocol.md`
§"Observer Notification"). Subscribers always see a fully-swapped,
fully-migrated world.

Serialized schemas (Fory): `core` itself does not own any schemas; the
plugin manifest schema (`PluginManifest`, `SemVer`, `ComponentDecl`,
`SystemDecl`, `PassDecl`, `PanelDecl`) lives under
`reviews/decisions/plugin-abi.md` §"Plugin Manifest Schema" and is
materialized by the `glibre-foryc` codegen pipeline; `core` consumes
the deserialized POD only at load time.

Error types: `glibre::Error` (rolled-up tagged union per
`reviews/decisions/error-model.md`) carrying `glibre::core::Error` as
one of its arms; the listed enumerators above are §10's authority and
the targets of every refusal site in the loader, schedule, and
barrier.

## 6. Internal Architecture

Non-binding sketch for implementers. The §5 public interface and the
§4 aggregates are authoritative; this section records the module
layout, the data structures, and the algorithmic shape we currently
expect to ship — together with the load-bearing decisions taken in
`reviews/decisions/{frame-phases,error-model,plugin-abi,
hot-reload-protocol,fory-codegen,perf-budget}.md`. Implementers may
deviate from the sketches below provided every §5 signature, §4
invariant, §9 budget cell, and §10 error arm is preserved.

### 6.1 Module Layout

The `core` library compiles to a single `glibre-core` static archive
whose internals are partitioned into seven sub-modules. Each sub-module
owns one aggregate from §4 and exposes only the §5 facade headers; the
internal headers live under `core/src/<sub>/` and are not on the
include path of plugins or other contexts.

| Sub-module        | Owns aggregates (§4)                  | §5 surface           | Sketches in 6.x |
|-------------------|---------------------------------------|----------------------|-----------------|
| `world/`          | `World`, `Entity`, archetype storage  | `World`, `CommandBuffer` | §6.2, §6.3 |
| `schedule/`       | `Schedule`, `SystemId`, access sets   | `Schedule`           | §6.4 |
| `frame/`          | `FrameLoop`, the nine phases          | `FrameLoop`, `Phase` | §6.5 |
| `plugin/`         | `Plugin`, `PluginLoader`              | `PluginLoader`       | §6.6 |
| `hot-reload/`     | `HotReloadBarrier`, migration arena   | `HotReloadBarrier`   | §6.7 |
| `asset/`          | `AssetHandle<T>` table                | `AssetHandle<T>`     | §6.8 |
| `type-registry/`  | `TypeRegistry`, `TypeId`              | (§5 header internals)| §6.9 |

SRP rule (PHILOSOPHY #1): a sub-module has one reason to change — its
owned aggregate's invariants. Any change that touches two sub-modules'
internals at once is a seam-violation flag and must be reviewed against
§4's invariant ownership column. The `core/src/` tree is forbidden
from cross-sub-module includes except through the §5 facades; the
build enforces this with per-sub-module visibility rules in CMake.

The codegen-emitted middleman (`glibre-types.dylib`,
`reviews/decisions/fory-codegen.md`) is a sibling library, not a
sub-module of `core`. `core` links it the same way every plugin does;
the only privileged consumer of `glibre-types.dylib`'s `_registry.cpp`
internals is `type-registry/`.

### 6.2 `world/` — Codegen-Driven Archetype Storage

ECS storage is hand-written-shape C++ emitted by `glibre-foryc`, **not**
a third-party ECS library (PHILOSOPHY #6 forbids runtime reflection in
shipping; entt and friends are out). The pipeline:

1. Every component declared in any plugin's `plugin.fory` (per
   `reviews/decisions/plugin-abi.md` §"Plugin Manifest Schema",
   `ComponentDecl.fqn` + `schema_hash` + `storage_hint`) lands in
   `glibre-types.dylib`'s registry alongside its POD layout.
2. `glibre-foryc` additionally emits an *archetype shape table* keyed
   by the sorted set of `TypeId`s present in an archetype. The shape
   table is built lazily at first-spawn into a new combination, but
   the *storage column code* (per-component `Column<T>` with `T`'s
   alignment + size hard-coded) is fully emitted at codegen time and
   resolved through a single function-pointer indirection per
   component-access.
3. `World` stores entities in fixed-size **chunks** (target 16 KiB per
   `reviews/decisions/perf-budget.md` heap accounting). Each chunk
   holds a parallel array per component column (SoA), padded to the
   component's `alignof`. Iteration walks chunks in registration order
   (PHILOSOPHY #7 fixed iteration order).

Sketch (illustrative; final shapes emit from `glibre-foryc`):

```cpp
// core/src/world/archetype.hpp — internal, not on include path.

namespace glibre::core::detail {

struct ArchetypeKey {
    std::span<const TypeId> sorted_type_ids;  // canonical ascending.
    friend constexpr bool operator==(ArchetypeKey, ArchetypeKey) noexcept;
};

class Chunk {
public:
    static constexpr std::size_t kBytes = 16 * 1024;
    std::byte storage[kBytes];                 // SoA columns laid out by emit.
    std::uint32_t row_count;
    std::uint32_t row_capacity;                // depends on row stride.
    ChangeTick last_modified[/*per-column*/];  // codegen sizes this array.
};

class Archetype {
public:
    ArchetypeKey                          key;
    std::pmr::vector<std::unique_ptr<Chunk>> chunks;   // PerContextAllocator-tagged.
    std::pmr::vector<ColumnDescriptor>    columns;     // emitted from registry.
    std::pmr::vector<Entity>              row_to_entity;
};

// Spawn / despawn / move-between-archetypes implemented in archetype.cpp;
// public API exposed through World facade in §5.

}  // namespace glibre::core::detail
```

Move-between-archetype paths (component add/remove) are bounded by the
chunk size and codegen-emitted memcpy stencils; no per-row virtual
dispatch.

### 6.3 `world/` — `CommandBuffer` Arena

Each system body receives a `CommandBuffer&` — a per-system, per-frame
arena (`PerContextAllocator` transient pool, drained at phase 9) that
captures spawn/despawn/insert/remove intents as 32-byte `Command`
records. The `Schedule` flushes a system's `CommandBuffer` only after
the system's owning phase exits; this guarantees every system body
observes a consistent `World` snapshot for its phase (§4.4 invariant).

### 6.4 `schedule/` — DAG From Declared Access Sets

Plugins declare each system's access set in `plugin.fory`
(`SystemDecl.reads`, `.writes`, `.after`, `.before`, `.phase`; per
plugin-abi §"Plugin Manifest Schema"). The schedule build
(triggered at process start and at each successful hot-reload) is
purely data-driven over the union of all loaded plugins' system
declarations — no system code runs during the build.

Algorithm (per-phase, runs nine times):

1. Collect every `SystemDecl` whose `phase` matches the current phase
   number (1..9).
2. Build a directed graph `G` on system names. Add an edge `A → B`
   whenever:
   - `A.writes ∩ B.reads ≠ ∅`, **or**
   - `A.writes ∩ B.writes ≠ ∅`, **or**
   - `A.name ∈ B.after`, **or**
   - `B.name ∈ A.before`.
3. Topologically sort `G`. A cycle yields
   `core::Error::SystemScheduleCycle` (§5 enum, §10 row); the loader
   rolls back the offending plugin's registration per
   `reviews/decisions/plugin-abi.md` §"Loader Sequence" step 10.
4. Cache the sorted order as the phase's `CompiledPhase` — a flat
   `std::pmr::vector<SystemId>` walked by `FrameLoop`.

Complexity: `O(systems²)` worst-case for the access-set intersection
sweep (acceptable; system count is bounded by plugin count × ~tens, and
the rebuild runs only at hot-reload, not per frame). Per-frame phase
execution is `O(systems)` and dispatches each system through a
codegen-emitted thunk that resolves component columns by `TypeId`
without runtime lookup.

Single-worker MVP: every system runs on the game-loop driver thread in
sorted order. The `(reads, writes)` machinery is already collected so
post-MVP per-system parallelism (a thread pool fanning out independent
nodes per phase, fence at the phase exit) drops in without re-spec'ing
the schedule shape. The codegen step that emits per-system thunks is
the natural seam to also emit a parallel-dispatch wrapper later.

### 6.5 `frame/` — The Nine-Phase Loop

`FrameLoop` owns the strict numeric ordering locked by
`reviews/decisions/frame-phases.md`. The loop body is a hard-coded
`switch` over `Phase` 1..9; phase identity is not data-driven because
PHILOSOPHY #1 (one reason to change) gives each phase one owning
context, and adding a phase is a frame-phases amendment, not a
configuration change.

Sketch:

```cpp
// core/src/frame/frame_loop.cpp — internal.

void FrameLoop::tick(World& w, Schedule& s) noexcept {
    run_phase(Phase::Input,         w, s);  // 1: platform
    run_phase(Phase::Logic,         w, s);  // 2: deferred (empty in MVP)
    run_phase(Phase::PhysicsFixed,  w, s);  // 3: physics
    run_phase(Phase::Animation,     w, s);  // 4: deferred (empty in MVP)
    run_phase(Phase::Transform,     w, s);  // 5: core
    run_phase(Phase::CullExtract,   w, s);  // 6: render
    run_phase(Phase::RenderSubmit,  w, s);  // 7: render
    barrier_.step(w, plugins_);             // 8: HotReloadBarrier (core)
    run_phase(Phase::Present,       w, s);  // 9: platform
    w.advance_change_tick();
}
```

The `HotReloadBarrier` is **inserted between phases 7 and 8 as a
distinct call** rather than threaded through `run_phase` — its body is
not a normal system schedule (no `(reads, writes)`-driven dispatch;
it mutates the loader registries directly), so the §6.4 DAG machinery
does not apply. Frame-phases §"Phase Table" line 8 already names this
shape.

Critical-path perf (per `perf-budget.md`):

- Phase ordering hot loop has a **~0.5 ms ceiling** for `core`'s share
  (0.40 ms sim + 0.05 ms submit per `perf-budget.md` Per-Context
  Budget Table, plus a handful of cycles for the `switch` itself).
- The `switch` dispatches through a function-pointer table populated
  at process start; no virtual calls, no `std::variant<>` visiting.

### 6.6 `plugin/` — `dlopen` + Manifest Read + ABI Hash Gate

`PluginLoader` is the sole module permitted to call `dlopen` /
`dlsym` / `dlclose`. The implementation is a literal transcription of
`reviews/decisions/plugin-abi.md` §"Loader Sequence":

1. **`dlopen`** the candidate `.dylib` with `RTLD_NOW | RTLD_LOCAL`.
   Failure → `core::Error::PluginDlopenFailed`.
2. **`dlsym`** the four required entry points
   (`glibre_plugin_abi_hash`, `glibre_plugin_manifest`,
   `glibre_plugin_manifest_size`, `glibre_plugin_register`). Missing
   symbol → `core::Error::PluginMissingEntryPoint`.
3. **Manifest read**: deserialize the in-`.rodata` Fory blob via
   `glibre::types::deserialize<PluginManifest>(...)`. Failure →
   `core::Error::PluginManifestInvalid`.
4. **ABI hash gate**: byte-compare `manifest.abi_hash` and the
   redundant `glibre_plugin_abi_hash` symbol against the host's
   `glibre_types_abi_hash()`. Either mismatch →
   `core::Error::PluginAbiHashMismatch`. PHILOSOPHY #9.
5. Engine-version, name-collision, dependency, and cycle checks
   (steps 5–7 of plugin-abi loader sequence) → matching
   `core::Error::PluginEngineTooOld / PluginNameCollision /
   PluginDependencyMissing / PluginDependencyCycle` arms.
6. **Register entry-point invocation**:
   `glibre_plugin_register(PluginContext&)` runs with the `World`,
   the four registries, and the deserialized manifest passed by
   reference (plugin-abi §"Registration Entry-Point Signature").
   The registries record per-plugin ownership so a refused load can
   be cleanly rolled back without leaking partial state.

Internal data structures:

```cpp
// core/src/plugin/loader.hpp — internal.

struct LoadedPlugin {
    std::string             fqn;               // PluginManifest.name
    std::filesystem::path   dylib_path;
    void*                   dl_handle;
    PluginManifest          manifest;          // deserialized once at load.
    std::array<void*, 4>    entry_points;      // dlsym'd at load.
    std::pmr::vector<TypeId>    owned_types;
    std::pmr::vector<SystemId>  owned_systems;
    // ... mirrors PassDecl / PanelDecl ownership for rollback.
};

class PluginLoader {
public:
    // §5 facade: load(), unload(), list().
private:
    std::pmr::vector<LoadedPlugin> loaded_;     // registration order.
    TypeRegistry&                  types_;
    SystemRegistry&                systems_;
    PassRegistry&                  passes_;
    PanelRegistry&                 panels_;
};
```

The registries (`TypeRegistry`, `SystemRegistry`, `PassRegistry`,
`PanelRegistry`) live in `type-registry/` and `schedule/` respectively;
the loader holds references obtained at construction. Cross-plugin
direct symbol use is forbidden (plugin-abi §"Decision" rule 2);
plugins communicate exclusively through middleman-typed components and
registered systems.

### 6.7 `hot-reload/` — Drain → Swap → Migrate → Resume

`HotReloadBarrier::step` is a literal transcription of
`reviews/decisions/hot-reload-protocol.md` §"Protocol Sequence".
Per-plugin state machine, executed by the loader on the game-loop
thread:

```cpp
// core/src/hot-reload/barrier.cpp — internal.

void HotReloadBarrier::step(World& w, PluginLoader& loader) noexcept {
    if (pending_.load(std::memory_order_relaxed) == 0) return;  // hot path.

    for (auto& req : pending_requests_) {
        const auto txn = begin_txn(req);
        if (!drain(req, w, txn))   { rollback(txn); continue; }
        if (!swap(req, loader, txn)) { rollback(txn); continue; }
        if (!migrate(req, w, txn))   { rollback(txn); continue; }
        if (!resume(req, loader, txn)) { rollback(txn); continue; }
        publish(HotReloadCompleted{req.fqn, /*...*/});
    }
    pending_requests_.clear();
    pending_.store(0, std::memory_order_relaxed);
}
```

State that survives a swap (hot-reload-protocol §"State Survival
Rules"): every byte that has a `.fory` schema. ECS archetype storages
of middleman-typed components, world singletons, asset-handle table,
plugin/type registries, frame counter, world tick, PRNG state. State
that does not survive: GPU resource handles, plugin-private caches,
worker-thread pools — re-derived in `glibre_plugin_register`.

Migrate functions (per `reviews/decisions/fory-codegen.md`
§"Migration Mechanic" + hot-reload-protocol §"Migrate Function
Contract") are pure free functions invoked from `glibre-types.dylib`'s
static migration table. The barrier supplies a per-phase migration
arena (16 MiB ceiling, accounted under `core`'s 64 MiB heap per
`perf-budget.md` Allocator Rules rule 6); the arena resets between
rows and never grows across plugins in the same phase.

Refusal cases (exactly three, per hot-reload-protocol §"Refusal
Cases"): ABI hash mismatch (`PluginAbiHashMismatch`), schema
migration failure (`SchemaMigrationFailed`), plugin init returns
error (`PluginInitFailed`). All three roll up under
`core::Error::HotReload` (umbrella) and leave the previous-good
plugin instance live and linked.

The barrier is a true no-op when no reload is pending: a single
relaxed atomic load on `pending_`, no fence, no cache flush — well
inside the `<0.1 ms` steady-state budget (`perf-budget.md`
"Pipelined Frame Timing" line for phase 8).

### 6.8 `asset/` — Generation-Tagged Opaque Slots

`AssetHandle<T>` (§5 stub) is an opaque `std::uint64_t` packing
`(index: 40, generation: 22, type_tag: 2)`. The internal table:

```cpp
// core/src/asset/table.hpp — internal.

template <class T>
struct Slot {
    std::uint32_t generation;        // bumped on each release.
    bool          live;
    T             payload;           // typed asset payload.
};

template <class T>
class AssetTable {
public:
    AssetHandle<T> insert(T&& payload) noexcept;
    Result<T*>     resolve(AssetHandle<T>) noexcept;     // AssetStale on miss.
    void           release(AssetHandle<T>) noexcept;
private:
    std::pmr::vector<Slot<T>> slots_;
    std::pmr::vector<std::uint32_t> free_indices_;
};
```

Generation-tagged dereference rules (§4.7 invariant): any handle whose
`generation` mismatches its slot's current generation resolves to
`core::Error::AssetStale` (§5 enum). Slots are never compacted in MVP;
the index is stable for the slot's lifetime, the generation rolls over
on `release` to invalidate every outstanding handle to that slot.

The handle table itself is a middleman-typed singleton (per the
hot-reload survival rule "if it has a `.fory` schema, it survives");
the *payloads* may or may not survive depending on whether the asset
type is middleman-declared. Plugin-private payloads are released at
drain and re-acquired at register, identical to GPU resource handles.

### 6.9 `type-registry/` — `TypeId` Lookup

`TypeRegistry` answers `TypeId → ColumnDescriptor` queries (size,
alignment, codegen-emitted move/destroy thunks) used by `world/` for
archetype storage and by `schedule/` for access-set intersection.
The registry is append-only within a process session: hot-reload may
add `TypeId` entries (a new plugin registers a new type) but never
remove or repurpose an existing entry — the ABI hash gate refuses
loads that would conflict, so the *rules* hold by construction.

The registry's contents are populated entirely from
`glibre-types.dylib`'s `_registry.cpp` (codegen-emitted) plus
per-plugin registration calls during `glibre_plugin_register`. No
reflection, no string lookup on the hot path: `TypeId` is a stable
codegen-emitted integer, equal byte-for-byte across every plugin
linked against the same middleman.

### 6.10 Concurrency

MVP runs every system on a single worker thread (the game-loop driver
thread). Justification:

- **Determinism**: PHILOSOPHY #7 forbids platform intrinsics in
  simulation. A single thread is the smallest model that trivially
  satisfies byte-equal world-snapshot replay across hosts.
- **Schedule data is already there**: §6.4 collects `(reads, writes)`
  per system; per-system parallel dispatch (a fork-join thread pool
  fanning out independent DAG nodes per phase, fence at phase exit)
  is a pure additive change over the same `CompiledPhase` shape.
- **Codegen as the seam**: the per-system thunk that today calls one
  function will tomorrow be a parallel dispatch wrapper emitted by
  the same `glibre-foryc` step that emits the thunk. No SPEC §5
  signature changes.

Hot-reload is single-threaded by contract (hot-reload-protocol
§"Decision": loader runs on the game-loop thread; observer
notifications are synchronous on that same thread). The `World`
exclusive write lock during phase 8 is documentation, not contention,
in MVP — the loader is the only writer because no system runs in
phase 8.

Memory ordering: every public API in §5 is `noexcept` and assumes
single-threaded access except where marked otherwise. The pending
hot-reload counter (§6.7) is the one `std::atomic` in core that is
read on the hot path. Every other shared-state read happens between
phases (i.e., on barriers the schedule already enforces).

### 6.11 Critical Paths

The two paths that pay our perf budget:

1. **Phase ordering hot loop (~0.5 ms / frame budget for `core`)** —
   `FrameLoop::tick` plus `core`-owned phases 5 and 8. The
   `run_phase` switch dispatches through a function-pointer table;
   the `Phase::Transform` body is a tight SIMD walk over packed
   `LocalTransform` columns; `Phase::HotReload` is a single relaxed
   atomic load when no reload is pending. Steady-state target is
   ~0.45 ms, leaving 0.05 ms for `switch` overhead and counter
   bumps. Drift here trips the `perf-budget.yml` p99 gate.
2. **Schedule (re)build — `O(systems²)`, runs only at hot-reload** —
   the access-set intersection sweep is the dominant cost and
   bounded by total registered system count (estimated ~tens × plugin
   count). The build must complete within phase 8's reload-frame
   budget (≤0.40 ms one-shot per `perf-budget.md`); a system count
   that approaches this ceiling is the `task-breakdown` trigger to
   move to a per-`(reads,writes)`-set bucketing build (`O(systems)`
   amortized). Listed as an open question in §12.

Allocator-tag rule (per `perf-budget.md` Allocator Rules): every
allocation in `core/src/` carries the `core` `ContextTag`; the build
flag `-Wglibre-no-raw-alloc` rejects raw `new` / `malloc`. The 64
MiB heap ceiling covers archetype tables, schedule DAG storage,
loader bookkeeping, and the migration arena's 16 MiB sub-budget.

### 6.12 Open Implementation Questions

Tracked in §12 alongside SPEC-level open questions; listed here
because each is internal-architecture-shaped:

1. Should `Chunk` size be a per-archetype tunable (heuristic on
   per-row stride) instead of a fixed 16 KiB? Defer until a real
   archetype distribution exists.
2. Per-system parallelism's seam: does the `glibre-foryc` thunk emit
   a single fork-join wrapper, or does the schedule own a separate
   `ParallelCompiledPhase`? Decide when the parallelism plan opens.
3. Does the `AssetTable` ever need compaction? MVP says no (slot
   indices are stable for the process lifetime); reconsider when
   asset count reaches `O(2^20)`.
4. Should the `HotReloadBarrier`'s migration arena grow on demand
   inside one phase, or refuse the reload when the 16 MiB ceiling
   is hit? Hot-reload-protocol §"Open Questions" #1 already lists
   this; cross-referenced here so the implementation plan owns it.

## 7. Persistence & Schemas

The `core` context persists exactly the state required to (a) admit a
plugin across the ABI seam and (b) carry process-wide bookkeeping
across the frame-8 hot-reload barrier. Everything else core touches —
`World` storage (entities, archetypes, chunks, columns), `Resource`
slots, `ChangeTick` clocks, `Schedule` DAGs, `CompiledFrame`s,
`CommandBuffer` arenas, the `TypeRegistry` lookup, the `AssetHandle`
table — is **runtime-only**: rebuilt from plugin registration on every
process start, never written to bytes core owns. The persistence
surface below is the closed set of types core itself authors as
`.fory` schemas; component bytes are owned by the plugins that declare
the components (per §1 and §3.3, R-1.4.* refusals routed to `data`).

Every schema below lives under `data/schemas/core/<Type>.fory`,
authored to the format defined in `reviews/decisions/fory-codegen.md`
§"Schema File Format" and consumed by `glibre-foryc` exactly as any
other persistent type. The `data` context owns the codegen pipeline,
the registry, the migration dispatcher, the `glibre_types_abi_hash`
gate value, and the byte-level round-trip guarantee (`specs/data/SPEC.md`
§4.10); `core` only authors the `.fory` files and writes the matching
migration bodies as pure free functions.

### 7.1 Schemas Core Owns

The core context authors the following persistent types. Each is
materialized into `glibre-types.dylib` by `glibre-foryc` and exposed
as a generated POD at `glibre/types/core/<Type>.hpp`. The C++ projection
of each lives already in §5 (see `glibre::types::PluginManifest` and
the `HotReload*Event` triplet); the schemas here are the authoritative
wire form, the C++ structs are derived.

| Schema file | FQN | Aggregate (§4) | Purpose |
|---|---|---|---|
| `data/schemas/core/PluginManifest.fory` | `glibre.core.PluginManifest` | `Plugin / PluginLoader` (§4.5) | Per-plugin manifest blob baked into each plugin's `.rodata` and re-read by the loader at step 3 of the load sequence. |
| `data/schemas/core/SemVer.fory` | `glibre.core.SemVer` | `Plugin` (§4.5) | Three-`u16` SemVer triple. Consumed by `PluginManifest.version` and `PluginManifest.min_engine_version`. |
| `data/schemas/core/ComponentDecl.fory` | `glibre.core.ComponentDecl` | `Plugin` (§4.5) | Per-component declaration inside a manifest: `(fqn, schema_hash, storage_hint)`. |
| `data/schemas/core/SystemDecl.fory` | `glibre.core.SystemDecl` | `Plugin` (§4.5) | Per-system declaration: `(name, phase, reads, writes, after, before)`. |
| `data/schemas/core/PassDecl.fory` | `glibre.core.PassDecl` | `Plugin` (§4.5) | Per-render-pass declaration: `(name, render_phase, inputs, outputs)`. |
| `data/schemas/core/PanelDecl.fory` | `glibre.core.PanelDecl` | `Plugin` (§4.5) | Per-editor-panel declaration: `(id, title, area)`. |
| `data/schemas/core/HotReloadCheckpoint.fory` | `glibre.core.HotReloadCheckpoint` | `HotReloadBarrier` (§4.6) | Carry-over state captured at barrier step `Drain → Swap`, consumed by per-component `migrate(...)` hooks at step `Migrate`, and either committed at `Resume` or rolled back on refusal. |
| `data/schemas/core/LoadedPluginRecord.fory` | `glibre.core.LoadedPluginRecord` | `PluginLoader` (§4.5) | Loader-side bookkeeping: which plugin dylib is currently mapped at which path, with the manifest hash captured at load time. Persisted across process boundaries only for diagnostic replay (e.g. crash reports embedding the prior load set); not consulted for runtime semantics. |

The first six rows are the `PluginManifest` family already required by
`reviews/decisions/plugin-abi.md` §"Plugin Manifest Schema". The seventh
and eighth are introduced by this section; each is justified by an
aggregate invariant in §4 and rationalized below in §7.2.

`PluginManifest.fory` (canonical form, reproduced from
`reviews/decisions/plugin-abi.md` §"Plugin Manifest Schema" so the spec
is self-contained):

```fory
schema glibre.core.PluginManifest {
  version 1
  since   "0.1.0"

  field name              : string             tag 1 since 1
  field version           : SemVer             tag 2 since 1
  field abi_hash          : string             tag 3 since 1
  field min_engine_version: SemVer             tag 4 since 1
  field components        : list<ComponentDecl> tag 5 since 1
  field systems           : list<SystemDecl>    tag 6 since 1
  field passes            : list<PassDecl>      tag 7 since 1
  field panels            : list<PanelDecl>     tag 8 since 1
  field depends_on        : list<string>        tag 9 since 1
}
```

`HotReloadCheckpoint.fory` (new):

```fory
schema glibre.core.HotReloadCheckpoint {
  version 1
  since   "0.1.0"

  field plugin_fqn        : string             tag 1 since 1
  field old_abi_hash      : string             tag 2 since 1
  field new_abi_hash      : string             tag 3 since 1
  field frame_index       : u64                tag 4 since 1
  field current_tick      : u64                tag 5 since 1
  field migrated_types    : list<string>       tag 6 since 1
  field carryover_payload : bytes              tag 7 since 1
}
```

The `carryover_payload` is an opaque, plugin-shaped blob the outgoing
plugin emits at `Drain` and the incoming plugin consumes at `Migrate`.
Core does not interpret it; it is the per-component migration arena's
serialized snapshot, dispatched by the data context's
`MigrationChain`. The remaining fields are pure metadata: what plugin
swapped, what hashes were involved, which frame the swap landed in,
and which type FQNs participated — sufficient to reconstruct the swap
in a crash-report replay without exposing component bytes.

`LoadedPluginRecord.fory` (new):

```fory
schema glibre.core.LoadedPluginRecord {
  version 1
  since   "0.1.0"

  field plugin_fqn        : string             tag 1 since 1
  field manifest_version  : SemVer             tag 2 since 1
  field abi_hash          : string             tag 3 since 1
  field dylib_path        : string             tag 4 since 1
  field load_frame_index  : u64                tag 5 since 1
  field depends_on        : list<string>       tag 6 since 1
}
```

`LoadedPluginRecord` is the persisted projection of one entry in the
`PluginLoader::list()` snapshot (§5.9). It is written exclusively by
the loader, never by plugins, and its lifetime is process-bound for
runtime semantics — persistence exists only so crash dumps and editor
session-restore can name what was loaded when something went wrong.
Core does not reload from `LoadedPluginRecord` at startup; `dylib_path`
is informational, not a re-load instruction. (Re-load is always driven
by configuration in `tools` / config layer per §3.3.)

### 7.2 What Core Does Not Persist

This list is closed and load-bearing; pull requests adding any of the
following to a core-owned `.fory` file should be rejected.

1. **Component data.** Every component type is declared by the plugin
   that owns it; its `.fory` schema lives under
   `data/schemas/<owning-plugin>/`, not `core/`. Core only routes the
   bytes through `World::set_component` / `get_component` (§5.5);
   the bytes never become a core-authored schema.
2. **`World` snapshots.** A "world snapshot" — the union of every
   archetype's chunk bytes plus the entity allocator's slot vector —
   is itself a serialization concern owned by the future `data` /
   `content` context's snapshot format, composed *out of* per-component
   schemas, not authored by core (per §3.3, R-1.4.* refusals).
3. **`Schedule` DAGs.** A compiled `CompiledFrame` is a pure function
   of the registered `(SystemDesc, ComponentDecl)` set; recomputed at
   load time and on every `Schedule::compile()` (§5.6 invariant 3).
   Persisting a compiled DAG would be a cache, not a contract.
4. **`TypeRegistry` contents.** The registry is populated at static
   init from `glibre-types.dylib`'s codegen output; plugins extend it
   through `glibre_plugin_register`. The registry is never written
   back; the `.fory` files plus the middleman binary are the only
   sources of truth (§4.9 invariant 1).
5. **`AssetHandle` payloads.** The asset handle table records opaque
   payload pointers maintained by the resolving plugin (§4.7
   invariant 2). The byte layout of what an `AssetHandle` denotes is
   the resolving plugin's schema concern, never core's.
6. **`ChangeTick` history.** `ChangeTick` is a `u64` advanced at every
   frame and every mutable access; it is process-local and meaningless
   across runs. Snapshots that need to carry change-tracking
   information embed their own per-payload tick (a domain decision),
   never core's.
7. **`CommandBuffer` arenas.** Per-system, per-frame, ephemeral
   (§4.8 invariant 4). Never persisted.
8. **`PluginContext` shape.** The struct passed to
   `glibre_plugin_register` (`reviews/decisions/plugin-abi.md`
   §"Registration Entry-Point Signature") is engine-internal C++;
   plugins read it once and let it die at the call boundary. It is
   not a persistent type and has no `.fory` schema.

### 7.3 Migration Rules

Core obeys the full migration discipline defined in
`reviews/decisions/fory-codegen.md` §"Migration Mechanic" and
`specs/data/SPEC.md` §4.6 / §4.7. The rules below are the
core-specific instances of that contract; nothing here weakens or
contradicts the data context's invariants.

**Function signature.** For every `(FQN, N → N+1)` pair across the
schemas in §7.1, the core context provides one pure migration body:

```cpp
// Lives under glibre/core/migrations/<Type>_v<N>_to_v<N+1>.cpp.
// Registered via the codegen-emitted GLIBRE_REGISTER_MIGRATION macro
// (glibre-types.dylib §4.6) at static-init time inside the core TU
// that links glibre-types.

namespace glibre::core::migrations {

[[nodiscard]] auto
migrate_PluginManifest_v1_to_v2(
    const glibre::types::PluginManifestV1& src,
    glibre::types::PluginManifestV2&       dst,
    glibre::types::Arena&                  arena) noexcept
    -> std::expected<void, glibre::Error>;

[[nodiscard]] auto
migrate_HotReloadCheckpoint_v1_to_v2(
    const glibre::types::HotReloadCheckpointV1& src,
    glibre::types::HotReloadCheckpointV2&       dst,
    glibre::types::Arena&                       arena) noexcept
    -> std::expected<void, glibre::Error>;

// ... one per (FQN, N → N+1) pair core owns.

}  // namespace glibre::core::migrations
```

The signature is fixed by `data` (`specs/data/SPEC.md` §5
`MigrationFn<VN, VNplus1>`). Core authors only the body. The macro
expansion that wires the body into the dispatcher's per-type chain is
emitted by `glibre-foryc` into the matching
`glibre/types/core/<Type>_migrations.hpp` companion header.

**Per-rule constraints** (each maps to one §4.6 invariant):

1. **Pure.** No reads of wall clock, RNG state, environment, locale,
   or filesystem (§4.6 inv. 1). Migration bodies are mechanically
   verifiable as pure by inspection — they only touch their `src` and
   `dst` parameters and the supplied `Arena&`.
2. **Arena-only allocation.** Every byte the migration writes lives in
   the `Arena&` passed in; no global heap calls, no STL containers
   that default-construct allocators, no I/O (§4.6 inv. 2).
3. **Total over the prior version.** The function returns `unexpected`
   only for *defective* payloads (e.g. a `ComponentDecl.schema_hash`
   that fails Blake3 form), never for domain-valid `VN` instances
   (§4.6 inv. 3). Logical translation between versions is always
   defined.
4. **Local.** Migration bodies do not consult the `SchemaRegistry`,
   do not call into `World`, and do not read or mutate global state
   (§4.6 inv. 4). They are plain in/out functions over their two POD
   arguments.
5. **Single-step.** Core never authors a `vN → vN+2` migration. Multi-
   version translation composes through `MigrationChain` (§4.7);
   per-step coverage is verified at codegen time and the build fails on
   a missing step (§4.7 inv. 1).

**Additive-only ABI rule.** Per `fory-codegen.md` §"ABI Stability
Rules" #1 and #3 plus `specs/data/SPEC.md` §4.2 inv. 3, every change
to a core-owned schema must be a *layout-additive append*. New fields
are introduced as new `tag` numbers strictly greater than every
previously-shipped tag in the schema, with `since == new_version`,
and either an `option<T>` type or a declared `default` so older
payloads materialize the new field deterministically. Concretely for
the schemas in §7.1:

- **`PluginManifest`**: extending the manifest with a new declaration
  list (e.g. a future `routes : list<RouteDecl>`) is permitted as a
  new tag (10, 11, …) at the next version; the migration body
  default-constructs an empty list. Reordering existing fields is
  forbidden — `glibre-foryc` rejects a `.fory` change that violates
  tag-sort layout (§4.2 inv. 3).
- **`SemVer`** is closed at three `u16` fields. Any change is a major
  break and would require a new `glibre.core.SemVerV2` schema rather
  than an in-place revision; the migration would be a
  `SemVerV1 → SemVerV2` shape change owned by core.
- **`ComponentDecl` / `SystemDecl` / `PassDecl` / `PanelDecl`**:
  additive only. New fields append at the next free tag with a
  default; new optional fields default to `option<T>::none()`.
- **`HotReloadCheckpoint`**: additive only. The `carryover_payload`
  is intentionally typed as `bytes` so per-plugin migration shape
  changes do not require a checkpoint schema bump — only a checkpoint
  *envelope* shape change (e.g. adding a new metadata tag like
  `wall_clock_ns`) bumps the checkpoint version.
- **`LoadedPluginRecord`**: additive only. New diagnostic fields
  (e.g. `plugin_build_id : option<string>`) append at fresh tags.

**Reserved-tag enforcement.** Removed fields convert their tag
number into `reserved` immediately; reuse is rejected by `glibre-foryc`
at codegen time (`specs/data/SPEC.md` §4.1 inv. 3, §4.2 inv. 4). Core
ships with zero reserved tags at v1 of every schema in §7.1.

**ABI hash effect.** Every additive bump of a schema in §7.1 triggers
a recomputation of `glibre_types_abi_hash` in `glibre-types.dylib`
(`reviews/decisions/plugin-abi.md` §"Versioning Rules" #1). Plugins
compiled against the prior hash will refuse to load at the next
process start with `core::Error::PluginAbiHashMismatch` — the
single, intended consequence. SONAME of `glibre-types.dylib` does
**not** bump on additive-only schema changes; SONAME bumps only on
layout-breaking changes, which the additive-only rule above forbids
for core-owned schemas (§"Versioning Rules" #2).

**Round-trip test obligation.** Per `fory-codegen.md` §"Consequences",
every persistent schema ships with at least one Catch2 round-trip
golden under `tests/data/schemas/core/<Type>.cpp`. Core's tests cover
each schema in §7.1 at v1 (round-trip), and starting at v2 of any
schema, an additional golden replays a v(N-1) payload through the
`MigrationChain` and asserts the resulting `VN` is byte-equal to the
hand-authored expected value. Tests are referenced by name from §11's
acceptance-criteria mapping.

### 7.4 Note: ECS World State Is Plugin-Owned

This restates §7.2 #1 emphatically because it is the single largest
refusal in this section: a `World` (§4.1) is the storage shape for
*plugin-declared* component bytes. Core never authors the schema for
the bytes living in any archetype column. Each plugin's components
have their own `.fory` files under `data/schemas/<plugin>/`, written
by the plugin author, hashed into `glibre_types_abi_hash`, and
migrated by the plugin author's own pure free functions. The core
loader runs `deserialize<T>` against the snapshot at
`HotReloadBarrier`'s migrate step (§4.6, §5.8); the migration *body*
for any component type belongs to the originating plugin's context,
not to core (PHILOSOPHY §3, §6; `fory-codegen.md` §"Migrations owned
by the originating context").

The smallest concrete consequence: a `plugins/render/` directory
authoring a `Mesh` component owns
`data/schemas/render/Mesh.fory` and `glibre::render::migrate_Mesh_v1_to_v2`.
`core` knows the FQN and the schema source hash through
`ComponentDecl.schema_hash` (§7.1) but never reads or writes the
component's bytes itself.

## 8. Hot-Reload Contract

The `core` context **is the orchestrator** of hot-reload. The
`HotReloadBarrier` aggregate (§4.6) and its supporting types
(`ReloadRequestId`, `ReloadOutcome`, `ReloadStatus`, the
`HotReload*Event` triplet — §5.8, §5.12) are the entire engine-side
surface that drives the four-step state machine **drain → swap →
migrate → resume** at frame phase 8 (`Phase::HotReload`, §5.3). No
other context schedules a swap; no other context owns the barrier.
This section is the load-bearing distillation of
`reviews/decisions/hot-reload-protocol.md` mapped onto core's
aggregate, public-interface, and persistence surface; everything below
is binding for an implementer of `HotReloadBarrier::step` (§5.8) and
referenced verbatim by §11's acceptance criteria.

The barrier is a true no-op when no reload is pending: `step` performs
exactly one relaxed atomic load against the pending-reload queue and
returns `Result<std::size_t>{0}` without taking the world lock or
publishing any event. Phase 8's idle budget (§9, frame-phases decision
record §Consequences ≤0.1 ms) is preserved by construction.

When `step` observes a non-empty queue it processes each pending
reload as an **independent transaction**: per-plugin atomicity, not
phase-wide. One plugin's refusal does not block another plugin's swap
in the same phase 8 tick. The four steps for a single transaction
are detailed in §8.3–§8.6; cross-cutting state-survival, migrate-body,
refusal, and rollback rules are §8.1–§8.2 and §8.7–§8.9.

### 8.1 What Survives a Swap

Surviving state is exactly the state owned by `core` or by
`glibre-types.dylib`. The invariant is mechanical: **a value
survives the swap iff its type has a `.fory` schema** (and therefore
appears in `glibre-types.dylib`'s registry). No per-plugin survival
manifest, no per-component opt-in flag, no second source of truth.

Concretely the loader guarantees the bytes of the following are
unchanged across the four steps (or migrated in place per §8.5):

1. **`PluginManifest` registry** — the loader's per-plugin record
   (`LoadedPlugin` snapshot, §5.9 `PluginLoader::list()`; persisted
   projection `LoadedPluginRecord`, §7.1) is owned by `core` and
   amended in place. The outgoing plugin's `PluginId` is **stable**:
   the registry slot is updated with the replacement dylib path and
   ABI hash, not freed and reissued. Re-loads do not rotate IDs;
   handles cached by editor UI / e2e harness remain valid.
2. **`World` handles** — `Entity` value objects (§4.3) keep their
   `(index, generation)` bits unchanged. No archetype is rebuilt on
   account of a plugin reload; the `World` aggregate's slot map and
   chunk storage (§4.1, §4.2) are byte-stable across the swap. A
   handle valid at phase 7 of frame N is valid at phase 1 of frame
   N+1 with no re-resolution. (§4.11 invariant 3.)
3. **`Schedule` structure** — the per-`Phase` topological order is
   re-derived after step 4 from the union of every loaded plugin's
   `(reads, writes, after, before)` declarations (§5.6
   invariant 3, §4.4 invariant 2). System *function pointers* re-
   resolve through the new plugin's vtable; the schedule's *shape*
   (phases 1..=9, system identities by FQN, ordering edges) is
   recomputed deterministically and is byte-equal to a fresh-process
   compile against the same loaded set.
4. **`AssetHandle` table** — the per-process handle slots (§4.7) keep
   their `(index, generation)` bits. The opaque payload pointer for
   any handle resolved by the *outgoing* plugin is invalidated at
   step 1 (drain) and re-resolved by the *incoming* plugin at step 4
   (resume); the handle bits do not change. Cross-plugin code holding
   an `AssetHandle<T>` continues to address the same logical asset.
5. **`TypeRegistry`** — the immutable-after-init descriptor map
   (§4.9) is append-only across a session. A reload extends the
   registry only with types that pass schema-hash equality against
   the surviving entries; replacing a type's layout is a refusal
   (§8.7 row "schema_hash drift"), not a registry mutation.
6. **`ChangeTick` clock and frame counter** — the `World`'s
   monotonic `ChangeTick` (§4.1, §5.5) and `FrameLoop::frame_index()`
   (§5.7) are unchanged across the swap. Phase 8 does not advance
   either; phase 9 (`Present`) advances them as it would on any
   frame.

State that **does not survive** — and which the incoming plugin
re-derives in `glibre_plugin_register` at step 4 — covers everything
without a `.fory` schema: GPU resource handles internal to the
plugin, plugin-private caches, JIT-derived dispatch tables, plugin-
owned worker pools (drained and joined at step 1), and any plugin-
private singleton not declared via a middleman type. Resolution of
an `AssetHandle` returned by the outgoing plugin's resolver may
re-look-up against the incoming plugin's resource table; the handle
identity is stable, the cached payload pointer is not.

### 8.2 The `migrate(...)` Contract

For every persistent component / singleton type T owned by the
incoming plugin Q whose `schema_version` exceeds the version
recorded in the surviving storage's header (§4.2 invariant 3, §7.1
`ComponentDecl.schema_hash`), the loader invokes the **per-type
migration chain** registered with the data context's `MigrationChain`
dispatcher (per `reviews/decisions/fory-codegen.md` §"Migration
Mechanic" and `specs/data/SPEC.md` §4.6 / §4.7).

Each chain step is a pure free function with a fixed signature
already constrained by core's own §7.3 contract — restated here as
the binding contract for **every plugin author** (the originating
context owns the body; core owns the call site):

```cpp
// Authored by the plugin that owns the component type T.
// Lives under <plugin>/migrations/<Type>_v<N>_to_v<N+1>.cpp.
// Registered at static-init via the codegen-emitted
// GLIBRE_REGISTER_MIGRATION macro inside the plugin TU that links
// glibre-types.dylib.

namespace glibre::<plugin_ctx>::migrations {

[[nodiscard]] auto
migrate_<Type>_v<N>_to_v<N+1>(
    const glibre::types::<plugin_ctx>::<Type>V<N>& src,
    glibre::types::<plugin_ctx>::<Type>V<N+1>&     dst,
    glibre::types::Arena&                          arena) noexcept
    -> std::expected<void, glibre::Error>;

}  // namespace glibre::<plugin_ctx>::migrations
```

The loader supplies the three arguments (`src`, `dst`, `arena`) and
**nothing else**: no `World&`, no `Registry&`, no `LogSink&`, no
clock. This is what makes the function relocatable into the
data context's static migration table and what makes its purity
mechanically verifiable.

Per-row execution order at step 3 (Migrate, §8.5):

1. The loader queries `MigrationChain` for
   `(stored_version → current_version)` of T. A missing chain →
   `core::Error::SchemaMigrationFailed`, abort, rollback (§8.8
   #4).
2. For each row in the surviving storage, the loader allocates
   `sizeof(T_current)` from a per-phase migration arena (a
   loader-owned `glibre::types::Arena`; the budget is set in §9, the
   arena is reset between rows, never grown across plugins in the
   same phase) and invokes the chain step-by-step:
   `migrate_T_vN_to_vNplus1(src, dst, arena)`,
   `migrate_T_vNplus1_to_vNplus2(src, dst, arena)`, ...
3. On success of the **full chain for that row**, the resulting
   bytes overwrite the storage row in place (size is bounded by the
   new struct's compile-time `sizeof`, per `fory-codegen.md` §"ABI
   Stability"). On any chain-step `unexpected`, the partially-
   migrated bytes living in the arena are discarded (the arena is
   reset, never published) and the loader rolls back the entire
   transaction (§8.8).
4. After all rows of all types Q owns have migrated successfully,
   the storage header for each migrated type is bumped to the new
   `schema_version`. The header bump is the commit point of step 3.

The contract obligations on the migrate body itself are the §7.3
list extended uniformly to every plugin: **pure**, **deterministic**,
**arena-only allocation**, **total over the prior version's domain**
(returning `unexpected` is reserved for *defective* payloads, not
"I don't know how to migrate this value"), **single-step** (chain
composition is the dispatcher's job, never the body's), **local**
(no global reads, no `World` access, no logger). The rules and
their motivation are restated identically to §7.3 #1–#5; §8.2 adds
no new constraints, only widens the scope from core-owned schemas
to plugin-owned components.

`HotReloadCheckpoint` (§7.1, `data/schemas/core/HotReloadCheckpoint.fory`)
captures the cross-step metadata for the in-flight transaction: the
plugin FQN, the old/new ABI hashes, the frame index, the world tick,
the list of migrated type FQNs, and the opaque `carryover_payload`
bytes the outgoing plugin emitted at drain. The checkpoint is the
sole vehicle that crosses the four steps; nothing else of plugin
origin is in scope at the migrate boundary.

### 8.3 Step 1 — Drain

Trigger: `pending_reloads > 0` observed by `HotReloadBarrier::step`
(§5.8) at `Phase::HotReload` entry. For each pending reload of plugin
P (the outgoing plugin):

1. The loader takes the `World`'s exclusive write lock (already held
   from the perspective of any system, since no system runs in phase
   8; this is documentation, not contention).
2. The loader invokes the outgoing plugin's `glibre_plugin_drain`
   (an entry point added by `reviews/decisions/hot-reload-protocol.md`
   §"Step 1 — Drain"; ABI shape `extern "C" std::expected<void,
   glibre::Error> glibre_plugin_drain(World&) noexcept`). The plugin
   must:
   - Flush any internal per-frame queues into ECS components owned
     by middleman types (so the bytes survive into step 3).
   - Emit any `carryover_payload` it wishes to hand to its
     replacement, written into the loader-supplied arena and
     attached to the in-flight `HotReloadCheckpoint` envelope
     (§7.1, §8.2).
   - Release any GPU resource handles it owns; the incoming plugin
     re-acquires equivalents at step 4.
   - Cancel any worker tasks it spawned and join them.
3. **Drain completeness budget.** If `glibre_plugin_drain` does not
   return within the loader's drain budget (concrete value set by
   §9), the loader refuses the reload with
   `core::Error::HotReloadDrainTimeout` (§4.6 invariant 2,
   §5.1 enum) and skips steps 2–4 for *this* plugin. Other
   pending plugins in the same phase 8 are processed independently.

Postcondition of step 1: every byte of plugin-survivable state
lives in middleman-typed ECS storage or in the
`HotReloadCheckpoint` carry-over arena; no thread other than the
loader is inside the outgoing plugin's code; every
`CommandBuffer` recorded by the outgoing plugin has either flushed
or been `clear()`-ed (§5.10).

### 8.4 Step 2 — Swap

Per outgoing plugin P, incoming candidate dylib Q (already
`dlopen`-ed and manifest-validated by `PluginLoader::load` per
`reviews/decisions/plugin-abi.md` §"Loader Sequence" steps 1–7):

1. **ABI hash recheck** (§4.6 invariant 3). Verify
   `Q::glibre_types_abi_hash() == host_glibre_types_abi_hash` and
   that the manifest's `abi_hash` agrees. Mismatch refuses with
   `core::Error::HotReloadAbiHashMismatch` (§5.1) wrapped under
   `core::Error::HotReload`. Q's mapped image is `dlclose`-ed; P
   is undisturbed. This is the second hash check (the first ran at
   `PluginLoader::load`); the recheck catches the case where Q's
   on-disk image was rewritten between load and barrier.
2. **Schema-coverage check.** Q's `PluginManifest.components`
   (§7.1) must be a superset of P's surviving component-storage
   types. A *subset* — meaning Q dropped a type P registered —
   is a major-version change requiring a fresh world, refused with
   `core::Error::HotReload` wrapping
   `core::Error::PluginManifestInvalid`. A `schema_hash` *drift* on
   an existing FQN is refused with the same umbrella wrapping
   `core::Error::PluginAbiHashMismatch`.
3. **Vtable swap.** The loader atomically replaces P's vtable
   pointer in the `PluginRegistry` with Q's. The vtable is one
   pointer indirection from every system call site; the swap is
   one relaxed store guarded by the loader's exclusive ownership
   of phase 8. After this point, every system call site dispatches
   to Q.
4. **Type-registry append.** Q's component type registrations are
   appended to the `TypeRegistry` (§4.9) for any *new* type FQNs Q
   declares; pre-existing FQNs are validated via
   `ComponentDecl.schema_hash` equality (§7.1) — a hash drift on
   an existing FQN is the refusal at step 2.2 above, not a silent
   re-registration. The registry remains immutable-after-init in
   the sense that the *closed* set of types reachable to any
   already-running system is unchanged across the swap; new types
   are only visible to systems Q registers at step 4 (resume).

`PluginId` stability (§8.1 #1) is enforced here: the registry slot
is mutated, not freed.

### 8.5 Step 3 — Migrate

Run the §8.2 chain over every persistent component / singleton
type T that bumped versions between P and Q. The migrations execute
on the loader thread, sequentially, in `TypeRegistry` registration
order (a stable order since the registry is append-only). Failure
of any chain step on any row triggers full rollback (§8.8 #4); the
migration arena is reset before the cause is returned to the caller
of `HotReloadBarrier::step`.

The `HotReloadCheckpoint` envelope (§7.1) is finalized at the end
of step 3: `migrated_types` is populated with the FQNs whose
storage headers were bumped, and the checkpoint is the payload
attached to every observer event published in §8.6.

### 8.6 Step 4 — Resume and Observer Notification

1. The loader invokes `Q::glibre_plugin_register(PluginContext&
   ctx)` (§5.9, `reviews/decisions/plugin-abi.md` §"Registration
   Entry-Point Signature"). The plugin must:
   - Re-acquire any GPU resources it released at step 1.
   - Register its systems into the `Schedule` (§4.4, §5.6).
     Re-registration of an already-known `(phase, system_fqn)` is
     idempotent.
   - Register its render-graph passes / editor panels.
   - Rebuild caches keyed off middleman state.
2. **Schedule rebuild.** After `glibre_plugin_register` returns, the
   loader re-derives the per-`Phase` topological order over the
   union of every loaded plugin's `(reads, writes, after, before)`
   set. A cycle refuses the swap with
   `core::Error::SystemScheduleCycle` (§4.4 invariant 4, §5.1) and
   triggers full rollback (§8.8 #5).
3. **Observer notification (success path).** The loader publishes
   `HotReloadCompletedEvent { plugin_fqn, old_abi_hash, new_abi_hash,
   migrated_types }` (§5.12) **synchronously** on the loader thread,
   between the schedule rebuild's commit point and the next pending
   reload's step 1. Subscribers always observe a fully-swapped,
   fully-migrated world; they never observe a half-state. Subscribers
   that cache plugin-derived state (the editor's panel registry, the
   e2e harness's golden-snapshot recorder, any plugin that listens
   for cross-plugin reloads) **invalidate those caches off this
   event**; the caches are re-derivable from middleman state by
   construction (§8.1).
4. The pending-reload counter is decremented. When it reaches zero,
   `HotReloadBarrier::step` returns `Result<std::size_t>{N}` (the
   count of processed requests) and `Phase::HotReload` exits;
   `Phase::Present` (phase 9) begins.

`HotReloadStartedEvent` is published at phase 8 entry, **once per
pending reload**, before that transaction's step 1 begins (§5.12,
`reviews/decisions/hot-reload-protocol.md` §"Observer Notification").
Subscribers that need to invalidate caches do so off this event;
they are guaranteed by step 4 atomicity that the next event they
see for the same `plugin_fqn` will be either `HotReloadCompletedEvent`
or `HotReloadRefusedEvent` (§5.12), never an interleaving of the
two and never a half-state.

The observer surface is itself a middleman type
(`glibre.core.HotReloadEvent` family, projected as the §5.12
structs); its layout survives any future core-runtime reload by
construction.

### 8.7 Refusal Cases (mapping to `core::Error`)

Every refusal site in the four-step state machine maps to **exactly
one** `core::Error` arm wrapped under the umbrella
`core::Error::HotReload` (§4.6 invariant 7, §5.1). Consumers see
both the umbrella ("a hot-reload was refused") and the specific
cause via `glibre::Error`'s detail payload (per the error-model
decision record). The closed table below is binding; adding a
fourth refusal class requires a PHILOSOPHY amendment and a new
enumerator.

| Step | Detection point                                            | Inner cause arm                                | Operator action                                                |
|------|------------------------------------------------------------|------------------------------------------------|----------------------------------------------------------------|
| 1    | `glibre_plugin_drain` does not return within drain budget  | `core::Error::HotReloadDrainTimeout`           | Investigate the outgoing plugin's drain implementation.        |
| 1    | `glibre_plugin_drain` returns `unexpected(...)`             | `core::Error::PluginInitFailed` (drain arm)    | Read the plugin's reported inner error.                        |
| 2.1  | ABI hash mismatch (recheck)                                 | `core::Error::HotReloadAbiHashMismatch`        | Rebuild the candidate plugin against current `glibre-types.dylib`. |
| 2.2  | Q drops a component FQN P registered                        | `core::Error::PluginManifestInvalid`           | Treat as major version; restart with a fresh world.            |
| 2.2  | Q changes layout of an existing FQN (`schema_hash` drift)   | `core::Error::PluginAbiHashMismatch`           | Bump the schema's `version` and ship a migration body.         |
| 3    | Missing chain `(stored → current)` for some type            | `core::Error::SchemaMigrationFailed`           | Author the missing migration step or restore from a snapshot.  |
| 3    | A migrate-chain step returns `unexpected(...)`              | `core::Error::SchemaMigrationFailed`           | Fix the migration body or the originating defective payload.   |
| 4.1  | `glibre_plugin_register` returns `unexpected(...)`          | `core::Error::PluginInitFailed` (register arm) | Read the plugin's reported inner error.                        |
| 4.2  | Schedule cycle in the rebuilt DAG                           | `core::Error::SystemScheduleCycle`             | Resolve the cyclic `(after, before)` declarations.             |
| any  | Self-reload of `core` requested                             | `core::Error::HotReloadSelfReference`          | Restart the process; core self-reload is out of MVP scope.     |

Each refusal logs **exactly once** at `warn` level via
`glibre::log_error` (per the error-model decision record's logging
rule for hot-reload refusals), with the structured fields
`plugin_fqn`, `attempted_dylib_path`, `host_abi_hash`,
`plugin_abi_hash`, and the inner cause arm's enumerator name.
Refusals do **not** escalate to `error` — the engine continues on
the previous-good plugin, which is always a tolerable steady state.

### 8.8 Failure & Rollback (atomic, all-or-nothing)

The atomicity guarantee is **per-plugin transaction**: rollback of
plugin P↔Q is all-or-nothing; concurrent transactions in the same
phase 8 are independent. Old plugin P is kept live on **any**
failure step; the new plugin Q's mapped image is `dlclose`-ed and
its `PluginRegistry` slot is reverted to P's. The per-step
discipline:

1. **Step 1 (Drain) failure.** No state changed. Mark the request
   refused, log per §8.7, continue to the next pending reload.
2. **Step 2.1 / 2.2 failure** (hash recheck or coverage check).
   Detected before any vtable mutation. `dlclose` Q, mark refused,
   continue.
3. **Step 2.3 / 2.4 failure.** By construction these cannot fault
   (a single relaxed atomic store and a registry append under
   exclusive ownership). If observed, the loader **terminates the
   process** — this is a contract violation, not a recoverable
   error, and silent recovery would mask a bug the
   deterministic-snapshot contract (§4.1, PHILOSOPHY §7) cannot
   tolerate.
4. **Step 3 (Migrate) failure.** Migrated bytes live exclusively in
   the per-phase migration arena until the row's chain commits in
   step 3.3; on any chain-step `unexpected`, the arena is reset
   before any in-place storage overwrite, so **at most one row is
   half-overwritten at any moment** and the loader's exclusive
   phase-8 lock guarantees no observer sees the intermediate.
   Rollback unwinds the step 2.4 type-registry append (Q's new
   types are removed), restores P's vtable pointer (§8.4 step 2.3),
   and re-invokes `P::glibre_plugin_register` to rebuild any caches
   P dropped at step 1 (which is why P's `register` is required to
   be idempotent — it may run a second time after a failed swap).
5. **Step 4.1 / 4.2 failure.** Treated identically to step 3
   failure: full rollback of swap + migrate, P remains live.
   Migrated bytes are reverted by running the **inverse migrate
   chain** when one exists (per `reviews/decisions/hot-reload-
   protocol.md` §"Failure & Rollback"); when no inverse exists for
   some chain step the originating context declared, the loader
   terminates the process — non-invertible migrations force a
   step-4 failure to a hard-fail, loud and immediate, exactly
   because deterministic replay cannot accept a silently
   half-rolled-back world.
6. **Refusal logging.** Every rollback path publishes
   `HotReloadRefusedEvent { plugin_fqn, cause }` (§5.12) on the
   observer bus exactly once, between the failure detection and
   the next pending reload's step 1. Subscribers that received
   `HotReloadStartedEvent` for the same `plugin_fqn` see exactly
   one terminal event per transaction.

The **terminate-on-contract-violation** cases (step 2.3/2.4 fault,
non-invertible step-4 rollback) are deliberately loud. They
indicate the loader's own invariants were broken; the only
acceptable response is a process kill so CI surfaces the bug
immediately.

### 8.9 Test Hooks

The barrier exposes a deterministic E2E trigger so the test harness
can exercise hot-reload without touching the filesystem and without
racing a development-build watcher. The trigger is the same public
surface that production uses — `HotReloadBarrier::request_reload`
(§5.8) — wrapped under a `#if defined(GLIBRE_E2E)` namespace that
adds blocking-await on the matching terminal event:

```cpp
// Visible under -DGLIBRE_E2E only. Production builds link only
// the plain HotReloadBarrier surface from §5.8.

namespace glibre::core::test {

// Blocks the calling thread until the referenced request reaches
// a terminal state (Completed or Refused). Returns ok on Completed,
// the §8.7 inner Error on Refused.
[[nodiscard]] std::expected<void, glibre::Error>
await_reload(HotReloadBarrier&  barrier,
             ReloadRequestId    id) noexcept;

}  // namespace glibre::core::test
```

The harness drives the contract through three CI scenario classes,
each backed by Catch2 cases referenced by name from §11:

1. **Happy-path reload, no schema bump.** Assert
   `HotReloadCompletedEvent` fires; assert `frame_index` advances
   by exactly 1 between `request_reload` return and
   `await_reload` return (the swap happens at the immediately-
   next phase 8); assert `Entity` and `AssetHandle` bits cached
   before the swap remain valid after it (§8.1).
2. **Each refusal case.** One Catch2 case per row of the §8.7
   table. Each fixture ships a malformed plugin (`bad-abi-hash`,
   `failing-init`, `failing-drain`, `cycle-induced-by-register`,
   `dropped-component-type`) that triggers the matching refusal;
   asserts `HotReloadRefusedEvent` fires with the expected `cause`
   inner arm and that the prior plugin still ticks at frame
   N+1 (§8.8 #1, #2, #4, #5).
3. **Schema migration round-trip.** A `v1-to-v2-migration` fixture
   plugin bumps a component schema; the harness records a frame N
   golden with the v1 storage, requests the reload, and asserts
   that the migrated v2 bytes round-trip byte-equal to a hand-
   authored expected payload (per the §7.3 round-trip obligation,
   widened here to plugin-owned schemas). A companion
   `migrate_T_v1_to_v2_force_fail` injects a step-3 failure and
   asserts full rollback **including the type-registry revert**
   (§8.8 #4).

All scenarios run in-process on a single deterministic frame
sequence; the filesystem-watcher path used in development builds
is a thin wrapper that calls `request_reload`, so testing the
in-process path covers the loader's own state machine.

### 8.10 Cross-References

- Aggregate invariants: §4.6 `HotReloadBarrier` (#1–#7), §4.5
  `Plugin / PluginLoader` (#1–#3), §4.11 cross-aggregate invariants
  #3 (entity stability across reload), #4 (self-reload refusal).
- Public interface: §5.1 `core::Error` enum (`HotReload`,
  `HotReloadDrainTimeout`, `HotReloadAbiHashMismatch`,
  `HotReloadSelfReference`, `SchemaMigrationFailed`, the
  `Plugin{...}` family); §5.3 `Phase::HotReload`; §5.8
  `HotReloadBarrier`, `ReloadRequestId`, `ReloadOutcome`,
  `ReloadStatus`; §5.9 `PluginLoader`; §5.12 the `HotReload*Event`
  triplet.
- Persistence: §7.1 `HotReloadCheckpoint`, `LoadedPluginRecord`;
  §7.3 migration-body contract (extended to plugin authors at §8.2);
  §7.4 the load-bearing refusal that ECS world bytes are
  plugin-owned, never core's.
- Decision records: `reviews/decisions/hot-reload-protocol.md`
  (the source of the four-step state machine and the refusal-cause
  table); `reviews/decisions/frame-phases.md` (the pinning of phase
  8 as the only barrier position); `reviews/decisions/plugin-abi.md`
  (the load-time half of the loader sequence and the
  `core::Error::Plugin{...}` arms reused by §8.7);
  `reviews/decisions/fory-codegen.md` (the `MigrationChain`
  dispatcher §8.2 calls into); `reviews/decisions/error-model.md`
  (the `glibre::Error` umbrella and the warn-level logging rule).

## 9. Performance Budget

### 9.1 Engine-Wide Allocation (Citation)

`reviews/decisions/perf-budget.md` Per-Context Budget Table assigns
`core` the cell **0.40 ms CPU sim + 0.05 ms CPU submit + n/a GPU + 64
MiB heap**, with phase ownership "owns 5 (transform), 8 (hot-reload);
systems in 1, 9". The 16.67 ms / 60 fps wall-clock target is met with
≥1.5 ms reserved headroom (`perf-budget.md` §"Decision"); core's row
is part of the **8.05 ms CPU steady-state** that constitutes the
sim-plus-submit half of every frame on the game-loop driver thread.
This SPEC §9 is the per-aggregate refinement of that cell — it MUST
sum into the cited row and MUST NOT silently expand it. Any cell-level
amendment requires a perf-budget amendment spike per
`perf-budget.md` §"Consequences".

### 9.2 Cell Summary

The core context's full cell, quoted verbatim from the engine-wide
table:

| Axis              | Budget       | Source                                      |
|-------------------|--------------|---------------------------------------------|
| CPU (sim)         | 0.40 ms      | `perf-budget.md` Per-Context Budget Table   |
| CPU (submit)      | 0.05 ms      | `perf-budget.md` Per-Context Budget Table   |
| CPU (combined)    | **0.45 ms**  | sum, used as the §9.3 sub-budget ceiling    |
| GPU               | n/a          | core owns no rendering work (PHILOSOPHY #3) |
| Heap ceiling      | **64 MiB**   | `perf-budget.md` Per-Context Budget Table   |
| Phase ownership   | 5, 8         | full ownership (transform; hot-reload)      |
| Phase systems     | 1, 9         | tick advance; frame-stat counter writes     |

Combined-CPU is the figure the §9.3 per-aggregate rows sum into. The
sim/submit split is enforced at the cell boundary, not per-aggregate;
each aggregate is annotated in §9.3 with which half it belongs to so
the totals reconcile against `perf-budget.md` §"Pipelined Frame
Timing" line-by-line.

### 9.3 Per-Aggregate Sub-Budgets

Sub-budgets refine the §9.2 cell across the seven `core`-owned
aggregates from §4. Each row lists CPU ms (sim or submit half), heap
allocation under the 64 MiB ceiling, and the dominant operation that
the budget pays for. The CI gate (§9.5) attaches one
`BENCHMARK_CELL(...)` block per aggregate that asserts steady-state
CPU time is `<= cpu_ms` under the S1 fixture defined in
`perf-budget.md` §"Justification Per Cell".

| Aggregate (§4 ref)       | CPU ms  | Half   | Heap     | Dominant operation                                                        |
|--------------------------|---------|--------|----------|---------------------------------------------------------------------------|
| `World` (§4.1)           | ~0.20   | sim    | 24 MiB   | archetype iteration + change-tick scan over packed component storages     |
| `Schedule` (§4.4)        | ~0.10   | sim    | 4 MiB    | phase ordering hot loop dispatching the nine-phase `switch` (§6.5)        |
| `FrameLoop` (§4.4)       | ~0.05   | sim    | 4 MiB    | dispatch overhead: phase switch + frame-stat counter writes               |
| `PluginLoader` (§4.5)    | 0       | submit | 4 MiB    | idle steady-state; load/unload work happens off the per-frame critical path |
| `HotReloadBarrier` (§4.6)| 0       | (n/a)  | 16 MiB   | scratch arena reserved; idle when no reload pending (relaxed atomic only) |
| `AssetHandle` table (§4.7)| ~0.05  | sim    | 8 MiB    | O(1) handle resolution + generation-tag check on lookup                   |
| `CommandBuffer` pool (§4.8)| ~0.10 | submit | 4 MiB    | per-system arena drain at the end of each system's apply phase            |
| **per-aggregate total**  | **~0.50**| —     | **64 MiB**| sums against §9.2 combined-CPU 0.45 ms (rounding margin within headroom)  |

Notes per row, indexed by aggregate:

- **`World` (~0.20 ms / 24 MiB).** Phase 5 transform propagation +
  phase-1 / 9 system dispatch reads dominate. `core`'s 0.40 ms sim
  in `perf-budget.md` §"Justification" calls out the ~2k entity
  dirty-set sweep at <0.3 ms; ~0.20 ms is the steady-state for the
  archetype iteration plus change-tick comparison loop. 24 MiB holds
  the archetype tables (estimated 8–16 KiB × ~256 archetypes max +
  handle tables) per the §6.2 `Chunk` layout and `perf-budget.md`
  Allocator Rules tagging.
- **`Schedule` (~0.10 ms / 4 MiB).** The phase ordering hot loop
  (§6.4 DAG dispatch) iterates the precomputed system order produced
  at registration time. No per-frame DAG recompute; the cost is the
  function-pointer indirection per system plus the small amount of
  bookkeeping to advance the phase cursor. 4 MiB stores the
  precomputed order arrays + access-set bitmasks.
- **`FrameLoop` (~0.05 ms / 4 MiB).** The §6.5 nine-phase `switch`
  dispatch plus tick advance and frame-stat counter writes. This is
  the `core`'s 0.05 ms submit half from the engine table. 4 MiB
  reserves the per-frame stats ring buffer and the phase-cursor
  state.
- **`PluginLoader` (0 / 4 MiB).** Idle on the per-frame critical path
  in shipping builds: no per-frame work outside hot-reload windows.
  Load and unload (§6.6) are one-shot operations triggered out-of-band
  and budgeted under `perf-budget.md` §"Hot-reload frame budget"
  rather than every-frame. 4 MiB holds the registry-of-registries
  bookkeeping (`LoadedPlugin` records, owned-type/system index maps).
- **`HotReloadBarrier` (0 / 16 MiB).** The `step()` body is a single
  relaxed atomic load on `pending_` when no reload is queued (§6.7);
  steady-state cost is therefore zero, well inside the per-frame
  budget. The 16 MiB is the **migration scratch arena** cited in
  `perf-budget.md` Allocator Rule #6 and §6.7 — drained between
  migration rows, never resident across frames, and accounted under
  `core`'s 64 MiB ceiling so a runaway migration cannot push other
  aggregates' allocations off-budget. Reload-frame cost is permitted
  up to 0.40 ms one-shot per `perf-budget.md` §"Pipelined Frame
  Timing" and is **excluded from the steady-state CPU sub-budget**.
- **`AssetHandle` table (~0.05 ms / 8 MiB).** O(1) handle ops:
  resolve, refcount inc/dec, generation-tag check (§4.7). The 0.05 ms
  is for the per-frame residency tickle + sanity sweep that runs in
  phase 1 / 9 systems; the table is not walked exhaustively per
  frame. 8 MiB holds the dense slot array + free-list (generation-
  tagged) for the MVP asset count ceiling.
- **`CommandBuffer` pool (~0.10 ms / 4 MiB).** Per-system arena drain
  at apply time (§4.8). Each system's `CommandBuffer` is a stack
  arena that is replayed and reset at the end of its execution slot;
  the 0.10 ms covers the aggregate replay cost across all systems
  registered in one frame. 4 MiB is the union of per-system arena
  caps; arenas reset to zero each frame so this is a residency
  ceiling, not steady-state usage.

The per-aggregate CPU total (~0.50 ms) exceeds the §9.2 combined-CPU
0.45 ms by ~0.05 ms; that ~0.05 ms slack is **inside the engine
headroom row**, not stolen from another context. The slack exists so
benchmark-asserted ceilings can be set at the per-aggregate row
without immediately tripping when one aggregate runs at the high end
of its variance. The headroom regression alarm (`perf-budget.md` §"CI
Gate Spec" rule 5) catches sustained drift.

### 9.4 Allocator Rules

`core` enforces its 64 MiB ceiling — and the per-aggregate sub-shares
in §9.3 — through `glibre::PerContextAllocator`, the allocator
declared in `perf-budget.md` §"Allocator Rules" and implemented under
plan #238. The contract `core` SPEC §9 imposes:

1. **Per-context tag (`ContextTag::core`).** Every allocation made by
   any module under `core/src/**` is stamped with `ContextTag::core`
   at the allocator-handle level (`perf-budget.md` Allocator Rule
   #1). The tag is supplied by the allocator handle that core's
   modules obtain at startup; module call sites are tag-free,
   eliminating the class of "forgot to tag" drift bugs.
2. **Hard ceiling in diagnostic / debug builds.** When
   `GLIBRE_ALLOC_STRICT=1` (debug + diagnostic presets), an
   allocation that would push live `ContextTag::core` bytes above 64
   MiB returns `std::unexpected{core::Error::OutOfBudget}`
   (`perf-budget.md` Allocator Rule #2). Every core call site that
   allocates uses the `Result<T>` form (§5) and propagates the error;
   a missing handler aborts with the diagnostic dump documented in
   plan #238.
3. **Soft warning in shipping builds.** Shipping builds log a `warn`
   once per-tag-per-frame to `spdlog` and increment a frame-stat
   counter on overshoot (`perf-budget.md` Allocator Rule #3). The
   editor's perf HUD surfaces the counter (cross-context plumbing in
   `tools`).
4. **Per-aggregate sub-shares are advisory at the allocator level.**
   `PerContextAllocator` enforces the 64 MiB cell ceiling, not the
   §9.3 per-aggregate row ceilings; per-aggregate enforcement is via
   the `BENCHMARK_CELL` heap-residency assertions (§9.5) which
   exercise the S1 fixture and record the resident bytes at frame
   end. This split keeps the runtime allocator path branch-free per
   aggregate while still catching drift on a CI cadence.
5. **Migration arena exemption pattern (§6.7).** The 16 MiB migration
   arena under `HotReloadBarrier` is allocated through the same
   `PerContextAllocator` handle and counts against `core`'s 64 MiB.
   It is not a transient-arena exemption (`perf-budget.md` Allocator
   Rule #4) — those are per-frame and drain at phase 9; the migration
   arena is per-reload-row and drains between rows. The accounting
   model documents the arena explicitly so it is not double-budgeted.
6. **No raw `new` / `malloc` in `core/`.** Per the
   `-Wglibre-no-raw-alloc` clang custom-warning-as-error documented
   in `perf-budget.md` Allocator Rules header, all dynamic allocations
   in `core/src/**` MUST go through `PerContextAllocator`. The build
   rejects raw `new` / `malloc`. Standard-library containers use
   `std::pmr::*` with `core`'s `PerContextAllocator`-backed
   `memory_resource` (§6 module layout).

### 9.5 CI Gate — `BENCHMARK_CELL` Per Aggregate

The per-context CI gate (`perf-budget.yml`, scoped under the
`task-breakdown-error-perf` follow-up spike) requires each per-context
SPEC §9 to declare at least one `BENCHMARK_CELL(...)` block per
aggregate (`perf-budget.md` §"CI Gate Spec" rule 1). The macro
expands to a Catch2 `BENCHMARK` body that:

1. constructs the §9.3 row's S1-derived fixture for the aggregate,
2. measures wall-clock time over a steady-state sample,
3. asserts `time <= cell_budget_ms` (the row's CPU ms cell),
4. records resident-byte usage for the aggregate's `ContextTag::core`
   sub-share and asserts `<= heap_budget_mib` (the row's heap cell).

Mandated `BENCHMARK_CELL` blocks for `core` (one per aggregate; PR
fails if any assert fails — `perf-budget.md` §"CI Gate Spec" rule 1):

| Aggregate            | `BENCHMARK_CELL` test name                          | CPU ceiling | Heap ceiling |
|----------------------|-----------------------------------------------------|-------------|--------------|
| `World`              | `core/world: archetype_iteration_change_tick_scan`  | 0.20 ms     | 24 MiB       |
| `Schedule`           | `core/schedule: phase_ordering_hot_loop`            | 0.10 ms     | 4 MiB        |
| `FrameLoop`          | `core/frame: dispatch_overhead`                     | 0.05 ms     | 4 MiB        |
| `PluginLoader`       | `core/plugin: idle_steady_state`                    | 0 ms        | 4 MiB        |
| `HotReloadBarrier`   | `core/hot-reload: idle_relaxed_atomic_only`         | 0 ms        | 16 MiB       |
| `AssetHandle` table  | `core/asset: handle_o1_resolve_refcount`            | 0.05 ms     | 8 MiB        |
| `CommandBuffer` pool | `core/command-buffer: per_system_arena_drain`       | 0.10 ms     | 4 MiB        |

Reload-frame variance (`HotReloadBarrier` one-shot up to 0.40 ms) is
exercised by the separate **hot-reload frame budget** gate
(`perf-budget.md` §"CI Gate Spec" rule 4) which scripts an S2 reload
and asserts phase 8 cost ≤ 0.40 ms; the `core/hot-reload:
idle_relaxed_atomic_only` row covers steady-state only.

End-to-end thresholds (`perf-budget.md` §"CI Gate Spec" rule 2) do
not bind individual aggregates; they bind the engine-wide
`cpu_sim+cpu_submit` per-frame totals. `core`'s contribution to those
is the §9.2 cell, and the `BENCHMARK_CELL` micro-asserts above are
the only enforcement `core` SPEC §9 imposes per-aggregate.

### 9.6 References

- `reviews/decisions/perf-budget.md` — engine-wide allocation cited
  in §9.1, allocator rules cited in §9.4, CI gate spec cited in §9.5.
- `reviews/decisions/frame-phases.md` — phase-ownership table
  referenced by §9.2 (phases 5, 8 owned by `core`; phases 1, 9
  systems registered by `core`).
- `specs/core/SPEC.md` §6.2 (`world/`), §6.4 (`schedule/`), §6.5
  (`frame/`), §6.6 (`plugin/`), §6.7 (`hot-reload/`), §6.8 (`asset/`),
  §6.3 (`CommandBuffer` arena) — the implementation surfaces whose
  steady-state cost the §9.3 rows budget against.
- Plan #238 — `glibre::PerContextAllocator` implementation
  (per-tag heap ceiling enforcement; Allocator Rules #1–#3 from
  `perf-budget.md`).

## 10. Failure Modes & Error Model

This section is the closed enumeration of every `core::Error` arm
emitted by the public boundaries declared in §5, the trigger condition
that raises each arm, the recovery posture (rollback / retry / refuse /
abort) the loader, schedule, or barrier takes, the observer event (if
any) published when the arm fires, and the `spdlog` severity each arm
uses. The table is the authoritative cross-reference for §11
acceptance-criteria tests: every arm below MUST have at least one
Catch2 case named in §11 that drives the trigger and asserts the
recovery + severity.

The arms below are the §5.1 enum (`enum class core::Error`)
listed in registration order. Per `reviews/decisions/error-model.md`
§"Type Sketch", every arm is a leaf with stable `to_string` mapping;
per the same record's §"Logging / Telemetry" rule, each arm is logged
**exactly once at the boundary where it is handled** (not at the site
that raises it) via `glibre::log_error(err, level)`. The "Severity"
column below is the level that handler passes; arms wrapped under the
`HotReload` umbrella (§8.7) keep `warn` per the error-model decision
record's hot-reload-refusal rule, even when their inner cause would
otherwise log at `error`.

The table is grouped by the §4 aggregate that emits each arm. The
"Recovery" column uses one of four verbs:

- **Refuse** — public API call returns `std::unexpected`; caller state
  is unchanged; no rollback needed because no state moved (the arm
  fires before any mutation). Caller decides whether to retry, log, or
  surface to the operator.
- **Rollback** — internal state had partially advanced; the emitting
  module reverts to the pre-call snapshot before returning
  `std::unexpected`. The §8.8 hot-reload rollback discipline is the
  load-bearing example; the schedule's compile-on-failure-revert
  (§4.4 invariant 3) is another.
- **Retry** — the next equivalent invocation may succeed without
  operator action (e.g. a coalesced `request_reload` on the same
  `plugin_fqn` after the operator addresses the cause). The arm itself
  refuses *this* attempt; "retry" is shorthand for "the API admits a
  follow-up call once the trigger condition clears".
- **Abort** — process termination via `std::terminate` (or equivalent
  loud-fail). Reserved for contract violations the loader's invariants
  cannot tolerate (§8.8 step 2.3 / 2.4 fault, non-invertible step-4
  rollback). Distinguished from "Refuse" because abort cannot be
  caught and is observed only via the OS-level exit code +
  `glibre::log_error(err, error)` written before termination.

### 10.1 Failure-Mode Table

| `core::Error` arm                | Trigger (§ ref)                                                                                                            | Recovery   | Observer event (§5.12)                       | Severity |
|----------------------------------|----------------------------------------------------------------------------------------------------------------------------|------------|----------------------------------------------|----------|
| `EntityStale`                    | `Entity` resolves to a slot whose generation has advanced (§4.3 #1, §4.11 #1).                                              | Refuse     | none                                         | `warn`   |
| `EntityForeignWorld`             | `Entity` from world A passed to world B's API (§4.3 #3; reserved for post-MVP multi-world).                                 | Refuse     | none                                         | `error`  |
| `HierarchyCycle`                 | `ChildOf` mutation would introduce a cycle in the parent/child forest (§4.1 #2).                                            | Refuse     | none                                         | `error`  |
| `TypeUnregistered`               | Public API names a `TypeId` not present in the immutable-after-init `TypeRegistry` (§4.1 #3, §4.9 #2).                      | Refuse     | none                                         | `error`  |
| `TypeRegistryClosed`             | Mutation of `TypeRegistry` attempted after `World` construction completes (§4.9 #1).                                        | Refuse     | none                                         | `error`  |
| `ScheduleAccessConflict`         | Two systems' declared `(reads, writes)` sets overlap such that no DAG ordering is conflict-free (§4.4 #2, §6.4).            | Refuse     | none                                         | `error`  |
| `SystemScheduleCycle`            | Schedule rebuild detects a cycle in the union of every loaded plugin's `(after, before)` declarations (§4.4 #4, §8.6 #2).   | Rollback   | `HotReloadRefusedEvent` (when raised in §8.6) | `warn` (in hot-reload context) / `error` (at static `Schedule::compile`) |
| `FramePhaseMisordered`           | Debug-build assertion: a phase observed a write from a later phase of the same frame (§4.4 #1).                             | Abort      | none                                         | `error`  |
| `AssetStale`                     | `AssetHandle::resolve` finds a slot whose generation has advanced past the handle's (§4.7 #1).                              | Refuse     | none                                         | `warn`   |
| `CommandBufferOverflow`          | A `CommandBuffer` append would exceed the per-frame arena cap declared by §9.3 (§4.8 #4).                                   | Refuse     | none                                         | `warn`   |
| `PluginDlopenFailed`             | `dlopen` returns null at `PluginLoader::load` step 1 (`reviews/decisions/plugin-abi.md` §"Loader Sequence").                | Refuse     | none (load-time, no barrier transaction)      | `error`  |
| `PluginMissingEntryPoint`        | `dlsym` cannot resolve one of the four required exports at step 2 (plugin-abi §"Loader Sequence").                          | Refuse     | none                                         | `error`  |
| `PluginManifestInvalid`          | Fory `deserialize<PluginManifest>` fails at step 3 — or, in §8.4 step 2.2, Q drops a component FQN P registered.            | Refuse / Rollback | `HotReloadRefusedEvent` (when raised at §8.4 step 2.2) | `error` (at load) / `warn` (under §8.7 umbrella) |
| `PluginAbiHashMismatch`          | Plugin-vs-host hash mismatch at load step 4, OR `schema_hash` drift on an existing FQN at §8.4 step 2.2.                    | Refuse / Rollback | `HotReloadRefusedEvent` (when raised under §8.4) | `error` (at load) / `warn` (under §8.7 umbrella) |
| `PluginEngineTooOld`             | Host `glibre_core_version` < `manifest.min_engine_version` at load step 5 (plugin-abi §"Versioning Rules").                 | Refuse     | none                                         | `error`  |
| `PluginNameCollision`            | A plugin with the same `PluginManifest.name` is already registered under a different file path (load step 6, §4.5 #1).      | Refuse     | none                                         | `error`  |
| `PluginDependencyMissing`        | An entry in `manifest.depends_on` is not yet registered at load step 7 (§4.5 #3).                                           | Refuse     | none                                         | `error`  |
| `PluginDependencyCycle`          | The `depends_on` graph contains a cycle when the loader topologically sorts a multi-plugin batch (§4.5 #3).                 | Refuse     | none                                         | `error`  |
| `PluginInitFailed`               | `glibre_plugin_register` returns `unexpected(...)` at load step 9 OR §8.6 step 4.1 (resume) OR drain returns unexpected at §8.3 step 2 (drain arm). | Rollback (compensating unregister) | `HotReloadRefusedEvent` (when raised under §8.6 / §8.3) | `error` (at load) / `warn` (under §8.7 umbrella) |
| `HotReload`                      | Umbrella tag set on every refusal raised inside `HotReloadBarrier::step` (§4.6 #7, §8.7). Wraps one of the inner causes below. | Rollback   | `HotReloadRefusedEvent`                      | `warn`   |
| `HotReloadDrainTimeout`          | `glibre_plugin_drain` does not return within the §9 drain budget (§4.6 #2, §8.3 step 3).                                    | Rollback   | `HotReloadRefusedEvent`                      | `warn`   |
| `HotReloadAbiHashMismatch`       | ABI-hash recheck at §8.4 step 2.1 fails between `PluginLoader::load` and the next phase-8 entry (§4.6 #3).                  | Rollback   | `HotReloadRefusedEvent`                      | `warn`   |
| `HotReloadSelfReference`         | `request_reload` names a plugin whose register-time work would itself touch the loader (§4.11 #5, §8.7 last row).            | Refuse     | none (refused at request, not at barrier step) | `warn`  |
| `SchemaMigrationFailed`          | Missing chain `(stored → current)` for a type at §8.5 step 3, or a migrate-chain step returns `unexpected` (§4.6 #4, §8.7). | Rollback   | `HotReloadRefusedEvent`                      | `warn`   |

### 10.2 Cross-Cutting Notes

1. **Severity escalation under hot-reload.** Every arm raised inside
   `HotReloadBarrier::step` (§8.3–§8.6) logs at `warn`, even when its
   non-barrier counterpart logs at `error`. The hot-reload contract
   guarantees the previous-good plugin keeps running on every refusal
   (§4.6 #7, §8.8 #1–#5), so the steady state remains tolerable; the
   `warn` level signals "operator action required, but engine still
   ticking". This is the load-bearing rule from
   `reviews/decisions/error-model.md` §"Logging / Telemetry" #3 applied
   uniformly across §10.1's umbrella column. Outside the barrier
   (e.g. `PluginLoader::load` raising `PluginAbiHashMismatch` on a
   first-time load) the arm logs at `error` because there is no
   prior-good plugin to fall back on.

2. **Observer events fire on terminal transitions only.** Per §8.6
   and §8.8 #6, `HotReloadRefusedEvent` is published exactly once
   per per-plugin transaction at the rollback commit point;
   `HotReloadCompletedEvent` is the success-path counterpart and
   is the *absence* of any §10.1 arm. Subscribers that received a
   `HotReloadStartedEvent` for a `plugin_fqn` see exactly one
   terminal event per transaction (refusal or completion), never
   both, never neither (§8.6 step 3, §8.8 #6).

3. **`HotReload` umbrella semantics.** `core::Error::HotReload`
   carries an inner cause arm via `glibre::Error`'s `ErrorContext`
   payload (per the error-model decision record's `ErrorContext`
   sketch). Consumers pattern-match `code()` against `HotReload`
   first, then read the inner arm from `where().detail` for the
   specific cause. The §8.7 refusal-cases table is the closed
   mapping from inner cause to operator action.

4. **Abort is reserved.** The two `Abort` rows in §10.1
   (`FramePhaseMisordered` debug assertion, plus the §8.8 step 2.3 /
   2.4 fault and non-invertible step-4 rollback paths whose
   triggers are *not* enumerated in §10.1 because they are loader
   contract violations rather than `core::Error` arms in their own
   right) terminate the process with `glibre::log_error(err, error)`
   written first. CI surfaces the exit code; no observer event
   fires because the process is already exiting. Silent recovery
   is forbidden because it would mask bugs the
   deterministic-snapshot contract (PHILOSOPHY §7) cannot tolerate.

5. **Caller-side recovery posture.** Public APIs in §5 return
   `Result<T> = std::expected<T, glibre::Error>`. Callers handle
   each §10.1 arm at exactly one boundary (per the error-model
   decision record's §"Logging / Telemetry" #1). The recovery
   verb in §10.1 is the *emitting module's* posture; the *caller's*
   posture is always one of:
   - **propagate** (`return std::unexpected{err}`) — common in
     plugin-loader internal helpers,
   - **handle + log** (`glibre::log_error(err, level)` followed
     by a bounded fallback) — common at the `FrameLoop::tick`
     boundary that drives `HotReloadBarrier::step`,
   - **terminate** (the §10.2 #4 abort path) — reserved for
     contract violations.

6. **Allocator-budget arm interaction.** `core::Error::OutOfBudget`
   from `reviews/decisions/error-model.md` §"Type Sketch" is
   raised by `glibre::PerContextAllocator` (§9.4 rule #2) when an
   allocation under `ContextTag::core` would exceed the 64 MiB
   ceiling. It is **not** listed in §10.1 because its emitter is the
   per-context allocator (an engine-wide service shared across all
   contexts), not a `core` aggregate. The arm is documented under
   the engine error-model record and surfaces in `core` call sites
   as a propagated `std::unexpected` from any allocation-bearing
   API; recovery is identical to the other §10.1 arms (refuse,
   propagate, log at the handling boundary). `core` SPEC §10
   references it here so the allocator interaction is explicit
   without duplicating the engine-wide enumeration.

7. **Deterministic-replay obligation.** Every §10.1 arm that fires
   inside the deterministic core path (everything except hot-reload,
   which is by contract excluded from determinism per §8.1 and
   PHILOSOPHY §7) must be byte-equal across runs and hosts: the same
   trigger from the same input produces the same arm with the same
   `ErrorContext` payload. Tests in §11 backed by the S1 fixture
   (`reviews/decisions/perf-budget.md` §"Justification") assert this
   via golden-snapshot comparison on the error stream. Hot-reload
   arms are exempt only because the trigger (a filesystem watcher
   event, an operator request) is non-deterministic; the rollback
   path itself is deterministic and asserted under §8.9 scenario
   class 2.

### 10.3 Cross-References

- §4 aggregate invariants — every arm in §10.1 names the §4
  invariant (`#N`) that detects its trigger.
- §5.1 — the closed `enum class core::Error` whose arms §10.1
  enumerates one-to-one (modulo `OutOfBudget`, which is engine-wide
  per §10.2 #6).
- §5.12 — the three `HotReload*Event` structs whose firing rule
  §10.1 references in the "Observer event" column.
- §8.7, §8.8 — the hot-reload refusal-cause table and rollback
  discipline §10.1 cites for every `HotReload*` arm.
- §9.4 — `glibre::PerContextAllocator` and the
  `core::Error::OutOfBudget` interaction noted in §10.2 #6.
- §11 — acceptance-criteria tests; every §10.1 row must have at
  least one Catch2 case driving the trigger.
- `reviews/decisions/error-model.md` — `glibre::Error` umbrella,
  per-context enum composition, and the `glibre::log_error`
  severity/once-per-handle rule §10.2 #1 and §10.2 #5 cite.
- `reviews/decisions/hot-reload-protocol.md` — the four-step state
  machine and the three-refusal-cases rule §10.1 expands across
  the §8.3–§8.6 detection points.
- `reviews/decisions/plugin-abi.md` — the loader sequence and the
  load-time refusal arms §10.1 lists in the `Plugin*` rows.

## 11. Acceptance Criteria

GitHub `type:user-story` issues this spec closes (parented to sub-epic
#19; drafted under spike #29; each carries a Catch2 test name plus the
story-required E2E `.glibre-trace`):

- #320 — `[STORY] spawn-despawn-entity-lifecycle` — pts:3
- #322 — `[STORY] archetype-soa-chunked-storage` — pts:5
- #324 — `[STORY] component-add-remove-archetype-migration` — pts:3
- #326 — `[STORY] queries-with-without-changed` — pts:5
- #328 — `[STORY] command-buffer-deferred-mutation` — pts:5
- #329 — `[STORY] component-lifecycle-hooks` — pts:3
- #331 — `[STORY] schedule-access-set-dag` — pts:5
- #333 — `[STORY] frame-loop-nine-phase-ordering` — pts:3
- #335 — `[STORY] typed-singleton-resources` — pts:3
- #337 — `[STORY] changetick-monotonic-clock` — pts:2
- #338 — `[STORY] childof-relationship-forest` — pts:3
- #340 — `[STORY] plugin-load-manifest-validation` — pts:3
- #343 — `[STORY] plugin-abi-hash-gate` — pts:3
- #344 — `[STORY] plugin-dependency-topological-order` — pts:3
- #345 — `[STORY] hot-reload-frame-boundary-barrier` — pts:5
- #346 — `[STORY] hot-reload-refusal-rollback` — pts:5
- #347 — `[STORY] type-registry-immutable-after-init` — pts:3
- #348 — `[STORY] asset-handle-generational-table` — pts:2

Each must have a Catch2 test by name.

## 12. Open Questions

- Owner / resolution gate.
