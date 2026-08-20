#include "lvgl_port.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lvgl.h"

#include "kiln_ui.h"
#include "settings.h"
#include "touch_cal_store.h"
#include "ui_theme.h"

static const char *TAG = "lvgl_port";

/* LVGL is not thread-safe: every lv_* call below (flush callback, indev
 * callback, tick, timer_handler, and the one-time screen build) runs on
 * lvgl_port_task and nowhere else. Nothing outside this file may touch an
 * lv_* API. */

typedef struct {
    ILI9488Class *display;
    NS2009Class *touch;     /* NULL if no touch hardware */
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
static void ili9488_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    lvgl_port_t *p = (lvgl_port_t *)lv_display_get_user_data(disp);

    bool screen_on = true;
    if (p->idle) {
        uint32_t idle_ms = 0;
        if (screen_idle_get_state(p->idle, &screen_on, &idle_ms) != ESP_OK) {
            screen_on = true; /* fail open: draw rather than go permanently dark */
        }
        if (screen_on && !p->last_screen_on) {
            ESP_LOGI(TAG, "screen woke -- forcing a full redraw");
            lv_obj_invalidate(lv_screen_active());
        }
        p->last_screen_on = screen_on;
    }

    if (screen_on) {
        uint16_t x = (uint16_t)area->x1;
        uint16_t y = (uint16_t)area->y1;
        uint16_t w = (uint16_t)(area->x2 - area->x1 + 1);
        uint16_t h = (uint16_t)(area->y2 - area->y1 + 1);

        esp_err_t err = ILI9488_blit_begin(p->display, x, y, w, h);
        if (err == ESP_OK) {
            err = ILI9488_blit_data(p->display, px_map, (size_t)w * (size_t)h * 2u);
            esp_err_t end_err = ILI9488_blit_end(p->display);
            if (err == ESP_OK) err = end_err;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "flush [%u,%u %ux%u] failed: %s", x, y, w, h, esp_err_to_name(err));
        }
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
} touch_inject_t;

static touch_inject_t s_inject;

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

void lvgl_port_inject_touch(uint16_t x, uint16_t y, bool pressed)
{
    if (!s_inject.lock) return; /* lvgl_port_start hasn't run yet -- nothing to inject into */
    if (!touch_inject_lock()) return; /* contention on a 1s timeout: drop rather than block the UART task */

    s_inject.pending = true;
    s_inject.pressed = pressed;
    s_inject.x = (int32_t)x;
    s_inject.y = (int32_t)y;
    s_inject.last_update_tick = xTaskGetTickCount();

    touch_inject_unlock();
}

/* --- Touch input device -----------------------------------------------
 * The one and only NS2009 reader once this module starts (see lvgl_port.h).
 * A real press is forwarded into screen_idle_inject_touch() so screen_idle's
 * idle timer / wake logic works exactly as it does for a UART-injected
 * touch -- screen_idle was built to not care which source a touch came from
 * (screen_idle.h/.c), and this keeps it that way. */
static int32_t touch_raw_to_px(uint16_t raw, uint16_t panel_extent, bool invert)
{
    if (raw > NS2009_ADC_MAX) raw = NS2009_ADC_MAX;
    uint32_t px = ((uint32_t)raw * (panel_extent - 1u)) / NS2009_ADC_MAX;
    if (invert) px = (panel_extent - 1u) - px;
    return (int32_t)px;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
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
                screen_idle_inject_touch(p->idle, (uint16_t)inject_x, (uint16_t)inject_y, inject_pressed);
            }
            return;
        }
    }

    if (!p->touch) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    bool pressed = false;
    uint16_t raw_x = 0, raw_y = 0, raw_z1 = 0;
    esp_err_t err = NS2009_read(p->touch, &pressed, &raw_x, &raw_y, &raw_z1);
    if (err != ESP_OK || !pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    s_last_raw_x = raw_x;
    s_last_raw_y = raw_y;
    s_last_raw_z1 = raw_z1;

    uint16_t width = 0, height = 0;
    ILI9488_get_dimensions(p->display, &width, &height);

    int32_t px, py;
    if (s_touch_cal.calibrated) {
        /* Real per-board fit -- see touch_cal_store.h. This is what every
         * page's hit-testing runs on once calibration has completed. */
        touch_cal_apply(&s_touch_cal, raw_x, raw_y, width, height, &px, &py);
    } else {
        /* Bootstrap-only fallback, before this board has ever been
         * calibrated: the Kconfig swap/invert guess. Known inaccurate (see
         * touch_cal_store.h's header comment) -- good enough only for
         * ui_page_touch_cal.c's full-screen "any press counts" capture
         * during the forced first-run calibration flow, not for real
         * button hit-testing. */
        uint16_t ax = TOUCH_CAL_SWAP_XY ? raw_y : raw_x;
        uint16_t ay = TOUCH_CAL_SWAP_XY ? raw_x : raw_y;
        px = touch_raw_to_px(ax, width, TOUCH_CAL_INVERT_X);
        py = touch_raw_to_px(ay, height, TOUCH_CAL_INVERT_Y);
    }

    data->point.x = px;
    data->point.y = py;
    data->state = LV_INDEV_STATE_PRESSED;

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
    if (ui_theme_touch_groups_active()) {
        lv_point_t raw_point = { .x = px, .y = py };
        lv_obj_t *default_hit = lv_indev_search_obj(lv_screen_active(), &raw_point);
        lv_obj_t *target = ui_theme_resolve_touch_target(default_hit, raw_point);
        if (target && target != default_hit) {
            lv_area_t coords;
            lv_obj_get_coords(target, &coords);
            data->point.x = (coords.x1 + coords.x2) / 2;
            data->point.y = (coords.y1 + coords.y2) / 2;
        }
    }

    if (p->idle) {
        screen_idle_inject_touch(p->idle, (uint16_t)px, (uint16_t)py, true);
    }
}

/* --- Task: the only thing that ever calls lv_timer_handler() ----------
 * LVGL's own return value from lv_timer_handler() is how long it's safe to
 * sleep before the next call is needed -- honoured directly rather than a
 * fixed poll period, same idea as every other "sleep until there's real
 * work" task in this codebase. */
static void lvgl_port_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms == LV_NO_TIMER_READY) sleep_ms = 50;
        if (sleep_ms < 1) sleep_ms = 1;
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}

esp_err_t lvgl_port_start(ILI9488Class *display, NS2009Class *touch, screen_idle_t *idle)
{
    if (!display) return ESP_ERR_INVALID_ARG;

    memset(&s_port, 0, sizeof(s_port));
    s_port.display = display;
    s_port.touch = touch;
    s_port.idle = idle;
    s_port.last_screen_on = true;

    memset(&s_inject, 0, sizeof(s_inject));
    s_inject.lock = xSemaphoreCreateMutex();
    if (!s_inject.lock) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex (touch inject) failed");
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
     * Safe for this task specifically: its stack holds LVGL's own bookkeeping,
     * not DMA buffers. The frame data handed to the ILI9488 comes from LVGL's
     * draw buffers, which are already allocated in PSRAM (see the buffer
     * allocation above and the "PSRAM buffers" note in the log line below),
     * and any transfer needing DMA-capable memory requests MALLOC_CAP_DMA and
     * is unaffected by where this stack lives. */
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(lvgl_port_task, "lvgl", 8192, NULL, 4, NULL,
                                                         tskNO_AFFINITY,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to start lvgl_port_task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LVGL up: %ux%u, %u-row PSRAM buffers, touch %s, idle-integration %s", width,
             height, (unsigned)LVGL_BUF_ROWS, touch ? "on" : "off", idle ? "on" : "off");
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

void lvgl_port_set_input_enabled(bool enabled)
{
    if (s_port.lv_indev) {
        lv_indev_enable(s_port.lv_indev, enabled);
    }
}
