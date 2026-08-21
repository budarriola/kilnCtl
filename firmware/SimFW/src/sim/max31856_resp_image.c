// max31856_resp_image.c -- see max31856_resp_image.h for the layout contract
// and why the CPU is not in the read path at all.
#include "max31856_resp_image.h"

static uint32_t entry(uint8_t value)
{
    return ((uint32_t)value) << MAX31856_RESP_IMAGE_BYTE_SHIFT;
}

void max31856_resp_image_init(max31856_resp_image_t *img)
{
    if (!img) {
        return;
    }
    uint32_t invalid = entry((uint8_t)MAX31856_INVALID_ADDR_VALUE);
    for (uint16_t i = 0; i < MAX31856_RESP_IMAGE_ENTRIES; i++) {
        img->word[i] = invalid;
    }
}

void max31856_resp_image_publish(max31856_resp_image_t *img, max31856_channel_t *ch)
{
    if (!img || !ch) {
        return;
    }

    /* Only the 16 implemented registers can ever change; 10h..7Fh are FFh
     * for the life of the image (max31856_resp_image_init() put them there)
     * UNLESS a dead-channel mode is forcing every byte, which is exactly the
     * case that has to overwrite them too. Splitting the loop this way keeps
     * the common publish at 32 stores -- cheap enough that the owner task can
     * do it every 20 ms scan without the cost being worth thinking about. */
    uint16_t span = (ch->corruption.dead_mode != MAX31856_DEAD_NONE)
                        ? MAX31856_RESP_IMAGE_HALF
                        : MAX31856_REG_COUNT;

    for (uint16_t addr = 0; addr < span; addr++) {
        uint8_t raw = max31856_regs_raw_read_value(ch->regs, (uint8_t)addr);
        uint32_t w = entry(max31856_regs_apply_read_corruption(ch, raw));
        img->word[addr] = w;
        /* The write-space mirror -- see the header's LAYOUT CONTRACT. Same
         * value, so a write transaction's don't-care MISO stream looks like
         * register data instead of a second, differently-corrupted image. */
        img->word[MAX31856_RESP_IMAGE_HALF + addr] = w;
    }
}

uint8_t max31856_resp_image_byte(const max31856_resp_image_t *img, uint16_t index)
{
    if (!img || index >= MAX31856_RESP_IMAGE_ENTRIES) {
        return (uint8_t)MAX31856_INVALID_ADDR_VALUE;
    }
    return (uint8_t)((img->word[index] >> MAX31856_RESP_IMAGE_BYTE_SHIFT) & 0xFFu);
}

uint16_t max31856_resp_image_index(uint8_t addr_byte, uint16_t byte_offset)
{
    /* The DMA data channel increments its read address by one entry per byte
     * and wraps inside a 512-byte (= 128-entry) ring, which is the half the
     * first byte landed in. That is the whole of the hardware's addressing
     * behaviour, and it reproduces the part's own 7-bit auto-increment wrap
     * ("the address will loop from 7Fh/FFh to 00h/80h", datasheet page 15)
     * for free -- including keeping a write transaction inside the mirror
     * half rather than falling into the read half. */
    uint16_t half_base = (uint16_t)(addr_byte & MAX31856_RESP_IMAGE_HALF);
    uint16_t within = (uint16_t)(((addr_byte & (MAX31856_RESP_IMAGE_HALF - 1u)) + byte_offset) &
                                 (MAX31856_RESP_IMAGE_HALF - 1u));
    return (uint16_t)(half_base + within);
}
