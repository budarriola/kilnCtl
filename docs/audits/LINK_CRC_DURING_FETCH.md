# Kilnlink CRC/length errors during update staging (2026-10-13)

## Symptom
During `update_stage_release` (TLS download of v1.0.0-pre.3, 2.96 MB, written to the stage
partition) the ESP<->Pico link counters rose (crc/framing 1->9, crc mismatch 0->8, length
mismatch 2->18, reply timeouts 1->3, `link_reply_us` max ~225-400 ms).

## Root cause
Flash erase/write disables the instruction cache for both cores. `update_stage.c` erases ahead
in `UPDATE_STAGE_ERASE_UNIT` = 64 KB blocks (a ~150-400 ms cache-off stall each). The UART1
(kilnlink) driver ISR was NOT in IRAM (`CONFIG_UART_ISR_IN_IRAM` unset, `uart_driver_install`
flags 0), so it cannot run during the stall. The S3 UART RX FIFO is 128 B = ~5.5 ms at 230400
baud; bytes past that are lost mid-frame, which the deframer reports as length/crc mismatch and
the poll as a reply timeout. The 4096 B driver ring was never the limit (it is only filled by
the ISR). Not Wi-Fi/TLS: 60 GETs of the 86 KB web UI (5 MB) gave 0 errors.

## Evidence (same dev tip d0e9fdc19, same worktree, only the Kconfig key toggled)
| Build | crc/framing | timeouts | crc mismatch | length mismatch |
|---|---|---|---|---|
| control (ISR not in IRAM), fetch #1 on 36f3ad031 | +0 -> 1 | 1 | 0 | 2 |
| control, fetch on d0e9fdc19 | 14 | 8 | 12 | 28 |
| fix, fetch #1 | 0 | 0 | 0 | 0 |
| fix, fetch #2 | 0 | 0 | 0 | 0 |

## Fix
`CONFIG_UART_ISR_IN_IRAM=y` (sdkconfig.defaults, pinned in `check_sdkconfig_defaults_applied.ps1`).
Side effect found on the bench: the IDF then allocates the UART rings `MALLOC_CAP_INTERNAL`
(they were PSRAM). With 2 ports x (4096+4096) the board lost ~22.7 KB internal heap and the
fetch refused with `low_heap_free`. `hal_uart_esp.c` rings are now RX 2048 / TX 1024
(worst-case stuffed kilnlink frame 528 B; static asserts added; TX chunking leaves 64 B for the
ring item header). Net cost 11.7 KB internal.

## Internal RAM
heap_internal min_free during fetch: 14267 B (control 22023 B; floor 8192 B). Largest free block
low-water dipped to 7936 B (control 16384 B), below the SK-04 8704 B alarm, during the TLS
handshake only; it recovers to 11776 B. Follow-up if wanted: shrink rings further or pull the
PC-link UART rings back to PSRAM by not sharing the global flag (not possible with this IDF).

## Not done
Smaller erase units (4 KB) would shorten stalls but cannot make them shorter than the FIFO.
Pico-side counters are not separately reported; the ESP-side counters above are the RX direction.
