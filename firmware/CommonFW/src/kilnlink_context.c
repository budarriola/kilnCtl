#include "kilnlink/kilnlink_context.h"

#include "kilnlink/kilnlink_bytes.h"

static size_t context_wire_len(uint8_t zone_count)
{
    return (size_t)KILNLINK_CONTEXT_FIXED_LEN +
           (size_t)zone_count * KILNLINK_CONTEXT_ZONE_BLOCK_LEN;
}

size_t kilnlink_context_encode(const kilnlink_context_t *ctx, uint8_t *out, size_t out_cap,
                               kilnlink_context_status_t *status)
{
    kilnlink_context_status_t local_status = KILNLINK_CONTEXT_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_CONTEXT_OK;

    if (ctx->zone_count > KILNLINK_CONTEXT_MAX_ZONES) {
        *status = KILNLINK_CONTEXT_ERR_ZONE_COUNT;
        return 0;
    }

    size_t needed = context_wire_len(ctx->zone_count);
    if (out_cap < needed) {
        *status = KILNLINK_CONTEXT_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out[0] = KILNLINK_CONTEXT_CMD;
    out[1] = ctx->flags;
    out[2] = ctx->boot_id;
    kilnlink_put_u32le(out, 3, ctx->seq);
    kilnlink_put_u32le(out, 7, ctx->uptime_ms);
    out[11] = ctx->relay_now_mask;
    out[12] = ctx->relay_recent_mask;
    out[13] = ctx->recent_window_s;
    out[14] = ctx->zone_count;

    size_t o = KILNLINK_CONTEXT_FIXED_LEN;
    for (uint8_t i = 0; i < ctx->zone_count; ++i) {
        const kilnlink_zone_context_t *z = &ctx->zones[i];
        out[o + 0] = z->zone_index;
        out[o + 1] = z->flags;
        kilnlink_put_f32le(out, o + 2, z->setpoint_c);
        kilnlink_put_f32le(out, o + 6, z->measured_c);
        out[o + 10] = z->sample_counter;
        out[o + 11] = z->tc_type;
        out[o + 12] = z->tc_fault;
        out[o + 13] = 0; /* reserved */
        o += KILNLINK_CONTEXT_ZONE_BLOCK_LEN;
    }

    return needed;
}

kilnlink_context_status_t kilnlink_context_decode(const uint8_t *payload, size_t len,
                                                  kilnlink_context_t *out)
{
    if (len < KILNLINK_CONTEXT_FIXED_LEN) {
        return KILNLINK_CONTEXT_ERR_TOO_SHORT;
    }
    if (payload[0] != KILNLINK_CONTEXT_CMD) {
        return KILNLINK_CONTEXT_ERR_WRONG_CMD;
    }

    uint8_t zone_count = payload[14];
    if (zone_count > KILNLINK_CONTEXT_MAX_ZONES) {
        return KILNLINK_CONTEXT_ERR_ZONE_COUNT;
    }
    if (len != context_wire_len(zone_count)) {
        return KILNLINK_CONTEXT_ERR_LENGTH_MISMATCH;
    }

    out->flags = payload[1];
    out->boot_id = payload[2];
    out->seq = kilnlink_get_u32le(payload, 3);
    out->uptime_ms = kilnlink_get_u32le(payload, 7);
    out->relay_now_mask = payload[11];
    out->relay_recent_mask = payload[12];
    out->recent_window_s = payload[13];
    out->zone_count = zone_count;

    size_t o = KILNLINK_CONTEXT_FIXED_LEN;
    for (uint8_t i = 0; i < zone_count; ++i) {
        kilnlink_zone_context_t *z = &out->zones[i];
        z->zone_index = payload[o + 0];
        z->flags = payload[o + 1];
        z->setpoint_c = kilnlink_get_f32le(payload, o + 2);
        z->measured_c = kilnlink_get_f32le(payload, o + 6);
        z->sample_counter = payload[o + 10];
        z->tc_type = payload[o + 11];
        z->tc_fault = payload[o + 12];
        o += KILNLINK_CONTEXT_ZONE_BLOCK_LEN;
    }

    return KILNLINK_CONTEXT_OK;
}
