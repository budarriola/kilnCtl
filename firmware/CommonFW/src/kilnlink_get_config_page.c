#include "kilnlink/kilnlink_get_config_page.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0      u8  cmd (0x1F)
 *   1      u8  page_index
 */
#define OFF_PAGE_INDEX 1u

size_t kilnlink_get_config_page_encode(const kilnlink_get_config_page_t *msg, uint8_t *out,
                                        size_t out_cap,
                                        kilnlink_get_config_page_status_t *status)
{
    kilnlink_get_config_page_status_t local_status = KILNLINK_GET_CONFIG_PAGE_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_GET_CONFIG_PAGE_OK;

    if (out_cap < KILNLINK_GET_CONFIG_PAGE_LEN) {
        *status = KILNLINK_GET_CONFIG_PAGE_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_GET_CONFIG_PAGE_CMD;
    out[OFF_PAGE_INDEX] = msg->page_index;

    return KILNLINK_GET_CONFIG_PAGE_LEN;
}

kilnlink_get_config_page_status_t kilnlink_get_config_page_decode(const uint8_t *payload,
                                                                    size_t len,
                                                                    kilnlink_get_config_page_t *out)
{
    if (len != KILNLINK_GET_CONFIG_PAGE_LEN) {
        return KILNLINK_GET_CONFIG_PAGE_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_GET_CONFIG_PAGE_CMD) {
        return KILNLINK_GET_CONFIG_PAGE_ERR_WRONG_CMD;
    }

    out->page_index = payload[OFF_PAGE_INDEX];

    return KILNLINK_GET_CONFIG_PAGE_OK;
}
