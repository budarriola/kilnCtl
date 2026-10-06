// zone_aux_convert_http -- thin httpd/store adapter for zone_aux_convert_core.c. Installs the
// `move_zone_to_aux` hook on POST /api/zones (zones_http_post.c); adds no URI route.
// Decisions live in the core; see zone_aux_convert_core.h.
#include "zone_aux_convert_http.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "aux_outputs_cfg.h"
#include "backup_restore_state.h"
#include "live_profile.h"
#include "persist_scratch.h"
#include "profile_rule_target.h"
#include "profiles_store.h"
#include "relay_authority.h"
#include "system_mode_gate.h"
#include "zone_aux_convert_core.h"
#include "zones_config_accessors.h"
#include "zones_config_query.h"
#include "zones_http_internal.h"

static const char *TAG = "zone_aux_convert";

static bool op_mode_blocked(char *reason, size_t cap)
{
    sys_mode_snapshot_t snap = { 0 };
    relay_authority_heat_run_active(&snap.profile_running, &snap.autotune_running);
    return system_mode_gate_check(SYS_ACTION_WRITE_ZONES_CONFIG, &snap, reason, cap);
}

static bool op_zone_get(uint8_t zone, zone_aux_zone_info_t *out)
{
    memset(out, 0, sizeof(*out));
    zone_type_t zt = ZONE_TYPE_HEATER;
    if (!zones_config_get_zone_type(zone, &zt)) {
        return true; /* exists stays false */
    }
    out->exists = true;
    out->is_on_off = zt == ZONE_TYPE_ON_OFF;
    bool fs = false;
    (void)zones_config_get_failsafe_state(zone, &fs);
    out->failsafe_on = fs;
    (void)zones_config_get_relay_mask(zone, &out->relay_mask);
    (void)zones_config_get_thermo_mask(zone, &out->thermo_mask);
    (void)zones_config_get_hyst_c(zone, &out->hyst_c);
    (void)zones_config_get_min_on_s(zone, &out->min_on_s);
    (void)zones_config_get_min_off_s(zone, &out->min_off_s);
    return true;
}

static uint8_t op_zones_union(void)
{
    uint8_t u = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        uint8_t m = 0;
        if (zones_config_get_relay_mask(zi, &m)) {
            u |= m;
        }
    }
    return u;
}

static bool op_live_uses_zone(uint8_t zone)
{
    profile_t *p = persist_scratch_alloc(sizeof(*p));
    if (p == NULL) {
        return true; /* cannot tell: refuse */
    }
    bool uses = false;
    if (live_profile_load_working(p)) {
        uint8_t n = p->on_off_rule_count > PROFILE_MAX_ON_OFF_RULES ? (uint8_t)PROFILE_MAX_ON_OFF_RULES
                                                                    : p->on_off_rule_count;
        for (uint8_t i = 0; i < n; i++) {
            if (p->on_off_rules[i].zone_index == zone) {
                uses = true;
            }
        }
    }
    free(p);
    return uses;
}

static zone_aux_free_result_t op_zone_free(uint8_t zone)
{
    switch (zones_http_zone_free_for_aux(zone)) {
    case ZONES_AUX_FREE_OK: return ZONE_AUX_FREE_OK;
    case ZONES_AUX_FREE_NOTHING_CHANGED: return ZONE_AUX_FREE_NOTHING_CHANGED;
    default: return ZONE_AUX_FREE_UNCERTAIN;
    }
}
static bool op_zone_restore(uint8_t zone)
{
    return zones_http_zone_restore_after_aux(zone);
}
static void op_zone_done(void)
{
    zones_http_zone_discard_saved_for_aux();
}

static void op_busy(bool on)
{
    backup_import_config_change_set(on);
}

static bool op_verify_persisted(void)
{
    return zones_config_nvs_equals_ram() && aux_outputs_cfg_verify_persisted();
}

static void *op_scratch_alloc(size_t n)
{
    return persist_scratch_alloc(n);
}

static const zone_aux_ops_t s_ops = {
    .mode_blocked = op_mode_blocked,
    .zone_get = op_zone_get,
    .zone_free = op_zone_free,
    .zone_restore = op_zone_restore,
    .zone_done = op_zone_done,
    .zones_union = op_zones_union,
    .aux_get = aux_outputs_cfg_get,
    .aux_get_raw = aux_outputs_cfg_get_raw,
    .aux_set = aux_outputs_cfg_set,
    .aux_quarantined = aux_outputs_cfg_quarantined,
    .live_uses_zone = op_live_uses_zone,
    .profiles_plan = profiles_retarget_zone_to_aux_plan,
    .profiles_commit = profiles_retarget_zone_to_aux_commit,
    .profiles_revert = profiles_retarget_zone_to_aux_revert,
    .profiles_resume = profiles_retarget_zone_to_aux_resume,
    .journal_read = aux_convert_journal_read,
    .journal_write = aux_convert_journal_write,
    .journal_clear = aux_convert_journal_clear,
    .verify_persisted = op_verify_persisted,
    .busy = op_busy,
    .scratch_alloc = op_scratch_alloc,
};

/* The reply block is heap, not stack: this handler runs inside zones_post_handler's call chain on the
 * httpd task, whose stack budget (check_httpd_task_stack_budget.py) has no room for another ~200 B
 * buffer on top of the core's own frames. */
static esp_err_t move_handler(httpd_req_t *req, const char *body)
{
    zone_aux_reply_t *reply = persist_scratch_alloc(sizeof(*reply));
    if (reply == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "out of memory -- nothing changed");
    }
    memset(reply, 0, sizeof(*reply));
    zone_aux_convert_run(&s_ops, body, reply);
    esp_err_t ret;
    if (reply->status == 200) {
        ESP_LOGI(TAG, "zone moved to aux: %s", reply->msg);
        httpd_resp_set_type(req, "application/json");
        ret = httpd_resp_sendstr(req, reply->msg);
    } else {
        ESP_LOGW(TAG, "move_zone_to_aux refused (%d): %s", reply->status, reply->msg);
        const char *status = "500 Internal Server Error";
        switch (reply->status) {
        case 400: status = "400 Bad Request"; break;
        case 409: status = "409 Conflict"; break;
        default: break;
        }
        httpd_resp_set_status(req, status);
        ret = httpd_resp_sendstr(req, reply->msg);
    }
    free(reply);
    return ret;
}

void zone_aux_convert_http_start(void)
{
    zones_http_set_move_to_aux_handler(zone_aux_convert_requested, move_handler);
}
