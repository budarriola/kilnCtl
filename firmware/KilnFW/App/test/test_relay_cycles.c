// Host tests for App/drivers/persist/relay_cycles.c -- specifically persist_locked()'s
// PSRAM-stack guard (DRAM_PSRAM_PLAN.md section 7: relay_cycles_maybe_persist()/
// relay_cycles_flush() are called directly from profile_executor's tick and
// stop paths, the same task that plan names as its highest-care relocation
// candidate -- this module had no guard until this test's companion change
// added one, matching kiln_cfg_store.c's/safety_cfg_store.c's/run_state.c's
// caller_stack_is_external() pattern).
//
// relay_cycles.c is #included directly (same convention as
// test_crash_report.c's #include of crash_report.c) so this file can reach
// persist_locked() directly and exercise it via fake_kv.h's RAM-backed
// hal_kv fake, plus fake_kv_set_write_safe_here() to simulate a
// PSRAM-stacked caller (relay_cycles.c's caller_stack_is_external() is now
// !hal_kv_write_safe_here() -- see HW_ABSTRACTION_PLAN.md Phase 3 item 3,
// the nvs.h -> hal_kv.h migration).
// Own executable (build_host_tests.ps1's own build+run step, /std:c11):
// relay_cycles.c is compiled alongside run_state.c's identical guard test in
// the same executable (both are small, single-purpose files with no
// _Static_assert-related conflict between them) -- see test_run_state.c's
// header comment for why /std:c11 is required. g_test_failures/g_test_count
// are shared with test_run_state.c's translation unit via extern (test_common.h),
// defined once in test_run_state.c; main() lives there too.
#include <string.h>

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

#include "../drivers/persist/relay_cycles.c"

static void reset_all(void)
{
    fake_kv_reset_all(); // every test in this file needs a real round trip
    fake_kv_set_write_safe_here(true); // leave shared fake state as every other test expects
    hal_kv_init_partition(KILN_NVS_PARTITION); // relay_cycles_init() normally does this once at boot;
                                                // these tests call persist_locked()/hal_kv_open() directly
    memset(&s_rc, 0, sizeof(s_rc));
    s_rc.counts[0] = 42;
    s_rc.counts[1] = 7;
    s_rc.dirty = true;
}

static void test_persist_locked_refuses_when_calling_stack_is_external_ram(void)
{
    TEST_SECTION("relay_cycles persist_locked -- refuses (does not crash) when called with a "
                 "PSRAM stack underneath it (DRAM_PSRAM_PLAN.md section 7 safety net)");
    reset_all();

    fake_kv_set_write_safe_here(false); // simulate being called from a PSRAM-stacked task

    hal_status_t err = persist_locked();

    TEST_CHECK(err == HAL_NOT_READY,
               "the wrong-task guard refuses with a diagnosable error, not a crash -- exactly "
               "the class of bug (an NVS write reached from a PSRAM-stack task) this net exists "
               "to catch before a future relocation of profile_executor (DRAM_PSRAM_PLAN.md "
               "section 7) makes it reachable for real");
    TEST_CHECK(s_rc.dirty == true, "a refused write must not clear the dirty flag -- the counts "
                                   "are still unpersisted and must be retried later");

    // The refused write never created the namespace at all, so a READ_ONLY
    // open of it fails NOT_FOUND -- matching real NVS's nvs_open_from_
    // partition(..., NVS_READONLY, ...) behavior on a namespace that has
    // never been written (see hal_kv.h/fake_kv.c; this is a stricter, more
    // accurate model than the old stubs/nvs.h fake's always-succeeds open).
    hal_kv_handle_t h;
    hal_status_t open_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    TEST_CHECK(open_err == HAL_NOT_FOUND,
               "the refused write left no namespace behind at all -- persist_locked() returned "
               "before calling hal_kv_open(HAL_KV_MODE_READ_WRITE, ...)/hal_kv_set_blob() at all");

    fake_kv_set_write_safe_here(true); // leave shared fake state as every other test expects
}

static void test_persist_locked_proceeds_normally_on_an_internal_ram_stack(void)
{
    TEST_SECTION("relay_cycles persist_locked -- proceeds normally when the calling task's "
                 "stack is internal RAM");
    reset_all();

    // fake_kv_set_write_safe_here(true) is the fake's default state (also reset_all()'s).
    hal_status_t err = persist_locked();

    TEST_CHECK(err == HAL_OK, "the guard does not fire on an internal-RAM stack -- the write "
                              "proceeds and lands in the fake store");
    TEST_CHECK(s_rc.dirty == false, "a successful write clears the dirty flag");

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
               "stub NVS opens for read-back");
    relay_cycles_blob_t readback;
    size_t len = sizeof(readback);
    hal_status_t get_err = hal_kv_get_blob(&h, NVS_KEY_CYCLES, &readback, &len);
    hal_kv_close(&h);
    TEST_CHECK(get_err == HAL_OK && len == sizeof(readback), "the blob round-trips through the fake store");
    TEST_CHECK(readback.counts[0] == 42 && readback.counts[1] == 7,
               "the persisted counts are the ones that were passed in, unmodified");
}

void run_test_relay_cycles(void)
{
    test_persist_locked_refuses_when_calling_stack_is_external_ram();
    test_persist_locked_proceeds_normally_on_an_internal_ram_stack();

    fake_kv_reset_all(); // leave shared fake state as every other test file in this binary expects
}
