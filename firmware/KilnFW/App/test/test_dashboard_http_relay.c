// test_dashboard_http_relay.c -- host test for the REAL dashboard_set_relay()
// and dashboard_http_get_safety_trip() (drivers/http/dashboard_http.c),
// R2-7 of HOST_TEST_COVERAGE_GAPS_ROUND2_2026-10-10.
//
// dashboard_set_relay() is the shared ownership/safety gate behind the Danger
// Zone relay route and the LCD per-zone relay toggle: it maps eight
// kiln_io_owner results onto the dashboard_relay_result_t the callers turn
// into HTTP statuses and LCD messages, and nothing exercised that mapping.
// dashboard_http_get_safety_trip() feeds the LCD/readiness trip display.
//
// APPROACH: dashboard_http.c is #included directly (no other seam into its
// file-scope state s_dash). Its private lvgl_port.h shim comes from
// stubs_dashboard_status, same as exe9. Every collaborator is faked at the
// symbol level; the two the scenarios drive are scriptable:
// kiln_io_owner_command_set_relay() and safety_link_get_status().
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#define TEST_ASSERT(cond, ...)                      \
    do {                                            \
        g_test_count++;                             \
        if (!(cond)) {                              \
            g_test_failures++;                      \
            printf("  FAIL line %d: ", __LINE__);   \
            printf(__VA_ARGS__);                    \
            printf("\n");                           \
        }                                           \
    } while (0)

#include "dashboard_http.c"

// ---- scriptable fakes ------------------------------------------------------
static kiln_io_owner_relay_result_t g_rr_result;
static uint32_t g_rr_sources;
static int g_rr_calls;
static uint8_t g_rr_last_relay;
static bool g_rr_last_on;
static bool g_rr_last_src_non_null;
static uint32_t *g_rr_last_src_ptr;

kiln_io_owner_relay_result_t kiln_io_owner_command_set_relay(uint8_t relay, bool on, uint32_t *out_safety_sources)
{
    g_rr_calls++;
    g_rr_last_relay = relay;
    g_rr_last_on = on;
    g_rr_last_src_non_null = out_safety_sources != NULL;
    g_rr_last_src_ptr = out_safety_sources;
    /* The owner writes the mask for every result that has one (SAFETY and
     * OWNED here); dashboard_set_relay() must hand it the caller's own
     * pointer and neither clobber nor replace what the owner wrote. */
    if (out_safety_sources && (g_rr_result == KILN_IO_OWNER_RELAY_ERR_SAFETY ||
                               g_rr_result == KILN_IO_OWNER_RELAY_ERR_OWNED)) {
        *out_safety_sources = g_rr_sources;
    }
    return g_rr_result;
}

static bool g_upd_blocked;
bool ota_http_heat_blocked_by_update(char *reason, size_t cap)
{
    if (g_upd_blocked && reason && cap) {
        snprintf(reason, cap, "update in progress");
    }
    return g_upd_blocked;
}

static esp_err_t g_sl_ret;
static safety_link_status_t g_sl_status;
static int g_sl_calls;
esp_err_t safety_link_get_status(SafetyLinkClass *link, safety_link_status_t *out)
{
    (void)link;
    g_sl_calls++;
    if (g_sl_ret == ESP_OK) {
        *out = g_sl_status;
    }
    return g_sl_ret;
}

// ---- inert fakes: the rest of the link surface of dashboard_http.c. None is
// reached by dashboard_set_relay()/dashboard_http_get_safety_trip(); they exist
// because the whole file is one translation unit. ----
esp_err_t autotune_abort_post_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t autotune_accept_post_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t autotune_matrix_get_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t autotune_start_post_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t autotune_status_get_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t autotune_trace_csv_get_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t control_status_get_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t dashboard_status_get_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t firing_history_get_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t history_csv_get_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t profile_exec_ack_last_run_post_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t profile_exec_pause_post_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t profile_exec_resume_post_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t profile_exec_start_post_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t profile_exec_status_get_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t profile_exec_stop_post_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t profile_plan_get_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t safety_clear_trip_post_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t safety_log_level_post_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
esp_err_t unit_pref_post_handler(httpd_req_t *req) { (void)req; return ESP_OK; }
bool MAX31856_bus_spi_wedged(const MAX31856BusClass *bus) { (void)bus; return false; }
uint8_t aux_outputs_cfg_enabled_mask(void) { return 0; }
bool cfg_fs_mount_format_confirmation_pending(void) { return false; }
const char *cfg_fs_mount_format_pending_reason(void) { return ""; }
esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out_size) { (void)chip; (void)out_size; return ESP_FAIL; }
esp_err_t esp_image_get_metadata(const esp_partition_pos_t *part, esp_image_metadata_t *metadata)
{
    (void)part; (void)metadata; return ESP_FAIL;
}
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 0; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 0; }
size_t heap_caps_get_minimum_free_size(uint32_t caps) { (void)caps; return 0; }
size_t heap_caps_get_total_size(uint32_t caps) { (void)caps; return 0; }
const char *kiln_cfg_swap_boot_fault_kind_name(kiln_cfg_swap_boot_fault_kind_t kind) { (void)kind; return ""; }
bool kiln_cfg_swap_get_boot_fault(kiln_cfg_swap_boot_fault_t *out) { (void)out; return false; }
esp_err_t kiln_http_register(httpd_handle_t server, const httpd_uri_t *uri_handler)
{
    (void)server; (void)uri_handler; return ESP_OK;
}
esp_err_t kiln_io_owner_command_read(kiln_io_state_t *out_state) { (void)out_state; return ESP_FAIL; }
void lvgl_port_get_flush_stats(uint32_t *last_us, uint32_t *max_us, uint32_t *count)
{
    if (last_us) { *last_us = 0; }
    if (max_us) { *max_us = 0; }
    if (count) { *count = 0; }
}
uint8_t profile_executor_aux_claim_mask(void) { return 0; }
bool ramp_assist_cfg_enabled(void) { return false; }
uint8_t relay_authority_latched_blocked_mask(void) { return 0; }
void relay_cycles_budget(uint8_t relay, relay_cycles_budget_t *out) { (void)relay; memset(out, 0, sizeof(*out)); }
void relay_cycles_get(uint32_t *out) { (void)out; }
void relay_cycles_get_type(uint8_t relay, relay_type_t *type, uint32_t *rated_override)
{
    (void)relay; (void)type; (void)rated_override;
}
relay_budget_tier_t relay_cycles_max_budget_tier(void) { return RELAY_BUDGET_TIER_NONE; }
uint32_t safety_link_get_fault_sources(SafetyLinkClass *link) { (void)link; return 0; }
esp_err_t safety_link_get_peer_build_status(SafetyLinkClass *link, bool *out_known, bool *out_dirty,
                                            uint8_t *commit_buf, uint8_t *out_commit_len, uint8_t *datetime_buf,
                                            uint8_t *out_datetime_len, uint8_t *out_config_version,
                                            uint16_t *out_config_crc)
{
    (void)link; (void)out_known; (void)out_dirty; (void)commit_buf; (void)out_commit_len; (void)datetime_buf;
    (void)out_datetime_len; (void)out_config_version; (void)out_config_crc;
    return ESP_FAIL;
}
esp_err_t safety_link_get_peer_version_status(SafetyLinkClass *link, bool *out_known, bool *out_compatible,
                                              uint16_t *out_peer_protocol, uint16_t *out_peer_min_compatible)
{
    (void)link; (void)out_known; (void)out_compatible; (void)out_peer_protocol; (void)out_peer_min_compatible;
    return ESP_FAIL;
}
esp_err_t thermo_owner_command_read_all(MAX31856Reading *out, size_t max_readings, size_t *out_count)
{
    (void)out; (void)max_readings; if (out_count) { *out_count = 0; } return ESP_FAIL;
}
void time_sync_get_status(time_sync_status_t *out) { memset(out, 0, sizeof(*out)); }
unit_pref_t unit_pref_get(void) { return (unit_pref_t)0; }
httpd_handle_t wifi_provision_http_get_server(void) { return NULL; }
float zones_config_apply_cal(uint8_t zone_index, float raw_c) { (void)zone_index; return raw_c; }
bool zones_config_get_load_fault(zones_cfg_load_fault_t *out) { (void)out; return false; }
bool zones_config_get_migration_persist_fault(zones_cfg_migration_persist_fault_t *out) { (void)out; return false; }
bool zones_config_is_valid(void) { return true; }

static void scenario_relay(void)
{
    // No board: refused before kiln_io_owner is consulted.
    memset(&s_dash, 0, sizeof(s_dash));
    g_rr_calls = 0;
    uint32_t src = 0xDEADu;
    TEST_ASSERT(dashboard_set_relay(1, true, &src) == DASHBOARD_RELAY_ERR_NO_BOARD, "no board -> NO_BOARD");
    TEST_ASSERT(g_rr_calls == 0, "no board: owner never called");
    TEST_ASSERT(src == 0xDEADu, "no board: out_safety_sources untouched");

    s_dash.io = (kiln_io_t *)(uintptr_t)0x1000;

    static const struct {
        kiln_io_owner_relay_result_t in;
        dashboard_relay_result_t out;
        const char *name;
    } map[] = {
        { KILN_IO_OWNER_RELAY_OK, DASHBOARD_RELAY_OK, "OK" },
        { KILN_IO_OWNER_RELAY_ERR_RANGE, DASHBOARD_RELAY_ERR_RANGE, "RANGE" },
        { KILN_IO_OWNER_RELAY_ERR_OWNED, DASHBOARD_RELAY_ERR_OWNED, "OWNED" },
        { KILN_IO_OWNER_RELAY_ERR_SAFETY, DASHBOARD_RELAY_ERR_SAFETY, "SAFETY" },
        { KILN_IO_OWNER_RELAY_ERR_IO_FAIL, DASHBOARD_RELAY_ERR_IO_FAIL, "IO_FAIL" },
        { KILN_IO_OWNER_RELAY_ERR_TIMEOUT, DASHBOARD_RELAY_ERR_IO_FAIL, "TIMEOUT folds into IO_FAIL" },
        { KILN_IO_OWNER_RELAY_ERR_UPDATING, DASHBOARD_RELAY_ERR_UPDATING, "UPDATING" },
        { KILN_IO_OWNER_RELAY_ERR_CRASH_UNACK, DASHBOARD_RELAY_ERR_CRASH_UNACK, "CRASH_UNACK" },
        { KILN_IO_OWNER_RELAY_ERR_RUNNING, DASHBOARD_RELAY_ERR_RUNNING, "RUNNING" },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        g_rr_result = map[i].in;
        g_rr_calls = 0;
        g_upd_blocked = (map[i].in == KILN_IO_OWNER_RELAY_ERR_UPDATING);
        dashboard_relay_result_t got = dashboard_set_relay(3, true, NULL);
        TEST_ASSERT(got == map[i].out, "owner result %s maps to dashboard result %d, got %d", map[i].name,
                    (int)map[i].out, (int)got);
        TEST_ASSERT(g_rr_calls == 1 && g_rr_last_relay == 3 && g_rr_last_on, "%s: owner called once with relay 3 on",
                    map[i].name);
        TEST_ASSERT(!g_rr_last_src_non_null, "%s: NULL sources stays NULL", map[i].name);
    }

    // UPDATING with the update already finished (race): still UPDATING.
    g_rr_result = KILN_IO_OWNER_RELAY_ERR_UPDATING;
    g_upd_blocked = false;
    TEST_ASSERT(dashboard_set_relay(2, true, NULL) == DASHBOARD_RELAY_ERR_UPDATING, "UPDATING race fallback");

    // Unknown owner result: default arm -> IO_FAIL.
    g_rr_result = (kiln_io_owner_relay_result_t)99;
    TEST_ASSERT(dashboard_set_relay(2, false, NULL) == DASHBOARD_RELAY_ERR_IO_FAIL, "unknown owner result -> IO_FAIL");
    TEST_ASSERT(!g_rr_last_on, "off request passed through as off");

    // dashboard_set_relay() forwards the caller's exact out_safety_sources
    // pointer to the owner and leaves whatever the owner wrote in place.
    g_rr_result = KILN_IO_OWNER_RELAY_ERR_SAFETY;
    g_rr_sources = 0x25u;
    src = 0;
    g_rr_last_src_ptr = NULL;
    TEST_ASSERT(dashboard_set_relay(4, true, &src) == DASHBOARD_RELAY_ERR_SAFETY, "SAFETY with sources");
    TEST_ASSERT(g_rr_last_src_ptr == &src, "SAFETY: caller's exact sources pointer forwarded to owner");
    TEST_ASSERT(src == 0x25u, "SAFETY: owner-written sources survive, got 0x%X", (unsigned)src);
    g_rr_result = KILN_IO_OWNER_RELAY_ERR_OWNED;
    g_rr_sources = 0x77u;
    src = 0;
    g_rr_last_src_ptr = NULL;
    TEST_ASSERT(dashboard_set_relay(4, true, &src) == DASHBOARD_RELAY_ERR_OWNED, "OWNED with sources ptr");
    TEST_ASSERT(g_rr_last_src_ptr == &src, "OWNED: caller's exact sources pointer forwarded to owner");
    TEST_ASSERT(src == 0x77u, "OWNED: owner-written sources not clobbered, got 0x%X", (unsigned)src);
}

static void scenario_safety_trip(void)
{
    bool up = true;
    uint16_t mask = 0xFFFFu;

    // No safety object: link down, mask 0, status never read.
    memset(&s_dash, 0, sizeof(s_dash));
    g_sl_calls = 0;
    dashboard_http_get_safety_trip(&up, &mask);
    TEST_ASSERT(!up && mask == 0 && g_sl_calls == 0, "no safety: down, mask 0, no read");

    s_dash.safety = (SafetyLinkClass *)(uintptr_t)0x2000;

    // Status read fails: link down, mask 0 even if the struct holds garbage.
    g_sl_ret = ESP_FAIL;
    memset(&g_sl_status, 0, sizeof(g_sl_status));
    g_sl_status.link_up = 1;
    g_sl_status.diag_trip_mask = 0x1234u;
    up = true;
    mask = 0xFFFFu;
    dashboard_http_get_safety_trip(&up, &mask);
    TEST_ASSERT(!up && mask == 0, "status error: down, mask 0 (got up=%d mask=0x%X)", (int)up, (unsigned)mask);

    // OK, link up with a trip mask.
    g_sl_ret = ESP_OK;
    dashboard_http_get_safety_trip(&up, &mask);
    TEST_ASSERT(up && mask == 0x1234u, "ok: up with mask 0x1234, got up=%d mask=0x%X", (int)up, (unsigned)mask);

    // OK, link down: mask still reported verbatim.
    g_sl_status.link_up = 0;
    g_sl_status.diag_trip_mask = 0x0020u;
    dashboard_http_get_safety_trip(&up, &mask);
    TEST_ASSERT(!up && mask == 0x0020u, "ok link down: mask 0x20 verbatim");

    // NULL out pointers are tolerated; each pointer is independent.
    dashboard_http_get_safety_trip(NULL, NULL);
    up = true;
    dashboard_http_get_safety_trip(&up, NULL);
    TEST_ASSERT(!up, "only link_up requested");
    mask = 0;
    dashboard_http_get_safety_trip(NULL, &mask);
    TEST_ASSERT(mask == 0x0020u, "only mask requested");
}

int main(void)
{
    scenario_relay();
    scenario_safety_trip();
    printf("test_dashboard_http_relay: %d checks, %d failures\n", g_test_count, g_test_failures);
    return g_test_failures == 0 ? 0 : 1;
}
