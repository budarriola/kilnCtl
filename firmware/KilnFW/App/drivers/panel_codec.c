// panel_codec.c -- see panel_codec.h for what this is and why it exists.
#include "panel_codec.h"

void panel_codec_rgb565_to_rgb666(uint16_t color, uint8_t out[3])
{
    uint8_t r5 = (uint8_t)((color >> 11) & 0x1F);
    uint8_t g6 = (uint8_t)((color >> 5) & 0x3F);
    uint8_t b5 = (uint8_t)(color & 0x1F);
    out[0] = (uint8_t)((r5 << 3) | (r5 >> 2));
    out[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
    out[2] = (uint8_t)((b5 << 3) | (b5 >> 2));
}

void panel_codec_rgb565_passthrough(uint16_t color, uint8_t out[2])
{
    /* Same byte order ILI9488_blit_data reads its input in: u16
     * little-endian on the wire (src[0] | src[1] << 8). Kept as an explicit
     * split rather than a memcpy so the "no transformation happens here" is
     * visible at the call site, not merely true by accident of layout. */
    out[0] = (uint8_t)(color & 0xFF);
    out[1] = (uint8_t)(color >> 8);
}

void panel_codec_build_caset(uint16_t x, uint16_t w, uint8_t out[4])
{
    uint16_t x1 = (uint16_t)(x + w - 1);
    out[0] = (uint8_t)(x >> 8);
    out[1] = (uint8_t)x;
    out[2] = (uint8_t)(x1 >> 8);
    out[3] = (uint8_t)x1;
}

void panel_codec_build_paset(uint16_t y, uint16_t h, uint8_t out[4])
{
    uint16_t y1 = (uint16_t)(y + h - 1);
    out[0] = (uint8_t)(y >> 8);
    out[1] = (uint8_t)y;
    out[2] = (uint8_t)(y1 >> 8);
    out[3] = (uint8_t)y1;
}

uint8_t panel_codec_madctl(const uint8_t madctl_by_rotation[4], uint8_t rotation, uint8_t color_order_bit)
{
    return (uint8_t)(madctl_by_rotation[rotation & 0x03] | color_order_bit);
}

bool panel_codec_rotation_swaps_dimensions(uint8_t rotation)
{
    return (rotation & 0x01) != 0;
}

bool panel_codec_rect_in_bounds(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                 uint16_t bounds_w, uint16_t bounds_h)
{
    if (w == 0 || h == 0) return false;
    /* 32-bit arithmetic so x = 0xFFFF, w = 2 cannot wrap into range. */
    return ((uint32_t)x + w) <= bounds_w && ((uint32_t)y + h) <= bounds_h;
}

size_t panel_codec_chunk_pixels(size_t chunk_bytes, size_t scratch_bytes, size_t bytes_per_pixel)
{
    if (bytes_per_pixel == 0) return 0;
    size_t clamped = (chunk_bytes > scratch_bytes) ? scratch_bytes : chunk_bytes;
    return clamped / bytes_per_pixel;
}

bool panel_codec_init_step(const uint8_t *seq, size_t len, size_t *offset,
                            uint8_t *out_cmd, const uint8_t **out_params,
                            uint8_t *out_param_len)
{
    if (!seq || !offset || !out_cmd || !out_params || !out_param_len) return false;
    size_t i = *offset;
    if (i + 2 > len) return false; /* no cmd+len byte pair left (also end-of-buffer) */
    uint8_t cmd = seq[i];
    uint8_t plen = seq[i + 1];
    if (i + 2 + (size_t)plen > len) return false; /* declared params run past the buffer */

    *out_cmd = cmd;
    *out_params = (plen > 0) ? &seq[i + 2] : NULL;
    *out_param_len = plen;
    *offset = i + 2 + plen;
    return true;
}

bool panel_codec_blit_overruns(uint32_t pixels, uint32_t pixels_total, uint32_t pixels_done)
{
    /* pixels_done is never > pixels_total in the real driver, but guard the
     * subtraction anyway so a caller that got that invariant wrong sees an
     * overrun rather than a huge wrapped "remaining" count. */
    if (pixels_done >= pixels_total) return pixels > 0;
    uint32_t remaining = pixels_total - pixels_done;
    return pixels > remaining;
}
