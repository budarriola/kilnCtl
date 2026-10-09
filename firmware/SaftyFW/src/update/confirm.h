// confirm.h -- the PENDING_VERIFY -> VALID confirmation gate, TODO.md
// Phase 10 item 10.8, docs/BOOTLOADER.md section 5's bar for "a working
// image": "An image that boots but cannot read its thermocouple is worse
// than the old one, and it would sail through any check that just proves
// main() ran." This module is that check -- called from the application's
// own startup/bring-up sequence, NOT at the end of main() (BOOTLOADER.md's
// own emphasis, matching CommonFW/docs/UPDATE_PROTOCOL.md's identical
// warning for the ESP side: "Do not call it at the end of app_main()").
//
// --- A note on "one acknowledged telemetry frame" ---
//
// docs/BOOTLOADER.md section 5 lists this bar item verbatim: "At least one
// telemetry frame acknowledged by the ESP." That phrasing does not fit this
// project's actual link design: CommonFW/docs/LINK_PROTOCOL.md section 2 is
// explicit and repeated throughout this codebase that the Pico NEVER waits
// for or receives an ACK for anything it sends -- "The Pico never sends an
// ACK, and never expects one," and telemetry is unconditionally
// unacknowledged BROADCAST. There is no ACK to wait for, and building one
// in just for this check would violate the Pico-never-blocks-on-the-link
// rule the whole protocol is designed around.
//
// This module's honest substitute: "at least one telemetry (status) frame
// was handed to the TX ring and NOT dropped" (i.e.
// link_task_send_status()'s underlying uart_owner_send() returned success
// at least once since boot) -- the closest available evidence that the
// telemetry path is actually functioning, without inventing an
// acknowledgement mechanism this link's design deliberately does not have.
// The gap between "sent" and "received by a healthy ESP" is real and stays
// open; it is not this module's job to close it, only to be honest that it
// has not.
//
// Pure, no RTOS/SDK dependency -- host-testable, same discipline as every
// other decision module in src/update/ and bootloader/.
#ifndef SAFTYFW_UPDATE_CONFIRM_H
#define SAFTYFW_UPDATE_CONFIRM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Each field is evidence the caller gathers from elsewhere -- this module
// has no I/O, no task dependency, and does not know how any of these are
// actually determined (docs/BOOTLOADER.md section 5's bar, restated as
// booleans):
typedef struct {
    bool config_crc_ok;          // config store loaded and its CRC verified (Phase 9 -- until
                                  // config_store exists, a caller with nothing to check should
                                  // pass false, never true, per this codebase's "unknown must
                                  // never read as confirmed-good" discipline elsewhere)
    bool thermocouple_plausible; // the safety thermocouple is returning a plausible reading
    bool adc_sampling_ok;        // current-sense ADC sampling is running
    bool all_tasks_checked_in;   // every FreeRTOS task has checked in with the watchdog at
                                  // least once since this boot
    bool telemetry_sent_ok;      // see this file's header comment -- "sent", not "acknowledged"
} update_confirm_checklist_t;

// One bit per unmet item, so a caller can log exactly what is still
// missing rather than a single opaque "not ready" -- same pattern as
// update_receiver.h's update_precondition_flag_t.
typedef enum {
    UPDATE_CONFIRM_MISSING_CONFIG_CRC        = 1u << 0,
    UPDATE_CONFIRM_MISSING_THERMOCOUPLE      = 1u << 1,
    UPDATE_CONFIRM_MISSING_ADC               = 1u << 2,
    UPDATE_CONFIRM_MISSING_WATCHDOG_CHECKINS = 1u << 3,
    UPDATE_CONFIRM_MISSING_TELEMETRY         = 1u << 4,
} update_confirm_missing_flag_t;

// Returns the OR of every unmet item's flag, or 0 once every item is true
// -- 0 is the caller's signal that the running image has earned
// confirmation and should be marked VALID (bootloader/metadata.h's
// BOOTLOADER_SLOT_VALID) via the same metadata-log write mechanism
// bootloader/main.c's persist_metadata() uses, adapted for the FreeRTOS/
// multicore application (flash_safe_execute(), not
// save_and_disable_interrupts() alone -- the bootloader is single-core
// bare-metal, the application is not). That flash write is real I/O and is
// NOT this function's job; this function only decides whether the write
// should happen yet.
uint8_t update_confirm_missing(const update_confirm_checklist_t *c);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_UPDATE_CONFIRM_H
