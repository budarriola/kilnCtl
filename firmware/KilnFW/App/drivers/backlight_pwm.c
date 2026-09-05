#include "backlight_pwm.h"

#include <string.h>

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "stack_margin.h"

// No screen_idle.h/display_power_cfg.h include here at all any more
// (HW_ABSTRACTION_PLAN.md "drivers/ layering" item 5) -- this hw-layer file
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

/* LEDC_TIMER_0 / LEDC_CHANNEL_0: nothing else in this tree uses LEDC (grepped
 * App/ for ledc_/LEDC_TIMER/LEDC_CHANNEL before adding this -- no hits), so
 * the first timer/channel is free. */
#define BACKLIGHT_LEDC_TIMER   LEDC_TIMER_0
#define BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
#define BACKLIGHT_LEDC_MODE    LEDC_LOW_SPEED_MODE

/* 13-bit resolution at 5 kHz is comfortably above flicker perception and
 * within the LEDC low-speed timer's max (80MHz APB / 2^13 ~= 9.8kHz ceiling
 * for this duty resolution) -- plenty of headroom for a backlight MOSFET
 * gate, not chasing PWM-audible-whine territory the way a motor driver
 * would need to. */
#define BACKLIGHT_LEDC_DUTY_RES LEDC_TIMER_13_BIT
#define BACKLIGHT_LEDC_FREQ_HZ  5000

#define BACKLIGHT_LEDC_DUTY_MAX ((1u << 13) - 1u) /* must match BACKLIGHT_LEDC_DUTY_RES */

// --- Pure logic (host-testable) ---------------------------------------------

uint8_t backlight_duty_percent_for_state(bool screen_on, uint8_t on_percent, uint8_t idle_percent)
{
    uint8_t pct = screen_on ? on_percent : idle_percent;
    if (pct > 100) pct = 100; /* Kconfig ranges already enforce this; belt and suspenders */
    return pct;
}

// --- Hardware driver ---------------------------------------------------------

#if CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE

static uint32_t duty_for_percent(uint8_t pct)
{
    if (pct >= 100) return BACKLIGHT_LEDC_DUTY_MAX;
    if (pct == 0) return 0;
    return (uint32_t)pct * BACKLIGHT_LEDC_DUTY_MAX / 100u;
}

static esp_err_t apply_duty(uint8_t pct)
{
    uint32_t duty = duty_for_percent(pct);
    esp_err_t err = ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, duty);
    if (err != ESP_OK) return err;
    return ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
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

        esp_err_t err = apply_duty(pct);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "apply_duty(%u%%) failed: %s", (unsigned)pct, esp_err_to_name(err));
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
    if (!bl || !query_fn) return ESP_ERR_INVALID_ARG;
    memset(bl, 0, sizeof(*bl));
    bl->query_fn = query_fn;
    bl->query_ctx = query_ctx;

    ledc_timer_config_t timer_cfg = {
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .duty_resolution = BACKLIGHT_LEDC_DUTY_RES,
        .timer_num = BACKLIGHT_LEDC_TIMER,
        .freq_hz = BACKLIGHT_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s -- backlight PWM unavailable this boot",
                 esp_err_to_name(err));
        return err;
    }

    /* Start at ON-percent duty, matching panel_spi.c leaving the panel lit
     * after bring-up (screen_idle_init() likewise seeds screen_on = true) --
     * the backlight must not start dark if the flying wire IS fitted. */
    ledc_channel_config_t chan_cfg = {
        .gpio_num = CONFIG_KILNCTL_BACKLIGHT_GPIO,
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .channel = BACKLIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BACKLIGHT_LEDC_TIMER,
        .duty = duty_for_percent(CONFIG_KILNCTL_BACKLIGHT_ON_PERCENT),
        .hpoint = 0,
    };
    err = ledc_channel_config(&chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config(GPIO%d) failed: %s -- backlight PWM unavailable this boot",
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
