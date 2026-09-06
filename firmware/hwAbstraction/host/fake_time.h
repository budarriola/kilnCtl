/* fake_time.h -- host fake backend for hal_time.h (Phase 2).
 *
 * See docs/HW_ABSTRACTION_PLAN.md "Host fakes (Phase 2 specs)" -- fake_time
 * (must): explicit advance; quantized/dithered inputs -- the idealized-input
 * bug class must not be reintroduced (project memory:
 * project_idealized_test_input_bug_class.md).
 *
 * Model: a single global monotonic microsecond counter, manually advanced
 * by the test (never a real sleep, never tied to wall-clock time). Tests
 * that need to prove behavior under non-uniform timing MUST advance by
 * varied, non-round amounts (e.g. 987us, 3001us) rather than always the
 * same step -- a fake that only ever advances by idealized round numbers
 * hides the same class of bug an idealized synthetic-data test does.
 *
 * hal_time_now_us/ms carry no handle and no init call, matching hal_time.h's
 * "stateless, callable from any task" contract -- there is nothing to reset
 * per-handle, only the global clock via fake_time_reset_all().
 */
#ifndef KILNCTL_FAKE_TIME_H
#define KILNCTL_FAKE_TIME_H

#include <stdint.h>

#include "hal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Resets the fake clock to 0. Call between test cases. */
void fake_time_reset_all(void);

/* Advances the clock forward by us/ms. Forward-only, matching hal_time.h's
 * "the host fake may inject jumps (forward only)" -- there is no
 * fake_time_set_us()/rewind, deliberately, so a test cannot construct a
 * clock going backward, which real hardware never does either. */
void fake_time_advance_us(uint64_t us);
void fake_time_advance_ms(uint32_t ms);

/* Current fake clock value, in microseconds -- equivalent to
 * hal_time_now_us() but named for test readability at call sites that
 * assert on elapsed time without implying they're testing the HAL entry
 * point itself. */
uint64_t fake_time_now_us(void);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_FAKE_TIME_H */
