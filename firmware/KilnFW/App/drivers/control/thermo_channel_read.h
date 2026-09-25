/* thermo_channel_read -- one shared "read every MAX31856 channel and apply
 * the sensor-validity filter" step, factored out 2026-09-24 so the filter
 * cannot drift between call sites again (it already had, silently: see
 * profile_executor_capture_baseline()'s missing fault_bits_bad check, fixed
 * in the same change that added this file).
 *
 * This step is also the SPI "producer call" CLAUDE.md's locking rule is
 * about -- MAX31856_read_all()/sim_backend_read_all() can block for up to
 * MAX31856_LOCK_TIMEOUT_MS per channel on a wedged bus. Every caller of
 * thermo_channels_read() must call it BEFORE taking its own module lock
 * (s_exec.lock / s_at.lock), never while holding one, and pass the
 * resulting snapshot into the locked section instead of reading through
 * the bus there. thermo_combine() (thermo_combine.h) remains the separate,
 * pure per-zone averaging step downstream of this one.
 *
 * Does not know about "zones" -- purely per physical channel, indexed the
 * same way MAX31856_read_all() indexes MAX31856Reading::channel. */
#ifndef THERMO_CHANNEL_READ_H
#define THERMO_CHANNEL_READ_H

#include <stdbool.h>
#include <stdint.h>

#include "hw/MAX31856.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float raw_c[MAX31856_CHANNEL_COUNT];  /* the channel's reported temperature, or NAN if the
                                            * bus did not answer for it; NOT cleared when `ok`
                                            * is false (a faulted channel keeps its finite
                                            * value) -- always read it together with `ok`. */
    bool  ok[MAX31856_CHANNEL_COUNT];     /* !spi_failed && !isnan && !fault_bits_bad */
    float cj_c[MAX31856_CHANNEL_COUNT];   /* cold-junction reading, NAN where unavailable;
                                            * not gated by `ok` -- callers that use it
                                            * (autotune's SETTLING->STEPPING transition)
                                            * check isnan() themselves, same as before
                                            * this extraction. */
} ThermoChannelSnapshot;

/* Reads every channel of `bus` (or the sim backend, if enabled -- `bus` may
 * be NULL in that case) into `out`, applying the same
 * `!spi_failed && !isnan(tc_temperature_c) && !fault_bits_bad` filter as
 * every correct call site already used. Channels at or past
 * MAX31856_CHANNEL_COUNT in a reading are ignored. `out` is fully
 * initialized (NAN/false) even when the bus is not usable, so a caller
 * never needs to zero it first. Takes no module lock (s_exec.lock/s_at.lock)
 * of its own; the producers it calls take only their own leaf locks
 * (each MAX31856 channel's own ch->lock, up to MAX31856_LOCK_TIMEOUT_MS
 * apiece; sim_backend's s_sim.lock), exactly as
 * before this extraction. Performs no I/O beyond the one bus scan -- must be
 * called BEFORE taking any module lock, never while holding one. */
void thermo_channels_read(MAX31856BusClass *bus, ThermoChannelSnapshot *out);

#ifdef __cplusplus
}
#endif

#endif // THERMO_CHANNEL_READ_H
