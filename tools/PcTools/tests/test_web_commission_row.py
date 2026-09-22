"""Tests for web_commission_row.py's --dry-run path only.

This task must not contact the board (another session holds it), so only
the dry-run selector-validation logic is exercised here -- login/CDP live
mode is deliberately untested by this suite (see the module's own docstring
for why it shares the login/HTTP pattern already reviewed in
bench_test/cases_web_rw.py rather than inventing a new one).
"""
from __future__ import annotations

import dataclasses
import json
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


# ---------------------------------------------------------------------------
# 2026-09-21 addition: the `fills` primitive (a list of {selector, value}
# applied to the page before the click, via CDP Runtime.evaluate setting
# .value and dispatching input/change) plus three newly-wired write rows
# that use it -- W22 (safety config), W38 (display brightness), both via the
# generic read-fill-save-confirm-restore-confirm helper
# (_run_fill_and_restore), and W42 (kiln config save-as-new + delete), via
# its own dedicated helper (_run_kiln_config_create_delete). None of this
# touches a real board -- every network/subprocess call is mocked.
# ---------------------------------------------------------------------------

def test_w22_and_w38_declare_fills_and_restore_from_field():
    for row_id, selector, endpoint, field in (
        ("W22", "#pcLink", "/api/zones", "pc_link_abort_silence_ms"),
        ("W38", "#kcDpBrightness", "/api/settings/display_power", "brightness_percent"),
    ):
        row = wcr.ROWS[row_id]
        assert row.classification == "write"
        assert row.verify_endpoint == endpoint
        assert row.expect_post == endpoint
        assert row.fills and row.fills[0][0] == selector
        assert row.restore_from_field == (field,)


def test_w42_declares_special_create_delete_shape():
    row = wcr.ROWS["W42"]
    assert row.classification == "write"
    assert row.special == "kiln_config_create_delete"
    assert row.verify_endpoint == "/api/kiln_configs"
    assert row.expect_post == "/api/kiln_configs/save"
    assert "kcSaveNewName" in row.fills[0][0]
    assert "kilnConfigSelect" in row.special_selectors
    assert "kcDeleteBtn" in row.special_selectors


def test_run_cdp_renders_fills_as_json_arg(monkeypatch):
    monkeypatch.setattr(wcr, "_get_json_with_cookie", lambda host, path, cookie: (200, {}))
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    captured = {}

    def fake_run(cmd, **kw):
        captured["cmd"] = cmd
        return _FakeProc()

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)
    row = wcr.ROWS["W22"]
    proc = wcr._run_cdp(row, "192.0.2.1", "/tmp/whatever", "fake-cookie", fills=row.fills)
    assert proc.returncode == 0
    assert "--fills" in captured["cmd"]
    import json as _json
    fills_arg = captured["cmd"][captured["cmd"].index("--fills") + 1]
    assert _json.loads(fills_arg) == [{"selector": "#pcLink", "value": "54000"}]


def _sequential_get_json(responses):
    it = iter(responses)

    def fake(host, path, cookie):
        return next(it)

    return fake


def _get_json_by_path(responses_by_path):
    """Like `_sequential_get_json`, but for a function under test that GETs
    more than one endpoint (W50's `_run_setup_wizard_step1`, which reads
    both `/api/status` and `/api/setup/progress`) -- each call consumes the
    next queued response for ITS OWN path, so the test can declare each
    endpoint's sequence independently instead of interleaving them by call
    order."""
    iters = {path: iter(responses) for path, responses in responses_by_path.items()}

    def fake(host, path, cookie):
        return next(iters[path])

    return fake


_DEFAULT_PROGRESS_BODY = {"version": 1, "steps": {"1": {"state": "pending", "ts": 0, "note": ""}}}


def _progress_body(state="pending", note="", ts=0):
    return {"version": 1, "steps": {"1": {"state": state, "ts": ts, "note": note}}}


def _zones_body(pclink, **guard_overrides):
    """A /api/zones response body carrying both W22's edited field and the
    three top-level counts W22 declares as `guard_fields` -- the page posts
    all of them in one whole-page submit, so a realistic fixture has to
    include them or the guard check has nothing to compare."""
    body = {"pc_link_abort_silence_ms": pclink,
            "thermo_count": 3, "relay_count": 4, "max_simultaneous_relays": 2}
    body.update(guard_overrides)
    return body


def test_fill_and_restore_full_flow_passes(monkeypatch):
    # Pre-read shows the original value, post-set read-back shows it changed,
    # post-restore read-back shows it back to the original -- three distinct
    # GETs, none of which may be skipped or reused stale.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, _zones_body(30000)),
            (200, _zones_body(54000)),
            (200, _zones_body(30000)),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_fill_and_restore(wcr.ROWS["W22"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert ok, msg
    assert "restored" in msg


def test_fill_and_restore_fails_if_value_never_changes(monkeypatch):
    # The write silently didn't land -- must FAIL, not report PASS with a
    # restore of a value that was never actually different. The restoring
    # Save still runs and is still read back, even though the flip itself
    # never landed.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, _zones_body(30000)),
            (200, _zones_body(30000)),
            (200, _zones_body(30000)),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    runs = []
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: runs.append(1) or _FakeProc())

    ok, msg = wcr._run_fill_and_restore(wcr.ROWS["W22"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "did not change" in msg
    assert len(runs) == 2, "restore Save must run even when the mid-state check fails"
    assert "restored" in msg


def test_fill_and_restore_fails_loud_if_restore_does_not_take(monkeypatch):
    # Restore POST "succeeded" (CDP exit 0) but the read-back after it still
    # shows the test value -- must fail loud rather than report PASS, since
    # this is exactly the "board left with the test value" hazard.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, _zones_body(30000)),
            (200, _zones_body(54000)),
            (200, _zones_body(54000)),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_fill_and_restore(wcr.ROWS["W22"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "RESTORE FAILED" in msg
    assert "LEFT ON BOARD" in msg


def test_kiln_config_create_delete_full_flow_passes(monkeypatch):
    # New (2026-09-21/22) shape: pre-read, create, re-select (Apply) the
    # ORIGINAL active id back (firmware marks the new slot active, and
    # refuses to delete the active one), poll apply_status, delete the
    # throwaway, final read confirms both "gone" and "active_id restored".
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"}]}),
            (200, {"active_id": "7", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "7", "name": "kc_test_123"}]}),
            (200, {"state": "done_ok"}),
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "7", "name": "kc_test_123"}]}),
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"}]}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    monkeypatch.setattr(wcr.time, "time", lambda: 123)
    monkeypatch.setattr(wcr.time, "sleep", lambda *a: None)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert ok, msg
    assert "deleted" in msg
    assert "re-selected original active id=1" in msg


def test_kiln_config_create_delete_fails_if_left_on_board(monkeypatch):
    # The final read-back after a "successful" delete POST still lists the
    # throwaway config -- must fail loud and say so, never silently report
    # success.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"}]}),
            (200, {"active_id": "7", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "7", "name": "kc_test_123"}]}),
            (200, {"state": "done_ok"}),
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "7", "name": "kc_test_123"}]}),
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "7", "name": "kc_test_123"}]}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    monkeypatch.setattr(wcr.time, "time", lambda: 123)
    monkeypatch.setattr(wcr.time, "sleep", lambda *a: None)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "LEFT ON BOARD" in msg


def test_kiln_config_create_delete_refuses_upfront_if_generated_name_already_listed(monkeypatch):
    # Advisory carried from an earlier review: the leftover sweep only
    # cleans up leftovers[0] -- a SECOND leftover-shaped name that happens to
    # equal the generated unique_name ("kc_test_123" when time.time() is
    # mocked to 123) survives that sweep and must still be caught before the
    # create click, not left to the board's own duplicate-name 400. Two
    # subprocess.run calls are allowed (the one cleanup delete of
    # leftovers[0]); a third call would mean the create was attempted
    # despite the collision.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"active_id": "1", "configs": [
                {"id": "1", "name": "existing"},
                {"id": "5", "name": "kc_test_100"},   # leftovers[0]: gets cleaned up
                {"id": "9", "name": "kc_test_123"},   # collides with the generated name
            ]}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)

    calls = {"n": 0}

    def fake_run(*a, **k):
        calls["n"] += 1
        if calls["n"] == 1:
            return _FakeProc()  # cleanup delete of leftovers[0] ("kc_test_100")
        raise AssertionError("must not touch the board again once the name collision is found")

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)
    monkeypatch.setattr(wcr.time, "time", lambda: 123)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "already listed" in msg
    assert "kc_test_123" in msg
    assert calls["n"] == 1


def test_kiln_config_create_delete_same_second_rerun_does_not_self_collide(monkeypatch):
    # F2 fix: the pre-read `configs` list is captured BEFORE the leftover
    # sweep deletes leftovers[0]. On a same-second re-run, the just-deleted
    # leftover's own name ("kc_test_123", time.time() mocked to 123) equals
    # the freshly generated unique_name -- the "already listed" upfront
    # collision check must exclude that just-deleted id, or this refuses a
    # name that no longer exists on the board.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"active_id": "1", "configs": [
                {"id": "1", "name": "existing"},
                {"id": "5", "name": "kc_test_123"},   # leftovers[0]: same name as the new one
            ]}),
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "7", "name": "kc_test_123"}]}),
            (200, {"state": "done_ok"}),
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "7", "name": "kc_test_123"}]}),
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"}]}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    monkeypatch.setattr(wcr.time, "time", lambda: 123)
    monkeypatch.setattr(wcr.time, "sleep", lambda *a: None)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert ok, msg
    assert "deleted pre-existing leftover 'kc_test_123'" in msg


def test_kiln_config_create_delete_cleans_up_inactive_leftover_first(monkeypatch):
    # A leftover from a previous incomplete run (not the active config) is
    # deleted before the real run starts, and reported as having done so.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "5", "name": "kc_test_999"}]}),
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "7", "name": "kc_test_123"}]}),
            (200, {"state": "done_ok"}),
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "7", "name": "kc_test_123"}]}),
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"}]}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    monkeypatch.setattr(wcr.time, "time", lambda: 123)
    monkeypatch.setattr(wcr.time, "sleep", lambda *a: None)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert ok, msg
    assert "deleted pre-existing leftover 'kc_test_999'" in msg


def test_kiln_config_create_delete_cleans_up_active_leftover_via_fallback(monkeypatch):
    # The leftover itself is the currently-active config (a previous run
    # died between create and its own restore step). There is no recorded
    # "original" to go back to, so this selects some OTHER existing config
    # first (fallback), then deletes the leftover, then proceeds with the
    # real run using the fallback as this run's "original active id".
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"active_id": "5", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "5", "name": "kc_test_999"}]}),
            (200, {"state": "done_ok"}),
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "5", "name": "kc_test_999"}]}),
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "7", "name": "kc_test_123"}]}),
            (200, {"state": "done_ok"}),
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "7", "name": "kc_test_123"}]}),
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"}]}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    monkeypatch.setattr(wcr.time, "time", lambda: 123)
    monkeypatch.setattr(wcr.time, "sleep", lambda *a: None)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert ok, msg
    assert "selected id=1 instead" in msg
    assert "deleted pre-existing leftover 'kc_test_999'" in msg
    assert "re-selected original active id=1" in msg


def test_kiln_config_create_delete_refuses_when_active_leftover_has_no_fallback(monkeypatch):
    # The leftover is active AND is the only config on the board -- nothing
    # exists to select instead, so this must refuse rather than guess.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"active_id": "5", "configs": [{"id": "5", "name": "kc_test_999"}]}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    monkeypatch.setattr(wcr.time, "time", lambda: 123)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "no other config exists" in msg


def test_kiln_config_create_delete_attempts_best_effort_delete_when_restore_apply_fails(monkeypatch):
    # The re-select (Apply) of the original active id after create comes
    # back a clean 500 -- this must still attempt to delete the throwaway
    # slot (best effort) and report exactly what, if anything, is left.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"}]}),
            (200, {"active_id": "7", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "7", "name": "kc_test_123"}]}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.time, "time", lambda: 123)

    class _OkProc:
        returncode = 0
        stdout = json.dumps({"ok": True, "post": None})
        stderr = ""

    class _RejectedApplyProc:
        returncode = 0
        stdout = json.dumps({
            "ok": True,
            "post": {"method": "POST", "url": "http://x/api/kiln_configs/apply",
                      "status": 500, "failed": False},
        })
        stderr = ""

    def fake_run(cmd, **kw):
        if "kcApplyBtn" in cmd:
            return _RejectedApplyProc()
        return _OkProc()

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "re-select of original active id=1" in msg
    assert "500" in msg
    assert "/api/kiln_configs/apply" in msg
    assert "nothing (throwaway config was still deleted)" in msg


def test_kiln_config_create_delete_reports_delete_400(monkeypatch):
    # The delete POST (of the throwaway, after a successful restore-apply)
    # comes back a clean 400 -- must be reported by URL/status, and the
    # throwaway must be reported LEFT ON BOARD, never silently swallowed.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"}]}),
            (200, {"active_id": "7", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "7", "name": "kc_test_123"}]}),
            (200, {"state": "done_ok"}),
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "7", "name": "kc_test_123"}]}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.time, "time", lambda: 123)
    monkeypatch.setattr(wcr.time, "sleep", lambda *a: None)

    class _OkProc:
        returncode = 0
        stdout = json.dumps({"ok": True, "post": None})
        stderr = ""

    class _RejectedDeleteProc:
        returncode = 0
        stdout = json.dumps({
            "ok": True,
            "post": {"method": "POST", "url": "http://x/api/kiln_configs/delete",
                      "status": 400, "failed": False},
        })
        stderr = ""

    def fake_run(cmd, **kw):
        if "kcDeleteBtn" in cmd:
            return _RejectedDeleteProc()
        return _OkProc()

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "400" in msg
    assert "/api/kiln_configs/delete" in msg
    assert "LEFT ON BOARD" in msg


def test_kiln_config_create_delete_refuses_leftover_as_fallback(monkeypatch):
    # The active leftover's only companion is ANOTHER kc_test_* leftover.
    # Applying that one would make a throwaway of unknown provenance the
    # board's LIVE kiln config (apply rewrites relay wiring, thermocouple
    # assignment, PID gains and guard thresholds) -- must refuse instead.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"active_id": "5", "configs": [{"id": "5", "name": "kc_test_999"},
                                                  {"id": "6", "name": "kc_test_998"}]}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.time, "time", lambda: 123)

    calls = []

    def fake_run(cmd, **kw):
        calls.append(cmd)
        raise AssertionError("no CDP action may run: there is no safe fallback to apply")

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "not itself a test leftover" in msg
    assert calls == []


def test_kiln_config_create_delete_reports_diverged_and_writes_nothing_further(monkeypatch):
    # The restore-apply ends DIVERGED (kiln_cfg_swap.c's alarmed exit:
    # config left pending for retry; heat off only on the ceiling-latch
    # branch, which is the `reason` used below). That must be named
    # in the failure, and NO further write (no delete) may be issued.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"}]}),
            (200, {"active_id": "7", "configs": [{"id": "1", "name": "existing"},
                                                  {"id": "7", "name": "kc_test_123"}]}),
            (200, {"state": "done_failed", "diverged": True,
                   "reason": "post-swap ceiling/arming check failed"}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.time, "time", lambda: 123)
    monkeypatch.setattr(wcr.time, "sleep", lambda *a: None)

    seen = []

    class _OkProc:
        returncode = 0
        stdout = json.dumps({"ok": True, "post": None})
        stderr = ""

    def fake_run(cmd, **kw):
        seen.append("kcDeleteBtn" if "kcDeleteBtn" in cmd else "other")
        return _OkProc()

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "DIVERGED" in msg
    assert "post-swap ceiling/arming check failed" in msg
    assert "NO further write attempted" in msg
    assert "LEFT ON BOARD" in msg
    assert "kcDeleteBtn" not in seen


def test_kiln_config_create_delete_reports_apply_still_running(monkeypatch):
    # apply_status never leaves "running" inside the ~60 s budget: the
    # two-processor swap may still be in flight, so no further write may be
    # issued and the message must say the active config is not confirmed.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json(
            [(200, {"active_id": "1", "configs": [{"id": "1", "name": "existing"}]}),
             (200, {"active_id": "7", "configs": [{"id": "1", "name": "existing"},
                                                   {"id": "7", "name": "kc_test_123"}]})]
            + [(200, {"state": "running", "diverged": False, "reason": ""})] * 60
        ),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.time, "time", lambda: 123)
    monkeypatch.setattr(wcr.time, "sleep", lambda *a: None)

    seen = []

    class _OkProc:
        returncode = 0
        stdout = json.dumps({"ok": True, "post": None})
        stderr = ""

    def fake_run(cmd, **kw):
        seen.append("kcDeleteBtn" if "kcDeleteBtn" in cmd else "other")
        return _OkProc()

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "still reports state=running" in msg
    assert "NO further write attempted" in msg
    assert "kcDeleteBtn" not in seen


def test_kiln_config_create_delete_fails_if_create_never_landed(monkeypatch):
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"configs": []}),
            (200, {"configs": []}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    monkeypatch.setattr(wcr.time, "time", lambda: 123)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "write did not land" in msg


def _generated_kiln_config_name(mod, monkeypatch):
    """Captures the name _run_kiln_config_create_delete() actually puts into
    #kcSaveNewName, by intercepting its _run_cdp() call -- rather than
    restating the format string here, which would pass even if the
    generator changed."""
    seen = {}

    class _Proc:
        returncode = 1
        stdout = ""
        stderr = "stopped"

    def _capture(row, host, shot_dir, cookie, **kw):
        seen["fills"] = kw.get("fills")
        return _Proc()

    monkeypatch.setattr(mod, "_get_json_with_cookie", lambda *a, **k: (200, {"configs": []}))
    monkeypatch.setattr(mod.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(mod, "_run_cdp", _capture)
    mod._run_kiln_config_create_delete(mod.ROWS["W42"], "h", "d", "c")
    assert seen["fills"] and seen["fills"][0][0] == "#kcSaveNewName"
    return seen["fills"][0][1]


def test_kiln_config_create_delete_generated_name_fits_firmware_limit(monkeypatch):
    # Root cause of the 2026-09-21 W42 live FAIL (docs/audits/
    # w42_kiln_config_create_2026-09-21.md): the old 38-char generated name
    # overflowed KILN_CFG_NAME_MAX_LEN (23, kiln_cfg_store.h) and the
    # board's save handler correctly rejected it 400. Pin the generated
    # name's length under the mirrored constant so this can't regress.
    # Drives the real generator (via a monkeypatched clock) rather than
    # restating its formula, so a change to the format is caught here.
    for t in (0, 1_790_032_582, 9_999_999_999):
        monkeypatch.setattr(wcr.time, "time", lambda t=t: t)
        name = _generated_kiln_config_name(wcr, monkeypatch)
        assert name.startswith("kc_test_")
        assert len(name) <= wcr._KILN_CFG_NAME_MAX_LEN, name


def test_kiln_config_create_delete_reports_real_status_on_rejected_create(monkeypatch):
    # A clean 400 from the save POST (the exact shape the 2026-09-21 audit
    # found) must be reported as a real status, not the generic
    # cause-blind "write did not land" the pre-fix runner produced.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"configs": []}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)

    class _RejectedCreateProc:
        returncode = 0
        stdout = json.dumps({
            "ok": True, "route": "/settings/kiln_configs",
            "post": {"method": "POST", "url": "http://x/api/kiln_configs/save",
                      "status": 400, "failed": False},
        })
        stderr = ""

    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _RejectedCreateProc())
    monkeypatch.setattr(wcr.time, "time", lambda: 123)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "400" in msg
    assert "/api/kiln_configs/save" in msg


def test_kiln_config_create_delete_reports_network_failed_create(monkeypatch):
    # _web_commission_cdp.mjs records {status: null, failed: true} for a POST
    # that never got a response (Network.loadingFailed, line 201-205): status
    # alone cannot distinguish that from "not recorded", so `failed` must be
    # inspected too, or this falls through to "write did not land" again.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([(200, {"configs": []})]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)

    class _FailedCreateProc:
        returncode = 0
        stdout = json.dumps({
            "ok": True, "route": "/settings/kiln_configs", "selector": "kcSaveNewBtn",
            "fills": [], "dialogs": [],
            "post": {"method": "POST", "url": "http://x/api/kiln_configs/save",
                      "status": None, "failed": True},
        })
        stderr = ""

    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FailedCreateProc())
    monkeypatch.setattr(wcr.time, "time", lambda: 123)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/x", "c")
    assert not ok
    assert "failed=True" in msg
    assert "write did not land" not in msg


def test_cdp_post_status_ignores_driver_preamble_line():
    # The driver prints a plain-text line before its JSON when the CDP port
    # it was given was busy (_web_commission_cdp.mjs:359), so only the LAST
    # stdout line is JSON.
    class _Proc:
        stdout = ("_web_commission_cdp: CDP port 9333 was busy, using 9334 instead\n"
                  + json.dumps({"ok": True, "post": {"method": "POST", "url": "u",
                                                      "status": 200, "failed": False}}) + "\n")
    assert wcr._cdp_post_status(_Proc()) == {"method": "POST", "url": "u",
                                              "status": 200, "failed": False}


def test_cdp_post_status_none_on_empty_or_nonjson_stdout():
    class _Empty:
        stdout = ""

    class _NonJson:
        stdout = "node: command failed\n"

    class _NoPost:
        stdout = json.dumps({"ok": True, "post": None})

    assert wcr._cdp_post_status(_Empty()) is None
    assert wcr._cdp_post_status(_NonJson()) is None
    assert wcr._cdp_post_status(_NoPost()) is None


def test_run_row_live_dispatches_to_fill_and_restore_for_w22(monkeypatch):
    monkeypatch.setattr(wcr, "validate_selector", lambda row: None)
    monkeypatch.setattr(wcr, "_read_credentials", lambda: ("u", "p"))
    monkeypatch.setattr(wcr, "_login_once", lambda host, user, pw: "fake-cookie")
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    seen = {}

    def fake_fill_restore(row, host, screenshot_dir, cookie):
        seen["called"] = row.row_id
        return True, "ok"

    monkeypatch.setattr(wcr, "_run_fill_and_restore", fake_fill_restore)
    ok, msg = wcr.run_row_live("W22", "192.0.2.1", "/tmp/whatever")
    assert ok, msg
    assert seen["called"] == "W22"


def test_run_row_live_dispatches_to_create_delete_for_w42(monkeypatch):
    monkeypatch.setattr(wcr, "validate_selector", lambda row: None)
    monkeypatch.setattr(wcr, "_read_credentials", lambda: ("u", "p"))
    monkeypatch.setattr(wcr, "_login_once", lambda host, user, pw: "fake-cookie")
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    seen = {}

    def fake_create_delete(row, host, screenshot_dir, cookie):
        seen["called"] = row.row_id
        return True, "ok"

    monkeypatch.setattr(wcr, "_run_kiln_config_create_delete", fake_create_delete)
    ok, msg = wcr.run_row_live("W42", "192.0.2.1", "/tmp/whatever")
    assert ok, msg
    assert seen["called"] == "W42"


def test_negative_mangled_selector_caught_by_dry_run_then_restored():
    # Required negative test: mangle a selector on a newly-wired row,
    # confirm --dry-run fails, restore by hand, confirm it passes again.
    # W22's #pcLink is temporarily swapped for an id that does not exist in
    # safety_config_page.html.
    good = wcr.ROWS["W22"]
    bad = dataclasses.replace(good, selector="save_typo_does_not_exist")
    wcr.ROWS["W22"] = bad
    try:
        ok, msg = wcr.dry_run("W22", repo_root=_repo_root())
        assert not ok
        assert "not found" in msg
    finally:
        wcr.ROWS["W22"] = good  # restore by hand, never git checkout --

    ok, msg = wcr.dry_run("W22", repo_root=_repo_root())
    assert ok, msg


def test_negative_mangled_fill_selector_caught_by_dry_run_then_restored():
    # Same shape, but mangling a `fills` selector (not the click selector)
    # -- proves _extra_ids/validate_selector actually inspects fills, not
    # just row.selector.
    good = wcr.ROWS["W38"]
    bad = dataclasses.replace(good, fills=(("#kcDpBrightnessTypo", "45"),))
    wcr.ROWS["W38"] = bad
    try:
        ok, msg = wcr.dry_run("W38", repo_root=_repo_root())
        assert not ok
        assert "not found" in msg
    finally:
        wcr.ROWS["W38"] = good

    ok, msg = wcr.dry_run("W38", repo_root=_repo_root())
    assert ok, msg


# ---------------------------------------------------------------------------
# 2026-09-21 review follow-up: collateral-write guard (`guard_fields`) and the
# CDP driver's fill read-back. Both close paths where the row could report a
# clean result while something the operator did not ask for was written, or
# while a click landed on a target nobody selected.
# ---------------------------------------------------------------------------

def test_w22_and_w38_declare_guard_fields_for_every_other_posted_field():
    # Both pages submit their whole form in one body, so every other field
    # that body carries must be guarded -- restore_from_field alone only
    # ever puts the EDITED field back.
    assert wcr.ROWS["W22"].guard_fields == (
        "thermo_count", "relay_count", "max_simultaneous_relays")
    assert wcr.ROWS["W38"].guard_fields == (
        "timeout_setting", "keep_on_while_firing", "display_on_error")


def test_fill_and_restore_fails_if_save_also_changed_a_guarded_field(monkeypatch):
    # settings_display_page.html's Save has no "loaded yet?" guard and posts
    # all four fields together, so a Save racing its own loadCurrent() writes
    # the markup defaults over the other three. That must FAIL loudly, not
    # pass because the one edited field round-tripped -- but the restoring
    # Save still runs and repairs the collateral write.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"brightness_percent": 100, "timeout_setting": 300,
                   "keep_on_while_firing": True, "display_on_error": True}),
            # Brightness took, but the three others got the markup defaults.
            (200, {"brightness_percent": 45, "timeout_setting": 0,
                   "keep_on_while_firing": False, "display_on_error": False}),
            # The restoring Save re-submits the original brightness alongside
            # markup-default-free values, repairing the collateral write too.
            (200, {"brightness_percent": 100, "timeout_setting": 300,
                   "keep_on_while_firing": True, "display_on_error": True}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    runs = []
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: runs.append(1) or _FakeProc())

    ok, msg = wcr._run_fill_and_restore(wcr.ROWS["W38"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "collateral write" in msg
    assert "timeout_setting" in msg
    assert len(runs) == 2, "restore Save must run after a collateral-write failure"
    assert "restored" in msg


def test_fill_and_restore_guard_check_precedes_the_did_not_change_check(monkeypatch):
    # The clobbering Save can also leave the EDITED field reading its
    # original value (the race posts the form's defaults for everything).
    # The older "write did not land" message would then be the only thing
    # reported, hiding the collateral write.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, _zones_body(30000)),
            (200, _zones_body(30000, thermo_count=0)),
            (200, _zones_body(30000)),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_fill_and_restore(wcr.ROWS["W22"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "collateral write" in msg
    assert "did not change" not in msg


def test_fill_and_restore_restores_even_when_the_set_driver_fails(monkeypatch):
    # The CDP driver can die AFTER the click's POST landed (screenshot or
    # teardown error, 60 s timeout on a page that already saved), so a
    # non-zero exit is not proof nothing was written -- the restoring Save
    # must run anyway, and its outcome must be reported (mirrors W50's
    # _run_setup_wizard_step1 fix, 14e2324b).
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, _zones_body(30000)),
            (200, _zones_body(30000)),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    calls = []

    def fake_run(*a, **k):
        calls.append(1)
        return _FailProc() if len(calls) == 1 else _FakeProc()

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)

    ok, msg = wcr._run_fill_and_restore(wcr.ROWS["W22"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "CDP driver (set) exited 3" in msg
    assert len(calls) == 2, "restore Save must run even when the set driver exits non-zero"
    assert "restored" in msg


def test_fill_and_restore_restores_when_the_set_driver_raises(monkeypatch):
    # Same, for a subprocess that raises (TimeoutExpired/OSError) rather
    # than returning a non-zero code -- a raise here is not proof nothing
    # was written, so it must not skip the restore.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, _zones_body(30000)),
            (200, _zones_body(30000)),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    calls = []

    def fake_run(*a, **k):
        calls.append(1)
        if len(calls) == 1:
            raise wcr.subprocess.TimeoutExpired(cmd="node", timeout=60)
        return _FakeProc()

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)

    ok, msg = wcr._run_fill_and_restore(wcr.ROWS["W22"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "raised TimeoutExpired" in msg
    assert len(calls) == 2, "restore Save must run even when the set driver raises"
    assert "restored" in msg


def test_fill_and_restore_fails_if_the_restore_save_drifts_a_guarded_field(monkeypatch):
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, _zones_body(30000)),
            (200, _zones_body(54000)),
            (200, _zones_body(30000, relay_count=0)),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_fill_and_restore(wcr.ROWS["W22"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "restore Save CHANGED" in msg
    assert "relay_count" in msg


def test_fill_and_restore_refuses_to_write_if_a_guarded_field_is_absent(monkeypatch):
    # No baseline for a guarded field means no way to prove the Save left it
    # alone -- refuse before the first CDP run, not after.
    ran = []
    monkeypatch.setattr(wcr, "_get_json_with_cookie",
                        lambda host, path, cookie: (200, {"pc_link_abort_silence_ms": 30000}))
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: ran.append(1) or _FakeProc())

    ok, msg = wcr._run_fill_and_restore(wcr.ROWS["W22"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "refusing to write" in msg
    assert ran == []


def test_cdp_driver_fill_reads_back_the_assigned_value():
    # The driver must compare .value after assigning it: a <select> whose
    # option list has not been populated yet silently keeps '' (W42's delete
    # step selects the slot it just created by id), and a range input snaps
    # an off-step value. Both would click with a value nobody asked for.
    src = os.path.join(_repo_root(), "tools", "PcTools", "scripts", "_web_commission_cdp.mjs")
    with open(src, "r", encoding="utf-8") as f:
        text = f.read()
    assert "VALUE_REJECTED" in text
    assert "      if (String(el.value) !== " in text
    assert "Refusing to click with an unintended value" in text
    # ...and the comparison must come BEFORE the events are dispatched, so a
    # rejected value never reaches the page's own listeners.
    assert text.index("if (String(el.value) !== ") < text.index("new Event('input'")


def test_cdp_driver_click_retries_before_giving_up():
    # W8's live run (2026-09-22, board at 7dcde0dd) hit a runner selector
    # defect: the create step passed and GET /api/profiles confirmed the new
    # scratch profile server-side, but the very next CDP invocation's delete
    # click (aria-label 'Delete "<name>"') fired only ~500ms after
    # navigation -- before profiles_page.html's own refreshAll() fetch had
    # repainted the list -- and failed NOT_FOUND. The fix is a bounded
    # poll-and-click (clickWithRetry()), used for BOTH the top-level
    # --selector-kind click and every `click` step, not a one-shot resolve.
    src = os.path.join(_repo_root(), "tools", "PcTools", "scripts", "_web_commission_cdp.mjs")
    with open(src, "r", encoding="utf-8") as f:
        text = f.read()
    assert "async function clickWithRetry(" in text
    assert "CLICK_WAIT_TIMEOUT_MS" in text
    # The `click` step action must delegate to the retrying helper, not
    # resolve-and-click exactly once.
    click_step = text[text.index("if (step.action === 'click') {"):]
    click_step = click_step[:click_step.index("if (step.action === 'fill')")]
    assert "clickWithRetry(cdp, step.kind, step.selector" in click_step
    assert "NOT_FOUND" not in click_step  # no more inline one-shot resolve here
    # The top-level single-shot --selector-kind click (id/css/aria-label and
    # text) must also delegate to it, not keep its own inline Runtime.evaluate.
    top_click = text[text.index("if (args.selectorKind === 'id'"):]
    top_click = top_click[:top_click.index("} else if (args.selectorKind === undefined")]
    assert "clickWithRetry(cdp, args.selectorKind, args.selector" in top_click
    assert "clickWithRetry(cdp, 'text', args.selector" in top_click
    assert "NOT_FOUND" not in top_click


def test_cdp_driver_click_timeout_zero_means_one_attempt_not_default():
    # Opus review advisory: `step.timeoutMs || CLICK_WAIT_TIMEOUT_MS` treats an
    # explicit `timeoutMs: 0` the same as "unset", silently promoting it to the
    # 5000ms default -- a caller that deliberately asks for a single immediate
    # attempt (0ms) instead waits the full default. `??` (nullish coalescing)
    # is required so only actually-missing (undefined/null) falls back; `0` is
    # a legitimate, distinct value that must survive.
    src = os.path.join(_repo_root(), "tools", "PcTools", "scripts", "_web_commission_cdp.mjs")
    with open(src, "r", encoding="utf-8") as f:
        text = f.read()
    assert "step.timeoutMs || CLICK_WAIT_TIMEOUT_MS" not in text
    assert "step.timeoutMs ?? CLICK_WAIT_TIMEOUT_MS" in text
    # A NOT_FOUND miss must fail loud with the timeout actually waited and the
    # selector that was never found, not a bare generic error -- otherwise a
    # miss is undiagnosable from the tool's own output.
    click_with_retry = text[text.index("async function clickWithRetry("):]
    click_with_retry = click_with_retry[:click_with_retry.index("\n}\n")]
    assert "${label}" in click_with_retry
    assert "${JSON.stringify(selector)}" in click_with_retry
    assert "${timeoutMs}ms" in click_with_retry
    # The retry loop must still make at least one attempt when timeoutMs is 0
    # (a do-at-least-once shape): the deadline check comes strictly after the
    # first Runtime.evaluate call in loop order, not before it.
    assert click_with_retry.index("cdp.send('Runtime.evaluate'") < click_with_retry.index("Date.now() >= deadline")
    # The same swallow-zero hazard applied to the two other step actions that
    # take a caller-supplied timeoutMs: wait-for-selector's own local default
    # and wait-for-post's default. Both must use `??`, not `||`, too.
    assert "step.timeoutMs || 5000" not in text
    assert "step.timeoutMs ?? 5000" in text
    assert "step.timeoutMs || POST_WAIT_TIMEOUT_MS" not in text
    assert "step.timeoutMs ?? POST_WAIT_TIMEOUT_MS" in text
    # wait-for-selector's own poll loop must also attempt once before its
    # deadline check, same shape as clickWithRetry above.
    wait_for_selector = text[text.index("if (step.action === 'wait-for-selector')"):]
    wait_for_selector = wait_for_selector[:wait_for_selector.index("if (step.action === 'wait-for-post')")]
    assert wait_for_selector.index("cdp.send('Runtime.evaluate'") < wait_for_selector.index("Date.now() >= deadline")
    # waitForPost() (CdpSession method backing wait-for-post) must have the
    # same shape: it scans for an already-completed match BEFORE its deadline
    # check, so timeoutMs: 0 still gets one real look rather than none.
    wait_for_post_method = text[text.index("async waitForPost(pathFragment, timeoutMs) {"):]
    wait_for_post_method = wait_for_post_method[:wait_for_post_method.index("async waitForQuiet(")]
    assert wait_for_post_method.index("this.completed.findIndex(") < wait_for_post_method.index("Date.now() >= deadline")


def test_cdp_driver_bounds_and_trims_the_emitted_network_list():
    # Opus advisory on 63b62945: cdp.completed is unbounded for the whole
    # driver run and used to be emitted whole as the `network` field, which
    # could bloat the final JSON without limit (and a `data:` URL bloats a
    # single record on its own). Cap the emitted copy and trim long URLs
    # without severing the path component that _cdp_post_statuses() matches
    # on (urlsplit(url).path) -- see NETWORK_RECORD_CAP/NETWORK_URL_MAX_LEN
    # and trimUrl() above.
    src = os.path.join(_repo_root(), "tools", "PcTools", "scripts", "_web_commission_cdp.mjs")
    with open(src, "r", encoding="utf-8") as f:
        text = f.read()
    assert "const NETWORK_RECORD_CAP = 500;" in text
    assert "const NETWORK_URL_MAX_LEN = 512;" in text
    assert "function trimUrl(url, maxLen)" in text
    # trimUrl must drop query/fragment before falling back to a blunt slice,
    # so the path component survives a trim whenever possible.
    trim_url = text[text.index("function trimUrl(url, maxLen)"):]
    trim_url = trim_url[:trim_url.index("\n}\n")]
    assert "u.search = ''" in trim_url
    assert "u.hash = ''" in trim_url
    # The cap/trim must apply ONLY to the copy built for the emitted JSON,
    # never to cdp.completed itself -- waitForPost()'s postCursor indexes
    # into cdp.completed BY POSITION, so trimming that live list would shift
    # every later --expect-post/wait-for-post onto the wrong record. This is
    # the "switch the cap to only apply to the emitted copy" option named in
    # the advisory, chosen over shifting the cursor as the simpler safe fix.
    emit_block = text[text.index("const networkDropped ="):]
    emit_block = emit_block[:emit_block.index("main().catch(")]
    assert "cdp.completed.slice(cdp.completed.length - NETWORK_RECORD_CAP)" in emit_block
    assert "network_truncated: true, network_dropped: networkDropped" in emit_block
    # waitForPost's own scan/consume logic must be untouched by this change.
    assert "this.postCursor = idx + 1;" in text


def test_w50_declares_special_setup_wizard_step1_shape():
    row = wcr.ROWS["W50"]
    assert row.special == "setup_wizard_step1"
    assert row.verify_endpoint == "/api/status"
    assert row.expect_post == "/api/unit_pref"
    assert row.route == "/setup#step=1"


def _w50_get_json(status_responses, progress_responses=None):
    """Builds the `_get_json_with_cookie` fake for a W50 test: `/api/status`
    gets `status_responses` in order, `/api/setup/progress` gets
    `progress_responses` in order (default: 'pending' pre-read, then 'done'
    confirmed after the restoring POST -- the common case where step 1
    hadn't been marked done before this row ran)."""
    if progress_responses is None:
        progress_responses = [(200, _progress_body("pending")), (200, _progress_body("pending"))]
    return _get_json_by_path({
        "/api/status": status_responses,
        "/api/setup/progress": progress_responses,
    })


def _w50_post_form(monkeypatch, status=200, body=None):
    """Mocks `_post_form_with_cookie` (used only for the setup-progress
    restore) and returns the list of calls made to it, so a test can assert
    the restore POST carried the right step/state."""
    calls = []

    def fake(host, path, cookie, fields, timeout=5.0):
        calls.append((path, fields))
        return status, body if body is not None else {"ok": True}

    monkeypatch.setattr(wcr, "_post_form_with_cookie", fake)
    return calls


def test_setup_wizard_step1_full_flow_passes(monkeypatch):
    # Pre-read shows the original tz/unit, post-set read-back shows the unit
    # flipped with tz unchanged, post-restore read-back shows the unit back
    # to original -- three distinct GETs against /api/status covering TWO
    # fields written by two different POST routes in one click, plus a
    # pre-read and a post-restore read of /api/setup/progress covering the
    # THIRD (postStepState) POST that same click makes.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _w50_get_json(
            [
                (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
                (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "F"}),
                (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            ],
            [(200, _progress_body("pending")), (200, _progress_body("pending"))],
        ),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    post_calls = _w50_post_form(monkeypatch)

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert ok, msg
    assert "restored" in msg
    assert post_calls == [("/api/setup/progress", {"step": "1", "state": "pending"})]


class _FakeProcWithNetwork:
    """Like `_FakeProc`, but its stdout carries a `network` list the way
    the real CDP driver's `--expect-post`-independent request/response
    bookkeeping does (`cdp.completed` in _web_commission_cdp.mjs), so a
    test can exercise `_cdp_post_statuses()`/`_non2xx_post_failures()`
    against W50's own two extra POSTs (tz, unit_pref) rather than just the
    one `--expect-post` waits for (setup/progress)."""

    def __init__(self, records):
        self.returncode = 0
        self.stdout = json.dumps({"ok": True, "network": records})
        self.stderr = ""


def test_setup_wizard_step1_fails_loud_on_a_non_2xx_tz_post(monkeypatch):
    # #step1Save's click fires THREE chained POSTs; the row's `expect_post`
    # only waits for and reports the LAST one (/api/setup/progress). A
    # non-2xx on /api/settings/tz (the FIRST) must fail this row loud even
    # though the GET /api/status read-back afterward would otherwise still
    # show temp_unit flipped (unit_pref can succeed independently of tz).
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _w50_get_json([
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "F"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    set_records = [
        {"method": "POST", "url": "http://192.0.2.1/api/settings/tz", "status": 500, "failed": False},
        {"method": "POST", "url": "http://192.0.2.1/api/unit_pref", "status": 200, "failed": False},
        {"method": "POST", "url": "http://192.0.2.1/api/setup/progress", "status": 200, "failed": False},
    ]
    calls = []

    def fake_run(*a, **k):
        calls.append(1)
        return _FakeProcWithNetwork(set_records) if len(calls) == 1 else _FakeProc()

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)
    _w50_post_form(monkeypatch)

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "did not answer 2xx" in msg
    assert "/api/settings/tz -> 500" in msg
    assert len(calls) == 2, "restore Save must still run after a bad tz POST status"
    assert "restored" in msg


def test_cdp_post_statuses_does_not_match_a_longer_route(monkeypatch):
    # `/api/unit_prefs` is NOT `/api/unit_pref`: a substring test would grade
    # the wrong request's status as this row's write. A query string on the
    # real route must still match.
    proc = _FakeProcWithNetwork([
        {"method": "POST", "url": "http://192.0.2.1/api/unit_prefs", "status": 500, "failed": False},
        {"method": "POST", "url": "http://192.0.2.1/api/settings/tz?cb=1", "status": 200, "failed": False},
    ])
    found = wcr._cdp_post_statuses(proc, ("/api/settings/tz", "/api/unit_pref"))
    assert "/api/unit_pref" not in found, found
    assert len(found["/api/settings/tz"]) == 1
    assert wcr._non2xx_post_failures(found) == []


def test_cdp_stdout_summary_drops_the_unbounded_network_list():
    # `network` sits last in the driver's JSON, so a raw stdout[-300:] would
    # be nothing but request records and would hide route/post from the PASS
    # message of every row.
    proc = _FakeProcWithNetwork([
        {"method": "POST", "url": "http://192.0.2.1/api/pad/%d" % i, "status": 200, "failed": False}
        for i in range(40)
    ])
    proc.stdout = json.dumps({"ok": True, "route": "/settings/zones", "post": {"status": 204},
                              "network": json.loads(proc.stdout)["network"]})
    summary = wcr._cdp_stdout_summary(proc)
    assert "/settings/zones" in summary, summary
    assert "/api/pad/" not in summary, summary
    assert "network_count" in summary


def test_cdp_stdout_summary_keeps_network_truncated_marker():
    # A capped/truncated run's extra top-level keys (network_truncated,
    # network_dropped -- see the JS driver's emission code) must survive
    # _cdp_stdout_summary()'s network-list drop like any other field; only
    # `network` itself is stripped.
    proc = _FakeProc()
    proc.stdout = json.dumps({
        "ok": True, "route": "/settings/zones", "post": {"status": 204},
        "network": [{"method": "POST", "url": "http://192.0.2.1/api/x", "status": 200, "failed": False}],
        "network_truncated": True, "network_dropped": 37,
    })
    summary = wcr._cdp_stdout_summary(proc)
    assert "network_truncated" in summary, summary
    assert "network_dropped" in summary, summary
    assert '"network":' not in summary, summary


def test_cdp_stdout_summary_falls_back_on_non_json_stdout():
    class _P:
        stdout = "not json at all"
    assert wcr._cdp_stdout_summary(_P()) == "not json at all"


def test_setup_wizard_step1_fails_if_unit_never_changes(monkeypatch):
    # The write silently didn't land -- must FAIL, not report PASS with a
    # restore of a value that was never actually different (same shape as
    # W22/W38's "did not change" negative case).
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _w50_get_json([
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            # ...and the restoring Save still runs and is still read back,
            # even though the flip itself never landed.
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    runs = []
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: runs.append(1) or _FakeProc())
    _w50_post_form(monkeypatch)

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "did not change" in msg
    assert len(runs) == 2, "restore Save must run even when the mid-state check fails"
    assert "restored" in msg


def test_setup_wizard_step1_fails_loud_if_time_tz_drifts(monkeypatch):
    # The Save also changed time_tz, which this row never asked to touch --
    # must fail loud and say LEFT ON BOARD, never silently accept a
    # collateral write to a real timezone rule.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _w50_get_json([
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            (200, {"time_tz": "UTC0", "temp_unit": "F"}),
            # The restoring Save re-submits the ORIGINAL tz alongside the
            # original unit, so it repairs the collateral write instead of
            # only reporting it.
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    runs = []
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: runs.append(1) or _FakeProc())
    _w50_post_form(monkeypatch)

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "collateral write" in msg
    assert len(runs) == 2, "restore Save must run after a collateral-write failure"
    assert "restored" in msg


class _FailProc:
    returncode = 3
    stdout = ""
    stderr = "boom"


def test_setup_wizard_step1_restores_even_when_the_set_driver_fails(monkeypatch):
    # The CDP driver can die AFTER the click's POSTs landed (screenshot or
    # teardown error, 60 s timeout on a page that already saved), so a
    # non-zero exit is not proof nothing was written -- the restoring Save
    # must run anyway, and its outcome must be reported.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _w50_get_json([
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    calls = []

    def fake_run(*a, **k):
        calls.append(1)
        return _FailProc() if len(calls) == 1 else _FakeProc()

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)
    _w50_post_form(monkeypatch)

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "CDP driver (set) exited 3" in msg
    assert len(calls) == 2, "restore Save must run even when the set driver exits non-zero"
    assert "restored" in msg


def test_setup_wizard_step1_restores_when_the_set_driver_raises(monkeypatch):
    # Same, for a subprocess that raises (TimeoutExpired/OSError) rather
    # than returning a non-zero code.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _w50_get_json([
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    calls = []

    def fake_run(*a, **k):
        calls.append(1)
        if len(calls) == 1:
            raise wcr.subprocess.TimeoutExpired(cmd="node", timeout=60)
        return _FakeProc()

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)
    _w50_post_form(monkeypatch)

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "raised TimeoutExpired" in msg
    assert len(calls) == 2, "restore Save must run even when the set driver raises"
    assert "restored" in msg


def test_setup_wizard_step1_reports_a_failed_restore_after_a_failed_set(monkeypatch):
    # Both halves broken: the failure message must carry BOTH the original
    # failure and the fact the board may still hold the test unit.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _w50_get_json([
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "F"}),
            (500, None),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    _w50_post_form(monkeypatch)

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "RESTORE UNCONFIRMED" in msg


def test_setup_wizard_step1_fails_loud_if_restore_does_not_take(monkeypatch):
    # Restore POST "succeeded" (CDP exit 0) but the read-back after it still
    # shows the test unit -- must fail loud rather than report PASS, since
    # this is exactly the "board left with the test value" hazard.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _w50_get_json([
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "F"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "F"}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    _w50_post_form(monkeypatch)

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "RESTORE FAILED" in msg
    assert "temp_unit still 'F'" in msg
    assert "LEFT ON BOARD" in msg


def test_setup_wizard_step1_refuses_when_status_body_unusable(monkeypatch):
    # No usable temp_unit/time_tz means no known-good value to restore to --
    # refuse before touching the board, same "refuse to guess" posture as
    # W22's guard-field check.
    ran = []
    monkeypatch.setattr(wcr, "_get_json_with_cookie",
                        lambda host, path, cookie: (200, {"time_tz": None, "temp_unit": "K"}))
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: ran.append(1) or _FakeProc())

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "refusing to guess" in msg
    assert ran == []


def test_setup_wizard_step1_refuses_when_progress_body_unusable(monkeypatch):
    # Same posture, for the progress route: no usable steps.1.state means no
    # known-good value to restore the commissioning-progress flag to, so
    # this must refuse before ever touching the board -- the status
    # pre-read succeeds, but the progress pre-read does not carry a step 1
    # entry.
    ran = []
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _w50_get_json(
            [(200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"})],
            [(200, {"version": 1, "steps": {}})],
        ),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: ran.append(1) or _FakeProc())

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "refusing to guess" in msg
    assert "setup/progress" in msg
    assert ran == []


def test_setup_wizard_step1_restores_progress_state_to_its_pre_run_value(monkeypatch):
    # If step 1 already read 'done' (an operator ran the wizard for real,
    # then this row ran again later), the restore must put it back to
    # 'done' -- not to 'pending' -- so this row never regresses a real
    # commissioning milestone.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _w50_get_json(
            [
                (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
                (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "F"}),
                (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            ],
            [(200, _progress_body("done", note="ran for real")), (200, _progress_body("done", note="ran for real"))],
        ),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    post_calls = _w50_post_form(monkeypatch)

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert ok, msg
    assert post_calls == [("/api/setup/progress", {"step": "1", "state": "done", "note": "ran for real"})]


def test_setup_wizard_step1_fails_loud_if_progress_restore_post_is_refused(monkeypatch):
    # The tz/unit restore succeeded, but the progress-restore POST itself
    # came back non-200 -- must fail loud and name the board's left-behind
    # state, not report PASS just because the field-level restore worked.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _w50_get_json([
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "F"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    _w50_post_form(monkeypatch, status=500, body={"error": "internal"})

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "PROGRESS RESTORE FAILED" in msg
    assert "restore by hand" in msg


def test_setup_wizard_step1_fails_loud_if_progress_restore_does_not_take(monkeypatch):
    # Progress-restore POST answered 200, but the read-back after it still
    # shows 'done' instead of the pre-run 'pending' -- must fail loud, the
    # same "board left with the test value" hazard as the tz/unit restore.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _w50_get_json(
            [
                (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
                (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "F"}),
                (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            ],
            [(200, _progress_body("pending")), (200, _progress_body("done"))],
        ),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    _w50_post_form(monkeypatch)

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "PROGRESS RESTORE FAILED" in msg
    assert "step 1 state now 'done'" in msg
    assert "LEFT ON BOARD" in msg


def test_setup_wizard_step1_both_cdp_calls_wait_for_the_progress_post(monkeypatch):
    # BOTH the flipping Save and the restoring Save run the page's same
    # three-POST chain (tz -> unit_pref -> setup/progress). If the restoring
    # call waited only for /api/unit_pref (the row's default expect_post,
    # the SECOND POST), the driver could exit while the page's own
    # postStepState(1,'done') was still in flight, and _restore_progress()
    # would then post + read back the pre-run state BEFORE that late 'done'
    # landed -- leaving step 1 at 'done' on a run that reported PASS.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _w50_get_json([
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "F"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    cmds = []

    def fake_run(cmd, **kwargs):
        cmds.append(cmd)
        return _FakeProc()

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)
    _w50_post_form(monkeypatch)

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert ok, msg
    assert len(cmds) == 2, cmds
    for cmd in cmds:
        assert "--expect-post" in cmd
        assert cmd[cmd.index("--expect-post") + 1] == "/api/setup/progress", cmd


def test_setup_wizard_step1_fails_loud_if_the_progress_note_is_not_restored(monkeypatch):
    # The click's postStepState(1,'done') carries no note, and firmware
    # CLEARS the stored note on a POST that omits it, so a pre-run note only
    # survives if this row's restore POST puts it back. A restore whose
    # state took but whose note did not must FAIL, never PASS with the
    # operator's text silently gone.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _w50_get_json(
            [
                (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
                (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "F"}),
                (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            ],
            [(200, _progress_body("done", note="skipped: no CT fitted")),
             (200, _progress_body("done", note=""))],
        ),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    post_calls = _w50_post_form(monkeypatch)

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "PROGRESS RESTORE FAILED" in msg
    assert "note" in msg
    assert "restore by hand" in msg
    # The note WAS sent -- the failure is that the board did not keep it.
    assert post_calls == [("/api/setup/progress",
                           {"step": "1", "state": "done", "note": "skipped: no CT fitted"})]


# ---------------------------------------------------------------------------
# W8/W9/W10 -- profile segment-builder create/delete, multi-select delete,
# favorite toggle.
# ---------------------------------------------------------------------------

def _profiles_body(*names_and_ids):
    """A GET /api/profiles-shaped body: a bare list of {id, name, builtin}."""
    return [{"id": i, "name": n, "builtin": False} for n, i in names_and_ids]


def test_w8_declares_create_delete_shape():
    row = wcr.ROWS["W8"]
    assert row.classification == "write"
    assert row.special == "profile_segment_create_delete"
    assert row.verify_endpoint == "/api/profiles"
    assert row.expect_post == "/api/profile"
    assert "pname" in row.special_selectors


def test_w9_declares_multi_delete_shape():
    row = wcr.ROWS["W9"]
    assert row.classification == "write"
    assert row.special == "profile_multi_delete"
    assert row.verify_endpoint == "/api/profiles"
    assert row.expect_post == "/api/profile/delete"
    assert "bulkActionBtn" in row.special_selectors


def test_w10_declares_favorite_toggle_shape():
    row = wcr.ROWS["W10"]
    assert row.classification == "write"
    assert row.special == "profile_favorite_toggle"
    assert row.verify_endpoint == "/api/profiles/favorites"
    assert row.expect_post == "/api/profile/favorite"


def test_new_scratch_profile_name_fits_firmware_limit():
    name = wcr._new_scratch_profile_name()
    assert name.startswith("wc_test_")
    assert len(name) <= wcr._PROFILE_NAME_MAX_LEN
    name2 = wcr._new_scratch_profile_name(7)
    assert name2 != name or name2.endswith("7")


def test_is_scratch_profile_name():
    assert wcr._is_scratch_profile_name("wc_test_1234560")
    assert not wcr._is_scratch_profile_name("kc_test_1234560")  # W42's own prefix, not this row's
    assert not wcr._is_scratch_profile_name("my real profile")
    assert not wcr._is_scratch_profile_name(None)
    assert not wcr._is_scratch_profile_name("")


def test_w8_full_flow_passes(monkeypatch):
    monkeypatch.setattr(wcr.time, "time", lambda: 1000000)
    name = wcr._new_scratch_profile_name()
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, _profiles_body(("existing", 1))),                   # pre-read
            (200, _profiles_body(("existing", 1), (name, 9))),        # post-create
            (200, _profiles_body(("existing", 1))),                   # post-delete
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_profile_segment_create_delete(wcr.ROWS["W8"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert ok, msg
    assert name in msg
    assert "deleted" in msg


def test_w8_refuses_when_leftover_scratch_profile_exists(monkeypatch):
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([(200, _profiles_body(("existing", 1), ("wc_test_999999", 5)))]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_profile_segment_create_delete(wcr.ROWS["W8"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "leftover scratch profile" in msg
    assert "wc_test_999999" in msg


def test_w8_reports_left_on_board_when_delete_cdp_fails(monkeypatch):
    # Negative test on the cleanup path: the create lands and is confirmed,
    # but the delete's own CDP invocation exits non-zero -- this must FAIL
    # loud and say LEFT ON BOARD, never silently report PASS or swallow the
    # failure.
    monkeypatch.setattr(wcr.time, "time", lambda: 2000000)
    name = wcr._new_scratch_profile_name()
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, _profiles_body(("existing", 1))),
            (200, _profiles_body(("existing", 1), (name, 9))),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)

    class _FailProc:
        returncode = 1
        stdout = ""
        stderr = "selector not found"

    calls = {"n": 0}

    def fake_run(cmd, **kw):
        calls["n"] += 1
        if calls["n"] == 1:
            return _FakeProc()  # the create succeeds
        return _FailProc()  # the delete fails

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)

    ok, msg = wcr._run_profile_segment_create_delete(wcr.ROWS["W8"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "LEFT ON BOARD" in msg
    assert name in msg


def test_w9_full_flow_passes(monkeypatch):
    monkeypatch.setattr(wcr.time, "time", lambda: 3000000)
    n0 = wcr._new_scratch_profile_name(0)
    n1 = wcr._new_scratch_profile_name(1)
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, _profiles_body(("existing", 1))),                                    # pre-read
            (200, _profiles_body(("existing", 1), (n0, 8), (n1, 9))),                   # post-create
            (200, _profiles_body(("existing", 1))),                                     # post-delete
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_profile_multi_delete(wcr.ROWS["W9"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert ok, msg
    assert n0 in msg and n1 in msg


def test_w9_refuses_when_leftover_scratch_profile_exists(monkeypatch):
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([(200, _profiles_body(("existing", 1), ("wc_test_111111", 5)))]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_profile_multi_delete(wcr.ROWS["W9"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "leftover scratch profile" in msg


def test_w9_still_present_after_bulk_delete_fails_loud(monkeypatch):
    # The bulk-delete POST(s) look like they succeeded (CDP exit 0), but the
    # read-back afterward still lists one of the two scratch profiles --
    # must FAIL and say LEFT ON BOARD, never PASS on a half-finished delete.
    monkeypatch.setattr(wcr.time, "time", lambda: 4000000)
    n0 = wcr._new_scratch_profile_name(0)
    n1 = wcr._new_scratch_profile_name(1)
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, _profiles_body(("existing", 1))),
            (200, _profiles_body(("existing", 1), (n0, 8), (n1, 9))),
            (200, _profiles_body(("existing", 1), (n1, 9))),  # n0 deleted, n1 still there
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_profile_multi_delete(wcr.ROWS["W9"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "LEFT ON BOARD" in msg
    assert n1 in msg


def test_w10_full_flow_passes(monkeypatch):
    monkeypatch.setattr(wcr.time, "time", lambda: 5000000)
    name = wcr._new_scratch_profile_name()
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, _profiles_body(("existing", 1))),                     # pre-read /api/profiles
            (200, _profiles_body(("existing", 1), (name, 9))),          # post-create /api/profiles
            (200, {"ids": [9]}),                                        # post-toggle-on favorites
            (200, {"ids": []}),                                         # post-toggle-off favorites
            (200, _profiles_body(("existing", 1))),                     # post-delete /api/profiles
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_profile_favorite_toggle(wcr.ROWS["W10"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert ok, msg
    assert name in msg


def test_w10_fails_if_toggle_off_does_not_take(monkeypatch):
    # The toggle-off click "succeeds" (CDP exit 0), but the favorites
    # read-back still shows the id -- must fail loud and still delete the
    # scratch profile rather than leaving a favorited leftover on the board.
    monkeypatch.setattr(wcr.time, "time", lambda: 6000000)
    name = wcr._new_scratch_profile_name()
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, _profiles_body(("existing", 1))),
            (200, _profiles_body(("existing", 1), (name, 9))),
            (200, {"ids": [9]}),
            (200, {"ids": [9]}),  # toggle-off did not take
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_profile_favorite_toggle(wcr.ROWS["W10"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "still in favorites" in msg
    assert "deleted" in msg  # the cleanup delete was still attempted and succeeded


def test_delete_profile_by_name_refuses_non_scratch_name():
    ok, msg = wcr._delete_profile_by_name(wcr.ROWS["W8"], "192.0.2.1", "/tmp/whatever",
                                           "fake-cookie", "a real user profile", "_x")
    assert not ok
    assert "refusing" in msg


def test_w9_cleans_up_the_second_profile_even_when_its_own_create_reports_failure(monkeypatch):
    # Negative test (reviewer-added): _create_scratch_profile() can report
    # failure (e.g. a driver exit-code/stderr mismatch after the POST
    # actually landed) even though the write reached the board. W8 and W10
    # both re-read GET after a reported create failure and delete the name
    # if it is actually present; W9's per-iteration failure branch used to
    # skip this check entirely for the profile that just "failed" -- it
    # only ever cleaned up names already in `created` from EARLIER
    # iterations, so a second profile that landed despite a reported
    # failure was never named and never deleted: silently LEFT ON BOARD
    # with no trace in the failure message.
    monkeypatch.setattr(wcr.time, "time", lambda: 7000000)
    n0 = wcr._new_scratch_profile_name(0)
    n1 = wcr._new_scratch_profile_name(1)
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, _profiles_body(("existing", 1))),                       # pre-read
            # post-failure read-back inside the loop: n1 actually landed
            # despite _create_scratch_profile() reporting failure for it.
            (200, _profiles_body(("existing", 1), (n0, 8), (n1, 9))),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)

    calls = {"n": 0}

    def fake_run(cmd, **kw):
        calls["n"] += 1
        if calls["n"] == 1:
            return _FakeProc()  # create of n0 succeeds
        if calls["n"] == 2:
            # create of n1: driver reports failure (e.g. bad exit code),
            # but the POST actually landed on the board (see the
            # post-failure GET above).
            class _FailProc:
                returncode = 1
                stdout = ""
                stderr = "driver reported failure after the POST landed"
            return _FailProc()
        return _FakeProc()  # any cleanup delete calls

    monkeypatch.setattr(wcr.subprocess, "run", fake_run)

    deleted_names = []
    real_delete = wcr._delete_profile_by_name

    def spy_delete(row, host, screenshot_dir, cookie, name, shot_suffix):
        deleted_names.append(name)
        return real_delete(row, host, screenshot_dir, cookie, name, shot_suffix)

    monkeypatch.setattr(wcr, "_delete_profile_by_name", spy_delete)

    ok, msg = wcr._run_profile_multi_delete(wcr.ROWS["W9"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    # n1 actually landed on the board despite the reported create failure --
    # it must be named and a cleanup delete attempted for it (or reported
    # LEFT ON BOARD), never silently dropped. n0 alone is not enough: that
    # was already covered by the pre-existing `created` list.
    assert n1 in deleted_names, (
        f"n1={n1!r} was never passed to a cleanup delete -- only {deleted_names} were; "
        f"message was: {msg}"
    )


# ---------------------------------------------------------------------------
# W18/W20/W33 -- guarded no-op click rows (sweep abort / autotune abort /
# danger-mode exit). Each refuses before touching the board if the status
# route already shows a real operation in progress, and otherwise clicks and
# confirms the state stays idle/inactive. Nothing to restore: the click is a
# no-op by construction on this path.
# ---------------------------------------------------------------------------


def test_w18_refuses_when_sweep_already_running(monkeypatch):
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([(200, {"state": "running"})]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)

    def _no_subprocess(*a, **k):
        raise AssertionError("must not click abort while a real sweep is running")

    monkeypatch.setattr(wcr.subprocess, "run", _no_subprocess)

    ok, msg = wcr._run_sweep_abort_guarded(wcr.ROWS["W18"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "refusing" in msg
    assert "current-sweep run" in msg


def test_w18_passes_when_idle_before_and_after(monkeypatch):
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"state": "idle"}),
            (200, {"state": "idle"}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_sweep_abort_guarded(wcr.ROWS["W18"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert ok, msg


def test_w18_fails_loud_if_running_after_click(monkeypatch):
    # Pre-read is idle (click proceeds), but the read-back afterward
    # unexpectedly shows running -- must fail loud, never PASS.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"state": "idle"}),
            (200, {"state": "running"}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_sweep_abort_guarded(wcr.ROWS["W18"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "check the board by hand" in msg


def test_w20_refuses_when_autotune_not_idle(monkeypatch):
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([(200, {"state": "settling"})]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)

    def _no_subprocess(*a, **k):
        raise AssertionError("must not click abort while autotune is not idle")

    monkeypatch.setattr(wcr.subprocess, "run", _no_subprocess)

    ok, msg = wcr._run_autotune_abort_guarded(wcr.ROWS["W20"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "refusing" in msg
    assert "autotune run" in msg


def test_w20_passes_when_idle_before_and_after(monkeypatch):
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"state": "idle"}),
            (200, {"state": "idle"}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_autotune_abort_guarded(wcr.ROWS["W20"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert ok, msg


def test_w20_passes_from_aborted_or_done_terminal_state(monkeypatch):
    # 'aborted'/'done' are leftover terminal states from a *previous* run,
    # not a run in progress -- autotune_engine_abort() is a verified no-op
    # in both (autotune_engine_guard.c only acts when state_is_running()),
    # so a board that has ever run autotune once (and can therefore never
    # read back 'idle' again) must still be able to pass this row.
    for terminal_state in ("aborted", "done"):
        monkeypatch.setattr(
            wcr, "_get_json_with_cookie",
            _sequential_get_json([
                (200, {"state": terminal_state}),
                (200, {"state": terminal_state}),
            ]),
        )
        monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
        monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

        ok, msg = wcr._run_autotune_abort_guarded(wcr.ROWS["W20"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
        assert ok, f"{terminal_state}: {msg}"


def test_w33_refuses_when_danger_mode_active(monkeypatch):
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([(200, {"active": True})]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)

    def _no_subprocess(*a, **k):
        raise AssertionError("must not click exit while another operator's Danger Mode session is active")

    monkeypatch.setattr(wcr.subprocess, "run", _no_subprocess)

    ok, msg = wcr._run_danger_exit_guarded(wcr.ROWS["W33"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "refusing" in msg
    assert "Danger Mode" in msg


def test_w33_passes_when_inactive_before_and_after(monkeypatch):
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"active": False}),
            (200, {"active": False}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_danger_exit_guarded(wcr.ROWS["W33"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert ok, msg


def test_run_row_live_dispatches_to_guarded_click_for_w18_w20_w33(monkeypatch):
    monkeypatch.setattr(wcr, "validate_selector", lambda *a, **k: None)
    monkeypatch.setattr(wcr, "_read_credentials", lambda: ("user", "pass"))
    monkeypatch.setattr(wcr, "_login_once", lambda *a, **k: "fake-cookie")
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    for row_id, body in (("W18", {"state": "idle"}), ("W20", {"state": "idle"}), ("W33", {"active": False})):
        monkeypatch.setattr(wcr, "_get_json_with_cookie", _sequential_get_json([(200, body), (200, body)]))
        ok, msg = wcr.run_row_live(row_id, "192.0.2.1", "/tmp/whatever")
        assert ok, f"{row_id}: {msg}"
