# REVIEW_UPDFX4: review of the REVIEW_UPDFX3 fixes

Date: 2026-10-13. Reviewer: Claude Opus 5.5. No board access.

Commits reviewed (origin/dev):

- `2cf5ace78`: fix. `update_fetch.c`, `update_fetch_heap.c/.h`, tests,
  `stubs_update_fetch/fake_support.c/.h`.
- `bd33f4793`: test pins the settle total as the literal 3000 ticks.
- `8022f7600`: docs (`REVIEW_UPDFX3.md` Fix status, `GITHUB_RELEASE_UPDATE_PLAN.md`,
  `BENCH_TEST_LOG.md`).

Goal the review judges against: an unattended GitHub update that is robust and
never takes internal heap `min_free` below the 8192 B owner floor.

Summary: 0 HIGH, 2 MED, 5 LOW, 4 INFO. The code does what its commit message
says. The arithmetic is right, every early return cleans up, the settle loop is
bounded and cancellable, and the new DMA rule is pinned at its boundaries. Both
MED findings are about the Fix status claims. MED-1 and MED-3 of REVIEW_UPDFX3
are marked fixed, but each is an accepted residual that still needs the bench.
Negtest: 10 of 12 mutations CAUGHT. Each CAUGHT mutant compiled and failed an
assertion. No log shows BUILD FAILURES, and the real tree was unchanged. The 2
MISSED mutants are LOW-4 and LOW-5.

## Threshold arithmetic (checked)

| Name | Value | Check |
|---|---|---|
| `FETCH_IDLE_MIN_FREE_BYTES` = `FETCH_HEAP_PRECHECK_MIN` | 29647 | sk04 idle minimum |
| `FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH` | 28623 | 29647 - 1024, correct |
| `FETCH_HOP_DOWNLOAD_HEAP_MIN` | 28623 | equal to the start need, so a hop asks the same as a start |
| `FETCH_HOP_CHECK_HEAP_MIN` | 24100 | 8192 + 12068 + 3840, correct |
| `FETCH_HEAP_ABORT_BELOW` | 10240 | 8192 + 2048 |
| `FETCH_DMA_FREE_MIN` / `_HARD_MIN` / `_LOW_READS` | 4096 / 256 / 3 | see MED-2 |
| settle | 12 x 250 ms | 13 samples over 3000 ms, pinned literally |

At the idle minimum, the measured whole-download draw of 20936 B leaves
28623 - 20936 = 7687 B. That is 505 B under the floor, and the header documents it.

## MED-1: REVIEW_UPDFX3 MED-1 is a documented residual, not a fix, and the hop rule got stricter

- **The fix closes no hazard.** The Fix status row says "Fixed in code". The
  code only chose the highest need an idle board passes. The documented 505 B
  under-floor hazard remains when httpd traffic coincides with the handshake.
- **The header's claim about the floor is false for part of the download.** The
  header says "the floor in the worst case is held by the mid-body abort". That
  abort runs only between body reads. It cannot see a dip during the TLS
  handshake, the hop's `esp_http_client_open`, or inside a single
  `esp_http_client_read`. Those are the phases where the draw peaks.
- **Hop downloads are now harder to admit.** The hop download need rose from
  28532 B (UPDFX3) to 28623 B. The one bench 302 hop reading on record
  (benchupd) is 27855 B, so the settle wait must now recover at least 768 B,
  where 677 B used to be enough. Every GitHub asset download goes through that
  302. If the first client's teardown leaves the heap lower for longer than
  3 s, every download is refused `low_heap_free` after a 3 s stall.
- **No bench evidence yet.** Nothing shows the settle recovers the gap
  (REVIEW_UPDFX3 MED-2 is still open).

Recommendation:
- Mark MED-1 "Accepted residual, bench-pending" rather than "Fixed".
- Reword the header so the mid-body abort is claimed for the body phase only.
- Make the MED-2 bench run (log the `heap settle:` line on the 302 hop) a
  precondition for calling the update path robust.

## MED-2: REVIEW_UPDFX3 MED-3 is reduced, not fixed; the DMA floor is still a fixed constant

REVIEW_UPDFX3 MED-3 recommended deriving the DMA floor from the measured offset
between internal and DMA free. Idle DMA free is about 7.8 KB below internal
free. The landed fix keeps the fixed 4096 B and adds a 3-consecutive-read rule.

**Why a healthy download can still abort.** With a 7.8 KB offset, DMA 4096 B
corresponds to about 11.9 KB internal free. That sits above the 10240 B
internal abort. A healthy body phase can sit there steadily: a start near
29.3 KB minus a draw of 17-21 KB is about 8-12 KB internal. Steady DMA between
256 and 4096 B therefore aborts a download the internal rule would let finish.
The 3-read rule removes only short dips.

**EAGAIN slices count as reads.** The DMA sample is taken on every loop pass,
and an EAGAIN `continue`s the loop. A stalled stream with steady low DMA
therefore aborts after about 3 timeout slices, not after 3 body reads.

**The test scenario is hypothetical.** The benchupd2 551 B figure is a
low-water mark over the whole job, not a between-read sample. The transient
dip test (two reads at 551 B, then recovery) is a guess at the shape, not a
replay.

Recommendation:
- Mark the row "Reduced; bench-pending".
- On the bench run, log the between-read DMA samples during a download.
- If steady body-phase DMA sits under 4096 B, derive the floor from the
  internal abort minus the offset, or count only reads that returned data.

## LOW-1: missed starvation shapes

`update_fetch_heap_dma_abort` resets the run on any sample at or above 4096 B.
An alternating low/ok pattern never aborts. Neither does a sustained shortage
between 256 and 4096 B that recovers on every third sample. The internal-free
abort still runs, so this is a gap in the second guard, not a missing guard.

## LOW-2: settle window does not re-check deadline or heat

`heap_admit` can wait up to 3 s at the start and at every hop. During that
wait it checks only `cancel`. Neither the job deadline nor `heat_run_active()`
is re-checked until the next hop or read-loop pass. A firing that starts during
the settle wait therefore runs for up to 3 s alongside an admitted TLS session
before the read loop sees it.

A cancel is reported only when the final verdict is `LOW_FREE`. A cancel
during the settle wait whose last sample reads `LOW_BLOCK`, or passes the free
check but fails the DMA check, is reported as a heap refusal rather than
`cancelled`. Both effects are bounded and cosmetic.

## LOW-3: stale comments and docs

`update_fetch_heap.h`:
- **UPDFX4 paragraph.** It says the UPDFX1..3 paragraphs are "below"; they are
  above it.
- **Earlier paragraphs.** They still give 29556 B and a 3931 B margin as live
  figures. The paragraph before `FETCH_LARGEST_BLOCK_MIN` says
  "29647 - 17524 = 12123 B, 3931 B above floor", against the 16500 B estimate
  rather than the measured 20936 B.
- **Limits section.** It still says "31123 >= 28672".
- **`FETCH_HEAP_SLACK_BYTES` comment.** It says admitted free minus the worst
  draw "must still leave floor + this margin". The slack is no longer an input
  to the start or download-hop need, only to the check hop.
- **`FETCH_HOP_DOWNLOAD_HEAP_MIN` comment.** It says "floor + draw + concurrent
  slack (UPDFX3)". The value is now the idle minimum minus the scratch.

`REVIEW_UPDFX3.md` Fix status:
- **Commit SHAs.** It cites the pre-rebase SHAs 350829c36 and 969b9e924 and
  promises a "landing note below" that does not exist. The landed commits are
  `2cf5ace78` and `bd33f4793`.
- **LOW-2 row.** It reads "Settle re-samples logged at INFO". That is MED-2's
  logging recommendation. LOW-2 asked for DMA-abort test coverage: stage blank,
  buffers freed, a following download succeeding. The tests in fact now assert
  failed state, an idle stage, zero live clients, and a following download that
  completes. Only `g_fr_heap_live` is not asserted. The row should say so.

`GITHUB_RELEASE_UPDATE_PLAN.md`:
- **28623 B.** It calls 28623 B "the 1024 B scratch hop need". It is the
  download need at both the start and the hop.
- **Mid-body abort.** It still says the abort is "unchanged at 12288 B". It is
  10240 B. This text predates UPDFX4.

## LOW-4: settle on LOW_BLOCK is untested (negtest U4 MISSED)

The mutant made the loop also settle on a `LOW_BLOCK` verdict, and no test
noticed. The same gap was R4 MISSED in REVIEW_UPDFX3. No test drives a
block-starved heap and asserts zero delays.

## LOW-5: per-response DMA run reset is untested (negtest U7 MISSED)

The mutant made `dma_low_run` static, so a count left over from a previous
response or job carries into the next, and no test noticed. With the counter
carried over, a new download can abort after one or two low reads. A test that
ends one download with two low DMA reads and starts the next with one low read
would pin the reset.

## INFO

- **INFO-1: cleanup on every early return is correct.** `esp_http_client_init`
  failure returns `no_memory` with no client allocated. An open failure now
  calls `esp_http_client_cleanup`; negtest U9 confirms the test catches its
  removal. Every other path, including `low_heap_midrun`, `low_heap_dma`,
  `cancelled`, deadline and heat, reaches the shared close and cleanup.
- **INFO-2: the DMA run counter is local per response,** reset on every hop and
  every job.
- **INFO-3: the pure DMA rule is pinned.** `update_fetch_heap_dma_abort` is
  tested at 255/256, 4095/4096, the third consecutive low read, and reset on
  recovery.
- **INFO-4: no test covers a cancel during the settle wait on a hop.** Only the
  start-path cancel is tested. The code path is shared, so this is coverage,
  not a bug.

## Negative test

`tools\negtest.ps1 -Preset kilnfw-host -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES" -Parallel 3`,
base `8022f7600`. The baseline passed. No mutant log contains BUILD FAILURES,
so every CAUGHT is a compiled mutant failing an assertion. `real_tree_unchanged: true`.

| # | Mutation | Verdict | Assertion |
|---|---|---|---|
| U1 | settle cancel return dropped | CAUGHT | `test_update_fetch.c:523` cancel reports cancelled |
| U2 | settle loop ignores cancel | CAUGHT | `:525` loop stops at the cancel |
| U3 | settle delay removed | CAUGHT | `:513` settle re-sample count |
| U4 | settle also on LOW_BLOCK | MISSED | LOW-4 |
| U5 | one extra settle try | CAUGHT | `:513` |
| U6 | single-sample DMA check in read loop | CAUGHT | `:492` transient dip completes |
| U7 | `dma_low_run` static across responses | MISSED | LOW-5 |
| U8 | DMA check dropped | CAUGHT | `:478` DMA drop aborts `low_heap_dma` |
| U9 | open-failure cleanup dropped | CAUGHT | `:292` no client leaked |
| U10 | hop uses the start rule | CAUGHT | `:440` check hop at 25000 B admitted |
| U11 | hop download need + 1 | CAUGHT | `test_update_fetch_heap.c:66` |
| U12 | start need back to 29556 | CAUGHT | `test_update_fetch_heap.c:10` |

## Fix status

Fixed on dev by 5bda58956 (round 1 fixes), 12d69356a (DMA floor 2048 B, admission margin documented) and 07e895fa3
(abort/admission consistency). Negative tests: round 1 N1..N11 CAUGHT, N12 MISSED as an equivalent mutant (that check was
removed); round 2 (12 mutations incl. N13 floor 2452) 12/12 CAUGHT, each by a FAIL assertion line; round 3 (3 mutations)
3/3 CAUGHT (a by the new static assert at build time, b and c by assertion failures).

| Finding | Status | Commit | Negtest |
|---|---|---|---|
| MED-1 (re-scoped: abort/admission consistency) | Fixed: `FETCH_HEAP_ABORT_BELOW` 10240 -> 8704 B (floor + 512); a fetch admitted at 29647 B bottoms out near 8711 B and used to abort every time. Static assert `PRECHECK_MIN >= ABORT_BELOW + MEASURED_DRAW` (29647 >= 29640); new test: admitted at the threshold, dip to 8711 B, completes. | 07e895fa3 | round 3 a (assert, build) / b (test) CAUGHT |
| MED-2 (DMA floor a fixed constant) | Fixed: explicit `FETCH_DMA_FREE_MIN` 2048 B, from the 2291 B healthy bench low-water, windowed sampling after data reads only | 5bda58956, 12d69356a, 07e895fa3 | rounds 1-2 CAUGHT; round 3 c (4096) CAUGHT |
| LOW-1 (missed starvation shapes) | Fixed: window rule (low,low,ok repeating aborts) | 5bda58956 | round 1/2 CAUGHT |
| LOW-2 (settle ignores deadline/heat) | Fixed: settle wait rechecks deadline, cancel and heat | 5bda58956, 12d69356a | round 1/2 CAUGHT |
| LOW-3 (stale comments and docs) | Fixed: header and plan doc updated to 8704 B / 2048 B | 5bda58956, 07e895fa3 | n/a |
| LOW-4 (settle on LOW_BLOCK untested) | Fixed: test added | 5bda58956 | round 1/2 CAUGHT (N8) |
| LOW-5 (per-response DMA run reset untested) | Fixed: test added | 5bda58956 | round 1/2 CAUGHT (N9) |
