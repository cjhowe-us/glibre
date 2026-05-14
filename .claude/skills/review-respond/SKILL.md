---
name: review-respond
description: Generalized review-respond loop for any artifact — code PR, design PR, plan issue, doc PR. Loops `review` → original-author re-dispatch on the same artifact until the reviewer verdict is APPROVE, or until the loop escalates to an `[SPIKE] iterate-*` issue. Replaces /review-loop. Author bucket adapts to artifact type. CI gate fires only for code-pr.
version: 1.0.0
---

# /review-respond

Drive review-and-respond cycles on one artifact until convergence. No fixed round count. The loop
iterates **as many rounds as the artifact needs**, and exits only when the reviewer issues `APPROVE`
(with CI green for code-pr) or when the author escalates to an iterate spike.

This skill replaces `/review-loop`. The loop is now **artifact-generic**: it works on code PRs,
design PRs, plan issues, and doc PRs.

## Inputs

| Variable | Required? | Notes |
|----------|-----------|-------|
| `ARTIFACT_TYPE` | required | `code-pr` / `design-pr` / `plan-issue` / `doc-pr` |
| `TARGET` | required | PR number for `*-pr` types; issue number for `plan-issue` |
| `AUTHOR_AGENT` | required | One of `product`, `design`, `plan`, `code`, `chore` — the role that originally produced the artifact |
| `ISSUE_NUMBER` | optional | The leaf issue the artifact closes; used for escalation |

`AUTHOR_AGENT` is **the same role that originally produced the artifact**. Design PRs re-dispatch
`design`; planning PRs / plan issues re-dispatch `plan`; code PRs re-dispatch `code` with
`stage:respond`; chore PRs re-dispatch `chore`; doc PRs re-dispatch the role that owns the touched
docs (`product` / `design` / `plan`). Every author owns its own review-response loop.

## Loop body

```text
round = 0
while true:
    round += 1

    # 1. Reviewer pass
    Agent({
      description: "review-respond r{round} {TARGET}",
      subagent_type: "review",
      run_in_background: false,
      prompt: review_prompt(ARTIFACT_TYPE, TARGET, round, AUTHOR_AGENT)
    })

    verdict = read_latest_review_verdict(ARTIFACT_TYPE, TARGET)
    # verdict ∈ {APPROVE, REQUEST_CHANGES, COMMENT}

    if verdict == APPROVE and ci_ok(ARTIFACT_TYPE, TARGET):
        if ARTIFACT_TYPE == "code-pr":
            enable_auto_merge(TARGET)
        elif ARTIFACT_TYPE in ("design-pr", "doc-pr"):
            enable_auto_merge(TARGET)
        # plan-issue: no merge — the issue body is the artifact
        break

    if verdict == APPROVE and not ci_ok(ARTIFACT_TYPE, TARGET):
        # Only fires for code-pr. APPROVE on red CI is forbidden.
        post_followup_comment(TARGET,
          "review-respond: ignoring APPROVE; CI not green on latest commit")
        continue

    # 2. Author respond pass
    findings = read_unresolved_review_threads(ARTIFACT_TYPE, TARGET)
    if same_finding_seen >= 3 consecutive_rounds:
        # Genuine disagreement / unsatisfiable contract.
        escalate_to_iterate_spike(ARTIFACT_TYPE, TARGET, findings)
        mark_artifact_draft(ARTIFACT_TYPE, TARGET)
        break

    Agent({
      description: "respond r{round} {TARGET}",
      subagent_type: AUTHOR_AGENT,
      run_in_background: false,
      prompt: respond_prompt(ARTIFACT_TYPE, TARGET, round, findings, AUTHOR_AGENT)
    })

    # Loop again — re-review the freshly-updated artifact.
```

### `ci_ok` semantics

| `ARTIFACT_TYPE` | CI gate |
|---|---|
| `code-pr` | All required CI checks green on the latest commit. APPROVE on red/pending CI is forbidden. |
| `design-pr` | Docs lint green (Conventional Commit title, markdown lint, SPEC.md 12-section integrity). |
| `doc-pr` | Same as `design-pr`. |
| `plan-issue` | No CI gate — the issue body is the artifact. APPROVE exits the loop directly. |

## Exit conditions

| Condition | Action |
|-----------|--------|
| Reviewer `APPROVE` + CI ok | Enable auto-merge for PR types; record final round comment; return success |
| Reviewer `APPROVE` + CI red (code-pr only) | Ignore APPROVE, continue loop |
| Same blocking finding survives 3 consecutive rounds | Open `[SPIKE] iterate-<area>-<finding>`, link the artifact, mark PR draft (or issue `status:escalated` for plan-issue), return escalated |
| Author respond reports it cannot satisfy a finding within scope | Open iterate spike per author's recommendation, mark artifact draft, return escalated |
| User types `stop` / `/stop` | Exit loop, leave artifact in current state |

## Per-round prompts

### Reviewer prompt (round N)

```text
Repo: /Users/cjhowe/Code/glibre
ARTIFACT_TYPE: {ARTIFACT_TYPE}
TARGET: {TARGET}
Round: {N}
Author agent: {AUTHOR_AGENT}

Review the latest state of {TARGET} against:
- the linked GitHub issue body (if any)
- the relevant SPEC.md / ADR(s) in the diff or referenced files
- PHILOSOPHY.md (storage contract: plans→issues, designs→repo;
  design-invalidation rule)
- CLAUDE.md
- specs/decisions/constraints.md

Pick the lens for ARTIFACT_TYPE (see review agent body):
- code-pr: coverage + correctness; SOLID/SRP/seam quality; spec alignment; CI green
- design-pr: re-derivation rigor; aggregate boundary justification; SOLID citation
  per choice; ADR completeness; design-invalidation handled
- plan-issue: template conformance; DoD populated with concrete assertions;
  Gherkin/test-list testable; estimate sanity; dependency graph
- doc-pr: internal consistency; cross-doc links valid; no plan-content drift into
  repo; no design-content drift into issues

Post inline review comments where applicable. Post a top-level verdict
with one of: APPROVE / REQUEST_CHANGES / COMMENT.

For code-pr: APPROVE is forbidden if any required CI check is failing
or pending on the latest commit.

This is round {N}; if the same finding has been flagged in previous
rounds and is still present, recommend escalation to an
[SPIKE] iterate-* issue instead of another in-artifact cycle.
```

### Author respond prompt (round N)

```text
Repo: /Users/cjhowe/Code/glibre
ARTIFACT_TYPE: {ARTIFACT_TYPE}
TARGET: {TARGET}
Round: {N}
Role: {AUTHOR_AGENT}

You authored {TARGET}. The reviewer's findings on round {N} are in the
unresolved review threads. Address them by:

- For *-pr types: editing the same files on the same branch and pushing
  a single Conventional-Commit follow-up commit per round (or, if the
  PR is merged and ARTIFACT_TYPE is code-pr, opening a follow-up PR per
  code.md `stage:respond` MERGED path).
- For plan-issue: editing the issue body via `gh issue edit` and
  replying inline on each finding thread.

Storage contract still applies: if a finding requires invalidating a
spec or ADR, update it in the same commit. If addressing a finding
requires more than this artifact's scope, abort the round and open an
[SPIKE] iterate-<area> issue, then mark the artifact draft (PR:
`gh pr ready --undo`; issue: add `status:escalated` label). The
/review-respond driver will catch the draft state and exit.

For code-pr: dispatch with `stage:respond` so `code` enters its
review-response branch protocol (push to same branch when PR open;
open follow-up PR when original merged).

Reply to each addressed thread with `decision:ADDRESSED commit:<sha>`
(or `followup:#<N>` for merged-PR mode). Reply to each declined thread
with `decision:PUSHBACK reasoning:<...> proposal:<...>`. For HIGH
findings, ADDRESSED or DEFER only — silent skips forbidden.
```

## Concurrency

Each `/review-respond` invocation runs as one foreground subagent loop in the caller's thread.
Caller-decides parallelism — the caller may invoke multiple `/review-respond` instances in a single
tool-call message for parallel artifact review. `/review-respond` itself stays single-artifact.

Rounds within a single artifact are strictly sequential — reviewer and author must each finish
before the next round starts.

## How `/work` invokes this skill

After `/work` returns from a successful agent dispatch that opened a PR (or authored a plan-issue),
`/work` hands off to `/review-respond`:

```text
Skill({
  skill: "review-respond",
  args: '{ "ARTIFACT_TYPE": "<code-pr|design-pr|plan-issue|doc-pr>",
           "TARGET": "#<N>",
           "AUTHOR_AGENT": "<product|design|plan|code|chore>",
           "ISSUE_NUMBER": <M> }'
})
```

## Notes

- CI gate (`feedback_go_review_rejects_red_ci` memory): APPROVE on red/pending CI is rejected for
  `code-pr` and treated as REQUEST_CHANGES.
- The PR's author-respond agent must push commits, not open a new PR — except for the
  merged-original case handled by `code` in `stage:respond` (opens a single follow-up PR).
- Plan-issue rounds run entirely against the issue body and its `## Definition of Done` block — no
  PR involved.
