#!/usr/bin/env python3
"""Evaluate the Definition-of-Done block of a GitHub issue against the
repo at HEAD (the workflow checks out `main`).

Inputs (env):
  GITHUB_REPOSITORY       owner/name
  ISSUE_NUMBER            issue number
  ISSUE_BODY              full issue body markdown
  GH_TOKEN                gh CLI token
Outputs (stdout):
  Markdown verdict block. Exit code 0 = all assertions pass,
  non-zero = at least one assertion failed (the workflow reopens).
"""

from __future__ import annotations

import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

try:
    import yaml  # type: ignore
except ImportError:
    sys.stderr.write("PyYAML missing — install in workflow runner\n")
    sys.exit(2)


REPO = os.environ["GITHUB_REPOSITORY"]
ISSUE_NUMBER = os.environ["ISSUE_NUMBER"]
ISSUE_BODY = os.environ.get("ISSUE_BODY", "")


def gh(*args: str) -> str:
    cmd = ["gh", *args]
    res = subprocess.run(cmd, check=True, capture_output=True, text=True)
    return res.stdout


def extract_block(body: str) -> str | None:
    m = re.search(
        r"^##\s+Definition of Done\b.*?\n+```yaml\s*\n(.*?)\n```",
        body,
        flags=re.DOTALL | re.MULTILINE | re.IGNORECASE,
    )
    return m.group(1) if m else None


def assertion_key(entry: dict | str) -> tuple[str, object]:
    if isinstance(entry, str):
        return entry, True
    if not isinstance(entry, dict) or len(entry) != 1:
        raise ValueError(f"malformed assertion entry: {entry!r}")
    [(k, v)] = entry.items()
    return k, v


def check_file_exists(arg: object) -> tuple[bool, str]:
    p = Path(str(arg))
    return p.is_file(), f"file_exists: {arg}"


def check_file_contains(arg: object) -> tuple[bool, str]:
    if not isinstance(arg, dict) or "path" not in arg or "regex" not in arg:
        return False, f"file_contains: malformed arg {arg!r}"
    p = Path(str(arg["path"]))
    label = f"file_contains: {arg['path']} :: /{arg['regex']}/"
    if not p.is_file():
        return False, f"{label} (file missing)"
    rc = subprocess.run(
        ["grep", "-Eq", str(arg["regex"]), str(p)],
        check=False,
    ).returncode
    return rc == 0, label


def check_glob_nonempty(arg: object) -> tuple[bool, str]:
    pattern = str(arg)
    out = subprocess.run(
        ["git", "ls-files", "--", pattern],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    return bool(out), f"glob_nonempty: {pattern}"


def check_pr_merged_closes_self(_arg: object) -> tuple[bool, str]:
    label = f"pr_merged_closes_self (#{ISSUE_NUMBER})"
    raw = gh(
        "pr", "list",
        "--repo", REPO,
        "--state", "merged",
        "--search", f"is:merged base:main #{ISSUE_NUMBER} in:body",
        "--json", "number,body,title",
        "--limit", "50",
    )
    prs = json.loads(raw or "[]")
    pat = re.compile(
        rf"\b(closes|fixes|resolves)\s+(?:[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)?#{ISSUE_NUMBER}\b",
        flags=re.IGNORECASE,
    )
    for pr in prs:
        if pat.search(pr.get("body") or ""):
            return True, f"{label} → PR #{pr['number']}"
    return False, f"{label} (no merged PR closes this issue)"


def check_workflow_passed(arg: object) -> tuple[bool, str]:
    name = str(arg)
    raw = gh(
        "run", "list",
        "--repo", REPO,
        "--workflow", name,
        "--branch", "main",
        "--limit", "1",
        "--json", "conclusion,status,databaseId",
    )
    runs = json.loads(raw or "[]")
    if not runs:
        return False, f"workflow_passed: {name} (no runs on main)"
    r = runs[0]
    ok = r.get("status") == "completed" and r.get("conclusion") == "success"
    return ok, f"workflow_passed: {name} → run {r.get('databaseId')} {r.get('conclusion')}"


def check_unit_test_named(arg: object) -> tuple[bool, str]:
    name = str(arg)
    label = f"unit_test_named: {name}"
    res = subprocess.run(
        [
            "grep", "-RE",
            rf'(TEST_CASE|SCENARIO)\s*\(\s*"{re.escape(name)}"',
            "tests/",
        ],
        check=False, capture_output=True, text=True,
    )
    return res.returncode == 0, label


def check_issue_comment_matches(arg: object) -> tuple[bool, str]:
    pattern = str(arg)
    label = f"issue_comment_matches: /{pattern}/"
    raw = gh(
        "issue", "view", ISSUE_NUMBER,
        "--repo", REPO,
        "--json", "comments",
    )
    data = json.loads(raw or "{}")
    rgx = re.compile(pattern, re.MULTILINE)
    for c in data.get("comments", []):
        author = (c.get("author") or {}).get("login") or ""
        if author.endswith("[bot]"):
            continue
        if rgx.search(c.get("body") or ""):
            return True, f"{label} → @{author}"
    return False, f"{label} (no matching comment)"


CHECKS = {
    "file_exists": check_file_exists,
    "file_contains": check_file_contains,
    "glob_nonempty": check_glob_nonempty,
    "pr_merged_closes_self": check_pr_merged_closes_self,
    "workflow_passed": check_workflow_passed,
    "unit_test_named": check_unit_test_named,
    "issue_comment_matches": check_issue_comment_matches,
}


def main() -> int:
    block = extract_block(ISSUE_BODY)
    if block is None:
        print("**DoD verifier: NO BLOCK FOUND.** "
              "This issue has no `## Definition of Done` section "
              "with a fenced ```yaml block. Add one per "
              "`.github/DOD-DSL.md` and re-run via `/verify-dod`.")
        return 1

    try:
        entries = yaml.safe_load(block) or []
    except yaml.YAMLError as e:
        print(f"**DoD verifier: YAML parse error.** `{e}`")
        return 1

    if not isinstance(entries, list) or not entries:
        print("**DoD verifier: EMPTY DoD list.** "
              "At least one assertion required (typically `pr_merged_closes_self: true`).")
        return 1

    results: list[tuple[bool, str]] = []
    for entry in entries:
        try:
            key, arg = assertion_key(entry)
        except ValueError as e:
            results.append((False, f"malformed entry: {e}"))
            continue
        check = CHECKS.get(key)
        if check is None:
            results.append((False, f"unknown assertion `{key}` — see .github/DOD-DSL.md"))
            continue
        try:
            results.append(check(arg))
        except subprocess.CalledProcessError as e:
            results.append((False, f"{key}: subprocess failed ({e})"))
        except Exception as e:  # noqa: BLE001
            results.append((False, f"{key}: {e}"))

    passed = sum(1 for ok, _ in results if ok)
    total = len(results)
    head = (
        f"**DoD verifier:** {passed}/{total} assertions passed for "
        f"#{ISSUE_NUMBER} against `main`."
    )
    lines = [head, ""]
    for ok, label in results:
        lines.append(f"- {'✅' if ok else '❌'} `{label}`")
    print("\n".join(lines))
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
