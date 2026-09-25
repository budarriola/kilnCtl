#include "thermo_channel_read.h"

#include <math.h>

#include "sim_backend.h"

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
        bool fault_bits_bad = (readings[i].fault_status & (0x01u | 0x02u | 0x40u)) != 0;
        out->ok[ci] = !readings[i].spi_failed && !isnan(out->raw_c[ci]) && !fault_bits_bad;
        if (!readings[i].spi_failed) {
            out->cj_c[ci] = readings[i].cj_temperature_c;
        }
    }
}
