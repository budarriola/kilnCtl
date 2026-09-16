// safety_cfg_write -- the ESP-side WRITER half of the Pico safety-parameter
// mirror (safety_cfg_store.h is the reader half). Stage (SET_PARAM), commit
// (COMMIT_CONFIG, or SAFETY_CMD_APPLY_CONFIG_VOLATILE for the RAM-only
// install), then CONFIRM BY FORCED LIVE READ-BACK -- "never trust a bare
// ACK" -- plus the out-of-band numeric classification of a refusal.
//
// Lives in drivers/safety/, NOT drivers/http/, since 2026-09-16: writing a
// safety parameter over the safety link is safety-layer work. http/
// (safety_cfg_http.c) is a CALLER of this module; it parses request bodies
// into safety_cfg_post_pair_t and hands them here. See safety_cfg_write.c's
// own header comment for the layering inversion this closed and the
// zeroed-ceiling defect that inversion produced.
//
// Nothing here may take an httpd_req_t or depend on any HTTP header.
#ifndef SAFETY_CFG_WRITE_H
#define SAFETY_CFG_WRITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kiln_package.h"          /* kiln_pkg_safety_t -- apply_package_and_confirm()'s pkg */
#include "kilnlink/kilnlink_param_value.h" /* kilnlink_param_value_t */
#include "safety_ceiling_policy.h" /* safety_ceiling_refusal_class_t */
#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One operator-submitted (id, raw text value) pair, before type-checking --
 * safety_cfg_http.c's parse_set_param_body() fills these in the order they
 * appeared in the body; type lookup and numeric parsing happen in
 * safety_cfg_write_apply_pairs(), not there, so that struct/parser stays
 * reusable for the bench-preset path's own, differently-sourced pairs. */
typedef struct {
    uint16_t param_id;
    char value_text[24]; /* generous over the longest legal u16/f32 literal */
} safety_cfg_post_pair_t;

#define SAFETY_CFG_POST_MAX_PAIRS SAFETY_CFG_PARAM_COUNT

/* Parses one pair's text value against `type` into `out`. Returns false (out
 * untouched) on a malformed number or a bool/enum value that fails
 * strtoul/strtof's own parse -- range-checking beyond "fits the wire type" is
 * deliberately NOT done here: that cross-field/enum-range validation is
 * COMMIT_CONFIG's job on the Pico (COMMISSIONING.md sec 2), and duplicating a
 * second, possibly-diverging range check here would be exactly the kind of
 * two-definitions-of-valid this codebase's other stores avoid. */
bool safety_cfg_write_parse_value_for_type(const char *text, uint8_t type, kilnlink_param_value_t *out);

/* Stage `n_pairs` pairs, optionally commit, and confirm by forced live
 * read-back. This is the wrapper every httpd-worker call site uses: always
 * the BLOCKING refetch. `link` may be NULL -- returns false with a reason.
 * `out_class` (may be NULL) receives the numeric refusal classification;
 * control flow must classify a refusal by that, never by substring-matching
 * `reason_out`'s prose. */
bool safety_cfg_write_apply_pairs(SafetyLinkClass *link, const safety_cfg_post_pair_t *pairs, int n_pairs,
                                   bool commit, char *reason_out, size_t reason_cap,
                                   safety_ceiling_refusal_class_t *out_class);

/* Stages, commits, and CONFIRMS-BY-READBACK a single f32 safety-cfg
 * parameter on the Pico -- the same machinery every commissioning write
 * uses, so safety_ceiling_sync.c never needs a second implementation of
 * "never trust a bare ACK." `link` may be NULL (no safety processor this
 * boot); returns false with a reason in that case. Reports the Pico's ARMED
 * refusal (config writes are refused unconditionally whenever the relay is
 * ARMED -- SaftyFW's config_store_decide_write(), no per-field carve-out)
 * verbatim via `reason_out` when that is why it failed -- callers must
 * surface this to the operator, not silently retry or require an
 * undocumented reset.
 *
 * `out_class` (may be NULL) receives the machine-readable classification of
 * the failure -- safety_ceiling_refusal_class_t -- 2026-09-10 opus review
 * finding: control flow (e.g. safety_ceiling_sync.c's reconcile backoff)
 * must never classify a refusal by substring-matching `reason_out`'s prose;
 * it must use this out-of-band, numeric classification instead. Only
 * meaningful when this function returns false. */
bool safety_cfg_write_set_and_confirm_f32(SafetyLinkClass *link, uint16_t param_id, float value,
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
bool safety_cfg_write_set_and_confirm_f32_volatile(SafetyLinkClass *link, uint16_t param_id, float value,
                                                    char *reason_out, size_t reason_cap,
                                                    safety_ceiling_refusal_class_t *out_class);

/* Bulk sibling of the above -- docs/KILN_PROFILES_PLAN.md item 5's two-
 * processor apply transaction (kiln_cfg_swap.c), step 6. Stages every SET
 * entry of `pkg` EXCEPT the ceiling field (SAFETY_PARAM_ID_ABS_MAX_TEMP_C --
 * always driven separately via kiln_cfg_swap.c's own raise-first/lower-last
 * calls, see this function's own .c-file doc comment for why), commits
 * once, and confirms by live read-back exactly like every other write here.
 * `link` may be NULL -- returns false, same convention.
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
bool safety_cfg_write_apply_package_and_confirm(SafetyLinkClass *link, const kiln_pkg_safety_t *pkg,
                                                 bool volatile_install,
                                                 char *reason_out, size_t reason_cap,
                                                 safety_ceiling_refusal_class_t *out_class);

#ifdef __cplusplus
}
#endif

#endif // SAFETY_CFG_WRITE_H
