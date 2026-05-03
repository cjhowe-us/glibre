---
name: advance-plan
description: This skill should be used when the user asks to "advance the plan", "pick up unblocked items", "what should I work on", "next batch of work", "make progress on issues", "start parallel agents", "drive the spec to completion", or any variation of "keep going on the plan". It queries GitHub for up to 5 unblocked leaf issues, dispatches one background subagent per item to run the appropriate SDLC stage (design / planning / implementation / QA / integration / maintenance), and reports the resulting PRs back through GitHub issue comments.
version: 0.1.0
---

# Advance Plan

Drive the glibre repo's plan forward by picking up to **5 unblocked
leaf issues** from GitHub, then dispatching one background subagent
per item to perform the next SDLC stage for that item. Track all
state through GitHub issue comments — never on disk.

## Invariants

Every dispatch must respect these (also enforced by `AGENTS.md`):

- **≤ 5 concurrent top-level subagents.** Children unbounded.
- **No on-disk state.** Status, progress, decisions: GitHub comments
  on the relevant issue.
- **PRs only on `main`.** Every code/doc change goes via Pull Request
  with a Conventional Commit subject.
- **One leaf, one session.** A `type:user-story`, `type:plan`, or
  `type:spike` must complete in a single session. If not, the agent
  splits the leaf into smaller leaves and posts the split.
- **Aggregators carry no estimate.** Story points only on
  `type:user-story` and `type:plan`.
- **Issues stay open until deliverables are merged into `main`.**
  Closing a leaf requires the linked PR(s) merged + (for stories)
  manual PASS recorded as a comment.

## Required Step Order

1. **Query** GitHub for unblocked leaves; collect up to 5.
2. **Filter** by SDLC stage: pick items in earliest open stage first
   (ideation → maintenance — see `references/sdlc.md`).
3. **Dispatch** one background subagent per picked item with the
   stage-specific prompt template from `references/dispatch-prompts.md`.
4. **Wait** for completion notifications (do not poll). On each
   completion, verify the agent posted the required status comment,
   verify the PR (if any) was opened, then optionally pick the next
   unblocked item to keep concurrency at ≤ 5.

## Step 1 — Query Unblocked Leaves

Run the GraphQL recipe documented in `references/github-recipes.md`.
The query returns open issues with zero open `blockedBy` edges and
type ∈ {`type:user-story`, `type:plan`, `type:spike`}.

Pick at most 5. Prefer:

- Earliest SDLC stage that is currently open in the project (e.g. if
  no spec spike has filled §4 of any context yet, prioritize
  `design-*-aggregates` spikes over later stages).
- Items whose closure unblocks the largest downstream subtree
  (rough heuristic: spikes with many `blocking` edges).

Topology is intrinsic to the GitHub dependency graph. Do not store
ordered lists in the repo — recompute every invocation.

## Step 2 — Match Item to SDLC Stage

Map issue title and labels to a stage. The mapping is:

| Issue pattern                              | Stage          | Output kind                                |
|--------------------------------------------|----------------|--------------------------------------------|
| `[SPIKE] research-*-responsibilities`      | ideation       | Decision record + spec §1, §2 fill         |
| `[SPIKE] research-*-harmonius-mining`      | ideation       | Decision record + spec §3 fill             |
| `[SPIKE] design-*-aggregates`              | design         | Spec §4 fill                                |
| `[SPIKE] design-*-public-interface`        | design         | Spec §5 (header stub)                       |
| `[SPIKE] design-*-persistence-schemas`     | design         | Spec §7 + Fory schema sketch                |
| `[SPIKE] design-*-hot-reload-contract`     | design         | Spec §8                                     |
| `[SPIKE] design-*-internal-architecture`   | design         | Spec §6                                     |
| `[SPIKE] design-*-perf-budget`             | design         | Spec §9                                     |
| `[SPIKE] design-*-failure-modes`           | design         | Spec §10                                    |
| `[SPIKE] draft-*-user-stories`             | breakdown      | New `type:user-story` issues + spec §11    |
| `[SPIKE] close-*-open-questions`           | iteration      | Resolutions in spec §12                     |
| `[SPIKE] task-breakdown-*-implementation`  | planning       | New `type:plan` issues parented to epic    |
| `[SPIKE] decide-*` (E0)                    | design         | Cross-cutting decision record               |
| `[USER-STORY] *`                           | testing + QA   | Manual test script + E2E trace             |
| `[PLAN] *`                                 | implementation | Code + unit tests + PR                      |

Detailed SDLC for each stage lives in `references/sdlc.md`. Stage-
specific subagent prompts live in `references/dispatch-prompts.md`.

## Step 3 — Dispatch Subagents

Use the Agent tool with `run_in_background: true`. The skeleton:

```
Agent({
  description: "<short title>",
  subagent_type: "general-purpose",
  run_in_background: true,
  prompt: <stage-prompt with item-specific variables substituted>
})
```

Send all picks in **a single tool-call message** so they start in
parallel.

The stage prompt MUST include:

1. The issue number and title.
2. Required reads: `PHILOSOPHY.md`, `AGENTS.md`, the relevant
   `reviews/decisions/*.md` already committed, and the parent
   sub-epic / epic / initiative bodies.
3. The deliverable contract (what file(s) and what content).
4. The status-comment schema from `AGENTS.md` (must include
   `branch / worktree / host / cloud / commit / pr / notes`).
5. The closure rule for that issue type (PR merged → close; story
   needs E2E green + manual PASS; spike needs deliverable merged).
6. The PR-only rule: subagent creates a feature branch, opens a PR
   with a Conventional Commit subject, sets auto-merge if applicable,
   never pushes directly to `main`.

See `references/dispatch-prompts.md` for the full templates.

## Step 4 — Verify and Continue

When a notification arrives:

1. Read the agent's summary message.
2. Check the issue's latest comment matches the required schema. If
   not, post a follow-up comment that documents the agent's run
   (using gh CLI) so the audit trail is intact.
3. Check the PR opened by the agent. If auto-merge is enabled and CI
   is green for non-critical paths, the merge will happen on its own.
   If the PR touches critical paths, mention this so the user can
   approve manually.
4. Pick the next unblocked leaf to keep ≤ 5 concurrent.

Stop dispatching when one of:

- Total open unblocked leaves becomes 0.
- The user asks to stop.
- A completion run reports a structural problem (e.g. agent split a
  leaf — the new leaves need to be triaged before continuing).

## Reference Files

- **`references/sdlc.md`** — full SDLC: ideation, breakdown, design,
  planning, testing, implementation, review, iteration, QA,
  integration, maintenance. One section per stage with concrete
  glibre-specific instructions.
- **`references/dispatch-prompts.md`** — prompt templates per stage.
  Substitute `{{ISSUE_NUMBER}}`, `{{ISSUE_TITLE}}`,
  `{{CONTEXT_NAME}}`, `{{SPEC_PATH}}`, `{{DECISION_PATHS}}` before
  dispatching.
- **`references/github-recipes.md`** — gh CLI + GraphQL queries for
  unblocked leaves, dependency lookup, status-comment posting, PR
  creation, auto-merge enable, and label diff.
