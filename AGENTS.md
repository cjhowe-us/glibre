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
   / plans / spikes. No estimate. SRP-bounded slice.
3. **Sub-Epic** (`type:sub-epic`) — nested aggregator under an epic
   (or another sub-epic). No estimate.
4. **User Story** (`type:user-story`) — persona-grounded testable
   acceptance criterion. **Tests required: manual test script + E2E
   trace.** Has its own estimate.
5. **Plan** (`type:plan`) — leaf; closes with **exactly one PR**.
   Conventional Commit subject pre-declared on the issue.
   **Tests required: unit tests (Catch2).** Has estimate.
6. **Spike** (`type:spike`) — time-boxed research / design. Output =
   doc, decision record, or prototype branch. No tests required.
   Has estimate.

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
