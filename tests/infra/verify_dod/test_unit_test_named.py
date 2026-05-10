"""tests/infra/verify_dod/test_unit_test_named.py

Pytest cases for verify_dod.check_unit_test_named.

These tests exercise the multi-line TEST_CASE fix introduced by plan #1035.
Each case:
  1. Writes a tiny fixture .cpp file into pytest's tmp_path.
  2. Monkey-patches the working directory so check_unit_test_named scans
     that fixture root instead of the real tests/ tree.
  3. Calls check_unit_test_named(name) and asserts the (ok, label) tuple.

Design note on monkey-patching:
  check_unit_test_named uses Path("tests") (a relative path) for its
  rglob scan.  We change the process working directory to tmp_path so
  that Path("tests") resolves to the fixture directory.  Each test
  restores cwd afterward via the monkeypatch fixture's teardown.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

# verify_dod is importable because conftest.py added .github/scripts/ to sys.path.
import verify_dod


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _write_cpp(tmp_path: Path, content: str) -> Path:
    """Write *content* to tmp_path/tests/bar_test.cpp and return the path."""
    tests_dir = tmp_path / "tests"
    tests_dir.mkdir(parents=True, exist_ok=True)
    cpp_file = tests_dir / "bar_test.cpp"
    cpp_file.write_text(content, encoding="utf-8")
    return cpp_file


# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------

def test_unit_test_named_matches_singleline_TEST_CASE(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """check_unit_test_named returns (True, label) for a single-line TEST_CASE."""
    name = "my/module: does_something_simple"
    _write_cpp(tmp_path, f'TEST_CASE("{name}", "[my][module]") {{\n}}\n')
    monkeypatch.chdir(tmp_path)

    ok, label = verify_dod.check_unit_test_named(name)

    assert ok is True
    assert f"unit_test_named: {name}" == label


def test_unit_test_named_matches_multiline_TEST_CASE(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """check_unit_test_named returns (True, label) for a TEST_CASE whose name
    is on the line after the opening parenthesis — the pattern clang-format
    produces when ColumnLimit: 100 causes the invocation to wrap.
    """
    name = "core/type_registry: lookup_unregistered_yields_TypeUnregistered"
    _write_cpp(
        tmp_path,
        f'TEST_CASE(\n    "{name}", "[core][type_registry]"\n) {{\n}}\n',
    )
    monkeypatch.chdir(tmp_path)

    ok, label = verify_dod.check_unit_test_named(name)

    assert ok is True
    assert f"unit_test_named: {name}" == label


def test_unit_test_named_matches_SCENARIO_alias(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """check_unit_test_named returns (True, label) for a SCENARIO macro.

    SCENARIO is a Catch2 alias for TEST_CASE that adds a "Scenario: " prefix
    to the display name.  The verifier's pattern matches both macros, so a
    SCENARIO with an exact name string should satisfy unit_test_named.
    """
    name = "user logs in with valid credentials"
    _write_cpp(tmp_path, f'SCENARIO("{name}", "[auth]") {{\n}}\n')
    monkeypatch.chdir(tmp_path)

    ok, label = verify_dod.check_unit_test_named(name)

    assert ok is True
    assert f"unit_test_named: {name}" == label


def test_unit_test_named_misses_when_name_absent(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """check_unit_test_named returns (False, label) when no TEST_CASE or
    SCENARIO with the queried name exists under tests/.
    """
    name = "nonexistent/test: this_name_does_not_appear_anywhere"
    _write_cpp(tmp_path, 'TEST_CASE("some/other: test_that_exists", "[tag]") {\n}\n')
    monkeypatch.chdir(tmp_path)

    ok, label = verify_dod.check_unit_test_named(name)

    assert ok is False
    assert f"unit_test_named: {name}" == label
