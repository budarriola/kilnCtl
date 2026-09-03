// Host-test stub -- see stubs/esp_err.h for why these exist. Added for
// backlight_pwm.c's host test (test_backlight_pwm.c), which #includes
// backlight_pwm.c directly (same convention as test_boot_button.c/
// test_boot_guard.c). CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE is off in the host
// build (not defined by stubs/sdkconfig.h, same as every other default-off
// KILNCTL flag there), so backlight_pwm.c's #if CONFIG_KILNCTL_BACKLIGHT_PWM_
// ENABLE block -- the only place these types/functions are actually used --
// never compiles here; this header only has to exist and parse so the
// unconditional `#include "driver/ledc.h"` at the top of backlight_pwm.c
// succeeds. Values/behavior here are never asserted against by any test --
// only "the identifiers exist and the header parses" matters, same
// convention as stubs/driver/gpio.h.
#ifndef TEST_STUB_DRIVER_LEDC_H
#define TEST_STUB_DRIVER_LEDC_H

#include "esp_err.h"

typedef enum { LEDC_LOW_SPEED_MODE = 0 } ledc_mode_t;
typedef enum { LEDC_TIMER_0 = 0 } ledc_timer_t;
typedef enum { LEDC_CHANNEL_0 = 0 } ledc_channel_t;
typedef enum { LEDC_TIMER_13_BIT = 13 } ledc_timer_bit_t;
typedef enum { LEDC_INTR_DISABLE = 0 } ledc_intr_type_t;
typedef enum { LEDC_AUTO_CLK = 0 } ledc_clk_cfg_t;

typedef struct {
    ledc_mode_t speed_mode;
    ledc_timer_bit_t duty_resolution;
    ledc_timer_t timer_num;
    uint32_t freq_hz;
    ledc_clk_cfg_t clk_cfg;
} ledc_timer_config_t;

typedef struct {
    int gpio_num;
    ledc_mode_t speed_mode;
    ledc_channel_t channel;
    ledc_intr_type_t intr_type;
    ledc_timer_t timer_sel;
    uint32_t duty;
    int hpoint;
} ledc_channel_config_t;

static inline esp_err_t ledc_timer_config(const ledc_timer_config_t *cfg)
{
    (void)cfg;
    return ESP_OK;
}

static inline esp_err_t ledc_channel_config(const ledc_channel_config_t *cfg)
{
    (void)cfg;
    return ESP_OK;
}

static inline esp_err_t ledc_set_duty(ledc_mode_t mode, ledc_channel_t chan, uint32_t duty)
{
    (void)mode;
    (void)chan;
    (void)duty;
    return ESP_OK;
}

static inline esp_err_t ledc_update_duty(ledc_mode_t mode, ledc_channel_t chan)
{
    (void)mode;
    (void)chan;
    return ESP_OK;
}

#endif // TEST_STUB_DRIVER_LEDC_H
