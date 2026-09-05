/* hal_gpio_esp.c -- ESP-IDF backend for interface/hal_gpio.h.
 *
 * Phase 1b ("adapt"): implements the Phase-0 interface against
 * ESP-IDF v6.0.2 (C:\esp\v6.0.2\esp-idf, esp_driver_gpio/include/driver/
 * gpio.h). Not wired into any CMakeLists yet -- see
 * firmware/hwAbstraction/test/compile_esp_backends.ps1 for the syntax-only
 * compile check that stands in for that until Phase 1a's real move lands.
 *
 * Clients this backend must serve unchanged (docs/HW_ABSTRACTION_PLAN.md
 * "hal_gpio" section): CS/DC/reset/fault pins in panel_spi.c /
 * panel_spi_bringup.c, MAX31856.c (CS + fault), SX1509.c (reset + ~INT
 * pin), safety_link.c (fault line), boot_button.c, and gpio_probe.c's
 * runtime pin scanning via the set_direction/set_pull escape hatch. None
 * of today's ESP call sites use open-drain (gpio_od_enable) or a non-default
 * drive strength (gpio_set_drive_capability), so this backend does not
 * expose either -- see the header-mismatch note at the bottom of this file
 * if a future consumer needs them.
 *
 * Latch-before-direction (hal_gpio.h contract, and
 * docs/HW_ABSTRACTION_PLAN.md's "Init ordering audit"): on this IDF version
 * gpio_set_level() writes out_w1ts/out_w1tc unconditionally
 * (esp_driver_gpio/src/gpio.c -> gpio_ll_set_level(), independent of the
 * pin's current direction) and gpio_set_direction() never touches the OUT
 * register, so calling gpio_set_level() BEFORE gpio_config() switches the
 * pin to OUTPUT is glitch-free on this backend. hal_gpio_init_out() below
 * relies on exactly that ordering; do not reorder it even though it looks
 * unnecessary on other targets.
 *
 * All raw IRQ registration (uart_bridge_io.c's SX1509 ~INT via
 * gpio_isr_handler_add) intentionally stays OUTSIDE this interface per the
 * "clean-room; no IRQ surface in v1" decision in hal_gpio.h and the plan;
 * this backend does not provide any ISR path.
 */
#include "hal_gpio.h"

#include "driver/gpio.h"

#include "hal_esp_common.h"

static gpio_pull_mode_t hal_gpio_esp_pull_mode(hal_gpio_pull_t pull) {
    switch (pull) {
        case HAL_GPIO_PULL_UP:   return GPIO_PULLUP_ONLY;
        case HAL_GPIO_PULL_DOWN: return GPIO_PULLDOWN_ONLY;
        case HAL_GPIO_PULL_NONE:
        default:                 return GPIO_FLOATING;
    }
}

static gpio_mode_t hal_gpio_esp_dir_mode(hal_gpio_dir_t dir) {
    return (dir == HAL_GPIO_DIR_OUT) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
}

hal_status_t hal_gpio_init_out(int num, bool idle_level) {
    /* Level first -- see latch-before-direction note above. This is safe to
     * call before the pin is configured as OUTPUT: gpio_set_level() only
     * touches the output-data register, not the direction/mux config. */
    esp_err_t err = gpio_set_level((gpio_num_t)num, idle_level ? 1u : 0u);
    if (err != ESP_OK) {
        return hal_esp_err_to_status(err);
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << (uint64_t)num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&cfg);
    return hal_esp_err_to_status(err);
}

hal_status_t hal_gpio_init_in(int num, hal_gpio_pull_t pull) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << (uint64_t)num),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (pull == HAL_GPIO_PULL_UP) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (pull == HAL_GPIO_PULL_DOWN) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    return hal_esp_err_to_status(err);
}

hal_status_t hal_gpio_set(int num, bool level) {
    esp_err_t err = gpio_set_level((gpio_num_t)num, level ? 1u : 0u);
    return hal_esp_err_to_status(err);
}

bool hal_gpio_get(int num) {
    return gpio_get_level((gpio_num_t)num) != 0;
}

/* gpio_probe.c only -- see contract note in hal_gpio.h. */
hal_status_t hal_gpio_set_direction(int num, hal_gpio_dir_t dir) {
    esp_err_t err = gpio_set_direction((gpio_num_t)num, hal_gpio_esp_dir_mode(dir));
    return hal_esp_err_to_status(err);
}

hal_status_t hal_gpio_set_pull(int num, hal_gpio_pull_t pull) {
    esp_err_t err = gpio_set_pull_mode((gpio_num_t)num, hal_gpio_esp_pull_mode(pull));
    return hal_esp_err_to_status(err);
}

/* Interface-mismatch notes (docs/HW_ABSTRACTION_PLAN.md asks these to be
 * reported, not silently papered over by widening hal_gpio.h):
 *
 * 1. Open-drain (gpio_od_enable/gpio_od_disable) and non-default drive
 *    strength (gpio_set_drive_capability) have no representation in
 *    hal_gpio.h. No current ESP consumer needs either, so this is not yet a
 *    blocker -- flagged so a future consumer does not silently get a
 *    push-pull, default-strength pin.
 * 2. hal_gpio_init_in()'s hal_gpio_pull_t cannot express
 *    GPIO_PULLUP_PULLDOWN's Diablo6 combined pull; ESP-IDF's gpio_config_t
 *    permits enabling both pull_up_en and pull_down_en simultaneously, and
 *    the interface's three-way enum cannot ask for that. No current ESP
 *    consumer needs it either.
 */
