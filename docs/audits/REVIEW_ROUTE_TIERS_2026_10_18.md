# Review 2026-10-18: route tiers, LCD roles, sessions

Scope: KilnFW on origin/dev. Review only, no builds, no bench. This review skips items already in
docs/KNOWN_ISSUES.md (EL-03 USB clear role-free, AZ-*, FX-*, F2-*, AF-*, HR-*).

Counts: HIGH 0, MED 1, LOW 3, INFO 4 (RT-05..RT-08).

## HTTP routes and tiers (route_tier_table.h)

- OPEN: `/`, `/login`, POST `/api/auth/login`, GET `/api/auth/session`, POST `/api/auth/forgot`,
  POST `/api/auth/reset`, `/app.js`, `/nav.js`, `/theme.css`, `/commissioning_shared.js`, `/api/status`,
  GET `/api/profile_exec`, `/api/readiness`, `/api/history.csv`, `/api/profile_plan`, `/status`
  (the payload is redacted through `http_auth_may_disclose`).
- WIFI_SETUP: `/scan`, `/networks`, `/wifi`.
- ADMIN_BOOTSTRAP: POST `/api/auth/bootstrap_password`.
- USER: POST `/api/auth/logout`, `/api/auth/session/extend`, the profile_exec start/stop/pause/resume/ack_last_run
  routes, GET `/api/profile`, `/api/profiles`, `/api/profiles/builtin`, `/api/profiles/favorites`,
  `/api/profile/export`, GET `/api/kiln_configs`, GET `/api/kiln_configs/apply_status`.
- ADMIN: every other route (table rows 232-478). That includes `/api/safety/*`, `/api/estop/verify`,
  `/api/ota/*` (including boot_guard_reset), `/api/factory_reset`, `/api/cfgfs/*`, `/api/update/*`,
  `/provision`, `/forget`, `/ip_config` and the settings pages.
- SAFETY_REDUCE: the tier is defined but no route uses it.
- Every route registers through `kiln_http_register`. The only direct `httpd_register_uri_handler` call is
  that wrapper (http_auth_http.c:531). A route with no table row falls back to ADMIN, so a missing row
  fails closed.

## LCD actions and roles

| Action | Role |
|---|---|
| Config hub: temperature, network, diagnostics, safety, profiles (ui_page_config.c:80,92,123,135,152) | USER |
| Touch calibration (ui_page_config.c:111), units (199) | ADMIN |
| Crash acknowledge (diagnostics :1564) | ADMIN |
| Edit firing apply/decide (edit_firing :391,:396; home_actions :405), live decide (:137) | ADMIN |
| Start/stop, pause/resume, Menu, pick profile (home_actions :330,:333,:375,:435,:463) | USER |
| Home trip strip to the Safety page (ui_page_home.c:235, `lcd_safety_strip_needs_pin`) | USER |
| Network mode, AP identity, add/forget network | ADMIN |
| Save profile (builder_review :166), delete profile (picker :245) | ADMIN |
| Profile detail start (:559) | USER |
| Safety clear (ui_page_safety.c:238, re-checked in `clear_action`, via `dashboard_safety_clear_trip`) | ADMIN |
| Relay toggle (temperature :439; the backend refuses while RUNNING) | ADMIN |

`ui_lcd_lock_has_role` returns true when the LCD lock policy is disabled, which matches the owner decision.

## Owner-decision checks

- Unauthenticated users get dashboards only, on the web and the LCD: holds. The only OPEN routes are dashboard reads
  and auth flows.
- AP-HMAC routes need a web admin login: holds. The HMAC gate is retired and those routes are ADMIN.
- LCD clear needs an admin login and follows the web clear: holds.
- The LCD Safety page needs a USER PIN: holds. The only ways in are the hub and the home strip, and both are USER.
- Safety writes are refused with 409 while running: partial. See RT-02, RT-03 and RT-04.
- FS-02 wiped auth leaves dashboards only until an admin is set up: there is a gap. See RT-01.
- EL-03 USB clear is role-free: already accepted, so it is skipped.

## Session checks

- Expiry is idle-only (`web_auth_session_is_valid` uses `last_seen_ms`). See RT-05.
- Destroy-all uses a posted/applied generation counter that httpd applies (http_session_iface.c). It is sound
  apart from RT-06.
- Origin check: non-GET/HEAD requests are cross-origin-refused before role resolution. See RT-07.
- Timing-safe compares: password (`web_auth_constant_time_equal`), session token hash
  (`web_auth_session_constant_time_equal`) and TOTP (`totp_constant_time_equal`). All are sound.
- Cookie: `kiln_sid` is HttpOnly, SameSite=Strict and Path=/. Logout sends Max-Age=0. Sessions are IP-bound,
  and a full table evicts the LRU slot.

## Findings

**RT-01 MED** (persist/web_auth_store.c:519, security_backend_web_auth.c bootstrap handler).
While the FS-02 storage-reset latch is held and the admin key is NOT_FOUND,
`web_auth_store_admin_configured_or_unreadable` checks only `ever_enabled`, never `s_storage_reset_latched`.
The admin therefore reads as not configured and `web_auth_admin_bootstrap_needed` returns true, so the OPEN
POST `/api/auth/bootstrap_password` lets any unauthenticated LAN client set the admin password. The FS-02
owner decision says the web stays dashboards only until an admin re-setup. REVIEW_FS02 noted this
("open to the first LAN client") but filed no row. Fix direction: refuse bootstrap while the latch is held,
or confirm with the owner that first-LAN-client bootstrap is accepted.

**RT-02 LOW** (safety_cfg_http.c:2333, `rate_guard_auto_post_locked`). POST `/api/safety/rate_guard/auto` has
no `safety_cfg_mode_gate_refuse` 409 gate. It writes Pico param 0x0204 and with confirm=1 can loosen it while
a firing runs. The Pico ARMED write refusal is a backstop. The HR-02 fix covered other POSTs but not this one.

**RT-03 LOW** (safety_cfg_http.c:2049, `bench_preset_post_handler`). There is no 409 mode gate here; the only
check is `http_async_job_busy`. The handler stages and commits 32 Pico params. The Pico ARMED refusal is a backstop.

**RT-04 LOW** (safety_cfg_http.c:1763, `ct_auto_zero_post_handler`). It refuses while running or autotuning
but answers 200 with ok:false, not the gate's 409. It also does not cover a backup restore in flight.
`safety_cfg_mode_gate_refuse` covers both.

**RT-05 INFO** (net/web_auth_session.c). There is no absolute session lifetime. `issued_ms` is never read,
expiry is idle-only, and a "never" timeout (-1) is accepted (security_http_core.c). IP binding and
SameSite=Strict limit the impact.

**RT-06 INFO** (security_backend_web_auth.c, TOTP reset). The TOTP reset calls `web_auth_table_destroy_all`
directly and does not call `ui_lcd_lock_force_lock`, unlike `invalidate_sessions_for_role`. An open LCD
session survives the reset.

**RT-07 INFO** (http_origin_check.h). The origin compare ignores the scheme and assumes port 80 by default. A
request with neither Origin nor Referer is allowed as a non-browser client. Because the cookie is
SameSite=Strict, a browser CSRF cannot ride a session, so this is informational.

**RT-08 INFO** (route_tier_table.h). The SAFETY_REDUCE tier is defined but no route uses it. It is dead code,
which is harmless.
