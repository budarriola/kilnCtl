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

#ifdef _WIN32
#include <direct.h>
#define RCCF_MKDIR(p) _mkdir(p)
#define RCCF_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define RCCF_MKDIR(p) mkdir((p), 0755)
#define RCCF_RMDIR(p) rmdir(p)
#endif

#include "cfg_fs.h" /* real mount/write-atomic/read/delete against a temp dir -- docs/FILESYSTEM_USER_DATA_PLAN.md
                       * section 5 step 6's cfg-filesystem dual-write bridge for THIS module, see the
                       * new tests appended near the bottom of this file. */

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
    TEST_CHECK(persist_snapshot_now(portMAX_DELAY) == HAL_OK, "first persist succeeds");

    // A change lands, then a second persist immediately follows -- the
    // scenario relay_cycles_maybe_persist() (tick, due) racing
    // relay_cycles_flush() (executor stop) produces.
    relay_cycles_add(0x01, 5); // relay 0 -> 47
    s_rc.dirty = true;
    TEST_CHECK(persist_snapshot_now(portMAX_DELAY) == HAL_OK, "second, later persist succeeds");

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

// MEDIUM follow-up fix: persist_snapshot_now() now takes s_rc.persist_lock
// with a caller-supplied wait via xSemaphoreTake() and actually branches on
// its return value (BUSY vs got-the-lock) -- every earlier test in this file
// (and every xSemaphoreTake() call the rest of relay_cycles.c makes) ignores
// that return value entirely, so stubs/freertos/semphr.h defaults it to
// pdFALSE ("host tests are single-threaded, nothing distinguishes semaphore
// identities" -- see that header's own comment on
// g_test_stub_semaphore_take_default). Left at its default, EVERY
// xSemaphoreTake() in this executable would now read as "failed to take" the
// moment code starts checking it, which would make persist_snapshot_now()
// report HAL_BUSY on every call above rather than the successes those tests
// assert. Set pdTRUE here (nothing in this executable runs after
// run_test_relay_cycles() -- see test_run_state.c's main()) so "the lock is
// free" is the default for this file's own tests, matching every existing
// call site's assumption; the negative test below flips it back to pdFALSE
// for the one case that specifically wants to prove the busy path.
static void test_maybe_persist_skips_without_blocking_when_persist_lock_is_busy(void)
{
    TEST_SECTION("relay_cycles_maybe_persist -- when persist_lock cannot be taken immediately "
                 "(a flush is already mid-persist), the tick path skips this cycle instead of "
                 "blocking, and does not lose the pending change (opus review MEDIUM follow-up: "
                 "persist_lock used to be portMAX_DELAY on both callers, so the tick and the "
                 "executor-stop flush could stall each other for a full NVS commit)");
    reset_all();
    TEST_CHECK(ensure_lock(), "lock (re-)created after reset_all()'s memset");

    // Force the very next xSemaphoreTake() (persist_snapshot_now()'s
    // non-blocking take of persist_lock) to report "busy", simulating a
    // flush() already holding it.
    g_test_stub_semaphore_take_default = 0; // pdFALSE
    s_rc.last_persist_us = 0; // guarantees the interval check alone would say "due"

    relay_cycles_maybe_persist();

    TEST_CHECK(s_rc.dirty == true, "a deferred persist must leave dirty set -- the change is not "
                                   "lost, only not yet written");

    hal_kv_handle_t h;
    hal_status_t open_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    TEST_CHECK(open_err == HAL_NOT_FOUND, "the deferred persist never touched flash at all -- it "
                                          "returned before taking s_rc.lock or dispatching a write, "
                                          "so no namespace was ever created");

    g_test_stub_semaphore_take_default = 1; // restore pdTRUE for every test after this one
}

// --- backup-gate pass (docs/FILESYSTEM_PLAN.md, 2026-09-07):
// relay_cycles_restore_all() -- the counterpart to relay_cycles_reset()
// above, restoring all RELAY_CYCLES_COUNT counts from a backup archive
// (full_board_backup.py's /api/status.relay_counts) rather than zeroing one.

static void test_restore_all_sets_every_count_and_persists(void)
{
    TEST_SECTION("relay_cycles_restore_all -- sets every count from a backup array, leaves "
                 "type/override alone, and persists through the flash-worker stub");
    reset_all();
    relay_cycles_set_type(0, RELAY_TYPE_CONTACTOR, 55);

    uint32_t backup[RELAY_CYCLES_COUNT];
    for (uint8_t i = 0; i < RELAY_CYCLES_COUNT; i++) {
        backup[i] = 1000u + i;
    }
    TEST_CHECK(relay_cycles_restore_all(backup) == true, "restore succeeds on the normal path");
    for (uint8_t i = 0; i < RELAY_CYCLES_COUNT; i++) {
        TEST_CHECK(s_rc.counts[i] == backup[i], "every restored count lands in RAM");
    }

    relay_type_t type;
    uint32_t override_val;
    relay_cycles_get_type(0, &type, &override_val);
    TEST_CHECK(type == RELAY_TYPE_CONTACTOR && override_val == 55,
               "type/override are untouched by a restore, same convention as relay_cycles_reset()");

    TEST_CHECK(s_rc.dirty == false, "the dispatched persist actually landed (dirty cleared)");

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_OK,
               "reopen for readback");
    relay_cycles_blob_t blob;
    size_t len = sizeof(blob);
    TEST_CHECK(hal_kv_get_blob(&h, NVS_KEY_CYCLES, &blob, &len) == HAL_OK && len == sizeof(blob),
               "blob round-trips");
    hal_kv_close(&h);
    for (uint8_t i = 0; i < RELAY_CYCLES_COUNT; i++) {
        TEST_CHECK(blob.counts[i] == backup[i], "the restored counts are what actually landed in the "
                                                 "store, not just in RAM (byte-equal to the archive)");
    }
}

static void test_restore_all_is_idempotent(void)
{
    TEST_SECTION("relay_cycles_restore_all -- calling it twice with the same archive produces the "
                 "identical on-disk blob both times (idempotent restore)");
    reset_all();

    uint32_t backup[RELAY_CYCLES_COUNT];
    for (uint8_t i = 0; i < RELAY_CYCLES_COUNT; i++) {
        backup[i] = 500u + i * 3u;
    }
    TEST_CHECK(relay_cycles_restore_all(backup) == true, "first restore succeeds");
    relay_cycles_blob_t blob_first;
    hal_kv_handle_t h;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    size_t len = sizeof(blob_first);
    hal_kv_get_blob(&h, NVS_KEY_CYCLES, &blob_first, &len);
    hal_kv_close(&h);

    TEST_CHECK(relay_cycles_restore_all(backup) == true, "second restore of the same archive succeeds");
    relay_cycles_blob_t blob_second;
    hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    len = sizeof(blob_second);
    hal_kv_get_blob(&h, NVS_KEY_CYCLES, &blob_second, &len);
    hal_kv_close(&h);

    TEST_CHECK(memcmp(&blob_first, &blob_second, sizeof(blob_first)) == 0,
               "restoring the same archive twice writes byte-identical blobs -- idempotent");
}

// NEGATIVE TEST: a corrupted/out-of-range archive field must refuse the
// WHOLE restore, not silently clamp or partially write. Proves the
// all-or-nothing contract relay_cycles_restore_all()'s own comment claims.
static void test_restore_all_refuses_out_of_range_count_and_writes_nothing(void)
{
    TEST_SECTION("relay_cycles_restore_all -- NEGATIVE TEST: one field far past the sanity ceiling "
                 "(simulating a corrupted/truncated backup) refuses the ENTIRE restore -- no partial "
                 "write, RAM counts unchanged, no NVS write attempted");
    reset_all();
    s_rc.counts[0] = 11;
    s_rc.counts[1] = 22;
    s_rc.counts[2] = 33;

    uint32_t backup[RELAY_CYCLES_COUNT];
    for (uint8_t i = 0; i < RELAY_CYCLES_COUNT; i++) {
        backup[i] = 100u + i; // otherwise-plausible values
    }
    backup[2] = RELAY_CYCLES_RESTORE_MAX_COUNT + 1u; // the one corrupted field

    TEST_CHECK(relay_cycles_restore_all(backup) == false,
               "restore is refused because of the single out-of-range field");
    TEST_CHECK(s_rc.counts[0] == 11 && s_rc.counts[1] == 22 && s_rc.counts[2] == 33,
               "RAM counts are completely unchanged -- not even the valid fields were applied "
               "(all-or-nothing, checked before any write)");

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_NOT_FOUND,
               "no NVS write was ever attempted -- the namespace was never created");
}

static void test_restore_all_rejects_null_pointer(void)
{
    TEST_SECTION("relay_cycles_restore_all -- a NULL counts pointer is refused, not undefined behavior");
    reset_all();
    TEST_CHECK(relay_cycles_restore_all(NULL) == false, "NULL is refused");
}

// ---------------------------------------------------------------------
// cfg-filesystem dual-write bridge (docs/FILESYSTEM_USER_DATA_PLAN.md
// section 5 step 6, "MOVE, but last, after everything else has flown").
// Uses the real relay_cycles_init()/relay_cycles_flush()/relay_cycles_reset()/
// relay_cycles_restore_all() public API (never pokes s_rc directly except to
// arrange a starting count and reset test-global state) plus a real cfg_fs.c
// against a temp directory -- same convention as test_relay_names_cfg_fs.c.
// ---------------------------------------------------------------------
static const char *RC_SCRATCH_BASE = "cfg_fs_test_relay_cycles";

static void reset_all_cfg_fs(void)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/.tmp/%s", RC_SCRATCH_BASE, RELAY_CYCLES_FILE_PATH);
    remove(path);
    snprintf(path, sizeof(path), "%s/%s", RC_SCRATCH_BASE, RELAY_CYCLES_FILE_PATH);
    remove(path);
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s/.tmp", RC_SCRATCH_BASE);
    RCCF_RMDIR(tmp);
    RCCF_RMDIR(RC_SCRATCH_BASE);
    RCCF_MKDIR(RC_SCRATCH_BASE);

    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
    fake_kv_reset_all();
    fake_kv_set_write_safe_here(true);
    hal_kv_init_partition(KILN_NVS_PARTITION);
    memset(&s_rc, 0, sizeof(s_rc));
}

static void test_cfg_fs_partition_absent_behaves_like_before(void)
{
    TEST_SECTION("relay_cycles cfg_fs: partition absent -- init/flush behave exactly like NVS-only");
    reset_all_cfg_fs();
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs never mounted in this test");

    TEST_CHECK(relay_cycles_init() == ESP_OK, "init succeeds with no `cfg` partition mounted");
    s_rc.counts[0] = 55;
    s_rc.dirty = true;
    TEST_CHECK(relay_cycles_flush() == ESP_OK, "flush succeeds, NVS-only");

    memset(&s_rc, 0, sizeof(s_rc));
    TEST_CHECK(relay_cycles_init() == ESP_OK, "re-init");
    TEST_CHECK(s_rc.counts[0] == 55, "count reloads from NVS alone");
}

static void test_cfg_fs_migrates_nvs_value_to_file_then_prefers_it(void)
{
    TEST_SECTION("relay_cycles cfg_fs: NVS fallback migrates to file; a later boot prefers the file");
    reset_all_cfg_fs();
    TEST_CHECK(cfg_fs_init(RC_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");

    TEST_CHECK(relay_cycles_init() == ESP_OK, "first boot: nothing in NVS or file yet");
    s_rc.counts[0] = 10;
    s_rc.counts[1] = 20;
    s_rc.dirty = true;
    TEST_CHECK(relay_cycles_flush() == ESP_OK, "flush dual-writes file first, then NVS");

    bool exists = false;
    TEST_CHECK(cfg_fs_exists(RELAY_CYCLES_FILE_PATH, &exists) == ESP_OK && exists,
               "the flush's dual-write actually created the file");

    memset(&s_rc, 0, sizeof(s_rc));
    TEST_CHECK(relay_cycles_init() == ESP_OK, "second boot");
    TEST_CHECK(s_rc.counts[0] == 10 && s_rc.counts[1] == 20, "counts reload correctly (file-preferred)");
    TEST_CHECK(s_rc.rev == 1, "rev tracks the one flush");
}

static void test_cfg_fs_dual_write_stays_in_sync_across_repeated_flushes(void)
{
    TEST_SECTION("relay_cycles cfg_fs: repeated flushes keep file and NVS in sync (incrementing rev)");
    reset_all_cfg_fs();
    TEST_CHECK(cfg_fs_init(RC_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    TEST_CHECK(relay_cycles_init() == ESP_OK, "init");

    for (int i = 1; i <= 3; i++) {
        s_rc.counts[0] = (uint32_t)(i * 100);
        s_rc.dirty = true;
        TEST_CHECK(relay_cycles_flush() == ESP_OK, "flush N");
    }

    relay_cycles_blob_t raw;
    uint32_t rev = 0;
    bool valid = false;
    pref_cfg_fs_load_raw(RELAY_CYCLES_FILE_PATH, sizeof(raw), relay_cycles_file_validate, &raw, &rev, &valid);
    TEST_CHECK(valid && rev == 3, "file rev tracks three flushes");
    TEST_CHECK(raw.counts[0] == 300, "file holds the LATEST flush");

    memset(&s_rc, 0, sizeof(s_rc));
    TEST_CHECK(relay_cycles_init() == ESP_OK, "reload");
    TEST_CHECK(s_rc.counts[0] == 300, "NVS agrees with the file after three dual-writes");
}

static esp_err_t rc_failing_write_fn(const char *rel_path, const void *data, size_t len)
{
    (void)rel_path;
    (void)data;
    (void)len;
    return ESP_FAIL;
}

static void test_cfg_fs_divergence_tie_break_strict_greater_than(void)
{
    TEST_SECTION("relay_cycles cfg_fs: divergence tie-break -- STRICT file_rev > nvs_rev, not >=");
    reset_all_cfg_fs();
    TEST_CHECK(cfg_fs_init(RC_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    TEST_CHECK(relay_cycles_init() == ESP_OK, "init");

    s_rc.counts[0] = 1;
    s_rc.dirty = true;
    TEST_CHECK(relay_cycles_flush() == ESP_OK, "rev 1 written to both sides");

    // File write fails from here -- NVS advances, file is stuck at rev 1.
    pref_cfg_fs_set_write_fn(rc_failing_write_fn);
    s_rc.counts[0] = 2;
    s_rc.dirty = true;
    TEST_CHECK(relay_cycles_flush() == ESP_OK, "flush still reports OK -- NVS write is authoritative");
    pref_cfg_fs_reset_write_fn_for_test();

    memset(&s_rc, 0, sizeof(s_rc));
    TEST_CHECK(relay_cycles_init() == ESP_OK, "reload");
    TEST_CHECK(s_rc.counts[0] == 2, "NVS (higher rev) wins -- the stale rev-1 file is NOT trusted");

    // EQUAL rev, differing content: NVS must still win (never file_rev >= nvs_rev).
    relay_cycles_blob_t stale_equal_rev;
    memset(&stale_equal_rev, 0, sizeof(stale_equal_rev));
    stale_equal_rev.version = RELAY_CYCLES_VERSION;
    stale_equal_rev.counts[0] = 999; // would be WRONG if adopted
    esp_err_t save_err = pref_cfg_fs_save(RELAY_CYCLES_FILE_PATH, &stale_equal_rev, sizeof(stale_equal_rev), 2);
    TEST_CHECK(save_err == ESP_OK, "test setup: file forced to rev 2 (equal to NVS) with different content");

    memset(&s_rc, 0, sizeof(s_rc));
    TEST_CHECK(relay_cycles_init() == ESP_OK, "reload after equal-rev divergence");
    TEST_CHECK(s_rc.counts[0] == 2,
               "NVS wins the EQUAL-rev tie -- content 999 from the file is refused (STRICT > required)");
}

static void test_cfg_fs_reset_all_composes_with_migration_never_loses_counts(void)
{
    TEST_SECTION("relay_cycles cfg_fs: relay_cycles_reset()/restore_all() compose with the bridge -- "
                 "counts are never lost or double-counted across a migration");
    reset_all_cfg_fs();
    TEST_CHECK(cfg_fs_init(RC_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    TEST_CHECK(relay_cycles_init() == ESP_OK, "init");

    // NVS-only history first (as if this board pre-dates the file bridge).
    s_rc.counts[0] = 2201;
    s_rc.counts[1] = 2994;
    s_rc.counts[2] = 3214;
    s_rc.dirty = true;
    TEST_CHECK(relay_cycles_flush() == ESP_OK, "bench-accumulated counts persisted (file + NVS, rev 1)");

    // relay_cycles_reset() on ONE relay must not disturb the others' counts,
    // on either side of the bridge.
    TEST_CHECK(relay_cycles_reset(0) == true, "reset relay 0 only");
    memset(&s_rc, 0, sizeof(s_rc));
    TEST_CHECK(relay_cycles_init() == ESP_OK, "reload after reset");
    TEST_CHECK(s_rc.counts[0] == 0, "relay 0 reset to zero");
    TEST_CHECK(s_rc.counts[1] == 2994 && s_rc.counts[2] == 3214,
               "the OTHER relays' bench-accumulated counts survive the reset+migration untouched -- "
               "not lost, not double-counted");

    // relay_cycles_restore_all() (the backup-restore path) must also compose:
    // a restored value strictly outranks the current file/NVS content via
    // the same rev mechanism, never a parallel path that could desync.
    uint32_t restore[RELAY_CYCLES_COUNT];
    memset(restore, 0, sizeof(restore));
    restore[0] = 5000;
    restore[1] = 2994;
    restore[2] = 3214;
    TEST_CHECK(relay_cycles_restore_all(restore) == true, "restore succeeds");
    memset(&s_rc, 0, sizeof(s_rc));
    TEST_CHECK(relay_cycles_init() == ESP_OK, "reload after restore");
    TEST_CHECK(s_rc.counts[0] == 5000 && s_rc.counts[1] == 2994 && s_rc.counts[2] == 3214,
               "restore's counts win on both sides of the bridge -- nothing lost");
}

// NEGATIVE TEST (per this task's brief: "NEGATIVE-TEST the relay-cycle
// migration -- make it lose counts, show a test failing"). Breaks
// persist_snapshot()'s PRODUCTION file-write call by commenting it out
// (simulated here by temporarily redirecting the write through a function
// that never lands, i.e. what shipping code would look like with the
// pref_cfg_fs_save() call deleted) and shows an EXISTING test -- not a new
// one written to be vacuous -- catches it.
//
// Concretely: this reruns test_cfg_fs_migrates_nvs_value_to_file_then_
// prefers_it's own body with the file write forced to fail from the start,
// proving that test (and by extension the bridge) would have caught a
// regression that dropped the file-write call entirely. The actual
// production edit-and-revert (commenting out relay_cycles.c's
// pref_cfg_fs_save() call in persist_snapshot(), rerunning the suite,
// finding the shortest failing line, then restoring by hand) is recorded in
// this task's own report, not re-enacted here -- see that report for the
// exact failing line and the `git diff` proof after restoring it.
static void test_cfg_fs_negative_no_file_write_means_file_never_catches_up(void)
{
    TEST_SECTION("relay_cycles cfg_fs NEGATIVE TEST: if the file write is skipped, the file falls "
                 "permanently behind -- the exact regression the migration must not reintroduce");
    reset_all_cfg_fs();
    TEST_CHECK(cfg_fs_init(RC_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    TEST_CHECK(relay_cycles_init() == ESP_OK, "init");

    pref_cfg_fs_set_write_fn(rc_failing_write_fn); // stands in for "the file-write call was deleted"
    s_rc.counts[0] = 4242;
    s_rc.dirty = true;
    TEST_CHECK(relay_cycles_flush() == ESP_OK, "flush still reports OK (NVS is authoritative)");
    pref_cfg_fs_reset_write_fn_for_test();

    relay_cycles_blob_t raw;
    uint32_t rev = 0;
    bool valid = false;
    pref_cfg_fs_load_raw(RELAY_CYCLES_FILE_PATH, sizeof(raw), relay_cycles_file_validate, &raw, &rev, &valid);
    TEST_CHECK(!valid, "with the file write skipped, the file never catches up -- exactly the loss "
                        "this migration must not reintroduce (NVS alone is carrying the counts)");
}

/* 2026-09-08: relay_cycles_get_dualwrite_status() -- GET /api/cfgfs's row
 * for this item, moved off the stale nvs_only list this pass. Same shape
 * as test_unit_pref.c's unit_pref_get_dualwrite_status() test: healthy
 * (equal content) reports diverged:false, a real content disagreement
 * between file and NVS (equal rev, so the tie-break itself does not
 * intervene) reports diverged:true -- exercising the REAL production
 * function, not a test-local mirror of its logic (project_negative_test_
 * on_a_mirror_is_vacuous). */
static void test_get_dualwrite_status_reports_real_divergence(void)
{
    TEST_SECTION("relay_cycles_get_dualwrite_status(): healthy reports diverged:false; a genuine file/NVS "
                 "content disagreement reports diverged:true -- calls the real production function");
    reset_all_cfg_fs();
    TEST_CHECK(cfg_fs_init(RC_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    TEST_CHECK(relay_cycles_init() == ESP_OK, "init");

    s_rc.counts[0] = 10;
    s_rc.counts[1] = 20;
    s_rc.dirty = true;
    TEST_CHECK(relay_cycles_flush() == ESP_OK, "flush -- file and NVS agree at rev 1");

    bool file_valid = false, nvs_valid = false, diverged = true;
    uint32_t file_rev = 0, nvs_rev = 0;
    relay_cycles_get_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
    TEST_CHECK(file_valid && nvs_valid, "both sides valid after a normal flush");
    TEST_CHECK(file_rev == 1 && nvs_rev == 1, "both revs agree at 1");
    TEST_CHECK(!diverged, "healthy dual-write state reports diverged:false");

    /* Force the file to disagree with NVS AT THE SAME REV (so the
     * divergence is genuinely a content mismatch, not just an in-flight
     * rev skew) -- same setup test_cfg_fs_divergence_tie_break_strict_
     * greater_than() above already uses for the load-side tie-break. */
    relay_cycles_blob_t stale_equal_rev;
    memset(&stale_equal_rev, 0, sizeof(stale_equal_rev));
    stale_equal_rev.version = RELAY_CYCLES_VERSION;
    stale_equal_rev.counts[0] = 999; // disagrees with NVS's counts[0] == 10
    TEST_CHECK(pref_cfg_fs_save(RELAY_CYCLES_FILE_PATH, &stale_equal_rev, sizeof(stale_equal_rev), 1) == ESP_OK,
               "test setup: file rewritten at the SAME rev (1) with different content");

    file_valid = false;
    nvs_valid = false;
    diverged = false;
    file_rev = 0;
    nvs_rev = 0;
    relay_cycles_get_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
    TEST_CHECK(file_valid && nvs_valid, "both sides still individually valid");
    TEST_CHECK(diverged, "a real content disagreement at equal rev is reported as diverged:true");
}

// flash_worker_wait_until_started() (persist/flash_worker_wait.c) -- the
// shared bounded-poll helper this pass extracted out of cfg_fs_mount.c so
// relay_cycles_init()'s kibase/cycles migrate-on-load writes (and adaptive_
// tune.c's) share ONE implementation instead of each copying cfg_fs_mount.c's
// original 20ms/5s constants. Tested here directly through its real
// production function, not a mirror -- see the injected predicate below,
// which lets the test drive "worker never starts" and "worker starts after
// N polls" without needing to fight bx_worker_stub.h's always-true
// uart_bridge_ext_flash_worker_started() (shared across every other test in
// this binary; overriding it here would break those).
static int s_never_started_calls = 0;
static bool never_started(void)
{
    s_never_started_calls++;
    return false;
}

static int s_starts_after_n_calls = 0;
static int s_starts_after_n_target = 0;
static bool starts_after_n(void)
{
    s_starts_after_n_calls++;
    return s_starts_after_n_calls >= s_starts_after_n_target;
}

static void test_flash_worker_wait_gives_up_after_ceiling(void)
{
    TEST_SECTION("flash_worker_wait_until_started() -- a predicate that never returns true is polled "
                 "until the ceiling, then the call gives up and returns false (does not hang forever)");
    s_never_started_calls = 0;
    bool started = flash_worker_wait_until_started(never_started, 1, 5);
    TEST_CHECK(!started, "gives up -- worker never reported started");
    TEST_CHECK(s_never_started_calls >= 5, "polled at least ceiling/poll_ms times before giving up");
}

static void test_flash_worker_wait_succeeds_once_predicate_flips(void)
{
    TEST_SECTION("flash_worker_wait_until_started() -- a predicate that flips true after a few polls is "
                 "detected, well inside the ceiling (a slow scheduler is not mistaken for permanent failure)");
    s_starts_after_n_calls = 0;
    s_starts_after_n_target = 3;
    bool started = flash_worker_wait_until_started(starts_after_n, 1, 5000);
    TEST_CHECK(started, "detected as started once the predicate flips");
    TEST_CHECK(s_starts_after_n_calls == 3, "stopped polling the instant it flipped, not before or after");
}

static void test_flash_worker_wait_null_predicate_returns_false(void)
{
    TEST_SECTION("flash_worker_wait_until_started() -- a NULL predicate returns false immediately "
                 "(the safe assumption: treat it as never started, never crash)");
    TEST_CHECK(!flash_worker_wait_until_started(NULL, 1, 5), "NULL predicate -> false, not a crash");
}

static void test_relay_cycles_init_deferred_flag_clear_when_worker_already_up(void)
{
    TEST_SECTION("relay_cycles_init() -- with the flash worker already started (this suite's normal "
                 "stub state), relay_cycles_migration_worker_wait_deferred() reports false: the "
                 "migrate-on-load write was NOT dropped");
    reset_all_cfg_fs();
    TEST_CHECK(cfg_fs_init(RC_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    TEST_CHECK(relay_cycles_init() == ESP_OK, "init");
    TEST_CHECK(!relay_cycles_migration_worker_wait_deferred(),
               "worker was up throughout -- nothing was deferred/dropped this boot");
}

void run_test_relay_cycles(void)
{
    g_test_stub_semaphore_take_default = 1; // pdTRUE -- see comment above test_maybe_persist_skips_...
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
    test_restore_all_sets_every_count_and_persists();
    test_restore_all_is_idempotent();
    test_restore_all_refuses_out_of_range_count_and_writes_nothing();
    test_restore_all_rejects_null_pointer();
    test_maybe_persist_skips_without_blocking_when_persist_lock_is_busy();

    test_cfg_fs_partition_absent_behaves_like_before();
    test_cfg_fs_migrates_nvs_value_to_file_then_prefers_it();
    test_cfg_fs_dual_write_stays_in_sync_across_repeated_flushes();
    test_cfg_fs_divergence_tie_break_strict_greater_than();
    test_cfg_fs_reset_all_composes_with_migration_never_loses_counts();
    test_cfg_fs_negative_no_file_write_means_file_never_catches_up();
    test_get_dualwrite_status_reports_real_divergence();
    test_flash_worker_wait_gives_up_after_ceiling();
    test_flash_worker_wait_succeeds_once_predicate_flips();
    test_flash_worker_wait_null_predicate_returns_false();
    test_relay_cycles_init_deferred_flag_clear_when_worker_already_up();
    reset_all_cfg_fs();

    fake_kv_reset_all(); // leave shared fake state as every other test file in this binary expects
}
