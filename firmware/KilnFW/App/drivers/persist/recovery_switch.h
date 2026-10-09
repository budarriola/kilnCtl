// recovery_switch.h -- verify-then-select the `recovery` (factory-subtype)
// partition as the next boot target. docs/OTA_SINGLE_SLOT_PLAN.md section 4.
//
// ONE implementation shared by both ways into the recovery image:
//   - the deliberate route, POST /api/ota/esp/recovery_boot
//     (ota_http_recovery.c), and
//   - the boot_guard threshold path (recovery_switch_at_boot_threshold(),
//     called from main_boot_early.c right after boot_guard_init()).
//
// Mechanism is stock IDF: esp_image_verify() the recovery partition first
// (a full checksum + hash verify, nothing written), and only if it verifies
// call esp_ota_set_boot_partition(factory), which itself re-verifies and then
// erases otadata so the bootloader falls through to factory. No otadata blob
// is hand-written. A failed verify returns before anything is touched.
//
// Target-only (recovery_switch.c). The decision of WHEN to switch at the
// threshold is the pure boot_guard_decide_recovery_route() in boot_guard.h.
#ifndef RECOVERY_SWITCH_H
#define RECOVERY_SWITCH_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RECOVERY_SWITCH_OK = 0,           /* boot target now points at recovery; caller reboots */
    RECOVERY_SWITCH_NOT_PRESENT,      /* no factory-subtype partition in this table */
    RECOVERY_SWITCH_RUNNING_FACTORY,  /* old layout: the running image IS the factory partition */
    RECOVERY_SWITCH_INVALID,          /* recovery partition does not verify as a bootable image */
    RECOVERY_SWITCH_SET_FAILED        /* verify passed but esp_ota_set_boot_partition() failed */
} recovery_switch_result_t;

/* Verify the recovery image and select it as the next boot target. `msg`
 * (may be NULL) receives a short human-readable refusal/success sentence,
 * JSON-safe (no quotes or backslashes). Never reboots. */
recovery_switch_result_t recovery_switch_select_boot(char *msg, size_t cap);

/* Put the boot target back on the RUNNING partition (undo a select whose
 * reboot could not be started). Returns false if even that failed. */
bool recovery_switch_restore_running(void);

/* boot_guard threshold path: if boot_guard_is_recovery_mode(), decide with
 * boot_guard_decide_recovery_route(); on SWITCH_PARTITION select recovery and
 * reboot (does not return on success). On an invalid/absent recovery image or
 * the old layout it logs loudly and returns, leaving the existing degraded
 * in-app recovery mode in force. Returns immediately when not at threshold. */
void recovery_switch_at_boot_threshold(void);

#ifdef __cplusplus
}
#endif

#endif /* RECOVERY_SWITCH_H */
