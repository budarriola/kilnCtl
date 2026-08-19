#ifndef KILNLINK_ANNOUNCE_H
#define KILNLINK_ANNOUNCE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico version handshake, SAFETY_CMD_ANNOUNCE_VERSION = 0x0F --
 * docs/LINK_PROTOCOL.md sec 4. Part of the compatibility floor (frame ids
 * 0x00-0x0F): this codec, and the layout it serializes, must stay usable
 * across any KILNLINK_MIN_COMPATIBLE mismatch -- LINK_PROTOCOL.md sec 4,
 * "The compatibility floor: why a mismatch can never brick the link". Fields
 * may only ever be appended, never reordered or resized.
 *
 * Same layout as Frame C (SAFETY_CMD_FW_VERSION, Pico -> ESP), truncated at
 * boot_id -- no config_version/config_crc, since those describe the Pico's
 * own active config, not the ESP's.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Fields are serialized by
 * hand, field by field, little endian -- no packed structs cast onto the
 * wire (rule 5). */

#define KILNLINK_ANNOUNCE_CMD 0x0Fu

/* docs/LINK_PROTOCOL.md sec 4's table has no stated cap on commit_len /
 * datetime_len beyond the u8 length prefix (0..255) and the frame's own
 * 253-byte payload ceiling (kilnlink_frame.h). A build identity longer than
 * this is not a real build stamp, so bound it generously rather than trust
 * an unbounded wire-declared length -- CommonFW/README.md rule 6. */
#define KILNLINK_ANNOUNCE_MAX_COMMIT_LEN 64u
#define KILNLINK_ANNOUNCE_MAX_DATETIME_LEN 32u

/* cmd(1) + protocol(2) + min_compatible(2) + dirty(1) + commit_len(1) +
 * datetime_len(1) + boot_id(1) = 9 fixed bytes, plus the two variable-length
 * ASCII strings. */
#define KILNLINK_ANNOUNCE_FIXED_LEN 9u
#define KILNLINK_ANNOUNCE_MAX_LEN                                            \
    (KILNLINK_ANNOUNCE_FIXED_LEN + KILNLINK_ANNOUNCE_MAX_COMMIT_LEN +        \
     KILNLINK_ANNOUNCE_MAX_DATETIME_LEN)

typedef enum {
    KILNLINK_ANNOUNCE_OK = 0,
    KILNLINK_ANNOUNCE_ERR_BUFFER_TOO_SMALL, /* output buffer can't hold the encoded payload */
    KILNLINK_ANNOUNCE_ERR_TOO_SHORT,        /* fewer than the 9-byte fixed prefix */
    KILNLINK_ANNOUNCE_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_ANNOUNCE_CMD */
    KILNLINK_ANNOUNCE_ERR_STRING_TOO_LONG,  /* commit_len/datetime_len exceeds this codec's cap */
    KILNLINK_ANNOUNCE_ERR_LENGTH_MISMATCH,  /* buffer length doesn't match the declared string lengths */
} kilnlink_announce_status_t;

typedef struct {
    uint16_t protocol_version;    /* KILNLINK_PROTOCOL_VERSION of the sender */
    uint16_t min_compatible;      /* KILNLINK_MIN_COMPATIBLE of the sender */
    uint8_t dirty;                /* 0 = clean, 1 = dirty or unknown */
    uint8_t commit_len;           /* bytes actually used in commit[] */
    uint8_t commit[KILNLINK_ANNOUNCE_MAX_COMMIT_LEN]; /* ASCII, not null-terminated */
    uint8_t datetime_len;         /* bytes actually used in datetime[] */
    uint8_t datetime[KILNLINK_ANNOUNCE_MAX_DATETIME_LEN]; /* ASCII, not null-terminated */
    uint8_t boot_id;
} kilnlink_announce_t;

/* Serializes `msg` (SAFETY_CMD_ANNOUNCE_VERSION payload, byte 0 = 0x0F
 * included) into `out`. Returns the number of bytes written (9 +
 * commit_len + datetime_len), or 0 on error -- check `*status`.
 * `msg->commit_len` and `msg->datetime_len` must each be <= their
 * KILNLINK_ANNOUNCE_MAX_*_LEN cap; a caller building this from a real build
 * identity is responsible for clamping upstream. */
size_t kilnlink_announce_encode(const kilnlink_announce_t *msg, uint8_t *out, size_t out_cap,
                                 kilnlink_announce_status_t *status);

/* Parses an ANNOUNCE_VERSION payload (as extracted from
 * kilnlink_frame_t::payload, i.e. NOT including the outer frame
 * header/CRC) into `out`. `len` must be exactly 9 + commit_len +
 * datetime_len for the lengths encoded in the payload -- this is untrusted
 * input from another processor across an isolated link (CommonFW/README.md
 * rule 6), so a payload whose length bytes disagree with how many bytes
 * actually arrived is rejected rather than partially parsed.
 *
 * Per LINK_PROTOCOL.md sec 4/6 ("Read bytes 1-4 first and decide
 * compatibility before parsing anything after them"), protocol_version and
 * min_compatible are the first two fields and can be read the moment
 * KILNLINK_ANNOUNCE_OK is returned -- a caller that only cares about the
 * compatibility verdict never needs to look past them. */
kilnlink_announce_status_t kilnlink_announce_decode(const uint8_t *payload, size_t len,
                                                     kilnlink_announce_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_ANNOUNCE_H */
