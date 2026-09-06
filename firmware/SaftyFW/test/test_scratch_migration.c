// test_scratch_migration.c -- host tests for HAL Phase 3 item 1
// (docs/HW_ABSTRACTION_PLAN.md "hal_scratch -- pico watchdog-scratch
// registry"): boot_reason.c, clear_trip_diag.c and watchdog_overdue_diag.c
// now go through hal_scratch_write_u32/read_u32/clear/claim instead of
// poking watchdog_hw->scratch[] directly. This links the REAL production
// .c files (not a mirror/reimplementation -- see
// project_binding_a_python_mirror_to_c.md's standing lesson on why that
// distinction matters) against fake_scratch.c, the host hal_scratch.h
// backend.
//
// Slot-4 refusal and the (slot, tag) claim-uniqueness rule are already
// covered directly against fake_scratch.c/hal_scratch_pico.c by
// firmware/hwAbstraction/test/test_fake_scratch.c and
// test_host_fakes.ps1 -- this file's job is narrower: prove the three real
// SaftyFW callers actually reach the backend through the real accessor
// functions (not a comment claiming they do), and that clearing one
// module's slot never disturbs another module's, matching hal_scratch.h's
// per-slot independence.
#include "test_common.h"

#include "../src/boot_reason.h"
#include "../src/clear_trip_diag.h"
#include "../src/watchdog_overdue_diag.h"
#include "fake_scratch.h"

// Real slot numbers, duplicated here (not #include-able -- they are
// private #defines inside boot_reason.c/clear_trip_diag.c/
// watchdog_overdue_diag.c) so this test can inspect the backend
// independently of the accessor under test, the same way
// test_fake_scratch.c inspects hal_scratch's claim table independently of
// whichever module claimed a slot.
#define SCRATCH_TRIP_REASON 0u
#define SCRATCH_TRIP_MAGIC  1u
#define SCRATCH_OVERDUE     5u
#define SCRATCH_CLEAR_TRIP  7u

static void test_boot_reason_round_trips_through_hal_scratch(void)
{
    fake_scratch_reset_all();

    boot_reason_latch_trip(0x2Au);
    saftyfw_boot_reason_t reason = boot_reason_read(false, false);

    TEST_CHECK(reason.trip_reason_valid, "a latched trip must read back valid through hal_scratch");
    TEST_CHECK(reason.trip_reason == 0x2Au, "the exact trip reason must survive the hal_scratch round trip");

    boot_reason_clear_trip();
    saftyfw_boot_reason_t after_clear = boot_reason_read(false, false);
    TEST_CHECK(!after_clear.trip_reason_valid,
               "boot_reason_clear_trip() must clear the magic word through hal_scratch_clear(), "
               "so a later read reports invalid");
}

static void test_boot_reason_unlatched_reads_invalid(void)
{
    fake_scratch_reset_all();

    saftyfw_boot_reason_t reason = boot_reason_read(true, true);
    TEST_CHECK(!reason.trip_reason_valid,
               "power-on-zeroed scratch (no prior latch) must read magic_ok == false, "
               "not garbage interpreted as a real trip");
    TEST_CHECK(reason.watchdog_caused_reboot, "the caller-supplied SDK facts must pass through unchanged");
    TEST_CHECK(reason.watchdog_enable_caused_reboot, "the caller-supplied SDK facts must pass through unchanged");
}

static void test_clear_trip_diag_round_trips_through_hal_scratch(void)
{
    fake_scratch_reset_all();

    clear_trip_diag_mark(/* stage */ 3u, /* reason */ 7u, /* fault_bits */ 0x41u,
                          /* tc_valid */ true, /* spi_failed */ false, /* tc_c_is_nan */ true,
                          /* outcome */ 2u);
    clear_trip_diag_t out = clear_trip_diag_read();

    TEST_CHECK(out.magic_ok, "a freshly marked checkpoint must decode with magic_ok true after the hal_scratch round trip");
    TEST_CHECK(out.stage == 3u, "stage must survive the hal_scratch round trip");
    TEST_CHECK(out.reason == 7u, "reason must survive the hal_scratch round trip");
    TEST_CHECK(out.fault_bits == 0x41u, "fault_bits must survive the hal_scratch round trip");

    clear_trip_diag_clear();
    clear_trip_diag_t after_clear = clear_trip_diag_read();
    TEST_CHECK(!after_clear.magic_ok, "clear_trip_diag_clear() must actually zero slot 7 through hal_scratch_clear()");
}

static void test_watchdog_overdue_diag_round_trips_through_hal_scratch(void)
{
    fake_scratch_reset_all();

    watchdog_overdue_diag_mark(/* overdue_mask */ 0x05u, /* worst_task_id */ 2u, /* worst_overage_ms */ 350u);
    watchdog_overdue_diag_t out = watchdog_overdue_diag_read();

    TEST_CHECK(out.magic_ok, "a freshly marked overdue latch must decode with magic_ok true after the hal_scratch round trip");
    TEST_CHECK(out.overdue_mask == 0x05u, "overdue_mask must survive the hal_scratch round trip");
    TEST_CHECK(out.worst_task_id == 2u, "worst_task_id must survive the hal_scratch round trip");
    TEST_CHECK(out.worst_overage_ms == 350u, "worst_overage_ms must survive the hal_scratch round trip");

    watchdog_overdue_diag_clear();
    watchdog_overdue_diag_t after_clear = watchdog_overdue_diag_read();
    TEST_CHECK(!after_clear.magic_ok, "watchdog_overdue_diag_clear() must actually zero slot 5 through hal_scratch_clear()");
}

// Slot independence: boot_reason.c (slots 0/1), clear_trip_diag.c (slot 7)
// and watchdog_overdue_diag.c (slot 5) must not step on each other now that
// all three go through the same hal_scratch backend and its one shared
// s_slots[] array (fake_scratch.c) -- a regression here (e.g. an
// off-by-one on which slot constant a module passes) would show up as one
// module's write bleeding into another's read.
static void test_the_three_modules_do_not_collide_on_shared_backend(void)
{
    fake_scratch_reset_all();

    boot_reason_latch_trip(0x11u);
    clear_trip_diag_mark(1u, 1u, 0u, false, false, false, 0u);
    watchdog_overdue_diag_mark(0x03u, 1u, 10u);

    saftyfw_boot_reason_t reason = boot_reason_read(false, false);
    clear_trip_diag_t trip = clear_trip_diag_read();
    watchdog_overdue_diag_t overdue = watchdog_overdue_diag_read();

    TEST_CHECK(reason.trip_reason_valid && reason.trip_reason == 0x11u,
               "boot_reason's slot must be unaffected by the other two modules writing their own slots");
    TEST_CHECK(trip.magic_ok, "clear_trip_diag's slot must be unaffected by the other two modules writing their own slots");
    TEST_CHECK(overdue.magic_ok, "watchdog_overdue_diag's slot must be unaffected by the other two modules writing their own slots");

    clear_trip_diag_clear();
    saftyfw_boot_reason_t reason_after = boot_reason_read(false, false);
    watchdog_overdue_diag_t overdue_after = watchdog_overdue_diag_read();
    TEST_CHECK(reason_after.trip_reason_valid, "clearing clear_trip_diag's slot 7 must not clear boot_reason's slot 0/1");
    TEST_CHECK(overdue_after.magic_ok, "clearing clear_trip_diag's slot 7 must not clear watchdog_overdue_diag's slot 5");
}

void run_test_scratch_migration(void)
{
    test_boot_reason_round_trips_through_hal_scratch();
    test_boot_reason_unlatched_reads_invalid();
    test_clear_trip_diag_round_trips_through_hal_scratch();
    test_watchdog_overdue_diag_round_trips_through_hal_scratch();
    test_the_three_modules_do_not_collide_on_shared_backend();
}
