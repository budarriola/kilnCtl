// pico_image_source.h -- "what SaftyFW image, if any, could this board push
// at the Pico right now, and what build is it?"
//
// This is the ESP side of docs/PICO_AUTO_UPDATE_PLAN.md G1/G2. It answers
// both halves of the out-of-date question's premise from ONE source of
// truth: the bytes actually sitting in the `pico_img` staging partition.
//
//   - Is there an image?  persist/pico_image_manifest.h says one was staged,
//     and a full re-read of the partition confirms the bytes still hash to
//     the CRC-32 that was recorded for them.
//   - Which build is it?  kilnlink/saftyfw_image_identity.h's record, found
//     by scanning those same verified bytes.
//
// WHY THE EXPECTED IDENTITY COMES FROM THE IMAGE RATHER THAN A CONSTANT.
// pico_auto_update.h's decision needs an `expected_commit`. Nothing in this
// tree could honestly stamp one at ESP build time (see
// persist/pico_image_manifest.h's header for the three reasons the plan's
// embed-the-image shape is not implementable here), and a WRONG expected
// commit is not a harmless placeholder: every board would resolve to a
// permanent mismatch it can never satisfy, which the readiness gate turns
// into a fleet-wide refusal to fire. Reading the expectation out of the
// image makes it impossible to expect a build that does not exist -- if
// there is no image, there is no expectation, and the updater has nothing to
// say (see pico_auto_update_boot.h's "feature-inert" rule).
//
// COST. Describing the source reads the whole staged image once (~200 KB
// today) to re-verify its CRC and scan for the identity record. That is
// deliberate and happens once, on the boot-time path, off the critical path
// of anything: trusting a stored CRC without re-reading would not
// distinguish "the image that was staged" from "a partition that has since
// been erased, half-rewritten, or never written at all", which is precisely
// the distinction that keeps the updater from pushing garbage at the
// processor whose job is to cut power.
#ifndef PICO_IMAGE_SOURCE_H
#define PICO_IMAGE_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "kilnlink/saftyfw_image_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Human-readable "why not" text, sized like ota_interlock.h's
 * OTA_INTERLOCK_REASON_MAX so it can be logged and carried the same way. */
#define PICO_IMAGE_SOURCE_REASON_MAX 96

typedef struct {
    /* A manifest exists: an image WAS staged at some point. False means the
     * feature is simply not in play on this board (nobody has ever posted a
     * SaftyFW image to it), which is not a fault and must never block
     * anything. */
    bool manifest_present;

    /* Recorded at stage time. Meaningful only when manifest_present. */
    uint32_t image_length;
    uint32_t image_crc32;

    /* The image is present, its bytes still match image_crc32, AND it
     * carries a readable identity record. Only a usable image may be
     * pushed. A manifest_present image that is NOT usable is the real,
     * reachable meaning of pico_auto_update.h's ABANDONED_NO_IMAGE: someone
     * staged something and it cannot be used. */
    bool usable;

    /* The image's own declared build identity. Valid only when `usable`.
     * `commit` is NUL-terminated here (unlike the wire and the record
     * itself) because every consumer on this side treats it as a C string;
     * commit_len is still carried for callers that hash or compare by
     * length. */
    char     commit[SAFTYFW_IMAGE_IDENTITY_COMMIT_MAX + 1u];
    uint8_t  commit_len;
    bool     dirty;
    uint16_t config_format_version;

    /* Why `usable` is false, when it is. Empty string otherwise. */
    char reason[PICO_IMAGE_SOURCE_REASON_MAX];
} pico_image_source_info_t;

/* Fills *out. Returns the value of out->manifest_present for convenience.
 *
 * Reads flash and takes tens of milliseconds; call it from a task, never
 * from an ISR or while holding a module lock. Not reentrant -- it uses a
 * static scan buffer, since the read buffer plus the required inter-chunk
 * overlap is far too large to sit on the caller's stack (this tree's
 * standing rule about big locals, and the reason the sole caller is a task
 * of its own). There is exactly one caller (pico_auto_update_boot.c) and it
 * calls this once per boot.
 *
 * NULL-safe: a NULL `out` returns false and does nothing. */
bool pico_image_source_describe(pico_image_source_info_t *out);

#ifdef __cplusplus
}
#endif

#endif // PICO_IMAGE_SOURCE_H
