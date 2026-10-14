# REVIEW_UPDFX8 -- fetch heap: backstop text, bench-pending, min-of-pairs rtc_free, every hop recorded

Reviewed 2026-10-13 on origin/dev at 33a116cd2: e0bd3641f (code and tests), 33a116cd2 (REVIEW_UPDFX7 fix
status). UPDFX8 answers REVIEW_UPDFX7 MED-1, MED-2, LOW-1..3 and INFO-1. Static review plus host-test
negative tests. No board access.

**Counts: HIGH 0, MED 1, LOW 3, INFO 4.**

## MED-1: the plan doc still carries the old claims and is not marked bench-pending

REVIEW_UPDFX7 MED-2 asked for the UPDFX7 numbers to be marked bench-pending in
`docs/GITHUB_RELEASE_UPDATE_PLAN.md`. UPDFX8 marked them only in `update_fetch_heap.h`. The fix status in
REVIEW_UPDFX7.md reports MED-2 as done without naming that gap.

The plan has two problems:
- Line 359 still reads as settled. It says "even the worst measured draw admitted at the edge ends at ... 2048 B
  above the 8192 B owner floor", with no bench-pending marker and no mention of n = 4.
- Line 362 still describes MAX_STEP as covering "the rest ... other tasks between two reads". That is MED-1's
  false guarantee in softer words. The plan does not call ABORT_BELOW a sampled backstop, and it still calls the
  hop evidence "the 302-hop `heap settle:` result", which is now the worst over every hop.

Fix (docs only):
- In the plan's WP8 heap bullets, mark the numbers BENCH-PENDING and repeat the gate: at least 5
  `heap.min_free` readings from this build.
- Replace the line 362 step sentence with the header's backstop wording.
- Say "worst over every hop" for the hop fields.
- Amend the MED-2 line in REVIEW_UPDFX7's fix status.

## LOW-1: the pair loop's integration is mostly untested; the mutant that reintroduces LOW-1 survives

The fake scripts only the DMA reading per call (`g_fr_dma_call_seq`). Internal free stays constant, so
`test_rtc_offset_min_of_pairs_updfx8` cannot tell which pair's `f` was used, or whether `f` was read again at
all. Four mutants of the new loop survive:
- `f_not_reread`: `pf[i] = f;`. Every pair then reuses the stale pair-0 internal reading. If an allocation
  between pair 0's two reads persists, every offset is inflated, and the DMA floor drops for the whole job.
  This is exactly the REVIEW_UPDFX7 LOW-1 defect, and it passes every test.
- `rtc_f_from_pair0`: `dma_offset(pf[0], pd[best])`.
- `pairs_two`: FETCH_DMA_OFFSET_PAIRS 3 changed to 2. The test still passes because pair 1 is the clean one.
- `pair_note_dropped`: the extra pairs never reach the low-waters. `min_dma` is not asserted.

The pure helper (`test_pick_pair`) is fine. The gap is only in how `heap_admit()` feeds it.

Fix:
- Add a per-call internal-free script to the fake, like `g_fr_dma_call_seq`.
- Add a case with a persistent allocation that lands between pair 0's reads (f drops on pairs 1-2), and
  assert `rtc_free == 7788`.
- Pin the pair count by leaving only the third pair clean.
- Assert `min_dma` against the lowest scripted reading.

## LOW-2: a hop refused on DMA is reported `"hop":"admitted"`

The hop record is written before the DMA check, from the internal verdict `hv` only. A hop that passes the
internal check but fails `update_fetch_heap_dma_admit` ends the job with `low_heap_dma` while the JSON says
`"hop":"admitted"`. This contradicts the new comment ("the WORST result (a refusal sticks)").

This is pre-existing: the first-hop record had the same position. `hop_dma_one_under` asserts `g_error` but not
the hop JSON.

Fix: record `HOP_REFUSED` on the `low_heap_dma` return of a hop as well, and assert `"hop":"refused"` in the
DMA-hop test.

## LOW-3: two residual over-claims in update_fetch_heap.h

- The ADMISSION row still says "An admitted fetch bottoms out at or above ABORT_BELOW with the margin to
  spare". That holds only for a draw no larger than MODEL_DRAW, which the BENCH-PENDING note just above it says
  is not yet established. Suggested wording: "if the draw is within MODEL_DRAW".
- The DMA section says the min of pairs means "a concurrent allocation between one pair's two reads cannot
  lower the DMA floor". That holds only if at least one of the three pairs is clean. It is a reduction, not a
  bound.
- Pair 0 is also not back-to-back. Its `f` is read before `largest_internal_block()`, a `heap_note` lock cycle,
  the admit decision and the hop-record lock, so pair 0 has the widest window. The `.c` comment says
  "back-to-back pairs" for all three.

The MED-1 text in the header's Limits section is otherwise accurate and honest. It does three things:
- names the census (1512 B on about 10 paths, 8192 B diagnostics, Wi-Fi);
- says the abort cannot prevent a jump under the floor;
- puts the protection on the draw model and the admission margin.

A grep of `docs/`, `firmware/KilnFW/App/drivers` and `tools/PcTools/src` for the old phrases ("cannot be taken
under the floor", "fires before the floor is crossed") finds no other copy outside the REVIEW_UPDFX6/7 audits.
The plan's softer variant is covered by MED-1.

## INFO

- INFO-1, pick direction. The smallest offset is the conservative direction.
  - Every consumer of `rtc_free` computes the internal quantity minus rtc, saturating:
    `update_fetch_heap_dma_need`, `_dma_floor` (sustained-low) and `_dma_hard_min`. A smaller rtc therefore
    gives a higher floor for every one of them.
  - The pick has one caller: the first admission, behind `!rtc_set`. Hops and the read loop use the latched value.
  - Side effects:
    - The pick always selects a deflated outlier. A concurrent free between one pair's reads makes every DMA
      floor of the job stricter by that size, which can cause a spurious `low_heap_dma`. That costs liveness,
      not safety.
    - The first DMA admission now tests `pd[best]`, which can be higher than `pd[0]`. This is not more
      permissive overall: the internal check on `pf[0]` already gates admission, and with rtc derived from the
      same pair, DMA admit reduces to `pf[best] >= need`.
    - `start_free` comes from pair 0 and `start_dma` from the picked pair, so in the JSON `start_free -
      start_dma` can differ from `rtc_free`.
- INFO-2, cost. No internal RAM is added.
  - The two 3-element arrays (24 B) sit on the `update_fetch` task stack, which is 12288 B and allocated in
    PSRAM (`xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)`).
  - There are no new statics, and `job_heap_t` is unchanged.
  - Time: 4 extra `heap_caps_get_free_size` calls and 2 `st_lock` cycles, once per job. That is microseconds.
  - The code is in flash. The 8192 B floor is untouched.
- INFO-3, hop JSON.
  - The fields are unchanged. The worst case is 219 chars with 10-digit fields, against `h[320]` in the PSRAM
    `status_bufs_t`, sent as its own chunk, so the httpd stack-blob rule is satisfied.
  - "A refusal sticks" is correct, but every internal refusal ends the job, so the worst result and the last
    hop's result cannot differ. The `hop_last_result` mutant is therefore equivalent, not a test gap.
  - `hop_free` and `hop_settle_tries` can come from different hops. `mcp_server_update.py:324` prints them as
    one hop's result ("heap settle: free=X after N re-samples"), and the `update_http_client.py:217` docstring
    does not say worst-of-hops.
  - A settle wait cut short by cancel, timeout or heat_run returns before the record, so it is not recorded.
- INFO-4, minor pick-helper gaps.
  - The tie case puts the lower DMA at the later index, so `off <= best_off` (ties go to the later index) survives.
  - No case has `dma > f`, so dropping `sub_sat` in the pick survives. The saturation only matters on a
    concurrent free larger than the RTC heap.

## Negative tests

Run with `tools\negtest.ps1 -Command 'powershell -ExecutionPolicy Bypass -File
firmware\KilnFW\App\test\build_host_tests.ps1 -OutDir {OUT} -Only update_fetch'` and ExpectPattern
`(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES`.
- The baseline passed. The real tree was unchanged and the copies were removed.
- Every CAUGHT is a compiled mutant failing a `FAIL file:line` assertion. None was a build failure.

Result: 5 CAUGHT, 7 MISSED (1 of them equivalent).

| mutation | change | verdict |
|---|---|---|
| dma_admit_pair0 | `dma = pd[best]` to `pd[0]` | CAUGHT (test_update_fetch.c:878) |
| rtc_f_from_pair0 | offset from `pf[0]`, `pd[best]` | MISSED (LOW-1) |
| f_not_reread | `pf[i] = f` | MISSED (LOW-1; reintroduces REVIEW_UPDFX7 LOW-1) |
| pairs_two | FETCH_DMA_OFFSET_PAIRS 2 | MISSED (LOW-1) |
| pair_note_dropped | extra pairs not noted | MISSED (LOW-1) |
| hop_last_result | hop result = last hop's | MISSED (equivalent, INFO-3) |
| hop_free_max | hop_free keeps the max | CAUGHT (test_update_fetch.c:844) |
| hop_tries_min | hop_tries keeps the min | CAUGHT (test_update_fetch.c:842/860) |
| pick_no_tiebreak | tie-break removed | CAUGHT (test_update_fetch_heap.c:231) |
| pick_le_ties_later | `off <= best_off` | MISSED (INFO-4) |
| pick_no_satsub | plain subtraction in pick | MISSED (INFO-4) |
| hop_record_first_only | first hop only again | CAUGHT (test_update_fetch.c:859/860) |

UPDFX8's own eight negtests (listed in REVIEW_UPDFX7's fix status) cover the pick helper and the hop fold. They
do not cover the pair loop's internal reads, which is where LOW-1 sits.
