// persist.c -- see persist.h. Byte-for-byte the same body as main.c's
// original static persist_metadata(), moved here unchanged so
// recovery_update.c can call it too (item 10.4).
#include "persist.h"

#include "hardware/flash.h"
#include "hardware/sync.h"

#include "flash_layout.h"

void bootloader_persist_metadata(bootloader_metadata_t *meta, size_t latest_slot_index)
{
    meta->seq = meta->seq + 1u;

    size_t next_write_slot = bootloader_metadata_next_write_slot(latest_slot_index);
    bool needs_erase = bootloader_metadata_next_write_needs_erase(latest_slot_index);

    uint8_t record[BOOTLOADER_METADATA_RECORD_LEN];
    bootloader_metadata_pack(meta, record); // pure byte packing, no flash I/O

    // See this file's header comment / main.c's original comment: single-core
    // bare-metal image, no ISRs registered, so plain
    // save_and_disable_interrupts()/restore_interrupts() is sufficient.
    uint32_t ints = save_and_disable_interrupts();
    if (needs_erase) {
        flash_range_erase(BOOTLOADER_METADATA_FLASH_OFFSET, BOOTLOADER_METADATA_FLASH_SIZE);
    }
    flash_range_program(BOOTLOADER_METADATA_FLASH_OFFSET +
                             (uint32_t)next_write_slot * BOOTLOADER_METADATA_RECORD_LEN,
                         record, BOOTLOADER_METADATA_RECORD_LEN);
    restore_interrupts(ints);
}
