// recovery_update.h -- TODO.md item 10.4 / docs/BOOTLOADER.md section 4: the
// bootloader's recovery-mode frame handling. Called from main.c's
// enter_recovery() once UART1 is up and GPIO6 is already low; never returns.
//
// This is a bare polling loop, NOT a FreeRTOS task -- there is no RTOS linked
// into the bootloader (bootloader/CMakeLists.txt's own header comment: "must
// never link FreeRTOS or anything the application needs"). It reuses, rather
// than reimplements, three things that already exist and are already
// host-tested:
//   - src/update/image_header.h, src/update/received_ranges.h,
//     src/update/update_receiver.h -- pure decision logic, no RTOS/SDK
//     dependency, compiled into this executable from ../src (see
//     CMakeLists.txt's target_include_directories/target_sources).
//   - CommonFW's kilnlink library (kilnlink_frame.h) -- freestanding C11,
//     no allocation/I/O/globals (CommonFW/README.md rules 1-6), so it is
//     exactly as safe to link into this bare-metal target as into the
//     FreeRTOS application. This is the SAME framing/CRC implementation
//     src/tasks/link_task.c uses -- see tools/check_no_duplicate_crc.ps1's
//     rule ("no CRC or byte-stuffing implementation outside CommonFW"),
//     which this file does not violate.
//   - bootloader/persist.c's bootloader_persist_metadata() -- the same
//     metadata-log writer main.c's own boot path uses, not a second one.
//
// --- What differs from src/tasks/update_task.c, and why ---
//
// 1. No FreeRTOS: recovery_update_run() is a single function with an
//    infinite for(;;) loop, called directly from enter_recovery(). There is
//    no queue, no separate RX task, no periodic-tick TickType_t bookkeeping
//    -- time is read via pico-sdk's time_us_64() (available without any RTOS,
//    it is a hardware timer read) instead of xTaskGetTickCount().
//
// 2. No preconditions: update_receiver_handle_begin()'s `update_preconditions_t`
//    argument is the Pico's OWN evidence about relay/trip/temperature state,
//    gathered from safety_core/thermo_task -- both of which are FreeRTOS
//    tasks that do not exist in this bare-metal image (recovery mode is
//    entered specifically because the application, and everything it
//    started, never ran). There is no live safety state to query here, and
//    inventing a way to read it would be exactly the kind of fabricated
//    evidence this codebase's "no dynamic allocation after init" / "never
//    guess" discipline forbids elsewhere. update_preconditions_t's own
//    contract (update_receiver.h) is "the caller's own evidence, gathered
//    from safety_core/thermo_task" -- a struct of plain bools with no
//    built-in notion of "not applicable". This file passes an
//    all-satisfied struct (relay_open=true, no_trip_pending=true,
//    temp_known_and_low=true), which is the only value that means "this
//    check does not apply here" given that contract, and is safe precisely
//    BECAUSE recovery mode implies GPIO6 is already latched low by main.c's
//    first statement and stays that way for the process's entire lifetime
//    (main.c's header comment: "this function does not reference
//    [GPIO6], by design") -- there is no relay for a stale "relay_open"
//    evaluation to protect against, unlike the live application where a
//    relay could be closed *right now*. update_receiver_handle_begin()
//    still independently validates the image header itself (magic/target/
//    version/length) -- that check is real and not bypassed.
//
// 3. Active slot: read via bootloader_metadata_find_latest() over the
//    XIP-mapped metadata region, the exact same read main.c's own boot path
//    (step 3) already performs -- not re-derived a different way.
//
// 4. Framing: kilnlink_unstuff() + kilnlink_frame_decode(), fed one byte at a
//    time via uart_is_readable()/uart_getc_raw() polling (mirroring
//    src/tasks/link_task.c's link_task_rx_process_byte() resync-on-0x7E
//    state machine, but polled here instead of driven by uart_owner's RTOS
//    RX path) -- reused logic, not a second framing implementation.
//
// 5. UPDATE_END's payload is s_header.crc32 repeated (CommonFW/docs/
//    UPDATE_PROTOCOL.md's "Section 4 deviations" note) -- this file logs
//    (does nothing else with) a mismatch between that repeated value and the
//    BEGIN header's own crc32, exactly like update_task_process_end(); the
//    read-back-from-flash CRC via bootloader_crc32() is what actually gates
//    acceptance.
//
// 6. UPDATE_STATUS: this file's recovery_update_pack_status() emits the
//    SAME 16-byte-header-plus-gap-list wire layout update_task.c's own
//    UPDATE_STATUS builder invented (that file's header comment: "state,
//    bytes received, last error... UPDATE_PROTOCOL.md section 4 names the
//    frame but never specifies a byte layout -- this is that layout"). It is
//    a fresh, small standalone implementation (not a call into
//    update_task.c, which is FreeRTOS/queue-coupled and not linkable here)
//    but byte-layout-identical, so a listener on the other end (KilnFW's
//    ota_pico_relay.c / a bench tool) parses both the application's and the
//    bootloader's UPDATE_STATUS frames the same way.
//
// 7. On a verified UPDATE_END, the target slot is marked PENDING_VERIFY
//    (not VALID) and active_slot flipped to it, then persisted via
//    bootloader_persist_metadata() -- exactly mirroring
//    update_task_process_end()'s own choice (item 10.7/10.8: the
//    application still has to earn VALID via the confirmation gate,
//    docs/BOOTLOADER.md section 5). This bootloader has no reboot call
//    after that write, matching update_task_process_end()'s own explicit
//    "deliberately NOT rebooting here" -- the ESP/operator is expected to
//    power-cycle or otherwise reset the board, at which point main.c's own
//    boot path picks up the newly-PENDING_VERIFY slot on the next boot.
//
// 8. GPIO6: never referenced anywhere in this file. main.c's enter_recovery()
//    already drove it low before calling recovery_update_run(), and nothing
//    below touches it again.
//
// 9. No timeout: recovery_update_run() is `__attribute__((noreturn))` and its
//    body is an unconditional for(;;) -- there is no code path that returns
//    to main.c or falls through to jump_to_app().
//
// --- What this file does NOT implement (honest scope, see this session's
// coordinator brief and docs/BOOTLOADER.md section 4/7) ---
//   - No retry/backoff tuning beyond reusing update_receiver.h's existing
//     UPDATE_MAX_RETRANSMIT_ROUNDS cap -- a transfer that exceeds it reverts
//     the target slot to EMPTY and keeps looping in recovery (there is
//     nowhere else to go; the operator must retry the whole transfer).
//   - No authentication/signing (docs/BOOTLOADER.md section 6, not planned
//     for the first version, same as the application-side receiver).
//   - Not exercised over a real link, real flash timing under this
//     bootloader's specific interrupt-disable window, or real recovery-mode
//     entry from a genuinely bad application image -- build/host-test
//     verified only, no probe/hardware available this pass (see TODO.md).
#ifndef SAFTYFW_BOOTLOADER_RECOVERY_UPDATE_H
#define SAFTYFW_BOOTLOADER_RECOVERY_UPDATE_H

#ifdef __cplusplus
extern "C" {
#endif

// Runs the recovery-mode UART1 frame loop forever. Must be called only after
// uart1 has been initialised (main.c's enter_recovery() does this before
// calling in). Never returns.
void recovery_update_run(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_BOOTLOADER_RECOVERY_UPDATE_H
