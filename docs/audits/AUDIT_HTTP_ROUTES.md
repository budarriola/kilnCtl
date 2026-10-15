# Audit: KilnFW HTTP routes (auhttp, 2026-10-14)

Scope: `firmware/KilnFW/App/drivers/http/` and every `httpd_uri_t` handler
registered under `firmware/KilnFW/App/drivers/`, on `origin/dev` at
`153c12cf5`. Read-only audit; no code changed. Paths below are relative to
`firmware/KilnFW/App/drivers/` unless they start with `firmware/` or `docs/`.

Areas covered:

- route tiers against `http/route_tier_table.h` and the owner decisions
  (unauthenticated = dashboards only; relay/zone/config writes 409 while a
  run is active; former AP-password HMAC routes are plain ADMIN)
- request body bounds and partial-recv handling
- stack-resident buffers in the 8192 B httpd task
- JSON output truncation and escaping
- concurrency with the update fetch (httpd serving while the TLS fetch runs)
- session leaks and missing unlocks on error paths
- NVS/cfg writes without the read-back the plans require

Prior reviews in `docs/audits/REVIEW_*.md` that touch HTTP were read first;
items already reported or fixed there are not repeated.

Summary: **HIGH 0, MED 3, LOW 9, INFO 10.**

## HIGH

None.

## MED

### MED-1: `POST /api/cfgfs/format_confirm` has no writer fence

- Where: `http/cfg_fs_format_http.c:44-111` (`format_confirm_post_handler`;
  mode gate at :57-65, decision at :83-97, `cfg_fs_confirm_format_device()`
  at :102); job body `persist/cfg_fs_mount.c:561-611`
  (`cfg_fs_confirm_format_job_run`: VFS unregister, deinit, format, remount
  on the flash worker).
- Scenario: an admin confirms a format (pending-format state, or
  `force_healthy=1`) while a backup restore job (`http_async_job`), a
  `kiln_configs/apply`, or a factory reset is in progress. The handler checks
  only the system mode gate and the recovery/healthy decision. No check of
  `http_async_job_busy()`, `backup_import_restore_in_flight()`, an active
  kiln_cfg apply, the factory-reset mark, `persist_reset_barrier()`, or the
  OTA interlock.
- Consequence: the format job is queued on the flash worker between the
  other writer's jobs. The restore or apply then reports success while the
  files it wrote are gone, or the remaining writes land on a freshly
  formatted, half-populated partition. Since the cfg dual-write close made
  `cfg` authoritative, the next boot loads whatever survived.
- Fix: refuse with 409 when any of those writers is active (or take
  `persist_reset_barrier()` for the duration), and refuse with 428 while the
  OTA interlock is held. Re-check inside the job before unregistering the VFS.

### MED-2: `POST /api/cfgfs/file` is not mode-gated

- Where: `http/diagnostics_http.c:1883-1975` (`cfgfs_file_post_handler`).
  It checks the name, the body size and `cfg_fs_is_available()`; there is no
  `system_mode_gate_check()` call anywhere in `diagnostics_http.c`.
- Scenario: an admin (or the PC tool) writes a zones/profile/config file
  while a firing or autotune is running.
- Consequence: violates the owner decision of 2026-09-25 (blanket refuse
  relay/zone/config writes while running, 409). The on-disk config silently
  diverges from the RAM config the run uses; a later save overwrites the
  upload, or the next boot loads a config nobody applied.
- Fix: add the same gate block `cfg_fs_format_http.c:57-65` uses before
  the body is read, re-check it in `cfgfs_file_write_job`, and list the route
  in `docs/SYSTEM_MODE_GATE.md`.

### MED-3: the physical reset gesture leaves live web sessions valid

- Where: `net/auth_reset_gesture_wiring.c:37-42`
  (`auth_reset_gesture_clear_all()` calls only
  `web_auth_store_clear_for_physical_reset()` and `totp_config_clear()`).
  The only `web_auth_table_destroy_all()` callers are
  `http/security_backend_web_auth.c:280` (HTTP clear_credentials) and
  `http/auth_totp_http.c:549` (TOTP reset). The bootstrap handler
  (`http/security_backend_web_auth.c:455-516`) sets the new password via
  `web_auth_backend_set_web_password()` (:144-179, called at :496), which
  invalidates nothing.
- Scenario: the owner performs the four-corner reset because the admin
  password leaked. During the bootstrap window `http_auth_check()` refuses
  ADMIN for every role (`http/http_auth_enforce.c:101-108`), but
  `ROUTE_TIER_USER` (:96-100) still allows the old admin session (profile
  start/stop/pause/resume, profile reads), and `http_auth_caller_is_admin()`
  (`http/http_auth_http.c:233-240`) ignores `bootstrap_needed`, so
  disclosure-gated pages (`/status`, `/api/readiness`) return unredacted data
  to it. Once the owner bootstraps the new password, the old cookie (same
  client IP, within its idle timeout or under a "never" policy) is full ADMIN
  again. A pending TOTP enrollment started by the old session also survives.
- Consequence: the credential-recovery gesture does not evict the holder of
  the old credential. Same reset-one-side class as REVIEW_WEB4_TESTS C3,
  which fixed only the HTTP clear_credentials path.
- Fix: in `auth_reset_gesture_clear_all()` also call
  `web_auth_table_destroy_all(http_session_table())`,
  `ui_lcd_lock_force_lock()`, `security_totp_pending_clear()` and
  `auth_totp_http_clear_reset_tokens()`. Destroy all sessions after a
  successful bootstrap as well, and make `http_auth_caller_is_admin()` return
  false while bootstrap is needed.

## LOW

### LOW-1: backup export leaves the chunked response unterminated on error

- Where: `http/backup_export.c:905-920`: the zero-length terminating chunk
  is sent only when `s.err == ESP_OK`, and the handler returns `ESP_OK`
  either way.
- Consequence: on a mid-stream error the connection is kept alive with an
  open chunked body; the client hangs until its timeout and may save a
  truncated export that looks complete.
- Fix: return `ESP_FAIL` on a stream error so httpd closes the socket.

### LOW-2: profile slot reads without the slot lock produce torn copies

- Where: `http/profiles_http.c:1726-1772` (`profiles_http_get`, unlocked
  `*out = s_profiles.profiles[id];` at :1771) and
  `http/profiles_catalog_http.c:333`, `:423`. Callers include
  `uart_bridge_ext_control.c:309/324/352`, `backup_export.c:367`,
  `backup_import.c:160/1267`, `dashboard_exec_http.c:346`,
  `profiles_export_http.c:53`, `profiles_live_http.c:66`.
- Scenario: a read runs concurrently with a profile save to the same slot.
- Consequence: a catalog, UART or backup-export copy mixes old and new
  segments. The executor start is protected by the `profiles_http_slot_rev`
  seqlock (`profile_executor_run.c:310`, re-check at :464); these readers are
  not.
- Fix: route every reader through the slot-rev retry loop the executor uses,
  or copy under the profiles lock.

### LOW-3: `update_stage_get_status` hashing blocks a concurrent download

- Where: `update/update_stage.c:586-668` claims the stage CHECKING state and
  hashes the image on the httpd task; `update/update_fetch.c:965-976`
  returns a `wr_call(WR_BEGIN, ...)` failure as "busy"
  (`claim_upload` succeeds only from IDLE, `update_stage.c:37-48`).
- Scenario: the update page polls stage status while a GitHub download is
  about to begin its stage write.
- Consequence: the download fails "busy" for no real conflict, and httpd is
  stalled for the hash duration while TLS is running.
- Fix: skip the claim and hash while `fetch_busy_probe()` reports busy
  (report the cached status), or have the fetch retry WR_BEGIN while the
  state is CHECKING.

### LOW-4: "user and admin LCD PINs must differ" is never enforced

- Where: `http/security_http.c:475` always sets
  `sreq.lcd_pin_other[0] = '\0'`; `security_pin_is_valid()`
  (`http/security_http_core.c:20`) skips the comparison when `other` is
  empty; `http/security_backend_web_auth.c:188-195` assumes core already
  checked. `ui/lcd_credential_bridge.c:17-20` tries the ADMIN PIN first.
- Consequence: if both PINs are set to the same digits, anyone with the USER
  PIN unlocks the LCD as ADMIN. The rule in `docs/WEB_AUTH_PLAN.md` item 3 is
  silently not enforced.
- Fix: in `web_auth_backend_set_lcd_pin()`, refuse when
  `web_auth_store_verify_pin(<other role>, pin)` returns true.

### LOW-5: expired sessions are never reaped; the 32-bit ms clock wrap revives them

- Where: `net/web_auth_session.c:170-180` computes
  `elapsed_ms = now_ms - last_seen_ms` in `uint32_t`; slots are freed only
  by logout, LRU eviction or destroy-all, never on a failed resolve
  (`http/http_session_iface.c:208`, `:257`).
- Consequence: on a board up past 49.7 days, a session that expired is valid
  again for up to its timeout window around each wrap, with its old role, for
  a client on the same IP that kept the cookie. It also keeps
  `http_auth_any_ap_session_active()` true during that window.
- Fix: destroy the slot as soon as `web_auth_session_is_valid()` fails on
  resolve, or keep timestamps in 64 bits.

### LOW-6: credential-bearing stack buffers are not wiped

- Where: `http/security_http.c:393` (`body[513]`) and `:436` (`sreq`) hold
  passwords, PINs or TOTP codes and are never zeroed;
  `http/wifi_provision_http.c:740` (`body[385]`) and the STA/AP password
  copies at `:794`, `:831`; `http/web_auth_login_http.c:557-589`
  (`token_raw`, `token_hex`, `cookie`) and `:472` (`admin_record`, hash and
  salt).
- Consequence: secrets remain in the shared httpd stack and can surface in a
  later coredump or crash-report export.
- Fix: zero them with the existing wipe helpers (`login_wipe`,
  `totp_secure_zero`) on every return path.

### LOW-7: username not validated on the bootstrap/reset path, under-escaped on output

- Where: `web_auth_backend_set_web_password()`
  (`http/security_backend_web_auth.c:144-179`) checks only non-empty;
  `username_is_valid()` (`http/security_http_core.c:63-75`) runs only on the
  `/api/auth/security` path. `security_config_get_handler`
  (`http/security_http.c:94-103`) escapes only `"` and `\`;
  `totp_build_otpauth_uri()` (`net/totp.c:370-373`) inserts the username raw
  and `http/security_http.c:293` embeds that URI unescaped.
- Consequence: a bootstrap username containing a control character or `"`
  makes `GET /api/auth/config` and TOTP enroll-begin return invalid JSON; a
  `?`, `&` or `#` corrupts the otpauth label. Admin-only and self-inflicted,
  but the security page breaks until the username is changed.
- Fix: call `username_is_valid()` inside `web_auth_backend_set_web_password()`
  for the admin role; JSON-escape control characters; percent-encode the
  otpauth label.

### LOW-8: TOTP backoff failure is dropped when its lock is contended

- Where: `http/auth_totp_http.c` `totp_backoff_record()`: when
  `xSemaphoreTake(s_totp_lock, 50 ms)` fails, the failure is not recorded and
  the request proceeds. Login refuses with 500 in the same situation
  (`http/web_auth_login_http.c`).
- Consequence: a small backoff leak under concurrent load; the board-wide
  forgot cap still bounds it.
- Fix: refuse with 500 when the record cannot be taken, as login does.

### LOW-9: `log_http.c` skips the chunk terminator on a send failure

- Where: `http/log_http.c:52-61`.
- Consequence: same as LOW-1 for the log download, on a client that drops
  mid-stream; the socket stays open until httpd's own timeout.
- Fix: return `ESP_FAIL` on a failed chunk send.

## INFO

- `http/adaptive_tune_http.c:189-205`: the enable path is check-then-act
  against the run state; the executor re-checks, so this only gives a
  misleading 200.
- `http/iter_tune_http.c:221-238`: a refusal while a run is active returns
  500, not the 409 the mode-gate decision specifies.
- `http/update_http.c:431`: the upload gate is cleared unconditionally on
  exit, including on paths that never claimed it. Harmless today because
  claim is single-flight.
- `update/update_fetch.c:443-458`: backoff fields are read without the fetch
  lock; worst case a stale retry delay.
- Route registration: 178 registered `(uri, method)` pairs match 178
  `route_tier_table.h` rows exactly; `kiln_http_register()`
  (`http/http_auth_http.c:531`) is the only direct
  `httpd_register_uri_handler()` call site. `max_uri_handlers` is 190
  (`http/wifi_provision_http.c:1260`) and `KILN_HTTP_MAX_ROUTES` is 192, so
  12 slots are spare; the 176/184 figures in CLAUDE.md are stale.
- `/provision`, `/forget`, `/ip_config` are ADMIN
  (`http/route_tier_table.h:427-429`) while `/wifi`, `/scan`, `/networks` are
  WIFI_SETUP. With auth on and the board unprovisioned, the captive portal can
  scan but needs an admin login to save credentials. Consistent with "Wi-Fi
  management is ADMIN"; whether POST `/provision` should be WIFI_SETUP is an
  owner decision.
- `net/web_auth_session.c`: if PSA SHA-256 fails the token hash is all
  zeros, so two failed-hash slots/tokens could match. Not shown reachable.
- `any_session_active()`/`any_ap_session_active()` read the session table
  without a lock from the wifi_prov_link task; worst case a stale boolean.
- sim_backend routes parse with `atoi`; `CONFIG_KILNCTL_SIM_PLANT` dev builds
  only.
- Owner-decision tiers verified: profile_exec start/stop/pause/resume and
  ack_last_run are USER; bootstrap_password is ADMIN_BOOTSTRAP; forgot/reset
  are OPEN; the former HMAC routes (boot_guard_reset, factory_reset,
  cfgfs format) are plain ADMIN; the fallback AP cannot open WIFI_SETUP
  (`net/wifi_prov.c:767`).

## Checked clean

- Update: `http/update_http.c` stage upload/clear/status,
  `http/update_install_http.c` (`kv_write_verified`), update fetch
  start/download/cancel/status, `http/update_settings_http.c`.
- OTA/reset: `http/ota_http_recovery.c` recovery_boot, `http/ota_http_esp.c`,
  `http/ota_http_pico.c`, `http/sw_reset_http.c`.
- Dashboard: `dashboard_json.c` and `dashboard_status_http.c`
  (`kiln_json_escape_ctl`), dashboard_autotune (accept gated), dashboard_exec,
  adaptive_tune revert, iter_tune, board_temps, stack_margin.
- Config writers: aux_outputs, zone_aux_convert, zones_http_post/pid/get,
  safety_cfg_http (`cfg_writer_guarded`), `safety_cfg_write.c`,
  `http/http_async_job.c`, backup_import job, profiles_catalog, profiles_edit,
  profiles_export, profiles_live, dualwrite_window, `kiln_cfg_http.c`
  lines 130-520, partition_info.
- Diagnostics: danger, relay_cycles, crash_report, coredump, nvs_keys.
- Server config: `http/wifi_provision_http.c` (8192 B stack, 13 sockets,
  handler cap 190); status/scan/networks use heap JSON with escaping and
  redaction; provision/forget/ip_config recv loops are bounded.
- Auth: `http/http_auth_enforce.c` (fails closed to ADMIN on a missing row),
  `http/http_auth_disclosure_gate.c`, `http/http_auth_policy_iface.c`,
  `http/http_auth_http.c` (prehandler, cookie extraction, Host/Origin
  gating), `http/http_session_iface.c` (UNREADABLE policy fails closed),
  `http/login_ip_scope.c`, `http/web_auth_login_http.c` (lockout fails
  closed, bounded recv, password wiped, HttpOnly + SameSite=Strict cookie),
  `http/web_auth_session_status_http.c`, `http/auth_totp_http.c` and
  `http/totp_http_core.c` (constant-time compare, single-use tokens, reset
  destroys all sessions), `http/security_http_core.c` dispatch,
  `http/security_backend_web_auth.c` set_policy and clear_all_credentials,
  `http/setup_wizard_http.c`, `http/setup_progress_http.c`.
- No handler stack frame in the auth slice exceeds about 1.1 KB; no truncated
  JSON was found sent as 200.

## Not deeply reviewed

`http/settings_http.c`, `http/dashboard_settings_http.c`,
`http/dashboard_http.c`, `profiles_validate.c`, `http/backup_http.c`,
`kiln_cfg_http.c` lines 520-841.
