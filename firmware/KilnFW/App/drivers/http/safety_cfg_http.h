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

/* Bulk sibling of the above -- docs/KILN_PROFILES_PLAN.md item 5's two-
 * processor apply transaction (kiln_cfg_swap.c), step 6. Stages every SET
 * entry of `pkg` EXCEPT the ceiling field (SAFETY_PARAM_ID_ABS_MAX_TEMP_C --
 * always driven separately via safety_ceiling_sync_guard_raise()/
 * _apply_lower(), see this function's own .c-file doc comment for why),
 * commits once, and confirms by live read-back exactly like every other
 * write in this file. `link` may be NULL -- returns false, same convention.
 *
 * ITEM 15 NOTE (also in the .c file, repeated here since this is the public
 * contract kiln_cfg_swap.c is written against): as of this function,
 * SaftyFW's volatile RAM-only install is not landed, so the commit this
 * function forces writes FLASH and is refused outright while the Pico is
 * ARMED. Callers must treat that refusal as an ordinary "nothing landed"
 * failure -- not a bug in this function -- and roll back rather than
 * retry-forever or treat it as success. */
bool safety_cfg_http_apply_package_and_confirm(SafetyLinkClass *link, const kiln_pkg_safety_t *pkg,
                                                char *reason_out, size_t reason_cap,
                                                safety_ceiling_refusal_class_t *out_class);

#ifdef __cplusplus
}
#endif

#endif // SAFETY_CFG_HTTP_H
