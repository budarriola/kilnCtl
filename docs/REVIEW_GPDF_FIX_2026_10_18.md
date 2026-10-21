# REVIEW_GPDF_FIX_2026_10_18 (HIGH 0, MED 0, LOW 4, INFO 3)

Scope: fixes landed on main c7bf7761d (dev af14080c0) for DF-01, DF-02, GP-01, GP-02.
Commits: 2bd0641e2, e5f826b8c, 32cc16df5, af14080c0. Static review, no board access.

## Findings

- **GD-01 LOW**, `update_fetch_heap.h:112-120`, `update_fetch.c:101`. GP-01 puts the extra 1024 B of
  update_fetch_wr stack (internal RAM) into DRAW_MARGIN, which was the run-to-run spread allowance (runs
  already spread 2.2 KB). The model now has no spread margin for that part. Floor math: a run that draws
  MEASURED_DRAW + 1024 still ends at ABORT_BELOW (9216 B), so the 8192 B floor is breached only if a run
  overshoots the measured maximum by more than about 2 KB. No floor breach on the measured runs, but the
  margin is gone until the BENCH-PENDING re-measure. Fix: re-measure on the bench, or add 1024 B to
  MEASURED_DRAW and re-pin the host tests.
- **GD-02 LOW**, `update_fetch_heap.h:191`, `update_fetch.c:682`. FETCH_LARGEST_BLOCK_MIN stays 6144 B while
  the writer stack is now 5120 B plus TCB and heap overhead (about 5.5 KB), so the headroom drops from 1.5x
  to about 1.1x (the comment says 1.2x, which counts the stack only). The block is probed at admission, but
  wr_start allocates later in the job, after the TLS session and request buffers are allocated. A fragmented
  heap fails the xTaskCreatePinnedToCore cleanly (wr_start fail path, no leak), so the result is a failed
  update, not a crash. Fix: raise LARGEST_BLOCK_MIN to about 7168 B after a bench check of the idle largest
  block (9728 B), or create the writer before the TLS session opens.
- **GD-03 LOW**, `test_update_fetch.c:799-841`. No test covers the GP-02 retry on the pre-release channel:
  an oversized list with allow_prerelease=1 must still return too_large and never read `per_page=5` or
  /latest (GP-03 says this is by design). If someone drops `!w->p.allow_prerelease` from the first
  list_overflow test, no test fails, because the retry's own `list_overflow` test has no channel guard.
  Fix: add an allow_prerelease=1 oversized case that requires too_large.
- **GD-04 LOW**, `web_auth_login_http.c:627-690`. DF-01 wipes token_hash on the 503 path only. On the
  success path and the Set-Cookie 500 path, token_hash (the session lookup key) stays on the httpd stack
  and is never wiped, and the 500 path also leaves a live session in the table that no client holds (it
  ages out). The hash is not the cookie, so the risk is low, but SS-04 is applied unevenly. Fix: wipe
  token_hash right after web_auth_table_create_session, and on the 500 path remove the session.
- **GD-05 INFO**. DF-02 is correct: retain_exit and retained now take the same s_read_lock as
  stack_margin_read. The name scan and s_count reads happen outside the lock, which is safe because
  registration happens only at boot. Lock hold time is a few loads and stores.
- **GD-06 INFO**. The new DF-01/DF-02 tests are source scans. They fail if the wipe or lock calls are
  removed or reordered, but they cannot catch a runtime race. That is acceptable for spinlock placement.
  The GP-02 behavioural test can fail: without the retry, the job reads /releases/latest, which has no
  route, so `state==done && error==""` fails. The 32cc16df5 tightening makes it strict.
- **GD-07 INFO**. GP-02 retry path: it shares the job deadline and body cap, any non-too_large retry
  error returns as is (fail closed), and a second too_large still falls through to /latest (GF-02
  behaviour). If the 5-entry page holds only pre-releases, the job still falls back to /latest (FA-01),
  which is acceptable. No leak: MEM_SINK reuses w->body.
