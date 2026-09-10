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

// --- Fatal-fault latch, the same scratch[5] register, 2026-09-09 ----------
//
// WHY. Before this, exactly ONE of the three ways this firmware can die
// fatally left any evidence at all: a stack overflow (watchdog_overflow_diag_t
// above). The other two -- vApplicationMallocFailedHook() and a failed
// configASSERT() -- did nothing but taskDISABLE_INTERRUPTS() and spin, which
// means watchdog_task never runs again, the unfed 1 s hardware watchdog
// resets the chip, and the board reboots roughly once a second forever with
// no diagnostic of any kind. That is not hypothetical: the thermo_task
// incident (5b8fc53d) was a heap-resident FreeRTOS queue control block
// smashed by a stack overflow that did NOT reliably trip the canary, so the
// fault surfaced as configASSERT(pxQueue->uxItemSize == 0) failing inside
// xQueueSemaphoreTake() -- indistinguishable, from the ESP's side, from
// "the safety link's status frame stopped decoding", and it cost hours of
// protocol investigation before somebody read the registers over JTAG.
//
// The three formats share the one physical scratch[5] register,
// distinguished by magic byte alone (0xE3 overflow / 0xB4 malloc-fail /
// 0xA5 assert, all distinct from watchdog_overdue_diag_t's own 0xD9). This
// is safe for exactly the reason the overflow format's own comment already
// gives: each of these hooks disables interrupts and hangs, so at most one
// of them can ever run in a given boot, and none of them can be followed by
// watchdog_task reaching its own 0xD9 write.
//
// PRECISION, stated plainly. The kind (overflow / malloc-fail / assert) is
// always recovered. Beyond that: overflow carries two task-name bytes;
// malloc-fail carries nothing further (the FreeRTOS hook takes no arguments
// -- it is not told the requested size); assert carries __LINE__ (16 bits)
// plus an 8-bit file id that a translation unit may opt into by defining
// SAFTYFW_ASSERT_FILE_ID before including FreeRTOS.h (0 = not declared,
// which is what every FreeRTOS kernel source reads as). So an assert is NOT
// localized to a file by default -- only to a line number within an
// unnamed file. That is still decisive for the incident above, but do not
// overclaim it: this codebase has 100+ configASSERT sites once the kernel
// sources are counted, so a bare line number narrows the field, it does not
// name the site. What it does do unambiguously, and what the incident
// actually needed, is separate "an assertion failed" from "a stack
// overflowed", from "the heap ran out", from "the watchdog fired with
// nothing recorded" -- four states that were previously one.
typedef enum {
    WATCHDOG_FATAL_KIND_NONE           = 0, // no magic tag present: nothing was recorded
    WATCHDOG_FATAL_KIND_STACK_OVERFLOW = 1, // 0xE3, vApplicationStackOverflowHook()
    WATCHDOG_FATAL_KIND_MALLOC_FAILED  = 2, // 0xB4, vApplicationMallocFailedHook()
    WATCHDOG_FATAL_KIND_ASSERT         = 3, // 0xA5, configASSERT() (FreeRTOSConfig.h)
} watchdog_fatal_kind_t;

typedef struct {
    bool     magic_ok;   // true iff `kind` != NONE -- kept for symmetry with the two structs above
    uint8_t  kind;       // watchdog_fatal_kind_t
    uint8_t  name_byte0; // STACK_OVERFLOW only, else 0
    uint8_t  name_byte1; // STACK_OVERFLOW only, else 0
    uint8_t  file_id;    // ASSERT only, else 0 -- SAFTYFW_ASSERT_FILE_ID at the failing site, 0 = not declared
    uint16_t line;       // ASSERT only, else 0 -- __LINE__ at the failing site, truncated to 16 bits
} watchdog_fatal_diag_t;

// Packing macros, deliberately macros and not functions: configASSERT()
// lives in FreeRTOSConfig.h and must not call into another compilation unit
// (it can fire from an ISR, from inside the scheduler's own critical
// sections, or -- as in the incident above -- from a context whose stack is
// already corrupt). A macro compiles to one constant-folded MMIO store.
// FreeRTOSConfig.h includes THIS header for them, which is safe: this file
// pulls in nothing but stdbool/stdint.
#define WATCHDOG_FATAL_MALLOC_WORD() (0xB4000000u)
#define WATCHDOG_FATAL_ASSERT_WORD(file_id, line)                            \
    (0xA5000000u | (((uint32_t)(file_id) & 0xFFu) << 16) |                   \
     ((uint32_t)(line) & 0xFFFFu))

// Decodes any of the three fatal formats out of one scratch[5] word. A word
// carrying watchdog_overdue_diag_t's 0xD9 tag, a zeroed register, or
// uninitialised power-on garbage all decode to WATCHDOG_FATAL_KIND_NONE --
// same "nothing usable here" contract as every other decode in this file.
watchdog_fatal_diag_t watchdog_fatal_diag_decode(uint32_t word);

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
