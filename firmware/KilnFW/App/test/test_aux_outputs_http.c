// Host tests for App/drivers/http/aux_outputs_http_core.c -- WP-2 of
// docs/SPARE_RELAY_ONOFF_PLAN.md. #includes the core directly (the repo's
// convention) and drives it with a fake ops table: no httpd, no store, no relay
// board. Every refusal and status-code choice in the core has a case here.
//
// LOAD-BEARING PROPERTIES:
//   1. Mid-run (mode gate) refuses BOTH the config write and the manual toggle with
//      409, BEFORE any field is parsed and without touching the store or the relay.
//   2. A zone-claimed relay can never be made an aux output (409) and never be
//      toggled by the manual route (409: not an enabled aux relay).
//   3. A quarantined aux store is never overwritten (409).
//   4. Safety refusal is 403 (a relay never turns ON through a safety fault).
#include <stdio.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/http/aux_outputs_http_core.c"

/* ---- fakes ---- */
static bool f_blocked;
static const char *f_reason;
static aux_http_action_t f_last_action;
static uint8_t f_zones_union;
static bool f_quarantined;
static uint8_t f_enabled_mask;
static uint8_t f_conflict_mask;
static aux_output_t f_cur[AUX_OUTPUTS_COUNT + 1];
static esp_err_t f_set_ret;
static int f_set_calls;
static uint8_t f_set_relay_arg;
static aux_output_entry_t f_set_entry;
static uint8_t f_set_union_arg;
static aux_relay_result_t f_relay_ret;
static int f_relay_calls;
static uint8_t f_relay_arg;
static bool f_relay_on;
static bool f_busy;
static int f_claims;
static int f_releases;

static void reset_fakes(void)
{
    f_blocked = false;
    f_reason = NULL;
    f_zones_union = 0;
    f_quarantined = false;
    f_enabled_mask = 0;
    f_conflict_mask = 0;
    memset(f_cur, 0, sizeof(f_cur));
    for (int i = 1; i <= (int)AUX_OUTPUTS_COUNT; i++) {
        f_cur[i].tc_zone = AUX_TC_ZONE_NONE;
        f_cur[i].hyst_c = AUX_HYST_C_DEFAULT;
        f_cur[i].min_on_s = AUX_MIN_ON_OFF_S_DEFAULT;
        f_cur[i].min_off_s = AUX_MIN_ON_OFF_S_DEFAULT;
    }
    f_set_ret = ESP_OK;
    f_set_calls = 0;
    f_relay_ret = AUX_RELAY_OK;
    f_relay_calls = 0;
    f_busy = false;
    f_claims = 0;
    f_releases = 0;
}

static bool fk_blocked(aux_http_action_t a, char *reason, size_t cap)
{
    f_last_action = a;
    if (f_blocked && f_reason) {
        snprintf(reason, cap, "%s", f_reason);
    }
    return f_blocked;
}
static uint8_t fk_union(void) { return f_zones_union; }
static bool fk_get(uint8_t relay, aux_output_t *out)
{
    if (relay < 1 || relay > AUX_OUTPUTS_COUNT) {
        return false;
    }
    *out = f_cur[relay];
    return true;
}
static esp_err_t fk_set(uint8_t relay, const aux_output_entry_t *e, uint8_t u)
{
    f_set_calls++;
    f_set_relay_arg = relay;
    f_set_entry = *e;
    f_set_union_arg = u;
    return f_set_ret;
}
static uint8_t fk_enabled(void) { return f_enabled_mask; }
static uint8_t fk_conflict(void) { return f_conflict_mask; }
static bool fk_quar(void) { return f_quarantined; }
static aux_relay_result_t fk_relay(uint8_t relay, bool on)
{
    f_relay_calls++;
    f_relay_arg = relay;
    f_relay_on = on;
    return f_relay_ret;
}

static bool fk_claim(void)
{
    if (f_busy) {
        return false;
    }
    f_claims++;
    return true;
}
static void fk_release(void) { f_releases++; }

static const aux_http_ops_t OPS = {
    .mode_blocked = fk_blocked,
    .zones_union = fk_union,
    .get = fk_get,
    .set = fk_set,
    .enabled_mask = fk_enabled,
    .conflict_mask = fk_conflict,
    .quarantined = fk_quar,
    .set_relay = fk_relay,
    .claim = fk_claim,
    .release = fk_release,
};

static int do_set(const char *body)
{
    aux_http_reply_t r;
    memset(&r, 0, sizeof(r));
    aux_http_core_set(&OPS, body, &r);
    return r.status;
}
static int do_manual(const char *body)
{
    aux_http_reply_t r;
    memset(&r, 0, sizeof(r));
    aux_http_core_manual(&OPS, body, &r);
    return r.status;
}

static void test_set_mode_gate(void)
{
    TEST_SECTION("aux config POST: mode gate refuses mid-run with 409, store untouched");
    reset_fakes();
    f_blocked = true;
    f_reason = "refused -- a firing is active";
    aux_http_reply_t r;
    memset(&r, 0, sizeof(r));
    aux_http_core_set(&OPS, "relay=3&enabled=1", &r);
    TEST_CHECK(r.status == 409, "mid-run config write is 409");
    TEST_CHECK(strstr(r.msg, "firing") != NULL, "the gate's own reason is reported");
    TEST_CHECK(f_set_calls == 0, "store never written mid-run");
    TEST_CHECK(f_last_action == AUX_HTTP_ACTION_CONFIG, "config route asks the config-write gate");
    TEST_CHECK(do_set("garbage") == 409, "gate runs before field parsing");
    f_reason = NULL;
    memset(&r, 0, sizeof(r));
    aux_http_core_set(&OPS, "relay=3&enabled=1", &r);
    TEST_CHECK(r.status == 409 && r.msg[0] != '\0', "gate with no reason still reports one");
}

static void test_set_field_validation(void)
{
    TEST_SECTION("aux config POST: field validation (400) and omitted-field preservation");
    reset_fakes();
    TEST_CHECK(do_set("enabled=1") == 400, "missing relay");
    TEST_CHECK(do_set("relay=0&enabled=1") == 400, "relay 0");
    TEST_CHECK(do_set("relay=5&enabled=1") == 400, "relay 5");
    TEST_CHECK(do_set("relay=x&enabled=1") == 400, "relay non-numeric");
    TEST_CHECK(do_set("relay=1") == 400, "missing enabled");
    TEST_CHECK(do_set("relay=1&enabled=2") == 400, "enabled 2");
    TEST_CHECK(do_set("relay=1&enabled=1&tc_zone=9") == 400, "tc_zone past last zone");
    TEST_CHECK(do_set("relay=1&enabled=1&tc_zone=-2") == 400, "tc_zone below -1");
    TEST_CHECK(do_set("relay=1&enabled=1&hyst_c=0.1") == 400, "hyst below min");
    TEST_CHECK(do_set("relay=1&enabled=1&hyst_c=99") == 400, "hyst above max");
    TEST_CHECK(do_set("relay=1&enabled=1&hyst_c=nan") == 400, "hyst nan");
    TEST_CHECK(do_set("relay=1&enabled=1&hyst_c=abc") == 400, "hyst non-numeric");
    TEST_CHECK(do_set("relay=1&enabled=1&min_on_s=0") == 400, "min_on_s 0");
    TEST_CHECK(do_set("relay=1&enabled=1&min_on_s=3601") == 400, "min_on_s too long");
    TEST_CHECK(do_set("relay=1&enabled=1&min_off_s=0") == 400, "min_off_s 0");
    TEST_CHECK(do_set("relay=1&enabled=1&min_off_s=3601") == 400, "min_off_s too long");
    TEST_CHECK(f_set_calls == 0, "no rejected request reached the store");

    f_cur[2].tc_zone = 1;
    f_cur[2].hyst_c = 4.5f;
    f_cur[2].min_on_s = 77;
    f_cur[2].min_off_s = 88;
    TEST_CHECK(do_set("relay=2&enabled=1") == 200, "minimal enable accepted");
    TEST_CHECK(f_set_calls == 1 && f_set_relay_arg == 2, "store called for relay 2");
    TEST_CHECK(f_set_entry.enabled == 1 && f_set_entry.tc_zone_plus1 == 2 && f_set_entry.min_on_s == 77 &&
                   f_set_entry.min_off_s == 88 && f_set_entry.hyst_c == 4.5f,
               "omitted fields keep the stored values");
    TEST_CHECK(do_set("relay=2&enabled=1&tc_zone=-1&hyst_c=3.25&min_on_s=10&min_off_s=20") == 200, "full request");
    TEST_CHECK(f_set_entry.tc_zone_plus1 == 0 && f_set_entry.hyst_c == 3.25f && f_set_entry.min_on_s == 10 &&
                   f_set_entry.min_off_s == 20,
               "explicit fields override (tc_zone -1 = none)");
    TEST_CHECK(do_set("relay=4&enabled=0&tc_zone=2") == 200 && f_set_entry.tc_zone_plus1 == 3 && f_set_entry.enabled == 0,
               "disable accepted, tc_zone 2 -> plus1 3");
}

static void test_set_zone_conflict(void)
{
    TEST_SECTION("aux config POST: zone-claimed relay refused (409), disable always allowed");
    reset_fakes();
    f_zones_union = 0x05; /* zones own relays 1 and 3 */
    TEST_CHECK(do_set("relay=1&enabled=1") == 409, "enable on zone relay 1");
    TEST_CHECK(do_set("relay=3&enabled=1") == 409, "enable on zone relay 3");
    TEST_CHECK(f_set_calls == 0, "refused before the store is called");
    TEST_CHECK(do_set("relay=2&enabled=1") == 200, "enable on a free relay");
    TEST_CHECK(f_set_union_arg == 0x05, "store handed the live zones union");
    TEST_CHECK(do_set("relay=1&enabled=0") == 200, "disable on a zone relay is not a conflict");
}

static void test_set_store_errors(void)
{
    TEST_SECTION("aux config POST: quarantine and store error mapping");
    reset_fakes();
    f_quarantined = true;
    TEST_CHECK(do_set("relay=1&enabled=1") == 409, "quarantined store refused");
    TEST_CHECK(f_set_calls == 0, "quarantined store never written");
    f_quarantined = false;
    f_set_ret = ESP_ERR_INVALID_ARG;
    TEST_CHECK(do_set("relay=1&enabled=1") == 400, "store INVALID_ARG -> 400");
    f_set_ret = ESP_ERR_INVALID_STATE;
    TEST_CHECK(do_set("relay=1&enabled=1") == 409, "store INVALID_STATE -> 409");
    f_set_ret = ESP_FAIL;
    TEST_CHECK(do_set("relay=1&enabled=1") == 500, "store write failure -> 500");
}

static void test_claim(void)
{
    TEST_SECTION("aux writes hold the HTTP_SYNC claim: busy -> 409 untouched, released on every path");
    reset_fakes();
    f_enabled_mask = 0x08; /* relay 4 enabled */
    f_busy = true;
    TEST_CHECK(do_manual("relay=4&on=1") == 409, "manual toggle while the claim is busy is 409");
    TEST_CHECK(f_relay_calls == 0, "busy manual toggle never drives the relay");
    TEST_CHECK(do_set("relay=4&enabled=0") == 409, "aux config write while busy is 409");
    TEST_CHECK(f_set_calls == 0, "busy aux config write never touches the store");
    TEST_CHECK(f_claims == 0 && f_releases == 0, "busy: nothing claimed, nothing released");
    f_busy = false;
    TEST_CHECK(do_manual("relay=4&on=1") == 200, "manual toggle succeeds when free");
    TEST_CHECK(f_claims == 1 && f_releases == 1, "success path releases the claim");
    TEST_CHECK(do_manual("relay=9&on=1") == 400, "validation refusal");
    TEST_CHECK(do_manual("relay=1&on=1") == 409, "not-enabled refusal");
    f_blocked = true;
    f_reason = "run active";
    TEST_CHECK(do_manual("relay=4&on=1") == 409, "mode-gate refusal");
    TEST_CHECK(do_set("relay=4&enabled=0") == 409, "mode-gate refusal on config write");
    TEST_CHECK(f_claims == f_releases && f_claims == 3, "every claimed path released (3 claims, 3 releases); gate-refused paths take no claim");
    f_busy = true;
    TEST_CHECK(do_manual("relay=4&on=1") == 409 && f_last_action == AUX_HTTP_ACTION_MANUAL,
               "gate-refused request under a busy claim still reports the gate (409 via gate, not busy)");
    {
        aux_http_reply_t r;
        memset(&r, 0, sizeof(r));
        aux_http_core_manual(&OPS, "relay=4&on=1", &r);
        TEST_CHECK(strstr(r.msg, "run active") != NULL, "mid-run reply carries the mode-gate reason, not the busy text");
    }
}

static void test_manual(void)
{
    TEST_SECTION("aux manual toggle: gate, validation, enabled-only, result mapping");
    reset_fakes();
    f_enabled_mask = 0x08; /* relay 4 is an enabled aux output */
    f_blocked = true;
    f_reason = "refused -- autotune is active";
    TEST_CHECK(do_manual("relay=4&on=1") == 409, "mid-run manual toggle is 409");
    TEST_CHECK(do_manual("relay=4&on=0") == 409, "mid-run manual OFF is also 409 (blanket)");
    TEST_CHECK(f_relay_calls == 0, "relay never touched mid-run");
    TEST_CHECK(f_last_action == AUX_HTTP_ACTION_MANUAL, "manual route asks the raw-relay-write gate");
    f_blocked = false;

    TEST_CHECK(do_manual("on=1") == 400, "missing relay");
    TEST_CHECK(do_manual("relay=9&on=1") == 400, "relay out of range");
    TEST_CHECK(do_manual("relay=4") == 400, "missing on");
    TEST_CHECK(do_manual("relay=4&on=7") == 400, "on not 0/1");
    TEST_CHECK(do_manual("relay=1&on=1") == 409, "a relay that is not an enabled aux output (zone/free)");
    TEST_CHECK(do_manual("relay=3&on=0") == 409, "even OFF is refused on a non-aux relay");
    TEST_CHECK(f_relay_calls == 0, "refused requests never reach the relay board");

    TEST_CHECK(do_manual("relay=4&on=1") == 200, "enabled aux relay ON");
    TEST_CHECK(f_relay_calls == 1 && f_relay_arg == 4 && f_relay_on, "relay 4 driven ON");
    TEST_CHECK(do_manual("relay=4&on=0") == 200 && !f_relay_on, "relay 4 driven OFF");

    f_relay_ret = AUX_RELAY_ERR_RUNNING;
    TEST_CHECK(do_manual("relay=4&on=1") == 409, "RUNNING -> 409");
    f_relay_ret = AUX_RELAY_ERR_OWNED;
    TEST_CHECK(do_manual("relay=4&on=1") == 409, "OWNED -> 409");
    f_relay_ret = AUX_RELAY_ERR_BLOCKED;
    TEST_CHECK(do_manual("relay=4&on=1") == 409, "BLOCKED -> 409");
    f_relay_ret = AUX_RELAY_ERR_SAFETY;
    TEST_CHECK(do_manual("relay=4&on=1") == 403, "SAFETY -> 403");
    f_relay_ret = AUX_RELAY_ERR_NO_BOARD;
    TEST_CHECK(do_manual("relay=4&on=1") == 503, "NO_BOARD -> 503");
    f_relay_ret = AUX_RELAY_ERR_RANGE;
    TEST_CHECK(do_manual("relay=4&on=1") == 400, "RANGE -> 400");
    f_relay_ret = AUX_RELAY_ERR_IO_FAIL;
    TEST_CHECK(do_manual("relay=4&on=1") == 500, "IO_FAIL -> 500");
}

static void test_format(void)
{
    TEST_SECTION("aux GET formatting");
    reset_fakes();
    f_quarantined = true;
    f_enabled_mask = 0x0A;
    f_conflict_mask = 0x02;
    f_zones_union = 0x05;
    f_cur[2].enabled = 1;
    f_cur[2].conflicted = 1;
    f_cur[2].tc_zone = 1;
    f_cur[2].hyst_c = 3.0f;
    char buf[200];
    size_t n = aux_http_core_format_head(&OPS, buf, sizeof(buf));
    TEST_CHECK(n == strlen(buf), "head length matches");
    TEST_CHECK(strstr(buf, "\"quarantined\":true") && strstr(buf, "\"enabled_mask\":10") &&
                   strstr(buf, "\"conflict_mask\":2") && strstr(buf, "\"zones_relay_mask\":5") &&
                   strstr(buf, "\"relays\":["),
               "head carries state and masks");
    n = aux_http_core_format_entry(&OPS, 2, buf, sizeof(buf));
    TEST_CHECK(n == strlen(buf), "entry length matches");
    TEST_CHECK(strstr(buf, "\"relay\":2") && strstr(buf, "\"enabled\":true") && strstr(buf, "\"conflicted\":true") &&
                   strstr(buf, "\"tc_zone\":1") && strstr(buf, "\"hyst_c\":3.00"),
               "entry carries the config");
    n = aux_http_core_format_entry(&OPS, 1, buf, sizeof(buf));
    TEST_CHECK(strstr(buf, "\"enabled\":false") && strstr(buf, "\"tc_zone\":-1"), "default entry: disabled, no zone");
    n = aux_http_core_format_entry(&OPS, 9, buf, sizeof(buf));
    TEST_CHECK(n == 0 && buf[0] == '\0', "out-of-range relay formats nothing");
    char tiny[8];
    n = aux_http_core_format_head(&OPS, tiny, sizeof(tiny));
    TEST_CHECK(n < sizeof(tiny), "tiny buffer truncates, never overruns");
    TEST_CHECK(aux_http_core_format_head(&OPS, tiny, 0) == 0, "zero cap formats nothing");
}

void run_test_aux_outputs_http(void)
{
    test_set_mode_gate();
    test_set_field_validation();
    test_set_zone_conflict();
    test_set_store_errors();
    test_manual();
    test_claim();
    test_format();
}
