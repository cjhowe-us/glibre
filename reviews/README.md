# Reviews

Per-iteration review artifacts (`iter-N/`). Loop scaffolding for
multi-perspective reviews of significant work.

```text
reviews/
├── iter-1/
│   ├── cohesion-srp.md
│   ├── scope-discipline.md
│   ├── topology-risk.md
│   ├── story-fidelity.md
│   ├── e2e-feasibility.md
│   ├── learnings.md       # distilled themes + action items
│   └── addressed.md       # responses (accept / defer / reject + reason)
├── iter-2/...
└── iter-3/...
```

ADRs and other decision records live under
[`../specs/decisions/`](../specs/decisions/), not here. The
`/review-loop` skill (`.claude/skills/review-loop/SKILL.md`) drives
PR-level review-respond cycles; this directory holds higher-level
iteration artifacts only.

Each iteration:

1. Spawn all reviewer perspectives in parallel.
2. Distill into `learnings.md`.
3. Address every learning; record in `addressed.md`.
4. Gate closes when every learning has a resolution.
