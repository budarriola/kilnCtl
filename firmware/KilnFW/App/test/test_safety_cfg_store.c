// Host tests for App/drivers/safety/safety_cfg_store.c -- the ESP-side NVS-backed
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
#include <math.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h" /* hal_kv.h's host fake -- safety_cfg_store.c now calls hal_kv_*() instead of
                       * nvs_*() directly (HW_ABSTRACTION.md Phase 3 item 3) */
#include "fake_time.h" /* hal_time.h's host fake -- safety_cfg_store.c now calls hal_time_now_us()
                         * instead of esp_timer_get_time(); see reset_all()/the tests below that
                         * used to drive the old stub esp_timer via esp_timer_test_set_now_us(). */

// 2026-08-28 audit fix (N2): safety_cfg_store.c's poll-side entry point
// (safety_cfg_store_refetch_nonblocking()) now genuinely checks
// xSemaphoreTake()'s return value -- stubs/freertos/semphr.h's shared stub
// deliberately always returns pdFALSE (test_boot_button.c's own header
// comment), which would make every safety_cfg_store_maybe_refetch() test
// below dead-end at "lock busy, nothing fetched" instead of reaching the
// refetch logic under test. Same fix shape test_ota_http.c's own header
// comment documents for the identical problem: pull in the real freertos/
// semphr.h first, then macro-redirect xSemaphoreTake to a LOCAL replacement
// with real single-threaded mutex semantics (always succeeds for a non-NULL
// handle) for exactly this file's #include of safety_cfg_store.c, restored
// immediately after so nothing else is affected.
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <assert.h>
// Controllable, not hard-wired to pdTRUE: a real single-threaded mutex would
// always succeed, but the whole point of the N2 fix under test is that
// safety_cfg_store_refetch_nonblocking() reacts correctly to a FAILED
// non-blocking take (the httpd worker holding the lock right now) by
// bailing out without touching the wire. test_refetch_nonblocking_when_
// locked() below flips this to pdFALSE to exercise exactly that path;
// every other test leaves it at the default (available).
static BaseType_t s_test_semaphore_take_result = pdTRUE;
static inline BaseType_t safety_cfg_store_test_xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    assert(sem != NULL && "xSemaphoreTake on a NULL handle -- would assert/panic on real FreeRTOS");
    (void)ticks;
    return s_test_semaphore_take_result;
}
#define xSemaphoreTake safety_cfg_store_test_xSemaphoreTake

#include "../drivers/safety/safety_cfg_store.c"

#undef xSemaphoreTake

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
        fake_time_advance_us((uint64_t)s_stub_advance_us_per_call);
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

// relay_cycles_set_type() stub -- RELAY_LIFE_BUDGET.md.
// safety_cfg_store.c now calls this (both at init/load and from
// safety_cfg_store_set_safety_relay_type()) instead of linking the real
// relay_cycles.c, same "fake the cross-module dependency, don't drag in its
// own NVS machinery" convention as safety_link_get_config_page()'s stub
// above -- this file already has its own hal_kv fake in play and relay_
// cycles.c's persistence is out of scope for these tests.
static int s_stub_relay_cycles_set_type_calls = 0;
static uint8_t s_stub_relay_cycles_last_relay = 0xFF;
static relay_type_t s_stub_relay_cycles_last_type = RELAY_TYPE_SSR;
void relay_cycles_set_type(uint8_t relay, relay_type_t type, uint32_t rated_override)
{
    (void)rated_override;
    s_stub_relay_cycles_set_type_calls++;
    s_stub_relay_cycles_last_relay = relay;
    s_stub_relay_cycles_last_type = type;
}

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
    s_stub_relay_cycles_set_type_calls = 0;
    s_stub_relay_cycles_last_relay = 0xFF;
    s_stub_relay_cycles_last_type = RELAY_TYPE_SSR;
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
        // 2026-08-27 audit fix (commissioning-write defect d): every entry
        // this helper stages represents a field the (fake) Pico considers
        // SET -- matching every existing caller's own assertions (`p.set &&
        // p.value... == ...`) below. See stage_page_unset() for the OTHER
        // half: an entry the Pico reports UNSET.
        p->entries[i].set = true;
    }
    if (page_idx + 1 > s_stub_page_count) {
        s_stub_page_count = page_idx + 1;
    }
}

// Non-static wrapper for test_kiln_cfg_store.c (same main test executable,
// see build_host_tests.ps1's comment on linking safety_cfg_store.c for real
// only once): review_divergence_fixes_b2e7017f_2026-09-15.md MEDIUM 4's test
// needs to seed this file's fake safety_link_get_config_page() with real
// param ids/values so kiln_cfg_store_recapture_pico_half_confirmed() ->
// kiln_package_capture_pico_half() -> safety_cfg_store_get_by_index() (all
// real, all linked once, right here) actually reports them SET. stage_page()
// itself stays static/file-local; this is the one crossing point.
void test_safety_cfg_store_stage_page_for_kiln_cfg_store_test(size_t page_idx, bool more, const uint16_t *ids,
                                                                const uint16_t *vals, size_t n)
{
    stage_page(page_idx, more, ids, vals, n);
}

// Second non-static crossing point, added for the apply-time live-ceiling
// re-check test (owner decision 2026-09-16, docs/KILN_PROFILES_PLAN.md
// section 5.3 row 1's apply-time sibling): stage_page() above always encodes
// its wire values as KILNLINK_PARAM_TYPE_U16 with the union's upper 16 bits
// left zeroed by stage_page()'s own memset -- fine for the BOOL/U8 fields
// every existing caller seeds (a truthy/low-byte readback survives), but
// useless for a real F32 abs_max_temp_c reading (the result is an unusable
// denormal). This sets the single entry's declared type to F32 and its
// union's f32_val directly, so a test can seed a REAL, comparable ceiling
// value into the live safety_cfg_store cache via the same
// safety_cfg_store_refetch(&fake_link, crc) install path.
// Third crossing point: resets the live safety_cfg_store cache to its
// boot-time empty state (reset_to_defaults(), this file's own static
// helper). test_kiln_cfg_store.c's reset_state() calls this at the top of
// EVERY test -- without it, a live-cache seed staged by one board-id/
// ack_hardware_differs/live-ceiling test (owner decisions 2026-09-16) would
// silently leak into the next test that assumes a fresh, uncommissioned
// cache (the same assumption most pre-existing apply tests in that file
// already make).
void test_safety_cfg_store_reset_for_kiln_cfg_store_test(void)
{
    reset_to_defaults();
}

void test_safety_cfg_store_stage_f32_for_kiln_cfg_store_test(size_t page_idx, uint16_t id, float value)
{
    kilnlink_config_page_t *p = &s_stub_pages[page_idx];
    memset(p, 0, sizeof(*p));
    p->page_index = (uint8_t)page_idx;
    p->entry_count = 1;
    p->more = 0;
    p->entries[0].param_id = id;
    p->entries[0].type = KILNLINK_PARAM_TYPE_F32;
    p->entries[0].value.f32_val = value;
    p->entries[0].set = true;
    if (page_idx + 1 > s_stub_page_count) {
        s_stub_page_count = page_idx + 1;
    }
}

// Same as stage_page(), but the ONE entry at `unset_index` (0-based within
// this page) is staged with set=false -- simulates a Pico reporting a
// no-safe-default field (e.g. abs_max_temp_c) that has never been
// commissioned, per kilnlink_config_page.h's KILNLINK_CONFIG_PAGE_UNSET_BIT.
static void stage_page_with_one_unset(size_t page_idx, bool more, const uint16_t *ids, const uint16_t *vals,
                                       size_t n, size_t unset_index)
{
    stage_page(page_idx, more, ids, vals, n);
    s_stub_pages[page_idx].entries[unset_index].set = false;
}

// Table POSITION of a param id, resolved at run time rather than hardcoded.
// 2026-08-29 fix: every test below used to name overshoot_time_s (0x0203) as
// "index 10" and rate_window_s (0x0205) as "index 12" -- true when they were
// written, and silently wrong from commit babbfdd on, which inserted
// ct_installed (0x0109) in the middle of SAFETY_CFG_PARAM_TABLE and pushed
// both down by one. Five checks in this file then failed for the whole time
// that went unfixed. Asking the table where a field lives, instead of
// restating a number that only the table actually owns, makes these tests
// survive the next insertion -- and index_for_id() itself is pinned
// independently by test_index_for_id_finds_known_and_rejects_unknown().
static size_t idx_of(uint16_t param_id)
{
    int idx = index_for_id(param_id);
    assert(idx >= 0 && "test names a param id SAFETY_CFG_PARAM_TABLE does not carry");
    return (size_t)idx;
}

#define IDX_OVERSHOOT_TIME_S idx_of(0x0203)
#define IDX_RATE_WINDOW_S    idx_of(0x0205)

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void reset_all(void)
{
    stub_reset();
    reset_to_defaults();
    fake_time_reset_all();
    s_fetched_at_us = -1;
    s_dirty = false; /* 2026-08-23 fix -- a prior test's unflushed write must not bleed into the next */
    fake_kv_set_write_safe_here(true); /* default: called from a normal, internal-RAM stack */
    fake_kv_reset_all(); /* every test except the version-refuse one runs without a real partition up,
                           * so hal_kv_open() fails closed with HAL_NOT_READY -- same "every test except
                           * the version-refuse one runs without real NVS" default nvs_test_enable(false)
                           * used to give */
    s_test_semaphore_take_result = pdTRUE; /* default: lock available, same as every real single-owner take */
}

static void test_index_for_id_finds_known_and_rejects_unknown(void)
{
    TEST_SECTION("index_for_id -- every table row is reachable by its own id, an unknown id is not");

    // 0x0101 is the table's first row (tc_source); 0x0322 (zone_ct_channel[2],
    // docs/CT_CHANNEL_MASK_PLAN.md step 2) is now its last, appended at the very
    // end per this table's own "only ever appended to" rule -- 0x0321
    // (zone_ct_channel[1]) is now second-to-last. Both ends, not just one, so a
    // future off-by-one in the table's bounds shows up here.
    //
    // MAINTENANCE: appending a row to SAFETY_CFG_PARAM_TABLE REQUIRES updating
    // the two ids below. That is deliberate -- the whole point of pinning the
    // tail is that it cannot be satisfied by anything read back out of the
    // table itself, so an append has to be acknowledged by a human here. It
    // has been missed twice already: 3b5ced00 appended estop_active_level and
    // left these two checks pinned to tc_offset_c, and the zone_ct_channel[0..2]
    // append left them pinned to estop_active_level. Both times the append was
    // correct and only this pin was stale -- which is the check doing its job.
    // Note that tools/run_all_checks.ps1 does NOT run this executable, so a
    // stale pin here is only visible to a direct build_host_tests.ps1 run.
    TEST_CHECK(index_for_id(0x0101) == 0, "first table row (tc_source) is index 0");
    TEST_CHECK(index_for_id(0x0322) == (int)(SAFETY_CFG_PARAM_COUNT - 1),
               "last table row (zone_ct_channel[2]) is the last index");
    TEST_CHECK(index_for_id(0x0321) == (int)(SAFETY_CFG_PARAM_COUNT - 2),
               "zone_ct_channel[1] is second-to-last");
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
    uint16_t ids2[] = { 0x0203 }; // overshoot_time_s (U16)
    uint16_t vals2[] = { 42 };
    stage_page(0, false, ids2, vals2, 1);

    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    fake_time_advance_us(5000000); // 5s, arbitrary -- clock is at 0 right after reset_all()

    bool refetched = safety_cfg_store_maybe_refetch(&fake_link, 0x0002);

    TEST_CHECK(refetched == true, "a differing CRC causes a real refetch");
    TEST_CHECK(s_stub_get_config_page_calls == 1, "exactly one page was requested (single stub page, more=0)");
    TEST_CHECK(safety_cfg_store_cached_crc() == 0x0002, "cached_crc now matches the fetched live CRC");

    safety_cfg_param_t p;
    TEST_CHECK(safety_cfg_store_get_by_index(IDX_OVERSHOOT_TIME_S, &p) && p.set && p.value.u16_val == 42,
               "overshoot_time_s picked up the fetched value");
    TEST_CHECK(safety_cfg_store_get_by_index(0, &p) && !p.set,
               "tc_source (index 0), NOT present in the fetched page, reads back as unset -- "
               "the whole cache was replaced, not merged with the stale entry that used to be there");
}

// 2026-08-28 audit fix (N2, BLOCKER): safety_poll_task's own entry point
// (safety_cfg_store_maybe_refetch() -> safety_cfg_store_refetch_nonblocking())
// must NEVER block behind the httpd worker's confirm_commit_landed() call
// (safety_cfg_store_refetch(), portMAX_DELAY) -- that call can legitimately
// hold s_store_lock for up to SAFETY_CFG_STORE_REFETCH_BUDGET_MS (2s) plus a
// synchronous NVS flush, and blocking safety_poll_task behind it stacks on
// top of that task's own already-tight ~3.2-3.7s worst case, risking a
// link_timeout_s nuisance trip mid-firing. Simulated here by making the
// test's xSemaphoreTake stand-in report "busy" (pdFALSE), the same signal a
// real mutex gives when another task holds it: the poll-side call must bail
// out immediately -- no page request at all -- rather than wait.
static void test_maybe_refetch_does_not_block_when_lock_is_busy(void)
{
    TEST_SECTION("safety_cfg_store_maybe_refetch -- lock held elsewhere (httpd worker mid-refetch) -- "
                 "bails out immediately, touches the wire NOT AT ALL (N2 fix)");
    reset_all();

    s_store.config_crc = 0x0001; // differs from live -- would normally trigger a fetch
    uint16_t ids[] = { 0x0203 };
    uint16_t vals[] = { 99 };
    stage_page(0, false, ids, vals, 1);

    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));

    s_test_semaphore_take_result = pdFALSE; // "the httpd worker holds s_store_lock right now"
    bool refetched = safety_cfg_store_maybe_refetch(&fake_link, 0x0002);

    TEST_CHECK(refetched == false, "lock busy -- maybe_refetch reports nothing changed, does not wait for it");
    TEST_CHECK(s_stub_get_config_page_calls == 0,
               "no page request went out at all -- safety_poll_task must not block on a busy lock");
    TEST_CHECK(safety_cfg_store_cached_crc() == 0x0001, "cache left exactly as it was");

    // RED-then-GREEN companion: the same call with the lock available (the
    // s_retry_not_before_us backoff from the failed attempt above would
    // normally suppress an immediate retry, so re-seed retry state as if
    // this is a fresh attempt, same as reset_all() would give a first call).
    s_retry_not_before_us = 0;
    s_test_semaphore_take_result = pdTRUE; // lock now available -- same CRC mismatch as before
    refetched = safety_cfg_store_maybe_refetch(&fake_link, 0x0002);
    TEST_CHECK(refetched == true, "same request, lock now available -- fetch actually happens");
    TEST_CHECK(s_stub_get_config_page_calls == 1, "exactly one page requested once the lock was free");
}

// 2026-08-27 audit fix (commissioning-write defect d), "ok cannot fail":
// safety_cfg_store_refetch() used to write `scratch.entries[idx].set = 1`
// UNCONDITIONALLY for every entry a CONFIG_PAGE reply carried -- regardless
// of whether the PICO considered that field set. This is the exact live-
// bench defect: abs_max_temp_c UNSET on the Pico (S1's overtemperature
// ceiling never commissioned) still showed up on the ESP's cache -- and from
// there, the operator-facing GET /api/safety/commissioning JSON -- as
// "{set:true, value:0}", and 0 on that specific field means the guard NEVER
// TRIPS. Proves the fix: the cache now carries the Pico's OWN per-entry
// answer (kilnlink_config_page_entry_t::set, decoded off KILNLINK_CONFIG_
// PAGE_UNSET_BIT) through unchanged.
static void test_refetch_carries_unset_bit_through_not_unconditional_true(void)
{
    TEST_SECTION("safety_cfg_store_refetch -- an UNSET entry from the Pico stays UNSET in the cache, "
                 "never promoted to set=true (defect d, 2026-08-27 audit)");
    reset_all();

    uint16_t ids[] = { 0x0203, 0x0205 }; // overshoot_time_s, rate_window_s
    uint16_t vals[] = { 42, 99 };
    // index 0 of THIS page (0x0203/overshoot_time_s) is the Pico's answer for
    // an UNSET no-safe-default field in this test -- the numeric value (42)
    // is a placeholder exactly like abs_max_temp_c==0.0 on the real bench;
    // what matters is the bit, not the number, per the audit's own framing.
    stage_page_with_one_unset(0, false, ids, vals, 2, 0);

    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));

    bool ok = safety_cfg_store_refetch(&fake_link, 0x0099);
    TEST_CHECK(ok == true, "the refetch itself succeeds -- an unset entry is not a decode error");

    safety_cfg_param_t p;
    TEST_CHECK(safety_cfg_store_get_by_index(IDX_OVERSHOOT_TIME_S, &p) && p.set == false,
               "the entry the Pico reported UNSET reads back set=false from the ESP cache -- "
               "NOT unconditionally promoted to true the way it used to be");
    TEST_CHECK(safety_cfg_store_get_by_index(IDX_RATE_WINDOW_S, &p) && p.set == true && p.value.u16_val == 99,
               "the OTHER entry on the same page, which the Pico DID report set, is unaffected -- "
               "the bit is per-entry, not page-wide");
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
    TEST_CHECK(safety_cfg_store_get_by_index(IDX_OVERSHOOT_TIME_S, &p) && p.set && p.value.u16_val == 100,
               "page 0's entry landed");
    TEST_CHECK(safety_cfg_store_get_by_index(IDX_RATE_WINDOW_S, &p) && p.set && p.value.u16_val == 200,
               "page 1's entry landed too (rate_window_s, 0x0205)");
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
    TEST_CHECK(safety_cfg_store_get_by_index(IDX_OVERSHOOT_TIME_S, &p) && p.set && p.value.u16_val == 7,
               "the recognised id in the same page still landed");
}

static void test_refetch_failure_leaves_cache_untouched(void)
{
    TEST_SECTION("safety_cfg_store_refetch -- a failed page request leaves the PREVIOUS cache intact");
    reset_all();

    // Seed a "previous, good" cache.
    s_store.config_crc = 0x00AA;
    s_store.entries[IDX_OVERSHOOT_TIME_S].set = 1;
    s_store.entries[IDX_OVERSHOOT_TIME_S].value.u16_val = 111;

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
    TEST_CHECK(safety_cfg_store_get_by_index(IDX_OVERSHOOT_TIME_S, &p) && p.set && p.value.u16_val == 111,
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
    hal_kv_init_partition(KILN_NVS_PARTITION); // this test alone needs a real hal_kv round trip

    // Simulate what would have been persisted by a hypothetical v2: same
    // struct shape here (no v2 exists yet), but version byte bumped past
    // what this build knows -- nvs_load_store() must refuse it wholesale
    // rather than trust the old-version bytes underneath. Written directly
    // into the fake_kv store (not via nvs_save_store(), which always
    // stamps the CURRENT version before writing -- exactly the behavior a
    // real save path should have, but it means it cannot be used to
    // manufacture a newer-than-current blob for this test).
    safety_cfg_store_blob_t fake_newer;
    memset(&fake_newer, 0, sizeof(fake_newer));
    fake_newer.version = SAFETY_CFG_STORE_VERSION + 1;
    fake_newer.entries[0].set = 1;
    fake_newer.entries[0].value.u8_val = 3;
    {
        hal_kv_handle_t h;
        TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
                   "setup: stage handle opens");
        TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_SAFETY_CFG, &fake_newer, sizeof(fake_newer)) == HAL_OK,
                   "setup: newer-version blob stages");
        TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "setup: stage commits");
        hal_kv_close(&h);
    }

    nvs_load_store(); // should refuse and reset to empty defaults

    TEST_CHECK(s_store.config_crc == 0, "a refused (newer) blob leaves the cache at its empty default");
    safety_cfg_param_t p;
    TEST_CHECK(safety_cfg_store_get_by_index(0, &p) && !p.set,
               "the newer blob's data was NOT loaded -- refused wholesale, per COMMISSIONING.md "
               "sec 1's own reasoning for the identical rule on the Pico's config_store");

    fake_kv_reset_all(); // leave the shared fake state as every other test in this binary expects
}

// ---------------------------------------------------------------------------
// 2026-08-27 audit fix (defect c): safety_cfg_store_init() used to stamp
// s_fetched_at_us = esp_timer_get_time() whenever the blob it loaded from NVS
// had config_crc != 0 -- treating a load off flash as if it were a live fetch
// that just happened. On a board that boots with a real cache already on
// flash (the ordinary case after the first commissioning), that made
// safety_cfg_store_fetched_ms_ago() report ~0 ms -- i.e. board uptime, not
// fetch age -- for values that might be a stale image days old, off a Pico
// that has since been reflashed or recommissioned. This test proves the fix:
// a cache loaded from NVS at boot must report fetched_ms_ago() == UINT32_MAX
// ("never fetched THIS boot") until a real safety_cfg_store_refetch() runs,
// even though its VALUES are already being served.
// ---------------------------------------------------------------------------

static void test_init_does_not_stamp_fetch_time_for_an_nvs_loaded_cache(void)
{
    TEST_SECTION("safety_cfg_store_init -- loading a real cache off NVS at boot must NOT "
                 "count as \"just fetched\" (defect c, 2026-08-27 audit)");
    reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);

    // Persist a real, current-version, already-fetched cache -- exactly what
    // a board that was commissioned on a PREVIOUS boot leaves on flash.
    s_store.config_crc = 0xBEEF;
    s_store.entries[0].set = 1;
    s_store.entries[0].value.u8_val = 3;
    TEST_CHECK(nvs_save_store() == ESP_OK, "setup: the current-version cache saves successfully");

    // Simulate a fresh boot: in-RAM state reset, clock at 0, nothing fetched
    // yet this boot -- then safety_cfg_store_init() is the ONLY thing under
    // test, exactly as it runs during real firmware startup.
    memset(&s_store, 0, sizeof(s_store));
    s_fetched_at_us = -1;
    fake_time_advance_us(5000ull * 1000ull); // clock has been running 5s since "boot" (still at 0 from reset_all())

    TEST_CHECK(safety_cfg_store_init() == ESP_OK, "init succeeds");
    TEST_CHECK(s_store.config_crc == 0xBEEF, "the persisted cache WAS loaded -- values are being served");
    safety_cfg_param_t p;
    TEST_CHECK(safety_cfg_store_get_by_index(0, &p) && p.set && p.value.u8_val == 3,
               "the loaded value is real and readable");
    TEST_CHECK(safety_cfg_store_fetched_ms_ago() == UINT32_MAX,
               "but its age is honestly UNKNOWN this boot -- UINT32_MAX (\"never\"/null), "
               "NOT ~0 ms manufactured from an NVS load that never talked to the Pico");

    fake_kv_reset_all();
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
    s_store.entries[IDX_OVERSHOOT_TIME_S].set = 1;
    s_store.entries[IDX_OVERSHOOT_TIME_S].value.u16_val = 111;

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
    TEST_CHECK(safety_cfg_store_get_by_index(IDX_OVERSHOOT_TIME_S, &p) && p.set && p.value.u16_val == 111,
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
    hal_kv_init_partition(KILN_NVS_PARTITION); // this test checks the flush actually SUCCEEDED
                            // (s_dirty cleared), so it needs a real hal_kv round trip, not the
                            // "every open fails closed (partition never initialized)" default every
                            // other test in this file relies on.

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

    fake_kv_reset_all(); // leave the shared fake state as every other test in this binary expects
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
    hal_kv_init_partition(KILN_NVS_PARTITION); // the RETRY flush below needs to actually succeed to prove s_dirty clears

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

    fake_kv_reset_all(); // leave the shared fake state as every other test in this binary expects
}

static void test_nvs_save_store_refuses_when_calling_stack_is_external_ram(void)
{
    TEST_SECTION("nvs_save_store -- refuses (does not crash) when called with a PSRAM stack underneath it");
    reset_all();

    fake_kv_set_write_safe_here(false); // simulate being called from a PSRAM-stacked task

    esp_err_t err = nvs_save_store();

    TEST_CHECK(err == ESP_ERR_INVALID_STATE,
               "the wrong-task guard refuses with a diagnosable error, not a crash, exactly the "
               "class of bug (an NVS write reached from a PSRAM-stack task) this whole fix closes");

    fake_kv_set_write_safe_here(true); // leave shared fake state as every other test expects
}

static void test_nvs_save_store_proceeds_normally_on_an_internal_ram_stack(void)
{
    TEST_SECTION("nvs_save_store -- proceeds normally when the calling task's stack is internal RAM");
    reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION); // exercise a real hal_kv round trip for this one

    // fake_kv_set_write_safe_here(true) is reset_all()'s implicit state
    // (nothing here has set it false).
    s_store.config_crc = 0x9999;
    esp_err_t err = nvs_save_store();

    TEST_CHECK(err == ESP_OK, "the guard does not fire on an internal-RAM stack -- the write proceeds "
                              "and succeeds exactly as it always did");

    fake_kv_reset_all(); // leave the shared fake state as every other test in this binary expects
}

// 2026-08-29 fix. SAFETY_CFG_PARAM_TABLE's row order IS the on-flash layout
// (entries[] is indexed by table position), so an id inserted anywhere but the
// very bottom shifts every row below it and remaps each already-persisted
// value onto the wrong field. Commit babbfdd did exactly that with
// ct_installed (0x0109) at the end of sec 1 and left SAFETY_CFG_STORE_VERSION
// at 1. Bumping it to 2 is what makes nvs_load_store() refuse the old layout
// for its real reason rather than incidentally (that commit also changed the
// blob's SIZE, which the length check happened to catch -- an insertion that
// replaced a row instead of adding one would not have been caught at all).
// This must be able to FAIL: against VERSION==1 the v1 blob below is
// "current version, right size" and loads, and the last two checks go red.
static void test_version_refuse_older_layout_without_a_migration(void)
{
    TEST_SECTION("nvs_load_store -- a blob from the PRE-ct_installed table layout is refused, "
                 "not loaded one slot out of register");
    reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);

    // A v1 blob, hand-built: same struct shape (only one layout has ever
    // shipped), version byte stamped 1, carrying a value in the slot that
    // WAS overshoot_time_s before 0x0109 pushed the whole tail down by one.
    safety_cfg_store_blob_t v1;
    memset(&v1, 0, sizeof(v1));
    v1.version = 1u;
    v1.config_crc = 0x4321;
    size_t stale_slot = IDX_OVERSHOOT_TIME_S - 1; // where v1 kept overshoot_time_s
    v1.entries[stale_slot].set = 1;
    v1.entries[stale_slot].value.u16_val = 321;
    {
        hal_kv_handle_t h;
        TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
                   "setup: stage handle opens");
        TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_SAFETY_CFG, &v1, sizeof(v1)) == HAL_OK,
                   "setup: v1 blob stages");
        TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "setup: stage commits");
        hal_kv_close(&h);
    }

    TEST_CHECK(SAFETY_CFG_STORE_VERSION > 1u,
               "the table-order change that moved overshoot_time_s bumped the on-flash version -- "
               "without this, the blob below is indistinguishable from a current one");

    nvs_load_store();

    TEST_CHECK(s_store.config_crc == 0,
               "the pre-insert blob was refused wholesale -- an empty cache, refilled by the next "
               "refetch from the Pico, beats a full one whose every sec 2-5 value names the wrong field");
    safety_cfg_param_t p;
    TEST_CHECK(safety_cfg_store_get_by_index(stale_slot, &p) && !p.set,
               "nothing from the old layout leaked in at its old slot either");

    fake_kv_reset_all();
}

// ---------------------------------------------------------------------------
// RELAY_LIFE_BUDGET.md -- the safety relay (K4) type.
// ---------------------------------------------------------------------------

static void test_safety_relay_type_defaults_to_contactor_and_pushes_to_relay_cycles(void)
{
    TEST_SECTION("safety_cfg_store_init -- no persisted safety relay type yet: defaults to "
                 "RELAY_TYPE_CONTACTOR and pushes it into relay_cycles.c immediately, every "
                 "boot (not only after a fresh POST)");

    fake_kv_reset_all();
    stub_reset();
    esp_err_t err = safety_cfg_store_init();
    TEST_CHECK(err == ESP_OK, "init succeeds with no relay-type blob on flash");
    TEST_CHECK(safety_cfg_store_get_safety_relay_type() == RELAY_TYPE_CONTACTOR,
               "first boot -- no blob -- defaults to contactor, never ssr");
    TEST_CHECK(s_stub_relay_cycles_set_type_calls >= 1,
               "safety_cfg_store_init() calls relay_cycles_set_type() so the budget calc is "
               "correct from the very first dashboard/LCD read, not only after a commissioning POST");
    TEST_CHECK(s_stub_relay_cycles_last_relay == RELAY_CYCLES_SAFETY_INDEX,
               "the call targets the safety relay's own slot, not a heater relay's");
    TEST_CHECK(s_stub_relay_cycles_last_type == RELAY_TYPE_CONTACTOR, "and reports the default type");

    fake_kv_reset_all();
}

static void test_safety_relay_type_set_persists_and_roundtrips_after_reload(void)
{
    TEST_SECTION("safety_cfg_store_set_safety_relay_type -- mercury sticks in RAM immediately and "
                 "survives a fresh safety_cfg_store_init() (a reboot)");

    fake_kv_reset_all();
    stub_reset();
    TEST_CHECK(safety_cfg_store_init() == ESP_OK, "setup: first boot");

    TEST_CHECK(safety_cfg_store_set_safety_relay_type(RELAY_TYPE_MERCURY, NULL) == true,
               "mercury is accepted -- the safety relay may be a contactor or a mercury relay");
    TEST_CHECK(safety_cfg_store_get_safety_relay_type() == RELAY_TYPE_MERCURY,
               "live value updates immediately, before any reboot");
    TEST_CHECK(s_stub_relay_cycles_last_type == RELAY_TYPE_MERCURY,
               "relay_cycles_set_type() is called again on every successful set, not only at boot");

    // Simulate a reboot: a fresh safety_cfg_store_init() must load mercury
    // back off NVS, not silently fall back to the contactor default.
    stub_reset();
    TEST_CHECK(safety_cfg_store_init() == ESP_OK, "reload succeeds");
    TEST_CHECK(safety_cfg_store_get_safety_relay_type() == RELAY_TYPE_MERCURY,
               "the persisted type survives a reboot -- this is the whole point of the NVS write");
    TEST_CHECK(s_stub_relay_cycles_last_type == RELAY_TYPE_MERCURY,
               "the reloaded value is pushed into relay_cycles.c again on this boot too");

    fake_kv_reset_all();
}

static void test_safety_relay_type_rejects_ssr(void)
{
    TEST_SECTION("safety_cfg_store_set_safety_relay_type -- RELAY_TYPE_SSR is REFUSED: the "
                 "safety relay never offers ssr (RELAY_LIFE_BUDGET.md's Request section) -- "
                 "this is the second line of defense behind the HTTP POST validator");

    fake_kv_reset_all();
    stub_reset();
    TEST_CHECK(safety_cfg_store_init() == ESP_OK, "setup: first boot, default contactor");
    int calls_before = s_stub_relay_cycles_set_type_calls;

    TEST_CHECK(safety_cfg_store_set_safety_relay_type(RELAY_TYPE_SSR, NULL) == false,
               "ssr is refused -- returns false, nothing changed");
    TEST_CHECK(safety_cfg_store_get_safety_relay_type() == RELAY_TYPE_CONTACTOR,
               "the stored type is untouched by the refused call");
    TEST_CHECK(s_stub_relay_cycles_set_type_calls == calls_before,
               "a refused set never reaches relay_cycles_set_type() at all");

    // NEGATIVE TEST (proves the check above can actually fail, per this
    // codebase's "negative-test every check" rule): an out-of-range value
    // that is neither a real relay_type_t member nor RELAY_TYPE_SSR must
    // also be refused -- if safety_cfg_store_set_safety_relay_type() were
    // accidentally written as "accept anything except literal 0", this
    // would catch it.
    TEST_CHECK(safety_cfg_store_set_safety_relay_type((relay_type_t)99, NULL) == false,
               "an unrecognised value is refused the same way ssr is -- 'anything but ssr' would "
               "be the wrong rule");

    fake_kv_reset_all();
}

// 2026-09-06 audit fix: an NVS write failure (including the PSRAM-stack
// refusal above) used to be only ESP_LOGE'd while the function still
// returned true, so an HTTP caller would report {"ok":true} for a value
// that was applied live but never actually persisted. out_nvs_err must
// surface that distinctly.
static void test_safety_relay_type_reports_nvs_failure_via_out_param(void)
{
    TEST_SECTION("safety_cfg_store_set_safety_relay_type -- out_nvs_err surfaces an NVS write "
                 "failure distinctly from the (unchanged) true/false return value");

    fake_kv_reset_all();
    stub_reset();
    TEST_CHECK(safety_cfg_store_init() == ESP_OK, "setup: first boot");

    fake_kv_set_write_safe_here(false); // simulate the PSRAM-stack refusal -- one concrete NVS failure

    esp_err_t nvs_err = ESP_OK;
    bool accepted = safety_cfg_store_set_safety_relay_type(RELAY_TYPE_MERCURY, &nvs_err);

    TEST_CHECK(accepted == true,
               "the type is still valid and applied live -- return value's meaning is unchanged");
    TEST_CHECK(safety_cfg_store_get_safety_relay_type() == RELAY_TYPE_MERCURY,
               "applied live even though it will not survive a reboot");
    TEST_CHECK(nvs_err != ESP_OK,
               "out_nvs_err reports the write failure -- THIS is the fix: before it, a caller had "
               "no way to distinguish this from a fully-persisted success");

    fake_kv_set_write_safe_here(true);
    fake_kv_reset_all();
}

// ---------------------------------------------------------------------------
// CT_COMMISSIONING_PLAN.md step 1 -- safety_ct_cal_convert() /
// safety_cfg_store_set/get_ct_cal_input().
// ---------------------------------------------------------------------------

static void test_ct_cal_convert_at_several_probe_ratings(void)
{
    TEST_SECTION("safety_ct_cal_convert -- k_ct_v_per_a = 1/A_fs and zero_counts = "
                 "zero_mv/1000 * gain * 4096/3.3, at several real probe ratings (quantized counts, "
                 "not idealized amps -- 'idealized test input' class)");

    float k = 0.0f;
    uint16_t zc = 0;

    // 1 A bench probe, +59 mV offset (project's actual bench probe, per
    // MEMORY.md's "CT sensor on GPIO28" note), gain 0.715 (R46/R43 default).
    TEST_CHECK(safety_ct_cal_convert(1.0f, 59.0f, 0.715f, &k, &zc), "1A probe converts");
    TEST_CHECK(fabsf(k - 1.0f) < 1e-6f, "k_ct_v_per_a = 1/1 = 1.0 V/A");
    // 0.059 * 0.715 * 4096/3.3 = 52.35... -> rounds to 52.
    TEST_CHECK(zc == 52, "zero_counts quantizes to 52 counts, not a fractional value");

    // 20 A probe, 0 mV offset.
    TEST_CHECK(safety_ct_cal_convert(20.0f, 0.0f, 0.715f, &k, &zc), "20A probe converts");
    TEST_CHECK(fabsf(k - 0.05f) < 1e-6f, "k_ct_v_per_a = 1/20 = 0.05 V/A");
    TEST_CHECK(zc == 0, "zero_mv=0 quantizes to exactly 0 counts");

    // 50 A probe, -30 mV offset (negative zero_mv is legal per the sanity range).
    TEST_CHECK(safety_ct_cal_convert(50.0f, -30.0f, 0.715f, &k, &zc), "50A probe converts");
    TEST_CHECK(fabsf(k - 0.02f) < 1e-6f, "k_ct_v_per_a = 1/50 = 0.02 V/A");
    TEST_CHECK(zc == 0, "a negative implied offset clamps to the ADC's honest floor of 0 counts, "
                        "never wraps a negative float into a huge unsigned value");

    // 100 A probe, +100 mV offset, a non-default gain (0.5, as if R46/R43
    // were refined per the commissioning page's own note).
    TEST_CHECK(safety_ct_cal_convert(100.0f, 100.0f, 0.5f, &k, &zc), "100A probe converts");
    TEST_CHECK(fabsf(k - 0.01f) < 1e-6f, "k_ct_v_per_a = 1/100 = 0.01 V/A");
    // 0.1 * 0.5 * 4096/3.3 = 62.06... -> 62.
    TEST_CHECK(zc == 62, "zero_counts scales with gain too, not just zero_mv");
}

static void test_ct_cal_convert_rejects_out_of_range(void)
{
    TEST_SECTION("safety_ct_cal_convert -- sanity ranges only, but real ones: 'nothing may "
                 "assume 1 A' means A_fs in [0.1, 2000], zero_mv in [-200, 200]");

    float k = 0.0f;
    uint16_t zc = 0;

    TEST_CHECK(safety_ct_cal_convert(0.05f, 0.0f, 0.715f, &k, &zc) == false,
               "A_fs below 0.1A is refused");
    TEST_CHECK(safety_ct_cal_convert(2001.0f, 0.0f, 0.715f, &k, &zc) == false,
               "A_fs above 2000A is refused -- a real kiln's 10-100A probe must never be treated "
               "as an edge case, but this is still a sanity ceiling");
    TEST_CHECK(safety_ct_cal_convert(1.0f, -201.0f, 0.715f, &k, &zc) == false,
               "zero_mv below -200 is refused");
    TEST_CHECK(safety_ct_cal_convert(1.0f, 201.0f, 0.715f, &k, &zc) == false,
               "zero_mv above 200 is refused");
    TEST_CHECK(safety_ct_cal_convert(1.0f, 0.0f, 0.0f, &k, &zc) == false,
               "a zero or negative gain is refused rather than dividing/producing garbage");
    TEST_CHECK(safety_ct_cal_convert(NAN, 0.0f, 0.715f, &k, &zc) == false, "non-finite A_fs is refused");

    // Boundary values PASS (the range is inclusive at both ends) -- this is
    // the NEGATIVE TEST half of this check: if safety_ct_cal_convert() had
    // been written with a strict `<`/`>` where the header comment promises
    // an inclusive [min, max] (or vice-versa), one of these two would flip
    // and this assertion would fail. Proves the boundary constants
    // themselves are load-bearing, not just "some number in the right
    // ballpark".
    TEST_CHECK(safety_ct_cal_convert(SAFETY_CT_CAL_A_FS_MIN, 0.0f, 0.715f, &k, &zc),
               "A_fs exactly at the minimum (0.1A) is accepted, not rejected");
    TEST_CHECK(safety_ct_cal_convert(SAFETY_CT_CAL_A_FS_MAX, 0.0f, 0.715f, &k, &zc),
               "A_fs exactly at the maximum (2000A) is accepted, not rejected");
}

static void test_ct_cal_manual_wins_over_sweep(void)
{
    TEST_SECTION("safety_cfg_store_set_ct_cal_input -- CT_COMMISSIONING_PLAN.md step 1: manual "
                 "wins over the sweep, the sweep must not overwrite a manual value");

    fake_kv_reset_all();
    stub_reset();
    TEST_CHECK(safety_cfg_store_init() == ESP_OK, "setup");

    float k = 0.0f;
    uint16_t zc = 0;
    TEST_CHECK(safety_cfg_store_set_ct_cal_input(0, 1.0f, 59.0f, SAFETY_CT_CAL_SOURCE_MANUAL, &k, &zc, NULL),
               "operator hand-enters channel 0's calibration");
    TEST_CHECK(fabsf(k - 1.0f) < 1e-6f, "converts using the gain default (0.715) since none is "
                                        "committed from the Pico in this test");

    // The sweep's own write attempt must be refused outright -- nothing
    // changes.
    TEST_CHECK(safety_cfg_store_set_ct_cal_input(0, 20.0f, 0.0f, SAFETY_CT_CAL_SOURCE_SWEEP, &k, &zc, NULL) ==
                   false,
               "a SWEEP-sourced write against a MANUAL channel is refused, not silently accepted");

    float a_fs = 0.0f, zero_mv = 0.0f;
    safety_ct_cal_source_t src = SAFETY_CT_CAL_SOURCE_SWEEP;
    TEST_CHECK(safety_cfg_store_get_ct_cal_input(0, &a_fs, &zero_mv, &src), "channel 0 still has a value");
    TEST_CHECK(fabsf(a_fs - 1.0f) < 1e-6f, "the manual A_fs (1.0) was NOT overwritten by the "
                                           "refused sweep write (which tried 20.0)");
    TEST_CHECK(src == SAFETY_CT_CAL_SOURCE_MANUAL, "source stays manual");

    // A channel that has NEVER been set (still sweep-eligible) accepts a
    // sweep write normally -- manual wins over the sweep, but the sweep is
    // not disabled everywhere.
    TEST_CHECK(safety_cfg_store_set_ct_cal_input(1, 20.0f, 0.0f, SAFETY_CT_CAL_SOURCE_SWEEP, &k, &zc, NULL),
               "a sweep write against an UNCOMMISSIONED channel succeeds");
    TEST_CHECK(safety_cfg_store_get_ct_cal_input(1, &a_fs, &zero_mv, &src), "channel 1 now has a value");
    TEST_CHECK(src == SAFETY_CT_CAL_SOURCE_SWEEP, "and its source is sweep, not manual");

    // AUTO_ZERO always applies, even over an existing manual value --
    // see safety_ct_cal_source_t's own header comment for why this is NOT
    // the same rule as the sweep's.
    TEST_CHECK(safety_cfg_store_set_ct_cal_input(0, 1.0f, 10.0f, SAFETY_CT_CAL_SOURCE_AUTO_ZERO, &k, &zc, NULL),
               "an auto-zero write against a MANUAL channel is applied, unlike the sweep's");
    TEST_CHECK(safety_cfg_store_get_ct_cal_input(0, &a_fs, &zero_mv, &src), "channel 0 still has a value");
    TEST_CHECK(fabsf(zero_mv - 10.0f) < 1e-6f, "zero_mv was updated by the auto-zero action");
    TEST_CHECK(src == SAFETY_CT_CAL_SOURCE_AUTO_ZERO, "source is now auto-zero");

    // Reload from NVS -- the calibration input and its source must survive
    // a reboot, same discipline as the relay type above.
    stub_reset();
    TEST_CHECK(safety_cfg_store_init() == ESP_OK, "reload succeeds");
    TEST_CHECK(safety_cfg_store_get_ct_cal_input(0, &a_fs, &zero_mv, &src), "channel 0 survives reload");
    TEST_CHECK(src == SAFETY_CT_CAL_SOURCE_AUTO_ZERO, "and its source survives too");
    TEST_CHECK(safety_cfg_store_get_ct_cal_input(2, NULL, NULL, NULL) == false,
               "channel 2, never set, correctly reports false -- not a fabricated zero");

    fake_kv_reset_all();
}

static void test_ct_cal_set_rejects_out_of_range_channel_and_value(void)
{
    TEST_SECTION("safety_cfg_store_set_ct_cal_input -- rejects an out-of-range channel or value, "
                 "same ranges safety_ct_cal_convert() enforces");

    fake_kv_reset_all();
    stub_reset();
    TEST_CHECK(safety_cfg_store_init() == ESP_OK, "setup");

    float k = 0.0f;
    uint16_t zc = 0;
    TEST_CHECK(safety_cfg_store_set_ct_cal_input(3, 1.0f, 0.0f, SAFETY_CT_CAL_SOURCE_MANUAL, &k, &zc, NULL) ==
                   false,
               "channel 3 does not exist (only 0..2) -- refused");
    TEST_CHECK(safety_cfg_store_set_ct_cal_input(0, 5000.0f, 0.0f, SAFETY_CT_CAL_SOURCE_MANUAL, &k, &zc, NULL) ==
                   false,
               "A_fs out of range is refused here too, not only in the pure convert function");
    TEST_CHECK(safety_cfg_store_get_ct_cal_input(0, NULL, NULL, NULL) == false,
               "the refused call left channel 0 unset");

    fake_kv_reset_all();
}

// 2026-09-06 audit fix, same as test_safety_relay_type_reports_nvs_failure_
// via_out_param() above.
static void test_ct_cal_set_reports_nvs_failure_via_out_param(void)
{
    TEST_SECTION("safety_cfg_store_set_ct_cal_input -- out_nvs_err surfaces an NVS write failure "
                 "distinctly from the (unchanged) true/false return value");

    fake_kv_reset_all();
    stub_reset();
    TEST_CHECK(safety_cfg_store_init() == ESP_OK, "setup");

    fake_kv_set_write_safe_here(false); // simulate the PSRAM-stack refusal -- one concrete NVS failure

    float k = 0.0f;
    uint16_t zc = 0;
    esp_err_t nvs_err = ESP_OK;
    bool accepted = safety_cfg_store_set_ct_cal_input(0, 1.0f, 59.0f, SAFETY_CT_CAL_SOURCE_MANUAL, &k,
                                                       &zc, &nvs_err);

    TEST_CHECK(accepted == true, "a valid input is still accepted -- return value's meaning is unchanged");
    float a_fs = 0.0f;
    TEST_CHECK(safety_cfg_store_get_ct_cal_input(0, &a_fs, NULL, NULL) == true && a_fs == 1.0f,
               "applied live even though it will not survive a reboot");
    TEST_CHECK(nvs_err != ESP_OK,
               "out_nvs_err reports the write failure -- THIS is the fix: before it, a caller had "
               "no way to distinguish this from a fully-persisted success");

    fake_kv_set_write_safe_here(true);
    fake_kv_reset_all();
}

// 2026-09-15 review (review_divergence_fixes_b2e7017f_2026-09-15.md, LOW 9/
// LOW 10): s_cache_stale used to be cleared only inside maybe_refetch()'s own
// CRC-match/success branches, so a caller that reaches a successful refetch
// via safety_cfg_store_refetch() directly (e.g. confirm_commit_landed() in
// safety_cfg_http.c, which never goes through maybe_refetch() at all) could
// leave the stale flag set even though the cache is now fresh -- a transient
// false "stale" warning right after a commissioning commit. The fix moved
// the clear into safety_cfg_store_refetch_locked()'s single success choke
// point, which every entry point (maybe_refetch, refetch, refetch_
// nonblocking) funnels through. This test drives that choke point directly,
// bypassing maybe_refetch() entirely, and also checks the MEDIUM 3 generation
// counter bumps alongside it (both are set at the same choke point).
static void test_cache_stale_cleared_by_direct_refetch_not_only_maybe_refetch(void)
{
    TEST_SECTION("s_cache_stale/generation set/clear lifecycle -- direct refetch() must clear "
                 "staleness too, not only maybe_refetch()'s own CRC-match branch");
    reset_all();

    TEST_CHECK(!safety_cfg_store_cache_is_stale(), "freshly reset cache starts NOT stale");
    uint32_t gen_before = safety_cfg_store_cache_generation();

    // Manually force the stale flag on, simulating the state left behind by
    // an in-flight mismatch that maybe_refetch() had already flagged (line
    // "s_cache_stale = true" above) before this direct, non-maybe_refetch
    // caller runs its own successful refetch.
    s_cache_stale = true;

    uint16_t ids[] = { 0x0203 }; // overshoot_time_s (U16)
    uint16_t vals[] = { 77 };
    stage_page(0, false, ids, vals, 1);

    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    bool ok = safety_cfg_store_refetch(&fake_link, 0x00AA);

    TEST_CHECK(ok, "the direct refetch() call itself succeeds");
    TEST_CHECK(!safety_cfg_store_cache_is_stale(),
               "LOW 9/10: a successful refetch() -- NOT routed through maybe_refetch() -- still "
               "clears the stale flag, because the clear lives at refetch_locked()'s single "
               "success choke point, not duplicated (and easily missed) in every caller");
    TEST_CHECK(safety_cfg_store_cache_generation() == gen_before + 1,
               "the generation counter bumps exactly once per successful refetch, regardless of "
               "which entry point reached it");
}

// ---------------------------------------------------------------------------
// docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md -- safety_ct_cal_blob_t v1 -> v2.
// ---------------------------------------------------------------------------

// Stages a v1 CT-calibration blob (the FROZEN pre-trim layout) on flash,
// exactly as a board commissioned by a pre-bump build would hold it.
static void stage_v1_ct_cal_blob(void)
{
    safety_ct_cal_blob_v1_t v1;
    memset(&v1, 0, sizeof(v1));
    v1.version = 1u;
    v1.ch[0].has_value = 1;
    v1.ch[0].source = (uint8_t)SAFETY_CT_CAL_SOURCE_MANUAL;
    v1.ch[0].a_fs = 30.0f;
    v1.ch[0].zero_mv = 59.0f;
    v1.ch[2].has_value = 1;
    v1.ch[2].source = (uint8_t)SAFETY_CT_CAL_SOURCE_SWEEP;
    v1.ch[2].a_fs = 100.0f;
    v1.ch[2].zero_mv = -12.5f;

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "setup: stage handle opens");
    TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_SAFETY_CT_CAL, &v1, sizeof(v1)) == HAL_OK,
               "setup: v1 ct_cal blob stages");
    TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "setup: stage commits");
    hal_kv_close(&h);
}

static void test_ct_cal_v1_blob_migrates_forward_instead_of_resetting(void)
{
    TEST_SECTION("load_ct_cal -- a v1 blob (a board commissioned before the trim existed) is "
                 "MIGRATED, not reset: every entered A_fs/zero_mv/source survives the bump");

    fake_kv_reset_all();
    stub_reset();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    stage_v1_ct_cal_blob();

    TEST_CHECK(SAFETY_CT_CAL_BLOB_VERSION == 2u,
               "the bump this migration exists for actually happened -- without it the v1 blob "
               "below is 'current version' and nothing here is proved");
    TEST_CHECK(sizeof(safety_ct_cal_blob_v1_t) != sizeof(safety_ct_cal_blob_t),
               "v1 and v2 really are different sizes -- which is why load_ct_cal() must "
               "discriminate on LENGTH before version, not read into a v2-sized struct");

    TEST_CHECK(safety_cfg_store_init() == ESP_OK, "init loads the stored blob");

    float a_fs = 0.0f, zero_mv = 0.0f;
    safety_ct_cal_source_t source = SAFETY_CT_CAL_SOURCE_SWEEP;
    TEST_CHECK(safety_cfg_store_get_ct_cal_input(0, &a_fs, &zero_mv, &source),
               "channel 0's entered calibration survived the migration");
    TEST_CHECK(fabsf(a_fs - 30.0f) < 1e-6f, "its clamp ratio is carried forward verbatim");
    TEST_CHECK(fabsf(zero_mv - 59.0f) < 1e-6f, "so is its zero offset");
    TEST_CHECK(source == SAFETY_CT_CAL_SOURCE_MANUAL,
               "and its provenance -- a migrated MANUAL channel must still beat the sweep");

    TEST_CHECK(safety_cfg_store_get_ct_cal_input(2, &a_fs, &zero_mv, &source),
               "channel 2 survived too");
    TEST_CHECK(fabsf(a_fs - 100.0f) < 1e-6f && fabsf(zero_mv - (-12.5f)) < 1e-6f &&
                   source == SAFETY_CT_CAL_SOURCE_SWEEP,
               "with its own values, not channel 0's");

    TEST_CHECK(!safety_cfg_store_get_ct_cal_input(1, NULL, NULL, NULL),
               "a channel that was unset in v1 is still unset -- the migration invents nothing");

    float off = 9.0f, gain = 9.0f;
    for (size_t c = 0; c < SAFETY_CT_CAL_CHANNELS; c++) {
        TEST_CHECK(safety_cfg_store_get_ct_cal_trim(c, &off, &gain), "the new trim reads back");
        TEST_CHECK(off == 0.0f && gain == 1.0f,
                   "seeded at IDENTITY, never at memset's 0.0 gain -- a zero gain would scale the "
                   "whole channel to zero amps");
    }

    fake_kv_reset_all();
}

static void test_ct_cal_migration_is_not_vacuous_a_broken_carry_forward_would_fail(void)
{
    TEST_SECTION("load_ct_cal -- NEGATIVE-TEST ANCHOR: the migration's carry-forward is what the "
                 "test above measures, and the values it checks are the ones a broken carry would "
                 "lose -- an unmigrated (reset-to-defaults) load reports has_value false");

    // The shape a defective migration takes: the v1 record is on flash and
    // readable, but the loader refuses it. That is observable ONLY as
    // has_value == false / defaulted values, which is exactly what the
    // assertions above would catch. Proved here from the other side: a blob
    // this build genuinely cannot know IS reset, and reads back that way.
    fake_kv_reset_all();
    stub_reset();
    hal_kv_init_partition(KILN_NVS_PARTITION);

    safety_ct_cal_blob_t future;
    memset(&future, 0, sizeof(future));
    future.version = (uint8_t)(SAFETY_CT_CAL_BLOB_VERSION + 1u);
    future.ch[0].has_value = 1;
    future.ch[0].a_fs = 30.0f;
    {
        hal_kv_handle_t h;
        TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
                   "setup: stage handle opens");
        TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_SAFETY_CT_CAL, &future, sizeof(future)) == HAL_OK,
                   "setup: newer-than-this-build blob stages");
        TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "setup: stage commits");
        hal_kv_close(&h);
    }

    TEST_CHECK(safety_cfg_store_init() == ESP_OK, "init runs");
    TEST_CHECK(!safety_cfg_store_get_ct_cal_input(0, NULL, NULL, NULL),
               "a version this build cannot know is refused wholesale, not reinterpreted field by "
               "field -- and a refusal is visibly different from a migration");
    float off = 9.0f, gain = 9.0f;
    TEST_CHECK(safety_cfg_store_get_ct_cal_trim(0, &off, &gain) && off == 0.0f && gain == 1.0f,
               "and the defaults it falls back to still carry an identity trim");

    fake_kv_reset_all();
}

static void test_ct_cal_trim_roundtrips_and_survives_a_reboot(void)
{
    TEST_SECTION("safety_cfg_store_set_ct_cal_trim -- the entered trim persists and comes back "
                 "after a fresh init, alongside the A_fs/zero_mv pair it sits beside");

    fake_kv_reset_all();
    stub_reset();
    TEST_CHECK(safety_cfg_store_init() == ESP_OK, "setup: first boot, no blob");

    esp_err_t nvs_err = ESP_FAIL;
    TEST_CHECK(safety_cfg_store_set_ct_cal_input(1, 30.0f, 59.0f, SAFETY_CT_CAL_SOURCE_MANUAL,
                                                 NULL, NULL, NULL),
               "setup: a clamp ratio is entered on channel 1");
    TEST_CHECK(safety_cfg_store_set_ct_cal_trim(1, -1.25f, 1.04f, &nvs_err),
               "a 4% scale trim with a small offset is accepted");
    TEST_CHECK(nvs_err == ESP_OK, "and the NVS write reports its own result, not a silent success");

    float off = 0.0f, gain = 0.0f;
    TEST_CHECK(safety_cfg_store_get_ct_cal_trim(1, &off, &gain) && fabsf(off - (-1.25f)) < 1e-6f &&
                   fabsf(gain - 1.04f) < 1e-6f,
               "it reads back immediately, before any reboot");

    // Reboot.
    stub_reset();
    TEST_CHECK(safety_cfg_store_init() == ESP_OK, "reload succeeds");
    TEST_CHECK(safety_cfg_store_get_ct_cal_trim(1, &off, &gain) && fabsf(off - (-1.25f)) < 1e-6f &&
                   fabsf(gain - 1.04f) < 1e-6f,
               "and survives the reboot as a v2 blob");
    float a_fs = 0.0f, zero_mv = 0.0f;
    TEST_CHECK(safety_cfg_store_get_ct_cal_input(1, &a_fs, &zero_mv, NULL) &&
                   fabsf(a_fs - 30.0f) < 1e-6f && fabsf(zero_mv - 59.0f) < 1e-6f,
               "without disturbing the pair stored in the same record");
    TEST_CHECK(safety_cfg_store_get_ct_cal_trim(0, &off, &gain) && off == 0.0f && gain == 1.0f,
               "and a channel nobody trimmed is still at identity, not at channel 1's values");

    fake_kv_reset_all();
}

static void test_ct_cal_trim_refuses_out_of_range_rather_than_clamping(void)
{
    TEST_SECTION("safety_cfg_store_set_ct_cal_trim -- CT_ATTRIBUTION_VERIFICATION_PLAN.md: "
                 "'do not clamp a bad trim into a plausible-looking one'");

    fake_kv_reset_all();
    stub_reset();
    TEST_CHECK(safety_cfg_store_init() == ESP_OK, "setup: first boot");

    TEST_CHECK(safety_cfg_store_set_ct_cal_trim(1, 0.0f, 1.10f, NULL), "setup: a good trim lands");

    TEST_CHECK(!safety_cfg_store_set_ct_cal_trim(1, 0.0f, 4.0f, NULL),
               "a gain past the ceiling is refused");
    TEST_CHECK(!safety_cfg_store_set_ct_cal_trim(1, 0.0f, 0.1f, NULL),
               "a gain below the floor is refused");
    TEST_CHECK(!safety_cfg_store_set_ct_cal_trim(1, 500.0f, 1.0f, NULL),
               "an absurd offset is refused");
    TEST_CHECK(!safety_cfg_store_set_ct_cal_trim(1, NAN, 1.0f, NULL), "a non-finite offset is refused");
    TEST_CHECK(!safety_cfg_store_set_ct_cal_trim(1, 0.0f, NAN, NULL), "a non-finite gain is refused");
    TEST_CHECK(!safety_cfg_store_set_ct_cal_trim(SAFETY_CT_CAL_CHANNELS, 0.0f, 1.0f, NULL),
               "an out-of-range channel is refused");

    float off = 0.0f, gain = 0.0f;
    TEST_CHECK(safety_cfg_store_get_ct_cal_trim(1, &off, &gain) && off == 0.0f &&
                   fabsf(gain - 1.10f) < 1e-6f,
               "and every refusal left the previously ACCEPTED trim exactly as it was -- refused "
               "means nothing changed, not 'clamped to the nearest legal value'");

    // The boundaries themselves are inclusive -- the NEGATIVE half of this
    // check: a strict comparison where the header promises [min, max] flips
    // one of these.
    TEST_CHECK(safety_cfg_store_set_ct_cal_trim(1, SAFETY_CT_CAL_TRIM_OFFSET_A_MIN,
                                                SAFETY_CT_CAL_TRIM_GAIN_MIN, NULL),
               "exactly at both minimums is accepted");
    TEST_CHECK(safety_cfg_store_set_ct_cal_trim(1, SAFETY_CT_CAL_TRIM_OFFSET_A_MAX,
                                                SAFETY_CT_CAL_TRIM_GAIN_MAX, NULL),
               "exactly at both maximums is accepted");

    fake_kv_reset_all();
}

void run_test_safety_cfg_store(void)
{
    test_index_for_id_finds_known_and_rejects_unknown();
    test_no_refetch_when_crc_unchanged();
    test_refetch_when_crc_changes();
    test_maybe_refetch_does_not_block_when_lock_is_busy();
    test_refetch_carries_unset_bit_through_not_unconditional_true();
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
    test_version_refuse_older_layout_without_a_migration();
    test_init_does_not_stamp_fetch_time_for_an_nvs_loaded_cache();
    test_safety_relay_type_defaults_to_contactor_and_pushes_to_relay_cycles();
    test_safety_relay_type_set_persists_and_roundtrips_after_reload();
    test_safety_relay_type_rejects_ssr();
    test_safety_relay_type_reports_nvs_failure_via_out_param();
    test_ct_cal_convert_at_several_probe_ratings();
    test_ct_cal_convert_rejects_out_of_range();
    test_ct_cal_manual_wins_over_sweep();
    test_ct_cal_set_rejects_out_of_range_channel_and_value();
    test_ct_cal_set_reports_nvs_failure_via_out_param();
    test_cache_stale_cleared_by_direct_refetch_not_only_maybe_refetch();
    test_ct_cal_v1_blob_migrates_forward_instead_of_resetting();
    test_ct_cal_migration_is_not_vacuous_a_broken_carry_forward_would_fail();
    test_ct_cal_trim_roundtrips_and_survives_a_reboot();
    test_ct_cal_trim_refuses_out_of_range_rather_than_clamping();
}
