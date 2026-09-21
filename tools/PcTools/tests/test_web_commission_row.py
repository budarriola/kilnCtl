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


def test_run_row_live_reuses_supplied_cookie_without_logging_in(monkeypatch):
    # Defect found running the first live class sweep: run_row_live() always
    # called _login_once, so running N rows in a class meant N logins --
    # against docs/agent_rules/BENCH.md's one-login-per-run rule and the
    # login lockout ladder. A caller running a whole class must be able to
    # log in once and pass the cookie into every row.
    def _boom(*a, **k):  # pragma: no cover - must never be called
        raise AssertionError("_login_once must not be called when a cookie is supplied")

    monkeypatch.setattr(wcr, "validate_selector", lambda row: None)
    monkeypatch.setattr(wcr, "_read_credentials", _boom)
    monkeypatch.setattr(wcr, "_login_once", _boom)
    monkeypatch.setattr(wcr, "_get_json_with_cookie", lambda host, path, cookie: (200, {}))
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)

    captured = {}

    def fake_run(cmd, **kwargs):
        captured["env"] = kwargs.get("env")
        return _FakeProc()

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)
    ok, msg = wcr.run_row_live("W4", "192.0.2.1", "/tmp/whatever", cookie="reused-cookie")

    assert ok, msg
    assert captured["env"]["KC_SID"] == "reused-cookie"


def test_run_row_live_still_logs_in_when_no_cookie_supplied(monkeypatch):
    # Backward-compat: a lone caller (no cookie argument) keeps the original
    # single-row behavior of logging in itself.
    calls = []
    monkeypatch.setattr(wcr, "validate_selector", lambda row: None)
    monkeypatch.setattr(wcr, "_read_credentials", lambda: ("u", "p"))
    monkeypatch.setattr(wcr, "_login_once", lambda host, user, pw: calls.append(1) or "fresh-cookie")
    monkeypatch.setattr(wcr, "_get_json_with_cookie", lambda host, path, cookie: (200, {}))
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr.run_row_live("W4", "192.0.2.1", "/tmp/whatever")
    assert ok, msg
    assert calls == [1]


def _capture_cmd(monkeypatch, row_id="W30", **kwargs):
    monkeypatch.setattr(wcr, "validate_selector", lambda row: None)
    monkeypatch.setattr(wcr, "_read_credentials", lambda: ("u", "p"))
    monkeypatch.setattr(wcr, "_login_once", lambda host, user, pw: "fake-cookie")
    monkeypatch.setattr(wcr, "_get_json_with_cookie", lambda host, path, cookie: (200, {}))
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    captured = {}

    def fake_run(cmd, **kw):
        captured["cmd"] = cmd
        captured["env"] = kw.get("env")
        return _FakeProc()

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)
    ok, msg = wcr.run_row_live(row_id, "192.0.2.1", "/tmp/whatever", **kwargs)
    assert ok, msg
    return captured


def test_cookie_never_appears_in_child_argv(monkeypatch):
    # The cookie travels in the child's environment only (KC_SID). A value
    # on argv is visible in a process listing and in any logged command
    # line, so this asserts it is nowhere in the argument vector -- the
    # reason main() has no --cookie flag either (see the next test).
    captured = _capture_cmd(monkeypatch, cookie="secret-cookie-value")
    assert captured["env"]["KC_SID"] == "secret-cookie-value"
    assert not any("secret-cookie-value" in str(tok) for tok in captured["cmd"]), captured["cmd"]


def test_main_rejects_a_cookie_on_the_command_line(monkeypatch):
    # Negative test for the argv rule: --cookie must not exist as a flag.
    # argparse exits 2 on an unknown option.
    with pytest.raises(SystemExit) as exc:
        wcr.main(["W2", "--host", "192.0.2.1", "--cookie", "secret-cookie-value"])
    assert exc.value.code == 2


def test_main_reuses_cookie_from_environment(monkeypatch):
    monkeypatch.setenv("KC_REUSE_SID", "env-cookie")
    seen = {}

    def fake_live(row_id, host, shots, cookie=None):
        seen["cookie"] = cookie
        return True, "ok"

    monkeypatch.setattr(wcr, "run_row_live", fake_live)
    assert wcr.main(["W2", "--host", "192.0.2.1"]) == 0
    assert seen["cookie"] == "env-cookie"


def test_write_row_gets_dialog_and_post_flags_read_only_row_does_not(monkeypatch):
    # W30's click opens a native confirm() and then POSTs /api/watchdog_cfg
    # asynchronously; the driver must be told both (accept the dialog, wait
    # for that POST to complete before teardown -- the 2026-09-21 W30 race).
    write_cmd = _capture_cmd(monkeypatch, "W30")
    assert "--accept-dialogs" in write_cmd["cmd"]
    assert "--expect-post" in write_cmd["cmd"]
    assert write_cmd["cmd"][write_cmd["cmd"].index("--expect-post") + 1] == "/api/watchdog_cfg"

    # A read-only row must NOT be allowed to answer a confirm() with OK:
    # dismissing unhangs the renderer without authorizing the action.
    ro_cmd = _capture_cmd(monkeypatch, "W28")
    assert "--accept-dialogs" not in ro_cmd["cmd"]
    assert "--expect-post" not in ro_cmd["cmd"]


def test_every_write_row_declares_the_post_it_expects():
    # A write row with no expect_post silently falls back to the old
    # "sleep 500ms then kill Chrome" behavior, which is exactly how W30's
    # write went missing. W1 is the one deliberate exception: the login form
    # submits as a navigation, not a fetch(), so the post-click network-quiet
    # wait covers it and an expect_post would be the wrong shape.
    missing = [r.row_id for r in wcr.ROWS.values()
               if r.classification == "write" and not r.expect_post and r.row_id != "W1"]
    assert missing == [], missing


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


# ---------------------------------------------------------------------------
# 2026-09-21 addition: ten read-only page-load rows (previously run "by
# hand" with no Row() entry, per docs/COMMISSIONING_TEST_MATRIX.md) plus
# one new write row, W31. dry_run coverage for all of these already comes
# free from test_dry_run_passes_for_every_known_row/test_all_row_source_files_exist
# above (both are parametrized/iterate over every ROWS entry); the tests
# below check the schema fields (classification, expect_post, restore
# wording) that those two do not.
# ---------------------------------------------------------------------------

_NEW_READ_ONLY_ROWS = ("W7", "W21", "W23", "W25", "W37", "W39", "W41", "W43", "W46", "W49")


@pytest.mark.parametrize("row_id", _NEW_READ_ONLY_ROWS)
def test_new_read_only_rows_are_classified_read_only_and_rendered(row_id):
    row = wcr.ROWS[row_id]
    assert row.classification == "read-only"
    ok, msg = wcr.dry_run(row_id, repo_root=_repo_root())
    assert ok, msg
    assert "class: read-only" in msg


@pytest.mark.parametrize("row_id", _NEW_READ_ONLY_ROWS)
def test_new_read_only_rows_declare_no_expect_post(row_id):
    # A read-only row never issues a state-changing POST, so it must not
    # declare one -- that field only exists to bound the wait for a write
    # row's async fetch() (see the Row.expect_post docstring).
    assert wcr.ROWS[row_id].expect_post is None


def test_w31_ramp_assist_toggle_is_a_write_row_with_expect_post_and_restore_note():
    row = wcr.ROWS["W31"]
    assert row.classification == "write"
    assert row.expect_post == "/api/ramp_assist"
    assert row.verify_endpoint == "/api/ramp_assist"
    assert "restore" in row.readback_desc.lower()


def test_w31_gets_accept_dialogs_and_expect_post_flags(monkeypatch):
    captured = _capture_cmd(monkeypatch, "W31")
    assert "--accept-dialogs" in captured["cmd"]
    assert "--expect-post" in captured["cmd"]
    assert captured["cmd"][captured["cmd"].index("--expect-post") + 1] == "/api/ramp_assist"


# All ten, not a [:3] sample: the flag is derived per row from that row's
# own classification, so sampling three of ten would leave seven rows'
# dialog policy unchecked for no saving worth having (each case is one
# mocked subprocess.run, no board, no browser).
@pytest.mark.parametrize("row_id", _NEW_READ_ONLY_ROWS)
def test_new_read_only_rows_get_no_dialog_flag_via_capture(monkeypatch, row_id):
    captured = _capture_cmd(monkeypatch, row_id)
    assert "--accept-dialogs" not in captured["cmd"]
    assert "--expect-post" not in captured["cmd"]
