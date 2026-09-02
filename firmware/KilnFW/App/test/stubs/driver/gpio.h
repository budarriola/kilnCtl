// Host-test stub -- see stubs/esp_err.h for why these exist. Originally
// empty (added 2026-08-21 for wifi_prov.c's host tests, which only reach
// this header through settings.h and never expand a GPIO-typed macro).
//
// Extended 2026-08-22 for boot_button.c's host tests (test_boot_button.c),
// which #includes boot_button.c directly (same convention as
// test_boot_guard.c/test_watchdog_cfg.c) -- the pure boot_button_step()/
// state_refuses_bypass() logic under test never calls gpio_config()/
// gpio_get_level() (only boot_button_start()/the poll task do, and the
// tests never call either), but the whole translation unit still has to
// compile and LINK, so these need real (if trivial) definitions, not just
// declarations. Values/behavior here are never asserted against by any
// test -- only "the identifiers exist and the call compiles" matters.
#ifndef TEST_STUB_DRIVER_GPIO_H
#define TEST_STUB_DRIVER_GPIO_H

#include "esp_err.h"

typedef int gpio_num_t;
#define GPIO_NUM_0 0

typedef enum { GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2 } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE = 1, GPIO_PULLUP_ONLY = 2 } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE = 1 } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE = 0 } gpio_int_type_t;

typedef struct {
    unsigned long long pin_bit_mask;
    gpio_mode_t         mode;
    gpio_pullup_t        pull_up_en;
    gpio_pulldown_t       pull_down_en;
    gpio_int_type_t        intr_type;
} gpio_config_t;

static inline esp_err_t gpio_config(const gpio_config_t *cfg)
{
    (void)cfg;
    return ESP_OK;
}

static inline int gpio_get_level(gpio_num_t gpio)
{
    (void)gpio;
    return 1; /* not pressed (ACTIVE LOW) -- never read by any host test */
}

/* Added for safety_link.c's host build (App/test/test_safety_link_compile.c)
 * -- values/behavior here are never asserted against; only "the identifier
 * exists and the call compiles/links" matters, same convention as the rest
 * of this file. */
#define GPIO_IS_VALID_OUTPUT_GPIO(gpio) (1)

/* DISPLAY_ST7796_PLAN.md 9.4: test_esp_spi_owner.c proves cs_pin < 0 skips
 * esp_spi_owner.c's bit-banging entirely (CONFIG_KILNCTL_SPI_HARDWARE_CS's
 * precondition) by counting calls here, rather than only "compiles and
 * links" like the rest of this stub file. `static`, NOT `extern` -- unlike
 * g_stub_spi_transmit_calls (spi_master.h; spi_device_transmit() is only
 * ever reachable through esp_spi_owner.c, which only ONE test executable
 * links), gpio_set_level() is called from many drivers spread across many
 * separate test executables (safety_link.c, boot_button.c, ...) that never
 * link test_esp_spi_owner.c -- an extern here left those with an unresolved
 * symbol (found building this pass: `LNK2019 g_stub_gpio_set_level_calls`
 * in the safety_link test executable). `static` gives every translation
 * unit that includes this header its own private counter, which is exactly
 * what a self-contained assertion inside test_esp_spi_owner.c's own TU
 * needs -- no cross-TU definition to link. */
static int g_stub_gpio_set_level_calls = 0;

static inline esp_err_t gpio_set_level(gpio_num_t gpio, int level)
{
    (void)gpio; (void)level;
    g_stub_gpio_set_level_calls++;
    return ESP_OK;
}

static inline esp_err_t gpio_set_pull_mode(gpio_num_t gpio, gpio_pullup_t pull)
{
    (void)gpio; (void)pull;
    return ESP_OK;
}

#endif // TEST_STUB_DRIVER_GPIO_H
