# Completed work -- October 2026

Closeout notes relocated out of `ROADMAP.md` per its maintenance rule. All items
below landed on `origin/main` on 2026-10-02 and are host-tested and target-built
only unless a section below says otherwise. **Verified on hardware 2026-10-03**
(`docs/BENCH_TEST_LOG.md`): the S7 guard's 409, recovery entry and exit, and the
unauthenticated LCD-passphrase recovery AP. Everything else here is still
unverified on hardware (the open bench items live in the ROADMAP index rows for
the recovery image, the OTA matrix and the bench test system).

## A3: `crash_report/clear` on `http_async_job` (`32fe5cee`, `689f0f24`)

The coredump erase behind `POST /api/crash_report/clear` now runs on the
`http_async_job` task instead of `httpd_worker`. Same one-POST/one-response
wire contract, a new 503 busy reply, and `GET /api/crash_report` gains
`clear_in_progress` on its `present:false` reply; no new route. Review fixed the
job's stack grade and a GET doc comment. Before the change the bench measured
a 3.4 s httpd stall; the after-change stall is unmeasured.

## S7: single-flight guard for Pico safety-config writers (`beaab290`..`b8c3e4a8`)

One guard serialises every writer of the Pico safety config, wired into the
async job, the sweep, the swap and the reconcile path. The spinlock has not run
on hardware.

Residual closed afterwards: the synchronous HTTP writers (the five
`safety_cfg_http.c` SET_PARAM/commit handlers and `zones_post_handler`) now
claim the guard under a new `HTTP_SYNC` owner for their whole write, release it
on every return path, and answer 409 busy when the claim fails. The kiln config
apply and backup import already claim atomically at admission (swap submit,
async try_start), so they need no extra claim. The swap worker's boot-recovery
claim retries up to 20 times, 100 ms apart, before running unclaimed. Host tests
cover the 409 for every other owner, release on every path and the bounded
retry; negative-tested by removing the release in both HTTP files, which failed
`zones_http` and `safety_cfg_http`. Hardware 2026-10-03: with a zone current
sweep holding the guard, a commissioning POST answered 409 "another
commissioning operation is running" in 0.34 s and the config CRC did not change.
Two HTTP_SYNC callers racing each other were not observable (httpd serialises
them). Landed as `f42ca1c8`;
Opus review `a0d59984` raised `check_all_task_stack_budgets.py`'s
`http_async_job` ceiling to 4576 B (the 48 B `safety_cfg_writer_release()`
call on that task's path had already pushed it past the old 4528 B on main).
Behaviour change to note: the five `safety_cfg_http.c` handlers used to answer
busy with HTTP 200 and `ok:false`; they now answer 409 with the same JSON body,
so `safety_cfg_http_client.post_commissioning()` raises `SafetyCfgHttpError`
on busy instead of returning `ok:false`, and the relay-type page no longer shows
"Saved." on a refused write.

## Recovery entry (`e25d8a30`..`24043ba9`)

`POST /api/ota/esp/recovery_boot` is the deliberate way from the application
into the recovery image; the boot_guard threshold now switches into recovery
the same way, and the `recovery_enter` MCP tool wraps the route (facade count
206 to 207). The boot target is restored on SET_FAILED. Hardware 2026-10-03:
`recovery_enter(confirm=True)` answered 200 and the board booted the recovery
image; `recovery_exit` returned it to `app` with boot_guard cleared and verified.
The boot_guard threshold switch itself was not exercised.
The ESP upload path (`recovery_push_esp_image` over the recovery AP) was also verified on hardware 2026-10-03 (`docs/BENCH_TEST_LOG.md`, "ESP upload through the recovery image (W5)").

## Pico bootloader hardening (`79264f5f`..`d7d6e9fc`)

The bootloader checks slot linkage before PENDING_VERIFY, `UPDATE_STATUS`
carries a slot trailer, and an 8 s watchdog is armed before the jump to the
app. Host test for `recovery_update.c`, plus the recovery ESP power-cycle
mapping helper and slot/end-state tests.

## Recovery image hardening A-G and hold watchdog (`7a3331a8`..`d4eac0f6`)

Wi-Fi degrades instead of rebooting in a loop, honest `sw_reset`/`recovery_exit`/
`wifi_reset` failures, richer status (uptime, reset reason, OTA state, coredump,
otadata, AP event counts), a per-client nonce ring, relays and LCD pins held
under a 1 s verifying watchdog, a latched hold fault reported in `relay_fault`
and on the LCD, `sw_reset` no longer writes otadata, and host tests for the
upload path and relay-hold logic. Hold-watchdog task stack margin, measured on
the bench 2026-10-03: `relay_hold_stack_free` 1952 B of 3072 B, no hold fault.

## Recovery image unauthenticated, LCD-only passphrase (`00e99237`, `581278ba`)

Owner decision 2026-10-02: the recovery image has no route authentication; its
SoftAP uses a random per-boot passphrase drawn from the RNG entropy source and
shown only on the LCD. `KILNCTL_RECOVERY_AP_PASSPHRASE` is a documented
convention for a human or joiner automation to supply the LCD passphrase; no code
reads or prints it, by design (never a call parameter, never echoed).

Hardware 2026-10-03: the LCD showed the SSID, a 12-character passphrase and the
AP IP on one 480x320 page without scrolling; the SSID `kilnctl-recovery` appeared
in a Wi-Fi scan as WPA2-Personal; a PC joined with the LCD passphrase and read
`GET /api/recovery/status` (200, `auth_mode` "lcd_passphrase"); the passphrase is
absent from the status JSON. Not exercised: the "wifi_storage_fail" path, ESP and
Pico uploads, `wifi_reset`.

## `recovery_status` rendering (`c7bd87d9`)

The MCP tool renders the recovery image's new diagnostic keys.

## Bench runner: trip-clear poll window

`wait_for_trip_clear` (`tools/PcTools/src/kilnctrl/bench_test/cases_smoke.py`, shared by
HP-07 and FL-11) polled `safety_get_diag()` for only 3.0 s after `safety_clear_trip()`.
The ESP serves a cache of the Pico's 2000 ms DIAG push and its context age is observed up
to ~3.5 s, so a working clear could still read `trip_reason` 6 at the deadline: HP-07
FAILed in run 20261003T032924Z_heat_b3_hp after 3.04 s, and a manual clear minutes later
read 0. The window is now 10.0 s (interval unchanged, 0.3 s). The helper also records
`clear_ack` (the return string of `safety_clear_trip()`) and `trip_reason_timeline`
(`(elapsed_s, trip_reason)` per poll) into the case's observed dict when given
`observed=`. Fake-board tests: `tools/PcTools/tests/test_bench_test_trip_clear.py`.
## CT commissioning steps 0-6 closed (2026-09-06..2026-09-19)

Steps 1-5 landed 2026-09-06 (editable calibration fields, auto idle-offset, `ct_topology`/S15/summed sweep on both processors, real-amps display, docs); step 0 (noise-floor capture) was recorded 2026-09-06 and step 6 closed 2026-09-19 by owner decision as a software walkthrough only (the live sweep returned its expected INCONCLUSIVE, ~23 mA/zone being under the 45 mA floor). S3/S4/S9/S14/S15 stay DORMANT on this bench; only a real load can arm them (hardware-gated). Plan: `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md`.

## High-temperature validation firing and field-update ESP half (2026-09-05)

`94b1a2a` confirmed on hardware that ff_hold is infeasible above 62 C (`PID_EXPANSION_PLAN.md` section 3.6i). The same day the ESP half of the field-update exercise passed on the bench: OTA into `ota_0` and rollback both verified, PID gains byte-identical before and after. The Pico half is still open under M8.

## LittleFS `cfg` partition migration status (history)

~~LittleFS for flash writes, 2026-09-06~~ — assessed **not adopted** for log retention, then **superseded 2026-09-07**: endurance review confirmed no wear problem exists either, but the owner directed the migration anyway for architectural reasons (structured, inspectable, backup-able user data). In progress — zones config, profiles, and prefs dual-write to a new `cfg` partition; see `docs/CONFIG_FILESYSTEM.md` for state and open items, `docs/LITTLEFS_ASSESSMENT.md`/`docs/FILESYSTEM_USER_DATA_PLAN.md` for the history. **RP2040 `config_store` A/B sectors (flash_endurance_review_2026-09-07.md R2): implemented AND FLASHED, `b7af9ebe` 2026-09-08** (bench-verified: commissioning config read back byte-for-byte across the migration, CRC unchanged) — this row previously read "NOT YET FLASHED" and was stale by a day. **The separate in-RAM-cache defect found 2026-09-09** (`s_cached_record` read with no synchronisation across cores, `safety_config_version` observed changing mid-read during a live heating run), fixed by a seqlock (`b202fe56`, barrier fix `5671ee03`, then a writer-owned-fallback correction `cb1ba325` after review found the first fix's own fallback snapshot could itself race two readers, 169+ host tests including a torn-read reproduction: 20,945/190k writes without the fix, 0 with it) — **IS now flashed** (`ae23aba4` 2026-09-09, `b88ea6ba` 2026-09-10; corrected 2026-09-14 roadmap truth-up — live `safety_get_fw_version` reads Pico build `d957d5fd`, far newer than either landing commit, commissioned). This row previously said "none of the three seqlock commits have been flashed" and was stale. See `docs/CONFIG_FILESYSTEM.md` for detail.

## Whole-kiln setup wizard (DONE 2026-09-09)

**Whole-kiln setup wizard — NEW, owner request 2026-09-08.** One web page, `/setup`, guiding a new owner from a blank board to a kiln `/api/readiness` reports ready: network/time/units, zones + thermocouples + types, zone type (HEATER vs ON_OFF_DEVICE), relays and names, zone commissioning limits, the safety processor's own commissioning, current sensing + CT verification under load, autotune and the coupling matrix. Modelled on `safety_commissioning_page.html`'s guided flow (stepper, consequence-bearing radio cards, read-back-verified commit) and backed by the existing `/api/readiness` checklist rather than a new model. **DONE (2026-09-09).** All 13 wizard steps and all 11 implementation steps shipped; progress persisted in NVS (not `cfg`) so a filesystem problem cannot lose it. Two steps apply heat (CT sweep, autotune) and five need the owner present. **Follow-up (2026-09-19) resolved (2026-09-21):** the coupling-matrix-step-removal rewrite (`3e9ee5f5`) landed and merged into `origin/main`, and `c339ad16` dropped the temporary `check_no_bench_text_in_ui.ps1` whole-file allowlist for `setup_wizard_page.html` in the same rebase; no allowlist entry for that file remains today.

Plan: `docs/SETUP_WIZARD.md`.

## KilnFW web POST handlers off the shared httpd worker (all slices done)

**KilnFW web POST handlers off the shared httpd worker** (owner approved 2026-09-25; KilnFW TODO.md 10.14 "Web side"). A1 landed 2026-09-25: `http_async_job` single-flight helper (`httpd_req_async_handler_begin`/`_complete`), first used by `ct_auto_zero_post_handler()` (about 10-15 s today), host-tested and negative-tested. **2026-09-25 fix-then-push review**: every existing body and status is unchanged, plus one new synchronous refusal for a concurrent second POST; corrected the stack ceiling to a real measured 3312 B (was a wrong hand-borrowed 2736 B, stack raised 4096 -> 6144 B); serialized `commissioning`/`relay_type`/`ct_cal`/`ct_trim`/`rate_guard_auto` POST handlers and `backup_import` against A1's async window; fixed a `s_task_handle`/`s_busy` clear-order race. W1 (`wifi_prov_owner` replying before its blocking scan and join) landed, pending bench verification. A2 (`bench_preset` onto A1's helper) landed 2026-09-25: same `http_async_job` pattern, byte-identical response bodies, stack unchanged (6144 B declared / 3312 B measured lower bound), `.dram0.bss` 99672/101000 B unchanged (the feature is `#if CONFIG_KILNCTL_DEV_TOOLS`, off on the real board), host-tested (271/271 checks, previously uncompiled/untested by the host suite -- reached via a `CONFIG_KILNCTL_DEV_TOOLS` host-stub override) and negative-tested. A4 (`backup/import` onto the same helper) landed 2026-09-28, driven by a real bench A4 finding (all three restore attempts timed out client-side, board unresponsive to other requests for several polls after each): mode-gate/interlock/busy checks and header reads stay on `httpd_worker`, the body read plus `backup_import_apply()`'s two-pass validate-then-commit move to `backup_import_job()` on the same `http_async_job` helper (6144 B), byte-identical response bodies/status codes (`classify_refusal()` needed no changes), `.dram0.bss` unchanged (99720/101000 B, ctx is heap-allocated), URI route count unchanged (163/170, no new route -- this is A1/A2's same-connection-reply shape, not a job-id/poll shape). Fixes the "other requests starve" symptom, not the restore's own duration, so `backup_import_http_client.py`'s client-side timeout was separately raised 30s -> 90s; neither the client nor the MCP tool retries a timed-out import (the bench's first attempt had, in fact, already committed despite the client timeout -- a caller must read the config back, never re-POST blindly). **Bench responsiveness check done 2026-09-28** (was the plan's own open item): ESP flashed `c021ad96` (verified); two no-op `backup_import` merge round-trips (~61 s each, "ok - restored") ran concurrently with a `GET /api/readiness` poller at 1 Hz for 90 s -- 66/66 OK, max 715.8 ms, avg 359.5 ms; zones/readiness/crash/trip state unchanged. The ~61 s is the ~48 per-setter `nvs_save()` calls in the commit loops plus the Pico round trips (safety ceiling guard, `i_normal_a` stage/COMMIT_CONFIG/read-back), not a symptom of the A4 move. **Batching those saves landed 2026-09-28** (`7b107411`, with stack/check follow-ups in `b4bad2c7`/`c021ad96`/`5d0a2756`/`c22ff081`): `backup_import_apply()`'s zone/timing-profile commit loop now uses the `_no_save` setter variants and a single `zones_config_save_now()` call at the end instead of one `nvs_save()` per field, with `zones_config_get_full_copy()`/`zones_config_restore_snapshot_no_save()` giving an atomic RAM rollback on a mid-batch failure; the async job also moved off `httpd_worker`'s stack onto `http_async_job`'s own task (8192 B declared after a stack-budget-checker fix). Host-tested (single-save-on-success, full-snapshot-restore-on-mid-batch-failure, round-trip save count) and negative-tested by hand. Bench re-measured 2026-10-04 on `a1232077`: a no-op restore of a full export now takes about 1.25 s (was about 61 s), zones and readiness bit-identical afterward; the first re-measure (2026-10-04, pre-fix firmware) found heap_internal min_free dipping to 2595 B, below the 8192 B floor, which `a1232077` fixed by moving the import/export, hash and cfg_fs scratch buffers to PSRAM (`persist_scratch_alloc()`, guarded by `tools/check_persist_scratch_malloc_caps.ps1`); on the fixed firmware the same two imports leave min_free at 12335 B from a 17699 B boot low-water. Safety-link `link_reply_us` timeouts (15) accumulated during the window with no trip -- benign, not an open finding: that counter counts status-push gaps, not failed replies (redefined 2026-09-10, `docs/audits/safety_link_get_status_timeout_counter_2026-09-10.md`), and the import's `safety_exchange` calls contend for `xact_lock`, so a few gaps are expected under this load. One of the earlier restore attempts that timed out client-side had zeroed the bench's cross-zone coupling matrix (`fee835fa`/`b4bad2c7`); it is now restored via the new narrow `control_set_zone_coupling` writer to z0=[0,25.42,24.52], z1=[12.44,0,28.69], z2=[8.08,10.81,0]. A3 `crash_report/clear` landed 2026-10-02 (`32fe5cee`, review fixes `689f0f24`; bench measurement: the synchronous coredump erase stalled httpd about 3.4 s, concurrent `GET /api/status` 2067 ms and `GET /api/readiness` 1378 ms; after-change stall unmeasured on bench, expected near zero since the erase now runs on the `http_async_job` task). Same one-POST/one-response wire contract, new 503 busy reply, `GET /api/crash_report` gains `clear_in_progress` on its `present:false` reply; no new route. All slices of this line are done. No route, API or auth-tier changes beyond that.

Plan: `docs/HTTP_POST_OWNER_MIGRATION.md`.

## Source layering and hardware abstraction (M16, CLOSED 2026-09-16)

**Source layering + hardware abstraction** — `drivers/` reorg applied in `9f18ca5` (2026-09-05); HAL Phases 0-4 all done (every interface has a real backend + host fake, every named consumer migrated, include-boundary enforcement is strict, `esp_random.h` classified 2026-09-06). **Closed 2026-09-16: the hardware timing re-check.** All three named measurements (safety-link reply, display frame time, thermo read latency) now have hardware numbers — safety-link reply against the pre-existing 345 ms budget (max 340 ms observed, thin margin, see the 2026-09-14 composition correction on what that counter actually measures); display/thermo have no prior figure to compare against and are recorded as fresh baselines against the ESP32-S3's 300 ms interrupt-watchdog ceiling (max 81 ms / 57 ms observed, comfortably under). No measurable cost from the HAL indirection against any of these bars. Full detail: `docs/HW_ABSTRACTION.md`

Plan: `docs/HW_ABSTRACTION.md`.

## Live profile edit mid-firing (delivered 2026-09-19)

**Edit the running profile mid-firing, from the web UI -- owner request 2026-09-18. DELIVERED 2026-09-19**: `live_profile_page.html`, five ADMIN routes in `profiles_live_http.c`, fork-on-edit, HARD-mode validation, executor pickup, end-of-firing prompt, shared duplicate-name refusal. The LCD Edit-firing page and the LCD end-of-run Discard/Save as/Overwrite page also landed, bench PASS 2026-10-01 (LCD-22..25: live edit adopted by a running firing, decide page, heap floor). Pending bench items: delete the stray test profile "LiveEditTest" in slot 0 of the bench board, and decide which of the stored `kiln_auth` record and the bench env-var credentials is authoritative (the 2026-09-21 clean login returned 401). Plan: `docs/LIVE_PROFILE_EDIT_PLAN.md` section 10.

Plan: `docs/LIVE_PROFILE_EDIT_PLAN.md`.

## M18 commissioning narrative (2026-09-21 to 2026-10-04)

Moved verbatim out of `ROADMAP.md` M18 once every item in it had landed; the open items stay in ROADMAP M18. Stale "not yet flashed" and "still unset" notes were struck or reconciled in the same pass.

**2026-09-23 pending, all blocking the bench reflash + commission pass:**

- [x] Pin the bench's hand-set `sdkconfig` values in `sdkconfig.defaults`
  (Wi-Fi static RX buffers 10, BA window 6, lwIP OOSEQ pbufs 4;
  `GPIO_PROBE` pinned unconditionally, no `DEV_AFFORDANCES`-style symbol
  exists to gate it on) — done, 2026-09-23 (`a513aa75`, watched-key coverage
  `0a8db924`).
- [x] Stale published `build/`/`sdkconfig` sibling guard in the stack-budget
  checkers, plus a `build_kilnfw()` refresh — done, 2026-09-23
  (`91477e4c` fix, `b9f574d3` wired the regression test into
  `run_all_checks.ps1`, `5a72eb50` skips the refresh when ninja did not
  relink).
- [x] `check_00_kilnfw_target_build.ps1` publishes `bootloader.bin`/
  `partition-table.bin`, not just `KilnCtrl.elf`/`.bin` — done, 2026-09-23
  (`d23d4eae`, freshness-gate fixes `51ae1ce0`/`ad2ee170`) — closes the
  second bench-commission gap noted in the twenty-ninth sweep above.
- [x] OTA transient task handles: `vTaskDelete` on the ESP rollback task,
  idempotent `stack_margin_register` — done, 2026-09-23 (`95be3327`,
  opus-review fixes `22fcc257`/`24e59726` comment-only follow-up).
- [x] Bench ESP reflash — done, 2026-09-23: `9c26dd91`, `nvs` erased, web
  auth re-bootstrapped, readiness 16 ok / 2 not_done / 3 other. See the
  top-of-file entry above for findings (D2-D5).
- [x] Bench Pico reflash — done, 2026-09-24: `6bb41fe1` (SaftyFW build
  `d6309f4a`) via `debug_program(peer="pico")` after the owner reseated the
  CMSIS-DAP probe.
- [x] `estop_verified` closed end to end, 2026-09-24 — S7 latched, verified,
  cleared, and recorded via the new `estop_verify` MCP tool (`fc16c186`,
  `055703af`). Class C heat/firing commissioning rows are now unblocked —
  not yet run.
- [x] First real-hardware bench_test heat/LCD/stack/web suite runs, 2026-09-24
  — see the top-of-file entry above and `docs/BENCH_TEST_LOG.md`. Every FAIL
  traced to the runner, not firmware; fixes landed same day.
- [x] Fix LCD-01/08/09/14/16 judge/navigation defects — done, 2026-09-24
  (`0df96d5d`). Further review-fix chain landed 2026-09-25: `d4e7ff29`/
  `f40e8d37` (harness, `cases_lcd.py`) and `0ef18917`/`466b29b2` (firmware,
  PIN keypad key-height regression and an LVGL-off-task call fix -- ~~not
  yet flashed to the bench~~ -- flashed since: the 2026-10-01 rerun below ran on ESP `eb83c1ac`). **Rerun done
  2026-10-01** (`20261001T155811Z_lcd`, ESP `eb83c1ac`, defaults, no heat): LCD-08/09/14/16/21
  PASS, LCD-01 and LCD-19 INCONCLUSIVE (camera exposure/cast; LCD-19 stop_gated
  not exercisable without `allow_heat`), the rest NOT_RUN (not_implemented or
  precondition absent); no FAIL. **Full rerun with heat opt-ins, 2026-10-01** (`20261001T183355Z_lcd_lcdsuite3`,
  `90fc6658`+`e52f256d`): LCD-08/09/14/16/19/21/22 PASS (LCD-16 rewind fix confirmed,
  7/7 pages; LCD-19 stop_gated and LCD-22 exercised), LCD-01 INCONCLUSIVE (camera cast), no FAIL.
- [x] Fix the SK-01/02 noise-tolerance/fw_commit-gate issue — done,
  2026-09-24 (`866003ea`; plan-note follow-up `9d905ab9`). Stack suite re-run 2026-10-04 on `a1232077`
  (`20261004T021004Z_stack`): SK-03 and SK-04 PASS, SK-01 and SK-02 INCONCLUSIVE
  (no baseline records yet). **Owner step pending:** capture baselines with
  `tools/PcTools/scripts/capture_stack_margin_baseline.py` (writes tracked files).
- [x] Capture the `idle`-load stack-margin baseline at the currently running
  commit — done, 2026-09-24 (`5c44ae95`,
  `docs/stack_margin_baseline/stack_margin_idle_111b1b6f_20260924T175921Z.json`).
- [x] Capture the `mid_firing` and `web_ui_open` stack-margin baselines at the
  currently running commit (`111b1b6f`/`6bb41fe1`) — done, 2026-09-24
  (`e18c645d`; `web_ui_open` had never been committed before, so this is its
  first baseline, not a recapture). Only `kiln_io_owner` shrank more than
  256 B against the `75a5e459` mid_firing baseline (2708 to 2100 B free, still
  OK). Both files record `profile_executor` at CRITICAL (468 B of 4096 B
  worst-since-boot) during the firing; a stack raise is dispatched. **Raised
  2026-09-24 (`b1f6c127`):** `profile_executor`'s declared
  stack 4096 -> 6144 B (`profile_executor_start.c`, INTERNAL DRAM -- this task
  writes NVS on its tick path and cannot use a PSRAM stack). `info_uart_bridge`
  (976 B/3584 B, 27.2%) and `lvgl` (1968 B/8192 B, 24.0%) also read LOW in the
  same two captures but both sit above the 15% CRITICAL threshold, so neither
  was raised. `check_executor_task_stack_budget.py`'s own static-path model
  moved from 940 B/22.9% (LOW) to 3148 B/51.2% (OK) at the same measured
  deepest path (1776 B, unchanged); DRAM impact is +2048 B of internal-heap
  task-stack allocation at runtime, not a `.dram0.bss` change (the stack is
  allocated dynamically by `xTaskCreatePinnedToCore`, not a static array) --
  `check_kilnfw_dram_bss_budget.ps1` still passes at the same static `.bss`
  figure (97256 B of a 101000 B ceiling).
- [x] Raise the `info_uart_bridge`/`lvgl` LOW margins noted just above --
  done, 2026-10-01 (`eb83c1ac`, Opus-reviewed): `lvgl` 8192 -> 10240 B
  (internal DRAM; its UI pages write NVS), `info_uart_bridge` 3584 ->
  4096 B (PSRAM). `adaptive_tune_zones[]` (2700 B) moved to PSRAM via
  `EXT_RAM_BSS_ATTR` to fund the internal-DRAM half; net `.dram0.bss`
  99672 -> 99016 B against the 101000 B ceiling (1984 B headroom). Flashed
  and bench-verified same day: `get_stack_margin` now reads `lvgl`
  4640/10240 B free (45.3%) and `info_uart_bridge` 1624/4096 B (39.6%),
  both clear of the 15% CRITICAL threshold. Full detail:
  `docs/BENCH_TEST_LOG.md`'s "lvgl/info_uart_bridge stack raise" entry.
- [x] LCD-19 FAIL on run `20260924T180332Z_full` ("Start tap after the LCD
  timeout did not raise the PIN keypad", `keypad_raised=false`) root-caused
  2026-09-24 as a runner defect, not firmware: `_wait_for_overlay_names(present=True)`
  in `cases_lcd.py` exits on any non-empty tap-target set, and the home
  page's own buttons satisfy it before LVGL processes the click (the case ran
  0.92 s against a 2.0 s timeout). Firmware force-lock-on-enable
  (`ui_lcd_lock.c`, `security_backend_web_auth.c`) is correct. Fix (a
  baseline-then-changed wait) has landed: `_wait_for_overlay_names()` in
  `cases_lcd.py` takes a `baseline` and waits for the set to change (on main at
  59c9306a). **Bench rerun 2026-09-30** (`20260930T043143Z_lcd_lcd19_rerun_0929_59c9306a`):
  still INCONCLUSIVE -- "could not exercise: wrong_pin_refused, right_pin_started,
  stop_gated" even with `KILNCTL_LCD_PIN` set, so the original NOT_RUN/missing-PIN
  case is ruled out but the case still can't complete; under investigation, not
  chased further in that run. Same rerun also surfaced a new LCD-16 FAIL
  (`click_by_name('settings')` -> `not_found`), unrelated to any fix under test
  here; also under investigation. **Reran again 2026-09-30** against harness
  `7550fdf6` (LCD-19 follow-up fixes `1ae70883`/`fab4f456`/`7550fdf6`) on the
  bench board at ESP `50edd830` (`KilnCtrl-6c9152cebb9d`), Pico `405d3c54`:
  `20260930T073410Z_lcd_lcd_rerun_0930_7550fdf6` and a same-day
  `20260930T073503Z_lcd_lcd_rerun_0930_7550fdf6_r2` -- LCD-16 now PASS (7
  pages, no retries; the prior `settings` not-found FAIL did not recur) and
  LCD-14 PASS, but **LCD-19 FAILed both runs with a new failure shape**: every
  PIN digit tap returned `ok` for both the wrong PIN and the correct PIN,
  `wrong_pin_refused=true` (correctly refused), `start_click_result=ok`, but
  `right_pin_started=false` -- the firing never actually started after the
  correct PIN was accepted. No `page_before`/`navigate_home` evidence was
  recorded either run. This is a different failure shape from both the prior
  `keypad_raised=false` FAIL and the `59c9306a` INCONCLUSIVE -- root cause not
  yet found; board was left idle both runs, no trip, no crash. **Harness
  fixes landed 2026-09-30** (`46d9726d` waits for a stable, digit-bearing
  keypad read before typing a PIN; `b2cf2f89` adds `enter_pin_verified()` to
  `ui_test_client.py`, verifying each PIN digit was actually applied before
  typing the next, and only reports `wrong_pin_refused=True` when the
  OK+Cancel dialog is present and its dots are cleared) -- pushed to
  `origin/main`. **Bench reran against `b2cf2f89`** (firmware build "Sep 30
  2026 01:50:10"), three runs: `20260930T103536Z_lcd` FAILed with "keypad not
  raised" after the LCD's own idle timeout, suspected a harness race
  (`_wait_for_overlay_names` has no raised-then-closed detection), fix in
  progress; `20260930T103634Z_lcd` INCONCLUSIVE with only `stop_gated`
  missing -- wrong PIN refused, right PIN accepted, all 6 digits verified,
  but the case never presses the Confirm Start dialog, so the executor stayed
  idle throughout; `20260930T103752Z_lcd` INCONCLUSIVE on
  `keypad_closed_before_entry`. Board stayed healthy across all three (armed,
  no trip, no reboot, no crash). Three gaps remain: (1) `stop_gated` is
  structurally unreachable today -- `pin_cfg`'s `firing_active_with_lock`
  condition is never set because LCD-19 never presses Confirm Start, so no
  firing is ever active to test stopping; needs an owner decision on whether
  the case should press Confirm Start or on another way to arm that
  precondition. (2) The raise-detection race in `_wait_for_overlay_names`
  (harness fix in progress). (3) The keypad's own self-close cause is
  unknown -- candidates in `ui_lcd_lock.c`'s `tick_timer_cb` (inactivity
  expiry, policy disabled, an unlocked->locked edge, or
  `ui_lcd_lock_force_lock`) -- needs a serial log capture on COM14 during a
  rerun to narrow down. **Harness fix landed 2026-09-30** (`4754e91f`): the
  keypad-raise poll now uses a tail-based FAIL judgment (FAIL only when the
  last 2 reads after the last empty/truncated read are identical, real,
  non-truncated and not the keypad), fixing gap (2) above; a keypad that
  raised then closed now reads INCONCLUSIVE with `keypad_raised_then_closed`
  recorded instead of a false FAIL. Reruns on `4754e91f`, firmware `9b77e2b2`
  (built 2026-09-30 08:49:24Z), `allow_heat=False`: `20260930T185935Z_lcd`
  INCONCLUSIVE with only `stop_gated` unexercised (keypad raised, wrong PIN
  refused, right PIN started); `20260930T190017Z_lcd` INCONCLUSIVE with
  `keypad_raised_then_closed=true`, reproducing gap (3) on hardware -- the
  keypad raised ~1 s after the Start tap then the home screen returned, with
  no `ui_lcd_lock` line in the serial log during that window. Board healthy
  both runs. **Keypad self-close root-caused and fixed 2026-09-30**
  (`7a71229b`): `ui_lcd_lock.c`'s `tick_timer_cb` force-locked every tick and
  set `s_was_locked=false` while the lock policy was disabled, so the first
  tick after the policy was re-enabled saw a spurious unlocked->locked
  (relock) edge and closed the keypad the tap had just raised as its own PIN
  gate -- the relock edge now excludes a keypad it raised itself; prompts and
  confirm dialogs still close on relock (owner 2026-09-28 decision,
  unchanged), and every lock-driven close now logs its reason. Flashed to the
  bench from a clean worktree, verified, ELF archived
  `KilnCtrl-95f9e0e194e1.elf`, boot_guard persisted 0/0, no trip.
  `stop_gated`'s opt-in firing landed the same day (`aaae0a23`): a new
  `lcd_stop_heat` mode (runner/MCP param, `bench_test.ps1 -LcdStopHeat`) lets
  the case start a short API-started BENCH_HP firing so `firing_active_with_lock`
  is actually set; teardown verifies idle and relays off, and reasons are no
  longer clobbered. Harness commit `4ecadec6` also fixed
  `_wait_for_home_settled` to judge on target-present/Cancel-absent over
  consecutive reads (min 2.5 s) instead of full name identity, since the
  Elapsed label changes every second; it accepts a truncated read containing
  the target and logs every read. Three bench runs followed: `20260930T200715Z_lcd`
  (old firmware, harness `aaae0a23`) INCONCLUSIVE -- the post-heat home page
  never settled, cleanup verified; `20260930T203132Z_lcd` (firmware
  `7a71229b`, harness `4ecadec6`, `allow_heat=False`) INCONCLUSIVE by design
  (`stop_gated` needs heat) but confirmed the wrong PIN is refused, the right
  PIN starts, and the keypad no longer self-closes; `20260930T203243Z_lcd`
  (`allow_heat=True`, `lcd_stop_heat=True`) INCONCLUSIVE -- PIN flow correct,
  firing started, relock correctly closed only the open confirm dialog (device
  log: "LCD relock edge: closed open confirm dialog"), but all 20 post-relock
  home reads showed `Plan` rather than a `Stop` target, so Stop was never
  tapped; the ~13.7 s firing ended clean, teardown verified idle/relays off,
  no trip, no reboot. Full detail: `docs/BENCH_TEST_LOG.md`'s 2026-09-30
  section. Still open: why the post-heat home page shows `Plan` instead of
  `Stop`, diagnosis in progress. **Root-caused and fixed 2026-09-30**
  (`e388752c`, Opus-reviewed): `UI_TEST LIST_TAP_TARGETS`'s ~253-byte reply
  truncated all 20 `allow_heat_settle_reads` polls before reaching the home
  page's merged Start/Stop button -- the walk order put the WiFi label,
  temperature readings, and the chart legend (`Plan`, not a button) first.
  The confirm dialog the relock closed was a stale Confirm Start from the
  earlier right-PIN sub-check (intended relock behavior, not a bug). Fix:
  `log_all_tap_targets` (`kiln_ui.c`) now walks each group twice, actionable
  targets (`lv_button` and its subclasses, including list rows and msgbox
  footer/header buttons) before non-actionable ones, overlay-first group
  order unchanged; the harness also dismisses a stale Confirm Start via
  Cancel before the API-started firing (`allow_heat_pre_start_dismiss`).
  Flashed to the bench from a clean worktree at `e388752c` (an earlier
  worktree's `build/` lacked `build_info.h` and was correctly refused as
  stale; rebuilding and reflashing verified OK), ELF
  `KilnCtrl-5384de5815b4.elf`, boot_guard persisted 0/0, no trip. Bench runs:
  `20260930T212112Z_lcd` (`allow_heat=False`) INCONCLUSIVE by design
  (`stop_gated` needs heat), wrong PIN refused, right PIN started, settle
  reads OK; `20260930T212155Z_lcd` (`allow_heat=True`, `lcd_stop_heat=True`)
  **PASS, exit 0** -- `stop_gated=true`, settle reads now contain
  `Stop`/`Pause`/`Edit`, Stop click `ok`, `bench_cleanup` verified idle and
  relays de-energized, no reboot/crash/trip. (Correction 2026-10-03:
  `lv_keyboard`/`lv_buttonmatrix` was already reported per key by `log_tap_targets()`; the walk now also skips HIDDEN/DISABLED keys. A keyboard's ~33 keys still compete with the 32-entry array and 253 B reply caps.)
  LCD-19 is now closed/passing. Full detail:
  `docs/BENCH_TEST_LOG.md`'s 2026-09-30 "LCD-19 root-caused and fixed"
  section.
- [x] LCD-09/LCD-16 regression from the `e388752c` LCD-19 fix, found in run
  `20260930T215921Z_lcd` -- resolved 2026-09-30/10-01. Root cause was not
  `e388752c`'s tap-target reordering: `kiln_ui_click_by_name()`/
  `list_tap_targets` dispatch the tap-target walk to `lvgl_port_task` with a
  300 ms wait (`UI_WALK_WAIT_TIMEOUT_MS`), and a timeout on a busy UI task
  returned `n=0`/truncated, which the PC side reported as `not_found` rather
  than "walk didn't run yet" -- the same symptom reproduced pre-`e388752c`
  (run `20260930T043143Z_lcd_lcd19_rerun_0929_59c9306a`, firmware `8ed37d8d`),
  ruling out that fix as the cause. Fixed in two steps: `82de0234` first made
  the PC harness retry a `not_found` (2x, 0.15 s pause, counted separately as
  `not_found_retries`) as a stopgap -- rerun
  `20260930T234916Z_lcd_harness_retry_verify` got LCD-16 to PASS but left
  LCD-09 INCONCLUSIVE (a busy `list_tap_targets` read against the topbar
  anchor, `profiles_count 29` vs `row_count 0`). `232e668f` + `c471101c` then
  fixed it at the source: firmware gained `KILN_UI_CLICK_WALK_BUSY`/
  `UI_TEST_CLICK_WALK_BUSY=0x08` (no UART protocol version bump needed, same
  precedent as OFFSCREEN `0ef18917`), and the PC side gained busy derivation
  for `list_tap_targets` (count==0 and truncated) via
  `_list_tap_targets_resolving_busy()`, used by LCD-09/14/16, plus a
  `walk_busy_retries` counter and host-test source-pattern pins. Flashed
  `c471101c` 2026-10-01 (ELF `KilnCtrl-39bdbebe1651.elf`, running `app`,
  boot_guard persisted 0/0, no trip). Verification: run
  `20261001T011155Z_lcd_walk_busy_verify` -- LCD-08/09/14/16/21 all PASS
  (LCD-01 INCONCLUSIVE on camera color, LCD-19 INCONCLUSIVE by design, no
  heat requested; LCD-14/LCD-16 each needed one `walk_busy_retries`); run
  `20261001T011311Z_lcd_walk_busy_lcd19` -- LCD-19 PASS. Full detail:
  `docs/BENCH_TEST_LOG.md`'s 2026-09-30/10-01 entries for this regression and
  its resolution. **2026-10-01 follow-up:** run `20261001T172420Z_lcd_lcd22run` --
  LCD-19 PASS with `lcd_stop_heat=True` (real firing, Stop PIN-gated); first
  LCD-22 board run FAILed (`click_by_name('Edit')` -> `not_found` while running);
  rerun after `e52f256d` PASSed (`20261001T183045Z_lcd_lcd22rerun`). **2026-10-02:**
  new LCD-23/LCD-24 first run `20261002T013957Z_lcd_lcdedit23_24` -- LCD-23 FAIL,
  LCD-24 INCONCLUSIVE (harness assumed the Edit page opens on segment 0 and the
  executor warm start skipped segment 0); rerun `20261002T021747Z_lcd_lcdedit23_24_r2`
  after `8200e7b1`/`010239ef`/`b7eb48c9` -- both PASS; see
  `docs/BENCH_TEST_LOG.md`.
- [x] Profile/autotune same-zone start race fix (atomic per-zone claim,
  `relay_authority_zone_claim_begin()`/`_end()` reusing `s_heat_claim_mux`) --
  landed 2026-09-24 (`540b2d72`, host tests in `test_profile_executor_prestart.c`,
  `test_autotune_engine_prestart.c`, `test_link_watchdog.c`). Confirmed flashed
  to the bench and reran clean 2026-09-30 (`20260930T043239Z_heat_heat_rerun_0929_773ec669_540b2d72`,
  HP-01..08 all PASS) -- see the top-of-file entry above.
- [x] Root-cause and fix the `profile_executor` dwell-fault panic hit by heat
  run `20260924T085059Z_heat` — done, 2026-09-24 (`3ce065ca` audit, `773ec669`
  fix, `adc7f65c` comment correction; see the top-of-file entry above).
  **Rerun done 2026-09-30, PASS:** heat suite `20260930T043239Z_heat_heat_rerun_0929_773ec669_540b2d72`
  ran HP-01 through HP-08 all PASS, no panic/reboot/trip during the run --
  first clean 8/8 heat-suite PASS recorded for this fix pair.
- [x] Add a bench board lock so concurrent heat/mutating runs cannot share one
  board — done, 2026-09-24 (`d5bfac19`, `759660c2`; see the top-of-file entry
  above).
- [x] Stop rebooting on an `exec_mode_state_check()` violation -- done,
  2026-09-24 (`002e71bd`, review fixes `118beb79`): target builds now latch
  FAULTED and log instead of asserting; host/debug builds keep the hard
  assert. See the top-of-file entry above.
- [x] Narrow `exec_enter_terminal_state()`'s zone-active clear to the IDLE
  transition only -- done, 2026-09-24 (`4012f8c7` then `fe938ef3`); FAULTED/DONE
  readers (`force_all_relays_off()`, firing-stats finalize, status JSON, fault
  clear) all depend on `active` staying set past those transitions. See the
  top-of-file entry above.
- [x] Harden the bench board lock -- done, 2026-09-24 (`8e696d78`): serialize
  reclaimers on an O_EXCL mutex file, publish-then-check ordering on both
  reader and mutating sides. See the top-of-file entry above.
- [x] Fix the LCD bench runner click-then-read race and per-frame capture
  naming -- done, 2026-09-24 (`93355ee9`). **Reran 2026-09-30**
  (`20260930T043143Z_lcd_lcd19_rerun_0929_59c9306a`): LCD-08/09/14/21 PASS,
  LCD-01 INCONCLUSIVE on camera exposure/cast (pre-existing limitation, not
  a firmware defect); no recurrence of the click-then-read FAIL shape this
  fix targeted.
- [x] Replace the email forgot-password design with TOTP -- done, 2026-09-24
  (`84fa2e7b`, owner change); WT-C and WT-D landed; **WT-B landed**
  (`62f8bd4e`, gesture follow-up `cafc80f3`); WT-A (firmware routes) landed
  `f5f06dee`..`9708015f`. See the top-of-file entry above.
- [x] `check_flash_worker_lint.ps1` red over `totp_config.c` -- green on main
  at `9708015f` (re-run 2026-09-24).
- [x] **S6a boot-clear one-shot fixed, 2026-09-28 (review find):**
  `safety_link_frames.c`'s stale-S6a boot-clear latched permanently on the
  first successful CLEAR_TRIP *send*, not on Pico *acceptance* -- CLEAR_TRIP
  has no on-wire ACK, and the Pico's `safety_guards_try_clear()` refuses
  while its own S6a release debounce (200 ms) hasn't yet elapsed, made more
  likely by `SAFETY_FAULT_MIN_HOLD_MS`'s >=300 ms fault-line hold. A refusal
  inside that window used to burn the one-shot with a releasable trip still
  latched. Replaced with a small bounded, spaced retry (3 attempts, 2 s
  apart, still inside the existing 30 s boot window, anchored to the ESP's
  own boot rather than the Pico's boot_id/link-up); "confirmed acceptance"
  is the next DIAG frame simply no longer reading TRIPPED/MAIN_FAULT --
  the existing gate condition, no new signal needed. All prior guards
  (S6a-only, `fault_sources==0`, the 30 s window) unchanged. Host-tested
  (refused-then-retried-succeeds, persistent-refusal-gives-up-after-bound,
  non-S6a-never-cleared) and negative-tested. Review fix: once a clear has
  gone out, a DIAG showing the trip gone or any new own-fault-source rising
  edge closes the window.
- [x] **Full-precision numeric printing, 2026-09-28 (`04fb2afe`):**
  `GET /api/zones` and backup export used to print PID gains, `model_k_dc`,
  `coupling_diag_k_dc` and `coupling_c%u` at `%.4f`, silently rounding a
  small `Ki` (or other small-magnitude value) to zero on read-back. Both now
  print at `%.9g`. The narrow MCP writers (`control_set_zone_limits`,
  `control_set_zone_type`, `control_set_zone_coupling`) no longer zero a
  small `Ki` on their GET-merge-POST round trip, and OTA-bench PID
  comparisons are now tolerance-based (`judgments.pid_gains_match`) instead
  of exact-string.
- [x] **`tuning_valid` invalidation tolerance, landed (`49b32a24`):** the
  compare in `zones_http_post_parse.c` that decides whether a re-posted
  value counts as "changed enough to invalidate `tuning_valid`" used an
  absolute `0.0001` tolerance, so a small-`Ki` edit (now printed and
  re-posted at full `%.9g` precision rather than being rounded to zero)
  could fail to clear `tuning_valid` even though the value genuinely
  changed. Now a relative tolerance; also carries a magnitude-sweep test and
  a comment fix from review.
- [x] **`backup_import` batched NVS save, landed (`c22ff081`, `5d0a2756`,
  `7b107411`).** The ~61 s import used to issue one NVS save per field as it
  restored each store; it now uses `_no_save` setter variants for the hot
  paths with one trailing save per store, a RAM rollback if a mid-batch
  setter fails partway through, and tracks the Pico's `abs_max_temp_c`
  ceiling down after import/rollback. **Bench-measured 2026-09-30:** a
  fresh-export/import round trip (`backup_export()` -> `backup_import()`)
  completed in 0.80 s (synchronous route), down from the ~61 s pre-fix
  measurement; readiness unchanged before/after.
- [x] **Web dashboard: drop the "Previous firing ended" card after a normal
  or deliberate stop, landed (`fd792793`).** The card now shows only for an
  *unexpected* end (fault/trip/crash) — a normal Stop or a profile reaching
  its own end clears it instead of leaving it displayed.
- [x] **Divergence-triggered stop recorded as FAULTED, landed (`c26df2ce`,
  `057ca631`).** Was recorded as HALTED with an empty reason; now records
  FAULTED with a reason. `057ca631`'s review fix keeps a DONE run DONE
  instead of overwriting it if a divergence appears after completion.
- [x] **Owner requests, 2026-09-28:**
  - (a) **Landed (`d484e51a`, `f017b285`):** an "Edit firing" button next to
    Start/Stop on the web UI opens `/live_profile`, prompting for login
    first if not already signed in (backend: `profiles_live_http.c`,
    `docs/LIVE_PROFILE_EDIT_PLAN.md`'s five routes, the `profile_live_*` MCP
    quartet). **LCD counterpart landed 2026-09-28** (`ebbbd34f`, review fixes
    `1a40133a`, `e35e1aff`): a live-edit-current-firing LCD page (guarded
    apply, refresh, heap state; `e35e1aff` clears a stale status line when
    the idle poll picks up a new firing after the page was opened).
  - **LCD relock on web credential change, landed 2026-09-28** (`33ecc6d5`,
    review fix `7783066c`): `ui_lcd_lock_force_lock()` made safe to call from
    the httpd task, firing a relock edge and gating a pending LCD request
    when the web password changes underneath an unlocked panel.
  - **Bootstrap-password gate redirect, landed 2026-09-28** (`da0accac`,
    review fix `8e3762e0`): fixed bootstrap_password being unreachable on a
    gated page (regression from `d25d5ccf`'s dashboards-only gate); review
    fix covers a first-load race and a lost `return=` param.
  - **`full_board_backup.py` now goes through web auth, landed 2026-09-28**
    (`91ed5818`): fails the run on any 401 instead of silently continuing,
    and reports a partial cfgfs restore rather than reporting success.
  - **Backup-restore false ceiling-divergence trip, fixed 2026-09-28**
    (`c35e2e8a`, review fix `8f777f1b`): a restore in flight now skips
    ceiling-divergence enforcement only for the latch, not for heat-off
    enforcement, which stays active throughout the restore.
  - (b) A confirm dialog before stopping a running profile, on both the web
    UI and the LCD — this already existed on both UIs before this round of
    requests (a prior status line here mistakenly listed it as still
    pending in `1037c4ff`; corrected). **Landed:** Stop itself now requires
    login on both UIs, per (c) below. The physical E-stop stays immediate,
    always available without login, and bypasses both the confirm dialog
    and the login requirement entirely.
  - (c) **Owner decision, supersedes the earlier "show login only when an
    action needs it" request. Landed on both UIs (`48962395`, `d25d5ccf`,
    `5901b09d`, `89fbe6d6`, `e4c5d7da`):** without login, the web and LCD
    show only the dashboards. Every other page and action — including
    Stop — requires login (`/api/profile_exec/stop` moved to USER tier,
    `/api/board_temps`/`/api/firing_history` to ADMIN; a `59e84b57`
    follow-up also moved `current_sweep/abort`, `autotune/abort` and
    `diagnostics/danger/stop` from SAFETY_REDUCE to ADMIN, so those three
    now need login too — the physical E-stop is the unauthenticated
    backstop for all of them). The LCD's PIN gate
    follows the same rules as the web password: the shared
    `login_backoff` module's lockout/backoff ladder (5/10/30/60/300 s) and
    the same idle-timeout semantics; the keypad's lockout text now states
    the actual wait ("Wrong PIN -- try again in Ns"). Setup/AP
    provisioning, the bootstrap-password flow, TOTP reset, and the LCD
    reset touch sequence stay reachable without login, since they exist to
    recover access in the first place. `/wifi`/`/networks`/`/scan` are a
    separate, narrower case: open only while the board is unprovisioned
    (`59e84b57`, `f3991c09`), ADMIN once provisioned, matching the
    captive-portal first-time-setup requirement.
  - **Wi-Fi AP fallback with auto-reconnect, landed 2026-09-28** (`8746e02d`,
    fixes `f4675a7e`/`ddebcf8c`, review fix `be7bcad4`): falls back to AP mode
    on home-network loss, reconnects when no web user is logged in. Owner
    decisions: keep the 30 s probe cadence while a user is logged in; with
    auth off, the AP stays up while any station is connected to it, not just
    while a user is logged in. `59e84b57`/`f3991c09`'s `/wifi` setup-tier gate
    (open only while unprovisioned) landed the same day and is unaffected.
  - **Fix, 2026-09-29:** `ap_teardown_should_defer()` deferred AP teardown for
    ANY active admin session, including a LAN-only one (e.g. the PC's own MCP
    tools reaching the board over the home LAN) -- so it stayed up with zero
    AP clients connected. Fixed by tagging each web session slot with
    `via_ap` (set at login; re-set on every successful touch, though in
    practice the login IP binding means a real session's tag never actually
    changes after login -- see `web_auth_session.h`'s field comment) and
    checking a new AP-scoped signal, `http_auth_any_ap_session_active()`,
    instead of "any session anywhere". A LAN/STA session now never defers
    teardown by itself. Owner-accepted judgment call: with auth on, a
    station physically associated to the AP but not yet authenticated
    (mid-login) still defers teardown, so a connecting operator is never
    stranded. **Review-fix round 2, same day:** `wifi_prov_request_arrived_on_ap()`
    always returned false on hardware (`CONFIG_LWIP_IPV6=y` means the httpd
    socket is PF_INET6, so `getsockname()` hands back an IPv4-mapped
    AF_INET6 address, not a plain AF_INET one) -- fixed to handle both
    shapes, which also fixes the `/status` `ap_password` field that gates on
    the same detector. Also: an AP-tagged session under a
    `WEB_AUTH_TIMEOUT_NEVER_S` policy now defers teardown only while an AP
    station is also actually associated, so one AP login under a never-expire
    policy can't pin the AP up forever.
  - **Live-edit feature bench-verified end to end, 2026-09-29 PASS** — see
    M18 item 3 above and `docs/BENCH_TEST_LOG.md` Test A.
  - **AP fallback/restore cycle bench-verified, 2026-09-29 PASS**
    (`docs/BENCH_TEST_LOG.md` Test B: home lost, board's own SoftAP came up,
    home restored, saved-network list matched baseline, no reboot). Still
    open: AP-fallback radio timing and the LCD's "[AP kept up]" render were
    not confirmed this run (LCD capture came back unreadable/black).
  - **Pending bench list of 2026-10-01, resolved items:** the LCD edit-firing page was
    hardware-verified (LCD-22..25 PASS, 2026-10-01); `wifi_prov_owner` stack margin under
    AP-fallback probing measured 2026-10-01 on `eb83c1ac`, min 1344 B free of 4096 B
    (32.8%, OK) vs. 1536 B idle baseline, see `docs/BENCH_TEST_LOG.md`. Items still open
    (login gates, AP-fallback radio timing, the LCD "[AP kept up]" render, `crash_report/clear`
    latency) stay in ROADMAP M18.
  - **`d3c4c826` AP-teardown fix, bench-verified PASS, 2026-09-30:** a
    decoy network was saved, the real one forgotten, mode cycled ap then
    home, and the board reached `state=reconnecting` with the LAN down.
    Restoring the real credentials rejoined within 26 s at `.156` with
    `ap_pending_teardown=False` on the first read after rejoin. A `GET
    /status` from the LAN admin session showed `ap_password` empty and
    `ap_password_known=false`, confirming the AP tears down after rejoin
    even with a LAN admin session active, and that a LAN client never sees
    the AP password. The companion AP-client case (a station associated to
    the SoftAP seeing `ap_password` in `/status`) was SKIPPED -- the bench
    PC has no free Wi-Fi adapter to join the SoftAP with.
  - **AP-password HMAC retired on the nine main-app admin routes, landed
    2026-09-30** (`a7b3e436`, review fixes `1822ab02`, build-failure fix
    `cae52ae9`, pytest fixes `429033f8`/`fd8fc308`, zero-caller allowlist
    `7cdb9539`): those nine routes (OTA/reset/boot_guard admin actions) now
    require only an admin web session and are reachable unauthenticated
    when web auth itself is off, matching every other admin route -- the
    AP-password HMAC challenge–response these routes used is gone. The
    recovery image's own HMAC (a separate, unrelated mechanism) is
    unchanged. `flash_firmware()`'s post-flash `boot_guard_reset` call now
    signs with the admin session instead of `KILNCTL_AP_PASSWORD`.
    **Bench-verified 2026-09-30:** an unauthenticated
    `POST /api/ota/esp/boot_guard_reset` returned 401; the same call under
    an admin session, issued right after a flash, succeeded.
  - **Static-IP reachability fix landed, through `50edd830`** (`03654117`
    fixes the shared `get_local_ipv4_string` helper's handling of an
    IPv4-mapped IPv6 address; `a829f71d`/`93a8716f` add the AP-subnet
    guard so the static-IP setter, the HTTP handler (a distinct 400) and
    the confirm-side check all refuse `192.168.4.0/24`; `dc1a8072`/
    `50edd830` render the provisioning page's errors via `textContent`
    with corrected no-response wording). Flashed to the bench; **bench-verified end to end, 2026-10-01 PASS** (`eb83c1ac`): static .156/24 gw .1 set,
    HTTP + reboot stayed at the static address, then DHCP revert, HTTP + reboot, same address.
    Remaining gap: the static-IP API has no DNS field and nothing reads the live DHCP
    netmask/gateway; see `docs/BENCH_TEST_LOG.md`.
  - **`persisted_count` fix landed on `origin/main`, 2026-09-30**
    (`9fc8b589`, `ee6a3809`, `9b77e2b2`, Opus-reviewed across three
    rounds): `GET /api/boot_guard` and `POST /api/ota/esp/boot_guard_reset`
    now report a separate `persisted_count`, the NVS-persisted counter,
    read by a strict reader that omits the field on any read or CRC
    failure rather than fabricating 0 -- `boot_count` stays this boot's
    fixed value. `flash_firmware()`'s result labels now distinguish
    "unknown (GET failed)" from "not reported (older firmware or read
    failure)". Flashed to the bench board at `9b77e2b2` (clean worktree,
    `flash_firmware()` verify OK: bootloader + partition table + app);
    afterward `boot_guard_get` showed `boot_count=1`, `persisted_count=0`,
    `recovery_mode=False`, and the reset line read "persisted before=0,
    after=0"; safety link up/armed/no trip, no unacknowledged crash.
    **Limitation:** the persisted count was already 0 before this reset,
    so the flash did not demonstrate a nonzero count dropping to 0 -- 0
    still merges "cleared" with "never written".
  - **Bench board flashed twice, 2026-09-30, both from clean worktrees via
    `flash_firmware()`:** first to `d5d6d64a` (`KilnCtrl-1ba4582e3e35`),
    then to `50edd830` (`KilnCtrl-6c9152cebb9d`); the Pico was untouched
    (`405d3c54`). Neither flash tripped or crashed the board. Readiness
    after: 17 ok / 1 not_done (`safety_commissioned`) / 3 other; task
    liveness OK.
