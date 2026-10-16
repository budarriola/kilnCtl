# Audit: GitHub auto-update under faults (2026-10-14)

Scope: device code only (`firmware/KilnFW`, `firmware/KilnFW_recovery`,
`firmware/CommonFW`). The path covered: release check, fetch and stage,
stage verify, install request, reboot into recovery, unattended
`apply_staged`, boot into the new app, boot_guard and the install result
record. Audited at origin/dev `153c12cf5`. No code was changed and the board
was not touched.

Owner goal: an unattended update that always ends in a bootable, known state
with a truthful result record, and needs no human unless the release itself
is bad.

Already reported elsewhere, so not repeated here:

- `UPDATE_PATH_ROBUSTNESS_AUDIT_2026-10-13.md` HIGH-1 and MED-1/2, and its
  LOW-1..8 (fixed or documented).
- REVIEW_AUTOFX LOW-4, still open: recovery HTTP starts before
  `recovery_install_boot()` (`recovery_main.c:95` vs `:128`).
- The plan's "power cut during the first boot of the new app" note (review
  2026-10-09 LOW-3) and the stale-stage auto-clear (OT-G06). MED-3 below
  covers a wider trigger and the result record, which that note does not.

Counts: HIGH 1, MED 3, LOW 3, INFO 4.

## HIGH

### HIGH-1: a failed unattended apply never leaves recovery on its own, even when `app` was never touched

- **Where:**
  - The apply task stays in recovery on failure: `recovery_apply_esp.c:177-181`.
  - `poll_task` records the failure and exits: `recovery_install.c:203-207`.
  - Pre-apply refusals do the same: `update_install_record.c:215-250` (via
    `recovery_install.c:235-242`).
  - The heap floor refuses the apply: `recovery_apply_esp.c:205-208`.
- **Fault scenarios.** Each of these fails with the old app still intact,
  because nothing in `app` has been erased yet (`app_modified` is set only at
  `recovery_apply.c:126`):
  - A stage valid at install time that was corrupted later
    (`RECOVERY_APPLY_ERR_STAGE_HASH`, `recovery_apply.c:108`).
  - A stage read error (`:102`, `:114`).
  - An image refused by `ric_validate_first_chunk` (`:121`).
  - Low internal heap at `recovery_apply_esp_start` (`ESP_ERR_NO_MEM`,
    `recovery_apply_esp.c:205-208`).
  - Refusals in `update_install_boot_run`: stage not ready, size or sha
    mismatch, bad record, NVS unavailable.
  Copy-stage failures (erase, write, read-back, `app_verify`, a pre_boot
  boot_guard clear, `set_boot`) leave `app` modified.
- **What the device does.** `poll_task` writes FAIL/`apply_failed`, puts
  "INSTALL FAILED" on the LCD and deletes itself. `apply_task` sets
  `s_running=false` and deletes itself. Nothing reboots, retries or exits.
  - The WP12 design's "bounded retry" (attempts kept in the request, max 3)
    only advances when something else reboots the board. No device code does
    that, so in practice the retry needs a power cycle.
  - The pre-apply refusals clear the request and also stay in recovery for
    good.
- **Bootable without a human:** no.
  - `app_modified == false`: the old, verified app sits behind recovery, and
    the board stays on the recovery SoftAP indefinitely. It is not
    controlling the kiln and not reachable on the LAN, so the unattended
    poller loses it.
  - `app_modified == true`: the bounded retry never runs unless someone power
    cycles the board.
- **Record truthful:** partly. It is FAIL, but see MED-1.
- **Suggested fix.** In `poll_task`, and after a `REFUSED` outcome in
  `recovery_install_boot`, act only when this recovery entry was caused by an
  install request:
  - If `!app_modified` and `app` passes `app_has_valid_image` plus
    `esp_image_verify`: auto-exit. Clear boot_guard (verified), clear the
    request but keep the FAIL result, run
    `recovery_boot_partition_set_and_verify(app)`, then restart. This is the
    same sequence as `recovery_exit_post` (`recovery_http.c:756-782`). The app
    then reads the FAIL result and can re-stage.
  - If `app_modified`: `esp_restart()` after a short delay, so the attempt
    counter (`update_install_record.c:243-248`) does what the design says.
    After `attempts_exceeded` it stays, as now.
  - If kiln NVS is unavailable (boot_guard clear is "n/a"): stay, so an
    app/recovery ping-pong cannot start.

**Fixed by (recfx), HIGH-1:** `recovery_install.c` `auto_recover()` runs a one-shot task after a failed apply (`poll_task`,
after the FAIL result is recorded) and after a refused install (`recovery_install_boot`). The decision is the pure
`update_install_after_fail(reason, app_modified, app_verifies, attempts_left)`: EXIT_APP only when `app` passes the full
image verify (re-run from flash via `recovery_http_app_verifies()`, which drops the cached verify first) and is either
unmodified or out of attempts; RESTART (so the bounded retry advances) while attempts remain; STAY otherwise. It always
STAYs when kiln NVS is unavailable, when the attempt counter did not advance (`counter_failed`), when the result write or
request clear failed, and when the outcome is unknown, so no app/recovery ping-pong can start. Exit order matches
`/api/recovery/exit`: verified boot_guard clear, drop the request (the FAIL result stays for the app), set the boot target
with read-back, restart. It is skipped while the Pico relay runs. Tests: `test_update_install_record.c`
`test_detail_and_after_fail` (decision matrix) and `test_recovery_lcd_policy.c` `test_install_source` (glue, ordering,
gates). Residual: if the verify task cannot be created for lack of heap the board stays in recovery, as before.

## MED

### MED-1: the result record cannot tell "old app intact" from "app erased", and reports a heap refusal as `apply_failed`

- **Where:** `recovery_install.c:204` writes `UPD_INST_REASON_APPLY_FAILED`
  for every `recovery_apply_result_t`. `update_install_record.c:249-250`
  writes the same reason when `apply_start` itself refused (heap floor,
  partition missing, busy).
- **Fault scenario:** any apply failure (see HIGH-1).
- **Consequence.**
  - The specific reason (`stage_hash_mismatch`, `app_erase_failed`,
    `set_boot_failed`, ...) is lost. So is `app_modified`, which is the one
    bit an unattended client needs to choose between "exit to the old app"
    and "re-stage and re-apply".
  - The specific reason is visible only in the RAM progress of
    `GET /api/recovery/apply_status`, and only until the next reboot.
  - After a `set_boot_failed` the record says `apply_failed`, but the app is
    fully written and verified (see MED-2).
- **Suggested fix.**
  - Carry the `recovery_apply_result_t` code and `app_modified` in the 48 B
    result record. It has room, or use a reason per group: untouched, app
    partially written, app written but not selected.
  - Give the `apply_start` refusal its own reason (`apply_start_refused` or
    `low_heap`).

**Fixed by (recfx), MED-1:** result blob byte 7 (unused before) now carries a detail code in its low 7 bits and an
`app_modified` flag in bit 7; the reason stays `apply_failed`, so an older decoder reads the record unchanged. Detail
1..15 mirror `recovery_apply_result_t` (static-asserted in `recovery_install.c`); the `apply_start` refusals get
`start_low_heap` (ESP_ERR_NO_MEM, the heap floor), `start_no_app`, `start_busy` and `start_failed`, always with
`app_modified` false. `GET /api/update/install` and the recovery status page carry `detail` and `app_modified` in
`last_install`, and the MCP install line prints them. Tests: `test_detail_and_after_fail`.

### MED-2: the request is consumed and "ok" is written before the boot target switch, so a cut or `set_boot` failure strands a good app

- **Where:**
  - `update_install_pre_boot` clears boot_guard, then the request, then
    writes the OK result (`update_install_record.c:168-178`).
  - It runs inside `cb_set_boot` before
    `recovery_boot_partition_set_and_verify` (`recovery_apply_esp.c:132-136`).
- **Fault scenarios:**
  - (a) Power cut or reset after `req_clear` / the OK write and before the
    otadata write completes.
  - (b) `recovery_boot_partition_set_and_verify` fails
    (`recovery_apply.c:173-174`).
- **What the device does.** `app` holds the new image, fully copied,
  re-hashed and `esp_image_verify`'d. otadata is still the blank or factory
  state the app left when it selected recovery, so the next boot is recovery
  again.
  - There is no request any more, so `update_install_boot_run` returns
    `NO_REQUEST` (`update_install_record.c:212-213`) and nothing happens.
  - In (b), `poll_task` then overwrites the result with FAIL/`apply_failed`.
- **Bootable without a human:** no. The board sits in recovery for good,
  even though the bounded retry exists exactly for cuts in this phase.
- **Record truthful:** no.
  - (a) says `ok` with the version and id while the board runs recovery. A
    client polling the app's `GET /api/update/install/status` never gets an
    answer, and the recovery status shows "ok".
  - (b) says `apply_failed`, but the app is good and only unselected.
- **Suggested fix.** Either works:
  - Move `req_clear` and the OK write after a verified
    `set_and_verify(app)`. Keep only the boot_guard clear before it. A cut
    after set_boot then re-enters recovery only through rollback, never
    through a re-apply, because otadata now points at `app`.
  - Or keep the order, and at recovery boot treat "no request, last result
    OK, `app` verifies and its sha prefix equals the result id" as
    "finish the switch": set_and_verify `app` and restart.
  For (b), retry `set_and_verify` once before giving up, and record
  `set_boot_failed`.

**Fixed by (recfx), MED-2:** the request is no longer cleared and "ok" is no longer written before the switch.
`update_install_pre_boot()` only clears boot_guard; the new `update_install_post_boot()` clears the request and writes the
OK result, and `recovery_apply_esp` calls it only after `recovery_boot_partition_set_and_verify()` succeeded. A
`set_boot` failure therefore leaves the request and a retryable state (the poll task records `set_boot_failed` with
`app_modified` set). Tests: `test_pre_boot`, `test_install_source`, `test_apply_esp_source`.

### MED-3: the new app's first-boot confirm uses boot-time-fixed flags, so a transient fault leaves it pending, and the rollback to recovery is recorded as "ok"

- **Where:**
  - `main_network_http.c:776-779`: `nvs_ok` comes from the one-shot
    `nvs_report_capture()` at `:522`, and `web_ok` from `dash_err`. Both are
    fixed for the boot.
  - `main_network_http.c:194` re-decides from those fixed values every poll,
    so a false flag never recovers.
  - `main_network_http.c:234/254`: `esp_ota_mark_app_valid_cancel_rollback()`
    is attempted once and not retried on error.
  - Recovery side: `recovery_install_boot` / `load_last`
    (`recovery_install.c:183-188`, `:219-242`) never checks whether `app` was
    rolled back.
- **Fault scenario:** the first boot after an unattended apply (image
  PENDING_VERIFY) hits any of these:
  - The NVS snapshot taken in a transient window (the class documented in
    `docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md`).
  - `dashboard_http_start` failing (it fails whenever `wifi_prov_start`
    brought up no httpd that boot).
  - The one mark-valid call failing.
- **What the device does.** The app runs normally for that whole boot but
  stays PENDING_VERIFY. The next reset of any kind (power blip, crash,
  watchdog, an operator's `sw_reset`) makes the bootloader abort the image
  and boot factory (recovery). The stage header is already erased
  (`recovery_apply.c:176`), so recovery does nothing.
  - The plan's LOW-3 note assumes a short window ("power cut during the first
    boot"). Here the window is the whole first boot, of unbounded length.
- **Bootable without a human:** no, although `app` holds a good image.
- **Record truthful:** no. The result still says OK with the new version, and
  recovery status shows it as the last install.
- **Suggested fixes:**
  - App: recompute `nvs_ok` live, or retry `nvs_report_capture`, and re-read
    `dash_err`/`ota_ok` each poll. Retry the mark-valid call while it returns
    an error.
  - Recovery boot: if `esp_ota_get_last_invalid_partition()` is `app` (the
    image was aborted), rewrite a last OK result whose id matches the `app`
    image to FAIL/`rolled_back`.
  - Optionally auto-exit once if `app` verifies, bounded by a recovery-side
    counter, so a really bad release cannot loop.

MED-3 app side: fixed by aa3955376 (see Resolution).

## LOW

### LOW-1: the unattended apply does not prove the boot_guard clear before erasing `app`

- **Where:** `op_apply_start` calls `recovery_apply_esp_start(app, pre_boot)`
  directly (`recovery_install.c:162-171`). The manual route proves the clear
  first (`recovery_http.c:1117-1124`) "so a clear that fails after the copy
  would [not] leave `app` rewritten but never selected".
- **Fault scenario:** kiln NVS writable at `req_write`
  (`update_install_record.c:246`) but the boot_guard clear fails later at
  `cb_set_boot` (`recovery_apply_esp.c:132-135`).
- **Consequence.** `app` is fully rewritten but never selected, with
  `apply_failed` and `app_modified`. A retry happens only after an external
  reboot (HIGH-1), and then repeats the whole erase and copy.
- **Suggested fix:** call `recovery_http_boot_guard_clear()` in
  `op_apply_start` before `recovery_apply_esp_start`, and refuse with a
  distinct reason if it fails.

### LOW-2: the unattended apply never invalidates the recovery `app` verify cache, so status can report a half-written `app` as valid

- **Where:**
  - The cache is in `recovery_http.c:126-145`.
  - It is invalidated only by the upload route (`:227`, `:231`), the manual
    apply (`:1125`) and the exit route (`:771`).
  - The status route fills it whenever no apply is running (`:489-491`).
- **Fault scenario.** A status GET lands between the HTTP start and
  `recovery_install_boot()`, in the AUTOFX LOW-4 window (`recovery_main.c:95`
  to `:128`). It caches `valid=true` for the old app. The unattended apply
  then fails after modifying `app`.
- **Consequence.** `GET /api/recovery/status` reports `app_valid: true`, and
  `sw_reset` says "resetting" instead of warning (`recovery_http.c:894`), for
  an `app` that is partly erased. `recovery_exit` is still safe, because
  `esp_ota_set_boot_partition` re-validates and returns
  `ESP_ERR_OTA_VALIDATE_FAILED` (`:768-772`). The status, though, misleads
  any client that decides from it.
- **Suggested fix:** export an invalidate hook and call it from
  `op_apply_start`. Alternatively, key the cache to an apply generation
  counter that `recovery_apply_esp_start` bumps.

### LOW-3: a request left behind by a cut or a failed undo is never dropped by the app, and replays at a later recovery entry

- **Where:**
  - `install_undo` only logs a failed erase (`update_install_http.c:143-148`).
  - Nothing at app boot clears a leftover `upd_inst/req`.
  - A later `recovery_switch_at_boot_threshold`
    (`recovery_switch.c:90-137`) or bare `recovery_enter` boots recovery,
    which applies it (`update_install_record.c:199-258`).
- **Fault scenario.** One of:
  - A power cut between `update_install_prepare` writing the request and
    `recovery_switch_select_boot`.
  - A failed select_boot or reboot-task create whose undo erase also fails.
  In each case the stage stays VERIFIED with the same sha.
- **Consequence.** Days later, a crash-loop or manual recovery entry silently
  installs an update the operator saw refused or abandoned. The result
  record is truthful about it, but the install was not requested at that
  time.
- **Suggested fix:** at a healthy app boot (`ota_confirm` success), erase a
  leftover request. Recovery can only see a request that was meant for this
  entry if the app selected recovery right after writing it, and in that
  case the app is not running.

## INFO

- **INFO-1: a failed or cancelled download destroys a previously verified
  stage.** `flush_head` erases the header sector before the first image byte
  (`update_stage.c:340`). After a Wi-Fi drop, timeout, rate limit,
  `low_heap_midrun` or cancel, the stage is blank, never half-valid. This is
  by design (one stage), but an unattended client must re-fetch before
  installing, not assume the old stage survived.
- **INFO-2: OK-result write failure after the request is cleared.**
  `recovery_install.c:155-158` only logs it. `update_install_prepare` erased
  the previous result (`update_install_record.c:305`), so the app reports no
  result rather than a stale one. That is truthful as "unknown", and better
  than a stale OK.
- **INFO-3: fetch-path faults end correctly.** They are refused or failed
  with a distinct reason, the stage is left blank, the update claim is
  released and the app keeps running:
  - TLS/DNS/connect failures (`connect_failed`).
  - Redirect checks (`redirect_*`, `update_redirect_check`).
  - 403/429/5xx with backoff.
  - 404 on latest (`no_stable_release`).
  - Truncation (`short_body` / `overrun`).
  - `size_mismatch` against the release asset size.
  - Wrong chip (`flush_head`'s `ota_esp_image_header_check`), wrong project,
    a dirty or mismatched version, `sha256_mismatch`.
  - Clock not set (`clock_not_synced`).
  - Low heap (`heap_admit`, `low_heap_midrun`, `low_heap_dma`).
  - Heat run active.
  - Concurrent requests: the `fetch_busy` atomic, the update claim held
    across the recovery reboot, and the stage `claim`.
  - Oversize is unreachable after staging: stage capacity (4 MiB minus 4 KB)
    is below the `app` size (4 MiB, `partitions.csv:278-279`).
- **INFO-4: power cuts in the stage and copy phases.** Each ends safely:
  - Mid-download, mid-erase, mid-verify, or mid header write: blank or
    bad-header stage, app unaffected.
  - Mid-copy or mid app-verify in recovery: otadata still selects factory, so
    recovery re-applies, bounded by the attempt counter.
  - Between `set_boot` and the stage header erase: the new app boots and
    OT-G06 clears the stale stage.
  The only cut windows that end badly are MED-2 (request consumed before
  set_boot) and MED-3 (first-boot rollback).

## Fault matrix

| Fault | Device action | Bootable known state without a human | Record truthful |
|---|---|---|---|
| Cut mid-download / mid stage-erase / mid stage-verify | stage blank or bad header; app keeps running after reboot | yes | yes (fetch status is RAM; nothing installed) |
| Cut mid header write | CRC fails, not installable | yes | yes |
| TLS/DNS/HTTP error, bad redirect | job FAILED with a distinct reason, claim released | yes | yes |
| Rate limit / 4xx / 5xx | FAILED plus backoff | yes | yes |
| Truncated / overrun / wrong size | `short_body`/`overrun`/`size_mismatch`, stage aborted | yes | yes |
| Wrong project / chip / version | refused before any flash write | yes | yes |
| Stage corrupted after VERIFIED | recovery STAGE_HASH, old app intact, stays in recovery | **no** (HIGH-1) | partly (MED-1) |
| Concurrent fetch / upload / install / apply | refused (busy, claim, atomic) | yes | yes |
| Low heap in app fetch | admission or mid-body abort | yes | yes |
| Low heap at recovery apply start | `apply_failed`, stays in recovery | **no** (HIGH-1) | partly (MED-1) |
| Clock not set | `clock_not_synced` refusal | yes | yes |
| Wi-Fi drop mid-fetch | `timeout`/`read_failed`, stage blank | yes | yes |
| Cut after app selects recovery, before the apply | recovery applies the request | yes | yes |
| Cut mid apply copy / app verify | re-apply on the next boot, at most 3 tries | yes (bounded) | yes |
| Copy-stage failure without a cut | stays in recovery, no retry | **no** (HIGH-1) | partly (MED-1) |
| Cut / `set_boot` fail after pre_boot, before the otadata switch | stays in recovery, request gone | **no** (MED-2) | **no** (MED-2) |
| Cut between `set_boot` and stage header erase | new app boots, stale stage auto-cleared | yes | yes |
| New app not confirmed on its first boot, then any reset | rollback into recovery | **no** (MED-3) | **no** (MED-3) |
| 3 attempts used up | `attempts_exceeded`, stays in recovery | no (by design) | yes |

## Resolution (otalow, 2026-10-15)

- LOW-1: fixed. `op_apply_start` (recovery_install.c) now calls `guard_clear()` before `recovery_apply_esp_start()` and returns `UPD_INST_DETAIL_START_FAILED` when the clear fails, so `app` is never erased on an unproved boot_guard clear.
- LOW-2: fixed. `op_apply_start` calls the new `recovery_http_app_verify_invalidate()` before the apply starts. The verify cache is now guarded by `s_verify_mux` plus a generation counter: `app_image_verified` verifies outside the lock and publishes only if the generation is unchanged, so a non-httpd task invalidating the cache cannot be overwritten by a stale verify. Pinned in `test_recovery_lcd_policy.c` (source order, lock and generation test); `check_recovery_lcd_policy.ps1` mutants `apply_guard_unchecked`, `apply_no_invalidate`, `verify_publish_ungated`, `verify_unlocked` all fail as required (the script now swaps any `recovery_*.c` mutant in as a scanned source).
- LOW-3: fixed (app side). The confirm task drops a leftover request on a healthy confirmed boot, see REVIEW_OTA_BOOT_FIXES LOW-4.
- Refusal paths: `update_install_boot_run` refusals that drop the request now go through `refuse_dropping()`; a failing `req_clear` is reported as `clear_failed` (STAY) instead of being ignored. Test `test_refusal_clear_failure`; negtest CAUGHT.
- LCD truncation: `s_warn` and the failure line buffer grew from 28 to 41 bytes; `update_install_reason_lcd()` names the reason, and `test_reason_lcd_fits` pins `UPD_INST_REASON_LCD_MAX` (24). Negtest (over-long text) CAUGHT.
- MED-3: recovery side fixed (otamed3). Result record gains version 2, written only for the new reason `rolled_back` (ordinary records stay v1, so older images still read them; older images report a v2 record unreadable, never misread; a v1 record carrying a v2 reason is refused). `recovery_install.c` `load_last()` rewrites a last OK result to FAIL/`rolled_back` when `esp_ota_get_last_invalid_partition()` is `app` (`update_install_res_apply_rollback`), tests `test_res_versions`, negtest CAUGHT. App side fixed (aa3955376): `boot_confirm_step()` (boot_guard.c) re-samples `nvs_ok` live every poll until confirmed and retries `esp_ota_mark_app_valid_cancel_rollback` with bounded backoff and read-back, logged each failure; `web_ok`/`ota_ok` are route-registration results fixed for the boot (a missing httpd cannot appear later), so they stay fixed. The boot_guard counter is still cleared only from the confirm task. Tests `test_boot_confirm_step_*` in `test_boot_guard.c`; negtest mutants no_resample and no_mark_retry CAUGHT by assertions. Recovery image build and size check verified. MED-3 fully fixed.
