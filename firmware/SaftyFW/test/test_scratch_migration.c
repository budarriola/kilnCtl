// test_scratch_migration.c -- host tests for HAL Phase 3 item 1
// (docs/HW_ABSTRACTION.md "hal_scratch -- pico watchdog-scratch
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
#include "../src/watchdog_overdue_diag_codec.h"
#include "fake_scratch.h"
#include "hal_scratch.h"

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

// 2026-09-10, opus review finding A: reproduces the SMP overwrite this
// firmware shipped with. Before the fix, watchdog_overdue_diag_mark() wrote
// its own 0xD9 tag over scratch[5] unconditionally, on the (false on a
// dual-core chip) assumption that a fatal hook and watchdog_task's own
// overdue branch could never both run in the same boot. Latch a fatal tag
// first (as vApplicationMallocFailedHook() would, on the OTHER core), then
// call watchdog_overdue_diag_mark() (as watchdog_task, still running on ITS
// core, would once it notices the hung core's task missed its check-in) --
// the fatal tag must survive.
static void test_overdue_mark_does_not_overwrite_a_live_fatal_tag(void)
{
    fake_scratch_reset_all();

    // Simulate vApplicationMallocFailedHook()'s raw write (main.c never
    // calls into watchdog_overdue_diag.c's own encoder from a fatal hook --
    // see that file's header comment -- so this test pokes the same word
    // main.c would, via the shared macro both sides already depend on).
    (void)hal_scratch_write_u32(SCRATCH_OVERDUE, WATCHDOG_FATAL_MALLOC_WORD());

    watchdog_fatal_diag_t before = watchdog_fatal_diag_read();
    TEST_CHECK(before.magic_ok && before.kind == (uint8_t)WATCHDOG_FATAL_KIND_MALLOC_FAILED,
               "the simulated malloc-fail latch must decode correctly before the overdue mark is attempted");

    // watchdog_task, still alive on its own core, now notices the hung
    // core's task missed its deadline and calls mark() -- exactly the
    // sequence that used to erase the fatal tag.
    watchdog_overdue_diag_mark(/* overdue_mask */ 0x02u, /* worst_task_id */ 1u,
                                /* worst_overage_ms */ 900u);

    watchdog_fatal_diag_t after = watchdog_fatal_diag_read();
    TEST_CHECK(after.magic_ok && after.kind == (uint8_t)WATCHDOG_FATAL_KIND_MALLOC_FAILED,
               "watchdog_overdue_diag_mark() must refuse to overwrite an already-latched fatal tag "
               "from the other core -- this is the fix for opus review finding A");

    watchdog_overdue_diag_t overdue = watchdog_overdue_diag_read();
    TEST_CHECK(!overdue.magic_ok,
               "the overdue format must NOT have been written over the fatal tag -- "
               "decoding the register as the overdue format must still show nothing usable");
}

// A genuine overdue mark, with no fatal tag present, must still write
// normally -- the guard must not be so broad that it refuses every write.
static void test_overdue_mark_still_writes_when_nothing_fatal_is_latched(void)
{
    fake_scratch_reset_all();

    watchdog_overdue_diag_mark(0x04u, 2u, 120u);

    watchdog_overdue_diag_t out = watchdog_overdue_diag_read();
    TEST_CHECK(out.magic_ok && out.overdue_mask == 0x04u,
               "an overdue mark with no pre-existing fatal tag must still write normally");
}

// 2026-09-10, opus review finding A (transient-overdue half): a miss that
// recovers before the hardware watchdog fires must not leave a stale 0xD9
// latch for a later, unrelated reset to misreport.
static void test_recovered_overdue_is_cleared_not_left_stale(void)
{
    fake_scratch_reset_all();

    watchdog_overdue_diag_mark(0x01u, 0u, 50u);
    TEST_CHECK(watchdog_overdue_diag_read().magic_ok,
               "sanity: the transient miss must actually latch before recovery is simulated");

    // watchdog_task_fn()'s next iteration finds all_ok true (the task caught
    // up and fed on schedule) and calls the new recovered-notification hook.
    watchdog_overdue_diag_notify_recovered();

    TEST_CHECK(!watchdog_overdue_diag_read().magic_ok,
               "a recovered transient overdue must be cleared so a LATER, unrelated reset "
               "does not misreport check-in overdue for a boot in which nothing was overdue");
}

// The recovered-clear must never touch a genuine fatal latch, even though
// in practice a fatal hook's hung core means watchdog_task should never
// reach the all_ok branch again this boot -- defence in depth, per the
// callee's own comment.
static void test_recovered_notify_never_clears_a_fatal_tag(void)
{
    fake_scratch_reset_all();

    (void)hal_scratch_write_u32(SCRATCH_OVERDUE, WATCHDOG_FATAL_MALLOC_WORD());

    watchdog_overdue_diag_notify_recovered();

    watchdog_fatal_diag_t after = watchdog_fatal_diag_read();
    TEST_CHECK(after.magic_ok && after.kind == (uint8_t)WATCHDOG_FATAL_KIND_MALLOC_FAILED,
               "watchdog_overdue_diag_notify_recovered() must never clear a genuine fatal latch");
}

void run_test_scratch_migration(void)
{
    test_boot_reason_round_trips_through_hal_scratch();
    test_boot_reason_unlatched_reads_invalid();
    test_clear_trip_diag_round_trips_through_hal_scratch();
    test_watchdog_overdue_diag_round_trips_through_hal_scratch();
    test_the_three_modules_do_not_collide_on_shared_backend();
    test_overdue_mark_does_not_overwrite_a_live_fatal_tag();
    test_overdue_mark_still_writes_when_nothing_fatal_is_latched();
    test_recovered_overdue_is_cleared_not_left_stale();
    test_recovered_notify_never_clears_a_fatal_tag();
}
