// Host tests for App/drivers/http/zone_aux_convert_core.c -- the one-shot "move an ON_OFF zone to
// an aux output" action (docs/SPARE_RELAY_ONOFF_PLAN.md section 10). #includes the core directly and
// drives it with a fake world: one zone, one aux entry, a stored-profile stub. No httpd, no flash.
//
// LOAD-BEARING PROPERTIES:
//   1. Every refusal (mode gate, not ON_OFF, failsafe ON, bad relay_mask, aux already set, quarantine,
//      live profile, plan refusal, missing confirm) leaves the world untouched: no free, no aux write.
//   2. A failure at ANY execute stage (aux enable, profile commit, read-back) undoes every completed
//      stage in reverse and reports it; a failed undo is reported as ROLLBACK INCOMPLETE.
//   3. The aux entry copies the zone's hysteresis / min on / min off and takes tc_zone = Z only when
//      the zone has a thermocouple.
#include <stdio.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/http/zone_aux_convert_core.c"

/* ---- fake world ---- */
static bool w_blocked;
static zone_aux_zone_info_t w_zone;     /* zone 1 */
static aux_output_t w_aux[AUX_OUTPUTS_COUNT + 1];
static bool w_quar;
static bool w_live_uses;
static bool w_plan_ok;
static bool w_commit_ok;
static bool w_revert_ok;
static bool w_free_ok;
static bool w_restore_ok;
static esp_err_t w_aux_set_ret;
static int w_aux_set_fail_on_call; /* 1-based; 0 = never */
static bool w_corrupt_after_commit; /* make the read-back disagree */

static int c_free, c_restore, c_done, c_set, c_plan, c_commit, c_revert;
static uint8_t c_commit_relay;
static bool c_commit_has_tc;
static aux_output_entry_t c_first_set;
static uint8_t c_order[16];
static int c_order_n;
static zone_aux_zone_info_t w_zone_saved;

static void note(uint8_t tag)
{
    if (c_order_n < (int)sizeof(c_order)) {
        c_order[c_order_n++] = tag;
    }
}
enum { T_FREE = 1, T_SET, T_COMMIT, T_REVERT, T_RESTORE, T_DONE };

static void reset_world(void)
{
    w_blocked = false;
    memset(&w_zone, 0, sizeof(w_zone));
    w_zone.exists = true;
    w_zone.is_on_off = true;
    w_zone.relay_mask = 0x04; /* relay 3 */
    w_zone.thermo_mask = 0x02;
    w_zone.hyst_c = 3.0f;
    w_zone.min_on_s = 20;
    w_zone.min_off_s = 30;
    memset(w_aux, 0, sizeof(w_aux));
    for (int i = 1; i <= (int)AUX_OUTPUTS_COUNT; i++) {
        w_aux[i].tc_zone = AUX_TC_ZONE_NONE;
        w_aux[i].hyst_c = AUX_HYST_C_DEFAULT;
        w_aux[i].min_on_s = AUX_MIN_ON_OFF_S_DEFAULT;
        w_aux[i].min_off_s = AUX_MIN_ON_OFF_S_DEFAULT;
    }
    w_quar = false;
    w_live_uses = false;
    w_plan_ok = true;
    w_commit_ok = true;
    w_revert_ok = true;
    w_free_ok = true;
    w_restore_ok = true;
    w_aux_set_ret = ESP_OK;
    w_aux_set_fail_on_call = 0;
    w_corrupt_after_commit = false;
    c_free = c_restore = c_done = c_set = c_plan = c_commit = c_revert = 0;
    c_order_n = 0;
    memset(&c_first_set, 0, sizeof(c_first_set));
}

static bool fk_blocked(char *reason, size_t cap)
{
    if (w_blocked) {
        snprintf(reason, cap, "firing active");
    }
    return w_blocked;
}
static bool fk_zone_get(uint8_t zone, zone_aux_zone_info_t *out)
{
    if (zone != 1) {
        memset(out, 0, sizeof(*out));
        return true;
    }
    *out = w_zone;
    return true;
}
static bool fk_zone_free(uint8_t zone)
{
    (void)zone;
    c_free++;
    note(T_FREE);
    if (!w_free_ok) {
        return false;
    }
    w_zone_saved = w_zone;
    w_zone.is_on_off = false;
    w_zone.relay_mask = 0;
    w_zone.failsafe_on = false;
    return true;
}
static bool fk_zone_restore(uint8_t zone)
{
    (void)zone;
    c_restore++;
    note(T_RESTORE);
    if (!w_restore_ok) {
        return false;
    }
    w_zone = w_zone_saved;
    return true;
}
static void fk_zone_done(void)
{
    c_done++;
    note(T_DONE);
}
static uint8_t fk_union(void) { return w_zone.relay_mask; }
static bool fk_aux_get(uint8_t relay, aux_output_t *out)
{
    if (relay < 1 || relay > AUX_OUTPUTS_COUNT) {
        return false;
    }
    *out = w_aux[relay];
    return true;
}
static esp_err_t fk_aux_set(uint8_t relay, const aux_output_entry_t *e, uint8_t u)
{
    (void)u;
    c_set++;
    note(T_SET);
    if (c_set == 1) {
        c_first_set = *e;
    }
    if (w_aux_set_fail_on_call == c_set) {
        return ESP_FAIL;
    }
    if (w_aux_set_ret != ESP_OK) {
        return w_aux_set_ret;
    }
    w_aux[relay].enabled = e->enabled;
    w_aux[relay].tc_zone = e->tc_zone_plus1 == 0 ? AUX_TC_ZONE_NONE : (uint8_t)(e->tc_zone_plus1 - 1u);
    w_aux[relay].hyst_c = e->hyst_c;
    w_aux[relay].min_on_s = e->min_on_s;
    w_aux[relay].min_off_s = e->min_off_s;
    return ESP_OK;
}
static bool fk_quar(void) { return w_quar; }
static bool fk_live(uint8_t zone)
{
    (void)zone;
    return w_live_uses;
}
static bool fk_plan(uint8_t zone, uint8_t relay, bool has_tc, profiles_retarget_counts_t *c, char *err, size_t cap)
{
    (void)zone;
    (void)relay;
    (void)has_tc;
    c_plan++;
    memset(c, 0, sizeof(*c));
    c->profiles_scanned = 4;
    c->profiles_affected = 2;
    c->rules_retargeted = 5;
    if (!w_plan_ok) {
        snprintf(err, cap, "slot 2 has temp_source 2");
    }
    return w_plan_ok;
}
static bool fk_commit(uint8_t zone, uint8_t relay, bool has_tc, profiles_retarget_counts_t *c, char *err, size_t cap)
{
    (void)zone;
    c_commit++;
    c_commit_relay = relay;
    c_commit_has_tc = has_tc;
    note(T_COMMIT);
    memset(c, 0, sizeof(*c));
    c->profiles_scanned = 4;
    c->profiles_affected = 2;
    c->rules_retargeted = 5;
    if (!w_commit_ok) {
        snprintf(err, cap, "slot 1 write failed");
        return false;
    }
    if (w_corrupt_after_commit) {
        w_aux[relay].enabled = 0; /* read-back must catch this */
    }
    return true;
}
static bool fk_revert(uint8_t zone, uint8_t relay)
{
    (void)zone;
    (void)relay;
    c_revert++;
    note(T_REVERT);
    return w_revert_ok;
}

static const zone_aux_ops_t OPS = {
    .mode_blocked = fk_blocked,
    .zone_get = fk_zone_get,
    .zone_free = fk_zone_free,
    .zone_restore = fk_zone_restore,
    .zone_done = fk_zone_done,
    .zones_union = fk_union,
    .aux_get = fk_aux_get,
    .aux_set = fk_aux_set,
    .aux_quarantined = fk_quar,
    .live_uses_zone = fk_live,
    .profiles_plan = fk_plan,
    .profiles_commit = fk_commit,
    .profiles_revert = fk_revert,
};

static int run(const char *body, zone_aux_reply_t *r)
{
    memset(r, 0, sizeof(*r));
    zone_aux_convert_run(&OPS, body, r);
    return r->status;
}

static bool untouched(void)
{
    return c_free == 0 && c_set == 0 && c_commit == 0 && c_revert == 0 && c_restore == 0 && c_done == 0;
}

static void test_requested(void)
{
    TEST_SECTION("move_zone_to_aux detection");
    TEST_CHECK(zone_aux_convert_requested("move_zone_to_aux=1&confirm=1"), "field present");
    TEST_CHECK(!zone_aux_convert_requested("z1_name=a"), "ordinary zones body is not a request");
    TEST_CHECK(!zone_aux_convert_requested(NULL), "NULL body is not a request");
}

static void test_bad_input(void)
{
    TEST_SECTION("move_zone_to_aux input validation");
    zone_aux_reply_t r;
    reset_world();
    TEST_CHECK(run("move_zone_to_aux=1", &r) == 400 && untouched(), "missing confirm -> 400");
    TEST_CHECK(run("move_zone_to_aux=1&confirm=0", &r) == 400 && untouched(), "confirm=0 -> 400");
    TEST_CHECK(run("move_zone_to_aux=1&confirm=2", &r) == 400 && untouched(), "confirm=2 -> 400");
    TEST_CHECK(run("move_zone_to_aux=3&confirm=1", &r) == 400 && untouched(), "zone out of range -> 400");
    TEST_CHECK(run("move_zone_to_aux=x&confirm=1", &r) == 400 && untouched(), "zone not numeric -> 400");
    TEST_CHECK(run("move_zone_to_aux=0&confirm=1", &r) == 400 && untouched(), "unconfigured zone -> 400");
}

static void test_refusals(void)
{
    TEST_SECTION("move_zone_to_aux refusals change nothing");
    zone_aux_reply_t r;
    const char *body = "move_zone_to_aux=1&confirm=1";

    reset_world();
    w_blocked = true;
    TEST_CHECK(run(body, &r) == 409 && untouched() && c_plan == 0, "mode gate -> 409 before any read");

    reset_world();
    w_zone.is_on_off = false;
    TEST_CHECK(run(body, &r) == 409 && untouched(), "not ON_OFF -> 409");

    reset_world();
    w_zone.failsafe_on = true;
    TEST_CHECK(run(body, &r) == 409 && untouched(), "failsafe ON -> 409");

    reset_world();
    w_zone.relay_mask = 0;
    TEST_CHECK(run(body, &r) == 409 && untouched(), "no relay -> 409");

    reset_world();
    w_zone.relay_mask = 0x06;
    TEST_CHECK(run(body, &r) == 409 && untouched(), "two relays -> 409");

    reset_world();
    w_zone.relay_mask = 0x20; /* relay 6: beyond the 4 aux relays */
    TEST_CHECK(run(body, &r) == 409 && untouched(), "relay beyond aux range -> 409");

    reset_world();
    w_quar = true;
    TEST_CHECK(run(body, &r) == 409 && untouched(), "quarantined aux store -> 409");

    reset_world();
    w_aux[3].enabled = 1;
    TEST_CHECK(run(body, &r) == 409 && untouched(), "aux already enabled -> 409");

    reset_world();
    w_aux[3].conflicted = 1;
    TEST_CHECK(run(body, &r) == 409 && untouched(), "aux conflicted -> 409");

    reset_world();
    w_zone.hyst_c = 0.1f;
    TEST_CHECK(run(body, &r) == 409 && untouched(), "hysteresis below aux range -> 409");

    reset_world();
    w_zone.min_on_s = 0;
    TEST_CHECK(run(body, &r) == 409 && untouched(), "min_on 0 -> 409");

    reset_world();
    w_zone.min_off_s = 4000;
    TEST_CHECK(run(body, &r) == 409 && untouched(), "min_off above range -> 409");

    reset_world();
    w_live_uses = true;
    TEST_CHECK(run(body, &r) == 409 && untouched(), "live working copy uses zone -> 409");

    reset_world();
    w_plan_ok = false;
    TEST_CHECK(run(body, &r) == 409 && untouched() && strstr(r.msg, "temp_source") != NULL,
               "plan refusal -> 409 with the plan's reason");
}

static void test_success(void)
{
    TEST_SECTION("move_zone_to_aux success");
    zone_aux_reply_t r;
    reset_world();
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1", &r) == 200, "200");
    TEST_CHECK(c_order_n == 4 && c_order[0] == T_FREE && c_order[1] == T_SET && c_order[2] == T_COMMIT &&
                   c_order[3] == T_DONE,
               "order: free zone, enable aux, rewrite profiles, release saved zone");
    TEST_CHECK(c_first_set.enabled == 1 && c_first_set.tc_zone_plus1 == 2 && c_first_set.hyst_c == 3.0f &&
                   c_first_set.min_on_s == 20 && c_first_set.min_off_s == 30,
               "aux entry copies zone params, tc_zone = zone");
    TEST_CHECK(c_commit_relay == 3 && c_commit_has_tc, "commit told relay 3 and TC present");
    TEST_CHECK(strstr(r.msg, "\"ok\":true") && strstr(r.msg, "\"zone\":1") && strstr(r.msg, "\"relay\":3") &&
                   strstr(r.msg, "\"profiles_affected\":2") && strstr(r.msg, "\"rules_retargeted\":5"),
               "ack names zone, relay and counts");
    TEST_CHECK(c_revert == 0 && c_restore == 0, "no undo on success");

    reset_world();
    w_zone.thermo_mask = 0;
    TEST_CHECK(run("move_zone_to_aux=1&confirm=1", &r) == 200 && c_first_set.tc_zone_plus1 == 0 && !c_commit_has_tc,
               "zone without a TC -> aux has no tc_zone");
}

static void test_rollbacks(void)
{
    TEST_SECTION("move_zone_to_aux rolls back every stage");
    zone_aux_reply_t r;
    const char *body = "move_zone_to_aux=1&confirm=1";

    reset_world();
    w_free_ok = false;
    TEST_CHECK(run(body, &r) == 500 && c_set == 0 && c_commit == 0 && c_restore == 0, "free fails -> nothing else ran");

    reset_world();
    w_aux_set_fail_on_call = 1;
    TEST_CHECK(run(body, &r) == 500 && c_commit == 0 && c_revert == 0 && c_restore == 1 && c_set == 1,
               "aux enable fails -> nothing to undo on aux, zone restored, no profile work");
    TEST_CHECK(w_zone.is_on_off && w_zone.relay_mask == 0x04 && strstr(r.msg, "everything restored"),
               "zone back as it was, reported clean");

    reset_world();
    w_commit_ok = false;
    TEST_CHECK(run(body, &r) == 500 && c_revert == 0 && c_set == 2 && c_restore == 1,
               "commit fails (it reverts its own slots) -> aux off, zone back, no second profile revert");
    TEST_CHECK(c_order[c_order_n - 2] == T_SET && c_order[c_order_n - 1] == T_RESTORE, "undo order: aux, zone");
    TEST_CHECK(!w_aux[3].enabled && w_zone.is_on_off && strstr(r.msg, "everything restored"), "world restored");

    reset_world();
    w_corrupt_after_commit = true;
    TEST_CHECK(run(body, &r) == 500 && c_revert == 1 && c_restore == 1 && strstr(r.msg, "read-back"),
               "read-back mismatch undoes all three steps");

    reset_world();
    w_corrupt_after_commit = true;
    w_revert_ok = false;
    TEST_CHECK(run(body, &r) == 500 && c_revert == 1 && strstr(r.msg, "ROLLBACK INCOMPLETE"),
               "failed profile revert reported");

    reset_world();
    w_commit_ok = false;
    w_aux_set_fail_on_call = 2;
    TEST_CHECK(run(body, &r) == 500 && strstr(r.msg, "ROLLBACK INCOMPLETE") && c_restore == 1,
               "failed aux undo reported but zone still restored");

    reset_world();
    w_aux_set_fail_on_call = 1;
    w_restore_ok = false;
    TEST_CHECK(run(body, &r) == 500 && strstr(r.msg, "ROLLBACK INCOMPLETE"), "failed zone restore reported");
}

void run_test_zone_aux_convert_core(void)
{
    test_requested();
    test_bad_input();
    test_refusals();
    test_success();
    test_rollbacks();
}
