// aux_outputs_http_core -- see the header. Pure: no httpd, no store, no hardware.
#include "aux_outputs_http_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_form.h"

#include "MAX31856.h"

/* parse result for one form field */
typedef enum { AUX_FIELD_OK = 0, AUX_FIELD_ABSENT, AUX_FIELD_BAD } aux_field_t;

static void reply_set(aux_http_reply_t *r, int status, const char *msg)
{
    r->status = status;
    snprintf(r->msg, sizeof(r->msg), "%s", msg);
}

static aux_field_t field_long(const char *body, const char *key, long min, long max, long *out)
{
    char val[16];
    int len = http_form_find_field(body, key, val, sizeof(val));
    if (len == -1) {
        return AUX_FIELD_ABSENT;
    }
    if (len < 0 || len == 0) {
        return AUX_FIELD_BAD;
    }
    char *end = NULL;
    long v = strtol(val, &end, 10);
    if (end == val || *end != '\0' || v < min || v > max) {
        return AUX_FIELD_BAD;
    }
    *out = v;
    return AUX_FIELD_OK;
}

static aux_field_t field_float(const char *body, const char *key, float *out)
{
    char val[24];
    int len = http_form_find_field(body, key, val, sizeof(val));
    if (len == -1) {
        return AUX_FIELD_ABSENT;
    }
    if (len < 0 || len == 0) {
        return AUX_FIELD_BAD;
    }
    char *end = NULL;
    float v = strtof(val, &end);
    if (end == val || *end != '\0' || !isfinite(v)) {
        return AUX_FIELD_BAD;
    }
    *out = v;
    return AUX_FIELD_OK;
}

static bool gate_refused(const aux_http_ops_t *ops, aux_http_action_t action, aux_http_reply_t *reply)
{
    char reason[AUX_HTTP_MSG_MAX];
    reason[0] = '\0';
    if (ops->mode_blocked(action, reason, sizeof(reason))) {
        reply_set(reply, 409, reason[0] != '\0' ? reason : "refused -- a firing or autotune run is active");
        return true;
    }
    return false;
}

void aux_http_core_set(const aux_http_ops_t *ops, const char *body, aux_http_reply_t *reply)
{
    /* Mode gate FIRST: aux config is zones-class config (SYS_ACTION_WRITE_ZONES_CONFIG),
     * refused with 409 while a firing or autotune run is active. */
    if (gate_refused(ops, AUX_HTTP_ACTION_CONFIG, reply)) {
        return;
    }
    long relay = 0;
    if (field_long(body, "relay", 1, AUX_OUTPUTS_COUNT, &relay) != AUX_FIELD_OK) {
        reply_set(reply, 400, "relay missing or out of range");
        return;
    }
    long enabled = 0;
    if (field_long(body, "enabled", 0, 1, &enabled) != AUX_FIELD_OK) {
        reply_set(reply, 400, "enabled missing or not 0/1");
        return;
    }

    /* Omitted optional fields keep the stored (effective) values. */
    aux_output_t cur;
    memset(&cur, 0, sizeof(cur));
    if (!ops->get((uint8_t)relay, &cur)) {
        reply_set(reply, 400, "relay out of range");
        return;
    }
    aux_output_entry_t e;
    memset(&e, 0, sizeof(e));
    e.enabled = (uint8_t)enabled;
    e.tc_zone_plus1 = cur.tc_zone == AUX_TC_ZONE_NONE ? 0 : (uint8_t)(cur.tc_zone + 1u);
    e.hyst_c = cur.hyst_c;
    e.min_on_s = cur.min_on_s;
    e.min_off_s = cur.min_off_s;

    long tz = 0;
    aux_field_t f = field_long(body, "tc_zone", -1, MAX31856_CHANNEL_COUNT - 1, &tz);
    if (f == AUX_FIELD_BAD) {
        reply_set(reply, 400, "tc_zone out of range (-1 = none)");
        return;
    }
    if (f == AUX_FIELD_OK) {
        e.tc_zone_plus1 = (uint8_t)(tz + 1);
    }
    float hyst = 0.0f;
    f = field_float(body, "hyst_c", &hyst);
    if (f == AUX_FIELD_BAD || (f == AUX_FIELD_OK && (hyst < AUX_HYST_C_MIN || hyst > AUX_HYST_C_MAX))) {
        reply_set(reply, 400, "hyst_c out of range");
        return;
    }
    if (f == AUX_FIELD_OK) {
        e.hyst_c = hyst;
    }
    long secs = 0;
    f = field_long(body, "min_on_s", AUX_MIN_ON_OFF_S_MIN, AUX_MIN_ON_OFF_S_MAX, &secs);
    if (f == AUX_FIELD_BAD) {
        reply_set(reply, 400, "min_on_s out of range");
        return;
    }
    if (f == AUX_FIELD_OK) {
        e.min_on_s = (uint16_t)secs;
    }
    f = field_long(body, "min_off_s", AUX_MIN_ON_OFF_S_MIN, AUX_MIN_ON_OFF_S_MAX, &secs);
    if (f == AUX_FIELD_BAD) {
        reply_set(reply, 400, "min_off_s out of range");
        return;
    }
    if (f == AUX_FIELD_OK) {
        e.min_off_s = (uint16_t)secs;
    }

    if (ops->quarantined()) {
        reply_set(reply, 409, "aux store holds newer-firmware data -- refusing to overwrite it");
        return;
    }
    uint8_t zones_union = ops->zones_union();
    /* Explicit conflict check, same predicate the store applies: an aux output may not
     * enable on a relay a zone claims. Named separately so the operator is told which side owns it. */
    if (e.enabled && aux_outputs_relay_conflict(zones_union, (uint8_t)(1u << (relay - 1)))) {
        reply_set(reply, 409, "that relay is claimed by a zone relay_mask -- remove it from the zone first");
        return;
    }
    esp_err_t err = ops->set((uint8_t)relay, &e, zones_union);
    if (err == ESP_ERR_INVALID_ARG) {
        reply_set(reply, 400, "aux field rejected");
        return;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        reply_set(reply, 409, "aux output refused: zone conflict or quarantined store");
        return;
    }
    if (err != ESP_OK) {
        reply_set(reply, 500, "applied live but could not be saved");
        return;
    }
    reply_set(reply, 200, "{\"ok\":true}");
}

void aux_http_core_manual(const aux_http_ops_t *ops, const char *body, aux_http_reply_t *reply)
{
    /* Idle-only (owner Q5): blanket-refused while a firing/autotune run is active,
     * whichever way the relay would move. */
    if (gate_refused(ops, AUX_HTTP_ACTION_MANUAL, reply)) {
        return;
    }
    long relay = 0;
    if (field_long(body, "relay", 1, AUX_OUTPUTS_COUNT, &relay) != AUX_FIELD_OK) {
        reply_set(reply, 400, "relay missing or out of range");
        return;
    }
    long on = 0;
    if (field_long(body, "on", 0, 1, &on) != AUX_FIELD_OK) {
        reply_set(reply, 400, "on missing or not 0/1");
        return;
    }
    /* This route moves ONLY enabled aux relays: a zone relay is never reachable here. */
    if ((ops->enabled_mask() & (1u << (relay - 1))) == 0) {
        reply_set(reply, 409, "that relay is not an enabled aux output");
        return;
    }
    switch (ops->set_relay((uint8_t)relay, on != 0)) {
    case AUX_RELAY_OK:
        reply_set(reply, 200, "{\"ok\":true}");
        break;
    case AUX_RELAY_ERR_RUNNING:
    case AUX_RELAY_ERR_OWNED:
        reply_set(reply, 409, "a firing, autotune run or sweep owns the relays -- manual control is not "
                              "available until it ends");
        break;
    case AUX_RELAY_ERR_BLOCKED:
        reply_set(reply, 409, "manual relay control is blocked (update in progress or unacknowledged crash)");
        break;
    case AUX_RELAY_ERR_SAFETY:
        reply_set(reply, 403, "a safety fault refuses turning a relay on");
        break;
    case AUX_RELAY_ERR_NO_BOARD:
        reply_set(reply, 503, "no relay board attached");
        break;
    case AUX_RELAY_ERR_RANGE:
        reply_set(reply, 400, "relay out of range");
        break;
    default:
        reply_set(reply, 500, "relay write failed");
        break;
    }
}

size_t aux_http_core_format_head(const aux_http_ops_t *ops, char *out, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    int n = snprintf(out, cap, "{\"quarantined\":%s,\"enabled_mask\":%u,\"conflict_mask\":%u,\"zones_relay_mask\":%u,"
                               "\"relays\":[",
                     ops->quarantined() ? "true" : "false", (unsigned)ops->enabled_mask(),
                     (unsigned)ops->conflict_mask(), (unsigned)ops->zones_union());
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n >= cap ? cap - 1 : (size_t)n;
}

size_t aux_http_core_format_entry(const aux_http_ops_t *ops, uint8_t relay, char *out, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    aux_output_t a;
    memset(&a, 0, sizeof(a));
    if (!ops->get(relay, &a)) {
        out[0] = '\0';
        return 0;
    }
    int n = snprintf(out, cap,
                     "{\"relay\":%u,\"enabled\":%s,\"conflicted\":%s,\"tc_zone\":%d,\"hyst_c\":%.2f,"
                     "\"min_on_s\":%u,\"min_off_s\":%u}",
                     (unsigned)relay, a.enabled ? "true" : "false", a.conflicted ? "true" : "false",
                     a.tc_zone == AUX_TC_ZONE_NONE ? -1 : (int)a.tc_zone, (double)a.hyst_c, (unsigned)a.min_on_s,
                     (unsigned)a.min_off_s);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n >= cap ? cap - 1 : (size_t)n;
}
