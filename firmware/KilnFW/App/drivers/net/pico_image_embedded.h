// pico_image_embedded.h -- the two SaftyFW slot images embedded directly in
// this ESP application build, and what they say about themselves.
//
// OWNER DECISION 2026-09-20, OVERRIDING docs/PICO_AUTO_UPDATE_PLAN.md sec 2's
// "CORRECTION 2026-09-18" (embedding a SaftyFW binary is not implementable in
// this tree). That correction was accurate for the tree as it stood: the
// ESP-IDF build could not produce a SaftyFW .bin, and none existed to embed
// by hand. Both reasons are now moot -- a parallel SaftyFW-side change adds
// an objcopy POST_BUILD step that emits SaftyFW_slotA.bin/SaftyFW_slotB.bin
// (see firmware/SaftyFW/test/check_00_saftyfw_target_build.ps1), and
// App/drivers/CMakeLists.txt now embeds both via EMBED_FILES, refusing to
// configure if either is missing (its own comment there names the exact
// build step to run first).
//
// WHY BOTH SLOTS, NOT ONE. flash_layout.h's two application slots are
// POSITION-DEPENDENT: slot A code and slot B code are different bytes even
// though they are the same source commit, because each is linked to run at
// its own base address. Sending slot A's bytes to whichever slot the Pico's
// bootloader actually selects for the incoming write is only correct half
// the time by construction -- see pico_update_attempts_next_slot()'s header
// comment for how the ESP compensates for not being able to ask the Pico
// which slot it will choose.
//
// WHY THE TWO EMBEDDED IMAGES ARE CROSS-CHECKED FOR ONE IDENTITY, NOT TWO.
// Both slot binaries are produced from the SAME SaftyFW commit in the same
// build (see the ONE check_00_saftyfw_target_build.ps1 invocation the
// FLASH_BUDGET.md / build instructions describe), so their embedded
// saftyfw_image_identity_t records must agree on every field this module
// reads (commit, commit_len, dirty, config_format_version,
// link_protocol_version) -- the fields the identity record itself defines,
// as opposed to placement/padding bytes around the record that this module
// never inspects.
// A mismatch here is not
// a "which one do we trust" question -- it means the two .bin files landed
// in this build from two DIFFERENT SaftyFW builds (a stale artifact left
// over from an earlier build, a build system race, a hand-copied file from
// the wrong worktree), which is exactly the "fleet-wide-refusal trap"
// pico_auto_update.h's ABANDONED_NO_IMAGE outcome exists to catch rather
// than silently push a Frankenstein pair at the Pico. See this header's
// pico_image_embedded_describe() for what it logs when this happens.
//
// STALE-EMBEDDED-IMAGE HAZARD (real once embedding lands, described here
// for the tooling agent that orders phase 1 of run_all_checks.ps1 and the
// tools/PcTools flash pipeline, since this file cannot fix it from inside
// itself): with an embedded image, PICO_AUTO_UPDATE_IMAGE_AVAILABLE is true
// on EVERY board that boots this ESP application, unconditionally -- unlike
// the manifest-staged path (pico_image_source.h) that stayed inert until
// someone POSTed an image to a specific board. If the SaftyFW binary that
// gets embedded is older than the SaftyFW commit actually intended for this
// release (a stale build directory, a build ordered before a SaftyFW source
// change lands, a worktree whose SaftyFW/build/ was not rebuilt), then EVERY
// board running this ESP build will decide NEEDED at boot and attempt to
// push that stale image -- the exact fleet-wide scenario
// docs/PICO_AUTO_UPDATE_PLAN.md sec 9 step 3's note warns about, except now
// reachable because an image is always present. What the tooling agent
// should check before this ESP build is ever flashed to more than a bench
// unit: (1) firmware/SaftyFW/build/'s SaftyFW_slotA.elf/.bin build timestamp
// is newer than the last SaftyFW source commit intended for this release,
// not merely present; (2) the identity commit embedded in the resulting
// KilnCtrl.bin (readable with pico_image_embedded's own scan logic, or by
// grepping the .bin for the SFID magic and decoding by hand) matches
// `git rev-parse HEAD` in firmware/SaftyFW/ at the time KilnFW was built,
// not merely "a" SaftyFW build; (3) phase 1 of run_all_checks.ps1 builds
// SaftyFW before KilnFW when both run concurrently -- see this pass's
// handback for the observed race and how it was worked around.
#ifndef PICO_IMAGE_EMBEDDED_H
#define PICO_IMAGE_EMBEDDED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kilnlink/kilnlink_version.h" /* KILNLINK_PROTOCOL_VERSION, for
                                        * pico_image_embedded_protocol_ok() below */
#include "kilnlink/saftyfw_image_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PICO_IMAGE_EMBEDDED_REASON_MAX 128

typedef struct {
    /* True only when BOTH embedded slot images carry a readable identity
     * record AND the two records agree on commit/dirty/config_format_version/
     * link_protocol_version.
     * This is the sole gate for "may this be used as an update source" --
     * see this header's top comment for why disagreement is treated as
     * unusable rather than "trust slot A". */
    bool usable;

    /* The shared identity, valid only when usable. NUL-terminated here
     * (unlike the wire and the record itself) since every consumer on this
     * side treats it as a C string. */
    char     commit[SAFTYFW_IMAGE_IDENTITY_COMMIT_MAX + 1u];
    uint8_t  commit_len;
    bool     dirty;
    uint16_t config_format_version;
    uint16_t link_protocol_version;

    /* Raw bytes of each embedded slot, for pico_img_stage.h's writer.
     * Always populated (even when !usable) so a caller can still log sizes;
     * never dereference when the corresponding *_found is false. */
    const uint8_t *slot_data[2]; /* [0] = slot A, [1] = slot B */
    uint32_t       slot_len[2];
    bool           slot_found[2]; /* this slot's own identity record was readable */

    /* Why usable is false, when it is. Empty otherwise. */
    char reason[PICO_IMAGE_EMBEDDED_REASON_MAX];
} pico_image_embedded_info_t;

/* Fills *out by scanning both embedded slot images directly in flash-mapped
 * .rodata -- no partition read, no copy, no static scan buffer (unlike
 * pico_image_source.c's manifest path): the bytes are already addressable
 * memory, so saftyfw_image_identity_find() runs once per slot over the whole
 * range in one call. Cheap enough to call from the boot task directly.
 *
 * Always returns true (there is always an answer, even "not usable, see
 * reason") -- the return value exists only for the NULL-safety convention
 * this codebase's other *_describe() functions share; check ->usable, not
 * the return value. NULL-safe: a NULL out returns false and does nothing. */
bool pico_image_embedded_describe(pico_image_embedded_info_t *out);

/* Review finding D4: whether the boot path should try to use *emb as its
 * update source at all. `usable` alone is not enough -- a dirty embedded
 * image can never be confirmed as matching by
 * pico_auto_update_identity_matches() (pico_auto_update.h, which requires
 * the Pico's OWN observed dirty flag to read 0), so attempting an update
 * against one would burn the 3-attempt budget every boot for no possible
 * gain (2026-09-20 owner decision, option c: a spent budget is now a
 * non-blocking /readiness warning rather than a permanent firing refusal,
 * but it is still a pointless, unwinnable retry loop worth avoiding here
 * rather than merely tolerating). Pure and NULL-safe (NULL reads as not
 * usable) so it is host-testable without pulling in the ESP-IDF task in
 * pico_auto_update_boot.c. */
/* Reviewer advisory (a), 2026-09-22: ota_http_pico.c's manual-upload path
 * refuses to relay a staged Pico image whose declared link_protocol_version
 * disagrees with this ESP binary's own KILNLINK_PROTOCOL_VERSION (that
 * file's own comment has the "0 means unknown, never a mismatch" rule this
 * mirrors). The equivalent gate did not exist on this, the embedded/boot-time
 * path -- that gap rested on an assumption (stated in ota_http_pico.c's own
 * comment) that the embedded pair is always built from the same commit as
 * this ESP binary, so no version skew is possible there. That assumption
 * does not hold for a `flash_firmware(kiln_fw_root=...)` build from a
 * separate worktree, nor for a stale SaftyFW build directory embedded into
 * an otherwise-fresh KilnFW build: both can leave the embedded pair's
 * declared protocol version disagreeing with this ESP binary's own, despite
 * the two slots agreeing with EACH OTHER (idents_agree(), which this
 * function does not duplicate -- it runs first, inside describe_from()).
 * Pure and NULL-safe, same convention as pico_image_embedded_should_use()
 * below, so it is host-testable without ESP-IDF. */
static inline bool pico_image_embedded_protocol_ok(const pico_image_embedded_info_t *emb)
{
    if (emb == NULL || !emb->usable) {
        return true; /* not usable for other reasons -- should_use() already refuses it */
    }
    if (emb->link_protocol_version == 0u) {
        return true; /* unknown (pre-field build); never treated as a mismatch */
    }
    return emb->link_protocol_version == (uint16_t)KILNLINK_PROTOCOL_VERSION;
}

static inline bool pico_image_embedded_should_use(const pico_image_embedded_info_t *emb)
{
    return emb != NULL && emb->usable && !emb->dirty && pico_image_embedded_protocol_ok(emb);
}

/* The pure, freestanding-C11 core of pico_image_embedded_describe(): does the
 * actual identity scan/cross-check over two caller-supplied buffers, with no
 * dependency on the linker-generated EMBED_FILES symbols or on ESP-IDF.
 * pico_image_embedded_describe() is a thin wrapper over this that supplies
 * the real embedded slot pointers; host tests
 * (test_pico_image_embedded.c) call this directly with synthetic buffers,
 * the same split other ESP-IDF-adjacent modules in this tree use to stay
 * host-testable (see pico_update_attempts.c's own #include-the-.c-directly
 * convention for the sibling half of that pattern). */
bool pico_image_embedded_describe_from(const uint8_t *slot_a, uint32_t slot_a_len,
                                       const uint8_t *slot_b, uint32_t slot_b_len,
                                       pico_image_embedded_info_t *out);

#ifdef __cplusplus
}
#endif

#endif // PICO_IMAGE_EMBEDDED_H
