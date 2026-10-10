// Link-only stubs for test_dashboard_autotune_http_handlers.c: the GET paths of
// dashboard_autotune_http.c share the TU but are not exercised by the POST matrix.
#include <stdio.h>
#include <stdlib.h>
#define STUB(n) long n(void) { fprintf(stderr, "UNEXPECTED CALL: " #n "\n"); abort(); return 0; }
STUB(autotune_engine_get_status)
STUB(dashboard_format_autotune_status_json)
STUB(autotune_engine_get_coupling_matrix)
STUB(autotune_engine_get_trace)
STUB(httpd_resp_set_hdr)
STUB(httpd_resp_send_chunk)
STUB(autotune_engine_compute_rga)
STUB(json_append_clamped)
STUB(zones_config_get_thermo_count)
const char *DASH_TAG = "dash";
struct { char pad[256]; } s_dash;
