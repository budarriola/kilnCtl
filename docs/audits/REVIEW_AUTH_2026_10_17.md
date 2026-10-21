# Review 2026-10-17: KilnFW web and LCD auth

Read-only static review of `origin/main` 81acb5dd7. No code changed, no board access.
Scope: login/KDF and lockout, session tokens, route tiers against the owner decisions
(dashboards-only unauthenticated, admin-only routes, 409 mode gate), LCD PIN gating, the
AU-02 auth-ever-enabled marker, backup/import auth, OTA upload auth. Items already in
`docs/KNOWN_ISSUES.md` (AU-01/02, HR-01..05, LU-01..04, BW-*, UP-*) are not repeated.

Counts: HIGH 1, MED 1, LOW 2, INFO 2.

Checked and found sound: every route goes through `kiln_http_register()`; route tier table
matches the owner decisions (OPEN = dashboard, status, profile_exec, readiness, history,
login/forgot/reset; profile_exec start/stop/pause/resume USER; everything else ADMIN,
including `/api/backup/import|export`, `/api/ota/*`, `/api/factory_reset`, the
`boot_guard_reset`/`recovery_*` routes); the relay mode gate sits in `kiln_io_owner.c:238`,
so it also covers LCD relay writes; backup does not carry credentials or auth policy;
OTA ESP and Pico uploads are ADMIN tier and run interlocks after auth; the login KDF runs
for unknown users too (`web_auth_store.c:486-494`); session tokens are 32 random bytes,
stored as SHA-256, bound to the client IP, cookie `HttpOnly; SameSite=Strict`; password
change, clear_credentials and the reset gesture all destroy sessions; the LCD policy fails
closed on UNREADABLE (`lcd_credential_bridge.c:42`), and every non-dashboard LCD action
goes through `ui_lcd_lock_run_gated` at the owner-decided role.

## AZ-01 HIGH: unreadable admin record opens the bootstrap route to anyone

- Where: `firmware/KilnFW/App/drivers/persist/web_auth_store.c:497-501`
  (`web_auth_store_password_configured()` returns false for UNREADABLE and for any read
  error, since `load_password` maps every `load_blob` error to ABSENT at line 450-451),
  used by `http_auth_policy_iface.c:34` and `security_backend_web_auth.c:461`;
  `http_auth_enforce.c:51-53` then opens `ROUTE_TIER_ADMIN_BOOTSTRAP` with no session.
- Scenario: the `web_auth` blob fails its CRC, or its version differs after an OTA
  rollback past a schema bump (the case the code itself names at line 453-456), or an NVS
  read fails. The policy reads OK-enabled or UNREADABLE (both count as enabled), so
  `bootstrap_needed` is true. Any LAN or AP client, without a session, POSTs
  `/api/auth/bootstrap_password` and sets a new administrator password.
  `web_auth_store_set_password()` (line 597-605) starts from a clean blob when the old one
  is unusable, so the write succeeds. The attacker now has admin: OTA upload, factory
  reset, safety commissioning. The LCD side of the same state locks hard (as the bridge
  comment intends); the web side gives the board away.
- Fix: bootstrap only on a real "never written" record. Make `load_blob` return
  `HAL_NOT_FOUND` distinctly, map only that to ABSENT in `load_password`, and add a
  `web_auth_store_admin_record_state()` that the bootstrap predicate uses, with UNREADABLE
  or any other error counting as configured. Recovery from an unreadable record stays the
  physical reset gesture. Host test: corrupt CRC and wrong version both keep the bootstrap
  route at 409/403.

## AZ-02 MED: a transient NVS read error turns auth off for that request

- Where: `web_auth_store.c:386-387` and `:779-786` (any `load_blob` error on the policy is
  treated as ABSENT), `:744-752` (`web_auth_store_ever_enabled()` returns false on any
  error except INVALID_SIZE).
- Scenario: on a commissioned board, `hal_kv_open` fails for a moment (heap pressure near
  the 8 KB internal floor, NVS handle exhaustion, an NVS error code during a concurrent
  write). Both the policy read and the marker read fail the same way, so the policy reads
  ABSENT and `http_auth_policy_web_enabled()` returns false. `http_auth_check()` then
  allows every tier for that request (`http_auth_enforce.c:34`), including
  `/api/ota/esp`, `/api/factory_reset` and `/api/auth/security`. An attacker who can
  cause or wait for memory pressure (for example by holding many sockets) gets an
  unauthenticated admin request through. The AU-02 marker does not help, because it is
  read through the same failing path.
- Fix: map only `HAL_NOT_FOUND` to ABSENT; any other error on the policy key returns
  UNREADABLE (fail closed). `web_auth_store_ever_enabled()` returns true on every result
  except `HAL_NOT_FOUND`. Host test with a KV stub that fails open with a non-NOT_FOUND
  code.

## AZ-03 LOW: session table changed from the LVGL task without a lock

- Where: `firmware/KilnFW/App/drivers/net/auth_reset_gesture_wiring.c:58,61` call
  `web_auth_table_destroy_all()` on the LVGL task. The table
  (`http_session_iface.c`, `web_auth_session.c`) has no lock; httpd creates, finds,
  touches and reaps slots at the same time.
- Scenario: the owner does the four-corner reset while a browser logs in or polls. A
  `create_session` on httpd and a `destroy_all` on LVGL interleave, which can leave a
  slot `in_use` with a half-written hash or role. The double destroy narrows the window
  but does not make the operations atomic. The same file's comment says the TOTP RAM is
  never touched from LVGL for this reason, but the session table is.
- Fix: post a "destroy all sessions" request that httpd applies before its next session
  lookup (same shape as `totp_wipe_request_post()`), or put a mutex around the session
  table.

## AZ-04 LOW: 16 locked local lockout slots refuse every local login

- Where: `firmware/KilnFW/App/drivers/http/web_auth_login_http.c:220-222` (no slot when all
  16 are in use and locked, because locked slots are never evicted) and `:374`
  (`locked = (slot == NULL) || ...`).
- Scenario: a LAN host takes 16 addresses in the board's subnet (one DHCP-less alias each)
  and fails a few logins from each. All slots lock; the owner's own login from a new IP
  then gets 429 until one ladder step expires, again and again. Per-IP slots also give
  the attacker 16 parallel guessing ladders.
- Fix: add a board-wide local failure ceiling alongside the per-IP slots, and when the
  table is full, use a shared local overflow slot (like `s_remote_login_slot`) instead of
  refusing outright.

## AZ-05 INFO: `http_auth_caller_is_admin()` ignores `bootstrap_needed`

- Where: `firmware/KilnFW/App/drivers/http/http_auth_http.c:233-240`.
- Scenario: `http_auth_check()` denies an admin session while bootstrap is needed
  (`http_auth_enforce.c:107`), but this helper (used for admin-only fields on OPEN routes,
  `dashboard_status_http.c:883`, `ota_http_esp.c:652`, `http_auth_disclosure_gate.c:11`)
  returns true for the same session. All current clear paths destroy sessions, so no live
  case exists; a future clear path that forgets to would disclose admin fields.
- Fix: implement it as `http_auth_check(ROUTE_TIER_ADMIN, role, enabled, bootstrap, ...)
  == ALLOW`.

## AZ-06 INFO: hash failure zeroes the session digest

- Where: `firmware/KilnFW/App/drivers/http/http_session_iface.c:78-88`.
- Scenario: if `psa_hash_compute` fails both when a session is minted and when a request
  is resolved, both digests are all zero and any token string matches that slot.
  Unlikely (PSA SHA-256 does not fail in normal use), but the failure is not fail-closed.
- Fix: return a status from `sha256()`; on failure refuse to mint
  (`web_auth_login_http.c`) and resolve to `HTTP_AUTH_ROLE_NONE`.
