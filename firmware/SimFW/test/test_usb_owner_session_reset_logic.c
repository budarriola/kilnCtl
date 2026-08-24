// Host tests for the SIMFW_CMD_SYS_SESSION_RESET dedup-bypass sequencing in
// usb_owner.c's usb_owner_handle_raw_frame() (bug-fix pass: "PC reconnect
// misclassified as duplicate traffic" -- see firmware/SimFW/src/tasks/
// cmd_ids.h's SIMFW_CMD_SYS_SESSION_RESET doc comment and
// firmware/SimFW/docs/PROTOCOL.md sec 4 for the full incident).
//
// Why this file mirrors ONE trivial predicate rather than linking
// usb_owner.c itself: usb_owner.c includes tusb.h and FreeRTOS headers
// (single-owner doctrine; it is a FreeRTOS task file), so it is not part of
// this host-test harness's source list -- the same pure/task boundary
// test_usb_dual_cdc_logic.c's own header comment documents for the same
// reason. UNLIKE that file, though, the actual reliability decision under
// test here -- "does a colliding msg_index get classified DELIVER or
// DUPLICATE_REACK" -- lives entirely in firmware/CommonFW's `benchproto`
// library, which has zero FreeRTOS/TinyUSB dependencies and IS linked into
// this binary for real (see build_host_tests.ps1's `$commonfwDir` sources).
// So only usb_owner_handle_raw_frame()'s own dispatch predicate
// (usb_owner_dispatch_is_session_reset() below, a byte-for-byte copy of that
// function's `if` condition) is a hand-maintained mirror; every dedup/reset
// decision this file asserts on runs through the REAL
// benchproto_link_on_frame()/benchproto_link_reset_device(), in the exact
// call order usb_owner.c uses them in.
#include <stdbool.h>
#include <stdint.h>

#include "test_common.h"

#include "benchproto/benchproto_link.h"
#include "cmd_ids.h"

enum { DEV_HOST = SIMFW_DEVICE_HOST, DEV_TARGET = SIMFW_DEVICE_TARGET };

static benchproto_frame_t make_frame(benchproto_msg_type_t type, uint16_t msg_index, uint8_t src_device,
                                      uint8_t src_task, uint8_t dst_device, uint8_t dst_task,
                                      const uint8_t *payload, uint8_t length)
{
    benchproto_frame_t f;
    f.msg_type = type;
    f.msg_index = msg_index;
    f.src_device = src_device;
    f.src_task = src_task;
    f.dst_device = dst_device;
    f.dst_task = dst_task;
    f.length = length;
    f.payload = payload;
    return f;
}

// Byte-for-byte mirror of usb_owner_handle_raw_frame()'s own `if` condition
// (usb_owner.c) that gates the SESSION_RESET dedup bypass -- keep this in
// sync BY HAND if that condition ever changes. Everything downstream of this
// predicate in the tests below calls the REAL benchproto_link functions, not
// a mirror.
static bool usb_owner_dispatch_is_session_reset(const benchproto_frame_t *frame)
{
    return frame->msg_type == BENCHPROTO_MSG_DATA && frame->dst_task == SIMFW_TASK_ID_SYS && frame->length >= 1 &&
           frame->payload[0] == SIMFW_CMD_SYS_SESSION_RESET;
}

// Reproduces usb_owner_handle_raw_frame()'s exact dispatch order for one
// already-decoded frame: if (and only if) the SESSION_RESET predicate above
// matches, benchproto_link_reset_device() runs UNCONDITIONALLY and BEFORE
// benchproto_link_on_frame() ever classifies the frame -- then the frame is
// classified normally regardless. Returns what usb_owner would have done
// with the classification (mirroring only the switch's DELIVER/
// DUPLICATE_REACK cases relevant to these tests, not the whole thing).
static benchproto_link_action_t usb_owner_dispatch(benchproto_link_t *link, const benchproto_frame_t *frame)
{
    if (usb_owner_dispatch_is_session_reset(frame)) {
        benchproto_link_reset_device(link, frame->src_device);
        // (usb_owner_reset_ack_cache() has no host-testable equivalent here
        // -- it is a plain memset() of usb_owner.c's own static array, no
        // decision logic to pin. What CAN be pinned, and is, is that this
        // reset happens before on_frame() ever runs -- see below.)
    }
    return benchproto_link_on_frame(link, NULL, frame);
}

// --- The hazard itself: does SESSION_RESET survive colliding with its OWN
// stale ring entry? ----------------------------------------------------------

static void test_session_reset_frame_is_not_suppressed_by_a_collision_with_itself(void)
{
    TEST_SECTION("SESSION_RESET dedup bypass -- the hazard: a colliding msg_index "
                 "must not swallow SESSION_RESET itself");

    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    TEST_CHECK(benchproto_link_register_task(&link, SIMFW_TASK_ID_SYS) == BENCHPROTO_LINK_OK,
               "setup: register SYS task");

    uint8_t session_reset_payload[1] = {SIMFW_CMD_SYS_SESSION_RESET};
    benchproto_frame_t reset_frame = make_frame(BENCHPROTO_MSG_DATA, 0, DEV_HOST, 0, DEV_TARGET,
                                                 SIMFW_TASK_ID_SYS, session_reset_payload, 1);

    // Prime the ring with the EXACT SAME (src_device, src_task, msg_index)
    // this SESSION_RESET frame itself carries -- the worst case: not just
    // "some other stale entry from last session", but a literal identity
    // match with the very frame under test, exactly what a PC client
    // restarting its msg_index counter at 0 would produce if SESSION_RESET
    // is always msg_index 0 (kilnsim.link's connect() sends it first).
    TEST_CHECK(benchproto_link_mark_delivered(&link, SIMFW_TASK_ID_SYS, DEV_HOST, 0, 0) == BENCHPROTO_LINK_OK,
               "setup: prime the ring with SESSION_RESET's own (src_device, src_task, msg_index)");

    // Without the bypass, this is exactly what would happen (proves the
    // hazard is real, not hypothetical):
    TEST_CHECK(benchproto_link_on_frame(&link, NULL, &reset_frame) == BENCHPROTO_LINK_ACTION_DUPLICATE_REACK,
               "plain on_frame() alone (no bypass) WOULD suppress this frame -- confirms the hazard is real");

    // The real dispatch order (reset-then-classify) must not suppress it:
    TEST_CHECK(usb_owner_dispatch(&link, &reset_frame) == BENCHPROTO_LINK_ACTION_DELIVER,
               "usb_owner's actual dispatch order -- reset BEFORE classify -- delivers SESSION_RESET "
               "even though it collided with its own stale ring entry");
}

static void test_session_reset_is_idempotent_across_repeated_identical_sends(void)
{
    TEST_SECTION("SESSION_RESET dedup bypass -- idempotent: sending it twice in a row never suppresses the second one");

    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    TEST_CHECK(benchproto_link_register_task(&link, SIMFW_TASK_ID_SYS) == BENCHPROTO_LINK_OK,
               "setup: register SYS task");

    uint8_t payload[1] = {SIMFW_CMD_SYS_SESSION_RESET};
    benchproto_frame_t reset_frame =
        make_frame(BENCHPROTO_MSG_DATA, 0, DEV_HOST, 0, DEV_TARGET, SIMFW_TASK_ID_SYS, payload, 1);

    TEST_CHECK(usb_owner_dispatch(&link, &reset_frame) == BENCHPROTO_LINK_ACTION_DELIVER,
               "first SESSION_RESET (nothing in the ring yet) -> DELIVER");
    // Note: usb_owner.c never calls benchproto_link_mark_delivered() for
    // this path only if cmd_task's inbox enqueue fails -- on the normal
    // success path it DOES get marked delivered, same as any other DATA
    // frame (usb_owner_handle_deliver() runs unconditionally once
    // classified DELIVER). Simulate that here.
    TEST_CHECK(benchproto_link_mark_delivered(&link, SIMFW_TASK_ID_SYS, DEV_HOST, 0, 0) == BENCHPROTO_LINK_OK,
               "first SESSION_RESET's delivery gets marked, same as any other DATA frame");

    // A retried/second identical SESSION_RESET (same msg_index -- e.g. the
    // client's ACK never arrived) must STILL be delivered, not
    // double-suppressed by the mark_delivered() call above.
    TEST_CHECK(usb_owner_dispatch(&link, &reset_frame) == BENCHPROTO_LINK_ACTION_DELIVER,
               "a second, identical SESSION_RESET -> still DELIVER, not DUPLICATE_REACK "
               "(the reset it just performed wiped the entry mark_delivered() had just written)");
}

// --- The fix, exercised through the same dispatch order: a full reconnect
// scenario spanning multiple command groups. ---------------------------------

static void test_reconnect_scenario_first_three_commands_no_longer_suppressed(void)
{
    TEST_SECTION("SESSION_RESET dedup bypass -- reconnect scenario: SESSION_RESET, then PING, then a "
                 "MODEL command, at the SAME msg_indexes a previous session already used");

    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    TEST_CHECK(benchproto_link_register_task(&link, SIMFW_TASK_ID_SYS) == BENCHPROTO_LINK_OK, "setup: register SYS");
    TEST_CHECK(benchproto_link_register_task(&link, SIMFW_TASK_ID_MODEL) == BENCHPROTO_LINK_OK,
               "setup: register MODEL");

    // Simulate a previous session ending with SYS having seen msg_index 0
    // (PING) and MODEL having seen msg_index 1 -- both marked delivered, as
    // real usage would leave them.
    TEST_CHECK(benchproto_link_mark_delivered(&link, SIMFW_TASK_ID_SYS, DEV_HOST, 0, 0) == BENCHPROTO_LINK_OK,
               "setup: previous session's SYS traffic at msg_index 0");
    TEST_CHECK(benchproto_link_mark_delivered(&link, SIMFW_TASK_ID_MODEL, DEV_HOST, 0, 1) == BENCHPROTO_LINK_OK,
               "setup: previous session's MODEL traffic at msg_index 1");

    // New session (PC reconnected, its msg_index counter restarted at 0):
    // frame 0 = SESSION_RESET to SYS at msg_index 0 (colliding),
    // frame 1 = PING to SYS at msg_index 1,
    // frame 2 = a MODEL command at msg_index 2.
    // None of these three literal (task, msg_index) pairs is fresh to the
    // ring on its own (msg_index 0 -> SYS collides directly; msg_index 1 was
    // MODEL's before, now going to SYS -- different task slot, so it
    // wouldn't have collided anyway, but included for completeness), so
    // frame 2 (MODEL, msg_index 2) is the one that DOES genuinely need the
    // reset: pre-reset, MODEL's ring still holds msg_index 1, not 2, so it
    // would already be DELIVER without a reset. To actually prove the
    // "spans every task" property, prime MODEL's ring with msg_index 2
    // too, matching what the new session is about to send.
    TEST_CHECK(benchproto_link_mark_delivered(&link, SIMFW_TASK_ID_MODEL, DEV_HOST, 0, 2) == BENCHPROTO_LINK_OK,
               "setup: previous session ALSO left MODEL's ring holding msg_index 2 "
               "(a second, later message in that same old session)");

    uint8_t session_reset_payload[1] = {SIMFW_CMD_SYS_SESSION_RESET};
    benchproto_frame_t f_session_reset = make_frame(BENCHPROTO_MSG_DATA, 0, DEV_HOST, 0, DEV_TARGET,
                                                     SIMFW_TASK_ID_SYS, session_reset_payload, 1);
    uint8_t ping_payload[1] = {0x01}; // SIMFW_CMD_SYS_PING
    benchproto_frame_t f_ping =
        make_frame(BENCHPROTO_MSG_DATA, 1, DEV_HOST, 0, DEV_TARGET, SIMFW_TASK_ID_SYS, ping_payload, 1);
    uint8_t model_payload[1] = {0x03}; // SIMFW_CMD_MODEL_SET_AMBIENT, args omitted -- classification doesn't decode args
    benchproto_frame_t f_model =
        make_frame(BENCHPROTO_MSG_DATA, 2, DEV_HOST, 0, DEV_TARGET, SIMFW_TASK_ID_MODEL, model_payload, 1);

    TEST_CHECK(usb_owner_dispatch(&link, &f_session_reset) == BENCHPROTO_LINK_ACTION_DELIVER,
               "reconnect command 1/3 (SESSION_RESET, colliding msg_index 0 on SYS) -> DELIVER");
    (void)benchproto_link_mark_delivered(&link, SIMFW_TASK_ID_SYS, DEV_HOST, 0, 0);

    TEST_CHECK(usb_owner_dispatch(&link, &f_ping) == BENCHPROTO_LINK_ACTION_DELIVER,
               "reconnect command 2/3 (PING, msg_index 1 on SYS) -> DELIVER, "
               "protected by the reset command 1/3 just performed");
    (void)benchproto_link_mark_delivered(&link, SIMFW_TASK_ID_SYS, DEV_HOST, 0, 1);

    TEST_CHECK(usb_owner_dispatch(&link, &f_model) == BENCHPROTO_LINK_ACTION_DELIVER,
               "reconnect command 3/3 (a MODEL command, colliding msg_index 2 on MODEL) -> DELIVER -- "
               "the fix this whole pass exists for: the SAME reset that protected SYS's own traffic "
               "ALSO protected a completely different command group");
}

// --- The non-abuse property: every OTHER command still goes through
// on_frame() completely unmodified. -----------------------------------------

static void test_ordinary_command_is_not_exempted_from_dedup(void)
{
    TEST_SECTION("SESSION_RESET dedup bypass -- an ordinary command (not SYS/SESSION_RESET) "
                 "still gets deduped normally, proving the bypass cannot be abused generically");

    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    TEST_CHECK(benchproto_link_register_task(&link, SIMFW_TASK_ID_MODEL) == BENCHPROTO_LINK_OK,
               "setup: register MODEL");

    uint8_t model_payload[1] = {0x01};
    benchproto_frame_t f_model =
        make_frame(BENCHPROTO_MSG_DATA, 5, DEV_HOST, 0, DEV_TARGET, SIMFW_TASK_ID_MODEL, model_payload, 1);

    TEST_CHECK(usb_owner_dispatch(&link, &f_model) == BENCHPROTO_LINK_ACTION_DELIVER,
               "first delivery of an ordinary MODEL command -> DELIVER");
    TEST_CHECK(benchproto_link_mark_delivered(&link, SIMFW_TASK_ID_MODEL, DEV_HOST, 0, 5) == BENCHPROTO_LINK_OK,
               "marked delivered, same as usb_owner_handle_deliver() would do");

    // A genuine retry of the SAME ordinary command must still be deduped --
    // the bypass above only ever fires for (SYS, SESSION_RESET); this frame
    // matches neither, so it must reach benchproto_link_on_frame()
    // unmodified and get the ordinary DUPLICATE_REACK answer.
    TEST_CHECK(usb_owner_dispatch(&link, &f_model) == BENCHPROTO_LINK_ACTION_DUPLICATE_REACK,
               "a retried ordinary command is still classified DUPLICATE_REACK -- "
               "the SESSION_RESET bypass did not leak into any other command's dedup protection");
}

static void test_sys_command_that_is_not_session_reset_is_not_exempted(void)
{
    TEST_SECTION("SESSION_RESET dedup bypass -- even WITHIN the SYS group, only cmd_id "
                 "SIMFW_CMD_SYS_SESSION_RESET is exempted, e.g. not PING");

    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    TEST_CHECK(benchproto_link_register_task(&link, SIMFW_TASK_ID_SYS) == BENCHPROTO_LINK_OK, "setup: register SYS");

    uint8_t ping_payload[1] = {0x01}; // SIMFW_CMD_SYS_PING, not SESSION_RESET
    benchproto_frame_t f_ping =
        make_frame(BENCHPROTO_MSG_DATA, 3, DEV_HOST, 0, DEV_TARGET, SIMFW_TASK_ID_SYS, ping_payload, 1);

    TEST_CHECK(usb_owner_dispatch(&link, &f_ping) == BENCHPROTO_LINK_ACTION_DELIVER, "first PING -> DELIVER");
    TEST_CHECK(benchproto_link_mark_delivered(&link, SIMFW_TASK_ID_SYS, DEV_HOST, 0, 3) == BENCHPROTO_LINK_OK,
               "PING marked delivered");

    TEST_CHECK(usb_owner_dispatch(&link, &f_ping) == BENCHPROTO_LINK_ACTION_DUPLICATE_REACK,
               "a retried PING (same SYS task, same msg_index) is still deduped normally -- "
               "the exemption is scoped to the SESSION_RESET cmd_id specifically, not the whole SYS group");
}

void run_test_usb_owner_session_reset_logic(void)
{
    test_session_reset_frame_is_not_suppressed_by_a_collision_with_itself();
    test_session_reset_is_idempotent_across_repeated_identical_sends();
    test_reconnect_scenario_first_three_commands_no_longer_suppressed();
    test_ordinary_command_is_not_exempted_from_dedup();
    test_sys_command_that_is_not_session_reset_is_not_exempted();
}
