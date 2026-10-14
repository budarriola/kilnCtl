# REVIEW_UPDFX7 -- fetch heap model re-derivation (UPDFX7)

Reviewed 2026-10-13 on origin/dev at 56036c135: 79232daa7, e1dfa978e, e95803d1a (code), fbb68c838,
56036c135 (docs). UPDFX7 answers REVIEW_UPDFX6 MED-1/MED-2/LOW-1..4 and REVIEW_LINKCRC LOW-2/LOW-3.
Static review against ESP-IDF v6.0.2 (`/c/esp/v6.0.2/esp-idf`) and the tree. No board access.

**Counts: HIGH 0, MED 2, LOW 3, INFO 4.**

## MED-1: MAX_STEP 1024 does not bound movement between two abort samples, so the ABORT_BELOW guarantee is false

`update_fetch_heap.h` says ABORT_BELOW (9216 B) is "a sample at or above this cannot be taken under the floor
by the next step, so the mid-body abort fires before the floor is crossed". The header's own LOW-1 text then
calls MAX_STEP "an empirical margin for concurrent tasks ... not a derived bound". The second statement is
right, which makes the first one false. The tree contradicts it directly.

- The fetch's own per-pass internal movement is small. Both cites are correct:
  - `components/spi_flash/esp_flash_api.c:1135-1142`: `uint32_t temp_buf[8]`, a 32 B stack copy.
  - `components/mbedtls/port/sha/core/sha.c:322/:334-335`: `buf_len <= block` check, then
    `heap_caps_malloc(buf_len, DMA|INTERNAL)` only for a non-DMA-capable source.
  - Inaccuracy: `sha.c:172/:185` are not "SPIRAM copies". They are reallocs taken only with flash encryption
    on, and it is off here (see INFO-1).
- Concurrent tasks routinely make single internal allocations larger than 1024 B. `profile_exec_status_t`
  (1512 B) is `heap_caps_malloc(..., MALLOC_CAP_INTERNAL)`'d at each of these sites:
  - every PUSH_CONTEXT broadcast (`safety_build_and_send_context`, `safety_link_frames.c:543`);
  - the home-page refresh (`ui_page_home_refresh.c:137`);
  - `screen_idle.c:195`;
  - `dashboard_exec_http.c:167/351/430`;
  - `ota_http.c:427`;
  - `safety_cfg_http.c:1580/1797`;
  - `uart_bridge_ext_control.c:389`.

  These run on different tasks, so 2-3 can be in flight at once (about 3-4.5 KB). Others are larger:
  `diagnostics_http.c:1911` (CFGFS_FILE_BODY_MAX, 8192 B internal) and the `profile_t` copies in
  `live_profile.c:604` / `profile_executor.c:471`.
- The Wi-Fi blob's dynamic RX buffers (32 configured, TCP_WND 5760) cannot be confirmed as PSRAM from source
  (`esp_adapter.c` prefers SPIRAM for `wifi_malloc`, but the blob's own internal requests are not visible).
  A window's worth of unread segments would be several KB between two samples.

Consequence: the whole-run low-waters in the draw table already contain these transients, so the admission
model itself is unaffected. The mid-body abort is a coarse backstop, not a floor guarantee. A run whose draw
exceeds MODEL_DRAW can take a sample at 9300 B and cross 8192 B before the next one, because a single routine
1512 B allocation is enough.

Compared with UPDFX6, floor headroom for the worst measured run, admitted at the edge, shrank from 3544 B
(33224 - 21488 - 8192) to 2048 B. That rests on n = 4 runs spreading 2.2 KB (19324..21488) plus a 1024 B
margin. The unexplained 258b80d4 dip to 8295 B and the benchupd2 low-water of 8339 B both sat within about
150 B of the floor.

Fix:
- Reword the ABORT_BELOW line to say what it is: a backstop that catches a draw well over the model, with no
  between-sample guarantee.
- Or derive MAX_STEP from the concurrent-allocation census: at least 2 x 1512 B plus heap overhead, about
  3.1 KB. This raises admission by about 2 KB (idle need about 34.8 KB), which removes benchupd5's +2031 B
  margin.

This trade (admission headroom against floor headroom) is the owner's decision. The current code makes it
silently, in a comment.

## MED-2: margin is sized from four runs; no bench evidence yet for the UPDFX7 numbers

- The draw margin (1024 B) is less than half the observed spread (2.2 KB) across only four runs.
- benchupd5 (19644 B) is the only run on current firmware, and it is below the maximum used.
- The header admits that idle transients are about 14.5 KB (low-water 16555 B against free 31123 B), which is
  larger than any margin in the model.
- The status JSON's `heap.min_free` (added here) is the right instrument. The model should not be called sized
  until several runs on this build report it.

Fix: mark the UPDFX7 numbers bench-pending in GITHUB_RELEASE_UPDATE_PLAN.md, and gate any further lowering of
ADMISSION on at least 5 more `heap.min_free` readings from this build. As written, the plan reads as settled.

## LOW-1: rtc_free is fixed from the first admission sample; f and dma are read at different instants

`heap_admit()` reads `free_internal()`, then `largest_internal_block()`, then (on OK) `dma_free_internal()`.
`rtc_free = f - dma` therefore absorbs any DRAM movement between the two reads, and that error is kept for the
whole job:
- A concurrent free between the reads makes rtc_free smaller, which raises the DMA floor (conservative).
- A concurrent allocation between the reads makes rtc_free larger, which lowers the DMA floor (permissive).

The error is bounded by one concurrent allocation (about 1.5 KB, MED-1), so the DMA floor can be anywhere in
about 0..2.9 KB instead of 1428 B. Since the RTC heap has the lowest allocation priority
(`heap/port/esp32s3/memory_layout.c:68`, RTCRAM caps in the third column), its true free size barely moves.

Fix: take the minimum of 2-3 back-to-back (f, dma) pairs, or read dma first and f immediately after it.

## LOW-2: hop_result records only the first hop, with no test for a second hop

`first_hop = hop && hop_result == HOP_NONE` means a second 302 hop's settle result is never reported.
GitHub's chain normally has a single hop. The negtest mutation that records every hop (`hop_result_every_hop`)
is MISSED, because no test has two hops. Either document "first hop only" in the JSON key list in
`update_http_client.py`, or add a two-hop case.

## LOW-3: DMA sustained floor lowered from 4500 B to 1428 B; runtime DMA allocators exist outside the fetch

Not safety-adjacent (see below), but the fetch now runs about 3 KB closer to DMA exhaustion. Runtime
MALLOC_CAP_DMA allocators found while the fetch runs:
- The shared SPI bus is DMA-enabled (`main_boot_early.c:560`, `HAL_SPI_DMA_AUTO`). MAX31856 transfers pass
  `uint8_t` stack arrays (`MAX31856.c:184-189`). `spicommon_dma_setup_priv_buffer` (`spi_common.c`) allocates
  an aligned internal DMA bounce buffer (at most about 20 B) when the address or length is not aligned. On
  failure it returns ESP_ERR_NO_MEM.
- The AES DMA path (`esp_aes_dma_core.c`) can bounce up to 1600 B for a non-GCM suite with an unaligned PSRAM
  record. GCM (software GHASH, per-16 B AES calls) bounces only about 16 B. GitHub negotiates GCM; this is not
  proven.
- The Wi-Fi blob's RX/TX DMA (not verifiable).

The LCD flush uses a preallocated DMA scratch (`panel_spi_bringup.c:268`), and LVGL buffers are PSRAM
(`lvgl_port.c:1245`), so there is no runtime DMA allocation there.

Why this is not HIGH:
- DMA free is internal free minus the about 7.8 KB RTC heap. The DMA floors therefore follow the internal ones,
  and at the owner floor only 404 B of DMA RAM is left by definition.
- A failed MAX31856 transfer is a thermocouple read error, which fails safe (fault, heat off).
- The fetch refuses to start, and aborts every 8 chunks, while a heat run is active (`update_fetch.c:725/845`).

Residual: an idle-board thermocouple read error during a fetch would latch a thermo fault the operator then
has to clear. Recommendation: note this in the header's Limits section. No code change.

## INFO

- INFO-1: The header says the `sha.c:172/:185` copies "go to SPIRAM because the chunk and the context are
  external RAM". They are the flash-encryption realloc paths, inactive here. The conclusion (no per-pass
  internal SHA allocation beyond 64 B) still holds. The PSA SHA driver mallocs its context
  (DMA|INTERNAL, about 100 B) once per `psa_hash_setup`, not per pass.
- INFO-2: The status JSON is fine:
  - `char h[320]` sits in the PSRAM `status_bufs_t` and is sent as its own chunk, which satisfies the httpd
    stack-blob rule;
  - the worst case is about 230 chars with 10-digit fields;
  - the `b` chunk's trailing `}` became `,` so the object closes in `h`, and the tests parse it.
- INFO-3: The `hal_uart_esp.c` asserts now use `KILNLINK_FRAME_STUFFED_MAX` (`CommonFW/include/kilnlink/
  kilnlink_frame.h:40`). The include path is already used by `uart_protocol.c` in the same directory.
  `check_sdkconfig_defaults_applied.ps1`'s `$mustBe` catches both `=n` and `# ... is not set`.
- INFO-4: Many `_Static_assert`/TEST_CHECK lines in the model are arithmetic identities of the `#define`s
  (for example `ABORT_BELOW - FLOOR == MAX_STEP`). The pinned-literal checks (31728, 30192, 21284, 1428, 404)
  carry the real coverage; the identities add none.

## Tests

The new tests are real.
- `test_update_fetch_heap.c` pins every edge one byte either side, and it replays the four bench runs and
  asserts that MEASURED_DRAW equals their maximum.
- The DMA helpers are exercised with offsets 0, 4096 and 100000, so a hard-coded 7788 fails.
- `test_update_fetch.c` drives the job end to end:
  - a hop refused one byte under the derived DMA need;
  - a mid-body dip to exactly 9216 B completing;
  - the heap JSON low-waters, including min_dma 0 instead of UINT32_MAX on a refused start;
  - LOW-4's check start between the hop and start needs.

## Negative tests

All runs used `tools\negtest.ps1` in throwaway copies. Each reported "baseline passed; real tree unchanged".

Host (`-Command 'build_host_tests.ps1 -OutDir {OUT} -Only update_fetch'`, ExpectPattern
`(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES`). Every CAUGHT was confirmed in its log as a compiled
mutant failing a `FAIL file:line` assertion, not a build failure.

Baseline passed. 16/17 CAUGHT, 1 MISSED.

| mutation | change | verdict |
|---|---|---|
| abort_le | `<` to `<=` in the abort test | CAUGHT (test_update_fetch_heap.c:224) |
| max_step_2048 | MAX_STEP 2048 | CAUGHT (heap.c:15 pin) |
| hop_need_is_start | hop need uses start need | CAUGHT (heap.c:92) |
| check_start_by_hop_rule | check start uses hop rule | CAUGHT (heap.c:61) |
| hop_dip_1024 | HOP_DIP 1024 | CAUGHT (heap.c:86) |
| dma_offset_hardcoded | DMA offset hard-coded 7788 | CAUGHT (heap.c:124) |
| dma_floor_hardcoded_rtc | DMA floor ignores rtc_free | CAUGHT (heap.c:137) |
| dma_window_gt | DMA window `>=` to `>` | CAUGHT (heap.c:199) |
| rtc_free_zero | rtc_free latched as 0 | CAUGHT (fetch.c:419) |
| rtc_never_latched | rtc_free never latched | CAUGHT (fetch.c:411) |
| readloop_dma_rtc_zero | read loop passes rtc 0 | CAUGHT (fetch.c:561) |
| readloop_free_not_noted | read loop skips heap_note(free) | CAUGHT (fetch.c:779) |
| low_water_direction | low-water keeps the max | CAUGHT (fetch.c:767) |
| low_water_umax_leak | min_dma prints UINT32_MAX | CAUGHT (fetch.c:826) |
| heap_chunk_dropped | heap JSON chunk not sent | CAUGHT (fetch.c:761) |
| hop_result_every_hop | record every hop's result | MISSED (no two-hop test, LOW-2) |
| hop_tries_zero | hop_settle_tries stays 0 | CAUGHT (fetch.c:799) |

pytest (`-Preset pytest -PresetArg tests/test_mcp_server_update_fetch.py`, ExpectPattern `AssertionError`):

| mutation | verdict |
|---|---|
| unsampled heap printed | CAUGHT |
| hop settle text hidden | CAUGHT |
| low-water dma prints start_dma | CAUGHT |
| heap text not appended | CAUGHT |

check (`-Preset check -PresetArg firmware/KilnFW/App/test/check_sdkconfig_defaults_applied.ps1`):

| mutation | verdict |
|---|---|
| `CONFIG_UART_ISR_IN_IRAM` to `# ... is not set` | CAUGHT (new `$mustBe` message) |
| `CONFIG_UART_ISR_IN_IRAM=n` | CAUGHT (new `$mustBe` message) |

## Fix status (UPDFX8, 2026-10-13)

- MED-1: fixed as text, no numeric change (owner/coordinator decision: MAX_STEP stays 1024 B, because the idle
  margin over the 32752 B precheck is only about 1.9 KB on the bench). The header now calls ABORT_BELOW a sampled
  backstop, not a guarantee, and its Limits section states what does protect the floor (draw model plus admission
  margin) and the residual risk with the review's census (profile_exec_status_t 1512 B on about 10 paths,
  diagnostics_http.c 8192 B, the Wi-Fi blob). No internal RAM added.
- MED-2: the draw margin and MEASURED_DRAW are marked BENCH-PENDING in the header and at the `#define`s: at least
  5 heap.min_free readings from this build before any further lowering. No numeric change.
- LOW-1: fixed. `heap_admit()` reads FETCH_DMA_OFFSET_PAIRS (3) back-to-back (internal, DMA) pairs at the first
  admission and `update_fetch_heap_dma_pick_pair()` keeps the smallest offset (the conservative direction). Host
  tests: `test_pick_pair`, `test_rtc_offset_min_of_pairs_updfx8` (scripted per-call DMA readings in the fake).
- LOW-2: fixed. Every hop folds into the job's hop evidence (worst result, a refusal sticks; lowest hop_free; most
  settle tries). New two-hop tests (admitted/admitted with recover, admitted then refused).
- LOW-3: DMA-floor residual risk (MAX31856 SPI bounce, AES bounce up to 1600 B, Wi-Fi) added to the header Limits.
- INFO-1: the `sha.c:172/:185` text now says flash-encryption-only reallocs, inactive here.
- INFO-4: the `_Static_assert` identities were left in place (not clearly safe to drop).

Negative tests (`tools\negtest.ps1 -Preset kilnfw-host`, all CAUGHT as compiled mutants failing a named assertion):
pick_first_pair (fetch.c:877), pick_max_offset (heap.c:223), pick_tie_higher_dma (heap.c:231), one_pair_only
(fetch.c:877), hop_first_only (fetch.c:859), hop_tries_overwrite (fetch.c:842), hop_free_overwrite (fetch.c:844),
hop_refused_not_recorded (fetch.c:859).
