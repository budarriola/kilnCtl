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

**2026-09-21/22 W42 live re-run + C1/C9/C16 status check (firmware 08f1c451):**
this run's premise ("C1, C9, C16 marked NOT ATTEMPTED") was stale for all
three -- **C1 and C9 already show PASS** in the Page-by-page inventory's
`/settings/zones` "Measure Normal Current" row below (dated 2026-09-21, same
day, a later run than the one that wrote the "NOT ATTEMPTED" narrative
above), and **C16 already shows PASS** in the `/settings/security` page's
credentials-save row (`cmd=set_web_password` re-set to the same env-var
value via raw form POST, working around `web_auth_setup`'s lack of a
"re-affirm current credential" path). None of the three was re-run this
session: C1/C9 to avoid needlessly re-cycling relays on an already-passed
row, and C16 because this run had no reason to touch the shared admin web
credential again once its already-PASS status was found -- re-attempting a
one-way credential write (`set_web_password`/`set_lcd_pin`/
`clear_credentials`, none of which has a clean single-field undo) without a
fresh need would only add unnecessary risk to the credential every
concurrent bench session authenticates with.

W42 (`kiln_config create-then-delete`, `web_commission_row.py --special
kiln_config_create_delete`) was re-run live against `08f1c451` with the fixed
runner (79f70f04/ea05886d/9c02787d chain, 18-char `kc_test_<epoch>` name).
Pre-check `GET /api/kiln_configs` showed 1 config, no leftovers. Create half
PASSED (`kc_test_1790034646` id=4 appeared). **Delete half FAILED**, a new,
real finding distinct from the name-length bug the fix chain closed:
`POST /api/kiln_configs/delete` returned `400
"'kc_test_1790034646' is the kiln config this controller is running; select
another kiln config first, or use Save as to keep a copy"`. Root cause read
from source (`kiln_cfg_store.c` `kiln_cfg_store_save_current_ex()` lines
~1201-1216): a "Save as new" from the *current live* setup deliberately marks
the new slot `active_id` on creation ("a config just saved FROM the running
kiln is, by construction, exactly what's live right now -- marking it active
is recording a fact, not applying anything"), and the H5 backstop interlock
(`kiln_cfg_store_delete()`) unconditionally refuses to delete the active
config. **W42's own scripted shape (create-then-immediately-delete the same
slot) is structurally incompatible with this intentional firmware behavior**
-- not a firmware defect, and not the same bug the fix chain closed; the
runner needs a different design (e.g. apply/select a different existing
config before deleting the fresh one, which is itself a zone-config-changing
action a bench read-mostly run should not take unprompted) before this row
can PASS end to end. Per this run's "do not patch firmware" instruction, no
source change was made; the runner (`web_commission_row.py`) was likewise
left unchanged since fixing it correctly needs a design decision (how should
the throwaway slot's active-marking be undone) rather than a one-line patch.
**Board state left behind:** the throwaway config `kc_test_1790034646`
(id=4) is still present and marked active in `/api/kiln_configs` --
undeletable via the ordinary UI/API path while active, and no route exists
to clear `active_id` without applying a different config's blob (a zone-
config-changing action this run did not take). Its blob content is
byte-identical to the zone config that was already live before the test
(that's the entire point of "Save as new" from current settings), so no
zone/PID/relay parameter actually changed on the board -- only the
`kiln_configs` store gained one harmless, byte-identical, currently-
undeletable extra entry. `get_heap_status` confirmed no reboot across the
whole sequence (uptime 785s -> 1066s, `reset_reason` unchanged,
`software (esp_restart)` from before this session). `get_readiness` before
and after: unchanged, 17 ok / 1 not_done / 3 other, no trip, crash
acknowledged, no new crash. Recommend the next session either gets explicit
authorization to apply a different existing kiln config (making id=4
deletable) to clean this up, or the runbook/runner is updated to avoid this
shape entirely (e.g. test against a config created via Clone, which does
*not* mark itself active, rather than Save-as-new).

**2026-09-21/22 W42 runner redesigned (code + tests only, not yet re-run
live):** `_run_kiln_config_create_delete()` (`web_commission_row.py`) now
matches the intentional firmware behavior above instead of fighting it: it
reads `GET /api/kiln_configs` first and records the original `active_id`;
cleans up any pre-existing `kc_test_*`/`__kc_web_commission_test__` leftover
first (selecting a fallback config active first if the leftover itself is
active, e.g. the still-live `kc_test_1790034646` (id=4) left behind by the
run above); creates the throwaway slot as before; re-selects the *original*
active id via the page's own `#kilnConfigSelect` + `#kcApplyBtn` control
(`POST /api/kiln_configs/apply`, polled to completion via
`/api/kiln_configs/apply_status`, confirmed via `active_id` in the read-back
-- there is no separate "select" endpoint) before deleting the throwaway,
since the saved blob is byte-identical to what was already running and this
is a restore, not a behavior change; and now checks the delete POST's own
status/failed fields too (closing the "delete-POST-status gap" noted above).
The fallback it selects when the leftover itself is active is never another
`kc_test_*` leftover (applying one would make a throwaway of unknown
provenance the board's LIVE config, which is what `apply` rewrites: relay
wiring, thermocouple assignment, PID gains, guard thresholds); if the only
other slots are leftovers it refuses. `apply_status`'s `diverged` and
`reason` fields are read, not just `state`: a swap that ends DIVERGED
(`kiln_cfg_swap.c`'s alarmed exit -- config left pending for retry; heat is
disabled only on the ceiling-latch branch of that exit, not on the four
rollback-failure branches, so the board's own `reason` text is what says
which occurred) or that is still `running` when the ~60 s budget runs out is
reported as such, and NO further write is issued against a board in either
state. Otherwise, on a failure after create it still attempts the re-select
and delete as best-effort cleanup and reports exactly what, if anything, is
left on the board. Covered by 8 new and 2 rewritten unit tests in
`tools/PcTools/tests/test_web_commission_row.py` -- 100 -> 108 tests in that
file (happy path, inactive-leftover cleanup, active-leftover-via-fallback
cleanup, refusal when an active leftover has no fallback, refusal when the
only fallback is itself a leftover, DIVERGED and still-running apply results,
best-effort delete when the restore-apply is refused outright, and a rejected
delete POST reported by URL/status) -- all mocked, no board contact. **This row stays FAIL until re-run live**, both to confirm
the new flow actually passes against real firmware and to clean up the
`kc_test_1790034646` (id=4) leftover the previous live run left active.

**2026-09-22 W42 live re-run at `e862a35a` against board firmware
08f1c451 -- NEW FIRMWARE DEFECT, board panicked, run stopped immediately.**
Preconditions checked and recorded before running: no firing
(`profiles_get_exec_status` state=0), safety link up/armed/not tripped, no
unacknowledged crash (`get_readiness`: `crash_report: ok`), `GET
/api/kiln_configs` still showed exactly the same leftover the prior run left
(`{"active_id": 4, "configs": [{"id": 1, "name": "M18RR_B21renamed",
"is_active": false}, {"id": 4, "name": "kc_test_1790034646", "is_active":
true}]}`), matching this run's premise exactly. Invocation: `.venv\Scripts\
python.exe -m kilnctrl.web_commission_row W42 --host 192.168.1.156` from a
clean tree already at `e862a35a` (no worktree divergence to reconcile;
`tools/wt/w42live_*` was minted per instruction but the main tree was
already at HEAD so the run used the main tree's checkout/venv directly).
Runner's own pre-read confirmed the same leftover and correctly began its
documented cleanup path (id=4 active, no other non-leftover config besides
id=1, so it applied id=1 as the fallback before deleting id=4, exactly the
redesigned flow's step 1). **The board panicked during that cleanup
`POST /api/kiln_configs/apply` (id=1) call**: the CDP driver's `waitForPost`
timed out waiting for the apply POST to resolve, and `get_heap_status`
immediately after showed `uptime_s=16`, `reset_reason='panic/exception'
(unclean boot)`, `exc_task='kiln_cfg_swap' exc_cause_str='IllegalInstruction'`
with an unacknowledged crash banner. `read_esp_coredump()` (read-only, no
ack) symbolized it: **`Panic reason: ***ERROR*** A stack overflow in task
kiln_cfg_swap has been detected`** -- a real stack overflow abort, not a
true illegal-instruction fault (`get_heap_status`'s `exc_cause_str` field is
reporting the abort's synthetic cause, consistent with this codebase's other
stack-overflow-presents-as-something-else incidents). Coredump archived at
`firmware/KilnFW/coredump_archive/coredump-78fe1c6691c5.bin`, ELF
`firmware/KilnFW/elf_archive/KilnCtrl-91efff0f77d4.elf`. **Run stopped
immediately per instruction: no crash ack, no retry, no further write of any
kind.** Read-only forensics only, one additional login: `GET
/api/kiln_configs` post-reboot showed `{"active_id": 4, "configs": [{"id":
1, ...is_active:false}, {"id": 4, "kc_test_1790034646", is_active:true}]}`
-- unchanged from before the run (the apply never completed, so `active_id`
never moved and no `kc_test_*` was ever created this run); `GET
/api/kiln_configs/apply_status` read `{"state": "idle", "id": -1,
"diverged": false, "reason": ""}` -- settled, not stuck and not diverged.
Post-panic `safety_get_status` showed link up, armed, not tripped;
`profiles_get_exec_status` state=0 (idle); `control_get_zones` byte-for-byte
identical Kp/Ki/Kd/cal/ranges to the pre-run read -- no zone/PID parameter
changed. `get_readiness` post-run: 16 ok / 2 not_done / 3 other (the new
`not_done` is exactly `crash_report: unacknowledged crash on record
(IllegalInstruction, task kiln_cfg_swap)`; every other line unchanged from
the 17 ok / 1 not_done / 3 other pre-run baseline). **Verdict: FAIL --
new, real firmware defect (a stack overflow in the `kiln_cfg_swap` task,
apparently reachable via the ordinary `/api/kiln_configs/apply` path this
runner already exercises), not a runner or fixture defect this time. Board
left with: the same pre-existing `kc_test_1790034646` (id=4, active) leftover
as before this run (unchanged), plus one new unacknowledged crash report.**
Needs firmware investigation of `kiln_cfg_swap`'s task stack sizing/call
depth before this row can be attempted live again; per this run's
instructions, no crash ack and no firmware fix were made. Full request/
response detail and the coredump/ELF paths are in `docs/BENCH_TEST_LOG.md`'s
2026-09-22 entry.

**2026-09-22 ESP flashed to `origin/main` 7dcde0dd (fix chain
`7e659e55`/`faae8492`/`987b84a6` for the `kiln_cfg_swap` stack overflow
above), crash acked, W42 re-run, plus first live runs of W50/W8/W9/W10/W11/
W18/W20/W33.** Built from a clean worktree (`C:\wt\flash7dc_hs69ym`, kept for
ELF provenance): SaftyFW configured/built first (`cmake -G Ninja -B build .`
with `PICO_SDK_PATH=C:\pico-tools\pico-sdk`, producing
`SaftyFW_slotA.bin`/`SaftyFW_slotB.bin`) then KilnFW via `idf.py build`
(sdkconfig copied from the main tree, byte-identical, board-tuned config).
`KilnCtrl.bin` was 0x25b1b0 (2,470,320 B) against the `app` partition's
0x800000 (8 MB) -- comfortable fit; the build's own "1/2 app partitions too
small" warning refers to the unrelated, much smaller `recovery` partition,
expected and harmless. Pre-flash: `get_fw_version` commit `08f1c451` (32
commits behind HEAD), `get_heap_status` showed the same unacknowledged
`kiln_cfg_swap`/`IllegalInstruction` crash banner as before
(`heap_internal` free=25187 B, min_free=12523 B), `safety_get_status` link
up/armed/not tripped, `get_readiness` 16 ok / 2 not_done / 3 other -- no
firing, safe to flash. `flash_firmware(kiln_fw_root=".../flash7dc_hs69ym/
firmware/KilnFW", verify=true)`: **"flashed and verified OK (bootloader +
partition table + app), board reset and running"**, provenance HEAD
`7dcde0dd` tree clean, ELF archived as `KilnCtrl-7e07b8fb64f7.elf`,
`boot_guard_reset` cleared and verified (boot_count before=1, after=1). Only
the ESP was reset (Pico untouched), so no S6a trip was expected or seen.
Post-flash: `get_fw_version` commit `7dcde0dd`, "board is running HEAD";
`heap_internal` free=35487 B, min_free=20043 B (both higher than pre-flash,
consistent with the fix moving large locals off the task stack rather than
onto `.bss`); `safety_get_status` link up/armed/not tripped, unchanged. The
same pending crash record was still shown (persisted across the reboot, not
yet acked) -- confirmed it names `exc_task='kiln_cfg_swap'
exc_cause_str='IllegalInstruction'` before acking: `crash_report_ack(confirm=
true)` returned "ok - acknowledged and confirmed by read-back" with the same
fields plus `exc_pc='0x00043d2e' exc_addr='0x00000000'`.

W42 re-run live (`python -m kilnctrl.web_commission_row W42 --host
192.168.1.156`): **PASS**, see the runbook entry above for the full
transcript; the leftover `kc_test_1790034646` (id=4) from the panicked run
was cleaned up as part of it. `get_heap_status` immediately after: no fresh
crash banner, `uptime_s` advanced 18 -> 98 with `reset_reason='software
(esp_restart)'` unchanged -- no reboot, confirming the fix holds under the
same apply-then-delete path that previously panicked the board.

W50, W11, W18, W20, W33 (first live runs): all **PASS** -- see their
runbook rows above for each one's exact request/response. W8 (first live
run): **FAIL** -- create step passed, delete step's CDP driver could not
find the `Delete "<name>"` aria-label selector at runtime and left
`wc_test_0427400` on the board; cleaned up out-of-band via the
`profiles_delete` MCP tool (not the UI) immediately afterward, confirmed via
`profiles_list`. This is a selector-mismatch defect in the runner/page, not
a firmware defect. W9 and W10 (first live attempts): both refused to start,
correctly detecting W8's leftover before touching anything -- board state
unaffected by either. A retry of W9/W10 after the W8 leftover was cleaned up
was blocked by the local permission classifier (irreversible-deletion
class) and was not attempted again; they remain first-live-attempt REFUSED,
not FAIL, and not yet PASS.

Board state at the end of this pass: firmware `7dcde0dd`, no
unacknowledged crash, no firing, only the pre-existing `#0 M18C_TEST`
profile and the original kiln config selection remain -- byte-for-byte the
same profile/config baseline as before this pass, modulo the crash ack
(intentional) and the flash itself (intentional).

**2026-09-22 W8 re-run with fixed runner (`63a48ab3`), ESP flashed to
`63a48ab3` (carries `0edfb313`'s factory_reset nvs/net80211 self-check), C6
self-check re-run.** Shared tree confirmed at `63a48ab3` before starting.
Preconditions: no crash banner, safety link up/armed/not tripped, no firing,
`profiles_list` showed only `#0 M18C_TEST` plus built-ins.

W8 re-run (`python -m kilnctrl.web_commission_row W8 --host 192.168.1.156`,
fix `63a48ab3` for the CDP click race): **PASS** -- "created
`wc_test_0438920` via the segment builder (`POST /api/profile`), confirmed
via `GET /api/profiles`, deleted via its own Delete icon (`POST
/api/profile/delete`), confirmed removed." `profiles_list` afterward showed
no scratch profile remaining.

ESP flashed to `63a48ab3` from a clean worktree (`C:\wt\flash63a_zw9974`,
kept for ELF provenance): SaftyFW built first for slot bins, KilnFW via
`idf.py build` (sdkconfig copied byte-identical from the main tree).
Pre-flash: `get_fw_version` commit `7dcde0dd`; `get_heap_status` no crash
banner; `safety_get_status` link up/armed/not tripped. `flash_firmware
(kiln_fw_root=".../flash63a_zw9974/firmware/KilnFW", verify=true)`: flashed
and verified OK, board reset and running, provenance HEAD `63a48ab3` tree
clean, ELF archived as `KilnCtrl-da119321dcbb.elf` (confirmed to contain the
string "nvs/net80211 key count"), `boot_guard_reset` cleared and verified.
Only the ESP was reset (Pico untouched), so no S6a trip was expected or
seen -- none occurred. Post-flash: `get_fw_version` commit `63a48ab3`,
"board is running HEAD"; `heap_internal` free=35031 B, min_free=17807 B;
`safety_get_status` link up/armed/not tripped, unchanged.

C6 self-check (`docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md`
"Update 2026-09-22 (0edfb313)" recipe): `get_device_log` polling started
before triggering an authenticated `POST /api/factory_reset` scope=wifi
(challenge/HMAC signed with `KILNCTL_AP_PASSWORD`, "factory-reset" context,
value never printed). Captured both self-check log lines verbatim:

```
factory_reset: nvs/net80211 key count before esp_wifi_restore(): 0
factory_reset: nvs/net80211 key count after esp_wifi_restore(): 0
```

Before=0, after=0 -- **PASS** (an after-count of 0 is the positive verdict
per the audit doc's recipe). STA re-provisioned from
`KILNCTL_STA_SSID`/`KILNCTL_STA_PASSWORD` (env vars, never printed) over the
shared UART hub via `WifiUartClient.add_network()`; board rejoined the LAN
at `192.168.1.156` within ~18 s (well inside the 3-minute limit), confirmed
by `wifi_get_status()` showing `sta_connected=True`. Web auth confirmed
still ON: unauthenticated `GET /api/status` returned `fw_build: null`
(redacted), matching the enabled-web-auth house gate.

Board state at the end of this pass: firmware `63a48ab3`, no unacknowledged
crash, no firing, no scratch profiles, safety link up/armed/not tripped, Wi-Fi
STA reconnected, web auth ON. Worktrees kept for provenance:
`C:\wt\flash63a_zw9974` (flash build) and `C:\wt\m18rec2_pkkk5a` (this
record).

**2026-09-22 `kiln_config_apply` MCP tool live exercise (backend, board
firmware `63a48ab3`).** Precheck: `get_heap_status` no unacknowledged crash
(`uptime_s=5235`, `reset_reason='software (esp_restart)'`),
`safety_get_status` link up/armed/not tripped, `profiles_get_exec_status`
state=0 (idle), `autotune_get_status` state=idle. `kiln_call(name=
"kiln_config_apply", args={"id":0})` with `confirm` omitted correctly
dry-ran: `"DRY RUN (pass confirm=True to actually apply) -- would POST
/api/kiln_configs/apply for id=0 (ack_hardware_differs=False,
host=192.168.1.156)"` -- no POST sent. No dedicated kiln_configs list/read
MCP tool exists yet (`kiln_find(query="kiln configs")` surfaced only
`kiln_config_apply`/`kiln_configs_quarantine_clear`); read the store
instead via the existing PC client library directly
(`kiln_configs_quarantine_http_client.get_kiln_configs_list()`, the same
read-only `GET /api/kiln_configs` call `kiln_configs_quarantine_clear`'s
own dry-run already makes internally) -- confirmed exactly one saved
config, `id=1` `"M18RR_B21renamed"`, `active_id=1`, `is_active=true`.

Applied it to itself (a no-op by content, per this run's scope rule --
exactly one saved config and it is already active):
`kiln_call(name="kiln_config_apply", args={"id":1,"confirm":true})`.
**Result: FAILED/DIVERGED**, verbatim: `"FAILED: apply for id=1 left the
board DIVERGED (state='done_failed', reason='both halves committed and
matched, but the post-swap ceiling/arming check failed (Pico reports no
config_crc after the swap) -- heaters disabled and alarmed, config left
pending for retry') -- heaters should be disabled, do not trust config
state (host=192.168.1.156)"`. This is a real, reproducible outcome, not a
dry-run artifact -- `confirm=True` was set and the tool polled
`/api/kiln_configs/apply_status` to this terminal state itself.
`ack_hardware_differs` was never passed and never applicable (no 428 was
returned; this was a 202-then-`done_failed` sequence, a different failure
class from the hardware-shape refusal that flag answers). Per instruction,
no retry, no crash ack, and no reset of either processor was attempted.

Post-failure read-back: `GET /api/kiln_configs` unchanged --
`{"active_id": 1, "configs": [{"id": 1, "name": "M18RR_B21renamed",
"is_active": true}], "max_count": 10}` -- the active id never moved.
`GET /api/kiln_configs/apply_status` read back
`{"state": "done_failed", "id": 1, "diverged": true, "reason": "..."}`
-- confirming the board's own `diverged=true` flag directly, not just the
tool's rendering of it. `get_heap_status` immediately after: `uptime_s`
advanced 5235 -> 5458, `reset_reason` unchanged (`software (esp_restart)`)
-- no reboot. `safety_get_status`: link up, armed, not tripped -- unchanged
from precheck; no alarm latched on the safety side despite the apply's own
"alarmed" wording. `control_get_zones`: all three zones still `mode 3`
(heat-capable), Kp/Ki/Kd unchanged from the pre-apply `get_board_state`
snapshot. `get_readiness` immediately after: 16 ok / 2 not_done / 3 other,
`control_mode: ok (3 of 3 zones can heat)`, `safety_trip: ok (no guard
currently tripped)` -- no readiness regression observed.

**Net finding: the board's externally-observable state (zones config,
safety arm state, readiness) did not visibly degrade, but the apply route
itself reports a genuine "heaters disabled and alarmed" DIVERGED outcome
for what should have been a pure no-op (identical content, same id applied
to itself) -- worth a firmware-side look at why a same-content swap's
post-swap ceiling/arming check reads no `config_crc` back from the Pico.**
Not root-caused this session (no source investigation was in scope for
this bench pass); flagging for the next backend pass. No firmware or
runner change made. Board left exactly as found: the same single config
(`id=1`, active), no crash, no reboot, safety link up/armed/not tripped.

**Fixed:** landed as `a526b0a0`/`023b1cec`/`e1e2c00a` (a clean apply was
reading a zeroed cache tag as a Pico-unconfigured divergence; see
`project_config_apply_false_diverged_cache_tag.md`), bench-verified
2026-09-22 -- self-apply `id=1` returned `done_ok`.

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
| E-stop verify | `POST /api/estop/verify` | ADMIN | `estop_verify` (`fc16c186`, `055703af`) | -- | **Hardware-gated, now run** | PASS 2026-09-24: owner pulled the bench E-stop jumper, S7 (`SAFETY_TRIP_ESTOP`, trip_reason 8, `trip_mask 0x0080`) latched end to end (Pico diag, ESP safety cache, readiness `safety_trip`, all relays de-energized), owner refitted the jumper, trip cleared via `safety_clear_trip()`, verification recorded with read-back via `estop_verify`. Pole 1 stays permanently unwired (owner decision, unchanged) |
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
| Save as new (`kcSaveNewBtn`) | `POST /api/kiln_configs/save` | ADMIN | none direct | -- | Testable | PASS 2026-09-21 (B19, after `kiln_configs_quarantine_clear`; form-urlencoded body, field `name`); Save-as-new + Delete selected wired together as W42 in `web_commission_row.py` (`_run_kiln_config_create_delete`: create a uniquely-named throwaway config, confirm via GET, select it in `#kilnConfigSelect` and delete it, confirm removed) -- **FAIL 2026-09-21, LIVE-RUN at board commit 33124aa8**: create step (fill `#kcSaveNewName`, click `#kcSaveNewBtn`) reported CDP success, but the follow-up `GET /api/kiln_configs` never listed a config named `__kc_web_commission_test_<ts>__` -- exact decisive line: "W42 FAIL: no config named '__kc_web_commission_test_1790032582__' found in /api/kiln_configs after create -- write did not land". Delete step never ran (function stops before selecting/deleting on a failed create, per its own guard), so no throwaway config was ever created or left behind; `GET /api/kiln_configs` before and after the attempt returned the same set, confirming the list is unchanged. **Root-caused `docs/audits/w42_kiln_config_create_2026-09-21.md` (verdict (c), runner-side fixture defect, no firmware defect):** the generated name was 37 chars (the audit doc says 38; `len("__kc_web_commission_test_")` is 25, not 26 -- the off-by-one does not change the verdict), over the firmware's `KILN_CFG_NAME_MAX_LEN` (23, `kiln_cfg_store.h`), so the `save` POST was correctly rejected 400 and the runner's success check never inspected the POST status, only the later GET, producing the generic message above. Fixed in `web_commission_row.py`: the generated name is now `kc_test_<ts>` (18 chars at today's epoch, asserted against a mirrored `_KILN_CFG_NAME_MAX_LEN` constant by driving the real generator), and the create step now parses the CDP driver's `--expect-post` result and reports a non-200 or network-failed create status directly instead of falling through to "write did not land". The delete step is NOT status-checked the same way (known, bounded gap: a rejected delete is still caught by the post-delete read-back, which fails with "LEFT ON BOARD", just without the POST status in the message). **Re-run 2026-09-21/22 at board commit 08f1c451 (fix chain 79f70f04/ea05886d/9c02787d): create step now PASSES** -- `kc_test_1790034646` (18 chars) appeared in `GET /api/kiln_configs` with `id=4` after the fixed create step. **Delete step FAILS, a new and different finding from the name-length bug above:** `POST /api/kiln_configs/delete` returned 400 `"'kc_test_1790034646' is the kiln config this controller is running; select another kiln config first, or use Save as to keep a copy"`. Root cause read from `kiln_cfg_store.c`: `kiln_cfg_store_save_current_ex()` deliberately marks a freshly-created "Save as new" slot as `active_id` (it is, by construction, exactly what's live), and `kiln_cfg_store_delete()`'s H5 backstop unconditionally refuses to delete the active config -- so W42's own scripted shape (create-then-immediately-delete the *same* slot) can never pass end to end against this firmware's intentional design; this is a runner test-design defect, not a firmware defect, and needs a different flow (e.g. delete a *different*, non-active slot, or apply another config first) rather than a one-line fix. Per this run's instruction, no firmware or runner change was made. **Board left with the throwaway `kc_test_1790034646` (id=4) still present and active** -- undeletable without applying a different config (a zone-config-changing write this run was not authorized to make); its blob is byte-identical to the zone config already live before the test, so no live parameter changed. Full narrative: the dated 2026-09-21/22 section above. **`_run_kiln_config_create_delete()` redesigned 2026-09-21/22** (code + unit tests only, see the "runner redesigned" note above): re-selects the original active id via `#kilnConfigSelect`+`#kcApplyBtn`/`POST /api/kiln_configs/apply` (polled via `apply_status`) before deleting the throwaway, checks the delete POST's own status, and cleans up any pre-existing leftover (including the `kc_test_1790034646`/id=4 left behind above) before the real run. **Re-run 2026-09-22 at board commit 08f1c451: FAIL, new blocking finding -- the board panicked (stack overflow in task `kiln_cfg_swap`, confirmed via `read_esp_coredump()`) during the redesigned runner's cleanup `POST /api/kiln_configs/apply` call, before ever reaching the create step. Run stopped immediately, no crash ack, no retry. See the dated 2026-09-22 W42 record above for full detail.** |
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

**2026-09-21 LCD class, closed:** 13 PASS, 2 N/A (`touch_cal`, `touch_test`:
FT6336U self-calibrating, unreachable by nav path), 1 N/A dead code
(`profiles_builtin_list`). All 16 rows accounted for; the class is terminal —
no further LCD rows remain to run.

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
| `profiles_builtin_list` | `ui_page_profiles_builtin_list.c` | Built-in profile list | `/profiles` builtin section | Testable | N/A 2026-09-21 (dead code) -- not a geometry gap: `ui_page_profiles.c` is now a thin alias for `ui_page_profile_picker_build_manage()` (single unified list, favorites-first, builtins and custom combined per UI_PLAN.md 6.2); this page is still registered in `kiln_ui.c` but no source anywhere calls `kiln_ui_show("profiles_builtin_list")`. Unreachable by any UI path on current firmware |
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

## Link-loss heating block during a Pico update -- 2026-09-21 hardware exercise (SKIP)

ROADMAP.md's open item (M8 area, "Link-loss heating block **not** bypassed during a
Pico update") asked whether a real ESP-driven Pico firmware update can be exercised on
the bench to observe `relay_authority_on_blocked()` holding through the link-loss
window. **Premise checked read-only, verdict SKIP -- no update can be pushed on this
fixture today:**

- `docs/PICO_AUTO_UPDATE_PLAN.md` section 1's review is **NO-GO** for installing the
  two-slot bootloader on the bench Pico (unverified metadata seeding, unverified restore
  path, no atomic multi-image flash tool) -- matches the standing owner-gated NO-GO in
  memory (`project_pico_bootloader_install_no_go.md`). A separate 2026-09-18/2026-09-20
  passage of the same plan describes a *different* bench Pico later booting through a
  two-slot bootloader with P1 marked CLOSED, then hitting an ESP-relay erase-watchdog
  defect and a CRC-parameterization defect (both later marked FIXED/flashed); that
  narrative is stale relative to this bench's current board (see live check below) --
  this fixture's Pico is not running that bootloader today.
- Live board check, 2026-09-21, ESP `63a48ab3` / Pico `05f1ab1f`, board idle
  (`safety_get_status`: link up, SaftyFW armed, not tripped; `get_heap_status`:
  `uptime_s=3674`, no unacknowledged-crash banner): `ota_status()` reports the **last**
  Pico relay attempt already on record as
  `last_update: processor='pico' success=False reason="Pico refused: update would
  overwrite its running flat image; re[flash via SWD]"` -- the exact
  `SAFETY_LINK_UPDATE_STATE_REFUSED_RUNNING_IMAGE_OVERLAP` (state 9) refusal
  `docs/PICO_AUTO_UPDATE_PLAN.md` section 2 describes for a flat-image Pico, confirming
  from the board's own history that a relay is structurally refused before the data
  phase ever starts, with no interlock/heating exposure to observe. Current phase at
  read time: `pico relay: phase='idle' percent=0`.

**No update was attempted this run** (per the coordinator's instruction, a confirmed
structural refusal is the SKIP condition, not a reason to still push a POST for its own
sake); no bootloader was installed, no `debug_program` call was made, no board state
changed. ROADMAP.md's item carries a one-line note pointing here. Re-attempt only after
an owner decision reopens the two-slot-bootloader install on this specific fixture.

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

## 2026-09-22 W20/W50/W8 live re-run (harness at 63b62945)

Re-ran three M18 web rows against the live board with `web_commission_row.py`
code as of `63b62945` (W20 now accepts idle/done/aborted autotune states,
W50 now grades the `/api/settings/tz` and `/api/unit_pref` POST statuses,
the CDP driver's `clickWithRetry` gained `??` timeout fallbacks). MCP
`kilnctrl` server was reporting STALE (1 file changed since it started at
commit `189326ff`) so the row tool itself was run from the docs worktree
(`C:\wt\w20live_imixfl`) via the main tree's venv with `PYTHONPATH`
pointing at the worktree's `tools/PcTools/src`; read-only board queries
(`get_heap_status`, `safety_get_status`, `autotune_get_status`,
`profiles_get_exec_status`, `profiles_list`) still went through the MCP
facade, since those code paths were unaffected by the pending change.

Precheck: `get_heap_status` showed `reset_reason='software (esp_restart)'`,
`uptime_s=4483`, no unacknowledged-crash banner; `safety_get_status` showed
link up, armed, not tripped; `autotune_get_status` idle; `profiles_get_exec_status`
`state=0` (idle); `profiles_list` showed only `#0 'M18C_TEST'` plus built-ins,
no leftover `wc_test_*`/`kc_test_*` scratch entries. No firing in progress.

Baseline read via a direct `GET /api/status` (worktree `http_auth.urlopen()`
seam, credentials from env, never printed): `temp_unit='C'`, `time_tz='UTC0'`.

- **W20** (`atAbortBtn`, `/settings/zones`): **PASS** -- "confirmed
  `/api/autotune` was idle/inactive before the click, clicked `atAbortBtn`,
  confirmed it remains idle/inactive afterward." Screenshot:
  `logs/web_commission/W20.png`.
- **W50** (`/setup` wizard step 1 Save, `/api/unit_pref` + `/api/settings/tz`):
  **PASS** -- toggled `temp_unit` 'C' -> 'F' via Save (`time_tz` re-submitted
  unchanged at 'UTC0'), confirmed via `GET /api/status`, restored to 'C' via
  a second Save, confirmed restored, and step 1's `/api/setup/progress`
  state restored to `pending` via a direct POST. Independent confirmation
  after the row returned: a fresh `GET /api/status` read `temp_unit='C'`,
  `time_tz='UTC0'` -- identical to the pre-run baseline above. Screenshots:
  `logs/web_commission/W50_set.png`, `logs/web_commission/W50_restore.png`.
- **W8** (segment-builder create/delete, `/profiles`): **PASS** -- created
  `wc_test_0491690` via the segment builder (`POST /api/profile`), confirmed
  via `GET /api/profiles`, deleted via its own Delete icon (`POST
  /api/profile/delete`), confirmed removed. This is the row that failed on
  2026-09-22 against `7dcde0dd` (delete selector not found); the fix in the
  current runner passed cleanly with no manual cleanup needed. Screenshots:
  `logs/web_commission/W8_create.png`, `logs/web_commission/W8_delete.png`.

Board final state: `get_heap_status` `uptime_s=4589` (up from 4483, no
reboot across the three rows), `reset_reason` unchanged, no crash banner;
`safety_get_status` link up, armed, not tripped, unchanged; `profiles_list`
back to only `#0 'M18C_TEST'` plus built-ins, no scratch profiles left on
board. No flash, no reset, no heating, no crash ack, W9/W10 not run, MCP
servers not restarted.

## 2026-09-22 web + LCD auth timeout live verification

Owner ask: confirm the web and LCD inactivity timeouts (WEB_AUTH_PLAN.md
section 8, `auth_policy.web_timeout_s`/`lcd_timeout_s`) actually work on
the bench, not just in source. Precheck: `get_heap_status` showed
`reset_reason='software (esp_restart)'`, `uptime_s=7295`, no
unacknowledged-crash banner; `safety_get_status` showed link up, armed,
not tripped, no firing in progress. `kilnctrl` MCP server reported STALE
(1 file changed since it started at commit `ec836434`); read-only board
queries still went through the facade, all HTTP for this test was raw
(`curl.exe`, cookie `kiln_sid`) since no MCP tool exposes login/session
cookies. Credentials from `KILNCTL_WEB_USERNAME`/`KILNCTL_WEB_PASSWORD`
(User-scope), never printed. Original policy read via `GET
/api/auth/config`: `web_enabled=true, lcd_enabled=false,
web_timeout_min=30, lcd_timeout_min=10, admin_pin_set=false,
user_pin_set=false`.

- **Web timeout**: **PASS**. Set `web_timeout_min=1` (the 1-60 min range's
  minimum) via `POST /api/auth/security cmd=set_policy`, kept
  `lcd_enabled`/`lcd_timeout_min` unchanged, confirmed via `GET
  /api/auth/config`. Logged in fresh, `GET /api/boot_guard` with the
  session cookie returned 200. Waited past the 60 s timeout with no
  activity-counting requests (only `GET /api/auth/session`, `ROUTE_TIER_OPEN`,
  polled -- WEB_AUTH_PLAN.md section 8 says this never counts as
  activity). Re-issued the identical `GET /api/boot_guard` with the same
  cookie: **401 Unauthorized** (`authentication required`). Board
  `uptime_s` before/after: 7295 -> 7885 (delta consistent with elapsed
  wall time, no reboot, `reset_reason` unchanged, no crash banner).
  **Needs live re-verify after flash**: this run predates `ed88d47b`/
  `87d3017b` (login redirect / admin-login modal replacing the
  "authentication required" page); an unauthenticated page GET now 302s
  to `/login?return=` instead of showing that page, and a session
  timeout is expected to surface the same way once the board is running
  that build. The raw API 401 body checked here may be unaffected, but
  has not been re-checked against the new build.
- **10 s stay-unlocked prompt window**: **PASS**. Fresh login, polled
  `GET /api/auth/session` at t+52 s (60 s timeout, window opens at
  t+50 s): `{"role":"admin","prompt":true,"seconds_left":7,
  "bootstrap_needed":false}` -- inside the window as expected.
- **LCD timeout**: **SKIP**. `GET /api/auth/config` showed
  `lcd_enabled=false` and, decisively, `admin_pin_set=false` and
  `user_pin_set=false` -- no PIN has ever been provisioned on this board,
  so there is no credential to unlock the LCD lock page with.
  `docs/COMMISSIONING_LCD_RUNBOOK.md` names no PIN source either. Turning
  `lcd_enabled` on for this test would arm a lock screen with no known way
  to pass it -- risking the physical panel getting stuck locked -- and
  provisioning a fresh PIN is a credential change beyond this
  verification's scope, so the LCD half was not exercised live. Source
  inspection only (`lcd_auth_state.c`/`ui_lcd_lock.c`, same
  `web_auth_session_in_prompt_window()` helper the web path uses) --
  unverified on hardware.

Policy restored and read back via `GET /api/auth/config`:
`web_enabled=true, lcd_enabled=false, web_timeout_min=30,
lcd_timeout_min=10` -- matches the original exactly. No `/api/auth/logout`
route exists in this firmware (checked); the last test session was left to
expire under the restored 30-minute policy rather than explicitly logged
out. Four logins total across the run (policy read, web-timeout test,
prompt-window probe, restore), each a correct-credential login (no
failures, no lockout ladder engaged); the first two (policy read) were
under 30 s apart before this run's pacing was locked in, all others were
well over 30 s apart. No flash, no reset, no heating, no crash ack,
`get_board_state`/W9/W10 not run, MCP servers not restarted.
