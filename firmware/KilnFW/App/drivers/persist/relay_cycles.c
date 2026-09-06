#include "relay_cycles.h"

#include <string.h>

#include "esp_log.h"
#include "hal_esp_common.h"
#include "hal_time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal_kv.h"

static const char *TAG = "relay_cycles";

/* Hand-declared rather than #include "uart_bridge.h" -- same reasoning as
 * safety_cfg_store.c's identical block: that header pulls in
 * ILI9488.h/screen_idle.h/kiln_io.h for hardware-bridge task declarations
 * this file needs none of, and which are not part of this module's
 * host-test stub surface. Keep in sync with uart_bridge.h by hand if either
 * signature ever changes. Used by relay_cycles_reset() (RELAY_LIFE_BUDGET_
 * PLAN.md step 5) to persist a reset immediately from whichever task calls
 * it -- the LCD diagnostics page's two-tap confirm runs on the LVGL task,
 * whose stack is PSRAM-backed (DRAM_PSRAM_PLAN.md), so it cannot call
 * persist_locked() directly any more than relay_cycles_flush() can. */
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg);
bool uart_bridge_ext_is_on_flash_worker(void);

/* Same namespace as the rest of this board's configuration (zones_http.c,
 * rules_http.c) but its own key -- deliberately NOT folded into the
 * zones_cfg blob, whose loader treats any size change as "corrupt, start
 * unconfigured". Adding a field there would silently wipe a user's zone
 * setup on the first boot after the update. */
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_CYCLES "relay_cyc"

/* TODO.md 8.1: this module's persisted store, split out of the default NVS
 * partition into its own partition so a corrupt/erased default partition
 * cannot take relay history with it. */
#define KILN_NVS_PARTITION "kiln_nvs"

/* The blob has no version field of its own on disk before this change (a
 * bare uint32_t[KILN_IO_RELAY_COUNT]); wrapping it in a versioned struct
 * changes the on-disk layout, which is fine here -- unlike run_state.c's
 * blob, this one is diagnostic-only and already treats any size mismatch as
 * "start at zero", so the version add rides the same tolerant path. */
/* Version 2 (RELAY_LIFE_BUDGET_PLAN.md step 1): adds a fifth counted slot
 * (RELAY_CYCLES_SAFETY_INDEX, the safety relay K4) and per-relay type +
 * rated-life override, both persisted so a budget survives reboot before the
 * zones/safety config steps that will actually set the type exist. A v1 blob
 * (bare 4-count array, no types) loads into the first four slots with type
 * defaulted to RELAY_TYPE_SSR and the fifth slot at 0 -- see the migration
 * block in relay_cycles_init(). */
#define RELAY_CYCLES_VERSION 2

typedef struct {
    uint8_t  version;
    uint32_t counts[KILN_IO_RELAY_COUNT];
} relay_cycles_blob_v1_t;

typedef struct {
    uint8_t  version;
    uint32_t counts[RELAY_CYCLES_COUNT];
    uint8_t  types[RELAY_CYCLES_COUNT];          /* relay_type_t, stored as uint8_t */
    uint32_t rated_overrides[RELAY_CYCLES_COUNT]; /* 0 = use the type's table value */
} relay_cycles_blob_t;

typedef struct {
    SemaphoreHandle_t lock;
    uint32_t          counts[RELAY_CYCLES_COUNT];
    uint8_t           types[RELAY_CYCLES_COUNT];
    uint32_t          rated_overrides[RELAY_CYCLES_COUNT];
    bool              dirty;
    int64_t           last_persist_us;
    bool              initialized;
} relay_cycles_t;

static relay_cycles_t s_rc;

/* Brings up KILN_NVS_PARTITION, erasing ONLY that partition if its contents
 * are unusable. hal_kv_init_partition() already implements the
 * erase-and-retry idiom this used to do by hand (see hal_kv_esp.c). */
static hal_status_t nvs_partition_init(const char *partition)
{
    return hal_kv_init_partition(partition);
}

/* Forward declaration -- defined below, but this write path needs it before
 * that point in the file. See its own definition for what it checks. */
static bool caller_stack_is_external(void);

/* One-time, one-directional copy of the old default-partition blob into
 * KILN_NVS_PARTITION, for boards provisioned by firmware predating the
 * split. The old copy is left in place (never deleted) so a rollback to
 * pre-split firmware still finds its counts -- see wifi_prov.c's
 * migrate_from_default_partition() for the fuller rationale. Only called
 * when KILN_NVS_PARTITION has nothing under NVS_KEY_CYCLES yet.
 *
 * DRAM_PSRAM_PLAN.md section 9 write-path re-audit (2026-09-02): this
 * function writes NVS (hal_kv_set_blob()/hal_kv_commit() below) exactly like
 * persist_locked() does further down, but never got persist_locked()'s
 * caller_stack_is_external() guard when that guard was added -- the earlier
 * pass treated "this file is guarded" as true of the file's main write path
 * and never re-checked every OTHER write call site inside it. In practice
 * this function is only ever reached from relay_cycles_init(), itself only
 * called once from app_main's own task (an internal-SRAM stack) before any
 * PSRAM-stacked task exists, so it cannot fire the crash today -- but that
 * makes it exactly the kind of gap a future audit would read as "covered"
 * without this guard, the same false-confidence class the third pass's
 * migrate_from_default_partition() finding described. Added for that reason,
 * not because a live path was found. */
static void migrate_from_default_partition(void)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, NULL);
    if (err != HAL_OK) {
        return;
    }
    /* This reads whatever the OLDEST possible sibling wrote to the default
     * partition before any board ever saw the partition split -- that could
     * only ever be a v1 blob (bare 4-count array), since the split predates
     * the v1->v2 type/fifth-slot change. Read it as v1 and upconvert, the
     * same way relay_cycles_init()'s own migration block does for a v1 blob
     * already in KILN_NVS_PARTITION. */
    relay_cycles_blob_v1_t old_blob_v1;
    size_t len = sizeof(old_blob_v1);
    err = hal_kv_get_blob(&h, NVS_KEY_CYCLES, &old_blob_v1, &len);
    hal_kv_close(&h);
    if (err != HAL_OK) {
        /* Nothing in the old location either (or it's the pre-version-field
         * bare uint32_t[] blob, a different size) -- nothing to migrate. */
        return;
    }
    if (len != sizeof(old_blob_v1) || old_blob_v1.version != 1) {
        return;
    }

    relay_cycles_blob_t old_blob;
    memset(&old_blob, 0, sizeof(old_blob));
    old_blob.version = RELAY_CYCLES_VERSION;
    memcpy(old_blob.counts, old_blob_v1.counts, sizeof(old_blob_v1.counts));
    /* types[]/rated_overrides[] stay zero -- RELAY_TYPE_SSR/no override,
     * same default the in-place v1->v2 migration uses. */

    if (caller_stack_is_external()) {
        ESP_LOGE(TAG, "migrate_from_default_partition: REFUSING -- calling task's stack is in "
                      "external RAM (PSRAM). See persist_locked()'s guard comment in this file "
                      "and DRAM_PSRAM_PLAN.md section 7.2/9.");
        return;
    }

    hal_kv_handle_t hw;
    err = hal_kv_open(&hw, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return;
    }
    err = hal_kv_set_blob(&hw, NVS_KEY_CYCLES, &old_blob, sizeof(old_blob));
    if (err == HAL_OK) {
        err = hal_kv_commit(&hw);
    }
    hal_kv_close(&hw);
    if (err == HAL_OK) {
        ESP_LOGI(TAG, "migrated relay cycle counts from default NVS partition to '%s'", KILN_NVS_PARTITION);
    } else {
        ESP_LOGW(TAG, "relay cycle count migration to '%s' failed: %s", KILN_NVS_PARTITION, hal_status_to_name(err));
    }
}

static bool ensure_lock(void)
{
    if (!s_rc.lock) {
        s_rc.lock = xSemaphoreCreateMutex();
        if (!s_rc.lock) {
            ESP_LOGE(TAG, "xSemaphoreCreateMutex failed -- cycle counts will not be kept");
            return false;
        }
    }
    return true;
}

/* True iff the CURRENTLY EXECUTING task's own stack lives in external RAM
 * (PSRAM) -- i.e. the negation of hal_kv_write_safe_here(). Same predicate,
 * same reasoning, and same incident class as kiln_cfg_store.c's/
 * safety_cfg_store.c's/run_state.c's caller_stack_is_external(): a flash/NVS
 * write disables the cache, which makes a PSRAM-resident stack unreachable
 * and aborts the whole board via ESP-IDF's own
 * esp_task_stack_is_sane_cache_disabled() rather than failing just this one
 * call. This module's persist_locked() is called directly from
 * profile_executor's tick path (relay_cycles_maybe_persist()) and its stop
 * path (relay_cycles_flush()) -- see run_state.c's identical guard for the
 * fuller rationale; this closes the same gap for this module. */
static bool caller_stack_is_external(void)
{
    return !hal_kv_write_safe_here();
}

static hal_status_t persist_locked(void)
{
    if (caller_stack_is_external()) {
        ESP_LOGE(TAG, "persist_locked: REFUSING -- calling task's stack is in external RAM "
                      "(PSRAM). A flash/NVS write from here would abort the whole board "
                      "(ESP-IDF's esp_task_stack_is_sane_cache_disabled()). Route this call "
                      "through a task with an internal-SRAM stack instead -- see "
                      "DRAM_PSRAM_PLAN.md section 7.2 and uart_bridge_ext.c's flash-safe "
                      "worker for the established pattern.");
        return HAL_NOT_READY;
    }
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return err;
    }
    relay_cycles_blob_t blob;
    blob.version = RELAY_CYCLES_VERSION;
    memcpy(blob.counts, s_rc.counts, sizeof(blob.counts));
    memcpy(blob.types, s_rc.types, sizeof(blob.types));
    memcpy(blob.rated_overrides, s_rc.rated_overrides, sizeof(blob.rated_overrides));
    err = hal_kv_set_blob(&h, NVS_KEY_CYCLES, &blob, sizeof(blob));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (err == HAL_OK) {
        s_rc.dirty = false;
        s_rc.last_persist_us = (int64_t)hal_time_now_us();
    }
    return err;
}

esp_err_t relay_cycles_init(void)
{
    if (!ensure_lock()) {
        return ESP_ERR_NO_MEM;
    }

    hal_status_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != HAL_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- relay cycle counts will not persist",
                 KILN_NVS_PARTITION, hal_status_to_name(part_err));
    }

    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    memset(s_rc.counts, 0, sizeof(s_rc.counts));
    memset(s_rc.types, RELAY_TYPE_SSR, sizeof(s_rc.types));
    memset(s_rc.rated_overrides, 0, sizeof(s_rc.rated_overrides));

    /* Migrate before the real load so a pre-split board's counts show up on
     * the very first boot after the update, not one boot late. */
    if (part_err == HAL_OK) {
        migrate_from_default_partition();
    }

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err == HAL_OK) {
        relay_cycles_blob_t blob;
        size_t len = sizeof(blob);
        err = hal_kv_get_blob(&h, NVS_KEY_CYCLES, &blob, &len);
        /* BUG FIXED (matching zones_http.c's nvs_load_from()): this used to
         * gate BOTH the current-version and newer-version branches on
         * `len == sizeof(blob)` before ever looking at `version`, which
         * would misclassify a genuinely OLDER (smaller) blob as unreadable
         * corruption instead of the version check below. Only a blob too
         * short to even contain the `version` byte is genuinely ambiguous;
         * everything else must be classified by version first, with the
         * exact-size check applied only to the current-version case (a real
         * current-version blob is always written at exactly sizeof(blob)). */
        if (err == HAL_OK && len < sizeof(blob.version)) {
            ESP_LOGW(TAG, "relay cycle blob is too short to contain a version -- starting at zero");
        } else if (err == HAL_OK && blob.version == RELAY_CYCLES_VERSION) {
            if (len != sizeof(blob)) {
                ESP_LOGW(TAG, "relay cycle blob claims current version but is the wrong size -- starting at zero");
            } else {
                memcpy(s_rc.counts, blob.counts, sizeof(s_rc.counts));
                memcpy(s_rc.types, blob.types, sizeof(s_rc.types));
                memcpy(s_rc.rated_overrides, blob.rated_overrides, sizeof(s_rc.rated_overrides));
            }
        } else if (err == HAL_OK && blob.version > RELAY_CYCLES_VERSION) {
            /* Newer than this firmware understands -- a firmware-rollback
             * case (TODO.md 8.1). Refuse to load rather than guess at a
             * layout this build doesn't know, and leave flash untouched so
             * a subsequent boot on the newer firmware still finds it. */
            ESP_LOGW(TAG, "relay cycle blob version %u is newer than this firmware's %u -- refusing to load, "
                     "leaving flash untouched", blob.version, RELAY_CYCLES_VERSION);
        } else if (err == HAL_OK && blob.version == 1 && len == sizeof(relay_cycles_blob_v1_t)) {
            /* v1 -> v2 migration (RELAY_LIFE_BUDGET_PLAN.md step 1): the old
             * blob is a bare 4-count array read through the SAME `blob`
             * variable's first sizeof(relay_cycles_blob_v1_t) bytes, since
             * v1's layout (version byte + 4 counts) is a strict prefix of
             * v2's (version byte + 5 counts + ...) -- hal_kv_get_blob() above
             * already wrote those bytes into `blob` before this branch is
             * reached, only the trailing v2-only fields were left untouched
             * by whatever hal_kv's fake/real backend does with a
             * shorter-than-buffer read. Re-read explicitly as v1 to avoid
             * depending on that. */
            relay_cycles_blob_v1_t v1;
            memcpy(&v1, &blob, sizeof(v1));
            memcpy(s_rc.counts, v1.counts, sizeof(v1.counts));
            /* Fifth slot (safety relay) starts at 0; types/overrides already
             * memset to RELAY_TYPE_SSR/0 above. */
            ESP_LOGI(TAG, "migrated relay cycle blob v1 -> v%u (fifth slot + types added, "
                     "existing relays default to ssr)", RELAY_CYCLES_VERSION);
        } else if (err == HAL_OK) {
            /* Anything else older than current with no migration defined. */
            ESP_LOGW(TAG, "relay cycle blob version %u predates this firmware's %u with no migration defined -- "
                     "starting at zero", blob.version, RELAY_CYCLES_VERSION);
        } else if (err != HAL_NOT_FOUND) {
            /* Missing (first boot, or nothing survived migration) is the
             * only case treated identically to "start at zero" without a
             * warning; anything else (unreadable) is logged as corrupt
             * data. */
            memset(s_rc.counts, 0, sizeof(s_rc.counts));
            ESP_LOGW(TAG, "relay cycle blob load failed (%s) -- starting at zero",
                     hal_status_to_name(err));
        }
        hal_kv_close(&h);
    }

    s_rc.dirty = false;
    s_rc.last_persist_us = (int64_t)hal_time_now_us();
    s_rc.initialized = true;
    xSemaphoreGive(s_rc.lock);

    ESP_LOGI(TAG, "relay contact cycles loaded: %lu %lu %lu %lu", (unsigned long)s_rc.counts[0],
             (unsigned long)(KILN_IO_RELAY_COUNT > 1 ? s_rc.counts[1] : 0),
             (unsigned long)(KILN_IO_RELAY_COUNT > 2 ? s_rc.counts[2] : 0),
             (unsigned long)(KILN_IO_RELAY_COUNT > 3 ? s_rc.counts[3] : 0));
    return ESP_OK;
}

void relay_cycles_add(uint8_t relay_mask, uint32_t cycles)
{
    if (cycles == 0 || relay_mask == 0 || !ensure_lock()) {
        return;
    }
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        if (relay_mask & (uint8_t)(1u << r)) {
            /* Saturate rather than wrap: a wrapped contact-life counter reads
             * as a brand-new relay, which is the one wrong answer that
             * matters here. */
            if (s_rc.counts[r] > UINT32_MAX - cycles) {
                s_rc.counts[r] = UINT32_MAX;
            } else {
                s_rc.counts[r] += cycles;
            }
            s_rc.dirty = true;
        }
    }
    xSemaphoreGive(s_rc.lock);
}

void relay_cycles_note_safety_edge(void)
{
    if (!ensure_lock()) {
        return;
    }
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    uint32_t *c = &s_rc.counts[RELAY_CYCLES_SAFETY_INDEX];
    /* Same saturate-rather-than-wrap rule as relay_cycles_add(). */
    if (*c < UINT32_MAX) {
        (*c)++;
    }
    s_rc.dirty = true;
    xSemaphoreGive(s_rc.lock);
}

void relay_cycles_get(uint32_t *out)
{
    /* Deliberately still KILN_IO_RELAY_COUNT entries, not RELAY_CYCLES_COUNT:
     * dashboard_http.c's only caller passes a KILN_IO_RELAY_COUNT-sized stack
     * array (TODO.md 6A.1 predates the safety-relay slot), and this step is
     * explicitly scoped to leave dashboard/HTTP files untouched. Widening
     * this call's contract would silently overflow that caller's buffer.
     * relay_cycles_get_all() below is the RELAY_CYCLES_COUNT-sized form for
     * new callers (a later plan step wiring the fifth slot into the
     * dashboard). */
    if (!out) {
        return;
    }
    if (!ensure_lock()) {
        memset(out, 0, sizeof(uint32_t) * KILN_IO_RELAY_COUNT);
        return;
    }
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    memcpy(out, s_rc.counts, sizeof(uint32_t) * KILN_IO_RELAY_COUNT);
    xSemaphoreGive(s_rc.lock);
}

void relay_cycles_get_all(uint32_t *out)
{
    if (!out) {
        return;
    }
    if (!ensure_lock()) {
        memset(out, 0, sizeof(uint32_t) * RELAY_CYCLES_COUNT);
        return;
    }
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    memcpy(out, s_rc.counts, sizeof(s_rc.counts));
    xSemaphoreGive(s_rc.lock);
}

void relay_cycles_set_type(uint8_t relay, relay_type_t type, uint32_t rated_override)
{
    if (relay >= RELAY_CYCLES_COUNT || !ensure_lock()) {
        return;
    }
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    /* opus review (LOW): only mark dirty when something actually changed --
     * every boot's zones_config load path calls this once per relay
     * (zones_config_store.c's zones_config_push_all_relay_types()) even when
     * the persisted type/override already match, which previously
     * guaranteed an NVS write on every single boot regardless of whether
     * anything was new. */
    if (s_rc.types[relay] != (uint8_t)type || s_rc.rated_overrides[relay] != rated_override) {
        s_rc.types[relay] = (uint8_t)type;
        s_rc.rated_overrides[relay] = rated_override;
        s_rc.dirty = true; /* type/override are persisted alongside the counts */
    }
    xSemaphoreGive(s_rc.lock);
}

void relay_cycles_get_type(uint8_t relay, relay_type_t *type, uint32_t *rated_override)
{
    if (relay >= RELAY_CYCLES_COUNT) {
        if (type) *type = RELAY_TYPE_SSR;
        if (rated_override) *rated_override = 0;
        return;
    }
    if (!ensure_lock()) {
        if (type) *type = RELAY_TYPE_SSR;
        if (rated_override) *rated_override = 0;
        return;
    }
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    if (type) *type = (relay_type_t)s_rc.types[relay];
    if (rated_override) *rated_override = s_rc.rated_overrides[relay];
    xSemaphoreGive(s_rc.lock);
}

/* Table lookup for a type with no override -- the plan's rated-life table.
 * RELAY_TYPE_SSR has no budget (returns 0, meaning "no budget" to callers
 * that check has_budget rather than relying on this return alone). */
static uint32_t rated_life_for_type(relay_type_t type)
{
    switch (type) {
        case RELAY_TYPE_CONTACTOR: return RELAY_RATED_LIFE_CONTACTOR;
        case RELAY_TYPE_MERCURY:   return RELAY_RATED_LIFE_MERCURY;
        case RELAY_TYPE_SSR:
        default:                   return 0;
    }
}

void relay_cycles_budget(uint8_t relay, relay_cycles_budget_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (relay >= RELAY_CYCLES_COUNT) {
        return;
    }
    if (!ensure_lock()) {
        return;
    }

    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    uint32_t cycles = s_rc.counts[relay];
    relay_type_t type = (relay_type_t)s_rc.types[relay];
    uint32_t override_val = s_rc.rated_overrides[relay];
    xSemaphoreGive(s_rc.lock);

    if (type == RELAY_TYPE_SSR) {
        /* No budget at all -- ssr never shows a percent (plan's "Design"
         * section: "ssr = no budget (icon never shown, percent reported as
         * null)"). An override on an ssr relay is ignored on purpose: the
         * type itself is the "this relay has no wear budget" statement. */
        return;
    }

    uint32_t rated = (override_val != 0) ? override_val : rated_life_for_type(type);
    if (rated == 0) {
        /* A non-ssr type with a table value of 0 (shouldn't happen given the
         * table above, but a future type addition could forget to fill it
         * in) is treated the same as "no budget" rather than dividing by
         * zero. */
        return;
    }

    out->has_budget = true;
    out->cycles = cycles;
    out->rated = rated;
    /* Computed on read, never stored, per the plan. Saturates past 100%
     * rather than wrapping -- a relay well past its rated life should read
     * as, say, 140%, not wrap back toward 0. */
    out->percent = ((float)cycles / (float)rated) * 100.0f;
    if (out->percent >= 90.0f) {
        out->tier = RELAY_BUDGET_TIER_ERROR;
    } else if (out->percent >= 80.0f) {
        out->tier = RELAY_BUDGET_TIER_WARN;
    } else {
        out->tier = RELAY_BUDGET_TIER_NONE;
    }
}

relay_budget_tier_t relay_cycles_max_budget_tier(void)
{
    relay_budget_tier_t max_tier = RELAY_BUDGET_TIER_NONE;
    for (uint8_t r = 0; r < RELAY_CYCLES_COUNT; r++) {
        relay_cycles_budget_t b;
        relay_cycles_budget(r, &b);
        if (b.has_budget && b.tier > max_tier) {
            max_tier = b.tier;
        }
    }
    return max_tier;
}

/* The job run ON the flash worker's own internal-SRAM stack -- see
 * safety_cfg_store.c's nvs_save_store_job()/adaptive_tune.c's
 * save_kibase_job() for the identical shape. `arg` points at the calling
 * task's own esp_err_t local, safe because uart_bridge_ext_run_on_flash_
 * worker() blocks the caller for the whole call. */
static void reset_persist_job(void *arg)
{
    esp_err_t *out_err = (esp_err_t *)arg;
    *out_err = hal_status_to_esp_err(persist_locked());
}

bool relay_cycles_reset(unsigned relay)
{
    if (relay >= RELAY_CYCLES_COUNT || !ensure_lock()) {
        return false;
    }

    uint32_t old_count = 0;
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    old_count = s_rc.counts[relay];
    s_rc.counts[relay] = 0;
    s_rc.dirty = true;
    xSemaphoreGive(s_rc.lock);

    /* RE-ENTRANCY (flash_worker_lint.py's pattern 1): check whether we are
     * already ON the flash worker before dispatching a second job onto it --
     * dispatching from inside an already-dispatched job deadlocks the real
     * board (see uart_bridge.h's doc comment and adaptive_tune.c's identical
     * guard). Neither of this function's two known callers (the LCD
     * diagnostics page's LVGL-task two-tap confirm, and diagnostics_http.c's
     * httpd-task POST handler) is expected to already be on the worker
     * today, but the check is cheap and this is exactly the class of bug
     * that stays invisible until a caller changes. */
    /* opus review finding (RELAY_LIFE_BUDGET_PLAN.md audit): persist_locked()
     * reads/clears s_rc.counts/types/rated_overrides/dirty with NO lock of
     * its own -- it is written to be called with s_rc.lock already held
     * (relay_cycles_maybe_persist() does exactly that around its own call a
     * few lines down). This dispatch used to call it via reset_persist_job()
     * with the lock already released above, racing relay_cycles_add(): a
     * concurrent add() between the release above and persist_locked()'s
     * memcpy could see its increment folded into the blob written to flash
     * (harmless) OR see dirty cleared by this persist and then never win the
     * race to set it again (the add()'s own increment silently never gets
     * persisted). Held across the dispatch instead -- reset_persist_job()
     * itself takes no lock, so this cannot deadlock the flash worker. */
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    esp_err_t err;
    if (uart_bridge_ext_is_on_flash_worker()) {
        reset_persist_job(&err);
    } else {
        esp_err_t job_err = ESP_FAIL;
        esp_err_t submit_err = uart_bridge_ext_run_on_flash_worker(reset_persist_job, &job_err);
        /* uart_bridge_ext_run_on_flash_worker() itself returning non-OK
         * (worker not started/queue busy) means reset_persist_job() never
         * ran and job_err was never written -- report submit_err in that
         * case rather than the uninitialized-in-effect job_err. */
        err = (submit_err != ESP_OK) ? submit_err : job_err;
    }
    xSemaphoreGive(s_rc.lock);

    if (err != ESP_OK) {
        /* opus review finding: "will retry on the next periodic persist" is
         * only true when something is actually ticking relay_cycles_maybe_
         * persist() -- boot_guard.h's RECOVERY MODE deliberately does not
         * start profile_executor/autotune_engine, and this codebase has no
         * other periodic caller of that function (grep confirms), so a
         * failed persist while the board is in recovery mode is RAM-only
         * until the next successful boot/persist and is lost on a reboot in
         * the meantime. Say so rather than promise a retry that may not
         * happen. */
        ESP_LOGW(TAG, "relay_cycles_reset(%u): persist failed (%s) -- count zeroed in RAM only; "
                      "this is lost on reboot unless something calls relay_cycles_maybe_persist() "
                      "again first (it will not tick in RECOVERY MODE)", relay, esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "relay_cycles_reset(%u): count reset from %lu to 0", relay, (unsigned long)old_count);
    return true;
}

void relay_cycles_maybe_persist(void)
{
    if (!ensure_lock()) {
        return;
    }
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    bool due = s_rc.dirty &&
               ((int64_t)hal_time_now_us() - s_rc.last_persist_us) >= (int64_t)RELAY_CYCLES_PERSIST_INTERVAL_S * 1000000;
    hal_status_t err = due ? persist_locked() : HAL_OK;
    xSemaphoreGive(s_rc.lock);

    if (err != HAL_OK) {
        ESP_LOGW(TAG, "periodic persist failed: %s (counts kept in RAM, will retry)", hal_status_to_name(err));
    }
}

esp_err_t relay_cycles_flush(void)
{
    if (!ensure_lock()) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    hal_status_t err = s_rc.dirty ? persist_locked() : HAL_OK;
    xSemaphoreGive(s_rc.lock);
    if (err != HAL_OK) {
        ESP_LOGW(TAG, "flush failed: %s", hal_status_to_name(err));
        return hal_status_to_esp_err(err);
    }
    return ESP_OK;
}
