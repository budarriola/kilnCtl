// test_link_task_announce_eval.c -- real host test for the REAL
// link_task_evaluate_announce_version() (tasks/link_task_announce_eval.c),
// which is the actual function link_task.c's
// link_task_handle_announce_version() calls to decide s_degraded_no_context
// from a decoded ANNOUNCE_VERSION frame.
//
// This closes a real coverage gap: link_frame_versions_compatible() (the
// pure protocol >= min_compatible formula) already had a full combination
// matrix test in test_link_frame.c, but link_task.c itself -- the file that
// actually wires that formula's verdict into s_degraded_no_context -- pulls
// in FreeRTOS/pico-sdk and is never linked into the host test executable,
// so that wiring had never been exercised at all (see git history: the
// TODO.md/UPDATE_PROTOCOL.md min_compatible checklist item cited a firmware
// gap that turned out to already be closed at the formula level; this was
// the one link left genuinely untested). link_task_announce_eval.c was
// split out for exactly this reason, same pattern as
// link_task_commit_reject.c and link_task_tc_type_gate.c.
//
// Pins BOTH directions, per LINK_PROTOCOL.md/UPDATE_PROTOCOL.md's
// requirement that the check be non-vacuous each way:
//   1. A peer too old for this build's own min_compatible -> refused
//      (degraded_no_context == true).
//   2. A compatible peer, including one strictly newer than self but still
//      within this build's floor -> accepted (degraded_no_context == false).
// A test that only ever exercised the accept path (or only the refuse path)
// would pass against an inverted or a hardcoded-true/false implementation;
// this file would not.
#include <stdio.h>

#include "kilnlink/kilnlink_announce.h"
#include "tasks/link_task_announce_eval.h"
#include "test_common.h"

static kilnlink_announce_t announce_with(uint16_t protocol_version, uint16_t min_compatible)
{
    kilnlink_announce_t msg;
    msg.protocol_version = protocol_version;
    msg.min_compatible = min_compatible;
    msg.dirty = 0;
    msg.commit_len = 0;
    msg.datetime_len = 0;
    msg.commit[0] = 0;
    msg.datetime[0] = 0;
    msg.boot_id = 0;
    return msg;
}

void run_test_link_task_announce_eval(void)
{
    TEST_SECTION("link_task_evaluate_announce_version -- ANNOUNCE_VERSION -> "
                 "degraded_no_context wiring (2026-09-20, real min_compatible "
                 "cross-check coverage)");

    // --- Direction 1: peer too old is REFUSED --------------------------
    // Self build: protocol 11, requires peer.protocol >= 9 (min_compatible).
    // Peer announces protocol 8 -- below self's floor -> NOT compatible ->
    // degraded_no_context must be true, and the real peer protocol version
    // (8) must still be cached for the GUI/status frame regardless.
    {
        kilnlink_announce_t msg = announce_with(/*protocol_version=*/8, /*min_compatible=*/1);
        link_task_announce_eval_t eval =
            link_task_evaluate_announce_version(&msg, /*self_protocol_version=*/11,
                                                 /*self_min_compatible=*/9);

        TEST_CHECK(eval.degraded_no_context == true,
                   "peer protocol (8) below self min_compatible (9) -> degraded_no_context "
                   "must be TRUE (refused)");
        TEST_CHECK(eval.peer_protocol_version == 8,
                   "peer_protocol_version is cached even for a refused/incompatible peer");
    }

    // --- Direction 2: self too old for what the peer requires is REFUSED
    // Self build: protocol 9. Peer announces protocol 12 but requires
    // min_compatible 11 -- self.protocol (9) < peer.min_compatible (11) ->
    // NOT compatible -> degraded_no_context must be true.
    {
        kilnlink_announce_t msg = announce_with(/*protocol_version=*/12, /*min_compatible=*/11);
        link_task_announce_eval_t eval =
            link_task_evaluate_announce_version(&msg, /*self_protocol_version=*/9,
                                                 /*self_min_compatible=*/1);

        TEST_CHECK(eval.degraded_no_context == true,
                   "self protocol (9) below peer's own min_compatible (11) -> "
                   "degraded_no_context must be TRUE (refused)");
        TEST_CHECK(eval.peer_protocol_version == 12,
                   "peer_protocol_version (12) cached even though self can't speak that floor");
    }

    // --- Direction 3: a genuinely compatible peer is ACCEPTED -----------
    // Self build: protocol 11, min_compatible 9 (today's real PC-to-ESP
    // pairing per LINK_PROTOCOL.md/CLAUDE.md). Peer announces exactly the
    // same values -- fully compatible -> degraded_no_context must be false.
    // This is the direction a hardcoded/inverted `degraded_no_context = true`
    // implementation would still pass on its own -- it is only paired with
    // the refusal cases above that the check becomes non-vacuous.
    {
        kilnlink_announce_t msg = announce_with(/*protocol_version=*/11, /*min_compatible=*/9);
        link_task_announce_eval_t eval =
            link_task_evaluate_announce_version(&msg, /*self_protocol_version=*/11,
                                                 /*self_min_compatible=*/9);

        TEST_CHECK(eval.degraded_no_context == false,
                   "identical protocol/min_compatible on both sides -> degraded_no_context "
                   "must be FALSE (accepted)");
        TEST_CHECK(eval.peer_protocol_version == 11, "peer_protocol_version cached (11)");
    }

    // --- Direction 4: a newer, still-backward-compatible peer is ACCEPTED
    // Peer is ahead (protocol 12) but its own floor (min_compatible 9) still
    // covers self's protocol (11), and peer.protocol (12) still satisfies
    // self's floor (9) -- compatible -> degraded_no_context must be false.
    {
        kilnlink_announce_t msg = announce_with(/*protocol_version=*/12, /*min_compatible=*/9);
        link_task_announce_eval_t eval =
            link_task_evaluate_announce_version(&msg, /*self_protocol_version=*/11,
                                                 /*self_min_compatible=*/9);

        TEST_CHECK(eval.degraded_no_context == false,
                   "peer newer (12) than self (11) but both floors satisfied -> "
                   "degraded_no_context must be FALSE (accepted)");
        TEST_CHECK(eval.peer_protocol_version == 12, "peer_protocol_version cached (12)");
    }
}
