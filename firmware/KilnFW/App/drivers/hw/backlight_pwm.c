#include "backlight_pwm.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal_pwm.h"
#include "stack_margin.h"

// No screen_idle.h/display_power_cfg.h include here at all any more
// (HW_ABSTRACTION.md "drivers/ layering" item 5) -- this hw-layer file
// reads screen state and brightness only through the backlight_pwm_query_fn
// the caller supplies to backlight_pwm_init() (see backlight_pwm.h). That
// also keeps the host-test build simple: it compiles the
// CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE-off (#else) branch below, which never
// calls query_fn at all.

static const char *TAG = "backlight_pwm";

/* DRAM_PSRAM_PLAN.md Phase 0 (4.2): stack_margin_register() target -- same
 * single-instance-module idiom as screen_idle.c/boot_button.c. */
static TaskHandle_t s_task_handle;

/* How often the poll task checks screen_idle's screen_on flag and re-syncs
 * LEDC duty. Backlight change is not latency-critical the way touch-wake
 * detection is (screen_idle.c polls the touch controller at 50ms); a lazier
 * poll here is enough to feel immediate against a human eye and costs less
 * CPU than matching screen_idle's own cadence. */
#define BACKLIGHT_PWM_POLL_MS 200u

/* Timer/channel/speed-mode selection is now hal_pwm_esp.c's concern (hardcoded
 * there to LEDC_TIMER_0/LEDC_CHANNEL_0/LEDC_LOW_SPEED_MODE -- nothing else in
 * this tree uses LEDC, so there is only ever one channel to abstract; see
 * hal_pwm.h's header comment). Only the frequency and resolution stay here,
 * since they are this driver's own config passed into hal_pwm_cfg_t below.
 *
 * 13-bit resolution at 5 kHz is comfortably above flicker perception and
 * within the LEDC low-speed timer's max (80MHz APB / 2^13 ~= 9.8kHz ceiling
 * for this duty resolution) -- plenty of headroom for a backlight MOSFET
 * gate, not chasing PWM-audible-whine territory the way a motor driver
 * would need to. */
#define BACKLIGHT_LEDC_FREQ_HZ  5000

// --- Pure logic (host-testable) ---------------------------------------------

uint8_t backlight_duty_percent_for_state(bool screen_on, uint8_t on_percent, uint8_t idle_percent)
{
    uint8_t pct = screen_on ? on_percent : idle_percent;
    if (pct > 100) pct = 100; /* Kconfig ranges already enforce this; belt and suspenders */
    return pct;
}

// --- Hardware driver ---------------------------------------------------------

#if CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE

#include "hal_esp_common.h" /* hal_status_to_esp_err() -- see panel_spi_bringup.c's identical use */

static hal_status_t apply_duty(uint8_t pct)
{
    return hal_pwm_set_duty(pct);
}

static void backlight_pwm_task(void *arg)
{
    backlight_pwm_t *bl = (backlight_pwm_t *)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BACKLIGHT_PWM_POLL_MS));

        bool screen_on = true;
        uint32_t idle_ms = 0;
        uint8_t brightness_pct = 0;
        if (bl->query_fn(bl->query_ctx, &screen_on, &idle_ms, &brightness_pct) != ESP_OK) {
            continue; /* caller's query_fn declined this tick (e.g. a lock
                       * timeout on screen_idle's side) -- retry next poll */
        }

        /* The ON duty is the operator's stored brightness, not a compile-time
         * constant -- that setting has persisted and round-tripped since the
         * display-power feature landed but drove nothing until the flying
         * wire to CONFIG_KILNCTL_BACKLIGHT_GPIO was fitted (owner confirmed
         * 2026-09-04). IDLE stays a Kconfig constant: it is the blanked
         * state, not something the brightness slider addresses. */
        uint8_t pct = backlight_duty_percent_for_state(
            screen_on, brightness_pct, CONFIG_KILNCTL_BACKLIGHT_IDLE_PERCENT);

        /* Gate on the DUTY, not on screen_on alone. Gating on screen_on made
         * a brightness change invisible until the next blank/wake edge --
         * i.e. the slider would appear dead for exactly as long as nobody
         * touched the screen. */
        if (bl->have_last_pct && bl->last_pct == pct &&
            bl->have_last_screen_on && bl->last_screen_on == screen_on) {
            continue; /* nothing to write: skip the LEDC call */
        }

        hal_status_t status = apply_duty(pct);
        if (status != HAL_OK) {
            /* Log the hal_status_t directly rather than round-tripping through
             * esp_err_to_name(status-as-esp_err_t) -- that conversion collapses
             * every unmapped LEDC error onto ESP_FAIL, hiding which hal_status_t
             * actually came back. */
            ESP_LOGW(TAG, "apply_duty(%u%%) failed: %s", (unsigned)pct, hal_status_to_name(status));
            continue; /* leave last_* unset so the next tick retries */
        }

        bl->last_screen_on = screen_on;
        bl->have_last_screen_on = true;
        bl->last_pct = pct;
        bl->have_last_pct = true;
        ESP_LOGI(TAG, "screen_on=%d -> backlight %u%%", (int)screen_on, (unsigned)pct);
    }
}

esp_err_t backlight_pwm_init(backlight_pwm_t *bl, backlight_pwm_query_fn query_fn, void *query_ctx)
{
    if (!bl || !query_fn || !query_ctx) return ESP_ERR_INVALID_ARG;
    memset(bl, 0, sizeof(*bl));
    bl->query_fn = query_fn;
    bl->query_ctx = query_ctx;

    /* hal_pwm_init() folds the old ledc_timer_config()+ledc_channel_config()
     * pair into one call, and applies start_duty_percent as part of the
     * channel config itself -- matching this driver's own "must not start
     * dark if the flying wire IS fitted" requirement (panel_spi.c leaves the
     * panel lit after bring-up; screen_idle_init() likewise seeds
     * screen_on = true) without a separate follow-up hal_pwm_set_duty(). */
    hal_pwm_cfg_t pwm_cfg = {
        .gpio_num = CONFIG_KILNCTL_BACKLIGHT_GPIO,
        .freq_hz = BACKLIGHT_LEDC_FREQ_HZ,
        .duty_resolution_bits = 13, /* see BACKLIGHT_LEDC_FREQ_HZ's comment above */
        .start_duty_percent = CONFIG_KILNCTL_BACKLIGHT_ON_PERCENT,
    };
    esp_err_t err = hal_status_to_esp_err(hal_pwm_init(&pwm_cfg));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hal_pwm_init(GPIO%d) failed: %s -- backlight PWM unavailable this boot",
                 CONFIG_KILNCTL_BACKLIGHT_GPIO, esp_err_to_name(err));
        return err;
    }

    bl->last_screen_on = true;
    bl->have_last_screen_on = true;
    bl->ready = true;
    return ESP_OK;
}

esp_err_t backlight_pwm_start(backlight_pwm_t *bl)
{
    if (!bl || !bl->ready) return ESP_ERR_INVALID_STATE;

    BaseType_t created = xTaskCreate(backlight_pwm_task, "backlight_pwm", 3072, bl,
                                      tskIDLE_PRIORITY + 1, &s_task_handle);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    /* DRAM_PSRAM_PLAN.md Phase 0 (4.2): registration only, no size change --
     * 3072 must match the xTaskCreate() literal above. */
    stack_margin_register("backlight_pwm", &s_task_handle, 3072);
    return ESP_OK;
}

#else // !CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE

/* Flag off (the default -- no flying wire fitted, DISPLAY_ST7796_PLAN.md
 * 3.4.1): touch nothing, claim no GPIO/LEDC peripheral, cost nothing in the
 * built image beyond these two trivial stubs. Matches spi_owner_transfer_
 * async()'s ESP_ERR_NOT_SUPPORTED-when-disabled convention (9.6 in the same
 * plan doc) rather than silently pretending to succeed. */
esp_err_t backlight_pwm_init(backlight_pwm_t *bl, backlight_pwm_query_fn query_fn, void *query_ctx)
{
    if (!bl) return ESP_ERR_INVALID_ARG;
    memset(bl, 0, sizeof(*bl));
    bl->query_fn = query_fn;
    bl->query_ctx = query_ctx;
    bl->ready = false;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t backlight_pwm_start(backlight_pwm_t *bl)
{
    (void)bl;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif // CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE
