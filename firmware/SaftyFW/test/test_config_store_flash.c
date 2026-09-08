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

#include "test_common.h"

#include "config_store.h"
#include "config_store_flash_host_stubs.h"
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

int main(void)
{
    test_boot_load_blank_sector_is_default();
    test_write_then_reload_round_trips();
    test_write_refused_while_armed();
    test_seq_increments_and_survives_wraparound();
    test_safe_execute_timeout_is_reported_and_leaves_cache_unchanged();
    test_program_failure_is_not_masked_by_safe_execute_ok();
    test_switch_to_sector_b_after_sector_a_fills();
    test_wear_is_spread_across_both_sectors();
    test_power_loss_mid_erase_of_target_leaves_old_sector_valid();
    test_power_loss_mid_program_of_target_leaves_old_sector_valid();
    test_power_loss_between_erase_and_program_of_target_leaves_old_sector_valid();
    test_migration_from_legacy_single_sector_layout();
    test_corrupt_active_sector_falls_back_to_other_sector();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
