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

/* Everything the run needs that is bigger than a scalar, in ONE heap block so the httpd task's stack
 * carries only this function's scalars (the move handler runs deep inside the zones POST path). */
typedef struct {
    char reason[ZONE_AUX_MSG_MAX];
    char perr[ZONE_AUX_MSG_MAX];
    char what[ZONE_AUX_MSG_MAX];
    zone_aux_zone_info_t zi;
    zone_aux_zone_info_t zafter;
    aux_output_t prev;
    aux_output_t aafter;
    aux_output_entry_t old_entry;
    aux_output_entry_t ne;
    aux_output_entry_t raw_now;
    profiles_retarget_counts_t plan;
    profiles_retarget_counts_t done;
    aux_convert_journal_t jr;
} run_scratch_t;

/* A failed step: roll back, then report. A clean rollback also drops the in-progress marker; an
 * unclean one KEEPS it, so /api/readiness keeps flagging the half-done state. */
static void fail_rolled_back(const zone_aux_ops_t *ops, zone_aux_reply_t *reply, const char *what, bool clean)
{
    bool marker_cleared = clean && ops->journal_clear();
    snprintf(reply->msg, sizeof(reply->msg), "%s -- %s", what,
             !clean ? "ROLLBACK INCOMPLETE, check zones, aux outputs and profiles"
                    : marker_cleared ? "everything restored, nothing changed"
                                     : "everything restored but the in-progress marker could not be cleared");
    reply->status = 500;
}

static void journal_stage(const zone_aux_ops_t *ops, aux_convert_journal_t *jr, uint8_t stage)
{
    jr->stage = stage;
    (void)ops->journal_write(jr); /* informational: a resume re-derives the real state */
}

/* Final read-back, shared by the run and the resume: zone relay-less heater, aux enabled with the
 * right tc_zone, the union no longer claiming the relay, AND the zones and aux blobs re-read from
 * the cfg files equal to RAM (a RAM-only read-back cannot see a save that failed). */
static bool read_back_ok(const zone_aux_ops_t *ops, run_scratch_t *sc, uint8_t zone, uint8_t relay, bool has_tc)
{
    memset(&sc->zafter, 0, sizeof(sc->zafter));
    memset(&sc->aafter, 0, sizeof(sc->aafter));
    return ops->zone_get(zone, &sc->zafter) && sc->zafter.exists && !sc->zafter.is_on_off &&
           sc->zafter.relay_mask == 0 && ops->aux_get(relay, &sc->aafter) && sc->aafter.enabled &&
           !sc->aafter.conflicted && sc->aafter.tc_zone == (has_tc ? zone : AUX_TC_ZONE_NONE) &&
           (ops->zones_union() & (uint8_t)(1u << (relay - 1u))) == 0 && ops->verify_persisted();
}

static void reply_ok(zone_aux_reply_t *reply, uint8_t zone, uint8_t relay, const profiles_retarget_counts_t *done,
                     bool resumed)
{
    snprintf(reply->msg, sizeof(reply->msg),
             "{\"ok\":true,\"zone\":%u,\"relay\":%u,\"profiles_scanned\":%u,\"profiles_affected\":%u,"
             "\"rules_retargeted\":%u%s}",
             (unsigned)zone, (unsigned)relay, (unsigned)done->profiles_scanned, (unsigned)done->profiles_affected,
             (unsigned)done->rules_retargeted, resumed ? ",\"resumed\":true" : "");
    reply->status = 200;
}

/* Finishes a conversion an earlier run left half done (power loss, or a rollback that could not
 * finish). Forward only: zone already freed, relay named, the journal holds the aux entry. Runs just
 * the missing steps, then the same read-back. A failure keeps the marker so it can be retried. */
static void resume_run(const zone_aux_ops_t *ops, const char *body, run_scratch_t *sc, uint8_t zone,
                       zone_aux_reply_t *reply)
{
    long rl = 0;
    if (!field_long(body, "relay", 1, AUX_OUTPUTS_COUNT, &rl)) {
        reply_set(reply, 400, "resume needs relay=1..4 (the relay the interrupted conversion named)");
        return;
    }
    uint8_t relay = (uint8_t)rl;
    if (!ops->journal_read(&sc->jr)) {
        reply_set(reply, 409, "no interrupted conversion is recorded -- nothing to resume");
        return;
    }
    if (sc->jr.zone != zone || sc->jr.relay != relay) {
        snprintf(reply->msg, sizeof(reply->msg),
                 "the recorded interrupted conversion is zone %u relay %u, not the zone and relay requested",
                 (unsigned)sc->jr.zone, (unsigned)sc->jr.relay);
        reply->status = 409;
        return;
    }
    bool has_tc = sc->jr.has_tc != 0;
    if (!ops->zone_get(zone, &sc->zi) || !sc->zi.exists) {
        reply_set(reply, 400, "zone is not configured");
        return;
    }
    if (sc->zi.is_on_off) {
        /* Still ON_OFF: the interrupted run never freed it, so nothing was changed. */
        if (ops->journal_clear()) {
            reply_set(reply, 409,
                      "the interrupted conversion had not changed anything; the marker is cleared -- run the "
                      "conversion again if wanted");
        } else {
            reply_set(reply, 500, "the interrupted conversion had not changed anything but the marker could not "
                                  "be cleared");
        }
        return;
    }
    if (sc->zi.relay_mask != 0) {
        reply_set(reply, 409, "zone still owns a relay -- it was not freed, so there is nothing to resume");
        return;
    }
    if (ops->aux_quarantined() || !ops->aux_get(relay, &sc->prev)) {
        reply_set(reply, 409, "aux output store is quarantined or unreadable");
        return;
    }
    if (sc->prev.conflicted) {
        reply_set(reply, 409, "the aux output on that relay is in conflict with a zone");
        return;
    }
    if (!sc->prev.enabled) {
        memset(&sc->ne, 0, sizeof(sc->ne));
        sc->ne.enabled = 1;
        sc->ne.tc_zone_plus1 = has_tc ? (uint8_t)(zone + 1u) : 0;
        sc->ne.hyst_c = sc->jr.hyst_c;
        sc->ne.min_on_s = sc->jr.min_on_s;
        sc->ne.min_off_s = sc->jr.min_off_s;
        if (ops->aux_set(relay, &sc->ne, ops->zones_union()) != ESP_OK) {
            reply_set(reply, 500, "resume: enabling the aux output failed -- marker kept, retry");
            return;
        }
    } else if (sc->prev.tc_zone != (has_tc ? zone : AUX_TC_ZONE_NONE)) {
        reply_set(reply, 409, "the aux output on that relay is enabled with a different thermocouple zone");
        return;
    }
    journal_stage(ops, &sc->jr, 3);
    memset(&sc->done, 0, sizeof(sc->done));
    sc->perr[0] = '\0';
    if (!ops->profiles_resume(zone, relay, has_tc, &sc->done, sc->perr, sizeof(sc->perr))) {
        snprintf(reply->msg, sizeof(reply->msg), "resume: rewriting profiles failed: %.120s -- marker kept, retry",
                 sc->perr);
        reply->status = 500;
        return;
    }
    journal_stage(ops, &sc->jr, 4);
    if (!read_back_ok(ops, sc, zone, relay, has_tc)) {
        reply_set(reply, 500, "resume: read-back did not match -- marker kept, retry");
        return;
    }
    if (!ops->journal_clear()) {
        reply_set(reply, 500, "resume finished but the in-progress marker could not be cleared -- resume again");
        return;
    }
    reply_ok(reply, zone, relay, &sc->done, true);
}

static void run_locked(const zone_aux_ops_t *ops, const char *body, run_scratch_t *sc, zone_aux_reply_t *reply)
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

    sc->reason[0] = '\0';
    if (ops->mode_blocked(sc->reason, sizeof(sc->reason))) {
        reply_set(reply, 409, sc->reason[0] != '\0' ? sc->reason : "refused -- a firing or autotune run is active");
        return;
    }

    long resume = 0;
    if (field_long(body, "resume", 1, 1, &resume)) {
        resume_run(ops, body, sc, zone, reply);
        return;
    }
    if (ops->journal_read(&sc->jr)) {
        snprintf(reply->msg, sizeof(reply->msg),
                 "an earlier conversion (zone %u, relay %u, stage %u) did not finish -- send resume=1 with "
                 "relay=%u to finish it",
                 (unsigned)sc->jr.zone, (unsigned)sc->jr.relay, (unsigned)sc->jr.stage, (unsigned)sc->jr.relay);
        reply->status = 409;
        return;
    }

    zone_aux_zone_info_t *zi = &sc->zi;
    memset(zi, 0, sizeof(*zi));
    if (!ops->zone_get(zone, zi) || !zi->exists) {
        reply_set(reply, 400, "zone is not configured");
        return;
    }
    if (!zi->is_on_off) {
        reply_set(reply, 409, "zone is not an on/off zone");
        return;
    }
    if (zi->failsafe_on) {
        reply_set(reply, 409,
                  "zone has failsafe_state ON, which an aux output does not support -- set it to OFF first");
        return;
    }
    if (zi->relay_mask == 0 || (zi->relay_mask & (uint8_t)(zi->relay_mask - 1u)) != 0) {
        reply_set(reply, 409, "zone relay_mask must name exactly one relay");
        return;
    }
    uint8_t relay = 1;
    while (relay <= 8 && ((zi->relay_mask >> (relay - 1u)) & 1u) == 0u) {
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
    memset(&sc->prev, 0, sizeof(sc->prev));
    if (!ops->aux_get(relay, &sc->prev)) {
        reply_set(reply, 409, "aux output for that relay cannot be read");
        return;
    }
    if (sc->prev.enabled || sc->prev.conflicted) {
        reply_set(reply, 409, "an aux output is already configured on that relay");
        return;
    }
    if (!isfinite(zi->hyst_c) || zi->hyst_c < AUX_HYST_C_MIN || zi->hyst_c > AUX_HYST_C_MAX ||
        zi->min_on_s < AUX_MIN_ON_OFF_S_MIN || zi->min_on_s > AUX_MIN_ON_OFF_S_MAX ||
        zi->min_off_s < AUX_MIN_ON_OFF_S_MIN || zi->min_off_s > AUX_MIN_ON_OFF_S_MAX) {
        reply_set(reply, 409, "zone hysteresis or min on/off time is outside what an aux output accepts");
        return;
    }
    if (ops->live_uses_zone(zone)) {
        reply_set(reply, 409, "the live profile working copy has a rule for this zone -- save or discard it first");
        return;
    }
    bool has_tc = zi->thermo_mask != 0;
    memset(&sc->plan, 0, sizeof(sc->plan));
    sc->perr[0] = '\0';
    if (!ops->profiles_plan(zone, relay, has_tc, &sc->plan, sc->perr, sizeof(sc->perr))) {
        reply_set(reply, 409, sc->perr[0] != '\0' ? sc->perr : "a stored profile cannot be moved to an aux output");
        return;
    }

    /* Entry to put back on failure: the STORED entry exactly as it is now (raw), not the effective
     * view with defaults substituted -- a rollback that wrote the defaults back would change what is
     * persisted for that relay. */
    memset(&sc->old_entry, 0, sizeof(sc->old_entry));
    if (!ops->aux_get_raw(relay, &sc->old_entry)) {
        reply_set(reply, 409, "aux output for that relay cannot be read");
        return;
    }

    memset(&sc->ne, 0, sizeof(sc->ne));
    sc->ne.enabled = 1;
    sc->ne.tc_zone_plus1 = has_tc ? (uint8_t)(zone + 1u) : 0;
    sc->ne.hyst_c = zi->hyst_c;
    sc->ne.min_on_s = zi->min_on_s;
    sc->ne.min_off_s = zi->min_off_s;

    /* Marker first: from here until the final read-back a power loss leaves it behind. */
    memset(&sc->jr, 0, sizeof(sc->jr));
    sc->jr.zone = zone;
    sc->jr.relay = relay;
    sc->jr.stage = 1;
    sc->jr.has_tc = has_tc ? 1 : 0;
    sc->jr.hyst_c = zi->hyst_c;
    sc->jr.min_on_s = zi->min_on_s;
    sc->jr.min_off_s = zi->min_off_s;
    if (!ops->journal_write(&sc->jr)) {
        reply_set(reply, 500, "could not write the in-progress marker -- nothing changed");
        return;
    }

    zone_aux_free_result_t fr = ops->zone_free(zone);
    if (fr == ZONE_AUX_FREE_NOTHING_CHANGED) {
        bool marker_cleared = ops->journal_clear();
        reply_set(reply, 500, marker_cleared ? "freeing the zone failed -- nothing changed"
                                             : "freeing the zone failed -- nothing changed, but the in-progress "
                                               "marker could not be cleared");
        return;
    }
    if (fr == ZONE_AUX_FREE_UNCERTAIN) {
        reply_set(reply, 500,
                  "freeing the zone failed and putting it back could not be confirmed -- ROLLBACK INCOMPLETE, "
                  "check zones; the in-progress marker is kept");
        return;
    }
    journal_stage(ops, &sc->jr, 2);

    if (ops->aux_set(relay, &sc->ne, ops->zones_union()) != ESP_OK) {
        /* aux_set changes RAM (and maybe the cfg file) before the NVS write can fail, so a failure
         * does not mean nothing changed: when the entry no longer reads as the stored original, undo
         * it too, or the zone would get relay R back while an enabled aux still owns it. */
        memset(&sc->raw_now, 0, sizeof(sc->raw_now));
        bool aux_touched = !ops->aux_get_raw(relay, &sc->raw_now) ||
                           memcmp(&sc->raw_now, &sc->old_entry, sizeof(sc->raw_now)) != 0;
        fail_rolled_back(ops, reply, "enabling the aux output failed",
                         rollback(ops, zone, relay, aux_touched ? 2 : 1, &sc->old_entry));
        return;
    }
    journal_stage(ops, &sc->jr, 3);
    memset(&sc->done, 0, sizeof(sc->done));
    sc->perr[0] = '\0';
    if (!ops->profiles_commit(zone, relay, has_tc, &sc->done, sc->perr, sizeof(sc->perr))) {
        snprintf(sc->what, sizeof(sc->what), "rewriting profiles failed: %.120s", sc->perr);
        fail_rolled_back(ops, reply, sc->what, rollback(ops, zone, relay, 2, &sc->old_entry));
        return;
    }
    journal_stage(ops, &sc->jr, 4);

    /* Read everything back; any mismatch undoes all three steps. */
    if (!read_back_ok(ops, sc, zone, relay, has_tc)) {
        fail_rolled_back(ops, reply, "read-back did not match", rollback(ops, zone, relay, 3, &sc->old_entry));
        return;
    }

    ops->zone_done();
    if (!ops->journal_clear()) {
        reply_set(reply, 500,
                  "the zone was converted and read back, but the in-progress marker could not be cleared -- send "
                  "resume=1 with the relay to finish cleanup");
        return;
    }
    reply_ok(reply, zone, relay, &sc->done, false);
}

void zone_aux_convert_run(const zone_aux_ops_t *ops, const char *body, zone_aux_reply_t *reply)
{
    run_scratch_t *sc = ops->scratch_alloc(sizeof(*sc));
    if (sc == NULL) {
        reply_set(reply, 500, "out of memory -- nothing changed");
        return;
    }
    memset(sc, 0, sizeof(*sc));
    /* Set before any check and cleared on every exit: a run start (profile or autotune) refuses while
     * this is up, so it cannot land between the multi-write sequence's steps. */
    ops->busy(true);
    run_locked(ops, body, sc, reply);
    ops->busy(false);
    free(sc);
}
