// Link-only stubs for test_dashboard_exec_http_handlers.c: the GET/status paths of
// dashboard_exec_http.c are in the same TU but are not exercised by the campaign-8
// matrix (start/stop/pause/resume/ack). Deliberately no headers: signatures are
// irrelevant for a symbol that is never called; calling one aborts the test loudly.
#include <stdio.h>
#include <stdlib.h>
#define STUB(n) long n(void) { fprintf(stderr, "UNEXPECTED CALL: " #n "\n"); abort(); return 0; }
STUB(safety_link_send_clear_trip)
STUB(profile_executor_get_status)
STUB(profile_executor_get_history)
STUB(profile_executor_get_firing_history)
STUB(httpd_resp_set_hdr)
STUB(httpd_resp_send_chunk)
STUB(httpd_req_get_url_query_str)
STUB(httpd_query_key_value)
STUB(append_zone_status_json)
STUB(dashboard_format_firing_history_json)
STUB(profile_feasibility_plan_curve)
STUB(profiles_http_get)
STUB(run_state_get_boot_record)
STUB(run_state_boot_record_interrupted)
STUB(run_state_phase_name)
const char *DASH_TAG = "dash";
struct { char pad[256]; } s_dash;
