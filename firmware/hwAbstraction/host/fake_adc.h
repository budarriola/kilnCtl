/* fake_adc.h -- host fake backend for hal_adc.h (Phase 2).
 *
 * See docs/HW_ABSTRACTION_PLAN.md "Host fakes (Phase 2 specs)" -- fake_adc
 * (must): scripted sample sequences with realistic quantization; the only
 * route to the current-sense guards (S3/S4/S9/S11/S14) since no CTs are
 * fitted on the bench board.
 *
 * hal_adc.h has no bus/device handle (single global RP2040 peripheral), so
 * this fake is a single global singleton too -- call fake_adc_reset()
 * between test cases.
 */
#ifndef KILNCTL_FAKE_ADC_H
#define KILNCTL_FAKE_ADC_H

#include <stddef.h>
#include <stdbool.h>

#include "hal_adc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_ADC_NUM_CHANNELS 8
#define FAKE_ADC_SCRIPT_CAP   256

void fake_adc_reset(void);

/* Queues `count` raw samples (caller supplies already-quantized values,
 * e.g. 12-bit 0..4095, matching hal_adc_read_raw's realistic range) to be
 * returned in order by hal_adc_read_raw() while `channel` is selected.
 * Returns HAL_INVALID_ARG for an out-of-range channel or NULL samples with
 * nonzero count, HAL_NO_MEM if the channel's queue is already at
 * FAKE_ADC_SCRIPT_CAP (buffer overflow). */
hal_status_t fake_adc_script_samples(int channel, const uint16_t *samples, size_t count);

size_t fake_adc_pending_count(int channel);
bool   fake_adc_is_initialized(void);
int    fake_adc_current_channel(void); /* -1 if none selected / not initialized */
bool   fake_adc_is_gpio_enabled(int pin);
size_t fake_adc_read_count(void); /* total hal_adc_read_raw() calls made */

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_FAKE_ADC_H */
