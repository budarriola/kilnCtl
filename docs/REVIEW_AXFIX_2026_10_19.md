# REVIEW_AXFIX_2026_10_19 (AY)

Review of the AX-01..AX-05 fixes (code 86e5ebfc2, docs bd784b3cc) against
REVIEW_AUTH_SESSIONS_2026_10_19.md. Review only, no board, target build not run (checked by inspection).
KNOWN_ISSUES rows AY-01..AY-07.

Counts: HIGH 0, MED 0, LOW 3, INFO 4.

## AY-01 LOW: AX-03 wipe is incomplete
The review named `resolve_role_for_request()`'s `token[128]`. That buffer is still not wiped
(http_auth_http.c:205). Two more non-login copies are also left on the stack:
`web_auth_session_status_http.c:59` `token[128]` and `logout_post_handler()` `token[128]`
(web_auth_login_http.c:746). The prehandler copy and the four `token_hash` locals are wiped now.
Fix: call a volatile wipe on these three buffers before return.

## AY-02 LOW: a login refused by the AX-02 generation check has already reset its ladder
The generation check runs after the attempt is recorded under `s_login_lock`. A correct password still
calls `login_backoff_record_success()` before the 401. The client gets the same 401 body as a wrong
password, but its next attempt is not delayed, so the ladder shows that the password was correct.
Exploiting this needs a four-corner reset posted during the verify window, which is a physical action,
so the risk is small. Fix: check the generation before recording the attempt. If it moved, record
nothing (or record a failure).

## AY-03 LOW: AX-05 revokes the old session before the new one exists
`http_auth_session_logout(old_token)` runs before `web_auth_table_create_session()`. If the create
fails (SL-07 503 when the table is all ADMIN, or the GD-04 Set-Cookie 500), the client loses its
previous session and gets no new one. The revoke also ignores the IP binding and role. A caller who
has valid credentials and also presents another client's cookie can revoke that session. That is a
DoS only: presenting the cookie already implies more access, and the revoke runs only after a
successful verify, so it cannot be used to escape lockout and it gives no oracle. Fix: revoke after
the new session is created and its cookie is set, and only when the old slot's IP matches.

## INFO
- AY-04: The AX-01 seqlock is correct and needed. All writers go through `session_wrap_guard()` on
  the httpd worker. `http_auth_any_session_active()` has no non-test caller. `wifi_prov_owner` only
  reads, through `http_session_table_peek()` and `session_table_gap_wrapped()`. A 64-bit load is not
  atomic on the LX7, so a single writer still needs the counter. `__atomic_*` with SEQ_CST emits
  `memw` on Xtensa, which gives the ordering on both cores. The reader spins without a bound, but the
  odd window is 3 stores, and both tasks run at priority 5, so equal-priority time slicing prevents a
  livelock. Nothing enforces the single-writer rule; a second writer task would need an atomic RMW.
- AY-05: The 2^31 ms wipe is correct. With 64-bit storage it fires only after at least 24.8 days idle
  under a finite timeout. The timeout comes from minutes in the policy, so any session would have
  expired already. Pre-existing: `timeout_s * 1000u` in the session-status handler overflows above
  4294967 s.
- AY-06: The ESP target should compile, by inspection. `hal_time_now_ms()` is `uint64_t` on all HALs.
  The header already uses `bool`. The login file includes `http_auth_http.h` for
  `http_auth_extract_session_token`, and the bootstrap file includes `http_session_iface.h`. Both .c
  files are in the same drivers component.
- AY-07: AX-04: every caller of `sha256()` and `http_session_hash_token()` checks the bool: the login
  mint plus the resolve/status/touch/logout wrappers. Some test helpers ignore it, which is harmless.
  The AX-02 bootstrap destroy-all and the check-after-`http_session_table()` ordering are correct. A
  destroy-all posted after the check wipes the new session on the next table access.
