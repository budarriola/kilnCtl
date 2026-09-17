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
 * verified), for logging. */
bool pico_update_attempts_record_attempt(uint32_t pair_hash, uint32_t *out_new_count);

/* Marks `pair_hash`'s most recent attempt as a terminal (non-retryable)
 * failure -- plan sec 4/7's third unrecoverable cause. Does not touch the
 * count. Read-back verified the same way. Returns true iff confirmed. */
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
