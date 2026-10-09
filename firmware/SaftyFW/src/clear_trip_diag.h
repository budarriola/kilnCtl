// clear_trip_diag.h -- reset-surviving checkpoint latch for the CLEAR_TRIP-
// reboots-the-Pico investigation, round 4.
//
// 2026-08-23: the previous round's SWD-readable BSS statics all read back
// zero after a reproduced crash -- not because the checkpoints were never
// reached, but because the crash reboots the chip, and the C runtime zeroes
// .bss on every boot before anyone can read what a debugger halted mid-
// crash might have seen. Same trap startup_diag.h/boot_reason.c already
// exist to avoid for the two other silent-reboot failures this firmware
// has had: RAM does not survive a reset, watchdog scratch registers do.
//
// Only watchdog_hw->scratch[7] is free -- see startup_diag.h's own budget
// comment for the other seven. This file is the thin hardware wrapper
// (read/write watchdog_hw->scratch[7]) around clear_trip_diag_codec.h's
// pure encode/decode -- same split watchdog_gate.c/watchdog_task.c and
// uart_owner_tx_policy.c/uart_owner.c already use in this codebase, so the
// bit-packing itself is host-testable even though this file, needing real
// hardware/structs/watchdog.h, is not.
//
// This module deliberately does NOT touch scratch[0]/[1] or any function in
// boot_reason.c/.h -- that latch is load-bearing for reporting why the board
// reset and this investigation has already produced enough confusion
// without corrupting it. See main.c's boot sequence for where this is read,
// immediately alongside (never inside) boot_reason_read()/
// boot_reason_clear_trip().
#ifndef SAFTYFW_CLEAR_TRIP_DIAG_H
#define SAFTYFW_CLEAR_TRIP_DIAG_H

#include "clear_trip_diag_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

// Encodes and writes watchdog_hw->scratch[7] in one packed word,
// unconditionally (no read-modify-write, no dependency on a previous call
// having run this boot) -- called from safety_core_task()'s CLEAR_TRIP drain
// block at each of clear_trip_diag_codec.h's four stages, passing the
// fields known at that point (fields not yet meaningful, e.g. outcome
// before it's computed, pass 0/false). Never blocks, never allocates, safe
// to call from an interrupt-disabled context (a plain MMIO write) --
// deliberately, since the whole point is to survive a fault that might
// occur moments later.
void clear_trip_diag_mark(uint8_t stage, uint8_t reason, uint8_t fault_bits, bool tc_valid,
                           bool spi_failed, bool tc_c_is_nan, uint8_t outcome);

// Reads scratch[7], decodes it (clear_trip_diag_codec.h), and caches the
// result. Call once, early in main(), in the same boot step as
// boot_reason_read() -- BEFORE clear_trip_diag_clear() (this is the only
// chance to see what the previous boot left behind).
clear_trip_diag_t clear_trip_diag_read(void);

// Returns whatever the one clear_trip_diag_read() call this boot passed in
// -- same "cached once at boot, safe from any task thereafter" contract as
// boot_reason_get_cached(). All-zero/magic_ok==false if clear_trip_diag_read()
// has not run yet.
clear_trip_diag_t clear_trip_diag_get_cached(void);

// Zeros scratch[7] so a future, unrelated reset does not re-report this
// boot's stale checkpoint. Call after clear_trip_diag_read(), same "read
// then clear" ordering boot_reason_clear_trip() already establishes.
void clear_trip_diag_clear(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_CLEAR_TRIP_DIAG_H
