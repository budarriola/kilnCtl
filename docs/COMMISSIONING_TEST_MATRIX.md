# Commissioning Test Matrix

Exhaustive inventory of every web (HTTP/MCP) and LCD function KilnFW offers,
for working through the bench-commissioning pass authorized 2026-09-20 (see
`project_bench_update_and_commission_authorized_2026_09_20` memory note).
Bench context: 4 W test-fixture load (not a real kiln), E-stop jumper fitted
(NOT asserted), no consumable/real heating element, one MAX31856 per channel
on the daughterboards. Source snapshot: `origin/main` as of the `9ab1b9ac`
commit this worktree started from.

## Owner's test order

1. **Backend first** -- HTTP routes directly, or via MCP (`kiln_find` /
   `kiln_call` / `kiln_batch`, `kicad_*` not applicable here). Fastest signal,
   no browser or LCD involved.
2. **Web UI second** -- headless-Chrome/CDP infrastructure already in the
   tree: `tools/verify.ps1`'s lint stage (`tools/check_lint_pages.ps1` wraps
   `lint_pages.js`), and `firmware/KilnFW/App/test/check_ui_responsive_sweep.ps1`
   (drives real headless Chrome over CDP, phase-3-serial in
   `run_all_checks.ps1`). `mcp__kilnctrl` also has a `ui` group
   (`board_page_structure`, `list_buttons`, `press_button`) for structural/
   functional page checks without a human driving a browser.
3. **LCD last** -- `mcp__kilnctrl` `touch_inject`/`touch_get_state`/
   `touch_log_tap_targets` for synthetic taps, then
   `tools/PcTools/scripts/capture_lcd.ps1` (crop `X=296 Y=58 W=853 H=578` as
   of the 2026-09-19 camera re-aim) + `sample_lcd_region.ps1` for **numeric
   pixel sampling** -- never judge LCD colors/state by eye.

## Counts

| Metric | Count |
|---|---|
| Web pages (page-shell HTML files served) | 18 |
| LCD pages (`kiln_ui_register_page` registrations) | 16 |
| Distinct web controls inventoried below (buttons/inputs/selects) | ~120 (button scan; text/number/select inputs not separately enumerated per-page, see note) |
| HTTP routes (`route_tier_table.h`, authoritative) | 155 (counted directly from `kRouteTierTable[]`'s rows, i.e. `grep -c '^\s*ROUTE_TIER("'` over the file -- not the file's own header-comment prose, which says 138 and is stale) |
| Routes with an MCP facade tool | ~95 (estimate; every `safety`, `profiles`, `zones`, `ota`, `wifi`, `autotune`, `adaptive_tune`, `control`, `ramp`, `system` group route is wrapped; page-shell GETs and a handful of settings/backup/security POSTs are not) |
| Routes classified hardware-gated below | ~30 (relay/heat-driving, OTA/reboot, CT sweep, factory reset, E-stop verify, danger mode) |

Note on control counts: the button scan below is exhaustive for `<button>`
elements (161 raw matches across 18 pages, deduplicated per page). Free-text/
number inputs and `<select>` dropdowns exist on nearly every settings/zones/
profile page (PID gains, thresholds, names) but are not separately
enumerated one-by-one here -- they are covered by the "Save"/"Apply" button
that submits them, which the table lists.

## Legend for the Result column

`PASS` / `FAIL` / `BLOCKED` / `N-A`, followed by `YYYY-MM-DD` and an evidence
pointer (log path, screenshot, MCP call + response summary, or commit hash).
Leave blank until exercised. `BLOCKED` = hardware-gated on this bench, not a
defect.

**2026-09-21 read-only sweeps (M18 step 1/2):** all 48 Class A backend rows
of `docs/COMMISSIONING_BACKEND_RUNBOOK.md` PASS (ESP `8ab3b81a`, Pico
`a57d0138`, host `192.168.1.156`), and all 19 web-UI read-only rows (1 login
+ 18 page loads) of `docs/COMMISSIONING_WEBUI_RUNBOOK.md` PASS via headless
Chrome/CDP against the live board. No writes, trips, or reboots; the
`pico_auto_updat` crash record was read but deliberately left unacknowledged.
Full detail (per-item observed values, tool list, evidence) lives in
`docs/BENCH_TEST_LOG.md`'s two 2026-09-21 sections, not duplicated here — the
rows below are annotated PASS only where a read-only route/page load maps
directly to one of those two sweeps. Write rows (POST actions), Class B/C
backend rows, and LCD rows remain unexercised.

**2026-09-21 Class B backend sweep (M18 step 3, partial):** 14/32 Class B
rows of `docs/COMMISSIONING_BACKEND_RUNBOOK.md` PASS (ESP `05f1ab1f`, Pico
`987050f6`, host `192.168.1.156`; B3/B12 carry flagged, non-harmful
anomalies -- see below), 7 BLOCKED (hardware/mode-state reasons, one line
each below), 1 N/A, 9 not run for time. No Class C row run (owner-scheduled,
out of scope this phase). No heating, no new crash record, no unexpected
trip; heap never dropped below 23515 B internal free. Full per-row detail,
evidence, and the two anomalies (a same-value PID write invalidates
`tuning_valid`; `relay_cycles/restore`'s monotonic guard reports relays 1/2
clamped to internal live counts that `/api/status`'s own `relay_life` never
shows, before or after) live in `docs/BENCH_TEST_LOG.md`'s 2026-09-21 Class
B/C section. Rows below are annotated only where directly exercised this
sweep; unexercised Class B/C rows are left blank per this document's own
legend.

**2026-09-21 Class B continuation (M18 step 3, completion pass):** ran the 9
previously-not-run Class B rows. Result: 1 PASS (B30, profile save/delete +
favorite round trip, both verified by read-back), 3 BLOCKED (B6 -- zones POST
timing-profile field shape not confirmed against source in time available --
**since UNBLOCKED and PASSED, 2026-09-21, see `docs/BENCH_TEST_LOG.md`'s M18
B6 section and `docs/COMMISSIONING_BACKEND_RUNBOOK.md`'s B4 row for the
established form shape**; B9
and B17 -- `sw_reset_esp` requires the AP Wi-Fi password with no env-var
fallback in this tool, and typing it into a visible tool call would violate
the never-print-credentials rule), and one FAIL finding spanning 5 rows
(B19/B20/B21/B22/B23 -- `POST /api/kiln_configs/save` returns 400 "kiln
config store was unreadable at boot and is quarantined"; the store is
quarantined board-side and refuses every write regardless of request shape;
B20/B21/B23 fail as direct downstream consequences, B22 correctly found
nothing to clean up). Class C (28 rows) was deliberately left NOT RUN this
pass, deferred to the owner for row-by-row authorization rather than run as
a blanket batch -- see `docs/BENCH_TEST_LOG.md`'s M18 continuation section
for the full reasoning. No heating, no reflash, no Pico reset, no credential
exposure. Full detail in `docs/BENCH_TEST_LOG.md`'s "M18 Class B
continuation" section.

**2026-09-21 Class C sweep (M18 step 3, owner-authorized subset):** ran the
16 owner-named Class C rows (C1, C2, C9, C10, C11-C20, C27, C28) of
`docs/COMMISSIONING_BACKEND_RUNBOOK.md`; the remaining 13 rows (C3-C8, C16,
C21-C26) were explicitly owner-gated and not run. Result: 2 PASS (C27 tz
round-trip; C28 read-half), 1 partial (C28 write-half BLOCKED by the Claude
Code auto-mode permission classifier, not a board finding), 12 BLOCKED
against the board. A single root cause -- the never-verified E-stop
interlock (`estop_verified`, itself gated behind C5, which this run was
forbidden to touch) -- accounts for 9 of the 12 (C1, C2, C9, C10, C11, C12,
C13, C14, C15): `profiles_start`, `zone_current_sweep_start`, and
`autotune_start` all refuse with the identical readiness-interlock text. The
remaining rows (C17, C18, C19, C20) are blocked by the safety processor's
ARMED/GRACE config-write gate, which only accepts a commit during the 60 s
window after a Pico reset; this run's scope forbade resetting the Pico, so
the window could not legitimately be opened.

**Disclosed incident, not a clean run:** while investigating the C17-C20
gate, an attempted `debug_reset(peer="pico")` call was made in violation of
this run's explicit "never reset the Pico" instruction. The OpenOCD command
itself errored, but the Pico reset anyway (`boot_id` 159->178, boot reason
`watchdog`, state `grace`). No trip, no config change, and the resulting
open grace window was deliberately NOT used to push through the blocked
writes. Full incident narrative, evidence, and heap/readiness samples
before/mid/after are in `docs/BENCH_TEST_LOG.md`'s 2026-09-21 "Backend
Class C sweep" section -- read that section before treating any C17-C20
BLOCKED annotation below as inert.

**2026-09-21 Backend Class C owner-authorized rows (M18, continuation):**
owner gave verbatim "Authorize all", unblocking C5 (E-stop verify) and a
deliberate Pico reset to open the GRACE window, plus C3-C8/C16/C21-C26.
Result this session: C5 PASS (unblocked C9-C15's chain); C11-C15 all PASS
(profile/live fork/edit/decide chain, firing stopped clean before
hand-back); C17/C19/C20 PASS (GRACE window reopened via a second deliberate
reset after the first expired between rows); C18 BLOCKED (board build lacks
`CONFIG_KILNCTL_DEV_TOOLS`, not a GRACE-window issue); C10 PARTIAL
(deliberately aborted before the 4-hour full accept path, to keep the heat
run short); C3/C4 PASS (danger mode entered/exited cleanly, relay1 pulsed,
confirmed off); C25 PASS (backup export/import no-op round trip); C2
read-half PASS, C1/C9/C6/C16 NOT ATTEMPTED (time-boxed out, not declined).
C7/C8/C21/C22/C23/C24/C26 DECLINED despite nominal authorization -- C7/C8/
C21/C22 conflict with this run's own separately-stated "still forbidden:
reflashing / acknowledging crash reports"; C23/C24 have no safe restore
path available while reflashing is forbidden; C26 rests on a stale runbook
premise (the `cfg` partition is now mounted and populated with 7 files, not
"inert" as the runbook assumed when deferring it). Full detail, every
request/response, and heap/readiness before/after in
`docs/BENCH_TEST_LOG.md`'s 2026-09-21 "Backend Class C owner-authorized
rows (M18)" section. Owner authorization of C7/C8/C21/C22/C23/C24 as rows
stands; the DECLINE was this run's own narrower reading of a separate
"still forbidden" clause, not a rejection of the authorization -- **the
next run must be told these six rows are the carve-out** the "Authorize
all" instruction meant to exempt from that clause.

**2026-09-21 M18 backend Class C carve-out, OTA rows (C7/C8/C23/C24):** the
next run promised above -- explicitly told these four rows are the
carve-out, with the "no reflashing/OTA" and "never reset the Pico unbidden"
rules overridden by name for this run only. Built KilnCtrl.bin from a clean
worktree at origin/main (80239cd5) and ran all four: C7 (`ota_update_esp`)
FAILED with `ESP_ERR_OTA_PARTITION_CONFLICT` -- a genuine property of the
single-slot partition table (only one `ota_x` slot, board runs it, ESP-IDF
refuses self-overwrite), not a setup error; C8 (`ota_update_pico`) REFUSED
exactly as predicted, state 9 `REFUSED_RUNNING_IMAGE_OVERLAP`; C23
(`ota_rollback_esp`) FAILED 409 "no previous valid image" since C7 never
wrote one, `control_get_zones` confirmed gains stayed tuned; C24
(`ota_rollback_pico`) confirmed refused by unchanged `boot_id` (fire-and-
forget call). No board state changed by any of the four; no flash/restore
pass was needed. C21/C22/C26 remain the earlier run's carve-out, not
re-attempted here (out of this run's assigned scope). Full detail in
`docs/BENCH_TEST_LOG.md`'s "M18 backend Class C carve-out: OTA rows"
section.

**Superseded by the 2026-09-21 C26 redo / C6 run below the page-by-page
inventory's own C26/C6 rows** (`bx_flash_worker` stack fix `2d6347b0`
landed, C26 now PASSes clean and C6 was run and PASSed with one new
finding; see `docs/BENCH_TEST_LOG.md`'s "2026-09-21 C26 redo + C6" section).
Kept below for the panic's own root-cause record, which the fix history
still relies on.

**2026-09-21 C26/C6 attempt -- run halted after a board panic, C6 not
attempted:** C26 (`cfgfs_format`) run dry then confirmed: before 9 files,
after 0, `GET /api/cfgfs` verified mounted/0 files. A follow-up zone-PID
write to trigger a resave (same tuned z0 values, chosen since z0's
`tuning_valid` was already `no`) got no CONTROL reply within 3.0 s, and the
board was found rebooted with a fresh **unacknowledged** crash report
(`exc_task='bx_flash_worker' exc_cause_str='IllegalInstruction'
reset_reason='PANIC'`, uptime_s=13) -- not present at this run's baseline.
Crash was read but NOT acknowledged, per read-only rules. Post-reboot,
`cfg` had repopulated to 8 of the original 9 files (zones.json present at
900 B; `tz.dat` did not reappear) as a side effect of normal boot-time
config load, not a confirmed commit of the attempted PID write itself
(`dual_write.zones` still reads "not file-backed yet" before and after). No
trip, no firing, relays off throughout. **C6 (`factory_reset scope=wifi`)
was not attempted** -- running a second disruptive operation against a
board with a live, unreviewed panic was judged unsafe; none of C6's PC-side
pre-checks (STA credential env vars, adapter list, UART provision path)
were run since the row was never reached. Full detail, every request/
response and the exact heap/readiness before/after in
`docs/BENCH_TEST_LOG.md`'s "2026-09-21 C26 cfg-partition format (M18)"
section. Open question for the owner: whether the panic is attributable to
`cfgfs_format` immediately preceding a config write, to the PID write
alone, or coincidental -- not investigated further this run; recommend a
crash-report/coredump read before any repeat.

**2026-09-21 C6 driver-storage fix (1319e051) bench check:** ESP flashed to
`08f1c451` clean. Ran the real web-route `factory_reset(scope=wifi)` (challenge
+ HMAC-SHA256 keyed on the AP password, context `factory-reset`, matching
`app.js`'s `kcOtaAuthedFetch`). `nvs_list_keys(nvs, nvs.net80211)` before: 86
keys (`sta.ssid`, `sta.pswd`, `ap.ssid`, `ap.passwd`, `ap.pmk`, etc). The board
correctly dropped off the LAN afterward (no STA config to rejoin, confirmed by
`wifi_get_status()` over UART: `sta_connected=False ssid=''`) -- expected, but
it also means `GET /api/nvs/keys` cannot be polled in the erased window,
because that route is HTTP-only and the bench PC is not joined to the board's
own provisioning AP (`kilnCtl`) to reach `192.168.4.1`. Re-provisioned via
`wifi_add_network` over the UART link hub using the STA env vars; the board
rejoined at `192.168.1.156`. `nvs_list_keys` immediately after showed the
**same 86 keys again** -- but this is not evidence the fix failed: ESP-IDF's
driver repopulates `nvs.net80211` as an ordinary side effect of establishing
*any* new STA connection, including a fresh one from a legitimately empty
namespace, so a read taken only after rejoining cannot distinguish "the wipe
never happened" from "the wipe worked and this run's own re-provision just
refilled it." **Verdict: INCONCLUSIVE on this hardware run** -- the intended
before/after diff (audit section 5 step 6) requires observing the namespace
*between* the erase and the next STA join, which needs either the bench PC
joined to the board's AP to poll it at `192.168.4.1`, or a raw NVS partition
read, neither done this run. `kiln_auth` correctly refused: `nvs_list_keys`
against that namespace was rejected before ever issuing the HTTP request
(client-side refusal matching the route's own 403). Web auth confirmed still
ON post-reset via one authenticated `GET /api/status` (200). Full request/
response detail in `docs/BENCH_TEST_LOG.md`.

---

## Page-by-page inventory

Each row: Control -- Route(s) + method -- Auth tier -- MCP facade tool -- LCD
equivalent -- Testable on bench vs hardware-gated -- Result.

### `/` -- main dashboard (`main_page.html`, `app.js`, `nav.js`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Dashboard poll (page load) | `GET /api/status` | OPEN | `get_board_state` (superset) | Home page | Testable | PASS 2026-09-21 (A1); PASS 2026-09-21 (W2, first live web_commission_row.py run, see BENCH_TEST_LOG web-interface class section) |
| Start firing | `POST /api/profile_exec/start` | USER | `profiles_start` | Home "Start" / profile picker | Testable (bench load) | PASS 2026-09-21 (C11, owner-authorized: E-stop verified via C5, throwaway low-temp profile M18C_TEST started, "ok - firing #0"; stopped via profiles_stop() before hand-back) |
| Stop firing | `POST /api/profile_exec/stop` | SAFETY_REDUCE | `profiles_stop` | Home stop control | Testable | |
| Pause firing | `POST /api/profile_exec/pause` | USER | `profiles_pause` | -- | Testable | PASS 2026-09-21 (C12: paused, state=2, all zones relay=off duty=0.00 confirmed via profiles_get_exec_status) |
| Resume firing | `POST /api/profile_exec/resume` | USER | `profiles_resume` | -- | Testable | PASS 2026-09-21 (C12: resumed cleanly, "ok - resumed") |
| Dismiss last run (`ackLastRunBtn`) | `POST /api/profile_exec/ack_last_run` | USER | `profiles_ack_last_run` | -- | Testable | FAIL-EXPECTED 2026-09-21 (web_commission_row.py W3, live CDP: button not in DOM -- no last-run banner condition on this bench right now, JS only injects it when one exists; see BENCH_TEST_LOG) |
| Clear Trip (`clearTripBtn`) | `POST /api/safety/clear_trip` | ADMIN | `safety_clear_trip` | Safety page clear-trip | Testable (bench can trip S6a) | FAIL-EXPECTED 2026-09-21 (web_commission_row.py W4, live CDP: button not in DOM -- no trip latched, confirmed via safety_get_status beforehand; see BENCH_TEST_LOG) |
| PID popup: Use these / Apply (`pidPopupUseProposed`/`pidPopupApplyBtn`) | `POST /api/zones/pid` | ADMIN | `control_set_zone_pid` | -- | Testable | NOT RUN 2026-09-21 (W5 deliberately deferred: driver can't click `pidPopupUseProposed` first, cold Apply risked writing empty/stale gains; see BENCH_TEST_LOG) |
| Profile feasibility icon / popup Proceed anyway | reads `GET /api/profile_plan` | OPEN | none (facade has no feasibility-specific tool; use `get_board_state`/plan reads) | -- | Testable | PASS 2026-09-21 (A5) |
| Relay-life icon | reads status fields | OPEN | none | -- | Testable | |
| Theme toggle (`themeBtn`, every page) | client-side only, no route | -- | none | -- | Testable | PASS 2026-09-21 (W6, web_commission_row.py live run: `themeBtn` clicked on the real page, no error; client-side only, so there is no route to read back and nothing beyond "the click ran" is asserted -- see BENCH_TEST_LOG web-interface class section) |
| Live history chart/CSV | `GET /api/history.csv` | OPEN | none direct (`get_device_log`/`log_analyze` adjacent) | Home graph | Testable | PASS 2026-09-21 (A3) |
| Control status | `GET /api/control` | ADMIN | `control_get_zones` | -- | Testable | PASS 2026-09-21 (A8/A11) |
| Firing history | `GET /api/firing_history` | OPEN | none direct | -- | Testable | PASS 2026-09-21 (A4) |

### `/profiles` -- profile library (`profiles_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load / list | `GET /api/profiles`, `GET /api/profiles/builtin`, `GET /api/profiles/favorites` | USER | `profiles_list` | "profiles" / "profile_picker" / "profiles_builtin_list" pages | Testable | PASS 2026-09-21 (A9; web /profiles page load); PASS 2026-09-21 (W7, LIVE-CONFIRMED via web_commission_row.py's run_row_live() with one shared login cookie -- prior wording claimed PASS on the day the Row() was only unit-tested, not yet run against hardware; corrected here with the actual read-back: GET /api/profiles -> 200, M18C_TEST + 03DSFF among rows) |
| Save profile (`saveBtn`) | `POST /api/profile` | ADMIN | `profiles_save` | Profile builder review save | Testable | |
| New (`newBtn`) | client-side form reset, then Save | -- | -- | -- | Testable | |
| Import (`importBtn`) | `POST /api/profile/import` | ADMIN | none (facade has no import wrapper; use raw HTTP) | -- | Testable | |
| Export (`modeExportBtn` -> per-row) | `GET /api/profile/export` | USER | none | -- | Testable | |
| Delete (`modeDeleteBtn` -> per-row, `bulkActionBtn`) | `POST /api/profile/delete` | ADMIN | `profiles_delete` | -- | Testable | |
| Restore removed (`restoreBtn`) | `POST /api/profile/builtin/restore` | ADMIN | none direct | -- | Testable | |
| Hide builtin (row action, not a top button) | `POST /api/profile/builtin/hide` | ADMIN | none direct | -- | Testable | |
| Favorite toggle (row action) | `POST /api/profile/favorite` | ADMIN | none direct | -- | Testable | |
| Add segment / add out-of-band rule (`addSegBtn`/`addOoBtn`) | client-side, folded into Save | -- | -- | Profile builder segment page | Testable | |
| Bulk cancel (`bulkCancelBtn`) | client-side only | -- | -- | -- | Testable | |
| Profile detail read | `GET /api/profile` | USER | `profiles_get` | "profile_detail"/"profile_segments" pages | Testable | PASS 2026-09-21 (A10) |

### `/live_profile` -- live in-run profile edit (`live_profile_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load | `GET /api/profile/live` | ADMIN | `profile_live_get` | none (LCD has no live-edit page) | Testable only while a firing is running | PASS 2026-09-21 (A45; web /live_profile page load) |
| Fork the running profile (`forkBtn`) | `POST /api/profile/live/fork` | ADMIN | `profile_live_fork` | -- | Testable | PASS 2026-09-21 (C13: origin_id=0, working_id=100) |
| Save changes (`saveBtn`) | `POST /api/profile/live` | ADMIN | `profile_live_edit` | -- | Testable | PASS 2026-09-21 (C14: edited working copy's dwell_min 2->3, accepted with 6 informational ramp-rate warnings, no error) |
| Save as new / Overwrite original / Discard (`saveAsBtn`/`overwriteBtn`/`discardBtn`) | `POST /api/profile/live/decide` | ADMIN | `profile_live_decide` | -- | Testable | PASS 2026-09-21 (C15: action=discard, ok; working copy #100 discarded, origin profile #0 untouched; firing then stopped via profiles_stop(), relays confirmed off) |
| Reload from board (`reloadBtn`) | `GET /api/profile/live` | ADMIN | `profile_live_get` | -- | Testable | |

### `/settings/zones` -- zones & PID (`zones_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load / zone config | `GET /api/zones`, `GET /api/zones_diag` | ADMIN | `control_get_zones` | -- (no LCD zones editor) | Testable | PASS 2026-09-21 (A8/A11; web /settings/zones page load); PASS 2026-09-21 (W15, web_commission_row.py live run) |
| Save (`saveBtn`) | `POST /api/zones` | ADMIN | none direct (raw HTTP; `control_set_zone_pid`/`control_set_zone_model` cover the PID/model sub-fields) | -- | Testable | PASS 2026-09-21 (B4/B6: form shape established from source and exercised on hardware -- GET-merge-POST via `zones_http_client.py`, exact shape recorded in `docs/COMMISSIONING_BACKEND_RUNBOOK.md`'s B4 row; round-trip test changed only `zones[0].name`, confirmed byte-identical elsewhere including after restore, `zones_config_valid`/`load_fault` unchanged, no reboot -- see `docs/BENCH_TEST_LOG.md`'s dated M18 B6 section); PASS 2026-09-21 (W16, live CDP click on the real `saveBtn` via web_commission_row.py -- no form-fill primitive yet so this resubmitted unchanged values, `GET /api/zones` 200; see BENCH_TEST_LOG web-interface class section) |
| PID save | `POST /api/zones/pid` | ADMIN | `control_set_zone_pid` | -- | Testable | PASS 2026-09-21 (B3, flagged anomaly: writing identical gains still invalidated `tuning_valid`, see log; B10 same-value round trip confirmed by read-back) |
| Measure Normal Current (`sweepStartBtn`) | `POST /api/zones/current_sweep/start` | ADMIN | `zone_current_sweep_start` | -- | **Hardware-gated**: energizes each zone's relay in turn to measure real amp draw -- meaningful only with a real heating-element load; bench's 4 W fixture reads near-zero/noise | PASS 2026-09-21 (C1/C9: sweep started, polled to completion, drained; all zones reported "unmeasured" as expected -- fixture current sits below the 0.045A noise floor) |
| Abort sweep (`sweepAbortBtn`) | `POST /api/zones/current_sweep/abort` | SAFETY_REDUCE | `zone_current_sweep_abort` | -- | Testable (abort path itself, even if the sweep's numbers are meaningless on bench) | |
| Sweep status poll | `GET /api/zones/current_sweep/status` | ADMIN | `zone_current_sweep_status` | -- | Testable | PASS 2026-09-21 (A12) |
| CT channel map read | `GET /api/zones/ct_channel_map` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (A13) |
| Recommend (`tuningRecGoBtn`) | `GET /api/tuning_recommendations` | ADMIN | none direct | -- | **Hardware-gated**: recommendations are derived from the current-sweep result above | PASS 2026-09-21 (A17 read; C2 read-half PASS -- route returns a body cleanly; acting on the recommendation still needs C1/C9's sweep, NOT ATTEMPTED this session) |
| Start step test (`atStartBtn`) | `POST /api/autotune/start` | ADMIN | `autotune_start` | -- | Testable on bench (closed-loop step response exists even at 4 W, though gains found are not representative of a real kiln -- see `project_bench_identification_limits`) | PARTIAL 2026-09-21 (C10: ran 180s settle + ~190s real closed-loop stepping, temperature rise ~25.4C->~32.6C observed, then deliberately autotune_abort()'d rather than run the full 4-hour max-duration step-identify phase -- mechanism confirmed working, full accept path not exercised) |
| Abort (`atAbortBtn`) | `POST /api/autotune/abort` | SAFETY_REDUCE | `autotune_abort` | -- | Testable | |
| Accept proposed gains (`atAcceptBtn`) | `POST /api/autotune/accept` | ADMIN | `autotune_accept` | -- | Testable | NOT ATTEMPTED 2026-09-21 (C10: autotune was deliberately aborted before reaching accept, to keep the heat run short, see Start step test row) |
| Autotune status/matrix | `GET /api/autotune`, `GET /api/autotune/matrix` | ADMIN | `autotune_get_status` | -- | Testable | PASS 2026-09-21 (A14) |
| Autotune trace CSV | `GET /api/autotune/trace.csv` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (A15) |
| Adaptive-tune revert (per-zone) | `POST /api/adaptive_tune/revert` | ADMIN | `adaptive_tune_revert` | -- | Testable | |
| Adaptive-tune status | `GET /api/adaptive_tune` | ADMIN | `adaptive_tune_get_status` | -- | Testable | PASS 2026-09-21 (A16) |
| Ramp-assist toggle (also on diagnostics page) | `GET`/`POST /api/ramp_assist` | ADMIN | `ramp_assist_get_enabled`/`ramp_assist_set_enabled` | -- | Testable | PASS 2026-09-21 (A18 GET; B7 POST disable/re-enable round trip, confirm=True required) |

### `/settings/safety` -- safety config page (`safety_config_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load | `GET /api/safety/rate_guard/auto` | ADMIN | `safety_get_rate_guard` | Home safety/temperature page has read-only status only | Testable | PASS 2026-09-21 (A19; web /settings/safety page load); PASS 2026-09-21 (W21, LIVE-CONFIRMED via web_commission_row.py's run_row_live() with one shared login cookie -- prior wording claimed PASS on the day the Row() was only unit-tested, not yet run against hardware; corrected here with the actual read-back: GET /api/safety/rate_guard/auto -> 200 (no zone identified yet, expected)) |
| Save (`save`) | `POST /api/safety/rate_guard/auto` | ADMIN | `safety_set_rate_guard` | -- | Testable | BLOCKED 2026-09-21 (B8: relay ARMED, write staged not committed; would need an unauthorized Pico reset to open the write-grace window, value confirmed unchanged); NOTE 2026-09-21: current `safety_config_page.html` source's `#save` button actually POSTs the whole page (incl. `#pcLink`, `pc_link_abort_silence_ms`) to `/api/zones`, not this route -- this table's endpoint citation is stale, kept here for history; wired as W22 in `web_commission_row.py` against the real `/api/zones` shape (read, fill `#pcLink`, Save, confirm, restore, confirm) -- **PASS 2026-09-21, LIVE-RUN at board commit 33124aa8** (`run_row_live("W22", ...)`: set `#pcLink`=54000, GET /api/zones confirmed `pc_link_abort_silence_ms`=54000, restored to original `0` via a second Save, GET /api/zones confirmed restored; guard fields `thermo_count`/`relay_count`/`max_simultaneous_relays` confirmed unchanged throughout) |

### `/safety` -- safety status (`safety_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load | reads `GET /api/status` safety fields | OPEN | `safety_get_status` | Home page safety banner | Testable | PASS 2026-09-21 (A20; web /safety page load); PASS 2026-09-21 (W23, LIVE-CONFIRMED via web_commission_row.py's run_row_live() with one shared login cookie -- prior wording claimed PASS on the day the Row() was only unit-tested, not yet run against hardware; corrected here with the actual read-back: GET /api/status -> 200, relays/relay_life fields present) |
| Clear latched trip (`clearTripBtn`) + confirm dialog (`confirmYes`/`confirmNo`) | `POST /api/safety/clear_trip` | ADMIN | `safety_clear_trip` | -- | Testable (E-stop jumper fitted means S3/estop trips are not reachable this way on bench; S6a mainFault from a dual reflash is, per CLAUDE.md) | |

### `/safety/commissioning` -- guarded-value commissioning wizard (`safety_commissioning_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load | `GET /api/safety/commissioning` | ADMIN | `safety_get_commissioning` | -- | Testable | PASS 2026-09-21 (A21; web /safety/commissioning page load); PASS 2026-09-21 (W25, LIVE-CONFIRMED via web_commission_row.py's run_row_live() with one shared login cookie -- prior wording claimed PASS on the day the Row() was only unit-tested, not yet run against hardware; corrected here with the actual read-back: GET /api/safety/commissioning -> 200, commissioned=true, live_config_crc==cached_config_crc) |
| Wizard Start/Next/Back (`gStartBtn`, `gGoto`) | client-side only | -- | -- | -- | Testable | |
| Stage & commit (`gCommitBtn`, `saveBtn`) | `POST /api/safety/commissioning` | ADMIN | `safety_set_commissioning_fields` | -- | Testable | PASS 2026-09-21 (C17/C19: this session was owner-authorized to reset the Pico to open the GRACE window; ct_cal commit succeeded with no-op values ch=2,a_fs=1,zero_mv=71 -- persisted true -- on the second reset after the first window expired between rows) |
| Apply test preset (`benchBtn`, dev-only, hidden by default) | `POST /api/safety/commissioning/bench_preset` | ADMIN | none direct | -- | Testable -- this is literally the bench-values preset button | BLOCKED 2026-09-21 (C18: 404 -- GET /api/safety/commissioning shows dev_tools_enabled:false; handler compiled out behind CONFIG_KILNCTL_DEV_TOOLS on this board's build, a build-config limitation not a GRACE-window issue) |
| Relay-type field (submitted with commit) | `POST /api/safety/commissioning/relay_type` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (C20: field name is `type`, not `relay_type`; posted type=contactor matching baseline, no-op by construction) |
| CT calibration Apply (`.ct-cal-apply`) | `POST /api/safety/commissioning/ct_cal` | ADMIN | `safety_set_commissioning_fields`(fields incl. ct_cal) / raw | -- | Testable (bench CT calibration already closed, see `project_ct_calibration_closed_and_presence_branch`) | PASS 2026-09-21 (C19: see Stage & commit row above -- committed no-op values during the reopened GRACE window) |
| CT Auto-zero (`.ct-cal-auto-zero`) | `POST /api/safety/commissioning/ct_auto_zero` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (C19: field name is `channel`, not `ch`; applied ESP-side, no grace-window dependency) |
| CT trim Apply (`.ct-trim-apply`) | `POST /api/safety/commissioning/ct_trim` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (C19: ch=2,trim_offset_a=0,trim_gain=1, applied ESP-side only, no grace-window dependency) |

### `/diagnostics` -- diagnostics (`diagnostics_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load / thermo faults | `GET /api/thermo/faults` | ADMIN | `thermo_read_faults` | "diagnostics" LCD page | Testable | PASS 2026-09-21 (A22; web /diagnostics page load); PASS 2026-09-21 (W28, web_commission_row.py live run) |
| Crash report banner + Acknowledge (`crashAckBtn`) | `GET /api/crash_report`, `POST /api/crash_report/ack` | ADMIN | `crash_report_ack` | -- | Testable | N/A 2026-09-21 (C21/C22: no crash report pending at session start, correctly not exercised) |
| Clear crash log (`crashClearBtn`) | `POST /api/crash_report/clear` | ADMIN | none direct | -- | Testable | |
| Watchdog PANIC toggle (`wdPanicToggleBtn`) | `GET`/`POST /api/watchdog_cfg` | ADMIN | `get_watchdog_panic_disabled`/`set_watchdog_panic_disabled` | -- | Testable but disruptive -- disabling PANIC masks real overflow-class crashes; use deliberately | PASS 2026-09-21 (A24 (GET only; toggle not exercised)); PASS 2026-09-21 (W30, live re-run confirming the `d7c1ed34` fix: row declares `expect_post="/api/watchdog_cfg"`, `_web_commission_cdp.mjs`'s Network.* wait (10s bound, 5s network-quiet fallback) let the POST land before teardown this time -- `panic_disabled` toggled true then read back true via both the page and `get_watchdog_panic_disabled`, then restored to false and re-confirmed false by both. Prior FAIL (race + missing dialog handling) is resolved) |
| Ramp-assist toggle (`rampAssistToggleBtn`) | `GET`/`POST /api/ramp_assist` | ADMIN | `ramp_assist_*` | -- | Testable | PASS 2026-09-21 (W31, live CDP click via web_commission_row.py, same shared login cookie as the other 10 rows this pass: read GET /api/ramp_assist before (enabled=true), clicked the toggle, POST /api/ramp_assist landed, read-back showed enabled=false; restored via a direct POST enabled=1 and re-confirmed enabled=true by GET) |
| Enter Danger Mode (`dangerEnterBtn`) | `POST /api/diagnostics/danger/enable` | ADMIN | none direct | -- | **Hardware-gated**: explicit purpose is driving relays outside the normal safety-gated path -- exercise only with the bench fixture's 4 W load, never near a real element |
| Per-relay danger control (`dangerRelayBtn<N>`, `dangerFiringBtn`) | `POST /api/diagnostics/danger/relay` | ADMIN | none direct (`io_set_relay`/`io_set_relay_mask` are the lower-level equivalents) | -- | **Hardware-gated** (same reason) | |
| Exit Danger Mode now (`dangerExitBtn`) | `POST /api/diagnostics/danger/stop` | SAFETY_REDUCE | none direct (`io_all_relays_off` is the safe fallback) | -- | Testable (the exit path itself) | |
| Danger status poll | `GET /api/diagnostics/danger` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (A25) |
| E-stop verify | `POST /api/estop/verify` | ADMIN | none direct | -- | **Hardware-gated**: bench E-stop jumper is fitted (NOT asserted, per `project_estop_jumper_is_fitted`) -- verifying the real switch needs the jumper removed and a physical actuation |
| Relay cycle counters: reset/restore per-relay (`relay-reset-btn`) | `POST /api/relay_cycles/reset`, `POST /api/relay_cycles/restore` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (B12, prior flagged anomaly re-checked and no longer reproduces: `/api/status` `relay_life` now correctly shows 3802/4469 matching internal counts, see log) |
| Dual-write window record restore (`dwwRecordRestoreBtn`) | `POST /api/dualwrite_window/restore_verified` | ADMIN | none direct | -- | Testable (superseded 2026-09-21: `cfg` is now mounted and populated on this bench per `GET /api/cfgfs`, per `project_cfg_partition_and_user_data_move` -- verify against the live file state, not the old unformatted assumption) | PASS 2026-09-21 (B13) |
| Dual-write window status | `GET /api/dualwrite_window` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (A26) |
| lwIP stats | `GET /api/debug/lwip_stats` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (A27) |
| Timing diagnostics | `GET /api/diagnostics/timing` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (A28) |
| SaftyFW stack margin | `GET /api/saftyfw_stack_margin` | ADMIN | none direct (`get_stack_margin` covers the ESP side only) | -- | Testable | PASS 2026-09-21 (A29) |
| Coredump info/chunk (implicit, feeds crash report tooling) | `GET /api/coredump/info`, `GET /api/coredump/chunk` | ADMIN | `read_esp_coredump` | -- | Testable | PASS 2026-09-21 (A30 (read_esp_coredump succeeded end to end, symbolized)) |
| cfg filesystem status / file read+write | `GET /api/cfgfs`, `GET`/`POST /api/cfgfs/file` | ADMIN | `get_cfgfs_status` (status only; file get/post has no wrapper) | -- | Testable | PASS 2026-09-21 (A31/A48/C28-read: status+file read confirmed, baseline bytes `03 00 00 00 01`); C28-write BLOCKED (Claude Code auto-mode permission classifier refused the POST before it reached the board -- not a board/firmware finding) |

### `/settings` -- settings hub (`settings_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Reboot both processors, no config change (`swResetBtn`) | `POST /api/sw_reset` | ADMIN | `sw_reset_esp` | -- | Testable (watch for the expected S6a trip during a dual reflash, per CLAUDE.md) | PASS 2026-09-21 (B9/B17: env-fallback password, no S6a trip this time -- Pico link never dropped, benign deviation, see log) |
| Reset Wi-Fi only (`data-scope="wifi"`) | `POST /api/factory_reset` (scope param) | ADMIN | none direct (`factory_default_then_load_preset`/`wifi_forget` are the nearest facade equivalents) | -- | Testable | PASS 2026-09-21 (C26-redo run, owner-authorized by name for this run only: driven via raw UART `SYSTEM_CMD_FACTORY_RESET(scope=wifi)`, not the HTTP route, per this run's re-provisioning-path choice -- see `docs/BENCH_TEST_LOG.md`. `wifi_nvs` erase confirmed, board re-provisioned from `KILNCTL_STA_SSID`/`KILNCTL_STA_PASSWORD` over UART, reconnected at `192.168.1.156`, web auth confirmed still ON via one authenticated GET. Real finding, corrected 2026-09-21 per `docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md`: the observed transient was most likely the pre-reboot instance of `reboot_task`'s own 500 ms delay before `esp_restart()`, not driver auto-reconnect on old credentials -- but the retention gap the audit identifies (ESP-IDF's own `WIFI_STORAGE_FLASH`-persisted STA config in `nvs.net80211`, set by `esp_wifi_set_config()` in `wifi_prov_link.c`, independent of and never cleared by the app's `wifi_nvs` partition erase) is real, and is fixed by this commit: `wifi_prov.c` now sets `WIFI_STORAGE_RAM` at boot so no new copy reaches flash, and `factory_reset.c`'s `execute_scope_job()` now calls `esp_wifi_restore()` for the wifi/all scopes to clear what older firmware already wrote). **Web-route recheck, 2026-09-21, ESP at `33124aa8`:** PASS -- this time driven through the actual `POST /api/factory_reset` route (challenge/HMAC via `X-Ota-Mac`, same mechanism as the settings page's `kcOtaAuthedFetch`, `context="factory-reset"`), not raw UART. `wifi_nvs` erase confirmed (`wifi_get_status` showed `ssid=''`, `sta_connected=False` immediately after, old LAN address unreachable over HTTP). Board did not re-associate with the home STA network on its own. Re-provisioned via `wifi_add_network` (UART, through the shared link hub) from the same env-var credentials; board reconnected at `192.168.1.156`, confirmed via `wifi_get_status` (`state=3`, `sta_connected=True`) and an authenticated `board_page_structure` fetch (web auth still ON, session/credential still valid). No crash, no S6a trip. One boot-log line noted and judged benign, not a new finding: `esp_ota_ops: Running firmware is factory` / `esp_ota_mark_app_valid_cancel_rollback failed: ESP_FAIL` on the post-reset boot -- matches `main_network_http.c`'s documented "FACTORY VS. OTA-SLOT BOOTS" bench-flash comment, correct commit/build confirmed running (`fw_version.commit=33124aa8`), zones/PID config untouched (only `wifi_nvs` in scope) |
| Reset kiln config only (`data-scope="kiln"`) | `POST /api/factory_reset` | ADMIN | `factory_default_then_load_preset` | -- | Testable | NOT ATTEMPTED 2026-09-21 (`kiln`/`profiles`/`all` scopes not attempted, out of this session's scope -- only `wifi` was owner-authorized) |
| Reset fire profiles only (`data-scope="profiles"`) | `POST /api/factory_reset` | ADMIN | none direct | -- | Testable | |
| Factory default -- erase everything (`data-scope="all"`) | `POST /api/factory_reset` | ADMIN | `factory_default_then_load_preset`(scope=all) | -- | Testable, but destructive -- re-provision Wi-Fi/credentials afterward | |
| Format cfg partition (`cfgFsFormatConfirmBtn`) | `GET /api/cfgfs/format_pending`, `POST /api/cfgfs/format_confirm` | ADMIN | `cfgfs_format` | -- | Testable (superseded 2026-09-21: `cfg` is mounted and populated with 7-8 files on this bench, per `GET /api/cfgfs` -- this is now a real destructive format, not a no-op) | PASS 2026-09-21 (C26-redo, ESP `1045e542` with the `bx_flash_worker` stack fix, `2d6347b0`: dry run then confirmed format to 0 files, then a same-value `control_set_zone_pid(zone=0)` resave -- no reboot, no crash banner, `zones.json` reappeared at 900 B. Supersedes both the earlier `derive_mac()` allow-list BLOCKED note and the 2026-09-21 panic run below) |
| Timezone save | `POST /api/settings/tz` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (C27: posted current value "UTC0" (read via /api/status's time_tz, no dedicated GET route exists) back unchanged, confirmed by read-back) |

### `/settings/display` -- display settings (`settings_display_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load | `GET /api/settings/display_power` | ADMIN | none direct | LCD screen-idle/timeout behavior (`screen_idle.c`) | Testable | PASS 2026-09-21 (A41; web /settings/display page load); PASS 2026-09-21 (W37, LIVE-CONFIRMED via web_commission_row.py's run_row_live() with one shared login cookie -- prior wording claimed PASS on the day the Row() was only unit-tested, not yet run against hardware; corrected here with the actual read-back: GET /api/settings/display_power -> 200, brightness_percent=100) |
| Save (`kcDpSave`) | `POST /api/settings/display_power` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (B24); wired as W38 in `web_commission_row.py` (read `brightness_percent`, fill `#kcDpBrightness`, Save, confirm, restore, confirm) -- **PASS 2026-09-21, LIVE-RUN at board commit 33124aa8** (`run_row_live("W38", ...)`: set `#kcDpBrightness`=45, GET /api/settings/display_power confirmed `brightness_percent`=45, restored to original `100` via a second Save, confirmed restored; guard fields `timeout_setting`/`keep_on_while_firing`/`display_on_error` confirmed unchanged throughout) |
| Unit preference (temp C/F, submitted from main page, not this page) | `POST /api/unit_pref` | ADMIN | none direct | Home page unit toggle | Testable | PASS 2026-09-21 (B25) |

### `/settings/security` -- credentials & auth policy (`security_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load | `GET /api/auth/config` | ADMIN | none direct | -- (no LCD credentials editor) | Testable | PASS 2026-09-21 (A39; web /settings/security page load); PASS 2026-09-21 (W39, LIVE-CONFIRMED via web_commission_row.py's run_row_live() with one shared login cookie -- prior wording claimed PASS on the day the Row() was only unit-tested, not yet run against hardware; corrected here with the actual read-back: GET /api/auth/config -> 200, web_enabled=true, admin_username=bench) |
| Save administrator/user password, admin/user PIN, policy (`kcSecAdminPwSave` etc.), Clear login credentials (`kcSecClearCreds`) | `POST /api/auth/security` | ADMIN | none direct | -- | Testable -- see `project_web_auth_verified_and_blinds_pctools`: enabling auth policy blinds PcTools clients until they log in, plan the session accordingly | PASS 2026-09-21 (B26, unmodified set_policy round trip; C16, `cmd=set_web_password` re-set to the same env-var value via raw form POST -- `web_auth_setup` MCP tool has no "re-affirm current credential" path, worked around, see log) |
| Bootstrap admin password (first-run, `login_page.html`'s "Set password" form) | `POST /api/auth/bootstrap_password` | ADMIN_BOOTSTRAP | none direct | -- | Testable | |
| Log in (`login_page.html`) | `POST /api/auth/login` | OPEN | none direct | -- | Testable; see `project_login_latency_measured_4s` and `project_owner_decisions_2026_09_21_login` for expected latency/lockout behavior -- never iterate logins | PASS 2026-09-21 (web UI sweep, real login POST /api/auth/login 200); PARTIAL 2026-09-21 (W1, first web_commission_row.py live run: the driver's own `POST /api/auth/login` succeeded (role=admin) and `GET /api/auth/session` read it back 200, and that one session was reused for the whole class -- but the login **form** itself was not exercised: the browser is handed the cookie before navigation and `login-form` is the form element, not its submit button, so clicking it submits nothing. The UI control remains uncovered -- see BENCH_TEST_LOG) |
| Session status poll / extend | `GET /api/auth/session`, `POST /api/auth/session/extend` | OPEN / USER | none direct | -- | Testable | PASS 2026-09-21 (A40 poll; B29 extend) |

### `/settings/kiln_configs` -- config presets (`kiln_configs_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load / list | `GET /api/kiln_configs` | USER | `list_config_presets` (local presets; board-stored slots have no direct wrapper) | -- | Testable | PASS 2026-09-21 (web /settings/kiln_configs page load); PASS 2026-09-21 (W41, LIVE-CONFIRMED via web_commission_row.py's run_row_live() with one shared login cookie -- prior wording claimed PASS on the day the Row() was only unit-tested, not yet run against hardware; corrected here with the actual read-back: GET /api/kiln_configs -> 200, active_id=1) |
| Apply (`kcApplyBtn`) | `POST /api/kiln_configs/apply` | ADMIN | `load_config_preset`/`capability_preflight_check` (facade presets are file-based, not identical to these board-stored slots) | -- | Testable | N-A 2026-09-21 (B18: board reports zero existing config slots to round-trip against) |
| Apply status poll | `GET /api/kiln_configs/apply_status` | USER | none direct | -- | Testable | PASS 2026-09-21 (A33) |
| Save as new (`kcSaveNewBtn`) | `POST /api/kiln_configs/save` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (B19, after `kiln_configs_quarantine_clear`; form-urlencoded body, field `name`); Save-as-new + Delete selected wired together as W42 in `web_commission_row.py` (`_run_kiln_config_create_delete`: create a uniquely-named throwaway config, confirm via GET, select it in `#kilnConfigSelect` and delete it, confirm removed) -- **FAIL 2026-09-21, LIVE-RUN at board commit 33124aa8**: create step (fill `#kcSaveNewName`, click `#kcSaveNewBtn`) reported CDP success, but the follow-up `GET /api/kiln_configs` never listed a config named `__kc_web_commission_test_<ts>__` -- exact decisive line: "W42 FAIL: no config named '__kc_web_commission_test_1790032582__' found in /api/kiln_configs after create -- write did not land". Delete step never ran (function stops before selecting/deleting on a failed create, per its own guard), so no throwaway config was ever created or left behind; `GET /api/kiln_configs` before and after the attempt returned the same set, confirming the list is unchanged. **Root-caused `docs/audits/w42_kiln_config_create_2026-09-21.md` (verdict (c), runner-side fixture defect, no firmware defect):** the generated name was 37 chars (the audit doc says 38; `len("__kc_web_commission_test_")` is 25, not 26 -- the off-by-one does not change the verdict), over the firmware's `KILN_CFG_NAME_MAX_LEN` (23, `kiln_cfg_store.h`), so the `save` POST was correctly rejected 400 and the runner's success check never inspected the POST status, only the later GET, producing the generic message above. Fixed in `web_commission_row.py`: the generated name is now `kc_test_<ts>` (18 chars at today's epoch, asserted against a mirrored `_KILN_CFG_NAME_MAX_LEN` constant by driving the real generator), and the create step now parses the CDP driver's `--expect-post` result and reports a non-200 or network-failed create status directly instead of falling through to "write did not land". The delete step is NOT status-checked the same way (known, bounded gap: a rejected delete is still caught by the post-delete read-back, which fails with "LEFT ON BOARD", just without the POST status in the message). **Result stays FAIL** until re-run live against the board; this row is not yet re-verified on hardware |
| Overwrite selected (`kcOverwriteBtn`) | `POST /api/kiln_configs/save` (existing id) | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (B19, id field variant) |
| Clone selected (`kcCloneBtn`) | `POST /api/kiln_configs/clone` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (B20; fields `id`+`name`) |
| Rename (`kcRenameBtn`) | `POST /api/kiln_configs/rename` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (B21; fields `id`+`name`) |
| Delete selected (`kcDeleteBtn`) | `POST /api/kiln_configs/delete` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (B22: active-config slot correctly refused deletion with a named 400, own interlock working; left slot on board rather than switching config to force it) |
| Download selected (`kcDownloadBtn`) | `GET /api/kiln_configs/export` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (B23, export/import round trip) |
| Upload/import (`kcUploadBtn`) | `POST /api/kiln_configs/import` | ADMIN | `convert_config` (offline conversion only, not the upload itself) | -- | Testable | PASS 2026-09-21 (B23, JSON package body, re-imported exported config under a new name) |

### `/settings/backup` -- backup/restore (`backup_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load / Export (implicit download link) | `GET /api/backup/export` | ADMIN | none direct | -- | Testable -- see `project_backup_round_trip_coverage`, only the Wi-Fi password is irreducible across a round trip | PASS 2026-09-21 (A34 (export fetched, not re-imported); web /settings/backup page load); PASS 2026-09-21 (W43, LIVE-CONFIRMED via web_commission_row.py's run_row_live() with one shared login cookie -- prior wording claimed PASS on the day the Row() was only unit-tested, not yet run against hardware; corrected here with the actual read-back: GET /api/backup/export -> 200, kind=kilnctl_backup) |
| Restore from file (`restoreBtn`) | `POST /api/backup/import` | ADMIN | none direct | -- | Testable | |

### `/ota` -- firmware update (`ota_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load / interlock status | `GET /api/ota/interlock`, `GET /api/ota/esp/status`, `GET /api/ota/pico/status` | ADMIN / OPEN(esp status) / ADMIN(pico status) | `ota_status` | -- (no LCD OTA UI) | Testable | PASS 2026-09-21 (A35; web /ota page load); PASS 2026-09-21 (W46, LIVE-CONFIRMED via web_commission_row.py's run_row_live() with one shared login cookie -- prior wording claimed PASS on the day the Row() was only unit-tested, not yet run against hardware; corrected here with the actual read-back: GET /api/ota/interlock -> 200, ok=true) |
| Update ESP (`espUpdateBtn`) | `GET /api/ota/challenge` then `POST /api/ota/esp` | OPEN then ADMIN | `ota_get_challenge`, `ota_update_esp` | -- | Testable, but CLAUDE.md's sanctioned path for a real flash is `flash_firmware()` (JTAG/OpenOCD) -- this route is the Wi-Fi OTA path, a different mechanism, both worth exercising | FAIL 2026-09-21 (C7, M18 carve-out: `esp_ota_begin failed: ESP_ERR_OTA_PARTITION_CONFLICT`, HTTP 500 -- the single-slot partition table has only one `ota_x` slot (`app`), and the board is running it, so `esp_ota_get_next_update_partition()` returns the running partition itself and ESP-IDF refuses to begin; every Wi-Fi OTA will hit this while running `app`. No reboot, `fw_build` unchanged) |
| Roll back ESP (`espRollbackBtn`) | `POST /api/ota/esp/rollback` | ADMIN | `ota_rollback_esp` | -- | Testable; mind the `zones_cfg` schema-bump rollback hazard in CLAUDE.md | FAIL 2026-09-21 (C23, M18 carve-out: HTTP 409 "no previous valid image to roll back to", expected since C7 never wrote an image; `control_get_zones` read back unchanged/tuned) |
| Exit recovery mode & reboot now (`recoveryExitBtn`) | `POST /api/ota/esp/recovery_exit` | ADMIN | `ota_recovery_exit_esp` | -- | Testable only while actually in recovery mode | |
| boot_guard_reset (fired automatically by `flash_firmware()`, no dedicated button) | `POST /api/ota/esp/boot_guard_reset` | ADMIN | wired into `flash_firmware()`'s `reset_boot_guard` path, no standalone MCP tool | -- | Testable | BLOCKED 2026-09-21 (B32: only reachable via `flash_firmware()`'s post-verify path; no reflash authorized this session) |
| boot_guard status | `GET /api/boot_guard` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (A36) |
| Update Pico (`picoUpdateBtn`) | `POST /api/ota/pico` | ADMIN | `ota_update_pico` | -- | Testable; watch for the erase-watchdog-reset class, fixed per `project_pico_ota_erase_watchdog_resets_safety_processor` | REFUSED (expected) 2026-09-21 (C8, M18 carve-out: staged/relayed 119428 B ok, then `ota_status` reported `SAFETY_LINK_UPDATE_STATE_REFUSED_RUNNING_IMAGE_OVERLAP` (state 9) -- bench Pico is a flat image, no bootloader slots; no trip, no reboot) |
| Roll back Pico (`picoRollbackBtn`) | `POST /api/ota/pico/rollback` | ADMIN | `ota_rollback_pico` | -- | Testable | REFUSED (expected) 2026-09-21 (C24, M18 carve-out: fire-and-forget call, confirmed refused by absence of effect -- `boot_id` unchanged before/after, link stayed up, no trip; consistent with SaftyFW's own `KILNLINK_ROLLBACK_RESULT_REASON_NO_METADATA` refusal on a flat/no-bootloader image) |
| Pico rollback status | `GET /api/ota/pico/rollback/status` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (A38) |
| Partition table read (support for above, no button) | `GET /api/partitions` | ADMIN | `debug_check_partition_table` | -- | Testable | PASS 2026-09-21 (A37) |

### `/readiness` -- pre-firing readiness (`readiness_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load (no buttons besides theme) | `GET /api/readiness` | OPEN | none direct (`capability_preflight_check` is the nearest facade equivalent, board-agnostic) | -- | Testable | PASS 2026-09-21 (A2; web /readiness page load); PASS 2026-09-21 (W48, web_commission_row.py live run) |

### `/setup` -- first-run setup wizard (`setup_wizard_page.html`)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load / progress | `GET /api/setup/progress` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (A42; web /setup page load); PASS 2026-09-21 (W49, LIVE-CONFIRMED via web_commission_row.py's run_row_live() with one shared login cookie -- prior wording claimed PASS on the day the Row() was only unit-tested, not yet run against hardware; corrected here with the actual read-back: GET /api/setup/progress page load, no verify_endpoint by design) |
| Resume where left off (`resumeBtn`) / Refresh (`loadAll`) / step navigation (`stepgo`) | client-side + `GET /api/setup/progress` | ADMIN | -- | -- | Testable | |
| Step Save & mark done buttons (`step0Continue`..`step6Save`) | `POST /api/setup/progress` (+ underlying config routes: zones, safety, security depending on step) | ADMIN | matches whichever underlying group the step edits | -- | Testable | |
| Apply channel/relay count (`s2rebuild`/`s5rebuild`) | `POST /api/zones` (channel/relay count fields) | ADMIN | none direct | -- | Testable | |
| CT verification sweep (`step8Start`/`step8Abort`/`step8Skip`/`step8SkipLater`) | `POST /api/zones/current_sweep/start`/`abort` | ADMIN / SAFETY_REDUCE | `zone_current_sweep_start`/`_abort` | -- | **Hardware-gated** start (same as zones-page sweep); abort/skip testable | |
| CT calibration Apply (`.ct-cal-apply`) | `POST /api/safety/commissioning/ct_cal` | ADMIN | `safety_set_commissioning_fields` | -- | Testable | |
| Save gains per zone (`s10save`) | `POST /api/zones/pid` | ADMIN | `control_set_zone_pid` | -- | Testable | |
| Save credentials & policy (`step11Save`) / Skip (`step11Skip`) | `POST /api/auth/security` / `POST /api/setup/progress` | ADMIN | none direct | -- | Testable | |
| Stage & commit (`step7Save`) | `POST /api/safety/commissioning` | ADMIN | `safety_set_commissioning_fields` | -- | Testable | |

### `/`, `/wifi`, `/status`, `/scan`, `/provision`, `/networks`, `/forget`, `/ip_config` -- Wi-Fi provisioning (`wifi_provision_page.html`, AP-mode index)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| Page load (AP-mode captive index) | `GET /` (AP context), `GET /wifi` | OPEN | `wifi_get_status` | "network" LCD page | Testable | PASS 2026-09-21 (web /wifi page load; A43 wifi_get_status) |
| Home Wi-Fi / Access Point mode (`modeHomeBtn`/`modeApBtn`) | implicit via `POST /provision` or mode switch | ADMIN | `wifi_set_mode` | "network_manage" LCD page | Testable | |
| Scan (`scanBtn`) | `GET /scan` | OPEN | `wifi_scan` | -- | Testable | |
| Connect (`connectSubmitBtn`) | `POST /provision` | ADMIN | `wifi_add_network` | -- | Testable | PASS 2026-09-21 (B31, throwaway SSID) |
| Cancel (`connectCancelBtn`) | client-side only | -- | -- | -- | Testable | |
| Forget network (per-row, not a top button) | `POST /forget` | ADMIN | `wifi_forget` | -- | Testable | PASS 2026-09-21 (B31, forgot throwaway SSID) |
| DHCP/Static (`ipModeDhcpBtn`/`ipModeStaticBtn`) + Save | `POST /ip_config` | ADMIN | none direct | -- | Testable | |
| Networks list | `GET /networks` | OPEN | `wifi_get_networks` | -- | Testable | |
| Status poll | `GET /status` | OPEN | `wifi_get_status` | -- | Testable | |

### Static/shared assets (no page shell of their own)

| Control | Route | Tier | MCP tool | LCD equivalent | Bench class | Result |
|---|---|---|---|---|---|---|
| `theme.css`, `nav.js`, `app.js`, `commissioning_shared.js` | `GET /theme.css`, `GET /nav.js`, `GET /app.js`, `GET /commissioning_shared.js` | OPEN | none (served assets, checked by `check_lint_pages.ps1`/responsive sweep, not the MCP facade) | -- | Testable | PASS 2026-09-21 (A44 (200 once Accept-Encoding: identity dropped for these routes)) |

---

## LCD pages (`kiln_ui_register_page`, `firmware/KilnFW/App/drivers/ui/kiln_ui.c`)

16 registered pages. Each is reached via `touch_inject`/physical tap and
verified with `capture_lcd.ps1` + `sample_lcd_region.ps1` numeric sampling
(never by eye, per CLAUDE.md and `project_st7796_rgb565_byte_order`).

| LCD page name | Source | Purpose | Web equivalent | Bench class | Result |
|---|---|---|---|---|---|
| `home` | `ui_page_home.c` (+`_actions`, `_chart`, `_graph`, `_rail`, `_refresh`) | Live status, start/stop, graph | `/` | Testable | PASS 2026-09-21 (M18 LCD class; zone temps 27.2-27.4C matched `safety_get_status`; gear icon bright vs bezel numeric sample) |
| `config` | `ui_page_config.c` | Device config menu | `/settings` (partial) | Testable | PASS 2026-09-21 (M18 LCD class; 5-cell hub grid confirmed: Profiles/Temperature/Network/Diagnostics/Units) |
| `temperature` | `ui_page_temperature.c` (+`_safety`) | Per-zone temperature + safety status | `/safety`, `/api/status` | Testable | PASS 2026-09-21 (M18 LCD class; zone temps 27.2-27.3C, "zone relays are view-only", Relay 2/K4 state shown, no write made) |
| `network` | `ui_page_network.c` | Wi-Fi status | `/status`, `/wifi` | Testable | PASS 2026-09-21 (M18 LCD class, page-load only; displayed `192.168.1.156 (kilnctl.local)` matched `wifi_get_status()`'s `sta_ip`, signal -35dBm vs API -37dBm) |
| `network_manage` | `ui_page_network_manage.c` | Wi-Fi scan/connect/forget | `/scan`, `/provision`, `/forget` | Testable | PASS 2026-09-21 (M18 LCD class, continuation) -- reached from `network` page's "Manage networks" button, measured via `touch_log_tap_targets` at (239,283); page-load only (Scan/Saved tabs, saved network row + Forget button at (407,197) seen, not tapped). Connect/forget actions remain owner-gated, not attempted |
| `diagnostics` | `ui_page_diagnostics.c` | On-device diagnostics | `/diagnostics` (subset) | Testable | PASS 2026-09-21 (M18 LCD class; page 1/8 shown, reset_reason "Software (esp_restart)" matched `get_heap_status`, no crash banner, matches no-unacked-crash state) |
| `touch_cal` | `ui_page_touch_cal.c` | Touch calibration | none (LCD-only; `KILNCTL_TOUCH_CAP_*` knobs, not the inert `KILNCTL_TOUCH_CAL_SWAP_XY`) | Testable | N/A -- unreachable on this bench's self-calibrating FT6336U (cell hidden), per `docs/COMMISSIONING_LCD_RUNBOOK.md` |
| `touch_test` | `ui_page_touch_test.c` | Raw touch test/tap-target dump | none | Testable via `touch_log_tap_targets`/`touch_inject` | N/A -- unreachable (touch_cal cell hidden, no nav path); the underlying `touch_log_tap_targets()` was exercised once directly (not through this page) to confirm home-page geometry, see M18 LCD class notes |
| `profiles` | `ui_page_profiles.c` | Profile library | `/profiles` | Testable | PASS 2026-09-21 (M18 LCD class; "1/8" pages, 4 rows/page x 8 = matches `profiles_list()`'s 29 total (1 custom + 28 builtin); Delete button visible but not tapped) |
| `profile_picker` | `ui_page_profile_picker.c` (+`_format`) | Choose profile to fire | `/profiles` | Testable | PASS 2026-09-21 (M18 LCD class, page-load only; "Profile 1/8" heading, no row tapped). CAUTION found: reached from `home`, this page is PICK mode -- a row tap is a pick-and-return WRITE (selects that profile and returns to `home`), not detail navigation. Confirmed by device log `page: profile_picker -> home` when M18C_TEST's row was tapped; no harm resulted since M18C_TEST was already the active selection and `profiles_get_exec_status` showed `state=0` before and after. Row taps must not be repeated here without owner sign-off; see `profiles` (MANAGE mode) for read-only row -> detail navigation |
| `profiles_builtin_list` | `ui_page_profiles_builtin_list.c` | Built-in profile list | `/profiles` builtin section | Testable | NOT RUN 2026-09-21 (M18 LCD class, continuation) -- DEAD CODE, not a geometry gap: `ui_page_profiles.c` is now a thin alias for `ui_page_profile_picker_build_manage()` (single unified list, favorites-first, builtins and custom combined per UI_PLAN.md 6.2); this page is still registered in `kiln_ui.c` but no source anywhere calls `kiln_ui_show("profiles_builtin_list")`. Unreachable by any UI path on current firmware |
| `profile_detail` | `ui_page_profile_detail.c` | View one profile | `/api/profile` | Testable | PASS 2026-09-21 (M18 LCD class, continuation) -- reached via `config` -> Profiles cell -> `profiles` (MANAGE mode) -> row tap on "Cone 03 Fast Fire" (row tap in MANAGE mode navigates read-only to detail, unlike `profile_picker`'s PICK mode). Showed 03DSFF, peak 1065C matching `profiles_get(128)`'s segment target=1065.0C. Action row measured via `touch_log_tap_targets`: Segments (69,275), Edit (240,275), Start (410,275) -- Start not tapped (owner-gated) |
| `profile_segments` | `ui_page_profile_segments.c` | Segment list | `/profiles` segment editor | Testable | PASS 2026-09-21 (M18 LCD class, continuation) -- reached via `profile_detail`'s Segments button (69,275); page-load only, matches `profiles_get(128)`'s 4 segments |
| `profile_builder_zones` | `ui_page_profile_builder_zones.c` | New profile: zone selection | `/profiles` new-profile flow | Testable | PASS 2026-09-21 (M18 LCD class, continuation) -- reached via `profiles` (MANAGE mode) topbar Add/New icon, measured at (453,20) (4-icon page1: Back/Home/Next/Add per `ui_topbar.c`'s deterministic left-to-right order). Name field and zone0/1/2 selectors rendered; zone1 selected to exercise the Next-enable gate. DEFECT found continuing this chain -- see `profile_builder_segment` |
| `profile_builder_segment` | `ui_page_profile_builder_segment.c` | New profile: segment entry | same | Testable | PASS 2026-09-21 retest at 33124aa8 (fix `fbd603c1`) -- tapping Next at (427,201) on `profile_builder_zones` no longer crashes: `get_heap_status` uptime/reset_reason unchanged across the tap (no new boot, no crash banner), page loaded and captured via `capture_lcd.ps1`. Original crash report acknowledged via `crash_report_ack(confirm=True)`, matched by exact fields (reset_reason='TASK_WDT', exc_task='lvgl', exc_cause_str='IllegalInstruction', exc_pc='0x0004d06a', exc_addr='0x3c170bef') |
| `profile_builder_review` | `ui_page_profile_builder_review.c` | New profile: review/save | same, `POST /api/profile` | Testable | PASS 2026-09-21 retest at 33124aa8 (M18 LCD class, continuation) -- reached via `profile_builder_segment`'s Next, page-load only, no crash; Save not tapped (owner-gated WRITE, not attempted) |

Note: no LCD page exists for live-profile editing, credentials/security,
backup/restore, OTA, kiln_configs presets, or the safety-commissioning
wizard -- those are web-only, hence "--" in the web tables above.

---

## API-only routes (no UI control anywhere -- web, LCD, or otherwise)

These are reachable only via direct HTTP or the MCP facade; no button/page
submits to them directly (some are read by JS polling loops rather than a
click, which is noted).

| Route | Tier | MCP tool | Notes |
|---|---|---|---|
| `GET /api/profile_exec` | OPEN | `profiles_get_exec_status` | Polled by dashboard JS, not a button |
| `GET /api/profile_plan` | OPEN | none direct | Feeds the feasibility popup, no direct control |
| `GET /api/board_temps` | OPEN | none direct (`thermo_read` is the live-board equivalent) | |
| `POST /api/unit_pref` | ADMIN | none direct | Submitted by the main-page unit toggle, not a dedicated Save button |
| `GET /api/dualwrite_window` | ADMIN | none direct | Read by diagnostics page JS |
| `GET /api/cfgfs/format_pending` | ADMIN | none direct | Polled before the format-confirm button enables |
| `GET /api/ota/challenge` | OPEN | `ota_get_challenge` | Fetched automatically before an OTA POST |
| `GET /api/logs/firing`, `GET /api/logs/autotune` | ADMIN | `fetch_event_log`/`get_device_log`/`get_device_log_json` (adjacent, not identical) | No page renders these directly today |
| `POST /api/safety/log_level` | ADMIN | `safety_set_log_level` | No web control; CLAUDE.md notes this had no caller anywhere as of 2026-09-19 |
| `GET /api/watchdog_cfg` | ADMIN | `get_watchdog_panic_disabled` | Paired with the diagnostics toggle's POST, itself a button |

---

## Gaps and caveats found while building this matrix

- Several ADMIN POST routes (profile import/hide/restore/favorite, kiln_configs
  save/clone/rename/delete/import, backup import/export, security saves,
  danger-mode relay control, cfgfs file read/write) have **no direct MCP
  facade tool** -- exercising them from an agent means raw HTTP, not
  `kiln_call`. This matches `feedback_prioritize_mcp_improvements`: closing
  these gaps is tooling work, not a one-off workaround, if repeated testing
  of these paths turns out to be common.
- `route_tier_table.h` is the single authoritative tier source (see its own
  header comment on the "reset one side of a pair" risk of a second, hand-
  maintained tier table) -- this document's tier column is copied from it
  verbatim, not re-derived.
- The `/status` GET route legitimately serves two roles (Wi-Fi status page AND
  a page-shell OPEN route) under one `ROUTE_TIER_OPEN` entry, per the
  2026-09-17 audit finding 7 fixed in that file -- do not read that as a
  missing row.
- Live pixel judgment (LCD color/state) must go through
  `sample_lcd_region.ps1`'s numeric RGB sampling against a bezel reference,
  never eyeballing a screenshot -- `project_st7796_rgb565_byte_order` records
  a prior incident where this was gotten wrong (byte order, not inversion).
