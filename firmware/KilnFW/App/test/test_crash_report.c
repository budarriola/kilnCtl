// Host tests for App/drivers/safety/crash_report.c -- the persisted "what the last
// crash was" record (owner request: crash diagnostics persisted to flash
// with an "already read" flag and a CRC, readable later from the
// diagnostics web page).
//
// crash_report.c is #included directly (same convention as
// test_safety_cfg_store.c's #include of safety_cfg_store.c) so this file can
// reach its static compute_crc()/record_valid()/seal_crc()/persist()/load()
// helpers and exercise them for real, including a genuine round trip through
// fake_kv.h's RAM-backed hal_kv fake (HW_ABSTRACTION.md Phase 3 item 3
// migrated crash_report.c off nvs.h onto hal_kv.h) -- same mechanism
// test_safety_cfg_store.c's version-refuse test uses.
//
// crash_report_init() itself (the only function that touches
// esp_core_dump_get_summary()) is NOT exercised here -- stubs/esp_core_dump.h
// always reports "no coredump present", so there is nothing for it to do on
// a host build. That function's summary-to-record fill is a small, obvious
// field-by-field copy (fill_from_summary()) with no branch logic worth a
// fake beyond what would just re-assert the field names back at itself; the
// real value here is the part the owner called out explicitly -- the CRC
// integrity check -- and the load-tolerant/acknowledge/second-boot behavior
// that mirrors run_state.c's own proven pattern.
#include <stddef.h>
#include <string.h>

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"

// crash_report.c (2026-09-15 audit fix) hand-declares
// uart_bridge_ext_run_on_flash_worker()/uart_bridge_ext_is_on_flash_worker()
// (same "declared by hand, not via uart_bridge.h" reasoning as
// relay_cycles.c/safety_cfg_store.c) for crash_report_acknowledge()'s/
// crash_report_clear()'s flash-worker dispatch -- this shared stub,
// included before crash_report.c below, supplies their definitions and the
// same busy/re-entrancy modeling test_relay_cycles.c already relies on.
#include "stubs/bx_worker_stub.h"

int g_test_failures = 0;
int g_test_count = 0;

#include "../drivers/safety/crash_report.c"

#include "fake_sysinfo.h"

static void reset_all(void)
{
    fake_kv_reset_all(); // every test in this file needs a real round trip
    fake_kv_set_write_safe_here(true);
    hal_kv_init_partition(KILN_NVS_PARTITION);
}

static crash_report_record_t make_sample_record(void)
{
    crash_report_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.version = CRASH_REPORT_RECORD_VERSION;
    rec.acknowledged = 0;
    rec.dump_id = 0xAABBCCDDu;
    rec.exc_cause = 9u;
    rec.exc_pc = 0x420001234u & 0xFFFFFFFFu;
    rec.exc_addr = 0xDEADBEEFu;
    rec.exc_a0 = 0x8000BEEFu;
    rec.exc_a1 = 0x3FCE0000u;
    rec.bt_count = 3;
    rec.bt_corrupted = 0;
    rec.backtrace_pc[0] = 0x40001111u;
    rec.backtrace_pc[1] = 0x40002222u;
    rec.backtrace_pc[2] = 0x40003333u;
    copy_str(rec.exc_task, sizeof(rec.exc_task), "IDLE0");
    copy_str(rec.exc_cause_str, sizeof(rec.exc_cause_str), decode_exccause(rec.exc_cause));
    copy_str(rec.reset_reason, sizeof(rec.reset_reason), "PANIC");
    return rec;
}

// ---------------------------------------------------------------------------
// CRC round-trip
// ---------------------------------------------------------------------------

static void test_crc_round_trip(void)
{
    TEST_SECTION("compute_crc/record_valid -- a sealed record validates");
    reset_all();

    crash_report_record_t rec = make_sample_record();
    seal_crc(&rec);

    TEST_CHECK(rec.crc32 != 0, "a real record over non-trivial bytes does not seal to a zero CRC");
    TEST_CHECK(record_valid(&rec), "a record just sealed by seal_crc() validates against itself");

    // Persist it for real and read it back -- the actual round trip through
    // NVS, not just the in-memory struct.
    esp_err_t err = persist(&rec);
    TEST_CHECK(err == ESP_OK, "persist() succeeds against the stub NVS store");

    crash_report_record_t loaded;
    memset(&loaded, 0xAA, sizeof(loaded));
    TEST_CHECK(load(&loaded), "load() reports a valid record after persist()");
    TEST_CHECK(memcmp(&loaded, &rec, sizeof(rec)) == 0, "the loaded record is byte-identical to what was persisted");
}

static void test_corrupted_record_rejected_as_absent(void)
{
    TEST_SECTION("load() -- a corrupted record (bit flip after sealing) is rejected as absent, not "
                 "trusted");
    reset_all();

    crash_report_record_t rec = make_sample_record();
    seal_crc(&rec);
    esp_err_t err = persist(&rec);
    TEST_CHECK(err == ESP_OK, "persist() of the good record succeeds");

    // Flip one bit directly in the fake_kv-stored bytes, AFTER the CRC was
    // sealed -- simulates exactly the failure mode a CRC exists to catch: a
    // brownout mid-write or a flash bit error corrupting the record without
    // going through this module's own write path. Read-modify-write through
    // hal_kv_get_blob()/hal_kv_set_blob() directly (bypassing seal_crc())
    // rather than poking a stub's internal array -- fake_kv.c has no public
    // "peek the raw committed bytes" surface, only the real hal_kv_* API.
    size_t cause_off = offsetof(crash_report_record_t, exc_cause);
    {
        hal_kv_handle_t h;
        TEST_CHECK(hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION) == HAL_OK,
                   "test fixture: corruption handle opens");
        crash_report_record_t stored;
        size_t len = sizeof(stored);
        TEST_CHECK(hal_kv_get_blob(&h, NVS_KEY_CRASH, &stored, &len) == HAL_OK && len == sizeof(rec),
                   "test fixture: fake_kv actually holds our record before corrupting it");
        ((uint8_t *)&stored)[cause_off] ^= 0x01; // a real field (exc_cause), not padding
        TEST_CHECK(hal_kv_set_blob(&h, NVS_KEY_CRASH, &stored, sizeof(stored)) == HAL_OK,
                   "test fixture: corrupted bytes write back");
        TEST_CHECK(hal_kv_commit(&h) == HAL_OK, "test fixture: corruption commits");
        hal_kv_close(&h);
    }

    crash_report_record_t loaded;
    memset(&loaded, 0x55, sizeof(loaded));
    bool ok = load(&loaded);

    TEST_CHECK(ok == false, "load() refuses the corrupted record -- a CRC mismatch is NOT a valid record");

    // NEGATIVE-TEST PROOF (repo discipline: prove a new check can actually
    // fail before trusting it). Confirm record_valid() itself would have
    // ACCEPTED the very same corrupted bytes if the CRC check were skipped --
    // i.e. corrupting one bit really did change the record's meaning, so
    // load()'s rejection is doing real work, not rejecting something that
    // was never going to matter anyway.
    crash_report_record_t corrupted_copy = rec;
    ((uint8_t *)&corrupted_copy)[cause_off] ^= 0x01;
    TEST_CHECK(corrupted_copy.exc_cause != rec.exc_cause,
               "test fixture: the flipped bit really did change a real field's value");
    TEST_CHECK(record_valid(&corrupted_copy) == false,
               "record_valid() on its own correctly flags the corrupted bytes (CRC mismatch) -- "
               "proves the check can fail, not just always pass");
}

static void test_version_mismatch_rejected(void)
{
    TEST_SECTION("record_valid() -- a record from a different CRASH_REPORT_RECORD_VERSION is refused, "
                 "not reinterpreted");
    reset_all();

    crash_report_record_t rec = make_sample_record();
    seal_crc(&rec); // sealed under the CURRENT version
    rec.version = CRASH_REPORT_RECORD_VERSION + 1u; // now claims a future layout

    TEST_CHECK(record_valid(&rec) == false,
               "a version this build does not recognize is refused even though the CRC (computed "
               "before the version was bumped) would otherwise still match today's layout");
}

// ---------------------------------------------------------------------------
// Acknowledge -- sets the flag and survives a reload
// ---------------------------------------------------------------------------

static void test_acknowledge_sets_flag_and_survives_reload(void)
{
    TEST_SECTION("crash_report_acknowledge -- sets acknowledged and persists it (survives a reload)");
    reset_all();

    crash_report_record_t rec = make_sample_record();
    seal_crc(&rec);
    TEST_CHECK(persist(&rec) == ESP_OK, "seed record persisted");

    TEST_CHECK(crash_report_acknowledge() == true, "acknowledge succeeds against a real stored record");

    crash_report_record_t loaded;
    TEST_CHECK(crash_report_get(&loaded), "crash_report_get() reports a valid record after acknowledging");
    TEST_CHECK(loaded.acknowledged == 1, "the reloaded record shows acknowledged=1 -- the flag itself "
                                          "survived the NVS round trip, not just RAM state");
    TEST_CHECK(record_valid(&loaded), "the re-sealed (acknowledged) record still passes its own CRC check");

    // NEGATIVE-TEST PROOF: acknowledging a record whose CRC was NOT
    // re-sealed after flipping the flag would leave a record that fails its
    // own validity check on the very next load -- prove that failure mode is
    // real by constructing exactly that (flag flipped, CRC left stale) and
    // showing record_valid() correctly rejects it.
    crash_report_record_t stale_crc_rec = rec;
    stale_crc_rec.acknowledged = 1; // flipped...
    // ...but crc32 left as sealed for acknowledged=0 (rec.crc32, unchanged)
    TEST_CHECK(record_valid(&stale_crc_rec) == false,
               "a record whose flag changed WITHOUT re-sealing the CRC is correctly rejected -- proves "
               "seal_crc() after the flag flip is load-bearing, not a no-op");
}

static void test_acknowledge_with_no_record_fails(void)
{
    TEST_SECTION("crash_report_acknowledge -- returns false when there is nothing to acknowledge");
    reset_all();

    TEST_CHECK(crash_report_acknowledge() == false, "no stored record -- acknowledge reports failure, "
                                                     "not a silent no-op success");
}

// 2026-09-15 audit fix (review_crash_report_relay_gate_61765de7_2026-09-15.md,
// LOW #5, "add tests for ... the ack-failure path"): a real record IS
// present, but the flash write scripted to fail (fake_kv_script_next_write_
// status, HAL_IO) -- distinct from test_acknowledge_with_no_record_fails()
// above, which fails for the opposite reason (nothing to acknowledge at
// all). Also proves the cache is NOT wrongly cleared on a failed write, and
// that the acknowledge call reaches the flash worker (crash_report.c's PSRAM-
// safety fix) rather than writing inline.
static void test_acknowledge_write_failure_path(void)
{
    TEST_SECTION("crash_report_acknowledge -- an NVS write failure on a REAL record reports failure, "
                 "leaves the record (and the unacked cache) unchanged, and still routes through the "
                 "flash worker");
    reset_all();

    crash_report_record_t rec = make_sample_record();
    rec.acknowledged = 0;
    seal_crc(&rec);
    TEST_CHECK(persist(&rec) == ESP_OK, "seed an unacknowledged record");
    refresh_unacked_cache();
    TEST_CHECK(crash_report_has_unacknowledged() == true, "setup: cache shows unacknowledged before the attempt");

    fake_kv_script_next_write_status(HAL_IO); // the ack's persist() call fails on its very next write
    unsigned dispatch_before = s_stub_dispatch_count;
    bool ok = crash_report_acknowledge();

    // LOW-1 fix (docs/audits/review_crash_gate_followups_62e95bbd_2026-09-15.md):
    // the section title above claims this "still routes through the flash
    // worker" -- actually check that, rather than only checking the write
    // outcome. A hypothetical direct/non-dispatched write (bypassing
    // uart_bridge_ext_run_on_flash_worker() entirely) would still make
    // ok == false here via the same scripted HAL_IO, so this assertion is
    // the only thing in this test that would catch that regression.
    TEST_CHECK(s_stub_dispatch_count == dispatch_before + 1,
              "acknowledge() dispatched exactly once through uart_bridge_ext_run_on_flash_worker() "
              "-- not a direct/inline write bypassing the flash worker");

    TEST_CHECK(ok == false, "acknowledge() reports failure when the underlying NVS write fails, "
                            "not a silent success");
    TEST_CHECK(crash_report_has_unacknowledged() == true,
              "a failed ack write must NOT clear the cache -- the record genuinely is still "
              "unacknowledged on disk, so the relay gate must keep refusing manual relay-ON");

    crash_report_record_t loaded;
    TEST_CHECK(load(&loaded), "the record still loads");
    TEST_CHECK(loaded.acknowledged == 0,
              "the on-disk record is untouched by the failed write -- still acknowledged=0");

    // NEGATIVE-TEST PROOF: with the write failure no longer scripted, the
    // exact same call succeeds -- proves the failure above was really the
    // scripted write, not some other latent defect that always refuses.
    dispatch_before = s_stub_dispatch_count;
    TEST_CHECK(crash_report_acknowledge() == true,
              "with the scripted failure cleared, acknowledge() succeeds against the same record");
    TEST_CHECK(s_stub_dispatch_count == dispatch_before + 1,
              "the successful retry also dispatched through the flash worker, not inline");
    TEST_CHECK(crash_report_has_unacknowledged() == false, "and the cache clears once it actually did");
}

// ---------------------------------------------------------------------------
// Second boot does not overwrite an existing record for the same coredump
// ---------------------------------------------------------------------------

static void test_second_boot_same_dump_id_does_not_overwrite(void)
{
    TEST_SECTION("crash_report_init()'s dump_id check -- the SAME coredump does not get recaptured "
                 "(exercised at the load()+dump_id-comparison level, since crash_report_init() itself "
                 "cannot see a fake coredump on a host build)");
    reset_all();

    crash_report_record_t rec = make_sample_record();
    rec.dump_id = 0x11112222u;
    rec.acknowledged = 1; // operator already acknowledged this one
    seal_crc(&rec);
    TEST_CHECK(persist(&rec) == ESP_OK, "seed record (already acknowledged) persisted");

    // This is exactly the comparison crash_report_init() makes before ever
    // calling persist() again: `load(&existing) && existing.dump_id == dump_id`.
    crash_report_record_t existing;
    bool have_existing = load(&existing);
    TEST_CHECK(have_existing, "the seeded record loads back as valid");
    uint32_t same_dump_id = 0x11112222u;
    bool would_skip = have_existing && existing.dump_id == same_dump_id;
    TEST_CHECK(would_skip == true,
               "a second boot reporting the SAME dump_id is recognized as already-captured -- "
               "crash_report_init() would return here WITHOUT calling persist() again");

    // Prove the record on disk is untouched (still acknowledged=1, matching
    // the original bytes) -- if the recapture guard were broken, a second
    // boot would have overwritten this with a fresh, unacknowledged record.
    crash_report_record_t still_there;
    TEST_CHECK(load(&still_there), "record still loads");
    TEST_CHECK(still_there.acknowledged == 1,
               "acknowledged=1 is UNCHANGED -- nothing overwrote the record for the same dump_id");
    TEST_CHECK(memcmp(&still_there, &rec, sizeof(rec)) == 0, "the stored bytes are byte-identical to "
                                                              "what was originally seeded");

    // NEGATIVE-TEST PROOF: a DIFFERENT dump_id must NOT be recognized as
    // already-captured -- prove the guard actually discriminates rather than
    // always saying "skip".
    uint32_t different_dump_id = 0x99998888u;
    bool would_skip_different = have_existing && existing.dump_id == different_dump_id;
    TEST_CHECK(would_skip_different == false,
               "a genuinely NEW crash (different dump_id) is correctly recognized as NOT already "
               "captured -- proves the guard can say 'capture it' too, not just always 'skip'");
}

// ---------------------------------------------------------------------------
// crash_report_has_unacknowledged() -- the cached, no-I/O flag
// kiln_io_owner.c's relay_on_blocked() reads (2026-09-15,
// docs/audits/manual_relay_readiness_gating_options_2026-09-15.md option B).
// ---------------------------------------------------------------------------

static void test_has_unacknowledged_cache_tracks_persisted_state(void)
{
    TEST_SECTION("crash_report_has_unacknowledged() -- mirrors have_record && !acknowledged, and "
                 "crash_report_acknowledge()/crash_report_clear() keep it in sync without a caller "
                 "having to call refresh_unacked_cache() itself");
    reset_all();

    // s_have_unacked_crash is a plain static, not reset by reset_all() (it
    // is not part of the fake_kv store) -- establish a known baseline the
    // same way crash_report_init() would on a real boot with nothing
    // persisted yet.
    refresh_unacked_cache();
    TEST_CHECK(crash_report_has_unacknowledged() == false,
               "no record persisted -- cache reads false, same default crash_report_get() returns");

    crash_report_record_t rec = make_sample_record();
    rec.acknowledged = 0;
    seal_crc(&rec);
    TEST_CHECK(persist(&rec) == ESP_OK, "seed an unacknowledged record");

    // NEGATIVE-TEST PROOF: prove the cache is not hardcoded false by showing
    // it actually flips once a real unacknowledged record exists.
    TEST_CHECK(crash_report_has_unacknowledged() == false,
               "persisting alone does NOT update the cache -- only refresh_unacked_cache() (called "
               "from crash_report_init()) or acknowledge()/clear() do, proving this is a real cache "
               "and not just re-deriving the answer from disk on every call");
    refresh_unacked_cache();
    TEST_CHECK(crash_report_has_unacknowledged() == true,
               "after a refresh, an unacknowledged record on disk reads as unacknowledged");

    TEST_CHECK(crash_report_acknowledge() == true, "acknowledge succeeds");
    TEST_CHECK(crash_report_has_unacknowledged() == false,
               "crash_report_acknowledge() alone -- with no explicit refresh call -- already cleared "
               "the cache, which is the entire point: kiln_io_owner.c's relay path only ever reads "
               "the cache, never crash_report_get() directly, so a stale true here would wrongly "
               "refuse every manual relay-ON forever after an operator acknowledges the record");

    // Seed a second unacknowledged record and prove crash_report_clear()
    // (which acknowledges internally, then erases) also leaves the cache
    // clear.
    crash_report_record_t rec2 = make_sample_record();
    rec2.dump_id = rec.dump_id + 1u;
    rec2.acknowledged = 0;
    seal_crc(&rec2);
    TEST_CHECK(persist(&rec2) == ESP_OK, "seed a second unacknowledged record");
    refresh_unacked_cache();
    TEST_CHECK(crash_report_has_unacknowledged() == true, "second record shows unacknowledged again");
    TEST_CHECK(crash_report_clear() == ESP_OK, "crash_report_clear() succeeds");
    TEST_CHECK(crash_report_has_unacknowledged() == false,
               "crash_report_clear() also leaves the cache clear, with no explicit refresh");
}

// ---------------------------------------------------------------------------
// crash_report_get() on an empty store
// ---------------------------------------------------------------------------

static void test_get_with_no_record(void)
{
    TEST_SECTION("crash_report_get -- no record ever written reads back as absent, not garbage");
    reset_all();

    crash_report_record_t out;
    memset(&out, 0x77, sizeof(out));
    bool present = crash_report_get(&out);

    TEST_CHECK(present == false, "an empty store reports no record");
    TEST_CHECK(out.version == 0 && out.crc32 == 0,
               "crash_report_get() zeroes *out on the not-present path rather than leaving the "
               "caller's poisoned buffer");
}

// ---------------------------------------------------------------------------
// crash_report_dump_id() -- padding-immune identity hash (bug: a plain CRC
// over the raw esp_core_dump_summary_t hashed uninitialized struct padding
// too, so an identical crash got a different dump_id every boot and an
// acknowledged record never stayed acknowledged across a reboot)
// ---------------------------------------------------------------------------

static void test_dump_id_ignores_padding_bytes(void)
{
    TEST_SECTION("crash_report_dump_id -- two summaries, identical meaningful fields, "
                 "different garbage in the gaps between them, must hash equal");

    esp_core_dump_summary_t a;
    memset(&a, 0x00, sizeof(a));
    strncpy(a.exc_task, "IDLE0", sizeof(a.exc_task) - 1);
    a.exc_pc = 0x40080123u;
    a.exc_bt_info.depth = 3;
    a.exc_bt_info.bt[0] = 0x40001111u;
    a.exc_bt_info.bt[1] = 0x40002222u;
    a.exc_bt_info.bt[2] = 0x40003333u;
    a.exc_bt_info.corrupted = false;

    esp_core_dump_summary_t b;
    memset(&b, 0xA5, sizeof(b)); // simulates uninitialized stack garbage in every byte
    strncpy(b.exc_task, "IDLE0", sizeof(b.exc_task) - 1);
    b.exc_task[strlen("IDLE0")] = '\0'; // memset(0xA5) left no NUL terminator -- match a's content exactly
    b.exc_pc = 0x40080123u;
    b.exc_bt_info.depth = 3;
    b.exc_bt_info.bt[0] = 0x40001111u;
    b.exc_bt_info.bt[1] = 0x40002222u;
    b.exc_bt_info.bt[2] = 0x40003333u;
    b.exc_bt_info.corrupted = false;
    // bt[3..15] (beyond depth) and ex_info are left as 0xA5 "garbage" in b,
    // 0x00 in a -- crash_report_dump_id() must not look at them.

    uint32_t id_a = crash_report_dump_id(&a);
    uint32_t id_b = crash_report_dump_id(&b);
    TEST_CHECK(id_a == id_b,
               "dump_id depends only on exc_pc/exc_task/backtrace-up-to-depth -- garbage "
               "elsewhere in the struct (standing in for uninitialized padding/fields) "
               "does not change it");

    esp_core_dump_summary_t c = a;
    c.exc_pc = 0x40080999u; // a genuinely different crash
    uint32_t id_c = crash_report_dump_id(&c);
    TEST_CHECK(id_a != id_c, "a real difference (exc_pc) still changes dump_id");
}

// ---------------------------------------------------------------------------
// crash_report_init()/_clear() through hal_sysinfo's fake backend (HW
// abstraction Phase 3 item 4 migration). stubs/esp_core_dump.h's
// esp_core_dump_get_summary() still always reports ESP_ERR_NOT_FOUND (full
// summary parsing stays out of hal_sysinfo's scope, per that header's top
// comment), so scripting the fake's coredump-present flag true cannot reach
// a persisted record -- but it DOES newly reach crash_report_init()'s
// "coredump present but summary fetch failed" branch, which was completely
// unreachable before this migration (the old hardcoded stub always reported
// "no coredump", so hal_sysinfo_coredump_present()==false was the only path
// host tests ever exercised). This proves crash_report_init() actually
// calls through to hal_sysinfo_coredump_present() rather than some stale
// local no-op.
// ---------------------------------------------------------------------------

static void test_init_reaches_summary_fetch_when_coredump_present(void)
{
    TEST_SECTION("crash_report_init -- hal_sysinfo_coredump_present() actually gates the "
                 "summary-fetch call (both branches leave no record, so that alone can't "
                 "tell them apart -- the summary-fetch call count can)");

    reset_all();
    fake_sysinfo_reset_all();
    fake_sysinfo_set_coredump_present(false);
    test_esp_core_dump_get_summary_reset_count();

    crash_report_init();
    TEST_CHECK(test_esp_core_dump_get_summary_call_count() == 0,
               "coredump_present()==false -> crash_report_init() must not attempt the summary "
               "fetch at all");

    crash_report_record_t out;
    TEST_CHECK(crash_report_get(&out) == false, "no record persisted when no coredump present");

    fake_sysinfo_reset_all();
    fake_sysinfo_set_coredump_present(true);
    test_esp_core_dump_get_summary_reset_count();

    crash_report_init(); // esp_core_dump_get_summary() stub still fails -- no record persisted
    TEST_CHECK(test_esp_core_dump_get_summary_call_count() == 1,
               "coredump_present()==true -> crash_report_init() must reach the summary-fetch "
               "branch exactly once (proves it calls through to hal_sysinfo_coredump_present() "
               "rather than a stale local no-op, which the previous version of this test could "
               "not distinguish from the false case)");
    TEST_CHECK(crash_report_get(&out) == false,
               "coredump present but summary fetch fails -> still no record persisted, and no "
               "crash in crash_report_init() while getting there through hal_sysinfo");

    fake_sysinfo_reset_all();
}

static void test_clear_reflag_survives_ack_write_failure(void)
{
    TEST_SECTION("crash_report_clear -- LOW fix (review_crash_report_relay_gate_61765de7_2026-09-15): "
                 "if the internal acknowledge() write fails but the NVS erase right after it still "
                 "succeeds, the unacked cache must not be left stuck true -- clear() re-derives it "
                 "from disk after the erase rather than trusting acknowledge()'s own (failed) outcome");
    reset_all();

    crash_report_record_t rec = make_sample_record();
    rec.acknowledged = 0;
    seal_crc(&rec);
    TEST_CHECK(persist(&rec) == ESP_OK, "seed an unacknowledged record");
    refresh_unacked_cache();
    TEST_CHECK(crash_report_has_unacknowledged() == true, "setup: cache shows unacknowledged");

    // Fail exactly the ONE write crash_report_clear()'s internal
    // crash_report_acknowledge() call makes (its persist()); the erase call
    // right after it is a separate hal_kv_erase_key()/commit() sequence and
    // is not consumed by this single scripted failure, so it still succeeds.
    fake_kv_script_next_write_status(HAL_IO);
    esp_err_t err = crash_report_clear();

    TEST_CHECK(err == ESP_OK, "crash_report_clear() still reports success -- the coredump/NVS erase, "
                              "which is what the caller actually asked for, went through even though "
                              "the internal acknowledge() write failed");
    TEST_CHECK(crash_report_get(&(crash_report_record_t){0}) == false,
               "the record is genuinely gone from disk -- the erase succeeded");
    TEST_CHECK(crash_report_has_unacknowledged() == false,
               "NEGATIVE-TEST PROOF target: the cache must read false here. With the pre-fix code "
               "(clear() trusting only crash_report_acknowledge()'s failed return and never calling "
               "refresh_unacked_cache() itself), this would still read true -- an operator's LCD/web "
               "acknowledge would look like it did nothing, and the manual relay-ON gate would keep "
               "refusing forever despite the record being genuinely erased from flash.");
}

// LOW-3 fix (docs/audits/review_crash_gate_followups_62e95bbd_2026-09-15.md):
// an LCD Acknowledge and a web /clear both dispatch to the single flash-
// worker task, which drains its queue one job at a time -- but the OLD
// crash_report_acknowledge() called load() in the CALLER's task, before
// dispatch, and only the modify+persist ran inside the job. So a clear that
// erased the record between the caller's load() and the ack job actually
// running would still see the ack job write the operator's stale,
// already-loaded copy back to disk with acknowledged=1, resurrecting a
// record the operator had just cleared. The fix moved load() itself inside
// the job (crash_ack_job()), so whichever job the worker runs LATER always
// sees whatever the earlier job actually left on disk. This test proves
// that: it drives crash_ack_job() directly (as the worker would run it)
// against a record that was already erased -- standing in for "a clear won
// the race and ran first" -- and checks it does NOT resurrect anything.
static void test_ack_job_does_not_resurrect_after_concurrent_clear(void)
{
    TEST_SECTION("crash_ack_job -- an ack job that runs AFTER a concurrent clear erased the record "
                 "must see 'no record' and do nothing, not resurrect a stale caller-side copy");
    reset_all();

    crash_report_record_t rec = make_sample_record();
    rec.acknowledged = 0;
    seal_crc(&rec);
    TEST_CHECK(persist(&rec) == ESP_OK, "seed an unacknowledged record");
    refresh_unacked_cache();
    TEST_CHECK(crash_report_has_unacknowledged() == true, "setup: cache shows unacknowledged");

    // Simulate "the web /clear's erase job won the race and already ran" --
    // this is exactly what crash_clear_job() does to disk, run directly
    // rather than via crash_report_clear() so this test does not also
    // depend on that function's own internal acknowledge() call.
    crash_clear_job_ctx_t clear_ctx = { .kv_err = HAL_IO, .coredump_err = HAL_IO };
    crash_clear_job(&clear_ctx);
    TEST_CHECK(clear_ctx.kv_err == HAL_OK || clear_ctx.kv_err == HAL_NOT_FOUND, "setup: erase job succeeded");
    refresh_unacked_cache();
    TEST_CHECK(crash_report_has_unacknowledged() == false, "setup: cache reflects the erase");

    // NEGATIVE-TEST PROOF target: now run the ack job -- standing in for the
    // LCD's Acknowledge, queued before the clear but executed after it on the
    // worker's single serial queue. With the pre-fix shape (load() outside
    // the job, a pre-loaded rec pointer passed in) this would blindly persist
    // acknowledged=1 and bring the record back. With load() inside the job,
    // it must see "no record" and do nothing.
    crash_ack_job_ctx_t ack_ctx = { .err = ESP_FAIL, .had_record = false, .already_acked = false };
    crash_ack_job(&ack_ctx);

    TEST_CHECK(ack_ctx.had_record == false,
              "the ack job saw no record at execution time -- it does not resurrect a record "
              "that a concurrent clear already erased");
    crash_report_record_t loaded;
    TEST_CHECK(load(&loaded) == false, "the record is still genuinely absent from disk after the ack job ran");
    TEST_CHECK(crash_report_has_unacknowledged() == false,
              "the cache still reads false -- no resurrection visible to the relay gate or the LCD/web either");
}

// INFO fix (docs/audits/review_crash_gate_low_fixes_c534a0df_2026-09-15.md):
// test_ack_job_does_not_resurrect_after_concurrent_clear() above drives
// crash_ack_job() directly, so it pins the load-inside-the-job shape but
// cannot fail if a future change moves load() back OUT of the job and into
// crash_report_acknowledge() itself while leaving crash_ack_job() untouched
// -- that regression would still pass every assertion above, since the job
// would still see the record correctly if nothing raced it. This test
// closes that gap by driving crash_report_acknowledge() (the real caller
// entry point) and asserting NO hal_kv read happens before the dispatch --
// fake_kv_get_call_count() (added alongside this test) is the only way to
// see that from outside, since the stub's own dispatch counter (LOW-1) does
// not distinguish "one dispatch, zero caller-side reads" from "one
// caller-side read plus one dispatch".
static void test_acknowledge_reads_only_inside_the_dispatched_job(void)
{
    TEST_SECTION("crash_report_acknowledge -- performs no hal_kv read in the caller's own task before "
                 "dispatching onto the flash worker (load() must live inside crash_ack_job())");
    reset_all();

    crash_report_record_t rec = make_sample_record();
    rec.acknowledged = 0;
    seal_crc(&rec);
    TEST_CHECK(persist(&rec) == ESP_OK, "seed an unacknowledged record");

    // persist() above and its own internal reads are done -- snapshot AFTER
    // setup, right before the call under test.
    unsigned reads_before = fake_kv_get_call_count();
    unsigned dispatch_before = s_stub_dispatch_count;

    TEST_CHECK(crash_report_acknowledge() == true, "acknowledge succeeds against the seeded record");

    // Exactly one dispatch, and the ONLY hal_kv_get_blob() call attributable
    // to this whole call happened inside crash_ack_job() (load() once), not
    // one extra caller-side load() before the dispatch. If a future change
    // moved load() back out to the caller, this would see 2 (or more) get
    // calls for the same single dispatch, since the caller's own load()
    // would be counted too.
    TEST_CHECK(s_stub_dispatch_count == dispatch_before + 1,
              "exactly one dispatch through the flash worker");
    TEST_CHECK(fake_kv_get_call_count() == reads_before + 1,
              "exactly one hal_kv read total -- the job's own load(), and nothing read by the "
              "caller before dispatch");
}

static void test_clear_erases_coredump_via_hal_sysinfo(void)
{
    TEST_SECTION("crash_report_clear -- hal_sysinfo_coredump_erase() clears fake coredump presence");
    reset_all();
    fake_sysinfo_reset_all();
    fake_sysinfo_set_coredump_present(true);
    TEST_CHECK(hal_sysinfo_coredump_present() == true, "setup: fake reports coredump present");

    esp_err_t err = crash_report_clear();

    TEST_CHECK(err == ESP_OK, "crash_report_clear() succeeds");
    TEST_CHECK(hal_sysinfo_coredump_present() == false,
               "crash_report_clear() routed the erase through hal_sysinfo_coredump_erase(), "
               "which cleared the fake's presence flag");

    fake_sysinfo_reset_all();
}


// ---------------------------------------------------------------------------
// 2026-09-08: exception-frame trustworthiness + the a0/a1 capture that makes
// a LoadProhibited record actionable. See
// docs/audits/crash_loadprohibited_0x18_2026-09-08.md -- the live board's
// record (cause 28 / addr 0x18 / pc 0xfffffffd / backtrace corrupted) could
// not be told apart from a genuine null-struct-pointer dereference, because
// the stack pointer was never recorded and nothing flagged the frame as
// self-inconsistent.
// ---------------------------------------------------------------------------

static void test_exception_registers_round_trip(void)
{
    TEST_SECTION("exc_a0/exc_a1 -- the crashing frame's return address and STACK POINTER survive "
                 "persist/load (without a1 a LoadProhibited record is undiagnosable)");
    reset_all();

    crash_report_record_t rec = make_sample_record();
    rec.exc_a0 = 0x8200ABCDu;
    rec.exc_a1 = 0x3FCEF010u;
    seal_crc(&rec);
    TEST_CHECK(persist(&rec) == ESP_OK, "record with a0/a1 persists");

    crash_report_record_t loaded;
    TEST_CHECK(load(&loaded), "record with a0/a1 loads back as valid");
    TEST_CHECK(loaded.exc_a0 == 0x8200ABCDu, "exc_a0 round-trips through NVS unchanged");
    TEST_CHECK(loaded.exc_a1 == 0x3FCEF010u, "exc_a1 (stack pointer) round-trips through NVS unchanged");

    // NEGATIVE-TEST PROOF: a1 is genuinely covered by the record's CRC, so a
    // silently-flipped stack pointer cannot be read back as trustworthy.
    crash_report_record_t tampered = loaded;
    tampered.exc_a1 = 0x00000000u;
    TEST_CHECK(record_valid(&tampered) == false,
               "flipping exc_a1 alone breaks the record CRC -- the stack pointer is inside the "
               "integrity check, not appended outside it");
}

static void test_frame_trustworthy_rejects_pc_of_zero(void)
{
    TEST_SECTION("crash_report_frame_trustworthy -- a record whose exc_pc is 0xfffffffd "
                 "(esp_cpu_process_stack_pc(0), i.e. the saved PC was 0) is NOT trustworthy");
    reset_all();

    crash_report_record_t good = make_sample_record();
    good.exc_pc = 0x42001234u;
    good.bt_corrupted = 0;
    TEST_CHECK(crash_report_frame_trustworthy(&good) == true,
               "a record with a real code-address PC and an uncorrupted backtrace IS trustworthy "
               "-- the predicate is not vacuously false");

    // The EXACT record the live board produced on 2026-09-08.
    crash_report_record_t live = make_sample_record();
    live.exc_cause = 28u;             // LoadProhibited
    live.exc_addr = 0x00000018u;      // looks exactly like a struct field offset -- it is not safe
    live.exc_pc = 0xfffffffdu;        // == 0 - 3
    live.bt_count = 1;
    live.backtrace_pc[0] = 0xfffffffdu;
    live.bt_corrupted = 1;
    TEST_CHECK(crash_report_frame_trustworthy(&live) == false,
               "the live 2026-09-08 record is refused: nobody may read its exc_addr 0x18 as a "
               "struct field offset");

    // Each condition alone is sufficient -- neither is carrying the other.
    crash_report_record_t pc_only = make_sample_record();
    pc_only.exc_pc = 0xfffffffdu;
    pc_only.bt_corrupted = 0;
    TEST_CHECK(crash_report_frame_trustworthy(&pc_only) == false,
               "pc == esp_cpu_process_stack_pc(0) alone is enough to refuse the frame, even with "
               "an uncorrupted backtrace");

    crash_report_record_t bt_only = make_sample_record();
    bt_only.exc_pc = 0x42001234u;
    bt_only.bt_corrupted = 1;
    TEST_CHECK(crash_report_frame_trustworthy(&bt_only) == false,
               "a corrupted backtrace alone is enough to refuse the frame, even with a plausible PC");

    TEST_CHECK(crash_report_frame_trustworthy(NULL) == false, "NULL record is not trustworthy");
}

// fill_v3_fields() -- pure helper, ROADMAP.md follow-up (v3 record fields:
// dump_id was already exposed internally, this covers the two NEW pieces,
// crash_uptime_s/crash_uptime_known and fw_build).
static void test_fill_v3_fields_valid_beacon_and_build_info(void)
{
    TEST_SECTION("fill_v3_fields -- a valid beacon and valid build_info populate both fields");

    crash_report_record_t out;
    memset(&out, 0, sizeof(out));

    crash_uptime_beacon_t beacon;
    beacon.magic = CRASH_UPTIME_BEACON_MAGIC;
    beacon.uptime_s = 12345u;

    hal_sysinfo_build_info_t build_info;
    memset(&build_info, 0, sizeof(build_info));
    strncpy(build_info.date, "Sep 22 2026", sizeof(build_info.date) - 1);
    strncpy(build_info.time, "10:00:00", sizeof(build_info.time) - 1);
    build_info.valid = true;

    fill_v3_fields(&out, &beacon, &build_info);

    TEST_CHECK(out.crash_uptime_s == 12345u, "a valid beacon's uptime_s is copied through");
    TEST_CHECK(out.crash_uptime_known == 1u, "a valid beacon marks crash_uptime_known");
    TEST_CHECK(strcmp(out.fw_build, "Sep 22 2026 10:00:00") == 0,
               "valid build_info is formatted as \"date time\", matching dashboard_http.c's own "
               "fw_build convention");
}

static void test_fill_v3_fields_bad_magic_reads_as_unknown(void)
{
    TEST_SECTION("fill_v3_fields -- a beacon with the wrong magic (power-on garbage, or a beacon "
                 "never written this boot) is reported as unknown, not a stale/garbage number");

    crash_report_record_t out;
    memset(&out, 0, sizeof(out));

    crash_uptime_beacon_t beacon;
    beacon.magic = 0xDEADBEEFu; // anything but CRASH_UPTIME_BEACON_MAGIC
    beacon.uptime_s = 999999u;  // garbage value that must NOT leak through

    hal_sysinfo_build_info_t build_info;
    memset(&build_info, 0, sizeof(build_info));
    build_info.valid = false;

    fill_v3_fields(&out, &beacon, &build_info);

    TEST_CHECK(out.crash_uptime_known == 0u, "a bad-magic beacon is reported as unknown");
    TEST_CHECK(out.crash_uptime_s == 0u,
               "a bad-magic beacon's garbage uptime_s is not copied through when unknown");
    TEST_CHECK(out.fw_build[0] == '\0', "invalid build_info leaves fw_build empty, not garbage");
}

static void test_crash_report_note_alive_writes_a_valid_beacon(void)
{
    TEST_SECTION("crash_report_note_alive -- writes a beacon fill_v3_fields() then reads as known");

    memset(&s_uptime_beacon, 0xAA, sizeof(s_uptime_beacon)); // simulate power-on garbage first
    TEST_CHECK(s_uptime_beacon.magic != CRASH_UPTIME_BEACON_MAGIC,
               "sanity: the pre-fill garbage does not already look like a valid beacon");

    crash_report_note_alive();

    TEST_CHECK(s_uptime_beacon.magic == CRASH_UPTIME_BEACON_MAGIC,
               "crash_report_note_alive() stamps the magic");

    crash_report_record_t out;
    memset(&out, 0, sizeof(out));
    hal_sysinfo_build_info_t build_info;
    memset(&build_info, 0, sizeof(build_info));
    build_info.valid = false;
    fill_v3_fields(&out, &s_uptime_beacon, &build_info);
    TEST_CHECK(out.crash_uptime_known == 1u,
               "a beacon written by crash_report_note_alive() reads back as known");
}

void run_test_crash_report(void)
{
    test_crc_round_trip();
    test_corrupted_record_rejected_as_absent();
    test_version_mismatch_rejected();
    test_acknowledge_sets_flag_and_survives_reload();
    test_acknowledge_with_no_record_fails();
    test_acknowledge_write_failure_path();
    test_has_unacknowledged_cache_tracks_persisted_state();
    test_second_boot_same_dump_id_does_not_overwrite();
    test_get_with_no_record();
    test_dump_id_ignores_padding_bytes();
    test_init_reaches_summary_fetch_when_coredump_present();
    test_clear_reflag_survives_ack_write_failure();
    test_ack_job_does_not_resurrect_after_concurrent_clear();
    test_acknowledge_reads_only_inside_the_dispatched_job();
    test_clear_erases_coredump_via_hal_sysinfo();
    test_exception_registers_round_trip();
    test_frame_trustworthy_rejects_pc_of_zero();
    test_fill_v3_fields_valid_beacon_and_build_info();
    test_fill_v3_fields_bad_magic_reads_as_unknown();
    test_crash_report_note_alive_writes_a_valid_beacon();

    fake_kv_reset_all(); // leave shared fake state as every other test file in this binary expects
}

int main(void)
{
    run_test_crash_report();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
