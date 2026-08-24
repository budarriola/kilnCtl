// watchdog_overdue_diag_codec.h -- pure bit-packing for the "which task(s)
// missed their check-in deadline, by how much" latch, round 2 of the
// CLEAR_TRIP-reboots-the-Pico investigation (which turned out not to be a
// crash at all -- see watchdog_overdue_diag.h's own header comment).
//
// Split out the same way clear_trip_diag_codec.h is split out of
// clear_trip_diag.c: free of pico-sdk/FreeRTOS/hardware includes so it is
// directly host-testable (test/test_watchdog_overdue_diag_codec.c links
// this .c file with no stub layer needed).
#ifndef SAFTYFW_WATCHDOG_OVERDUE_DIAG_CODEC_H
#define SAFTYFW_WATCHDOG_OVERDUE_DIAG_CODEC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     magic_ok;
    uint8_t  overdue_mask;     // one bit per watchdog_checkin_id_t, set iff that task was past ITS OWN deadline at the moment the feed was withheld
    uint8_t  worst_task_id;    // the watchdog_checkin_id_t whose deadline was exceeded by the largest margin (meaningless if overdue_mask == 0, which cannot happen for a genuine "not fed" latch, but decode does not assume that)
    uint16_t worst_overage_ms; // elapsed_ms - deadline_ms for worst_task_id, saturated -- "missed by 20ms" vs "missed by 2000ms" is the whole point of this field
} watchdog_overdue_diag_t;

// 2026-08-23, round 2 of THIS investigation: scratch[5]'s overdue latch
// read empty (magic_ok == false) after a reproduced crash -- proof
// watchdog_task_fn() never reached the branch that withholds a feed at
// all, because vApplicationStackOverflowHook() (main.c) had already
// disabled interrupts and hung, permanently, before watchdog_task ever got
// another scheduler slice. That hook cannot safely call into this file's
// own encoder (it may be running moments after the stack it would call on
// overflowed) -- see watchdog_overflow_diag_decode()'s own comment below
// for why the ENCODE side is deliberately NOT here, only decode is. Shares
// the physical scratch[5] register with watchdog_overdue_diag_t above,
// distinguished by magic byte alone: the two events are mutually exclusive
// by construction (a stack overflow's interrupt-disable is exactly what
// prevents watchdog_task from ever reaching its own write), so one
// register serves both without ambiguity.
typedef struct {
    bool    magic_ok;
    uint8_t name_byte0; // pcTaskName[0] of the overflowing task, raw -- e.g. 's' for "safety_core"
    uint8_t name_byte1; // pcTaskName[1] -- two bytes is enough to disambiguate every task name this codebase currently registers (see test_two_name_bytes_disambiguate_every_current_task)
} watchdog_overflow_diag_t;

// Exact inverse of the packed word vApplicationStackOverflowHook() (main.c)
// writes INLINE (not via a shared encoder -- see the struct's own comment
// above for why). The two magic bytes (this format's and
// watchdog_overdue_diag_t's) MUST be kept distinct by hand; there is no
// shared source of truth between this file and main.c's hook for that
// constant, deliberately, because the hook must not depend on calling into
// another compilation unit's function from a context where the stack may
// already be corrupted. magic_ok is false (every other field zeroed) iff
// the high byte does not match this format's tag -- same "nothing usable
// here" contract as every other decode in this codebase's scratch-register
// diagnostics.
watchdog_overflow_diag_t watchdog_overflow_diag_decode(uint32_t word);

// Largest overage this format can represent -- (1 << 13) - 1. Encoding a
// larger value saturates to this rather than silently truncating into a
// misleadingly small one (20ms instead of 2000ms would be exactly the wrong
// direction to be wrong in for this diagnostic).
#define WATCHDOG_OVERDUE_DIAG_OVERAGE_MAX_MS 8191u

// Packs every field into one word, always including the magic tag -- same
// "no encode without the tag" discipline as clear_trip_diag_encode()
// (clear_trip_diag_codec.h), since an untagged word is exactly the
// ambiguous case this format exists to avoid. worst_task_id is masked to 3
// bits (0-7, covers every watchdog_checkin_id_t this codebase has); a value
// outside that range from a future 9th+ registered task would silently wrap
// rather than corrupt a neighbouring field -- see
// test_watchdog_checkin_id_fits_in_3_bits in the host test for the guard
// against that actually happening unnoticed.
uint32_t watchdog_overdue_diag_encode(uint8_t overdue_mask, uint8_t worst_task_id,
                                       uint16_t worst_overage_ms);

// Exact inverse of watchdog_overdue_diag_encode(). magic_ok is false (every
// other field zeroed) iff the high byte does not match the tag -- a fresh
// boot's zeroed register, a stale value from an unrelated reset, or (before
// this module has ever run once) a genuinely never-written register all
// collapse to "nothing usable here", same contract as clear_trip_diag_decode().
watchdog_overdue_diag_t watchdog_overdue_diag_decode(uint32_t word);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_WATCHDOG_OVERDUE_DIAG_CODEC_H
