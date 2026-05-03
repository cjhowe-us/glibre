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
