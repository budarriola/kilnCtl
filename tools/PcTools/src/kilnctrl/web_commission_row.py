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

from . import http_auth


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
    # generic fills/restore_from_field path can't express: "kiln_config_
    # create_delete" (W42's create-then-delete-the-same-slot flow),
    # "setup_wizard_step1" (W50), "profile_segment_create_delete" (W8),
    # "profile_multi_delete" (W9), "profile_favorite_toggle" (W10). None =
    # use the generic single-click (optionally fills+restore) path in
    # run_row_live().
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


# 34 of docs/COMMISSIONING_WEB_RUNBOOK.md's 51 rows are wired here so far
# (W1-W11, W15, W16, W18, W20, W21-W23, W25, W28-W31, W33, W37-W39, W41-W43,
# W46, W48, W49, W50) -- enough to cover both selector kinds ("id" and
# "page") and
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
# W8/W9/W10 (profile create / delete / favorite toggle) were left unwired
# through 2026-09-21 for exactly the reasons this note used to describe:
# the segment-builder UI needs more than one fixed selector->value fill,
# and delete/favorite act on a specific LIST ROW's control that only
# exists after the list renders with the new profile in it and has no
# stable id -- the favorite star's own textContent is the bare glyph
# "☆"/"★", IDENTICAL on every row, which the "text" selector kind would
# have matched on whichever profile renders first, not the intended one.
# Wired 2026-09-22 once the CDP driver gained `--steps` (ordered
# click/fill/wait-for-selector/wait-for-post) and the `aria-label`/`css`
# selector kinds (`_web_commission_cdp.mjs`'s `elementExpr()`): the
# per-row favorite/delete/select-for-delete controls DO carry a per-profile
# EXACT aria-label (`Add "<name>" to favorites"`, `Delete "<name>"`,
# `Select "<name>" for delete"`, all confirmed against
# profiles_page.html's favToggleBtn()/iconBtn()/selectionCheckbox()), which
# the "text"-substring kind could never disambiguate but "aria-label"'s
# exact match can -- see _run_profile_segment_create_delete(),
# _run_profile_multi_delete(), _run_profile_favorite_toggle() below.
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
               "(`#kcSaveNewBtn`) -- firmware marks the new slot ACTIVE -- then re-select the "
               "original active slot in `#kilnConfigSelect` and click Apply (`#kcApplyBtn`) to "
               "restore it, then select the throwaway slot and click Delete selected "
               "(`#kcDeleteBtn`)",
               "new slot appears in #kilnConfigSelect, active id returns to the original after "
               "Apply, then the throwaway slot is removed",
               "write", "GET /api/kiln_configs lists the throwaway slot after create, "
               "active_id equals the original after Apply, and the throwaway slot is gone "
               "with active_id still the original after delete",
               verify_endpoint="/api/kiln_configs", expect_post="/api/kiln_configs/save",
               fills=(("#kcSaveNewName", "__kc_web_commission_test__"),),
               special_selectors=("kilnConfigSelect", "kcApplyBtn", "kcDeleteBtn"),
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
    # W8/W9/W10, wired 2026-09-22 using the --steps/aria-label/css selector
    # support that landed for exactly this purpose (see the "Left unwired"
    # note above, now stale for these three). Every path a Row() here can
    # express statically is checked by validate_selector() via
    # special_selectors; the parts that genuinely cannot be checked without
    # a live board -- the per-profile aria-label text built at runtime
    # (favToggleBtn()/selectionCheckbox()/the per-row delete icon in
    # profiles_page.html) and the zone-checkbox/segment-field CSS class
    # selectors (.pzone-cb/.s-target/.s-ramp/.s-dwell, which have no id at
    # all) -- were verified by hand against profiles_page.html's source
    # instead and are cited in each special function's own docstring.
    "W8": Row("W8", "/profiles", "firmware/KilnFW/App/drivers/http/profiles_page.html",
              "saveBtn", "id",
              "create a scratch profile via the segment builder (name, one zone, one "
              "temperature ramp/dwell segment), Save, then delete it via its own row's "
              "Delete icon",
              "profile appears in the list after Save, then disappears after Delete",
              "write", "GET /api/profiles shows the scratch profile after create, then "
              "shows it gone after delete",
              verify_endpoint="/api/profiles", expect_post="/api/profile",
              special="profile_segment_create_delete",
              special_selectors=("pname",)),
    "W9": Row("W9", "/profiles", "firmware/KilnFW/App/drivers/http/profiles_page.html",
              "modeDeleteBtn", "id",
              "create two scratch profiles, enter delete mode, tick both via their "
              "per-row select-for-delete checkboxes, click Bulk delete",
              "both scratch profiles disappear from the list",
              "write", "GET /api/profiles shows neither scratch profile afterward",
              verify_endpoint="/api/profiles", expect_post="/api/profile/delete",
              special="profile_multi_delete",
              special_selectors=("pname", "saveBtn", "bulkActionBtn")),
    "W10": Row("W10", "/profiles", "firmware/KilnFW/App/drivers/http/profiles_page.html",
               "saveBtn", "id",
               "create a scratch profile, toggle its favorite star on, confirm, toggle "
               "it back off, confirm, then delete the scratch profile",
               "favorite star toggles on then off; scratch profile is deleted afterward",
               "write", "GET /api/profiles/favorites includes then excludes the scratch "
               "profile's id",
               verify_endpoint="/api/profiles/favorites", expect_post="/api/profile/favorite",
               special="profile_favorite_toggle",
               special_selectors=("pname",)),
    # W11, W18, W20, W33 -- wired 2026-09-21 (see the header comment above
    # this dict for the general 30-row baseline this adds to). W11 is a bare
    # page load, same shape as W7/W49. W18/W20/W33 are the three "abort/exit
    # only" controls the runbook itself classifies read-only because they
    # can only cancel, never start, an operation -- but clicking one for
    # real still needs a precondition: if the corresponding status route
    # already shows an operation genuinely in progress (a real sweep,
    # autotune run, or danger-mode session another operator started),
    # clicking abort/exit would disrupt THEIR work, not just probe the
    # control. Each is guarded by _run_guarded_click(): refuse before
    # touching the board unless the status read-back already shows idle/
    # inactive, then click and confirm it stays idle/inactive -- a no-op by
    # construction, so there is nothing to restore. See
    # _run_sweep_abort_guarded()/_run_autotune_abort_guarded()/
    # _run_danger_exit_guarded() below.
    "W11": Row("W11", "/live_profile", "firmware/KilnFW/App/drivers/http/live_profile_page.html",
               "", "page", "load the live_profile page",
               "working-copy status/content renders (empty/no-working-copy state outside a firing)",
               "read-only", "GET /api/profile/live matches",
               verify_endpoint="/api/profile/live"),
    "W18": Row("W18", "/settings/zones", "firmware/KilnFW/App/drivers/http/zones_page.html",
               "sweepAbortBtn", "id", "click Abort sweep, guarded",
               "sweep UI stays/returns to idle",
               "read-only", "GET /api/zones/current_sweep/status state remains not-running",
               verify_endpoint="/api/zones/current_sweep/status",
               expect_post="/api/zones/current_sweep/abort",
               special="sweep_abort_guarded"),
    "W20": Row("W20", "/settings/zones", "firmware/KilnFW/App/drivers/http/zones_page.html",
               "atAbortBtn", "id", "click Abort autotune, guarded",
               "autotune UI stays/returns to idle",
               "read-only", "GET /api/autotune state remains idle",
               verify_endpoint="/api/autotune", expect_post="/api/autotune/abort",
               special="autotune_abort_guarded"),
    "W33": Row("W33", "/diagnostics", "firmware/KilnFW/App/drivers/http/diagnostics_page.html",
               "dangerExitBtn", "id", "click Exit Danger Mode, guarded",
               "danger banner stays/returns cleared",
               "read-only", "GET /api/diagnostics/danger active remains false",
               verify_endpoint="/api/diagnostics/danger", expect_post="/api/diagnostics/danger/stop",
               special="danger_exit_guarded"),
}

# Rows left unwired after this pass, and why (each is also noted in its own
# runbook row): W12/W13/W14 (live_profile fork/edit/discard) need an actual
# firing running to mean anything -- starting heat is out of scope for this
# driver. W17 (current-sweep Start) and W19 (autotune Start) actively drive
# every zone's relays/heaters -- owner-gated, out of scope. W24 (safety page
# Clear Trip) needs the board already latched into a specific S6a-only trip
# state; creating or safely verifying that precondition is out of scope, and
# W4 (already wired) exercises the same clear-trip shape on a different
# page, so this duplicate control adds no new coverage worth the risk. W26
# (commissioning wizard commit) and W27 (CT auto-zero) are owner-gated
# safety-configuration writes (W26 also needs a deliberate Pico reset/GRACE
# window). W32 (Enter Danger Mode) drives relays outside the normal
# safety-gated path -- owner-gated. W34 (E-stop verify) needs the bench
# E-stop jumper physically pulled, which no CDP driver can do. W35 (relay-
# cycle reset) has a known restore anomaly (B12: relays 1/2's restore can
# clamp to an internal count not shown on /api/status) that would make a
# mechanical PASS/FAIL unreliable. W36 (reboot both processors) has no
# meaningful "restore" for a reboot and needs a subsequent trip-clear per
# CLAUDE.md's S6a note -- an attended, not mechanically-swept, action. W40
# (set web password/policy) changes auth credentials -- explicitly out of
# scope. W44 (backup Export download) needs CDP download interception this
# driver doesn't have, and has no read-back distinct from W43's. W45
# (backup Import) is a destructive overwrite -- owner-gated. W47 (OTA
# update/rollback) is flash/OTA -- explicitly out of scope, use
# flash_firmware()/ota_rollback_esp() instead. W51 (Wi-Fi scan/connect/
# forget) can change Wi-Fi credentials/reachability -- explicitly out of
# scope.


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

def _login_once(host: str, timeout: float = 10.0) -> str:
    """One POST /api/auth/login via http_auth's own login seam (the same
    one urlopen() uses on a 401), so this module's login has to agree with
    every other client's about the request shape, the cookie name, and
    session bookkeeping, instead of a second, independently-maintained
    implementation of the same POST. Returns the session cookie value
    (needed as a raw value here, not just an authenticated response --
    it is handed to a CDP-driven Chrome child via KC_SID, which has no way
    to go through http_auth's request/response seam itself). Never logs
    the password. http_auth.login() always reads the credential from the
    environment itself (the same KILNCTL_WEB_USERNAME/PASSWORD this
    module's callers already read via _read_credentials())."""
    return http_auth.login(f"http://{host}", timeout=timeout)


def _read_credentials() -> "tuple[str, str]":
    user = os.environ.get("KILNCTL_WEB_USERNAME")
    pw = os.environ.get("KILNCTL_WEB_PASSWORD")
    if not user or not pw:
        raise RuntimeError(
            "KILNCTL_WEB_USERNAME/KILNCTL_WEB_PASSWORD not set (bool presence: "
            f"user={bool(user)} pw={bool(pw)})")
    return user, pw


def _get_json_with_cookie(host: str, path: str, cookie: str, timeout: float = 5.0) -> "tuple[Optional[int], Optional[dict]]":
    """Authenticated GET, used only for the post-action read-back below.
    Returns (status, parsed-json-or-None). Goes through http_auth.urlopen()
    with ``no_relogin=True`` -- the ``cookie`` passed in is the SAME session
    the CDP-driven Chrome child is using (handed to it via KC_SID), not one
    this module's own process logged in for. If that specific session has
    gone stale and the board answers 401, silently falling back to
    http_auth's own env-credential login (its default behaviour for every
    other caller) would authenticate THIS request with a fresh session
    while the browser half keeps presenting the old, now-invalid cookie --
    a Python-side read-back could then report PASS while the actual UI
    action never happened under authentication. ``no_relogin=True`` makes
    that divergence impossible: a 401 here is returned as a failed
    read-back, never quietly repaired."""
    url = f"http://{host}{path}"
    req = urllib.request.Request(url, headers={"Cookie": f"kiln_sid={cookie}", "Accept-Encoding": "identity"})
    try:
        with http_auth.urlopen(req, timeout=timeout, no_relogin=True) as resp:
            status = resp.getcode()
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        if exc.code == 401:
            return None, {"error": "session cookie rejected (401)"}
        try:
            text = exc.read().decode("utf-8", errors="replace")
        except Exception:  # noqa: BLE001
            text = None
        status = exc.code
    except (urllib.error.URLError, OSError, http_auth.HttpAuthError) as exc:
        return None, {"error": str(exc)}
    try:
        return status, json.loads(text) if text else None
    except (ValueError, TypeError):
        return status, None


def _post_form_with_cookie(host: str, path: str, cookie: str, fields: dict,
                            timeout: float = 5.0) -> "tuple[Optional[int], Optional[dict]]":
    """Authenticated form-urlencoded POST, used for the setup-progress
    save/restore in `_run_setup_wizard_step1()` (this module's other write
    paths all go through the CDP driver instead, since they need a real
    page's own JS to build the request; the progress route's shape is
    simple enough -- `step=<n>&state=<name>[&note=...]` -- to post directly
    the same way `_login_once()` already posts the login form). Returns
    (status, parsed-json-or-None), same contract as
    `_get_json_with_cookie()`. Goes through http_auth.urlopen() with
    ``no_relogin=True`` for the same reason `_get_json_with_cookie()` does
    -- see its docstring."""
    url = f"http://{host}{path}"
    body = urllib.parse.urlencode(fields).encode("utf-8")
    req = urllib.request.Request(
        url, data=body, method="POST",
        headers={
            "Content-Type": "application/x-www-form-urlencoded",
            "Cookie": f"kiln_sid={cookie}",
            "Accept-Encoding": "identity",
        },
    )
    try:
        with http_auth.urlopen(req, timeout=timeout, no_relogin=True) as resp:
            status = resp.getcode()
            text = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        if exc.code == 401:
            return None, {"error": "session cookie rejected (401)"}
        try:
            text = exc.read().decode("utf-8", errors="replace")
        except Exception:  # noqa: BLE001
            text = None
        status = exc.code
    except (urllib.error.URLError, OSError, http_auth.HttpAuthError) as exc:
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
             steps: "Optional[list[dict]]" = None,
             expect_post: Optional[str] = None, accept_dialogs: Optional[bool] = None,
             shot_suffix: str = "") -> "subprocess.CompletedProcess":
    """Runs the CDP driver once against `row.route`, with any of
    selector/selector_kind/fills/steps/expect_post/accept_dialogs overridden
    from the row's own defaults. Extracted out of run_row_live() so a row
    that needs more than one CDP action -- a fill-then-restore pair (W22,
    W38) or a create-then-delete pair (W42) -- can call this twice with
    different overrides and a distinct `shot_suffix` per screenshot,
    instead of duplicating the subprocess/env-building logic per shape.

    `steps` (added for W8/W9/W10) is an ordered list of the CDP script's
    step objects (click/fill/wait-for-selector/wait-for-post -- see
    _web_commission_cdp.mjs's `runStep()`), forwarded verbatim as
    `--steps <json>`. When `steps` is given, this call defaults to
    steps-only: no top-level `--selector-kind`/`--selector` is emitted
    unless the caller ALSO passes an explicit `selector`/`selector_kind`
    override. This matters because a `special` row's own `row.selector`/
    `row.selector_kind` exist only so `validate_selector()` has something
    static to check (a stable id referenced somewhere in the row's flow),
    not because the row wants one more click tacked on after its steps
    already ran -- the .mjs script itself documents this ("a --steps run
    ... may have no top-level click at all").
    """
    if steps is None:
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
        "--screenshot", os.path.join(screenshot_dir, f"{row.row_id}{shot_suffix}.png"),
    ]
    if selector_kind is not None:
        cmd += ["--selector-kind", selector_kind, "--selector", selector]
    # Dialog policy is per row, not global: only a row this module already
    # classifies as a write/owner-gated action (or an explicit override, for
    # a restore/delete sub-step of one) may ANSWER a confirmation (the in-page kcConfirm() modal) with
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
    if steps:
        cmd += ["--steps", json.dumps(steps)]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=60, env=_child_env(cookie))


def _run_fill_and_restore(row: Row, host: str, screenshot_dir: str, cookie: str) -> "tuple[bool, str]":
    """Generic 'edit one or more fields, Save, confirm the read-back
    changed, put the ORIGINAL value(s) back, confirm restored' shape for a
    row with both `fills` and `restore_from_field` set (W22, W38). Every
    failure path below names exactly what state, if any, may have been
    left on the board -- this must never report PASS with a field still
    holding the test value.

    Restore is UNCONDITIONAL once the first (flipping) Save has been
    attempted: every failure path after that point -- a CDP driver that
    exits non-zero or raises, a read-back that does not answer 200, a
    collateral guard-field write, or a field that never changed -- still
    runs the restoring Save and appends its outcome to the failure
    message. The board must never be left holding the test value merely
    because the check that would have noticed happened to fail first
    (mirrors the W50 fix, 14e2324b)."""
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

    restore_fills = tuple((sel, orig) for (sel, _new), orig in zip(row.fills, originals))

    def _restore() -> "tuple[bool, str]":
        """The restoring second Save, plus its read-back. Called on EVERY
        path that got as far as attempting the first (flipping) Save --
        including a mid-state read-back that failed, a CDP driver that
        errored, a collateral guard-field write, or a field that never
        changed -- because any of those may have left the board on the
        test value(s), and this row must never end with a field still
        flipped."""
        try:
            restore_proc = _run_cdp(row, host, screenshot_dir, cookie,
                                     fills=restore_fills, shot_suffix="_restore")
        except Exception as exc:  # noqa: BLE001 -- subprocess timeout/OSError must not skip the report
            return False, (f" -- RESTORE ATTEMPT ITSELF FAILED ({type(exc).__name__}: {exc}); board may "
                            f"be LEFT with the test value, restore by hand: "
                            f"{dict(zip(row.restore_from_field, originals))}")
        if restore_proc.returncode != 0:
            return False, (f" -- RESTORE FAILED: CDP driver (restore) exited {restore_proc.returncode}: "
                            f"{restore_proc.stderr.strip()[-500:]}; board may be LEFT with the test "
                            f"value, restore by hand: {dict(zip(row.restore_from_field, originals))}")
        final_status, final_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
        if final_status != 200 or not isinstance(final_body, dict):
            return False, (f" -- RESTORE UNCONFIRMED: post-restore read-back GET {row.verify_endpoint} "
                            f"-> {final_status}: {json.dumps(final_body)[:300]}; restore POST landed, "
                            f"but read-back could not confirm it")
        not_restored = [k for k, orig in zip(row.restore_from_field, originals) if str(final_body.get(k)) != orig]
        if not_restored:
            return False, (f" -- RESTORE FAILED: field(s) {not_restored} still read "
                            f"{[final_body.get(k) for k in not_restored]!r}, expected original "
                            f"{dict(zip(row.restore_from_field, originals))} -- LEFT ON BOARD, "
                            f"restore by hand")
        # The restore is a second full-form Save, so it can drift the
        # guarded fields just as the first one can.
        final_drifted = [k for k, orig in guarded.items() if str(final_body.get(k)) != orig]
        if final_drifted:
            return False, (f" -- RESTORE FAILED: the restore Save CHANGED field(s) {final_drifted} "
                            f"(now {[final_body.get(k) for k in final_drifted]!r}, were "
                            f"{[guarded[k] for k in final_drifted]!r}) -- the edited field is back to "
                            f"its original, but this collateral write is LEFT ON BOARD, restore by hand")
        return True, (f" -- board restored to {dict(zip(row.restore_from_field, originals))} and "
                       f"confirmed; guarded field(s) {sorted(guarded)} confirmed unchanged")

    failure: "Optional[str]" = None
    try:
        proc = _run_cdp(row, host, screenshot_dir, cookie, fills=row.fills, shot_suffix="_set")
    except Exception as exc:  # noqa: BLE001
        # The driver can die AFTER the click's POST landed (a screenshot or
        # teardown error, a 60 s timeout on a page that already saved), so a
        # raise here is not proof nothing was written -- restore anyway.
        failure = f"{row.row_id} FAIL: CDP driver (set) raised {type(exc).__name__}: {exc}"
    else:
        if proc.returncode != 0:
            failure = (f"{row.row_id} FAIL: CDP driver (set) exited {proc.returncode}: "
                        f"{proc.stderr.strip()[-500:]}")

    if failure is None:
        mid_status, mid_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
        if mid_status != 200 or not isinstance(mid_body, dict):
            failure = (f"{row.row_id} FAIL: post-set read-back GET {row.verify_endpoint} -> "
                        f"{mid_status}: {json.dumps(mid_body)[:300]}")
        else:
            # Checked BEFORE the "did the edited field change?" test below: a
            # Save that raced the page's own loadCurrent() can post the
            # form's markup defaults for every field it carries, including
            # one that leaves the edited field reading its original value --
            # that path would otherwise return "write did not land" while
            # the collateral write went unmentioned.
            drifted = [k for k, orig in guarded.items() if str(mid_body.get(k)) != orig]
            unchanged = [k for k, orig in zip(row.restore_from_field, originals) if str(mid_body.get(k)) == orig]
            if drifted:
                failure = (f"{row.row_id} FAIL: this row's Save also CHANGED field(s) {drifted} "
                            f"(now {[mid_body.get(k) for k in drifted]!r}, were "
                            f"{[guarded[k] for k in drifted]!r}) -- collateral write")
            elif unchanged:
                failure = (f"{row.row_id} FAIL: field(s) {unchanged} did not change after Save "
                            f"(still {[mid_body.get(k) for k in unchanged]!r}) -- write likely did "
                            f"not land")

    restored_ok, restore_note = _restore()
    if failure is not None:
        return False, failure + restore_note
    if not restored_ok:
        return False, f"{row.row_id} FAIL:{restore_note}"

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


def _cdp_post_statuses(proc: "subprocess.CompletedProcess", paths: "tuple[str, ...]") -> "dict[str, list[dict]]":
    """Best-effort parse of the CDP driver's final JSON line to pull out
    EVERY completed POST whose URL contains one of `paths`, from the
    `network` field (`cdp.completed` in _web_commission_cdp.mjs -- the same
    request/response bookkeeping `--expect-post`/`_cdp_post_status()`
    already reads from, just the whole list instead of one cursor-tracked
    match). Needed for a row whose single click fires more than one POST it
    must grade individually (W50: `/api/settings/tz` then `/api/unit_pref`,
    both ahead of the `/api/setup/progress` POST `--expect-post` already
    waits for and that `_cdp_post_status()` reports). Returns a dict keyed
    by the matched path with a list of {method,url,status,failed} records
    (usually one; a list because nothing prevents more than one match), or
    an empty dict per path with no match at all -- distinguish "no record"
    from "recorded and it answered 2xx" the same way callers of
    `_cdp_post_status()` already do (a status of None means "couldn't
    observe it", not "it failed"). Returns {} entirely if stdout wasn't the
    expected JSON shape (e.g. `_FakeProc`'s `{"ok": true}` in tests, or
    firmware/driver output that predates this field)."""
    try:
        parsed = json.loads(proc.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return {}
    network = parsed.get("network") if isinstance(parsed, dict) else None
    if not isinstance(network, list):
        return {}
    out: "dict[str, list[dict]]" = {}
    for path in paths:
        # Compare the URL's PATH COMPONENT for equality rather than asking
        # `path in url`: a substring test would also match a longer route
        # that merely starts with (or contains) this one -- a hypothetical
        # `/api/unit_prefs` would satisfy `"/api/unit_pref" in url`, and
        # grading THAT request's status as if it were this row's write is
        # exactly the silent wrong-route confusion this function exists to
        # avoid. urlsplit() also strips any query/fragment, so a URL the
        # page happens to cache-bust is still matched on its real route.
        matches = [rec for rec in network
                   if isinstance(rec, dict) and rec.get("method") == "POST"
                   and urllib.parse.urlsplit(str(rec.get("url") or "")).path == path]
        if matches:
            out[path] = matches
    return out


def _cdp_stdout_summary(proc: "subprocess.CompletedProcess", limit: int = 300) -> str:
    """The tail of the CDP driver's stdout for a PASS message, with the
    `network` field (every completed request of the whole run -- see
    `_cdp_post_statuses()`) dropped first. That list is unbounded and sits
    last in the driver's JSON object, so a raw `stdout[-limit:]` would show
    nothing but request records and push the fields a reader actually wants
    (route, selector, fills, dialogs, post) off the front for EVERY row,
    not just the one that asked for `network`. Falls back to the raw tail
    if stdout isn't the expected JSON shape."""
    raw = (proc.stdout or "").strip()
    try:
        parsed = json.loads(raw.splitlines()[-1])
    except (ValueError, IndexError):
        return raw[-limit:]
    if not isinstance(parsed, dict) or "network" not in parsed:
        return raw[-limit:]
    trimmed = {k: v for k, v in parsed.items() if k != "network"}
    trimmed["network_count"] = len(parsed["network"]) if isinstance(parsed["network"], list) else None
    return json.dumps(trimmed)[-limit:]


def _non2xx_post_failures(post_statuses: "dict[str, list[dict]]") -> "list[str]":
    """Given `_cdp_post_statuses()`'s output, names each recorded POST that
    did not answer 2xx (or was flagged `failed`), one entry per bad record.
    A path this run never observed a POST for is silently skipped here --
    that is "couldn't observe it", graded elsewhere, not a failure of this
    check."""
    bad: "list[str]" = []
    for path, records in post_statuses.items():
        for rec in records:
            status = rec.get("status")
            if rec.get("failed") or not isinstance(status, int) or not (200 <= status < 300):
                bad.append(f"{path} -> {status!r}{' (failed)' if rec.get('failed') else ''}")
    return bad


def _is_leftover_config_name(name: "Optional[str]") -> bool:
    """True for a name this row's own generator could have produced, or the
    older fixed literal name a previous version of this function used
    (``__kc_web_commission_test__`` -- still checked so a slot left behind by
    that older code is also cleaned up). A leftover of either shape means a
    previous run of this row did not finish its own cleanup."""
    if not name:
        return False
    return name == "__kc_web_commission_test__" or str(name).startswith("kc_test_")


def _apply_kiln_config(row: Row, host: str, screenshot_dir: str, cookie: str,
                        target_id: object, shot_suffix: str) -> "tuple[bool, str, bool]":
    """Selects `target_id` in `#kilnConfigSelect` and clicks `#kcApplyBtn` --
    the page's own select/apply control; there is no separate "select"
    endpoint, POST /api/kiln_configs/apply both selects AND makes a config
    active in one step (kiln_configs_page.html). The apply job runs async
    (202, then /api/kiln_configs/apply_status) exactly like the page's own
    pollApplyStatus(), so this polls that route (bounded, same ~60s budget)
    until the job leaves 'running', then confirms via GET
    `row.verify_endpoint` that `active_id` really is `target_id` -- never
    trusting the POST status alone, the same rule every other row here
    follows.

    Returns (ok, detail, board_quiet). `board_quiet` is False when this
    function cannot prove the board is settled again: the swap is still
    reporting `running` when the poll budget runs out, or apply_status
    reported `diverged` (kiln_cfg_swap.c's alarmed exit -- config left
    pending for retry; heaters are actually disabled only on the
    ceiling-latch branch of that exit, not the four rollback-failure
    branches -- see docs/audits/kiln_config_self_apply_diverged_2026-09-22.md
    sec 4, `reason` below is the honest source for which one occurred). A
    caller MUST NOT issue any further write (a delete, another apply) while
    `board_quiet` is False: a
    kiln-config swap is a 60+ round-trip two-processor transaction, and
    kiln_cfg_swap.c's own H6 generation check exists precisely because a
    concurrent store write during one is a real hazard."""
    proc = _run_cdp(row, host, screenshot_dir, cookie,
                     selector="kcApplyBtn", selector_kind="id",
                     fills=(("#kilnConfigSelect", str(target_id)),),
                     expect_post="/api/kiln_configs/apply",
                     accept_dialogs=True, shot_suffix=shot_suffix)
    if proc.returncode != 0:
        return False, (f"CDP driver (apply id={target_id}) exited {proc.returncode}: "
                        f"{proc.stderr.strip()[-500:]}"), True
    post = _cdp_post_status(proc)
    if post is not None and (post.get("failed") or post.get("status") not in (200, 202, None)):
        # The POST itself was refused (409 interlock, 428 hardware-differs,
        # 404, ...) -- nothing was dispatched, so the board is untouched.
        return False, (f"apply POST {post.get('url')} -> {post.get('status')} "
                        f"(failed={post.get('failed')}) for id={target_id}"), True

    last_state = None
    last_diverged = False
    last_reason = ""
    for _ in range(60):  # ~60s at 1s cadence, same budget as the page's own pollApplyStatus()
        st_status, st_body = _get_json_with_cookie(host, "/api/kiln_configs/apply_status", cookie)
        if st_status == 200 and isinstance(st_body, dict):
            last_state = st_body.get("state")
            last_diverged = bool(st_body.get("diverged"))
            last_reason = str(st_body.get("reason") or "")
            if last_state != "running":
                break
        else:
            last_state = None
        time.sleep(1.0)

    # DIVERGED first: it is the single most consequential outcome this row
    # can produce and the operator has to be told about it before anything
    # else. kiln_cfg_swap.c sets it on the paths that leave the two halves
    # (or the Pico ceiling/arming cross-check) unreconciled -- config left
    # pending for retry. 2026-09-22 fix: this does NOT always mean heaters
    # were disabled -- only the ceiling-latch branch does that, not the
    # four rollback-failure branches (docs/audits/
    # kiln_config_self_apply_diverged_2026-09-22.md sec 4) -- so this no
    # longer claims "heaters disabled" generically; `last_reason` (from the
    # board) is the honest source for what actually happened.
    if last_diverged:
        return False, (f"apply id={target_id} left the board DIVERGED "
                        f"(alarmed, config left pending for retry; see reason): "
                        f"{last_reason or 'no reason reported by apply_status'}"), False
    if last_state == "running":
        return False, (f"apply id={target_id} still reports state=running after the ~60 s "
                        f"apply_status budget -- the two-processor kiln-config swap may still "
                        f"be in flight"), False
    if last_state == "done_failed":
        return False, (f"apply id={target_id} reported done_failed (apply_status): "
                        f"{last_reason or 'no reason reported by apply_status'}"), True

    final_status, final_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if final_status != 200 or not isinstance(final_body, dict):
        return False, (f"post-apply read-back GET {row.verify_endpoint} -> "
                        f"{final_status}: {json.dumps(final_body)[:300]}"), last_state is not None
    if str(final_body.get("active_id")) != str(target_id):
        return False, (f"apply id={target_id} did not take -- active_id now "
                        f"{final_body.get('active_id')!r}"), last_state is not None
    return True, f"applied id={target_id}, confirmed active_id={target_id}", True


def _delete_kiln_config(row: Row, host: str, screenshot_dir: str, cookie: str,
                         target_id: object, shot_suffix: str) -> "tuple[bool, str]":
    """Selects `target_id` in `#kilnConfigSelect` and clicks `#kcDeleteBtn`,
    checking the delete POST's own status (closing the "delete-POST-status
    gap" the 2026-09-21 W42 record noted -- the old code checked CDP exit
    code but never the POST's own status/failed fields)."""
    proc = _run_cdp(row, host, screenshot_dir, cookie,
                     selector="kcDeleteBtn", selector_kind="id",
                     fills=(("#kilnConfigSelect", str(target_id)),),
                     expect_post="/api/kiln_configs/delete",
                     accept_dialogs=True, shot_suffix=shot_suffix)
    if proc.returncode != 0:
        return False, (f"CDP driver (delete id={target_id}) exited {proc.returncode}: "
                        f"{proc.stderr.strip()[-500:]}")
    post = _cdp_post_status(proc)
    if post is not None and (post.get("failed") or post.get("status") not in (200, None)):
        return False, (f"delete POST {post.get('url')} -> {post.get('status')} "
                        f"(failed={post.get('failed')}) for id={target_id}")
    return True, f"deleted id={target_id}"


def _run_kiln_config_create_delete(row: Row, host: str, screenshot_dir: str, cookie: str) -> "tuple[bool, str]":
    """W42's real shape, redesigned 2026-09-21/22 after the live FAIL
    (docs/COMMISSIONING_TEST_MATRIX.md's W42 record): firmware's
    kiln_cfg_store_save_current_ex() deliberately marks a freshly saved slot
    ACTIVE (kiln_cfg_store.c), and kiln_cfg_store_delete()'s H5 backstop
    correctly refuses to delete the currently-active config 400 ("... is the
    kiln config this controller is running; select another kiln config
    first ..."). The old runner tried to delete the just-created (and thus
    active) slot directly and got that 400 -- a runner defect, not a
    firmware one. This version:

      1. reads GET /api/kiln_configs first and records the original active
         id and the full list; a stale `kc_test_*`/
         `__kc_web_commission_test__` leftover from a previous incomplete
         run is treated as a pre-existing fixture problem and cleaned up
         (selecting away from it first, if it happens to be active, before
         deleting it) BEFORE the real run, and reported as having done so;
      2. creates `kc_test_<epoch>` via Save as new, as before;
      3. re-selects the ORIGINAL active id via the page's own select+Apply
         control (#kilnConfigSelect + #kcApplyBtn -- there is no separate
         "select" endpoint) -- this is a restore, not a behavior change: the
         saved blob is byte-identical to what was already running;
      4. deletes the new (now inactive) config via Delete selected;
      5. reads back: the new config is gone and active_id is back to the
         original.

    On any failure after step 2 (create), this still attempts steps 3 and 4
    as a best-effort restore and reports exactly what, if anything, remained
    on the board -- never silently reports FAIL without saying so.
    """
    pre_status, pre_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if pre_status != 200 or not isinstance(pre_body, dict):
        return False, (f"{row.row_id} FAIL: pre-read GET {row.verify_endpoint} -> "
                        f"{pre_status}: {json.dumps(pre_body)[:300]}")
    configs = pre_body.get("configs") or []
    original_active_id = pre_body.get("active_id")
    notes: "list[str]" = []

    leftovers = [c for c in configs if isinstance(c, dict) and _is_leftover_config_name(c.get("name"))]
    deleted_leftover_id = None
    if leftovers:
        stale = leftovers[0]
        stale_id = stale.get("id")
        stale_name = stale.get("name")
        if str(stale_id) == str(original_active_id):
            # The fallback becomes the board's LIVE kiln config (apply
            # rewrites relay wiring, thermocouple assignment, PID gains and
            # guard thresholds -- kiln_configs_page.html's own confirm()
            # text). So it must never be another `kc_test_*` leftover: those
            # are this row's own throwaways, of unknown provenance, and
            # making one live would be exactly the "changed live gains"
            # outcome this row must never produce. Only a non-leftover,
            # operator-created config qualifies.
            others = [c for c in configs
                      if isinstance(c, dict) and str(c.get("id")) != str(stale_id)
                      and not _is_leftover_config_name(c.get("name"))]
            if not others:
                return False, (f"{row.row_id} FAIL: pre-existing leftover config {stale_name!r} "
                                f"(id={stale_id}) is active and no other config exists to select "
                                f"before deleting it that is not itself a test leftover -- "
                                f"refusing, needs owner review")
            fallback_id = others[0].get("id")
            ok, detail, _quiet = _apply_kiln_config(row, host, screenshot_dir, cookie, fallback_id,
                                                     "_cleanup_apply")
            if not ok:
                return False, (f"{row.row_id} FAIL: cleanup of pre-existing leftover "
                                f"{stale_name!r} (id={stale_id}) could not select it away first: "
                                f"{detail}")
            original_active_id = fallback_id
            notes.append(f"leftover {stale_name!r} (id={stale_id}) was active with no other "
                         f"record of an original -- selected id={fallback_id} instead before "
                         f"deleting the leftover")
        ok, detail = _delete_kiln_config(row, host, screenshot_dir, cookie, stale_id, "_cleanup_delete")
        if not ok:
            return False, (f"{row.row_id} FAIL: cleanup of pre-existing leftover "
                            f"{stale_name!r} (id={stale_id}) failed: {detail}")
        notes.append(f"deleted pre-existing leftover {stale_name!r} (id={stale_id}) before the real run")
        deleted_leftover_id = stale_id

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
    # Advisory carried from an earlier review: the leftover sweep above only
    # matches by PREFIX (_is_leftover_config_name()), so a same-second re-run
    # after a partial failure -- or any other reason `unique_name` is already
    # taken -- would otherwise only be caught by the board's own "a saved
    # kiln config already has that name" 400 well after the CDP click. Fail
    # here instead, before touching the board at all, naming the collision.
    # Excludes the leftover just deleted above: `configs` was read BEFORE
    # that deletion, so on a same-second re-run the just-deleted leftover's
    # own `kc_test_<epoch>` name can equal the freshly generated one and
    # would otherwise cause a spurious refusal here for a name that no
    # longer exists on the board.
    if unique_name in [c.get("name") for c in configs
                       if isinstance(c, dict)
                       and not (deleted_leftover_id is not None
                                and str(c.get("id")) == str(deleted_leftover_id))]:
        return False, (f"{row.row_id} FAIL: generated name {unique_name!r} is already listed in "
                        f"{row.verify_endpoint} -- refusing to create before touching the board")
    create_fills = (("#kcSaveNewName", unique_name),)

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
    match = [c for c in (mid_body.get("configs") or [])
             if isinstance(c, dict) and c.get("name") == unique_name]
    if not match:
        return False, (f"{row.row_id} FAIL: no config named {unique_name!r} found in "
                        f"{row.verify_endpoint} after create -- write did not land")
    new_id = match[0].get("id")

    # Firmware marks the just-created slot ACTIVE, and refuses to delete the
    # active config (H5 backstop) -- select the ORIGINAL active id back
    # before the delete below. The saved blob is byte-identical to the
    # config that was already running, so this is a restore, not a change.
    restore_ok, restore_detail, board_quiet = _apply_kiln_config(
        row, host, screenshot_dir, cookie, original_active_id, "_restore_apply")
    if not restore_ok:
        if not board_quiet:
            # A swap that is still in flight, or one that ended DIVERGED, is
            # not a board to issue another write at: kiln_cfg_swap.c's H6
            # generation check treats a concurrent store write during a swap
            # as a hazard, and a diverged board is already alarmed (heaters
            # disabled only on the ceiling-latch branch -- see the DIVERGED
            # detail string above for what actually happened). Stop here and
            # say exactly what is left.
            return False, (f"{row.row_id} FAIL: re-select of original active "
                            f"id={original_active_id} after create failed: {restore_detail}; "
                            f"NO further write attempted (board not confirmed settled). "
                            f"LEFT ON BOARD: throwaway config {unique_name!r} (id={new_id}), and "
                            f"the active kiln config may NOT be the original "
                            f"{original_active_id!r} -- check GET /api/kiln_configs and "
                            f"GET /api/kiln_configs/apply_status by hand before any further use")
        # Best-effort: the apply was refused with nothing dispatched, so the
        # board is settled -- still try to delete the throwaway slot so at
        # least one half of the cleanup lands, and say exactly what is left.
        del_ok, del_detail = _delete_kiln_config(row, host, screenshot_dir, cookie, new_id,
                                                  "_delete_after_restore_fail")
        remaining = "nothing (throwaway config was still deleted)" if del_ok else \
            f"throwaway config {unique_name!r} (id={new_id}) -- delete ALSO failed: {del_detail}"
        return False, (f"{row.row_id} FAIL: re-select of original active id={original_active_id} "
                        f"after create failed: {restore_detail}; best-effort delete attempted, "
                        f"LEFT ON BOARD: {remaining}")

    delete_ok, delete_detail = _delete_kiln_config(row, host, screenshot_dir, cookie, new_id, "_delete")
    if not delete_ok:
        return False, (f"{row.row_id} FAIL: delete of throwaway config {unique_name!r} "
                        f"(id={new_id}) failed: {delete_detail} -- LEFT ON BOARD, delete by hand")

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
    if str(final_body.get("active_id")) != str(original_active_id):
        return False, (f"{row.row_id} FAIL: active_id is {final_body.get('active_id')!r} after "
                        f"cleanup, expected original {original_active_id!r} -- board state changed")

    prefix = (("; ".join(notes)) + " | ") if notes else ""
    return True, (
        f"{row.row_id} PASS: {prefix}created {unique_name!r} (id={new_id}) via Save as new "
        f"(POST /api/kiln_configs/save), confirmed via GET {row.verify_endpoint}, re-selected "
        f"original active id={original_active_id} via Apply (POST /api/kiln_configs/apply), "
        f"deleted the throwaway via Delete selected (POST /api/kiln_configs/delete), confirmed "
        f"removed and active_id restored | expected: {row.expected_outcome}"
    )


# ---------------------------------------------------------------------------
# W8/W9/W10 -- profile segment-builder create, multi-select delete, and
# favorite toggle. Shared helpers first, then one function per row.
# ---------------------------------------------------------------------------

_PROFILE_NAME_MAX_LEN = 15  # profiles_page.html: <input id="pname" maxlength="15">


def _is_scratch_profile_name(name: "Optional[str]") -> bool:
    """True for a name one of this module's own generators
    (_new_scratch_profile_name()) could have produced. Every scratch-profile
    delete in W8/W9/W10 is gated on this -- never touch a profile whose name
    doesn't match, builtin or otherwise."""
    return bool(name) and str(name).startswith("wc_test_")


def _new_scratch_profile_name(idx: int = 0) -> str:
    # #pname's maxlength="15" leaves only 7 characters after the 8-char
    # "wc_test_" prefix: 6 digits of epoch seconds (mod 1e6, wraps roughly
    # every 11.6 days) plus one trailing digit distinguishing multiple
    # profiles a single row run creates within the same second (W9 creates
    # two). A wrap-around collision would be caught by this row's own
    # up-front "leftover scratch profile already exists" refusal, same as
    # W42's kc_test_* naming accepts an equivalent tradeoff for a tighter
    # limit.
    name = f"wc_test_{int(time.time()) % 1_000_000:06d}{idx % 10}"
    assert len(name) <= _PROFILE_NAME_MAX_LEN and name.startswith("wc_test_")
    return name


def _profile_names(body) -> "list[str]":
    """GET /api/profiles (profiles_page.html's refreshAll()) returns a bare
    JSON array of {id, name, builtin, ...} objects, not a dict -- unlike
    every other verify_endpoint in this module. Centralized here so a
    caller never needs its own isinstance/.get() dance for this one shape."""
    if not isinstance(body, list):
        return []
    return [c.get("name") for c in body if isinstance(c, dict)]


def _profile_id_by_name(body, name: str) -> object:
    if isinstance(body, list):
        for c in body:
            if isinstance(c, dict) and c.get("name") == name:
                return c.get("id")
    return None


def _unconfirmed_cleanup_note(endpoint: str, status, name: str) -> str:
    """W8/W9/W10 share this sentence: a post-failure cleanup read-back did
    not come back 200, so whether `name` was actually created (and thus
    still sits on the board) cannot be determined either way. Centralized so
    the wording -- in particular "cannot confirm board state", "LEFT ON
    BOARD" and "check by hand", which existing tests assert on -- stays
    identical across all three call sites."""
    return (f"cannot confirm board state -- post-failure read-back GET "
            f"{endpoint} -> {status}; {name!r} may be LEFT ON BOARD "
            f"unconfirmed, check by hand")


def _create_scratch_profile(row: Row, host: str, screenshot_dir: str, cookie: str,
                             name: str, shot_suffix: str) -> "tuple[bool, str]":
    """Drives profiles_page.html's segment builder to create one scratch
    profile named `name` via POST /api/profile: fill #pname, tick the FIRST
    zone checkbox (`#pzone label:nth-child(1) .pzone-cb` -- the checkboxes
    are plain <input class="pzone-cb"> with no id or aria-label, per
    selectedZoneMask()'s zero-mask "Select at least one zone" refusal,
    which is why a zone tick is mandatory here even though the original
    task description did not call it out), then fill one ramp/dwell
    segment's default fields (.s-target/.s-ramp/.s-dwell -- segmentRow({})
    defaults kind=0, and resetEditor() -- which loadZones().then(...) or a
    #newBtn click both run -- already clicks #addSegBtn once on its own, so
    exactly one such segment row already exists; no separate #addSegBtn
    step is needed here). Passes `expect_post=""` explicitly so the
    driver's post-steps settle() never waits on a caller row's own
    unrelated `expect_post` default (W9's row default is
    "/api/profile/delete", which would otherwise make settle() hang here
    waiting for a delete that hasn't happened yet) -- the explicit
    "wait-for-post" step already covers the actual save.

    Returns (ok, detail); does not read back GET /api/profiles or clean up
    -- callers do both themselves, since W8/W9/W10 verify and restore
    differently."""
    steps = [
        {"action": "fill", "kind": "css", "selector": "#pname", "value": name},
        {"action": "click", "kind": "css", "selector": "#pzone label:nth-child(1) .pzone-cb"},
        {"action": "fill", "kind": "css", "selector": "#segments .seg:nth-child(1) .s-target", "value": "150"},
        {"action": "fill", "kind": "css", "selector": "#segments .seg:nth-child(1) .s-ramp", "value": "60"},
        {"action": "fill", "kind": "css", "selector": "#segments .seg:nth-child(1) .s-dwell", "value": "5"},
        {"action": "click", "kind": "id", "selector": "saveBtn"},
        {"action": "wait-for-post", "path": "/api/profile"},
    ]
    proc = _run_cdp(row, host, screenshot_dir, cookie, steps=steps, expect_post="",
                     shot_suffix=shot_suffix)
    if proc.returncode != 0:
        return False, f"CDP driver (create {name!r}) exited {proc.returncode}: {proc.stderr.strip()[-500:]}"
    post = _cdp_post_status(proc)
    if post is not None and (post.get("failed") or post.get("status") not in (200, None)):
        return False, (f"create POST {post.get('url')} -> {post.get('status')} "
                        f"(failed={post.get('failed')}) for {name!r}")
    return True, f"created {name!r}"


def _delete_profile_by_name(row: Row, host: str, screenshot_dir: str, cookie: str,
                             name: str, shot_suffix: str) -> "tuple[bool, str]":
    """Clicks the named profile's own row Delete icon (aria-label
    'Delete "<name>"' -- profiles_page.html's per-row iconBtn('kgIconDelete',
    ...)), answering the resulting confirm() with OK. Refuses -- without
    touching the board -- if `name` does not pass _is_scratch_profile_name(),
    the one gate every cleanup path in W8/W9/W10 shares."""
    if not _is_scratch_profile_name(name):
        return False, f"refusing to delete non-scratch profile name {name!r}"
    proc = _run_cdp(row, host, screenshot_dir, cookie,
                     selector=f'Delete "{name}"', selector_kind="aria-label",
                     expect_post="/api/profile/delete", accept_dialogs=True,
                     shot_suffix=shot_suffix)
    if proc.returncode != 0:
        return False, f"CDP driver (delete {name!r}) exited {proc.returncode}: {proc.stderr.strip()[-500:]}"
    post = _cdp_post_status(proc)
    if post is not None and (post.get("failed") or post.get("status") not in (200, None)):
        return False, (f"delete POST {post.get('url')} -> {post.get('status')} "
                        f"(failed={post.get('failed')}) for {name!r}")
    return True, f"deleted {name!r}"


def _run_profile_segment_create_delete(row: Row, host: str, screenshot_dir: str, cookie: str) -> "tuple[bool, str]":
    """W8: create one scratch profile through the segment builder, confirm
    it via GET /api/profiles, delete it through its own row's Delete icon,
    confirm it is gone. Refuses up front if a `wc_test_*` profile already
    exists (a previous run's incomplete cleanup). Every failure path from
    the create CDP call onward still attempts the delete and says so --
    LEFT ON BOARD when it cannot confirm the board is clean afterward."""
    pre_status, pre_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if pre_status != 200 or not isinstance(pre_body, list):
        return False, (f"{row.row_id} FAIL: pre-read GET {row.verify_endpoint} -> "
                        f"{pre_status}: {json.dumps(pre_body)[:300]}")
    existing_scratch = [n for n in _profile_names(pre_body) if _is_scratch_profile_name(n)]
    if existing_scratch:
        return False, (f"{row.row_id} FAIL: a leftover scratch profile {existing_scratch[0]!r} "
                        f"already exists -- refusing to start; delete it by hand and re-run")

    name = _new_scratch_profile_name()

    def _cleanup_after_create_attempt() -> str:
        chk_status, chk_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
        if chk_status != 200 or not isinstance(chk_body, list):
            return _unconfirmed_cleanup_note(row.verify_endpoint, chk_status, name)
        if name not in _profile_names(chk_body):
            return "nothing was created"
        del_ok, del_detail = _delete_profile_by_name(row, host, screenshot_dir, cookie, name,
                                                      "_cleanup_delete")
        return f"cleaned up {name!r} ({del_detail})" if del_ok else \
            f"{name!r} was created but cleanup delete FAILED ({del_detail}) -- LEFT ON BOARD"

    ok, detail = _create_scratch_profile(row, host, screenshot_dir, cookie, name, "_create")
    if not ok:
        return False, f"{row.row_id} FAIL: create failed: {detail} -- {_cleanup_after_create_attempt()}"

    mid_status, mid_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if mid_status != 200 or not isinstance(mid_body, list):
        return False, (f"{row.row_id} FAIL: post-create read-back GET {row.verify_endpoint} -> "
                        f"{mid_status}: {json.dumps(mid_body)[:300]} -- if {name!r} was created it "
                        f"is LEFT ON BOARD, check by hand")
    if name not in _profile_names(mid_body):
        return False, (f"{row.row_id} FAIL: no profile named {name!r} found in "
                        f"{row.verify_endpoint} after create -- write did not land")

    delete_ok, delete_detail = _delete_profile_by_name(row, host, screenshot_dir, cookie, name, "_delete")
    if not delete_ok:
        return False, (f"{row.row_id} FAIL: created {name!r}, confirmed via GET "
                        f"{row.verify_endpoint}, but delete failed: {delete_detail} -- "
                        f"LEFT ON BOARD, delete by hand")

    final_status, final_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if final_status != 200 or not isinstance(final_body, list):
        return False, (f"{row.row_id} FAIL: post-delete read-back GET {row.verify_endpoint} -> "
                        f"{final_status}: {json.dumps(final_body)[:300]} -- delete POST landed, "
                        f"but read-back could not confirm it")
    if name in _profile_names(final_body):
        return False, (f"{row.row_id} FAIL: profile {name!r} still present after delete -- "
                        f"LEFT ON BOARD, delete by hand")

    return True, (
        f"{row.row_id} PASS: created {name!r} via the segment builder (POST /api/profile), "
        f"confirmed via GET {row.verify_endpoint}, deleted via its own Delete icon "
        f"(POST /api/profile/delete), confirmed removed | expected: {row.expected_outcome}"
    )


def _run_profile_multi_delete(row: Row, host: str, screenshot_dir: str, cookie: str) -> "tuple[bool, str]":
    """W9: create two scratch profiles, enter delete mode (#modeDeleteBtn),
    tick each one's own per-row checkbox (aria-label 'Select "<name>" for
    delete' -- profiles_page.html's selectionCheckbox()), click
    #bulkActionBtn (bulkDelete(): one kcConfirm() modal then one POST
    /api/profile/delete per ticked id, issued together via Promise.all --
    which is why this row's own `expect_post` default is left in place on
    the bulk-delete _run_cdp() call below: the explicit "wait-for-post"
    step consumes the FIRST of those two POSTs in order, and the driver's
    own post-steps settle() -- given no selectorKind, so it takes the
    steps-only branch -- then waits on `args.expectPost` (this row's
    default, "/api/profile/delete") for the SECOND, so both complete before
    Chrome tears down rather than racing screenshot/exit against the
    slower one).

    Reads the full profile list FIRST and refuses to start if a leftover
    `wc_test_*` scratch profile already exists. Only ever ticks/deletes
    names this row generated itself -- never a builtin or a real saved
    profile."""
    pre_status, pre_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if pre_status != 200 or not isinstance(pre_body, list):
        return False, (f"{row.row_id} FAIL: pre-read GET {row.verify_endpoint} -> "
                        f"{pre_status}: {json.dumps(pre_body)[:300]}")
    existing_scratch = [n for n in _profile_names(pre_body) if _is_scratch_profile_name(n)]
    if existing_scratch:
        return False, (f"{row.row_id} FAIL: leftover scratch profile(s) {existing_scratch} already "
                        f"exist -- refusing to start; delete by hand and re-run")

    names = [_new_scratch_profile_name(0), _new_scratch_profile_name(1)]
    created: "list[str]" = []
    for i, name in enumerate(names):
        ok, detail = _create_scratch_profile(row, host, screenshot_dir, cookie, name, f"_create{i}")
        if not ok:
            # `ok=False` reports the driver's own verdict, not the board's:
            # a CDP exit-code/stderr mismatch can still follow a POST that
            # actually landed. Re-read GET before deciding what to clean up
            # -- W8's _cleanup_after_create_attempt() and W10's inline check
            # both do this for their own single scratch profile; without it
            # here, a `name` that landed despite a reported failure was
            # never added to `created` and so was never named or deleted,
            # silently LEFT ON BOARD with no trace in the failure message.
            cleanup_targets = list(created)
            chk_status, chk_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
            # A non-200 here (in particular a 401 -- `_get_json_with_cookie()`
            # maps a rejected session cookie to `(None, {"error": ...})` per
            # its own `no_relogin=True` contract) means the probe could not
            # SEE the board's state, not that `name` was never created. Treat
            # that as unconfirmed, distinct from a confirmed-absent 200 whose
            # body simply lacks `name`.
            probe_confirmed = chk_status == 200 and isinstance(chk_body, list)
            if probe_confirmed and name in _profile_names(chk_body):
                cleanup_targets.append(name)
            cleanup_results = [_delete_profile_by_name(row, host, screenshot_dir, cookie, n,
                                                         f"_cleanup{j}")
                                for j, n in enumerate(cleanup_targets)]
            left = [n for n, (cok, _cd) in zip(cleanup_targets, cleanup_results) if not cok]
            if left:
                extra = f" -- LEFT ON BOARD: {left}"
            elif cleanup_targets:
                extra = f" -- cleaned up {cleanup_targets}"
            elif probe_confirmed:
                extra = " -- nothing was created"
            else:
                extra = ""
            # ADDITIVE, not an else-branch: when the create that failed is the
            # SECOND one, `cleanup_targets` already holds the first (earlier,
            # confirmed) profile, so an else-chain would report "cleaned up
            # [first]" / "LEFT ON BOARD: [first]" and silently drop the fact
            # that `name` itself could not be checked at all. Every
            # unconfirmed probe gets its own sentence regardless of what
            # happened to the earlier profiles.
            if not probe_confirmed:
                extra += " -- " + _unconfirmed_cleanup_note(row.verify_endpoint, chk_status, name)
            return False, f"{row.row_id} FAIL: create of {name!r} failed: {detail}{extra}"
        created.append(name)

    mid_status, mid_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if mid_status != 200 or not isinstance(mid_body, list):
        return False, (f"{row.row_id} FAIL: post-create read-back GET {row.verify_endpoint} -> "
                        f"{mid_status}: {json.dumps(mid_body)[:300]} -- {created} may be LEFT ON "
                        f"BOARD, check by hand")
    mid_names = _profile_names(mid_body)
    missing = [n for n in names if n not in mid_names]
    if missing:
        return False, (f"{row.row_id} FAIL: profile(s) {missing} not found after create -- write "
                        f"did not land; {created} may be LEFT ON BOARD, check by hand")

    # Defensive: every name here must be one this row generated. Refuses
    # rather than ticking anything that fails this, even though
    # _new_scratch_profile_name() cannot itself produce a non-matching name.
    for n in names:
        if not _is_scratch_profile_name(n):
            return False, (f"{row.row_id} FAIL: internal error -- generated name {n!r} is not a "
                            f"scratch name; refusing to select it for delete")

    steps = [{"action": "click", "kind": "id", "selector": "modeDeleteBtn"}]
    for name in names:
        steps.append({"action": "click", "kind": "aria-label",
                      "selector": f'Select "{name}" for delete'})
    steps.append({"action": "click", "kind": "id", "selector": "bulkActionBtn"})
    steps.append({"action": "wait-for-post", "path": "/api/profile/delete"})
    proc = _run_cdp(row, host, screenshot_dir, cookie, steps=steps,
                     accept_dialogs=True, shot_suffix="_bulkdelete")
    if proc.returncode != 0:
        return False, (f"{row.row_id} FAIL: CDP driver (bulk delete) exited {proc.returncode}: "
                        f"{proc.stderr.strip()[-500:]} -- {names} may be LEFT ON BOARD, check and "
                        f"delete by hand")

    final_status, final_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if final_status != 200 or not isinstance(final_body, list):
        return False, (f"{row.row_id} FAIL: post-delete read-back GET {row.verify_endpoint} -> "
                        f"{final_status}: {json.dumps(final_body)[:300]} -- delete POST(s) may have "
                        f"landed, but read-back could not confirm it; {names} may be LEFT ON BOARD")
    still_present = [n for n in names if n in _profile_names(final_body)]
    if still_present:
        return False, (f"{row.row_id} FAIL: profile(s) {still_present} still present after bulk "
                        f"delete -- LEFT ON BOARD, delete by hand")

    return True, (
        f"{row.row_id} PASS: created {names} via the segment builder, entered delete mode "
        f"(#modeDeleteBtn), ticked both via their per-row aria-label checkboxes, clicked "
        f"#bulkActionBtn (POST /api/profile/delete x{len(names)}), confirmed both removed via GET "
        f"{row.verify_endpoint} | expected: {row.expected_outcome}"
    )


def _run_profile_favorite_toggle(row: Row, host: str, screenshot_dir: str, cookie: str) -> "tuple[bool, str]":
    """W10: create one scratch profile (never a user's real profile, so
    nothing else's favorite state is ever touched), toggle its favorite
    star on (aria-label 'Add "<name>" to favorites' -- profiles_page.html's
    favToggleBtn()), confirm via GET /api/profiles/favorites (`{"ids": [...]}`
    -- loadFavorites()'s own shape), toggle it back off (aria-label
    'Remove "<name>" from favorites', confirmed verbatim against
    favToggleBtn()'s string build), confirm off, then delete the scratch
    profile via its own row's Delete icon."""
    pre_status, pre_body = _get_json_with_cookie(host, "/api/profiles", cookie)
    if pre_status != 200 or not isinstance(pre_body, list):
        return False, (f"{row.row_id} FAIL: pre-read GET /api/profiles -> "
                        f"{pre_status}: {json.dumps(pre_body)[:300]}")
    existing_scratch = [n for n in _profile_names(pre_body) if _is_scratch_profile_name(n)]
    if existing_scratch:
        return False, (f"{row.row_id} FAIL: leftover scratch profile {existing_scratch[0]!r} "
                        f"already exists -- refusing to start; delete by hand and re-run")

    name = _new_scratch_profile_name()

    def _cleanup_delete() -> str:
        del_ok, del_detail = _delete_profile_by_name(row, host, screenshot_dir, cookie, name, "_delete")
        return f"deleted {name!r}" if del_ok else f"delete FAILED: {del_detail} -- LEFT ON BOARD, delete by hand"

    ok, detail = _create_scratch_profile(row, host, screenshot_dir, cookie, name, "_create")
    if not ok:
        chk_status, chk_body = _get_json_with_cookie(host, "/api/profiles", cookie)
        # See _run_profile_multi_delete()'s matching comment: a non-200 probe
        # (a rejected session cookie in particular) means the probe could not
        # see the board's state, not that `name` was never created -- do not
        # conflate that with a confirmed-absent 200.
        if chk_status == 200 and isinstance(chk_body, list) and name in _profile_names(chk_body):
            del_ok, del_detail = _delete_profile_by_name(row, host, screenshot_dir, cookie, name,
                                                          "_cleanup_delete")
            extra = f" -- cleaned up {name!r}" if del_ok else f" -- {name!r} LEFT ON BOARD ({del_detail})"
        elif chk_status == 200 and isinstance(chk_body, list):
            extra = " -- nothing was created"
        else:
            extra = " -- " + _unconfirmed_cleanup_note("/api/profiles", chk_status, name)
        return False, f"{row.row_id} FAIL: create failed: {detail}{extra}"

    mid_status, mid_body = _get_json_with_cookie(host, "/api/profiles", cookie)
    if mid_status != 200 or not isinstance(mid_body, list) or name not in _profile_names(mid_body):
        return False, (f"{row.row_id} FAIL: profile {name!r} not confirmed present after create "
                        f"(GET /api/profiles -> {mid_status}) -- write did not land")
    pid = _profile_id_by_name(mid_body, name)

    on_proc = _run_cdp(row, host, screenshot_dir, cookie,
                        selector=f'Add "{name}" to favorites', selector_kind="aria-label",
                        shot_suffix="_favon")
    if on_proc.returncode != 0:
        return False, (f"{row.row_id} FAIL: CDP driver (favorite on) exited {on_proc.returncode}: "
                        f"{on_proc.stderr.strip()[-500:]} -- {_cleanup_delete()}")
    on_post = _cdp_post_status(on_proc)
    if on_post is not None and (on_post.get("failed") or on_post.get("status") not in (200, None)):
        return False, (f"{row.row_id} FAIL: favorite-on POST {on_post.get('url')} -> "
                        f"{on_post.get('status')} (failed={on_post.get('failed')}) -- {_cleanup_delete()}")

    fav_status, fav_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if fav_status != 200 or not isinstance(fav_body, dict):
        return False, (f"{row.row_id} FAIL: post-favorite-on read-back GET {row.verify_endpoint} -> "
                        f"{fav_status}: {json.dumps(fav_body)[:300]} -- {_cleanup_delete()}")
    if pid not in (fav_body.get("ids") or []):
        return False, (f"{row.row_id} FAIL: {name!r} (id={pid}) not in favorites after toggle-on "
                        f"(ids={fav_body.get('ids')}) -- write likely did not land -- {_cleanup_delete()}")

    off_proc = _run_cdp(row, host, screenshot_dir, cookie,
                         selector=f'Remove "{name}" from favorites', selector_kind="aria-label",
                         shot_suffix="_favoff")
    if off_proc.returncode != 0:
        return False, (f"{row.row_id} FAIL: CDP driver (favorite off) exited {off_proc.returncode}: "
                        f"{off_proc.stderr.strip()[-500:]} -- favorite may be LEFT ON, and "
                        f"{_cleanup_delete()}")
    off_post = _cdp_post_status(off_proc)
    if off_post is not None and (off_post.get("failed") or off_post.get("status") not in (200, None)):
        return False, (f"{row.row_id} FAIL: favorite-off POST {off_post.get('url')} -> "
                        f"{off_post.get('status')} (failed={off_post.get('failed')}) -- favorite may "
                        f"be LEFT ON, and {_cleanup_delete()}")

    final_status, final_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if final_status != 200 or not isinstance(final_body, dict):
        return False, (f"{row.row_id} FAIL: post-favorite-off read-back GET {row.verify_endpoint} -> "
                        f"{final_status}: {json.dumps(final_body)[:300]} -- {_cleanup_delete()}")
    if pid in (final_body.get("ids") or []):
        return False, (f"{row.row_id} FAIL: {name!r} (id={pid}) still in favorites after "
                        f"toggle-off -- {_cleanup_delete()}")

    delete_ok, delete_detail = _delete_profile_by_name(row, host, screenshot_dir, cookie, name, "_delete")
    if not delete_ok:
        return False, (f"{row.row_id} FAIL: favorite toggle verified on then off, but delete of "
                        f"{name!r} failed: {delete_detail} -- LEFT ON BOARD, delete by hand")

    end_status, end_body = _get_json_with_cookie(host, "/api/profiles", cookie)
    if end_status != 200 or not isinstance(end_body, list):
        return False, (f"{row.row_id} FAIL: post-delete read-back GET /api/profiles -> "
                        f"{end_status}: {json.dumps(end_body)[:300]} -- delete POST landed, but "
                        f"read-back could not confirm it")
    if name in _profile_names(end_body):
        return False, f"{row.row_id} FAIL: profile {name!r} still present after delete -- LEFT ON BOARD"

    return True, (
        f"{row.row_id} PASS: created {name!r}, toggled favorite on (POST /api/profile/favorite), "
        f"confirmed via GET {row.verify_endpoint}, toggled off, confirmed removed, deleted the "
        f"scratch profile, confirmed gone | expected: {row.expected_outcome}"
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
    both.

    THIRD WRITE, now also restored: #step1Save's own handler calls
    postStepState(1, 'done') -> POST /api/setup/progress after both field
    POSTs resolve (setup_wizard_page.html), so running this row marks
    wizard step 1 "done" in the board's persisted commissioning progress.
    GET /api/setup/progress (setup_progress_http.c's
    api_setup_progress_get_handler) reads back every step's
    {state, ts, note} keyed by string index, and the same route's POST
    handler (api_setup_progress_post_handler, form body
    "step=<n>&state=pending|done|skipped[&note=...]") accepts an arbitrary
    state for a valid step index -- there is no dedicated "undo" verb, but
    writing the step back to its PRE-RUN state is a POST like any other, so
    this function snapshots step 1's {state, note} before the flip and
    re-POSTs that snapshot afterward via `_post_form_with_cookie()` (no
    page/CDP round trip needed for this one, since the route's shape is
    just two form fields). `ts` is not restorable -- the route has no field
    for it and always stamps the current time on write -- so a run of this
    row does leave step 1's `ts` at "now" even after a successful restore;
    only `state`/`note` are round-tripped and verified.

    Determinism: `expect_post` for BOTH CDP calls (the flipping Save and
    the restoring one -- each runs the same three-POST chain) is
    overridden to `/api/setup/progress` -- the THIRD and last of the three
    chained POSTs -- rather than the row's own default
    (`/api/unit_pref`, the second), so the driver waits for the whole
    chain (tz -> unit_pref -> progress) to resolve before tearing Chrome
    down. The previous default let the driver exit after the second POST,
    before the progress POST was necessarily issued, making the progress
    write's occurrence a race; waiting for it makes the pre/post snapshot
    comparison meaningful.

    Restore is UNCONDITIONAL once the first (flipping) Save has been
    attempted: every failure path after that point -- a CDP driver that
    exits non-zero or raises, a read-back that does not answer 200, a
    collateral time_tz write, or a unit that never changed -- still runs
    the restoring Save and appends its outcome to the failure message. The
    board must never be left holding the test unit merely because the
    check that would have noticed happened to fail first."""
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

    # Step 1's pre-run progress state, so postStepState(1,'done') (the third
    # of #step1Save's three chained POSTs) can be undone the same way the
    # tz/unit fields are: read it back, POST the snapshot back afterward.
    progress_path = "/api/setup/progress"
    pre_prog_status, pre_prog_body = _get_json_with_cookie(host, progress_path, cookie)
    if pre_prog_status != 200 or not isinstance(pre_prog_body, dict):
        return False, (f"{row.row_id} FAIL: pre-read GET {progress_path} -> "
                        f"{pre_prog_status}: {json.dumps(pre_prog_body)[:300]}")
    pre_step1 = (pre_prog_body.get("steps") or {}).get("1")
    if not isinstance(pre_step1, dict) or "state" not in pre_step1:
        return False, (f"{row.row_id} FAIL: {progress_path} body missing usable steps.1.state "
                        f"({json.dumps(pre_prog_body)[:300]}) -- refusing to guess a value to "
                        f"restore to")
    orig_step1_state = str(pre_step1["state"])
    orig_step1_note = pre_step1.get("note")
    test_unit = "F" if orig_unit == "C" else "C"

    set_fills = (("#wTz", orig_tz), ("#wUnit", test_unit))
    restore_fills = (("#wTz", orig_tz), ("#wUnit", orig_unit))

    def _restore_progress() -> "tuple[bool, str]":
        """Re-POSTs step 1's pre-run {state, note} to /api/setup/progress
        and confirms it took via read-back. `ts` is not restorable (the
        route has no field for it and always stamps the write time), so
        this only round-trips state/note -- see the docstring's "THIRD
        WRITE" section."""
        restore_fields = {"step": "1", "state": orig_step1_state}
        if orig_step1_note:
            restore_fields["note"] = orig_step1_note
        prog_status, prog_body = _post_form_with_cookie(host, progress_path, cookie, restore_fields)
        if prog_status != 200:
            return False, (f" -- PROGRESS RESTORE FAILED: POST {progress_path} -> {prog_status}: "
                            f"{json.dumps(prog_body)[:300]}; step 1 progress may be LEFT at "
                            f"'done', restore by hand to state={orig_step1_state!r}")
        final_prog_status, final_prog_body = _get_json_with_cookie(host, progress_path, cookie)
        if final_prog_status != 200 or not isinstance(final_prog_body, dict):
            return False, (f" -- PROGRESS RESTORE UNCONFIRMED: post-restore read-back GET "
                            f"{progress_path} -> {final_prog_status}: {json.dumps(final_prog_body)[:300]}; "
                            f"restore POST landed, but read-back could not confirm it")
        final_step1 = (final_prog_body.get("steps") or {}).get("1")
        if not isinstance(final_step1, dict) or str(final_step1.get("state")) != orig_step1_state:
            got = final_step1.get("state") if isinstance(final_step1, dict) else None
            return False, (f" -- PROGRESS RESTORE FAILED: step 1 state now {got!r}, expected "
                            f"{orig_step1_state!r} -- LEFT ON BOARD, restore by hand")
        # `note` is restored, so it is also VERIFIED. The click's own
        # postStepState(1,'done') carries no note field, and
        # setup_wizard_progress_set_step() CLEARS the stored note whenever
        # the POST omits it or sends it empty (setup_wizard_progress.c:
        # `note[0] = '\\0'` on a NULL/empty note), so any pre-run note is
        # destroyed by the flip and only this restore puts it back.
        # Comparing `state` alone would let a silently-dropped note (one
        # too long for http_form_find_field's buffer, so refused as -2 and
        # treated as absent) report PASS with the operator's text gone.
        final_note = str(final_step1.get("note") or "")
        if final_note != str(orig_step1_note or ""):
            return False, (f" -- PROGRESS RESTORE FAILED: step 1 note now {final_note!r}, expected "
                            f"{str(orig_step1_note or '')!r} (state itself was restored) -- the "
                            f"pre-run note is LEFT changed on the board, restore by hand")
        return True, (f" -- step 1 progress restored to state={orig_step1_state!r} "
                       f"(note {str(orig_step1_note or '')!r}) and confirmed")

    def _restore() -> "tuple[bool, str]":
        """The restoring second Save, plus its read-back, plus the setup-
        progress restore. Called on EVERY path that got as far as
        attempting the first (flipping) Save -- including a mid-state
        read-back that failed or a CDP driver that errored -- because any
        of those may have left the board on `test_unit` (and step 1 marked
        'done'), and this row must never end with the displayed unit
        flipped or the progress write unaddressed. The restore re-submits
        the ORIGINAL tz alongside the original unit, so it also repairs a
        collateral tz write rather than only reporting one."""
        try:
            # Same `expect_post` override as the flipping Save above, and for
            # the same reason plus a sharper one: the restoring Save runs the
            # SAME three-POST chain, so its own postStepState(1,'done') can
            # still be in flight when the driver exits. Waiting only for
            # /api/unit_pref (the row default, the SECOND POST) would let
            # _restore_progress() below post the pre-run state and read it
            # back BEFORE the page's late 'done' POST lands -- leaving step 1
            # at 'done' on the board while this row reported it restored and
            # confirmed.
            restore_proc = _run_cdp(row, host, screenshot_dir, cookie,
                                     fills=restore_fills,
                                     expect_post="/api/setup/progress", shot_suffix="_restore")
        except Exception as exc:  # noqa: BLE001 -- subprocess timeout/OSError must not skip the report
            _, prog_note = _restore_progress()
            return False, (f" -- RESTORE ATTEMPT ITSELF FAILED ({type(exc).__name__}: {exc}); board may "
                            f"be LEFT with temp_unit={test_unit!r}, restore by hand (originally "
                            f"{orig_unit!r}){prog_note}")
        if restore_proc.returncode != 0:
            _, prog_note = _restore_progress()
            return False, (f" -- RESTORE FAILED: CDP driver (restore) exited {restore_proc.returncode}: "
                            f"{restore_proc.stderr.strip()[-500:]}; board may be LEFT with "
                            f"temp_unit={test_unit!r}, restore by hand (originally {orig_unit!r}){prog_note}")
        bad_restore_posts = _non2xx_post_failures(
            _cdp_post_statuses(restore_proc, ("/api/settings/tz", "/api/unit_pref")))
        if bad_restore_posts:
            _, prog_note = _restore_progress()
            return False, (f" -- RESTORE FAILED: POST(s) did not answer 2xx during the restoring Save "
                            f"click: {'; '.join(bad_restore_posts)}; board may be LEFT with "
                            f"temp_unit={test_unit!r}, restore by hand (originally {orig_unit!r}){prog_note}")
        final_status, final_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
        prog_ok, prog_note = _restore_progress()
        if final_status != 200 or not isinstance(final_body, dict):
            return False, (f" -- RESTORE UNCONFIRMED: post-restore read-back GET {row.verify_endpoint} "
                            f"-> {final_status}: {json.dumps(final_body)[:300]}; restore POST landed, "
                            f"but read-back could not confirm it{prog_note}")
        if str(final_body.get("time_tz")) != str(orig_tz):
            return False, (f" -- RESTORE FAILED: time_tz now {final_body.get('time_tz')!r}, expected "
                            f"{orig_tz!r} -- LEFT ON BOARD, restore by hand{prog_note}")
        if final_body.get("temp_unit") != orig_unit:
            return False, (f" -- RESTORE FAILED: temp_unit still {final_body.get('temp_unit')!r}, "
                            f"expected original {orig_unit!r} -- LEFT ON BOARD, restore by hand{prog_note}")
        if not prog_ok:
            return False, f" -- board restored to temp_unit={orig_unit!r} and confirmed{prog_note}"
        return True, f" -- board restored to temp_unit={orig_unit!r} and confirmed{prog_note}"

    failure: "Optional[str]" = None
    try:
        proc = _run_cdp(row, host, screenshot_dir, cookie, fills=set_fills,
                         expect_post="/api/setup/progress", shot_suffix="_set")
    except Exception as exc:  # noqa: BLE001
        # The driver can die AFTER the click's POSTs landed (a screenshot or
        # teardown error, a 60 s timeout on a page that already saved), so a
        # raise here is not proof nothing was written -- restore anyway.
        failure = (f"{row.row_id} FAIL: CDP driver (set) raised {type(exc).__name__}: {exc}")
    else:
        if proc.returncode != 0:
            failure = (f"{row.row_id} FAIL: CDP driver (set) exited {proc.returncode}: "
                        f"{proc.stderr.strip()[-500:]}")
        else:
            # #step1Save's click fires THREE chained POSTs (tz, then
            # unit_pref, then setup/progress); `expect_post` above only
            # waits for and reports the LAST of the three
            # (`_cdp_post_status()`), so a non-2xx on either of the first
            # two would previously go unnoticed by this row entirely --
            # only the GET /api/status read-back afterward could catch it,
            # and only indirectly (a field failing to change). Check the
            # first two explicitly, from the same driver-recorded network
            # list `expect_post`/`_cdp_post_status()` already reads from.
            bad_posts = _non2xx_post_failures(
                _cdp_post_statuses(proc, ("/api/settings/tz", "/api/unit_pref")))
            if bad_posts:
                failure = (f"{row.row_id} FAIL: POST(s) did not answer 2xx during the Save click: "
                            f"{'; '.join(bad_posts)}")

    if failure is None:
        mid_status, mid_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
        if mid_status != 200 or not isinstance(mid_body, dict):
            failure = (f"{row.row_id} FAIL: post-set read-back GET {row.verify_endpoint} -> "
                        f"{mid_status}: {json.dumps(mid_body)[:300]}")
        elif str(mid_body.get("time_tz")) != str(orig_tz):
            failure = (f"{row.row_id} FAIL: this row's Save also CHANGED time_tz (now "
                        f"{mid_body.get('time_tz')!r}, was {orig_tz!r}) -- collateral write")
        elif mid_body.get("temp_unit") != test_unit:
            failure = (f"{row.row_id} FAIL: temp_unit did not change (still "
                        f"{mid_body.get('temp_unit')!r}, expected {test_unit!r}) -- write likely "
                        f"did not land")

    restored_ok, restore_note = _restore()
    if failure is not None:
        return False, failure + restore_note
    if not restored_ok:
        return False, f"{row.row_id} FAIL:{restore_note}"

    return True, (
        f"{row.row_id} PASS: toggled temp_unit {orig_unit!r} -> {test_unit!r} via setup wizard "
        f"step 1 Save (time_tz re-submitted unchanged at {orig_tz!r}), confirmed via GET "
        f"{row.verify_endpoint}, restored to {orig_unit!r} via a second Save, confirmed restored, "
        f"and step 1's /api/setup/progress state restored to {orig_step1_state!r} via a direct "
        f"POST | expected: {row.expected_outcome}"
    )


def _run_guarded_click(row: Row, host: str, screenshot_dir: str, cookie: str, *,
                        is_busy: "Callable[[dict], bool]", busy_desc: str) -> "tuple[bool, str]":
    """Shared shape for W18/W20/W33: a control that only ever cancels or
    exits an operation, never starts one, so the runbook itself classifies
    it read-only -- but a live click still needs a precondition, because
    clicking it while a REAL operation is in progress would cancel another
    session's work, not merely probe the control. Refuses before touching
    the board unless `row.verify_endpoint`'s read-back already shows idle/
    inactive per `is_busy`; then clicks, then re-reads and fails loud if the
    status somehow reads busy afterward (which would mean this click
    started something, the opposite of what it's supposed to do). There is
    nothing to restore: the click is a no-op by construction on this path."""
    pre_status, pre_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if pre_status != 200 or not isinstance(pre_body, dict):
        return False, (f"{row.row_id} FAIL: pre-read GET {row.verify_endpoint} -> "
                        f"{pre_status}: {json.dumps(pre_body)[:300]}")
    if is_busy(pre_body):
        return False, (f"{row.row_id} FAIL: refusing -- {row.verify_endpoint} shows {busy_desc} "
                        f"already in progress ({json.dumps(pre_body)[:200]}); clicking this row's "
                        f"abort/exit control would cancel a real operation, not just probe it")
    try:
        proc = _run_cdp(row, host, screenshot_dir, cookie)
    except Exception as exc:  # noqa: BLE001
        return False, f"{row.row_id} FAIL: CDP driver raised {type(exc).__name__}: {exc}"
    if proc.returncode != 0:
        return False, (f"{row.row_id} FAIL: CDP driver exited {proc.returncode}: "
                        f"{proc.stderr.strip()[-500:]}")
    post_status, post_body = _get_json_with_cookie(host, row.verify_endpoint, cookie)
    if post_status != 200 or not isinstance(post_body, dict):
        return False, (f"{row.row_id} FAIL: post-click read-back GET {row.verify_endpoint} -> "
                        f"{post_status}: {json.dumps(post_body)[:300]}")
    if is_busy(post_body):
        return False, (f"{row.row_id} FAIL: {row.verify_endpoint} shows {busy_desc} after clicking "
                        f"the abort/exit control ({json.dumps(post_body)[:200]}) -- unexpected, "
                        f"check the board by hand")
    return True, (
        f"{row.row_id} PASS: confirmed {row.verify_endpoint} was idle/inactive before the click, "
        f"clicked {row.selector}, confirmed it remains idle/inactive afterward "
        f"| expected: {row.expected_outcome}"
    )


def _run_sweep_abort_guarded(row: Row, host: str, screenshot_dir: str, cookie: str) -> "tuple[bool, str]":
    """W18: refuses unless /api/zones/current_sweep/status already reads
    something other than 'running' (zones_page.html's own
    `renderSweepStatus()` derives `running = st.state === 'running'`)."""
    return _run_guarded_click(row, host, screenshot_dir, cookie,
                               is_busy=lambda body: body.get("state") == "running",
                               busy_desc="a current-sweep run")



# The states autotune_engine actually treats as "running" -- everything else
# (idle, done, aborted) is a state where autotune_engine_abort() itself is a
# verified no-op (autotune_engine_guard.c's autotune_engine_abort(): it takes
# the lock, checks state_is_running(s_at.state), and returns immediately
# without touching s_at at all when that's false). 'done'/'aborted' are
# terminal states left over from a *previous* run and stay that way until the
# next autotune_start -- on any board where autotune has ever run once, W20's
# /api/autotune read-back can never show 'idle' again, so treating "anything
# but idle" as busy (the original, more conservative reading) makes this
# row's precondition permanently unsatisfiable rather than merely cautious.
# Since the click is a true no-op in 'done'/'aborted' too, per the firmware
# guard above, those two are idle-equivalent start states for this row.
_AT_NON_RUNNING_STATES = ("idle", "done", "aborted")


def _run_autotune_abort_guarded(row: Row, host: str, screenshot_dir: str, cookie: str) -> "tuple[bool, str]":
    """W20: refuses unless /api/autotune already reads a non-running state
    (idle, done, or aborted -- see `_AT_NON_RUNNING_STATES` above). Anything
    else (settling/stepping/relay_approach/relay_cycling, per
    zones_page.html's `AT_STATE_NAMES`) means a run is actually in flight and
    still counts as busy."""
    return _run_guarded_click(row, host, screenshot_dir, cookie,
                               is_busy=lambda body: body.get("state") not in _AT_NON_RUNNING_STATES,
                               busy_desc="an autotune run in progress")


def _run_danger_exit_guarded(row: Row, host: str, screenshot_dir: str, cookie: str) -> "tuple[bool, str]":
    """W33: refuses unless /api/diagnostics/danger already reads
    active=false (diagnostics_page.html's `dangerPoll()`/`renderDangerLive()`)."""
    return _run_guarded_click(row, host, screenshot_dir, cookie,
                               is_busy=lambda body: bool(body.get("active")),
                               busy_desc="an active Danger Mode session")


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
        _read_credentials()  # fail fast, actionable, before touching the board
        cookie = _login_once(host)
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
    if row.special == "profile_segment_create_delete":
        return _run_profile_segment_create_delete(row, host, screenshot_dir, cookie)
    if row.special == "profile_multi_delete":
        return _run_profile_multi_delete(row, host, screenshot_dir, cookie)
    if row.special == "profile_favorite_toggle":
        return _run_profile_favorite_toggle(row, host, screenshot_dir, cookie)
    if row.special == "sweep_abort_guarded":
        return _run_sweep_abort_guarded(row, host, screenshot_dir, cookie)
    if row.special == "autotune_abort_guarded":
        return _run_autotune_abort_guarded(row, host, screenshot_dir, cookie)
    if row.special == "danger_exit_guarded":
        return _run_danger_exit_guarded(row, host, screenshot_dir, cookie)

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
        f"{row_id} PASS: {_cdp_stdout_summary(proc)} | expected: {row.expected_outcome} | "
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
