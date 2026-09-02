// Host tests for App/drivers/relay_cycles.c -- specifically persist_locked()'s
// PSRAM-stack guard (DRAM_PSRAM_PLAN.md section 7: relay_cycles_maybe_persist()/
// relay_cycles_flush() are called directly from profile_executor's tick and
// stop paths, the same task that plan names as its highest-care relocation
// candidate -- this module had no guard until this test's companion change
// added one, matching kiln_cfg_store.c's/safety_cfg_store.c's/run_state.c's
// caller_stack_is_external() pattern).
//
// relay_cycles.c is #included directly (same convention as
// test_crash_report.c's #include of crash_report.c) so this file can reach
// persist_locked() directly and exercise it via stubs/nvs.h's
// nvs_test_enable(true) opt-in stub store, plus stubs/esp_heap_caps.h's
// esp_ptr_external_ram_test_set() to simulate a PSRAM-stacked caller.
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

#include "../drivers/relay_cycles.c"

static void reset_all(void)
{
    nvs_test_enable(true); // every test in this file needs a real round trip
    nvs_test_clear();
    esp_ptr_external_ram_test_set(false); // leave shared stub state as every other test expects
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

    esp_ptr_external_ram_test_set(true); // simulate being called from a PSRAM-stacked task

    esp_err_t err = persist_locked();

    TEST_CHECK(err == ESP_ERR_INVALID_STATE,
               "the wrong-task guard refuses with a diagnosable error, not a crash -- exactly "
               "the class of bug (an NVS write reached from a PSRAM-stack task) this net exists "
               "to catch before a future relocation of profile_executor (DRAM_PSRAM_PLAN.md "
               "section 7) makes it reachable for real");
    TEST_CHECK(s_rc.dirty == true, "a refused write must not clear the dirty flag -- the counts "
                                   "are still unpersisted and must be retried later");

    nvs_handle_t h;
    TEST_CHECK(nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK,
               "stub NVS still opens for a read-back check");
    relay_cycles_blob_t readback;
    size_t len = sizeof(readback);
    esp_err_t get_err = nvs_get_blob(h, NVS_KEY_CYCLES, &readback, &len);
    nvs_close(h);
    TEST_CHECK(get_err == ESP_ERR_NVS_NOT_FOUND,
               "the refused write left no blob behind -- persist_locked() returned before "
               "calling nvs_open_from_partition()/nvs_set_blob() at all");

    esp_ptr_external_ram_test_set(false); // leave shared stub state as every other test expects
}

static void test_persist_locked_proceeds_normally_on_an_internal_ram_stack(void)
{
    TEST_SECTION("relay_cycles persist_locked -- proceeds normally when the calling task's "
                 "stack is internal RAM");
    reset_all();

    // esp_ptr_external_ram_test_set(false) is the stub's default state (also reset_all()'s).
    esp_err_t err = persist_locked();

    TEST_CHECK(err == ESP_OK, "the guard does not fire on an internal-RAM stack -- the write "
                              "proceeds and lands in the stub store");
    TEST_CHECK(s_rc.dirty == false, "a successful write clears the dirty flag");

    nvs_handle_t h;
    TEST_CHECK(nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK,
               "stub NVS opens for read-back");
    relay_cycles_blob_t readback;
    size_t len = sizeof(readback);
    esp_err_t get_err = nvs_get_blob(h, NVS_KEY_CYCLES, &readback, &len);
    nvs_close(h);
    TEST_CHECK(get_err == ESP_OK && len == sizeof(readback), "the blob round-trips through the stub store");
    TEST_CHECK(readback.counts[0] == 42 && readback.counts[1] == 7,
               "the persisted counts are the ones that were passed in, unmodified");
}

void run_test_relay_cycles(void)
{
    test_persist_locked_refuses_when_calling_stack_is_external_ram();
    test_persist_locked_proceeds_normally_on_an_internal_ram_stack();

    nvs_test_enable(false); // leave shared stub state as every other test file in this binary expects
    nvs_test_clear();
}
