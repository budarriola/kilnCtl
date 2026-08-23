// Host tests pinning the pure decision logic behind SimFW's USB-triggered
// reflash path (SIMFW_CMD_SYS_REBOOT_BOOTLOADER, cmd_task.c; the 1200-baud
// touch convention, usb_owner.c; the safe-state confirmation loop,
// src/tasks/safe_reboot.c).
//
// Why this file mirrors rather than calls the real code: safe_reboot.c
// includes FreeRTOS.h/task.h and pico/bootrom.h, and cmd_task.c/usb_owner.c
// are FreeRTOS task files that pull in i2c_owner.h/wave_owner.h/usb_owner.h
// task APIs -- none of that is part of this host-test harness's source list
// (build_host_tests.ps1 compiles only src/sim/'s pure modules), the exact
// same pure/task boundary test_cmd_task_gap_closure.c's own header comment
// already documents. Each function below is a deliberately small,
// byte-for-byte mirror of the corresponding block in the real file (cited in
// each function's comment) -- keep the two in sync by hand if either
// changes.
#include <stdbool.h>
#include <stdint.h>

#include "test_common.h"

// --- SIMFW_CMD_SYS_REBOOT_BOOTLOADER magic-confirm gate ---------------------
// Mirrors handle_sys_reboot_bootloader()'s magic check (cmd_task.c):
// SIMFW_CMD_SYS_REBOOT_BOOTLOADER_MAGIC == 0xB007B007 (cmd_ids.h). Anything
// else is refused with ERR_BAD_ARGS before safe_reboot_into_bootloader() is
// ever called -- this is the "malformed frame" half of the accidental-
// trigger case: a request that fails to decode, or decodes but carries the
// wrong confirm value, must never reach the safe-reboot sequence at all.
#define MIRROR_REBOOT_MAGIC 0xB007B007u

static bool mirror_reboot_magic_ok(uint32_t confirm)
{
    return confirm == MIRROR_REBOOT_MAGIC;
}

static void test_reboot_bootloader_magic_gate(void)
{
    TEST_SECTION("SYS_REBOOT_BOOTLOADER -- magic-confirm gate");

    TEST_CHECK(mirror_reboot_magic_ok(0xB007B007u), "the documented magic value is accepted");
    TEST_CHECK(!mirror_reboot_magic_ok(0u), "an all-zero (e.g. truncated/garbage) payload is refused");
    TEST_CHECK(!mirror_reboot_magic_ok(0xB007B006u), "one bit off the real magic is refused");
    TEST_CHECK(!mirror_reboot_magic_ok(0xFFFFFFFFu), "an unrelated payload is refused");
}

// --- 1200-baud touch-convention gate -----------------------------------
// Mirrors usb_owner.c's usb_owner_check_bootloader_touch(): triggers only
// when BOTH the last-seen CDC line-coding baud is exactly 1200 AND DTR is
// currently deasserted. This is the accidental-trigger case the task
// explicitly calls out: a scenario runner (or any other tool) opening this
// same CDC port at its own usual baud rate must never reboot the fixture
// mid-run, so the gate must reject baud==1200-with-DTR-asserted and
// baud!=1200-with-DTR-deasserted just as firmly as it accepts the
// conjunction.
#define MIRROR_TOUCH_BAUD 1200u

static bool mirror_bootloader_touch(uint32_t baud, bool dtr_asserted)
{
    return baud == MIRROR_TOUCH_BAUD && !dtr_asserted;
}

static void test_bootloader_touch_gate_positive(void)
{
    TEST_SECTION("1200-baud touch -- the real convention fires");
    TEST_CHECK(mirror_bootloader_touch(1200u, false), "1200 baud + DTR deasserted triggers");
}

static void test_bootloader_touch_gate_negative_baud_only(void)
{
    TEST_SECTION("1200-baud touch -- baud alone (DTR still asserted) must NOT fire");
    // This is the case pico-sdk's own stdio_usb reference implementation
    // (reset_interface.c) does NOT guard against -- it triggers on baud
    // alone. This project's own convention is stricter on purpose: gating
    // on DTR too is what keeps an ordinary "some tool set 1200 baud for its
    // own reason but never touched DTR" from being enough by itself.
    TEST_CHECK(!mirror_bootloader_touch(1200u, true), "1200 baud with DTR still asserted does not trigger");
}

static void test_bootloader_touch_gate_negative_dtr_only(void)
{
    TEST_SECTION("1200-baud touch -- DTR-deasserted at another baud must NOT fire");
    // The exact "stray baud-rate change / scenario runner reconnect" case
    // the task calls out: closing a port often deasserts DTR regardless of
    // baud, so DTR-deasserted-at-some-other-baud (9600, 115200, ...) must
    // not be enough on its own.
    TEST_CHECK(!mirror_bootloader_touch(9600u, false), "9600 baud with DTR deasserted does not trigger");
    TEST_CHECK(!mirror_bootloader_touch(115200u, false), "115200 baud with DTR deasserted does not trigger");
    TEST_CHECK(!mirror_bootloader_touch(0u, false), "the pre-any-line-coding-change sentinel (0) never trigger");
}

static void test_bootloader_touch_gate_both_conditions_false(void)
{
    TEST_SECTION("1200-baud touch -- neither condition true");
    TEST_CHECK(!mirror_bootloader_touch(115200u, true), "an ordinary port open (real baud, DTR asserted) does not trigger");
}

// --- safe_reboot_into_bootloader()'s safe_state_confirmed() mirror ---------
// Mirrors safe_reboot.c's safe_state_confirmed(): ALL of estop-open,
// dut-power-main-off, dut-power-safety-off, and every CT channel silenced
// (MANUAL mode, valid readback, amps == 0) must hold simultaneously.
typedef struct {
    bool estop_open;
    bool dut_power_main_on;
    bool dut_power_safety_on;
    bool ct_valid[3];
    bool ct_manual[3];
    float ct_amps[3];
} mirror_fixture_state_t;

static bool mirror_safe_state_confirmed(const mirror_fixture_state_t *s)
{
    if (!s->estop_open) {
        return false;
    }
    if (s->dut_power_main_on) {
        return false;
    }
    if (s->dut_power_safety_on) {
        return false;
    }
    for (int ch = 0; ch < 3; ch++) {
        if (!s->ct_valid[ch]) {
            return false;
        }
        if (!s->ct_manual[ch] || s->ct_amps[ch] != 0.0f) {
            return false;
        }
    }
    return true;
}

static mirror_fixture_state_t mirror_all_safe(void)
{
    mirror_fixture_state_t s;
    s.estop_open = true;
    s.dut_power_main_on = false;
    s.dut_power_safety_on = false;
    for (int ch = 0; ch < 3; ch++) {
        s.ct_valid[ch] = true;
        s.ct_manual[ch] = true;
        s.ct_amps[ch] = 0.0f;
    }
    return s;
}

static void test_safe_state_confirmed_all_conditions_met(void)
{
    TEST_SECTION("safe_state_confirmed -- every condition met");
    mirror_fixture_state_t s = mirror_all_safe();
    TEST_CHECK(mirror_safe_state_confirmed(&s), "fully safe fixture state confirms");
}

static void test_safe_state_confirmed_estop_still_closed(void)
{
    TEST_SECTION("safe_state_confirmed -- E-stop loop still closed refuses");
    mirror_fixture_state_t s = mirror_all_safe();
    s.estop_open = false;
    TEST_CHECK(!mirror_safe_state_confirmed(&s), "estop_open == false (loop still closed/healthy) refuses confirmation");
}

static void test_safe_state_confirmed_dut_power_main_still_on(void)
{
    TEST_SECTION("safe_state_confirmed -- DUT power (main) still on refuses");
    mirror_fixture_state_t s = mirror_all_safe();
    s.dut_power_main_on = true;
    TEST_CHECK(!mirror_safe_state_confirmed(&s), "main relay still commanded on refuses confirmation");
}

static void test_safe_state_confirmed_dut_power_safety_still_on(void)
{
    TEST_SECTION("safe_state_confirmed -- DUT power (safety) still on refuses");
    mirror_fixture_state_t s = mirror_all_safe();
    s.dut_power_safety_on = true;
    TEST_CHECK(!mirror_safe_state_confirmed(&s), "safety relay still commanded on refuses confirmation");
}

static void test_safe_state_confirmed_ct_not_yet_valid(void)
{
    TEST_SECTION("safe_state_confirmed -- a CT channel with no confirmed reading yet refuses");
    mirror_fixture_state_t s = mirror_all_safe();
    s.ct_valid[1] = false;
    TEST_CHECK(!mirror_safe_state_confirmed(&s), "an unconfirmed CT channel (wave_owner never ticked) refuses");
}

static void test_safe_state_confirmed_ct_still_model_mode(void)
{
    TEST_SECTION("safe_state_confirmed -- a CT channel still in MODEL mode refuses");
    mirror_fixture_state_t s = mirror_all_safe();
    s.ct_manual[2] = false;
    TEST_CHECK(!mirror_safe_state_confirmed(&s), "a channel still tracking MODEL (not yet MANUAL) refuses");
}

static void test_safe_state_confirmed_ct_nonzero_amps(void)
{
    TEST_SECTION("safe_state_confirmed -- a CT channel with nonzero amps refuses");
    mirror_fixture_state_t s = mirror_all_safe();
    s.ct_amps[0] = 0.5f;
    TEST_CHECK(!mirror_safe_state_confirmed(&s), "MANUAL mode with a nonzero commanded amps is not silent");
}

// --- bounded poll-then-refuse loop ------------------------------------------
// Mirrors safe_reboot_into_bootloader()'s poll loop shape: SAFE_REBOOT_POLL_MS
// == 10, SAFE_REBOOT_TIMEOUT_MS == 300 (safe_reboot.c) -> at most 30 polls
// before the loop body's own bound is exhausted, plus the one extra
// post-loop check safe_reboot.c makes to avoid discarding a confirmation
// that lands exactly at the boundary -- 31 checks total, never more.
#define MIRROR_POLL_MS    10u
#define MIRROR_TIMEOUT_MS 300u

// Simulates the loop's structure: calls `confirmed_at_check(check_index)` up
// to the bound, and returns true (and how many checks it took) the moment
// one confirms, or false if the whole bound is exhausted without one.
static bool mirror_poll_then_reboot(bool (*confirmed_at_check)(uint32_t), uint32_t *out_checks_used)
{
    uint32_t checks = 0;
    for (uint32_t waited_ms = 0; waited_ms < MIRROR_TIMEOUT_MS; waited_ms += MIRROR_POLL_MS) {
        checks++;
        if (confirmed_at_check(checks - 1)) {
            *out_checks_used = checks;
            return true;
        }
    }
    // one last check past the loop, mirroring safe_reboot.c's post-loop check
    checks++;
    if (confirmed_at_check(checks - 1)) {
        *out_checks_used = checks;
        return true;
    }
    *out_checks_used = checks;
    return false;
}

static bool s_confirm_never(uint32_t check_index)
{
    (void)check_index;
    return false;
}

static bool s_confirm_on_third_check(uint32_t check_index)
{
    return check_index == 2u; // confirms on the 3rd call (index 2)
}

static bool s_confirm_on_last_possible_check(uint32_t check_index)
{
    // MIRROR_TIMEOUT_MS/MIRROR_POLL_MS == 30 loop iterations (indices 0..29)
    // plus the one post-loop check (index 30) -- 31 checks total. Confirming
    // only on the very last one proves the "one extra check past the loop"
    // boundary is actually exercised, not just present in the mirror.
    return check_index == 30u;
}

static void test_poll_loop_confirms_promptly(void)
{
    TEST_SECTION("poll-then-reboot -- confirms well before the timeout");
    uint32_t checks_used = 0;
    bool rebooted = mirror_poll_then_reboot(s_confirm_on_third_check, &checks_used);
    TEST_CHECK(rebooted, "a fixture that confirms safe within a few ticks reboots");
    TEST_CHECK(checks_used == 3u, "stops polling the moment confirmation lands (3rd check), not the full bound");
}

static void test_poll_loop_confirms_at_the_very_last_check(void)
{
    TEST_SECTION("poll-then-reboot -- confirms exactly at the timeout boundary");
    uint32_t checks_used = 0;
    bool rebooted = mirror_poll_then_reboot(s_confirm_on_last_possible_check, &checks_used);
    TEST_CHECK(rebooted, "a confirmation landing exactly at the boundary is not discarded");
    TEST_CHECK(checks_used == 31u, "the post-loop check (31st) is what catches a boundary-exact confirmation");
}

static void test_poll_loop_refuses_after_bound_exhausted(void)
{
    TEST_SECTION("poll-then-reboot -- never confirms => refuses within the bound");
    uint32_t checks_used = 0;
    bool rebooted = mirror_poll_then_reboot(s_confirm_never, &checks_used);
    TEST_CHECK(!rebooted, "a fixture that never confirms safe is refused, not rebooted anyway");
    TEST_CHECK(checks_used == 31u, "exactly the bounded number of checks is made -- the loop is not unbounded");
}

void run_test_safe_reboot_logic(void)
{
    test_reboot_bootloader_magic_gate();
    test_bootloader_touch_gate_positive();
    test_bootloader_touch_gate_negative_baud_only();
    test_bootloader_touch_gate_negative_dtr_only();
    test_bootloader_touch_gate_both_conditions_false();
    test_safe_state_confirmed_all_conditions_met();
    test_safe_state_confirmed_estop_still_closed();
    test_safe_state_confirmed_dut_power_main_still_on();
    test_safe_state_confirmed_dut_power_safety_still_on();
    test_safe_state_confirmed_ct_not_yet_valid();
    test_safe_state_confirmed_ct_still_model_mode();
    test_safe_state_confirmed_ct_nonzero_amps();
    test_poll_loop_confirms_promptly();
    test_poll_loop_confirms_at_the_very_last_check();
    test_poll_loop_refuses_after_bound_exhausted();
}
