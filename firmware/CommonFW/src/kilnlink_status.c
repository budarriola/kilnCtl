#include "kilnlink/kilnlink_status.h"

#include "kilnlink/kilnlink_bytes.h"

size_t kilnlink_status_encode(const kilnlink_status_t *st, uint8_t *out, size_t out_cap,
                              kilnlink_status_status_t *status)
{
    kilnlink_status_status_t local_status = KILNLINK_STATUS_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_STATUS_OK;

    if (out_cap < KILNLINK_STATUS_LEN) {
        *status = KILNLINK_STATUS_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_STATUS_CMD;
    out[1] = st->flags;
    kilnlink_put_f32le(out, 2, st->safety_tc_c);
    kilnlink_put_f32le(out, 6, st->cold_junction_c);
    out[10] = st->tc_fault;
    kilnlink_put_f32le(out, 11, st->current1_a);
    kilnlink_put_f32le(out, 15, st->current2_a);
    kilnlink_put_f32le(out, 19, st->current3_a);

    return KILNLINK_STATUS_LEN;
}

kilnlink_status_status_t kilnlink_status_decode(const uint8_t *payload, size_t len,
                                                kilnlink_status_t *out)
{
    if (len != KILNLINK_STATUS_LEN) {
        return KILNLINK_STATUS_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_STATUS_CMD) {
        return KILNLINK_STATUS_ERR_WRONG_CMD;
    }

    out->flags = payload[1];
    out->safety_tc_c = kilnlink_get_f32le(payload, 2);
    out->cold_junction_c = kilnlink_get_f32le(payload, 6);
    out->tc_fault = payload[10];
    out->current1_a = kilnlink_get_f32le(payload, 11);
    out->current2_a = kilnlink_get_f32le(payload, 15);
    out->current3_a = kilnlink_get_f32le(payload, 19);

    return KILNLINK_STATUS_OK;
}
