#ifndef DASHBOARD_HTTP_INTERNAL_H
#define DASHBOARD_HTTP_INTERNAL_H

/* Internal seams for the dashboard_http.c split (2026-09-04, ROADMAP.md M15
 * "files over 1500 lines should be broken up where it makes sense" --
 * dashboard_http.c had grown to 2576 lines). This header is NOT public API
 * -- dashboard_http.h stays that -- it exists purely so pieces that used to
 * be one translation unit (and could reach each other's `static` state and
 * helpers for free) can still do so now that they are five:
 *
 *   dashboard_http.c          -- s_dash board-object storage, TAG, flash
 *                                 facts, reset_reason_name(),
 *                                 dashboard_get_status(),
 *                                 dashboard_http_get_hw_ready(),
 *                                 dashboard_set_relay(), and
 *                                 dashboard_http_start() (registers every
 *                                 handler defined in the other four files)
 *   dashboard_status_http.c   -- json_f() and GET /api/status
 *                                 (dashboard_status_get_handler())
 *   dashboard_settings_http.c -- POST /api/unit_pref, POST
 *                                 /api/safety/log_level
 *   dashboard_exec_http.c     -- profile executor family: GET
 *                                 /api/profile_exec, /api/profile_plan,
 *                                 /api/control, /api/firing_history,
 *                                 /api/history.csv; POST
 *                                 /api/profile_exec/{start,stop,pause,
 *                                 resume,ack_last_run}, POST
 *                                 /api/safety/clear_trip
 *   dashboard_autotune_http.c -- GET /api/autotune, /api/autotune/matrix,
 *                                 /api/autotune/trace.csv; POST
 *                                 /api/autotune/{start,abort,accept}
 *
 * Every symbol declared below was `static` in the original single file and
 * is widened to file-scope-internal linkage ONLY because a sibling .c file
 * in this split now calls or reads it directly. dashboard_http.c is never
 * pulled into a host-test translation unit (it #includes lvgl_port.h at
 * file scope, which drags in ILI9488.h's __attribute__((format(printf,...)))
 * -- a GCC extension MSVC's host toolchain rejects outright, confirmed by
 * build_host_tests.ps1's own comment above its test_dashboard_json.c
 * executable), so unlike profile_executor_internal.h's PE_TAG precedent
 * there is no risk of two `static const char *TAG = ...` colliding inside
 * one host-test TU -- but TAG is still widened the same way for consistency
 * and because dashboard_http.c is the one file among the split that must
 * see every other file's log tag conceptually as "the same dashboard log".
 *
 * status_get_handler() is renamed dashboard_status_get_handler() here (not
 * just widened) -- App/drivers already has two OTHER static
 * status_get_handler() functions (adaptive_tune_http.c, wifi_provision_http.c).
 * Those stay static and neither collides at link time today, but per the
 * repo's own audit rule ("a collision with a same-named static in another
 * file links silently and later breaks -- rename yours too, even when today's
 * check comes back clean") this one gets a `dashboard_` prefix rather than a
 * bare widen. */

#include "dashboard_http.h"

#include "esp_http_server.h"
#include "esp_system.h" /* esp_reset_reason_t -- reset_reason_name() */
#include "kiln_io.h" /* kiln_io_t */
#include "MAX31856.h" /* MAX31856BusClass */
#include "safety_link.h" /* SafetyLinkClass */

/* ---- shared log tag ------------------------------------------------------ */
extern const char *DASH_TAG;

/* ---- shared board-object storage (dashboard_http.c) ----------------------
 * Set once by dashboard_http_start(); read by every handler across the
 * split that needs s_dash.io / s_dash.thermo_bus / s_dash.safety. */
extern struct dashboard_board_objects {
    kiln_io_t *io;
    MAX31856BusClass *thermo_bus;
    SafetyLinkClass *safety;
} s_dash;

/* ---- dashboard_status_http.c ---------------------------------------------- */
esp_err_t dashboard_status_get_handler(httpd_req_t *req);

/* ---- dashboard_settings_http.c -------------------------------------------- */
esp_err_t unit_pref_post_handler(httpd_req_t *req);
esp_err_t safety_log_level_post_handler(httpd_req_t *req);

/* ---- dashboard_exec_http.c ------------------------------------------------ */
esp_err_t profile_exec_status_get_handler(httpd_req_t *req);
esp_err_t profile_plan_get_handler(httpd_req_t *req);
esp_err_t control_status_get_handler(httpd_req_t *req);
esp_err_t firing_history_get_handler(httpd_req_t *req);
esp_err_t history_csv_get_handler(httpd_req_t *req);
esp_err_t profile_exec_start_post_handler(httpd_req_t *req);
esp_err_t profile_exec_stop_post_handler(httpd_req_t *req);
esp_err_t profile_exec_pause_post_handler(httpd_req_t *req);
esp_err_t profile_exec_resume_post_handler(httpd_req_t *req);
esp_err_t profile_exec_ack_last_run_post_handler(httpd_req_t *req);
esp_err_t safety_clear_trip_post_handler(httpd_req_t *req);

/* ---- dashboard_autotune_http.c -------------------------------------------- */
esp_err_t autotune_status_get_handler(httpd_req_t *req);
esp_err_t autotune_matrix_get_handler(httpd_req_t *req);
esp_err_t autotune_start_post_handler(httpd_req_t *req);
esp_err_t autotune_abort_post_handler(httpd_req_t *req);
esp_err_t autotune_accept_post_handler(httpd_req_t *req);
esp_err_t autotune_trace_csv_get_handler(httpd_req_t *req);

#endif /* DASHBOARD_HTTP_INTERNAL_H */
