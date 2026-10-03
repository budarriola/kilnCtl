// config_store.c -- see config_store.h.
#include "config_store.h"

#include "discrete_pin_policy.h" // DISCRETE_PIN_POLICY_ESTOP_ACTIVE_* -- the
                                // encoding this record's estop_active_level
                                // byte carries; one shared definition rather
                                // than a duplicated 0/1 convention

#include "tasks/log_task.h" // log_task_log() -- config_store_only_tc_type_differs()'s reentrancy trip-wire logs instead of aborting (2026-09-15, F3)
#include <string.h>

#include "config_params.h" // config_params_validate_ranges() -- load-time re-check, see config_store_unpack()
#include "crc32.h" // bootloader/ -- same CRC-32 used for metadata records

// --- v3 byte layout (little-endian, same convention as bootloader/metadata.c)
//
// v3 (CT_CHANNEL_MASK.md step 2) changed the record in two ways: it
// widened fields_set from 2 to 4 bytes IN PLACE at offset 12, which pushed
// every field from tc_source onward up by exactly 2, and it carved
// zone_ct_channel[3] out of the front of the reserved tail. Every v2 offset
// is preserved verbatim in the frozen REC_V2_OFF_* table below, which is
// what the v2-to-v3 migration reads. The CRC region is [0, 504) at BOTH
// versions -- same bytes, different meanings -- which is exactly why
// format_version, not the CRC and not the record length, is what
// discriminates them.
//
// Offset  Size  Field
//      0     4  magic
//      4     2  format_version
//      6     2  reserved0 (0)
//      8     4  seq
//     12     4  fields_set (bitmask, CONFIG_STORE_SET_*) -- u32 since v3
//     16     1  tc_source
//     17     1  borrowed_zone_index
//     18     1  tc_placement_mode
//     19     4  abs_max_temp_c (f32 LE)
//     23     1  tc_type
//     24     3  ct_channel_map[3]
//     27     1  calibration_missing (0/1)
//     28     4  firing_margin_c
//     32     4  overshoot_margin_c
//     36     4  overshoot_time_s (u32 LE)
//     40     4  max_rate_c_per_min
//     44     4  rate_window_s
//     48     4  blind_grace_s
//     52     4  frozen_window_s
//     56     4  tc_disagreement_c
//     60     4  tc_disagreement_time_s
//     64     4  tc_expected_offset_c
//     68     4  cj_warn_c
//     72     4  cj_max_c
//     76     4  cj_time_s
//     80     4  borrowed_stale_s
//     84     4  borrowed_stale_trip_s
//     88     1  borrowed_type_expected
//     89     4  i_present_a
//     93     6  zero_counts[3] (u16 LE x3)
//     99     4  correlation_window_s
//    103     4  stuck_on_time_s
//    107     4  trip_verify_s
//    111    12  k_ct_v_per_a[3] (f32 LE x3)
//    123    12  gain[3] (f32 LE x3)
//    135     4  mains_voltage_v
//    139     4  power_window_s
//    143     4  context_max_age_s
//    147     4  link_timeout_s
//    151     4  link_dead_hard_s
//    155     4  mainfault_debounce_ms
//    159     4  telemetry_period_ms
//    163     4  startup_grace_s
//    167     4  estop_debounce_ms
//    171     4  watchdog_timeout_ms
//    175     4  config_check_period_s
//    179    27  ct_cal: 3 channels x 9 B each (calibrated u8 + gain f32 LE +
//               offset f32 LE) -- unchanged shape/offset-within-block from v1
//    206     1  safety_tc_installed_marker (u8) -- carved out of the former
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
//    207     4  max_expected_power_w (f32 LE); gated by
//               CONFIG_STORE_SET_MAX_EXPECTED_POWER_W, carved out of the
//               front of the former 299 B reserved block, same convention as
//               REC_OFF_SAFETY_TC_INSTALLED above
//    211     4  i_normal_a[3][0] -- S14 (COMMISSIONING_UX.md sec 3.3), carved
//               out of the former 295 B reserved block, same convention as
//               REC_OFF_MAX_EXPECTED_POWER_W above
//    215     8  i_normal_a[3][1..2] (f32 LE x2)
//    223     2  overcurrent_pct (u16 LE)
//    225     4  overcurrent_time_s (u32 LE)
//    229     1  ct_installed_marker (u8) -- same positive-assertion marker
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
//    230     1  ct_topology (u8, 0=per_zone/1=summed) -- CT_COMMISSIONING_
//               PLAN.md step 3, param 0x031F. Plain 0/1 byte; any other
//               value (legacy 0x00, erased-flash 0xFF) decodes as per_zone.
//    231     1  i_present_a_manual (u8, 0/1) -- true iff i_present_a (offset
//               87) was set directly via SET_PARAM rather than auto-derived
//               from i_normal_a. Same "only 1 means true" decode as above.
//    232     4  tc_offset_c (f32 LE) -- safety board's own TC calibration
//               correction, owner request 2026-09-08; see config_store.h's
//               struct comment
//    236     1  estop_active_level (u8, 0=active-high/1=active-low) -- param
//               0x0212, owner decision 2026-09-08. Plain 0/1 byte, same
//               convention as ct_topology at 230: only the value 1 selects
//               ACTIVE_LOW, so a legacy record's 0x00 and erased flash's
//               0xFF both decode to ACTIVE_HIGH -- which is both the bench's
//               real wiring and the fail-safe polarity. An unreadable
//               configuration must never select the polarity that cannot
//               detect a broken E-stop line.
//    237     3  zone_ct_channel[3] (u8 x3) -- which CT channel each zone is
//               read on, params 0x0320-0x0322, CT_CHANNEL_MASK.md
//               step 2. Gated by CONFIG_STORE_SET_ZONE_CT_CHANNEL; when
//               that bit is clear the map is derived from ct_topology at
//               230 instead (config_store_effective_zone_ct_channel()).
//               Carved out of the front of the former 267 B reserved
//               block, same convention as every field above.
//    240   264  reserved, 0xFF-filled (headroom for a future field)
//    504     4  record_crc32, over bytes [0, 504)
//    508     4  reserved, 0xFF-filled (pad to CONFIG_STORE_RECORD_LEN)
//    512  total = CONFIG_STORE_RECORD_LEN
#define REC_OFF_MAGIC                  0u
#define REC_OFF_FORMAT_VERSION         4u
#define REC_OFF_SEQ                    8u
#define REC_OFF_FIELDS_SET             12u
#define REC_OFF_TC_SOURCE              16u
#define REC_OFF_BORROWED_ZONE_INDEX    17u
#define REC_OFF_TC_PLACEMENT_MODE      18u
#define REC_OFF_ABS_MAX_TEMP_C         19u
#define REC_OFF_TC_TYPE                23u
#define REC_OFF_CT_CHANNEL_MAP         24u
#define REC_CT_CHANNEL_MAP_LEN         3u
#define REC_OFF_CALIBRATION_MISSING    27u
#define REC_OFF_FIRING_MARGIN_C        28u
#define REC_OFF_OVERSHOOT_MARGIN_C     32u
#define REC_OFF_OVERSHOOT_TIME_S       36u
#define REC_OFF_MAX_RATE_C_PER_MIN     40u
#define REC_OFF_RATE_WINDOW_S          44u
#define REC_OFF_BLIND_GRACE_S          48u
#define REC_OFF_FROZEN_WINDOW_S        52u
#define REC_OFF_TC_DISAGREEMENT_C      56u
#define REC_OFF_TC_DISAGREEMENT_TIME_S 60u
#define REC_OFF_TC_EXPECTED_OFFSET_C   64u
#define REC_OFF_CJ_WARN_C              68u
#define REC_OFF_CJ_MAX_C               72u
#define REC_OFF_CJ_TIME_S              76u
#define REC_OFF_BORROWED_STALE_S       80u
#define REC_OFF_BORROWED_STALE_TRIP_S  84u
#define REC_OFF_BORROWED_TYPE_EXPECTED 88u
#define REC_OFF_I_PRESENT_A            89u
#define REC_OFF_ZERO_COUNTS            93u
#define REC_OFF_CORRELATION_WINDOW_S   99u
#define REC_OFF_STUCK_ON_TIME_S        103u
#define REC_OFF_TRIP_VERIFY_S          107u
#define REC_OFF_K_CT_V_PER_A           111u
#define REC_OFF_GAIN                   123u
#define REC_OFF_MAINS_VOLTAGE_V        135u
#define REC_OFF_POWER_WINDOW_S         139u
#define REC_OFF_CONTEXT_MAX_AGE_S      143u
#define REC_OFF_LINK_TIMEOUT_S         147u
#define REC_OFF_LINK_DEAD_HARD_S       151u
#define REC_OFF_MAINFAULT_DEBOUNCE_MS  155u
#define REC_OFF_TELEMETRY_PERIOD_MS    159u
#define REC_OFF_STARTUP_GRACE_S        163u
#define REC_OFF_ESTOP_DEBOUNCE_MS      167u
#define REC_OFF_WATCHDOG_TIMEOUT_MS    171u
#define REC_OFF_CONFIG_CHECK_PERIOD_S  175u
#define REC_OFF_CT_CAL                 179u
#define REC_CT_CAL_CHANNEL_LEN         9u /* calibrated u8(1) + gain f32(4) + offset f32(4) */
#define REC_OFF_SAFETY_TC_INSTALLED \
    (REC_OFF_CT_CAL + CONFIG_STORE_CT_CAL_NUM_CHANNELS * REC_CT_CAL_CHANNEL_LEN) /* 206 */
#define REC_OFF_MAX_EXPECTED_POWER_W   (REC_OFF_SAFETY_TC_INSTALLED + 1u) /* 207 */
// S14 (COMMISSIONING_UX.md section 3.3), carved out of the reserved tail --
// OQ2 resolved: 16 B fit comfortably inside the 295 B reserved block with no
// format_version bump, so every already-committed v2 record keeps loading
// exactly as it did (calibration_missing untouched by this addition).
#define REC_OFF_I_NORMAL_A             (REC_OFF_MAX_EXPECTED_POWER_W + 4u) /* 211 */
#define REC_OFF_OVERCURRENT_PCT        (REC_OFF_I_NORMAL_A + 3u * 4u)      /* 223 */
#define REC_OFF_OVERCURRENT_TIME_S     (REC_OFF_OVERCURRENT_PCT + 2u)      /* 225 */
#define REC_OFF_CT_INSTALLED           (REC_OFF_OVERCURRENT_TIME_S + 4u)  /* 229 */
// ct_topology/i_present_a_manual (CT_COMMISSIONING_PLAN.md step 3), carved
// out of the front of the reserved tail same as every field above -- plain
// 0/1 bytes, not the 0xA5-marker convention: this build only ever writes 0
// or 1 (config_params.c's CHECK_U8_MAX(1u)/CHECK_TYPE(BOOL)), and BOTH
// decoders below only special-case the value 1 (SUMMED / manual==true) --
// any other byte, including an old record's never-written 0xFF or a legacy
// record's zeroed 0x00, already falls through to the safe default (per_zone
// / auto-derive) without needing a distinct improbable sentinel.
#define REC_OFF_CT_TOPOLOGY            (REC_OFF_CT_INSTALLED + 1u)        /* 230 */
#define REC_OFF_I_PRESENT_A_MANUAL     (REC_OFF_CT_TOPOLOGY + 1u)         /* 231 */
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
#define REC_OFF_TC_OFFSET_C            (REC_OFF_I_PRESENT_A_MANUAL + 1u)  /* 232 */
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
#define REC_OFF_ESTOP_ACTIVE_LEVEL     (REC_OFF_TC_OFFSET_C + 4u)         /* 236 */
// zone_ct_channel (CT_CHANNEL_MASK.md step 2), carved out of the front
// of the reserved tail like every field above. Raw 0-2 bytes, NOT a marker
// convention: the field is meaningless unless CONFIG_STORE_SET_ZONE_CT_
// CHANNEL is set, and that bit cannot be set on any record written before
// v3 existed, so there is no legacy byte pattern to defend against here.
// unpack additionally refuses the whole array (clearing the bit) if any byte
// is out of range, so a CRC-valid but garbled record falls back to the
// ct_topology-derived map rather than to a channel id nothing is wired to.
#define REC_OFF_ZONE_CT_CHANNEL        (REC_OFF_ESTOP_ACTIVE_LEVEL + 1u)  /* 237 */
#define REC_ZONE_CT_CHANNEL_LEN        3u
#define REC_OFF_RESERVED               (REC_OFF_ZONE_CT_CHANNEL + REC_ZONE_CT_CHANNEL_LEN) /* 240 */
#define REC_RESERVED_LEN               264u

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

// --- v2 (legacy) byte layout -- kept ONLY for the v2-to-v3 migration in
// config_store_unpack_ex(). Frozen: real flash written by the shipped v2
// firmware exists in this shape, and this table's job is to keep reading it
// correctly forever, never to track whatever v3 (REC_OFF_* above) becomes.
// The full v2 field list is the REC_OFF_* table above with every offset from
// REC_V2_OFF_TC_SOURCE through estop_active_level two bytes lower; only the
// constants the migration actually needs are frozen as symbols, because
// those are what make the field block a single contiguous run (see the v2
// branch of config_store_unpack_ex()) and freezing offsets nothing reads
// would be freezing a claim nothing checks.
#define REC_V2_OFF_FIELDS_SET          12u
#define REC_V2_FIELDS_SET_LEN          2u  /* u16 at v2, u32 at v3 */
#define REC_V2_OFF_TC_SOURCE           14u
#define REC_V2_OFF_RESERVED            235u
#define REC_V2_RESERVED_LEN            269u
#define REC_V2_OFF_CRC                 504u

// The v2 field block [REC_V2_OFF_TC_SOURCE, REC_V2_OFF_RESERVED) maps
// byte-for-byte onto v3's [REC_OFF_TC_SOURCE, REC_OFF_ZONE_CT_CHANNEL): the
// only v3 change below offset 235 was fields_set growing by 2 bytes, so the
// whole run shifted by exactly that much and nothing inside it was reordered
// or resized. This assert is what keeps that true -- a future field inserted
// into the middle of the v3 table would break it at compile time rather than
// silently making the migration copy the wrong bytes.
typedef char config_store_v2_block_shift_check
    [((REC_V2_OFF_RESERVED - REC_V2_OFF_TC_SOURCE) ==
      (REC_OFF_ZONE_CT_CHANNEL - REC_OFF_TC_SOURCE) &&
      (REC_OFF_TC_SOURCE - REC_V2_OFF_TC_SOURCE) == 2u &&
      REC_V2_OFF_CRC == REC_OFF_CRC) ? 1 : -1];

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
    put_u32_le(&out[REC_OFF_FIELDS_SET], rec->fields_set);

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

    memcpy(&out[REC_OFF_ZONE_CT_CHANNEL], rec->zone_ct_channel, REC_ZONE_CT_CHANNEL_LEN);

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
    out->fields_set = get_u32_le(&in[REC_OFF_FIELDS_SET]);

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

    // zone_ct_channel (CT_CHANNEL_MASK.md step 2). Gated by
    // CONFIG_STORE_SET_ZONE_CT_CHANNEL, which no pre-v3 record can have set,
    // so the raw bytes are only ever trusted on a record this build's own
    // commissioning flow wrote. The extra range check below is belt and
    // braces against a CRC-valid but garbled record: rather than hand a
    // guard a channel id nothing is wired to, clear the bit, which makes
    // config_store_effective_zone_ct_channel() fall back to the
    // ct_topology-derived map -- the same "an unreadable configuration lands
    // on the conservative behaviour" direction every other decode in this
    // function takes.
    memcpy(out->zone_ct_channel, &in[REC_OFF_ZONE_CT_CHANNEL], REC_ZONE_CT_CHANNEL_LEN);
    for (unsigned z = 0; z < REC_ZONE_CT_CHANNEL_LEN; z++) {
        if (out->zone_ct_channel[z] > 2u) {
            out->fields_set &= ~(uint32_t)(CONFIG_STORE_SET_ZONE_CT_CHANNEL |
                                            CONFIG_STORE_SET_ZONE_CT_CHANNEL_0 |
                                            CONFIG_STORE_SET_ZONE_CT_CHANNEL_1 |
                                            CONFIG_STORE_SET_ZONE_CT_CHANNEL_2);
            // Replace the bytes as well, not just the bits: config_params_
            // validate_ranges() runs on every loaded record and refuses an
            // out-of-range zone_ct_channel byte outright, so leaving the
            // garbage in place would turn a field this build is perfectly
            // able to fall back on into a whole-record rejection.
            config_store_derive_zone_ct_channel(out->ct_topology, out->zone_ct_channel);
            break;
        }
    }

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

    if (version == CONFIG_STORE_FORMAT_VERSION_V2) {
        // Legacy v2 layout (CT_CHANNEL_MASK.md step 2). Its CRC covers
        // [0, REC_V2_OFF_CRC) -- numerically the same region v3 uses, over
        // different contents, which is why format_version and not the CRC is
        // what got us into this branch.
        uint32_t stored_crc = get_u32_le(&in[REC_V2_OFF_CRC]);
        uint32_t computed_crc = bootloader_crc32(in, REC_V2_OFF_CRC);
        if (stored_crc != computed_crc) {
            return false; // corrupted, or a torn write caught mid-program
        }

        // Rewrite the v2 bytes into v3 shape, then decode them with the ONE
        // v3 decoder, rather than duplicating ~60 lines of field decoding
        // that would then be free to drift away from unpack_v2_fields(). The
        // rewrite itself is driven entirely by the frozen REC_V2_OFF_*
        // constants above and is compile-time-checked by
        // config_store_v2_block_shift_check.
        uint8_t v3[CONFIG_STORE_RECORD_LEN];
        memset(v3, 0xFF, sizeof(v3));
        memcpy(v3, in, REC_V2_OFF_FIELDS_SET); // magic, format_version, reserved0, seq
        // Zero-extend the old 16-bit fields_set into the new 32-bit field:
        // bits 0-15 keep their exact meanings and positions, so no remapping
        // is needed -- and bit 16 (ZONE_CT_CHANNEL) lands CLEAR, which is
        // precisely the "this record predates the field, derive it from
        // ct_topology" state the decode below depends on.
        put_u32_le(&v3[REC_OFF_FIELDS_SET],
                   (uint32_t)get_u16_le(&in[REC_V2_OFF_FIELDS_SET]));
        memcpy(&v3[REC_OFF_TC_SOURCE], &in[REC_V2_OFF_TC_SOURCE],
               REC_V2_OFF_RESERVED - REC_V2_OFF_TC_SOURCE);
        memcpy(&v3[REC_OFF_RESERVED], &in[REC_V2_OFF_RESERVED], REC_RESERVED_LEN);

        config_store_record_t scratch;
        unpack_v2_fields(v3, &scratch);
        // zone_ct_channel: derived from the migrated ct_topology byte, per
        // CT_CHANNEL_MASK.md's old-record rule. The bytes at the new
        // offset are meaningless on a v2 record (they were reserved fill)
        // and the gating bit stays clear, so every reader that goes through
        // config_store_effective_zone_ct_channel() would derive the same map
        // anyway -- filling the array here as well just means a debugger or
        // a log dump of this record shows the map that is actually in force.
        config_store_derive_zone_ct_channel(scratch.ct_topology, scratch.zone_ct_channel);
        scratch.format_version = CONFIG_STORE_FORMAT_VERSION;
        // Deliberately NOT forcing calibration_missing = true, unlike the v1
        // branch below. v1 predated the entire commissioning surface, so a
        // migrated v1 record genuinely had never been commissioned against
        // it. A v2 record was commissioned against every field v3 has except
        // zone_ct_channel, and that one field has a correct, fully-specified
        // derivation from ct_topology -- nothing about it is unknown.
        // Forcing recommissioning here would be a false claim that the
        // operator's existing answers are missing, and would disarm guards
        // on a board that is, in fact, configured.
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

    // zone_ct_channel: the identity map, the per-zone default that
    // reproduces every existing behaviour -- and deliberately NOT the
    // memset(0) {0,0,0} the rest of this function leans on, which would
    // claim all three zones share channel 0. The gating bit stays clear
    // either way, so this value is what a debugger shows and what
    // config_store_effective_zone_ct_channel() would derive anyway from the
    // PER_ZONE default below; writing it explicitly keeps those two agreeing
    // without either side having to know the other's rule.
    out->zone_ct_channel[0] = 0u;
    out->zone_ct_channel[1] = 1u;
    out->zone_ct_channel[2] = 2u;

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

config_store_write_decision_t config_store_decide_write_ex(bool armed, bool narrow_change_only,
                                                              bool heat_safe)
{
    // 2026-09-15 owner decision on the Opus review's F1 (widened 2026-09-18,
    // see config_store_only_ct_cal_differs()'s header comment): while
    // ARMED, a narrow change -- today either a tc_type-only change or a
    // single channel's zero_counts/k_ct_v_per_a-only change -- is accepted
    // when the Pico's own inputs say heat is not currently being delivered
    // (heat_safe) -- the Pico stays ARMED throughout, never disarms or
    // drops to GRACE. Every other change (a record differing in ANY other
    // field, or a narrow-eligible field change bundled with other field
    // changes) keeps the original unconditional ARMED refusal -- there is
    // still no "this field is harmless" carve-out for anything else,
    // matching this codebase's general preference for a simple, honest
    // rule.
    if (!armed) {
        return CONFIG_STORE_WRITE_OK;
    }
    if (narrow_change_only) {
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
    // 2026-09-15 (Opus re-review LOW): the comment above documents single-
    // writer BY CONVENTION only -- nothing previously checked it at runtime.
    // Assert it instead of trusting the comment: a second overlapping call
    // (this function re-entered before the first call's memcmp() below has
    // read `a`/`b`) would corrupt both callers' comparisons silently, since
    // they share these two static buffers. `s_call_in_progress` is not
    // itself thread-safe against a true concurrent entry from a second core/
    // ISR -- it is a straight-line reentrancy trip-wire, adequate here
    // because the real hazard this guards against is a future caller added
    // from a different task, not a race that needs a lock to detect.
    static volatile bool s_call_in_progress = false;
    if (s_call_in_progress) {
        // 2026-09-15 (Opus adversarial re-review, F3): this was an abort()
        // until this fix. abort() resets the whole RP2040 -- on this board
        // that drops K4 (heat-enable output) and leaves the safety processor
        // unarmed until it reboots and clears GRACE, which is exactly what
        // the owner's hard line forbids ("there should never be a way that
        // the pico is not armed"), traded for a condition this function's
        // own header comment says is not reachable today (single caller,
        // single task) and that `s_call_in_progress` -- a plain volatile
        // bool, not a lock -- could not reliably detect anyway if it ever
        // did happen concurrently. Fail closed instead: log it and report
        // "not a tc_type-only change", which routes the caller into the
        // existing unconditional ARMED refusal (see the non-tc_type-only
        // branch above) instead of resetting the chip.
        //
        // Not host-tested, and the reason matters: it is NOT that a
        // file-local static is unreachable from a test (the 2026-09-15
        // re-review of d43e96b2 corrected that claim -- the house pattern
        // of #include-ing the production .c directly into a test is used
        // elsewhere in this repo, and `s_call_in_progress` is in any case
        // function-local, so even that would not reach it). The real
        // reason is that there is NO RE-ENTRY SEAM: everything executed
        // between setting and clearing the flag -- config_store_pack(),
        // the little-endian accessors, memcmp() -- is same-translation-
        // unit or libc, so no test can interpose a call that re-enters
        // this function. Reaching this branch would require adding a seam
        // to production code purely to test a defence-in-depth trip-wire,
        // which is not worth the production risk. If a seam ever appears
        // here for another reason, test this branch then.
        log_task_log(LOG_LEVEL_ERROR, "config_store",
                     "config_store_only_tc_type_differs re-entered -- refusing "
                     "as not-tc_type-only, chip stays armed");
        return false;
    }
    s_call_in_progress = true;
    config_store_pack(current, a);
    config_store_pack(candidate, b);

    put_u16_le(&a[REC_OFF_FORMAT_VERSION], 0);
    put_u16_le(&b[REC_OFF_FORMAT_VERSION], 0);
    put_u32_le(&a[REC_OFF_SEQ], 0);
    put_u32_le(&b[REC_OFF_SEQ], 0);
    a[REC_OFF_TC_TYPE] = 0;
    b[REC_OFF_TC_TYPE] = 0;
    uint32_t fields_set_a = get_u32_le(&a[REC_OFF_FIELDS_SET]) | CONFIG_STORE_SET_TC_TYPE;
    uint32_t fields_set_b = get_u32_le(&b[REC_OFF_FIELDS_SET]) | CONFIG_STORE_SET_TC_TYPE;
    put_u32_le(&a[REC_OFF_FIELDS_SET], fields_set_a);
    put_u32_le(&b[REC_OFF_FIELDS_SET], fields_set_b);

    bool equal = memcmp(a, b, REC_OFF_CRC) == 0;
    s_call_in_progress = false;
    return equal;
}

// 2026-09-18 CT-auto-zero deadlock fix -- see this function's header comment
// (config_store.h) for the safety argument. zero_counts[]/k_ct_v_per_a[]
// carry no fields_set gating bit (unlike tc_type's CONFIG_STORE_SET_TC_TYPE),
// so unlike config_store_only_tc_type_differs() above there is no bit to
// neutralize -- only the two per-channel fields themselves.
//
// Own static scratch buffers and own reentrancy trip-wire, deliberately not
// shared with config_store_only_tc_type_differs()'s -- see that function's
// comment for why `static` (this call chain's stack-budget grading) and why
// a plain volatile bool is an adequate straight-line trip-wire here (single
// caller, config_store_write_ex(), same as that function).
bool config_store_only_ct_cal_differs(const config_store_record_t *current,
                                       const config_store_record_t *candidate)
{
    // First, cheaply find whether exactly one channel's zero_counts/
    // k_ct_v_per_a pair differs -- comparing the struct fields directly is
    // simpler and just as correct as packing for this part, since neither
    // field carries a fields_set gate to normalize.
    int changed_channel = -1;
    unsigned changed_count = 0u;
    for (size_t ch = 0; ch < CONFIG_STORE_CT_CAL_NUM_CHANNELS; ch++) {
        bool differs = (current->zero_counts[ch] != candidate->zero_counts[ch]) ||
                       (current->k_ct_v_per_a[ch] != candidate->k_ct_v_per_a[ch]);
        if (differs) {
            changed_count++;
            changed_channel = (int)ch;
        }
    }
    if (changed_count != 1u) {
        // Neither field changed anywhere (nothing to relax for), or more
        // than one channel's pair changed -- config_store_write_ex()'s
        // real caller (the CT auto-zero HTTP flow) only ever proposes one
        // channel per commit, so two-or-more-channels-at-once is refused,
        // not narrowed for.
        return false;
    }

    static uint8_t a[CONFIG_STORE_RECORD_LEN];
    static uint8_t b[CONFIG_STORE_RECORD_LEN];
    static volatile bool s_ct_cal_call_in_progress = false;
    if (s_ct_cal_call_in_progress) {
        // Same fail-closed reasoning as config_store_only_tc_type_differs()'s
        // own re-entrancy trip-wire above: no real re-entry seam exists
        // today (single caller, single task), so this is defence in depth,
        // not a reachable path -- refusing as "not a narrow change" routes
        // the caller into the existing unconditional ARMED refusal instead
        // of trusting two shared static buffers a second concurrent call
        // could be mutating.
        log_task_log(LOG_LEVEL_ERROR, "config_store",
                     "config_store_only_ct_cal_differs re-entered -- refusing "
                     "as not-ct-cal-only, chip stays armed");
        return false;
    }
    s_ct_cal_call_in_progress = true;
    config_store_pack(current, a);
    config_store_pack(candidate, b);

    put_u16_le(&a[REC_OFF_FORMAT_VERSION], 0);
    put_u16_le(&b[REC_OFF_FORMAT_VERSION], 0);
    put_u32_le(&a[REC_OFF_SEQ], 0);
    put_u32_le(&b[REC_OFF_SEQ], 0);
    size_t ch = (size_t)changed_channel;
    put_u16_le(&a[REC_OFF_ZERO_COUNTS + ch * 2u], 0);
    put_u16_le(&b[REC_OFF_ZERO_COUNTS + ch * 2u], 0);
    put_f32_le(&a[REC_OFF_K_CT_V_PER_A + ch * 4u], 0.0f);
    put_f32_le(&b[REC_OFF_K_CT_V_PER_A + ch * 4u], 0.0f);

    bool equal = memcmp(a, b, REC_OFF_CRC) == 0;
    s_ct_cal_call_in_progress = false;
    return equal;
}

uint32_t config_store_record_crc(const config_store_record_t *rec)
{
    uint8_t packed[CONFIG_STORE_RECORD_LEN];
    config_store_pack(rec, packed);
    return get_u32_le(&packed[REC_OFF_CRC]);
}

bool config_store_ram_integrity_ok(const config_store_record_t *rec, uint32_t expected_crc)
{
    // See config_store.h's header comment for why this compares against a
    // CRC captured at the record's last legitimate install, not against
    // flash. config_store_is_volatile_dirty() DOES exist and does tell a
    // live volatile install (config_store_write_volatile()) apart from
    // flash -- that is not the reason for the CRC-at-install design here.
    // The real reason: this function's job is corruption detection, which
    // has to work identically whether the cache currently holds a durable
    // record or a legitimately-diverged volatile one -- a raw RAM-vs-flash
    // byte comparison would flag every live volatile install as a false
    // positive, since "diverged from flash" is exactly what a volatile
    // install is SUPPOSED to look like. Comparing against a CRC captured at
    // the record's own last legitimate install (durable or volatile, both
    // go through the same seqlock-write choke point) sidesteps that
    // entirely: only a corruption that happens AFTER that point -- bit rot,
    // an overrun, a wild write -- can make this return false, regardless of
    // whether is_volatile_dirty() would currently read true or false.
    return config_store_record_crc(rec) == expected_crc;
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
        case CONFIG_STORE_WRITE_REFUSED_ARMED_HEAT_UNKNOWN:
            return "refused: relay is ARMED and this write path did not determine heat state -- "
                   "tc_type may only be changed while ARMED via a caller that confirms heat is off";
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
