// Link-only stubs for test_dashboard_autotune_http_get_handlers.c: the POST paths of
// dashboard_autotune_http.c share the TU but are not exercised by the GET tests.
#include <stdio.h>
#include <stdlib.h>
#define STUB(n) long n(void) { fprintf(stderr, "UNEXPECTED CALL: " #n "\n"); abort(); return 0; }
STUB(httpd_req_recv)
STUB(readiness_gate_collect)
STUB(autotune_engine_run)
STUB(autotune_engine_run_relay)
STUB(autotune_engine_abort)
STUB(autotune_engine_accept)
STUB(danger_mode_blocks_start)
STUB(system_mode_gate_http_send_refusal)
const char *DASH_TAG = "dash";
struct { char pad[256]; } s_dash;
