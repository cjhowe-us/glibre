# Specs

Bounded-context specs. Each MVP context owns one `<context>/SPEC.md`.

## Template sections

1. Purpose
2. Ubiquitous Language
3. Derived From (harmonius refs + collapse decisions)
4. Aggregates & Invariants
5. Public Interface
6. Internal Architecture (non-binding)
7. Persistence & Schemas
8. Hot-Reload Contract
9. Performance Budget
10. Failure Modes & Error Model
11. Acceptance Criteria (links to `type:user-story` GitHub issues)
12. Open Questions

A spec is **done** when:

- Acceptance criteria are mechanically testable.
- Public interface compiles as a header-only stub.
- Two peer contexts have reviewed seams.
- All MVP user-story issues for the context are linked.
