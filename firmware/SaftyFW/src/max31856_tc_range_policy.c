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
