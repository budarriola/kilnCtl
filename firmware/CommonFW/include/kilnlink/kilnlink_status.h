#ifndef KILNLINK_STATUS_H
#define KILNLINK_STATUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP telemetry, Frame A: SAFETY_CMD_GET_STATUS = 0x01 --
 * docs/LINK_PROTOCOL.md sec 6. Byte-for-byte the existing 23-byte layout
 * KilnFW already parses (safety_link.h SAFETY_LINK_STATUS_FRAME_LEN = 23),
 * so this codec can be brought up against today's unmodified KilnFW.
 *
 * Only Frame A is implemented here (the one the protocol doc marks
 * "required" and gives a fully concrete, fixed-size layout for). Frames
 * B (DIAG), C (FW_VERSION), D (TRIP_EVENT), E (POWER) and the LOG relay
 * are documented in LINK_PROTOCOL.md sec 6 but not yet coded -- see
 * CommonFW/README.md's completion checklist.
 *
 * kilnlink_version.h documents protocol 5 -> 6 growing this frame by one
 * OPTIONAL byte: offset 23, `tx_dropped_sat` (saturating TX-ring-drop
 * counter), sent only once a sender has positively learned -- via
 * ANNOUNCE_VERSION -- that its peer speaks protocol 6+
 * (LINK_FRAME_STATUS_V2_MIN_PROTOCOL in firmware/SaftyFW/src/tasks/
 * link_frame.h). This codec mirrors that same negotiation, byte for byte:
 * `link_frame_pack_status()` is the reference implementation this was
 * written against, not reinvented independently. `has_tx_dropped` is
 * carried explicitly on `kilnlink_status_t`, never inferred from a
 * sentinel value -- same "found is carried explicitly" convention
 * kilnlink_param.h documents for its own optional-field problem -- because
 * a real `tx_dropped_sat == 0` (peer sent V2, no drops) must be
 * distinguishable from "peer only sent V1, this field was never on the
 * wire at all". A decoder that collapsed those two cases into the same
 * zero would misreport "zero drops" for a peer that never said anything
 * about drops.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. */

#define KILNLINK_STATUS_CMD 0x01u
#define KILNLINK_STATUS_LEN_V1 23u /* original layout, still the floor -- every peer understands this */
#define KILNLINK_STATUS_LEN_V2 24u /* V1 + tx_dropped_sat (protocol 6+, sent only when negotiated) */
#define KILNLINK_STATUS_LEN KILNLINK_STATUS_LEN_V1 /* kept as an alias: every existing caller that only
                                                      ever spoke V1 keeps compiling unchanged */

/* Flags byte (offset 1). Bits 0-1 (LINK_UP, FAULT) are the ESP's own -- the
 * Pico always sends them as 0; they are defined here only so a decoder can
 * name them, not because the Pico ever sets them. */
typedef enum {
    KILNLINK_STATUS_FLAG_LINK_UP = 0x01u, /* ESP-owned; Pico always sends 0 */
    KILNLINK_STATUS_FLAG_FAULT = 0x02u,   /* ESP-owned; Pico always sends 0 */
    KILNLINK_STATUS_FLAG_ESTOP = 0x04u,
    KILNLINK_STATUS_FLAG_RELAY = 0x08u,
    KILNLINK_STATUS_FLAG_ENABLED = 0x10u,
    KILNLINK_STATUS_FLAG_TEMP_VALID = 0x20u,
} kilnlink_status_flag_t;

typedef enum {
    KILNLINK_STATUS_OK = 0,
    KILNLINK_STATUS_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_STATUS_LEN */
    KILNLINK_STATUS_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_STATUS_LEN (fixed-size frame) */
    KILNLINK_STATUS_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_STATUS_CMD */
} kilnlink_status_status_t;

typedef struct {
    uint8_t flags; /* kilnlink_status_flag_t bits */
    float safety_tc_c;    /* NaN if invalid */
    float cold_junction_c; /* NaN if invalid */
    uint8_t tc_fault;      /* THERMO_FAULT_* bits */
    float current1_a;
    float current2_a;
    float current3_a;
    uint8_t has_tx_dropped; /* 0/1 -- 1 iff tx_dropped_sat below was actually on the wire (V2, 24
                                bytes); 0 means "peer only sent V1 (23 bytes)", NOT "zero drops" --
                                see this header's file comment. Controls encode()'s output length. */
    uint8_t tx_dropped_sat; /* saturating TX-ring-drop counter (LINK_FRAME_STATUS_TX_DROPPED_SAT_MAX
                                = 254 caps it, 255 = "254 or more" -- link_frame.h); meaningful only
                                when has_tx_dropped == 1, ignored by encode() otherwise */
} kilnlink_status_t;

/* Serializes `st` (SAFETY_CMD_GET_STATUS payload, byte 0 = 0x01 included)
 * into `out`. Writes KILNLINK_STATUS_LEN_V2 (24) bytes, including byte 23 =
 * st->tx_dropped_sat, iff st->has_tx_dropped is nonzero; otherwise writes
 * exactly KILNLINK_STATUS_LEN_V1 (23) bytes and byte 23 is not touched.
 * Mirrors link_frame_pack_status()'s own negotiation -- the caller decides
 * has_tx_dropped (by the same "peer proved it speaks protocol 6+" rule
 * that function's own doc comment gives), this codec only ever acts on
 * that verdict. Returns the number of bytes written, or 0 on
 * KILNLINK_STATUS_ERR_BUFFER_TOO_SMALL if `out_cap` is smaller than that. */
size_t kilnlink_status_encode(const kilnlink_status_t *st, uint8_t *out, size_t out_cap,
                              kilnlink_status_status_t *status);

/* Parses a GET_STATUS payload (as extracted from kilnlink_frame_t::payload)
 * into `out`. `len` must be exactly KILNLINK_STATUS_LEN_V1 (23) or
 * KILNLINK_STATUS_LEN_V2 (24) -- any other length is
 * KILNLINK_STATUS_ERR_LENGTH_MISMATCH. This is untrusted input from another
 * processor across an isolated link (CommonFW/README.md rule 6).
 * out->has_tx_dropped is set to 1 (and out->tx_dropped_sat to payload[23])
 * only when `len` is 24; for a 23-byte frame out->has_tx_dropped is set to
 * 0 and out->tx_dropped_sat to 0 (0 there is a placeholder, not a claim --
 * has_tx_dropped is what a caller must check, per this header's file
 * comment). */
kilnlink_status_status_t kilnlink_status_decode(const uint8_t *payload, size_t len,
                                                kilnlink_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_STATUS_H */
