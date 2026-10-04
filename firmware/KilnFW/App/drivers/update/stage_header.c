// stage_header.c -- see stage_header.h for the byte layout.
#include "stage_header.h"

#include <string.h>

#include "update_semver.h"

#define OFF_MAGIC 0u
#define OFF_VERSION 4u
#define OFF_HSIZE 6u
#define OFF_STATE 8u
#define OFF_LENGTH 12u
#define OFF_SHA 16u
#define OFF_SEMVER 48u
#define OFF_COMMIT 80u
#define OFF_SOURCE 120u
#define OFF_RESERVED 124u
#define OFF_CRC 252u

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool state_valid(uint32_t s)
{
    return s == STAGE_STATE_WRITING || s == STAGE_STATE_VERIFIED || s == STAGE_STATE_APPLYING;
}
static bool source_valid(uint32_t s)
{
    return s == STAGE_SOURCE_UNKNOWN || s == STAGE_SOURCE_UPLOAD || s == STAGE_SOURCE_GITHUB;
}

// semver field: NUL-terminated string within the 32-byte field, parseable,
// canonical (no leading v: the field is the normalised form), and every byte
// after the NUL is zero.
static stage_hdr_status_t check_semver_field(const char *f, bool *reserved_bad)
{
    size_t n = 0;
    while (n < STAGE_SEMVER_FIELD_LEN && f[n] != '\0') {
        n++;
    }
    if (n == 0 || n == STAGE_SEMVER_FIELD_LEN) {
        return STAGE_HDR_BAD_SEMVER; // empty, or no NUL terminator
    }
    for (size_t k = n; k < STAGE_SEMVER_FIELD_LEN; k++) {
        if (f[k] != '\0') {
            *reserved_bad = true;
        }
    }
    if (f[0] == 'v' || f[0] == 'V') {
        return STAGE_HDR_BAD_SEMVER;
    }
    update_semver_t v;
    if (!update_semver_parse_n(f, n, &v)) {
        return STAGE_HDR_BAD_SEMVER;
    }
    return STAGE_HDR_OK;
}

// commit field of exactly 40 bytes: all lowercase hex, or all zero bytes.
static bool commit_field_valid(const uint8_t *f)
{
    bool all_zero = true;
    for (size_t i = 0; i < STAGE_COMMIT_HEX_LEN; i++) {
        if (f[i] != 0) {
            all_zero = false;
        }
    }
    if (all_zero) {
        return true;
    }
    for (size_t i = 0; i < STAGE_COMMIT_HEX_LEN; i++) {
        uint8_t c = f[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

stage_hdr_status_t stage_header_encode(const stage_header_t *h, uint32_t image_capacity,
                                       uint8_t out[STAGE_HEADER_SIZE])
{
    if (h == NULL || out == NULL) {
        return STAGE_HDR_BAD_ARG;
    }
    if (!state_valid((uint32_t)h->state)) {
        return STAGE_HDR_BAD_STATE;
    }
    if (h->image_length == 0 || h->image_length > image_capacity) {
        return STAGE_HDR_BAD_LENGTH;
    }
    bool reserved_bad = false;
    stage_hdr_status_t sv = check_semver_field(h->semver, &reserved_bad);
    if (sv != STAGE_HDR_OK) {
        return sv;
    }
    if (reserved_bad) {
        return STAGE_HDR_BAD_SEMVER;
    }
    size_t clen = strnlen(h->commit, sizeof(h->commit));
    if (clen != 0 && clen != STAGE_COMMIT_HEX_LEN) {
        return STAGE_HDR_BAD_COMMIT;
    }
    uint8_t cf[STAGE_COMMIT_HEX_LEN];
    memset(cf, 0, sizeof(cf));
    memcpy(cf, h->commit, clen);
    if (!commit_field_valid(cf)) {
        return STAGE_HDR_BAD_COMMIT;
    }
    if (!source_valid((uint32_t)h->source)) {
        return STAGE_HDR_BAD_SOURCE;
    }

    uint8_t buf[STAGE_HEADER_SIZE];
    memset(buf, 0, sizeof(buf));
    put_u32(buf + OFF_MAGIC, STAGE_HEADER_MAGIC);
    put_u16(buf + OFF_VERSION, (uint16_t)STAGE_HEADER_VERSION);
    put_u16(buf + OFF_HSIZE, (uint16_t)STAGE_HEADER_SIZE);
    put_u32(buf + OFF_STATE, (uint32_t)h->state);
    put_u32(buf + OFF_LENGTH, h->image_length);
    memcpy(buf + OFF_SHA, h->sha256, STAGE_SHA256_LEN);
    memcpy(buf + OFF_SEMVER, h->semver, strnlen(h->semver, STAGE_SEMVER_FIELD_LEN));
    memcpy(buf + OFF_COMMIT, cf, STAGE_COMMIT_HEX_LEN);
    put_u32(buf + OFF_SOURCE, (uint32_t)h->source);
    put_u32(buf + OFF_CRC, stage_header_crc32(buf, OFF_CRC));
    memcpy(out, buf, STAGE_HEADER_SIZE);
    return STAGE_HDR_OK;
}

stage_hdr_status_t stage_header_decode(const uint8_t *buf, size_t len, uint32_t image_capacity,
                                       stage_header_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (buf == NULL || out == NULL || len < STAGE_HEADER_SIZE) {
        return STAGE_HDR_BAD_ARG;
    }
    bool blank = true;
    for (size_t i = 0; i < STAGE_HEADER_SIZE; i++) {
        if (buf[i] != 0xFFu) {
            blank = false;
            break;
        }
    }
    if (blank) {
        return STAGE_HDR_BLANK;
    }
    if (get_u32(buf + OFF_MAGIC) != STAGE_HEADER_MAGIC) {
        return STAGE_HDR_BAD_MAGIC;
    }
    if (get_u16(buf + OFF_VERSION) != STAGE_HEADER_VERSION) {
        return STAGE_HDR_BAD_VERSION;
    }
    if (get_u16(buf + OFF_HSIZE) != STAGE_HEADER_SIZE) {
        return STAGE_HDR_BAD_SIZE;
    }
    if (get_u32(buf + OFF_CRC) != stage_header_crc32(buf, OFF_CRC)) {
        return STAGE_HDR_BAD_CRC;
    }
    uint32_t state = get_u32(buf + OFF_STATE);
    if (!state_valid(state)) {
        return STAGE_HDR_BAD_STATE;
    }
    uint32_t length = get_u32(buf + OFF_LENGTH);
    if (length == 0 || length > image_capacity) {
        return STAGE_HDR_BAD_LENGTH;
    }
    bool reserved_bad = false;
    stage_hdr_status_t sv = check_semver_field((const char *)(buf + OFF_SEMVER), &reserved_bad);
    if (sv != STAGE_HDR_OK) {
        return sv;
    }
    if (!commit_field_valid(buf + OFF_COMMIT)) {
        return STAGE_HDR_BAD_COMMIT;
    }
    uint32_t source = get_u32(buf + OFF_SOURCE);
    if (!source_valid(source)) {
        return STAGE_HDR_BAD_SOURCE;
    }
    if (reserved_bad) {
        return STAGE_HDR_BAD_RESERVED;
    }
    for (size_t i = OFF_RESERVED; i < OFF_CRC; i++) {
        if (buf[i] != 0) {
            return STAGE_HDR_BAD_RESERVED;
        }
    }

    stage_header_t t;
    memset(&t, 0, sizeof(t));
    t.state = (stage_state_t)state;
    t.image_length = length;
    memcpy(t.sha256, buf + OFF_SHA, STAGE_SHA256_LEN);
    memcpy(t.semver, buf + OFF_SEMVER, STAGE_SEMVER_FIELD_LEN);
    t.semver[STAGE_SEMVER_FIELD_LEN - 1] = '\0';
    memcpy(t.commit, buf + OFF_COMMIT, STAGE_COMMIT_HEX_LEN); // all-zero stays ""
    t.commit[STAGE_COMMIT_HEX_LEN] = '\0';
    t.source = (stage_source_t)source;
    *out = t;
    return STAGE_HDR_OK;
}

bool stage_header_is_installable(const stage_header_t *h)
{
    return h != NULL && h->state == STAGE_STATE_VERIFIED;
}

bool stage_header_transition_allowed(stage_state_t from, stage_state_t to)
{
    return (from == STAGE_STATE_WRITING && to == STAGE_STATE_VERIFIED) ||
           (from == STAGE_STATE_VERIFIED && to == STAGE_STATE_APPLYING);
}

bool stage_header_sha256_matches(const stage_header_t *h, const uint8_t computed[STAGE_SHA256_LEN])
{
    if (h == NULL || computed == NULL) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < STAGE_SHA256_LEN; i++) {
        diff |= (uint8_t)(h->sha256[i] ^ computed[i]);
    }
    return diff == 0;
}

const char *stage_hdr_status_name(stage_hdr_status_t s)
{
    switch (s) {
    case STAGE_HDR_OK: return "ok";
    case STAGE_HDR_BLANK: return "blank";
    case STAGE_HDR_BAD_ARG: return "bad_arg";
    case STAGE_HDR_BAD_MAGIC: return "bad_magic";
    case STAGE_HDR_BAD_VERSION: return "bad_version";
    case STAGE_HDR_BAD_SIZE: return "bad_size";
    case STAGE_HDR_BAD_CRC: return "bad_crc";
    case STAGE_HDR_BAD_STATE: return "bad_state";
    case STAGE_HDR_BAD_LENGTH: return "bad_length";
    case STAGE_HDR_BAD_SEMVER: return "bad_semver";
    case STAGE_HDR_BAD_COMMIT: return "bad_commit";
    case STAGE_HDR_BAD_SOURCE: return "bad_source";
    case STAGE_HDR_BAD_RESERVED: return "bad_reserved";
    }
    return "unknown";
}

const char *stage_state_name(stage_state_t s)
{
    switch (s) {
    case STAGE_STATE_WRITING: return "writing";
    case STAGE_STATE_VERIFIED: return "verified";
    case STAGE_STATE_APPLYING: return "applying";
    }
    return "unknown";
}
