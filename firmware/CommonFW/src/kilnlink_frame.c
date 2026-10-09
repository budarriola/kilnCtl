#include "kilnlink/kilnlink_frame.h"

size_t kilnlink_stuff(const uint8_t *raw, size_t raw_len, uint8_t *out, size_t out_cap)
{
    /* Worst case: every byte escapes (2x) plus two delimiters. Checked up
     * front so the loop below never has to bounds-check per byte. */
    if (out_cap < raw_len * 2u + 2u) {
        return 0;
    }

    size_t o = 0;
    out[o++] = KILNLINK_FRAME_DELIM;
    for (size_t i = 0; i < raw_len; ++i) {
        uint8_t b = raw[i];
        if (b == KILNLINK_FRAME_DELIM || b == KILNLINK_FRAME_ESC) {
            out[o++] = KILNLINK_FRAME_ESC;
            out[o++] = (uint8_t)(b ^ KILNLINK_FRAME_ESC_XOR);
        } else {
            out[o++] = b;
        }
    }
    out[o++] = KILNLINK_FRAME_DELIM;
    return o;
}

size_t kilnlink_unstuff(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                        kilnlink_frame_status_t *status)
{
    kilnlink_frame_status_t local_status = KILNLINK_FRAME_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_FRAME_OK;

    size_t start = 0;
    size_t end = in_len;
    if (in_len > 0 && in[0] == KILNLINK_FRAME_DELIM) {
        start = 1;
    }
    if (end > start && in[end - 1] == KILNLINK_FRAME_DELIM) {
        end -= 1;
    }

    if (out_cap < in_len) {
        *status = KILNLINK_FRAME_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    size_t o = 0;
    int escaped = 0;
    for (size_t i = start; i < end; ++i) {
        uint8_t b = in[i];
        if (escaped) {
            out[o++] = (uint8_t)(b ^ KILNLINK_FRAME_ESC_XOR);
            escaped = 0;
        } else if (b == KILNLINK_FRAME_ESC) {
            escaped = 1;
        } else {
            out[o++] = b;
        }
    }
    if (escaped) {
        /* Trailing 0x7D with nothing after it: an escape byte promises one
         * more byte that never arrived. Whatever was decoded before it is
         * discarded rather than returned half-trustworthy. */
        *status = KILNLINK_FRAME_ERR_UNTERMINATED_ESC;
        return 0;
    }
    return o;
}

size_t kilnlink_frame_encode_raw(const kilnlink_frame_t *frame, uint8_t *out_raw, size_t out_cap,
                                 kilnlink_frame_status_t *status)
{
    kilnlink_frame_status_t local_status = KILNLINK_FRAME_OK;
    if (!status) {
        status = &local_status;
    }
    *status = KILNLINK_FRAME_OK;

    if (frame->length > KILNLINK_FRAME_MAX_PAYLOAD) {
        *status = KILNLINK_FRAME_ERR_LENGTH_TOO_LONG;
        return 0;
    }
    size_t needed = KILNLINK_FRAME_HEADER_LEN + frame->length + KILNLINK_FRAME_CRC_LEN;
    if (out_cap < needed) {
        *status = KILNLINK_FRAME_ERR_BUFFER_TOO_SMALL;
        return 0;
    }

    out_raw[0] = (uint8_t)frame->msg_type;
    out_raw[1] = (uint8_t)(frame->msg_index >> 8);
    out_raw[2] = (uint8_t)(frame->msg_index & 0xFFu);
    out_raw[3] = frame->src_device;
    out_raw[4] = frame->src_task;
    out_raw[5] = frame->dst_device;
    out_raw[6] = frame->dst_task;
    out_raw[7] = frame->length;
    if (frame->length > 0) {
        /* No memcpy-of-a-struct anywhere here -- README.md rule 5 -- but a
         * byte-for-byte copy of an already-serialized payload buffer into
         * its wire position is not that; the payload's own field layout is
         * whatever kilnlink_context.c/kilnlink_status.c serialize by hand. */
        for (size_t i = 0; i < frame->length; ++i) {
            out_raw[KILNLINK_FRAME_HEADER_LEN + i] = frame->payload[i];
        }
    }

    uint16_t crc = kilnlink_crc16_ccitt_false(out_raw, KILNLINK_FRAME_HEADER_LEN + frame->length);
    out_raw[KILNLINK_FRAME_HEADER_LEN + frame->length] = (uint8_t)(crc >> 8);
    out_raw[KILNLINK_FRAME_HEADER_LEN + frame->length + 1] = (uint8_t)(crc & 0xFFu);

    return needed;
}

kilnlink_frame_status_t kilnlink_frame_decode(const uint8_t *raw, size_t raw_len,
                                              kilnlink_frame_t *out)
{
    if (raw_len < KILNLINK_FRAME_HEADER_LEN + KILNLINK_FRAME_CRC_LEN) {
        return KILNLINK_FRAME_ERR_TOO_SHORT;
    }

    uint8_t length = raw[7];
    /* Checked before the length/size comparison below so an over-long
     * LENGTH byte is rejected on its own terms, with its own error code,
     * rather than folded into a generic mismatch. */
    if (length > KILNLINK_FRAME_MAX_PAYLOAD) {
        return KILNLINK_FRAME_ERR_LENGTH_TOO_LONG;
    }
    if (raw_len != (size_t)KILNLINK_FRAME_HEADER_LEN + length + KILNLINK_FRAME_CRC_LEN) {
        return KILNLINK_FRAME_ERR_LENGTH_MISMATCH;
    }

    uint16_t expected = kilnlink_crc16_ccitt_false(raw, KILNLINK_FRAME_HEADER_LEN + length);
    uint16_t actual = (uint16_t)((raw[KILNLINK_FRAME_HEADER_LEN + length] << 8) |
                                 raw[KILNLINK_FRAME_HEADER_LEN + length + 1]);
    if (expected != actual) {
        return KILNLINK_FRAME_ERR_CRC;
    }

    uint8_t type_byte = raw[0];
    if (type_byte != KILNLINK_MSG_DATA && type_byte != KILNLINK_MSG_ACK &&
        type_byte != KILNLINK_MSG_NACK && type_byte != KILNLINK_MSG_BROADCAST) {
        return KILNLINK_FRAME_ERR_UNKNOWN_TYPE;
    }

    out->msg_type = (kilnlink_msg_type_t)type_byte;
    out->msg_index = (uint16_t)((raw[1] << 8) | raw[2]);
    out->src_device = raw[3];
    out->src_task = raw[4];
    out->dst_device = raw[5];
    out->dst_task = raw[6];
    out->length = length;
    out->payload = raw + KILNLINK_FRAME_HEADER_LEN;
    return KILNLINK_FRAME_OK;
}
