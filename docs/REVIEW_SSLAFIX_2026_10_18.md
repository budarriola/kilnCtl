# Review: SS/LA auth fixes (2026-10-18)

Read-only review of the SS-01..SS-04 and LA-01/LA-02 fixes at origin/dev 623535f1f (main 179cb0058).
No board, no target builds.

Counts: HIGH 0, MED 0, LOW 2, INFO 4. Rows in KNOWN_ISSUES under SL-.

## Scope
- `drivers/net/web_auth_session.c`: SS-02 USER-first eviction, SS-03 12 h absolute lifetime.
- `drivers/http/web_auth_login_http.c`: SS-01 per-(slot, role) ladders.
- `drivers/ui/ui_lcd_keypad.c`, `ui/lcd_auth_state.c`: LA-01 worker flag, post retry, failure-path count.
- `drivers/persist/web_auth_store.c`: LA-02 PIN hash/salt wipe.

## Findings
- **SL-01 LOW**: SS-02 picks the victim by the role already in the table, not the role being created. With every
  slot held by ADMIN sessions, a USER login evicts the LRU ADMIN session. A USER holder can't evict an admin
  while any USER slot exists, so this needs an all-admin table. Fix: when the new role is USER and no USER
  slot exists, evict nothing and refuse (or evict only expired slots).
- **SL-02 LOW**: SS-03 ages from `issued_ms` (uint32). Under timeout NEVER the lifetime check is skipped and
  `session_wrap_guard` does not reset the table, so a session can live longer than 2^32 ms. If the owner then
  changes the timeout to a finite value, `now - issued_ms` aliases and the 12 h cap can be missed by up to
  one wrap's worth of age. Needs 49.7 days uptime and a policy change. Fix: reap all slots when the timeout
  changes from NEVER to finite, or store a 64-bit issue time.
- **SL-03 INFO**: SS-01 ladders choose the role from the username. Admin guesses must use the admin username,
  so they always reach the ADMIN ladder. The KDF runs only after the role's ladder is checked, and every
  verify is recorded under `s_login_lock` (fail-closed on lock timeout). I found no bypass.
- **SL-04 INFO**: LA-01 is sound. The worker counts a wrong PIN before it clears `s_worker_running`
  (seq_cst atomics). `try_submit()` checks `s_post_failed` and then `s_worker_running`, so `submit_begin`
  cannot run at the same time as the worker's `submit_finish`. `s_ks` persists across close and reopen
  (`build_overlay` runs only once), so cancel and reopen does not reset the LCD ladder.
- **SL-05 INFO**: The web and LCD ladders are RAM-only, so any reboot clears them. An unauthenticated client
  has no reboot route, so this is the same as before the fixes. The LCD and web ladders are separate by
  design (WEB_AUTH_PLAN section 7), so they share no state to race on.
- **SL-06 INFO**: `web_auth_lcd_session_*` (web_auth_session.c) has no callers outside tests and no SS-03
  lifetime. It is dead code. Remove it, or apply the lifetime if it is ever wired up.
