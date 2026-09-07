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
// !hal_kv_write_safe_here() -- see HW_ABSTRACTION.md Phase 3 item 3,
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

// relay_cycles.c (RELAY_LIFE_BUDGET.md) hand-declares
// uart_bridge_ext_run_on_flash_worker()/uart_bridge_ext_is_on_flash_worker()
// (same "declared by hand, not via uart_bridge.h" reasoning as
// safety_cfg_store.c) for relay_cycles_reset()'s flash-worker dispatch --
// this shared stub, included before relay_cycles.c below, supplies their
// definitions and the same busy/re-entrancy modeling test_adaptive_tune.c
// and test_profile_executor_prestart.c already rely on.
#include "stubs/bx_worker_stub.h"

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

// --- RELAY_LIFE_BUDGET.md: type table, budget math, fifth slot,
// v1->v2 blob migration. These tests call relay_cycles_budget()/
// relay_cycles_set_type()/relay_cycles_note_safety_edge() directly (this
// file already #includes relay_cycles.c), quantizing counts against a rated
// life chosen so 79/80/89/90% land on exact integer counts -- rated life
// 100 (an override, not the real table value) makes count==tier boundaries
// trivial to hit exactly instead of rounding into or out of a tier.

static void test_budget_ssr_has_no_budget(void)
{
    TEST_SECTION("relay_cycles_budget -- ssr relays report has_budget == false regardless of count "
                 "or override (RELAY_LIFE_BUDGET.md: 'ssr = no budget, percent reported as null')");
    reset_all();

    relay_cycles_set_type(0, RELAY_TYPE_SSR, 100); // override present but must be ignored for ssr
    s_rc.counts[0] = 1000000;

    relay_cycles_budget_t b;
    relay_cycles_budget(0, &b);
    TEST_CHECK(b.has_budget == false, "ssr never has a budget, however high the count");
    TEST_CHECK(b.tier == RELAY_BUDGET_TIER_NONE, "no budget means no tier either");
}

static void test_budget_quantized_thresholds(void)
{
    TEST_SECTION("relay_cycles_budget -- 79/80/89/90% land exactly on the warn/error tier "
                 "boundaries (>=80% warn, >=90% error)");
    reset_all();

    relay_cycles_set_type(1, RELAY_TYPE_CONTACTOR, 100); // override -> rated life of exactly 100

    relay_cycles_budget_t b;

    s_rc.counts[1] = 79;
    relay_cycles_budget(1, &b);
    TEST_CHECK(b.has_budget && b.percent == 79.0f && b.tier == RELAY_BUDGET_TIER_NONE,
               "79%% is below the warn threshold");

    s_rc.counts[1] = 80;
    relay_cycles_budget(1, &b);
    TEST_CHECK(b.has_budget && b.percent == 80.0f && b.tier == RELAY_BUDGET_TIER_WARN,
               "80%% is exactly the warn threshold (>=80%%)");

    s_rc.counts[1] = 89;
    relay_cycles_budget(1, &b);
    TEST_CHECK(b.has_budget && b.percent == 89.0f && b.tier == RELAY_BUDGET_TIER_WARN,
               "89%% is still warn, not yet error");

    s_rc.counts[1] = 90;
    relay_cycles_budget(1, &b);
    TEST_CHECK(b.has_budget && b.percent == 90.0f && b.tier == RELAY_BUDGET_TIER_ERROR,
               "90%% is exactly the error threshold (>=90%%)");
}

static void test_budget_override_wins_over_table(void)
{
    TEST_SECTION("relay_cycles_budget -- a nonzero rated_override wins over the type's table value");
    reset_all();

    // Table value for contactor is 100000; an override of 10 makes the same
    // count read as a wildly different percent, proving the override (not
    // the table) was actually used.
    relay_cycles_set_type(2, RELAY_TYPE_CONTACTOR, 10);
    s_rc.counts[2] = 9;

    relay_cycles_budget_t b;
    relay_cycles_budget(2, &b);
    TEST_CHECK(b.has_budget && b.rated == 10, "the override value is used as the rated life, not the table's 100000");
    TEST_CHECK(b.percent == 90.0f && b.tier == RELAY_BUDGET_TIER_ERROR,
               "9/10 = 90%% -- only reachable if the override, not the 100000 table value, was used");

    // Zero override falls back to the table.
    relay_cycles_set_type(2, RELAY_TYPE_CONTACTOR, 0);
    relay_cycles_budget(2, &b);
    TEST_CHECK(b.has_budget && b.rated == RELAY_RATED_LIFE_CONTACTOR,
               "override == 0 means 'use the table', per relay_cycles_set_type()'s documented contract");
}

static void test_safety_slot_edge_and_persistence(void)
{
    TEST_SECTION("relay_cycles_note_safety_edge -- increments the fifth slot (K4) independently "
                 "of relay_cycles_add(), and it round-trips through persist/load like the others");
    reset_all();

    TEST_CHECK(RELAY_CYCLES_SAFETY_INDEX == KILN_IO_RELAY_COUNT,
               "the safety slot is the one right after the four heater relays");

    relay_cycles_add(0x0F, 5); // all four heater relays, must NOT touch the safety slot
    relay_cycles_note_safety_edge();
    relay_cycles_note_safety_edge();
    relay_cycles_note_safety_edge();

    TEST_CHECK(s_rc.counts[RELAY_CYCLES_SAFETY_INDEX] == 3, "three edges noted, one each call");
    TEST_CHECK(s_rc.counts[0] == 42 + 5, "relay_cycles_add() still only touches the four heater slots");

    hal_status_t err = persist_locked();
    TEST_CHECK(err == HAL_OK, "persist succeeds with the fifth slot populated");

    memset(&s_rc, 0, sizeof(s_rc));
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
               "reopen for readback");
    relay_cycles_blob_t blob;
    size_t len = sizeof(blob);
    TEST_CHECK(hal_kv_get_blob(&h, NVS_KEY_CYCLES, &blob, &len) == HAL_OK && len == sizeof(blob),
               "the v2 blob round-trips at its full size");
    hal_kv_close(&h);
    TEST_CHECK(blob.counts[RELAY_CYCLES_SAFETY_INDEX] == 3,
               "the persisted blob carries the safety slot's count, not just the four heater ones");
}

static void test_v1_blob_migrates_to_v2(void)
{
    TEST_SECTION("relay_cycles_init -- a v1 blob (bare 4-count array, no types, no fifth slot) "
                 "migrates to v2: existing counts kept, type defaults to ssr, fifth slot starts at 0");
    fake_kv_reset_all();
    fake_kv_set_write_safe_here(true);
    hal_kv_init_partition(KILN_NVS_PARTITION);

    // Write a v1-shaped blob directly, bypassing persist_locked() (which
    // only ever writes the current version) -- this simulates a board that
    // last persisted before this change shipped.
    relay_cycles_blob_v1_t v1;
    v1.version = 1;
    v1.counts[0] = 111;
    v1.counts[1] = 222;
    v1.counts[2] = 0;
    v1.counts[3] = 0;
    hal_kv_handle_t hw;
    TEST_CHECK(hal_kv_open(&hw, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
               "open for the v1 blob write");
    TEST_CHECK(hal_kv_set_blob(&hw, NVS_KEY_CYCLES, &v1, sizeof(v1)) == HAL_OK, "write the v1-sized blob");
    TEST_CHECK(hal_kv_commit(&hw) == HAL_OK, "commit the v1 blob");
    hal_kv_close(&hw);

    memset(&s_rc, 0, sizeof(s_rc));
    s_rc.lock = NULL;
    s_rc.initialized = false;
    TEST_CHECK(relay_cycles_init() == ESP_OK, "init succeeds against a v1-shaped blob");

    TEST_CHECK(s_rc.counts[0] == 111 && s_rc.counts[1] == 222 && s_rc.counts[2] == 0 && s_rc.counts[3] == 0,
               "the four heater relays' existing counts survive the migration unchanged");
    TEST_CHECK(s_rc.counts[RELAY_CYCLES_SAFETY_INDEX] == 0, "the new fifth slot starts at zero, not garbage");
    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        TEST_CHECK(s_rc.types[r] == RELAY_TYPE_SSR, "a migrated v1 relay defaults to ssr type");
    }

    relay_cycles_budget_t b;
    relay_cycles_budget(0, &b);
    TEST_CHECK(b.has_budget == false, "a migrated relay's default ssr type means no budget shown yet");

    fake_kv_reset_all();
}

// --- RELAY_LIFE_BUDGET.md: relay_cycles_reset() -- the API the
// LCD diagnostics page's two-tap confirm and diagnostics_http.c's
// POST /api/relay_cycles/reset both call.

static void test_reset_zeroes_count_and_persists(void)
{
    TEST_SECTION("relay_cycles_reset -- zeroes the count, leaves type/override alone, "
                 "and persists through the flash-worker stub");
    reset_all();
    relay_cycles_set_type(0, RELAY_TYPE_CONTACTOR, 55);

    TEST_CHECK(relay_cycles_reset(0) == true, "reset succeeds on the normal (not-on-worker) path");
    TEST_CHECK(s_rc.counts[0] == 0, "the count is zeroed");

    relay_type_t type;
    uint32_t override_val;
    relay_cycles_get_type(0, &type, &override_val);
    TEST_CHECK(type == RELAY_TYPE_CONTACTOR && override_val == 55,
               "type/override are untouched -- a reset means 'contact replaced', not 'forget the type'");

    TEST_CHECK(s_rc.dirty == false, "the dispatched persist actually landed (dirty cleared)");

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
               "reopen for readback");
    relay_cycles_blob_t blob;
    size_t len = sizeof(blob);
    TEST_CHECK(hal_kv_get_blob(&h, NVS_KEY_CYCLES, &blob, &len) == HAL_OK && len == sizeof(blob),
               "blob round-trips");
    hal_kv_close(&h);
    TEST_CHECK(blob.counts[0] == 0, "the zeroed count is what actually landed in the store, "
                                    "not just in RAM");
}

static void test_reset_rejects_out_of_range_relay(void)
{
    TEST_SECTION("relay_cycles_reset -- an out-of-range relay index is refused, not undefined behavior");
    reset_all();

    TEST_CHECK(relay_cycles_reset(RELAY_CYCLES_COUNT) == false, "index == COUNT is out of range");
    TEST_CHECK(relay_cycles_reset((unsigned)RELAY_CYCLES_COUNT + 10) == false, "well past COUNT is also refused");
}

static void test_reset_runs_inline_when_already_on_flash_worker(void)
{
    TEST_SECTION("relay_cycles_reset -- called from a handler already on the flash worker runs "
                 "inline instead of dispatching a second job (the re-entrancy deadlock class "
                 "flash_worker_lint.py's pattern 1 exists to avoid)");
    reset_all();
    s_rc.counts[3] = 99;

    s_stub_on_flash_worker = true; // simulate already being on the worker, same as bx_worker_stub.h's
                                    // own doc comment describes for calling INTO a dispatched job
    TEST_CHECK(relay_cycles_reset(3) == true, "reset still succeeds via the inline path");
    TEST_CHECK(s_stub_bx_busy == false, "no dispatch was attempted -- a real dispatch here would "
                                        "have deadlocked, and the busy-modeling stub would have "
                                        "failed loudly instead if one had been attempted");
    s_stub_on_flash_worker = false; // leave shared stub state as every other test expects

    TEST_CHECK(s_rc.counts[3] == 0, "the count was actually reset via the inline path");
}

// opus review (MEDIUM, follow-up audit): relay_cycles_reset() used to hold
// s_rc.lock across the ENTIRE flash-worker dispatch, which correctly avoided
// losing a concurrent relay_cycles_add() but stalled every other lock holder
// (the executor tick) for a full NVS commit. The fix takes a snapshot under
// the lock, releases it, then dispatches the write against the snapshot
// alone. This test proves the fix didn't just trade the stall back for the
// race it was closing: a relay_cycles_add() that lands in the window between
// the snapshot and the dispatched write actually running must not be
// silently dropped -- either because the write in flight already captured it
// (impossible here, since the snapshot was already taken) or because `dirty`
// is left set so a later relay_cycles_maybe_persist()/relay_cycles_flush()
// picks it up.
//
// This file already #includes relay_cycles.c directly (see the top of this
// file), so persist_snapshot()/reset_persist_job()/reset_persist_job_ctx_t
// and reset_persist_job_arg_t are all directly visible here. Rather than
// touch the shared bx_worker_stub.h (used by other test files too) to add a
// mid-dispatch hook, this test reproduces relay_cycles_reset()'s own
// snapshot-then-dispatch sequence by hand, inserting the "concurrent"
// relay_cycles_add() call itself between the snapshot and the dispatch --
// exactly the window the real function's fix is protecting.
//
// Before this fix's dirty-handling was written carefully, a naive version
// could overwrite `dirty` back to false unconditionally after a successful
// write, silently losing the concurrent increment (relay_cycles_get() would
// show the incremented RAM value, but nothing would ever persist it past a
// reboot -- exactly this codebase's "reset one side of a pair" bug class).
static void test_reset_does_not_lose_a_concurrent_add(void)
{
    TEST_SECTION("relay_cycles_reset -- a relay_cycles_add() landing between the snapshot and the "
                 "dispatched write must not be lost: dirty must survive so it persists later "
                 "(opus review MEDIUM: lock-across-dispatch was fixed by a snapshot, not by "
                 "silently dropping the race it was protecting against)");
    reset_all();
    s_rc.counts[1] = 7; // relay 1's starting count, matches reset_all()'s own setup

    // --- relay_cycles_reset(0)'s own snapshot step, reproduced by hand ---
    // reset_all() above memset()s s_rc (including s_rc.lock) to zero, so the
    // lock must be (re-)created here the same way relay_cycles_reset() does
    // via ensure_lock() -- every other test in this file reaches s_rc.lock
    // indirectly through a real relay_cycles_*() call that calls ensure_lock()
    // itself first; this one takes the lock directly, so it must too.
    TEST_CHECK(ensure_lock(), "lock (re-)created after reset_all()'s memset");
    reset_persist_job_arg_t snap;
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    s_rc.counts[0] = 0;
    memcpy(snap.counts, s_rc.counts, sizeof(snap.counts));
    memcpy(snap.types, s_rc.types, sizeof(snap.types));
    memcpy(snap.rated_overrides, s_rc.rated_overrides, sizeof(snap.rated_overrides));
    s_rc.dirty = false;
    xSemaphoreGive(s_rc.lock);

    // --- the "concurrent" add(), landing in the window between the snapshot
    // above and the dispatched write below -- exercises the real lock, not a
    // bypass of it. ---
    relay_cycles_add(0x02, 9); // relay index 1 (mask bit 1), same slot the snapshot above did NOT touch

    // --- the dispatched write itself, against the snapshot ONLY (never
    // touching the live s_rc that the concurrent add() above just updated) --
    reset_persist_job_ctx_t ctx = { .snap = &snap, .err = ESP_FAIL };
    reset_persist_job(&ctx);
    TEST_CHECK(ctx.err == ESP_OK, "the dispatched write against the snapshot succeeds");

    TEST_CHECK(s_rc.counts[1] == 7 + 9, "the concurrent add()'s increment is visible in RAM");
    TEST_CHECK(s_rc.dirty == true, "dirty must still be set after reset(0)'s own successful "
                                   "persist -- the concurrent add() happened after the snapshot "
                                   "was taken, so it was NOT included in what reset(0) just wrote, "
                                   "and must survive to be picked up by the next persist. A version "
                                   "that unconditionally cleared dirty after a successful write "
                                   "would fail this check, silently losing the increment across a "
                                   "reboot.");

    // Prove the flag being set is not vacuous: flushing now actually
    // persists relay 1's incremented count, not just relay 0's reset.
    TEST_CHECK(relay_cycles_flush() == ESP_OK, "the follow-up flush this dirty flag exists to "
                                               "trigger succeeds");
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
               "reopen for readback");
    relay_cycles_blob_t blob;
    size_t len = sizeof(blob);
    TEST_CHECK(hal_kv_get_blob(&h, NVS_KEY_CYCLES, &blob, &len) == HAL_OK && len == sizeof(blob),
               "blob round-trips");
    hal_kv_close(&h);
    TEST_CHECK(blob.counts[1] == 7 + 9, "the concurrent add()'s increment actually reached flash "
                                        "via the follow-up flush -- not just left dirty in RAM "
                                        "forever");
    TEST_CHECK(blob.counts[0] == 0, "relay 0's reset from earlier is still reflected too");
}

// opus review finding (LOW): persist_snapshot_now() releases s_rc.lock
// between snapshotting and writing (see its own comment), which used to
// leave nothing serializing two overlapping callers -- relay_cycles_maybe_persist()
// from the executor's tick path and relay_cycles_flush() from its stop path
// can both decide `dirty` is set and both call persist_snapshot_now() close
// together. The fix adds s_rc.persist_lock, held around the whole
// snapshot-then-write section. Host tests are single-threaded (stubs/freertos/
// semphr.h's xSemaphoreTake()/_Give() are no-ops), so the actual race cannot
// be reproduced here -- this proves the mechanics instead: ensure_lock()
// creates persist_lock, and back-to-back persist_snapshot_now() calls (the
// same sequence maybe_persist()-then-flush() produces) still leave the LAST
// call's data as the one on flash, not an earlier one silently winning.
static void test_persist_lock_created_and_back_to_back_persists_keep_the_latest_write(void)
{
    TEST_SECTION("relay_cycles persist_snapshot_now -- a dedicated persist_lock exists and "
                 "back-to-back persists (maybe_persist immediately followed by flush) do not let "
                 "an earlier snapshot land after a later one");
    reset_all();

    TEST_CHECK(ensure_lock(), "ensure_lock must succeed");
    TEST_CHECK(s_rc.persist_lock != NULL, "a dedicated persist_lock is created alongside s_rc.lock");

    // First persist: relay 0 at 42.
    TEST_CHECK(persist_snapshot_now() == HAL_OK, "first persist succeeds");

    // A change lands, then a second persist immediately follows -- the
    // scenario relay_cycles_maybe_persist() (tick, due) racing
    // relay_cycles_flush() (executor stop) produces.
    relay_cycles_add(0x01, 5); // relay 0 -> 47
    s_rc.dirty = true;
    TEST_CHECK(persist_snapshot_now() == HAL_OK, "second, later persist succeeds");

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
               "reopen for readback");
    relay_cycles_blob_t blob;
    size_t len = sizeof(blob);
    TEST_CHECK(hal_kv_get_blob(&h, NVS_KEY_CYCLES, &blob, &len) == HAL_OK && len == sizeof(blob),
               "blob round-trips");
    hal_kv_close(&h);
    TEST_CHECK(blob.counts[0] == 47, "the LATER snapshot's value is what's on flash, not the "
                                     "earlier one -- an older snapshot landing after a newer one "
                                     "would leave 42 here instead");
    TEST_CHECK(s_rc.dirty == false, "the second persist cleared dirty; nothing left it stuck set");
}

void run_test_relay_cycles(void)
{
    test_persist_locked_refuses_when_calling_stack_is_external_ram();
    test_persist_lock_created_and_back_to_back_persists_keep_the_latest_write();
    test_persist_locked_proceeds_normally_on_an_internal_ram_stack();
    test_budget_ssr_has_no_budget();
    test_budget_quantized_thresholds();
    test_budget_override_wins_over_table();
    test_safety_slot_edge_and_persistence();
    test_v1_blob_migrates_to_v2();
    test_reset_zeroes_count_and_persists();
    test_reset_rejects_out_of_range_relay();
    test_reset_runs_inline_when_already_on_flash_worker();
    test_reset_does_not_lose_a_concurrent_add();

    fake_kv_reset_all(); // leave shared fake state as every other test file in this binary expects
}
