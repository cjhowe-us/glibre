# glibre — High-Level Design

Strategy documents for the glibre engine. **High level only.** No code, no interface signatures, no
file-tree dictates. Tactics live elsewhere; this set captures *what* and *why*.

## Reading order

| File | Topic |
|---|---|
| [`00-vision.md`](00-vision.md) | Thesis, audience, refusals, non-goals. |
| [`10-bounded-contexts.md`](10-bounded-contexts.md) | Nine bounded contexts, responsibilities, ubiquitous language. |
| [`15-context-map.md`](15-context-map.md) | Fifteen named seams between contexts and their integration patterns. |
| [`20-platform-strategy.md`](20-platform-strategy.md) | Supported platforms, substrate stances, three-seam native interop. |
| [`30-authoring-and-no-code.md`](30-authoring-and-no-code.md) | What no-code means daily, Composition as substrate, fragments, AI-assist posture. |
| [`40-runtime-architecture.md`](40-runtime-architecture.md) | Editor/runtime as distinct processes, viewport hosting, tick/frame decoupling, hot-reload. |
| [`50-rendering-strategy.md`](50-rendering-strategy.md) | Portable layer above the seams, shader pipeline, 2D/3D unification, tier-based scalability. |
| [`60-content-pipeline.md`](60-content-pipeline.md) | Source vs baked, Asset DB identity, bake-as-build-time, hot-reload semantics. |
| [`70-accessibility-localization.md`](70-accessibility-localization.md) | A11y/L10n as primitive properties, semantic tree, structural localization. |
| [`80-distribution-and-extensibility.md`](80-distribution-and-extensibility.md) | Packaging, extensions as first-class, marketplace as channel not gatekeeper. |
| [`90-risks-and-open-questions.md`](90-risks-and-open-questions.md) | Consolidated risk register; living. |

## Reading-order narrative

Start at the vision (`00`). Everything else cites it. The vision commits to a thesis (the no-code
path is canonical), an audience (makers), and a refusal list (no closed marketplaces, no
rendering-as-headline, no real-time co-edit).

From the vision, the design splits into two strands that meet later.

**Strand A — what the engine is modeled around.** Read `10` (bounded contexts) and `15` (context
map) together. `10` names nine contexts and their ubiquitous language; `15` names the fifteen seams
between them and the DDD integration pattern governing each. Every later pillar uses the terms and
seams committed in these two files.

**Strand B — what substrate the engine is built on.** Read `20` (platform strategy) before any of
the architectural pillars. `20` pins the .NET substrate, the three-seam native rendering interop,
the shader pipeline at the platform-API tier, the logic-graph AOT toolchain stance, and the
editor-JIT / iOS-AOT execution model.

The two strands meet at the architectural pillars. Read them in dependency order:

- `40` (runtime architecture) — editor and runtime as distinct OS processes, viewport
  hosting, tick/frame decoupling, hot-reload contract, execution-model boundary.
- `30` (authoring & no-code) — Composition as the substrate of authored behavior,
  graphs compile (never interpret), live preview as default, maker ergonomics dominate.
- `60` (content pipeline) — source/baked split, Asset DB identity, bake at build time,
  hot-reload at edit time.
- `50` (rendering strategy) — portable layer above the three seams, shaders baked
  per-target, 2D/3D unified, tier-based scalability, editor and runtime share one
  renderer.
- `70` (accessibility & localization) — a11y/l10n as properties of primitives,
  authoring-time linting for the incomplete-primitive rule, string identity, semantic
  tree, Platform delivers / A11y shapes.
- `80` (distribution & extensibility) — Distribution consumes pre-baked artifacts,
  packaging conforms to platform expectations, extensions are first-class, marketplace
  is a channel not a gatekeeper, AI providers are extensions.

Close the loop at `90` — the register of every risk and open question named across the pillars. `90`
is meant to be read often; entries are retired as the design commits to their resolution.

## Scope

- **In scope:** strategic decisions, domain boundaries, integration shape, philosophy.
- **Out of scope:** APIs, file layout, language-level idioms, test plans, build scripts.

Each pillar passed a review–respond cycle before its successor was authored. The holistic pass on
the doc set as a whole is in `.review-state/_holistic.md` until the set is committed; per the
design-set authoring plan, `.review-state/` is scratch that does not ship with the project.
