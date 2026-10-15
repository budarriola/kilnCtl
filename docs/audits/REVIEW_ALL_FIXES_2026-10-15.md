# Review: all 2026-10-15 fix work on origin/dev

Reviewer: single opus review pass. Scope: the commits in origin/main..origin/dev, reviewed at
f75cae102 against the Resolution sections in docs/audits. This was a review only. No code was
changed.

Totals: HIGH 0, MED 1, LOW 5, INFO 7.

## Negative-test evidence

Nine mutations were run with tools/negtest.ps1. Every one was CAUGHT, the unmutated baseline
passed both times, and the real tree was unchanged. Every mutant compiled, and each CAUGHT came
from a test assertion failing, not from a build failure.

| Mutation | Preset | Failing assertion |
|---|---|---|
| wipe_epoch_check_removed | kilnfw-host | test_web_auth_store.c:992 |
| boot_guard_invalid_size_is_error | kilnfw-host | test_boot_guard.c:1057 |
| drop_stale_ignores_claim | kilnfw-host | test_update_install_record.c:424 |
| max31856_no_suspect_stage | kilnfw-host | test_max31856_hal_spi.c:192 |
| sx1509_no_reset_pulse | kilnfw-host | test_sx1509_reset_first.c:71 |
| gesture_wipe_not_posted_first | kilnfw-host | test_totp_enroll_gesture.c:632 |
| A8_s2_invalid_passes | saftyfw-host | test_safety_guards.c:3415, 3416 |
| A9_no_tripped_refusal | saftyfw-host | test_safety_core_host.c:1140 |
| A10_no_republish | saftyfw-host | test_safety_core_host.c:565-567 |

## MED

### M1. An unreadable boot_guard record makes crash-loop protection blind

- **Where:** firmware/KilnFW/App/drivers/boot/boot_guard.c, around lines 526-560, in the
  `load_count()` bool path (c58ca2ab2, c27c415a1).
- **What happens:** when the stored record cannot be read (a real read error, not
  HAL_INVALID_SIZE), the boot is counted as 1 in RAM only and nothing is persisted.
- **Failure scenario:** a persistent NVS read error on the record, combined with firmware that
  crashes every boot, reads as boot 1 every time. The recovery threshold is never reached, so the
  board reset-loops forever and only JTAG can get it out.
- **Suggested fix:** on a read error, erase the key and persist count=1 so the following boots
  increment. Alternatively, keep a fallback counter in RTC memory or a secondary NVS key, and
  enter recovery when the fallback reaches the threshold.

## LOW

### L1. web_auth_store set_password: the wipe epoch is checked without a lock

- **Where:** web_auth_store.c:498-570; the epoch check is at line 560.
- **Failure scenario:** no lock covers the window between the epoch check and
  `set_blob_verified`. If the LVGL task runs a wipe during that NVS write, the password write
  lands after the wipe and brings back a credential the wipe was meant to remove.
- **Suggested fix:** hold one store mutex across the epoch check and the write, and take the same
  mutex in the wipe.

### L2. web_auth_store set_pin has no epoch check

- **Where:** web_auth_store.c:957-973.
- **Failure scenario:** a web clear_credentials on httpd races a set_pin on LVGL, and the PIN
  survives the clear.
- **Suggested fix:** give set_pin the same epoch capture and re-check (under the L1 lock).

### L3. Session reaping only runs on httpd request paths

- **Where:** http_session_iface.c.
- **Failure scenario:** the board sees no HTTP traffic for 49.7 days, the 32-bit millisecond
  tick wraps, and a token that has expired compares as fresh again.
- **Suggested fix:** compare expiry with 64-bit `esp_timer` time, or reap from a periodic timer
  as well.

### L4. MAX31856 suspect stage is cleared on one error path but not the other

- **Where:** MAX31856.c:1477-1507; the lock-failure path is at line 1463.
- **Failure scenario:** an SPI_ERROR clears `live_suspect`, so a mismatch that alternates with SPI
  errors never confirms a reset. The lock-failure path does not clear it, which is inconsistent
  with the SPI_ERROR path.
- **Suggested fix:** pick one rule for both paths. Keeping the suspect state across transient
  errors is preferred.

### L5. A self-delete aborts when internal RAM is exhausted

- **Where:** every `vTaskDeleteWithCaps(NULL)` site changed in bd8b44411.
- **Failure scenario:** in IDF 6.0.2, a self-delete spawns a cleanup task with
  `configMINIMAL_STACK_SIZE` from internal RAM, and calls `abort()` if that create fails. Under
  internal-RAM exhaustion, a task exiting normally therefore panics the board.
- **Suggested fix:** delete these tasks from their owner rather than self-deleting, or document
  this behavior against the 8 KB internal RAM floor.
- **Checked:** every deleted task was created WithCaps. ota_http_esp.c:755 correctly keeps plain
  `vTaskDelete`.

## INFO

- **I1. MAX31856 all-zero burst** (max31856.c:375, A6): an all-zero CJTH..SR burst is treated as
  a failed read. A genuine 0 C reading would also hit this, but that case is vanishingly
  unlikely.
- **I2. SX1509 bus clear "STOP"** (SX1509_i2c_bus_clear): the STOP is issued with SCL already
  high, so it produces START then STOP. This is harmless. The order, bus clear before
  `i2c_new_master_bus` (main_boot_early.c:373), is correct, and the reset pulse runs only at
  boot.
- **I3. Recovery verify cache** (recovery_http.c:118-180): the portMUX and generation counter are
  correct. A verify that overlaps an invalidate returns its stale result without caching it,
  which is acceptable.
- **I4. Stale install drop** (update_install_http.c:150-170): this is sound. The install path holds
  the `ota_http_update` claim until reboot, so the worst race is a 409 busy.
- **I5. log_store two-copy manifest:** seq and slot are updated only after a successful write,
  which is correct. However, the 600-byte path buffers on the stack add up to about 1.8 KB in
  save, so the callers' stack headroom should be re-measured.
- **I6. login_backoff start/duration:** the tick arithmetic wraps, but the unsigned difference is
  correct.
- **I7. SaftyFW A8-A11, A15** (1f88770be): the A8 clear refusal is sound, because the tick
  returns early while tripped so `s11_window_active` stays set. The A9 tripped-enable refusal,
  A10 republish, A11 bad-read S8 accounting and A15 calibration copy are correct and covered by
  tests. The boot_guard soak gate (30 s after boot complete), the HAL_INVALID_SIZE rewrite, the
  autotune measurement abort and `pid_reseed_derivative` were also checked and look correct.
