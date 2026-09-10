#ifndef SAFETY_CEILING_SYNC_H
#define SAFETY_CEILING_SYNC_H

#include <stdbool.h>
#include <stddef.h>

#include "safety_ceiling_policy.h"
#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ESP-side glue between safety_ceiling_policy.c's pure decision logic and
 * the real Pico link (safety_cfg_http.c's set+commit+confirm wrapper,
 * safety_cfg_store.c's cached abs_max_temp_c). See safety_ceiling_policy.h
 * for the invariant and ordering rule this implements; see zones_http_post.c
 * for the two call sites (before and after the zone config commit). */

/* param_id 0x0104 -- abs_max_temp_c, safety_cfg_store.c's
 * SAFETY_CFG_PARAM_TABLE. Duplicated here (not #included from that table)
 * deliberately: this file only needs the one numeric id, not the whole
 * table, and the table's own header comment already treats ids as
 * permanent wire constants safe to reference by literal elsewhere in this
 * codebase (e.g. SAFETY_PARAM_ID_ESTOP_ACTIVE_LEVEL in safety_cfg_http.c
 * follows the identical convention). */
#define SAFETY_PARAM_ID_ABS_MAX_TEMP_C 0x0104u

/* Call BEFORE committing a proposed new zones_cfg_t -- i.e. before the
 * line that overwrites the live config (zones_http_post.c's `s_zones.cfg =
 * tmp;`). `link` may be NULL (no safety processor this boot): treated as
 * "no invariant to maintain," always returns true, *out_result = NONE.
 *
 * Returns true iff the caller's commit may proceed (see safety_ceiling_
 * policy_guard_raise()'s own contract -- this is a thin wrapper around it).
 * On false, `reason_out` names why and the caller MUST reject the whole
 * zones POST without applying `new_max_temp_c` or anything else from that
 * submission.
 *
 * `out_refusal_class` (may be NULL) receives the machine-readable
 * classification of a failed raise -- safety_ceiling_refusal_class_t, see
 * safety_ceiling_policy.h. zones_http_post.c/backup_import.c's callers
 * pass NULL (they only need the human-readable `reason_out`); the
 * reconcile path (safety_ceiling_sync_reconcile_on_link_up() below) is the
 * one caller that needs it, to drive its backoff without parsing prose. */
bool safety_ceiling_sync_guard_raise(SafetyLinkClass *link, const float *new_max_temp_c, size_t n,
                                      safety_ceiling_sync_result_t *out_result, char *reason_out,
                                      size_t reason_cap, safety_ceiling_refusal_class_t *out_refusal_class);

/* Call AFTER a zones_cfg_t has already been committed. Best-effort --
 * never blocks, never reports a failure the caller must act on (see
 * safety_ceiling_policy_apply_lower()'s own contract). `link` may be NULL,
 * same as the guard above. */
void safety_ceiling_sync_apply_lower(SafetyLinkClass *link, const float *new_max_temp_c, size_t n,
                                      safety_ceiling_sync_result_t *out_result, char *reason_out,
                                      size_t reason_cap);

/* Reads the Pico's currently-cached abs_max_temp_c (safety_cfg_store.c's
 * live cache -- populated by GET_CONFIG_PAGE fetches, refreshed by every
 * safety_poll_task tick and by this file's own confirm-by-readback writes).
 * Returns false (out_value untouched) if the cache has never held a set
 * value for this param -- e.g. the Pico has never been fetched from this
 * boot, or from a build old enough not to report it. Exposed for the
 * zones/safety config pages to display "the Pico's ceiling currently
 * tracks: N C" alongside the ESP's own zone maximum. */
bool safety_ceiling_sync_get_current_pico_ceiling(float *out_value);

/* 2026-09-10 opus review: guard_raise()/apply_lower() above only run from
 * zones_http_post.c's POST handler, so a board that boots (or reconnects)
 * with `link == NULL` -- and every path that changes the ESP's own zone
 * max_temp_c other than that one POST handler (backup_import.c's restore
 * path being the concrete example that motivated this) -- can leave the
 * ESP's zone ceiling ABOVE the Pico's abs_max_temp_c with nothing to
 * notice or fix it, because guard_raise() treats `link == NULL` as "no
 * invariant to maintain" rather than deferring the check. The ESP's zone
 * config persists in NVS across a Pico that gets replaced, reflashed, or
 * simply comes up later than the ESP's own boot; only the ESP side knows
 * what the ceiling SHOULD be, so reconciliation has to be driven from
 * here, not the Pico.
 *
 * Call this on every safety_poll_task() tick while the link is up (same
 * call site and same level-triggered discipline as safety_sync_tc_type()
 * in safety_link_poll.c -- see the comment at its call site for why this
 * is level-triggered rather than gated on the down->up edge alone: a
 * down_logged-gated call would miss the very first tick after boot, when
 * the link can come up before ever having been observed down). It reads
 * the live zones_cfg_t, builds the same per-zone max_temp_c array
 * zones_http_post.c's handler would, and runs it through safety_ceiling_
 * sync_guard_raise(): a no-op if the Pico's ceiling (kept current by
 * safety_cfg_store.c's own refetch) is already wide enough -- the ordinary
 * case on every tick of a healthy link -- otherwise a real raise+confirm
 * write, logged either way. Never blocks heat or fails the boot: a failed
 * reconcile here is logged and left for the next tick or the next
 * interactive zones POST to retry, same "never silently drop it, keep
 * retrying" convention as safety_sync_tc_type(). Requires zones_config_
 * is_valid() internally, same gate as that function, for the same reason
 * (no fabricated ceiling from a zeroed default config). */
void safety_ceiling_sync_reconcile_on_link_up(SafetyLinkClass *link);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_CEILING_SYNC_H */
