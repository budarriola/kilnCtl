// Host tests for firmware/SaftyFW/src/config_store_flash.c -- the flash I/O
// and ARMED-check glue, host-testable for the first time since its Phase 3
// item 2 rebase onto hal_flash.h (docs/HW_ABSTRACTION.md). Exercises
// the three things that rebase promised to keep intact: the ARMED write
// gate (config_store_decide_write() against relay_owner_get_state(), here
// driven by config_store_flash_host_stubs.c), the seq/CRC round-robin log
// across a real fake_flash-backed sector, and hal_flash_safe_execute()
// failure injection surfacing through config_store_flash_rc_reason().
//
// config_store.c's own pure pack/unpack/CRC/decision logic is already
// covered by test_config_store.c -- this file exercises config_store_
// flash.c's real-flash glue ON TOP of that, via config_store_boot_load()/
// config_store_write()/the getters, backed by fake_flash.c instead of a
// real RP2040.
//
// A SEPARATE executable from test_main.c's, same reason test_hal_spi_pico.c
// is (see build_host_tests.ps1's comment on that one): config_store_flash_
// host_stubs.c defines relay_owner_get_state()/console_uart_puts()/
// console_uart_write() as bare test doubles, and the main exe already links
// the REAL relay_owner.c (for its own hal_gpio tests) -- linking both into
// one executable is an LNK2005 multiply-defined-symbol error, not a valid
// combination. Own main(), own g_test_failures/g_test_count, own exit code,
// folded into build_host_tests.ps1's overall exit code same as the
// hal_spi_pico executable is.
#include <string.h>
#include <windows.h>

#include "test_common.h"

#include "config_store.h"
#include "config_store_flash_host_stubs.h"
#include "discrete_pin_policy.h"
#include "fake_flash.h"
#include "tasks/relay_owner.h"

int g_test_failures = 0;
int g_test_count = 0;

// Every test case starts from a freshly-erased sector at the SAME (base,
// size) config_store_flash.c's own hal_flash_region_t binds to --
// SAFTYFW_CONFIG_STORE_FLASH_OFFSET/_SIZE (flash_layout.h) -- big enough
// that hal_flash_region_init() accepts that real offset against the fake's
// configured total size. See fake_flash.h's FAKE_FLASH_MAX_SIZE_BYTES
// comment for why the fake was sized to allow this.
#include "flash_layout.h"
// Covers sector A AND sector B (flash_endurance_review_2026-09-07.md R2):
// OFFSET_B == OFFSET + SIZE (flash_layout.h), so the fake image must reach
// OFFSET_B + SIZE for hal_flash_region_init() to accept sector B's real
// offset too.
#define TEST_FLASH_TOTAL_SIZE \
    (SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B + SAFTYFW_CONFIG_STORE_FLASH_SIZE)

static void reset_all(void)
{
    TEST_CHECK(fake_flash_reset_all_sized(TEST_FLASH_TOTAL_SIZE) == true,
               "fake_flash sized to cover the real config-store offset");
    config_store_flash_host_stub_reset();
    // The fallback double buffer's own statics (s_fallback_valid/_active/
    // _gen/_buf) are NOT reset by the above -- on real hardware a power
    // cycle reinitialises them, but in this one long-lived test process
    // they would otherwise carry state across every test case (see
    // config_store_test_reset_fallback_state()'s own comment).
    config_store_test_reset_fallback_state();
}

static void test_boot_load_blank_sector_is_default(void)
{
    TEST_SECTION("config_store_flash: blank sector -> default record, not rejected");
    reset_all();

    config_store_boot_load();

    TEST_CHECK(config_store_is_config_rejected() == false, "blank sector is not a rejection");
    TEST_CHECK(config_store_get_config_crc() == 0, "uncommissioned CRC reads 0");
    TEST_CHECK(config_store_get_config_version() == 0, "uncommissioned version reads 0");
    TEST_CHECK(config_store_is_calibration_missing() == true, "default calibration_missing");

    config_store_record_t rec;
    config_store_get_full_record(&rec);
    config_store_record_t def;
    config_store_default(&def);
    TEST_CHECK(memcmp(&rec, &def, sizeof(rec)) == 0,
               "full record matches config_store_default() on a blank sector");
}

static void test_write_then_reload_round_trips(void)
{
    TEST_SECTION("config_store_flash: write, then a fresh boot_load sees it");
    reset_all();
    config_store_boot_load();

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u; // MAX31856_TC_TYPE_T
    rec.calibration_missing = false;

    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == true, "write succeeds while not ARMED");
    TEST_CHECK(reason != NULL && strcmp(reason, "ok") == 0, "reason is 'ok' on success");

    // Simulate a reboot: nothing in config_store_flash.c resets on its own,
    // so re-running boot_load re-scans the (now real) flash image fake_flash
    // is holding, exactly like a power cycle would re-scan real NOR flash.
    config_store_boot_load();

    TEST_CHECK(config_store_get_tc_type() == 0x07u, "tc_type survives a reload");
    TEST_CHECK(config_store_is_calibration_missing() == false,
               "calibration_missing survives a reload");
    TEST_CHECK(config_store_get_config_crc() != 0, "a committed record reports a nonzero CRC");
    TEST_CHECK(config_store_is_config_rejected() == false, "a good record is not a rejection");
}

// opus review 2026-09-09, finding B: before this fix, s_fallback_valid
// stayed false from boot until the first-ever config_store_write() in that
// boot -- so a core-1 reader hitting the exhausted-retries fallback path
// during that window got accessor defaults (calibration_missing=true, etc)
// instead of the perfectly good record config_store_boot_load() just loaded
// and validated from flash. This test commits a record, reboots (a fresh
// config_store_boot_load(), same simulated-power-cycle idiom as the reload
// test above), and then -- with NO write at all in this "boot" -- forces
// the fallback path via config_store_test_force_fallback_path() (no hook
// needed: proving the seed alone is enough, with no writer activity in this
// boot to ever flip s_fallback_valid the old way) and checks the record
// that comes back is the persisted one, not a default.
static void test_fallback_seeded_at_boot_before_any_write(void)
{
    TEST_SECTION("config_store_flash: fallback buffer is seeded at boot -- no availability gap "
                 "before the first commissioning write of a boot (opus review finding B)");
    reset_all();
    config_store_boot_load();

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u; // MAX31856_TC_TYPE_T
    rec.calibration_missing = false;
    const char *reason = NULL;
    TEST_CHECK(config_store_write(&rec, &reason) == true, "fixture: commissioning write succeeds");

    // Simulate a reboot -- same idiom as test_write_then_reload_round_trips(),
    // but ALSO reset the fallback double buffer's own statics back to a true
    // cold-boot state (reset_all() does this too, but that ran before the
    // fixture write above -- this call is what actually simulates the power
    // cycle between the write and the reload). Deliberately NO config_store_
    // write() call after this point: the whole point is to prove the
    // fallback is trustworthy in a boot that has not committed anything yet.
    config_store_test_reset_fallback_state();
    config_store_boot_load();

    config_store_test_force_fallback_path(true);
    config_store_record_t snap;
    memset(&snap, 0, sizeof(snap));
    config_store_get_full_record(&snap);
    config_store_test_force_fallback_path(false);

    TEST_CHECK(snap.tc_type == 0x07u,
               "the fallback path returns the persisted tc_type, not CONFIG_STORE_DEFAULT_TC_TYPE "
               "-- no write happened yet this boot, so the old code would have returned false "
               "here (s_fallback_valid still false) and every getter's own accessor default");
    TEST_CHECK(snap.calibration_missing == false,
               "the fallback path returns the persisted calibration_missing, not the fail-safe "
               "'true' every getter falls back to when config_store_seqlock_read() returns false");
}

static void test_write_refused_while_armed(void)
{
    TEST_SECTION("config_store_flash: ARMED gate refuses the write and touches no flash");
    reset_all();
    config_store_boot_load();
    config_store_flash_host_stub_set_relay_state(RELAY_OWNER_STATE_ARMED);

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u;

    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == false, "write refused while ARMED");
    TEST_CHECK(reason != NULL &&
                   reason == config_store_write_decision_reason(CONFIG_STORE_WRITE_REFUSED_ARMED),
               "refusal reason is the ARMED one, not a flash-layer reason");

    // No sector was touched: config_store_write() must refuse BEFORE
    // scheduling any hal_flash_erase()/hal_flash_program() -- confirmed here
    // by checking the sector still reads back as a blank/default record.
    config_store_boot_load();
    TEST_CHECK(config_store_get_config_crc() == 0,
               "sector unchanged by a refused write -- still uncommissioned");
}

static void test_seq_increments_and_survives_wraparound(void)
{
    TEST_SECTION("config_store_flash: seq/CRC log across a full 8-slot wraparound");
    reset_all();
    config_store_boot_load();

    // CONFIG_STORE_SLOTS_PER_SECTOR (8) writes plus a few more to force at
    // least one erase-and-wrap, checking seq keeps climbing and the record
    // read back after EVERY write matches what was just written.
    for (uint32_t i = 0; i < CONFIG_STORE_SLOTS_PER_SECTOR + 3u; i++) {
        config_store_record_t rec;
        config_store_default(&rec);
        rec.tc_type = (uint8_t)(i % 8u);
        rec.calibration_missing = false;

        const char *reason = NULL;
        bool ok = config_store_write(&rec, &reason);
        TEST_CHECK(ok == true, "each write in the wraparound loop succeeds");

        uint8_t expect_version = config_store_seq_to_version(i + 1u);
        TEST_CHECK(config_store_get_config_version() == expect_version,
                   "config_version advances with seq, including across the wrap");
        TEST_CHECK(config_store_get_tc_type() == (uint8_t)(i % 8u),
                   "just-written tc_type is immediately visible without a reload");
    }

    // A fresh boot_load (simulated reboot) must still find the LATEST
    // record, not an older one a naive scan might prefer after the sector
    // wrapped and was re-erased.
    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == (uint8_t)((CONFIG_STORE_SLOTS_PER_SECTOR + 2u) % 8u),
               "reload after wraparound finds the truly latest record");
}

static void test_safe_execute_timeout_is_reported_and_leaves_cache_unchanged(void)
{
    TEST_SECTION("config_store_flash: hal_flash_safe_execute() TIMEOUT surfaces distinctly");
    reset_all();
    config_store_boot_load();

    fake_flash_script_next_op_status(FAKE_FLASH_OP_SAFE_EXECUTE, HAL_TIMEOUT);

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x03u;

    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == false, "write fails when hal_flash_safe_execute() times out");
    TEST_CHECK(reason != NULL && strstr(reason, "timed out") != NULL,
               "TIMEOUT is reported as a timeout, not folded into a generic failure");

    // The in-RAM cache must not have been advanced by a failed write --
    // config_store_write() only assigns s_cached_record/s_cached_slot AFTER
    // a successful hal_flash_safe_execute().
    TEST_CHECK(config_store_get_config_crc() == 0,
               "a timed-out write leaves the cache at its pre-write (uncommissioned) state");
}

// persist/save logging audit (2026-09-06): config_store_write_cb() used to
// discard hal_flash_program()'s own return value with a bare (void) cast,
// on the argument that it could only ever fail with a caller-bug status.
// The host fake's hal_flash_safe_execute() (fake_flash.c) documents exactly
// why that argument does not cover this case: "any erase/program failure the
// callback triggers is surfaced through ITS own return path ... not through
// this function's return value" -- so a HAL_OK from hal_flash_safe_execute()
// says only that the callback RAN, never that the program landed. This
// pins that config_store_write() now catches a scripted PROGRAM failure
// even though SAFE_EXECUTE itself reports HAL_OK.
static void test_program_failure_is_not_masked_by_safe_execute_ok(void)
{
    TEST_SECTION("config_store_flash: a failed hal_flash_program() is not "
                 "masked by hal_flash_safe_execute()'s own HAL_OK");
    reset_all();
    config_store_boot_load();

    fake_flash_script_next_op_status(FAKE_FLASH_OP_PROGRAM, HAL_IO);

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x03u;

    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == false,
               "write fails when the underlying hal_flash_program() fails, "
               "even though hal_flash_safe_execute() itself returns HAL_OK");
    TEST_CHECK(reason != NULL && strcmp(reason, "ok") != 0,
               "the reason string is not the success sentinel on a masked failure");

    // Same "must not silently advance the cache" property the TIMEOUT test
    // above pins, for the other failure path.
    TEST_CHECK(config_store_get_config_crc() == 0,
               "a program failure the safe_execute wrapper did not itself "
               "report still leaves the cache at its pre-write state");
}

// --- A/B sectors (flash_endurance_review_2026-09-07.md R2) -----------------
//
// Sector A is at SAFTYFW_CONFIG_STORE_FLASH_OFFSET, sector B immediately
// after it at SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B -- both HAL_FLASH_ERASE_
// SIZE-aligned, so fake_flash_get_erase_count()'s sector index is simply
// offset / HAL_FLASH_ERASE_SIZE.
#define SECTOR_A_INDEX (SAFTYFW_CONFIG_STORE_FLASH_OFFSET / HAL_FLASH_ERASE_SIZE)
#define SECTOR_B_INDEX (SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B / HAL_FLASH_ERASE_SIZE)

static void write_default_with_tc_type(uint8_t tc_type)
{
    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = tc_type;
    rec.calibration_missing = false;
    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == true, "setup write succeeds");
}

static void test_switch_to_sector_b_after_sector_a_fills(void)
{
    TEST_SECTION("config_store_flash: A/B -- 9th write switches to sector B");
    reset_all();
    config_store_boot_load();

    // Fill sector A's 8 slots -- no erase needed for any of these (mirrors
    // the pre-A/B wraparound test).
    for (uint32_t i = 0; i < CONFIG_STORE_SLOTS_PER_SECTOR; i++) {
        write_default_with_tc_type((uint8_t)(i % 8u));
    }
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_A_INDEX) == 0,
               "filling sector A's 8 slots never erases it");
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_B_INDEX) == 0,
               "sector B untouched while sector A still has room");

    // The 9th write must switch to sector B (erase B, program slot 0),
    // leaving sector A's 8 records untouched (not erased).
    write_default_with_tc_type(0x05u);
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_A_INDEX) == 0,
               "the switch erases the OTHER sector, never the one just filled");
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_B_INDEX) == 1,
               "sector B was erased exactly once, for this switch");
    TEST_CHECK(config_store_get_tc_type() == 0x05u, "the switch write is immediately visible");

    config_store_boot_load(); // simulated reboot
    TEST_CHECK(config_store_get_tc_type() == 0x05u,
               "reboot after a sector switch still finds sector B's record");
}

static void test_wear_is_spread_across_both_sectors(void)
{
    TEST_SECTION("config_store_flash: A/B -- erases alternate between sectors (wear levelling)");
    reset_all();
    config_store_boot_load();

    // 3 full sector-fills' worth of writes (24) forces two switches: A->B,
    // then B->A. Each sector should show ~1 erase per 8 writes, not one
    // sector absorbing all of them the way the single-sector design did.
    for (uint32_t i = 0; i < 3u * CONFIG_STORE_SLOTS_PER_SECTOR; i++) {
        write_default_with_tc_type((uint8_t)(i % 8u));
    }
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_A_INDEX) == 1,
               "sector A erased once (the B->A switch at write 17)");
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_B_INDEX) == 1,
               "sector B erased once (the A->B switch at write 9)");
}

static void test_power_loss_mid_erase_of_target_leaves_old_sector_valid(void)
{
    TEST_SECTION("config_store_flash: power cut MID-ERASE of the target sector "
                 "leaves the old sector's record valid");
    reset_all();
    config_store_boot_load();

    for (uint32_t i = 0; i < CONFIG_STORE_SLOTS_PER_SECTOR; i++) {
        write_default_with_tc_type((uint8_t)(i % 8u));
    }
    uint8_t last_good_tc_type = config_store_get_tc_type();
    uint16_t crc_before = config_store_get_config_crc();

    // The 9th write must erase sector B first. fake_flash_simulate_power_
    // loss_during() is NOT usable here: it is a one-shot consumed by the
    // very next hal_flash_* call regardless of which primitive that is
    // (fake_flash.h's own doc comment), and config_store_write() always
    // reaches the erase THROUGH hal_flash_safe_execute() -- that outer call
    // is itself a hal_flash_* call, so arming ERASE here would be silently
    // eaten by hal_flash_safe_execute()'s own internal power-loss check
    // before the erase ever runs (confirmed: the write succeeded regardless
    // when this test first used simulate_power_loss_during(ERASE)).
    // fake_flash_script_next_op_status() is the mechanism this repo's other
    // config_store_flash tests already use to reach an op nested inside
    // safe_execute (see test_program_failure_is_not_masked_by_safe_execute_
    // ok() above) -- it is a per-op slot, not consumed by an unrelated call.
    // This models the erase call failing outright with NO effect at all
    // (take_scripted_status() short-circuits before touching the image) --
    // one legitimate power-cut instant: the erase command was issued but
    // nothing landed before power was lost.
    fake_flash_script_next_op_status(FAKE_FLASH_OP_ERASE, HAL_IO);

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x06u;
    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == false, "write fails when the switch-target erase is interrupted");

    // The cache must not have advanced -- same property the existing TIMEOUT/
    // program-failure tests pin, now for an erase failure during a switch.
    TEST_CHECK(config_store_get_tc_type() == last_good_tc_type,
               "in-RAM cache still reports the old sector's record after a failed erase");

    // Simulated reboot: re-scan BOTH sectors from scratch. Sector A must
    // still hold its full, untouched, valid last record -- the erase never
    // reached sector A at all (config_store_write_cb() was only ever handed
    // sector B's region).
    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == last_good_tc_type,
               "reboot after an interrupted switch-erase still finds the old sector's record");
    TEST_CHECK(config_store_get_config_crc() == crc_before,
               "the surviving record's CRC/version is exactly what it was before the failed switch");
}

static void test_power_loss_mid_program_of_target_leaves_old_sector_valid(void)
{
    TEST_SECTION("config_store_flash: power cut MID-PROGRAM of the target sector "
                 "leaves the old sector's record valid");
    reset_all();
    config_store_boot_load();

    for (uint32_t i = 0; i < CONFIG_STORE_SLOTS_PER_SECTOR; i++) {
        write_default_with_tc_type((uint8_t)(i % 8u));
    }
    uint8_t last_good_tc_type = config_store_get_tc_type();
    uint16_t crc_before = config_store_get_config_crc();

    // fake_flash_simulate_power_loss_during(PROGRAM)'s torn-write model is
    // not reachable through config_store_write() for the same reason the
    // erase test above switched away from it -- hal_flash_safe_execute()'s
    // own internal power-loss check consumes the arm before the inner
    // program ever runs. So this test reproduces the torn-write STATE
    // directly (bypassing config_store_write() entirely for the fault
    // injection), which is arguably the more faithful test anyway: it
    // exercises exactly what the ARBITER (config_store_find_latest_multi_ex(),
    // via config_store_boot_load()) does when it finds a sector genuinely
    // left mid-program by a real power cut, independent of how the write
    // path happens to be plumbed.
    hal_flash_region_t region_b;
    TEST_CHECK(hal_flash_region_init(&region_b, SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B,
                                      SAFTYFW_CONFIG_STORE_FLASH_SIZE) == HAL_OK,
               "fixture can bind sector B's real region");
    TEST_CHECK(hal_flash_erase(&region_b, 0, SAFTYFW_CONFIG_STORE_FLASH_SIZE) == HAL_OK,
               "fixture erases sector B (this IS what the real switch's erase half would do)");

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x06u;
    rec.seq = 999u; // would-be seq -- irrelevant, this record is never fully committed
    uint8_t packed[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&rec, packed);
    // Program only the FIRST HAL_FLASH_PROGRAM_SIZE (256 B) page of the 512 B
    // record -- exactly fake_flash's own torn-write model (fake_flash.h:
    // "the NEXT hal_flash_program() call writes only the first program-page
    // ... and then reports HAL_IO"), just driven directly instead of through
    // the swallowed power-loss arm. Slot 0's remaining 256 B, and the CRC at
    // offset 504, stay erased (0xFF) -- a real crash mid multi-page
    // flash_range_program() loop leaves exactly this shape.
    TEST_CHECK(hal_flash_program(&region_b, 0, packed, HAL_FLASH_PROGRAM_SIZE) == HAL_OK,
               "fixture programs only the first page of the torn record");

    // Simulated reboot: sector B's slot 0 fails CRC (magic/seq are there,
    // but the CRC field at offset 504 was never programmed and the fields it
    // covers past byte 256 are still 0xFF) -- the arbiter must fall back to
    // sector A's still-fully-valid last record.
    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == last_good_tc_type,
               "reboot after a torn switch-program still finds the OLD sector's valid record, "
               "not the torn (CRC-invalid) one in the new sector");
    TEST_CHECK(config_store_get_config_crc() == crc_before,
               "the surviving record is exactly the pre-switch one, seq and all");
    TEST_CHECK(config_store_is_config_rejected() == false,
               "a torn write is an ordinary CRC failure, not the range-rejection diagnostic case");
}

static void test_power_loss_between_erase_and_program_of_target_leaves_old_sector_valid(void)
{
    TEST_SECTION("config_store_flash: power cut BETWEEN erase and program of the "
                 "target sector (erase lands, program never starts) leaves the old "
                 "sector's record valid");
    reset_all();
    config_store_boot_load();

    for (uint32_t i = 0; i < CONFIG_STORE_SLOTS_PER_SECTOR; i++) {
        write_default_with_tc_type((uint8_t)(i % 8u));
    }
    uint8_t last_good_tc_type = config_store_get_tc_type();

    // Erase succeeds; program never runs at all (a clean HAL_IO on the very
    // next PROGRAM call, no partial-write model this time) -- this is the
    // exact instant the endurance review's original single-sector defect
    // had ZERO valid copies of the config: sector B is now blank, and
    // nothing has been written to it yet.
    fake_flash_script_next_op_status(FAKE_FLASH_OP_PROGRAM, HAL_IO);

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x06u;
    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == false, "write fails when the program half never runs");

    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == last_good_tc_type,
               "sector B is blank (erased, never programmed) at this exact instant -- "
               "sector A, never touched by this write, is still found and still valid");
}

// D2 fix (docs/audits/rp2040_config_store_write_atomicity_2026-09-14.md):
// a torn write left behind by a power cut mid-program, INSIDE a sector that
// still has room left (not the 8th-write/switch case above -- an ordinary
// same-sector append), used to be silently reused by the very next write:
// config_store_next_write_slot() picks latest_good+1 by arithmetic alone,
// which is exactly the torn slot in this scenario. Programming a fresh
// record on top of those non-erased bytes ANDs the new record against the
// leftover torn bits (hal_flash_program()'s own doc comment), corrupting it
// -- so THIS write would also silently fail to actually land, while every
// flash call along the way still reports HAL_OK. The fix must detect the
// target slot is not erased and switch sectors instead of reusing it.
static void test_torn_inline_slot_is_never_reprogrammed(void)
{
    TEST_SECTION("config_store_flash: a torn write left mid-sector (not a switch) is "
                 "never reprogrammed by the next write -- it switches sectors instead");
    reset_all();
    config_store_boot_load();

    // Three good writes land in sector A slots 0, 1, 2.
    write_default_with_tc_type(0x01u);
    write_default_with_tc_type(0x02u);
    write_default_with_tc_type(0x03u);
    uint8_t last_good_tc_type = config_store_get_tc_type();
    TEST_CHECK(last_good_tc_type == 0x03u, "setup: sector A slot 2 holds the last good record");

    // Directly tear slot 3 of sector A (bypassing config_store_write(), same
    // fixture technique test_power_loss_mid_program_of_target_leaves_old_
    // sector_valid() above uses) -- models a power cut mid-program during
    // what WOULD have been the 4th, ordinary, no-erase-needed append.
    hal_flash_region_t region_a;
    TEST_CHECK(hal_flash_region_init(&region_a, SAFTYFW_CONFIG_STORE_FLASH_OFFSET,
                                      SAFTYFW_CONFIG_STORE_FLASH_SIZE) == HAL_OK,
               "fixture can bind sector A's real region");
    config_store_record_t torn;
    config_store_default(&torn);
    torn.tc_type = 0x09u;
    torn.seq = 999u;
    uint8_t torn_packed[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&torn, torn_packed);
    TEST_CHECK(hal_flash_program(&region_a, 3u * CONFIG_STORE_RECORD_LEN, torn_packed,
                                  HAL_FLASH_PROGRAM_SIZE) == HAL_OK,
               "fixture tears slot 3: only its first page is programmed");
    uint8_t slot3_after_tear[CONFIG_STORE_RECORD_LEN];
    TEST_CHECK(hal_flash_read(&region_a, 3u * CONFIG_STORE_RECORD_LEN, slot3_after_tear,
                               sizeof(slot3_after_tear)) == HAL_OK,
               "fixture reads back the torn slot for later comparison");

    // Simulated reboot: the arbiter finds slot 2 (last CRC-valid record);
    // slot 3 fails CRC and is skipped, exactly like any other corrupt slot.
    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == last_good_tc_type,
               "boot after the mid-sector tear still finds slot 2's valid record");

    // The next write must NOT land back on slot 3. Before the D2 fix this
    // would silently AND-corrupt slot 3 with the new record's bytes; now it
    // must detect slot 3 is not erased and switch to sector B instead.
    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u;
    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == true, "the write itself succeeds (via a sector switch, not slot 3 reuse)");
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_B_INDEX) == 1,
               "the write switched to (and erased) sector B rather than reusing torn slot 3");
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_A_INDEX) == 0,
               "sector A -- still holding the one valid record at slot 2 -- was never erased");

    uint8_t slot3_after_write[CONFIG_STORE_RECORD_LEN];
    TEST_CHECK(hal_flash_read(&region_a, 3u * CONFIG_STORE_RECORD_LEN, slot3_after_write,
                               sizeof(slot3_after_write)) == HAL_OK,
               "fixture re-reads slot 3 after the write");
    TEST_CHECK(memcmp(slot3_after_tear, slot3_after_write, CONFIG_STORE_RECORD_LEN) == 0,
               "slot 3's torn bytes are byte-for-byte untouched -- the write never "
               "reprogrammed over them");

    TEST_CHECK(config_store_get_tc_type() == 0x07u,
               "the new record is immediately visible after the switch");
    config_store_boot_load(); // simulated reboot
    TEST_CHECK(config_store_get_tc_type() == 0x07u,
               "the value present after a simulated reboot matches what was last "
               "successfully written (sector B's record), not slot 2's older one");
}

// Companion to the above from the NO_SLOT (never-committed) side: a torn
// FIRST-EVER write (slot 0 of sector A, before any record ever validated)
// must not be reprogrammed by the next write either.
static void test_torn_first_slot_before_any_valid_record_is_never_reprogrammed(void)
{
    TEST_SECTION("config_store_flash: a torn FIRST write (no valid record ever committed) "
                 "is never reprogrammed by the next write");
    reset_all();
    config_store_boot_load();
    TEST_CHECK(config_store_is_config_rejected() == false,
               "setup: a blank board is not the rejected case, just never-committed");

    hal_flash_region_t region_a;
    TEST_CHECK(hal_flash_region_init(&region_a, SAFTYFW_CONFIG_STORE_FLASH_OFFSET,
                                      SAFTYFW_CONFIG_STORE_FLASH_SIZE) == HAL_OK,
               "fixture can bind sector A's real region");
    config_store_record_t torn;
    config_store_default(&torn);
    torn.tc_type = 0x09u;
    torn.seq = 1u;
    uint8_t torn_packed[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&torn, torn_packed);
    TEST_CHECK(hal_flash_program(&region_a, 0, torn_packed, HAL_FLASH_PROGRAM_SIZE) == HAL_OK,
               "fixture tears slot 0 of sector A: only its first page is programmed");

    // Simulated reboot: nothing validates -- s_cached_slot == CONFIG_STORE_NO_SLOT.
    config_store_boot_load();

    config_store_record_t rec;
    config_store_default(&rec);
    rec.tc_type = 0x07u;
    const char *reason = NULL;
    bool ok = config_store_write(&rec, &reason);
    TEST_CHECK(ok == true, "the write succeeds (sector A is erased first, reclaiming the torn slot)");
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_A_INDEX) == 1,
               "the NO_SLOT case erases sector A itself before writing -- nothing valid "
               "existed anywhere to protect");
    TEST_CHECK(config_store_get_tc_type() == 0x07u,
               "the new record is immediately visible");
    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == 0x07u,
               "the value survives a simulated reboot");
}

static void test_migration_from_legacy_single_sector_layout(void)
{
    TEST_SECTION("config_store_flash: migration -- a board running the OLD "
                 "single-sector firmware's on-flash image loads unchanged");
    reset_all();

    // Build EXACTLY what a board flashed with the pre-A/B firmware has on
    // flash today: real committed records in sector A's slots (packed
    // directly, bypassing config_store_write() so this fixture cannot
    // accidentally depend on the NEW write path), sector B left at its
    // erased-flash 0xFF -- because the old firmware never knew sector B
    // existed and never wrote a byte there. abs_max_temp_c and tc_type are
    // exactly the safety fields the audit named as at risk of a silent
    // reset to defaults.
    config_store_record_t legacy_rec;
    config_store_default(&legacy_rec);
    legacy_rec.seq = 7;
    legacy_rec.tc_type = 0x01u; // MAX31856_TC_TYPE_J -- deliberately non-default
    legacy_rec.abs_max_temp_c = 1300.0f;
    legacy_rec.fields_set |= CONFIG_STORE_SET_ABS_MAX_TEMP_C | CONFIG_STORE_SET_TC_TYPE;
    legacy_rec.calibration_missing = false;

    uint8_t packed[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&legacy_rec, packed);

    // Write straight into the fake image at sector A's slot 3, sector B
    // untouched (still all-0xFF from reset_all()) -- config_store_flash.c's
    // own hal_flash_region_t for sector A binds at SAFTYFW_CONFIG_STORE_
    // FLASH_OFFSET, so this is byte-for-byte "what real flash already holds".
    hal_flash_region_t region_a;
    TEST_CHECK(hal_flash_region_init(&region_a, SAFTYFW_CONFIG_STORE_FLASH_OFFSET,
                                      SAFTYFW_CONFIG_STORE_FLASH_SIZE) == HAL_OK,
               "fixture can bind sector A's real region");
    TEST_CHECK(hal_flash_program(&region_a, 3u * CONFIG_STORE_RECORD_LEN, packed,
                                  sizeof(packed)) == HAL_OK,
               "fixture can program the legacy record into sector A slot 3");

    // Now boot the NEW (A/B) firmware against this pre-existing image --
    // this is the exact call main.c makes at boot, no migration step exists
    // or is needed.
    config_store_boot_load();

    TEST_CHECK(config_store_get_tc_type() == 0x01u,
               "the pre-existing tc_type is found, not silently reset to the K default");
    TEST_CHECK(config_store_is_calibration_missing() == false,
               "the pre-existing calibration_missing==false survives -- not forced back to true");
    TEST_CHECK(config_store_is_config_rejected() == false,
               "the legacy record is not treated as a rejection");
    config_store_record_t rt;
    config_store_get_full_record(&rt);
    TEST_CHECK(rt.abs_max_temp_c == 1300.0f,
               "abs_max_temp_c (the safety ceiling the audit specifically named) survives intact");
    TEST_CHECK(config_store_field_is_set(&rt.fields_set, CONFIG_STORE_SET_ABS_MAX_TEMP_C),
               "the commissioned bit for abs_max_temp_c survives, not just the raw float");

    // A write after this point must still behave correctly -- proves the
    // cached (sector, slot) this boot_load derived (sector 0, slot 3) feeds
    // config_store_plan_write() sanely, continuing the round-robin from
    // exactly where the legacy image left off rather than assuming slot 0.
    write_default_with_tc_type(0x02u);
    TEST_CHECK(fake_flash_get_erase_count(SECTOR_A_INDEX) == 0,
               "continuing sector A's round-robin from slot 3 needs no erase yet");
    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == 0x02u,
               "the post-migration write round-trips normally");
}

static void test_corrupt_active_sector_falls_back_to_other_sector(void)
{
    TEST_SECTION("config_store_flash: a corrupted current sector falls back "
                 "to the other, still-valid sector");
    reset_all();
    config_store_boot_load();

    // Fill A, switch to B (B now holds the current record; A's 8 records
    // are stale but individually still valid -- never erased proactively).
    for (uint32_t i = 0; i < CONFIG_STORE_SLOTS_PER_SECTOR; i++) {
        write_default_with_tc_type((uint8_t)(i % 8u));
    }
    write_default_with_tc_type(0x04u); // switch write, lands in sector B slot 0
    TEST_CHECK(config_store_get_tc_type() == 0x04u, "sanity: switch write landed as expected");

    // Directly corrupt sector B's only record (bit-rot / a botched program
    // that still happens to have looked complete to the write path but rots
    // afterward) -- read the whole first program-page back, clear one data
    // byte to 0x00 in the local copy, and reprogram that SAME page (AND-only,
    // matching real NOR semantics: every other byte ANDs with itself and is
    // unchanged; only the forced byte actually clears). hal_flash_program()
    // requires an HAL_FLASH_PROGRAM_SIZE(256)-aligned offset/len -- a bare
    // single byte at offset 20 is not a legal call, hence the whole-page
    // round trip.
    hal_flash_region_t region_b;
    TEST_CHECK(hal_flash_region_init(&region_b, SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B,
                                      SAFTYFW_CONFIG_STORE_FLASH_SIZE) == HAL_OK,
               "fixture can bind sector B's real region");
    uint8_t page0[HAL_FLASH_PROGRAM_SIZE];
    TEST_CHECK(hal_flash_read(&region_b, 0u, page0, sizeof(page0)) == HAL_OK,
               "fixture can read sector B's first page");
    // Offset 20 (inside abs_max_temp_c) is a bad choice -- config_store_
    // default()'s abs_max_temp_c is 0.0f, whose bytes are already all-zero,
    // so forcing it to 0 would be a no-op that leaves the CRC untouched and
    // proves nothing. Byte 0 (part of the magic, "KLC1") is never zero in a
    // real record -- forcing it to 0x00 is guaranteed to break the record.
    page0[0] = 0x00u; // clearing bits is always legal without an erase
    TEST_CHECK(hal_flash_program(&region_b, 0u, page0, sizeof(page0)) == HAL_OK,
               "fixture can corrupt sector B's record in place");

    // Reboot: sector B's slot 0 now fails CRC. Sector A's slot 7 (seq one
    // less than B's) must be found instead, SILENTLY -- no rejection is
    // reported, because a corrupt/superseded slot is not the "structurally-
    // valid-but-range-refused" case config_store_is_config_rejected() exists
    // to flag (see config_store.h's "Load-time rejection diagnostics").
    config_store_boot_load();
    TEST_CHECK(config_store_get_tc_type() == (uint8_t)((CONFIG_STORE_SLOTS_PER_SECTOR - 1u) % 8u),
               "corrupted sector B is skipped; sector A's last-good record is used instead");
    TEST_CHECK(config_store_is_config_rejected() == false,
               "falling back to the other sector is not reported as a rejection -- ordinary skip");
}

// --- Seqlock concurrency test (2026-09-09) ----------------------------------
//
// WHAT THIS DOES AND DOES NOT PROVE. This host test cannot model two
// Cortex-M0+ cores, their memory-ordering rules, or the AHB-Lite fabric
// between them -- there is no RP2040 in this build. What it CAN model
// honestly: config_store_record_t is 512 bytes, far bigger than any atomic
// access this compiler/CPU can do in one instruction, so a plain struct
// assignment (the pre-seqlock code: `s_cached_record = to_write;`) is
// necessarily a multi-store copy that a genuinely concurrent reader,
// running on a real OS thread that this host DOES schedule onto a separate
// physical core, can observe mid-copy. Two real Windows threads (one
// hammering config_store_write(), one hammering config_store_get_full_
// record()) is a real concurrent-memory-tearing hazard, just not the exact
// RP2040 cross-core hazard the seqlock was written for -- it exercises "is
// the read/write pair internally consistent under real concurrency", not
// "does this specific barrier sequence hold on Cortex-M0+ silicon". Treat a
// PASS here as evidence the seqlock protocol itself is sound under real
// concurrent access, not as proof the HAL_DMB() choice is correct on
// RP2040 hardware -- that requires the actual bench board.
//
// Detection method: each write sets TWO fields that live far apart inside
// the 512-byte record (tc_type near the front, estop_active_level near the
// end -- config_store.h's field layout) to values that encode the SAME
// alternating generation bit. A reader that ever observes the two fields
// disagreeing has necessarily read a struct that was partway through being
// overwritten -- there is no valid committed record in which they differ,
// since every write sets both from the same `g`.
typedef struct {
    volatile LONG stop;
    volatile LONG torn_count;
    volatile LONG consistent_count;
    volatile LONG write_count;
} seqlock_race_ctx_t;

static DWORD WINAPI seqlock_race_writer_fn(LPVOID param)
{
    seqlock_race_ctx_t *ctx = (seqlock_race_ctx_t *)param;
    uint8_t g = 0;
    while (!ctx->stop) {
        config_store_record_t rec;
        config_store_default(&rec);
        rec.calibration_missing = false;
        rec.tc_type = g ? 0x02u : 0x01u; // both <= 7 (config_params.c's RANGE_U8_MAX), no fields_set gating required
        rec.estop_active_level = g ? DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW
                                    : DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH;
        const char *reason = NULL;
        (void)config_store_write(&rec, &reason);
        InterlockedIncrement(&ctx->write_count);
        g = (uint8_t)(g ^ 1u);
    }
    return 0;
}

static DWORD WINAPI seqlock_race_reader_fn(LPVOID param)
{
    seqlock_race_ctx_t *ctx = (seqlock_race_ctx_t *)param;
    while (!ctx->stop) {
        config_store_record_t snap;
        config_store_get_full_record(&snap);
        uint8_t g_from_tc_type = (snap.tc_type == 0x02u) ? 1u : 0u;
        uint8_t g_from_estop = (snap.estop_active_level == DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW) ? 1u : 0u;
        if (g_from_tc_type != g_from_estop) {
            InterlockedIncrement(&ctx->torn_count);
        } else {
            InterlockedIncrement(&ctx->consistent_count);
        }
    }
    return 0;
}

static void test_seqlock_concurrent_read_never_tears(void)
{
    TEST_SECTION("config_store_flash: concurrent writer/reader never observes a torn record "
                 "(see this test's own header comment for exactly what this does and does not prove)");
    reset_all();
    config_store_boot_load();

    seqlock_race_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    HANDLE writer = CreateThread(NULL, 0, seqlock_race_writer_fn, &ctx, 0, NULL);
    HANDLE reader = CreateThread(NULL, 0, seqlock_race_reader_fn, &ctx, 0, NULL);
    TEST_CHECK(writer != NULL && reader != NULL, "fixture can start both race threads");

    Sleep(500); // real wall-clock window for the OS scheduler to interleave both threads
    InterlockedExchange(&ctx.stop, 1);
    WaitForSingleObject(writer, INFINITE);
    WaitForSingleObject(reader, INFINITE);
    CloseHandle(writer);
    CloseHandle(reader);

    printf("    (writes=%ld, consistent reads=%ld, torn reads=%ld)\n",
           ctx.write_count, ctx.consistent_count, ctx.torn_count);

    TEST_CHECK(ctx.write_count > 0, "sanity: the writer thread actually ran");
    TEST_CHECK(ctx.consistent_count > 0, "sanity: the reader thread actually ran");
    TEST_CHECK(ctx.torn_count == 0,
               "no reader ever observed tc_type/estop_active_level disagreeing -- "
               "the seqlock rejected or masked every torn snapshot");
}

// --- Multi-reader seqlock fallback test (2026-09-09, opus review) ----------
//
// The single-reader test above is exactly what let b202fe56/5671ee03's
// fallback bug through: with only one reader thread, there is no second
// reader around to race against config_store_seqlock_read()'s (pre-fix)
// unsynchronised `s_last_good_record = copy;` write, which every
// successful read used to perform regardless of whether that call was on
// the exhausted-retries fallback path or not. On real hardware the two
// concurrent readers are core-1's trip-path callers (safety_core.c,
// thermo_task.c, current_task.c) -- so this test spawns several reader
// threads against one writer thread, all real OS threads the host
// scheduler does put on separate physical cores, same rationale as the
// single-reader test's header comment above.
//
// SAME CAVEAT AS ABOVE APPLIES: this proves the read-side protocol has no
// internal data race under real concurrent access on x86/Windows threads;
// it does not and cannot prove the HAL_DMB()/hardware/sync.h barrier choice
// is correct on actual Cortex-M0+ silicon. That still requires the bench
// board.
#define SEQLOCK_RACE_READER_COUNT 4

static void test_seqlock_multi_reader_never_tears(void)
{
    TEST_SECTION("config_store_flash: MULTIPLE concurrent readers never observe a torn record "
                 "(regression test for the writer-owned-fallback fix -- a single reader thread "
                 "cannot exercise this)");
    reset_all();
    config_store_boot_load();
    config_store_test_fallback_taken_count_reset();

    seqlock_race_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    HANDLE writer = CreateThread(NULL, 0, seqlock_race_writer_fn, &ctx, 0, NULL);
    TEST_CHECK(writer != NULL, "fixture can start the writer thread");

    HANDLE readers[SEQLOCK_RACE_READER_COUNT];
    for (int i = 0; i < SEQLOCK_RACE_READER_COUNT; i++) {
        readers[i] = CreateThread(NULL, 0, seqlock_race_reader_fn, &ctx, 0, NULL);
        TEST_CHECK(readers[i] != NULL, "fixture can start each reader thread");
    }

    Sleep(500); // real wall-clock window for the OS scheduler to interleave all threads
    InterlockedExchange(&ctx.stop, 1);
    WaitForSingleObject(writer, INFINITE);
    CloseHandle(writer);
    for (int i = 0; i < SEQLOCK_RACE_READER_COUNT; i++) {
        WaitForSingleObject(readers[i], INFINITE);
        CloseHandle(readers[i]);
    }

    uint32_t fallback_taken = config_store_test_fallback_taken_count();
    printf("    (%d readers; writes=%ld, consistent reads=%ld, torn reads=%ld, "
           "fallback_taken=%lu)\n",
           SEQLOCK_RACE_READER_COUNT, ctx.write_count, ctx.consistent_count, ctx.torn_count,
           (unsigned long)fallback_taken);

    TEST_CHECK(ctx.write_count > 0, "sanity: the writer thread actually ran");
    TEST_CHECK(ctx.consistent_count > 0, "sanity: the reader threads actually ran");
    // opus review 2026-09-09: a torn_count==0 result proves nothing about the
    // fallback path specifically unless the fallback path actually ran at
    // least once during this test -- otherwise "0 torn" is equally
    // consistent with "the fallback was exercised and never tore" (what this
    // test claims) and "the primary seqlock always won and the fallback
    // branch never executed" (a vacuous pass). See s_fallback_taken_count's
    // comment in config_store_flash.c.
    TEST_CHECK(fallback_taken > 0,
               "the exhausted-retries fallback path was actually taken at least once during "
               "this race -- otherwise the torn_count==0 check below is vacuous");
    TEST_CHECK(ctx.torn_count == 0,
               "no reader ever observed tc_type/estop_active_level disagreeing with "
               "MULTIPLE concurrent readers -- the fallback double buffer's own seqlock "
               "(s_fallback_gen) means a reader that catches the writer mid-commit, "
               "including across two commits (ABA), retries instead of trusting a torn copy");
}

// --- Deterministic fallback-buffer ABA test (2026-09-09, opus review finding A) --
//
// The real-thread races above are honest about what they can and cannot
// prove (see that section's own header comment) -- and in practice, even
// heavily loaded, they did not land the exact two-commits-during-one-copy
// interleaving needed to exercise the ABA this fix closes: the window is a
// handful of instructions wide, not a useful fraction of an OS scheduling
// quantum. This test does not rely on OS thread timing at all. It uses
// config_store_test_force_fallback_path() to make config_store_seqlock_
// read() go straight to the fallback branch (single-threaded, no writer is
// ever actually racing the primary seqlock, so it would otherwise never
// fail on its own), and config_store_test_set_fallback_hook() to run two
// full config_store_write() commits from INSIDE the fallback branch, in the
// gap between the two halves of its own copy -- deterministically
// reproducing "the writer lands two commits while a reader is mid-copy of
// the fallback buffer", landing the second one back in the exact slot the
// reader started reading. See config_store_flash.c's s_fallback_gen comment
// for why this specific shape (index returns to the same value, but the
// buffer underneath was rewritten in between) defeats a bare index recheck.
static int s_aba_hook_calls;      // every time the production code invoked the hook pointer
static int s_aba_hook_commits_ran; // guards the actual writes to fire only once -- the fix's
                                    // own retry loop calls this hook again on every fallback
                                    // attempt it takes (up to CONFIG_STORE_FALLBACK_SEQLOCK_
                                    // MAX_RETRIES times), and re-running two more commits on a
                                    // later retry would just move the goalposts, not test
                                    // anything -- one deliberate double-commit is the scenario.

static void aba_two_commits_hook(void)
{
    s_aba_hook_calls++;
    if (s_aba_hook_commits_ran) {
        return;
    }
    s_aba_hook_commits_ran = 1;
    config_store_record_t rec;
    config_store_default(&rec);
    rec.calibration_missing = false;
    const char *reason = NULL;
    // First commit: writer targets the OTHER slot from whatever the reader
    // is currently reading and flips active there (safe on its own -- the
    // reader's slot is untouched by this one commit).
    rec.tc_type = 0x01u;
    rec.estop_active_level = DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH;
    TEST_CHECK(config_store_write(&rec, &reason), "ABA test: first in-flight commit succeeds");
    // Second commit: writer's target flips back to the reader's ORIGINAL
    // slot -- overwriting the exact buffer the reader is mid-copy of.
    rec.tc_type = 0x02u;
    rec.estop_active_level = DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW;
    TEST_CHECK(config_store_write(&rec, &reason), "ABA test: second in-flight commit succeeds");
}

static void test_fallback_aba_two_commits_during_one_copy(void)
{
    TEST_SECTION("config_store_flash: fallback buffer survives TWO writer commits landing "
                 "during one reader's copy (deterministic ABA regression test, opus review "
                 "finding A -- ff2506e9-shape fix)");
    reset_all();
    config_store_boot_load();

    // Seed the fallback buffer with a known, stable record (tc_type=0x01,
    // ESTOP_ACTIVE_HIGH) before forcing the race, so the "before" state is
    // well-defined and distinct from both in-flight commits the hook makes.
    config_store_record_t seed;
    config_store_default(&seed);
    seed.calibration_missing = false;
    seed.tc_type = 0x01u;
    seed.estop_active_level = DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH;
    const char *reason = NULL;
    TEST_CHECK(config_store_write(&seed, &reason), "fixture: seed commit succeeds");

    s_aba_hook_calls = 0;
    s_aba_hook_commits_ran = 0;
    config_store_test_force_fallback_path(true);
    config_store_test_set_fallback_hook(aba_two_commits_hook);

    config_store_record_t snap;
    memset(&snap, 0, sizeof(snap));
    config_store_get_full_record(&snap);

    config_store_test_set_fallback_hook(NULL);
    config_store_test_force_fallback_path(false);

    TEST_CHECK(s_aba_hook_calls >= 1,
               "fixture: the hook ran at least once during the read (the fix's own retry loop "
               "may call it again on a later attempt; only the first invocation's two commits "
               "actually run, see the hook's own guard)");
    TEST_CHECK(s_aba_hook_commits_ran == 1, "fixture: the two in-flight commits actually ran");

    uint8_t g_from_tc_type = (snap.tc_type == 0x02u) ? 1u : (snap.tc_type == 0x01u ? 0u : 0xFFu);
    uint8_t g_from_estop =
        (snap.estop_active_level == DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW)
            ? 1u
            : (snap.estop_active_level == DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH ? 0u : 0xFFu);
    printf("    (snap.tc_type=0x%02X snap.estop_active_level=%u -> g_tc=%u g_estop=%u)\n",
           snap.tc_type, snap.estop_active_level, g_from_tc_type, g_from_estop);
    TEST_CHECK(g_from_tc_type != 0xFFu && g_from_estop != 0xFFu,
               "fixture: the returned record carries one of the known generations in each field");
    TEST_CHECK(g_from_tc_type == g_from_estop,
               "the fallback seqlock's generation recheck caught the two in-flight commits and "
               "did not hand back a record mixing tc_type from one generation with "
               "estop_active_level from another -- this is the exact ABA a bare 0/1 index "
               "recheck cannot detect (the index legitimately returns to the same value)");
}

int main(void)
{
    test_boot_load_blank_sector_is_default();
    test_write_then_reload_round_trips();
    test_fallback_seeded_at_boot_before_any_write();
    test_write_refused_while_armed();
    test_seq_increments_and_survives_wraparound();
    test_safe_execute_timeout_is_reported_and_leaves_cache_unchanged();
    test_program_failure_is_not_masked_by_safe_execute_ok();
    test_switch_to_sector_b_after_sector_a_fills();
    test_wear_is_spread_across_both_sectors();
    test_power_loss_mid_erase_of_target_leaves_old_sector_valid();
    test_power_loss_mid_program_of_target_leaves_old_sector_valid();
    test_power_loss_between_erase_and_program_of_target_leaves_old_sector_valid();
    test_torn_inline_slot_is_never_reprogrammed();
    test_torn_first_slot_before_any_valid_record_is_never_reprogrammed();
    test_migration_from_legacy_single_sector_layout();
    test_corrupt_active_sector_falls_back_to_other_sector();
    test_seqlock_concurrent_read_never_tears();
    test_seqlock_multi_reader_never_tears();
    test_fallback_aba_two_commits_during_one_copy();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
