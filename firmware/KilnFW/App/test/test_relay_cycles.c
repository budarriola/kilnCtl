// Host tests for App/drivers/persist/relay_cycles.c -- specifically persist_snapshot()'s
// PSRAM-stack guard (DRAM_PSRAM_PLAN.md section 7: relay_cycles_maybe_persist()/
// relay_cycles_flush() are called directly from profile_executor's tick and
// stop paths, the same task that plan names as its highest-care relocation
// candidate -- this module had no guard until this test's companion change
// added one, matching kiln_cfg_store.c's/safety_cfg_store.c's/run_state.c's
// caller_stack_is_external() pattern).
//
// relay_cycles.c is #included directly (same convention as
// test_crash_report.c's #include of crash_report.c) so this file can reach
// persist_snapshot() directly and exercise it with
// fake_kv.h's RAM-backed hal_kv fake plus a real cfg_fs scratch mount, plus fake_kv_set_write_safe_here() to simulate a
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
#include <stddef.h>
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

static void mount_cfg_fresh(void);

static void reset_all(void)
{
    // Saves are cfg-file-only since the dual-write close, so every test that
    // persists needs a real mounted cfg scratch (this also resets the fake NVS,
    // re-inits the partition and zeroes s_rc).
    mount_cfg_fresh();
    s_rc.counts[0] = 42;
    s_rc.counts[1] = 7;
    s_rc.dirty = true;
}

/* Forward declarations: the cfg_fs scratch mount lives further down this file. */
static void reset_all_cfg_fs(void);
static void mount_cfg_fresh(void);
static void read_cycles_file(relay_cycles_blob_t *out, uint32_t *rev, bool *valid);

/* Stages the blob a LEGACY (pre dual-write-close) firmware would have left in
 * NVS -- relay_cycles.c no longer has any NVS writer for it, so a test that
 * needs a legacy NVS-only board writes the bytes itself. */
static hal_status_t stage_legacy_nvs_blob(uint32_t rev)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return err;
    }
    relay_cycles_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.version = RELAY_CYCLES_VERSION;
    memcpy(blob.counts, s_rc.counts, sizeof(blob.counts));
    memcpy(blob.types, s_rc.types, sizeof(blob.types));
    memcpy(blob.rated_overrides, s_rc.rated_overrides, sizeof(blob.rated_overrides));
    err = hal_kv_set_blob(&h, NVS_KEY_CYCLES, &blob, sizeof(blob));
    if (err == HAL_OK) {
        err = hal_kv_set_u32(&h, NVS_KEY_CYCLES_REV, rev);
    }
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return err;
}

static void test_persist_snapshot_refuses_when_calling_stack_is_external_ram(void)
{
    TEST_SECTION("relay_cycles persist_snapshot -- refuses (does not crash) when called with a "
                 "PSRAM stack underneath it (DRAM_PSRAM_PLAN.md section 7 safety net)");
    reset_all();

    fake_kv_set_write_safe_here(false); // simulate being called from a PSRAM-stacked task

    reset_persist_job_arg_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.counts[0] = 42;
    snap.rev = 1;
    esp_err_t err = persist_snapshot(&snap);

    TEST_CHECK(err == ESP_ERR_INVALID_STATE,
               "the wrong-task guard refuses with a diagnosable error, not a crash -- exactly "
               "the class of bug (a flash write reached from a PSRAM-stack task) this net exists "
               "to catch before a future relocation of profile_executor (DRAM_PSRAM_PLAN.md "
               "section 7) makes it reachable for real");
    TEST_CHECK(s_rc.dirty == true, "a refused write must not clear the dirty flag -- the counts "
                                   "are still unpersisted and must be retried later");

    // The refused write wrote nothing: no NVS namespace either.
    hal_kv_handle_t h;
    hal_status_t open_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    TEST_CHECK(open_err == HAL_NOT_FOUND, "the refused write left no NVS namespace behind");

    fake_kv_set_write_safe_here(true); // leave shared fake state as every other test expects
}

static void test_persist_snapshot_proceeds_normally_on_an_internal_ram_stack(void)
{
    TEST_SECTION("relay_cycles persist_snapshot -- proceeds normally when the calling task's "
                 "stack is internal RAM, and writes the cfg file ONLY (NVS untouched)");
    mount_cfg_fresh();

    reset_persist_job_arg_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.counts[0] = 42;
    snap.counts[1] = 7;
    snap.rev = 1;
    esp_err_t err = persist_snapshot(&snap);

    TEST_CHECK(err == ESP_OK, "the guard does not fire on an internal-RAM stack -- the write "
                              "proceeds and lands in the cfg file");

    relay_cycles_blob_t readback;
    uint32_t rev = 0;
    bool valid = false;
    read_cycles_file(&readback, &rev, &valid);
    TEST_CHECK(valid && rev == 1, "the file round-trips at the snapshot's rev");
    TEST_CHECK(readback.counts[0] == 42 && readback.counts[1] == 7,
               "the persisted counts are the ones that were passed in, unmodified");

    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_NOT_FOUND,
               "NVS was never written -- the dual-write window is closed");
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
    TEST_CHECK(b.cycles == 1000000,
               "cycles must still be the real live count for ssr -- only percent/rated/tier/"
               "has_budget are null (dashboard_status_http.c), not cycles itself");
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
    mount_cfg_fresh();
    TEST_CHECK(relay_cycles_init() == ESP_OK, "init");
    s_rc.counts[0] = 42;
    s_rc.counts[1] = 7;

    TEST_CHECK(RELAY_CYCLES_SAFETY_INDEX == KILN_IO_RELAY_COUNT,
               "the safety slot is the one right after the four heater relays");

    relay_cycles_add(0x0F, 5); // all four heater relays, must NOT touch the safety slot
    relay_cycles_note_safety_edge();
    relay_cycles_note_safety_edge();
    relay_cycles_note_safety_edge();

    TEST_CHECK(s_rc.counts[RELAY_CYCLES_SAFETY_INDEX] == 3, "three edges noted, one each call");
    TEST_CHECK(s_rc.counts[0] == 42 + 5, "relay_cycles_add() still only touches the four heater slots");

    TEST_CHECK(relay_cycles_flush() == ESP_OK, "persist succeeds with the fifth slot populated");

    memset(&s_rc, 0, sizeof(s_rc));
    relay_cycles_blob_t blob;
    uint32_t rev = 0;
    bool valid = false;
    read_cycles_file(&blob, &rev, &valid);
    TEST_CHECK(valid, "the v2 blob round-trips at its full size through the cfg file");
    TEST_CHECK(blob.counts[RELAY_CYCLES_SAFETY_INDEX] == 3,
               "the persisted blob carries the safety slot's count, not just the four heater ones");
}

static void test_v1_blob_migrates_to_v2(void)
{
    TEST_SECTION("relay_cycles_init -- a v1 blob (bare 4-count array, no types, no fifth slot) "
                 "migrates to v2: existing counts kept, type defaults to ssr, fifth slot starts at 0");
    mount_cfg_fresh();

    // Write a v1-shaped blob directly, bypassing the
    // production writer (which only ever writes the current version) -- this simulates a board that
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

    {
        relay_cycles_blob_t mig;
        uint32_t mig_rev = 0;
        bool mig_valid = false;
        read_cycles_file(&mig, &mig_rev, &mig_valid);
        TEST_CHECK(mig_valid && mig.counts[0] == 111 && mig.counts[1] == 222,
                   "the v1 counts were migrated into the cfg file (the only store written now)");
    }

    fake_kv_reset_all();
}

// --- migrate_from_default_partition() must only run into an EMPTY kiln partition.
// The old default-partition copy is never erased, so an unconditional copy would
// overwrite live wear counts with the stale v1 copy on every boot (752f9ac0 forbids
// moving a count downward).

static void write_default_partition_v1(uint32_t c0, uint32_t c1)
{
    hal_kv_init_partition(NULL);
    relay_cycles_blob_v1_t v1;
    memset(&v1, 0, sizeof(v1));
    v1.version = 1;
    v1.counts[0] = c0;
    v1.counts[1] = c1;
    hal_kv_handle_t hw;
    TEST_CHECK(hal_kv_open(&hw, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, NULL) == HAL_OK, "open default partition");
    TEST_CHECK(hal_kv_set_blob(&hw, NVS_KEY_CYCLES, &v1, sizeof(v1)) == HAL_OK, "write stale v1 copy");
    TEST_CHECK(hal_kv_commit(&hw) == HAL_OK, "commit stale v1 copy");
    hal_kv_close(&hw);
}

static void test_migration_skipped_when_kiln_partition_already_has_blob(void)
{
    TEST_SECTION("relay_cycles_init -- kiln partition already holds a current blob with higher counts, "
                 "default partition holds a stale v1 copy: live counts win, never overwritten");
    mount_cfg_fresh();
    write_default_partition_v1(10, 20);

    memset(&s_rc, 0, sizeof(s_rc));
    s_rc.counts[0] = 5000;
    s_rc.counts[1] = 7000;
    s_rc.types[0] = RELAY_TYPE_CONTACTOR;
    s_rc.rated_overrides[0] = 77;
    TEST_CHECK(stage_legacy_nvs_blob(1) == HAL_OK, "stage the live current-version blob in NVS (legacy writer)");

    memset(&s_rc, 0, sizeof(s_rc));
    TEST_CHECK(relay_cycles_init() == ESP_OK, "init succeeds");
    TEST_CHECK(s_rc.counts[0] == 5000 && s_rc.counts[1] == 7000,
               "live counts survive; the stale default-partition v1 copy did not overwrite them");
    TEST_CHECK(s_rc.types[0] == RELAY_TYPE_CONTACTOR && s_rc.rated_overrides[0] == 77,
               "live relay type/override survive too");
    {
        relay_cycles_blob_t mig;
        uint32_t mig_rev = 0;
        bool mig_valid = false;
        read_cycles_file(&mig, &mig_rev, &mig_valid);
        TEST_CHECK(mig_valid && mig.counts[0] == 5000, "the live legacy NVS blob was migrated into the cfg file");
    }
    fake_kv_reset_all();
}

static void test_migration_still_runs_on_fresh_kiln_partition(void)
{
    TEST_SECTION("relay_cycles_init -- kiln partition empty, default partition holds v1: still migrates");
    mount_cfg_fresh();
    write_default_partition_v1(111, 222);

    memset(&s_rc, 0, sizeof(s_rc));
    TEST_CHECK(relay_cycles_init() == ESP_OK, "init succeeds");
    TEST_CHECK(s_rc.counts[0] == 111 && s_rc.counts[1] == 222,
               "counts migrated from the default partition on a board with no kiln copy");
    {
        relay_cycles_blob_t mig;
        uint32_t mig_rev = 0;
        bool mig_valid = false;
        read_cycles_file(&mig, &mig_rev, &mig_valid);
        TEST_CHECK(mig_valid && mig.counts[0] == 111, "and landed in the cfg file");
    }
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

    relay_cycles_blob_t blob;
    uint32_t blob_rev = 0;
    bool blob_valid = false;
    read_cycles_file(&blob, &blob_rev, &blob_valid);
    TEST_CHECK(blob_valid, "the cfg file round-trips");
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

// docs/audits/review_crash_gate_followups_f07ad24d_2026-09-15.md: neither
// relay_cycles_reset_timeout() nor its crash_report.c precedent had any test
// coverage at all -- the busy-worker/timeout path is routine (any profile
// save or config write occupies the flash worker) and nothing in this file
// would have caught a defect in it. Against the parent (f07ad24d) this test
// fails for a behavioural reason, not an API-missing reason: the busy-worker
// branch must return false/timed_out=true/dispatch_count unchanged, and a
// broken tail (e.g. one that still tries to persist on a busy worker, or
// that clears `dirty` on a timeout) would show up as a wrong value here, not
// a compile error.
static void test_reset_timeout_busy_worker_reports_timeout(void)
{
    TEST_SECTION("relay_cycles_reset_timeout -- a busy flash worker is reported as a timeout: "
                 "nothing is dispatched, the in-RAM count is still zeroed from the snapshot, and "
                 "dirty is left set so a later flush still persists it");
    reset_all();
    s_rc.counts[2] = 123;

    s_stub_bx_busy = true; // model the worker already running someone else's job
    unsigned dispatch_before = s_stub_dispatch_count;

    bool timed_out = false;
    bool ok = relay_cycles_reset_timeout(2, 300, &timed_out);

    s_stub_bx_busy = false; // leave shared stub state as every other test expects

    TEST_CHECK(ok == false, "a timed-out acquire must not report success");
    TEST_CHECK(timed_out == true, "the caller must be told this was specifically a timeout, not "
                                   "some other persist failure");
    TEST_CHECK(s_rc.counts[2] == 0, "the count is zeroed in RAM by the snapshot step, which runs "
                                     "BEFORE the bounded dispatch attempt, independent of whether "
                                     "the dispatch itself ever ran");
    TEST_CHECK(s_rc.dirty == true, "dirty must be left set -- the write never happened, so the "
                                    "next periodic persist must still retry it");
    TEST_CHECK(s_stub_dispatch_count == dispatch_before, "a busy worker must mean NOTHING was "
                                                          "dispatched -- if this incremented, the "
                                                          "bounded stub ran the job anyway instead "
                                                          "of reporting ESP_ERR_TIMEOUT");
}

static void test_reset_timeout_idle_worker_succeeds(void)
{
    TEST_SECTION("relay_cycles_reset_timeout -- an idle flash worker dispatches and persists "
                 "exactly like the unbounded relay_cycles_reset(), pinning the shared tail "
                 "against N1-style drift (a lost log field or a wrong dirty/rev update)");
    reset_all();
    s_rc.counts[2] = 123;

    s_stub_bx_busy = false;
    unsigned dispatch_before = s_stub_dispatch_count;

    bool timed_out = false;
    bool ok = relay_cycles_reset_timeout(2, 300, &timed_out);

    TEST_CHECK(ok == true, "an idle worker must let the reset actually succeed");
    TEST_CHECK(timed_out == false, "a successful dispatch is not a timeout");
    TEST_CHECK(s_rc.counts[2] == 0, "the count is zeroed");
    TEST_CHECK(s_rc.dirty == false, "the dispatched persist actually landed (dirty cleared) -- "
                                     "the same postcondition relay_cycles_reset() itself has");
    TEST_CHECK(s_stub_dispatch_count == dispatch_before + 1, "exactly one job was dispatched "
                                                              "through the flash worker");

    relay_cycles_blob_t blob;
    uint32_t blob_rev = 0;
    bool blob_valid = false;
    read_cycles_file(&blob, &blob_rev, &blob_valid);
    TEST_CHECK(blob_valid, "readback of the persisted cfg file succeeds");
    TEST_CHECK(blob.counts[2] == 0, "the zeroed count actually reached the store, not just RAM");
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
    relay_cycles_blob_t blob;
    uint32_t blob_rev = 0;
    bool blob_valid = false;
    read_cycles_file(&blob, &blob_rev, &blob_valid);
    TEST_CHECK(blob_valid, "the cfg file round-trips");
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

    relay_cycles_blob_t blob;
    uint32_t blob_rev = 0;
    bool blob_valid = false;
    read_cycles_file(&blob, &blob_rev, &blob_valid);
    TEST_CHECK(blob_valid, "the cfg file round-trips");
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
    TEST_CHECK(relay_cycles_restore_all(backup, 0, NULL) == true, "restore succeeds on the normal path");
    for (uint8_t i = 0; i < RELAY_CYCLES_COUNT; i++) {
        TEST_CHECK(s_rc.counts[i] == backup[i], "every restored count lands in RAM");
    }

    relay_type_t type;
    uint32_t override_val;
    relay_cycles_get_type(0, &type, &override_val);
    TEST_CHECK(type == RELAY_TYPE_CONTACTOR && override_val == 55,
               "type/override are untouched by a restore, same convention as relay_cycles_reset()");

    TEST_CHECK(s_rc.dirty == false, "the dispatched persist actually landed (dirty cleared)");

    relay_cycles_blob_t blob;
    uint32_t blob_rev = 0;
    bool blob_valid = false;
    read_cycles_file(&blob, &blob_rev, &blob_valid);
    TEST_CHECK(blob_valid, "the cfg file round-trips");
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
    TEST_CHECK(relay_cycles_restore_all(backup, 0, NULL) == true, "first restore succeeds");
    relay_cycles_blob_t blob_first;
    uint32_t rev_first = 0;
    bool valid_first = false;
    read_cycles_file(&blob_first, &rev_first, &valid_first);
    TEST_CHECK(valid_first, "first restore's file is valid");

    TEST_CHECK(relay_cycles_restore_all(backup, 0, NULL) == true, "second restore of the same archive succeeds");
    relay_cycles_blob_t blob_second;
    uint32_t rev_second = 0;
    bool valid_second = false;
    read_cycles_file(&blob_second, &rev_second, &valid_second);
    TEST_CHECK(valid_second, "second restore's file is valid");

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

    TEST_CHECK(relay_cycles_restore_all(backup, 0, NULL) == false,
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
    TEST_CHECK(relay_cycles_restore_all(NULL, 0, NULL) == false, "NULL is refused");
}

// MONOTONIC GUARD (2026-09-20 backup/restore review finding): the positive
// half -- a legitimate HIGHER value must still be accepted verbatim, with
// nothing reported as clamped.
static void test_restore_all_accepts_higher_value_no_clamp(void)
{
    TEST_SECTION("relay_cycles_restore_all -- a value ABOVE the board's live count is accepted "
                 "verbatim and reported as not clamped");
    reset_all();
    s_rc.counts[0] = 100;
    s_rc.counts[1] = 200;

    uint32_t backup[RELAY_CYCLES_COUNT];
    for (uint8_t i = 0; i < RELAY_CYCLES_COUNT; i++) {
        backup[i] = 9000u + i; // above every live count set above (and above the zeroed defaults)
    }
    relay_cycles_restore_result_t result;
    memset(&result, 0, sizeof(result));
    TEST_CHECK(relay_cycles_restore_all(backup, 0, &result) == true,
               "restore of a higher value succeeds");
    for (uint8_t i = 0; i < RELAY_CYCLES_COUNT; i++) {
        TEST_CHECK(s_rc.counts[i] == backup[i], "the higher requested value is applied exactly");
        TEST_CHECK(result.entries[i].clamped == false, "not reported as clamped");
        TEST_CHECK(result.entries[i].requested == backup[i] && result.entries[i].applied == backup[i],
                   "requested == applied for an unclamped relay");
    }
}

// MONOTONIC GUARD, negative half: a value BELOW the board's live count must
// be refused in effect (clamped back up to the live count, never applied),
// and the clamp must be visible -- both numbers, named by relay -- in
// out_result. This is the exact defect the backup/restore review found:
// relay_cycles_restore_all() used to accept this silently.
static void test_restore_all_clamps_value_below_live_count(void)
{
    TEST_SECTION("relay_cycles_restore_all -- NEGATIVE TEST: a value BELOW the board's live count is "
                 "clamped back UP to the live count, never silently applied, and the clamp is reported "
                 "with both the requested and applied numbers");
    reset_all();
    s_rc.counts[0] = 5000; // relay 0's live count is well above what the "archive" below claims
    s_rc.counts[1] = 300;
    s_rc.counts[2] = 10;
    s_rc.counts[3] = 10;
    s_rc.counts[4] = 10;

    uint32_t backup[RELAY_CYCLES_COUNT];
    backup[0] = 42; // stale/understated -- must be refused (clamped up), not applied
    backup[1] = 300; // equal to live -- not a lowering, must NOT be reported as clamped
    backup[2] = 11; // a real higher value -- must be applied exactly
    backup[3] = 10;
    backup[4] = 10;

    relay_cycles_restore_result_t result;
    memset(&result, 0, sizeof(result));
    TEST_CHECK(relay_cycles_restore_all(backup, 0, &result) == true,
               "restore call succeeds (a clamp is not a persist failure)");

    TEST_CHECK(s_rc.counts[0] == 5000,
               "relay 0's live count is NEVER moved downward -- this is the defect the 2026-09-20 "
               "backup/restore review found and this guard closes");
    TEST_CHECK(result.entries[0].clamped == true, "relay 0 is reported as clamped");
    TEST_CHECK(result.entries[0].requested == 42 && result.entries[0].applied == 5000,
               "the report names BOTH numbers: what was requested (42) and what was actually applied "
               "(5000, the live count) -- never a silent success");

    TEST_CHECK(s_rc.counts[1] == 300 && result.entries[1].clamped == false,
               "a value equal to the live count is not a lowering and is not reported as clamped");
    TEST_CHECK(s_rc.counts[2] == 11 && result.entries[2].clamped == false,
               "a genuinely higher value is applied exactly and not reported as clamped");
}

// The override: allow_lower_mask lets ONE named relay (a physically replaced
// one) restart below its live count, while every OTHER relay in the SAME
// call keeps the monotonic guard -- proves the override is per-relay, not
// all-or-nothing, and fails closed by default (bit unset = safe behaviour).
static void test_restore_all_allow_lower_mask_overrides_one_relay_only(void)
{
    TEST_SECTION("relay_cycles_restore_all -- allow_lower_mask permits an explicit, per-relay override "
                 "(a replaced relay restarting near zero) without weakening the guard for any other "
                 "relay in the same call");
    reset_all();
    s_rc.counts[0] = 5000; // the "replaced" relay -- caller explicitly permits lowering this one
    s_rc.counts[1] = 300;  // NOT permitted -- must still be clamped if the request tries to lower it

    uint32_t backup[RELAY_CYCLES_COUNT];
    backup[0] = 0;  // replaced relay restarting at zero
    backup[1] = 1;  // stale/understated -- allow_lower_mask does NOT cover this relay
    backup[2] = 0;
    backup[3] = 0;
    backup[4] = 0;

    relay_cycles_restore_result_t result;
    memset(&result, 0, sizeof(result));
    uint8_t allow_lower_mask = (uint8_t)(1u << 0); // relay 0 only
    TEST_CHECK(relay_cycles_restore_all(backup, allow_lower_mask, &result) == true, "restore succeeds");

    TEST_CHECK(s_rc.counts[0] == 0 && result.entries[0].clamped == false,
               "relay 0's explicit override is honored exactly -- accepted at 0, not clamped");
    TEST_CHECK(s_rc.counts[1] == 300 && result.entries[1].clamped == true,
               "relay 1 is NOT in the mask -- still clamped back up to its live count, unaffected by "
               "relay 0's override in the same call");
    TEST_CHECK(result.entries[1].requested == 1 && result.entries[1].applied == 300,
               "relay 1's clamp reports both numbers");
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

static void mount_cfg_fresh(void)
{
    reset_all_cfg_fs();
    TEST_CHECK(cfg_fs_init(RC_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
}

static void read_cycles_file(relay_cycles_blob_t *out, uint32_t *rev, bool *valid)
{
    memset(out, 0, sizeof(*out));
    pref_cfg_fs_load_raw(RELAY_CYCLES_FILE_PATH, sizeof(*out), relay_cycles_file_validate, out, rev, valid);
}

static esp_err_t rc_failing_write_fn(const char *rel_path, const void *data, size_t len)
{
    (void)rel_path;
    (void)data;
    (void)len;
    return ESP_FAIL;
}

static bool rc_nvs_namespace_absent(void)
{
    hal_kv_handle_t h;
    return hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION) == HAL_NOT_FOUND;
}

static void test_cfg_fs_partition_absent_fails_loud_and_still_loads_legacy(void)
{
    TEST_SECTION("relay_cycles cfg_fs: no partition mounted -- a legacy NVS copy still loads, a save fails loud "
                 "and never falls back to NVS");
    reset_all_cfg_fs();
    TEST_CHECK(!cfg_fs_is_available(), "cfg_fs never mounted in this test");

    s_rc.counts[0] = 55;
    TEST_CHECK(stage_legacy_nvs_blob(1) == HAL_OK, "stage the legacy NVS blob");
    memset(&s_rc, 0, sizeof(s_rc));

    TEST_CHECK(relay_cycles_init() == ESP_OK, "init succeeds with no `cfg` partition mounted");
    TEST_CHECK(s_rc.counts[0] == 55, "the legacy NVS-only board still loads its count");

    s_rc.counts[0] = 56;
    s_rc.dirty = true;
    TEST_CHECK(relay_cycles_flush() != ESP_OK, "flush fails loud: there is nowhere to persist");
    TEST_CHECK(s_rc.dirty, "the counts stay dirty for a retry");

    memset(&s_rc, 0, sizeof(s_rc));
    TEST_CHECK(relay_cycles_init() == ESP_OK, "re-init");
    TEST_CHECK(s_rc.counts[0] == 55, "the NVS copy was NOT overwritten by the failed save (no fallback)");
}

static void test_cfg_fs_migrates_nvs_value_to_file_then_prefers_it(void)
{
    TEST_SECTION("relay_cycles cfg_fs: legacy NVS value migrates to the file; a later boot needs only the file");
    mount_cfg_fresh();

    s_rc.counts[0] = 10;
    s_rc.counts[1] = 20;
    TEST_CHECK(stage_legacy_nvs_blob(3) == HAL_OK, "stage the legacy NVS blob at rev 3");
    memset(&s_rc, 0, sizeof(s_rc));

    TEST_CHECK(relay_cycles_init() == ESP_OK, "first boot after the upgrade");
    TEST_CHECK(s_rc.counts[0] == 10 && s_rc.counts[1] == 20, "legacy counts load");

    relay_cycles_blob_t mig;
    uint32_t mig_rev = 0;
    bool mig_valid = false;
    read_cycles_file(&mig, &mig_rev, &mig_valid);
    TEST_CHECK(mig_valid && mig.counts[0] == 10 && mig.counts[1] == 20,
               "init migrated the legacy value into the cfg file");

    // The legacy copy vanishes (erased, or a board that never had it): the file alone suffices.
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    memset(&s_rc, 0, sizeof(s_rc));
    TEST_CHECK(relay_cycles_init() == ESP_OK, "later boot, no NVS copy");
    TEST_CHECK(s_rc.counts[0] == 10 && s_rc.counts[1] == 20, "counts reload from the file alone");

    s_rc.counts[0] = 11;
    s_rc.dirty = true;
    TEST_CHECK(relay_cycles_flush() == ESP_OK, "a later save lands");
    uint32_t after_rev = 0;
    bool after_valid = false;
    relay_cycles_blob_t after;
    read_cycles_file(&after, &after_rev, &after_valid);
    TEST_CHECK(after_valid && after.counts[0] == 11 && after_rev == mig_rev + 1,
               "the save advanced the file rev by one");
    TEST_CHECK(rc_nvs_namespace_absent(), "and never recreated an NVS copy");
}

static void test_cfg_fs_dual_write_stays_in_sync_across_repeated_flushes(void)
{
    TEST_SECTION("relay_cycles cfg_fs: repeated flushes advance the file rev; NVS is never written");
    mount_cfg_fresh();
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
    TEST_CHECK(rc_nvs_namespace_absent(), "no NVS copy was ever written");

    memset(&s_rc, 0, sizeof(s_rc));
    TEST_CHECK(relay_cycles_init() == ESP_OK, "reload");
    TEST_CHECK(s_rc.counts[0] == 300, "the file alone carries the latest value");
}

static void test_cfg_fs_divergence_tie_break_strict_greater_than(void)
{
    TEST_SECTION("relay_cycles cfg_fs: legacy NVS copy vs file -- higher rev wins, EQUAL rev goes to NVS");
    mount_cfg_fresh();
    TEST_CHECK(relay_cycles_init() == ESP_OK, "init");

    s_rc.counts[0] = 1;
    s_rc.dirty = true;
    TEST_CHECK(relay_cycles_flush() == ESP_OK, "file rev 1 written");

    // A rolled-back/legacy firmware wrote NVS at a HIGHER rev.
    memset(&s_rc, 0, sizeof(s_rc));
    s_rc.counts[0] = 2;
    TEST_CHECK(stage_legacy_nvs_blob(2) == HAL_OK, "stage a legacy NVS blob at rev 2");
    memset(&s_rc, 0, sizeof(s_rc));
    TEST_CHECK(relay_cycles_init() == ESP_OK, "reload");
    TEST_CHECK(s_rc.counts[0] == 2, "NVS (higher rev) wins -- the older rev-1 file is NOT trusted");

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

static void test_cfg_write_failure_is_loud_and_rev_unadvanced(void)
{
    TEST_SECTION("relay_cycles cfg_fs: a failed cfg write is reported, leaves the old file and rev intact, "
                 "and is NOT retried against NVS");
    mount_cfg_fresh();
    TEST_CHECK(relay_cycles_init() == ESP_OK, "init");
    s_rc.counts[0] = 1;
    s_rc.dirty = true;
    TEST_CHECK(relay_cycles_flush() == ESP_OK, "first save lands at rev 1");
    TEST_CHECK(s_rc.rev == 1, "in-RAM rev is 1");

    pref_cfg_fs_set_write_fn(rc_failing_write_fn);
    s_rc.counts[0] = 2;
    s_rc.dirty = true;
    TEST_CHECK(relay_cycles_flush() != ESP_OK, "flush reports the failed cfg write");
    pref_cfg_fs_reset_write_fn_for_test();
    TEST_CHECK(s_rc.dirty, "still dirty so the next persist retries");
    TEST_CHECK(s_rc.rev == 1, "rev did not advance without a verified write");

    relay_cycles_blob_t raw;
    uint32_t rev = 0;
    bool valid = false;
    read_cycles_file(&raw, &rev, &valid);
    TEST_CHECK(valid && rev == 1 && raw.counts[0] == 1, "the previous file is intact");
    TEST_CHECK(rc_nvs_namespace_absent(), "nothing fell back to NVS");

    TEST_CHECK(relay_cycles_flush() == ESP_OK, "the retry lands");
    read_cycles_file(&raw, &rev, &valid);
    TEST_CHECK(valid && rev == 2 && raw.counts[0] == 2, "file now at rev 2 with the new count");
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
    TEST_CHECK(relay_cycles_restore_all(restore, 0x1Fu, NULL) == true, "restore succeeds");
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
    TEST_SECTION("relay_cycles cfg_fs NEGATIVE TEST: if the file write is skipped nothing is persisted "
                 "anywhere -- NVS is no longer a safety net, so the save must report failure");
    mount_cfg_fresh();
    TEST_CHECK(relay_cycles_init() == ESP_OK, "init");

    pref_cfg_fs_set_write_fn(rc_failing_write_fn); // stands in for "the file-write call was deleted"
    s_rc.counts[0] = 4242;
    s_rc.dirty = true;
    TEST_CHECK(relay_cycles_flush() != ESP_OK, "flush reports failure (never a silent OK)");
    pref_cfg_fs_reset_write_fn_for_test();

    relay_cycles_blob_t raw;
    uint32_t rev = 0;
    bool valid = false;
    pref_cfg_fs_load_raw(RELAY_CYCLES_FILE_PATH, sizeof(raw), relay_cycles_file_validate, &raw, &rev, &valid);
    TEST_CHECK(!valid, "with the file write skipped there is no file");
    TEST_CHECK(rc_nvs_namespace_absent(), "and no NVS fallback either");
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
    TEST_CHECK(relay_cycles_flush() == ESP_OK, "flush -- file written at rev 1, NVS untouched");

    bool file_valid = false, nvs_valid = false, diverged = true;
    uint32_t file_rev = 0, nvs_rev = 0;
    relay_cycles_get_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
    TEST_CHECK(file_valid && !nvs_valid, "only the file is valid after a normal cfg-only flush");
    TEST_CHECK(file_rev == 1, "file rev is 1");
    TEST_CHECK(!diverged, "no NVS copy means nothing to diverge from");

    /* A legacy writer leaves an NVS copy at the SAME rev with different content. */
    {
        uint32_t keep = s_rc.counts[0];
        s_rc.counts[0] = 999;
        TEST_CHECK(stage_legacy_nvs_blob(1) == HAL_OK, "stage a legacy NVS blob at rev 1, count 999");
        s_rc.counts[0] = keep;
    }
    relay_cycles_get_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
    TEST_CHECK(file_valid && nvs_valid && nvs_rev == 1, "both sides valid, equal rev");
    TEST_CHECK(diverged, "equal rev with differing content is reported (raw) diverged:true");
}

/* 2026-10-04 bench finding: relay_cycles_blob_t has padding after `version`
 * and after `types`. relay_cycles_init() handed pref_cfg_fs_resolve() an
 * un-memset candidate (stack garbage in the padding), so resolve() saw
 * "differs" at EQUAL revs, logged DIVERGED and rewrote the file, and
 * relay_cycles_get_dualwrite_status()'s whole-struct memcmp then flagged the
 * file-vs-NVS padding difference on every boot. Padding is not data. */
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void rc_scribble_stack(void)
{
    volatile unsigned char junk[4096];
    for (size_t i = 0; i < sizeof(junk); i++) {
        junk[i] = 0xA5;
    }
    (void)junk[100];
}

/* Overwrite every padding byte of `b` (the gaps between the declared members) with `pad`. */
static void rc_fill_padding(relay_cycles_blob_t *b, unsigned char pad)
{
    unsigned char *raw = (unsigned char *)b;
    for (size_t i = offsetof(relay_cycles_blob_t, version) + 1; i < offsetof(relay_cycles_blob_t, counts); i++) {
        raw[i] = pad;
    }
    for (size_t i = offsetof(relay_cycles_blob_t, types) + RELAY_CYCLES_COUNT;
         i < offsetof(relay_cycles_blob_t, rated_overrides); i++) {
        raw[i] = pad;
    }
}

static void test_padding_is_not_data_status_and_init(void)
{
    TEST_SECTION("relay_cycles: file and NVS with identical contents but different padding bytes read as "
                 "in sync, and init() neither logs DIVERGED nor resolves (padding is not data)");
    reset_all_cfg_fs();
    TEST_CHECK(cfg_fs_init(RC_SCRATCH_BASE, NULL) == ESP_OK, "cfg_fs mounts");
    TEST_CHECK(relay_cycles_init() == ESP_OK, "init");
    s_rc.counts[0] = 10;
    s_rc.counts[1] = 20;
    s_rc.dirty = true;
    TEST_CHECK(relay_cycles_flush() == ESP_OK, "flush -- file written at rev 1");

    /* (a) status: file padding 0xAA, NVS padding 0x55, same content, same rev. */
    relay_cycles_blob_t fb;
    memset(&fb, 0, sizeof(fb));
    fb.version = RELAY_CYCLES_VERSION;
    fb.counts[0] = 10;
    fb.counts[1] = 20;
    relay_cycles_blob_t nb = fb;
    rc_fill_padding(&fb, 0xAA);
    rc_fill_padding(&nb, 0x55);
    TEST_CHECK(memcmp(&fb, &nb, sizeof(fb)) != 0, "test setup: the two blobs differ ONLY in padding bytes");
    TEST_CHECK(pref_cfg_fs_save(RELAY_CYCLES_FILE_PATH, &fb, sizeof(fb), 1) == ESP_OK, "file written, padding 0xAA");
    {
        hal_kv_handle_t h;
        TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK, "open NVS");
        TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_CYCLES, &nb, sizeof(nb)) == HAL_OK, "NVS written, padding 0x55");
        TEST_CHECK(hal_kv_set_u32(&h, NVS_KEY_CYCLES_REV, 1) == HAL_OK, "NVS rev 1");
        hal_kv_commit(&h);
        hal_kv_close(&h);
    }
    bool file_valid = false, nvs_valid = false, diverged = true;
    uint32_t file_rev = 0, nvs_rev = 0;
    relay_cycles_get_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
    TEST_CHECK(file_valid && nvs_valid && file_rev == 1 && nvs_rev == 1, "both sides valid at rev 1");
    TEST_CHECK(!diverged, "padding-only difference reads as in sync, diverged:false");

    /* a real content difference must still be caught */
    fb.counts[0] = 11;
    TEST_CHECK(pref_cfg_fs_save(RELAY_CYCLES_FILE_PATH, &fb, sizeof(fb), 1) == ESP_OK, "file content now differs");
    relay_cycles_get_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
    TEST_CHECK(diverged, "a real count difference is still diverged:true");

    /* (b) init: canonical zero-padded file (what this module writes) vs an NVS blob whose padding is
     * non-zero (an older firmware's writer). The NVS candidate init hands to resolve() is rebuilt
     * field-wise, so it must be zero-padded regardless of stack contents or NVS padding. */
    fb.counts[0] = 10;
    rc_fill_padding(&fb, 0x00);
    TEST_CHECK(pref_cfg_fs_save(RELAY_CYCLES_FILE_PATH, &fb, sizeof(fb), 1) == ESP_OK, "file zero-padded, rev 1");
    rc_scribble_stack();
    TEST_CHECK(relay_cycles_init() == ESP_OK, "init with padding-differing NVS");
    /* pref_cfg_fs.c is its own translation unit, so its DIVERGED log never reaches this file's log capture;
     * observe the effect instead. A divergent resolve adopts the NVS candidate and REWRITES the file from it,
     * so a garbage-padded candidate would leave 0xA5 padding in the file. A clean resolve leaves it zero. */
    {
        relay_cycles_blob_t after;
        memset(&after, 0xEE, sizeof(after));
        uint32_t arev = 0;
        bool avalid = false;
        pref_cfg_fs_load_raw(RELAY_CYCLES_FILE_PATH, sizeof(after), relay_cycles_file_validate, &after, &arev,
                             &avalid);
        const unsigned char *ar = (const unsigned char *)&after;
        bool pad_zero = true;
        for (size_t i = offsetof(relay_cycles_blob_t, version) + 1; i < offsetof(relay_cycles_blob_t, counts); i++) {
            pad_zero = pad_zero && ar[i] == 0;
        }
        for (size_t i = offsetof(relay_cycles_blob_t, types) + RELAY_CYCLES_COUNT;
             i < offsetof(relay_cycles_blob_t, rated_overrides); i++) {
            pad_zero = pad_zero && ar[i] == 0;
        }
        TEST_CHECK(avalid && pad_zero, "init did not resolve/rewrite the file from a garbage-padded candidate "
                                       "(file padding still zero)");
    }
    TEST_CHECK(s_rc.counts[0] == 10 && s_rc.counts[1] == 20 && s_rc.rev == 1, "counts and rev loaded unchanged");
    relay_cycles_get_dualwrite_status(&file_valid, &file_rev, &nvs_valid, &nvs_rev, &diverged);
    TEST_CHECK(!diverged, "still in sync after init");
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

// LOW-1, docs/audits/review_crash_gate_n1n3_120bba6f_2026-09-15.md: the shared
// snapshot head writes s_rc.counts[relay], and before this test's companion
// change it had no bounds check of its own -- it relied on a comment saying
// both of its callers validate first. Both DO, so the public entry points
// cannot reach the unguarded write and no public-API test can cover this;
// this file #includes relay_cycles.c, so the static helper is probed directly
// here, which is the only way to cover the guard that a THIRD caller added
// later would depend on.
//
// Asserted on s_rc.dirty / counts / rev rather than on memory past
// s_rc.counts[] deliberately: an out-of-bounds write is undefined behavior
// and its blast radius is not something a test can pin down. Clearing
// `dirty` is the deterministic, layout-independent consequence of the
// snapshot head having run at all, and it is also the one with real
// consequences -- a cleared `dirty` silently discards a pending persist.
static void test_reset_snapshot_refuses_out_of_range_relay(void)
{
    TEST_SECTION("relay_cycles_reset_snapshot -- an out-of-range relay is refused by the snapshot "
                 "helper itself, not only by its callers");
    reset_all();

    s_rc.dirty = true; // a persist is pending; a refused snapshot must not eat it
    uint32_t before[RELAY_CYCLES_COUNT];
    memcpy(before, s_rc.counts, sizeof(before));
    uint32_t rev_before = s_rc.rev;

    reset_persist_job_arg_t snap;
    uint32_t old_count = 0xA5A5A5A5u;

    TEST_CHECK(relay_cycles_reset_snapshot(RELAY_CYCLES_COUNT, &snap, &old_count) == false,
               "index == COUNT is refused by the snapshot helper itself");
    TEST_CHECK(old_count == 0xA5A5A5A5u,
               "the out param is left untouched on the refusal path");
    TEST_CHECK(s_rc.dirty == true,
               "a refused snapshot must not clear `dirty` -- that would silently discard a pending persist");
    TEST_CHECK(memcmp(before, s_rc.counts, sizeof(before)) == 0,
               "no count was modified by the refused call");
    TEST_CHECK(s_rc.rev == rev_before,
               "no dual-write rev was consumed by the refused call");

    TEST_CHECK(relay_cycles_reset_snapshot((unsigned)RELAY_CYCLES_COUNT + 10, &snap, &old_count) == false,
               "well past COUNT is also refused");
    TEST_CHECK(s_rc.dirty == true,
               "still pending after the second refusal");
}

void run_test_relay_cycles(void)
{
    g_test_stub_semaphore_take_default = 1; // pdTRUE -- see comment above test_maybe_persist_skips_...
    test_persist_snapshot_refuses_when_calling_stack_is_external_ram();
    test_persist_lock_created_and_back_to_back_persists_keep_the_latest_write();
    test_persist_snapshot_proceeds_normally_on_an_internal_ram_stack();
    test_budget_ssr_has_no_budget();
    test_budget_quantized_thresholds();
    test_budget_override_wins_over_table();
    test_safety_slot_edge_and_persistence();
    test_v1_blob_migrates_to_v2();
    test_migration_skipped_when_kiln_partition_already_has_blob();
    test_migration_still_runs_on_fresh_kiln_partition();
    test_reset_zeroes_count_and_persists();
    test_reset_rejects_out_of_range_relay();
    test_reset_snapshot_refuses_out_of_range_relay();
    test_reset_runs_inline_when_already_on_flash_worker();
    test_reset_timeout_busy_worker_reports_timeout();
    test_reset_timeout_idle_worker_succeeds();
    test_reset_does_not_lose_a_concurrent_add();
    test_restore_all_sets_every_count_and_persists();
    test_restore_all_is_idempotent();
    test_restore_all_refuses_out_of_range_count_and_writes_nothing();
    test_restore_all_rejects_null_pointer();
    test_restore_all_accepts_higher_value_no_clamp();
    test_restore_all_clamps_value_below_live_count();
    test_restore_all_allow_lower_mask_overrides_one_relay_only();
    test_maybe_persist_skips_without_blocking_when_persist_lock_is_busy();

    test_cfg_fs_partition_absent_fails_loud_and_still_loads_legacy();
    test_cfg_write_failure_is_loud_and_rev_unadvanced();
    test_cfg_fs_migrates_nvs_value_to_file_then_prefers_it();
    test_cfg_fs_dual_write_stays_in_sync_across_repeated_flushes();
    test_cfg_fs_divergence_tie_break_strict_greater_than();
    test_cfg_fs_reset_all_composes_with_migration_never_loses_counts();
    test_cfg_fs_negative_no_file_write_means_file_never_catches_up();
    test_get_dualwrite_status_reports_real_divergence();
    test_padding_is_not_data_status_and_init();
    test_flash_worker_wait_gives_up_after_ceiling();
    test_flash_worker_wait_succeeds_once_predicate_flips();
    test_flash_worker_wait_null_predicate_returns_false();
    test_relay_cycles_init_deferred_flag_clear_when_worker_already_up();
    reset_all_cfg_fs();

    fake_kv_reset_all(); // leave shared fake state as every other test file in this binary expects
}
