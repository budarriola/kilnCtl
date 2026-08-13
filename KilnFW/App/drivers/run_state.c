#include "run_state.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

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

/* Must be called with s_rs.lock held. */
static esp_err_t persist_locked(const run_state_record_t *rec)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_RUN, rec, sizeof(*rec));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        s_rs.last_write_us = esp_timer_get_time();
    }
    return err;
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
    r->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
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

    xSemaphoreTake(s_rs.lock, portMAX_DELAY);
    memset(&s_rs.boot, 0, sizeof(s_rs.boot));
    memset(&s_rs.live, 0, sizeof(s_rs.live));
    s_rs.boot_valid = false;
    s_rs.wrote_this_boot = false;
    s_rs.last_write_us = esp_timer_get_time();
    s_rs.initialized = true;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        run_state_record_t rec;
        size_t len = sizeof(rec);
        err = nvs_get_blob(h, NVS_KEY_RUN, &rec, &len);
        if (err == ESP_OK && len == sizeof(rec) && rec.version == RUN_STATE_RECORD_VERSION) {
            /* Strings come out of flash and are about to be logged and later
             * emitted as JSON. Terminate them here rather than trusting the
             * blob: a truncated/garbled write is exactly the failure mode a
             * brownout mid-commit produces, and this is the one place that
             * can still contain it. */
            rec.profile_name[sizeof(rec.profile_name) - 1] = '\0';
            rec.fault_reason[sizeof(rec.fault_reason) - 1] = '\0';
            s_rs.boot = rec;
            s_rs.boot_valid = true;
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            /* Missing (first boot), wrong size (a build with a different
             * layout), or an old version: treat as "no record". Same
             * load-tolerant convention as relay_cycles.c -- refusing to boot
             * over a lost breadcrumb would be the wrong trade for a kiln. */
            ESP_LOGW(TAG, "run-state record unreadable (%s, %u bytes) -- treating as no record",
                     esp_err_to_name(err), (unsigned)len);
        }
        nvs_close(h);
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
               (esp_timer_get_time() - s_rs.last_write_us) >= (int64_t)RUN_STATE_REFRESH_INTERVAL_S * 1000000;
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
            s_rs.last_write_us = esp_timer_get_time();
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
