# REVIEW_SESSIONS_2026_10_18 (HIGH 0, MED 1, LOW 2, INFO 3)

Review-only, no board. Base: origin/dev. Scope: KilnFW web session lifecycle (create, cookie,
token entropy, expiry, logout, destroy-all, role/password changes, CSRF, constant-time compares,
login lockout AZ-04 eviction). Files: `drivers/net/web_auth_session.c`, `drivers/net/login_backoff.c`,
`drivers/http/http_session_iface.c`, `drivers/http/web_auth_login_http.c`,
`drivers/http/security_backend_web_auth.c`, `drivers/http/security_http_core.c`,
`drivers/http/auth_totp_http.c`, `drivers/net/auth_reset_gesture_wiring.c`.

## Findings

- **SS-01** MED, `drivers/http/web_auth_login_http.c:496-498`, `drivers/net/login_backoff.c:44`.
  Any successful login clears the whole backoff ladder of the slot it lands in. The slot is keyed
  by address (LOCAL) or shared by every off-subnet client (`*remote*`), not by role or username.
  A holder of the USER password interleaves one good user login after each lock expires, so every
  admin guess stays on the first 5 s step instead of escalating to 300 s. In the REMOTE bucket the
  owner's own successful login resets an attacker's ladder. Fix: key the ladder by (scope, target
  role), or clear only the role that succeeded; a host test that fails admin, succeeds user, and
  expects the next admin failure to escalate.
- **SS-02** LOW, `drivers/net/web_auth_session.c:47-54`. The 8-slot session table evicts the LRU
  slot of any role when full. A USER credential holder can log in 8 times and evict a live ADMIN
  session (and, behind LRU, every other session). Needs a valid credential; impact is a forced
  re-login. Fix: prefer evicting same-role or expired slots before another role's live slot.
- **SS-03** LOW, `drivers/net/web_auth_session.c:73`, `web_auth_session_is_valid()`. Expiry is
  idle-only; `issued_ms` is recorded but never read, so an active session (or any session under
  the "never" timeout) lives until reboot, destroy or eviction. Fix: add an absolute lifetime cap
  against `issued_ms`, or document that idle-only expiry is intended.
- **SS-04** INFO, `drivers/http/web_auth_login_http.c:541-547`. `token_raw`, `token_hex` and the
  cookie buffer stay on the httpd stack after the response; the handler already has `login_wipe()`
  for the password. Fix: wipe them before return.
- **SS-05** INFO, `drivers/http/web_auth_login_http.c:173-220`. AZ-04 evict-oldest means a host
  with 17 or more addresses can rotate fresh ladders; this is the owner-accepted tradeoff
  (2026-10-17), recorded so it is not re-reported.
- **SS-06** INFO, verified correct: token is 256 bits from `hal_sysinfo_fill_random()`, stored only
  as SHA-256; cookie is `HttpOnly; SameSite=Strict; Path=/` (no `Secure`: plain HTTP by design);
  logout clears with `Max-Age=0` and destroys only the caller's slot; exact client-IP binding on
  resolve/status/touch; unreadable policy fails closed; expired slots reaped before lookup, 2^31 ms
  wrap guard; password change (admin/user), PIN change, clear-credentials, auth enable edge, TOTP
  reset and the physical gesture (posted destroy-all, applied on httpd) all invalidate sessions;
  token and password hash compares are constant-time; state-changing routes pass the Origin/Referer
  guard (`http_origin_check.h`, see RT-07); backup import restores no credentials.
