// Host tests for App/drivers/persist/ota_record.c -- specifically ota_record_fill(),
// the pure (no ESP-IDF I/O) half of the module. Added 2026-08-21 alongside
// ota_record_t's new image_sha256_hex field (CommonFW/docs/UPDATE_PROTOCOL.md's
// "an append-only update record in NVS: timestamp, processor, image SHA-256,
// version before and after, result").
//
// ota_record_append()/ota_record_load() (the NVS I/O, now hal_kv_* per
// HW_ABSTRACTION_PLAN.md Phase 3 item 3) are NOT exercised here -- this
// file's own header comment already explains why the module as a whole
// isn't host-tested (run_state.c/relay_cycles.c precedent), and this file
// never #includes ota_record.c or opens a real (fake_kv-backed) partition,
// so there would be nothing real to assert about a round trip anyway. What
// IS worth a host test, now that this field exists,
// is the pure fill/truncate/NULL-tolerance behavior every other string field
// in ota_record_t already gets implicitly exercised by -- this file makes
// that explicit for the new field specifically.
#include <string.h>

#include "test_common.h"

#include "../drivers/persist/ota_record.h"

static void test_fill_populates_sha256(void)
{
    TEST_SECTION("ota_record_fill -- image_sha256_hex populated verbatim when it fits");

    ota_record_t rec;
    const char *hash64 =
        "0123456789abcdef" "0123456789abcdef" "0123456789abcdef" "0123456789abcdef"; // 4 * 16 = 64 hex chars
    TEST_CHECK(strlen(hash64) == 64, "test fixture itself must be exactly 64 chars");

    ota_record_fill(&rec, 123u, "esp", "1.0.0", "1.1.0", true, "ok", hash64);

    TEST_CHECK(strcmp(rec.image_sha256_hex, hash64) == 0,
               "a 64-char hex hash fits OTA_RECORD_SHA256_HEX_MAX-1 and is stored verbatim");
    TEST_CHECK(rec.version == OTA_RECORD_VERSION, "version field still stamped");
    TEST_CHECK(strcmp(rec.processor, "esp") == 0, "processor field unaffected by the new field");
}

static void test_fill_null_hash_is_empty_string(void)
{
    TEST_SECTION("ota_record_fill -- NULL image_sha256_hex_or_null yields \"\", never a placeholder");

    ota_record_t rec;
    memset(&rec, 0xAA, sizeof(rec)); // poison first, so a leftover byte would be visible

    ota_record_fill(&rec, 0u, "pico", "", "", false, "some failure", NULL);

    TEST_CHECK(rec.image_sha256_hex[0] == '\0',
               "NULL hash -- record's hash field is an empty string, not garbage or a fake value");
}

static void test_fill_truncates_oversized_hash(void)
{
    TEST_SECTION("ota_record_fill -- an oversized hash string is truncated, never overflows");

    // NEGATIVE-TEST PROOF, per this repo's "prove a new check can fail"
    // discipline: OTA_RECORD_SHA256_HEX_MAX is 65 (64 hex chars + NUL). Feed
    // ota_record_fill() a string one character LONGER than that and confirm
    // truncation actually happens (stored length == 64, not 65) -- if
    // copy_str()'s cap arithmetic were ever off by one, this is the check
    // that would catch it.
    char too_long[OTA_RECORD_SHA256_HEX_MAX + 1 + 1]; // 67 chars + NUL
    memset(too_long, 'f', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    TEST_CHECK(strlen(too_long) == OTA_RECORD_SHA256_HEX_MAX + 1,
               "test fixture is deliberately one char longer than the field can hold");

    ota_record_t rec;
    ota_record_fill(&rec, 0u, "esp", "", "", true, "ok", too_long);

    TEST_CHECK(strlen(rec.image_sha256_hex) == OTA_RECORD_SHA256_HEX_MAX - 1,
               "stored hash is truncated to exactly OTA_RECORD_SHA256_HEX_MAX-1 chars, not left "
               "at the input's full length");
    TEST_CHECK(rec.image_sha256_hex[OTA_RECORD_SHA256_HEX_MAX - 1] == '\0',
               "still NUL-terminated within the field -- no overflow into adjacent struct fields");
}

static void test_fill_does_not_clobber_other_fields(void)
{
    TEST_SECTION("ota_record_fill -- adjacent fields (reason, reserved padding) are untouched by "
                 "the new hash field");

    ota_record_t rec;
    ota_record_fill(&rec, 42u, "esp", "1.0.0", "2.0.0", true, "ok",
                     "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");

    TEST_CHECK(strcmp(rec.reason, "ok") == 0, "reason field intact next to the new hash field");
    TEST_CHECK(rec.uptime_s == 42u, "uptime_s intact");
    TEST_CHECK(strcmp(rec.version_after, "2.0.0") == 0, "version_after intact");
}

void run_test_ota_record(void)
{
    test_fill_populates_sha256();
    test_fill_null_hash_is_empty_string();
    test_fill_truncates_oversized_hash();
    test_fill_does_not_clobber_other_fields();
}
