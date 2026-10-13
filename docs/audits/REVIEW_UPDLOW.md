# Review UPDLOW: update-path LOW fixes (2026-10-13)

Adversarial review of the fixes that close
`UPDATE_PATH_ROBUSTNESS_AUDIT_2026-10-13.md` LOW-1, 2, 4, 5, 7 and 8, as landed on
origin/dev:

- `980c50c59` code and tests (rate_limited/server_error reasons, RAM-only
  Retry-After / X-RateLimit-Reset backoff with 429 answers on the check and
  download routes, timeout labels, 1 s read slicing, `per_page=15`, wedged-writer
  buffer handoff)
- `574616774` cancel tests
- `88d316ddf` audit doc fix status

Scope: these three commits only. Files: `update_fetch.c`, `update_url.c/.h`,
`test_update_fetch.c`, `test_update_url.c`, `stubs_update_fetch/fake_support.c`,
the audit doc. A parallel fixer (UPDFX4) is working on REVIEW_UPDFX3 in the same
files; nothing here depends on that work.

Result: 0 HIGH, 1 MED, 4 LOW, 7 INFO; negtest 14 CAUGHT, 5 MISSED. No use-after-free or double free
found in the wedged handoff.

## MED-1: X-RateLimit-Reset trusted without X-RateLimit-Remaining: 0; every 403 is "rate_limited"

`update_rate_backoff_s()` (`update_url.c`) uses `reset_epoch - now` whenever an
`X-RateLimit-Reset` header was seen, for any of 403, 429 or 5xx, and
`update_fetch.c` labels every 403 `rate_limited`. GitHub sends the
`X-RateLimit-*` headers on every api.github.com response, including 5xx and
403s that are not rate limits (blocked/DMCA repo, abuse 403, "Resource not
accessible"), and `X-RateLimit-Remaining` is never read. Consequences:

- A transient 502/503 from the API shortly after the hourly window opened backs
  off for up to the remaining window (clamped at 3600 s) instead of the intended
  60 s server default.
- A non-rate 403 is reported to the user as `rate_limited` and backs off for the
  window reset (API host) or 900 s (asset/CDN host, which sends no rate headers,
  e.g. an S3 `AccessDenied`). The user is told to wait for a condition that will
  not change.
- The backoff refuses both check and download with 429 and is cleared only by
  expiry or a reboot; changing the repo setting does not clear it, so fixing a
  wrong repo still waits out the backoff.

Bounded: the result is clamped to [5, 3600] s, so it never blocks forever.
Not covered by any test: there is no case for a 5xx carrying a reset header,
nor for a 403 with `X-RateLimit-Remaining` non-zero (negtest table: no mutant
can distinguish it because the field is not parsed).

Fix direction: capture `X-RateLimit-Remaining`; treat a 403 as `rate_limited`
only when it is a 429, carries `Retry-After`, or has `Remaining: 0`; use the
reset value only in that case; label other 403s `forbidden` (or keep
`http_error`) with no backoff or a short one; clear the backoff when the repo
setting changes.

## LOW-1: missed-give wedge path leaks the job buffers permanently

`wr_call()`'s "completion give missing" path sets `wr_wedged` although the
arbiter says the writer already returned. That writer returned with
`abandoned == false`, so it never set `wr_late_exited` and never looks at
`leak_*` again. `fetch_task()` then sees `wr_wedged && !wedged_at_start`,
reads `wr_late_exited == false` and hands `w`, `body` and `scratch` to a writer
that will never free them: about 2 KiB of internal stager scratch plus the PSRAM
job buffers are lost until reboot. Not a regression (before this fix the job
leaked them unconditionally), the window is narrow (the give itself lost), and
the leak is once per boot because `wr_wedged` is never reset. Untested.

Fix direction: in that path the writer is known gone; set `wr_late_exited`
(under `s_wr_mux`) before setting `wr_wedged`, so the job frees its own buffers.

## LOW-2: body stall patience halved; comment claims "same total patience"

Before: a body read got the 5 s client timeout plus `FETCH_EAGAIN_RETRIES` (1)
retry, about 10 s without a byte. After: 1 s slices, `FETCH_STALL_SLICES` (5),
so 5 s. The commit comment says the total patience is unchanged; it is not for
body reads (the header wait is about 5 s in both). 5 s is still reasonable for
GitHub/S3, but on a weak link a download that previously survived a 6-9 s stall
now fails `timeout`. Separately, the header slice loop does not check the
job deadline (bounded by 5 slices, so harmless), and the TLS connect phase is
still a single uncancellable 5 s wait, as the audit already documents.

Fix direction: either correct the comment or set `FETCH_STALL_SLICES` to 10.

## LOW-3: tick-wrap resurrection of an unpolled backoff

`backoff_remaining_s()` computes `(int32_t)(until - now)`. If no one polls it
for more than 2^31 ticks (about 248 days at 100 Hz) after the backoff expired,
the difference wraps positive and a stale backoff reappears for up to 3600 s.
The status GET and both POST routes poll it and clear it on expiry, so in
practice only a board nobody opens the update page on for eight months could
see it. Fix direction: clear on any read and store an absolute 64-bit tick
(`xTaskGetTickCount` plus an overflow count) or `esp_timer_get_time()`.

## LOW-4: test gaps (negtest MISSED M5, M8, M10, M15, M17)

No test pins the 3600 s upper clamp (M5) or the Retry-After clamp at capture
(M17): a huge Retry-After or reset is never fed. No test pins the stall slice
count (M8) or that a body read that makes progress resets the stall counter
(M10): a download trickling one chunk every 4 s would be killed after five
slices in total if M10 regressed. No test feeds a long digit string to
`parse_u64` (M15), so its overflow guard is unpinned. Fix direction: add one
host test each (Retry-After 99999999 and reset now+10^9 expect 3600; a body
that delivers data on every fourth slice for 30 slices completes; a 6-slice
header stall fails `timeout` while 4 slices succeed; a 40-digit Retry-After is
rejected and the default used).

## INFO

- INFO-1, handoff ownership: no double free. Exactly one side frees, decided
  under `s_wr_mux`: the writer sets `wr_late_exited` and takes `leak_*` in the
  same critical section where the job reads `wr_late_exited` and stores
  `leak_*`. A new job never touches `leak_*` or `s_c->wr`, and `wr_wedged` is
  never reset, so a second job cannot start a writer op while the late one is
  out (it fails `writer_wedged`). A late writer returning after a new job
  started frees only the old job's buffers.
- INFO-2, dangling stage pointers after a late WR_BEGIN: `st->gate_ctx` points
  into the freed `w` and `st->scratch` at the freed job scratch. Unreachable:
  the gate is called only from `flush_head` of a GitHub upload begun by a
  fetch job, the abandoned undo runs `abort_owned`, and every new upload goes
  through `upload_begin`, which resets both; `get_status` sets its own scratch.
- INFO-3, backoff race: `s_backoff_valid`/`s_backoff_until` are written by the
  fetch task and read and cleared by httpd without a lock. Worst case a reader
  clears a just-written valid flag or sees the old `until` once; the effect is
  one admitted or refused request. Benign.
- INFO-4, clock: an unsynced clock (`now_epoch() == 0`) uses the default; a
  reset in the past clamps to 5 s; a huge reset or Retry-After clamps to 3600 s;
  `parse_u64` takes digits only (at most 12), so no negative values and no
  overflow; the deadline is stored in ticks, so SNTP jumps after capture do not
  move it.
- INFO-5, internal RAM: two 4-byte DRAM statics. The rate capture lives in the
  PSRAM `work_t`; no new internal heap. No effect on the 8192 B floor.
- INFO-6, cancel slicing and timeouts: slicing keeps the per-job deadline and
  total-time semantics; cancel is now honoured within about 1 s of a stalled
  read. Only the stall patience changed (LOW-2).
- INFO-7, audit doc: section 6 cited pre-rebase SHAs `4d17ed8eb` and
  `288c629bd`; corrected here to the landed `980c50c59` and `574616774`. Its
  LOW-8 row credited "the settle-wait polls cancel" to this fix; that polling
  predates it and the row now says so.

## Negative tests

`tools/negtest.ps1 -Mutations` over `build_host_tests.ps1 -Only "update_url|update_fetch"`,
base 88d316ddf, ExpectPattern `(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES`,
baseline passed, real tree unchanged, copies removed. Every CAUGHT below failed an
assertion (`FAIL test_*.c:<line>`); none was a compile failure.

| Mutation | Verdict | Failing assertion |
|---|---|---|
| M1 label swap rate_limited/server_error | CAUGHT | test_update_fetch.c:655/672/679 |
| M2 check route ignores backoff | CAUGHT | test_update_fetch.c:657 |
| M2b download route ignores backoff | CAUGHT | test_update_fetch.c:658 |
| M3 backoff never recorded | CAUGHT | test_update_fetch.c:656-680 |
| M4 min clamp removed | CAUGHT | test_update_url.c:197 |
| M5 max clamp removed | MISSED | LOW-4 |
| M6 reset beats Retry-After | CAUGHT | test_update_url.c:192 |
| M7 reset used with unsynced clock | CAUGHT | test_update_url.c:196 |
| M8 header stall one extra slice | MISSED | LOW-4 (patience count unpinned) |
| M9 header loop ignores cancel | CAUGHT | test_update_fetch.c:720 |
| M10 body eagain not reset on progress | MISSED | LOW-4 |
| M11 job frees despite live writer | CAUGHT | test_update_fetch.c:755 |
| M12 writer never frees handoff | CAUGHT | test_update_fetch.c:806 |
| M13 per_page back to 5 | CAUGHT | test_update_fetch.c:481 |
| M14 403 not rate-or-server | CAUGHT | test_update_url.c:207, test_update_fetch.c:672 |
| M15 parse_u64 digit cap 12 -> 40 | MISSED | LOW-4 |
| M16 rate default equals server default | CAUGHT | test_update_url.c:187/196/203 |
| M17 Retry-After not clamped at capture | MISSED | LOW-4 (final clamp also unpinned, see M5) |

14 CAUGHT, 5 MISSED.

## Fix status

Fixed in 48010bf4a and d884f482c (rebased onto origin/dev; landed SHAs recorded below once pushed). Targeted host tests (`update_*`, `backup_import`) pass.

| Finding | Status |
|---|---|
| MED-1 | fixed: X-RateLimit-Reset used only when Remaining is 0; a non-rate 403 is an access error; a 5xx does not inherit a reset backoff; a repo-setting change clears the backoff |
| LOW-1 | fixed (`wr_late_exited` on the missed-completion path); no test pins it, open as a test gap |
| LOW-2 | fixed: about 10 s stall patience restored, header loop bounded by the job deadline |
| LOW-3 | fixed: unsigned tick elapsed in the backoff expiry |
| LOW-4 | fixed: M8, M10, M15 CAUGHT; M5 and M17 are equivalent alone (redundant clamps), CAUGHT combined |

Negtest of the fixes (base 48010bf4a, kilnfw-host preset):

| Mutation | Verdict | Failing assertion |
|---|---|---|
| M8 header stall extra slice | CAUGHT | test_update_fetch.c:730 |
| M10 eagain not reset on progress | CAUGHT | test_update_fetch.c:745/746 |
| M15 parse_u64 digit cap 40 | CAUGHT | test_update_url |
| 403 any = limited | CAUGHT | test_update_url / test_update_fetch |
| 5xx inherits reset | CAUGHT | test_update_fetch |
| settings change does not clear backoff | CAUGHT | test_update_http_refusals |
| tick wrap signed compare | CAUGHT | test_update_fetch |
| M5 alone / M17 alone | MISSED (equivalent: the other clamp covers it) | |
| M5 + M17 combined | CAUGHT | test_update_url.c:216/223 |
| reset ignores Remaining | CAUGHT after added test | test_update_url.c:213 |
