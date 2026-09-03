#ifndef KILNLINK_FW_VERSION_H
#define KILNLINK_FW_VERSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP build identity, Frame C, SAFETY_CMD_FW_VERSION = 0x0B --
 * docs/LINK_PROTOCOL.md sec 6. Sent in reply to SAFETY_CMD_GET_FW_VERSION
 * (kilnlink_get_fw_version.h, also 0x0B -- a deliberate, documented shared
 * id: LINK_PROTOCOL.md's "Request/reply ids must never be shared" rule
 * carves this pair out on purpose, because it sits in the frozen 0x00-0x0F
 * compatibility floor, where a refusal reply is never needed), and pushed
 * unsolicited once at boot.
 *
 * Part of the compatibility floor (frame ids 0x00-0x0F): this codec, and the
 * layout it serializes, must stay usable across any KILNLINK_MIN_COMPATIBLE
 * mismatch -- LINK_PROTOCOL.md sec 4, "The compatibility floor: why a
 * mismatch can never brick the link". Fields may only ever be appended,
 * never reordered or resized. "Read bytes 1-4 first and decide
 * compatibility before parsing anything after them" -- protocol_version and
 * min_compatible are the first two fields and are valid the moment
 * KILNLINK_FW_VERSION_OK is returned.
 *
 * Same layout as SAFETY_CMD_ANNOUNCE_VERSION (kilnlink_announce.h), extended
 * past boot_id with config_version and config_crc -- fields that describe
 * the Pico's own active config, which ANNOUNCE_VERSION (an ESP->Pico frame)
 * has no reason to carry.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Fields are serialized by
 * hand, field by field, little endian -- no packed structs cast onto the
 * wire (rule 5). */

#define KILNLINK_FW_VERSION_CMD 0x0Bu

/* Same caps and same rationale as KILNLINK_ANNOUNCE_MAX_COMMIT_LEN /
 * KILNLINK_ANNOUNCE_MAX_DATETIME_LEN: LINK_PROTOCOL.md sec 6's table has no
 * stated cap on commit_len / datetime_len beyond the u8 length prefix
 * (0..255) and the frame's own 253-byte payload ceiling (kilnlink_frame.h).
 * A build identity longer than this is not a real build stamp, so bound it
 * generously rather than trust an unbounded wire-declared length --
 * CommonFW/README.md rule 6. */
#define KILNLINK_FW_VERSION_MAX_COMMIT_LEN 64u
#define KILNLINK_FW_VERSION_MAX_DATETIME_LEN 32u

/* cmd(1) + protocol(2) + min_compatible(2) + dirty(1) + commit_len(1) +
 * datetime_len(1) + boot_id(1) + config_version(1) + config_crc(2) = 12
 * fixed bytes, plus the two variable-length ASCII strings. */
#define KILNLINK_FW_VERSION_FIXED_LEN 12u
#define KILNLINK_FW_VERSION_MAX_LEN                                         \
    (KILNLINK_FW_VERSION_FIXED_LEN + KILNLINK_FW_VERSION_MAX_COMMIT_LEN +   \
     KILNLINK_FW_VERSION_MAX_DATETIME_LEN)

typedef enum {
    KILNLINK_FW_VERSION_OK = 0,
    KILNLINK_FW_VERSION_ERR_BUFFER_TOO_SMALL, /* output buffer can't hold the encoded payload */
    KILNLINK_FW_VERSION_ERR_TOO_SHORT,        /* fewer than the 12-byte fixed prefix */
    KILNLINK_FW_VERSION_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_FW_VERSION_CMD */
    KILNLINK_FW_VERSION_ERR_STRING_TOO_LONG,  /* commit_len/datetime_len exceeds this codec's cap */
    KILNLINK_FW_VERSION_ERR_LENGTH_MISMATCH,  /* buffer length doesn't match the declared string lengths */
} kilnlink_fw_version_status_t;

typedef struct {
    uint16_t protocol_version;    /* KILNLINK_PROTOCOL_VERSION of the sender */
    uint16_t min_compatible;      /* KILNLINK_MIN_COMPATIBLE of the sender */
    uint8_t dirty;                /* 0 = clean, 1 = dirty or unknown */
    uint8_t commit_len;           /* bytes actually used in commit[] */
    uint8_t commit[KILNLINK_FW_VERSION_MAX_COMMIT_LEN]; /* ASCII, not null-terminated */
    uint8_t datetime_len;         /* bytes actually used in datetime[] */
    uint8_t datetime[KILNLINK_FW_VERSION_MAX_DATETIME_LEN]; /* ASCII, not null-terminated */
    uint8_t boot_id;
    uint8_t config_version;
    uint16_t config_crc;          /* CRC of the Pico's active threshold/calibration set; 0 = never commissioned */
} kilnlink_fw_version_t;

/* Serializes `msg` (SAFETY_CMD_FW_VERSION payload, byte 0 = 0x0B included)
 * into `out`. Returns the number of bytes written (12 + commit_len +
 * datetime_len), or 0 on error -- check `*status`. `msg->commit_len` and
 * `msg->datetime_len` must each be <= their KILNLINK_FW_VERSION_MAX_*_LEN
 * cap; a caller building this from a real build identity is responsible for
 * clamping upstream. */
size_t kilnlink_fw_version_encode(const kilnlink_fw_version_t *msg, uint8_t *out, size_t out_cap,
                                   kilnlink_fw_version_status_t *status);

/* Parses a FW_VERSION payload (as extracted from kilnlink_frame_t::payload,
 * i.e. NOT including the outer frame header/CRC) into `out`. `len` must be
 * exactly 12 + commit_len + datetime_len for the lengths encoded in the
 * payload -- this is untrusted input from another processor across an
 * isolated link (CommonFW/README.md rule 6), so a payload whose length
 * bytes disagree with how many bytes actually arrived is rejected rather
 * than partially parsed. */
kilnlink_fw_version_status_t kilnlink_fw_version_decode(const uint8_t *payload, size_t len,
                                                          kilnlink_fw_version_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_FW_VERSION_H */
