# Commissioning Web-UI Row Runbook

Prepared so the M18 web-interface class (`docs/COMMISSIONING_TEST_MATRIX.md`,
"Web UI second" per `project_owner_decisions_2026_09_21_login`, the class
that runs after the backend class in `docs/COMMISSIONING_BACKEND_RUNBOOK.md`)
can be executed mechanically once the board is free, by clicking the real
control rather than only reading the route it calls. It complements, and
does not replace, `docs/COMMISSIONING_WEBUI_RUNBOOK.md` (the route/tier
inventory and read-only/reversible/deferred classification) and
`docs/COMMISSIONING_TEST_MATRIX.md`'s page-by-page inventory (the
authoritative Control/Route/Tier/MCP-tool/Result table) -- this document adds
the missing layer those two identify but do not provide: a literal,
selector-level UI action for each row, its expected visible outcome, and the
backend call that confirms the click actually took effect. Every selector
below was verified present in source at `origin/main` `fb1a933f` (grep
against the listed file); see "Selectors not found" at the end.

This document was produced without contacting any board.

## Classification used below

Per the task's own gate, not `COMMISSIONING_WEBUI_RUNBOOK.md`'s three-way
split (that document's classes are about *board-state risk*; this one is
about *who may run the row at all*):

- **read-only** -- a GET/page-load or a client-side-only action. Any bench
  agent may run these any time, any order.
- **write** -- a POST that changes persisted state but does not heat, reset
  a processor, flash firmware, or change auth. Runnable by a bench agent
  with the row's restore step, per `COMMISSIONING_WEBUI_RUNBOOK.md`'s
  "Reversible controls" bucket.
- **owner-gated** -- heats, resets a processor, flashes, or changes
  authentication/credentials. Per this task's instruction, E-stop verify and
  the Pico-reset rows are already owner-authorized for M18 (owner decisions
  2026-09-21, `project_owner_authorized_class_c_all_2026_09_21`) -- listed
  here as owner-gated (**pre-authorized**) rather than blocked outright.
  Every other owner-gated row still needs a fresh authorization naming it.

## Row table

Columns: page/route, exact UI action (selector or button text), expected
visible outcome, backend read-back, classification. "Selector" is a DOM id
unless noted; "button text" is given only for controls with no stable id.

| # | Page (route) | UI action | Expected visible outcome | Backend read-back | Class |
|---|---|---|---|---|---|
| W1 | `/login` | Fill `#username`/`#password` in `#login-form`, submit (no separate submit button id -- the form's own submit handler, `login_page.html`) | Redirect to `/` (or to the page originally requested pre-login); no `#error` text shown | `GET /api/auth/session` shows an authenticated session | write (auth) |
| W2 | `/` | Page load (no click) | Dashboard renders: status tiles, history chart, zone rows | `GET /api/status` (`get_board_state`) matches rendered temps/state | read-only |
| W3 | `/` | Click `#ackLastRunBtn` ("Dismiss") | Last-run banner disappears | `profiles_ack_last_run` / re-poll `GET /api/profile_exec` shows no last-run banner condition | write |
| W4 | `/` | Click `#clearTripBtn` ("Clear Trip") | Trip banner clears from the dashboard | `safety_get_status`: `trip_reason`/`trip_mask` return to none -- **only run after confirming via `safety_get_status` that the only latched trip is `SAFETY_TRIP_MAIN_FAULT` (bit 5, `0x0020`)**, per CLAUDE.md | write |
| W5 | `/` | PID popup: click `#pidPopupUseProposed` then `#pidPopupApplyBtn` | Popup closes, zone's Kp/Ki/Kd row updates to the proposed values | `control_get_zones` -- read gains before, confirm after, restore afterward | write |
| W6 | `/` | Click `#themeBtn` | Page recolors light/dark | none -- client-side only, no route | read-only |
| W7 | `/profiles` | Page load | Profile list renders (builtin + saved + favorites) | `profiles_list` matches rendered rows | read-only |
| W8 | `/profiles` | Click `#newBtn`, fill form, click `#saveBtn` | New profile appears in the list | `profiles_get`/`profiles_list` shows the new profile; **use a throwaway name, delete via W9 afterward**. **Not wired into `web_commission_row.py`, and won't be forced: the segment builder (`#segments`/`#addSegBtn`) adds rows dynamically via JS, so there are no stable pre-existing ids for the CDP driver's one-shot pre-click `fills` to target** | write |
| W9 | `/profiles` | Select the throwaway profile, click delete (`#modeDeleteBtn` then the per-row delete action) | Profile removed from the list | `profiles_list` no longer lists it. **Not wired: the per-row delete action has no stable id, only a dynamically-rendered per-row control the CDP driver's selector kinds can't address** | write |
| W10 | `/profiles` | Toggle favorite (per-row star/favorite action) | Star state flips | `profiles_get` shows `favorite` toggled; **toggle back before leaving**. **Not wired: `favToggleBtn()` (profiles_page.html) gives the per-row star no id, and its `textContent` is the bare glyph (identical on every row), so the CDP driver's "text" selector kind -- first matching button wins -- would toggle whichever profile renders first, not the intended one** | write |
| W11 | `/live_profile` | Page load (only meaningful during a firing) | Working-copy status/content renders | `profile_live_get` | read-only |
| W12 | `/live_profile` | Click fork button (`forkBtn` id, `live_profile_page.html`) | Working copy id appears, editable fields populate | `profile_live_get` reports a `working_id` | owner-gated (needs a firing running -- W-heat) |
| W13 | `/live_profile` | Edit a dwell/segment field, click `saveBtn` | Field save confirms, no 400/409 shown | `profile_live_get` shows the edited value | owner-gated (same precondition as W12) |
| W14 | `/live_profile` | Click `discardBtn` | Working copy discarded, page returns to origin profile view | `profile_live_get`/`profile_live_decide` -- origin profile unchanged | owner-gated (same precondition) |
| W15 | `/settings/zones` | Page load | Zone table + PID fields render | `control_get_zones` matches | read-only |
| W16 | `/settings/zones` | Edit a zone name field, click `#saveBtn` | Field save confirms (no error banner) | `control_get_zones` shows new name; **GET first, restore verbatim after** -- see backend runbook B4 for the required whole-page GET-merge-POST body shape | write |
| W17 | `/settings/zones` | Click Measure Normal Current (`sweepStartBtn` id) | Sweep progress UI appears, then a result | `zone_current_sweep_status` | owner-gated (drives every zone's relay) |
| W18 | `/settings/zones` | Click Abort sweep (`sweepAbortBtn`) | Sweep UI returns to idle | `zone_current_sweep_status` reports idle | read-only (abort-only, per `COMMISSIONING_WEBUI_RUNBOOK.md`) |
| W19 | `/settings/zones` | Click Start step test (`atStartBtn`) | Autotune progress UI appears | `autotune_get_status` shows running | owner-gated (heats) |
| W20 | `/settings/zones` | Click Abort (`atAbortBtn`) | Autotune UI returns to idle | `autotune_get_status` shows idle | read-only (abort-only) |
| W21 | `/settings/safety` | Page load | Rate-guard fields render | `safety_get_rate_guard` matches | read-only |
| W22 | `/settings/safety` | Edit a field, click Save (`save` id) | Save confirms | `safety_get_rate_guard`; **GET first, restore after** -- note BLOCKED in the last sweep while ARMED, needs the GRACE window | write |
| W23 | `/safety` | Page load | Safety status/banner renders | `safety_get_status` matches | read-only |
| W24 | `/safety` | Click `#clearTripBtn`, confirm dialog `#confirmYes` | Same as W4 | `safety_get_status` | write (same S6a-only caveat as W4) |
| W25 | `/safety/commissioning` | Page load | Wizard renders current commissioning values | `safety_get_commissioning` matches | read-only |
| W26 | `/safety/commissioning` | Wizard Stage & commit (`gCommitBtn`/`saveBtn`) | "Committed" confirmation, no 4xx | `safety_get_commissioning` shows the field; **needs the Pico GRACE window, a deliberate reset -- pre-authorized per M18 owner decisions** | owner-gated (pre-authorized: Pico reset) |
| W27 | `/safety/commissioning` | CT Auto-zero (`.ct-cal-auto-zero`) | Confirmation shown, no error | `safety_get_commissioning` reflects the new zero | owner-gated (safety-config write) |
| W28 | `/diagnostics` | Page load | Thermo fault table + diagnostics tiles render | `thermo_read_faults` matches | read-only |
| W29 | `/diagnostics` | Click `#crashAckBtn` ("Acknowledge") | Crash banner disappears/greys out | `crash_report_ack` read-back shows `acknowledged: true` | write |
| W30 | `/diagnostics` | Click `#wdPanicToggleBtn` | Toggle state flips visibly | `get_watchdog_panic_disabled` shows new value; **restore to enabled before ending session, per CLAUDE.md** | write |
| W31 | `/diagnostics` | Click `#rampAssistToggleBtn` | Toggle state flips | `ramp_assist_get_enabled`; restore | write |
| W32 | `/diagnostics` | Click Enter Danger Mode (`dangerEnterBtn`) | Danger-mode banner appears, per-relay controls unlock | `GET /api/diagnostics/danger` shows enabled | owner-gated (drives relays outside the safety-gated path) |
| W33 | `/diagnostics` | Click Exit Danger Mode (`dangerExitBtn`) | Banner clears | `GET /api/diagnostics/danger` shows disabled | read-only (exit-only, per `COMMISSIONING_WEBUI_RUNBOOK.md`) |
| W34 | `/diagnostics` | E-stop verify button (route `POST /api/estop/verify`) | Verify result shown | `GET /api/status` safety fields | owner-gated (**pre-authorized for M18** -- needs the bench E-stop jumper physically pulled) |
| W35 | `/diagnostics` | Relay-cycle reset then restore (`relay-reset-btn`) | Counter resets, then restores | `GET /api/status` relay_life fields; **note the known B12 anomaly, relays 1/2's restore can clamp to an internal count not shown on `/api/status`** | write |
| W36 | `/settings` | Click Reboot both processors (`#swResetBtn`) | Page shows "rebooting"/disconnect, board comes back on `/` | `get_heap_status` shows a fresh `uptime_s`; watch for expected S6a during the dual reset, per CLAUDE.md | owner-gated (**pre-authorized for M18** -- Pico reset) |
| W37 | `/settings/display` | Page load | Display settings render | `GET /api/settings/display_power` | read-only |
| W38 | `/settings/display` | Edit a field, Save | Save confirms | `GET /api/settings/display_power`; GET first, restore after | write |
| W39 | `/settings/security` | Page load | Auth config/policy fields render | `GET /api/auth/config` | read-only |
| W40 | `/settings/security` | Set web password/policy (save button, `security_page.html`) | Save confirms | `GET /api/auth/config` -- **credential source only `KILNCTL_WEB_USERNAME`/`KILNCTL_WEB_PASSWORD`, never typed by an agent**, per `reference_bench_web_credentials_env_vars` | owner-gated (auth change) |
| W41 | `/settings/kiln_configs` | Page load | Config-preset list renders | `list_config_presets` / `GET /api/kiln_configs` | read-only |
| W42 | `/settings/kiln_configs` | `#kcSaveNewName` + click `#kcSaveNewBtn` | New slot appears in `#kilnConfigSelect` | `GET /api/kiln_configs` lists the throwaway slot; delete it afterward | write |
| W43 | `/settings/backup` | Page load | Backup/restore controls render | `GET /api/backup/export` headers (no body diff needed) | read-only |
| W44 | `/settings/backup` | Export (download link/button) | Browser downloads a file | file exists, non-empty | read-only |
| W45 | `/settings/backup` | Import (restore-from-file control) | Confirmation/apply UI | `GET /api/status`/`GET /api/zones` reflect the imported config | owner-gated (destructive overwrite) |
| W46 | `/ota` | Page load | OTA/interlock status renders | `ota_status` matches | read-only |
| W47 | `/ota` | Update/rollback controls | Progress UI, then result | `ota_status` | owner-gated (flash/OTA -- use `flash_firmware()`/`ota_rollback_esp()` per CLAUDE.md, not this browser control, when actually scheduled) |
| W48 | `/readiness` | Page load | Commissioning checklist renders, one line per item | `get_readiness` matches | read-only |
| W49 | `/setup` | Wizard Start/Next/Back navigation | Step changes, no route hit (client-side) | none -- client-side only | read-only |
| W50 | `/setup` | Step save that posts to `/api/unit_pref`/`/api/settings/tz` | Step confirms | `GET /api/unit_pref` / `GET /api/settings/tz`; read first, restore after (restore runs on every failure path too, including a driver raise). **Also marks wizard step 1 via `POST /api/setup/progress` -- the row now reads `GET /api/setup/progress` before the click too and restores that step's prior `state`/`note` (via `POST /api/setup/progress`) after, verified by a read-back; refuses up front if the prior state can't be read, and fails loud (never PASS) if the progress restore doesn't take.** The driver waits for the LAST of the click's three chained POSTs (`/api/setup/progress`, overriding the row's default `expect_post`), not the second, so teardown can't race the chain. **Wired, not yet run live: `_run_setup_wizard_step1` in `web_commission_row.py`, reached via the `#step=1` URL-hash deep link (`setup_wizard_page.html`'s `loadAll()` reads `location.hash` on load and calls `gGoto()` directly)** | write |
| W51 | `/`, `/wifi` | Wi-Fi scan/connect/forget | Network list updates / connect result shown | `wifi_get_status`/`wifi_get_networks`; **only add/forget a throwaway network, never the bench's primary one without a second access path confirmed first** | write |

Rows not listed individually (theme toggle repeated per page, wizard
client-side-only navigation repeated per step, static asset loads) follow
the same read-only classification as their first occurrence above (W6/W49)
and are not worth a separate row.

## CDP driver capability note (2026-09-21)

`tools/PcTools/scripts/_web_commission_cdp.mjs` (the live-mode driver behind
`web_commission_row.py`'s `run_row_live()`) previously could only match a
click target by `id` or by button text, and could only run one `--fills` +
one click per invocation. That is what left W8/W9/W10 above unwired (see
`web_commission_row.py`'s `ROWS` dict header comment, "Left unwired, and
why"): W10's favourite star has no id at all, only a per-profile
`aria-label`; W9's per-row delete checkbox is created only after
`#modeDeleteBtn` is clicked and likewise has no id; and W8's segment fields
(`profiles_page.html`'s `segmentRow()`) are created only after `#addSegBtn`
is clicked and have no id, only classes.

The driver now has three additions that remove those specific blockers (see
the driver's own header comment for full option syntax):

- `--selector-kind aria-label` -- exact-match click on an element's
  `aria-label` attribute. Reaches W10's star directly:
  `--selector-kind aria-label --selector 'Add "<profile name>" to favorites'`
  (or `Remove "..." from favorites` if already a favourite -- read the
  current state via `profiles_list`'s `favorite` field first, since the
  label itself flips).
- `--selector-kind css` -- click on a raw CSS selector. Reaches a per-row
  control that has neither an id nor a useful aria-label, e.g. a specific
  saved profile's delete checkbox once the list is in delete mode:
  `[aria-label="Select \"<profile name>\" for delete"]` (this could also be
  reached directly via `--selector-kind aria-label`, since that checkbox
  does carry one -- `css` is for the cases that don't, like an
  `nth-child()`-addressed segment row).
- `--steps <json>` -- an ordered click/fill/wait-for-selector/wait-for-post
  sequence, run after navigation and after the original single-shot
  `--fills` + click path (which is unchanged). This is what actually
  removes the "created by an earlier click" blocker common to all three
  rows.

Concrete step lists a Row() entry for each could pass (not implemented here
-- wiring `web_commission_row.py` itself is out of scope for this change):

- **W10** (favourite toggle) needs no `--steps` at all -- a single
  `--selector-kind aria-label` click plus `--expect-post /api/profile/favorite`
  is sufficient, same shape as every other single-click write row.
- **W9** (per-row delete):
  ```json
  [
    {"action": "click", "kind": "id", "selector": "modeDeleteBtn"},
    {"action": "wait-for-selector", "selector": "input[aria-label='Select \"<throwaway name>\" for delete']"},
    {"action": "click", "kind": "aria-label", "selector": "Select \"<throwaway name>\" for delete"},
    {"action": "click", "kind": "id", "selector": "bulkActionBtn"}
  ]
  ```
  run with `--accept-dialogs` (bulk delete confirms via `kcConfirm()`) and
  `--expect-post /api/profile/delete`.
- **W8** (segment builder), after `#newBtn`/name fill create the profile
  shell:
  ```json
  [
    {"action": "click", "kind": "id", "selector": "addSegBtn"},
    {"action": "wait-for-selector", "selector": "#segments .seg"},
    {"action": "fill", "kind": "css", "selector": "#segments .seg:nth-child(1) select.seg-kind", "value": "0"},
    {"action": "fill", "kind": "css", "selector": "#segments .seg:nth-child(1) input.s-target", "value": "950"},
    {"action": "fill", "kind": "css", "selector": "#segments .seg:nth-child(1) input.s-ramp", "value": "120"},
    {"action": "fill", "kind": "css", "selector": "#segments .seg:nth-child(1) input.s-dwell", "value": "10"},
    {"action": "click", "kind": "id", "selector": "saveBtn"},
    {"action": "wait-for-post", "path": "/api/profile"}
  ]
  ```
  Field classes verified against `rampFieldsHtml()`
  (`firmware/KilnFW/App/drivers/http/profiles_page.html`, which
  `renderSegFields()` calls for any kind other than 1): the ramp/dwell
  row's three inputs are `.s-target`, `.s-ramp` and `.s-dwell` -- there is
  no `.target-c`. A `seg-kind` of `1` renders `ioFieldsHtml()` instead,
  whose fields (`.io-blocking`, `.io-state`) are a different set entirely,
  and switching kind rebuilds that row's fields from scratch, so the
  `seg-kind` fill must always come FIRST -- a field filled before it is
  destroyed by the rebuild.

End-to-end coverage of the three new primitives (aria-label click, css
click, and a click -> wait-for-selector -> fill -> click -> wait-for-post
chain against a local fixture, plus a negative case for a `--steps` fill
against a missing selector) is in
`tools/PcTools/tests/web_commission_cdp_driver.test.mjs`
(`node tools/PcTools/tests/web_commission_cdp_driver.test.mjs`, real headless
Chrome, no board contacted). It also covers the two things a driver bug
would otherwise hide from every row that uses these primitives: that a
`fill` really dispatches a bubbling `change` (the event
`profiles_page.html`'s `segmentRow()` rebuilds its fields from), and that
a mid-sequence `wait-for-post` followed by a final `--expect-post` on the
SAME path reports the second request rather than re-reporting the first.

It runs in the standing suite via
`tools/PcTools/tests/check_web_commission_cdp_driver.ps1`, in
`run_all_checks.ps1`'s serial phase 3 alongside
`check_ui_responsive_sweep.ps1` (both drive real headless Chrome), and
SKIPs -- never fails -- when `node` or a Chrome/Edge binary is absent.

## Selectors not found

`#kilnConfigCard`'s per-slot Apply action (`#kcApplyBtn`) exists and was
grepped successfully; every id cited above was confirmed present in its
named source file at `fb1a933f`. The one caveat: `#save` on
`safety_config_page.html` (W22) and `#pidPopupApplyBtn` (W5) are id
selectors reused verbatim from `docs/COMMISSIONING_TEST_MATRIX.md`'s
page-by-page table, not independently re-derived here — that table itself
records they were grepped from source when built. No row's selector was
found to be missing or renamed.

## Recommended order

1. Read-only rows first (any order) -- confirms every page renders and every
   status route agrees with what's on screen.
2. Write rows, one page at a time, restoring after each per its row above --
   do Wi-Fi and security last (W40, W51), since they can change how the
   board is reached or authenticated.
3. Owner-gated rows only as separately authorized -- W26/W34/W36 are already
   pre-authorized for M18 per the owner decisions above; everything else in
   that class needs its own go-ahead.
4. Log results in `docs/COMMISSIONING_TEST_MATRIX.md`'s existing per-row
   `Result` column, matched by route/control, the same way the backend class
   already does -- this document is a driving aid, not a second source of
   truth.
