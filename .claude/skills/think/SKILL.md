---
name: think
description: Invoke when reasoning depth is the blocker — a hard root-cause question, a recurring blocker, an ambiguous spec point, a refutation pass before pushing back on review, or a "should I split this scope" judgement call. Dispatches the `go-thinker` agent (opus / xhigh) and returns a structured analysis with ranked hypotheses, strongest evidence, refutation attempt, and a recommended next dispatch. Read-only — never writes code or opens PRs. Callable from chat OR from inside any subagent (go-coding, go-design, go-planning, go-orchestrator, go-chore, go-review, go-qa).
version: 0.1.0
---

# /think

Dispatch the `go-thinker` agent for one specific question. Cheaper
than the parent agent guessing wrong and cycling through review;
deeper than the parent agent's own extended-thinking turns. Use it
liberally — opus xhigh thinking saves more downstream cost than it
spends.

## When to invoke

| Caller | Trigger |
|--------|---------|
| Chat | "think about #N", "deep think on bug X", "thinker on the auth drift" |
| `go-design` | **MANDATORY** at session start (see go-design body) — author candidate aggregate boundaries / public-interface signatures only AFTER /think returns |
| `go-planning` | **MANDATORY** at session start (see go-planning body) — author candidate user-story / plan decomposition only AFTER /think returns |
| `go-coding` | Before any PUSHBACK reply on review; before opening a follow-up PR; when scope-vs-design tension surfaces |
| `go-chore` | When the "chore" seems to touch a documented invariant — decide escalate-or-not |
| `go-orchestrator` | When choosing between competing dispatch trees |
| `go-review` | When unsure whether a finding is real or a misreading |
| `go-qa` | When a manual test FAILS in a way the script did not predict |

## Inputs

| Variable | Required? | Notes |
|----------|-----------|-------|
| `QUESTION` | yes | One-line restatement of what to think about |
| `TARGETS` | yes | Issue numbers, PR numbers, file paths, or commit SHAs to start from |
| `COMMENT_TARGET` | optional | If set, the analysis lands as a comment on that issue / PR; otherwise it returns inline |
| `CALLER_BUCKET` | optional | The bucket invoking /think (`go-design`, `go-coding`, etc.) so the recommended-next-dispatch row knows what kind of follow-up the caller can act on |

## Dispatch shape

```
Agent({
  description: "/think — <one-line question>",
  subagent_type: "go-thinker",
  run_in_background: false,
  prompt: |
    Question: {{QUESTION}}
    Targets: {{TARGETS}}
    Comment target: {{COMMENT_TARGET or "(return inline)"}}
    Caller: {{CALLER_BUCKET or "(chat)"}}

    Required reads: PHILOSOPHY.md, AGENTS.md, the relevant
    specs/<ctx>/SPEC.md, every specs/decisions/*.md cited by the
    targets, the parent epic body.

    Produce the fixed thinker output schema:
    1. ≥ 5 ranked hypotheses (state the rank order explicitly)
    2. Strongest evidence anchoring the lead hypothesis (file:line,
       git blame, spec citations)
    3. Refutation attempt against the lead
    4. Recommended next dispatch (bucket, issue/scope, one-sentence
       brief) — pick the bucket the caller can actually act on

    If the question is malformed (unanswerable or trivially
    answerable from one read), say so directly and recommend a
    reformulation rather than padding.
})
```

`go-thinker` is read-only. It does not call Edit / Write / Agent /
`gh pr create` / `gh pr merge` / `git commit`. The only mutation
it may perform is posting a single comment on the named GitHub
target (when `COMMENT_TARGET` is set) plus its own status comment.

## Cost rationale

Thinking is cheap relative to:

- A round of `go-review` + `go-coding` respond pass on a wrong code
  push
- A merged PR that invalidates a spec because the implementation
  guessed wrong
- An `[SPIKE] iterate-*` born from review escalation that thinker
  could have prevented in one upstream call

Default to invoking /think when a decision has any of: contested
spec text, hard root-cause unknown, pushback-vs-address ambiguity,
scope-vs-design tension. Skip /think only for self-evident
decisions.

## Nesting

`/think` is itself invokable from inside any /go-* agent. The
thinker invocation does not count against the parent's top-level
concurrency budget; it is a nested child. Multiple parents may run
/think in parallel.

`go-thinker` never nests further — it does not spawn children.
That is the invariant that makes its cost predictable.

## Exit

Returns the thinker's structured output to the caller (in chat) or
posts it to `COMMENT_TARGET` (when set). The caller is responsible
for acting on the recommended next dispatch — /think does not
trigger downstream agents itself.
