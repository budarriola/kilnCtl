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

#include "pref_cfg_fs.h" /* PREF_CFG_FS_MAX_ITEM -- zone_normals_cfg_t size assert below */
#include "kiln_io_owner.h" /* kiln_io_owner_relay_result_t, used by zone_sweep_zone_deps_t::energize */

/* docs/ARCHITECTURE_DECISIONS.md#zones-page-clean-up-info-disclosure-schema-v20-v21-chartjs (ZONES_CFG_VERSION 20->21): the URL-key suffix
 * for each of the SRC_GROUP_COUNT independent "same as zone N" groups --
 * z%u_settings_source_<SRC_GROUP_NAMES[g]> -- shared between
 * zones_http_post_parse.c (parses it) and zones_http_get.c (emits the
 * matching JSON key per group in its GET response), so the two can never
 * silently disagree on spelling. Defined once, in zones_http_post_parse.c. */
extern const char *const SRC_GROUP_NAMES[SRC_GROUP_COUNT];

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
#include "nvs_key_check.h" /* NVS_KEY_LEN_CHECK -- see that header; namespace is
                             * subject to the same 15-usable-char NVS limit as
                             * a key. */
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);

/* kiln_nvs is the 2026-08-13 split target for zones/rules/relay_cycles/
 * run_state (see partitions.csv and TODO.md 8.1); each module manages its own
 * migration and partition init independently rather than assuming another
 * module already brought the partition up. NVS_DEFAULT_PART_NAME (from
 * nvs_flash.h, expands to "nvs") is the old, still-live home this module's
 * data used to persist to, kept readable for the one-time migration below and
 * for firmware rollback. */
#define KILN_NVS_PARTITION "kiln_nvs"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);

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
/* 1 -> 2 (docs/ZONE_GRAPHIC_PLAN.md stage 1, 2026-09-18): added types[], the
 * per-relay device type. This is the FIRST version bump this blob has ever
 * had, so zones_config_store.c grew its first migration at the same time --
 * see relay_names_decode_any() there. Bumping this without one would have
 * made relay_names_validate() reject every existing v1 blob and blank every
 * operator-entered relay name on the next firmware update, silently.
 *
 * NOT to be confused with ZONES_CFG_VERSION (zones_config_json.h, the
 * per-zone on-flash schema carrying the PID gains) or SaftyFW's
 * CONFIG_STORE_FORMAT_VERSION -- three separate schemas, three separate
 * constants, on two processors. This one is deliberately independent so
 * cosmetic per-relay labels do not share the PID gains' rollback fate. */
#define RELAY_NAMES_CFG_VERSION 2
#define NVS_KEY_RELAY_NAMES "relay_names_cfg"
NVS_KEY_LEN_CHECK(NVS_KEY_RELAY_NAMES);

/* docs/FILESYSTEM_USER_DATA_PLAN.md item 3 (relay names), section 5 step 5
 * close-out: separate tiny NVS key for the dual-write rev counter, same
 * reasoning as NVS_KEY_ZONES_REV in zones_config_store.c -- a rev counter has
 * nothing to do with the operator-entered labels themselves, so it is not a
 * field on relay_names_cfg_t. */
#define NVS_KEY_RELAY_NAMES_REV "relnames_rev"
NVS_KEY_LEN_CHECK(NVS_KEY_RELAY_NAMES_REV);

/* The `cfg` LittleFS file relay names dual-writes to, via the generic
 * pref_cfg_fs.h bridge (see zones_config_store.c's relay_names_load()/
 * relay_names_save()). */
#define RELAY_NAMES_FILE_PATH "relay_names.dat"

/* FROZEN v1 layout -- the shape that is on every board in the field today.
 * Never edit this struct: it is not "the old version of relay_names_cfg_t",
 * it is a permanent description of bytes already written to flash, and the
 * only thing that can read them correctly. Same discipline zones_cfg_vN_t
 * follows, and the reason this codebase writes migrations against a named
 * frozen type instead of memcpy'ing one shape over another. */
typedef struct {
    uint8_t version;
    char names[KILN_IO_RELAY_COUNT][RELAY_NAME_MAX_LEN + 1];
    uint32_t crc32;
} relay_names_cfg_v1_t;

typedef struct {
    uint8_t version;
    char names[KILN_IO_RELAY_COUNT][RELAY_NAME_MAX_LEN + 1];
    /* v2: what each relay drives -- relay_device_type_t values, stored one
     * byte each. Index r is relay r+1, the same dense 1-based mapping
     * names[] uses. RELAY_DEVICE_TYPE_UNSET (0) is what a migrated v1 board
     * and a never-configured relay both read as, deliberately. */
    uint8_t types[KILN_IO_RELAY_COUNT];
    uint32_t crc32;
} relay_names_cfg_t;

/* The migration in zones_config_store.c tells v1 from v2 by BYTE LENGTH
 * before it interprets a single field (the same length-before-interpretation
 * rule zones_config_json_decode_blob() uses), so these two sizes differing is
 * load-bearing, not incidental. If a future field ever made them equal, the
 * length check would silently accept a v1 blob as v2 and read types[] out of
 * the old blob's padding -- pin it here rather than discovering it on a
 * board. */
_Static_assert(sizeof(relay_names_cfg_v1_t) != sizeof(relay_names_cfg_t),
               "relay_names v1 and v2 must differ in size -- the migration distinguishes them by length");

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

/* RELAY_LIFE_BUDGET.md: pushes s_zones.cfg.zones[zone_index]'s
 * relay_type out to relay_cycles_set_type() for every relay named in that
 * zone's relay_mask (rated_override left at 0 -- use the type's table value;
 * there is no per-relay override UI yet). Defined in zones_config_store.c
 * (the file that already owns s_zones and includes relay_cycles.h);
 * zones_config_accessors.c's zones_config_set_relay_type() calls this after
 * a successful nvs_save(), and zones_config_store.c's own nvs_load()/
 * nvs_load_from() success path calls it for every zone right after loading.
 * A no-op for zone_index >= MAX31856_CHANNEL_COUNT. */
void zones_config_push_relay_type(uint8_t zone_index);

/* Calls zones_config_push_relay_type() for every zone 0..MAX31856_CHANNEL_
 * COUNT-1 -- the whole-config sweep nvs_load()/nvs_load_from() run once
 * right after a successful load, since a fresh boot has no per-zone "just
 * changed" edge to key a narrower push off. */
void zones_config_push_all_relay_types(void);
void migrate_from_default_partition(void);

void relay_names_load(void);
esp_err_t relay_names_save(void);


/* Zone normals (docs/CONFIG_FILESYSTEM.md item 2) -- shared with the host tests. See
 * zones_config_store.c for the version history and the NVS-key rename story. */
#define ZONE_NORMALS_CFG_VERSION 3
#define NVS_KEY_ZONE_NORMALS "zone_norm_cfg"
NVS_KEY_LEN_CHECK(NVS_KEY_ZONE_NORMALS);
/* dual-write rev counter, its own key like NVS_KEY_RELAY_NAMES_REV */
#define NVS_KEY_ZONE_NORMALS_REV "znorm_rev"
NVS_KEY_LEN_CHECK(NVS_KEY_ZONE_NORMALS_REV);
/* the `cfg` LittleFS file, via the generic pref_cfg_fs.h bridge */
#define ZONE_NORMALS_FILE_PATH "zone_normals.dat"

typedef struct {
    uint8_t  version;
    uint8_t  measured_mask; /* bit i = zone i has a measured normal current */
    float    normal_current_a[MAX31856_CHANNEL_COUNT];
    /* v2: the derived CT-channel -> zone mapping. bit c of
     * ct_map_derived_mask set means ct_map_zone[c] is a zone index the sweep
     * derived UNAMBIGUOUSLY (COMMISSIONING_UX.md sec 1.2's condition); a
     * clear bit means "never derived", and ct_map_zone[c] is meaningless. */
    uint8_t  ct_map_derived_mask;
    uint8_t  ct_map_zone[ZONE_CT_CHANNEL_COUNT];
    /* v3: the derived CT volts-per-amp scale. bit c of k_ct_derived_mask set
     * means k_ct_v_per_a[c] is a value this board CALIBRATED from a complete
     * sweep and confirmed written to the safety processor; a clear bit means
     * "never derived here" and k_ct_v_per_a[c] is meaningless -- it says
     * nothing about whether the Pico's own k_ct_v_per_a[c] is set, which an
     * operator may always have entered by hand. */
    uint8_t  k_ct_derived_mask;
    float    k_ct_v_per_a[ZONE_CT_CHANNEL_COUNT];
    uint32_t crc32;
} zone_normals_cfg_t;
_Static_assert(sizeof(zone_normals_cfg_t) <= PREF_CFG_FS_MAX_ITEM,
               "zone_normals_cfg_t must fit pref_cfg_fs's per-item cap (raise PREF_CFG_FS_MAX_ITEM, don't truncate)");

void zone_normals_load(void);
bool zone_normals_set(uint8_t zone_index, float amps);
bool zone_normals_invalidate_mask(uint8_t zone_mask);

/* Read-only dual-write status for GET /api/cfgfs's "zone_normals" row -- same
 * contract as profiles_builtin_get_dualwrite_status() (fresh re-read of both
 * sides, never a resync write, safe to poll). */
void zone_normals_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                       bool *diverged);

/* Read-only dual-write status for GET /api/cfgfs's "relay_names" row (same
 * contract as zone_normals_get_dualwrite_status()). */
void relay_names_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                      bool *diverged);

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
/* GET /api/zones_diag (docs/audits/zones_diag_endpoint_split_2026-09-14.md) --
 * the diagnostics-only per-zone fields split out of zones_get_handler() once
 * GET /api/zones ran low on json_cap headroom; also defined in
 * zones_http_get.c, registered from zones_http_start() alongside get_uri. */
esp_err_t zones_diag_get_handler(httpd_req_t *req);
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
    /* Feature: nameplate-derived S14/S15 arming. Mirrors k_ct_derived_mask/
     * k_ct_reason above, one push behind: after zone_sweep_push_k_ct_v_per_a()
     * (which calibrates the CT SCALE), zone_sweep_push_i_normal_a() pushes
     * every zone's already-measured, already-persisted normal current
     * (zones_config_get_normal_current(), zone_normals_set()'s own store) to
     * the safety processor's i_normal_a[0..2] (0x031A-0x031C) -- the single
     * missing link that leaves S14 (per-channel over-current) and S15
     * (per-zone under-current, summed topology) dormant even after a
     * successful sweep: those guards read cfg->i_normal_a on the Pico, and
     * nothing before this pushed the ESP's measured value there. See
     * zone_sweep_plan_i_normal()'s doc comment for what "already measured"
     * means in each CT topology. */
    volatile uint8_t             i_normal_pushed_mask;
    volatile char                i_normal_reason[96];
    /* Owner feature (2026-09-10): "set the nameplate value, find the normal
     * current on first heat, cause a fault if much higher or lower than
     * expected." This is a SEPARATE, ESP-side-only advisory check from
     * i_normal_pushed_mask above -- that push arms the Pico's own S14/S15
     * guards against the MEASURED value as their own baseline; this one
     * compares the SAME measurement against what the operator's nameplate
     * (whole-kiln sum, or a per-coil override -- zone_cfg_t::coil_power_w)
     * IMPLIES the current should be, at commissioning time. Deliberately
     * NOT wired to any Pico safety trip -- see
     * zone_sweep_check_expected_current()'s own doc comment for why arming
     * a hard fault here is refused on this bench specifically. Bit zi set
     * in nameplate_mismatch_mask means zone zi's measured normal current
     * disagreed with its nameplate-implied expectation by more than
     * zone_sweep_check_expected_current()'s plausibility band; 0 means
     * every checked zone was plausible (or nothing was checked --
     * nameplate_reason says which). */
    volatile uint8_t             nameplate_mismatch_mask;
    volatile char                nameplate_reason[96];
    /* opus review finding (MEDIUM): see zone_sweep_status_t's identically-
     * named field in zones_config_accessors.h. */
    volatile uint8_t             summed_unmeasured_mask;
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

/* ---- CT attribution verification (docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md)
 * Defined in zones_current_sweep_engine.c. Pure: every input is a plain number
 * the caller gathers, so the whole verdict is host-testable off-target and so
 * the fitted question keeps its single owner (config_store_ct_channel_fitted()
 * on SaftyFW) instead of gaining a second implementation here. */
typedef enum {
    ZONE_CT_VERDICT_INCONCLUSIVE = 0, /* initial value, and the only one reachable without a measurement */
    ZONE_CT_VERDICT_PASS,
    ZONE_CT_VERDICT_FAIL,
} zone_ct_verdict_t;

typedef enum {
    ZONE_CT_VERIFY_OK = 0,
    ZONE_CT_VERIFY_NO_INPUT,
    ZONE_CT_VERIFY_NOT_FITTED,
    ZONE_CT_VERIFY_RATIO_NOT_ENTERED,
    ZONE_CT_VERIFY_SHARED_CHANNEL,
    ZONE_CT_VERIFY_NO_NORMAL_CURRENT,
    ZONE_CT_VERIFY_NO_DOMINANT_CHANNEL,
    ZONE_CT_VERIFY_BELOW_THRESHOLD,
    ZONE_CT_VERIFY_WRONG_CHANNEL,
    ZONE_CT_VERIFY_CONFLICT,
} zone_ct_verify_reason_t;

typedef struct {
    float per_ch_a[ZONE_CT_CHANNEL_COUNT]; /* averaged per-channel amps for this zone's window */
    uint8_t configured_ch;                 /* zone_ct_channel[z], read through its accessor */
    bool configured_ch_fitted;             /* config_store_ct_channel_fitted() -- caller asks, never this file */
    bool configured_ch_shared;             /* another zone maps to the same channel */
    bool conflict;                         /* another zone resolved to this channel though config says distinct */
    bool ratio_entered;                    /* safety_cfg_store's has_value for the channel */
    float live_k_ct_v_per_a;               /* committed; <= 0 or non-finite means never commissioned */
    float i_normal_a;                      /* this zone's recorded normal; <= 0 or non-finite means unrecorded */
} zone_ct_verify_in_t;

typedef struct {
    zone_ct_verdict_t verdict;
    zone_ct_verify_reason_t reason;
    uint8_t responded_ch; /* 0xFF when nothing resolved */
    float measured_a;     /* NaN when nothing resolved */
    float threshold_a;    /* NaN unless the threshold was actually reached in the decision */
} zone_ct_verify_out_t;

float zone_ct_verify_threshold_a(float live_k_ct_v_per_a, float i_normal_a);
zone_ct_verdict_t zone_sweep_verify_ct_attribution(const zone_ct_verify_in_t *in, zone_ct_verify_out_t *out);
const char *zone_ct_verdict_str(zone_ct_verdict_t v);
const char *zone_ct_verify_reason_str(zone_ct_verify_reason_t r);

typedef enum {
    ZONE_NAMEPLATE_CHECK_OK = 0,
    ZONE_NAMEPLATE_CHECK_NO_NAMEPLATE, /* no usable expected current -- nameplate/coil share/mains_v missing */
    ZONE_NAMEPLATE_CHECK_NO_MEASUREMENT, /* this zone has no measured normal current yet */
    ZONE_NAMEPLATE_CHECK_MISMATCH, /* measured current is implausible vs. the nameplate-implied expectation */
} zone_nameplate_check_t;

/* Pure decision, host-testable off-target -- see zones_current_sweep_engine.c
 * for the full derivation and why this reuses zone_sweep_derive_k_ct()'s own
 * ZONE_KCT_RATIO_MIN/MAX plausibility band rather than a new number.
 * zone_count (opus review finding 6): the divisor for the equal-split
 * default MUST be the number of zones (thermo_count), never a relay count
 * -- a zone's measured current is always one CT reading for the whole
 * zone (every relay in zone_cfg_t::relay_mask summed together), so the
 * comparison unit is per-zone regardless of how many relays one zone
 * drives. See the function's own doc comment for the full reasoning. */
float zone_sweep_expected_coil_current_a(float coil_power_w_override, float sum_power_w, uint8_t zone_count,
                                          float mains_voltage_v);
zone_nameplate_check_t zone_sweep_check_expected_current(float measured_a, float expected_a);
const char *zone_nameplate_check_str(zone_nameplate_check_t r);

/* zones_current_sweep_task.c defines this -- see its own doc comment. Exposed
 * here so host tests can drive the planning decision without a fake link. */
uint8_t zone_sweep_plan_i_normal(float *out_a, char *note, size_t note_cap);

/* CT_COMMISSIONING_PLAN.md step 3, `ct_topology = summed`: the shared CT
 * (channel 3, index 2) reads every zone, so a zone's own normal is not the
 * raw reading -- it is that reading minus whatever the same channel already
 * read with every zone off (`normal_a[zone] = sum(with zone on) - sum(idle)`,
 * the plan's own formula). Pure and host-testable, no I/O: `sum_idle_a` is
 * whatever the caller sampled once, before the per-zone loop, with every
 * relay off. Returns false (out untouched) if either input is non-finite,
 * OR if sum_with_zone_on_a < sum_idle_a (a channel that reads LOWER with
 * the zone on than idle is a wiring/noise artifact, not evidence of
 * negative current, and clamping it to a persisted zero would silently make
 * S14/S15 inert for that zone forever -- opus review finding, MEDIUM). The
 * caller must treat false as "not measured": leave the zone's bit clear in
 * measured_mask rather than persisting anything for it.
 *
 * `live_k_ct_v_per_a`: 2026-09-10 fix (opus review round 2, finding C) --
 * the shared channel's currently-committed k_ct_v_per_a (read the same way
 * zone_sweep_derive_k_ct()'s own `k_old` is, via zone_cfg_committed_f32();
 * 0.0f/non-finite for "never committed", which falls back to the reference
 * floor). The noise floor this function refuses below is a fixed AMPS
 * value derived at one specific k_ct; since counts->amps is inversely
 * proportional to k_ct, a channel committed to a different k_ct needs the
 * floor rescaled to still reject the same underlying ADC noise -- see
 * ZONE_SWEEP_NORMAL_NOISE_FLOOR_REF_K_CT's doc comment
 * (zones_current_sweep_engine.c) for the full derivation. */
bool zone_sweep_summed_normal_a(float sum_with_zone_on_a, float sum_idle_a, float live_k_ct_v_per_a,
                                float *out_normal_a);
zone_sweep_refusal_t zone_sweep_check_refusal(bool already_running, bool have_hw, bool config_valid,
                                              uint8_t thermo_count, bool profile_running_or_paused,
                                              bool autotune_active, bool link_up, bool trip_latched,
                                              bool relays_on, bool ct_topology_unknown);
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
