// test_saftyfw_image_identity_record.c -- host test for the REAL
// saftyfw_image_identity_get() (src/update/saftyfw_image_identity_record.c),
// SaftyFW's own build-identity record (R2-5, HOST_TEST_COVERAGE_GAPS_ROUND2).
//
// test_link_task_fuzz.c replaces this accessor with a stub, so until now the
// shipped record was never read by any test. Covered: every field's exact
// value, the byte layout the ESP-side scanner depends on, the round trip
// through the real CommonFW saftyfw_image_identity_find() inside a larger
// buffer, and that single-byte corruption of a GUARDED byte (magic, version,
// length, trailer) is rejected by the same scanner. The record has no CRC, so a
// flipped commit/config byte is NOT detected and would read as a different id.
#include <stdio.h>
#include <string.h>

#include "config_store.h"
#include "kilnlink/kilnlink_version.h"
#include "kilnlink/saftyfw_image_identity.h"
#include "saftyfw_image_identity_record.h"

static int g_checks = 0;
static int g_fail = 0;
#define CHECK(cond, ...)                        \
    do {                                        \
        g_checks++;                             \
        if (!(cond)) {                          \
            g_fail++;                           \
            printf("FAIL line %d: ", __LINE__); \
            printf(__VA_ARGS__);                \
            printf("\n");                       \
        }                                       \
    } while (0)

#define REC_OFF 148u

int main(void)
{
    const saftyfw_image_identity_t *rec = saftyfw_image_identity_get();
    CHECK(rec != NULL, "accessor never returns NULL");
    CHECK(saftyfw_image_identity_get() == rec, "accessor returns the one compiled-in record");
    CHECK(saftyfw_image_identity_is_valid(rec), "shipped record is structurally valid");

    CHECK(rec->magic0 == 0x44494653u, "magic0 0x%08X", (unsigned)rec->magic0);
    CHECK(rec->magic1 == 0xA5C31E7Bu, "magic1 0x%08X", (unsigned)rec->magic1);
    CHECK(rec->magic_end == 0x7BE1C35Au, "magic_end 0x%08X", (unsigned)rec->magic_end);
    CHECK(rec->record_version == 2u, "record_version %u", (unsigned)rec->record_version);
    CHECK(rec->dirty == 1u, "dirty normalised to 1, got %u", (unsigned)rec->dirty);
    CHECK(rec->commit_len == 12u, "commit_len %u", (unsigned)rec->commit_len);
    CHECK(memcmp(rec->commit, "0123456789ab", 12) == 0, "commit bytes");
    for (size_t i = 12; i < SAFTYFW_IMAGE_IDENTITY_COMMIT_MAX; i++) {
        CHECK(rec->commit[i] == '\0', "commit[%zu] zero padded", i);
    }
    CHECK(rec->config_format_version == (uint16_t)CONFIG_STORE_FORMAT_VERSION,
          "config_format_version %u", (unsigned)rec->config_format_version);
    CHECK(rec->link_protocol_version == (uint16_t)KILNLINK_PROTOCOL_VERSION,
          "link_protocol_version %u", (unsigned)rec->link_protocol_version);

    // Wire layout the scanner relies on (little-endian, no padding).
    uint8_t raw[SAFTYFW_IMAGE_IDENTITY_SIZE];
    memcpy(raw, rec, sizeof(raw));
    CHECK(raw[0] == 'S' && raw[1] == 'F' && raw[2] == 'I' && raw[3] == 'D', "magic0 bytes spell SFID");
    CHECK(raw[8] == 2 && raw[9] == 0, "record_version little-endian at offset 8");
    CHECK(raw[10] == 1, "dirty at offset 10");
    CHECK(raw[11] == 12, "commit_len at offset 11");
    CHECK(raw[12] == '0' && raw[23] == 'b', "commit starts at offset 12");
    CHECK(raw[52] == (uint8_t)(CONFIG_STORE_FORMAT_VERSION & 0xFFu), "config_format_version at offset 52");
    CHECK(raw[54] == (uint8_t)(KILNLINK_PROTOCOL_VERSION & 0xFF), "link_protocol_version at offset 54");

    // Round trip: bury the shipped bytes at a 4-aligned offset in a larger
    // image-like buffer and find them with the ESP-side scanner.
    uint8_t img[512];
    memset(img, 0xFF, sizeof(img));
    memcpy(img + REC_OFF, raw, sizeof(raw));
    saftyfw_image_identity_t out;
    memset(&out, 0, sizeof(out));
    CHECK(saftyfw_image_identity_find(img, sizeof(img), &out), "scanner finds the shipped record");
    CHECK(memcmp(&out, rec, sizeof(out)) == 0, "found record equals shipped record byte for byte");

    // Corruption of magic0, magic1, record_version, commit_len, trailer: the
    // scanner must report absent and leave *out untouched.
    const size_t guarded[] = { 0, 4, 8, 11, 56 };
    for (size_t g = 0; g < sizeof(guarded) / sizeof(guarded[0]); g++) {
        uint8_t bad[512];
        memcpy(bad, img, sizeof(bad));
        bad[REC_OFF + guarded[g]] ^= (guarded[g] == 11) ? 0x80u : 0x01u;
        saftyfw_image_identity_t probe;
        memset(&probe, 0x5A, sizeof(probe));
        saftyfw_image_identity_t before = probe;
        bool found = saftyfw_image_identity_find(bad, sizeof(bad), &probe);
        CHECK(!found, "corrupt byte at record offset %zu is rejected", guarded[g]);
        CHECK(memcmp(&probe, &before, sizeof(before)) == 0,
              "failed find leaves *out untouched (offset %zu)", guarded[g]);
    }

    // Unknown record version reads as absent, never guessed.
    {
        uint8_t bad[512];
        memcpy(bad, img, sizeof(bad));
        bad[REC_OFF + 8] = 3;
        saftyfw_image_identity_t o;
        CHECK(!saftyfw_image_identity_find(bad, sizeof(bad), &o), "record_version 3 is treated as absent");
    }

    printf("test_saftyfw_image_identity_record: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
