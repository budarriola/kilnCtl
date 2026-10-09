/* hal_adc.h -- portable ADC interface. Pico-only (current sense).
 *
 * See docs/HW_ABSTRACTION.md "hal_adc -- pico-only". Wraps
 * current_task.c and current_sense.c's raw RP2040 ADC use. No ESP consumer
 * exists (the ESP path has no equivalent current-sense ADC), so this
 * interface has no opaque bus/device handle at all -- the RP2040 ADC is a
 * single global peripheral with one active channel, matching the pico-sdk
 * shape (adc_init/adc_select_input) directly. If a second ADC-bearing
 * backend ever appears, a handle can be added without breaking this API
 * (all functions would gain a first handle parameter).
 *
 * Threading/ownership contract:
 *  - Single-writer: one task selects a channel and reads it; channel
 *    selection and the following read must not be interleaved with another
 *    caller's selection on the same peripheral. Callers serialize
 *    themselves (today's current_task/current_sense are already the sole
 *    users).
 *  - Oversampling and discard-first-sample policy stay ABOVE this
 *    interface, in the caller -- hal_adc_read_raw returns one raw sample,
 *    no filtering.
 *  - The host fake_adc (Phase 2) is the ONLY off-target route to exercise
 *    the current-sense guards (S3/S4/S9/S11/S14): no CTs are fitted on the
 *    bench board, so hardware cannot exercise them today.
 */
#ifndef KILNCTL_HAL_ADC_H
#define KILNCTL_HAL_ADC_H

#include <stdint.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

hal_status_t hal_adc_init(void);
hal_status_t hal_adc_gpio_enable(int pin);
hal_status_t hal_adc_select(int channel);
uint16_t     hal_adc_read_raw(void);

/* Bounded variant of hal_adc_read_raw(): same single-shot conversion, but
 * the wait for "conversion done" is capped at a documented, generous spin
 * count instead of spinning forever. Returns HAL_OK with *out_counts set on
 * a normal conversion, or HAL_TIMEOUT (out_counts left unwritten) if the
 * bound is exceeded -- a caller must treat HAL_TIMEOUT as "no reading this
 * attempt", never substitute 0 or a stale value itself (see current_sense.c
 * for how the accumulator layer surfaces this as a degraded window rather
 * than a silently-wrong average). Added so current_sense.c's acquisition
 * path can honor "every hardware read path bounded, with a stated bound and
 * named degraded-result behavior on timeout" without ever holding a lock
 * across a hardware wait -- see hal_adc_pico.c / fake_adc.c for the bound
 * each backend enforces. */
hal_status_t hal_adc_read_raw_bounded(uint16_t *out_counts);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_HAL_ADC_H */
