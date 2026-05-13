---
name: review-loop
description: This skill should be used when a PR needs review-respond cycles. Loops `go-review` → original-author-respond on the same PR branch until the reviewer verdict is APPROVE, or until the loop escalates to an `[SPIKE] iterate-*` issue. Replaces the fixed three-round review pipeline. Takes a PR number; uses the PR author agent (`go-design`, `go-planning`, or `go-coding` in `MODE: respond` once coding re-enables) for responses.
version: 0.2.0
---

# /review-loop

Drive review-and-respond cycles on a single PR until convergence.
No fixed round count. The loop iterates **as many rounds as the diff
needs**, and exits only when the reviewer issues `APPROVE` or when
the response agent escalates to an iterate spike.

This skill replaces the previous three-round review pipeline in
`/go` (Step 5). `/go` now dispatches `/review-loop` after every
design/planning PR opens.

## Inputs

| Variable | Source | Notes |
|----------|--------|-------|
| `PR_NUMBER` | required | The open PR to drive |
| `AUTHOR_BUCKET` | required | One of `go-design`, `go-planning`, `go-coding` |
| `ISSUE_NUMBER` | optional | The leaf issue the PR closes; used for escalation |

`AUTHOR_BUCKET` is **the same bucket that originally produced the PR**.
Design PRs re-dispatch `go-design`; planning PRs re-dispatch
`go-planning`; code PRs (once coding re-enables) re-dispatch
`go-coding` with `MODE: respond` in the prompt — `go-coding` subsumes
the former `go-impl-respond`. No separate respond-agent for any
bucket; every author owns its own review-response loop.

While coding is locked, `AUTHOR_BUCKET ∈ {go-design, go-planning}`
only.

## Loop body

```
round = 0
while true:
    round += 1

    # 1. Reviewer pass
    Agent({
      description: "review-loop r{round} #{PR_NUMBER}",
      subagent_type: "go-review",
      run_in_background: false,
      prompt: review_prompt(PR_NUMBER, round)
    })

    verdict = read_latest_review_verdict(PR_NUMBER)
    # verdict ∈ {APPROVE, REQUEST_CHANGES, COMMENT}

    if verdict == APPROVE and ci_green(PR_NUMBER):
        # Exit success.
        enable_auto_merge(PR_NUMBER)
        break

    if verdict == APPROVE and not ci_green(PR_NUMBER):
        # APPROVE on red CI is forbidden; treat as REQUEST_CHANGES
        # (see memory feedback_go_review_rejects_red_ci).
        post_followup_comment(PR_NUMBER,
          "review-loop: ignoring APPROVE; CI not green on latest commit")
        continue

    # 2. Author respond pass
    findings = read_unresolved_review_threads(PR_NUMBER)
    if same_finding_seen >= 3 consecutive_rounds:
        # Genuine disagreement / unsatisfiable contract.
        escalate_to_iterate_spike(PR_NUMBER, findings)
        mark_pr_draft(PR_NUMBER)
        break

    Agent({
      description: "respond r{round} #{PR_NUMBER}",
      subagent_type: AUTHOR_BUCKET,
      run_in_background: false,
      prompt: respond_prompt(PR_NUMBER, round, findings)
    })

    # Loop again — re-review the freshly-pushed commits.
```

## Exit conditions

| Condition | Action |
|-----------|--------|
| Reviewer `APPROVE` + CI green | Enable auto-merge; record final round comment; return success |
| Reviewer `APPROVE` + CI red | Ignore APPROVE, continue loop (CI not green on latest commit) |
| Same blocking finding survives 3 consecutive rounds | Open `[SPIKE] iterate-<area>-<finding>` issue, link the PR, mark PR draft, return escalated |
| Author respond reports it cannot satisfy a finding within PR scope | Open iterate spike per author's recommendation, mark PR draft, return escalated |
| User types `stop` / `/stop` | Exit loop, leave PR in current state |

## Per-round prompts

### Reviewer prompt (round N)

```
Repo: /Users/cjhowe/Code/glibre
PR: #{PR_NUMBER}
Round: {N}
Author bucket: {AUTHOR_BUCKET}

Review the latest commit of PR #{PR_NUMBER} against:
- the linked GitHub issue body (if any)
- the relevant SPEC.md / ADR(s) in the diff
- PHILOSOPHY.md (storage contract: plans→issues, designs→repo;
  design-invalidation rule)
- AGENTS.md
- specs/decisions/constraints.md

Post inline review comments where applicable. Post a top-level
verdict comment with one of: APPROVE / REQUEST_CHANGES / COMMENT.
APPROVE is forbidden if any required CI check is failing or pending
on the latest commit (memory: feedback_go_review_rejects_red_ci).
This is round {N}; if the same finding has been flagged in the
previous rounds and is still present, recommend escalation to an
[SPIKE] iterate-* issue instead of another in-PR cycle.
```

### Author respond prompt (round N)

```
Repo: /Users/cjhowe/Code/glibre
PR: #{PR_NUMBER}
Round: {N}
Bucket: {AUTHOR_BUCKET}

You authored PR #{PR_NUMBER}. The reviewer's findings on round {N}
are in the unresolved review threads. Address them by:
- editing the same files (designs in specs/ + specs/decisions/,
  issues for planning PRs) on the same branch
- pushing a single Conventional-Commit `refactor` / `docs` /
  `chore` follow-up commit per round (or `MODE: respond` commits
  for go-coding once coding re-enables)
- replying inline on each thread you resolved
- replying inline on each thread you decline, with reasoning

Storage contract still applies: if a finding requires invalidating a
spec or ADR, update it in the same commit. If addressing a finding
requires more than this PR's scope, **abort the round and open an
[SPIKE] iterate-<area>** issue, then mark the PR draft via
`gh pr ready --undo`. The /review-loop driver will catch the draft
state and exit.

go-coding dispatches MUST include `MODE: respond` in the prompt so
the agent enters its review-response branch protocol (push to same
branch when PR open; open follow-up PR when original merged) rather
than its `MODE: implement` (fresh PR from main) path.
```

## How `/go` invokes this skill

After Step 4 hands off an opened design/planning PR, /go runs:

```
Skill({
  skill: "review-loop",
  args: '{ "PR_NUMBER": <N>, "AUTHOR_BUCKET": "<go-design|go-planning>", "ISSUE_NUMBER": <M> }'
})
```

`/go` does not run rounds itself anymore. The previous three-round
pipeline in `/go` Step 5 is removed; `/go` simply hands off and
watches for the loop's exit state.

## Notes

- The loop runs **per PR**. /go may have at most 2 PRs in
  review-loop concurrently (≤2 top-level slot budget — each loop
  consumes one slot at the dispatching moment, then releases between
  rounds while awaiting completions).
- `go-review` and the author bucket must both finish each round
  before the next round starts; rounds within a single PR are
  strictly sequential.
- CI gates still apply: `APPROVE` on red CI is rejected per
  `feedback_go_review_rejects_red_ci`.
- The PR's author-respond agent must push commits, not open a new
  PR — except for the merged-original case handled by `go-coding`
  in `MODE: respond`, which opens a single follow-up PR.
