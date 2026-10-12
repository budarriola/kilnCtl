// Host-test stub (campaign 7) for SX1509.c's hard-reset delay.
#ifndef TEST_STUB_ESP_ROM_SYS_H
#define TEST_STUB_ESP_ROM_SYS_H
#include <stdint.h>
static inline void esp_rom_delay_us(uint32_t us) { (void)us; }
#endif
