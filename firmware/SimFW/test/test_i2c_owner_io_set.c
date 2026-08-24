// Host tests pinning i2c_owner_io_set_dir()/i2c_owner_io_write()'s two-way
// failure-cause logic and cmd_task.c's handle_io_set_dir()/handle_io_write()
// mapping of it onto the wire status byte -- the CONFIRMED BUG this pass
// fixes: both setters collapsed "reserved/out-of-range pin" (permanent,
// retry never helps) and "command queue transiently full" (i2c_owner's own
// task drains it on its next ~8 ms scan tick, retry likely succeeds) into a
// single bare `false`, and both cmd_task.c handlers then mapped that bare
// `false` unconditionally to SIMFW_CMD_STATUS_ERR_BAD_ARGS -- so a
// perfectly legal SET_DIR/WRITE that merely lost a race against a full
// command queue reported "bad arguments" instead of "busy, retry", sending
// the reader after their command syntax instead of a transient condition
// their own retry would very likely clear.
//
// Same species of bug as i2c_owner_io_read()'s (test_i2c_owner_io_read.c,
// fixed earlier this pass), and this file follows that one's precedent
// exactly, including the reason it mirrors rather than calls the real code:
// i2c_owner.c includes pico-sdk (hardware/i2c.h, hardware/gpio.h) and
// FreeRTOS headers and drives real I2C0/xQueueSend() -- it is not part of
// this host-test harness's source list (build_host_tests.ps1 compiles only
// src/sim/'s pure modules), the same pure/task boundary
// test_cmd_task_gap_closure.c's and test_i2c_owner_io_read.c's own header
// comments document. There is no test_i2c_owner.c for the same reason. Each
// mirror function below is a deliberately small, byte-for-byte copy of the
// corresponding block in i2c_owner.c/cmd_task.c (cited in each function's
// comment) -- keep the two in sync by hand if either changes.
//
// What this DOES prove: the two-outcome decision logic (bad args vs. queue
// full vs. success) and cmd_task.c's status-byte mapping of it, entirely in
// pure C with no hardware/FreeRTOS. What this CANNOT prove, and would need
// real (or at minimum simulated-I2C/FreeRTOS) hardware to cover: that
// xQueueSend() genuinely returns pdFALSE only when I2C_OWNER_CMD_QUEUE_DEPTH
// entries are already queued -- that fact is asserted by i2c_owner.c's own
// use of FreeRTOS's queue API and is not independently re-derived here.
#include <stdbool.h>
#include <stdint.h>

#include "test_common.h"

// --- mirror of i2c_owner.h's i2c_owner_io_set_status_t ---------------------
typedef enum {
    I2C_OWNER_IO_SET_OK = 0,
    I2C_OWNER_IO_SET_BAD_ARGS,
    I2C_OWNER_IO_SET_QUEUE_FULL,
} i2c_owner_io_set_status_t;

// Mirror of i2c_owner.c's io_pin_allowed() -- identical to
// test_i2c_owner_io_read.c's mirror_io_pin_allowed(), duplicated here rather
// than shared because each test file is a standalone, independently
// buildable translation unit in this harness (see build_host_tests.ps1's
// per-file compile list).
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

// Mirror of i2c_owner_io_set_dir()/i2c_owner_io_write() (i2c_owner.c, after
// this pass's fix): bad-args check first (pin range/reserved, or the
// internal "no command queue" case -- folded into BAD_ARGS per this pass's
// instruction, since it should never happen post-boot and is permanent like
// a reserved pin), then the xQueueSend() outcome. `queue_send_ok` stands in
// for the real xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE call i2c_owner.c
// guards behind no lock (the command queue is itself the synchronization
// primitive).
static bool mirror_io_set(mirror_expander_t exp, uint8_t pin, bool cmd_queue_exists, bool queue_send_ok,
                           i2c_owner_io_set_status_t *out_status)
{
    if (!cmd_queue_exists || !mirror_io_pin_allowed(exp, pin)) {
        if (out_status) {
            *out_status = I2C_OWNER_IO_SET_BAD_ARGS;
        }
        return false;
    }
    if (!queue_send_ok) {
        if (out_status) {
            *out_status = I2C_OWNER_IO_SET_QUEUE_FULL;
        }
        return false;
    }
    if (out_status) {
        *out_status = I2C_OWNER_IO_SET_OK;
    }
    return true;
}

// --- mirror of cmd_task.c's handle_io_set_dir()/handle_io_write() status-byte mapping ---
#define STATUS_OK           0x00u
#define STATUS_ERR_BAD_ARGS 0x02u
#define STATUS_ERR_BUSY     0x04u

// Mirrors both handlers' post-call mapping: set_status == QUEUE_FULL ->
// ERR_BUSY, anything else failing -> ERR_BAD_ARGS, success -> OK. (The
// exp-range/args.overflow pre-check that also answers ERR_BAD_ARGS directly,
// before ever calling the real i2c_owner_io_set_dir()/_io_write(), is not
// re-mirrored here -- it is an unchanged, pre-existing bound check this pass
// did not touch, same as test_i2c_owner_io_read.c's equivalent note.)
static uint8_t mirror_handle_io_set_status(bool set_ok, i2c_owner_io_set_status_t set_status)
{
    if (set_ok) {
        return STATUS_OK;
    }
    return (set_status == I2C_OWNER_IO_SET_QUEUE_FULL) ? STATUS_ERR_BUSY : STATUS_ERR_BAD_ARGS;
}

static void test_reserved_pin_reports_bad_args(void)
{
    TEST_SECTION("i2c_owner_io_set_dir/_write -- reserved/out-of-range pin reports BAD_ARGS, not QUEUE_FULL");

    i2c_owner_io_set_status_t status = I2C_OWNER_IO_SET_OK;
    // exp1 pin 3 is inside the reserved 0..7 run, even with the command
    // queue present and xQueueSend() that would otherwise succeed -- the
    // args check must short-circuit before ever touching the queue.
    bool ok = mirror_io_set(MIRROR_EXP_1, 3u, /*cmd_queue_exists=*/true, /*queue_send_ok=*/true, &status);
    TEST_CHECK(!ok, "reserved exp1 pin 3 is rejected");
    TEST_CHECK(status == I2C_OWNER_IO_SET_BAD_ARGS, "reserved pin reports BAD_ARGS");

    status = I2C_OWNER_IO_SET_OK;
    ok = mirror_io_set(MIRROR_EXP_1, 10u, /*cmd_queue_exists=*/true, /*queue_send_ok=*/true, &status);
    TEST_CHECK(!ok, "exp1 pin 10 (DUT_POWER_SAFETY, reserved outside the 0..7 run) is rejected");
    TEST_CHECK(status == I2C_OWNER_IO_SET_BAD_ARGS, "reserved pin 10 reports BAD_ARGS");

    status = I2C_OWNER_IO_SET_OK;
    ok = mirror_io_set(MIRROR_EXP_1, 16u, /*cmd_queue_exists=*/true, /*queue_send_ok=*/true, &status);
    TEST_CHECK(!ok, "pin 16 (one past the last valid pin) is rejected");
    TEST_CHECK(status == I2C_OWNER_IO_SET_BAD_ARGS, "out-of-range pin reports BAD_ARGS");
}

static void test_missing_cmd_queue_reports_bad_args(void)
{
    TEST_SECTION("i2c_owner_io_set_dir/_write -- missing command queue (internal error) reports BAD_ARGS");

    i2c_owner_io_set_status_t status = I2C_OWNER_IO_SET_OK;
    // exp1 pin 8 is otherwise perfectly legal (generic J20 IO_3), but the
    // internal-error "queue was never created" case is folded into
    // BAD_ARGS per this pass's instruction: it should never happen post-
    // boot and, like a reserved pin, retrying can never fix it.
    bool ok = mirror_io_set(MIRROR_EXP_1, 8u, /*cmd_queue_exists=*/false, /*queue_send_ok=*/true, &status);
    TEST_CHECK(!ok, "a missing command queue rejects an otherwise-legal pin");
    TEST_CHECK(status == I2C_OWNER_IO_SET_BAD_ARGS, "missing command queue reports BAD_ARGS, not QUEUE_FULL");
}

static void test_full_queue_reports_queue_full(void)
{
    TEST_SECTION("i2c_owner_io_set_dir/_write -- legal pin, full command queue reports QUEUE_FULL (the bug fix)");

    i2c_owner_io_set_status_t status = I2C_OWNER_IO_SET_OK;
    // exp1 pin 8, exp2 pin 0: both generic/unreserved (i2c_owner.h 197-202),
    // command queue exists, but xQueueSend() itself fails (I2C_OWNER_CMD_QUEUE_DEPTH
    // entries already queued) -- the exact "i2c_owner's scan task hasn't
    // drained its queue yet" condition that, before this pass's fix,
    // indistinguishably reported the same BAD_ARGS as a genuinely reserved
    // pin.
    bool ok = mirror_io_set(MIRROR_EXP_1, 8u, /*cmd_queue_exists=*/true, /*queue_send_ok=*/false, &status);
    TEST_CHECK(!ok, "a full command queue on exp1 -- set fails");
    TEST_CHECK(status == I2C_OWNER_IO_SET_QUEUE_FULL,
               "reports QUEUE_FULL, not BAD_ARGS, for a legal pin against a full queue");

    status = I2C_OWNER_IO_SET_OK;
    ok = mirror_io_set(MIRROR_EXP_2, 0u, /*cmd_queue_exists=*/true, /*queue_send_ok=*/false, &status);
    TEST_CHECK(!ok, "a full command queue on exp2 -- set fails");
    TEST_CHECK(status == I2C_OWNER_IO_SET_QUEUE_FULL, "exp2's QUEUE_FULL path is independent of exp1's");
}

static void test_valid_send_succeeds(void)
{
    TEST_SECTION("i2c_owner_io_set_dir/_write -- valid pin with a successful queue send succeeds");

    i2c_owner_io_set_status_t status = I2C_OWNER_IO_SET_BAD_ARGS; // sentinel, must be overwritten to OK
    bool ok = mirror_io_set(MIRROR_EXP_1, 8u, /*cmd_queue_exists=*/true, /*queue_send_ok=*/true, &status);
    TEST_CHECK(ok, "a legal pin with a successful queue send succeeds");
    TEST_CHECK(status == I2C_OWNER_IO_SET_OK, "success reports OK");
}

static void test_handle_io_set_status_mapping(void)
{
    TEST_SECTION("handle_io_set_dir/handle_io_write -- status-byte mapping (the CONFIRMED BUG's fix)");

    TEST_CHECK(mirror_handle_io_set_status(true, I2C_OWNER_IO_SET_OK) == STATUS_OK,
               "a successful set/write maps to STATUS_OK");
    TEST_CHECK(mirror_handle_io_set_status(false, I2C_OWNER_IO_SET_BAD_ARGS) == STATUS_ERR_BAD_ARGS,
               "a bad-args failure maps to STATUS_ERR_BAD_ARGS");
    TEST_CHECK(mirror_handle_io_set_status(false, I2C_OWNER_IO_SET_QUEUE_FULL) == STATUS_ERR_BUSY,
               "a queue-full failure now maps to STATUS_ERR_BUSY, not STATUS_ERR_BAD_ARGS -- this is "
               "the exact behavior change the CONFIRMED BUG report asked for");
}

void run_test_i2c_owner_io_set(void)
{
    test_reserved_pin_reports_bad_args();
    test_missing_cmd_queue_reports_bad_args();
    test_full_queue_reports_queue_full();
    test_valid_send_succeeds();
    test_handle_io_set_status_mapping();
}
