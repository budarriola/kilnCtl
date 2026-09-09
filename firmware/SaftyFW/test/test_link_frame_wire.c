// Host round-trip test: SaftyFW's link_frame_pack_status()/
// link_frame_pack_fw_version() (real code, src/tasks/link_frame.c) ->
// CommonFW's real kilnlink_frame_encode_raw() + kilnlink_stuff() (the exact
// codec link_task.c calls, see link_task_send_broadcast_to()) -> real
// kilnlink_unstuff() + kilnlink_frame_decode() -> a MIRROR of the ESP-side
// parsing logic in firmware/KilnFW/App/drivers/safety_link.c.
//
// KilnFW/App/drivers/safety_link.c cannot be linked into this host test: it
// depends on ESP-IDF (FreeRTOS mutexes, esp_log.h, etc.), even if an adapter
// seam were worth adding. So the two functions below are TRANSCRIPTIONS, not
// links, of:
//   - safety_apply_status()   firmware/KilnFW/App/drivers/safety_link.c
//   - safety_parse_fw_version() firmware/KilnFW/App/drivers/safety_link.c
// Every offset, mask, and the NaN-substitution rule below is copied from
// those exact lines. If this test ever disagrees with the real file, that
// disagreement is the point: it means one of the two copies drifted and
// needs reconciling by hand, not that the test is wrong.
//
// 2026-08-23: mirror_apply_status() updated for Frame A's V1/V2 split
// (LINK_FRAME_STATUS_LEN_V1 == 23, LINK_FRAME_STATUS_LEN_V2 == 24, byte 23 =
// tx_dropped_sat) -- see link_frame.h's own doc comment on
// link_frame_pack_status() for the full skew-safety story this mirrors.
#include <math.h>
#include <string.h>

#include "test_common.h"
#include "../src/tasks/link_frame.h"
#include "kilnlink/kilnlink_frame.h"
#include "kilnlink/kilnlink_frame_a_offsets.h" // KILNLINK_FRAME_A_* -- single source of truth for
                                                // Frame A's byte offsets/lengths (ROADMAP.md M15).
                                                // Freestanding, no ESP-IDF-adjacent types, so unlike
                                                // safety_link.h this one IS safe to #include directly
                                                // rather than restate -- see this file's own header
                                                // comment on SAFETY_LINK_STATUS_FRAME_LEN_V1_MIRROR
                                                // etc. for why THOSE stay restated.

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
#define MIRROR_SAFETY_FLAG_TC_NOT_INSTALLED 0x40u
#define MIRROR_SAFETY_FLAG_TC_INJECTED      0x80u
#define MIRROR_SAFETY_FLAG2_BORROWED        0x01u
#define MIRROR_SAFETY_FLAG2_CJ_VALID        0x02u

// safety_link.h's SAFETY_LINK_STATUS_FRAME_LEN_V1/_V2/_V3 (KilnFW) and this
// file's own LINK_FRAME_STATUS_LEN_V1/_V2/_V3 (link_frame.h, SaftyFW) now
// both resolve to kilnlink_frame_a_offsets.h's KILNLINK_FRAME_A_LEN_V1/_V2/
// _V3 -- ROADMAP.md M15. Aliased here (not restated as separate literals)
// since they must equal that same shared value for the two firmwares to
// agree at all; the very first assertions in test_status_frame_round_trip()
// below prove that.
#define SAFETY_LINK_STATUS_FRAME_LEN_V1_MIRROR KILNLINK_FRAME_A_LEN_V1
#define SAFETY_LINK_STATUS_FRAME_LEN_V2_MIRROR KILNLINK_FRAME_A_LEN_V2
#define SAFETY_LINK_STATUS_FRAME_LEN_V3_MIRROR KILNLINK_FRAME_A_LEN_V3

typedef struct {
    bool ok;
    uint8_t flags;
    float tc_temp_c;
    float cj_temp_c;
    uint8_t tc_fault;
    float current_a[3];
    bool tx_dropped_known; // true iff a V2 (24-byte) or wider frame was parsed
    uint8_t tx_dropped_sat; // meaningless when tx_dropped_known is false
    bool borrowed_known; // true iff a V3 (26-byte) frame was parsed
    bool borrowed;       // meaningless when borrowed_known is false
    uint8_t borrowed_zone_index; // meaningless when borrowed_known is false
    // 2026-09-08: cj_valid_known/cj_valid mirror LINK_FLAG2_CJ_VALID
    // (link_frame.h) -- same "meaningless until a V3 frame proves otherwise"
    // convention as borrowed_known above. Deliberately does NOT gate
    // cj_temp_c's NaN-substitution above (only MIRROR_SAFETY_FLAG_TEMP_VALID
    // does that in this mirror, matching the real pre-fix safety_link_frames.c
    // behaviour this file transcribes) -- see test_status_frame_v3_cj_valid()
    // for the standalone proof that cj_valid survives pack->wire->parse
    // independent of temp_valid.
    bool cj_valid_known;
    bool cj_valid;
} mirror_status_t;

static float mirror_read_f32_le(const uint8_t *p)
{
    uint32_t bits = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                    ((uint32_t)p[3] << 24);
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

// Mirrors safety_apply_status(), firmware/KilnFW/App/drivers/safety_link.c,
// field for field -- updated 2026-08-23 for the V1/V2 length tolerance that
// same file's real length check now has (accepts EITHER 23 or 24, never just
// one, so an old peer's 23-byte frame and a new peer's 24-byte frame are
// both still accepted -- see link_frame.h's skew-safety comment for why
// that symmetry matters).
static mirror_status_t mirror_apply_status(const uint8_t *payload, uint8_t length)
{
    mirror_status_t out;
    memset(&out, 0, sizeof(out));

    if ((length != SAFETY_LINK_STATUS_FRAME_LEN_V1_MIRROR &&
         length != SAFETY_LINK_STATUS_FRAME_LEN_V2_MIRROR &&
         length != SAFETY_LINK_STATUS_FRAME_LEN_V3_MIRROR) ||
        payload[0] != LINK_FRAME_STATUS_CMD) {
        out.ok = false;
        return out;
    }

    const uint8_t *p = payload;
    out.flags = (uint8_t)(p[KILNLINK_FRAME_A_OFF_FLAGS] & ~(MIRROR_SAFETY_FLAG_LINK_UP | MIRROR_SAFETY_FLAG_FAULT));
    out.tc_temp_c = mirror_read_f32_le(&p[KILNLINK_FRAME_A_OFF_TC_TEMP_C]);
    out.cj_temp_c = mirror_read_f32_le(&p[KILNLINK_FRAME_A_OFF_CJ_TEMP_C]);
    if (!(out.flags & MIRROR_SAFETY_FLAG_TEMP_VALID)) {
        out.tc_temp_c = NAN;
        out.cj_temp_c = NAN;
    }
    out.tc_fault = p[KILNLINK_FRAME_A_OFF_TC_FAULT];
    out.current_a[0] = mirror_read_f32_le(&p[KILNLINK_FRAME_A_OFF_AMPS1]);
    out.current_a[1] = mirror_read_f32_le(&p[KILNLINK_FRAME_A_OFF_AMPS2]);
    out.current_a[2] = mirror_read_f32_le(&p[KILNLINK_FRAME_A_OFF_AMPS3]);
    if (length == SAFETY_LINK_STATUS_FRAME_LEN_V2_MIRROR ||
        length == SAFETY_LINK_STATUS_FRAME_LEN_V3_MIRROR) {
        out.tx_dropped_known = true;
        out.tx_dropped_sat = p[KILNLINK_FRAME_A_OFF_TX_DROPPED_SAT];
    } else {
        out.tx_dropped_known = false;
        out.tx_dropped_sat = 0;
    }
    if (length == SAFETY_LINK_STATUS_FRAME_LEN_V3_MIRROR) {
        out.borrowed_known = true;
        out.borrowed = (p[KILNLINK_FRAME_A_OFF_FLAGS2] & MIRROR_SAFETY_FLAG2_BORROWED) != 0u;
        out.borrowed_zone_index = p[KILNLINK_FRAME_A_OFF_BORROWED_ZONE_INDEX];
        out.cj_valid_known = true;
        out.cj_valid = (p[KILNLINK_FRAME_A_OFF_FLAGS2] & MIRROR_SAFETY_FLAG2_CJ_VALID) != 0u;
    } else {
        out.borrowed_known = false;
        out.borrowed = false;
        out.borrowed_zone_index = 0;
        out.cj_valid_known = false;
        out.cj_valid = false;
    }
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

    TEST_CHECK(LINK_FRAME_STATUS_LEN_V1 == SAFETY_LINK_STATUS_FRAME_LEN_V1_MIRROR,
               "LINK_FRAME_STATUS_LEN_V1 (SaftyFW) == SAFETY_LINK_STATUS_FRAME_LEN_V1 (KilnFW) -- "
               "the very first thing that has to agree");
    TEST_CHECK(LINK_FRAME_STATUS_LEN_V2 == SAFETY_LINK_STATUS_FRAME_LEN_V2_MIRROR,
               "LINK_FRAME_STATUS_LEN_V2 (SaftyFW) == SAFETY_LINK_STATUS_FRAME_LEN_V2 (KilnFW)");

    uint8_t payload[LINK_FRAME_STATUS_LEN_V2];
    link_frame_pack_status(payload, /*estop=*/true, /*relay_energized=*/true,
                            /*heating_enabled=*/false, /*temp_valid=*/true, 851.25f, 23.5f,
                            0x03u, 1.25f, 2.5f, 3.75f, /*tc_not_installed=*/false, /*tc_injected=*/false,
                            /*peer_supports_status_v2=*/false, /*tx_dropped_sat=*/0, /*peer_supports_status_v3=*/false, /*is_borrowed=*/false, LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN, /*cj_valid=*/false);

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

    uint8_t wire_payload[LINK_FRAME_STATUS_LEN_V1];
    uint8_t wire_length = 0;
    bool sent = wire_round_trip(payload, LINK_FRAME_STATUS_LEN_V1, wire_payload, &wire_length);
    TEST_CHECK(sent, "status frame survives kilnlink_frame_encode_raw+stuff+unstuff+decode");
    TEST_CHECK(wire_length == LINK_FRAME_STATUS_LEN_V1, "wire length unchanged by the codec round trip");

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
                            0.0f, 0.0f, 0.0f, false, false, /*peer_supports_status_v2=*/false, 0, false, false, LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN, /*cj_valid=*/false);

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
                            0.0f, 0.0f, 0.0f, false, false, /*peer_supports_status_v2=*/false, 0, false, false, LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN, /*cj_valid=*/false);
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
    TEST_SECTION("status frame -- mirror parser rejects malformed frames, tolerates V1/V2");

    uint8_t payload[LINK_FRAME_STATUS_LEN_V1];
    link_frame_pack_status(payload, false, false, false, true, 1.0f, 2.0f, 0, 0.0f, 0.0f, 0.0f,
                            false, false, /*peer_supports_status_v2=*/false, 0, false, false, LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN, /*cj_valid=*/false);

    // Truncated payload: one byte short of the (still valid) V1 length.
    {
        mirror_status_t parsed = mirror_apply_status(payload, LINK_FRAME_STATUS_LEN_V1 - 1);
        TEST_CHECK(!parsed.ok, "length one short of V1 (22) is rejected");
    }

    // Wrong opcode: corrupt byte0.
    {
        uint8_t bad[LINK_FRAME_STATUS_LEN_V1];
        memcpy(bad, payload, sizeof(bad));
        bad[0] = 0x0Bu; // FW_VERSION's opcode, not GET_STATUS's
        mirror_status_t parsed = mirror_apply_status(bad, LINK_FRAME_STATUS_LEN_V1);
        TEST_CHECK(!parsed.ok, "wrong opcode (0x0B where 0x01 is expected) is rejected");
    }

    // 2026-08-23: 24 (V2) is now a LEGITIMATE length, not "trailing garbage"
    // -- this is the length-boundary case that would have caught a
    // regression to the old exact-match-23-only check (the very bug this
    // whole pass fixes on the real KilnFW side). One byte past V1 must be
    // ACCEPTED now, where it used to be rejected.
    {
        uint8_t v2[LINK_FRAME_STATUS_LEN_V2];
        memcpy(v2, payload, LINK_FRAME_STATUS_LEN_V1);
        v2[LINK_FRAME_STATUS_LEN_V1] = 42u; // tx_dropped_sat
        mirror_status_t parsed = mirror_apply_status(v2, LINK_FRAME_STATUS_LEN_V2);
        TEST_CHECK(parsed.ok, "length 24 (V2) is ACCEPTED, not rejected as trailing garbage");
        TEST_CHECK(parsed.tx_dropped_known, "a V2-length frame reports tx_dropped_known");
        TEST_CHECK(parsed.tx_dropped_sat == 42u, "byte 23 is read as tx_dropped_sat on a V2 frame");
    }

    // The new boundary: one byte past the WIDER length (25) is still
    // trailing garbage and must still be rejected -- proves the tolerance
    // added above is exactly {23, 24}, not "any length >= 23".
    {
        uint8_t bad[LINK_FRAME_STATUS_LEN_V2 + 1];
        memcpy(bad, payload, LINK_FRAME_STATUS_LEN_V1);
        bad[LINK_FRAME_STATUS_LEN_V1] = 42u;
        bad[LINK_FRAME_STATUS_LEN_V2] = 0xFFu;
        mirror_status_t parsed = mirror_apply_status(bad, LINK_FRAME_STATUS_LEN_V2 + 1);
        TEST_CHECK(!parsed.ok, "length one over V2 (25, trailing garbage) is still rejected");
    }
}

// link_frame_status_v2_supported() -- the pure peer-version gate
// link_task_send_status() uses to decide whether it may EVER emit a V2
// frame at the current peer. Boundary-tested directly, same as
// link_frame_versions_compatible() below is for the larger question.
static void test_status_v2_supported_gate(void)
{
    TEST_SECTION("link_frame_status_v2_supported -- peer protocol_version gate boundary");

    TEST_CHECK(!link_frame_status_v2_supported(0u),
               "0 (never announced -- s_peer_protocol_version's default) is NOT supported: "
               "the safe default before any ANNOUNCE_VERSION has arrived");
    TEST_CHECK(!link_frame_status_v2_supported(5u),
               "protocol_version 5 (the pre-V2 version this whole repo shipped until now) "
               "is NOT supported");
    TEST_CHECK(link_frame_status_v2_supported(6u),
               "protocol_version 6 (LINK_FRAME_STATUS_V2_MIN_PROTOCOL itself) IS supported");
    TEST_CHECK(link_frame_status_v2_supported(7u), "protocol_version 7 (above the floor) IS supported");
}

// link_frame_rollback_result_supported() -- same gate shape as
// link_frame_status_v2_supported() above, mirrored for SAFETY_CMD_ROLLBACK_
// RESULT (0x25)'s own floor (LINK_FRAME_ROLLBACK_RESULT_MIN_PROTOCOL = 9).
static void test_rollback_result_supported_gate(void)
{
    TEST_SECTION("link_frame_rollback_result_supported -- peer protocol_version gate boundary");

    TEST_CHECK(!link_frame_rollback_result_supported(0u),
               "0 (never announced -- s_peer_protocol_version's default) is NOT supported: "
               "the safe default before any ANNOUNCE_VERSION has arrived");
    TEST_CHECK(!link_frame_rollback_result_supported(8u),
               "protocol_version 8 (the pre-ROLLBACK_RESULT version this whole repo shipped "
               "until now) is NOT supported");
    TEST_CHECK(link_frame_rollback_result_supported(9u),
               "protocol_version 9 (LINK_FRAME_ROLLBACK_RESULT_MIN_PROTOCOL itself) IS supported");
    TEST_CHECK(link_frame_rollback_result_supported(10u), "protocol_version 10 (above the floor) IS supported");
}

// 2026-08-23, the DIAG-frame-went-dark investigation: link_frame_saturate_
// tx_dropped() (link_frame.c) is the pure conversion feeding byte 23's
// tx_dropped_sat wire value. Tested directly at its own boundaries first --
// no wire round trip needed for the saturation math itself.
static void test_saturate_tx_dropped(void)
{
    TEST_SECTION("link_frame_saturate_tx_dropped -- saturation boundary");

    TEST_CHECK(link_frame_saturate_tx_dropped(0u) == 0u, "0 drops -> 0");
    TEST_CHECK(link_frame_saturate_tx_dropped(1u) == 1u, "1 drop -> 1");
    TEST_CHECK(link_frame_saturate_tx_dropped(LINK_FRAME_STATUS_TX_DROPPED_SAT_MAX) ==
                   (uint8_t)LINK_FRAME_STATUS_TX_DROPPED_SAT_MAX,
               "the largest real value (254) is carried through exactly, not saturated early");
    TEST_CHECK(link_frame_saturate_tx_dropped(LINK_FRAME_STATUS_TX_DROPPED_SAT_MAX + 1u) == 255u,
               "one past the max (255 real drops) saturates to the 255 sentinel");
    TEST_CHECK(link_frame_saturate_tx_dropped(0xFFFFFFFFu) == 255u,
               "the largest possible uint32_t input also saturates to 255, not wraps/truncates");
}

// V2 (24-byte) Frame A -- the actual point of this pass: verify the byte is
// both present when the peer is known to support it, absent (frame stays 23
// bytes) when it is not, and readable by the mirror in the V2 case. This is
// the "verify that claim against your own change" the coordinator asked
// for, not an assumption.
static void test_status_frame_v2_tx_dropped(void)
{
    TEST_SECTION("status frame -- V2 tx_dropped_sat byte, both directions of peer support");

    // Peer NOT known to support V2 (the safe default: never announced, or
    // announced an old protocol_version) -- must stay 23 bytes, byte 23
    // never written, mirror reports tx_dropped_known == false.
    {
        uint8_t payload[LINK_FRAME_STATUS_LEN_V2];
        memset(payload, 0xAAu, sizeof(payload)); // poison byte 23 so "untouched" is provable
        size_t len = link_frame_pack_status(payload, false, false, false, true, 1.0f, 2.0f, 0,
                                             0.0f, 0.0f, 0.0f, false, false,
                                             /*peer_supports_status_v2=*/false, /*tx_dropped_sat=*/77u, false, false, LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN, /*cj_valid=*/false);
        TEST_CHECK(len == LINK_FRAME_STATUS_LEN_V1,
                   "peer_supports_status_v2=false -> returns the V1 (23) length");
        TEST_CHECK(payload[LINK_FRAME_STATUS_LEN_V1] == 0xAAu,
                   "byte 23 is left untouched (still poisoned) when V2 is not emitted -- "
                   "tx_dropped_sat=77 passed in is NOT written anywhere");

        mirror_status_t parsed = mirror_apply_status(payload, (uint8_t)len);
        TEST_CHECK(parsed.ok, "V1-length frame still parses");
        TEST_CHECK(!parsed.tx_dropped_known, "mirror correctly reports tx_dropped_known == false for a V1 frame");
    }

    // Peer KNOWN to support V2 -- 24 bytes, byte 23 carries tx_dropped_sat,
    // mirror reads it back.
    {
        uint8_t payload[LINK_FRAME_STATUS_LEN_V2];
        size_t len = link_frame_pack_status(payload, false, false, false, true, 1.0f, 2.0f, 0,
                                             0.0f, 0.0f, 0.0f, false, false,
                                             /*peer_supports_status_v2=*/true, /*tx_dropped_sat=*/200u, false, false, LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN, /*cj_valid=*/false);
        TEST_CHECK(len == LINK_FRAME_STATUS_LEN_V2,
                   "peer_supports_status_v2=true -> returns the V2 (24) length");
        TEST_CHECK(payload[LINK_FRAME_STATUS_LEN_V1] == 200u, "byte 23 carries tx_dropped_sat exactly");

        uint8_t wire_payload[LINK_FRAME_STATUS_LEN_V2];
        uint8_t wire_length = 0;
        TEST_CHECK(wire_round_trip(payload, (uint8_t)len, wire_payload, &wire_length),
                   "V2 status frame survives the real kilnlink wire round trip");
        TEST_CHECK(wire_length == LINK_FRAME_STATUS_LEN_V2, "wire length is 24, unchanged by the codec");

        mirror_status_t parsed = mirror_apply_status(wire_payload, wire_length);
        TEST_CHECK(parsed.ok, "V2-length frame parses");
        TEST_CHECK(parsed.tx_dropped_known, "mirror correctly reports tx_dropped_known == true for a V2 frame");
        TEST_CHECK(parsed.tx_dropped_sat == 200u, "tx_dropped_sat survives pack->wire->parse");
    }
}

// link_frame_status_v3_supported() -- same boundary shape as
// link_frame_status_v2_supported() above, one protocol version higher.
static void test_status_v3_supported_gate(void)
{
    TEST_SECTION("link_frame_status_v3_supported -- peer protocol_version gate boundary");

    TEST_CHECK(!link_frame_status_v3_supported(0u),
               "0 (never announced) is NOT supported: the safe default before any "
               "ANNOUNCE_VERSION has arrived");
    TEST_CHECK(!link_frame_status_v3_supported(9u),
               "protocol_version 9 (the pre-V3/BORROWED version this whole repo shipped "
               "until now) is NOT supported");
    TEST_CHECK(link_frame_status_v3_supported(10u),
               "protocol_version 10 (LINK_FRAME_STATUS_V3_MIN_PROTOCOL itself) IS supported");
    TEST_CHECK(link_frame_status_v3_supported(11u), "protocol_version 11 (above the floor) IS supported");
}

// V3 (26-byte) Frame A, the BORROWED status flag -- TASK 1's actual point.
// Same "verify the byte both ways" shape as test_status_frame_v2_tx_dropped()
// above, plus the V2-gate-implies-V3-gate interaction: a peer known to
// support V2 but NOT V3 must still get exactly the 24-byte V2 frame, never a
// half-grown 25/26-byte one.
static void test_status_frame_v3_borrowed(void)
{
    TEST_SECTION("status frame -- V3 BORROWED flag (bytes 24/25), every peer-support combination");

    TEST_CHECK(LINK_FRAME_STATUS_LEN_V3 == SAFETY_LINK_STATUS_FRAME_LEN_V3_MIRROR,
               "LINK_FRAME_STATUS_LEN_V3 (SaftyFW) == SAFETY_LINK_STATUS_FRAME_LEN_V3 (KilnFW)");

    // Peer supports V2 but NOT V3 -- must stay 24 bytes, bytes 24/25 never
    // written, even though is_borrowed=true and a real zone index are passed
    // in. This is the case that would silently overrun/desync the wire if
    // link_frame_pack_status() trusted peer_supports_status_v3 without also
    // gating on peer_supports_status_v2 having been the caller's basis for
    // it (see this function's own doc comment, link_frame.h).
    {
        uint8_t payload[LINK_FRAME_STATUS_LEN_V3];
        memset(payload, 0xAAu, sizeof(payload)); // poison bytes 24/25
        size_t len = link_frame_pack_status(payload, false, false, false, true, 1.0f, 2.0f, 0,
                                             0.0f, 0.0f, 0.0f, false, false,
                                             /*peer_supports_status_v2=*/true, /*tx_dropped_sat=*/5u,
                                             /*peer_supports_status_v3=*/false, /*is_borrowed=*/true,
                                             /*borrowed_zone_index=*/1u, /*cj_valid=*/false);
        TEST_CHECK(len == LINK_FRAME_STATUS_LEN_V2,
                   "peer_supports_status_v3=false -> returns the V2 (24) length even though "
                   "is_borrowed=true was passed in");
        TEST_CHECK(payload[24] == 0xAAu && payload[25] == 0xAAu,
                   "bytes 24/25 are left untouched (still poisoned) when V3 is not emitted");

        mirror_status_t parsed = mirror_apply_status(payload, (uint8_t)len);
        TEST_CHECK(parsed.ok, "V2-length frame still parses");
        TEST_CHECK(!parsed.borrowed_known, "mirror correctly reports borrowed_known == false for a V2 frame");
    }

    // Peer supports V2 but is_borrowed=false / zone unknown -- proves the bit
    // and sentinel are both driven by the arguments, not hard-coded true.
    {
        uint8_t payload[LINK_FRAME_STATUS_LEN_V3];
        size_t len = link_frame_pack_status(payload, false, false, false, true, 1.0f, 2.0f, 0,
                                             0.0f, 0.0f, 0.0f, false, false,
                                             /*peer_supports_status_v2=*/true, /*tx_dropped_sat=*/0u,
                                             /*peer_supports_status_v3=*/true, /*is_borrowed=*/false,
                                             LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN, /*cj_valid=*/false);
        TEST_CHECK(len == LINK_FRAME_STATUS_LEN_V3, "peer_supports_status_v3=true -> returns the V3 (26) length");
        TEST_CHECK(payload[24] == 0u, "flags2 byte is 0 when is_borrowed=false");
        TEST_CHECK(payload[25] == LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN,
                   "borrowed_zone_index sentinel (0xFF) survives pack when the zone is unknown");

        mirror_status_t parsed = mirror_apply_status(payload, (uint8_t)len);
        TEST_CHECK(parsed.ok, "V3-length frame parses");
        TEST_CHECK(parsed.borrowed_known, "mirror reports borrowed_known == true for a V3 frame");
        TEST_CHECK(!parsed.borrowed, "borrowed bit is clear when is_borrowed=false");
        TEST_CHECK(parsed.borrowed_zone_index == LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN,
                   "sentinel round-trips through the mirror parse");
    }

    // Peer supports V3 AND is_borrowed=true with a real, commissioned zone
    // index -- the actual "S13's condition is visible on the wire" proof,
    // taken all the way through the real kilnlink wire codec, not just the
    // packer.
    {
        uint8_t payload[LINK_FRAME_STATUS_LEN_V3];
        size_t len = link_frame_pack_status(payload, false, false, false, true, 1.0f, 2.0f, 0,
                                             0.0f, 0.0f, 0.0f, false, false,
                                             /*peer_supports_status_v2=*/true, /*tx_dropped_sat=*/0u,
                                             /*peer_supports_status_v3=*/true, /*is_borrowed=*/true,
                                             /*borrowed_zone_index=*/2u, /*cj_valid=*/false);
        TEST_CHECK(len == LINK_FRAME_STATUS_LEN_V3, "V3 length returned");
        TEST_CHECK((payload[24] & 0x01u) != 0u, "flags2 bit 0 (BORROWED) is set when is_borrowed=true");
        TEST_CHECK(payload[25] == 2u, "borrowed_zone_index (2) survives pack exactly");

        uint8_t wire_payload[LINK_FRAME_STATUS_LEN_V3];
        uint8_t wire_length = 0;
        TEST_CHECK(wire_round_trip(payload, (uint8_t)len, wire_payload, &wire_length),
                   "V3 status frame survives the real kilnlink wire round trip");
        TEST_CHECK(wire_length == LINK_FRAME_STATUS_LEN_V3, "wire length is 26, unchanged by the codec");

        mirror_status_t parsed = mirror_apply_status(wire_payload, wire_length);
        TEST_CHECK(parsed.ok, "V3-length frame parses after a real wire round trip");
        TEST_CHECK(parsed.borrowed_known, "mirror reports borrowed_known == true");
        TEST_CHECK(parsed.borrowed, "borrowed bit survives pack->wire->parse");
        TEST_CHECK(parsed.borrowed_zone_index == 2u, "borrowed_zone_index (2) survives pack->wire->parse");
    }

    // Negative proof this isn't vacuous: peer NOT known to support V2 at all
    // (the safe pre-ANNOUNCE_VERSION default) -- must stay 23 bytes even if
    // both v2 and v3 support flags were (incorrectly) passed true, mirroring
    // the same defense-in-depth link_frame_pack_status() already applies via
    // the peer_supports_status_v2 outer gate.
    {
        uint8_t payload[LINK_FRAME_STATUS_LEN_V3];
        size_t len = link_frame_pack_status(payload, false, false, false, true, 1.0f, 2.0f, 0,
                                             0.0f, 0.0f, 0.0f, false, false,
                                             /*peer_supports_status_v2=*/false, /*tx_dropped_sat=*/0u,
                                             /*peer_supports_status_v3=*/true, /*is_borrowed=*/true,
                                             /*borrowed_zone_index=*/0u, /*cj_valid=*/false);
        TEST_CHECK(len == LINK_FRAME_STATUS_LEN_V1,
                   "peer_supports_status_v2=false wins even when peer_supports_status_v3=true is "
                   "(wrongly) also passed -- V3 can never be emitted without V2");
    }
}

// 2026-09-08 (safety_tc_warn_mask_disagreement audit): LINK_FLAG2_CJ_VALID
// (flags2 bit 1) is the actual point of this task -- proving cj_valid rides
// the wire INDEPENDENTLY of temp_valid, which is exactly the distinction
// ("chip alive, probe dead" vs "chip itself not converting") that was
// unavailable before this bit existed. Same shape as
// test_status_frame_v3_borrowed() above, reusing the same spare byte.
static void test_status_frame_v3_cj_valid(void)
{
    TEST_SECTION("status frame -- V3 CJ_VALID flag (flags2 bit 1), independent of temp_valid");

    // THE distinguishing case: temp_valid=false (probe/CR1-verify fault) but
    // cj_valid=true with a real, finite cj_c -- must NOT collapse to
    // "everything NaN" on the wire the way it did before this fix.
    {
        uint8_t payload[LINK_FRAME_STATUS_LEN_V3];
        size_t len = link_frame_pack_status(payload, false, false, false, /*temp_valid=*/false,
                                             NAN, /*cj_c=*/23.5f, 0, 0.0f, 0.0f, 0.0f, false, false,
                                             /*peer_supports_status_v2=*/true, /*tx_dropped_sat=*/0u,
                                             /*peer_supports_status_v3=*/true, /*is_borrowed=*/false,
                                             LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN,
                                             /*cj_valid=*/true);
        TEST_CHECK(len == LINK_FRAME_STATUS_LEN_V3, "V3 length returned");
        TEST_CHECK((payload[24] & MIRROR_SAFETY_FLAG2_CJ_VALID) != 0u,
                   "flags2 bit 1 (CJ_VALID) is set when cj_valid=true even though temp_valid=false");

        uint8_t wire_payload[LINK_FRAME_STATUS_LEN_V3];
        uint8_t wire_length = 0;
        TEST_CHECK(wire_round_trip(payload, (uint8_t)len, wire_payload, &wire_length),
                   "V3 status frame survives the real kilnlink wire round trip");

        mirror_status_t parsed = mirror_apply_status(wire_payload, wire_length);
        TEST_CHECK(parsed.ok, "V3-length frame parses after a real wire round trip");
        TEST_CHECK(parsed.cj_valid_known, "mirror reports cj_valid_known == true for a V3 frame");
        TEST_CHECK(parsed.cj_valid, "cj_valid bit survives pack->wire->parse");
        TEST_CHECK_NEAR(parsed.cj_temp_c, 23.5f, 0.0001,
                        "cj_c itself is a real, finite number on the wire -- the exact "
                        "probe-fault-vs-chip-dead distinction this bit exists to carry");
        TEST_CHECK((parsed.flags & MIRROR_SAFETY_FLAG_TEMP_VALID) == 0,
                   "TEMP_VALID stays clear -- cj_valid is independent, not a substitute");
    }

    // Both invalid: temp_valid=false AND cj_valid=false -- the genuine
    // "chip not converting" case must still read that way.
    {
        uint8_t payload[LINK_FRAME_STATUS_LEN_V3];
        size_t len = link_frame_pack_status(payload, false, false, false, /*temp_valid=*/false,
                                             NAN, NAN, 0, 0.0f, 0.0f, 0.0f, false, false,
                                             /*peer_supports_status_v2=*/true, /*tx_dropped_sat=*/0u,
                                             /*peer_supports_status_v3=*/true, /*is_borrowed=*/false,
                                             LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN,
                                             /*cj_valid=*/false);
        TEST_CHECK(len == LINK_FRAME_STATUS_LEN_V3, "V3 length returned");
        TEST_CHECK((payload[24] & MIRROR_SAFETY_FLAG2_CJ_VALID) == 0u,
                   "flags2 bit 1 (CJ_VALID) is clear when cj_valid=false");

        mirror_status_t parsed = mirror_apply_status(payload, (uint8_t)len);
        TEST_CHECK(parsed.ok, "V3-length frame parses");
        TEST_CHECK(!parsed.cj_valid, "cj_valid bit is clear -- both TC and CJ genuinely invalid");
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

// --- TODO.md Phase 7b.8: exhaustive mutual-compatibility matrix ------------
// link_frame_versions_compatible() (src/tasks/link_frame.c) implements
// LINK_PROTOCOL.md section 4's formula exactly:
//   compatible == peer.protocol >= self.min_compatible
//              && self.protocol >= peer.min_compatible
// This is the ONE function that decides whether the Pico trusts anything the
// ESP's ANNOUNCE_VERSION says, so it is tested two ways below: (1) an
// explicit, named table covering every older/newer/equal relationship on
// both sides plus the pathological peer, and (2) a brute-force sweep over a
// small integer range against an INDEPENDENT reference formula written here
// (not copy-pasted from link_frame.c), so a bug in the real implementation
// has something other than itself to disagree with.

// Independent reimplementation of LINK_PROTOCOL.md section 4's formula --
// deliberately not calling into link_frame_versions_compatible() or sharing
// any code with it, so this is a real second opinion, not an echo.
static bool reference_versions_compatible(uint16_t self_protocol, uint16_t self_min_compatible,
                                           uint16_t peer_protocol, uint16_t peer_min_compatible)
{
    bool peer_can_read_self = (peer_protocol >= self_min_compatible);
    bool self_can_read_peer = (self_protocol >= peer_min_compatible);
    return peer_can_read_self && self_can_read_peer;
}

typedef struct {
    const char *label;
    uint16_t self_protocol;
    uint16_t self_min_compatible;
    uint16_t peer_protocol;
    uint16_t peer_min_compatible;
    bool expect_compatible;
} version_case_t;

static void test_version_compatibility_named_matrix(void)
{
    TEST_SECTION("ANNOUNCE_VERSION -- named older/newer/equal matrix (TODO.md 7b.8)");

    // Self is fixed at a well-formed "current build": protocol 5, willing to
    // talk down to protocol 4 (min_compatible 4). Peer is varied across every
    // older/newer/equal relationship, on both the protocol axis and the
    // min_compatible axis, including peer combinations no honest build could
    // ever produce.
    const version_case_t cases[] = {
        // -- peer.protocol vs self: older / equal / newer, peer well-formed --
        {"peer older protocol (4), peer well-formed (min=4): compatible (self talks down to 4)",
         5, 4, 4, 4, true},
        {"peer equal protocol (5), peer well-formed (min=5): compatible",
         5, 4, 5, 5, true},
        {"peer newer protocol (6), peer well-formed (min=5): compatible (self.protocol(5)>=peer.min(5))",
         5, 4, 6, 5, true},
        {"peer newer protocol (6), peer requires min=6: incompatible (self.protocol(5) < peer.min(6))",
         5, 4, 6, 6, false},

        // -- peer.protocol older than self.min_compatible: self can't read peer's era --
        {"peer protocol (3) older than self.min_compatible (4): incompatible (self dropped support)",
         5, 4, 3, 3, false},
        {"peer protocol exactly at self.min_compatible (4): compatible (boundary, inclusive)",
         5, 4, 4, 3, true},
        {"peer protocol one below self.min_compatible (3 vs min 4): incompatible (boundary, exclusive)",
         5, 4, 3, 4, false},

        // -- self.protocol vs peer.min_compatible: peer dropped support for self --
        {"peer.min_compatible exactly equals self.protocol (5): compatible (boundary, inclusive)",
         5, 4, 5, 5, true},
        {"peer.min_compatible one above self.protocol (6 vs 5): incompatible (boundary, exclusive)",
         5, 4, 5, 6, false},

        // -- both sides identical (the common case) --
        {"both sides identical build (5,5) vs (5,5): compatible",
         5, 5, 5, 5, true},

        // -- self older than peer, mirrored relationship --
        {"self older (protocol=4,min=4) than a newer peer requiring min=5: incompatible",
         4, 4, 6, 5, false},
        {"self older (protocol=4,min=4) than a newer peer requiring min=4: compatible",
         4, 4, 6, 4, true},

        // -- THE pathological case: peer announces min_compatible ABOVE ITS
        // OWN protocol version. This is a malformed/hostile announcement --
        // no honest build ever sets min_compatible > its own protocol_version
        // (kilnlink_version.h's own invariant) -- and the local side must not
        // be talked into anything by it. Even though peer.protocol (5) alone
        // would satisfy self.min_compatible (4), and self.protocol (5) is
        // not old enough for what the peer hostilely claims to require (9),
        // the formula must still refuse: self.protocol(5) >= peer.min(9) is
        // false, so this correctly evaluates to incompatible regardless of
        // the peer's own internal nonsense.
        {"PATHOLOGICAL: peer.min_compatible (9) > peer.protocol (5) itself: incompatible, not trusted",
         5, 4, 5, 9, false},
        // A second pathological shape: pathological peer whose own claimed
        // protocol is otherwise perfectly compatible on every other axis --
        // proves the hostile min_compatible field alone is what defeats it,
        // not some other mismatched field.
        {"PATHOLOGICAL: peer.protocol==self.protocol==5 but peer.min_compatible=100: incompatible",
         5, 5, 5, 100, false},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const version_case_t *c = &cases[i];
        bool actual = link_frame_versions_compatible(c->self_protocol, c->self_min_compatible,
                                                       c->peer_protocol, c->peer_min_compatible);
        TEST_CHECK(actual == c->expect_compatible, c->label);
    }
}

static void test_version_compatibility_exhaustive_sweep(void)
{
    TEST_SECTION("ANNOUNCE_VERSION -- exhaustive brute-force sweep vs independent reference formula "
                 "(TODO.md 7b.8: every combination of older/newer/equal on both sides)");

    // Every combination of protocol/min_compatible for both self and peer
    // over a small integer range that comfortably covers older (<), equal
    // (==), and newer (>) on all four pairwise relationships the formula
    // cares about (peer.protocol vs self.min_compatible, self.protocol vs
    // peer.min_compatible), plus values above/below the "sensible" 3..7
     // window on both ends. Includes every pathological shape where
    // min_compatible > protocol on either side, not just the peer.
    const uint16_t values[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    const size_t n = sizeof(values) / sizeof(values[0]);

    int compared = 0;
    bool all_agree = true;
    for (size_t a = 0; a < n; a++) {
        for (size_t b = 0; b < n; b++) {
            for (size_t c = 0; c < n; c++) {
                for (size_t d = 0; d < n; d++) {
                    uint16_t self_protocol = values[a];
                    uint16_t self_min = values[b];
                    uint16_t peer_protocol = values[c];
                    uint16_t peer_min = values[d];

                    bool actual = link_frame_versions_compatible(self_protocol, self_min,
                                                                   peer_protocol, peer_min);
                    bool expected = reference_versions_compatible(self_protocol, self_min,
                                                                    peer_protocol, peer_min);
                    compared++;
                    if (actual != expected) {
                        all_agree = false;
                    }
                }
            }
        }
    }

    TEST_CHECK(compared == (int)(n * n * n * n),
               "exhaustive sweep actually covered every (self_protocol, self_min, peer_protocol, "
               "peer_min) combination in the grid");
    TEST_CHECK(all_agree,
               "link_frame_versions_compatible() agrees with an independently-written reference "
               "formula across all 9^4 combinations, including every pathological "
               "min_compatible > protocol_version shape on either side");
}

// --- TODO.md Phase 7b.8 (continued): telemetry keeps flowing on a mismatch --
// LINK_PROTOCOL.md section 4: "keeps sending telemetry, including its own
// version, so the ESP can display the mismatch and so an update can be
// pushed to fix it." link_frame_pack_status()/link_frame_pack_fw_version()
// (the actual telemetry packers, exercised above) take no compatibility/
// degraded-mode argument at all and have no notion of link_task's
// s_degraded_no_context flag -- there is structurally nothing in their
// signature for a mismatch to gate. This test proves that emptily-true
// structural fact is also true operationally: telemetry packs and survives
// the real wire round trip identically regardless of what the compatibility
// verdict for the current peer would be.
static void test_telemetry_keeps_flowing_on_version_mismatch(void)
{
    TEST_SECTION("telemetry keeps flowing on a version mismatch (LINK_PROTOCOL.md sec 4)");

    // A peer announcement that is definitely incompatible (self is protocol
    // 5/min 5; this peer's own protocol is 3, below self's floor).
    bool mismatch_compatible =
        link_frame_versions_compatible(/*self_protocol=*/5, /*self_min_compatible=*/5,
                                        /*peer_protocol=*/3, /*peer_min_compatible=*/3);
    TEST_CHECK(!mismatch_compatible, "sanity: the scenario below is a real version mismatch");

    // Build and wire-round-trip a status telemetry frame exactly as
    // test_status_frame_round_trip() does above -- nothing here consults
    // mismatch_compatible, by construction: link_frame_pack_status() gained
    // a peer_supports_status_v2 parameter (2026-08-23) since this comment
    // was first written, but that parameter is driven by link_task.c's
    // separately-cached peer protocol_version, never by
    // link_frame_versions_compatible()'s min_compatible-range verdict --
    // passed false here, matching this test's whole point: even a
    // definitely-incompatible peer still gets a valid (V1) telemetry frame,
    // not a withheld or malformed one.
    uint8_t payload[LINK_FRAME_STATUS_LEN_V1];
    link_frame_pack_status(payload, /*estop=*/false, /*relay_energized=*/false,
                            /*heating_enabled=*/false, /*temp_valid=*/true, 500.0f, 22.0f, 0, 0.1f,
                            0.2f, 0.3f, false, false, /*peer_supports_status_v2=*/false, 0, false, false, LINK_FRAME_STATUS_BORROWED_ZONE_UNKNOWN, /*cj_valid=*/false);
    uint8_t wire_payload[LINK_FRAME_STATUS_LEN_V1];
    uint8_t wire_length = 0;
    TEST_CHECK(wire_round_trip(payload, LINK_FRAME_STATUS_LEN_V1, wire_payload, &wire_length),
               "status telemetry (Frame A) still packs and survives the wire round trip "
               "during a version mismatch -- no guard exists to silence it");
    mirror_status_t parsed = mirror_apply_status(wire_payload, wire_length);
    TEST_CHECK(parsed.ok, "status telemetry still parses correctly during a version mismatch");
    TEST_CHECK_NEAR(parsed.tc_temp_c, 500.0f, 0.0001,
                     "status telemetry content is unaffected by the mismatch (no silent degradation "
                     "of the frame itself, only of the context-consuming guards elsewhere)");

    // Same for the FW_VERSION frame itself -- LINK_PROTOCOL.md is explicit
    // that the version frame is part of the compatibility FLOOR and must be
    // emitted "regardless of whether the two sides agree on anything else".
    const char *commit = "deadbee";
    uint8_t fwv[64];
    size_t fwv_len = link_frame_pack_fw_version(fwv, sizeof(fwv), /*protocol=*/5, /*min_compat=*/5,
                                                  /*dirty=*/0, commit, (uint8_t)strlen(commit), "",
                                                  0, /*boot_id=*/3, /*config_version=*/1,
                                                  /*config_crc=*/0x1234u);
    TEST_CHECK(fwv_len > 0, "FW_VERSION (compatibility-floor frame) still packs during a mismatch");
    uint8_t fwv_wire[64];
    uint8_t fwv_wire_len = 0;
    TEST_CHECK(wire_round_trip(fwv, (uint8_t)fwv_len, fwv_wire, &fwv_wire_len),
               "FW_VERSION frame still survives the wire round trip during a mismatch");
}

void run_test_link_frame_wire(void)
{
    test_status_frame_round_trip();
    test_status_frame_nan_when_invalid();
    test_status_frame_negative();
    test_status_v2_supported_gate();
    test_status_v3_supported_gate();
    test_rollback_result_supported_gate();
    test_saturate_tx_dropped();
    test_status_frame_v2_tx_dropped();
    test_status_frame_v3_borrowed();
    test_status_frame_v3_cj_valid();
    test_fw_version_round_trip();
    test_fw_version_negative();
    test_version_compatibility_named_matrix();
    test_version_compatibility_exhaustive_sweep();
    test_telemetry_keeps_flowing_on_version_mismatch();
}
