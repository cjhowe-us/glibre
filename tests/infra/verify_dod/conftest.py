"""conftest.py — pytest configuration for tests/infra/verify_dod/.

Adds .github/scripts/ to sys.path so that `import verify_dod` resolves
from any working directory, without requiring the package to be installed.

verify_dod.py reads several environment variables at module-level
(GITHUB_REPOSITORY, ISSUE_NUMBER, ISSUE_BODY).  We set sentinel values
before the first import so collection does not abort with KeyError.  The
individual test functions that exercise GitHub-API checks (pr_merged,
workflow_passed, issue_comment) must mock these further; the tests in
test_unit_test_named.py only exercise check_unit_test_named, which does
not reference those module-level constants.

The path is computed relative to this conftest.py file so it works
whether pytest is invoked from the repo root or from within tests/infra/.
"""

import os
import sys
from pathlib import Path

# Resolve .github/scripts/ relative to the repo root.
# This file lives at tests/infra/verify_dod/conftest.py, so:
#   repo_root = this_file.parent.parent.parent.parent
_repo_root = Path(__file__).resolve().parent.parent.parent.parent
_scripts_dir = _repo_root / ".github" / "scripts"

if str(_scripts_dir) not in sys.path:
    sys.path.insert(0, str(_scripts_dir))

# verify_dod reads GITHUB_REPOSITORY, ISSUE_NUMBER, and ISSUE_BODY at
# module-level.  Provide sentinel values so the import succeeds; tests that
# only call check_unit_test_named never access these constants.
os.environ.setdefault("GITHUB_REPOSITORY", "test-owner/test-repo")
os.environ.setdefault("ISSUE_NUMBER", "0")
os.environ.setdefault("ISSUE_BODY", "")
