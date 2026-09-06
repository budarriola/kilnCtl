/* fake_pwm.c -- host fake backend for hal_pwm.h. See fake_pwm.h. */
#include "fake_pwm.h"

#include <string.h>

static bool          s_initialized = false;
static hal_pwm_cfg_t s_cfg;
static uint8_t       s_duty_history[FAKE_PWM_MAX_DUTY_HISTORY];
static uint32_t      s_duty_history_count = 0;

void fake_pwm_reset_all(void) {
    s_initialized = false;
    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(s_duty_history, 0, sizeof(s_duty_history));
    s_duty_history_count = 0;
}

bool fake_pwm_is_initialized(void) {
    return s_initialized;
}

bool fake_pwm_get_init_cfg(hal_pwm_cfg_t *out) {
    if (out == NULL || !s_initialized) {
        return false;
    }
    *out = s_cfg;
    return true;
}

uint32_t fake_pwm_get_duty_history_count(void) {
    return s_duty_history_count;
}

uint8_t fake_pwm_get_duty_history(uint32_t index) {
    if (index >= s_duty_history_count) {
        return 0xFF;
    }
    return s_duty_history[index];
}

uint8_t fake_pwm_get_last_duty(void) {
    if (s_duty_history_count == 0) {
        return 0xFF;
    }
    return s_duty_history[s_duty_history_count - 1];
}

/* --- hal_pwm.h implementation --- */

hal_status_t hal_pwm_init(const hal_pwm_cfg_t *cfg) {
    /* Mirrors hal_pwm_esp.c's own validation exactly: 1..20 bits, the real
     * LEDC_TIMER_BIT_MAX ceiling on this port's low-speed timers. */
    if (cfg == NULL || cfg->duty_resolution_bits == 0 || cfg->duty_resolution_bits > 20) {
        return HAL_INVALID_ARG;
    }
    s_cfg = *cfg;
    s_initialized = true;
    s_duty_history_count = 0;
    return HAL_OK;
}

hal_status_t hal_pwm_set_duty(uint8_t duty_percent) {
    if (!s_initialized) {
        return HAL_NOT_READY;
    }
    if (duty_percent > 100) {
        return HAL_INVALID_ARG;
    }
    if (s_duty_history_count < FAKE_PWM_MAX_DUTY_HISTORY) {
        s_duty_history[s_duty_history_count++] = duty_percent;
    }
    return HAL_OK;
}

hal_status_t hal_pwm_deinit(void) {
    if (!s_initialized) {
        return HAL_NOT_READY;
    }
    s_initialized = false;
    return HAL_OK;
}
