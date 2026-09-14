// readiness_gate -- the FIRING INTERLOCK built on the readiness checklist.
//
// WHY THIS EXISTS (owner decision, 2026-09-09). Until this module,
// /api/readiness gated NOTHING. readiness_http.h's own top comment said so
// in as many words: the only consumers were two browser pages, neither
// profile_executor_run()'s start path nor tools/PcTools' capability_preflight
// read any item, and an operator could start a firing with every light on
// the page red. Three of the four conditions below had independent
// enforcement elsewhere; `estop_verified` had none at all and was explicitly
// advisory. The owner's decision was to make the checklist a real gate.
//
// NO OVERRIDE. There is deliberately no password bypass, no confirm-dialog
// escape hatch and no "force" parameter anywhere in this module or its
// callers. If the E-stop interlock has not been verified, no firing. Do not
// add one; if you believe one is needed, raise it with the owner rather than
// implementing it.
//
// ONE DEFINITION OF EACH BLOCKING RULE -- read this before adding an item.
// This gate does NOT decide, for itself, what "tripped" or "unverified"
// means. Every item below is decided by calling the SAME
// readiness_*_status() pure predicate readiness_http.c's JSON handler calls
// to render that item, and blocks on exactly one condition:
//
//     the gate blocks item X  <=>  item X's displayed status is READY_NOT_DONE
//
// That biconditional is the whole design. This codebase has been burned four
// separate times by the "reset one side of a pair" class (CLAUDE.md's
// standing note): two pieces of state joined by a semantic contract that is
// never expressed as a shared function, each written independently, drifting
// apart silently. A gate that re-implemented "is the safety processor
// tripped?" next to a checklist that already answers it would be exactly
// that shape -- and its failure mode is the worst kind: a green page over a
// board that refuses to fire, or (far worse) a red page over a board that
// fires anyway. Making the gate a thin READY_NOT_DONE test over the shared
// predicates makes the two physically incapable of disagreeing.
// check_readiness_gate_display_agreement.ps1 enforces that this stays true,
// and test_readiness_gate.c proves the biconditional over the full cross
// product of facts.
//
// WHAT IS DELIBERATELY *NOT* GATED: guard_max_temp, hardware, safety_context,
// cfg_fs, network, commissioning and the rest stay advisory, per the same
// owner decision. guard_max_temp in particular already has its own, better
// refusal deeper in profile_executor_run() (zones_config_accessors.h's
// guard-5 refusal) which knows WHICH zones this profile actually uses; the
// checklist item is a whole-board summary and would refuse firings the
// guard-5 check correctly allows. Adding an item to readiness_http.c does
// NOT add it here -- that is intentional, and the agreement check above only
// binds the four keys named in READINESS_GATE_KEY_* below.
//
// LAYERING: this header is PURE (facts in, decision out) so it can be
// host-tested and so profile_executor_run.c can call it without dragging in
// esp_http_server. readiness_gate_collect() -- the one thing that actually
// touches the board -- is declared here and defined in readiness_gate.c,
// which is target-only. Host tests supply their own body for it, the same
// convention test_recovery_start_refusal.c uses for
// boot_guard_is_recovery_mode().
#ifndef READINESS_GATE_H
#define READINESS_GATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "readiness_http.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which item refused, or READINESS_GATE_OK. Ordered as the operator should
 * act on them: recovery mode first (nothing else can be fixed until the
 * board boots normally), then the two "something went wrong" conditions,
 * then the one standing prerequisite. */
typedef enum {
    READINESS_GATE_OK = 0,
    READINESS_GATE_BLOCK_RECOVERY_MODE,
    READINESS_GATE_BLOCK_SAFETY_TRIP,
    READINESS_GATE_BLOCK_CRASH_REPORT,
    READINESS_GATE_BLOCK_ESTOP_VERIFIED,
    /* 2026-09-14 owner decision: "the Pico ceiling must ALWAYS equal the
     * ESP's ... divergence is a fault, not a quiet mismatch" -- promoted
     * from advisory (readiness_http.c's "safety_ceiling_match" item) into
     * this gate's blocking set, the same way estop_verified was promoted
     * on 2026-09-09. Ordered last: the operator can only act on it once the
     * safety link is actually up (the other four conditions are either
     * link-independent or already surface a down link first). */
    READINESS_GATE_BLOCK_CEILING_MISMATCH,
} readiness_gate_block_t;

/* The `key` strings /api/readiness uses for these same items. The
 * agreement check greps readiness_http.c's append_item() calls for exactly
 * these, so an item renamed on one side and not the other fails the repo
 * checks rather than silently un-gating a firing. */
#define READINESS_GATE_KEY_RECOVERY_MODE "recovery_mode"
#define READINESS_GATE_KEY_SAFETY_TRIP   "safety_trip"
#define READINESS_GATE_KEY_CRASH_REPORT  "crash_report"
#define READINESS_GATE_KEY_ESTOP         "estop_verified"
#define READINESS_GATE_KEY_CEILING_MATCH "safety_ceiling_match"

/* The /api/readiness item key a refusal corresponds to, or NULL for
 * READINESS_GATE_OK. Exists so a refusal can hand the operator's browser the
 * exact item to look at rather than dropping them on a checklist of eighteen
 * and letting them find it: main_page.html's start handler puts this in the
 * response as "readiness_item". Returning the SAME constants the page renders
 * is the point -- check_readiness_gate_display_agreement.ps1 holds both ends
 * of that to the same four keys. */
static inline const char *readiness_gate_item_key(readiness_gate_block_t which)
{
    switch (which) {
    case READINESS_GATE_BLOCK_RECOVERY_MODE: return READINESS_GATE_KEY_RECOVERY_MODE;
    case READINESS_GATE_BLOCK_SAFETY_TRIP:   return READINESS_GATE_KEY_SAFETY_TRIP;
    case READINESS_GATE_BLOCK_CRASH_REPORT:  return READINESS_GATE_KEY_CRASH_REPORT;
    case READINESS_GATE_BLOCK_ESTOP_VERIFIED: return READINESS_GATE_KEY_ESTOP;
    case READINESS_GATE_BLOCK_CEILING_MISMATCH: return READINESS_GATE_KEY_CEILING_MATCH;
    case READINESS_GATE_OK:
    default:
        return NULL;
    }
}

/* Every board fact the four blocking items need, and nothing else. Passed by
 * value rather than read from globals inside the decision so the decision
 * itself is pure and the full cross product is testable. Field meanings are
 * exactly the arguments of the readiness_*_status() predicates they feed --
 * see readiness_http.h for each one's own doc comment. */
typedef struct {
    bool     recovery_mode;      /* boot_guard_is_recovery_mode() */
    bool     safety_link_up;     /* dashboard_http_get_safety_trip()'s link_up */
    uint16_t safety_trip_mask;   /* dashboard_http_get_safety_trip()'s diag_trip_mask */
    bool     crash_have_record;  /* crash_report_get() returned true */
    bool     crash_acknowledged; /* that record's acknowledged flag */
    bool     estop_verified;     /* estop_verification_is_verified() */
    bool     ceiling_diverged;  /* safety_ceiling_sync_is_diverged() -- the SAME verdict the enforcement acts on */
} readiness_gate_facts_t;

/* Reads the six facts above off the live board. Target-only
 * (readiness_gate.c); host tests define their own body. Safe to call before
 * any of the modules it reads have been started -- each underlying accessor
 * documents its own not-yet-started answer, and every one of them fails in
 * the direction of "unknown", never "green". */
void readiness_gate_collect(readiness_gate_facts_t *out);

/* The decision. Returns the FIRST blocking item (there is no value in
 * listing all four: the operator has to clear them one at a time anyway, and
 * a truncating LCD dialog cannot show more than one), or READINESS_GATE_OK.
 *
 * When something blocks and msg/cap are non-NULL, writes an operator-facing
 * refusal that (a) names the item and (b) says what to do about it. The
 * shortest UI caller's buffer truncates, so every message here is
 * FRONT-LOADED: the item's name comes first, the remedy second, so a
 * truncated message still tells the operator which item refused. Leaves msg
 * untouched when nothing blocks (same convention as
 * recovery_mode_refuses_start()). */
static inline readiness_gate_block_t readiness_gate_evaluate(const readiness_gate_facts_t *f, char *msg,
                                                             size_t cap)
{
    readiness_gate_block_t which = READINESS_GATE_OK;
    const char *text = NULL;

    if (f == NULL) {
        /* No facts is not a green light. A caller that cannot produce the
         * facts must not be able to start a firing by passing NULL. */
        which = READINESS_GATE_BLOCK_ESTOP_VERIFIED;
        text = "refused -- readiness facts unavailable; the firing interlock cannot confirm this "
               "board is ready (see the Readiness page)";
    } else if (readiness_recovery_mode_status(f->recovery_mode) == READY_NOT_DONE) {
        which = READINESS_GATE_BLOCK_RECOVERY_MODE;
        /* Single quotes, not escaped double quotes: these messages are
         * embedded verbatim into a JSON body by dashboard_exec_http.c, which
         * runs on the shared 8 KB httpd stack and therefore cannot afford the
         * doubled scratch buffer a general JSON escape would need. No message
         * in this function may contain a '"' or a '\\'.
         * test_readiness_gate.c's test_messages_are_json_safe() enforces it,
         * so a future edit that reintroduces one fails a test rather than
         * emitting a malformed refusal the dashboard silently drops. */
        text = "refused -- RECOVERY MODE boot: firing is not available this boot. Use 'Exit recovery "
               "mode & reboot now' on the Firmware update page.";
    } else if (readiness_safety_trip_status(f->safety_link_up, f->safety_trip_mask) == READY_NOT_DONE) {
        which = READINESS_GATE_BLOCK_SAFETY_TRIP;
        text = "refused -- the safety processor has a TRIP latched. Clear it on the Safety page "
               "(a reboot latches S6a mainFault; that is expected -- clear it, then start).";
    } else if (readiness_crash_report_status(f->crash_have_record, f->crash_acknowledged) ==
               READY_NOT_DONE) {
        which = READINESS_GATE_BLOCK_CRASH_REPORT;
        text = "refused -- an UNACKNOWLEDGED CRASH REPORT is stored. Review it on the Diagnostics "
               "page and acknowledge it before firing.";
    } else if (readiness_estop_verification_status(f->estop_verified) == READY_NOT_DONE) {
        which = READINESS_GATE_BLOCK_ESTOP_VERIFIED;
        text = "refused -- the E-STOP INTERLOCK has not been verified on this board. Run the bench "
               "procedure, then confirm it on the Readiness page.";
    } else if (readiness_ceiling_match_status(f->safety_link_up, f->ceiling_diverged) == READY_NOT_DONE) {
        which = READINESS_GATE_BLOCK_CEILING_MISMATCH;
        text = "refused -- the safety processor's ceiling does not match the ESP's zone config. Check "
               "the Safety page (a raise may need the safety processor to be de-energized first).";
    }

    if (which != READINESS_GATE_OK && msg != NULL && cap > 0 && text != NULL) {
        snprintf(msg, cap, "%s", text);
    }
    return which;
}

/* Convenience wrapper for start paths: collects the live facts and evaluates
 * them. Returns true when the start must be REFUSED. `out_which` is
 * optional. */
static inline bool readiness_gate_refuses_start(char *msg, size_t cap, readiness_gate_block_t *out_which)
{
    readiness_gate_facts_t facts;
    readiness_gate_collect(&facts);
    readiness_gate_block_t which = readiness_gate_evaluate(&facts, msg, cap);
    if (out_which != NULL) {
        *out_which = which;
    }
    return which != READINESS_GATE_OK;
}

#ifdef __cplusplus
}
#endif

#endif // READINESS_GATE_H
