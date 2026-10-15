# AUDIT_LOGSTORE_TIME: KilnFW log storage and time handling

Date: 2026-10-14. Base: origin/dev `d5bbf13fc`. Code-only audit; no board access.

Scope:
- **Logs:** `log_store` (firing, autotune and event segment files on the `logs` SPIFFS partition), flash wear, ring wrap, torn-record recovery after power loss, and HTTP export (`log_http.c`).
- **Time:** SNTP and `time(NULL)` before sync, uptime vs wall clock, and 32-bit tick/ms wrap in control, profile executor, guards, sessions, login backoff and update scheduling.

Key question: does any timer comparison break after long uptime or a clock jump?
Yes, in the auth/backoff paths (MED-1, MED-2) and, to a lesser degree, in sessions and the Pico OTA relay (LOW-1..3). The control, guard and executor timing paths were checked and found wrap-safe. A wall-clock jump (SNTP sync) does not reach any of them, because they all run on monotonic uptime.

Time bases: `CONFIG_FREERTOS_HZ=100`, so a 32-bit `TickType_t` wraps after about 497 days. Any `uint32_t` ms value wraps after 2^32 ms, about 49.7 days. That covers `(uint32_t)hal_time_now_ms()`, `ticks*portTICK_PERIOD_MS` and `lv_tick_get()`. The signed-diff idiom `(int32_t)(a - b)` gives the wrong sign once the true distance exceeds 2^31 ms, about 24.86 days. `hal_time_now_us()/ms()` are 64-bit and safe until a caller truncates them.

HIGH 0, MED 2, LOW 5, INFO 6

## Findings

### MED-1: Phantom web-login lockout about 24.86 days after an expired lock

- **Evidence:**
  - `firmware/KilnFW/App/drivers/net/login_backoff.c:5-11`: `login_backoff_is_locked()` returns `(int32_t)(locked_until_ms - now_ms) > 0` whenever `locked_until_ms != 0`.
  - `login_backoff.c:26`: `record_failure` sets `locked_until_ms = now + ladder`.
  - `locked_until_ms` returns to 0 only on success (`:32`) or in `cycle_reset_if_due()` once `failure_count >= 5` and the slot is unlocked (`:17`). An expired lock below 5 failures is never cleared.
  - Caller: `firmware/KilnFW/App/drivers/http/web_auth_login_http.c`. Slot lookup and LRU are at `:198-232`; the 429 path and `retry_after_ms = locked_until_ms - now` are at `:387`. `now` is `(uint32_t)hal_time_now_ms()`.
- **Failure scenario:**
  1. A user mistypes the password once. Lock is 5 s, so `locked_until = T+5000`.
  2. Nobody logs in from that slot again, or the remote clients all share the remote slot (`claim_remote_slot`).
  3. Once 2^31 ms (24.86 days) have passed after `T+5000`, `(int32_t)(locked_until - now)` turns positive again. The slot reads as locked for the next 24.86 days.
- **Effects:**
  - Every login from that client, or every remote client on the shared remote slot, gets HTTP 429. `Retry-After` is computed from the wrapped difference and comes out as weeks.
  - The correct password is refused, because the lock is checked before the password.
  - A locked slot is never LRU-evicted (`:213`), so the slot cannot be reclaimed.
  - At `failure_count == 5`, `cycle_reset_if_due()` is blocked as well, because it requires `!is_locked`.
  - Only a reboot clears it. This is reachable on a kiln controller left powered for a month, which is the normal deployment.
- **Fix:** store `lock_start_ms` and `lock_dur_ms`, and test `(uint32_t)(now - start) < dur`. That is wrap-safe for any uptime, because dur ≤ 300 s. Alternatively, zero `locked_until_ms` the first time it is seen expired: in `is_locked`, if the diff is ≤ 0, clear it. That still fails if nobody looks at the slot within 24.86 days, so the start+duration form is preferred.
- **Host test:** `test/test_login_backoff.c` exists, but has no case at `now = locked_until + 2^31`. A test should record one failure, advance `now` by 2^31 + 1 ms, and expect not-locked.

### MED-2: Same phantom lockout on TOTP password reset and on the LCD PIN keypad

- **TOTP reset evidence:** `firmware/KilnFW/App/drivers/http/auth_totp_http.c:157-187` is a private copy of the same backoff (`totp_backoff_is_locked` at `:160-166`, `record_failure` at `:181`). Its slot LRU is at `:215`, the shared remote slot is the same as in MED-1, and `retry_after_ms` is at `:269`. `now` is `(uint32_t)hal_time_now_ms()` (`:98`).
- **TOTP reset scenario:** after one wrong TOTP code and 24.86+ days of uptime, `POST /api/auth/forgot` and `POST /api/auth/reset` return 429 for about 24.86 days. Password recovery is the escape hatch for a forgotten admin password, and it is blocked exactly when it is needed.
- **LCD keypad evidence:** `firmware/KilnFW/App/drivers/ui/lcd_auth_state.c:72-110` (`lcd_keypad_state_submit`) uses `login_backoff` with `now_ms = (uint32_t)lv_tick_get()` (`ui_lcd_keypad.c:167`). The state `s_ks` (`ui_lcd_keypad.c:93`) is initialized only in `build_overlay()` (`:238`). That function runs once (`if (!s_mbox) build_overlay();`, about `:296`), so the backoff state persists across keypad opens for the whole boot.
- **LCD keypad scenario:** one wrong PIN, then 24.86 days of uptime. The keypad then shows LOCKED_OUT with a huge remaining time (`:176-185`) and refuses the correct PIN. LCD trip clear requires admin login (owner decision 2026-10-07), so local trip clearing is blocked until reboot.
- **Fix:** same as MED-1. The TOTP copy should call the shared `login_backoff` after it is fixed, instead of keeping a duplicate.
- **Host test:** `test/test_lcd_auth_state.c` has no wrap case. No host test covers the TOTP backoff copy's lock timing at a wrap offset.

### LOW-1: An expired web session revives every 49.7 days

- **Evidence:** `firmware/KilnFW/App/drivers/net/web_auth_session.c:170-180` computes `elapsed_ms = now_ms - last_seen_ms` as uint32 and returns `elapsed_ms <= timeout_ms`. The comment at `:176` claims wrap is fine. Expired slots are never freed: `in_use` is cleared only by logout or LRU eviction. Callers are in `http_session_iface.c` (`:172`, `:207`, `:245`, `:282`, `:302`) and use `(uint32_t)hal_time_now_ms()`.
- **Scenario:** a session expires and its token is kept, for example in a browser cookie or a captured request. If no new logins evict the slot, then 2^32 ms (49.7 days) after its last use, `elapsed` falls back into `[0, timeout]` and the token is accepted again for up to `timeout` (1-60 min). The idle-timeout policy is "never revive an expired session" (`http_session_iface.c:253`), and this violates it. It needs a stale token, a still-allocated slot and a hit inside a narrow window. The session's source-IP binding, if enforced, also has to match.
- **Fix:** when `is_valid` returns false, the caller should free the slot (`in_use = false`). Alternatively, keep `last_seen` as a 64-bit ms value.
- **Host test:** `test/test_web_auth.c:162` covers the expiry boundaries but not `last_seen + 2^32`.

### LOW-2: LRU eviction compares raw uint32 timestamps

- **Evidence:** `web_auth_session.c:25-28`, `web_auth_login_http.c:213` and `auth_totp_http.c:215` choose a victim with `ts < oldest`.
- **Scenario:** after the 49.7-day wrap, a slot touched just after the wrap has a small value and is evicted, while a slot untouched for weeks with a large value survives. For sessions, an active user is logged out instead of a stale one. For the login and TOTP tables, a fresh failure-history slot can be evicted, which slightly weakens the backoff.
- **Fix:** pick the largest `(uint32_t)(now - ts)`.
- **Host test:** none.

### LOW-3: Pico OTA relay deadline is not wrap-safe

- **Evidence:** `firmware/KilnFW/App/drivers/net/ota_pico_relay.c:319` sets `deadline = since_ms + timeout_ms`, and `:338` tests `if (now_ms() >= deadline)`. `now_ms()` is `(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS)`.
- **Scenarios:** both need a Pico OTA run within `timeout_ms` of the 49.7-day uptime wrap.
  - If `since + timeout` wraps past 2^32, `deadline` is small and the very first check times out at once. The relay wait fails spuriously.
  - If `deadline` lands just below 2^32 and the poll interval steps `now` past both `deadline` and the wrap between two checks, then `now < deadline` again. The wait runs for up to about 49.7 days instead of `timeout_ms`.
- **Fix:** test `(uint32_t)(now_ms() - since_ms) >= timeout_ms`.
- **Host test:** none.

### LOW-4: Torn log manifest after power loss orphans old segments

- **Evidence:**
  - `firmware/KilnFW/App/drivers/persist/log_store.c:91`: `save_manifest()` rewrites the manifest with `fopen(path, "w")` after every append.
  - The partition is SPIFFS (`CONFIG_KILNCTL_LOGS_LITTLEFS` default n; `partitions.csv` `logs, data, spiffs`), and "w" truncates first. SPIFFS gives no atomic replace.
  - `load_manifest()` (`:72` onward) sets `has_data = false` (`:66`) when `fscanf` fails.
  - `rotate_if_needed()` then restarts at segment index 0.
- **Scenario:** power is lost between the truncate and the `fprintf`/`fclose`, which is plausible because kiln power is often cut at the breaker. On the next boot the manifest is empty, so the store restarts at idx 0. Old segment files, up to 8 × 32 KiB per kind, stay on flash untracked until the new sequence reaches their indices.
- **Effects:**
  - Readers that walk oldest..newest skip them, so history is lost from the export.
  - The new idx 0 segment is appended to an existing old file, so records come out of order.
  - Free space is reduced by up to 256 KiB per kind until the old files are overwritten or trimmed. With three kinds on the 768 KiB partition this can make appends fail. Appends degrade without wedging (`:188`), but logging stops.
- **Fix:** write `manifest.tmp`, then `rename()` it over the manifest. Or, on a missing or bad manifest, rebuild oldest and newest by probing which `<kind>_%010zu.log` files exist.
- **Host test:** none for a truncated or empty manifest.

### LOW-5: A torn record mid-segment corrupts the rest of that segment

- **Evidence:** `log_store_append()` (`log_store.c:197` onward) writes a uint16 length prefix and then the payload with "ab". The reader (around `:282`) trusts each prefix in turn. A short read moves to the next segment, and an oversized prefix is skipped with `fseek`. There is no resync on a record magic; the 0xE7 magic is checked only by the PC-side decoder.
- **Scenario:** power is lost after the prefix, or partway through the payload. The next boot keeps appending to the same segment (`newest` is unchanged), so every later record in that segment is read at a misaligned offset. The rest of the segment decodes as garbage or is skipped, which can be up to 32 KiB of firing history. HTTP export (`/api/logs/firing`) streams the misaligned bytes.
- **Fix:** at mount, start a new segment (bump `newest`) so a torn tail never has records appended after it. Alternatively, validate the record magic and length and scan forward to resync.
- **Host test:** none for a torn tail.

### INFO

1. **Event records have no wall-clock time.** Event records carry `uptime_s` only (`persist/event_log_emit.c:24`). Events from different boots cannot be ordered or dated except by segment order. If the timeline matters, record the boot id, or `time(NULL)` once SNTP has synced.
2. **Manifest rewrite doubles the write volume.** Each event append also rewrites the manifest, so there are two flash writes per record. Events are rare and SPIFFS wear-levels, so this is not a wear problem at realistic rates. It does widen the LOW-4 window.
3. **HTTP export is not synchronized with append and trim.** The HTTP export (`http/log_http.c:30-62`) reads segments on the httpd task without synchronizing with the flash-worker append and trim. A segment removed or extended mid-read degrades to a short read and a truncated response. No crash path was found.
4. **Polling can miss a short transition.** `persist/telemetry_log.c` derives events by polling state each tick, so a state that enters and leaves within one tick produces no event.
5. **`time(NULL)` before SNTP is stored as if it were wall clock.**
   - `adaptive_tune_model.c:366` stores `last_applied_unix_s = time(NULL)`. It is display-only (`adaptive_tune_http.c:99`), and before sync it shows a 1970-era value.
   - `zones_current_sweep_task.c:2275` stores `blob.taken_unix = time(NULL)`. Its comment says this is "0 when the clock was never set", but it is actually seconds since boot.
   - Neither feeds any timing decision.
   - `profile_executor_run.c:781-786` and `safety_link.c:985` already gate on an epoch at or after 2020. TOTP and the update fetch refuse until SNTP has synced.
6. **Checked and wrap-safe** (signed or unsigned differences, 64-bit us, or an at most 497-day tick wrap):
   - `safety/danger_mode.c` (257, 273, 379)
   - `control/heat_enable.c:894`
   - `control/ct_leak_alarm_service.c` (80, 113)
   - `ui/screen_idle.c` (282, 445)
   - `safety/safety_link_frames.c` (1092-1098, 1197)
   - `update/update_fetch.c` (435-464)
   - `update/update_url.c` (455-477)
   - `hw/MAX31856.c` (273-300)
   - the `control/profile_executor.c` guard-9 timing (2154-2193)
   - `control/autotune_engine.c`
   - `ui/ui_page_profile_picker.c:167`
   - the LCD inactivity lock (`lcd_lock_tick`)

## Test gaps

- `test_login_backoff.c`: no lock check at `locked_until + 2^31`. This would catch MED-1 and, via the shared helper, the LCD half of MED-2.
- `test_lcd_auth_state.c`: no wrap case.
- The TOTP backoff copy in `auth_totp_http.c`: no lock-timing test at all. Folding it onto the shared `login_backoff` would remove the gap.
- `test_web_auth.c`: no `last_seen + 2^32` revival case (LOW-1) and no LRU-after-wrap case (LOW-2).
- `ota_pico_relay.c` `relay_wait_for_states`: no deadline test near the wrap (LOW-3).
- `log_store`: no host tests for a truncated or empty manifest (LOW-4) or a torn final record followed by another append (LOW-5).
