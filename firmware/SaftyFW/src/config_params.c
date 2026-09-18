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

#include "kilnlink/kilnlink_commit_config_rejected.h"

// config_params_id_for_field_name()'s CONFIG_PARAMS_NO_PARAM_ID sentinel
// must be byte-identical to the wire sentinel it stands in for -- a mismatch
// would make link_task.c's "no specific field" case silently name a real
// (and wrong) param_id on the wire.
typedef char config_params_no_param_id_matches_wire_sentinel
    [(CONFIG_PARAMS_NO_PARAM_ID == KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID) ? 1 : -1];

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
    { 0x0109u, KILNLINK_PARAM_TYPE_U8 },  // ct_installed -- next unallocated id
                                           // in this section-1 group, deliberately
                                           // adjacent to the three ct_channel_map
                                           // ids it gates
    { 0x010Au, KILNLINK_PARAM_TYPE_F32 }, // tc_offset_c -- owner request
                                           // 2026-09-08; next unallocated id
                                           // after 0x0109, deliberately
                                           // adjacent to tc_type (0x0105) in
                                           // this same section-1 group
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
    // 0x0207 is already frozen_window_s (S11) -- the task brief that minted
    // this param asked for 0x0207, which collides with an existing entry in
    // this same table (see the entry three lines above at 0x0206/0x0207).
    // Following the brief's number here would silently misroute
    // safety_tc_installed writes into frozen_window_s's slot (or vice
    // versa) -- exactly the "wrong id/type pairing silently misroutes a
    // commissioned value" bug this file's own header comment warns about.
    // Minted 0x0211 instead: the next unallocated id after 0x0210 in this
    // same section-1-style commissioning group, following the same
    // "append after the last used id in the field's own doc section" rule
    // every other id in this table already follows.
    { 0x0211u, KILNLINK_PARAM_TYPE_U8 },  // safety_tc_installed
    // estop_active_level, owner decision 2026-09-08. Minted 0x0212 by the
    // same "append after the last used id in this section-1-style
    // commissioning group" rule 0x0211 above followed.
    { 0x0212u, KILNLINK_PARAM_TYPE_U8 },  // estop_active_level
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
    { 0x0319u, KILNLINK_PARAM_TYPE_F32 }, // max_expected_power_w -- ROADMAP.md
                                           // M12; next unallocated id after
                                           // 0x0318 in this section-3 group
    { 0x031Au, KILNLINK_PARAM_TYPE_F32 }, // i_normal_a[0] -- S14, NEW, COMMISSIONING_UX.md sec 3.3
    { 0x031Bu, KILNLINK_PARAM_TYPE_F32 }, // i_normal_a[1] -- S14, NEW
    { 0x031Cu, KILNLINK_PARAM_TYPE_F32 }, // i_normal_a[2] -- S14, NEW
    { 0x031Du, KILNLINK_PARAM_TYPE_U16 }, // overcurrent_pct -- S14, NEW
    { 0x031Eu, KILNLINK_PARAM_TYPE_U16 }, // overcurrent_time_s -- S14, NEW
    { 0x031Fu, KILNLINK_PARAM_TYPE_U8 },  // ct_topology -- CT_COMMISSIONING_PLAN.md
                                           // step 3, NEW; next unallocated id
                                           // after 0x031E in this section-3 group
    // zone_ct_channel[0..2] -- CT_CHANNEL_MASK_PLAN.md step 2, NEW; next
    // three unallocated ids after 0x031F in this section-3 group.
    { 0x0320u, KILNLINK_PARAM_TYPE_U8 },  // zone_ct_channel[0]
    { 0x0321u, KILNLINK_PARAM_TYPE_U8 },  // zone_ct_channel[1]
    { 0x0322u, KILNLINK_PARAM_TYPE_U8 },  // zone_ct_channel[2]
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
// stack array to match (comfortably above this table's real size) -- if this
// table ever grows past that, the build must fail loudly here rather than
// that array silently truncating the last few ids off every page dump.
//
// Raised 64 -> 72 by the ct_installed (0x0109) addition, which took the table
// to exactly 65 entries and tripped this assert -- working as designed. The
// paired array in link_task.c was raised in the same edit; the two numbers
// are not independent and must never be changed apart.
//
// The three zone_ct_channel ids (0x0320-0x0322, CT_CHANNEL_MASK_PLAN.md
// step 2) took the table to 68 entries, still inside the same 72 bound --
// no change to this assert or to link_task.c's paired array was needed.
typedef char config_params_table_fits_72 [(CONFIG_PARAM_TABLE_LEN <= 72u) ? 1 : -1];

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
    case 0x0109u: *out_type = KILNLINK_PARAM_TYPE_U8;  out_value->u8_val = rec->ct_installed; return true;
    case 0x010Au: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->tc_offset_c; return true;
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
    case 0x0211u: *out_type = KILNLINK_PARAM_TYPE_U8;  out_value->u8_val = rec->safety_tc_installed; return true;
    case 0x0212u: *out_type = KILNLINK_PARAM_TYPE_U8;  out_value->u8_val = rec->estop_active_level; return true;

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
    case 0x0319u: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->max_expected_power_w; return true;

    case 0x031Au: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->i_normal_a[0]; return true;
    case 0x031Bu: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->i_normal_a[1]; return true;
    case 0x031Cu: *out_type = KILNLINK_PARAM_TYPE_F32; out_value->f32_val = rec->i_normal_a[2]; return true;
    case 0x031Du: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->overcurrent_pct); return true;
    case 0x031Eu: *out_type = KILNLINK_PARAM_TYPE_U16; out_value->u16_val = clamp_u16(rec->overcurrent_time_s); return true;
    case 0x031Fu: *out_type = KILNLINK_PARAM_TYPE_U8;  out_value->u8_val = rec->ct_topology; return true;
    // zone_ct_channel[0..2] -- the RAW stored bytes, deliberately not the
    // effective map config_store_effective_zone_ct_channel() would derive.
    // GET_PARAM's job is to show what is staged/stored so an operator can
    // see their own answers back; config_params_is_set() on the same id is
    // what says whether those bytes mean anything yet.
    case 0x0320u: *out_type = KILNLINK_PARAM_TYPE_U8;  out_value->u8_val = rec->zone_ct_channel[0]; return true;
    case 0x0321u: *out_type = KILNLINK_PARAM_TYPE_U8;  out_value->u8_val = rec->zone_ct_channel[1]; return true;
    case 0x0322u: *out_type = KILNLINK_PARAM_TYPE_U8;  out_value->u8_val = rec->zone_ct_channel[2]; return true;

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

bool config_params_is_set(const config_store_record_t *rec, uint16_t id)
{
    if (!rec) {
        return false;
    }
    switch (id) {
    // The eleven fields_set-gated ids (config_store.h's CONFIG_STORE_SET_*
    // bits) -- ct_channel_map's three wire ids each check their OWN
    // per-channel bit, not the derived CONFIG_STORE_SET_CT_CHANNEL_MAP group
    // bit, so an operator can see which specific channel still needs
    // confirming rather than a page-wide "not commissioned yet" for all
    // three at once (config_store.h's own comment on why the group bit is
    // DERIVED, never staged directly).
    case 0x0101u: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_TC_SOURCE);
    case 0x0102u: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_BORROWED_ZONE_INDEX);
    case 0x0103u: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_TC_PLACEMENT_MODE);
    case 0x0104u: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_ABS_MAX_TEMP_C);
    case 0x0105u: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_TC_TYPE);
    case 0x0109u: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_CT_INSTALLED);
    case 0x0106u: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_CT_CHANNEL_MAP_0);
    case 0x0107u: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_CT_CHANNEL_MAP_1);
    case 0x0108u: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_CT_CHANNEL_MAP_2);
    case 0x0204u: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_MAX_RATE_C_PER_MIN);
    case 0x030Eu: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_MAINS_VOLTAGE_V);
    case 0x0319u: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_MAX_EXPECTED_POWER_W);
    case 0x031Au: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_I_NORMAL_A_0);
    case 0x031Bu: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_I_NORMAL_A_1);
    case 0x031Cu: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_I_NORMAL_A_2);
    // Per-zone bits, not the derived group bit -- same reasoning as
    // ct_channel_map's three ids above: an operator must be able to see
    // WHICH zone is still unanswered.
    case 0x0320u: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_ZONE_CT_CHANNEL_0);
    case 0x0321u: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_ZONE_CT_CHANNEL_1);
    case 0x0322u: return config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_ZONE_CT_CHANNEL_2);
    default: {
        // Every other id in CONFIG_PARAM_TABLE has a real compiled-in
        // default (CONFIG_REFERENCE.md secs 2-5's threshold fields) --
        // config_params_get() below returns false only for an id this
        // table does not recognise at all; any id it accepts here is
        // "known, and always reported set".
        uint8_t discard_type = 0;
        kilnlink_param_value_t discard_value;
        memset(&discard_value, 0, sizeof(discard_value));
        return config_params_get(rec, id, &discard_type, &discard_value);
    }
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
//   - Strictly-positive floats (CHECK_F32_POS, added ROADMAP.md M12): a
//     single field, abs_max_temp_c, is stricter still than CHECK_F32_NONNEG.
//     Before M12 a committed abs_max_temp_c of 0.0 meant "S1 never trips" --
//     a real, reachable "no limit" state. M12's owner amendment is explicit
//     that no "no limit" state is offered any more: a kiln maximum
//     temperature is either UNSET (fields_set clear, the field must not be
//     trusted by anything that respects the bit) or SET to a real positive
//     ceiling -- there is no third, "set to no-limit" state, so 0 (and any
//     negative value) must be refused at SET_PARAM/COMMIT_CONFIG exactly
//     like a NaN, not merely discouraged. CHECK_F32_NONNEG stays exactly as
//     it was for i_present_a (0 A is a perfectly normal "no idle load
//     present" reading, not a disabled-guard sentinel) -- only
//     abs_max_temp_c moves to CHECK_F32_POS.
#define CHECK_U8_MAX(maxval) do { if (value.u8_val > (uint8_t)(maxval)) { return false; } } while (0)
#define CHECK_F32_FINITE() do { if (!isfinite(value.f32_val)) { return false; } } while (0)
#define CHECK_F32_NONNEG() do { if (!isfinite(value.f32_val) || value.f32_val < 0.0f) { return false; } } while (0)
#define CHECK_F32_POS() do { if (!isfinite(value.f32_val) || value.f32_val <= 0.0f) { return false; } } while (0)
// 0.0f (disabled/not-commissioned) always passes; any nonzero value must
// land in [min, max] -- see config_store.h's CONFIG_STORE_MAX_RATE_C_PER_MIN_
// FLOOR/_CEILING comment for why this exists and what it protects against.
#define CHECK_F32_RANGE_OR_ZERO(min, max) do { \
        if (!isfinite(value.f32_val)) { return false; } \
        if (value.f32_val != 0.0f && (value.f32_val < (min) || value.f32_val > (max))) { return false; } \
    } while (0)

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
    case 0x0104u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_POS(); rec->abs_max_temp_c = value.f32_val; rec->fields_set |= CONFIG_STORE_SET_ABS_MAX_TEMP_C; return true;
    case 0x0105u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  CHECK_U8_MAX(7u); /* MAX31856_TC_TYPE_T (max31856.h); this file stays dependency-free of that header, same reason config_store.h gives -- see this file's own header comment on that isolation */ rec->tc_type = value.u8_val; rec->fields_set |= CONFIG_STORE_SET_TC_TYPE; return true; // 2026-08-24: NOW fields_set-gated too -- tc_type still keeps its own compiled default (config_store.h), the bit exists so a real commissioning write can be told apart from that default (see CONFIG_STORE_SET_TC_TYPE's comment)
    // Per-channel bookkeeping only -- the group bit (CONFIG_STORE_SET_
    // CT_CHANNEL_MAP) is NOT set here. See config_store.h's header comment
    // on these four bits and config_params_finalize_ct_channel_map() below:
    // the group is derived at COMMIT_CONFIG time from all three of these,
    // never asserted piecemeal by an individual SET_PARAM.
    case 0x0106u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  rec->ct_channel_map[0] = value.u8_val; rec->fields_set |= CONFIG_STORE_SET_CT_CHANNEL_MAP_0; return true;
    case 0x0107u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  rec->ct_channel_map[1] = value.u8_val; rec->fields_set |= CONFIG_STORE_SET_CT_CHANNEL_MAP_1; return true;
    case 0x0108u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  rec->ct_channel_map[2] = value.u8_val; rec->fields_set |= CONFIG_STORE_SET_CT_CHANNEL_MAP_2; return true;
    // ct_installed: 0/1 only, and fields_set-gated -- the ONE place this
    // field differs from safety_tc_installed (0x0211 below). Setting the bit
    // here, at SET_PARAM time rather than deriving it at COMMIT like
    // ct_channel_map's group bit, is correct because unlike that map this is
    // a single indivisible answer: there is no partial state to guard
    // against. Monotonic like every other bit -- answering "installed" after
    // "not installed" changes the VALUE (and so re-arms the guards and
    // re-requires ct_channel_map) but never un-answers the question.
    case 0x0109u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  CHECK_U8_MAX(1u); rec->ct_installed = value.u8_val; rec->fields_set |= CONFIG_STORE_SET_CT_INSTALLED; return true;
    // tc_offset_c: same "unbounded beyond finiteness" treatment as its
    // sibling tc_expected_offset_c (0x020A) -- see this file's own header
    // comment on that field and config_store.h's struct comment on this one
    // for why no magnitude/sign bound is invented here. Not fields_set-gated
    // (0.0f is a safe "no correction" default).
    case 0x010Au: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->tc_offset_c = value.f32_val; return true;

    case 0x0201u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->firing_margin_c = value.f32_val; return true;
    case 0x0202u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_FINITE(); rec->overshoot_margin_c = value.f32_val; return true;
    case 0x0203u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->overshoot_time_s = value.u16_val; return true;
    case 0x0204u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_RANGE_OR_ZERO(CONFIG_STORE_MAX_RATE_C_PER_MIN_FLOOR, CONFIG_STORE_MAX_RATE_C_PER_MIN_CEILING); rec->max_rate_c_per_min = value.f32_val; rec->fields_set |= CONFIG_STORE_SET_MAX_RATE_C_PER_MIN; return true;
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
    // safety_tc_installed: 0/1 only (CHECK_U8_MAX(1u)) -- a boolean-shaped
    // field carried as u8 for wire-type consistency with every other
    // section-1 commissioning field. NOT fields_set-gated: unlike
    // abs_max_temp_c (no safe default is possible) this field's default of
    // 1 -- "I expect a sensor and I will trip if it is missing" -- IS safe
    // for an uncommissioned board, so it follows tc_type/borrowed_type_
    // expected's convention (a real compiled default, not an unset-until-
    // commissioned flag) rather than the four/six no-safe-default fields'.
    case 0x0211u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  CHECK_U8_MAX(1u); rec->safety_tc_installed = value.u8_val; return true;
    // estop_active_level: 0 = ACTIVE_HIGH (default, fail-safe, and this
    // bench's real wiring), 1 = ACTIVE_LOW. CHECK_U8_MAX(1u) keeps the
    // encoding closed, so no third value can ever reach flash and force the
    // decoder's fall-through to do the work. NOT fields_set-gated -- 0 is a
    // genuinely safe compiled default (see config_store.h's field comment),
    // so "never answered" and "answered 0" are the same state, exactly the
    // reasoning ct_topology uses.
    //
    // Deliberately NOT added to config_params_all_required_set(): making
    // this an ASKED question would leave every existing board
    // uncommissionable until someone answered a question whose safe answer
    // is already the default -- and the unsafe answer is the one an
    // operator might pick by accident.
    case 0x0212u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  CHECK_U8_MAX(1u); rec->estop_active_level = value.u8_val; return true;

    // A direct write to i_present_a is exactly "set by hand" (CT_
    // COMMISSIONING_PLAN.md step 3) -- marking i_present_a_manual here means
    // config_params_finalize_i_present_a() (called at COMMIT_CONFIG) will
    // never overwrite this operator-supplied value with the auto-derived
    // half-of-smallest-normal figure, even on a later commit that also
    // changes i_normal_a.
    case 0x0301u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_NONNEG(); rec->i_present_a = value.f32_val; rec->i_present_a_manual = true; return true;
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
    // max_expected_power_w: ROADMAP.md M12, sanity/plausibility input only --
    // no guard reads this (breakers are assumed sized for full load at 100%
    // duty per the owner). CHECK_F32_NONNEG, not CHECK_F32_POS: unlike
    // abs_max_temp_c there is no "0 secretly disables a guard" trap here, so
    // 0 W is merely an unusual entry, not a forbidden one -- still gated by
    // CONFIG_STORE_SET_MAX_EXPECTED_POWER_W so a stale/garbage byte from an
    // old record never gets read as a real operator-entered value.
    case 0x0318u: CHECK_TYPE(KILNLINK_PARAM_TYPE_BOOL); rec->ct_cal[2].calibrated = (value.bool_val != 0u); return true;
    case 0x0319u: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_NONNEG(); rec->max_expected_power_w = value.f32_val; rec->fields_set |= CONFIG_STORE_SET_MAX_EXPECTED_POWER_W; return true;

    // i_normal_a[0..2]: S14 (COMMISSIONING_UX.md sec 3.3), the measured
    // per-channel "normal" current the M12 zones-page button records.
    // CHECK_F32_NONNEG, not POS: a channel could genuinely measure ~0A on a
    // zone with no load wired yet -- 0 is a legitimate reading, distinct
    // from "not measured" (which is fields_set == unset, checked separately
    // by S14 itself, never by this range check).
    case 0x031Au: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_NONNEG(); rec->i_normal_a[0] = value.f32_val; rec->fields_set |= CONFIG_STORE_SET_I_NORMAL_A_0; return true;
    case 0x031Bu: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_NONNEG(); rec->i_normal_a[1] = value.f32_val; rec->fields_set |= CONFIG_STORE_SET_I_NORMAL_A_1; return true;
    case 0x031Cu: CHECK_TYPE(KILNLINK_PARAM_TYPE_F32); CHECK_F32_NONNEG(); rec->i_normal_a[2] = value.f32_val; rec->fields_set |= CONFIG_STORE_SET_I_NORMAL_A_2; return true;
    case 0x031Du: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->overcurrent_pct = value.u16_val; return true;
    case 0x031Eu: CHECK_TYPE(KILNLINK_PARAM_TYPE_U16); rec->overcurrent_time_s = value.u16_val; return true;
    // ct_topology: 0 (per_zone) or 1 (summed) only -- CONFIG_STORE_CT_
    // TOPOLOGY_PER_ZONE/_SUMMED. Not fields_set-gated: per_zone (0) is
    // already the safe default for a never-answered record, the same
    // reasoning as i_present_a above, not the "no safe default" reasoning
    // ct_installed/tc_source use.
    case 0x031Fu: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  CHECK_U8_MAX(CONFIG_STORE_CT_TOPOLOGY_SUMMED); rec->ct_topology = value.u8_val; return true;
    // zone_ct_channel[0..2] -- per-zone bookkeeping bits only; the group bit
    // (CONFIG_STORE_SET_ZONE_CT_CHANNEL) is DERIVED at COMMIT_CONFIG by
    // config_params_finalize_zone_ct_channel() below, exactly like
    // ct_channel_map's group bit, so answering two zones of three and
    // committing leaves the map untrusted rather than half-applied.
    case 0x0320u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  CHECK_U8_MAX(2u); rec->zone_ct_channel[0] = value.u8_val; rec->fields_set |= CONFIG_STORE_SET_ZONE_CT_CHANNEL_0; return true;
    case 0x0321u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  CHECK_U8_MAX(2u); rec->zone_ct_channel[1] = value.u8_val; rec->fields_set |= CONFIG_STORE_SET_ZONE_CT_CHANNEL_1; return true;
    case 0x0322u: CHECK_TYPE(KILNLINK_PARAM_TYPE_U8);  CHECK_U8_MAX(2u); rec->zone_ct_channel[2] = value.u8_val; rec->fields_set |= CONFIG_STORE_SET_ZONE_CT_CHANNEL_2; return true;

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
#undef CHECK_F32_POS
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
bool config_params_validate_ranges(const config_store_record_t *rec,
                                    const char **out_field, const char **out_rule,
                                    config_params_reject_reason_t *out_reason)
{
#define RANGE_FAIL(field_name, rule_text) do { \
        if (out_field) *out_field = (field_name); \
        if (out_rule) *out_rule = (rule_text); \
        if (out_reason) *out_reason = CONFIG_PARAMS_REJECT_RANGE; \
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
#define RANGE_F32_POS(field, field_name) do { \
        if (!isfinite(field) || (field) <= 0.0f) { \
            RANGE_FAIL((field_name), "this field must be a real positive value -- no unlimited/no-limit state exists"); \
        } \
    } while (0)
// 0.0f (disabled/not-commissioned) always passes; matches CHECK_F32_RANGE_OR_
// ZERO's SET_PARAM-time gate above -- this is the load-time/re-validate half
// of the same belt-and-suspenders pattern every other commissioned field uses.
#define RANGE_F32_RANGE_OR_ZERO(field, min, max, field_name) do { \
        if (!isfinite(field)) { \
            RANGE_FAIL((field_name), "NaN/Inf is never a valid value for this field"); \
        } \
        if ((field) != 0.0f && ((field) < (min) || (field) > (max))) { \
            RANGE_FAIL((field_name), "commissioned value outside its documented floor/ceiling"); \
        } \
    } while (0)

    RANGE_U8_MAX(rec->tc_source, CONFIG_STORE_TC_SOURCE_BOTH, "tc_source");
    RANGE_U8_MAX(rec->borrowed_zone_index, 2u, "borrowed_zone_index");
    RANGE_U8_MAX(rec->tc_placement_mode, CONFIG_STORE_TC_PLACEMENT_EXTERNAL_OVERHEAT, "tc_placement_mode");
    RANGE_U8_MAX(rec->tc_type, 7u, "tc_type");
    RANGE_U8_MAX(rec->borrowed_type_expected, 7u, "borrowed_type_expected");
    // zone_ct_channel[0..2] -- a CT channel id, so 0-2 and nothing else.
    // Safe to enforce unconditionally (no fields_set guard, unlike
    // abs_max_temp_c below): config_store_default() writes the identity map,
    // the v2-to-v3 migration derives it from ct_topology, and unpack
    // normalises a garbled record back onto the derived map before this
    // check ever sees it -- so there is no legitimate record shape in which
    // an out-of-range byte can reach here.
    RANGE_U8_MAX(rec->zone_ct_channel[0], 2u, "zone_ct_channel[0]");
    RANGE_U8_MAX(rec->zone_ct_channel[1], 2u, "zone_ct_channel[1]");
    RANGE_U8_MAX(rec->zone_ct_channel[2], 2u, "zone_ct_channel[2]");

    // abs_max_temp_c: NaN/Inf is refused UNCONDITIONALLY, bit or no bit.
    // Unlike max_expected_power_w below, this field is not carved out of a
    // formerly-reserved block -- it has always been a real, addressed part
    // of every record layout, so no pre-existing record's bytes here can be
    // legacy garbage. That means this function's backstop role (this file's
    // own header comment: catch an impossible value even in a record that
    // never went through config_params_set(), e.g. hand-built or migrated)
    // must not be defeated just because nobody has (yet) flipped the
    // fields_set bit -- a NaN sitting in this field is dangerous whether or
    // not it is "officially" set.
    //
    // Positivity (0 or negative refused) is a SEPARATE, narrower check that
    // IS guarded by the bit: an untouched default record legitimately holds
    // 0.0 unset (config_store_default() leaves it at memset(0)), and
    // RANGE_F32_POS's "no unlimited/no-limit state" refusal (M12's owner
    // ruling -- see CHECK_F32_POS's header comment above config_params_set())
    // must not fire on a field the operator has not yet had a chance to
    // commission. Once the bit IS set, the stored value came only from
    // CHECK_F32_POS at SET_PARAM time and is re-checked here as full
    // belt-and-suspenders, same as every other field.
    RANGE_F32_FINITE(rec->abs_max_temp_c, "abs_max_temp_c");
    if (config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_ABS_MAX_TEMP_C)) {
        RANGE_F32_POS(rec->abs_max_temp_c, "abs_max_temp_c");
    }
    RANGE_F32_NONNEG(rec->i_present_a, "i_present_a");
    // Guarded the same way abs_max_temp_c is just above, but for a DIFFERENT
    // reason: max_expected_power_w's wire bytes were carved out of the FRONT
    // of the former reserved block (config_store.c's REC_OFF_MAX_EXPECTED_
    // POWER_W layout comment), so every record committed before this pass
    // shipped holds whatever incidental byte pattern used to live there --
    // typically the 0xFF erased-flash fill, which decodes as NaN or a huge
    // magnitude float. An unconditional RANGE_F32_NONNEG here would refuse to
    // LOAD every pre-existing commissioned board purely because of stale
    // bytes in a field nobody has ever written, exactly the load-time
    // rejection hazard config_store.h's "Load-time rejection diagnostics"
    // block warns about. Once the bit IS set, the value came only from
    // CHECK_F32_NONNEG at SET_PARAM time and is re-checked here as full
    // belt-and-suspenders, same as abs_max_temp_c.
    if (config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_MAX_EXPECTED_POWER_W)) {
        RANGE_F32_NONNEG(rec->max_expected_power_w, "max_expected_power_w");
    }

    // i_normal_a[0..2]: same "carved out of the former reserved block" hazard
    // as max_expected_power_w just above -- a record committed before S14
    // shipped holds the old reserved region's incidental bytes here, so the
    // range check is gated on each channel's own fields_set bit, never
    // unconditional.
    if (config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_I_NORMAL_A_0)) {
        RANGE_F32_NONNEG(rec->i_normal_a[0], "i_normal_a[0]");
    }
    if (config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_I_NORMAL_A_1)) {
        RANGE_F32_NONNEG(rec->i_normal_a[1], "i_normal_a[1]");
    }
    if (config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_I_NORMAL_A_2)) {
        RANGE_F32_NONNEG(rec->i_normal_a[2], "i_normal_a[2]");
    }
    // overcurrent_pct/overcurrent_time_s are unpack()-normalized (erased-fill
    // 0xFFFF/0xFFFFFFFF -> 0) before this function ever sees them -- see
    // config_store.c's unpack_v2_fields() comment -- so no NaN/huge-magnitude
    // hazard reaches here the way it does for the two f32 fields above; U16
    // wire values have no NaN representation to guard against.

    RANGE_F32_FINITE(rec->firing_margin_c, "firing_margin_c");
    RANGE_F32_FINITE(rec->overshoot_margin_c, "overshoot_margin_c");
    // max_rate_c_per_min: same "would refuse to LOAD every pre-existing
    // commissioned board" hazard as abs_max_temp_c/max_expected_power_w/
    // i_normal_a[0..2] above, found in the 2026-09-10 S8 review -- this
    // range check was unconditional here, unlike every one of those
    // adjacent bounded fields. RANGE_F32_RANGE_OR_ZERO's own "0 always
    // bypasses" sentinel does NOT cover this: a board legitimately
    // commissioned to a tighter value in (0, CONFIG_STORE_MAX_RATE_C_PER_
    // MIN_FLOOR) before this [15, 60] bound existed is nonzero and would
    // hit RANGE_FAIL, which rejects the WHOLE record -- losing
    // abs_max_temp_c and every other commissioned field along with it, not
    // just this one. Gate on the fields_set bit exactly like the precedent
    // fields do: once the bit IS set, the value came only from CHECK_F32_
    // RANGE_OR_ZERO at SET_PARAM time (config_params_set()) and is
    // re-checked here as full belt-and-suspenders; an untouched pre-
    // existing record's stale/legacy value in this field is never refused
    // purely because nobody has re-commissioned it since this bound shipped.
    if (config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_MAX_RATE_C_PER_MIN)) {
        RANGE_F32_RANGE_OR_ZERO(rec->max_rate_c_per_min, CONFIG_STORE_MAX_RATE_C_PER_MIN_FLOOR,
                                 CONFIG_STORE_MAX_RATE_C_PER_MIN_CEILING, "max_rate_c_per_min");
    }
    RANGE_F32_FINITE(rec->tc_disagreement_c, "tc_disagreement_c");
    RANGE_F32_FINITE(rec->tc_expected_offset_c, "tc_expected_offset_c");
    RANGE_F32_FINITE(rec->tc_offset_c, "tc_offset_c");
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
#undef RANGE_F32_POS
}

bool config_params_validate_ex(const config_store_record_t *rec, const char **out_field,
                                const char **out_rule, config_params_reject_reason_t *out_reason)
{
    if (out_reason) {
        *out_reason = CONFIG_PARAMS_REJECT_NONE;
    }
    if (!rec) {
        if (out_field) *out_field = "rec";
        if (out_rule) *out_rule = "NULL record";
        // No CONFIG_PARAMS_REJECT_* value fits "not even a record" --
        // link_task.c never calls this with a NULL rec (s_staged_config is
        // always a real object), so *out_reason is deliberately left at
        // CONFIG_PARAMS_REJECT_NONE here; the wire mapping in link_task.c
        // treats that as KILNLINK_COMMIT_CONFIG_REJECT_UNKNOWN, its own
        // documented fallback for exactly this "should not occur" case.
        return false;
    }

    if (!config_params_validate_ranges(rec, out_field, out_rule, out_reason)) {
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
        if (out_reason) {
            *out_reason = CONFIG_PARAMS_REJECT_CONTRADICTION;
        }
        return false;
    }

    // 2026-08-28 audit fix (M2): safety_commissioning_page.html's
    // checkTcMaxContradiction() enforces "abs_max_temp_c cannot contradict
    // the sensor [tc_type]" client-side ONLY -- inadequate for a
    // dangerous-risk field: (a) /safety/commissioning accepts POSTs from
    // anything that can reach it (a curl with abs_max_temp_c=1500,
    // tc_type=7/Type T, whose sensor tops out at 400 C, was accepted and
    // committed with no server-side check at all), and (b) the JS check
    // returns null unless BOTH fields are in the same submission, so it
    // cannot fire in the common case of field-by-field commissioning. This
    // is the backstop -- same fields_set-gating idiom as the tc_source/
    // tc_placement_mode contradiction just above, and the same values as the
    // JS table (TC_MAX_C_BY_TYPE, safety_commissioning_page.html), which
    // must be kept in sync with this table by hand if either changes; both
    // cite MAX31856.pdf page 4/15/26 as the source of the numbers.
    bool abs_max_temp_set =
        config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_ABS_MAX_TEMP_C);
    bool tc_type_set =
        config_store_field_is_set(&rec->fields_set, CONFIG_STORE_SET_TC_TYPE);
    if (abs_max_temp_set && tc_type_set) {
        static const float TC_MAX_C_BY_TYPE[8] = {
            1798.0f, // 0: Type B, TTC 95..1798 C
            1000.0f, // 1: Type E, TTC -200..1000 C
            1200.0f, // 2: Type J, TTC -210..1200 C
            1372.0f, // 3: Type K, TTC -200..1372 C
            1300.0f, // 4: Type N, TTC -200..1300 C
            1768.0f, // 5: Type R, TTC -50..1768 C
            1768.0f, // 6: Type S, TTC -50..1768 C
            400.0f,  // 7: Type T, TTC -200..400 C
        };
        // rec->tc_type is already RANGE_U8_MAX-checked <= 7 above, so this
        // index is always in bounds by the time control reaches here.
        if (rec->abs_max_temp_c > TC_MAX_C_BY_TYPE[rec->tc_type]) {
            if (out_field) {
                *out_field = "abs_max_temp_c";
            }
            if (out_rule) {
                *out_rule = "CONFIG_REFERENCE.md sec1 / ROADMAP.md M12: abs_max_temp_c cannot "
                            "exceed the selected tc_type's own sensor maximum -- the kiln ceiling "
                            "cannot exceed what the thermocouple itself can report";
            }
            if (out_reason) {
                *out_reason = CONFIG_PARAMS_REJECT_CONTRADICTION;
            }
            return false;
        }
    }

    return true;
}

bool config_params_validate(const config_store_record_t *rec, const char **out_field,
                             const char **out_rule)
{
    return config_params_validate_ex(rec, out_field, out_rule, NULL);
}

// Name -> param_id lookup for config_params_validate()/_ex()'s out_field
// strings -- COMMISSIONING.md sec 2.1's table, restricted to the fields that
// can actually appear as an out_field here (every RANGE_FAIL site above,
// plus the tc_placement_mode contradiction). "rec" (the NULL-record guard)
// is deliberately absent -- CONFIG_PARAMS_NO_PARAM_ID is exactly the right
// answer for it, same as for any other name this table does not recognise.
typedef struct {
    const char *name;
    uint16_t id;
} config_param_name_id_t;

static const config_param_name_id_t CONFIG_PARAM_NAME_TABLE[] = {
    { "tc_source", 0x0101u },
    { "borrowed_zone_index", 0x0102u },
    { "tc_placement_mode", 0x0103u },
    { "abs_max_temp_c", 0x0104u },
    { "tc_type", 0x0105u },
    { "tc_offset_c", 0x010Au },
    { "borrowed_type_expected", 0x0210u },
    { "i_present_a", 0x0301u },
    { "max_expected_power_w", 0x0319u },
    { "i_normal_a[0]", 0x031Au },
    { "i_normal_a[1]", 0x031Bu },
    { "i_normal_a[2]", 0x031Cu },
    { "overcurrent_pct", 0x031Du },
    { "overcurrent_time_s", 0x031Eu },
    { "ct_topology", 0x031Fu },
    { "zone_ct_channel[0]", 0x0320u },
    { "zone_ct_channel[1]", 0x0321u },
    { "zone_ct_channel[2]", 0x0322u },
    { "firing_margin_c", 0x0201u },
    { "overshoot_margin_c", 0x0202u },
    { "max_rate_c_per_min", 0x0204u },
    { "tc_disagreement_c", 0x0208u },
    { "tc_expected_offset_c", 0x020Au },
    { "cj_warn_c", 0x020Bu },
    { "cj_max_c", 0x020Cu },
    { "k_ct_v_per_a[0]", 0x0308u },
    { "k_ct_v_per_a[1]", 0x0309u },
    { "k_ct_v_per_a[2]", 0x030Au },
    { "gain[0]", 0x030Bu },
    { "gain[1]", 0x030Cu },
    { "gain[2]", 0x030Du },
    { "mains_voltage_v", 0x030Eu },
    { "ct_cal[0].gain", 0x0310u },
    { "ct_cal[1].gain", 0x0311u },
    { "ct_cal[2].gain", 0x0312u },
    { "ct_cal[0].offset", 0x0313u },
    { "ct_cal[1].offset", 0x0314u },
    { "ct_cal[2].offset", 0x0315u },
};
#define CONFIG_PARAM_NAME_TABLE_COUNT \
    (sizeof(CONFIG_PARAM_NAME_TABLE) / sizeof(CONFIG_PARAM_NAME_TABLE[0]))

uint16_t config_params_id_for_field_name(const char *name)
{
    if (!name) {
        return CONFIG_PARAMS_NO_PARAM_ID;
    }
    for (size_t i = 0; i < CONFIG_PARAM_NAME_TABLE_COUNT; i++) {
        if (strcmp(CONFIG_PARAM_NAME_TABLE[i].name, name) == 0) {
            return CONFIG_PARAM_NAME_TABLE[i].id;
        }
    }
    return CONFIG_PARAMS_NO_PARAM_ID;
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
    uint32_t need = (uint32_t)(CONFIG_STORE_SET_CT_CHANNEL_MAP_0 | CONFIG_STORE_SET_CT_CHANNEL_MAP_1 |
                                CONFIG_STORE_SET_CT_CHANNEL_MAP_2);
    if (config_store_field_is_set(&rec->fields_set, need)) {
        rec->fields_set |= CONFIG_STORE_SET_CT_CHANNEL_MAP;
    }
}

void config_params_finalize_zone_ct_channel(config_store_record_t *rec)
{
    if (!rec) {
        return;
    }
    // CT_CHANNEL_MASK_PLAN.md step 2, and the exact shape of
    // config_params_finalize_ct_channel_map() above: the group bit is only
    // ever newly set here, once all three per-zone bits are present. Two of
    // three leaves it unset on purpose -- a half-answered zone-to-channel
    // map is a DIFFERENT kiln topology, not a smaller one, since the
    // unanswered zone keeps the compiled identity default while its siblings
    // move. Monotonic like every other fields_set bit: an incomplete triple
    // never clears a group bit a prior commit already earned.
    uint32_t need = (uint32_t)(CONFIG_STORE_SET_ZONE_CT_CHANNEL_0 | CONFIG_STORE_SET_ZONE_CT_CHANNEL_1 |
                                CONFIG_STORE_SET_ZONE_CT_CHANNEL_2);
    if (config_store_field_is_set(&rec->fields_set, need)) {
        rec->fields_set |= CONFIG_STORE_SET_ZONE_CT_CHANNEL;
    }
}

void config_params_finalize_i_present_a(config_store_record_t *rec)
{
    if (!rec || rec->i_present_a_manual) {
        // Either no record, or an operator already wrote i_present_a by
        // hand (0x0301's SET_PARAM handler set the marker) -- CT_
        // COMMISSIONING_PLAN.md step 3's "unless set by hand" means a
        // manual value is NEVER overwritten here, even by a later commit
        // that also changes i_normal_a.
        return;
    }
    // Half the smallest CONFIRMED zone normal (CONFIG_STORE_SET_I_NORMAL_A_
    // 0/_1/_2 -- a channel whose bit is clear has never been measured and
    // must not participate, same "skip entirely" rule i_normal_valid[]
    // follows in safety_guards.c). If no zone has ever been measured yet,
    // i_present_a is left exactly as it was (the compiled 2.0A default on a
    // fresh board, or whatever the previous commit already computed) --
    // this function only ever narrows a genuine measurement into a
    // load-active threshold, it never invents one from nothing.
    static const uint16_t I_NORMAL_A_BITS[3] = {
        CONFIG_STORE_SET_I_NORMAL_A_0, CONFIG_STORE_SET_I_NORMAL_A_1, CONFIG_STORE_SET_I_NORMAL_A_2
    };
    bool  have_any = false;
    float smallest = 0.0f;
    for (unsigned z = 0; z < 3; z++) {
        if (!config_store_field_is_set(&rec->fields_set, I_NORMAL_A_BITS[z])) {
            continue;
        }
        float v = rec->i_normal_a[z];
        if (!isfinite(v) || v < 0.0f) {
            continue; // cannot happen via SET_PARAM's own CHECK_F32_NONNEG, defensive only
        }
        // Opus review of 51c084f/c49bb0e, finding 2: a zone commissioned
        // with i_normal_a == 0.0f (CHECK_F32_NONNEG allows exactly 0, e.g.
        // a zone measured with its element disconnected, or a commissioning
        // mistake) must not win this smallest-search -- it would drive
        // i_present_a itself to 0.0f, and current_sense.c:285's
        // `conducting = (amps > i_present_a)` then reads "conducting" on
        // pure ADC noise for every other zone too, since i_present_a is a
        // single shared scalar, not per-zone. Skip it exactly like an
        // unset/negative/non-finite value above -- a 0 A "normal" carries no
        // usable load-active threshold information.
        if (v <= 0.0f) {
            continue;
        }
        if (!have_any || v < smallest) {
            smallest = v;
            have_any = true;
        }
    }
    if (have_any) {
        rec->i_present_a = smallest * 0.5f;
    }
}

bool config_params_all_required_set(const config_store_record_t *rec)
{
    if (!rec) {
        return false;
    }
    // CONFIG_STORE_SET_TC_TYPE joined this mask 2026-08-24. tc_type is NOT a
    // no-safe-default field the way the other six/seven bits below are (it
    // keeps a real compiled default, K) -- but this function's return value
    // is the only thing that feeds calibration_missing
    // (link_task.c: `to_write.calibration_missing =
    // !config_params_all_required_set(&to_write);`), and calibration_missing
    // is the one existing, already-wired, wire-visible ("this board is not
    // fully commissioned") signal this codebase has (DIAG's
    // CALIBRATION_MISSING bit). tc_type has no sentinel value that can make
    // "never touched" visibly distinct from "commissioned as K" the way an
    // unset abs_max_temp_c reads back as the suspicious 0.0 -- folding
    // CONFIG_STORE_SET_TC_TYPE into this mask is therefore the only way,
    // without inventing a second wire flag this pass does not add, to keep
    // that fact from being silently permissive: a board that has committed
    // every other field but never touched tc_type now correctly still reads
    // back as NOT fully commissioned, exactly as if abs_max_temp_c itself
    // were still 0.
    //
    // CONFIG_STORE_SET_CT_INSTALLED joined this mask on the "CTs are optional
    // hardware" pass, and it is the one bit here that is required
    // UNCONDITIONALLY while the bit it gates -- CT_CHANNEL_MAP -- became
    // CONDITIONAL. The asymmetry is the whole point:
    //
    //   * "Are CTs fitted?" must always be answered. Defaulting it either way
    //     is a silent claim about this board's hardware that the firmware has
    //     no way to check (config_store.h's comment on the bit).
    //   * "Which relay does each CT watch?" is only a meaningful question on a
    //     board that HAS CTs. Requiring it on a CT-less board made that board
    //     permanently uncommissionable and therefore permanently unable to
    //     heat -- correct while "no CT" was not a state this firmware could
    //     represent, wrong now that it is.
    //
    // Note the order-independence: if the CT_INSTALLED bit is clear, the value
    // of rec->ct_installed is not trustworthy, but the strict branch is taken
    // anyway (`!= 0` on a record defaulting to 1) AND the missing bit fails
    // the check on its own. There is no bit pattern in which an unanswered
    // question relaxes a requirement.
    // 2026-09-09: ct_channel_map is required only when a per-zone CT actually
    // needs one relay per channel resolved. In ct_topology == SUMMED, the one
    // shared CT (channel 2) is read straight into S14's sum-vs-commanded
    // check and S15's per-zone deficit check by zone id, never through
    // ct_channel_map -- see safety_core.c's safety_guard_input_t builder
    // (the `relay_commanded_now_for_zone` block's own comment: "unlike
    // relay_commanded_now_for_ct above this does NOT go through
    // ct_channel_map ... summed mode's S14/S15 do not consult that map at
    // all") and CURRENT_SENSE.md section 0.2 ("the mapping check ... is
    // skipped entirely -- there is no per-relay mapping to resolve"). Before
    // this fix the gate still demanded the three per-channel bits on a
    // summed board, which the summed design has no meaningful answer for --
    // making a fully-configured summed-CT board permanently uncommissionable
    // (docs/audits/commissioning_gap_and_no_heat_2026-09-09.md). ct_topology
    // itself is a plain marker byte with no fields_set bit and a safe
    // PER_ZONE(0) default (config_store.h), so reading it here is always
    // well-defined even on a record that has never touched it.
    uint32_t required = (uint32_t)(CONFIG_STORE_SET_TC_SOURCE | CONFIG_STORE_SET_BORROWED_ZONE_INDEX |
                                    CONFIG_STORE_SET_TC_PLACEMENT_MODE | CONFIG_STORE_SET_ABS_MAX_TEMP_C |
                                    CONFIG_STORE_SET_MAX_RATE_C_PER_MIN |
                                    CONFIG_STORE_SET_MAINS_VOLTAGE_V | CONFIG_STORE_SET_TC_TYPE |
                                    CONFIG_STORE_SET_CT_INSTALLED);
    if (rec->ct_installed != 0u && rec->ct_topology != CONFIG_STORE_CT_TOPOLOGY_SUMMED) {
        required = (uint32_t)(required | CONFIG_STORE_SET_CT_CHANNEL_MAP);
    }
    return config_store_field_is_set(&rec->fields_set, required);
}
