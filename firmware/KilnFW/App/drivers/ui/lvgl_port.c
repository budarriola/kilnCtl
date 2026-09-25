#include "lvgl_port.h"

#include <stdint.h>
#include <string.h>

#include "esp_attr.h" /* EXT_RAM_BSS_ATTR -- s_ui_walk_targets */
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h" /* esp_timer_create/esp_timer_start_periodic for the 1ms lv_tick callback below --
                         * only the elapsed-time reads (esp_timer_get_time) migrated to hal_time.h; this
                         * file still owns a real periodic esp_timer, which hal_time.h does not model. */
#include "hal_time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lvgl.h"

#include "kiln_ui.h"
#include "settings.h"
#include "stack_margin.h"
#include "touch_cal_store.h"
#include "touch_dev.h"
#include "ui_theme.h"

static const char *TAG = "lvgl_port";

/* LVGL is not thread-safe: every lv_* call below (flush callback, indev
 * callback, tick, timer_handler, and the one-time screen build) runs on
 * lvgl_port_task and nowhere else. Nothing outside this file may touch an
 * lv_* API. */

typedef struct {
    ILI9488Class *display;
    /* Copied out of the `touch_dev` pointer lvgl_port_start() was handed --
     * NULL ctx/read (i.e. a zeroed touch_dev_t) means "no touch hardware".
     * Built once in lvgl_port_start() and read (never mutated) by
     * touch_read_cb() on every poll. main.c decides which controller's
     * read function/ctx/self_calibrating go in here (NS2009 vs FT6336U,
     * DISPLAY_ST7796_PLAN.md section 7) -- this file only consumes it. */
    touch_dev_t touch_dev;
    screen_idle_t *idle;    /* NULL if no auto-blank integration */

    lv_display_t *lv_disp;
    lv_indev_t *lv_indev;

    /* Tracks the screen_idle on/off flag as of the last flush, so the
     * off->on edge (a wake) can be told apart from "still on" -- see
     * ili9488_flush_cb's comment. Starts true: screen_idle_init leaves
     * screen_on true and this module never blanks anything itself. */
    bool last_screen_on;
} lvgl_port_t;

static lvgl_port_t s_port;

/* Last raw NS2009 sample that produced a press, for
 * lvgl_port_get_last_raw_touch() -- see lvgl_port.h. Written only from
 * touch_read_cb (lvgl_port_task), read only from an LV_EVENT_CLICKED handler
 * (also lvgl_port_task, since that's the only task calling
 * lv_timer_handler()) -- no lock needed, same single-task rule as every
 * other lv_* access in this file. */
static uint16_t s_last_raw_x, s_last_raw_y, s_last_raw_z1;

/* Pull-based touch/input diagnostics (2026-08-21, replacing the push-based
 * TEMP DIAGNOSTIC log lines in this file and kiln_ui.c -- those proved
 * useless on this bench: uart_log_bridge's queue was dropping lines during
 * exactly the boot burst that mattered, so "the enable log never appeared"
 * was indistinguishable from "the enable call never ran". These counters are
 * read on demand over TOUCH_CMD_GET_STATE (uart_bridge.c) instead of pushed
 * as log lines, so a drop anywhere in the log pipeline can no longer hide
 * what happened. All four are monotonic and single-writer:
 *   - s_input_enabled: shadow of the indev's enabled flag. LVGL 9.5 has no
 *     public getter (lv_indev_enable() is set-only, confirmed by reading
 *     components/lvgl/src/indev/lv_indev.h -- do not add one there, that
 *     tree is off limits), so this is kept in lockstep by
 *     lvgl_port_set_input_enabled(), the only place that ever calls
 *     lv_indev_enable(). Written only from whichever task calls that
 *     function (today always lvgl_port_task, via kiln_ui_show()'s
 *     LV_EVENT-adjacent call path); read from the UART bridge task via the
 *     accessor below, which is a single bool read/write, not worth a lock.
 *   - s_touch_read_cb_count: incremented at the top of touch_read_cb(),
 *     i.e. every time LVGL's indev core actually calls this read callback.
 *     Written only from lvgl_port_task (the only caller of
 *     lv_timer_handler(), which is the only thing that can invoke an indev
 *     read callback).
 *   - s_injected_delivered_count: incremented every time touch_read_cb()
 *     hands an injected sample to LVGL (data->point/state set from
 *     s_inject), i.e. a strict superset in cadence of the old
 *     transition-only "injected touch delivered to LVGL" log line -- this
 *     counts every delivery, not just changes, so it also answers "is the
 *     injection path being polled at all" even when the sample never
 *     changes between polls. Same single-writer task as the line above. */
static volatile bool s_input_enabled = true;
static volatile uint32_t s_touch_read_cb_count;
static volatile uint32_t s_injected_delivered_count;

/* Loaded once in lvgl_port_start(). {.calibrated = false} until
 * ui_page_touch_cal.c finishes a calibration pass and calls
 * touch_cal_store_save() -- see touch_read_cb() below for the fallback used
 * before that ever happens. */
static touch_cal_t s_touch_cal;

/* --- lv_tick source -------------------------------------------------------
 * A 1ms esp_timer periodic callback. Runs in the esp_timer task's context,
 * not lvgl_port_task -- lv_tick_inc() is documented as safe to call from any
 * context (it only touches an internal atomic-ish counter LVGL itself
 * serializes), unlike every other lv_* entry point used in this file. */
static void lv_tick_timer_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

/* --- Display flush --------------------------------------------------------
 * ILI9488 has no framebuffer and no read-back (ILI9488.h) -- every flush
 * goes straight to the panel's own GRAM through the streaming blit, which is
 * exactly the shape LV_DISPLAY_RENDER_MODE_PARTIAL wants: a small buffer,
 * flushed as soon as its rectangle is ready, no compositing needed on this
 * side.
 *
 * Skip-while-blanked + wake redraw: screen_idle.h's contract is "blanking
 * paints the frame black and flips a flag; waking is JUST a flag flip --
 * whatever owns the UI is responsible for repainting real content once it
 * sees screen_on go back to true." Before this module existed nothing did
 * that repaint, which is why a touch woke the flag but the glass stayed
 * black. This flush callback is that owner: while screen_idle reports
 * blanked, flushes are skipped (no point drawing under screen_idle's own
 * black paint, and it saves the SPI traffic); the moment it reports on again
 * after having been off, the active screen is invalidated so LVGL redraws
 * everything on the very next cycle instead of only whatever widget next
 * changes. */
/* DISPLAY_ST7796_PLAN.md 9.1: the measurement the plan asks for before any
 * DMA/async work is trusted, made reportable rather than requiring a bench
 * session with a scope. Written only from ili9488_flush_cb() (lvgl_port_task,
 * this file's own single-writer convention -- e.g. s_timer_handler_calls
 * below), read by lvgl_port_get_flush_stats() from any task, same pattern.
 * s_max_flush_us is a running high-water mark since boot, never reset --
 * exactly what "worst chunked flush this board has actually pushed" needs to
 * mean for a decision like 9.2/9.6's. */
static volatile uint32_t s_last_flush_us;
static volatile uint32_t s_min_flush_us = UINT32_MAX;
static volatile uint32_t s_max_flush_us;
static volatile uint32_t s_flush_count;
static volatile uint64_t s_flush_sum_us; /* for the running mean */

void lvgl_port_get_flush_stats(uint32_t *last_us, uint32_t *max_us, uint32_t *count)
{
    if (last_us) *last_us = s_last_flush_us;
    if (max_us) *max_us = s_max_flush_us;
    if (count) *count = s_flush_count;
}

/* Extended form of the above, adding min/mean (HW_ABSTRACTION.md "Still
 * open") -- kept as a second getter rather than widening the original so
 * every existing caller of lvgl_port_get_flush_stats() stays untouched.
 * count == 0 means "never flushed yet"; min_us/mean_us only meaningful once
 * count > 0 (both read back 0 until then). Any out-param may be NULL. */
void lvgl_port_get_flush_stats_ex(uint32_t *last_us, uint32_t *min_us, uint32_t *max_us,
                                  uint32_t *count, uint32_t *mean_us)
{
    uint32_t n = s_flush_count;
    if (last_us) *last_us = s_last_flush_us;
    if (min_us) *min_us = (n == 0) ? 0 : s_min_flush_us;
    if (max_us) *max_us = s_max_flush_us;
    if (count) *count = n;
    if (mean_us) *mean_us = (n == 0) ? 0 : (uint32_t)(s_flush_sum_us / n);
}

/* Single bookkeeping helper so every one of ili9488_flush_cb()'s three
 * completion paths (sync success/failure, async success via
 * ili9488_flush_async_done(), async begin-failure) updates the exact same set
 * of stats the exact same way -- see this file's earlier comment on why these
 * are single-writer, lock-free volatiles. */
static void flush_stats_record(uint32_t flush_us)
{
    s_last_flush_us = flush_us;
    if (flush_us < s_min_flush_us) {
        s_min_flush_us = flush_us;
    }
    if (flush_us > s_max_flush_us) {
        s_max_flush_us = flush_us;
    }
    s_flush_sum_us += flush_us;
    s_flush_count++;
}

#if KILNCTL_SPI_ASYNC_FLUSH
/* DISPLAY_ST7796_PLAN.md 9.6, wired in. A single static instance, not a
 * per-flush allocation: panel_spi.c's ILI9488_blit_data_async() guarantees
 * at most one async flush outstanding at a time (LVGL will not call this
 * flush callback again until lv_display_flush_ready() has fired for the
 * current one, and that only happens from within
 * ili9488_flush_async_done() below), so one struct is all this ever needs
 * to carry across the gap between "chunk handed to the SPI owner" and "its
 * completion callback runs". */
typedef struct {
    lv_display_t *lv_disp;
    int64_t flush_start_us;
} async_flush_ctx_t;
static async_flush_ctx_t s_async_flush_ctx;

/* Fires from the SPI owner task's own thread (never an ISR -- see
 * spi_owner_async_done_cb_t's contract in esp_spi_owner.h and
 * ili9488_blit_async_trampoline()'s comment in panel_spi.c), exactly once,
 * after the last chunk of the flush that queued it has actually completed
 * on the wire -- never after only the first chunk, and never twice. Finishes
 * the same 9.1 stats bookkeeping the synchronous path below does, then
 * calls lv_display_flush_ready(), which LVGL's own threading doc names
 * (alongside lv_tick_inc()) as safe to call from any context. */
static void ili9488_flush_async_done(void *ctx, esp_err_t result)
{
    async_flush_ctx_t *actx = (async_flush_ctx_t *)ctx;

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "async flush failed: %s", esp_err_to_name(result));
    }

    uint32_t flush_us = (uint32_t)((int64_t)hal_time_now_us() - actx->flush_start_us);
    flush_stats_record(flush_us);

    lv_display_flush_ready(actx->lv_disp);
}
#endif /* KILNCTL_SPI_ASYNC_FLUSH */

static void ili9488_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    lvgl_port_t *p = (lvgl_port_t *)lv_display_get_user_data(disp);

    /* 2026-09-04 bench crash (task-watchdog reset, task='lvgl', garbage
     * exc_cause/pc/addr in the captured crash record -- the same "corrupted
     * beyond a trustworthy backtrace" signature as e7b8efc's stack-overflow
     * bug earlier today): this callback used to detect the off->on wake edge
     * ITSELF and invalidate the active screen right here, forcing a redraw.
     * flush_cb runs from INSIDE lv_timer_handler()'s active refresh (LVGL is
     * partway through walking/consuming the invalid-area list when it calls
     * this to blit one of them) -- invalidating an object from there
     * reenters that same refresh machinery mid-walk, which LVGL does not
     * support from a flush callback. Two clean reproductions on an IDLE,
     * non-firing board (touch_inject after a timeout-driven blank, no
     * profile/autotune running, nothing else changed) landed the identical
     * signature both times, immediately after the "screen woke -- forcing a
     * full redraw" log line -- see UI_PLAN.md's Display power section. The
     * fix: the wake edge is now detected and invalidated from
     * lvgl_port_service_idle_wake(), called from lvgl_port_task's own loop
     * BEFORE lv_timer_handler() runs (see below) -- same "outside any active
     * refresh" footing lvgl_port_service_idle_blank()'s ILI9488_clear() call
     * already had. This callback now only reads screen_on to decide whether
     * to skip the blit; it must never again call an lv_* mutator. */
    bool screen_on = true;
    if (p->idle) {
        uint32_t idle_ms = 0;
        if (screen_idle_get_state(p->idle, &screen_on, &idle_ms) != ESP_OK) {
            screen_on = true; /* fail open: draw rather than go permanently dark */
        }
    }

    if (screen_on) {
        uint16_t x = (uint16_t)area->x1;
        uint16_t y = (uint16_t)area->y1;
        uint16_t w = (uint16_t)(area->x2 - area->x1 + 1);
        uint16_t h = (uint16_t)(area->y2 - area->y1 + 1);

        /* DISPLAY_ST7796_PLAN.md 9.1: "measure first" -- flush duration,
         * taken here rather than guessed, so the real number can be read the
         * moment anyone looks instead of waiting on a bench session. Spans
         * exactly the SPI work (begin/data/end), not the screen_idle read or
         * the skip-while-blanked branch above, since those aren't what 9.6's
         * async-flush decision turns on. hal_time_now_us() is a plain
         * volatile read of a hardware counter -- safe to call from
         * lvgl_port_task same as anywhere else, no lock needed for a
         * single-writer stat. */
        int64_t flush_start_us = (int64_t)hal_time_now_us();

#if KILNCTL_SPI_ASYNC_FLUSH
        /* DISPLAY_ST7796_PLAN.md 9.6. ILI9488_blit_begin() stays synchronous
         * (it is a handful of tiny command transfers, not the bulk of flush
         * time -- see 9.1's own measurement rationale); only the pixel
         * payload's LAST chunk, dispatched inside
         * ILI9488_blit_data_async(), is async. lv_display_flush_ready() is
         * NOT called at the bottom of this function in this branch -- it is
         * called exactly once, from ili9488_flush_async_done(), either
         * synchronously (right now, from this task, if blit_data_async hits
         * a validation/early-transfer error) or later from the SPI owner
         * task once the last chunk truly completes. Either way this
         * function must return without touching lv_display_flush_ready()
         * itself, or LVGL sees two calls for one flush. */
        esp_err_t err = ILI9488_blit_begin(p->display, x, y, w, h);
        if (err == ESP_OK) {
            size_t bytes_per_pixel = lv_color_format_get_size(lv_display_get_color_format(disp));
            s_async_flush_ctx.lv_disp = disp;
            s_async_flush_ctx.flush_start_us = flush_start_us;
            err = ILI9488_blit_data_async(p->display, px_map, (size_t)w * (size_t)h * bytes_per_pixel,
                                           ili9488_flush_async_done, &s_async_flush_ctx);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "flush [%u,%u %ux%u] failed: %s", x, y, w, h, esp_err_to_name(err));
            }
            /* ili9488_flush_async_done() -- called either just now,
             * synchronously, or later from the owner task -- owns both the
             * stats bookkeeping and lv_display_flush_ready() from here. */
            return;
        }

        /* ILI9488_blit_begin() itself failed: ILI9488_blit_data_async() was
         * never called, so nothing will invoke ili9488_flush_async_done()
         * for this flush -- finish the bookkeeping and flush_ready here,
         * exactly as the synchronous path below does on any failure. */
        ESP_LOGW(TAG, "flush [%u,%u %ux%u] failed: %s", x, y, w, h, esp_err_to_name(err));
        uint32_t flush_us = (uint32_t)((int64_t)hal_time_now_us() - flush_start_us);
        flush_stats_record(flush_us);
        lv_display_flush_ready(disp);
        return;
#else
        esp_err_t err = ILI9488_blit_begin(p->display, x, y, w, h);
        if (err == ESP_OK) {
            /* Bytes/pixel from LVGL's own color format, not a hard-coded 2 --
             * DISPLAY_ST7796_PLAN.md Sec.6 Step 2: this constant is the
             * panel's wire format on the *decode* side (ILI9488_blit_data
             * still only accepts RGB565 in), so it must track what
             * lv_display_set_color_format() was actually set to, not what one
             * particular panel happens to want. RGB565 -> 2, unchanged from
             * today. */
            size_t bytes_per_pixel = lv_color_format_get_size(lv_display_get_color_format(disp));
            err = ILI9488_blit_data(p->display, px_map, (size_t)w * (size_t)h * bytes_per_pixel);
            esp_err_t end_err = ILI9488_blit_end(p->display);
            if (err == ESP_OK) err = end_err;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "flush [%u,%u %ux%u] failed: %s", x, y, w, h, esp_err_to_name(err));
        }

        uint32_t flush_us = (uint32_t)((int64_t)hal_time_now_us() - flush_start_us);
        flush_stats_record(flush_us);
#endif /* KILNCTL_SPI_ASYNC_FLUSH */
    }

    lv_display_flush_ready(disp);
}

/* --- Touch injection (TOUCH_CMD_INJECT) ------------------------------------
 * uart_bridge.c's UART touch task calls lvgl_port_inject_touch() below with
 * whatever x/y/pressed came off the wire. That task is NOT lvgl_port_task, so
 * it must never call an lv_* function (see this file's top comment) -- this
 * struct plus a short-held mutex is the hand-off, same shape as
 * thermo_owner.c/kiln_io_owner.c's "one owner task consumes a small state
 * struct another task writes" convention and screen_idle.h's own lock.
 * touch_read_cb() (lvgl_port_task, i.e. the one legal LVGL caller) is the
 * only consumer.
 *
 * Coordinate space: SCREEN PIXELS, entering the pipeline downstream of the
 * NS2009 calibration transform (touch_cal_apply() / the swap-invert fallback
 * a few lines below) rather than upstream of it as raw ADC counts. A real
 * press starts as raw ADC counts and only becomes a pixel coordinate after
 * that per-board fit runs; an injected press already IS the coordinate a test
 * harness wants hit-tested (e.g. "the Menu button center is (240,160)" read
 * straight off ui_page_home.c's lv_obj_set_pos/size calls), and making it
 * detour through the calibration transform first would require either
 * inverting that transform (fragile, and pointless extra work) or shipping a
 * synthetic ADC count that happens to map back to the intended pixel (equally
 * fragile, and couples the test harness to whichever board's calibration
 * happens to be loaded). Entering post-transform is also what keeps injected
 * taps working identically on a board that has never been calibrated at all
 * (s_touch_cal.calibrated == false) -- see touch_cal_store.h.
 *
 * Press/release lifecycle: a press stays "pending" (returned by touch_read_cb
 * on every poll, same coordinates) until an explicit release arrives on the
 * wire, exactly mirroring how a finger held down on real glass reads PRESSED
 * on every NS2009 poll until it lifts -- LVGL's own click/long-press/drag
 * state machine needs that repetition, not a single edge, to do the right
 * thing. One press message followed later by one release message therefore
 * produces exactly one LVGL click, the same as a real tap.
 *
 * Auto-release safeguard: TOUCH_INJECT_AUTORELEASE_MS bounds how long a press
 * can be held with no matching release or refresh before this module lets go
 * of it on its own. A test script that injects a press and then crashes,
 * disconconnects, or simply forgets the matching release would otherwise wedge
 * the UI in a permanent PRESSED state -- worse than doing nothing, since it
 * also blocks real NS2009 touches (see touch_read_cb: an active injection
 * takes priority over the physical read every poll). 5000ms is chosen to be
 * far longer than any legitimate LVGL interaction this UI performs (clicks
 * resolve in well under a second; the longest deliberate hold anywhere in the
 * UI is the config grid's drag-to-scroll gesture, which is a handful of
 * discrete move points a test harness sends within milliseconds of each
 * other, not a multi-second hold) while still being short enough that a
 * forgotten release recovers on its own well within the length of a manual
 * bench-test pass rather than requiring a reboot. */
typedef struct {
    SemaphoreHandle_t lock;
    bool pending;   /* an unreleased press or an unconsumed release is waiting */
    bool pressed;
    int32_t x, y;
    TickType_t last_update_tick;
    /* 2026-09-24 tap-swallow observability: a monotonically increasing id
     * assigned to every PRESS injection (a release keeps the id of the press
     * it ends -- releases are never themselves swallow candidates, see
     * screen_idle.h's touch_held comment). touch_read_cb() runs in a
     * different task than the caller of lvgl_port_inject_touch() and only
     * learns the real screen_idle_touch_swallow() verdict once it actually
     * delivers this press on its own ~30ms LVGL poll -- this seq is the
     * handoff kiln_ui_click_by_name() polls against (see
     * lvgl_port_get_inject_verdict()) instead of guessing from whether the
     * page changed. 0 is never assigned (reserved as "no verdict recorded
     * yet" in s_inject_verdict_seq below). */
    uint32_t seq;
} touch_inject_t;

static touch_inject_t s_inject;
static uint32_t s_inject_seq_counter; /* guarded by s_inject.lock, same as the struct above */

/* The most recently recorded swallow verdict, written ONLY by touch_read_cb()
 * (lvgl_port_task) under s_inject.lock, read by lvgl_port_get_inject_verdict()
 * from any task. Only ever holds the LATEST press's verdict -- a caller must
 * check seq against the id lvgl_port_inject_touch() returned it, not merely
 * "is a verdict present", since a second injection could otherwise overwrite
 * the first's verdict before a slow poller reads it (see that function's
 * bounded-timeout doc comment for why callers are expected to poll promptly). */
static uint32_t s_inject_verdict_seq; /* 0 = none recorded yet */
static bool s_inject_verdict_swallowed;

/* Last injected sample actually written to the log, used by touch_read_cb() to
 * log transitions only (see the rationale at that call site). Deliberately NOT
 * part of touch_inject_t and deliberately not guarded by s_inject.lock: these
 * are read and written only by touch_read_cb(), i.e. only ever from
 * lvgl_port_task, so they have a single owner and taking the lock for them
 * would be pure overhead on the hottest path in this file. */
static bool s_inject_logged_valid;
static bool s_inject_logged_pressed;
static int32_t s_inject_logged_x;
static int32_t s_inject_logged_y;

#define TOUCH_INJECT_LOCK_TIMEOUT_MS 1000u
#define TOUCH_INJECT_AUTORELEASE_MS  5000u

static bool touch_inject_lock(void)
{
    return xSemaphoreTake(s_inject.lock, pdMS_TO_TICKS(TOUCH_INJECT_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static void touch_inject_unlock(void)
{
    xSemaphoreGive(s_inject.lock);
}

uint32_t lvgl_port_inject_touch(uint16_t x, uint16_t y, bool pressed)
{
    /* Both early returns below are silent by design (a NULL lock means
     * lvgl_port_start() hasn't run yet; a lock timeout means touch_read_cb
     * is wedged on the same mutex, itself a symptom worth its own
     * investigation, not this function's). Whether a write here actually
     * reaches LVGL is now provable from the far side, on demand, via
     * lvgl_port_get_touch_diag()'s injected_delivered_count -- see that
     * function's declaration comment -- so this no longer needs its own
     * per-call log to answer the question the removed TEMP DIAGNOSTIC WARNs
     * existed for. 0 is returned on either failure path -- never a valid seq
     * (see s_inject_seq_counter's declaration comment) -- so a caller polling
     * lvgl_port_get_inject_verdict() with this return value simply never
     * matches, which is the correct "no verdict, this never even queued"
     * answer. */
    if (!s_inject.lock) {
        return 0;
    }
    if (!touch_inject_lock()) {
        return 0;
    }

    /* Concurrent-caller note (2026-09-24 review): this is a single-slot
     * hand-off, not a queue. TOUCH_CMD_INJECT (uart_bridge.c, task 13) and
     * kiln_ui_click_by_name() (task 14, UI_TEST) both reach this function
     * from their own independent UART tasks, so a press from one arriving
     * between another's still-unconsumed press and touch_read_cb()'s next
     * poll overwrites its x/y/pressed fields outright -- the earlier press's
     * coordinates never reach touch_read_cb() at all. This is not a torn
     * read (the lock keeps `s_inject` internally consistent) and it is not
     * silently misreported: the overwriting call mints a NEW seq, so the
     * first call's lvgl_port_get_inject_verdict(press_seq, ...) can never
     * find a match (seq-match check below) and its caller times out to
     * KILN_UI_CLICK_VERDICT_UNKNOWN rather than a false verdict. Turning
     * this into a real per-press queue would need a second FreeRTOS
     * primitive threaded through touch_read_cb()'s existing lock ordering
     * for no benefit either caller of this file currently needs (a bench
     * harness does not run TOUCH_CMD_INJECT and click_by_name() against the
     * same board at once), so it is left as a documented race rather than
     * "fixed" here. */
    s_inject.pending = true;
    s_inject.pressed = pressed;
    s_inject.x = (int32_t)x;
    s_inject.y = (int32_t)y;
    s_inject.last_update_tick = xTaskGetTickCount();
    /* A release keeps riding the press's own seq (nothing waits on a
     * release's verdict -- see the struct comment) rather than minting a new
     * one, so s_inject_seq_counter only ever advances on a press. */
    if (pressed) {
        if (++s_inject_seq_counter == 0) {
            s_inject_seq_counter = 1; /* skip the reserved 0 on the rare wraparound */
        }
        s_inject.seq = s_inject_seq_counter;
    }
    uint32_t seq = s_inject.seq;

    touch_inject_unlock();
    return seq;
}

/* --- Touch input device -----------------------------------------------
 * touch_dev_t (touch_dev.h) reader once this module starts (see
 * lvgl_port.h). A real press is forwarded into screen_idle_inject_touch() so
 * screen_idle's idle timer / wake logic works exactly as it does for a
 * UART-injected touch -- screen_idle was built to not care which source a
 * touch came from (screen_idle.h/.c), and this keeps it that way. */

/* Shared by both the physical-touch and injected-touch paths below --
 * originally this lived only in the physical path, which meant
 * touch_inject() (and therefore touch_log_tap_targets()/this whole tool
 * chain) could never exercise or verify it: TOUCH_CMD_INJECT's early
 * `return` in touch_read_cb() skipped straight past it. That made the
 * back-button overlap bug (adjacent topbar icons' extended click areas
 * overlapping, resolved by LVGL's z-order instead of nearest-center)
 * invisible to synthetic taps even after ui_topbar.c started registering
 * the icon row as a touch group -- a real finger got the fix, an injected
 * tap at the same coordinate did not. Factored out and called from both
 * places so injected touches see exactly the same arbitration a physical
 * touch does. */
static void apply_touch_group_arbitration(lv_indev_data_t *data)
{
    if (!ui_theme_touch_groups_active()) {
        return;
    }
    lv_point_t raw_point = { .x = data->point.x, .y = data->point.y };
    lv_obj_t *default_hit = lv_indev_search_obj(lv_screen_active(), &raw_point);
    lv_obj_t *target = ui_theme_resolve_touch_target(default_hit, raw_point);
    if (target && target != default_hit) {
        lv_area_t coords;
        lv_obj_get_coords(target, &coords);
        data->point.x = (coords.x1 + coords.x2) / 2;
        data->point.y = (coords.y1 + coords.y2) / 2;
    }
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    /* Pull-based replacement for the old one-shot "first call reached" log
     * -- see s_touch_read_cb_count's declaration comment above. */
    s_touch_read_cb_count++;

    lvgl_port_t *p = (lvgl_port_t *)lv_indev_get_user_data(indev);

    /* An injected touch always takes priority over the physical NS2009 read
     * for this poll -- see the "Touch injection" block comment above for why
     * that's the right call (and its auto-release safeguard for why this
     * can't wedge the UI against real touch forever). Injected coordinates
     * are already screen pixels (see that same comment), so they go straight
     * to data->point with no calibration transform. */
    if (touch_inject_lock()) {
        bool have_injection = s_inject.pending;
        bool inject_pressed = s_inject.pressed;
        int32_t inject_x = s_inject.x;
        int32_t inject_y = s_inject.y;
        uint32_t inject_seq = s_inject.seq;
        TickType_t elapsed = xTaskGetTickCount() - s_inject.last_update_tick;

        if (have_injection && inject_pressed && elapsed > pdMS_TO_TICKS(TOUCH_INJECT_AUTORELEASE_MS)) {
            /* Safeguard tripped: synthesize the release the host never sent
             * and stop overriding the physical path after this poll. */
            ESP_LOGW(TAG, "injected touch held > %ums with no release -- auto-releasing",
                     (unsigned)TOUCH_INJECT_AUTORELEASE_MS);
            inject_pressed = false;
            s_inject.pressed = false;
            s_inject.pending = false;
        } else if (have_injection && !inject_pressed) {
            /* An explicit release: deliver it once, then let the physical
             * path resume on the next poll. */
            s_inject.pending = false;
        }
        touch_inject_unlock();

        if (have_injection) {
            s_injected_delivered_count++;
            data->point.x = inject_x;
            data->point.y = inject_y;
            data->state = inject_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
            /* This is the one piece of evidence, short of a framebuffer
             * readback this panel doesn't support, that a TOUCH_CMD_INJECT
             * frame actually reached LVGL's input pipeline rather than
             * stopping at screen_idle like it did before this feature
             * existed. It is worth keeping at INFO for exactly that reason --
             * but only on a CHANGE.
             *
             * 2026-08-20, found live on the bench: an earlier version logged
             * on every delivery, on the assumption that a harness "sends a
             * handful of these per gesture". That assumption was wrong. A
             * latched press is re-reported on every LVGL poll (~30ms) by
             * design -- that repetition is what LVGL's click/drag state
             * machine needs -- so logging each delivery emitted ~33 lines a
             * second and produced exactly the flood this codebase has now
             * fixed twice elsewhere (NS2009's poll log, uart_log_bridge's own
             * retry storm): the ring filled, uart_log_bridge reported
             * "log line(s) dropped (queue full)", and the dropped lines
             * included the RELEASED line this message exists to show. A log
             * that destroys the evidence it was added to capture is worse
             * than no log.
             *
             * Logging only on a transition -- press, each drag point, release
             * -- yields the "handful per gesture" the original intent
             * described, with none of the flood. */
            if (!s_inject_logged_valid || s_inject_logged_pressed != inject_pressed ||
                s_inject_logged_x != inject_x || s_inject_logged_y != inject_y) {
                ESP_LOGI(TAG, "injected touch delivered to LVGL: (%ld,%ld) %s", (long)inject_x,
                         (long)inject_y, inject_pressed ? "PRESSED" : "RELEASED");
                s_inject_logged_valid = true;
                s_inject_logged_pressed = inject_pressed;
                s_inject_logged_x = inject_x;
                s_inject_logged_y = inject_y;
            }
            if (p->idle) {
                // 2026-09-04 owner request (docs/UI_PLAN.md "Display power",
                // rule 3/4): screen_idle_touch_swallow() replaces the plain
                // wake-only screen_idle_inject_touch() here -- it does the
                // same wake/idle-timer bookkeeping PLUS runs
                // display_power_policy_step() and reports whether THIS
                // touch must be swallowed (the display was off, or an
                // error-hold dismissal). Swallowing means overwriting
                // data->state back to RELEASED right after it was set above
                // -- LVGL then hit-tests nothing for this poll, exactly the
                // same as if no press had happened, while screen_idle still
                // saw it and already woke/dismissed. Applies to an injected
                // touch exactly like a physical one (see the physical
                // branch below) -- a test harness driving the UI through
                // TOUCH_CMD_INJECT must see the identical wake-swallows
                // behaviour a real finger would, or its results would not
                // mean what they claim to. */
                bool swallow = false;
                screen_idle_touch_swallow(p->idle, (uint16_t)inject_x, (uint16_t)inject_y, inject_pressed,
                                          &swallow);
                if (swallow) {
                    data->state = LV_INDEV_STATE_RELEASED;
                }
                /* Record the verdict for THIS press so kiln_ui_click_by_name()
                 * (a different task, waiting on lvgl_port_inject_touch()'s
                 * returned seq) can learn it instead of guessing from whether
                 * the page changed -- see s_inject_verdict_seq's declaration
                 * comment. Only a press has a swallow decision worth
                 * recording (screen_idle_touch_swallow() always reports
                 * *out_swallow=false for a release); a release is left alone
                 * here so it never clobbers the press's own verdict with a
                 * stale "false". */
                if (inject_pressed && touch_inject_lock()) {
                    s_inject_verdict_seq = inject_seq;
                    s_inject_verdict_swallowed = swallow;
                    touch_inject_unlock();
                }
            } else if (inject_pressed && touch_inject_lock()) {
                /* No screen_idle wired up at all (p->idle == NULL, e.g. a
                 * host/sim build with no auto-blank integration -- see
                 * lvgl_port_start()'s doc comment): nothing can ever be
                 * swallowed, so record that plainly rather than leaving a
                 * waiter to time out for no reason. */
                s_inject_verdict_seq = inject_seq;
                s_inject_verdict_swallowed = false;
                touch_inject_unlock();
            }
            if (inject_pressed && data->state == LV_INDEV_STATE_PRESSED) {
                apply_touch_group_arbitration(data);
            }
            return;
        }
    }

    if (!p->touch_dev.read) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    bool pressed = false;
    uint16_t raw_x = 0, raw_y = 0, raw_z1 = 0;
    esp_err_t err = touch_dev_read(&p->touch_dev, &pressed, &raw_x, &raw_y, &raw_z1);
    if (err != ESP_OK || !pressed) {
        // 2026-09-04 (docs/UI_PLAN.md "Display power"): this is the ONLY
        // place a physical release is ever observed (touch_dev_read simply
        // stops reporting `pressed`, unlike the injected path's explicit
        // pressed=false frame) -- screen_idle's press-edge tracker
        // (screen_idle.h's touch_held) needs this notified or a physical
        // touch's release would never clear it, and every later press would
        // be mistaken for a "repeat" of the first (wrongly reusing that
        // first press's swallow verdict forever). p->idle is intentionally
        // allowed to be NULL (no auto-blank integration -- see
        // lvgl_port_start()'s doc comment); this is then simply a no-op.
        if (p->idle) {
            bool swallow_unused;
            screen_idle_touch_swallow(p->idle, 0, 0, false, &swallow_unused);
        }
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    s_last_raw_x = raw_x;
    s_last_raw_y = raw_y;
    s_last_raw_z1 = raw_z1;

    uint16_t width = 0, height = 0;
    ILI9488_get_dimensions(p->display, &width, &height);

    int32_t px, py;
    if (touch_dev_use_calibrated_fit(&p->touch_dev, s_touch_cal.calibrated)) {
        /* Real per-board fit -- see touch_cal_store.h. This is what every
         * page's hit-testing runs on once calibration has completed. Never
         * reached by a self_calibrating device (see below) -- its raw
         * samples are already panel coordinates, and running them through a
         * fit meant for uncalibrated ADC counts would double-apply a
         * correction the controller already did on-chip. */
        touch_cal_apply(&s_touch_cal, raw_x, raw_y, width, height, &px, &py);
    } else {
        /* Two callers land here, sharing the same rotation/axis-swap
         * mapping (touch_dev.h's touch_dev_map_uncalibrated) but for
         * different reasons:
         *   - NS2009, before this board has ever been calibrated: a
         *     bootstrap-only fallback, the Kconfig swap/invert guess. Known
         *     inaccurate (see touch_cal_store.h's header comment) -- good
         *     enough only for ui_page_touch_cal.c's full-screen "any press
         *     counts" capture during the forced first-run calibration flow,
         *     not for real button hit-testing. raw_x_max/raw_y_max are both
         *     NS2009_ADC_MAX, matching the old touch_raw_to_px() exactly.
         *   - Any self_calibrating device (FT6336U.h; unreached today --
         *     see the lvgl_port_t.touch_dev field comment): this is its
         *     ONLY mapping, permanently, not a bootstrap stand-in for a fit
         *     that will later replace it -- touch_cal_store.h's affine fit
         *     never runs on this device's output at all. Its raw_x/raw_y
         *     are already panel coordinates, so raw_x_max/raw_y_max are the
         *     panel's own extents: touch_dev_axis_to_px's scale step
         *     becomes identity (clamp + optional invert only), leaving just
         *     the rotation/axis-swap correction the module's physical
         *     mounting needs. touch_dev.h documents raw_x_max/raw_y_max as
         *     each axis's PRE-swap raw ceiling (touch_dev_map_uncalibrated
         *     swaps the maxes right along with the axes when swap_xy is
         *     set) -- so when TOUCH_CAL_SWAP_XY is set, raw_x's native
         *     range runs along the screen's height, not its width, and
         *     raw_y_max must be the width. Assigning width to raw_x_max
         *     and height to raw_y_max unconditionally (as if already
         *     post-swap) is wrong the moment swap_xy is set: this board's
         *     only attached controller (NS2009) never reaches this branch
         *     with self_calibrating true, so it stayed silent, but it would
         *     mis-scale every touch on a swapped FT6336U panel. The
         *     assignment itself is pulled out into touch_dev_uncalibrated_max()
         *     so it is host-testable rather than living only in this
         *     ESP-IDF-dependent function -- see test_touch_dev.c. */
        /* Per-CONTROLLER touch mapping (touch_dev.h's touch_dev_t.swap_xy et
         * al, DISPLAY_ST7796_PLAN.md section 7, 2026-09-04). REVISED
         * 2026-09-04, same day: this used to live on panel_desc_t instead,
         * keyed by whichever display panel was selected -- broken the
         * moment panel and touch controller became independently
         * selectable (ILI9488 driver for pixels, FT6336U still the only
         * touch chip physically wired up, during this session's own
         * color-regression bisect). Sourced from main.c, which sets it from
         * the matching per-controller Kconfig knobs (TOUCH_CAL_* for
         * NS2009, TOUCH_CAP_* for FT6336U) when it builds the touch_dev_t
         * -- so it now tracks the actual controller, not the panel. */
        bool swap_xy = p->touch_dev.swap_xy;
        bool invert_x = p->touch_dev.invert_x;
        bool invert_y = p->touch_dev.invert_y;

        uint16_t raw_x_max = 0, raw_y_max = 0;
        touch_dev_uncalibrated_max(p->touch_dev.self_calibrating, swap_xy, width, height,
                                    NS2009_ADC_MAX, &raw_x_max, &raw_y_max);
        touch_dev_map_uncalibrated(raw_x, raw_y, raw_x_max, raw_y_max, width, height, swap_xy,
                                    invert_x, invert_y, &px, &py);
    }

    data->point.x = px;
    data->point.y = py;
    data->state = LV_INDEV_STATE_PRESSED;

    // 2026-09-04 owner request (docs/UI_PLAN.md "Display power", rule 3/4):
    // report this press to screen_idle BEFORE arbitration/hit-testing --
    // screen_idle_touch_swallow() both wakes a blanked/error-held screen and
    // (on the press edge) returns whether this touch must be swallowed. On
    // swallow, revert to RELEASED and skip arbitration entirely: this exact
    // press must never reach any widget underneath, on this or any later
    // poll of the same held press (see screen_idle.h's touch_held comment --
    // the verdict is cached for the whole press/release gesture, not just
    // this one sample). This is the fix for the gap lvgl_port.h's own
    // header comment used to document ("neither path checks screen_idle's
    // blanked/awake state before hit-testing ... a wake tap also activates
    // whatever it lands on underneath, intentionally") -- that was correct
    // for the old "blank is just a black paint, nothing to protect"
    // feature, but is exactly the bug this pass fixes for the new policy's
    // wake-only-does-not-act rule.
    if (p->idle) {
        bool swallow = false;
        screen_idle_touch_swallow(p->idle, (uint16_t)px, (uint16_t)py, true, &swallow);
        if (swallow) {
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
    }

    /* Touch-group arbitration -- TODO.md 10.4's open item, ui_theme.h's
     * "Touch-group arbitration" block comment has the full design writeup.
     * ui_theme_touch_groups_active() is a single flag check, so this costs
     * nothing on every build that never calls
     * ui_theme_register_touch_group() (true of every page today -- no dense
     * grid layout exists yet). When a group IS registered: re-run LVGL's own
     * public lv_indev_search_obj() against the raw point to get the same
     * answer LVGL's real pipeline is about to compute anyway, hand it to
     * ui_theme_resolve_touch_target() to arbitrate within that widget's
     * group (a no-op if the widget isn't in a group), and if arbitration
     * picked a different widget, overwrite data->point with THAT widget's
     * own center. LVGL's indev core (lv_indev.c's _lv_indev_read()) copies
     * data->point into indev->pointer.act_point right after this callback
     * returns, and indev_proc_press() re-resolves the press from that exact
     * point through the same public search path -- so rewriting the point
     * here is sufficient to redirect the press; nothing about LVGL's own
     * press/release/drag state machine needs to be touched or duplicated. */
    apply_touch_group_arbitration(data);
    // screen_idle was already notified of this press above (before
    // arbitration), which is also where the swallow decision that could
    // have returned early from this function was made -- no second
    // notification needed here.
}

/* --- Task: the only thing that ever calls lv_timer_handler() ----------
 * LVGL's own return value from lv_timer_handler() is how long it's safe to
 * sleep before the next call is needed -- honoured directly rather than a
 * fixed poll period, same idea as every other "sleep until there's real
 * work" task in this codebase. */
/* ONE-OFF root-cause probe (2026-08-21): touch_read_cb_count staying at 0
 * forever with the indev confirmed to exist (lvgl_port_indev_exists()) means
 * either lv_timer_handler() itself is never being called (this task never
 * runs, or is stuck before its first iteration) or it runs but skips the
 * indev's read timer specifically. This counter settles the first half:
 * incremented on every loop iteration, read back the same way as the other
 * touch diagnostics. */
static volatile uint32_t s_timer_handler_calls;

void lvgl_port_get_timer_handler_calls(uint32_t *calls)
{
    if (calls) *calls = s_timer_handler_calls;
}

/* screen_idle_task (screen_idle.c) requests a blank purely by flipping its
 * own screen_on flag to false -- it does not touch the display. This is the
 * other half of that handoff: on the on->off edge, this (the sole legal
 * draw-call owner, DISPLAY_ST7796_PLAN.md section 8) performs the actual
 * ILI9488_clear(). The off->on "wake" edge is handled the other way around,
 * inside ili9488_flush_cb -- it only needs to invalidate the active screen
 * so the next normal flush redraws it, no explicit clear required there.
 * Runs every loop iteration (not just on flush), because a blanked screen
 * stops producing flushes -- nothing else would ever notice the edge. */
static void lvgl_port_service_idle_blank(void)
{
    if (!s_port.idle) return;

    bool screen_on = true;
    uint32_t idle_ms = 0;
    if (screen_idle_get_state(s_port.idle, &screen_on, &idle_ms) != ESP_OK) return;

    if (!screen_on && s_port.last_screen_on) {
        esp_err_t err = ILI9488_clear(s_port.display, 0x0000);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "blank (ILI9488_clear black) failed: %s -- will retry next loop",
                     esp_err_to_name(err));
            return; /* leave last_screen_on true so this is retried */
        }
        s_port.last_screen_on = false;
        ESP_LOGI(TAG, "screen blanked");
    }
}

/* The other half of the on->off handoff above, and the fix for the
 * 2026-09-04 bench task-watchdog crash documented in ili9488_flush_cb()'s
 * comment: the off->on wake edge used to be detected AND invalidated from
 * inside the flush callback, which reenters LVGL's refresh machinery
 * mid-walk. Detecting it here instead -- called from lvgl_port_task's loop
 * before lv_timer_handler() runs, i.e. with no refresh in progress -- is the
 * same safe footing lvgl_port_service_idle_blank()'s ILI9488_clear() call
 * already stood on. last_screen_on is this function's and
 * lvgl_port_service_idle_blank()'s alone to write now; ili9488_flush_cb()
 * only ever reads screen_on. */
static void lvgl_port_service_idle_wake(void)
{
    if (!s_port.idle) return;

    bool screen_on = true;
    uint32_t idle_ms = 0;
    if (screen_idle_get_state(s_port.idle, &screen_on, &idle_ms) != ESP_OK) return;

    if (screen_on && !s_port.last_screen_on) {
        ESP_LOGI(TAG, "screen woke -- forcing a full redraw");
        /* All THREE roots, not just the active screen. The blank is an
         * ILI9488_clear() straight to panel GRAM (lvgl_port_service_idle_
         * blank() above) -- it erases every pixel on the glass, including
         * whatever is drawn on LVGL's screen-independent overlay layers.
         * But every modal this UI puts up lives on lv_layer_top(), NOT
         * under the active screen: ui_confirm.c's lv_msgbox_create(NULL),
         * ui_num_pad.c's keypad and ui_page_network.c's connect modal --
         * kiln_ui.c's log_all_tap_targets() documents exactly this and
         * walks all three roots for the same reason. Invalidating only
         * lv_screen_active() therefore redrew the page underneath and left
         * the open dialog missing from the glass until something else
         * happened to dirty it: a blank-then-wake with a confirm dialog up
         * repainted the page WITHOUT the dialog, while LVGL still had the
         * dialog focused and swallowing input -- a UI that looks idle but
         * does not respond to the page beneath it. lv_layer_sys() is
         * included for the same "nothing uses it yet, but it is the same
         * kind of root" reason kiln_ui.c gives.
         *
         * Same context rule as the rest of this function: it runs from
         * lvgl_port_task's loop BEFORE lv_timer_handler(), i.e. with no
         * refresh in progress -- these are still lv_* MUTATORS and must
         * never migrate back into ili9488_flush_cb() (see that function's
         * 2026-09-04 crash comment). */
        lv_obj_invalidate(lv_screen_active());
        lv_obj_t *top = lv_layer_top();
        if (top) lv_obj_invalidate(top);
        lv_obj_t *sys = lv_layer_sys();
        if (sys) lv_obj_invalidate(sys);
        s_port.last_screen_on = true;
    }
}

/* Set by lvgl_port_request_tap_dump() (any task), cleared here once
 * serviced. A plain bool is enough: the only write from outside this task
 * is "set true", this task only ever reads it and (on that one path) writes
 * it back to false, so there is no read-modify-write race to protect --
 * same single-writer-per-field reasoning lvgl_port_get_touch_diag()'s header
 * comment already applies to this module's other cross-task counters.
 * volatile so the compiler doesn't hoist the check-and-clear out of the
 * loop. */
static volatile bool s_tap_dump_requested;

void lvgl_port_request_tap_dump(void)
{
    s_tap_dump_requested = true;
}

/* --- Cross-task tap-target collection (kiln_ui_collect_tap_targets()) ------
 * uart_bridge_ui_test.c's UI_TEST_CMD_LIST_TAP_TARGETS handler and kiln_ui.c's
 * own kiln_ui_click_by_name() used to call kiln_ui_collect_tap_targets()
 * directly from ui_test_bridge_task, walking the live LVGL tree from a task
 * other than lvgl_port_task -- the same bug class TOUCH_CMD_LOG_TAP_TARGETS
 * hit (IllegalInstruction panic, 2026-09-19, fixed by
 * lvgl_port_request_tap_dump() above) and one kiln_ui.h's own doc comment on
 * kiln_ui_collect_tap_targets() named as a known, unresolved gap at the time
 * of that fix. This mirrors that fix's shape but needs an actual reply --
 * the caller wants the target list back, not a fire-and-forget log dump --
 * so a bounded-wait completion semaphore is added rather than reusing the
 * plain boolean flag alone.
 *
 * The result buffer (s_ui_walk_targets) is a static array OWNED by this
 * file, in PSRAM (EXT_RAM_BSS_ATTR, so it costs nothing against the
 * internal-DRAM .dram0.bss ceiling this file's other statics share), not a
 * pointer into the caller's own stack frame: a caller that gives up on the
 * bounded wait below (lvgl_port_task itself is wedged, or just slow) must
 * never leave lvgl_port_task free to keep writing into a stack frame that
 * has since been reused for something else. Results are copied into the
 * caller's `out` array only after a confirmed, non-timed-out completion.
 *
 * s_ui_walk.lock serializes callers (this module has exactly two: the
 * UI_TEST bridge task and kiln_ui_click_by_name(), and per that function's
 * own doc comment only one UART command runs at a time, so contention here
 * is not expected in practice, but the lock makes it safe by construction
 * rather than by convention). s_ui_walk.done is a binary semaphore lvgl_
 * port_task gives exactly once per serviced request. Each request carries a
 * sequence number and a caller accepts a completion only when served_seq
 * matches its own, copying out under s_ui_walk.data (which lvgl_port_task
 * holds across the whole walk), so a request abandoned to a timeout -- even
 * one whose walk is still in flight when the next request is issued -- can
 * never be mistaken for, or tear, a later request's result. */
#define UI_WALK_MAX_TARGETS 32
#define UI_WALK_LOCK_TIMEOUT_MS 1000u
#define UI_WALK_WAIT_TIMEOUT_MS 300u /* several lvgl_port_task poll periods
                                      * (~50ms typical, see lvgl_port_task()
                                      * below) -- comfortably under ui_test_
                                      * client.py's DEFAULT_REPLY_TIMEOUT_S
                                      * (2.0s), with margin left over for the
                                      * reply's own UART encode/send time. */
typedef struct {
    SemaphoreHandle_t lock; /* serializes requesters; held across one whole
                             * request/wait/copy-out cycle. Never taken by
                             * lvgl_port_task. */
    SemaphoreHandle_t data; /* guards every field below AND s_ui_walk_targets.
                             * Held by lvgl_port_task across one whole walk
                             * (taken with a zero timeout -- a busy lock just
                             * defers the walk to the next loop iteration, so
                             * lvgl_port_task never blocks on a requester) and
                             * by a requester while issuing a request and
                             * across its copy-out. Lock order: lock -> data. */
    SemaphoreHandle_t done; /* given by lvgl_port_task once a request is
                             * serviced; taken by the requester */
    bool requested;
    uint32_t req_seq;    /* bumped per issued request */
    uint32_t served_seq; /* req_seq value the buffer contents answer */
    size_t out_max;
    size_t out_count;
    bool out_truncated;
    int32_t out_disp_w; /* display resolution, read on lvgl_port_task during
                         * the same walk (2026-09-25 -- see
                         * lvgl_port_collect_tap_targets()'s doc comment) */
    int32_t out_disp_h;
} ui_walk_req_t;

static ui_walk_req_t s_ui_walk;
/* lvgl_port_task's handle, for lvgl_port_collect_tap_targets()'s
 * self-dispatch guard. NULL until the task is created. */
static TaskHandle_t s_ui_walk_owner_task;
static EXT_RAM_BSS_ATTR kiln_ui_tap_target_t s_ui_walk_targets[UI_WALK_MAX_TARGETS];

size_t lvgl_port_collect_tap_targets(kiln_ui_tap_target_t *out, size_t max, bool *truncated,
                                      int32_t *out_disp_w, int32_t *out_disp_h)
{
    if (truncated) {
        *truncated = false;
    }
    if (!out || max == 0) {
        return 0;
    }
    /* Self-dispatch guard: a call from lvgl_port_task itself would wait the
     * full window on a request only this same task can service. The walk is
     * already on the right task there, so do it directly -- and this task IS
     * the one legal LVGL caller, so reading the resolution here is safe. */
    if (s_ui_walk_owner_task != NULL && xTaskGetCurrentTaskHandle() == s_ui_walk_owner_task) {
        size_t n = kiln_ui_collect_tap_targets(out, max, truncated);
        if (out_disp_w) {
            *out_disp_w = lv_display_get_horizontal_resolution(NULL);
        }
        if (out_disp_h) {
            *out_disp_h = lv_display_get_vertical_resolution(NULL);
        }
        return n;
    }
    if (!s_ui_walk.lock || !s_ui_walk.data || !s_ui_walk.done) {
        if (truncated) {
            *truncated = true;
        }
        return 0;
    }
    if (max > UI_WALK_MAX_TARGETS) {
        max = UI_WALK_MAX_TARGETS;
    }
    if (xSemaphoreTake(s_ui_walk.lock, pdMS_TO_TICKS(UI_WALK_LOCK_TIMEOUT_MS)) != pdTRUE) {
        /* Another caller is mid-request and this one timed out waiting its
         * turn -- report exactly the same shape as a dispatch timeout below
         * (empty, truncated) rather than a distinct failure mode; either way
         * the caller got no list and must not treat it as "zero targets". */
        if (truncated) {
            *truncated = true;
        }
        return 0;
    }

    size_t n = 0;
    bool was_truncated = true;
    const TickType_t start = xTaskGetTickCount();
    const TickType_t window = pdMS_TO_TICKS(UI_WALK_WAIT_TIMEOUT_MS);

    /* Drain a stale completion left by an earlier, abandoned (timed-out)
     * request. Not load-bearing on its own -- the sequence check below is
     * what rejects a late completion for an older request, including one
     * whose walk was already in flight when this request was issued -- but
     * it saves a spurious wakeup. */
    (void)xSemaphoreTake(s_ui_walk.done, 0);

    if (xSemaphoreTake(s_ui_walk.data, window) == pdTRUE) {
        const uint32_t my_seq = ++s_ui_walk.req_seq;
        s_ui_walk.out_max = max;
        s_ui_walk.requested = true;
        xSemaphoreGive(s_ui_walk.data);

        for (;;) {
            const TickType_t elapsed = xTaskGetTickCount() - start;
            if (elapsed >= window) {
                break;
            }
            if (xSemaphoreTake(s_ui_walk.done, window - elapsed) != pdTRUE) {
                break;
            }
            /* Accept a completion only if it answers THIS request, and copy
             * out under the data lock so lvgl_port_task cannot start another
             * walk into s_ui_walk_targets mid-copy. lvgl_port_task gives
             * `done` only after releasing `data`. lvgl_port_task itself takes
             * `data` with a zero timeout (s_ui_walk.data's doc comment above)
             * so it never blocks; THIS take, by contrast, is bounded by
             * whatever is left of the window, so a brief hold by the task
             * cannot turn a served request into a false timeout. */
            bool mine = false;
            const TickType_t used = xTaskGetTickCount() - start;
            if (xSemaphoreTake(s_ui_walk.data, used < window ? window - used : 0) == pdTRUE) {
                if (s_ui_walk.served_seq == my_seq) {
                    mine = true;
                    n = s_ui_walk.out_count;
                    if (n > max) {
                        n = max; /* defensive; the walk already clamps to out_max */
                    }
                    was_truncated = s_ui_walk.out_truncated;
                    memcpy(out, s_ui_walk_targets, n * sizeof(out[0]));
                    if (out_disp_w) {
                        *out_disp_w = s_ui_walk.out_disp_w;
                    }
                    if (out_disp_h) {
                        *out_disp_h = s_ui_walk.out_disp_h;
                    }
                }
                xSemaphoreGive(s_ui_walk.data);
            }
            if (mine) {
                break;
            }
        }
    }
    /* On a timeout s_ui_walk.requested is left set on purpose: lvgl_port_task
     * still services it whenever it next gets a turn, and the sequence check
     * above keeps that late completion from ever being taken as a LATER
     * caller's answer. n == 0 with was_truncated == true is the "no answer
     * available" shape documented in lvgl_port.h. */

    xSemaphoreGive(s_ui_walk.lock);
    if (truncated) {
        *truncated = was_truncated;
    }
    return n;
}

static void lvgl_port_task(void *arg)
{
    (void)arg;
    while (true) {
        lvgl_port_service_idle_blank();
        lvgl_port_service_idle_wake();
        if (s_tap_dump_requested) {
            /* Cleared BEFORE the call, not after: a second request arriving
             * while the dump is running is picked up on the NEXT loop
             * iteration this way, rather than being silently dropped by a
             * clear-after-call that would stomp on it. */
            s_tap_dump_requested = false;
            kiln_ui_log_tap_targets();
        }
        /* Zero-timeout take: a requester holds `data` only briefly (issuing
         * a request, or its copy-out), and a busy lock just defers the walk
         * to the next iteration -- lvgl_port_task never blocks on a
         * requester. `done` is given only after `data` is released. The
         * unlocked peek at `requested` only skips the lock on idle loops;
         * it is re-checked under the lock below. */
        if (s_ui_walk.requested && s_ui_walk.data && xSemaphoreTake(s_ui_walk.data, 0) == pdTRUE) {
            bool serviced = false;
            if (s_ui_walk.requested) {
                s_ui_walk.requested = false;
                bool walk_truncated = false;
                s_ui_walk.out_count = kiln_ui_collect_tap_targets(s_ui_walk_targets, s_ui_walk.out_max,
                                                                  &walk_truncated);
                s_ui_walk.out_truncated = walk_truncated;
                /* lvgl_port_task is the one legal LVGL caller -- read the
                 * resolution here so a requester on another task never has
                 * to call an lv_display_get_*_resolution() itself. */
                s_ui_walk.out_disp_w = lv_display_get_horizontal_resolution(NULL);
                s_ui_walk.out_disp_h = lv_display_get_vertical_resolution(NULL);
                s_ui_walk.served_seq = s_ui_walk.req_seq;
                serviced = true;
            }
            xSemaphoreGive(s_ui_walk.data);
            if (serviced) {
                xSemaphoreGive(s_ui_walk.done);
            }
        }
        s_timer_handler_calls++;
        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms == LV_NO_TIMER_READY) sleep_ms = 50;
        if (sleep_ms < 1) sleep_ms = 1;
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}

esp_err_t lvgl_port_start(ILI9488Class *display, const touch_dev_t *touch_dev, screen_idle_t *idle)
{
    if (!display) return ESP_ERR_INVALID_ARG;

    memset(&s_port, 0, sizeof(s_port));
    s_port.display = display;
    s_port.idle = idle;
    s_port.last_screen_on = true;

    /* Caller (main.c) already built this against whichever controller is
     * actually wired up -- NS2009 (ns2009_touch_dev_read()) or FT6336U
     * (FT6336U_touch_dev_read()), see the lvgl_port_t.touch_dev field
     * comment. A NULL touch_dev leaves s_port.touch_dev zeroed (ctx/read
     * both NULL), same as the old "touch hardware absent" case. */
    if (touch_dev) {
        s_port.touch_dev = *touch_dev;
    }

    memset(&s_inject, 0, sizeof(s_inject));
    s_inject.lock = xSemaphoreCreateMutex();
    if (!s_inject.lock) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex (touch inject) failed");
        return ESP_ERR_NO_MEM;
    }

    memset(&s_ui_walk, 0, sizeof(s_ui_walk));
    s_ui_walk.lock = xSemaphoreCreateMutex();
    s_ui_walk.data = xSemaphoreCreateMutex();
    s_ui_walk.done = xSemaphoreCreateBinary();
    if (!s_ui_walk.lock || !s_ui_walk.data || !s_ui_walk.done) {
        ESP_LOGE(TAG, "xSemaphoreCreate (ui walk) failed");
        return ESP_ERR_NO_MEM;
    }

    touch_cal_store_load(&s_touch_cal);

    lv_init();

    const esp_timer_create_args_t tick_args = {
        .callback = lv_tick_timer_cb,
        .name = "lv_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    esp_err_t err = esp_timer_create(&tick_args, &tick_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create (lv_tick) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_timer_start_periodic(tick_timer, 1000 /* us */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic (lv_tick) failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t width = 0, height = 0;
    ILI9488_get_dimensions(display, &width, &height);

    s_port.lv_disp = lv_display_create(width, height);
    if (!s_port.lv_disp) {
        ESP_LOGE(TAG, "lv_display_create failed");
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_user_data(s_port.lv_disp, &s_port);
    lv_display_set_flush_cb(s_port.lv_disp, ili9488_flush_cb);
    lv_display_set_color_format(s_port.lv_disp, LV_COLOR_FORMAT_RGB565);

    /* Buffers live in PSRAM -- see TODO.md 9.1a's 2026-08-17 reversal and
     * lvgl_port.h's header comment for why that's safe on this driver.
     * Two buffers: LVGL can render into one while the other's flush is still
     * in flight. The current flush callback is synchronous (ILI9488_blit_*
     * blocks on the SPI transfer), so this doesn't buy overlap today -- it's
     * cheap insurance against a future async/DMA-complete-callback flush
     * path costing nothing to keep now, given how small this is against 8 MB
     * of PSRAM. */
    size_t buf_pixels = (size_t)width * (size_t)LVGL_BUF_ROWS;
    size_t buf_bytes = buf_pixels * 2u; /* RGB565 */
    void *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    void *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "PSRAM draw buffer allocation failed (%u bytes each)", (unsigned)buf_bytes);
        if (buf1) heap_caps_free(buf1);
        if (buf2) heap_caps_free(buf2);
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_buffers(s_port.lv_disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_port.lv_indev = lv_indev_create();
    if (s_port.lv_indev) {
        lv_indev_set_type(s_port.lv_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_port.lv_indev, touch_read_cb);
        lv_indev_set_user_data(s_port.lv_indev, &s_port);
        lv_indev_set_display(s_port.lv_indev, s_port.lv_disp);
    } else {
        ESP_LOGW(TAG, "lv_indev_create failed -- UI will be view-only");
    }

    esp_err_t ui_err = kiln_ui_init();
    if (ui_err != ESP_OK) {
        ESP_LOGE(TAG, "kiln_ui_init failed: %s", esp_err_to_name(ui_err));
        return ui_err;
    }

    /* PSRAM stack, 2026-08-20. This 8192-byte stack is the single largest
     * contiguous internal-SRAM allocation the firmware makes, and plain
     * xTaskCreatePinnedToCore() can only take it from internal RAM. On this
     * board that stopped being possible: the internal heap reports ~243KB
     * free but fragments down to a largest contiguous block under 1KB by the
     * time this runs (uart_bridge_ext.c logs the figure at each bridge-task
     * creation), so an 8KB request fails no matter how much is free in total
     * and the display never came up at all -- "Failed to start
     * lvgl_port_task" on every boot.
     *
     * Same fix, and the same API, already applied to the UART bridge tasks in
     * uart_bridge_ext.c and gpio_probe.c: CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
     * does NOT make plain xTaskCreate* use PSRAM -- that needs
     * xTaskCreatePinnedToCoreWithCaps() with an explicit MALLOC_CAP_SPIRAM.
     *
     * REVERTED TO INTERNAL SRAM, 2026-08-20 (same day). The PSRAM stack above
     * CRASHED THE BOARD, and the reasoning that justified it was wrong.
     *
     * It said: "Safe for this task specifically: its stack holds LVGL's own
     * bookkeeping, not DMA buffers." That considered DMA and missed the real
     * constraint. A task whose stack is in PSRAM must never be running when
     * the flash cache is disabled -- PSRAM is reached THROUGH that cache, so
     * the stack itself vanishes mid-call. ESP-IDF asserts on exactly this:
     *
     *   Crashed task: 'lvgl'
     *   assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
     *                  cache_utils.c:126 (esp_task_stack_is_sane_cache_disabled())
     *
     * Reproduced by tapping "Touch Calibration", whose page writes calibration
     * data to NVS (touch_cal_store.c) from an LVGL event callback -- i.e. on
     * this task. Any UI callback that persists anything does the same, so this
     * was not an obscure corner: it was every settings write in the UI.
     *
     * "~17KB, comfortably more than the 8KB needed" turned out not to hold:
     * confirmed live on the bench 2026-08-21 (this file's new
     * touch_read_cb_count / lvgl_port_get_timer_handler_calls() counters,
     * added for the "all touch is dead" investigation, both stayed at
     * EXACTLY ZERO forever -- proving lvgl_port_task never ran a single
     * lv_timer_handler() call, not merely that touch itself was
     * misbehaving). The device log's "heap stage lvgl_start" line at that
     * exact boot showed largest internal block = 7680 B, just under this
     * task's 8192 B ask: xTaskCreatePinnedToCore() below failed, logged its
     * ESP_LOGE, and returned -- silently, because nothing downstream of a
     * failed lvgl_port_start() reboots or halts the board (main.c logs its
     * own ESP_LOGE and boots on). The one screen the user ever saw was
     * painted by kiln_ui_init()'s single kiln_ui_show("home") call, which
     * runs synchronously (via lv_refr_now()) INSIDE lvgl_port_start(),
     * before this task is even created -- so the panel looked normal while
     * every timer this UI depends on, including the touch indev's read
     * timer, silently never existed.
     *
     * More UART bridge tasks (dashboard_http, log/info/system/thermo/io) run
     * between the last time this fit and now, each taking its own slice of
     * internal SRAM before lvgl_port_start() ever gets a turn -- a genuinely
     * moving target, not something to keep re-measuring and hoping stays
     * above 8192 forever.
     *
     * REAL FIX (2026-08-21): stop asking the runtime heap for this stack at
     * all. Static allocation (xTaskCreateStaticPinnedToCore(), backed by a
     * plain .bss array below) has its address decided by the LINKER at
     * build time, before a single byte of runtime heap fragmentation exists
     * -- there is no "largest free block" query to lose to whatever else
     * booted first, because nothing is being carved out of a shared pool at
     * all. This is strictly better than chasing a bigger number for the
     * dynamic largest-free-block query: it can't be re-broken by some
     * future task claiming one more chunk of internal SRAM before this one
     * gets its turn. Internal SRAM is still the right place (not PSRAM --
     * see this comment's PSRAM-crash section above, still true, still
     * unrelated to whether the stack is static or dynamic): a static array
     * with internal linkage still lands in on-chip DRAM by default on this
     * target, reachable with the flash cache disabled, same as the dynamic
     * allocation was. */
    static StackType_t s_lvgl_task_stack[8192 / sizeof(StackType_t)];
    static StaticTask_t s_lvgl_task_tcb;
    TaskHandle_t created_handle = xTaskCreateStaticPinnedToCore(
        lvgl_port_task, "lvgl", sizeof(s_lvgl_task_stack) / sizeof(StackType_t), NULL, 4,
        s_lvgl_task_stack, &s_lvgl_task_tcb, tskNO_AFFINITY);
    if (created_handle == NULL) {
        /* Deliberately NOT falling back to a PSRAM stack: that is the
         * configuration that crashes on the first settings write, and a UI
         * that reboots the controller is worse than no UI. Static
         * allocation from a fixed-size .bss array can still fail if this
         * task's own arguments are wrong (they aren't) or FreeRTOS itself
         * rejects the call -- kept as a safety net, not because internal
         * SRAM headroom is the failure mode anymore. */
        ESP_LOGE(TAG, "Failed to start lvgl_port_task (static allocation) -- no local display "
                      "this boot");
        return ESP_ERR_NO_MEM;
    }
    /* DRAM_PSRAM_PLAN.md Phase 0 (4.2): registration only, no size change --
     * only reached with a real handle since the failure branch above already
     * returned. Static creation returns the handle directly rather than
     * filling an out-param, so it's copied into a static slot for
     * stack_margin_register() to read through, same as every other call
     * site's &task_handle. 8192 must match sizeof(s_lvgl_task_stack) above. */
    static TaskHandle_t s_lvgl_task_handle;
    s_lvgl_task_handle = created_handle;
    s_ui_walk_owner_task = created_handle;
    stack_margin_register("lvgl", &s_lvgl_task_handle, sizeof(s_lvgl_task_stack));

    ESP_LOGI(TAG, "LVGL up: %ux%u, %u-row PSRAM buffers, touch %s, idle-integration %s", width,
             height, (unsigned)LVGL_BUF_ROWS, s_port.touch_dev.read ? "on" : "off", idle ? "on" : "off");

    /* 2026-08-27: the touch-calibration state was previously logged only
     * inside lvgl_port_reload_touch_cal(), reachable exclusively by
     * finishing a calibration run -- a board that had NEVER been
     * calibrated said nothing about it, ever, at boot or otherwise. That
     * silence is exactly what let a board run for weeks on the
     * known-inaccurate Kconfig swap/invert bootstrap guess (see
     * touch_read_cb()'s "else" branch above) while its owner filed the
     * symptom -- small edge controls like the topbar back/home icons not
     * responding -- as a UI bug rather than an uncalibrated board. Log it
     * once here, every boot, loud enough (WARN) to be seen when it matters:
     * an operator staring at a boot log for an unrelated reason should not
     * be able to miss "touch is unreliable right now". */
    /* A self_calibrating device (unreached today -- see the touch_dev field
     * comment) never runs touch_cal_store's fit at all, so this WARN would
     * be permanently, misleadingly true for it forever -- gated out here
     * rather than discovered later as a boot-log lie. */
    if (touch_dev && !s_port.touch_dev.self_calibrating && !s_touch_cal.calibrated) {
        ESP_LOGW(TAG, "touch NOT calibrated -- running the known-inaccurate Kconfig "
                       "swap/invert bootstrap mapping; small controls (e.g. the topbar "
                       "back/home icons) may not respond to touch until a calibration run "
                       "completes");
    } else if (touch_dev && !s_port.touch_dev.self_calibrating) {
        ESP_LOGI(TAG, "touch calibrated -- using the per-board touch_cal_apply() fit");
    } else if (touch_dev) {
        ESP_LOGI(TAG, "touch self-calibrating -- touch_cal_store's per-board fit does not apply");
    }

    return ESP_OK;
}

void lvgl_port_get_last_raw_touch(uint16_t *raw_x, uint16_t *raw_y, uint16_t *z1)
{
    if (raw_x) *raw_x = s_last_raw_x;
    if (raw_y) *raw_y = s_last_raw_y;
    if (z1) *z1 = s_last_raw_z1;
}

void lvgl_port_reload_touch_cal(void)
{
    touch_cal_store_load(&s_touch_cal);
    ESP_LOGI(TAG, "touch calibration reloaded: calibrated=%d", (int)s_touch_cal.calibrated);
}

bool lvgl_port_touch_is_calibrated(void)
{
    return s_touch_cal.calibrated;
}

/* The shared calibration-visibility predicate -- see lvgl_port.h. Delegates
 * to touch_dev.h's pure touch_dev_cal_support() rather than re-deriving the
 * answer here, so the host tests covering that function cover this path too
 * (nothing in this file is in the host build: it pulls in LVGL and the whole
 * LCD driver stack). */
touch_cal_support_t lvgl_port_touch_cal_support(void)
{
    return touch_dev_cal_support(&s_port.touch_dev);
}

void lvgl_port_set_input_enabled(bool enabled)
{
    if (s_port.lv_indev) {
        lv_indev_enable(s_port.lv_indev, enabled);
    }
    /* Kept in lockstep even if s_port.lv_indev is NULL (no touch hardware),
     * so the shadow always reflects "what this module was last told", not
     * "what LVGL actually has" -- see the field's declaration comment on why
     * no public getter exists to cross-check against. */
    s_input_enabled = enabled;
}

void lvgl_port_get_touch_diag(bool *input_enabled, uint32_t *touch_read_cb_count,
                               uint32_t *injected_delivered_count)
{
    if (input_enabled) *input_enabled = s_input_enabled;
    if (touch_read_cb_count) *touch_read_cb_count = s_touch_read_cb_count;
    if (injected_delivered_count) *injected_delivered_count = s_injected_delivered_count;
}

bool lvgl_port_get_inject_verdict(uint32_t seq, bool *out_swallowed)
{
    if (seq == 0 || !s_inject.lock) {
        return false; /* 0 is never a valid seq (lvgl_port_inject_touch()'s
                       * own "queued nothing" sentinel) and no lock means
                       * lvgl_port_start() hasn't run -- either way there is
                       * nothing to have recorded. */
    }
    if (!touch_inject_lock()) {
        return false; /* lock contention/timeout: same "could not determine"
                       * answer as every other timeout in this file -- caller
                       * (kiln_ui_click_by_name()) retries within its own
                       * bounded poll rather than this call blocking longer. */
    }
    bool found = (s_inject_verdict_seq == seq);
    bool swallowed = s_inject_verdict_swallowed;
    touch_inject_unlock();

    if (found && out_swallowed) {
        *out_swallowed = swallowed;
    }
    return found;
}

/* ONE-OFF root-cause probe (2026-08-21): is s_port.lv_indev even non-NULL?
 * touch_read_cb_count staying at 0 forever (proven on the bench: it does not
 * move even across several seconds of idling with no injection at all) is
 * consistent with either (a) lv_indev_create() itself failing, in which case
 * touch_read_cb was never registered as anyone's read callback, or (b) the
 * indev existing but its internal read_timer never firing. This resolves
 * which. */
bool lvgl_port_indev_exists(void)
{
    return s_port.lv_indev != NULL;
}

/* Exposes the touch indev so a caller can attach its own lv_indev_add_event_cb()
 * -- added for ui_lcd_lock.c's inactivity-lock activity hook
 * (docs/WEB_AUTH_PLAN.md section 8: "any touch event delivered to LVGL,
 * including a touch that only scrolls or is swallowed by a backdrop" --
 * an indev-level LV_EVENT_PRESSED callback sees every press regardless of
 * which widget, if any, ends up handling it, unlike a callback attached to
 * one specific widget). Returns NULL before lvgl_port_start() has created
 * the indev, same as lvgl_port_indev_exists() reports false in that window. */
lv_indev_t *lvgl_port_get_indev(void)
{
    return s_port.lv_indev;
}
