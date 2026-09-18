// Host tests for the SaftyFW build-identity record a slot image carries about
// itself (firmware/CommonFW/src/saftyfw_image_identity.c) --
// docs/PICO_AUTO_UPDATE_PLAN.md G2.
//
// WHY THIS IS WORTH HOST-TESTING. The scanner is the ONE thing standing
// between "a staged image" and "the identity this board expects the safety
// processor to report". A false positive here (accepting garbage that happens
// to contain the magic) makes a board chase an identity no image can deliver,
// and the readiness gate then refuses to fire; a false negative makes a
// perfectly good staged image unusable. Both failures are invisible on the
// bench until a firing is refused, and the code is compiled into BOTH
// firmwares from CommonFW, so neither target build alone covers it.
//
// The source is #included directly (the pattern every pure-logic test here
// uses) so the scanner's statics and exact compiled behaviour are under test,
// not a re-implementation.
#include <string.h>

#include "test_common.h"

#include "../../../CommonFW/src/saftyfw_image_identity.c"

/* Builds a well-formed record with the given commit text. */
static void make_record(saftyfw_image_identity_t *r, const char *commit, uint8_t dirty)
{
    memset(r, 0, sizeof(*r));
    r->magic0 = SAFTYFW_IMAGE_IDENTITY_MAGIC0;
    r->magic1 = SAFTYFW_IMAGE_IDENTITY_MAGIC1;
    r->record_version = SAFTYFW_IMAGE_IDENTITY_RECORD_VERSION;
    r->dirty = dirty;
    r->commit_len = (uint8_t)strlen(commit);
    memcpy(r->commit, commit, strlen(commit));
    r->config_format_version = 2u;
    r->reserved = 0u;
    r->magic_end = SAFTYFW_IMAGE_IDENTITY_MAGIC_END;
}

void run_test_pico_image_identity(void);

void run_test_pico_image_identity(void)
{
    printf("\n-- pico image identity --\n");

    saftyfw_image_identity_t rec;
    make_record(&rec, "0123456789abcdef0123456789abcdef01234567", 0u);
    TEST_CHECK(saftyfw_image_identity_is_valid(&rec), "a well-formed record validates");

    /* Each independent witness, broken one at a time. Three witnesses is the
     * whole reason this record needs no CRC of its own (a CRC over a string
     * literal cannot be computed in a C initialiser) -- so each one has to
     * actually be load-bearing. */
    saftyfw_image_identity_t bad = rec;
    bad.magic0 ^= 1u;
    TEST_CHECK(!saftyfw_image_identity_is_valid(&bad), "a corrupt magic0 is rejected");
    bad = rec;
    bad.magic1 ^= 1u;
    TEST_CHECK(!saftyfw_image_identity_is_valid(&bad), "a corrupt magic1 is rejected");
    bad = rec;
    bad.magic_end ^= 1u;
    TEST_CHECK(!saftyfw_image_identity_is_valid(&bad), "a corrupt trailer is rejected");

    /* A FUTURE record version must be refused, not half-read: the whole point
     * of carrying a version is that a later layout is a different struct. */
    bad = rec;
    bad.record_version = (uint16_t)(SAFTYFW_IMAGE_IDENTITY_RECORD_VERSION + 1u);
    TEST_CHECK(!saftyfw_image_identity_is_valid(&bad), "an unknown record version is rejected");

    /* commit_len bounds. Zero would mean "an image that declares no identity",
     * which must never compare equal to anything; over-long would let a reader
     * walk off the end of commit[]. */
    bad = rec;
    bad.commit_len = 0u;
    TEST_CHECK(!saftyfw_image_identity_is_valid(&bad), "a zero-length commit is rejected");
    bad = rec;
    bad.commit_len = (uint8_t)(SAFTYFW_IMAGE_IDENTITY_COMMIT_MAX + 1u);
    TEST_CHECK(!saftyfw_image_identity_is_valid(&bad), "an over-long commit_len is rejected");

    /* dirty is a flag, not a number: anything but 0/1 means this is not really
     * one of our records. */
    bad = rec;
    bad.dirty = 2u;
    TEST_CHECK(!saftyfw_image_identity_is_valid(&bad), "a non-boolean dirty flag is rejected");

    /* ---- the scanner ---- */
    saftyfw_image_identity_t found;
    uint8_t buf[512];
    memset(buf, 0xA5, sizeof(buf));

    memset(&found, 0, sizeof(found));
    TEST_CHECK(!saftyfw_image_identity_find(buf, sizeof(buf), &found),
                "a buffer with no record yields nothing");

    /* At a 4-byte-aligned offset: the normal case, since the record is a
     * word-aligned object in the image's rodata. */
    memcpy(buf + 128, &rec, sizeof(rec));
    memset(&found, 0, sizeof(found));
    TEST_CHECK(saftyfw_image_identity_find(buf, sizeof(buf), &found),
                "an embedded record is found");
    TEST_CHECK(found.commit_len == 40u, "the found record carries its commit length");
    TEST_CHECK(memcmp(found.commit, rec.commit, 40u) == 0, "the found commit text matches");
    TEST_CHECK(found.config_format_version == 2u,
                "the found record carries its config format version");

    /* A record that would run off the end must not be reported -- this is the
     * bound that keeps the scan from reading past the image. */
    memset(buf, 0xA5, sizeof(buf));
    memcpy(buf + sizeof(buf) - sizeof(rec), &rec, sizeof(rec));
    memset(&found, 0, sizeof(found));
    TEST_CHECK(saftyfw_image_identity_find(buf, sizeof(buf), &found),
                "a record ending exactly at the buffer end is still found");
    memset(&found, 0, sizeof(found));
    TEST_CHECK(!saftyfw_image_identity_find(buf, sizeof(buf) - 1u, &found),
                "a record truncated by one byte is NOT reported");

    /* The magic alone must not be enough: a buffer that merely contains the
     * magic word (a copy of this header in some unrelated data, say) must not
     * be mistaken for a record. */
    memset(buf, 0xA5, sizeof(buf));
    uint32_t m0 = SAFTYFW_IMAGE_IDENTITY_MAGIC0;
    memcpy(buf + 64, &m0, sizeof(m0));
    memset(&found, 0, sizeof(found));
    TEST_CHECK(!saftyfw_image_identity_find(buf, sizeof(buf), &found),
                "the magic word alone is not accepted as a record");

    /* A bare magic immediately before a REAL record must not make the scanner
     * give up at the first candidate -- it has to keep going. */
    memcpy(buf + 96, &rec, sizeof(rec));
    memset(&found, 0, sizeof(found));
    TEST_CHECK(saftyfw_image_identity_find(buf, sizeof(buf), &found),
                "a false magic does not hide a real record later in the buffer");
    TEST_CHECK(memcmp(found.commit, rec.commit, 40u) == 0,
                "the record found after a false magic is the real one");
}
