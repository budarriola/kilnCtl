// persist.h -- the bootloader's single metadata-persist routine, factored out
// of main.c so recovery_update.c (item 10.4's UPDATE_END slot-validation
// write) can reuse it rather than hand-rolling a second metadata writer.
// docs/BOOTLOADER.md section 2's discipline ("the log scheme") applies
// equally to both call sites -- this is the ONE place bootloader/ actually
// calls flash_range_erase()/flash_range_program() against
// BOOTLOADER_METADATA_FLASH_OFFSET/_SIZE, matching TODO.md's completion
// checklist item "Never writes its own region or the config partition --
// main.c only ever calls flash_range_erase()/flash_range_program() against
// BOOTLOADER_METADATA_FLASH_OFFSET/_SIZE" (now also true of recovery_update.c
// via this shared function, not a second implementation).
//
// Bare-metal, single-core, no RTOS -- save_and_disable_interrupts()/
// restore_interrupts() is sufficient here (see main.c's original header
// comment on this exact point: flash_safe_execute()'s multicore lockout
// exists for FreeRTOS SMP builds where core 1 might be running unrelated
// code concurrently, which cannot happen in this bare-metal image).
#ifndef SAFTYFW_BOOTLOADER_PERSIST_H
#define SAFTYFW_BOOTLOADER_PERSIST_H

#include <stddef.h>

#include "metadata.h"

#ifdef __cplusplus
extern "C" {
#endif

// Writes `*meta` to the next free log slot in the metadata sector,
// incrementing seq exactly once first. `latest_slot_index` is whatever
// bootloader_metadata_find_latest() last returned (BOOTLOADER_METADATA_NO_SLOT
// if the sector has never held a valid record). Blocks for the duration of
// the flash write with interrupts disabled -- callers on the recovery-mode
// UART1 RX path must expect the link to go silent for that window, same as
// every other flash operation in this bootloader.
void bootloader_persist_metadata(bootloader_metadata_t *meta, size_t latest_slot_index);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_BOOTLOADER_PERSIST_H
