// Host stub of pico/time.h for test_safety_core_host.c (Campaign 1). The
// clock behind get_absolute_time() is owned by the test.
#ifndef SAFTYFW_TEST_STUB_SAFETY_CORE_PICO_TIME_H
#define SAFTYFW_TEST_STUB_SAFETY_CORE_PICO_TIME_H
#include <stdint.h>
typedef struct { uint64_t us; } absolute_time_t;
absolute_time_t get_absolute_time(void);
uint32_t to_ms_since_boot(absolute_time_t t);
uint32_t time_us_32(void);
uint64_t time_us_64(void);
#endif
