# Review: UPDFX1 update-fetch heap admission fix (2026-10-12)

Scope: origin/dev 721b3fc7e, commits e535b7cc4, 534dfb748, 721b3fc7e.
Files: `firmware/KilnFW/App/drivers/update/update_fetch.c`, `update_fetch_heap.c/.h`,
`firmware/KilnFW/App/test/test_update_fetch_heap.c`, `test_update_fetch.c`,
`firmware/KilnFW/App/drivers/net/ota_page.html`.
Method: source read against the bench report (main 5098a1316, `update_check` refused `low_heap` at
30311 B free / 10752 B largest block), sdkconfig, `docs/audits/UPDATE_FETCH_HEAP_DRAW_2026-10-09.md`,
and a 10-mutation negtest over `build_host_tests.ps1 -Only update_fetch` (table at the end). No
board access.

Result: 0 HIGH, 1 MED, 6 LOW, 4 INFO.

## Verification of the asked points

1. **Double charge: real, and the download arithmetic is sound.** `fetch_task` allocates the 1024 B
   internal scratch (`update_fetch.c:861`) before `run_job` calls `heap_admit` (`update_fetch.c:730`),
   so the live reading already excludes it while the old threshold `FETCH_HEAP_PRECHECK_MIN` (29556)
   counted it again: 30311 - 1024 = 29287 < 29556. `FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH` = 28532
   (`update_fetch_heap.h:98`) removes exactly that charge; the post-draw floor margin for a download
   is unchanged. The download fix alone admits the bench reading (29287 >= 28532).
   **11044 B is sourced** (`UPDATE_FETCH_HEAP_DRAW_2026-10-09.md:15`, WP7), but as a single api.github.com
   hop with 2 KB rx / 1 KB tx buffers; it was never measured on the manifest or release-asset
   redirect hops. See MED-1. **3072 B block is enough** (INFO-1).
2. **8192 B floor:** holds on paper for a download (unchanged budget) and for every per-hop admission
   (`update_fetch.c:537`, each fresh TLS session). For a check it holds only if the single-hop
   estimate is right (MED-1). The 12288 B mid-body abort (`update_fetch_heap.h:100`) sits above the
   admitted post-draw level (LOW-1).
3. **Claim and state release:** correct. `fetch_task` (`update_fetch.c:855-905`) sets error/state,
   calls `ota_http_update_end()` when claimed, and clears `busy` on every path including `no_memory`
   and the writer-wedge leak path; `run_job` returns early on every refusal without holding locks.
   `test_update_fetch.c:300` (`expect_claim_balanced`) covers the low-free path.
4. **Literal `"low_heap"`:** no caller still depends on it. `ota_page.html:866` keeps the old key
   (harmless; covers an older board) and adds the three new ones (`:867-869`). PcTools
   `cases_ota.py:2194` names it only in a comment; a low-heap failure there falls to INCONCLUSIVE
   either way. No LCD or MCP code matches the string.
5. **Tests:** 3 of 10 mutants CAUGHT (each by a compiled test assertion), 7 MISSED. The pure
   function's need selection and the scratch subtraction are pinned; the one-line kind wiring that is
   the actual UPDFX1 fix, the block-threshold selection, the midrun string, and the per-hop admission
   are not (LOW-5).
6. **DMA-capable heap:** not considered by admission (LOW-6).

## MED

### MED-1: check admission at 24100 B rests on an unmeasured single-hop draw and drops 5.5 KB of protection the bench fix did not need

`update_fetch_heap.h:94-96`, `update_fetch_heap.c:6-7`, `update_fetch.c:200`.

A check is admitted at `FETCH_CHECK_HEAP_MIN` = 8192 + 12068 + 3840 = 24100 B, 5.5 KB below the
29647 B idle minimum. The draw 12068 is the WP7 single api-hop measurement plus 1024 B for the 2 KB tx
buffer. A check is not "releases/latest only" (`update_fetch_heap.h:44`): it fetches the release JSON,
then the manifest, whose download URL redirects to the release-assets host, so it opens up to three
TLS sessions to two hosts with different certificate chains. None of the asset/redirect hops was
measured. The only recorded update run that staged a release (258b80d4,
`GITHUB_RELEASE_UPDATE_PLAN.md:355`) dipped to min_free 8295 B, an unexplained draw of up to about
22.8 KB, some 6 KB over its own budget (`update_fetch_heap.h:66-69`).

Failure scenario: a concurrent httpd request leaves free internal at about 24.2 KB. A check is
admitted. If a hop draws what the 258b80d4 run suggests (12068 + ~6 KB), it ends near 6 KB, below the
owner's 8192 B floor. The 12288 B mid-body abort cannot help: it runs only between body reads, not
during the handshake where the draw happens. Below the floor, other tasks' internal allocations
(httpd, lwIP, Wi-Fi with `TRY_ALLOCATE_WIFI_LWIP=y`, safety-link UART) can fail.

The download-side fix already admits the bench reading (29287 >= 28532), so the lower check
threshold was not needed to clear the reported refusal. Recommend either holding the check to the
download need until the WP8 gate (b) bench run measures min_free across all three check hops, or
measuring the check draw now (log `free_internal()` before and min after each hop) and setting the
check need from that number.

## LOW

### LOW-1: admitted post-draw level sits below the mid-body abort threshold, so a near-threshold job aborts itself

`update_fetch_heap.h:89,95,98,100`. Download: 28532 - 16500 = 12032 B; check: 24100 - 12068 = 12032 B.
Both are 256 B under `FETCH_HEAP_ABORT_BELOW` = 12288 because the slack (3840) is smaller than the
abort's transient room (4096). Pre-existing for downloads (since slack went 4096 -> 3840); UPDFX1
extends it to checks.

Scenario: a check admitted at exactly 24100 B whose handshake draws its estimated 12068 B reads its
first body chunk at about 12.0 KB free and fails `low_heap_midrun` at once, after spending a GitHub
API request. Recommend slack >= 4096 or an abort threshold <= floor + slack, and a static
`TEST_CHECK(need - draw >= FETCH_HEAP_ABORT_BELOW)`.

### LOW-2: stale and inaccurate header comments

- `update_fetch_heap.h:44`: "a CHECK (releases/latest only, memory sink, no writer task, no stager)".
  Wrong: with `allow_prerelease` it reads the list endpoint, and it always fetches the manifest across a
  redirect (see MED-1). A reader sizing the check budget from this comment under-counts hops.
- `update_fetch_heap.h:52-53`: "Margin: 29556 B sits only 91 B below the 29647 B measured idle minimum".
  29556 is no longer an admission threshold for either kind; the live numbers are 28532 / 24100.
- `update_fetch_heap.h:104-105`: enum comments still name `FETCH_HEAP_PRECHECK_MIN` and
  `FETCH_LARGEST_BLOCK_MIN` as the thresholds; they are now per kind.
- `update_fetch.c:68`: "FETCH_HEAP_PRECHECK_MIN 28 KB on current free internal heap" is the old rule.

### LOW-3: a check job allocates the 1024 B internal scratch it never uses, before any admission

`update_fetch.c:861` allocates `FETCH_SCRATCH_LEN` from `MALLOC_CAP_INTERNAL` for every job; only the
stage sink (download) uses it. Scenario: free internal is about 9 KB when a check is requested; the
scratch takes 1 KB (and a fragment of the largest block) before `heap_admit` refuses, briefly
pushing the board toward the 8192 B floor for nothing. It also means the check need of 24100 B is
measured with 1 KB already taken that the budget does not count. Recommend allocating the scratch only
for `KIND_DOWNLOAD` (or after admission, with the download need restored to include it).

### LOW-4: `low_heap_midrun` UI text says "during the download" for a check

`ota_page.html:869`. The mid-body abort (`update_fetch.c:607-608`) runs in the shared read loop, so a
check reading the release JSON can fail with it. Scenario: the user presses Check, sees "ran low on
memory during the download and stopped", and believes a download was attempted and possibly left a
partial stage. Recommend wording that covers both ("while talking to GitHub").

### LOW-5: tests do not pin the kind wiring, block selection, midrun string or per-hop admission

Negtest (table below): M1 (kind inverted), M2 (kind always download), M4 (always `low_heap_free`),
M5 (midrun string reverted to `low_heap`), M6 (block always download minimum), M9 (check block
9728), M10 (per-hop admission removed) all MISSED.

- M1/M2: `update_fetch.c:200` is the line that fixes the bench refusal, and no test drives a check
  through `heap_admit` with a heap between 24100 and 28531. The fake heap defaults
  (`fake_support.c:109-110`, 200000 / 100000) admit everything.
- M6/M9: no test admits a check with a largest block in [3072, 6143]; `test_update_fetch_heap.c:20`
  uses 10752 and `:29` only probes `FETCH_CHECK_BLOCK_MIN - 1`, which both variants refuse.
- M4: no fetch-level test sets a small largest block, so `low_heap_block` is never asserted.
- M5: no test drives the mid-body abort.
- M10: removing the per-hop gate in `http_get` is invisible because `run_job` admits first and the
  fake heap never changes between hops.

Recommend: fetch-level tests with the fake free set to 26000 (check admitted, download refused) and
the block to 4096 (check admitted, download `low_heap_block`); a fake that drops free below 12288
after the first read (expects `low_heap_midrun`); and a fake that lowers free after the first hop
(expects a refusal on the redirect hop).

### LOW-6: admission ignores the DMA-capable heap, and the internal figure includes RTC fast memory

`update_fetch.c:185-193` reads `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`. With
`ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP=y` that total includes RTC fast memory, which is not
DMA-capable, while the fetch's internal pieces (lwIP pbufs, Wi-Fi TX buffers, the 2 KB client
buffers via plain malloc) land in DMA-capable DRAM first. The bench DMA-capable min_free at idle was
6351 B. Scenario: internal free reads 24.5 KB (check admitted), of which several KB are RTC fast
memory; the DMA-capable pool, already near 6 KB at idle, runs out during the handshake and a Wi-Fi
or SPI DMA allocation fails, stalling the network or a thermocouple read rather than the fetch.
Recommend logging `heap_caps_get_free_size(MALLOC_CAP_DMA)` and its min before and after each hop in
the WP8 gate (b) run, and adding a DMA floor to admission if the run shows the fetch draws from it.

## INFO

- **INFO-1: 3072 B check block is sufficient.** `MBEDTLS_SSL_IN_CONTENT_LEN=16384` /
  `OUT_CONTENT_LEN=4096` buffers go to PSRAM (`MBEDTLS_EXTERNAL_MEM_ALLOC`, `DYNAMIC_BUFFER`);
  `SPIRAM_MALLOC_ALWAYSINTERNAL=8192` lets larger mallocs fall back to PSRAM. The largest internal
  pieces a check needs are the 2048 B client rx/tx buffers; a 3072 B block covers one with heap
  overhead. No writer task stack (4096 B) is created for a check (`wr_start` runs only for the stage
  sink), so the download's 6144 B block rule does not apply.
- **INFO-2: redirect hops are gated.** Every `esp_http_client_init` in `http_get` is preceded by
  `heap_admit` (`update_fetch.c:537`), including the asset redirect hop of a download, which is
  admitted at 28532 B before `wr_start`.
- **INFO-3: LOW_FREE is reported ahead of LOW_BLOCK** (`update_fetch_heap.c:8-12`), so a board short on
  both reports `low_heap_free`. Reasonable; the block message ("try after a reboot") only shows when
  free is adequate.
- **INFO-4:** the old `low_heap` key kept in `ota_page.html:866` is useful while older images are in
  the field; drop it once the minimum supported image is past UPDFX1.

## Negtest

`tools\negtest.ps1 -Command 'powershell ... build_host_tests.ps1 -OutDir "{OUT}" -Only "update_fetch"'
-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES" -Parallel 2` at base
721b3fc7e. Baseline passed; real tree unchanged; copies removed. Every CAUGHT row matched a compiled
test assertion (`FAIL ...c:NNN`), none a build failure.

| Mutation | Change | Verdict | Caught by |
|---|---|---|---|
| M1 | `heap_admit`: `kind != KIND_DOWNLOAD` | MISSED | - |
| M2 | `heap_admit`: `is_download` always `true` | MISSED | - |
| M3 | block/free error strings swapped | CAUGHT | `test_update_fetch.c:299` |
| M4 | always `"low_heap_free"` | MISSED | - |
| M5 | midrun string back to `"low_heap"` | MISSED | - |
| M6 | block threshold always `FETCH_LARGEST_BLOCK_MIN` | MISSED | - |
| M7 | need always download need | CAUGHT | `test_update_fetch_heap.c:30` |
| M8 | `AFTER_SCRATCH` without the scratch subtraction | CAUGHT | `test_update_fetch_heap.c:23` |
| M9 | `FETCH_CHECK_BLOCK_MIN` 9728 | MISSED | - |
| M10 | per-hop `heap_admit` in `http_get` removed | MISSED | - |
