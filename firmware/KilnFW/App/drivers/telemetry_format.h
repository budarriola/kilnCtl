// telemetry_format -- pure formatters for the firing/autotune telemetry
// lines telemetry_log.c emits over the debug UART, split out into their own
// no-FreeRTOS/no-ESP-IDF-logging file for the exact reason dashboard_json.c
// was split out of dashboard_http.c (see that file's own doc comment):
// these functions only ever touch profile_exec_status_t/
// autotune_engine_status_t and a caller-owned buffer, so pulling them out of
// telemetry_log.c (which needs FreeRTOS task creation, ESP_LOGI, and
// MALLOC_CAP_SPIRAM to actually run the emitting task) is what lets them be
// host-tested at all -- linking a host test against real
// profile_executor_get_status()/autotune_engine_get_status() bodies would
// drag in the whole control-task/relay_authority/thermal_guard dependency
// graph for no reason, when all a formatting test needs is the struct shape.
#ifndef TELEMETRY_FORMAT_H
#define TELEMETRY_FORMAT_H

#include <stddef.h>

#include "autotune_engine.h"
#include "profile_executor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped any time a field is added, removed, or changes meaning/units in
 * either format below -- carried as the "ver=" key on every FIRE line (and
 * the same numeral prefixes TUNE's tag too, "KTELn") so a PC-side parser
 * written against an older schema fails loudly (unknown/missing key)
 * instead of silently misreading a shifted line. */
#define TELEMETRY_LOG_SCHEMA_VERSION 1u

/* One line, tag "KTEL<ver> FIRE": elapsed time, profile/segment/dwell state,
 * the executor's current RAMPED target (profile_exec_status_t::target_c,
 * NOT a segment endpoint), then per ACTIVE zone: actual_c, actual_valid,
 * the signed error (actual - target, 0 if !actual_valid), commanded duty,
 * and the two feedforward-hold diagnostics (ff_hold_used_matrix/
 * ff_hold_infeasible). A zone with .active == false is skipped entirely,
 * same convention as profile_exec_zone_status_t itself.
 *
 * Per-zone firing_stats (mean_error_c etc., profile_exec_firing_stats_t) are
 * deliberately NOT included here -- unlike dashboard_json.c's JSON, this
 * line has to fit inside ONE uart_log_bridge.c queue entry (UART_LOG_TEXT_MAX
 * bytes, currently UART_PROTO_MAX_PAYLOAD-1 = 252); adding firing_stats'
 * ~16 more fields per zone would add on the order of 200 more bytes at 3
 * zones and blow that single-entry budget for data that is already visible
 * over HTTP (GET /api/profile_exec).
 *
 * Follows the exact snprintf-return convention dashboard_json.c's
 * append_zone_status_json() already uses in this codebase: the return value
 * is the number of bytes that WOULD have been written for an unbounded
 * buffer (so a caller compares it against cap to detect truncation), and
 * out[] is ALWAYS left NUL-terminated and never written past cap-1
 * regardless of how small cap is, down to cap==0 (nothing written at all).
 * Worst case at MAX31856_CHANNEL_COUNT==5 all-active zones is bigger than
 * UART_LOG_TEXT_MAX -- see telemetry_format.c's own comment on this
 * function for the exact arithmetic and why that's flagged rather than
 * "fixed": a real run targets at most a handful of zones. */
int telemetry_format_firing(const profile_exec_status_t *st, char *out, size_t cap);

/* One line, tag "KTEL<ver> TUNE": state, method, zone, elapsed time in the
 * current phase, sample_count, actual_c, actual_valid, commanded duty. When
 * state == AUTOTUNE_ENGINE_DONE, appends the fitted model (whichever of
 * model/relay is valid for this run's method), the confidence flags, and
 * proposed_gains. When state == AUTOTUNE_ENGINE_ABORTED, appends
 * abort_reason, sanitized (see telemetry_sanitize_token() in the .c) so a
 * free-text reason can never embed a space/'='/'"' of its own and break
 * this format's whitespace-delimited key=value parsing. Same NUL-
 * termination/truncation-safety and snprintf-return convention as
 * telemetry_format_firing() above. */
int telemetry_format_autotune(const autotune_engine_status_t *st, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif // TELEMETRY_FORMAT_H
