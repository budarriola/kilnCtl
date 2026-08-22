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

esp_err_t safety_link_get_config_page(SafetyLinkClass *link, uint8_t page_index, kilnlink_config_page_t *out)
{
    (void)link;
    s_stub_get_config_page_calls++;
    if (s_stub_fail_at_page >= 0 && (int)page_index == s_stub_fail_at_page) {
        return s_stub_page_err;
    }
    if (page_index >= s_stub_page_count) {
        return ESP_ERR_INVALID_ARG; // test asked for a page never staged -- a test bug, not a real path
    }
    *out = s_stub_pages[page_index];
    return ESP_OK;
}

static void stub_reset(void)
{
    memset(s_stub_pages, 0, sizeof(s_stub_pages));
    s_stub_page_count = 0;
    s_stub_page_err = ESP_OK;
    s_stub_fail_at_page = -1;
    s_stub_get_config_page_calls = 0;
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

void run_test_safety_cfg_store(void)
{
    test_index_for_id_finds_known_and_rejects_unknown();
    test_no_refetch_when_crc_unchanged();
    test_refetch_when_crc_changes();
    test_refetch_pages_until_more_is_false();
    test_refetch_unknown_id_is_skipped_not_fatal();
    test_refetch_failure_leaves_cache_untouched();
    test_unset_param_reports_set_false();
    test_lookup_by_id();
    test_version_refuse_newer_than_firmware();
}
