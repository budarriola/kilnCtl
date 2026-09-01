#include "adaptive_tune.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "http_form.h"
#include "pid_autotune.h"
#include "zones_http.h" // zones_config_get_model/set_model/get_pid/set_pid

// uart_bridge_ext.c's internal-SRAM-stack flash-write executor. Declared by
// hand rather than #include "uart_bridge.h" -- same reasoning safety_cfg_
// store.c's identical declaration gives: that header pulls in a pile of
// hardware bridge dependencies this file does not need, and which are not
// part of the host-test stub surface. A flash WRITE from a PSRAM-stacked
// task asserts on this board every time (see PID_EXPANSION_PLAN.md and
// project_psram_stack_nvs_panic); every NVS WRITE this file makes is
// dispatched through this worker for exactly that reason. Reads are not
// routed through it -- reading is not the hazard, only writing is (same
// distinction safety_cfg_store.c's own comments draw), and this module's
// only read (adaptive_tune_init()'s flag load) runs once at boot from
// app_main's task, which is not PSRAM-stacked.
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg);

// wifi_provision_http.c's shared httpd handle accessor. Hand-declared for
// the same host-test-stub-surface reason as the flash-worker declaration
// above -- wifi_provision_http.h pulls in wifi/netif headers this file has
// no other use for.
httpd_handle_t wifi_provision_http_get_server(void);

static const char *TAG = "adaptive_tune";

// ---------------------------------------------------------------------
// Guards -- every numeric bound this module can refuse an update on.
// ---------------------------------------------------------------------

// Settling: reuses autotune_engine.c's check_thermal_readiness_locked()
// approach (stability-primary slope test) rather than a second settled-ness
// test, per PID_EXPANSION_PLAN.md Phase 7d-2 -- same numbers, redefined
// here by hand since autotune_engine.c is off-limits to edit or include
// (another agent holds it live) and neither constant is exported.
#define ADAPTIVE_TUNE_SETTLE_MIN_S 180.0f          // == AUTOTUNE_ENGINE_SETTLE_S
#define ADAPTIVE_TUNE_SETTLE_SLOPE_FLOOR_C_PER_S 0.003f // == autotune_engine.c's SETTLE_ABS_SLOPE_FLOOR_C_PER_S

// A dwell duty below this is too close to the noise floor for a duty-vs-
// rise pair to mean anything -- a MAX31856 channel's practical noise floor
// is a few hundredths of a degree, and at 3% duty even a well-identified
// zone's rise is only a few degrees, so the signal-to-noise on the fit
// starts to collapse below this.
#define ADAPTIVE_TUNE_MIN_DUTY_FOR_OBSERVATION 0.03f

// One ring per zone. Cost: 8 bytes/observation (two floats) * 12 * 5 zones
// = 480 bytes -- small enough that PSRAM buys nothing; kept as a plain
// static array rather than heap_caps_malloc'd PSRAM, unlike autotune's much
// larger sample trace.
#define ADAPTIVE_TUNE_RING_CAPACITY 12

// Refuse to fit with fewer than this many observations -- the plan's own
// worked example (a single dwell yields 3 equations against 9 unknowns for
// the full coupled matrix) scales down to "one point cannot separate a
// slope from noise" even for this diagonal-only 1-parameter fit. 4 is a
// deliberately conservative floor above the theoretical minimum of 2.
#define ADAPTIVE_TUNE_MIN_OBSERVATIONS 4u

// The stacked duty values must span at least this much (max-min) or the
// fit is effectively estimating a slope from one operating point repeated
// several times -- the 1-D analogue of the plan's rank/conditioning check
// on the full coupled matrix. Below this, refuse rather than report a
// confident number the data cannot support.
#define ADAPTIVE_TUNE_MIN_DUTY_SPREAD 0.05f

// A raw fit more than this multiple away from the existing model (in
// either direction) is treated as an implausible fit rather than a real
// change in the element -- a genuinely aged element drifts run over run,
// it does not 5x between one firing and the next. Guards against a
// contaminated observation (a stuck relay, a misread thermocouple) that
// slipped past the settle/spread checks.
#define ADAPTIVE_TUNE_MAX_JUMP_RATIO 5.0f

// Blend fraction per Phase 7d-2 ("start near 15%") and the per-run cap on
// how far the blended value may move from the prior one -- independent
// numbers so a single very-off fit cannot swing the model by more than
// MAX_FRACTIONAL_MOVE even if ALPHA alone would have allowed it (they
// currently coincide at the same value, but are separate knobs on purpose).
#define ADAPTIVE_TUNE_BLEND_ALPHA 0.15f
#define ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE 0.20f

// A run whose zone lost more than this fraction of its samples to sensor
// dropout must not become training data -- same reasoning as Phase 7a's
// excluded_sample_count field this reuses.
#define ADAPTIVE_TUNE_MAX_EXCLUDED_FRACTION 0.05f

#define ADAPTIVE_TUNE_NVS_PARTITION "kiln_nvs"
#define ADAPTIVE_TUNE_NVS_NAMESPACE "adap_tune"
#define ADAPTIVE_TUNE_NVS_KEY_ENMASK "en_mask"

// sum(u*u) below this is too little energy to divide by -- a handful of
// near-zero-duty observations (which ADAPTIVE_TUNE_MIN_DUTY_FOR_OBSERVATION
// should already have excluded one at a time, but a stacked ring of several
// small-but-individually-legal duties could still sum small) would otherwise
// produce an enormous, meaningless K from dividing by almost nothing.
#define ADAPTIVE_TUNE_FIT_MIN_DENOM 1e-4

typedef struct {
    float duty;
    float rise_c; // actual_c - ambient_c at the settled instant
} adaptive_tune_obs_t;

typedef struct {
    bool enabled;

    // Settle-window tracking, mirrors autotune's readiness_start_* fields.
    bool  dwelling_prev;
    bool  settle_start_valid;
    float settle_start_c;
    float settle_elapsed_s;
    bool  recorded_this_dwell;

    // Observation ring (oldest evicted first).
    adaptive_tune_obs_t ring[ADAPTIVE_TUNE_RING_CAPACITY];
    uint32_t ring_count;
    uint32_t ring_head; // index of the OLDEST entry
    uint32_t observations_lifetime;

    // Last-applied bookkeeping, surfaced on the zones page.
    bool     has_applied;
    float    prior_k_dc;
    float    applied_k_dc;
    float    last_delta_pct;
    uint8_t  last_applied_profile_id;
    uint32_t last_applied_unix_s;
    char     last_refusal_reason[96];
} adaptive_tune_zone_t;

static adaptive_tune_zone_t s_zones[MAX31856_CHANNEL_COUNT];
static SemaphoreHandle_t s_lock; // guards s_zones; taken only from this file, never across profile_executor.c's
                                  // s_exec.lock (see adaptive_tune.h's doc comment on the lock order this keeps)
static bool s_lock_ready;

static void ensure_lock(void)
{
    if (!s_lock_ready) {
        s_lock = xSemaphoreCreateMutex();
        s_lock_ready = true;
    }
}

// ---------------------------------------------------------------------
// Pure fit math -- host-tested directly, see test_adaptive_tune.c.
// ---------------------------------------------------------------------

bool adaptive_tune_fit_gain(const float *duty, const float *rise_c, uint32_t n, float *out_k)
{
    if (!duty || !rise_c || !out_k || n == 0) {
        return false;
    }
    double num = 0.0, den = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        num += (double)duty[i] * (double)rise_c[i];
        den += (double)duty[i] * (double)duty[i];
    }
    if (den < ADAPTIVE_TUNE_FIT_MIN_DENOM) {
        return false; // too little duty energy in this set to divide by
    }
    *out_k = (float)(num / den);
    return true;
}

// ---------------------------------------------------------------------
// Layer 1 -- harvest a dwell observation, one per dwell, once settled.
// ---------------------------------------------------------------------

void adaptive_tune_zone_tick(uint8_t zone_index, float actual_c, bool actual_valid, float duty, bool dwelling,
                              float ambient_c, float dt_s)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return;
    }
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    adaptive_tune_zone_t *z = &s_zones[zone_index];

    if (!z->enabled || !actual_valid || isnan(ambient_c)) {
        // Learning is off, or this tick has nothing trustworthy to offer --
        // still reset the settle tracker below like any other non-dwelling
        // tick so a later dwell starts from a clean baseline rather than
        // one contaminated by a stale reading.
        if (!dwelling) {
            z->dwelling_prev = false;
            z->settle_start_valid = false;
        }
        xSemaphoreGive(s_lock);
        return;
    }

    if (!dwelling) {
        z->dwelling_prev = false;
        z->settle_start_valid = false;
        z->settle_elapsed_s = 0.0f;
        z->recorded_this_dwell = false;
        xSemaphoreGive(s_lock);
        return;
    }

    if (!z->dwelling_prev) {
        // Just entered this dwell -- start a fresh settle window.
        z->dwelling_prev = true;
        z->settle_start_valid = true;
        z->settle_start_c = actual_c;
        z->settle_elapsed_s = 0.0f;
        z->recorded_this_dwell = false;
    }
    z->settle_elapsed_s += dt_s;

    if (z->recorded_this_dwell || !z->settle_start_valid) {
        xSemaphoreGive(s_lock);
        return;
    }
    if (z->settle_elapsed_s < ADAPTIVE_TUNE_SETTLE_MIN_S) {
        xSemaphoreGive(s_lock);
        return; // still within the settle window -- keep waiting
    }
    float slope = fabsf(actual_c - z->settle_start_c) / z->settle_elapsed_s;
    if (slope > ADAPTIVE_TUNE_SETTLE_SLOPE_FLOOR_C_PER_S) {
        // Genuinely still drifting -- do NOT reset the window; a real drift
        // keeps failing this test every tick going forward (the elapsed-time
        // denominator only grows), which is the correct outcome. A brief
        // noise spike self-corrects on the next tick's smaller slope.
        xSemaphoreGive(s_lock);
        return;
    }

    // Settled. One observation per dwell, regardless of outcome below --
    // recorded_this_dwell is set on every path out from here so a marginal
    // (too-low-duty, implausible) dwell does not get re-evaluated every
    // tick for the rest of its length.
    z->recorded_this_dwell = true;

    if (duty < ADAPTIVE_TUNE_MIN_DUTY_FOR_OBSERVATION) {
        xSemaphoreGive(s_lock);
        return; // real steady state, but too little duty to trust the ratio
    }
    float rise_c = actual_c - ambient_c;
    if (!isfinite(rise_c) || rise_c <= 0.0f) {
        // A settled dwell above ambient always has positive rise on a
        // working heater; zero/negative here means a bad ambient capture
        // or a zone that never actually rose (thermocouple/relay fault
        // elsewhere) -- not a physically usable point either way.
        xSemaphoreGive(s_lock);
        return;
    }

    uint32_t slot = (z->ring_head + z->ring_count) % ADAPTIVE_TUNE_RING_CAPACITY;
    if (z->ring_count < ADAPTIVE_TUNE_RING_CAPACITY) {
        z->ring_count++;
    } else {
        z->ring_head = (z->ring_head + 1) % ADAPTIVE_TUNE_RING_CAPACITY; // evict oldest
    }
    z->ring[slot].duty = duty;
    z->ring[slot].rise_c = rise_c;
    z->observations_lifetime++;

    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------------
// Layer 2/3 -- batch fit, blend, guard, and apply -- called only from
// adaptive_tune_run_end(), i.e. only ever at a run boundary. This is what
// makes a mid-firing bump structurally impossible: there is no other call
// site that can reach zones_config_set_model()/set_pid() from this file.
// ---------------------------------------------------------------------

static void set_refusal(adaptive_tune_zone_t *z, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(z->last_refusal_reason, sizeof(z->last_refusal_reason), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "refined not applied: %s", z->last_refusal_reason);
}

static void try_refine_zone_locked(uint8_t zi, uint8_t profile_id)
{
    adaptive_tune_zone_t *z = &s_zones[zi];

    if (z->ring_count < ADAPTIVE_TUNE_MIN_OBSERVATIONS) {
        set_refusal(z, "only %u/%u dwell observations", (unsigned)z->ring_count,
                    (unsigned)ADAPTIVE_TUNE_MIN_OBSERVATIONS);
        return;
    }

    float duty[ADAPTIVE_TUNE_RING_CAPACITY], rise[ADAPTIVE_TUNE_RING_CAPACITY];
    float umin = INFINITY, umax = -INFINITY;
    for (uint32_t i = 0; i < z->ring_count; i++) {
        uint32_t idx = (z->ring_head + i) % ADAPTIVE_TUNE_RING_CAPACITY;
        duty[i] = z->ring[idx].duty;
        rise[i] = z->ring[idx].rise_c;
        if (duty[i] < umin) umin = duty[i];
        if (duty[i] > umax) umax = duty[i];
    }
    if ((umax - umin) < ADAPTIVE_TUNE_MIN_DUTY_SPREAD) {
        set_refusal(z, "observations too clustered (duty spread %.3f < %.3f)", (double)(umax - umin),
                    (double)ADAPTIVE_TUNE_MIN_DUTY_SPREAD);
        return;
    }

    float k_fit;
    if (!adaptive_tune_fit_gain(duty, rise, z->ring_count, &k_fit)) {
        set_refusal(z, "fit degenerate (insufficient duty energy)");
        return;
    }
    if (!(k_fit > 0.0f)) {
        set_refusal(z, "fitted gain %.4f is not positive", (double)k_fit);
        return;
    }

    float k_dc, tau_s, dead_time_s;
    if (!zones_config_get_model(zi, &k_dc, &tau_s, &dead_time_s) || !(k_dc > 0.0f)) {
        set_refusal(z, "no existing step-test model -- learning refines, it does not create one");
        return;
    }

    if (k_fit > k_dc * ADAPTIVE_TUNE_MAX_JUMP_RATIO || k_fit < k_dc / ADAPTIVE_TUNE_MAX_JUMP_RATIO) {
        set_refusal(z, "fit %.4f is implausible against prior K %.4f (>%.0fx)", (double)k_fit, (double)k_dc,
                    (double)ADAPTIVE_TUNE_MAX_JUMP_RATIO);
        return;
    }

    float k_blended = k_dc + ADAPTIVE_TUNE_BLEND_ALPHA * (k_fit - k_dc);
    float max_move = k_dc * ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE;
    if (k_blended > k_dc + max_move) k_blended = k_dc + max_move;
    if (k_blended < k_dc - max_move) k_blended = k_dc - max_move;
    if (!(k_blended > 0.0f)) {
        set_refusal(z, "blended gain %.4f is not positive", (double)k_blended);
        return;
    }

    // Recompute PID gains through the SAME rule autotune's Accept path
    // uses (pid_autotune.c's pid_autotune_tune_from_fopdt(), SIMC, default
    // lambda) -- never a second, looser tuning formula. tau_s/dead_time_s
    // are carried over UNCHANGED: dwell data cannot inform dynamics (see
    // adaptive_tune.h's scope note), only the gain.
    fopdt_model_t model = {
        .k_gain_c_per_duty = k_blended,
        .tau_s = tau_s,
        .dead_time_s = dead_time_s,
        .valid = true,
        .settled = true,
        .tau_consistent_with_gain = true,
        .extrapolation_converged = true,
    };
    autotune_gains_t gains = pid_autotune_tune_from_fopdt(&model, AUTOTUNE_RULE_SIMC, 0.0f);
    if (gains.refusal != AUTOTUNE_REFUSAL_OK) {
        set_refusal(z, "SIMC refused the refined model: %s", gains.refusal_reason);
        return;
    }

    if (!zones_config_set_model(zi, k_blended, tau_s, dead_time_s)) {
        set_refusal(z, "zones_config_set_model() rejected %.4f/%.1f/%.1f", (double)k_blended, (double)tau_s,
                    (double)dead_time_s);
        return;
    }
    if (!zones_config_set_pid(zi, gains.kp, gains.ki, gains.kd)) {
        set_refusal(z, "zones_config_set_pid() rejected %.4f/%.4f/%.4f", (double)gains.kp, (double)gains.ki,
                    (double)gains.kd);
        return;
    }

    z->last_refusal_reason[0] = '\0';
    z->has_applied = true;
    z->prior_k_dc = k_dc;
    z->applied_k_dc = k_blended;
    z->last_delta_pct = (k_dc > 0.0f) ? ((k_blended - k_dc) / k_dc) * 100.0f : 0.0f;
    z->last_applied_profile_id = profile_id;
    z->last_applied_unix_s = (uint32_t)time(NULL);

    ESP_LOGI(TAG, "zone %u: K_dc %.4f -> %.4f (%.1f%%) from %u observations, profile %u", (unsigned)zi,
             (double)k_dc, (double)k_blended, (double)z->last_delta_pct, (unsigned)z->ring_count,
             (unsigned)profile_id);
}

void adaptive_tune_run_end(const profile_firing_run_record_t *rec, bool clean)
{
    if (!rec) {
        return;
    }
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        adaptive_tune_zone_t *z = &s_zones[zi];
        if (!z->enabled) {
            continue;
        }
        const profile_firing_zone_record_t *zr = &rec->zones[zi];
        if (!zr->active) {
            continue;
        }
        if (!clean) {
            set_refusal(z, "run was faulted or stopped early -- not used as training data");
            continue;
        }
        uint32_t total = zr->stats.sample_count + zr->stats.excluded_sample_count;
        if (total > 0 && (float)zr->stats.excluded_sample_count / (float)total > ADAPTIVE_TUNE_MAX_EXCLUDED_FRACTION) {
            set_refusal(z, "run excluded %u/%u samples (>%.0f%%) -- not used as training data",
                        (unsigned)zr->stats.excluded_sample_count, (unsigned)total,
                        (double)(ADAPTIVE_TUNE_MAX_EXCLUDED_FRACTION * 100.0f));
            continue;
        }
        try_refine_zone_locked(zi, rec->profile_id);
    }
    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------------
// Opt-in flag: own NVS namespace, flash-worker-routed write.
// ---------------------------------------------------------------------

typedef struct {
    uint8_t mask;
    esp_err_t result;
} enmask_job_t;

static void save_enmask_job(void *arg)
{
    enmask_job_t *job = (enmask_job_t *)arg;
    nvs_handle_t h;
    esp_err_t err =
        nvs_open_from_partition(ADAPTIVE_TUNE_NVS_PARTITION, ADAPTIVE_TUNE_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        job->result = err;
        return;
    }
    err = nvs_set_u8(h, ADAPTIVE_TUNE_NVS_KEY_ENMASK, job->mask);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    job->result = err;
}

static uint8_t enmask_locked(void)
{
    uint8_t mask = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (s_zones[zi].enabled) {
            mask |= (uint8_t)(1u << zi);
        }
    }
    return mask;
}

bool adaptive_tune_set_enabled(uint8_t zone_index, bool enabled)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_zones[zone_index].enabled = enabled;
    enmask_job_t job = {.mask = enmask_locked(), .result = ESP_FAIL};
    xSemaphoreGive(s_lock);

    // Dispatched OUTSIDE the lock -- uart_bridge_ext_run_on_flash_worker()
    // blocks the calling task until the worker task runs the job (see that
    // function's own doc comment), and this file's lock must not be held
    // across a wait on a different task.
    esp_err_t err = uart_bridge_ext_run_on_flash_worker(save_enmask_job, &job);
    if (err != ESP_OK || job.result != ESP_OK) {
        ESP_LOGE(TAG, "adaptive_tune_set_enabled(%u,%d): NVS save failed: %s / %s", (unsigned)zone_index,
                 (int)enabled, esp_err_to_name(err), esp_err_to_name(job.result));
        return false; // live flag still stands -- see time_sync_set_tz()'s identical convention
    }
    return true;
}

bool adaptive_tune_get_enabled(uint8_t zone_index)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool en = s_zones[zone_index].enabled;
    xSemaphoreGive(s_lock);
    return en;
}

void adaptive_tune_get_status(uint8_t zone_index, adaptive_tune_zone_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return;
    }
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    adaptive_tune_zone_t *z = &s_zones[zone_index];
    out->enabled = z->enabled;
    out->ring_count = z->ring_count;
    out->observations_lifetime = z->observations_lifetime;
    out->has_applied = z->has_applied;
    out->prior_k_dc = z->prior_k_dc;
    out->applied_k_dc = z->applied_k_dc;
    out->last_delta_pct = z->last_delta_pct;
    out->last_applied_profile_id = z->last_applied_profile_id;
    out->last_applied_unix_s = z->last_applied_unix_s;
    strncpy(out->last_refusal_reason, z->last_refusal_reason, sizeof(out->last_refusal_reason) - 1);
    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------------
// HTTP: GET /api/adaptive_tune (status, all zones), POST
// /api/adaptive_tune/enable (form body "zone=<n>&enabled=<0|1>").
// ---------------------------------------------------------------------

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char buf[96 * MAX31856_CHANNEL_COUNT + 64];
    size_t off = 0;
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "{\"zones\":[");
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        adaptive_tune_zone_status_t st;
        adaptive_tune_get_status(zi, &st);
        off += (size_t)snprintf(
            buf + off, sizeof(buf) - off,
            "%s{\"zone\":%u,\"enabled\":%s,\"observation_count\":%u,\"has_applied\":%s,"
            "\"prior_k_dc\":%.4f,\"applied_k_dc\":%.4f,\"delta_pct\":%.2f,\"last_profile_id\":%u,"
            "\"last_applied_unix_s\":%u,\"refusal\":\"%s\"}",
            zi == 0 ? "" : ",", (unsigned)zi, st.enabled ? "true" : "false", (unsigned)st.ring_count,
            st.has_applied ? "true" : "false", (double)st.prior_k_dc, (double)st.applied_k_dc,
            (double)st.last_delta_pct, (unsigned)st.last_applied_profile_id, (unsigned)st.last_applied_unix_s,
            st.last_refusal_reason);
        if (off >= sizeof(buf)) {
            off = sizeof(buf) - 1; // truncated -- still a syntactically-recoverable prefix is not guaranteed,
                                    // but MAX31856_CHANNEL_COUNT is small (<=5) and the buffer sized generously
        }
    }
    snprintf(buf + off, sizeof(buf) - off, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

#define ADAPTIVE_TUNE_ENABLE_BODY_MAX 64

static esp_err_t enable_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > ADAPTIVE_TUNE_ENABLE_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[ADAPTIVE_TUNE_ENABLE_BODY_MAX + 1];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char zone_val[8], en_val[8];
    int zone_len = http_form_find_field(body, "zone", zone_val, sizeof(zone_val));
    int en_len = http_form_find_field(body, "enabled", en_val, sizeof(en_val));
    if (zone_len <= 0 || en_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zone and enabled fields required");
        return ESP_OK;
    }
    int zone = atoi(zone_val);
    bool enabled = (atoi(en_val) != 0);
    if (zone < 0 || zone >= MAX31856_CHANNEL_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zone out of range");
        return ESP_OK;
    }

    bool saved = adaptive_tune_set_enabled((uint8_t)zone, enabled);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, saved ? "{\"ok\":true}" : "{\"ok\":true,\"warning\":\"applied live, save failed\"}");
}

void adaptive_tune_init(void)
{
    ensure_lock();
    memset(s_zones, 0, sizeof(s_zones));

    // Boot-time read, direct (not through the flash worker -- see this
    // file's top comment on why reads are exempt).
    nvs_handle_t h;
    esp_err_t err =
        nvs_open_from_partition(ADAPTIVE_TUNE_NVS_PARTITION, ADAPTIVE_TUNE_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        uint8_t mask = 0;
        if (nvs_get_u8(h, ADAPTIVE_TUNE_NVS_KEY_ENMASK, &mask) == ESP_OK) {
            for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
                s_zones[zi].enabled = (mask & (1u << zi)) != 0;
            }
        }
        nvs_close(h);
    }
    // ESP_ERR_NVS_NOT_FOUND (namespace never written) leaves every zone at
    // its struct-zero default: enabled = false. DEFAULT OFF, as required.

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGW(TAG, "no HTTP server yet -- adaptive tune status/enable endpoints not registered");
        return;
    }
    static const httpd_uri_t status_uri = {
        .uri = "/api/adaptive_tune", .method = HTTP_GET, .handler = status_get_handler,
    };
    static const httpd_uri_t enable_uri = {
        .uri = "/api/adaptive_tune/enable", .method = HTTP_POST, .handler = enable_post_handler,
    };
    esp_err_t reg_err = httpd_register_uri_handler(server, &status_uri);
    if (reg_err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/adaptive_tune) failed: %s", esp_err_to_name(reg_err));
    }
    reg_err = httpd_register_uri_handler(server, &enable_uri);
    if (reg_err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/adaptive_tune/enable) failed: %s", esp_err_to_name(reg_err));
    }
}
