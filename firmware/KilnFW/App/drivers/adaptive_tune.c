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

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

const char *ADAPTIVE_TUNE_TAG = "adaptive_tune";

adaptive_tune_zone_t s_at_zones[MAX31856_CHANNEL_COUNT];

// ---------------------------------------------------------------------
// Joint (all-zone) dwell observations for the coupled solve -- module-wide,
// not per-zone, because a "joint observation" is every zone's duty/rise at
// the SAME instant. Guarded by the same s_lock as s_at_zones[] above.
// ---------------------------------------------------------------------

adaptive_tune_joint_obs_t s_joint_ring[ADAPTIVE_TUNE_JOINT_RING_CAPACITY];
uint32_t s_joint_ring_count;
uint32_t s_joint_ring_head;
uint32_t s_joint_observations_lifetime;

// True once a joint row has been committed for the CURRENT dwell -- reset
// whenever any zone begins a fresh dwell (adaptive_tune_zone_tick()'s "just
// entered this dwell" branch). Every enabled zone settles independently
// (each has its own settle_start_c/settle_elapsed_s), so with N zones
// dwelling at the same operating point, each one crossing its settle floor
// used to commit its OWN joint row from the same s_joint_last_duty/rise_c
// snapshot -- one physical dwell (one distinct operating point) silently
// contributing N near-identical rows to the ring. try_refine_coupled_
// locked()'s "N joint observations" floor is a rank/conditioning
// requirement on DISTINCT equations; duplicate rows inflate the count
// without adding one. Gating the commit on this flag makes ring rows and
// distinct dwells the same number again, so the existing floor check is
// correct without a second counter. This assumes every enabled zone shares
// the same profile segment boundaries (true for this firmware -- all zones
// in a firing follow the same profile), so "any zone enters a fresh dwell"
// is a reasonable proxy for "a new dwell has begun" module-wide.
bool s_joint_dwell_row_committed;

// Latest known duty/rise for every zone, updated on EVERY tick for EVERY
// zone regardless of that zone's own opt-in flag -- a zone that has not
// opted its own row into learning is still a valid NEIGHBOUR column in
// another zone's coupled row, so its duty must still be tracked. Frozen
// (not reset to 0/NaN) on an invalid tick, same "hold last value" posture
// as zone_coupling_filter_tick() -- a stale value sitting unused until the
// next valid tick does no harm; only last_valid gates whether it is ever
// read.
float s_joint_last_duty[MAX31856_CHANNEL_COUNT];
float s_joint_last_rise_c[MAX31856_CHANNEL_COUNT];
bool  s_joint_last_valid[MAX31856_CHANNEL_COUNT];
SemaphoreHandle_t s_lock; // guards s_at_zones; taken only from this file, never across profile_executor.c's
                          // s_exec.lock (see adaptive_tune.h's doc comment on the lock order this keeps)
bool s_lock_ready;

void ensure_lock(void)
{
    if (!s_lock_ready) {
        s_lock = xSemaphoreCreateMutex();
        s_lock_ready = true;
    }
}

void set_refusal(adaptive_tune_zone_t *z, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(z->last_refusal_reason, sizeof(z->last_refusal_reason), fmt, ap);
    va_end(ap);
    ESP_LOGI(ADAPTIVE_TUNE_TAG, "refined not applied: %s", z->last_refusal_reason);
}

void set_reason(char *buf, size_t bufsz, const char *fmt, ...)
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
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    adaptive_tune_zone_t *z = &s_at_zones[zone_index];

    // Joint duty/rise cache: updated for EVERY zone on EVERY tick,
    // regardless of that zone's own opt-in flag -- see s_joint_last_duty[]'s
    // own comment above. Deliberately BEFORE the enabled/actual_valid/
    // dwelling gates below: an opted-out zone, or one whose own reading is
    // bad this instant, must still contribute its latest-known duty as a
    // neighbour column for another zone's coupled row.
    if (actual_valid && !isnan(ambient_c) && isfinite(actual_c) && isfinite(ambient_c)) {
        s_joint_last_duty[zone_index] = duty;
        s_joint_last_rise_c[zone_index] = actual_c - ambient_c;
        s_joint_last_valid[zone_index] = true;
    } else {
        s_joint_last_valid[zone_index] = false;
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
    //       s_joint_dwell_row_committed and admitting a second joint row
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
        // for it (see s_joint_dwell_row_committed's own comment: this zone
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
        s_joint_dwell_row_committed = false;
    }
    if (!dwelling) {
        // Not dwelling any more (or not yet) -- keep the settle tracker
        // clean so the next dwell starts from a baseline uncontaminated by
        // this tick, whatever z->enabled/actual_valid say about it.
        z->settle_start_valid = false;
        z->settle_elapsed_s = 0.0f;
        z->recorded_this_dwell = false;
    }

    if (!z->enabled || !actual_valid || isnan(ambient_c)) {
        // Learning is off, or this tick has nothing trustworthy to offer.
        // dwelling_prev and the fresh-dwell reset above already ran
        // unconditionally, so there is nothing left to do here.
        xSemaphoreGive(s_lock);
        return;
    }

    if (!dwelling) {
        xSemaphoreGive(s_lock);
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
    }
    z->settle_elapsed_s += dt_s;

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

    // Joint (all-zone) row for the coupled solve, committed at the SAME
    // settle instant as this zone's own diagonal point above -- only if
    // EVERY zone (this one included) currently has a fresh, valid,
    // above-floor duty reading. A partial row (a neighbour never having
    // ticked yet, or sitting invalid/too-low this instant) is discarded
    // outright rather than committed with a placeholder -- a zero-filled
    // column would silently poison that zone out of every future row's
    // design matrix instead of just being absent from this one.
    {
        bool joint_ok = !s_joint_dwell_row_committed; // see s_joint_dwell_row_committed's own comment --
                                                       // at most one joint row per distinct dwell
        for (uint8_t j = 0; joint_ok && j < MAX31856_CHANNEL_COUNT; j++) {
            if (!s_joint_last_valid[j] || s_joint_last_duty[j] < ADAPTIVE_TUNE_JOINT_MIN_DUTY) {
                joint_ok = false;
            }
        }
        if (joint_ok) {
            s_joint_dwell_row_committed = true;
            uint32_t jslot = (s_joint_ring_head + s_joint_ring_count) % ADAPTIVE_TUNE_JOINT_RING_CAPACITY;
            if (s_joint_ring_count < ADAPTIVE_TUNE_JOINT_RING_CAPACITY) {
                s_joint_ring_count++;
            } else {
                s_joint_ring_head = (s_joint_ring_head + 1) % ADAPTIVE_TUNE_JOINT_RING_CAPACITY;
            }
            memcpy(s_joint_ring[jslot].duty, s_joint_last_duty, sizeof(s_joint_last_duty));
            memcpy(s_joint_ring[jslot].rise_c, s_joint_last_rise_c, sizeof(s_joint_last_rise_c));
            s_joint_observations_lifetime++;
        }
    }

    // The Ki diagnosis itself runs at adaptive_tune_run_end(), not here --
    // it needs profile_exec_firing_stats_t's dwell_err_mean_c/dwell_err_max_c
    // (the SETPOINT-aware error figures profile_executor.c already computes;
    // this file never receives setpoint_c per tick, see adaptive_tune.c's
    // top comment), which only arrive with the run record. z->trace_* above
    // is left as-is (this dwell's most recent trace) for that call to read.

    xSemaphoreGive(s_lock);
}

// H2/K1/K2 fix: every run_end path that skips both try_refine_zone_locked()
// and try_refine_ki_locked()/try_refine_coupled_locked() entirely must reset
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
    set_refusal(z, "%s", reason);
    set_reason(z->coupled_refusal_reason, sizeof(z->coupled_refusal_reason), "%s", reason);
    z->coupled_attempted = false;
    z->coupled_applied = false;
    z->coupled_cells_changed = 0;
    set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason), "%s", reason);
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

static void save_kibase_job(void *arg)
{
    kibase_job_t *job = (kibase_job_t *)arg;
    nvs_handle_t h;
    esp_err_t err =
        nvs_open_from_partition(ADAPTIVE_TUNE_NVS_PARTITION, ADAPTIVE_TUNE_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        job->result = err;
        return;
    }
    err = nvs_set_blob(h, ADAPTIVE_TUNE_NVS_KEY_KIBASE, &job->blob, sizeof(job->blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    job->result = err;
}

void adaptive_tune_run_end(const profile_firing_run_record_t *rec, bool clean)
{
    if (!rec) {
        return;
    }
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // P1: tracks whether ANY zone's ki_baseline_valid flipped false -> true
    // during this run's loop below -- the NVS write dispatched after the
    // lock is released only actually needs to happen when this is true (a
    // baseline, once persisted, never changes again -- see ki_baseline's
    // own "latched once, never overwritten" comment), but it costs nothing
    // to always snapshot the full current state into kibase_snapshot while
    // still holding the lock, so the dispatch below is a plain memcpy-then-
    // write with no second lock acquisition needed.
    bool baseline_newly_latched = false;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        adaptive_tune_zone_t *z = &s_at_zones[zi];
        const profile_firing_zone_record_t *zr = &rec->zones[zi];

        // K1: single skip decision for this zone -- see block comment above.
        const char *skip_reason = NULL;
        char excluded_reason_buf[96]; // must outlive the chain below -- so skip_reason never dangles
        if (!z->enabled) {
            skip_reason = "zone not opted into adaptive tuning -- not used as training data";
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
            continue;
        }
        // D5: try_refine_zone_locked() rewrites Kp/Ki/Kd from a fresh SIMC
        // recompute when it applies. try_refine_ki_locked() diagnoses Ki
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
        bool model_refined = try_refine_zone_locked(zi, rec->profile_id);
        try_refine_coupled_locked(zi);
        if (!model_refined) {
            bool baseline_was_valid = z->ki_baseline_valid; // P1: see baseline_newly_latched's own comment
            try_refine_ki_locked(zi, &zr->stats);
            if (!baseline_was_valid && z->ki_baseline_valid) {
                baseline_newly_latched = true;
            }
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
            set_reason(z->ki_refusal_reason, sizeof(z->ki_refusal_reason),
                       "Ki diagnosis skipped this run -- the model/PID refinement already rewrote Ki from SIMC, "
                       "see adaptive_tune_run_end()'s D5 comment");
        }
    }

    // P1: snapshot the current baseline state for persistence WHILE still
    // holding the lock -- cheap (a memcpy-sized loop), and the alternative
    // (re-taking the lock after it is released below) would just be more
    // code for no benefit.
    kibase_job_t job = {.result = ESP_FAIL};
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        job.blob.vals[zi] = s_at_zones[zi].ki_baseline;
        if (s_at_zones[zi].ki_baseline_valid) {
            job.blob.mask |= (uint8_t)(1u << zi);
        }
    }
    xSemaphoreGive(s_lock);

    // Dispatched OUTSIDE the lock, only when a baseline actually changed --
    // same "never wait on the flash worker with s_lock held" rule as
    // adaptive_tune_set_enabled() below. A run that latched no NEW baseline
    // (every zone already had one, or none reached try_refine_ki_locked()
    // at all this run) skips the write entirely -- there is nothing new to
    // persist, and en_mask-style writing the identical bytes back every run
    // would just be wear for no benefit.
    if (baseline_newly_latched) {
        esp_err_t err = uart_bridge_ext_run_on_flash_worker(save_kibase_job, &job);
        if (err != ESP_OK || job.result != ESP_OK) {
            ESP_LOGE(ADAPTIVE_TUNE_TAG, "adaptive_tune_run_end(): Ki baseline NVS save failed: %s / %s",
                     esp_err_to_name(err), esp_err_to_name(job.result));
        }
    }
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
        if (s_at_zones[zi].enabled) {
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
    s_at_zones[zone_index].enabled = enabled;
    enmask_job_t job = {.mask = enmask_locked(), .result = ESP_FAIL};
    xSemaphoreGive(s_lock);

    // Dispatched OUTSIDE the lock -- uart_bridge_ext_run_on_flash_worker()
    // blocks the calling task until the worker task runs the job (see that
    // function's own doc comment), and this file's lock must not be held
    // across a wait on a different task.
    esp_err_t err = uart_bridge_ext_run_on_flash_worker(save_enmask_job, &job);
    if (err != ESP_OK || job.result != ESP_OK) {
        ESP_LOGE(ADAPTIVE_TUNE_TAG, "adaptive_tune_set_enabled(%u,%d): NVS save failed: %s / %s",
                 (unsigned)zone_index, (int)enabled, esp_err_to_name(err), esp_err_to_name(job.result));
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
    bool en = s_at_zones[zone_index].enabled;
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
    adaptive_tune_zone_t *z = &s_at_zones[zone_index];
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

    out->joint_observations = s_joint_ring_count;
    out->coupled_attempted = z->coupled_attempted;
    out->coupled_applied = z->coupled_applied;
    out->coupled_cells_changed = z->coupled_cells_changed;
    strncpy(out->coupled_refusal_reason, z->coupled_refusal_reason, sizeof(out->coupled_refusal_reason) - 1);
    out->ki_verdict = z->ki_verdict;
    out->ki_correction_pct = z->ki_correction_pct;
    out->ki_applied = z->ki_applied;
    strncpy(out->ki_refusal_reason, z->ki_refusal_reason, sizeof(out->ki_refusal_reason) - 1);
    xSemaphoreGive(s_lock);
}

// HTTP registration lives in adaptive_tune_http.c now (GET /api/adaptive_tune,
// POST /api/adaptive_tune/enable) -- split out so this file has no httpd
// dependency at all; adaptive_tune_http.c reaches everything it needs through
// the public accessors below (adaptive_tune_get_enabled/set_enabled/
// get_status). Call adaptive_tune_http_start() separately (main.c does, near
// log_http_start()) once the shared httpd server is up.

void adaptive_tune_init(void)
{
    ensure_lock();
    memset(s_at_zones, 0, sizeof(s_at_zones));

    // Boot-time read, direct (not through the flash worker -- see this
    // file's top comment on why reads are exempt).
    nvs_handle_t h;
    esp_err_t err =
        nvs_open_from_partition(ADAPTIVE_TUNE_NVS_PARTITION, ADAPTIVE_TUNE_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        uint8_t mask = 0;
        if (nvs_get_u8(h, ADAPTIVE_TUNE_NVS_KEY_ENMASK, &mask) == ESP_OK) {
            for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
                s_at_zones[zi].enabled = (mask & (1u << zi)) != 0;
            }
        }
        // P1: reload the persisted Ki baseline too -- see adaptive_tune_
        // kibase_blob_t's own comment (adaptive_tune_internal.h) for the
        // on-disk shape, and ki_baseline's own comment for WHY this matters:
        // without this reload, the very first try_refine_ki_locked() call
        // after a reboot would re-latch the baseline from whatever Ki is
        // CURRENTLY live -- which, if this zone's Ki has already grown from
        // prior corrections, is exactly the already-grown value, defeating
        // the whole point of a baseline. ESP_ERR_NVS_NOT_FOUND (never
        // written -- e.g. a zone that has never yet reached try_refine_ki_
        // locked() even once) leaves every zone's ki_baseline_valid at its
        // struct-zero default (false), which is correct: that zone's
        // baseline genuinely has not been established yet, on this boot or
        // any previous one.
        adaptive_tune_kibase_blob_t kb;
        memset(&kb, 0, sizeof(kb));
        size_t kb_len = sizeof(kb);
        if (nvs_get_blob(h, ADAPTIVE_TUNE_NVS_KEY_KIBASE, &kb, &kb_len) == ESP_OK && kb_len == sizeof(kb)) {
            for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
                if (kb.mask & (1u << zi)) {
                    s_at_zones[zi].ki_baseline_valid = true;
                    s_at_zones[zi].ki_baseline = kb.vals[zi];
                }
            }
        }
        nvs_close(h);
    }
    // ESP_ERR_NVS_NOT_FOUND (namespace never written) leaves every zone at
    // its struct-zero default: enabled = false. DEFAULT OFF, as required.
    //
    // No httpd registration here any more -- call adaptive_tune_http_start()
    // separately once the shared httpd server is up (see adaptive_tune_http.c).
}
