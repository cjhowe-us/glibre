# MVP Plan

The smallest end-to-end engine that proves the architecture. macOS only.
All work tracked in GitHub Issues; this file maps phases to epics.

## Exit Criteria

1. `glibre-editor.app` opens, loads sample scene, plays + pauses.
2. Hybrid-RT shadow visible in Xcode GPU capture.
3. Hot-reload swaps a gameplay plugin without state loss.
4. Determinism: physics fixture byte-equal across 10 runs.
5. All MVP user-story acceptance tests green.
6. All MVP epics closed; `MVP` milestone 100%.

## Bring-up DAG

```text
infra-repo
   ↓
core (ECS + plugin + reload + frame loop)
   ↓
platform (SDL3 + metal-cpp surface) ────┐
   ↓                                    │
data (Fory schemas + reflection)        │
   ↓                                    │
shader (slangc + reflection)            │
   ↓                                    │
render (graph + Metal 4 + mesh-shader gbuffer + RT shadow)
   ↓
geometry (meshlets) → content (FBX import) → physics (Jolt)
   ↓
tools (editor shell)
   ↓
e2e (replay harness + golden assertions)
   ↓
MVP milestone
```

## Epics (GitHub Issues)

Each epic owns sub-issues for tasks + user stories. Issue numbers filled
in once GitHub repo is provisioned.

| # | Context  | Epic                                              | Issue |
|---|----------|---------------------------------------------------|-------|
| 1 | infra    | Repo, CI, vcpkg, CMake bootstrap                  | TBD   |
| 2 | core     | ECS + plugin loader + hot-reload + frame loop     | TBD   |
| 3 | platform | SDL3 window/input + metal-cpp surface + watcher   | TBD   |
| 4 | data     | Apache Fory schemas + reflection + migration      | TBD   |
| 5 | shader   | Slang → slangc → metallib + reflection            | TBD   |
| 6 | render   | Render graph + Metal 4 backend + mesh-shader gbuffer | TBD |
| 7 | render   | Hybrid-RT shadow pass                             | TBD   |
| 8 | geometry | meshoptimizer meshlets + Draco compression + pak  | TBD   |
| 9 | content  | FBX SDK + FreeImage + FreeType + CAS              | TBD   |
|10 | physics  | Jolt rigid bodies + deterministic config          | TBD   |
|11 | tools    | Editor shell + scene tree + inspector + gizmo     | TBD   |
|12 | e2e      | InputDriver + trace replay + golden assertions    | TBD   |

## Cross-cutting story imports

Initial user-story batch mined from harmonius `US-*` candidates;
each survivor becomes a `type:user-story` issue phase-tagged
`phase:mvp`. See `plans/post-mvp.md` and `plans/long-term.md` for
stories deferred beyond MVP.
