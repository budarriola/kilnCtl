// rules_http -- Settings > Relays & Rules page (TODO.md section 3's
// relay/alternate-function item), serving /settings/relays and its
// text-based API, using the rule-engine design TODO.md section 0 settled:
// a relay has a list of rules (OR across rules), each rule a flat AND'd
// list of conditions; condition types are a zone's temperature vs. a
// threshold, elapsed time since profile start vs. a threshold, or another
// relay's *commanded* state.
//
// This module stores/validates rule configs and serves the page; the rule
// engine itself (evaluating rules_cfg_t against live inputs and driving
// relays) is rules_eval.c (pure logic) + rules_task.c (the FreeRTOS task
// that samples live state and calls kiln_io_owner). rules_task.c reads this
// module's config via rules_http_get_cfg() below every tick rather than
// this module pushing changes to it -- same "poll the live config" pattern
// zones_http.c's zones_config_get_*() getters already use for
// profile_executor.c.
//
// A relay's rule_driven flag (mirrors relay_authority.h's RELAY_OWNER_RULE
// tag) is enforced by rules_task.c, which claims/releases that ownership
// tag to match this flag -- see rules_task.h's top comment.
//
// Field-count explosion (4 relays * 3 rules * 3 conditions * ~4 fields)
// made a fully flat form impractical, so this uses a compact line-based
// text format instead -- see rules_http.c's parser comment for the exact
// grammar. GET /api/rules regenerates that text; POST /api/rules takes it
// back verbatim (plus edits) as the raw request body, not form-encoded.
// GET /api/rules/status (rules_task.c-backed) reports live per-relay
// firing/ownership state for the page to poll.
#ifndef RULES_HTTP_H
#define RULES_HTTP_H

#include "esp_err.h"

#include "rules_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads rules_cfg from NVS (namespace "kiln_cfg", key "rules_cfg";
 * ESP_ERR_NVS_NOT_FOUND is not an error, mirrors wifi_prov.c/zones_http.c)
 * and registers /settings/relays + GET/POST /api/rules on the server
 * wifi_provision_http.c already started. No hardware pointers needed. */
esp_err_t rules_http_start(void);

/* Copies the current in-RAM rules_cfg_t out to *out. Read-only snapshot for
 * rules_task.c's tick -- no locking (same tolerance every zones_config_get_*
 * getter in zones_http.c already has: a config struct read racing a POST's
 * whole-struct assignment is, at worst, one stale/mixed tick's worth of
 * rule evaluation, never a torn pointer or a crash, and the next tick
 * self-corrects). */
void rules_http_get_cfg(rules_cfg_t *out);

#ifdef __cplusplus
}
#endif

#endif // RULES_HTTP_H
