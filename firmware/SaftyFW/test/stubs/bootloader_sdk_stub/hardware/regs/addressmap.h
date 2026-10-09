// Host stub: XIP_BASE is the address of a RAM array standing in for the 2 MiB
// flash, so recovery_update.c's `(const uint8_t *)(XIP_BASE + offset)` reads
// work unchanged on a 64-bit host. The test owns g_fake_flash.
#ifndef BOOTLOADER_SDK_STUB_ADDRESSMAP_H
#define BOOTLOADER_SDK_STUB_ADDRESSMAP_H
#include <stdint.h>
extern uint8_t g_fake_flash[];
#define XIP_BASE ((uintptr_t)g_fake_flash)
#endif
