# Roadmap

Three horizons. No time estimates — story points only. Each horizon's
detail lives in `plans/{mvp,post-mvp,long-term}.md` with GitHub issue
links.

## Short-term — MVP (`plans/mvp.md`)

Smallest end-to-end engine that proves the architecture: scene editor +
hot-reloadable plugin runtime + deterministic physics + hybrid-RT-shadow
mesh-shader gbuffer + asset import + standalone runtime. macOS only.

MVP contexts (each = one plugin + one spec + one epic):

- `core` — custom codegen-driven archetype ECS, plugin loader,
  hot-reload barrier, frame loop, type registry, asset handle table.
- `platform` — SDL3 window/input, file watcher, Metal 4 surface (via
  metal-cpp).
- `data` — Apache Fory schemas, reflection, migration.
- `shader` — Slang → slangc → metallib (Apple Silicon / Metal 4); slangc native reflection.
- `render` — declarative render graph, Metal 4 backend (metal-cpp),
  mesh shader gbuffer, deferred lighting, hybrid-RT shadows, present.
- `geometry` — meshoptimizer meshlets + Draco mesh(let) compression
  (cooked-asset side), BLAS build for static meshes.
- `physics` — Jolt rigid bodies, deterministic config.
- `content` — FBX import (FBX SDK), FreeImage, FreeType, CAS, residency.
- `tools` — editor shell: scene tree, transform gizmo, inspector, asset
  browser, play/pause. No graph editors.
- `e2e` — InputDriver abstraction, replay traces, golden-screenshot +
  ECS-snapshot assertions, editor record mode.

MVP exit: editor opens, loads sample scene, plays + pauses, hybrid-RT
shadow visible in Xcode capture, hot-reload preserves state, physics
deterministic across 10 runs, all MVP stories' acceptance tests green.

## Mid-term — Post-MVP (`plans/post-mvp.md`)

Layered onto MVP without core rewrite. Each item independent:

- Virtualized geometry — cluster DAG, screen-space-error LOD, streaming.
- Hybrid RT expansion — AO, reflections, GI probes, denoisers.
- Visual scripting (logic graph) editor + codegen-to-C++.
- Material graph editor + Slang fragment codegen.
- Effects/VFX graph + GPU compute particles.
- Render graph editor (read-only viewer first).
- Skeletal animation + state machines.
- Audio (spatial, mixer).
- Spatial indexing, NavMesh.
- Profiler UI.

## Long-term — Harmonius parity horizon (`plans/long-term.md`)

Match the full feature scope mined from harmonius (~3,162 stories) under
a coherent architecture. Drives:

- AI (behavior trees, perception, planners).
- Networking (replication, prediction/rollback).
- Collaborative editing, asset versioning, build farm.
- Procedural generation, cinematics/timeline.
- Advanced rendering (DDGI, RTGI, volumetrics, hair, water).
- Full game-framework primitives (containers, graphs, tables,
  attributes, grids, timelines, event logs) — re-validated, not ported.
- Windows + D3D12 backend; Linux + Vulkan backend.

Path mid-term → long-term is incremental plugin addition. SRP discipline
keeps core untouched.
