// test_config_store_zone_ct_channel.c -- docs/CT_CHANNEL_MASK_PLAN.md step 2.
//
// Step 2 is "purely additive storage, verifiable by round-tripping every
// existing config-store host test unmodified plus new ones for the migration
// and the derivation". test_config_store.c supplies the first half (it is
// unchanged apart from widening one local to match fields_set's new u32
// type); this file supplies the second: the v2-to-v3 migration branch in
// config_store_unpack_ex(), and every derivation/collapse helper the later
// steps will read the new field through.
//
// Nothing here reads a guard or a sweep -- at step 2 nothing consumes
// zone_ct_channel yet. What is proven is that the field survives a write/read
// round trip, that an already-committed v2 record keeps loading with its
// values intact and an honest derived map, and that each helper collapses
// exactly onto the legacy topology behaviour for both shipped topologies.
#include <stdbool.h>
#include <string.h>

#include "test_common.h"

#include "config_params.h"
#include "config_store.h"
#include "crc32.h" // bootloader/ -- to re-seal a hand-edited record

// The frozen v2 and current v3 byte offsets, spelled out here rather than
// shared with config_store.c's private REC_OFF_* table on purpose: a test
// that imported the implementation's own constants would keep passing if
// those constants moved, which is exactly the failure a frozen layout table
// exists to prevent. These numbers are the wire format, and if an edit to
// config_store.c changes them this file must fail.
#define V_OFF_FORMAT_VERSION 4u
#define V_OFF_FIELDS_SET     12u
#define V2_OFF_TC_SOURCE     14u
#define V2_OFF_RESERVED      235u
#define V3_OFF_TC_SOURCE     16u
#define V3_OFF_ZONE_CT_CHAN  237u
#define V3_OFF_RESERVED      240u
#define V_OFF_CRC            504u

static void reseal(uint8_t rec[CONFIG_STORE_RECORD_LEN])
{
    uint32_t crc = bootloader_crc32(rec, V_OFF_CRC);
    rec[V_OFF_CRC + 0u] = (uint8_t)(crc & 0xFFu);
    rec[V_OFF_CRC + 1u] = (uint8_t)((crc >> 8) & 0xFFu);
    rec[V_OFF_CRC + 2u] = (uint8_t)((crc >> 16) & 0xFFu);
    rec[V_OFF_CRC + 3u] = (uint8_t)((crc >> 24) & 0xFFu);
}

// Rewrites a packed v3 record into the v2 bytes the shipped v2 firmware
// would actually have written for the same configuration: fields_set
// truncated to its u16, the whole field block two bytes lower, no
// zone_ct_channel at all, reserved starting at 235, and a fresh CRC over the
// same [0, 504) region.
static void v3_bytes_to_v2(const uint8_t v3[CONFIG_STORE_RECORD_LEN],
                           uint8_t v2[CONFIG_STORE_RECORD_LEN])
{
    memset(v2, 0xFF, CONFIG_STORE_RECORD_LEN);
    memcpy(v2, v3, V_OFF_FIELDS_SET);
    v2[V_OFF_FORMAT_VERSION + 0u] = (uint8_t)CONFIG_STORE_FORMAT_VERSION_V2;
    v2[V_OFF_FORMAT_VERSION + 1u] = 0u;
    v2[V_OFF_FIELDS_SET + 0u] = v3[V_OFF_FIELDS_SET + 0u];
    v2[V_OFF_FIELDS_SET + 1u] = v3[V_OFF_FIELDS_SET + 1u];
    memcpy(&v2[V2_OFF_TC_SOURCE], &v3[V3_OFF_TC_SOURCE], V2_OFF_RESERVED - V2_OFF_TC_SOURCE);
    memcpy(&v2[V2_OFF_RESERVED], &v3[V3_OFF_RESERVED], V_OFF_CRC - V2_OFF_RESERVED);
    reseal(v2);
}

static void base_record(config_store_record_t *rec, uint8_t ct_topology)
{
    config_store_default(rec);
    rec->seq = 11u;
    rec->tc_type = 0x03u;
    rec->abs_max_temp_c = 1285.0f;
    rec->ct_topology = ct_topology;
    rec->overcurrent_pct = 137u;
    rec->estop_active_level = 1u;
    rec->calibration_missing = false;
    config_store_derive_zone_ct_channel(ct_topology, rec->zone_ct_channel);
}

static void test_derive(void)
{
    TEST_SECTION("config_store_derive_zone_ct_channel: legacy topology byte -> zone map");

    uint8_t map[3];
    config_store_derive_zone_ct_channel(CONFIG_STORE_CT_TOPOLOGY_PER_ZONE, map);
    TEST_CHECK(map[0] == 0u && map[1] == 1u && map[2] == 2u,
               "PER_ZONE derives the identity map {0,1,2}");

    config_store_derive_zone_ct_channel(CONFIG_STORE_CT_TOPOLOGY_SUMMED, map);
    TEST_CHECK(map[0] == 2u && map[1] == 2u && map[2] == 2u,
               "SUMMED derives {2,2,2} -- every zone on the one shared CT");

    // Structural backstop only (unpack range-checks the byte to 0/1 first),
    // but it must point at the pooled map, never at three independent
    // per-channel thresholds.
    config_store_derive_zone_ct_channel(7u, map);
    TEST_CHECK(map[0] == 2u && map[1] == 2u && map[2] == 2u,
               "an unrecognised topology byte derives the conservative {2,2,2}, not the identity");
}

static void test_topology_collapse(void)
{
    TEST_SECTION("config_store_ct_topology_from_zone_ct_channel: zone map -> legacy byte "
                 "(what a downgraded board reads)");

    const uint8_t per_zone[3] = { 0u, 1u, 2u };
    const uint8_t summed[3] = { 2u, 2u, 2u };
    const uint8_t split[3] = { 0u, 0u, 1u };
    const uint8_t permuted[3] = { 2u, 1u, 0u };

    TEST_CHECK(config_store_ct_topology_from_zone_ct_channel(per_zone) ==
                   CONFIG_STORE_CT_TOPOLOGY_PER_ZONE,
               "identity map collapses to PER_ZONE");
    TEST_CHECK(config_store_ct_topology_from_zone_ct_channel(summed) ==
                   CONFIG_STORE_CT_TOPOLOGY_SUMMED,
               "{2,2,2} collapses to SUMMED");
    TEST_CHECK(config_store_ct_topology_from_zone_ct_channel(split) ==
                   CONFIG_STORE_CT_TOPOLOGY_SUMMED,
               "a genuine two-CT split collapses to SUMMED (blunt, never mis-scaled)");
    TEST_CHECK(config_store_ct_topology_from_zone_ct_channel(permuted) ==
                   CONFIG_STORE_CT_TOPOLOGY_SUMMED,
               "a permuted one-CT-per-zone map is NOT PER_ZONE: old firmware would read the "
               "wrong channel for each zone, so it must see the pooled label instead");
}

static void test_member_count(void)
{
    TEST_SECTION("config_store_ct_channel_member_count: |member(ch)|");

    const uint8_t per_zone[3] = { 0u, 1u, 2u };
    const uint8_t summed[3] = { 2u, 2u, 2u };
    const uint8_t split[3] = { 0u, 0u, 1u };

    for (uint8_t ch = 0; ch < 3u; ch++) {
        TEST_CHECK(config_store_ct_channel_member_count(per_zone, ch) == 1u,
                   "PER_ZONE: every channel has exactly one member zone");
    }

    TEST_CHECK(config_store_ct_channel_member_count(summed, 0u) == 0u,
               "SUMMED: channel 0 has no member zone");
    TEST_CHECK(config_store_ct_channel_member_count(summed, 1u) == 0u,
               "SUMMED: channel 1 has no member zone");
    TEST_CHECK(config_store_ct_channel_member_count(summed, 2u) == 3u,
               "SUMMED: channel 2 is shared by all three zones");

    TEST_CHECK(config_store_ct_channel_member_count(split, 0u) == 2u,
               "split {0,0,1}: channel 0 is shared by two zones (S15's >= 2 gate)");
    TEST_CHECK(config_store_ct_channel_member_count(split, 1u) == 1u,
               "split {0,0,1}: channel 1 has one member zone");
    TEST_CHECK(config_store_ct_channel_member_count(split, 2u) == 0u,
               "split {0,0,1}: channel 2 is unwired");
}

static void test_fitted_map_collapses_onto_legacy_predicate(void)
{
    TEST_SECTION("config_store_ct_channel_fitted_map: collapses onto "
                 "config_store_ct_channel_fitted() for both shipped topologies");

    const uint8_t per_zone[3] = { 0u, 1u, 2u };
    const uint8_t summed[3] = { 2u, 2u, 2u };
    const uint8_t split[3] = { 0u, 0u, 1u };

    for (uint8_t ch = 0; ch < 3u; ch++) {
        TEST_CHECK(config_store_ct_channel_fitted_map(ch, true, per_zone) ==
                       config_store_ct_channel_fitted(ch, true,
                                                      CONFIG_STORE_CT_TOPOLOGY_PER_ZONE),
                   "identity map agrees with the PER_ZONE predicate on every channel");
        TEST_CHECK(config_store_ct_channel_fitted_map(ch, true, summed) ==
                       config_store_ct_channel_fitted(ch, true,
                                                      CONFIG_STORE_CT_TOPOLOGY_SUMMED),
                   "{2,2,2} agrees with the SUMMED predicate on every channel");
        TEST_CHECK(!config_store_ct_channel_fitted_map(ch, false, per_zone),
                   "ct_installed_effective == false masks every channel, map notwithstanding");
        TEST_CHECK(!config_store_ct_channel_fitted_map(ch, false, summed),
                   "ct_installed_effective == false masks every channel in SUMMED too");
    }

    TEST_CHECK(config_store_ct_channel_fitted_map(0u, true, split),
               "split {0,0,1}: channel 0 is fitted");
    TEST_CHECK(config_store_ct_channel_fitted_map(1u, true, split),
               "split {0,0,1}: channel 1 is fitted");
    TEST_CHECK(!config_store_ct_channel_fitted_map(2u, true, split),
               "split {0,0,1}: channel 2 is NOT fitted -- no zone claims it, so it can never "
               "contribute a false presence");
}

static void test_effective_map_honours_the_group_bit(void)
{
    TEST_SECTION("config_store_effective_zone_ct_channel: the stored bytes are only "
                 "trusted once CONFIG_STORE_SET_ZONE_CT_CHANNEL is set");

    config_store_record_t rec;
    base_record(&rec, CONFIG_STORE_CT_TOPOLOGY_SUMMED);
    // Deliberately inconsistent raw bytes: a record whose group bit is clear
    // must ignore them entirely and derive from ct_topology instead.
    rec.zone_ct_channel[0] = 0u;
    rec.zone_ct_channel[1] = 0u;
    rec.zone_ct_channel[2] = 1u;
    rec.fields_set &= ~(uint32_t)CONFIG_STORE_SET_ZONE_CT_CHANNEL;

    uint8_t map[3];
    config_store_effective_zone_ct_channel(&rec, map);
    TEST_CHECK(map[0] == 2u && map[1] == 2u && map[2] == 2u,
               "bit clear: the map is derived from ct_topology (SUMMED), not read from the bytes");

    rec.fields_set |= CONFIG_STORE_SET_ZONE_CT_CHANNEL;
    config_store_effective_zone_ct_channel(&rec, map);
    TEST_CHECK(map[0] == 0u && map[1] == 0u && map[2] == 1u,
               "bit set: the stored split map is returned verbatim");

    config_store_effective_zone_ct_channel(NULL, map);
    TEST_CHECK(map[0] == 0u && map[1] == 1u && map[2] == 2u,
               "NULL record falls back to the PER_ZONE identity rather than reading memory");
}

static void test_round_trip(void)
{
    TEST_SECTION("zone_ct_channel survives pack/unpack unchanged");

    config_store_record_t rec;
    base_record(&rec, CONFIG_STORE_CT_TOPOLOGY_SUMMED);
    rec.zone_ct_channel[0] = 0u;
    rec.zone_ct_channel[1] = 0u;
    rec.zone_ct_channel[2] = 1u;
    rec.fields_set |= (uint32_t)(CONFIG_STORE_SET_ZONE_CT_CHANNEL |
                                 CONFIG_STORE_SET_ZONE_CT_CHANNEL_0 |
                                 CONFIG_STORE_SET_ZONE_CT_CHANNEL_1 |
                                 CONFIG_STORE_SET_ZONE_CT_CHANNEL_2);

    uint8_t packed[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&rec, packed);
    TEST_CHECK(packed[V3_OFF_ZONE_CT_CHAN + 0u] == 0u &&
                   packed[V3_OFF_ZONE_CT_CHAN + 1u] == 0u &&
                   packed[V3_OFF_ZONE_CT_CHAN + 2u] == 1u,
               "pack() writes zone_ct_channel at the frozen v3 offset 237");

    config_store_record_t back;
    TEST_CHECK(config_store_unpack(packed, &back), "a v3 record with the new field unpacks");
    TEST_CHECK(back.zone_ct_channel[0] == 0u && back.zone_ct_channel[1] == 0u &&
                   back.zone_ct_channel[2] == 1u,
               "zone_ct_channel round-trips byte for byte");
    TEST_CHECK(config_store_field_is_set(&back.fields_set, CONFIG_STORE_SET_ZONE_CT_CHANNEL),
               "the group bit round-trips -- fields_set is a u32 on the wire since v3");
    TEST_CHECK(back.overcurrent_pct == 137u && back.estop_active_level == 1u,
               "the fields that moved two bytes up round-trip unchanged");
}

static void test_v2_migration(void)
{
    TEST_SECTION("config_store_unpack_ex: a shipped v2 record migrates to v3");

    for (uint8_t topo = 0; topo <= 1u; topo++) {
        config_store_record_t rec;
        base_record(&rec, topo);
        // A v2 record cannot carry the new bits; make sure the migration is
        // not merely copying bits we set ourselves.
        rec.fields_set = (uint32_t)(CONFIG_STORE_SET_TC_TYPE | CONFIG_STORE_SET_ABS_MAX_TEMP_C);

        uint8_t v3[CONFIG_STORE_RECORD_LEN];
        uint8_t v2[CONFIG_STORE_RECORD_LEN];
        config_store_pack(&rec, v3);
        v3_bytes_to_v2(v3, v2);
        TEST_CHECK(v2[V_OFF_FORMAT_VERSION] == CONFIG_STORE_FORMAT_VERSION_V2,
                   "the hand-built legacy record really is stamped format_version 2");

        config_store_record_t back;
        config_store_reject_info_t reject;
        memset(&reject, 0xAA, sizeof(reject));
        TEST_CHECK(config_store_unpack_ex(v2, &back, &reject), "v2 record loads");
        TEST_CHECK(!reject.rejected, "v2 record is not range-rejected");
        TEST_CHECK(back.format_version == CONFIG_STORE_FORMAT_VERSION,
                   "the migrated record reports the current format_version");
        TEST_CHECK(back.tc_type == 0x03u && back.abs_max_temp_c == 1285.0f,
                   "fields below the shift point survive the migration");
        TEST_CHECK(back.overcurrent_pct == 137u && back.estop_active_level == 1u,
                   "fields at the top of the shifted block survive the migration -- this is "
                   "the +2 shift being applied correctly, not an off-by-two");
        TEST_CHECK(back.ct_topology == topo, "ct_topology survives the migration");
        TEST_CHECK(back.fields_set == (uint32_t)(CONFIG_STORE_SET_TC_TYPE |
                                                 CONFIG_STORE_SET_ABS_MAX_TEMP_C),
                   "the v2 u16 fields_set is zero-extended into the v3 u32, no stray high bits");
        TEST_CHECK(!config_store_field_is_set(&back.fields_set, CONFIG_STORE_SET_ZONE_CT_CHANNEL),
                   "the migration never claims the operator answered the new question");

        uint8_t expected[3];
        config_store_derive_zone_ct_channel(topo, expected);
        TEST_CHECK(back.zone_ct_channel[0] == expected[0] &&
                       back.zone_ct_channel[1] == expected[1] &&
                       back.zone_ct_channel[2] == expected[2],
                   "the migrated record carries the map derived from its own ct_topology");
        TEST_CHECK(!back.calibration_missing,
                   "the migration does NOT force calibration_missing -- a board that was "
                   "commissioned under v2 must not be sent back through commissioning by a "
                   "purely additive storage change");
    }
}

static void test_v2_migration_rejects_a_bad_crc(void)
{
    TEST_SECTION("config_store_unpack_ex: a v2 record with a broken CRC is refused, "
                 "not migrated");

    config_store_record_t rec;
    base_record(&rec, CONFIG_STORE_CT_TOPOLOGY_PER_ZONE);
    uint8_t v3[CONFIG_STORE_RECORD_LEN];
    uint8_t v2[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&rec, v3);
    v3_bytes_to_v2(v3, v2);
    v2[V2_OFF_TC_SOURCE] = (uint8_t)(v2[V2_OFF_TC_SOURCE] ^ 0x01u); // flip a bit, leave the CRC stale

    config_store_record_t back;
    TEST_CHECK(!config_store_unpack(v2, &back),
               "a v2 record whose CRC no longer covers its bytes is rejected");
}

static void test_garbled_zone_bytes_normalise(void)
{
    TEST_SECTION("config_store_unpack: an out-of-range zone_ct_channel byte is normalised "
                 "back onto the derived map instead of rejecting the whole record");

    config_store_record_t rec;
    base_record(&rec, CONFIG_STORE_CT_TOPOLOGY_SUMMED);
    rec.fields_set |= (uint32_t)(CONFIG_STORE_SET_ZONE_CT_CHANNEL |
                                 CONFIG_STORE_SET_ZONE_CT_CHANNEL_0 |
                                 CONFIG_STORE_SET_ZONE_CT_CHANNEL_1 |
                                 CONFIG_STORE_SET_ZONE_CT_CHANNEL_2);

    uint8_t packed[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&rec, packed);
    packed[V3_OFF_ZONE_CT_CHAN + 1u] = 9u; // not a channel id
    reseal(packed);

    config_store_record_t back;
    config_store_reject_info_t reject;
    memset(&reject, 0xAA, sizeof(reject));
    TEST_CHECK(config_store_unpack_ex(packed, &back, &reject),
               "the record still loads -- one garbled byte must not cost every other "
               "commissioned value");
    TEST_CHECK(!reject.rejected,
               "and it is not range-rejected: unpack normalises before validation runs");
    TEST_CHECK(!config_store_field_is_set(&back.fields_set, CONFIG_STORE_SET_ZONE_CT_CHANNEL) &&
                   !config_store_field_is_set(&back.fields_set,
                                              CONFIG_STORE_SET_ZONE_CT_CHANNEL_0) &&
                   !config_store_field_is_set(&back.fields_set,
                                              CONFIG_STORE_SET_ZONE_CT_CHANNEL_1) &&
                   !config_store_field_is_set(&back.fields_set,
                                              CONFIG_STORE_SET_ZONE_CT_CHANNEL_2),
               "every zone_ct_channel bit is cleared -- the map is no longer trustworthy");
    TEST_CHECK(back.zone_ct_channel[0] == 2u && back.zone_ct_channel[1] == 2u &&
                   back.zone_ct_channel[2] == 2u,
               "the bytes themselves are normalised onto the ct_topology-derived map");
}

static void test_default_record(void)
{
    TEST_SECTION("config_store_default: the compiled default is the identity map, unset");

    config_store_record_t rec;
    config_store_default(&rec);
    TEST_CHECK(rec.zone_ct_channel[0] == 0u && rec.zone_ct_channel[1] == 1u &&
                   rec.zone_ct_channel[2] == 2u,
               "default zone_ct_channel is {0,1,2}");
    TEST_CHECK(!config_store_field_is_set(&rec.fields_set, CONFIG_STORE_SET_ZONE_CT_CHANNEL),
               "and its bit is clear -- a default is not a commissioned answer");
}

static void test_finalize_requires_all_three(void)
{
    TEST_SECTION("config_params_finalize_zone_ct_channel: two of three zones is not a map");

    config_store_record_t rec;
    config_store_default(&rec);
    rec.fields_set |= (uint32_t)(CONFIG_STORE_SET_ZONE_CT_CHANNEL_0 |
                                 CONFIG_STORE_SET_ZONE_CT_CHANNEL_1);
    config_params_finalize_zone_ct_channel(&rec);
    TEST_CHECK(!config_store_field_is_set(&rec.fields_set, CONFIG_STORE_SET_ZONE_CT_CHANNEL),
               "two of three per-zone bits leaves the group bit clear");

    rec.fields_set |= (uint32_t)CONFIG_STORE_SET_ZONE_CT_CHANNEL_2;
    config_params_finalize_zone_ct_channel(&rec);
    TEST_CHECK(config_store_field_is_set(&rec.fields_set, CONFIG_STORE_SET_ZONE_CT_CHANNEL),
               "all three per-zone bits derive the group bit at COMMIT_CONFIG");

    // Monotonic: a later, incomplete staging round must not un-commission a
    // map an earlier commit already earned.
    rec.fields_set &= ~(uint32_t)CONFIG_STORE_SET_ZONE_CT_CHANNEL_2;
    config_params_finalize_zone_ct_channel(&rec);
    TEST_CHECK(config_store_field_is_set(&rec.fields_set, CONFIG_STORE_SET_ZONE_CT_CHANNEL),
               "an already-set group bit is never cleared by this finalizer");

    config_params_finalize_zone_ct_channel(NULL); // NULL-safe, must not crash
}

static void test_backfill_legacy_topology(void)
{
    TEST_SECTION("config_store_backfill_legacy_ct_topology: legacy maps are byte-for-byte no-ops");

    // PER_ZONE, the identity map: a commissioned zone map that says exactly
    // what ct_topology already said must not change one byte of the record.
    config_store_record_t rec;
    config_store_default(&rec);
    rec.ct_topology = CONFIG_STORE_CT_TOPOLOGY_PER_ZONE;
    rec.zone_ct_channel[0] = 0u;
    rec.zone_ct_channel[1] = 1u;
    rec.zone_ct_channel[2] = 2u;
    rec.fields_set |= (uint32_t)CONFIG_STORE_SET_ZONE_CT_CHANNEL;
    uint8_t before[CONFIG_STORE_RECORD_LEN];
    uint8_t after[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&rec, before);
    config_store_backfill_legacy_ct_topology(&rec);
    config_store_pack(&rec, after);
    TEST_CHECK(rec.ct_topology == CONFIG_STORE_CT_TOPOLOGY_PER_ZONE,
               "identity map keeps ct_topology at PER_ZONE");
    TEST_CHECK(memcmp(before, after, CONFIG_STORE_RECORD_LEN) == 0,
               "PER_ZONE record is byte-for-byte unchanged by the back-fill");

    // SUMMED, the all-channel-2 map: same property.
    config_store_default(&rec);
    rec.ct_topology = CONFIG_STORE_CT_TOPOLOGY_SUMMED;
    rec.zone_ct_channel[0] = 2u;
    rec.zone_ct_channel[1] = 2u;
    rec.zone_ct_channel[2] = 2u;
    rec.fields_set |= (uint32_t)CONFIG_STORE_SET_ZONE_CT_CHANNEL;
    config_store_pack(&rec, before);
    config_store_backfill_legacy_ct_topology(&rec);
    config_store_pack(&rec, after);
    TEST_CHECK(rec.ct_topology == CONFIG_STORE_CT_TOPOLOGY_SUMMED,
               "all-channel-2 map keeps ct_topology at SUMMED");
    TEST_CHECK(memcmp(before, after, CONFIG_STORE_RECORD_LEN) == 0,
               "SUMMED record is byte-for-byte unchanged by the back-fill");
}

static void test_backfill_splits_collapse_to_summed(void)
{
    TEST_SECTION("config_store_backfill_legacy_ct_topology: a genuine split collapses to SUMMED");

    // Every two-CT split shape, plus a permuted three-CT map. None of them is
    // the identity, so all of them must write SUMMED -- PER_ZONE would let
    // downgraded firmware arm a shared CT's channel against a one-zone
    // threshold.
    static const uint8_t splits[5][3] = {
        { 0u, 0u, 1u }, { 0u, 1u, 1u }, { 0u, 1u, 0u }, { 1u, 1u, 1u }, { 2u, 1u, 0u },
    };
    for (size_t i = 0; i < sizeof(splits) / sizeof(splits[0]); i++) {
        config_store_record_t rec;
        config_store_default(&rec);
        rec.ct_topology = CONFIG_STORE_CT_TOPOLOGY_PER_ZONE;
        rec.zone_ct_channel[0] = splits[i][0];
        rec.zone_ct_channel[1] = splits[i][1];
        rec.zone_ct_channel[2] = splits[i][2];
        rec.fields_set |= (uint32_t)CONFIG_STORE_SET_ZONE_CT_CHANNEL;
        config_store_backfill_legacy_ct_topology(&rec);
        TEST_CHECK(rec.ct_topology == CONFIG_STORE_CT_TOPOLOGY_SUMMED,
                   "a non-identity map back-fills ct_topology as SUMMED");

        // Idempotent: a second write of the same record must not move it again.
        uint8_t once[CONFIG_STORE_RECORD_LEN];
        uint8_t twice[CONFIG_STORE_RECORD_LEN];
        config_store_pack(&rec, once);
        config_store_backfill_legacy_ct_topology(&rec);
        config_store_pack(&rec, twice);
        TEST_CHECK(memcmp(once, twice, CONFIG_STORE_RECORD_LEN) == 0,
                   "back-fill is idempotent across repeated writes");
    }
}

static void test_backfill_respects_the_group_bit(void)
{
    TEST_SECTION("config_store_backfill_legacy_ct_topology: an untrusted map never rewrites ct_topology");

    // The dangerous direction: a board commissioned SUMMED under v2, upgraded,
    // never re-commissioned. Its zone bytes are the derived/compiled map and
    // the group bit is clear, so ct_topology is authoritative and must
    // survive untouched -- back-filling from the default identity map would
    // silently downgrade it to PER_ZONE.
    config_store_record_t rec;
    config_store_default(&rec);
    rec.ct_topology = CONFIG_STORE_CT_TOPOLOGY_SUMMED;
    rec.zone_ct_channel[0] = 0u;
    rec.zone_ct_channel[1] = 1u;
    rec.zone_ct_channel[2] = 2u;
    TEST_CHECK(!config_store_field_is_set(&rec.fields_set, CONFIG_STORE_SET_ZONE_CT_CHANNEL),
               "the default record has no committed zone map");
    uint8_t before[CONFIG_STORE_RECORD_LEN];
    uint8_t after[CONFIG_STORE_RECORD_LEN];
    config_store_pack(&rec, before);
    config_store_backfill_legacy_ct_topology(&rec);
    config_store_pack(&rec, after);
    TEST_CHECK(rec.ct_topology == CONFIG_STORE_CT_TOPOLOGY_SUMMED,
               "ct_topology survives a back-fill while the group bit is clear");
    TEST_CHECK(memcmp(before, after, CONFIG_STORE_RECORD_LEN) == 0,
               "an uncommitted zone map leaves the record byte-for-byte unchanged");

    // ct_channel_map (channel -> relay id) is deliberately NOT synthesised
    // from zone_ct_channel; prove the back-fill leaves it alone even for a
    // committed split map.
    config_store_default(&rec);
    rec.ct_channel_map[0] = 2u;
    rec.ct_channel_map[1] = 0u;
    rec.ct_channel_map[2] = 1u;
    rec.zone_ct_channel[0] = 0u;
    rec.zone_ct_channel[1] = 0u;
    rec.zone_ct_channel[2] = 1u;
    rec.fields_set |= (uint32_t)CONFIG_STORE_SET_ZONE_CT_CHANNEL;
    config_store_backfill_legacy_ct_topology(&rec);
    TEST_CHECK(rec.ct_channel_map[0] == 2u && rec.ct_channel_map[1] == 0u &&
               rec.ct_channel_map[2] == 1u,
               "operator-confirmed ct_channel_map is untouched by the back-fill");

    config_store_backfill_legacy_ct_topology(NULL); // NULL-safe, must not crash
}

void run_test_config_store_zone_ct_channel(void)
{
    test_derive();
    test_topology_collapse();
    test_member_count();
    test_fitted_map_collapses_onto_legacy_predicate();
    test_effective_map_honours_the_group_bit();
    test_round_trip();
    test_v2_migration();
    test_v2_migration_rejects_a_bad_crc();
    test_garbled_zone_bytes_normalise();
    test_default_record();
    test_finalize_requires_all_three();
    test_backfill_legacy_topology();
    test_backfill_splits_collapse_to_summed();
    test_backfill_respects_the_group_bit();
}
