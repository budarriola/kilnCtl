/* hal_adc_pico.c -- pico-sdk backend for interface/hal_adc.h.
 *
 * Phase 1b ("adapt"): implements the Phase-0 interface against pico-sdk 2.x
 * (hardware_adc/include/hardware/adc.h). Not wired into any CMakeLists yet
 * -- see firmware/hwAbstraction/test/compile_pico_backends.ps1 for the
 * syntax-only compile check that stands in for that until Phase 1a's real
 * move lands.
 *
 * This is the interface's real, validated consumer (hal_adc.h and
 * docs/HW_ABSTRACTION.md's hal_adc section both describe it as
 * pico-only, wrapping current_task.c/current_sense.c): shape checked
 * against firmware/SaftyFW/src/tasks/current_task.c:98-104
 * (adc_init()/adc_gpio_init() x3 at bring-up) and
 * firmware/SaftyFW/src/current_sense.c:157-165 (adc_select_input() then
 * adc_read(), single-shot, blocking, discard-first-sample policy staying
 * above this interface per hal_adc.h's threading/ownership contract). The
 * mapping is 1:1 -- hal_adc_init/hal_adc_gpio_enable/hal_adc_select/
 * hal_adc_read_raw onto adc_init/adc_gpio_init/adc_select_input/adc_read,
 * no adaptation needed.
 */
#include "hal_adc.h"

#include "hardware/adc.h"

hal_status_t hal_adc_init(void) {
    adc_init();
    return HAL_OK;
}

hal_status_t hal_adc_gpio_enable(int pin) {
    /* adc_gpio_init() disables the digital functions on the pin, matching
     * current_task.c:99's own comment. */
    adc_gpio_init((uint)pin);
    return HAL_OK;
}

hal_status_t hal_adc_select(int channel) {
    adc_select_input((uint)channel);
    return HAL_OK;
}

uint16_t hal_adc_read_raw(void) {
    return adc_read();
}

/* No INTERFACE MISMATCH here: pico-sdk's adc_init/adc_gpio_init/
 * adc_select_input/adc_read is exactly the shape hal_adc.h was written
 * for -- see the header's own comment ("matching the pico-sdk shape...
 * directly"). */
