#ifndef KILNLINK_GET_CT_AUTO_ZERO_H
#define KILNLINK_GET_CT_AUTO_ZERO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_GET_CT_AUTO_ZERO = 0x27 -- docs/LINK_PROTOCOL.md
 * sec 4. One byte, no arguments -- same shape as SAFETY_CMD_GET_CT_CAL. The
 * ESP polls this repeatedly after a SAFETY_CMD_CT_AUTO_ZERO_BEGIN (0x26)
 * until the reply (SAFETY_CMD_CT_AUTO_ZERO_STATUS, kilnlink_ct_auto_zero_
 * status.h) reports DONE, same "answer every copy seen" convention
 * GET_FW_VERSION/GET_CT_CAL use -- own id, never shared with its reply, per
 * this link's "request/reply ids must never be shared" rule.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. */

#define KILNLINK_GET_CT_AUTO_ZERO_CMD 0x27u
#define KILNLINK_GET_CT_AUTO_ZERO_LEN 1u /* cmd(1), no fields */

typedef enum {
    KILNLINK_GET_CT_AUTO_ZERO_OK = 0,
    KILNLINK_GET_CT_AUTO_ZERO_ERR_BUFFER_TOO_SMALL,
    KILNLINK_GET_CT_AUTO_ZERO_ERR_LENGTH_MISMATCH,
    KILNLINK_GET_CT_AUTO_ZERO_ERR_WRONG_CMD,
} kilnlink_get_ct_auto_zero_status_t;

typedef struct {
    uint8_t _unused; /* no fields -- kept nonempty for portable struct semantics */
} kilnlink_get_ct_auto_zero_t;

size_t kilnlink_get_ct_auto_zero_encode(const kilnlink_get_ct_auto_zero_t *msg, uint8_t *out,
                                         size_t out_cap, kilnlink_get_ct_auto_zero_status_t *status);

kilnlink_get_ct_auto_zero_status_t kilnlink_get_ct_auto_zero_decode(
    const uint8_t *payload, size_t len, kilnlink_get_ct_auto_zero_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_GET_CT_AUTO_ZERO_H */
