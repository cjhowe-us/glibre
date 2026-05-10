"""conftest.py — pytest configuration for tests/infra/verify_dod/.

Adds .github/scripts/ to sys.path so that `import verify_dod` resolves
from any working directory, without requiring the package to be installed.

verify_dod.py reads GITHUB_REPOSITORY, ISSUE_NUMBER, and ISSUE_BODY at
module-level during the first import.  We must set sentinel values before
pytest collection (which triggers test-file imports), so we use the
``pytest_configure`` hook — the earliest conftest hook that executes before
collection.  ``pytest_unconfigure`` is the symmetric counterpart: it pops
the same keys at session end so they do not leak into the calling process.

After import, a function-scoped autouse fixture re-applies the sentinels
via ``monkeypatch.setenv`` for each test so any test that overrides them
gets a clean environment at teardown.  Future tests that exercise the
GitHub-API checks (pr_merged, workflow_passed, issue_comment) should
override these per-test via additional ``monkeypatch.setenv`` calls.

The path is computed relative to this conftest.py file so it works
whether pytest is invoked from the repo root or from within tests/infra/.
"""

import sys
from pathlib import Path

import pytest

# Resolve .github/scripts/ relative to the repo root.
# This file lives at tests/infra/verify_dod/conftest.py, so:
#   repo_root = this_file.parent.parent.parent.parent
_repo_root = Path(__file__).resolve().parent.parent.parent.parent
_scripts_dir = _repo_root / ".github" / "scripts"

if str(_scripts_dir) not in sys.path:
    sys.path.insert(0, str(_scripts_dir))

_SENTINEL_ENV: dict[str, str] = {
    "GITHUB_REPOSITORY": "test-owner/test-repo",
    "ISSUE_NUMBER": "0",
    "ISSUE_BODY": "",
}


def pytest_configure(config: pytest.Config) -> None:  # noqa: ARG001
    """Set sentinel env vars before collection so ``import verify_dod`` succeeds.

    verify_dod reads GITHUB_REPOSITORY, ISSUE_NUMBER, and ISSUE_BODY at
    module-level; this hook runs before test-file collection, ensuring those
    vars are present when pytest imports the test modules.
    """
    import os

    for key, value in _SENTINEL_ENV.items():
        os.environ.setdefault(key, value)


def pytest_unconfigure(config: pytest.Config) -> None:  # noqa: ARG001
    """Remove the sentinel env vars set by ``pytest_configure`` at session end.

    Symmetric teardown: each key that ``pytest_configure`` wrote via
    ``setdefault`` is popped here so the vars do not leak into any parent
    process or shell that captures the pytest subprocess env.  Keys already
    absent (e.g. if they were set externally) are not touched — ``pop``
    with a default is safe here.
    """
    import os

    for key in _SENTINEL_ENV:
        os.environ.pop(key, None)


@pytest.fixture(autouse=True)
def _verify_dod_env(monkeypatch: pytest.MonkeyPatch) -> None:
    """Re-apply sentinel env vars for every test via monkeypatch.

    monkeypatch.setenv undoes each mutation at function teardown, so tests
    that override these vars (e.g. future GitHub-API check tests) do not
    bleed state into subsequent tests.
    """
    for key, value in _SENTINEL_ENV.items():
        monkeypatch.setenv(key, value)
