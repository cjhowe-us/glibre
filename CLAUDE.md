# Claude Code Instructions

## Project

Glibre — Rust no-code game engine. Cross-platform (macOS, Windows,
Linux, iOS, Android). Plugin-style: codegen middleman `.dylib` hot
reload in editor; statically linked under LTO for shipping. Built on
the harmonius design substrate (re-derived, not ported).

## Workflow

- **Spec → story → test → code.** Stories and tasks live in **GitHub
  Issues**, not in this repo. Always reference issues from PR bodies.
- **Six issue types**: initiative → epic → sub-epic → { plan | spike };
  user-story is orthogonal. Aggregators carry no estimate; leaves do
  (except spikes, which carry no estimate either). No roles, no kinds.
  See `AGENTS.md`.
- **Always create issues via templates** in `.github/ISSUE_TEMPLATE/`.
  Bodies created programmatically must mirror the template's section
  structure.
- **Tests by type**: `user-story` → manual test script + `.glibre-trace`
  E2E. `plan` → cargo unit tests (once coding re-enables). `spike` →
  deliverable doc / decision (no tests).
- **Story closure**: E2E test must be green in CI before any manual
  testing. Story closes only after manual test PASS recorded as a
  comment. Stories drive the test suite — never close prematurely.
- **Plan execution via foreground role subagents**: invoke `/work` to
  pick one unblocked leaf at a time. Caller-decides parallelism —
  invoke `/work` multiple times in a single tool-call message for
  parallel leaves; each `/work` itself stays single-leaf.
- **Seven role agents**: `product`, `design`, `plan`, `code`, `test`,
  `review`, `chore`. `code` flattens implementation + review-response
  via `stage:implement` / `stage:respond`. `review` is dispatched only
  by `/review-respond`.
- **Progress tracked in GitHub issue comments (English)**. No on-disk
  progress logs. Tracking issues are the source of truth.
- **Review/respond runs until resolved** via the `/review-respond`
  skill. No fixed pass count. Author agent re-dispatches on its own
  artifact (code PR / design PR / plan issue / doc PR) until reviewer
  verdict is `APPROVE` or the loop escalates to an `[SPIKE] iterate-*`
  issue.
- **No time estimates.** Story points only (Fibonacci 1/2/3/5/8). > 8
  → split. Story points roll up from leaves; aggregators (initiative
  / epic / sub-epic) and spikes never carry `pts:*`.
- **Cohesion AND completeness.** Abstractions are the foundation
  completeness rests on. Reject the false trade-off.
- **SOLID, SRP first.** One responsibility per module. Two reasons to
  change → split.
- **Greatly reduced MVP scope.** Defer post-MVP / long-term
  aggressively.
- **Conventional Commits** on every commit and PR subject:
  `feat(scope):`, `fix(scope):`, `test(scope):`, `refactor(scope):`,
  `perf(scope):`, `chore(scope):`, `docs(scope):`, `build(scope):`,
  `ci(scope):`. Granular PRs; one task issue may have many.
- **`/think` is a meta-modifier.** Invoke before any reasoning-bound
  decision (hard root cause, refutation pass, ambiguous spec point,
  scope-split judgement). Boosts the next reasoning turn to
  `model:opus`, `effort:xhigh`, extended-thinking on, with the
  ranked-hypothesis output contract.

## Storage of artifacts

- **Plans** (task breakdowns, leaf issues, stories, spikes,
  aggregators) live in **GitHub Issues only**. No plan markdown
  bodies in the repo. `plans/*.md` are index pointers to issues, not
  plan content.
- **Designs** (specs, ADRs, integration contracts, decision records,
  diagrams) live in **the repository** under `specs/` and
  `specs/decisions/`.
- Any change that invalidates an existing design must update the
  affected design files in the same PR. If scope blocks that, open an
  `[SPIKE] iterate-*` issue first.

## Tech Stack (locked)

Rust stable (1.80+), Cargo workspace `resolver = "3"`, bundled
`rustc` + `cargo` for editor middleman hot reload, LTO static link
for shipping. Custom archetype ECS with AoSoA tiled storage. Custom
Chase-Lev work-stealing job system on `crossbeam-deque` /
`crossbeam-channel` / `crossbeam-utils`. Graphics: Metal 4 (Apple)
via `objc2-metal`, Direct3D 12 (Windows) via `windows-rs`, Vulkan 1.4
(Linux/Android) via `ash`. Mesh shaders + ray tracing baseline.
Shaders: Slang → `slangc` → metallib / SPIR-V / DXIL (single IL, single compiler, native reflection drives bindings + permutations).
Windowing: NSWindow via `objc2-app-kit`, Win32 via `windows-rs`, X11
via `x11rb`, Wayland via `wayland-client`. Platform I/O: `io_uring`
via `rustix` (Linux), IOCP + DirectStorage via `windows-rs` (Windows),
GCD `dispatch_io` + Metal I/O via `dispatch2` + `objc2` (Apple).
Serialization: `rkyv` only. Networking: QUIC unified
(`quinn-proto`/`Networking.framework`/MsQuic). Math: `glam`,
`smallvec`, `bytemuck` / `zerocopy`.

## Repo Layout

- `Cargo.toml` — empty workspace (`members = []`) until coding
  re-enables.
- `specs/<context>/SPEC.md` — bounded-context specs (15 contexts).
- `specs/decisions/*.md` — ADRs.
- `reviews/iter-N/` — review-iteration artifacts.
- `plans/{mvp,post-mvp,long-term}.md` — GitHub-issue index pointers.

## Harmonius

`/Users/cjhowe/Code/harmonius/` is unreliable prior art (older
model). Treat as research input only. Re-derive every conclusion. Do
not port. Glibre inherits the substrate (Rust, custom ECS + jobs,
HLSL pipeline, `rkyv`, codegen middleman, no async, no reflection);
specific design decisions are re-derived per context.

## Don'ts

- No `docs/` folder.
- No filesystem-stored stories or task lists.
- No time estimates.
- No mid-frame hot-reload.
- No runtime reflection in shipping builds (no `dyn Reflect`,
  `TypeRegistry`, `TypeId` dispatch).
- No serialized render-graph files (render graph is Rust code).
- No C, C++, Objective-C, Objective-C++, Swift in engine, runtime,
  editor, or tools.
- No `metal-cpp`. Use `objc2-metal`.
- No `async` / `await`, `Future`, `tokio`, `mio`, `rayon`, `compio`
  in engine or game runtime. Async permitted in backend services
  only.
- No `serde` or non-`rkyv` binary serialization.
- No `winit`, SDL, glfw. Custom windowing only.
- No `HashMap` on deterministic hot paths.
- No singletons. Dependency injection only.
- No mocking libraries or mock objects in tests.
- No nightly Rust features.
