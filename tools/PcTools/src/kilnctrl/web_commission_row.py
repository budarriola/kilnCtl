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
    # Form-fill fields for a row whose write needs typed/selected input
    # before the click, not just a bare click (added alongside the CDP
    # script's --fills support). A tuple of (css_selector, value) pairs --
    # tuple rather than dict so the frozen dataclass stays hashable. The
    # selector here is a full CSS selector (e.g. "#pcLink"), unlike
    # `selector` above which is a bare id/text per `selector_kind`.
    fills: "Optional[tuple[tuple[str, str], ...]]" = None
    # For a simple "edit one field, Save, read GET-merge-POST style
    # read-back, then restore" row (W22, W38): the JSON keys in
    # verify_endpoint's response body holding each fill's ORIGINAL value,
    # in the same order as `fills`. run_row_live() reads these before the
    # primary click, then re-runs the same click with these original
    # values re-filled in as a second, restoring action. None = no
    # automatic restore step (either a read-only/no-fills row, or a row
    # using `special` below for a shape this generic path can't express).
    restore_from_field: "Optional[tuple[str, ...]]" = None
    # Extra element ids (beyond `selector` and any `fills` selectors) that
    # a `special` run path also depends on, checked by validate_selector so
    # a stale id in one of those paths is still caught by --dry-run.
    special_selectors: "Optional[tuple[str, ...]]" = None
    # Name of a dedicated run function in this module for a write shape the
    # generic fills/restore_from_field path can't express (currently only
    # "kiln_config_create_delete", W42's create-then-delete-the-same-slot
    # flow). None = use the generic single-click (optionally fills+restore)
    # path in run_row_live().
    special: "Optional[str]" = None
    # Other keys in verify_endpoint's response body that this row's Save
    # posts alongside the filled field and must therefore leave UNCHANGED.
    # These pages submit every field on the form in one body, not just the
    # one being edited, so a Save clicked before the page's own async
    # loadCurrent() has populated the rest of the form writes that form's
    # MARKUP DEFAULTS over the board's real values -- and
    # `restore_from_field` only ever restores the field being edited, so
    # nothing else would notice. _run_fill_and_restore() snapshots these
    # before the first Save and re-checks them after the restore, so a
    # collateral write fails the row loudly instead of passing.
    guard_fields: "Optional[tuple[str, ...]]" = None


# 27 of docs/COMMISSIONING_WEB_RUNBOOK.md's 51 rows are wired here so far
# (W1-W7, W15, W16, W21-W23, W25, W28-W31, W37-W39, W41-W43, W46, W48,
# W49, W50) -- enough to cover both selector kinds ("id" and "page") and
# both the read-only and write classes. The remaining rows are documented
# in the runbook but do not yet have a Row() entry; adding one for each is
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
#
# Second addition, same day: a `fills` primitive was added to the CDP
# script (a list of {selector, value} applied before the click), closing
# the "no form-fill primitive yet" gap noted on W16's live run. W22 (edit
# `#pcLink`, Save) and W38 (edit `#kcDpBrightness`, Save) use the generic
# fills+restore_from_field path in run_row_live(): read the field's
# current value from verify_endpoint, fill in a distinct test value and
# click Save, confirm the read-back changed, then re-fill the ORIGINAL
# value and click Save again, confirming the read-back is restored -- all
# within the one row, nothing left changed on the board. W42 (save current
# setup as a new named kiln_configs slot, then delete that same slot) uses
# a dedicated `special="kiln_config_create_delete"` path instead, since its
# restore is "delete what was just created", not "write back an old
# value" -- see _run_kiln_config_create_delete() below.
#
# Left unwired, and why (re-verified 2026-09-21, still true):
#   - W8/W9/W10 (profile create / delete / favorite toggle): creating a
#     profile safely needs the segment-builder UI (dynamically added
#     segment rows, a zone selector, per-segment temp/hold/rate fields --
#     see profiles_page.html's #segments/#addSegBtn), which the fills
#     primitive (fixed selector -> value, applied once before one click)
#     cannot drive; and delete/favorite act on a specific LIST ROW's
#     button, which only exists after the list renders with the new
#     profile in it and has no stable id (`modeDeleteBtn` arms bulk-delete
#     mode for whichever rows get checked afterward -- a second, unmodeled
#     interaction; the favorite star's own per-row button has no id either,
#     only an aria-label built from the profile's name, which the CDP
#     driver's "id"/"text" selector kinds cannot target). Getting this
#     wrong risks leaving a stray profile or, worse, deleting/favoriting
#     the wrong row. Not attempted this pass.
#
# W50 (setup wizard step save, /api/unit_pref and /api/settings/tz) turned
# out to be automatable despite the note that used to sit here: the
# wizard's own Resume button already deep-links via `#step=N` in the URL
# (gGoto()/gRenderTarget(), read back out of location.hash by loadAll() on
# page load), so navigating straight to `/setup#step=1` renders step 1's
# real form (`#wTz`, `#wUnit`, `#step1Save`) without clicking through the
# overview and stepper first -- the same one-shot navigation every other
# route-based row already relies on, not a second modeled interaction.
# It still needs its own function rather than the generic fills+restore
# path: `#step1Save` posts to TWO endpoints in one click
# (`/api/settings/tz` then `/api/unit_pref`), so no single verify_endpoint
# holds every restored field the way W22/W38 assume. See
# _run_setup_wizard_step1() below.
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
    "W22": Row("W22", "/settings/safety", "firmware/KilnFW/App/drivers/http/safety_config_page.html",
               "save", "id", "edit the PC-link abort silence timer (`#pcLink`), click Save",
               "save confirms; a whole-page GET-merge-POST via /api/zones "
               "(same route/shape as W16's #saveBtn on /settings/zones -- "
               "this page's own #save button posts to /api/zones too, "
               "despite the matrix's stale note citing "
               "/api/safety/rate_guard/auto for an older page shape)",
               "write", "GET /api/zones shows the new pc_link_abort_silence_ms, "
               "then restored to its original value by a second Save",
               verify_endpoint="/api/zones", expect_post="/api/zones",
               fills=(("#pcLink", "54000"),),
               restore_from_field=("pc_link_abort_silence_ms",),
               # /api/zones is a whole-page submit; these three top-level
               # counts ride along in every Save this page issues.
               guard_fields=("thermo_count", "relay_count", "max_simultaneous_relays")),
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
    "W38": Row("W38", "/settings/display", "firmware/KilnFW/App/drivers/http/settings_display_page.html",
               "kcDpSave", "id", "edit the brightness slider (`#kcDpBrightness`), click Save",
               "save confirms",
               "write", "GET /api/settings/display_power shows the new brightness_percent, "
               "then restored to its original value by a second Save",
               verify_endpoint="/api/settings/display_power", expect_post="/api/settings/display_power",
               fills=(("#kcDpBrightness", "45"),),
               restore_from_field=("brightness_percent",),
               # settings_display_page.html's Save always posts all four
               # fields together and has no "loaded yet?" guard, so a Save
               # racing its own loadCurrent() would write the markup
               # defaults (Never / both off) over these three.
               guard_fields=("timeout_setting", "keep_on_while_firing", "display_on_error")),
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
    "W42": Row("W42", "/settings/kiln_configs", "firmware/KilnFW/App/drivers/http/kiln_configs_page.html",
               "kcSaveNewBtn", "id",
               "fill `#kcSaveNewName` with a unique throwaway name, click Save as new "
               "(`#kcSaveNewBtn`), then select the new slot in `#kilnConfigSelect` and "
               "click Delete selected (`#kcDeleteBtn`)",
               "new slot appears in #kilnConfigSelect, then is removed",
               "write", "GET /api/kiln_configs lists the throwaway slot after create, "
               "then no longer does after delete",
               verify_endpoint="/api/kiln_configs", expect_post="/api/kiln_configs/save",
               fills=(("#kcSaveNewName", "__kc_web_commission_test__"),),
               special_selectors=("kilnConfigSelect", "kcDeleteBtn"),
               special="kiln_config_create_delete"),
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
    "W50": Row("W50", "/setup#step=1", "firmware/KilnFW/App/drivers/http/setup_wizard_page.html",
               "step1Save", "id",
               "deep-link straight to setup wizard step 1 (time zone/unit), toggle the "
               "displayed temperature unit, click Save",
               "step confirms; temp_unit flips, time_tz is re-submitted unchanged",
               "write", "GET /api/status shows temp_unit changed then restored; time_tz "
               "unchanged throughout (posts to /api/settings/tz then /api/unit_pref)",
               verify_endpoint="/api/status", expect_post="/api/unit_pref",
               # Placeholder values -- only used by validate_selector() to confirm
               # #wTz/#wUnit exist in source. _run_setup_wizard_step1() builds its
               # own fills at runtime from the board's actual current values.
               fills=(("#wTz", "EST5EDT,M3.2.0,M11.1.0"), ("#wUnit", "C")),
               special="setup_wizard_step1"),
}


class SelectorNotFoundError(RuntimeError):
    pass


def _id_present(text: str, elem_id: str) -> bool:
    """True if `elem_id` shows up as either an `id="..."` attribute or a
    getElementById() call in `text`. Shared by validate_selector()'s main
    selector check and its extra checks for `fills`/`special_selectors`
    ids, which are always plain element ids (never button text)."""
    pattern = re.compile(r"""id=["']""" + re.escape(elem_id) + r"""["']""")
    return bool(pattern.search(text)) or f"getElementById('{elem_id}')" in text \
        or f'getElementById("{elem_id}")' in text


def validate_selector(row: Row, repo_root: Optional[str] = None) -> str:
    """Reads the row's source file off disk and confirms its selector is
    present. Raises SelectorNotFoundError if the file is missing or the
    selector cannot be found. Returns a short human-readable confirmation
    string on success. Pure file I/O -- no network.

    Also confirms every id referenced by `row.fills` (stripped of its
    leading '#') and `row.special_selectors` is present, so a stale id in
    either of those -- not just the row's own primary `selector` -- is
    still caught by --dry-run before anything touches a real browser."""
    root = repo_root or _repo_root()
    path = os.path.join(root, row.source_file.replace("/", os.sep))
    if not os.path.isfile(path):
        raise SelectorNotFoundError(f"source file not found: {row.source_file}")
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()

    for extra_id in _extra_ids(row):
        if not _id_present(text, extra_id):
            raise SelectorNotFoundError(
                f"id \"{extra_id}\" (from fills/special_selectors) not found in {row.source_file}")

    if row.selector_kind == "page":
        return f"page source present ({row.source_file})"
    if row.selector_kind == "id":
        if not _id_present(text, row.selector):
            raise SelectorNotFoundError(
                f"id \"{row.selector}\" not found in {row.source_file}")
        return f'id="{row.selector}" confirmed in {row.source_file}'
    if row.selector_kind == "text":
        if row.selector not in text:
            raise SelectorNotFoundError(
                f'button text "{row.selector}" not found in {row.source_file}')
        return f'button text "{row.selector}" confirmed in {row.source_file}'
    raise SelectorNotFoundError(f"unknown selector_kind {row.selector_kind!r}")


def _extra_ids(row: Row) -> "list[str]":
    """The extra element ids a row's fills/special path depends on, beyond
    its primary `selector` -- fills selectors are CSS selectors (e.g.
    "#pcLink"), so only the leading '#' id form is checked here (the only
    form this module's Row entries ever use)."""
    out: "list[str]" = []
    for css_selector, _value in (row.fills or ()):
        if css_selector.startswith("#"):
            out.append(css_selector[1:])
    out.extend(row.special_selectors or ())
    return out


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


_PASSTHROUGH_ENV_KEYS = ("PATH", "SYSTEMROOT", "SYSTEMDRIVE", "TEMP", "TMP",
                         "COMSPEC", "WINDIR", "PROGRAMFILES", "PROGRAMFILES(X86)",
                         "LOCALAPPDATA", "APPDATA", "USERPROFILE", "NUMBER_OF_PROCESSORS")


def _child_env(cookie: str) -> dict:
    # Only KC_SID plus a minimal allowlist reaches the Chrome child -- Chrome
    # does not need this process's unrelated secrets, and a minimal env keeps
    # the child's inherited surface reviewable.
    child_env = {k: os.environ[k] for k in _PASSTHROUGH_ENV_KEYS if k in os.environ}
    child_env["KC_SID"] = cookie
    return child_env


def _run_cdp(row: Row, host: str, screenshot_dir: str, cookie: str, *,
             selector: Optional[str] = None, selector_kind: Optional[str] = None,
             fills: "Optional[tuple[tuple[str, str], ...]]" = None,
             expect_post: Optional[str] = None, accept_dialogs: Optional[bool] = None,
             shot_suffix: str = "") -> "subprocess.CompletedProcess":
    """Runs the CDP driver once against `row.route`, with any of
    selector/selector_kind/fills/expect_post/accept_dialogs overridden from
    the row's own defaults. Extracted out of run_row_live() so a row that
    needs more than one CDP action -- a fill-then-restore pair (W22, W38)
    or a create-then-delete pair (W42) -- can call this twice with
    different overrides and a distinct `shot_suffix` per screenshot,
    instead of duplicating the subprocess/env-building logic per shape."""
    selector = row.selector if selector is None else selector
    selector_kind = row.selector_kind if selector_kind is None else selector_kind
    if expect_post is None:
        expect_post = row.expect_post
    if accept_dialogs is None:
        accept_dialogs = row.classification != "read-only"
    script = os.path.join(_repo_root(), "tools", "PcTools", "scripts", "_web_commission_cdp.mjs")
    cmd = [
        "node", script,
        "--host", host,
        "--route", row.route,
        "--selector-kind", selector_kind,
        "--selector", selector,
        "--screenshot", os.path.join(screenshot_dir, f"{row.row_id}{shot_suffix}.png"),
    ]
    # Dialog policy is per row, not global: only a row this module already
    # classifies as a write/owner-gated action (or an explicit override, for
    # a restore/delete sub-step of one) may ANSWER a native confirm() with
    # OK. A read-only row that unexpectedly raises one gets it dismissed --
    # that unhangs the renderer without authorizing whatever the dialog
    # guards, which on these pages includes Danger Mode and the per-relay
    # lifetime-cycle reset. The driver logs every dialog with its message
    # either way.
    if accept_dialogs:
        cmd.append("--accept-dialogs")
    if expect_post:
        cmd += ["--expect-post", expect_post]
    if fills:
        cmd += ["--fills", json.dumps([{"selector": s, "value": v} for s, v in fills])]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=60, env=_child_env(cookie))


def _run_fill_and_restore(row: Row, host: str, screenshot_dir: str, cookie: str) -> "tuple[bool, str]":
    """Generic 'edit one or more fields, Save, confirm the read-back
    changed, put the ORIGINAL value(s) back, confirm restored' shape for a
    row with both `fills` and `restore_from_field` set (W22, W38). Every
    failure path below names exactly what state, if any, may have been
    left on the board -- this must never report PASS with a field still
    holding the test value."""
    pre_status, pre_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if pre_status != 200 or not isinstance(pre_body, dict):
        return False, (f"{row.row_id} FAIL: pre-read GET {row.verify_endpoint} -> "
                        f"{pre_status}: {json.dumps(pre_body)[:300]}")
    originals: "list[str]" = []
    for key in row.restore_from_field:
        if key not in pre_body:
            return False, f"{row.row_id} FAIL: expected field {key!r} missing from {row.verify_endpoint} body"
        originals.append(str(pre_body[key]))
    guarded = {k: str(pre_body[k]) for k in (row.guard_fields or ()) if k in pre_body}
    missing_guards = [k for k in (row.guard_fields or ()) if k not in pre_body]
    if missing_guards:
        return False, (f"{row.row_id} FAIL: guard field(s) {missing_guards} missing from "
                        f"{row.verify_endpoint} body -- cannot prove this row's Save left them "
                        f"alone, refusing to write")

    proc = _run_cdp(row, host, screenshot_dir, cookie, fills=row.fills, shot_suffix="_set")
    if proc.returncode != 0:
        return False, f"{row.row_id} FAIL: CDP driver (set) exited {proc.returncode}: {proc.stderr.strip()[-500:]}"

    mid_status, mid_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if mid_status != 200 or not isinstance(mid_body, dict):
        return False, (f"{row.row_id} FAIL: post-set read-back GET {row.verify_endpoint} -> "
                        f"{mid_status}: {json.dumps(mid_body)[:300]}")
    # Checked BEFORE the "did the edited field change?" test below: a Save
    # that raced the page's own loadCurrent() can post the form's markup
    # defaults for every field it carries, including one that leaves the
    # edited field reading its original value -- that path would otherwise
    # return "write did not land" while the collateral write went
    # unmentioned.
    drifted = [k for k, orig in guarded.items() if str(mid_body.get(k)) != orig]
    if drifted:
        return False, (f"{row.row_id} FAIL: this row's Save also CHANGED field(s) {drifted} "
                        f"(now {[mid_body.get(k) for k in drifted]!r}, were "
                        f"{[guarded[k] for k in drifted]!r}) -- collateral write, LEFT ON BOARD, "
                        f"restore by hand")
    unchanged = [k for k, orig in zip(row.restore_from_field, originals) if str(mid_body.get(k)) == orig]
    if unchanged:
        return False, (f"{row.row_id} FAIL: field(s) {unchanged} did not change after Save "
                        f"(still {[mid_body.get(k) for k in unchanged]!r}) -- write likely did not land")

    restore_fills = tuple((sel, orig) for (sel, _new), orig in zip(row.fills, originals))
    restore_proc = _run_cdp(row, host, screenshot_dir, cookie, fills=restore_fills, shot_suffix="_restore")
    if restore_proc.returncode != 0:
        return False, (f"{row.row_id} FAIL: CDP driver (restore) exited {restore_proc.returncode}: "
                        f"{restore_proc.stderr.strip()[-500:]} -- board may be LEFT with the test "
                        f"value, restore by hand: {dict(zip(row.restore_from_field, originals))}")

    final_status, final_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if final_status != 200 or not isinstance(final_body, dict):
        return False, (f"{row.row_id} FAIL: post-restore read-back GET {row.verify_endpoint} -> "
                        f"{final_status}: {json.dumps(final_body)[:300]} -- restore POST landed, "
                        f"but read-back could not confirm it")
    not_restored = [k for k, orig in zip(row.restore_from_field, originals) if str(final_body.get(k)) != orig]
    if not_restored:
        return False, (f"{row.row_id} FAIL: restore did not take -- field(s) {not_restored} still read "
                        f"{[final_body.get(k) for k in not_restored]!r}, expected original "
                        f"{dict(zip(row.restore_from_field, originals))}")
    # The restore is a second full-form Save, so it can drift the guarded
    # fields just as the first one can.
    final_drifted = [k for k, orig in guarded.items() if str(final_body.get(k)) != orig]
    if final_drifted:
        return False, (f"{row.row_id} FAIL: the restore Save CHANGED field(s) {final_drifted} "
                        f"(now {[final_body.get(k) for k in final_drifted]!r}, were "
                        f"{[guarded[k] for k in final_drifted]!r}) -- the edited field is back to "
                        f"its original, but this collateral write is LEFT ON BOARD, restore by hand")

    return True, (
        f"{row.row_id} PASS: set {dict(row.fills)} via Save, confirmed via GET {row.verify_endpoint}, "
        f"restored to original {dict(zip(row.restore_from_field, originals))} via a second Save, "
        f"confirmed restored; guarded field(s) {sorted(guarded)} confirmed unchanged throughout "
        f"| expected: {row.expected_outcome}"
    )


# Mirrors KILN_CFG_NAME_MAX_LEN (firmware/KilnFW/App/drivers/persist/kiln_cfg_store.h:87).
# The generated throwaway name below must fit this or the board's `save`
# handler correctly rejects it 400 ("name missing or too long",
# kiln_cfg_http.c:192) -- see docs/audits/w42_kiln_config_create_2026-09-21.md.
_KILN_CFG_NAME_MAX_LEN = 23


def _cdp_post_status(proc: "subprocess.CompletedProcess") -> "Optional[dict]":
    """Best-effort parse of the CDP driver's final JSON line (stdout) to
    pull out the `post` field recorded by `--expect-post` (method/url/
    status/failed -- see _web_commission_cdp.mjs's `settle()`). Returns
    None if stdout wasn't the expected JSON shape, so callers always have
    a status/body-ish string to report instead of a generic message."""
    try:
        parsed = json.loads(proc.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return None
    post = parsed.get("post") if isinstance(parsed, dict) else None
    return post if isinstance(post, dict) else None


def _run_kiln_config_create_delete(row: Row, host: str, screenshot_dir: str, cookie: str) -> "tuple[bool, str]":
    """W42's shape: save the board's CURRENT setup as a new, uniquely-named
    kiln_configs slot, confirm it appears, select that exact slot (matched
    by the unique name this function chose, never whatever the dropdown
    happens to have selected) and delete it, confirm it's gone. Never
    touches any other slot -- the active one or a builtin -- since it only
    ever selects the id it just read back for its own generated name."""
    # Short and clearly a test artifact, but well under _KILN_CFG_NAME_MAX_LEN
    # (root cause of the 2026-09-21 W42 FAIL: the old 37-char name overflowed
    # the firmware's 23-char limit and was correctly rejected 400). The FULL
    # epoch seconds are kept (18 chars, still well inside the limit) rather
    # than a modulo: truncating to 5 digits wraps every ~27.8 h, so a
    # leftover slot from a previous day could collide, and firmware refuses
    # a duplicate name ("a saved kiln config already has that name",
    # kiln_cfg_store.c:1151) -- a needless FAIL.
    unique_name = f"kc_test_{int(time.time())}"
    assert len(unique_name) <= _KILN_CFG_NAME_MAX_LEN, (
        f"generated kiln_config test name {unique_name!r} ({len(unique_name)} chars) "
        f"exceeds _KILN_CFG_NAME_MAX_LEN ({_KILN_CFG_NAME_MAX_LEN})")
    create_fills = (("#kcSaveNewName", unique_name),)

    pre_status, pre_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if pre_status != 200 or not isinstance(pre_body, dict):
        return False, (f"{row.row_id} FAIL: pre-read GET {row.verify_endpoint} -> "
                        f"{pre_status}: {json.dumps(pre_body)[:300]}")

    proc = _run_cdp(row, host, screenshot_dir, cookie, fills=create_fills, shot_suffix="_create")
    if proc.returncode != 0:
        return False, f"{row.row_id} FAIL: CDP driver (create) exited {proc.returncode}: {proc.stderr.strip()[-500:]}"
    create_post = _cdp_post_status(proc)
    if create_post is not None and (create_post.get("failed")
                                    or create_post.get("status") not in (200, None)):
        # A clean 4xx/5xx here means the save POST completed but was
        # rejected -- report that instead of letting the read-back below
        # produce the generic, cause-blind "write did not land".
        return False, (f"{row.row_id} FAIL: create POST {create_post.get('url')} -> "
                        f"{create_post.get('status')} (failed={create_post.get('failed')}) -- "
                        f"save did not succeed, see board's error response for detail")

    mid_status, mid_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if mid_status != 200 or not isinstance(mid_body, dict):
        return False, (f"{row.row_id} FAIL: post-create read-back GET {row.verify_endpoint} -> "
                        f"{mid_status}: {json.dumps(mid_body)[:300]}")
    configs = mid_body.get("configs") or []
    match = [c for c in configs if isinstance(c, dict) and c.get("name") == unique_name]
    if not match:
        return False, (f"{row.row_id} FAIL: no config named {unique_name!r} found in "
                        f"{row.verify_endpoint} after create -- write did not land")
    new_id = str(match[0].get("id"))

    delete_fills = (("#kilnConfigSelect", new_id),)
    delete_proc = _run_cdp(row, host, screenshot_dir, cookie,
                            selector="kcDeleteBtn", selector_kind="id",
                            fills=delete_fills, expect_post="/api/kiln_configs/delete",
                            accept_dialogs=True, shot_suffix="_delete")
    if delete_proc.returncode != 0:
        return False, (f"{row.row_id} FAIL: CDP driver (delete) exited {delete_proc.returncode}: "
                        f"{delete_proc.stderr.strip()[-500:]} -- board may be LEFT with throwaway "
                        f"config id={new_id} name={unique_name!r}, delete by hand")

    final_status, final_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if final_status != 200 or not isinstance(final_body, dict):
        return False, (f"{row.row_id} FAIL: post-delete read-back GET {row.verify_endpoint} -> "
                        f"{final_status}: {json.dumps(final_body)[:300]} -- delete POST landed, "
                        f"but read-back could not confirm it")
    still_present = [c for c in (final_body.get("configs") or [])
                      if isinstance(c, dict) and c.get("name") == unique_name]
    if still_present:
        return False, (f"{row.row_id} FAIL: config {unique_name!r} (id={new_id}) still present "
                        f"after delete -- LEFT ON BOARD, delete by hand")

    return True, (
        f"{row.row_id} PASS: created {unique_name!r} (id={new_id}) via Save as new, confirmed via "
        f"GET {row.verify_endpoint}, deleted it via Delete selected, confirmed removed | "
        f"expected: {row.expected_outcome}"
    )


def _run_setup_wizard_step1(row: Row, host: str, screenshot_dir: str, cookie: str) -> "tuple[bool, str]":
    """W50's shape: the setup wizard's step-1 screen (time zone + displayed
    unit) is built entirely by client-side JS after an async fetch
    (renderStep1() in setup_wizard_page.html) -- unlike a static-markup
    page whose form fields exist immediately and are merely updated later
    (kcDpBrightness on W38, say), #wTz/#wUnit do not exist in the DOM until
    that fetch resolves, on top of the two fetches loadAll() already makes
    before it renders any step at all. `_run_cdp()`'s fixed post-navigate
    settle delay predates this row and was tuned against pages with
    static markup; a live run of this row that FAILs with "not found on
    page" most likely means that chain didn't finish in time, not that the
    selectors are wrong -- re-run before assuming a real defect.

    Only the displayed temperature unit is actually toggled and restored;
    the time-zone field is always re-submitted with its OWN current value,
    never a distinct test string, since #step1Save posts both fields in
    one click (`/api/settings/tz` then `/api/unit_pref`) and there is no
    reason to touch a real timezone rule -- which also firing logs are
    timestamped with, per the page's own hint text -- just to prove this
    control works. Needs its own function rather than the generic
    fills+restore_from_field path (W22/W38) because those two fields live
    behind two different POST routes, not one verify_endpoint holding
    both."""
    pre_status, pre_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if pre_status != 200 or not isinstance(pre_body, dict):
        return False, (f"{row.row_id} FAIL: pre-read GET {row.verify_endpoint} -> "
                        f"{pre_status}: {json.dumps(pre_body)[:300]}")
    orig_tz = pre_body.get("time_tz")
    orig_unit = pre_body.get("temp_unit")
    if not orig_tz or orig_unit not in ("C", "F"):
        return False, (f"{row.row_id} FAIL: {row.verify_endpoint} body missing usable "
                        f"time_tz/temp_unit ({json.dumps(pre_body)[:300]}) -- refusing to guess "
                        f"a value to restore to")
    test_unit = "F" if orig_unit == "C" else "C"

    set_fills = (("#wTz", orig_tz), ("#wUnit", test_unit))
    proc = _run_cdp(row, host, screenshot_dir, cookie, fills=set_fills, shot_suffix="_set")
    if proc.returncode != 0:
        return False, f"{row.row_id} FAIL: CDP driver (set) exited {proc.returncode}: {proc.stderr.strip()[-500:]}"

    mid_status, mid_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if mid_status != 200 or not isinstance(mid_body, dict):
        return False, (f"{row.row_id} FAIL: post-set read-back GET {row.verify_endpoint} -> "
                        f"{mid_status}: {json.dumps(mid_body)[:300]}")
    if str(mid_body.get("time_tz")) != str(orig_tz):
        return False, (f"{row.row_id} FAIL: this row's Save also CHANGED time_tz (now "
                        f"{mid_body.get('time_tz')!r}, was {orig_tz!r}) -- collateral write, LEFT "
                        f"ON BOARD, restore by hand")
    if mid_body.get("temp_unit") != test_unit:
        return False, (f"{row.row_id} FAIL: temp_unit did not change (still "
                        f"{mid_body.get('temp_unit')!r}, expected {test_unit!r}) -- write likely "
                        f"did not land")

    restore_fills = (("#wTz", orig_tz), ("#wUnit", orig_unit))
    restore_proc = _run_cdp(row, host, screenshot_dir, cookie, fills=restore_fills, shot_suffix="_restore")
    if restore_proc.returncode != 0:
        return False, (f"{row.row_id} FAIL: CDP driver (restore) exited {restore_proc.returncode}: "
                        f"{restore_proc.stderr.strip()[-500:]} -- board may be LEFT with "
                        f"temp_unit={test_unit!r}, restore by hand (originally {orig_unit!r})")

    final_status, final_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if final_status != 200 or not isinstance(final_body, dict):
        return False, (f"{row.row_id} FAIL: post-restore read-back GET {row.verify_endpoint} -> "
                        f"{final_status}: {json.dumps(final_body)[:300]} -- restore POST landed, "
                        f"but read-back could not confirm it")
    if str(final_body.get("time_tz")) != str(orig_tz):
        return False, (f"{row.row_id} FAIL: the restore Save CHANGED time_tz (now "
                        f"{final_body.get('time_tz')!r}, expected {orig_tz!r}) -- LEFT ON BOARD, "
                        f"restore by hand")
    if final_body.get("temp_unit") != orig_unit:
        return False, (f"{row.row_id} FAIL: restore did not take -- temp_unit still "
                        f"{final_body.get('temp_unit')!r}, expected original {orig_unit!r}")

    return True, (
        f"{row.row_id} PASS: toggled temp_unit {orig_unit!r} -> {test_unit!r} via setup wizard "
        f"step 1 Save (time_tz re-submitted unchanged at {orig_tz!r}), confirmed via GET "
        f"{row.verify_endpoint}, restored to {orig_unit!r} via a second Save, confirmed restored "
        f"| expected: {row.expected_outcome}"
    )


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

    # A row with a dedicated multi-step shape (currently only W42's
    # create-then-delete) is handled entirely by its own function -- it
    # does its own CDP invocations, read-backs and restore, and returns
    # directly rather than falling through to the generic single-click path
    # below.
    if row.special == "kiln_config_create_delete":
        return _run_kiln_config_create_delete(row, host, screenshot_dir, cookie)
    if row.special == "setup_wizard_step1":
        return _run_setup_wizard_step1(row, host, screenshot_dir, cookie)

    # A row with both `fills` and `restore_from_field` set (W22, W38) is the
    # generic "edit a field, Save, confirm, restore, confirm" shape -- also
    # handled by its own function so this one stays the simple single-click
    # path for every other row.
    if row.fills and row.restore_from_field:
        return _run_fill_and_restore(row, host, screenshot_dir, cookie)

    # The actual CDP navigate/click/screenshot sequence reuses the Node CDP
    # session helper already reviewed for this repo
    # (firmware/KilnFW/App/test/ui_responsive_sweep.mjs's CdpSession class)
    # via a small companion script, rather than re-implementing a WebSocket
    # CDP client a second time in Python. The session cookie is passed
    # through the child's environment, never on argv, so it cannot leak via
    # a process listing or a logged command line.
    proc = _run_cdp(row, host, screenshot_dir, cookie, fills=row.fills)
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
