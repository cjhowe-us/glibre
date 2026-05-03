# GitHub Recipes

Operational recipes for the `advance-plan` skill. All commands assume
`gh auth status` shows logged-in and `cwd = /Users/cjhowe/Code/glibre`.

---

## Topology — Up to 5 Unblocked Leaves

GraphQL query for open issues whose `blockedBy` count of OPEN issues
is 0, filtered to leaf types:

```bash
gh api graphql --paginate -f query='
query($endCursor: String) {
  repository(owner:"cjhowe-us", name:"glibre") {
    issues(states: OPEN, first: 100, after: $endCursor) {
      pageInfo { hasNextPage endCursor }
      nodes {
        number title
        labels(first: 5) { nodes { name } }
        blockedBy(first: 30) { nodes { state } }
      }
    }
  }
}' --jq '.data.repository.issues.nodes[]
  | select([.blockedBy.nodes[] | select(.state=="OPEN")] | length == 0)
  | select(any(.labels.nodes[].name; . == "type:spike" or . == "type:plan" or . == "type:user-story"))
  | "\(.number)\t\(([.labels.nodes[].name | select(startswith("type:"))][0]))\t\(.title)"' \
  | head -5
```

Returns up to 5 work-ready leaves, one per line:
`<number>\t<type>\t<title>`.

---

## Pick by SDLC Stage

Earlier-stage spikes first:

```bash
gh api graphql --paginate -f query='...' \
  --jq '<as above without head>' \
  | awk -F'\t' '
      $3 ~ /research-.*-responsibilities/    { p=1 }
      $3 ~ /research-.*-harmonius-mining/    { p=2 }
      $3 ~ /design-.*-aggregates/            { p=3 }
      $3 ~ /design-.*-public-interface/      { p=4 }
      $3 ~ /design-.*-persistence-schemas/   { p=5 }
      $3 ~ /design-.*-hot-reload-contract/   { p=6 }
      $3 ~ /design-.*-internal-architecture/ { p=7 }
      $3 ~ /design-.*-perf-budget/           { p=8 }
      $3 ~ /design-.*-failure-modes/         { p=9 }
      $3 ~ /draft-.*-user-stories/           { p=10 }
      $3 ~ /close-.*-open-questions/         { p=11 }
      $3 ~ /task-breakdown-.*/               { p=12 }
      $2 == "type:user-story"                { p=13 }
      $2 == "type:plan"                      { p=14 }
      { print p"\t"$0 }' \
  | sort -n | head -5 | cut -f2-
```

---

## Single Issue Lookup

```bash
gh issue view <N> --json number,title,labels,body,state
gh api repos/cjhowe-us/glibre/issues/<N>/dependencies/blocked_by \
  --jq '.[] | "\(.number)\t\(.state)\t\(.title)"'
gh api repos/cjhowe-us/glibre/issues/<N>/dependencies/blocking \
  --jq '.[] | "\(.number)\t\(.state)\t\(.title)"'
```

---

## Sub-Issue Parent (GraphQL)

```bash
PARENT_ID=$(gh api repos/cjhowe-us/glibre/issues/<P> --jq .node_id)
CHILD_ID=$(gh api repos/cjhowe-us/glibre/issues/<C> --jq .node_id)
gh api graphql -f query='mutation($p:ID!,$c:ID!){
  addSubIssue(input:{issueId:$p, subIssueId:$c}){issue{number}}
}' -f p="$PARENT_ID" -f c="$CHILD_ID"
```

---

## Add Issue Dependency (REST)

```bash
BLOCKER_ID=$(gh api repos/cjhowe-us/glibre/issues/<BLOCKER> --jq .id)
gh api -X POST "repos/cjhowe-us/glibre/issues/<TARGET>/dependencies/blocked_by" \
  -F issue_id="$BLOCKER_ID"
```

---

## Status Comment (full schema)

```bash
gh issue comment <N> --body "agent:<short-name>
status:done
issue:#<N>
branch:<branch>
worktree:/Users/cjhowe/Code/glibre
host:$(hostname -s)
cloud:none
commit:$(git rev-parse HEAD)
pr:#<PR_NUM>
notes:<one-paragraph English summary>"
```

---

## Branch + PR + Auto-merge

```bash
# Branch from current main
git fetch origin main
git checkout -b <type>/<scope>-<slug> origin/main

# … work …

git add -A
git commit -m "<type>(<scope>): <subject>"   # Conventional Commit
git push -u origin HEAD

PR_URL=$(gh pr create \
  --title "<type>(<scope>): <subject>" \
  --body "$(cat <<EOF
## Summary

<what this PR does>

## Refs

Refs: #<issue_number>

## Test plan

<unit / E2E / golden>
EOF
)" )
PR_NUM=$(echo "$PR_URL" | sed 's/.*\///')

# Auto-merge on green CI
gh pr merge "$PR_NUM" --auto --squash
```

---

## Verify Workflow Result

```bash
gh pr view <N> --json mergeStateStatus,statusCheckRollup \
  --jq '{state: .mergeStateStatus,
         checks: [.statusCheckRollup[] | {name, conclusion}]}'
```

---

## Open New Issue from Template Body

`gh issue create` cannot directly fill yml-form-typed templates. Use
`--body-file` whose content mirrors the template section structure:

```bash
cat > /tmp/body.md <<'EOF'
### Domain
core

### Phase
phase:mvp

### Story Points (Fibonacci)
pts:3

### Persona
engine developer

### User Story
As an engine developer, I want X, so that Y.

### Acceptance Criteria
Given …
When …
Then …

### Manual Test Script
1. …

### E2E Test Plan
tests/e2e/core/topic.glibre-trace + assertions
EOF
gh issue create --title "[STORY] core/<topic>" \
  --body-file /tmp/body.md \
  --label "type:user-story,phase:mvp,domain:core,pts:3"
```

---

## Counts — Sanity Check

```bash
# Open per type
for t in initiative epic sub-epic user-story plan spike; do
  printf "%-12s %s\n" "$t" \
    "$(gh issue list --state open --label type:$t --limit 500 --json number --jq 'length')"
done
```
