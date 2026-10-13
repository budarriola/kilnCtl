// pico_auto_update.h -- pure decision logic for docs/PICO_AUTO_UPDATE.md
// ("the ESP checks the Pico's firmware version on every boot and updates it
// automatically if needed", owner requirement 2026-09-16, ROADMAP.md).
//
// SCOPE OF THIS FILE. This is step 3 of the plan's work list ("the decision
// function, pure and host-testable"): given the expected Pico identity, what
// the Pico actually reported, whether a firing is in the way, the persisted
// attempt count, and whether a config-format migration chain gap exists, it
// answers exactly one of the outcomes docs/PICO_AUTO_UPDATE.md sec 9
// step 3 names. It does not touch flash, does not talk to the link, and does
// not persist anything -- see pico_update_attempts.h for the counter and
// pico_auto_update_state.h for the target-only glue that calls this with
// live inputs at boot.
//
// HEADER-ONLY, freestanding C11 (no allocation, no I/O, no globals) so it can
// be host-tested directly and included from both target and host builds,
// same convention as readiness_gate.h.
//
// IMAGE_AVAILABLE / EXPECTED_COMMIT: G1 (plan sec 1) is NOT closed by this
// pass -- no SaftyFW slot image is embedded in the ESP application build yet
// (that is plan step 2, and it is explicitly out of scope this pass: it
// needs a real build-system change to embed an actual binary, is only
// meaningful to test once P1's two-slot bootloader is flashed, and arming it
// with an empty/placeholder expected commit would make EVERY board's boot
// decide "abandoned: no image" forever, per this file's own decide()
// function below). PICO_AUTO_UPDATE_IMAGE_AVAILABLE therefore defaults to 0
// and PICO_AUTO_UPDATE_EXPECTED_COMMIT defaults to "" (never matches
// anything, by design -- see pico_auto_update_identity_matches()). Both are
// build-time overridable macros so step 2, when it lands, only has to define
// them (plus a real image writer) rather than touch this file.
#ifndef PICO_AUTO_UPDATE_H
#define PICO_AUTO_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Whether this build embeds a usable SaftyFW slot image (plan G1/step 2).
 * See this file's top comment -- 0 until step 2 actually embeds one. */
#ifndef PICO_AUTO_UPDATE_IMAGE_AVAILABLE
#define PICO_AUTO_UPDATE_IMAGE_AVAILABLE 0
#endif

/* Build-time expected SaftyFW commit (plan sec 2, sec 9 step 1). Compared
 * byte-for-byte against kilnlink_fw_version_t's commit[]/commit_len (NOT
 * null-terminated on the wire -- see pico_auto_update_identity_matches()).
 * "" never matches anything, which is the safe default before step 1 stamps
 * a real value from the embedded image's own build metadata. */
#ifndef PICO_AUTO_UPDATE_EXPECTED_COMMIT
#define PICO_AUTO_UPDATE_EXPECTED_COMMIT ""
#endif

/* Owner decision 10.2: 3, matching RECOVERY_MODE_BOOT_THRESHOLD
 * (boot_guard.h) so there is one number in the tree to remember. */
#define PICO_AUTO_UPDATE_ATTEMPT_BUDGET 3u

/* Every outcome docs/PICO_AUTO_UPDATE.md sec 9 step 3 names. The four
 * ABANDONED_* values are exactly the "unrecoverable" causes of plan sec 4/
 * 10.3 (this classification, and is_unrecoverable() below, are unchanged by
 * the 2026-09-20 amendments). What DOES differ per-cause, as of the same
 * amendments, is how pico_auto_update_boot.c's switch CONSUMES an
 * unrecoverable verdict at boot: BUDGET_SPENT (option c) and PRIOR_FAILED
 * (Opus review, same day) are surfaced as a non-blocking
 * pico_auto_update_state_set_warning() rather than
 * pico_auto_update_state_set_blocking(true) -- see that switch and
 * docs/PICO_AUTO_UPDATE.md sec 4/10.3's amendment notes. Only
 * NO_IMAGE and CHAIN_GAP still actually refuse the next firing start;
 * treat this enum's is_unrecoverable() as "the decision cannot self-heal",
 * not as "blocks firing" -- those stopped being the same thing here. */
typedef enum {
    PICO_AUTO_UPDATE_MATCH = 0,           /* identity equal, dirty==0: no action, counter untouched */
    PICO_AUTO_UPDATE_NEEDED,              /* mismatch, an attempt may be made (budget remains, chain ok) */
    PICO_AUTO_UPDATE_DEFER_FIRING,        /* a firing is running/paused/resumable -- try again next idle boot */
    PICO_AUTO_UPDATE_LINK_DOWN,           /* no FW_VERSION has arrived yet this boot -- cannot decide, not an update trigger */
    PICO_AUTO_UPDATE_ABANDONED_NO_IMAGE,       /* unrecoverable cause 1: G1 unmet, or the embedded image is unusable */
    PICO_AUTO_UPDATE_ABANDONED_CHAIN_GAP,      /* unrecoverable cause 1b: a config-migration step is missing (plan sec 6/8) */
    PICO_AUTO_UPDATE_ABANDONED_BUDGET_SPENT,   /* unrecoverable cause 2: the persisted per-pair counter is spent (sec 7) */
    PICO_AUTO_UPDATE_ABANDONED_PRIOR_FAILED,   /* unrecoverable cause 3: the last attempt for this pair reported a terminal failure (sec 7 Q5) */
} pico_auto_update_decision_t;

/* True for exactly the three ABANDONED_* outcomes above -- the ones plan
 * sec 4/10.3 says must refuse the next firing start. Kept as one function so
 * readiness_gate's fact and this decision can never drift on "what counts as
 * unrecoverable" (the same "reset one side of a pair" discipline every other
 * gate item in this codebase already follows). */
static inline bool pico_auto_update_decision_is_unrecoverable(pico_auto_update_decision_t d)
{
    return d == PICO_AUTO_UPDATE_ABANDONED_NO_IMAGE || d == PICO_AUTO_UPDATE_ABANDONED_CHAIN_GAP ||
           d == PICO_AUTO_UPDATE_ABANDONED_BUDGET_SPENT || d == PICO_AUTO_UPDATE_ABANDONED_PRIOR_FAILED;
}

#define PICO_AUTO_UPDATE_MAX_COMMIT_LEN 64u /* matches KILNLINK_FW_VERSION_MAX_COMMIT_LEN */

typedef struct {
    /* Build-time expected identity, e.g. PICO_AUTO_UPDATE_EXPECTED_COMMIT. */
    const char *expected_commit;      /* NUL-terminated */
    /* What the Pico actually reported this boot (kilnlink_fw_version_t).
     * NOT NUL-terminated on the wire -- observed_commit_len is authoritative. */
    uint8_t     observed_commit[PICO_AUTO_UPDATE_MAX_COMMIT_LEN];
    uint8_t     observed_commit_len;
    bool        observed_dirty;       /* kilnlink_fw_version_t.dirty != 0 */
    bool        fw_version_known;     /* a FW_VERSION reply has actually arrived this boot (safety_link_get_peer_build_status()'s out_known) */
    bool        image_available;      /* PICO_AUTO_UPDATE_IMAGE_AVAILABLE, passed in so the decision stays pure */
    bool        firing_active;        /* a profile is running, paused, or resumable at boot (plan sec 4) */
    bool        config_chain_gap;     /* a migration step is missing between the Pico's config_version and the target (plan sec 6/8) */
    uint32_t    attempt_count;        /* persisted count for THIS (expected, observed) pair (plan sec 7) */
    bool        prior_attempt_failed; /* the last relay attempt for this pair ended in a terminal (non-retryable) failure */
} pico_auto_update_inputs_t;

/* Exact-match identity comparison (plan sec 2): equal commit bytes AND
 * dirty == 0. Deliberately NOT an ordering -- there is no monotonic version
 * field on the wire, only a commit string (plan sec 2's own rationale). An
 * empty expected_commit (the unstamped default) never matches anything, so a
 * build that has not run step 1 yet cannot accidentally read as "matched". A
 * dirty Pico build never matches either, by design. */
static inline bool pico_auto_update_identity_matches(const pico_auto_update_inputs_t *in)
{
    if (in == NULL || in->expected_commit == NULL) {
        return false;
    }
    if (in->observed_dirty) {
        return false;
    }
    size_t expected_len = strlen(in->expected_commit);
    if (expected_len == 0 || expected_len > PICO_AUTO_UPDATE_MAX_COMMIT_LEN) {
        return false;
    }
    if (expected_len != (size_t)in->observed_commit_len) {
        return false;
    }
    return memcmp(in->expected_commit, in->observed_commit, expected_len) == 0;
}

/* The decision. Pure: same inputs always produce the same outcome, no I/O.
 * `reason` (may be NULL) receives a short, static, human-readable string --
 * never dynamically formatted, so it is safe to log or surface without a
 * scratch buffer. Order matters and is deliberate, matching plan sec 4/7/9:
 *
 *   1. link down / no FW_VERSION yet   -> cannot decide at all
 *   2. identity already matches        -> nothing to do, cheapest check first
 *   3. a firing is in the way          -> defer, never abandon on this alone
 *   4. no image to attempt with        -> unrecoverable (G1 unmet)
 *   5. a chain gap in config migration -> unrecoverable (plan sec 6/8)
 *   6. the prior attempt for this pair terminally failed -> unrecoverable
 *   7. the persisted budget is spent   -> unrecoverable
 *   8. otherwise                       -> needed, an attempt may be made
 *
 * Firing-in-progress is checked BEFORE any of the unrecoverable causes on
 * purpose (plan sec 4: "a version mismatch by itself still only defers...
 * a live firing already in progress ... is not aborted by this rule"): a
 * board that is mid-firing when its budget happens to be spent must still
 * defer, not silently transition to a state that only matters for the NEXT
 * start -- the unrecoverable causes only need to be visible at the next
 * start attempt, which readiness_gate.h's biconditional over the persisted
 * decision already guarantees regardless of the order checked here. */
static inline pico_auto_update_decision_t pico_auto_update_decide(const pico_auto_update_inputs_t *in,
                                                                    const char **reason)
{
    const char *why = NULL;
    pico_auto_update_decision_t d;

    if (in == NULL || !in->fw_version_known) {
        d = PICO_AUTO_UPDATE_LINK_DOWN;
        why = "no FW_VERSION from the Pico yet this boot -- cannot compare identities";
    } else if (pico_auto_update_identity_matches(in)) {
        d = PICO_AUTO_UPDATE_MATCH;
        why = "Pico identity matches the expected build -- no action";
    } else if (in->firing_active) {
        d = PICO_AUTO_UPDATE_DEFER_FIRING;
        why = "a firing is running, paused, or resumable -- deferred to the next idle boot";
    } else if (!in->image_available) {
        d = PICO_AUTO_UPDATE_ABANDONED_NO_IMAGE;
        why = "no usable SaftyFW image is embedded in this ESP build (plan G1 unmet)";
    } else if (in->config_chain_gap) {
        d = PICO_AUTO_UPDATE_ABANDONED_CHAIN_GAP;
        why = "a config-migration step is missing between the Pico's config version and the target";
    } else if (in->prior_attempt_failed) {
        d = PICO_AUTO_UPDATE_ABANDONED_PRIOR_FAILED;
        why = "the last update attempt for this version pair ended in a terminal failure";
    } else if (in->attempt_count >= PICO_AUTO_UPDATE_ATTEMPT_BUDGET) {
        d = PICO_AUTO_UPDATE_ABANDONED_BUDGET_SPENT;
        why = "the 3-attempt budget for this version pair is spent";
    } else {
        d = PICO_AUTO_UPDATE_NEEDED;
        why = "Pico identity mismatch -- an update attempt may be made";
    }

    if (reason != NULL) {
        *reason = why;
    }
    return d;
}

#ifdef __cplusplus
}
#endif

#endif // PICO_AUTO_UPDATE_H
