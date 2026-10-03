#ifndef BOOTLOADER_SDK_STUB_SYNC_H
#define BOOTLOADER_SDK_STUB_SYNC_H
#include <stdint.h>
static inline uint32_t save_and_disable_interrupts(void) { return 0u; }
static inline void restore_interrupts(uint32_t status) { (void)status; }
#endif
