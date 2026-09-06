// Host tests for App/drivers/control/run_state.c -- specifically persist_locked()'s
// PSRAM-stack guard (DRAM_PSRAM_PLAN.md section 7: relocating profile_executor
// to a PSRAM stack is the highest-care candidate in that plan precisely
// because it calls run_state_note()/run_state_note_progress() -> here ->
// persist_locked() on every tick/transition; this module had no guard until
// this test's companion change added one, matching kiln_cfg_store.c's/
// safety_cfg_store.c's caller_stack_is_external() pattern).
//
// run_state.c is #included directly (same convention as
// test_crash_report.c's #include of crash_report.c) so this file can reach
// persist_locked() directly and exercise it via fake_kv.h's RAM-backed
// hal_kv fake, plus fake_kv_set_write_safe_here() to simulate a
// PSRAM-stacked caller (run_state.c's caller_stack_is_external() is now
// !hal_kv_write_safe_here() -- see HW_ABSTRACTION_PLAN.md Phase 3 item 3,
// the nvs.h -> hal_kv.h migration).
// Own executable (build_host_tests.ps1's own build+run step, /std:c11):
// run_state.c uses _Static_assert, which MSVC's cl.exe only recognizes under
// /std:c11 -- the main executable's $sources are built without that flag
// (matching test_zones_http.c's/test_profiles_http.c's precedent for the
// same reason). g_test_failures/g_test_count are defined here rather than
// pulled from test_main.c since this is a separate translation unit/binary.
#include <string.h>

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/control/run_state.c"

static run_state_record_t make_sample_record(void)
{
    run_state_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.version = RUN_STATE_RECORD_VERSION;
    rec.phase = RUN_STATE_PHASE_RUNNING;
    rec.profile_id = 3;
    rec.zone_mask = 0x01;
    rec.segment_index = 1;
    rec.segment_count = 4;
    rec.target_c = 950.0f;
    rec.segment_elapsed_s = 120;
    rec.uptime_s = 3600;
    strcpy(rec.profile_name, "cone6-slow");
    return rec;
}

static void reset_all(void)
{
    fake_kv_reset_all(); // every test in this file needs a real round trip
    fake_kv_set_write_safe_here(true); // leave shared fake state as every other test expects
    hal_kv_init_partition(KILN_NVS_PARTITION); // run_state_init() normally does this once at boot;
                                                // these tests call persist_locked()/hal_kv_open() directly
}

static void test_persist_locked_refuses_when_calling_stack_is_external_ram(void)
{
    TEST_SECTION("run_state persist_locked -- refuses (does not crash) when called with a "
                 "PSRAM stack underneath it (DRAM_PSRAM_PLAN.md section 7 safety net)");
    reset_all();

    run_state_record_t rec = make_sample_record();
    fake_kv_set_write_safe_here(false); // simulate being called from a PSRAM-stacked task

    esp_err_t err = persist_locked(&rec);

    TEST_CHECK(err == ESP_ERR_INVALID_STATE,
               "the wrong-task guard refuses with a diagnosable error, not a crash -- exactly "
               "the class of bug (an NVS write reached from a PSRAM-stack task) this net exists "
               "to catch before a future relocation of profile_executor (DRAM_PSRAM_PLAN.md "
               "section 7) makes it reachable for real");

    // The refused write never created the namespace at all, so a READ_ONLY
    // open of it fails NOT_FOUND -- matching real NVS's nvs_open_from_
    // partition(..., NVS_READONLY, ...) behavior on a namespace that has
    // never been written (see hal_kv.h/fake_kv.c; this is a stricter, more
    // accurate model than the old stubs/nvs.h fake's always-succeeds open).
    fake_kv_set_write_safe_here(true); // reopen for the read-back check as an ordinary task would
    hal_kv_handle_t h;
    hal_status_t open_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    TEST_CHECK(open_err == HAL_NOT_FOUND,
               "the refused write left no namespace behind at all -- persist_locked() returned "
               "before calling hal_kv_open(HAL_KV_MODE_READ_WRITE, ...)/hal_kv_set_blob() at all");

    // 2026-09-06 flash-safety review: the namespace-level check above is
    // strictly stronger (no namespace means no key either), but restore the
    // original, more specific key-level assertion too -- a READ_WRITE open
    // auto-creates the namespace (fake_kv.c's hal_kv_open(), same as real
    // NVS's nvs_open_from_partition(..., NVS_READWRITE, ...)), which is fine
    // here since it only happens AFTER the namespace-absence check above has
    // already run and proven persist_locked() itself never got that far.
    hal_kv_handle_t hw;
    TEST_CHECK(hal_kv_open(&hw, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "test fixture: read-write reopen for the key-level check succeeds");
    run_state_record_t readback;
    size_t len = sizeof(readback);
    hal_status_t get_err = hal_kv_get_blob(&hw, NVS_KEY_RUN, &readback, &len);
    hal_kv_close(&hw);
    TEST_CHECK(get_err == HAL_NOT_FOUND,
               "NVS_KEY_RUN itself is absent -- the refused write left no blob behind, not just no "
               "namespace");
}

static void test_persist_locked_proceeds_normally_on_an_internal_ram_stack(void)
{
    TEST_SECTION("run_state persist_locked -- proceeds normally when the calling task's stack "
                 "is internal RAM");
    reset_all();

    run_state_record_t rec = make_sample_record();

    // fake_kv_set_write_safe_here(true) is the fake's default state (also reset_all()'s).
    esp_err_t err = persist_locked(&rec);

    TEST_CHECK(err == ESP_OK, "the guard does not fire on an internal-RAM stack -- the write "
                              "proceeds and lands in the fake_kv store");

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
               "fake_kv opens for read-back");
    run_state_record_t readback;
    size_t len = sizeof(readback);
    hal_status_t get_err = hal_kv_get_blob(&h, NVS_KEY_RUN, &readback, &len);
    hal_kv_close(&h);
    TEST_CHECK(get_err == HAL_OK && len == sizeof(readback), "the record round-trips through the fake_kv store");
    TEST_CHECK(readback.profile_id == rec.profile_id && readback.segment_index == rec.segment_index,
               "the persisted bytes are the record that was passed in, unmodified");
}

void run_test_run_state(void)
{
    test_persist_locked_refuses_when_calling_stack_is_external_ram();
    test_persist_locked_proceeds_normally_on_an_internal_ram_stack();

    fake_kv_reset_all(); // leave shared fake state as every other test file in this binary expects
}

void run_test_relay_cycles(void); // defined in test_relay_cycles.c, this executable's other TU

int main(void)
{
    run_test_run_state();
    run_test_relay_cycles();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
