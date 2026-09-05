/* hal_adc_esp.c -- SPECULATIVE ESP-IDF backend for interface/hal_adc.h.
 *
 * *** See the "INTERFACE MISMATCH" block at the end of this file before
 * *** relying on this backend for anything. Short version: hal_adc.h is
 * *** documented as pico-only ("Pico-only (current sense)... No ESP
 * *** consumer exists") and docs/HW_ABSTRACTION_PLAN.md's hal_adc section
 * *** is titled "hal_adc -- pico-only" and says explicitly it wraps
 * *** current_task.c/current_sense.c, both of which live under
 * *** firmware/SaftyFW/ (the RP2040 safety processor), not firmware/KilnFW
 * *** (the ESP32-S3). There is today no ESP consumer for this file to
 * *** preserve behavior from, and the interface's shape (no unit, no
 * *** channel-config call, no calibration handle) cannot express
 * *** ESP-IDF's adc_oneshot/adc_cali model without widening it. This file
 * *** exists only because it was asked for; it is not wired into any
 * *** CMakeLists (Phase 1a is move-only) and should not be treated as a
 * *** validated Phase-1b deliverable the way hal_gpio_esp.c is.
 *
 * Modeled as closely as the interface allows on the pico shape it was
 * written for (adc_init/adc_gpio_init/adc_select_input/adc_read), using
 * ESP-IDF v6.0.2's esp_adc/adc_oneshot.h on a single global ADC_UNIT_1,
 * matching the interface's documented "single global peripheral with one
 * active channel" model and its "no filtering, no calibration -- one raw
 * sample" contract (hal_adc.h's threading/ownership comment).
 */
#include "hal_adc.h"

#include "esp_adc/adc_oneshot.h"

#include "hal_esp_common.h"

/* No consumer exists to pin these down (see mismatch note below), so a
 * default attenuation/bitwidth is chosen here rather than left for a
 * per-channel config call the interface does not offer. ADC_ATTEN_DB_12 is
 * the widest input range (~0-2.5V on the S3); ADC_BITWIDTH_DEFAULT resolves
 * to the unit's native width (12-bit on S3). */
#define HAL_ADC_ESP_ATTEN    ADC_ATTEN_DB_12
#define HAL_ADC_ESP_BITWIDTH ADC_BITWIDTH_DEFAULT

static adc_oneshot_unit_handle_t s_unit = NULL;
static adc_channel_t s_selected_channel = 0;
static bool s_selected_valid = false;

hal_status_t hal_adc_init(void) {
    if (s_unit != NULL) {
        return HAL_OK; /* ALREADY_INIT -> HAL_OK, per hal_spi/hal_i2c precedent
                         * in docs/HW_ABSTRACTION_PLAN.md Phase 0. */
    }
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_cfg, &s_unit);
    return hal_esp_err_to_status(err);
}

/* Interface names this "gpio_enable", matching the pico shape
 * (adc_gpio_init(pin) disables the pin's digital function). On ESP the
 * closest equivalent is resolving the IO pin to its (unit, channel) pair
 * and configuring that channel -- there is no separate "just make this pin
 * analog" step distinct from channel config, so both happen here. */
hal_status_t hal_adc_gpio_enable(int pin) {
    if (s_unit == NULL) {
        return HAL_NOT_READY;
    }
    adc_unit_t unit_id;
    adc_channel_t channel;
    esp_err_t err = adc_oneshot_io_to_channel(pin, &unit_id, &channel);
    if (err != ESP_OK) {
        return hal_esp_err_to_status(err);
    }
    if (unit_id != ADC_UNIT_1) {
        /* This backend only ever brings up ADC_UNIT_1 -- see mismatch note
         * below (no unit parameter in the interface). */
        return HAL_INVALID_ARG;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = HAL_ADC_ESP_ATTEN,
        .bitwidth = HAL_ADC_ESP_BITWIDTH,
    };
    err = adc_oneshot_config_channel(s_unit, channel, &chan_cfg);
    return hal_esp_err_to_status(err);
}

/* Interface passes "channel" as a bare int, matching the pico
 * adc_select_input(0..3) shape. This backend treats it as an adc_channel_t
 * on ADC_UNIT_1 directly (ADC_CHANNEL_0..9 on S3) -- callers must have
 * already brought the same channel up via hal_adc_gpio_enable(). */
hal_status_t hal_adc_select(int channel) {
    if (s_unit == NULL) {
        return HAL_NOT_READY;
    }
    s_selected_channel = (adc_channel_t)channel;
    s_selected_valid = true;
    return HAL_OK;
}

uint16_t hal_adc_read_raw(void) {
    if (s_unit == NULL || !s_selected_valid) {
        return 0;
    }
    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_unit, s_selected_channel, &raw);
    if (err != ESP_OK || raw < 0) {
        return 0;
    }
    if (raw > 0xFFFF) {
        raw = 0xFFFF; /* S3's ADC is <=12-bit; this only guards the cast. */
    }
    return (uint16_t)raw;
}

/* INTERFACE MISMATCH -- reported per task instructions rather than silently
 * widening hal_adc.h:
 *
 * 1. No ESP consumer. hal_adc.h's own header comment and
 *    docs/HW_ABSTRACTION_PLAN.md's hal_adc section both say this interface
 *    wraps current_task.c/current_sense.c -- both are SaftyFW (RP2040)
 *    files, not KilnFW/ESP. There is no current_task.c or current_sense.c
 *    under firmware/KilnFW, and grep of firmware/KilnFW/App/drivers found
 *    no adc_oneshot/adc_cali/legacy adc1_* call site to preserve behavior
 *    from. This file has no real-world caller to validate against.
 * 2. No unit selection. ESP32-S3 has two independent ADC units
 *    (ADC_UNIT_1/ADC_UNIT_2); hal_adc_init()/hal_adc_gpio_enable() take no
 *    unit parameter, so this backend hardcodes ADC_UNIT_1 and fails closed
 *    (HAL_INVALID_ARG) if a pin resolves to ADC_UNIT_2. The pico model this
 *    interface was shaped for really does have exactly one ADC peripheral,
 *    so the interface's "single global peripheral" framing is simply wrong
 *    for this vendor.
 * 3. No per-channel attenuation/bitwidth config call. ESP-IDF requires
 *    adc_oneshot_config_channel(unit, channel, {atten, bitwidth}) before a
 *    channel can be read; the interface has no call shaped like that, so
 *    hal_adc_gpio_enable() picks a fixed default (ADC_ATTEN_DB_12,
 *    ADC_BITWIDTH_DEFAULT) for every channel. A real consumer needing a
 *    different input range per channel cannot express that through this
 *    interface today.
 * 4. No calibration handle. docs/HW_ABSTRACTION_PLAN.md's Phase 1b task
 *    description asks this backend to preserve "calibration scheme
 *    curve-fitting vs line-fitting" -- that is an ESP-IDF adc_cali_handle_t
 *    concept (adc_cali_create_scheme_curve_fitting on S3,
 *    ...line_fitting on chips without curve-fitting support) requiring
 *    esp_adc/adc_cali.h + adc_cali_scheme.h, a separate handle, and
 *    adc_cali_raw_to_voltage()/adc_oneshot_get_calibrated_result() call
 *    sites. hal_adc.h's hal_adc_read_raw() is documented to return exactly
 *    "one raw sample, no filtering" with oversampling policy staying above
 *    the interface -- there is no calibrated-reading entry point at all, so
 *    a curve-fitting-vs-line-fitting choice has nothing to attach to
 *    without widening the interface (new function returning mV, or a new
 *    init-time scheme parameter). Not implemented here for that reason.
 */
