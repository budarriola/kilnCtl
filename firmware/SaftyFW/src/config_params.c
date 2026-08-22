// config_params.c -- see config_params.h. The id table below is
// docs/COMMISSIONING.md section 2.1 transcribed into code; if the two ever
// disagree, COMMISSIONING.md's own header rule applies ("if it disagrees
// with the code, the code wins" is CONFIG_REFERENCE.md's rule for itself,
// but the id table is COMMISSIONING.md's -- either way, a mismatch here is a
// bug in this file, not a documentation nit, since a wrong id/type pairing
// silently misroutes a commissioned value into the wrong field).
#include "config_params.h"

#include <math.h>
#include <string.h>

// u16-wire <-> u32-record clamp: every wire-U16 field in config_store.h is
// stored as uint32_t (seconds/ms counters use the wider type internally so
// arithmetic on them never has to think about u16 wraparound), but the wire
// tag for every one of them is U16 per COMMISSIONING.md section 2.1's table.
// SET_PARAM can only ever stage a value that fit in a u16 on the wire, so the
// down-cast on write never loses anything a legitimate sender could produce;
// the up-cast on read is exact by construction. The clamp exists only so a
// record field that somehow held a value in flash, or a value greater than 65535
// (which nothing in this codebase's own write paths ever produces) still
// reads back as a valid u16 rather than truncating silently -- defensive,
// not load-bearing.
static uint16_t clamp_u16(uint32_t v)
{
    return (v > 0xFFFFu) ? 0xFFFFu : (uint16_t)v;
}

// --- The id table (docs/COMMISSIONING.md section 2.1) -----------------------
// One entry per param_id this build recognises, in the exact order that
// document lists them -- used only by config_params_count()/_id_at() for
// GET_CONFIG_PAGE's enumeration; config_params_get()/_set() below dispatch on
// `id` directly via switch, not via this table, so the two are two
// independent statements of the same id set. A test that walks this table
// and confirms get()/set() both recognise every id it lists (test_config_store.c)
// is what catches the two ever drifting apart.
typedef struct {
    uint16_t id;
    uint8_t  type; // KILNLINK_PARAM_TYPE_*
} config_param_id_type_t;

static const config_param_id_type_t CONFIG_PARAM_TABLE[] = {
    { 0x0101u, KILNLINK_PARAM_TYPE_U8 },  // tc_source
    { 0x0102u, KILNLINK_PARAM_TYPE_U8 },  // borrowed_zone_index
    { 0x0103u, KILNLINK_PARAM_TYPE_U8 },  // tc_placement_mode
    { 0x0104u, KILNLINK_PARAM_TYPE_F32 }, // abs_max_temp_c
    { 0x0105u, KILNLINK_PARAM_TYPE_U8 },  // tc_type
    { 0x0106u, KILNLINK_PARAM_TYPE_U8 },  // ct_channel_map[0]
    { 0x0107u, KILNLINK_PARAM_TYPE_U8 },  // ct_channel_map[1]
    { 0x0108u, KILNLINK_PARAM_TYPE_U8 },  // ct_channel_map[2]
    { 0x0201u, KILNLINK_PARAM_TYPE_F32 }, // firing_margin_c
    { 0x0202u, KILNLINK_PARAM_TYPE_F32 }, // overshoot_margin_c
    { 0x0203u, KILNLINK_PARAM_TYPE_U16 }, // overshoot_time_s
    { 0x0204u, KILNLINK_PARAM_TYPE_F32 }, // max_rate_c_per_min
    { 0x0205u, KILNLINK_PARAM_TYPE_U16 }, // rate_window_s
    { 0x0206u, KILNLINK_PARAM_TYPE_U16 }, // blind_grace_s
    { 0x0207u, KILNLINK_PARAM_TYPE_U16 }, // frozen_window_s
    { 0x0208u, KILNLINK_PARAM_TYPE_F32 }, // tc_disagreement_c
    { 0x0209u, KILNLINK_PARAM_TYPE_U16 }, // tc_disagreement_time_s
    { 0x020Au, KILNLINK_PARAM_TYPE_F32 }, // tc_expected_offset_c
    { 0x020Bu, KILNLINK_PARAM_TYPE_F32 }, // cj_warn_c
    { 0x020Cu, KILNLINK_PARAM_TYPE_F32 }, // cj_max_c
    { 0x020Du, KILNLINK_PARAM_TYPE_U16 }, // cj_time_s
    { 0x020Eu, KILNLINK_PARAM_TYPE_U16 }, // borrowed_stale_s
    { 0x020Fu, KILNLINK_PARAM_TYPE_U16 }, // borrowed_stale_trip_s
    { 0x0210u, KILNLINK_PARAM_TYPE_U8 },  // borrowed_type_expected
    { 0x0301u, KILNLINK_PARAM_TYPE_F32 }, // i_present_a
    { 0x0302u, KILNLINK_PARAM_TYPE_U16 }, // zero_counts[0]
    { 0x0303u, KILNLINK_PARAM_TYPE_U16 }, // zero_counts[1]
    { 0x0304u, KILNLINK_PARAM_TYPE_U16 }, // zero_counts[2]
    { 0x0305u, KILNLINK_PARAM_TYPE_U16 }, // correlation_window_s
    { 0x0306u, KILNLINK_PARAM_TYPE_U16 }, // stuck_on_time_s
    { 0x0307u, KILNLINK_PARAM_TYPE_U16 }, // trip_verify_s
    { 0x0308u, KILNLINK_PARAM_TYPE_F32 }, // k_ct_v_per_a[0]
    { 0x0309u, KILNLINK_PARAM_TYPE_F32 }, // k_ct_v_per_a[1]
    { 0x030Au, KILNLINK_PARAM_TYPE_F32 }, // k_ct_v_per_a[2]
    { 0x030Bu, KILNLINK_PARAM_TYPE_F32 }, // gain[0]
    { 0x030Cu, KILNLINK_PARAM_TYPE_F32 }, // gain[1]
    { 0x030Du, KILNLINK_PARAM_TYPE_F32 }, // gain[2]
    { 0x030Eu, KILNLINK_PARAM_TYPE_F32 }, // mains_voltage_v
    { 0x030Fu, KILNLINK_PARAM_TYPE_U16 }, // power_window_s
    { 0x0310u, KILNLINK_PARAM_TYPE_F32 }, // ct_cal[0].gain
    { 0x0311u, KILNLINK_PARAM_TYPE_F32 }, // ct_cal[1].gain
    { 0x0312u, KILNLINK_PARAM_TYPE_F32 }, // ct_cal[2].gain
    { 0x0313u, KILNLINK_PARAM_TYPE_F32 }, // ct_cal[0].offset
    { 0x0314u, KILNLINK_PARAM_TYPE_F32 }, // ct_cal[1].offset
    { 0x0315u, KILNLINK_PARAM_TYPE_F32 }, // ct_cal[2].offset
    { 0x0316u, KILNLINK_PARAM_TYPE_BOOL }, // ct_cal[0].calibrated
    { 0x0317u, KILNLINK_PARAM_TYPE_BOOL }, // ct_cal[1].calibrated
    { 0x0318u, KILNLINK_PARAM_TYPE_BOOL }, // ct_cal[2].calibrated
    { 0x0401u, KILNLINK_PARAM_TYPE_U16 }, // context_max_age_s
    { 0x0402u, KILNLINK_PARAM_TYPE_U16 }, // link_timeout_s
    { 0x0403u, KILNLINK_PARAM_TYPE_U16 }, // link_dead_hard_s
    { 0x0404u, KILNLINK_PARAM_TYPE_U16 }, // mainfault_debounce_ms
    { 0x0405u, KILNLINK_PARAM_TYPE_U16 }, // telemetry_period_ms
    { 0x0501u, KILNLINK_PARAM_TYPE_U16 }, // startup_grace_s
    { 0x0502u, KILNLINK_PARAM_TYPE_U16 }, // estop_debounce_ms
    { 0x0503u, KILNLINK_PARAM_TYPE_U16 }, // watchdog_timeout_ms
    { 0x0504u, KILNLINK_PARAM_TYPE_U16 }, // config_check_period_s
};
#define CONFIG_PARAM_TABLE_LEN (sizeof(CONFIG_PARAM_TABLE) / sizeof(CONFIG_PARAM_TABLE[0]))

// Compile-time bound: link_task.c's GET_CONFIG_PAGE handler sizes a fixed
// stack array at 64 entries (comfortably above this table's real size) --
// if this table ever grows past that, the build must fail loudly here
// rather than that array silently truncating the last few ids off every
// page dump.
typedef char config_params_table_fits_64 [(CONFIG_PARAM_TABLE_LEN <= 64u) ? 1 : -1];

size_t config_params_count(void)
{
    return CONFIG_PARAM_TABLE_LEN;
}

bool config_params_id_at(size_t index, uint16_t *out_id, uint8_t *out_type)
{
    if (index >= CONFIG_PARAM_TABLE_LEN) {
        return false;
    }
    if (out_id) {
        *out_id = CONFIG_PARAM_TABLE[index].id;
    }
    if (out_type) {
        *out_type = CONFIG_PARAM_TABLE[index].type;
    }
    return true;
}

bool config_params_get(const config_store_record_t *rec, uint16_t id, uint8_t *out_type,
                        kilnlink_param_value_t *out_value)
{
    if (!rec || !out_type || !out_value) {
        return false;
    }

    switch (id) {
    case 0x0101u: *out_type = KILNLINK_PARAM_TYPE_U8;  out_value->u8_val = rec->tc_source; return true;
    case 0x0102u: *out_type = KILNLINK_PARAM_TYPE_U8;  out_value->u8_val = rec->borrowed_zone_index; return true;
    case 0x0103u: *out_type = KILNLINK_PARAM_TYPE_U8;  out_value->u8_val = rec->tc_placement_mode; return true;
    case 0x0104u: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->abs_max_temp_c; return true;
    case 0x0105u: *out_type = KILNLINK_PARAM_TYPE_U8;  out_value->u8_val = rec->tc_type; return true;
    case 0x0106u: *out_type = KILNLINK_PARAM_TYPE_U8;  out_value->u8_val = rec->ct_channel_map[0]; return true;
    case 0x0107u: *out_type = KILNLINK_PARAM_TYPE_U8;  out_value->u8_val = rec->ct_channel_map[1]; return true;
    case 0x0108u: *out_type = KILNLINK_PARAM_TYPE_U8;  out_value->u8_val = rec->ct_channel_map[2]; return true;

    case 0x0201u: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->firing_margin_c; return true;
    case 0x0202u: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->overshoot_margin_c; return true;
    case 0x0203u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->overshoot_time_s); return true;
    case 0x0204u: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->max_rate_c_per_min; return true;
    case 0x0205u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->rate_window_s); return true;
    case 0x0206u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->blind_grace_s); return true;
    case 0x0207u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->frozen_window_s); return true;
    case 0x0208u: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->tc_disagreement_c; return true;
    case 0x0209u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->tc_disagreement_time_s); return true;
    case 0x020Au: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->tc_expected_offset_c; return true;
    case 0x020Bu: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->cj_warn_c; return true;
    case 0x020Cu: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->cj_max_c; return true;
    case 0x020Du: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->cj_time_s); return true;
    case 0x020Eu: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->borrowed_stale_s); return true;
    case 0x020Fu: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->borrowed_stale_trip_s); return true;
    case 0x0210u: *out_type = KILNLINK_PARAM_TYPE_U8;  out_value->u8_val = rec->borrowed_type_expected; return true;

    case 0x0301u: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->i_present_a; return true;
    case 0x0302u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = rec->zero_counts[0]; return true;
    case 0x0303u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = rec->zero_counts[1]; return true;
    case 0x0304u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = rec->zero_counts[2]; return true;
    case 0x0305u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->correlation_window_s); return true;
    case 0x0306u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->stuck_on_time_s); return true;
    case 0x0307u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->trip_verify_s); return true;
    case 0x0308u: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->k_ct_v_per_a[0]; return true;
    case 0x0309u: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->k_ct_v_per_a[1]; return true;
    case 0x030Au: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->k_ct_v_per_a[2]; return true;
    case 0x030Bu: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->gain[0]; return true;
    case 0x030Cu: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->gain[1]; return true;
    case 0x030Du: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->gain[2]; return true;
    case 0x030Eu: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->mains_voltage_v; return true;
    case 0x030Fu: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->power_window_s); return true;
    case 0x0310u: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->ct_cal[0].gain; return true;
    case 0x0311u: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->ct_cal[1].gain; return true;
    case 0x0312u: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->ct_cal[2].gain; return true;
    case 0x0313u: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->ct_cal[0].offset; return true;
    case 0x0314u: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->ct_cal[1].offset; return true;
    case 0x0315u: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->ct_cal[2].offset; return true;
    case 0x0316u: *out_type = KILNLINK_PARAM_TYPE_BOOL; out_value->bool_val = rec->ct_cal[0].calibrated ? 1u : 0u; return true;
    case 0x0317u: *out_type = KILNLINK_PARAM_TYPE_BOOL; out_value->bool_val = rec->ct_cal[1].calibrated ? 1u : 0u; return true;
    case 0x0318u: *out_type = KILNLINK_PARAM_TYPE_BOOL; out_value->bool_val = rec->ct_cal[2].calibrated ? 1u : 0u; return true;

    case 0x0401u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->context_max_age_s); return true;
    case 0x0402u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->link_timeout_s); return true;
    case 0x0403u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->link_dead_hard_s); return true;
    case 0x0404u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->mainfault_debounce_ms); return true;
    case 0x0405u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->telemetry_period_ms); return true;

    case 0x0501u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->startup_grace_s); return true;
    case 0x0502u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->estop_debounce_ms); return true;
    case 0x0503u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->watchdog_timeout_ms); return true;
    case 0x0504u: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->config_check_period_s); return true;

    default:
        return false; // unknown/removed/future id -- see this file's header comment
    }
}

// --- Range validation (CONFIG_REFERENCE.md sections 1-5) --------------------
// CHECK_TYPE (below) refuses a value whose WIRE TYPE is wrong; these refuse a
// value whose type is right but whose CONTENT cannot be. Two different
// classes of value are rejected, on two different justifications:
//
//   - Enums / documented finite domains (CHECK_U8_MAX): CONFIG_REFERENCE.md
//     section 1 gives tc_source, tc_placement_mode, tc_type and
//     borrowed_type_expected a closed, named set of values, and
//     borrowed_zone_index a documented "0-2" range. A value outside either is
//     not a value this build's guards have any defined behaviour for --
//     tc_source = 7 or borrowed_zone_index = 200 must be refused here, not
//     stored and left for whichever guard reads it next to misinterpret.
//   - Physically impossible floats (CHECK_F32_FINITE / CHECK_F32_NONNEG): a
//     NaN or +/-Inf anywhere in this table is the single most dangerous
//     value this surface can carry -- every `x > threshold` comparison a
//     guard makes against a NaN threshold is false, so a NaN abs_max_temp_c
//     is a ceiling that silently never trips, and a NaN i_present_a is a
//     load-active threshold that can never be crossed either direction.
//     CHECK_F32_FINITE rejects NaN and both infinities for EVERY float field,
//     no exceptions. CHECK_F32_NONNEG additionally rejects a negative value
//     for the two fields CONFIG_REFERENCE.md section 1/3 name as physically
//     bounded below by zero -- a negative absolute temperature ceiling
//     (abs_max_temp_c, section 1) and a negative current threshold
//     (i_present_a, section 3) are both impossible quantities, not merely
//     unusual ones (safety_guards.c's own history -- ROADMAP.md -- has an
//     example of exactly this bug: a finite but negative ceiling passing an
//     `isfinite(x) && x < abs_max` check with no lower bound). Every OTHER
//     float field in this table (firing_margin_c, tc_disagreement_c,
//     tc_expected_offset_c, cj_warn_c/cj_max_c, k_ct_v_per_a, gain,
//     mains_voltage_v, ct_cal gain/offset, ...) is deliberately left
//     UNBOUNDED beyond finiteness: CONFIG_REFERENCE.md does not document a
//     sign or magnitude constraint for any of them (tc_expected_offset_c is
//     explicitly signed; a margin or gain of unusual magnitude is merely
//     unusual commissioning, not an impossible one), and inventing a bound
//     the document does not state is exactly the "tight sensible bound"
//     COMMISSIONING.md's own discipline forbids.
#define CHECK_U8_MAX(maxval) do { if (value.u8_val > (uint8_t)(maxval)) { return false; } } while (0)
#define CHECK_F32_FINITE() do { if (!isfinite(value.f32_val)) { return false; } } while (0)
#define CHECK_F32_NONNEG() do { if (!isfinite(value.f32_val) || value.f32_val < 0.0f) { return false; } } while (0)

bool config_params_set(config_store_record_t *rec, uint16_t id, uint8_t type,
                        kilnlink_param_value_t value)
{
    if (!rec) {
        return false;
    }

// A field's own wire type (matching config_params_get()'s switch above,
// verbatim) must match what the sender claims -- see this file's header
// comment on config_params_set() for why a type mismatch is refused exactly
// like an unknown id, not coerced.
#define CHECK_TYPE(expected) do { if (type != (expected)) { return false; } } while (0)

    switch (id) {
    case 0x0101u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  CHECK_U8_MAX(CONFIG_STORE_TC_SOURCE_BOTH); rec->tc_source = value.u8_val; rec->fields_set |= CONFIG_STORE_SET_TC_SOURCE; return true;
    case 0x0102u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  CHECK_U8_MAX(2u); /* CONFIG_REFERENCE.md sec1: "0-2" */ rec->borrowed_zone_index = value.u8_val; rec->fields_set |= CONFIG_STORE_SET_BORROWED_ZONE_INDEX; return true;
    case 0x0103u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  CHECK_U8_MAX(CONFIG_STORE_TC_PLACEMENT_EXTERNAL_OVERHEAT); rec->tc_placement_mode = value.u8_val; rec->fields_set |= CONFIG_STORE_SET_TC_PLACEMENT_MODE; return true;
    case 0x0104u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_NONNEG(); rec->abs_max_temp_c = value.f32_val; rec->fields_set |= CONFIG_STORE_SET_ABS_MAX_TEMP_C; return true;
    case 0x0105u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  CHECK_U8_MAX(7u); /* MAX31856_TC_TYPE_T (max31856.h); this file stays dependency-free of that header, same reason config_store.h gives -- see this file's own header comment on that isolation */ rec->tc_type = value.u8_val; return true; // NOT fields_set-gated -- keeps its own compiled default (config_store.h)
    // Per-channel bookkeeping only -- the group bit (CONFIG_STORE_SET_
    // CT_CHANNEL_MAP) is NOT set here. See config_store.h's header comment
    // on these four bits and config_params_finalize_ct_channel_map() below:
    // the group is derived at COMMIT_CONFIG time from all three of these,
    // never asserted piecemeal by an individual SET_PARAM.
    case 0x0106u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  rec->ct_channel_map[0] = value.u8_val; rec->fields_set |= CONFIG_STORE_SET_CT_CHANNEL_MAP_0; return true;
    case 0x0107u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  rec->ct_channel_map[1] = value.u8_val; rec->fields_set |= CONFIG_STORE_SET_CT_CHANNEL_MAP_1; return true;
    case 0x0108u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  rec->ct_channel_map[2] = value.u8_val; rec->fields_set |= CONFIG_STORE_SET_CT_CHANNEL_MAP_2; return true;

    case 0x0201u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->firing_margin_c = value.f32_val; return true;
    case 0x0202u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->overshoot_margin_c = value.f32_val; return true;
    case 0x0203u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->overshoot_time_s = value.u16_val; return true;
    case 0x0204u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->max_rate_c_per_min = value.f32_val; rec->fields_set |= CONFIG_STORE_SET_MAX_RATE_C_PER_MIN; return true;
    case 0x0205u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->rate_window_s = value.u16_val; return true;
    case 0x0206u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->blind_grace_s = value.u16_val; return true;
    case 0x0207u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->frozen_window_s = value.u16_val; return true;
    case 0x0208u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->tc_disagreement_c = value.f32_val; return true;
    case 0x0209u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->tc_disagreement_time_s = value.u16_val; return true;
    case 0x020Au: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->tc_expected_offset_c = value.f32_val; return true;
    case 0x020Bu: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->cj_warn_c = value.f32_val; return true;
    case 0x020Cu: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->cj_max_c = value.f32_val; return true;
    case 0x020Du: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->cj_time_s = value.u16_val; return true;
    case 0x020Eu: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->borrowed_stale_s = value.u16_val; return true;
    case 0x020Fu: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->borrowed_stale_trip_s = value.u16_val; return true;
    case 0x0210u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  CHECK_U8_MAX(7u); /* MAX31856_TC_TYPE_T -- see 0x0105 above */ rec->borrowed_type_expected = value.u8_val; return true;

    case 0x0301u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_NONNEG(); rec->i_present_a = value.f32_val; return true;
    case 0x0302u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->zero_counts[0] = value.u16_val; return true;
    case 0x0303u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->zero_counts[1] = value.u16_val; return true;
    case 0x0304u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->zero_counts[2] = value.u16_val; return true;
    case 0x0305u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->correlation_window_s = value.u16_val; return true;
    case 0x0306u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->stuck_on_time_s = value.u16_val; return true;
    case 0x0307u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->trip_verify_s = value.u16_val; return true;
    case 0x0308u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->k_ct_v_per_a[0] = value.f32_val; return true;
    case 0x0309u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->k_ct_v_per_a[1] = value.f32_val; return true;
    case 0x030Au: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->k_ct_v_per_a[2] = value.f32_val; return true;
    case 0x030Bu: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->gain[0] = value.f32_val; return true;
    case 0x030Cu: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->gain[1] = value.f32_val; return true;
    case 0x030Du: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->gain[2] = value.f32_val; return true;
    case 0x030Eu: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->mains_voltage_v = value.f32_val; rec->fields_set |= CONFIG_STORE_SET_MAINS_VOLTAGE_V; return true;
    case 0x030Fu: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->power_window_s = value.u16_val; return true;
    case 0x0310u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->ct_cal[0].gain = value.f32_val; return true;
    case 0x0311u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->ct_cal[1].gain = value.f32_val; return true;
    case 0x0312u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->ct_cal[2].gain = value.f32_val; return true;
    case 0x0313u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->ct_cal[0].offset = value.f32_val; return true;
    case 0x0314u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->ct_cal[1].offset = value.f32_val; return true;
    case 0x0315u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->ct_cal[2].offset = value.f32_val; return true;
    case 0x0316u: CHECK_TYPE(KILNLINK_PARAM_TYPE_BOOL); rec->ct_cal[0].calibrated = (value.bool_val != 0u); return true;
    case 0x0317u: CHECK_TYPE(KILNLINK_PARAM_TYPE_BOOL); rec->ct_cal[1].calibrated = (value.bool_val != 0u); return true;
    case 0x0318u: CHECK_TYPE(KILNLINK_PARAM_TYPE_BOOL); rec->ct_cal[2].calibrated = (value.bool_val != 0u); return true;

    case 0x0401u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->context_max_age_s = value.u16_val; return true;
    case 0x0402u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->link_timeout_s = value.u16_val; return true;
    case 0x0403u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->link_dead_hard_s = value.u16_val; return true;
    case 0x0404u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->mainfault_debounce_ms = value.u16_val; return true;
    case 0x0405u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->telemetry_period_ms = value.u16_val; return true;

    case 0x0501u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->startup_grace_s = value.u16_val; return true;
    case 0x0502u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->estop_debounce_ms = value.u16_val; return true;
    case 0x0503u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->watchdog_timeout_ms = value.u16_val; return true;
    case 0x0504u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->config_check_period_s = value.u16_val; return true;

    default:
        return false; // unknown/removed/future id -- rec left untouched
    }

#undef CHECK_TYPE
#undef CHECK_U8_MAX
#undef CHECK_F32_FINITE
#undef CHECK_F32_NONNEG
}

// Re-checks every bound config_params_set() enforces at SET_PARAM time,
// against the record AS A WHOLE, right before COMMIT_CONFIG would hand it to
// config_store_write(). This is deliberate belt-and-suspenders, not
// redundant paranoia: config_params_set() is the only INTENDED way a value
// gets into `rec`, but it is not the only POSSIBLE one -- `rec` can also
// arrive here already populated by config_store_default() (compiled
// defaults, always in range) or by a v1-format record migrated forward by
// config_store_unpack() (old bytes this file's own table never validated,
// since v1 predates this table). A future code path that ever builds a
// `rec` some other way inherits this same safety net for free. Every check
// here is a duplicate, byte-for-byte in effect, of the corresponding
// CHECK_U8_MAX/CHECK_F32_FINITE/CHECK_F32_NONNEG call in
// config_params_set() above -- see that function's own header comment for
// which CONFIG_REFERENCE.md line justifies each one; this function does not
// repeat that justification, only the check.
static bool config_params_validate_ranges(const config_store_record_t *rec,
                                           const char **out_field, const char **out_rule)
{
#define RANGE_FAIL(field_name, rule_text) do { \
        if (out_field) *out_field = (field_name); \
        if (out_rule) *out_rule = (rule_text); \
        return false; \
    } while (0)
#define RANGE_U8_MAX(field, maxval, field_name) do { \
        if ((field) > (maxval)) { \
            RANGE_FAIL((field_name), "value exceeds this field's documented/enum range"); \
        } \
    } while (0)
#define RANGE_F32_FINITE(field, field_name) do { \
        if (!isfinite(field)) { \
            RANGE_FAIL((field_name), "NaN/Inf is never a valid value for this field"); \
        } \
    } while (0)
#define RANGE_F32_NONNEG(field, field_name) do { \
        if (!isfinite(field) || (field) < 0.0f) { \
            RANGE_FAIL((field_name), "this field is a physically non-negative quantity"); \
        } \
    } while (0)

    RANGE_U8_MAX(rec->tc_source, CONFIG_STORE_TC_SOURCE_BOTH, "tc_source");
    RANGE_U8_MAX(rec->borrowed_zone_index, 2u, "borrowed_zone_index");
    RANGE_U8_MAX(rec->tc_placement_mode, CONFIG_STORE_TC_PLACEMENT_EXTERNAL_OVERHEAT, "tc_placement_mode");
    RANGE_U8_MAX(rec->tc_type, 7u, "tc_type");
    RANGE_U8_MAX(rec->borrowed_type_expected, 7u, "borrowed_type_expected");

    RANGE_F32_NONNEG(rec->abs_max_temp_c, "abs_max_temp_c");
    RANGE_F32_NONNEG(rec->i_present_a, "i_present_a");

    RANGE_F32_FINITE(rec->firing_margin_c, "firing_margin_c");
    RANGE_F32_FINITE(rec->overshoot_margin_c, "overshoot_margin_c");
    RANGE_F32_FINITE(rec->max_rate_c_per_min, "max_rate_c_per_min");
    RANGE_F32_FINITE(rec->tc_disagreement_c, "tc_disagreement_c");
    RANGE_F32_FINITE(rec->tc_expected_offset_c, "tc_expected_offset_c");
    RANGE_F32_FINITE(rec->cj_warn_c, "cj_warn_c");
    RANGE_F32_FINITE(rec->cj_max_c, "cj_max_c");
    RANGE_F32_FINITE(rec->k_ct_v_per_a[0], "k_ct_v_per_a[0]");
    RANGE_F32_FINITE(rec->k_ct_v_per_a[1], "k_ct_v_per_a[1]");
    RANGE_F32_FINITE(rec->k_ct_v_per_a[2], "k_ct_v_per_a[2]");
    RANGE_F32_FINITE(rec->gain[0], "gain[0]");
    RANGE_F32_FINITE(rec->gain[1], "gain[1]");
    RANGE_F32_FINITE(rec->gain[2], "gain[2]");
    RANGE_F32_FINITE(rec->mains_voltage_v, "mains_voltage_v");
    RANGE_F32_FINITE(rec->ct_cal[0].gain, "ct_cal[0].gain");
    RANGE_F32_FINITE(rec->ct_cal[1].gain, "ct_cal[1].gain");
    RANGE_F32_FINITE(rec->ct_cal[2].gain, "ct_cal[2].gain");
    RANGE_F32_FINITE(rec->ct_cal[0].offset, "ct_cal[0].offset");
    RANGE_F32_FINITE(rec->ct_cal[1].offset, "ct_cal[1].offset");
    RANGE_F32_FINITE(rec->ct_cal[2].offset, "ct_cal[2].offset");

    return true;

#undef RANGE_FAIL
#undef RANGE_U8_MAX
#undef RANGE_F32_FINITE
#undef RANGE_F32_NONNEG
}

bool config_params_validate(const config_store_record_t *rec, const char **out_field,
                             const char **out_rule)
{
    if (!rec) {
        if (out_field) *out_field = "rec";
        if (out_rule) *out_rule = "NULL record";
        return false;
    }

    if (!config_params_validate_ranges(rec, out_field, out_rule)) {
        return false;
    }

    // CONFIG_REFERENCE.md section 1 / COMMISSIONING.md section 2: only a
    // meaningful contradiction when BOTH fields have actually been staged --
    // an unset field cannot contradict anything, and refusing a commit that
    // only touches unrelated fields because tc_placement_mode still holds
    // its zero-initialised default would block ordinary incremental
    // commissioning for no reason.
    bool tc_source_set =
        config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_TC_SOURCE);
    bool tc_placement_set =
        config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_TC_PLACEMENT_MODE);
    if (tc_source_set && tc_placement_set &&
        rec->tc_source == CONFIG_STORE_TC_SOURCE_BORROWED_ZONE &&
        rec->tc_placement_mode == CONFIG_STORE_TC_PLACEMENT_EXTERNAL_OVERHEAT) {
        if (out_field) {
            *out_field = "tc_placement_mode";
        }
        if (out_rule) {
            *out_rule = "CONFIG_REFERENCE.md sec1: tc_placement_mode is forced to "
                        "CHAMBER_AGREED when tc_source is BORROWED_ZONE -- a staged "
                        "EXTERNAL_OVERHEAT with BORROWED_ZONE is a contradiction, "
                        "rejected rather than silently reconciled";
        }
        return false;
    }

    return true;
}

void config_params_finalize_ct_channel_map(config_store_record_t *rec)
{
    if (!rec) {
        return;
    }
    // CONFIG_REFERENCE.md section 1: the CT channel map is confirmed by ONE
    // indivisible one-relay-at-a-time pass (CURRENT_SENSE.md section 5 step
    // 2), so the group bit only ever gets newly set here when all three
    // per-channel bits are present -- two of three leaves it unset, on
    // purpose, every time this is called (see test_config_store.c's
    // "two-of-three channels" case for the proof this can actually fail to
    // confirm). Monotonic like every other fields_set bit: an incomplete
    // triple never clears a group bit a PRIOR commit already earned.
    uint16_t need = (uint16_t)(CONFIG_STORE_SET_CT_CHANNEL_MAP_0 | CONFIG_STORE_SET_CT_CHANNEL_MAP_1 |
                                CONFIG_STORE_SET_CT_CHANNEL_MAP_2);
    if (config_store_field_is_set(&rec->fields_set, need)) {
        rec->fields_set |= CONFIG_STORE_SET_CT_CHANNEL_MAP;
    }
}

bool config_params_all_required_set(const config_store_record_t *rec)
{
    if (!rec) {
        return false;
    }
    uint16_t required = (uint16_t)(CONFIG_STORE_SET_TC_SOURCE | CONFIG_STORE_SET_BORROWED_ZONE_INDEX |
                                    CONFIG_STORE_SET_TC_PLACEMENT_MODE | CONFIG_STORE_SET_ABS_MAX_TEMP_C |
                                    CONFIG_STORE_SET_CT_CHANNEL_MAP | CONFIG_STORE_SET_MAX_RATE_C_PER_MIN |
                                    CONFIG_STORE_SET_MAINS_VOLTAGE_V);
    return config_store_field_is_set(&rec->fields_set, required);
}
