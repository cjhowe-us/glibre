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
