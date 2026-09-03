#include "backlight_pwm.h"

#include <string.h>

#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "stack_margin.h"

// screen_idle.h (needed only by the CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE
// branch below, for the `const screen_idle_t *` cast) is included from
// inside that #if, not here -- it pulls in panel_spi.h/NS2009.h, which pull
// in real ESP-IDF SPI/I2C driver headers that this project's host-test
// build cannot compile (confirmed while adding this file). The flag is off
// in the host build (see backlight_pwm.h's header comment), so the #else
// branch below -- the only one host-compiled -- never needs the real type.

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

#include "screen_idle.h"

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
        const screen_idle_t *idle = (const screen_idle_t *)bl->idle;
        if (screen_idle_get_state(idle, &screen_on, &idle_ms) != ESP_OK) {
            continue; /* lock timeout on screen_idle's side -- retry next poll */
        }

        if (bl->have_last_screen_on && bl->last_screen_on == screen_on) {
            continue; /* no change: skip the LEDC call */
        }

        uint8_t pct = backlight_duty_percent_for_state(
            screen_on, CONFIG_KILNCTL_BACKLIGHT_ON_PERCENT, CONFIG_KILNCTL_BACKLIGHT_IDLE_PERCENT);
        esp_err_t err = apply_duty(pct);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "apply_duty(%u%%) failed: %s", (unsigned)pct, esp_err_to_name(err));
            continue; /* leave last_screen_on unset so the next tick retries */
        }

        bl->last_screen_on = screen_on;
        bl->have_last_screen_on = true;
        ESP_LOGI(TAG, "screen_on=%d -> backlight %u%%", (int)screen_on, (unsigned)pct);
    }
}

esp_err_t backlight_pwm_init(backlight_pwm_t *bl, const void *idle)
{
    if (!bl || !idle) return ESP_ERR_INVALID_ARG;
    memset(bl, 0, sizeof(*bl));
    bl->idle = idle;

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
esp_err_t backlight_pwm_init(backlight_pwm_t *bl, const void *idle)
{
    if (!bl) return ESP_ERR_INVALID_ARG;
    memset(bl, 0, sizeof(*bl));
    bl->idle = idle;
    bl->ready = false;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t backlight_pwm_start(backlight_pwm_t *bl)
{
    (void)bl;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif // CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE
