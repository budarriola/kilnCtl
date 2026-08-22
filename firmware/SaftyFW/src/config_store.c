// config_store.c -- see config_store.h.
#include "config_store.h"

#include <string.h>

#include "crc32.h" // bootloader/ -- same CRC-32 used for metadata records

// --- v2 byte layout (little-endian, same convention as bootloader/metadata.c)
//
// Offset  Size  Field
//      0     4  magic
//      4     2  format_version
//      6     2  reserved0 (0)
//      8     4  seq
//     12     2  fields_set (bitmask, CONFIG_STORE_SET_*)
//     14     1  tc_source
//     15     1  borrowed_zone_index
//     16     1  tc_placement_mode
//     17     4  abs_max_temp_c (f32 LE)
//     21     1  tc_type
//     22     3  ct_channel_map[3]
//     25     1  calibration_missing (0/1)
//     26     4  firing_margin_c
//     30     4  overshoot_margin_c
//     34     4  overshoot_time_s (u32 LE)
//     38     4  max_rate_c_per_min
//     42     4  rate_window_s
//     46     4  blind_grace_s
//     50     4  frozen_window_s
//     54     4  tc_disagreement_c
//     58     4  tc_disagreement_time_s
//     62     4  tc_expected_offset_c
//     66     4  cj_warn_c
//     70     4  cj_max_c
//     74     4  cj_time_s
//     78     4  borrowed_stale_s
//     82     4  borrowed_stale_trip_s
//     86     1  borrowed_type_expected
//     87     4  i_present_a
//     91     6  zero_counts[3] (u16 LE x3)
//     97     4  correlation_window_s
//    101     4  stuck_on_time_s
//    105     4  trip_verify_s
//    109    12  k_ct_v_per_a[3] (f32 LE x3)
//    121    12  gain[3] (f32 LE x3)
//    133     4  mains_voltage_v
//    137     4  power_window_s
//    141     4  context_max_age_s
//    145     4  link_timeout_s
//    149     4  link_dead_hard_s
//    153     4  mainfault_debounce_ms
//    157     4  telemetry_period_ms
//    161     4  startup_grace_s
//    165     4  estop_debounce_ms
//    169     4  watchdog_timeout_ms
//    173     4  config_check_period_s
//    177    27  ct_cal: 3 channels x 9 B each (calibrated u8 + gain f32 LE +
//               offset f32 LE) -- unchanged shape/offset-within-block from v1
//    204   300  reserved, 0xFF-filled (headroom for a future field)
//    504     4  record_crc32, over bytes [0, 504)
//    508     4  reserved, 0xFF-filled (pad to CONFIG_STORE_RECORD_LEN)
//    512  total = CONFIG_STORE_RECORD_LEN
#define REC_OFF_MAGIC                  0u
#define REC_OFF_FORMAT_VERSION         4u
#define REC_OFF_SEQ                    8u
#define REC_OFF_FIELDS_SET             12u
#define REC_OFF_TC_SOURCE              14u
#define REC_OFF_BORROWED_ZONE_INDEX    15u
#define REC_OFF_TC_PLACEMENT_MODE      16u
#define REC_OFF_ABS_MAX_TEMP_C         17u
#define REC_OFF_TC_TYPE                21u
#define REC_OFF_CT_CHANNEL_MAP         22u
#define REC_CT_CHANNEL_MAP_LEN         3u
#define REC_OFF_CALIBRATION_MISSING    25u
#define REC_OFF_FIRING_MARGIN_C        26u
#define REC_OFF_OVERSHOOT_MARGIN_C     30u
#define REC_OFF_OVERSHOOT_TIME_S       34u
#define REC_OFF_MAX_RATE_C_PER_MIN     38u
#define REC_OFF_RATE_WINDOW_S          42u
#define REC_OFF_BLIND_GRACE_S          46u
#define REC_OFF_FROZEN_WINDOW_S        50u
#define REC_OFF_TC_DISAGREEMENT_C      54u
#define REC_OFF_TC_DISAGREEMENT_TIME_S 58u
#define REC_OFF_TC_EXPECTED_OFFSET_C   62u
#define REC_OFF_CJ_WARN_C              66u
#define REC_OFF_CJ_MAX_C               70u
#define REC_OFF_CJ_TIME_S              74u
#define REC_OFF_BORROWED_STALE_S       78u
#define REC_OFF_BORROWED_STALE_TRIP_S  82u
#define REC_OFF_BORROWED_TYPE_EXPECTED 86u
#define REC_OFF_I_PRESENT_A            87u
#define REC_OFF_ZERO_COUNTS            91u
#define REC_OFF_CORRELATION_WINDOW_S   97u
#define REC_OFF_STUCK_ON_TIME_S        101u
#define REC_OFF_TRIP_VERIFY_S          105u
#define REC_OFF_K_CT_V_PER_A           109u
#define REC_OFF_GAIN                   121u
#define REC_OFF_MAINS_VOLTAGE_V        133u
#define REC_OFF_POWER_WINDOW_S         137u
#define REC_OFF_CONTEXT_MAX_AGE_S      141u
#define REC_OFF_LINK_TIMEOUT_S         145u
#define REC_OFF_LINK_DEAD_HARD_S       149u
#define REC_OFF_MAINFAULT_DEBOUNCE_MS  153u
#define REC_OFF_TELEMETRY_PERIOD_MS    157u
#define REC_OFF_STARTUP_GRACE_S        161u
#define REC_OFF_ESTOP_DEBOUNCE_MS      165u
#define REC_OFF_WATCHDOG_TIMEOUT_MS    169u
#define REC_OFF_CONFIG_CHECK_PERIOD_S  173u
#define REC_OFF_CT_CAL                 177u
#define REC_CT_CAL_CHANNEL_LEN         9u /* calibrated u8(1) + gain f32(4) + offset f32(4) */
#define REC_OFF_RESERVED \
    (REC_OFF_CT_CAL + CONFIG_STORE_CT_CAL_NUM_CHANNELS * REC_CT_CAL_CHANNEL_LEN) /* 204 */
#define REC_RESERVED_LEN               300u
#define REC_OFF_CRC                    504u

// --- v1 (legacy) byte layout -- kept ONLY for config_store_unpack()'s
// migration path. This is the exact layout the original 256 B record used;
// it must never change, since real flash written by the shipped v1 firmware
// already exists in this shape and this module's job is to keep reading it
// correctly forever, not to keep it in sync with whatever v2 becomes.
#define REC_V1_OFF_MAGIC               0u
#define REC_V1_OFF_FORMAT_VERSION      4u
#define REC_V1_OFF_SEQ                 8u
#define REC_V1_OFF_TC_TYPE             12u
#define REC_V1_OFF_CALIBRATION_MISSING 13u
#define REC_V1_OFF_CT_CAL              16u
#define REC_V1_CT_CAL_CHANNEL_LEN      9u
#define REC_V1_OFF_CRC                 248u

// Compile-time budget check, mirroring
// bootloader_metadata_record_budget_check: the fixed header plus every field
// plus reserved room must fit inside the record before the CRC field -- if a
// future field addition breaks this, it must fail the build, not silently
// overrun into the CRC.
typedef char config_store_record_budget_check
    [(REC_OFF_RESERVED + REC_RESERVED_LEN <= REC_OFF_CRC) ? 1 : -1];

// Cross-check the v1 legacy layout's CRC field never collides with anything
// v2 now uses at overlapping offsets in the FIRST 248 bytes (v1's own
// records never occupy more) -- this can never fail given the constants
// above are literal, but it documents the invariant the same way the two
// pico/error.h cross-checks in config_store_flash.c do: a silent future edit
// to REC_V1_OFF_CRC would otherwise be the kind of change nothing else here
// would catch.
typedef char config_store_v1_layout_sane_check
    [(REC_V1_OFF_CT_CAL + CONFIG_STORE_CT_CAL_NUM_CHANNELS * REC_V1_CT_CAL_CHANNEL_LEN <=
      REC_V1_OFF_CRC) ? 1 : -1];

static void put_u16_le(uint8_t *out, uint16_t v)
{
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32_le(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v & 0xFFu);
    out[1] = (uint8_t)((v >> 8) & 0xFFu);
    out[2] = (uint8_t)((v >> 16) & 0xFFu);
    out[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint16_t get_u16_le(const uint8_t *in)
{
    return (uint16_t)(in[0] | ((uint16_t)in[1] << 8));
}

static uint32_t get_u32_le(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

// Float <-> 4-byte-LE via a union, same type-punning convention CommonFW's
// kilnlink_bytes.h uses for its f32 helpers -- this module does not depend
// on kilnlink, so it carries its own copy rather than reaching across a
// layer boundary for two small functions.
static void put_f32_le(uint8_t *out, float v)
{
    union {
        float    f;
        uint32_t u;
    } conv;
    conv.f = v;
    put_u32_le(out, conv.u);
}

static float get_f32_le(const uint8_t *in)
{
    union {
        float    f;
        uint32_t u;
    } conv;
    conv.u = get_u32_le(in);
    return conv.f;
}

// Packs just the ct_cal block -- shared between the v2 pack path below and
// (via unpack) both the v2 and v1-legacy unpack paths, since the 9-byte
// per-channel shape has never changed between format versions.
static void pack_ct_cal(uint8_t *out_base, const config_store_ct_channel_cal_t *ct_cal,
                         uint32_t channel_len)
{
    for (unsigned ch = 0; ch < CONFIG_STORE_CT_CAL_NUM_CHANNELS; ch++) {
        uint8_t *out = out_base + (size_t)ch * channel_len;
        out[0] = ct_cal[ch].calibrated ? 1u : 0u;
        put_f32_le(&out[1], ct_cal[ch].gain);
        put_f32_le(&out[5], ct_cal[ch].offset);
    }
}

static void unpack_ct_cal(const uint8_t *in_base, config_store_ct_channel_cal_t *ct_cal,
                           uint32_t channel_len)
{
    for (unsigned ch = 0; ch < CONFIG_STORE_CT_CAL_NUM_CHANNELS; ch++) {
        const uint8_t *in = in_base + (size_t)ch * channel_len;
        // Explicit: only wire byte value 1 means calibrated. Any other byte
        // -- 0 (config_store_default()'s zero-init, and every record ever
        // written before this field existed) or 0xFF (erased flash) --
        // decodes as NOT calibrated. See config_store.h's header comment on
        // config_store_ct_channel_cal_t for why this must be an explicit
        // byte, never inferred from the numbers.
        ct_cal[ch].calibrated = (in[0] == 1u);
        ct_cal[ch].gain = get_f32_le(&in[1]);
        ct_cal[ch].offset = get_f32_le(&in[5]);
    }
}

void config_store_pack(const config_store_record_t *rec,
                        uint8_t out[CONFIG_STORE_RECORD_LEN])
{
    memset(out, 0xFF, CONFIG_STORE_RECORD_LEN); // matches erased-flash background
    put_u32_le(&out[REC_OFF_MAGIC], CONFIG_STORE_MAGIC);
    put_u16_le(&out[REC_OFF_FORMAT_VERSION], rec->format_version);
    put_u16_le(&out[6], 0); // reserved0
    put_u32_le(&out[REC_OFF_SEQ], rec->seq);
    put_u16_le(&out[REC_OFF_FIELDS_SET], rec->fields_set);

    out[REC_OFF_TC_SOURCE] = rec->tc_source;
    out[REC_OFF_BORROWED_ZONE_INDEX] = rec->borrowed_zone_index;
    out[REC_OFF_TC_PLACEMENT_MODE] = rec->tc_placement_mode;
    put_f32_le(&out[REC_OFF_ABS_MAX_TEMP_C], rec->abs_max_temp_c);
    out[REC_OFF_TC_TYPE] = rec->tc_type;
    memcpy(&out[REC_OFF_CT_CHANNEL_MAP], rec->ct_channel_map, REC_CT_CHANNEL_MAP_LEN);
    out[REC_OFF_CALIBRATION_MISSING] = rec->calibration_missing ? 1u : 0u;

    put_f32_le(&out[REC_OFF_FIRING_MARGIN_C], rec->firing_margin_c);
    put_f32_le(&out[REC_OFF_OVERSHOOT_MARGIN_C], rec->overshoot_margin_c);
    put_u32_le(&out[REC_OFF_OVERSHOOT_TIME_S], rec->overshoot_time_s);
    put_f32_le(&out[REC_OFF_MAX_RATE_C_PER_MIN], rec->max_rate_c_per_min);
    put_u32_le(&out[REC_OFF_RATE_WINDOW_S], rec->rate_window_s);
    put_u32_le(&out[REC_OFF_BLIND_GRACE_S], rec->blind_grace_s);
    put_u32_le(&out[REC_OFF_FROZEN_WINDOW_S], rec->frozen_window_s);
    put_f32_le(&out[REC_OFF_TC_DISAGREEMENT_C], rec->tc_disagreement_c);
    put_u32_le(&out[REC_OFF_TC_DISAGREEMENT_TIME_S], rec->tc_disagreement_time_s);
    put_f32_le(&out[REC_OFF_TC_EXPECTED_OFFSET_C], rec->tc_expected_offset_c);
    put_f32_le(&out[REC_OFF_CJ_WARN_C], rec->cj_warn_c);
    put_f32_le(&out[REC_OFF_CJ_MAX_C], rec->cj_max_c);
    put_u32_le(&out[REC_OFF_CJ_TIME_S], rec->cj_time_s);
    put_u32_le(&out[REC_OFF_BORROWED_STALE_S], rec->borrowed_stale_s);
    put_u32_le(&out[REC_OFF_BORROWED_STALE_TRIP_S], rec->borrowed_stale_trip_s);
    out[REC_OFF_BORROWED_TYPE_EXPECTED] = rec->borrowed_type_expected;

    put_f32_le(&out[REC_OFF_I_PRESENT_A], rec->i_present_a);
    for (unsigned i = 0; i < 3u; i++) {
        put_u16_le(&out[REC_OFF_ZERO_COUNTS + i * 2u], rec->zero_counts[i]);
    }
    put_u32_le(&out[REC_OFF_CORRELATION_WINDOW_S], rec->correlation_window_s);
    put_u32_le(&out[REC_OFF_STUCK_ON_TIME_S], rec->stuck_on_time_s);
    put_u32_le(&out[REC_OFF_TRIP_VERIFY_S], rec->trip_verify_s);
    for (unsigned i = 0; i < 3u; i++) {
        put_f32_le(&out[REC_OFF_K_CT_V_PER_A + i * 4u], rec->k_ct_v_per_a[i]);
    }
    for (unsigned i = 0; i < 3u; i++) {
        put_f32_le(&out[REC_OFF_GAIN + i * 4u], rec->gain[i]);
    }
    put_f32_le(&out[REC_OFF_MAINS_VOLTAGE_V], rec->mains_voltage_v);
    put_u32_le(&out[REC_OFF_POWER_WINDOW_S], rec->power_window_s);

    put_u32_le(&out[REC_OFF_CONTEXT_MAX_AGE_S], rec->context_max_age_s);
    put_u32_le(&out[REC_OFF_LINK_TIMEOUT_S], rec->link_timeout_s);
    put_u32_le(&out[REC_OFF_LINK_DEAD_HARD_S], rec->link_dead_hard_s);
    put_u32_le(&out[REC_OFF_MAINFAULT_DEBOUNCE_MS], rec->mainfault_debounce_ms);
    put_u32_le(&out[REC_OFF_TELEMETRY_PERIOD_MS], rec->telemetry_period_ms);

    put_u32_le(&out[REC_OFF_STARTUP_GRACE_S], rec->startup_grace_s);
    put_u32_le(&out[REC_OFF_ESTOP_DEBOUNCE_MS], rec->estop_debounce_ms);
    put_u32_le(&out[REC_OFF_WATCHDOG_TIMEOUT_MS], rec->watchdog_timeout_ms);
    put_u32_le(&out[REC_OFF_CONFIG_CHECK_PERIOD_S], rec->config_check_period_s);

    pack_ct_cal(&out[REC_OFF_CT_CAL], rec->ct_cal, REC_CT_CAL_CHANNEL_LEN);

    memcpy(&out[REC_OFF_RESERVED], rec->reserved, sizeof(rec->reserved));
    // bytes [REC_OFF_RESERVED + REC_RESERVED_LEN, REC_OFF_CRC) already 0xFF
    // from the initial memset -- further headroom.
    uint32_t crc = bootloader_crc32(out, REC_OFF_CRC);
    put_u32_le(&out[REC_OFF_CRC], crc);
    // bytes [REC_OFF_CRC + 4, CONFIG_STORE_RECORD_LEN) already 0xFF -- pad.
}

// Unpacks a v2-shaped, CRC-verified record (caller has already checked magic
// and CRC) into `*out`. Split out of config_store_unpack() only so the v2
// load path and the "what a fresh migration looks like before defaults are
// overlaid" reasoning stay easy to follow -- there is exactly one caller.
static void unpack_v2_fields(const uint8_t *in, config_store_record_t *out)
{
    out->format_version = get_u16_le(&in[REC_OFF_FORMAT_VERSION]);
    out->seq = get_u32_le(&in[REC_OFF_SEQ]);
    out->fields_set = get_u16_le(&in[REC_OFF_FIELDS_SET]);

    out->tc_source = in[REC_OFF_TC_SOURCE];
    out->borrowed_zone_index = in[REC_OFF_BORROWED_ZONE_INDEX];
    out->tc_placement_mode = in[REC_OFF_TC_PLACEMENT_MODE];
    out->abs_max_temp_c = get_f32_le(&in[REC_OFF_ABS_MAX_TEMP_C]);
    out->tc_type = in[REC_OFF_TC_TYPE];
    memcpy(out->ct_channel_map, &in[REC_OFF_CT_CHANNEL_MAP], REC_CT_CHANNEL_MAP_LEN);
    out->calibration_missing = in[REC_OFF_CALIBRATION_MISSING] != 0u;

    out->firing_margin_c = get_f32_le(&in[REC_OFF_FIRING_MARGIN_C]);
    out->overshoot_margin_c = get_f32_le(&in[REC_OFF_OVERSHOOT_MARGIN_C]);
    out->overshoot_time_s = get_u32_le(&in[REC_OFF_OVERSHOOT_TIME_S]);
    out->max_rate_c_per_min = get_f32_le(&in[REC_OFF_MAX_RATE_C_PER_MIN]);
    out->rate_window_s = get_u32_le(&in[REC_OFF_RATE_WINDOW_S]);
    out->blind_grace_s = get_u32_le(&in[REC_OFF_BLIND_GRACE_S]);
    out->frozen_window_s = get_u32_le(&in[REC_OFF_FROZEN_WINDOW_S]);
    out->tc_disagreement_c = get_f32_le(&in[REC_OFF_TC_DISAGREEMENT_C]);
    out->tc_disagreement_time_s = get_u32_le(&in[REC_OFF_TC_DISAGREEMENT_TIME_S]);
    out->tc_expected_offset_c = get_f32_le(&in[REC_OFF_TC_EXPECTED_OFFSET_C]);
    out->cj_warn_c = get_f32_le(&in[REC_OFF_CJ_WARN_C]);
    out->cj_max_c = get_f32_le(&in[REC_OFF_CJ_MAX_C]);
    out->cj_time_s = get_u32_le(&in[REC_OFF_CJ_TIME_S]);
    out->borrowed_stale_s = get_u32_le(&in[REC_OFF_BORROWED_STALE_S]);
    out->borrowed_stale_trip_s = get_u32_le(&in[REC_OFF_BORROWED_STALE_TRIP_S]);
    out->borrowed_type_expected = in[REC_OFF_BORROWED_TYPE_EXPECTED];

    out->i_present_a = get_f32_le(&in[REC_OFF_I_PRESENT_A]);
    for (unsigned i = 0; i < 3u; i++) {
        out->zero_counts[i] = get_u16_le(&in[REC_OFF_ZERO_COUNTS + i * 2u]);
    }
    out->correlation_window_s = get_u32_le(&in[REC_OFF_CORRELATION_WINDOW_S]);
    out->stuck_on_time_s = get_u32_le(&in[REC_OFF_STUCK_ON_TIME_S]);
    out->trip_verify_s = get_u32_le(&in[REC_OFF_TRIP_VERIFY_S]);
    for (unsigned i = 0; i < 3u; i++) {
        out->k_ct_v_per_a[i] = get_f32_le(&in[REC_OFF_K_CT_V_PER_A + i * 4u]);
    }
    for (unsigned i = 0; i < 3u; i++) {
        out->gain[i] = get_f32_le(&in[REC_OFF_GAIN + i * 4u]);
    }
    out->mains_voltage_v = get_f32_le(&in[REC_OFF_MAINS_VOLTAGE_V]);
    out->power_window_s = get_u32_le(&in[REC_OFF_POWER_WINDOW_S]);

    out->context_max_age_s = get_u32_le(&in[REC_OFF_CONTEXT_MAX_AGE_S]);
    out->link_timeout_s = get_u32_le(&in[REC_OFF_LINK_TIMEOUT_S]);
    out->link_dead_hard_s = get_u32_le(&in[REC_OFF_LINK_DEAD_HARD_S]);
    out->mainfault_debounce_ms = get_u32_le(&in[REC_OFF_MAINFAULT_DEBOUNCE_MS]);
    out->telemetry_period_ms = get_u32_le(&in[REC_OFF_TELEMETRY_PERIOD_MS]);

    out->startup_grace_s = get_u32_le(&in[REC_OFF_STARTUP_GRACE_S]);
    out->estop_debounce_ms = get_u32_le(&in[REC_OFF_ESTOP_DEBOUNCE_MS]);
    out->watchdog_timeout_ms = get_u32_le(&in[REC_OFF_WATCHDOG_TIMEOUT_MS]);
    out->config_check_period_s = get_u32_le(&in[REC_OFF_CONFIG_CHECK_PERIOD_S]);

    unpack_ct_cal(&in[REC_OFF_CT_CAL], out->ct_cal, REC_CT_CAL_CHANNEL_LEN);

    memcpy(out->reserved, &in[REC_OFF_RESERVED], sizeof(out->reserved));
}

bool config_store_unpack(const uint8_t in[CONFIG_STORE_RECORD_LEN],
                          config_store_record_t *out)
{
    if (in == NULL || out == NULL) {
        return false;
    }

    if (get_u32_le(&in[REC_OFF_MAGIC]) != CONFIG_STORE_MAGIC) {
        return false; // erased flash (0xFFFFFFFF) or garbage -- not this format
    }

    uint16_t version = get_u16_le(&in[REC_OFF_FORMAT_VERSION]);

    if (version == CONFIG_STORE_FORMAT_VERSION) {
        // Current (v2) layout -- CRC covers the whole v2 header/field block.
        uint32_t stored_crc = get_u32_le(&in[REC_OFF_CRC]);
        uint32_t computed_crc = bootloader_crc32(in, REC_OFF_CRC);
        if (stored_crc != computed_crc) {
            return false; // corrupted, or a torn write caught mid-program
        }
        unpack_v2_fields(in, out);
        return true;
    }

    if (version == CONFIG_STORE_FORMAT_VERSION_V1) {
        // Legacy v1 layout -- its CRC covers only the shorter v1 header/
        // ct_cal block (bytes [0, REC_V1_OFF_CRC)), the same range
        // config_store_pack() covered before this pass existed. A v1 record
        // is real flash from before this pass shipped: its CRC must still be
        // checked against ITS OWN layout, never the v2 one.
        uint32_t stored_crc = get_u32_le(&in[REC_V1_OFF_CRC]);
        uint32_t computed_crc = bootloader_crc32(in, REC_V1_OFF_CRC);
        if (stored_crc != computed_crc) {
            return false; // corrupted, or a torn write caught mid-program
        }

        // Migrate forward: start from the safe v2 default (every new field
        // gets its documented default, fields_set is 0 -- nothing the v1
        // record never had a chance to commission reads back as set), then
        // overlay exactly what v1 actually held.
        config_store_default(out);
        out->seq = get_u32_le(&in[REC_V1_OFF_SEQ]);
        out->tc_type = in[REC_V1_OFF_TC_TYPE];
        unpack_ct_cal(&in[REC_V1_OFF_CT_CAL], out->ct_cal, REC_V1_CT_CAL_CHANNEL_LEN);
        // calibration_missing is FORCED true regardless of what the v1
        // record held -- see this file's header comment (config_store.h)
        // for why: a migrated record was never commissioned against the
        // fields this pass added, so it must not be trusted as "fully
        // commissioned" just because v1's own narrower surface was.
        out->calibration_missing = true;
        // The record is now v2-shaped in RAM; a caller inspecting *out has
        // no way to tell "loaded as v2" from "migrated from v1" except via
        // calibration_missing above, which is the only distinction that
        // actually matters to any guard.
        out->format_version = CONFIG_STORE_FORMAT_VERSION;
        return true;
    }

    // Anything else -- version 0, or any value > CONFIG_STORE_FORMAT_VERSION
    // (a record written by firmware newer than this build) -- is refused,
    // not reinterpreted. See config_store.h's header comment for why a
    // "confident, plausible, wrong" misread is worse than an honest refusal;
    // the caller (config_store_find_latest() / config_store_flash.c's
    // read_latest_or_default()) falls back to config_store_default() with
    // calibration_missing set, exactly as it does for a CRC failure.
    return false;
}

void config_store_default(config_store_record_t *out)
{
    memset(out, 0, sizeof(*out));
    out->format_version = CONFIG_STORE_FORMAT_VERSION;
    out->seq = 0;
    out->fields_set = 0; // nothing commissioned

    // Section 1 -- fields_set-gated fields left at 0/0.0f: IRRELEVANT (see
    // config_store.h's header comment), documented here only so the zero is
    // never mistaken for a real default.
    out->tc_source = 0;
    out->borrowed_zone_index = 0;
    out->tc_placement_mode = 0;
    out->abs_max_temp_c = 0.0f;
    out->tc_type = CONFIG_STORE_DEFAULT_TC_TYPE; // K -- this one DOES have a
                                                  // real compiled default,
                                                  // unlike the fields above
    memset(out->ct_channel_map, 0xFF, sizeof(out->ct_channel_map)); // 0xFF: a
                                                                     // visibly
                                                                     // implausible
                                                                     // relay/zone
                                                                     // id, not
                                                                     // that it
                                                                     // matters --
                                                                     // fields_set
                                                                     // is what
                                                                     // gates it
    out->calibration_missing = true; // always true until a real commissioning
                                      // pass clears it -- see config_store.h

    // Section 2: temperature guards -- CONFIG_REFERENCE.md section 2 defaults.
    out->firing_margin_c = 100.0f;
    out->overshoot_margin_c = 75.0f;
    out->overshoot_time_s = 120u;
    out->max_rate_c_per_min = 0.0f; // "0 = off"; genuinely UNSET (fields_set clear)
    out->rate_window_s = 60u;
    out->blind_grace_s = 60u;
    out->frozen_window_s = 600u;
    out->tc_disagreement_c = 200.0f;
    out->tc_disagreement_time_s = 300u;
    out->tc_expected_offset_c = 0.0f;
    out->cj_warn_c = 60.0f;
    out->cj_max_c = 85.0f;
    out->cj_time_s = 60u;
    out->borrowed_stale_s = 10u;
    out->borrowed_stale_trip_s = 60u;
    out->borrowed_type_expected = CONFIG_STORE_DEFAULT_TC_TYPE; // reuses
                                                                 // tc_type's
                                                                 // own
                                                                 // documented
                                                                 // placeholder;
                                                                 // not
                                                                 // fields_set-gated
                                                                 // (see
                                                                 // config_store.h)

    // Section 3: current channels -- CONFIG_REFERENCE.md section 3 defaults.
    out->i_present_a = 2.0f;
    // zero_counts left at memset(0) above -- "measured", re-measured at
    // runtime after idle; 0 is a placeholder, not a claim of accuracy.
    out->correlation_window_s = 150u;
    out->stuck_on_time_s = 20u;
    out->trip_verify_s = 10u;
    // k_ct_v_per_a left at memset(0) above -- CONFIG_REFERENCE.md documents
    // no default ("--"); power-estimate-only, no guard depends on it.
    out->gain[0] = 0.715f;
    out->gain[1] = 0.715f;
    out->gain[2] = 0.715f;
    out->mains_voltage_v = 0.0f; // genuinely UNSET (fields_set clear)
    out->power_window_s = 120u;

    // Section 4: link and liveness.
    out->context_max_age_s = 5u;
    out->link_timeout_s = 10u;
    out->link_dead_hard_s = 120u;
    out->mainfault_debounce_ms = 200u;
    out->telemetry_period_ms = 500u;

    // Section 5: timing and system.
    out->startup_grace_s = 60u;
    out->estop_debounce_ms = 50u;
    out->watchdog_timeout_ms = 1000u;
    out->config_check_period_s = 10u;

    // ct_cal: left at the memset(0) above -- calibrated == false on every
    // channel, gain/offset == 0 but IGNORED (never read) as a consequence.
    // This is the "uncalibrated is explicit" default: ct_amps_cal_apply()
    // treats calibrated == false as pass-the-raw-reading-through, not as
    // "gain 0, offset 0" -- see config_store.h's header comment.

    // reserved: left at memset(0) above. Note this differs from
    // config_store_pack()'s own 0xFF fill of the wire bytes -- pack() always
    // re-fills the reserved region as 0xFF regardless of what *out held, so
    // this in-RAM zero is never actually written to flash as-is.
}

size_t config_store_find_latest(const uint8_t sector[SAFTYFW_CONFIG_STORE_FLASH_SIZE],
                                 config_store_record_t *out_rec)
{
    if (sector == NULL || out_rec == NULL) {
        return CONFIG_STORE_NO_SLOT;
    }

    size_t best_slot = CONFIG_STORE_NO_SLOT;
    config_store_record_t best_rec;
    memset(&best_rec, 0, sizeof(best_rec));
    bool have_best = false;

    for (size_t i = 0; i < CONFIG_STORE_SLOTS_PER_SECTOR; i++) {
        const uint8_t *rec_bytes = &sector[i * CONFIG_STORE_RECORD_LEN];
        config_store_record_t candidate;
        if (!config_store_unpack(rec_bytes, &candidate)) {
            continue; // erased, corrupt, or too-new-to-trust -- skip, not an error
        }
        if (!have_best || candidate.seq > best_rec.seq) {
            best_rec = candidate;
            best_slot = i;
            have_best = true;
        }
    }

    if (!have_best) {
        return CONFIG_STORE_NO_SLOT;
    }

    *out_rec = best_rec;
    return best_slot;
}

size_t config_store_next_write_slot(size_t latest_slot_index)
{
    if (latest_slot_index == CONFIG_STORE_NO_SLOT) {
        return 0;
    }
    size_t next = latest_slot_index + 1;
    if (next >= CONFIG_STORE_SLOTS_PER_SECTOR) {
        return 0;
    }
    return next;
}

bool config_store_next_write_needs_erase(size_t latest_slot_index)
{
    if (latest_slot_index == CONFIG_STORE_NO_SLOT) {
        return false; // never written -- slot 0 may already be erased, don't assume otherwise
    }
    return (latest_slot_index + 1) >= CONFIG_STORE_SLOTS_PER_SECTOR;
}

config_store_write_decision_t config_store_decide_write(bool armed)
{
    return armed ? CONFIG_STORE_WRITE_REFUSED_ARMED : CONFIG_STORE_WRITE_OK;
}

uint32_t config_store_record_crc(const config_store_record_t *rec)
{
    uint8_t packed[CONFIG_STORE_RECORD_LEN];
    config_store_pack(rec, packed);
    return get_u32_le(&packed[REC_OFF_CRC]);
}

const char *config_store_write_decision_reason(config_store_write_decision_t decision)
{
    switch (decision) {
        case CONFIG_STORE_WRITE_OK:
            return "ok";
        case CONFIG_STORE_WRITE_REFUSED_ARMED:
            return "refused: relay is ARMED, config writes are refused while ARMED";
        default:
            return "unknown";
    }
}

bool config_store_confirm_crc_ok(uint8_t config_version)
{
    // See config_store.h's header comment on this function for the full
    // reasoning -- version 0 is the one value a genuinely written, CRC-
    // verified record can never produce, so it is the only safe "not
    // confirmed" signal, covering both "never loaded" and "loaded but
    // nothing valid found" without needing to tell those two apart.
    return config_version != 0u;
}

const char *config_store_flash_rc_reason(int rc)
{
    switch (rc) {
        case CONFIG_STORE_FLASH_RC_OK:
            return "ok";
        case CONFIG_STORE_FLASH_RC_TIMEOUT:
            return "flash write failed: flash_safe_execute() timed out waiting for the "
                   "other core to answer the lockout request";
        case CONFIG_STORE_FLASH_RC_NOT_PERMITTED:
            return "flash write failed: flash_safe_execute() reports safe execution is "
                   "not possible (other core not initialised for lockout, or called from "
                   "an unsafe context)";
        case CONFIG_STORE_FLASH_RC_INSUFFICIENT_RESOURCES:
            return "flash write failed: flash_safe_execute()'s lockout handshake could "
                   "not allocate the resources it needed";
        default:
            return "flash write failed: flash_safe_execute() returned an unrecognised "
                   "error code";
    }
}
