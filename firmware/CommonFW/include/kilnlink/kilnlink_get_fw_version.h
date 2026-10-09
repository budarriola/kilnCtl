#ifndef KILNLINK_GET_FW_VERSION_H
#define KILNLINK_GET_FW_VERSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_GET_FW_VERSION = 0x0B -- docs/LINK_PROTOCOL.md
 * sec 4. One byte, no arguments. Sent by the ESP at boot and whenever the
 * Pico's boot_id changes. The ESP retries this; the Pico answers every copy
 * it sees and never tracks whether its answer arrived (sec 2).
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. There is no payload beyond
 * the command byte, so this codec is a trivial fixed-length wrapper --
 * still going through the encode/decode + status-enum shape every other
 * kilnlink codec uses, rather than a special case, so callers don't need to
 * special-case a no-argument frame differently from the rest. */

#define KILNLINK_GET_FW_VERSION_CMD 0x0Bu
#define KILNLINK_GET_FW_VERSION_LEN 1u /* cmd(1), no fields */

typedef enum {
    KILNLINK_GET_FW_VERSION_OK = 0,
    KILNLINK_GET_FW_VERSION_ERR_BUFFER_TOO_SMALL, /* output buffer smaller than KILNLINK_GET_FW_VERSION_LEN */
    KILNLINK_GET_FW_VERSION_ERR_LENGTH_MISMATCH,  /* input length != KILNLINK_GET_FW_VERSION_LEN (fixed-size frame) */
    KILNLINK_GET_FW_VERSION_ERR_WRONG_CMD,        /* byte 0 isn't KILNLINK_GET_FW_VERSION_CMD */
} kilnlink_get_fw_version_status_t;

/* No fields -- the payload is the command byte alone. Kept as an (empty)
 * struct anyway so this codec's encode/decode signatures match every other
 * kilnlink_<name>_t codec's shape. */
typedef struct {
    uint8_t reserved; /* unused; always 0, not part of the wire payload */
} kilnlink_get_fw_version_t;

/* Serializes `msg` (SAFETY_CMD_GET_FW_VERSION payload, byte 0 = 0x0B) into
 * `out`. Always exactly KILNLINK_GET_FW_VERSION_LEN (1) byte. `msg` may be
 * NULL, since there is nothing in it to read. Returns 1, or 0 on
 * KILNLINK_GET_FW_VERSION_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_get_fw_version_encode(const kilnlink_get_fw_version_t *msg, uint8_t *out,
                                       size_t out_cap, kilnlink_get_fw_version_status_t *status);

/* Parses a GET_FW_VERSION payload (as extracted from
 * kilnlink_frame_t::payload) into `out`. `len` must be exactly
 * KILNLINK_GET_FW_VERSION_LEN -- this is untrusted input from another
 * processor across an isolated link (CommonFW/README.md rule 6). `out` may
 * be NULL, since there is nothing to fill in. */
kilnlink_get_fw_version_status_t kilnlink_get_fw_version_decode(const uint8_t *payload, size_t len,
                                                                 kilnlink_get_fw_version_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_GET_FW_VERSION_H */
