// Host-test coverage (c78b): the move_zone_to_aux adapter (POST /api/zones, docs/SPARE_RELAY_ONOFF_PLAN.md
// section 10) -- the REAL zone_aux_convert_http.c with the REAL zone_aux_convert_core.c and the REAL
// system_mode_gate.c. test_zone_aux_convert_core.c drives the core with injected ops; this one drives the
// production ops wiring: mode gate on the heat-run flags, the zone/aux/profile/journal adapters, the busy
// flags, and the status mapping (400/409/500) of move_handler. Zones, aux store, profile store and journal
// are small stateful fakes, so every refusal is asserted against the stored state, not the reply text.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "../drivers/http/zone_aux_convert_core.c"
#include "../drivers/http/zone_aux_convert_http.c"

#define NZ 3

/* ---- zones fake ---- */
static bool f_z_on_off[NZ], f_z_failsafe[NZ];
static uint8_t f_z_relay[NZ], f_z_thermo[NZ];
static float f_z_hyst[NZ];
static uint16_t f_z_min_on[NZ], f_z_min_off[NZ];
static int f_free_result; /* ZONES_AUX_FREE_* */
static int f_free_calls, f_restore_calls, f_done_calls;
static bool f_restore_ok;
static bool f_persisted_equal;
static struct { bool on_off; uint8_t relay; float hyst; uint16_t on, off; } f_saved;

bool zones_config_get_zone_type(uint8_t z, zone_type_t *out)
{
    if (z >= NZ) return false;
    *out = f_z_on_off[z] ? ZONE_TYPE_ON_OFF : ZONE_TYPE_HEATER;
    return true;
}
bool zones_config_get_failsafe_state(uint8_t z, bool *out) { if (z >= NZ) return false; *out = f_z_failsafe[z]; return true; }
bool zones_config_get_relay_mask(uint8_t z, uint8_t *out) { if (z >= NZ) return false; *out = f_z_relay[z]; return true; }
bool zones_config_get_thermo_mask(uint8_t z, uint8_t *out) { if (z >= NZ) return false; *out = f_z_thermo[z]; return true; }
bool zones_config_get_hyst_c(uint8_t z, float *out) { if (z >= NZ) return false; *out = f_z_hyst[z]; return true; }
bool zones_config_get_min_on_s(uint8_t z, uint16_t *out) { if (z >= NZ) return false; *out = f_z_min_on[z]; return true; }
bool zones_config_get_min_off_s(uint8_t z, uint16_t *out) { if (z >= NZ) return false; *out = f_z_min_off[z]; return true; }
bool zones_config_persisted_equals_ram(void) { return f_persisted_equal; }

int zones_http_zone_free_for_aux(uint8_t z)
{
    f_free_calls++;
    if (f_free_result == ZONES_AUX_FREE_OK) {
        f_saved.on_off = f_z_on_off[z]; f_saved.relay = f_z_relay[z];
        f_saved.hyst = f_z_hyst[z]; f_saved.on = f_z_min_on[z]; f_saved.off = f_z_min_off[z];
        f_z_on_off[z] = false; f_z_relay[z] = 0; f_z_failsafe[z] = false;
        f_z_hyst[z] = 0; f_z_min_on[z] = 0; f_z_min_off[z] = 0;
    }
    return f_free_result;
}
bool zones_http_zone_restore_after_aux(uint8_t z)
{
    f_restore_calls++;
    f_z_on_off[z] = f_saved.on_off; f_z_relay[z] = f_saved.relay;
    f_z_hyst[z] = f_saved.hyst; f_z_min_on[z] = f_saved.on; f_z_min_off[z] = f_saved.off;
    return f_restore_ok;
}
void zones_http_zone_discard_saved_for_aux(void) { f_done_calls++; }

static zones_move_to_aux_is_request_t f_hook_is_req;
static zones_move_to_aux_handler_t f_hook_handler;
void zones_http_set_move_to_aux_handler(zones_move_to_aux_is_request_t a, zones_move_to_aux_handler_t h)
{
    f_hook_is_req = a;
    f_hook_handler = h;
}

/* ---- aux store fake ---- */
static aux_output_entry_t f_aux[AUX_OUTPUTS_COUNT];
static bool f_aux_quarantined, f_aux_set_fail, f_aux_verify_ok;
bool aux_outputs_cfg_get(uint8_t r, aux_output_t *out)
{
    if (r < 1 || r > AUX_OUTPUTS_COUNT) return false;
    const aux_output_entry_t *e = &f_aux[r - 1];
    memset(out, 0, sizeof(*out));
    out->enabled = e->enabled != 0;
    out->tc_zone = e->tc_zone_plus1 ? (uint8_t)(e->tc_zone_plus1 - 1u) : AUX_TC_ZONE_NONE;
    out->hyst_c = e->hyst_c; out->min_on_s = e->min_on_s; out->min_off_s = e->min_off_s;
    return true;
}
bool aux_outputs_cfg_get_raw(uint8_t r, aux_output_entry_t *out)
{
    if (r < 1 || r > AUX_OUTPUTS_COUNT) return false;
    *out = f_aux[r - 1];
    return true;
}
esp_err_t aux_outputs_cfg_set(uint8_t r, const aux_output_entry_t *e, uint8_t zu)
{
    (void)zu;
    if (f_aux_set_fail) return ESP_FAIL;
    f_aux[r - 1] = *e;
    return ESP_OK;
}
bool aux_outputs_cfg_quarantined(void) { return f_aux_quarantined; }
bool aux_outputs_cfg_verify_persisted(void) { return f_aux_verify_ok; }

static aux_convert_journal_t f_journal;
static bool f_journal_present, f_journal_write_ok;
bool aux_convert_journal_read(aux_convert_journal_t *o) { if (!f_journal_present) return false; *o = f_journal; return true; }
bool aux_convert_journal_write(const aux_convert_journal_t *j)
{
    if (!f_journal_write_ok) return false;
    f_journal = *j; f_journal_present = true;
    return true;
}
bool aux_convert_journal_clear(void) { f_journal_present = false; return true; }

/* ---- profile store fake: 3 stored rules; targets use the real encoding helper ---- */
#define NRULES 3
static uint8_t f_rule_target[NRULES];
static bool f_profiles_commit_fail, f_profiles_plan_fail, f_live_uses;
static int f_revert_calls, f_commit_relay_seen;
bool live_profile_load_working(profile_t *p)
{
    memset(p, 0, sizeof(*p));
    if (f_live_uses) {
        p->on_off_rule_count = 1;
        p->on_off_rules[0].zone_index = 1;
    }
    return true;
}
bool profiles_retarget_zone_to_aux_plan(uint8_t z, uint8_t r, bool tc, profiles_retarget_counts_t *c, char *e, size_t cap)
{
    (void)z; (void)r; (void)tc; (void)c;
    if (f_profiles_plan_fail) { snprintf(e, cap, "profile cannot be moved"); return false; }
    return true;
}
bool profiles_retarget_zone_to_aux_commit(uint8_t z, uint8_t r, bool tc, profiles_retarget_counts_t *c, char *e, size_t cap)
{
    (void)tc;
    f_commit_relay_seen = r;
    if (f_profiles_commit_fail) { snprintf(e, cap, "slot 2 write failed"); return false; }
    for (int i = 0; i < NRULES; i++) {
        if (f_rule_target[i] == z) {
            f_rule_target[i] = profile_rule_target_from_aux_relay(r);
            c->rules_retargeted++;
        }
    }
    c->profiles_scanned = 3; c->profiles_affected = 1;
    return true;
}
bool profiles_retarget_zone_to_aux_revert(uint8_t z, uint8_t r)
{
    f_revert_calls++;
    for (int i = 0; i < NRULES; i++) {
        if (f_rule_target[i] == profile_rule_target_from_aux_relay(r)) f_rule_target[i] = z;
    }
    return true;
}
bool profiles_retarget_zone_to_aux_resume(uint8_t z, uint8_t r, bool tc, profiles_retarget_counts_t *c, char *e, size_t cap)
{
    return profiles_retarget_zone_to_aux_commit(z, r, tc, c, e, cap);
}

/* ---- flags ---- */
static bool f_profile_running, f_autotune_running;
void relay_authority_heat_run_active(bool *p, bool *a) { *p = f_profile_running; *a = f_autotune_running; }
static bool f_import_busy, f_convert_busy;
static int f_busy_raise, f_busy_lower;
void backup_import_config_change_set(bool on) { f_import_busy = on; }
void profiles_http_set_convert_busy(bool on)
{
    f_convert_busy = on;
    if (on) f_busy_raise++; else f_busy_lower++;
}

/* ---- transport ---- */
static int s_status;
static char s_type[32], s_reply[512];
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { (void)r; s_status = atoi(s); return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t) { (void)r; snprintf(s_type, sizeof(s_type), "%s", t); return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s) { (void)r; snprintf(s_reply, sizeof(s_reply), "%s", s); return ESP_OK; }

static void reset(void)
{
    memset(f_z_on_off, 0, sizeof(f_z_on_off)); memset(f_z_failsafe, 0, sizeof(f_z_failsafe));
    memset(f_z_relay, 0, sizeof(f_z_relay)); memset(f_z_thermo, 0, sizeof(f_z_thermo));
    memset(f_z_hyst, 0, sizeof(f_z_hyst)); memset(f_z_min_on, 0, sizeof(f_z_min_on)); memset(f_z_min_off, 0, sizeof(f_z_min_off));
    /* zone 1: ON_OFF on relay 3, with a thermocouple */
    f_z_on_off[1] = true; f_z_relay[1] = 0x04; f_z_thermo[1] = 0x02;
    f_z_hyst[1] = 3.0f; f_z_min_on[1] = 60; f_z_min_off[1] = 90;
    f_free_result = ZONES_AUX_FREE_OK; f_free_calls = f_restore_calls = f_done_calls = 0;
    f_restore_ok = true; f_persisted_equal = true;
    memset(f_aux, 0, sizeof(f_aux));
    f_aux_quarantined = f_aux_set_fail = false; f_aux_verify_ok = true;
    memset(&f_journal, 0, sizeof(f_journal)); f_journal_present = false; f_journal_write_ok = true;
    f_rule_target[0] = 1; f_rule_target[1] = 0; f_rule_target[2] = 1;
    f_profiles_commit_fail = f_profiles_plan_fail = f_live_uses = false;
    f_revert_calls = 0; f_commit_relay_seen = 0;
    f_profile_running = f_autotune_running = false;
    f_import_busy = f_convert_busy = false; f_busy_raise = f_busy_lower = 0;
    s_status = 200; s_type[0] = 0; s_reply[0] = 0;
}

static int run(const char *body)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    s_status = 200; s_type[0] = 0; s_reply[0] = 0;
    (void)f_hook_handler(&req, body);
    return s_status;
}

#define UNCHANGED()                                                                                           \
    (f_z_on_off[1] && f_z_relay[1] == 0x04 && !f_aux[2].enabled && f_rule_target[0] == 1 &&                   \
     f_rule_target[2] == 1 && f_free_calls == 0 && !f_journal_present)

static void test_wiring(void)
{
    TEST_SECTION("hook install and detection");
    reset();
    f_hook_is_req = NULL; f_hook_handler = NULL;
    zone_aux_convert_http_start();
    TEST_CHECK(f_hook_handler != NULL && f_hook_is_req != NULL, "start installs both halves of the hook");
    TEST_CHECK(f_hook_is_req("move_zone_to_aux=1&confirm=1") && !f_hook_is_req("zone=1"), "detector keys on move_zone_to_aux");
}

static void test_refusals(void)
{
    TEST_SECTION("refusals change nothing");
    reset();
    zone_aux_convert_http_start();
    TEST_CHECK(run("move_zone_to_aux=1") == 400 && UNCHANGED(), "confirm missing -> 400, nothing changed");
    TEST_CHECK(run("move_zone_to_aux=1&confirm=0") == 400 && UNCHANGED(), "confirm=0 -> 400");
    TEST_CHECK(run("move_zone_to_aux=1&confirm=true") == 400 && UNCHANGED(), "confirm=true (not 1) -> 400");
    TEST_CHECK(run("move_zone_to_aux=7&confirm=1") == 400 && UNCHANGED(), "zone out of range -> 400");
    TEST_CHECK(strstr(s_reply, "out of range") != NULL, "400 body names the field");
    f_profile_running = true;
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1") == 409 && UNCHANGED(), "profile running -> 409 mode gate");
    TEST_CHECK(strstr(s_reply, "firing or autotune run is active") != NULL, "409 carries the gate text");
    f_profile_running = false; f_autotune_running = true;
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1") == 409 && UNCHANGED(), "autotune running -> 409 mode gate");
    f_autotune_running = false;
    f_z_on_off[1] = false;
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1") == 409 && f_free_calls == 0, "not an on/off zone -> 409");
    f_z_on_off[1] = true;
    f_aux_quarantined = true;
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1") == 409 && UNCHANGED(), "quarantined aux store -> 409");
    f_aux_quarantined = false;
    f_live_uses = true;
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1") == 409 && UNCHANGED(), "live working profile uses the zone -> 409");
    f_live_uses = false;
    f_profiles_plan_fail = true;
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1") == 409 && UNCHANGED(), "profile plan refusal -> 409");
    TEST_CHECK(strstr(s_reply, "profile cannot be moved") != NULL, "plan reason relayed");
    f_profiles_plan_fail = false;
    TEST_CHECK(f_busy_raise == f_busy_lower && f_busy_raise > 0 && !f_convert_busy && !f_import_busy,
               "busy flags raised and lowered on every refusal path");
}

static void test_success(void)
{
    TEST_SECTION("success rewrites rules to aux target 8+(relay-1)");
    reset();
    zone_aux_convert_http_start();
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1") == 200, "200");
    TEST_CHECK(strstr(s_type, "json") != NULL && strstr(s_reply, "\"ok\":true") != NULL &&
                   strstr(s_reply, "\"zone\":1") != NULL && strstr(s_reply, "\"relay\":3") != NULL &&
                   strstr(s_reply, "\"rules_retargeted\":2") != NULL,
               "JSON ack names zone, relay, rules retargeted");
    TEST_CHECK(f_commit_relay_seen == 3, "profile commit got the zone's relay (3)");
    TEST_CHECK(f_rule_target[0] == 10 && f_rule_target[2] == 10 && f_rule_target[1] == 0,
               "zone-1 rules now target 8+(3-1)=10; other zone's rule untouched");
    TEST_CHECK(!f_z_on_off[1] && f_z_relay[1] == 0, "zone freed: heater, relay_mask 0");
    TEST_CHECK(f_aux[2].enabled == 1 && f_aux[2].tc_zone_plus1 == 2 && f_aux[2].hyst_c == 3.0f &&
                   f_aux[2].min_on_s == 60 && f_aux[2].min_off_s == 90,
               "aux 3 enabled with tc_zone 1 and the zone's hyst/min times");
    TEST_CHECK(f_done_calls == 1 && !f_journal_present, "saved zone released, marker cleared");
    TEST_CHECK(f_busy_raise == 1 && f_busy_lower == 1 && !f_convert_busy && !f_import_busy, "busy flags balanced");
    reset();
    zone_aux_convert_http_start();
    f_z_relay[1] = 0x01; f_z_thermo[1] = 0;
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1") == 200 && f_rule_target[0] == 8 && f_aux[0].tc_zone_plus1 == 0,
               "relay 1 -> target 8; no thermocouple -> no tc_zone");
}

static void test_rollback(void)
{
    TEST_SECTION("all-or-nothing rollback");
    reset();
    zone_aux_convert_http_start();
    f_profiles_commit_fail = true;
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1") == 500, "profile rewrite failure -> 500");
    TEST_CHECK(strstr(s_reply, "slot 2 write failed") != NULL && strstr(s_reply, "everything restored") != NULL,
               "reply names the failure and a clean rollback");
    TEST_CHECK(f_z_on_off[1] && f_z_relay[1] == 0x04 && f_z_hyst[1] == 3.0f && f_z_min_on[1] == 60,
               "zone restored exactly");
    TEST_CHECK(!f_aux[2].enabled && f_aux[2].min_on_s == 0, "aux entry back to its stored original");
    TEST_CHECK(f_rule_target[0] == 1 && f_rule_target[2] == 1, "no rule left at an aux target");
    TEST_CHECK(!f_journal_present && f_done_calls == 0 && f_restore_calls == 1, "marker cleared after a clean undo");
    TEST_CHECK(!f_convert_busy && !f_import_busy && f_busy_raise == f_busy_lower, "busy flags lowered");
    reset();
    zone_aux_convert_http_start();
    f_profiles_commit_fail = true; f_restore_ok = false;
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1") == 500 && strstr(s_reply, "ROLLBACK INCOMPLETE") != NULL &&
                   f_journal_present,
               "failed zone restore: reported incomplete and the marker is KEPT");
    reset();
    zone_aux_convert_http_start();
    f_aux_set_fail = true;
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1") == 500 && f_z_on_off[1] && f_z_relay[1] == 0x04 && f_revert_calls == 0,
               "aux enable failure: zone restored, profiles never touched");
    reset();
    zone_aux_convert_http_start();
    f_free_result = ZONES_AUX_FREE_NOTHING_CHANGED;
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1") == 500 && f_restore_calls == 0 && f_rule_target[0] == 1 && !f_journal_present,
               "zone_free NOTHING_CHANGED maps to 500, nothing to undo, marker cleared");
    reset();
    zone_aux_convert_http_start();
    f_free_result = ZONES_AUX_FREE_UNCERTAIN;
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1") == 500 && strstr(s_reply, "ROLLBACK INCOMPLETE") != NULL && f_journal_present,
               "zone_free UNCERTAIN maps to incomplete, marker kept");
    reset();
    zone_aux_convert_http_start();
    f_aux_verify_ok = false;
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1") == 500 && f_z_on_off[1] && f_rule_target[0] == 1 && f_revert_calls == 1 &&
                   !f_aux[2].enabled,
               "persisted read-back mismatch undoes all three steps");
    reset();
    zone_aux_convert_http_start();
    f_persisted_equal = false;
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1") == 500 && f_z_on_off[1] && f_rule_target[0] == 1,
               "zones blob not equal to RAM also fails the read-back");
}

int main(void)
{
    test_wiring();
    test_refusals();
    test_success();
    test_rollback();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
