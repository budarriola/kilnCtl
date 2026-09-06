// ROADMAP.md M15 1500-line rule: move-only split of panel_spi.c. This file
// owns the streaming-blit protocol (BEGIN/DATA/END/ABORT, the sync and
// async flush fast paths) and RDDID read-back. See panel_spi.c's header
// comment for the full file map and panel_spi_internal.h for the shared
// declarations.
//
// EXTREME CARE (2026-09-04, settled after a long debugging effort):
// panel_codec.c/this file send RGB565 MSB-first (a fix for a real bug where
// LVGL's native little-endian order was sent as-is) -- preserve the
// byte-swap in ILI9488_blit_data() and ILI9488_blit_data_async() exactly,
// in both the sync and async paths. The async-flush and zero-copy-flush
// branches (CONFIG_KILNCTL_SPI_ASYNC_FLUSH / CONFIG_KILNCTL_DISPLAY_
// ZERO_COPY_FLUSH) are compiled OUT by Kconfig default and are documented
// KNOWN BROKEN with respect to byte order -- moved faithfully, warning
// comments intact, not reworked here.
#include "panel_spi.h"
#include "panel_spi_internal.h"

#include <string.h>

#include "esp_log.h"
#include "settings.h"

/* ===================================================================
 * Streaming blit
 * ===================================================================
 *
 * The point of this three-call shape is that a 480x320 image is 460,800
 * bytes -- far more than one UART frame (127 bytes of payload) and far more
 * than the ESP could buffer. So the window is opened once and left open
 * while the PC dribbles pixels in over hundreds of frames.
 *
 * That is legal precisely because of the datasheet's Data Transfer Pause
 * rule (§4.3): releasing CS between whole bytes of frame memory data pauses
 * rather than aborts the write, and the controller resumes at the same
 * address. So the gaps between UART frames -- milliseconds of them -- are
 * invisible to the panel.
 *
 * The cost is that the driver is holding hardware state across calls that
 * the caller could get wrong, so every transition is checked:
 *   - DATA before BEGIN                 -> ESP_ERR_INVALID_STATE
 *   - BEGIN while already open          -> the old window is abandoned (logged)
 *   - an odd byte count (a split pixel) -> ESP_ERR_INVALID_ARG, blit aborted
 *   - more pixels than the window holds -> ESP_ERR_INVALID_SIZE, blit aborted
 *   - END with the window unfilled      -> succeeds, but logs the shortfall
 * Anything else touching the panel aborts the blit (panel_spi_reject_if_blitting).
 */

esp_err_t ILI9488_blit_begin(ILI9488Class *disp, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    if (disp->async_pending) {
        /* Same reasoning as panel_spi_reject_if_blitting(): a blit already
         * open is safe to abandon (nothing is mid-transfer), an async
         * flush's last chunk still in flight is not. */
        panel_spi_unlock(disp);
        ESP_LOGE(PANEL_SPI_TAG, "BLIT_BEGIN while an async flush's last chunk is still in flight; refusing");
        return ESP_ERR_INVALID_STATE;
    }
    if (disp->blit.active) {
        ESP_LOGW(PANEL_SPI_TAG, "BLIT_BEGIN with a blit already open (%u/%u pixels); abandoning the old one",
                 (unsigned)disp->blit.pixels_done, (unsigned)disp->blit.pixels_total);
        panel_spi_blit_clear_state(disp);
    }
    if (!panel_spi_rect_in_bounds(disp, x, y, w, h)) {
        panel_spi_unlock(disp);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = panel_spi_begin_ram_write(disp, x, y, w, h);
    if (err == ESP_OK) {
        disp->blit.active = true;
        disp->blit.x = x;
        disp->blit.y = y;
        disp->blit.w = w;
        disp->blit.h = h;
        disp->blit.pixels_total = (uint32_t)w * h;
        disp->blit.pixels_done = 0;
    }
    panel_spi_unlock(disp);
    return err;
}

esp_err_t ILI9488_blit_data(ILI9488Class *disp, const uint8_t *data, size_t len)
{
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;
    if (!data && len > 0) return ESP_ERR_INVALID_ARG;

    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;

    if (disp->async_pending) {
        /* disp->blit.active is already false in this window (see
         * async_pending's comment in panel_spi.h), so the check below would
         * already catch this with a less specific message -- this one is
         * just clearer about why. */
        panel_spi_unlock(disp);
        ESP_LOGE(PANEL_SPI_TAG, "BLIT_DATA while an async flush's last chunk is still in flight; refusing");
        return ESP_ERR_INVALID_STATE;
    }
    if (!disp->blit.active) {
        panel_spi_unlock(disp);
        ESP_LOGE(PANEL_SPI_TAG, "BLIT_DATA with no open window");
        return ESP_ERR_INVALID_STATE;
    }
    if ((len & 1u) != 0) {
        /* A half pixel would shift every following pixel by one byte and
         * smear the rest of the image; there is no way to recover, so the
         * blit ends here. */
        ESP_LOGE(PANEL_SPI_TAG, "BLIT_DATA length %u is odd (split RGB565 pixel); aborting", (unsigned)len);
        panel_spi_blit_clear_state(disp);
        panel_spi_unlock(disp);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t pixels = (uint32_t)(len / 2);
    if (panel_codec_blit_overruns(pixels, disp->blit.pixels_total, disp->blit.pixels_done)) {
        ESP_LOGE(PANEL_SPI_TAG, "BLIT_DATA overruns the window: %u more pixels, %u remaining; aborting",
                 (unsigned)pixels,
                 (unsigned)(disp->blit.pixels_total - disp->blit.pixels_done));
        panel_spi_blit_clear_state(disp);
        panel_spi_unlock(disp);
        return ESP_ERR_INVALID_SIZE;
    }

    /* D/C is already parked in "data" from blit_begin and stays there: this
     * loop is pure SPI, with zero expander traffic no matter how many chunks
     * or how many UART frames the image takes. */
    /* chunk_pixels of zero would make the loop below advance by nothing and
     * spin forever with the lock held -- reachable by lowering chunk_bytes
     * under three, which the header explicitly invites callers to do. */
    esp_err_t err = ESP_OK;
    size_t chunk_pixels = panel_spi_chunk_pixels(disp);
    if (chunk_pixels == 0) {
        ESP_LOGE(PANEL_SPI_TAG, "chunk_bytes (%u) is smaller than one pixel; aborting the blit",
                 (unsigned)disp->chunk_bytes);
        panel_spi_blit_clear_state(disp);
        panel_spi_unlock(disp);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t bpp = disp->panel->bytes_per_pixel;
    uint32_t sent = 0;
    while (sent < pixels) {
        size_t n = (size_t)(pixels - sent);
        if (n > chunk_pixels) n = chunk_pixels;

        if (bpp == 2 && KILNCTL_DISPLAY_ZERO_COPY_FLUSH) {
            /* DISPLAY_ST7796_PLAN.md 9.7, CONFIG_KILNCTL_DISPLAY_ZERO_COPY_FLUSH
             * (default OFF). The Phase 3 fast path just below already proved
             * there is nothing to COMPUTE for a bpp==2 panel; this goes one
             * step further and skips the memcpy() into disp->scratch too --
             * &data[sent*2] is DMA'd straight to the panel. Only reachable
             * when bpp == 2, i.e. only on the ST7796 descriptor, which has
             * never run on real hardware (see the plan doc's STOP block) --
             * inert on the ILI9488 (bpp == 3) regardless of the Kconfig
             * setting, and inert on the ST7796 too unless this option is
             * explicitly turned on.
             *
             * Correctness note this option's own Kconfig help repeats: the
             * transfer below requires `data` itself to be DMA-capable
             * (internal DRAM 4-byte aligned, or PSRAM -- see section 9's
             * "Facts established" alignment bullets). lvgl_port.c's flush
             * callback passes LVGL's own PSRAM draw buffer, which qualifies;
             * a BLIT_DATA frame decoded straight off the UART link may not,
             * so this is scoped bench-tuning behind an explicit opt-in, not
             * something safe to turn on unconditionally for every caller of
             * this shared function. */
            /* KNOWN BROKEN as of 2026-09-04 -- DO NOT ENABLE alongside a bpp==2
             * panel until this is reworked: `data` is DMA'd verbatim in
             * LVGL's own little-endian byte order, but the byte-swap fix in
             * this function's non-zero-copy sibling branch (below) proved the
             * wire wants MSB-first -- a real fix here needs either a real copy
             * (defeating the point of this option) or the panel's byte order
             * reconciled some other way. Left compiling (still default OFF,
             * see the Kconfig help) rather than deleted, since it is a real
             * TODO, not dead code, but it will send visibly wrong colors if
             * turned on today. */
            err = panel_spi_tx(disp, &data[sent * 2], n * 2);
            sent += n;
            if (err != ESP_OK) {
                ESP_LOGE(PANEL_SPI_TAG, "BLIT_DATA transfer failed: %s; aborting", esp_err_to_name(err));
                panel_spi_blit_clear_state(disp);
                panel_spi_unlock(disp);
                return err;
            }
            continue;
        }

        if (bpp == 2) {
            /* FIXED 2026-09-04: this used to be a straight memcpy() on the
             * assumption LVGL's own in-memory (little-endian) byte order
             * IS the RAMWR wire order for a bpp==2 (ST7796) panel -- wrong,
             * see panel_codec_rgb565_passthrough()'s comment for the full
             * story (a real ST7796 module's colors, camera-verified, proved
             * it). Still no per-pixel COMPUTATION (no channel math, no
             * widening), just a mandatory byte swap -- one loop, not a
             * memcpy, but still the cheap bpp==2 path relative to the
             * RGB666 widening in the `else` branch below. */
            for (size_t i = 0; i < n; ++i) {
                const uint8_t *src = &data[(sent + i) * 2];
                disp->scratch[i * 2] = src[1];
                disp->scratch[i * 2 + 1] = src[0];
            }
        } else {
            for (size_t i = 0; i < n; ++i) {
                const uint8_t *src = &data[(sent + i) * 2];
                uint16_t color = (uint16_t)(src[0] | ((uint16_t)src[1] << 8));  /* u16 LE on the wire */
                panel_codec_rgb565_to_rgb666(color, &disp->scratch[i * 3]);
            }
        }
        err = panel_spi_tx(disp, disp->scratch, n * bpp);
        if (err != ESP_OK) {
            ESP_LOGE(PANEL_SPI_TAG, "BLIT_DATA transfer failed: %s; aborting", esp_err_to_name(err));
            panel_spi_blit_clear_state(disp);
            panel_spi_unlock(disp);
            return err;
        }
        sent += n;
    }

    disp->blit.pixels_done += pixels;
    panel_spi_unlock(disp);
    return ESP_OK;
}

/* DISPLAY_ST7796_PLAN.md 9.6. Fires from the SPI owner task's own thread
 * (spi_owner_transfer_async()'s documented contract -- never an ISR), once,
 * after the one and only chunk ILI9488_blit_data_async() ever dispatches
 * asynchronously has actually completed on the wire.
 *
 * Takes disp->lock itself to clear async_pending and retrieve the stashed
 * done_cb/ctx. This is legal even though the lock was last taken (and
 * released) by a different task (whichever called
 * ILI9488_blit_data_async()): a FreeRTOS mutex may be TAKEN by any task: it
 * only has to be GIVEN BACK by whoever currently holds it, and here the same
 * task (the owner task) both takes and gives it back, so there is no
 * cross-task-give hazard. By the time this runs,
 * ILI9488_blit_data_async() has already unlocked and returned, so this take
 * succeeds immediately unless some other caller (the UART BLIT_DATA handler,
 * or a non-blit draw call) is genuinely mid-operation -- ordinary
 * contention, not a deadlock.
 *
 * Must NOT call panel_spi_tx()/panel_spi_write_cmd()/spi_owner_transfer[_async]()
 * or anything else that enqueues a new request on this SAME owner: this runs
 * ON the owner task's own thread, inside spi_owner_task()'s loop, before it
 * has gone back to read the next queue entry. Re-entering the owner from
 * here would need the queue to have a free slot available right now to work
 * at all, and if it does not, this task would be blocking on itself to
 * drain its own queue -- that never resolves; SPI_OWNER_TRANSFER_TIMEOUT_MS
 * later the enqueue call gives up and latches `wedged`, freezing the owner
 * task for a full second and then permanently taking the whole bus down
 * (display AND every MAX31856 channel routed through it) for something that
 * was never actually stuck. This is exactly why ILI9488_blit_data_async()
 * does not send the trailing NOP from here -- see its own comment. */
static void ili9488_blit_async_trampoline(void *ctx, esp_err_t result)
{
    ILI9488Class *disp = (ILI9488Class *)ctx;
    spi_owner_async_done_cb_t user_cb;
    void *user_ctx;

    if (panel_spi_lock(disp)) {
        user_cb = disp->async_done_cb;
        user_ctx = disp->async_done_ctx;
        disp->async_done_cb = NULL;
        disp->async_done_ctx = NULL;
        disp->async_pending = false;
        panel_spi_unlock(disp);
    } else {
        /* ILI9488_LOCK_TIMEOUT_MS (5s) elapsed without the lock -- something
         * else is wedged far worse than this transfer. Fail safe in the
         * direction that matters most: still deliver the completion (a
         * caller waiting on lv_display_flush_ready() must never wedge
         * forever over this), read the stashed callback without the lock
         * rather than losing it, but leave async_pending set -- refusing
         * new blits is the safer failure than silently allowing one while
         * this diagnosis is still unresolved. */
        ESP_LOGE(PANEL_SPI_TAG, "async completion could not take the display lock; delivering anyway, "
                      "leaving async_pending latched");
        user_cb = disp->async_done_cb;
        user_ctx = disp->async_done_ctx;
    }

    if (user_cb) {
        user_cb(user_ctx, result);
    }
}

esp_err_t ILI9488_blit_data_async(ILI9488Class *disp, const uint8_t *data, size_t len,
                                   spi_owner_async_done_cb_t done_cb, void *cb_ctx)
{
    if (!done_cb) return ESP_ERR_INVALID_ARG; /* nothing to signal -- caller bug, not runtime error */
    if (!panel_spi_ready(disp)) {
        done_cb(cb_ctx, ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if (!data && len > 0) {
        done_cb(cb_ctx, ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }

    if (!panel_spi_lock(disp)) {
        done_cb(cb_ctx, ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }

    if (disp->async_pending) {
        panel_spi_unlock(disp);
        ESP_LOGE(PANEL_SPI_TAG, "BLIT_DATA_ASYNC with a previous async flush still in flight");
        done_cb(cb_ctx, ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if (!disp->blit.active) {
        panel_spi_unlock(disp);
        ESP_LOGE(PANEL_SPI_TAG, "BLIT_DATA_ASYNC with no open window");
        done_cb(cb_ctx, ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if ((len & 1u) != 0) {
        ESP_LOGE(PANEL_SPI_TAG, "BLIT_DATA_ASYNC length %u is odd (split RGB565 pixel); aborting", (unsigned)len);
        panel_spi_blit_clear_state(disp);
        panel_spi_unlock(disp);
        done_cb(cb_ctx, ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t pixels = (uint32_t)(len / 2);
    if (panel_codec_blit_overruns(pixels, disp->blit.pixels_total, disp->blit.pixels_done)) {
        ESP_LOGE(PANEL_SPI_TAG, "BLIT_DATA_ASYNC overruns the window: %u more pixels, %u remaining; aborting",
                 (unsigned)pixels,
                 (unsigned)(disp->blit.pixels_total - disp->blit.pixels_done));
        panel_spi_blit_clear_state(disp);
        panel_spi_unlock(disp);
        done_cb(cb_ctx, ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t chunk_pixels = panel_spi_chunk_pixels(disp);
    if (chunk_pixels == 0) {
        ESP_LOGE(PANEL_SPI_TAG, "chunk_bytes (%u) is smaller than one pixel; aborting the blit",
                 (unsigned)disp->chunk_bytes);
        panel_spi_blit_clear_state(disp);
        panel_spi_unlock(disp);
        done_cb(cb_ctx, ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }

    if (pixels == 0) {
        /* Degenerate empty flush -- nothing to send, nothing to ever go
         * async. Close the blit exactly like ILI9488_blit_end() (minus the
         * NOP -- see this function's declaration comment for why that is
         * safe) and deliver done_cb synchronously right here. */
        panel_spi_blit_clear_state(disp);
        panel_spi_unlock(disp);
        done_cb(cb_ctx, ESP_OK);
        return ESP_OK;
    }

    uint8_t bpp = disp->panel->bytes_per_pixel;
    uint32_t sent = 0;
    while (sent < pixels) {
        size_t n = (size_t)(pixels - sent);
        if (n > chunk_pixels) n = chunk_pixels;
        bool last_chunk = (sent + n >= pixels);

        const uint8_t *tx_ptr;
        size_t tx_len = n * bpp;
        if (bpp == 2 && KILNCTL_DISPLAY_ZERO_COPY_FLUSH) {
            /* DISPLAY_ST7796_PLAN.md 9.7 combined with 9.6: `data` is DMA'd
             * straight from the caller's buffer for every chunk, including
             * the async last one. This is exactly why deferring done_cb
             * until real completion (not merely "queued") matters here: for
             * this chunk, `data` -- lvgl_port.c's LVGL draw buffer -- IS the
             * DMA source, not a copy of it, and LVGL will not reuse/repaint
             * that buffer until lv_display_flush_ready() has been called.
             * Since this driver defers that call (via done_cb) until the
             * SPI owner's completion callback actually fires,
             * both-flags-on is SAFE: the buffer's lifetime is guaranteed to
             * outlive the transfer by construction, not by luck. */
/* KNOWN BROKEN as of 2026-09-04 -- same wrong-byte-order issue as the
             * sync zero-copy branch above; see that comment. */
            tx_ptr = &data[sent * 2];
        } else if (bpp == 2) {
            /* Same 2026-09-04 byte-swap fix as ILI9488_blit_data()'s sync
             * path above -- was a straight memcpy(), wrong wire order. */
            for (size_t i = 0; i < n; ++i) {
                const uint8_t *src = &data[(sent + i) * 2];
                disp->scratch[i * 2] = src[1];
                disp->scratch[i * 2 + 1] = src[0];
            }
            tx_ptr = disp->scratch;
        } else {
            for (size_t i = 0; i < n; ++i) {
                const uint8_t *src = &data[(sent + i) * 2];
                uint16_t color = (uint16_t)(src[0] | ((uint16_t)src[1] << 8));  /* u16 LE on the wire */
                panel_codec_rgb565_to_rgb666(color, &disp->scratch[i * 3]);
            }
            tx_ptr = disp->scratch;
        }

        if (!last_chunk) {
            /* Every chunk before the last stays fully synchronous -- this
             * is what makes the copy-path branches above safe: the memcpy
             * for chunk N+1 cannot start (next loop iteration) until this
             * panel_spi_tx() has already returned, i.e. until chunk N's own
             * transfer has completed. Only the FINAL chunk is ever
             * outstanding when this function returns. */
            esp_err_t err = panel_spi_tx(disp, tx_ptr, tx_len);
            if (err != ESP_OK) {
                ESP_LOGE(PANEL_SPI_TAG, "BLIT_DATA_ASYNC transfer failed: %s; aborting", esp_err_to_name(err));
                panel_spi_blit_clear_state(disp);
                panel_spi_unlock(disp);
                done_cb(cb_ctx, err);
                return err;
            }
            sent += n;
            continue;
        }

        /* Last chunk: close the blit's logical state now, under the lock --
         * exactly what ILI9488_blit_end() does, minus the NOP (see this
         * function's declaration comment) -- and hand the transfer itself
         * to the SPI owner asynchronously. async_pending is the guard that
         * keeps a second caller off disp->scratch/the bus until this
         * transfer's completion callback clears it. */
        disp->blit.pixels_done += pixels;
        panel_spi_blit_clear_state(disp);
        disp->async_done_cb = done_cb;
        disp->async_done_ctx = cb_ctx;
        disp->async_pending = true;

        esp_err_t err = spi_owner_transfer_async(disp->owner, disp->dev, tx_ptr, tx_len, disp->cs_gpio,
                                                  ili9488_blit_async_trampoline, disp);
        if (err != ESP_OK) {
            /* CONFIG_KILNCTL_SPI_ASYNC_FLUSH off (ESP_ERR_NOT_SUPPORTED) or
             * a genuine enqueue failure -- either way nothing was queued and
             * the trampoline will never run, so undo the async bookkeeping,
             * send this last chunk synchronously right here instead of
             * leaving it half-sent, and deliver done_cb ourselves. */
            disp->async_pending = false;
            disp->async_done_cb = NULL;
            disp->async_done_ctx = NULL;
            esp_err_t sync_err = panel_spi_tx(disp, tx_ptr, tx_len);
            panel_spi_unlock(disp);
            if (sync_err != ESP_OK) {
                ESP_LOGE(PANEL_SPI_TAG, "BLIT_DATA_ASYNC fallback transfer failed: %s", esp_err_to_name(sync_err));
            }
            done_cb(cb_ctx, sync_err);
            return sync_err;
        }

        panel_spi_unlock(disp);
        return ESP_OK; /* done_cb fires later, from the SPI owner task */
    }

    /* Unreachable: pixels > 0 was established above, and every loop
     * iteration either continues (not the last chunk) or returns (the last
     * chunk, either branch). Kept only so the function has a defined return
     * if that invariant is ever violated by a future edit. */
    panel_spi_unlock(disp);
    done_cb(cb_ctx, ESP_OK);
    return ESP_OK;
}

esp_err_t ILI9488_blit_end(ILI9488Class *disp)
{
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;

    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    if (!disp->blit.active) {
        panel_spi_unlock(disp);
        ESP_LOGE(PANEL_SPI_TAG, "BLIT_END with no open window");
        return ESP_ERR_INVALID_STATE;
    }
    if (disp->blit.pixels_done < disp->blit.pixels_total) {
        /* Not an error: a caller is allowed to stop early, and the panel
         * simply keeps whatever was already in the unwritten part of the
         * window. Logged because it is far more often a dropped frame. */
        ESP_LOGW(PANEL_SPI_TAG, "BLIT_END with %u of %u pixels written",
                 (unsigned)disp->blit.pixels_done, (unsigned)disp->blit.pixels_total);
    }
    panel_spi_blit_clear_state(disp);

    /* NOP closes the memory write cleanly: per §5.2.x any new command ends
     * the RAMWR stream, and sending a harmless one now means the next
     * operation cannot accidentally be interpreted as more pixel data. */
    esp_err_t err = panel_spi_write_cmd(disp, ILI9488_CMD_NOP, NULL, 0);
    panel_spi_unlock(disp);
    return err;
}

esp_err_t ILI9488_blit_abort(ILI9488Class *disp)
{
    if (!panel_spi_ready(disp)) return ESP_ERR_INVALID_STATE;
    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    panel_spi_blit_clear_state(disp);
    esp_err_t err = panel_spi_write_cmd(disp, ILI9488_CMD_NOP, NULL, 0);
    panel_spi_unlock(disp);
    return err;
}

bool ILI9488_blit_active(ILI9488Class *disp)
{
    if (!disp || !disp->lock) return false;
    /* A lock we cannot take says nothing about the blit state, and this
     * function has no way to report "don't know" -- so it answers with the
     * conservative one: callers use `true` to refuse other drawing. */
    if (!panel_spi_lock(disp)) return true;
    bool active = disp->blit.active;
    panel_spi_unlock(disp);
    return active;
}

/* ===================================================================
 * Read-back
 * =================================================================== */

/* RDDID (04h), §5.2.3: 24 bits of ID preceded by dummy data.
 *
 * Two things about reads on this part, both from the datasheet:
 *
 *  1. The read clock is much slower than the write clock. §17.4.3 gives
 *     twc (serial clock cycle, write) >= 50ns but trc (read) >= 150ns --
 *     20 MHz vs 6.67 MHz. Clocking a read at the write speed is the classic
 *     reason an ILI9488 "has no ID": the panel is fine, the sampling is not.
 *     Hence the separate slow device handle (ILI9488_READ_CLOCK_HZ).
 *
 *  2. The dummy is ambiguous in the document. The command table (§5.2.3)
 *     calls the 1st parameter "dummy data", i.e. a whole byte, while the
 *     4-line read waveform (Figure 10) shows a single "Dummy Clock Cycle"
 *     before D23. This driver reads four bytes and discards the first, which
 *     matches the byte reading and is what byte-oriented SPI masters can
 *     actually do. If the IDs ever come back looking shifted left by one bit
 *     relative to a known-good value, the one-bit reading is the right one
 *     and this needs a bit-banged or 33-bit transfer instead.
 *
 * MISO is wired on J2 (pin 8), so a read is physically possible here -- but
 * plenty of these modules leave the panel's SDO unconnected internally or
 * share it with the touch controller, which is why an all-zero/all-ones
 * answer is reported as ESP_ERR_NOT_FOUND rather than as success. */
esp_err_t ILI9488_read_id(ILI9488Class *disp, uint8_t out_id[3])
{
    if (!panel_spi_ready(disp) || !disp->read_dev) return ESP_ERR_INVALID_STATE;
    if (!out_id) return ESP_ERR_INVALID_ARG;

    if (!panel_spi_lock(disp)) return ESP_ERR_TIMEOUT;
    esp_err_t err = panel_spi_reject_if_blitting(disp);
    if (err != ESP_OK) {
        panel_spi_unlock(disp);
        return err;
    }

    err = panel_spi_set_dc(disp, false);
    if (err == ESP_OK) {
        disp->scratch[0] = ILI9488_CMD_RDDID;
        err = spi_owner_transfer(disp->owner, disp->read_dev, disp->scratch, 1, NULL, 0,
                                 disp->cs_gpio);
    }
    if (err != ESP_OK) {
        panel_spi_unlock(disp);
        ESP_LOGE(PANEL_SPI_TAG, "RDDID command failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Clock out four don't-care bytes to clock in dummy + ID1..ID3. tx and rx
     * use disjoint slices of the scratch because the transfer is full-duplex
     * and would otherwise overwrite the tx pattern as it reads. */
    err = panel_spi_set_dc(disp, true);
    if (err == ESP_OK) {
        uint8_t *tx = disp->scratch;
        uint8_t *rx = disp->scratch + 8;
        memset(tx, 0x00, 4);
        memset(rx, 0x00, 4);
        err = spi_owner_transfer(disp->owner, disp->read_dev, tx, 4, rx, 4, disp->cs_gpio);
        if (err == ESP_OK) {
            out_id[0] = rx[1];
            out_id[1] = rx[2];
            out_id[2] = rx[3];
            if ((out_id[0] | out_id[1] | out_id[2]) == 0x00 ||
                (out_id[0] & out_id[1] & out_id[2]) == 0xFF) {
                ESP_LOGW(PANEL_SPI_TAG, "RDDID returned %02X %02X %02X -- MISO is probably not driven",
                         out_id[0], out_id[1], out_id[2]);
                err = ESP_ERR_NOT_FOUND;
            }
        }
    }
    panel_spi_unlock(disp);
    return err;
}
