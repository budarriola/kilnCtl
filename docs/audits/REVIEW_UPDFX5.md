# REVIEW_UPDFX5: 0 HIGH, 3 MED, 2 LOW, 5 INFO -- review of the REVIEW_UPDFX4 fixes

Date: 2026-10-13. Reviewer: Claude Opus 5.5. No board access.

Commits reviewed (origin/dev):

- `427fc586a`: windowed DMA abort sampled after data reads only, settle wait
  re-checks deadline and heat, header rewrite, tests.
- `c234371d5`: explicit DMA floor 2048 B, redundant settle cancel check dropped.
- `741ca7067`: mid-body abort 8704 B, static assert, admission-edge test.
- `36f3ad031`: Fix status table in `REVIEW_UPDFX4.md`.

Goal the review judges against: an unattended GitHub update that is robust and
never takes internal heap `min_free` under the 8192 B owner floor.

Summary. The code changes are small and correct as written. The settle wait
returns its own reason for cancel, deadline and heat. The DMA window is pure,
local per response and pinned at its edges. Every early return still cleans
up. The problems are in the arithmetic the commits rest on:

- The static assert and the admission-edge test compare the admission number
  against a draw measured on a different base. The 1024 B stager scratch is
  counted on one side only (MED-1).
- The two bench runs give the same DMA-to-internal offset, 7788 B. With that
  offset the 2048 B DMA floor binds about 1.1 KB above the 8704 B internal
  abort. Abort/admission consistency was checked for the internal rule only
  (MED-2).
- The Fix status table cites three SHAs that are on no branch, and marks
  bench-pending items as fixed (MED-3).

Negtest: 12 of 12 own mutants caught (11 assertion failures, 1 intended static-assert build failure).

## Answers to the review questions

1. **Healthy fetch on the bench numbers.** The healthy run was v1.0.0-pre.3
   on `c549a144c`: internal low-water 10079 B, DMA low-water 2291 B. No rule
   aborts on those numbers. The internal abort sits at 8704 B, 1375 B lower.
   The DMA floor sits at 2048 B, 243 B lower. The margin is thin, though.
   The same draw from a start 243 B or more lower aborts, and the board idles
   from 29647 B (see MED-2). So "can no longer abort" holds for that one run,
   not for healthy fetches in general.
2. **A starving fetch and the floor.** Not guaranteed. The internal abort is
   one sample per loop pass, taken before each `esp_http_client_read`. It
   fires only when a sample lands in [8192, 8704). Free that falls more than
   512 B between two samples crosses the floor before the abort can see it
   (LOW-1). The DMA rule needs 3 consecutive data reads, or 4 of 8, so it is
   slower still. Only its 256 B hard minimum acts at once, and with the 7788 B
   offset that equals about 8044 B internal, already under the floor.
3. **DMA window reset (reset-one-side check).** Correct. `dma_hist` is a
   local declared inside the `status == 200` branch of the per-hop loop in
   `http_get`. It starts at 0 for every response, every hop and every job.
   Nothing else holds a copy of the history or anything derived from it, so
   there is no second side to reset. A redirect never reads a body and never
   samples. The test for this pin (LOW-5 of UPDFX4) is real: mutant V1
   (`static` history) is CAUGHT.
4. **REVIEW_UPDFX4 findings:**
   - MED-2: partly fixed. The two code recommendations are done: count only
     reads that returned data, and do not reuse the 4096 B constant. The
     bench recommendation (log between-read DMA samples) is not done.
     MED-2 below shows the new floor is still inconsistent with the internal
     abort.
   - LOW-1: fixed (window rule).
   - LOW-2: fixed (cancel, deadline and heat each return their own reason
     during the settle wait).
   - LOW-3: partly fixed. Stale text remains (LOW-2 below).
   - LOW-4: fixed. Mutant V3 is CAUGHT.
   - LOW-5: fixed. Mutant V1 is CAUGHT.
5. **Fix status table.** Not accurate (MED-3).

## MED-1: the abort/admission consistency charges the stager scratch on one side only

`update_fetch_heap.h` states:

> benchupd2: the job started at about 29275 B free (30299 idle minus the
> 1024 B scratch) and the low-water was 8339 B, so the whole download drew
> about 20936 B

So `FETCH_HEAP_MEASURED_DRAW_BYTES` = 20936 B is measured from the level
**after** the scratch is held. Admission also compares the level after the
scratch: `update_fetch_heap_admit()` uses
`FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH` = 28623 B. A fetch admitted at the
threshold therefore bottoms out at:

    28623 - 20936 = 7687 B

That is 1017 B under the 8704 B abort and 505 B under the floor. It is also
the residual that `test_update_fetch_heap.c:17` pins and that header line 100
states.

The new consistency check uses the level before the scratch on the admission
side:

- **The static assert:** `FETCH_HEAP_PRECHECK_MIN` (29647) >=
  `FETCH_HEAP_ABORT_BELOW` + `MEASURED_DRAW` (29640).
- **The test** at `test_update_fetch_heap.c:143`.
- **The fake-board test** "admitted at the threshold dipping to 8711 B".
  Its own comment says "live 28623 B + 1024 B scratch = 29647 B", then
  subtracts a draw that already excludes the scratch.

The header resolves the conflict the wrong way at line 92: "The 1024 B stager
scratch is part of that draw measurement, not extra." Line 76 of the same
header says the opposite. The pre.3 run's 20240 B is measured "from idle
free", so it does include the scratch. Measured on the same base (from idle),
the two runs drew:

| Run | Draw from idle |
|---|---|
| benchupd2 | 30299 - 8339 = 21960 B |
| pre.3 | 30319 - 10079 = 20240 B |

The header calls 20936 B "the worse figure", but that compares figures on
different bases.

**Failure scenario.** The board idles at its observed minimum, 29647 B, so
the live reading after the scratch is 28623 B. It is admitted. The fetch
repeats the benchupd2 draw. Free falls through 8704 B and the read loop
aborts `low_heap_midrun`. This is exactly the self-abort that UPDFX4
MED-1 (re-scoped) claims to have removed. If instead the low point falls
inside a read, the floor is crossed by up to 505 B and nothing sees it.

A correct assert would be

    FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH >= FETCH_HEAP_ABORT_BELOW + MEASURED_DRAW

That is 28623 >= 29640, which is false. No 512 B abort margin is consistent
with the measured draw and an idle-minimum admission. The honest options:

- State the 1017 B gap as a residual, beside the existing 505 B one.
- Take the bench run (REVIEW_UPDFX3 MED-2) and decide from the result.

Recommendation:
- Fix the header line 92 contradiction.
- Rebase the assert and the fake test on the after-scratch level. The assert
  will then fail, which is the point: document the gap rather than assert a
  wrong identity.
- Re-label the Fix status row.

## MED-2: the 2048 B DMA floor binds about 1.1 KB above the internal abort

The two bench runs give the same offset between internal and DMA-capable
free:

| Run | Internal low-water | DMA low-water | Offset |
|---|---|---|---|
| benchupd2 (`BENCH_TEST_LOG.md:3283`) | 8339 B | 551 B | 7788 B |
| pre.3 (`BENCH_TEST_LOG.md:3297`) | 10079 B | 2291 B | 7788 B |

Two independent low-water marks agreeing to the byte means the
non-DMA-capable part of the internal heap is fixed at about 7788 B. DMA free
is then internal free minus 7788 B. Mapped onto internal free:

| DMA threshold | Internal equivalent | Compared with the 8704 B internal abort |
|---|---|---|
| `FETCH_DMA_FREE_MIN` 2048 B | about 9836 B | 1132 B above |
| `FETCH_DMA_HARD_MIN` 256 B | about 8044 B | under the floor |

So whenever the body phase sits between about 8704 and 9836 B internal for 3
consecutive data reads, or 4 of 8, the DMA rule aborts `low_heap_dma` while
the internal rule would carry on. The static assert and the admission-edge
test model the internal rule only. In the fake the DMA free stays at its
100000 B default while internal free is driven to 8711 B, so the test cannot
see this.

**Failure scenario.** The board idles at 29815 B (seen on the bench,
`test_update_fetch_heap.c:47`). It runs the pre.3 fetch: the same 20240 B
draw from idle and a long low plateau. Internal free bottoms at 9575 B, so
DMA free is about 1787 B, under 2048 B. After 3 reads on the plateau the
download fails `low_heap_dma`. Nothing is wrong with the heap: the internal
rule and the owner floor both have room. The same draw aborts from any start
below 30076 B. At the idle minimum, 29647 B, DMA reaches about 1619 B.

The guard is "derived" from a single run with 243 B of margin. The UPDFX4
recommendation to log between-read DMA samples on the bench is still open.
Without those samples nobody knows whether the low-water is a plateau, which
would abort, or a dip inside a read, which would not.

Recommendation (pick one):
- Derive the DMA floor from the internal abort and the measured offset:
  `FETCH_HEAP_ABORT_BELOW - 7788` is about 916 B.
- Drop the DMA floor in favour of the internal rule and keep only the hard
  minimum.

Also:
- Extend the static assert (or a test) to the DMA rule.
- Make the fake's DMA free follow internal free minus the offset in the
  admission-edge test.

## MED-3: the REVIEW_UPDFX4 Fix status table is inaccurate

- **Phantom SHAs.** It cites `5bda58956`, `12d69356a` and `07e895fa3`. None
  is on any remote branch (`git branch -r --contains` is empty for all three).
  The landed commits are `427fc586a`, `c234371d5` and `741ca7067`. This is
  the same defect UPDFX4 LOW-3 found in the REVIEW_UPDFX3 table.
- **MED-1 row.** It says "a fetch admitted at 29647 B bottoms out near
  8711 B". Admission is on the 28623 B after-scratch reading, and the bottom
  is 7687 B (MED-1). The row also re-scopes MED-1 without saying what became
  of the finding's own recommendations. The REVIEW_UPDFX3 MED-1 row still
  reads "Fixed in code", although UPDFX4 asked for "Accepted residual,
  bench-pending".
- **MED-2 row ("Fixed").** The bench half of the recommendation, logging
  between-read DMA samples, is not done, and the floor is still inconsistent
  with the internal abort (MED-2 above). It should read "Reduced;
  bench-pending".
- **LOW-3 row ("Fixed").** Stale text remains (LOW-2 below).
- **Unverifiable negtest notes.** "N12 MISSED as an equivalent mutant (that
  check was removed)" and "round 3 a (assert, build)" cannot be checked: no
  log or mutation list is cited.

## LOW-1: a 512 B abort margin is smaller than one read slice can move the heap

The internal abort samples once per loop pass. Between two samples:

- one `esp_http_client_read` of up to 4096 B runs (TLS record decrypt,
  socket receive);
- the sink hands the chunk to the writer;
- the WiFi, lwIP, httpd and LVGL tasks allocate on their own schedule.

`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192` sends every generic malloc up to
8 KB to internal RAM first. The header itself records board transients of
about 14.5 KB (idle low-water 16555 B against idle free 31123 B).

With a 2048 B margin a step of up to 2 KB between samples was still caught
above the floor. With 512 B, any step over 512 B from a sample at or just
above 8704 B lands under the floor before the next sample.

**Scenario.** Sample reads 8710 B (continue). A concurrent httpd request takes
about 1 KB during the next read. Free reaches about 7.7 KB, under the floor,
before the next check aborts.

The trade-off is deliberate: a larger margin would abort admitted fetches.
But the header should say the body-phase abort no longer protects the floor
against any single step over 512 B.

## LOW-2: stale or contradictory text left after UPDFX5

- **`update_fetch_heap.h` lines 5-7.** They still describe admission as
  "floor + estimated worst fetch draw + slack". It is now the idle minimum.
- **`update_fetch_heap.h` lines 59-61.** They still say, in the present
  tense, "1792 B above the abort level 10240 B ... The abort level itself
  keeps FLOOR + 2048 B".
- **`update_fetch_heap.h` line 92.** It contradicts line 76 (MED-1).
- **`update_fetch_heap.h` line 137.** It calls `MEASURED_DRAW` "Informational
  pin, not an admission input". It is now an input to the static assert.
- **`update_fetch_heap.h` line 159** says the DMA floor is "DERIVED from bench
  data" and then "NOT derived from FETCH_HEAP_ABORT_BELOW".
  `GITHUB_RELEASE_UPDATE_PLAN.md:361` says "a DMA-capable guard derived from
  it", meaning from the abort. The two documents disagree.
- **`test_update_fetch_heap.c`:**
  - Line 48 still pins "idle-min margin over floor is 3931 B" (the superseded
    17524 B estimate).
  - Lines 81-82 and 146-148 still prove "post-draw >= abort" with the 16500 B
    estimate. The measured draw says otherwise, so these checks are vacuous
    reassurance.
  - The comment at line 80 still says ">= floor + 2048".

## INFO

- **INFO-1: `dma_hist` is reset correctly** per response, per hop and per job
  (question 3). It has no paired state elsewhere.
- **INFO-2: the settle-wait deadline check is skipped at the first
  admission.** That is correct: `deadline_tick` is armed at
  `update_fetch.c:901` and is 0 before then, and the wait is bounded at 3 s.
- **INFO-3: dropping the pre-delay cancel check (`c234371d5`) changes nothing.** That check ran only on a `LOW_FREE` verdict, and the post-delay check returns `cancelled` on the same tick.
- **INFO-4: the DMA sample comes after the read and the internal sample
  before it.** A read that fails the DMA rule never reaches the sink, so a
  `low_heap_dma` abort leaves no partial chunk half-written.

- **INFO-5: one low DMA sample in three never aborts.** That pattern peaks at 3 lows in 8 samples, under the 4 the window needs. Strict alternation (4 in 8) does abort. The internal abort covers the slower patterns. This is a limit of the second guard, not a gap.

## Negative test

`tools\negtest.ps1 -Preset kilnfw-host` on base `36f3ad031`, with
`-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.
The baseline passed and the real tree was unchanged. All 12 mutants were
caught. Eleven compiled and failed an assertion, with no build failures.
V10 was meant to break the static assert. Its only build error is C2338 at
`update_fetch_heap.h:151` ("PRECHECK_MIN >= ABORT_BELOW + measured draw").

| # | Mutant | Verdict | First failing assertion |
|---|--------|---------|-------------------------|
| V1 | `dma_hist` made static, so it carries over between responses | CAUGHT | `test_update_fetch.c:565-566`, `:582`, `:589` (job B aborted by job A's history) |
| V2 | window rule `lows > WINDOW_LOW` | CAUGHT | `test_update_fetch_heap.c:120`, `:135`; `test_update_fetch.c:551` |
| V3 | settle wait also on `LOW_BLOCK` | CAUGHT | `test_update_fetch.c:663` |
| V4 | settle deadline ignored | CAUGHT | `test_update_fetch.c:643-644` |
| V5 | settle heat check removed | CAUGHT | `test_update_fetch.c:652-653` |
| V6 | settle post-delay cancel ignored | CAUGHT | `test_update_fetch.c:618`, `:620`, `:631` |
| V7 | DMA sampled on EAGAIN slices too | CAUGHT | `test_update_fetch.c:565-566` |
| V8 | abort at `< ABORT_BELOW + 8` | CAUGHT | `test_update_fetch_heap.c:149`; `test_update_fetch.c:527` |
| V9 | DMA floor compared with `<=` | CAUGHT | `test_update_fetch_heap.c:104-105`; `test_update_fetch.c:382` |
| V10 | `ABORT_BELOW` = floor + 520 (targets the static assert) | CAUGHT (intended build failure) | C2338 at `update_fetch_heap.h:151` |
| V11 | `WINDOW` 8 -> 16 | CAUGHT | `test_update_fetch_heap.c:95`, `:126` |
| V12 | consecutive-run rule dropped | CAUGHT | `test_update_fetch_heap.c:100` |

The suite pins the code's own behaviour well. It cannot catch MED-1 or
MED-2: those are wrong inputs to the model (pre-scratch admission level,
DMA floor above the internal abort). The code matches the tests; the model
behind the tests is wrong.

## Fix status

Open. No fixes in this commit.
