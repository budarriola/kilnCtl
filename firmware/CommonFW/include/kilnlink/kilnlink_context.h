#ifndef KILNLINK_CONTEXT_H
#define KILNLINK_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico context broadcast, SAFETY_CMD_PUSH_CONTEXT = 0x07 --
 * docs/LINK_PROTOCOL.md sec 4. This is the payload carried inside a
 * kilnlink_frame_t (see kilnlink_frame.h); this file only serializes the
 * payload bytes, it knows nothing about the envelope around them.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Fields are serialized by
 * hand, field by field, little endian -- no packed structs cast onto the
 * wire (rule 5). */

#define KILNLINK_CONTEXT_CMD 0x07u

/* docs/LINK_PROTOCOL.md sec 4: "zone_count (N, 0-3)". */
#define KILNLINK_CONTEXT_MAX_ZONES 3u

/* 15 (fixed header) + N * 14 (per-zone block). */
#define KILNLINK_CONTEXT_ZONE_BLOCK_LEN 14u
#define KILNLINK_CONTEXT_FIXED_LEN 15u
#define KILNLINK_CONTEXT_MAX_LEN \
    (KILNLINK_CONTEXT_FIXED_LEN + KILNLINK_CONTEXT_MAX_ZONES * KILNLINK_CONTEXT_ZONE_BLOCK_LEN)

/* Top-level flags byte (offset 1). */
typedef enum {
    KILNLINK_CONTEXT_FLAG_PROFILE_RUNNING = 0x01u,
    KILNLINK_CONTEXT_FLAG_ANY_ZONE_FAULTED = 0x02u,
    KILNLINK_CONTEXT_FLAG_HEAT_REQUESTED = 0x04u,
    KILNLINK_CONTEXT_FLAG_CONTEXT_VALID = 0x08u,
    KILNLINK_CONTEXT_FLAG_SIM_PLANT = 0x10u,
} kilnlink_context_flag_t;

/* Per-zone flags byte (zone block offset 1). */
typedef enum {
    KILNLINK_ZONE_FLAG_MEASURED_VALID = 0x01u,
    KILNLINK_ZONE_FLAG_ACTIVE = 0x02u,
    KILNLINK_ZONE_FLAG_RELAY_ON = 0x04u,
    KILNLINK_ZONE_FLAG_GUARD_TRIPPED = 0x08u,
} kilnlink_zone_flag_t;

typedef enum {
    KILNLINK_CONTEXT_OK = 0,
    KILNLINK_CONTEXT_ERR_BUFFER_TOO_SMALL, /* output buffer can't hold the encoded payload */
    KILNLINK_CONTEXT_ERR_TOO_SHORT,        /* fewer bytes than the fixed 15-byte header */
    KILNLINK_CONTEXT_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_CONTEXT_CMD */
    KILNLINK_CONTEXT_ERR_ZONE_COUNT,       /* zone_count > KILNLINK_CONTEXT_MAX_ZONES */
    KILNLINK_CONTEXT_ERR_LENGTH_MISMATCH,  /* buffer length doesn't match 15 + zone_count*14 */
} kilnlink_context_status_t;

typedef struct {
    uint8_t zone_index;
    uint8_t flags; /* kilnlink_zone_flag_t bits */
    float setpoint_c;
    float measured_c; /* raw, uncalibrated */
    uint8_t sample_counter;
    uint8_t tc_type;
    uint8_t tc_fault;
    /* offset 13, "reserved, 0" on the wire -- not exposed here, always
     * encoded as 0 and ignored on decode. */
} kilnlink_zone_context_t;

typedef struct {
    uint8_t flags; /* kilnlink_context_flag_t bits */
    uint8_t boot_id;
    uint32_t seq;
    uint32_t uptime_ms;
    uint8_t relay_now_mask;
    uint8_t relay_recent_mask;
    uint8_t recent_window_s;
    uint8_t zone_count; /* 0..KILNLINK_CONTEXT_MAX_ZONES */
    kilnlink_zone_context_t zones[KILNLINK_CONTEXT_MAX_ZONES];
} kilnlink_context_t;

/* Serializes `ctx` (SAFETY_CMD_PUSH_CONTEXT payload, byte 0 = 0x07 included)
 * into `out`. Returns the number of bytes written (15 + ctx->zone_count*14),
 * or 0 on error -- check `*status`. `ctx->zone_count` must be <=
 * KILNLINK_CONTEXT_MAX_ZONES; a caller building this from live state is
 * responsible for clamping upstream (docs/LINK_PROTOCOL.md sec 4 caps it at
 * 3 by construction, not by this codec silently truncating). */
size_t kilnlink_context_encode(const kilnlink_context_t *ctx, uint8_t *out, size_t out_cap,
                               kilnlink_context_status_t *status);

/* Parses a PUSH_CONTEXT payload (as extracted from kilnlink_frame_t::payload,
 * i.e. NOT including the outer frame header/CRC) into `out`. `len` must be
 * exactly 15 + zone_count*14 for the zone_count encoded in the payload --
 * this is untrusted input from another processor across an isolated link
 * (CommonFW/README.md rule 6), so a payload whose length byte disagrees with
 * how many bytes actually arrived is rejected rather than partially parsed. */
kilnlink_context_status_t kilnlink_context_decode(const uint8_t *payload, size_t len,
                                                  kilnlink_context_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_CONTEXT_H */
