#ifndef KILNLINK_POWER_H
#define KILNLINK_POWER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP telemetry, Frame E: SAFETY_CMD_POWER = 0x0E --
 * docs/LINK_PROTOCOL.md sec 6. "No guard reads any of this. It exists to be
 * displayed" -- unlike Frame A (GET_STATUS), nothing in either firmware's
 * safety logic depends on this frame; it feeds the ESP web GUI / LCD "Safety
 * Processor" power readout only.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Same conventions as
 * kilnlink_status.c: byte-for-byte little-endian packing via
 * kilnlink_bytes.h, fixed-size frame (no variable-length fields), NaN means
 * "not a valid reading" rather than 0. */

#define KILNLINK_POWER_CMD 0x0Eu
#define KILNLINK_POWER_CHANNELS 3u
#define KILNLINK_POWER_LEN 55u

/* Flags byte (offset 2). */
typedef enum {
    KILNLINK_POWER_FLAG_MAINS_VOLTAGE_CONFIGURED = 0x01u,
    KILNLINK_POWER_FLAG_ANY_CHANNEL_CLIPPED = 0x02u,
    KILNLINK_POWER_FLAG_CALIBRATED = 0x04u,
} kilnlink_power_flag_t;

typedef enum {
    KILNLINK_POWER_OK = 0,
    KILNLINK_POWER_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_POWER_LEN */
    KILNLINK_POWER_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_POWER_LEN (fixed-size frame) */
    KILNLINK_POWER_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_POWER_CMD */
} kilnlink_power_status_t;

typedef struct {
    uint8_t power_window_s;    /* the window the conduction fractions cover, default 120 */
    uint8_t flags;             /* kilnlink_power_flag_t bits */
    float   mains_voltage_v;   /* as configured; NaN if not set */
    /* Per channel (index 0..KILNLINK_POWER_CHANNELS-1): draw *while
     * conducting*, not duty-averaged -- see firmware/SaftyFW/docs/
     * CURRENT_SENSE.md sec 3b for why this and conduction_fraction are
     * reported separately rather than pre-multiplied on the wire. */
    float i_conducting_a[KILNLINK_POWER_CHANNELS];
    float conduction_fraction[KILNLINK_POWER_CHANNELS];
    /* Per channel average power, NaN if mains_voltage_v is unset or that
     * channel clipped. */
    float p_avg_w[KILNLINK_POWER_CHANNELS];
    float  p_total_w;  /* NaN if any contributing channel is invalid */
    double energy_wh;  /* accumulated since the Pico last booted; resets on reboot */
} kilnlink_power_t;

/* Serializes `pw` (SAFETY_CMD_POWER payload, byte 0 = 0x0E included) into
 * `out`. Always exactly KILNLINK_POWER_LEN (55) bytes -- this frame has no
 * variable-length fields. Returns 55, or 0 on
 * KILNLINK_POWER_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_power_encode(const kilnlink_power_t *pw, uint8_t *out, size_t out_cap,
                             kilnlink_power_status_t *status);

/* Parses a POWER payload (as extracted from kilnlink_frame_t::payload) into
 * `out`. `len` must be exactly KILNLINK_POWER_LEN -- this is untrusted input
 * from another processor across an isolated link (CommonFW/README.md
 * rule 6). */
kilnlink_power_status_t kilnlink_power_decode(const uint8_t *payload, size_t len,
                                              kilnlink_power_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_POWER_H */
