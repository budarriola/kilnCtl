#ifndef KILNLINK_DIAG_H
#define KILNLINK_DIAG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP telemetry, Frame B: SAFETY_CMD_DIAG = 0x08 --
 * docs/LINK_PROTOCOL.md sec 6. "Everything the 23-byte frame has no room
 * for." Additive: a KilnFW that has never heard of 0x08 ignores it, so this
 * can ship before the ESP side decodes it.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Same conventions as
 * kilnlink_status.c / kilnlink_power.c: byte-for-byte little-endian packing
 * via kilnlink_bytes.h, fixed-size frame (no variable-length fields). */

#define KILNLINK_DIAG_CMD 0x08u
#define KILNLINK_DIAG_LEN 26u

/* boot_reason byte (offset 10). */
typedef enum {
    KILNLINK_DIAG_BOOT_POWERON  = 0x01u,
    KILNLINK_DIAG_BOOT_WATCHDOG = 0x02u,
    KILNLINK_DIAG_BOOT_BROWNOUT = 0x04u,
} kilnlink_diag_boot_flag_t;

/* flags byte (offset 25). */
typedef enum {
    KILNLINK_DIAG_FLAG_SIM_CONTEXT_SEEN      = 0x01u,
    KILNLINK_DIAG_FLAG_CALIBRATION_MISSING   = 0x02u,
    KILNLINK_DIAG_FLAG_ESTOP_UNWIRED_SUSPECT = 0x04u,
} kilnlink_diag_flag_t;

/* state byte (offset 24) -- LINK_PROTOCOL.md sec 6, Frame B: 0 init,
 * 1 grace, 2 armed, 3 warn, 4 tripped. */
typedef enum {
    KILNLINK_DIAG_STATE_INIT    = 0,
    KILNLINK_DIAG_STATE_GRACE   = 1,
    KILNLINK_DIAG_STATE_ARMED   = 2,
    KILNLINK_DIAG_STATE_WARN    = 3,
    KILNLINK_DIAG_STATE_TRIPPED = 4,
} kilnlink_diag_state_t;

/* context_age_100ms sentinel: "never received" -- same convention link_task's
 * own build uses (LINK_PROTOCOL.md sec 6, byte 11). */
#define KILNLINK_DIAG_CONTEXT_AGE_NEVER 255u

typedef enum {
    KILNLINK_DIAG_OK = 0,
    KILNLINK_DIAG_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_DIAG_LEN */
    KILNLINK_DIAG_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_DIAG_LEN (fixed-size frame) */
    KILNLINK_DIAG_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_DIAG_CMD */
} kilnlink_diag_status_t;

typedef struct {
    uint8_t  trip_reason;         /* SAFETY_TRIP_* (SaftyFW's safety_guards.h), 0 = none */
    uint16_t warn_mask;           /* one bit per guard currently warning */
    uint16_t trip_mask;           /* one bit per guard currently tripped */
    uint32_t uptime_ms;           /* Pico uptime */
    uint8_t  boot_reason;         /* kilnlink_diag_boot_flag_t bits */
    uint8_t  context_age_100ms;   /* KILNLINK_DIAG_CONTEXT_AGE_NEVER if never received */
    uint32_t context_frames_ok;
    uint32_t context_frames_bad;  /* CRC/framing/length errors */
    uint32_t tx_frames_dropped;   /* TX ring full */
    uint8_t  state;               /* kilnlink_diag_state_t */
    uint8_t  flags;               /* kilnlink_diag_flag_t bits */
} kilnlink_diag_t;

/* Serializes `dg` (SAFETY_CMD_DIAG payload, byte 0 = 0x08 included) into
 * `out`. Always exactly KILNLINK_DIAG_LEN (26) bytes -- this frame has no
 * variable-length fields. Returns 26, or 0 on
 * KILNLINK_DIAG_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_diag_encode(const kilnlink_diag_t *dg, uint8_t *out, size_t out_cap,
                            kilnlink_diag_status_t *status);

/* Parses a DIAG payload (as extracted from kilnlink_frame_t::payload) into
 * `out`. `len` must be exactly KILNLINK_DIAG_LEN -- this is untrusted input
 * from another processor across an isolated link (CommonFW/README.md
 * rule 6). */
kilnlink_diag_status_t kilnlink_diag_decode(const uint8_t *payload, size_t len,
                                            kilnlink_diag_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_DIAG_H */
