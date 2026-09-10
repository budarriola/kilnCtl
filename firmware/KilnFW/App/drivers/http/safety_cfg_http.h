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
 * to the operator, not silently retry or require an undocumented reset. */
bool safety_cfg_http_set_and_confirm_f32(SafetyLinkClass *link, uint16_t param_id, float value,
                                          char *reason_out, size_t reason_cap);

#ifdef __cplusplus
}
#endif

#endif // SAFETY_CFG_HTTP_H
