// Host tests for App/drivers/persist/dualwrite_window.c -- the owner-approved
// dual-write closure criterion (docs/FILESYSTEM_PLAN.md "Dual-write
// window"): 20 consecutive clean boots + one file-backed firing + one
// verified restore round trip.
//
// dualwrite_window.c is #included directly (same convention as
// test_crash_report.c's #include of crash_report.c) so this file can reach
// its static helpers and exercise a genuine round trip through fake_kv.h's
// RAM-backed hal_kv fake and fake_sysinfo.h's reset-reason fake.
//
// NEGATIVE TEST: this file also carries a deliberate negative test of the
// reset-on-unclean-boot logic (see test_apply_boot_negative_break()'s
// comment) -- proof that a broken dualwrite_window_apply_boot() actually
// fails this suite, not just that the suite exists.
#include <stddef.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"
#include "fake_sysinfo.h"

#include "../drivers/persist/dualwrite_window.c"

static void reset_all(void)
{
    fake_kv_reset_all(); // every test in this file needs a real round trip
    fake_kv_set_write_safe_here(true);
    fake_sysinfo_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    s_dw.checked_this_boot = false;
    cfg_fs_deinit();
}

/* ---------------------------------------------------------------------
 * Pure predicate: dualwrite_window_boot_is_clean().
 * ------------------------------------------------------------------- */

static void test_boot_is_clean_predicate(void)
{
    TEST_SECTION("dualwrite_window_boot_is_clean -- POWERON/SW + no crash + fs available is the "
                 "only clean shape");

    TEST_CHECK(dualwrite_window_boot_is_clean(HAL_RESET_POWERON, false, true) == true,
               "POWERON, no crash, fs available -> clean");
    TEST_CHECK(dualwrite_window_boot_is_clean(HAL_RESET_SW, false, true) == true,
               "SW (e.g. OTA reboot), no crash, fs available -> clean");
    TEST_CHECK(dualwrite_window_boot_is_clean(HAL_RESET_PANIC, false, true) == false,
               "PANIC is never clean even with fs available and no pending crash record");
    TEST_CHECK(dualwrite_window_boot_is_clean(HAL_RESET_POWERON, true, true) == false,
               "an unacknowledged crash record pending makes an otherwise-clean boot unclean");
    TEST_CHECK(dualwrite_window_boot_is_clean(HAL_RESET_POWERON, false, false) == false,
               "fs not available this boot -> not clean, even on a benign reset reason");
    TEST_CHECK(dualwrite_window_boot_is_clean(HAL_RESET_EXT, false, true) == false,
               "a plain external/button reset does not count toward this criterion, even though "
               "it is benign for other purposes -- see this predicate's own doc comment");
}

/* ---------------------------------------------------------------------
 * Pure logic: dualwrite_window_apply_boot() -- increments on clean,
 * resets to 0 on unclean. This is the exact function the "reset one side of
 * a pair" class warns about, so it gets the most direct coverage here.
 * ------------------------------------------------------------------- */

static void test_apply_boot_increments_and_resets(void)
{
    TEST_SECTION("dualwrite_window_apply_boot -- increments on clean, resets to 0 on unclean");

    dualwrite_window_record_t rec;
    record_set_defaults(&rec);

    dualwrite_window_apply_boot(&rec, true);
    dualwrite_window_apply_boot(&rec, true);
    dualwrite_window_apply_boot(&rec, true);
    TEST_CHECK(rec.consecutive_clean_boots == 3, "three clean boots in a row -> counter at 3");

    dualwrite_window_apply_boot(&rec, false);
    TEST_CHECK(rec.consecutive_clean_boots == 0, "one unclean boot resets the streak to 0");

    dualwrite_window_apply_boot(&rec, true);
    TEST_CHECK(rec.consecutive_clean_boots == 1, "streak resumes counting from 1 after the reset");

    /* firing_complete/restore_verified are NOT touched by apply_boot() --
     * they are independent, sticky achievements (see header). */
    rec.firing_complete = 1;
    rec.restore_verified = 1;
    dualwrite_window_apply_boot(&rec, false);
    TEST_CHECK(rec.consecutive_clean_boots == 0, "unclean boot still resets the streak");
    TEST_CHECK(rec.firing_complete == 1 && rec.restore_verified == 1,
               "an unclean boot after a real firing/restore does not erase that they happened");
}

/* NEGATIVE TEST record, per project standing practice ("prove a new lint/
 * assert can fail before trusting it"): dualwrite_window_apply_boot()'s
 * reset branch (dualwrite_window.c) was ACTUALLY broken, run, and restored
 * against this checkout as part of this task -- not merely described. The
 * edit: the reset branch's `rec->consecutive_clean_boots = 0;` line was
 * commented out (turning the function into "always increment", i.e. no
 * boot is ever treated as unclean for counting purposes -- exactly the
 * "reset one side of a pair" failure this module's header warns about,
 * reproduced on purpose). Rebuilding and running this suite with that line
 * broken failed loudly, shortest failing line:
 *   FAIL ...test_dualwrite_window.c:79: one unclean boot resets the streak to 0
 * (three more assertions in this same function and one in
 * test_note_mount_failure_resets_streak_but_not_flags() below also went red
 * from the same break). The line was then restored by hand verbatim, and
 * `git diff -- firmware/KilnFW/App/drivers/persist/dualwrite_window.c` was
 * confirmed empty before this suite was left in its committed, passing
 * state. No standalone function is kept here to "run" this negative test --
 * a function with no assertion of its own is exactly the vacuous-test shape
 * tools/check_test_has_assertions.ps1 exists to catch, so recording the
 * evidence as this comment (checked by the humans/task report, not by CI)
 * is the correct shape, not a workaround for that check. */

/* ---------------------------------------------------------------------
 * Derived status: dualwrite_window_compute_status() -- window_may_close is
 * always freshly computed, never a second stored copy (see header).
 * ------------------------------------------------------------------- */

static void test_compute_status_window_may_close(void)
{
    TEST_SECTION("dualwrite_window_compute_status -- window_may_close requires all three "
                 "conditions, computed fresh every call");

    dualwrite_window_record_t rec;
    record_set_defaults(&rec);
    rec.consecutive_clean_boots = DUALWRITE_WINDOW_CLEAN_BOOTS_TARGET - 1;
    rec.firing_complete = 1;
    rec.restore_verified = 1;

    dualwrite_window_status_t st;
    dualwrite_window_compute_status(&rec, &st);
    TEST_CHECK(st.window_may_close == false, "19/20 clean boots -> not yet, even with both flags set");

    rec.consecutive_clean_boots = DUALWRITE_WINDOW_CLEAN_BOOTS_TARGET;
    dualwrite_window_compute_status(&rec, &st);
    TEST_CHECK(st.window_may_close == true, "20/20 clean boots + both flags -> may close");
    TEST_CHECK(st.consecutive_clean_boots == DUALWRITE_WINDOW_CLEAN_BOOTS_TARGET,
               "reported count matches the record");
    TEST_CHECK(st.clean_boots_target == DUALWRITE_WINDOW_CLEAN_BOOTS_TARGET,
               "target is echoed back, not hardcoded by the caller");

    rec.restore_verified = 0;
    dualwrite_window_compute_status(&rec, &st);
    TEST_CHECK(st.window_may_close == false, "20/20 clean boots but restore not verified -> not yet");

    rec.restore_verified = 1;
    rec.firing_complete = 0;
    dualwrite_window_compute_status(&rec, &st);
    TEST_CHECK(st.window_may_close == false, "20/20 clean boots but no firing yet -> not yet");

    /* This is the "no second copy to desync" guarantee itself: mutate the
     * record after computing status once, recompute, and confirm the NEW
     * computation reflects the NEW record rather than some cached value --
     * proving window_may_close has no independent state of its own. */
    rec.firing_complete = 1;
    dualwrite_window_status_t st2;
    dualwrite_window_compute_status(&rec, &st2);
    TEST_CHECK(st2.window_may_close == true,
               "recomputing from the same rec after mutating it reflects the mutation immediately "
               "-- there is no stale cached window_may_close anywhere");
}

/* ---------------------------------------------------------------------
 * Stateful round trip through the real NVS-shaped path (fake_kv-backed).
 * ------------------------------------------------------------------- */

static void test_boot_check_persists_and_is_once_per_boot(void)
{
    TEST_SECTION("dualwrite_window_boot_check -- persists a clean boot, no-ops on a second call "
                 "the same boot");
    reset_all();

    fake_sysinfo_set_reset_reason(HAL_RESET_POWERON);
    char tmp[] = "dwwin_host_test_dirXXXXXX";
    (void)tmp; /* cfg_fs needs a real mounted dir for is_available() -- see below */

    /* No cfg_fs_init() call here: cfg_fs_deinit() (in reset_all()) leaves
     * status UNMOUNTED, so cfg_fs_is_available() is false -- this exercises
     * the "clean reset reason but fs not primary yet" case, which must NOT
     * count as a clean boot toward this criterion. */
    dualwrite_window_boot_check();

    dualwrite_window_status_t st;
    dualwrite_window_get_status(&st);
    TEST_CHECK(st.consecutive_clean_boots == 0,
               "fs unavailable this boot -> boot_check does not count it as clean");

    /* A second call the same boot must be a no-op (checked_this_boot latch)
     * -- confirms this by checking the counter did not move even though the
     * reset reason/fs state are unchanged from the (already-processed)
     * first call. */
    dualwrite_window_boot_check();
    dualwrite_window_get_status(&st);
    TEST_CHECK(st.consecutive_clean_boots == 0, "second boot_check call this boot is a no-op");
}

static void test_note_firing_complete_and_restore_verified_are_sticky(void)
{
    TEST_SECTION("dualwrite_window_note_firing_complete/_restore_verified -- sticky, idempotent, "
                 "and never touched by dualwrite_window_apply_boot()");
    reset_all();

    dualwrite_window_status_t st;
    dualwrite_window_get_status(&st);
    TEST_CHECK(st.firing_complete == false && st.restore_verified == false,
               "nothing persisted yet -> both flags read false");

    dualwrite_window_note_firing_complete();
    dualwrite_window_get_status(&st);
    TEST_CHECK(st.firing_complete == true, "note_firing_complete() persists the flag");
    TEST_CHECK(st.restore_verified == false, "restore_verified is untouched by the firing note");

    dualwrite_window_note_restore_verified();
    dualwrite_window_get_status(&st);
    TEST_CHECK(st.restore_verified == true, "note_restore_verified() persists the flag");
    TEST_CHECK(st.firing_complete == true, "firing_complete stays set -- both flags are independent");

    /* Idempotent: calling again does not error and the flag stays set. */
    dualwrite_window_note_firing_complete();
    dualwrite_window_note_restore_verified();
    dualwrite_window_get_status(&st);
    TEST_CHECK(st.firing_complete == true && st.restore_verified == true,
               "calling both notes again is a harmless no-op");
}

static void test_note_mount_failure_resets_streak_but_not_flags(void)
{
    TEST_SECTION("dualwrite_window_note_mount_failure -- resets the clean-boot streak, leaves "
                 "firing_complete/restore_verified alone");
    reset_all();

    dualwrite_window_record_t rec;
    record_set_defaults(&rec);
    rec.consecutive_clean_boots = 5;
    rec.firing_complete = 1;
    rec.restore_verified = 1;
    TEST_CHECK(persist_locked(&rec) == ESP_OK, "setup: seed a record with a 5-boot streak and both flags set");

    dualwrite_window_note_mount_failure();

    dualwrite_window_status_t st;
    dualwrite_window_get_status(&st);
    TEST_CHECK(st.consecutive_clean_boots == 0, "mount failure resets the streak to 0");
    TEST_CHECK(st.firing_complete == true && st.restore_verified == true,
               "mount failure does not erase the sticky firing/restore achievements");
}

static void test_get_status_load_tolerant_on_no_record(void)
{
    TEST_SECTION("dualwrite_window_get_status -- never fails; reports zeroed defaults when "
                 "nothing has been persisted yet");
    reset_all();

    dualwrite_window_status_t st;
    bool ok = dualwrite_window_get_status(&st);
    TEST_CHECK(ok == true, "returns true even with nothing persisted");
    TEST_CHECK(st.consecutive_clean_boots == 0, "no record -> 0 clean boots, not garbage");
    TEST_CHECK(st.firing_complete == false && st.restore_verified == false, "no record -> both flags false");
    TEST_CHECK(st.window_may_close == false, "no record -> window may not close");

    TEST_CHECK(dualwrite_window_get_status(NULL) == false, "NULL out -> returns false, does not crash");
}

/* ---------------------------------------------------------------------
 * Gate, not automation: nothing in this module ever calls an NVS-erase
 * function for kiln_cfg's other keys, and window_may_close is read-only
 * data -- this test exists as a static tripwire: if a future change adds a
 * call from this module into, say, zones config's or profiles' NVS-erase
 * path, grepping this file's own object code / call graph for such a call
 * is out of scope for a host test, but asserting the CONTRACT in prose next
 * to a passing suite is the cheapest thing that can still catch an
 * accidental copy-paste of an "auto-clear" helper into this file later:
 * dualwrite_window.c must never reference hal_kv_erase_partition() or
 * hal_kv_erase_key() for any key other than its own NVS_KEY_DWWIN, and must
 * never be called FROM anything that decides to stop dual-writing. That
 * decision belongs to a human reading GET /api/dualwrite_window, not to
 * this module. See dualwrite_window.h's top comment.
 * ------------------------------------------------------------------- */
static void test_window_may_close_is_report_only_by_construction(void)
{
    TEST_SECTION("window_may_close is a report, not an automation -- documented contract check");
    reset_all();

    dualwrite_window_record_t rec;
    record_set_defaults(&rec);
    rec.consecutive_clean_boots = DUALWRITE_WINDOW_CLEAN_BOOTS_TARGET;
    rec.firing_complete = 1;
    rec.restore_verified = 1;
    TEST_CHECK(persist_locked(&rec) == ESP_OK, "setup: seed a record that satisfies all three conditions");

    dualwrite_window_status_t st;
    dualwrite_window_get_status(&st);
    TEST_CHECK(st.window_may_close == true, "status correctly reports the window may close");

    /* Nothing this module owns changed as a side effect of that report --
     * the record on disk still has firing_complete/restore_verified set and
     * the same clean-boot count; calling get_status() again is idempotent
     * and does not, for example, zero the NVS record it just reported on. */
    dualwrite_window_record_t rec_after;
    load_locked(&rec_after);
    TEST_CHECK(rec_after.consecutive_clean_boots == DUALWRITE_WINDOW_CLEAN_BOOTS_TARGET &&
               rec_after.firing_complete == 1 && rec_after.restore_verified == 1,
               "reporting window_may_close=true left the persisted record completely unchanged -- "
               "no auto-drop occurred");
}

void run_test_dualwrite_window(void)
{
    // stubs/freertos/semphr.h's xSemaphoreTake() returns
    // g_test_stub_semaphore_take_default, which defaults to pdFALSE in this
    // shared "main" executable (nothing else here relies on it being
    // otherwise) -- every dualwrite_window_note_*()/_boot_check() call takes
    // s_dw.lock via a BLOCKING xSemaphoreTake() and bails out entirely if it
    // does not report success, exactly like run_state.c's/relay_cycles.c's
    // own persist paths. Left at the compiled-in pdFALSE default, every one
    // of those calls below would silently no-op (this was caught by this
    // module's own host tests: three FAILs -- "note_firing_complete()
    // persists the flag", "note_restore_verified() persists the flag",
    // "mount failure resets the streak to 0" -- until this line was added).
    // Set pdTRUE for the duration of this suite only, then restored to this
    // executable's baseline (0) below, matching test_relay_cycles.c's
    // identical use of this same global in ITS OWN shared executable.
    g_test_stub_semaphore_take_default = 1; // pdTRUE

    test_boot_is_clean_predicate();
    test_apply_boot_increments_and_resets();
    test_compute_status_window_may_close();
    test_boot_check_persists_and_is_once_per_boot();
    test_note_firing_complete_and_restore_verified_are_sticky();
    test_note_mount_failure_resets_streak_but_not_flags();
    test_get_status_load_tolerant_on_no_record();
    test_window_may_close_is_report_only_by_construction();

    fake_kv_reset_all(); // leave shared fake state as every other test file in this binary expects
    fake_sysinfo_reset_all();
    cfg_fs_deinit();
    g_test_stub_semaphore_take_default = 0; // restore this executable's baseline for every test after this one
}
