// config_store.c -- see config_store.h.
#include "config_store.h"

#include "discrete_pin_policy.h" // DISCRETE_PIN_POLICY_ESTOP_ACTIVE_* -- the
                                // encoding this record's estop_active_level
                                // byte carries; one shared definition rather
                                // than a duplicated 0/1 convention

#include <string.h>

#include "config_params.h" // config_params_validate_ranges() -- load-time re-check, see config_store_unpack()
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
//    204     1  safety_tc_installed_marker (u8) -- carved out of the former
//               300 B reserved block's first byte; see REC_OFF_SAFETY_TC_
//               INSTALLED below. NOT a 0/1 bool on the wire: a record from
//               before this field existed holds 0x00 here (confirmed on
//               real hardware -- old config_store_pack() memcpy'd rec->
//               reserved, and config_store_default() memset that array to
//               0, so 0x00 -- not the erased-flash 0xFF this comment used
//               to claim -- is what every pre-existing record actually
//               contains). A "declared not installed" state must therefore
//               be a positive, improbable assertion (SAFETY_TC_INSTALLED_
//               MARKER_NOT_INSTALLED, 0xA5) rather than inferred from the
//               byte's absence -- every other value (0x00 legacy, 0xFF
//               erased flash, this build's own 0x01 "installed" marker,
//               or any garbage byte) decodes as installed, the safe
//               default. See unpack_v2_fields()'s comment at this offset.
//    205     4  max_expected_power_w (f32 LE); gated by
//               CONFIG_STORE_SET_MAX_EXPECTED_POWER_W, carved out of the
//               front of the former 299 B reserved block, same convention as
//               REC_OFF_SAFETY_TC_INSTALLED above
//    209     4  i_normal_a[3][0] -- S14 (COMMISSIONING_UX.md sec 3.3), carved
//               out of the former 295 B reserved block, same convention as
//               REC_OFF_MAX_EXPECTED_POWER_W above
//    213     8  i_normal_a[3][1..2] (f32 LE x2)
//    221     2  overcurrent_pct (u16 LE)
//    223     4  overcurrent_time_s (u32 LE)
//    227     1  ct_installed_marker (u8) -- same positive-assertion marker
//               convention as safety_tc_installed_marker at 204: only the
//               explicit CT_INSTALLED_MARKER_NOT_INSTALLED (0xA5) byte
//               decodes as "no CTs fitted"; 0x00 (a legacy record's zeroed
//               reserved byte), 0xFF (erased flash / the reserved fill) and
//               anything else decode as INSTALLED, the strict state. Unlike
//               safety_tc_installed this field is additionally gated by
//               CONFIG_STORE_SET_CT_INSTALLED, so a legacy record cannot
//               even reach the value -- the marker convention is belt and
//               braces, deliberately, because the consequence of a
//               mis-decode here is three disarmed guards.
//    228     1  ct_topology (u8, 0=per_zone/1=summed) -- CT_COMMISSIONING_
//               PLAN.md step 3, param 0x031F. Plain 0/1 byte; any other
//               value (legacy 0x00, erased-flash 0xFF) decodes as per_zone.
//    229     1  i_present_a_manual (u8, 0/1) -- true iff i_present_a (offset
//               87) was set directly via SET_PARAM rather than auto-derived
//               from i_normal_a. Same "only 1 means true" decode as above.
//    230     4  tc_offset_c (f32 LE) -- safety board's own TC calibration
//               correction, owner request 2026-09-08; see config_store.h's
//               struct comment
//    234     1  estop_active_level (u8, 0=active-high/1=active-low) -- param
//               0x0212, owner decision 2026-09-08. Plain 0/1 byte, same
//               convention as ct_topology at 228: only the value 1 selects
//               ACTIVE_LOW, so a legacy record's 0x00 and erased flash's
//               0xFF both decode to ACTIVE_HIGH -- which is both the bench's
//               real wiring and the fail-safe polarity. An unreadable
//               configuration must never select the polarity that cannot
//               detect a broken E-stop line.
//    235   269  reserved, 0xFF-filled (headroom for a future field)
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
#define REC_OFF_SAFETY_TC_INSTALLED \
    (REC_OFF_CT_CAL + CONFIG_STORE_CT_CAL_NUM_CHANNELS * REC_CT_CAL_CHANNEL_LEN) /* 204 */
#define REC_OFF_MAX_EXPECTED_POWER_W   (REC_OFF_SAFETY_TC_INSTALLED + 1u) /* 205 */
// S14 (COMMISSIONING_UX.md section 3.3), carved out of the reserved tail --
// OQ2 resolved: 16 B fit comfortably inside the 295 B reserved block with no
// format_version bump, so every already-committed v2 record keeps loading
// exactly as it did (calibration_missing untouched by this addition).
#define REC_OFF_I_NORMAL_A             (REC_OFF_MAX_EXPECTED_POWER_W + 4u) /* 209 */
#define REC_OFF_OVERCURRENT_PCT        (REC_OFF_I_NORMAL_A + 3u * 4u)      /* 221 */
#define REC_OFF_OVERCURRENT_TIME_S     (REC_OFF_OVERCURRENT_PCT + 2u)      /* 223 */
#define REC_OFF_CT_INSTALLED           (REC_OFF_OVERCURRENT_TIME_S + 4u)  /* 227 */
// ct_topology/i_present_a_manual (CT_COMMISSIONING_PLAN.md step 3), carved
// out of the front of the reserved tail same as every field above -- plain
// 0/1 bytes, not the 0xA5-marker convention: this build only ever writes 0
// or 1 (config_params.c's CHECK_U8_MAX(1u)/CHECK_TYPE(BOOL)), and BOTH
// decoders below only special-case the value 1 (SUMMED / manual==true) --
// any other byte, including an old record's never-written 0xFF or a legacy
// record's zeroed 0x00, already falls through to the safe default (per_zone
// / auto-derive) without needing a distinct improbable sentinel.
#define REC_OFF_CT_TOPOLOGY            (REC_OFF_CT_INSTALLED + 1u)        /* 228 */
#define REC_OFF_I_PRESENT_A_MANUAL     (REC_OFF_CT_TOPOLOGY + 1u)         /* 229 */
// tc_offset_c (owner request "I should be able to set the safety
// thermocouple type" pass, 2026-09-08): a calibration correction ADDED to
// the safety board's own MAX31856 hot-junction reading before any guard
// (or the wire telemetry) ever sees it -- distinct from tc_expected_offset_c
// (0x020A) above, which is S10's captured steady-state DISAGREEMENT offset
// between the safety TC and a zone TC, never applied to the reading itself.
// Carved out of the front of the reserved tail, same convention as every
// field above since REC_OFF_SAFETY_TC_INSTALLED: a record committed before
// this field existed holds whatever incidental byte pattern used to live
// here (0xFF erased-flash fill, most likely) at this offset, which is why
// this field is NOT fields_set-gated -- 0.0f (no correction) is a safe
// default for an unset offset, unlike abs_max_temp_c/tc_type, so
// config_store_default()/config_store_unpack() (below) explicitly zero
// this offset on migration/erased-flash decode rather than trusting
// whatever stale bytes are already there, the same "explicit zero, not
// trust-the-bytes" discipline overcurrent_pct/_time_s use for their own
// carved-from-reserved fields.
#define REC_OFF_TC_OFFSET_C            (REC_OFF_I_PRESENT_A_MANUAL + 1u)  /* 230 */
// estop_active_level (owner decision 2026-09-08, "Estop polarity should be
// configureable but the state it is in now on my test setup should be
// considered the default and the prefered safe to fire state"). Carved out
// of the front of the reserved tail, plain 0/1 byte, same convention and
// same reasoning as REC_OFF_CT_TOPOLOGY above: the decoder special-cases
// only the value 1, so every byte pattern a record written before this
// field existed could hold at this offset -- 0xFF erased-flash fill most
// likely -- decodes to ACTIVE_HIGH. That is deliberately the direction
// where an unreadable/legacy configuration lands on the FAIL-SAFE polarity
// (the one where a cut E-stop line reads as STOP), never on the one that
// cannot see a broken wire at all.
#define REC_OFF_ESTOP_ACTIVE_LEVEL     (REC_OFF_TC_OFFSET_C + 4u)         /* 234 */
#define REC_OFF_RESERVED               (REC_OFF_ESTOP_ACTIVE_LEVEL + 1u)  /* 235 */
#define REC_RESERVED_LEN               269u

// The one byte at REC_OFF_SAFETY_TC_INSTALLED is NOT a 0/1 bool -- see this
// file's own layout-table comment above for the hardware-confirmed reason:
// a record written before this field existed holds 0x00 there (not 0xFF),
// so "not installed" must be a positive, improbable assertion rather than
// anything inferable from an old record's incidental contents. 0xA5 was
// picked because it is neither 0x00 (every legacy record), 0xFF (erased
// flash / the old reserved-fill), nor 0x01 (this build's own "installed"
// marker, chosen only for readability in a flash dump -- decode treats
// every non-0xA5 byte identically, so 0x01 has no special status the
// decoder actually depends on).
#define SAFETY_TC_INSTALLED_MARKER_INSTALLED     0x01u
#define SAFETY_TC_INSTALLED_MARKER_NOT_INSTALLED 0xA5u

// ct_installed uses the identical encoding, for the identical reason -- see
// REC_OFF_CT_INSTALLED in this file's layout table. Same two byte values on
// purpose: two different meanings for 0xA5 at two different offsets is one
// fewer magic number to remember than two different sentinels would be, and
// the offsets are what disambiguate them.
#define CT_INSTALLED_MARKER_INSTALLED            0x01u
#define CT_INSTALLED_MARKER_NOT_INSTALLED        0xA5u
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

    // Explicit sentinel encoding -- see unpack_v2_fields()'s comment at this
    // same offset for why 0/1 was replaced with a positive assertion.
    out[REC_OFF_SAFETY_TC_INSTALLED] = rec->safety_tc_installed
                                            ? SAFETY_TC_INSTALLED_MARKER_INSTALLED
                                            : SAFETY_TC_INSTALLED_MARKER_NOT_INSTALLED;

    out[REC_OFF_CT_INSTALLED] = rec->ct_installed ? CT_INSTALLED_MARKER_INSTALLED
                                                  : CT_INSTALLED_MARKER_NOT_INSTALLED;

    // Plain 0/1 bytes -- see REC_OFF_CT_TOPOLOGY's own layout-table comment
    // for why no 0xA5-style marker is needed here.
    out[REC_OFF_CT_TOPOLOGY] = (rec->ct_topology == CONFIG_STORE_CT_TOPOLOGY_SUMMED) ? 1u : 0u;
    out[REC_OFF_I_PRESENT_A_MANUAL] = rec->i_present_a_manual ? 1u : 0u;
    put_f32_le(&out[REC_OFF_TC_OFFSET_C], rec->tc_offset_c);
    // Plain 0/1 byte -- see REC_OFF_ESTOP_ACTIVE_LEVEL's layout comment.
    out[REC_OFF_ESTOP_ACTIVE_LEVEL] =
        (rec->estop_active_level == DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW) ? 1u : 0u;

    put_f32_le(&out[REC_OFF_MAX_EXPECTED_POWER_W], rec->max_expected_power_w);

    for (unsigned ch = 0; ch < 3; ch++) {
        put_f32_le(&out[REC_OFF_I_NORMAL_A + ch * 4u], rec->i_normal_a[ch]);
    }
    put_u16_le(&out[REC_OFF_OVERCURRENT_PCT], rec->overcurrent_pct);
    put_u32_le(&out[REC_OFF_OVERCURRENT_TIME_S], rec->overcurrent_time_s);

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
    // Defense in depth (config_store.h's CONFIG_STORE_TC_TYPE_MAX_REAL
    // comment): a CRC-valid record whose tc_type byte is 0x08-0x0F selects
    // the MAX31856's Voltage Mode, not a real thermocouple type -- clamp to
    // the safe compiled default here rather than trust the raw byte, so a
    // garbled-but-CRC-valid record or a future writer that forgot to
    // validate can never hand max31856_configure() a voltage-mode code.
    uint8_t tc_type_byte = in[REC_OFF_TC_TYPE];
    out->tc_type = (tc_type_byte <= CONFIG_STORE_TC_TYPE_MAX_REAL) ? tc_type_byte
                                                                    : CONFIG_STORE_DEFAULT_TC_TYPE;
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

    // ONLY the explicit sentinel SAFETY_TC_INSTALLED_MARKER_NOT_INSTALLED
    // (0xA5) means "not installed" -- every other byte value decodes as
    // installed. This is deliberately NOT "explicit 0 means not installed,
    // anything else means installed" (what this comment used to claim):
    // that polarity was checked against hardware and found wrong -- a
    // record written by firmware from before this field existed holds
    // 0x00 here, not 0xFF, because old config_store_pack() memcpy'd
    // rec->reserved (a 300-byte array starting at this exact offset) over
    // it, and config_store_default() left that array at memset(0). An
    // "explicit 0 means not installed" decoder therefore read every
    // pre-existing board as declared-absent on first boot of this
    // firmware -- confirmed live: S5 silently downgraded to WARN and
    // heat refused with nobody having declared anything. A positive,
    // improbable sentinel closes that: 0x00 (every legacy record), 0xFF
    // (erased flash / the old reserved-fill), 0x01 (this build's own
    // "installed" marker), and any other garbage byte all decode as
    // installed -- the safe default -- and ONLY a real SET_PARAM/
    // COMMIT_CONFIG that has genuinely declared the sensor absent (which
    // writes the sentinel explicitly, config_store_pack() below) reads
    // back as not-installed. This is the same polarity direction as
    // unpack_ct_cal()'s calibrated flag just above (an old/unknown byte
    // decodes to the SAFE state, not the asserted one) -- the earlier
    // "opposite polarity" framing of this comment was itself wrong, a
    // symptom of the same unverified assumption this whole comment now
    // corrects.
    out->safety_tc_installed =
        (in[REC_OFF_SAFETY_TC_INSTALLED] == SAFETY_TC_INSTALLED_MARKER_NOT_INSTALLED) ? 0u : 1u;

    // ct_installed -- identical decode, identical safe-direction reasoning:
    // every byte except the one explicit "not installed" sentinel means
    // INSTALLED, i.e. the state in which ct_channel_map stays required and
    // S3/S4/S9/S14 stay armed. Callers must additionally check
    // CONFIG_STORE_SET_CT_INSTALLED before treating a 0 here as an operator's
    // answer rather than a decode artefact (safety_core.c and
    // config_params_all_required_set() both do).
    out->ct_installed =
        (in[REC_OFF_CT_INSTALLED] == CT_INSTALLED_MARKER_NOT_INSTALLED) ? 0u : 1u;

    // ct_topology/i_present_a_manual (CT_COMMISSIONING_PLAN.md step 3):
    // ONLY the byte value 1 decodes as the non-default state -- a legacy
    // record (0x00, never wrote this offset) and an erased/never-written
    // record (0xFF) both fall through to the safe default (per_zone /
    // auto-derive), same direction as every other decode in this function.
    out->ct_topology = (in[REC_OFF_CT_TOPOLOGY] == 1u)
                            ? CONFIG_STORE_CT_TOPOLOGY_SUMMED
                            : CONFIG_STORE_CT_TOPOLOGY_PER_ZONE;
    out->i_present_a_manual = (in[REC_OFF_I_PRESENT_A_MANUAL] == 1u);
    // tc_offset_c: raw decode, no fields_set gate needed -- see config_store.h's
    // struct comment on why every pre-existing record's byte pattern here
    // (0x00, confirmed, not erased-flash 0xFF) already decodes as the safe
    // 0.0f default.
    out->tc_offset_c = get_f32_le(&in[REC_OFF_TC_OFFSET_C]);
    // Only the value 1 selects ACTIVE_LOW; everything else (legacy 0x00,
    // erased 0xFF, anything unrecognised) is the fail-safe ACTIVE_HIGH.
    out->estop_active_level = (in[REC_OFF_ESTOP_ACTIVE_LEVEL] == 1u)
                                  ? DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW
                                  : DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH;

    // Raw decode only -- CONFIG_STORE_SET_MAX_EXPECTED_POWER_W (already read
    // into out->fields_set above) is what gates whether any caller may trust
    // this value. A record written before this field existed has whatever
    // byte the OLD reserved-block fill (0xFF, or an old build's own
    // memcpy'd rec->reserved) happened to leave here, which can decode as
    // NaN/Inf/garbage -- exactly why no consumer may read this field without
    // checking the bit first, same discipline as abs_max_temp_c and the other
    // fields_set-gated fields.
    out->max_expected_power_w = get_f32_le(&in[REC_OFF_MAX_EXPECTED_POWER_W]);

    // S14 (COMMISSIONING_UX.md section 3.3). i_normal_a[ch] is gated by
    // CONFIG_STORE_SET_I_NORMAL_A_0/_1/_2 (already read into out->fields_set
    // above) exactly like max_expected_power_w above -- raw decode only,
    // callers must check the bit. A record written before this field
    // existed has whatever this offset's old reserved-fill byte happened to
    // be (0xFF, erased flash) here, which decodes as NaN/Inf/garbage; that
    // is fine precisely because the bit that would be required to trust it
    // cannot be set on such a record (it did not exist to set).
    for (unsigned ch = 0; ch < 3; ch++) {
        out->i_normal_a[ch] = get_f32_le(&in[REC_OFF_I_NORMAL_A + ch * 4u]);
    }
    // overcurrent_pct/overcurrent_time_s are NOT fields_set-gated (see
    // config_store.h's struct comment) -- they follow the ordinary
    // "0 means not configured, substitute the default" convention every
    // other DEFAULTED field in this file uses. Unlike those older fields,
    // though, this offset previously lived inside the RESERVED tail.
    //
    // What a record written before this pass actually holds here is 0x00,
    // NOT the erased-flash 0xFF this comment used to claim: this file's own
    // hardware-confirmed comment at REC_OFF_SAFETY_TC_INSTALLED above proves
    // the legacy fill is 0x00 -- config_store_pack() memcpy'd rec->reserved,
    // and config_store_default() memset that array to 0, so every
    // pre-existing record's reserved tail (including this offset, before it
    // was carved out of it) was written as zero bytes, not left as raw
    // erased flash. 0x0000/0x00000000 already decodes to plain 0, which is
    // exactly "not configured" -- no fold is needed for that case, and the
    // 0xFFFF/0xFFFFFFFF branch below is therefore structurally unreachable
    // against every record this firmware has ever actually written (the
    // same "check that cannot fire" pattern as this repo's other four
    // instances). It is harmless either way (0xFFFF also folds to the same
    // 0 that 0x0000 already reads as), and kept here purely as belt-and-
    // braces against a byte pattern this migration has never observed in
    // practice -- e.g. a record read from flash that was truly erased and
    // never packed by any build. Not deleted, because the cost of keeping it
    // is one dead branch and the cost of being wrong about "never happens"
    // on a safety config migration is a silent default.
    uint16_t oc_pct_raw = get_u16_le(&in[REC_OFF_OVERCURRENT_PCT]);
    out->overcurrent_pct = (oc_pct_raw == 0xFFFFu) ? 0u : oc_pct_raw;
    uint32_t oc_time_raw = get_u32_le(&in[REC_OFF_OVERCURRENT_TIME_S]);
    out->overcurrent_time_s = (oc_time_raw == 0xFFFFFFFFu) ? 0u : oc_time_raw;

    memcpy(out->reserved, &in[REC_OFF_RESERVED], sizeof(out->reserved));
}

bool config_store_unpack(const uint8_t in[CONFIG_STORE_RECORD_LEN],
                          config_store_record_t *out)
{
    return config_store_unpack_ex(in, out, NULL);
}

bool config_store_unpack_ex(const uint8_t in[CONFIG_STORE_RECORD_LEN],
                             config_store_record_t *out,
                             config_store_reject_info_t *out_reject)
{
    // Written on every path below except the two early NULL-argument guards
    // (nothing to report -- a caller bug, not a record outcome) -- see this
    // function's own header comment (config_store.h) for "always written
    // when non-NULL". Cleared here, up front, so every early `return false`
    // below (bad magic, bad CRC, bad format_version) leaves it in the
    // documented "ordinary, not news" shape without having to repeat the
    // memset at each one.
    if (out_reject != NULL) {
        memset(out_reject, 0, sizeof(*out_reject));
    }

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
        // Unpack into a local scratch record, not directly into `*out`, so
        // the range-check below can still refuse the record wholesale
        // without violating this function's own documented contract that
        // `*out` is left COMPLETELY untouched on any false return (see this
        // file's header comment). `*out` is assigned only once every check
        // has already passed.
        config_store_record_t scratch;
        unpack_v2_fields(in, &scratch);
        // A valid CRC proves these bytes were not corrupted in flash -- it
        // says NOTHING about whether they mean what THIS build thinks they
        // mean. config_params_validate_ranges() previously ran only at
        // COMMIT_CONFIG, on a record this build itself just staged one
        // field at a time through config_params_set() (which already
        // range-checks each field as it arrives); it never ran here, on a
        // record that reached RAM by a completely different path -- e.g. a
        // record written by a different firmware build whose tc_source enum
        // grew a member this build does not recognise, or whose bytes rotted
        // into an in-range-looking-but-wrong value that still happens to sum
        // to the same CRC-32 as something valid. Re-running the same range/
        // finiteness check here, on every load, closes that gap: an
        // out-of-range field now makes THIS SLOT invalid (config_store_
        // find_latest() skips it, same as a bad magic/CRC/format_version),
        // so the caller falls back to the next-best slot or, if none, to
        // config_store_default() with calibration_missing forced true --
        // never to a guard evaluating a threshold nobody in this build's own
        // commissioning flow ever actually approved.
        // Capture WHICH field and WHY when out_reject was asked for -- this
        // is the one call site that turns a validation failure into
        // something a human can actually debug from a log line, rather than
        // a bare "false" that looks identical to an ordinary erased sector.
        // See this file's header comment on config_store_unpack_ex() and
        // config_store.h's "Load-time rejection diagnostics" block for why
        // this distinction (case 2: structurally-valid-but-refused) must
        // never be silent.
        const char *field = NULL;
        const char *rule = NULL;
        if (!config_params_validate_ranges(&scratch, &field, &rule, NULL)) {
            if (out_reject != NULL) {
                out_reject->rejected = true;
                out_reject->field = field;
                out_reject->rule = rule;
                out_reject->seq = scratch.seq;
            }
            return false;
        }
        *out = scratch;
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

        // Migrate forward into a local scratch record, not directly into
        // `*out` -- same "*out untouched on false" reasoning as the v2
        // branch above: start from the safe v2 default (every new field
        // gets its documented default, fields_set is 0 -- nothing the v1
        // record never had a chance to commission reads back as set), then
        // overlay exactly what v1 actually held.
        config_store_record_t scratch;
        config_store_default(&scratch);
        scratch.seq = get_u32_le(&in[REC_V1_OFF_SEQ]);
        // Same voltage-mode clamp as unpack_v2_fields() above -- a v1 record
        // predates this bound existing at all, so its tc_type byte gets no
        // less scrutiny than a v2 one.
        uint8_t v1_tc_type_byte = in[REC_V1_OFF_TC_TYPE];
        scratch.tc_type = (v1_tc_type_byte <= CONFIG_STORE_TC_TYPE_MAX_REAL)
                               ? v1_tc_type_byte
                               : CONFIG_STORE_DEFAULT_TC_TYPE;
        unpack_ct_cal(&in[REC_V1_OFF_CT_CAL], scratch.ct_cal, REC_V1_CT_CAL_CHANNEL_LEN);
        // calibration_missing is FORCED true regardless of what the v1
        // record held -- see this file's header comment (config_store.h)
        // for why: a migrated record was never commissioned against the
        // fields this pass added, so it must not be trusted as "fully
        // commissioned" just because v1's own narrower surface was.
        scratch.calibration_missing = true;
        // The record is now v2-shaped in RAM; a caller inspecting *out has
        // no way to tell "loaded as v2" from "migrated from v1" except via
        // calibration_missing above, which is the only distinction that
        // actually matters to any guard.
        scratch.format_version = CONFIG_STORE_FORMAT_VERSION;
        // Same load-time re-check as the v2 branch above, and for the same
        // reason: v1's own table never validated these bytes (v1 predates
        // this table entirely), and the migrated tc_type/ct_cal bytes came
        // from real, possibly-ancient flash -- a CRC-valid v1 record whose
        // preserved ct_cal gain/offset floats rotted into NaN/Inf, or whose
        // migration path someday grows to carry another field through
        // un-clamped, must not reach a guard either. tc_type itself is
        // already clamped above independent of this call (voltage-mode
        // bytes), so this is redundant for that one field specifically, but
        // it is the same backstop every other field on this path deserves.
        // Same out_reject capture as the v2 branch above, and for the same
        // reason -- a v1 record that migrates cleanly but then fails this
        // re-check is just as much a "found and refused" case as a v2 one.
        const char *field = NULL;
        const char *rule = NULL;
        if (!config_params_validate_ranges(&scratch, &field, &rule, NULL)) {
            if (out_reject != NULL) {
                out_reject->rejected = true;
                out_reject->field = field;
                out_reject->rule = rule;
                out_reject->seq = scratch.seq;
            }
            return false;
        }
        *out = scratch;
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
    out->safety_tc_installed = 1u;   // default: installed -- see config_store.h's
                                      // header comment on this field
    out->tc_offset_c = 0.0f;         // no correction -- explicit here even
                                      // though memset(0) above already gives
                                      // this, same "document the zero, don't
                                      // just rely on it" style as the section
                                      // 1 fields above
    out->ct_installed = 1u;          // default: installed = STRICT. The value
                                      // is only reachable once
                                      // CONFIG_STORE_SET_CT_INSTALLED is set
                                      // (it is ASKED, not defaulted), but if
                                      // anything ever does read it unguarded
                                      // it must read as the armed state.

    // Section 2: temperature guards -- CONFIG_REFERENCE.md section 2 defaults.
    out->firing_margin_c = 100.0f;
    out->overshoot_margin_c = 75.0f;
    out->overshoot_time_s = 120u;
    out->max_rate_c_per_min = CONFIG_STORE_DEFAULT_MAX_RATE_C_PER_MIN; // 2x max
                                      // shipped built-in profile ramp; see the
                                      // macro's doc comment in config_store.h.
                                      // Still genuinely UNSET (fields_set
                                      // clear) -- a real commissioning pass
                                      // can override it, e.g. to a tighter
                                      // bench-measured value.
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

    // max_expected_power_w left at memset(0) above -- IRRELEVANT, fields_set
    // gates it (same "zero is never mistaken for a real default" reasoning
    // as the section 1 fields above).

    // S14: i_normal_a[0..2] left at memset(0) above -- IRRELEVANT, gated by
    // CONFIG_STORE_SET_I_NORMAL_A_0/_1/_2, same reasoning as
    // max_expected_power_w. overcurrent_pct/overcurrent_time_s left at
    // memset(0) too -- genuinely "use the default", per this file's
    // effective_u16()/effective_f() substitution (150%/30s) -- deliberately
    // NOT written here explicitly, unlike i_present_a etc. above, so a
    // fresh record and a migrated-forward legacy record produce the exact
    // same in-RAM 0 for these two fields.

    // ct_topology: left at memset(0) above == CONFIG_STORE_CT_TOPOLOGY_
    // PER_ZONE (0) -- the safe default that reproduces every existing
    // behaviour. i_present_a_manual: left at memset(0) above == false, so a
    // fresh board's i_present_a is free to be auto-derived once i_normal_a
    // is commissioned (config_params_finalize_i_present_a()).

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
    return config_store_find_latest_ex(sector, out_rec, NULL);
}

size_t config_store_find_latest_ex(const uint8_t sector[SAFTYFW_CONFIG_STORE_FLASH_SIZE],
                                    config_store_record_t *out_rec,
                                    config_store_reject_info_t *out_reject)
{
    if (out_reject != NULL) {
        memset(out_reject, 0, sizeof(*out_reject));
    }

    if (sector == NULL || out_rec == NULL) {
        return CONFIG_STORE_NO_SLOT;
    }

    size_t best_slot = CONFIG_STORE_NO_SLOT;
    config_store_record_t best_rec;
    memset(&best_rec, 0, sizeof(best_rec));
    bool have_best = false;

    // Tracks the highest-seq REJECTED (structurally valid, range-refused)
    // slot seen so far, independent of `best_rec`/`have_best` above -- a
    // sector can hold both a good slot and a rejected one (e.g. slot 3 was
    // committed cleanly, slot 4's write was interrupted by a different
    // build's bad range, or vice versa in wear-order), and only matters to
    // report when NO good slot exists anywhere in the sector -- see this
    // function's own header comment.
    config_store_reject_info_t best_reject;
    memset(&best_reject, 0, sizeof(best_reject));
    bool have_rejected = false;

    for (size_t i = 0; i < CONFIG_STORE_SLOTS_PER_SECTOR; i++) {
        const uint8_t *rec_bytes = &sector[i * CONFIG_STORE_RECORD_LEN];
        config_store_record_t candidate;
        config_store_reject_info_t reject_info;
        if (!config_store_unpack_ex(rec_bytes, &candidate, &reject_info)) {
            if (reject_info.rejected && (!have_rejected || reject_info.seq > best_reject.seq)) {
                best_reject = reject_info;
                have_rejected = true;
            }
            continue; // erased, corrupt, too-new, or range-rejected -- skip, not an error
        }
        if (!have_best || candidate.seq > best_rec.seq) {
            best_rec = candidate;
            best_slot = i;
            have_best = true;
        }
    }

    if (!have_best) {
        // Only surface the rejection here -- a caller that DID find a good
        // slot has a trustworthy config regardless of what else was in the
        // sector, and out_reject was already zeroed above for that case.
        if (out_reject != NULL && have_rejected) {
            *out_reject = best_reject;
        }
        return CONFIG_STORE_NO_SLOT;
    }

    *out_rec = best_rec;
    return best_slot;
}

size_t config_store_find_latest_multi_ex(const uint8_t *sectors[SAFTYFW_CONFIG_STORE_NUM_SECTORS],
                                          size_t *out_sector_index,
                                          config_store_record_t *out_rec,
                                          config_store_reject_info_t *out_reject)
{
    if (out_reject != NULL) {
        memset(out_reject, 0, sizeof(*out_reject));
    }

    if (sectors == NULL || out_sector_index == NULL || out_rec == NULL) {
        return CONFIG_STORE_NO_SLOT;
    }

    size_t best_sector = 0;
    size_t best_slot = CONFIG_STORE_NO_SLOT;
    config_store_record_t best_rec;
    memset(&best_rec, 0, sizeof(best_rec));
    bool have_best = false;

    // Same "highest-seq rejection, only reported if no sector has a good
    // record" bookkeeping as config_store_find_latest_ex(), just carried
    // across both sectors -- a rejection in sector A must not be reported if
    // sector B has a perfectly good, newer record, and vice versa.
    config_store_reject_info_t best_reject;
    memset(&best_reject, 0, sizeof(best_reject));
    bool have_rejected = false;

    for (size_t s = 0; s < SAFTYFW_CONFIG_STORE_NUM_SECTORS; s++) {
        if (sectors[s] == NULL) {
            continue;
        }
        config_store_record_t sector_rec;
        config_store_reject_info_t sector_reject;
        size_t sector_slot = config_store_find_latest_ex(sectors[s], &sector_rec, &sector_reject);
        if (sector_slot == CONFIG_STORE_NO_SLOT) {
            if (sector_reject.rejected && (!have_rejected || sector_reject.seq > best_reject.seq)) {
                best_reject = sector_reject;
                have_rejected = true;
            }
            continue;
        }
        if (!have_best || sector_rec.seq > best_rec.seq) {
            best_rec = sector_rec;
            best_slot = sector_slot;
            best_sector = s;
            have_best = true;
        }
    }

    if (!have_best) {
        if (out_reject != NULL && have_rejected) {
            *out_reject = best_reject;
        }
        return CONFIG_STORE_NO_SLOT;
    }

    *out_sector_index = best_sector;
    *out_rec = best_rec;
    return best_slot;
}

config_store_write_plan_t config_store_plan_write(size_t current_sector_index,
                                                    size_t current_slot_index)
{
    config_store_write_plan_t plan;
    if (current_slot_index == CONFIG_STORE_NO_SLOT) {
        // Never written to either sector yet -- sector 0, slot 0, and (same
        // reasoning as config_store_next_write_needs_erase()'s own
        // CONFIG_STORE_NO_SLOT case) do not assume an erase is needed; a
        // fresh/blank sector may already be erased.
        plan.sector_index = 0;
        plan.slot_index = 0;
        plan.needs_erase = false;
        return plan;
    }

    if (!config_store_next_write_needs_erase(current_slot_index)) {
        // Room left in the current sector -- reuse the existing per-sector
        // round-robin logic unchanged.
        plan.sector_index = current_sector_index;
        plan.slot_index = config_store_next_write_slot(current_slot_index);
        plan.needs_erase = false;
        return plan;
    }

    // Current sector is full: switch to the other one. It must be erased
    // (it holds either nothing or a full sector of now-superseded records)
    // before slot 0 can be programmed -- but the sector holding the CURRENT
    // live record is never touched by this, which is the whole point.
    plan.sector_index = (current_sector_index == 0) ? 1u : 0u;
    plan.slot_index = 0;
    plan.needs_erase = true;
    return plan;
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
    return config_store_decide_write_ex(armed, false, false);
}

config_store_write_decision_t config_store_decide_write_ex(bool armed, bool tc_type_only_change,
                                                              bool heat_safe)
{
    // 2026-09-15 owner decision on the Opus review's F1: while ARMED, a
    // tc_type-only change is accepted when the Pico's own inputs say heat
    // is not currently being delivered (heat_safe) -- the Pico stays ARMED
    // throughout, never disarms or drops to GRACE. Every other change (a
    // record differing in ANY other field, or a tc_type change bundled with
    // other field changes) keeps the original unconditional ARMED refusal --
    // there is still no "this field is harmless" carve-out for anything
    // else, matching this codebase's general preference for a simple,
    // honest rule.
    if (!armed) {
        return CONFIG_STORE_WRITE_OK;
    }
    if (tc_type_only_change) {
        return heat_safe ? CONFIG_STORE_WRITE_OK : CONFIG_STORE_WRITE_REFUSED_ARMED_HEAT_ON;
    }
    return CONFIG_STORE_WRITE_REFUSED_ARMED;
}

bool config_store_only_tc_type_differs(const config_store_record_t *current,
                                        const config_store_record_t *candidate)
{
    if (current->tc_type == candidate->tc_type) {
        return false; // not a tc_type change at all -- nothing to relax for
    }

    // Pack both records and neutralize exactly the bytes a legitimate
    // tc_type-only change is allowed to touch: format_version/seq (always
    // caller-overwritten before a real write, never operator content),
    // tc_type itself, and fields_set's CONFIG_STORE_SET_TC_TYPE bit (a
    // board's first-ever commissioning of tc_type sets this bit alongside
    // the value). Comparing the rest of the packed bytes (excluding the
    // trailing CRC, which depends on all of the above) is more robust than
    // a hand-written field-by-field comparison: it cannot silently miss a
    // newly added struct field the way a manually maintained comparator
    // could.
    // static, not on-stack: config_store_write_ex() (this function's only
    // caller, config_store_flash.c) is documented single-writer -- link_task
    // is the only task that ever calls config_store_write()/_ex() (see that
    // function's own header comment) -- so two on-stack CONFIG_STORE_RECORD_LEN
    // buffers here is pure stack cost with no reentrancy to protect against.
    // Added 2026-09-15 (stack-budget fix, see check_saftyfw_task_stack_budgets.py):
    // this function alone was the single largest contributor to link_task's
    // deepest call-chain frame, and 1040 B of on-stack buffers here pushed
    // link_task over that checker's regsp-margin grading against its
    // declared stack. Not thread-safe against a second concurrent caller --
    // do not add one without revisiting this.
    static uint8_t a[CONFIG_STORE_RECORD_LEN];
    static uint8_t b[CONFIG_STORE_RECORD_LEN];
    config_store_pack(current, a);
    config_store_pack(candidate, b);

    put_u16_le(&a[REC_OFF_FORMAT_VERSION], 0);
    put_u16_le(&b[REC_OFF_FORMAT_VERSION], 0);
    put_u32_le(&a[REC_OFF_SEQ], 0);
    put_u32_le(&b[REC_OFF_SEQ], 0);
    a[REC_OFF_TC_TYPE] = 0;
    b[REC_OFF_TC_TYPE] = 0;
    uint16_t fields_set_a = get_u16_le(&a[REC_OFF_FIELDS_SET]) | CONFIG_STORE_SET_TC_TYPE;
    uint16_t fields_set_b = get_u16_le(&b[REC_OFF_FIELDS_SET]) | CONFIG_STORE_SET_TC_TYPE;
    put_u16_le(&a[REC_OFF_FIELDS_SET], fields_set_a);
    put_u16_le(&b[REC_OFF_FIELDS_SET], fields_set_b);

    return memcmp(a, b, REC_OFF_CRC) == 0;
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
        case CONFIG_STORE_WRITE_REFUSED_ARMED_HEAT_ON:
            return "refused: relay is ARMED and heat is on (or a firing may be running) -- "
                   "tc_type may only be changed while ARMED when heat is off";
        default:
            return "unknown";
    }
}

uint8_t config_store_seq_to_version(uint32_t seq)
{
    // See config_store.h's header comment on this function for the full
    // reasoning. seq == 0 is config_store_default()'s own seq -- the ONE
    // input that must keep mapping to 0, since that is what a board which
    // has never committed a real config (or booted with a blank/corrupt
    // sector) actually holds, and 0 is the sentinel config_store_confirm_
    // crc_ok() reads as "no valid config". Getting this direction wrong --
    // e.g. having seq == 0 map to some non-zero byte -- would be worse than
    // the bug this function fixes: it would make an UNCOMMITTED board's
    // config_version read back as "confirmed."
    //
    // Every seq >= 1 (config_store_write() always assigns cached_seq + 1u,
    // so the first real write is already seq == 1) maps into [1, 255]:
    // `(seq - 1u) % 255u` folds the wider counter down to [0, 254], and
    // `+ 1u` shifts that up to [1, 255] -- a range that, by construction,
    // can never be 0. Deliberately NOT `(uint8_t)(seq & 0xFFu)` (the old
    // truncation): that 256-wide mapping included 0 itself, so seq == 256,
    // 512, 768, ... (and any other multiple of 256) collided with the
    // "never loaded" sentinel above.
    if (seq == 0u) {
        return 0u;
    }
    return (uint8_t)(((seq - 1u) % 255u) + 1u);
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
