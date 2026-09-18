// http_auth_disclosure_gate.h -- the one owner of the house disclosure-gate
// expression documented in CLAUDE.md's "Tooling" section:
//
//     bool may_disclose = !http_auth_policy_web_enabled() ||
//                          http_auth_caller_is_admin(req);
//
// 2026-09-17 adversarial review of 6de75575 (finding 2): that commit added
// two ROUTE_TIER_OPEN handlers (wifi_provision_http.c's GET /status,
// readiness_http.c's crash-report checklist item) that each spelled this
// disjunction out by hand at their own call site, plus 661 lines of host
// tests. A negative test proved the tests were vacuous against exactly the
// defect that matters most here: replacing `bool may_disclose = ...` with
// `bool may_disclose = true;` at EITHER call site compiled clean and passed
// all 54/54 host-test executables, because both extracted helpers
// (wifi_prov_status_redact_field(), readiness_crash_report_detail()) are
// pure and fully covered, but the tests compose `may_disclose` by hand
// inside the test file rather than exercising the real call site -- and
// neither handler's .c file can be host-compiled at all (asm("_binary_...")
// blob externs, <sys/socket.h>), so no test can execute that line either
// way.
//
// This header closes that gap the only way available without a large
// per-handler host-compilation shim: it moves the DECISION itself out of
// each handler's uncompilable .c file into this tiny, dependency-light
// pair (http_auth_disclosure_gate.h/.c) that pulls in nothing a handler
// doesn't already need (http_auth_http.h, http_auth_policy_iface.h) and
// nothing that blocks host compilation. http_auth_disclosure_gate.c is the
// REAL implementation, linked directly (never stubbed) into
// kilnctl_host_tests_wifi_prov_status_disclosure.exe and
// kilnctl_host_tests_readiness_crash_disclosure.exe, so
// test_http_auth_may_disclose() in both test files now calls the exact
// object code either handler calls, not a hand-reimplemented copy of it.
//
// This closes the vacuous-test half of finding 2, but NOT the whole gap by
// itself: a host test linking this file can prove http_auth_may_disclose()
// itself is correct, but it still can never observe whether
// wifi_provision_http.c or readiness_http.c actually CALLS this function
// at their one gated site, since neither file is ever compiled by a host
// test. That residual gap -- "does the call site still call the shared
// gate, or was it replaced by a literal `true`" -- is what
// tools/check_disclosure_gate_call_sites.ps1 checks instead, by source
// inspection of exactly those two files. That script is honestly a text
// check, not a behavioural one; it exists because, with both handlers
// closed to host compilation, no behavioural check can reach the call site
// at all. Making the call site a single, named function call (rather than
// the bare disjunction spelled out inline) is what makes that text check
// simple and robust instead of fragile: it greps for one identifier,
// `http_auth_may_disclose(req)`, rather than trying to pattern-match a
// boolean expression that can be reshaped a dozen equivalent ways.
//
// Do NOT reshape the expression this function returns. It is a disjunction,
// not a conjunction, deliberately: http_auth_caller_is_admin() already
// returns true when web auth is off (that's the bootstrap path for setting
// the first admin password), so `&&` here would hide disclosure-gated
// fields from that operator and from this project's own commissioning
// tooling -- see CLAUDE.md's "Tooling: always go through the MCP facade"
// section and readiness_http.h's own doc comment for the full history.
#ifndef KILNCTL_HTTP_AUTH_DISCLOSURE_GATE_H
#define KILNCTL_HTTP_AUTH_DISCLOSURE_GATE_H

#include <stdbool.h>

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// True iff a caller of `req` may be shown a field gated behind admin
// disclosure (a saved SSID, static-IP topology, a crash report's exception
// detail, ...): web auth is currently off, OR the caller resolves to an
// authenticated administrator. See this header's own comment above for why
// it is a disjunction and why it lives in its own tiny translation unit.
bool http_auth_may_disclose(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_HTTP_AUTH_DISCLOSURE_GATE_H
