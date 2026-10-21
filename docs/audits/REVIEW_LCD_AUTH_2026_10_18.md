# Review: LCD-side auth in KilnFW (origin/dev), 2026-10-18 (HIGH 0, MED 0, LOW 1, INFO 4)

Review only, no board. Scope: PIN entry, USER vs ADMIN roles, inactivity lock, PIN lockout,
FS-02 storage-reset banner and latch exit, LCD trip clear (admin, owner 2026-10-07), Safety
page USER PIN (owner 2026-10-09), dashboards only when unauthenticated (owner 2026-09-28).

Files: `ui/lcd_auth_state.c`, `ui/lcd_credential_bridge.c`, `ui/ui_lcd_lock.c`,
`ui/ui_lcd_keypad.c`, `ui/kiln_ui.c`, every `ui/ui_page_*.c` gate, `persist/web_auth_store.c`.

## Findings
- **LA-01 LOW** `ui_lcd_keypad.c:245-249`: an `lv_async_call()` failure drops a wrong-PIN result
  without recording it in the lockout; `s_worker_running` is a non-atomic cross-task flag.
- **LA-02 INFO** `web_auth_store.c:714-723`: computed hash and stored record left on the stack.
- **INFO (dup LU-02, accepted 2026-10-16)**: PIN lockout is RAM-only, so a power cycle clears it.
  Not re-filed.
- **INFO (verified)** Gates: every navigation into Config hub, Menu, Profiles, Temperature,
  Network, Diagnostics, Safety (home strip and Config cell) is `run_gated(USER)`; trip clear,
  relay toggle, units, Wi-Fi mode/AP/networks, profile save/delete, live edit, crash ack and
  touch recalibration are ADMIN, and the action bodies re-check `has_role`. Only exceptions:
  first-boot touch_cal when uncalibrated and touch_test after a saved calibration (owner
  2026-10-10). Relock edge closes keypad, confirm, num pad and returns home; a pending
  force-lock counts as locked in `has_role`; disabling the policy force-locks every tick.
- **INFO (verified)** Storage: PINs are salted PBKDF2, compared with
  `web_auth_constant_time_equal`; USER and ADMIN PINs cannot collide (SAME_AS_OTHER).
  Unreadable or absent-on-commissioned policy fails closed (locked). FS-02: latch only with
  no policy blob, which then loads UNREADABLE (LCD locked, banner shown); setting an admin
  password or admin PIN (`web_auth_store.c:680, 783`) runs `storage_reset_resolve()`, which
  writes a policy with the LCD lock on. Verification runs off the LVGL task, one attempt at a
  time; an attempt cancelled mid-verify still counts if wrong.

No LCD screen or action found reachable without its required role.
