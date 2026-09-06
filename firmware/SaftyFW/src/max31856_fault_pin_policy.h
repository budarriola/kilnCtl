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
// This output feeds max31856_reading_t.fault_pin_asserted, which safety_
// guards.c's S5 (thermocouple fault) reads. A flipped polarity here would
// make S5 either never trip on a genuine MAX31856 fault (open TC, over/
// undervoltage, or a bad cold-junction reading -- exactly what ~FAULT exists
// to report) or trip permanently on a healthy, idle part -- either silently
// disabling S5 or making it impossible to arm, the same two failure shapes
// the E-stop bug produced for S7.
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
