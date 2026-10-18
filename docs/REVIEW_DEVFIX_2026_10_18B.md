# REVIEW_DEVFIX_2026_10_18B

Read-only adversarial review of origin/dev since main 179cb0058 (to 6fb770155): OF-01/02, RB-01/02,
SL-01/02/07/08, UB-01/02/03, the last_verify_us and stack_margin_retain_exit hooks, lvgl stack 14336.
No board, no target builds.

Counts: HIGH 0, MED 0, LOW 2, INFO 4.

## Findings

- **DF-01** LOW, `web_auth_login_http.c` (SL-07 503 path). The early return after
  `web_auth_table_create_session()` refuses skips the SS-04 wipe of `token_hex` and `token_hash`
  (the token was minted but never stored). No session is created, so nothing leaks a half-built
  session; only stack residue of an unused token. Fix: `login_wipe()` both before the 503 return.
- **DF-02** LOW, `stack_margin.c` `stack_margin_retain_exit()`/`stack_margin_retained()`. Written by
  update_fetch_wr and read by the uart bridge task with no lock or barrier; `retained_valid` can be
  observed before `retained_bytes` (compiler/store reorder), giving one get_stack_margin reply with 0
  or a stale value. Diagnostic only. Fix: take `s_read_lock` in both.
- **DF-03** INFO, OF-01 keep logic. A FAIL result for a different image cannot be kept: the keep needs
  `old.id == req.sha256`, and the install-request path erases the old result before writing a new
  request (`res_erase` then `req_write`), so a matching FAIL is from this attempt. A res_read error
  falls back to the old overwrite. No issue.
- **DF-04** INFO, RB-01 `xQueueSendToFront`. Only the LCD PIN job goes to the front and
  `s_worker_running` allows one at a time; queued Wi-Fi jobs keep their relative order. A PIN storm
  cannot starve them beyond one job each. No issue.
- **DF-05** INFO, UB-01 lock order. `stack_margin_read()` is called only from the uart bridge task
  (no ISR caller); it holds only `s_read_lock`. The only nested use is http_async_job (`s_mux` then
  `s_read_lock`), never the reverse, so no deadlock. Critical-section length is UB-03.
- **DF-06** INFO, exit-path coverage. Every registered self-deleting task now calls
  `stack_margin_task_exit()` before `vTaskDelete`/`vTaskDeleteWithCaps`. Remaining bare `= NULL`
  stores are create-failure paths (task never ran) or unregistered tasks (ota_pico_relay). The retained
  high-water is keyed to the "update_fetch_wr" entry and shown only while that slot is NULL, so it is
  reported for the right task.
