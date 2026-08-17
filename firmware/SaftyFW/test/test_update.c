// Host tests for firmware/SaftyFW/src/update/{image_header,received_ranges,
// update_receiver,confirm}.c -- TODO.md Phase 10 items 10.8/10.8b/10.8c/10.9's
// pure decision logic. No pico-sdk/FreeRTOS dependency.
#include <string.h>

#include "test_common.h"

#include "image_header.h"
#include "received_ranges.h"
#include "update_receiver.h"
#include "confirm.h"

// --- image_header ---------------------------------------------------------

static update_image_header_t make_valid_header(void)
{
    update_image_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = UPDATE_IMAGE_HEADER_MAGIC;
    h.target = UPDATE_IMAGE_TARGET_RP2040;
    h.header_version = UPDATE_IMAGE_HEADER_VERSION;
    h.protocol_version = 5;
    h.min_compatible = 3;
    h.requested_slot = 1;
    h.flags = 0;
    h.length = 200000;
    h.crc32 = 0xABCD1234u;
    memcpy(h.version, "1.4.0-rc1", 9);
    return h;
}

static void test_image_header_roundtrip(void)
{
    TEST_SECTION("update_image_header_pack/unpack -- roundtrip");

    update_image_header_t h = make_valid_header();
    uint8_t wire[UPDATE_IMAGE_HEADER_WIRE_LEN];
    update_image_header_pack(&h, wire);

    update_image_header_t back;
    memset(&back, 0xAA, sizeof(back));
    bool ok = update_image_header_unpack(wire, sizeof(wire), &back);
    TEST_CHECK(ok, "well-formed header unpacks");
    TEST_CHECK(back.magic == UPDATE_IMAGE_HEADER_MAGIC, "magic roundtrips");
    TEST_CHECK(back.target == UPDATE_IMAGE_TARGET_RP2040, "target roundtrips");
    TEST_CHECK(back.protocol_version == 5, "protocol_version roundtrips");
    TEST_CHECK(back.min_compatible == 3, "min_compatible roundtrips");
    TEST_CHECK(back.requested_slot == 1, "requested_slot roundtrips");
    TEST_CHECK(back.length == 200000u, "length roundtrips");
    TEST_CHECK(back.crc32 == 0xABCD1234u, "crc32 roundtrips");
    TEST_CHECK(memcmp(back.version, "1.4.0-rc1", 9) == 0, "version roundtrips");
}

static void test_image_header_unpack_hostile(void)
{
    TEST_SECTION("update_image_header_unpack -- hostile inputs");

    update_image_header_t h = make_valid_header();
    uint8_t wire[UPDATE_IMAGE_HEADER_WIRE_LEN];
    update_image_header_pack(&h, wire);

    update_image_header_t sentinel;
    memset(&sentinel, 0xAA, sizeof(sentinel));
    update_image_header_t out = sentinel;

    TEST_CHECK(!update_image_header_unpack(wire, UPDATE_IMAGE_HEADER_WIRE_LEN - 1, &out),
               "one byte short rejected");
    TEST_CHECK(memcmp(&out, &sentinel, sizeof(out)) == 0, "*out untouched on short-length reject");

    out = sentinel;
    TEST_CHECK(!update_image_header_unpack(wire, UPDATE_IMAGE_HEADER_WIRE_LEN + 1, &out),
               "one byte long rejected");

    out = sentinel;
    TEST_CHECK(!update_image_header_unpack(NULL, sizeof(wire), &out), "NULL payload rejected");
    TEST_CHECK(!update_image_header_unpack(wire, sizeof(wire), NULL), "NULL out rejected");
}

static void test_image_header_validate(void)
{
    TEST_SECTION("update_image_header_validate");

    update_image_header_t h = make_valid_header();
    TEST_CHECK(update_image_header_validate(&h, 832u * 1024u) == UPDATE_IMAGE_HEADER_OK,
               "well-formed header validates OK");

    update_image_header_t bad_magic = h;
    bad_magic.magic = 0x12345678u;
    TEST_CHECK(update_image_header_validate(&bad_magic, 832u * 1024u) == UPDATE_IMAGE_HEADER_BAD_MAGIC,
               "wrong magic rejected");

    update_image_header_t bad_target = h;
    bad_target.target = 99; // some hypothetical ESP/other-target value
    TEST_CHECK(update_image_header_validate(&bad_target, 832u * 1024u) == UPDATE_IMAGE_HEADER_BAD_TARGET,
               "wrong target rejected -- 'refuses an ESP image outright'");

    update_image_header_t bad_version = h;
    bad_version.header_version = 2;
    TEST_CHECK(update_image_header_validate(&bad_version, 832u * 1024u) ==
                   UPDATE_IMAGE_HEADER_BAD_VERSION,
               "unrecognised header_version rejected");

    update_image_header_t zero_len = h;
    zero_len.length = 0;
    TEST_CHECK(update_image_header_validate(&zero_len, 832u * 1024u) ==
                   UPDATE_IMAGE_HEADER_ZERO_LENGTH,
               "zero length rejected");

    update_image_header_t too_big = h;
    too_big.length = 900u * 1024u;
    TEST_CHECK(update_image_header_validate(&too_big, 832u * 1024u) == UPDATE_IMAGE_HEADER_TOO_LARGE,
               "length exceeding slot capacity rejected");

    update_image_header_t exact = h;
    exact.length = 832u * 1024u;
    TEST_CHECK(update_image_header_validate(&exact, 832u * 1024u) == UPDATE_IMAGE_HEADER_OK,
               "length exactly at the slot capacity boundary is OK (inclusive)");
}

// --- received_ranges -------------------------------------------------------

static void test_received_ranges_basic(void)
{
    TEST_SECTION("update_received_ranges -- mark/complete/count");

    update_received_ranges_t r;
    update_received_ranges_reset(&r, 5 * UPDATE_CHUNK_LEN); // exactly 5 chunks
    TEST_CHECK(r.total_chunks == 5, "5 whole chunks -> total_chunks == 5");
    TEST_CHECK(!update_received_ranges_is_complete(&r), "freshly reset -- not complete");
    TEST_CHECK(update_received_ranges_count(&r) == 0, "freshly reset -- 0 received");

    TEST_CHECK(update_received_ranges_mark(&r, 0 * UPDATE_CHUNK_LEN), "mark chunk 0 accepted");
    TEST_CHECK(update_received_ranges_mark(&r, 2 * UPDATE_CHUNK_LEN), "mark chunk 2 accepted");
    TEST_CHECK(update_received_ranges_count(&r) == 2, "2 chunks marked -> count == 2");
    TEST_CHECK(!update_received_ranges_is_complete(&r), "still incomplete with gaps");

    // Duplicate mark (retransmission) -- idempotent, still accepted.
    TEST_CHECK(update_received_ranges_mark(&r, 0 * UPDATE_CHUNK_LEN),
               "re-marking an already-received chunk is accepted (idempotent)");
    TEST_CHECK(update_received_ranges_count(&r) == 2, "duplicate mark does not double-count");

    update_received_ranges_mark(&r, 1 * UPDATE_CHUNK_LEN);
    update_received_ranges_mark(&r, 3 * UPDATE_CHUNK_LEN);
    update_received_ranges_mark(&r, 4 * UPDATE_CHUNK_LEN);
    TEST_CHECK(update_received_ranges_is_complete(&r), "all 5 chunks marked -> complete");
}

static void test_received_ranges_hostile_offsets(void)
{
    TEST_SECTION("update_received_ranges_mark -- hostile offsets");

    update_received_ranges_t r;
    update_received_ranges_reset(&r, 3 * UPDATE_CHUNK_LEN);

    TEST_CHECK(!update_received_ranges_mark(&r, 1), "unaligned offset (not a multiple of chunk len) rejected");
    TEST_CHECK(!update_received_ranges_mark(&r, UPDATE_CHUNK_LEN + 3),
               "unaligned offset mid-range rejected");
    TEST_CHECK(!update_received_ranges_mark(&r, 3 * UPDATE_CHUNK_LEN),
               "chunk index == total_chunks (one past the end) rejected");
    TEST_CHECK(!update_received_ranges_mark(&r, 1000 * UPDATE_CHUNK_LEN),
               "wildly out-of-range offset rejected");
    TEST_CHECK(update_received_ranges_count(&r) == 0, "no rejected offset left any bit set");
}

static void test_received_ranges_find_gaps(void)
{
    TEST_SECTION("update_received_ranges_find_gaps -- cursor / wraparound");

    update_received_ranges_t r;
    update_received_ranges_reset(&r, 6 * UPDATE_CHUNK_LEN);
    // Mark everything except chunks 1, 3, 5.
    update_received_ranges_mark(&r, 0 * UPDATE_CHUNK_LEN);
    update_received_ranges_mark(&r, 2 * UPDATE_CHUNK_LEN);
    update_received_ranges_mark(&r, 4 * UPDATE_CHUNK_LEN);

    uint32_t gaps[8];
    size_t n = update_received_ranges_find_gaps(&r, 0, gaps, 8);
    TEST_CHECK(n == 3, "exactly 3 gaps found (chunks 1, 3, 5)");
    TEST_CHECK(gaps[0] == 1 && gaps[1] == 3 && gaps[2] == 5, "gaps reported in scan order starting at 0");

    // Limited output buffer -- only the first N.
    n = update_received_ranges_find_gaps(&r, 0, gaps, 2);
    TEST_CHECK(n == 2, "max_out caps the number of gaps returned");
    TEST_CHECK(gaps[0] == 1 && gaps[1] == 3, "first two gaps in scan order");

    // Starting cursor mid-way, with wraparound back past 0.
    n = update_received_ranges_find_gaps(&r, 4, gaps, 8);
    TEST_CHECK(n == 3, "starting at index 4: finds all 3 gaps (5, then wraps to 1, then 3)");
    TEST_CHECK(gaps[0] == 5 && gaps[1] == 1 && gaps[2] == 3,
               "wraps around past total_chunks, in scan order from the cursor");

    // Fully complete -- no gaps.
    update_received_ranges_mark(&r, 1 * UPDATE_CHUNK_LEN);
    update_received_ranges_mark(&r, 3 * UPDATE_CHUNK_LEN);
    update_received_ranges_mark(&r, 5 * UPDATE_CHUNK_LEN);
    n = update_received_ranges_find_gaps(&r, 0, gaps, 8);
    TEST_CHECK(n == 0, "no gaps once complete");
}

// --- update_receiver --------------------------------------------------------

static void test_preconditions_check(void)
{
    TEST_SECTION("update_preconditions_check");

    update_preconditions_t all_ok = { .relay_open = true, .no_trip_pending = true,
                                       .temp_known_and_low = true };
    TEST_CHECK(update_preconditions_check(&all_ok) == 0, "all satisfied -> 0");

    update_preconditions_t relay_bad = all_ok;
    relay_bad.relay_open = false;
    TEST_CHECK(update_preconditions_check(&relay_bad) == UPDATE_PRECOND_RELAY_CLOSED,
               "relay closed -> RELAY_CLOSED flag only");

    update_preconditions_t all_bad = { .relay_open = false, .no_trip_pending = false,
                                        .temp_known_and_low = false };
    uint8_t flags = update_preconditions_check(&all_bad);
    TEST_CHECK((flags & UPDATE_PRECOND_RELAY_CLOSED) != 0, "all-bad includes RELAY_CLOSED");
    TEST_CHECK((flags & UPDATE_PRECOND_TRIP_PENDING) != 0, "all-bad includes TRIP_PENDING");
    TEST_CHECK((flags & UPDATE_PRECOND_TOO_HOT) != 0, "all-bad includes TOO_HOT");
}

static void test_handle_begin(void)
{
    TEST_SECTION("update_receiver_handle_begin");

    update_image_header_t h = make_valid_header();
    update_preconditions_t ok_precond = { .relay_open = true, .no_trip_pending = true,
                                           .temp_known_and_low = true };

    // Active slot A (0) -> target should be B (1), regardless of what the
    // header requested.
    h.requested_slot = 0; // ESP (wrongly) thinks A is inactive
    update_begin_decision_t d = update_receiver_handle_begin(&h, &ok_precond, /*active_slot=*/0,
                                                               832u * 1024u);
    TEST_CHECK(d.outcome == UPDATE_BEGIN_ACCEPTED, "accepted when preconditions and header are fine");
    TEST_CHECK(d.target_slot == 1, "Pico's own choice (B) wins regardless of requested_slot");
    TEST_CHECK(d.requested_slot_mismatch, "mismatch between requested (0) and chosen (1) is flagged");

    h.requested_slot = 1; // ESP correctly guesses B
    d = update_receiver_handle_begin(&h, &ok_precond, /*active_slot=*/0, 832u * 1024u);
    TEST_CHECK(!d.requested_slot_mismatch, "no mismatch flagged when the ESP guessed correctly");

    // Preconditions checked before the header -- an invalid header AND a
    // failed precondition should report the precondition failure.
    update_image_header_t bad_h = h;
    bad_h.magic = 0; // also invalid
    update_preconditions_t bad_precond = { .relay_open = false, .no_trip_pending = true,
                                            .temp_known_and_low = true };
    d = update_receiver_handle_begin(&bad_h, &bad_precond, /*active_slot=*/0, 832u * 1024u);
    TEST_CHECK(d.outcome == UPDATE_BEGIN_REFUSED_PRECONDITION,
               "precondition failure reported even when the header is also bad -- preconditions checked first");
    TEST_CHECK(d.precondition_flags == UPDATE_PRECOND_RELAY_CLOSED, "specific unmet precondition named");

    // Header failure reported when preconditions are fine.
    d = update_receiver_handle_begin(&bad_h, &ok_precond, /*active_slot=*/0, 832u * 1024u);
    TEST_CHECK(d.outcome == UPDATE_BEGIN_REFUSED_HEADER_INVALID,
               "header failure reported once preconditions are satisfied");
    TEST_CHECK(d.header_check == UPDATE_IMAGE_HEADER_BAD_MAGIC, "specific header failure named");

    // active_slot B (1) -> target should be A (0).
    d = update_receiver_handle_begin(&h, &ok_precond, /*active_slot=*/1, 832u * 1024u);
    TEST_CHECK(d.target_slot == 0, "active slot B -> targets slot A");
}

static void test_retransmit_cap(void)
{
    TEST_SECTION("update_retransmit_should_continue");

    TEST_CHECK(update_retransmit_should_continue(0), "round 0 -- continue");
    TEST_CHECK(update_retransmit_should_continue(UPDATE_MAX_RETRANSMIT_ROUNDS - 1),
               "one below the cap -- still continue");
    TEST_CHECK(!update_retransmit_should_continue(UPDATE_MAX_RETRANSMIT_ROUNDS),
               "at the cap -- stop");
    TEST_CHECK(!update_retransmit_should_continue(UPDATE_MAX_RETRANSMIT_ROUNDS + 5),
               "past the cap -- stop");
}

// --- confirm -----------------------------------------------------------------

static void test_confirm_missing(void)
{
    TEST_SECTION("update_confirm_missing");

    update_confirm_checklist_t all_good = { .config_crc_ok = true, .thermocouple_plausible = true,
                                             .adc_sampling_ok = true, .all_tasks_checked_in = true,
                                             .telemetry_sent_ok = true };
    TEST_CHECK(update_confirm_missing(&all_good) == 0, "everything true -> 0, ready to confirm");

    update_confirm_checklist_t nothing_yet = { 0 };
    uint8_t missing = update_confirm_missing(&nothing_yet);
    TEST_CHECK((missing & UPDATE_CONFIRM_MISSING_CONFIG_CRC) != 0, "missing config CRC flagged");
    TEST_CHECK((missing & UPDATE_CONFIRM_MISSING_THERMOCOUPLE) != 0, "missing thermocouple flagged");
    TEST_CHECK((missing & UPDATE_CONFIRM_MISSING_ADC) != 0, "missing ADC flagged");
    TEST_CHECK((missing & UPDATE_CONFIRM_MISSING_WATCHDOG_CHECKINS) != 0,
               "missing watchdog check-ins flagged");
    TEST_CHECK((missing & UPDATE_CONFIRM_MISSING_TELEMETRY) != 0, "missing telemetry flagged");

    // One item missing -- only that bit set.
    update_confirm_checklist_t only_telemetry_missing = all_good;
    only_telemetry_missing.telemetry_sent_ok = false;
    TEST_CHECK(update_confirm_missing(&only_telemetry_missing) == UPDATE_CONFIRM_MISSING_TELEMETRY,
               "exactly one unmet item reports exactly one flag");
}

void run_test_update(void)
{
    test_image_header_roundtrip();
    test_image_header_unpack_hostile();
    test_image_header_validate();
    test_received_ranges_basic();
    test_received_ranges_hostile_offsets();
    test_received_ranges_find_gaps();
    test_preconditions_check();
    test_handle_begin();
    test_retransmit_cap();
    test_confirm_missing();
}
