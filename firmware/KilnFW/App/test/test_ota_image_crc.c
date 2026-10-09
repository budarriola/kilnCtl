// Host tests for App/drivers/http/ota_image_crc.c -- the CRC-32 the ESP
// computes over a staged Pico image.
//
// WHY THIS TEST EXISTS, and why the module it tests exists at all. Until
// 2026-09-18 this CRC was computed inline inside ota_pico_do_stage()
// (ota_http_pico.c) with the wrong parameterization: it seeded
// esp_rom_crc32_le() with 0xFFFFFFFF and then applied its own final XOR, on
// top of a primitive that already performs both of those inversions itself.
// The two extra inversions do not cancel -- they shift the result to a
// CRC-32 variant with init 0 and no output XOR. The Pico reads the written
// slot back and CRCs it with bootloader_crc32(), which is real CRC-32/zlib,
// so the comparison could never succeed for ANY image and the entire
// ESP-driven Pico OTA path was inoperable. Full analysis:
// docs/audits/pico_ota_staged_crc_mismatch_2026-09-18.md.
//
// SaftyFW's side was pinned by a known-answer test the whole time
// (firmware/SaftyFW/test/test_bootloader_metadata.c asserts
// bootloader_crc32("123456789", 9) == 0xCBF43926). The ESP side had none,
// and could not easily have one: HTTP handlers in this project are
// target-build-only and do not link into the host suite, so a test written
// against ota_pico_do_stage() would never have run. That is why the CRC
// step was factored out into ota_image_crc.c -- a small, linkable, ESP-IDF-
// free module -- and why this file asserts the SAME standard check value
// the Pico's own test does. The two known-answer tests now pin both ends of
// the comparison to the same number.
#include <stdio.h>
#include <string.h>

#include "test_common.h"

#include "ota_image_crc.h"

// The standard CRC-32 check value: the ASCII bytes "123456789".
static const uint8_t KAT_INPUT[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
#define KAT_EXPECTED 0xCBF43926u

// ---------------------------------------------------------------------------
// The check value. This is the assertion that would have failed the day the
// defect was written, and the one that pins the fix.
// ---------------------------------------------------------------------------
static void test_known_answer_check_value(void)
{
    uint32_t crc = ota_image_crc32(KAT_INPUT, sizeof(KAT_INPUT));
    if (crc != KAT_EXPECTED) {
        printf("  (got 0x%08X, want 0x%08X)\n", (unsigned)crc, (unsigned)KAT_EXPECTED);
    }
    TEST_CHECK(crc == KAT_EXPECTED,
               "CRC-32 of \"123456789\" is the standard check value 0xCBF43926 -- the same "
               "number SaftyFW's bootloader_crc32() known-answer test asserts");
}

// The streaming form must reach the same value as the one-shot form, since
// ota_pico_do_stage() only ever uses the streaming form.
static void test_streaming_form_matches_one_shot(void)
{
    uint32_t crc = ota_image_crc32_update(OTA_IMAGE_CRC32_INIT, KAT_INPUT, sizeof(KAT_INPUT));
    TEST_CHECK(crc == KAT_EXPECTED,
               "the running form, seeded OTA_IMAGE_CRC32_INIT and finished with no final XOR, "
               "reaches the same check value");
}

// ---------------------------------------------------------------------------
// The specific trap, asserted directly: the recipe this code used to follow
// must NOT produce the correct answer. If it did, the defect would have been
// harmless and this module would have no reason to exist -- and, more to the
// point, an implementation that quietly reintroduced the double inversion
// could pass the check-value test above by accident.
// ---------------------------------------------------------------------------
static void test_double_inverted_recipe_is_wrong(void)
{
    // Exactly what ota_pico_do_stage() did before the fix: seed 0xFFFFFFFF,
    // chain, then apply a final XOR on top of a primitive that already
    // inverts at both ends.
    uint32_t bad = ota_image_crc32_update(0xFFFFFFFFu, KAT_INPUT, sizeof(KAT_INPUT));
    bad ^= 0xFFFFFFFFu;
    TEST_CHECK(bad != KAT_EXPECTED,
               "seeding 0xFFFFFFFF and applying a final XOR does NOT reach the standard check "
               "value -- the extra inversions do not cancel, they select a different variant");
    TEST_CHECK(bad != ota_image_crc32(KAT_INPUT, sizeof(KAT_INPUT)),
               "the double-inverted recipe and the correct one disagree on the same bytes");
}

// ---------------------------------------------------------------------------
// Chunk-boundary independence. ota_pico_do_stage() folds the image in
// 4096-byte pieces whose sizes are whatever httpd_req_recv() happens to
// return, so the result must not depend on where the splits land.
// ---------------------------------------------------------------------------
static void test_chunking_does_not_change_the_result(void)
{
    uint8_t buf[5000];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)((i * 31u + (i >> 3)) & 0xFFu);
    }

    uint32_t whole = ota_image_crc32(buf, sizeof(buf));

    // Split at the handler's own chunk size, with a short tail -- the exact
    // shape a real staged image produces.
    uint32_t split = ota_image_crc32_update(OTA_IMAGE_CRC32_INIT, buf, 4096);
    split = ota_image_crc32_update(split, buf + 4096, sizeof(buf) - 4096);
    TEST_CHECK(split == whole, "a 4096-byte chunk plus a short tail matches the one-shot CRC");

    // Pathological splits: one byte at a time over a prefix, then the rest.
    uint32_t bytewise = OTA_IMAGE_CRC32_INIT;
    for (size_t i = 0; i < 37; i++) {
        bytewise = ota_image_crc32_update(bytewise, buf + i, 1);
    }
    bytewise = ota_image_crc32_update(bytewise, buf + 37, sizeof(buf) - 37);
    TEST_CHECK(bytewise == whole, "byte-at-a-time chunking matches the one-shot CRC");
}

// ---------------------------------------------------------------------------
// It is a real integrity check, not a stand-in that returns a constant: a
// single flipped bit anywhere must change the value.
// ---------------------------------------------------------------------------
static void test_single_bit_corruption_is_detected(void)
{
    uint8_t buf[256];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)i;
    }
    uint32_t clean = ota_image_crc32(buf, sizeof(buf));

    buf[0] ^= 0x01u;
    TEST_CHECK(ota_image_crc32(buf, sizeof(buf)) != clean, "a flipped bit in the first byte changes the CRC");
    buf[0] ^= 0x01u;

    buf[sizeof(buf) - 1] ^= 0x80u;
    TEST_CHECK(ota_image_crc32(buf, sizeof(buf)) != clean, "a flipped bit in the last byte changes the CRC");
    buf[sizeof(buf) - 1] ^= 0x80u;

    TEST_CHECK(ota_image_crc32(buf, sizeof(buf)) == clean, "restoring both bits restores the original CRC");
}

// ---------------------------------------------------------------------------
// Degenerate inputs the handler can hand it (a zero-length recv is already
// rejected upstream, but the helper must not read through a NULL).
// ---------------------------------------------------------------------------
static void test_empty_and_null_inputs_pass_the_seed_through(void)
{
    TEST_CHECK(ota_image_crc32_update(OTA_IMAGE_CRC32_INIT, NULL, 0) == OTA_IMAGE_CRC32_INIT,
               "a NULL/empty update returns the running value unchanged");

    uint32_t mid = ota_image_crc32_update(OTA_IMAGE_CRC32_INIT, KAT_INPUT, 4);
    TEST_CHECK(ota_image_crc32_update(mid, KAT_INPUT, 0) == mid,
               "a zero-length update mid-stream does not disturb the running value");
    TEST_CHECK(ota_image_crc32_update(mid, NULL, 9) == mid,
               "a NULL buffer with a nonzero length returns the running value rather than reading it");
}

void run_test_ota_image_crc(void)
{
    TEST_SECTION("ota_image_crc");
    test_known_answer_check_value();
    test_streaming_form_matches_one_shot();
    test_double_inverted_recipe_is_wrong();
    test_chunking_does_not_change_the_result();
    test_single_bit_corruption_is_detected();
    test_empty_and_null_inputs_pass_the_seed_through();
}
