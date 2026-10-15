# Review: OTA/boot fixes on dev (confirmfx, bootfx, recfx, wififx recovery part)

Date: 2026-10-15. Adversarial review only, no fixes. Base: origin/dev at 8af425e10.

Scope:
- confirmfx (65927acc7, MED-3 resample in aa3955376): `boot_confirm_step()`, mark-valid retry with otadata read-back.
- bootfx (c58ca2ab2, up to 8af425e10): boot-complete gate, 30 s soak, `boot_guard_confirm_withhold()`, `load_count()` read-error handling.
- recfx (d99a5230b, 6d442720d, 3cfd0a564): install-after-fail STAY / EXIT_APP / RESTART, `post_boot` ordering, result detail byte, LCD passphrase kept on failure.
- wififx (bb828bee5, recovery part in 7675b92c6): `recovery_wifi.c` failure path when the first `start_softap` fails.

Questions asked: can any path brick the board into a permanent recovery loop or reset loop, or strand it in recovery? How does the soak interact with `flash_firmware`'s boot_guard_reset and with recovery_exit? What does power loss do at each install step? Are there reset-one-side pairs? Can the passphrase leak?

## Verdict

HIGH 0, MED 0, LOW 4, INFO 9.

I found no path to a permanent recovery loop, a reset loop, or a board that can never leave recovery. Every automatic loop is bounded by one of three things: the install attempt counter (`UPD_INST_MAX_ATTEMPTS`), the boot_guard threshold, or the recovery Wi-Fi restart budget. The passphrase is never logged, persisted or served.

## Negative tests

Each mutant was run with `tools\negtest.ps1 -Command 'firmware\KilnFW\App\test\build_host_tests.ps1 -Only <test> -OutDir {OUT}'` and the kilnfw-host ExpectPattern. Every baseline passed and the real tree was unchanged. For every CAUGHT, the log shows the mutant compiled and an assertion failed. None of them is a build failure.

| Mutation | Test | Verdict | Failing assertion |
|---|---|---|---|
| soak gate `since < 0 \|\| since < BOOT_CONFIRM_SOAK_MS` to `since < 0` (boot_guard.c:681) | test_boot_guard | CAUGHT | test_boot_guard.c:938, :943 (SOAK GATE) |
| persist skip on read error `part_err == HAL_OK && !load_failed` to `part_err == HAL_OK` (boot_guard.c:563) | test_boot_guard | CAUGHT | test_boot_guard.c:1025 (READ ERROR must not overwrite) |
| withhold ignored `s_boot_complete && !s_confirm_withheld` to `s_boot_complete` (boot_guard.c:637) | test_boot_guard | CAUGHT | test_boot_guard.c:985, :993 (MONITOR FAILURE) |
| after_fail ignores app_modified `app_verifies && (!app_modified \|\| !attempts_left)` to `app_verifies` (update_install_record.c:204) | test_update_install_record | CAUGHT | test_update_install_record.c:613 (retry the switch) |

## LOW

### LOW-1: an oversized stored boot_guard record now reads as a read error, and the record is never rewritten
`firmware/KilnFW/App/drivers/persist/boot_guard.c:286`, `:532`, `:563`.

Suppose the stored blob is larger than the 12-byte `boot_guard_record_t`, for example after a downgrade from a build with a larger layout. Then `hal_kv_get_blob` returns `ESP_ERR_NVS_INVALID_LENGTH`. `hal_kv_esp.c:169` maps that to `HAL_INVALID_SIZE`. `load_count()` then returns false at :286 ("genuine read error"), so `boot_guard_init()` sets `load_failed`, skips the persist at :563, and runs the boot with an in-RAM count of 1.

The result is that every boot counts as 1 and the counter never climbs. A crash-looping board in that state never reaches `RECOVERY_MODE_BOOT_THRESHOLD`, which is the exact masking that LOW-3 set out to remove. Before this change, the same state became count 0 plus a persist, and the persist healed it.

The header comment at :272 says a "wrong size (a build with a different layout)" is a data state with a true return, so the code contradicts its own documentation. The legacy-namespace read at :310 has the same behavior. The LOW-3 test (test_boot_guard.c:1004) only scripts `HAL_NOT_READY` on open. No test covers an oversized blob.

This is LOW, not MED, because the record layout at :72-77 has not changed since it was introduced, so no shipped build writes a larger record today. A successful healthy confirm also rewrites the record at the correct size. Treating `HAL_INVALID_SIZE` as "wrong layout, count 0" would make the code match its comment.

### LOW-2: the soak widens the window in which a reset rolls an OTA image back into recovery
`firmware/KilnFW/App/drivers/persist/boot_guard.c:681`; `firmware/KilnFW/App/main_bridges_bringup.c:249`.

With `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, the image stays PENDING_VERIFY until the boot-complete signal fires and then for another `BOOT_CONFIRM_SOAK_MS` (30 s). Any reset in that window marks the image aborted, and the bootloader then boots `recovery`. That includes an operator power cycle, a dual reset, or a `debug_reset`, right after an OTA or an unattended install.

This can be recovered from: the stage keeps its verified image, and `recovery_exit` or a new install works. It still needs operator action. Repeated quick resets also each count toward the boot_guard threshold of 3, as before.

`flash_firmware()`'s JTAG path is not affected, because a JTAG write leaves otadata alone (see INFO-2).

### LOW-3: the soak delta overflows int32 on a board that stays unhealthy for weeks
`firmware/KilnFW/App/main_network_http.c:229`.

`(int32_t)((xTaskGetTickCount() - sc->complete_tick) * portTICK_PERIOD_MS)` goes negative after about 24.8 days at 1 ms ticks. Once it is negative, :681's `since < 0` keeps the step PENDING, until the 32-bit tick delta wraps again.

This only matters if the decision stayed unhealthy for that long and then turned healthy. The effect is a delayed confirm, not a brick. Clamping the delta, or treating "elapsed past the soak once" as a latch, would remove it.

### LOW-4: power loss between the boot switch and `post_boot` leaves a stale install request that the app never clears
`firmware/KilnFW_recovery/main/recovery_apply_esp.c:137`; `firmware/KilnFW/App/drivers/update/update_install_record.c:222-240`; `firmware/KilnFW/App/drivers/update/update_install_http.c:138`, `:145`.

`post_boot` clears the request only after `recovery_boot_partition_set_and_verify()` succeeds. If power is lost in between, the new app boots, confirms, and clears the now-matching stage via `update_http_stale_stage_check()`. The request record (attempts 1, no OK result) survives, because the app only erases it on its own install-POST failure paths.

The next time the board enters recovery, for any reason including a deliberate `recovery_enter`, `recovery_install_boot()` (`recovery_install.c:316`) sees the request. It finds no matching stage, refuses, and `auto_recover` decides EXIT_APP (`recovery_install.c:260-264`). That clears the request and boots the app. The operator is pulled out of a recovery session they entered on purpose, once, and the next entry stays.

On a crash-loop entry, the same exit sends the board back into the failing app once more before boot_guard returns it to recovery. Both cases are bounded.

If an `update_post_boot` failure is ever widened, an app-side "clear a request whose image is the confirmed running one" would close this gap.

## INFO

- INFO-1: The soak only catches failures that show up within about 10 s of boot-complete. `monitor_task` is the only feeder of the 20 s RTC watchdog, so a dead monitor resets the board inside the soak. A `monitor_task_start` failure is fully covered by the withhold instead (`main_network_http.c:975`, `boot_guard.c:637`).
- INFO-2: The flash_firmware `boot_guard_reset` route (`ota_http_recovery.c:434-460`) calls `boot_guard_reset_counter()` (`boot_guard.c:826`), which bypasses the soak and the withhold by design. It sets `healthy_marked`, so a later `boot_guard_mark_healthy()` returns at once (`boot_guard.c:765`). Mark-valid still waits for the soak in `boot_confirm_step()`, and a JTAG flash leaves otadata alone, so the two do not conflict.
- INFO-3: `boot_confirm_step()` clears the counter even when mark-valid has not finished (backoff up to `BOOT_CONFIRM_MARK_MAX_FAILS`). This is intended: the counter tracks boot health, and the rollback state is retried separately. A persistently failing mark-valid leaves the image PENDING_VERIFY, so the next reset rolls back to recovery, which is visible and bounded.
- INFO-4: A permanently unreadable NVS behaves as before (the counter cannot climb). That is the trade-off LOW-3 accepted.
- INFO-5: Recovery-image `recovery_exit` erases both boot_guard keys and verifies they are absent (`recovery_http.c:701`). The app reads "absent" as count 0, which is consistent. The in-app recovery_exit uses `boot_guard_mark_healthy()` (`ota_http_recovery.c:147`), which is not soak-gated.
- INFO-6: A new image that crashes after an unattended install is rolled back by the bootloader. Recovery then boots, `post_boot` has already cleared the request, and recovery stays. This is bounded with no loop.
- INFO-7: `update_install_after_fail()` (`update_install_record.c:191-211`) sends every uncertain outcome to STAY. RESTART needs attempts left, `boot_run` raises the attempt count and reads it back before applying, and ATTEMPTS_EXCEEDED is treated as modified (`recovery_install.c:345`), which makes it EXIT_APP only if the app verifies. All three are bounded.
- INFO-8: wififx: when the first `start_softap` fails, the image restarts only while `rhealth_ap_reraise_failed_action()` allows it (`recovery_wifi.c:419-420`). Past the RTC budget, it stops Wi-Fi and stays up with no network (:425). The restart waits for an apply in progress (`restart_image_for_wifi`, :63). This runs before `recovery_install_boot()`, so it never uses up install attempts. A power-on reset clears the budget.
- INFO-9: Passphrase: generated per boot, held in RAM only (driver storage is RAM), wiped on every shutdown path, and never in a log line, NVS, an HTTP response, or an install result record. `show_failure()` (`recovery_install.c:219`) uses a warning overlay that keeps it on the LCD, as intended. There is also a small time-of-check-to-time-of-use gap in `recover_task()` between the busy recheck and `recovery_http_select_app_and_restart()`, which is harmless because nothing in that gap starts an apply.

## Reset-one-side pairs checked

- `s_boot_complete` / `s_confirm_withheld` against the confirm task: both are reset in `boot_guard_init()`, and the task is created after init. No pair left half-reset.
- RTC stuck-counter marker against the NVS count: `boot_guard_reset_counter()` and the healthy path both arm the marker after a verified clear, so the two stay together.
- Install request against the stage: one pair is split by design. The stage is cleared by the app's stale check, and the request only by recovery or install-POST failure. That gap is LOW-4.
- Recovery Wi-Fi restart counter (RTC) against the install attempt counter (NVS): independent, and neither derives from the other.
