// Host stub of pico/time.h for the current_sense.c and thermo_task.c host builds
// (guard audit 2026-10-14 A2: snapshot timestamps now come from the hardware
// timer, to_ms_since_boot(get_absolute_time()), not the FreeRTOS tick). Same
// type and include guard as stubs/safety_core_host/pico/time.h so the two never
// both define it. The clock behind it is owned by the test.
#ifndef SAFTYFW_TEST_STUB_SAFETY_CORE_PICO_TIME_H
#define SAFTYFW_TEST_STUB_SAFETY_CORE_PICO_TIME_H
#include <stdint.h>
typedef struct { uint64_t us; } absolute_time_t;
absolute_time_t get_absolute_time(void);
uint32_t to_ms_since_boot(absolute_time_t t);
#endif
