// stage_header.h -- the on-flash header at the start of the `stage` partition
// (docs/GITHUB_RELEASE_UPDATE_PLAN.md sections 2-4): the application's stager
// writes the candidate image after it, then this header; the recovery image's
// apply_staged route validates it before copying stage -> app.
//
// Layout: one 4096-byte flash sector holds the header (STAGE_HEADER_SECTOR),
// the image starts at STAGE_IMAGE_OFFSET so the header can be erased alone.
// The header proper is STAGE_HEADER_SIZE (256) bytes, fixed little-endian:
//
//   off  size  field
//     0     4  magic            0x4754534B ("KSTG" as bytes K,S,T,G)
//     4     2  header_version   1
//     6     2  header_size      256
//     8     4  state            1 WRITING, 2 VERIFIED, 3 APPLYING
//    12     4  image_length     bytes of image at STAGE_IMAGE_OFFSET
//    16    32  sha256           of exactly image_length image bytes
//    48    32  semver           NUL-padded "MAJOR.MINOR.PATCH[-pre]" (no v)
//    80    40  commit           40 lowercase hex chars, or all zero bytes
//   120     4  source          0 unknown, 1 upload, 2 github
//   124   128  reserved         must be zero
//   252     4  crc32            IEEE CRC-32 of bytes 0..251
//
// Serialized explicitly (no struct overlay) so the byte layout is the same on
// the ESP application, the recovery image and the host tests. Erased flash
// reads 0xFF everywhere: stage_header_decode() reports that as BLANK, never as
// an error, so "nothing staged" is a normal state.
//
// Pure C, no ESP-IDF dependency; host-tested by App/test/test_update_stage_header.c.
#ifndef KILNCTL_STAGE_HEADER_H
#define KILNCTL_STAGE_HEADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STAGE_HEADER_MAGIC 0x4754534Bu
#define STAGE_HEADER_VERSION 1u
#define STAGE_HEADER_SIZE 256u
#define STAGE_HEADER_SECTOR 4096u
#define STAGE_IMAGE_OFFSET STAGE_HEADER_SECTOR
#define STAGE_SHA256_LEN 32u
#define STAGE_SEMVER_FIELD_LEN 32u
#define STAGE_COMMIT_HEX_LEN 40u

typedef enum {
    STAGE_STATE_WRITING = 1,  // image bytes being written; not installable
    STAGE_STATE_VERIFIED = 2, // sha256 re-read from flash and matched; installable
    STAGE_STATE_APPLYING = 3, // recovery has started copying stage -> app
} stage_state_t;

typedef enum {
    STAGE_SOURCE_UNKNOWN = 0,
    STAGE_SOURCE_UPLOAD = 1,
    STAGE_SOURCE_GITHUB = 2,
} stage_source_t;

typedef enum {
    STAGE_HDR_OK = 0,
    STAGE_HDR_BLANK,        // all 0xFF: nothing staged (decode only)
    STAGE_HDR_BAD_ARG,      // NULL pointer / buffer shorter than the header
    STAGE_HDR_BAD_MAGIC,
    STAGE_HDR_BAD_VERSION,
    STAGE_HDR_BAD_SIZE,     // header_size field != STAGE_HEADER_SIZE
    STAGE_HDR_BAD_CRC,
    STAGE_HDR_BAD_STATE,
    STAGE_HDR_BAD_LENGTH,   // 0, or larger than the partition's image capacity
    STAGE_HDR_BAD_SEMVER,   // unparsable or has a leading 'v' / does not fit
    STAGE_HDR_BAD_COMMIT,   // neither 40 lowercase hex nor all zero
    STAGE_HDR_BAD_SOURCE,
    STAGE_HDR_BAD_RESERVED, // non-zero reserved byte or garbage after a semver NUL
} stage_hdr_status_t;

typedef struct {
    stage_state_t state;
    uint32_t image_length;
    uint8_t sha256[STAGE_SHA256_LEN];
    char semver[STAGE_SEMVER_FIELD_LEN]; // NUL-terminated
    char commit[STAGE_COMMIT_HEX_LEN + 1]; // "" when unknown, else 40 hex + NUL
    stage_source_t source;
} stage_header_t;

// IEEE 802.3 CRC-32 (reflected, poly 0xEDB88320), exposed for tests.
uint32_t stage_header_crc32(const uint8_t *data, size_t len);

// Serialize *h into out[STAGE_HEADER_SIZE] after validating every field with
// the same rules decode applies; image_capacity bounds image_length. On any
// failure out is left untouched and the reason is returned.
stage_hdr_status_t stage_header_encode(const stage_header_t *h, uint32_t image_capacity,
                                       uint8_t out[STAGE_HEADER_SIZE]);

// Parse and fully validate buf (len >= STAGE_HEADER_SIZE). On STAGE_HDR_OK *out
// is filled; on any other status *out is zeroed. Check order: args, blank,
// magic, version, size, crc, state, length, semver, commit, source, reserved.
stage_hdr_status_t stage_header_decode(const uint8_t *buf, size_t len, uint32_t image_capacity,
                                       stage_header_t *out);

// True only for state VERIFIED (the one installable state).
bool stage_header_is_installable(const stage_header_t *h);

// Legal forward transitions: WRITING->VERIFIED, VERIFIED->APPLYING.
bool stage_header_transition_allowed(stage_state_t from, stage_state_t to);

// Constant-time compare of the header's sha256 against a freshly computed one.
bool stage_header_sha256_matches(const stage_header_t *h, const uint8_t computed[STAGE_SHA256_LEN]);

const char *stage_hdr_status_name(stage_hdr_status_t s);
const char *stage_state_name(stage_state_t s);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_STAGE_HEADER_H
