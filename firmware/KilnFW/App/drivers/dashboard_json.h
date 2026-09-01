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

#include "profile_executor.h"

/* Buffer-size expressions shared with dashboard_http.c's two call sites and
 * with test_dashboard_json.c, so the test can never go stale against a
 * reverted/changed handler buffer without itself failing to compile against
 * the same macro the handler uses (item 3, firmware cleanup pass: previously
 * the test hardcoded its own copy of these two expressions, so a handler
 * regression to the old undersized buffer would leave the test green). See
 * dashboard_http.c's own call sites for the sizing math these numbers come
 * from. */
#define DASHBOARD_JSON_PROFILE_EXEC_BUF_SIZE (960 + MAX31856_CHANNEL_COUNT * 512)
#define DASHBOARD_JSON_CONTROL_BUF_SIZE      (256 + MAX31856_CHANNEL_COUNT * 448)

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

#endif // DASHBOARD_JSON_H
