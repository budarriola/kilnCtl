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
#include <string.h>

#include "esp_log.h"

#include "hal_kv.h"
#include "hal_esp_common.h" /* hal_status_to_esp_err() -- keeps esp_err_to_name() below meaningful */
#include "nvs_key_check.h"

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
    if (!z->actual_valid) {
        z->fs_excluded_sample_count++;
        return;
    }
    float error = z->actual_c - target_c;
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
        zr->active = z->active;
        if (!z->active) continue;
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
 * error -- that's the normal, common case for most profiles. */
bool firing_stats_load(uint8_t profile_id, profile_firing_history_blob_t *out)
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
    size_t len = sizeof(*out);
    err = hal_kv_get_blob(&h, key, out, &len);
    hal_kv_close(&h);
    if (err == HAL_NOT_FOUND) {
        memset(out, 0, sizeof(*out));
        return true; /* this profile has never fired -- not an error */
    }
    if (err != HAL_OK || len != sizeof(*out)) {
        ESP_LOGW(PE_TAG, "firing_stats_load(%u) failed: %s (len %u/%u)", (unsigned)profile_id,
                 hal_status_to_name(err), (unsigned)len, (unsigned)sizeof(*out));
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
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
        return;
    }
    profile_firing_history_blob_t blob;
    firing_stats_load(rec->profile_id, &blob); /* empty blob on any failure -- still safe to prepend into */

    uint8_t keep = (blob.count < PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH)
                       ? blob.count
                       : (PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH - 1);
    if (keep > 0) {
        memmove(&blob.runs[1], &blob.runs[0], keep * sizeof(blob.runs[0]));
    }
    blob.runs[0] = *rec;
    blob.count = keep + 1;

    char key[16];
    snprintf(key, sizeof(key), "fs_%u", (unsigned)rec->profile_id);
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, FIRING_STATS_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE,
                                    FIRING_STATS_NVS_PARTITION);
    if (err != HAL_OK) {
        ESP_LOGE(PE_TAG, "firing_stats_persist(%u): hal_kv_open failed: %s", (unsigned)rec->profile_id,
                 hal_status_to_name(err));
        return;
    }
    err = hal_kv_set_blob(&h, key, &blob, sizeof(blob));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    if (err != HAL_OK) {
        ESP_LOGE(PE_TAG, "firing_stats_persist(%u): write failed: %s", (unsigned)rec->profile_id,
                 hal_status_to_name(err));
    } else {
        ESP_LOGI(PE_TAG, "firing stats persisted for profile %u (%s), %u/%u history entries",
                 (unsigned)rec->profile_id, rec->profile_name, (unsigned)blob.count,
                 (unsigned)PROFILE_EXECUTOR_FIRING_HISTORY_DEPTH);
    }
    hal_kv_close(&h);
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
