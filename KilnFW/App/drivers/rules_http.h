// rules_http -- Settings > Relays & Rules page (TODO.md section 3's
// relay/alternate-function item), serving /settings/relays and its
// text-based API, using the rule-engine design TODO.md section 0 settled:
// a relay has a list of rules (OR across rules), each rule a flat AND'd
// list of conditions; condition types are a zone's temperature vs. a
// threshold, elapsed time since profile start vs. a threshold, or another
// relay's *commanded* state.
//
// IMPORTANT: this page stores and validates rule configs only. There is no
// rule-evaluator task -- TODO.md section 0 designed the rule engine,
// section 6 (the profile executor / control tick) is where it would
// actually run, and neither is built here. Saved rules are inert until
// then; the page says so explicitly so this doesn't read as a working
// automation feature to whoever's driving the kiln.
//
// A relay's rule_driven flag (mirrors TODO.md's RULE owner tag) is
// likewise recorded intent only -- enforcing it exclusively against
// MANUAL/PROFILE ownership needs the ownership machinery TODO.md section 0
// designed for section 6, not built here either.
//
// Field-count explosion (4 relays * 3 rules * 3 conditions * ~4 fields)
// made a fully flat form impractical, so this uses a compact line-based
// text format instead -- see rules_http.c's parser comment for the exact
// grammar. GET /api/rules regenerates that text; POST /api/rules takes it
// back verbatim (plus edits) as the raw request body, not form-encoded.
#ifndef RULES_HTTP_H
#define RULES_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads rules_cfg from NVS (namespace "kiln_cfg", key "rules_cfg";
 * ESP_ERR_NVS_NOT_FOUND is not an error, mirrors wifi_prov.c/zones_http.c)
 * and registers /settings/relays + GET/POST /api/rules on the server
 * wifi_provision_http.c already started. No hardware pointers needed. */
esp_err_t rules_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // RULES_HTTP_H
