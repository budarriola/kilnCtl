// zone_aux_convert_core -- see the header. Pure: no httpd, no store, no hardware.
#include "zone_aux_convert_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_form.h"

#include "MAX31856.h"

static void reply_set(zone_aux_reply_t *r, int status, const char *msg)
{
    r->status = status;
    snprintf(r->msg, sizeof(r->msg), "%s", msg);
}

static bool field_long(const char *body, const char *key, long min, long max, long *out)
{
    char val[16];
    int len = http_form_find_field(body, key, val, sizeof(val));
    if (len <= 0) {
        return false;
    }
    char *end = NULL;
    long v = strtol(val, &end, 10);
    if (end == val || *end != '\0' || v < min || v > max) {
        return false;
    }
    *out = v;
    return true;
}

bool zone_aux_convert_requested(const char *body)
{
    char val[4];
    return body != NULL && http_form_find_field(body, "move_zone_to_aux", val, sizeof(val)) != -1;
}

/* Undo steps in reverse. `stage` is how far the run got: 1 = zone freed, 2 = aux enabled,
 * 3 = profiles rewritten. Returns true when every undo step worked. */
static bool rollback(const zone_aux_ops_t *ops, uint8_t zone, uint8_t relay, int stage,
                     const aux_output_entry_t *old_entry)
{
    bool clean = true;
    if (stage >= 3 && !ops->profiles_revert(zone, relay)) {
        clean = false;
    }
    if (stage >= 2) {
        /* Zone is still freed here, so the union does not claim the relay. */
        if (ops->aux_set(relay, old_entry, ops->zones_union()) != ESP_OK) {
            clean = false;
        }
    }
    if (stage >= 1 && !ops->zone_restore(zone)) {
        clean = false;
    }
    return clean;
}

static void fail_rolled_back(zone_aux_reply_t *reply, const char *what, bool clean)
{
    char m[ZONE_AUX_MSG_MAX];
    snprintf(m, sizeof(m), "%s -- %s", what,
             clean ? "everything restored, nothing changed"
                   : "ROLLBACK INCOMPLETE, check zones, aux outputs and profiles");
    reply_set(reply, 500, m);
}

void zone_aux_convert_run(const zone_aux_ops_t *ops, const char *body, zone_aux_reply_t *reply)
{
    long zl = 0;
    if (!field_long(body, "move_zone_to_aux", 0, MAX31856_CHANNEL_COUNT - 1, &zl)) {
        reply_set(reply, 400, "move_zone_to_aux missing or out of range");
        return;
    }
    uint8_t zone = (uint8_t)zl;
    long confirm = 0;
    if (!field_long(body, "confirm", 1, 1, &confirm)) {
        reply_set(reply, 400,
                  "confirm=1 required: this moves the zone relay to an aux output and rewrites every stored "
                  "profile; it cannot be reversed");
        return;
    }

    char reason[ZONE_AUX_MSG_MAX];
    reason[0] = '\0';
    if (ops->mode_blocked(reason, sizeof(reason))) {
        reply_set(reply, 409, reason[0] != '\0' ? reason : "refused -- a firing or autotune run is active");
        return;
    }

    zone_aux_zone_info_t zi;
    memset(&zi, 0, sizeof(zi));
    if (!ops->zone_get(zone, &zi) || !zi.exists) {
        reply_set(reply, 400, "zone is not configured");
        return;
    }
    if (!zi.is_on_off) {
        reply_set(reply, 409, "zone is not an on/off zone");
        return;
    }
    if (zi.failsafe_on) {
        reply_set(reply, 409,
                  "zone has failsafe_state ON, which an aux output does not support -- set it to OFF first");
        return;
    }
    if (zi.relay_mask == 0 || (zi.relay_mask & (uint8_t)(zi.relay_mask - 1u)) != 0) {
        reply_set(reply, 409, "zone relay_mask must name exactly one relay");
        return;
    }
    uint8_t relay = 1;
    while (relay <= 8 && ((zi.relay_mask >> (relay - 1u)) & 1u) == 0u) {
        relay++;
    }
    if (relay > AUX_OUTPUTS_COUNT) {
        reply_set(reply, 409, "zone relay is out of aux range");
        return;
    }
    if (ops->aux_quarantined()) {
        reply_set(reply, 409, "aux output store is quarantined -- clear it first");
        return;
    }
    aux_output_t prev;
    memset(&prev, 0, sizeof(prev));
    if (!ops->aux_get(relay, &prev)) {
        reply_set(reply, 409, "aux output for that relay cannot be read");
        return;
    }
    if (prev.enabled || prev.conflicted) {
        reply_set(reply, 409, "an aux output is already configured on that relay");
        return;
    }
    if (!isfinite(zi.hyst_c) || zi.hyst_c < AUX_HYST_C_MIN || zi.hyst_c > AUX_HYST_C_MAX ||
        zi.min_on_s < AUX_MIN_ON_OFF_S_MIN || zi.min_on_s > AUX_MIN_ON_OFF_S_MAX ||
        zi.min_off_s < AUX_MIN_ON_OFF_S_MIN || zi.min_off_s > AUX_MIN_ON_OFF_S_MAX) {
        reply_set(reply, 409, "zone hysteresis or min on/off time is outside what an aux output accepts");
        return;
    }
    if (ops->live_uses_zone(zone)) {
        reply_set(reply, 409, "the live profile working copy has a rule for this zone -- save or discard it first");
        return;
    }
    bool has_tc = zi.thermo_mask != 0;
    profiles_retarget_counts_t plan;
    memset(&plan, 0, sizeof(plan));
    char perr[ZONE_AUX_MSG_MAX];
    perr[0] = '\0';
    if (!ops->profiles_plan(zone, relay, has_tc, &plan, perr, sizeof(perr))) {
        reply_set(reply, 409, perr[0] != '\0' ? perr : "a stored profile cannot be moved to an aux output");
        return;
    }

    /* Entry to put back on failure: the disabled state that was there. */
    aux_output_entry_t old_entry;
    memset(&old_entry, 0, sizeof(old_entry));
    old_entry.tc_zone_plus1 = prev.tc_zone == AUX_TC_ZONE_NONE ? 0 : (uint8_t)(prev.tc_zone + 1u);
    old_entry.hyst_c = prev.hyst_c;
    old_entry.min_on_s = prev.min_on_s;
    old_entry.min_off_s = prev.min_off_s;

    aux_output_entry_t ne;
    memset(&ne, 0, sizeof(ne));
    ne.enabled = 1;
    ne.tc_zone_plus1 = has_tc ? (uint8_t)(zone + 1u) : 0;
    ne.hyst_c = zi.hyst_c;
    ne.min_on_s = zi.min_on_s;
    ne.min_off_s = zi.min_off_s;

    if (!ops->zone_free(zone)) {
        reply_set(reply, 500, "freeing the zone failed -- nothing changed");
        return;
    }
    if (ops->aux_set(relay, &ne, ops->zones_union()) != ESP_OK) {
        fail_rolled_back(reply, "enabling the aux output failed", rollback(ops, zone, relay, 1, &old_entry));
        return;
    }
    profiles_retarget_counts_t done;
    memset(&done, 0, sizeof(done));
    perr[0] = '\0';
    if (!ops->profiles_commit(zone, relay, has_tc, &done, perr, sizeof(perr))) {
        char m[ZONE_AUX_MSG_MAX];
        snprintf(m, sizeof(m), "rewriting profiles failed: %.120s", perr);
        fail_rolled_back(reply, m, rollback(ops, zone, relay, 2, &old_entry));
        return;
    }

    /* Read everything back; any mismatch undoes all three steps. */
    zone_aux_zone_info_t zafter;
    memset(&zafter, 0, sizeof(zafter));
    aux_output_t aafter;
    memset(&aafter, 0, sizeof(aafter));
    bool ok = ops->zone_get(zone, &zafter) && zafter.exists && !zafter.is_on_off && zafter.relay_mask == 0 &&
              ops->aux_get(relay, &aafter) && aafter.enabled && !aafter.conflicted &&
              aafter.tc_zone == (has_tc ? zone : AUX_TC_ZONE_NONE) &&
              (ops->zones_union() & (uint8_t)(1u << (relay - 1u))) == 0;
    if (!ok) {
        fail_rolled_back(reply, "read-back did not match", rollback(ops, zone, relay, 3, &old_entry));
        return;
    }

    ops->zone_done();
    char m[ZONE_AUX_MSG_MAX];
    snprintf(m, sizeof(m),
             "{\"ok\":true,\"zone\":%u,\"relay\":%u,\"profiles_scanned\":%u,\"profiles_affected\":%u,"
             "\"rules_retargeted\":%u}",
             (unsigned)zone, (unsigned)relay, (unsigned)done.profiles_scanned, (unsigned)done.profiles_affected,
             (unsigned)done.rules_retargeted);
    reply_set(reply, 200, m);
}
