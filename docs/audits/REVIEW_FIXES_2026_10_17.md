# Review of fixes landed 2026-10-17

Read-only static review on origin/dev. No bench access. Commits: ae90a1539 (AZ-01..04),
08b1a7b4e (RO-01), 1c45b5d1b (RO-02 accepted), c9d4f2e3d (PM-03..06), 8c87343f5 (recovery
install order). AZ-04 evict-oldest is the owner's design and is not re-raised.

Counts: HIGH 0, MED 1, LOW 4, INFO 2. Rows: KNOWN_ISSUES FX-01..FX-07.

## Findings

- **FX-01 MED: AZ-03 destroy-all can still run on a non-httpd task.**
  `http_session_iface.c` applies the posted destroy-all inside `http_session_table()`.
  `http_auth_any_ap_session_active()` calls `http_session_table()` and runs on the
  `wifi_prov_owner` task (`wifi_prov_link.c:486`). REVIEW_FIXERS LOW-1 made that path read-only
  because httpd creates sessions in the same unlocked table. A gesture post that the Wi-Fi task
  sees first now runs `web_auth_table_destroy_all()` on the Wi-Fi task, at the same time as httpd
  login (`web_auth_login_http.c:583-591`). That is the torn-slot race AZ-03 was meant to remove.
  `s_destroy_all_applied_gen` also gets a non-atomic read-modify-write from two tasks. The likely
  result is a session left half-created. The AP-teardown read can also briefly see an empty
  table. A failure that keeps a session alive after a credential wipe needs worse luck, but it
  cannot be ruled out without a lock. No test covers this. Fix: add a read-only accessor (no
  apply, no lazy init) for the Wi-Fi path, or apply only from httpd handlers. Add a source-order
  or host test that `http_auth_any_ap_session_active()` never reaches the apply.
- **FX-02 LOW: one fail-open case remains in AZ-01.** `web_auth_store_admin_configured_or_unreadable()`
  returns false when the web blob key is HAL_NOT_FOUND, even if the AU-02 ever-enabled marker is
  set. If only the credential key is lost on a board that was set up before, unauthenticated
  `POST /api/auth/bootstrap_password` opens again. AU-02 already closes the same case for the
  policy key. Fix: treat NOT_FOUND plus `web_auth_store_ever_enabled()` as configured. First
  check that the physical reset writes an unconfigured blob and does not erase the key, so the
  gesture can still reach bootstrap.
- **FX-03 LOW: AZ-04 left stale comments and dead code.** The header comment of
  `login_lockout_slot_for()` (`web_auth_login_http.c` ~177-195) still says a saturated table
  returns NULL and that login answers 429 before credentials are checked. The `if (idx < 0)` block
  after the AZ-04 fallback says "deliberately not falling back to evict the globally-oldest slot".
  The code now does exactly that, and the block can only run if `LOGIN_LOCKOUT_MAX_IPS == 0`. A
  later reader could "restore" the old refusal. Fix: rewrite both comments and drop the dead branch.
- **FX-04 LOW: the RO-01 wait gives up and still cuts the apply.** `recovery_health_restart_for_http()`
  stops waiting after 10 minutes and calls `esp_restart()` while `recovery_apply_busy()` is still
  true. During the wait the LCD shows "HTTP FAILED / restarting", and it can replace the install's
  "DO NOT POWER-CYCLE" warning. Fix: when the wait times out, take the existing stay-up branch
  instead of restarting, and keep the install warning on screen while waiting.
- **FX-05 LOW: the PM-03 fallback count cannot be read.** `s_fallbacks` is a `static` inside a
  `static inline` function in `persist_scratch.h`. Each translation unit gets its own copy, the
  increment is not atomic, and no diagnostic reads it, so the commit's "counted" adds nothing past
  the first log in each TU. The host test only checks the source text. Fix: one atomic counter in
  a .c file, shown with the heap_internal diagnostics.
- **FX-06 INFO: PM-06 reads the running partition outside the reader bracket.**
  `ui_page_diagnostics_prime_ota_state()` calls `hal_sysinfo_get_running_partition()` and
  `esp_ota_get_running_partition()` before `flash_xip_reader_enter()`. Both are cached early in
  boot, so no mmap is expected. Add a comment, or move them inside the bracket, so they match the
  mmap serialization rule.
- **FX-07 INFO: tests for PM-04 and PM-06 only check the source text.** No test fails the first N
  `xTaskCreatePinnedToCoreWithCaps` calls. The RO-01 test checks only that the wait comes before
  `esp_restart`, not the timeout behaviour.

## Checked, no finding

- AZ-01/AZ-02 error mapping: `hal_kv_esp.c:142` maps ESP_ERR_NVS_NOT_FOUND, including a missing
  namespace on a fresh board, to HAL_NOT_FOUND. Fresh boards still bootstrap, and the marker
  still reads false.
- AZ-03 on httpd: every path that resolves, logs in, logs out or reports status goes through
  `http_session_table()`, so the destroy runs before the next use. The gesture test
  (`test_totp_enroll_gesture.c:537-562`) uses the real wiring. Other `destroy_all` callers
  (`security_backend_web_auth.c:280`, `auth_totp_http.c:555`) run on httpd only.
- 8c87343f5 order: `recovery_install_boot()` and its auto-recover helpers depend on no state
  that `recovery_http_start()` sets up. The Pico relay can only start through HTTP, so RO-01
  does not need to wait on `recovery_pico_busy()`.
- RO-02: owner accepted. The check pins the call inside the bracket.
- PM-04: the retry is bounded at 5 x 50 ms and runs from task context.
