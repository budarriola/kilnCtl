// Host stub of pico/time.h for the bootloader recovery_update host test. The
// test defines time_us_64() and controls it.
#ifndef BOOTLOADER_SDK_STUB_PICO_TIME_H
#define BOOTLOADER_SDK_STUB_PICO_TIME_H
#include <stdint.h>
uint64_t time_us_64(void);
#endif
