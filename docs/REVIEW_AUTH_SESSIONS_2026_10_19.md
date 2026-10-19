# REVIEW_AUTH_SESSIONS_2026_10_19 (AX)

Holistic review of web auth sessions on origin/main 285e798cb after the SS-01..SS-04, SL-01/02/07,
DF-01, GD-04, AZ-04 and FS-02 point fixes. Review only, no board. KNOWN_ISSUES rows AX-01..AX-09.

Counts: HIGH 1, MED 1, LOW 3, INFO 4.

Files: `App/drivers/net/web_auth_session.c`, `App/drivers/http/http_session_iface.c`,
`web_auth_login_http.c`, `http_auth_http.c`, `http_auth_enforce.c`, `security_backend_web_auth.c`,
`net/auth_reset_gesture_wiring.c`, `test/test_http_session_iface.c`.

## AX-01 HIGH: session table wiped on every access after 49.7 days uptime
`hal_time_now_ms()` returns `uint64_t` (hal_time.h:59), but OF-01 made `s_last_access_ms` a
`uint32_t` on the premise "hal_time_now_ms() is a uint32 ms tick" (http_session_iface.c:100-103).
`session_table_gap_wrapped()` compares the full 64-bit `now64` against that truncated value. Once
uptime reaches 2^32 ms, `now64 - last` is always at least 2^32 (>= `SESSION_WRAP_GAP_MS` = 2^31), so
every `session_wrap_guard()` call (resolve, status, touch, any_session_active) runs
`web_auth_table_init()`. A login mints a session and the next request finds the table empty: with
web auth on, nobody can hold a USER or ADMIN session after ~49.7 days uptime until reboot. This
includes POST /api/profile_exec/stop (USER tier); the LCD still works.
`http_auth_any_ap_session_active()` also reads false from then on. The test
`test_session_expired_after_idle_longer_than_tick_wrap` passes for this wrong reason (gap wipe, not
reap), and no test makes a session past 2^32 ms uptime and resolves it twice.
Fix: keep `s_last_access_ms` and the compare in one 32-bit width (`(uint32_t)(now - last) >= 2^31` on
uint32 values), or store a 64-bit value under a spinlock. Add a host test: advance to 2^32 + 10 s,
create a session, then resolve it twice, expecting ADMIN both times. Negtest it against the current code.

## AX-02 MED: a login that straddles the four-corner reset keeps its session; bootstrap does not clear sessions
`auth_reset_gesture_clear_all()` posts destroy-all before and after the credential wipe and says a
login "that verified against the old password just before the clear cannot leave a fresh session
behind". But the post is applied lazily by the next `http_session_table()` call, and that call is the
login's own `web_auth_table_create_session(http_session_table(), ...)` (web_auth_login_http.c:635).
Here is the order: httpd verifies the old password (KDF, hundreds of ms), the LVGL task posts, wipes
and posts again, then httpd applies destroy-all and creates the new session right after. The session
minted with the old password survives. A LAN client that loops logins with the old credentials keeps
httpd inside the verify window most of the time, so hitting the race is likely. Then:
- USER-tier routes ALLOW any session even while `bootstrap_needed` (http_auth_enforce.c, ROUTE_TIER_USER
  case). FS-02 "dashboards only until admin re-setup" does not hold for this session.
- `auth_bootstrap_password_post_handler()` (security_backend_web_auth.c:506) sets the admin password
  but invalidates no sessions. A race-minted ADMIN session becomes full ADMIN once the new owner
  bootstraps.
Fix: in login_post_handler, snapshot the destroy-all posted generation before verify and refuse to
mint (401, same body) if it changed. Also destroy all web sessions on a successful bootstrap_password.
Host test for both.

## AX-03 LOW: session token copies outside login are not wiped
SS-04 wipes the token at login only. `resolve_role_for_request()` keeps `token[128]` on the httpd
stack, copies it to the caller's `out_token`, and the prehandler re-extracts it. The `token_hash[32]`
locals in `http_auth_session_resolve/_status/_touch/_logout` are left in place. The bearer token is in
stack residue on every authenticated request. Fix: volatile-wipe these buffers before return.

## AX-04 LOW: a SHA-256 failure produces an all-zero key that matches another all-zero key
`sha256()` in http_session_iface.c writes zeros on `psa_hash_compute` failure. Login never checks
this, so a session can be created keyed on 32 zero bytes. Any later lookup whose hash also fails then
matches it (only the IP binding protects it). Fix: return a status, refuse the login and resolve NONE
on failure.

## AX-05 LOW: re-login never revokes the caller's previous session; ADMIN logins can starve USER
Login ignores the presented `kiln_sid` cookie, so each login takes a new slot and the old token stays
valid until idle or the 12 h cap. Eight ADMIN logins (one browser re-logging) fill the table. After
that every USER login gets the SL-07 503 for up to the idle timeout (indefinitely under NEVER, where no
lifetime cap applies). There is no session fixation: the server always mints the token. Fix: on login,
destroy the slot matching the presented cookie, if any.

## INFO
- **AX-06** Token compare is constant-time over the hashed key (`web_auth_session_constant_time_equal`).
  `find_by_token` returning on the first match leaks only the slot index, not token bytes.
- **AX-07** Locking: all table mutations run on the single httpd worker. Cross-task callers (gesture,
  LCD) only post generations, and the Wi-Fi task uses the peek path. The peek reads slots unlocked while
  httpd may memset one, so it can see a torn read of an AP-fallback boolean. This is benign, and AX-01
  dominates it. The lockout check and record are unlocked across the KDF, but httpd runs one request at
  a time, so the ladder cannot be raced.
- **AX-08** Cookie: `HttpOnly; SameSite=Strict; Path=/`, no `Secure` (the device serves plain HTTP) and
  no Max-Age (browser-session cookie); logout sets Max-Age=0. Role changes need re-login; a password
  change destroys that role's sessions, and TOTP reset and clear_credentials destroy all.
- **AX-09** Lockout ladders: SS-01 per-(slot, role) ladders close the USER-resets-ADMIN path. AZ-04
  evict-oldest lets a LAN host with more than 16 addresses rotate fresh ladders (owner-accepted
  2026-10-17). Remote and unknown scopes share one slot. Timeout input is bounded 1-60 min or never, so
  `timeout_s * 1000` cannot overflow.
