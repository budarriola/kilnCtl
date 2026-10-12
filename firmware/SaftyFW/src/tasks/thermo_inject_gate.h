// thermo_inject_gate.h -- pure gate for the INJECT_TC bench facility.
// Header-only, no pico-sdk/FreeRTOS includes, so it is host-tested
// (test/test_thermo_inject_gate.c).
//
// 2026-10-09 guard review F1: injection replaces the real thermocouple
// reading every guard sees, so it is allowed ONLY when the safety TC is
// declared not installed AND the relay is not energized/ARMED. Without the
// second term, a (now refused) SET_PARAM safety_tc_installed=0 while K4 was
// ARMED would let the ESP feed the Pico a fake 25 C under a live firing.
#ifndef SAFTYFW_TASKS_THERMO_INJECT_GATE_H
#define SAFTYFW_TASKS_THERMO_INJECT_GATE_H

#include <stdbool.h>
#include <stdint.h>

static inline bool thermo_inject_allowed(uint8_t safety_tc_installed, bool relay_armed_or_energized)
{
    return safety_tc_installed == 0u && !relay_armed_or_energized;
}

#endif
