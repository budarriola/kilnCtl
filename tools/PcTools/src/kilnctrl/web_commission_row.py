"""Driver for one row of docs/COMMISSIONING_WEB_RUNBOOK.md.

Given a row id (e.g. ``W28``), this module either:

- ``--dry-run``: validates the row's selector against the page's *source*
  file on disk (no network, no board, no browser). This is the mode this
  module's own tests exercise, and the only mode safe to run while another
  session holds the board.
- live mode (default): logs into the board's real login form exactly once
  per invocation (credentials from ``KILNCTL_WEB_USERNAME``/
  ``KILNCTL_WEB_PASSWORD`` env vars, form-encoded POST, never printed),
  drives headless Chrome over the Chrome DevTools Protocol (the same
  approach as ``firmware/KilnFW/App/test/ui_responsive_sweep.mjs``, but
  against the *live* board instead of a throwaway static server) to load
  the row's page and click its control, screenshots the result to a scratch
  directory, then calls the row's backend read-back and prints
  PASS/FAIL.

No existing harness clicks a web control against the live board --
``docs/COMMISSIONING_WEBUI_RUNBOOK.md``'s "Harness verdict" section audited
this and found ``ui_responsive_sweep.mjs`` renders only a throwaway static
serve, never the board, and has no click simulation at all. This module is
the new tooling that closes that gap, scoped to one named row per
invocation so a bench agent can run exactly what's authorized and nothing
else.
"""
from __future__ import annotations

import dataclasses
import http.cookiejar
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Callable, Optional


def _repo_root() -> str:
    """tools/PcTools/src/kilnctrl/web_commission_row.py -> repo root is four
    levels up. This file sits directly in kilnctrl/, one level shallower
    than bench_test/cases_web.py's own _repo_root() (which is five levels
    up from inside kilnctrl/bench_test/) -- do not copy that constant
    without adjusting for the extra directory."""
    return os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))


@dataclasses.dataclass(frozen=True)
class Row:
    row_id: str
    route: str
    source_file: str  # repo-relative path to the page's .html source
    selector: str  # DOM id (without '#') or literal button text, per selector_kind
    selector_kind: str  # "id" or "text"
    action_desc: str
    expected_outcome: str
    classification: str  # "read-only" | "write" | "owner-gated"
    readback_desc: str
    verify_endpoint: "Optional[str]" = None  # GET path polled after the UI action in live mode; None = no automated read-back (e.g. client-side-only rows)
    # Path fragment of the POST the row's control is expected to issue. When
    # set, the CDP driver waits (bounded) for that request to actually
    # complete before screenshotting and killing Chrome -- without it, an
    # async fetch() started by the click handler can be torn down before it
    # is written, so the write silently never lands (the W30 defect found on
    # the 2026-09-21 live run). None = no POST expected (page loads,
    # client-side-only controls).
    expect_post: "Optional[str]" = None


# 23 of docs/COMMISSIONING_WEB_RUNBOOK.md's 51 rows are wired here so far
# (W1-W7, W15, W16, W21, W23, W25, W28-W31, W37, W39, W41, W43, W46, W48,
# W49) -- enough to cover both selector kinds ("id" and "page") and both
# the read-only and write classes. The remaining rows are documented in the
# runbook but do not yet have a Row() entry; adding one for each is
# straightforward follow-up work, not a gap in this module's own logic.
# Kept in sync by hand; a mismatch here is a doc bug, not a code bug, and
# should be fixed in both places together.
#
# 2026-09-21 addition: the ten read-only page-load rows below (W7, W21,
# W23, W25, W37, W39, W41, W43, W46, W49) were previously run "by hand"
# through this same CDP path with no Row() entry -- see
# docs/COMMISSIONING_TEST_MATRIX.md's "driven by hand ... no Row() entry
# yet" notes on the corresponding backend rows. Giving them a Row() entry
# makes them runnable the same mechanical way as every other wired row
# instead of ad hoc. W31 (rampAssistToggleBtn) is added alongside them as
# the one new write row: a single click on a stable id, no text entry (the
# CDP driver only clicks and screenshots -- it cannot fill a form field),
# an existing GET/POST pair (/api/ramp_assist) for read-back, and the same
# restore-before-leaving shape already used by W30's watchdog-panic toggle.
# Rows that would need in-page text entry to exercise safely (W8/W9/W10,
# a real edit for W16-style rows, W38, W42, W50, ...) are left unwired --
# see this task's hand-back note for the full list and why.
ROWS: "dict[str, Row]" = {
    "W1": Row("W1", "/login", "firmware/KilnFW/App/drivers/http/login_page.html",
              "login-form", "id", "submit the login form",
              "redirect to / (or the originally requested page)",
              "write", "GET /api/auth/session shows an authenticated session",
              verify_endpoint="/api/auth/session"),
    "W2": Row("W2", "/", "firmware/KilnFW/App/drivers/http/main_page.html",
              "", "page", "load the dashboard",
              "status tiles, history chart, zone rows render",
              "read-only", "GET /api/status matches rendered temps/state",
              verify_endpoint="/api/status"),
    "W3": Row("W3", "/", "firmware/KilnFW/App/drivers/http/main_page.html",
              "ackLastRunBtn", "id", "click Dismiss",
              "last-run banner disappears",
              "write", "GET /api/profile_exec shows no last-run banner condition",
              verify_endpoint="/api/profile_exec", expect_post="/api/profile_exec/ack_last_run"),
    "W4": Row("W4", "/", "firmware/KilnFW/App/drivers/http/main_page.html",
              "clearTripBtn", "id", "click Clear Trip",
              "trip banner clears",
              "write", "GET /api/status trip_reason/trip_mask return to none",
              verify_endpoint="/api/status", expect_post="/api/safety/clear_trip"),
    "W5": Row("W5", "/", "firmware/KilnFW/App/drivers/http/main_page.html",
              "pidPopupApplyBtn", "id", "click Apply in the PID popup",
              "popup closes, zone PID row updates",
              "write", "GET /api/zones reflects the applied gains",
              verify_endpoint="/api/zones", expect_post="/api/zones/pid"),
    "W6": Row("W6", "/", "firmware/KilnFW/App/drivers/http/main_page.html",
              "themeBtn", "id", "click the theme toggle",
              "page recolors light/dark",
              "read-only", "none -- client-side only, no route to read back"),
    "W15": Row("W15", "/settings/zones", "firmware/KilnFW/App/drivers/http/zones_page.html",
               "", "page", "load the zones page",
               "zone table + PID fields render",
               "read-only", "control_get_zones matches",
               verify_endpoint="/api/zones"),
    "W16": Row("W16", "/settings/zones", "firmware/KilnFW/App/drivers/http/zones_page.html",
               "saveBtn", "id", "edit a zone name, click Save",
               "save confirms, no error banner",
               "write", "control_get_zones shows the new name (GET-merge-POST body)",
               verify_endpoint="/api/zones", expect_post="/api/zones"),
    "W7": Row("W7", "/profiles", "firmware/KilnFW/App/drivers/http/profiles_page.html",
              "", "page", "load the profiles page",
              "profile list renders (builtin + saved + favorites)",
              "read-only", "profiles_list matches rendered rows",
              verify_endpoint="/api/profiles"),
    "W21": Row("W21", "/settings/safety", "firmware/KilnFW/App/drivers/http/safety_config_page.html",
               "", "page", "load the settings/safety page",
               # The runbook row says "rate-guard fields render"; the page is
               # actually titled "Safety timings" and renders guard TIMING
               # fields from GET /api/zones -- the rate/limit fields live on
               # /settings/zones (safety_config_page.html says so in its own
               # intro paragraph). Described here as what the page really
               # shows so an operator does not grade this row against fields
               # that are on a different page. verify_endpoint is kept as the
               # matrix's paired read-back for this row.
               "safety timing profile/zone timing fields render",
               "read-only", "safety_get_rate_guard matches",
               verify_endpoint="/api/safety/rate_guard/auto"),
    "W23": Row("W23", "/safety", "firmware/KilnFW/App/drivers/http/safety_page.html",
               "", "page", "load the safety page",
               "safety status/banner renders",
               "read-only", "safety_get_status matches",
               verify_endpoint="/api/status"),
    "W25": Row("W25", "/safety/commissioning", "firmware/KilnFW/App/drivers/http/safety_commissioning_page.html",
               "", "page", "load the safety/commissioning page",
               "wizard renders current commissioning values",
               "read-only", "safety_get_commissioning matches",
               verify_endpoint="/api/safety/commissioning"),
    "W28": Row("W28", "/diagnostics", "firmware/KilnFW/App/drivers/http/diagnostics_page.html",
               "", "page", "load the diagnostics page",
               "thermo fault table + diagnostics tiles render",
               "read-only", "thermo_read_faults matches",
               verify_endpoint="/api/thermo/faults"),
    "W29": Row("W29", "/diagnostics", "firmware/KilnFW/App/drivers/http/diagnostics_page.html",
               "crashAckBtn", "id", "click Acknowledge",
               "crash banner disappears/greys out",
               "write", "crash_report_ack read-back shows acknowledged: true",
               verify_endpoint="/api/crash_report", expect_post="/api/crash_report/ack"),
    "W30": Row("W30", "/diagnostics", "firmware/KilnFW/App/drivers/http/diagnostics_page.html",
               "wdPanicToggleBtn", "id", "click the watchdog PANIC toggle",
               "toggle state flips",
               "write", "get_watchdog_panic_disabled shows the new value; restore to enabled",
               verify_endpoint="/api/watchdog_cfg", expect_post="/api/watchdog_cfg"),
    "W31": Row("W31", "/diagnostics", "firmware/KilnFW/App/drivers/http/diagnostics_page.html",
               "rampAssistToggleBtn", "id", "click the ramp-assist toggle",
               "toggle state flips",
               "write", "GET /api/ramp_assist shows the new value; restore before leaving",
               verify_endpoint="/api/ramp_assist", expect_post="/api/ramp_assist"),
    "W37": Row("W37", "/settings/display", "firmware/KilnFW/App/drivers/http/settings_display_page.html",
               "", "page", "load the settings/display page",
               "display settings render",
               "read-only", "GET /api/settings/display_power matches",
               verify_endpoint="/api/settings/display_power"),
    "W39": Row("W39", "/settings/security", "firmware/KilnFW/App/drivers/net/security_page.html",
               "", "page", "load the settings/security page",
               "auth config/policy fields render",
               "read-only", "GET /api/auth/config matches",
               verify_endpoint="/api/auth/config"),
    "W41": Row("W41", "/settings/kiln_configs", "firmware/KilnFW/App/drivers/http/kiln_configs_page.html",
               "", "page", "load the settings/kiln_configs page",
               "config-preset list renders",
               "read-only", "GET /api/kiln_configs matches",
               verify_endpoint="/api/kiln_configs"),
    "W43": Row("W43", "/settings/backup", "firmware/KilnFW/App/drivers/http/backup_page.html",
               "", "page", "load the settings/backup page",
               "backup/restore controls render",
               "read-only", "GET /api/backup/export responds (headers only, no body diff needed)",
               verify_endpoint="/api/backup/export"),
    "W46": Row("W46", "/ota", "firmware/KilnFW/App/drivers/net/ota_page.html",
               "", "page", "load the ota page",
               "OTA/interlock status renders",
               "read-only", "ota_status matches",
               verify_endpoint="/api/ota/interlock"),
    "W48": Row("W48", "/readiness", "firmware/KilnFW/App/drivers/http/readiness_page.html",
               "", "page", "load the readiness page",
               "commissioning checklist renders",
               "read-only", "get_readiness matches",
               verify_endpoint="/api/readiness"),
    "W49": Row("W49", "/setup", "firmware/KilnFW/App/drivers/http/setup_wizard_page.html",
               "", "page", "load the setup wizard page",
               # The runbook row is "Start/Next/Back navigation"; this Row()
               # covers the page load only (the CDP driver clicks one named
               # id, not a multi-step sequence). The runbook's "no route hit"
               # refers to the STEP navigation being client-side -- the page
               # load itself does fetch /api/setup/progress and friends --
               # so there is still no single endpoint that reads back the
               # step change, hence no verify_endpoint.
               "wizard start step renders; step navigation is client-side",
               "read-only", "none -- step navigation is client-side, no route to read back"),
}


class SelectorNotFoundError(RuntimeError):
    pass


def validate_selector(row: Row, repo_root: Optional[str] = None) -> str:
    """Reads the row's source file off disk and confirms its selector is
    present. Raises SelectorNotFoundError if the file is missing or the
    selector cannot be found. Returns a short human-readable confirmation
    string on success. Pure file I/O -- no network."""
    root = repo_root or _repo_root()
    path = os.path.join(root, row.source_file.replace("/", os.sep))
    if not os.path.isfile(path):
        raise SelectorNotFoundError(f"source file not found: {row.source_file}")
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    if row.selector_kind == "page":
        return f"page source present ({row.source_file})"
    if row.selector_kind == "id":
        pattern = re.compile(r"""id=["']""" + re.escape(row.selector) + r"""["']""")
        if not pattern.search(text) and f"getElementById('{row.selector}')" not in text \
                and f'getElementById("{row.selector}")' not in text:
            raise SelectorNotFoundError(
                f"id \"{row.selector}\" not found in {row.source_file}")
        return f'id="{row.selector}" confirmed in {row.source_file}'
    if row.selector_kind == "text":
        if row.selector not in text:
            raise SelectorNotFoundError(
                f'button text "{row.selector}" not found in {row.source_file}')
        return f'button text "{row.selector}" confirmed in {row.source_file}'
    raise SelectorNotFoundError(f"unknown selector_kind {row.selector_kind!r}")


def dry_run(row_id: str, repo_root: Optional[str] = None) -> "tuple[bool, str]":
    """Validates a row's page + selector exist in source. No network, no
    board, no browser -- safe to run at any time, including while another
    session holds the board."""
    row = ROWS.get(row_id)
    if row is None:
        return False, f"unknown row id {row_id!r} (known: {', '.join(sorted(ROWS))})"
    try:
        detail = validate_selector(row, repo_root=repo_root)
    except SelectorNotFoundError as exc:
        return False, str(exc)
    return True, (
        f"{row_id} DRY-RUN PASS: {row.action_desc} on {row.route} -- {detail}; "
        f"expected outcome: {row.expected_outcome}; class: {row.classification}"
    )


# ---------------------------------------------------------------------------
# Live mode -- not exercised by this task (board held by another session).
# Kept deliberately close to bench_test/cases_web_rw.py's login pattern so
# it shares the one already reviewed for this repo, rather than inventing a
# second auth client.
# ---------------------------------------------------------------------------

def _login_once(host: str, username: str, password: str, timeout: float = 10.0) -> str:
    """One POST /api/auth/login, form-encoded, Accept-Encoding: identity
    (the API path, unlike the static page shells, is fine with identity
    encoding -- see docs/COMMISSIONING_WEBUI_RUNBOOK.md's Accept-Encoding
    note). Returns the session cookie value. Never logs the password."""
    url = f"http://{host}/api/auth/login"
    body = urllib.parse.urlencode({"username": username, "password": password}).encode("utf-8")
    req = urllib.request.Request(
        url, data=body, method="POST",
        headers={
            "Content-Type": "application/x-www-form-urlencoded",
            "Accept-Encoding": "identity",
        },
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        set_cookie = resp.headers.get("Set-Cookie", "")
    m = re.search(r"kiln_sid=([^;]+)", set_cookie)
    if not m:
        raise RuntimeError("login succeeded but no kiln_sid cookie in response")
    return m.group(1)


def _read_credentials() -> "tuple[str, str]":
    user = os.environ.get("KILNCTL_WEB_USERNAME")
    pw = os.environ.get("KILNCTL_WEB_PASSWORD")
    if not user or not pw:
        raise RuntimeError(
            "KILNCTL_WEB_USERNAME/KILNCTL_WEB_PASSWORD not set (bool presence: "
            f"user={bool(user)} pw={bool(pw)})")
    return user, pw


def _get_json_with_cookie(host: str, path: str, cookie: str, timeout: float = 5.0) -> "tuple[Optional[int], Optional[dict]]":
    """Bare authenticated GET, used only for the post-action read-back
    below. Returns (status, parsed-json-or-None)."""
    url = f"http://{host}{path}"
    req = urllib.request.Request(url, headers={"Cookie": f"kiln_sid={cookie}", "Accept-Encoding": "identity"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            status = resp.getcode()
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        try:
            text = exc.read().decode("utf-8", errors="replace")
        except Exception:  # noqa: BLE001
            text = None
        status = exc.code
    except (urllib.error.URLError, OSError) as exc:
        return None, {"error": str(exc)}
    try:
        return status, json.loads(text) if text else None
    except (ValueError, TypeError):
        return status, None


def run_row_live(row_id: str, host: str, screenshot_dir: str,
                  find_chrome: Optional[Callable[[], str]] = None,
                  cookie: Optional[str] = None) -> "tuple[bool, str]":
    """Live mode: drive headless Chrome over CDP against the real board,
    click the row's control, screenshot, then GET the row's
    ``verify_endpoint`` (when it has one) so the caller can compare the
    read-back against the expected outcome. This function contacts the
    board and must never be called by this task's own tests -- see
    cases_web_rw.py's ctx-injection convention for how a caller can fake
    the transport in a unit test instead of hitting a real board.

    ``cookie``, when supplied, is reused as-is and no login is performed --
    this lets a caller running an entire class of rows in one process log in
    exactly ONCE (per docs/agent_rules/BENCH.md's "one login attempt" rule)
    and pass the resulting session cookie into every row instead of this
    function re-logging in per row, which is what the original
    implementation always did (a defect found running the first live class
    sweep: N rows meant N logins, risking the login lockout ladder in
    project_owner_decisions_2026_09_21_login for no reason -- one CDP
    session per row already needs its own cookie value, but obtaining that
    value doesn't require a fresh HTTP login each time). When ``cookie`` is
    omitted, the pre-existing single-row behavior (log in, then run) is
    unchanged so a lone caller of this function keeps working exactly as
    before.
    """
    row = ROWS.get(row_id)
    if row is None:
        return False, f"unknown row id {row_id!r}"
    validate_selector(row)  # fail fast on a stale selector before touching the board
    if cookie is None:
        user, password = _read_credentials()
        cookie = _login_once(host, user, password)
    os.makedirs(screenshot_dir, exist_ok=True)
    # The actual CDP navigate/click/screenshot sequence reuses the Node CDP
    # session helper already reviewed for this repo
    # (firmware/KilnFW/App/test/ui_responsive_sweep.mjs's CdpSession class)
    # via a small companion script, rather than re-implementing a WebSocket
    # CDP client a second time in Python. The session cookie is passed
    # through the child's environment, never on argv, so it cannot leak via
    # a process listing or a logged command line.
    script = os.path.join(_repo_root(), "tools", "PcTools", "scripts", "_web_commission_cdp.mjs")
    cmd = [
        "node", script,
        "--host", host,
        "--route", row.route,
        "--selector-kind", row.selector_kind,
        "--selector", row.selector,
        "--screenshot", os.path.join(screenshot_dir, f"{row_id}.png"),
    ]
    # Dialog policy is per row, not global: only a row this module already
    # classifies as a write/owner-gated action may ANSWER a native confirm()
    # with OK. A read-only row that unexpectedly raises one gets it
    # dismissed -- that unhangs the renderer (the original defect) without
    # authorizing whatever the dialog guards, which on these pages includes
    # Danger Mode and the per-relay lifetime-cycle reset. The driver logs
    # every dialog with its message either way.
    if row.classification != "read-only":
        cmd.append("--accept-dialogs")
    # And when the row declares the POST its control should issue, the
    # driver waits for that request to complete instead of sleeping a fixed
    # 500 ms and killing Chrome underneath it (the W30 defect).
    if row.expect_post:
        cmd += ["--expect-post", row.expect_post]
    # Only KC_SID plus a minimal environment reaches the Chrome child --
    # Chrome does not need this process's unrelated secrets (e.g. any other
    # credential env vars this session happens to hold), and a minimal env
    # keeps the child's inherited surface reviewable.
    _passthrough_keys = ("PATH", "SYSTEMROOT", "SYSTEMDRIVE", "TEMP", "TMP",
                         "COMSPEC", "WINDIR", "PROGRAMFILES", "PROGRAMFILES(X86)",
                         "LOCALAPPDATA", "APPDATA", "USERPROFILE", "NUMBER_OF_PROCESSORS")
    child_env = {k: os.environ[k] for k in _passthrough_keys if k in os.environ}
    child_env["KC_SID"] = cookie
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60, env=child_env)
    if proc.returncode != 0:
        return False, f"{row_id} FAIL: CDP driver exited {proc.returncode}: {proc.stderr.strip()[-500:]}"

    if row.verify_endpoint:
        status, body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
        readback = f"GET {row.verify_endpoint} -> {status}: {json.dumps(body)[:300]}"
        # A non-200 (including None, meaning a transport-level failure such
        # as a connection error) means the read-back itself could not
        # confirm the action -- this must fail the row, not report PASS
        # with an unreadable read-back baked into the message.
        if status != 200:
            return False, f"{row_id} FAIL: read-back GET {row.verify_endpoint} -> {status}: {json.dumps(body)[:300]}"
    else:
        readback = "no verify_endpoint for this row -- read-back is client-side only, not automated"

    return True, (
        f"{row_id} PASS: {proc.stdout.strip()[-300:]} | expected: {row.expected_outcome} | "
        f"read-back ({row.readback_desc}): {readback}"
    )


def main(argv=None) -> int:
    import argparse

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("row_id", help="row id from docs/COMMISSIONING_WEB_RUNBOOK.md, e.g. W28")
    ap.add_argument("--dry-run", action="store_true",
                     help="validate page+selector against source only; no network")
    ap.add_argument("--host", default=os.environ.get("KILNCTL_BOARD_HOST", ""),
                     help="board host:port, live mode only")
    ap.add_argument("--screenshot-dir", default=os.path.join(_repo_root(), "logs", "web_commission"))
    # Deliberately NO --cookie flag: a session cookie must never appear in
    # argv (process listings, shell history, and this repo's own logged
    # command lines all capture it -- the same reason
    # _web_commission_cdp.mjs takes KC_SID from the environment). A caller
    # reusing one login across a class sets KC_REUSE_SID in the child's
    # environment instead, or calls run_row_live(cookie=...) in-process.
    args = ap.parse_args(argv)
    reuse_cookie = os.environ.get("KC_REUSE_SID") or None

    if args.dry_run:
        ok, msg = dry_run(args.row_id)
    else:
        if not args.host:
            print("live mode requires --host (or KILNCTL_BOARD_HOST)", file=sys.stderr)
            return 2
        ok, msg = run_row_live(args.row_id, args.host, args.screenshot_dir, cookie=reuse_cookie)

    print(("PASS: " if ok else "FAIL: ") + msg)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
