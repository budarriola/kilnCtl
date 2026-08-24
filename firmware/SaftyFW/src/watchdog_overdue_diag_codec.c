// watchdog_overdue_diag_codec.c -- see watchdog_overdue_diag_codec.h.
#include "watchdog_overdue_diag_codec.h"

// 8-bit tag, distinct from clear_trip_diag_codec.c's CLEAR_TRIP_DIAG_MAGIC_BYTE
// (0xC7) and every other magic/tag value already in use on this board
// (boot_reason.c's SAFTYFW_TRIP_MAGIC_WORD, startup_diag.h's
// SAFTYFW_STARTUP_DIAG_MAGIC/SAFTYFW_LAST_CHECKIN_WRITTEN) -- a stale value
// from one of those formats landing in this register by mistake must never
// be misread as fresh overdue data.
#define WATCHDOG_OVERDUE_DIAG_MAGIC_BYTE 0xD9u

// Bit layout of the packed word, MSB to LSB:
//   [31:24] magic byte (WATCHDOG_OVERDUE_DIAG_MAGIC_BYTE)
//   [23:16] overdue_mask, the full byte -- one bit per watchdog_checkin_id_t
//   [15:13] worst_task_id (3 bits, values 0-7)
//   [12:0]  worst_overage_ms, saturated to WATCHDOG_OVERDUE_DIAG_OVERAGE_MAX_MS
#define WATCHDOG_OVERDUE_DIAG_MAGIC_SHIFT 24u
#define WATCHDOG_OVERDUE_DIAG_MASK_SHIFT  16u
#define WATCHDOG_OVERDUE_DIAG_MASK_MASK   0xFFu
#define WATCHDOG_OVERDUE_DIAG_TASK_SHIFT  13u
#define WATCHDOG_OVERDUE_DIAG_TASK_MASK   0x7u
#define WATCHDOG_OVERDUE_DIAG_OVERAGE_SHIFT 0u
#define WATCHDOG_OVERDUE_DIAG_OVERAGE_MASK  0x1FFFu

uint32_t watchdog_overdue_diag_encode(uint8_t overdue_mask, uint8_t worst_task_id,
                                       uint16_t worst_overage_ms)
{
    uint16_t overage = (worst_overage_ms > WATCHDOG_OVERDUE_DIAG_OVERAGE_MAX_MS)
                            ? (uint16_t)WATCHDOG_OVERDUE_DIAG_OVERAGE_MAX_MS
                            : worst_overage_ms;

    return (WATCHDOG_OVERDUE_DIAG_MAGIC_BYTE << WATCHDOG_OVERDUE_DIAG_MAGIC_SHIFT) |
           ((uint32_t)overdue_mask << WATCHDOG_OVERDUE_DIAG_MASK_SHIFT) |
           ((uint32_t)(worst_task_id & WATCHDOG_OVERDUE_DIAG_TASK_MASK) << WATCHDOG_OVERDUE_DIAG_TASK_SHIFT) |
           ((uint32_t)(overage & WATCHDOG_OVERDUE_DIAG_OVERAGE_MASK) << WATCHDOG_OVERDUE_DIAG_OVERAGE_SHIFT);
}

watchdog_overdue_diag_t watchdog_overdue_diag_decode(uint32_t word)
{
    watchdog_overdue_diag_t out;
    out.magic_ok = false;
    out.overdue_mask = 0;
    out.worst_task_id = 0;
    out.worst_overage_ms = 0;

    uint8_t magic = (uint8_t)(word >> WATCHDOG_OVERDUE_DIAG_MAGIC_SHIFT);
    if (magic == WATCHDOG_OVERDUE_DIAG_MAGIC_BYTE) {
        out.magic_ok = true;
        out.overdue_mask =
            (uint8_t)((word >> WATCHDOG_OVERDUE_DIAG_MASK_SHIFT) & WATCHDOG_OVERDUE_DIAG_MASK_MASK);
        out.worst_task_id =
            (uint8_t)((word >> WATCHDOG_OVERDUE_DIAG_TASK_SHIFT) & WATCHDOG_OVERDUE_DIAG_TASK_MASK);
        out.worst_overage_ms =
            (uint16_t)((word >> WATCHDOG_OVERDUE_DIAG_OVERAGE_SHIFT) & WATCHDOG_OVERDUE_DIAG_OVERAGE_MASK);
    }

    return out;
}

// 2026-08-23, round 2: shares watchdog_overdue_diag_t's physical scratch[5]
// register, distinguished by magic byte alone -- see
// watchdog_overflow_diag_t's own doc comment (watchdog_overdue_diag_codec.h)
// for why. WATCHDOG_OVERFLOW_DIAG_MAGIC_BYTE and the bit positions below
// MUST be kept in exact sync with main.c's vApplicationStackOverflowHook()
// BY HAND -- there is deliberately no shared encoder for this format (that
// hook cannot safely call into another compilation unit from a context
// where the stack may already be corrupted), so this decoder's only
// correctness check is this host test file plus the constants matching on
// both sides.
#define WATCHDOG_OVERFLOW_DIAG_MAGIC_BYTE 0xE3u
#define WATCHDOG_OVERFLOW_DIAG_MAGIC_SHIFT  24u
#define WATCHDOG_OVERFLOW_DIAG_NAME0_SHIFT  16u
#define WATCHDOG_OVERFLOW_DIAG_NAME1_SHIFT  8u
#define WATCHDOG_OVERFLOW_DIAG_BYTE_MASK    0xFFu

watchdog_overflow_diag_t watchdog_overflow_diag_decode(uint32_t word)
{
    watchdog_overflow_diag_t out;
    out.magic_ok = false;
    out.name_byte0 = 0;
    out.name_byte1 = 0;

    uint8_t magic = (uint8_t)(word >> WATCHDOG_OVERFLOW_DIAG_MAGIC_SHIFT);
    if (magic == WATCHDOG_OVERFLOW_DIAG_MAGIC_BYTE) {
        out.magic_ok = true;
        out.name_byte0 =
            (uint8_t)((word >> WATCHDOG_OVERFLOW_DIAG_NAME0_SHIFT) & WATCHDOG_OVERFLOW_DIAG_BYTE_MASK);
        out.name_byte1 =
            (uint8_t)((word >> WATCHDOG_OVERFLOW_DIAG_NAME1_SHIFT) & WATCHDOG_OVERFLOW_DIAG_BYTE_MASK);
    }

    return out;
}
