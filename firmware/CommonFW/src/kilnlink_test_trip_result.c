#include "kilnlink/kilnlink_test_trip_result.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0  u8  cmd (0x2F)
 *   1  u8  request_id
 *   2  u8  outcome
 *   3  u8  trip_seq
 */
#define OFF_REQUEST_ID 1u
#define OFF_OUTCOME 2u
#define OFF_TRIP_SEQ 3u

size_t kilnlink_test_trip_result_encode(const kilnlink_test_trip_result_t *msg, uint8_t *out,
                                        size_t out_cap, kilnlink_test_trip_result_status_t *status)
{
    kilnlink_test_trip_result_status_t local_status = KILNLINK_TEST_TRIP_RESULT_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_TEST_TRIP_RESULT_OK;

    if (out_cap < KILNLINK_TEST_TRIP_RESULT_LEN) {
        *status = KILNLINK_TEST_TRIP_RESULT_ERR_BUFFER_TOO_SMALL;
        return 0;
    }
    if (msg->outcome > KILNLINK_TEST_TRIP_OUTCOME_MAX) {
        *status = KILNLINK_TEST_TRIP_RESULT_ERR_BAD_OUTCOME;
        return 0;
    }

    out[0] = KILNLINK_TEST_TRIP_RESULT_CMD;
    out[OFF_REQUEST_ID] = msg->request_id;
    out[OFF_OUTCOME] = msg->outcome;
    out[OFF_TRIP_SEQ] = msg->trip_seq;

    return KILNLINK_TEST_TRIP_RESULT_LEN;
}

kilnlink_test_trip_result_status_t kilnlink_test_trip_result_decode(const uint8_t *payload, size_t len,
                                                                    kilnlink_test_trip_result_t *out)
{
    if (len != KILNLINK_TEST_TRIP_RESULT_LEN) {
        return KILNLINK_TEST_TRIP_RESULT_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_TEST_TRIP_RESULT_CMD) {
        return KILNLINK_TEST_TRIP_RESULT_ERR_WRONG_CMD;
    }
    if (payload[OFF_OUTCOME] > KILNLINK_TEST_TRIP_OUTCOME_MAX) {
        return KILNLINK_TEST_TRIP_RESULT_ERR_BAD_OUTCOME;
    }

    if (out) {
        out->request_id = payload[OFF_REQUEST_ID];
        out->outcome = payload[OFF_OUTCOME];
        out->trip_seq = payload[OFF_TRIP_SEQ];
    }

    return KILNLINK_TEST_TRIP_RESULT_OK;
}
