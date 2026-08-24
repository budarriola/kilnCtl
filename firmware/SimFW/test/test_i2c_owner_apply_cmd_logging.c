// Host tests pinning i2c_owner.c's apply_pending_commands() failure-logging
// fix (2026-08-24 robustness audit): before this pass, five of the six
// command cases in that switch either discarded the mcp23017_*() write's
// bool return outright (IO_SET_DIR, IO_WRITE) or checked it only to gate a
// local shadow-state update, with no trace anywhere that the write failed
// (SET_ESTOP, SET_DUT_POWER_MAIN, SET_DUT_POWER_SAFETY). Every one of these
// commands is posted through i2c_owner_io_set_dir()/_io_write()/_set_estop()/
// _set_dut_power_*(), which report "queued" (I2C_OWNER_IO_SET_OK) back to
// the caller the instant xQueueSend() succeeds -- long before this task-body
// code ever runs the real I2C transaction. A caller that got "OK" had no way
// to ever learn the physical write never happened: not a wire-visible
// status (queuing already succeeded), not a log line (none was emitted).
//
// The fix adds a log_task_log(LOG_LEVEL_ERROR, "i2c_owner", ...) call on
// each write-failure path, so a bench operator polling the retained log ring
// (log_task_get_entries()) has a trace of the failure even though the wire
// protocol has no way to carry it back to that specific command.
//
// Same mirror-not-call rationale as test_i2c_owner_io_read.c/
// test_i2c_owner_io_set.c: i2c_owner.c pulls in pico-sdk (hardware/i2c.h)
// and FreeRTOS headers and is not part of this host-test harness's source
// list. Each mirror function below is a deliberately small, byte-for-byte
// copy of the corresponding case in i2c_owner.c's apply_pending_commands(),
// with mcp23017_pin_write()/_pin_set_dir()/_pin_set_pullup() replaced by a
// caller-supplied bool standing in for the real I2C transaction's outcome,
// and log_task_log() replaced by a counter increment.
#include <stdbool.h>
#include <stdint.h>

#include "test_common.h"

static int g_mirror_log_error_count;
static int g_mirror_state_update_count;

static void mirror_log_error(void)
{
    g_mirror_log_error_count++;
}

// Mirror of the I2C_OWNER_CMD_SET_ESTOP case (i2c_owner.c apply_pending_commands()):
// `open` takes the direction-toggle branch (a single write), `!open` takes
// the write-then-direction branch (two writes, `ok` is their AND) -- both
// gate the shadow-state update on `ok` and, after this pass, log on !ok.
static void mirror_apply_set_estop(bool open, bool dir_write_ok, bool level_write_ok)
{
    bool ok = open ? dir_write_ok : (level_write_ok && dir_write_ok);
    if (ok) {
        g_mirror_state_update_count++;
    } else {
        mirror_log_error();
    }
}

// Mirror of I2C_OWNER_CMD_SET_DUT_POWER_MAIN / _SAFETY (identical shape,
// one write each).
static void mirror_apply_set_dut_power(bool write_ok)
{
    if (write_ok) {
        g_mirror_state_update_count++;
    } else {
        mirror_log_error();
    }
}

// Mirror of I2C_OWNER_CMD_IO_SET_DIR: two independent writes (direction,
// then pullup), no shadow state of i2c_owner's own to gate -- the fix logs
// if EITHER write failed, matching the real code's `!dir_ok || !pullup_ok`.
static void mirror_apply_io_set_dir(bool dir_ok, bool pullup_ok)
{
    if (!dir_ok || !pullup_ok) {
        mirror_log_error();
    }
}

// Mirror of I2C_OWNER_CMD_IO_WRITE: one write, logs on failure.
static void mirror_apply_io_write(bool write_ok)
{
    if (!write_ok) {
        mirror_log_error();
    }
}

static void reset_counters(void)
{
    g_mirror_log_error_count = 0;
    g_mirror_state_update_count = 0;
}

static void test_estop_logs_only_on_failure(void)
{
    TEST_SECTION("apply_pending_commands SET_ESTOP -- logs on failure, updates state on success, never both");

    reset_counters();
    mirror_apply_set_estop(/*open=*/true, /*dir_write_ok=*/true, /*level_write_ok=*/true);
    TEST_CHECK(g_mirror_state_update_count == 1, "open+success updates shadow state");
    TEST_CHECK(g_mirror_log_error_count == 0, "open+success does not log");

    reset_counters();
    mirror_apply_set_estop(/*open=*/true, /*dir_write_ok=*/false, /*level_write_ok=*/true);
    TEST_CHECK(g_mirror_state_update_count == 0, "open+failed dir write leaves shadow state untouched");
    TEST_CHECK(g_mirror_log_error_count == 1,
               "open+failed dir write logs an error -- THE FIX: this used to be silent");

    reset_counters();
    mirror_apply_set_estop(/*open=*/false, /*dir_write_ok=*/true, /*level_write_ok=*/true);
    TEST_CHECK(g_mirror_state_update_count == 1, "close+both writes ok updates shadow state");
    TEST_CHECK(g_mirror_log_error_count == 0, "close+both writes ok does not log");

    reset_counters();
    mirror_apply_set_estop(/*open=*/false, /*dir_write_ok=*/true, /*level_write_ok=*/false);
    TEST_CHECK(g_mirror_state_update_count == 0,
               "close+failed LEVEL write (dir write still ok) still counts as failed overall");
    TEST_CHECK(g_mirror_log_error_count == 1,
               "close+failed level write logs an error -- THE FIX: this used to be silent");
}

static void test_dut_power_logs_only_on_failure(void)
{
    TEST_SECTION("apply_pending_commands SET_DUT_POWER_MAIN/SAFETY -- logs on failure, updates state on success");

    reset_counters();
    mirror_apply_set_dut_power(/*write_ok=*/true);
    TEST_CHECK(g_mirror_state_update_count == 1, "successful write updates shadow state");
    TEST_CHECK(g_mirror_log_error_count == 0, "successful write does not log");

    reset_counters();
    mirror_apply_set_dut_power(/*write_ok=*/false);
    TEST_CHECK(g_mirror_state_update_count == 0, "failed write leaves shadow state untouched");
    TEST_CHECK(g_mirror_log_error_count == 1,
               "failed write logs an error -- THE FIX: this used to be silent");
}

static void test_io_set_dir_logs_on_either_write_failing(void)
{
    TEST_SECTION("apply_pending_commands IO_SET_DIR -- logs if EITHER the direction or pullup write fails "
                 "(THE CONFIRMED BUG: both return values used to be discarded outright)");

    reset_counters();
    mirror_apply_io_set_dir(/*dir_ok=*/true, /*pullup_ok=*/true);
    TEST_CHECK(g_mirror_log_error_count == 0, "both writes ok: no log");

    reset_counters();
    mirror_apply_io_set_dir(/*dir_ok=*/false, /*pullup_ok=*/true);
    TEST_CHECK(g_mirror_log_error_count == 1, "direction write failed alone: logs -- used to be silent");

    reset_counters();
    mirror_apply_io_set_dir(/*dir_ok=*/true, /*pullup_ok=*/false);
    TEST_CHECK(g_mirror_log_error_count == 1, "pullup write failed alone: logs -- used to be silent");

    reset_counters();
    mirror_apply_io_set_dir(/*dir_ok=*/false, /*pullup_ok=*/false);
    TEST_CHECK(g_mirror_log_error_count == 1, "both writes failed: logs exactly once, not twice");
}

static void test_io_write_logs_on_failure(void)
{
    TEST_SECTION("apply_pending_commands IO_WRITE -- logs on failure "
                 "(THE CONFIRMED BUG: the return value used to be discarded outright)");

    reset_counters();
    mirror_apply_io_write(/*write_ok=*/true);
    TEST_CHECK(g_mirror_log_error_count == 0, "successful write: no log");

    reset_counters();
    mirror_apply_io_write(/*write_ok=*/false);
    TEST_CHECK(g_mirror_log_error_count == 1, "failed write logs an error -- used to be silent");
}

void run_test_i2c_owner_apply_cmd_logging(void)
{
    test_estop_logs_only_on_failure();
    test_dut_power_logs_only_on_failure();
    test_io_set_dir_logs_on_either_write_failing();
    test_io_write_logs_on_failure();
}
