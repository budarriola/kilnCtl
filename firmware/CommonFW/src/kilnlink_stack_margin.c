#include "kilnlink/kilnlink_stack_margin.h"
#include "kilnlink/kilnlink_bytes.h"

/* Offsets, per docs/LINK_PROTOCOL.md sec 4:
 *   0                        u8  cmd (0x2C)
 *   1                        u8  rounds_completed
 *   2 + i*5 + 0              u8  entries[i].task_id
 *   2 + i*5 + 1              u16 entries[i].high_water_words
 *   2 + i*5 + 3              u16 entries[i].stack_total_words
 * for i in [0, KILNLINK_STACK_MARGIN_NUM_TASKS)
 */
#define OFF_ROUNDS   1u
#define OFF_ENTRIES  2u

size_t kilnlink_stack_margin_encode(const kilnlink_stack_margin_t *msg, uint8_t *out,
                                     size_t out_cap, kilnlink_stack_margin_status_t *status)
{
    kilnlink_stack_margin_status_t local_status = KILNLINK_STACK_MARGIN_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_STACK_MARGIN_OK;

    if (out_cap < KILNLINK_STACK_MARGIN_LEN) {
        *status = KILNLINK_STACK_MARGIN_ERR_BUFFER_TOO_SMALL;
        return 0;
    }
    if (!msg) {
        *status = KILNLINK_STACK_MARGIN_ERR_BUFFER_TOO_SMALL; /* no distinct "null msg" code; treat as unusable */
        return 0;
    }

    out[0] = KILNLINK_STACK_MARGIN_CMD;
    out[OFF_ROUNDS] = msg->rounds_completed;

    for (size_t i = 0; i < KILNLINK_STACK_MARGIN_NUM_TASKS; i++) {
        size_t base = OFF_ENTRIES + i * KILNLINK_STACK_MARGIN_ENTRY_LEN;
        out[base] = msg->entries[i].task_id;
        kilnlink_put_u16le(out, base + 1u, msg->entries[i].high_water_words);
        kilnlink_put_u16le(out, base + 3u, msg->entries[i].stack_total_words);
    }

    return KILNLINK_STACK_MARGIN_LEN;
}

kilnlink_stack_margin_status_t kilnlink_stack_margin_decode(const uint8_t *payload, size_t len,
                                                              kilnlink_stack_margin_t *out)
{
    if (len != KILNLINK_STACK_MARGIN_LEN) {
        return KILNLINK_STACK_MARGIN_ERR_LENGTH_MISMATCH;
    }
    if (payload[0] != KILNLINK_STACK_MARGIN_CMD) {
        return KILNLINK_STACK_MARGIN_ERR_WRONG_CMD;
    }

    if (out) {
        out->rounds_completed = payload[OFF_ROUNDS];
        for (size_t i = 0; i < KILNLINK_STACK_MARGIN_NUM_TASKS; i++) {
            size_t base = OFF_ENTRIES + i * KILNLINK_STACK_MARGIN_ENTRY_LEN;
            out->entries[i].task_id = payload[base];
            out->entries[i].high_water_words = kilnlink_get_u16le(payload, base + 1u);
            out->entries[i].stack_total_words = kilnlink_get_u16le(payload, base + 3u);
        }
    }

    return KILNLINK_STACK_MARGIN_OK;
}
