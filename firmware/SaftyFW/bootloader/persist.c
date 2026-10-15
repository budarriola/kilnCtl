// persist.c -- see persist.h. Byte-for-byte the same body as main.c's
// original static persist_metadata(), moved here unchanged so
// recovery_update.c can call it too (item 10.4).
#include "persist.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"

#include "flash_layout.h"

bool bootloader_persist_metadata(bootloader_metadata_t *meta, size_t latest_slot_index)
{
    meta->seq = meta->seq + 1u;

    size_t next_write_slot = bootloader_metadata_next_write_slot(latest_slot_index);
    bool needs_erase = bootloader_metadata_next_write_needs_erase(latest_slot_index);
    bool needs_backup = bootloader_metadata_wrap_needs_backup(latest_slot_index);

    uint8_t record[BOOTLOADER_METADATA_RECORD_LEN];
    bootloader_metadata_pack(meta, record); // pure byte packing, no flash I/O

    const uint8_t *main_xip = (const uint8_t *)(XIP_BASE + BOOTLOADER_METADATA_FLASH_OFFSET);
    const uint8_t *backup_xip = (const uint8_t *)(XIP_BASE + BOOTLOADER_METADATA_BACKUP_FLASH_OFFSET);

    // See this file's header comment / main.c's original comment: single-core
    // bare-metal image, no ISRs registered, so plain
    // save_and_disable_interrupts()/restore_interrupts() is sufficient.
    uint32_t ints = save_and_disable_interrupts();
    if (needs_backup) {
        // Save the newest record before the only copy is erased. Torn at any
        // point here: the main sector is untouched. Torn after the main erase
        // starts: the backup holds the newest record.
        const uint8_t *newest = main_xip + latest_slot_index * BOOTLOADER_METADATA_RECORD_LEN;
        flash_range_erase(BOOTLOADER_METADATA_BACKUP_FLASH_OFFSET, BOOTLOADER_METADATA_BACKUP_FLASH_SIZE);
        flash_range_program(BOOTLOADER_METADATA_BACKUP_FLASH_OFFSET, newest,
                             BOOTLOADER_METADATA_RECORD_LEN);
        if (memcmp(backup_xip, newest, BOOTLOADER_METADATA_RECORD_LEN) != 0) {
            restore_interrupts(ints);
            return false; // no safe copy: keep the main log (and its only record) intact
        }
    }
    if (needs_erase) {
        flash_range_erase(BOOTLOADER_METADATA_FLASH_OFFSET, BOOTLOADER_METADATA_FLASH_SIZE);
    }
    const uint8_t *dst = main_xip + (size_t)next_write_slot * BOOTLOADER_METADATA_RECORD_LEN;
    flash_range_program(BOOTLOADER_METADATA_FLASH_OFFSET +
                             (uint32_t)next_write_slot * BOOTLOADER_METADATA_RECORD_LEN,
                         record, BOOTLOADER_METADATA_RECORD_LEN);
    bool ok = memcmp(dst, record, BOOTLOADER_METADATA_RECORD_LEN) == 0;
    restore_interrupts(ints);
    return ok;
}
