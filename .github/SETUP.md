# GitHub Setup

One-time provisioning for the glibre repo. All work tracked here as
issues; no on-disk task lists.

## 0. Prerequisites

- `gh` CLI authenticated (`gh auth status`).
- Repo created (`gh repo create cjhowe/glibre --private --source=. --remote=origin`).
- Default branch: `main`.

## 1. Branch Protection

Apply to `main`:

```bash
gh api -X PUT repos/:owner/:repo/branches/main/protection \
  -f required_status_checks.strict=true \
  -F required_status_checks.contexts='["lint","build","spec-check","dep-check"]' \
  -F enforce_admins=true \
  -F required_pull_request_reviews.required_approving_review_count=1 \
  -F required_pull_request_reviews.dismiss_stale_reviews=true \
  -F restrictions=null \
  -F required_linear_history=true
```

## 2. Labels

Sync from `.github/labels.yml`:

```bash
# Manual one-shot (workflow handles it on push to main):
gh extension install heaths/gh-label
gh label list --json name -q '.[].name' | xargs -I{} gh label delete {} --yes
yq '.[] | "--name \(.name) --color \(.color)"' .github/labels.yml |
  xargs -L1 gh label create
```

Or push a change to `.github/labels.yml` on `main` and the
`sync-labels` workflow will reconcile.

## 3. Issue Hierarchy (native sub-issues + dependencies)

GitHub native sub-issues + issue dependencies are GA. Enable per-repo:

```bash
gh api -X PATCH repos/:owner/:repo \
  -F has_issues=true \
  -F use_squash_pr_title_as_default=true
```

Hierarchy mapping:

```
initiative
  └─ epic
       └─ sub-epic (optional, may nest)
            └─ plan | spike
```

User stories are linked from any aggregator or leaf via the issue body
(`Stories: #12, #13`); they are not sub-issues of the work item.

For each child, set the parent via the issue's "Parent issue" UI
control or:

```bash
gh api graphql -f query='mutation($parent:ID!,$child:ID!){
  addSubIssue(input:{issueId:$parent, subIssueId:$child}){ issue { number } }
}' -f parent=PARENT_NODE_ID -f child=CHILD_NODE_ID
```

For dependencies (must close before child can start):

```bash
gh api graphql -f query='mutation($issue:ID!,$blocker:ID!){
  addIssueDependency(input:{issueId:$issue, blockedByIssueId:$blocker}){
    issue { number }
  }
}' -f issue=BLOCKED_NODE_ID -f blocker=BLOCKER_NODE_ID
```

A CI dependency-check job enforces the topological-order rule: a PR
referencing an issue with open blockers fails the gate.

## 4. Story-Point Roll-up

Aggregators (`type:initiative`, `type:epic`, `type:sub-epic`) carry no
`pts:*` label. Their estimate = sum of `pts:*` on leaf descendants
(plan / spike / user-story).

A roll-up workflow (TBD: `.github/workflows/rollup.yml`) recomputes
totals on every issue change and posts a comment to the aggregator:

```
roll-up: pts=37 (12 plan, 13 spike, 12 user-story)  leaves=14 closed=3
```

## 5. Issue Templates

Live under `.github/ISSUE_TEMPLATE/`:

- `initiative.yml`
- `epic.yml`
- `sub-epic.yml`
- `user-story.yml` — manual test + E2E test required; closure
  checklist (E2E green → manual PASS → close).
- `plan.yml` — unit test plan required; one or more PRs.
- `spike.yml` — research / design; deliverable = doc / decision.

`config.yml` disables blank issues so every new issue is typed.

## 6. Pull Requests

- Conventional Commit subject: `feat(scope):`, `fix(scope):`,
  `test(scope):`, `refactor(scope):`, `perf(scope):`, `chore(scope):`,
  `docs(scope):`, `build(scope):`, `ci(scope):`.
- PR body uses `.github/pull_request_template.md`.
- One plan issue may have multiple PRs; each PR advances the plan
  toward closure.

## 7. Workflows

`.github/workflows/`:

- `ci.yml` — lint, build, test, spec-check, dep-check.
- `labels.yml` — syncs label set from `.github/labels.yml`.
- `rollup.yml` (TBD) — recomputes aggregator estimates.

## 8. Bootstrap Issue Set

After the repo is provisioned, mine harmonius `US-*` candidates into
draft `type:user-story` issues (manual triage to keep a
necessary-and-sufficient set), then open initial `type:initiative` and
`type:epic` issues matching the `plans/mvp.md` epic table. Subagents
take over from there.

## 9. Agent Execution

See `AGENTS.md`. Up to 5 concurrent top-level plan executors; status
posted as English issue comments. Three review passes before any leaf
plan starts implementation work.
