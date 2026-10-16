# REVIEW_LINKCRC: UART ISR in IRAM + 2048/1024 rings (d715aeaba, docs ac18ba56b): 0 HIGH, 1 MED, 4 LOW, 4 INFO

Reviewed: `CONFIG_UART_ISR_IN_IRAM=y` (`firmware/KilnFW/sdkconfig.defaults`), the
`hal_uart_esp.c` ring cut from 4096/4096 to RX 2048 / TX 1024 with two static asserts and TX
chunking at ring size minus 64, and the new watched key in
`firmware/KilnFW/App/test/check_sdkconfig_defaults_applied.ps1`. Evidence:
`docs/audits/LINK_CRC_DURING_FETCH.md`, the BENCH_TEST_LOG 2026-10-13 linkcrc entry, IDF v6.0.2
`esp_driver_uart/src/uart.c`, and the fix and control linker maps. Board not touched.

## Verdict on the fix

IRAM ISR is the right fix. Flash erase/write turns the cache off on both cores. Only IRAM ISRs
run then, and the UART hardware RX FIFO (128 B) fills in 5.5 ms at 230400 baud. `update_stage`
erases 64 KB blocks (150-400 ms typical, about 2 s datasheet max). Every alternative is worse:

- **Pausing the link around erases** does not help. The Pico still sends unsolicited frames:
  status every 500 ms, DIAG/POWER every 2 s, trip and boot-version repeats every 250 ms, and log
  frames. A non-IRAM ISR loses them whether or not the ESP is sending. Making the Pico go quiet
  would need a protocol change, and an ESP that stops answering for the length of a fetch moves
  towards S6b instead of away from it. Rejected; it also comes close to blinding a guard.
- **4 KB sector erase** (45 ms typical, 400 ms max) is still 8x longer than the 5.5 ms FIFO time.
  It would lose fewer bytes but still lose frames. Rejected.
- **`CONFIG_SPIRAM_XIP_FROM_PSRAM`** is the one alternative that costs no internal RAM. The S3
  supports it (`SOC_SPIRAM_XIP_SUPPORTED`). With it, flash operations no longer disable the
  cache, so the ISR flag, its 2.8 KB of IRAM and the 8.9 KB of internal driver objects could all
  go back. It costs PSRAM equal to .text+.rodata (about 3 MB of the 7.86 MB free) and changes
  boot and timing behaviour for the whole image. It is an owner decision and needs a soak. Not
  implemented.
- **`CONFIG_SPI_FLASH_AUTO_SUSPEND`** depends on the flash chip model and is risky. Not
  recommended.

Bench result to keep: link errors went from 14/8/12/28 to 0/0/0/0 over two fetches.

## MED-1: fix + UPDFX6 admission was never benched together; estimated idle margin about 1.2 KB

The fix was benched on d0e9fdc19, which predates UPDFX6 (6e237eadd). The d0e9 fetch admission
was looser: precheck 29647 B (`FETCH_IDLE_MIN_FREE_BYTES`), abort 8704 B, DMA minimum 2048 B.
Today's `update_fetch_heap.h` requires the following of an idle board:

- `FETCH_HEAP_PRECHECK_MIN` 34248 B internal = ADMISSION 33224 + scratch 1024;
- DMA admission 25436 B = ADMISSION - 7788.

So "the fetch was admitted on the bench with the fix" does not show the fix is admitted under
UPDFX6.

Numbers:

- **The fix's cost.** The fix costs about 11.7 KB internal, and the map confirms the IRAM share.
  `.iram0.text` grew 0x15c77 to 0x1676f (+2808 B), and the DRAM start moved 0x3fc9a100 to
  0x3fc9ac00 (2816 B less heap). bss and data are unchanged. The rest is driver objects the IDF
  now takes `MALLOC_CAP_INTERNAL`: rings 2 x 3072 = 6144 B, `rx_data_buf` 2 x 512 = 1024 B, and
  queues, semaphores and the obj about 1.7 KB.
- **The only post-UPDFX6 idle reading** is the one `update_fetch_heap.h` quotes: 47191 B on
  36f3ad031, a single boot. Fix applied: 47191 - 11700 = about 35.5 KB, against 34248. **About
  1.2 KB of margin.**
- **DMA rule.** Internal SRAM is DMA-capable, so the fix's cost comes out of the DMA pool too.
  Under the measured 7788 B offset: 39403 - 11700 = 27.7 KB against 25436. About 2.3 KB of
  margin.
- **Every other idle reading** the header lists is older firmware (pre.3 30559, benchupd2 30299,
  sk04 29647..31123). Those were already refused before this fix.

**Agrees with REVIEW_UPDFX6 MED-2**, which landed while this review ran. It starts from the d0e9
control idle of about 46.6 KB instead of 47191 B and gets about +650 B at both start admissions.
It also notes that the asset download's 302 hop sits about 700 B under its 33224 B need and is
admitted only if the settle wait recovers it. Taken together, the margin is +650 B to +1.2 KB at
the start, and about zero or negative at the redirect hop. One bench fetch settles both reviews'
open item.

**Scenario.** Dev tip boots with idle internal free about 1.3 KB lower than the 36f3ad031
reading. One extra httpd socket, an LVGL page allocation, or simply a different boot does that.
`update_stage_release` then fails `low_heap_free` after the settle wait. This is safe: it never
reaches the floor. But the GitHub update path is unavailable, and nothing on the bench has shown
otherwise.

**Action (bench owner).** Run one `update_stage_release` on the dev tip, which now holds both
changes. Record idle internal and DMA free before the fetch and the job's low-water. Until then,
any change that adds internal RAM should count against this 1.2 KB. The trims in LOW-4 would
restore margin, but none can be verified without the board.

## LOW-1: RX ring halves the erase-stall tolerance

RX 2048 B holds 89 ms of continuous traffic at 230400 (safety link) and 22 ms at 921600 (PC
link). 4096 B held 178 / 44 ms. The ring only has to absorb what arrives while the ISR runs and
the consuming task is stalled by the cache-off. In the typical 400 ms block erase, the Pico sends
about one reply plus 1-3 broadcasts. That is well under 2048 B (a status frame is far smaller
than the 528 B stuffed maximum).

**Worst-case scenario.** A 2 s datasheet-max erase that coincides with a trip latch (trip repeat
every 250 ms, 8 repeats) plus 4 status broadcasts and a DIAG/POWER pair. Even at a mean of 200 B
per frame that is about 2.8 KB, which exceeds 2048. A full ring makes the event task flush the
whole ring (`UART_BUFFER_FULL`/`UART_FIFO_OVF`), so every frame in it is lost, not only the
newest. Trip repeats exist so a lost one is resent, and status rebroadcasts every 500 ms, so the
link recovers. No guard is blinded: the Pico latches on its own side. This tolerance case was
not exercised on the bench. Acceptable as is; noted so that nobody cuts the RX ring further
without measuring.

## LOW-2: static asserts hardcode 528 instead of tracking the frame maximum

`_Static_assert(HAL_UART_ESP_RX_RING_BUF_SIZE >= 3 * 528, ...)` and `TX >= 528 + 64` use a
literal. The real limits are `KILNLINK_FRAME_STUFFED_MAX` (CommonFW `kilnlink_frame.h`, 528) and
`uart_protocol.c`'s local `STUFFED_FRAME_MAX` (also 528 from `UART_PROTO_MAX_PAYLOAD` 253). A
kilnlink protocol bump (18 is planned) that raises the payload would leave both asserts passing
against a stale 528. The TX ring would then fail differently: a frame larger than 504 B can only
go into an empty 1024 ring, and anything over 960 B is split into two chunks, which brings back
the partial-send caveat in `hal_uart.h`.

Also, `hal_uart_esp.c` is compiled only by the target build and by
`firmware/hwAbstraction/test/compile_esp_backends.ps1`. Both are SKIP-FAST under `-Fast`, so a
`-Fast` run never evaluates these asserts.

**Proposal.** Export the ring sizes in a small header and static-assert in `uart_protocol.c`
against `STUFFED_FRAME_MAX`. Or make `hal_uart_esp.c` include the CommonFW header if it is on the
component's include path. Not done here: it reshuffles headers across components and the gain is
for a future bump.

## LOW-3: the sdkconfig pin catches deletion of the key, not a flip to "not set"

Negtest result:

- Replacing `CONFIG_UART_ISR_IN_IRAM=y` with `# CONFIG_UART_ISR_IN_IRAM is not set` in
  `sdkconfig.defaults` is MISSED by `check_sdkconfig_defaults_applied.ps1`.
- Deleting the line is CAUGHT.

The check is designed to verify that a pinned value reached the generated sdkconfig, whatever
that value is. A "not set" line is a valid pin, and a full build then generates a matching
`# ... is not set`, so the full suite passes too. Nothing in the standing suite asserts that this
flag is ON, and turning it off silently brings back the 14/8/12/28 link errors during a fetch.

**Proposal.** Add an expected-value list (key=value) to the check for keys whose value is
load-bearing, starting with this one. Not done here: it changes the semantics of a shared check
for every watched key.

## LOW-4: SK-04 largest-block alarm crossed during the fetch's TLS handshake

The fix bench run shows internal largest-free-block low-water 7936 B during the TLS handshake,
768 B under `KILN_DRAM_LARGEST_ALARM_BYTES` 8704. The control on d0e9 showed 16384. The block
recovers to 11776 B. The free-heap alarm, 11903 B, was not crossed (min_free 14267). The fetch's
own `FETCH_LARGEST_BLOCK_MIN` is 6144, so the fetch keeps 1792 B of margin.

This is not new to this change. The pre.3 fetch (BENCH_TEST_LOG, update_stage_release
2026-10-13) dipped to 7680 B, and some earlier builds idled at 7680-8192 B.

8704 is the level at which a browser loading `/app.js` once failed (`dram_margin.h`). So during
the seconds of a TLS handshake a web page load can fail. A JSON status poll is small and is not
at risk.

**Recommendation (owner decides; threshold not changed here).**

- Do not raise or lower 8704. It is a measured failure level, not a margin.
- Accept the transient dip while a fetch job is running: annotate SK-04 or the alarm reporter as
  fetch-tolerant while `update_fetch_status` is RUNNING, and keep it hard at idle.
- To clear the dip outright, the block needs about 800 B more. These would plausibly lift it
  above 8704 again:
  - TX ring 0 on the PC link: about 1.1 KB (ring plus its ringbuf struct);
  - PC-link RX 2048 to 1024: 1 KB.

  Both are board-gated (see below).

**Trim options (all proposals; none verifiable without the board):**

- **TX ring 0 (Q2).** Safe to call: the only TX caller is `uart_protocol.c` `frame_and_send()`,
  via `hal_uart_send_blocking()`, for both ports. `hal_uart_send()` has no live caller.
  `frame_and_send` already waits for `uart_wait_tx_done`, so a 0 ring adds no latency. Two catches:
  - `hal_uart_send_blocking()` must change. `uart_get_tx_buffer_free_size()` returns 0 for
    `tx_buf_size == 0`, so the free-space poll would always time out.
  - With ring 0 the task feeds the FIFO 128 B at a time. A non-IRAM task does not run during
    cache-off, so a frame in flight stalls mid-frame for the erase time. That is the pre-fix
    TX-direction behaviour, and the Pico's inter-byte handling would see torn frames.

  Not recommended for the safety link. Possible for the PC link only: saves about 1.1 KB, needs a
  per-port ring size in `hal_uart_cfg_t`.
- **PC-link RX 1024.** Stop-and-wait, one frame (at most 528 B stuffed) outstanding plus PC
  retries every 200 ms. A 400 ms erase can hold about 3 frames (1584 B), so 1024 overflows and
  the PC retries. That is acceptable on a bench/debug link. Saves 1 KB.
- **Event queue 16 to 8 per port.** About 200 B total. Not worth a change on its own.
- **`CONFIG_SPIRAM_XIP_FROM_PSRAM`** (see verdict). Gets back all 11.7 KB. Owner decision.

## INFO-1: ISR path is IRAM-safe (Q1)

Under the flag:

- `UART_MALLOC_CAPS` becomes `MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT` for the uart obj,
  `rx_data_buf`, both rings, the event and pattern queues and the semaphores.
- The UART Kconfig selects `VFS_SELECT_IN_RAM` and depends on
  `!RINGBUF_PLACE_ISR_FUNCTIONS_INTO_FLASH`, so the ringbuf ISR functions stay in IRAM. The
  FreeRTOS ISR functions stay in IRAM because `FREERTOS_PLACE_ISR_FUNCTIONS_INTO_FLASH` depends on
  auto-suspend, which is off.
- No pattern detection and no app ISR callbacks are installed (`uart_driver_install(..., 0)`).
- `PM_ENABLE` is off, so there is no PM lock path.

Nothing in the ISR touches PSRAM or flash-resident code or data.

Both links use the same `hal_uart_init()`. The safety link is UART1 at 230400; the PC link is
UART_OWNER_PORT_NUM at 921600. Both frame at most 528 B stuffed. 1024 TX holds one such frame:
as items 504 + 24 plus the header item, about 570 B of ring. The PC link's 2048 RX covers one
stop-and-wait frame plus two retries during a typical erase.

## INFO-2: TX free-size cap on a non-empty 1024 ring

`uart_get_tx_buffer_free_size()` reports:

- `tx_buf_size - 8` = 1016 for an empty NOSPLIT ring;
- at most `xRingbufferGetMaxItemSize` = ALIGN4(1024/2) - 8 = 504 while anything is queued.

A 528 B frame therefore waits for the ring to drain completely. That is harmless today because
`frame_and_send` waits `uart_wait_tx_done` after every frame, so the ring is empty at the next
send. The worst wait is 1016 B at 23 KB/s (44 ms) plus 10 ms poll steps, within the 200 ms ack
timeout. On the old 4096 ring the cap was 2040, so this wait never happened.

## INFO-3: tick period comment was wrong (fixed)

`hal_uart_esp.c` said `portTICK_PERIOD_MS` is 1 ms. KilnFW builds with `CONFIG_FREERTOS_HZ=100`,
so `vTaskDelay(1)` polls every 10 ms and `HAL_UART_ESP_MIN_WAIT_TX_DONE_TICKS` (2) is 20 ms. That
is still well under the 200 ms ack timeout, so behaviour is unaffected. This existed before the
change.

## INFO-4: stale comments fixed; recovery image unaffected

These still described a 4096 B TX ring:

- `hal_uart_esp.c`: the chunking comment, and a header comment pointing at uart_owner.c macros
  that no longer exist;
- `uart_protocol.c` partial-send note;
- `hal_uart.h` caveat.

All are updated to 1024 B ring / 960 B chunk / 528 B frame. `firmware/KilnFW_recovery` has its
own sdkconfig without the flag and installs its Pico UART RX-only with no TX ring. It is not
affected by this change.

## Q4 answer (UPDFX6 fetch admission)

On paper yes, by about 650 B (REVIEW_UPDFX6 MED-2, from the 46.6 KB control idle) to 1.2 KB
internal (estimated 35.5 KB idle against 34248) and about 2.3 KB DMA
(about 27.7 KB against 25436). That rests on one 47191 B idle reading from a pre-fix boot. Not
proven: the fix was benched on d0e9fdc19 under the older 29647 B rule. See MED-1 for the bench
action. A refusal is safe (`low_heap_free`, the floor is never reached) but makes GitHub updates
unavailable. The 302 hop is tighter still (about -700 B before the settle wait recovers it, per
REVIEW_UPDFX6).

## Negtests (`tools\negtest.ps1`)

| Mutation | Target | Command | Result |
|---|---|---|---|
| RX ring 2048 -> 1024 | `hal_uart_esp.c` | `-Command`: real IDF compile command for `hal_uart_esp.c` from a KilnFW target build's `compile_commands.json`, `-fsyntax-only`; baseline compiles (exit 0) | CAUGHT: `static assertion failed: "RX ring must hold several worst-case stuffed kilnlink frames"` |
| TX ring 1024 -> 512 | `hal_uart_esp.c` | same | CAUGHT: `static assertion failed: "TX ring must hold one worst-case stuffed frame plus ring item overhead"` |
| delete `CONFIG_UART_ISR_IN_IRAM=y` | `sdkconfig.defaults` | `-Preset check firmware/KilnFW/App/test/check_sdkconfig_defaults_applied.ps1` | CAUGHT: `CHECK FAILED: watched key(s) missing from ...sdkconfig.defaults` |
| `=y` -> `# ... is not set` | `sdkconfig.defaults` | same | MISSED (LOW-3) |

The two compile CAUGHTs are the static assertions themselves firing; the unmutated file compiles
cleanly with the same command. Every run reported the real tree unchanged.

## Fix status (UPDFX7)

Landed on dev as 79232daa7, e1dfa978e, e95803d1a and fbb68c838.

- LOW-2: `hal_uart_esp.c` now includes `kilnlink/kilnlink_frame.h` and asserts
  `RX >= 3 * KILNLINK_FRAME_STUFFED_MAX` and `TX >= KILNLINK_FRAME_STUFFED_MAX + 64`,
  so a protocol bump that raises the maximum frame is caught at compile time.
  The file compiles only in the target build, which the full suite ran
  (`check_00_kilnfw_target_build.ps1` passed).
- LOW-3: `check_sdkconfig_defaults_applied.ps1` has a `$mustBe` table of keys
  whose value is load-bearing, starting with `CONFIG_UART_ISR_IN_IRAM = y`.
  Negtest (`-Preset check`): flipping the line to
  `# CONFIG_UART_ISR_IN_IRAM is not set` is now CAUGHT ("load-bearing
  sdkconfig.defaults value changed").
