# Reviews

Per-iteration review artifacts. Three iterations gate any
implementation work.

```
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

Each iteration:

1. Spawn all reviewer perspectives in parallel.
2. Distill into `learnings.md`.
3. Address every learning; record in `addressed.md`.
4. Gate closes when every learning has a resolution.

Implementation begins after iter-3 closes.
