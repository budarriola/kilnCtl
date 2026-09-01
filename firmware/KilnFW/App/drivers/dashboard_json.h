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
// char buffer -- so moving them here (unchanged, not reimplemented) makes them
// host-testable without inventing a stub wall for hardware this code never
// touches.
//
// Every call site in dashboard_http.c keeps calling json_escape()/
// append_zone_status_json() by the same names -- this split changes where the
// two functions are DEFINED, not their signatures or behavior.
#ifndef DASHBOARD_JSON_H
#define DASHBOARD_JSON_H

#include <stdbool.h>
#include <stddef.h>

#include "profile_executor.h"

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
 * Bails cleanly (returns o unchanged, appends nothing more) the instant a
 * snprintf() would truncate -- the CALLER is responsible for sizing json[]
 * generously enough that this never actually triggers on a real, worst-case
 * status; see dashboard_http.c's two call sites for the sizing math and
 * test_dashboard_json.c for the test that catches a budget going stale the
 * next time a field is added to either JSON shape. */
size_t append_zone_status_json(char *json, size_t cap, size_t o, const profile_exec_status_t *st,
                               bool control_fields);

#endif // DASHBOARD_JSON_H
