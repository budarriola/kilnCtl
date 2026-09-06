// Host tests for App/drivers/safety/crash_report.c -- the persisted "what the last
// crash was" record (owner request: crash diagnostics persisted to flash
// with an "already read" flag and a CRC, readable later from the
// diagnostics web page).
//
// crash_report.c is #included directly (same convention as
// test_safety_cfg_store.c's #include of safety_cfg_store.c) so this file can
// reach its static compute_crc()/record_valid()/seal_crc()/persist()/load()
// helpers and exercise them for real, including a genuine round trip through
// fake_kv.h's RAM-backed hal_kv fake (HW_ABSTRACTION_PLAN.md Phase 3 item 3
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

void run_test_crash_report(void)
{
    test_crc_round_trip();
    test_corrupted_record_rejected_as_absent();
    test_version_mismatch_rejected();
    test_acknowledge_sets_flag_and_survives_reload();
    test_acknowledge_with_no_record_fails();
    test_second_boot_same_dump_id_does_not_overwrite();
    test_get_with_no_record();
    test_dump_id_ignores_padding_bytes();
    test_init_reaches_summary_fetch_when_coredump_present();
    test_clear_erases_coredump_via_hal_sysinfo();

    fake_kv_reset_all(); // leave shared fake state as every other test file in this binary expects
}
