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
#include "kiln_package.h"
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

/* The three Pico set-and-confirm wrappers that used to be declared here --
 * safety_cfg_http_set_and_confirm_f32(), _f32_volatile() and
 * safety_cfg_http_apply_package_and_confirm() -- MOVED to
 * drivers/safety/safety_cfg_write.h on 2026-09-16, renamed
 * safety_cfg_write_*. Staging, committing and confirming-by-read-back a
 * parameter over the safety link is safety-layer work, not HTTP work; this
 * file is now one of that module's callers, and safety_ceiling_sync.c /
 * kiln_cfg_swap.c reach it without including any http/ header. The `http`
 * in the old names WAS the layering inversion.
 *
 * Body parsing (parse_set_param_body()) stays here; it fills
 * safety_cfg_write.h's safety_cfg_post_pair_t and hands it over. */

/* The recent-ARMED-refusal predicate that used to be declared here moved to
 * safety_cfg_store.h (safety_cfg_store_recent_armed_refusal()) on 2026-09-15
 * -- item G of review_divergence_wiring_60d6552f_2026-09-15.md. Its only
 * reader is safety_ceiling_sync.c, a safety/ module, which had to include
 * this http/ header to reach it. */

#ifdef __cplusplus
}
#endif

#endif // SAFETY_CFG_HTTP_H
