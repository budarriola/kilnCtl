// max31856_fault_pin_policy.h -- pure raw-GPIO-level-to-logical-meaning
// mapping for the MAX31856 ~FAULT pin, split out of max31856.c the same way
// max31856_tc_type_policy.h is split out of max31856.c (see that header's own
// comment): no pico-sdk/FreeRTOS/hardware includes, so this predicate is
// host-testable directly (test/test_max31856_fault_pin_policy.c links this .c
// file alone) even though max31856.c itself cannot be (it calls
// hardware/gpio.h's gpio_get()).
//
// Same bug class as discrete_pin_policy.h's E-stop polarity fix (see that
// header's comment): before this split, max31856.c did the polarity check
// inline as `gpio_get(s_fault_gpio) == 0`, buried inside max31856_read()
// alongside SPI transport code, with no direct host test able to reach it.
// This output feeds max31856_reading_t.fault_pin_asserted. CORRECTED
// 2026-09-08 (docs/MAX31856_FAULT_PIN_PLAN.md, and the audit it came from,
// docs/audits/max31856_pin_usage_review_2026-09-08.md): the previous version
// of this comment claimed safety_guards.c's S5 (thermocouple fault) reads
// this field. It does NOT, and never has. S5's bad-read test
// (s5_bad_read_now(), safety_guards.c) reads in->fault_bits -- the MAX31856
// Fault Status register (0Fh) byte carried in over SPI -- together with
// spi_failed / tc_valid / isnan(tc_c). fault_pin_asserted is written in
// max31856.c's max31856_read() and is consumed by NOTHING outside this
// module's own host test: it is not copied into thermo_snapshot_t, and it
// reaches no safety_guard_input_t field. So today the ~FAULT *pin* on the
// safety channel is a diagnostic, not a guard input.
//
// That means a flipped polarity here would NOT silently disable S5 -- S5 is
// unaffected by this predicate entirely. It is still worth getting right and
// worth host-testing (the pin is the intended hardware corroboration path,
// and see the plan doc for the open owner decision on whether to wire it
// into S5 as an additional bad-read term), but do not read this header as
// evidence that a guard depends on it. If you DO wire fault_pin_asserted
// into a guard input, update this comment in the same commit -- the stale
// version of it is exactly what made a decorative pin look load-bearing.
//
// Polarity itself is unchanged and still matters: LOW = asserted, per the
// datasheet and both KilnFW comments cited below.
#ifndef SAFTYFW_MAX31856_FAULT_PIN_POLICY_H
#define SAFTYFW_MAX31856_FAULT_PIN_POLICY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// `fault_gpio_high` is the raw `gpio_get(s_fault_gpio)` reading (converted to
// bool by the caller). Returns true (fault asserted) whenever the pin reads
// LOW -- the MAX31856's ~FAULT output is active-low and open-drain (KilnFW's
// firmware/KilnFW/App/drivers/hw/MAX31856.c line 531: "~FAULT is an active-low
// output from the part with nothing pulling it up [internally]"; MAX31856.h
// line 482: "Current level of the ~FAULT GPIO: true when asserted (low)").
// Returns false (healthy / not asserted) when the pin reads HIGH. Do not
// invert this without re-reading the datasheet and both KilnFW comments
// above -- see this header's own comment for what a flipped polarity does to
// guard S5.
bool max31856_fault_pin_asserted(bool fault_gpio_high);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_MAX31856_FAULT_PIN_POLICY_H
