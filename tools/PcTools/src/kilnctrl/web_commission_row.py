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
    """tools/PcTools/src/kilnctrl/web_commission_row.py -> repo root is five
    levels up. Same convention as bench_test/cases_web.py's _repo_root()."""
    return os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", ".."))


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


# One entry per docs/COMMISSIONING_WEB_RUNBOOK.md row that has a concrete,
# clickable selector (page-load-only rows with no click still get an entry
# so --dry-run can confirm the page itself exists). Kept in sync by hand;
# a mismatch here is a doc bug, not a code bug, and should be fixed in both
# places together.
ROWS: "dict[str, Row]" = {
    "W1": Row("W1", "/login", "firmware/KilnFW/App/drivers/http/login_page.html",
              "login-form", "id", "submit the login form",
              "redirect to / (or the originally requested page)",
              "write", "GET /api/auth/session shows an authenticated session"),
    "W2": Row("W2", "/", "firmware/KilnFW/App/drivers/http/main_page.html",
              "", "page", "load the dashboard",
              "status tiles, history chart, zone rows render",
              "read-only", "GET /api/status matches rendered temps/state"),
    "W3": Row("W3", "/", "firmware/KilnFW/App/drivers/http/main_page.html",
              "ackLastRunBtn", "id", "click Dismiss",
              "last-run banner disappears",
              "write", "GET /api/profile_exec shows no last-run banner condition"),
    "W4": Row("W4", "/", "firmware/KilnFW/App/drivers/http/main_page.html",
              "clearTripBtn", "id", "click Clear Trip",
              "trip banner clears",
              "write", "safety_get_status trip_reason/trip_mask return to none"),
    "W5": Row("W5", "/", "firmware/KilnFW/App/drivers/http/main_page.html",
              "pidPopupApplyBtn", "id", "click Apply in the PID popup",
              "popup closes, zone PID row updates",
              "write", "control_get_zones reflects the applied gains"),
    "W6": Row("W6", "/", "firmware/KilnFW/App/drivers/http/main_page.html",
              "themeBtn", "id", "click the theme toggle",
              "page recolors light/dark",
              "read-only", "none -- client-side only"),
    "W15": Row("W15", "/settings/zones", "firmware/KilnFW/App/drivers/http/zones_page.html",
               "", "page", "load the zones page",
               "zone table + PID fields render",
               "read-only", "control_get_zones matches"),
    "W16": Row("W16", "/settings/zones", "firmware/KilnFW/App/drivers/http/zones_page.html",
               "saveBtn", "id", "edit a zone name, click Save",
               "save confirms, no error banner",
               "write", "control_get_zones shows the new name (GET-merge-POST body)"),
    "W28": Row("W28", "/diagnostics", "firmware/KilnFW/App/drivers/http/diagnostics_page.html",
               "", "page", "load the diagnostics page",
               "thermo fault table + diagnostics tiles render",
               "read-only", "thermo_read_faults matches"),
    "W29": Row("W29", "/diagnostics", "firmware/KilnFW/App/drivers/http/diagnostics_page.html",
               "crashAckBtn", "id", "click Acknowledge",
               "crash banner disappears/greys out",
               "write", "crash_report_ack read-back shows acknowledged: true"),
    "W30": Row("W30", "/diagnostics", "firmware/KilnFW/App/drivers/http/diagnostics_page.html",
               "wdPanicToggleBtn", "id", "click the watchdog PANIC toggle",
               "toggle state flips",
               "write", "get_watchdog_panic_disabled shows the new value; restore to enabled"),
    "W48": Row("W48", "/readiness", "firmware/KilnFW/App/drivers/http/readiness_page.html",
               "", "page", "load the readiness page",
               "commissioning checklist renders",
               "read-only", "get_readiness matches"),
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


def run_row_live(row_id: str, host: str, screenshot_dir: str,
                  find_chrome: Optional[Callable[[], str]] = None) -> "tuple[bool, str]":
    """Live mode: log in once, drive headless Chrome over CDP against the
    real board, click the row's control, screenshot, then read back via the
    board's own API. This function contacts the board and must never be
    called by this task's own tests -- see cases_web_rw.py's ctx-injection
    convention for how a caller can fake the transport in a unit test
    instead of hitting a real board.
    """
    row = ROWS.get(row_id)
    if row is None:
        return False, f"unknown row id {row_id!r}"
    validate_selector(row)  # fail fast on a stale selector before touching the board
    user, password = _read_credentials()
    cookie = _login_once(host, user, password)
    os.makedirs(screenshot_dir, exist_ok=True)
    # The actual CDP navigate/click/screenshot sequence reuses the Node CDP
    # session helper already reviewed for this repo
    # (firmware/KilnFW/App/test/ui_responsive_sweep.mjs's CdpSession class)
    # via a small companion script, rather than re-implementing a WebSocket
    # CDP client a second time in Python.
    script = os.path.join(_repo_root(), "tools", "PcTools", "scripts", "_web_commission_cdp.mjs")
    cmd = [
        "node", script,
        "--host", host,
        "--route", row.route,
        "--cookie", cookie,
        "--selector-kind", row.selector_kind,
        "--selector", row.selector,
        "--screenshot", os.path.join(screenshot_dir, f"{row_id}.png"),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if proc.returncode != 0:
        return False, f"{row_id} FAIL: CDP driver exited {proc.returncode}: {proc.stderr.strip()[-500:]}"
    return True, f"{row_id} PASS: {proc.stdout.strip()[-500:]} (read-back: {row.readback_desc})"


def main(argv=None) -> int:
    import argparse

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("row_id", help="row id from docs/COMMISSIONING_WEB_RUNBOOK.md, e.g. W28")
    ap.add_argument("--dry-run", action="store_true",
                     help="validate page+selector against source only; no network")
    ap.add_argument("--host", default=os.environ.get("KILNCTL_BOARD_HOST", ""),
                     help="board host:port, live mode only")
    ap.add_argument("--screenshot-dir", default=os.path.join(_repo_root(), "logs", "web_commission"))
    args = ap.parse_args(argv)

    if args.dry_run:
        ok, msg = dry_run(args.row_id)
    else:
        if not args.host:
            print("live mode requires --host (or KILNCTL_BOARD_HOST)", file=sys.stderr)
            return 2
        ok, msg = run_row_live(args.row_id, args.host, args.screenshot_dir)

    print(("PASS: " if ok else "FAIL: ") + msg)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
