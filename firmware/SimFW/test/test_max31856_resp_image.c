// Host tests for src/sim/max31856_resp_image.{c,h} -- the DMA-streamed
// response image that IS the MISO byte stream under Plan B
// (docs/SPI_ACCESS_AUDIT.md section 6).
//
// WHY THIS FILE EXISTS AT ALL. Three bugs have been found in this subsystem,
// all the same species: the .pio comments asserted a contract the C glue did
// not deliver, and no host test covered the glue. The PIO/DMA half genuinely
// cannot be host-tested -- there is no fixture hardware and no RP2040 in this
// environment. So the response path was deliberately split so that the part
// that CAN be tested is: what bytes end up on the wire, in what order, for
// which transaction. The cross-check in test_matches_reference_model_for_real_shapes()
// runs every one of the six real master transaction shapes through BOTH the
// reference register machine (max31856_regs_read_burst, the thing the old
// ISR path used) and the image+index arithmetic the hardware uses, and
// demands they agree byte-for-byte. Any future change to shift directions,
// justification, addressing or the auto-increment wrap that breaks one and
// not the other fails here rather than on a Saleae trace.
//
// The layout constants are asserted explicitly too, because they are the
// numbers the PIO program and the DMA configuration hard-code (a >> 10 in the
// PIO, a ring size of 9 in the DMA) and nothing else would notice them
// drifting.
#include <stdint.h>
#include <string.h>

#include "max31856_regs.h"
#include "max31856_resp_image.h"

#include "test_common.h"

/* Same alignment the real owner tasks must declare. Also lets the test assert
 * the macro actually does something. */
static MAX31856_RESP_IMAGE_ALIGN max31856_resp_image_t s_img;
static MAX31856_RESP_IMAGE_ALIGN max31856_resp_image_t s_img2;

static void seed_channel(max31856_channel_t *ch)
{
    max31856_regs_init(ch, 0x1234u);
    /* Something recognisable in every writable register, so a byte landing
     * one position early or late is visible rather than plausible. */
    uint8_t cfg[] = { 0x80u, 0x03u, 0x00u, 0x7Fu, 0xC0u, 0x11u, 0x22u, 0x33u, 0x44u, 0x05u };
    max31856_regs_write_burst(ch, MAX31856_REG_CR0, cfg, sizeof(cfg));
    max31856_regs_advance_conversion(ch, 812.5f, 26.0f);
}

static void test_layout_constants(void)
{
    TEST_SECTION("resp image -- layout constants the PIO and DMA hard-code");

    /* The PIO builds the DMA read pointer as (base >> 10) << 10 | addr*4.
     * That only works if one image is exactly 1024 bytes and 1024-aligned. */
    TEST_CHECK(MAX31856_RESP_IMAGE_BYTES == 1024u,
               "whole image must be 1024 bytes -- the PIO's >>10 base publish depends on it");
    TEST_CHECK(sizeof(max31856_resp_image_t) == 1024u,
               "sizeof must match MAX31856_RESP_IMAGE_BYTES");
    TEST_CHECK(((uintptr_t)&s_img & (MAX31856_RESP_IMAGE_BYTES - 1u)) == 0u,
               "MAX31856_RESP_IMAGE_ALIGN must actually 1024-align an instance");
    TEST_CHECK(((uintptr_t)&s_img2 & (MAX31856_RESP_IMAGE_BYTES - 1u)) == 0u,
               "second instance aligned too (arrays of images must stay aligned)");

    /* The DMA read ring is 9 bits == 512 bytes == one half. */
    TEST_CHECK(MAX31856_RESP_IMAGE_HALF_BYTES == 512u,
               "one half must be 512 bytes -- that is the DMA ring size of 9");
    TEST_CHECK(MAX31856_RESP_IMAGE_HALF == MAX31856_ADDR_SPACE,
               "one half must cover exactly the part's 7-bit address space");
    TEST_CHECK(MAX31856_RESP_IMAGE_ENTRIES == 256u,
               "both halves must cover every value the raw first byte can take");

    /* OUT shifts left and emits OSR bit 31, so the byte lives in bits[31:24]. */
    TEST_CHECK(MAX31856_RESP_IMAGE_BYTE_SHIFT == 24u,
               "left-justification must match the .pio OUT shift-left contract");
}

static void test_init_is_all_invalid(void)
{
    TEST_SECTION("resp image -- init fills the whole image with FFh");

    memset(&s_img, 0x5Au, sizeof(s_img));
    max31856_resp_image_init(&s_img);

    int bad = 0;
    for (uint16_t i = 0; i < MAX31856_RESP_IMAGE_ENTRIES; i++) {
        if (s_img.word[i] != ((uint32_t)MAX31856_INVALID_ADDR_VALUE << MAX31856_RESP_IMAGE_BYTE_SHIFT)) {
            bad++;
        }
    }
    TEST_CHECK(bad == 0, "every entry is FFh left-justified before the first publish");
}

static void test_publish_left_justifies_and_mirrors(void)
{
    TEST_SECTION("resp image -- publish: registers, FFh fill, mirror half, justification");

    max31856_channel_t ch;
    seed_channel(&ch);
    max31856_resp_image_init(&s_img);
    max31856_resp_image_publish(&s_img, &ch);

    int reg_mismatch = 0;
    int justify_mismatch = 0;
    for (uint8_t a = 0; a < MAX31856_REG_COUNT; a++) {
        if (max31856_resp_image_byte(&s_img, a) != ch.regs[a]) {
            reg_mismatch++;
        }
        if (s_img.word[a] != ((uint32_t)ch.regs[a] << 24)) {
            justify_mismatch++;
        }
    }
    TEST_CHECK(reg_mismatch == 0, "00h..0Fh read back the live register file");
    TEST_CHECK(justify_mismatch == 0, "each entry is exactly byte << 24, nothing in the low 24 bits");

    int fill_mismatch = 0;
    for (uint16_t a = MAX31856_REG_COUNT; a < MAX31856_RESP_IMAGE_HALF; a++) {
        if (max31856_resp_image_byte(&s_img, a) != MAX31856_INVALID_ADDR_VALUE) {
            fill_mismatch++;
        }
    }
    TEST_CHECK(fill_mismatch == 0,
               "10h..7Fh report FFh -- datasheet p15 'Invalid memory addresses report an FFh value'");

    int mirror_mismatch = 0;
    for (uint16_t a = 0; a < MAX31856_RESP_IMAGE_HALF; a++) {
        if (s_img.word[MAX31856_RESP_IMAGE_HALF + a] != s_img.word[a]) {
            mirror_mismatch++;
        }
    }
    TEST_CHECK(mirror_mismatch == 0,
               "the write half (80h..FFh) is a verbatim mirror, so a write transaction's don't-care MISO is defined");
}

/* The heart of the file: the hardware path and the reference model must agree
 * byte-for-byte on every transaction shape either real master emits
 * (SPI_ACCESS_AUDIT.md section 1's inventory). */
static void check_shape(const char *name, uint8_t addr_byte, uint16_t len)
{
    max31856_channel_t model;
    max31856_channel_t imaged;
    seed_channel(&model);
    seed_channel(&imaged); /* identical seed, so identical registers */

    max31856_resp_image_init(&s_img);
    max31856_resp_image_publish(&s_img, &imaged);

    uint8_t reference[64];
    max31856_regs_read_burst(&model, addr_byte, reference, len);

    int mismatch = 0;
    for (uint16_t k = 0; k < len; k++) {
        uint16_t idx = max31856_resp_image_index(addr_byte, k);
        if (max31856_resp_image_byte(&s_img, idx) != reference[k]) {
            mismatch++;
        }
    }
    TEST_CHECK(mismatch == 0, name);
}

static void test_matches_reference_model_for_real_shapes(void)
{
    TEST_SECTION("resp image -- wire bytes match the reference register machine, every real shape");

    /* R1: both masters' temperature burst, 0Ah..0Fh, six response bytes. */
    check_shape("R1 (0Ah, 6 bytes: CJTH CJTL LTCBH LTCBM LTCBL SR) matches the model", MAX31856_REG_CJTH, 6);
    /* R2: KilnFW's SR-only poll. */
    check_shape("R2 (0Fh, 1 byte: SR) matches the model", MAX31856_REG_SR, 1);
    /* R3: KilnFW's debug burst, the widest legal one. */
    check_shape("R3 (00h, 16 bytes: the whole register file) matches the model", MAX31856_REG_CR0, 16);
    check_shape("R3 (05h, 8 bytes: a mid-file burst) matches the model", MAX31856_REG_LTHFTH, 8);
    /* Past the implemented registers, into the FFh region. */
    check_shape("read running off 0Fh into the FFh region matches the model", 0x0Du, 12);
}

/* Write shapes are checked differently and deliberately so: the reference
 * model returns 0x00 for a read clocked inside a WRITE transaction (it
 * refuses the wrong-direction byte), whereas the hardware has no direction
 * concept on the response path at all -- the DMA streams the mirror half.
 * Neither master looks at those bytes (SPI_ACCESS_AUDIT.md section 1: "both
 * masters explicitly discard it"), so what matters is only that the stream is
 * DEFINED and stays inside the mirror half. */
static void test_write_shapes_stream_the_mirror(void)
{
    TEST_SECTION("resp image -- write transactions stream the mirror half, defined and self-contained");

    max31856_channel_t ch;
    seed_channel(&ch);
    max31856_resp_image_init(&s_img);
    max31856_resp_image_publish(&s_img, &ch);

    const uint8_t regs[] = { MAX31856_REG_CR0, MAX31856_REG_CJHF, MAX31856_REG_SR };
    const uint16_t lens[] = { 1u, 2u, 16u };
    int mismatch = 0;
    for (size_t s = 0; s < sizeof(regs); s++) {
        for (uint16_t k = 0; k < lens[s]; k++) {
            uint16_t widx = max31856_resp_image_index(MAX31856_WRITE_ADDR(regs[s]), k);
            uint16_t ridx = max31856_resp_image_index(regs[s], k);
            if (widx < MAX31856_RESP_IMAGE_HALF) {
                mismatch++;
            }
            if (max31856_resp_image_byte(&s_img, widx) != max31856_resp_image_byte(&s_img, ridx)) {
                mismatch++;
            }
        }
    }
    TEST_CHECK(mismatch == 0,
               "W1/W2/W3 stream the same bytes the equivalent read would, from the mirror half");
}

static void test_auto_increment_wrap_matches_the_part(void)
{
    TEST_SECTION("resp image -- the DMA read ring reproduces the part's 7Fh -> 00h wrap");

    /* The datasheet (p15): "the address will loop from 7Fh/FFh to 00h/80h".
     * In the hardware that is a 512-byte DMA read ring, not code, so the
     * index arithmetic is what has to be checked. */
    TEST_CHECK(max31856_resp_image_index(0x7Eu, 0) == 0x7Eu, "read burst starts where the address byte says");
    TEST_CHECK(max31856_resp_image_index(0x7Eu, 1) == 0x7Fu, "and increments");
    TEST_CHECK(max31856_resp_image_index(0x7Eu, 2) == 0x00u, "7Fh wraps to 00h, not into the write half");
    TEST_CHECK(max31856_resp_image_index(0x7Eu, 3) == 0x01u, "and keeps going from CR0");
    TEST_CHECK(max31856_resp_image_index(0x7Eu, 0x82u) == 0x00u, "a wrap of more than one lap still lands right");

    /* A write transaction rings inside the mirror half and can never fall
     * into the read half -- checked because the failure would be silent. */
    TEST_CHECK(max31856_resp_image_index(0xFEu, 2) == 0x80u, "FFh wraps to 80h, staying in the write half");
    int escaped = 0;
    for (uint16_t k = 0; k < 300u; k++) {
        if (max31856_resp_image_index(0x8Au, k) < MAX31856_RESP_IMAGE_HALF) {
            escaped++;
        }
        if (max31856_resp_image_index(0x0Au, k) >= MAX31856_RESP_IMAGE_HALF) {
            escaped++;
        }
    }
    TEST_CHECK(escaped == 0, "no burst of any length ever crosses between the read half and the write half");

    /* And the wrap must agree with the reference model, not just with
     * itself: an 8-byte read from 7Eh. */
    check_shape("a burst that wraps 7Fh -> 00h matches the model byte-for-byte", 0x7Eu, 8);
}

static void test_dead_mode_covers_the_whole_half(void)
{
    TEST_SECTION("resp image -- dead-channel modes force every address, not just the 16 real ones");

    max31856_channel_t ch;
    seed_channel(&ch);
    ch.corruption.dead_mode = MAX31856_DEAD_ALL_ZERO;
    max31856_resp_image_init(&s_img);
    max31856_resp_image_publish(&s_img, &ch);

    int bad = 0;
    for (uint16_t a = 0; a < MAX31856_RESP_IMAGE_ENTRIES; a++) {
        if (max31856_resp_image_byte(&s_img, a) != 0x00u) {
            bad++;
        }
    }
    TEST_CHECK(bad == 0, "ALL_ZERO forces 00h across both halves, including 10h..7Fh");

    ch.corruption.dead_mode = MAX31856_DEAD_HIGH_Z;
    max31856_resp_image_publish(&s_img, &ch);
    bad = 0;
    for (uint16_t a = 0; a < MAX31856_RESP_IMAGE_ENTRIES; a++) {
        if (max31856_resp_image_byte(&s_img, a) != 0xFFu) {
            bad++;
        }
    }
    TEST_CHECK(bad == 0, "HIGH_Z/ALL_ONE force FFh across both halves");

    /* And a dead channel's wire bytes still match the reference model. */
    max31856_channel_t model;
    seed_channel(&model);
    model.corruption.dead_mode = MAX31856_DEAD_HIGH_Z;
    uint8_t reference[6];
    max31856_regs_read_burst(&model, MAX31856_REG_CJTH, reference, sizeof(reference));
    int mismatch = 0;
    for (uint16_t k = 0; k < sizeof(reference); k++) {
        if (max31856_resp_image_byte(&s_img, max31856_resp_image_index(MAX31856_REG_CJTH, (uint16_t)k)) != reference[k]) {
            mismatch++;
        }
    }
    TEST_CHECK(mismatch == 0, "a dead channel's R1 burst matches the model too");
}

static void test_publish_is_a_snapshot_not_a_view(void)
{
    TEST_SECTION("resp image -- an unpublished register change cannot reach the wire");

    /* This is the coherency property the double-banked publish exists to give:
     * the hardware reads the image, never the register file, so a mid-flight
     * model update is invisible until the owner task publishes it into the
     * OTHER bank. Stated as a test so nobody "optimises" the image away into
     * a pointer at ch->regs. */
    max31856_channel_t ch;
    seed_channel(&ch);
    max31856_resp_image_init(&s_img);
    max31856_resp_image_publish(&s_img, &ch);
    uint8_t before = max31856_resp_image_byte(&s_img, MAX31856_REG_LTCBH);

    ch.regs[MAX31856_REG_LTCBH] = (uint8_t)(before ^ 0xFFu);
    TEST_CHECK(max31856_resp_image_byte(&s_img, MAX31856_REG_LTCBH) == before,
               "the image does not track the register file between publishes");

    max31856_resp_image_publish(&s_img2, &ch);
    TEST_CHECK(max31856_resp_image_byte(&s_img2, MAX31856_REG_LTCBH) == (uint8_t)(before ^ 0xFFu),
               "and a publish into the other bank picks the change up");
    TEST_CHECK(max31856_resp_image_byte(&s_img, MAX31856_REG_LTCBH) == before,
               "without disturbing the bank the hardware may still be reading");
}

static void test_null_safety(void)
{
    TEST_SECTION("resp image -- null/out-of-range arguments do not crash");

    max31856_channel_t ch;
    seed_channel(&ch);
    max31856_resp_image_init(NULL);
    max31856_resp_image_publish(NULL, &ch);
    max31856_resp_image_publish(&s_img, NULL);
    TEST_CHECK(max31856_resp_image_byte(NULL, 0) == MAX31856_INVALID_ADDR_VALUE,
               "a null image reads as the invalid-address value");
    TEST_CHECK(max31856_resp_image_byte(&s_img, MAX31856_RESP_IMAGE_ENTRIES) == MAX31856_INVALID_ADDR_VALUE,
               "an out-of-range index reads as the invalid-address value");
}

void run_test_max31856_resp_image(void)
{
    test_layout_constants();
    test_init_is_all_invalid();
    test_publish_left_justifies_and_mirrors();
    test_matches_reference_model_for_real_shapes();
    test_write_shapes_stream_the_mirror();
    test_auto_increment_wrap_matches_the_part();
    test_dead_mode_covers_the_whole_half();
    test_publish_is_a_snapshot_not_a_view();
    test_null_safety();
}
