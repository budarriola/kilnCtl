// safety_cfg_write -- the ESP-side WRITER half of the Pico safety-parameter
// mirror, the sibling of safety_cfg_store.c's reader half.
//
// Everything in this file is one primitive expressed at several widths:
// stage (SET_PARAM), commit (COMMIT_CONFIG, or SAFETY_CMD_APPLY_CONFIG_
// VOLATILE for the RAM-only install), then CONFIRM BY FORCED LIVE READ-BACK
// (GET_CONFIG_PAGE) -- "never trust a bare ACK". It also owns the numeric
// classification of a Pico refusal (safety_ceiling_refusal_class_t), which
// control flow must use instead of substring-matching prose reasons.
//
// WHY THIS LIVES IN safety/ AND NOT http/ (2026-09-16): it used to live in
// drivers/http/safety_cfg_http.c, so drivers/safety/safety_ceiling_sync.c --
// a safety module -- had to include an http/ header to reach it, a layering
// inversion. That inversion was not cosmetic: an attempt to close it by
// deleting the #include left the call with NO PROTOTYPE IN SCOPE, C assumed
// "extern int f()", the float target went through default argument promotion,
// and the Pico ceiling was written as 0. The compiler only WARNED; a host
// test caught it. This module is the real fix -- the set-and-confirm
// primitive over the safety link is safety-layer work, and http/ is now one
// of its CALLERS, not its owner. -Werror=implicit-function-declaration is
// now set on this component so that same mistake can never compile again.
//
// This file must not gain any HTTP, httpd_req_t or form-parsing dependency.
// Body parsing stays in http/safety_cfg_http.c; it hands the pairs here.

#include "safety_cfg_write.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

#include "estop_verification.h"
#include "kilnlink/kilnlink_commit_config_rejected.h"
#include "safety_ceiling_policy.h" /* safety_ceiling_refusal_class_t */
#include "safety_ceiling_sync.h"   /* SAFETY_PARAM_ID_ABS_MAX_TEMP_C -- the
                                    * ceiling field apply_package_and_confirm()
                                    * deliberately excludes. No cycle: that
                                    * header includes only safety_ceiling_
                                    * policy.h/safety_link.h, never this one. */
#include "safety_cfg_store.h"
#include "safety_link.h"

/* Parses one pair's text value against `type` into `out`. Returns false
 * (out untouched) on a malformed number or a bool/enum value that fails
 * strtoul/strtof's own parse -- range-checking beyond "fits the wire type"
 * is deliberately NOT done here (e.g. tc_source's enum range): that
 * cross-field/enum-range validation is COMMIT_CONFIG's job on the Pico,
 * per COMMISSIONING.md sec 2's "validation happens at COMMIT_CONFIG, not at
 * SET_PARAM, because the rules that matter are cross-field" -- duplicating
 * a second, possibly-diverging range check here would be exactly the kind
 * of two-definitions-of-valid this codebase's other stores avoid. */
bool safety_cfg_write_parse_value_for_type(const char *text, uint8_t type, kilnlink_param_value_t *out)
{
    char *end = NULL;
    switch (type) {
    case KILNLINK_PARAM_TYPE_BOOL: {
        long v = strtol(text, &end, 10);
        if (end == text || *end != '\0' || (v != 0 && v != 1)) {
            return false;
        }
        out->bool_val = (uint8_t)v;
        return true;
    }
    case KILNLINK_PARAM_TYPE_U8: {
        long v = strtol(text, &end, 10);
        if (end == text || *end != '\0' || v < 0 || v > 0xFF) {
            return false;
        }
        out->u8_val = (uint8_t)v;
        return true;
    }
    case KILNLINK_PARAM_TYPE_U16: {
        long v = strtol(text, &end, 10);
        if (end == text || *end != '\0' || v < 0 || v > 0xFFFF) {
            return false;
        }
        out->u16_val = (uint16_t)v;
        return true;
    }
    case KILNLINK_PARAM_TYPE_F32: {
        float v = strtof(text, &end);
        if (end == text || *end != '\0' || !isfinite(v)) {
            return false;
        }
        out->f32_val = v;
        return true;
    }
    default:
        return false;
    }
}

/* Forward declaration -- confirm_commit_landed() below needs this before its
 * own definition later in the file (kept where it always lived, right next
 * to safety_cfg_write_apply_pairs() which is its other caller). */
static const char *commit_reject_reason_words(uint8_t reason);

/* Forward declaration -- confirm_commit_landed() below needs this before its
 * own definition later in the file (kept next to commit_reject_reason_words(),
 * which every call site of this function already pairs with). */
static safety_ceiling_refusal_class_t reject_reason_to_refusal_class(uint8_t reason);

/* True if two kilnlink_param_value_t of the same wire `type` hold the same
 * value. F32 is compared bit-for-bit (memcmp), not with an epsilon -- both
 * ends of this link encode/decode the identical 4-byte IEEE-754 layout
 * (kilnlink_param_value.h), so a value that survived SET_PARAM -> COMMIT_CONFIG
 * -> flash -> GET_CONFIG_PAGE unchanged reads back BIT-IDENTICAL or it did not
 * survive at all; there is no legitimate case of "close enough" here. */
static bool param_value_equal(uint8_t type, const kilnlink_param_value_t *a, const kilnlink_param_value_t *b)
{
    switch (type) {
    case KILNLINK_PARAM_TYPE_BOOL: return a->bool_val == b->bool_val;
    case KILNLINK_PARAM_TYPE_U8:   return a->u8_val == b->u8_val;
    case KILNLINK_PARAM_TYPE_U16:  return a->u16_val == b->u16_val;
    case KILNLINK_PARAM_TYPE_F32:  return memcmp(&a->f32_val, &b->f32_val, sizeof(a->f32_val)) == 0;
    default:                       return false;
    }
}

/* Positive confirmation that a commit this function just reported ACKed (and
 * not rejected within safety_link_send_commit_config()'s own reply window)
 * actually landed on the Pico's flash, per COMMISSIONING.md/the 2026-08-26
 * commissioning-write audit: "ok cannot fail" because SET_PARAM/COMMIT_CONFIG
 * are both fire-and-forget broadcasts (uart_protocol_send_broadcast() reports
 * only "the local UART accepted the bytes"), and a REJECTED reply that misses
 * the ~144 ms reply window used to be silently discarded, defining "no
 * rejection seen" as acceptance.
 *
 * This function is what turns that around: it forces a LIVE re-fetch of the
 * Pico's just-committed record (safety_cfg_store_refetch(), a real blocking
 * round trip -- never the ESP's own stale NVS cache) and checks that every
 * field THIS caller just submitted now reads back exactly the value that was
 * sent. That is strictly stronger than comparing config_crc before/after:
 * a commit that legitimately writes bytes identical to what was already
 * committed bumps nothing a CRC could detect, but the read-back still
 * matches what was sent, so it is correctly reported as success. Only a
 * field that reads back UNSET, or SET to something other than what was sent,
 * is reported as a failure -- i.e. the only two ways a "successful" commit
 * could still be a lie.
 *
 * Returns true (reason_out untouched) iff every submitted pair's value is
 * confirmed. On failure, reason_out names the first mismatching field and, if
 * a COMMIT_CONFIG_REJECTED frame turns up late in the stash while this
 * function was busy doing the live re-fetch (safety_link_take_stashed_
 * commit_rejected()), attaches the Pico's OWN reason instead of a generic
 * "does not match" message -- the read-back is what DECIDES pass/fail, the
 * stash only explains WHY when it can.
 *
 * `out_class` (may be NULL) receives the machine-readable refusal
 * classification whenever this returns false -- 2026-09-10 opus review
 * finding. Defaults to SAFETY_CEILING_REFUSAL_OTHER (never a guessed
 * ARMED) unless a stashed COMMIT_CONFIG_REJECTED frame is actually found,
 * in which case it is set from that frame's own numeric reason via
 * reject_reason_to_refusal_class() -- exactly mirroring what reason_out's
 * prose says in that branch, just as a number instead of a substring to
 * grep for.
 *
 * `nonblocking_refetch` -- 2026-09-10 opus review, second finding: this
 * function used to call the blocking safety_cfg_store_refetch() (portMAX_
 * DELAY) unconditionally, which was correct for its original httpd-worker
 * callers but became a rule violation once the ceiling-reconcile path
 * (safety_ceiling_sync.c) started calling into safety_cfg_write_apply_pairs()/this function
 * from safety_poll_task itself, via safety_cfg_write_set_and_confirm_f32().
 * safety_cfg_store.h's own safety_cfg_store_refetch_nonblocking() doc
 * comment states the rule this serves: safety_poll_task must never block
 * on s_store_lock behind an httpd commissioning POST. Pass true only from
 * a safety_poll_task caller; every httpd-worker caller keeps passing
 * false, unchanged. A non-blocking refetch that loses the lock race
 * reports failure here (SAFETY_CEILING_REFUSAL_OTHER, via the "could not
 * read the config back" branch below) exactly like a comms failure would
 * -- the caller's own backoff/retry-next-tick contract already covers
 * this, same as safety_cfg_store_maybe_refetch()'s existing non-blocking
 * use. */
static bool confirm_commit_landed(SafetyLinkClass *link, const safety_cfg_post_pair_t *pairs, int n_pairs,
                                   char *reason_out, size_t reason_cap, safety_ceiling_refusal_class_t *out_class,
                                   bool nonblocking_refetch)
{
    if (out_class) {
        *out_class = SAFETY_CEILING_REFUSAL_OTHER;
    }
    uint16_t best_known_crc = 0;
    bool peer_known = false;
    (void)safety_link_get_peer_build_status(link, &peer_known, NULL, NULL, NULL, NULL, NULL, NULL,
                                             &best_known_crc);

    bool refetch_ok = nonblocking_refetch
                           ? safety_cfg_store_refetch_nonblocking(link, peer_known ? best_known_crc : 0)
                           : safety_cfg_store_refetch(link, peer_known ? best_known_crc : 0);
    if (!refetch_ok) {
        /* 2026-09-10 opus review, lower-priority finding: safety_link_take_
         * stashed_commit_rejected() is CONSUMING -- whichever caller reads
         * the stash first, on either path through apply_pairs_ex(), removes
         * it for everyone else. Now that the ceiling-reconcile path runs on
         * a fixed 30s ARMED backoff (restored from the earlier ~6-9s
         * jittered cadence), it competes for this one-shot frame with an
         * interactive commissioning POST far less often than it did before
         * this fix -- but the race is not eliminated, just made rarer. An
         * operator's own POST can still occasionally lose its own rejection
         * reason to a background reconcile attempt that happened to refetch
         * first. Separately: this branch classifies from whatever is in the
         * stash when THIS refetch failed -- a stale ARMED rejection left
         * over from an earlier, unrelated POST can misclassify what is
         * actually a fresh comms failure here. Harmless in effect (the
         * worst case is an ARMED-length backoff applied to a comms hiccup,
         * never the reverse), but the classification is not the certainty
         * the code around it implies. */
        uint16_t rp = 0;
        uint8_t rr = 0;
        if (safety_link_take_stashed_commit_rejected(link, &rp, &rr)) {
            if (out_class) {
                *out_class = reject_reason_to_refusal_class(rr);
            }
            uint8_t rt = 0;
            const char *rn = NULL;
            if (rp != KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID && safety_cfg_store_lookup(rp, &rt, &rn)) {
                snprintf(reason_out, reason_cap,
                         "commit rejected: %s (id %u) -- %s -- values were staged but NOT written", rn,
                         (unsigned)rp, commit_reject_reason_words(rr));
            } else {
                snprintf(reason_out, reason_cap, "commit rejected: %s -- values were staged but NOT written",
                         commit_reject_reason_words(rr));
            }
        } else {
            snprintf(reason_out, reason_cap,
                     "the safety processor accepted the commit but this board could not read the "
                     "config back to confirm it -- treating the write as UNCONFIRMED, not successful");
        }
        return false;
    }

    for (int i = 0; i < n_pairs; i++) {
        uint8_t type = 0;
        const char *name = NULL;
        if (!safety_cfg_store_lookup(pairs[i].param_id, &type, &name)) {
            /* 2026-08-27 audit fix (LOW): this used to be `continue`, silently
             * SKIPPING verification of a pair whose lookup failed -- believed
             * unreachable (safety_cfg_write_apply_pairs() already refused any unknown id
             * before staging began), but a skipped verification that then
             * lets the OVERALL commit report success is exactly the failure
             * shape this whole audit exists to close. If this branch is ever
             * actually reached, the honest answer is "could not confirm",
             * never "confirmed". */
            snprintf(reason_out, reason_cap,
                     "internal error: could not verify id %u (%s) after commit -- treating the write "
                     "as UNCONFIRMED, not successful",
                     (unsigned)pairs[i].param_id, name ? name : "unknown");
            return false;
        }
        kilnlink_param_value_t sent;
        if (!safety_cfg_write_parse_value_for_type(pairs[i].value_text, type, &sent)) {
            /* Same reasoning as the lookup failure just above -- believed
             * unreachable (safety_cfg_write_apply_pairs() already parsed this value
             * successfully before staging), same fail-closed answer. */
            snprintf(reason_out, reason_cap,
                     "internal error: could not re-verify the value submitted for %s (id %u) after "
                     "commit -- treating the write as UNCONFIRMED, not successful",
                     name, (unsigned)pairs[i].param_id);
            return false;
        }

        bool found = false;
        safety_cfg_param_t confirmed = {0};
        size_t count = safety_cfg_store_param_count();
        for (size_t j = 0; j < count; j++) {
            safety_cfg_param_t row;
            if (safety_cfg_store_get_by_index(j, &row) && row.param_id == pairs[i].param_id) {
                confirmed = row;
                found = true;
                break;
            }
        }

        if (!found || !confirmed.set || !param_value_equal(type, &confirmed.value, &sent)) {
            uint16_t rp = 0;
            uint8_t rr = 0;
            if (safety_link_take_stashed_commit_rejected(link, &rp, &rr) &&
                (rp == pairs[i].param_id || rp == KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID)) {
                if (out_class) {
                    *out_class = reject_reason_to_refusal_class(rr);
                }
                snprintf(reason_out, reason_cap,
                         "commit rejected: %s (id %u) -- %s -- values were staged but NOT written", name,
                         (unsigned)pairs[i].param_id, commit_reject_reason_words(rr));
            } else {
                snprintf(reason_out, reason_cap,
                         "the safety processor ACKed the commit, but %s (id %u) does not read back "
                         "as the submitted value -- treating the write as FAILED, not successful (possible cause: staged edits "
                         "are discarded after 5 s without a context frame or an ESP reboot before the commit)",
                         name, (unsigned)pairs[i].param_id);
            }
            return false;
        }
    }
    return true;
}

/* kilnlink_commit_config_reject_reason_t -> a short human phrase, for
 * safety_cfg_write_apply_pairs()'s rejection message below. Matches the wording
 * config_params.h's config_params_reject_reason_t doc comment and
 * kilnlink_commit_config_rejected.h's own reason enum use to describe each
 * case -- kept here, not in a shared header, for the same "ESP web surface
 * owns its own wording" split safety_trip_words.h's own comment documents
 * for the LCD/web trip-reason tables (this one just has one caller instead
 * of two). */
static const char *commit_reject_reason_words(uint8_t reason)
{
    switch (reason) {
    case KILNLINK_COMMIT_CONFIG_REJECT_RANGE: return "value out of range";
    case KILNLINK_COMMIT_CONFIG_REJECT_CONTRADICTION: return "contradicts another staged field";
    case KILNLINK_COMMIT_CONFIG_REJECT_ARMED: return "relay is ARMED -- config writes are refused while ARMED";
    case KILNLINK_COMMIT_CONFIG_REJECT_STORAGE: return "the safety processor's flash write failed";
    /* 2026-09-15 (Opus adversarial re-review, F1): these two reject reasons
     * (link_task.c's tc_type heat-safety gate) existed on the wire since
     * a2384dd5 with no ESP-side case here, so they fell to the generic
     * default below -- readable as "unrecognised reason" instead of the
     * correct, actionable sentence. Both sentences deliberately contain
     * "ARMED" so the commissioning page's /ARMED/i match
     * (commissioning_shared.js, safety_commissioning_page.html) recognises
     * them as ARMED-family refusals too. */
    case KILNLINK_COMMIT_CONFIG_REJECT_ARMED_HEAT_ON:
        return "relay is ARMED and heat is on right now -- turn heat off and retry";
    case KILNLINK_COMMIT_CONFIG_REJECT_ARMED_HEAT_UNKNOWN:
        return "relay is ARMED and heat state is not currently known -- retry once it is";
    /* 2026-09-15 (Opus adversarial re-review, F2): the N4 "mixed change
     * while ARMED" sentence (config_store_flash.c) never reached the wire
     * until this reason existed -- see kilnlink_commit_config_rejected.h's
     * own comment on this value. */
    case KILNLINK_COMMIT_CONFIG_REJECT_ARMED_MIXED:
        return "relay is ARMED and this change also modifies field(s) other than "
               "thermocouple type -- only thermocouple type may change while ARMED";
    default: return "refused (unrecognised reason)";
    }
}

/* 2026-09-10 opus review finding: safety_ceiling_sync.c's reconcile backoff
 * used to classify a refusal by `strstr(reason, "ARMED")` against the
 * human-readable sentence above -- wrong on multiple counts (see
 * safety_ceiling_policy.h's block comment on the backoff for the full
 * list). The fix is to classify from the NUMERIC reject reason directly,
 * whenever it is actually known, and report SAFETY_CEILING_REFUSAL_OTHER
 * (never a guessed ARMED) whenever it is not. This is the one place that
 * numeric code exists on this side of the link -- reuse it, do not
 * re-derive a classification from prose anywhere else. */
/* 2026-09-15 review (review_divergence_fixes_b2e7017f_2026-09-15.md,
 * HIGH 2): the armed-refusal timestamp this handler records now lives in
 * safety_cfg_store (safety_cfg_store_note_armed_refusal() /
 * _recent_armed_refusal()) -- see item G in
 * review_divergence_wiring_60d6552f_2026-09-15.md for why it moved out of
 * this file. It is still set ONLY by commissioning_post_handler below, the
 * operator-facing entry point this is meant to explain -- not by every
 * internal safety_cfg_write_apply_pairs()/confirm_commit_landed() caller (e.g.
 * safety_ceiling_sync.c's own ceiling-raise retries back off on ARMED as a
 * matter of course and must not spuriously claim to explain an unrelated
 * standing-field mismatch). */

static safety_ceiling_refusal_class_t reject_reason_to_refusal_class(uint8_t reason)
{
    switch (reason) {
    case KILNLINK_COMMIT_CONFIG_REJECT_ARMED: return SAFETY_CEILING_REFUSAL_ARMED;
    case KILNLINK_COMMIT_CONFIG_REJECT_STORAGE: return SAFETY_CEILING_REFUSAL_STORAGE;
    case KILNLINK_COMMIT_CONFIG_REJECT_RANGE: return SAFETY_CEILING_REFUSAL_RANGE;
    case KILNLINK_COMMIT_CONFIG_REJECT_CONTRADICTION: return SAFETY_CEILING_REFUSAL_CONTRADICTION;
    /* 2026-09-15 (Opus adversarial re-review, F1): both are ARMED-family
     * refusals (the tc_type heat-safety gate refusing while ARMED for a
     * more specific reason than the plain ARMED case) -- classify them
     * that way so safety_ceiling_reconcile_record_result() uses the ARMED
     * fixed backoff instead of the generic OTHER retry backoff, which
     * otherwise retries sooner than intended. */
    case KILNLINK_COMMIT_CONFIG_REJECT_ARMED_HEAT_ON: return SAFETY_CEILING_REFUSAL_ARMED;
    case KILNLINK_COMMIT_CONFIG_REJECT_ARMED_HEAT_UNKNOWN: return SAFETY_CEILING_REFUSAL_ARMED;
    case KILNLINK_COMMIT_CONFIG_REJECT_ARMED_MIXED: return SAFETY_CEILING_REFUSAL_ARMED;
    default: return SAFETY_CEILING_REFUSAL_OTHER;
    }
}

/* Stages every pair via safety_link_send_set_param(), then (if `commit`)
 * sends COMMIT_CONFIG. Writes a human-readable outcome into reason_out
 * (always NUL-terminated if reason_cap > 0) and returns true only if every
 * stage succeeded AND (if requested) the commit was ACKed and ACCEPTED.
 *
 * COMMISSIONING.md sec 3.1 asks this endpoint to "name the offending field
 * and the rule it broke" on rejection -- SAFETY_CMD_COMMIT_CONFIG_REJECTED
 * (0x20) now carries exactly that (ROADMAP.md loose end, closed): a REJECTED
 * commit is no longer indistinguishable from an ACCEPTED one at this layer.
 * safety_link_send_commit_config()'s out_rejected/out_param_id/out_reason
 * report it; this function turns the param_id back into a field NAME via
 * safety_cfg_store_lookup() (the same table safety_cfg_store.c's ESP-side
 * cache uses) and the reason code into words via commit_reject_reason_words()
 * above, then reports both -- never "sent, awaiting confirmation" for a
 * rejection this build can now actually see. A param_id this build's own
 * table does not recognise (KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID, or
 * a real id from a newer Pico this ESP predates) falls back to reporting the
 * numeric id, same "refused individually... reported by numeric id" fallback
 * the unknown-id case just below already uses. */
/* param 0x0212 (estop_active_level) -- see safety_cfg_store.h's table entry
 * and discrete_pin_policy.h. A successful commit that includes this param
 * must invalidate any standing E-stop bench-verification record
 * (estop_verification.h): the operator's confirmation was made against a
 * specific polarity, and a re-send of this param -- even one that ends up
 * setting the SAME value -- is grounds to distrust a verification made
 * before this ESP can prove which polarity it was against. */
#define SAFETY_PARAM_ID_ESTOP_ACTIVE_LEVEL 0x0212u

/* `out_class` (may be NULL) receives the machine-readable refusal
 * classification whenever this returns false -- 2026-09-10 opus review
 * finding, same contract as confirm_commit_landed()'s own out_class.
 * Defaults to SAFETY_CEILING_REFUSAL_OTHER at entry (correct for every
 * failure path here except a directly-observed COMMIT_CONFIG_REJECTED
 * reply, which knows its numeric reason for certain and overrides it via
 * reject_reason_to_refusal_class() -- no guessing involved either way).
 *
 * `nonblocking_refetch` is threaded straight through to confirm_commit_
 * landed() -- see that function's own doc comment for the rule. Every
 * httpd-worker call site in this file goes through the safety_cfg_write_apply_pairs()
 * wrapper just below, which always passes false (unchanged blocking
 * behaviour); only safety_cfg_write_set_and_confirm_f32() (the ceiling-
 * reconcile writer, called from safety_poll_task) calls this function
 * directly with true.
 *
 * `volatile_install` (item 15, added 2026-09-14): when true, the commit leg
 * sends SAFETY_CMD_APPLY_CONFIG_VOLATILE (0x2D) instead of COMMIT_CONFIG --
 * installs into the Pico's live RAM record, never refused for ARMED, never
 * reaches flash. confirm_commit_landed() below needs no changes for this:
 * it only ever forces a live GET_CONFIG_PAGE re-fetch and compares values,
 * which is identical regardless of which command produced the live state.
 * Every existing caller passes false (unchanged behaviour); only kiln_cfg_
 * swap.c's swap-transaction callers (routed through safety_cfg_http_set_
 * and_confirm_f32_volatile()/apply_package_and_confirm(..., true, ...))
 * pass true. */
static bool apply_pairs_ex(SafetyLinkClass *link, const safety_cfg_post_pair_t *pairs, int n_pairs,
                            bool commit, bool volatile_install, char *reason_out, size_t reason_cap,
                            safety_ceiling_refusal_class_t *out_class, bool nonblocking_refetch)
{
    if (out_class) {
        *out_class = SAFETY_CEILING_REFUSAL_OTHER;
    }
    if (!link) {
        snprintf(reason_out, reason_cap, "safety link not available on this board");
        return false;
    }
    for (int i = 0; i < n_pairs; i++) {
        uint8_t type = 0;
        const char *name = NULL;
        if (!safety_cfg_store_lookup(pairs[i].param_id, &type, &name)) {
            /* COMMISSIONING.md sec 2: "unknown ids are refused individually
             * and named in the reply, rather than the whole transfer
             * failing" -- this IS that per-id refusal, reported by numeric
             * id since this build's table has no name for it. */
            snprintf(reason_out, reason_cap, "unknown parameter id %u", (unsigned)pairs[i].param_id);
            return false;
        }
        kilnlink_param_value_t value;
        if (!safety_cfg_write_parse_value_for_type(pairs[i].value_text, type, &value)) {
            snprintf(reason_out, reason_cap, "invalid value for %s (id %u)", name,
                     (unsigned)pairs[i].param_id);
            return false;
        }
        esp_err_t err = safety_link_send_set_param(link, pairs[i].param_id, type, value);
        if (err != ESP_OK) {
            snprintf(reason_out, reason_cap, "communication with the safety processor failed while "
                                              "staging %s (id %u): %s",
                     name, (unsigned)pairs[i].param_id, esp_err_to_name(err));
            return false;
        }
    }
    if (commit) {
        uint16_t reject_param_id = 0;
        uint8_t reject_reason = 0;
        bool rejected = false;
        esp_err_t err = volatile_install
                            ? safety_link_send_apply_config_volatile(link, &reject_param_id, &reject_reason, &rejected)
                            : safety_link_send_commit_config(link, &reject_param_id, &reject_reason, &rejected);
        if (err != ESP_OK) {
            snprintf(reason_out, reason_cap, "the safety processor did not acknowledge the commit "
                                              "(%s) -- values were staged but NOT written",
                     esp_err_to_name(err));
            return false;
        }
        if (rejected) {
            if (out_class) {
                /* Known for certain here (this is the direct, in-window
                 * reply -- no stash race involved), so this is the MORE
                 * reliable of the two classification sites in this file. */
                *out_class = reject_reason_to_refusal_class(reject_reason);
            }
            uint8_t reject_type = 0;
            const char *reject_name = NULL;
            if (reject_param_id != KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID &&
                safety_cfg_store_lookup(reject_param_id, &reject_type, &reject_name)) {
                snprintf(reason_out, reason_cap,
                         "commit rejected: %s (id %u) -- %s -- values were staged but NOT written",
                         reject_name, (unsigned)reject_param_id, commit_reject_reason_words(reject_reason));
            } else if (reject_param_id != KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID) {
                snprintf(reason_out, reason_cap,
                         "commit rejected: parameter id %u -- %s -- values were staged but NOT written",
                         (unsigned)reject_param_id, commit_reject_reason_words(reject_reason));
            } else {
                snprintf(reason_out, reason_cap, "commit rejected: %s -- values were staged but NOT written",
                         commit_reject_reason_words(reject_reason));
            }
            return false;
        }
        /* ESP_OK and not rejected within the reply window is NOT proof the
         * write landed (see confirm_commit_landed()'s header comment for the
         * full audit trail) -- force a live read-back before this function
         * is allowed to report success. */
        if (!confirm_commit_landed(link, pairs, n_pairs, reason_out, reason_cap, out_class, nonblocking_refetch)) {
            return false;
        }
        /* Landed for real -- now invalidate a standing E-stop verification if
         * estop_active_level was one of the committed params. See
         * SAFETY_PARAM_ID_ESTOP_ACTIVE_LEVEL's comment above for why this
         * fires on ANY commit of the param, not just a value change. */
        for (int i = 0; i < n_pairs; i++) {
            if (pairs[i].param_id == SAFETY_PARAM_ID_ESTOP_ACTIVE_LEVEL) {
                /* The clear's own result decides this POST's result. If the
                 * record could not be cleared (and estop_verification_clear()
                 * only returns ESP_OK once a read-back confirms it), a
                 * standing verified=1 survives a polarity commit and
                 * /api/readiness keeps reporting "confirmed by operator" for
                 * a polarity nobody verified. Answering {"ok":true} there
                 * would be this file's own "logging unchecked success" class
                 * -- the commit DID land, so the reason says so explicitly
                 * rather than implying the values were not written. */
                esp_err_t clear_err = estop_verification_clear();
                if (clear_err != ESP_OK) {
                    /* Kept inside reason[160] deliberately: the full
                     * "do not trust the readiness page" explanation lives in
                     * estop_verification.c's own ESP_LOGE, not here. */
                    snprintf(reason_out, reason_cap,
                             "estop_active_level committed, but the E-stop verification record "
                             "could NOT be cleared (%s) -- re-run the bench procedure",
                             esp_err_to_name(clear_err));
                    /* Not a Pico-ceiling refusal of any kind (the commit
                     * itself landed) -- explicit OTHER, overriding whatever
                     * confirm_commit_landed() left (irrelevant here, since
                     * it succeeded). */
                    if (out_class) {
                        *out_class = SAFETY_CEILING_REFUSAL_OTHER;
                    }
                    return false;
                }
                break;
            }
        }
    }
    if (out_class) {
        *out_class = SAFETY_CEILING_REFUSAL_NONE;
    }
    reason_out[0] = '\0';
    return true;
}

/* Every httpd-worker call site in this file calls THIS wrapper, unchanged --
 * always the blocking refetch (nonblocking_refetch=false), same behaviour
 * as before apply_pairs_ex() existed. */
bool safety_cfg_write_apply_pairs(SafetyLinkClass *link, const safety_cfg_post_pair_t *pairs, int n_pairs,
                         bool commit, char *reason_out, size_t reason_cap,
                         safety_ceiling_refusal_class_t *out_class)
{
    return apply_pairs_ex(link, pairs, n_pairs, commit, /*volatile_install=*/false, reason_out, reason_cap,
                           out_class, /*nonblocking_refetch=*/false);
}

/* Public single-field stage+commit+confirm wrapper -- owner request
 * 2026-09-10 ("if i change the max temp in the web gui it should change it
 * in the pico too."). safety_ceiling_sync.c (zones_http_post.c's helper)
 * uses this as the `safety_ceiling_writer_fn` callback for abs_max_temp_c
 * (param id 0x0104): it needs exactly this file's safety_cfg_write_apply_pairs()/confirm_
 * commit_landed() machinery -- stage, commit, and a live read-back that
 * proves the value actually landed, not merely that it was ACKed within the
 * reply window -- and does not deserve a second implementation of any of
 * that. Declared in safety_cfg_http.h.
 *
 * Single pair, always committed (`commit=true` is unconditional -- there is
 * no legitimate reason to stage this field without committing it). Reports
 * failure (including the ARMED refusal, verbatim via commit_reject_reason_
 * words()) through `reason_out`/`reason_cap` exactly like every other
 * caller of safety_cfg_write_apply_pairs() in this file. */
bool safety_cfg_write_set_and_confirm_f32(SafetyLinkClass *link, uint16_t param_id, float value,
                                          char *reason_out, size_t reason_cap,
                                          safety_ceiling_refusal_class_t *out_class)
{
    if (out_class) {
        *out_class = SAFETY_CEILING_REFUSAL_OTHER;
    }
    if (!reason_out || reason_cap == 0) {
        return false;
    }
    safety_cfg_post_pair_t pair;
    pair.param_id = param_id;
    snprintf(pair.value_text, sizeof(pair.value_text), "%.9g", (double)value);
    /* nonblocking_refetch=true -- this is the ceiling-reconcile writer,
     * called from safety_poll_task (safety_ceiling_sync.c), never from an
     * httpd worker. See confirm_commit_landed()'s doc comment for the rule
     * this must never violate: safety_poll_task may not block on
     * s_store_lock behind an httpd commissioning POST. */
    return apply_pairs_ex(link, &pair, 1, /*commit=*/true, /*volatile_install=*/false, reason_out, reason_cap,
                           out_class, /*nonblocking_refetch=*/true);
}

/* See this function's own doc comment in safety_cfg_http.h -- kiln_cfg_
 * swap.c's step 4 (raise-first) is the ONLY intended caller. Always blocking
 * (nonblocking_refetch=false): this always runs from a swap's own dedicated
 * worker task (see kiln_cfg_swap.h's TASK PLACEMENT note), never from
 * safety_poll_task, so the rule that function's doc comment states does not
 * apply here. */
bool safety_cfg_write_set_and_confirm_f32_volatile(SafetyLinkClass *link, uint16_t param_id, float value,
                                                   char *reason_out, size_t reason_cap,
                                                   safety_ceiling_refusal_class_t *out_class)
{
    if (out_class) {
        *out_class = SAFETY_CEILING_REFUSAL_OTHER;
    }
    if (!reason_out || reason_cap == 0) {
        return false;
    }
    safety_cfg_post_pair_t pair;
    pair.param_id = param_id;
    snprintf(pair.value_text, sizeof(pair.value_text), "%.9g", (double)value);
    return apply_pairs_ex(link, &pair, 1, /*commit=*/true, /*volatile_install=*/true, reason_out, reason_cap,
                           out_class, /*nonblocking_refetch=*/false);
}

/* Public bulk stage+commit+confirm wrapper -- docs/KILN_PROFILES_PLAN.md
 * item 5 (kiln_cfg_swap.c), built for the two-processor apply transaction's
 * step 6 ("PUSH the Pico half ... all 68 params staged via the existing
 * batched safety_cfg_write_apply_pairs()/confirm_commit_landed() machinery"). Reuses
 * apply_pairs_ex() verbatim -- no second write path.
 *
 * DELIBERATELY EXCLUDES the ceiling field (SAFETY_PARAM_ID_ABS_MAX_TEMP_C,
 * 0x0104) from the bulk push, always, regardless of its value in `pkg` --
 * kiln_cfg_swap.c drives that one field itself through safety_ceiling_
 * sync_guard_raise()/_apply_lower(), because the owner's "never a window
 * where the Pico's ceiling is below the ESP's" rule needs it ordered
 * relative to the ESP-side commit, which this bulk call has no visibility
 * into. Skips any entry without KILN_PKG_PARAM_FLAG_SET (never fetched on
 * the side that captured `pkg` -- nothing to push, not a fabricated 0/false,
 * same "set" discipline COMMISSIONING.md sec 3.1 states for the wire
 * itself).
 *
 * ITEM 15 LANDED 2026-09-14: `volatile_install` selects which command this
 * function's forced commit sends -- true for SAFETY_CMD_APPLY_CONFIG_
 * VOLATILE (0x2D, installs into the Pico's live RAM record, never refused
 * for ARMED, never reaches flash), false for the original COMMIT_CONFIG ->
 * config_store_write() flash path (still unconditionally refused while the
 * Pico is ARMED). kiln_cfg_swap.c passes true for the swap's own forward
 * push and rollback-restore (the owner's "Pico never leaves ARMED" rule)
 * and false only for its own best-effort, allowed-to-fail flash-fallback
 * persist step run after a swap has already verified -- see docs/audits/
 * kiln_swap_volatile_wiring_2026-09-14.md for the full ordering. */
bool safety_cfg_write_apply_package_and_confirm(SafetyLinkClass *link, const kiln_pkg_safety_t *pkg,
                                                bool volatile_install,
                                                char *reason_out, size_t reason_cap,
                                                safety_ceiling_refusal_class_t *out_class)
{
    if (out_class) {
        *out_class = SAFETY_CEILING_REFUSAL_OTHER;
    }
    if (!reason_out || reason_cap == 0) {
        return false;
    }
    if (!pkg) {
        snprintf(reason_out, reason_cap, "no package to push");
        return false;
    }
    if (pkg->count > SAFETY_CFG_POST_MAX_PAIRS) {
        snprintf(reason_out, reason_cap, "package has %u params, this build's bulk-push buffer holds %u",
                 (unsigned)pkg->count, (unsigned)SAFETY_CFG_POST_MAX_PAIRS);
        return false;
    }
    /* static, not stack -- SAFETY_CFG_POST_MAX_PAIRS (68) * sizeof(safety_
     * cfg_post_pair_t) is a few KB, and this file's httpd handlers already
     * avoid exactly this class of large local (see build_commissioning_
     * json()'s own EXT_RAM_BSS_ATTR scratch buffer comment above). The
     * transaction this feeds is single-flight by construction
     * (kiln_cfg_swap.c serializes swaps through kiln_cfg_store_lock()'s
     * generation check), so a single process-wide scratch buffer is safe --
     * it is never touched by two callers at once. */
    static safety_cfg_post_pair_t s_bulk_pairs[SAFETY_CFG_POST_MAX_PAIRS];
    int n = 0;
    for (uint16_t i = 0; i < pkg->count; i++) {
        const kiln_pkg_pico_param_t *e = &pkg->entries[i];
        if (e->param_id == SAFETY_PARAM_ID_ABS_MAX_TEMP_C) {
            continue;
        }
        if (!(e->flags & KILN_PKG_PARAM_FLAG_SET)) {
            continue;
        }
        s_bulk_pairs[n].param_id = e->param_id;
        switch (e->type) {
        case KILNLINK_PARAM_TYPE_BOOL:
        case KILNLINK_PARAM_TYPE_U8:
            snprintf(s_bulk_pairs[n].value_text, sizeof(s_bulk_pairs[n].value_text), "%u",
                     (unsigned)(e->value_bits & 0xFFu));
            break;
        case KILNLINK_PARAM_TYPE_U16:
            snprintf(s_bulk_pairs[n].value_text, sizeof(s_bulk_pairs[n].value_text), "%u",
                     (unsigned)(e->value_bits & 0xFFFFu));
            break;
        case KILNLINK_PARAM_TYPE_F32: {
            float f;
            memcpy(&f, &e->value_bits, sizeof(f));
            snprintf(s_bulk_pairs[n].value_text, sizeof(s_bulk_pairs[n].value_text), "%.9g", (double)f);
            break;
        }
        default:
            snprintf(reason_out, reason_cap, "package param id %u has an unknown wire type %u",
                     (unsigned)e->param_id, (unsigned)e->type);
            return false;
        }
        n++;
    }
    if (n == 0) {
        return true; /* nothing set to push -- not an error, just a no-op bulk write */
    }
    return apply_pairs_ex(link, s_bulk_pairs, n, /*commit=*/true, volatile_install, reason_out, reason_cap,
                           out_class, /*nonblocking_refetch=*/false);
}
