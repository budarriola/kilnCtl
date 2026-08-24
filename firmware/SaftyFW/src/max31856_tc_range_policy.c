// max31856_tc_range_policy.c -- see max31856_tc_range_policy.h.
#include "max31856_tc_range_policy.h"

#include <math.h>

#include "max31856.h" // MAX31856_TC_TYPE_* -- pure header, safe for a host-tested file

// Datasheet MAX31856.pdf page 12, Table 1 "Supported Thermocouples and
// Temperature Ranges", TEMP RANGE column -- see max31856_tc_range_policy.h's
// file header for the full citation and the type-B-starts-at-250degC note.
// Indexed by MAX31856_TC_TYPE_* (B=0 .. T=7, max31856.h).
typedef struct {
    float min_c;
    float max_c;
} tc_range_t;

// Positional, not designated-initializer, array: indices 0..7 must line up
// exactly with MAX31856_TC_TYPE_B..MAX31856_TC_TYPE_T (max31856.h), which are
// themselves fixed by the datasheet's CR1.TC TYPE[3:0] encoding (Table 2),
// so relying on that order rather than repeating it as designated-initializer
// tags keeps this file buildable under strict MSVC /W4 /WX (the host-test
// toolchain) without relying on a C99 array-designator extension.
static const tc_range_t TC_RANGES[8] = {
    {250.0f, 1820.0f},  // B
    {-200.0f, 1000.0f}, // E
    {-210.0f, 1200.0f}, // J
    {-200.0f, 1372.0f}, // K
    {-200.0f, 1300.0f}, // N
    {-50.0f, 1768.0f},  // R
    {-50.0f, 1768.0f},  // S
    {-200.0f, 400.0f},  // T
};

bool max31856_tc_range_is_plausible(uint8_t tc_type, float tc_c)
{
    if (isnan(tc_c)) {
        return false;
    }
    if (tc_type > MAX31856_TC_TYPE_T) {
        return false; // no datasheet range for a voltage-mode/unrecognised type
    }

    const tc_range_t *r = &TC_RANGES[tc_type];
    return tc_c >= r->min_c && tc_c <= r->max_c; // inclusive both ends -- see header comment
}

// Union of all eight TC_RANGES rows -- computed by inspection, not at
// runtime: min_c is J's -210 (the lowest of the eight), max_c is B's 1820
// (the highest). Kept as a named constant, not folded into the loop below,
// so a reviewer checking this against the datasheet table does not have to
// re-derive it. See max31856_tc_range_policy.h's header comment for why this
// union, not any single type's band, is what an uncommissioned tc_type gets.
static const tc_range_t TC_RANGE_UNION = { -210.0f, 1820.0f };

bool max31856_tc_range_is_plausible_uncommissioned(float tc_c)
{
    if (isnan(tc_c)) {
        return false;
    }
    return tc_c >= TC_RANGE_UNION.min_c && tc_c <= TC_RANGE_UNION.max_c;
}

max31856_cr1_readback_result_t max31856_cr1_readback_check(uint8_t intended_tc_type,
                                                             uint8_t cr1_readback)
{
    // Checked BEFORE the nibble compare -- see header comment: these two
    // whole-byte values can never be what this driver itself wrote for any
    // real tc_type (AVGSEL is always fixed at 4 samples, CR1 upper nibble
    // 0x2X), so they are a dead/shifted-bus symptom, not "a different real
    // type", regardless of what intended_tc_type's low nibble happens to be.
    if (cr1_readback == 0x00u || cr1_readback == 0xFFu) {
        return MAX31856_CR1_READBACK_DEAD_BUS;
    }

    uint8_t readback_type = (uint8_t)(cr1_readback & 0x0Fu);
    return (readback_type == (uint8_t)(intended_tc_type & 0x0Fu))
               ? MAX31856_CR1_READBACK_MATCH
               : MAX31856_CR1_READBACK_MISMATCH;
}
