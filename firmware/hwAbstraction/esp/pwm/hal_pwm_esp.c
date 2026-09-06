/* hal_pwm_esp.c -- ESP-IDF backend for interface/hal_pwm.h.
 *
 * Phase 1b ("adapt"): implements the Phase-0 interface against ESP-IDF
 * v6.0.2 (C:\esp\v6.0.2\esp-idf, driver/ledc.h), grounded in hal_pwm.h's
 * own consumer census: backlight_pwm.c is the ONLY LEDC user in this tree.
 * Real call sequence reproduced verbatim (backlight_pwm.c:
 * BACKLIGHT_LEDC_TIMER=LEDC_TIMER_0, BACKLIGHT_LEDC_CHANNEL=LEDC_CHANNEL_0,
 * BACKLIGHT_LEDC_MODE=LEDC_LOW_SPEED_MODE, BACKLIGHT_LEDC_FREQ_HZ=5000,
 * BACKLIGHT_LEDC_DUTY_RES=LEDC_TIMER_13_BIT): ledc_timer_config() then
 * ledc_channel_config() at init (duty=0, hpoint=0), ledc_set_duty()
 * immediately followed by ledc_update_duty() on every re-sync. Not wired
 * into any CMakeLists yet -- see
 * firmware/hwAbstraction/test/compile_esp_backends.ps1.
 *
 * Timer/channel/speed-mode are hardcoded to LEDC_TIMER_0/LEDC_CHANNEL_0/
 * LEDC_LOW_SPEED_MODE here, matching hal_pwm.h's own comment that ESP
 * mode/timer/channel selection stays a backend concern since nothing else
 * in the tree uses LEDC -- there is only ever one channel to abstract.
 *
 * No interface mismatch found: hal_pwm.h's three calls (init/set_duty/
 * deinit) cover backlight_pwm.c's real, sole use exactly, including the
 * fold of ledc_set_duty()+ledc_update_duty() into one call.
 */
#include "hal_pwm.h"

#include "driver/ledc.h"
#include "soc/soc_caps.h"

#include "hal_esp_common.h"

#define HAL_PWM_ESP_TIMER   LEDC_TIMER_0
#define HAL_PWM_ESP_CHANNEL LEDC_CHANNEL_0
#define HAL_PWM_ESP_MODE    LEDC_LOW_SPEED_MODE

/* Set at hal_pwm_init() time, read back by hal_pwm_set_duty()'s percent-to-
 * raw-duty scaling -- moved verbatim from backlight_pwm.c's duty_for_
 * percent(), which computed this same "(1 << bits) - 1" ceiling against its
 * own compile-time BACKLIGHT_LEDC_DUTY_RES constant. */
static uint32_t s_duty_max;
static bool     s_initialized;

hal_status_t hal_pwm_init(const hal_pwm_cfg_t *cfg) {
    if (!cfg || cfg->duty_resolution_bits == 0 ||
        cfg->duty_resolution_bits > SOC_LEDC_TIMER_BIT_WIDTH ||
        cfg->start_duty_percent > 100) {
        /* Real ceiling on this chip's LEDC timers (ESP32-S3:
         * SOC_LEDC_TIMER_BIT_WIDTH == 14), not the generic ledc_timer_bit_t
         * enum's 1..20 range -- a duty_resolution_bits beyond 14 is a valid
         * enumerator but not a valid config on this SoC, and letting it
         * through to ledc_timer_config() defers the rejection to the IDF
         * call instead of catching it here per hal_pwm.h's contract. */
        return HAL_INVALID_ARG;
    }

    ledc_timer_config_t timer_cfg = {
        .speed_mode = HAL_PWM_ESP_MODE,
        .duty_resolution = (ledc_timer_bit_t)cfg->duty_resolution_bits,
        .timer_num = HAL_PWM_ESP_TIMER,
        .freq_hz = cfg->freq_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        return hal_esp_err_to_status(err);
    }

    s_duty_max = (1u << cfg->duty_resolution_bits) - 1u;

    /* Start at start_duty_percent / hpoint 0, matching backlight_pwm.c's own
     * init order (timer first, then channel) AND its real channel config,
     * which sets `.duty = duty_for_percent(CONFIG_KILNCTL_BACKLIGHT_ON_PERCENT)`
     * directly rather than starting at 0 and relying on a follow-up
     * hal_pwm_set_duty() -- the panel must not start dark if the flying wire
     * IS fitted. Same pct-to-raw rule as hal_pwm_set_duty() below (100 ->
     * duty_max verbatim, 0 -> 0, else truncated pct*max/100); duplicated
     * rather than shared because s_initialized is not yet true here. */
    uint32_t start_duty;
    if (cfg->start_duty_percent >= 100) {
        start_duty = s_duty_max;
    } else if (cfg->start_duty_percent == 0) {
        start_duty = 0;
    } else {
        start_duty = (uint32_t)cfg->start_duty_percent * s_duty_max / 100u;
    }

    ledc_channel_config_t chan_cfg = {
        .gpio_num = cfg->gpio_num,
        .speed_mode = HAL_PWM_ESP_MODE,
        .channel = HAL_PWM_ESP_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = HAL_PWM_ESP_TIMER,
        .duty = start_duty,
        .hpoint = 0,
    };
    err = ledc_channel_config(&chan_cfg);
    if (err != ESP_OK) {
        return hal_esp_err_to_status(err);
    }

    s_initialized = true;
    return HAL_OK;
}

/* Folds ledc_set_duty()+ledc_update_duty() -- backlight_pwm.c's apply_
 * duty() never calls one without the other. duty_percent-to-raw scaling:
 * duty_for_percent()'s exact rule (100 -> duty_max verbatim rather than a
 * rounded pct*max/100, which could shave one LSB off full-scale; 0 -> 0;
 * otherwise pct*max/100 truncated). */
hal_status_t hal_pwm_set_duty(uint8_t duty_percent) {
    if (!s_initialized) {
        return HAL_NOT_READY;
    }
    if (duty_percent > 100) {
        return HAL_INVALID_ARG;
    }

    uint32_t duty;
    if (duty_percent >= 100) {
        duty = s_duty_max;
    } else if (duty_percent == 0) {
        duty = 0;
    } else {
        duty = (uint32_t)duty_percent * s_duty_max / 100u;
    }

    esp_err_t err = ledc_set_duty(HAL_PWM_ESP_MODE, HAL_PWM_ESP_CHANNEL, duty);
    if (err != ESP_OK) {
        return hal_esp_err_to_status(err);
    }
    err = ledc_update_duty(HAL_PWM_ESP_MODE, HAL_PWM_ESP_CHANNEL);
    return hal_esp_err_to_status(err);
}

/* No real caller today (backlight PWM runs for the board's life once
 * armed) -- ledc_stop() releases the channel by holding it at idle level 0,
 * the closest LEDC-native "give this back" operation; there is no
 * ledc_channel_deconfig()/ledc_timer_deconfig() API on this IDF version. */
hal_status_t hal_pwm_deinit(void) {
    if (!s_initialized) {
        return HAL_NOT_READY;
    }
    esp_err_t err = ledc_stop(HAL_PWM_ESP_MODE, HAL_PWM_ESP_CHANNEL, 0);
    s_initialized = false;
    return hal_esp_err_to_status(err);
}
