#ifndef SAFETY_CEILING_POLICY_H
#define SAFETY_CEILING_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* HISTORICAL, NO LONGER APPLIED -- kept only so the reasoning that was
 * tried and then explicitly overridden is not lost. Originally this file
 * added 5 C of headroom on top of the ESP's zone ceiling before writing
 * the Pico's target, reusing the owner's separate "profile target vs
 * configured limit" convention (dashboard warning, a9abd273) on the
 * theory that it would keep the two ceilings from tripping at effectively
 * the same instant on ordinary sensor noise/offset -- the Pico is meant
 * to catch the ESP having ALREADY failed to limit, not to race it.
 *
 * 2026-09-10 owner correction, verbatim: "the intent of the web page
 * setting was to put a hard cutoff." The number typed into the zone
 * ceiling field is meant to BE the Pico's cutoff, not a value the Pico
 * trips 5 C above -- so safety_ceiling_policy_target_c() no longer adds
 * this constant; it returns the ESP's own configured maximum exactly.
 *
 * The nuisance-trip concern above is real but is handled differently, not
 * ignored: equality (Pico ceiling == ESP zone ceiling) still satisfies the
 * standing invariant that the Pico must never be TIGHTER than the ESP
 * (see this file's own top comment) -- a value that is EQUAL is not
 * tighter. What headroom would have bought was a margin against the two
 * ceilings tripping on the same instant of sensor noise; a hard cutoff
 * accepts that trade deliberately; there is no separate noise margin
 * elsewhere in this file's logic. This constant is left defined (unused
 * by safety_ceiling_policy_target_c()) rather than deleted outright, in
 * case a future noise-margin need resurfaces and wants to reuse the
 * reviewed value rather than inventing a new one -- if nothing comes to
 * reference it, delete it in a later pass instead of leaving dead
 * ballast. */
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
 * `max(max_temp_c[i] for max_temp_c[i] > 0)` EXACTLY -- no added headroom
 * (2026-09-10 owner correction: the web page's setting is a hard cutoff;
 * see SAFETY_CEILING_HEADROOM_C's comment below for what this used to
 * add and why it no longer does). */
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

/* 2026-09-10 opus review finding: classifying a refusal by substring-
 * matching the human-readable `reason_out` sentence (the original
 * implementation of this backoff) is wrong on multiple counts -- the
 * string containing "ARMED" is not always produced on a genuine ARMED
 * refusal (safety_cfg_http.c's confirm-by-readback path only has it when a
 * one-shot stashed COMMIT_CONFIG_REJECTED frame happens to still be there),
 * `strstr` has no word boundary (a hypothetical "not ARMED"/"DISARMED"
 * message would misclassify), and prose is not a stable contract between
 * caller and callee. The writer now reports the refusal class directly, by
 * NUMBER, alongside the human string -- `reason_out` stays exactly what an
 * operator sees; `out_class` is what control flow (the reconcile backoff)
 * is allowed to depend on. `out_class` may be NULL (callers that only need
 * the human message, e.g. apply_lower()'s best-effort writer call, pass
 * NULL and are not required to interpret it). When non-NULL, the writer
 * MUST write it whenever it returns false: SAFETY_CEILING_REFUSAL_OTHER is
 * the safe default for any failure whose exact cause could not be proven
 * (comms error, unconfirmed read-back with no rejection frame in hand) --
 * never guess ARMED without the numeric reject reason in hand. */
typedef enum {
    SAFETY_CEILING_REFUSAL_NONE = 0,     /* not a failure (writer returned true) */
    SAFETY_CEILING_REFUSAL_ARMED,        /* Pico's KILNLINK_COMMIT_CONFIG_REJECT_ARMED, known for certain */
    SAFETY_CEILING_REFUSAL_STORAGE,      /* Pico's KILNLINK_COMMIT_CONFIG_REJECT_STORAGE, known for certain */
    SAFETY_CEILING_REFUSAL_RANGE,        /* Pico's KILNLINK_COMMIT_CONFIG_REJECT_RANGE, known for certain */
    SAFETY_CEILING_REFUSAL_CONTRADICTION,/* Pico's KILNLINK_COMMIT_CONFIG_REJECT_CONTRADICTION, known for certain */
    SAFETY_CEILING_REFUSAL_OTHER,        /* anything else: comms failure, unconfirmed read-back, no writer, etc */
} safety_ceiling_refusal_class_t;

/* Writes (stage + commit + confirm-by-readback) a single float parameter
 * to the Pico and reports whether it is now CONFIRMED to hold that exact
 * value -- the same contract safety_cfg_http.c's confirm_commit_landed()
 * already enforces for every other commissioning field (never trust a
 * bare ACK). `reason_out` must be filled with a human-readable outcome on
 * failure (including, verbatim or paraphrased, the ARMED refusal --
 * commit_reject_reason_words()'s "relay is ARMED -- config writes are
 * refused while ARMED" -- when that is why it failed, since that is the
 * single most likely and most operator-actionable failure mode for this
 * particular field, see this header's own top comment). `out_class` (may
 * be NULL) must be filled with the machine-readable classification above
 * whenever this function returns false -- see the enum's own comment for
 * why this exists separately from `reason_out`. */
typedef bool (*safety_ceiling_writer_fn)(void *ctx, float target_c, char *reason_out, size_t reason_cap,
                                          safety_ceiling_refusal_class_t *out_class);

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
 *     without saying so.
 *
 * `out_refusal_class` (may be NULL) receives the machine-readable
 * classification of a failed raise -- see safety_ceiling_refusal_class_t's
 * own comment. Only meaningful when this function returns false; left
 * untouched on success. */
bool safety_ceiling_policy_guard_raise(float current_pico_ceiling_c, bool current_known,
                                        const float *new_max_temp_c, size_t new_n,
                                        safety_ceiling_writer_fn writer, void *writer_ctx,
                                        safety_ceiling_sync_result_t *out_result, char *reason_out,
                                        size_t reason_cap, safety_ceiling_refusal_class_t *out_refusal_class);

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

/* 2026-09-10 opus review finding A: safety_ceiling_sync_reconcile_on_link_up()
 * (safety_link_poll.c) runs this policy's guard_raise() unconditionally on
 * EVERY safety_poll_task tick (~500 ms, sdkconfig CONFIG_ESP_TASK_WDT_TIMEOUT_S
 * = 5 s) while the link is up. That is a true no-op only when the cached Pico
 * ceiling is already known and wide enough; the moment a raise is genuinely
 * needed AND the Pico refuses it (config_store.c's unconditional "ARMED
 * refuses every write", which is the Pico's ORDINARY standing state -- see
 * safety_ceiling_policy_guard_raise()'s own doc comment), the exact same
 * expensive stage+commit+confirm round trip (safety_cfg_http_set_and_
 * confirm_f32() -> apply_pairs() -> two UART exchanges, PLUS confirm_commit_
 * landed()'s up-to-2000ms safety_cfg_store_refetch() on any ACKed-but-
 * unconfirmed path) would otherwise be retried twice a second, forever,
 * against a refusal that cannot resolve until the relay de-energises --
 * pure cost, and unrate-limited logging with it.
 *
 * This is a tiny PURE backoff state machine (no I/O, no clock source of its
 * own -- the caller supplies `now_us` from whatever monotonic clock it has,
 * exactly the same "pure decision, real clock stays outside" split this
 * file's other functions already use) that the ESP-glue reconcile call site
 * (safety_ceiling_sync.c) consults BEFORE calling guard_raise() at all: while
 * backed off, the reconcile is skipped entirely -- no UART round trip, no
 * log line -- and only re-attempted once `now_us` reaches `backoff_until_us`.
 *
 * Backoff duration depends on the CLASSIFIED reason the previous attempt
 * failed (safety_ceiling_refusal_class_t, reported by the writer as a
 * number -- 2026-09-10 opus review finding B fixed this from an earlier
 * version that classified by `strstr(reason, "ARMED")`, which is wrong on
 * three counts: (1) the human-readable reason string is not always
 * produced on a genuine ARMED refusal -- safety_cfg_http.c's confirm-by-
 * readback path only attaches it when a one-shot stashed
 * COMMIT_CONFIG_REJECTED frame happens to still be there when it looks,
 * so a late or already-consumed frame silently fell through to a generic
 * "could not confirm" sentence with no "ARMED" in it at all, and the
 * identical physical condition got the SHORT backoff instead of the long
 * one; (2) `strstr` has no word boundary (a hypothetical "not ARMED" or
 * "DISARMED" message would misclassify -- no such string exists today, but
 * the classifier was wrong regardless of whether anything currently
 * triggers it); (3) it silently lumped the genuinely persistent
 * KILNLINK_COMMIT_CONFIG_REJECT_STORAGE failure (a Pico with a failing
 * flash write) in with "ordinary transient failure" and gave it the SHORT
 * backoff, which hammers a hardware fault that retrying faster cannot fix):
 *
 *   - SAFETY_CEILING_REFUSAL_ARMED -- CORRECTED 2026-09-10 (a second opus
 *     review found the ARMED-clears-on-de-energise/PWM-window rationale
 *     this backoff originally shipped with, and the jitter term derived
 *     from it, were both built on a false premise -- see below).
 *     RELAY_OWNER_STATE_ARMED (relay_owner.h) is a LATCH:
 *     INIT -> GRACE -> ARMED -> TRIPPED. relay_owner_task() (relay_owner.c)
 *     has exactly one assignment out of GRACE (the grace-timeout tick) and
 *     nothing demotes ARMED back to GRACE; de-energising GPIO6 (a normal
 *     PWM off-window, or any other de-energise) sets `s_energized = false`
 *     and leaves `s_state == ARMED` untouched. So ARMED is NOT correlated
 *     with the relay's instantaneous energized/de-energized state at all --
 *     it is the Pico's ordinary STANDING state for the rest of its uptime
 *     once the post-boot/post-trip-clear grace window ends, whether the
 *     relay happens to be chopping high or sitting low at any given
 *     instant. A raise attempted while ARMED therefore fails on EVERY
 *     attempt for as long as the latch holds (see this header's own
 *     "Remaining window" note below) -- there is no PWM off-window for a
 *     retry to "catch" by varying its phase, because the condition being
 *     retried against does not come and go with the relay's duty cycle.
 *     Given that, a long, FIXED backoff is simply correct:
 *     SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_US, chosen at least as long as
 *     the pre-2026-09-10 30s figure this backoff first shipped with. No
 *     jitter: jitter only has value against a condition that changes on a
 *     timescale the jitter can decorrelate against, and this one does not
 *     change on any timescale a retry can usefully catch early -- it
 *     resolves only when the relay latch itself clears (de-energised AND a
 *     boot/trip-clear cycle passes back through GRACE), an event this
 *     backoff cannot hasten by retrying more or less often.
 *   - SAFETY_CEILING_REFUSAL_STORAGE -- a real hardware/flash fault that
 *     retrying sooner cannot fix and that does not resolve on its own the
 *     way ARMED does; back off LONGER
 *     (SAFETY_CEILING_RECONCILE_STORAGE_BACKOFF_US) than even the ARMED
 *     case, and rely on the (already rate-limited) WARN log for an operator
 *     to notice and intervene rather than hammering the link.
 *   - SAFETY_CEILING_REFUSAL_RANGE / _CONTRADICTION / _OTHER (comms error,
 *     unconfirmed read-back with no rejection frame in hand, no writer
 *     available) -- back off modestly (SAFETY_CEILING_RECONCILE_RETRY_
 *     BACKOFF_US), the original "ordinary transient failure" case; this is
 *     also the SAFE DEFAULT the writer reports whenever it cannot prove
 *     exactly which numbered reason applied, so an unproven failure never
 *     silently earns the long backoff either.
 *
 * A successful attempt (or one that needed no write at all) clears the
 * backoff immediately, so the very next tick can react promptly to a
 * relay that has just been de-energised or a config that has just changed.
 * The cost this backoff exists to bound (versus the pre-backoff every-
 * ~500ms retry) is preserved by every branch above: at most one ≤2 s
 * safety_cfg_store_refetch() every few seconds, never every tick.
 *
 * Remaining window -- CORRECTED 2026-09-10 (the previous wording here was
 * wrong): a failed raise leaves the ESP's zone ceiling ABOVE the Pico's
 * confirmed ceiling -- i.e. the Pico strictly TIGHTER than the ESP, the one
 * state this feature exists to prevent -- for as long as the underlying
 * cause persists, NOT merely "up to one backoff period". The backoff only
 * bounds how often a retry is ATTEMPTED; it does nothing to bound how long
 * the mismatch itself lasts. Concretely: a raise attempted while the relay
 * is ARMED (its ordinary standing state through an entire firing) fails on
 * every attempt until the relay de-energises, so the mismatch can persist
 * for the WHOLE firing, not one backoff period of it. This reconcile path
 * was already best-effort/eventual (safety_ceiling_sync.h's own doc
 * comment: "never blocks heat or fails the boot... left for the next tick
 * or the next interactive zones POST to retry") and this is a plain
 * statement of how eventual "eventual" can be, not a new hazard introduced
 * by the backoff -- the pre-backoff ~500 ms retry had the exact same
 * unbounded-persistence property whenever the relay stayed ARMED, it just
 * polled more often while doing nothing more effective. */
#define SAFETY_CEILING_RECONCILE_RETRY_BACKOFF_US ((int64_t)5 * 1000 * 1000)  /* 5 s -- ordinary/unclassified transient failure */
/* CORRECTED 2026-09-10 (second opus review): restored to a conservative
 * fixed 30 s, no jitter -- see this header's block comment on
 * SAFETY_CEILING_REFUSAL_ARMED above for why. ARMED is a latch, not a
 * PWM-correlated condition: it does not clear on relay de-energise, only on
 * a full de-energise-then-reboot-or-trip-clear cycle back through GRACE.
 * A retry's phase relative to the relay's duty cycle is therefore
 * irrelevant to whether this attempt can succeed, so jitter buys nothing
 * and was removed along with the false rationale. 30 s (the figure this
 * backoff shipped with before jitter was introduced) is still strictly
 * longer than the 5 s ordinary backoff and short enough that a relay that
 * has just cleared its latch (reboot, or a trip clear) is noticed within
 * one Pico link-up cycle, not held back indefinitely. */
#define SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_US ((int64_t)30 * 1000 * 1000) /* 30 s -- ARMED is a standing latch, not a transient window; no jitter needed or possible to justify */
#define SAFETY_CEILING_RECONCILE_STORAGE_BACKOFF_US ((int64_t)60 * 1000 * 1000) /* 60 s -- persistent hardware fault; faster retry cannot help */

typedef struct {
    int64_t backoff_until_us; /* 0 == never backed off / not currently backing off */
} safety_ceiling_reconcile_backoff_t;

/* True iff a reconcile attempt may be made now (state->backoff_until_us has
 * been reached or was never set). Never mutates `state`. */
bool safety_ceiling_reconcile_should_attempt(const safety_ceiling_reconcile_backoff_t *state, int64_t now_us);

/* Updates `state` after an attempt. `ok` is guard_raise()'s own return
 * value; `refusal_class` is whatever it left in *out_refusal_class (ignored
 * when ok is true). A successful attempt (ok == true, including the
 * "nothing needed" case) clears the backoff. A failed attempt sets
 * backoff_until_us = now_us plus the backoff duration selected by
 * `refusal_class` -- see this header's block comment above for the full
 * rationale per class (CORRECTED 2026-09-10: the ARMED case is now a fixed
 * duration, no jitter -- see SAFETY_CEILING_RECONCILE_ARMED_BACKOFF_US's own
 * comment for why). This remains a pure function purely of its inputs
 * (`now_us` is used only to compute the new deadline, not to derive any
 * random or clock-dependent term), host-testable exactly like every other
 * function in this file. */
void safety_ceiling_reconcile_record_result(safety_ceiling_reconcile_backoff_t *state, int64_t now_us, bool ok,
                                             safety_ceiling_refusal_class_t refusal_class);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_CEILING_POLICY_H */
