#include "kilnlink/kilnlink_inject_tc.h"

#include "kilnlink/kilnlink_bytes.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0      u8  cmd (0x21)
 *   1      u8  valid
 *   2      f32 tc_c LE
 *   6      f32 cj_c LE
 *   10     u8  fault_bits
 */
#define OFF_VALID      1u
#define OFF_TC_C       2u
#define OFF_CJ_C       6u
#define OFF_FAULT_BITS 10u

size_t kilnlink_inject_tc_encode(const kilnlink_inject_tc_t *msg, uint8_t *out, size_t out_cap,
                                  kilnlink_inject_tc_status_t *status)
{
    kilnlink_inject_tc_status_t local_status = KILNLINK_INJECT_TC_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_INJECT_TC_OK;

    if (out_cap < KILNLINK_INJECT_TC_LEN) {
        *status = KILNLINK_INJECT_TC_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_INJECT_TC_CMD;
    out[OFF_VALID] = msg->valid;
    kilnlink_put_f32le(out, OFF_TC_C, msg->tc_c);
    kilnlink_put_f32le(out, OFF_CJ_C, msg->cj_c);
    out[OFF_FAULT_BITS] = msg->fault_bits;

    return KILNLINK_INJECT_TC_LEN;
}

kilnlink_inject_tc_status_t kilnlink_inject_tc_decode(const uint8_t *payload, size_t len,
                                                       kilnlink_inject_tc_t *out)
{
    if (len != KILNLINK_INJECT_TC_LEN) {
        return KILNLINK_INJECT_TC_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_INJECT_TC_CMD) {
        return KILNLINK_INJECT_TC_ERR_WRONG_CMD;
    }

    out->valid = payload[OFF_VALID];
    out->tc_c = kilnlink_get_f32le(payload, OFF_TC_C);
    out->cj_c = kilnlink_get_f32le(payload, OFF_CJ_C);
    out->fault_bits = payload[OFF_FAULT_BITS];

    return KILNLINK_INJECT_TC_OK;
}
