# Roadmap

Three horizons. No time estimates — story points only. Each horizon's detail lives in
`plans/{mvp,post-mvp,long-term}.md` as GitHub-issue index pointers.

## Short-term — MVP (`plans/mvp.md`)

Smallest end-to-end engine that proves the architecture: scene editor

- hot-reloadable codegen middleman + deterministic physics +
mesh-shader + ray-tracing render path + Slang shader pipeline + asset import + standalone runtime.
macOS first, Windows + Linux follow once the substrate is proven.

MVP contexts (each = one Rust crate + one spec + one epic):

- `core-runtime` — custom codegen-driven archetype ECS, plugin loader, hot-reload barrier, frame
  loop, type registry, asset handle table, change detection, console vars, error / primitive types.
- `platform` — custom windowing (NSWindow / Win32 / X11 / Wayland), input devices, file watcher, OS
  event loop, threading, job system, Metal / D3D12 / Vulkan surface seam.
- `data-systems` — generic primitives (directed graphs, data tables, attributes / effects,
  containers / slots).
- `rendering` — declarative render graph in Rust, GPU-driven mesh-shader gbuffer + clustered light
  culling + deferred lighting
  - hybrid-RT shadows + post + present; Slang shader pipeline through
  `slangc` (slangc native reflection, BLAKE3 CAS shader cache, permutation key codec).
- `geometry` — Nanite-style cluster LOD (cluster DAG, BLAS recipes, meshlet streaming).
- `physics` — Jolt-equivalent or in-house deterministic rigid-body sim (fixed timestep, shared BVH,
  private physics BVH).
- `content-pipeline` — asset import (glTF / Alembic / KTX2 / EXR / PNG), `rkyv` baking, CAS keyed by
  BLAKE3, residency, file watcher.
- `tools` — editor shell: scene tree, transform gizmo, inspector, asset browser, play/pause, command
  stack, trace recorder.
- `integration` — pair-wise contracts between subsystems.

MVP exit: editor opens, loads sample scene, plays + pauses, hybrid-RT shadow visible, hot-reload
preserves state, physics deterministic across 10 runs, all MVP stories' acceptance tests green.

## Mid-term — Post-MVP (`plans/post-mvp.md`)

Layered onto MVP without core rewrite. Each item independent:

- Visual scripting (logic graph) editor + codegen-to-Rust.
- Material graph editor + Slang fragment codegen.
- Effects/VFX graph + GPU compute particles.
- Render graph editor (read-only viewer first).
- Skeletal animation + state machines.
- Audio (spatial, mixer) on its own realtime thread bridged to ECS via SPSC.
- Spatial indexing, NavMesh.
- Profiler UI.
- Windows + D3D12 backend, Linux + Vulkan backend (if not already shipped at MVP exit).

## Long-term — Harmonius parity horizon (`plans/long-term.md`)

Match the full feature scope mined from harmonius under a coherent architecture (re-derived, not
ported). Drives:

- AI (behavior trees, perception, planners, navigation, steering / crowds).
- Networking (QUIC replication, prediction / rollback, relevancy).
- Collaborative editing, asset versioning, build farm.
- Procedural generation, cinematics / timeline.
- Advanced rendering (DDGI, RTGI, volumetrics, hair, water).
- Full game-framework primitives (containers, graphs, tables, attributes, grids, timelines, event
  logs).
- iOS + Android backends.
- Accessibility / localization core services.

Path mid-term → long-term is incremental crate addition. SRP discipline keeps the core untouched.
