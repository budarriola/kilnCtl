# REVIEW_UPDFX6 (6e237eadd, cd5ef2623): 0 HIGH, 2 MED, 4 LOW, 3 INFO

Adversarial review of the UPDFX6 GitHub-fetch heap model on origin/dev
(`update_fetch_heap.h/.c`, `update_fetch.c`, `test_update_fetch_heap.c`,
`test_update_fetch.c`, docs). Reviewer worktree `C:\wt\rvupdfx6_0lyz76` at
origin/dev. The board was not touched. Every number below comes from the code
or from docs/BENCH_TEST_LOG.md (benchupd2/3/4).

## Top question: can the board still update itself? Yes.

The new DMA admission does not refuse a normal idle board on current dev.

- The 7788 B offset is the free RTC FAST memory heap. The board's sdkconfig
  has `CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP=y`. That region is in the
  INTERNAL heap but is not DMA-capable. So
  `DMA free = internal free - RTC-heap free`. The offset is 7788 B in every
  bench pair, and it is the same for free and for min_free. benchupd4 shows it
  too: 27587 - 19799 = 7788 and 24679 - 16891 = 7788. No pair mixes free with
  min_free:
  - benchupd2 idle 30299 / 22511 is free against free;
  - the three low-water pairs are min_free against min_free.
- Idle DMA free on current dev is 47191 - 7788 = 39403 B. With the scratch
  held it is 38379 B, or 39403 B if the scratch landed in RTC RAM. The DMA
  admission is 25436 B, so the margin is about 12.9 KB. That is the same margin
  the internal rule leaves: 47191 - 34248 = 12943 B.
- The task brief quotes an earlier "idle" DMA of 6767 B. That figure is
  benchupd3's DMA **min_free**, not its free. On that older firmware, idle DMA
  free was about 30427 - 7788 = 22639 B. That board is refused anyway, by the
  internal rule: 30427 < 34248.
- The internal admission (34248 B before the scratch) admits 47191 B. The
  download hop rule asks for the same 33224 B. A 302 hop reads about 2.4 KB
  under idle (benchupd: 27855 against 30299), which is about 44.8 KB here, so
  the hop is admitted.

Verdict on the reviewed base (36f3ad031 heap profile): not HIGH, with about
12.9 KB to spare. **On current dev the margin drops to about 650 B: see MED-2.** That margin has only been measured at uptime
34-101 s after a fresh boot (INFO-2).

## MED-2: on current dev (d715aeaba, UART ISR in IRAM) an idle board clears admission by only about 650 B, and the redirect hop sits under its need

d715aeaba (bench log "linkcrc", audit LINK_CRC_DURING_FETCH.md) sets
`CONFIG_UART_ISR_IN_IRAM=y` and moves the UART rings into internal RAM, a net
cost of about 11.7 KB internal. The bench figures for that change:

- control build: idle internal about 46.6 KB, fetch min_free 22023 B;
- fix build: fetch min_free 14267 B, largest block low-water 7936 B in the
  TLS handshake.

Those fix-build fetches ran on a **d0e9fdc19-based** build. That build has the
UPDFX5 rule (admission 29647 B), not the UPDFX6 one. **The UPDFX6 rule has
never run on a board with the UART change.**

Estimate for current dev:

| quantity | value | need | margin |
|---|---|---|---|
| idle internal | 46.6 - 11.7 = about 34.9 KB | 34248 B (`FETCH_HEAP_PRECHECK_MIN`) | about +650 B |
| at admission, scratch held | about 33.9 KB | 33224 B | about +650 B |
| DMA at admission (internal - 7788 - scratch) | about 26.1 KB | 25436 B | about +650 B (the same margin by construction, LOW-2) |
| 302 hop, about 1.4 KB under the scratch-held start (benchupd 27855 against 29275) | about 32.5 KB | 33224 B | about -700 B; admitted only if the 3 s settle recovers it |
| bottom of the fetch (fix-build bench) | 14267 B | above the 12288 B abort | +1979 B |

So an idle board on current dev **still passes** both start admissions,
33224 B internal and 25436 B DMA, but with only about 650 B of margin.

The asset download's redirect hop is likely below the 33224 B hop need
(`FETCH_HOP_DOWNLOAD_HEAP_MIN` = the start need). That hop is admitted only if
the lwIP/TLS teardown drains within the 12 x 250 ms settle. If the board loses
more than about 650 B of idle heap, every check and download is refused
`low_heap_free`. That covers longer uptime, an open web session, or the
552 B draw spread in MED-1. It is the HIGH case from the brief, and the
measured numbers sit about one web session away from it.

This is not marked HIGH because no bench run shows a refusal. It is the
highest-priority item in this review.

Needed: one bench fetch on current dev showing
- idle free at the job start;
- the `heap settle:` line at the 302 hop;
- the final state.

If it refuses, lower the hop need to the start need minus the measured hop dip,
or reclaim heap elsewhere.

## MED-1: the model's draw is below the newest measurement, and the header misreads benchupd4

Header (`update_fetch_heap.h`, "Idle boards against this rule") says the
benchupd4 fetch "drew only 2908 B below its prior low-water". That 2908 B is a
**min_free delta** (27587 -> 24679), not a draw. Free stayed at 47191 B through
the run, and the low-water during the fetch was 24679 B. So the fetch drew
**22512 B from idle**, or **21488 B with the scratch held**. That is the same
base as `FETCH_HEAP_MEASURED_DRAW_BYTES` = 20936 B, and it exceeds it by 552 B.

Draw by run:

| run | firmware | idle free | low-water | draw from idle | scratch held |
|---|---|---|---|---|---|
| benchupd2 | c549a144c | 30299 | 8339 | 21960 | 20936 (the model) |
| benchupd3 | c549a144c | 30427 | 10079 | 20348 | 19324 |
| benchupd4 | 36f3ad031 (current dev's parent) | 47191 | 24679 | 22512 | **21488** |

(The header's benchupd3/pre.3 figure is 20240 B. It was taken against a
different free sample, 30319 after the fetch. Either way it is below the
model.)

Scenario. Take an idle board at exactly `FETCH_HEAP_PRECHECK_MIN` = 34248 B
with the benchupd4 draw. It is admitted, then bottoms at
34248 - 22512 = 11736 B. That is 552 B under `FETCH_HEAP_ABORT_BELOW`
(12288 B). If that low lands between reads, the loop aborts
`low_heap_midrun`. The header claim "an admitted fetch bottoms out at or above
ABORT_BELOW, so it neither self-aborts nor reaches the floor" is false for
every idle level in 34248..34799 B. The floor is still held: 11736 > 8192.
The failure is a refused/aborted update in a 552 B band, not a floor breach.
That is why this is MED and not HIGH.

The "draws" are global low-waters. They include concurrent activity during the
run (MCP polling, httpd, LVGL). That is the real operating condition, and the
spread (19.3-21.5 KB with the scratch held) is about 2.2 KB. A single-run
"measured draw" is therefore not a bound.

Fix:
- Take the draw as the maximum of the measured runs (21488 B), or add an
  explicit draw margin.
- Correct the benchupd4 sentence in the header, the plan doc and REVIEW_UPDFX5
  (the trade-off paragraph).
- Add the benchupd4 row to the derivation comment.

## LOW-1: the MAX_STEP justification is wrong, so the static asserts guard nothing

The header and REVIEW_UPDFX5 justify `FETCH_HEAP_MAX_STEP_BYTES` = 4096 B as
"the flash write ... esp_partition_write bounces through an internal buffer of
min(len, 8192) = 4096 B (update_http.c:38 ...)". That is not what the code
does:

- The download path writes the PSRAM `w->chunk` through `wr_call(WR_WRITE)`,
  then `update_stage_upload_write` -> `put` -> `esp_partition_write`.
- ESP-IDF v6.0.2 `esp_flash_write` (`spi_flash/esp_flash_api.c:1114-1142`)
  copies a non-DRAM source through a **32 B stack** `temp_buf[8]`. It makes no
  heap allocation.
- `update_http.c:38`, the line the header cites, says exactly that: "would
  bounce through a 32 B stack temp".

Other candidates per read-loop pass, checked:
- mbedtls: `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`, so `esp_mbedtls_mem_calloc`
  is PSRAM (`mbedtls/port/esp_mem.c:19`). The dynamic rx/tx record buffers go
  to PSRAM.
- SHA-256 (hardware, GDMA): `esp_sha_dma` mallocs internal DMA memory only for
  a partial block, at most 64 B (`sha/core/sha.c:334-342`).
- lwIP/WiFi: `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`, so pbufs prefer PSRAM.
  The static rx buffers are preallocated.
- esp_http_client rx/tx buffers: allocated at init, not per pass.

So the fetch itself makes no 4096 B internal allocation between two samples.
The 4096 B is a margin with no derivation behind it. Two static asserts in
`update_fetch.c` tie it to unrelated sizes:
- `FETCH_CHUNK_LEN <= MAX_STEP`: the chunk is PSRAM and never bounced.
- `FETCH_TX_BUF_BYTES <= MAX_STEP`: allocated once, before the loop.

They would block a harmless chunk-size change and permit a harmful change
elsewhere. Also, the rx `buffer_size = 2048` literal is not asserted at all.

The value itself is not unsafe, because concurrent tasks (httpd, KDF, LVGL)
dominate the between-sample movement. Fix:
- Restate MAX_STEP as an empirical concurrency margin.
- Drop the chunk assert, or keep it with an honest message.
- Correct REVIEW_UPDFX5's "LOW-1: fixed" text.

## LOW-2: the DMA guards cannot bind on this configuration, and they bind wrongly if the RTC usage changes

Because the offset is the RTC FAST heap (above):
`DMA free = internal free - RTC free >= internal free - 7788` whenever the RTC
heap is unused, and DMA is higher still if anything allocates there. Hence:

- DMA admission 25436 = internal admission 33224 - 7788. It can never refuse
  a start the internal rule admitted. The new `low_heap_dma` admission refusal
  is unreachable on real hardware with this sdkconfig. The header says "it
  binds if DMA is scarcer than that". Here DMA cannot be scarcer than that.
- The mid-body DMA floor 4500 B maps to internal 12288 B, which the internal
  abort already enforces at the top of every pass. The DMA rule can only fire
  in the window between the internal sample and the post-read DMA sample.

The converse hazard is that the offset is hard-coded. If a future build puts
less static data in RTC FAST RAM, the RTC heap grows and the offset grows with
it. Say it grows by 500 B, to 8288 B. A board at internal 33224 B (admitted)
then reads DMA 24936 B < 25436 B and is refused `low_heap_dma`, a refusal the
model says cannot happen. The band is small, and today's 12.9 KB margin makes
this latent.

Fix: one of these.
- Derive the DMA numbers at runtime from the RTC heap free
  (`heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - heap_caps_get_free_size(MALLOC_CAP_DMA)`
  sampled at boot).
- Or document that the DMA checks are a cross-check that is redundant by
  construction.

Also state the physical meaning. The owner floor (internal min_free >= 8192 B)
counts about 7.8 KB of RTC FAST RAM, so at the floor the DMA-capable DRAM is
down to about 404 B. That is what `FETCH_DMA_HARD_MIN` = 404 B encodes.

## LOW-3: the hop rule and the measured hop dip leave the start-edge band refused at the 302

`FETCH_HOP_DOWNLOAD_HEAP_MIN` equals the start need, 33224 B. The 302 hop
reads about 2.4 KB under idle (benchupd: 27855 against 30299, scratch held
29275, so a 1.4 KB dip with the scratch already counted). A board admitted at
the start edge, 33224..34643 B with the scratch held, reaches the hop below
33224 B. It then depends on the 3 s settle recovering the teardown drain;
otherwise it is refused `low_heap_free`. This is consistent with the header
("a heap that never recovers is refused"). But the effective idle need for a
download that survives its redirect is about 1.4 KB above
`FETCH_HEAP_PRECHECK_MIN`, and no test or comment states it. It fails safe.
Bench-pending: the `heap settle:` log line on a board flashed with this build.

## LOW-4: the fetch-level tests never pin that a check START uses the start need

Negtest N4 is MISSED. In `heap_admit()` it changes
`hv = hop ? admit_hop : admit` to `(hop || !dl) ? admit_hop : admit`. That
lets a check start at 24356 B instead of 33224 B, and the suite stays green.

- `test_update_fetch.c` refuses a check only at 22910 B, which both rules
  refuse.
- The check-start edge is pinned only in `test_update_fetch_heap.c`, which
  calls `update_fetch_heap_admit()` directly and never reaches the call site.

Fix: add a fetch-level check start at 25000 B (or `FETCH_HOP_CHECK_HEAP_MIN`)
that must fail `low_heap_free`.

## INFO

- INFO-1: the tests pin the constants and the edges correctly. Several static
  asserts are tautologies of their own definitions:
  - `ABORT_BELOW >= FLOOR + MAX_STEP`;
  - `ADMISSION - DRAW >= ABORT_BELOW`;
  - `HOP_CHECK - CHECK_DRAW >= ABORT_BELOW`;
  - `DMA_ADMIT - DRAW >= DMA_FREE_MIN`.

  They cannot fail unless the `#define` shape changes. They document intent;
  they do not check the model against data.
- INFO-2: the 47191 B idle figure was taken at uptime 34-101 s right after a
  flash and a second `debug_reset`. The last long-uptime idle measurement
  (benchupd3, uptime ~1500 s, older firmware) was 30427 B. A long-uptime idle
  reading on current dev is needed before relying on the 12.9 KB margin. If
  dev idles near the old 30 KB after hours, every update is refused
  `low_heap_free` (the internal rule, not DMA). That would be a HIGH, and
  nothing in the tree measures it today.
- INFO-3: a check job holds no scratch but is admitted at the scratch-held
  download need (33224 B on a scratch-free reading). It is effectively 1024 B
  stricter than a download. This is documented as intended ("a check that
  starts is a precursor to a download").

## Negtest

Command: `tools\negtest.ps1 -Preset kilnfw-host -Mutations <json>` with
`-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.
The baseline passed. Each CAUGHT was confirmed from the log: the mutant
compiled and a test assertion failed.

| # | mutation | verdict | evidence |
|---|---|---|---|
| N1 | `update_fetch_heap_dma_admit`: a check start uses the check-hop DMA need | CAUGHT | test_update_fetch_heap.c:94, test_update_fetch.c:383 |
| N2 | `dma_abort`: hard minimum `<` -> `<=` | CAUGHT | test_update_fetch_heap.c:117 |
| N3 | `heap_admit`: skip the DMA admission on a hop | CAUGHT | test_update_fetch.c:516 |
| N4 | `heap_admit`: a check start uses the hop rule (24356 B) | **MISSED** | LOW-4 |
| N5 | read loop: internal mid-body abort removed | CAUGHT | test_update_fetch.c:369, :376 |

4/5 CAUGHT. All four CAUGHT mutants compiled and failed a test assertion (no
BUILD FAILURES). Real tree unchanged. The run was at base cd5ef2623.


## Fix status (UPDFX7)

Landed on dev as 79232daa7, e1dfa978e, e95803d1a and fbb68c838 (model, tests,
status JSON, PcTools, docs). Mid-run SK-04 alarm threshold is unchanged.

Model (`update_fetch_heap.h`):

| name | value | derivation |
|---|---|---|
| `FETCH_HEAP_FLOOR_BYTES` | 8192 | owner floor |
| `FETCH_HEAP_MAX_STEP_BYTES` | 1024 | see LOW-1 |
| `FETCH_HEAP_ABORT_BELOW` | 9216 | 8192 + 1024 |
| `FETCH_HEAP_MEASURED_DRAW_BYTES` | 21488 | largest measured draw |
| `FETCH_HEAP_MODEL_DRAW_BYTES` | 22512 | 21488 + 1024 margin |
| `FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH` | 31728 | 9216 + 22512 |
| `FETCH_HEAP_PRECHECK_MIN` | 32752 | 31728 + 1024 scratch |
| `FETCH_HOP_DOWNLOAD_HEAP_MIN` | 30192 | 31728 - 1536 hop dip |
| `FETCH_HOP_CHECK_HEAP_MIN` | 21284 | 9216 + 12068 check draw |

- MED-1: the model draw is 22512, the largest measured draw (21488) plus a
  stated 1024 margin. An admission at exactly the edge ending with the worst
  measured draw sits at 31728 - 21488 = 10240, 2048 above the floor. Static
  asserts pin this.
- LOW-1: MAX_STEP is no longer 4096 on the strength of a flash-write bounce.
  The write path stages through a stack `temp_buf[8]`
  (`components/spi_flash/esp_flash_api.c:1135-1142`, ESP-IDF v6.0.2) and the
  SHA DMA buffer is at most 64 B (`components/mbedtls/port/sha/core/sha.c:322-335`;
  the PSRAM copies at `:172`/`:185`). The real fetch pass allocates about
  100 B between samples, so 1024 is an empirical concurrency margin for other
  tasks. Start admission on the measured benchupd5 idle of 34783 B is
  32752 (precheck, scratch not yet held), margin +2031 B, which satisfies
  the 2 KB requirement; the predicted hop reading is 34783 - 1024 - 1420 =
  32339, +2147 over the hop need. Older firmware idling at 29647 is refused
  (it would end at 7135 under the floor). The mid-run abort at 9216 keeps
  min_free >= 8192 with 1024 B of step margin.
- MED-2 / LOW-3: the hop rule is the start need less the measured hop dip
  (1536 B), documented at the constants, with the settle wait
  (12 x 250 ms) re-sampling before a refusal.
- LOW-2: the DMA guard is derived: the RTC FAST heap (7788 B on the bench) is
  internal free minus DMA free at the first admission. DMA start need
  31728 - 7788 = 23940, hop need 30192 - 7788 = 22404, abort floor
  9216 - 7788 = 1428, hard minimum 8192 - 7788 = 404. The first-admission DMA
  gate is redundant by construction (it is derived from the same sample); it
  binds at hops and in the read loop, where the tests exercise it.
- LOW-4: `test_check_start_between_needs_updfx7` starts a check with internal
  free between the hop need and the download need; mutant N4 is now CAUGHT.
- `GET /api/update/fetch` carries a `heap` object (start free/DMA, RTC, the
  302-hop `heap settle:` result and tries, internal/DMA/largest-block
  low-waters); the PcTools `update_fetch_status` prints it.

Negtest: kilnfw-host preset, 20 mutations (every threshold constant, each
admit/abort comparison, the hop and DMA paths, N4): baseline passed, 20 of 20
CAUGHT, each confirmed by a compiled mutant failing a test assertion, real
tree unchanged. pytest preset on `test_mcp_server_update_fetch.py`: 4 of 4
CAUGHT. Full `run_all_checks.ps1`: 206 passed, 0 failed, 0 NEW.
