# Agent Execution Model

Plans are executed by parallel nested subagents. Progress is tracked
exclusively through GitHub issue comments (English).

## Issue Types (six — no tracking, no roles, no kinds)

Hierarchy: **initiative → epic → sub-epic → { plan | spike }**.
User-story is orthogonal (linked from any leaf or aggregator).

Aggregators carry **no estimate**; leaves carry estimates that roll up.

1. **Initiative** (`type:initiative`) — top-level. Groups epics. No
   estimate. Source of truth for top-level plan execution status.
2. **Epic** (`type:epic`) — under an initiative. Aggregates sub-epics
   / plans / spikes. No estimate. SRP-bounded scope.
3. **Sub-Epic** (`type:sub-epic`) — nested aggregator under an epic
   (or another sub-epic). No estimate.
4. **User Story** (`type:user-story`) — persona-grounded testable
   acceptance criterion. **Tests required: manual test script + E2E
   trace.** Has its own estimate. **Closure rules**:
   1. E2E test must be authored and **passing in CI** before any human
      manual testing begins.
   2. Manual test must be executed; PASS recorded as an issue comment.
   3. Story may not be closed until both (1) and (2) hold.
5. **Plan** (`type:plan`) — leaf implementation unit.
   **Tests required: unit tests (Catch2).** Has estimate. Closed by
   one or more granular Conventional Commit PRs.
6. **Spike** (`type:spike`) — time-boxed research / design. Output =
   doc, decision record, or prototype branch. No tests required.
   **No estimate** — spikes do not carry story points.

Story-point rollup is automatic: tracking issues never carry a
`pts:*` label; their total = sum of `pts:*` across leaf descendants.

## Concurrency

- **≤ 5 concurrent top-level plan-executor subagents** at any time.
- Each top-level executor MAY spawn its own child subagents in parallel
  (no cap on the second tier; budget governed by sensible saturation).
- Top-level executors are launched in a single message with multiple
  `Agent` tool calls.

## Leaf Sizing — One Session, One Leaf

Every leaf issue (`type:user-story`, `type:plan`, `type:spike`) must
be **completable inside a single Claude Code session**. No leaf is
allowed to carry a major chunk of work without an intermediate
checkpoint. Practical heuristics:

- `type:plan` ≤ pts:5. Larger scopes split into multiple plans.
- `type:user-story` ≤ pts:5. Bigger stories split into smaller ones
  whose acceptance criteria compose.
- `type:spike` produces one decision record / prototype / triage
  document — no estimate; if it would not fit one session, split into
  smaller spikes whose deliverables compose.

If a subagent realizes mid-session that a leaf has grown beyond one
session, it must: (a) check in via the issue comments, (b) split the
leaf into smaller leaves and link them, (c) close out the original
session boundary cleanly without committing half-done work.

## Status Communication (in issue comments)

Subagents post structured status to the issue they are working on:

```
agent:<short-name> status:<started|progress|blocked|done> issue:#N
notes:<one-paragraph English summary>
```

The parent tracking issue receives roll-up comments from the orchestrator
when child status changes. No status lives on disk — issues are the log.

## Three-Pass Authoring (mandatory)

Every plan / design / issue set goes through **at least three passes**
before any leaf-level execution starts. Each pass runs as parallel
review subagents over the prior pass's output, then a distillation
turn updates plans, designs, and issues.

| Pass | Goal                                                    |
|------|---------------------------------------------------------|
| 1    | Coverage — are all needed stories + tasks present?      |
| 2    | Cohesion + SRP — are seams clean? Any duplication?      |
| 3    | Topology + estimation — DAG correctness, point sanity   |

Each pass produces a `reviews/iter-N/<perspective>.md` file plus a
distilled `learnings.md` and `addressed.md`. Issues are created /
updated to reflect the addressed findings before the next pass starts.

Implementation begins **only after Pass 3 closes**.

## Tooling

- `gh issue create` / `gh issue comment` — agents use these for all
  status updates. No file-based progress logs.
- CI gates (workflows under `.github/workflows/`):
  - dependency check — PR blocked if its linked issue has open
    GitHub-native blockers.
  - spec check — every acceptance criterion maps to a Catch2 test.
  - roll-up — aggregator estimates recomputed from leaf descendants
    and posted as comments.
