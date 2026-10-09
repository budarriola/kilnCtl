#include "zones_http_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "zone_settings_source_chain.h" /* zones_config_settings_source_import_has_cycle() shares the
                                          * chain-walk algorithm with this file's setters. */

/* ---- Public getters (profiles_http.c) ------------------------------------ */

bool zones_config_get_max_ramp(uint8_t zone_index, float *out_c_per_hr)
{
    if (!out_c_per_hr || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_c_per_hr = s_zones.cfg.zones[zone_index].max_ramp_c_per_hr;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_ramp enforces. 0 is legal (the
 * documented "never configured" encoding). */
bool zones_config_set_max_ramp_no_save(uint8_t zone_index, float c_per_hr)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(c_per_hr) || c_per_hr < 0.0f || c_per_hr > ZONE_MAX_RAMP_C_PER_HR_MAX) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].max_ramp_c_per_hr = c_per_hr;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_max_ramp(uint8_t zone_index, float c_per_hr)
{
    if (!zones_config_set_max_ramp_no_save(zone_index, c_per_hr)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_coil_power_w(uint8_t zone_index, float *out_power_w)
{
    if (!out_power_w || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_power_w = s_zones.cfg.zones[zone_index].coil_power_w;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_coil_power enforces. 0 is legal (the
 * documented "not overridden" encoding). */
bool zones_config_set_coil_power_w_no_save(uint8_t zone_index, float power_w)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(power_w) ||
        (power_w != 0.0f && (power_w < ZONE_COIL_POWER_W_MIN || power_w > ZONE_COIL_POWER_W_MAX))) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].coil_power_w = power_w;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_coil_power_w(uint8_t zone_index, float power_w)
{
    if (!zones_config_set_coil_power_w_no_save(zone_index, power_w)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_cal_offset(uint8_t zone_index, float *out_cal_offset_c)
{
    if (!out_cal_offset_c || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_cal_offset_c = s_zones.cfg.zones[zone_index].cal_offset_c;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_cal enforces. */
bool zones_config_set_cal_offset_no_save(uint8_t zone_index, float cal_offset_c)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(cal_offset_c) || cal_offset_c < ZONE_CAL_OFFSET_MIN_C || cal_offset_c > ZONE_CAL_OFFSET_MAX_C) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].cal_offset_c = cal_offset_c;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_cal_offset(uint8_t zone_index, float cal_offset_c)
{
    if (!zones_config_set_cal_offset_no_save(zone_index, cal_offset_c)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

/* PID_EXPANSION_PLAN.md 3.3 consolidation -- see zones_http.h's own doc
 * comment on this pair. zone_index >= thermo_count reads as "not enabled",
 * same convention every other simple getter in this file uses (see
 * zones_config_get_max_ramp() just above), not a special "cannot answer"
 * case -- opt-in has one meaning (on/off) at every valid index and a safe
 * default (off) everywhere else. */
bool zones_config_get_adaptive_tune_enabled(uint8_t zone_index)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    return s_zones.cfg.zones[zone_index].adaptive_tune_enabled != 0;
}

bool zones_config_set_adaptive_tune_enabled_no_save(uint8_t zone_index, bool enabled)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].adaptive_tune_enabled = enabled ? 1 : 0;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_adaptive_tune_enabled(uint8_t zone_index, bool enabled)
{
    if (!zones_config_set_adaptive_tune_enabled_no_save(zone_index, enabled)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

uint32_t zones_config_generation(void)
{
    return s_config_generation;
}

/* Whole-struct snapshot/restore pair for a caller (backup_import.c) that
 * batches many _no_save() setter calls together and needs to roll RAM back
 * atomically if one of them fails partway through, without ever calling
 * nvs_save() itself -- same "RAM must never run ahead of NVS" discipline as
 * every other setter in this file, just applied to a whole batch instead of
 * one field. zones_cfg_t is <= ZONES_CONFIG_BLOB_MAX_SIZE (896 B); the
 * struct copy here is a plain memcpy, cheap next to the flash I/O this
 * whole change exists to amortize, so no allocation of any kind is needed
 * on either side of this pair. */
void zones_config_get_full_copy(zones_cfg_t *out)
{
    *out = s_zones.cfg;
}

void zones_config_restore_snapshot_no_save(const zones_cfg_t *snapshot)
{
    zones_cfg_lock();
    s_zones.cfg = *snapshot;
    s_config_generation++;
    zones_cfg_unlock();
}

/* TODO.md 8.2 "Tie it to the guards, not only the UI" -- see
 * s_zones_config_valid's comment for the exact rule. Consulted by
 * profile_executor.c/autotune_engine.c before allowing a run to start, and
 * reported on /api/status (dashboard_http.c) as zones_config_valid. */
bool zones_config_is_valid(void)
{
    return s_zones_config_valid;
}

uint8_t zones_config_get_thermo_count(void)
{
    return s_zones.cfg.thermo_count;
}

uint8_t zones_config_get_relay_count(void)
{
    return s_zones.cfg.relay_count;
}

uint8_t zones_config_get_max_simultaneous_relays(void)
{
    return s_zones.cfg.max_simultaneous_relays;
}

bool zones_config_get_continue_on_zone_trip(void)
{
    return s_zones.cfg.continue_on_zone_trip != 0;
}

/* 2026-08-21, TODO.md owner-report item 3 -- safety_link.c's consumer (see
 * its safety_sync_tc_type()): the RP2040 safety processor's own,
 * independent thermocouple type, as last saved on this page. Always
 * answerable, unlike the per-zone getters below -- this is a global setting
 * with a real value from the moment NVS first loads (defaulting to
 * THERMO_TC_K either via a fresh zero-init struct reading 0/THERMO_TC_B...
 * no: see below) -- so, unlike zones_config_get_max_ramp() and friends,
 * there is no "cannot answer" case to report via a bool return; the return
 * value exists only so this getter's shape matches every other one in this
 * file and a future caller doesn't have to special-case it.
 *
 * IMPORTANT: on a board that has never loaded a valid zones config at all
 * (s_zones_config_valid false -- first boot, corrupt NVS, a refused
 * newer-than-firmware blob), s_zones.cfg is the zeroed default, and 0 here
 * decodes as THERMO_TC_B, NOT THERMO_TC_K -- unlike MAX31856.c's own
 * hardcoded boot default. safety_link.c's caller MUST check
 * zones_config_is_valid() itself before trusting this value for anything
 * other than "what would get sent if asked to sync right now" -- see that
 * file's safety_sync_tc_type() for how it actually guards this. */
bool zones_config_get_safety_tc_type(uint8_t *out_tc_type)
{
    if (!out_tc_type) {
        return false;
    }
    *out_tc_type = s_zones.cfg.safety_tc_type;
    return true;
}

/* 2026-08-21, LCD item 1 (TODO.md owner-report item 1/4's on-device half):
 * the web zones page (zones_page.html) gained per-channel thermocouple type
 * selection this same day, but only as an HTTP form field -- there was no
 * public getter/setter an LCD page could call, only the POST body parser's
 * private z%u_tctype handling above. Follows zones_config_get_pid()'s/
 * zones_config_set_pid()'s exact shape (same zone_index bounds check, same
 * "false means cannot answer" convention, setter bumps s_config_generation
 * before nvs_save() so a running profile's next tick sees the new value
 * whether or not the flash write itself succeeds) so this module keeps
 * exactly one accessor pattern rather than growing a second one for LCD
 * callers specifically.
 *
 * Deliberately does NOT call thermo_owner_command_config_channel() to push
 * the new type to hardware immediately -- checked zones_http.c's own POST
 * /api/zones handler (the web page's save path) before writing this, and it
 * doesn't either: the only place this module ever calls that function is
 * zones_http_start()'s boot-time apply, further down this file. So an LCD
 * write and a web-page save now have the SAME behaviour (persisted
 * immediately, taken into the running MAX31856 register only on the next
 * boot) rather than the LCD accidentally doing more than the web form it is
 * mirroring. If that boot-only gap is ever closed for the web path, this
 * setter should gain the same live-apply call at the same time -- not
 * silently drift ahead of it. */
bool zones_config_get_tc_type(uint8_t zone_index, uint8_t *out_tc_type)
{
    if (!out_tc_type || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_tc_type = s_zones.cfg.zones[zone_index].tc_type;
    return true;
}

bool zones_config_set_tc_type_no_save(uint8_t zone_index, uint8_t tc_type)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (tc_type > ZONE_TC_TYPE_MAX_REAL) {
        /* Same bound the POST parser enforces (parse_zone_fields()) -- a
         * voltage-input mode is never a legal choice from an
         * operator-facing "thermocouple type" control, LCD or web alike. */
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].tc_type = tc_type;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_tc_type(uint8_t zone_index, uint8_t tc_type)
{
    if (!zones_config_set_tc_type_no_save(zone_index, tc_type)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

/* Setter half of zones_config_get_safety_tc_type() above -- same "global,
 * not per-zone" setting (the RP2040 safety processor's own MAX31856-equivalent
 * type), same bound as the per-channel setter just above, same generation/
 * nvs_save shape as every other setter in this file. safety_link.c's
 * safety_sync_tc_type() is the consumer that notices the generation bump and
 * re-mirrors this to the Pico -- this function does not talk to the safety
 * link itself, matching the POST handler's own division of labor (this
 * module owns storage only). */
bool zones_config_set_safety_tc_type(uint8_t tc_type)
{
    if (tc_type > ZONE_TC_TYPE_MAX_REAL) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.safety_tc_type = tc_type;
    s_config_generation++;
    zones_cfg_unlock();
    return nvs_save() == ESP_OK;
}

bool zones_config_get_relay_mask(uint8_t zone_index, uint8_t *out_mask)
{
    if (!out_mask || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_mask = s_zones.cfg.zones[zone_index].relay_mask;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_relay_mask handling enforces --
 * relay_mask may only reference relays 1..relay_count. */
bool zones_config_set_relay_mask_no_save(uint8_t zone_index, uint8_t relay_mask)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint8_t valid_bits = s_zones.cfg.relay_count >= 8 ? 0xFF : (uint8_t)((1u << s_zones.cfg.relay_count) - 1u);
    if ((relay_mask & ~valid_bits) != 0) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].relay_mask = relay_mask;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_relay_mask(uint8_t zone_index, uint8_t relay_mask)
{
    if (!zones_config_set_relay_mask_no_save(zone_index, relay_mask)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_thermo_mask(uint8_t zone_index, uint8_t *out_mask)
{
    if (!out_mask || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_mask = s_zones.cfg.zones[zone_index].thermo_mask;
    return true;
}

/* Same bound parse_zone_fields()'s explicit z%u_thermo_mask handling
 * enforces -- thermo_mask may only reference channels 1..thermo_count.
 * Unlike the POST handler this setter has no "omitted means preserve the
 * legacy mapping" case -- see this function's header comment. */
bool zones_config_set_thermo_mask_no_save(uint8_t zone_index, uint8_t thermo_mask)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint8_t valid_bits =
        s_zones.cfg.thermo_count >= 8 ? 0xFF : (uint8_t)((1u << s_zones.cfg.thermo_count) - 1u);
    if ((thermo_mask & ~valid_bits) != 0) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].thermo_mask = thermo_mask;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_thermo_mask(uint8_t zone_index, uint8_t thermo_mask)
{
    if (!zones_config_set_thermo_mask_no_save(zone_index, thermo_mask)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_ct_mask(uint8_t zone_index, uint8_t *out_mask)
{
    if (!out_mask || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_mask = s_zones.cfg.zones[zone_index].ct_mask;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_ct_mask handling enforces -- ct_mask
 * may only reference channels 1..ZONE_CT_CHANNEL_COUNT, a fixed hardware
 * count (not relay_count/thermo_count-relative like the two setters above). */
bool zones_config_set_ct_mask_no_save(uint8_t zone_index, uint8_t ct_mask)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint8_t valid_bits = (uint8_t)((1u << ZONE_CT_CHANNEL_COUNT) - 1u);
    if ((ct_mask & ~valid_bits) != 0) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].ct_mask = ct_mask;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_ct_mask(uint8_t zone_index, uint8_t ct_mask)
{
    if (!zones_config_set_ct_mask_no_save(zone_index, ct_mask)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_name(uint8_t zone_index, char *out, size_t out_cap)
{
    if (!out || out_cap == 0 || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    strncpy(out, s_zones.cfg.zones[zone_index].name, out_cap - 1);
    out[out_cap - 1] = '\0';
    return true;
}

/* Same rejection parse_zone_fields() produces for an overlong z%u_name
 * (http_form_find_field() returning -2), applied to a NUL-terminated C
 * string. NULL is treated as an empty name (clears it), matching a POST that
 * omits the field. */
bool zones_config_set_name_no_save(uint8_t zone_index, const char *name)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    size_t len = name ? strlen(name) : 0;
    if (len > ZONE_NAME_MAX_LEN) {
        return false;
    }
    zones_cfg_lock();
    strncpy(s_zones.cfg.zones[zone_index].name, name ? name : "", ZONE_NAME_MAX_LEN);
    s_zones.cfg.zones[zone_index].name[ZONE_NAME_MAX_LEN] = '\0';
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_name(uint8_t zone_index, const char *name)
{
    if (!zones_config_set_name_no_save(zone_index, name)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

/* See zones_http.h's doc comment on this pair, and this file's relay-names
 * section header comment for the full design rationale (separate NVS blob,
 * name kept across zone reassignment). */
bool zones_config_get_relay_name(uint8_t relay_n, char *out, size_t out_cap)
{
    if (relay_n < 1 || relay_n > KILN_IO_RELAY_COUNT || !out || out_cap == 0) {
        return false;
    }
    strncpy(out, s_relay_names.cfg.names[relay_n - 1], out_cap - 1);
    out[out_cap - 1] = '\0';
    return true;
}

bool zones_config_set_relay_name(uint8_t relay_n, const char *name)
{
    if (relay_n < 1 || relay_n > KILN_IO_RELAY_COUNT) {
        return false;
    }
    size_t len = name ? strlen(name) : 0;
    if (len > RELAY_NAME_MAX_LEN) {
        return false;
    }
    strncpy(s_relay_names.cfg.names[relay_n - 1], name ? name : "", RELAY_NAME_MAX_LEN);
    s_relay_names.cfg.names[relay_n - 1][RELAY_NAME_MAX_LEN] = '\0';
    return relay_names_save() == ESP_OK;
}

/* See zones_config_accessors.h's doc comment on this pair. Note the getter
 * reports UNSET as a successful answer, not a failure -- "nobody has said"
 * is a fact about this relay, not an inability to look it up. */
bool zones_config_get_relay_device_type(uint8_t relay_n, relay_device_type_t *out)
{
    if (relay_n < 1 || relay_n > KILN_IO_RELAY_COUNT || !out) {
        return false;
    }
    uint8_t stored = s_relay_names.cfg.types[relay_n - 1];
    /* A stored value this build does not recognise reads as UNSET rather
     * than being passed through to a caller that would have to invent a
     * rendering for it. This is reachable only from a blob written by newer
     * firmware that appended an enum value, then rolled back -- the same
     * direction zones_config_json_decode_blob() refuses outright. Degrading
     * to "unknown" is the honest answer here (the whole point of UNSET), and
     * it is strictly safer than surfacing a number with no icon. */
    if (stored >= RELAY_DEVICE_TYPE_COUNT) {
        *out = RELAY_DEVICE_TYPE_UNSET;
        return true;
    }
    *out = (relay_device_type_t)stored;
    return true;
}

bool zones_config_set_relay_device_type(uint8_t relay_n, relay_device_type_t type)
{
    if (relay_n < 1 || relay_n > KILN_IO_RELAY_COUNT) {
        return false;
    }
    if ((uint8_t)type >= RELAY_DEVICE_TYPE_COUNT) {
        return false;
    }
    s_relay_names.cfg.types[relay_n - 1] = (uint8_t)type;
    return relay_names_save() == ESP_OK;
}

bool zones_config_get_pid(uint8_t zone_index, float *out_kp, float *out_ki, float *out_kd)
{
    if (!out_kp || !out_ki || !out_kd || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    *out_kp = z->pid_kp;
    *out_ki = z->pid_ki;
    *out_kd = z->pid_kd;
    return true;
}

bool zones_config_set_pid_no_save(uint8_t zone_index, float kp, float ki, float kd)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    /* Same bound parse_zone_fields()'s z%u_kp/z%u_ki/z%u_kd handling enforces
     * (ZONE_PID_GAIN_MAX, zones_http.h) -- this used to check only isfinite()
     * and >= 0.0f, with no upper bound, which meant a caller reaching this
     * setter directly (the LCD UI, PcTools/MCP, and now POST /api/zones/pid)
     * could exceed what the web form's own POST /api/zones path would ever
     * accept for the identical field. Closed 2026-08-31 alongside the new
     * narrow PID-only endpoint precisely because that endpoint is a second
     * caller of this same code -- exactly the moment a latent bound mismatch
     * like this stops being harmless. */
    if (!isfinite(kp) || !isfinite(ki) || !isfinite(kd) || kp < 0.0f || ki < 0.0f || kd < 0.0f ||
        kp > ZONE_PID_GAIN_MAX || ki > ZONE_PID_GAIN_MAX || kd > ZONE_PID_GAIN_MAX) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    /* Judged BEFORE the stores below overwrite the old values. Uses
     * zones_config_gain_changed(), the same helper the whole-page POST
     * /api/zones path uses (which carries the rationale), so both paths
     * agree on what "the gains changed" means. */
    const bool gains_changed = zones_config_gain_changed(z->pid_kp, kp) || zones_config_gain_changed(z->pid_ki, ki) ||
                               zones_config_gain_changed(z->pid_kd, kd);
    zones_cfg_lock();
    z->pid_kp = kp;
    z->pid_ki = ki;
    z->pid_kd = kd;
    /* ZONES_CFG_VERSION 12->13: invalidate the tuning-quality record (set 1
     * -- see zone_cfg_t::tuning_valid's own doc comment) when the gains
     * ACTUALLY change, regardless of caller -- autotune's accept() path, a
     * manual POST /api/zones/pid edit, adaptive_tune.c's blended re-tune,
     * backup import, or the LCD UI/uart_bridge_ext.c path. All of them reach
     * gains through this one setter, which is why the invalidation lives
     * HERE and not at every call site (the "reset-one-side" bug class). A
     * same-value write-back (a GET-merge-POST client re-posting the stored
     * gains) leaves the record standing: the gains it describes are still
     * the gains the zone runs. That is what lost bench zone 0's record
     * (docs/BENCH_TEST_LOG.md, a same-value control_set_zone_pid write).
     * autotune accept() re-establishes a fresh record via
     * zones_config_set_tuning_quality() immediately after this call, so the
     * invalidate-then-repopulate ordering never leaves a stale-but-valid-
     * looking record visible in between. A stale quality record pinned to
     * changed gains would be worse than none. */
    if (gains_changed) {
        z->tuning_valid = 0;
    }
    /* Bumped before the NVS write, not after it: the gains are already live
     * for the next control tick at this point, so a running profile must
     * re-read them (TODO.md 6A.7) whether or not the save succeeds. Note
     * this returns false on a save failure while the config change stands --
     * unchanged behaviour, and the generation reflects the in-RAM truth. */
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_pid(uint8_t zone_index, float kp, float ki, float kd)
{
    if (!zones_config_set_pid_no_save(zone_index, kp, ki, kd)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

/* See zones_http.h -- Phase 3 control-loop wiring's read of the fuzzy
 * adjustment-strength knob. */
bool zones_config_get_fuzzy_strength_pct(uint8_t zone_index, float *out_pct)
{
    if (!out_pct || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_pct = s_zones.cfg.zones[zone_index].fuzzy_strength_pct;
    return true;
}

/* Writer for the getter above. Same bound parse_zone_fields()'s
 * z%u_fuzzy_strength enforces (0..ZONE_FUZZY_STRENGTH_PCT_MAX) -- refused,
 * never clamped, same discipline as every other setter in this file. */
bool zones_config_set_fuzzy_strength_pct_no_save(uint8_t zone_index, float pct)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(pct) || pct < 0.0f || pct > ZONE_FUZZY_STRENGTH_PCT_MAX) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].fuzzy_strength_pct = pct;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_fuzzy_strength_pct(uint8_t zone_index, float pct)
{
    if (!zones_config_set_fuzzy_strength_pct_no_save(zone_index, pct)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

/* PID_EXPANSION_PLAN.md section 3.2 follow-up (ZONES_CFG_VERSION 14->15) --
 * see zone_cfg_t::coupling_diag_k_dc's own doc comment for what this is and
 * why it is a separate field from model_k_dc/ff_k_dc. STORAGE ONLY: no
 * control-loop consumer reads this yet, same as coupling_coeff[] was itself
 * pure storage for one pass before profile_executor.c's Phase 3b wired it
 * into the feedforward. */
bool zones_config_get_coupling_diag_k_dc(uint8_t zone_index, float *out_k_dc)
{
    if (!out_k_dc || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_k_dc = s_zones.cfg.zones[zone_index].coupling_diag_k_dc;
    return true;
}

/* Writer for the getter above. Same bound validate_zones_cfg()'s
 * coupling_diag_k_dc check enforces (0..ZONE_MODEL_K_MAX) -- refused, never
 * clamped, matching every other setter in this file. backup_http.c's import
 * needs this to round-trip the field, the same reason every other setter in
 * this file exists. */
bool zones_config_set_coupling_diag_k_dc_no_save(uint8_t zone_index, float k_dc)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(k_dc) || k_dc < 0.0f || k_dc > ZONE_MODEL_K_MAX) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].coupling_diag_k_dc = k_dc;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_coupling_diag_k_dc(uint8_t zone_index, float k_dc)
{
    if (!zones_config_set_coupling_diag_k_dc_no_save(zone_index, k_dc)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

/* Row-based (ZONES_CFG_VERSION 10->11) -- see zone_cfg_t::coupling_coeff's
 * own doc comment for what each cell means. out_row must have room for
 * MAX31856_CHANNEL_COUNT floats; the diagonal (out_row[zone_index]) is
 * always 0 on return, same "unused, stays zero" rule the storage itself
 * enforces. */
bool zones_config_get_coupling(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (!out_row || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    memcpy(out_row, z->coupling_coeff, sizeof(z->coupling_coeff));
    /* docs/ON_OFF_ZONE_PLAN.md sec 1, belt and braces: zero this zone's
     * WHOLE row if it is itself ZONE_TYPE_ON_OFF ("no neighbour's heat is
     * corrected for on this zone" -- it has no setpoint to correct), and
     * zero any COLUMN whose neighbor is on/off even when zone_index itself
     * is a heater ("this zone injects no heat into anyone" -- it's a fan,
     * not a source zone_coupling_solve.c's Jacobi system may allocate real
     * commandable duty to). Enforced here, at every read, not only at
     * zones_config_set_coupling() time below -- the matrix is also editable
     * via autotune's coupling pass, so a getter-side guard is the only place
     * that can never be bypassed by a future write path.
     *
     * docs/SPARE_RELAY_ONOFF_PLAN.md sec 10: a MONITOR-ONLY zone
     * (zone_is_monitor_only(): HEATER with no heater relay) is masked the
     * same way, for the same two reasons -- it is never driven, so there is
     * no duty of its own to correct for a neighbour's heat (row), and it
     * injects no heat into anyone (column). Without this its stored column
     * kept feeding coupled feedforward's neighbour sum, the S8 rate-guard
     * estimate and profile_feasibility's effective_k_dc as if it still
     * heated. _raw below stays unmasked, so the stored cells survive for a
     * later revert to a real heater. */
    if (zone_is_on_off(zone_index) || zone_is_monitor_only(zone_index)) {
        memset(out_row, 0, MAX31856_CHANNEL_COUNT * sizeof(out_row[0]));
        return true;
    }
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        if (zone_is_on_off(j) || zone_is_monitor_only(j)) {
            out_row[j] = 0.0f;
        }
    }
    return true;
}

/* RAW sibling of the getter above -- same bounds check, same memcpy, but
 * deliberately skips the on/off row/column zeroing. backup_export.c needs
 * this: a zone that is CURRENTLY on/off still has real, previously-measured
 * coupling cells sitting in flash (from before it was retyped, or measured
 * while it was still a heater), and zones_config_get_coupling()'s
 * belt-and-braces mask (docs/ON_OFF_ZONE_PLAN.md sec 1) is a live-control-
 * loop guard, not a storage truncation -- reading through it for a backup
 * silently exported 0.0 for every cell touching an on/off zone (as either
 * row or column), and a subsequent import then committed that 0.0 via
 * zones_config_set_coupling_cell() (which has no on/off awareness at all,
 * by design -- see that function's own header comment), permanently
 * overwriting the real stored value with zero. Found via bench A4
 * (2026-09-28): zone 2 on/off, coupling cells z0[2]/z1[2]/z2[0]/z2[1] all
 * went to 0 after a plain export/import round trip. Same "raw accessor for
 * backup round-trip" pattern as zones_config_get_coil_power_w()/
 * zones_config_get_cal_offset() above -- never call this from any live
 * control-loop path; those must keep going through the masking getter. */
bool zones_config_get_coupling_raw(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (!out_row || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    memcpy(out_row, z->coupling_coeff, sizeof(z->coupling_coeff));
    return true;
}

/* ZONES_CFG_VERSION 11->12 siblings of the getter above -- identical shape
 * and orientation, for coupling_tau_s[]/coupling_dead_time_s[]. */
bool zones_config_get_coupling_tau(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (!out_row || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    memcpy(out_row, z->coupling_tau_s, sizeof(z->coupling_tau_s));
    return true;
}

bool zones_config_get_coupling_dead_time(uint8_t zone_index, float out_row[MAX31856_CHANNEL_COUNT])
{
    if (!out_row || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    memcpy(out_row, z->coupling_dead_time_s, sizeof(z->coupling_dead_time_s));
    return true;
}

/* Whole-row setter -- every cell checked before ANY is written, same
 * "no half-updated group" discipline as zones_config_set_model()/
 * zones_config_set_temp_limits(). Bounds match parse_zone_fields()'s
 * z%u_coupling_c%u: each off-diagonal cell finite and in
 * 0..ZONE_COUPLING_COEFF_MAX (see that macro's doc comment for why this
 * stayed non-negative rather than gaining an independent sign). The
 * diagonal MUST be exactly 0 -- a zone's response to its own heater is
 * model_k_dc, not a coupling cell, and a nonzero diagonal would be
 * ambiguous with a real (if coincidentally equal) cross-gain. */
bool zones_config_set_coupling(uint8_t zone_index, const float row[MAX31856_CHANNEL_COUNT])
{
    if (!row || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    /* docs/ON_OFF_ZONE_PLAN.md sec 1: refuse a nonzero row for an on/off
     * zone outright -- it has no setpoint to correct, so a caller asking to
     * store real coupling data for one is asking to store something that
     * cannot mean anything, not something this store should silently zero
     * and accept. The getter above is the belt-and-braces backstop for
     * whatever is ALREADY on flash; this is the suspenders for what a
     * caller tries to write next. */
    if (zone_is_on_off(zone_index)) {
        for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
            if (row[j] != 0.0f) {
                return false;
            }
        }
    }
    for (uint8_t j = 0; j < MAX31856_CHANNEL_COUNT; j++) {
        if (!isfinite(row[j])) {
            return false;
        }
        if (j == zone_index) {
            if (row[j] != 0.0f) {
                return false;
            }
            continue;
        }
        /* A column pointed at an on/off neighbor must also stay 0 -- see the
         * getter's own comment on why a fan must never inject heat into the
         * Jacobi system. */
        if (zone_is_on_off(j)) {
            if (row[j] != 0.0f) {
                return false;
            }
            continue;
        }
        if (row[j] < 0.0f || row[j] > ZONE_COUPLING_COEFF_MAX) {
            return false;
        }
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    zones_cfg_lock();
    memcpy(z->coupling_coeff, row, sizeof(z->coupling_coeff));
    s_config_generation++;
    zones_cfg_unlock();
    return nvs_save() == ESP_OK;
}

/* Single-cell setter -- lets a caller update ONE neighbor's measured
 * coupling without clobbering the rest of the row's already-stored cells.
 * autotune_engine.c's finalize_fit() needs exactly this: a single relay run
 * on zone i only measures i's effect on each OTHER zone j, one cell of zone
 * j's row at a time, and must never wipe out zone j's other, previously
 * measured neighbors just because this run didn't touch them. Same bounds
 * as the whole-row setter above, applied to the one cell being written. */
bool zones_config_set_coupling_cell_no_save(uint8_t zone_index, uint8_t neighbor_index, float coeff, float tau_s,
                                     float dead_time_s)
{
    if (zone_index >= s_zones.cfg.thermo_count || neighbor_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    if (!isfinite(coeff) || !isfinite(tau_s) || !isfinite(dead_time_s)) {
        return false;
    }
    if (neighbor_index == zone_index) {
        if (coeff != 0.0f || tau_s != 0.0f || dead_time_s != 0.0f) {
            return false;
        }
        return true; /* writing the diagonal to 0 is a no-op, not an error */
    }
    if (coeff < 0.0f || coeff > ZONE_COUPLING_COEFF_MAX) {
        return false;
    }
    /* ZONES_CFG_VERSION 11->12: all-or-nothing with coeff above -- an
     * out-of-range tau_s/dead_time_s must refuse the WHOLE cell, not just
     * silently leave the two new fields unwritten while coeff still lands.
     * See zones_config_set_coupling_cell()'s own header comment. */
    if (tau_s < 0.0f || tau_s > ZONE_MODEL_TIME_MAX_S || dead_time_s < 0.0f ||
        dead_time_s > ZONE_MODEL_TIME_MAX_S) {
        return false;
    }
    zones_cfg_lock();
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->coupling_coeff[neighbor_index] = coeff;
    z->coupling_tau_s[neighbor_index] = tau_s;
    z->coupling_dead_time_s[neighbor_index] = dead_time_s;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_coupling_cell(uint8_t zone_index, uint8_t neighbor_index, float coeff, float tau_s,
                                     float dead_time_s)
{
    if (!zones_config_set_coupling_cell_no_save(zone_index, neighbor_index, coeff, tau_s, dead_time_s)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_settings_source(uint8_t zone_index, uint8_t group, uint8_t *out_settings_source)
{
    if (!out_settings_source || zone_index >= s_zones.cfg.thermo_count || group >= SRC_GROUP_COUNT) {
        return false;
    }
    *out_settings_source = s_zones.cfg.zones[zone_index].settings_source[group];
    return true;
}

/* Setter for the getter above. Same rule parse_zone_fields()'s
 * z%u_settings_source enforces: either ZONE_SETTINGS_SOURCE_CUSTOM (0xFF) or
 * a real zone index < MAX31856_CHANNEL_COUNT that is NOT zone_index itself
 * (self-reference is the degenerate inheritance cycle, refused here for the
 * identical reason parse_zone_fields() refuses it) -- AND, past that, does
 * not close a longer cycle through any OTHER zone's already-stored link
 * (zones_config_json_settings_source_chain_has_cycle() on a probe copy with this one link
 * applied). Only this one link changes here, so a cycle can only be newly
 * created running THROUGH zone_index -- walking the chain starting there,
 * against every other zone's live stored value, is sufficient; it does not
 * need to check every zone. */
bool zones_config_set_settings_source(uint8_t zone_index, uint8_t group, uint8_t settings_source)
{
    if (group >= SRC_GROUP_COUNT) {
        return false;
    }
    /* zone_index is bounds-checked against thermo_count, same as every
     * other per-zone setter in this file -- but thermo_count itself is only
     * ever trusted up to MAX31856_CHANNEL_COUNT elsewhere (see
     * zones_config_settings_source_import_has_cycle() and
     * zones_config_json_normalize_settings_source_cycles(), which both clamp it before using
     * it as a bound). This site did not: a corrupt thermo_count >
     * MAX31856_CHANNEL_COUNT (direct NVS tampering, or firmware that
     * predates zones_config_json_validate()'s own range check on it) would let a
     * zone_index >= MAX31856_CHANNEL_COUNT pass this check and then index
     * probe[]/s_zones.cfg.zones[] -- both fixed
     * MAX31856_CHANNEL_COUNT-sized arrays -- out of bounds on the very next
     * line, before the chain-walk below is even reached. Clamping here (not
     * just in the chain-walk's own thermo_count argument, which is a
     * separate, secondary consistency fix below) is what actually closes
     * that hole. */
    uint8_t thermo_count = s_zones.cfg.thermo_count > MAX31856_CHANNEL_COUNT ? MAX31856_CHANNEL_COUNT
                                                                             : s_zones.cfg.thermo_count;
    if (zone_index >= thermo_count) {
        return false;
    }
    if (settings_source != ZONE_SETTINGS_SOURCE_CUSTOM && settings_source >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    if (settings_source == zone_index) {
        return false;
    }
    zone_cfg_t probe[MAX31856_CHANNEL_COUNT];
    memcpy(probe, s_zones.cfg.zones, sizeof(probe));
    probe[zone_index].settings_source[group] = settings_source;
    if (zones_config_json_settings_source_chain_has_cycle(probe, group, zone_index, thermo_count)) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].settings_source[group] = settings_source;
    s_config_generation++;
    zones_cfg_unlock();
    return nvs_save() == ESP_OK;
}

/* Commit-loop counterpart to zones_config_set_settings_source() for a
 * multi-entry import/whole-page write that has ALREADY passed
 * zones_config_settings_source_import_has_cycle() against the full proposed
 * set. That pre-check validates the FINAL assembled state; this setter skips
 * the chain-walk zones_config_set_settings_source() runs against the
 * PARTIALLY APPLIED live config while a multi-entry commit loop is
 * mid-flight, because that walk can spuriously refuse an intermediate state
 * a valid two-zone swap (e.g. live 0->1,1->0 changing to 0->2,1->CUSTOM: pass
 * 1 accepts the final state, but committing zone 0 first makes the walk see
 * live {0->2,1->0}, no cycle there -- the actual failure case is the reverse
 * order or a longer swap, see backup_http.c's restore-scenario comment)
 * without any live-config help from an in-progress commit. Only bounds and
 * self-reference are re-checked here (still real defenses against a
 * corrupt/malicious override_source entry slipping past pass 1); the cycle
 * walk itself is intentionally omitted so this call cannot fail for a
 * zone/value pair pass 1 already accepted, keeping the two-pass invariant
 * this file's import/whole-page paths depend on: pass 2 must not be able to
 * fail. Callers MUST have run zones_config_settings_source_import_has_cycle()
 * over every candidate in this commit loop first -- this function trusts
 * that check, it does not repeat it. */
bool zones_config_set_settings_source_unchecked(uint8_t zone_index, uint8_t group, uint8_t settings_source)
{
    if (group >= SRC_GROUP_COUNT) {
        return false;
    }
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (settings_source != ZONE_SETTINGS_SOURCE_CUSTOM && settings_source >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    if (settings_source == zone_index) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].settings_source[group] = settings_source;
    s_config_generation++;
    zones_cfg_unlock();
    return nvs_save() == ESP_OK;
}

/* Same checks as zones_config_set_settings_source_unchecked() above, minus
 * the nvs_save() -- see this function's header comment in
 * zones_config_accessors.h. Caller owns calling nvs_save() once after the
 * whole batch. */
bool zones_config_set_settings_source_unchecked_no_save(uint8_t zone_index, uint8_t group, uint8_t settings_source)
{
    if (group >= SRC_GROUP_COUNT) {
        return false;
    }
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (settings_source != ZONE_SETTINGS_SOURCE_CUSTOM && settings_source >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    if (settings_source == zone_index) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].settings_source[group] = settings_source;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_save_now(void)
{
    return nvs_save() == ESP_OK;
}

bool zones_config_settings_source_import_has_cycle(uint8_t group,
                                                    const bool has_override[MAX31856_CHANNEL_COUNT],
                                                    const uint8_t override_source[MAX31856_CHANNEL_COUNT],
                                                    uint8_t *out_cycle_zone)
{
    if (group >= SRC_GROUP_COUNT) {
        return true; /* refuse rather than silently walk an out-of-range group */
    }
    zone_cfg_t probe[MAX31856_CHANNEL_COUNT];
    memcpy(probe, s_zones.cfg.zones, sizeof(probe));
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        if (has_override[i]) {
            probe[i].settings_source[group] = override_source[i];
        }
    }
    uint8_t thermo_count = s_zones.cfg.thermo_count > MAX31856_CHANNEL_COUNT ? MAX31856_CHANNEL_COUNT
                                                                             : s_zones.cfg.thermo_count;
    for (uint8_t i = 0; i < thermo_count; i++) {
        if (zones_config_json_settings_source_chain_has_cycle(probe, group, i, thermo_count)) {
            if (out_cycle_zone) {
                *out_cycle_zone = i;
            }
            return true;
        }
    }
    return false;
}

bool zones_config_get_sanity_rate(uint8_t zone_index, float *out_c_per_min)
{
    if (!out_c_per_min || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_c_per_min = s_zones.cfg.zones[zone_index].sanity_rate_c_per_min;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_sanity enforces. 0 is legal (the
 * documented "never configured" encoding). */
bool zones_config_set_sanity_rate_no_save(uint8_t zone_index, float c_per_min)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(c_per_min) || c_per_min < 0.0f || c_per_min > ZONE_SANITY_RATE_MAX_C_PER_MIN) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].sanity_rate_c_per_min = c_per_min;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_sanity_rate(uint8_t zone_index, float c_per_min)
{
    if (!zones_config_set_sanity_rate_no_save(zone_index, c_per_min)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_control_mode(uint8_t zone_index, zone_control_mode_t *out_mode)
{
    if (!out_mode || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_mode = (zone_control_mode_t)s_zones.cfg.zones[zone_index].control_mode;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_mode enforces (0-3). */
bool zones_config_set_control_mode_no_save(uint8_t zone_index, zone_control_mode_t mode)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if ((unsigned)mode > (unsigned)ZONE_CONTROL_MODE_PID_FUZZY) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].control_mode = (uint8_t)mode;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_control_mode(uint8_t zone_index, zone_control_mode_t mode)
{
    if (!zones_config_set_control_mode_no_save(zone_index, mode)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_zone_type(uint8_t zone_index, zone_type_t *out_type)
{
    if (!out_type || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_type = (zone_type_t)s_zones.cfg.zones[zone_index].zone_type;
    return true;
}

/* Same bound zones_config_set_control_mode() enforces, against ZONE_TYPE's
 * own max instead of zone_control_mode_t's. */
bool zones_config_set_zone_type_no_save(uint8_t zone_index, zone_type_t type)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if ((unsigned)type > (unsigned)ZONE_TYPE_ON_OFF) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].zone_type = (uint8_t)type;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_zone_type(uint8_t zone_index, zone_type_t type)
{
    if (!zones_config_set_zone_type_no_save(zone_index, type)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

/* docs/ON_OFF_ZONE_PLAN.md sec 1's predicate -- see zones_config_accessors.h
 * for the fail-closed convention on an out-of-range zone_index (false here,
 * i.e. "not on/off", matching zones_config_get_zone_type()'s own false
 * return for the same case). */
bool zone_is_on_off(uint8_t zone_index)
{
    zone_type_t t;
    if (!zones_config_get_zone_type(zone_index, &t)) {
        return false;
    }
    return t == ZONE_TYPE_ON_OFF;
}

/* docs/SPARE_RELAY_ONOFF_PLAN.md sec 10 (owner decision, finding 6): the one
 * rule for a MONITOR-ONLY zone -- a valid ZONE_TYPE_HEATER zone whose
 * relay_mask is 0, which is what a zone is left as after its relay was
 * converted to an aux output. Every executor/guard site asks this predicate;
 * none of them test relay_mask == 0 themselves. Fail-closed: false for an
 * out-of-range index or an unreadable type/mask, so a read failure can never
 * exempt a zone from heater supervision. */
bool zone_is_monitor_only(uint8_t zone_index)
{
    zone_type_t t;
    if (!zones_config_get_zone_type(zone_index, &t) || t != ZONE_TYPE_HEATER) {
        return false;
    }
    uint8_t mask = 0;
    if (!zones_config_get_relay_mask(zone_index, &mask)) {
        return false;
    }
    return mask == 0;
}

/* docs/ON_OFF_ZONE_PLAN.md sec 2's zone_needs_ceiling(zi) predicate -- see
 * zones_config_accessors.h for the fail-closed convention on an out-of-range
 * zone_index (true here, i.e. "needs a ceiling", the safer default). */
bool zone_needs_ceiling(uint8_t zone_index)
{
    zone_type_t t;
    if (!zones_config_get_zone_type(zone_index, &t)) {
        return true;
    }
    if (t != ZONE_TYPE_ON_OFF) {
        return true; /* HEATER always needs a ceiling -- unchanged rule */
    }
    uint8_t tmask = 0;
    if (!zones_config_get_thermo_mask(zone_index, &tmask)) {
        return true;
    }
    return tmask != 0; /* on/off zone: only if it actually has a TC assigned */
}

bool zones_config_get_failsafe_state(uint8_t zone_index, bool *out_on)
{
    if (!out_on) {
        return false;
    }
    *out_on = false; /* fail-closed default, set before the range check so every early return is safe */
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_on = s_zones.cfg.zones[zone_index].failsafe_state != 0;
    return true;
}

/* Writer for the getter above -- 2026-09-16 backup-round-trip-gap closure,
 * group 1. Same [0,1] bound validate_zones_cfg() enforces on this field
 * (z->failsafe_state > 1). */
bool zones_config_set_failsafe_state_no_save(uint8_t zone_index, bool on_state)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].failsafe_state = on_state ? 1u : 0u;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_failsafe_state(uint8_t zone_index, bool on_state)
{
    if (!zones_config_set_failsafe_state_no_save(zone_index, on_state)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_hyst_c(uint8_t zone_index, float *out_hyst_c)
{
    if (!out_hyst_c) {
        return false;
    }
    *out_hyst_c = 2.0f; /* plan default, set before the range check -- see header comment */
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    float stored = s_zones.cfg.zones[zone_index].hyst_c;
    *out_hyst_c = (stored > 0.0f) ? stored : 2.0f;
    return true;
}

/* Writer for the getter above -- 2026-09-16 backup-round-trip-gap closure,
 * group 2. 0 is accepted (explicit "reset to firmware default"); a non-zero
 * value must fall in [ZONE_HYST_C_MIN, ZONE_HYST_C_MAX] -- same
 * "(0, MIN)-sliver refused" shape validate_zones_cfg() enforces. */
bool zones_config_set_hyst_c_no_save(uint8_t zone_index, float hyst_c)
{
    if (zone_index >= s_zones.cfg.thermo_count || !isfinite(hyst_c) || hyst_c < 0.0f ||
        hyst_c > ZONE_HYST_C_MAX || (hyst_c != 0.0f && hyst_c < ZONE_HYST_C_MIN)) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].hyst_c = hyst_c;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_hyst_c(uint8_t zone_index, float hyst_c)
{
    if (!zones_config_set_hyst_c_no_save(zone_index, hyst_c)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_min_on_s(uint8_t zone_index, uint16_t *out_s)
{
    if (!out_s) {
        return false;
    }
    *out_s = 30u;
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint16_t stored = s_zones.cfg.zones[zone_index].min_on_s;
    *out_s = (stored > 0u) ? stored : 30u;
    return true;
}

/* Writer for the getter above -- 2026-09-16 backup-round-trip-gap closure,
 * group 2. Same "0 = reset to default, (0, MIN)-sliver refused" shape as
 * zones_config_set_hyst_c(), bounded against ZONE_MIN_ON_OFF_S_MIN/MAX. */
bool zones_config_set_min_on_s_no_save(uint8_t zone_index, uint16_t min_on_s)
{
    if (zone_index >= s_zones.cfg.thermo_count || min_on_s > ZONE_MIN_ON_OFF_S_MAX ||
        (min_on_s != 0u && min_on_s < ZONE_MIN_ON_OFF_S_MIN)) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].min_on_s = min_on_s;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_min_on_s(uint8_t zone_index, uint16_t min_on_s)
{
    if (!zones_config_set_min_on_s_no_save(zone_index, min_on_s)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_min_off_s(uint8_t zone_index, uint16_t *out_s)
{
    if (!out_s) {
        return false;
    }
    *out_s = 30u;
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint16_t stored = s_zones.cfg.zones[zone_index].min_off_s;
    *out_s = (stored > 0u) ? stored : 30u;
    return true;
}

/* Writer for the getter above -- same shape as zones_config_set_min_on_s(). */
bool zones_config_set_min_off_s_no_save(uint8_t zone_index, uint16_t min_off_s)
{
    if (zone_index >= s_zones.cfg.thermo_count || min_off_s > ZONE_MIN_ON_OFF_S_MAX ||
        (min_off_s != 0u && min_off_s < ZONE_MIN_ON_OFF_S_MIN)) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].min_off_s = min_off_s;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_min_off_s(uint8_t zone_index, uint16_t min_off_s)
{
    if (!zones_config_set_min_off_s_no_save(zone_index, min_off_s)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_temp_limits(uint8_t zone_index, float *out_max_temp_c, float *out_min_temp_c)
{
    if (!out_max_temp_c || !out_min_temp_c || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    *out_max_temp_c = z->max_temp_c;
    *out_min_temp_c = z->min_temp_c;
    return true;
}

/* Bundled setter -- see zones_http.h's comment on this pair for why NO
 * max_temp_c >= min_temp_c cross-check is added here: parse_zone_fields()
 * (the POST /api/zones authority) does not enforce one either, so this
 * setter matches it exactly rather than becoming stricter than the page it
 * mirrors. Both fields checked before either is written, same
 * reject-nothing-half-applied discipline as zones_config_set_model(). */
bool zones_config_set_temp_limits_no_save(uint8_t zone_index, float max_temp_c, float min_temp_c)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(max_temp_c) || max_temp_c < 0.0f || max_temp_c > ZONE_MAX_TEMP_C_MAX) {
        return false;
    }
    if (!isfinite(min_temp_c) || min_temp_c < ZONE_MIN_TEMP_C_MIN || min_temp_c > ZONE_MIN_TEMP_C_MAX) {
        return false;
    }
    zones_cfg_lock();
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->max_temp_c = max_temp_c;
    z->min_temp_c = min_temp_c;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_temp_limits(uint8_t zone_index, float max_temp_c, float min_temp_c)
{
    if (!zones_config_set_temp_limits_no_save(zone_index, max_temp_c, min_temp_c)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_heater_cfg(uint8_t zone_index, float *out_window_ms, float *out_min_on_ms,
                                 float *out_min_off_ms)
{
    if (!out_window_ms || !out_min_on_ms || !out_min_off_ms || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    *out_window_ms = z->heater_window_ms;
    *out_min_on_ms = z->heater_min_on_ms;
    *out_min_off_ms = z->heater_min_off_ms;
    return true;
}

/* Bundled setter, same "no half-updated group" discipline as
 * zones_config_set_model()/zones_config_set_temp_limits(). No
 * min_on_ms/min_off_ms-vs-window_ms cross-check: parse_zone_fields() (the
 * POST authority) does not enforce one either -- see
 * zones_config_set_temp_limits()'s comment for the identical reasoning. */
bool zones_config_set_heater_cfg_no_save(uint8_t zone_index, float window_ms, float min_on_ms, float min_off_ms)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(window_ms) || window_ms < 0.0f || window_ms > ZONE_HEATER_WINDOW_MS_MAX) {
        return false;
    }
    if (!isfinite(min_on_ms) || min_on_ms < 0.0f || min_on_ms > ZONE_HEATER_MIN_ON_OFF_MS_MAX) {
        return false;
    }
    /* Same disjoint rule parse_zone_fields() enforces -- this setter is a
     * second door into the same field (it is reachable without going through
     * a POST body) and must not be the loose one. */
    if (min_on_ms > 0.0f && min_on_ms < ZONE_HEATER_MIN_ON_MS_FLOOR) {
        return false;
    }
    if (!isfinite(min_off_ms) || min_off_ms < 0.0f || min_off_ms > ZONE_HEATER_MIN_ON_OFF_MS_MAX) {
        return false;
    }
    /* Window-vs-min-on relationship, same disjoint rule parse_zone_fields()
     * enforces. This setter is the door backup import comes through, and a
     * backup written before 2026-08-29 can easily carry a 2000 ms window. */
    if (window_ms > 0.0f && window_ms < zone_required_window_ms(min_on_ms)) {
        return false;
    }
    zones_cfg_lock();
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->heater_window_ms = window_ms;
    z->heater_min_on_ms = min_on_ms;
    z->heater_min_off_ms = min_off_ms;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_heater_cfg(uint8_t zone_index, float window_ms, float min_on_ms, float min_off_ms)
{
    if (!zones_config_set_heater_cfg_no_save(zone_index, window_ms, min_on_ms, min_off_ms)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_guard_thresholds(uint8_t zone_index, float *out_wrong_dir_window_s,
                                       float *out_wrong_dir_rate_c_per_min, float *out_off_settle_s,
                                       float *out_runaway_rate_c_per_min, float *out_runaway_margin_c,
                                       float *out_drift_period_s, float *out_sensor_fault_debounce_ticks,
                                       float *out_frozen_window_s)
{
    if (!out_wrong_dir_window_s || !out_wrong_dir_rate_c_per_min || !out_off_settle_s ||
        !out_runaway_rate_c_per_min || !out_runaway_margin_c || !out_drift_period_s ||
        !out_sensor_fault_debounce_ticks || !out_frozen_window_s || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    *out_wrong_dir_window_s = z->guard_wrong_dir_window_s;
    *out_wrong_dir_rate_c_per_min = z->guard_wrong_dir_rate_c_per_min;
    *out_off_settle_s = z->guard_off_settle_s;
    *out_runaway_rate_c_per_min = z->guard_runaway_rate_c_per_min;
    *out_runaway_margin_c = z->guard_runaway_margin_c;
    *out_drift_period_s = z->guard_drift_period_s;
    *out_sensor_fault_debounce_ticks = z->guard_sensor_fault_debounce_ticks;
    *out_frozen_window_s = z->guard_frozen_window_s;
    return true;
}

/* The five per-zone guard overrides added in v8 (see zone_cfg_t). Bundled for
 * the same reason the 8 above are: thermal_guard.c reads them as one group
 * when it builds a thermal_guard_cfg_t. Every one keeps the "0 = use the
 * module's named default" convention, so this getter reports the stored value
 * verbatim and the substitution stays where it belongs -- in the module that
 * owns the default.
 *
 * SIGNATURE UNCHANGED by ZONES_CFG_VERSION 9 (2026-08-27, timing profiles --
 * see zones_http.h's own note on this pair): zone_index now resolves to
 * zone_cfg_t::timing_profile first, then to that slot in
 * zones_cfg_t::timing_profiles[], instead of reading the nine fields directly
 * off zone_cfg_t -- neither caller had to change. */
bool zones_config_get_guard_extra(uint8_t zone_index, float *out_progress_duty_min,
                                  float *out_progress_window_s, float *out_drift_hysteresis_c,
                                  float *out_frozen_eps_c, float *out_cross_zone_period_s)
{
    if (!out_progress_duty_min || !out_progress_window_s || !out_drift_hysteresis_c ||
        !out_frozen_eps_c || !out_cross_zone_period_s || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint8_t p = s_zones.cfg.zones[zone_index].timing_profile;
    /* Defensive, not expected in practice: every path that can write
     * s_zones.cfg (POST /api/zones, NVS load via zones_config_json_validate(), a
     * kiln-config import) already bounds timing_profile against
     * timing_profile_count before it is ever stored. A getter must still
     * never read past the array on the strength of that alone. */
    if (p >= s_zones.cfg.timing_profile_count) {
        return false;
    }
    const zone_timing_profile_t *tp = &s_zones.cfg.timing_profiles[p];
    *out_progress_duty_min = tp->guard_progress_duty_min;
    *out_progress_window_s = tp->guard_progress_window_s;
    *out_drift_hysteresis_c = tp->guard_drift_hysteresis_c;
    *out_frozen_eps_c = tp->guard_frozen_eps_c;
    *out_cross_zone_period_s = tp->guard_cross_zone_period_s;
    return true;
}

/* The four per-zone executor overrides added in v8 -- profile_executor.c's
 * side of the same pass. Same 0-means-default convention, and the same
 * "signature unchanged, only the internal lookup moved to timing_profiles[]"
 * note as zones_config_get_guard_extra() above applies here too. */
bool zones_config_get_executor_thresholds(uint8_t zone_index, float *out_bangbang_hysteresis_c,
                                          float *out_cooling_limited_margin_c,
                                          float *out_cooling_limited_hold_s, float *out_ramp_lock_band_c)
{
    if (!out_bangbang_hysteresis_c || !out_cooling_limited_margin_c || !out_cooling_limited_hold_s ||
        !out_ramp_lock_band_c || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    uint8_t p = s_zones.cfg.zones[zone_index].timing_profile;
    if (p >= s_zones.cfg.timing_profile_count) {
        return false; /* defensive -- see zones_config_get_guard_extra()'s identical comment */
    }
    const zone_timing_profile_t *tp = &s_zones.cfg.timing_profiles[p];
    *out_bangbang_hysteresis_c = tp->bangbang_hysteresis_c;
    *out_cooling_limited_margin_c = tp->cooling_limited_margin_c;
    *out_cooling_limited_hold_s = tp->cooling_limited_hold_s;
    *out_ramp_lock_band_c = tp->ramp_lock_band_c;
    return true;
}

/* The one global v8 override. Not gated on thermo_count -- the PC link exists
 * whether or not a single zone has been configured. */
bool zones_config_get_pc_link_abort_silence_ms(float *out_ms)
{
    if (!out_ms) {
        return false;
    }
    *out_ms = s_zones.cfg.pc_link_abort_silence_ms;
    return true;
}

/* ---- Timing-profile bundle accessors -- 2026-09-16 backup-round-trip-gap
 * closure, group 3. See zones_config_accessors.h's header comment on this
 * block for why these exist and why the bundle is spelled as scalar
 * out-params rather than a zone_timing_profile_t*. */

bool zones_config_get_timing_profile_index(uint8_t zone_index, uint8_t *out_index)
{
    if (!out_index || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_index = s_zones.cfg.zones[zone_index].timing_profile;
    return true;
}

bool zones_config_set_timing_profile_index_no_save(uint8_t zone_index, uint8_t index)
{
    if (zone_index >= s_zones.cfg.thermo_count || index >= s_zones.cfg.timing_profile_count) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].timing_profile = index;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_timing_profile_index(uint8_t zone_index, uint8_t index)
{
    if (!zones_config_set_timing_profile_index_no_save(zone_index, index)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

uint8_t zones_config_get_timing_profile_count(void)
{
    return s_zones.cfg.timing_profile_count;
}

bool zones_config_get_timing_profile_raw(uint8_t profile_index, char *out_name, size_t name_cap,
                                         float *out_progress_duty_min, float *out_progress_window_s,
                                         float *out_drift_hysteresis_c, float *out_frozen_eps_c,
                                         float *out_cross_zone_period_s, float *out_bangbang_hysteresis_c,
                                         float *out_cooling_limited_margin_c,
                                         float *out_cooling_limited_hold_s, float *out_ramp_lock_band_c)
{
    if (!out_name || name_cap == 0 || !out_progress_duty_min || !out_progress_window_s ||
        !out_drift_hysteresis_c || !out_frozen_eps_c || !out_cross_zone_period_s ||
        !out_bangbang_hysteresis_c || !out_cooling_limited_margin_c || !out_cooling_limited_hold_s ||
        !out_ramp_lock_band_c || profile_index >= s_zones.cfg.timing_profile_count) {
        return false;
    }
    const zone_timing_profile_t *tp = &s_zones.cfg.timing_profiles[profile_index];
    strncpy(out_name, tp->name, name_cap - 1);
    out_name[name_cap - 1] = '\0';
    *out_progress_duty_min = tp->guard_progress_duty_min;
    *out_progress_window_s = tp->guard_progress_window_s;
    *out_drift_hysteresis_c = tp->guard_drift_hysteresis_c;
    *out_frozen_eps_c = tp->guard_frozen_eps_c;
    *out_cross_zone_period_s = tp->guard_cross_zone_period_s;
    *out_bangbang_hysteresis_c = tp->bangbang_hysteresis_c;
    *out_cooling_limited_margin_c = tp->cooling_limited_margin_c;
    *out_cooling_limited_hold_s = tp->cooling_limited_hold_s;
    *out_ramp_lock_band_c = tp->ramp_lock_band_c;
    return true;
}

bool zones_config_set_timing_profile_raw_no_save(uint8_t profile_index, const char *name,
                                         float progress_duty_min, float progress_window_s,
                                         float drift_hysteresis_c, float frozen_eps_c,
                                         float cross_zone_period_s, float bangbang_hysteresis_c,
                                         float cooling_limited_margin_c, float cooling_limited_hold_s,
                                         float ramp_lock_band_c)
{
    if (!name || profile_index > s_zones.cfg.timing_profile_count ||
        profile_index >= MAX31856_CHANNEL_COUNT || strlen(name) > TIMING_PROFILE_NAME_MAX_LEN) {
        return false;
    }
    if (!isfinite(progress_duty_min) || progress_duty_min < 0.0f || progress_duty_min > ZONE_GUARD_DUTY_MAX) {
        return false;
    }
    if (!isfinite(progress_window_s) || progress_window_s < 0.0f || progress_window_s > ZONE_GUARD_TIME_S_MAX) {
        return false;
    }
    if (!isfinite(drift_hysteresis_c) || drift_hysteresis_c < 0.0f || drift_hysteresis_c > ZONE_GUARD_MARGIN_C_MAX) {
        return false;
    }
    if (!isfinite(frozen_eps_c) || frozen_eps_c < 0.0f || frozen_eps_c > ZONE_GUARD_EPS_C_MAX) {
        return false;
    }
    if (!isfinite(cross_zone_period_s) || cross_zone_period_s < 0.0f ||
        cross_zone_period_s > ZONE_GUARD_TIME_S_MAX) {
        return false;
    }
    if (!isfinite(bangbang_hysteresis_c) || bangbang_hysteresis_c < 0.0f ||
        bangbang_hysteresis_c > ZONE_GUARD_MARGIN_C_MAX) {
        return false;
    }
    if (!isfinite(cooling_limited_margin_c) || cooling_limited_margin_c < 0.0f ||
        cooling_limited_margin_c > ZONE_GUARD_MARGIN_C_MAX) {
        return false;
    }
    if (!isfinite(cooling_limited_hold_s) || cooling_limited_hold_s < 0.0f ||
        cooling_limited_hold_s > ZONE_GUARD_TIME_S_MAX) {
        return false;
    }
    if (!isfinite(ramp_lock_band_c) || ramp_lock_band_c < 0.0f || ramp_lock_band_c > ZONE_GUARD_MARGIN_C_MAX) {
        return false;
    }
    zone_timing_profile_t *tp = &s_zones.cfg.timing_profiles[profile_index];
    zones_cfg_lock();
    strncpy(tp->name, name, TIMING_PROFILE_NAME_MAX_LEN);
    tp->name[TIMING_PROFILE_NAME_MAX_LEN] = '\0';
    tp->guard_progress_duty_min = progress_duty_min;
    tp->guard_progress_window_s = progress_window_s;
    tp->guard_drift_hysteresis_c = drift_hysteresis_c;
    tp->guard_frozen_eps_c = frozen_eps_c;
    tp->guard_cross_zone_period_s = cross_zone_period_s;
    tp->bangbang_hysteresis_c = bangbang_hysteresis_c;
    tp->cooling_limited_margin_c = cooling_limited_margin_c;
    tp->cooling_limited_hold_s = cooling_limited_hold_s;
    tp->ramp_lock_band_c = ramp_lock_band_c;
    if (profile_index == s_zones.cfg.timing_profile_count) {
        s_zones.cfg.timing_profile_count = (uint8_t)(profile_index + 1);
    }
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_timing_profile_raw(uint8_t profile_index, const char *name,
                                         float progress_duty_min, float progress_window_s,
                                         float drift_hysteresis_c, float frozen_eps_c,
                                         float cross_zone_period_s, float bangbang_hysteresis_c,
                                         float cooling_limited_margin_c, float cooling_limited_hold_s,
                                         float ramp_lock_band_c)
{
    if (!zones_config_set_timing_profile_raw_no_save(profile_index, name, progress_duty_min, progress_window_s, drift_hysteresis_c, frozen_eps_c, cross_zone_period_s, bangbang_hysteresis_c, cooling_limited_margin_c, cooling_limited_hold_s, ramp_lock_band_c)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

/* ZONES_CFG_VERSION 16->17: PER-ZONE as of this pass (was a single global
 * scalar at 15->16 -- see zone_cfg_t::ease_off_window_mult's own comment for
 * why z0 needed its own reach). Gated on MAX31856_CHANNEL_COUNT, not
 * thermo_count -- same reasoning as zones_config_get_executor_thresholds()
 * above: a caller with a valid zone index should get an answer even for a
 * zone past the currently-configured count, and every other per-zone
 * accessor in this file already follows that rule.
 *
 * Unlike a bare field read, this one is defensive: s_zones.cfg.zones[zone_
 * index].ease_off_window_mult reads 0.0f in the zeroed, not-yet-configured
 * s_zones.cfg (first boot, corrupt NVS, a refused newer-than-firmware blob --
 * see zones_http.c's own zeroing on load failure) -- 0 IS a legal, validated
 * value (the same "use the firmware default" sentinel pc_link_abort_
 * silence_ms uses, see ZONE_EASE_OFF_WINDOW_MULT_MIN/MAX/DEFAULT's own
 * comment), but it is not a value profile_executor_feedforward.c's
 * zone_taper_climb_rate() can safely divide by -- THIS is where the sentinel
 * actually gets resolved into the real 2.0 default, not at storage time.
 * Falling back to the default for anything else outside [MIN, MAX] too (not
 * just exactly 0) covers a hypothetical stored value from before this range
 * was enforced -- always answer with a value zone_taper_climb_rate() can
 * safely use, never with whatever raw bytes happen to be sitting in
 * s_zones.cfg. */
bool zones_config_get_ease_off_window_mult(uint8_t zone_index, float *out_mult)
{
    if (!out_mult || zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    float v = s_zones.cfg.zones[zone_index].ease_off_window_mult;
    if (v == 0.0f) {
        v = ZONE_EASE_OFF_WINDOW_MULT_DEFAULT; /* the sentinel */
    } else if (!isfinite(v) || v < ZONE_EASE_OFF_WINDOW_MULT_MIN || v > ZONE_EASE_OFF_WINDOW_MULT_MAX) {
        v = ZONE_EASE_OFF_WINDOW_MULT_DEFAULT; /* defensive: not a value that should ever be on flash */
    }
    *out_mult = v;
    return true;
}

/* Writer for the getter above. Refused, never clamped, matching every other
 * setter in this file (e.g. zones_config_set_coupling_diag_k_dc() just
 * below) -- an A/B campaign that asks for an out-of-range multiplier needs
 * to find out its request was rejected, not silently get a different number
 * substituted for it. 0 is accepted (same sentinel zones_config_json_
 * validate() allows) as an explicit "reset to the firmware default" -- an
 * A/B campaign ending an arm should be able to ask for that directly rather
 * than having to know and pass 2.0 by hand. Setting one zone's value never
 * touches any other zone's -- that independence is the entire point of this
 * pass. */
bool zones_config_set_ease_off_window_mult_no_save(uint8_t zone_index, float mult)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || !isfinite(mult) ||
        (mult != 0.0f && (mult < ZONE_EASE_OFF_WINDOW_MULT_MIN || mult > ZONE_EASE_OFF_WINDOW_MULT_MAX))) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].ease_off_window_mult = mult;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_ease_off_window_mult(uint8_t zone_index, float mult)
{
    if (!zones_config_set_ease_off_window_mult_no_save(zone_index, mult)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

/* ZONES_CFG_VERSION 17->18 (PID_EXPANSION_PLAN.md sec 3.6d / PER_ZONE_
 * TARGET_DESIGN_STUDY.md option (b)): per-zone approach-rate cap. Gated on
 * MAX31856_CHANNEL_COUNT, same reasoning as zones_config_get_ease_off_
 * window_mult() just above.
 *
 * UNLIKE that getter, 0 is returned VERBATIM here, not resolved into some
 * other in-range value -- 0 IS "uncapped," the field's own documented
 * meaning (see ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN's own comment for why
 * there is no sensible default cap to substitute the way ease_off_window_
 * mult substitutes 2.0). A stored value that is finite, nonzero, and
 * outside [MIN, MAX] -- only reachable via direct NVS tampering or a
 * rollback from firmware with a wider range -- is defensively treated as
 * uncapped (0), the safe answer for THIS field, rather than clamped into
 * range (which would silently narrow the shared ramp for a value nobody
 * actually configured). */
bool zones_config_get_approach_rate_cap_c_per_hr(uint8_t zone_index, float *out_cap_c_per_hr)
{
    if (!out_cap_c_per_hr || zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    float v = s_zones.cfg.zones[zone_index].approach_rate_cap_c_per_hr;
    if (v != 0.0f && (!isfinite(v) || v < ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN ||
                       v > ZONE_APPROACH_RATE_CAP_C_PER_HR_MAX)) {
        v = 0.0f; /* defensive: not a value that should ever be on flash -- treat as uncapped */
    }
    *out_cap_c_per_hr = v;
    return true;
}

/* Writer for the getter above. Refused, never clamped, matching every other
 * setter in this file. 0 is accepted as an explicit "remove this zone's cap"
 * -- an A/B campaign ending an arm should be able to ask for that directly.
 * Setting one zone's value never touches any other zone's. */
bool zones_config_set_approach_rate_cap_c_per_hr_no_save(uint8_t zone_index, float cap_c_per_hr)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || !isfinite(cap_c_per_hr) ||
        (cap_c_per_hr != 0.0f && (cap_c_per_hr < ZONE_APPROACH_RATE_CAP_C_PER_HR_MIN ||
                                   cap_c_per_hr > ZONE_APPROACH_RATE_CAP_C_PER_HR_MAX))) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].approach_rate_cap_c_per_hr = cap_c_per_hr;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_approach_rate_cap_c_per_hr(uint8_t zone_index, float cap_c_per_hr)
{
    if (!zones_config_set_approach_rate_cap_c_per_hr_no_save(zone_index, cap_c_per_hr)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

/* ZONES_CFG_VERSION 18->19 (PID_EXPANSION_PLAN.md sec 3.6g): per-zone
 * fuzzy-PID membership-band widths. Gated on MAX31856_CHANNEL_COUNT, same
 * reasoning as zones_config_get_ease_off_window_mult() above.
 *
 * Like that getter (and UNLIKE zones_config_get_approach_rate_cap_c_per_hr()
 * just above), 0 -- and anything else outside [MIN, MAX] -- resolves to the
 * field's own firmware DEFAULT: there is no "no band" answer a fuzzy
 * membership function can give the way "uncapped" is a real answer for a
 * rate limiter, so this getter always hands the caller a finite, positive
 * width it can pass straight to pid_fuzzy_adjust(). */
bool zones_config_get_error_band_c(uint8_t zone_index, float *out_band_c)
{
    if (!out_band_c || zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    float v = s_zones.cfg.zones[zone_index].error_band_c;
    if (v == 0.0f) {
        v = ZONE_ERROR_BAND_C_DEFAULT; /* the sentinel */
    } else if (!isfinite(v) || v < ZONE_ERROR_BAND_C_MIN || v > ZONE_ERROR_BAND_C_MAX) {
        v = ZONE_ERROR_BAND_C_DEFAULT; /* defensive: not a value that should ever be on flash */
    }
    *out_band_c = v;
    return true;
}

/* Writer for the getter above. Refused, never clamped, matching every other
 * setter in this file. 0 is accepted as an explicit "reset to the firmware
 * default." Setting one zone's value never touches any other zone's. */
bool zones_config_set_error_band_c_no_save(uint8_t zone_index, float band_c)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || !isfinite(band_c) ||
        (band_c != 0.0f && (band_c < ZONE_ERROR_BAND_C_MIN || band_c > ZONE_ERROR_BAND_C_MAX))) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].error_band_c = band_c;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_error_band_c(uint8_t zone_index, float band_c)
{
    if (!zones_config_set_error_band_c_no_save(zone_index, band_c)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

/* Same shape as zones_config_get_error_band_c()/_set_error_band_c() just
 * above, for the rate axis. */
bool zones_config_get_rate_band_c_per_s(uint8_t zone_index, float *out_band_c_per_s)
{
    if (!out_band_c_per_s || zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    float v = s_zones.cfg.zones[zone_index].rate_band_c_per_s;
    if (v == 0.0f) {
        v = ZONE_RATE_BAND_C_PER_S_DEFAULT; /* the sentinel */
    } else if (!isfinite(v) || v < ZONE_RATE_BAND_C_PER_S_MIN || v > ZONE_RATE_BAND_C_PER_S_MAX) {
        v = ZONE_RATE_BAND_C_PER_S_DEFAULT; /* defensive: not a value that should ever be on flash */
    }
    *out_band_c_per_s = v;
    return true;
}

bool zones_config_set_rate_band_c_per_s_no_save(uint8_t zone_index, float band_c_per_s)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || !isfinite(band_c_per_s) ||
        (band_c_per_s != 0.0f &&
         (band_c_per_s < ZONE_RATE_BAND_C_PER_S_MIN || band_c_per_s > ZONE_RATE_BAND_C_PER_S_MAX))) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].rate_band_c_per_s = band_c_per_s;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_rate_band_c_per_s(uint8_t zone_index, float band_c_per_s)
{
    if (!zones_config_set_rate_band_c_per_s_no_save(zone_index, band_c_per_s)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

/* Getter for zone_cfg_t::relay_type (ZONES_CFG_VERSION 19->20,
 * RELAY_LIFE_BUDGET.md) -- unlike error_band_c/rate_band_c_per_s,
 * there is no "resolve to a firmware default" step: an out-of-range stored
 * byte (only reachable via direct NVS tampering or a rollback from newer
 * firmware with a wider range) defensively reads back as RELAY_TYPE_SSR (0),
 * the same "no budget" answer relay_cycles_budget() gives any relay it
 * cannot make sense of, rather than fabricating a contactor/mercury budget
 * out of a byte that was never really one of those values. */
bool zones_config_get_relay_type(uint8_t zone_index, uint8_t *out_relay_type)
{
    if (!out_relay_type || zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    uint8_t v = s_zones.cfg.zones[zone_index].relay_type;
    if (v > ZONE_RELAY_TYPE_MAX) {
        v = 0; /* defensive: not a value that should ever be on flash */
    }
    *out_relay_type = v;
    return true;
}

/* Writer for the getter above. Refused, never clamped, matching every other
 * setter in this file. On success, pushes the new type to every relay named
 * in this zone's relay_mask via relay_cycles_set_type() -- RELAY_LIFE_
 * BUDGET_PLAN.md step 2's "on every successful save" requirement -- with
 * rated_override left at 0 (use the type's table value; there is no operator
 * override UI yet). The load path (nvs_load_from(), zones_config_store.c)
 * makes the identical per-zone push directly after a successful load rather
 * than routing through this setter, since a load has no "successful save" of
 * its own to gate the push on. */
/* Mutation-only half of zones_config_set_relay_type() below -- see
 * zones_config_accessors.h's doc comment on the _no_save convention. Does
 * NOT save to NVS and does NOT push the new type to relay_cycles_set_type():
 * a batched caller (backup_import.c) calls zones_config_save_now() once
 * after its whole batch and pushes the type itself afterward -- see
 * backup_import.c's own commit-loop comment. */
bool zones_config_set_relay_type_no_save(uint8_t zone_index, uint8_t relay_type)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || relay_type > ZONE_RELAY_TYPE_MAX) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].relay_type = relay_type;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_relay_type(uint8_t zone_index, uint8_t relay_type)
{
    if (!zones_config_set_relay_type_no_save(zone_index, relay_type)) {
        return false;
    }
    if (nvs_save() != ESP_OK) {
        return false;
    }
    zones_config_push_relay_type(zone_index);
    return true;
}

/* Getter for zone_cfg_t::progress_band_c (ZONES_CFG_VERSION 21->22,
 * docs/audits/consumer_without_producer_2026-09-06.md finding 1) -- same
 * shape as zones_config_get_error_band_c() just above: 0 and anything
 * outside [MIN, MAX] resolve to ZONE_PROGRESS_BAND_C_DEFAULT, since there is
 * no "band disabled" state guard 1's arrival test can accept. */
bool zones_config_get_progress_band_c(uint8_t zone_index, float *out_band_c)
{
    if (!out_band_c || zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    float v = s_zones.cfg.zones[zone_index].progress_band_c;
    if (v == 0.0f) {
        v = ZONE_PROGRESS_BAND_C_DEFAULT; /* the sentinel */
    } else if (!isfinite(v) || v < ZONE_PROGRESS_BAND_C_MIN || v > ZONE_PROGRESS_BAND_C_MAX) {
        v = ZONE_PROGRESS_BAND_C_DEFAULT; /* defensive: not a value that should ever be on flash */
    }
    *out_band_c = v;
    return true;
}

/* Writer for the getter above. Refused, never clamped, matching every other
 * setter in this file. 0 is accepted as an explicit "reset to the firmware
 * default." Setting one zone's value never touches any other zone's. */
bool zones_config_set_progress_band_c_no_save(uint8_t zone_index, float band_c)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT || !isfinite(band_c) ||
        (band_c != 0.0f && (band_c < ZONE_PROGRESS_BAND_C_MIN || band_c > ZONE_PROGRESS_BAND_C_MAX))) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].progress_band_c = band_c;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_progress_band_c(uint8_t zone_index, float band_c)
{
    if (!zones_config_set_progress_band_c_no_save(zone_index, band_c)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

/* Bundled setter, same "reject nothing half-written" discipline as every
 * bundled setter above. Each of the 8 fields checked against its own
 * independent bound (matching which ceiling parse_zone_fields() applies to
 * that specific key) -- no cross-field check between any pair of these 8,
 * matching the POST authority's own lack of one. */
bool zones_config_set_guard_thresholds_no_save(uint8_t zone_index, float wrong_dir_window_s,
                                       float wrong_dir_rate_c_per_min, float off_settle_s,
                                       float runaway_rate_c_per_min, float runaway_margin_c,
                                       float drift_period_s, float sensor_fault_debounce_ticks,
                                       float frozen_window_s)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(wrong_dir_window_s) || wrong_dir_window_s < 0.0f || wrong_dir_window_s > ZONE_GUARD_TIME_S_MAX) {
        return false;
    }
    if (!isfinite(wrong_dir_rate_c_per_min) || wrong_dir_rate_c_per_min < 0.0f ||
        wrong_dir_rate_c_per_min > ZONE_GUARD_RATE_C_PER_MIN_MAX) {
        return false;
    }
    if (!isfinite(off_settle_s) || off_settle_s < 0.0f || off_settle_s > ZONE_GUARD_TIME_S_MAX) {
        return false;
    }
    if (!isfinite(runaway_rate_c_per_min) || runaway_rate_c_per_min < 0.0f ||
        runaway_rate_c_per_min > ZONE_GUARD_RATE_C_PER_MIN_MAX) {
        return false;
    }
    if (!isfinite(runaway_margin_c) || runaway_margin_c < 0.0f || runaway_margin_c > ZONE_GUARD_MARGIN_C_MAX) {
        return false;
    }
    if (!isfinite(drift_period_s) || drift_period_s < 0.0f || drift_period_s > ZONE_GUARD_TIME_S_MAX) {
        return false;
    }
    if (!isfinite(sensor_fault_debounce_ticks) || sensor_fault_debounce_ticks < 0.0f ||
        sensor_fault_debounce_ticks > ZONE_GUARD_DEBOUNCE_TICKS_MAX) {
        return false;
    }
    if (!isfinite(frozen_window_s) || frozen_window_s < 0.0f || frozen_window_s > ZONE_GUARD_TIME_S_MAX) {
        return false;
    }
    zones_cfg_lock();
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->guard_wrong_dir_window_s = wrong_dir_window_s;
    z->guard_wrong_dir_rate_c_per_min = wrong_dir_rate_c_per_min;
    z->guard_off_settle_s = off_settle_s;
    z->guard_runaway_rate_c_per_min = runaway_rate_c_per_min;
    z->guard_runaway_margin_c = runaway_margin_c;
    z->guard_drift_period_s = drift_period_s;
    z->guard_sensor_fault_debounce_ticks = sensor_fault_debounce_ticks;
    z->guard_frozen_window_s = frozen_window_s;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_guard_thresholds(uint8_t zone_index, float wrong_dir_window_s,
                                       float wrong_dir_rate_c_per_min, float off_settle_s,
                                       float runaway_rate_c_per_min, float runaway_margin_c,
                                       float drift_period_s, float sensor_fault_debounce_ticks,
                                       float frozen_window_s)
{
    if (!zones_config_set_guard_thresholds_no_save(zone_index, wrong_dir_window_s, wrong_dir_rate_c_per_min, off_settle_s, runaway_rate_c_per_min, runaway_margin_c, drift_period_s, sensor_fault_debounce_ticks, frozen_window_s)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_cross_zone_delta(uint8_t zone_index, float *out_max_delta_c)
{
    if (!out_max_delta_c || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_max_delta_c = s_zones.cfg.zones[zone_index].cross_zone_max_delta_c;
    return true;
}

/* Same bound parse_zone_fields()'s z%u_xzone enforces. 0 is legal (the
 * documented "guard disabled" encoding). */
bool zones_config_set_cross_zone_delta_no_save(uint8_t zone_index, float max_delta_c)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(max_delta_c) || max_delta_c < 0.0f || max_delta_c > ZONE_CROSS_ZONE_DELTA_C_MAX) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].cross_zone_max_delta_c = max_delta_c;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_cross_zone_delta(uint8_t zone_index, float max_delta_c)
{
    if (!zones_config_set_cross_zone_delta_no_save(zone_index, max_delta_c)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_model(uint8_t zone_index, float *out_k_dc, float *out_tau_s, float *out_dead_time_s)
{
    if (!out_k_dc || !out_tau_s || !out_dead_time_s || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    *out_k_dc = z->model_k_dc;
    *out_tau_s = z->model_tau_s;
    *out_dead_time_s = z->model_dead_time_s;
    return true;
}

bool zones_config_set_model_no_save(uint8_t zone_index, float k_dc, float tau_s, float dead_time_s)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    /* Same reject-without-writing-anything discipline as
     * zones_config_set_pid(): a caller handing us one bad number must not
     * end up with two of the three fields updated, because a half-written
     * model is indistinguishable from a whole one to every reader and would
     * feed the feedforward term a gain from one run and a tau from another.
     *
     * Note this deliberately accepts an all-zero triple: that is the
     * documented "no model" encoding, so writing it is how a caller clears a
     * stale model rather than a validation failure. A NEGATIVE value is
     * rejected outright -- a heater with negative static gain, or a plant
     * that responds before it is driven, is a fit that went wrong, not a
     * kiln. */
    if (!isfinite(k_dc) || !isfinite(tau_s) || !isfinite(dead_time_s) || k_dc < 0.0f || tau_s < 0.0f ||
        dead_time_s < 0.0f || k_dc > ZONE_MODEL_K_MAX || tau_s > ZONE_MODEL_TIME_MAX_S ||
        dead_time_s > ZONE_MODEL_TIME_MAX_S) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    zones_cfg_lock();
    z->model_k_dc = k_dc;
    z->model_tau_s = tau_s;
    z->model_dead_time_s = dead_time_s;
    /* Bumped before the NVS write for the same reason set_pid does it: the
     * model is live for the next control tick regardless of whether it
     * reaches flash, and a feedforward term computed from a stale K while a
     * firing is running is precisely what TODO.md 6A.7's reload path
     * exists to prevent. */
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_model(uint8_t zone_index, float k_dc, float tau_s, float dead_time_s)
{
    if (!zones_config_set_model_no_save(zone_index, k_dc, tau_s, dead_time_s)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

/* Accepts the ZONE_MODEL_FIT_TEMP_UNKNOWN sentinel outright (that is the
 * documented "no context recorded" encoding, same "sentinel is always
 * legal" convention zones_config_set_model()'s all-zero triple uses); any
 * other value must be finite and inside a generous physically-plausible
 * kiln range, wide enough to never reject a real measurement while still
 * catching a typo/garbage value. */
static bool zone_model_fit_temp_valid(float v)
{
    if (v == ZONE_MODEL_FIT_TEMP_UNKNOWN) {
        return true;
    }
    return isfinite(v) && v > -50.0f && v < 1300.0f;
}

bool zones_config_get_model_fit_context(uint8_t zone_index, float *out_fit_temp_c, float *out_fit_ambient_c)
{
    if (!out_fit_temp_c || !out_fit_ambient_c || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    *out_fit_temp_c = z->model_fit_temp_c;
    *out_fit_ambient_c = z->model_fit_ambient_c;
    return true;
}

bool zones_config_set_model_fit_context_no_save(uint8_t zone_index, float fit_temp_c, float fit_ambient_c)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!zone_model_fit_temp_valid(fit_temp_c) || !zone_model_fit_temp_valid(fit_ambient_c)) {
        return false;
    }
    zones_cfg_lock();
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    z->model_fit_temp_c = fit_temp_c;
    z->model_fit_ambient_c = fit_ambient_c;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_model_fit_context(uint8_t zone_index, float fit_temp_c, float fit_ambient_c)
{
    if (!zones_config_set_model_fit_context_no_save(zone_index, fit_temp_c, fit_ambient_c)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

bool zones_config_get_autotune_baseline_k_dc(uint8_t zone_index, float *out_k_dc)
{
    if (!out_k_dc || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    *out_k_dc = s_zones.cfg.zones[zone_index].autotune_baseline_k_dc;
    return true;
}

bool zones_config_set_autotune_baseline_k_dc_no_save(uint8_t zone_index, float k_dc)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    /* 0 is always legal (the "no baseline recorded" sentinel); any other
     * value must be finite, non-negative, and inside ZONE_AUTOTUNE_K_DC_MAX
     * -- see that constant's own comment for the physical justification.
     * Same reject-nothing-half-applied discipline as every other setter in
     * this file, though there is only one field here to half-apply. */
    if (!isfinite(k_dc) || k_dc < 0.0f || k_dc > ZONE_AUTOTUNE_K_DC_MAX) {
        return false;
    }
    zones_cfg_lock();
    s_zones.cfg.zones[zone_index].autotune_baseline_k_dc = k_dc;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_set_autotune_baseline_k_dc(uint8_t zone_index, float k_dc)
{
    if (!zones_config_set_autotune_baseline_k_dc_no_save(zone_index, k_dc)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

/* Passthrough seams -- see their own header comment (zones_config_accessors.h)
 * for why T_c is accepted but not yet used. (void)-cast rather than an
 * unnamed parameter so the seam's future consumer is easy to grep for. */
bool zone_model_at(uint8_t zone_index, float T_c, float *out_k_dc, float *out_tau_s, float *out_dead_time_s)
{
    (void)T_c;
    return zones_config_get_model(zone_index, out_k_dc, out_tau_s, out_dead_time_s);
}

bool coupling_at(uint8_t zone_index, float T_c, float out_row[MAX31856_CHANNEL_COUNT])
{
    (void)T_c;
    return zones_config_get_coupling(zone_index, out_row);
}

bool zones_config_get_tuning_quality(uint8_t zone_index, zone_tuning_quality_t *out)
{
    if (!out || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    const zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    out->valid = z->tuning_valid != 0;
    out->method = z->tuning_method;
    out->rule = z->tuning_rule;
    out->settled = z->tuning_settled != 0;
    out->extrapolation_converged = z->tuning_extrapolation_converged != 0;
    out->tau_consistent = z->tuning_tau_consistent != 0;
    out->baseline_c = z->tuning_baseline_c;
    out->step_ambient_c = z->tuning_step_ambient_c;
    out->raw_rise_c = z->tuning_raw_rise_c;
    out->rise_inf_c = z->tuning_rise_inf_c;
    return true;
}

bool zones_config_set_tuning_quality_no_save(uint8_t zone_index, const zone_tuning_quality_t *q)
{
    /* q->valid must be true -- see this setter's own header comment
     * (zones_http.h) for why a caller wanting to CLEAR the record uses
     * zones_config_set_pid() instead, and why this setter refuses rather
     * than silently writing a "set but empty" record. */
    if (!q || !q->valid || zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    if (!isfinite(q->baseline_c) || !isfinite(q->step_ambient_c) || !isfinite(q->raw_rise_c) ||
        !isfinite(q->rise_inf_c) || q->method > 1 || q->rule > 3) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    zones_cfg_lock();
    z->tuning_valid = 1;
    z->tuning_method = q->method;
    z->tuning_rule = q->rule;
    z->tuning_settled = q->settled ? 1 : 0;
    z->tuning_extrapolation_converged = q->extrapolation_converged ? 1 : 0;
    z->tuning_tau_consistent = q->tau_consistent ? 1 : 0;
    z->tuning_baseline_c = q->baseline_c;
    z->tuning_step_ambient_c = q->step_ambient_c;
    z->tuning_raw_rise_c = q->raw_rise_c;
    z->tuning_rise_inf_c = q->rise_inf_c;
    /* Self-incrementing -- the caller does not supply a seq, see this
     * function's own header comment (zones_http.h) for why. */
    z->tuning_seq++;
    s_config_generation++;
    zones_cfg_unlock();
    return true;
}

bool zones_config_reinstate_tuning_quality_no_save(uint8_t zone_index)
{
    if (zone_index >= s_zones.cfg.thermo_count) {
        return false;
    }
    zone_cfg_t *z = &s_zones.cfg.zones[zone_index];
    if (!z->tuning_valid) {
        zones_cfg_lock();
        z->tuning_valid = 1;
        s_config_generation++;
        zones_cfg_unlock();
    }
    return true;
}

bool zones_config_set_tuning_quality(uint8_t zone_index, const zone_tuning_quality_t *q)
{
    if (!zones_config_set_tuning_quality_no_save(zone_index, q)) {
        return false;
    }
    return nvs_save() == ESP_OK;
}

float zones_config_apply_cal(uint8_t zone_index, float raw_c)
{
    if (isnan(raw_c) || zone_index >= s_zones.cfg.thermo_count) {
        return raw_c;
    }
    return raw_c + s_zones.cfg.zones[zone_index].cal_offset_c;
}

/* ---- Whole-config export/import for kiln_cfg_store.c (see zones_http.h's
 * doc comment on this pair for the full rationale) -------------------------- */

_Static_assert(sizeof(zones_cfg_t) <= ZONES_CONFIG_BLOB_MAX_SIZE,
               "zones_cfg_t grew past ZONES_CONFIG_BLOB_MAX_SIZE -- widen the macro in "
               "zones_http.h (existing kiln_cfg_store entries keep their old, smaller blob "
               "size until re-saved, same discipline as ZONES_CFG_VERSION migrations)");

size_t zones_config_blob_size(void)
{
    return sizeof(zones_cfg_t);
}

bool zones_config_export_blob(void *out, size_t out_cap)
{
    if (!out || out_cap < sizeof(s_zones.cfg)) {
        return false;
    }
    memcpy(out, &s_zones.cfg, sizeof(s_zones.cfg));
    return true;
}

bool zones_config_import_blob(const void *blob, size_t len, char *reason_out, size_t reason_cap)
{
    if (reason_out && reason_cap) {
        reason_out[0] = '\0';
    }

    /* Same decoder nvs_load_from() uses -- length-vs-claimed-version check
     * before anything is interpreted, typed per-version conversion (never a
     * memcpy of one struct shape over another), zones_config_json_validate(), and a
     * CRC check on the current-version path. This blob may have been saved
     * years ago by older firmware under looser bounds (kiln_cfg_store.c), so
     * it gets exactly the same scrutiny a blob read off flash does -- no
     * separate, looser path for "this one came from a kiln config slot
     * instead of the live NVS key." */
    zones_cfg_t cand;
    const char *reason = "";
    zones_decode_result_t result = zones_config_json_decode_blob(blob, len, &cand, &reason);
    if (result != ZONES_DECODE_OK) {
        if (reason_out && reason_cap) {
            snprintf(reason_out, reason_cap, "%s", reason);
        }
        return false;
    }

    /* Pass 1 for settings_source: zones_config_json_decode_blob()/zones_config_json_validate()
     * do not check for an inheritance cycle (they're shared with the LOAD
     * path, which must collapse rather than reject one -- see
     * zones_config_json_normalize_settings_source_cycles()'s comment). The import path is
     * different: there is a live client on the other end of this call
     * (backup_http.c's importer) who can be handed a clear reason, so this
     * is refused HERE, before the commit point below, matching this file's
     * own two-pass-import discipline (reject in pass 1, never fail
     * mid-commit) rather than silently rewriting a hand-edited backup's
     * cycle to Custom underneath the operator. */
    {
    uint8_t import_thermo_count = cand.thermo_count > MAX31856_CHANNEL_COUNT ? MAX31856_CHANNEL_COUNT
                                                                             : cand.thermo_count;
    for (uint8_t group = 0; group < SRC_GROUP_COUNT; group++) {
        for (uint8_t i = 0; i < import_thermo_count; i++) {
            if (zones_config_json_settings_source_chain_has_cycle(cand.zones, group, i, import_thermo_count)) {
                if (reason_out && reason_cap) {
                    snprintf(reason_out, reason_cap,
                             "zone %u's settings_source (group %u) forms an inheritance cycle", (unsigned)i,
                             (unsigned)group);
                }
                return false;
            }
        }
    }
    }

    /* Commit point -- everything above only touched `cand`, a local scratch
     * copy; nothing has been written to s_zones or NVS until this line, so
     * any rejection above (bad version, bad size, any one field out of
     * range) leaves the live config completely untouched. Same
     * all-or-nothing discipline as zones_post_handler()'s own commit
     * point. */
    zones_cfg_lock();
    s_zones.cfg = cand;
    s_zones_config_valid = true;
    s_config_generation++;
    zones_cfg_unlock();
    esp_err_t err = nvs_save();
    if (err != ESP_OK) {
        ESP_LOGE(ZONES_HTTP_TAG, "nvs_save after kiln-config apply failed: %s -- config applied live but "
                      "will not survive a reboot",
                 esp_err_to_name(err));
    }
    return true;
}


/* ---- Canonical (padding-free) serialization -- H1 -------------------------
 * docs/audits/kiln_profiles_robustness_2026-09-14.md,
 * docs/audits/kiln_package_canonical_serializer_2026-09-14.md.
 * See zones_config_accessors.h's own comment on this pair for the "why" and
 * the round-trip contract. This block is the "how", and specifically how it
 * is made impossible to silently forget a field.
 *
 * THE CONSTRUCTION: one X-macro field-descriptor TABLE per struct --
 * ZONE_TIMING_PROFILE_FIELDS / ZONE_CFG_FIELDS / ZONES_CFG_FIELDS just below
 * -- each entry `F(KIND, NAME, COUNT)` naming one field's wire kind, its
 * member name, and its element count (1 for a scalar). Each table is
 * expanded FOUR times, by four different consumers of the SAME list:
 *   1. ZCFG_SHADOW_DECL -- declares a "shadow" struct with an identical
 *      field list, in the identical order, using ordinary (non-packed) C
 *      struct declarations -- so the compiler lays it out with the exact
 *      same alignment rules as the real struct.
 *   2. The _Static_assert block right after each shadow struct -- proves,
 *      per field AND for total size, that the shadow struct's layout is
 *      byte-for-byte identical to the corresponding real struct
 *      (zone_timing_profile_t / zone_cfg_t / zones_cfg_t). This is the
 *      completeness guard: a real-struct field that is NOT in the table is
 *      not in the shadow struct either, so from that field's position
 *      onward either an offsetof() comparison mismatches or (if the missing
 *      field is the very last one) the whole-struct sizeof() comparison
 *      mismatches. EITHER WAY THE BUILD FAILS, naming this comment block.
 *      No hand-computed literal offsets anywhere in this proof -- every
 *      number comes from the compiler comparing two structs it just laid
 *      out itself, so there is no transcription-typo risk the way a
 *      hardcoded `offsetof(...) == 188` style assert (used elsewhere in
 *      this codebase for FROZEN historical layouts, where the numbers are
 *      deliberately pinned forever) would have.
 *   3. ZCFG_EMIT -- the encoder: walks the table in order, calling one
 *      zcfg_emit_KIND() helper per entry against the REAL struct's own
 *      field (never the shadow struct, which exists only for the
 *      compile-time proof above and is otherwise unused).
 *   4. ZCFG_PARSE_KIND -- the decoder, symmetric.
 * A field that exists in the table but is wired up wrong (wrong KIND, wrong
 * COUNT) is caught by ordinary C type errors in the emit/parse expansion or
 * by the round-trip fidelity test; a field that exists in the real struct
 * but is MISSING from the table is caught by #2 above, at compile time,
 * before any test can even run. This is the same "un-forgettable" property
 * kiln_package_capture_pico_half() already has via CONFIG_PARAM_TABLE's own
 * enumerable count/get-by-index -- here achieved by making the struct itself
 * the enumeration authority, checked by reconstruction, since zone_cfg_t has
 * no equivalent runtime introspection table to walk.
 *
 * THE ONE GAP THIS DOES NOT CLOSE, STATED PLAINLY: a field inserted into the
 * real struct whose size exactly fills an existing alignment-padding gap
 * WITHOUT changing the offset of any field after it, and without changing
 * the struct's total size, would not be caught -- the shadow struct would
 * still match byte-for-byte because the "extra" field lives entirely inside
 * bytes neither struct's field list claims. This cannot happen for any field
 * type this codebase actually declares in these structs: every gap in
 * zone_cfg_t/zones_cfg_t is 0-3 bytes (this struct's largest alignment
 * requirement is 4, for uint32_t/float), and every field this project has
 * ever added is itself >= 1 byte with the SAME alignment as its neighbours
 * (uint8_t/uint16_t/uint32_t/float, never a sub-byte bitfield) -- inserting
 * a real field always either exactly fills a small gap AND shifts every
 * following field (still caught, because the assert chain covers every
 * field after the insertion point) or grows the struct (caught by the tail
 * size assert). The only way to defeat this proof is to insert a field
 * whose own size, in isolation, happens to equal the gap AND to insert it as
 * the struct's textually LAST field where there is nothing after it to
 * shift -- but a gap can only exist BETWEEN two fields (alignment padding
 * has no reason to exist after the last field beyond the struct's own
 * trailing alignment, which the tail assert already covers), so this case
 * does not arise in practice. Documented here rather than silently assumed,
 * per this task's own "say plainly how yours fails loudly" requirement.
 *
 * Byte order: everything little-endian. Multi-byte integers via
 * zcfg_put_u16/zcfg_put_u32; floats via zcfg_put_f32, which flushes -0.0f to
 * +0.0f FIRST (plan section 3.1.3 rule 4) so a zone whose float happens to
 * be negative zero hashes identically to the same zone with an ordinary
 * +0.0f -- the two are numerically and operationally identical, and a hash
 * that distinguished them would treat an unmeasurable non-difference as a
 * divergence. */

#define ZONE_TIMING_PROFILE_FIELDS(F)                                    \
    F(CHARARR, name, (TIMING_PROFILE_NAME_MAX_LEN + 1))                  \
    F(F32, guard_progress_duty_min, 1)                                   \
    F(F32, guard_progress_window_s, 1)                                   \
    F(F32, guard_drift_hysteresis_c, 1)                                  \
    F(F32, guard_frozen_eps_c, 1)                                        \
    F(F32, guard_cross_zone_period_s, 1)                                 \
    F(F32, bangbang_hysteresis_c, 1)                                     \
    F(F32, cooling_limited_margin_c, 1)                                  \
    F(F32, cooling_limited_hold_s, 1)                                    \
    F(F32, ramp_lock_band_c, 1)

/* Field order below is a verbatim transcription of zone_cfg_t's own
 * declaration order in zones_config_json.h (checked field-for-field against
 * that header while writing this table) -- crc32 has no per-zone analogue,
 * so there is nothing to exclude here the way ZONES_CFG_FIELDS excludes
 * zones_cfg_t::crc32 below. */
#define ZONE_CFG_FIELDS(F)                                                \
    F(CHARARR, name, (ZONE_NAME_MAX_LEN + 1))                            \
    F(F32, cal_offset_c, 1)                                              \
    F(F32, pid_kp, 1)                                                    \
    F(F32, pid_ki, 1)                                                    \
    F(F32, pid_kd, 1)                                                    \
    F(F32, max_ramp_c_per_hr, 1)                                         \
    F(F32, sanity_rate_c_per_min, 1)                                     \
    F(F32, max_temp_c, 1)                                                \
    F(F32, min_temp_c, 1)                                                \
    F(F32, heater_window_ms, 1)                                          \
    F(F32, heater_min_on_ms, 1)                                          \
    F(F32, heater_min_off_ms, 1)                                         \
    F(F32, guard_wrong_dir_window_s, 1)                                  \
    F(F32, guard_wrong_dir_rate_c_per_min, 1)                            \
    F(F32, guard_off_settle_s, 1)                                        \
    F(F32, guard_runaway_rate_c_per_min, 1)                              \
    F(F32, guard_runaway_margin_c, 1)                                    \
    F(F32, guard_drift_period_s, 1)                                      \
    F(F32, guard_sensor_fault_debounce_ticks, 1)                         \
    F(F32, guard_frozen_window_s, 1)                                     \
    F(F32, cross_zone_max_delta_c, 1)                                    \
    F(F32, model_k_dc, 1)                                                \
    F(F32, model_tau_s, 1)                                               \
    F(F32, model_dead_time_s, 1)                                         \
    F(F32, fuzzy_strength_pct, 1)                                        \
    F(F32ARR, coupling_coeff, MAX31856_CHANNEL_COUNT)                    \
    F(F32ARR, coupling_tau_s, MAX31856_CHANNEL_COUNT)                    \
    F(F32ARR, coupling_dead_time_s, MAX31856_CHANNEL_COUNT)              \
    F(U8, relay_mask, 1)                                                 \
    F(U8, control_mode, 1)                                               \
    F(U8, tc_type, 1)                                                    \
    F(U8, thermo_mask, 1)                                                \
    F(U8, ct_mask, 1)                                                    \
    F(U8, timing_profile, 1)                                             \
    F(U8ARR, settings_source, SRC_GROUP_COUNT)                           \
    F(U8, tuning_valid, 1)                                               \
    F(U8, tuning_method, 1)                                              \
    F(U8, tuning_rule, 1)                                                \
    F(U8, tuning_settled, 1)                                             \
    F(U8, tuning_extrapolation_converged, 1)                             \
    F(U8, tuning_tau_consistent, 1)                                      \
    F(F32, tuning_baseline_c, 1)                                         \
    F(F32, tuning_step_ambient_c, 1)                                     \
    F(F32, tuning_raw_rise_c, 1)                                         \
    F(F32, tuning_rise_inf_c, 1)                                         \
    F(U32, tuning_seq, 1)                                                \
    F(U8, adaptive_tune_enabled, 1)                                      \
    F(F32, coupling_diag_k_dc, 1)                                        \
    F(F32, ease_off_window_mult, 1)                                      \
    F(F32, approach_rate_cap_c_per_hr, 1)                                \
    F(F32, error_band_c, 1)                                              \
    F(F32, rate_band_c_per_s, 1)                                         \
    F(U8, relay_type, 1)                                                 \
    F(F32, progress_band_c, 1)                                           \
    F(U8, zone_type, 1)                                                  \
    F(U8, failsafe_state, 1)                                             \
    F(F32, hyst_c, 1)                                                    \
    F(U16, min_on_s, 1)                                                  \
    F(U16, min_off_s, 1)                                                 \
    F(F32, model_fit_temp_c, 1)                                          \
    F(F32, model_fit_ambient_c, 1)                                       \
    F(F32, coil_power_w, 1)                                              \
    F(F32, autotune_baseline_k_dc, 1)

/* zones_cfg_t's own top-level fields, verbatim declaration order. `zones`
 * and `timing_profiles` are arrays of the two structs above and are encoded
 * by RECURSING into encode_zone_cfg()/encode_zone_timing_profile() per
 * element -- their own completeness is already proven by their own tables
 * above, so this table does not need to (and must not) re-enumerate their
 * internals. `crc32` is the true tail field and is DELIBERATELY EXCLUDED --
 * see this file's and the header's comments on why (H1 rule 1: a struct
 * hash cannot include itself, and zones_cfg_t::crc32 is out of this pair's
 * scope as its own separate, named follow-up). Because crc32 is excluded,
 * the completeness assert for zones_cfg_t below intentionally compares
 * against the shadow struct's size PLUS sizeof(uint32_t) -- see that assert
 * for the exact statement -- rather than a bare sizeof() equality, so this
 * table's deliberate omission of the true tail field does not itself trip
 * the very guard meant to catch an ACCIDENTAL omission. */
#define ZONES_CFG_FIELDS(F)                                               \
    F(U8, version, 1)                                                    \
    F(U8, thermo_count, 1)                                               \
    F(U8, relay_count, 1)                                                \
    F(U8, max_simultaneous_relays, 1)                                    \
    F(U8, continue_on_zone_trip, 1)                                      \
    F(U8, safety_tc_type, 1)                                             \
    F(ZONEARR, zones, MAX31856_CHANNEL_COUNT)                            \
    F(U8, timing_profile_count, 1)                                       \
    F(TPARR, timing_profiles, MAX31856_CHANNEL_COUNT)                    \
    F(F32, pc_link_abort_silence_ms, 1)

/* ---- 1. Shadow structs -- ordinary (non-packed) mirrors of the field
 * tables above, used ONLY for the compile-time completeness proof in
 * section 2. Never instantiated at runtime. */
#define ZCFG_SHADOW_DECL(KIND, NAME, COUNT) ZCFG_SHADOW_DECL_##KIND(NAME, COUNT)
#define ZCFG_SHADOW_DECL_CHARARR(NAME, COUNT) char NAME[COUNT];
#define ZCFG_SHADOW_DECL_U8(NAME, COUNT) uint8_t NAME;
#define ZCFG_SHADOW_DECL_U8ARR(NAME, COUNT) uint8_t NAME[COUNT];
#define ZCFG_SHADOW_DECL_U16(NAME, COUNT) uint16_t NAME;
#define ZCFG_SHADOW_DECL_U32(NAME, COUNT) uint32_t NAME;
#define ZCFG_SHADOW_DECL_F32(NAME, COUNT) float NAME;
#define ZCFG_SHADOW_DECL_F32ARR(NAME, COUNT) float NAME[COUNT];
#define ZCFG_SHADOW_DECL_ZONEARR(NAME, COUNT) zone_cfg_t NAME[COUNT];
#define ZCFG_SHADOW_DECL_TPARR(NAME, COUNT) zone_timing_profile_t NAME[COUNT];

typedef struct {
    ZONE_TIMING_PROFILE_FIELDS(ZCFG_SHADOW_DECL)
} zone_timing_profile_shadow_t;

typedef struct {
    ZONE_CFG_FIELDS(ZCFG_SHADOW_DECL)
} zone_cfg_shadow_t;

typedef struct {
    ZONES_CFG_FIELDS(ZCFG_SHADOW_DECL)
} zones_cfg_shadow_t; /* deliberately has NO crc32 member -- see ZONES_CFG_FIELDS's comment */

/* ---- 2. The completeness proof -- see this block's own top-of-file
 * comment for the full explanation. Every _Static_assert below compares two
 * structs the compiler just laid out; no literal numbers. */
#define ZCFG_ASSERT_TP(KIND, NAME, COUNT)                                                                 \
    _Static_assert(offsetof(zone_timing_profile_t, NAME) == offsetof(zone_timing_profile_shadow_t, NAME), \
                   "zone_timing_profile_t::" #NAME                                                        \
                   " offset drifted from ZONE_TIMING_PROFILE_FIELDS -- a field was added, removed, "      \
                   "reordered, or retyped without updating that table (zones_config_accessors.c)");
ZONE_TIMING_PROFILE_FIELDS(ZCFG_ASSERT_TP)
_Static_assert(sizeof(zone_timing_profile_t) == sizeof(zone_timing_profile_shadow_t),
               "zone_timing_profile_t's total size does not match ZONE_TIMING_PROFILE_FIELDS -- a field "
               "was added or removed without updating that table (zones_config_accessors.c)");

#define ZCFG_ASSERT_ZC(KIND, NAME, COUNT)                                                 \
    _Static_assert(offsetof(zone_cfg_t, NAME) == offsetof(zone_cfg_shadow_t, NAME),       \
                   "zone_cfg_t::" #NAME                                                   \
                   " offset drifted from ZONE_CFG_FIELDS -- a field was added, removed, " \
                   "reordered, or retyped without updating that table (zones_config_accessors.c)");
ZONE_CFG_FIELDS(ZCFG_ASSERT_ZC)
_Static_assert(sizeof(zone_cfg_t) == sizeof(zone_cfg_shadow_t),
               "zone_cfg_t's total size does not match ZONE_CFG_FIELDS -- a field was added or "
               "removed without updating that table (zones_config_accessors.c)");

#define ZCFG_ASSERT_ZONES(KIND, NAME, COUNT)                                                \
    _Static_assert(offsetof(zones_cfg_t, NAME) == offsetof(zones_cfg_shadow_t, NAME),       \
                   "zones_cfg_t::" #NAME                                                    \
                   " offset drifted from ZONES_CFG_FIELDS -- a field was added, removed, "  \
                   "reordered, or retyped without updating that table (zones_config_accessors.c)");
ZONES_CFG_FIELDS(ZCFG_ASSERT_ZONES)
/* zones_cfg_t's true tail is crc32 (uint32_t), deliberately excluded from
 * the table (see that macro's comment) -- so the whole-struct proof is
 * "shadow struct size + one trailing uint32_t == real struct size", not a
 * bare equality, and additionally pins crc32 itself to be the immediate,
 * only, four-byte tail so nothing else could be hiding after
 * pc_link_abort_silence_ms other than that one named, excluded field. */
_Static_assert(offsetof(zones_cfg_t, crc32) == sizeof(zones_cfg_shadow_t),
               "zones_cfg_t::crc32 is no longer the field immediately after everything "
               "ZONES_CFG_FIELDS enumerates -- a field was added, removed, or reordered "
               "without updating that table (zones_config_accessors.c)");
_Static_assert(offsetof(zones_cfg_t, crc32) + sizeof(((zones_cfg_t *)0)->crc32) == sizeof(zones_cfg_t),
               "zones_cfg_t has a field after crc32 that ZONES_CFG_FIELDS does not know about "
               "(zones_config_accessors.c)");

/* ---- 3. Byte-level primitives -- little-endian, -0.0 flushed to +0.0. ---- */

static void zcfg_put_u8(uint8_t **p, uint8_t v)
{
    *(*p)++ = v;
}

static void zcfg_put_u16(uint8_t **p, uint16_t v)
{
    *(*p)++ = (uint8_t)(v & 0xFFu);
    *(*p)++ = (uint8_t)((v >> 8) & 0xFFu);
}

static void zcfg_put_u32(uint8_t **p, uint32_t v)
{
    *(*p)++ = (uint8_t)(v & 0xFFu);
    *(*p)++ = (uint8_t)((v >> 8) & 0xFFu);
    *(*p)++ = (uint8_t)((v >> 16) & 0xFFu);
    *(*p)++ = (uint8_t)((v >> 24) & 0xFFu);
}

static void zcfg_put_f32(uint8_t **p, float v)
{
    if (v == 0.0f) {
        v = 0.0f; /* flush -0.0f to +0.0f -- plan sec 3.1.3 rule 4; see this
                   * block's top comment for why this is correct and not a
                   * silent numeric change. */
    }
    union {
        float f;
        uint32_t bits;
    } c;
    c.f = v;
    zcfg_put_u32(p, c.bits);
}

static uint8_t zcfg_get_u8(const uint8_t **p)
{
    return *(*p)++;
}

static uint16_t zcfg_get_u16(const uint8_t **p)
{
    uint16_t v = (uint16_t)((*p)[0] | ((*p)[1] << 8));
    *p += 2;
    return v;
}

static uint32_t zcfg_get_u32(const uint8_t **p)
{
    uint32_t v = (uint32_t)(*p)[0] | ((uint32_t)(*p)[1] << 8) | ((uint32_t)(*p)[2] << 16) |
                 ((uint32_t)(*p)[3] << 24);
    *p += 4;
    return v;
}

static float zcfg_get_f32(const uint8_t **p)
{
    union {
        float f;
        uint32_t bits;
    } c;
    c.bits = zcfg_get_u32(p);
    return c.f;
}

/* ---- 4. Encoders/decoders, generated from the SAME field tables that were
 * just proven complete above. ---------------------------------------------- */

static void encode_zone_timing_profile(uint8_t **p, const zone_timing_profile_t *src);
static void encode_zone_cfg(uint8_t **p, const zone_cfg_t *src);
static void decode_zone_timing_profile(const uint8_t **p, zone_timing_profile_t *dst);
static void decode_zone_cfg(const uint8_t **p, zone_cfg_t *dst);

static void zcfg_emit_CHARARR(uint8_t **p, const char *v, size_t n)
{
    memcpy(*p, v, n);
    *p += n;
}
static void zcfg_emit_U8(uint8_t **p, uint8_t v, size_t n)
{
    (void)n;
    zcfg_put_u8(p, v);
}
static void zcfg_emit_U8ARR(uint8_t **p, const uint8_t *v, size_t n)
{
    memcpy(*p, v, n);
    *p += n;
}
static void zcfg_emit_U16(uint8_t **p, uint16_t v, size_t n)
{
    (void)n;
    zcfg_put_u16(p, v);
}
static void zcfg_emit_U32(uint8_t **p, uint32_t v, size_t n)
{
    (void)n;
    zcfg_put_u32(p, v);
}
static void zcfg_emit_F32(uint8_t **p, float v, size_t n)
{
    (void)n;
    zcfg_put_f32(p, v);
}
static void zcfg_emit_F32ARR(uint8_t **p, const float *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        zcfg_put_f32(p, v[i]);
    }
}
static void zcfg_emit_ZONEARR(uint8_t **p, const zone_cfg_t *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        encode_zone_cfg(p, &v[i]);
    }
}
static void zcfg_emit_TPARR(uint8_t **p, const zone_timing_profile_t *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        encode_zone_timing_profile(p, &v[i]);
    }
}

#define ZCFG_EMIT(KIND, NAME, COUNT) zcfg_emit_##KIND(p, src->NAME, (size_t)(COUNT));

static void encode_zone_timing_profile(uint8_t **p, const zone_timing_profile_t *src)
{
    ZONE_TIMING_PROFILE_FIELDS(ZCFG_EMIT)
}

static void encode_zone_cfg(uint8_t **p, const zone_cfg_t *src)
{
    ZONE_CFG_FIELDS(ZCFG_EMIT)
}

static void encode_zones_cfg(uint8_t **p, const zones_cfg_t *src)
{
    ZONES_CFG_FIELDS(ZCFG_EMIT)
    /* crc32 intentionally not emitted -- see ZONES_CFG_FIELDS's comment. */
}

/* Parse needs per-kind addressing: a scalar destination needs `&dst->NAME`
 * (write-through a pointer to one field); an array destination is already a
 * pointer once it decays, so it must be passed as `dst->NAME` -- taking its
 * address would produce a pointer-to-array, not the pointer-to-element every
 * zcfg_get_* (or memcpy) helper expects. Same table, per-kind macro instead of
 * one generic macro, purely for this addressing difference -- the
 * completeness proof above does not depend on this macro at all, so this
 * asymmetry cannot reintroduce the "forgettable field" risk. */
#define ZCFG_PARSE_CHARARR(NAME, COUNT)                        \
    do {                                                       \
        memcpy(dst->NAME, *p, (size_t)(COUNT));                \
        *p += (size_t)(COUNT);                                 \
    } while (0);
#define ZCFG_PARSE_U8(NAME, COUNT) dst->NAME = zcfg_get_u8(p);
#define ZCFG_PARSE_U8ARR(NAME, COUNT)                           \
    do {                                                        \
        memcpy(dst->NAME, *p, (size_t)(COUNT));                 \
        *p += (size_t)(COUNT);                                  \
    } while (0);
#define ZCFG_PARSE_U16(NAME, COUNT) dst->NAME = zcfg_get_u16(p);
#define ZCFG_PARSE_U32(NAME, COUNT) dst->NAME = zcfg_get_u32(p);
#define ZCFG_PARSE_F32(NAME, COUNT) dst->NAME = zcfg_get_f32(p);
#define ZCFG_PARSE_F32ARR(NAME, COUNT)                            \
    for (size_t zcfg_i = 0; zcfg_i < (size_t)(COUNT); zcfg_i++) { \
        dst->NAME[zcfg_i] = zcfg_get_f32(p);                      \
    }
#define ZCFG_PARSE_ZONEARR(NAME, COUNT)                           \
    for (size_t zcfg_i = 0; zcfg_i < (size_t)(COUNT); zcfg_i++) { \
        decode_zone_cfg(p, &dst->NAME[zcfg_i]);                   \
    }
#define ZCFG_PARSE_TPARR(NAME, COUNT)                             \
    for (size_t zcfg_i = 0; zcfg_i < (size_t)(COUNT); zcfg_i++) { \
        decode_zone_timing_profile(p, &dst->NAME[zcfg_i]);        \
    }
#define ZCFG_PARSE(KIND, NAME, COUNT) ZCFG_PARSE_##KIND(NAME, COUNT)

static void decode_zone_timing_profile(const uint8_t **p, zone_timing_profile_t *dst)
{
    ZONE_TIMING_PROFILE_FIELDS(ZCFG_PARSE)
}

static void decode_zone_cfg(const uint8_t **p, zone_cfg_t *dst)
{
    ZONE_CFG_FIELDS(ZCFG_PARSE)
}

static void decode_zones_cfg(const uint8_t **p, zones_cfg_t *dst)
{
    ZONES_CFG_FIELDS(ZCFG_PARSE)
    /* crc32 intentionally not parsed -- left at 0 from the caller's
     * memset(0); see zones_config_import_canonical()'s own comment. */
}

/* ---- 5. Public entry points ------------------------------------------------ */

size_t zones_config_canonical_max_size(void)
{
    /* See this pair's header comment: the canonical form has no padding and
     * excludes crc32, so it is always strictly smaller than
     * sizeof(zones_cfg_t), which is itself already bounded by
     * ZONES_CONFIG_BLOB_MAX_SIZE (asserted just above
     * zones_config_export_blob()). Reusing that ceiling means no second cap
     * to keep in sync as the struct grows. */
    return ZONES_CONFIG_BLOB_MAX_SIZE;
}

bool zones_config_export_canonical(const void *cfg, uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!cfg || !out || !out_len || out_cap < zones_config_canonical_max_size()) {
        return false;
    }
    uint8_t *p = out;
    encode_zones_cfg(&p, (const zones_cfg_t *)cfg);
    *out_len = (size_t)(p - out);
    return true;
}

bool zones_config_import_canonical(const uint8_t *buf, size_t len, void *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(zones_cfg_t)); /* H1's zero-fill-on-reconstruction requirement --
                                           * see the header comment; also makes a refusal
                                           * below leave `out` in a defined, all-zero state
                                           * rather than whatever it held on entry. */
    if (!buf) {
        return false;
    }
    /* Compute this build's own encoder length for a fully-populated struct
     * and refuse anything else outright -- same "no partial write" rule
     * kiln_package_capture_pico_half() uses. A throwaway scratch encode of
     * an all-zero zones_cfg_t is cheap (this runs once per import, never a
     * hot path) and avoids maintaining a second, hand-computed length
     * constant that could itself drift from the field tables above. */
    static uint8_t s_scratch[ZONES_CONFIG_BLOB_MAX_SIZE];
    zones_cfg_t zero_cfg;
    memset(&zero_cfg, 0, sizeof(zero_cfg));
    uint8_t *scratch_p = s_scratch;
    encode_zones_cfg(&scratch_p, &zero_cfg);
    size_t expected_len = (size_t)(scratch_p - s_scratch);
    if (len != expected_len) {
        return false;
    }
    const uint8_t *p = buf;
    decode_zones_cfg(&p, (zones_cfg_t *)out);
    return true;
}
