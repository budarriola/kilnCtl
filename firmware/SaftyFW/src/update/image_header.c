// image_header.c -- see image_header.h.
#include "image_header.h"

#include <string.h>

static void put_u16_le(uint8_t *out, uint16_t v)
{
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32_le(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
    out[2] = (uint8_t)((v >> 16) & 0xFFu);
    out[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t get_u16_le(const uint8_t *in)
{
    return (uint16_t)(in[0] | ((uint16_t)in[1] << 8));
}

static uint32_t get_u32_le(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

void update_image_header_pack(const update_image_header_t *hdr,
                               uint8_t out[UPDATE_IMAGE_HEADER_WIRE_LEN])
{
    put_u32_le(&out[0], hdr->magic);
    out[4] = hdr->target;
    out[5] = hdr->header_version;
    put_u16_le(&out[6], hdr->protocol_version);
    put_u16_le(&out[8], hdr->min_compatible);
    out[10] = hdr->requested_slot;
    out[11] = hdr->flags;
    put_u32_le(&out[12], hdr->length);
    put_u32_le(&out[16], hdr->crc32);
    memcpy(&out[20], hdr->version, sizeof(hdr->version));
}

bool update_image_header_unpack(const uint8_t *payload, uint8_t length,
                                 update_image_header_t *out)
{
    if (payload == NULL || out == NULL) {
        return false;
    }
    if (length != UPDATE_IMAGE_HEADER_WIRE_LEN) {
        return false;
    }

    out->magic = get_u32_le(&payload[0]);
    out->target = payload[4];
    out->header_version = payload[5];
    out->protocol_version = get_u16_le(&payload[6]);
    out->min_compatible = get_u16_le(&payload[8]);
    out->requested_slot = payload[10];
    out->flags = payload[11];
    out->length = get_u32_le(&payload[12]);
    out->crc32 = get_u32_le(&payload[16]);
    memcpy(out->version, &payload[20], sizeof(out->version));
    return true;
}

update_image_header_check_t update_image_header_validate(const update_image_header_t *hdr,
                                                            uint32_t max_length)
{
    if (hdr->magic != UPDATE_IMAGE_HEADER_MAGIC) {
        return UPDATE_IMAGE_HEADER_BAD_MAGIC;
    }
    if (hdr->target != UPDATE_IMAGE_TARGET_RP2040) {
        return UPDATE_IMAGE_HEADER_BAD_TARGET;
    }
    if (hdr->header_version != UPDATE_IMAGE_HEADER_VERSION) {
        return UPDATE_IMAGE_HEADER_BAD_VERSION;
    }
    if (hdr->length == 0u) {
        return UPDATE_IMAGE_HEADER_ZERO_LENGTH;
    }
    if (hdr->length > max_length) {
        return UPDATE_IMAGE_HEADER_TOO_LARGE;
    }
    return UPDATE_IMAGE_HEADER_OK;
}
