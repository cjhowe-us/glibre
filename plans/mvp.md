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

## Cross-cutting initiatives

### EASTL removal (initiative #1032)

<!-- This section uses bullet prose because the dependency graph is enumerable
     rather than chronological — the Bring-up DAG ASCII above is reserved for
     time-ordered milestones. Cross-cutting initiatives use bullet prose rather
     than the Epics table format because each child plan needs a one-line scope
     description that the table's column shape can't accommodate without
     truncation. -->

Engine-wide reversal of PHILOSOPHY.md §11 (EASTL adoption). Rationale
established in design spike #1033; decision record merged via PR #1034
(`reviews/decisions/eastl-removal.md`). Every `#include <EASTL/...>` is
replaced with the libc++ stdlib equivalent (`std::pmr::*` for engine code,
plain `std::*` for tools). Breakdown spike #1037 partitioned the migration
into 16 per-directory `[PLAN]` leaves + 2 build-cleanup chores:

- core production: #1040 (error/variant keystone), #1041 (alloc), #1042
  (plugin-manifest), #1043 (plugin-loader), #1044 (plugin-loader-registry),
  #1045 (phase-registry), #1046 (context-tag-resolver)
- plugins / tools: #1047 (plugin/shader), #1048 (tools/foryc)
- tests: #1049 (tests/core/error — blocked_by #1040), #1050 (tests/core/plugin),
  #1051 (tests/core/runtime), #1052 (tests/data), #1053 (tests/perf),
  #1054 (tests/shader), #1055 (tests/tools/foryc)
- build cleanup (blocked_by every migration plan above): #1056
  (vcpkg-drop-eastl), #1057 (cmake-drop-eastl-link)

## Cross-cutting story imports

Initial user-story batch mined from harmonius `US-*` candidates;
each survivor becomes a `type:user-story` issue phase-tagged
`phase:mvp`. See `plans/post-mvp.md` and `plans/long-term.md` for
stories deferred beyond MVP.
