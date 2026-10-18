# Review 2026-10-18: FA-02 / FA-03 / FA-05 fix (0ab3c6bec, docs f6160e4c4, 4d98095a8)

Verdict: no HIGH, no MED. Counts: HIGH 0, MED 0, LOW 1, INFO 3. RW-05 accepted (not reviewed further).
Host tests `session_iface|persist_campaign10` green on origin/dev (16 executables).

## OF-01 LOW: the hi/lo retry loop does not prevent a torn timestamp
`firmware/KilnFW/App/drivers/http/http_session_iface.c` `last_access_load()` / `last_access_store()`.
The writer stores lo then hi; the reader reads hi, lo, hi. If the writer runs between the reader's
first two loads during a hi carry (H -> H+1), the reader gets hi=H, lo=L_new, hi2=H and returns
(H, L_new), about 2^32 ms behind. `session_table_gap_wrapped()` then reports a wrap and
`http_auth_any_ap_session_active()` returns false for that one poll. Swapping the store order only
moves the tear (hi=H+1 with lo=L_old); a single writer needs a sequence counter (seqlock) or a
32-bit epoch to be tear-free. Impact: hi carries once per ~49.7 days of uptime, the window is a
few instructions, and the outcome is one false "no AP session" poll. The AP still stays up while
any station is associated (`ap_teardown_should_defer()` ORs `ap_client_count > 0`), so the owner
rule holds. Same severity as the original FA-03. Fix (optional): seq counter around the pair, or
store a 32-bit ms epoch since the hi word never matters for the 2^31 gap test.

## OF-02 INFO: gap test loads the timestamp twice
`session_table_gap_wrapped()` calls `last_access_load()` twice in one expression; a write between
them can mix two values. Harmless (both are monotonic and the comparison stays sane). Load once.

## OF-03 INFO: atomics are correct on the ESP32-S3
Aligned 32-bit `volatile` objects with `__atomic_load_n/__atomic_store_n` SEQ_CST compile to
plain l32i/s32i with memw on Xtensa LX7, atomic across both cores. The init flag is written only
by httpd (`http_session_table()`); the peek never inits, so the flag/table publish order (table
init before SEQ_CST flag store) is enough. `s_destroy_all_applied_gen` keeps a single writer.

## OF-04 INFO: FA-02 is fail-safe; tests are real
Posted != applied means every session is already revoked, so "no AP session" is the true answer.
Auth-off path (`ap_client_count > 0` only) is untouched, and with auth on the station count is
still ORed in: the auth-off AP, and the auth-on AP, stay while stations are connected. A NEVER
timeout session can no longer pin the AP after a reset gesture. `test_fx01_wifi_read_does_not_apply_destroy_all`
now asserts false (with and without a station), so dropping the FA-02 check fails it. The FA-05
round trip links the real `persist_scratch.c` and forces a PSRAM failure; removing the
`persist_scratch_note_fallback()` call fails the `base + 1` check.
