// pico_auto_update_boot.h -- the boot-time trigger for
// docs/PICO_AUTO_UPDATE_PLAN.md (G3, plan sec 9 step 5): once per boot, ask
// whether the safety processor is running the build this board has staged
// for it, and if not, attempt exactly one update within a persisted,
// read-back-verified budget.
//
// WHAT IT DOES, IN ORDER (all of it on its own task -- see below):
//   1. Describes the staged image (net/pico_image_source.h). No staged image
//      means no expectation, and the whole feature stays INERT this boot:
//      nothing is decided, nothing is blocked. See "feature-inert" below.
//   2. Waits, bounded, for the Pico to report its identity. The link
//      re-requests FW_VERSION every poll period while it has none, so this is
//      a wait, not a poke.
//   3. Calls pico_auto_update_decide() (net/pico_auto_update.h) with live
//      inputs and acts on the answer: MATCH clears the attempt counter,
//      NEEDED makes one attempt, any ABANDONED_* raises the readiness block
//      (net/pico_auto_update_state.h -> safety/readiness_gate.c), and
//      DEFER/LINK_DOWN do nothing at all.
//
// FEATURE-INERT WITHOUT A STAGED IMAGE, DELIBERATELY -- AND THIS IS A
// DEVIATION FROM THE PLAN'S LITERAL WORDING. Plan sec 9 step 5 has the boot
// path always call decide(). If it did that on a board with no staged image,
// decide() would answer ABANDONED_NO_IMAGE (its own step 4), the readiness
// gate would refuse to fire, and since no board ships with a staged image,
// EVERY board would refuse to fire on its first boot after this lands. That
// is the exact trap pico_auto_update.h's own header already warns about
// ("arming it with an empty/placeholder expected commit would make EVERY
// board's boot decide 'abandoned: no image' forever"). Because the expected
// identity now comes FROM the image (G2's design), "no image" means "no
// expectation" -- there is nothing to compare, so there is nothing to
// abandon. ABANDONED_NO_IMAGE stays reachable and keeps its real meaning: an
// image WAS staged and is unusable (failed CRC, or carries no identity).
//
// A TASK, NOT INLINE IN THE BOOT PATH. Waiting for the Pico's FW_VERSION and
// scanning the staged image both take real time; app_main() must not stall
// there. The task ends itself as soon as it has a verdict -- it is not a
// service.
//
// SAFETY PROPERTIES THIS PATH MUST NOT BREAK, and how each is held:
//   * Never update during a firing, or while heat is granted. The decision to
//     proceed goes through ota_http_check_interlocks() (ota_state.h), the
//     SAME seven-check interlock every HTTP update path uses -- not a second
//     rule invented here -- and it is re-checked immediately before claiming
//     the update mutex, not merely at decision time.
//   * The Pico must never end up unarmed. Nothing here erases or modifies the
//     Pico's flash directly: an attempt hands off to ota_pico_relay_start(),
//     the same relay the HTTP path uses, which drives SaftyFW's own two-slot
//     update receiver. A failed or abandoned attempt leaves the Pico running
//     the image it already had, guarding as before.
//   * abs_max_temp_c parity. Untouched here. A Pico reboot into a new image
//     re-runs the existing ceiling-sync reconciliation
//     (safety_ceiling_sync_reconcile_on_link_up()), which is what holds this
//     invariant today and is not bypassed by this path.
//
// RECOVERY MODE. The caller must not start this in recovery mode (see
// boot_guard.h). main_control_bringup() gates the call, matching every other
// subsystem there -- this board has been bricked into a permanent recovery
// loop three times from that area.
#ifndef PICO_AUTO_UPDATE_BOOT_H
#define PICO_AUTO_UPDATE_BOOT_H

#include <stdbool.h>

#include "esp_err.h"

#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the one-shot boot-time evaluation task described above.
 *
 * `link` must be the live SafetyLinkClass safety_link_start() brought up; a
 * NULL link (or a failed safety_link_start()) means there is no way to learn
 * the Pico's identity, so the call is refused with ESP_ERR_INVALID_ARG and
 * nothing is blocked -- an unreachable Pico is the link's own fault to report
 * (S6b), never this feature's to escalate.
 *
 * Returns ESP_OK if the task was created (its verdict arrives later,
 * asynchronously, via pico_auto_update_state_is_blocking()). Safe to call
 * exactly once per boot; a second call while the first task is still running
 * is refused with ESP_ERR_INVALID_STATE.
 *
 * MUST NOT be called in recovery mode -- see this header's top comment. */
esp_err_t pico_auto_update_boot_start(SafetyLinkClass *link);

#ifdef __cplusplus
}
#endif

#endif // PICO_AUTO_UPDATE_BOOT_H
