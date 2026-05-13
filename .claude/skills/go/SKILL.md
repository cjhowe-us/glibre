---
name: go
description: This skill should be used when the user asks to "advance the plan", "pick up unblocked items", "what should I work on", "next batch of work", "make progress on issues", "start parallel agents", "drive the spec to completion", or any variation of "keep going on the plan". It queries GitHub for up to 2 unblocked leaf issues, dispatches one specialized background subagent per item to run the appropriate SDLC stage (ideation / breakdown / design / planning / testing / review / iteration / qa), and reports the resulting PRs back through GitHub issue comments. Coding-bucket stages are disabled while the C++→Rust pivot lockout is in effect.
version: 0.3.0
---

# Advance Plan

> ## CODING LOCKOUT — design / planning / review only
>
> The 2026-05-13 C++→Rust pivot disables the coding bucket. Plans
> live in **GitHub Issues**; designs live in the **repository** and
> must be updated when invalidated. PR review iterates via the
> `/review-loop` skill until the reviewer issues `APPROVE` — no
> fixed round count.
>
> Dormant agents (re-enable by restoring their rows in the SDLC stage
> table below and stripping the `STATUS: DORMANT` banners on the
> agent files):
> - `go-coding` (handles both `MODE: implement` and `MODE: respond`;
>   subsumes the former `go-impl-respond`)
> - `go-chore`
>
> Step 0.5 (red-CI triage) is disabled — no code PRs exist while the
> lockout holds.

Drive the glibre repo's plan forward by picking up to **2 unblocked
leaf issues** from GitHub, then dispatching one **specialized**
background subagent per item to perform the next SDLC stage for that
item. Each bucket pins its own model + reasoning effort (see Step 3).
Track all state through GitHub issue comments — never on disk.

## Storage contract (applies to every dispatched agent)

| Artifact | Storage location | Notes |
|----------|------------------|-------|
| Plans (task breakdowns, `[PLAN]`, `[STORY]`, `[SPIKE]`, sub-epics, epics, initiatives) | GitHub Issues only | `gh issue edit` for body changes; no plan markdown in the repo |
| Designs (specs, ADRs, integration contracts, decision records, diagrams) | Repository — `specs/<ctx>/SPEC.md`, `specs/<ctx>/*.md`, `specs/decisions/*.md` | Committed via PR; must be updated when any change invalidates them |
| Manual test scripts | `[USER-STORY]` issue body | Issue body edit |
| E2E traces | `e2e/` as `.glibre-trace` files (created once tooling lands) | Committed via PR |
| Progress / status | GitHub issue comments | Append-only |
| Test PASS / FAIL | GitHub issue comment | Append-only |

Hard rules every agent honors:

1. **Plans never land in the repo.** If an agent is about to write a
   plan body to disk, abort and open an issue instead.
   `plans/{mvp,post-mvp,long-term}.md` are GitHub-issue index pointers,
   not plan content.
2. **Designs never land in issues.** Issue bodies describe *what to
   design*; the design itself lives in the repo. `[SPIKE] design-*`
   and `[SPIKE] research-*` close by opening a PR that adds / edits
   `specs/<ctx>/SPEC.md` or `specs/decisions/<topic>.md`, then
   linking the merged PR from the issue.
3. **Design invalidation is mandatory.** Every PR (design, planning,
   and code once coding re-enables) must check: does this change
   invalidate existing design under `specs/`? If yes, the same PR
   updates the affected files. If not feasible without scope creep,
   abort and open `[SPIKE] iterate-<area>` first.
4. **Cross-reference.** Every design PR body cites `closes #N` /
   `refs #M`; every issue closure comment cites the merged design PR.

## Agent + skill inventory

Every dispatchable surface this skill knows about. Use the table to
pick a target; use the per-agent sections below to know what to put
in the prompt. Agent files live under
`.claude/agents/<name>.md`; skills under `.claude/skills/<name>/SKILL.md`.

### Quick map

| Surface | Kind | Status | When to invoke | Output |
|---------|------|--------|----------------|--------|
| `go-orchestrator` | agent | live | User typed "orchestrate #N" / "drive #N end-to-end"; issue carries `orchestration:multi-stage` label | Plan comment, nested dispatches, closure verdict |
| `go-design` | agent | live | Step-2 row matches `[SPIKE] research-*`, `design-*`, `decide-*`, `close-*-open-questions` | PR editing `specs/<ctx>/SPEC.md` / `specs/decisions/*.md` |
| `go-planning` | agent | live | Step-2 row matches `[SPIKE] draft-*-user-stories`, `task-breakdown-*`, or `[USER-STORY] *` in testing stage | New GitHub issues + PR with `.glibre-trace` + CI hook + DoD blocks |
| `go-review` | agent | live | Dispatched by `/review-loop` on every design / planning PR — NOT by /go directly | Inline review comments + top-level verdict (`APPROVE` / `REQUEST_CHANGES` / `COMMENT`) |
| `go-qa` | agent | live | `[USER-STORY] *` with merged trace + CI green; story moves to qa stage | Issue comment `manual-test:PASS` / `manual-test:FAIL` + screenshots |
| `go-thinker` | agent | live, **dispatched via `/think` skill only** | Any subagent (and chat) invokes `/think` when reasoning depth is the blocker | Read-only analysis comment + recommended next dispatch |
| `go-coding` | agent | **DORMANT** | (re-enables when coding lockout lifts; handles `[PLAN] *` in `MODE: implement` AND respond rounds in `MODE: respond` on its own PRs) | (locked) |
| `go-chore` | agent | **DORMANT** | (re-enables when coding lockout lifts; would match `[CHORE] *` / `kind:chore`) | (locked) |
| `/go` | skill | live | User typed "advance the plan", "what should I work on", any /go variant | This skill's own loop |
| `/think` | skill | live | Any agent (or chat) needs deep reasoning before acting. **MANDATORY** at session start for `go-design` + `go-planning` | Dispatches `go-thinker`; returns structured analysis |
| `/review-loop` | skill | live | Step-4 hands off an opened design / planning PR (also code PRs once coding re-enables) | Reviewer-APPROVE + auto-merge enabled, **or** escalation to `[SPIKE] iterate-*` |

### Nested-subagent-tree doctrine

Cost efficiency comes from a wide, shallow tree:

- **Top level** — /go or chat picks ONE leaf per slot (≤ 2 slots).
- **First nesting** — the leaf agent dispatches `/think` at session
  start (mandatory for go-design / go-planning), then opens its PR.
- **Inside the PR session** — agents may fan out child Agent calls
  for parallel reads / boilerplate / mechanical edits. Pick the
  cheapest sufficient model per child:

  | Child kind | Bucket | Cost |
  |------------|--------|------|
  | Deep reasoning / refutation | `/think` → `go-thinker` (opus xhigh) | high but saves downstream cycles |
  | Mechanical edits (label sync, regen, single-rename) | `go-chore` (haiku low; dormant) | cheapest |
  | Implementation fan-out (file-per-file, test-per-test) | Direct Agent (sonnet) | medium |
  | Cross-context spec drilling | Sibling `go-design` | only via parent epic; do NOT inline nest a design spike inside a planning session |

- **Never nest peer-bucket work inside a session that should split
  instead.** If go-design uncovers a planning need, open an issue and
  let /go pick the planning leaf on the next tick — do not run
  `go-planning` as a child of `go-design`. Bucket boundaries match
  PR boundaries; mixing them produces unreviewable PRs.

The nested children do not count against the ≤ 2 top-level slot
budget. /think nested calls never consume a top-level slot.

### Agents — usage detail

#### `go-orchestrator`

- **File**: `.claude/agents/go-orchestrator.md`
- **Model**: opus / **effort**: high
- **Top-level slot**: 1 (nested children do NOT count)
- **Triggers**: chat phrase "orchestrate #N", "drive #N end-to-end";
  or open issue with label `orchestration:multi-stage`
- **NOT auto-picked.** The leaf scanner ignores this agent — Step 0
  is the only entrypoint.
- **Use when** an issue spans multiple SDLC stages and you want one
  long-running TPM driving the whole arc.
- **Children it spawns**: any of `go-design`, `go-planning`,
  `go-coding` (dormant; both modes), `go-chore` (dormant), `go-qa`,
  `/think`. Drives `/review-loop` on every PR it opens.

#### `go-design`

- **File**: `.claude/agents/go-design.md`
- **Model**: opus / **effort**: xhigh (deepest reasoning bucket)
- **Top-level slot**: 1
- **MANDATORY: `/think` first.** Every dispatch starts with a
  `/think` call seeded with the spike's question and targets. No
  spec section is authored until the thinker output returns.
- **Triggers** (Step-2 row patterns):
  - `[SPIKE] research-*-responsibilities` → §1, §2 fill + ADR
  - `[SPIKE] research-*-harmonius-mining` → §3 fill + ADR
  - `[SPIKE] design-*-aggregates` → §4
  - `[SPIKE] design-*-public-interface` → §5 (Rust trait sketch)
  - `[SPIKE] design-*-persistence-schemas` → §7 + rkyv sketch
  - `[SPIKE] design-*-hot-reload-contract` → §8
  - `[SPIKE] design-*-internal-architecture` → §6
  - `[SPIKE] design-*-perf-budget` → §9
  - `[SPIKE] design-*-failure-modes` → §10
  - `[SPIKE] decide-*` → cross-cutting ADR under `specs/decisions/`
  - `[SPIKE] close-*-open-questions` → §12 resolutions
- **Output**: PR editing `specs/<ctx>/SPEC.md` and/or
  `specs/decisions/<topic>.md`. Open PR with `gh pr create` and
  stop — `/review-loop` enables auto-merge after `APPROVE`.
- **Review-respond**: author-responds via `/review-loop` on the
  same branch. No separate respond-agent.

#### `go-planning`

- **File**: `.claude/agents/go-planning.md`
- **Model**: opus / **effort**: high
- **Top-level slot**: 1
- **MANDATORY: `/think` first.** Every dispatch starts with a
  `/think` call to validate the breakdown / decomposition. No
  issue is opened until the thinker output returns.
- **Triggers** (Step-2 row patterns):
  - `[SPIKE] draft-*-user-stories` → opens `[USER-STORY]` issues +
    fills spec §11
  - `[SPIKE] task-breakdown-*-implementation` → opens `[PLAN]`
    issues parented to epic (each tagged `awaiting-coding-unlock`
    while coding is locked)
  - `[USER-STORY] *` with no merged trace → authors `.glibre-trace`
    file + CI hook PR (testing stage)
- **Output**: New GitHub issues authored via
  `.github/ISSUE_TEMPLATE/` + PR with `## Definition of Done`
  blocks. Open PR with `gh pr create` and stop.
- **Review-respond**: author-responds via `/review-loop`.

#### `go-review`

- **File**: `.claude/agents/go-review.md`
- **Model**: opus / **effort**: high
- **Top-level slot**: 1 per round (sequential within a PR)
- **Triggers**: dispatched **only** by `/review-loop`, never by /go
  directly.
- **Output**: Inline review comments on the PR diff + a single
  top-level verdict comment with one of `APPROVE` /
  `REQUEST_CHANGES` / `COMMENT`. `APPROVE` is forbidden if CI is
  red/pending on the latest commit.
- **Storage contract awareness**: review lens checks that the diff
  obeys plans-in-issues / designs-in-repo / design-invalidation
  rules.
- **Loop interaction**: /review-loop dispatches go-review, then the
  author bucket, repeats. Same blocking finding surviving 3 rounds
  → escalate.

#### `go-qa`

- **File**: `.claude/agents/go-qa.md`
- **Model**: sonnet / **effort**: medium
- **Top-level slot**: 1
- **Triggers**: `[USER-STORY] *` whose linked trace file is merged
  AND the CI lane is green.
- **Output**: Comment on the story issue with
  `manual-test:PASS` / `manual-test:FAIL` + screenshots from
  browser / computer-use MCP tools. Story closes only after PASS
  is recorded + DoD verifier verifies.
- **Storage contract**: scripts in `[USER-STORY]` body; PASS/FAIL
  result in issue comment.

#### `go-thinker` — via `/think` skill

- **File**: `.claude/agents/go-thinker.md`
- **Model**: opus / **effort**: xhigh
- **Top-level slot**: 1 if chat-summoned; 0 if nested (default)
- **Dispatch surface**: always via the `/think` skill, never via a
  raw Agent call. The skill takes `QUESTION`, `TARGETS`,
  optional `COMMENT_TARGET`, optional `CALLER_BUCKET`.
- **Triggers**:
  - **MANDATORY** at start of every `go-design` and `go-planning`
    session.
  - On-demand by any subagent when reasoning depth is the blocker
    (pushback before review reply, scope-vs-design tension,
    chore-or-not judgement, root-cause analysis).
  - Chat: user typed "think about #N", "thinker on bug X".
- **Output**: ≥ 5 ranked hypotheses, strongest evidence, refutation
  attempt, recommended next dispatch. Never writes code, never
  opens PRs.

#### `go-coding` — DORMANT

- **File**: `.claude/agents/go-coding.md` (`STATUS: DORMANT` banner)
- **Subsumes** the former `go-impl-respond`. Two modes:
  - `MODE: implement` — fresh implementation against a `[PLAN]` /
    `[CHORE]` / `kind:bug` leaf. Opens a new PR.
  - `MODE: respond` — review-response round dispatched by
    `/review-loop` on a PR the agent (or a prior go-coding
    session) authored. Pushes commits to the same branch, or opens
    a follow-up PR if the original is merged.
- **When re-enabled**:
  - Model: sonnet / effort: high
  - Triggers: `[PLAN] *`, `[PLAN] *` with `kind:bug`, integration
    PRs, maintenance PRs (`MODE: implement`); `/review-loop`
    re-dispatches on its own open PRs (`MODE: respond`).
  - Output: Rust crate PR with `#[test]` fns + spec/ADR updates if
    implementation invalidates design.
  - Build commands: `cargo build --workspace && cargo test
    --workspace && cargo clippy --workspace -- -D warnings &&
    cargo fmt --all -- --check`.
- **While dormant**: /go does NOT dispatch this agent. `[PLAN]`
  leaves stay tagged `awaiting-coding-unlock` in the backlog.

#### `go-chore` — DORMANT

- **File**: `.claude/agents/go-chore.md` (`STATUS: DORMANT` banner)
- **When re-enabled**:
  - Model: haiku / effort: low
  - Triggers: `[CHORE] *`, `[PLAN] kind:chore`, single-symbol
    rename, vendor pin bump, label sync, deterministic regen.
  - Output: tiny PR (< 50 LOC). Single-round review via
    `/review-loop`'s chore carve-out.
  - **Hard escalation rule**: if the chore touches a documented
    invariant in `specs/`, open `[SPIKE] iterate-*` instead of
    landing it silently.
- **While dormant**: not dispatched.

### Skills — usage detail

#### `/go` (this skill)

- **File**: `.claude/skills/go/SKILL.md` (this file)
- **Triggers**: user phrases like "advance the plan", "pick up
  unblocked items", "next batch", "what should I work on", "drive
  the spec to completion", or any "keep going on /go" variant.
- **Loop**: Steps 0 → 0.5 (disabled) → 1 → 2 → 3 → 4 → /review-loop
  hand-off → 4b/4c → 6, repeating until user types `stop`.
- **Invokes**: every live agent above + `/review-loop`.

#### `/think`

- **File**: `.claude/skills/think/SKILL.md`
- **Dispatch shape**:
  ```
  Skill({ skill: "think",
          args: '{ "QUESTION": "<one-line restatement>",
                   "TARGETS": "<issue#, PR#, file paths>",
                   "CALLER_BUCKET": "<go-design|go-planning|...|chat>",
                   "COMMENT_TARGET": "<optional issue/PR>" }' })
  ```
- **Mandatory callers**: `go-design` and `go-planning` at session
  start.
- **Optional callers**: every other agent (and chat) when
  reasoning depth is the blocker.
- **Output**: returns thinker's structured analysis to caller
  (inline) or posts as comment on `COMMENT_TARGET`.
- **Concurrency**: nested /think calls do NOT consume top-level
  slots. Multiple parents may run /think in parallel.

#### `/review-loop`

- **File**: `.claude/skills/review-loop/SKILL.md`
- **Dispatch shape**:
  ```
  Skill({ skill: "review-loop",
          args: '{ "PR_NUMBER": <N>,
                   "AUTHOR_BUCKET": "<go-design|go-planning|go-coding>",
                   "ISSUE_NUMBER": <M> }' })
  ```
- **Triggers**: /go Step 4 hands off after a leaf opens a PR.
  Callable from chat for retroactive review on any open PR.
- **Loop**: dispatch `go-review`; if not `APPROVE` + CI green,
  dispatch `AUTHOR_BUCKET` to address findings (go-coding uses
  `MODE: respond`); repeat. No fixed round count. Escalate on 3
  consecutive identical blocking findings → open
  `[SPIKE] iterate-*` and mark PR draft.
- **Exit states**: `APPROVE + CI green` → auto-merge enabled;
  `escalated` → iterate spike opened, PR draft.
- **Concurrency**: each running loop consumes 1 top-level slot at
  the dispatching moment, releases between rounds. Up to 2 PRs in
  review-loop concurrently.

### Storage-contract cheat sheet (all agents enforce this)

| You're about to write… | Allowed surface | Forbidden surface |
|------------------------|-----------------|-------------------|
| A task / plan / story body | GitHub issue (via `gh issue create/edit`) | `plans/*.md`, repo files |
| A spec section / ADR | `specs/<ctx>/SPEC.md`, `specs/decisions/*.md` | Issue body |
| A `.glibre-trace` test | `e2e/<ctx>/*.glibre-trace` (committed via PR) | Issue body |
| Progress / status / decision-rationale | Issue comment | On-disk log files |
| Manual test PASS/FAIL | Issue comment | Filesystem |

If the agent finds itself about to violate the row above, abort
and route through the correct surface or open an iterate spike.

## Invariants

Every dispatch must respect these (also enforced by `AGENTS.md`):

- **≤ 2 concurrent top-level subagents.** Children unbounded.
- **No on-disk state.** Status, progress, decisions: GitHub comments
  on the relevant issue.
- **PRs only on `main`.** Every doc change goes via Pull Request with
  a Conventional Commit subject.
- **One leaf, one session.** A `type:user-story`, `type:plan`, or
  `type:spike` must complete in a single session. If not, the agent
  splits the leaf into smaller leaves and posts the split.
- **Aggregators carry no estimate.** Story points only on
  `type:user-story` and `type:plan`.
- **Issues stay open until deliverables are merged into `main`.**
- **Definition of Done is authoritative.** Every leaf carries a
  machine-checkable `## Definition of Done` block (DSL in
  `.github/DOD-DSL.md`). Closure is the verifier's verdict.

## Required Step Order

0. **Manual orchestrator hook (optional, before the leaf picker).**
   If the user said "orchestrate #N", or an open issue carries the
   `orchestration:multi-stage` label, dispatch a single
   `go-orchestrator` against that issue and stop the leaf picker for
   this turn. Otherwise fall through to Step 1.
0.5. **Red-CI triage — DISABLED while coding is locked.** Re-enable
   when restoring the coding bucket.
1. **Query** GitHub for unblocked leaves; collect up to 2.
2. **Filter** by SDLC stage: pick items in earliest open stage first.
   `[PLAN] *` leaves are **not picked** while coding is locked.
3. **Dispatch** one background subagent per picked item.
4. **Wait** for completion notifications (do not poll). On each
   completion, verify the agent posted the required status comment,
   verify the PR (if any) was opened, then hand the PR off to the
   `/review-loop` skill.

## Step 1 — Query Unblocked Leaves

Run the GraphQL recipe in `references/github-recipes.md`. The query
returns open issues with zero open `blockedBy` edges and
type ∈ {`type:user-story`, `type:plan`, `type:spike`}.

**While coding is locked, exclude `[PLAN] *` from the candidate
pool**. Tag any plan-leaf that becomes unblocked with
`awaiting-coding-unlock`.

Pick at most 2. Selection priority:

1. Downstream fan-out (primary).
2. Earliest open SDLC stage (tie-breaker).

## Step 2 — Match Item to SDLC Stage

Coding-bucket rows are struck out for the lockout.

| Issue pattern                              | Stage          | Bucket                  | Output kind                                |
|--------------------------------------------|----------------|-------------------------|--------------------------------------------|
| `[SPIKE] research-*-responsibilities`      | ideation       | `go-design`             | ADR + spec §1, §2 fill                     |
| `[SPIKE] research-*-harmonius-mining`      | ideation       | `go-design`             | ADR + spec §3 fill                         |
| `[SPIKE] design-*-aggregates`              | design         | `go-design`             | Spec §4 fill                               |
| `[SPIKE] design-*-public-interface`        | design         | `go-design`             | Spec §5 (Rust trait sketch)                |
| `[SPIKE] design-*-persistence-schemas`     | design         | `go-design`             | Spec §7 + `rkyv` schema sketch             |
| `[SPIKE] design-*-hot-reload-contract`     | design         | `go-design`             | Spec §8                                    |
| `[SPIKE] design-*-internal-architecture`   | design         | `go-design`             | Spec §6                                    |
| `[SPIKE] design-*-perf-budget`             | design         | `go-design`             | Spec §9                                    |
| `[SPIKE] design-*-failure-modes`           | design         | `go-design`             | Spec §10                                   |
| `[SPIKE] decide-*`                         | design         | `go-design`             | Cross-cutting ADR                          |
| `[SPIKE] close-*-open-questions`           | iteration      | `go-design`             | Resolutions in spec §12                    |
| `[SPIKE] draft-*-user-stories`             | breakdown      | `go-planning`           | New `type:user-story` issues + spec §11    |
| `[SPIKE] task-breakdown-*-implementation`  | planning       | `go-planning`           | New `type:plan` issues parented to epic    |
| `[USER-STORY] *` (E2E trace authoring)     | testing        | `go-planning`           | `.glibre-trace` + CI hook                  |
| `[USER-STORY] *` (manual execution)        | qa             | `go-qa`                 | Manual test PASS / FAIL + screenshots      |
| ~~`[PLAN] *`~~                             | implementation | ~~`go-coding`~~ DORMANT | (locked — sits in backlog)                 |
| ~~`[PLAN] *` with `kind:bug`~~             | maintenance    | ~~`go-coding`~~ DORMANT | (locked — sits in backlog)                 |
| ~~`[CHORE] *` / `[PLAN] kind:chore`~~      | maintenance    | ~~`go-chore`~~ DORMANT  | (locked — sits in backlog)                 |
| (manual: `orchestration:multi-stage`)      | orchestration  | `go-orchestrator`       | Plan comment + nested dispatches + closure |
| (manual: "think about #N" — via `/think`)  | analysis       | `go-thinker` via /think | Read-only analysis comment                 |

Detailed SDLC per stage lives in `references/sdlc.md`.

## Step 3 — Dispatch Subagents

Use the Agent tool with `run_in_background: true`. Skeleton:

```
Agent({
  description: "<short title — issue # + stage>",
  subagent_type: "<bucket from Step-2 table>",
  run_in_background: true,
  prompt: <stage-prompt with item-specific variables substituted>
})
```

Bucket → model + effort (resolved by the agent's own frontmatter):

- `go-orchestrator` → `model: opus`,   `effort: high`
- `go-design`       → `model: opus`,   `effort: xhigh`
- `go-thinker` (via `/think`) → `model: opus`, `effort: xhigh`
- `go-planning`     → `model: opus`,   `effort: high`
- `go-review` (via `/review-loop`) → `model: opus`, `effort: high`
- `go-qa`           → `model: sonnet`, `effort: medium`
- ~~`go-coding`~~   → DORMANT
- ~~`go-chore`~~    → DORMANT

**Concurrency accounting.** Each top-level dispatch counts as one
slot against the ≤ 2 budget. Orchestrator's nested children do NOT
count. `/think` invocations never count.

Send all picks (up to 2) in **a single tool-call message** so they
start in parallel.

The stage prompt MUST include:

1. Issue number and title.
2. Required reads: `PHILOSOPHY.md`, `AGENTS.md`, the relevant
   `specs/decisions/*.md` already committed, parent issue bodies.
3. Deliverable contract.
4. Status-comment schema from `AGENTS.md`.
5. Closure rule for the issue type.
6. PR-only rule: agent opens PR with `gh pr create` and **stops**;
   `/review-loop` enables auto-merge after `APPROVE`.
7. **Storage contract reminder**.
8. **For `go-design` / `go-planning`: explicit reminder that
   `/think` is mandatory at session start.**

## Step 4 — Verify Leaf Output and Hand to /review-loop

On each completion notification:

1. Read the agent's summary.
2. Check the issue's latest comment matches the schema and cites a
   PR number.
3. Confirm the PR exists and is OPEN with auto-merge NOT enabled.
4. Run `git fetch origin main && git pull --ff-only origin main`.
5. Verify the issue carries a `## Definition of Done` block.
6. Hand the PR to `/review-loop`:

   ```
   Skill({
     skill: "review-loop",
     args: '{ "PR_NUMBER": <N>, "AUTHOR_BUCKET": "<go-design|go-planning|go-coding>", "ISSUE_NUMBER": <M> }'
   })
   ```
7. Optionally pick the next unblocked leaf if a slot is free.

### Step 4b — DoD-gated closure

After all PRs that close a leaf merge into `main`:

1. The `dod-verify` workflow fires on close. Manual trigger:
   `/verify-dod` comment.
2. On `dod:verified`, the leaf is done.
3. On `dod:failed`, dispatch `go-planning` first to revise the
   plan / DoD / scope before any further pass. While coding is
   locked, design / planning / story-trace leaves are likewise
   re-routed through `go-planning` / `go-design` via `/review-loop`.

The orchestrator MUST NOT close a leaf via `gh issue close` directly.

### Step 4c — Stage transition

A closed leaf typically unlocks the next SDLC stage. On every
`dod:verified` closure, inspect the parent and open next-stage
children if missing.

| Closed leaf pattern                              | Next-stage child(ren) | Bucket           |
|--------------------------------------------------|------------------------|------------------|
| `[SPIKE] research-*-responsibilities` (§1, §2)   | `[SPIKE] research-*-harmonius-mining` | `go-design` |
| `[SPIKE] research-*-harmonius-mining` (§3)       | `[SPIKE] design-*-aggregates` | `go-design` |
| `[SPIKE] design-*-aggregates` (§4)               | `[SPIKE] design-*-public-interface` | `go-design` |
| `[SPIKE] design-*-public-interface` (§5)         | parallel design-* spikes | `go-design` |
| `[SPIKE] design-*-failure-modes` (§10)           | `[SPIKE] draft-*-user-stories` | `go-planning` |
| `[SPIKE] design-*-detailed`                      | `[SPIKE] task-breakdown-*-implementation` | `go-planning` |
| `[SPIKE] draft-*-user-stories`                   | `[SPIKE] task-breakdown-*-implementation` per story | `go-planning` |
| `[SPIKE] task-breakdown-*-implementation`        | `[PLAN] *` — tag `awaiting-coding-unlock`; do NOT dispatch | n/a (locked) |
| `[USER-STORY] *` testing closed via merged trace | Story moves to qa (currently blocked by lockout) | (queued) |

**Planning → implementation transition is DISABLED.**

If a single Step-1 query returns zero candidates:

1. `git pull --ff-only`.
2. Scan for `dod:failed` reopens + new Step-4c children.
3. Schedule a wakeup (1200–1800s) and re-enter the loop on fire.

## Step 5 — Review pipeline (delegated to /review-loop)

The previous three-round Step 5 is gone. /review-loop runs review →
author-respond → repeat until `APPROVE` or escalates to an iterate
spike. No fixed round count.

/go's job for PRs:

1. Hand off after PR opens (see Step 4.6).
2. Wait for loop exit:
   - `APPROVE + CI green` → /review-loop enabled auto-merge.
   - `escalated` → /review-loop opened iterate spike, marked PR
     draft.
3. Across distinct PRs, /review-loop runs may parallelise up to the
   ≤ 2 top-level slot budget.

`AUTHOR_BUCKET` while coding is locked: `go-design` | `go-planning`.
Once coding re-enables, code-PR loops use `go-coding` in
`MODE: respond`.

## Step 6 — Continuous-drive policy

```
loop:
  1. git fetch + git pull --ff-only.
  2. (Step 0.5 red-CI triage is DISABLED.)
  3. If any leaf is `dod:verified` since last tick, run Step 4c.
  4. If a top-level slot is free, fill it with a fresh unblocked
     leaf, or hand an open PR to /review-loop.
  5. Wait for the next completion notification.
  6. On completion, run Step 4.
  7. Goto 1.
```

**Stop condition.** The loop runs until the user explicitly types
`stop`, `/stop`, or `pause`. "Nothing pickable right now" is not a
stop condition; schedule a wakeup and re-query.

## Reference Files

- **`references/sdlc.md`** — full SDLC stages.
- **`references/dispatch-prompts.md`** — per-stage prompt templates.
- **`references/github-recipes.md`** — gh CLI + GraphQL queries.

## Re-enable coding (future)

1. Remove the `## CODING LOCKOUT` banner.
2. Restore the struck-through rows in the Step-2 stage table.
3. Restore the bullet entries for `go-coding`, `go-chore` in the
   Step-3 model-effort list.
4. Restore Step 0.5 (red-CI triage).
5. Strip the `STATUS: DORMANT` banners on
   `.claude/agents/go-coding.md` and `.claude/agents/go-chore.md`.
6. Remove the `awaiting-coding-unlock` label from backlog plans.
7. `AUTHOR_BUCKET` in /review-loop now also accepts `go-coding`
   (which handles `MODE: respond`).
