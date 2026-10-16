#include "thermo_channel_read.h"

#include <math.h>

#include "sim_backend.h"
#include "thermo_combine.h"

bool thermo_reading_ok(const MAX31856Reading *r)
{
    if (!r) return false;
    bool fault_bits_bad = (r->fault_status & (0x01u | 0x02u | 0x40u)) != 0;
    /* AUDIT_THERMOCOUPLE MED-1: a continuously converting channel whose last
     * good conversion is older than MAX31856_CONTROL_MAX_AGE_MS (or has never
     * produced one: age_ms == AGE_UNKNOWN) is a frozen reading, not a
     * temperature -- e.g. a part that power-on-reset into normally-off mode.
     * Unsigned compare: UNKNOWN (UINT32_MAX) is rejected too. */
    bool too_old = r->continuous && (r->age_ms > MAX31856_CONTROL_MAX_AGE_MS);
    return !r->spi_failed && !isnan(r->tc_temperature_c) && !fault_bits_bad && !too_old;
}

float thermo_zone_raw_from_readings(const MAX31856Reading *readings, size_t count,
                                    uint8_t thermo_mask, bool *out_valid)
{
    float ch_c[MAX31856_CHANNEL_COUNT];
    bool ch_ok[MAX31856_CHANNEL_COUNT];
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        ch_c[i] = NAN;
        ch_ok[i] = false;
    }
    if (readings) {
        for (size_t r = 0; r < count; r++) {
            uint8_t ci = readings[r].channel;
            if (ci >= MAX31856_CHANNEL_COUNT) continue;
            ch_c[ci] = readings[r].tc_temperature_c;
            ch_ok[ci] = !readings[r].spi_failed && !isnan(readings[r].tc_temperature_c);
        }
    }
    return thermo_combine(ch_c, ch_ok, MAX31856_CHANNEL_COUNT, thermo_mask, out_valid);
}

bool thermo_channel_cal_applies(uint8_t channel, uint8_t zone_thermo_mask)
{
    return channel < 8u && zone_thermo_mask == (uint8_t)(1u << channel);
}

void thermo_channels_read(MAX31856BusClass *bus, ThermoChannelSnapshot *out)
{
    for (uint8_t ci = 0; ci < MAX31856_CHANNEL_COUNT; ci++) {
        out->raw_c[ci] = NAN;
        out->ok[ci] = false;
        out->cj_c[ci] = NAN;
    }
    if (!(sim_backend_enabled() || (bus && bus->initialized))) {
        return;
    }
    MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
    size_t count = 0;
    if (sim_backend_enabled()) {
        sim_backend_read_all(readings, MAX31856_CHANNEL_COUNT, &count);
    } else {
        MAX31856_read_all(bus, readings, MAX31856_CHANNEL_COUNT, &count);
    }
    for (size_t i = 0; i < count; i++) {
        uint8_t ci = readings[i].channel;
        if (ci >= MAX31856_CHANNEL_COUNT) continue;
        out->raw_c[ci] = readings[i].tc_temperature_c;
        out->ok[ci] = thermo_reading_ok(&readings[i]);
        if (!readings[i].spi_failed) {
            out->cj_c[ci] = readings[i].cj_temperature_c;
        }
    }
}
