// Host tests for link_frame.c's link_frame_unpack_context() -- TODO.md
// Phase 7, CommonFW/docs/LINK_PROTOCOL.md section 4.
//
// Style follows test_safety_guards.c: untrusted-wire input, so the hostile
// cases (short length, over-max zone_count, length/zone_count mismatches,
// NULL pointers) get as much attention as the happy path, and each hostile
// case additionally proves `*out` was left completely untouched.
#include <math.h>
#include <string.h>

#include "test_common.h"
#include "../src/tasks/link_frame.h"
#include "../src/snapshots.h"

static void pack_u32_le(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
    out[2] = (uint8_t)((v >> 16) & 0xFFu);
    out[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void pack_f32_le(uint8_t *out, float f)
{
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    pack_u32_le(out, bits);
}

// Fills a header-only (zone_count==0) 15-byte frame with distinct,
// recognizable values, then lets the caller append zone blocks.
static uint8_t build_header(uint8_t *buf, uint8_t flags, uint8_t boot_id, uint32_t seq,
                             uint32_t uptime_ms, uint8_t relay_now_mask, uint8_t relay_recent_mask,
                             uint8_t recent_window_s, uint8_t zone_count)
{
    buf[0] = LINK_FRAME_PUSH_CONTEXT_CMD;
    buf[1] = flags;
    buf[2] = boot_id;
    pack_u32_le(&buf[3], seq);
    pack_u32_le(&buf[7], uptime_ms);
    buf[11] = relay_now_mask;
    buf[12] = relay_recent_mask;
    buf[13] = recent_window_s;
    buf[14] = zone_count;
    return LINK_FRAME_CONTEXT_MIN_LEN;
}

static void build_zone(uint8_t *buf, uint8_t zone_index, uint8_t flags, float setpoint_c,
                        float measured_c, uint8_t sample_counter, uint8_t tc_type, uint8_t tc_fault)
{
    buf[0] = zone_index;
    buf[1] = flags;
    pack_f32_le(&buf[2], setpoint_c);
    pack_f32_le(&buf[6], measured_c);
    buf[10] = sample_counter;
    buf[11] = tc_type;
    buf[12] = tc_fault;
    buf[13] = 0xFFu; /* reserved -- must be ignored, so poison it */
}

static context_snapshot_t sentinel_snapshot(void)
{
    context_snapshot_t out;
    memset(&out, 0xAA, sizeof(out));
    out.timestamp_ms = 0x11223344u; /* a known value, to prove it is truly untouched */
    return out;
}

static void assert_untouched(const context_snapshot_t *out, const char *what)
{
    context_snapshot_t expected = sentinel_snapshot();
    TEST_CHECK(memcmp(out, &expected, sizeof(expected)) == 0, what);
}

static void test_zero_zones(void)
{
    TEST_SECTION("link_frame_unpack_context -- 0 zones (header only)");

    uint8_t buf[LINK_FRAME_CONTEXT_MIN_LEN];
    uint8_t len = build_header(buf, CONTEXT_FLAG_PROFILE_RUNNING | CONTEXT_FLAG_HEAT_REQUESTED, 7,
                                0x01020304u, 0x0A0B0C0Du, 0x05u, 0x0Au, 200u, 0);
    TEST_CHECK(len == LINK_FRAME_CONTEXT_MIN_LEN, "sanity: header length is 15");

    context_snapshot_t out = sentinel_snapshot();
    uint32_t saved_timestamp = out.timestamp_ms;
    bool ok = link_frame_unpack_context(buf, len, &out);

    TEST_CHECK(ok, "0-zone frame unpacks successfully");
    TEST_CHECK(out.valid, "out->valid set true");
    TEST_CHECK(out.flags == (CONTEXT_FLAG_PROFILE_RUNNING | CONTEXT_FLAG_HEAT_REQUESTED), "flags round-trip");
    TEST_CHECK(out.boot_id == 7, "boot_id round-trips");
    TEST_CHECK(out.seq == 0x01020304u, "seq round-trips");
    TEST_CHECK(out.uptime_ms == 0x0A0B0C0Du, "uptime_ms round-trips");
    TEST_CHECK(out.relay_now_mask == 0x05u, "relay_now_mask round-trips");
    TEST_CHECK(out.relay_recent_mask == 0x0Au, "relay_recent_mask round-trips");
    TEST_CHECK(out.recent_window_s == 200u, "recent_window_s round-trips");
    TEST_CHECK(out.zone_count == 0, "zone_count round-trips as 0");
    TEST_CHECK(out.timestamp_ms == saved_timestamp, "timestamp_ms left untouched, even on success");
}

static void test_three_zones(void)
{
    TEST_SECTION("link_frame_unpack_context -- 3 zones, distinct values per field");

    uint8_t buf[LINK_FRAME_CONTEXT_MIN_LEN + 3 * LINK_FRAME_CONTEXT_ZONE_LEN];
    uint8_t off = build_header(buf, CONTEXT_FLAG_CONTEXT_VALID | CONTEXT_FLAG_SIM_PLANT, 3, 999u,
                                123456u, 0x0Bu, 0x0Fu, 180u, 3);
    build_zone(&buf[off], 0, CONTEXT_ZONE_FLAG_MEASURED_VALID | CONTEXT_ZONE_FLAG_ACTIVE, 100.5f,
               99.25f, 11, 1, 0x00u);
    build_zone(&buf[off + LINK_FRAME_CONTEXT_ZONE_LEN], 1,
               CONTEXT_ZONE_FLAG_RELAY_ON | CONTEXT_ZONE_FLAG_GUARD_TRIPPED, 500.0f, -12.75f, 222,
               2, 0x03u);
    build_zone(&buf[off + 2 * LINK_FRAME_CONTEXT_ZONE_LEN], 2, CONTEXT_ZONE_FLAG_MEASURED_VALID,
               1200.25f, 1199.125f, 42, 4, 0x81u);
    uint8_t len = off + 3 * LINK_FRAME_CONTEXT_ZONE_LEN;
    TEST_CHECK(len == LINK_FRAME_CONTEXT_MIN_LEN + 3 * LINK_FRAME_CONTEXT_ZONE_LEN, "sanity: 57 bytes at 3 zones");

    context_snapshot_t out = sentinel_snapshot();
    bool ok = link_frame_unpack_context(buf, len, &out);

    TEST_CHECK(ok, "3-zone frame unpacks successfully");
    TEST_CHECK(out.valid, "out->valid set true");
    TEST_CHECK(out.zone_count == 3, "zone_count round-trips as 3");

    TEST_CHECK(out.zones[0].zone_index == 0, "zone0 zone_index");
    TEST_CHECK(out.zones[0].flags == (CONTEXT_ZONE_FLAG_MEASURED_VALID | CONTEXT_ZONE_FLAG_ACTIVE), "zone0 flags");
    TEST_CHECK_NEAR(out.zones[0].setpoint_c, 100.5f, 0.0001, "zone0 setpoint_c");
    TEST_CHECK_NEAR(out.zones[0].measured_c, 99.25f, 0.0001, "zone0 measured_c");
    TEST_CHECK(out.zones[0].sample_counter == 11, "zone0 sample_counter");
    TEST_CHECK(out.zones[0].tc_type == 1, "zone0 tc_type");
    TEST_CHECK(out.zones[0].tc_fault == 0x00u, "zone0 tc_fault");

    TEST_CHECK(out.zones[1].zone_index == 1, "zone1 zone_index");
    TEST_CHECK(out.zones[1].flags == (CONTEXT_ZONE_FLAG_RELAY_ON | CONTEXT_ZONE_FLAG_GUARD_TRIPPED), "zone1 flags");
    TEST_CHECK_NEAR(out.zones[1].setpoint_c, 500.0f, 0.0001, "zone1 setpoint_c");
    TEST_CHECK_NEAR(out.zones[1].measured_c, -12.75f, 0.0001, "zone1 measured_c");
    TEST_CHECK(out.zones[1].sample_counter == 222, "zone1 sample_counter");
    TEST_CHECK(out.zones[1].tc_type == 2, "zone1 tc_type");
    TEST_CHECK(out.zones[1].tc_fault == 0x03u, "zone1 tc_fault");

    TEST_CHECK(out.zones[2].zone_index == 2, "zone2 zone_index");
    TEST_CHECK(out.zones[2].flags == CONTEXT_ZONE_FLAG_MEASURED_VALID, "zone2 flags");
    TEST_CHECK_NEAR(out.zones[2].setpoint_c, 1200.25f, 0.0001, "zone2 setpoint_c");
    TEST_CHECK_NEAR(out.zones[2].measured_c, 1199.125f, 0.0001, "zone2 measured_c");
    TEST_CHECK(out.zones[2].sample_counter == 42, "zone2 sample_counter");
    TEST_CHECK(out.zones[2].tc_type == 4, "zone2 tc_type");
    TEST_CHECK(out.zones[2].tc_fault == 0x81u, "zone2 tc_fault");
}

static void test_hostile_inputs(void)
{
    TEST_SECTION("link_frame_unpack_context -- hostile / malformed inputs");

    /* length == 0 */
    {
        uint8_t buf[1] = {0};
        context_snapshot_t out = sentinel_snapshot();
        bool ok = link_frame_unpack_context(buf, 0, &out);
        TEST_CHECK(!ok, "length==0 rejected");
        assert_untouched(&out, "length==0 leaves *out untouched");
    }

    /* length == 14, one short of the minimum header */
    {
        uint8_t buf[14];
        memset(buf, 0, sizeof(buf));
        buf[0] = LINK_FRAME_PUSH_CONTEXT_CMD;
        context_snapshot_t out = sentinel_snapshot();
        bool ok = link_frame_unpack_context(buf, 14, &out);
        TEST_CHECK(!ok, "length==14 (one short of MIN_LEN) rejected");
        assert_untouched(&out, "length==14 leaves *out untouched");
    }

    /* zone_count == 4 (one over max) with a length "consistent" for 4 zones
     * -- must be rejected on the zone_count check itself, not accidentally
     * accepted because the length happens to match. */
    {
        uint8_t buf[LINK_FRAME_CONTEXT_MIN_LEN + 4 * LINK_FRAME_CONTEXT_ZONE_LEN];
        memset(buf, 0, sizeof(buf));
        uint8_t off = build_header(buf, 0, 0, 0, 0, 0, 0, 0, 4);
        for (int i = 0; i < 4; i++) {
            build_zone(&buf[off + i * LINK_FRAME_CONTEXT_ZONE_LEN], (uint8_t)i, 0, 0.0f, 0.0f, 0, 0, 0);
        }
        uint8_t len = off + 4 * LINK_FRAME_CONTEXT_ZONE_LEN;
        context_snapshot_t out = sentinel_snapshot();
        bool ok = link_frame_unpack_context(buf, len, &out);
        TEST_CHECK(!ok, "zone_count==4 (over CONTEXT_SNAPSHOT_MAX_ZONES) rejected even with a matching length");
        assert_untouched(&out, "zone_count==4 leaves *out untouched");
    }

    /* zone_count == 1 but length == 15 -- claims a zone, provides no bytes for it */
    {
        uint8_t buf[LINK_FRAME_CONTEXT_MIN_LEN];
        (void)build_header(buf, 0, 0, 0, 0, 0, 0, 0, 1);
        context_snapshot_t out = sentinel_snapshot();
        bool ok = link_frame_unpack_context(buf, LINK_FRAME_CONTEXT_MIN_LEN, &out);
        TEST_CHECK(!ok, "zone_count==1 with length==15 (no zone bytes) rejected");
        assert_untouched(&out, "zone_count==1/length==15 leaves *out untouched");
    }

    /* zone_count == 2 but length one byte short of what 2 zones need */
    {
        uint8_t buf[LINK_FRAME_CONTEXT_MIN_LEN + 2 * LINK_FRAME_CONTEXT_ZONE_LEN];
        memset(buf, 0, sizeof(buf));
        uint8_t off = build_header(buf, 0, 0, 0, 0, 0, 0, 0, 2);
        build_zone(&buf[off], 0, 0, 0.0f, 0.0f, 0, 0, 0);
        build_zone(&buf[off + LINK_FRAME_CONTEXT_ZONE_LEN], 1, 0, 0.0f, 0.0f, 0, 0, 0);
        uint8_t full_len = off + 2 * LINK_FRAME_CONTEXT_ZONE_LEN;
        context_snapshot_t out = sentinel_snapshot();
        bool ok = link_frame_unpack_context(buf, (uint8_t)(full_len - 1), &out);
        TEST_CHECK(!ok, "zone_count==2 with length one byte short rejected");
        assert_untouched(&out, "zone_count==2/short length leaves *out untouched");
    }

    /* zone_count == 2 but length one byte of trailing garbage past 2 well-formed zones */
    {
        uint8_t buf[LINK_FRAME_CONTEXT_MIN_LEN + 2 * LINK_FRAME_CONTEXT_ZONE_LEN + 1];
        memset(buf, 0, sizeof(buf));
        uint8_t off = build_header(buf, 0, 0, 0, 0, 0, 0, 0, 2);
        build_zone(&buf[off], 0, 0, 0.0f, 0.0f, 0, 0, 0);
        build_zone(&buf[off + LINK_FRAME_CONTEXT_ZONE_LEN], 1, 0, 0.0f, 0.0f, 0, 0, 0);
        uint8_t full_len = off + 2 * LINK_FRAME_CONTEXT_ZONE_LEN;
        buf[full_len] = 0xFFu; /* trailing garbage */
        context_snapshot_t out = sentinel_snapshot();
        bool ok = link_frame_unpack_context(buf, (uint8_t)(full_len + 1), &out);
        TEST_CHECK(!ok, "zone_count==2 with one byte of trailing garbage rejected");
        assert_untouched(&out, "zone_count==2/trailing garbage leaves *out untouched");
    }

    /* payload == NULL */
    {
        context_snapshot_t out = sentinel_snapshot();
        bool ok = link_frame_unpack_context(NULL, LINK_FRAME_CONTEXT_MIN_LEN, &out);
        TEST_CHECK(!ok, "payload==NULL rejected");
        assert_untouched(&out, "payload==NULL leaves *out untouched");
    }

    /* out == NULL -- must not crash, must simply return false */
    {
        uint8_t buf[LINK_FRAME_CONTEXT_MIN_LEN];
        (void)build_header(buf, 0, 0, 0, 0, 0, 0, 0, 0);
        bool ok = link_frame_unpack_context(buf, LINK_FRAME_CONTEXT_MIN_LEN, NULL);
        TEST_CHECK(!ok, "out==NULL rejected");
    }
}

static void test_nan_survives(void)
{
    TEST_SECTION("link_frame_unpack_context -- NaN measured_c survives the wire");

    uint8_t buf[LINK_FRAME_CONTEXT_MIN_LEN + LINK_FRAME_CONTEXT_ZONE_LEN];
    uint8_t off = build_header(buf, 0, 0, 0, 0, 0, 0, 0, 1);
    build_zone(&buf[off], 0, CONTEXT_ZONE_FLAG_MEASURED_VALID, 55.0f, 0.0f, 1, 1, 0);
    /* Overwrite measured_c's bytes (offset 6..9 of the zone block) with a
     * standard little-endian IEEE-754 quiet NaN by hand -- not via any
     * pack_* helper from link_frame.c, to keep this test independent of
     * that module's own packing code. */
    buf[off + 6] = 0x00u;
    buf[off + 7] = 0x00u;
    buf[off + 8] = 0xC0u;
    buf[off + 9] = 0x7Fu;
    uint8_t len = off + LINK_FRAME_CONTEXT_ZONE_LEN;

    context_snapshot_t out = sentinel_snapshot();
    bool ok = link_frame_unpack_context(buf, len, &out);

    TEST_CHECK(ok, "frame with a NaN measured_c still unpacks (validation is length/count only)");
    TEST_CHECK(isnan(out.zones[0].measured_c), "measured_c NaN survives the wire, not coerced to 0");
    TEST_CHECK_NEAR(out.zones[0].setpoint_c, 55.0f, 0.0001, "setpoint_c in the same zone is unaffected by the NaN");
}

// Host tests for link_frame_versions_compatible() -- TODO.md Phase 7b, item
// 7b.8: "every combination of older/newer/equal on both sides, including a
// peer that announces a min_compatible above its own version." Uses
// distinct, realistic version numbers (not 0/1) so an off-by-one or a
// swapped operand would actually be caught.
//
// This is a read-only sanity pass over the existing implementation
// (link_frame.c:188-192) -- deliberately not touched by this test file.
static void test_versions_compatible(void)
{
    TEST_SECTION("link_frame_versions_compatible -- combination matrix");

    /* Equal versions both sides. */
    TEST_CHECK(link_frame_versions_compatible(5, 3, 5, 3),
                "equal protocol and min_compatible on both sides -> compatible");

    /* Peer newer than self, but still within self's floor, and self within
     * peer's floor -- the "newer peer, still backward compatible" case. */
    TEST_CHECK(link_frame_versions_compatible(5, 3, 6, 4),
                "peer newer (6) than self (5), both floors satisfied -> compatible");

    /* Peer older than self, but still >= self's min_compatible, and self's
     * protocol >= peer's min_compatible. */
    TEST_CHECK(link_frame_versions_compatible(6, 4, 5, 3),
                "peer older (5) than self (6), both floors satisfied -> compatible");

    /* Self has raised its own min_compatible above what an older peer
     * offers: peer.protocol (4) < self.min_compatible (5) -> self refuses. */
    TEST_CHECK(!link_frame_versions_compatible(6, 5, 4, 3),
                "self min_compatible (5) above peer protocol (4) -> NOT compatible");

    /* Peer has raised ITS min_compatible above self's protocol: self.protocol
     * (4) < peer.min_compatible (5) -> peer would refuse self. */
    TEST_CHECK(!link_frame_versions_compatible(4, 3, 6, 5),
                "peer min_compatible (5) above self protocol (4) -> NOT compatible");

    /* Peer claims a min_compatible above its own protocol (a self-inconsistent
     * / malformed peer claim: peer_min_compatible > peer_protocol). The
     * function should still evaluate the plain formula, not special-case it.
     * peer_protocol=4, peer_min_compatible=7 (peer claims to require a floor
     * newer than itself). Against self_protocol=6 >= peer_min_compatible(7)?
     * No -- 6 < 7, so self.protocol >= peer_min_compatible fails ->
     * NOT compatible, confirming the formula is applied literally. */
    TEST_CHECK(!link_frame_versions_compatible(6, 3, 4, 7),
                "peer_min_compatible (7) > peer_protocol (4), self (6) still below that "
                "floor -> NOT compatible (formula applied literally, no special-casing)");

    /* Same malformed-peer shape, but push self_protocol above the peer's
     * nonsensical floor to confirm the *other* half of the formula
     * (peer.protocol >= self.min_compatible) is what actually gates it here:
     * self_protocol=8 >= peer_min_compatible=7 passes, but peer_protocol=4 <
     * self_min_compatible=5 still fails on the first half. */
    TEST_CHECK(!link_frame_versions_compatible(8, 5, 4, 7),
                "malformed peer (min_compatible 7 > own protocol 4), self.min_compatible (5) "
                "above peer.protocol (4) -> NOT compatible on the other half of the formula");

    /* Self below peer's min_compatible (peer requires newer than self has). */
    TEST_CHECK(!link_frame_versions_compatible(3, 3, 5, 4),
                "self.protocol (3) < peer.min_compatible (4) -> NOT compatible");

    /* Both directions simultaneously fail. */
    TEST_CHECK(!link_frame_versions_compatible(4, 6, 3, 8),
                "self too old for peer's floor AND peer too old for self's floor -> NOT compatible");

    /* Degenerate/boundary case: everyone at floor exactly -- >= is inclusive
     * both ways per the formula. */
    TEST_CHECK(link_frame_versions_compatible(5, 5, 5, 5),
                "self_protocol==self_min_compatible==peer_protocol==peer_min_compatible -> compatible");
}

// Host tests for link_frame_trip_mask_for_reason() -- factored out of
// link_task_send_diag()'s single-bit degraded trip_mask approximation so
// link_task_handle_clear_trip()'s mismatch check (link_task.c) uses the
// exact same mapping DIAG frames report. TODO.md Phase 7's CLEAR_TRIP item.
static void test_trip_mask_for_reason(void)
{
    TEST_SECTION("link_frame_trip_mask_for_reason -- single-bit mapping");

    TEST_CHECK(link_frame_trip_mask_for_reason(SAFETY_TRIP_NONE) == 0u,
                "SAFETY_TRIP_NONE -> mask 0 (nothing latched)");
    TEST_CHECK(link_frame_trip_mask_for_reason(SAFETY_TRIP_OVERTEMP) == 0x0001u,
                "SAFETY_TRIP_OVERTEMP (1) -> bit 0");
    TEST_CHECK(link_frame_trip_mask_for_reason(SAFETY_TRIP_OVER_SETPOINT) == 0x0002u,
                "SAFETY_TRIP_OVER_SETPOINT (2) -> bit 1");
    TEST_CHECK(link_frame_trip_mask_for_reason(SAFETY_TRIP_ESTOP) == (1u << 7),
                "SAFETY_TRIP_ESTOP (8) -> bit 7");
    TEST_CHECK(link_frame_trip_mask_for_reason(SAFETY_TRIP_BORROWED_STALE) == (1u << 13),
                "SAFETY_TRIP_BORROWED_STALE (14) -> bit 13, still within a u16");
}

void run_test_link_frame(void)
{
    test_zero_zones();
    test_three_zones();
    test_hostile_inputs();
    test_nan_survives();
    test_versions_compatible();
    test_trip_mask_for_reason();
}
