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
// !hal_kv_write_safe_here() -- see HW_ABSTRACTION.md Phase 3 item 3,
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
#include "../drivers/persist/legacy_default_nvs.h"

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

static void stage_default_run_record(void)
{
    hal_kv_init_partition(NULL);
    run_state_record_t rec = make_sample_record();
    hal_kv_handle_t hw;
    TEST_CHECK(hal_kv_open(&hw, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, NULL) == HAL_OK, "open default partition");
    TEST_CHECK(hal_kv_set_blob(&hw, NVS_KEY_RUN, &rec, sizeof(rec)) == HAL_OK, "stage legacy record");
    TEST_CHECK(hal_kv_commit(&hw) == HAL_OK, "commit legacy record");
    hal_kv_close(&hw);
}

static bool kiln_run_record_present(void)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) != HAL_OK) {
        return false;
    }
    run_state_record_t rb;
    size_t len = sizeof(rb);
    hal_status_t st = hal_kv_get_blob(&h, NVS_KEY_RUN, &rb, &len);
    hal_kv_close(&h);
    return st == HAL_OK;
}

static void test_migration_resurrects_without_legacy_erase_and_not_with(void)
{
    TEST_SECTION("run_state migrate_from_default_partition -- a kiln reset's legacy erase stops the "
                 "stale default-partition record being copied back");
    reset_all();
    stage_default_run_record();
    migrate_from_default_partition();
    TEST_CHECK(kiln_run_record_present(), "control: without the erase the stale record is migrated in");

    reset_all();
    stage_default_run_record();
    TEST_CHECK(legacy_default_nvs_erase_kiln() == ESP_OK, "legacy erase succeeds");
    migrate_from_default_partition();
    TEST_CHECK(!kiln_run_record_present(), "after the legacy erase nothing is migrated: fallback yields defaults");
}

static void test_migration_erases_source_and_never_overwrites_destination(void)
{
    TEST_SECTION("run_state migrate_from_default_partition -- copies once, verifies, erases the legacy key; "
                 "never overwrites an existing kiln_nvs record (DEV_FIRMWARE_REVIEW_2 finding 3)");
    reset_all();
    stage_default_run_record();
    migrate_from_default_partition();
    TEST_CHECK(kiln_run_record_present(), "first boot: record migrated into kiln_nvs");
    hal_kv_handle_t h;
    run_state_record_t rb;
    size_t len = sizeof(rb);
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, NULL) == HAL_OK, "default ns still opens");
    TEST_CHECK(hal_kv_get_blob(&h, NVS_KEY_RUN, &rb, &len) == HAL_NOT_FOUND, "legacy source erased after verified copy");
    hal_kv_close(&h);

    /* The live record then moves on (a newer firing); a second boot must not resurrect the stale one. */
    run_state_record_t live = make_sample_record();
    live.profile_id = 5;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK, "open kiln");
    TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_RUN, &live, sizeof(live)) == HAL_OK, "write newer live record");
    hal_kv_commit(&h);
    hal_kv_close(&h);
    stage_default_run_record(); /* a stale legacy copy reappears (e.g. rollback+reflash) */
    migrate_from_default_partition();
    len = sizeof(rb);
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK, "reopen kiln");
    TEST_CHECK(hal_kv_get_blob(&h, NVS_KEY_RUN, &rb, &len) == HAL_OK && rb.profile_id == 5,
               "destination already populated: stale legacy record did NOT overwrite it");
    hal_kv_close(&h);
}

/* legacy_default_nvs.h promises key-name drift fails a host test. The
 * run_state/relay_cycles names are exercised by the real migrations above; the
 * zones and profiles migrations live in files that cannot be linked here, so
 * pin their literal key names by scanning the source of truth. */
static bool source_contains(const char *rel_from_test_dir, const char *needle)
{
    char path[512];
    snprintf(path, sizeof(path), "%s", __FILE__);
    char *slash = strrchr(path, '\\');
    char *slash2 = strrchr(path, '/');
    if (slash2 && (!slash || slash2 > slash)) {
        slash = slash2;
    }
    if (!slash) {
        return false;
    }
    snprintf(slash + 1, sizeof(path) - (size_t)(slash + 1 - path), "%s", rel_from_test_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false; /* fail loud: a missing source is a failed check, never a skip */
    }
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static void test_legacy_key_names_match_zones_and_profiles_sources(void)
{
    TEST_SECTION("legacy_default_nvs key names -- zones and profiles names match their migrating modules");
    TEST_CHECK(source_contains("../drivers/persist/zones_http_internal.h", "#define NVS_KEY_ZONES \"zones_cfg\""),
               "zones_cfg key name matches zones_http_internal.h");
    TEST_CHECK(source_contains("../drivers/http/profiles_http.c", "#define NVS_KEY_USED \"prof_used\""),
               "prof_used key name matches profiles_http.c");
    TEST_CHECK(source_contains("../drivers/http/profiles_http.c", "snprintf(out, out_cap, \"prof%u\", id)"),
               "profN key format matches profile_nvs_key()");
    TEST_CHECK(source_contains("../drivers/http/profiles_http.c", "#define NVS_NAMESPACE \"kiln_cfg\""),
               "kiln_cfg namespace matches profiles_http.c");
    /* And the erase really removes exactly those keys. */
    reset_all();
    hal_kv_init_partition(NULL);
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, "kiln_cfg", HAL_KV_MODE_READ_WRITE, NULL) == HAL_OK, "open");
    uint8_t one = 1;
    hal_kv_set_blob(&h, "zones_cfg", &one, 1);
    hal_kv_set_u8(&h, "prof_used", 1);
    hal_kv_set_blob(&h, "prof0", &one, 1);
    hal_kv_set_blob(&h, "prof7", &one, 1);
    hal_kv_commit(&h);
    hal_kv_close(&h);
    TEST_CHECK(legacy_default_nvs_erase_kiln() == ESP_OK && legacy_default_nvs_erase_profiles() == ESP_OK, "erases");
    TEST_CHECK(hal_kv_open(&h, "kiln_cfg", HAL_KV_MODE_READ_ONLY, NULL) == HAL_OK, "reopen");
    uint8_t b[4];
    size_t l = sizeof(b);
    uint8_t u = 0;
    TEST_CHECK(hal_kv_get_blob(&h, "zones_cfg", b, &l) == HAL_NOT_FOUND, "zones_cfg erased");
    TEST_CHECK(hal_kv_get_u8(&h, "prof_used", &u) == HAL_NOT_FOUND, "prof_used erased");
    l = sizeof(b);
    TEST_CHECK(hal_kv_get_blob(&h, "prof0", b, &l) == HAL_NOT_FOUND, "prof0 erased");
    l = sizeof(b);
    TEST_CHECK(hal_kv_get_blob(&h, "prof7", b, &l) == HAL_NOT_FOUND, "prof7 erased");
    hal_kv_close(&h);
}

static void test_legacy_erase_does_not_create_missing_namespace(void)
{
    TEST_SECTION("legacy_default_nvs erase -- a missing kiln_cfg namespace is left missing (finding 7)");
    reset_all();
    hal_kv_init_partition(NULL);
    TEST_CHECK(legacy_default_nvs_erase_kiln() == ESP_OK, "erase on missing namespace is OK");
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, "kiln_cfg", HAL_KV_MODE_READ_ONLY, NULL) == HAL_NOT_FOUND,
               "the erase did not create an empty namespace");
}

void run_test_run_state(void)
{
    test_persist_locked_refuses_when_calling_stack_is_external_ram();
    test_persist_locked_proceeds_normally_on_an_internal_ram_stack();
    test_migration_resurrects_without_legacy_erase_and_not_with();
    test_migration_erases_source_and_never_overwrites_destination();
    test_legacy_key_names_match_zones_and_profiles_sources();
    test_legacy_erase_does_not_create_missing_namespace();

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
