// Host tests for App/drivers/safety_cfg_store.c -- the ESP-side NVS-backed
// cache of the RP2040 safety processor's commissioning parameter set
// (docs/COMMISSIONING.md sec 3).
//
// safety_cfg_store.c is #included directly (same convention as
// test_kiln_cfg_store.c/test_backup_import.c) so this file can reach its
// static helpers and s_store/s_fetched_at_us directly. Its one real
// dependency on "the wire", safety_link_get_config_page(), is stubbed here
// as a plain, controllable C function returning canned pages -- there is no
// real SafetyLinkClass/UART in a host test, same split test_kiln_cfg_store.c
// uses for zones_config_export_blob()/_import_blob().
#include <string.h>

#include "test_common.h"

#include "esp_err.h"

#include "../drivers/safety_cfg_store.c"

// ---------------------------------------------------------------------------
// safety_link_get_config_page() stub -- controllable canned pages.
// ---------------------------------------------------------------------------

#define STUB_MAX_PAGES 4
static kilnlink_config_page_t s_stub_pages[STUB_MAX_PAGES];
static size_t s_stub_page_count = 0;
static esp_err_t s_stub_page_err = ESP_OK;      // returned instead of a real page, if != ESP_OK
static int s_stub_fail_at_page = -1;            // page index at which s_stub_page_err is returned
static int s_stub_get_config_page_calls = 0;
// Simulates wall-clock time actually elapsing inside safety_link_get_config_
// page() -- on real hardware each call can take up to SAFETY_LINK_REPLY_
// TIMEOUT_MS (~1.2s). The stub esp_timer is otherwise frozen (host tests have
// no real clock), so a budget-exhaustion test has to advance it explicitly;
// this lets a test do that per-call instead of hand-rolling esp_timer_test_
// set_now_us() calls around every stage_page().
static int64_t s_stub_advance_us_per_call = 0;

esp_err_t safety_link_get_config_page(SafetyLinkClass *link, uint8_t page_index, kilnlink_config_page_t *out)
{
    (void)link;
    s_stub_get_config_page_calls++;
    if (s_stub_advance_us_per_call > 0) {
        esp_timer_test_set_now_us(esp_timer_get_time() + s_stub_advance_us_per_call);
    }
    if (s_stub_fail_at_page >= 0 && (int)page_index == s_stub_fail_at_page) {
        return s_stub_page_err;
    }
    if (page_index >= s_stub_page_count) {
        return ESP_ERR_INVALID_ARG; // test asked for a page never staged -- a test bug, not a real path
    }
    *out = s_stub_pages[page_index];
    return ESP_OK;
}

// safety_link_clear_stashed_config_page() stub -- safety_cfg_store_maybe_
// refetch() calls this after consuming a page (see safety_link.c's comment
// at safety_link_clear_stashed_config_page()); no stashed-page state exists
// in this host test's fake link, so there is nothing to do.
static int s_stub_clear_stashed_config_page_calls = 0;
void safety_link_clear_stashed_config_page(SafetyLinkClass *link)
{
    (void)link;
    s_stub_clear_stashed_config_page_calls++;
}

// ---------------------------------------------------------------------------
// uart_bridge_ext_run_on_flash_worker() stub -- 2026-08-23 panic fix.
// safety_cfg_store.c no longer calls nvs_save_store() directly; it hands the
// job to this function instead (see safety_cfg_store_flush_if_dirty()). The
// real implementation (uart_bridge_ext.c) runs the job on a SEPARATE task's
// stack; this stub, like every other host test in this file, is single-
// threaded, so it simply invokes the job inline -- exactly what the real
// worker does from the caller's point of view (it blocks until the job
// completes), so nvs_save_store()'s own behavior/stubbing is unaffected.
// Controllable so a test can simulate the worker being unavailable (the
// "boot ordering" case safety_cfg_store_flush_if_dirty()'s own comment
// describes) without needing a real second task.
// ---------------------------------------------------------------------------

static int s_stub_flash_worker_calls = 0;
static esp_err_t s_stub_flash_worker_submit_err = ESP_OK; // returned instead of running the job, if != ESP_OK

esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg)
{
    s_stub_flash_worker_calls++;
    if (s_stub_flash_worker_submit_err != ESP_OK) {
        return s_stub_flash_worker_submit_err; // simulates "worker not started" -- job never runs
    }
    if (fn) {
        fn(arg);
    }
    return ESP_OK;
}

static void stub_reset(void)
{
    memset(s_stub_pages, 0, sizeof(s_stub_pages));
    s_stub_page_count = 0;
    s_stub_page_err = ESP_OK;
    s_stub_fail_at_page = -1;
    s_stub_get_config_page_calls = 0;
    s_stub_advance_us_per_call = 0;
    s_stub_flash_worker_calls = 0;
    s_stub_flash_worker_submit_err = ESP_OK;
}

// One page containing entries for the given (id, u16 value) pairs, `more`
// set as given.
static void stage_page(size_t page_idx, bool more, const uint16_t *ids, const uint16_t *vals, size_t n)
{
    kilnlink_config_page_t *p = &s_stub_pages[page_idx];
    memset(p, 0, sizeof(*p));
    p->page_index = (uint8_t)page_idx;
    p->entry_count = (uint8_t)n;
    p->more = more ? 1 : 0;
    for (size_t i = 0; i < n; i++) {
        p->entries[i].param_id = ids[i];
        p->entries[i].type = KILNLINK_PARAM_TYPE_U16;
        p->entries[i].value.u16_val = vals[i];
    }
    if (page_idx + 1 > s_stub_page_count) {
        s_stub_page_count = page_idx + 1;
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void reset_all(void)
{
    stub_reset();
    reset_to_defaults();
    esp_timer_test_set_now_us(0);
    s_fetched_at_us = -1;
    s_dirty = false; /* 2026-08-23 fix -- a prior test's unflushed write must not bleed into the next */
    esp_ptr_external_ram_test_set(false); /* default: called from a normal, internal-RAM stack */
    nvs_test_enable(false); /* every test except the version-refuse one runs without real NVS */
    nvs_test_clear();
}

static void test_index_for_id_finds_known_and_rejects_unknown(void)
{
    TEST_SECTION("index_for_id -- every table row is reachable by its own id, an unknown id is not");

    // 0x0101 is the table's first row (tc_source); 0x0504 is its last
    // (config_check_period_s). Both ends, not just one, so a future
    // off-by-one in the table's bounds shows up here.
    TEST_CHECK(index_for_id(0x0101) == 0, "first table row (tc_source) is index 0");
    TEST_CHECK(index_for_id(0x0504) == (int)(SAFETY_CFG_PARAM_COUNT - 1),
               "last table row (config_check_period_s) is the last index");
    TEST_CHECK(index_for_id(0xBEEF) == -1, "an id no CONFIG_REFERENCE.md section uses is not found");
    TEST_CHECK(safety_cfg_store_param_count() == SAFETY_CFG_PARAM_COUNT,
               "safety_cfg_store_param_count() matches the table size exactly");
}

static void test_no_refetch_when_crc_unchanged(void)
{
    TEST_SECTION("safety_cfg_store_maybe_refetch -- steady state (same CRC) touches the wire NOT AT ALL");
    reset_all();

    s_store.config_crc = 0x1234;
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));

    bool refetched = safety_cfg_store_maybe_refetch(&fake_link, 0x1234);

    TEST_CHECK(refetched == false, "same CRC -- maybe_refetch reports nothing changed");
    TEST_CHECK(s_stub_get_config_page_calls == 0,
               "a steady-state reboot (matching CRCs) must transfer NOTHING -- "
               "safety_link_get_config_page() was never even called");
}

static void test_refetch_when_crc_changes(void)
{
    TEST_SECTION("safety_cfg_store_maybe_refetch -- a differing CRC DOES trigger a real fetch");
    reset_all();

    s_store.config_crc = 0x0001;
    s_store.entries[0].set = 1;
    s_store.entries[0].value.u8_val = 9; // stale value that must be replaced

    uint16_t ids[] = { 0x0101 };  // tc_source, table index 0, but table row is U8 -- value ignored by
                                  // this stub page (U16-only helper); use a U16 field instead below.
    (void)ids;
    uint16_t ids2[] = { 0x0203 }; // overshoot_time_s (U16), table index 10
    uint16_t vals2[] = { 42 };
    stage_page(0, false, ids2, vals2, 1);

    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    esp_timer_test_set_now_us(5000000); // 5s, arbitrary

    bool refetched = safety_cfg_store_maybe_refetch(&fake_link, 0x0002);

    TEST_CHECK(refetched == true, "a differing CRC causes a real refetch");
    TEST_CHECK(s_stub_get_config_page_calls == 1, "exactly one page was requested (single stub page, more=0)");
    TEST_CHECK(safety_cfg_store_cached_crc() == 0x0002, "cached_crc now matches the fetched live CRC");

    safety_cfg_param_t p;
    TEST_CHECK(safety_cfg_store_get_by_index(10, &p) && p.set && p.value.u16_val == 42,
               "overshoot_time_s (index 10) picked up the fetched value");
    TEST_CHECK(safety_cfg_store_get_by_index(0, &p) && !p.set,
               "tc_source (index 0), NOT present in the fetched page, reads back as unset -- "
               "the whole cache was replaced, not merged with the stale entry that used to be there");
}

static void test_refetch_pages_until_more_is_false(void)
{
    TEST_SECTION("safety_cfg_store_refetch -- pages through until `more == 0`");
    reset_all();

    uint16_t ids_a[] = { 0x0203 };
    uint16_t vals_a[] = { 100 };
    stage_page(0, true, ids_a, vals_a, 1); // more=1 -- expect a second request

    uint16_t ids_b[] = { 0x0205 };
    uint16_t vals_b[] = { 200 };
    stage_page(1, false, ids_b, vals_b, 1); // more=0 -- stop here

    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));

    bool ok = safety_cfg_store_refetch(&fake_link, 0xABCD);

    TEST_CHECK(ok == true, "a two-page fetch with more=1 then more=0 succeeds");
    TEST_CHECK(s_stub_get_config_page_calls == 2, "exactly two pages were requested, not more, not fewer");
    safety_cfg_param_t p;
    TEST_CHECK(safety_cfg_store_get_by_index(10, &p) && p.set && p.value.u16_val == 100,
               "page 0's entry landed");
    TEST_CHECK(safety_cfg_store_get_by_index(12, &p) && p.set && p.value.u16_val == 200,
               "page 1's entry landed too (index 12 == rate_window_s, 0x0205)");
}

static void test_refetch_unknown_id_is_skipped_not_fatal(void)
{
    TEST_SECTION("safety_cfg_store_refetch -- an id this build's table predates is skipped, not an error");
    reset_all();

    uint16_t ids[] = { 0x0203, 0x9999 }; // 0x9999: a future field this ESP doesn't know about yet
    uint16_t vals[] = { 7, 8 };
    stage_page(0, false, ids, vals, 2);

    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));

    bool ok = safety_cfg_store_refetch(&fake_link, 0x0010);

    TEST_CHECK(ok == true, "an unrecognised id in the page does not fail the whole refetch "
                           "(COMMISSIONING.md sec 2's version-tolerance)");
    safety_cfg_param_t p;
    TEST_CHECK(safety_cfg_store_get_by_index(10, &p) && p.set && p.value.u16_val == 7,
               "the recognised id in the same page still landed");
}

static void test_refetch_failure_leaves_cache_untouched(void)
{
    TEST_SECTION("safety_cfg_store_refetch -- a failed page request leaves the PREVIOUS cache intact");
    reset_all();

    // Seed a "previous, good" cache.
    s_store.config_crc = 0x00AA;
    s_store.entries[10].set = 1;
    s_store.entries[10].value.u16_val = 111;

    s_stub_fail_at_page = 0;
    s_stub_page_err = ESP_ERR_TIMEOUT;

    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));

    bool ok = safety_cfg_store_refetch(&fake_link, 0x00BB);

    TEST_CHECK(ok == false, "a timed-out page request fails the refetch");
    TEST_CHECK(safety_cfg_store_cached_crc() == 0x00AA,
               "cached_crc is UNCHANGED -- an interrupted refetch must never leave a half-updated "
               "cache with no way to tell old data from new");
    safety_cfg_param_t p;
    TEST_CHECK(safety_cfg_store_get_by_index(10, &p) && p.set && p.value.u16_val == 111,
               "the previously-cached value at index 10 is still exactly what it was");
}

static void test_unset_param_reports_set_false(void)
{
    TEST_SECTION("safety_cfg_store_get_by_index -- a param never fetched reads set=false");
    reset_all();

    safety_cfg_param_t p;
    TEST_CHECK(safety_cfg_store_get_by_index(0, &p) == true, "a valid index always returns true");
    TEST_CHECK(p.set == false, "a fresh (never-fetched) cache entry is unset");
    TEST_CHECK(p.param_id == 0x0101 && strcmp(p.name, "tc_source") == 0,
               "index 0's id/name are tc_source, matching COMMISSIONING.md sec 2.1's table order");

    TEST_CHECK(safety_cfg_store_get_by_index(SAFETY_CFG_PARAM_COUNT, &p) == false,
               "an out-of-range index is rejected, not silently clamped");
}

static void test_lookup_by_id(void)
{
    TEST_SECTION("safety_cfg_store_lookup -- type/name by id, for the POST handler's per-id validation");
    reset_all();

    uint8_t type = 0xFF;
    const char *name = NULL;
    TEST_CHECK(safety_cfg_store_lookup(0x0104, &type, &name) == true, "abs_max_temp_c is a known id");
    TEST_CHECK(type == KILNLINK_PARAM_TYPE_F32, "abs_max_temp_c is F32, matching CONFIG_REFERENCE.md");
    TEST_CHECK(name != NULL && strcmp(name, "abs_max_temp_c") == 0, "the name matches too");

    TEST_CHECK(safety_cfg_store_lookup(0x9999, &type, &name) == false,
               "an id no section uses is refused, not silently accepted with a guessed type");
}

static void test_version_refuse_newer_than_firmware(void)
{
    TEST_SECTION("nvs_load_store -- a blob from NEWER firmware is refused, not reinterpreted");
    reset_all();
    nvs_test_enable(true); // this test alone needs a real stub NVS round trip

    // Simulate what would have been persisted by a hypothetical v2: same
    // struct shape here (no v2 exists yet), but version byte bumped past
    // what this build knows -- nvs_load_store() must refuse it wholesale
    // rather than trust the old-version bytes underneath. Written directly
    // into the stub's blob storage (not via nvs_save_store(), which always
    // stamps the CURRENT version before writing -- exactly the behavior a
    // real save path should have, but it means it cannot be used to
    // manufacture a newer-than-current blob for this test).
    safety_cfg_store_blob_t fake_newer;
    memset(&fake_newer, 0, sizeof(fake_newer));
    fake_newer.version = SAFETY_CFG_STORE_VERSION + 1;
    fake_newer.entries[0].set = 1;
    fake_newer.entries[0].value.u8_val = 3;
    memcpy(s_stub_nvs_blob, &fake_newer, sizeof(fake_newer));
    s_stub_nvs_blob_len = sizeof(fake_newer);
    s_stub_nvs_has_blob = true;

    nvs_load_store(); // should refuse and reset to empty defaults

    TEST_CHECK(s_store.config_crc == 0, "a refused (newer) blob leaves the cache at its empty default");
    safety_cfg_param_t p;
    TEST_CHECK(safety_cfg_store_get_by_index(0, &p) && !p.set,
               "the newer blob's data was NOT loaded -- refused wholesale, per COMMISSIONING.md "
               "sec 1's own reasoning for the identical rule on the Pico's config_store");

    nvs_test_enable(false); // leave the shared stub state as every other test in this binary expects
}

// ---------------------------------------------------------------------------
// SAFETY_CFG_STORE_REFETCH_BUDGET_MS -- 2026-08-23 fix. Pins the bound that
// keeps a multi-page refetch from accumulating unbounded wall-clock time
// inside one safety_poll_task iteration (see that constant's own comment in
// safety_cfg_store.c). A future edit that removed or loosened this cap
// without noticing would silently reintroduce the risk it closes, so this
// must be able to fail against the unbounded code.
// ---------------------------------------------------------------------------

static void test_refetch_aborts_when_wall_clock_budget_exhausted(void)
{
    TEST_SECTION("safety_cfg_store_refetch -- a multi-page fetch that runs long ABORTS at the budget, "
                 "not after every page");
    reset_all();

    // Three pages, each carrying `more=1` except the last -- a real refetch
    // would need all three to complete. Values are irrelevant; only the
    // page count and the simulated per-call wall-clock cost matter here.
    uint16_t ids_a[] = { 0x0203 };
    uint16_t vals_a[] = { 1 };
    stage_page(0, true, ids_a, vals_a, 1);
    uint16_t ids_b[] = { 0x0205 };
    uint16_t vals_b[] = { 2 };
    stage_page(1, true, ids_b, vals_b, 1);
    uint16_t ids_c[] = { 0x0206 };
    uint16_t vals_c[] = { 3 };
    stage_page(2, false, ids_c, vals_c, 1);

    // Seed a previous good cache so a "left unchanged" claim is actually
    // checked, not vacuously true because the cache started empty.
    s_store.config_crc = 0x00AA;
    s_store.entries[10].set = 1;
    s_store.entries[10].value.u16_val = 111;

    // Each simulated page "costs" 1.3s of wall clock -- comfortably inside
    // SAFETY_LINK_REPLY_TIMEOUT_MS's real ~1.2s worst case (this is what a
    // slow-but-still-answering Pico looks like), so two pages already exceed
    // SAFETY_CFG_STORE_REFETCH_BUDGET_MS (2000ms) before a third is ever
    // attempted.
    s_stub_advance_us_per_call = 1300000; // 1.3s

    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));

    bool ok = safety_cfg_store_refetch(&fake_link, 0x00BB);

    TEST_CHECK(ok == false, "the refetch aborts once its wall-clock budget is exhausted, "
                            "even though every page it DID ask for would have succeeded");
    TEST_CHECK(s_stub_get_config_page_calls == 2,
               "exactly two pages were attempted (0 then 1, each costing 1.3s) before the budget "
               "check refused a third -- proves this is a wall-clock bound, not a page-count one");
    TEST_CHECK(safety_cfg_store_cached_crc() == 0x00AA,
               "the previous cache is untouched by an aborted-for-budget refetch, same as any "
               "other failed refetch");
    safety_cfg_param_t p;
    TEST_CHECK(safety_cfg_store_get_by_index(10, &p) && p.set && p.value.u16_val == 111,
               "the previously-cached value survives the aborted refetch intact");
}

static void test_refetch_within_budget_still_completes_all_pages(void)
{
    TEST_SECTION("safety_cfg_store_refetch -- a multi-page fetch that stays within budget still "
                 "completes normally");
    reset_all();

    uint16_t ids_a[] = { 0x0203 };
    uint16_t vals_a[] = { 10 };
    stage_page(0, true, ids_a, vals_a, 1);
    uint16_t ids_b[] = { 0x0205 };
    uint16_t vals_b[] = { 20 };
    stage_page(1, false, ids_b, vals_b, 1);

    // No simulated per-call cost here (s_stub_advance_us_per_call stays 0 --
    // reset_all() -> stub_reset() zeroes it) -- proves the budget check
    // itself never blocks a fetch that is actually fast, only one that is
    // genuinely slow/stalled.
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));

    bool ok = safety_cfg_store_refetch(&fake_link, 0xCAFE);

    TEST_CHECK(ok == true, "a two-page fetch that costs no simulated wall-clock time completes normally");
    TEST_CHECK(s_stub_get_config_page_calls == 2, "both pages were fetched -- the budget check "
                                                   "never fires when nothing is actually slow");
    TEST_CHECK(safety_cfg_store_cached_crc() == 0xCAFE, "the fetch committed -- cache updated");
}

// ---------------------------------------------------------------------------
// Deferred NVS flush + wrong-task guard -- 2026-08-23 panic fix. A successful
// safety_cfg_store_refetch() (called from safety_poll_task, whose stack is
// PSRAM) used to call nvs_save_store() directly, which aborts the whole
// board on real hardware the moment it actually executes (ESP-IDF's
// esp_task_stack_is_sane_cache_disabled(), see safety_cfg_store.c's
// caller_stack_is_external() comment). Two things are pinned here: the
// refetch path now goes through the flash-safe worker (uart_bridge_ext_
// run_on_flash_worker(), stubbed above) instead of writing directly, and the
// wrong-task guard inside nvs_save_store() itself refuses (rather than
// crashing) if ever called with an external-RAM stack underneath it.
// ---------------------------------------------------------------------------

static void test_successful_refetch_flushes_via_the_flash_worker_not_directly(void)
{
    TEST_SECTION("safety_cfg_store_refetch -- a successful fetch flushes via the flash-safe "
                 "worker, not nvs_save_store() run directly on the caller");
    reset_all();
    nvs_test_enable(true); // this test checks the flush actually SUCCEEDED (s_dirty cleared), so it
                            // needs the stub's real NVS round trip, not the "every open fails closed"
                            // default every other test in this file relies on.

    uint16_t ids[] = { 0x0203 };
    uint16_t vals[] = { 55 };
    stage_page(0, false, ids, vals, 1);

    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));

    bool ok = safety_cfg_store_refetch(&fake_link, 0x1234);

    TEST_CHECK(ok == true, "the refetch itself still succeeds");
    TEST_CHECK(s_stub_flash_worker_calls == 1,
               "the NVS flush was handed to uart_bridge_ext_run_on_flash_worker() exactly once -- "
               "this is the ONLY route safety_cfg_store.c may use to reach nvs_save_store() now");
    TEST_CHECK(s_dirty == false, "a successful flush clears the dirty flag");

    nvs_test_enable(false); // leave the shared stub state as every other test in this binary expects
}

static void test_flush_is_a_noop_when_nothing_is_dirty(void)
{
    TEST_SECTION("safety_cfg_store_flush_if_dirty -- a no-op (and no worker call) when nothing changed");
    reset_all();

    TEST_CHECK(s_dirty == false, "a freshly reset store is not dirty");
    esp_err_t err = safety_cfg_store_flush_if_dirty();

    TEST_CHECK(err == ESP_OK, "flushing a clean store reports success trivially");
    TEST_CHECK(s_stub_flash_worker_calls == 0,
               "the flash-safe worker is never bothered when there is nothing to persist");
}

static void test_flush_worker_unavailable_leaves_store_dirty_for_a_later_retry(void)
{
    TEST_SECTION("safety_cfg_store_flush_if_dirty -- the worker being unavailable leaves the "
                 "cache dirty for the next attempt, rather than silently dropping the write");
    reset_all();
    nvs_test_enable(true); // the RETRY flush below needs to actually succeed to prove s_dirty clears

    uint16_t ids[] = { 0x0203 };
    uint16_t vals[] = { 77 };
    stage_page(0, false, ids, vals, 1);

    s_stub_flash_worker_submit_err = ESP_FAIL; // simulates "worker not started yet" (boot ordering)

    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));

    bool ok = safety_cfg_store_refetch(&fake_link, 0x5678);

    TEST_CHECK(ok == true, "the refetch itself still reports success -- the in-RAM cache DID update, "
                           "only the flash persist failed");
    TEST_CHECK(s_dirty == true, "the dirty flag stays set -- the change is live but not yet on flash");

    // Now simulate the worker coming up and a later flush attempt succeeding.
    s_stub_flash_worker_submit_err = ESP_OK;
    esp_err_t err = safety_cfg_store_flush_if_dirty();

    TEST_CHECK(err == ESP_OK, "a later flush, once the worker is available, succeeds");
    TEST_CHECK(s_dirty == false, "the dirty flag clears once the deferred write actually lands");
    TEST_CHECK(s_stub_flash_worker_calls == 2,
               "the worker was asked twice: once inside the failed refetch, once on the explicit retry");

    nvs_test_enable(false); // leave the shared stub state as every other test in this binary expects
}

static void test_nvs_save_store_refuses_when_calling_stack_is_external_ram(void)
{
    TEST_SECTION("nvs_save_store -- refuses (does not crash) when called with a PSRAM stack underneath it");
    reset_all();

    esp_ptr_external_ram_test_set(true); // simulate being called from a PSRAM-stacked task

    esp_err_t err = nvs_save_store();

    TEST_CHECK(err == ESP_ERR_INVALID_STATE,
               "the wrong-task guard refuses with a diagnosable error, not a crash, exactly the "
               "class of bug (an NVS write reached from a PSRAM-stack task) this whole fix closes");

    esp_ptr_external_ram_test_set(false); // leave shared stub state as every other test expects
}

static void test_nvs_save_store_proceeds_normally_on_an_internal_ram_stack(void)
{
    TEST_SECTION("nvs_save_store -- proceeds normally when the calling task's stack is internal RAM");
    reset_all();
    nvs_test_enable(true); // exercise the real stub NVS round trip for this one

    // esp_ptr_external_ram_test_set(false) is reset_all()'s implicit state
    // (the stub defaults to false and nothing here has set it true).
    s_store.config_crc = 0x9999;
    esp_err_t err = nvs_save_store();

    TEST_CHECK(err == ESP_OK, "the guard does not fire on an internal-RAM stack -- the write proceeds "
                              "and succeeds exactly as it always did");

    nvs_test_enable(false); // leave the shared stub state as every other test in this binary expects
}

void run_test_safety_cfg_store(void)
{
    test_index_for_id_finds_known_and_rejects_unknown();
    test_no_refetch_when_crc_unchanged();
    test_refetch_when_crc_changes();
    test_refetch_pages_until_more_is_false();
    test_refetch_unknown_id_is_skipped_not_fatal();
    test_refetch_failure_leaves_cache_untouched();
    test_refetch_aborts_when_wall_clock_budget_exhausted();
    test_refetch_within_budget_still_completes_all_pages();
    test_successful_refetch_flushes_via_the_flash_worker_not_directly();
    test_flush_is_a_noop_when_nothing_is_dirty();
    test_flush_worker_unavailable_leaves_store_dirty_for_a_later_retry();
    test_nvs_save_store_refuses_when_calling_stack_is_external_ram();
    test_nvs_save_store_proceeds_normally_on_an_internal_ram_stack();
    test_unset_param_reports_set_false();
    test_lookup_by_id();
    test_version_refuse_newer_than_firmware();
}
