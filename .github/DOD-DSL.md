# Definition-of-Done (DoD) DSL

Every leaf issue (`type:plan`, `type:spike`, `type:user-story`) carries a `## Definition of Done`
section. The body of that section is a single fenced YAML block whose top level is a list of
assertions. The `dod-verify` GitHub Action (`.github/workflows/dod-verify.yml`) parses and evaluates
the block against the repository at `main` whenever the issue is closed (or when `/verify-dod` is
commented). If any assertion fails the action reopens the issue and posts the failure list.

The DSL is intentionally small. New assertion kinds are added only when they cannot be expressed as
a composition of existing ones.

## Block location & format

~~~markdown
## Definition of Done

```yaml
- pr_merged_closes_self: true
- file_exists: specs/core/world-design.md
- workflow_passed: ci.yml
```
~~~

The verifier uses the **first** fenced ```yaml``` block following the `## Definition of Done`
heading. Anything outside the block is ignored.

## Assertion grammar

Each list entry is an object with a single key. Supported keys:

| Key                     | Argument                          | Meaning                                                                                                                    |
| ----------------------- | --------------------------------- | -------------------------------------------------------------------------------------------------------------------------- |
| `pr_merged_closes_self` | `true`                            | At least one merged PR into `main` whose body or commit messages contain `closes #<this-issue>` (or `fixes` / `resolves`). |
| `file_exists`           | path (string)                     | Path is a regular file at `main` HEAD.                                                                                     |
| `file_contains`         | `{ path, regex }`                 | File exists AND a POSIX-extended regex matches at least one line.                                                          |
| `glob_nonempty`         | glob (string)                     | At least one path matches the glob (uses `git ls-files` so it sees only tracked files).                                    |
| `workflow_passed`       | workflow filename or display name | The most recent run of that workflow on the `main` branch concluded `success`.                                             |
| `unit_test_named`       | Rust test fn name (snake_case)    | A `#[test]` (or `#[tokio::test]` etc.) fn with that exact identifier appears anywhere in the workspace.                    |
| `issue_comment_matches` | POSIX-ext regex (string)          | At least one comment on this issue (excluding bots flagged with `[bot]`) matches the regex.                                |

All paths are relative to the repository root. All regexes are matched with `grep -E`. All checks
run in O(repo); none invoke a project build.

## Authoring rules

- Every leaf MUST include `pr_merged_closes_self: true` so the closure is anchored to a merge into
  `main`.
- Spikes whose deliverable is a doc MUST include a `file_exists` for that doc, and SHOULD include a
  `file_contains` checking the spec template's section-1 heading.
- Plans MUST include either a `unit_test_named` for each Rust test fn named in their unit-test plan,
  OR a `workflow_passed: ci.yml` (the latter is acceptable when the plan's tests live in an existing
  test-suite the CI already runs).
- User stories MUST include the trace file via `file_exists`, `workflow_passed: ci.yml`, and an
  `issue_comment_matches` for the manual PASS line (`^manual-test:PASS`).

## Authoring agents

Subagents dispatched by the `/go` skill that **author leaf issues** (planning bucket:
`draft-*-user-stories`, `task-breakdown-*`) are required to populate the DoD block on every issue
they create. Other agents must update the DoD block when their work changes the deliverable contract
— never the closure rule. The DoD block is the single source of truth for closure: if the verifier
passes, the issue closes; if it fails, the issue reopens. There is no human-only override.
