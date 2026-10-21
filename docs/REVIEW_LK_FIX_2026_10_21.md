# Review: LK fixes (commit 8f75cc927), 2026-10-21

Scope: LK-05, LK-01, LK-02, LK-07, RS-02 as landed in 8f75cc927 on origin/dev. Read-only review, no code changed.
Totals: HIGH 0, MED 1, LOW 3, INFO 4.

## LF-01 MED: LK-05b is a real race at the worst moment (stage status GET / app_stage_info vs stale_running_matches)
`stage_status_get_handler` (update_http.c) and `app_stage_info` (update_install_http.c, also used by the
install `status_get_handler`) pass the shared 4 KB chunk as `update_stage_get_status` scratch and hash the
staged image through it. They hold only the stage's own claim, not the update claim. `stale_running_matches`
hashes the running partition through the same chunk on the ota_confirm task (under the update claim).
These overlap exactly after an install reboot: the OTA page re-polls status (OC-07) while ota_confirm runs.
Effect: a corrupted running-image hash records outcome_unknown (the install record OF/OG work targets), or a
status GET reports a wrong staged SHA. Not safety-relevant. Severity raised from LOW to MED for that reason.
Fix (preferred): give `stale_running_matches` its own transient buffer (PSRAM `heap_caps_malloc`, freed after
the hash; no internal-RAM cost). Alternative: status readers `ota_http_update_try_begin` and answer busy when held.

## LF-02 LOW: the 2 s drain cap is checked only between reads
`refusal_result` checks elapsed time after `httpd_req_recv` returns. Each recv can block for
`recv_wait_timeout` (3 s, wifi_provision_http.c). Worst hold per unauthenticated connection is about
2 s + 3 s = 5 s, not 2 s. Bounded, and a slow-header client can already hold the worker for a similar or longer
time, so no new class of DoS, but the "hard cap" comment overstates it. Fix: correct the comment, or set
SO_RCVTIMEO on `httpd_req_to_sockfd(req)` to the remaining budget for the drain (and restore it).

## LF-03 LOW: vTaskDelay(1) per 256 B read uses most of the 2 s budget at the top of the range
At CONFIG_FREERTOS_HZ=100, 32 KB / 256 B = 128 reads x 10 ms = 1.28 s of pure delay before network time.
A 25-32 KB unauthenticated body on a weak link can hit the 2 s cap, return ESP_FAIL and close with unread
data (RST), which is the RS-02 symptom again at the upper sizes. 23 KB (the bench case) costs ~0.9 s.
The auth buffer is also 256 B, not the 512 B the commit title implies. Fix: yield once per ~50 ms or every
8 reads instead of every read, and/or use 512 B.

## LF-04 LOW: storage_reset_resolve retry can hold the httpd worker up to 15 s
Called from `web_auth_store_set_password` / `set_lcd_pin` (httpd, after unlock) and from boot evaluation.
Each `web_auth_store_set_policy` attempt waits up to CRED_STORE_LOCK_WAIT_MS (5 s), so 3 retries can block
for 15 s. It yields (vTaskDelay), so no task WDT, and only happens when another writer holds the lock for 5 s,
which is abnormal (critical sections are NVS writes, ms). Fix: a shorter wait per retry (e.g. 3 x 1 s) or
one retry.

## LF-05 INFO: chunked bodies and a missing Content-Length
With no Content-Length (or Transfer-Encoding: chunked, which esp_http_server does not parse as a body)
`content_len` is 0, the drain is skipped and ESP_OK is returned. Leftover bytes are parsed as the next request
and httpd answers 400/closes. Same as before the commit; no new exposure.

## LF-06 INFO: session lookups, keep-alive, close on cap: correct
The drain runs after the refusal decision and status send; it touches no session state and does not extend
any session. A complete drain returns ESP_OK so keep-alive works; cap, timeout or socket error returns
ESP_FAIL and httpd closes the socket (possible RST if bytes remained; accepted trade-off). Bodies above
32 KB still close without a drain (`http_auth_refusal_should_close`). `total > MAX` cannot trigger since
recv is bounded by `remaining_len`, harmless.

## LF-07 INFO: drain buffers are single-task
`s_refusal_drain_buf` (ota_http.c, 512 B) callers: ota_esp/ota_pico post handlers, stage upload,
`backup_import_refusal_finish`; all run on httpd before any async handoff (the async-failure branch does not
drain). `s_auth_refusal_drain_buf` (http_auth_http.c, 256 B) is used only by the pre-handler on httpd. OK.

## LF-08 INFO: set_policy_checked lock and epoch are correct
The check (`load_policy`, `password_configured`, `pin_configured`) and `set_policy_locked` run under one
`cred_store_lock` hold; none of those helpers take the lock (non-recursive lock, no self-deadlock). Wipes take
the same lock with an infinite wait and bump the epoch inside it, so no wipe can land between check and
write; no epoch capture is needed because no KDF runs outside the lock here. UNREADABLE/ABSENT compares as
fully off, same as the old code. HAL_BUSY maps to SECURITY_ERR_BUSY -> 503 + Retry-After: 1 (LK-02),
transition refusal still wins over busy only when the lock was obtained. Correct.
