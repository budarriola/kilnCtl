/* Firing-statistics accounting, NVS persistence, and the reboot breadcrumb
 * snapshot -- split out of profile_executor.c (2026-09-01, "files over 1500
 * lines should be broken up where it makes sense"). See
 * profile_executor_internal.h's own doc comment for the full three-way split
 * this is one piece of. profile_firing_history_blob_t and run_snapshot_buf_t
 * moved to that internal header instead of staying local here, since both
 * are also used directly by profile_executor.c call sites (run()/halt()/
 * pause()/resume()/the tick loop/the watchdog task). */

#include "profile_executor_internal.h"

#include <math.h>
#include <stdint.h> /* uintptr_t -- firing_shadow zone-index recovery, firing_stats_zone_tick() */
#include <stdlib.h> /* free() -- blobs below are heap-allocated, see firing_stats_load() */
#include <string.h>

#include "freertos/portmacro.h" /* portMUX_TYPE -- last-run cache guard below */

#include "esp_heap_caps.h"
#include "persist_scratch.h"
#include "esp_log.h"

#include "hal_kv.h"
#include "hal_esp_common.h" /* hal_status_to_esp_err() -- keeps esp_err_to_name() below meaningful */
#include "nvs_key_check.h"
#include "zones_config_accessors.h" /* zone_is_on_off() -- on/off zones have no target_c and no
                                      * meaningful IAE; skip them here, see ON_OFF_ZONE_PLAN.md
                                      * sec 1 "Firing stats / IAE" row */
#include "firing_stats_cfg_fs.h" /* cfg-filesystem dual-write bridge, docs/FILESYSTEM_USER_DATA_PLAN.md
                                     section 5 item 7 */
#include "cfg_fs_status.h" /* cfg_fs_status_item_diverged() -- firing_stats_get_dualwrite_status() below */
#include "profiles_builtin.h" /* g_builtin_profile_count/PROFILE_BUILTIN_ID_BASE -- last-run cache sizing/indexing */
#include <assert.h>
#include "firing_shadow.h" /* ITER_TUNE_REDESIGN_PLAN.md step 8, "shadow mode" -- scores every
                             * firing and writes only its own compact verdict-summary NVS
                             * namespace; never writes a gain, never calls iter_tune_*. */

/* ---- firing quality stats (PID_EXPANSION_PLAN.md Phase 7a/7a-2/7a-3) ------
 *
 * Set 2 from the plan: per-profile firing-accuracy history, distinct from
 * (and independent of) Set 1's per-zone tuning-quality figures, which this
 * task does not touch (autotune_engine.c/pid_autotune.c are off limits here
 * -- another agent owns that half).
 *
 * STORAGE: profiles_nvs (384 KB, see partitions.csv), namespace "fire_stats"
 * -- deliberately NOT kiln_nvs (64 KB, already carrying zones/rules/
 * relay_cycles/run_state) and NOT the "kiln_cfg" namespace profiles_http.c
 * itself uses in profiles_nvs, so a firing-stats read/write can never
 * collide with a profile-blob key. One key per profile_id
 * ("fs_<id>", <=8 chars, well under NVS's 15-char key limit even for a
 * 3-digit builtin id), holding a ring of the last
 * PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH runs for THAT profile only -- see
 * profile_firing_history_blob_t below. Only profiles that have actually
 * fired allocate a key, so the realistic footprint is (profiles actually
 * run) * sizeof(profile_firing_history_blob_t), not
 * (PROFILES_MAX_COUNT + builtin count) * that -- see this module's
 * PID_EXPANSION_PLAN.md report for the exact byte arithmetic.
 *
 * WHICH TASK WRITES: the profile executor's own control task
 * (executor_task_entry(), INTERNAL-stacked -- see profile_executor_start()'s
 * xTaskCreatePinnedToCore() call and its doc comment a few hundred lines
 * below, ~3491-3506 in this file as of this pass) or profile_executor_halt()
 * (called from whichever task the operator's Stop request lands on, e.g.
 * dashboard_http.c's HTTP handler task or the UART bridge). NEITHER is
 * PSRAM-stacked: the control task is internal by the same 2026-08-22-crash
 * reasoning that already lets it call run_state_note()/relay_cycles_
 * maybe_persist() from this exact tick path, and every HTTP/bridge task in
 * this codebase already writes NVS routinely (settings, profiles, kiln
 * config) without routing through uart_bridge_ext_run_on_flash_worker() --
 * that worker exists for PSRAM-stacked tasks only (autotune_engine.c's),
 * which this write path never runs on. Written ONCE at run completion (the
 * DONE/FAULTED tick transition, or an operator halt() that ends a RUNNING/
 * PAUSED run early) -- never per tick, so the partition is never worn by
 * this feature. */

#define FIRING_STATS_NVS_PARTITION "profiles_nvs"
#define FIRING_STATS_NVS_NAMESPACE "fire_stats"
NVS_KEY_LEN_CHECK(FIRING_STATS_NVS_PARTITION);
NVS_KEY_LEN_CHECK(FIRING_STATS_NVS_NAMESPACE);
/* NOTE: the per-profile key ("fs_%u", snprintf'd into a 16-byte buffer at
 * each call site below) is NOT a literal and cannot be checked at compile
 * time -- it stays within the 15-usable-char limit only because profile_id
 * is a small integer; a compile-time check would need to bound profile_id's
 * range instead of the string. */

/* One tick's worth of firing-quality accumulation for one zone -- pure
 * (touches only *z and its own arguments, no s_exec, no lock, no I/O), so a
 * host test can drive a synthetic error sequence through it directly without
 * a real FreeRTOS task loop. See profile_exec_firing_stats_t's doc comment
 * (profile_executor.h) for the exclusion/edge-case rules this implements:
 *
 *   - actual_valid == false: counted in fs_excluded_sample_count, NOT folded
 *     into fs_err_sum/fs_iae_raw_sum as a zero error. duration still
 *     accrues (the zone was still "in the run" for that tick), but nothing
 *     else does.
 *   - error is SIGNED (actual - target): + = running hot, - = running cold.
 *   - overshoot tracks the largest positive error; undershoot tracks the
 *     largest positive MAGNITUDE of a negative error (both reported as
 *     non-negative numbers, see the struct's own comment).
 *   - ramp vs dwell is decided by the caller's `dwelling` flag for this
 *     tick, not re-derived here. */
void firing_stats_zone_tick(zone_runtime_t *z, float target_c, bool dwelling, uint32_t elapsed_s,
                                    uint8_t segment_index, float dt_s)
{
    z->fs_duration_s += (uint32_t)(dt_s + 0.5f);

    /* Shadow mode (ITER_TUNE_REDESIGN_PLAN.md step 8): fed every tick of
     * every zone, valid or not -- firing_shadow_zone_tick() itself skips an
     * invalid sample, same exclusion rule as this function's own
     * fs_excluded_sample_count path just below. zone_index is recovered
     * WITHOUT pointer subtraction rather than adding a parameter to this
     * function's signature or a field to zone_runtime_t -- this hook must
     * not touch profile_executor.c's call site (another agent owns that
     * file concurrently), and s_exec is already extern-visible via
     * profile_executor_internal.h.
     *
     * Coordinator review, 2026-09-24: `z - s_exec.zones` (the original form
     * here) is undefined behaviour whenever `z` is not actually an element
     * of that array, regardless of any bounds check performed on the
     * result. test_profile_executor_prestart.c's own unit tests for this
     * function legitimately call it with standalone zone_runtime_t locals
     * that are NOT part of s_exec.zones (see that file's
     * firing_stats_zone_tick test section), so that UB is live, not
     * theoretical, in this tree. Fixed by comparing addresses as uintptr_t
     * against the array's [begin, end) byte range instead: relational/
     * equality comparison of unrelated-object addresses via uintptr_t is
     * well-defined (unlike pointer subtraction or pointer relational
     * comparison across unrelated objects), so a standalone local outside
     * the array now deterministically resolves to "not a real s_exec zone"
     * -- a silent skip, not an assert, since that is an expected, non-fatal
     * case for this function's existing test callers, not a defect. */
    uintptr_t z_addr = (uintptr_t)z;
    uintptr_t zones_begin = (uintptr_t)&s_exec.zones[0];
    uintptr_t zones_end = (uintptr_t)&s_exec.zones[MAX31856_CHANNEL_COUNT];
    if (z_addr >= zones_begin && z_addr < zones_end) {
        size_t byte_off = (size_t)(z_addr - zones_begin);
        size_t zi_shadow = byte_off / sizeof(s_exec.zones[0]);
        if ((byte_off % sizeof(s_exec.zones[0])) == 0 && zi_shadow < MAX31856_CHANNEL_COUNT) {
            firing_shadow_zone_tick((uint8_t)zi_shadow, z->actual_valid, z->actual_c, target_c, dwelling,
                                     segment_index, dt_s);
        }
    }

    if (!z->actual_valid) {
        z->fs_excluded_sample_count++;
        return;
    }
    float error = z->actual_c - target_c;
    /* docs/audits/iter_tune_decision_2026-09-07.md prep: capture this
     * zone's actual_c at the FIRST accumulated (actual_valid) tick, before
     * fs_sample_count increments below -- matches iter_tune.h's own
     * "start_temp_c: this zone's actual_c at the first accumulated tick"
     * wording exactly. Written once per run: fs_sample_count only reads 0
     * on this same tick. */
    if (z->fs_sample_count == 0) {
        z->fs_start_temp_c = z->actual_c;
    }
    z->fs_sample_count++;
    z->fs_err_sum += error;
    z->fs_iae_raw_sum += fabsf(error) * dt_s;
    if (error > z->fs_max_overshoot_c) {
        z->fs_max_overshoot_c = error;
        z->fs_max_overshoot_elapsed_s = elapsed_s;
        z->fs_max_overshoot_segment = segment_index;
    }
    if (-error > z->fs_max_undershoot_c) {
        z->fs_max_undershoot_c = -error;
        z->fs_max_undershoot_elapsed_s = elapsed_s;
        z->fs_max_undershoot_segment = segment_index;
    }
    float abs_error = fabsf(error);
    if (dwelling) {
        z->fs_dwell_abs_err_sum += abs_error;
        z->fs_dwell_sample_count++;
        if (abs_error > z->fs_dwell_err_max_c) z->fs_dwell_err_max_c = abs_error;
    } else {
        z->fs_ramp_abs_err_sum += abs_error;
        z->fs_ramp_sample_count++;
        if (abs_error > z->fs_ramp_err_max_c) z->fs_ramp_err_max_c = abs_error;
    }
}

/* Derives the reportable (mean/max/normalized) figures from a zone's raw
 * running sums. Pure -- takes no lock, touches no NVS -- so it's equally
 * usable for a live in-progress snapshot (profile_executor_get_status()) and
 * for the final record persisted at run end. duration_s/setpoint_span_c
 * come from the caller (duration is per-zone; span is run-wide, shared
 * across zones -- see s_exec_state_t.fs_target_min_c/fs_target_max_c). */
void firing_stats_snapshot(const zone_runtime_t *z, float setpoint_span_c,
                                   profile_exec_firing_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    out->sample_count = z->fs_sample_count;
    out->excluded_sample_count = z->fs_excluded_sample_count;
    out->duration_s = z->fs_duration_s;
    out->max_overshoot_c = z->fs_max_overshoot_c;
    out->max_overshoot_elapsed_s = z->fs_max_overshoot_elapsed_s;
    out->max_overshoot_segment = z->fs_max_overshoot_segment;
    out->max_undershoot_c = z->fs_max_undershoot_c;
    out->max_undershoot_elapsed_s = z->fs_max_undershoot_elapsed_s;
    out->max_undershoot_segment = z->fs_max_undershoot_segment;
    out->iae_raw_c_s = z->fs_iae_raw_sum;

    if (z->fs_sample_count > 0) {
        out->mean_error_c = z->fs_err_sum / (float)z->fs_sample_count;
    }
    if (z->fs_ramp_sample_count > 0) {
        out->ramp_err_mean_c = z->fs_ramp_abs_err_sum / (float)z->fs_ramp_sample_count;
    }
    out->ramp_err_max_c = z->fs_ramp_err_max_c;
    if (z->fs_dwell_sample_count > 0) {
        out->dwell_err_mean_c = z->fs_dwell_abs_err_sum / (float)z->fs_dwell_sample_count;
    }
    out->dwell_err_max_c = z->fs_dwell_err_max_c;

    /* Normalized IAE -- see profile_exec_firing_stats_t's doc comment for
     * the span-floor and zero-duration guards. Both guards make this 0
     * rather than NAN/inf for a degenerate run (no ticks yet, or a run that
     * somehow ended in the same tick it started) -- 0 reads as "nothing to
     * show," which is true, rather than propagating a NaN into whatever
     * later formats it. */
    float span = fmaxf(setpoint_span_c, PROFILE_EXECUTOR_FIRING_STATS_MIN_SPAN_C);
    if (out->duration_s > 0) {
        out->iae_normalized = out->iae_raw_c_s / ((float)out->duration_s * span);
    }
}

/* ---- last-run-started RAM cache (PROFILE_SLOTS_100_PLAN.md review LOW,
 * "list perf") ---------------------------------------------------------
 *
 * profiles_catalog_http.c's GET /api/profiles calls profile_executor_
 * last_run_started_unix_s() once per profile id -- up to PROFILES_MAX_COUNT +
 * g_builtin_profile_count times per request (36 today; ~132 once
 * PROFILES_MAX_COUNT reaches 100, docs/PROFILE_SLOTS_100_PLAN.md) -- and that
 * function used to do a full firing_stats_load() (an NVS blob read, cfg-fs
 * resolve, and a 1364 B heap_caps_malloc) on EVERY call, for EVERY request,
 * on the httpd_worker task. This cache makes that O(1) NVS per request:
 * first read after boot (or after this id's history last changed) is a real
 * load and fills the cache; every other call for that id is one array read
 * under a spinlock, no NVS/flash touched at all.
 *
 * This module is the single writer of "fs_<id>" (firing_stats_persist()) and
 * its single eraser (firing_stats_erase()) -- see this file's own top
 * comment -- so both update the cache directly rather than relying on the
 * next reader to notice a change (CLAUDE.md's "reset one side of a pair" bug
 * class: whoever writes/erases the NVS side must also update the cached
 * side, in the SAME module, so the two can never drift). backup_import.c and
 * every other profiles.* writer never touch "fs_<id>" directly (grepped
 * 2026-09-19) -- profiles_http.c's slot-delete path is the only external
 * caller, and it goes through firing_stats_erase() like everything else.
 *
 * Sized from PROFILES_MAX_COUNT + g_builtin_profile_count (never a literal)
 * so it tracks both knobs automatically -- including the slots8->slots100
 * bump landing in a sibling worktree. Allocated lazily, once, on first use
 * (a board that never serves /api/profiles never pays for it) rather than at
 * some fixed boot hook, matching history_buf_ensure_alloc()'s established
 * convention (profile_executor_run.c) for this same reason. Kept off the
 * calling task's stack and off .bss: heap_caps_malloc'd, from PSRAM once the
 * array would exceed roughly 1 KB (which .bss growth toward 100 slots would
 * do), internal DRAM below that threshold since MALLOC_CAP_SPIRAM churn
 * isn't worth it for ~144 B today. A failed allocation degrades to "caching
 * disabled" (every call falls back to firing_stats_load()) rather than
 * aborting anything -- same non-fatal contract as every other lazy alloc in
 * this split. */
#define FS_LAST_RUN_CACHE_UNCACHED UINT32_MAX /* sentinel: "not yet looked up this boot" --
                                                * distinct from the legitimate cached value 0
                                                * ("looked up, genuinely never fired / fired
                                                * before RTC/SNTP sync") */

static uint32_t *volatile s_fs_last_run_cache = NULL;
static size_t s_fs_last_run_cache_count = 0;
static portMUX_TYPE s_fs_last_run_alloc_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_fs_last_run_alloc_claimed = false; /* true once some task has committed to
                                                   * doing (or has finished/failed) the
                                                   * one-time allocation below */
static portMUX_TYPE s_fs_last_run_data_mux = portMUX_INITIALIZER_UNLOCKED;

/* Maps a profile id (a user slot 0..PROFILES_MAX_COUNT-1, or a builtin id
 * PROFILE_BUILTIN_ID_BASE..+g_builtin_profile_count-1) to a cache slot.
 * Returns false for anything else (a stale/invalid/out-of-range id) -- such
 * an id is never cached and every lookup for it just falls back to a direct
 * firing_stats_load(), which is already correct, if not O(1). */
/* The index map below keys user slots by raw id, so the two id spaces must
 * not overlap -- a PROFILES_MAX_COUNT past PROFILE_BUILTIN_ID_BASE would
 * make builtin ids alias user slots silently. */
_Static_assert(PROFILES_MAX_COUNT <= PROFILE_BUILTIN_ID_BASE,
               "user profile ids must not reach into the builtin id space");
static bool fs_last_run_cache_index(uint8_t profile_id, size_t *out_idx)
{
    if (profile_id < PROFILES_MAX_COUNT) {
        *out_idx = profile_id;
        return true;
    }
    if (profile_id >= PROFILE_BUILTIN_ID_BASE) {
        size_t bi = (size_t)profile_id - (size_t)PROFILE_BUILTIN_ID_BASE;
        if (bi < g_builtin_profile_count) {
            *out_idx = (size_t)PROFILES_MAX_COUNT + bi;
            return true;
        }
    }
    return false;
}

/* One-time lazy allocation, safe under concurrent httpd_worker callers: the
 * first task to see !s_fs_last_run_alloc_claimed wins the (very short,
 * malloc-only, no NVS) allocation race under a spinlock that is released
 * before the allocation itself runs, so heap_caps_malloc() never executes
 * with interrupts disabled. Every other concurrent caller simply reports
 * "not ready yet" (returns false) for that one call and falls back to an
 * uncached direct load -- there is no blocking wait, since the allocation
 * finishes in well under a tick and the next call from that same request
 * loop (the very next profile id in the /api/profiles for-loop) will see it
 * published. */
static bool fs_last_run_cache_ensure_alloc(void)
{
    if (s_fs_last_run_cache != NULL) {
        return true;
    }
    bool i_am_allocator = false;
    portENTER_CRITICAL(&s_fs_last_run_alloc_mux);
    if (!s_fs_last_run_alloc_claimed) {
        s_fs_last_run_alloc_claimed = true;
        i_am_allocator = true;
    }
    portEXIT_CRITICAL(&s_fs_last_run_alloc_mux);
    if (!i_am_allocator) {
        return s_fs_last_run_cache != NULL;
    }

    size_t count = (size_t)PROFILES_MAX_COUNT + g_builtin_profile_count;
    size_t bytes = count * sizeof(uint32_t);
    uint32_t *buf = NULL;
    if (bytes > 1024) {
        buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) {
            ESP_LOGW(PE_TAG, "firing_stats last-run cache: %u B PSRAM alloc failed, falling back to "
                             "internal DRAM", (unsigned)bytes);
        }
    }
    if (buf == NULL) {
        buf = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        ESP_LOGE(PE_TAG, "firing_stats last-run cache: alloc(%u) failed -- caching disabled, every "
                         "GET /api/profiles will keep doing a full NVS read per profile",
                 (unsigned)bytes);
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        buf[i] = FS_LAST_RUN_CACHE_UNCACHED;
    }
    s_fs_last_run_cache_count = count;
    s_fs_last_run_cache = buf; /* publish last */
    return true;
}

/* Returns true and fills *out_started_unix_s from the cache on a hit; false
 * (cache miss, id uncacheable, or cache unavailable) otherwise -- caller
 * falls back to firing_stats_load() and should firing_stats_cache_store()
 * the result. No NVS access here; the spinlock below only ever guards a
 * plain array read. */
bool firing_stats_cache_lookup(uint8_t profile_id, uint32_t *out_started_unix_s)
{
    size_t idx;
    if (!fs_last_run_cache_index(profile_id, &idx)) {
        return false;
    }
    if (!fs_last_run_cache_ensure_alloc() || idx >= s_fs_last_run_cache_count) {
        return false;
    }
    uint32_t v;
    portENTER_CRITICAL(&s_fs_last_run_data_mux);
    v = s_fs_last_run_cache[idx];
    portEXIT_CRITICAL(&s_fs_last_run_data_mux);
    if (v == FS_LAST_RUN_CACHE_UNCACHED) {
        return false;
    }
    *out_started_unix_s = v;
    return true;
}

/* Fills (or corrects) this id's cache entry. Called from three places, all
 * in this file: the lazy-miss path (profile_executor_status.c, after a real
 * firing_stats_load()), firing_stats_persist() (a new run just became the
 * newest one), and firing_stats_erase() (history for this id is gone, so the
 * correct cached value is 0 -- the same value profile_executor_last_run_
 * started_unix_s() already returns for "never fired"). A no-op (not an
 * error) for an id fs_last_run_cache_index() does not recognize, or if the
 * cache failed to allocate -- there is nothing to update in either case. */
void firing_stats_cache_store(uint8_t profile_id, uint32_t started_unix_s)
{
    size_t idx;
    if (!fs_last_run_cache_index(profile_id, &idx)) {
        return;
    }
    if (!fs_last_run_cache_ensure_alloc() || idx >= s_fs_last_run_cache_count) {
        return;
    }
    portENTER_CRITICAL(&s_fs_last_run_data_mux);
    s_fs_last_run_cache[idx] = started_unix_s;
    portEXIT_CRITICAL(&s_fs_last_run_data_mux);
}

/* See profile_executor.h for why this exists (factory_reset.c erases the
 * whole profiles NVS partition without ever calling firing_stats_erase()).
 * Resets every slot to the UNCACHED sentinel so the next lookup for any id
 * does a real firing_stats_load() against whatever NVS now holds. No
 * allocation and no NVS here: if the cache was never allocated there is
 * nothing stale to drop. The loop runs under the data spinlock, which is
 * bounded by PROFILES_MAX_COUNT + g_builtin_profile_count plain stores. */
void firing_stats_cache_invalidate_all(void)
{
    if (s_fs_last_run_cache == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_fs_last_run_data_mux);
    for (size_t i = 0; i < s_fs_last_run_cache_count; i++) {
        s_fs_last_run_cache[i] = FS_LAST_RUN_CACHE_UNCACHED;
    }
    portEXIT_CRITICAL(&s_fs_last_run_data_mux);
}

/* Must be called with s_exec.lock held. Fills rec from the run currently (or
 * just-finished) in s_exec -- profile id/name/zone_mask/duration, then each
 * active zone's derived stats snapshot plus the PID gains in force right
 * now, so a later comparison against an older ring entry can tell whether a
 * re-tune happened between them. */
static void firing_stats_build_record(profile_firing_run_record_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    rec->profile_id = s_exec.profile_id;
    strncpy(rec->profile_name, s_exec.profile.name, sizeof(rec->profile_name) - 1);
    rec->run_started_unix_s = s_exec.run_started_unix_s;
    rec->duration_s = s_exec.total_elapsed_s;
    rec->zone_mask = s_exec.profile.zone_mask;

    float span = fabsf(s_exec.fs_target_max_c - s_exec.fs_target_min_c);
    if (isnan(s_exec.fs_target_min_c) || isnan(s_exec.fs_target_max_c)) {
        span = 0.0f; /* never ticked -- e.g. halted the instant it started */
    }

    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        zone_runtime_t *z = &s_exec.zones[zi];
        profile_firing_zone_record_t *zr = &rec->zones[zi];
        // An on/off zone has no target_c, no PID and no meaningful IAE --
        // report it as inactive here so no thermal-model statistic is ever
        // derived from it downstream (adaptive_tune, comparators). See
        // ON_OFF_ZONE_PLAN.md sec 1 "Firing stats / IAE" row.
        zr->active = z->active && !zone_is_on_off(zi) && !z->monitor_only;
        if (!zr->active) continue;
        firing_stats_snapshot(z, span, &zr->stats);
        if (z->control_mode == ZONE_CONTROL_MODE_PID || z->control_mode == ZONE_CONTROL_MODE_PID_FUZZY) {
            zr->kp = z->pid_cfg.kp;
            zr->ki = z->pid_cfg.ki;
            zr->kd = z->pid_cfg.kd;
        }
    }
}

/* NVS I/O only -- no s_exec, no lock. Safe to call from any task/state.
 * Loads FIRING_STATS_NVS_NAMESPACE/"fs_<id>" from profiles_nvs; a missing
 * key (never fired) is reported as an empty (count == 0) blob, not an
 * error -- that's the normal, common case for most profiles. Its own
 * return value already means exactly "is *out trustworthy" (true for a
 * genuine current/v1-migrated blob AND for the legitimate "never fired"
 * empty case; false only for actually-corrupt/unreadable data) -- reused
 * verbatim below as firing_stats_load()'s nvs_valid input to
 * firing_stats_cfg_fs_resolve(). */
static bool nvs_only_load(uint8_t profile_id, profile_firing_history_blob_t *out)
{
    memset(out, 0, sizeof(*out));
    char key[16];
    snprintf(key, sizeof(key), "fs_%u", (unsigned)profile_id);

    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, FIRING_STATS_NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY,
                                    FIRING_STATS_NVS_PARTITION);
    if (err != HAL_OK) {
        /* HAL_NOT_FOUND here means the namespace itself has never been
         * written to (no profile has ever finished a firing yet) --
         * expected on a fresh board, not worth logging. */
        return (err == HAL_NOT_FOUND);
    }
    /* Size-probe first (buf == NULL, hal_kv.h's documented two-call pattern)
     * so a length mismatch can be told apart from a read failure, and so an
     * old (but known) on-disk size can be migrated instead of discarded --
     * see PROFILE_FIRING_HISTORY_BLOB_SIZE_V1's doc comment in
     * profile_executor_internal.h. */
    size_t on_disk_len = 0;
    err = hal_kv_get_blob(&h, key, NULL, &on_disk_len);
    if (err == HAL_NOT_FOUND) {
        hal_kv_close(&h);
        memset(out, 0, sizeof(*out));
        return true; /* this profile has never fired -- not an error */
    }
    if (err != HAL_OK) {
        hal_kv_close(&h);
        ESP_LOGW(PE_TAG, "firing_stats_load(%u) failed: %s (size probe)", (unsigned)profile_id,
                 hal_status_to_name(err));
        memset(out, 0, sizeof(*out));
        return false;
    }

    if (on_disk_len == sizeof(*out)) {
        size_t len = sizeof(*out);
        err = hal_kv_get_blob(&h, key, out, &len);
        hal_kv_close(&h);
        if (err != HAL_OK || len != sizeof(*out)) {
            ESP_LOGW(PE_TAG, "firing_stats_load(%u) failed: %s (len %u/%u) -- discarding history",
                     (unsigned)profile_id, hal_status_to_name(err), (unsigned)len,
                     (unsigned)sizeof(*out));
            memset(out, 0, sizeof(*out));
            return false;
        }
        return true;
    }

    if (on_disk_len == PROFILE_FIRING_HISTORY_BLOB_SIZE_V1 && on_disk_len < sizeof(*out)) {
        /* Known-old layout, smaller than today's: read the old bytes into
         * the front of *out (already zeroed above) and leave the tail
         * zero-filled -- the tail-append migration this pass's mechanism
         * exists for. Unreachable today (PROFILE_FIRING_HISTORY_BLOB_SIZE_V1
         * == sizeof(*out) until a field is actually added), exercised only
         * by the host tests' synthetic short blob. */
        size_t len = on_disk_len;
        err = hal_kv_get_blob(&h, key, out, &len);
        hal_kv_close(&h);
        if (err != HAL_OK || len != on_disk_len) {
            ESP_LOGW(PE_TAG,
                     "firing_stats_load(%u) failed: %s (len %u/%u) migrating v1 blob -- discarding history",
                     (unsigned)profile_id, hal_status_to_name(err), (unsigned)len,
                     (unsigned)on_disk_len);
            memset(out, 0, sizeof(*out));
            return false;
        }
        ESP_LOGW(PE_TAG,
                 "firing_stats_load(%u): migrated version-1 blob (%u B) to current layout (%u B), "
                 "tail zero-filled", (unsigned)profile_id, (unsigned)on_disk_len,
                 (unsigned)sizeof(*out));
        return true;
    }

    hal_kv_close(&h);
    ESP_LOGW(PE_TAG,
             "firing_stats_load(%u) failed: on-disk size %u matches neither current (%u) nor known "
             "prior (%u) layout -- discarding history", (unsigned)profile_id, (unsigned)on_disk_len,
             (unsigned)sizeof(*out), (unsigned)PROFILE_FIRING_HISTORY_BLOB_SIZE_V1);
    memset(out, 0, sizeof(*out));
    return false;
}

/* cfg-filesystem read-through wrapper (docs/FILESYSTEM_USER_DATA_PLAN.md
 * section 5 item 7, "firing stats / history") -- public entry point,
 * unchanged signature/contract (requirement 2: existing callers keep
 * working unchanged). Loads the NVS side exactly as before via
 * nvs_only_load() (including its tail-append v1 migration tolerance),
 * then lets firing_stats_cfg_fs_resolve() decide whether the file or NVS
 * side actually wins -- see that module's header comment for the full
 * divergence table. This NEVER discards firing history: the resolve either
 * adopts NVS outright (file missing/corrupt) or, when NVS itself is
 * untrustworthy but the file decodes, adopts the file instead of falling
 * back to empty -- there is no path here that zeroes real history the way
 * a bare size-mismatch NVS read used to (see the VERSIONLESS HAZARD comment
 * in profile_executor_internal.h for that pre-existing NVS-only hazard,
 * which this pass does not widen). */
bool firing_stats_load(uint8_t profile_id, profile_firing_history_blob_t *out)
{
    /* HEAP, not the stack (2026-09-08 panic, docs/audits/firing_history_
     * stack_overflow_2026-09-08.md): this 1364 B blob was one of four
     * nested copies on the httpd_worker stack reached from
     * GET /api/firing_history. Internal DRAM -- this path touches NVS. */
    profile_firing_history_blob_t *nvs_blob =
        heap_caps_malloc(sizeof(*nvs_blob), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (nvs_blob == NULL) {
        ESP_LOGE(PE_TAG, "firing_stats_load(%u): malloc(%u) failed", (unsigned)profile_id,
                 (unsigned)sizeof(*nvs_blob));
        memset(out, 0, sizeof(*out));
        return false;
    }
    bool nvs_valid = nvs_only_load(profile_id, nvs_blob);

    uint32_t nvs_rev = firing_stats_cfg_fs_read_rev(profile_id);
    uint32_t resolved_rev = nvs_rev;
    bool used_file = false;
    bool have_value =
        firing_stats_cfg_fs_resolve(profile_id, nvs_blob, nvs_valid, nvs_rev, out, &resolved_rev, &used_file);
    free(nvs_blob);
    if (!have_value) {
        memset(out, 0, sizeof(*out));
        // nvs_valid is only false here for genuinely corrupt/unrecognized
        // NVS data (nvs_only_load()'s own "never fired" case already sets
        // nvs_valid true on an empty blob, so resolve() would have taken
        // the "adopt NVS" branch above instead of reaching here) -- preserve
        // firing_stats_load()'s pre-existing false-on-corruption contract
        // for callers that check it (profile_executor_status.c).
        return nvs_valid;
    }
    if (used_file) {
        ESP_LOGD(PE_TAG, "firing_stats_load(%u): loaded from cfg filesystem (rev=%lu)", (unsigned)profile_id,
                 (unsigned long)resolved_rev);
    }
    return true;
}

/* GET /api/cfgfs dual-write picture for the firing_stats_cfg_fs.c bridge --
 * 2026-09-08, moving this item's reporting out of cfg_fs_status.c's stale
 * "nvs_only" hardcoded list (docs/FILESYSTEM_USER_DATA_PLAN.md item 7,
 * landed in 762bb29e). This bridge is keyed per PROFILE ID, unlike every
 * other item this endpoint already reports one row for -- there is no
 * fixed, enumerable set of ids (a profile can be any user slot 0..
 * PROFILES_MAX_COUNT-1 OR a 3-digit builtin id, and only ids that have
 * actually fired ever get a key at all), so a byte-for-byte content compare
 * across "every id that could possibly exist" is not a well-defined
 * operation the way it is for the other bridges' single fixed-size blobs.
 *
 * SCOPE (documented gap, not silently assumed complete): this checks only
 * the user profile slots 0..PROFILES_MAX_COUNT-1 -- the same id range
 * cfgfs_status_get_handler() already itemizes separately for the profiles'
 * own config rows ("profile0".."profile7") -- not any builtin 3-digit id.
 * A divergence confined to a builtin profile's firing history would not be
 * caught here. Widening this to enumerate every id actually on disk would
 * need a directory walk over "stats/" (cfg_fs_list()) cross-referenced
 * against every "fsr_<id>" NVS key with no equivalent listing API on the
 * NVS side -- out of scope for restoring accurate /api/cfgfs reporting,
 * which is this pass's job.
 *
 * One row, aggregated: `file_valid`/`nvs_valid` are true if ANY covered id
 * has a valid side; `file_rev`/`nvs_rev` report the MAX rev seen (purely
 * informational -- unlike the per-item revs elsewhere, this is not one
 * blob's single counter); `diverged` is true if ANY covered id's file and
 * NVS copies disagree in content. Read-only throughout: nvs_only_load() (in
 * this same file, static) and firing_stats_cfg_fs_load_raw() are both pure
 * reads, no resolve/resync call anywhere in this function, matching every
 * sibling *_get_dualwrite_status(). */
void firing_stats_get_dualwrite_status_ex(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                           bool *diverged, bool *nvs_stale)
{
    if (nvs_stale) {
        *nvs_stale = false;
    }
    if (file_valid) {
        *file_valid = false;
    }
    if (file_rev) {
        *file_rev = 0;
    }
    if (nvs_valid) {
        *nvs_valid = false;
    }
    if (nvs_rev) {
        *nvs_rev = 0;
    }
    if (diverged) {
        *diverged = false;
    }

    bool any_file_valid = false, any_nvs_valid = false, any_diverged = false, any_stale = false;
    uint32_t max_file_rev = 0, max_nvs_rev = 0;

    /* 2026-10-04: now PSRAM first via persist_scratch_alloc(): this read-only
     * path held 2 x 1364 B of internal RAM across the 100-slot loop on every
     * GET /api/cfgfs. History: 2026-09-08 (httpd_worker stack-budget pass)
     * moved these off the stack onto the heap; this was two
     * profile_firing_history_blob_t (1364 B each) as stack locals inside the
     * loop -- the same "four copies of a 1364 B
     * blob" class eb92592c already fixed for the firing-history handler
     * itself, here as two copies in this dualwrite-status helper, which
     * `/api/cfgfs` (diagnostics_http.c's cfgfs_status_get_handler) and the
     * setup wizard's poll both reach. Read-only comparison, no flash write
     * in this function, so internal vs. PSRAM is not safety-critical here
     * (the 2026-09-08 pass used internal DRAM to match its siblings; that
     * choice is superseded by the PSRAM-first allocation above). One
     * allocation reused across every loop iteration, freed once after the loop (this
     * function's only exit path). An allocation failure degrades to
     * reporting the honest "nothing valid, nothing diverged" defaults
     * already set above, rather than a stack overflow. */
    profile_firing_history_blob_t *f_blob = persist_scratch_alloc(sizeof(*f_blob));
    profile_firing_history_blob_t *n_blob = persist_scratch_alloc(sizeof(*n_blob));
    if (!f_blob || !n_blob) {
        free(f_blob);
        free(n_blob);
        return;
    }

    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        uint32_t f_rev = 0;
        bool f_valid = false;
        firing_stats_cfg_fs_load_raw(id, f_blob, &f_rev, &f_valid);

        bool n_valid = nvs_only_load(id, n_blob);
        uint32_t n_rev = firing_stats_cfg_fs_read_rev(id);

        bool content_equal = f_valid && n_valid && memcmp(f_blob, n_blob, sizeof(*f_blob)) == 0;
        if (cfg_fs_status_item_diverged(f_valid, n_valid, content_equal)) {
            // Saves are cfg-file-only since the dual-write close, so a file
            // with a strictly higher rev and different content is the legacy
            // NVS copy going stale, not a divergence (cfg_fs_status.h).
            if (cfg_fs_status_item_nvs_stale(true, f_rev, n_rev)) {
                any_stale = true;
            } else {
                any_diverged = true;
            }
        }
        if (f_valid) {
            any_file_valid = true;
            if (f_rev > max_file_rev) {
                max_file_rev = f_rev;
            }
        }
        if (n_valid) {
            any_nvs_valid = true;
            if (n_rev > max_nvs_rev) {
                max_nvs_rev = n_rev;
            }
        }
    }
    free(f_blob);
    free(n_blob);

    if (file_valid) {
        *file_valid = any_file_valid;
    }
    if (file_rev) {
        *file_rev = max_file_rev;
    }
    if (nvs_valid) {
        *nvs_valid = any_nvs_valid;
    }
    if (nvs_rev) {
        *nvs_rev = max_nvs_rev;
    }
    if (diverged) {
        *diverged = any_diverged;
    }
    if (nvs_stale) {
        *nvs_stale = any_stale;
    }
}

void firing_stats_get_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid, uint32_t *nvs_rev,
                                        bool *diverged)
{
    firing_stats_get_dualwrite_status_ex(file_valid, file_rev, nvs_valid, nvs_rev, diverged, NULL);
}

/* Persists rec as the newest entry for its own profile_id -- read-modify-
 * write against profiles_nvs, called from whichever task ended the run
 * (see this section's top comment for why that's always safe here). A
 * failure is logged and otherwise swallowed: losing one run's history is
 * not worth failing the run itself over, matching relay_cycles_flush()'s
 * and run_state_note()'s own non-fatal convention. */
/* True iff the CURRENTLY EXECUTING task's own stack lives in external RAM
 * (PSRAM). Same predicate, same reasoning, and same incident class as
 * kiln_cfg_store.c's/safety_cfg_store.c's/run_state.c's/relay_cycles.c's
 * caller_stack_is_external(): a flash/NVS write disables the cache, which
 * makes a PSRAM-resident stack unreachable and aborts the whole board via
 * ESP-IDF's own esp_task_stack_is_sane_cache_disabled() rather than failing
 * just this one call. firing_stats_persist() is called directly from
 * profile_executor.c's tick and halt paths -- see run_state.c's identical
 * guard for the fuller rationale; this closes the same gap for this
 * module. */
static bool caller_stack_is_external(void)
{
    return !hal_kv_write_safe_here();
}

void firing_stats_persist(const profile_firing_run_record_t *rec)
{
    if (caller_stack_is_external()) {
        ESP_LOGE(PE_TAG, "firing_stats_persist: REFUSING -- calling task's stack is in external "
                         "RAM (PSRAM). A flash/NVS write from here would abort the whole board "
                         "(ESP-IDF's esp_task_stack_is_sane_cache_disabled()). Route this call "
                         "through a task with an internal-SRAM stack instead -- see "
                         "DRAM_PSRAM_PLAN.md section 7.2 and uart_bridge_ext.c's flash-safe "
                         "worker for the established pattern.");
        // "Reset one side of a pair" class (2026-09-24 review advisory):
        // firing_shadow_finish_firing() is never reached below when this
        // guard refuses, so without this call the shadow module's
        // still-open per-zone segment state from THIS firing would silently
        // carry into the next one's first ticks. firing_shadow_abandon_
        // firing() is RAM-only (no NVS), so it is safe to call from this
        // PSRAM-stacked task even though the rest of this function is not.
        firing_shadow_abandon_firing();
        return;
    }

    /* Shadow mode (ITER_TUNE_REDESIGN_PLAN.md step 8): finishes this firing's
     * in-progress score set, compares it against the previous finished
     * firing (RAM only), and persists a compact verdict-summary NVS blob of
     * its own -- relies on the caller_stack_is_external() guard just above
     * (already refused and returned by this point if unsafe), so it does no
     * stack check of its own. Runs before this function's own NVS work
     * below so a failure in either is independent of the other. Never
     * writes a gain, never calls any iter_tune_* function. */
    firing_shadow_finish_firing();

    /* HEAP, not the executor task's 4096 B stack (2026-09-09 panic,
     * docs/audits/executor_panic_stack_overflow_2026-09-09.md): this is the
     * WRITE-path twin of the same 1364 B blob that firing_stats_load() above
     * already heap-allocates -- the 2026-09-08 fix moved only the read path
     * (httpd_worker) off the stack; this call, from executor_task_entry()'s
     * run-end sequence, was left on the smaller stack and overflowed it one
     * day later. Internal DRAM -- this path touches NVS/flash. */
    profile_firing_history_blob_t *blob = heap_caps_malloc(sizeof(*blob), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (blob == NULL) {
        ESP_LOGE(PE_TAG, "firing_stats_persist(%u): malloc(%u) failed -- this run's history was not saved",
                 (unsigned)rec->profile_id, (unsigned)sizeof(*blob));
        return;
    }
    firing_stats_load(rec->profile_id, blob); /* empty blob on any failure -- still safe to prepend into */

    uint8_t keep = (blob->count < PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH)
                       ? blob->count
                       : (PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH - 1);
    if (keep > 0) {
        memmove(&blob->runs[1], &blob->runs[0], keep * sizeof(blob->runs[0]));
    }
    blob->runs[0] = *rec;
    blob->count = keep + 1;

    // FILE ONLY (docs/CONFIG_FILESYSTEM.md "NVS dual-write closed"): the
    // verified cfg-file write is the only persistence. A failure is logged
    // loudly and this run's history is NOT recorded (no cache update). Rev is
    // max(file rev, legacy NVS rev) + 1, read fresh (this function can run
    // from the executor task or an operator halt() on another task, so no
    // shared RAM copy), so a board upgraded from dual-write firmware keeps
    // the file strictly above its stale NVS blob.
    uint32_t cur_rev = firing_stats_cfg_fs_read_file_rev(rec->profile_id);
    uint32_t nvs_rev = firing_stats_cfg_fs_read_rev(rec->profile_id);
    uint32_t new_rev = (cur_rev > nvs_rev ? cur_rev : nvs_rev) + 1;
    esp_err_t file_err = firing_stats_cfg_fs_save(rec->profile_id, blob, new_rev);
    if (file_err != ESP_OK) {
        ESP_LOGE(PE_TAG, "firing_stats_persist(%u): cfg file write failed: %s -- this run's history was not saved",
                 (unsigned)rec->profile_id, esp_err_to_name(file_err));
        free(blob);
        return;
    }
    ESP_LOGI(PE_TAG, "firing stats persisted for profile %u (%s), %u/%u history entries",
             (unsigned)rec->profile_id, rec->profile_name, (unsigned)blob->count,
             (unsigned)PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH);
    // The verified file write just succeeded (every earlier failure returned),
    // so this run is now genuinely the newest one on disk -- update the
    // last-run cache to match. See this file's "last-run-started RAM cache"
    // section above.
    firing_stats_cache_store(rec->profile_id, rec->run_started_unix_s);
    free(blob);
}

/* docs/PROFILE_SLOTS_100_PLAN.md section 7 task 10: erases this id's
 * firing-history blob ("fs_<id>", profiles_nvs/fire_stats) plus the
 * cfg-filesystem mirror and its rev key (firing_stats_cfg_fs_delete()) --
 * called from profiles_http.c's nvs_erase_slot() when a profile SLOT is
 * deleted, so a later profile created at the same id never inherits the
 * previous occupant's runs. Best-effort like every other half of a slot
 * delete (profiles_cfg_fs_delete() one line above this call site in
 * profiles_http.c): a failure here is logged and swallowed, never turned
 * into a failed slot delete. Safe (a clean no-op) for an id that never
 * fired -- HAL_NOT_FOUND on the NVS erase is treated as success, matching
 * nvs_erase_slot()'s own hal_kv_erase_key() call. Checks
 * caller_stack_is_external() itself (check_nvs_write_guard_coverage.ps1
 * requires every NVS-writing function in this file to, not just its
 * callers) -- defense in depth, same reasoning as firing_stats_persist()
 * above: today's only caller (nvs_erase_slot(), on the httpd_worker path)
 * already guards before reaching here, but a future caller on a
 * PSRAM-stacked task must not be able to bypass this by skipping that
 * caller's own check. */
esp_err_t firing_stats_erase(uint8_t profile_id)
{
    if (caller_stack_is_external()) {
        ESP_LOGE(PE_TAG, "firing_stats_erase: REFUSING -- calling task's stack is in external "
                         "RAM (PSRAM). A flash/NVS write from here would abort the whole board "
                         "(ESP-IDF's esp_task_stack_is_sane_cache_disabled()). Route this call "
                         "through a task with an internal-SRAM stack instead -- see "
                         "DRAM_PSRAM_PLAN.md section 7.2 and uart_bridge_ext.c's flash-safe "
                         "worker for the established pattern.");
        return ESP_ERR_INVALID_STATE;
    }
    // ERASE-FIRST: the legacy "fs_<id>" NVS blob goes first and is checked; on
    // failure nothing else is touched (the file stays) and the error is
    // returned, so the NVS fallback can never resurrect a half-deleted history.
    char key[16];
    snprintf(key, sizeof(key), "fs_%u", (unsigned)profile_id);
    hal_kv_handle_t h;
    hal_status_t err =
        hal_kv_open(&h, FIRING_STATS_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, FIRING_STATS_NVS_PARTITION);
    if (err != HAL_OK) {
        ESP_LOGW(PE_TAG, "firing_stats_erase(%u): hal_kv_open failed: %s", (unsigned)profile_id,
                 hal_status_to_name(err));
        return hal_status_to_esp_err(err);
    }
    hal_status_t erase_err = hal_kv_erase_key(&h, key);
    if (erase_err != HAL_OK && erase_err != HAL_NOT_FOUND) {
        ESP_LOGW(PE_TAG, "firing_stats_erase(%u): erase failed: %s", (unsigned)profile_id,
                 hal_status_to_name(erase_err));
        hal_kv_close(&h);
        return hal_status_to_esp_err(erase_err);
    }
    err = hal_kv_commit(&h);
    hal_kv_close(&h);
    if (err != HAL_OK) {
        ESP_LOGW(PE_TAG, "firing_stats_erase(%u): commit failed: %s", (unsigned)profile_id,
                 hal_status_to_name(err));
        return hal_status_to_esp_err(err);
    }

    esp_err_t file_err = firing_stats_cfg_fs_delete(profile_id);
    if (file_err != ESP_OK) {
        ESP_LOGW(PE_TAG, "firing_stats_erase(%u): cfg-fs delete failed: %s", (unsigned)profile_id,
                 esp_err_to_name(file_err));
        return file_err;
    }

    // The legacy NVS keys and the file are both gone (or were already empty),
    // so 0 ("never fired") is the correct cached value, matching what a fresh
    // firing_stats_load() would now report. Reached only on full success: every
    // failure above returned early and left the cache alone. See this file's
    // "last-run-started RAM cache" section.
    firing_stats_cache_store(profile_id, 0);
    return ESP_OK;
}

/* Called from the tick loop's non-RUNNING branch (DONE/FAULTED) and from
 * profile_executor_halt() (an operator stop out of RUNNING/PAUSED, which
 * never passes through that tick-loop transition). s_exec.fs_persisted
 * makes a second call for the same run a no-op -- halt() dismissing an
 * already-DONE/FAULTED run must not persist twice. Must be called with
 * s_exec.lock held; the actual NVS write happens after the caller releases
 * the lock (matching capture_run_snapshot()'s split), via the record this
 * writes into *out_rec and the bool it returns. */
bool firing_stats_maybe_finalize(profile_firing_run_record_t *out_rec)
{
    if (s_exec.fs_persisted || s_exec.state == PROFILE_EXEC_IDLE) {
        return false;
    }
    firing_stats_build_record(out_rec);
    s_exec.fs_persisted = true;
    return true;
}

/* ---- reboot breadcrumb (TODO.md 6A.3, "No auto-resume across reboot") ------
 *
 * The relays-come-up-off half of that bullet is kiln_io_init()'s latch
 * ordering and is untouched here. This is the other half: a persisted
 * description of what was running and how far it got, so that after a
 * brownout the operator can tell "I stopped it" from "the power went out".
 * run_state.c owns the flash side, including the write cadence; this file
 * only decides WHEN a transition is worth recording.
 *
 * Nothing in this file ever reads that record back. There is deliberately no
 * path from a stored record to profile_executor_run() -- see run_state.h. */


/* Copies the live run into a caller-owned buffer so the NVS write can happen
 * without pinning s_exec.lock across it wherever that's practical. Must be
 * called with s_exec.lock held. */
void capture_run_snapshot(run_snapshot_buf_t *b)
{
    memset(b, 0, sizeof(*b));
    strncpy(b->name, s_exec.profile.name, sizeof(b->name) - 1);
    strncpy(b->reason, s_exec.fault_reason, sizeof(b->reason) - 1);

    /* On the DONE path segment_index has already stepped one past the last
     * segment (that step is what ends the run). Clamp it, or the breadcrumb
     * reads "segment 6 / 5" -- a detail nobody would trust the rest of the
     * record after seeing. */
    uint8_t seg = s_exec.segment_index;
    if (s_exec.profile.segment_count > 0 && seg >= s_exec.profile.segment_count) {
        seg = (uint8_t)(s_exec.profile.segment_count - 1u);
    }

    b->snap.profile_id = s_exec.profile_id;
    b->snap.profile_name = b->name;
    b->snap.zone_mask = s_exec.profile.zone_mask;
    b->snap.segment_index = seg;
    b->snap.segment_count = s_exec.profile.segment_count;
    b->snap.dwelling = s_exec.dwelling;
    b->snap.target_c = s_exec.target_c;
    b->snap.segment_elapsed_s = s_exec.segment_elapsed_s;
    b->snap.fault_guard = (uint8_t)s_exec.fault_guard;
    b->snap.fault_reason = b->reason;
}
