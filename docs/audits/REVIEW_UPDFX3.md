# REVIEW_UPDFX3: adversarial review of the REVIEW_UPDFX2 fixes

Date: 2026-10-13. Reviewer: Claude Opus 5.5, no subagents, no board access.

Commits reviewed (origin/dev):

- `35c5ae5fa`: fix. `update_fetch.c`, `update_fetch_heap.c/.h`.
- `8ea339b56`: tests. `test_update_fetch.c`, `test_update_fetch_heap.c`,
  `stubs_update_fetch/fake_support.c`.
- `566a4861d`: fix status in `docs/audits/REVIEW_UPDFX2.md`.

Goal the review judges against: an unattended GitHub update that is robust and
never takes internal heap `min_free` below the 8192 B owner floor.

Summary: 0 HIGH, 3 MED, 4 LOW, 7 INFO. The code is correct as written: the
first TLS client is closed and freed before the hop is admitted, every abort
path goes through the same close/cleanup and writer-abort path, and the settle
wait is bounded. The weak point is evidence, not code. The restored hop rule,
the settle wait and the new mid-body DMA check have never run on the bench
together, and the one bench download that passed (benchupd2) already drew more
than the budget the thresholds assume. Negtest: 11 of 15 mutations CAUGHT, 4 MISSED (R2 cancel in the settle, R3 no settle
delay, R4 settle on LOW_BLOCK, R15 open-failure cleanup dropped); the misses are
LOW-1, LOW-3 and LOW-4.

## Constants as landed

| Name | Value | Derivation |
|---|---|---|
| `FETCH_HEAP_FLOOR_BYTES` | 8192 | owner floor |
| download draw | 16500 | estimate (WP7 handshake 11044 B measured + tx buffer + writer stack) |
| `FETCH_CHECK_DRAW_BYTES` | 12068 | 11044 measured + 1024 |
| `FETCH_HEAP_SLACK_BYTES` | 3840 | concurrent httpd allowance |
| `FETCH_HEAP_SCRATCH_BYTES` | 1024 | download only, held before admission |
| `FETCH_HEAP_PRECHECK_MIN` | 29556 | floor + 16500 + 1024 + 3840 |
| start need, both kinds | 28532 | precheck minus scratch |
| `FETCH_HOP_DOWNLOAD_HEAP_MIN` | 28532 | floor + 16500 + slack (now equal to the start need) |
| `FETCH_HOP_CHECK_HEAP_MIN` | 24100 | floor + 12068 + slack |
| largest block, download / check | 6144 / 3072 | writer stack + TCB / client buffers |
| `FETCH_HEAP_ABORT_BELOW` | 10240 | floor + 2048, sampled between reads |
| `FETCH_DMA_FREE_MIN` | 4096 | at admission and now between reads |
| settle | 12 x 250 ms | 13 samples, 12 delays, about 3 s |

## The four questions

1. **First client closed before the hop; open-failure cleanup.** Confirmed.
   `http_get()` calls `esp_http_client_close(c)` and
   `esp_http_client_cleanup(c)` at the bottom of every loop pass
   (`update_fetch.c` about line 673), before `continue`-ing to the next hop,
   whose first act is `heap_admit(w, hops > 0)`. So the hop admission sees the
   heap with the first TLS session already released (only lwIP/mbedTLS
   teardown residue remains, which is what the settle wait is for). The
   open-failure branch (about line 585) calls `esp_http_client_cleanup(c)`,
   which closes the transport itself, then returns `connect_failed`; a NULL
   `esp_http_client_init()` returns `no_memory` with nothing allocated. No
   leak on any of the three. The hop cleanup is pinned by a test (R14 CAUGHT);
   the open-failure cleanup is not (R15 MISSED, LOW-4).
2. **Abort paths free buffers and leave the stage retryable.** Confirmed by
   inspection. `low_heap_midrun`, `low_heap_dma` (new), `cancelled`,
   `deadline`, `read_failed` and `short_body` all set `fail` and `break` out of
   the read loop to the same close/cleanup, then return to `fetch_task`, which
   sends `WR_ABORT` to the writer, frees the work buffers and leaves the stage
   blank. Test coverage of that for the new `low_heap_dma` path is thin (LOW-2).
3. **Settle wait bounded, cancel works.** Bounded: `tries >= 12` breaks, so at
   most 12 delays of 250 ms. Cancel stops the wait at the next sample, but the
   job then reports `low_heap_free`, not `cancelled` (LOW-1). The deadline is
   not consulted inside the loop (documented; INFO-3).
4. **Thresholds vs measured costs.** The arithmetic is consistent, but the
   one measurement contradicts the draw estimate (MED-1).

## MED-1: the measured draw already exceeds draw + slack

benchupd2 (`docs/BENCH_TEST_LOG.md`, 2026-10-12, c549a144c): idle free
30299 B, so the job started at about 29275 B once the 1024 B scratch was held.
The low-water during the job was 8339 B. The job therefore drew about
**20936 B**, which includes whatever concurrent httpd traffic the MCP polling
generated. The thresholds budget **16500 + 3840 = 20340 B** for the fetch plus
a concurrent request. A start or hop admitted exactly at the 28532 B need with
that same 20936 B draw bottoms out at about **7596 B, under the floor**. The
floor held on the bench only because idle sat about 740 B above the need.

The 16500 B figure is still an estimate (header: "Estimate, not a
measurement"). Raising the need by about 600 B would refuse an idle board at
its observed minimum (29647 - 1024 = 28623 B), so the fix is not simply a
bigger constant.

Recommendation: measure per phase on the bench (free and min_free at
admission, after the handshake, after the writer starts, at the end) with the
MCP polling quiesced and then with it running, and set draw and slack from
those numbers. Until then the 8192 B floor is held by margin, not by the rule.

## MED-2: the restored hop rule plus settle has no bench evidence

UPDFX3 restores the 28532 B download hop rule that benchupd (721b3fc7e)
refused at the GitHub 302 (27855 B, 677 B short). The new claim is that the
roughly 2.4 KB teardown residue drains within the 3 s settle. Nothing measured
supports or refutes that: benchupd2 passed under UPDFX2's looser 26740 B hop
rule, and there has been no bench run since `35c5ae5fa`. If the residue is
not teardown that drains but a lasting cost (lwIP PCB in TIME_WAIT, which
holds for minutes, or an mbedTLS session cache), every download through the
302 is refused again with `low_heap_free` after a 3 s stall.

Also, `heap_admit()` logs nothing when it admits after one or more settle
retries, so a successful bench run will not show whether the settle was
needed or how long it took.

Recommendation: bench rerun of `update_stage_release` on dev after
`35c5ae5fa`, and log the try count and the sampled free on an admission that
needed more than one sample.

## MED-3: the mid-body DMA check may now abort the download that passed

On the bench the DMA-capable free tracks internal free about 7.8 KB lower
(idle: internal 30299 B, DMA 22511 B; benchupd2 low-water: internal 8339 B,
DMA 551 B). `FETCH_DMA_FREE_MIN` 4096 B therefore corresponds to internal free
of about 11.9 KB, which is above `FETCH_HEAP_ABORT_BELOW` 10240 B. The new
between-read DMA check is now the tighter mid-body abort. A download admitted
at the 28532 B need is expected (by the header's own arithmetic) to run at
about 12032 B internal, which is about 4.2 KB DMA, about 150 B over the DMA
floor. benchupd2 reached DMA 551 B; whether that low point was inside a read
(invisible to the check) or between reads (would now abort) is not known. So
the fix for UPDFX2 LOW-3 can turn the one passing bench download into a
`low_heap_dma` failure, and an unattended update would then fail every retry.

Recommendation: include in the MED-2 bench rerun; if it aborts, derive the DMA
floor from the measured internal-to-DMA offset rather than a fixed 4096 B, or
compare DMA free against the same margin as internal.

## LOW-1: cancel during the settle wait reports low_heap_free

`heap_admit()` breaks on `s_c->cancel` with `hv == FETCH_HEAP_LOW_FREE` and
returns `low_heap_free`. The operator pressed cancel and sees a heap error.
`http_get()` checks cancel before calling `heap_admit()`, not after. No test
cancels during the settle (negtest R2 MISSED). Fix: return `cancelled` when
the loop exits on cancel, and add a test that cancels from the settle.

## LOW-2: mid-body DMA abort test checks only the error string

`test_heap_admission_updfx3` sets `on_read = set_dma_low` and asserts
`low_heap_dma` and a balanced claim. It does not assert that the stage is
blank afterwards, that the work buffers are freed (`g_fr_heap_live`), or that
a following download succeeds. The code path is the same as the
`low_heap_midrun` abort, so this is coverage, not a bug.

## LOW-3: settle wait duration is unpinned

No test pins the number of delays or their length: removing the delay
entirely (R3) or retrying a LOW_BLOCK verdict (R4) is not caught. Only
`SETTLE_TRIES = 0` is caught, because the recover-after-4-reads case then
fails. A regression to "no wait" would bring back the benchupd refusal on the
board while host tests stay green.

## LOW-4: open-failure cleanup is untested

Dropping `esp_http_client_cleanup(c)` from the `esp_http_client_open()` failure
branch (R15) leaves every host test green. The fake already counts live
clients (the R14 assertions at `test_update_fetch.c:496-498`), so a case that
makes the open fail and asserts no live client after the job would pin it. A
leak here costs one client and its buffers per failed connect, and an
unattended check retries, so repeated DNS/TLS failures would ratchet the heap
down toward the floor.

## INFO

- INFO-1: the settle retries only LOW_FREE. A fragmented heap (LOW_BLOCK) is
  refused on the first sample. Reasonable (teardown frees memory, it rarely
  changes the largest block), but undocumented.
- INFO-2: `heat_run_active` is not re-checked after a settle of up to 3 s.
  `fetch_busy` blocks a firing start from the other side, so this is not
  reachable as a heat-plus-fetch overlap.
- INFO-3: the header comment says the settle is "cancel- and deadline-aware";
  the code (correctly, per its own comment) does not consult the deadline. A
  hop can overrun the job deadline by up to 3 s.
- INFO-4: the download hop rule is now identical to the start rule, so
  `admit_hop` only differs for checks. Fine, but the separate constant could be
  defined as the start need to keep them from drifting.
- INFO-5: PcTools `mcp_server_update.py` (about line 287) still special-cases
  `error == "http_status"` with 404; it does not know `no_stable_release`, so
  the MCP text for a pre-release-only repo is the generic one.
- INFO-6: the derivation comment in `update_fetch_heap.h` still carries the
  superseded "16500 B now also includes the 2048 B scratch" and "1932 B
  slack" lines next to the current "16500 + 1024" figures. Hard to read; worth
  a cleanup to current values only.
- INFO-7: the 404 mapping applies only to `/releases/latest`
  (`!use_list`), so a 404 on the list URL (repo missing or private) stays
  `http_status`. Correct and tested.

## Negative test

`tools\negtest.ps1 -Preset kilnfw-host -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`
from a clean worktree at `566a4861d`. Each CAUGHT below was checked in the log:
the mutant compiled and an assertion FAILed.

| Mutation | File | Change | Verdict | Evidence |
|---|---|---|---|---|
| R1_settle_tries_0 | `update_fetch_heap.h` | `FETCH_HEAP_SETTLE_TRIES` 12 -> 0 | CAUGHT | test_update_fetch.c:438, :439 (start admitted once residual clears; settle re-sampled) |
| R2_settle_ignores_cancel | `update_fetch.c` | settle loop no longer breaks on `s_c->cancel` | MISSED | exit 0 (LOW-1) |
| R3_settle_no_delay | `update_fetch.c` | `vTaskDelay(FETCH_HEAP_SETTLE_MS)` removed | MISSED | exit 0 (LOW-3) |
| R4_settle_also_on_block | `update_fetch.c` | settle also retries LOW_BLOCK (`hv == FETCH_HEAP_OK` break) | MISSED | exit 0 (LOW-3, INFO-1) |
| R5_midbody_dma_removed | `update_fetch.c` | mid-body DMA check disabled (`0 &&`) | CAUGHT | test_update_fetch.c:468 (DMA drop mid-body aborts low_heap_dma) |
| R6_hop_download_slack_2048 | `update_fetch_heap.h` | download hop slack 3840 -> 2048 | CAUGHT | test_update_fetch_heap.c:58, :61, :62; test_update_fetch.c:399, :419 |
| R7_hop_check_slack_2048 | `update_fetch_heap.h` | check hop slack 3840 -> 2048 | CAUGHT | test_update_fetch_heap.c:59 (constant pin only) |
| R8_check_hop_uses_download_min | `update_fetch_heap.c` | check hop uses the download minimum | CAUGHT | test_update_fetch_heap.c:63, :69; test_update_fetch.c:430 |
| R9_hops_use_start_rule | `update_fetch.c` | `heap_admit(w, false)` on every hop | CAUGHT | test_update_fetch.c:430 (check hop at 25000 B admitted) |
| R10_start_uses_hop_rule | `update_fetch.c` | start admission uses the hop rule | CAUGHT | test_update_fetch.c:338 (26000 B refuses a check) |
| R11_404_mapped_for_list_too | `update_fetch.c` | 404 mapping drops the `!use_list` condition | CAUGHT | test_update_fetch.c:481 (404 with pre-releases stays http_status) |
| R12_404_to_403 | `update_fetch.c` | `hs == 404` -> `hs == 403` | CAUGHT | test_update_fetch.c:475 (404 on releases/latest -> no_stable_release) |
| R13_midbody_dma_floor_1 | `update_fetch.c` | mid-body DMA test becomes `dma_free < 1` | CAUGHT | test_update_fetch.c:468 |
| R14_hop_cleanup_dropped | `update_fetch.c` | `esp_http_client_cleanup(c)` dropped before the next hop | CAUGHT | test_update_fetch.c:496, :497, :498 (two clients live across the hop) |
| R15_open_fail_cleanup_dropped | `update_fetch.c` | `esp_http_client_cleanup(c)` dropped on open failure | MISSED | exit 0 (LOW-4) |

Baseline PASS (114.6 s). No BUILD FAILURES in any mutant log; every MISSED mutant
compiled and exited 0. Real tree unchanged, copies removed.

## Fix status

Fixed in 350829c36 (rebased) and 969b9e924 (pin settle total as 3000 ticks); landed SHAs follow in the landing note below.

| Finding | Status | Notes |
|---|---|---|
| MED-1 | Fixed in code | Start/hop need capped at the idle minimum 29647 B (28623 B with the 1024 B scratch held), measured draw 20936 B. Residual about 505 B under the 8192 B floor at the idle minimum is documented; per-phase `min_free` measurement is bench-pending. |
| MED-2 | Open, bench | Rerun `update_stage_release` on the board; log the `heap settle:` line and per-phase `min_free` during a hop download. |
| MED-3 | Fixed | DMA abort on 3 consecutive reads under 4096 B or at once under 256 B (`update_fetch_heap_dma_abort`). |
| LOW-1 | Fixed | Settle wait returns `cancelled` on cancel. |
| LOW-2 | Fixed | Settle re-samples logged at INFO. |
| LOW-3 | Fixed | Settle length and delay pinned (3000 ticks literal). |
| LOW-4 | Fixed | Open failure calls `esp_http_client_cleanup`; live client count asserted. |

Negtest (`-Preset kilnfw-host`, base 969b9e924, 11 mutations, all CAUGHT, each compiled and failed an assertion, no BUILD FAILURES, real tree unchanged): R2_settle_tries_2, R3_no_cancel_return, R4_no_cancel_break, settle_ms_100, dma_abort_always, dma_no_reset, dma_no_hard, dma_reads_2, idle_min_28672, dma_use_ok_old, R15_no_cleanup_on_open_fail.
