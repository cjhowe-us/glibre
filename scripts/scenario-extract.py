#!/usr/bin/env python3
"""Mine harmonius user-stories into draft GitHub issues for glibre.

Reads /Users/cjhowe/Code/harmonius/docs/user-stories/**/*.md, parses
`US-X.Y.Z` blocks, and emits one JSON record per story to stdout. The
records are intended to be POSTed via `gh api` to create
type:user-story issues, but are not posted automatically — review and
filter before creation.

Usage:
    scripts/scenario-extract.py > stories.jsonl

TODO: implement parsing + bucketing once GitHub repo provisioned.
"""

from __future__ import annotations
import sys


def main() -> int:
    print("scenario-extract: not yet implemented", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
