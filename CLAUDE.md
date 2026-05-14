# Claude Code Instructions

## Project

Glibre — Rust no-code game engine. Cross-platform (macOS, Windows, Linux, iOS, Android).
Plugin-style: codegen middleman `.dylib` hot reload in editor; statically linked under LTO for
shipping. Built on the harmonius design substrate (re-derived, not ported).

## Commands

- Build: `cargo build --workspace`
- Test: `cargo test --workspace`
- Lint: `cargo clippy --workspace -- -D warnings`
- Format: `cargo fmt --all -- --check`
- Issue / PR ops: `gh` CLI; no on-disk progress logs.

## Workflow

- **Spec → story → test → code.** Stories and tasks live in **GitHub Issues**, not in this repo.
  Always reference issues from PR bodies.
- **Six issue types** (see § Issue Types below): initiative → epic → sub-epic → { plan | spike};
  user-story is orthogonal. Aggregators carry no estimate; leaves do (except spikes, which carry no
  estimate). No roles, no kinds.
- **Always create issues via templates** in `.github/ISSUE_TEMPLATE/` (`initiative.yml`, `epic.yml`,
  `sub-epic.yml`, `user-story.yml`, `plan.yml`, `spike.yml`). Bodies created programmatically must
  mirror the template's section structure. Bare/blank issues are disabled by
  `.github/ISSUE_TEMPLATE/config.yml`.
- **Tests by type**: `user-story` → manual test script + `.glibre-trace` E2E. `plan` → cargo unit
  tests (`#[test]` / integration; once coding re-enables). `spike` → deliverable doc / decision (no
  tests).
- **Story closure**: E2E test must be green in CI before any manual testing. Story closes only after
  manual test PASS recorded as a comment. Stories drive the test suite — never close prematurely.
- **Plan execution via foreground role subagents**: invoke `/work` to pick one unblocked leaf at a
  time. Caller-decides parallelism — invoke `/work` multiple times in a single tool-call message for
  parallel leaves; each `/work` itself stays single-leaf.
- **Seven role agents**: `product`, `design`, `plan`, `code`, `test`, `review`, `chore`. `code`
  flattens implementation + review-response via `stage:implement` / `stage:respond`. `review` is
  dispatched only by `/review-respond`.
- **Progress tracked in GitHub issue comments (English)**. No on-disk progress logs. Tracking issues
  are the source of truth. Required fields documented in § Status Comment Schema below.
- **Review/respond runs until resolved** via the `/review-respond` skill. No fixed pass count.
  Author agent re-dispatches on its own artifact (code PR / design PR / plan issue / doc PR) until
  reviewer verdict is `APPROVE` or the loop escalates to an `[SPIKE] iterate-*` issue.
- **No time estimates.** Story points only (Fibonacci 1/2/3/5/8). > 8 → split. Story points roll up
  from leaves; aggregators (initiative / epic / sub-epic) and spikes never carry `pts:*`.
- **Cohesion AND completeness.** Abstractions are the foundation completeness rests on. Reject the
  false trade-off.
- **SOLID, SRP first.** One responsibility per module. Two reasons to change → split.
- **Greatly reduced MVP scope.** Defer post-MVP / long-term aggressively.
- **Conventional Commits** on every commit and PR subject: `feat(scope):`, `fix(scope):`,
  `test(scope):`, `refactor(scope):`, `perf(scope):`, `chore(scope):`, `docs(scope):`,
  `build(scope):`, `ci(scope):`. Granular PRs; one task issue may have many.
- **`/think` is a meta-modifier.** Invoke before any reasoning-bound decision (hard root cause,
  refutation pass, ambiguous spec point, scope-split judgement). Boosts the next reasoning turn to
  `model:opus`, `effort:xhigh`, extended-thinking on, with the ranked-hypothesis output contract.

## Issue Types

Hierarchy: **initiative → epic → sub-epic → { plan | spike }**.
User-story is orthogonal (linked from any leaf or aggregator).

1. **Initiative** (`type:initiative`) — top-level. Groups epics. No estimate.
2. **Epic** (`type:epic`) — under an initiative. Aggregates sub-epics / plans / spikes. No estimate.
   SRP-bounded scope.
3. **Sub-Epic** (`type:sub-epic`) — nested aggregator. No estimate.
4. **User Story** (`type:user-story`) — persona-grounded testable acceptance criterion. Tests
   required: manual test script + E2E trace. Has estimate. Closure rules: (1) E2E green in CI before
   manual testing; (2) manual PASS recorded as comment; (3) cannot close until both hold.
5. **Plan** (`type:plan`) — leaf implementation unit. Tests required: unit tests (cargo `#[test]` /
   integration). Has estimate. Closed by one or more granular Conventional Commit PRs.
6. **Spike** (`type:spike`) — time-boxed research / design. Output = doc, decision record, or
   prototype branch. No tests required. No estimate.

Story-point rollup is automatic: aggregators never carry `pts:*`; total = sum of `pts:*` across leaf
descendants.

## Leaf Sizing — One Session, One Leaf

Every leaf issue must be completable inside a single Claude Code session. Heuristics:

- `type:plan` ≤ `pts:5` ideal, `pts:8` hard cap. Larger → split.
- `type:user-story` ≤ `pts:5`. Bigger → split into stories whose acceptance criteria compose.
- `type:spike` produces one decision / prototype / triage document. Doesn't fit one session → split
  into spikes whose deliverables compose.

If a subagent realizes mid-session that a leaf has grown beyond one session, it MUST: (a) check in
via issue comments; (b) split the leaf into smaller leaves + link them; (c) close out the original
session boundary cleanly without committing half-done work.

## Status Comment Schema

All work on a leaf issue is documented via comments on that issue. **Issues are the log.** No
on-disk status files.

Required fields on every status comment:

```text
agent:<role-name>            # product | design | plan | code | test | review | chore
status:<started|progress|done>
issue:#<n>
branch:<git-branch>
worktree:<absolute-path>
host:<hostname-or-'local'>
cloud:<provider-env-or-'none'>
commit:<sha-if-any-or-'pending'>
pr:#<n>-or-'pending'-or-'none'
notes:<one-paragraph English summary of work performed>
```

Posted at minimum on:

- `started` — when the subagent claims the issue
- `progress` — at meaningful checkpoints (decisions, unexpected splits)
- `done` — when the deliverable is committed; references the commit SHA and any PR

Status `blocked` is forbidden — split + redirect + exit `done` instead. Use this bash heredoc form
so `$BRANCH` / `$WT` / `$(hostname -s)` / `$(git rev-parse HEAD)` expand to resolved values before
the comment is posted; never let literal `$VAR` reach GitHub:

```bash
gh issue comment <N> --body "$(cat <<EOF
agent:<role>
status:done
issue:#<N>
branch:$BRANCH
worktree:$WT
host:$(hostname -s)
cloud:none
commit:$(git -C "$WT" rev-parse HEAD)
pr:#<PR>
notes:<one-paragraph English summary>
EOF
)"
```

## Storage of artifacts

- **Plans** (task breakdowns, leaf issues, stories, spikes, aggregators) live in
  **GitHub Issues only**. No plan markdown bodies in the repo.
- **Designs** (specs, ADRs, integration contracts, decision records, diagrams) live in
  **the repository** under `specs/` and `specs/decisions/`.
- Any change that invalidates an existing design must update the affected design files in the same
  PR. If scope blocks that, open an `[SPIKE] iterate-*` issue first.

## CI gates

Workflows under `.github/workflows/`:

- **dependency check** — PR blocked if its linked issue has open GitHub-native blockers.
- **spec check** — every acceptance criterion maps to a cargo test name.
- **roll-up** — aggregator estimates recomputed from leaf descendants and posted as comments.
- **dod-verify** — fires on `issues:closed`. Verifies the closed issue's `## Definition of Done`
  block (per `.github/DOD-DSL.md`) against the merged diff. Manual trigger: `/verify-dod` comment.

## Tech Stack (locked)

Rust stable (1.80+), Cargo workspace `resolver = "3"`, bundled `rustc` + `cargo` for editor
middleman hot reload, LTO static link for shipping. Custom archetype ECS with AoSoA tiled storage.
Custom Chase-Lev work-stealing job system on `crossbeam-deque` / `crossbeam-channel` /
`crossbeam-utils`. Graphics: Metal 4 (Apple) via `objc2-metal`, Direct3D 12 (Windows) via
`windows-rs`, Vulkan 1.4 (Linux/Android) via `ash`. Mesh shaders + ray tracing baseline. Shaders:
Slang → `slangc` → metallib / SPIR-V / DXIL (single IL, single compiler, native reflection drives
bindings + permutations). Windowing: NSWindow via `objc2-app-kit`, Win32 via `windows-rs`, X11 via
`x11rb`, Wayland via `wayland-client`. Platform I/O: `io_uring` via `rustix` (Linux), IOCP +
DirectStorage via `windows-rs` (Windows), GCD `dispatch_io` + Metal I/O via `dispatch2` + `objc2`
(Apple). Serialization: `rkyv` only. Networking: QUIC unified
(`quinn-proto`/`Networking.framework`/MsQuic). Math: `glam`, `smallvec`, `bytemuck` / `zerocopy`.

## Repo Layout

- `Cargo.toml` — empty workspace (`members = []`) until coding re-enables.
- `specs/<context>/SPEC.md` — bounded-context specs (15 contexts).
- `specs/decisions/*.md` — ADRs.
- `.claude/agents/<role>.md` — 7 role agents.
- `.claude/skills/{work,think,review-respond}/SKILL.md` — 3 skills.

## Harmonius

`/Users/cjhowe/Code/harmonius/` is unreliable prior art (older model). Treat as research input only.
Re-derive every conclusion. Do not port. Glibre inherits the substrate (Rust, custom ECS + jobs,
HLSL pipeline, `rkyv`, codegen middleman, no async, no reflection); specific design decisions are
re-derived per context.

## Don'ts

- No `docs/` folder.
- No filesystem-stored stories or task lists.
- No time estimates.
- No mid-frame hot-reload.
- No runtime reflection in shipping builds (no `dyn Reflect`, `TypeRegistry`, `TypeId` dispatch).
- No serialized render-graph files (render graph is Rust code).
- No C, C++, Objective-C, Objective-C++, Swift in engine, runtime, editor, or tools.
- No `metal-cpp`. Use `objc2-metal`.
- No `async` / `await`, `Future`, `tokio`, `mio`, `rayon`, `compio` in engine or game runtime. Async
  permitted in backend services only.
- No `serde` or non-`rkyv` binary serialization.
- No `winit`, SDL, glfw. Custom windowing only.
- No `HashMap` on deterministic hot paths.
- No singletons. Dependency injection only.
- No mocking libraries or mock objects in tests.
- No nightly Rust features.
- No `status:blocked` comments — split + redirect + exit `done`.
- No `gh issue close` by agents — closure happens via merged PR + `Closes #N` + `dod-verify` green.
