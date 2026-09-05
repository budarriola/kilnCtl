#ifndef AUTOTUNE_ENGINE_INTERNAL_H
#define AUTOTUNE_ENGINE_INTERNAL_H

/* Internal seams for the autotune_engine.c split (2026-09-04, ROADMAP.md M15
 * item A3: "files over 1500 lines should be broken up where it makes sense"
 * -- autotune_engine.c had grown to 4120 lines. Same precedent and rationale
 * as profile_executor.c's own split (profile_executor_internal.h, 2026-09-01)
 * -- see that header's own top comment for the general shape of this move.
 * This header is NOT public API -- autotune_engine.h stays that -- it exists
 * purely so pieces that used to be one translation unit (and could reach
 * each other's `static` state and helpers for free) can still do so now that
 * they are five:
 *
 *   autotune_engine.c                -- task/lifecycle, the tick state
 *                                        machine (autotune_engine_tick_
 *                                        locked(), shared by both methods),
 *                                        autotune_begin_run_locked(), autotune_engine_
 *                                        run()/run_to_target(), get_status(),
 *                                        get_trace(), is_active()/
 *                                        is_active_on_zone()
 *   autotune_engine_guard.c          -- shared low-level plumbing (relay
 *                                        apply/release, escalate/abort,
 *                                        threshold scaling, trace unpack,
 *                                        "is a run in progress") plus the
 *                                        two persistence entry points,
 *                                        autotune_engine_abort()/accept()
 *   autotune_engine_step_identify.c  -- STEP method: settle/onset detection,
 *                                        the FOPDT fit and its guards
 *                                        (autotune_finalize_fit()), target-mode probe
 *                                        handling, pre-start thermal
 *                                        readiness
 *   autotune_engine_relay.c          -- RELAY method: the bang-bang law,
 *                                        the relay fit, autotune_engine_
 *                                        run_relay()
 *   autotune_engine_coupling.c       -- cross-zone coupling-matrix
 *                                        persistence job, the coupling
 *                                        matrix getter, RGA computation
 *
 * Every symbol declared below was `static` in the original single file and
 * is widened to file-scope-internal linkage ONLY because a sibling .c file
 * in this split now calls or reads it directly -- see each new file's own
 * top-of-file comment for which of these it defines vs. only consumes.
 * s_at_t/etc. moved here unchanged so all five files see the identical
 * layout; the original anonymous `static s_at_t s_at;` is now defined
 * (non-static) in autotune_engine.c and `extern`-declared here. Same for
 * AT_TAG -- non-static in autotune_engine.c, `extern` here.
 *
 * test_autotune_engine_prestart.c #includes every one of these five .c
 * files directly into one translation unit (same convention as
 * test_profile_executor_prestart.c's #include of its own split -- see that
 * file's header comment), so AT_TAG is defined once, non-static, in
 * autotune_engine.c and only declared `extern` here -- five independent
 * `static const char *AT_TAG = ...` in one TU would be a redefinition error
 * there even though it's fine in the normal separate-TU firmware build. */

#include "autotune_engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "adaptive_tune.h" /* adaptive_tune_clear_ki_baseline() -- see this file's accept-path call site */
#include "heat_enable.h"
#include "heater_output.h"
#include "kiln_io_owner.h"
#include "ota_http.h" /* ota_http_heat_blocked_by_update() -- heat_interlock.h's own doc comment */
#include "profile_executor.h"
#include "relay_authority.h"
#include "relay_cycles.h"
#include "safety_trip_words.h" /* safety_fault_source_words() -- ROADMAP.md M13, decode the
                                 * fault-source mask for the operator instead of a bare hex value */
#include "sim_backend.h"
#include "stack_margin.h"
#include "thermo_combine.h"
#include "zones_config_accessors.h"
/* uart_bridge_ext.c's flash-safe executor (bx_flash_worker) -- see this
 * file's coupling_persist_job()/autotune_finalize_fit() comments below for why
 * autotune_finalize_fit()'s NVS write is routed through it rather than executed
 * directly on task_entry()'s own PSRAM-stacked task. Same mechanism, same
 * precedent as safety_cfg_store.c's nvs_save_store_job(), and the exact
 * hazard uart_bridge_ext.c:104-127's HAZARD block documents.
 *
 * Declared here by hand rather than via #include "uart_bridge.h", for the
 * same reason safety_cfg_store.c gives: that header pulls in hardware
 * dependencies (ILI9488.h/screen_idle.h/kiln_io.h) this file neither needs
 * nor wants, and which are not part of this file's host-test stub surface.
 * The real declaration and its full doc comment live in uart_bridge.h; this
 * one must be kept in sync with it by hand if that signature ever changes. */
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg);

extern const char *AT_TAG;

/* Pre-start warnings from the POLLED readers below, throttled to one line
 * each per boot.
 *
 * The action entry points (run/halt/pause/resume) log every time, which is
 * right -- each is a discrete operator request that got refused, and there
 * are never many. The readers are different: safety_link.c's poll task calls
 * profile_executor_get_status() every 500 ms, the dashboard polls it every
 * 2 s, and every GET /api/status hits it too. In recovery mode, where this
 * module is deliberately never started, an unthrottled warning there is a
 * continuous stream that floods the UART log bridge (this board already
 * drops lines when that queue fills) and buries the recovery-mode banner --
 * degrading exactly the mode these guards exist to make survivable. Once per
 * boot says everything a reader needs; the hundredth copy says nothing. */
#define LOG_PRESTART_ONCE(msg)                                                                       do {                                                                                                  static bool s_warned_once = false;                                                                if (!s_warned_once) {                                                                                 s_warned_once = true;                                                                             ESP_LOGW(AT_TAG, msg " (further occurrences this boot are suppressed)");                         }                                                                                              } while (0)

#define HEATER_WINDOW_MS HEATER_DEFAULT_WINDOW_MS
#define HEATER_MIN_ON_MS HEATER_DEFAULT_MIN_ON_MS
#define HEATER_MIN_OFF_MS HEATER_DEFAULT_MIN_OFF_MS

/* "Reached steady state" heuristic for ending STEPPING early (TODO.md 6A.4
 * doesn't mandate a specific detector -- the two-point fit itself is what
 * validates the trace is usable).
 *
 * 2026-08-31 FIX -- the original detector declared settled once the last
 * SETTLE_CHECK_SAMPLES samples all fell within a fixed SETTLE_CHECK_BAND_C
 * (1.0 degC) of each other. That band is absolute, but a kiln's rate of
 * climb is not: a slow ramp with a time constant of tens of minutes moves
 * comfortably less than 1.0 degC in the 50s window (SETTLE_CHECK_SAMPLES-1)
 * 10s-samples span *while still rising*, so the old detector fired
 * mid-transient every time, at the earliest possible moment
 * (MIN_STEPPING_SAMPLES_BEFORE_SETTLE_CHECK * 10s = 120s in). autotune_finalize_fit()
 * then fit a FOPDT model against a trace that never got near its asymptote,
 * producing a k_gain_c_per_duty that massively underestimates the plant's
 * real static gain -- confirmed on real traces (21.74 and 31.96 degC/duty,
 * implying a ~42-52 degC ceiling on a kiln that reaches hundreds).
 * Downstream, zone_feedforward() divides by that gain and saturates at full
 * duty just above the implied ceiling, which is the 10 degC overshoot this
 * whole fix exists to close. (pid_autotune_fit_fopdt() ALSO now extrapolates
 * to the asymptote rather than trusting the last sample -- see that
 * function's own comment -- so this detector's job is narrower than it used
 * to be: it only decides when a fit is trustworthy enough to STOP the run
 * early, not whether the resulting K is biased.)
 *
 * Replacement: settled is now relative to THIS RUN's own observed dynamics,
 * not a fixed band. autotune_engine_tick_locked() tracks the peak slope
 * (degC/s) seen so far this STEPPING phase (s_at.step_peak_slope_c_per_s)
 * and declares settled once the recent slope (trailing SETTLE_CHECK_SAMPLES
 * window) has fallen to SETTLE_RELATIVE_SLOPE_FRAC or less of that peak.
 *
 * 2026-09-02 review fix -- BE HONEST ABOUT WHAT THIS ACTUALLY DOES: the
 * paragraph above is true in principle but misleading in practice at
 * realistic kiln slopes. recent_slope is quantized to the trace's 0.1 degC
 * packing over a 50s window, i.e. multiples of 0.002 degC/s. The relative
 * arm (recent_slope <= SETTLE_RELATIVE_SLOPE_FRAC*peak, 4%) can only ever
 * admit ONE quantum step when peak >= 0.002/0.04 = 0.05 degC/s (180 degC/hr)
 * and only beats SETTLE_ABS_SLOPE_FLOOR_C_PER_S (0.003) outright when peak >
 * 0.075 degC/s (270 degC/hr). A realistic kiln's peak slope right after
 * dead time is more often in the ~0.03 degC/s range -- below both of those
 * thresholds -- so in practice the relative arm reduces to `recent_slope ==
 * 0` exactly (the trailing window reads bit-identical across its 6
 * samples), and SETTLE_ABS_SLOPE_FLOOR_C_PER_S is the operative criterion
 * doing the actual work, not the "relative to peak" ratio the name and the
 * paragraph above suggest. This is still a real, large improvement over the
 * old fixed 1.0 degC/60s band (roughly a 10x tighter bar, and the
 * comparison is now against THIS run's own noise floor rather than an
 * arbitrary absolute degree count) and it is conservative in the safe
 * direction (harder to satisfy than the name implies, not easier), so it is
 * being KEPT as-is rather than re-tuned -- but a reader relying on "decays
 * to 4% of peak" as a precise description of on-target behavior would be
 * wrong; "decays to bit-exact flat, or to 4% of an unusually fast peak" is
 * the honest version.
 *
 * 2026-09-01 review fixes (three defects found in the first version of this
 * detector, each independently capable of reproducing the original bug):
 *
 *   (3) A second "criterion 2" (elapsed stepping time >= SETTLE_MIN_TAU_
 *       MULTIPLE * an online tau estimate derived from peak/recent slope)
 *       was removed. It looked like an independent check but wasn't: the
 *       online tau estimate is ALGEBRAICALLY DERIVED from the same
 *       peak/recent ratio criterion 1 already tests (tau_estimate =
 *       time_since_peak / ln(peak/recent)), so "elapsed >= MULTIPLE *
 *       tau_estimate" reduces to a restatement of criterion 1's ratio bound
 *       in different units -- with SETTLE_RELATIVE_SLOPE_FRAC=0.04 (ratio
 *       >= 25, ln >= 3.219) and the old MULTIPLE=3.0, criterion 2 was
 *       provably true whenever criterion 1 was, for any peak that occurred
 *       in the first 93% of the elapsed run -- i.e. essentially always. A
 *       single-criterion detector that says what it does is more honest
 *       than a two-criterion one where the second criterion is vacuous
 *       dead weight; if a genuine SECOND signal is wanted later, it needs
 *       to come from something criterion 1 doesn't already encode (e.g. an
 *       independent trace-shape check), not another function of the same
 *       peak/recent ratio.
 *
 *   (4) step_peak_slope_c_per_s was tracked from raw adjacent-sample
 *       differences (a single 10s interval) with no restriction on WHEN in
 *       the run it could be set. That is the most noise-amplified slope
 *       estimator available, and letting it run for the whole STEPPING
 *       phase means one 1-2 degC noise spike anywhere -- not just near the
 *       true dead-time-adjacent peak -- inflates "peak" and, because
 *       criterion 1 divides by it, makes the detector fire EASIER, i.e.
 *       reproduces the original mid-transient-settle bug through a
 *       different door. Fixed: the peak is now measured the same way
 *       "recent slope" is (a first-to-last-of-window rate over
 *       PEAK_SLOPE_WINDOW_SAMPLES samples, not a raw 2-point difference).
 *
 *   (4b) 2026-09-02 review fix -- the first version of (4)'s fix restricted
 *       the peak search to a FIXED PEAK_SLOPE_SEARCH_SAMPLES (30) from the
 *       start of STEPPING, on the assumption that dead time is always
 *       short relative to that window. It is not: a zone whose dead time
 *       exceeds 300s never sees its peak recorded at all (stays ~0
 *       forever), which permanently fails the peak-floor gate below and
 *       burns the full AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S budget
 *       marked unsettled, however cleanly the plant actually responded.
 *       Worse, a coincidental pair of quantization ticks landing in one
 *       window DURING that same dead time (0.1+0.1 degC / 50s = 0.004,
 *       just over the old-style floor) could latch a spurious tiny "peak"
 *       and let the detector declare settled while still inside dead time,
 *       producing a spurious "response too small to fit" abort from
 *       pid_autotune's own noise floor.
 *
 *       Fixed by anchoring the search window to DETECTED response onset
 *       instead of a fixed sample count from run start: the peak is not
 *       tracked at all until the windowed slope first clears
 *       RESPONSE_ONSET_SLOPE_C_PER_S (a margin comfortably above the noise
 *       floor -- see that constant's own comment for why 2 coincidental
 *       quantization ticks cannot trigger it), at which point s_at.step_
 *       onset_seen latches and s_at.step_onset_trace_count records when.
 *       The peak search window (PEAK_SLOPE_SEARCH_SAMPLES, unchanged at 30)
 *       then runs from THAT sample, not sample 0 -- so a 1000s dead time is
 *       handled exactly the same as a 20s one; only the ELAPSED budget
 *       (AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S) bounds how long onset
 *       itself can be waited for, which is the correct backstop (a plant
 *       that truly never responds should end up marked unsettled and
 *       refused, not spin forever).
 *
 *   (5) SETTLE_ABS_SLOPE_FLOOR_C_PER_S (the absolute noise floor, meant to
 *       stop a plant that starts almost perfectly flat from satisfying
 *       criterion 1 on noise alone) was smaller than the quantization noise
 *       of the very quantity it floors: the trace is packed to 0.1 degC
 *       (record_trace_sample()) and "recent slope" spans (SETTLE_CHECK_
 *       SAMPLES-1)*AUTOTUNE_ENGINE_SAMPLE_PERIOD_S = 50s, so ONE quantum of
 *       trace noise over that window is 0.1/50 = 0.002 degC/s -- 2.5x
 *       LARGER than the old floor of 0.0008. The floor was therefore
 *       unreachable except at exactly recent_slope==0 (already handled by
 *       the peak-clearing gate below), making it decorative. Re-derived
 *       below from that same quantization arithmetic with headroom.
 *
 * A trace whose slope never decays relative to its own peak -- a synthetic
 * constant-rate ramp, or a genuinely still-climbing kiln -- never satisfies
 * criterion 1, so this detector never fires early on one; it falls through
 * to the AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S backstop instead, and
 * autotune_finalize_fit() marks that fit NOT settled (fopdt_model_t::settled) rather
 * than accepting it as steady state -- see autotune_finalize_fit()'s own comment. */
#define SETTLE_CHECK_SAMPLES 6u
#define SETTLE_RELATIVE_SLOPE_FRAC 0.04f       /* recent slope <= 4% of this run's peak slope */
/* 0.1 degC trace quantum / 50s recent-slope window = 0.002 degC/s per
 * quantum step; 1.5x that margin makes a single quantum step over the
 * window (a real, if minimal, floor-clearing event) actually reachable,
 * where the old 0.0008 constant never was -- see item (5) above. */
#define SETTLE_ABS_SLOPE_FLOOR_C_PER_S 0.003f
#define MIN_STEPPING_SAMPLES_BEFORE_SETTLE_CHECK 12u /* don't even look until well past the dead-time region */
/* Peak-slope search window and its onset anchor -- see item (4b) above.
 * PEAK_SLOPE_WINDOW_SAMPLES-sized window (same filtering as "recent slope",
 * not a raw 2-point difference); the search now runs for
 * PEAK_SLOPE_SEARCH_SAMPLES samples starting from DETECTED onset (see
 * RESPONSE_ONSET_SLOPE_C_PER_S), not from the start of STEPPING -- 30
 * samples (300s) is comfortably past the dead-time-adjacent peak of even a
 * slow kiln relative to its own tens-of-minutes tau, while still being
 * short enough that a late noise spike (deep into the exponential's decay)
 * cannot be mistaken for the true peak once onset has already located
 * where "early" actually is for THIS run. */
#define PEAK_SLOPE_WINDOW_SAMPLES 6u
#define PEAK_SLOPE_SEARCH_SAMPLES 30u
/* Response-onset threshold: the windowed slope must clear this before the
 * peak search window even starts. Set to 3x SETTLE_ABS_SLOPE_FLOOR_C_PER_S
 * (0.009 degC/s) specifically so that TWO coincidental quantization ticks
 * landing in one PEAK_SLOPE_WINDOW_SAMPLES window during dead time (0.1+0.1
 * degC / 50s = 0.004 degC/s -- the exact sub-case item (4b) names) cannot
 * false-trigger onset: 0.004 < 0.009 with margin to spare. A genuine FOPDT
 * response's slope right after dead time is expected to be far larger than
 * this on any kiln zone with a working element (see AUTOTUNE_MIN_RISE_*'s
 * own reasoning for the scale of rise this whole detector is built around),
 * so this threshold is conservative in the direction of NOT missing a real
 * onset, while still ruling out a 2-tick noise coincidence specifically. */
#define RESPONSE_ONSET_SLOPE_C_PER_S (3.0f * SETTLE_ABS_SLOPE_FLOOR_C_PER_S)

/* (B) Minimum-excursion requirement -- a fit is refused (same abort-reason
 * channel as every other autotune_finalize_fit() refusal, see that function) if the
 * total rise implied by the fitted gain (k_gain_c_per_duty * step_duty) is
 * below a threshold computed at the point of use -- see
 * autotune_min_rise_c() below for why this is no longer a bare constant.
 *
 * 2026-09-01 review fix: a bare 3.0 degC floor is exactly
 * |k_gain_c_per_duty * duty_step| = |final_c - baseline_c|, i.e. a
 * restatement of pid_autotune_fit_fopdt()'s own 0.5 degC noise floor with a
 * bigger number -- it says nothing about whether 3 degC of rise is enough
 * to trust a GAIN meant to predict behavior over a span of hundreds of
 * degrees. The two real incidents this whole pass exists to fix had rises
 * of 22 and 32 degC and would have sailed through a flat 3.0 threshold
 * unchallenged. Identifiability is a fraction-of-span question, not an
 * absolute-degrees one: AUTOTUNE_MIN_RISE_FRACTION_OF_SPAN of the zone's
 * configured headroom (max_temp_c - baseline_c) when a ceiling is
 * configured (max_temp_c > 0 -- the zero-means-disabled convention, same as
 * every other max_temp_c check in this file), with AUTOTUNE_MIN_RISE_FLOOR_C
 * as an absolute floor under that fraction so a zone with very little
 * headroom left still gets a meaningful minimum. AUTOTUNE_MIN_RISE_
 * NO_CEILING_C is the fallback when no ceiling is configured at all -- a
 * kiln's typical high-fire span is many hundreds of degrees, so a large
 * fallback (40 degC, the midpoint of the reviewer's suggested 30-50 range)
 * is far more defensible than the old 3.0 for a zone that could be
 * anywhere from a hobby kiln (~350C) to an industrial one (~1300C). */
#define AUTOTUNE_MIN_RISE_FRACTION_OF_SPAN 0.15f
#define AUTOTUNE_MIN_RISE_FLOOR_C 3.0f
#define AUTOTUNE_MIN_RISE_NO_CEILING_C 40.0f

/* Step-test guards 1/2 fallback headroom -- see the thermal_guard_input_t
 * comment at its use site below for the full defect this fixes. Any strictly
 * positive value works for guard 1: its "did it rise enough" math
 * (thermal_guard.c) never reads the *size* of setpoint_c - measurement_c,
 * only its sign, so this doesn't need to resemble a real target temperature.
 * Kept comfortably under thermal_guard.c's DRIFT_HYSTERESIS_C (25.0f) so
 * guard 4 reads this fallback the same way it always has when no ceiling is
 * configured -- "at setpoint" (abs_error <= hysteresis), i.e. still dormant,
 * not a new false trip.
 *
 * 2026-09-01 review fix: this was raised 5.0f -> 20.0f on 2026-08-31 with a
 * justification that does not hold up -- checked directly against
 * thermal_guard.c: guard 1's actual trip condition (progress delta < an
 * expected-rise threshold derived from sanity_rate_c_per_min, NOT from
 * setpoint_c's magnitude) never reads error's size, only whether commanded
 * heat produced enough real temperature delta, and this fallback is
 * recomputed from the CURRENT raw reading every tick (see this run's
 * thermal_guard_input_t construction below), which pins error at exactly
 * the headroom constant on every tick regardless of its value -- 5.0 and
 * 20.0 are behaviorally IDENTICAL to guard 1. The only thing the 20.0 value
 * changed was guard 4's margin against DRIFT_HYSTERESIS_C (25.0f,
 * per-zone-overridable), which it narrowed from 20 degC to 5 degC for no
 * corresponding benefit. Reverted to 5.0f. */
#define STEP_TEST_GUARD_HEADROOM_C 5.0f

/* (C) physical-plausibility check's ambient reference -- see autotune_finalize_fit()
 * and the SETTLING->STEPPING transition's own comments. Same value and
 * meaning as profile_executor.c's FALLBACK_AMBIENT_C (20.0f): a reasonable
 * room-temperature default for when no cold-junction reading was available
 * to capture at all. Not #include-shared with profile_executor.c because
 * that constant is `static`/file-local there, same as this one is here. */
#define AUTOTUNE_FALLBACK_AMBIENT_C 20.0f

/* ---------------------------------------------------------------------------
 * Target-temperature step mode (autotune_engine_run_to_target()). Two-phase:
 * a short, low-duty PROBE step to get a rough gain estimate, then a real
 * identification step at the duty that rough gain implies will land the
 * response's asymptote at the operator's requested target. Both phases reuse
 * pid_autotune_fit_fopdt() -- see handle_probe_done_locked() -- rather than a
 * second estimator, and both reuse the existing SETTLING/STEPPING state
 * machine and autotune_finalize_fit()'s guards unmodified.
 *
 * PROBE_DUTY / PROBE_DURATION_S tradeoff, sized against the hardware numbers
 * measured on zone 0 the night this was written (baseline 30.6C, 100% duty:
 * 51.16C at 71s, 61.11C at 242s, 65.70C at 389s and still climbing -- a
 * three-point fit gives K ~= 41 degC/duty, tau ~= 270s): a 600s (10 min,
 * ~2.2*tau on that zone) probe at 15% duty reaches roughly 1 - e^(-580/270)
 * ~= 88% of its own (small) asymptote of K*0.15 ~= 6.2 degC above baseline --
 * enough curvature past dead time for pid_autotune_fit_fopdt()'s two-point
 * method to locate a rise it can extrapolate from (that method only needs to
 * reach 28.3%/63.2% of the trace's OWN raw rise, not of the true asymptote,
 * so it does not require anything close to full settling), while staying a
 * small fraction of ANY reasonable max_temp_c ceiling even on a much
 * stronger zone than this one. Going shorter risks a probe trace too flat to
 * fit (mostly dead time, no real curvature yet); going longer buys little
 * extra fit confidence for tau's of several minutes while eating more of the
 * run's time budget and, on a fast/strong zone, more of its headroom. 15%
 * duty (not lower) keeps the response comfortably above the sensor's
 * quantization/noise floor so the probe fit is not itself noise-dominated. */
#define AUTOTUNE_ENGINE_PROBE_DUTY 0.15f
#define AUTOTUNE_ENGINE_PROBE_DURATION_S 600u

/* PHASE 1 self-termination -- 600s above is a HARD UPPER BOUND, not the
 * normal path. step_settle_check_locked() (the STEPPING settle detector,
 * shared with the real identification step -- see its own header comment)
 * already runs during probing too, but at the probe's low 15% duty its own
 * asymptote is small (the PROBE_DUTY/PROBE_DURATION_S comment above puts it
 * at roughly K*0.15 ~= 6.2 degC on the reference plant) and that detector's
 * relative arm needs the peak slope to clear real margin above quantization
 * noise before it can fire at all (see SETTLE_RELATIVE_SLOPE_FRAC's own
 * "2026-09-02 review fix" paragraph) -- in practice a probe's peak slope
 * rarely gets there before the trace is most of the way to its own
 * asymptote, so that shared detector alone leaves the probe running to
 * (or very near) the 600s budget on every run, which is exactly the "fixed
 * 600s" behavior this generalization pass exists to fix.
 *
 * A probe does not need the same "genuinely flat" bar an identification
 * step does -- it only needs a GAIN ESTIMATE that has stopped moving, since
 * that estimate (not the raw trace) is all handle_probe_done_locked() ever
 * reads. probe_gain_converged_locked() re-fits pid_autotune_fit_fopdt() on
 * the trace-so-far every sample (same fit function, same cadence
 * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S as record_trace_sample(), same MIN_
 * STEPPING_SAMPLES_BEFORE_SETTLE_CHECK floor before it ever looks -- reusing
 * the existing detector's noise-floor discipline rather than inventing a
 * second one) and declares PHASE 1 done once consecutive re-fits agree
 * within AUTOTUNE_PROBE_GAIN_STABLE_FRAC for AUTOTUNE_PROBE_GAIN_STABLE_
 * DWELL samples running -- the same "one sample is noise, several in a row
 * is signal" dwell pattern this file already uses for the death-floor check
 * (AUTOTUNE_ELEMENT_DEATH_FLOOR_CONSECUTIVE_TICKS), not a new noise model.
 * ROBUSTNESS TO NOISE: a two-point FOPDT fit on a still-curving trace keeps
 * revising K sample to sample (the fit is still learning), so noise alone
 * cannot fake DWELL consecutive near-identical fits in a row -- an actually
 * converging trace's successive K estimates settle toward each other well
 * before the raw temperature trace itself goes flat, which is precisely why
 * this criterion can fire earlier than step_settle_check_locked()'s "trace
 * is flat" bar without being any less trustworthy: it is asking a narrower,
 * earlier-answerable question ("has the ESTIMATE stopped moving") instead of
 * "has the PLANT stopped moving". Checked in addition to (OR'd with, never
 * instead of) step_settle_check_locked(), so a fast/strong zone whose probe
 * genuinely goes flat before gain-stability triggers is unaffected. */
#define AUTOTUNE_PROBE_GAIN_STABLE_FRAC 0.03f     /* consecutive re-fits of K must agree within 3% */
#define AUTOTUNE_PROBE_GAIN_STABLE_DWELL 3u       /* that many samples running (30s at the 10s trace period) */

/* Default target = this fraction of the zone's configured max_temp_c, used
 * when the operator supplies no explicit target_c. Deliberately requires an
 * explicit target when max_temp_c == 0 (guard disabled, see
 * autotune_engine_run_to_target()) rather than inventing a fallback ceiling
 * of its own -- there is no "reasonable" absolute default across a kiln
 * range from a ~350C hobby kiln to a ~1300C industrial one. */
#define AUTOTUNE_ENGINE_TARGET_DEFAULT_FRACTION 0.75f

/* Minimum clearance required between a (given or defaulted) target_c and the
 * zone's max_temp_c ceiling, checked BEFORE any heating starts. Exists so a
 * target right at the ceiling doesn't hand the identification step a duty
 * that guard 5 (or guard 4's drift-near-ceiling check) is expected to trip
 * on as a matter of course -- the same "don't build a test to trip its own
 * guard" reasoning as AUTOTUNE_RELAY_SETPOINT_HEADROOM_C above, sized
 * smaller because this mode targets a temperature directly (not an
 * oscillation around one) so it needs less margin than the relay path's 50C. */
#define AUTOTUNE_ENGINE_TARGET_CEILING_MARGIN_C 5.0f

/* ---------------------------------------------------------------------------
 * Guard 1 relaxation for a step test (step_element_proven) -- REVISED after
 * review found the first version reused autotune_finalize_fit()'s FIT-TRUST threshold
 * (autotune_min_rise_c()) for an unrelated question ("is this element
 * alive"). Those thresholds do not agree: on the plant measured the night
 * this was written (K=41.7 degC/duty, tau=285s, baseline 30C, no ceiling ->
 * AUTOTUNE_MIN_RISE_NO_CEILING_C=40.0), guard 1 fires once REMAINING rise
 * drops below rate*tau/60 = 0.5*285/60 = 2.4C, i.e. at cumulative rise ~=
 * 41.7 - 2.4 = 39.3C -- BELOW the 40.0C fit-trust floor, so the old latch
 * never armed before guard 1 tripped: it reproduced the exact bench defect
 * it was written to fix, and only ever helped a run that was never going to
 * trip in the first place. A ceiling-scaled threshold is worse still: at
 * max_temp_c=1300 (a real high-fire kiln), the (B) fraction-of-headroom
 * threshold is 0.15*(1300-30) = 190.5C -- unreachable at any sane duty this
 * side of scorching the ware.
 *
 * "Element alive" is a DIFFERENT, much cheaper question than "is this fit
 * trustworthy", and gets its own threshold: a small ABSOLUTE rise (this is
 * not "enough signal to trust a K/tau/L fit", just "the plant visibly
 * responded to commanded heat, not noise"), combined with the SAME onset
 * detector step_settle_check_locked() already computes
 * (RESPONSE_ONSET_SLOPE_C_PER_S) so a couple of coincidental quantization
 * ticks cannot latch it. On the measured plant, a 3.0C rise at ANY duty > 0
 * arrives within tens of seconds of dead time ending (duty=1.0: solving
 * 41.7*(1-exp(-(t-L)/285))=3.0 gives t-L ~= 20.9s; duty=0.15 -- roughly
 * target mode's typical identify duty -- gives t-L ~= 141s), i.e. it engages
 * within the first STEPPING minutes, nowhere near the ~816s the guard 1 trip
 * actually happened at -- see test_guard1_relaxes_... in
 * test_autotune_engine_prestart.c for this driven through the real engine at
 * a REALISTIC ceiling (1300C) and at max_temp_c==0, not just the one
 * configuration (80C) the first version's test happened to make reachable. */
#define AUTOTUNE_ELEMENT_ALIVE_RISE_C 3.0f

/* Guard 2 (falling while heating) is structurally UNREACHABLE during a step
 * test: this file's own thermal_guard_input_t.setpoint_c construction pins
 * setpoint_c to the zone's ceiling (or raw_c + STEP_TEST_GUARD_HEADROOM_C
 * with none configured) for the whole run, so error = setpoint_c -
 * measurement_c stays large and `climbing` (thermal_guard.c, the 3C arrival
 * band) is true on every tick outside a sliver just under the ceiling --
 * where guard 5 has already fired. Relaxing guard 1's rise check therefore
 * silences the WHOLE progress-window family for the rest of the step, not
 * "guard 1 only" as the first version's comment claimed. Nothing else in
 * the guard suite covers "element dies partway through a proven-alive step":
 * guard 3 needs commanded_duty <= 0 (this is a heating step), guard 4 needs
 * abs_error <= 25C (never true against a ceiling setpoint), guard 7 needs
 * +/-0.05C for 600s (a genuinely falling reading moves faster than that),
 * and guard 8 is not wired for autotune at all (no peer_c/peer_ok passed).
 *
 * So this file does guard 2's job itself, directly on the trace it is
 * already recording: once step_element_proven has latched, s_at.
 * step_rise_running_max_c tracks the highest rise seen since STEPPING
 * started, and a drop of this many degrees below that running peak aborts
 * the run (autotune_escalate_and_abort(WRONG_DIRECTION, ...), the same trip reason
 * and per-zone-block/severity guard 2 itself would have used). Sized well
 * above sensor noise/quantization (0.1C trace quantum, a few tenths of real
 * MAX31856 noise) so a healthy plateau's dither never chatters this, while
 * staying small enough to catch a genuinely dying element well before it has
 * fallen back anywhere near baseline. */
#define AUTOTUNE_ELEMENT_DEATH_DROP_C 5.0f

/* Review round-3 finding 2: the absolute-floor death condition (added to
 * close blocker 3's 3.0-5.0C dead zone) used the SAME 3.0C as the latch,
 * with no hysteresis and no dwell. A zone whose probe asymptote sits just
 * above the floor -- K*0.15 in roughly 21-28 degC/duty is a K range this
 * file's own Blocker 4 rationale already calls "well within a real kiln's
 * plausible range" -- plateaus a few tenths above 3.0C, and ordinary
 * MAX31856 noise plus the 0.1C trace quantum dips a single reading under
 * it. That called autotune_escalate_and_abort(WRONG_DIRECTION, ...), which LATCHES
 * a per-zone block (relay_authority_set_zone_blocked()) -- not just an
 * aborted tune, a block that outlives the run.
 *
 * Fixed with a deadband (trip below 2.5C, i.e. AUTOTUNE_ELEMENT_ALIVE_
 * RISE_C minus this margin -- NOT the same 3.0C the latch itself used, so a
 * plateau sitting anywhere between 2.5 and 3.0C cannot trip merely by
 * dithering across the LATCH threshold) AND a dwell (the floor must read
 * true for this many CONSECUTIVE ticks, 1Hz, before it fires -- a single
 * noisy sample can no longer trip it, only a genuine sustained fall). Both
 * together: a healthy plateau at, say, 3.2C +/-0.15C noise dips to ~3.05C
 * at worst -- comfortably above 2.5C, so neither condition engages; a
 * genuinely dying element's reading keeps falling past 2.5C and stays
 * there, so both conditions engage within a few seconds of the real drop. */
#define AUTOTUNE_ELEMENT_DEATH_FLOOR_MARGIN_C 0.5f
#define AUTOTUNE_ELEMENT_DEATH_FLOOR_CONSECUTIVE_TICKS 5u

/* Guards 1/2's progress window is gated on commanded_duty >=
 * cfg.progress_duty_min, which defaults (thermal_guard.c's
 * PROGRESS_DUTY_MIN) to 0.5 -- a step test's own probe duty
 * (AUTOTUNE_ENGINE_PROBE_DUTY, 0.15) and target mode's typical identify duty
 * (often well under 0.5 too -- a 60C target on a K~41 zone computes duty
 * ~0.36) never reach that bar, so with the default left in place guard 1/2
 * are COMPLETELY INERT for the run's own actual duty, the whole time --
 * discovered in review, not by a bench trip, because nothing in this file's
 * own step_element_proven mechanism is duty-gated (it reads the trace
 * directly), so it silently covered up that the underlying thermal_guard
 * window is not seeing this duty at all.
 *
 * autotune_begin_run_locked() explicitly overrides progress_duty_min to this (any
 * commanded step duty > 0 arms guard 1/2) for every step-method run, probe
 * or identify, so the ordinary "rate_cfg C/min" rise requirement -- not just
 * this file's own alive/death checks -- covers a dead element from the very
 * first STEPPING tick, at whatever duty this run happens to be driving.
 *
 * autotune_engine_run_relay() applies the SAME override (name kept as-is --
 * "step test" here means "this file's own progress-window arming", not
 * literally AUTOTUNE_METHOD_STEP): a relay run's low branch (center - d,
 * 0.15 at the default d=0.35) is likewise below thermal_guard.c's stock 0.5
 * default, and once the guard input feeds want_duty (relay_law_tick()'s
 * pre-PWM branch value, see that construction's own comment) instead of the
 * post-PWM state, this override is what keeps guard 1/2's window armed
 * continuously across BOTH branches of the oscillation rather than degating
 * every time the law dips to its low branch. */
#define AUTOTUNE_PROGRESS_DUTY_MIN_FOR_STEP_TEST 0.01f

/* Absolute margin between target mode's requested target_c and what the
 * identification step's OWN fitted model says it actually landed on
 * (target_achieved_c, computed in autotune_finalize_fit()) before this file logs a
 * WARNING (not a refusal -- the fit itself is still valid and DONE, this is
 * purely an operator-visible flag). Exists because duty is computed from the
 * PROBE's baseline (handle_probe_done_locked()) while the identification
 * step re-baselines after only a 180s re-settle (AUTOTUNE_ENGINE_SETTLE_S)
 * from a zone that is still hot and cooling, not back at the probe's
 * baseline -- a one-directional bias toward a HIGHER re-settle baseline than
 * the probe's, which (duty held fixed) computes toward overshooting
 * target_c. Not corrected in this pass -- RECORDED instead, via this log
 * line and the target_achieved_c status field, so an operator can see the
 * actual landing point rather than trusting the requested one blindly. */
#define AUTOTUNE_TARGET_ACHIEVED_WARN_C 5.0f

/* Hard ceiling on total run duration, measured from autotune_begin_run_locked()'s
 * s_at.run_start_tick (set ONCE per run, never reset at a phase transition)
 * -- STEP method only. Exists because AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S
 * (4h) is a PER-PHASE backstop measured from s_at.phase_start_tick, which
 * DOES reset at every phase transition: target mode's worst case is
 * therefore 180s (settle) + 600s (probe) + 180s (re-settle) + 4h (identify)
 * ~= 4.27h, not 4h, with nothing capping the SUM. Computed from the same
 * per-phase constants so it tracks them automatically rather than needing a
 * second hand-picked number to stay in sync; plain duty-based runs (worst
 * case 180s + 4h ~= 4.05h) never approach it in practice -- it exists purely
 * as target mode's outer safety net, not a tighter budget for the plain
 * path. Relay method keeps its own two independent 4h phase budgets
 * (AUTOTUNE_RELAY_APPROACH_MAX_S / AUTOTUNE_RELAY_CYCLE_MAX_S, worst case
 * ~8h) untouched -- this check is gated to AUTOTUNE_METHOD_STEP only. */
#define AUTOTUNE_ENGINE_WHOLE_RUN_MAX_DURATION_S \
    (2u * AUTOTUNE_ENGINE_SETTLE_S + AUTOTUNE_ENGINE_PROBE_DURATION_S + AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S)

/* ---------------------------------------------------------------------------
 * Guard-threshold generalization off the measured plant (owner's explicit
 * ask: "use this device to help you create generalized algorithms", not
 * hand-tune every constant below to this one bench kiln).
 *
 * Seven of the degC constants above/below (AUTOTUNE_MIN_RISE_FLOOR_C,
 * AUTOTUNE_MIN_RISE_NO_CEILING_C, STEP_TEST_GUARD_HEADROOM_C, AUTOTUNE_
 * ELEMENT_ALIVE_RISE_C, AUTOTUNE_ELEMENT_DEATH_DROP_C, AUTOTUNE_ELEMENT_
 * DEATH_FLOOR_MARGIN_C, AUTOTUNE_TARGET_ACHIEVED_WARN_C) were each picked by
 * eyeballing a rise/drop/margin against ONE measured plant: the AUTOTUNE_
 * ENGINE_PROBE_DUTY/PROBE_DURATION_S comment above records K ~= 41.7
 * degC/duty, tau ~= 270s, measured on zone 0 the night this file's guard
 * suite was built. A zone with a much weaker element (low K -- a big kiln,
 * or a small one on a low-voltage element) never rises enough to satisfy an
 * "alive" threshold sized for a 41.7 K/duty plant; a much stronger one
 * (high K) reaches "death" thresholds sized for that plant on ordinary
 * noise. Every one of these seven is a temperature-RISE bar, so the natural
 * generalization is to scale each linearly with the CURRENT run's own rough
 * gain estimate (probe_k_rough, degC per unit duty, from PHASE 1's fit)
 * relative to the reference plant these constants were tuned against.
 *
 * autotune_scale_threshold_c() is the one place that arithmetic happens.
 * FALLBACK, EXPLICIT: probe_k_rough <= 0.0f (the probe was skipped -- a
 * plain autotune_engine_run(), not target mode -- or a target-mode run that
 * has not finished PHASE 1 yet, or a fresh board where the probe fit itself
 * failed) returns base_c UNCHANGED, exactly the old hand-tuned constant.
 * This is a multiplication, never a division BY probe_k_rough, so there is
 * no div-by-zero path regardless of what the probe measured -- a
 * pathological probe_k_rough (e.g. a fit that came back with a tiny
 * positive gain) only ever shrinks the threshold toward zero, it can never
 * blow it up or crash. */
#define AUTOTUNE_REFERENCE_K_C_PER_DUTY 41.7f


/* ---- threshold scaling (autotune_engine_guard.c) -- shared by the tick
 * loop (autotune_engine.c) and every fit-trust/alive/death threshold in
 * autotune_engine_step_identify.c. */
float autotune_scale_threshold_c(float base_c, float probe_k_rough);

typedef struct {
    kiln_io_t *io;
    MAX31856BusClass *thermo_bus;
    SafetyLinkClass *safety;

    SemaphoreHandle_t lock;
    TaskHandle_t task;

    autotune_engine_state_t state;
    autotune_method_t method;
    uint8_t  zone_index;
    float    step_duty;
    autotune_rule_t step_rule; /* AUTOTUNE_METHOD_STEP only -- SIMC or Cohen-Coon, see
                                 * autotune_engine_run()'s header comment for why only
                                 * those two are valid here. */

    /* Target-temperature step mode (AUTOTUNE_METHOD_STEP only). Reset to
     * false/0 at the top of every autotune_begin_run_locked() call so a plain
     * duty-based run never inherits a previous target-mode run's state. */
    bool     target_mode;   /* true: this run is autotune_engine_run_to_target(), not _run() */
    bool     probe_phase;   /* true while PHASE 1 (probe) is in progress; target_mode only */
    float    target_c;      /* requested (or defaulted) target; target_mode only */
    float    probe_k_rough; /* K estimated from the probe fit; target_mode only, 0 until valid */
    /* Post-hoc landing point -- computed by autotune_finalize_fit() once the
     * identification step's OWN fit is in (baseline_c + k_gain_c_per_duty *
     * step_duty, i.e. what the fitted model says this step actually
     * asymptotes to), NOT the requested target_c. target_mode only, 0 until
     * DONE with a valid model. See AUTOTUNE_TARGET_ACHIEVED_WARN_C's own
     * comment for why this can legitimately differ from target_c. */
    float    target_achieved_c;

    /* Relay-feedback method only (AUTOTUNE_METHOD_RELAY). relay_on is the
     * current branch of the bang-bang law, held across the band so the relay
     * has real hysteresis rather than switching on every noise wiggle; it is
     * also what the cycle counter watches for edges. */
    float    relay_setpoint_c;
    float    relay_d;
    float    relay_h;
    autotune_rule_t relay_rule;
    bool     relay_on;
    uint16_t relay_cycles_seen;
    relay_model_t relay;

    TickType_t phase_start_tick;
    TickType_t last_sample_tick;
    TickType_t prev_tick;
    uint32_t   elapsed_s; /* time in the current phase, updated every tick */
    /* Set ONCE per run by autotune_begin_run_locked(), never touched at a phase
     * transition -- see AUTOTUNE_ENGINE_WHOLE_RUN_MAX_DURATION_S's own
     * comment for why phase_start_tick alone cannot bound a multi-phase
     * (settle/probe/settle/identify) target-mode run. */
    TickType_t run_start_tick;

    heater_output_cfg_t heater_cfg;
    heater_output_state_t heater_state;
    thermal_guard_cfg_t guard_cfg;
    thermal_guard_state_t guard_state;

    bool  per_zone_blocked;
    uint32_t global_fault_source; /* 0 = none asserted by this run -- mirrors
                                    * profile_executor.c's field of the same
                                    * name; see autotune_escalate_and_abort()'s `global`
                                    * branch and the clearing next to
                                    * clear_block_if_any() below. */
    uint32_t cycles_reported; /* high-water mark of heater_state.cycle_count
                                * already handed to relay_cycles_add() --
                                * mirrors profile_executor.c's zone field of
                                * the same name, see the tick's own comment. */
    char  abort_reason[96];

    /* Row-indexed by observed zone, not just the zone under test --
     * TODO.md 6A.5(b): every configured zone is sampled every period while
     * STEPPING, at the same tick, so any zone's trace can be fit against
     * the SAME duty step applied to s_at.zone_index. zone_baseline_c is
     * captured at the same SETTLING->STEPPING transition as the tested
     * zone's own baseline_c used to be. */
    float zone_baseline_c[MAX31856_CHANNEL_COUNT];
    bool  zone_baseline_valid[MAX31856_CHANNEL_COUNT];
    /* Readiness-check state (Phase 7c pre-start thermal readiness): the
     * FIRST valid reading seen for each zone after entering SETTLING,
     * captured once (readiness_start_captured) and never touched again
     * this SETTLING phase. Used only to estimate a slope (degC/s) over the
     * SETTLE_S window at the SETTLING->STEPPING transition below, so a
     * zone that is low but still visibly cooling/heating is told apart
     * from one that is genuinely flat. Reset at every SETTLING entry
     * (autotune_begin_run_locked() and handle_probe_done_locked()'s rewind), same
     * as the other SETTLING->STEPPING-transition state above. */
    float readiness_start_c[MAX31856_CHANNEL_COUNT];
    bool  readiness_start_valid[MAX31856_CHANNEL_COUNT];
    bool  readiness_start_captured;
    float zone_last_valid_c[MAX31856_CHANNEL_COUNT]; /* carry-forward for a zone's momentary bad read, so one dropped
                                                        * sample on a non-tested zone doesn't NaN its whole trace */
    /* Packed, 2 bytes a sample per zone -- see autotune_engine.h. */
    autotune_trace_sample_t zone_trace[MAX31856_CHANNEL_COUNT][AUTOTUNE_ENGINE_MAX_SAMPLES];
    uint16_t trace_count; /* shared sample index -- every zone's row advances together */

    float actual_c;
    bool  actual_valid;
    float duty;

    /* STEPPING-only settle-detector state (A) -- reset at the SETTLING->
     * STEPPING transition, read/written only from autotune_engine_tick_
     * locked() while STEPPING. See SETTLE_RELATIVE_SLOPE_FRAC's comment for
     * what these drive. */
    float    step_peak_slope_c_per_s;    /* max |slope| seen so far this STEPPING phase */
    uint32_t step_peak_slope_at_s;       /* elapsed_s at which that peak was recorded */
    bool     step_settled;               /* true only if the detector genuinely fired -- see autotune_finalize_fit() */
    /* Latched true once this STEPPING phase's cumulative rise from baseline
     * has crossed AUTOTUNE_ELEMENT_ALIVE_RISE_C (a small ABSOLUTE alive
     * threshold -- NOT autotune_finalize_fit()'s much larger fit-trust threshold, see
     * that constant's own comment for why conflating the two was the first
     * version's defect) with a genuine onset already detected. STEP method
     * only; reset false at every SETTLING->STEPPING transition (both a
     * plain run's and target mode's probe->identify one), and also in
     * autotune_begin_run_locked() so it cannot survive into a new run's SETTLING
     * phase either. See thermal_guard_input_t::progress_rise_check_relaxed's
     * comment for what this relaxes and why. */
    bool     step_element_proven;
    /* Highest (actual_c - baseline_c) seen so far this STEPPING phase --
     * tracked regardless of step_element_proven so the running peak is
     * already correct the instant the element IS proven. Used only once
     * proven: AUTOTUNE_ELEMENT_DEATH_DROP_C below this peak aborts the run
     * (this file's own stand-in for guard 2, which cannot reach this case --
     * see that constant's comment). Reset with step_element_proven at every
     * SETTLING->STEPPING transition and in autotune_begin_run_locked(). */
    float    step_rise_running_max_c;
    /* Review blocker 1 fix (lock-order deadlock): whether ANY other zone
     * has an active profile, as of the START of this tick -- computed by
     * task_entry() by calling any_other_zone_profile_active() BEFORE it
     * takes s_at.lock (see that function's own comment, and task_entry()'s,
     * for the documented lock-order invariant this preserves:
     * profile_executor.c's sweep_unowned_relays() takes s_exec.lock then
     * autotune_engine_is_active_on_zone() (s_at.lock) -- the reverse of
     * s_at.lock then s_exec.lock, which is exactly what calling
     * profile_executor_zone_is_active() from INSIDE autotune_engine_tick_
     * locked() -- i.e. while s_at.lock is already held -- would have done).
     * A single plain field, not a function parameter, because the tick
     * function has ~30 call sites in the host test suite and every one but
     * run_ticks()/task_entry() itself is unaffected by this value -- see
     * run_ticks()'s own comment for how tests keep it correct. Reset false
     * in autotune_begin_run_locked() so a new run never starts on a stale hint from
     * a previous one before task_entry() gets a chance to compute a fresh
     * one. */
    bool     other_zone_profile_active_hint;
    /* Consecutive-tick counter for the death check's absolute floor (review
     * round-3 finding 2) -- counts 1Hz ticks with rise_c below AUTOTUNE_
     * ELEMENT_ALIVE_RISE_C - AUTOTUNE_ELEMENT_DEATH_FLOOR_MARGIN_C in a row;
     * reset to 0 the instant rise_c is back at/above that floor. Reset with
     * step_element_proven at every SETTLING->STEPPING transition and in
     * autotune_begin_run_locked(). */
    uint16_t step_below_death_floor_ticks;
    /* Response-onset anchor (item 5, 2026-09-02 review fix) -- see
     * PEAK_SLOPE_SEARCH_SAMPLES's own comment for why the peak-slope search
     * window is anchored to DETECTED onset rather than a fixed sample count
     * from the start of STEPPING. */
    bool     step_onset_seen;            /* true once a real (not noise) response has been detected */
    uint16_t step_onset_trace_count;     /* trace_count at the sample onset was first detected */
    /* PHASE 1 (probe) self-termination -- see probe_gain_converged_locked()'s
     * own comment. probe_last_k_c_per_duty is the previous sample's re-fit
     * of the probe trace-so-far (0 = no prior estimate yet); probe_stable_
     * checks counts CONSECUTIVE samples whose fit moved by no more than
     * AUTOTUNE_PROBE_GAIN_STABLE_FRAC from the one before. Reset at every
     * SETTLING->STEPPING transition (both autotune_begin_run_locked()'s and
     * handle_probe_done_locked()'s rewind), same as step_onset_seen above --
     * a re-settle before the IDENTIFY phase must not inherit the PROBE
     * phase's convergence state. */
    float    probe_last_k_c_per_duty;
    uint16_t probe_stable_checks;
    /* Cold-junction reference captured at the SETTLING->STEPPING transition
     * (or AUTOTUNE_FALLBACK_AMBIENT_C if no cj reading was available) --
     * see autotune_finalize_fit()'s physical-plausibility check for why this, not
     * zone_baseline_c, is the right reference for "can this zone reach
     * max_temp_c at all". */
    float    step_ambient_c;

    fopdt_model_t    model;
    autotune_gains_t proposed_gains;
    float            predicted_max_ramp_c_per_hr;
    float            predicted_max_ramp_ambient_c_per_hr; /* see autotune_engine_status_t's own field of this name */

    autotune_coupling_matrix_t coupling;
} s_at_t;

extern s_at_t s_at;

/* ---- "a test is in progress" -- the one definition of it (autotune_engine_
 * guard.c). There are four running states across two methods, and every
 * place that used to spell out "SETTLING || STEPPING" (relay authority,
 * mutual exclusion with profile_executor, the tick's own early-out, the
 * abort paths) has to agree about all four or the engine can end up driving
 * relays in a state the rest of the firmware believes is idle. */
bool state_is_running(autotune_engine_state_t s);
uint32_t at_ticks_to_s(TickType_t t);
uint32_t at_ticks_to_ms(TickType_t t);

/* ---- cross-zone-active hint + relay/abort plumbing (autotune_engine_
 * guard.c) -- see each definition's own doc comment for the lock-order and
 * single-funnel reasoning these exist to preserve. */
bool any_other_zone_profile_active(uint8_t zone_index);
void autotune_apply_relay(bool want_on);
void force_relays_off(void);
void autotune_escalate_and_abort(thermal_guard_trip_t reason, const char *detail);
void clear_block_if_any(void);
void abort_locked(const char *reason);
autotune_sample_t *autotune_unpack_zone_trace(uint8_t zone, size_t count);

/* ---- coupling-matrix cross-gain persistence (autotune_engine_coupling.c) --
 * autotune_finalize_fit() (autotune_engine_step_identify.c) is the one caller: it
 * gathers every cell to write into one of these and hands it to
 * bx_flash_worker in a single job -- see coupling_persist_job()'s own
 * comment for why a direct zones_config_set_coupling_cell() call from
 * task_entry()'s task is a hard panic, not just a bug. */
typedef struct {
    uint8_t affected_zone[MAX31856_CHANNEL_COUNT];
    float   coeff[MAX31856_CHANNEL_COUNT];
    float   tau_s[MAX31856_CHANNEL_COUNT];
    float   dead_time_s[MAX31856_CHANNEL_COUNT];
    uint8_t count;
    uint8_t stepped_zone;
    uint8_t fail_count;
} coupling_persist_job_t;
void coupling_persist_job(void *arg);

/* ---- STEP method: settle/onset detection, the FOPDT fit and its guards,
 * target-mode probe handling (autotune_engine_step_identify.c) -- called
 * from the tick loop (autotune_engine.c). */
float autotune_step_guard_sanity_rate(float configured_rate_c_per_min, float step_duty);
void autotune_finalize_fit(void);
void handle_probe_done_locked(void);
void record_trace_sample(const float *raw_by_zone, const bool *ok_by_zone);
bool step_settle_check_locked(void);
bool probe_gain_converged_locked(void);
bool check_thermal_readiness_locked(const float *raw_by_zone, const bool *ok_by_zone, char *reason,
                                    size_t reason_cap);

/* ---- RELAY method (autotune_engine_relay.c) -- called from the tick loop
 * (autotune_engine.c). */
float relay_law_tick(bool sensor_ok, float meas_c, bool *edge_on_to_off);
void finalize_relay_fit(void);

/* ---- run setup, shared by every autotune_engine_run*() entry point
 * (autotune_engine.c; autotune_engine_run_relay() lives in
 * autotune_engine_relay.c and calls this too). */
bool autotune_begin_run_locked(uint8_t zone_index, char *err_msg, size_t err_cap);

#endif /* AUTOTUNE_ENGINE_INTERNAL_H */
