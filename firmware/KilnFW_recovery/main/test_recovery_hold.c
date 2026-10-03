// test_recovery_hold.c -- host test for recovery_hold.c, the decision logic of
// the relay-hold watchdog in recovery_io.c: what a periodic SX1509 read-back
// means, when the fault latches, and how re-assert retries are counted.
// Built and run by check_recovery_hold.ps1 (MSVC). Prints "RESULT pass=N fail=N".
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>

#include "recovery_hold.h"

static int g_pass, g_fail;

#define CHECK(cond, name)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            g_pass++;                                                       \
        } else {                                                            \
            g_fail++;                                                       \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);        \
        }                                                                   \
    } while (0)

// Representative masks: relay outputs on pins 0..3, one extra output on pin 8.
#define OUT_MASK 0x010Fu
#define HOLD_MASK 0x010Fu

static void test_init(void)
{
    rhold_state_t s;
    memset(&s, 0xFF, sizeof(s));
    rhold_init(&s);
    CHECK(!s.fault && !s.fault_seen_s_valid && s.fault_s == 0, "init: no fault");
    CHECK(!s.ever_ok && s.last_ok_s == 0, "init: never observed ok");
    CHECK(s.mismatch_count == 0 && s.reassert_fail_count == 0, "init: counters zero");
}

static void test_regs_match(void)
{
    CHECK(rhold_regs_match(0x0000, 0x0000, OUT_MASK, HOLD_MASK), "outputs + low: match");
    CHECK(!rhold_regs_match(0x0001, 0x0000, OUT_MASK, HOLD_MASK), "a relay pin back to input: mismatch");
    CHECK(!rhold_regs_match(0x0100, 0x0000, OUT_MASK, HOLD_MASK), "the extra output pin back to input: mismatch");
    CHECK(!rhold_regs_match(0x0000, 0x0004, OUT_MASK, HOLD_MASK), "a held pin reading high: mismatch");
    CHECK(rhold_regs_match(0x0010, 0x0000, OUT_MASK, HOLD_MASK), "an unrelated pin left as input: still a match");
    CHECK(rhold_regs_match(0x0000, 0x0020, OUT_MASK, HOLD_MASK), "an unrelated pin high: still a match");
    CHECK(rhold_regs_match(0xFEF0, 0xFEF0, OUT_MASK, HOLD_MASK), "only the masked bits matter");
    CHECK(!rhold_regs_match(0x0000, 0x0100, OUT_MASK, HOLD_MASK), "extra held pin high: mismatch");
}

static void test_observe(void)
{
    rhold_state_t s;
    rhold_init(&s);
    CHECK(rhold_observe(&s, true, 0, 0, OUT_MASK, HOLD_MASK, 10) == RHOLD_HEALTHY, "good read: healthy");
    CHECK(s.ever_ok && s.last_ok_s == 10 && !s.fault && s.mismatch_count == 0, "good read: recorded");

    // An unreadable expander cannot be shown to be holding, even if the stale
    // buffers happen to look right.
    CHECK(rhold_observe(&s, false, 0, 0, OUT_MASK, HOLD_MASK, 20) == RHOLD_REASSERT, "I2C error: re-assert");
    CHECK(s.fault && s.fault_seen_s_valid && s.fault_s == 20 && s.mismatch_count == 1,
          "I2C error latches the fault with its time");
    CHECK(s.last_ok_s == 10, "a failed read does not refresh last_ok");

    // Later mismatches keep the FIRST fault time but still count.
    CHECK(rhold_observe(&s, true, 0x0001, 0, OUT_MASK, HOLD_MASK, 30) == RHOLD_REASSERT, "register mismatch: re-assert");
    CHECK(s.fault_s == 20 && s.mismatch_count == 2, "fault time is the first mismatch, count keeps rising");

    // A healthy observation afterwards never clears the latch.
    CHECK(rhold_observe(&s, true, 0, 0, OUT_MASK, HOLD_MASK, 40) == RHOLD_HEALTHY, "recovered: healthy again");
    CHECK(s.fault && s.last_ok_s == 40, "fault stays latched after recovery; last_ok advances");

    // A fault at uptime 0 is still a fault with a valid time.
    rhold_state_t z;
    rhold_init(&z);
    rhold_observe(&z, true, 0x0002, 0, OUT_MASK, HOLD_MASK, 0);
    CHECK(z.fault && z.fault_seen_s_valid && z.fault_s == 0, "fault at t=0 is flagged valid, not inferred from the time");
}

static void test_reassert(void)
{
    rhold_state_t s;
    rhold_init(&s);
    rhold_observe(&s, true, 0x0008, 0, OUT_MASK, HOLD_MASK, 5);
    rhold_reassert_result(&s, false, 5);
    CHECK(s.reassert_fail_count == 1 && s.fault && !s.ever_ok, "failed re-assert counted, fault kept, not ok");
    rhold_observe(&s, true, 0x0008, 0, OUT_MASK, HOLD_MASK, 6);
    rhold_reassert_result(&s, false, 6);
    rhold_observe(&s, true, 0x0008, 0, OUT_MASK, HOLD_MASK, 7);
    rhold_reassert_result(&s, false, 7);
    CHECK(s.reassert_fail_count == 3 && s.mismatch_count == 3, "three failed retries counted");
    rhold_observe(&s, true, 0x0008, 0, OUT_MASK, HOLD_MASK, 8);
    rhold_reassert_result(&s, true, 8);
    CHECK(s.reassert_fail_count == 3, "a verified re-assert is not a failure");
    CHECK(s.ever_ok && s.last_ok_s == 8, "a verified re-assert records last_ok");
    CHECK(s.fault && s.fault_s == 5, "a successful repair does not clear the latched fault");
    CHECK(s.mismatch_count == 4, "mismatch count is untouched by re-assert results");
}

int main(void)
{
    test_init();
    test_regs_match();
    test_observe();
    test_reassert();
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
