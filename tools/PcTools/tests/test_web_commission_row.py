"""Tests for web_commission_row.py's --dry-run path only.

This task must not contact the board (another session holds it), so only
the dry-run selector-validation logic is exercised here -- login/CDP live
mode is deliberately untested by this suite (see the module's own docstring
for why it shares the login/HTTP pattern already reviewed in
bench_test/cases_web_rw.py rather than inventing a new one).
"""
from __future__ import annotations

import os
import sys

import pytest

_HERE = os.path.dirname(__file__)
_SRC = os.path.normpath(os.path.join(_HERE, "..", "src"))
if _SRC not in sys.path:
    sys.path.insert(0, _SRC)

from kilnctrl import web_commission_row as wcr  # noqa: E402


def _repo_root() -> str:
    return os.path.normpath(os.path.join(_HERE, "..", "..", ".."))


@pytest.mark.parametrize("row_id", sorted(wcr.ROWS.keys()))
def test_dry_run_passes_for_every_known_row(row_id):
    ok, msg = wcr.dry_run(row_id, repo_root=_repo_root())
    assert ok, msg
    assert row_id in msg


def test_dry_run_unknown_row_fails_cleanly():
    ok, msg = wcr.dry_run("W_NOPE", repo_root=_repo_root())
    assert not ok
    assert "unknown row id" in msg


def test_dry_run_uses_default_repo_root_when_omitted():
    # No repo_root passed -- exercises web_commission_row's own _repo_root()
    # default resolution (the off-by-one bug fixed after Opus review of
    # 5fa097c7 landed exactly here: this file sits under the worktree, whose
    # repo root and the test file's own computed root must agree).
    ok, msg = wcr.dry_run("W2")
    assert ok, msg


def test_dry_run_reports_missing_source_file():
    bad = wcr.Row("WX", "/nope", "firmware/KilnFW/App/drivers/http/does_not_exist.html",
                   "", "page", "load a page that does not exist", "n/a", "read-only", "n/a")
    wcr.ROWS["WX"] = bad
    try:
        ok, msg = wcr.dry_run("WX", repo_root=_repo_root())
    finally:
        del wcr.ROWS["WX"]
    assert not ok
    assert "not found" in msg


def test_dry_run_reports_missing_selector():
    # main_page.html exists but has no element with this id -- negative
    # case proving the check actually inspects selector text, not just file
    # existence (see docs/agent_rules/IMPLEMENTER.md's negative-test rule).
    bad = wcr.Row("WY", "/", "firmware/KilnFW/App/drivers/http/main_page.html",
                   "thisIdDoesNotExistAnywhere12345", "id", "click a button that does not exist",
                   "n/a", "read-only", "n/a")
    wcr.ROWS["WY"] = bad
    try:
        ok, msg = wcr.dry_run("WY", repo_root=_repo_root())
    finally:
        del wcr.ROWS["WY"]
    assert not ok
    assert "not found" in msg


def test_all_row_source_files_exist():
    root = _repo_root()
    for row in wcr.ROWS.values():
        path = os.path.join(root, row.source_file.replace("/", os.sep))
        assert os.path.isfile(path), f"{row.row_id}: {row.source_file} missing"


def test_live_mode_requires_credentials(monkeypatch):
    monkeypatch.delenv("KILNCTL_WEB_USERNAME", raising=False)
    monkeypatch.delenv("KILNCTL_WEB_PASSWORD", raising=False)
    with pytest.raises(RuntimeError, match="not set"):
        wcr._read_credentials()


class _FakeProc:
    returncode = 0
    stdout = '{"ok": true}'
    stderr = ""


@pytest.mark.parametrize("fake_status,fake_body", [
    (401, {"error": "unauthorized"}),
    (404, {"error": "not found"}),
    (None, {"error": "connection refused"}),
])
def test_run_row_live_fails_when_readback_is_not_200(monkeypatch, fake_status, fake_body):
    # Never contact a real board -- every network-touching call is mocked.
    # This is the negative test for the required fix: a read-back that is
    # not a clean 200 must fail the row, not report PASS with the failure
    # text buried inside a "successful" message.
    monkeypatch.setattr(wcr, "validate_selector", lambda row: None)
    monkeypatch.setattr(wcr, "_read_credentials", lambda: ("u", "p"))
    monkeypatch.setattr(wcr, "_login_once", lambda host, user, pw: "fake-cookie")
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    monkeypatch.setattr(wcr, "_get_json_with_cookie", lambda host, path, cookie: (fake_status, fake_body))
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)

    ok, msg = wcr.run_row_live("W4", "192.0.2.1", "/tmp/whatever")
    assert not ok
    assert "FAIL" in msg
    assert str(fake_status) in msg


def test_run_row_live_passes_on_clean_readback(monkeypatch):
    monkeypatch.setattr(wcr, "validate_selector", lambda row: None)
    monkeypatch.setattr(wcr, "_read_credentials", lambda: ("u", "p"))
    monkeypatch.setattr(wcr, "_login_once", lambda host, user, pw: "fake-cookie")
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    monkeypatch.setattr(wcr, "_get_json_with_cookie", lambda host, path, cookie: (200, {"trip_reason": "none"}))
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)

    ok, msg = wcr.run_row_live("W4", "192.0.2.1", "/tmp/whatever")
    assert ok, msg
    assert "PASS" in msg


def test_run_row_live_passes_minimal_env_to_child(monkeypatch):
    # Advisory fix: only KC_SID plus a minimal allowlist should reach the
    # Chrome child, not this process's full environment.
    monkeypatch.setattr(wcr, "validate_selector", lambda row: None)
    monkeypatch.setattr(wcr, "_read_credentials", lambda: ("u", "p"))
    monkeypatch.setattr(wcr, "_login_once", lambda host, user, pw: "fake-cookie")
    monkeypatch.setattr(wcr, "_get_json_with_cookie", lambda host, path, cookie: (200, {}))
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setenv("KILNCTL_WEB_PASSWORD_UNRELATED_SECRET", "should-not-leak")

    captured = {}

    def fake_run(cmd, **kwargs):
        captured["env"] = kwargs.get("env")
        return _FakeProc()

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)
    wcr.run_row_live("W4", "192.0.2.1", "/tmp/whatever")

    assert captured["env"]["KC_SID"] == "fake-cookie"
    assert "KILNCTL_WEB_PASSWORD_UNRELATED_SECRET" not in captured["env"]
