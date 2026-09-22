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
    # restore of a value that was never actually different.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, _zones_body(30000)),
            (200, _zones_body(30000)),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_fill_and_restore(wcr.ROWS["W22"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "did not change" in msg


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
    assert "restore did not take" in msg


def test_kiln_config_create_delete_full_flow_passes(monkeypatch):
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"configs": [{"id": "1", "name": "existing"}]}),
            (200, {"configs": [{"id": "1", "name": "existing"},
                                {"id": "7", "name": "kc_test_123"}]}),
            (200, {"configs": [{"id": "1", "name": "existing"}]}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    monkeypatch.setattr(wcr.time, "time", lambda: 123)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert ok, msg
    assert "deleted" in msg


def test_kiln_config_create_delete_fails_if_left_on_board(monkeypatch):
    # The delete step's own read-back still lists the throwaway config --
    # must fail loud and say so, never silently report success.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"configs": []}),
            (200, {"configs": [{"id": "7", "name": "kc_test_123"}]}),
            (200, {"configs": [{"id": "7", "name": "kc_test_123"}]}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())
    monkeypatch.setattr(wcr.time, "time", lambda: 123)

    ok, msg = wcr._run_kiln_config_create_delete(wcr.ROWS["W42"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "LEFT ON BOARD" in msg


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
    # pass because the one edited field round-tripped.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"brightness_percent": 100, "timeout_setting": 300,
                   "keep_on_while_firing": True, "display_on_error": True}),
            # Brightness took, but the three others got the markup defaults.
            (200, {"brightness_percent": 45, "timeout_setting": 0,
                   "keep_on_while_firing": False, "display_on_error": False}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_fill_and_restore(wcr.ROWS["W38"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "collateral write" in msg
    assert "timeout_setting" in msg


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
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_fill_and_restore(wcr.ROWS["W22"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "collateral write" in msg
    assert "did not change" not in msg


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


def test_w50_declares_special_setup_wizard_step1_shape():
    row = wcr.ROWS["W50"]
    assert row.special == "setup_wizard_step1"
    assert row.verify_endpoint == "/api/status"
    assert row.expect_post == "/api/unit_pref"
    assert row.route == "/setup#step=1"


def test_setup_wizard_step1_full_flow_passes(monkeypatch):
    # Pre-read shows the original tz/unit, post-set read-back shows the unit
    # flipped with tz unchanged, post-restore read-back shows the unit back
    # to original -- three distinct GETs against ONE endpoint (/api/status)
    # covering TWO fields written by two different POST routes in one click.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "F"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert ok, msg
    assert "restored" in msg


def test_setup_wizard_step1_fails_if_unit_never_changes(monkeypatch):
    # The write silently didn't land -- must FAIL, not report PASS with a
    # restore of a value that was never actually different (same shape as
    # W22/W38's "did not change" negative case).
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
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
        _sequential_get_json([
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
        _sequential_get_json([
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
        _sequential_get_json([
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
        _sequential_get_json([
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "F"}),
            (500, None),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

    ok, msg = wcr._run_setup_wizard_step1(wcr.ROWS["W50"], "192.0.2.1", "/tmp/whatever", "fake-cookie")
    assert not ok
    assert "RESTORE UNCONFIRMED" in msg


def test_setup_wizard_step1_fails_loud_if_restore_does_not_take(monkeypatch):
    # Restore POST "succeeded" (CDP exit 0) but the read-back after it still
    # shows the test unit -- must fail loud rather than report PASS, since
    # this is exactly the "board left with the test value" hazard.
    monkeypatch.setattr(
        wcr, "_get_json_with_cookie",
        _sequential_get_json([
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "C"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "F"}),
            (200, {"time_tz": "EST5EDT,M3.2.0,M11.1.0", "temp_unit": "F"}),
        ]),
    )
    monkeypatch.setattr(wcr.os, "makedirs", lambda *a, **k: None)
    monkeypatch.setattr(wcr.subprocess, "run", lambda *a, **k: _FakeProc())

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
