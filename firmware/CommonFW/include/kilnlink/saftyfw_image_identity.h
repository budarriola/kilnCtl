// saftyfw_image_identity.h -- the build-identity record every SaftyFW slot
// image carries INSIDE ITSELF, and the pure scanner that finds it.
//
// WHY THIS EXISTS (docs/PICO_AUTO_UPDATE_PLAN.md G1/G2). The ESP has to
// answer "is the Pico out of date?" before it may push a new image at it.
// That needs two identities: what the Pico reports over the link (it already
// reports one -- SAFETY_CMD_FW_VERSION carries commit/dirty), and what the
// image the ESP is about to push WOULD report once it is running. The plan
// originally proposed stamping the second one into the ESP build at compile
// time. That cannot work in this tree (see the plan's sec 2 revision), and
// worse, it would be a second hand-maintained copy of a string whose only
// real source is a different processor's build -- exactly the "MANUAL SYNC
// HAZARD" firmware/SaftyFW/CMakeLists.txt already flags about its own slot
// addresses.
//
// So the image states its own identity instead. SaftyFW compiles one
// instance of this record into every slot image
// (firmware/SaftyFW/src/update/saftyfw_image_identity_record.c), populated
// from the SAME saftyfw_build_info.h macros that feed the FW_VERSION frame
// -- and link_task.c's FW_VERSION packer reads the record rather than the
// macros, so the two can never disagree by construction. The ESP scans the
// staged image for it (firmware/KilnFW/App/drivers/net/pico_image_source.c)
// and compares the result against what the running Pico reports. No stamped
// constant, no build-order coupling, nothing to keep in sync by hand.
//
// NO FIXED OFFSET, DELIBERATELY. The record is found by scanning for its
// magic words rather than by being placed at a known address by the linker
// script. A fixed offset would mean editing app_slot.ld.in and the
// bootloader's expectations of the slot layout -- a change with real
// bricking risk for a record that is only ever read by a host-side scanner
// that can afford to look. Scanning a 832 KB slot four bytes at a time costs
// milliseconds, once, at boot.
//
// NOT A SECURITY BOUNDARY. Two magic words, a bounded length and an end
// marker make an accidental match essentially impossible, but a hostile
// image could of course claim any identity it likes. The integrity of the
// image itself is established elsewhere and independently: CRC-32 over the
// whole staged image (ota_image_crc.h, checked by SaftyFW's update_task.c
// after writing the slot) and the HMAC on the upload that staged it
// (ota_http.c's X-Ota-Mac). This record answers "which build is this",
// not "may I trust this".
//
// Freestanding C11 per firmware/CommonFW/README.md: no allocation, no I/O,
// no globals, every decoder bounds-checked.
#ifndef KILNLINK_SAFTYFW_IMAGE_IDENTITY_H
#define KILNLINK_SAFTYFW_IMAGE_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "SFID" / a second, arbitrary constant. Two independent 32-bit words, not
 * one: a single 4-byte magic has a real chance of appearing by accident
 * somewhere in ~832 KB of ARM code and rodata, a matching PAIR at the same
 * 4-byte-aligned offset does not. */
#define SAFTYFW_IMAGE_IDENTITY_MAGIC0 0x44494653u /* 'S','F','I','D' little-endian */
#define SAFTYFW_IMAGE_IDENTITY_MAGIC1 0xA5C31E7Bu
/* Trailer, checked after the payload -- a third independent witness that the
 * bytes in between really are this record and not a coincidence. */
#define SAFTYFW_IMAGE_IDENTITY_MAGIC_END 0x7BE1C35Au

/* Bumped only if the layout below changes. A reader that does not recognise
 * the version must treat the record as absent, never guess. */
#define SAFTYFW_IMAGE_IDENTITY_RECORD_VERSION 1u

/* Room for a git short hash with generous headroom (`git rev-parse --short
 * HEAD` produces 7-12 characters in practice; SAFTYFW_GIT_COMMIT can also be
 * the literal "unknown"). Stays well under kilnlink_fw_version.h's 64-byte
 * wire maximum, which is what the ESP compares against. */
#define SAFTYFW_IMAGE_IDENTITY_COMMIT_MAX 40u

/* Fixed layout, little-endian, no padding (every member is naturally
 * aligned and the total is a multiple of 4 -- asserted below). Read out of
 * raw flash bytes by memcpy, never by casting a pointer into a scan buffer. */
typedef struct {
    uint32_t magic0;
    uint32_t magic1;
    uint16_t record_version;
    uint8_t  dirty;      /* 0 or 1 -- SAFTYFW_GIT_DIRTY */
    uint8_t  commit_len; /* <= SAFTYFW_IMAGE_IDENTITY_COMMIT_MAX */
    /* ASCII, zero-padded past commit_len. NOT relied on to be
     * NUL-terminated: commit_len is authoritative, matching the wire
     * contract of kilnlink_fw_version_t's commit[]/commit_len. */
    char     commit[SAFTYFW_IMAGE_IDENTITY_COMMIT_MAX];
    /* SaftyFW's CONFIG_STORE_FORMAT_VERSION at the time this image was
     * built. The ESP needs it for the plan's sec 6 pre-flight chain-gap
     * check: an image whose config format is more than one step ahead of
     * the record the Pico is actually holding cannot carry the CT
     * calibration forward, and must not be pushed. */
    uint16_t config_format_version;
    uint16_t reserved; /* 0 */
    uint32_t magic_end;
} saftyfw_image_identity_t;

#define SAFTYFW_IMAGE_IDENTITY_SIZE 60u

typedef char saftyfw_image_identity_size_check
    [(sizeof(saftyfw_image_identity_t) == SAFTYFW_IMAGE_IDENTITY_SIZE) ? 1 : -1];

/* A reader scanning an image in chunks must overlap consecutive chunks by at
 * least this many bytes, or a record straddling a chunk boundary is missed.
 * (SIZE - 4, because the scan steps 4 bytes at a time: a record whose first
 * byte lands in the previous chunk is still found as long as SIZE-4 of its
 * bytes are re-presented.) */
#define SAFTYFW_IMAGE_IDENTITY_SCAN_OVERLAP (SAFTYFW_IMAGE_IDENTITY_SIZE - 4u)

/* Structural validity: both magics, the trailer, a recognised version, and a
 * commit_len within bounds. Does not judge the CONTENT of commit[] -- an
 * image built from a tree with no git available legitimately says "unknown".
 * NULL-safe. */
bool saftyfw_image_identity_is_valid(const saftyfw_image_identity_t *rec);

/* Scans `buf[0..len)` for the first valid record and copies it to *out.
 *
 * `buf` MUST begin at an offset that is 4-byte aligned WITHIN THE IMAGE, not
 * merely 4-byte aligned in memory: the scan steps in 4-byte increments from
 * buf[0], so a caller whose chunk boundaries drift off a multiple of 4 would
 * step past the record. Every caller in this tree reads the image in
 * fixed-size chunks with a fixed overlap, both multiples of 4.
 *
 * Returns false (leaving *out untouched) if no valid record is present, if
 * `len` is shorter than one record, or on a NULL argument. Pure: no I/O, no
 * statics, no allocation. */
bool saftyfw_image_identity_find(const uint8_t *buf, size_t len, saftyfw_image_identity_t *out);

#ifdef __cplusplus
}
#endif

#endif // KILNLINK_SAFTYFW_IMAGE_IDENTITY_H
