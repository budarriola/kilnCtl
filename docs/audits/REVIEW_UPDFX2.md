# REVIEW_UPDFX2: adversarial review of the REVIEW_UPDFX1 fixes

Date: 2026-10-12. Reviewer: Claude Opus 5.5, no subagents, no board access.

Commits reviewed (origin/dev):

- `27b056e99`: code and tests for REVIEW_UPDFX1 MED-1 and LOW-1..6
  (`update_fetch.c`, `update_fetch_heap.c/.h`, `ota_page.html`,
  `test_update_fetch.c`, `test_update_fetch_heap.c`, `fake_support.c`).
- `f71556bcd`: fix status in `docs/audits/REVIEW_UPDFX1.md`.

Summary: 0 HIGH, 1 MED, 5 LOW, 6 INFO. The fix does unblock the bench 302
(27855 B at the redirect hop). It does so by dropping the 3840 B concurrent
slack from the hop rule, so a hop admission's own post-draw budget is 2048 B
over the 8192 B floor, while the start rule keeps 3840 B of slack for the same
draw. Negtest: 11 of 13 mutations CAUGHT by assertion, 2 MISSED.

## Constants as landed

| Name | Value | Origin |
|---|---|---|
| `FETCH_HEAP_FLOOR_BYTES` | 8192 | owner floor |
| scratch | 1024 | download only |
| worst TLS + writer draw | 16500 | derived, not measured |
| check draw | 12068 | 11044 measured (WP7) + 1024 |
| slack | 3840 | concurrent httpd allowance |
| start need, both kinds | 28532 | floor + 16500 + 3840 |
| `FETCH_HEAP_ABORT_BELOW` | 10240 | floor + 2048 |
| `FETCH_HOP_DOWNLOAD_HEAP_MIN` | 26740 | 10240 + 16500 |
| `FETCH_HOP_CHECK_HEAP_MIN` | 22308 | 10240 + 12068 |
| `FETCH_DMA_FREE_MIN` | 4096 | |

Only 11044 B is a measurement (WP7, 2 KB rx / 1 KB tx buffers). 16500 B is
12068 + the 4096 B writer stack + about 336 B; it is an estimate. The
258b80d4 bench run recorded a dip to 8295 B from roughly 31 KB idle, a draw
of about 22.8 KB, which the header's own Limits section records as
unexplained.

## MED-1: hop rule drops the concurrent slack; a full download can reach below 8192 B

`admit_hop` admits a download hop at `free >= 26740`, which is the abort level
plus the draw and nothing else. The start rule budgets 3840 B for a concurrent
httpd request on top of the draw; the hop rule does not. With the design's own
numbers, a hop admitted at 26740 B, followed by the 16500 B draw and one
concurrent 3840 B request, leaves 6400 B, under the floor.

The mid-body abort (`update_fetch_heap_abort`, checked before each read) cannot
prevent this. It runs on the fetch task between reads. It does not run during
the TLS handshake, `sink->begin` (where the 4096 B writer task is created) or
`sink->data`, and it cannot stop another task's allocation. It only notices
afterward.

The asset URL on GitHub always redirects (bench: HTTP 302), so every real
stage passes through the hop rule. This is not a corner case. The rule also
rests on the derived 16500 B figure. If the 22.8 KB dip is a real draw, a hop
admitted at 26740 B ends near 3940 B before any concurrent request.

Recommendation: keep the slack in the hop rule (abort level + draw + slack, or
floor + draw + slack minus a measured redirect residual), and measure the real
residual and draw on the bench (min_free across a staged download with the web
UI polling) before choosing the number. If the 27855 B bench hop must pass,
that is a measured residual of at most 677 B over the start need, which argues
for a measured residual allowance, not for dropping the slack.

## LOW-1: start rule still applies at every http_get's first connection

`run_job` makes three `http_get` calls (release, manifest, asset). `hops` is
local to each call, so each call's first connection uses the full start rule
(28532 B), after the previous call's client cleanup. If the 2.4 KB residual the
fix was built around is anything but instant, the manifest or asset first
connection can still be refused `low_heap_free`, intermittently. Bench attempt 2
(force, confirm_downgrade) passed all three at 28532 B, so the residual is
transient or variable, but no test covers a residual carried across calls.

## LOW-2: hop admission has zero margin over the abort level

A hop admitted at exactly 26740 B ends at 10240 B after the 16500 B draw. One
more byte of draw and the job aborts itself `low_heap_midrun` partway through.
This is the class of UPDFX1 LOW-1 (admit and abort thresholds that meet with no
margin), reintroduced for hops. `test_update_fetch_heap.c` asserts the hop
post-draw level only `>= FETCH_HEAP_ABORT_BELOW`, so it pins zero margin as
acceptable.

## LOW-3: the DMA gate is nominal on this target

On the ESP32-S3, internal DRAM is DMA-capable. `MALLOC_CAP_DMA |
MALLOC_CAP_INTERNAL` free is therefore almost the same pool as internal free,
and once internal free is at least 22308 B (the lowest hop need) a 4096 B DMA
minimum cannot fail in practice. The 6351 B figure the fix cites is a low-water
mark, not a free level at admission. The gate is harmless and correctly
ordered. It is tested only with fake values. Do not count it as protection.

## LOW-4: stale text

- `REVIEW_UPDFX1.md` fix status cites a pre-rebase code commit that is on no
  remote branch, so the citation does not resolve in a fresh clone. The
  landed commit is `27b056e99`.
- The same section uses 17524 B and 10331 B. The code uses 16500 B and a post-draw
  level of 11355 B.
- It says "Start admission is unchanged (28532)". The check start need rose from
  24100 B to 28532 B.
- It says the floor "holds with margin". See MED-1.
- `update_fetch.c` `http_get` still says "Gate each esp_http_client_init on the
  full start threshold instead", above the line that now passes `hops > 0`.
- The comment "Full start/hop admission ..." sits above `dma_free_internal`,
  not above `heap_admit`.
- `update_fetch_heap.h` around line 51 still says the mid-body abort is 12288
  "unchanged". It is 10240. The enum comments near lines 122-123 still name
  `PRECHECK_MIN` and `LARGEST_BLOCK_MIN` for both kinds.

## LOW-5: test gaps

- The hop-low test (on_open free 26000) is refused by both the hop rule and the
  start rule, so it cannot tell them apart. The 27855/7680 hop-ok test can, and
  M1 and M13 were caught there.
- Several assertions use `||` over reason strings, which accepts more than one
  outcome.
- No test asserts that a check job allocates no scratch. M8 (scratch for all
  kinds) was MISSED.
- No test sets low DMA free at a redirect hop. M12 (DMA skipped at hops) was
  MISSED.
- No test carries a residual from one `http_get` into the next call's first
  connection (LOW-1).

## INFO

- INFO-1: the reason strings (`low_heap_free`, `low_heap_block`, `low_heap_dma`,
  `low_heap_midrun`) match between firmware and `ota_page.html` (lines 866-870).
  PcTools and the LCD pass the reason through and need no change.
- INFO-2: the check start need rose to 28532 B. Idle is 29.6-31.1 KB on the bench,
  so a check still fits at idle, but a check right after a download, before
  the heap recovers, may now be refused.
- INFO-3: the release and manifest hops use the download draw of 16500 B, not the
  check draw. That is conservative and correct, because both run inside a
  download job.
- INFO-4: an EAGAIN `continue` loops back through the mid-body check, so the
  check cannot be skipped by a read that returns no data.
- INFO-5: bench attempt 2 on 721b3fc7e reached the asset with the writer created,
  then failed `version_mismatch` in `update_stage.c` `flush_head`. That is a
  separate issue, not in scope here.
- INFO-6: this review lands without `land.ps1 -CheckLog`. The dev flow forbids
  agents from running `run_all_checks.ps1`, and the coordinator batch-runs
  checks on the dev tip.

## Negtest

`tools\negtest.ps1 -Preset kilnfw-host -Parallel 4`, ExpectPattern
`(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES`, base `f71556bcd`.
The baseline passed. Real tree unchanged, copies removed. Every CAUGHT below
matched an assertion `FAIL <file>.c:<line>`. No log contains `BUILD FAILURES`,
so no CAUGHT comes from a compile failure.

| # | Mutation | Verdict | First assertion |
|---|---|---|---|
| M1 | `heap_admit(w, hops > 0)` to `hops > 1` | CAUGHT | test_update_fetch.c:399 "27855 B hop admitted" |
| M2 | hop admits everything (`hops > 0 ? NULL : ...`) | CAUGHT | test_update_fetch.c:391 "low heap at redirect hop refused" |
| M3 | hop download draw 16500 to 17524 | CAUGHT | test_update_fetch_heap.c:58 |
| M4 | abort `<` to `<=` | CAUGHT | test_update_fetch_heap.c:93 "at threshold continues" |
| M5 | abort floor + 2048 to + 4096 | CAUGHT | test_update_fetch_heap.c:58/59/88, test_update_fetch.c:399 |
| M6 | DMA `>=` to `>` | CAUGHT | test_update_fetch_heap.c:80, test_update_fetch.c:380 |
| M7 | DMA caps INTERNAL only | CAUGHT | test_update_fetch.c:372/376 |
| M8 | scratch allocated for all kinds | MISSED | no test asserts a check allocates no scratch |
| M9 | mid-body abort removed | CAUGHT | test_update_fetch.c:365 "mid-body drop aborts low_heap_midrun" |
| M10 | `low_heap_dma` reported as `low_heap_free` | CAUGHT | test_update_fetch.c:372/376 |
| M11 | check start need back to 24100 | CAUGHT | test_update_fetch_heap.c:26/29, test_update_fetch.c:338 |
| M12 | DMA check skipped at hops | MISSED | no DMA-at-hop test |
| M13 | download hop uses the check minimum | CAUGHT | test_update_fetch_heap.c:64, test_update_fetch.c:391 |

M7 is caught only because the fake returns a separate DMA value. On target,
the two pools nearly coincide (LOW-3).

## Fix status (UPDFX3)

Fixed: MED-1 (redirect-hop slack restored to 3840 B), LOW-1..5, settle-wait on
low free before refusing, 404 mapped to `no_stable_release`, and a mid-body
DMA abort (`low_heap_dma`). DMA 551 B investigation: the DMA-capable margin
is checked on its own pool at every hop and mid-body; the fake returns a
separate DMA value so the check is exercised, while on target the pools nearly
coincide.

| Mutation | Result | Evidence |
|---|---|---|
| hop slack reduced | CAUGHT | test_update_fetch_heap.c:58 |
| mid-body DMA abort removed | CAUGHT | test_update_fetch.c:467 |
| settle-wait removed | CAUGHT | test_update_fetch.c:437/438 |
| 404 not mapped to no_stable_release | CAUGHT | test_update_fetch.c:474 |
| M8 scratch allocated for all kinds | CAUGHT | test_update_fetch.c:445 |
| M12 DMA check skipped at hops | CAUGHT | test_update_fetch.c:461 "low DMA at a redirect hop refused" (assertion failure, mutant compiled) |