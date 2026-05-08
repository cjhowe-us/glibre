# Software Development Lifecycle (glibre)

This document defines the SDLC stages used by the `go`
skill. Each stage has: **input**, **what to do**, **deliverable**,
**closure rule**, and **next stage gate**.

The stages map onto issue types and the workflow already documented
in `AGENTS.md`, `PHILOSOPHY.md`, and `.github/SETUP.md`. Read those
first.

---

## 1. Ideation

**Input:** Bounded-context name (e.g. `core`, `render`); harmonius
prior art at `/Users/cjhowe/Code/harmonius/docs/`.

**Issue types:** `[SPIKE] research-<ctx>-responsibilities`,
`[SPIKE] research-<ctx>-harmonius-mining`.

**Do:**

1. Read `PHILOSOPHY.md` (SOLID, SRP, cohesion AND completeness).
2. Read the matching harmonius docs only as input — independently
   re-derive every conclusion.
3. State what the context owns and what it refuses to own (one
   paragraph each). Pull the smallest reasonable boundary.
4. List the ubiquitous-language terms that will be used unchanged in
   code (≤ 20 entries; expand later if needed).
5. Note any Occam collapses (multiple harmonius concepts → one glibre
   primitive).

**Deliverable:** Spec sections §1, §2, §3 of `specs/<ctx>/SPEC.md`
filled with substantive content. PR opens for the change.

**Closure rule:** PR merged. Issue stays open until merged.

**Gate to next stage:** §1, §2, §3 stable enough that aggregate
shape can be designed without further re-derivation.

---

## 2. Breakdown

**Input:** Filled §1–§3 of a spec; the engine-wide decision records
under `reviews/decisions/`.

**Issue types:** `[SPIKE] draft-<ctx>-user-stories`.

**Do:**

1. List concrete user-stories in the harmonius style:
   `As a <persona>, I want <capability>, so that <outcome>.`
2. Each story includes acceptance criteria (Gherkin), a manual test
   script, and an E2E test plan (replay trace path).
3. Open one `[STORY]` GitHub issue per story using the
   `user-story.yml` template. Reference all open questions inline.

**Deliverable:** New `type:user-story` issues opened, parented to the
context's Acceptance + Implementation Plan sub-epic. Spec §11 listing
those issue numbers, committed via PR.

**Closure rule:** PR merged. Issue stays open until merged.

**Gate to next stage:** every user-story issue has been triaged for
phase (`phase:mvp` / `phase:post-mvp` / `phase:long-term`) and
estimate (`pts:*`).

---

## 3. Design

**Input:** Filled §1–§3, §11 (drafted stories), engine-wide decision
records.

**Issue types:** all per-context `[SPIKE] design-*` and the E0
`[SPIKE] decide-*`.

**Do:**

1. For aggregates (§4): identify entities, value objects, invariants,
   and the boundaries that protect them. Justify each with a SOLID
   principle (usually SRP). Reject internal cross-domain abstractions
   that have only one user.
2. For public interface (§5): write a header-only C++ stub that
   compiles. Use `std::expected<T, glibre::Error>` at every public
   boundary. No runtime reflection.
3. For persistence schemas (§7): coordinate with the `data` context.
   Cite the Apache Fory codegen pipeline from
   `reviews/decisions/fory-codegen.md`. Provide migration rules.
4. For hot-reload contract (§8): cite the protocol from
   `reviews/decisions/hot-reload-protocol.md` (when it lands; until
   then, use the sketch in the plan). State what survives swap, what
   `migrate(...)` does, and which refusal cases apply.
5. For internal architecture (§6): non-binding sketch. One short
   diagram in code-block ASCII or one paragraph per system.
6. For perf budget (§9): cite engine-wide allocation; declare CPU/GPU/
   heap cells for the context.
7. For failure modes (§10): enumerate `core::Error` variants the
   context emits.

**Deliverable:** Spec sections filled, committed via PR. Decision
records under `reviews/decisions/` for cross-cutting items.

**Closure rule:** PR merged. Issue stays open until merged.

**Gate to next stage:** design review pass (3 iterations for E0;
1 light pass per per-context epic) signed off via comments on the
parent sub-epic.

---

## 4. Planning

**Input:** Filled spec.

**Issue types:** `[SPIKE] task-breakdown-<ctx>-implementation`.

**Do:**

1. Convert the public interface (§5) and acceptance criteria (§11)
   into a leaf `type:plan` backlog.
2. Each plan is one Claude Code session of work, ≤ pts:5 ideal,
   pts:8 hard cap, with ≥ 1 named Catch2 unit test in its plan.
3. Open `[PLAN]` issues using the `plan.yml` template. Parent them
   to the per-context epic.
4. Set GitHub native dependencies: plans that depend on others
   reference them via the issue's `blocked_by` relationship.

**Deliverable:** New `type:plan` issues opened. Their numbers cited
from the relevant epic body (PR updates the epic's body).

**Closure rule:** PR merged + plan issues opened.

**Gate to next stage:** dependency DAG terminates; no cycles; every
user-story issue is satisfied by ≥ 1 plan.

---

## 5. Testing (acceptance authoring)

**Input:** A `type:user-story` issue with full Gherkin acceptance and
test plan fields.

**Do:**

1. Write the manual test script in the issue body. Each step is a
   concrete action a human reviewer can execute against the editor or
   runtime.
2. Author the E2E `.glibre-trace` file under `tests/e2e/<ctx>/` via
   PR. Trace must encode the input + assertion ops needed to prove
   the acceptance criteria.
3. CI runs the trace; it should fail loudly until implementation
   plans land (this is correct red).

**Deliverable:** Trace committed via PR; CI workflow exercises it.

**Closure rule:** PR merged. The user-story issue itself does NOT
close yet — closure waits for QA stage.

**Gate to next stage:** trace runs deterministically across hosts.

---

## 6. Implementation

**Input:** A `type:plan` issue.

**Do:**

1. Branch from `main`. Name: `feat/<scope>-<short-slug>` or
   `fix/<scope>-<short-slug>`.
2. Implement the slice declared in the plan's Scope section. Do not
   widen scope. Don't touch other plans' surfaces.
3. Add the named Catch2 unit tests from the plan's Unit Test Plan
   section. They must pass.
4. Run `cmake --preset macos-debug` build + `ctest` locally. Address
   any clang-tidy regressions.
5. Open a PR. Title = Conventional Commit subject. Body references
   the plan issue and the user-story issues it advances.
6. Enable auto-merge on the PR (`gh pr merge <n> --auto --squash`).

**Deliverable:** PR merged. Code, tests, possibly docs.

**Closure rule:** plan issue closes when its declared PR(s) merge AND
all named tests pass on `main`.

**Gate to next stage:** unit tests green on `main`; story-level E2E
test now finds something concrete to assert against.

---

## 7. Review

**Input:** Open PR.

**Do:**

1. The `review.yml` workflow runs automatically: Conventional Commit
   PR title, markdown lint, YAML lint, SPEC.md 12-section integrity,
   commit-subject issue-ref advisory.
2. All PRs auto-merge on green once the three-round review converges.

**Deliverable:** PR merged.

**Gate to next stage:** PR merged + branch deleted.

---

## 8. Iteration

Used after a stage's output is reviewed and found incomplete.

**Do:**

1. Open follow-up `[SPIKE]` issues against the same parent sub-epic,
   one per outstanding concern.
2. The spike's deliverable is the targeted fix to a spec section or
   decision record.
3. For E0 (cross-cutting), iteration runs as 3 explicit parallel
   review passes per `AGENTS.md` § Three-Pass Authoring; each pass
   produces `reviews/iter-N/<perspective>.md` files via PR.

**Deliverable:** Spec / decision-record updates merged via PR; the
follow-up spike issue closes when its specific concern lands on
`main`.

---

## 9. QA

**Input:** A `type:user-story` issue whose E2E trace is **green in CI**
(asserted via the trace's CI step).

**Do:**

1. The user (human reviewer) executes the manual test script from the
   issue body against the editor/runtime.
2. PASS recorded as a comment on the issue with the schema below.
3. FAIL recorded analogously triggers an iteration spike.

**Manual-test comment schema (exact format — single line):**

```
manual-test status:<PASS|FAIL> reviewer:<name> commit:<sha> notes:<observations or step <N> — observed:<…> expected:<…>>
```

Field order is fixed. `reviewer:` is the short agent name (e.g.
`go-qa`) or human handle. `commit:` is the SHA the test
ran against (`git rev-parse origin/main` after `git fetch`). For
FAIL, `notes:` MUST start with `step <N> —` then observed-vs-expected.

**Deliverable:** Manual PASS or FAIL comment matching the schema above.

**Closure rule:** user-story issue closes only after the manual PASS
comment is posted (per the closure checklist in
`.github/ISSUE_TEMPLATE/user-story.yml`).

---

## 10. Integration

**Input:** Multiple plan PRs landed; user-stories closing.

**Do:**

1. Verify cross-context seams: read peer specs, run the smoke trace,
   confirm no spec invariants violated.
2. Update the per-context epic body to reflect closure status.
3. When all four sub-epics under a per-context epic close, the epic
   itself can be closed (its dependencies all resolved). Close it via
   a comment summarizing the trail of merged PRs.
4. Initiative #1 closes only when all child epics close.

**Deliverable:** Epic / initiative closure comments referencing
merged PRs and the relevant decision records.

---

## 11. Maintenance

**Input:** Open `kind:bug` plan issues, dep-bump chores, or post-MVP
feedback that touches MVP code.

**Do:**

1. Open `[PLAN]` issues for fixes; same flow as Implementation.
2. For dependency bumps (vcpkg baseline, submodule SHA): include
   the bump in the PR alongside any required code adjustments. Cite
   the upstream changelog in the PR body.
3. For post-MVP feedback that requires re-opening MVP scope:
   document the regression and decide via interview whether to fix
   in-place or schedule for the next milestone.

**Deliverable:** PR merged; issue closed; if regression, also a
follow-up plan opened against the affected MVP epic.

---

## Stage Order Cheat Sheet

```
ideation → breakdown → design ↔ iteration
                          ↓
                      planning
                          ↓
                       testing  (E2E authoring)
                          ↓
                  implementation ↔ review
                          ↓
                         QA
                          ↓
                     integration
                          ↓
                     maintenance
```

The double-headed arrows mean stages cycle until their gate holds.

---

## Cross-Stage Roles

The eleven stages above describe leaf execution paths. Three roles
operate orthogonally to those stages:

### Orchestration (`go-orchestrator`)

Drives one issue end-to-end across multiple stages. Does NOT replace
the leaf executors — it dispatches them. Use when:

- An issue spans more than one stage (e.g. an epic fragment that
  needs both design + planning + implementation under one driver).
- The user explicitly requests "orchestrate #N" / "drive #N
  end-to-end".
- An open issue carries the `orchestration:multi-stage` label.

The orchestrator counts as one /go top-level slot; its nested
children are unbounded. Lifecycle: inventory → orchestration-plan
comment → DoD authoring gate → stage dispatches → review pipeline
per PR → DoD verifier loop → final status:done. Closure happens
via merged PR + green dod-verify, never by `gh issue close`.

### Analysis (`go-thinker`)

Read-only deep-thinking specialist. Fires inside any stage when the
blocker is reasoning depth, not artifact production. Examples:

- During **maintenance**: hard-to-reproduce bug → thinker enumerates
  hypotheses before coding fix.
- During **iteration**: a recurring open question → thinker before
  the next `close-*-open-questions` spike.
- During **review** (round 2/3): an ambiguous design call surfaced
  by a comment → impl-respond delegates to thinker for the
  reasoning, then writes the response.

Output is a structured analysis (≥ 5 ranked hypotheses + refutation
+ recommended next dispatch). Thinker never writes code or opens
PRs. Manually summoned from chat or as a nested child.

### Chore (`go-chore`)

Low-effort mechanical worker. Fires for:

- **Maintenance** issues labelled `kind:chore` or titled `[CHORE]`.
- Any tiny mechanical sub-task spawned by another agent (e.g. a
  go-design agent delegates "add the new doc to specs/<ctx>/SPEC.md
  §5 references" to a chore child rather than burning opus
  thinking on the formatting).

Chore PRs go through the single-round review carve-out (one
`go-review` round, optional one `go-impl-respond` round, then
auto-merge) — see /go SKILL.md Step 5.
