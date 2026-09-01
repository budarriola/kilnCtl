// See dashboard_json.h for why these two functions live in their own
// dependency-light file. NOT a verbatim move out of dashboard_http.c: three
// fields (ff_hold_used_matrix, ff_hold_infeasible, ff_membership_change_count)
// were added to both format strings and their argument lists in the same
// commit that relocated these functions, and append_zone_status_json()'s
// truncation path has since been changed to log and still close the array
// (see its doc comment in dashboard_json.h) instead of bailing silently.
#include "dashboard_json.h"

#include <stdarg.h>
#include <stdio.h>

#include "esp_log.h"

static const char *TAG = "dashboard_json";

size_t json_append_clamped(char *json, size_t cap, size_t o, const char *fmt, ...)
{
    if (o > cap - 1) {
        /* Already at/over the limit (shouldn't happen if every prior call
         * went through this same function, but a caller mixing this with a
         * raw snprintf could hand in an already-overrun `o` -- clamp before
         * doing any arithmetic on it so `cap - o` below can never wrap). */
        return cap - 1;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(json + o, cap - o, fmt, ap);
    va_end(ap);
    if (n > 0) {
        o += (size_t)n;
    }
    return o > cap - 1 ? cap - 1 : o;
}

void json_escape(const char *src, char *out, size_t out_cap)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < out_cap; p++) {
        if (*p == '"' || *p == '\\') {
            if (o + 3 >= out_cap) {
                break;
            }
            out[o++] = '\\';
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

/* Shared by both handlers below: one JSON object per active zone. Appends
 * to *o, using the APPEND-into-json-buffer pattern every other handler in
 * dashboard_http.c already uses (the caller owns the buffer/APPEND macro
 * since C has no closures to hand this a local one). control_fields selects
 * between /api/profile_exec's exec-lifecycle shape and /api/control's
 * tuning-focused shape (TODO.md 6A.9 asks for both, as separate endpoints
 * with different focuses, not one bloated one). */
size_t append_zone_status_json(char *json, size_t cap, size_t o, const profile_exec_status_t *st,
                               bool control_fields)
{
    int n;
    bool first = true;
    n = snprintf(json + o, cap - o, "\"zones\":[");
    if (n < 0 || (size_t)n >= cap - o) goto truncated;
    o += (size_t)n;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        const profile_exec_zone_status_t *z = &st->zones[zi];
        if (!z->active) continue;
        char reason_escaped[sizeof(z->fault_reason) * 2 + 1];
        reason_escaped[0] = '\0';
        if (z->faulted) {
            json_escape(z->fault_reason, reason_escaped, sizeof(reason_escaped));
        }
        if (control_fields) {
            n = snprintf(json + o, cap - o,
                        "%s{\"zone\":%u,\"control_mode\":%u,\"actual_c\":%.2f,\"actual_valid\":%s,"
                        "\"duty\":%.3f,\"relay_on\":%s,\"pid_p\":%.4f,\"pid_i\":%.4f,\"pid_d\":%.4f,"
                        "\"pid_ff\":%.4f,\"cooling_limited\":%s,\"faulted\":%s,\"fault_guard\":%u,"
                        "\"heat_blocked\":%s,\"heat_blocked_sources\":%lu,\"ff_hold_used_matrix\":%s,"
                        "\"ff_hold_infeasible\":%s,\"ff_membership_change_count\":%lu}",
                        first ? "" : ",", zi, z->control_mode, (double)(z->actual_valid ? z->actual_c : 0.0f),
                        z->actual_valid ? "true" : "false", (double)z->duty,
                        z->relay_commanded_on ? "true" : "false", (double)z->pid_p, (double)z->pid_i,
                        (double)z->pid_d, (double)z->pid_ff, z->cooling_limited ? "true" : "false",
                        z->faulted ? "true" : "false", z->fault_guard,
                        z->heat_blocked ? "true" : "false", (unsigned long)z->heat_blocked_sources,
                        z->ff_hold_used_matrix ? "true" : "false", z->ff_hold_infeasible ? "true" : "false",
                        (unsigned long)z->ff_membership_change_count);
        } else {
            n = snprintf(json + o, cap - o,
                        "%s{\"zone\":%u,\"actual_c\":%.2f,\"actual_valid\":%s,\"relay_on\":%s,"
                        "\"duty\":%.3f,\"control_mode\":%u,\"faulted\":%s,\"fault_reason\":\"%s\","
                        "\"fault_guard\":%u,\"heat_blocked\":%s,\"heat_blocked_sources\":%lu,"
                        "\"ff_hold_used_matrix\":%s,\"ff_hold_infeasible\":%s,"
                        "\"ff_membership_change_count\":%lu}",
                        first ? "" : ",", zi, (double)(z->actual_valid ? z->actual_c : 0.0f),
                        z->actual_valid ? "true" : "false", z->relay_commanded_on ? "true" : "false",
                        (double)z->duty, z->control_mode, z->faulted ? "true" : "false", reason_escaped,
                        z->fault_guard, z->heat_blocked ? "true" : "false",
                        (unsigned long)z->heat_blocked_sources,
                        z->ff_hold_used_matrix ? "true" : "false", z->ff_hold_infeasible ? "true" : "false",
                        (unsigned long)z->ff_membership_change_count);
        }
        if (n < 0 || (size_t)n >= cap - o) goto truncated;
        o += (size_t)n;
        first = false;
    }
    n = snprintf(json + o, cap - o, "]");
    if (n > 0 && (size_t)n < cap - o) o += (size_t)n;
    return o;

truncated:
    /* A truncation here used to bail silently (return o unchanged, array
     * left open) -- the caller (dashboard_http.c's two handlers) still
     * appended its own trailing '}' on top of that, producing an
     * unterminated "zones":[{...},{...} -- invalid JSON served with HTTP
     * 200 and no error anywhere on the wire or in the log. Log it by name
     * (which shape, how big the buffer was) so a future field addition that
     * outgrows the budget is loud instead of silent, and still close the
     * array so the document stays at least PARSEABLE (fewer zones than
     * expected beats a syntax error). */
    ESP_LOGE(TAG, "%s JSON truncated at %u/%u bytes -- raise DASHBOARD_JSON_%s_BUF_SIZE "
             "(dashboard_json.h)", control_fields ? "/api/control" : "/api/profile_exec",
             (unsigned)o, (unsigned)cap, control_fields ? "CONTROL" : "PROFILE_EXEC");
    n = snprintf(json + o, cap - o, "]");
    if (n > 0 && (size_t)n < cap - o) o += (size_t)n;
    return o;
}
