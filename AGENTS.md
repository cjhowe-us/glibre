# Agent Execution Model

Plans are executed by parallel nested subagents. Progress is tracked
exclusively through GitHub issue comments (English).

## Issue Kinds (only three)

1. **Tracking** (`type:tracking`) — non-leaf. Aggregates child issues.
   **No own story-point estimate**; estimate = sum of leaf descendants.
   Source of truth for plan execution status. Updated by subagents
   posting comments as work progresses. Roles (via `role:*` label):
   - `role:epic` — full context slice (largest)
   - `role:plan` — one Claude Code session's worth of work
   - `role:slice` — mid-level grouping under an epic
   - `role:review` — review-iteration tracker
2. **User Story** (`type:user-story`) — testable acceptance criterion;
   leaf for testing purposes. Has its own story-point estimate.
3. **Task Execution** (`type:task-execution`) — leaf unit of work.
   Kind label: `kind:implementation`, `kind:design`, `kind:planning`,
   `kind:bug`, or `kind:chore`. Has its own story-point estimate.

Story-point rollup is automatic: tracking issues never carry a
`pts:*` label; their total = sum of `pts:*` across leaf descendants.

## Concurrency

- **≤ 5 concurrent top-level plan-executor subagents** at any time.
- Each top-level executor MAY spawn its own child subagents in parallel
  (no cap on the second tier; budget governed by sensible saturation).
- Top-level executors are launched in a single message with multiple
  `Agent` tool calls.

## Plan Size

- Each generated plan must be **completable in a single Claude Code
  session** (rough budget: ≤ ~30 leaf issues, ≤ ~100 story points
  total). Larger scopes split into multiple plans, each with its own
  tracking issue.
- A "plan" is therefore: one tracking issue + its leaf descendants +
  the design/spec docs that motivate them.

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
- `scripts/dep-check.sh` — CI gate: PRs blocked if their task issue
  has open dependencies.
- `scripts/spec-check.sh` — CI gate: every acceptance criterion maps
  to a Catch2 test.
- `scripts/rollup.sh` (TBD) — recomputes tracking-issue point totals
  from leaf descendants and posts the result as a comment.
