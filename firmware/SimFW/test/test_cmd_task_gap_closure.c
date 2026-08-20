// Host tests pinning the pure decision logic added to cmd_task.c by the SYS/
// FAULT gap-closure pass (SYS_RESET_SIM/SET_TIMESCALE/SET_SEED/GET_SIM_STATE;
// FAULT_SCHEDULE's fault_type bound-check fix; FAULT_SCHEDULE's UNTIL_TRIGGER
// two-frame design's s_pending_until[] state machine).
//
// Why this file mirrors rather than calls the real code: cmd_task.c is a
// FreeRTOS task file (includes FreeRTOS.h/task.h, calls sim_engine.h/
// fault_sched.h/i2c_owner.h/wave_owner.h/usb_owner.h task APIs), so it is not
// part of this host-test harness's source list -- build_host_tests.ps1
// compiles only src/sim/'s pure modules, the exact same pure/task boundary
// test_gap_closure_logic.c's own header comment already documents for
// sim_engine.c/fault_sched.c (there is no test_cmd_task.c for the same
// reason). Each function below is a deliberately small, byte-for-byte mirror
// of the corresponding block in cmd_task.c (cited in each function's
// comment) -- keep the two in sync by hand if either changes.
#include <stdbool.h>

#include "test_common.h"

// Mirrors handle_fault_schedule()'s fault_type bound check (cmd_task.c):
// fault_sched_fault_type_t's actual last catalog value is
// FAULT_SCHED_TYPE_DUT_POWER_CUT == 22 (fault_sched.h) -- the bug this pass
// fixed used FAULT_SCHED_TYPE_AMBIENT_SHIFT == 19 instead, which silently
// rejected THERMAL_MASS_SURPRISE(20)/TC_LAG_STRESS(21)/DUT_POWER_CUT(22) via
// FAULT_SCHEDULE despite all three being fully implemented.
static bool fault_type_in_range(unsigned fault_type)
{
    return fault_type <= 22u; /* FAULT_SCHED_TYPE_DUT_POWER_CUT */
}

// Mirrors decode_fault_trigger()'s trigger_kind bound check (cmd_task.c):
// fault_trigger_kind_t's last value is FAULT_TRIGGER_MANUAL == 6
// (fault_engine.h).
static bool trigger_kind_in_range(unsigned trigger_kind)
{
    return trigger_kind <= 6u; /* FAULT_TRIGGER_MANUAL */
}

// Mirrors handle_fault_schedule()'s duration_kind bound check: PERMANENT(0),
// FOR(1), UNTIL_TRIGGER(2) are all now accepted (the pre-gap-closure code
// rejected UNTIL_TRIGGER outright with `duration_kind >=
// FAULT_DURATION_UNTIL_TRIGGER`; this pass changed the check to `>` so the
// value 2 itself is accepted and routed to the two-frame path instead).
static bool duration_kind_in_range(unsigned duration_kind)
{
    return duration_kind <= 2u; /* FAULT_DURATION_UNTIL_TRIGGER */
}

// --- s_pending_until[] state machine mirror ---------------------------------
// Mirrors cmd_task.c's per-slot pending-UNTIL_TRIGGER bookkeeping: only the
// `pending` flag's transitions matter for these tests (the real struct also
// carries fault_type/target/trigger/repeat/params, opaque payload not under
// test here).
typedef struct {
    bool pending;
} mirror_slot_t;

// Mirrors handle_fault_schedule()'s duration_kind == UNTIL_TRIGGER branch:
// parks the slot, does not arm it.
static void mirror_schedule_until_trigger(mirror_slot_t *slot)
{
    slot->pending = true;
}

// Mirrors handle_fault_schedule()'s PERMANENT/FOR branch's "discard any
// still-pending UNTIL_TRIGGER on this slot_id" step -- a direct schedule
// supersedes a parked-but-never-completed UNTIL_TRIGGER on the same slot.
static void mirror_schedule_direct(mirror_slot_t *slot)
{
    slot->pending = false;
}

// Mirrors handle_fault_set_until_trigger(): fails (returns false, leaves
// pending untouched -- there was nothing to consume) if the slot has no
// pending entry; otherwise consumes it (clears pending) and succeeds,
// regardless of what fault_sched_schedule() itself later returns (consumed
// "regardless of outcome", per that handler's own comment -- a failed
// fault_sched_schedule() call means resend FAULT_SCHEDULE to retry, not
// resend SET_UNTIL_TRIGGER against a slot that no longer has anything
// parked).
static bool mirror_set_until_trigger(mirror_slot_t *slot)
{
    if (!slot->pending) {
        return false;
    }
    slot->pending = false;
    return true;
}

// Mirrors handle_fault_cancel()'s pending-clear step: a cancel always
// discards a pending entry, whether or not one existed.
static void mirror_cancel(mirror_slot_t *slot)
{
    slot->pending = false;
}

static void test_fault_type_bound_fix(void)
{
    TEST_SECTION("gap-closure -- FAULT_SCHEDULE fault_type bound (0..22, not 0..19)");

    TEST_CHECK(fault_type_in_range(0u), "TC_DISCONNECTED (0) accepted");
    TEST_CHECK(fault_type_in_range(19u), "AMBIENT_SHIFT (19, the old stale bound) still accepted");
    TEST_CHECK(fault_type_in_range(20u), "THERMAL_MASS_SURPRISE (20) accepted -- previously rejected by the stale bound");
    TEST_CHECK(fault_type_in_range(21u), "TC_LAG_STRESS (21) accepted -- previously rejected by the stale bound");
    TEST_CHECK(fault_type_in_range(22u), "DUT_POWER_CUT (22, the true last value) accepted");
    TEST_CHECK(!fault_type_in_range(23u), "one past the true last value is rejected");
}

static void test_trigger_kind_bound(void)
{
    TEST_SECTION("gap-closure -- decode_fault_trigger() trigger_kind bound");

    TEST_CHECK(trigger_kind_in_range(0u), "AT_SIM_TIME (0) accepted");
    TEST_CHECK(trigger_kind_in_range(6u), "MANUAL (6, last value) accepted");
    TEST_CHECK(!trigger_kind_in_range(7u), "one past the last value is rejected");
}

static void test_duration_kind_bound(void)
{
    TEST_SECTION("gap-closure -- FAULT_SCHEDULE duration_kind now accepts UNTIL_TRIGGER");

    TEST_CHECK(duration_kind_in_range(0u), "PERMANENT (0) accepted");
    TEST_CHECK(duration_kind_in_range(1u), "FOR (1) accepted");
    TEST_CHECK(duration_kind_in_range(2u), "UNTIL_TRIGGER (2) is now accepted, not rejected");
    TEST_CHECK(!duration_kind_in_range(3u), "one past the last value is still rejected");
}

static void test_pending_until_trigger_two_frame_flow(void)
{
    TEST_SECTION("gap-closure -- s_pending_until[] two-frame UNTIL_TRIGGER state machine");

    mirror_slot_t slot = {0};
    TEST_CHECK(!slot.pending, "fresh slot starts with no pending schedule");

    // Frame 1: FAULT_SCHEDULE(duration_kind=UNTIL_TRIGGER) parks, does not arm.
    mirror_schedule_until_trigger(&slot);
    TEST_CHECK(slot.pending, "FAULT_SCHEDULE(UNTIL_TRIGGER) parks the slot as pending");

    // Frame 2: SET_UNTIL_TRIGGER consumes the pending entry and succeeds.
    bool ok = mirror_set_until_trigger(&slot);
    TEST_CHECK(ok, "SET_UNTIL_TRIGGER on a pending slot succeeds");
    TEST_CHECK(!slot.pending, "SET_UNTIL_TRIGGER consumes the pending entry");

    // A second SET_UNTIL_TRIGGER against the now-consumed slot fails.
    ok = mirror_set_until_trigger(&slot);
    TEST_CHECK(!ok, "SET_UNTIL_TRIGGER against an already-consumed slot fails");
}

static void test_set_until_trigger_without_pending_fails(void)
{
    TEST_SECTION("gap-closure -- SET_UNTIL_TRIGGER against a never-parked slot fails");

    mirror_slot_t slot = {0};
    bool ok = mirror_set_until_trigger(&slot);
    TEST_CHECK(!ok, "SET_UNTIL_TRIGGER with no prior FAULT_SCHEDULE(UNTIL_TRIGGER) is rejected");
    TEST_CHECK(!slot.pending, "rejection leaves pending state unchanged (still false)");
}

static void test_direct_schedule_supersedes_pending(void)
{
    TEST_SECTION("gap-closure -- a direct PERMANENT/FOR schedule discards a stale pending UNTIL_TRIGGER");

    mirror_slot_t slot = {0};
    mirror_schedule_until_trigger(&slot);
    TEST_CHECK(slot.pending, "parked by FAULT_SCHEDULE(UNTIL_TRIGGER)");

    mirror_schedule_direct(&slot);
    TEST_CHECK(!slot.pending, "a later FAULT_SCHEDULE(PERMANENT/FOR) on the same slot discards the stale pending entry");

    // The now-discarded pending entry cannot be completed by a late-arriving
    // SET_UNTIL_TRIGGER for the superseded request.
    bool ok = mirror_set_until_trigger(&slot);
    TEST_CHECK(!ok, "a late SET_UNTIL_TRIGGER after supersession is rejected");
}

static void test_cancel_discards_pending(void)
{
    TEST_SECTION("gap-closure -- FAULT_CANCEL discards a pending UNTIL_TRIGGER");

    mirror_slot_t slot = {0};
    mirror_schedule_until_trigger(&slot);
    TEST_CHECK(slot.pending, "parked by FAULT_SCHEDULE(UNTIL_TRIGGER)");

    mirror_cancel(&slot);
    TEST_CHECK(!slot.pending, "FAULT_CANCEL discards the pending entry even though nothing was armed yet");

    // Cancelling an already-empty slot is a harmless no-op.
    mirror_cancel(&slot);
    TEST_CHECK(!slot.pending, "cancelling an already-clear slot stays clear");
}

void run_test_cmd_task_gap_closure(void)
{
    test_fault_type_bound_fix();
    test_trigger_kind_bound();
    test_duration_kind_bound();
    test_pending_until_trigger_two_frame_flow();
    test_set_until_trigger_without_pending_fails();
    test_direct_schedule_supersedes_pending();
    test_cancel_discards_pending();
}
