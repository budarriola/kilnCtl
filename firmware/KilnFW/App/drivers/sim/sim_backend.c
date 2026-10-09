#include "sim_backend.h"

#if CONFIG_KILNCTL_SIM_PLANT

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "http_auth_http.h" // kiln_http_register() -- WEB_AUTH_PLAN.md section 5
#include "esp_log.h"
#include "hal_time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "http_form.h"
#include "uart_task_ids.h" /* THERMO_FAULT_* */
#include "wifi_provision_state.h" /* wifi_provision_get_httpd_handle() -- item 13 */
#include "zones_config_query.h"

/* The model itself lives in App/test/ and is compiled into this build by
 * App/drivers/CMakeLists.txt when CONFIG_KILNCTL_SIM_PLANT is set. Deliberately
 * the same file the host tests use -- a second, on-target copy of a thermal
 * model would be a second thing to keep honest, and the whole value of this
 * feature is that what trips a guard on the host trips it on the board. */
#include "../test/sim_plant.h"

static const char *TAG = "sim_backend";

#define SIM_MAX_BODY 256

typedef struct {
    SemaphoreHandle_t lock;
    sim_kiln_cfg_t    cfg;
    sim_kiln_state_t  state;
    bool              relay_on[SIM_KILN_MAX_ZONES];
    int64_t           last_step_us;
    bool              initialized;
} sim_backend_t;

static sim_backend_t s_sim;

static void sim_init_locked(void)
{
    memset(&s_sim.cfg, 0, sizeof(s_sim.cfg));

    uint8_t zones = zones_config_get_thermo_count();
    if (zones == 0) {
        /* Nothing configured yet (fresh NVS). Simulate the full channel
         * count rather than nothing, so a first-boot sim build still shows
         * a plausible board instead of an empty one -- the executor only
         * ever drives the zones a profile's zone_mask names anyway. */
        zones = MAX31856_CHANNEL_COUNT;
    }
    if (zones > SIM_KILN_MAX_ZONES) {
        zones = SIM_KILN_MAX_ZONES;
    }
    s_sim.cfg.zone_count = zones;

    for (int i = 0; i < zones; i++) {
        s_sim.cfg.zone[i].plant.ambient_c = (float)CONFIG_KILNCTL_SIM_AMBIENT_C;
        s_sim.cfg.zone[i].plant.thermal_mass_j_per_c = (float)CONFIG_KILNCTL_SIM_THERMAL_MASS_J_PER_C;
        s_sim.cfg.zone[i].plant.heater_power_w = (float)CONFIG_KILNCTL_SIM_HEATER_POWER_W;
        s_sim.cfg.zone[i].plant.loss_coeff_w_per_c = (float)CONFIG_KILNCTL_SIM_LOSS_W_PER_C;
        s_sim.cfg.zone[i].plant.sensor_delay_s = (float)CONFIG_KILNCTL_SIM_SENSOR_DELAY_S;
        s_sim.cfg.zone[i].plant.sensor_lag_tau_s = (float)CONFIG_KILNCTL_SIM_SENSOR_LAG_S;
        /* Kconfig has no float type; the radiative coefficient is entered in
         * units of 1e-12 W/K^4 so a sane default (2000 -> 2e-9) is an int. */
        s_sim.cfg.zone[i].radiative_coeff_w_per_k4 = (float)CONFIG_KILNCTL_SIM_RADIATIVE_COEFF_E12 * 1e-12f;
        for (int j = 0; j < zones; j++) {
            if (i == j) continue;
            s_sim.cfg.coupling_w_per_c[i][j] = (float)CONFIG_KILNCTL_SIM_COUPLING_W_PER_C;
        }
    }
    s_sim.cfg.sensor_noise_c = (float)CONFIG_KILNCTL_SIM_SENSOR_NOISE_MC / 1000.0f;

    sim_kiln_reset(&s_sim.state, &s_sim.cfg);
    memset(s_sim.relay_on, 0, sizeof(s_sim.relay_on));
    s_sim.last_step_us = (int64_t)hal_time_now_us();
    s_sim.initialized = true;

    ESP_LOGW(TAG, "SIMULATED PLANT ACTIVE: %d zone(s), %dW elements, coupling %dW/degC -- "
                  "reported temperatures are fabricated, no thermocouple is being read",
             zones, CONFIG_KILNCTL_SIM_HEATER_POWER_W, CONFIG_KILNCTL_SIM_COUPLING_W_PER_C);
}

/* Lazily created on first use: this module has no init() call site of its
 * own, and every entry point below is reached long after the scheduler is
 * running. Must be called with no lock held. */
static bool sim_ensure(void)
{
    if (!s_sim.lock) {
        SemaphoreHandle_t lock = xSemaphoreCreateMutex();
        if (!lock) {
            ESP_LOGE(TAG, "xSemaphoreCreateMutex failed -- simulation disabled");
            return false;
        }
        s_sim.lock = lock;
    }
    return true;
}

/* Advances the model to now. Wall-clock driven rather than
 * one-step-per-call, so the 1 Hz executor, the 2 s dashboard poll, and the
 * autotune engine can all read it without any of them making simulated time
 * run fast. Must be called with the lock held. */
static void sim_advance_locked(void)
{
    if (!s_sim.initialized) {
        sim_init_locked();
        return;
    }

    int64_t now_us = (int64_t)hal_time_now_us();
    float elapsed_s = (float)(now_us - s_sim.last_step_us) / 1e6f;
    if (elapsed_s <= 0.0f) {
        return;
    }
    /* Cap the catch-up: a long gap (Wi-Fi reconnect, debugger halt) must not
     * turn into one enormous forward-Euler step that makes the model itself
     * go unstable and reports a nonsense temperature. */
    if (elapsed_s > 60.0f) {
        elapsed_s = 60.0f;
    }
    s_sim.last_step_us = now_us;

    float duty[SIM_KILN_MAX_ZONES];
    for (int i = 0; i < SIM_KILN_MAX_ZONES; i++) {
        duty[i] = s_sim.relay_on[i] ? 1.0f : 0.0f;
    }

    /* Sub-step at <= 1 s so the integrator sees roughly the same step size
     * the host tests validated the model at, regardless of caller cadence. */
    while (elapsed_s > 0.0f) {
        float step_s = (elapsed_s > 1.0f) ? 1.0f : elapsed_s;
        sim_kiln_step(&s_sim.state, &s_sim.cfg, duty, step_s);
        elapsed_s -= step_s;
    }
}

esp_err_t sim_backend_read_all(MAX31856Reading *out, size_t max_readings, size_t *out_count)
{
    if (out_count) *out_count = 0;
    if (!out || max_readings == 0 || !sim_ensure()) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_sim.lock, portMAX_DELAY);
    sim_advance_locked();

    size_t count = 0;
    for (int zi = 0; zi < s_sim.cfg.zone_count && count < max_readings; zi++) {
        float reading_c = sim_kiln_reading_c(&s_sim.state, &s_sim.cfg, zi);
        /* The sensor_map indirection means an injected fault follows the
         * physical thermocouple, not the zone slot -- same as a real swapped
         * connector would. */
        sim_zone_fault_t fault = s_sim.state.fault[s_sim.cfg.sensor_map[zi]];

        MAX31856Reading *r = &out[count++];
        memset(r, 0, sizeof(*r));
        r->channel = (uint8_t)zi;
        r->cj_temperature_c = (float)CONFIG_KILNCTL_SIM_AMBIENT_C;

        if (fault == SIM_ZONE_FAULT_TC_OPEN || isnan(reading_c)) {
            /* Mirrors the real driver's contract: fault bit set, temperature
             * NaN, spi_failed false -- the bus worked, the sensor did not. */
            r->fault_status = THERMO_FAULT_OPEN;
            r->fault_pin_asserted = true;
            r->tc_temperature_c = NAN;
            /* Mirrors the real driver here too: a faulted channel produces no
             * usable conversion, so its age never refreshes. */
            r->age_ms = MAX31856_READING_AGE_UNKNOWN;
        } else {
            r->tc_temperature_c = reading_c;
        }
    }
    xSemaphoreGive(s_sim.lock);

    if (out_count) *out_count = count;
    return ESP_OK;
}

void sim_backend_note_zone_relay(uint8_t zone, bool on)
{
    if (zone >= SIM_KILN_MAX_ZONES || !sim_ensure()) {
        return;
    }
    xSemaphoreTake(s_sim.lock, portMAX_DELAY);
    /* Advance with the OLD relay state first, so the heat delivered up to
     * this instant is credited to what was actually commanded then. */
    sim_advance_locked();
    s_sim.relay_on[zone] = on;
    xSemaphoreGive(s_sim.lock);
}

/* ---- HTTP: GET/POST /api/sim ------------------------------------------- */

static const char *fault_name(sim_zone_fault_t f)
{
    switch (f) {
        case SIM_ZONE_FAULT_ELEMENT_DEAD: return "element_dead";
        case SIM_ZONE_FAULT_RELAY_WELDED: return "relay_welded";
        case SIM_ZONE_FAULT_TC_DETACHED:  return "tc_detached";
        case SIM_ZONE_FAULT_TC_FROZEN:    return "tc_frozen";
        case SIM_ZONE_FAULT_TC_OPEN:      return "tc_open";
        default:                           return "none";
    }
}

static bool fault_from_name(const char *name, sim_zone_fault_t *out)
{
    if (strcmp(name, "none") == 0)          { *out = SIM_ZONE_FAULT_NONE; return true; }
    if (strcmp(name, "element_dead") == 0)  { *out = SIM_ZONE_FAULT_ELEMENT_DEAD; return true; }
    if (strcmp(name, "relay_welded") == 0)  { *out = SIM_ZONE_FAULT_RELAY_WELDED; return true; }
    if (strcmp(name, "tc_detached") == 0)   { *out = SIM_ZONE_FAULT_TC_DETACHED; return true; }
    if (strcmp(name, "tc_frozen") == 0)     { *out = SIM_ZONE_FAULT_TC_FROZEN; return true; }
    if (strcmp(name, "tc_open") == 0)       { *out = SIM_ZONE_FAULT_TC_OPEN; return true; }
    return false;
}

static esp_err_t sim_get_handler(httpd_req_t *req)
{
    if (!sim_ensure()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sim unavailable");
    }

    char body[512];
    int n = snprintf(body, sizeof(body), "{\"simulated\":true,\"zones\":[");

    xSemaphoreTake(s_sim.lock, portMAX_DELAY);
    sim_advance_locked();
    for (int zi = 0; zi < s_sim.cfg.zone_count && n > 0 && n < (int)sizeof(body); zi++) {
        float reading_c = sim_kiln_reading_c(&s_sim.state, &s_sim.cfg, zi);
        int physical = s_sim.cfg.sensor_map[zi];

        /* NaN has no JSON spelling and "%.1f" would emit `nan` or `-nan(ind)`,
         * which is not parseable -- render it as null, matching how the rest
         * of this firmware's APIs report an unreadable channel. */
        char reading_str[24];
        if (isnan(reading_c)) {
            snprintf(reading_str, sizeof(reading_str), "null");
        } else {
            snprintf(reading_str, sizeof(reading_str), "%.1f", (double)reading_c);
        }

        n += snprintf(body + n, sizeof(body) - n,
                      "%s{\"zone\":%d,\"element_c\":%.1f,\"reading_c\":%s,\"relay_on\":%s,\"fault\":\"%s\"}",
                      (zi > 0) ? "," : "", zi, (double)sim_kiln_element_c(&s_sim.state, zi), reading_str,
                      s_sim.relay_on[zi] ? "true" : "false", fault_name(s_sim.state.fault[physical]));
    }
    xSemaphoreGive(s_sim.lock);

    if (n > 0 && n < (int)sizeof(body)) {
        snprintf(body + n, sizeof(body) - n, "]}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t sim_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= SIM_MAX_BODY) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
    }
    if (!sim_ensure()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sim unavailable");
    }

    char buf[SIM_MAX_BODY];
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read failed");
    }
    buf[received] = '\0';

    /* Connector swap: `swap=a,b` exchanges which physical thermocouple two
     * zones read, which is what a miswired kiln actually looks like and the
     * only way to provoke guard 2 (wrong-direction) on the board -- the
     * per-zone faults below cannot express it. Deliberately separate from
     * `fault` because it is a property of the wiring, not of a sensor. */
    char swap_str[16];
    if (http_form_find_field(buf, "swap", swap_str, sizeof(swap_str)) >= 0) {
        int a = -1, b = -1;
        if (sscanf(swap_str, "%d,%d", &a, &b) != 2) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "swap must be \"a,b\" (two zone indices)");
        }
        xSemaphoreTake(s_sim.lock, portMAX_DELAY);
        sim_advance_locked();
        if (a < 0 || b < 0 || a >= s_sim.cfg.zone_count || b >= s_sim.cfg.zone_count) {
            xSemaphoreGive(s_sim.lock);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "swap zone index out of range");
        }
        int tmp = s_sim.cfg.sensor_map[a];
        s_sim.cfg.sensor_map[a] = s_sim.cfg.sensor_map[b];
        s_sim.cfg.sensor_map[b] = tmp;
        ESP_LOGW(TAG, "simulated connector swap: zone %d now reads sensor %d, zone %d reads sensor %d", a,
                 s_sim.cfg.sensor_map[a], b, s_sim.cfg.sensor_map[b]);
        xSemaphoreGive(s_sim.lock);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }

    char zone_str[8];
    char fault_str[32];
    if (http_form_find_field(buf, "zone", zone_str, sizeof(zone_str)) < 0 ||
        http_form_find_field(buf, "fault", fault_str, sizeof(fault_str)) < 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zone and fault are required, or swap=a,b");
    }

    int zone = atoi(zone_str);
    sim_zone_fault_t fault;
    if (!fault_from_name(fault_str, &fault)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "fault must be none|element_dead|relay_welded|tc_detached|tc_frozen|tc_open");
    }

    xSemaphoreTake(s_sim.lock, portMAX_DELAY);
    sim_advance_locked();
    if (zone < 0 || zone >= s_sim.cfg.zone_count) {
        xSemaphoreGive(s_sim.lock);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zone out of range");
    }
    sim_kiln_inject_fault(&s_sim.state, &s_sim.cfg, zone, fault);
    xSemaphoreGive(s_sim.lock);

    ESP_LOGW(TAG, "injected fault '%s' on simulated zone %d", fault_str, zone);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t sim_backend_register_http(void)
{
    httpd_handle_t server = (httpd_handle_t)wifi_provision_get_httpd_handle();
    if (!server) {
        ESP_LOGW(TAG, "no httpd running -- /api/sim not registered");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t get_uri = {
        .uri = "/api/sim", .method = HTTP_GET, .handler = sim_get_handler, .user_ctx = NULL};
    static const httpd_uri_t post_uri = {
        .uri = "/api/sim", .method = HTTP_POST, .handler = sim_post_handler, .user_ctx = NULL};

    esp_err_t err = kiln_http_register(server, &get_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/sim) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = kiln_http_register(server, &post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/sim) failed: %s", esp_err_to_name(err));
    }
    return err;
}

#endif /* CONFIG_KILNCTL_SIM_PLANT */
