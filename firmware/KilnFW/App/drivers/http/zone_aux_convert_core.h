// zone_aux_convert_core -- the HTTP-free decision core of the one-shot "move an ON_OFF zone to
// an aux output" action (docs/SPARE_RELAY_ONOFF_PLAN.md section 10). Carried as
// `move_zone_to_aux=Z confirm=1` on the existing POST /api/zones (no new route); the zones
// handler dispatches here after its mode-gate / interlock / HTTP_SYNC claim, and
// zone_aux_convert_http.c supplies the ops. Every refusal and the all-or-nothing ordering live
// here behind an injected ops table so the host test drives each one without httpd or flash.
//
// Order (the relay-conflict invariant forces it: a relay bit is in at most one of a zone
// relay_mask or an ENABLED aux entry, and the profile validator needs the aux entry enabled):
//   1. free zone Z (HEATER, relay_mask 0, failsafe 0, hyst/min_on/min_off 0), persisted
//   2. enable the aux entry on Z's old relay (copies hyst/min_on/min_off, tc_zone = Z if it has a TC)
//   3. rewrite every stored profile's rules for Z to the aux target, read back
//   4. read zone and aux back
// Any failure undoes the completed steps in reverse and reports whether the undo was clean.
//
// Status codes: 200 done; 400 malformed/out-of-range field or confirm missing; 409 refused with
// nothing changed (mode gate, not ON_OFF, nonzero failsafe_state, relay_mask not exactly one
// relay, relay already an aux, quarantined aux store, a running/paused/live profile uses Z, a
// rule an aux cannot represent); 500 a step failed and was rolled back (message says whether
// the rollback was clean).
#ifndef ZONE_AUX_CONVERT_CORE_H
#define ZONE_AUX_CONVERT_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "aux_outputs_cfg.h"
#include "profiles_store.h"

#define ZONE_AUX_MSG_MAX 200

typedef struct {
    bool exists;          /* zone index < thermo_count */
    bool is_on_off;       /* zone_type == ON_OFF */
    bool failsafe_on;     /* failsafe_state != 0 */
    uint8_t relay_mask;   /* raw */
    uint8_t thermo_mask;  /* nonzero = the zone has a thermocouple */
    float hyst_c;         /* effective */
    uint16_t min_on_s;    /* effective */
    uint16_t min_off_s;   /* effective */
} zone_aux_zone_info_t;

typedef struct {
    /* true = REFUSE; writes the operator-facing reason. */
    bool (*mode_blocked)(char *reason, size_t cap);
    bool (*zone_get)(uint8_t zone, zone_aux_zone_info_t *out);
    /* Frees zone `zone` (see above) and persists; saves what it replaced for zone_restore().
     * false = nothing changed. */
    bool (*zone_free)(uint8_t zone);
    /* Puts back exactly what zone_free() replaced and persists. */
    bool (*zone_restore)(uint8_t zone);
    /* Called once after a successful move: release whatever zone_free() kept. */
    void (*zone_done)(void);
    uint8_t (*zones_union)(void);
    bool (*aux_get)(uint8_t relay, aux_output_t *out);
    esp_err_t (*aux_set)(uint8_t relay, const aux_output_entry_t *entry, uint8_t zones_union);
    bool (*aux_quarantined)(void);
    /* true = the live working copy of a profile (live_profile) holds a rule for `zone`. */
    bool (*live_uses_zone)(uint8_t zone);
    bool (*profiles_plan)(uint8_t zone, uint8_t relay, bool zone_has_tc, profiles_retarget_counts_t *counts,
                          char *err, size_t cap);
    bool (*profiles_commit)(uint8_t zone, uint8_t relay, bool zone_has_tc, profiles_retarget_counts_t *counts,
                            char *err, size_t cap);
    /* Swaps every rule at the aux target back to `zone`; true = every changed slot persisted. */
    bool (*profiles_revert)(uint8_t zone, uint8_t relay);
} zone_aux_ops_t;

typedef struct {
    int status;
    char msg[ZONE_AUX_MSG_MAX]; /* plain-text error, or the JSON ack on 200 */
} zone_aux_reply_t;

/* True when `body` carries the move_zone_to_aux field (value validated by the run). */
bool zone_aux_convert_requested(const char *body);

/* Runs the whole action. The caller has already passed the zones route's gates and holds the
 * single-flight claim. */
void zone_aux_convert_run(const zone_aux_ops_t *ops, const char *body, zone_aux_reply_t *reply);

#endif // ZONE_AUX_CONVERT_CORE_H
