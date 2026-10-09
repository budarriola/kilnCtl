// Host stub of hardware/flash.h. Erase sets 0xFF; program only clears bits
// (flash semantics), so a missing erase shows up as a corrupted image.
#ifndef BOOTLOADER_SDK_STUB_FLASH_H
#define BOOTLOADER_SDK_STUB_FLASH_H
#include <stddef.h>
#include <stdint.h>
#define FLASH_PAGE_SIZE   256u
#define FLASH_SECTOR_SIZE 4096u
#define FLASH_BLOCK_SIZE  65536u
void flash_range_erase(uint32_t flash_offs, size_t count);
void flash_range_program(uint32_t flash_offs, const uint8_t *data, size_t count);
#endif
