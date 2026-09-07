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
#define TEST_FLASH_TOTAL_SIZE \
    (SAFTYFW_CONFIG_STORE_FLASH_OFFSET + SAFTYFW_CONFIG_STORE_FLASH_SIZE)

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

int main(void)
{
    test_boot_load_blank_sector_is_default();
    test_write_then_reload_round_trips();
    test_write_refused_while_armed();
    test_seq_increments_and_survives_wraparound();
    test_safe_execute_timeout_is_reported_and_leaves_cache_unchanged();
    test_program_failure_is_not_masked_by_safe_execute_ok();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
