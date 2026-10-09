// Host tests for App/drivers/update/stage_header.c (docs/GITHUB_RELEASE_UPDATE_PLAN.md sections 2-4).
#include <string.h>

#include "test_common.h"

#include "../drivers/update/stage_header.h"

#define CAP (4u * 1024u * 1024u - STAGE_HEADER_SECTOR)

static stage_header_t good(void)
{
    stage_header_t h;
    memset(&h, 0, sizeof(h));
    h.state = STAGE_STATE_VERIFIED;
    h.image_length = 2600000u;
    for (int i = 0; i < 32; i++) {
        h.sha256[i] = (uint8_t)(i * 7 + 1);
    }
    strcpy(h.semver, "1.0.0");
    strcpy(h.commit, "0123456789abcdef0123456789abcdef01234567");
    h.source = STAGE_SOURCE_GITHUB;
    return h;
}

// Rewrite the trailing CRC after a deliberate byte edit so the field check
// under test is the one that fires, not the CRC check.
static void refresh_crc(uint8_t *buf)
{
    uint32_t c = stage_header_crc32(buf, 252);
    buf[252] = (uint8_t)c;
    buf[253] = (uint8_t)(c >> 8);
    buf[254] = (uint8_t)(c >> 16);
    buf[255] = (uint8_t)(c >> 24);
}

static void test_crc_vector(void)
{
    TEST_SECTION("stage_header -- CRC-32 known-answer");
    TEST_CHECK(stage_header_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u, "CRC-32 of '123456789'");
    TEST_CHECK(stage_header_crc32((const uint8_t *)"", 0) == 0u, "CRC-32 of empty input is 0");
}

static void test_roundtrip(void)
{
    TEST_SECTION("stage_header -- encode/decode round trip and byte layout");
    stage_header_t h = good();
    uint8_t buf[STAGE_HEADER_SIZE];
    TEST_CHECK(stage_header_encode(&h, CAP, buf) == STAGE_HDR_OK, "encode ok");
    TEST_CHECK(buf[0] == 'K' && buf[1] == 'S' && buf[2] == 'T' && buf[3] == 'G', "magic bytes K S T G");
    TEST_CHECK(buf[4] == 1 && buf[5] == 0, "header_version 1 little-endian");
    TEST_CHECK(buf[6] == 0 && buf[7] == 1, "header_size 256 little-endian");
    TEST_CHECK(buf[8] == STAGE_STATE_VERIFIED && buf[9] == 0, "state at offset 8");
    TEST_CHECK(buf[12] == (2600000u & 0xFF) && buf[15] == 0, "length little-endian at offset 12");
    TEST_CHECK(buf[16] == 1 && buf[47] == (uint8_t)(31 * 7 + 1), "sha256 at offsets 16..47");
    TEST_CHECK(memcmp(buf + 48, "1.0.0", 6) == 0 && buf[53] == 0, "semver at offset 48, NUL padded");
    TEST_CHECK(memcmp(buf + 80, h.commit, 40) == 0, "commit at offset 80");
    TEST_CHECK(buf[120] == STAGE_SOURCE_GITHUB, "source at offset 120");

    stage_header_t d;
    TEST_CHECK(stage_header_decode(buf, sizeof(buf), CAP, &d) == STAGE_HDR_OK, "decode ok");
    TEST_CHECK(d.state == h.state && d.image_length == h.image_length && d.source == h.source, "scalars survive");
    TEST_CHECK(memcmp(d.sha256, h.sha256, 32) == 0, "sha256 survives");
    TEST_CHECK(strcmp(d.semver, "1.0.0") == 0 && strcmp(d.commit, h.commit) == 0, "strings survive");

    // Longer buffer (the whole 4 KB sector) is fine; only the first 256 bytes matter.
    uint8_t sector[STAGE_HEADER_SECTOR];
    memset(sector, 0xFF, sizeof(sector));
    memcpy(sector, buf, sizeof(buf));
    TEST_CHECK(stage_header_decode(sector, sizeof(sector), CAP, &d) == STAGE_HDR_OK, "decode from a full sector");

    // Prerelease and empty commit.
    h = good();
    strcpy(h.semver, "1.1.0-rc.2");
    h.commit[0] = '\0';
    h.source = STAGE_SOURCE_UPLOAD;
    TEST_CHECK(stage_header_encode(&h, CAP, buf) == STAGE_HDR_OK, "prerelease + empty commit encodes");
    TEST_CHECK(stage_header_decode(buf, sizeof(buf), CAP, &d) == STAGE_HDR_OK && strcmp(d.semver, "1.1.0-rc.2") == 0 &&
                   d.commit[0] == '\0' && d.source == STAGE_SOURCE_UPLOAD,
               "prerelease and empty commit round trip");
}

static void test_blank(void)
{
    TEST_SECTION("stage_header -- erased flash is BLANK, not an error");
    uint8_t buf[STAGE_HEADER_SIZE];
    stage_header_t d;
    memset(buf, 0xFF, sizeof(buf));
    TEST_CHECK(stage_header_decode(buf, sizeof(buf), CAP, &d) == STAGE_HDR_BLANK, "all-0xFF is BLANK");
    buf[255] = 0xFE;
    TEST_CHECK(stage_header_decode(buf, sizeof(buf), CAP, &d) == STAGE_HDR_BAD_MAGIC, "one stray bit is not BLANK");
    memset(buf, 0, sizeof(buf));
    TEST_CHECK(stage_header_decode(buf, sizeof(buf), CAP, &d) == STAGE_HDR_BAD_MAGIC, "all-zero is not BLANK");
}

static void test_decode_rejections(void)
{
    TEST_SECTION("stage_header -- every validation rule rejects");
    stage_header_t h = good();
    uint8_t base[STAGE_HEADER_SIZE];
    TEST_CHECK(stage_header_encode(&h, CAP, base) == STAGE_HDR_OK, "baseline encodes");
    uint8_t b[STAGE_HEADER_SIZE];
    stage_header_t d;

    TEST_CHECK(stage_header_decode(NULL, 256, CAP, &d) == STAGE_HDR_BAD_ARG, "NULL buffer");
    TEST_CHECK(stage_header_decode(base, 255, CAP, &d) == STAGE_HDR_BAD_ARG, "short buffer");
    TEST_CHECK(stage_header_decode(base, 256, CAP, NULL) == STAGE_HDR_BAD_ARG, "NULL out");

    memcpy(b, base, sizeof(b));
    b[0] ^= 1;
    refresh_crc(b);
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_MAGIC, "bad magic");

    memcpy(b, base, sizeof(b));
    b[4] = 2;
    refresh_crc(b);
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_VERSION, "unknown header_version");

    memcpy(b, base, sizeof(b));
    b[7] = 2;
    refresh_crc(b);
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_SIZE, "wrong header_size");

    memcpy(b, base, sizeof(b));
    b[100] ^= 0x40; // corrupt commit without refreshing the CRC
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_CRC, "corrupted body -> CRC mismatch");
    memcpy(b, base, sizeof(b));
    b[252] ^= 1;
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_CRC, "corrupted CRC field");

    for (int s = 0; s <= 5; s++) {
        memcpy(b, base, sizeof(b));
        b[8] = (uint8_t)s;
        refresh_crc(b);
        stage_hdr_status_t want = (s >= 1 && s <= 3) ? STAGE_HDR_OK : STAGE_HDR_BAD_STATE;
        TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == want, "state 0 and 4+ invalid, 1..3 valid");
    }

    memcpy(b, base, sizeof(b));
    memset(b + 12, 0, 4);
    refresh_crc(b);
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_LENGTH, "zero length");
    h = good();
    h.image_length = CAP;
    TEST_CHECK(stage_header_encode(&h, CAP, b) == STAGE_HDR_OK && stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_OK,
               "length == capacity accepted");
    TEST_CHECK(stage_header_decode(b, 256, CAP - 1, &d) == STAGE_HDR_BAD_LENGTH, "length == capacity + 1 rejected");

    memcpy(b, base, sizeof(b));
    b[48] = 'x';
    refresh_crc(b);
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_SEMVER, "unparsable semver");
    memcpy(b, base, sizeof(b));
    b[48] = 'v';
    refresh_crc(b);
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_SEMVER, "semver with leading v rejected");
    memcpy(b, base, sizeof(b));
    memset(b + 48, 0, 32);
    refresh_crc(b);
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_SEMVER, "empty semver");
    memcpy(b, base, sizeof(b));
    memset(b + 48, '1', 32); // no NUL terminator
    refresh_crc(b);
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_SEMVER, "unterminated semver field");
    memcpy(b, base, sizeof(b));
    b[60] = 'z'; // garbage after the NUL
    refresh_crc(b);
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_RESERVED, "garbage after semver NUL");

    memcpy(b, base, sizeof(b));
    b[80] = 'G'; // uppercase/non-hex
    refresh_crc(b);
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_COMMIT, "non-hex commit");
    memcpy(b, base, sizeof(b));
    b[80] = 'A';
    refresh_crc(b);
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_COMMIT, "uppercase hex commit");
    memcpy(b, base, sizeof(b));
    b[119] = 0; // short commit: zero in the middle of hex
    refresh_crc(b);
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_COMMIT, "commit that is neither 40 hex nor all zero");

    memcpy(b, base, sizeof(b));
    b[120] = 3;
    refresh_crc(b);
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_SOURCE, "unknown source");

    memcpy(b, base, sizeof(b));
    b[124] = 1;
    refresh_crc(b);
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_RESERVED, "non-zero reserved byte (first)");
    memcpy(b, base, sizeof(b));
    b[251] = 1;
    refresh_crc(b);
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) == STAGE_HDR_BAD_RESERVED, "non-zero reserved byte (last)");

    // A rejection zeroes the output.
    memcpy(b, base, sizeof(b));
    b[0] ^= 1;
    memset(&d, 0x5A, sizeof(d));
    TEST_CHECK(stage_header_decode(b, 256, CAP, &d) != STAGE_HDR_OK && d.image_length == 0 && d.semver[0] == '\0',
               "failed decode zeroes the output");
}

static void test_encode_rejections(void)
{
    TEST_SECTION("stage_header -- encode refuses an invalid header and leaves the buffer alone");
    uint8_t out[STAGE_HEADER_SIZE];
    stage_header_t h;
#define ENC_FAIL(MUTATE, WANT, MSG)                                                                                    \
    do {                                                                                                               \
        h = good();                                                                                                    \
        MUTATE;                                                                                                        \
        memset(out, 0xA5, sizeof(out));                                                                                \
        TEST_CHECK(stage_header_encode(&h, CAP, out) == (WANT), MSG);                                                  \
        TEST_CHECK(out[0] == 0xA5 && out[255] == 0xA5, "buffer untouched after refused encode");                       \
    } while (0)
    ENC_FAIL(h.state = (stage_state_t)0, STAGE_HDR_BAD_STATE, "state 0");
    ENC_FAIL(h.state = (stage_state_t)9, STAGE_HDR_BAD_STATE, "state 9");
    ENC_FAIL(h.image_length = 0, STAGE_HDR_BAD_LENGTH, "zero length");
    ENC_FAIL(h.image_length = CAP + 1, STAGE_HDR_BAD_LENGTH, "over capacity");
    ENC_FAIL(strcpy(h.semver, "v1.0.0"), STAGE_HDR_BAD_SEMVER, "v-prefixed semver");
    ENC_FAIL(strcpy(h.semver, "bogus"), STAGE_HDR_BAD_SEMVER, "bogus semver");
    ENC_FAIL(h.semver[0] = '\0', STAGE_HDR_BAD_SEMVER, "empty semver");
    ENC_FAIL(strcpy(h.commit, "abc"), STAGE_HDR_BAD_COMMIT, "short commit");
    ENC_FAIL(h.commit[5] = 'Z', STAGE_HDR_BAD_COMMIT, "non-hex commit");
    ENC_FAIL(h.source = (stage_source_t)7, STAGE_HDR_BAD_SOURCE, "unknown source");
#undef ENC_FAIL
    TEST_CHECK(stage_header_encode(NULL, CAP, out) == STAGE_HDR_BAD_ARG, "NULL header");
    h = good();
    TEST_CHECK(stage_header_encode(&h, CAP, NULL) == STAGE_HDR_BAD_ARG, "NULL out");
}

static void test_state_machine(void)
{
    TEST_SECTION("stage_header -- states, transitions, sha256 match");
    stage_header_t h = good();
    TEST_CHECK(stage_header_is_installable(&h), "VERIFIED is installable");
    h.state = STAGE_STATE_WRITING;
    TEST_CHECK(!stage_header_is_installable(&h), "WRITING is not installable");
    h.state = STAGE_STATE_APPLYING;
    TEST_CHECK(!stage_header_is_installable(&h), "APPLYING is not installable (interrupted apply needs a re-verify)");
    TEST_CHECK(!stage_header_is_installable(NULL), "NULL not installable");

    TEST_CHECK(stage_header_transition_allowed(STAGE_STATE_WRITING, STAGE_STATE_VERIFIED), "WRITING->VERIFIED");
    TEST_CHECK(stage_header_transition_allowed(STAGE_STATE_VERIFIED, STAGE_STATE_APPLYING), "VERIFIED->APPLYING");
    TEST_CHECK(!stage_header_transition_allowed(STAGE_STATE_WRITING, STAGE_STATE_APPLYING),
               "WRITING->APPLYING skips verification");
    TEST_CHECK(!stage_header_transition_allowed(STAGE_STATE_VERIFIED, STAGE_STATE_WRITING), "no backward step");
    TEST_CHECK(!stage_header_transition_allowed(STAGE_STATE_APPLYING, STAGE_STATE_VERIFIED), "APPLYING is terminal");
    TEST_CHECK(!stage_header_transition_allowed(STAGE_STATE_VERIFIED, STAGE_STATE_VERIFIED), "no self transition");

    h = good();
    uint8_t c[32];
    memcpy(c, h.sha256, 32);
    TEST_CHECK(stage_header_sha256_matches(&h, c), "identical digest matches");
    for (int i = 0; i < 32; i++) {
        c[i] ^= 0x80;
        TEST_CHECK(!stage_header_sha256_matches(&h, c), "single-byte difference rejected");
        c[i] ^= 0x80;
    }
    TEST_CHECK(!stage_header_sha256_matches(NULL, c) && !stage_header_sha256_matches(&h, NULL), "NULL args rejected");

    TEST_CHECK(strcmp(stage_hdr_status_name(STAGE_HDR_BAD_CRC), "bad_crc") == 0, "status name");
    TEST_CHECK(strcmp(stage_state_name(STAGE_STATE_VERIFIED), "verified") == 0, "state name");
}

void run_test_update_stage_header(void)
{
    test_crc_vector();
    test_roundtrip();
    test_blank();
    test_decode_rejections();
    test_encode_rejections();
    test_state_machine();
}
