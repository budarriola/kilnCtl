// profiles_live_http -- docs/LIVE_PROFILE_EDIT_PLAN.md pass 2: the five-route
// HTTP surface over pass 1's live_profile.c/profile_executor_live_pickup.c
// backend (landed bca094fc). Deliberately its own file, not folded into
// profiles_http.c/profiles_edit_http.c/profiles_catalog_http.c: the plan's
// section 8 sequencing note names those three as contested with other
// in-flight work, and this feature reuses their exported seam
// (profiles_http_internal.h's profiles_parse_profile_fields()/
// profiles_validate_candidate()) rather than editing inside them.
#ifndef PROFILES_LIVE_HTTP_H
#define PROFILES_LIVE_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "live_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers GET /live_profile, GET+POST /api/profile/live, POST
 * /api/profile/live/fork and POST /api/profile/live/decide on the server
 * wifi_provision_http_start() already started. Call after profiles_http_start()
 * (main_network_http.c) -- this module calls profiles_http_get()/
 * profiles_http_save()/profiles_http_delete() (profiles_store.h) and
 * profiles_parse_profile_fields()/profiles_validate_candidate()
 * (profiles_http_internal.h), all of which are ready the moment
 * profiles_http_start() returns ESP_OK. */
esp_err_t profiles_live_http_start(void);

/* ---- decide: the ONE implementation behind both surfaces ----------------
 * POST /api/profile/live/decide (this file's handler) and the LCD "Keep
 * edit?" page (ui_page_live_decide.c) both call profiles_live_decide_apply();
 * neither re-implements a rule. The handler only maps the result code to an
 * HTTP status; the LCD maps it to a message. NVS is written on the CALLING
 * task (profiles_http_save()/live_profile_clear()), so the caller must not
 * be a PSRAM-stack task (the httpd task and the LVGL task are both fine).
 * Whole decisions are serialised by a static leaf mutex (created in
 * profiles_live_http_start()); if it cannot be taken within 1 s the call
 * returns LIVE_DECIDE_SERVER_ERROR ("another decision is in progress"). */
typedef enum {
    LIVE_DECIDE_OK = 0,
    LIVE_DECIDE_NOTHING_PENDING, /* HTTP 409 */
    LIVE_DECIDE_BAD_REQUEST,     /* HTTP 400 -- name collision, missing name/confirm, save refused */
    LIVE_DECIDE_FORBIDDEN,       /* HTTP 403 -- overwrite of a builtin origin */
    LIVE_DECIDE_BUSY,            /* HTTP 409 -- a zone conversion is running; retry */
    LIVE_DECIDE_SERVER_ERROR,    /* HTTP 500 -- out of memory, working copy unreadable, clear failed */
} profiles_live_decide_result_t;

/* `name` is used by SAVE_AS only (NULL/empty -> BAD_REQUEST "missing name");
 * `confirm` by OVERWRITE only (false -> BAD_REQUEST). `out_id` (may be NULL)
 * receives the saved slot id for SAVE_AS/OVERWRITE. `err` always gets a
 * human-readable reason on any non-OK result. */
profiles_live_decide_result_t profiles_live_decide_apply(live_edit_decision_kind_t kind, const char *name, bool confirm,
                                                          uint8_t *out_id, char *err, size_t err_cap);

typedef struct {
    bool record_pending;       /* a decision is owed (persisted record says pending) */
    bool pending_decision;     /* record_pending AND the executor is not RUNNING/PAUSED/FAULTED --
                                * exactly GET /api/profile/live's `pending_decision` */
    bool origin_is_builtin;    /* overwrite refused (structural, from the persisted record) */
    char origin_name[PROFILE_NAME_MAX_LEN + 1];
} profiles_live_decide_status_t;

/* Reads the persisted record (an NVS read -- callers that poll should cache
 * on live_profile_generation()) and the executor's live status. */
void profiles_live_decide_status(profiles_live_decide_status_t *out);

/* Auto-name for an LCD save-as (the LCD has no text entry): "<origin>-E",
 * then "-E2".. "-E9" until it collides with no USER slot, clipped to
 * PROFILE_NAME_MAX_LEN. False only if all candidates collide. The web
 * default is a typed name; this is the LCD substitute. */
bool profiles_live_decide_default_name(const profiles_live_decide_status_t *st, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif // PROFILES_LIVE_HTTP_H
