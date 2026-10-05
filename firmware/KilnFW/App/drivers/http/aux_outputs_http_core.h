// aux_outputs_http_core -- the HTTP-free decision core of the spare-relay aux routes
// (docs/SPARE_RELAY_ONOFF_PLAN.md WP-2). Every refusal and status-code choice lives
// here, behind an injected ops table, so the host test drives each one without httpd,
// the zones store, the relay board or the mode gate. aux_outputs_http.c is the thin
// wrapper that reads the body, calls these, and sends the reply.
//
//   GET  /api/aux_outputs         -- aux_http_core_format_json(): per-relay config/state
//   POST /api/aux_outputs         -- aux_http_core_set(): one relay's config, relay=N
//   POST /api/aux_outputs/manual  -- aux_http_core_manual(): admin manual on/off, idle only
//
// Status codes: 200 ok; 400 malformed/out-of-range field; 403 safety fault refuses a
// relay ON; 409 refused-and-nothing-changed (mode gate, zone conflict, quarantined store,
// not an enabled aux relay, relay owned/updating/crash-unacked); 500 write failed;
// 503 no relay board.
#ifndef AUX_OUTPUTS_HTTP_CORE_H
#define AUX_OUTPUTS_HTTP_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "aux_outputs_cfg.h"

#define AUX_HTTP_MSG_MAX 160

typedef enum {
    AUX_HTTP_ACTION_CONFIG = 0, /* write an aux config -- same gate as zones config writes */
    AUX_HTTP_ACTION_MANUAL,     /* manual relay on/off -- same gate as raw relay writes */
} aux_http_action_t;

/* Outcome of the manual relay write, mapped from dashboard_relay_result_t by the adapter. */
typedef enum {
    AUX_RELAY_OK = 0,
    AUX_RELAY_ERR_NO_BOARD,
    AUX_RELAY_ERR_RANGE,
    AUX_RELAY_ERR_RUNNING,  /* firing/autotune/restore active */
    AUX_RELAY_ERR_OWNED,    /* owned by a run */
    AUX_RELAY_ERR_SAFETY,   /* a safety fault refuses relay ON */
    AUX_RELAY_ERR_BLOCKED,  /* update in progress / unacknowledged crash */
    AUX_RELAY_ERR_IO_FAIL,
} aux_relay_result_t;

typedef struct {
    /* true = REFUSE; writes the operator-facing reason. */
    bool (*mode_blocked)(aux_http_action_t action, char *reason, size_t cap);
    uint8_t (*zones_union)(void); /* OR of every configured zone's relay_mask */
    bool (*get)(uint8_t relay, aux_output_t *out);
    esp_err_t (*set)(uint8_t relay, const aux_output_entry_t *entry, uint8_t zones_union);
    uint8_t (*enabled_mask)(void);
    uint8_t (*conflict_mask)(void);
    bool (*quarantined)(void);
    aux_relay_result_t (*set_relay)(uint8_t relay, bool on);
} aux_http_ops_t;

typedef struct {
    int status; /* HTTP status (200, 400, 403, 409, 500, 503) */
    char msg[AUX_HTTP_MSG_MAX]; /* plain-text error, or the JSON ack on 200 */
} aux_http_reply_t;

/* body is the urlencoded form: relay=1..4 enabled=0|1 [tc_zone=-1|0..n-1] [hyst_c=] [min_on_s=]
 * [min_off_s=]. Omitted optional fields keep the relay's current values. */
void aux_http_core_set(const aux_http_ops_t *ops, const char *body, aux_http_reply_t *reply);

/* body: relay=1..4 on=0|1. Refused unless the relay is an ENABLED aux relay. */
void aux_http_core_manual(const aux_http_ops_t *ops, const char *body, aux_http_reply_t *reply);

/* Header (quarantined/masks) into out; returns bytes written (truncated to cap-1). */
size_t aux_http_core_format_head(const aux_http_ops_t *ops, char *out, size_t cap);
/* One relay's JSON object (relay 1-based) into out; returns bytes written. */
size_t aux_http_core_format_entry(const aux_http_ops_t *ops, uint8_t relay, char *out, size_t cap);

#endif // AUX_OUTPUTS_HTTP_CORE_H
