# Claude Code Instructions

## Project

Glibre — C++23/26 no-code game engine. macOS-first. Plugin-based. SDL3 + Metal 4 (via metal-cpp) +
Slang. macOS 26 / Apple Silicon baseline.

## Workflow

- **Spec → story → test → code.** Stories and tasks live in **GitHub
  Issues**, not in this repo. Always reference issues from `plans/*.md`
  and PR bodies.
- **Six issue types**: initiative → epic → sub-epic → { plan | spike };
  user-story is orthogonal. Aggregators carry no estimate; leaves do
  (except spikes, which carry no estimate either). No roles, no kinds.
  See `AGENTS.md`.
- **Always create issues via templates** in `.github/ISSUE_TEMPLATE/`.
  Bodies created programmatically must mirror the template's section
  structure.
- **Tests by type**: `user-story` → manual test script + E2E trace.
  `plan` → unit tests. `spike` → deliverable doc / decision (no tests).
- **Story closure**: E2E test must be green in CI before any manual
  testing. Story closes only after manual test PASS recorded as a
  comment. Stories drive the test suite — never close prematurely.
- **Plan execution via parallel nested subagents**: ≤5 concurrent
  top-level plan executors; children may also be parallel. Each
  generated plan must fit in a single Claude Code session.
- **Progress tracked in GitHub issue comments (English)**. No on-disk
  progress logs. Tracking issues are the source of truth.
- **≥3 passes** for every plan / design / issue set before leaves
  execute (coverage → cohesion+SRP → topology+estimation).
- **No time estimates.** Story points only (Fibonacci 1/2/3/5/8). > 8 → split.
  Story points roll up from leaves; aggregators (initiative / epic /
  sub-epic) and spikes never carry `pts:*`.
- **Cohesion AND completeness.** Abstractions are the foundation
  completeness rests on. Reject the false trade-off.
- **SOLID, SRP first.** One responsibility per module. Two reasons to
  change → split.
- **Greatly reduced MVP scope.** Defer post-MVP / long-term aggressively.
- **Conventional Commits** on every commit and PR subject:
  `feat(scope):`, `fix(scope):`, `test(scope):`, `refactor(scope):`,
  `perf(scope):`, `chore(scope):`, `docs(scope):`, `build(scope):`,
  `ci(scope):`. Granular PRs; one task issue may have many.

## Tech Stack (locked)

C++23, CMake ≥ 4.3.0 + Ninja, vcpkg manifest mode, clang ≥ 21, SDL3, Metal 4 via metal-cpp, Slang,
Jolt, meshoptimizer, Draco, Flatbuffers, FreeImage, HarfBuzz, FBX SDK, Catch2, spdlog. Containers /
strings / smart pointers / `optional` / `variant` / `tuple` / `function` use libc++ (`std::*` /
`std::pmr::*`); per-context allocation via `std::pmr::polymorphic_allocator<T>` over
`glibre::PerContextAllocatorResource` (see `reviews/decisions/eastl-removal.md` and PHILOSOPHY.md §11).

## Repo Layout

- `core/` — minimal core hub (ECS, plugin loader, hot-reload, frame loop).
- `plugins/<name>/` — each domain ships as its own `.dylib`.
- `tools/` — `glibre-editor`, `glibre-cook`, `glibre-codegen`.
- `runtime/` — shipping game runtime entry.
- `shaders/` — Slang.
- `tests/` — Catch2.
- `specs/<context>/SPEC.md` — bounded-context specs.
- `plans/{mvp,post-mvp,long-term}.md` — phase plans linking issues.
- `reviews/iter-N/` — review-iteration artifacts.

## Harmonius

`/Users/cjhowe/Code/harmonius/` is unreliable prior art (older model).
Treat as research input only. Re-derive every conclusion. Do not port.

## Don'ts

- No `docs/` folder.
- No filesystem-stored stories or task lists.
- No time estimates.
- No mid-frame hot-reload.
- No runtime reflection in shipping builds.
- No serialized render-graph files (render graph is C++ code).
- No Obj-C++ in engine code (use metal-cpp).
