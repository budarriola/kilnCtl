// dashboard_json -- pure JSON-string-building helpers shared by dashboard_http.c's
// status/control handlers. Split out of dashboard_http.c (Opus review, blocker 1
// round 3: /api/control was silently emitting invalid JSON at 3 zones, and the
// fix needed a host test that renders the real per-zone object -- but
// dashboard_http.c #includes lvgl_port.h at file scope, which pulls in the LCD/
// touch driver stack (ILI9488.h's __attribute__((format(printf,...))), a GCC
// extension MSVC's host toolchain does not understand), so the whole
// translation unit cannot compile on the host build no matter how many
// ESP-IDF stub headers are added. These two functions have no such
// dependency -- they only ever touch profile_exec_status_t and a caller-owned
// char buffer -- so moving them here makes them host-testable without
// inventing a stub wall for hardware this code never touches. NOTE: this was
// NOT a verbatim, behavior-preserving move -- three fields (ff_hold_used_matrix,
// ff_hold_infeasible, ff_membership_change_count) were added to both format
// strings and their argument lists in the same commit that did the move, and
// the truncation path has since grown an ESP_LOGE + always-closed-bracket
// behavior it didn't originally have (see append_zone_status_json()'s own
// doc comment below). Treat this file as the current, evolving source of
// truth for these two functions, not a frozen copy of dashboard_http.c's.
//
// Every call site in dashboard_http.c keeps calling json_escape()/
// append_zone_status_json() by the same names -- this split changes where the
// two functions are DEFINED, not their signatures.
#ifndef DASHBOARD_JSON_H
#define DASHBOARD_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "autotune_engine.h"
#include "profile_executor.h"

/* Buffer-size expressions shared with dashboard_http.c's two call sites and
 * with test_dashboard_json.c, so the test can never go stale against a
 * reverted/changed handler buffer without itself failing to compile against
 * the same macro the handler uses (item 3, firmware cleanup pass: previously
 * the test hardcoded its own copy of these two expressions, so a handler
 * regression to the old undersized buffer would leave the test green). See
 * dashboard_http.c's own call sites for the sizing math these numbers come
 * from. */
/* Per-zone allowance raised from 512 -> 1024 (PID_EXPANSION_PLAN.md Phase 7a
 * dashboard wiring): append_zone_status_json()'s control_fields==false shape
 * (/api/profile_exec only -- /api/control's shape is unaffected) now appends
 * a nested "firing_stats" object per zone (profile_exec_zone_status_t::
 * firing_stats, profile_executor.h). Worst case measured by summing every
 * field's widest %-format output: 8x "%.2f" (mean_error_c, max_overshoot_c,
 * max_undershoot_c, iae_raw_c_s, ramp_err_mean_c, ramp_err_max_c,
 * dwell_err_mean_c, dwell_err_max_c) at up to 8 bytes each ("-1234.56") =
 * 64B, 5x "%lu" (max_overshoot_elapsed_s, max_undershoot_elapsed_s,
 * sample_count, excluded_sample_count, duration_s) at up to 10 bytes each
 * ("4294967295") = 50B, 2x "%u" (max_overshoot_segment,
 * max_undershoot_segment, both uint8_t) at up to 3 bytes each ("255") = 6B,
 * 1x "%.4f" (iae_normalized) at up to 10 bytes ("-1234.5678") = 10B --
 * 130B of values. Static text (keys + punctuation, format specifiers
 * subtracted out) is 342B. 130+342 = 472B worst case.
 *
 * PID_EXPANSION_PLAN.md sec 7.1/7.4 added four more fields to this same
 * shape (ramp_lag_sustained/ramp_lag_held_s/ramp_lag_commanded_rate_c_per_hr/
 * ramp_lag_achieved_rate_c_per_hr): 1x bool (5B "false") + 3x "%.2f" at up
 * to 8B each ("-1234.56") = 24B of values, plus ~115B of keys/punctuation =
 * ~144B more, bringing the worst case to 472+144 = 616B.
 *
 * PID_EXPANSION_PLAN.md sec 7.3 added one more field, "ramp_dwell_credit_s"
 * (1x "%.2f" at up to 8B, "-1234.56"): the key+colon+comma
 * (`"ramp_dwell_credit_s":`, then a trailing comma before the next field)
 * is 24B, plus the 8B value = 32B more, bringing the worst case to
 * 616+32 = 648B. 1024/zone still leaves >375B headroom. See
 * test_dashboard_json.c's fill_worst_case_zone() for the exact widths this
 * measures against. */
#define DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE (960 + MAX31856_CHANNEL_COUNT * 1024)
#define DASHBOARD_JSON_CONTROL_BUF_SIZE      (256 + MAX31856_CHANNEL_COUNT * 448)

/* GET /api/status's own buffer (dashboard_http.c's status_get_handler()) --
 * NOT a per-zone allowance like the two macros above (this endpoint has no
 * zones array), it is the whole response's worst-case size, moved here for
 * the same reason DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE/DASHBOARD_JSON_
 * CONTROL_BUF_SIZE already live here rather than as a local #define in
 * dashboard_http.c: so a host test (test_dashboard_json.c's
 * test_status_json_worst_case_render_fits_documented_buffer() and friends)
 * can reference the SAME constant the handler mallocs, and a revert/shrink
 * of one is a revert/shrink of both. status_get_handler() itself cannot be
 * host-tested directly (dashboard_http.c #includes lvgl_port.h at file
 * scope -- see this file's own top comment), so that test instead renders a
 * field-by-field MIRROR of the handler's APPEND sequence using the same
 * literal format strings and the same real helper functions this handler
 * calls (json_escape(), safety_trip_words_*(), safety_fault_source_words())
 * -- see that test for the exact worst-case value chosen per field, and
 * dashboard_http.c's own status_get_handler() doc comment (right above its
 * `#define DASHBOARD_STATUS_JSON_BUF_SIZE`) for the running sizing math
 * that PREVIOUSLY arrived at 4224. Sizing bug found by the opus review this
 * constant's move addresses: that number was folklore ("~100B headroom"),
 * never defended by anything that would go red on a revert or an
 * unbudgeted field addition -- and once it WAS actually checked (this
 * macro's own test, test_dashboard_json.c's
 * test_status_json_worst_case_render_fits_documented_buffer(), which
 * renders every field, including trip_reason_cause, at its true worst
 * width rather than assuming the byte count of its enclosing buffer), 4224
 * turned out to be 99 bytes too small: the dominant term the old hand
 * arithmetic approximated as "trip_reason_cause at its FULL cause_buf[320]-1
 * capacity" is actually reachable much closer to that cap than the rest of
 * the hand sum assumed once safety_trip_words_cause_numbered()'s S3 case
 * (4 embedded floats) is fed a hostile/corrupted current-sense reading over
 * the isolated UART link (the exact "hostile/uncommissioned current
 * reading" threat this file's own trip_reason_cause sizing comment already
 * named) -- measured 4323 bytes worst case, not "under 4224". Raised to
 * 4480 (same 128-byte-step convention as the earlier 4096->4224 bump) for
 * ~157 bytes of real, test-measured headroom rather than a folklore
 * figure. */
#define DASHBOARD_JSON_STATUS_BUF_SIZE 4480

/* Escapes '"' and '\\' for JSON string embedding. Truncates (never writes
 * past out_cap, always NUL-terminates) rather than overflow -- src is
 * sometimes an operator- or peer-supplied string (profile names, safety-link
 * build strings) that must never be trusted to fit. */
void json_escape(const char *src, char *out, size_t out_cap);

/* Appends `"zones":[...]` (one object per ACTIVE zone) to *o within the
 * caller-owned json/cap buffer, returning the new offset. control_fields
 * selects between /api/profile_exec's exec-lifecycle shape (false) and
 * /api/control's tuning-focused shape (true) -- TODO.md 6A.9 asks for both
 * as separate endpoints with different focuses, not one bloated one.
 *
 * The CALLER is responsible for sizing json[] generously enough that
 * truncation never actually happens on a real, worst-case status (see
 * dashboard_http.c's two call sites for the sizing math, and
 * test_dashboard_json.c for the test that catches a budget going stale the
 * next time a field is added to either JSON shape) -- but if it DOES
 * truncate anyway (a future field addition nobody re-sized for), this stops
 * cleanly rather than emitting invalid JSON: it logs an ESP_LOGE naming the
 * shape (control vs. profile_exec) and the buffer size, then still appends
 * the closing ']' so the caller's own trailing '}' produces a shorter-than-
 * expected but syntactically valid document (fewer zones than expected,
 * still parseable) instead of the pre-fix behavior of returning silently
 * with the array left open, which the caller's '}' turned into unparseable
 * JSON served with HTTP 200 and no error anywhere. */
size_t append_zone_status_json(char *json, size_t cap, size_t o, const profile_exec_status_t *st,
                               bool control_fields);

/* Self-clamping snprintf-append: writes at most one formatted chunk into
 * json[o..cap), and ALWAYS returns an offset <= cap - 1, never more --
 * unlike a bare `o += snprintf(json+o, cap-o, ...)`, which returns
 * snprintf's WOULD-BE length even when truncated, so a caller that keeps
 * chaining appends off that unclamped `o` can walk it past `cap`; the next
 * call's `cap - o` then wraps a size_t and writes out of bounds (dashboard_
 * http.c's autotune_matrix_get_handler hit exactly this after its RGA
 * block's json[] moved off the stack onto the heap during the 2026-08-31
 * httpd_worker stack-overflow fix -- a stack smash before, a heap smash
 * after, neither ever actually reachable at today's MAX31856_CHANNEL_COUNT,
 * but this closes the class regardless of how large a future n/cap gets).
 * Every call clamps immediately, so a long chain of appends into a
 * deliberately undersized buffer can never leave `o` unbounded partway
 * through -- see test_dashboard_json.c's own stress test against this
 * exact property. cap must be >= 1. */
size_t json_append_clamped(char *json, size_t cap, size_t o, const char *fmt, ...);

/* GET /api/autotune's whole response body, moved out of dashboard_http.c's
 * autotune_status_get_handler() (2026-08-31 dashboard-split pass) -- pure
 * formatting of an autotune_engine_status_t snapshot the caller already
 * fetched via autotune_engine_get_status(), no httpd/hardware touched here.
 * Same escaping (json_escape()) and same single-snprintf-into-caller-owned-
 * buffer shape as the original handler body -- NOT a behavior change, just a
 * different translation unit. Returns whatever snprintf returned (negative
 * on an encoding error, or the would-be length -- the caller decides how to
 * clamp that for httpd_resp_send(), same as the original code did). `json`
 * must be at least `cap` bytes; this never writes past `cap`. */
int dashboard_format_autotune_status_json(char *json, size_t cap, const autotune_engine_status_t *st);

/* GET /api/firing_history?profile_id=N's whole response body: up to
 * PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH persisted run records for one
 * profile, newest-first (profile_executor_get_firing_history()), each with
 * its per-zone firing_stats + PID gains in force at completion
 * (profile_firing_zone_record_t). Pure formatting, same shape/host-testing
 * rationale as dashboard_format_autotune_status_json() above -- the caller
 * already fetched `records`/`record_count` via
 * profile_executor_get_firing_history(), no httpd/hardware touched here.
 * Appends into the caller-owned json[cap] buffer with the same self-clamping
 * discipline as append_zone_status_json() (never writes past cap, always
 * NUL-terminates, closes every array/object it opened even if a later
 * record/zone doesn't fit). Returns the final offset (== strlen(json)). */
size_t dashboard_format_firing_history_json(char *json, size_t cap, uint8_t profile_id,
                                            const profile_firing_run_record_t *records,
                                            size_t record_count);

#endif // DASHBOARD_JSON_H
