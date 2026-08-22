#include "kilnlink/kilnlink_param_value.h"

#include "kilnlink/kilnlink_bytes.h"

size_t kilnlink_param_value_len(uint8_t type)
{
    switch (type) {
        case KILNLINK_PARAM_TYPE_BOOL: return 1u;
        case KILNLINK_PARAM_TYPE_U8:   return 1u;
        case KILNLINK_PARAM_TYPE_U16:  return 2u;
        case KILNLINK_PARAM_TYPE_F32:  return 4u;
        default: return 0u; /* not a recognised tag */
    }
}

bool kilnlink_param_value_encode(uint8_t type, const kilnlink_param_value_t *value, uint8_t *out,
                                  size_t off)
{
    switch (type) {
        case KILNLINK_PARAM_TYPE_BOOL:
            out[off] = (value->bool_val != 0) ? 1u : 0u;
            return true;
        case KILNLINK_PARAM_TYPE_U8:
            out[off] = value->u8_val;
            return true;
        case KILNLINK_PARAM_TYPE_U16:
            kilnlink_put_u16le(out, off, value->u16_val);
            return true;
        case KILNLINK_PARAM_TYPE_F32:
            kilnlink_put_f32le(out, off, value->f32_val);
            return true;
        default:
            return false; /* bad type: write nothing */
    }
}

bool kilnlink_param_value_decode(uint8_t type, const uint8_t *in, size_t off,
                                   kilnlink_param_value_t *value)
{
    switch (type) {
        case KILNLINK_PARAM_TYPE_BOOL:
            /* A bool that arrived as anything but 0/1 is still a decode
             * ERROR, not a silent clamp -- callers check this explicitly
             * rather than kilnlink_param_value_decode() rejecting it here,
             * because "value" and "type" validity are different questions
             * (this function only proves the TYPE is one it knows how to
             * read; range-checking the value it read is the caller's job,
             * same split kilnlink_set_config.c documents for tc_type). */
            value->bool_val = in[off];
            return true;
        case KILNLINK_PARAM_TYPE_U8:
            value->u8_val = in[off];
            return true;
        case KILNLINK_PARAM_TYPE_U16:
            value->u16_val = kilnlink_get_u16le(in, off);
            return true;
        case KILNLINK_PARAM_TYPE_F32:
            value->f32_val = kilnlink_get_f32le(in, off);
            return true;
        default:
            return false; /* bad type: *value untouched */
    }
}
