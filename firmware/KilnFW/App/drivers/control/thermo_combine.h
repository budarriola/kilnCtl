// thermo_combine -- TODO.md 10.8's multi-thermocouple-per-zone combiner.
//
// Pure C, no FreeRTOS, no ESP-IDF, no logging, no I/O -- same discipline as
// pid.h/thermal_guard.h (see pid.h's header comment for why: it is what lets
// this be unit-tested on the host, off-target, before it ever runs against a
// real kiln). This module does not know what a "zone" or a "channel" is in
// hardware terms; it is handed parallel per-channel reading/validity arrays
// and a bitmask naming which entries belong to one zone, and returns one
// number.
//
// Combining function: arithmetic MEAN of every assigned, valid (non-faulted)
// reading -- TODO.md 10.8's default candidate, and the only one this pass
// builds. Outlier rejection and a max/min-biased combiner ("control off the
// hottest reading, never let any point overshoot") are explicitly named in
// TODO.md 10.8 as open questions needing their own decision before they are
// buildable; this module does not guess at either.
//
// A masked-in channel that reads invalid (open/short/stale/spi-failed --
// whatever the caller's own validity check already decided, this module
// does not re-derive it) is simply dropped from the average rather than
// corrupting it with a NaN or a stale number. Zero valid channels among the
// mask is the same "thermocouple invalid" case a single-channel zone has
// always had -- signalled by *out_valid = false and a NaN return, mirroring
// zones_config_apply_cal()'s NaN-passthrough convention (zones_http.h) so a
// caller that forgets to check *out_valid still gets a value that propagates
// as obviously invalid rather than as a plausible-looking zero.
#ifndef THERMO_COMBINE_H
#define THERMO_COMBINE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* channel_c/channel_ok are parallel arrays of length channel_count, indexed
 * by physical thermocouple channel (0-based) -- e.g. what MAX31856_read_all()
 * fills for one bus scan, whatever the caller's own sensor-validity check
 * already decided for channel_ok (spi_failed / isnan / THERMO_FAULT_* -- this
 * module deliberately does not know that vocabulary; matching it here would
 * be a second copy of a decision that must never disagree with the caller's).
 *
 * thermo_mask names which of those channels belong to the zone being
 * combined: bit N set means channel_c[N]/channel_ok[N] contributes if
 * channel_ok[N] is true. Bits at or past channel_count are ignored rather
 * than treated as an error -- validating a mask against the current channel
 * count is the config layer's job (zones_http.c), not this pure function's.
 *
 * Returns the mean of every masked-in, valid reading. *out_valid is set true
 * iff at least one reading contributed; when false the return value is NaN
 * and must not be used -- same "false/NaN means cannot answer" convention as
 * every other invalid-reading signal in this codebase (see the header
 * comment above). out_valid must not be NULL. */
float thermo_combine(const float *channel_c, const bool *channel_ok, uint8_t channel_count,
                     uint8_t thermo_mask, bool *out_valid);

#ifdef __cplusplus
}
#endif

#endif // THERMO_COMBINE_H
