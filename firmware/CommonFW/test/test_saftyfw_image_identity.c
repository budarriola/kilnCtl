/* Host-native test for saftyfw_image_identity.{c,h} -- the pure scanner both
 * SaftyFW and KilnFW share to find the build-identity record embedded in a
 * SaftyFW slot image. Covers TODO.md 9.4's link_protocol_version field and
 * the SAFTYFW_IMAGE_IDENTITY_RECORD_VERSION 1 -> 2 bump: a record_version 1
 * image (the shape every SaftyFW image had before this field existed) must
 * be treated as structurally invalid -- "unknown", never misread as
 * declaring link_protocol_version 0.
 *
 * Exits 0 and prints "ALL PASS" if every check passes; on first failure
 * prints which one and exits 1. */

#include <stdio.h>
#include <string.h>

#include "kilnlink/saftyfw_image_identity.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);          \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static saftyfw_image_identity_t make_valid_record(uint16_t link_protocol_version)
{
    saftyfw_image_identity_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic0 = SAFTYFW_IMAGE_IDENTITY_MAGIC0;
    rec.magic1 = SAFTYFW_IMAGE_IDENTITY_MAGIC1;
    rec.record_version = SAFTYFW_IMAGE_IDENTITY_RECORD_VERSION;
    rec.dirty = 0;
    rec.commit_len = 7;
    memcpy(rec.commit, "abc1234", 7);
    rec.config_format_version = 3;
    rec.link_protocol_version = link_protocol_version;
    rec.magic_end = SAFTYFW_IMAGE_IDENTITY_MAGIC_END;
    return rec;
}

static void test_valid_record_round_trips_link_protocol_version(void)
{
    saftyfw_image_identity_t rec = make_valid_record(16);
    CHECK(saftyfw_image_identity_is_valid(&rec), "record_version 2 record with all magics is valid");

    uint8_t buf[SAFTYFW_IMAGE_IDENTITY_SIZE];
    memcpy(buf, &rec, sizeof(rec));

    saftyfw_image_identity_t out;
    memset(&out, 0, sizeof(out));
    bool found = saftyfw_image_identity_find(buf, sizeof(buf), &out);
    CHECK(found, "scanner finds a record filling the whole buffer exactly");
    CHECK(out.link_protocol_version == 16, "scanner round-trips link_protocol_version 16");
    CHECK(out.config_format_version == 3, "scanner round-trips config_format_version unchanged");
}

static void test_record_version_1_is_rejected_not_misread_as_zero(void)
{
    /* This is the exact backward-compatibility guarantee the record_version
     * bump's doc comment promises: an image built before link_protocol_version
     * existed reads back its trailing field as whatever the old `reserved`
     * writer put there (0, by convention) but must never be reported as "this
     * image declares link protocol 0" -- record_version 1 must fail
     * is_valid() outright so callers see "unknown", not a fabricated
     * mismatch. */
    saftyfw_image_identity_t rec = make_valid_record(0);
    rec.record_version = 1;
    CHECK(!saftyfw_image_identity_is_valid(&rec), "record_version 1 is rejected by is_valid()");

    uint8_t buf[SAFTYFW_IMAGE_IDENTITY_SIZE];
    memcpy(buf, &rec, sizeof(rec));
    saftyfw_image_identity_t out;
    memset(&out, 0, sizeof(out));
    bool found = saftyfw_image_identity_find(buf, sizeof(buf), &out);
    CHECK(!found, "scanner does not find a record_version 1 record as valid");
}

static void test_unrecognised_future_version_is_rejected(void)
{
    saftyfw_image_identity_t rec = make_valid_record(16);
    rec.record_version = 3;
    CHECK(!saftyfw_image_identity_is_valid(&rec), "an unrecognised future record_version is rejected, not guessed at");
}

static void test_bad_magic_is_rejected(void)
{
    saftyfw_image_identity_t rec = make_valid_record(16);
    rec.magic1 ^= 1u;
    CHECK(!saftyfw_image_identity_is_valid(&rec), "a corrupted magic1 is rejected");
}

static void test_bad_trailer_is_rejected(void)
{
    saftyfw_image_identity_t rec = make_valid_record(16);
    rec.magic_end ^= 1u;
    CHECK(!saftyfw_image_identity_is_valid(&rec), "a corrupted magic_end trailer is rejected");
}

static void test_commit_len_out_of_bounds_is_rejected(void)
{
    saftyfw_image_identity_t rec = make_valid_record(16);
    rec.commit_len = (uint8_t)(SAFTYFW_IMAGE_IDENTITY_COMMIT_MAX + 1u);
    CHECK(!saftyfw_image_identity_is_valid(&rec), "commit_len past COMMIT_MAX is rejected");
}

static void test_find_null_safe_and_short_buffer(void)
{
    CHECK(!saftyfw_image_identity_is_valid(NULL), "is_valid(NULL) is false, not a crash");

    saftyfw_image_identity_t rec = make_valid_record(16);
    uint8_t buf[SAFTYFW_IMAGE_IDENTITY_SIZE];
    memcpy(buf, &rec, sizeof(rec));
    saftyfw_image_identity_t out;

    CHECK(!saftyfw_image_identity_find(buf, sizeof(buf) - 4u, &out),
          "a buffer shorter than one record never reports found");
    CHECK(!saftyfw_image_identity_find(NULL, sizeof(buf), &out), "find(NULL, ...) is false, not a crash");
}

static void test_find_scans_past_leading_garbage(void)
{
    /* The record does not have to start at offset 0 -- the scanner steps
     * 4 bytes at a time looking for the magic pair, which is the entire
     * point of "no fixed offset" in this file's own header comment. */
    saftyfw_image_identity_t rec = make_valid_record(16);
    uint8_t buf[64 + SAFTYFW_IMAGE_IDENTITY_SIZE];
    memset(buf, 0xAA, sizeof(buf));
    memcpy(buf + 64, &rec, sizeof(rec));

    saftyfw_image_identity_t out;
    memset(&out, 0, sizeof(out));
    bool found = saftyfw_image_identity_find(buf, sizeof(buf), &out);
    CHECK(found, "scanner finds a record offset into a larger buffer by leading garbage");
    CHECK(out.link_protocol_version == 16, "record found past leading garbage round-trips link_protocol_version");
}

int main(void)
{
    test_valid_record_round_trips_link_protocol_version();
    test_record_version_1_is_rejected_not_misread_as_zero();
    test_unrecognised_future_version_is_rejected();
    test_bad_magic_is_rejected();
    test_bad_trailer_is_rejected();
    test_commit_len_out_of_bounds_is_rejected();
    test_find_null_safe_and_short_buffer();
    test_find_scans_past_leading_garbage();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
