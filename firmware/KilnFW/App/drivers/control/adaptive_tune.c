// adaptive_tune.c -- PID_EXPANSION_PLAN.md Phase 7d: refine a zone's
// steady-state gain (K_dc) from the settled DWELLS every ordinary firing
// already produces, and, if the operator has opted in, quietly recompute
// that zone's PID gains from the refined model through the SAME SIMC path
// autotune's Accept uses (pid_autotune_tune_from_fopdt()). See adaptive_
// tune.h for the full scope note.
//
// 2026-09-01 split (see adaptive_tune_internal.h's own top comment for the
// full shape): this file now owns lock/state ownership, Layer 1 (zone_tick,
// the settle/observation/joint-cache bookkeeping), the adaptive_tune_run_
// end() dispatcher, opt-in flag + Ki-baseline NVS persistence, adaptive_
// tune_init(), and the public accessors. The diagonal/coupled model refine
// moved to adaptive_tune_model.c, the Ki diagnosis to adaptive_tune_ki.c.
//
// Safety: every write this module makes happens at profile_executor.c's
// run-end (adaptive_tune_run_end()), AFTER the firing's relays are already
// off and the zone is no longer active -- never mid-firing, so there is no
// live control loop for a gain change to bump. See that function's own
// comment for why this makes seed_bumpless_with_ff() unnecessary here (it
// remains the right tool if a future pass ever needs a mid-run apply).
//
// Persistence: the LEARNED MODEL (K_dc) and the PID gains derived from it
// are written through the existing, already-validated setters (zones_
// config_set_model()/set_pid()) into the ordinary zone config blob -- the
// same place autotune's Accept path writes, so a reader cannot tell a
// learned gain from a hand-tuned or autotuned one, which is the point. The
// per-zone OPT-IN FLAG and the Ki-diagnosis BASELINE (P1, see ki_baseline's
// own comment in adaptive_tune_internal.h) live in a SEPARATE NVS namespace
// owned by this file (ADAPTIVE_TUNE_NVS_NAMESPACE), NOT as new fields on the
// zone config blob: zones_http.c/zones_config_accessors.c (the zone config
// blob's owner) were off-limits for this pass (another agent holds them
// live), so a field there was not an option.
//
// NVS writes are always dispatched through uart_bridge_ext_run_on_flash_
// worker() below -- never called directly from a PSRAM-stacked task, which
// panics on a flash write every time (see project_psram_stack_nvs_panic and
// this file's declaration of that function further down). Reads are not
// routed through it -- reading is not the hazard, only writing is (same
// distinction safety_cfg_store.c's own comments draw), and this module's
// boot-time reads (adaptive_tune_init()) run once from app_main's task,
// which is not PSRAM-stacked.
#include "adaptive_tune_internal.h"
#include "cfgfs_file_validators.h"

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h" /* EXT_RAM_BSS_ATTR -- adaptive_tune_zones[] below, moved to PSRAM 2026-09-30
                        * to pay for lvgl's stack bump (see lvgl_port.c). Safe: every access to this
                        * array -- the zone_tick/run_end hot path, init/load_enable_flags,
                        * set_enabled/clear_ki_baseline, the getters (including the HTTP status
                        * read in adaptive_tune_http.c), revert, the model/Ki-diagnosis helpers in
                        * adaptive_tune_ki.c/adaptive_tune_model.c, and the autotune accept path --
                        * runs in ordinary task context under adaptive_tune_lock, never an ISR or
                        * IRAM code. Every flash/NVS/cfg persistence path copies through a separate
                        * local or job blob rather than passing this array itself as a buffer, so
                        * it is never a DMA target and never touched with the flash cache disabled.
                        * This is not a closed enumeration -- any future caller must keep taking
                        * adaptive_tune_lock and must not hand this array to a DMA/ISR path. */
#include "esp_log.h"

#include "hal_kv.h"
#include "hal_esp_common.h" /* hal_status_to_esp_err() -- preserve the specific esp_err_t
                              * save_kibase_job()'s caller (the flash worker) already branches on */
#include "cfg_fs_status.h" /* cfg_fs_status_item_diverged() -- adaptive_tune_get_kibase_dualwrite_status() below */
#include "flash_worker_wait.h" /* bounded wait for the flash-safe worker -- see adaptive_tune_init()'s
                                 * kibase resolve call site below and flash_worker_wait.h's header comment */
#include "pref_cfg_fs.h" /* cfg-filesystem dual-write bridge, docs/FILESYSTEM_USER_DATA.md
                             section 5 item 9 -- see adaptive_tune_internal.h's
                             ADAPTIVE_TUNE_KIBASE_FILE_PATH comment for the simplified
                             (re-derivable) treatment this item gets. */
#include "profile_executor.h"

#include "pid_fuzzy_confidence.h" // PID_FUZZY_CONFIDENCE_MAX_C -- ADAPTIVE_FUZZY_EVALUATION.md sec 3
#include "zones_config_accessors.h" // zones_config_get/set_adaptive_tune_enabled/get_pid/set_pid/get_model/set_model --
                         // this file now writes the opt-in flag here too (U2) and reads/writes
                         // gains directly for adaptive_tune_revert() (U1)

const char *ADAPTIVE_TUNE_TAG = "adaptive_tune";

EXT_RAM_BSS_ATTR adaptive_tune_zone_t adaptive_tune_zones[MAX31856_CHANNEL_COUNT];

/* Set true if adaptive_tune_init()'s boot-time kibase migrate-on-load write
 * (pref_cfg_fs_resolve() below) was attempted before the flash-safe worker
 * existed and the bounded wait gave up. Same hazard and same fix pattern as
 * relay_cycles.c's identical flag -- see flash_worker_wait.h. Surfaced via
 * adaptive_tune_get_kibase_dualwrite_status() into GET /api/cfgfs. */
static bool s_kibase_migration_worker_wait_deferred = false;

// ---------------------------------------------------------------------
// Joint (all-zone) dwell observations for the coupled solve -- module-wide,
// not per-zone, because a "joint observation" is every zone's duty/rise at
// the SAME instant. Guarded by the same adaptive_tune_lock as adaptive_tune_zones[] above.
// ---------------------------------------------------------------------

adaptive_tune_joint_obs_t adaptive_tune_joint_ring[ADAPTIVE_TUNE_JOINT_RING_CAPACITY];
uint32_t adaptive_tune_joint_ring_count;
uint32_t adaptive_tune_joint_ring_head;
uint32_t adaptive_tune_joint_observations_lifetime;

// True once a joint row has been committed for the CURRENT dwell -- reset
// whenever any zone begins a fresh dwell (adaptive_tune_zone_tick()'s "just
// entered this dwell" branch). Every enabled zone settles independently
// (each has its own settle_start_c/settle_elapsed_s), so with N zones
// dwelling at the same operating point, each one crossing its settle floor
// used to commit its OWN joint row from the same adaptive_tune_joint_last_duty/rise_c
// snapshot -- one physical dwell (one distinct operating point) silently
// contributing N near-identical rows to the ring. adaptive_tune_refine_coupled_
// locked()'s "N joint observations" floor is a rank/conditioning
// requirement on DISTINCT equations; duplicate rows inflate the count
// without adding one. Gating the commit on this flag makes ring rows and
// distinct dwells the same number again, so the existing floor check is
// correct without a second counter. This assumes every enabled zone shares
// the same profile segment boundaries (true for this firmware -- all zones
// in a firing follow the same profile), so "any zone enters a fresh dwell"
// is a reasonable proxy for "a new dwell has begun" module-wide.
bool adaptive_tune_joint_dwell_row_committed;

// Latest known duty/rise for every zone, updated on EVERY tick for EVERY
// zone regardless of that zone's own opt-in flag -- a zone that has not
// opted its own row into learning is still a valid NEIGHBOUR column in
// another zone's coupled row, so its duty must still be tracked. Frozen
// (not reset to 0/NaN) on an invalid tick, same "hold last value" posture
// as zone_coupling_filter_tick() -- a stale value sitting unused until the
// next valid tick does no harm; only last_valid gates whether it is ever
// read.
float adaptive_tune_joint_last_duty[MAX31856_CHANNEL_COUNT];
float adaptive_tune_joint_last_rise_c[MAX31856_CHANNEL_COUNT];
bool  adaptive_tune_joint_last_valid[MAX31856_CHANNEL_COUNT];
SemaphoreHandle_t adaptive_tune_lock; // guards adaptive_tune_zones; taken only from this file, never across profile_executor.c's
                          // s_exec.lock (see adaptive_tune.h's doc comment on the lock order this keeps)
bool adaptive_tune_lock_ready;

void adaptive_tune_ensure_lock(void)
{
    if (!adaptive_tune_lock_ready) {
        adaptive_tune_lock = xSemaphoreCreateMutex();
        adaptive_tune_lock_ready = true;
    }
}

void adaptive_tune_set_refusal(adaptive_tune_zone_t *z, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(z->last_refusal_reason, sizeof(z->last_refusal_reason), fmt, ap);
    va_end(ap);
    ESP_LOGI(ADAPTIVE_TUNE_TAG, "refined not applied: %s", z->last_refusal_reason);
}

void adaptive_tune_set_reason(char *buf, size_t bufsz, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, bufsz, fmt, ap);
    va_end(ap);
    ESP_LOGI(ADAPTIVE_TUNE_TAG, "%s", buf);
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
    adaptive_tune_ensure_lock();
    xSemaphoreTake(adaptive_tune_lock, portMAX_DELAY);
    adaptive_tune_zone_t *z = &adaptive_tune_zones[zone_index];

    // Joint duty/rise cache: updated for EVERY zone on EVERY tick,
    // regardless of that zone's own opt-in flag -- see adaptive_tune_joint_last_duty[]'s
    // own comment above. Deliberately BEFORE the enabled/actual_valid/
    // dwelling gates below: an opted-out zone, or one whose own reading is
    // bad this instant, must still contribute its latest-known duty as a
    // neighbour column for another zone's coupled row.
    if (actual_valid && !isnan(ambient_c) && isfinite(actual_c) && isfinite(ambient_c)) {
        adaptive_tune_joint_last_duty[zone_index] = duty;
        adaptive_tune_joint_last_rise_c[zone_index] = actual_c - ambient_c;
        adaptive_tune_joint_last_valid[zone_index] = true;
    } else {
        adaptive_tune_joint_last_valid[zone_index] = false;
    }

    // F3 fix: dwell-transition bookkeeping (dwelling_prev, and everything
    // that resets on a fresh dwell entry) runs UNCONDITIONALLY here, before
    // the enabled/actual_valid/ambient gates below -- same "before the
    // gates" posture as the joint cache update above, and for the same
    // reason. Previously dwelling_prev was only ever touched inside the
    // gated branches, which left two holes (see the review's F3 finding):
    //   (a) a zone whose first dwelling tick(s) have !actual_valid or a NaN
    //       ambient hit the early return below without ever setting
    //       dwelling_prev true. When a later tick in the SAME dwell finally
    //       had good data, dwelling_prev was still false, so THAT tick was
    //       misread as "just entered this dwell" -- re-clearing
    //       adaptive_tune_joint_dwell_row_committed and admitting a second joint row
    //       from one physical dwell.
    //   (b) identically for a zone that is enabled mid-dwell: every tick
    //       before it was enabled returned early (via the old !z->enabled
    //       check) without updating dwelling_prev, so the first tick after
    //       enabling hit the same false "fresh dwell" branch.
    // Tracking the raw dwelling transition here, independent of data
    // validity and the opt-in flag, makes "just entered this dwell" true
    // exactly once per physical dwell, regardless of what the gates below
    // do with any given tick's data.
    bool dwell_just_entered = dwelling && !z->dwelling_prev;
    z->dwelling_prev = dwelling;

    if (dwell_just_entered) {
        // Just entered this dwell -- start a fresh Ki-diagnosis trace (see
        // adaptive_tune_zone_t's own comment on trace_t_s[] -- this window
        // covers exactly one dwell, unlike the K_dc observation ring, which
        // spans the whole run) and allow one more joint row to be committed
        // for it (see adaptive_tune_joint_dwell_row_committed's own comment: this zone
        // starting a fresh dwell is this module's proxy for "a new joint
        // dwell has begun" module-wide). The settle window itself is
        // (re)started below, at the first tick that actually has usable
        // data -- which may be this same tick, or a later one if this one's
        // reading is not trustworthy (see holes (a)/(b) above).
        z->settle_start_valid = false;
        z->settle_elapsed_s = 0.0f;
        z->recorded_this_dwell = false;
        z->trace_count = 0;
        z->trace_head = 0;
        z->trace_elapsed_s = 0.0f;

        // K9 (docs/audits/simc_sole_gain_writer_2026-09-14.md): this used to
        // also snapshot PID_FUZZY/fuzzy_strength_pct here (K8, docs/audits/
        // adaptive_tune_ki_guard_timing_and_failopen_2026-09-14.md) for
        // adaptive_tune_refine_ki_locked()'s effective-vs-reference guard.
        // That guard, and the write path it protected, are both gone now --
        // adaptive_tune_ki.c is diagnostic-only and SIMC (adaptive_tune_
        // model.c) is the sole AUTOMATIC gain writer, which is
        // frame-independent of fuzzy by construction (see that file's own
        // top comment) -- so
        // there is nothing left here for a snapshot to protect. Removed
        // rather than left as dead bookkeeping.
        // H4(c): this flag is shared MODULE-WIDE (see its own comment), so
        // ANY zone's dwell_just_entered -- including a DISABLED zone's, since
        // this branch runs unconditionally, before the z->enabled gate below
        // -- clears it and reopens the commit window. Safe today only
        // because profile_executor.c's s_exec.dwelling is a single global
        // flag applied to every zone in the same loop iteration (see
        // profile_executor.c:501), so every enabled AND disabled zone
        // transitions dwelling->true on the exact same tick; a disabled
        // zone's "fresh dwell" is therefore never actually early relative to
        // the enabled zones it shares a physical dwell with. If profile_
        // executor.c ever moves to a PER-ZONE dwelling signal, a disabled
        // zone could enter its own dwell on a different tick than the
        // enabled zones and reopen this shared commit flag mid-dwell,
        // silently admitting a second joint row from what the enabled zones
        // still consider one physical dwell.
        adaptive_tune_joint_dwell_row_committed = false;
    }
    if (!dwelling) {
        // Not dwelling any more (or not yet) -- keep the settle tracker
        // clean so the next dwell starts from a baseline uncontaminated by
        // this tick, whatever z->enabled/actual_valid say about it.
        z->settle_start_valid = false;
        z->settle_elapsed_s = 0.0f;
        z->recorded_this_dwell = false;
        z->duty_win_bucket_open_idx = 0;
        z->duty_win_bucket_elapsed_s = 0.0f;
        z->duty_win_bucket_has_sample = false;
        z->duty_win_closed_count = 0;
    }

    if (!z->enabled || !actual_valid || isnan(ambient_c)) {
        // Learning is off, or this tick has nothing trustworthy to offer.
        // dwelling_prev and the fresh-dwell reset above already ran
        // unconditionally, so there is nothing left to do here.
        xSemaphoreGive(adaptive_tune_lock);
        return;
    }

    if (!dwelling) {
        xSemaphoreGive(adaptive_tune_lock);
        return;
    }

    if (!z->settle_start_valid) {
        // Either this is genuinely the first valid tick of a fresh dwell, or
        // the dwell was entered earlier while the data was invalid/the zone
        // was disabled (holes (a)/(b) above) -- either way, this is the
        // first valid opportunity to start the settle window for this
        // dwell.
        z->settle_start_valid = true;
        z->settle_start_c = actual_c;
        z->settle_duty_min = duty;
        z->settle_duty_max = duty;
        z->duty_win_bucket_open_idx = 0;
        z->duty_win_bucket_elapsed_s = 0.0f;
        z->duty_win_bucket_has_sample = false;
        z->duty_win_closed_count = 0;
    }
    z->settle_elapsed_s += dt_s;
    if (duty < z->settle_duty_min) {
        z->settle_duty_min = duty;
    }
    if (duty > z->settle_duty_max) {
        z->settle_duty_max = duty;
    }
    // Trailing SLIDING duty-stability window (ADAPTIVE_TUNE_DUTY_WINDOW_
    // BUCKET_S/_NUM_BUCKETS' own comment, adaptive_tune_internal.h): fold
    // the current sample into the OPEN bucket every tick, unconditionally.
    // Bucket closure/rotation happens AFTER the fold, same "verdict reads
    // from what was just folded" discipline the previous (defective) single-
    // window tumble used, but here closing one bucket never discards the
    // other NUM_BUCKETS-1 already-closed buckets, so the combined min/max a
    // verdict is read from (further down) can never come from fewer than
    // (NUM_BUCKETS-1)*BUCKET_S seconds of history once the window is mature.
    if (!z->duty_win_bucket_has_sample) {
        z->duty_win_bucket_min[z->duty_win_bucket_open_idx] = duty;
        z->duty_win_bucket_max[z->duty_win_bucket_open_idx] = duty;
        z->duty_win_bucket_has_sample = true;
    } else {
        if (duty < z->duty_win_bucket_min[z->duty_win_bucket_open_idx]) {
            z->duty_win_bucket_min[z->duty_win_bucket_open_idx] = duty;
        }
        if (duty > z->duty_win_bucket_max[z->duty_win_bucket_open_idx]) {
            z->duty_win_bucket_max[z->duty_win_bucket_open_idx] = duty;
        }
    }
    z->duty_win_bucket_elapsed_s += dt_s;
    if (z->duty_win_bucket_elapsed_s >= ADAPTIVE_TUNE_DUTY_WINDOW_BUCKET_S) {
        // Close the open bucket and rotate to the next ring slot, seeding it
        // fresh (has_sample=false) -- the OLD occupant of that slot, if any,
        // is now overwritten, which is exactly the eviction of the oldest
        // bucket a ring is supposed to perform.
        if (z->duty_win_closed_count < (uint32_t)ADAPTIVE_TUNE_DUTY_WINDOW_NUM_BUCKETS) {
            z->duty_win_closed_count++;
        }
        z->duty_win_bucket_open_idx =
            (z->duty_win_bucket_open_idx + 1) % (uint32_t)ADAPTIVE_TUNE_DUTY_WINDOW_NUM_BUCKETS;
        z->duty_win_bucket_elapsed_s = 0.0f;
        z->duty_win_bucket_has_sample = false;
    }

    // Trace append happens on EVERY dwelling tick, not gated on settle --
    // the Ki diagnosis needs the window's SHAPE (drift, oscillation), which
    // the single post-settle dc-gain point below cannot provide.
    z->trace_elapsed_s += dt_s;
    {
        uint32_t tslot = (z->trace_head + z->trace_count) % ADAPTIVE_TUNE_KI_TRACE_CAPACITY;
        if (z->trace_count < ADAPTIVE_TUNE_KI_TRACE_CAPACITY) {
            z->trace_count++;
        } else {
            z->trace_head = (z->trace_head + 1) % ADAPTIVE_TUNE_KI_TRACE_CAPACITY;
        }
        z->trace_t_s[tslot] = z->trace_elapsed_s;
        z->trace_actual_c[tslot] = actual_c;
        z->trace_duty[tslot] = duty;
    }

    if (z->recorded_this_dwell || !z->settle_start_valid) {
        xSemaphoreGive(adaptive_tune_lock);
        return;
    }
    if (z->settle_elapsed_s < ADAPTIVE_TUNE_SETTLE_MIN_S) {
        xSemaphoreGive(adaptive_tune_lock);
        return; // still within the settle window -- keep waiting
    }
    float slope = fabsf(actual_c - z->settle_start_c) / z->settle_elapsed_s;
    if (slope > ADAPTIVE_TUNE_SETTLE_SLOPE_FLOOR_C_PER_S) {
        // Genuinely still drifting -- do NOT reset the window; a real drift
        // keeps failing this test every tick going forward (the elapsed-time
        // denominator only grows), which is the correct outcome. A brief
        // noise spike self-corrects on the next tick's smaller slope.
        xSemaphoreGive(adaptive_tune_lock);
        return;
    }

    // Settled on TEMPERATURE. Duty-stability check next -- see ADAPTIVE_TUNE_
    // DUTY_STABILITY_ABS/FRAC/_WINDOW_S and _WINDOW_BUCKET_S/_NUM_BUCKETS'
    // own comments (adaptive_tune_internal.h) for the real-hardware defect
    // this closes, the 2026-09-14 fix, and that same day's correction of the
    // fix. A temperature slope passing this file's floor is not sufficient
    // evidence of steady state on an under-damped zone; duty itself must
    // also have stayed put, over its OWN trailing window (not the whole
    // dwell -- an entry transient must not veto every later tick, and not a
    // single fresh sample either -- a verdict from 1 tick of history is not
    // steady-state evidence, see the correction comment above).
    //
    // recorded_this_dwell is deliberately NOT set for either an immature-
    // window or a duty_unstable verdict: neither says anything permanent
    // about the dwell's operating point -- "not enough window history yet"
    // resolves itself purely by more time passing, and "unstable right now"
    // can resolve as the entry transient ages out of the window -- so both
    // retry every tick, exactly like the unchanged temperature-slope retry
    // a few lines above for the identical reason.
    if (z->duty_win_closed_count < (uint32_t)ADAPTIVE_TUNE_DUTY_WINDOW_NUM_BUCKETS) {
        adaptive_tune_set_refusal(z,
            "duty window still gathering history (%lu/%d buckets closed) -- not a steady-state observation",
            (unsigned long)z->duty_win_closed_count, ADAPTIVE_TUNE_DUTY_WINDOW_NUM_BUCKETS);
        xSemaphoreGive(adaptive_tune_lock);
        return;
    }
    {
        // Combine every bucket currently held in the ring (there are always
        // exactly NUM_BUCKETS slots once duty_win_closed_count has saturated,
        // including the CURRENTLY OPEN one, which contributes only the
        // samples folded into it so far) into one min/max pair. This can
        // never be read from fewer than (NUM_BUCKETS-1)*BUCKET_S seconds of
        // closed-bucket history, regardless of how little time has elapsed
        // in the currently-open bucket -- unlike the single-pair tumble this
        // replaces, there is no instant at which the combined range reflects
        // only the open bucket's contents.
        float win_min = z->duty_win_bucket_has_sample ? z->duty_win_bucket_min[z->duty_win_bucket_open_idx] : duty;
        float win_max = z->duty_win_bucket_has_sample ? z->duty_win_bucket_max[z->duty_win_bucket_open_idx] : duty;
        for (int bi = 0; bi < ADAPTIVE_TUNE_DUTY_WINDOW_NUM_BUCKETS; bi++) {
            if (bi == (int)z->duty_win_bucket_open_idx) {
                continue; // already folded in above (open bucket, may be empty)
            }
            if (z->duty_win_bucket_min[bi] < win_min) {
                win_min = z->duty_win_bucket_min[bi];
            }
            if (z->duty_win_bucket_max[bi] > win_max) {
                win_max = z->duty_win_bucket_max[bi];
            }
        }
        float duty_range = win_max - win_min;
        bool duty_unstable = duty_range > ADAPTIVE_TUNE_DUTY_STABILITY_ABS ||
                              (duty > 0.0f && duty_range > ADAPTIVE_TUNE_DUTY_STABILITY_FRAC * duty);

        if (duty_unstable) {
            // Distinct refusal string (task requirement: an operator must be
            // able to tell "no data yet" from "data rejected as unsettled")
            // -- reaches the same last_refusal_reason surface every other
            // guard in this module reports through.
            adaptive_tune_set_refusal(z,
                "duty still oscillating (range %.3f, %.0f%% of %.3f) -- not a steady-state observation",
                (double)duty_range, (double)((duty > 0.0f) ? (100.0f * duty_range / duty) : 0.0f), (double)duty);
            xSemaphoreGive(adaptive_tune_lock);
            return;
        }
    }

    // Settled on both temperature and duty. From here on, every path
    // terminates this dwell's observation attempt one way or another, so
    // recorded_this_dwell is set on every remaining path out -- a marginal
    // (too-low-duty, implausible-rise) dwell does not get re-evaluated every
    // tick for the rest of its length, since those two verdicts reflect the
    // dwell's operating point, not a transient that can still resolve.
    z->recorded_this_dwell = true;

    if (duty < ADAPTIVE_TUNE_MIN_DUTY_FOR_OBSERVATION) {
        xSemaphoreGive(adaptive_tune_lock);
        return; // real steady state, but too little duty to trust the ratio
    }
    float rise_c = actual_c - ambient_c;
    if (!isfinite(rise_c) || rise_c <= 0.0f) {
        // A settled dwell above ambient always has positive rise on a
        // working heater; zero/negative here means a bad ambient capture
        // or a zone that never actually rose (thermocouple/relay fault
        // elsewhere) -- not a physically usable point either way.
        xSemaphoreGive(adaptive_tune_lock);
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

    // Joint (all-zone) row for the coupled solve, committed at the SAME
    // settle instant as this zone's own diagonal point above -- only if
    // EVERY zone (this one included) currently has a fresh, valid,
    // above-floor duty reading. A partial row (a neighbour never having
    // ticked yet, or sitting invalid/too-low this instant) is discarded
    // outright rather than committed with a placeholder -- a zero-filled
    // column would silently poison that zone out of every future row's
    // design matrix instead of just being absent from this one.
    {
        bool joint_ok = !adaptive_tune_joint_dwell_row_committed; // see adaptive_tune_joint_dwell_row_committed's own comment --
                                                       // at most one joint row per distinct dwell
        for (uint8_t j = 0; joint_ok && j < MAX31856_CHANNEL_COUNT; j++) {
            if (!adaptive_tune_joint_last_valid[j] || adaptive_tune_joint_last_duty[j] < ADAPTIVE_TUNE_JOINT_MIN_DUTY) {
                joint_ok = false;
            }
        }
        if (joint_ok) {
            adaptive_tune_joint_dwell_row_committed = true;
            uint32_t jslot = (adaptive_tune_joint_ring_head + adaptive_tune_joint_ring_count) % ADAPTIVE_TUNE_JOINT_RING_CAPACITY;
            if (adaptive_tune_joint_ring_count < ADAPTIVE_TUNE_JOINT_RING_CAPACITY) {
                adaptive_tune_joint_ring_count++;
            } else {
                adaptive_tune_joint_ring_head = (adaptive_tune_joint_ring_head + 1) % ADAPTIVE_TUNE_JOINT_RING_CAPACITY;
            }
            memcpy(adaptive_tune_joint_ring[jslot].duty, adaptive_tune_joint_last_duty, sizeof(adaptive_tune_joint_last_duty));
            memcpy(adaptive_tune_joint_ring[jslot].rise_c, adaptive_tune_joint_last_rise_c, sizeof(adaptive_tune_joint_last_rise_c));
            adaptive_tune_joint_observations_lifetime++;
        }
    }

    // The Ki diagnosis itself runs at adaptive_tune_run_end(), not here --
    // it needs profile_exec_firing_stats_t's dwell_err_mean_c/dwell_err_max_c
    // (the SETPOINT-aware error figures profile_executor.c already computes;
    // this file never receives setpoint_c per tick, see adaptive_tune.c's
    // top comment), which only arrive with the run record. z->trace_* above
    // is left as-is (this dwell's most recent trace) for that call to read.

    xSemaphoreGive(adaptive_tune_lock);
}

// H2/K1/K2 fix: every run_end path that skips both adaptive_tune_refine_zone_locked()
// and adaptive_tune_refine_ki_locked()/adaptive_tune_refine_coupled_locked() entirely must reset
// this run's PER-RUN status fields (ki_applied/ki_verdict/ki_correction_pct/
// coupled_applied/coupled_cells_changed) to a neutral "nothing happened this
// run" state, not just write a refusal-reason STRING -- adaptive_tune_get_
// status() publishes all five verbatim, so a path that only touched the
// strings left them holding whatever a PREVIOUS run left there (measured
// pre-H2: a faulted run right next to a stale "Ki correction applied,
// +20%" from the prior run).
//
// K1: H2's own fix still missed two more paths of the SAME shape -- the
// `!z->enabled` and `!zr->active` (the profile's zone mask) continues. On a
// kiln with fewer zones wired than MAX31856_CHANNEL_COUNT, ANY profile that
// doesn't touch a given zone left it publishing its previous firing's status
// forever. Restructured so there is exactly ONE call site that can skip a
// zone, via an if/else-if chain rather than a sequence of early `continue`s
// -- a future fifth skip condition is just another else-if into the SAME
// reset call, not a new early-exit that has to remember to make it.
//
// K2: has_applied is documented (adaptive_tune.h) as a LIFETIME LATCH --
// zones_page.html gates its "Last applied change" column on it, alongside
// prior_k_dc/applied_k_dc/last_delta_pct/last_applied_profile_id, none of
// which are per-run either. H2's fix cleared has_applied on every skip path,
// a REGRESSION: a faulted run after a real applied refinement made the UI
// revert to "no refinement applied yet" even though the applied change is
// still exactly what is live on the zone. reset_run_status_locked() below
// therefore leaves has_applied and its lifetime siblings untouched -- only
// the genuinely PER-RUN fields (coupled_*, ki_*) are reset here.
static void reset_run_status_locked(adaptive_tune_zone_t *z, const char *reason)
{
    adaptive_tune_set_refusal(z, "%s", reason);
    adaptive_tune_set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason), "%s", reason);
    z->coupled_attempted = false;
    z->coupled_applied = false;
    z->coupled_cells_changed = 0;
    adaptive_tune_set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason), "%s", reason);
    z->ki_applied = false;
    z->ki_verdict = (uint8_t)ADAPTIVE_TUNE_KI_INSUFFICIENT;
    z->ki_correction_pct = 0.0f;
}

// ---------------------------------------------------------------------
// P1: Ki-baseline NVS persistence -- dispatched through the flash worker,
// same convention as save_enmask_job() below, and for the identical reason
// (a flash WRITE from a PSRAM-stacked task panics on this board every time).
// See adaptive_tune_kibase_blob_t's own comment (adaptive_tune_internal.h)
// for why this is one blob, not a mask byte plus a separate values array.
// ---------------------------------------------------------------------

typedef struct {
    adaptive_tune_kibase_blob_t blob;
    esp_err_t result;
} kibase_job_t;

// In-memory rev counter for the cfg-filesystem dual-write below -- resolved
// at boot (adaptive_tune_init()) from whichever side (file/NVS) won, then
// incremented by every save_kibase_job() call. All three call sites that
// build a kibase_job_t (adaptive_tune_run_end()/adaptive_tune_clear_ki_
// baseline()/adaptive_tune_revert()) funnel through this ONE function to
// reach NVS/the file, so a single counter here -- rather than one at each
// call site -- cannot desync between them. Simplified treatment per this
// item's "re-derivable over one firing" audit finding: no per-zone
// divergence forensics, just monotonically increasing so a stale file can
// never outrank a fresher NVS write or vice versa.
static uint32_t s_kibase_rev = 0;

// pref_cfg_fs_validate_fn_t for this blob: the only structural invariant
// worth checking is that `mask` never claims a zone index this build does
// not have -- everything else (a garbage baseline value for a zone whose
// bit IS set) is exactly as trusted as the NVS blob always was, since this
// item's whole point is that losing/re-deriving it is cheap.
bool adaptive_tune_kibase_file_validate(const void *bytes, size_t len)
{
    if (len != sizeof(adaptive_tune_kibase_blob_t)) {
        return false;
    }
    const adaptive_tune_kibase_blob_t *b = (const adaptive_tune_kibase_blob_t *)bytes;
    uint8_t valid_mask = (uint8_t)((1u << MAX31856_CHANNEL_COUNT) - 1u);
    return (b->mask & (uint8_t)~valid_mask) == 0;
}

// cfg file ONLY (docs/CONFIG_FILESYSTEM.md, "Dual-write window: closed"): no
// NVS write follows, and a failed write is reported through job->result. The
// rev counter still advances on failure (a gap is harmless, a reused rev is not).
static void save_kibase_job(void *arg)
{
    kibase_job_t *job = (kibase_job_t *)arg;
    uint32_t rev = ++s_kibase_rev;
    job->result = pref_cfg_fs_commit(ADAPTIVE_TUNE_KIBASE_FILE_PATH, &job->blob, sizeof(job->blob), rev,
                                     "adaptive-tune ki baseline");
}

uint32_t adaptive_tune_ki_clear_gen[MAX31856_CHANNEL_COUNT];

void adaptive_tune_run_end(const profile_firing_run_record_t *rec, bool clean)
{
    if (!rec) {
        return;
    }
    adaptive_tune_ensure_lock();
    xSemaphoreTake(adaptive_tune_lock, portMAX_DELAY);
    // P1: tracks whether ANY zone's ki_baseline_valid flipped false -> true
    // during this run's loop below -- the NVS write dispatched after the
    // lock is released only actually needs to happen when this is true (a
    // baseline, once persisted, never changes again -- see ki_baseline's
    // own "latched once, never overwritten" comment), but it costs nothing
    // to always snapshot the full current state into kibase_snapshot while
    // still holding the lock, so the dispatch below is a plain memcpy-then-
    // write with no second lock acquisition needed.
    bool baseline_newly_latched = false;
    bool zone_active[MAX31856_CHANNEL_COUNT] = {false};
    bool zone_planned[MAX31856_CHANNEL_COUNT] = {false};
    bool baseline_was_valid_a[MAX31856_CHANNEL_COUNT] = {false};
    float baseline_before_a[MAX31856_CHANNEL_COUNT] = {0};
    adaptive_tune_zone_plan_t zone_plans[MAX31856_CHANNEL_COUNT];
    adaptive_tune_coupled_plan_t coupled_plans[MAX31856_CHANNEL_COUNT];
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        adaptive_tune_zone_t *z = &adaptive_tune_zones[zi];
        const profile_firing_zone_record_t *zr = &rec->zones[zi];

        // K1: single skip decision for this zone -- see block comment above.
        const char *skip_reason = NULL;
        char excluded_reason_buf[96]; // must outlive the chain below -- so skip_reason never dangles
        if (!z->enabled) {
            skip_reason = "zone not opted into adaptive tuning -- not used as training data";
        } else if (zone_is_on_off(zi)) {
            skip_reason = "on/off zone -- no PID, no model, not used as training data";
        } else if (zone_is_monitor_only(zi)) {
            // docs/SPARE_RELAY_ONOFF_PLAN.md sec 10: never driven, so this run
            // says nothing about its plant model; the masked (all-zero)
            // coupling row must not be blended into the stored one either.
            skip_reason = "monitor-only zone (no heater relay) -- not used as training data";
        } else if (z->write_in_flight) {
            // F3 follow-up: adaptive_tune_revert() is writing this zone's gains
            // with the lock released; planning against them now would capture a
            // revert snapshot and commit a result its write then undoes.
            skip_reason = "a revert of this zone was in progress at run end -- not used as training data";
        } else if (!zr->active) {
            skip_reason = "zone not active in this profile's zone mask -- not used as training data";
        } else if (!clean) {
            skip_reason = "run was faulted or stopped early -- not used as training data";
        } else {
            uint32_t total = zr->stats.sample_count + zr->stats.excluded_sample_count;
            if (total > 0 &&
                (float)zr->stats.excluded_sample_count / (float)total > ADAPTIVE_TUNE_MAX_EXCLUDED_FRACTION) {
                snprintf(excluded_reason_buf, sizeof(excluded_reason_buf),
                          "run excluded %u/%u samples (>%.0f%%) -- not used as training data",
                          (unsigned)zr->stats.excluded_sample_count, (unsigned)total,
                          (double)(ADAPTIVE_TUNE_MAX_EXCLUDED_FRACTION * 100.0f));
                skip_reason = excluded_reason_buf;
            }
        }
        if (skip_reason) {
            reset_run_status_locked(z, skip_reason);
            // ADAPTIVE_FUZZY_EVALUATION.md sec 3: a run this module
            // could not use as training data (disabled, on/off, inactive,
            // faulted/dirty, or too many excluded samples) is not evidence
            // the plant model is still good -- floor the confidence counter
            // rather than leave a stale high count from an earlier, unrelated
            // run standing. Asymmetric by design (floor immediately, rise
            // only one step per accepted run below), matching the
            // oscillation backstop's own asymmetry (N3).
            z->fuzzy_confidence_c = 0;
            continue;
        }
        // D5: adaptive_tune_refine_zone_locked() rewrites Kp/Ki/Kd from a fresh SIMC
        // recompute when it applies. adaptive_tune_refine_ki_locked() diagnoses Ki
        // from this run's WITHIN-DWELL TRACE -- evidence gathered under
        // whatever Ki was actually running during the dwell, which is the
        // OLD value if the model refine just replaced it. Running the Ki
        // diagnosis's correction on top of a Ki that postdates the evidence
        // it was measured against is exactly the blind-stacking bug this
        // fixes: the two layers do not compose in one run, so only one of
        // them may act. The model/K_dc refinement wins when both would
        // apply -- it is the more direct measurement (a dwell duty/rise
        // ratio) versus the Ki diagnosis's shape-based inference, and a
        // freshly-recomputed SIMC Ki is itself already responsive to a
        // gain change this run. The Ki diagnosis gets its turn on any run
        // where the model refine did not fire (guard refusal, no change,
        // or the zone's coupled/diagonal fit was simply not due) -- by
        // which point its trace evidence and the live Ki agree on which
        // run produced them.
        // Q4: captured BEFORE either refine call -- adaptive_tune_refine_zone_locked()
        // (the model_refined branch below) can now re-latch ki_baseline from
        // a fresh SIMC Ki too, not just adaptive_tune_refine_ki_locked(), so this must
        // watch for a VALUE change, not only a false->true valid transition
        // (the transition alone was already sufficient before Q4, when
        // adaptive_tune_refine_ki_locked() was the only writer).
        // F3: plan only (no zones setter) while the lock is held; the setters run
        // in the unlocked pass below and their outcome is committed in the third.
        zone_active[zi] = true;
        baseline_was_valid_a[zi] = z->ki_baseline_valid;
        baseline_before_a[zi] = z->ki_baseline;
        zone_planned[zi] = adaptive_tune_plan_zone_locked(zi, rec->profile_id, &zone_plans[zi]);
        adaptive_tune_plan_coupled_locked(zi, &coupled_plans[zi]);
        z->write_in_flight = true; // F3 follow-up: a revert refuses until the commit pass clears this
    }

    // F3: the zones setters end in the zones NVS save, which dispatches onto the
    // flash worker; the worker takes adaptive_tune_lock (UART Accept ->
    // adaptive_tune_clear_ki_baseline()). Never hold the lock across them.
    xSemaphoreGive(adaptive_tune_lock);
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!zone_active[zi]) {
            continue;
        }
        if (zone_planned[zi] || zone_plans[zi].bootstrap_baseline) {
            adaptive_tune_apply_zone_plan(zi, &zone_plans[zi]);
        }
        adaptive_tune_apply_coupled_plan(zi, &coupled_plans[zi]);
    }
    xSemaphoreTake(adaptive_tune_lock, portMAX_DELAY);

    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!zone_active[zi]) {
            continue;
        }
        adaptive_tune_zone_t *z = &adaptive_tune_zones[zi];
        const profile_firing_zone_record_t *zr = &rec->zones[zi];
        bool baseline_was_valid = baseline_was_valid_a[zi];
        float baseline_before = baseline_before_a[zi];
        z->write_in_flight = false;
        bool model_refined = adaptive_tune_commit_zone_locked(zi, &zone_plans[zi]);
        adaptive_tune_commit_coupled_locked(zi, &coupled_plans[zi]);
        if (!model_refined) {
            adaptive_tune_refine_ki_locked(zi, &zr->stats);
        } else {
            // F2: unlike the full-skip paths above (reset_run_status_
            // locked()), coupled_* is NOT reset here -- the
            // model refine and coupled solve DID run this cycle and their
            // fields legitimately reflect this run's own outcome. Only the
            // Ki-diagnosis fields are stale here (that diagnosis alone was
            // skipped), so only those three are reset.
            //
            // The diagnosis did not run this cycle, so ki_verdict/
            // ki_correction_pct must not keep publishing whatever they held
            // from a PREVIOUS run -- adaptive_tune_get_status() surfaces
            // both verbatim, and a stale verdict/pct read as this run's
            // result is exactly the kind of "reset one side" hole this
            // module has shipped before. ADAPTIVE_TUNE_KI_INSUFFICIENT is
            // the closest existing verdict for "nothing to report" (see its
            // own doc comment); there is no dedicated "skipped" verdict, so
            // the refusal reason string is what actually distinguishes this
            // case from a real too-short-trace refusal.
            z->ki_applied = false;
            z->ki_verdict = (uint8_t)ADAPTIVE_TUNE_KI_INSUFFICIENT;
            z->ki_correction_pct = 0.0f;
            adaptive_tune_set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason),
                       "Ki diagnosis skipped this run -- the model/PID refinement already rewrote Ki from SIMC, "
                       "see adaptive_tune_run_end()'s D5 comment");
        }
        // P1/Q4: a NEW or CHANGED baseline this run (from either writer --
        // see baseline_before/baseline_was_valid's own comment above) is
        // what actually needs persisting; a zone whose baseline was already
        // valid and unchanged this run has nothing new to write.
        if (z->ki_baseline_valid && (!baseline_was_valid || z->ki_baseline != baseline_before)) {
            baseline_newly_latched = true;
        }

        // ADAPTIVE_FUZZY_EVALUATION.md sec 3: cross-firing confidence
        // counter. DISCLOSED SIMPLIFICATION (see fuzzy_confidence_c's own
        // comment, adaptive_tune_internal.h) -- this is a scope-limited
        // proxy for the plan's full forward-checked-residual signal, using
        // what this module already computes: model_refined (a fit actually
        // ran and passed its own eligibility/plausibility gates in
        // adaptive_tune_refine_zone_locked()) and last_delta_pct (how much
        // the fit moved from the prior accepted value). A refined model
        // whose K_dc barely moved is read as agreement with the standing
        // model -- one "good" run, rise by exactly one step (never jump
        // straight to MAX_C on a single run, matching the plan's own
        // "rate-limited... one step per accepted run" language). A refined
        // model that moved a lot, or a run where refinement did not fire,
        // is not evidence of a stable fit -- floor immediately rather than
        // hold the previous count.
        if (model_refined && fabsf(z->last_delta_pct) <= ADAPTIVE_TUNE_FUZZY_CONFIDENCE_STABLE_DELTA_PCT) {
            if (z->fuzzy_confidence_c < PID_FUZZY_CONFIDENCE_MAX_C) {
                z->fuzzy_confidence_c++;
            }
        } else {
            z->fuzzy_confidence_c = 0;
        }
    }

    // P1: snapshot the current baseline state for persistence WHILE still
    // holding the lock -- cheap (a memcpy-sized loop), and the alternative
    // (re-taking the lock after it is released below) would just be more
    // code for no benefit.
    kibase_job_t job = {.result = ESP_FAIL};
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        job.blob.vals[zi] = adaptive_tune_zones[zi].ki_baseline;
        if (adaptive_tune_zones[zi].ki_baseline_valid) {
            job.blob.mask |= (uint8_t)(1u << zi);
        }
    }
    xSemaphoreGive(adaptive_tune_lock);

    // Dispatched OUTSIDE the lock, only when a baseline actually changed --
    // same "never wait on the flash worker with adaptive_tune_lock held" rule as
    // adaptive_tune_set_enabled() below. A run that latched no NEW baseline
    // (every zone already had one, or none reached adaptive_tune_refine_ki_locked()
    // at all this run) skips the write entirely -- there is nothing new to
    // persist, and en_mask-style writing the identical bytes back every run
    // would just be wear for no benefit.
    //
    // RE-ENTRANCY: this function is reached ON the flash-safe worker task
    // itself -- profile_executor_halt() (profile_executor_status.c) calls
    // adaptive_tune_run_end() directly, and profile_executor_halt() is
    // itself one of uart_bridge_ext.c's on-worker calls (its own list at
    // uart_bridge_ext.c:120-127). That part is true and stays true. But the
    // hazard itself -- a dispatch onto the worker from inside this call --
    // is NOT reachable through that path today: profile_executor_status.c
    // hardcodes `clean=false` at its adaptive_tune_run_end() call site,
    // which forces every zone through the skip-and-continue branch below
    // before baseline_newly_latched can ever be set, so the `if
    // (baseline_newly_latched)` block below never dispatches from there.
    // The guard is kept anyway -- defensive, cheap, and correct if `clean`
    // ever stops being a constant -- and it mirrors the genuinely reachable
    // guard on adaptive_tune_clear_ki_baseline() above, which check
    // uart_bridge_ext_is_on_flash_worker() first and run the save inline
    // instead of dispatching a second job onto the worker from inside the
    // first.
    if (baseline_newly_latched) {
        esp_err_t err;
        if (uart_bridge_ext_is_on_flash_worker()) {
            save_kibase_job(&job);
            err = ESP_OK;
        } else {
            err = uart_bridge_ext_run_on_flash_worker(save_kibase_job, &job);
        }
        if (err != ESP_OK || job.result != ESP_OK) {
            ESP_LOGE(ADAPTIVE_TUNE_TAG, "adaptive_tune_run_end(): Ki baseline NVS save failed: %s / %s",
                     esp_err_to_name(err), esp_err_to_name(job.result));
        }
    }
}

// ---------------------------------------------------------------------
// Opt-in flag: PID_EXPANSION_PLAN.md 3.3, U2 -- persisted through the zone
// config blob now (zones_config_set_adaptive_tune_enabled(),
// zones_config_accessors.c), NOT this module's own NVS namespace. See
// adaptive_tune.h's top comment for the full "why" and adaptive_tune_
// migrate_enable_flags()/adaptive_tune_load_enable_flags() below for how an
// upgrading board's prior choice, still sitting in the old namespace, is
// carried into its new home.
// ---------------------------------------------------------------------

bool adaptive_tune_set_enabled(uint8_t zone_index, bool enabled)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    adaptive_tune_ensure_lock();
    xSemaphoreTake(adaptive_tune_lock, portMAX_DELAY);
    adaptive_tune_zones[zone_index].enabled = enabled;
    xSemaphoreGive(adaptive_tune_lock);

    // Called OUTSIDE the lock -- same "never hold this lock across a write"
    // rule the old en_mask dispatch kept, even though zones_config_set_
    // adaptive_tune_enabled() itself is a direct, unwrapped nvs_save(), not
    // a flash-worker dispatch: this function's only caller (adaptive_tune_
    // http.c's enable_post_handler()) runs on httpd_worker, which is
    // confirmed NOT PSRAM-stacked -- see zones_http_handlers.c's "TASK/FLASH
    // SAFETY" comment on zones_pid_post_handler(), the same task, doing the
    // identical kind of direct zones_config_set_*() write. A future second
    // caller on a PSRAM-stacked task would need to route through the flash
    // worker itself, same as adaptive_tune_revert() does for ITS zones_
    // config writes below (see that function's own comment on why it does
    // NOT need to, for the same httpd_worker reason, today).
    bool saved = zones_config_set_adaptive_tune_enabled(zone_index, enabled);
    if (!saved) {
        ESP_LOGE(ADAPTIVE_TUNE_TAG, "adaptive_tune_set_enabled(%u,%d): zone config save failed",
                 (unsigned)zone_index, (int)enabled);
        // live flag still stands -- see time_sync_set_tz()'s identical convention
    }
    return saved;
}

// U2 migration -- called once from adaptive_tune_load_enable_flags() below,
// idempotent (safe to call every boot). Reads the OLD 'adap_tune'/en_mask
// byte directly (not through the flash worker) and, the FIRST time this
// runs on a given board, applies every bit it finds to the zone config
// blob's new adaptive_tune_enabled field via zones_config_set_adaptive_
// tune_enabled() -- then marks en_migrated so it is NEVER consulted again.
// That last part matters: without it, an operator who explicitly turned
// adaptive tuning back OFF in its new home would have it silently RE-
// enabled on the next boot by the stale old byte -- exactly the "silent
// reset to default-off/on defeats an explicit operator choice" failure
// mode this migration must not have, just pointed the other direction (a
// silent reset back ON instead of back OFF).
//
// Direct NVS access throughout -- reads for the usual "app_main task, not
// PSRAM-stacked" reason (adaptive_tune_init()'s own top comment), and WRITES
// here (the migrated-flag write, and zones_config_set_adaptive_tune_
// enabled()'s own nvs_save()) for the SAME reason plus one more: this runs
// from adaptive_tune_load_enable_flags(), called from main.c right after
// zones_http_start() -- well before uart_bridge_ext_start_flash_worker()
// runs (main.c's boot order), so the flash worker literally does not exist
// yet at this point; dispatching onto it here would fail or hang.
static void adaptive_tune_migrate_enable_flags(void)
{
    hal_kv_handle_t h;
    hal_status_t err =
        hal_kv_open(&h, ADAPTIVE_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, ADAPTIVE_TUNE_NVS_PARTITION);
    if (err != HAL_OK) {
        // Namespace never touched at all (a brand-new board, or one that
        // never opted any zone in under the old scheme) -- nothing to
        // migrate; the new home's struct-zero default (off) is already
        // correct, and there is no "migrated" marker to write since there
        // is nothing to open.
        return;
    }

    uint8_t migrated = 0;
    if (hal_kv_get_u8(&h, ADAPTIVE_TUNE_NVS_KEY_ENMASK_MIGRATED, &migrated) == HAL_OK && migrated) {
        hal_kv_close(&h);
        return; // already migrated on a previous boot -- en_mask must never be consulted again
    }

    uint8_t old_mask = 0;
    hal_status_t mask_err = hal_kv_get_u8(&h, ADAPTIVE_TUNE_NVS_KEY_ENMASK, &old_mask);
    if (mask_err == HAL_OK && old_mask != 0) {
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if ((old_mask & (1u << zi)) != 0) {
                if (!zones_config_set_adaptive_tune_enabled(zi, true)) {
                    ESP_LOGE(ADAPTIVE_TUNE_TAG,
                             "migrate: zone %u's opt-in write to its new home failed -- will retry next boot "
                             "(en_migrated not set)",
                             (unsigned)zi);
                    hal_kv_close(&h);
                    return; // do NOT mark migrated -- a partial migration must be retried whole, not
                            // half-applied and then never revisited
                }
            }
        }
        ESP_LOGI(ADAPTIVE_TUNE_TAG,
                 "migrated adaptive-tune opt-in mask 0x%02x from the old 'adap_tune' namespace into the "
                 "zone config blob",
                 (unsigned)old_mask);
    }
    // mask_err == HAL_NOT_FOUND (key never written) is just as much
    // "nothing to migrate" as a found-but-zero mask -- either way this
    // namespace must never be consulted again after this point.
    hal_status_t mark_err = hal_kv_set_u8(&h, ADAPTIVE_TUNE_NVS_KEY_ENMASK_MIGRATED, 1);
    if (mark_err == HAL_OK) {
        mark_err = hal_kv_commit(&h);
    }
    if (mark_err != HAL_OK) {
        ESP_LOGE(ADAPTIVE_TUNE_TAG, "migrate: failed to persist the migrated marker: %s -- will retry next boot",
                 hal_status_to_name(mark_err));
    }
    hal_kv_close(&h);
}

void adaptive_tune_load_enable_flags(void)
{
    adaptive_tune_ensure_lock();
    adaptive_tune_migrate_enable_flags();
    xSemaphoreTake(adaptive_tune_lock, portMAX_DELAY);
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        adaptive_tune_zones[zi].enabled = zones_config_get_adaptive_tune_enabled(zi);
    }
    xSemaphoreGive(adaptive_tune_lock);
}

// Q3: the escape hatch the cumulative-bound refusal message actually names
// ("-- re-autotune this zone"). Before this existed, ki_baseline was latched
// once (adaptive_tune_refine_ki_locked()/adaptive_tune_refine_zone_locked(), see either's own
// comment) and NEVER cleared by anything: not adaptive_tune_set_enabled(),
// not zones_config_set_pid()/set_model() (autotune_engine.c calls those
// directly, never through this module), not a fresh autotune. Once the
// cumulative bound actually started refusing, the operator's only named
// remedy -- re-autotune -- was a no-op: the ceiling stayed old_baseline *
// ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT forever, even after a brand-new step
// test produced a completely different plant model. Call this from wherever
// an autotune RESULT is
// actually committed for a zone (autotune_engine.c's accept path, after its
// own zones_config_set_pid()/set_model() calls succeed -- see that call
// site's own comment) so the next adaptive_tune_refine_ki_locked()/adaptive_tune_
// refine_zone_locked() call re-latches fresh against the zone the operator just
// re-tuned, rather than an assumption that autotune result superseded.
//
// Lock order: this module's own adaptive_tune_lock only -- autotune_engine.c
// calls this AFTER releasing its own s_at.lock (see the call site), so there
// is no cross-module lock nesting here at all, let alone the s_exec.lock ->
// this-module ordering adaptive_tune.h's own comment documents for the
// zone_tick/run_end path. NVS write routed through the flash worker exactly
// like adaptive_tune_run_end()'s own save_kibase_job() dispatch -- and for
// the identical reason (a flash write from a PSRAM-stacked task panics on
// this board every time) -- and, same as adaptive_tune_set_enabled() just
// above, dispatched OUTSIDE the lock.
//
// RE-ENTRANCY, not just lock order: autotune_engine_accept() -- this
// function's only caller -- can itself already be running ON the flash-safe
// worker task when reached over the UART bridge (uart_bridge_ext.c's
// AUTOTUNE_CMD_ACCEPT runs the whole switch body, including this call, on
// bx_worker_task). Dispatching a SECOND job onto the same worker from inside
// the first one used to deadlock the board permanently: uart_bridge_ext.c's
// executor mutex is non-recursive and already held by the original caller,
// which is itself blocked waiting for the job this call is nested inside to
// finish; a recursive mutex would not save it either, since the 1-deep job
// queue behind it is only ever drained by the very worker task that would
// now be stuck trying to enqueue into it.
//
// Fixed two ways, deliberately redundant:
//   1. uart_bridge_ext_is_on_flash_worker() lets THIS function check first
//      and, when true, call save_kibase_job() directly instead of dispatching
//      at all -- see below. This is the primary fix: it is explicit, host-
//      testable (see test_adaptive_tune.c's stub, which models the worker's
//      real non-recursive-lock/depth-1-queue semantics closely enough that a
//      caller which skips this check and dispatches anyway fails loudly
//      instead of hanging), and it is safe for the identical reason the
//      worker's own stack is internal SRAM in the first place: no cache-
//      disable-vs-PSRAM-stack hazard exists once we are already executing on
//      that internal-SRAM stack, so save_kibase_job() may simply run here.
//   2. uart_bridge_ext.c's bx_run_on_internal_stack() ALSO now detects "caller
//      is already the worker task" and runs a dispatched fn() inline rather
//      than deadlocking -- a second, generic backstop for any OTHER re-
//      entrant caller this audit did not know to name explicitly.
bool adaptive_tune_any_write_in_flight(void)
{
    adaptive_tune_ensure_lock();
    xSemaphoreTake(adaptive_tune_lock, portMAX_DELAY);
    bool busy = false;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (adaptive_tune_zones[zi].write_in_flight) {
            busy = true;
        }
    }
    xSemaphoreGive(adaptive_tune_lock);
    return busy;
}

void adaptive_tune_clear_ki_baseline(uint8_t zone_index)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return;
    }
    adaptive_tune_ensure_lock();
    xSemaphoreTake(adaptive_tune_lock, portMAX_DELAY);
    adaptive_tune_zones[zone_index].ki_baseline_valid = false;
    adaptive_tune_zones[zone_index].ki_baseline = 0.0f;
    adaptive_tune_ki_clear_gen[zone_index]++; // F3: invalidates an in-flight run_end plan's re-latch for this zone

    kibase_job_t job = {.result = ESP_FAIL};
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        job.blob.vals[zi] = adaptive_tune_zones[zi].ki_baseline;
        if (adaptive_tune_zones[zi].ki_baseline_valid) {
            job.blob.mask |= (uint8_t)(1u << zi);
        }
    }
    xSemaphoreGive(adaptive_tune_lock);

    esp_err_t err;
    if (uart_bridge_ext_is_on_flash_worker()) {
        // Already on the flash-safe worker's own internal-SRAM stack (the
        // UART-bridge accept path) -- run the save inline. See this
        // function's header comment: dispatching here instead would
        // deadlock the board.
        save_kibase_job(&job);
        err = ESP_OK;
    } else {
        err = uart_bridge_ext_run_on_flash_worker(save_kibase_job, &job);
    }
    if (err != ESP_OK || job.result != ESP_OK) {
        ESP_LOGE(ADAPTIVE_TUNE_TAG, "adaptive_tune_clear_ki_baseline(%u): NVS save failed: %s / %s",
                 (unsigned)zone_index, esp_err_to_name(err), esp_err_to_name(job.result));
        // Live state is still cleared either way -- same "applied live,
        // logged if the save failed" convention as adaptive_tune_set_
        // enabled(). A failed persist here just means a reboot before the
        // NEXT successful save would see the OLD baseline reload -- no worse
        // than today, and strictly better than the ceiling never clearing at
        // all.
    }
}

bool adaptive_tune_get_enabled(uint8_t zone_index)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    adaptive_tune_ensure_lock();
    xSemaphoreTake(adaptive_tune_lock, portMAX_DELAY);
    bool en = adaptive_tune_zones[zone_index].enabled;
    xSemaphoreGive(adaptive_tune_lock);
    return en;
}

// ADAPTIVE_FUZZY_EVALUATION.md sec 3: see adaptive_tune.h's own comment
// on these two. Short, non-blocking, lock-protected reads/writes of a single
// uint8_t -- no producer or blocking call happens under the lock here, same
// discipline as adaptive_tune_get_enabled() above.
uint8_t adaptive_tune_get_fuzzy_confidence_c(uint8_t zone_index)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return 0;
    }
    adaptive_tune_ensure_lock();
    xSemaphoreTake(adaptive_tune_lock, portMAX_DELAY);
    uint8_t c = adaptive_tune_zones[zone_index].fuzzy_confidence_c;
    xSemaphoreGive(adaptive_tune_lock);
    return c;
}

void adaptive_tune_fuzzy_confidence_floor_now(uint8_t zone_index)
{
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        return;
    }
    adaptive_tune_ensure_lock();
    xSemaphoreTake(adaptive_tune_lock, portMAX_DELAY);
    adaptive_tune_zones[zone_index].fuzzy_confidence_c = 0;
    xSemaphoreGive(adaptive_tune_lock);
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
    adaptive_tune_ensure_lock();
    xSemaphoreTake(adaptive_tune_lock, portMAX_DELAY);
    adaptive_tune_zone_t *z = &adaptive_tune_zones[zone_index];
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

    out->joint_observations = adaptive_tune_joint_ring_count;
    out->coupled_attempted = z->coupled_attempted;
    out->coupled_applied = z->coupled_applied;
    out->coupled_cells_changed = z->coupled_cells_changed;
    strncpy(out->coupled_refusal_reason, z->coupled_refusal_reason, sizeof(out->coupled_refusal_reason) - 1);
    out->ki_verdict = z->ki_verdict;
    out->ki_correction_pct = z->ki_correction_pct;
    out->ki_applied = z->ki_applied;
    strncpy(out->ki_refusal_reason, z->ki_refusal_reason, sizeof(out->ki_refusal_reason) - 1);
    out->revert_available = z->revert_available;
    xSemaphoreGive(adaptive_tune_lock);
}

// ---------------------------------------------------------------------
// U1: one-click revert (PID_EXPANSION_PLAN.md 3.3). See adaptive_tune.h's
// own comment on adaptive_tune_revert() for the full contract.
// ---------------------------------------------------------------------

void adaptive_tune_capture_revert_locked(adaptive_tune_zone_t *z, float kp, float ki, float kd, float k_dc,
                                          float tau_s, float dead_time_s)
{
    z->revert_available = true;
    z->revert_kp = kp;
    z->revert_ki = ki;
    z->revert_kd = kd;
    z->revert_k_dc = k_dc;
    z->revert_tau_s = tau_s;
    z->revert_dead_time_s = dead_time_s;
    // Whatever ki_baseline currently is AT THE INSTANT just before the
    // caller's own commit -- see this function's own prototype comment
    // (adaptive_tune_internal.h) and adaptive_tune_revert()'s header
    // comment for why capturing it here, rather than clearing it on
    // revert, is what keeps the baseline consistent with whichever gains
    // end up live.
    z->revert_ki_baseline_valid = z->ki_baseline_valid;
    z->revert_ki_baseline = z->ki_baseline;
}

adaptive_tune_revert_result_t adaptive_tune_revert(uint8_t zone_index, char *reason, size_t reason_cap)
{
    if (reason && reason_cap > 0) {
        reason[0] = '\0';
    }
    if (zone_index >= MAX31856_CHANNEL_COUNT) {
        if (reason) {
            snprintf(reason, reason_cap, "invalid zone index");
        }
        return ADAPTIVE_TUNE_REVERT_INVALID_ZONE;
    }

    // Mid-firing decision (see adaptive_tune.h's own comment on this
    // function): refuse outright while ANY firing is RUNNING or PAUSED,
    // board-wide. profile_executor_get_status() takes and releases s_exec.
    // lock entirely INSIDE this call, strictly before this function ever
    // takes adaptive_tune_lock below -- s_exec.lock -> adaptive_tune_lock,
    // never nested the other way, same order every other call site in this
    // module keeps.
    /* Only state (RUNNING/PAUSED) is needed here -- use the narrow accessor
     * profile_executor.h recommends rather than a 1464-byte profile_exec_
     * status_t stack local. This is reachable from the httpd task
     * (adaptive_tune_http.c's revert POST handler calls straight into this
     * function on its own 8192-byte stack). */
    uint8_t active_id = 0;
    if (profile_executor_get_active_id(&active_id)) {
        if (reason) {
            snprintf(reason, reason_cap, "cannot revert while a firing is running or paused");
        }
        return ADAPTIVE_TUNE_REVERT_FIRING_ACTIVE;
    }

    adaptive_tune_ensure_lock();
    xSemaphoreTake(adaptive_tune_lock, portMAX_DELAY);
    adaptive_tune_zone_t *z = &adaptive_tune_zones[zone_index];
    if (z->write_in_flight) {
        // F3 follow-up: run_end's apply pass (or another revert) is writing this
        // zone with the lock released. Its snapshot may describe a change that
        // has not landed yet; reverting now would be overwritten by it.
        xSemaphoreGive(adaptive_tune_lock);
        if (reason) {
            snprintf(reason, reason_cap, "an adaptive-tune write to this zone is in progress -- retry");
        }
        return ADAPTIVE_TUNE_REVERT_BUSY;
    }
    if (!z->revert_available) {
        xSemaphoreGive(adaptive_tune_lock);
        if (reason) {
            snprintf(reason, reason_cap, "no adaptive-tune change recorded this boot to revert");
        }
        return ADAPTIVE_TUNE_REVERT_NOTHING_TO_REVERT;
    }
    {
        // F2: the snapshot only applies while the live zone still holds exactly what the adaptive
        // commit left. Any other writer since (Accept, PID POST, UART, iter_tune restore, backup
        // import, kiln_cfg apply) makes it stale; reverting would overwrite their newer values.
        float lkp, lki, lkd, lk, lt, ld;
        const bool have = zones_config_get_pid(zone_index, &lkp, &lki, &lkd) &&
                          zones_config_get_model(zone_index, &lk, &lt, &ld);
        if (!have || lkp != z->revert_expect_kp || lki != z->revert_expect_ki || lkd != z->revert_expect_kd ||
            lk != z->revert_expect_k_dc || lt != z->revert_expect_tau_s || ld != z->revert_expect_dead_time_s) {
            z->revert_available = false; // F2 precheck
            z->has_applied = false;
            xSemaphoreGive(adaptive_tune_lock);
            if (reason) {
                snprintf(reason, reason_cap,
                         "zone gains or model were changed by another writer since the adaptive change -- "
                         "nothing to revert");
            }
            return ADAPTIVE_TUNE_REVERT_NOTHING_TO_REVERT;
        }
    }
    z->write_in_flight = true;
    float kp = z->revert_kp, ki = z->revert_ki, kd = z->revert_kd;
    float k_dc = z->revert_k_dc, tau_s = z->revert_tau_s, dead_time_s = z->revert_dead_time_s;
    bool base_valid = z->revert_ki_baseline_valid;
    float base_val = z->revert_ki_baseline;
    const float expect[3] = {z->revert_expect_kp, z->revert_expect_ki, z->revert_expect_kd};
    xSemaphoreGive(adaptive_tune_lock); // never hold this lock across the zone-config write below

    // Direct, unwrapped zones_config_set_*() calls -- this function's only
    // caller (adaptive_tune_http.c's revert_post_handler()) runs on httpd_
    // worker, the same confirmed-not-PSRAM-stacked task zones_pid_post_
    // handler()/adaptive_tune_set_enabled() above already write zones_
    // config from directly, unwrapped -- see either's own comment for the
    // evidence this rests on.
    // F1/F3: one atomic model + gains write (single claim check, single lock section). The expected
    // prior is the adaptive-applied pair, so a writer landing since the check above is detected.
    zones_set_result_t wr = zones_config_set_model_and_pid_checked(zone_index, k_dc, tau_s, dead_time_s, kp, ki, kd,
                                                                   expect);
    bool ok = (wr == ZONES_SET_OK || wr == ZONES_SET_SAVE_FAILED);
    if (!ok) {
        xSemaphoreTake(adaptive_tune_lock, portMAX_DELAY);
        z->write_in_flight = false;
        if (wr == ZONES_SET_STALE_PRIOR) {
            z->revert_available = false; // F2 stale
            z->has_applied = false;
        }
        xSemaphoreGive(adaptive_tune_lock);
        if (wr == ZONES_SET_BUSY_RUNNING) {
            if (reason) {
                snprintf(reason, reason_cap, "a profile or autotune run is active -- zone writes refused");
            }
            return ADAPTIVE_TUNE_REVERT_FIRING_ACTIVE;
        }
        if (wr == ZONES_SET_STALE_PRIOR) {
            if (reason) {
                snprintf(reason, reason_cap, "zone gains were changed by another writer -- nothing to revert");
            }
            return ADAPTIVE_TUNE_REVERT_NOTHING_TO_REVERT;
        }
        if (reason) {
            snprintf(reason, reason_cap, "zone config write rejected the reverted gains");
        }
        return ADAPTIVE_TUNE_REVERT_WRITE_FAILED;
    }

    xSemaphoreTake(adaptive_tune_lock, portMAX_DELAY);
    z->write_in_flight = false;
    // One-shot: consume the snapshot so a second press without a fresh
    // applied change in between reports NOTHING_TO_REVERT honestly, rather
    // than silently reapplying the same old values again.
    z->revert_available = false;
    // Restore the Ki-diagnosis baseline to match -- see adaptive_tune_
    // capture_revert_locked()'s own comment and adaptive_tune_revert()'s
    // header comment for why this is a restore, not a clear.
    z->ki_baseline_valid = base_valid;
    z->ki_baseline = base_val;
    // The applied-change latch this reverts now describes gains that are no
    // longer live -- clear it so zones_page.html's "last applied change"
    // column stops advertising a change that was just undone.
    z->has_applied = false;
    kibase_job_t job = {.result = ESP_FAIL};
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        job.blob.vals[zi] = adaptive_tune_zones[zi].ki_baseline;
        if (adaptive_tune_zones[zi].ki_baseline_valid) {
            job.blob.mask |= (uint8_t)(1u << zi);
        }
    }
    xSemaphoreGive(adaptive_tune_lock);

    // Ki-baseline NVS persist -- same dispatch pattern as adaptive_tune_
    // clear_ki_baseline() above (its own comment has the full re-entrancy
    // reasoning): check uart_bridge_ext_is_on_flash_worker() first and run
    // the save inline if already there, otherwise dispatch. Unlike the
    // migration writes above, the flash worker DOES exist by the time this
    // function can ever run (it is only reachable via an HTTP request,
    // which cannot happen until well after main.c's boot sequence finishes
    // starting every task), so dispatching here is the correct, safe path.
    esp_err_t err;
    if (uart_bridge_ext_is_on_flash_worker()) {
        save_kibase_job(&job);
        err = ESP_OK;
    } else {
        err = uart_bridge_ext_run_on_flash_worker(save_kibase_job, &job);
    }
    if (err != ESP_OK || job.result != ESP_OK) {
        ESP_LOGE(ADAPTIVE_TUNE_TAG, "adaptive_tune_revert(%u): Ki baseline NVS save failed: %s / %s",
                 (unsigned)zone_index, esp_err_to_name(err), esp_err_to_name(job.result));
        // Live state (gains AND baseline) is still reverted either way --
        // same "applied live, logged if the save failed" convention as
        // adaptive_tune_clear_ki_baseline() above.
    }

    if (reason) {
        reason[0] = '\0';
    }
    return ADAPTIVE_TUNE_REVERT_OK;
}

// HTTP registration lives in adaptive_tune_http.c now (GET /api/adaptive_tune,
// POST /api/adaptive_tune/enable) -- split out so this file has no httpd
// dependency at all; adaptive_tune_http.c reaches everything it needs through
// the public accessors below (adaptive_tune_get_enabled/set_enabled/
// get_status). Call adaptive_tune_http_start() separately (main.c does, near
// log_http_start()) once the shared httpd server is up.

void adaptive_tune_init(void)
{
    // ADAPTIVE_TUNE_DUTY_WINDOW_NUM_BUCKETS is a plain integer literal (see
    // its own comment, adaptive_tune_internal.h) that MUST equal WINDOW_S /
    // BUCKET_S -- a float division can't drive a _Static_assert, so this
    // runtime check is what catches a future edit that breaks the
    // relationship (rather than silently mis-sizing the sliding window).
    assert((float)ADAPTIVE_TUNE_DUTY_WINDOW_NUM_BUCKETS * ADAPTIVE_TUNE_DUTY_WINDOW_BUCKET_S ==
           ADAPTIVE_TUNE_DUTY_STABILITY_WINDOW_S);

    adaptive_tune_ensure_lock();
    memset(adaptive_tune_zones, 0, sizeof(adaptive_tune_zones));

    // Boot-time read, direct (not through the flash worker -- see this
    // file's top comment on why reads are exempt).
    adaptive_tune_kibase_blob_t kb;
    memset(&kb, 0, sizeof(kb));
    bool nvs_valid = false;
    uint32_t nvs_rev = 0;

    hal_kv_handle_t h;
    hal_status_t err =
        hal_kv_open(&h, ADAPTIVE_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, ADAPTIVE_TUNE_NVS_PARTITION);
    if (err == HAL_OK) {
        // U2: the opt-in mask is NO LONGER read here -- it moved out of this
        // namespace to the zone config blob (zone_cfg_t::adaptive_tune_
        // enabled). See adaptive_tune_load_enable_flags()/adaptive_tune_
        // migrate_enable_flags() (called separately, later in boot, once
        // zones config has actually loaded) for where an upgrading board's
        // prior en_mask value is carried forward, and why that cannot
        // happen here (this function runs too early -- zones config is not
        // loaded yet at this point in main.c's boot sequence).
        //
        // P1: reload the persisted Ki baseline too -- see adaptive_tune_
        // kibase_blob_t's own comment (adaptive_tune_internal.h) for the
        // on-disk shape, and ki_baseline's own comment for WHY this matters:
        // without this reload, the very first adaptive_tune_refine_ki_locked() call
        // after a reboot would re-latch the baseline from whatever Ki is
        // CURRENTLY live -- which, if this zone's Ki has already grown from
        // prior corrections, is exactly the already-grown value, defeating
        // the whole point of a baseline. ESP_ERR_NVS_NOT_FOUND (never
        // written -- e.g. a zone that has never yet reached adaptive_tune_refine_ki_
        // locked() even once) leaves every zone's ki_baseline_valid at its
        // struct-zero default (false), which is correct: that zone's
        // baseline genuinely has not been established yet, on this boot or
        // any previous one.
        size_t kb_len = sizeof(kb);
        if (hal_kv_get_blob(&h, ADAPTIVE_TUNE_NVS_KEY_KIBASE, &kb, &kb_len) == HAL_OK && kb_len == sizeof(kb)) {
            nvs_valid = true;
            uint32_t rev = 0;
            if (hal_kv_get_u32(&h, ADAPTIVE_TUNE_NVS_KEY_KIBASE_REV, &rev) == HAL_OK) {
                nvs_rev = rev;
            }
        }
        hal_kv_close(&h);
    }
    // ESP_ERR_NVS_NOT_FOUND (namespace never written) leaves every zone at
    // its struct-zero default: enabled = false. DEFAULT OFF, as required --
    // and nvs_valid stays false, so the resolve below falls through entirely
    // to whatever the file alone can offer (a fresh board with no NVS
    // namespace yet can still have a valid file from a prior boot's save).

    // cfg-filesystem read-through (docs/FILESYSTEM_USER_DATA.md section
    // 5 item 9) -- see adaptive_tune_internal.h's ADAPTIVE_TUNE_KIBASE_FILE_PATH
    // comment for why this item gets the simplified generic-bridge treatment
    // rather than zones config's bespoke divergence forensics.
    /* adaptive_tune_init() runs from profile_executor_start(), called by
     * main_control_bringup.c BEFORE uart_bridge_ext_start_flash_worker() is
     * called later in that same function -- the identical boot-ordering
     * race relay_cycles_init() has (see that function's matching comment)
     * and cfg_fs_mount.c's auto-format path had before 1136c0a9. Bounded
     * wait first, then let the write attempt proceed either way, recording
     * whether the worker was actually up so a dropped write is not silent. */
    s_kibase_migration_worker_wait_deferred = !flash_worker_wait_default();
    if (s_kibase_migration_worker_wait_deferred) {
        ESP_LOGW(ADAPTIVE_TUNE_TAG, "flash-safe worker still not started -- kibase migrate-on-load write may "
                                     "be dropped this boot; see GET /api/cfgfs");
    }

    adaptive_tune_kibase_blob_t resolved;
    uint32_t resolved_rev = nvs_rev;
    bool used_file = false;
    bool have_value = pref_cfg_fs_resolve(ADAPTIVE_TUNE_KIBASE_FILE_PATH, &kb, sizeof(kb), nvs_valid, nvs_rev,
                                           adaptive_tune_kibase_file_validate, &resolved, &resolved_rev, &used_file);
    if (have_value) {
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (resolved.mask & (1u << zi)) {
                adaptive_tune_zones[zi].ki_baseline_valid = true;
                adaptive_tune_zones[zi].ki_baseline = resolved.vals[zi];
            }
        }
        s_kibase_rev = resolved_rev;
        if (used_file) {
            ESP_LOGI(ADAPTIVE_TUNE_TAG, "ki-baseline loaded from cfg filesystem (rev=%lu)",
                     (unsigned long)resolved_rev);
        }
    } else {
        s_kibase_rev = 0;
    }
    //
    // No httpd registration here any more -- call adaptive_tune_http_start()
    // separately once the shared httpd server is up (see adaptive_tune_http.c).

    // Coupled-solve breadcrumb check, once per boot (see docs/audits/
    // profile_executor_stop_panic_static_narrowing_2026-09-16.md): the
    // breadcrumb lives in RTC_NOINIT_ATTR storage, so it survives exactly
    // the kind of software reset (panic/watchdog) this module is trying to
    // catch, but NOT a power cycle -- a cold-booted board reads it as
    // magic-mismatched (false) below, same as "never ran", which is
    // correct and not a false negative worth chasing. adaptive_tune_init()
    // runs once per profile_executor_start() call, not once per physical
    // boot, so the static guard below is what limits this to one log line
    // per boot rather than once per firing.
    {
        static bool s_coupled_bc_checked_this_boot = false;
        if (!s_coupled_bc_checked_this_boot) {
            s_coupled_bc_checked_this_boot = true;
            adaptive_tune_coupled_breadcrumb_t bc;
            if (adaptive_tune_coupled_breadcrumb_get(&bc) && adaptive_tune_coupled_breadcrumb_is_mid_solve(&bc)) {
                ESP_LOGE(ADAPTIVE_TUNE_TAG,
                         "PREVIOUS BOOT RESET WHILE A COUPLED SOLVE WAS IN PROGRESS -- zone=%lu stage=%lu "
                         "joint_obs=%lu solve_row=%lu stack_hwm=%lu bytes (seq=%lu); see docs/audits/"
                         "profile_executor_stop_panic_static_narrowing_2026-09-16.md",
                         (unsigned long)bc.zone_index, (unsigned long)bc.stage, (unsigned long)bc.joint_observations,
                         (unsigned long)bc.solve_row, (unsigned long)bc.stack_hwm, (unsigned long)bc.seq);
            }
        }
    }
}

bool adaptive_tune_kibase_migration_worker_wait_deferred(void)
{
    return s_kibase_migration_worker_wait_deferred;
}

// GET /api/cfgfs dual-write picture for the Ki-baseline blob -- 2026-09-08,
// moving this item's reporting out of cfg_fs_status.c's stale "nvs_only"
// hardcoded list (docs/FILESYSTEM_USER_DATA.md's Ki-baseline bridge,
// step 9, landed in 762bb29e). Read-only: unlike adaptive_tune_init()'s
// boot-time load, this does NOT call pref_cfg_fs_resolve() (no resync
// write) -- same "a status GET must never heal or mask a divergence"
// discipline every sibling *_get_dualwrite_status() follows. `diverged`
// uses cfg_fs_status_item_diverged() (a real field-by-field compare of
// adaptive_tune_kibase_blob_t -- mask AND every zone's baseline value, not
// just the mask; padding after `mask` is not data and is never compared),
// not a rev-only guess. All five output pointers accept
// NULL.
void adaptive_tune_get_kibase_dualwrite_status(bool *file_valid, uint32_t *file_rev, bool *nvs_valid,
                                                uint32_t *nvs_rev, bool *diverged)
{
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

    adaptive_tune_kibase_blob_t f_blob;
    memset(&f_blob, 0, sizeof(f_blob));
    uint32_t f_rev = 0;
    bool f_valid = false;
    pref_cfg_fs_load_raw(ADAPTIVE_TUNE_KIBASE_FILE_PATH, sizeof(f_blob), adaptive_tune_kibase_file_validate, &f_blob, &f_rev,
                          &f_valid);

    adaptive_tune_kibase_blob_t n_blob;
    memset(&n_blob, 0, sizeof(n_blob));
    bool n_valid = false;
    uint32_t n_rev = 0;
    hal_kv_handle_t h;
    if (hal_kv_open(&h, ADAPTIVE_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, ADAPTIVE_TUNE_NVS_PARTITION) == HAL_OK) {
        size_t len = sizeof(n_blob);
        if (hal_kv_get_blob(&h, ADAPTIVE_TUNE_NVS_KEY_KIBASE, &n_blob, &len) == HAL_OK && len == sizeof(n_blob)) {
            n_valid = true;
            uint32_t rev = 0;
            if (hal_kv_get_u32(&h, ADAPTIVE_TUNE_NVS_KEY_KIBASE_REV, &rev) == HAL_OK) {
                n_rev = rev;
            }
        }
        hal_kv_close(&h);
    }

    /* Field by field, not memcmp() of the whole struct: adaptive_tune_kibase_blob_t has 3
     * padding bytes after `mask`, and padding is not data (see docs/CONFIG_FILESYSTEM.md). */
    bool content_equal = f_valid && n_valid && f_blob.mask == n_blob.mask
                         && memcmp(f_blob.vals, n_blob.vals, sizeof(f_blob.vals)) == 0;
    if (file_valid) {
        *file_valid = f_valid;
    }
    if (file_rev) {
        *file_rev = f_rev;
    }
    if (nvs_valid) {
        *nvs_valid = n_valid;
    }
    if (nvs_rev) {
        *nvs_rev = n_rev;
    }
    if (diverged) {
        *diverged = cfg_fs_status_item_diverged(f_valid, n_valid, content_equal);
    }
}
