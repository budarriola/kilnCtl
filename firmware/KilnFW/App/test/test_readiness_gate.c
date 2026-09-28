// Host tests for App/drivers/safety/readiness_gate.h -- the firing interlock
// the owner asked for on 2026-09-09, closing the gap that /api/readiness
// gated NOTHING (readiness_http.h's own top comment, before this pass:
// "an operator can start a firing with any item on this page showing red").
//
// WHAT THIS FILE MUST PROVE, and why each part is here:
//
//  1. Each blocking item INDEPENDENTLY refuses a start. A gate that refuses
//     on any one condition would pass a test that only ever sets all four at
//     once, so every "blocks" test below sets exactly ONE fact bad and leaves
//     the other five in their ready state.
//
//  2. A fully-ready board is ALLOWED. This is the half that a badly-written
//     test skips: a gate hardwired to `return true` passes every refusal test
//     in this file and would brick the kiln. test_fully_ready_board_is_allowed()
//     and the cross-product's "no NOT_DONE item => no block" arm are what
//     stand between that mistake and the bench.
//
//  3. THE BICONDITIONAL the whole design rests on (readiness_gate.h's top
//     comment): the gate blocks item X if and only if item X's DISPLAYED
//     status -- the one readiness_http.c renders into /api/readiness -- is
//     READY_NOT_DONE. test_gate_and_display_agree_over_the_cross_product()
//     walks all 256 fact combinations and checks both directions, plus that
//     the item NAMED is the first NOT_DONE one in gate order. This is the
//     honest form of "a check that fails if the gate and the display can
//     disagree" for the DECISION side; check_readiness_gate_display_agreement.ps1
//     covers the wiring side (that readiness_http.c still feeds these same
//     predicates into these same item keys).
//
// FAKES: readiness_gate.h declares readiness_gate_collect() and defines
// everything else as static inline, exactly so this file can supply that one
// body and reach the real decision.
//
// Own, separate executable (own main()): this file defines a body for
// readiness_gate_collect(), which readiness_gate.c also defines -- linking
// both into one binary would be a duplicate symbol.
//
// NEGATIVE TEST (performed 2026-09-09, RED confirmed, restored by hand):
// changing readiness_gate.h's estop branch from
//     readiness_estop_verification_status(f->estop_verified) == READY_NOT_DONE
// to
//     false
// (i.e. the pre-2026-09-09 behaviour, where estop_verified was advisory only)
// fails this file with:
//   FAIL test_readiness_gate.c:<N>: an unverified E-stop interlock refuses the start
//   FAIL test_readiness_gate.c:<N>: gate blocks <=> some displayed item is NOT_DONE
// Restore by reversing the edit textually; `git diff` on readiness_gate.h
// then comes back empty. Do NOT use git stash/checkout/restore to undo it --
// this tree is shared with other sessions.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"

int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/safety/readiness_gate.h"

/* The fake board. readiness_gate_collect() below hands this struct straight
 * back, so readiness_gate_refuses_start() -- the wrapper the real start paths
 * call -- runs its real body over facts this file controls. */
static readiness_gate_facts_t s_fake_facts;

void readiness_gate_collect(readiness_gate_facts_t *out)
{
    if (out != NULL) {
        *out = s_fake_facts;
    }
}

/* The state of a board with nothing wrong with it: not in recovery mode, a
 * live safety link with no trip latched, no stored crash record, and an
 * E-stop interlock a human has verified. Every "one thing is wrong" test
 * below starts from exactly this and changes ONE field. */
static void set_fully_ready(void)
{
    memset(&s_fake_facts, 0, sizeof(s_fake_facts));
    s_fake_facts.recovery_mode = false;
    s_fake_facts.safety_link_up = true;
    s_fake_facts.safety_trip_mask = 0u;
    s_fake_facts.crash_have_record = false;
    s_fake_facts.crash_acknowledged = false;
    s_fake_facts.estop_verified = true;
    /* 2026-09-14: the ceiling-match item, promoted into this gate the same
     * way estop_verified was. Not diverged -- the two sides agree. */
    s_fake_facts.ceiling_diverged = false;
    /* docs/PICO_AUTO_UPDATE_PLAN.md's new item: not blocked, i.e. the
     * Pico's firmware version matches (or the boot-time glue has not
     * flagged an unrecoverable mismatch). */
    s_fake_facts.pico_update_blocked = false;
    /* docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md's new item. PASS is the only
     * fact that reads READY_OK; on the actual bench fixture the reachable
     * value is INCONCLUSIVE, which deliberately does NOT block a start --
     * test_ct_attribution_non_fail_verdicts_do_not_block() below is the half
     * of the owner's rule that a FAIL-only test would miss. */
    s_fake_facts.ct_attribution = READINESS_CT_ATTR_PASS;
}

/* ---- 2. the allowed case ------------------------------------------------- */

static void test_fully_ready_board_is_allowed(void)
{
    TEST_SECTION("a fully-ready board is ALLOWED to start (the half a refuse-everything gate fails)");
    set_fully_ready();
    char msg[192];
    strcpy(msg, "untouched");
    readiness_gate_block_t which = READINESS_GATE_BLOCK_ESTOP_VERIFIED;
    bool refused = readiness_gate_refuses_start(msg, sizeof(msg), &which);
    TEST_CHECK(!refused, "a fully-ready board is not refused");
    TEST_CHECK(which == READINESS_GATE_OK, "out_which reads READINESS_GATE_OK when nothing blocks");
    TEST_CHECK(strcmp(msg, "untouched") == 0,
               "the refusal message is left untouched when nothing blocks");
}

/* ---- 1. each item, independently ----------------------------------------- */

/* Every refusal must NAME its item, or the operator gets a board that says no
 * and nothing else -- the "reads as a broken board" failure this task exists
 * to avoid. `needle` is checked case-insensitively-ish by exact substring on
 * the distinctive uppercase word each message front-loads. */
static void check_one_blocking_item(const char *what, readiness_gate_block_t expect, const char *needle)
{
    char msg[192];
    memset(msg, 0, sizeof(msg));
    readiness_gate_block_t which = READINESS_GATE_OK;
    bool refused = readiness_gate_refuses_start(msg, sizeof(msg), &which);
    TEST_CHECK(refused, what);
    TEST_CHECK(which == expect, "the refusal names the correct item");
    TEST_CHECK(msg[0] != '\0', "the refusal writes an operator-facing message");
    TEST_CHECK(strstr(msg, needle) != NULL, "the message says WHICH condition blocked the start");
    TEST_CHECK(strstr(msg, needle) - msg < 80,
               "the item is front-loaded, so a truncating LCD dialog still shows it");
}

static void test_recovery_mode_alone_refuses(void)
{
    TEST_SECTION("recovery mode alone refuses a start");
    set_fully_ready();
    s_fake_facts.recovery_mode = true;
    check_one_blocking_item("a recovery-mode boot refuses the start", READINESS_GATE_BLOCK_RECOVERY_MODE,
                            "RECOVERY MODE");
}

static void test_safety_trip_alone_refuses(void)
{
    TEST_SECTION("a latched safety trip alone refuses a start");
    set_fully_ready();
    s_fake_facts.safety_trip_mask = 0x0020u; /* SAFETY_TRIP_MAIN_FAULT, S6a (trip_mask = 1 << (reason-1) = 1 << 5) -- what a reboot latches */
    check_one_blocking_item("a latched safety trip refuses the start", READINESS_GATE_BLOCK_SAFETY_TRIP,
                            "TRIP");
}

static void test_crash_report_alone_refuses(void)
{
    TEST_SECTION("an unacknowledged crash report alone refuses a start");
    set_fully_ready();
    s_fake_facts.crash_have_record = true;
    s_fake_facts.crash_acknowledged = false;
    check_one_blocking_item("an unacknowledged crash report refuses the start",
                            READINESS_GATE_BLOCK_CRASH_REPORT, "UNACKNOWLEDGED CRASH REPORT");
}

static void test_estop_unverified_alone_refuses(void)
{
    TEST_SECTION("an unverified E-stop interlock alone refuses a start (the NEW enforcement)");
    set_fully_ready();
    s_fake_facts.estop_verified = false;
    check_one_blocking_item("an unverified E-stop interlock refuses the start",
                            READINESS_GATE_BLOCK_ESTOP_VERIFIED, "E-STOP INTERLOCK");
}

static void test_ceiling_divergence_alone_refuses(void)
{
    /* 2026-09-14 owner decision, verbatim: "if a config doesn't land and
     * match on both sides then alarm and dissable heaters" -- this gate's
     * own item is the display/start-blocking half of that; the ACTIVE
     * heaters-off half lives in safety_ceiling_sync.c's enforcement, driven
     * by the exact same safety_ceiling_sync_is_diverged() verdict this
     * fact is a stand-in for (config_divergence.h's format-version+hash
     * identity check covers a numeric mismatch, an unconfirmed/"unarmed"
     * Pico, and a hash/version mismatch all under this one boolean --
     * config_divergence.h's own host tests cover those cases at the
     * comparator level; this gate only needs to prove it reacts to the
     * boolean correctly). */
    TEST_SECTION("a safety config divergence alone refuses a start (the NEW 2026-09-14 enforcement)");
    set_fully_ready();
    s_fake_facts.ceiling_diverged = true;
    check_one_blocking_item("a ceiling divergence refuses the start", READINESS_GATE_BLOCK_CEILING_MISMATCH,
                            "ceiling");
}

static void test_pico_update_blocked_alone_refuses(void)
{
    /* docs/PICO_AUTO_UPDATE_PLAN.md owner decision, verbatim: "on an
     * unrecoverable version mismatch the ESP refuses to fire until
     * matched" -- an actual block, not a warning surface. This gate's item
     * is the enforcement half of that; pico_auto_update_state_is_blocking()
     * (currently a stub returning false until the boot-time glue lands) is
     * the same live verdict readiness_http.c's "pico_update" item shows. */
    TEST_SECTION("an unrecoverable Pico version mismatch alone refuses a start (the NEW enforcement)");
    set_fully_ready();
    s_fake_facts.pico_update_blocked = true;
    check_one_blocking_item("a blocked Pico auto-update refuses the start", READINESS_GATE_BLOCK_PICO_UPDATE,
                            "VERSION");
}

static void test_ct_attribution_fail_alone_refuses(void)
{
    /* docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md, owner decision: "a FAIL
     * blocks both the step and firing". A FAIL means a current clamp is not
     * on the conductor the configuration names, so the over-current guard is
     * aimed at the wrong zone -- it reads as armed and healthy while
     * protecting nothing. */
    TEST_SECTION("a FAILED CT attribution alone refuses a start (the NEW enforcement)");
    set_fully_ready();
    s_fake_facts.ct_attribution = READINESS_CT_ATTR_FAIL;
    check_one_blocking_item("a failed CT attribution refuses the start",
                            READINESS_GATE_BLOCK_CT_ATTRIBUTION, "CT ATTRIBUTION");
}

/* ---- the cases that must NOT block --------------------------------------- */

/* THE OTHER HALF of the owner's rule, and the half a careless implementation
 * gets wrong: an INCONCLUSIVE verdict blocks only the wizard step, never the
 * firing interlock. On the ~4 W bench fixture every zone draws about 23 mA
 * against a 45 mA sweep floor, so INCONCLUSIVE is the ONLY outcome this
 * hardware can produce -- a gate that blocked on it could never fire at all.
 * NEVER_RUN and STALE ride the same rule: neither is evidence of miswiring,
 * and a board that has simply never run the step must not be bricked by it. */
static void test_ct_attribution_non_fail_verdicts_do_not_block(void)
{
    TEST_SECTION("INCONCLUSIVE / STALE / never-run CT attribution never blocks firing");
    readiness_ct_attribution_fact_t non_blocking[] = {
        READINESS_CT_ATTR_NOT_INSTALLED,
        READINESS_CT_ATTR_NEVER_RUN,
        READINESS_CT_ATTR_STALE,
        READINESS_CT_ATTR_INCONCLUSIVE,
        READINESS_CT_ATTR_PASS,
    };
    for (size_t i = 0; i < sizeof(non_blocking) / sizeof(non_blocking[0]); i++) {
        set_fully_ready();
        s_fake_facts.ct_attribution = non_blocking[i];
        char msg[192];
        strcpy(msg, "untouched");
        TEST_CHECK(!readiness_gate_refuses_start(msg, sizeof(msg), NULL),
                   "a CT attribution verdict short of FAIL does not refuse a start");
        TEST_CHECK(strcmp(msg, "untouched") == 0, "no message written when nothing blocks");
    }
    /* And the asymmetry is real, not an accident of all five reading OK: the
     * three "cannot yet" values must still be visibly NOT ready on the page,
     * which is what blocks the wizard step. */
    TEST_CHECK(readiness_ct_attribution_status(READINESS_CT_ATTR_INCONCLUSIVE) == READY_CANNOT_YET,
               "INCONCLUSIVE displays as CANNOT_YET -- blocks the wizard step, not the firing");
    TEST_CHECK(readiness_ct_attribution_status(READINESS_CT_ATTR_STALE) == READY_CANNOT_YET,
               "a STALE verdict displays as CANNOT_YET, never as the verdict it once was");
    TEST_CHECK(readiness_ct_attribution_status(READINESS_CT_ATTR_NEVER_RUN) == READY_CANNOT_YET,
               "a never-run check displays as CANNOT_YET");
    TEST_CHECK(readiness_ct_attribution_status(READINESS_CT_ATTR_FAIL) == READY_NOT_DONE,
               "and only FAIL reads NOT_DONE, which is what this gate blocks on");
}


static void test_acknowledged_crash_does_not_block(void)
{
    TEST_SECTION("an ACKNOWLEDGED crash report does not block -- the operator has an action and took it");
    set_fully_ready();
    s_fake_facts.crash_have_record = true;
    s_fake_facts.crash_acknowledged = true;
    char msg[192];
    strcpy(msg, "untouched");
    TEST_CHECK(!readiness_gate_refuses_start(msg, sizeof(msg), NULL),
               "a reviewed and acknowledged crash record does not refuse a start");
    TEST_CHECK(strcmp(msg, "untouched") == 0, "no message written when nothing blocks");
}

static void test_link_down_is_not_this_gates_refusal(void)
{
    /* readiness_safety_trip_status() maps a down link to CANNOT_YET, not
     * NOT_DONE, deliberately: with the link down the ESP cannot tell "not
     * tripped" from "do not know", and profile_executor_run()'s
     * relay_authority_on_blocked() check already refuses a down-link start in
     * far better words ("heat is blocked ... usually the safety link down").
     * If this gate ALSO blocked here it would report the wrong item for the
     * most common bench failure. This test pins that choice so a future
     * "tighten the gate" edit has to argue with it explicitly. */
    TEST_SECTION("a DOWN safety link is CANNOT_YET, not this gate's refusal (relay_authority owns it)");
    set_fully_ready();
    s_fake_facts.safety_link_up = false;
    s_fake_facts.safety_trip_mask = 0x0040u; /* unknowable while the link is down */
    TEST_CHECK(readiness_safety_trip_status(false, 0x0040u) == READY_CANNOT_YET,
               "precondition: the shared predicate calls a down link CANNOT_YET");
    TEST_CHECK(!readiness_gate_refuses_start(NULL, 0, NULL),
               "a down safety link is not refused BY THIS GATE (relay_authority_on_blocked() is)");
}

static void test_null_facts_are_not_a_green_light(void)
{
    TEST_SECTION("NULL facts refuse -- a caller that cannot produce facts must not be able to fire");
    char msg[192];
    memset(msg, 0, sizeof(msg));
    readiness_gate_block_t which = READINESS_GATE_OK;
    which = readiness_gate_evaluate(NULL, msg, sizeof(msg));
    TEST_CHECK(which != READINESS_GATE_OK, "readiness_gate_evaluate(NULL, ...) blocks");
    TEST_CHECK(msg[0] != '\0', "and still says something to the operator");
}

static void test_no_message_buffer_does_not_change_the_decision(void)
{
    TEST_SECTION("passing NULL/0 for the message buffer changes nothing but the message");
    set_fully_ready();
    s_fake_facts.estop_verified = false;
    TEST_CHECK(readiness_gate_refuses_start(NULL, 0, NULL),
               "still refused with no message buffer");
    set_fully_ready();
    TEST_CHECK(!readiness_gate_refuses_start(NULL, 0, NULL),
               "still allowed with no message buffer");
}

/* ---- 3. the biconditional ------------------------------------------------ */

/* The DISPLAY side, computed exactly the way readiness_http.c's handler
 * computes it for these four items: same predicates, same arguments. If the
 * gate can ever disagree with this, the operator sees a green checklist over
 * a board that refuses to fire, or a red one over a board that fires. */
static readiness_gate_block_t first_not_done_item(const readiness_gate_facts_t *f)
{
    if (readiness_recovery_mode_status(f->recovery_mode) == READY_NOT_DONE) {
        return READINESS_GATE_BLOCK_RECOVERY_MODE;
    }
    if (readiness_safety_trip_status(f->safety_link_up, f->safety_trip_mask) == READY_NOT_DONE) {
        return READINESS_GATE_BLOCK_SAFETY_TRIP;
    }
    if (readiness_crash_report_status(f->crash_have_record, f->crash_acknowledged) == READY_NOT_DONE) {
        return READINESS_GATE_BLOCK_CRASH_REPORT;
    }
    if (readiness_estop_verification_status(f->estop_verified) == READY_NOT_DONE) {
        return READINESS_GATE_BLOCK_ESTOP_VERIFIED;
    }
    if (readiness_ceiling_match_status(f->safety_link_up, f->ceiling_diverged) == READY_NOT_DONE) {
        return READINESS_GATE_BLOCK_CEILING_MISMATCH;
    }
    if (readiness_pico_update_status(f->pico_update_blocked) == READY_NOT_DONE) {
        return READINESS_GATE_BLOCK_PICO_UPDATE;
    }
    if (readiness_ct_attribution_status(f->ct_attribution) == READY_NOT_DONE) {
        return READINESS_GATE_BLOCK_CT_ATTRIBUTION;
    }
    return READINESS_GATE_OK;
}

static void test_gate_and_display_agree_over_the_cross_product(void)
{
    /* 256 boolean combinations x the 6 values of the CT attribution fact,
     * which is an enum, not a bool -- a cross product that only walked its
     * false/true would never exercise STALE or INCONCLUSIVE, the two values
     * the owner's asymmetry actually turns on. */
    TEST_SECTION("gate blocks item X <=> item X's DISPLAYED status is NOT_DONE (all 1536 combinations)");
    int mismatches = 0;
    int blocked = 0;
    int allowed = 0;
    for (unsigned bits = 0; bits < 256u; bits++) {
        readiness_gate_facts_t f;
        memset(&f, 0, sizeof(f));
        f.recovery_mode = (bits & 1u) != 0u;
        f.safety_link_up = (bits & 2u) != 0u;
        f.safety_trip_mask = (bits & 4u) ? 0x0040u : 0u;
        f.crash_have_record = (bits & 8u) != 0u;
        f.crash_acknowledged = (bits & 16u) != 0u;
        f.estop_verified = (bits & 32u) != 0u;
        f.ceiling_diverged = (bits & 64u) != 0u;
        f.pico_update_blocked = (bits & 128u) != 0u;

        for (unsigned ct = 0; ct <= (unsigned)READINESS_CT_ATTR_FAIL; ct++) {
            f.ct_attribution = (readiness_ct_attribution_fact_t)ct;

            readiness_gate_block_t expect = first_not_done_item(&f);
            readiness_gate_block_t got = readiness_gate_evaluate(&f, NULL, 0);
            if (got != expect) {
                mismatches++;
            }
            if (expect == READINESS_GATE_OK) {
                allowed++;
            } else {
                blocked++;
            }
        }
    }
    TEST_CHECK(blocked + allowed == 256 * 6,
               "every combination was walked, CT attribution's six values included");
    TEST_CHECK(mismatches == 0, "gate blocks <=> some displayed item is NOT_DONE, and names the same item");
    /* Both arms must actually be exercised, or "0 mismatches" is vacuous --
     * this is the same lesson as the vacuous-check history in CLAUDE.md. */
    TEST_CHECK(blocked > 0, "the cross product contains blocking combinations (not a vacuous pass)");
    TEST_CHECK(allowed > 0, "the cross product contains allowed combinations (not a refuse-everything gate)");
}

/* The four keys the gate names must be the four keys /api/readiness renders.
 * The .ps1 check does the real grep of readiness_http.c; this pins the string
 * constants themselves so a rename has to change both files or fail here. */
static void test_gate_keys_match_the_api_item_keys(void)
{
    TEST_SECTION("the gate's item keys are the /api/readiness item keys");
    TEST_CHECK(strcmp(READINESS_GATE_KEY_RECOVERY_MODE, "recovery_mode") == 0, "recovery_mode key");
    TEST_CHECK(strcmp(READINESS_GATE_KEY_SAFETY_TRIP, "safety_trip") == 0, "safety_trip key");
    TEST_CHECK(strcmp(READINESS_GATE_KEY_CRASH_REPORT, "crash_report") == 0, "crash_report key");
    TEST_CHECK(strcmp(READINESS_GATE_KEY_ESTOP, "estop_verified") == 0, "estop_verified key");
    TEST_CHECK(strcmp(READINESS_GATE_KEY_CEILING_MATCH, "safety_ceiling_match") == 0, "safety_ceiling_match key");
    TEST_CHECK(strcmp(READINESS_GATE_KEY_PICO_UPDATE, "pico_update") == 0, "pico_update key");
    TEST_CHECK(strcmp(READINESS_GATE_KEY_CT_ATTRIBUTION, "ct_attribution") == 0, "ct_attribution key");
}

/* dashboard_exec_http.c embeds these messages verbatim into a JSON body,
 * WITHOUT escaping -- deliberately, because escaping would need a doubled
 * scratch buffer on the shared 8 KB httpd stack (the httpd-stack-blob class)
 * to guard against an input that cannot occur. "Cannot occur" has to be
 * enforced somewhere, or the first refusal message someone writes with a
 * quote in it produces malformed JSON that the dashboard's r.json() throws
 * on -- and the operator sees the Start button do nothing at all. This is
 * that enforcement. It also checks the message fits the buffer the HTTP
 * handler sizes for it. */
static void test_messages_are_json_safe(void)
{
    TEST_SECTION("every refusal message is JSON-safe and fits the response buffer (no escaping is done)");
    readiness_gate_block_t all[] = {
        READINESS_GATE_BLOCK_RECOVERY_MODE,
        READINESS_GATE_BLOCK_SAFETY_TRIP,
        READINESS_GATE_BLOCK_CRASH_REPORT,
        READINESS_GATE_BLOCK_ESTOP_VERIFIED,
        READINESS_GATE_BLOCK_CEILING_MISMATCH,
        READINESS_GATE_BLOCK_PICO_UPDATE,
        READINESS_GATE_BLOCK_CT_ATTRIBUTION,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        set_fully_ready();
        switch (all[i]) {
        case READINESS_GATE_BLOCK_RECOVERY_MODE: s_fake_facts.recovery_mode = true; break;
        case READINESS_GATE_BLOCK_SAFETY_TRIP: s_fake_facts.safety_trip_mask = 0x0040u; break;
        case READINESS_GATE_BLOCK_CRASH_REPORT: s_fake_facts.crash_have_record = true; break;
        case READINESS_GATE_BLOCK_CEILING_MISMATCH: s_fake_facts.ceiling_diverged = true; break;
        case READINESS_GATE_BLOCK_PICO_UPDATE: s_fake_facts.pico_update_blocked = true; break;
        case READINESS_GATE_BLOCK_CT_ATTRIBUTION: s_fake_facts.ct_attribution = READINESS_CT_ATTR_FAIL; break;
        default: s_fake_facts.estop_verified = false; break;
        }
        char msg[192];
        memset(msg, 0, sizeof(msg));
        readiness_gate_block_t which = readiness_gate_evaluate(&s_fake_facts, msg, sizeof(msg));
        TEST_CHECK(which == all[i], "precondition: the intended item is the one that blocked");
        TEST_CHECK(strchr(msg, '"') == NULL, "the message contains no double quote (it is not escaped)");
        TEST_CHECK(strchr(msg, '\\') == NULL, "the message contains no backslash (it is not escaped)");
        /* 191 chars + NUL is the most dashboard_exec_http.c's recovery_err[192]
         * can carry; a message longer than that would be silently truncated
         * mid-sentence in the operator's refusal. */
        TEST_CHECK(strlen(msg) < 191, "the message fits the HTTP handler's 192-byte refusal buffer");
        TEST_CHECK(readiness_gate_item_key(which) != NULL, "the blocking item maps to an /api/readiness key");
    }
    TEST_CHECK(readiness_gate_item_key(READINESS_GATE_OK) == NULL,
               "READINESS_GATE_OK maps to no item key -- nothing blocked, nothing to point at");
}

int main(void)
{
    test_fully_ready_board_is_allowed();
    test_recovery_mode_alone_refuses();
    test_safety_trip_alone_refuses();
    test_crash_report_alone_refuses();
    test_estop_unverified_alone_refuses();
    test_ceiling_divergence_alone_refuses();
    test_pico_update_blocked_alone_refuses();
    test_acknowledged_crash_does_not_block();
    test_link_down_is_not_this_gates_refusal();
    test_null_facts_are_not_a_green_light();
    test_no_message_buffer_does_not_change_the_decision();
    test_gate_and_display_agree_over_the_cross_product();
    test_ct_attribution_fail_alone_refuses();
    test_ct_attribution_non_fail_verdicts_do_not_block();
    test_gate_keys_match_the_api_item_keys();
    test_messages_are_json_safe();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
