#ifndef ZONES_HTTP_INTERNAL_H
#define ZONES_HTTP_INTERNAL_H

/* Internal seams for the zones_http.c split (2026-09-01, "files over 1500
 * lines should be broken up where it makes sense" -- zones_http.c had grown
 * to 5628 lines, the largest file in the firmware). This header is NOT
 * public API -- zones_http.h stays that -- it exists purely so pieces that
 * used to be one translation unit (and could reach each other's `static`
 * state and helpers for free) can still do so now that they are six:
 *
 *   zones_http.c                 -- hw wiring, page/registration, sweep HTTP
 *   zones_config_store.c         -- NVS load/save, relay_names, zone_normals,
 *                                    CT-map/K_CT storage (state OWNER)
 *   zones_config_accessors.c     -- zones_config_get_.../set_...() public API
 *   zones_http_get.c             -- GET /api/zones + the two static pages
 *   zones_http_post_parse.c      -- per-zone POST field parse/validate
 *   zones_http_post.c            -- POST /api/zones (whole-page submit)
 *   zones_http_pid.c             -- POST /api/zones/pid (narrow PID-only)
 *                                    (zones_http_get.c/_post_parse.c/_post.c/
 *                                    _pid.c were one file, zones_http_
 *                                    handlers.c, until it was itself split
 *                                    2026-09-04, ROADMAP.md M15's 1500-line
 *                                    item -- see the section below.)
 *   zones_current_sweep_engine.c -- Task 1 sweep mechanics (derive/refuse/
 *                                    ceiling/run-one-zone/hw callbacks)
 *   zones_current_sweep_task.c   -- the sweep task driver + CT/K_CT commit +
 *                                    Task 2/3 public API
 *
 * Every symbol declared below was `static` in the original single file and
 * is widened to file-scope-internal linkage ONLY because a sibling .c file
 * in this split now calls or reads it directly -- see each new file's own
 * top-of-file comment for which of these it defines vs. only consumes, and
 * the split's commit message for the full "every symbol whose linkage
 * changed, and why" accounting.
 *
 * zones_state_t/zones_relay_names_state_t exist only so s_zones/s_relay_names
 * can be `extern`-declared here -- the original anonymous
 * `static struct { ... } s_zones;` has no name a header could reference. */

#include "zones_config_accessors.h"
#include "zones_config_json.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kiln_io_owner.h" /* kiln_io_owner_relay_result_t, used by zone_sweep_zone_deps_t::energize */

/* ---- shared log tag ----------------------------------------------------
 * Was `static const char *TAG = "zones_http";`, independently duplicated in
 * every section, before this split. That still works for a normal
 * separate-TU build (each file's `static` is its own), but
 * test_zones_http.c reaches this module's `static` internals
 * (parse_zone_fields() etc.) by #including every split .c file into ONE
 * translation unit -- see that test's header comment -- and six independent
 * `static const char *TAG = ...` definitions in one TU is a redefinition
 * error there. Defined once, non-static, in zones_http.c; every other file
 * only declares it.
 *
 * Named `ZONES_HTTP_TAG`, not plain `TAG` (renamed 2026-09-04, same hygiene
 * as commit 42288a2): every other internal header in this split family uses
 * a prefixed tag (`AT_TAG`, `OTA_HTTP_TAG`, `WIFI_PROV_TAG`, `ZONES_CFG_TAG`)
 * precisely because a global `TAG` clashes the moment another split's own
 * globalized tag ends up in the same link -- an unprefixed one here was the
 * lone holdout. */
extern const char *ZONES_HTTP_TAG;

/* Moved here (unchanged) from the top of the original zones_http.c --
 * zones_config_store.c (which now owns nvs_partition_init()/nvs_load()/
 * nvs_save()/relay_names_*()/zone_normals_*()) needs these, not just
 * zones_http.c's own zones_http_start(). */
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_ZONES "zones_cfg"

/* kiln_nvs is the 2026-08-13 split target for zones/rules/relay_cycles/
 * run_state (see partitions.csv and TODO.md 8.1); each module manages its own
 * migration and partition init independently rather than assuming another
 * module already brought the partition up. NVS_DEFAULT_PART_NAME (from
 * nvs_flash.h, expands to "nvs") is the old, still-live home this module's
 * data used to persist to, kept readable for the one-time migration below and
 * for firmware rollback. */
#define KILN_NVS_PARTITION "kiln_nvs"

/* application/x-www-form-urlencoded whole-page submit: thermo_count,
 * relay_count, and 27 fields per zone (name/relay_mask/thermo_mask/cal/kp/
 * ki/kd/ramp/sanity/mode/maxtemp/mintemp/window/minon/minoff/xzone/k/tau/
 * deadtime plus the 8 guard-threshold overrides below) across up to
 * MAX31856_CHANNEL_COUNT zones. Generous headroom over what a legitimate
 * 3-zone submission needs -- checked against Content-Length before a single
 * byte is read, same discipline as every other handler in this codebase.
 * Bumped 2048->2560 when heater_window_ms/min_on_ms/min_off_ms were added,
 * 2560->2816 when cross_zone_max_delta_c was, 2816->3200 when the three
 * plant-model fields were, 3200->4096 when the 8 guard-threshold overrides
 * were: worst case those add 8 "z0_<name>=" keys plus separators and up to
 * zones_config_json_parse_float_field()'s 23-char value each, ~250 bytes a zone, ~750 across
 * three. z%u_thermo_mask (TODO.md 10.8) is a single 0-255 u8 field, well
 * under 20 bytes a zone even with its key name -- left inside the existing
 * 4096 without another bump; the three-zone worst case is nowhere near it.
 * z%u_tctype (2026-08-21) and the top-level safety_tc_type are each a
 * single 0-7 u8 field, smaller still -- also left inside the existing 4096.
 * z%u_ct_mask (2026-08-27) is the same shape as z%u_thermo_mask, similarly
 * left inside 4096. */
#define ZONES_BODY_MAX 4096

/* ---- shared config state (owned by zones_config_store.c) -------------- */

typedef struct {
    zones_cfg_t cfg;
} zones_state_t;
extern zones_state_t s_zones;

/* Was defined locally in the relay-names section of the original
 * zones_http.c, right above `static struct { relay_names_cfg_t cfg; }
 * s_relay_names;` -- moved up here (unchanged) because
 * zones_config_accessors.c's zones_config_get_relay_name()/set_relay_name()
 * and zones_http_handlers.c's zones_get_handler()/zones_post_handler() all
 * need the type too, not just zones_config_store.c which owns the storage. */
#define RELAY_NAMES_CFG_VERSION 1
#define NVS_KEY_RELAY_NAMES "relay_names_cfg"

typedef struct {
    uint8_t version;
    char names[KILN_IO_RELAY_COUNT][RELAY_NAME_MAX_LEN + 1];
    uint32_t crc32;
} relay_names_cfg_t;

typedef struct {
    relay_names_cfg_t cfg;
} zones_relay_names_state_t;
extern zones_relay_names_state_t s_relay_names;

extern bool s_zones_config_valid;
extern uint32_t s_config_generation;

/* ---- shared hardware handles (owned by zones_http.c) ------------------- */

extern kiln_io_t *s_hw_io;
extern MAX31856BusClass *s_hw_thermo_bus;
extern SafetyLinkClass *s_hw_safety;

/* ---- zones_config_store.c: persistence + CT/K_CT/relay-name state ------ */

esp_err_t nvs_partition_init(const char *partition);
esp_err_t nvs_load(bool *out_found, bool *out_valid);
esp_err_t nvs_save(void);
void migrate_from_default_partition(void);

void relay_names_load(void);
esp_err_t relay_names_save(void);

void zone_normals_load(void);
bool zone_normals_set(uint8_t zone_index, float amps);

void zone_ct_map_clear(void);
bool zone_ct_map_set(uint8_t ct_channel, uint8_t zone_index);
void zone_k_ct_clear(void);
bool zone_k_ct_set(uint8_t ct_channel, float k_v_per_a);

uint8_t zone_owned_relay_mask(const zones_cfg_t *cfg);

/* ---- zones_http_get.c / zones_http_post_parse.c / zones_http_post.c /
 * zones_http_pid.c (the former zones_http_handlers.c, split 2026-09-04,
 * ROADMAP.md M15's 1500-line item -- zones_http_handlers.c had grown to
 * 1598 lines):
 *
 *   zones_http_get.c        -- page_get_handler/safety_config_page_get_
 *                               handler/zones_get_handler (GET side) plus
 *                               zones_json_escape(), shared with zones_http.c's
 *                               sweep-status handler (its only other caller).
 *   zones_http_post_parse.c -- zones_http_parse_zone_fields() (was `static
 *                               parse_zone_fields`; renamed with this split's
 *                               prefix, no collision found, per this repo's
 *                               "rename every symbol widened out of `static`
 *                               regardless" rule), the per-zone POST field
 *                               validator zones_http_post.c calls.
 *   zones_http_post.c       -- zones_post_handler(), POST /api/zones.
 *   zones_http_pid.c        -- zones_pid_post_handler(), POST /api/zones/pid.
 *
 * All four still register by function pointer from zones_http_start() in
 * zones_http.c, unchanged. */
void zones_json_escape(const char *src, char *out, size_t out_cap);
esp_err_t page_get_handler(httpd_req_t *req);
esp_err_t safety_config_page_get_handler(httpd_req_t *req);
esp_err_t zones_get_handler(httpd_req_t *req);
bool zones_http_parse_zone_fields(const char *body, uint8_t i, uint8_t thermo_count, uint8_t relay_count,
                                  uint8_t timing_profile_count, const zone_cfg_t *current_z, zone_cfg_t *z,
                                  const char **err_reason);
esp_err_t zones_post_handler(httpd_req_t *req);
esp_err_t zones_pid_post_handler(httpd_req_t *req);

/* ---- shared between zones_current_sweep_engine.c and
 * zones_current_sweep_task.c: the sweep context, the per-zone dependency-
 * injection types, and the mechanics functions the task driver calls. ---- */

/* LOW (opus review, 2026-08-27): every field below is written by the sweep
 * task on one core and read by the HTTP task (zones_current_sweep_get_status()/
 * zones_current_sweep_start()'s `.active` check) on the other -- see the
 * struct's original doc comment (now here) for why `volatile`, not a lock,
 * is the right tool for each field. */
typedef struct {
    volatile bool               active;   /* a sweep task is currently running */
    volatile bool               abort_requested;
    volatile zone_sweep_state_t state;
    volatile uint8_t            zone_index;
    volatile uint8_t            zones_done;
    volatile uint8_t            zones_total;
    volatile char                reason[64];
    volatile uint8_t             ct_map_derived_mask;
    volatile char                ct_map_reason[96];
    volatile uint8_t             k_ct_derived_mask;
    volatile char                k_ct_reason[96];
    TaskHandle_t                 task;
} zone_sweep_ctx_t;
extern zone_sweep_ctx_t s_sweep;

typedef struct {
    kiln_io_owner_relay_result_t (*energize)(void *ctx, uint8_t relay_mask, uint32_t *out_safety_sources);
    void (*force_off)(void *ctx);
    void (*read_temp)(void *ctx, uint8_t zi, float *out_c, bool *out_valid);
    float (*sample_current)(void *ctx, uint8_t zi);
    void (*sample_channels)(void *ctx, float *out_a); /* ZONE_CT_CHANNEL_COUNT entries */
    bool (*link_up)(void *ctx);
    bool (*trip_latched)(void *ctx);
    bool (*abort_requested)(void *ctx);
    void (*delay_poll)(void *ctx);
    void *ctx;
} zone_sweep_zone_deps_t;

typedef struct {
    uint8_t (*relay_mask_for_zone)(void *ctx, uint8_t zi);
    void (*set_zone_index)(void *ctx, uint8_t zi);
    void (*record_normal)(void *ctx, uint8_t zi, float avg_a);
    void (*record_ct_channels)(void *ctx, uint8_t zi, uint8_t relay_mask, const float *per_ch_avg_a);
    void (*zone_done)(void *ctx);
    void *ctx;
} zone_sweep_all_hooks_t;

typedef struct {
    zone_sweep_state_t state; /* ZONE_SWEEP_DONE / _ABORTED / _FAILED */
    char               reason[64];
    uint8_t            zones_done;
} zone_sweep_all_result_t;

typedef enum {
    ZONE_KCT_DERIVE_OK = 0,
    ZONE_KCT_DERIVE_NO_NAMEPLATE,
    ZONE_KCT_DERIVE_NO_MEASUREMENT,
    ZONE_KCT_DERIVE_NO_PRIOR_K,
    ZONE_KCT_DERIVE_IMPLAUSIBLE,
} zone_kct_derive_t;

/* zones_current_sweep_engine.c defines these; zones_current_sweep_task.c
 * (zone_sweep_task() building hw_deps/hw_hooks, and
 * zones_current_sweep_start()/zone_sweep_task_record_ct_channels()/
 * zone_sweep_plan_k_ct()) calls them. */
bool zone_sweep_derive_ct_channel(const float *per_ch_a, uint8_t *out_ch);
zone_kct_derive_t zone_sweep_derive_k_ct(float measured_total_a, float expected_power_w, float mains_voltage_v,
                                         float k_old, float *out_k);
const char *zone_kct_derive_str(zone_kct_derive_t r);
zone_sweep_refusal_t zone_sweep_check_refusal(bool already_running, bool have_hw, bool config_valid,
                                              uint8_t thermo_count, bool profile_running_or_paused,
                                              bool autotune_active, bool link_up, bool trip_latched,
                                              bool relays_on);
void zone_sweep_force_relays_off(void);
void zone_sweep_run_all_zones(uint8_t zones_total, const zone_sweep_zone_deps_t *deps,
                              const zone_sweep_all_hooks_t *hooks, zone_sweep_all_result_t *out);

/* Real hardware bindings for zone_sweep_zone_deps_t -- defined in
 * zones_current_sweep_engine.c, assigned by name into zone_sweep_task()'s
 * hw_deps in zones_current_sweep_task.c. Host tests supply their own fakes
 * instead and never link these. */
kiln_io_owner_relay_result_t zone_sweep_hw_energize(void *ctx, uint8_t relay_mask, uint32_t *out_safety_sources);
void zone_sweep_hw_force_off(void *ctx);
void zone_sweep_hw_read_temp(void *ctx, uint8_t zi, float *out_c, bool *out_valid);
float zone_sweep_hw_sample_current(void *ctx, uint8_t zi);
void zone_sweep_hw_sample_channels(void *ctx, float *out_a);
bool zone_sweep_hw_link_up(void *ctx);
bool zone_sweep_hw_trip_latched(void *ctx);
bool zone_sweep_hw_abort_requested(void *ctx);
void zone_sweep_hw_delay_poll(void *ctx);

#endif /* ZONES_HTTP_INTERNAL_H */
