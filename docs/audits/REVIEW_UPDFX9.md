# REVIEW_UPDFX9 -- fetch heap: DMA-hop refusal recorded, per-call internal script, text fixes

Reviewed 2026-10-13 on origin/dev at 7c5c63988 (UPDFX9), which answers REVIEW_UPDFX8 MED-1, LOW-1..3, INFO-3
and INFO-4. Static review plus host-test and pytest negative tests. No board access.

**Counts: HIGH 0, MED 0, LOW 2, INFO 5.**

## LOW-2 change: correct, reporting only

`heap_admit()` (`update_fetch.c`) now sets `hop_result = HOP_REFUSED` on the `low_heap_dma` return when `hop`
is true. Every refusal path of a hop was walked:

| path in `heap_admit(w, hop=true)` | returns | hop record |
|---|---|---|
| settle wait cut by cancel / deadline / heat run | `cancelled` / `timeout` / `heat_run_active` | not written (not a heap verdict; unchanged, correct) |
| internal verdict LOW_FREE / LOW_BLOCK | `low_heap_free` / `low_heap_block` | REFUSED (unchanged) |
| internal OK, DMA admit fails | `low_heap_dma` | was ADMITTED, now REFUSED (the fix) |
| internal OK, DMA OK | NULL | ADMITTED unless an earlier hop refused |

- A start admission (`hop` false: `run_job` precheck, and the first request of each `http_get` call: API, manifest, asset) never
  touches `hop_result`, so a start DMA refusal still reports `"hop":"none"`. Correct.
- Job behavior is unchanged. `hop_result` is read only by the `GET /api/update/fetch` JSON writer (`update_fetch.c`
  around line 1423); no control decision reads it. The returned error string is the same as before.
- The new block is under `st_lock`, like the other `job_heap_t` writers. No new state, no statics.
- Internal RAM: three statements of code in flash; nothing in `.bss`/`.data`. The fake-heap globals are
  host-test only.

## LOW-1: one comment still contradicts itself and one claim went stale with the pick

- `update_fetch_heap.h:85` still opens "the smallest offset of FETCH_DMA_OFFSET_PAIRS back-to-back (internal, DMA)
  pairs" and two lines later says pair 0 is not back-to-back. The `#define FETCH_DMA_OFFSET_PAIRS` comment
  (`update_fetch_heap.h:200`) also still says "Back-to-back (internal, DMA) pairs".
- The new text (header and `update_fetch.c:336`) says pair 0's window includes "the hop-record lock". The pair loop
  runs only behind `!rtc_set`, which is always the job's first admission, the `run_job` precheck with `hop` false,
  so the hop-record lock is never in that window. The window is: the largest-block read, a `heap_note` lock cycle,
  the verdict and the DMA read. Harmless, but inaccurate.
- `update_fetch_heap.h:96-97` still says "At the FIRST admission the DMA check is redundant with the internal one by
  construction (the offset comes from the same sample)". Since UPDFX8 the offset comes from the picked pair
  (`pf[best], pd[best]`) and the internal verdict from `pf[0]`. When an allocation persists after pair 0,
  `pf[best] < pf[0]` and the DMA check can refuse where the internal one admitted (it reduces to
  `pf[best] >= need`, REVIEW_UPDFX8 INFO-1). Not redundant any more; the sentence over-claims.

Fix (comments only): drop "back-to-back" from line 85 and the `#define` comment ("pairs 1.. back-to-back"), drop
"and the hop-record lock", and change the "redundant by construction" sentence to say the first DMA check
re-judges the picked pair (`pf[best]`) and can refuse when free fell after pair 0.

## LOW-2: REVIEW_UPDFX8's fix status points at negtests that are not there

The LOW-1 fix line ends "Negtests below.", but nothing follows: the UPDFX9 author's negative tests are not
recorded in REVIEW_UPDFX8.md or the commit. This review's own negtests (below) supply the evidence, and they
pass for the LOW-1 mutants, but the fix status should not cite missing evidence.

Fixed in this commit (docs only): the line now points at this review.

## INFO

- INFO-1, residual "302 hop" wording. `_fmt_heap`'s docstring (`mcp_server_update.py:318`) still says "the 302
  hop's settle result", and the test comment at `test_mcp_server_update_fetch.py:164` says the same. The printed
  text and the client docstring are fixed. Change the docstring to "the worst over all redirect hops".
- INFO-2, transient status. On a DMA-refused hop, the record is first written ADMITTED under one `st_lock` and then
  overwritten REFUSED under a second. A status GET between the two sees `"hop":"admitted"` while the job is still
  running. The window is microseconds and the final state is right. Folding the DMA verdict into the one record
  block (read DMA before recording) would remove it; not worth a change on its own.
- INFO-3, refused-hop MCP text untested. No pytest feeds `hop: "refused"`, so the mutant that prints the settle
  details only for `admitted` survives. One extra needle in `test_heap_readings_are_printed` (a refused case)
  closes it.
- INFO-5, two firmware reporting gaps the new tests leave open (mutants survive, no behavior impact):
  - `lowdma_record_start_too`: recording REFUSED on a START DMA refusal as well (the hop guard dropped) passes
    every test. No test refuses a START on DMA at all (every `low_heap_dma` case is a hop or mid-body). A
    start can be DMA-refused: at the first admission when free fell after pair 0 (`pf[best] < need <= pf[0]`), or
    at the first request of a later `http_get` (manifest or asset) with rtc latched. One such
    case asserting `"hop":"none"` pins the `if (hop)` guard of the new block.
  - `start_free_from_best`: `start_free` taken from the picked pair instead of pair 0 passes. Neither pair test
    asserts `start_free`; `test_rtc_offset_persistent_alloc_updfx9` could assert `start_free == BENCH_IDLE_FREE`
    (pair 0) cheaply. Which pair `start_free` should report is a reporting choice (REVIEW_UPDFX8 INFO-1 noted the
    mismatch with `start_dma`); either way it should be pinned.
- INFO-4, fake heap sequencing. `g_fr_int_call_seq` is gated on `g_fr_dma_call_seq_i >= g_fr_int_call_after`,
  so it can only express "internal readings change after the Nth DMA reading". That is exactly the pair-loop
  window it was built for and is realistic (a persistent allocation between pair 0's internal and DMA reads,
  seen by both later reads). Two limits, neither hides a current defect:
  - The scripted branch returns before the `g_fake_heap_recover_reads` countdown, so a test that combined both
    scripts would freeze the recovery count while the internal script runs. No test combines them today.
  - After the script runs out, internal free returns to the static value (the "persistent" allocation vanishes for
    the rest of the job). The tests only assert the admission-time numbers (`rtc_free`, `start_dma`, `min_dma`),
    which the later readings cannot lower, so this does not mask anything.
  - `hop_dma_one_under` sets internal 47191 B with DMA at the hop need less one, an implied RTC heap of about
    24.8 KB, far from the bench 7788 B. It only exercises the DMA branch (rtc is latched at 7788 from the start),
    so the unrealistic offset is harmless, but it is not a bench-shaped reading.

## Negative tests

Firmware: `tools\negtest.ps1 -Command 'powershell -NoProfile -ExecutionPolicy Bypass -File
firmware\KilnFW\App\test\build_host_tests.ps1 -OutDir "{OUT}" -Only update_fetch'` with ExpectPattern
`(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES`. PcTools: `-Preset pytest -PresetArg
tests/test_mcp_server_update_fetch.py`. Baselines passed, the real tree was unchanged, the copies were removed.
Every CAUGHT below was confirmed from its log as a compiled mutant failing an assertion, not a build failure.

Firmware: 8 CAUGHT, 2 MISSED. (A first run was void: the main tree moved mid-run and negtest reported
REAL TREE CHANGED; this is the clean re-run.)

| mutation | change | verdict |
|---|---|---|
| lowdma_hop_unrecorded | DMA-refused hop leaves the record alone | CAUGHT (test_update_fetch.c:413) |
| lowdma_record_admitted | DMA-refused hop recorded ADMITTED | CAUGHT (test_update_fetch.c:413) |
| lowdma_record_start_too | START DMA refusal also recorded REFUSED | MISSED (INFO-5) |
| f_not_reread | pairs 1.. reuse pair 0's internal reading | CAUGHT (test_update_fetch.c:902) |
| rtc_f_from_pair0 | offset from `pf[0]` with `pd[best]` | CAUGHT (test_update_fetch.c:902) |
| pairs_two | `FETCH_DMA_OFFSET_PAIRS` 3 to 2 | CAUGHT (test_update_fetch.c:919, 920) |
| pair_note_dropped | extra pairs not passed to `heap_note` | CAUGHT (test_update_fetch.c:921) |
| pick_le_ties_later | pick `<=`, later tie wins | CAUGHT (test_update_fetch_heap.c:235) |
| pick_no_satsub | plain subtraction, no saturation | CAUGHT (test_update_fetch_heap.c:239) |
| start_free_from_best | `start_free` from the picked pair | MISSED (INFO-5) |

PcTools: 3 CAUGHT, 1 MISSED.

| mutation | change | verdict |
|---|---|---|
| tries_prints_free | "most re-samples" prints `hop_free` | CAUGHT (test_heap_readings_are_printed) |
| lowest_prints_min | "lowest free" prints `min_free` | CAUGHT (test_heap_readings_are_printed) |
| worst_text_dropped | "worst over all hops" phrase removed | CAUGHT (test_heap_readings_are_printed) |
| refused_hop_no_settle | settle details only for `admitted` | MISSED (INFO-3) |

## Fix status (UPDLOWFX)

- LOW-1: fixed (comments only): "back-to-back" dropped from the `update_fetch_heap.h` pair text and the `FETCH_DMA_OFFSET_PAIRS` define; "hop-record lock" dropped from the header and `update_fetch.c`; the "redundant by construction" sentence now says the first DMA check re-judges the picked pair.
- INFO-1: fixed (`_fmt_heap` docstring and the test comment now say "worst settle result over all redirect hops").
- INFO-2: fixed with no new RAM: `heap_admit()` reads the hop's DMA verdict (`hop_dma`, a local) BEFORE the hop record, so the record is written once under one `st_lock`. The post-hoc REFUSED overwrite is gone. Cost: 0 B internal static/heap, 8 B of stack.
- INFO-3: fixed: `test_refused_hop_prints_its_settle_details` (pytest) feeds `hop: "refused"`.
- INFO-5: fixed: `test_start_dma_refusal_leaves_hop_none_updfx9` (a START refused on DMA keeps `"hop":"none"`) and a `start_free == BENCH_IDLE_FREE` assertion in `test_rtc_offset_persistent_alloc_updfx9` (pair 0's reading is what `start_free` reports).
- INFO-4: unchanged (test-fake limits, no defect).

## UPDLOWFX negtest results (2026-10-14)

Every mutation compiled/ran and a test assertion failed (CAUGHT), real tree unchanged.

kilnfw-host (`update_fetch`, `update_http_refusals`): settings always clears backoff (test_update_http_refusals.c:190), repo compare case-sensitive (:192), hop DMA refusal not recorded (test_update_fetch.c:413), START DMA refusal recorded as hop (:926), start_free from picked pair (:903).

pytest: refused-hop settle detail, single-flight marker (added `test_marker_is_held_during_the_call_so_a_concurrent_call_is_refused` after the first run MISSED it), stale failure record, 404 probe, plain status read, post-POST exception wrapper, login wording, RL-D01 4xx regex, RL-D01 raised-unreadable FAIL, `install_non_ok`, dram_bss missing data section, both `is not None` ceilings: all CAUGHT.

check_internal_alloc_allowlist.ps1: `strdup`, `#define` alias to malloc, `(malloc)(n)`, malloc in a `.inc`: all CAUGHT.
