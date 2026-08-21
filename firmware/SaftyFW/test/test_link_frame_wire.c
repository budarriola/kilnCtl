// Host round-trip test: SaftyFW's link_frame_pack_status()/
// link_frame_pack_fw_version() (real code, src/tasks/link_frame.c) ->
// CommonFW's real kilnlink_frame_encode_raw() + kilnlink_stuff() (the exact
// codec link_task.c calls, see link_task_send_broadcast_to()) -> real
// kilnlink_unstuff() + kilnlink_frame_decode() -> a MIRROR of the ESP-side
// parsing logic in firmware/KilnFW/App/drivers/safety_link.c.
//
// KilnFW/App/drivers/safety_link.c cannot be linked into this host test: it
// depends on ESP-IDF (FreeRTOS mutexes, esp_log.h, etc.) and per this task's
// rules is off-limits to edit right now (another agent is working in
// firmware/KilnFW/App/drivers/) even if an adapter seam were worth adding.
// So the two functions below are TRANSCRIPTIONS, not links, of:
//   - safety_apply_status()   firmware/KilnFW/App/drivers/safety_link.c:639-682
//   - safety_parse_fw_version() firmware/KilnFW/App/drivers/safety_link.c:299-333
// Every offset, mask, and the NaN-substitution rule below is copied from
// those exact lines. If this test ever disagrees with the real file, that
// disagreement is the point: it means one of the two copies drifted and
// needs reconciling by hand, not that the test is wrong.
#include <math.h>
#include <string.h>

#include "test_common.h"
#include "../src/tasks/link_frame.h"
#include "kilnlink/kilnlink_frame.h"

// --- device/task ids link_task.c actually uses for this traffic (link_task.c
// LINK_DEVICE_SAFETY/LINK_DEVICE_ESP/LINK_TASK_ID_SAFETY) -- only their
// numeric identity matters for this test (kilnlink_frame_decode() doesn't
// interpret them), so small stand-in constants are used instead of pulling
// in link_task.c's own FreeRTOS-dependent header.
#define TEST_DEV_SAFETY 1u
#define TEST_DEV_ESP    0u
#define TEST_TASK_SAFETY 7u

// --- Mirror of safety_link.c's SAFETY_FLAG_* bit values (uart_task_ids.h) --
// Same numeric values as LINK_FLAG_* in link_frame.h; kept as a separate set
// of constants here (rather than reusing LINK_FLAG_*) so a accidental edit
// to one side's header doesn't silently drag the other side's "expected"
// value along with it -- the whole point of this file is to catch exactly
// that kind of drift.
#define MIRROR_SAFETY_FLAG_LINK_UP    0x01u
#define MIRROR_SAFETY_FLAG_FAULT      0x02u
#define MIRROR_SAFETY_FLAG_ESTOP      0x04u
#define MIRROR_SAFETY_FLAG_RELAY      0x08u
#define MIRROR_SAFETY_FLAG_ENABLED    0x10u
#define MIRROR_SAFETY_FLAG_TEMP_VALID 0x20u

// safety_link.h:157's SAFETY_LINK_STATUS_FRAME_LEN, restated here (not
// #included -- that header pulls in ESP-IDF-adjacent types) since it must
// equal LINK_FRAME_STATUS_LEN for the two sides to agree at all; the very
// first assertion in test_status_frame_round_trip() below proves that.
#define SAFETY_LINK_STATUS_FRAME_LEN_MIRROR 23u

typedef struct {
    bool ok;
    uint8_t flags;
    float tc_temp_c;
    float cj_temp_c;
    uint8_t tc_fault;
    float current_a[3];
} mirror_status_t;

static float mirror_read_f32_le(const uint8_t *p)
{
    uint32_t bits = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                    ((uint32_t)p[3] << 24);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

// Mirrors safety_apply_status(), safety_link.c:639-682, field for field.
static mirror_status_t mirror_apply_status(const uint8_t *payload, uint8_t length)
{
    mirror_status_t out;
    memset(&out, 0, sizeof(out));

    // safety_link.c:644
    if (length != SAFETY_LINK_STATUS_FRAME_LEN_MIRROR || payload[0] != LINK_FRAME_STATUS_CMD) {
        out.ok = false;
        return out;
    }

    const uint8_t *p = payload;
    // safety_link.c:661
    out.flags = (uint8_t)(p[1] & ~(MIRROR_SAFETY_FLAG_LINK_UP | MIRROR_SAFETY_FLAG_FAULT));
    // safety_link.c:662-663
    out.tc_temp_c = mirror_read_f32_le(&p[2]);
    out.cj_temp_c = mirror_read_f32_le(&p[6]);
    // safety_link.c:669-672
    if (!(out.flags & MIRROR_SAFETY_FLAG_TEMP_VALID)) {
        out.tc_temp_c = NAN;
        out.cj_temp_c = NAN;
    }
    // safety_link.c:673-676
    out.tc_fault = p[10];
    out.current_a[0] = mirror_read_f32_le(&p[11]);
    out.current_a[1] = mirror_read_f32_le(&p[15]);
    out.current_a[2] = mirror_read_f32_le(&p[19]);
    out.ok = true;
    return out;
}

typedef struct {
    bool ok;
    uint16_t protocol;
    uint16_t min_compatible;
    bool have_boot_id;
    uint8_t boot_id;
} mirror_fw_version_t;

// Mirrors safety_parse_fw_version(), safety_link.c:299-333, field for field.
static mirror_fw_version_t mirror_parse_fw_version(const uint8_t *p, uint8_t len)
{
    mirror_fw_version_t out;
    memset(&out, 0, sizeof(out));

    if (len < 5u) {
        out.ok = false;
        return out;
    }
    out.protocol = (uint16_t)(p[1] | ((uint16_t)p[2] << 8));
    out.min_compatible = (uint16_t)(p[3] | ((uint16_t)p[4] << 8));
    out.ok = true;

    if (len < 7u) {
        return out; // no commit_len byte to even start the tail
    }
    size_t i = 6; // byte5 = dirty, not needed here
    uint8_t commit_len = p[i++];
    if ((size_t)commit_len + i > (size_t)len) {
        return out;
    }
    i += commit_len;
    if (i >= (size_t)len) {
        return out;
    }
    uint8_t datetime_len = p[i++];
    if ((size_t)datetime_len + i > (size_t)len) {
        return out;
    }
    i += datetime_len;
    if (i >= (size_t)len) {
        return out;
    }
    out.boot_id = p[i];
    out.have_boot_id = true;
    return out;
}

// Round-trips `payload`/`length` through the REAL kilnlink codec
// (kilnlink_frame_encode_raw + kilnlink_stuff, then kilnlink_unstuff +
// kilnlink_frame_decode) exactly the way link_task_send_broadcast_to()
// (src/tasks/link_task.c) and the ESP's receive path do. On success, returns
// true and fills `*out_payload`/`*out_length` from the decoded frame.
static bool wire_round_trip(const uint8_t *payload, uint8_t length, uint8_t *out_payload,
                             uint8_t *out_length)
{
    kilnlink_frame_t frame = {
        .msg_type = KILNLINK_MSG_BROADCAST,
        .msg_index = 42,
        .src_device = TEST_DEV_SAFETY,
        .src_task = TEST_TASK_SAFETY,
        .dst_device = TEST_DEV_ESP,
        .dst_task = TEST_TASK_SAFETY,
        .length = length,
        .payload = payload,
    };

    uint8_t raw[KILNLINK_FRAME_RAW_MAX];
    kilnlink_frame_status_t status;
    size_t raw_len = kilnlink_frame_encode_raw(&frame, raw, sizeof(raw), &status);
    if (raw_len == 0) {
        return false;
    }

    uint8_t stuffed[KILNLINK_FRAME_STUFFED_MAX];
    size_t stuffed_len = kilnlink_stuff(raw, raw_len, stuffed, sizeof(stuffed));
    if (stuffed_len == 0) {
        return false;
    }

    // Receive side: unstuff, then decode.
    uint8_t unstuffed[KILNLINK_FRAME_RAW_MAX];
    size_t unstuffed_len =
        kilnlink_unstuff(stuffed, stuffed_len, unstuffed, sizeof(unstuffed), &status);
    if (unstuffed_len == 0) {
        return false;
    }

    kilnlink_frame_t decoded;
    if (kilnlink_frame_decode(unstuffed, unstuffed_len, &decoded) != KILNLINK_FRAME_OK) {
        return false;
    }
    if (decoded.msg_type != KILNLINK_MSG_BROADCAST) {
        return false;
    }

    memcpy(out_payload, decoded.payload, decoded.length);
    *out_length = decoded.length;
    return true;
}

static void test_status_frame_round_trip(void)
{
    TEST_SECTION("status frame (Frame A) -- pack -> kilnlink wire -> mirror parse");

    TEST_CHECK(LINK_FRAME_STATUS_LEN == SAFETY_LINK_STATUS_FRAME_LEN_MIRROR,
               "LINK_FRAME_STATUS_LEN (SaftyFW) == SAFETY_LINK_STATUS_FRAME_LEN (KilnFW) -- "
               "the very first thing that has to agree");

    uint8_t payload[LINK_FRAME_STATUS_LEN];
    link_frame_pack_status(payload, /*estop=*/true, /*relay_energized=*/true,
                            /*heating_enabled=*/false, /*temp_valid=*/true, 851.25f, 23.5f,
                            0x03u, 1.25f, 2.5f, 3.75f);

    // link_frame_pack_status() never sets bits 0/1 (LINK_UP/FAULT are
    // documented as "the ESP's to own", link_frame.h) -- so on its own this
    // frame can't prove the ESP-side masking in mirror_apply_status() (line
    // "out.flags = ... & ~(LINK_UP|FAULT)") does anything at all; a peer
    // that never sets those bits would look identical whether or not the
    // mask were even applied. Simulate a rogue/buggy peer that DOES set
    // them by ORing them into the wire byte directly (bypassing the real
    // packer, which structurally cannot produce this), so the assertion
    // below is actually exercising the mask, not just observing its absence.
    payload[1] |= (MIRROR_SAFETY_FLAG_LINK_UP | MIRROR_SAFETY_FLAG_FAULT);

    uint8_t wire_payload[LINK_FRAME_STATUS_LEN];
    uint8_t wire_length = 0;
    bool sent = wire_round_trip(payload, LINK_FRAME_STATUS_LEN, wire_payload, &wire_length);
    TEST_CHECK(sent, "status frame survives kilnlink_frame_encode_raw+stuff+unstuff+decode");
    TEST_CHECK(wire_length == LINK_FRAME_STATUS_LEN, "wire length unchanged by the codec round trip");

    mirror_status_t parsed = mirror_apply_status(wire_payload, wire_length);
    TEST_CHECK(parsed.ok, "mirror of safety_apply_status() accepts the wire bytes");
    TEST_CHECK((parsed.flags & MIRROR_SAFETY_FLAG_ESTOP) != 0, "ESTOP bit survives pack->wire->parse");
    TEST_CHECK((parsed.flags & MIRROR_SAFETY_FLAG_RELAY) != 0, "RELAY bit survives pack->wire->parse");
    TEST_CHECK((parsed.flags & MIRROR_SAFETY_FLAG_ENABLED) == 0,
               "ENABLED bit (not set) stays clear through pack->wire->parse");
    TEST_CHECK((parsed.flags & MIRROR_SAFETY_FLAG_TEMP_VALID) != 0,
               "TEMP_VALID bit survives pack->wire->parse");
    // LINK_UP/FAULT are ESP-owned and must never be settable by the Pico's
    // packer at all, wire or no wire.
    TEST_CHECK((parsed.flags & (MIRROR_SAFETY_FLAG_LINK_UP | MIRROR_SAFETY_FLAG_FAULT)) == 0,
               "LINK_UP/FAULT bits are never set by the Pico side, even after a real wire trip");
    TEST_CHECK_NEAR(parsed.tc_temp_c, 851.25f, 0.0001, "safety_tc_c survives pack->wire->parse");
    TEST_CHECK_NEAR(parsed.cj_temp_c, 23.5f, 0.0001, "cj_c survives pack->wire->parse");
    TEST_CHECK(parsed.tc_fault == 0x03u, "tc_fault_bits survives pack->wire->parse");
    TEST_CHECK_NEAR(parsed.current_a[0], 1.25f, 0.0001, "amps1 survives pack->wire->parse");
    TEST_CHECK_NEAR(parsed.current_a[1], 2.5f, 0.0001, "amps2 survives pack->wire->parse");
    TEST_CHECK_NEAR(parsed.current_a[2], 3.75f, 0.0001, "amps3 survives pack->wire->parse");
}

static void test_status_frame_nan_when_invalid(void)
{
    TEST_SECTION("status frame -- NaN-when-invalid rule (LINK_PROTOCOL.md: send NaN, never 0)");

    uint8_t payload[LINK_FRAME_STATUS_LEN];
    // temp_valid=false, but deliberately pass REAL (non-NaN, non-zero)
    // numbers rather than NAN -- simulating a caller that slipped and forgot
    // LINK_PROTOCOL.md's "send NaN, never 0" rule (link_frame_pack_status()
    // itself is documented as passing tc_c/cj_c through UNCHANGED, not
    // substituting NaN on the caller's behalf -- see link_frame.h). If this
    // test instead fed NAN in here, it would pass even with the mirror's
    // enforcement deleted (proved during review: packing real NaN in makes
    // the check vacuous), so a real number is used specifically to prove
    // safety_link.c:669-672's own coercion -- "the flag is the authority;
    // enforce it here rather than trusting the far side to have been
    // careful" -- is what makes the assertions below pass, not the packer.
    link_frame_pack_status(payload, false, false, false, /*temp_valid=*/false, 777.0f, 888.0f, 0,
                            0.0f, 0.0f, 0.0f);

    uint8_t wire_payload[LINK_FRAME_STATUS_LEN];
    uint8_t wire_length = 0;
    TEST_CHECK(wire_round_trip(payload, LINK_FRAME_STATUS_LEN, wire_payload, &wire_length),
               "NaN-carrying status frame survives the wire round trip");

    mirror_status_t parsed = mirror_apply_status(wire_payload, wire_length);
    TEST_CHECK(parsed.ok, "mirror parse accepts the NaN-carrying frame");
    TEST_CHECK(isnan(parsed.tc_temp_c), "tc_temp_c is NaN, not 0, when TEMP_VALID is clear");
    TEST_CHECK(isnan(parsed.cj_temp_c), "cj_temp_c is NaN, not 0, when TEMP_VALID is clear");

    // Negative proof this isn't vacuous: pack a frame that DOES claim
    // temp_valid with a real (non-NaN) reading and confirm the mirror parser
    // does NOT force it to NaN -- i.e. the NaN above is really gated on the
    // flag, not just always emitted.
    uint8_t payload2[LINK_FRAME_STATUS_LEN];
    link_frame_pack_status(payload2, false, false, false, /*temp_valid=*/true, 100.0f, 20.0f, 0,
                            0.0f, 0.0f, 0.0f);
    uint8_t wire2[LINK_FRAME_STATUS_LEN];
    uint8_t wire2_len = 0;
    TEST_CHECK(wire_round_trip(payload2, LINK_FRAME_STATUS_LEN, wire2, &wire2_len),
               "temp_valid=true frame also survives the wire");
    mirror_status_t parsed2 = mirror_apply_status(wire2, wire2_len);
    TEST_CHECK(!isnan(parsed2.tc_temp_c) && !isnan(parsed2.cj_temp_c),
               "when TEMP_VALID is set, real readings are NOT coerced to NaN "
               "(proves the NaN check above is conditional, not unconditional)");
}

static void test_status_frame_negative(void)
{
    TEST_SECTION("status frame -- mirror parser rejects malformed frames");

    uint8_t payload[LINK_FRAME_STATUS_LEN];
    link_frame_pack_status(payload, false, false, false, true, 1.0f, 2.0f, 0, 0.0f, 0.0f, 0.0f);

    // Truncated payload: one byte short.
    {
        mirror_status_t parsed = mirror_apply_status(payload, LINK_FRAME_STATUS_LEN - 1);
        TEST_CHECK(!parsed.ok, "length one short of 23 is rejected");
    }

    // Wrong opcode: corrupt byte0.
    {
        uint8_t bad[LINK_FRAME_STATUS_LEN];
        memcpy(bad, payload, sizeof(bad));
        bad[0] = 0x0Bu; // FW_VERSION's opcode, not GET_STATUS's
        mirror_status_t parsed = mirror_apply_status(bad, LINK_FRAME_STATUS_LEN);
        TEST_CHECK(!parsed.ok, "wrong opcode (0x0B where 0x01 is expected) is rejected");
    }

    // Wrong length: one byte too long (e.g. trailing garbage).
    {
        uint8_t bad[LINK_FRAME_STATUS_LEN + 1];
        memcpy(bad, payload, LINK_FRAME_STATUS_LEN);
        bad[LINK_FRAME_STATUS_LEN] = 0xFFu;
        mirror_status_t parsed = mirror_apply_status(bad, LINK_FRAME_STATUS_LEN + 1);
        TEST_CHECK(!parsed.ok, "length one over 23 (trailing garbage) is rejected");
    }
}

static void test_fw_version_round_trip(void)
{
    TEST_SECTION("fw_version frame (Frame C) -- pack -> kilnlink wire -> mirror parse");

    const char *commit = "abc1234";
    const char *datetime = "2026-08-19T12:00:00Z";
    uint8_t buf[64];
    size_t len = link_frame_pack_fw_version(buf, sizeof(buf), /*protocol=*/7, /*min_compat=*/5,
                                             /*dirty=*/1, commit, (uint8_t)strlen(commit), datetime,
                                             (uint8_t)strlen(datetime), /*boot_id=*/9,
                                             /*config_version=*/2, /*config_crc=*/0xBEEFu);
    TEST_CHECK(len > 0, "link_frame_pack_fw_version succeeds with a normal-sized commit/datetime");

    uint8_t wire_payload[64];
    uint8_t wire_length = 0;
    TEST_CHECK(wire_round_trip(buf, (uint8_t)len, wire_payload, &wire_length),
               "fw_version frame survives the kilnlink wire round trip");
    TEST_CHECK(wire_length == (uint8_t)len, "fw_version wire length unchanged by the codec round trip");

    mirror_fw_version_t parsed = mirror_parse_fw_version(wire_payload, wire_length);
    TEST_CHECK(parsed.ok, "mirror of safety_parse_fw_version() accepts the wire bytes");
    TEST_CHECK(parsed.protocol == 7, "protocol_version survives pack->wire->parse");
    TEST_CHECK(parsed.min_compatible == 5, "min_compatible survives pack->wire->parse");
    TEST_CHECK(parsed.have_boot_id, "boot_id is reachable (frame wasn't truncated)");
    TEST_CHECK(parsed.boot_id == 9, "boot_id survives pack->wire->parse");
}

static void test_fw_version_negative(void)
{
    TEST_SECTION("fw_version frame -- mirror parser rejects/degrades malformed frames");

    // Too short to read even bytes 1-4 (protocol/min_compatible).
    {
        uint8_t buf[4] = {LINK_FRAME_FW_VERSION_CMD, 0, 0, 0};
        mirror_fw_version_t parsed = mirror_parse_fw_version(buf, 4);
        TEST_CHECK(!parsed.ok, "length 4 (< 5) rejected outright -- can't even read the floor fields");
    }

    // Long enough for protocol/min_compatible, but truncated before commit_len.
    {
        uint8_t buf[6] = {LINK_FRAME_FW_VERSION_CMD, 7, 0, 5, 0, 1 /* dirty */};
        mirror_fw_version_t parsed = mirror_parse_fw_version(buf, 6);
        TEST_CHECK(parsed.ok, "length 6 still reports protocol/min_compatible (floor rule)");
        TEST_CHECK(parsed.protocol == 7 && parsed.min_compatible == 5,
                   "floor fields correct even when the tail is unreachable");
        TEST_CHECK(!parsed.have_boot_id,
                   "boot_id NOT reported as reachable when the frame is truncated before it "
                   "(a stale/zero boot_id must never look real)");
    }

    // commit_len claims more bytes than are actually present.
    {
        uint8_t buf[8] = {LINK_FRAME_FW_VERSION_CMD, 7, 0, 5, 0, 1, /*commit_len=*/200, 'a'};
        mirror_fw_version_t parsed = mirror_parse_fw_version(buf, 8);
        TEST_CHECK(parsed.ok, "floor fields still reported");
        TEST_CHECK(!parsed.have_boot_id, "boot_id not reachable when commit_len overruns the buffer");
    }
}

void run_test_link_frame_wire(void)
{
    test_status_frame_round_trip();
    test_status_frame_nan_when_invalid();
    test_status_frame_negative();
    test_fw_version_round_trip();
    test_fw_version_negative();
}
