// Host tests for tc_fault_state.c: the fault_sched-writes/spi_emu-reads
// contract (tc_fault_state.h's ownership doc) -- write/read/clear
// round-trip, out-of-range rejection, per-channel independence, and the
// lock-free seq-counter torn-read-retry protocol itself (PLAN.md section
// 13.1 flags this module as needing its own dedicated coverage, having none
// before this pass). No FreeRTOS/pico-sdk dependency: the module is plain C
// with a portable compiler barrier (see tc_fault_state.c's
// TC_FAULT_STATE_BARRIER() comment for the MSVC-vs-GCC story), so it runs
// in this host-test harness unmodified, same as every other src/sim/ module.
#include <string.h>

#include "test_common.h"
#include "../src/sim/tc_fault_state.h"

static void test_write_read_round_trip(void)
{
    TEST_SECTION("tc_fault_state -- write/read round trip");

    tc_fault_override_t ovr;
    memset(&ovr, 0, sizeof(ovr));
    ovr.corruption.noise_sigma_c = 3.5f;
    ovr.corruption.stuck_ltcb = true;
    ovr.corruption.shorted = true;
    ovr.corruption.drift_offset_c = 12.0f;
    ovr.corruption.cj_fault_offset_c = -8.0f;
    ovr.force_sr_bits = MAX31856_FAULT_OPEN;

    TEST_CHECK(tc_fault_state_write(TC_FAULT_CHANNEL_MAIN_0, &ovr), "write to a valid channel succeeds");

    tc_fault_override_t out;
    memset(&out, 0xAA, sizeof(out)); /* poison so a no-op read would be caught */
    TEST_CHECK(tc_fault_state_read(TC_FAULT_CHANNEL_MAIN_0, &out), "read from a valid channel succeeds");

    TEST_CHECK(out.corruption.noise_sigma_c == ovr.corruption.noise_sigma_c, "round-trip: noise_sigma_c preserved");
    TEST_CHECK(out.corruption.stuck_ltcb == ovr.corruption.stuck_ltcb, "round-trip: stuck_ltcb preserved");
    TEST_CHECK(out.corruption.shorted == ovr.corruption.shorted, "round-trip: shorted preserved");
    TEST_CHECK(out.corruption.drift_offset_c == ovr.corruption.drift_offset_c, "round-trip: drift_offset_c preserved");
    TEST_CHECK(out.corruption.cj_fault_offset_c == ovr.corruption.cj_fault_offset_c, "round-trip: cj_fault_offset_c preserved");
    TEST_CHECK(out.force_sr_bits == ovr.force_sr_bits, "round-trip: force_sr_bits preserved");
}

static void test_clear_resets_to_no_fault(void)
{
    TEST_SECTION("tc_fault_state -- clear resets to all-zero/no-fault");

    tc_fault_override_t ovr;
    memset(&ovr, 0, sizeof(ovr));
    ovr.corruption.spurious_fault_pin = true;
    ovr.force_sr_bits = MAX31856_FAULT_OVUV;
    TEST_CHECK(tc_fault_state_write(TC_FAULT_CHANNEL_MAIN_1, &ovr), "write before clear succeeds");

    TEST_CHECK(tc_fault_state_clear(TC_FAULT_CHANNEL_MAIN_1), "clear succeeds on a valid channel");

    tc_fault_override_t out;
    memset(&out, 0xAA, sizeof(out));
    TEST_CHECK(tc_fault_state_read(TC_FAULT_CHANNEL_MAIN_1, &out), "read after clear succeeds");

    tc_fault_override_t zero;
    memset(&zero, 0, sizeof(zero));
    TEST_CHECK(memcmp(&out, &zero, sizeof(out)) == 0, "clear leaves an all-zero override (no fault)");
}

static void test_never_written_channel_reads_as_no_fault(void)
{
    TEST_SECTION("tc_fault_state -- a channel never written reads back as all-zero/no-fault");

    /* TC_FAULT_CHANNEL_SAFETY is not touched by any earlier test in this
     * file (test order matters here, deliberately -- this is what
     * tc_fault_state.h's own doc comment calls out: "statics zero-init to a
     * seq of 0 ... a valid 'nothing published yet' state, so callers do not
     * need a separate has-fault_sched-run-yet check"). */
    tc_fault_override_t out;
    memset(&out, 0xAA, sizeof(out));
    TEST_CHECK(tc_fault_state_read(TC_FAULT_CHANNEL_SAFETY, &out), "read from a never-written channel still succeeds");

    tc_fault_override_t zero;
    memset(&zero, 0, sizeof(zero));
    TEST_CHECK(memcmp(&out, &zero, sizeof(out)) == 0, "a never-written channel reads back all-zero/no-fault");
}

static void test_channels_are_independent(void)
{
    TEST_SECTION("tc_fault_state -- per-channel storage is independent");

    tc_fault_override_t a;
    memset(&a, 0, sizeof(a));
    a.corruption.noise_sigma_c = 1.0f;
    tc_fault_override_t b;
    memset(&b, 0, sizeof(b));
    b.corruption.noise_sigma_c = 2.0f;

    TEST_CHECK(tc_fault_state_write(TC_FAULT_CHANNEL_MAIN_2, &a), "write channel MAIN_2");
    TEST_CHECK(tc_fault_state_write(TC_FAULT_CHANNEL_SAFETY, &b), "write channel SAFETY");

    tc_fault_override_t out_a, out_b;
    TEST_CHECK(tc_fault_state_read(TC_FAULT_CHANNEL_MAIN_2, &out_a), "read channel MAIN_2 back");
    TEST_CHECK(tc_fault_state_read(TC_FAULT_CHANNEL_SAFETY, &out_b), "read channel SAFETY back");

    TEST_CHECK(out_a.corruption.noise_sigma_c == 1.0f, "MAIN_2's write did not leak into SAFETY's storage");
    TEST_CHECK(out_b.corruption.noise_sigma_c == 2.0f, "SAFETY's write did not leak into MAIN_2's storage");

    /* Clearing one channel must not disturb the other. */
    TEST_CHECK(tc_fault_state_clear(TC_FAULT_CHANNEL_MAIN_2), "clear MAIN_2");
    tc_fault_override_t out_b2;
    TEST_CHECK(tc_fault_state_read(TC_FAULT_CHANNEL_SAFETY, &out_b2), "read channel SAFETY after clearing MAIN_2");
    TEST_CHECK(out_b2.corruption.noise_sigma_c == 2.0f, "clearing MAIN_2 leaves SAFETY's override untouched");
}

static void test_out_of_range_channel_rejected(void)
{
    TEST_SECTION("tc_fault_state -- out-of-range channel / NULL pointer rejected");

    tc_fault_override_t ovr;
    memset(&ovr, 0, sizeof(ovr));
    tc_fault_channel_t bad = (tc_fault_channel_t)TC_FAULT_CHANNEL_COUNT; /* one past the end */

    TEST_CHECK(!tc_fault_state_write(bad, &ovr), "write to an out-of-range channel is rejected");
    TEST_CHECK(!tc_fault_state_clear(bad), "clear on an out-of-range channel is rejected");
    TEST_CHECK(!tc_fault_state_read(bad, &ovr), "read from an out-of-range channel is rejected");

    TEST_CHECK(!tc_fault_state_write(TC_FAULT_CHANNEL_MAIN_0, NULL), "write with a NULL override pointer is rejected");
    TEST_CHECK(!tc_fault_state_read(TC_FAULT_CHANNEL_MAIN_0, NULL), "read with a NULL output pointer is rejected");
}

static void test_repeated_write_read_stays_consistent(void)
{
    TEST_SECTION("tc_fault_state -- repeated write/read cycles stay internally consistent (seq-counter sanity)");

    /* Not a real concurrency test (this harness is single-threaded), but
     * exercises the seq-counter write/read path many times over with
     * different payloads each cycle -- a torn read or an off-by-one in the
     * seq arithmetic would show up as a mismatch somewhere in this loop,
     * same spirit as sim_snapshot.h's own single-threaded host coverage. */
    bool all_ok = true;
    for (int i = 0; i < 1000; i++) {
        tc_fault_override_t ovr;
        memset(&ovr, 0, sizeof(ovr));
        ovr.corruption.noise_sigma_c = (float)i * 0.01f;
        ovr.corruption.bit_error_rate = (float)(i % 100) * 0.001f;
        ovr.force_sr_bits = (uint8_t)(i & (MAX31856_FAULT_OPEN | MAX31856_FAULT_OVUV));

        if (!tc_fault_state_write(TC_FAULT_CHANNEL_MAIN_0, &ovr)) {
            all_ok = false;
            break;
        }
        tc_fault_override_t out;
        if (!tc_fault_state_read(TC_FAULT_CHANNEL_MAIN_0, &out)) {
            all_ok = false;
            break;
        }
        if (out.corruption.noise_sigma_c != ovr.corruption.noise_sigma_c ||
            out.corruption.bit_error_rate != ovr.corruption.bit_error_rate ||
            out.force_sr_bits != ovr.force_sr_bits) {
            all_ok = false;
            break;
        }
    }
    TEST_CHECK(all_ok, "1000 write/read cycles each return exactly what was just written");
}

void run_test_tc_fault_state(void)
{
    test_write_read_round_trip();
    test_clear_resets_to_no_fault();
    test_never_written_channel_reads_as_no_fault();
    test_channels_are_independent();
    test_out_of_range_channel_rejected();
    test_repeated_write_read_stays_consistent();
}
