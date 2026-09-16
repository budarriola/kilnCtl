// safety_cfg_http -- docs/COMMISSIONING.md sec 3.1's HTTP surface:
//   GET  /api/safety/commissioning
//   POST /api/safety/commissioning
//   POST /api/safety/commissioning/bench_preset
// Reads/writes through safety_cfg_store.h's cache and safety_link.h's
// SET_PARAM/COMMIT_CONFIG/GET_CONFIG_PAGE wire calls -- this file owns no
// storage of its own.
#ifndef SAFETY_CFG_HTTP_H
#define SAFETY_CFG_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "kiln_package.h" /* kiln_pkg_safety_t -- apply_package_and_confirm()'s pkg */
#include "safety_ceiling_policy.h" /* safety_ceiling_refusal_class_t -- set_and_confirm_f32()'s out_class */
#include "safety_link.h"
#include "kiln_io.h" /* CT_COMMISSIONING_PLAN.md step 2 -- ct_auto_zero_post_handler()'s
                      * relays-off precondition reads kiln_io_relays_off_ms()/
                      * kiln_io_get_relay_shadow() */

#ifdef __cplusplus
extern "C" {
#endif

/* Registers this file's routes on the server wifi_provision_http.c
 * already started -- same "call after the board it needs has come up, any
 * pointer may be NULL" convention as dashboard_http_start(). `link` may be
 * NULL (safety domain not populated this boot); every handler then answers
 * link_up=false / commissioned=false rather than crashing, same as
 * dashboard_http.c's own NULL-pointer handling for an absent board.
 *
 * `io_or_null` (CT_COMMISSIONING_PLAN.md step 2, added 2026-09-06) is used
 * only by POST /api/safety/commissioning/ct_auto_zero's relays-off
 * precondition; NULL there refuses that one action ("board I/O not
 * available this boot") without affecting any other route. */
esp_err_t safety_cfg_http_start(SafetyLinkClass *link_or_null, kiln_io_t *io_or_null);

/* Stages, commits, and CONFIRMS-BY-READBACK a single f32 safety-cfg
 * parameter on the Pico -- the same apply_pairs()/confirm_commit_landed()
 * machinery every other commissioning write in safety_cfg_http.c uses,
 * exposed for safety_ceiling_sync.c (zones_http.c's Pico-ceiling-tracking
 * helper, owner request 2026-09-10) so it never needs a second
 * implementation of "never trust a bare ACK." `link` may be NULL (no safety
 * processor this boot); returns false with a reason in that case, same as
 * every other caller of apply_pairs(). Reports the Pico's ARMED refusal
 * (config writes are refused unconditionally whenever the relay is ARMED --
 * SaftyFW's config_store_decide_write(), no per-field carve-out) verbatim
 * via `reason_out` when that is why it failed -- callers must surface this
 * to the operator, not silently retry or require an undocumented reset.
 *
 * `out_class` (may be NULL) receives the machine-readable classification
 * of the failure -- safety_ceiling_refusal_class_t -- 2026-09-10 opus
 * review finding: control flow (e.g. safety_ceiling_sync.c's reconcile
 * backoff) must never classify a refusal by substring-matching
 * `reason_out`'s prose; it must use this out-of-band, numeric
 * classification instead. Only meaningful when this function returns
 * false. */
bool safety_cfg_http_set_and_confirm_f32(SafetyLinkClass *link, uint16_t param_id, float value,
                                          char *reason_out, size_t reason_cap,
                                          safety_ceiling_refusal_class_t *out_class);

/* Volatile-install sibling of the above -- docs/KILN_PROFILES_PLAN.md item 15
 * (SAFETY_CMD_APPLY_CONFIG_VOLATILE, 0x2D) reached the wire 2026-09-14; this
 * is the ONE caller allowed to use it for the ceiling field, and it exists
 * ONLY for kiln_cfg_swap.c's step 4 ("raise-first"), NEVER for the standing
 * safety_ceiling_sync.c reconcile loop, which keeps calling the FLASH-
 * writing sibling above unchanged. Reason for the split: kiln_cfg_swap.c's
 * owner rule is "the Pico never leaves ARMED, ever" for the whole two-
 * processor transaction -- raising the ceiling via the ordinary flash path
 * would refuse outright while ARMED (the Pico's ordinary running state) and
 * abort the swap before it even reaches the bulk push, exactly the failure
 * mode docs/audits/kiln_swap_transaction_2026-09-14.md documented as "safe
 * but blocks the feature" before item 15 existed. This function can never
 * be refused for ARMED (config_store_write_volatile() never calls config_
 * store_decide_write()) but also never reaches flash, so the raised value
 * does NOT survive a Pico reboot on its own -- kiln_cfg_swap.c's best-effort
 * flash-fallback step (after the whole swap verifies) is what gives it a
 * chance to persist; see that module's own doc comment for the ordering and
 * docs/audits/kiln_swap_volatile_wiring_2026-09-14.md for why a reboot
 * between those two steps is still caught, not silently lost. */
bool safety_cfg_http_set_and_confirm_f32_volatile(SafetyLinkClass *link, uint16_t param_id, float value,
                                                   char *reason_out, size_t reason_cap,
                                                   safety_ceiling_refusal_class_t *out_class);

/* Bulk sibling of the above -- docs/KILN_PROFILES_PLAN.md item 5's two-
 * processor apply transaction (kiln_cfg_swap.c), step 6. Stages every SET
 * entry of `pkg` EXCEPT the ceiling field (SAFETY_PARAM_ID_ABS_MAX_TEMP_C --
 * always driven separately via kiln_cfg_swap.c's own raise-first/lower-last
 * calls, see this function's own .c-file doc comment for why), commits
 * once, and confirms by live read-back exactly like every other write in
 * this file. `link` may be NULL -- returns false, same convention.
 *
 * `volatile_install`: true sends SAFETY_CMD_APPLY_CONFIG_VOLATILE (0x2D,
 * item 15) instead of COMMIT_CONFIG -- installs into the Pico's live RAM
 * record, never refused for ARMED, never reaches flash. kiln_cfg_swap.c
 * passes true for the swap's own forward push and rollback-restore (the
 * owner's "Pico never leaves ARMED" rule) and false only for its own
 * best-effort, allowed-to-fail flash-fallback persist step, run AFTER a
 * swap has already verified -- see docs/audits/kiln_swap_volatile_wiring_
 * 2026-09-14.md for the full ordering and why a Pico reboot between the two
 * is still caught by the existing ceiling/arming divergence check rather
 * than silently lost. */
bool safety_cfg_http_apply_package_and_confirm(SafetyLinkClass *link, const kiln_pkg_safety_t *pkg,
                                                bool volatile_install,
                                                char *reason_out, size_t reason_cap,
                                                safety_ceiling_refusal_class_t *out_class);

/* The recent-ARMED-refusal predicate that used to be declared here moved to
 * safety_cfg_store.h (safety_cfg_store_recent_armed_refusal()) on 2026-09-15
 * -- item G of review_divergence_wiring_60d6552f_2026-09-15.md. Its only
 * reader is safety_ceiling_sync.c, a safety/ module, which had to include
 * this http/ header to reach it. */

#ifdef __cplusplus
}
#endif

#endif // SAFETY_CFG_HTTP_H
