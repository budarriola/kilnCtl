# Review: fixer batch 2026-10-15 (totpfx, lockfx, ctfx, tcfx, sxfx, bkfx, wififx)

Adversarial code review only. No fixes were made. Reviewed on origin/dev at
`8af425e10`.

| Fix | Landing commit | Code commit(s) |
|---|---|---|
| totpfx | 7625118a6 | 7625118a6 |
| lockfx | cb69a499c | f23a28e4f, 34bfc96f1 |
| ctfx | 67b6b7d9f | a63c94867 |
| tcfx | 0c7b0fb64 | 03e58ed8e, 766b4c96f |
| sxfx | 0a00f4273 | 43228c8d2 |
| bkfx | e2d69bfe6 | 8b6ca20d9 |
| wififx (application part) | bb828bee5 | 7675b92c6 |

**Counts: HIGH 0, MED 1, LOW 6, INFO 7.**

## MED

### MED-1 (totpfx): a racing enrollment can survive the LCD physical reset

- `firmware/KilnFW/App/drivers/net/auth_reset_gesture_wiring.c:51-57`
- `firmware/KilnFW/App/drivers/http/security_http.c:64-70` and `:363`
- `firmware/KilnFW/App/drivers/persist/totp_config.c:233-242`

`auth_reset_gesture_clear_all()` runs these steps in order:

1. `destroy_all`
2. `web_auth_store_clear_for_physical_reset`
3. `totp_config_clear()` (takes the totp state lock)
4. `destroy_all`
5. `totp_wipe_request_post()`

An `enroll_confirm` request that was authenticated before step 1 reaches `totp_config_enroll_commit()` and blocks on the state lock while step 3 runs its NVS erases. That takes tens of milliseconds. When step 3 releases the lock, the commit takes it. Its `still_valid()` callback, `security_totp_enroll_still_valid`, compares the wipe generation, but step 5 has not run yet. The callback therefore reports the pending secret as valid, and the commit writes counter and secret to NVS after the reset cleared them.

The pre-reset session holder then has an enrolled TOTP secret. That secret drives `/api/auth/forgot` and `/api/auth/reset`, so the physical reset no longer evicts everyone, which was the stated aim of AUDIT_TOTP MED-1. The httpd waiter wins this race whenever it runs at a higher priority than the LVGL task or on the other core, because the mutex give wakes it immediately.

The comment says the request is "posted last so a pending secret begun in between is also wiped". That ordering is what opens the hole.

Suggested fix: also post the wipe request before `totp_config_clear()`, and keep the post after it. Better, post it inside the same state-lock hold as the clear. Pin it with a host test in which `still_valid` runs after the clear but before the trailing post.

## LOW

### LOW-1 (lockfx): session reaping writes the unlocked table from a second task

- `firmware/KilnFW/App/drivers/http/http_session_iface.c:31` (static table, no lock) and `:307`
- `firmware/KilnFW/App/drivers/net/web_auth_session.c:119-131`
- `firmware/KilnFW/App/drivers/net/wifi_prov_link.c:480`

`http_auth_any_ap_session_active()` used to only read the table. It now calls `web_auth_table_reap_expired()`, which uses `memset` on slots. The caller is `ap_teardown_should_defer()`, which runs on the `wifi_prov_owner` task, while httpd creates and resolves sessions in the same table concurrently.

`web_auth_table_create_session()` (`web_auth_session.c:52-72`) does three things in order:

1. `memset`s the slot.
2. Sets `in_use=true`.
3. Writes `last_seen_ms=now` several statements later.

A reap that lands between steps 2 and 3 sees `last_seen_ms==0`. It judges the slot expired once uptime is past the timeout, and wipes it. The new login is silently lost and the user gets a 401 on the next request. The race cannot raise privilege, because a wiped slot has `in_use=false`. The comment at `wifi_prov_link.c:480`, "safe to call from any task", is no longer true.

Suggested fix: drop the reap from the AP query, which can filter expired slots read-only as it already does. Or set `last_seen_ms` before `in_use`.

### LOW-2 (totpfx): the reset route can re-install a password after the physical reset

- `firmware/KilnFW/App/drivers/http/auth_totp_http.c:517-528`

`reset_post_handler()` checks `totp_config_enrolled()` and that the admin password is configured, and sets `still_bound`. After that it calls `set_web_password()`, which runs the KDF and takes seconds, with no lock and no re-check. If the gesture runs inside that window, the old TOTP holder's new password is written after the store was cleared.

The class is pre-existing, but the totpfx fix claims the gesture evicts everyone. This is the same TOCTOU as MED-1, with a wider window but a narrower precondition: the attacker needs a valid reset token from a TOTP code.

### LOW-3 (ctfx): S9 can go warn-only on a split-map board with a fitted but uncalibrated channel

- `firmware/SaftyFW/src/config_store.h:1099-1112`
- `firmware/SaftyFW/src/safety_guards.c:412`, `:647`

`config_store_current_sensing_commissioned()` now requires every fitted channel to be calibrated. Take a `{0,2,2}` map with only `k_ct[2]` set: before this change it commissioned on the topology predicate and latched S9. Now it counts as uncommissioned, and S9 only warns.

That is the intended rule (HIGH-1). But the board loses a latching guard it had, and no ESP readiness item names the uncalibrated fitted channel (`readiness_http.h:160` does not check `k_ct`).

Suggested fix: surface "fitted CT channel N uncalibrated" in readiness.

### LOW-4 (ctfx): newly watched uncalibrated channels use the sensitive presence fallback

- `firmware/SaftyFW/src/current_presence_policy.c:4-30`

Channels that the split map now marks as fitted, but that have no `k_ct`, take the uncalibrated sensitive-margin presence path. On a real split-map board this can produce an S3 trip on noise that the old topology mask never watched. The fix is not tested against a live noisy channel.

### LOW-5 (tcfx): with debounce set to 1, one spurious live-check mismatch trips guard 6

- `firmware/KilnFW/App/drivers/hw/MAX31856.c:1456-1527`
- `firmware/KilnFW/App/drivers/control/thermal_guard.c:31`, `:181`

A live-check mismatch (for example SPI EMI corrupting one register read) clears `has_good_result` and re-arms a one-shot conversion of about 785 ms. The channel reads not-ok for at least one 1 s executor tick.

The default debounce of 3 ticks absorbs this, so it is safe. A commissioning value of `sensor_fault_debounce_ticks=1` turns a single bad read into a guard-6 trip during a firing. The live check has no "mismatch twice before acting" confirmation.

### LOW-6 (wififx): the missed-disconnect check is skipped while the AP teardown is deferred

- `firmware/KilnFW/App/drivers/net/wifi_prov_link.c:734-745`, `:748-768`

The new ground-truth `esp_wifi_sta_get_ap_info()` check sits after the `ap_pending_teardown` early return. Suppose the state is CONNECTED and a teardown is pending because someone is logged in over the AP. Each tick then returns early, so a dropped STA_DISCONNECTED event leaves the STA stranded until that session ends. The AP is still up in this state, so the board stays reachable.

Suggested fix: run the association check before the deferred-teardown branch.

## INFO

- **INFO-1 (sxfx)**, `firmware/KilnFW/App/drivers/hw/SX1509.c:531`, `:565`; `main_boot_early.c:378`. While ~RESET (GPIO10) is low, the SX1509 holds every I/O in its POR state, which is input/high-Z. Relay drivers therefore see the same state as at power-on, so the relays are de-energized (fail-safe). There is no glitch to the energized state. The pulse's return value is discarded with `(void)`. A failed gpio config simply falls through to the probe, which is acceptable.
- **INFO-2 (tcfx)**. Without DRDY, a POR of the MAX31856 is caught only by the 5 s live check, not by the 3 s age bound. With DRDY, a 0 °C POR result can read ok for up to about 3 s; this is pre-existing and now narrowed. The MASK compare cannot detect POR because `mask_shadow` defaults to 0xFF, which is also the POR value. `write_reg` keeps the shadows in step, so the compare is sound.
- **INFO-3 (tcfx)**. A not-ok channel gives zero duty and holds the ramp lock for that tick (`profile_executor.c:982`, `:1455`). A reconfigure therefore costs one tick of heat, not a trip, under the default debounce.
- **INFO-4 (lockfx)**. No permanent lockout found. The ladder caps at 300 s, and `login_backoff_cycle_reset_if_due()` drops expired windows on every lookup. The only residual 49.7-day revival is for a slot that is never looked up, which by definition locks no one.
- **INFO-5 (bkfx)**, `firmware/KilnFW/App/drivers/http/backup_import.c:1186-1209`. The post-import ceiling follows the same precedence as the commit at `:2602`: the backup's zone value, then the target slot under topology apply, then the live value. Duplicate zone indices resolve last-wins in both places, and the zone parse at `:1587` range-checks first. Stored profiles that are not in the backup are not re-judged against a lowered ceiling. That gap is pre-existing.
- **INFO-6 (ctfx)**. The ESP H9 fitted map (`ct_leak_alarm_service.c`) is used only when all three of 0x0320-0x0322 read as set; otherwise H9 falls back to all channels. That is conservative.
- **INFO-7 (test infrastructure)**. At base `8af425e10`, a full `build_host_tests.ps1` run fails to compile `test_main_reset_fence.c` (`relay_cycles.h(199): error C2065: 'KILN_IO_RELAY_COUNT': undeclared identifier`). This break is unrelated to these fixes. The negtests below used `-Only` to avoid it.

## Negative tests

All mutants compiled, and test assertions failed on each. The baseline passed and the real tree was unchanged.

| Mutation | Result | Failing assertion |
|---|---|---|
| lockfx: `login_backoff_is_locked` reverted to the absolute form `now_ms < lock_start_ms + lock_dur_ms` | CAUGHT | `test_login_backoff.c:100`, `:105`, `:106`; `test_lcd_auth_state.c:168` |
| tcfx: `thermo_reading_ok` age gate dropped | CAUGHT | `test_max31856_hal_spi.c:122`, `:124`, `:207` |
| ctfx: S9 commissioned predicate reverted to topology-only (`config_store.h:1104`) | CAUGHT | `SaftyFW/test/test_ct_split_map_guards.c:161`, `:164`, `:173` |

How they were run:

- KilnFW: `tools/negtest.ps1 -Command "build_host_tests.ps1 -OutDir {OUT} -Only test_login_backoff|test_max31856_hal_spi"` with `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.
- SaftyFW: `-Preset saftyfw-host` with `-ExpectPattern "FAIL .*\.c:\d+|SAFTYFW HOST TESTS: FAILED"`.

No mutation was run for the MED-1 race, because no host test exercises the ordering between the gesture and a commit. That gap is part of the finding.
