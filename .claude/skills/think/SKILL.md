---
name: think
description: Effort/model modifier — boosts the next agent dispatch (or current reasoning turn) to opus + xhigh + extended-thinking. Use when reasoning depth is the blocker — a hard root-cause question, a recurring blocker, an ambiguous spec point, a refutation pass before pushing back on review, a "should I split this scope" judgement call. Pins the ranked-hypothesis output contract. Callable from chat or from inside any role agent.
version: 1.0.0
---

# /think

`/think` is a **meta-modifier**. It does NOT spawn a separate thinker agent. Instead, it elevates
the **next reasoning turn** (the caller's own turn, or the next agent dispatch the caller will make)
to:

- `model: opus`
- `effort: xhigh`
- extended-thinking ON
- structured output contract: ranked hypothesis schema (below)

Use it liberally — opus xhigh thinking saves more downstream cost than it spends.

## When to invoke

| Caller | Trigger |
|--------|---------|
| Chat | "think about #N", "deep think on bug X", "thinker on the auth drift" |
| `design` | At session start when deriving aggregate boundaries / public-interface signatures; mid-session when a fresh hard question emerges |
| `plan` | At session start before authoring `[STORY]` / `[PLAN]` batches; mid-session when partition ambiguity surfaces |
| `code` | Before any PUSHBACK reply on review; before opening a follow-up PR; when scope-vs-design tension surfaces |
| `product` | When scope is contested, persona is ambiguous, MVP-vs-post-MVP cut is unclear |
| `chore` | When the "chore" seems to touch a documented invariant — decide escalate-or-not |
| `review` | When unsure whether a finding is real or a misreading |
| `test` | When a manual test FAILS in a way the script did not predict |

## Inputs

| Variable | Required? | Notes |
|----------|-----------|-------|
| `QUESTION` | yes | One-line restatement of what to think about |
| `TARGETS` | yes | Issue numbers, PR numbers, file paths, or commit SHAs to start from |
| `COMMENT_TARGET` | optional | If set, the analysis lands as a comment on that issue / PR; otherwise it returns inline |
| `CALLER_BUCKET` | optional | The role invoking `/think` (`design`, `plan`, etc.) so the recommended-next-dispatch row knows what kind of follow-up the caller can act on |

## Modifier semantics

When `/think` is invoked:

1. The **next agent dispatch** the caller makes runs at `model:opus`, `effort:xhigh`,
   extended-thinking on.
2. The dispatched agent (or the caller itself, if reasoning inline) follows the **output schema**
   below.
3. The modifier applies for ONE turn. The caller resumes its normal model/effort after the turn
   returns.

### Dispatch shape (for caller reference)

If the caller plans to dispatch a subagent for the boosted turn:

```text
Agent({
  description: "/think — <one-line question>",
  subagent_type: "<caller's role or a peer role suited to the question>",
  model: "opus",
  effort: "xhigh",
  run_in_background: false,
  prompt: |
    Question: {{QUESTION}}
    Targets: {{TARGETS}}
    Comment target: {{COMMENT_TARGET or "(return inline)"}}
    Caller: {{CALLER_BUCKET or "(chat)"}}

    Required reads: PHILOSOPHY.md, CLAUDE.md, the relevant
    specs/<ctx>/SPEC.md, every specs/decisions/*.md cited by the
    targets, the parent epic body.

    Produce the fixed think output schema:
    1. ≥ 5 ranked hypotheses (state the rank order explicitly)
    2. Strongest evidence anchoring the lead hypothesis (file:line,
       git blame, spec citations)
    3. Refutation attempt against the lead
    4. Recommended next dispatch (role, issue/scope, one-sentence brief)

    If the question is malformed (unanswerable or trivially answerable
    from one read), say so directly and recommend a reformulation
    rather than padding.

    Read-only on source code. No code edits, no PRs, no `gh issue close`.
    The only mutation permitted is posting the final analysis to
    COMMENT_TARGET (if set) plus a status comment.
})
```

If the caller is reasoning inline (no subagent), the caller follows the same output contract on its
own turn at opus xhigh.

### Output schema (the contract the boosted turn follows)

```markdown
# Analysis — <one-line restatement of the question>

## Hypothesis ranking

1. **<H1 — strongest>** — <one paragraph: claim + evidence>
2. **<H2>** — …
3. **<H3>** — …
… ≥ 5 hypotheses, ranked by evidence weight. State the rank order explicitly.

## Strongest evidence

- File:line citations or `git blame` excerpts anchoring H1.
- Counter-evidence considered for H1 and why it does not refute.

## Refutation attempt

For H1, narrate the strongest argument *against* it that you could construct, and why it failed (or,
if it succeeded, why H2 now leads).

## Recommended next dispatch

| Role | Issue # / scope | Concrete brief |
|---|---|---|
| `<product|design|plan|code|chore|test|review>` | #N or path | one sentence |

End with the CLAUDE.md status-comment block (`agent:<role> status:done …`).
```

If ≥ 5 hypotheses cannot be ranked (search space genuinely smaller), state the cap explicitly and
explain why.

## Cost rationale

Thinking is cheap relative to:

- A round of `review` + `code` respond-pass on a wrong code push
- A merged PR that invalidates a spec because the implementation guessed wrong
- An `[SPIKE] iterate-*` born from review escalation that `/think` could have prevented in one
  upstream call

Default to invoking `/think` when a decision has any of: contested spec text, hard root-cause
unknown, pushback-vs-address ambiguity, scope-vs-design tension. Skip `/think` only for self-evident
decisions.

## Nesting

`/think` is invokable from inside any role agent. The modifier does not consume any top-level slot
budget — caller controls parallelism, not the modifier. Multiple parents may invoke `/think` in
parallel.

The boosted turn itself does not spawn children (read-only, single-track). That is the invariant
that makes its cost predictable.

## Exit

The boosted turn returns its structured output to the caller (in chat) or posts it to
`COMMENT_TARGET` (when set). The caller acts on the recommended next dispatch — `/think` does not
trigger downstream agents itself.
