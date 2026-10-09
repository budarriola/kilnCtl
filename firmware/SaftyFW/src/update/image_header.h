// image_header.h -- the header an incoming Pico firmware image carries in
// its SAFETY_CMD_UPDATE_BEGIN (0x10) frame, validated BEFORE a single flash
// sector is erased. TODO.md Phase 10 item 10.8b, CommonFW/docs/
// UPDATE_PROTOCOL.md's "The image must say what it is, before anything is
// erased".
//
// --- A note on reconciling UPDATE_PROTOCOL.md's two field lists ---
//
// That document describes UPDATE_BEGIN's wire payload twice, and the two
// descriptions do not obviously line up: section 4's frame table gives
// "32 B: image length u32, image CRC32, target slot u8, version string 16 B,
// flags", while the later "the image must say what it is" subsection lists
// a different field set (magic, target, header_version, protocol_version,
// min_compatible, length, crc32) with no offsets at all. Nothing has been
// built against either list yet (docs/UPDATE_PROTOCOL.md's own status line:
// "planning, nothing built"), so this file is the one place that actually
// has to pick a single, real layout -- it merges both lists into one 36-byte
// payload, documented field-by-field below. If firmware/KilnFW ever sends a
// real UPDATE_BEGIN frame, THIS layout is what it must match; update
// UPDATE_PROTOCOL.md's frame table to match this file, not the reverse, per
// this codebase's "if this file disagrees with the code, the code wins, fix
// the doc" convention (BOOTLOADER.md's own header comment states the same
// rule for its document).
//
// One deliberate design choice beyond just picking a layout: `requested_slot`
// is carried on the wire but is ADVISORY ONLY. The Pico computes which slot
// it will actually stage into itself (always "whichever slot is not
// currently active", from its own metadata -- see docs/BOOTLOADER.md
// section 2) rather than trusting the ESP's claim. This matches
// UPDATE_PROTOCOL.md section 1's own "the Pico enforces the last three
// [preconditions] itself... for the same reason the whole safety processor
// exists: the ESP is the thing that might be wrong" -- extended here to
// "which slot to write" as well, not just the go/no-go preconditions. A
// mismatch between `requested_slot` and the Pico's own choice is logged, not
// obeyed.
//
// Pure, no RTOS/SDK dependency -- host-testable, same discipline as
// bootloader/metadata.c and src/tasks/link_frame.c.
#ifndef SAFTYFW_UPDATE_IMAGE_HEADER_H
#define SAFTYFW_UPDATE_IMAGE_HEADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_IMAGE_HEADER_MAGIC 0x53414655u // "SAFU" -- distinct from
                                                // BOOTLOADER_METADATA_MAGIC
                                                // ("KLN1") and from kilnlink's
                                                // own frame delimiter/CRC --
                                                // this magic identifies an
                                                // UPDATE_BEGIN payload
                                                // specifically, not a flash
                                                // metadata record or a
                                                // kilnlink wire frame.
#define UPDATE_IMAGE_HEADER_VERSION 1u

// Refuses an ESP image outright (UPDATE_PROTOCOL.md's own example of why
// `target` matters) -- only one value exists today because only one other
// processor exists in this system, but the field is there so a third
// target can be refused just as cleanly later, the same reason kilnlink's
// device-id byte exists rather than assuming exactly two peers forever.
#define UPDATE_IMAGE_TARGET_RP2040 1u

// --- Wire layout, 36 bytes total (comfortably inside the 253-byte payload
// cap, LINK_PROTOCOL.md section 3) ---
//
// Offset  Size  Field
//      0     4  magic               (UPDATE_IMAGE_HEADER_MAGIC)
//      4     1  target              (UPDATE_IMAGE_TARGET_RP2040)
//      5     1  header_version      (UPDATE_IMAGE_HEADER_VERSION)
//      6     2  protocol_version    (u16 LE -- KILNLINK_PROTOCOL_VERSION the new image speaks)
//      8     2  min_compatible      (u16 LE -- the new image's own floor)
//     10     1  requested_slot      (advisory only, see header comment above)
//     11     1  flags               (reserved, 0 today)
//     12     4  length              (u32 LE, bytes of application image)
//     16     4  crc32               (u32 LE, over [0, length) of the image)
//     20    16  version             (ASCII, NOT null-terminated -- same
//                                    convention as CommonFW/docs/
//                                    LINK_PROTOCOL.md's FW_VERSION frame)
//     36  total = UPDATE_IMAGE_HEADER_WIRE_LEN
#define UPDATE_IMAGE_HEADER_WIRE_LEN 36u

typedef struct {
    uint32_t magic;
    uint8_t  target;
    uint8_t  header_version;
    uint16_t protocol_version;
    uint16_t min_compatible;
    uint8_t  requested_slot; // advisory only -- see this file's header comment
    uint8_t  flags;          // reserved, always 0 today
    uint32_t length;
    uint32_t crc32;
    char     version[16]; // ASCII, not null-terminated
} update_image_header_t;

// Packs `hdr` into a UPDATE_IMAGE_HEADER_WIRE_LEN-byte buffer. Pure byte
// layout, no validation of `hdr`'s own field values -- same "this function
// does not decide what any field means" discipline as link_frame.c's pack
// functions. Exists primarily so host tests can round-trip a header without
// hand-assembling bytes; a real sender (firmware/KilnFW, a separate
// codebase) is not built against this function.
void update_image_header_pack(const update_image_header_t *hdr,
                               uint8_t out[UPDATE_IMAGE_HEADER_WIRE_LEN]);

// Unpacks a raw UPDATE_BEGIN payload. Returns false (leaving `*out`
// completely untouched) if `length != UPDATE_IMAGE_HEADER_WIRE_LEN` exactly,
// or if either `payload`/`out` is NULL. Does NOT check magic/target/
// header_version -- this only decodes bytes into fields; call
// update_image_header_validate() afterward to decide whether the decoded
// header is actually acceptable. Untrusted-wire-input discipline, same as
// bootloader/metadata.c's bootloader_metadata_unpack() and src/tasks/
// link_frame.c's link_frame_unpack_context().
bool update_image_header_unpack(const uint8_t *payload, uint8_t length,
                                 update_image_header_t *out);

// Semantic validation, separate from unpack() so a caller can log exactly
// which check failed (magic vs. target vs. length-too-big vs. ...) rather
// than getting one opaque "invalid" bool. Checks, in this order:
//   - magic == UPDATE_IMAGE_HEADER_MAGIC
//   - target == UPDATE_IMAGE_TARGET_RP2040 ("refuses an ESP image outright")
//   - header_version == UPDATE_IMAGE_HEADER_VERSION ("refuse the
//     unrecognised rather than guess", UPDATE_PROTOCOL.md's own phrase for
//     this exact field)
//   - length > 0 (an empty image is never legitimate)
//   - length <= max_length (the caller passes BOOTLOADER_SLOT_FLASH_SIZE --
//     this file does not depend on bootloader/flash_layout.h to stay free of
//     that coupling, so the caller supplies the ceiling)
// Does NOT check protocol_version/min_compatible compatibility -- that is a
// job for src/tasks/link_frame.c's link_frame_versions_compatible(), already
// built and host-tested, which the caller should also call. Returns which
// check failed via `out_reason` (may be NULL if the caller only wants the
// bool).
typedef enum {
    UPDATE_IMAGE_HEADER_OK = 0,
    UPDATE_IMAGE_HEADER_BAD_MAGIC,
    UPDATE_IMAGE_HEADER_BAD_TARGET,
    UPDATE_IMAGE_HEADER_BAD_VERSION,
    UPDATE_IMAGE_HEADER_ZERO_LENGTH,
    UPDATE_IMAGE_HEADER_TOO_LARGE,
} update_image_header_check_t;

update_image_header_check_t update_image_header_validate(const update_image_header_t *hdr,
                                                            uint32_t max_length);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_UPDATE_IMAGE_HEADER_H
