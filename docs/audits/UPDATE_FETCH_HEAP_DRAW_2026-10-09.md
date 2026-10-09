# Update fetch internal-heap draw: can the 18548 B worst case shrink? (2026-10-09)

Status: study only. No code change shipped; sdkconfig changes are proposals.

## Problem
`FETCH_HEAP_PRECHECK_MIN` = 8192 (owner floor) + 18548 (worst draw) + 4096 (KDF/httpd margin) = 30836 B
(`firmware/KilnFW/App/drivers/update/update_fetch_heap.h`, d19d3df4). Idle free internal heap is
29647-31123 B (`logs/sk04_sampling/2026-10-06.tsv`, main tree), so a board at its idle minimum is refused.
To admit at 29647 B with the full 4096 B margin the worst draw must be <= 29647 - 8192 - 4096 = **17359 B**,
i.e. cut at least **1189 B**.

## What the 18548 B is made of (from header and update_fetch.c)
| item | bytes | where it lives | movable? |
|---|---|---|---|
| WP7 measured handshake draw (rx 2 KB + tx 1 KB client buffers, lwip/socket, non-buffer mbedTLS internals) | 11044 | internal | partly, see below |
| tx buffer raised 1 KB -> 2 KB (`FETCH_TX_BUF_BYTES`) | 1024 | internal (esp_http_client malloc <= `SPIRAM_MALLOC_ALWAYSINTERNAL` 8192) | yes, via size |
| writer task stack `update_fetch_wr` 4096 + TCB/overhead | ~4.5 KB (4476 incl. rounding of the 16500 figure) | internal | **no** |
| stager scratch `FETCH_HEAP_SCRATCH_BYTES` | 2048 | internal on purpose (MED-1) | size only |
| total | 18548 | | |

(The 16500 B base is itself an estimate: 11044 + 1024 + ~4.5 KB; the sum is not re-derived on hardware here.)

## Routes studied
1. **mbedTLS buffers.** Already option D: `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` and `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`
   (sdkconfig.defaults:604-607), enforced by a `#error` in update_fetch.c. The in/out record buffers are already in
   PSRAM and freed after the handshake, and are NOT in the 18548 B. Shrinking `MBEDTLS_SSL_IN_CONTENT_LEN` is
   pointless for internal heap and unsafe (GitHub sends 16 KB records). Nothing to gain.
2. **Writer stack to PSRAM.** Not safe. The writer task exists to call flash erase/write (cache disabled); a PSRAM
   stack is inaccessible during those windows. Must stay internal. The TLS task stack is already PSRAM
   (`xTaskCreatePinnedToCoreWithCaps(... MALLOC_CAP_SPIRAM)`), as are the work area, API body and chunk buffers.
3. **Scratch to PSRAM.** Reverted deliberately in MED-1: a PSRAM scratch makes `esp_partition_read` borrow an internal
   bounce buffer of up to 16 KiB. Rejected.
4. **Smaller scratch.** `UPDATE_STAGE_SCRATCH_MIN` is 256 B, so 2048 -> 1024 is API-legal and saves **1024 B**
   (cost: more, smaller flash read/verify calls in stage begin/status; throughput not measured here).
5. **Smaller http buffers.** `esp_http_client_config_t.buffer_size = 2048` (rx) and `.buffer_size_tx = 2048`.
   tx cannot go back to 1 KB: a JWT-signed release-assets GET line can exceed 1 KB (comment at update_fetch.c:70).
   rx 2048 -> 1024 would save **1024 B** but must hold the longest response header line (the signed `Location`
   is the long one; the loc-capture refuses overlong ones). Untested against real GitHub redirects.
6. **Route the http client mallocs to PSRAM.** esp_http_client uses plain malloc; with `SPIRAM_MALLOC_ALWAYSINTERNAL=8192`
   they land internal. Lowering that threshold (e.g. to 1024) would send the 2 KB rx/tx buffers (4096 B) and similar
   mid-size blocks to PSRAM, but it is global (LVGL objects and every other module's allocations), so it is a
   board-wide change needing a full soak. Proposal only.

## Arithmetic
- Need >= 1189 B reduction. Route 4 alone: 18548 - 1024 = 17524 B -> margin at 29647 = 29647 - 8192 - 17524 = 3931 B
  (165 B short of 4096).
- Routes 4 + 5 together: 18548 - 2048 = 16500 B -> margin 4955 B (precheck 28788 B). Passes with room.
- Route 6 alone at 1024: frees at least the 4096 B of rx+tx, leaving ~14.4 KB -> margin ~9 KB.

## Why nothing was implemented
No single change is clearly safe and sufficient. Route 4 is safe but 165 B short; route 5 changes protocol-visible
behaviour (header capacity) and needs a bench redirect run with a JWT-sized URL, which this study could not do.
The 16500 B base is an estimate, so any new constant should come from a measured WP8 gate (b) run
(log current free before the job and min_free after it), not from subtraction.

## Recommendation
1. Cheapest sufficient: scratch 2048 -> 1024 plus rx buffer 2048 -> 1024, set `FETCH_HEAP_WORST_DRAW_BYTES` to
   16500, precheck 28788 B, then **measure** on the bench with a JWT-length redirect and confirm min_free >= 8192.
   If rx 1024 truncates a real Location header, fall back to scratch-only and accept a 3931 B margin
   (set slack 3900) as a documented, smaller margin, or pursue route 6.
2. Proposed sdkconfig (not applied): `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024` frees ~4 KB for the fetch and
   likely helps idle free overall; needs a full soak for LVGL/DMA regressions and an owner decision.
3. Keep the 8192 B owner floor and the 4096 B margin.
