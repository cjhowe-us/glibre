---
name: go
description: This skill should be used when the user asks to "advance the plan", "pick up unblocked items", "what should I work on", "next batch of work", "make progress on issues", "start parallel agents", "drive the spec to completion", or any variation of "keep going on the plan". It queries GitHub for up to 2 unblocked leaf issues, dispatches one specialized background subagent per item to run the appropriate SDLC stage (ideation / breakdown / design / planning / testing / implementation / review / iteration / qa / integration / maintenance), and reports the resulting PRs back through GitHub issue comments.
version: 0.1.0
---

# Advance Plan

Drive the glibre repo's plan forward by picking up to **2 unblocked
leaf issues** from GitHub, then dispatching one **specialized**
background subagent per item to perform the next SDLC stage for that
item. Each bucket pins its own model + reasoning effort (see Step 3).
Track all state through GitHub issue comments — never on disk.

## Invariants

Every dispatch must respect these (also enforced by `AGENTS.md`):

- **≤ 2 concurrent top-level subagents.** Children unbounded.
  (Lowered from 5 to 2 so each top-level slot has headroom for a
  high-effort opus bucket without thrashing the harness.)
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
- **Definition of Done is authoritative.** Every leaf carries a
  machine-checkable `## Definition of Done` block (DSL in
  `.github/DOD-DSL.md`). The `dod-verify` GitHub Action evaluates the
  block against `main` whenever the issue is closed (or on
  `/verify-dod` comment). Failure → action reopens the issue and tags
  `dod:failed`. Success → tags `dod:verified`. Closure is the
  verifier's verdict, not human discretion. Authoring agents
  (planning bucket) populate the DoD block; executor agents must
  satisfy it before requesting closure; the orchestrator only closes
  a leaf after Step 4's verifier-check passes.

## Required Step Order

0. **Manual orchestrator hook (optional, before the leaf picker).**
   If the user said "orchestrate #N" / "drive #N end-to-end", or an
   open issue carries the `orchestration:multi-stage` label, dispatch
   a single `go-orchestrator` against that issue and stop the leaf
   picker for this turn. Orchestrator counts as one top-level slot;
   its nested children do not. If neither trigger applies, fall
   through to Step 1.
1. **Query** GitHub for unblocked leaves; collect up to 2.
2. **Filter** by SDLC stage: pick items in earliest open stage first
   (ideation → maintenance — see `references/sdlc.md`).
3. **Dispatch** one background subagent per picked item with the
   stage-specific prompt template from `references/dispatch-prompts.md`.
4. **Wait** for completion notifications (do not poll). On each
   completion, verify the agent posted the required status comment,
   verify the PR (if any) was opened, then optionally pick the next
   unblocked item to keep concurrency at ≤ 2.

## Step 1 — Query Unblocked Leaves

Run the GraphQL recipe documented in `references/github-recipes.md`.
The query returns open issues with zero open `blockedBy` edges and
type ∈ {`type:user-story`, `type:plan`, `type:spike`}.

Pick at most 2. Prefer:

- Earliest SDLC stage that is currently open in the project (e.g. if
  no spec spike has filled §4 of any context yet, prioritize
  `design-*-aggregates` spikes over later stages).
- Items whose closure unblocks the largest downstream subtree
  (rough heuristic: spikes with many `blocking` edges).

Topology is intrinsic to the GitHub dependency graph. Do not store
ordered lists in the repo — recompute every invocation.

## Step 2 — Match Item to SDLC Stage

Map issue title and labels to a stage. The mapping is:

| Issue pattern                              | Stage          | Bucket                  | Output kind                                |
|--------------------------------------------|----------------|-------------------------|--------------------------------------------|
| `[SPIKE] research-*-responsibilities`      | ideation       | `go-design`   | Decision record + spec §1, §2 fill         |
| `[SPIKE] research-*-harmonius-mining`      | ideation       | `go-design`   | Decision record + spec §3 fill             |
| `[SPIKE] design-*-aggregates`              | design         | `go-design`   | Spec §4 fill                                |
| `[SPIKE] design-*-public-interface`        | design         | `go-design`   | Spec §5 (header stub)                       |
| `[SPIKE] design-*-persistence-schemas`     | design         | `go-design`   | Spec §7 + Fory schema sketch                |
| `[SPIKE] design-*-hot-reload-contract`     | design         | `go-design`   | Spec §8                                     |
| `[SPIKE] design-*-internal-architecture`   | design         | `go-design`   | Spec §6                                     |
| `[SPIKE] design-*-perf-budget`             | design         | `go-design`   | Spec §9                                     |
| `[SPIKE] design-*-failure-modes`           | design         | `go-design`   | Spec §10                                    |
| `[SPIKE] decide-*` (E0)                    | design         | `go-design`   | Cross-cutting decision record               |
| `[SPIKE] close-*-open-questions`           | iteration      | `go-design`   | Resolutions in spec §12                     |
| `[SPIKE] draft-*-user-stories`             | breakdown      | `go-planning` | New `type:user-story` issues + spec §11    |
| `[SPIKE] task-breakdown-*-implementation`  | planning       | `go-planning` | New `type:plan` issues parented to epic    |
| `[USER-STORY] *` (E2E trace authoring)     | testing        | `go-planning` | `.glibre-trace` + CI hook                   |
| `[USER-STORY] *` (manual execution)        | qa             | `go-qa`       | Manual test PASS/FAIL comment + screenshots |
| `[PLAN] *`                                 | implementation | `go-coding`        | Code + unit tests + PR                      |
| `[PLAN] *` with `kind:bug`                 | maintenance    | `go-coding`        | Fix + tests + PR                            |
| `[CHORE] *` or `[PLAN] *` with `kind:chore` | maintenance   | `go-chore`         | Tiny PR (single-round review carve-out)     |
| (manual: `orchestration:multi-stage` / chat) | orchestration | `go-orchestrator`  | Plan comment + nested dispatches + closure  |
| (manual: "think about #N" / nested call)   | analysis       | `go-thinker`       | Read-only analysis comment / chat reply     |
| (PR review chore — manual dispatch only)   | review         | `go-coding`        | Review comments / merge approval            |
| (epic-closure / cross-context — manual)    | integration    | `go-coding`        | Aggregator body updates + PR                |

**Step-1 reachability note.** The Step-1 GraphQL filter selects only
`type ∈ {user-story, plan, spike}`. Rows tagged `(PR review chore — manual
dispatch only)` and `(epic-closure / cross-context — manual)` above
are NOT reachable from auto-pick — they describe manual dispatches
the user invokes from chat (e.g. "review PR #123" or "close out epic
#42") which the skill should still route to `go-coding`.
The `kind:bug` row IS reachable since `kind:bug` issues remain
`type:plan`.

For `type:user-story` picks, decide between `testing` (planning bucket)
and `qa` (qa bucket) by inspecting the linked E2E trace's CI status:

```bash
# Strip backticks/parens off the trace path so markdown formatting
# in the issue body doesn't pollute the match.
TRACE=$(gh issue view <N> --json body --jq '.body' \
          | grep -oE 'tests/[^[:space:]`)>]*\.glibre-trace' | head -1)
if [ -n "$TRACE" ] && \
   gh pr list --search "$TRACE is:merged" --state merged \
     --json mergedAt --jq 'length' | grep -q '^[1-9]'; then
  BUCKET=go-qa          # trace merged → manual execution
else
  BUCKET=go-planning    # trace not yet authored / merged
fi
# Fallback: any malformed regex match → BUCKET=go-planning.
```

Detailed SDLC for each stage lives in `references/sdlc.md`. Stage-
specific subagent prompts live in `references/dispatch-prompts.md`.

## Step 3 — Dispatch Subagents

Use the Agent tool with `run_in_background: true`. The skeleton:

```
Agent({
  description: "<short title — issue # + stage>",
  subagent_type: "<bucket from Step-2 table>",
  run_in_background: true,
  prompt: <stage-prompt with item-specific variables substituted>
})
```

**Routing is via agent frontmatter, not dispatch params.** The bucket
name in `subagent_type:` is the only routing key the dispatcher needs
to set. Each agent's frontmatter pins `model:` (opus / sonnet) and
`effort:` (xhigh / high / medium). Do NOT pass `model:` or `effort:`
at dispatch time — the agent file is the source of truth.

Bucket → model + effort (resolved by the agent's own frontmatter):

- `go-orchestrator` → `model: opus`,   `effort: high`
- `go-design`       → `model: opus`,   `effort: xhigh`
- `go-thinker`      → `model: opus`,   `effort: xhigh`
- `go-planning`     → `model: opus`,   `effort: high`
- `go-coding`       → `model: sonnet`, `effort: high`
- `go-review`       → `model: opus`,   `effort: high`
- `go-impl-respond` → `model: sonnet`, `effort: high`
- `go-qa`           → `model: sonnet`, `effort: medium`
- `go-chore`        → `model: haiku`,  `effort: low`

**Concurrency accounting.** Each top-level dispatch counts as one
slot against the `≤ 2 top-level subagents` budget — including
`go-orchestrator` and `go-chore`. An orchestrator's nested children
do NOT count against the budget; fan them out as the dependency
graph allows. `go-thinker` invoked as a nested child of another
agent likewise does not consume a top-level slot; only direct
chat-summoned thinker dispatches do.

**Effort caveat (active fallback in agent body).** As of Claude Code
2.1.x, `effort:` frontmatter is officially honored for plugin-shipped
agents only. For project-level agents under `.claude/agents/` the
runtime may silently ignore it. Each agent body therefore embeds
explicit extended-thinking guidance keyed to its bucket — that is
the active enforcement of reasoning depth today. Do not duplicate
that guidance into per-stage dispatch prompts; the agent body owns it.

Send all picks (up to 2) in **a single tool-call message** so they
start in parallel.

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
   with a Conventional Commit subject, **stops without enabling
   auto-merge** (Step 5's review pipeline owns auto-merge), never
   pushes directly to `main`.

See `references/dispatch-prompts.md` for the full templates.

## Step 4 — Verify Leaf Output and Hand to Review Loop

When a leaf-bucket agent's completion notification arrives:

1. Read the agent's summary message.
2. Check the issue's latest comment matches the required schema and
   cites a PR number. If not, post a follow-up comment that documents
   the agent's run (using `gh CLI`) so the audit trail is intact.
3. Confirm the PR exists and is OPEN with auto-merge **NOT** enabled
   (`gh pr view <N> --json state,autoMergeRequest`). If the leaf agent
   incorrectly enabled auto-merge, disable it with `gh pr merge <N>
   --disable-auto`.
4. Run `git -C /Users/cjhowe/Code/glibre fetch origin main && git -C ...
   pull --ff-only origin main` to keep local main in sync with any
   sibling /go agent's already-merged PRs.
5. **Verify the issue carries a `## Definition of Done` block.** If
   missing, dispatch a `go-planning` follow-up to author it before
   advancing — never close a leaf without a DoD.
6. Hand the PR to **Step 5** (three-round review pipeline). Review
   runs sequentially per PR; across PRs you may parallelise up to the
   ≤ 2 top-level concurrency budget.
7. Optionally pick the next unblocked leaf if a slot is free.

### Step 4b — DoD-gated closure

After all PRs that close a leaf have merged into `main`:

1. The `dod-verify` workflow fires automatically on close events. To
   trigger it manually, comment `/verify-dod` on the issue.
2. If the verifier posts `passed`-of-`total` with all green ticks and
   tags `dod:verified`, the leaf is done.
3. If the verifier reopens the issue with `dod:failed`, treat it as a
   live leaf again: pick the next agent (typically the same bucket
   that produced the original deliverable) to address the failures
   in a follow-up PR. Do not bypass the verifier or hand-close.

The orchestrator MUST NOT close a leaf via `gh issue close` directly;
closure happens by merging a PR whose body uses `closes #<N>`, which
triggers the verifier on the resulting `issues: closed` event.

### Step 4c — Stage transition (advance the parent through SDLC)

A closed leaf is rarely the end of the road for its parent epic /
sub-epic. Most leaves *unlock* the next SDLC stage rather than
*completing* the parent. Whenever a leaf closes with `dod:verified`,
the orchestrator MUST inspect the parent and open + dispatch the
next-stage children if they are missing.

**Transition map** (issue patterns the just-closed leaf produced →
required next-stage child issues, if absent):

| Closed leaf pattern                              | Next-stage child(ren) the parent now needs                    | Bucket to author them   |
|--------------------------------------------------|---------------------------------------------------------------|-------------------------|
| `[SPIKE] research-*-responsibilities` (§1, §2)   | `[SPIKE] research-*-harmonius-mining`                          | `go-design`              |
| `[SPIKE] research-*-harmonius-mining` (§3)       | `[SPIKE] design-*-aggregates`                                  | `go-design`              |
| `[SPIKE] design-*-aggregates` (§4)               | `[SPIKE] design-*-public-interface`                            | `go-design`              |
| `[SPIKE] design-*-public-interface` (§5)         | `[SPIKE] design-*-persistence-schemas`, `*-hot-reload-contract`, `*-internal-architecture`, `*-perf-budget`, `*-failure-modes` (parallel set) | `go-design`              |
| `[SPIKE] design-*-failure-modes` (§10) — last design spike of a context | `[SPIKE] draft-*-user-stories`                                 | `go-planning`            |
| `[SPIKE] design-*-detailed` (per-aggregate)      | `[SPIKE] task-breakdown-*-implementation` (sibling, parented to same sub-epic) | `go-planning`            |
| `[SPIKE] draft-*-user-stories`                   | `[SPIKE] task-breakdown-*-implementation` (one per emitted user-story or per logical work unit) | `go-planning`            |
| `[SPIKE] task-breakdown-*-implementation`        | `[PLAN] *` leaves (already created by the spike — verify each has DoD + named tests + parent link) | n/a (verification only) |
| `[USER-STORY] *` (testing stage closed via merged trace) | (none — story moves to qa)                                     | (the next /go pick fires `go-qa`) |
| `[PLAN] *` (implementation stage closed)         | (verify the parent user-story's E2E + manual test gate; see Step-2 trace heuristic) | n/a                     |

**Procedure on every closed leaf:**

1. Read the parent (sub-issue chain via `gh api repos/.../issues/<N>` →
   `parent_issue_id`).
2. Enumerate the parent's open children (`gh issue list --search
   "parent:<P>" --state open` plus the sub-issue API for native
   children).
3. Compare against the transition-map row for the closed leaf's
   pattern. If a required next-stage child is missing AND no closed
   sibling already produced its deliverable, dispatch the bucket in
   the rightmost column to author the missing issue(s).
4. The authoring agent (typically `go-planning` or `go-design`) opens
   the child issue(s) using the relevant `.github/ISSUE_TEMPLATE/`,
   parents them via the sub-issue API, and (for `[PLAN]` leaves) wires
   `blocked_by` edges so the topology stays explicit.
5. The newly-opened issues become candidates for Step 1's next leaf
   pick. Continue driving (Step 6 — continuous-drive policy).

A leaf closing therefore typically results in one of: (a) a
`go-planning` dispatch to author the next-stage spike or task
breakdown, (b) a `go-design` dispatch to author the next-stage design
spike, or (c) no action because every required next-stage child
already exists and is either open or closed-with-deliverable.

**Never let a context's design subtree dead-end.** If a `design-*`
spike closes and its parent sub-epic has no `task-breakdown-*` or
`draft-*-user-stories` child, the orchestrator's first action MUST be
to author one. Designs that don't break down into plans and stories
ship nothing.

Stop dispatching new leaves when one of:

- Total open unblocked leaves becomes 0.
- The user asks to stop.
- A completion run reports a structural problem (e.g. agent split a
  leaf — the new leaves need to be triaged before continuing).

## Step 6 — Continuous-drive policy

`/go` is a continuous driver, not a one-shot dispatcher. Once
invoked, the skill keeps the dispatch pipeline saturated until one
of the stop conditions above fires. The default lifecycle is:

```
loop:
  1. Sync local main (git fetch + git pull --ff-only).
  2. If any leaf is `dod:verified` since last tick, run Step 4c
     (stage-transition) — open + dispatch the next-stage children.
  3. If a top-level slot is free, run Steps 1–3 to fill the slot
     with a fresh unblocked leaf (or a Step-5 review-pipeline tick
     on an open PR).
  4. Wait for the next completion notification (no polling).
  5. On completion, run Step 4 (verify output + hand to review).
  6. Goto 1.
```

**Stop conditions.** Continuous drive runs until one of: (a) total
open unblocked leaves becomes 0, (b) the user types stop/pause, or
(c) a structural problem requires triage. Token-budget self-throttling
is removed because the harness already auto-compresses prior messages
near context limits, and the watermark heuristic is unreliable. At
end-of-loop, post a single round-up message summarising: PRs opened,
issues closed via dod-verify, stage transitions opened, any leaves
left blocked. Then end the turn.

**User-overridable.** If the user types "stop", "pause", or a fresh
non-/go message, exit the loop immediately — leave running agents
in place (they will complete on their own and post status comments
as usual) but do NOT dispatch any new work.

## Step 5 — Three-Round Sequential Review (per PR)

Every PR — whether produced by a /go leaf agent in the current session
or already merged — gets three sequential rounds of review before it
is allowed to merge (or, for already-merged PRs, before its review
trail is considered complete). Each round is a pair of dispatches:

```
for ROUND in 1 2 3:
    1. Dispatch advance-plan-style review:
         Agent({ subagent_type: "go-review",        run_in_background: true, prompt: <round-N review prompt> })
       Wait for completion.
    2. Dispatch impl-response:
         Agent({ subagent_type: "go-impl-respond",  run_in_background: true, prompt: <round-N respond prompt> })
       Wait for completion.
    3. If the impl-respond opened a follow-up PR (only happens for
       merged-original PRs), the next round's review targets the
       follow-up. Otherwise the next round targets the same PR.
```

After round 3 converges:

- For an OPEN PR (still on its head branch): enable auto-merge with
  `gh pr merge <N> --auto --squash`. The CI gates still apply on top
  of the three-round review.
- For an already-MERGED PR with no impl-respond changes: nothing more
  to do. Post a final round-3 summary comment on the PR.
- For an already-MERGED PR whose impl-respond produced a follow-up
  PR chain: the LAST follow-up PR is now an open PR — recurse Step 5
  on it (it gets its own three-round review). Auto-merge fires on the
  last follow-up after its round 3 converges.

**Round lenses (review agent's body documents these in detail):**

| Round | Lens                                | Concrete focus                                                                            |
|-------|-------------------------------------|-------------------------------------------------------------------------------------------|
| 1     | Coverage + correctness              | Diff fulfills declared scope; tests cover Gherkin Thens; CC subject + issue refs sane.     |
| 2     | Cohesion + SRP + seam quality       | One responsibility per touched module; `std::expected<T, glibre::Error>` at boundaries.   |
| 3     | Topology + spec alignment + polish  | DAG / dependency story coherent; spec invariants cited; sub-issue parenting + labels right. |

**Per-round dispatch:** see `references/dispatch-prompts.md` §
"## Review Round — go-review" and § "## Implementation Response —
go-impl-respond" for the substituted prompt skeletons.

**Concurrency:** within a PR's three rounds the steps are sequential.
Across distinct PRs the rounds may be parallelised up to the ≤ 2
top-level concurrency budget. Practical pattern: at most two PRs in
review pipeline at once, both running their current round's review or
respond agent in parallel.

**Retroactive review of already-merged PRs:** invoke Step 5 directly
on the merged PR number, skipping Steps 1–4. Use it when (a) a PR
was merged before the three-round pipeline existed, (b) the user
explicitly asks for a retroactive review, (c) a follow-up PR cycle
needs to start from a merged baseline.

**Chore PR carve-out (single-round review).** PRs opened by
`go-chore` (or by another agent for a clearly-mechanical change —
typo fix, label sync, vendor pin bump, deterministic regen) get a
single `go-review` round only, then the orchestrator/main thread
enables auto-merge. The chore agent body forbids non-trivial scope,
so additional rounds add cycles without adding signal. If round 1
returns blocking findings, address them (one `go-impl-respond`
round) and merge — do NOT escalate to three rounds unless the diff
turns out to be non-trivial after all (in which case treat the PR
as a `go-coding` deliverable and run the full Step-5 pipeline).

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
