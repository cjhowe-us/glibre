---
name: work
description: This skill should be used when the user asks to "advance the plan", "pick up unblocked items", "what should I work on", "do the next thing", "execute the next leaf", or any variation of "make progress on the backlog". Picks ONE unblocked leaf issue, dispatches a single foreground role agent (product / design / plan / code / test / review / chore), blocks until done. Caller-decides parallelism — invoke /work multiple times in one chat turn for parallel leaves; each /work itself stays single-leaf.
version: 1.0.0
---

# /work

Execute one unblocked leaf issue in a foreground subagent. **Caller-decides parallelism.** The skill
itself dispatches exactly one leaf in one foreground subagent and returns when it completes. The
caller (chat or another orchestrating skill) decides whether to invoke `/work` again — sequentially
for context isolation, or in parallel by sending multiple `/work` invocations in a single tool-call
message.

## Inputs

| Variable | Required? | Notes |
|----------|-----------|-------|
| `ISSUE_NUMBER` | optional | Specific leaf to pick. If omitted, the skill runs the unblocked-leaves query and picks the highest-priority leaf. |
| `STAGE_OVERRIDE` | optional | Force a stage (`product` / `design` / `plan` / `code` / `test` / `review` / `chore`). If omitted, the skill matches the issue against the stage routing table. |

## Storage contract (every dispatched agent honors)

| Artifact | Storage | Notes |
|---|---|---|
| Plans (task breakdowns, `[PLAN]`, `[STORY]`, `[SPIKE]`, sub-epics, epics, initiatives) | GitHub Issues only | `gh issue edit` for body changes; no plan markdown in the repo |
| Designs (specs, ADRs, decision records) | Repository — `specs/<ctx>/SPEC.md`, `specs/<ctx>/*.md`, `specs/decisions/*.md` | Committed via PR; must be updated when any change invalidates them |
| Manual test scripts | `[USER-STORY]` issue body | Issue body edit |
| E2E traces | `e2e/<ctx>/*.glibre-trace` (committed via PR) | |
| Progress / status / decisions | GitHub issue comments (English) | Append-only |
| Test PASS / FAIL | GitHub issue comment, prefix `manual-test status:` | Append-only |

Hard rules:

1. Plans never land in the repo. Every plan / story / spike / epic / sub-epic / initiative body
   lives in a GitHub issue.
2. Designs never land in issues. Spike issues describe *what to design*; the design itself lives in
   `specs/`.
3. **Design invalidation is mandatory.** Every PR (design, planning, code, chore) must check whether
   the change invalidates existing design under `specs/`. If yes, the same PR updates the affected
   files. If scope blocks that, open `[SPIKE] iterate-<area>` first.
4. Every design PR body cites `Closes #N` / `Refs #M`; every issue closure cites the merged design
   PR.

## Step 1 — Pick the leaf

If `ISSUE_NUMBER` is set, use it directly. Otherwise:

```bash
gh api graphql --paginate -f query='
query($endCursor: String) {
  repository(owner:"cjhowe-us", name:"glibre") {
    issues(states: OPEN, first: 100, after: $endCursor) {
      pageInfo { hasNextPage endCursor }
      nodes {
        number title
        labels(first: 5) { nodes { name } }
        blockedBy(first: 30) { nodes { state } }
      }
    }
  }
}' --jq '.data.repository.issues.nodes[]
  | select([.blockedBy.nodes[] | select(.state=="OPEN")] | length == 0)
  | select(any(.labels.nodes[].name; . == "type:spike" or . == "type:plan" or . == "type:user-story"))
  | "\(.number)\t\(([.labels.nodes[].name | select(startswith("type:"))][0]))\t\(.title)"' \
  | awk -F'\t' '
      $3 ~ /research-.*-responsibilities/    { p=1 }
      $3 ~ /research-.*-harmonius-mining/    { p=2 }
      $3 ~ /design-.*-aggregates/            { p=3 }
      $3 ~ /design-.*-public-interface/      { p=4 }
      $3 ~ /design-.*-persistence-schemas/   { p=5 }
      $3 ~ /design-.*-hot-reload-contract/   { p=6 }
      $3 ~ /design-.*-internal-architecture/ { p=7 }
      $3 ~ /design-.*-perf-budget/           { p=8 }
      $3 ~ /design-.*-failure-modes/         { p=9 }
      $3 ~ /draft-.*-user-stories/           { p=10 }
      $3 ~ /close-.*-open-questions/         { p=11 }
      $3 ~ /task-breakdown-.*/               { p=12 }
      $2 == "type:user-story"                { p=13 }
      $2 == "type:plan"                      { p=14 }
      { print p"\t"$0 }' \
  | sort -n | head -1 | cut -f2-
```

If zero candidates: `git pull --ff-only`, scan for `dod:failed` reopens, return "no unblocked
leaves" — caller decides whether to wait or stop.

## Step 2 — Match leaf to role

| Issue pattern | Role | Output kind |
|---|---|---|
| New product area (no spec stub, no parent epic) | `product` | Initiative / epic / story issues + optional spec-stub PR (§1 §2 §11) |
| `[SPIKE] research-*-responsibilities` / `*-harmonius-mining` | `design` | ADR + spec §1 §2 §3 fill |
| `[SPIKE] design-*-aggregates` / `*-public-interface` / `*-persistence-schemas` / `*-hot-reload-contract` / `*-internal-architecture` / `*-perf-budget` / `*-failure-modes` | `design` | Spec §4 / §5 / §7 / §8 / §6 / §9 / §10 |
| `[SPIKE] decide-*` | `design` | Cross-cutting ADR under `specs/decisions/` |
| `[SPIKE] close-*-open-questions` | `design` | Resolutions in spec §12 |
| `[SPIKE] draft-*-user-stories` | `plan` | New `[STORY]` issues + spec §11 |
| `[SPIKE] task-breakdown-*-implementation` | `plan` | New `[PLAN]` issues parented to epic |
| `[USER-STORY] *` (E2E trace authoring) | `plan` | `.glibre-trace` + CI hook |
| `[PLAN] *` | `code` | Rust PR with `#[test]` fns |
| `[PLAN] *` + `kind:bug` | `code` | Bug-fix PR |
| `[CHORE] *` / `[PLAN] kind:chore` | `chore` | <50 LOC PR |
| `[USER-STORY] *` (manual execution, trace already green) | `test` | Manual test PASS / FAIL + screenshots |
| Open PR (any author) needing review | `review` (via `/review-respond`) | Inline findings + verdict |

If `STAGE_OVERRIDE` is set, use it regardless of pattern.

## Step 3 — Dispatch one foreground subagent

```text
Agent({
  description: "<short title — issue # + role>",
  subagent_type: "<role from Step 2>",
  run_in_background: false,
  prompt: <dispatch prompt — see below>
})
```

The dispatch prompt MUST include:

1. Issue number and title.
2. Required reads (role-specific; see each role's agent file).
3. Storage contract reminder.
4. Status-comment schema (`agent:<role>` per `CLAUDE.md`).
5. PR-only rule: agent opens PR with `gh pr create` and STOPS — `/review-respond` enables auto-merge
   after `APPROVE`.
6. **Foreground dispatch is blocking.** The caller waits.

### Dispatch prompt skeleton

```text
You are the <ROLE> executor for GitHub issue #<N> in repo cjhowe-us/glibre — <TITLE>.

REQUIRED READS:
- /Users/cjhowe/Code/glibre/PHILOSOPHY.md
- /Users/cjhowe/Code/glibre/CLAUDE.md
- /Users/cjhowe/Code/glibre/.github/SETUP.md
- /Users/cjhowe/Code/glibre/.github/DOD-DSL.md
- /Users/cjhowe/Code/glibre/specs/_TEMPLATE.md
<EXTRA_READS — role and stage specific; see role agent file>

INVARIANTS:
- One leaf, one session. Never block. If scope grows, split via redirect comment + new issues, exit.
- Branch from main: <verb>/<scope>-<slug> (per .github/SETUP.md).
- Conventional Commit PR title.
- Open PR with `gh pr create` and STOP. Do NOT call `gh pr merge --auto --squash`.
  /review-respond drives review→author cycles.
- PR body MUST contain `Closes #<N>` so dod-verify fires on merge.
- Definition of Done block on the issue per .github/DOD-DSL.md. Author or refresh
  in this PR via `gh issue edit`. Every assertion must be satisfied by the merged diff.
- Issue stays OPEN. Closure happens via PR merge + Closes #N + dod-verify green.
- Status comment schema (post via `gh issue comment <N> --body "$(cat <<EOF ... EOF)"`
  so $BRANCH / $WT expand):
    agent:<role>
    status:<started|progress|done>
    issue:#<N>
    branch:$BRANCH
    worktree:$WT
    host:$(hostname -s)
    cloud:none
    commit:$(git -C "$WT" rev-parse HEAD)
    pr:#<n>-or-pending
    notes:<one-paragraph English summary>

WORKING DIRECTORY (per-dispatch git worktree — required for safety):
Before any code/spec edits:
    BRANCH="<verb>/<scope>-<short-slug>"
    WT="/Users/cjhowe/Code/glibre/.claude/worktrees/$(date +%s)-issue-<N>"
    git -C /Users/cjhowe/Code/glibre fetch origin main
    git -C /Users/cjhowe/Code/glibre worktree add "$WT" -b "$BRANCH" origin/main
    cd "$WT"

All work, commits, and `gh pr create` happen from $WT. Main worktree stays on main.
```

Per-role `<EXTRA_READS>`:

- `product` — `/Users/cjhowe/Code/harmonius/docs/requirements/<ctx>/`, peer specs in the same domain
- `design` — `/Users/cjhowe/Code/glibre/specs/<ctx>/SPEC.md`, every `specs/decisions/*.md` cited by
  parent epic, `/Users/cjhowe/Code/harmonius/docs/design/<ctx>/` (research input only — re-derive)
- `plan` — `/Users/cjhowe/Code/glibre/specs/<ctx>/SPEC.md` (must be filled),
  `.github/ISSUE_TEMPLATE/user-story.yml` and `plan.yml`
- `code` — plan-issue body (Scope / Unit Test Plan / Stories Satisfied), `specs/<ctx>/SPEC.md`,
  `specs/decisions/constraints.md`
- `test` — story issue body (Manual Test Script, Gherkin, persona), green-CI check recipe (see
  `test.md`)
- `review` — dispatched only via `/review-respond`; see that skill
- `chore` — only the file the chore touches; `CLAUDE.md` for repo-policy chores

## Step 4 — On completion

The foreground subagent returns. Verify:

1. Status comment posted on the issue with the schema above.
2. If a PR was opened, it is OPEN with auto-merge NOT enabled.
3. PR body contains `Closes #<N>`.
4. Issue carries a populated `## Definition of Done` block.

Then hand the PR to `/review-respond`:

```text
Skill({
  skill: "review-respond",
  args: '{ "ARTIFACT_TYPE": "<code-pr|design-pr|plan-issue|doc-pr>",
           "TARGET": "#<PR_NUMBER or ISSUE_NUMBER>",
           "AUTHOR_AGENT": "<product|design|plan|code|chore>",
           "ISSUE_NUMBER": <N> }'
})
```

Return to the caller. The caller decides whether to invoke `/work` again (sequentially or in
parallel) or stop.

## Step 4b — DoD-gated closure

After the closing PR merges into `main`:

1. The `dod-verify` workflow fires on `issues:closed`. Manual trigger: `/verify-dod` comment.
2. On `dod:verified`, the leaf is done.
3. On `dod:failed`, the next `/work` invocation routes the reopened leaf through `plan` first to
   revise plan / DoD / scope before any further pass.

`/work` MUST NOT close a leaf via `gh issue close` directly.

## Step 4c — Stage transition

A closed leaf typically unlocks the next stage. On every `dod:verified` closure, the caller may
inspect the parent and open next-stage children if missing:

| Closed leaf pattern | Next-stage child(ren) | Role |
|---|---|---|
| `[SPIKE] research-*-responsibilities` (§1 §2) | `[SPIKE] research-*-harmonius-mining` | `design` |
| `[SPIKE] research-*-harmonius-mining` (§3) | `[SPIKE] design-*-aggregates` | `design` |
| `[SPIKE] design-*-aggregates` (§4) | `[SPIKE] design-*-public-interface` | `design` |
| `[SPIKE] design-*-public-interface` (§5) | parallel design-* spikes | `design` |
| `[SPIKE] design-*-failure-modes` (§10) | `[SPIKE] draft-*-user-stories` | `plan` |
| `[SPIKE] draft-*-user-stories` | `[SPIKE] task-breakdown-*-implementation` per story | `plan` |
| `[SPIKE] task-breakdown-*-implementation` | `[PLAN] *` issues opened by `plan` | `code` |
| `[USER-STORY] *` testing closed via merged trace | Story moves to QA | `test` |

## Invariants

- **One leaf, one foreground subagent.** `/work` itself is single-leaf; parallelism is the caller's
  choice.
- **No on-disk state.** Status, progress, decisions: GitHub issue comments.
- **PRs only on `main`.** Every change goes via PR with Conventional Commit subject.
- **One leaf, one session.** A leaf must complete in a single session. If not, split + redirect +
  exit.
- **Aggregators carry no estimate.** Story points only on `type:user-story` and `type:plan`.
- **Issues stay open until deliverables merge into `main`.**
- **Definition of Done is authoritative.** Every leaf carries a machine-checkable
  `## Definition of Done` block. Closure is the verifier's verdict.
