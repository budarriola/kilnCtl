#include "run_state.h"

#include <string.h>

#include "esp_log.h"
#include "hal_time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "hal_kv.h"
#include "hal_esp_common.h" /* hal_status_to_esp_err() -- preserve the specific esp_err_t this
                              * module's callers already branch on */

static const char *TAG = "run_state";

/* Same namespace as the rest of this board's configuration (zones_http.c,
 * rules_http.c, relay_cycles.c) but its OWN key -- the same deliberate
 * choice relay_cycles.c documents, for the same reason and then some. The
 * zones_cfg blob's loader treats any size change as "corrupt, start
 * unconfigured", so folding a field into it silently wipes an operator's
 * zone setup on the first boot after an update. This codebase has been
 * bitten by that twice. A separate key also means a corrupt/rejected run
 * record can never take the zone config down with it: the worst case here is
 * losing one breadcrumb, and it must stay that way. */
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_RUN "run_state"

/* TODO.md 8.1: this module's persisted store, split out of the default NVS
 * partition into its own partition, shared with relay_cycles.c/zones_http.c/
 * rules_http.c, each of which manages its own init/migration independently. */
#define KILN_NVS_PARTITION "kiln_nvs"

/* WRITE CADENCE AND FLASH WEAR
 * ---------------------------------------------------------------------
 * The executor ticks at 1 Hz. Writing this record per tick would be ~43,000
 * NVS writes across a 12-hour firing, which is how you trade a kiln
 * controller's flash for information nobody needed at one-second
 * resolution. So writes happen on state transitions -- run start, segment
 * change, pause, resume, fault, done, halt -- plus a slow periodic refresh
 * while RUNNING, which is the only thing that keeps "how far did it get"
 * meaningful when the power cuts mid-segment.
 *
 * RUN_STATE_REFRESH_INTERVAL_S = 300 (5 min), against a 12-hour firing:
 *   - periodic refreshes:  12 h / 5 min = 144
 *   - transitions:         ~1 per segment (PROFILE_MAX_SEGMENTS is 12) plus
 *                          start/end, so under 30 even with pausing
 *   -> under 200 writes per firing, of a 104-byte blob.
 * NVS distributes entry updates across the whole partition and erases at
 * page granularity, so even at ~1000 lifetime firings (a decade of heavy
 * hobby use) that is on the order of 2*10^5 entry writes spread over the
 * partition -- comfortably inside the ~10^5 erase-cycles-per-sector budget
 * this flash is rated for, and the same order of magnitude as
 * relay_cycles.c's 600 s cadence which is already in service.
 *
 * Why 300 s and not relay_cycles.c's 600 s: the two records answer different
 * questions. A contact-cycle counter losing ten minutes of counts loses a
 * rounding error off a six-figure total. This record losing ten minutes
 * loses resolution on the one number the operator is squinting at -- how far
 * into the segment it was when the lights went out. Five minutes bounds that
 * uncertainty at five minutes, against segments that are typically an hour
 * or more, for double the (already negligible) write count. */

typedef struct {
    SemaphoreHandle_t lock;

    /* The record as loaded at boot -- the breadcrumb. Frozen at init and
     * never written by this boot's run, so the operator's view of "what was
     * happening before the reset" cannot be overwritten by the firing they
     * start while reading it. */
    run_state_record_t boot;
    bool boot_valid;

    /* The live record this boot's run is maintaining. Kept in RAM so a
     * periodic refresh has something complete to write without the executor
     * having to re-supply every field it hasn't changed. */
    run_state_record_t live;

    bool     wrote_this_boot; /* a run has overwritten the stored record since init */
    int64_t  last_write_us;
    bool     initialized;
} run_state_ctx_t;

static run_state_ctx_t s_rs;

/* Brings up KILN_NVS_PARTITION, erasing ONLY that partition if its contents
 * are unusable. Adapted from wifi_prov.c's nvs_partition_init() (2026-08-12
 * NVS-partition split): NO_FREE_PAGES / NEW_VERSION_FOUND leave NVS unable
 * to mount at all, so erasing is the only cure, but it must stay scoped to
 * the partition that is actually broken. */
static esp_err_t nvs_partition_init(const char *partition)
{
    return hal_status_to_esp_err(hal_kv_init_partition(partition));
}

/* Forward declaration -- defined below, but this write path needs it before
 * that point in the file. See its own definition for what it checks. */
static bool caller_stack_is_external(void);

/* One-time, one-directional copy of the old default-partition record into
 * KILN_NVS_PARTITION, for boards provisioned by firmware predating the
 * split. The old copy is left in place (never deleted) so a rollback to
 * pre-split firmware still finds its breadcrumb -- see wifi_prov.c's
 * migrate_from_default_partition() for the fuller rationale. Only called
 * when KILN_NVS_PARTITION has nothing under NVS_KEY_RUN yet. The record's
 * existing version check (RUN_STATE_RECORD_VERSION) is reused unchanged --
 * this function does not add any versioning logic of its own.
 *
 * DRAM_PSRAM_PLAN.md section 9 write-path re-audit (2026-09-02): this
 * function writes NVS (nvs_set_blob()/nvs_commit() below) exactly like
 * persist_locked() does further down, but never got persist_locked()'s
 * caller_stack_is_external() guard when that guard was added -- the earlier
 * pass treated "this file is guarded" as true of the file's main write path
 * and never re-checked every OTHER write call site inside it. In practice
 * this function is only ever reached from run_state_init(), itself only
 * called once from profile_executor_start() on app_main's own task (an
 * internal-SRAM stack) before any PSRAM-stacked task exists, so it cannot
 * fire the crash today -- but that makes it exactly the kind of gap a future
 * audit would read as "covered" without this guard. Added for that reason,
 * not because a live path was found. */
static void migrate_from_default_partition(void)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, NULL);
    if (err != HAL_OK) {
        return;
    }
    run_state_record_t old_rec;
    size_t len = sizeof(old_rec);
    err = hal_kv_get_blob(&h, NVS_KEY_RUN, &old_rec, &len);
    hal_kv_close(&h);
    if (err != HAL_OK || len != sizeof(old_rec) || old_rec.version != RUN_STATE_RECORD_VERSION) {
        /* Nothing there, wrong size, or an old/new version this build's
         * existing check wouldn't have trusted anyway -- nothing to
         * migrate. */
        return;
    }

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
    err = hal_kv_set_blob(&hw, NVS_KEY_RUN, &old_rec, sizeof(old_rec));
    if (err == HAL_OK) {
        err = hal_kv_commit(&hw);
    }
    hal_kv_close(&hw);
    if (err == HAL_OK) {
        ESP_LOGI(TAG, "migrated run-state record from default NVS partition to '%s'", KILN_NVS_PARTITION);
    } else {
        ESP_LOGW(TAG, "run-state record migration to '%s' failed: %s", KILN_NVS_PARTITION, hal_status_to_name(err));
    }
}

/* The blob written to flash IS this struct, and the loader rejects any blob
 * whose size differs -- so a silent layout change (an added field, a
 * different alignment) would look like corruption and quietly stop reporting
 * interrupted firings. Pinning the number makes that a build error instead,
 * with run_state.h's version bump as the intended way to change it. The
 * whole context is two records plus a lock and a timestamp: 240 bytes of
 * .bss, which is the entire memory cost of this feature. That budget is
 * deliberate -- this firmware was once found booting with 7 KB of free heap
 * because a static buffer was sized without anybody adding it up. */
_Static_assert(sizeof(run_state_record_t) == 104, "run_state_record_t layout changed -- bump RUN_STATE_RECORD_VERSION");

const char *run_state_phase_name(run_state_phase_t phase)
{
    switch (phase) {
    case RUN_STATE_PHASE_NONE:    return "none";
    case RUN_STATE_PHASE_RUNNING: return "running";
    case RUN_STATE_PHASE_PAUSED:  return "paused";
    case RUN_STATE_PHASE_DONE:    return "done";
    case RUN_STATE_PHASE_HALTED:  return "halted";
    case RUN_STATE_PHASE_FAULTED: return "faulted";
    default:                      return "unknown";
    }
}

/* The one predicate this whole module exists to answer. A record still
 * saying RUNNING or PAUSED means no ending was ever recorded -- the firmware
 * stopped between two writes. Everything else is an ending the firmware
 * itself wrote down, which is the operator's evidence that the stop was
 * deliberate. */
static bool phase_is_interrupted(uint8_t phase)
{
    return phase == RUN_STATE_PHASE_RUNNING || phase == RUN_STATE_PHASE_PAUSED;
}

static bool ensure_lock(void)
{
    if (!s_rs.lock) {
        s_rs.lock = xSemaphoreCreateMutex();
        if (!s_rs.lock) {
            ESP_LOGE(TAG, "xSemaphoreCreateMutex failed -- no run-state breadcrumb will be kept");
            return false;
        }
    }
    return true;
}

/* True iff the CURRENTLY EXECUTING task's own stack lives in external RAM
 * (PSRAM). Same predicate, same reasoning, and same incident class as
 * kiln_cfg_store.c's/safety_cfg_store.c's caller_stack_is_external(): a
 * flash/NVS write disables the cache, which makes a PSRAM-resident stack
 * unreachable and aborts the whole board via ESP-IDF's own
 * esp_task_stack_is_sane_cache_disabled() rather than failing just this one
 * call. This module's persist_locked() is called directly from
 * profile_executor's tick path (run_state_note()/run_state_note_progress()),
 * which DRAM_PSRAM_PLAN.md section 7.3 names as the highest-care relocation
 * candidate specifically because of this write -- this is the belt-and-
 * suspenders check for it, closing the gap an earlier pass of that plan left
 * (kiln_cfg_store.c and safety_cfg_store.c got this guard; this module,
 * reached from the very task the plan is most worried about, had not). */
static bool caller_stack_is_external(void)
{
    return !hal_kv_write_safe_here();
}

/* Must be called with s_rs.lock held. */
static esp_err_t persist_locked(const run_state_record_t *rec)
{
    if (caller_stack_is_external()) {
        ESP_LOGE(TAG, "persist_locked: REFUSING -- calling task's stack is in external RAM "
                      "(PSRAM). A flash/NVS write from here would abort the whole board "
                      "(ESP-IDF's esp_task_stack_is_sane_cache_disabled()). Route this call "
                      "through a task with an internal-SRAM stack instead -- see "
                      "DRAM_PSRAM_PLAN.md section 7.2 and uart_bridge_ext.c's flash-safe "
                      "worker for the established pattern.");
        return ESP_ERR_INVALID_STATE;
    }
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_blob(&h, NVS_KEY_RUN, rec, sizeof(*rec));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    if (err == HAL_OK) {
        s_rs.last_write_us = (int64_t)hal_time_now_us();
    }
    return hal_status_to_esp_err(err);
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

/* Must be called with s_rs.lock held. */
static void fill_live_locked(run_state_phase_t phase, const run_state_snapshot_t *snap)
{
    run_state_record_t *r = &s_rs.live;
    memset(r, 0, sizeof(*r));
    r->version = RUN_STATE_RECORD_VERSION;
    r->phase = (uint8_t)phase;
    /* Uptime, not a date: this board has no RTC and no guaranteed SNTP, so
     * an absolute timestamp here would be invented. See run_state.h. */
    r->uptime_s = (uint32_t)((int64_t)hal_time_now_us() / 1000000);
    if (!snap) {
        return;
    }
    r->profile_id = snap->profile_id;
    r->zone_mask = snap->zone_mask;
    r->segment_index = snap->segment_index;
    r->segment_count = snap->segment_count;
    r->dwelling = snap->dwelling ? 1u : 0u;
    r->target_c = snap->target_c;
    r->segment_elapsed_s = snap->segment_elapsed_s;
    r->fault_guard = snap->fault_guard;
    copy_str(r->profile_name, sizeof(r->profile_name), snap->profile_name);
    copy_str(r->fault_reason, sizeof(r->fault_reason), snap->fault_reason);
}

esp_err_t run_state_init(void)
{
    if (!ensure_lock()) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- run-state record will not persist",
                 KILN_NVS_PARTITION, esp_err_to_name(part_err));
    }

    xSemaphoreTake(s_rs.lock, portMAX_DELAY);
    memset(&s_rs.boot, 0, sizeof(s_rs.boot));
    memset(&s_rs.live, 0, sizeof(s_rs.live));
    s_rs.boot_valid = false;
    s_rs.wrote_this_boot = false;
    s_rs.last_write_us = (int64_t)hal_time_now_us();
    s_rs.initialized = true;

    /* Migrate before the real load so a pre-split board's breadcrumb shows up
     * on the very first boot after the update, not one boot late. */
    if (part_err == ESP_OK) {
        migrate_from_default_partition();
    }

    hal_kv_handle_t h;
    hal_status_t kv_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (kv_err == HAL_OK) {
        run_state_record_t rec;
        size_t len = sizeof(rec);
        kv_err = hal_kv_get_blob(&h, NVS_KEY_RUN, &rec, &len);
        if (kv_err == HAL_OK && len == sizeof(rec) && rec.version == RUN_STATE_RECORD_VERSION) {
            /* Strings come out of flash and are about to be logged and later
             * emitted as JSON. Terminate them here rather than trusting the
             * blob: a truncated/garbled write is exactly the failure mode a
             * brownout mid-commit produces, and this is the one place that
             * can still contain it. */
            rec.profile_name[sizeof(rec.profile_name) - 1] = '\0';
            rec.fault_reason[sizeof(rec.fault_reason) - 1] = '\0';
            s_rs.boot = rec;
            s_rs.boot_valid = true;
        } else if (kv_err != HAL_NOT_FOUND) {
            /* Missing (first boot), wrong size (a build with a different
             * layout), or an old version: treat as "no record". Same
             * load-tolerant convention as relay_cycles.c -- refusing to boot
             * over a lost breadcrumb would be the wrong trade for a kiln. */
            ESP_LOGW(TAG, "run-state record unreadable (%s, %u bytes) -- treating as no record",
                     hal_status_to_name(kv_err), (unsigned)len);
        }
        hal_kv_close(&h);
    }

    run_state_record_t boot = s_rs.boot;
    bool boot_valid = s_rs.boot_valid;
    xSemaphoreGive(s_rs.lock);

    if (!boot_valid || boot.phase == RUN_STATE_PHASE_NONE) {
        ESP_LOGI(TAG, "no previous firing recorded");
        return ESP_OK;
    }

    if (phase_is_interrupted(boot.phase) && !boot.acknowledged) {
        /* Loud on purpose. This is the line an operator (or whoever reads
         * the boot log afterwards) needs to find. It is also the whole
         * extent of what a loaded record is allowed to cause: nothing here
         * starts a profile, energizes a relay, or is handed to
         * profile_executor.c. See run_state.h. */
        ESP_LOGW(TAG, "**********************************************************");
        ESP_LOGW(TAG, "PREVIOUS FIRING WAS INTERRUPTED -- it never ended cleanly.");
        ESP_LOGW(TAG, "  profile '%s' (id %u), zones 0x%02X", boot.profile_name, boot.profile_id, boot.zone_mask);
        ESP_LOGW(TAG, "  segment %u/%u, %s, target %.1f C, %lu s into the segment",
                 (unsigned)(boot.segment_index + 1u), boot.segment_count,
                 boot.dwelling ? "dwelling" : "ramping", (double)boot.target_c,
                 (unsigned long)boot.segment_elapsed_s);
        ESP_LOGW(TAG, "  last written %lu s into the previous boot (no wall clock on this board)",
                 (unsigned long)boot.uptime_s);
        ESP_LOGW(TAG, "  state at that point: %s", run_state_phase_name((run_state_phase_t)boot.phase));
        ESP_LOGW(TAG, "NOTHING WILL BE RESTARTED. Relays are off and stay off until an");
        ESP_LOGW(TAG, "operator explicitly starts a firing (TODO.md 6A.3: no auto-resume).");
        ESP_LOGW(TAG, "**********************************************************");
    } else {
        /* Either a recorded ending, or an interruption the operator has
         * already acknowledged -- the phase name says which, and an
         * acknowledged interruption must not be re-announced as if it were
         * new. It is still logged: acknowledging is "I have seen this", not
         * "erase the history". */
        ESP_LOGI(TAG, "previous firing: profile '%s' (id %u), %s%s, segment %u/%u%s%s",
                 boot.profile_name, boot.profile_id, run_state_phase_name((run_state_phase_t)boot.phase),
                 phase_is_interrupted(boot.phase) ? " (interrupted, already acknowledged)" : " (clean end)",
                 (unsigned)(boot.segment_index + 1u), boot.segment_count,
                 boot.fault_reason[0] ? ", reason: " : "", boot.fault_reason);
    }
    return ESP_OK;
}

void run_state_note(run_state_phase_t phase, const run_state_snapshot_t *snap)
{
    if (!ensure_lock()) {
        return;
    }
    xSemaphoreTake(s_rs.lock, portMAX_DELAY);
    fill_live_locked(phase, snap);
    esp_err_t err = persist_locked(&s_rs.live);
    if (err == ESP_OK) {
        s_rs.wrote_this_boot = true;
    }
    xSemaphoreGive(s_rs.lock);

    if (err != ESP_OK) {
        /* A failed transition write is the case that makes the record lie:
         * a DONE that didn't land reads back next boot as an interrupted
         * firing. Nothing here can fix that, but it must not be silent --
         * the log is then the only place the true ending exists. */
        ESP_LOGE(TAG, "could not persist run state '%s': %s -- next boot may report this firing as interrupted",
                 run_state_phase_name(phase), esp_err_to_name(err));
    }
}

void run_state_note_progress(const run_state_snapshot_t *snap)
{
    if (!snap || !ensure_lock()) {
        return;
    }
    xSemaphoreTake(s_rs.lock, portMAX_DELAY);
    /* Only refresh a run already noted as RUNNING. A PAUSED/DONE/FAULTED
     * record is finished as far as this module is concerned, and re-writing
     * it from a stale tick would either burn flash for nothing or, worse,
     * re-open an ending that was already recorded. */
    bool due = s_rs.live.phase == RUN_STATE_PHASE_RUNNING &&
               ((int64_t)hal_time_now_us() - s_rs.last_write_us) >= (int64_t)RUN_STATE_REFRESH_INTERVAL_S * 1000000;
    esp_err_t err = ESP_OK;
    if (due) {
        fill_live_locked(RUN_STATE_PHASE_RUNNING, snap);
        err = persist_locked(&s_rs.live);
        if (err == ESP_OK) {
            s_rs.wrote_this_boot = true;
        } else {
            /* Don't retry-storm a failing flash: last_write_us is only
             * advanced on success, so the next attempt is one tick later.
             * Losing a refresh costs progress resolution, not the record. */
            s_rs.last_write_us = (int64_t)hal_time_now_us();
        }
    }
    xSemaphoreGive(s_rs.lock);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "periodic run-state refresh failed: %s (record kept in RAM, will retry)",
                 esp_err_to_name(err));
    }
}

bool run_state_get_boot_record(run_state_record_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!ensure_lock()) {
        return false;
    }
    xSemaphoreTake(s_rs.lock, portMAX_DELAY);
    bool have = s_rs.boot_valid && s_rs.boot.phase != RUN_STATE_PHASE_NONE && !s_rs.boot.acknowledged;
    if (have) {
        *out = s_rs.boot;
    }
    xSemaphoreGive(s_rs.lock);
    return have;
}

bool run_state_boot_record_interrupted(void)
{
    if (!ensure_lock()) {
        return false;
    }
    xSemaphoreTake(s_rs.lock, portMAX_DELAY);
    bool interrupted = s_rs.boot_valid && !s_rs.boot.acknowledged && phase_is_interrupted(s_rs.boot.phase);
    xSemaphoreGive(s_rs.lock);
    return interrupted;
}

bool run_state_acknowledge(void)
{
    if (!ensure_lock()) {
        return false;
    }
    xSemaphoreTake(s_rs.lock, portMAX_DELAY);
    if (!s_rs.boot_valid || s_rs.boot.acknowledged) {
        xSemaphoreGive(s_rs.lock);
        return false;
    }
    s_rs.boot.acknowledged = 1u;

    /* Persist the acknowledgement only if the stored record is still the one
     * being acknowledged. Once a firing started this boot, the key holds
     * THAT run's record and the old one is already gone from flash --
     * writing the acknowledged copy back would overwrite a live firing's
     * breadcrumb with a dead one, which is the exact failure this module
     * exists to prevent. Dropping the RAM copy is then the whole job. */
    esp_err_t err = ESP_OK;
    if (!s_rs.wrote_this_boot) {
        err = persist_locked(&s_rs.boot);
    }
    xSemaphoreGive(s_rs.lock);

    if (err != ESP_OK) {
        /* The banner is gone from the UI either way (the RAM copy is
         * flagged); it will simply come back after the next reboot. */
        ESP_LOGW(TAG, "could not persist acknowledgement: %s -- it will reappear after a reboot",
                 esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "previous-run record acknowledged by operator");
    return true;
}
