#include "screen_idle.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/task.h"
#include "settings.h"
#include "stack_margin.h"

static const char *TAG = "screen_idle";

/* DRAM_PSRAM_PLAN.md Phase 0 (4.2): stack_margin_register() target. Only one
 * screen_idle_t instance exists (main.c's static screen_idle), so a single
 * module-static handle is enough -- matches kiln_io_owner.c/thermo_owner.c's
 * s_task_handle idiom for owners whose public struct has no handle field. */
static TaskHandle_t s_task_handle;

/* How often screen_idle_task polls the touch controller (when one is up).
 * NS2009_read does three back-to-back I2C conversions worst case (Z1, then
 * X and Y only if pressed); 50ms is comfortably above that and fast enough
 * that a touch feels immediate against a 60s timeout. */
#define SCREEN_IDLE_POLL_MS 50u

/* How long a caller waits for the lock. Only ever held for a few variable
 * reads/writes, never across I2C or SPI traffic -- generous on purpose. */
#define SCREEN_IDLE_LOCK_TIMEOUT_MS 1000u

static bool screen_idle_lock(screen_idle_t *idle)
{
    return xSemaphoreTake(idle->lock, pdMS_TO_TICKS(SCREEN_IDLE_LOCK_TIMEOUT_MS)) == pdTRUE;
}

static void screen_idle_unlock(screen_idle_t *idle)
{
    xSemaphoreGive(idle->lock);
}

/* Stamps activity and, if the screen is currently blanked, wakes it. Shared
 * by the poll task's own press detection and screen_idle_inject_touch(), so
 * a real press and an MCP-injected one are indistinguishable to everything
 * downstream of this call. Returns ESP_OK immediately if the screen was
 * already on -- nothing to do.
 *
 * "Wake" is deliberately just a flag flip, not a hardware call: this board
 * has no backlight control line (docs/HARDWARE.md), so blanking (below)
 * never powers the panel down -- it paints black and leaves the panel
 * running. There is nothing to "turn back on"; whatever owns the UI (the
 * PC/dashboard) is responsible for repainting real content once it sees
 * screen_on go back to true (TOUCH_CMD_GET_STATE), same as it would after
 * any other blank period. */
static esp_err_t screen_idle_mark_active(screen_idle_t *idle)
{
    if (!screen_idle_lock(idle)) return ESP_ERR_TIMEOUT;
    idle->last_activity_tick = xTaskGetTickCount();
    bool need_wake = !idle->screen_on;
    if (need_wake) idle->screen_on = true;
    screen_idle_unlock(idle);

    if (need_wake) ESP_LOGI(TAG, "screen woken");
    return ESP_OK;
}

static void screen_idle_task(void *arg)
{
    screen_idle_t *idle = (screen_idle_t *)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SCREEN_IDLE_POLL_MS));

        if (idle->touch) {
            bool pressed = false;
            uint16_t x = 0, y = 0;
            esp_err_t err = NS2009_read(idle->touch, &pressed, &x, &y, NULL);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "NS2009_read failed: %s", esp_err_to_name(err));
            } else if (pressed) {
                screen_idle_mark_active(idle);
            }
        }

        TickType_t last_activity_tick;
        bool screen_on;
        if (!screen_idle_lock(idle)) continue;
        last_activity_tick = idle->last_activity_tick;
        screen_on = idle->screen_on;
        screen_idle_unlock(idle);

        if (!screen_on) continue;

/* CONFIG_KILNCTL_TOUCH_IDLE_TIMEOUT_MS == 0 (Kconfig default as of
 * 2026-08-18, user request) means auto-blank is disabled: the panel just
 * stays on. This is a compile-time #if, not a runtime `if (... == 0)`
 * check, for two reasons -- both of the blank-after-idle block below become
 * dead code when the timeout is 0, so there's no reason to pay for it in
 * the built image; and a runtime `elapsed_ticks < pdMS_TO_TICKS(0)`
 * comparison is a `TickType_t < 0` in disguise once the macro is
 * substituted, which -Werror=type-limits correctly flags as always-false
 * regardless of whether it's reachable (found building 2026-08-18).
 * Activity tracking above still runs either way -- last_activity_tick keeps
 * updating from real and injected touches, and TOUCH_CMD_GET_STATE's
 * idle_ms still reports it -- only the blank decision itself is compiled
 * out. The feature is untouched and stays toggleable via menuconfig
 * (setting it back above 0 and rebuilding restores the old behavior). */
#if CONFIG_KILNCTL_TOUCH_IDLE_TIMEOUT_MS > 0
        /* Unsigned subtraction on TickType_t wraps correctly across the tick
         * counter's ~49-day rollover, same reasoning as
         * uart_bridge.c's link_watchdog_task. */
        TickType_t elapsed_ticks = xTaskGetTickCount() - last_activity_tick;
        if (elapsed_ticks < pdMS_TO_TICKS(TOUCH_IDLE_TIMEOUT_MS)) continue;

        /* No backlight control line on this board (docs/HARDWARE.md), so
         * ILI9488_set_power(false) (display-off + sleep-in) is not used here
         * -- on this panel that shows as a blank WHITE page (2026-08-17
         * bench finding: DISPOFF forces a blank page independent of GRAM
         * content, so pre-clearing wouldn't help either), which is brighter
         * than doing nothing. Painting the frame black instead is the
         * closest a firmware-only fix gets to "dark": the backlight stays
         * lit (nothing can switch it off), but black LCD content blocks far
         * more of it than white does. A real "screen and backlight out" needs
         * a board revision with a GPIO-driven backlight switch.
         *
         * This task does NOT issue the ILI9488_clear() call itself -- the
         * display's SPI device has exactly one legal owner, the LVGL task
         * (DISPLAY_ST7796_PLAN.md section 8), and a second task drawing to
         * it is precisely the race that got the old UART DISPLAY task
         * deleted on 2026-08-27. This task only requests the blank by
         * flipping screen_on; lvgl_port_task notices the on->off edge (the
         * same way it already notices the off->on "wake" edge) and performs
         * the actual clear from within its own task context. */
        if (!screen_idle_lock(idle)) continue; /* lock timeout: retry next poll */
        idle->screen_on = false;
        screen_idle_unlock(idle);
        ESP_LOGI(TAG, "idle timeout (%lu ms) reached -- requesting blank",
                 (unsigned long)(TOUCH_IDLE_TIMEOUT_MS));
#else
        (void)last_activity_tick; /* only consumed by the elapsed_ticks calc above, compiled out here */
#endif
    }
}

esp_err_t screen_idle_init(screen_idle_t *idle, ILI9488Class *display, NS2009Class *touch)
{
    if (!idle || !display) return ESP_ERR_INVALID_ARG;

    memset(idle, 0, sizeof(*idle));
    idle->display = display;
    idle->touch = touch;
    idle->last_activity_tick = xTaskGetTickCount();
    idle->screen_on = true; /* the display driver leaves the panel lit after bring-up */

    idle->lock = xSemaphoreCreateMutex();
    if (!idle->lock) return ESP_ERR_NO_MEM;

    idle->ready = true;
    return ESP_OK;
}

esp_err_t screen_idle_start(screen_idle_t *idle)
{
    if (!idle || !idle->ready) return ESP_ERR_INVALID_STATE;

    BaseType_t created =
        xTaskCreatePinnedToCore(screen_idle_task, "screen_idle", 3072, idle, 3, &s_task_handle, tskNO_AFFINITY);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    /* DRAM_PSRAM_PLAN.md Phase 0 (4.2): registration only, no size change --
     * only reached with a real handle since the failure branch above already
     * returned. 3072 must match the xTaskCreatePinnedToCore() literal above. */
    stack_margin_register("screen_idle", &s_task_handle, 3072);
    return ESP_OK;
}

esp_err_t screen_idle_inject_touch(screen_idle_t *idle, uint16_t x, uint16_t y, bool pressed)
{
    if (!idle || !idle->ready) return ESP_ERR_INVALID_STATE;
    (void)x;
    (void)y; /* not used for the idle/wake decision -- only *that* a touch happened */

    if (!pressed) return ESP_OK; /* a release event: nothing to feed the idle timer */
    return screen_idle_mark_active(idle);
}

esp_err_t screen_idle_get_state(const screen_idle_t *idle, bool *out_screen_on,
                                uint32_t *out_idle_ms)
{
    if (!idle || !idle->ready || !out_screen_on || !out_idle_ms) return ESP_ERR_INVALID_ARG;

    /* const-correctness note: the lock itself is not modified through this
     * pointer's constness in any way the compiler would reject, but taking
     * it requires a non-const handle -- cast away const on the semaphore
     * only, mirroring how ILI9488_get_dimensions-style read-only queries are
     * written elsewhere in this codebase. */
    screen_idle_t *mutable_idle = (screen_idle_t *)idle;
    if (!screen_idle_lock(mutable_idle)) return ESP_ERR_TIMEOUT;
    TickType_t last_activity_tick = idle->last_activity_tick;
    *out_screen_on = idle->screen_on;
    screen_idle_unlock(mutable_idle);

    TickType_t elapsed_ticks = xTaskGetTickCount() - last_activity_tick;
    uint64_t elapsed_ms = (uint64_t)elapsed_ticks * portTICK_PERIOD_MS;
    *out_idle_ms = (elapsed_ms > UINT32_MAX) ? UINT32_MAX : (uint32_t)elapsed_ms;
    return ESP_OK;
}
