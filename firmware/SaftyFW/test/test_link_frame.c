// Host tests for link_frame.c's link_frame_unpack_context() -- TODO.md
// Phase 7, CommonFW/docs/LINK_PROTOCOL.md section 4.
//
// Style follows test_safety_guards.c: untrusted-wire input, so the hostile
// cases (short length, over-max zone_count, length/zone_count mismatches,
// NULL pointers) get as much attention as the happy path, and each hostile
// case additionally proves `*out` was left completely untouched.
#include <math.h>
#include <stdlib.h>
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

// kilnlink audit 2026-10-09 L1: only an ESP -> Pico frame refreshes S6b's
// liveness; the Pico's own frames looped back (src SAFETY, dst ESP) must not.
static void test_counts_for_liveness(void)
{
    TEST_SECTION("link_frame_counts_for_liveness -- only ESP->Pico frames refresh S6b (audit L1)");
    TEST_CHECK(link_frame_counts_for_liveness(LINK_FRAME_DEVICE_ESP, LINK_FRAME_DEVICE_SAFETY),
               "ESP (0) -> SAFETY (2): counts");
    TEST_CHECK(!link_frame_counts_for_liveness(LINK_FRAME_DEVICE_SAFETY, LINK_FRAME_DEVICE_ESP),
               "our own frame echoed back (SAFETY -> ESP): does NOT count");
    TEST_CHECK(!link_frame_counts_for_liveness(LINK_FRAME_DEVICE_SAFETY, LINK_FRAME_DEVICE_SAFETY),
               "SAFETY -> SAFETY: does NOT count");
    TEST_CHECK(!link_frame_counts_for_liveness(LINK_FRAME_DEVICE_ESP, LINK_FRAME_DEVICE_ESP),
               "ESP -> ESP (not addressed to us): does NOT count");
    TEST_CHECK(!link_frame_counts_for_liveness(1u, LINK_FRAME_DEVICE_SAFETY),
               "HOST (1) -> SAFETY: does NOT count (only the ESP is our peer)");
    TEST_CHECK(LINK_FRAME_DEVICE_ESP == 0u && LINK_FRAME_DEVICE_SAFETY == 2u,
               "device ids match the frozen wire contract (LINK_PROTOCOL.md section 3)");
}

static const char *LINK_TASK_CANDIDATES_L1[] = {
    "../src/tasks/link_task.c",
    "src/tasks/link_task.c",
    "firmware/SaftyFW/src/tasks/link_task.c",
};

// Wiring half of audit L1: the pure predicate above proves nothing unless
// link_task_handle_raw_frame() actually gates the S6b liveness refresh on it.
static void test_liveness_refresh_is_gated_in_link_task(void)
{
    TEST_SECTION("link_task.c gates the S6b liveness refresh on link_frame_counts_for_liveness() (audit L1)");
    char *text = test_read_source_anchored(__FILE__, LINK_TASK_CANDIDATES_L1[0], LINK_TASK_CANDIDATES_L1,
                                           sizeof(LINK_TASK_CANDIDATES_L1) / sizeof(LINK_TASK_CANDIDATES_L1[0]));
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/link_task.c -- update the candidate paths");
        return;
    }
    const char *refresh = strstr(text, "s_last_valid_frame_tick = xTaskGetTickCount();");
    TEST_CHECK(refresh != NULL, "the liveness refresh statement exists (sanity: the scan is not chasing a rename)");
    if (refresh) {
        TEST_CHECK(strstr(refresh + 1, "s_last_valid_frame_tick = xTaskGetTickCount();") == NULL,
                   "exactly one liveness refresh site");
        // The gate must sit in the ~600 bytes immediately before the refresh,
        // i.e. in the same function, with an early return.
        const char *window = (refresh - text > 600) ? refresh - 600 : text;
        const char *gate = strstr(window, "if (!link_frame_counts_for_liveness(frame.src_device, frame.dst_device)) {");
        TEST_CHECK(gate != NULL && gate < refresh,
                   "link_frame_counts_for_liveness() gate immediately precedes the refresh");
        if (gate != NULL && gate < refresh) {
            const char *ret = strstr(gate, "return;");
            TEST_CHECK(ret != NULL && ret < refresh, "the gate returns before refreshing liveness");
        }
    }
    free(text);
}

// Host tests for link_frame_decide_clear_trip() -- extracted from
// link_task_handle_clear_trip() (this session's guard-test-matrix pass,
// commit 9a6d3e9 flagged this as a gap; the refuse checks themselves were
// added in 62ce6bf). Covers both refusal paths, the accept path, and the
// boundary where a real trip's mask happens to be 0 (impossible by
// construction, but proves the ACCEPT path isn't reachable through a
// mask-of-zero coincidence).
static void test_decide_clear_trip(void)
{
    TEST_SECTION("link_frame_decide_clear_trip -- accept/refuse decision");

    TEST_CHECK(link_frame_decide_clear_trip(SAFETY_TRIP_NONE, 0u) ==
                   LINK_CLEAR_TRIP_REFUSE_NOTHING_TRIPPED,
               "nothing tripped, wire mask 0 -> REFUSE_NOTHING_TRIPPED");
    TEST_CHECK(link_frame_decide_clear_trip(SAFETY_TRIP_NONE, 0x0001u) ==
                   LINK_CLEAR_TRIP_REFUSE_NOTHING_TRIPPED,
               "nothing tripped even with a nonzero wire mask -> still REFUSE_NOTHING_TRIPPED "
               "(checked before the mask comparison)");

    TEST_CHECK(link_frame_decide_clear_trip(SAFETY_TRIP_OVERTEMP, 0x0002u) ==
                   LINK_CLEAR_TRIP_REFUSE_MASK_MISMATCH,
               "tripped OVERTEMP (mask 0x0001), wire sends 0x0002 -> REFUSE_MASK_MISMATCH");
    TEST_CHECK(link_frame_decide_clear_trip(SAFETY_TRIP_OVERTEMP, 0u) ==
                   LINK_CLEAR_TRIP_REFUSE_MASK_MISMATCH,
               "tripped OVERTEMP, wire sends 0 (stale clear queued before this trip) -> "
               "REFUSE_MASK_MISMATCH");

    TEST_CHECK(link_frame_decide_clear_trip(SAFETY_TRIP_OVERTEMP, 0x0001u) == LINK_CLEAR_TRIP_ACCEPT,
               "tripped OVERTEMP, wire mask matches exactly -> ACCEPT");
    TEST_CHECK(link_frame_decide_clear_trip(SAFETY_TRIP_ESTOP, (uint16_t)(1u << 7)) ==
                   LINK_CLEAR_TRIP_ACCEPT,
               "tripped ESTOP, wire mask matches exactly -> ACCEPT");

    /* Audit 2026-08-27: SAFETY_TRIP_INEFFECTIVE (S9, welded contactor) is
     * unclearable -- ARCHITECTURE.md section 9 / SAFETY_MODEL.md section 4's
     * "no exit except power removal at the breaker" -- and that refusal has
     * to be visible at the wire layer with its own reason code, not just
     * fail silently once safety_core_request_clear_trip() is reached. Checked
     * even with a wire_trip_mask that matches S9's own mask exactly, proving
     * this is not reachable through the ACCEPT path by any mask coincidence. */
    TEST_CHECK(link_frame_decide_clear_trip(SAFETY_TRIP_INEFFECTIVE,
                                             link_frame_trip_mask_for_reason(SAFETY_TRIP_INEFFECTIVE)) ==
                   LINK_CLEAR_TRIP_REFUSE_INEFFECTIVE,
               "tripped INEFFECTIVE (S9), wire mask matches exactly -> still REFUSE_INEFFECTIVE, "
               "never ACCEPT");
    TEST_CHECK(link_frame_decide_clear_trip(SAFETY_TRIP_INEFFECTIVE, 0u) ==
                   LINK_CLEAR_TRIP_REFUSE_INEFFECTIVE,
               "tripped INEFFECTIVE (S9), wire mask 0 -> REFUSE_INEFFECTIVE, not REFUSE_MASK_MISMATCH "
               "(checked before the mask comparison, same ordering as NOTHING_TRIPPED)");
}

// --- link_frame_ceiling_is_active (SET_FIRING_CEILING bounds check) --------
// SAFETY_MODEL.md section 4, S1: 0/NaN means "no firing", and a hostile or
// garbled negative value must be rejected outright (safety_guards.c's own
// min() clamp only defends against a firing_max_c that is too HIGH -- see
// this function's own header comment in link_frame.h for why negative sails
// straight through that clamp otherwise).
static void test_ceiling_is_active(void)
{
    TEST_SECTION("link_frame_ceiling_is_active -- SET_FIRING_CEILING bounds check");

    TEST_CHECK(link_frame_ceiling_is_active(900.0f), "a normal positive value is active");
    TEST_CHECK(link_frame_ceiling_is_active(0.0001f), "a tiny positive value is still active");

    TEST_CHECK(!link_frame_ceiling_is_active(0.0f), "0.0 -- wire convention for 'no firing' -- not active");
    TEST_CHECK(!link_frame_ceiling_is_active(-1.0f), "negative -- rejected, not silently clamped by the caller");
    TEST_CHECK(!link_frame_ceiling_is_active(-900.0f),
               "a large negative value is rejected the same as a small one");
    TEST_CHECK(!link_frame_ceiling_is_active((float)NAN), "NaN -- wire convention for 'no firing' -- not active");
    TEST_CHECK(!link_frame_ceiling_is_active((float)INFINITY), "+Infinity is rejected, not active");
    TEST_CHECK(!link_frame_ceiling_is_active(-(float)INFINITY), "-Infinity is rejected, not active");
}

// --- link_frame_clock_epoch_is_plausible (SET_CLOCK bounds check) ---------
static void test_clock_epoch_plausible(void)
{
    TEST_SECTION("link_frame_clock_epoch_is_plausible -- SET_CLOCK bounds check");

    TEST_CHECK(link_frame_clock_epoch_is_plausible(1700000000000ULL),
               "a real-world 2023-ish epoch is plausible");
    TEST_CHECK(link_frame_clock_epoch_is_plausible(1577836800000ULL),
               "the lower bound (2020-01-01) itself is plausible (inclusive)");
    TEST_CHECK(link_frame_clock_epoch_is_plausible(4102444800000ULL),
               "the upper bound (2100-01-01) itself is plausible (inclusive)");

    TEST_CHECK(!link_frame_clock_epoch_is_plausible(0ULL), "epoch 0 (1970) is rejected");
    TEST_CHECK(!link_frame_clock_epoch_is_plausible(1577836799999ULL),
               "one millisecond below the lower bound is rejected");
    TEST_CHECK(!link_frame_clock_epoch_is_plausible(4102444800001ULL),
               "one millisecond above the upper bound is rejected");
    TEST_CHECK(!link_frame_clock_epoch_is_plausible(UINT64_MAX),
               "a garbled/maxed-out u64 is rejected");
}

// --- link_frame_apply_set_config / link_frame_apply_set_ct_cal -------------
// Regression coverage for the 2026-08-24 fix: link_task_handle_set_config()
// and link_task_handle_set_ct_cal() used to build the record they wrote from
// config_store_default() (or, for SET_CT_CAL, individual getters layered on
// top of config_store_default()), so a one-field wire command silently reset
// every OTHER commissioned field, threshold, and (for SET_CONFIG) every
// ct_cal channel to compiled defaults on every write -- including the
// automatic SET_CONFIG resend KilnFW's safety_link.c fires on every link
// reconnect. link_frame_apply_set_config()/link_frame_apply_set_ct_cal()
// (link_frame.h/.c) are the pure record-mutation step extracted so this is
// host-testable without stubbing flash -- same "extraction for the test
// matrix" reasoning as link_frame_decide_clear_trip() above.
//
// Builds a FULLY POPULATED, non-default committed record -- every
// fields_set bit set, every section 2-5 threshold at a distinctive
// non-default value, all three ct_cal channels calibrated with distinctive
// gain/offset, safety_tc_installed explicitly at the "not installed" (0)
// marker -- so "survives byte-for-byte" actually proves something: a bug
// that resets to config_store_default() would be invisible against a
// committed record that already equalled the defaults.
static void build_fully_populated_committed_record(config_store_record_t *out)
{
    config_store_default(out); // baseline for format_version/seq/reserved[]/
                                // anything this test doesn't override below

    out->fields_set = (uint16_t)(CONFIG_STORE_SET_TC_SOURCE | CONFIG_STORE_SET_BORROWED_ZONE_INDEX |
                                  CONFIG_STORE_SET_TC_PLACEMENT_MODE | CONFIG_STORE_SET_ABS_MAX_TEMP_C |
                                  CONFIG_STORE_SET_CT_CHANNEL_MAP | CONFIG_STORE_SET_MAX_RATE_C_PER_MIN |
                                  CONFIG_STORE_SET_MAINS_VOLTAGE_V | CONFIG_STORE_SET_CT_CHANNEL_MAP_0 |
                                  CONFIG_STORE_SET_CT_CHANNEL_MAP_1 | CONFIG_STORE_SET_CT_CHANNEL_MAP_2);

    out->tc_source = CONFIG_STORE_TC_SOURCE_BORROWED_ZONE;
    out->borrowed_zone_index = 2;
    out->tc_placement_mode = CONFIG_STORE_TC_PLACEMENT_EXTERNAL_OVERHEAT;
    out->abs_max_temp_c = 1310.5f;
    out->tc_type = 3u; // a real, non-default MAX31856_TC_TYPE_* value
    out->ct_channel_map[0] = 0;
    out->ct_channel_map[1] = 1;
    out->ct_channel_map[2] = 2;
    out->safety_tc_installed = 0u; // deliberately marked "not installed" --
                                    // must never revert to its default of 1
    out->calibration_missing = false; // deliberately CLEARED -- proves a
                                       // real commissioning pass, so an
                                       // unwarranted re-arm is visible

    out->firing_margin_c = 111.0f;
    out->overshoot_margin_c = 66.0f;
    out->overshoot_time_s = 121u;
    out->max_rate_c_per_min = 12.5f;
    out->rate_window_s = 61u;
    out->blind_grace_s = 62u;
    out->frozen_window_s = 601u;
    out->tc_disagreement_c = 199.0f;
    out->tc_disagreement_time_s = 301u;
    out->tc_expected_offset_c = 3.5f;
    out->cj_warn_c = 59.0f;
    out->cj_max_c = 84.0f;
    out->cj_time_s = 61u;
    out->borrowed_stale_s = 11u;
    out->borrowed_stale_trip_s = 61u;
    out->borrowed_type_expected = 0x05u;

    out->i_present_a = 2.5f;
    out->zero_counts[0] = 1000;
    out->zero_counts[1] = 2000;
    out->zero_counts[2] = 3000;
    out->correlation_window_s = 151u;
    out->stuck_on_time_s = 21u;
    out->trip_verify_s = 11u;
    out->k_ct_v_per_a[0] = 0.1f;
    out->k_ct_v_per_a[1] = 0.2f;
    out->k_ct_v_per_a[2] = 0.3f;
    out->gain[0] = 0.700f;
    out->gain[1] = 0.710f;
    out->gain[2] = 0.720f;
    out->mains_voltage_v = 240.0f;
    out->power_window_s = 121u;

    out->context_max_age_s = 6u;
    out->link_timeout_s = 11u;
    out->link_dead_hard_s = 121u;
    out->mainfault_debounce_ms = 201u;
    out->telemetry_period_ms = 501u;

    out->startup_grace_s = 61u;
    out->estop_debounce_ms = 51u;
    out->watchdog_timeout_ms = 1001u;
    out->config_check_period_s = 11u;

    for (size_t i = 0; i < CONFIG_STORE_CT_CAL_NUM_CHANNELS; i++) {
        out->ct_cal[i].calibrated = true;
        out->ct_cal[i].gain = 1.0f + (float)i * 0.1f;
        out->ct_cal[i].offset = 0.01f * (float)i;
    }
}

// SET_CONFIG, tc_type actually CHANGES: only tc_type and calibration_missing
// may differ from the committed record; every other byte -- every threshold,
// every fields_set bit, every ct_cal channel, safety_tc_installed -- must
// survive untouched.
static void test_apply_set_config_changed_type_preserves_rest(void)
{
    TEST_SECTION("link_frame_apply_set_config -- tc_type CHANGES: only tc_type + "
                 "calibration_missing move, everything else survives byte-for-byte");

    config_store_record_t committed;
    build_fully_populated_committed_record(&committed);
    TEST_CHECK(committed.tc_type != 5u, "test setup: new tc_type below must actually differ");

    config_store_record_t out;
    memset(&out, 0xAA, sizeof(out)); // poison -- a field this function forgets to
                                      // write would otherwise show up as 0, not
                                      // as an obviously-wrong pattern
    link_frame_apply_set_config(&committed, 5u, &out);

    TEST_CHECK(out.tc_type == 5u, "tc_type takes the new wire value");
    TEST_CHECK(out.calibration_missing == true,
               "a REAL tc_type change re-arms calibration_missing, per config_store.h's "
               "own doc comment on that field");

    // Byte-for-byte on everything else: overwrite the two fields this
    // command is ALLOWED to change on both sides so memcmp only sees
    // unintended drift.
    config_store_record_t committed_norm = committed;
    config_store_record_t out_norm = out;
    committed_norm.tc_type = 0;
    out_norm.tc_type = 0;
    committed_norm.calibration_missing = false;
    out_norm.calibration_missing = false;
    TEST_CHECK(memcmp(&committed_norm, &out_norm, sizeof(committed_norm)) == 0,
               "every field other than tc_type/calibration_missing survives byte-for-byte "
               "(this is the regression check for the 2026-08-24 whole-record-reset bug)");

    // Spot-check the fields the original bug actually destroyed, named
    // individually so a failure here points straight at what broke.
    TEST_CHECK(out.fields_set == committed.fields_set, "fields_set (commissioning bits) unchanged");
    TEST_CHECK(out.abs_max_temp_c == committed.abs_max_temp_c, "abs_max_temp_c unchanged");
    TEST_CHECK(out.firing_margin_c == committed.firing_margin_c, "firing_margin_c (S1) unchanged");
    TEST_CHECK(out.blind_grace_s == committed.blind_grace_s, "blind_grace_s (S5) unchanged");
    TEST_CHECK(out.safety_tc_installed == committed.safety_tc_installed,
               "safety_tc_installed stays 0 (not reverted to its default of 1)");
    for (size_t i = 0; i < CONFIG_STORE_CT_CAL_NUM_CHANNELS; i++) {
        TEST_CHECK(out.ct_cal[i].calibrated == committed.ct_cal[i].calibrated &&
                       out.ct_cal[i].gain == committed.ct_cal[i].gain &&
                       out.ct_cal[i].offset == committed.ct_cal[i].offset,
                   "ct_cal channel unchanged");
    }
}

// SET_CONFIG, tc_type is UNCHANGED (an idempotent resend -- exactly what
// KilnFW's automatic link-reconnect resend is when nobody re-commissioned in
// between): calibration_missing must NOT be re-armed, whatever its committed
// value was, and nothing else may move either.
static void test_apply_set_config_idempotent_resend_does_not_rearm(void)
{
    TEST_SECTION("link_frame_apply_set_config -- tc_type UNCHANGED (idempotent resend): "
                 "calibration_missing is left exactly as committed, never re-armed");

    config_store_record_t committed;
    build_fully_populated_committed_record(&committed);
    committed.calibration_missing = false; // a real commissioning pass already cleared it

    config_store_record_t out;
    memset(&out, 0xAA, sizeof(out));
    link_frame_apply_set_config(&committed, committed.tc_type, &out);

    TEST_CHECK(out.tc_type == committed.tc_type, "tc_type unchanged");
    TEST_CHECK(out.calibration_missing == false,
               "calibration_missing stays false -- an unchanged tc_type must not distrust a "
               "calibration a real type change never touched");
    TEST_CHECK(memcmp(&committed, &out, sizeof(committed)) == 0,
               "the whole record is untouched when both fields this command could move "
               "are already at their committed values");

    // Same resend, but the committed record already had calibration_missing
    // == true (e.g. a genuine change happened earlier and nothing has
    // cleared it yet, or it was never commissioned): a resend of the SAME
    // tc_type must not clear it either -- this function only ever sets it
    // true on a change, never resets it to false.
    committed.calibration_missing = true;
    memset(&out, 0xAA, sizeof(out));
    link_frame_apply_set_config(&committed, committed.tc_type, &out);
    TEST_CHECK(out.calibration_missing == true,
               "calibration_missing stays true on an idempotent resend too -- this function "
               "never clears it, only conditionally sets it");
    TEST_CHECK(memcmp(&committed, &out, sizeof(committed)) == 0, "record otherwise untouched");
}

// SET_CT_CAL: only the named channel's calibrated/gain/offset may differ;
// the other two channels and every non-ct_cal field, including tc_type and
// calibration_missing, must survive byte-for-byte.
static void test_apply_set_ct_cal_preserves_other_channels_and_rest(void)
{
    TEST_SECTION("link_frame_apply_set_ct_cal -- only ct_cal[channel] moves, "
                 "everything else (including the OTHER two channels) survives byte-for-byte");

    config_store_record_t committed;
    build_fully_populated_committed_record(&committed);

    config_store_record_t out;
    memset(&out, 0xAA, sizeof(out));
    link_frame_apply_set_ct_cal(&committed, 1u, true, 9.5f, -3.25f, &out);

    TEST_CHECK(out.ct_cal[1].calibrated == true && out.ct_cal[1].gain == 9.5f &&
                   out.ct_cal[1].offset == -3.25f,
               "channel 1 takes the new wire values");
    TEST_CHECK(out.ct_cal[0].calibrated == committed.ct_cal[0].calibrated &&
                   out.ct_cal[0].gain == committed.ct_cal[0].gain &&
                   out.ct_cal[0].offset == committed.ct_cal[0].offset,
               "channel 0 is untouched by a channel-1 write");
    TEST_CHECK(out.ct_cal[2].calibrated == committed.ct_cal[2].calibrated &&
                   out.ct_cal[2].gain == committed.ct_cal[2].gain &&
                   out.ct_cal[2].offset == committed.ct_cal[2].offset,
               "channel 2 is untouched by a channel-1 write");

    config_store_record_t committed_norm = committed;
    config_store_record_t out_norm = out;
    memset(&committed_norm.ct_cal[1], 0, sizeof(committed_norm.ct_cal[1]));
    memset(&out_norm.ct_cal[1], 0, sizeof(out_norm.ct_cal[1]));
    TEST_CHECK(memcmp(&committed_norm, &out_norm, sizeof(committed_norm)) == 0,
               "every field outside ct_cal[1] survives byte-for-byte -- tc_type, "
               "calibration_missing, every threshold, every fields_set bit, "
               "safety_tc_installed, and the other two channels (this is the regression "
               "check for the 2026-08-24 whole-record-reset bug)");

    TEST_CHECK(out.tc_type == committed.tc_type, "tc_type unchanged (a separate commissioning "
                                                    "concern SET_CT_CAL has no business touching)");
    TEST_CHECK(out.calibration_missing == committed.calibration_missing,
               "calibration_missing unchanged");
    TEST_CHECK(out.safety_tc_installed == committed.safety_tc_installed, "safety_tc_installed unchanged");
}

// Out-of-range channel: second line of defense behind link_task.c's own
// refusal -- must be a no-op (*out == *committed), never an out-of-bounds
// write into ct_cal[].
static void test_apply_set_ct_cal_out_of_range_is_noop(void)
{
    TEST_SECTION("link_frame_apply_set_ct_cal -- out-of-range channel is a no-op, "
                 "not an out-of-bounds write");

    config_store_record_t committed;
    build_fully_populated_committed_record(&committed);

    config_store_record_t out;
    memset(&out, 0x55, sizeof(out)); // distinct poison from build_fully_populated's own
                                      // values, so "unchanged from poison" would be caught
    link_frame_apply_set_ct_cal(&committed, CONFIG_STORE_CT_CAL_NUM_CHANNELS, true, 1.0f, 0.0f, &out);

    TEST_CHECK(memcmp(&out, &out, sizeof(out)) == 0, "sanity: memcmp against self is always 0");
    uint8_t poison[sizeof(out)];
    memset(poison, 0x55, sizeof(poison));
    TEST_CHECK(memcmp(&out, poison, sizeof(out)) == 0,
               "out-of-range channel leaves *out completely untouched (still the poison "
               "pattern), never partially written");
}

void run_test_link_frame(void)
{
    test_zero_zones();
    test_three_zones();
    test_hostile_inputs();
    test_nan_survives();
    test_versions_compatible();
    test_trip_mask_for_reason();
    test_decide_clear_trip();
    test_counts_for_liveness();
    test_liveness_refresh_is_gated_in_link_task();
    test_ceiling_is_active();
    test_clock_epoch_plausible();
    test_apply_set_config_changed_type_preserves_rest();
    test_apply_set_config_idempotent_resend_does_not_rearm();
    test_apply_set_ct_cal_preserves_other_channels_and_rest();
    test_apply_set_ct_cal_out_of_range_is_noop();
}
