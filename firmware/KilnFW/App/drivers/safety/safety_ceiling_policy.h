#ifndef SAFETY_CEILING_POLICY_H
#define SAFETY_CEILING_POLICY_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Owner request 2026-09-10: "if i change the max temp in the web gui it
 * should change it in the pico too." Today the ESP's per-zone
 * `max_temp_c` (zones_cfg_t) and the Pico's own independent absolute
 * ceiling (`abs_max_temp_c`, safety_cfg_store.h param id 0x0104) are
 * completely unrelated numbers that happen to start out equal (80 C).
 * Raising the ESP's limit to fire hotter silently leaves the Pico's
 * ceiling behind, so the Pico trips as soon as the kiln passes the OLD
 * limit -- fail-safe, but a trap for an operator who did the sensible
 * thing in the UI and only found out at the kiln.
 *
 * STANDING INVARIANT, never to be violated: the Pico's ceiling must never
 * be TIGHTER than the highest ESP zone ceiling. It is the second set of
 * eyes -- a previous proposal to make it tighter was an explicit owner
 * rejection (see feedback_abs_max_same_or_looser.md). That invariant
 * dictates the ORDER of operations for any change that keeps it true:
 *
 *   RAISING (new zone max > current Pico ceiling): the Pico MUST be
 *   raised FIRST and its write CONFIRMED (read back, not just ACKed --
 *   see safety_cfg_store's own "ok cannot fail" audit trail) before the
 *   ESP's zone config is allowed to commit. Reversing this order would
 *   let the ESP briefly claim a ceiling the Pico has not actually agreed
 *   to enforce yet -- exactly the window this invariant exists to close.
 *
 *   LOWERING (new zone max < current Pico ceiling): the ESP change is
 *   applied FIRST; the Pico's ceiling is only tightened afterward, on a
 *   best-effort basis. Reversing THIS order would (briefly, or
 *   permanently if the Pico write then fails) put the Pico's ceiling
 *   BELOW the ESP's current live max -- itself a hazard (Pico trips a
 *   temperature the ESP legitimately intends to reach) and, more to the
 *   point, exactly the "make the Pico's limit tighter than the ESP's"
 *   shape the standing rejection above already ruled out. A lowering
 *   Pico write that fails or is refused is NOT a safety hazard (the Pico
 *   ceiling merely stays wider than the current zone max, which is a
 *   compliant state) and must never block the ESP-side change.
 *
 * This file is the PURE decision logic only -- no wire I/O, no HTTP, no
 * FreeRTOS -- host-testable exactly like config_store.c/config_params.c
 * split their own pure logic from *_flash.c's/link_task.c's hardware
 * plumbing. safety_cfg_http.c (ESP side) supplies the actual
 * set-param+commit+confirm write via the writer callback below; the real
 * SaftyFW `config_store_write()` refuses UNCONDITIONALLY whenever the
 * relay is ARMED (config_store.c: `armed ? REFUSED_ARMED : OK`, no
 * per-field carve-out) -- and RELAY_OWNER_STATE_ARMED is the Pico's
 * ordinary STANDING state (everything past the post-boot/post-trip-clear
 * grace window), not merely "mid-firing". So a RAISE attempt made while
 * the Pico is armed -- which is most of the time on a bench that has been
 * up for more than a minute -- is EXPECTED to fail, not a rare edge case:
 * see safety_ceiling_policy_guard_raise()'s doc comment for how the ESP
 * side must surface that to the operator instead of silently requiring a
 * safety-processor reset. */

/* Owner's standing "profile target vs configured limit" convention
 * elsewhere in this codebase (dashboard warning, a9abd273) uses 5 C of
 * headroom between an intended target and a hard ceiling. The Pico's
 * absolute ceiling is one layer further out than that: it must clear not
 * just the profile's intended target but the ESP's OWN zone ceiling
 * (which is itself already above any sane profile target). Adding the
 * SAME 5 C headroom here, on top of the ESP's zone ceiling rather than on
 * top of a profile target, keeps the two ceilings from tripping at
 * effectively the same instant on ordinary sensor noise/offset -- the
 * Pico is meant to catch the ESP having ALREADY failed to limit, not to
 * race it. Using the same constant (rather than inventing a second
 * number) also means one already-reviewed convention governs both
 * gaps, instead of two headroom values that could drift apart. */
#define SAFETY_CEILING_HEADROOM_C 5.0f

/* Computes the Pico ceiling TARGET this policy wants for a given set of
 * zone `max_temp_c` values (`n` entries, one per configured zone).
 *
 * A zone whose max_temp_c is <= 0 does NOT participate in the maximum.
 * This is deliberate, not an oversight: the 2026-08-27 audit
 * (project_zone_band_zero_is_default_sentinel) established that 0 is NOT
 * a "use the firmware default" sentinel for this field -- there is no
 * repo-established safe absolute-temperature default, and
 * profile_executor_start.c already refuses to run any zone whose ceiling
 * is <= 0. A zero-ceiling zone therefore can never actually fire, so it
 * must never be allowed to drag the Pico's ceiling down (or up) on its
 * behalf; it is excluded from the maximum entirely, exactly as if it
 * were not configured at all.
 *
 * Returns 0.0f (never a real target) if NO zone has a positive
 * max_temp_c -- callers must treat that as "no ceiling opinion", i.e
 * leave the Pico's abs_max_temp_c untouched, never write a manufactured
 * default in its place (same "do not invent a default" rule the zero-
 * zone case above follows). Otherwise returns
 * `max(max_temp_c[i] for max_temp_c[i] > 0) + SAFETY_CEILING_HEADROOM_C`. */
float safety_ceiling_policy_target_c(const float *max_temp_c, size_t n);

/* Outcome of a sync attempt, reported back to the HTTP layer so it can
 * build an honest response (never "ok" for a raise that did not land). */
typedef enum {
    SAFETY_CEILING_SYNC_NONE = 0,   /* no valid target, or target already satisfied -- nothing written */
    SAFETY_CEILING_SYNC_RAISED,     /* Pico ceiling written and confirmed, strictly higher than before */
    SAFETY_CEILING_SYNC_LOWERED,    /* Pico ceiling written and confirmed, strictly lower than before */
    SAFETY_CEILING_SYNC_RAISE_FAILED, /* a raise was needed and the Pico write did not confirm -- BLOCKING */
    SAFETY_CEILING_SYNC_LOWER_FAILED, /* a lower was attempted and failed -- NON-blocking, Pico stays wider */
} safety_ceiling_sync_result_t;

/* Writes (stage + commit + confirm-by-readback) a single float parameter
 * to the Pico and reports whether it is now CONFIRMED to hold that exact
 * value -- the same contract safety_cfg_http.c's confirm_commit_landed()
 * already enforces for every other commissioning field (never trust a
 * bare ACK). `reason_out` must be filled with a human-readable outcome on
 * failure (including, verbatim or paraphrased, the ARMED refusal --
 * commit_reject_reason_words()'s "relay is ARMED -- config writes are
 * refused while ARMED" -- when that is why it failed, since that is the
 * single most likely and most operator-actionable failure mode for this
 * particular field, see this header's own top comment). */
typedef bool (*safety_ceiling_writer_fn)(void *ctx, float target_c, char *reason_out, size_t reason_cap);

/* Call BEFORE committing a proposed new zone configuration. Computes the
 * new target from `new_max_temp_c`/`new_n` and compares it against the
 * Pico's own current ceiling (`current_pico_ceiling_c`; pass 0.0f and
 * `current_known=false` if it has never been fetched this boot -- see
 * safety_cfg_store_fetched_ms_ago()'s own "never fetched" contract --
 * which this function treats as "assume the worst, i.e. that the Pico's
 * ceiling might be lower than the new target" so it does not skip a
 * needed raise just because the current value is unknown).
 *
 *   - No valid new target (every zone's max_temp_c <= 0): returns true,
 *     *out_result = SAFETY_CEILING_SYNC_NONE. The caller's zone commit
 *     may proceed unconditionally -- there is nothing to raise or guard.
 *   - New target <= current (no raise needed, including "unknown, but
 *     the new target is 0 so it doesn't matter"): returns true,
 *     *out_result = SAFETY_CEILING_SYNC_NONE. `writer` is NOT called --
 *     lowering (if any) is a separate, POST-commit, best-effort step
 *     (safety_ceiling_policy_apply_lower() below), never gated here.
 *   - New target > current (a raise is needed): calls `writer` with the
 *     new target. On confirmed success, returns true, *out_result =
 *     SAFETY_CEILING_SYNC_RAISED -- the caller's zone commit may now
 *     proceed, since the Pico has already agreed to the wider ceiling.
 *     On failure, returns FALSE, *out_result =
 *     SAFETY_CEILING_SYNC_RAISE_FAILED, and `reason_out` carries the
 *     writer's own reason -- the caller MUST NOT commit the proposed
 *     zone change in this case: doing so would let the ESP claim a
 *     ceiling higher than the Pico has agreed to enforce, exactly the
 *     window the ordering rule in this header's top comment exists to
 *     close. This is the expected, common outcome whenever the Pico is
 *     in its ordinary ARMED state (see this header's top comment) -- the
 *     caller must surface `reason_out` to the operator directly, not log
 *     it and proceed, and not silently require a safety-processor reset
 *     without saying so. */
bool safety_ceiling_policy_guard_raise(float current_pico_ceiling_c, bool current_known,
                                        const float *new_max_temp_c, size_t new_n,
                                        safety_ceiling_writer_fn writer, void *writer_ctx,
                                        safety_ceiling_sync_result_t *out_result, char *reason_out,
                                        size_t reason_cap);

/* Call AFTER a zone configuration has already been committed (whether or
 * not safety_ceiling_policy_guard_raise() was involved -- calling this
 * when a raise just happened is harmless: the new target already equals
 * or is below current, so it is a no-op). Best-effort only: a failure
 * here (most commonly the Pico being ARMED, same as the raise path) is
 * reported via *out_result/reason_out for logging/operator visibility,
 * but NEVER treated as an error the caller must act on -- the invariant
 * stays satisfied either way, since the Pico's ceiling is only ever left
 * WIDER than required, never tighter, when this fails. */
void safety_ceiling_policy_apply_lower(float current_pico_ceiling_c, bool current_known,
                                        const float *new_max_temp_c, size_t new_n,
                                        safety_ceiling_writer_fn writer, void *writer_ctx,
                                        safety_ceiling_sync_result_t *out_result, char *reason_out,
                                        size_t reason_cap);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_CEILING_POLICY_H */
