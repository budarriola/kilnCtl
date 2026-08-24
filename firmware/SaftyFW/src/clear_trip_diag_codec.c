// clear_trip_diag_codec.c -- see clear_trip_diag_codec.h.
#include "clear_trip_diag_codec.h"

// 8-bit tag distinguishing a fresh write from this module from power-on-
// uninitialised SRAM or a stale value left by an unrelated reset cycle --
// same principle as boot_reason.c's SAFTYFW_TRIP_MAGIC_WORD, packed into the
// high byte of the one word this module gets instead of a separate
// register. Arbitrary, just needs to be unlikely to appear by chance and
// distinct from every other tag/magic value already in use on this board.
#define CLEAR_TRIP_DIAG_MAGIC_BYTE 0xC7u

// Bit layout of the packed word, MSB to LSB:
//   [31:24] magic byte (CLEAR_TRIP_DIAG_MAGIC_BYTE)
//   [23:20] stage (CLEAR_TRIP_DIAG_STAGE_*, 4 bits, values 0-4)
//   [19:16] reason (safety_trip_t, 4 bits -- every value seen so far fits; a
//           trip reason above 15 would truncate, but this is a diagnostic
//           snapshot, not the authoritative source, same tolerance
//           boot_reason.c's own uint32_t trip_reason field does not need
//           because it has a whole register to itself)
//   [15:8]  fault_bits, the full byte, unpacked verbatim (SAFETY_THERMO_FAULT_*)
//   [7]     tc_valid
//   [6]     spi_failed
//   [5]     tc_c_is_nan
//   [4:2]   outcome (safety_clear_trip_outcome_t, 3 bits, values 0-3 all fit)
//   [1:0]   reserved, always 0
#define CLEAR_TRIP_DIAG_MAGIC_SHIFT 24u
#define CLEAR_TRIP_DIAG_STAGE_SHIFT 20u
#define CLEAR_TRIP_DIAG_STAGE_MASK  0xFu
#define CLEAR_TRIP_DIAG_REASON_SHIFT 16u
#define CLEAR_TRIP_DIAG_REASON_MASK  0xFu
#define CLEAR_TRIP_DIAG_FAULT_SHIFT  8u
#define CLEAR_TRIP_DIAG_FAULT_MASK   0xFFu
#define CLEAR_TRIP_DIAG_TC_VALID_BIT   (1u << 7)
#define CLEAR_TRIP_DIAG_SPI_FAILED_BIT (1u << 6)
#define CLEAR_TRIP_DIAG_TC_NAN_BIT     (1u << 5)
#define CLEAR_TRIP_DIAG_OUTCOME_SHIFT  2u
#define CLEAR_TRIP_DIAG_OUTCOME_MASK   0x7u

uint32_t clear_trip_diag_encode(uint8_t stage, uint8_t reason, uint8_t fault_bits, bool tc_valid,
                                 bool spi_failed, bool tc_c_is_nan, uint8_t outcome)
{
    uint32_t word = (CLEAR_TRIP_DIAG_MAGIC_BYTE << CLEAR_TRIP_DIAG_MAGIC_SHIFT) |
                    ((uint32_t)(stage & CLEAR_TRIP_DIAG_STAGE_MASK) << CLEAR_TRIP_DIAG_STAGE_SHIFT) |
                    ((uint32_t)(reason & CLEAR_TRIP_DIAG_REASON_MASK) << CLEAR_TRIP_DIAG_REASON_SHIFT) |
                    ((uint32_t)(fault_bits & CLEAR_TRIP_DIAG_FAULT_MASK) << CLEAR_TRIP_DIAG_FAULT_SHIFT) |
                    ((uint32_t)(outcome & CLEAR_TRIP_DIAG_OUTCOME_MASK) << CLEAR_TRIP_DIAG_OUTCOME_SHIFT);
    if (tc_valid) {
        word |= CLEAR_TRIP_DIAG_TC_VALID_BIT;
    }
    if (spi_failed) {
        word |= CLEAR_TRIP_DIAG_SPI_FAILED_BIT;
    }
    if (tc_c_is_nan) {
        word |= CLEAR_TRIP_DIAG_TC_NAN_BIT;
    }
    return word;
}

clear_trip_diag_t clear_trip_diag_decode(uint32_t word)
{
    clear_trip_diag_t out;
    out.magic_ok = false;
    out.stage = CLEAR_TRIP_DIAG_STAGE_NONE;
    out.reason = 0;
    out.fault_bits = 0;
    out.tc_valid = false;
    out.spi_failed = false;
    out.tc_c_is_nan = false;
    out.outcome = 0;

    uint8_t magic = (uint8_t)(word >> CLEAR_TRIP_DIAG_MAGIC_SHIFT);
    if (magic == CLEAR_TRIP_DIAG_MAGIC_BYTE) {
        out.magic_ok = true;
        out.stage = (uint8_t)((word >> CLEAR_TRIP_DIAG_STAGE_SHIFT) & CLEAR_TRIP_DIAG_STAGE_MASK);
        out.reason = (uint8_t)((word >> CLEAR_TRIP_DIAG_REASON_SHIFT) & CLEAR_TRIP_DIAG_REASON_MASK);
        out.fault_bits = (uint8_t)((word >> CLEAR_TRIP_DIAG_FAULT_SHIFT) & CLEAR_TRIP_DIAG_FAULT_MASK);
        out.tc_valid = (word & CLEAR_TRIP_DIAG_TC_VALID_BIT) != 0u;
        out.spi_failed = (word & CLEAR_TRIP_DIAG_SPI_FAILED_BIT) != 0u;
        out.tc_c_is_nan = (word & CLEAR_TRIP_DIAG_TC_NAN_BIT) != 0u;
        out.outcome = (uint8_t)((word >> CLEAR_TRIP_DIAG_OUTCOME_SHIFT) & CLEAR_TRIP_DIAG_OUTCOME_MASK);
    }

    return out;
}
