// Host tests pinning i2c_owner_io_read()'s three-way failure-cause logic and
// handle_io_read()'s mapping of it onto the wire status byte (cmd_task.c) --
// the CONFIRMED BUG this pass fixes: i2c_owner_io_read() collapsed "bad
// exp/pin", "level == NULL", and "no I2C sample has ever been taken" (no
// MCP23017 has ever ACKed a scan) into a single bare `false`, and
// handle_io_read() then ORed that into one SIMFW_CMD_STATUS_ERR_BAD_ARGS --
// so a perfectly legal IO/READ against an unpopulated bus (the bench-day
// case: no expander attached to J20) reported "bad arguments" instead of
// "no sample yet", sending the reader after their command syntax instead of
// their wiring.
//
// Why this file mirrors rather than calls the real code: i2c_owner.c
// includes pico-sdk (hardware/i2c.h, hardware/gpio.h) and FreeRTOS headers
// and drives a real MCP23017 over I2C0 -- it is not part of this host-test
// harness's source list (build_host_tests.ps1 compiles only src/sim/'s pure
// modules), the same pure/task boundary test_cmd_task_gap_closure.c's own
// header comment documents for cmd_task.c. There is no test_i2c_owner.c for
// the same reason. Each mirror function below is a deliberately small,
// byte-for-byte copy of the corresponding block in i2c_owner.c/cmd_task.c
// (cited in each function's comment) -- keep the two in sync by hand if
// either changes.
//
// What this DOES prove: the three-outcome decision logic (bad args vs. no
// sample vs. success) and cmd_task.c's status-byte mapping of it, entirely
// in pure C with no hardware. What this CANNOT prove, and would need real
// (or at minimum simulated-I2C) hardware to cover: that
// s_exp1_raw_valid/s_exp2_raw_valid are actually only ever set true by a
// successful mcp23017_read_gpio_word() in scan_tick() -- that fact is
// asserted by i2c_owner.c's own comments (i2c_owner.c lines ~123-127) and is
// not independently re-derived here.
#include <stdbool.h>
#include <stdint.h>

#include "test_common.h"

// --- mirror of i2c_owner.h's i2c_owner_io_read_status_t ---------------------
typedef enum {
    I2C_OWNER_IO_READ_OK = 0,
    I2C_OWNER_IO_READ_BAD_ARGS,
    I2C_OWNER_IO_READ_NO_SAMPLE,
} i2c_owner_io_read_status_t;

// Mirror of i2c_owner.c's io_pin_allowed(): pin > 15 is always rejected;
// exp1 pins 0..7 (EXP1_RESERVED_MAX_PIN) and pin 10 (DUT_POWER_SAFETY) are
// fixed-role and rejected only on exp1 -- exp2 has no reserved pins.
#define EXP1_RESERVED_MAX_PIN 7u
#define EXP1_PIN_DUT_POWER_SAFETY 10u
typedef enum { MIRROR_EXP_1 = 0, MIRROR_EXP_2 = 1 } mirror_expander_t;

static bool mirror_io_pin_allowed(mirror_expander_t exp, uint8_t pin)
{
    if (pin > 15u) {
        return false;
    }
    if (exp == MIRROR_EXP_1 && (pin <= EXP1_RESERVED_MAX_PIN || pin == EXP1_PIN_DUT_POWER_SAFETY)) {
        return false;
    }
    return true;
}

// Mirror of i2c_owner_io_read() (i2c_owner.c): bad-args check first (pin
// range/reserved, or a NULL out-param -- a caller bug), then the "has a
// scan sample ever landed for this expander" check (s_exp1_raw_valid /
// s_exp2_raw_valid, only ever set true by a successful
// mcp23017_read_gpio_word() call in scan_tick()). `raw_valid`/`raw_word`
// stand in for the real per-expander state i2c_owner.c guards with
// state_lock()/state_unlock().
static bool mirror_io_read(mirror_expander_t exp, uint8_t pin, bool *level, bool raw_valid, uint16_t raw_word,
                            i2c_owner_io_read_status_t *out_status)
{
    if (!mirror_io_pin_allowed(exp, pin) || !level) {
        if (out_status) {
            *out_status = I2C_OWNER_IO_READ_BAD_ARGS;
        }
        return false;
    }
    if (!raw_valid) {
        if (out_status) {
            *out_status = I2C_OWNER_IO_READ_NO_SAMPLE;
        }
        return false;
    }
    *level = (raw_word & (uint16_t)(1u << pin)) != 0;
    if (out_status) {
        *out_status = I2C_OWNER_IO_READ_OK;
    }
    return true;
}

// --- mirror of cmd_task.c's handle_io_read() status-byte mapping -----------
#define STATUS_OK           0x00u
#define STATUS_ERR_BAD_ARGS 0x02u
#define STATUS_ERR_NO_SAMPLE 0x05u

// Mirrors handle_io_read()'s post-read mapping: read_status ==
// NO_SAMPLE -> ERR_NO_SAMPLE, anything else failing -> ERR_BAD_ARGS,
// success -> OK. (The exp-range/args.overflow pre-check that also answers
// ERR_BAD_ARGS directly, before ever calling i2c_owner_io_read(), is not
// re-mirrored here -- it is an unchanged, pre-existing bound check this
// pass did not touch.)
static uint8_t mirror_handle_io_read_status(bool read_ok, i2c_owner_io_read_status_t read_status)
{
    if (read_ok) {
        return STATUS_OK;
    }
    return (read_status == I2C_OWNER_IO_READ_NO_SAMPLE) ? STATUS_ERR_NO_SAMPLE : STATUS_ERR_BAD_ARGS;
}

static void test_bad_pin_reports_bad_args(void)
{
    TEST_SECTION("i2c_owner_io_read -- reserved/out-of-range pin reports BAD_ARGS, not NO_SAMPLE");

    bool level = false;
    i2c_owner_io_read_status_t status = I2C_OWNER_IO_READ_OK;
    // exp1 pin 3 is inside the reserved 0..7 run even with a perfectly
    // valid raw sample sitting behind it -- the args check must short-
    // circuit before ever looking at raw_valid.
    bool ok = mirror_io_read(MIRROR_EXP_1, 3u, &level, /*raw_valid=*/true, /*raw_word=*/0xFFFFu, &status);
    TEST_CHECK(!ok, "reserved exp1 pin 3 is rejected");
    TEST_CHECK(status == I2C_OWNER_IO_READ_BAD_ARGS, "reserved pin reports BAD_ARGS");

    status = I2C_OWNER_IO_READ_OK;
    ok = mirror_io_read(MIRROR_EXP_1, 16u, &level, /*raw_valid=*/true, /*raw_word=*/0xFFFFu, &status);
    TEST_CHECK(!ok, "pin 16 (one past the last valid pin) is rejected");
    TEST_CHECK(status == I2C_OWNER_IO_READ_BAD_ARGS, "out-of-range pin reports BAD_ARGS");

    status = I2C_OWNER_IO_READ_OK;
    ok = mirror_io_read(MIRROR_EXP_1, 10u, &level, /*raw_valid=*/true, /*raw_word=*/0xFFFFu, &status);
    TEST_CHECK(!ok, "exp1 pin 10 (DUT_POWER_SAFETY, reserved outside the 0..7 run) is rejected");
    TEST_CHECK(status == I2C_OWNER_IO_READ_BAD_ARGS, "reserved pin 10 reports BAD_ARGS");
}

static void test_null_level_reports_bad_args(void)
{
    TEST_SECTION("i2c_owner_io_read -- NULL level out-param reports BAD_ARGS");

    i2c_owner_io_read_status_t status = I2C_OWNER_IO_READ_OK;
    bool ok = mirror_io_read(MIRROR_EXP_1, 8u, NULL, /*raw_valid=*/true, /*raw_word=*/0xFFFFu, &status);
    TEST_CHECK(!ok, "a NULL level pointer is rejected even for an otherwise-legal pin");
    TEST_CHECK(status == I2C_OWNER_IO_READ_BAD_ARGS, "NULL level reports BAD_ARGS");
}

static void test_no_sample_yet_reports_no_sample(void)
{
    TEST_SECTION("i2c_owner_io_read -- valid pin, no scan sample yet, reports NO_SAMPLE (the bug fix)");

    bool level = true; // pre-set to a sentinel to prove it's untouched on failure
    i2c_owner_io_read_status_t status = I2C_OWNER_IO_READ_OK;
    // exp1 pin 8, exp2 pin 0: both generic/unreserved (i2c_owner.h 197-202),
    // but raw_valid is false -- the exact bench state with no MCP23017
    // attached to J20, before this pass's fix this indistinguishably
    // reported the same BAD_ARGS as a genuinely malformed request.
    bool ok = mirror_io_read(MIRROR_EXP_1, 8u, &level, /*raw_valid=*/false, /*raw_word=*/0u, &status);
    TEST_CHECK(!ok, "no scan sample yet on exp1 -- read fails");
    TEST_CHECK(status == I2C_OWNER_IO_READ_NO_SAMPLE, "reports NO_SAMPLE, not BAD_ARGS, for a legal pin with no sample");
    TEST_CHECK(level == true, "level out-param is left untouched on a NO_SAMPLE failure");

    status = I2C_OWNER_IO_READ_OK;
    ok = mirror_io_read(MIRROR_EXP_2, 0u, &level, /*raw_valid=*/false, /*raw_word=*/0u, &status);
    TEST_CHECK(!ok, "no scan sample yet on exp2 -- read fails");
    TEST_CHECK(status == I2C_OWNER_IO_READ_NO_SAMPLE, "exp2's NO_SAMPLE path is independent of exp1's");
}

static void test_valid_sample_succeeds(void)
{
    TEST_SECTION("i2c_owner_io_read -- valid pin with a real sample succeeds");

    bool level = false;
    i2c_owner_io_read_status_t status = I2C_OWNER_IO_READ_BAD_ARGS; // sentinel, must be overwritten to OK
    bool ok = mirror_io_read(MIRROR_EXP_1, 8u, &level, /*raw_valid=*/true, /*raw_word=*/(uint16_t)(1u << 8), &status);
    TEST_CHECK(ok, "a legal pin with a valid sample succeeds");
    TEST_CHECK(status == I2C_OWNER_IO_READ_OK, "success reports OK");
    TEST_CHECK(level == true, "level is decoded from the correct bit of raw_word");

    ok = mirror_io_read(MIRROR_EXP_1, 9u, &level, /*raw_valid=*/true, /*raw_word=*/(uint16_t)(1u << 8), &status);
    TEST_CHECK(ok, "an adjacent pin also succeeds against the same sample");
    TEST_CHECK(level == false, "an unset bit decodes to false");
}

static void test_handle_io_read_status_mapping(void)
{
    TEST_SECTION("handle_io_read -- status-byte mapping (the CONFIRMED BUG's fix)");

    TEST_CHECK(mirror_handle_io_read_status(true, I2C_OWNER_IO_READ_OK) == STATUS_OK,
               "a successful read maps to STATUS_OK");
    TEST_CHECK(mirror_handle_io_read_status(false, I2C_OWNER_IO_READ_BAD_ARGS) == STATUS_ERR_BAD_ARGS,
               "a bad-args failure maps to STATUS_ERR_BAD_ARGS");
    TEST_CHECK(mirror_handle_io_read_status(false, I2C_OWNER_IO_READ_NO_SAMPLE) == STATUS_ERR_NO_SAMPLE,
               "a no-sample failure now maps to the new STATUS_ERR_NO_SAMPLE, not STATUS_ERR_BAD_ARGS "
               "-- this is the exact behavior change the CONFIRMED BUG report asked for");
}

void run_test_i2c_owner_io_read(void)
{
    test_bad_pin_reports_bad_args();
    test_null_level_reports_bad_args();
    test_no_sample_yet_reports_no_sample();
    test_valid_sample_succeeds();
    test_handle_io_read_status_mapping();
}
