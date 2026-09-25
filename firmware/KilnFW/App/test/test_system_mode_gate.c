// test_system_mode_gate.c -- host tests for App/drivers/safety/system_mode_gate.c.
// docs/SYSTEM_MODE_GATE_PLAN.md is the design doc; section 5 records the
// owner's 2026-09-25 decisions this table encodes. No ESP-IDF dependency --
// system_mode_gate.c is pure (snapshot in, verdict out, no I/O, no locks).
#include <stdio.h>
#include <string.h>

#include "../drivers/safety/system_mode_gate.h"

static int g_checks = 0;
static int g_failures = 0;

#define TEST_SECTION(msg) printf("-- %s --\n", (msg))
#define TEST_CHECK(cond, msg) do { \
        g_checks++; \
        if (!(cond)) { \
            g_failures++; \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
        } \
    } while (0)

// A snapshot that passes every wired rule -- each test below mutates one
// field away from this baseline so a failure is attributable to exactly one
// precondition.
static sys_mode_snapshot_t good_snapshot(void)
{
    sys_mode_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.profile_running = false;
    s.autotune_running = false;
    s.ota_holds_interlock = false;
    s.recovery_mode = false;
    s.safety_tripped = false;
    s.readiness_gate_ready = true;
    return s;
}

static void test_null_snapshot_refuses(void)
{
    TEST_SECTION("system_mode_gate_check -- NULL snapshot refuses, never a silent pass");

    char reason[SYSTEM_MODE_GATE_REASON_MAX] = { 0 };
    TEST_CHECK(system_mode_gate_check(SYS_ACTION_RAW_RELAY_DEBUG_WRITE, NULL, reason, sizeof(reason)) == true,
               "NULL snapshot is refused, not treated as a pass");
    TEST_CHECK(reason[0] != '\0', "a NULL-snapshot refusal still writes a reason");

    // Every other action must refuse on NULL too -- the NULL check runs
    // before the per-action switch, so this isn't specific to one action.
    TEST_CHECK(system_mode_gate_check(SYS_ACTION_START_PROFILE, NULL, NULL, 0) == true,
               "NULL snapshot refuses for an unwired action too");
    TEST_CHECK(system_mode_gate_check(SYS_ACTION_FACTORY_RESET, NULL, NULL, 0) == true,
               "NULL snapshot refuses for FACTORY_RESET too");
}

static void test_relay_write_allowed_when_idle(void)
{
    TEST_SECTION("SYS_ACTION_RAW_RELAY_DEBUG_WRITE -- allowed when nothing is running");

    sys_mode_snapshot_t snap = good_snapshot();
    char reason[SYSTEM_MODE_GATE_REASON_MAX] = { 0 };
    TEST_CHECK(system_mode_gate_check(SYS_ACTION_RAW_RELAY_DEBUG_WRITE, &snap, reason, sizeof(reason)) == false,
               "idle profile and idle autotune: manual relay write allowed");
    TEST_CHECK(reason[0] == '\0', "reason is left untouched when allowed");

    // NULL reason_out/reason_cap must not crash and must not change the result.
    TEST_CHECK(system_mode_gate_check(SYS_ACTION_RAW_RELAY_DEBUG_WRITE, &snap, NULL, 0) == false,
               "NULL reason_out is accepted, result unaffected");
}

static void test_relay_write_refused_cross_product(void)
{
    TEST_SECTION("SYS_ACTION_RAW_RELAY_DEBUG_WRITE -- BLANKET refusal cross product (owner Q1)");

    // Owner decision 2026-09-25 Q1: BLANKET-refuse (not claimed-relays-only)
    // any manual relay write while EITHER a firing or an autotune session is
    // active. Exercise all four combinations of the two flags explicitly so
    // this cross product is pinned, not just the two single-flag cases.
    static const struct {
        bool profile_running;
        bool autotune_running;
        bool expect_refused;
        const char *label;
    } cases[] = {
        { false, false, false, "both idle: allowed" },
        { true,  false, true,  "profile running only: refused" },
        { false, true,  true,  "autotune running only: refused" },
        { true,  true,  true,  "both running: refused" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sys_mode_snapshot_t snap = good_snapshot();
        snap.profile_running = cases[i].profile_running;
        snap.autotune_running = cases[i].autotune_running;
        char reason[SYSTEM_MODE_GATE_REASON_MAX] = { 0 };
        bool refused = system_mode_gate_check(SYS_ACTION_RAW_RELAY_DEBUG_WRITE, &snap, reason, sizeof(reason));
        TEST_CHECK(refused == cases[i].expect_refused, cases[i].label);
        if (cases[i].expect_refused) {
            TEST_CHECK(reason[0] != '\0', "a refusal always writes a reason");
            TEST_CHECK(strstr(reason, "firing or autotune") != NULL,
                       "the reason names a firing/autotune run, not a generic refusal");
        } else {
            TEST_CHECK(reason[0] == '\0', "an allowed case leaves reason untouched");
        }
    }
}

static void test_relay_write_ignores_reserved_fields(void)
{
    TEST_SECTION("SYS_ACTION_RAW_RELAY_DEBUG_WRITE -- reserved snapshot fields are not consulted yet");

    // ota_holds_interlock/recovery_mode/safety_tripped/readiness_gate_ready
    // are documented as reserved and unused by this pass's rules
    // (system_mode_gate.h's field comments) -- kiln_io_owner.c already
    // checks its own OTA interlock separately and first, so this gate must
    // not double-refuse (or fail to refuse) off a field it doesn't own yet.
    sys_mode_snapshot_t snap = good_snapshot();
    snap.ota_holds_interlock = true;
    snap.recovery_mode = true;
    snap.safety_tripped = true;
    snap.readiness_gate_ready = false;
    TEST_CHECK(system_mode_gate_check(SYS_ACTION_RAW_RELAY_DEBUG_WRITE, &snap, NULL, 0) == false,
               "reserved fields alone never trigger a refusal from this action's rule");
}

static void test_unwired_actions_are_ok_for_now(void)
{
    TEST_SECTION("Unwired actions -- OK unconditionally, correct today per this file's top comment");

    // Q2/Q3 decisions exist in the plan doc but are NOT wired into the gate
    // table this pass -- no caller invokes this gate for these actions yet
    // (zones/config keeps its own future gate; factory reset/cfgfs likewise).
    // A worst-case snapshot (everything running/tripped) must still return
    // "not refused" for these, since returning true here would be reporting
    // a rule that does not exist.
    sys_mode_snapshot_t snap = good_snapshot();
    snap.profile_running = true;
    snap.autotune_running = true;
    snap.ota_holds_interlock = true;
    snap.recovery_mode = true;
    snap.safety_tripped = true;
    snap.readiness_gate_ready = false;

    TEST_CHECK(system_mode_gate_check(SYS_ACTION_START_PROFILE, &snap, NULL, 0) == false,
               "SYS_ACTION_START_PROFILE not wired -- always OK from this gate (readiness_gate.h owns it)");
    TEST_CHECK(system_mode_gate_check(SYS_ACTION_START_AUTOTUNE, &snap, NULL, 0) == false,
               "SYS_ACTION_START_AUTOTUNE not wired -- always OK from this gate");
    TEST_CHECK(system_mode_gate_check(SYS_ACTION_WRITE_ZONES_CONFIG, &snap, NULL, 0) == false,
               "SYS_ACTION_WRITE_ZONES_CONFIG not wired this pass -- plan section 3.6 item 4, deferred");
    TEST_CHECK(system_mode_gate_check(SYS_ACTION_FACTORY_RESET, &snap, NULL, 0) == false,
               "SYS_ACTION_FACTORY_RESET not wired this pass -- plan section 3.6 item 5, deferred");
    TEST_CHECK(system_mode_gate_check(SYS_ACTION_CFGFS_FORMAT, &snap, NULL, 0) == false,
               "SYS_ACTION_CFGFS_FORMAT not wired this pass -- plan section 3.6 item 5, deferred");
    TEST_CHECK(system_mode_gate_check(SYS_ACTION_OTA_START, &snap, NULL, 0) == false,
               "SYS_ACTION_OTA_START not gated here at all -- ota_interlock.c stays the owner");
}

static void test_reason_truncation_is_safe(void)
{
    TEST_SECTION("system_mode_gate_check -- a too-small reason buffer truncates, never overflows");

    sys_mode_snapshot_t snap = good_snapshot();
    snap.profile_running = true;
    char tiny[8];
    memset(tiny, 'X', sizeof(tiny));
    bool refused = system_mode_gate_check(SYS_ACTION_RAW_RELAY_DEBUG_WRITE, &snap, tiny, sizeof(tiny));
    TEST_CHECK(refused == true, "still refused even with a tiny reason buffer");
    TEST_CHECK(strlen(tiny) < sizeof(tiny), "reason is NUL-terminated within the small buffer");
}

int main(void)
{
    test_null_snapshot_refuses();
    test_relay_write_allowed_when_idle();
    test_relay_write_refused_cross_product();
    test_relay_write_ignores_reserved_fields();
    test_unwired_actions_are_ok_for_now();
    test_reason_truncation_is_safe();

    printf("\n%d/%d checks passed\n", g_checks - g_failures, g_checks);
    if (g_failures > 0) {
        printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
