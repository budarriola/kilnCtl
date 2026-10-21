# Review: WP-01, RF2-09/RF3-06 lock change, RS-02 refusal drain (2026-10-20)

Scope: c016030aa (WP-01), 9f0d2448f (RF2-09/RF3-06 + RS-02), on origin/dev. Review only, no code changed.
Totals: HIGH 0, MED 1, LOW 3, INFO 4. Rows LK-01..LK-08 in KNOWN_ISSUES.md.

## WP-01: load_policy probes in noinline helpers (web_auth_store.c:1086-1131)
Behaviour is unchanged for the return value: the same three probes decide UNREADABLE vs ABSENT.

- **LK-07 INFO.** The old code ran all three probes. The new `a() || b() || c()` short-circuits, so a board with
  an admin record no longer reads the USER record or the LCD blob. None of the probes has a side effect (no latch
  or cache), so this only saves NVS reads. The comment "Non-reentrant-safe: all locals are on the caller's stack"
  says the opposite of what it means: the helpers are reentrant. Reword it.

## RF2-09/RF3-06: set_password policy decision under cred_store_lock (web_auth_store.c:675-740, 1160-1199)
Checked: `cred_store_lock` is a non-recursive atomic flag. No path re-enters it. Inside the lock, set_password
calls `set_policy_locked` (no lock), `load_blob`, and `web_auth_store_load_policy`. load_policy and its probes
(`load_password`, `password_configured`, `load_blob`) take no lock. `storage_reset_resolve()` (which takes the
lock through `web_auth_store_set_policy`) runs only after `cred_store_unlock()`. `legacy_policy_migrate` runs at
boot without the lock. Every early return after the lock unlocks: the epoch mismatch, a failed off-policy write,
and a failed on-policy write. Lock order: cred_store_lock is always the outer lock and NVS's internal mutex the
inner one. No caller takes NVS or another lock and then cred_store_lock. The wipe-epoch check now comes BEFORE
the policy write, so a wipe during the KDF can no longer leave a policy written for a credential that was then
refused. This is correct and it fixes RF2-09.

- **LK-01 LOW.** `security_backend_web_auth.c:254-266` evaluates `web_auth_policy_check_transition` (admin
  password/PIN configured) outside the lock, then calls `web_auth_store_set_policy`. A wipe that runs between the
  check and the write can store an enabled policy with no admin credential. set_policy has no epoch check. The
  result fails closed (locked out, LCD gesture recovers), so it is not an opening. Fix: an epoch-checked set_policy
  variant, or re-check under the lock.
- **LK-02 LOW.** set_policy can now return HAL_BUSY on contention (5 s wait). The security backend maps that to
  SECURITY_ERR_STORAGE, so the client sees a storage error rather than a busy/retry. `storage_reset_resolve()` then
  stays latched until the next admin write or boot, and `legacy_policy_migrate` retries only at the next boot. Both
  fail closed. Fix: map HAL_BUSY to a busy/retry status.
- **LK-08 INFO.** The critical section now covers up to two verified policy writes (plus the marker) and
  load_policy's probe reads, several times the old hold. Waiters poll at one tick with no priority inheritance.
  This is acceptable at a 5 s wait. `lcd_pin_pending_refresh()` reads outside the lock (a benign cache). The
  failure paths skip the refresh, as before.

## RS-02: backup_import_refusal_finish drain (backup_import.c:4561-4575)
Checked: the drain runs only after the status was sent. It is bounded by `content_len <= BACKUP_BODY_MAX` and by
the 30 s `OTA_REFUSAL_DRAIN_CAP_MS`. It yields with `vTaskDelay(1)` (WDT safe). A body that is too large returns
ESP_FAIL, so httpd closes the socket. It runs before any async handoff, so it does not interact with the async
job's request copy. The final RESOURCE_FAILURE path does not drain. Sessions are not touched (the auth check ran
earlier, and the drain neither touches nor extends the session).

- **LK-05 MED.** `ota_http_esp_chunk_buf()` is documented as safe because "every user runs on the single httpd
  task". That is no longer true. `stale_running_matches` (update_install_http.c:345-371) reads partition data into
  the shared 4 KB buffer from the **ota_confirm task** (main_network_http.c:304) under the update claim. A backup
  import refused by `ota_http_check_interlocks` because that claim is held now drains the client body into the same
  buffer on the httpd task at the same time, which corrupts the hash input. The effect is that the stale install
  request is judged not to match (outcome_unknown, or not dropped). This is a one-shot boot window, not a safety
  issue. The OTA routes' own refusal drains already had this race; RS-02 adds an easier trigger. Fix: drain into a
  small local buffer (the data is discarded, so 256-512 B is enough), or give the shared buffer an owner flag.
- **LK-03 LOW.** Each refusal holds the single httpd worker for up to 30 s while a slow client trickles its body.
  Repeated refusals can keep the web UI unresponsive. The route sits behind the web auth gate (an admin write), so
  this needs a logged-in client. This is the same exposure as the OTA refusal drains. Fix (optional): a smaller cap
  for backup bodies, or close instead of draining when the remaining body is large.
- **LK-04 INFO.** `content_len > BACKUP_BODY_MAX` returns ESP_FAIL after the 400 is sent. A Windows client may
  still see 10054 for an over-size body. That is intended and documented in the comment.
- **LK-06 INFO.** The host tests cover the drain on the refusal paths. They do not cover the cross-task buffer
  sharing in LK-05 (it needs a target or a two-thread test).
