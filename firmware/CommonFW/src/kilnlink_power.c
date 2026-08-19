#include "kilnlink/kilnlink_power.h"

#include "kilnlink/kilnlink_bytes.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 6 Frame E:
 *   0        u8  cmd (0x0E)
 *   1        u8  power_window_s
 *   2        u8  flags
 *   3..6     f32 mains_voltage_v
 *   7..30    3 x (f32 i_conducting_a, f32 conduction_fraction), 8 bytes each
 *   31..42   3 x f32 p_avg_w
 *   43..46   f32 p_total_w
 *   47..54   f64 energy_wh
 */
#define OFF_WINDOW 1u
#define OFF_FLAGS 2u
#define OFF_MAINS_V 3u
#define OFF_CHANNELS 7u
#define OFF_CHANNEL_STRIDE 8u
#define OFF_P_AVG 31u
#define OFF_P_TOTAL 43u
#define OFF_ENERGY 47u

size_t kilnlink_power_encode(const kilnlink_power_t *pw, uint8_t *out, size_t out_cap,
                             kilnlink_power_status_t *status)
{
    kilnlink_power_status_t local_status = KILNLINK_POWER_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_POWER_OK;

    if (out_cap < KILNLINK_POWER_LEN) {
        *status = KILNLINK_POWER_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_POWER_CMD;
    out[OFF_WINDOW] = pw->power_window_s;
    out[OFF_FLAGS] = pw->flags;
    kilnlink_put_f32le(out, OFF_MAINS_V, pw->mains_voltage_v);

    for (unsigned ch = 0; ch < KILNLINK_POWER_CHANNELS; ch++) {
        size_t base = OFF_CHANNELS + (size_t)ch * OFF_CHANNEL_STRIDE;
        kilnlink_put_f32le(out, base, pw->i_conducting_a[ch]);
        kilnlink_put_f32le(out, base + 4u, pw->conduction_fraction[ch]);
    }

    for (unsigned ch = 0; ch < KILNLINK_POWER_CHANNELS; ch++) {
        kilnlink_put_f32le(out, OFF_P_AVG + (size_t)ch * 4u, pw->p_avg_w[ch]);
    }

    kilnlink_put_f32le(out, OFF_P_TOTAL, pw->p_total_w);
    kilnlink_put_f64le(out, OFF_ENERGY, pw->energy_wh);

    return KILNLINK_POWER_LEN;
}

kilnlink_power_status_t kilnlink_power_decode(const uint8_t *payload, size_t len,
                                              kilnlink_power_t *out)
{
    if (len != KILNLINK_POWER_LEN) {
        return KILNLINK_POWER_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_POWER_CMD) {
        return KILNLINK_POWER_ERR_WRONG_CMD;
    }

    out->power_window_s = payload[OFF_WINDOW];
    out->flags = payload[OFF_FLAGS];
    out->mains_voltage_v = kilnlink_get_f32le(payload, OFF_MAINS_V);

    for (unsigned ch = 0; ch < KILNLINK_POWER_CHANNELS; ch++) {
        size_t base = OFF_CHANNELS + (size_t)ch * OFF_CHANNEL_STRIDE;
        out->i_conducting_a[ch] = kilnlink_get_f32le(payload, base);
        out->conduction_fraction[ch] = kilnlink_get_f32le(payload, base + 4u);
    }

    for (unsigned ch = 0; ch < KILNLINK_POWER_CHANNELS; ch++) {
        out->p_avg_w[ch] = kilnlink_get_f32le(payload, OFF_P_AVG + (size_t)ch * 4u);
    }

    out->p_total_w = kilnlink_get_f32le(payload, OFF_P_TOTAL);
    out->energy_wh = kilnlink_get_f64le(payload, OFF_ENERGY);

    return KILNLINK_POWER_OK;
}
