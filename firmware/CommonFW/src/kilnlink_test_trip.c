#include "kilnlink/kilnlink_test_trip.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0  u8  cmd (0x2E)
 *   1  u8  pico_boot_id
 *   2  u8  request_id
 *   3  u8  magic (0xA5)
 */
#define OFF_BOOT_ID 1u
#define OFF_REQUEST_ID 2u
#define OFF_MAGIC 3u

size_t kilnlink_test_trip_encode(const kilnlink_test_trip_t *msg, uint8_t *out, size_t out_cap,
                                 kilnlink_test_trip_status_t *status)
{
    kilnlink_test_trip_status_t local_status = KILNLINK_TEST_TRIP_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_TEST_TRIP_OK;

    if (out_cap < KILNLINK_TEST_TRIP_LEN) {
        *status = KILNLINK_TEST_TRIP_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_TEST_TRIP_CMD;
    out[OFF_BOOT_ID] = msg->pico_boot_id;
    out[OFF_REQUEST_ID] = msg->request_id;
    out[OFF_MAGIC] = msg->magic;

    return KILNLINK_TEST_TRIP_LEN;
}

kilnlink_test_trip_status_t kilnlink_test_trip_decode(const uint8_t *payload, size_t len,
                                                      kilnlink_test_trip_t *out)
{
    if (len != KILNLINK_TEST_TRIP_LEN) {
        return KILNLINK_TEST_TRIP_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_TEST_TRIP_CMD) {
        return KILNLINK_TEST_TRIP_ERR_WRONG_CMD;
    }

    if (out) {
        out->pico_boot_id = payload[OFF_BOOT_ID];
        out->request_id = payload[OFF_REQUEST_ID];
        out->magic = payload[OFF_MAGIC];
    }

    return KILNLINK_TEST_TRIP_OK;
}
