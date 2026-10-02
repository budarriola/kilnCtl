// pico_update_attempts.h -- the persisted, per-pair attempt-budget counter
// behind docs/PICO_AUTO_UPDATE_PLAN.md sec 7/9 step 4 ("the persisted
// counter... per boot_guard.c's helpers").
//
// WHY A NEW COUNTER RATHER THAN REUSING boot_guard.c's: that counter answers
// "has THIS BOARD failed to come up healthy N times in a row" -- a single
// scalar with no notion of WHICH failure. This one must answer "how many
// times has THIS SPECIFIC (expected, observed) Pico identity pair been
// attempted", so a NEW mismatch (a different Pico commit, or a corrected
// expected commit after a fix) gets its own fresh budget rather than
// inheriting a stale count from an unrelated prior mismatch -- plan sec 7:
// "keyed by the pair... so a new mismatch gets its own fresh budget and a
// stuck one cannot ratchet." Reusing boot_guard's single-slot record for two
// different questions would be exactly the kind of shared, undifferentiated
// state CLAUDE.md's "reset one side of a pair" note warns about.
//
// MECHANICS COPIED DELIBERATELY, NOT RE-DERIVED (plan sec 7's own
// instruction): a bare HAL_OK from an NVS write is not evidence in this
// module's neighbourhood (2026-09-08 boot_guard audit: a write reported
// success while the persisted value never changed). Every write here is
// read back before being reported as success, with one bounded
// erase-then-retry, same as boot_guard_mark_healthy()'s
// persist_count()/verify_persisted_count()/erase_then_persist_count().
//
// SINGLE-SLOT RECORD, LIKE boot_guard.c: only one (expected, observed) pair
// is remembered at a time. A board can only be mismatched against ONE Pico
// at once, so this is not a real limitation -- loading a record whose
// pair_hash does not match the CURRENT pair is treated exactly like no
// record at all (a fresh budget), which is what "a new mismatch gets its
// own fresh budget" requires.
//
// NVS key length: NVS_KEY_LEN_CHECK() in the .c enforces the 15-character
// cap at compile time.
#ifndef PICO_UPDATE_ATTEMPTS_H
#define PICO_UPDATE_ATTEMPTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CRC32 (same table-less IEEE/zlib routine as boot_guard.c's crc32_compute(),
 * verified against the same "123456789" -> 0xCBF43926 vector in
 * test_pico_update_attempts.c) over `expected` (NUL-terminated) followed by
 * `observed[0..observed_len)` (NOT NUL-terminated -- the wire's commit bytes
 * are copied in as-is). Pure, host-testable, no I/O. This is an identity
 * KEY, not a security boundary: a hash collision between two different real
 * commit pairs would only ever share a budget, never bypass one. */
uint32_t pico_update_attempts_pair_hash(const char *expected, const uint8_t *observed,
                                         uint8_t observed_len);

/* Loads the persisted attempt count and terminal-failure flag for
 * `pair_hash`. Returns false (count=0, failed=false) for "no record", "the
 * stored record belongs to a different pair" (a fresh budget, by design --
 * see this header's top comment), or "the record failed its version/CRC
 * check" (corruption collapses to the same safe default as boot_guard.c's
 * load_count()). `out_count`/`out_failed` may be NULL. */
bool pico_update_attempts_load(uint32_t pair_hash, uint32_t *out_count, bool *out_failed);

/* Records one more attempt for `pair_hash`: if the persisted record belongs
 * to a different pair (or none), starts this pair at count=1 (a fresh
 * budget); otherwise increments. Read-back verified with one bounded
 * erase-then-retry, same discipline as boot_guard_mark_healthy(). Returns
 * true iff the new count was confirmed in flash; on false the caller must
 * NOT trust that the attempt was actually counted (the same "logging
 * unchecked success" class CLAUDE.md's standing note names). `out_new_count`
 * (may be NULL) receives the count that was attempted (whether or not it
 * verified), for logging.
 *
 * `slot_tried` (0 = embedded SaftyFW_slotA, 1 = embedded SaftyFW_slotB) is
 * persisted alongside the count -- see pico_update_attempts_next_slot()'s
 * header comment for why: with two position-dependent embedded images and
 * no way for the ESP to know which slot the Pico's bootloader will actually
 * select, alternating which one is pushed on successive attempts means a
 * wrong first guess is followed by the right one rather than repeating the
 * same wrong guess for the whole budget. */
bool pico_update_attempts_record_attempt(uint32_t pair_hash, int slot_tried, uint32_t *out_new_count);

/* Pure slot selection (no I/O). If `wire_known`, the Pico's report wins: the
 * result is the opposite of the reported active slot (`wire_active_is_b`).
 * Otherwise the result alternates from `last_slot` (0=A,1=B) when
 * `have_record`, and is 0 (A) for a fresh pair. `*out_disagree` (may be
 * NULL) is true iff there is a record AND the wire slot is known AND they
 * differ (last_slot != reported active slot). */
int pico_update_attempts_select_slot(bool have_record, int last_slot, bool wire_known,
                                     bool wire_active_is_b, bool *out_disagree);

/* Which embedded slot image (0=A, 1=B) the NEXT attempt for `pair_hash`
 * should push. Owner decision 2026-10-02: when the Pico's reported active
 * slot is known (`wire_known`/`wire_active_is_b`, from safety_link.h's
 * pico_active_slot_known/_is_b -- the caller must read them only once the
 * link is up and a V3 Frame A has arrived, else pass wire_known=false) the
 * answer is the opposite of it. When unknown, it alternates from whichever
 * slot was tried last for this exact pair, and a fresh pair starts at slot 0
 * (A). On a disagreement between the wire and the persisted guess it logs
 * once at WARN and rewrites the persisted last_slot to the reported slot
 * (count and failed flag preserved). Returns true iff a record for
 * `pair_hash` existed.
 *
 * HOOK FOR THE SAFTYFW-SIDE REJECTION (N6, opus review 2026-09-20: this
 * rejection code EXISTS on the wire already -- Pico update_task.c:244's
 * state 8 sends it, and the ESP side already names it in safety_link.h:720
 * -- what is NOT yet implemented is this module wiring into it). Today, ANY
 * relay failure -- wrong-slot or otherwise -- is treated identically
 * (ota_pico_relay's existing failure path, unchanged by this feature) and
 * consumes one attempt from the budget like any other failure. Wiring the
 * existing wrong-slot code in here would let a relay failure specifically
 * identified as "wrong slot" retry immediately with
 * pico_update_attempts_next_slot()'s other value WITHOUT calling
 * pico_update_attempts_record_attempt() again (i.e. without consuming
 * budget for a guess that was never given a fair chance to succeed) -- see
 * pico_auto_update_boot.c's attempt_update(), which names this same hook at
 * its ota_pico_relay_start() call site. */
bool pico_update_attempts_next_slot(uint32_t pair_hash, bool wire_known, bool wire_active_is_b,
                                    int *out_slot);

/* Marks `pair_hash`'s most recent attempt as a terminal (non-retryable)
 * failure -- plan sec 4/7's third unrecoverable cause. Does not touch the
 * count. Read-back verified the same way. Returns true iff confirmed.
 *
 * NO CALLER as of 2026-09-20 (Opus review fix). pico_auto_update_boot.c's
 * attempt_update_staged_locked() used to call this when
 * ota_pico_relay_start() returned false -- but that return means the relay
 * task never took ownership, an ESP-local failure (task-create, typically)
 * that never touched the Pico at all. Persisting a terminal failure for
 * that made ABANDONED_PRIOR_FAILED latch permanently (the only clear is a
 * verified MATCH, which an abandoned pair can never attempt again to
 * reach), off a fault that says nothing about the Pico or the staged image.
 * This function is intentionally kept, undeleted, for a GENUINE terminal
 * relay OUTCOME -- the relay took ownership, ran, and the far side (or
 * ota_pico_relay.c's own state machine) reported back a result that is
 * known non-retryable, as opposed to "budget spent" or "local hiccup,
 * budget still bounds it." No such call site exists yet; ota_pico_relay.c's
 * own header names the same still-unwired hook (the SAFETY_LINK_UPDATE_
 * STATE_REJECTED_SLOT_LINKAGE retry note). Do not re-wire this to the
 * ota_pico_relay_start() failure path removed above -- and note that even
 * once a real caller is wired up, pico_auto_update_boot.c's
 * ABANDONED_PRIOR_FAILED handling is now non-blocking (set_warning, not
 * set_blocking) regardless, so this counter alone can never again latch a
 * permanent firing refusal. */
bool pico_update_attempts_record_failure(uint32_t pair_hash);

/* Clears the record entirely -- called only once the Pico reports the
 * expected identity (plan sec 7: "the counter clears only on a verified
 * success"). Read-back verified the same way. Returns true iff confirmed
 * cleared (or there was nothing to clear). */
bool pico_update_attempts_clear(void);

#ifdef __cplusplus
}
#endif

#endif // PICO_UPDATE_ATTEMPTS_H
