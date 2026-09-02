// Host tests for the ST7796 panel_desc_t (../drivers/st7796_panel.c) and for
// the packed init-sequence decoder it and panel_spi.c's ILI9488 descriptor
// share (panel_codec_init_step(), ../drivers/panel_codec.c).
// DISPLAY_ST7796_PLAN.md Sec.12 Phase 3.
//
// panel_spi.c (the ILI9488.c rename) is NOT host-tested here or anywhere --
// it pulls in driver/spi_master.h, esp_spi_owner.h and FreeRTOS, the same
// reason ILI9488.c never had a host test file. st7796_panel.c/.h and
// panel_codec.c/.h have no such dependency, so what IS host-tested here is
// everything that can be: the descriptor's metadata, an exact byte-for-byte
// check of the transcribed vendor init table, and the decoder those bytes
// are meant to be read back with -- including the failure paths a malformed
// table would hit, which nothing else exercises since neither real
// descriptor is malformed.
#include "test_common.h"
#include "../drivers/panel_codec.h"
#include "../drivers/st7796_panel.h"

#include <string.h>

/* --- panel_codec_init_step(): decoder correctness and failure paths ------ */
static void test_init_step_decoder(void)
{
    TEST_SECTION("panel_codec_init_step");

    /* A well-formed two-step buffer: cmd 0xAA with 2 params, then cmd 0xBB
     * with 0 params, decodes to exactly those two steps and then reports
     * "no more" at the buffer's end (not a failure -- offset == len is the
     * normal loop-termination signal, same thing ili9488_run_init_sequence()
     * relies on). */
    {
        static const uint8_t buf[] = { 0xAA, 2, 0x01, 0x02, 0xBB, 0, };
        size_t offset = 0;
        uint8_t cmd, plen;
        const uint8_t *params;

        TEST_CHECK(panel_codec_init_step(buf, sizeof(buf), &offset, &cmd, &params, &plen) == true,
                   "init_step: first step of a well-formed buffer decodes");
        TEST_CHECK(cmd == 0xAA && plen == 2 && params[0] == 0x01 && params[1] == 0x02,
                   "init_step: first step cmd/params match the buffer");
        TEST_CHECK(offset == 4, "init_step: offset advances past cmd+len+2 params");

        TEST_CHECK(panel_codec_init_step(buf, sizeof(buf), &offset, &cmd, &params, &plen) == true,
                   "init_step: second step decodes");
        TEST_CHECK(cmd == 0xBB && plen == 0 && params == NULL,
                   "init_step: zero-param step has NULL params, not a dangling pointer");
        TEST_CHECK(offset == sizeof(buf), "init_step: offset reaches the buffer end exactly");

        TEST_CHECK(panel_codec_init_step(buf, sizeof(buf), &offset, &cmd, &params, &plen) == false,
                   "init_step: no third step -- returns false at the buffer end, not a crash");
        TEST_CHECK(offset == sizeof(buf), "init_step: a false return leaves offset unchanged");
    }

    /* NEGATIVE TEST 1: a lone trailing cmd byte with no length byte after it
     * (a truncated table) must fail, not read one byte past the buffer to
     * manufacture a paramLen. This is the failure panel_codec_init_step()
     * exists to catch that a naive "always read 2 header bytes" decoder
     * would not: proves the "fewer than 2 bytes remain" branch can fire. */
    {
        static const uint8_t buf[] = { 0xAA, 2, 0x01, 0x02, 0xCC };
        size_t offset = 4; /* positioned right at the dangling 0xCC */
        uint8_t cmd, plen;
        const uint8_t *params;
        TEST_CHECK(panel_codec_init_step(buf, sizeof(buf), &offset, &cmd, &params, &plen) == false,
                   "init_step NEGATIVE: a lone trailing cmd byte (no length byte) fails, not OOB read");
        TEST_CHECK(offset == 4, "init_step NEGATIVE: offset is untouched on failure");
    }

    /* NEGATIVE TEST 2: a declared paramLen that runs past the buffer must
     * fail rather than read (or let a caller read) past seq[len-1]. This is
     * the case that matters most: a corrupted or hand-edited init table with
     * a wrong length byte must not become an out-of-bounds SPI transfer. */
    {
        static const uint8_t buf[] = { 0xAA, 5, 0x01, 0x02 }; /* claims 5 params, only 2 present */
        size_t offset = 0;
        uint8_t cmd, plen;
        const uint8_t *params;
        TEST_CHECK(panel_codec_init_step(buf, sizeof(buf), &offset, &cmd, &params, &plen) == false,
                   "init_step NEGATIVE: paramLen running past the buffer fails");
        TEST_CHECK(offset == 0, "init_step NEGATIVE: offset is untouched, not partially advanced");
    }

    /* NEGATIVE TEST 3, proving the checks above are actually load-bearing and
     * not vacuously true: an empty buffer must fail on the FIRST call (there
     * is no possible step), which also doubles as the real driver's
     * "descriptor with init_len == 0" case if ever reached with a bad
     * descriptor (ILI9488_init rejects that before this is ever called, but
     * the decoder itself must still be safe standalone). */
    {
        size_t offset = 0;
        uint8_t cmd, plen;
        const uint8_t *params;
        TEST_CHECK(panel_codec_init_step(NULL, 0, &offset, &cmd, &params, &plen) == false,
                   "init_step NEGATIVE: zero-length buffer has no first step");
    }
}

/* --- The ST7796 descriptor's metadata ------------------------------------- */
static void test_st7796_descriptor_metadata(void)
{
    TEST_SECTION("ST7796_get_panel_desc: metadata");

    const panel_desc_t *panel = ST7796_get_panel_desc();
    TEST_CHECK(panel != NULL, "ST7796_get_panel_desc: never NULL");
    if (!panel) return;

    TEST_CHECK(panel == ST7796_get_panel_desc(), "ST7796_get_panel_desc: same static instance every call");
    TEST_CHECK(panel->name != NULL && strcmp(panel->name, "ST7796") == 0,
               "ST7796 descriptor: name is \"ST7796\"");
    TEST_CHECK(panel->panel_width == 320 && panel->panel_height == 480,
               "ST7796 descriptor: native geometry is 320x480, same as the ILI9488 (Sec.6 Step 4 invariant)");
    TEST_CHECK(panel->colmod == 0x55, "ST7796 descriptor: colmod metadata is 0x55 as directed");
    TEST_CHECK(panel->bytes_per_pixel == 2,
               "ST7796 descriptor: 2 bytes/pixel (COLMOD 0x55, RGB565) -- this is what selects "
               "panel_spi.c's fast blit path");
    TEST_CHECK(panel->init_seq != NULL && panel->init_len > 0,
               "ST7796 descriptor: init_seq is populated, not NULL/0 like Phase 2's ILI9488 placeholder");
    TEST_CHECK(panel->id_matches == NULL,
               "ST7796 descriptor: id_matches is NULL -- blocked on Sec.4's bench RDDID bytes (Phase 4)");
    TEST_CHECK(panel->blank_via_power_off == false,
               "ST7796 descriptor: blank_via_power_off defaults to the safe (ILI9488-matching) choice, "
               "pending Sec.4 bench confirmation of the DISPOFF blank color");

    /* MADCTL table: same MX/MV/MY bit pattern as the ILI9488's own table
     * (both implement the same MIPI MADCTL bit positions) -- verified
     * against the vendor's LCD_direction() derivation in st7796_panel.c's
     * own comment, not re-derived here. */
    const uint8_t expect[4] = { 0x40, 0x20, 0x80, 0xE0 };
    TEST_CHECK(memcmp(panel->madctl, expect, sizeof(expect)) == 0,
               "ST7796 descriptor: madctl[] matches the vendor LCD_direction() MX/MV/MY bits");
}

/* --- Exact transcription of ST7796_Init.txt ------------------------------- */
typedef struct {
    uint8_t cmd;
    uint8_t len;
    const uint8_t *params;
} expected_step_t;

static void test_st7796_init_table_transcription(void)
{
    TEST_SECTION("ST7796 init table: exact transcription of ST7796_Init.txt");

    static const uint8_t e0_params[] = { 0xF0, 0x09, 0x13, 0x12, 0x12, 0x2B, 0x3C, 0x44,
                                          0x4B, 0x1B, 0x18, 0x17, 0x1D, 0x21 };
    static const uint8_t e1_params[] = { 0xF0, 0x09, 0x13, 0x0C, 0x0D, 0x27, 0x3B, 0x44,
                                          0x4D, 0x0B, 0x17, 0x17, 0x1D, 0x21 };
    static const uint8_t e8_params[] = { 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33 };
    static const uint8_t b5_params[] = { 0x02, 0x03, 0x00, 0x04 };
    static const uint8_t b6_params[] = { 0x00, 0x02 };
    static const uint8_t b1_params[] = { 0x80, 0x10 };
    static const uint8_t one[] = { 0 }; /* placeholder, indexed [0] only for 1-byte steps below */

    /* One expected (cmd, len, first-and-last param) tuple per vendor
     * LCD_WR_REG()/LCD_WR_DATA() group, in the vendor file's own order --
     * this is the "22 steps, none reordered, none dropped" check. Full
     * param arrays are checked below for the multi-byte gamma/power steps
     * where a truncated check would hide a transcription slip; the 1-byte
     * steps are checked by value directly since a 22-entry array of
     * 1-element arrays would be noise for no extra coverage. */
    const expected_step_t expected[] = {
        { 0xF0, 1, NULL }, { 0xF0, 1, NULL }, { 0x36, 1, NULL }, { 0x3A, 1, NULL },
        { 0xB0, 1, NULL }, { 0xB6, 2, b6_params }, { 0xB5, 4, b5_params }, { 0xB1, 2, b1_params },
        { 0xB4, 1, NULL }, { 0xB7, 1, NULL }, { 0xC5, 1, NULL }, { 0xE4, 1, NULL },
        { 0xE8, 8, e8_params }, { 0xC2, 0, NULL }, { 0xA7, 0, NULL },
        { 0xE0, 14, e0_params }, { 0xE1, 14, e1_params },
        { 0xF0, 1, NULL }, { 0xF0, 1, NULL }, { 0x13, 0, NULL }, { 0x11, 0, NULL }, { 0x29, 0, NULL },
    };
    /* The single-byte params, in the same order as `expected` above (index
     * -1 for multi-byte/zero-byte steps, where this array's entry is
     * unused). */
    const uint8_t single_byte_params[] = {
        0xC3, 0x96, 0x48, 0x05, 0x80, 0, 0, 0, 0x00, 0xC6, 0x1C, 0x31,
        0, 0, 0, 0, 0, 0x3C, 0x69, 0, 0, 0,
    };
    (void)one;

    const panel_desc_t *panel = ST7796_get_panel_desc();
    size_t offset = 0;
    size_t n = sizeof(expected) / sizeof(expected[0]);
    for (size_t i = 0; i < n; ++i) {
        uint8_t cmd, plen;
        const uint8_t *params;
        bool ok = panel_codec_init_step(panel->init_seq, panel->init_len, &offset,
                                         &cmd, &params, &plen);
        char msg[96];
        snprintf(msg, sizeof(msg), "ST7796 init step %u decodes (cmd 0x%02X expected)",
                 (unsigned)i, expected[i].cmd);
        TEST_CHECK(ok, msg);
        if (!ok) break;

        snprintf(msg, sizeof(msg), "ST7796 init step %u: cmd == 0x%02X", (unsigned)i, expected[i].cmd);
        TEST_CHECK(cmd == expected[i].cmd, msg);
        snprintf(msg, sizeof(msg), "ST7796 init step %u: paramLen == %u", (unsigned)i, expected[i].len);
        TEST_CHECK(plen == expected[i].len, msg);

        if (expected[i].params) {
            snprintf(msg, sizeof(msg), "ST7796 init step %u: full param bytes match ST7796_Init.txt",
                     (unsigned)i);
            TEST_CHECK(plen == expected[i].len && params &&
                           memcmp(params, expected[i].params, expected[i].len) == 0,
                       msg);
        } else if (expected[i].len == 1) {
            snprintf(msg, sizeof(msg), "ST7796 init step %u: single param byte matches ST7796_Init.txt",
                     (unsigned)i);
            TEST_CHECK(params && params[0] == single_byte_params[i], msg);
        } else {
            snprintf(msg, sizeof(msg), "ST7796 init step %u: zero-param command has NULL params",
                     (unsigned)i);
            TEST_CHECK(params == NULL, msg);
        }
    }
    TEST_CHECK(offset == panel->init_len,
               "ST7796 init table: decoder consumes every byte -- exactly 22 steps, nothing left over");

    /* NEGATIVE TEST: proves the step count above is a real assertion, not
     * vacuous -- decoding must NOT succeed for a 23rd step past the real
     * table's end. */
    {
        uint8_t cmd, plen;
        const uint8_t *params;
        TEST_CHECK(panel_codec_init_step(panel->init_seq, panel->init_len, &offset,
                                          &cmd, &params, &plen) == false,
                   "ST7796 init table NEGATIVE: no 23rd step exists past the transcribed 22");
    }
}

/* --- Fast-path justification: passthrough == raw copy --------------------- */
static void test_st7796_fast_path_equivalence(void)
{
    TEST_SECTION("ST7796 fast path: passthrough encode == raw memcpy");

    /* panel_spi.c's blit fast path (bytes_per_pixel == 2) replaces a
     * per-pixel panel_codec_rgb565_passthrough() loop with one memcpy of the
     * whole chunk, on the reasoning that COLMOD 0x55's RAMWR stream is
     * byte-identical to the incoming RGB565 wire data. This is the host-
     * testable half of that claim: encoding every pixel of an arbitrary
     * buffer one at a time via the codec function produces EXACTLY the same
     * bytes as copying the input verbatim -- so panel_spi.c's memcpy is not
     * a shortcut that happens to look right, it is provably the same
     * output. (panel_spi.c's own use of memcpy instead of the loop is not
     * itself host-testable -- it lives in the ESP-IDF-dependent driver --
     * this test is what can be proven from here.) */
    static const uint8_t wire[] = {
        0x00, 0x00, 0xFF, 0xFF, 0x34, 0x12, 0xAB, 0xCD, 0x00, 0xF8, 0x1F, 0x00,
    };
    size_t pixels = sizeof(wire) / 2;
    uint8_t looped[sizeof(wire)];
    for (size_t i = 0; i < pixels; ++i) {
        uint16_t color = (uint16_t)(wire[i * 2] | ((uint16_t)wire[i * 2 + 1] << 8));
        panel_codec_rgb565_passthrough(color, &looped[i * 2]);
    }
    TEST_CHECK(memcmp(looped, wire, sizeof(wire)) == 0,
               "fast path: per-pixel passthrough encode reproduces the input buffer exactly, "
               "byte for byte -- proves a whole-chunk memcpy is equivalent, not just plausible");

    /* NEGATIVE TEST: proves the equivalence check above can actually fail --
     * corrupt one output byte the way a real widening bug (e.g. accidentally
     * routing ST7796 through the RGB666 widener) would, and confirm the
     * comparison catches it. */
    uint8_t corrupted[sizeof(wire)];
    memcpy(corrupted, wire, sizeof(wire));
    corrupted[2] ^= 0xFF;
    TEST_CHECK(memcmp(looped, corrupted, sizeof(wire)) != 0,
               "fast path NEGATIVE: a single corrupted byte is detected (proves the check above is live)");
}

void run_test_st7796_panel(void)
{
    test_init_step_decoder();
    test_st7796_descriptor_metadata();
    test_st7796_init_table_transcription();
    test_st7796_fast_path_equivalence();
}
