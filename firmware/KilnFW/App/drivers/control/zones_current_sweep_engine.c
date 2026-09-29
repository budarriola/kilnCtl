#include "zones_http_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "MAX31856.h"
#include "kiln_io.h"
#include "profile_executor.h"
#include "safety_trip_words.h"
#include "thermo_channel_read.h"
#include "thermo_combine.h"
#include "thermo_owner.h"

static const char *TAG = "zone_sweep";

/* ---- Task 1: per-zone normal-current measurement sweep -------------------
 *
 * How long to hold one zone's relay(s) on while measuring, and why:
 *
 * SaftyFW/docs/CURRENT_SENSE.md §3: the analog front end's rise to a full
 * reading is < 10 ms once current actually flows through the CT -- that part
 * of the path is effectively instant and does not gate anything here.
 *
 * The real settle time is mechanical + protocol + analog. The relay coil
 * (EE2-12NUH, kiln_io.h) needs to physically close; this ESP only LEARNS the
 * new current_a[] reading once the next SAFETY_LINK status poll lands
 * (safety_link.h's poll_period_ms is 500 ms); and the CT front end is a
 * peak-hold RECTIFIED ENVELOPE with a time constant of about one second
 * (SaftyFW/docs/CURRENT_SENSE.md, and the project's own bench measurements),
 * not a sampled waveform -- so the reading approaches its final value
 * exponentially over several seconds after the relay closes, long after the
 * relay itself has settled.
 *
 * ZONE_SWEEP_SETTLE_MS is therefore 10000 ms: roughly ten envelope time
 * constants, so the envelope is settled to well under a percent of its step
 * before any sample is taken, and many poll periods, so a poll that happened
 * to land just before the relay engaged cannot contribute. A shorter settle
 * biases every measured current LOW by an amount that depends on how long
 * ago the relay closed -- a systematic error that looks like a calibration
 * problem rather than a timing one. This value is an owner decision; it is
 * not derived from the poll period.
 *
 * After settling, ZONE_SWEEP_SAMPLE_MS (4000ms, ~8 more polls at 500ms) of
 * current_a[] readings are averaged -- the same "oversample and average"
 * discipline CURRENT_SENSE.md sec 4 documents for the ADC itself (16x per
 * sample there), applied one level up here to average out poll-to-poll
 * noise on the already-demodulated current reading.
 *
 * Total 14000ms/zone: swept back-to-back across MAX31856_CHANNEL_COUNT
 * zones that is about 42 s for three zones, and every zone but the one being
 * measured has its relay(s) OFF for the whole sweep (structural, not a
 * convention -- see zone_sweep_task() below). */
#define ZONE_SWEEP_SETTLE_MS 10000u
#define ZONE_SWEEP_SAMPLE_MS 4000u
#define ZONE_SWEEP_ENERGIZE_MS (ZONE_SWEEP_SETTLE_MS + ZONE_SWEEP_SAMPLE_MS)
#define ZONE_SWEEP_POLL_MS 500u /* matches safety_link.h's poll_period_ms */

/* ---- M12: deriving ct_channel_map from this sweep ------------------------
 * COMMISSIONING_UX.md sec 1.2 permits ct_channel_map[0..2] to be derived
 * "only when the mapping is unambiguous AND confirmed by the one-zone-at-a-
 * time energize (CURRENT_SENSE.md sec 5 step 2)". This sweep IS that
 * energize -- one zone's relay(s) on, every other relay forced off for the
 * whole 5s window (zone_sweep_hw_energize()'s 0xFF mask) -- so it is the
 * only place on this board that can honestly claim both halves. Before
 * this, the only producer of ct_channel_map was three hand-typed fields on
 * the commissioning page, which means S14 (and the mapping half of S3/S4)
 * armed only if somebody typed the right three numbers.
 *
 * A channel is taken to have responded to the zone under test only if:
 *   - it is carrying real load current (>= ZONE_SWEEP_CT_RESPOND_A), and
 *   - it dominates every other channel by ZONE_SWEEP_CT_DOMINANCE.
 * The threshold is the same order as SaftyFW's i_present_a load-active
 * default (2.0 A) on purpose: this only has to separate a conducting
 * element from measurement noise, and CURRENT_SENSE.md sec 0 already scopes
 * the current chain's accuracy as "within a factor of ~2" -- a tighter
 * number would be promising precision the hardware does not have. The
 * dominance factor is what refuses the genuinely ambiguous cases: ct_mask
 * explicitly permits one CT feeding more than one zone (zones_http.c's 5->6
 * comment), and two channels reading within 4x of each other during one
 * zone's window is exactly that shared-CT case, or a foreign load. Neither
 * is derivable, so nothing is written and the operator is told which zone
 * could not be resolved -- guessing here writes a wrong value into a red
 * field that S3/S4/S14 then trust. */
#define ZONE_SWEEP_CT_RESPOND_A 2.0f
#define ZONE_SWEEP_CT_DOMINANCE 4.0f

/* Pure decision for the rule above. `per_ch_a` is ZONE_CT_CHANNEL_COUNT
 * averaged per-channel currents from one zone's sample window; a NaN entry
 * (no per-channel sampler wired) makes the whole call ambiguous rather than
 * being treated as 0 A. */
bool zone_sweep_derive_ct_channel(const float *per_ch_a, uint8_t *out_ch)
{
    uint8_t best = 0;
    float best_a = -1.0f, second_a = -1.0f;
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        if (!isfinite(per_ch_a[c])) {
            return false;
        }
        if (per_ch_a[c] > best_a) {
            second_a = best_a;
            best_a = per_ch_a[c];
            best = c;
        } else if (per_ch_a[c] > second_a) {
            second_a = per_ch_a[c];
        }
    }
    if (best_a < ZONE_SWEEP_CT_RESPOND_A) {
        return false; /* nothing conducted -- CT not fitted, or the zone drew no current */
    }
    if (second_a > 0.0f && best_a < second_a * ZONE_SWEEP_CT_DOMINANCE) {
        return false; /* two channels saw this zone -- shared CT or foreign load */
    }
    if (out_ch) {
        *out_ch = best;
    }
    return true;
}

/* ---- M12b: calibrating k_ct_v_per_a from the same sweep -------------------
 * CURRENT_SENSE.md sec 5 step 3 used to be the only producer of
 * k_ct_v_per_a: "compare the computed amps against a clamp meter and adjust
 * k_ct_v_per_a to match". That is a number nobody has at commissioning time,
 * on a page that then asked for it in V/A -- so in practice it stayed at
 * config_store.c's memset(0) placeholder, which is NOT harmless: with
 * k_ct_v_per_a <= 0 current_presence_policy.c falls back to a fixed
 * counts-domain margin rather than the i_present_a the operator configured
 * (that header's own comment), and every reported amps/watts figure reads 0.
 *
 * The sweep already has both halves of a calibration the operator does not
 * have to type:
 *   - the EXPECTED whole-kiln current at full output, from the two figures
 *     the guided commissioning flow already collects (COMMISSIONING_UX.md
 *     Q3 mains_voltage_v 0x030E, Q4 max_expected_power_w 0x0319):
 *     I_expected = P / V.
 *   - the MEASURED whole-kiln current, as the sum of each zone's dominant
 *     CT channel over a run that energized every zone exactly once with all
 *     other relays forced off (zone_sweep_hw_energize()'s 0xFF mask). Summing
 *     the one-zone-at-a-time readings is the same total a simultaneous
 *     full-output firing would draw, and it is the only version of that total
 *     this fixture can measure without ever having two zones on at once.
 *
 * Since amps are computed on the Pico as I = V_adc / (gain * sqrt2 * k_ct),
 * the measurement is inversely proportional to k_ct, so the whole calibration
 * is one scale factor:
 *
 *     k_new[c] = k_old[c] * (I_measured_total / I_expected_total)
 *
 * with k_old[c] the value the Pico has actually COMMITTED. That dependence on
 * a prior k_old is not a weakness of this derivation, it is inherent: the
 * link carries amps, never raw counts (safety_link_status_t has current_a[3]
 * and nothing else), so an uncommissioned k_old makes every channel read
 * 0.0 A and there is no measurement to scale. The same is already true of
 * the CT-MAP derivation above -- zone_sweep_derive_ct_channel() needs
 * >= 2.0 A to resolve anything -- so a board that can derive the map can
 * always derive this too, and one that cannot is refused here with a reason
 * rather than being given an invented number. */

/* The correction this is willing to apply. CURRENT_SENSE.md sec 0 scopes the
 * whole current chain's accuracy as "within a factor of ~2", so a correction
 * inside this band is a plausible calibration of a plausible starting value.
 * Outside it, the disagreement is not a scale error at all -- a nameplate in
 * the wrong units, a CT on the wrong conductor, a current-output CT fitted
 * where a voltage-output one belongs (CURRENT_SENSE.md's "wiring error the
 * firmware cannot detect") -- and quietly scaling k_ct to paper over it would
 * make the reported amps look right while the presence threshold this same
 * constant feeds moved to match a lie. Refuse and say so. */
#define ZONE_KCT_RATIO_MIN 0.2f
#define ZONE_KCT_RATIO_MAX 5.0f

/* The absolute band a self-burdened voltage-output CT can plausibly land in:
 * an SCT-013-030 is 0.0333 V/A (CURRENT_SENSE.md sec 5), a 1 V/100 A part is
 * 0.01, and a burdened part with R72 fitted is higher still. Two decades
 * either side of that spread is generous; anything outside is not a CT. */
#define ZONE_KCT_MIN_V_PER_A 0.0005f
#define ZONE_KCT_MAX_V_PER_A 0.5f

/* The measured total below which no calibration is attempted. Same order and
 * same reasoning as ZONE_SWEEP_CT_RESPOND_A: below a conducting element's
 * current the ratio is dominated by measurement noise, and a scale factor
 * computed from noise is worse than no scale factor. */
#define ZONE_KCT_MIN_MEASURED_A ZONE_SWEEP_CT_RESPOND_A

/* Opus review finding 1 (S15 false-WARN): zone_sweep_summed_normal_a() used
 * to accept ANY positive difference as a "measured normal", including a
 * single ADC count of drift -- indistinguishable from real load current.
 * ZONE_SWEEP_CT_RESPOND_A (2.0A) is the wrong floor to reuse here: it exists
 * to gate k_ct CALIBRATION confidence against a normal kiln heater element,
 * and this bench's own ~4W/120V fixture never reaches it -- reusing it would
 * make i_normal_a un-measurable on this bench forever, not just noise-safe.
 *
 * REVISITED 2026-09-18, constant UNCHANGED. The derivation this comment used
 * to document was built on two numbers that both turned out to be measuring
 * the wrong thing:
 *
 *   - The original "one observed real load swing was 60 -> 283 counts"
 *     figure, and every earlier live-fixture reading in this area's history,
 *     was taken with the relays driven via io_set_relay_mask but WITHOUT
 *     ever asking the RP2040 to close K4 via SAFETY_CMD_REQUEST_ENABLE (see
 *     heat_enable.h's own top-of-file note) -- i.e. against a circuit that
 *     was never actually energized. Every "no load response" or "tiny load
 *     swing" number measured that way is void and must not be used to size
 *     anything.
 *   - The "channel 2's idle counts wander 60-79" band this floor was
 *     originally built from is a LONG-WINDOW figure (minutes), but the
 *     quantity this floor actually has to survive is the drift between the
 *     ONE idle baseline sample zone_sweep_task() takes before its whole
 *     multi-zone loop and each zone's own on-sample tens of seconds later
 *     (up to ~30-45 s for the last zone of three, at
 *     ZONE_SWEEP_SETTLE_MS=10000 per zone) -- a different, shorter window
 *     than the one that band was measured over.
 *
 * Corrected bench measurement, 2026-09-18, with heat genuinely enabled
 * (safety_request_enable(true), summed topology, channel 2, 10 s settle
 * discarded before each measurement window -- the settle time matters: the
 * peak-hold front end's own charge-up transient reads std=63.5 counts
 * mid-settle vs std=6.6 once settled, so an unsettled window measures the
 * turn-on ramp, not noise):
 *
 *   all relays off,  20 s: mean  72.30  std 3.510  (range 66-76)
 *   all zones on, settle (10s, discarded): mean 237.94  std 63.495
 *   all zones on, settled, 60 s: mean 261.25  std 6.573  (range 251-275)
 *
 * Delta = 188.95 counts summed across 3 zones, confirmed as real load by a
 * ~5 C thermocouple rise over the same 60 s window (not an ADC artifact).
 * Converting counts to amps uses current_sense.c's own formula
 * (I = delta_counts * Vref / 4096 / (gain * sqrt(2) * k_ct)), at Vref=3.3V,
 * k_ct=1.0 V/A (this floor's own reference, see
 * ZONE_SWEEP_NORMAL_NOISE_FLOOR_REF_K_CT below) and the committed
 * CS_DEFAULT_GAIN of 0.715: ~0.8 mA/count, so the summed delta is
 * ~151 mA, or roughly 50 mA per zone (~63 counts/zone).
 *
 * That per-zone signal is ABOVE the existing 45 mA (~56-count) floor, not
 * below it -- the premise that this bench's fixture could never clear the
 * floor was wrong; it was the K4/energize bug making every earlier reading
 * look tiny, not an undersized fixture. The floor is not currently blocking
 * a sweep on this bench. The margin is thin (~1.1x over the corrected ~50 mA
 * signal) rather than the originally-claimed wide margin, and the natural
 * idle-to-idle drift this floor actually has to survive (per the settled
 * data above, on the order of single-digit-to-tens of counts over the
 * relevant tens-of-seconds gap) has not been characterized per-zone or over
 * the full baseline-to-onset window -- so the number is left UNCHANGED
 * rather than moved on a still-incomplete basis: there is currently no
 * measured evidence that 45 mA is either overshooting the real noise floor
 * by an unsafe amount or actively blocking anything. A future re-derivation
 * should be sized from a per-zone, heat-enabled capture spanning the actual
 * idle-sample-to-onset gap, not from a same-window std alone. */
#define ZONE_SWEEP_NORMAL_NOISE_FLOOR_A 0.045f
/* The k_ct_v_per_a this floor was DERIVED against (see the comment above --
 * "k_ct=1.0 V/A, the CT probe's own owner-stated spec"). 2026-09-10 fix
 * (opus review round 2, finding C): the floor above is a fixed AMPS
 * constant, but the quantity it must reject is noise in raw ADC COUNTS, and
 * counts->amps is inversely proportional to k_ct_v_per_a (see this file's
 * own I = delta_counts / (gain * sqrt(2) * k_ct) formula, quoted above).
 * If a channel's LIVE, committed k_ct_v_per_a differs from this reference
 * value -- e.g. commissioned down to 0.1 V/A -- the SAME 19-count noise
 * band converts to a proportionally larger amps reading (0.1/1.0 = 10x, so
 * ~0.45A instead of ~0.045A) and would sail past this floor unchanged,
 * silently accepting noise as a measurement again. zone_sweep_summed_
 * normal_a() below rescales the floor by (this reference k_ct / the live
 * k_ct) so the check tracks whatever the channel is actually calibrated to
 * today, not just the value it happened to be measured against once.
 * Residual, documented rather than closed: the front-end ADC gain half of
 * the same formula (CS_DEFAULT_GAIN, 0.715) is a Pico-side hardware/
 * firmware constant with no live-read path from the ESP32 side today, so
 * this rescaling corrects for k_ct drift only, not a hypothetical future
 * gain change -- see current_sense commissioning docs for that value's own
 * provenance. */
#define ZONE_SWEEP_NORMAL_NOISE_FLOOR_REF_K_CT 1.0f

/* ---- CT attribution verification (docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md)
 *
 * The verdict is deliberately SEPARATE from zone_sweep_derive_ct_channel()
 * above. That function's job is coarse and unchanged: "did anything conduct,
 * and was one channel dominant", with a fixed ZONE_SWEEP_CT_RESPOND_A floor of
 * 2.0 A. The verification needs a CALIBRATED floor instead, because the
 * threshold a real installation must clear depends on the clamp ratio the
 * operator entered and on the zone's own recorded normal current -- a fixed
 * 2.0 A bakes one clamp's assumption into firmware.
 *
 *   threshold_a(z) = max( noise_floor_a(ch), RESPOND_FRACTION * i_normal_a(z) )
 *
 * where noise_floor_a(ch) is ZONE_SWEEP_NORMAL_NOISE_FLOOR_A rescaled by the
 * live committed k_ct exactly as zone_sweep_summed_normal_a() already does --
 * same derivation, same reference constant, so the two cannot drift apart.
 *
 * Structural rule that makes this safe, and the reason the function is shaped
 * this way rather than as a chain of ifs each able to assign any value:
 * INCONCLUSIVE is the initial value and the only value reachable without a
 * measurement, and PASS is written in EXACTLY ONE place, at the bottom, on a
 * branch that has already established a resolved dominant channel matching the
 * configured one, on a fitted channel, with an entered clamp ratio, above
 * threshold_a. Absence of evidence therefore cannot fall through to PASS; it
 * lands on the value it started at.
 *
 * Everything is a plain scalar the caller gathers. In particular `fitted` is
 * passed in rather than looked up, so this stays pure and host-testable and so
 * the single fitted predicate (config_store_ct_channel_fitted(), SaftyFW) stays
 * the one owner of that question -- this must not become a second notion of
 * fitted. */

/* Fraction of a zone's own recorded normal current the response must clear.
 * NOT an owner-settled number (the owner settled the settle time, the verdict
 * scope, the ratio-entry design and the land order; this constant was not among
 * them). 0.5 is chosen as the loosest value that still means "this zone drew
 * something like the current it is on record as drawing": a zone responding at
 * under half its recorded normal is reporting something the verdict should not
 * call verified. It only ever TIGHTENS the decision above the noise floor --
 * the floor is a hard lower bound that a small RESPOND_FRACTION cannot erode --
 * so a wrong value here cannot manufacture a PASS out of noise. */
#define ZONE_CT_VERIFY_RESPOND_FRACTION 0.5f

/* Dominance-only resolution: NaN-safe, no absolute current floor. The absolute
 * floor is the verdict's own calibrated threshold_a, applied below. Keeping the
 * 2.0 A constant out of here is what lets a real bench reading of 23 mA reach
 * the threshold comparison at all -- if this refused on an absolute floor
 * first, every small-fixture measurement would exit as "ambiguous" and the
 * floor comparison that gates PASS would never be exercised, which would make
 * its mandated negative test vacuous. */
static bool zone_ct_dominant_channel(const float *per_ch_a, uint8_t *out_ch, float *out_best_a)
{
    uint8_t best = 0;
    float best_a = -1.0f, second_a = -1.0f;
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        if (!isfinite(per_ch_a[c])) {
            return false; /* a NaN entry makes the whole call ambiguous, never a zero */
        }
        if (per_ch_a[c] > best_a) {
            second_a = best_a;
            best_a = per_ch_a[c];
            best = c;
        } else if (per_ch_a[c] > second_a) {
            second_a = per_ch_a[c];
        }
    }
    if (best_a <= 0.0f) {
        return false;
    }
    if (second_a > 0.0f && best_a < second_a * ZONE_SWEEP_CT_DOMINANCE) {
        return false; /* two channels saw this zone -- shared CT or foreign load */
    }
    if (out_ch) { *out_ch = best; }
    if (out_best_a) { *out_best_a = best_a; }
    return true;
}

/* The calibrated response threshold. Exposed (not static) so a host test can
 * assert it is genuinely derived from the entered ratio rather than a constant
 * wearing a new name. */
float zone_ct_verify_threshold_a(float live_k_ct_v_per_a, float i_normal_a)
{
    float noise_floor_a = ZONE_SWEEP_NORMAL_NOISE_FLOOR_A;
    if (isfinite(live_k_ct_v_per_a) && live_k_ct_v_per_a > 0.0f) {
        noise_floor_a = ZONE_SWEEP_NORMAL_NOISE_FLOOR_A *
                        (ZONE_SWEEP_NORMAL_NOISE_FLOOR_REF_K_CT / live_k_ct_v_per_a);
    }
    float from_normal_a = -1.0f;
    if (isfinite(i_normal_a) && i_normal_a > 0.0f) {
        from_normal_a = ZONE_CT_VERIFY_RESPOND_FRACTION * i_normal_a;
    }
    return (from_normal_a > noise_floor_a) ? from_normal_a : noise_floor_a;
}

zone_ct_verdict_t zone_sweep_verify_ct_attribution(const zone_ct_verify_in_t *in,
                                                   zone_ct_verify_out_t *out)
{
    /* INCONCLUSIVE is the initial value -- see this block's header comment. */
    zone_ct_verify_out_t r;
    uint8_t resolved = 0xFFu;
    float measured_a = NAN;

    r.verdict = ZONE_CT_VERDICT_INCONCLUSIVE;
    r.reason = ZONE_CT_VERIFY_NO_INPUT;
    r.responded_ch = 0xFFu;
    r.measured_a = NAN;
    r.threshold_a = NAN;

    if (in == NULL || in->configured_ch >= ZONE_CT_CHANNEL_COUNT) {
        goto done;
    }

    /* Not fitted is outside the comparison, not evidence of miswiring: never a
     * FAIL. The fitted answer comes from the caller's single predicate. */
    if (!in->configured_ch_fitted) {
        r.reason = ZONE_CT_VERIFY_NOT_FITTED;
        goto done;
    }
    /* Never substitute a default clamp ratio. An uncalibrated channel already
     * falls back to a counts-domain presence margin and reports 0 A, so a PASS
     * computed here would be arithmetic on a number the firmware itself
     * declines to believe. */
    if (!in->ratio_entered || !isfinite(in->live_k_ct_v_per_a) || in->live_k_ct_v_per_a <= 0.0f) {
        r.reason = ZONE_CT_VERIFY_RATIO_NOT_ENTERED;
        goto done;
    }
    /* Two zones on one channel: per-zone attribution is not derivable by
     * construction, so it is INCONCLUSIVE -- not FAIL, and never PASS. */
    if (in->configured_ch_shared) {
        r.reason = ZONE_CT_VERIFY_SHARED_CHANNEL;
        goto done;
    }
    if (!isfinite(in->i_normal_a) || in->i_normal_a <= 0.0f) {
        r.reason = ZONE_CT_VERIFY_NO_NORMAL_CURRENT;
        goto done;
    }

    if (!zone_ct_dominant_channel(in->per_ch_a, &resolved, &measured_a)) {
        r.reason = ZONE_CT_VERIFY_NO_DOMINANT_CHANNEL;
        goto done;
    }
    r.responded_ch = resolved;
    r.measured_a = measured_a;

    /* Two zones the configuration says are distinct both resolved to one
     * channel: that is miswiring, and it is a FAIL. */
    if (in->conflict) {
        r.verdict = ZONE_CT_VERDICT_FAIL;
        r.reason = ZONE_CT_VERIFY_CONFLICT;
        goto done;
    }
    /* A clamp on the wrong conductor. The triple (zone, configured, responded)
     * is what lets an operator walk to the panel and move it. */
    if (resolved != in->configured_ch) {
        r.verdict = ZONE_CT_VERDICT_FAIL;
        r.reason = ZONE_CT_VERIFY_WRONG_CHANNEL;
        goto done;
    }

    r.threshold_a = zone_ct_verify_threshold_a(in->live_k_ct_v_per_a, in->i_normal_a);
    /* THE floor comparison that gates PASS. Deleting or inverting this line
     * must make the below-floor test report PASS -- that is the plan's mandated
     * negative test, and it is the single line standing between a noise-level
     * reading and a green verdict. Written as !(a >= b) so a NaN measurement
     * refuses rather than passing. */
    if (!(measured_a >= r.threshold_a)) {
        r.reason = ZONE_CT_VERIFY_BELOW_THRESHOLD;
        goto done;
    }

    /* The one and only place PASS is written. */
    r.verdict = ZONE_CT_VERDICT_PASS;
    r.reason = ZONE_CT_VERIFY_OK;

done:
    if (out) { *out = r; }
    return r.verdict;
}

const char *zone_ct_verdict_str(zone_ct_verdict_t v)
{
    switch (v) {
    case ZONE_CT_VERDICT_PASS: return "pass";
    case ZONE_CT_VERDICT_FAIL: return "fail";
    case ZONE_CT_VERDICT_INCONCLUSIVE: return "inconclusive";
    }
    return "inconclusive";
}

const char *zone_ct_verify_reason_str(zone_ct_verify_reason_t r)
{
    switch (r) {
    case ZONE_CT_VERIFY_OK: return "verified";
    case ZONE_CT_VERIFY_NO_INPUT: return "no measurement was taken";
    case ZONE_CT_VERIFY_NOT_FITTED: return "the configured CT channel is not fitted";
    case ZONE_CT_VERIFY_RATIO_NOT_ENTERED: return "clamp ratio not entered for this channel";
    case ZONE_CT_VERIFY_SHARED_CHANNEL:
        return "two zones share this CT channel, so per-zone attribution is not derivable";
    case ZONE_CT_VERIFY_NO_NORMAL_CURRENT: return "no normal current recorded for this zone";
    case ZONE_CT_VERIFY_NO_DOMINANT_CHANNEL: return "no single CT channel responded dominantly";
    case ZONE_CT_VERIFY_BELOW_THRESHOLD: return "the response was below the calibrated threshold";
    case ZONE_CT_VERIFY_WRONG_CHANNEL: return "a different CT channel responded than the one configured";
    case ZONE_CT_VERIFY_CONFLICT: return "two zones resolved to the same CT channel";
    }
    return "no measurement was taken";
}


/* zone_kct_derive_t moved to zones_http_internal.h -- zones_current_sweep_
 * task.c calls zone_sweep_derive_k_ct()/zone_kct_derive_str() too. */

/* Pure decision for the rule above -- every input is a plain number the
 * caller gathers, so the whole calibration is host-testable off-target.
 * *out_k is written ONLY on ZONE_KCT_DERIVE_OK. */
zone_kct_derive_t zone_sweep_derive_k_ct(float measured_total_a, float expected_power_w,
                                                 float mains_voltage_v, float k_old, float *out_k)
{
    if (!isfinite(expected_power_w) || expected_power_w <= 0.0f || !isfinite(mains_voltage_v) ||
        mains_voltage_v <= 0.0f) {
        return ZONE_KCT_DERIVE_NO_NAMEPLATE;
    }
    if (!isfinite(measured_total_a) || measured_total_a < ZONE_KCT_MIN_MEASURED_A) {
        return ZONE_KCT_DERIVE_NO_MEASUREMENT;
    }
    if (!isfinite(k_old) || k_old <= 0.0f) {
        return ZONE_KCT_DERIVE_NO_PRIOR_K;
    }
    float expected_a = expected_power_w / mains_voltage_v;
    if (!isfinite(expected_a) || expected_a <= 0.0f) {
        return ZONE_KCT_DERIVE_NO_NAMEPLATE; /* P/V overflowed or underflowed to nothing usable */
    }
    float ratio = measured_total_a / expected_a;
    if (!isfinite(ratio) || ratio < ZONE_KCT_RATIO_MIN || ratio > ZONE_KCT_RATIO_MAX) {
        return ZONE_KCT_DERIVE_IMPLAUSIBLE;
    }
    float k_new = k_old * ratio;
    if (!isfinite(k_new) || k_new < ZONE_KCT_MIN_V_PER_A || k_new > ZONE_KCT_MAX_V_PER_A) {
        return ZONE_KCT_DERIVE_IMPLAUSIBLE;
    }
    if (out_k) {
        *out_k = k_new;
    }
    return ZONE_KCT_DERIVE_OK;
}

bool zone_sweep_summed_normal_a(float sum_with_zone_on_a, float sum_idle_a, float live_k_ct_v_per_a,
                                float *out_normal_a)
{
    if (!isfinite(sum_with_zone_on_a) || !isfinite(sum_idle_a)) {
        return false;
    }
    float normal_a = sum_with_zone_on_a - sum_idle_a;
    /* 2026-09-10 fix (finding C): rescale the reference floor by how far the
     * live, committed k_ct_v_per_a for this channel has drifted from the
     * value the floor was derived against -- see ZONE_SWEEP_NORMAL_NOISE_
     * FLOOR_REF_K_CT's own comment. An uncommissioned/non-finite/non-
     * positive live value (0.0f on a fresh board, per zone_cfg_committed_f32's
     * own "never committed" contract) falls back to the reference floor
     * unchanged -- there is no live calibration to rescale against yet, and
     * that is also the state this floor was originally sized for. */
    float noise_floor_a = ZONE_SWEEP_NORMAL_NOISE_FLOOR_A;
    if (isfinite(live_k_ct_v_per_a) && live_k_ct_v_per_a > 0.0f) {
        noise_floor_a = ZONE_SWEEP_NORMAL_NOISE_FLOOR_A *
                        (ZONE_SWEEP_NORMAL_NOISE_FLOOR_REF_K_CT / live_k_ct_v_per_a);
    }
    if (normal_a < noise_floor_a) {
        /* Below the noise floor (see ZONE_SWEEP_NORMAL_NOISE_FLOOR_A's own
         * comment): a difference this small is not distinguishable from ADC
         * drift, so it is not a measurement. Includes normal_a < 0.0f, the
         * pre-existing wiring/noise-artifact case below -- same "leave the
         * zone unmeasured, never persist a noise-derived threshold" outcome
         * either way. */
        /* opus review finding (MEDIUM): this used to clamp to 0.0f and
         * still return true, which PERSISTS a normal_a of exactly zero --
         * indistinguishable from "measured and it's genuinely zero" -- and
         * silently makes S14/S15 inert for that zone forever (a zero normal
         * can never register as an overcurrent). A channel that reads lower
         * with the zone on than idle is a wiring/noise artifact, not a
         * measurement: report it as unmeasured (false, *out_normal_a left
         * untouched) so the caller can leave the zone's bit clear in
         * measured_mask and the sweep result can say which zones were not
         * measured, instead of persisting a wrong-shaped zero. */
        return false;
    }
    if (out_normal_a) {
        *out_normal_a = normal_a;
    }
    return true;
}

const char *zone_kct_derive_str(zone_kct_derive_t r)
{
    switch (r) {
    case ZONE_KCT_DERIVE_OK: return "ok";
    case ZONE_KCT_DERIVE_NO_NAMEPLATE:
        return "answer the mains voltage and full-output power questions first";
    case ZONE_KCT_DERIVE_NO_MEASUREMENT: return "the sweep measured no load current";
    case ZONE_KCT_DERIVE_NO_PRIOR_K: return "k_ct_v_per_a has never been set, so amps read zero";
    case ZONE_KCT_DERIVE_IMPLAUSIBLE:
        return "measured and nameplate current disagree too far to be a scale error";
    default: return "unknown";
    }
}

/* ---- Owner feature (2026-09-10): nameplate-implied expected current -----
 * "the user should be able to set the name plate value and then on the
 * first heating of the coils find the normal current. Cause a fault if
 * much higher or lower than expected. Account that all coils are the same
 * wattage and that the name plate is for the sum. Allow the user to enter
 * different wattage for each coil."
 *
 * zone_sweep_expected_coil_current_a() implements "account that all coils
 * are the same wattage and the nameplate is for the sum, AND allow a
 * per-coil override": an override (coil_power_w_override > 0, zone_cfg_t::
 * coil_power_w) always wins; otherwise this zone's expected share is the
 * whole-kiln sum split evenly across every ZONE (not every relay -- see
 * this function's own comparison-unit note below). Both modes are the SAME
 * function, not two separate code paths, so there is only ever one place
 * that can disagree with itself about which convention applies.
 *
 * COMPARISON UNIT (opus review finding 6, 2026-09-10): the measurement this
 * gets compared against (zones_config_get_normal_current(), the sweep's
 * own zone_sweep_summed_normal_a()/per-channel path) is ALWAYS a whole
 * ZONE's current -- one CT reading per zone, summing every relay
 * zone_cfg_t::relay_mask commands together, with no way to see an
 * individual relay's own share. So this function's divisor must be the
 * ZONE count (thermo_count, the number of things a measurement exists
 * for), never the RELAY count -- a zone with relay_mask driving two
 * relays still has exactly one measured current, and "coil" in this
 * feature's naming (matching the owner's own wording, and the schema
 * field zone_cfg_t::coil_power_w) means "this zone's element(s) taken
 * together," not "one physical relay." Naming this parameter zone_count,
 * not relay_count, so the signature cannot silently drift back to the
 * wrong divisor the way it briefly did before this note (the one call
 * site, zones_current_sweep_task.c's zone_sweep_check_nameplate_all(),
 * always passed thermo_count -- the disagreement was between the CORRECT
 * call site and this function's own former name/comment, never a live
 * bug in what was computed).
 *
 * I_expected = P_share / V is the exact formula CLAUDE.md's mains_voltage_v
 * warning describes (once commissioned at 240V on a 120V board, silently
 * doubling every derived current) -- mains_voltage_v is read from the SAME
 * committed safety-processor cache (0x030E) every other consumer in this
 * file uses (zone_sweep_derive_k_ct() above), so there is exactly one
 * number in this firmware that answers "what mains voltage feeds this
 * board's P/V", never a second copy that could drift out of agreement with
 * it. */
float zone_sweep_expected_coil_current_a(float coil_power_w_override, float sum_power_w, uint8_t zone_count,
                                          float mains_voltage_v)
{
    float share_w;
    if (isfinite(coil_power_w_override) && coil_power_w_override > 0.0f) {
        share_w = coil_power_w_override; /* explicit per-zone override wins */
    } else if (zone_count > 0 && isfinite(sum_power_w) && sum_power_w > 0.0f) {
        share_w = sum_power_w / (float)zone_count; /* equal-split default, split across ZONES */
    } else {
        return -1.0f; /* no usable nameplate at all -- negative is never a valid current */
    }
    if (!isfinite(share_w) || share_w <= 0.0f || !isfinite(mains_voltage_v) || mains_voltage_v <= 0.0f) {
        return -1.0f;
    }
    float expected_a = share_w / mains_voltage_v;
    return (isfinite(expected_a) && expected_a > 0.0f) ? expected_a : -1.0f;
}

/* Pure comparison: does this zone's already-measured normal current
 * (zones_config_get_normal_current(), the SAME value zone_sweep_plan_i_
 * normal() pushes to the Pico's S14/S15 baseline) agree with what the
 * nameplate implies it should be?
 *
 * Reuses ZONE_KCT_RATIO_MIN/MAX (0.2x-5.0x) verbatim rather than inventing
 * a second plausibility band: it is the same shape of question this file
 * already answers for k_ct calibration ("does a measured current agree
 * with a nameplate-implied one, within CURRENT_SENSE.md sec 0's own
 * documented ~2x accuracy scope") -- see zone_sweep_derive_k_ct()'s own
 * comment for that provenance. A tighter band here would be inventing
 * precision the same hardware does not have anywhere else in this file.
 *
 * NOT armed as a safety trip anywhere -- this is an ESP-side advisory
 * result only (zone_sweep_ctx_t::nameplate_mismatch_mask/nameplate_reason,
 * surfaced on GET /api/zones' status). docs/audits/s14_s15_followup_
 * summed_kct_and_averaging_2026-09-10.md found this bench's own current
 * chain sits against an UNCHARACTERIZED SYSTEMATIC offset
 * (zero_counts[2]=63, no live-read path to characterize it) at the ~4W
 * fixture load this bench actually draws -- averaging cannot correct a
 * fixed offset at any sample count, so a "fault" armed against that offset
 * would not be testing the nameplate agreement the owner asked for, it
 * would be testing the offset. This function still runs and still reports
 * MISMATCH/OK honestly; nothing downstream escalates that into a trip. A
 * real kiln drawing amps (not milliamps) makes that same offset negligible
 * by comparison, which is exactly where this check is meant to matter. */
zone_nameplate_check_t zone_sweep_check_expected_current(float measured_a, float expected_a)
{
    if (!isfinite(expected_a) || expected_a <= 0.0f) {
        return ZONE_NAMEPLATE_CHECK_NO_NAMEPLATE;
    }
    if (!isfinite(measured_a) || measured_a <= 0.0f) {
        return ZONE_NAMEPLATE_CHECK_NO_MEASUREMENT;
    }
    float ratio = measured_a / expected_a;
    if (!isfinite(ratio) || ratio < ZONE_KCT_RATIO_MIN || ratio > ZONE_KCT_RATIO_MAX) {
        return ZONE_NAMEPLATE_CHECK_MISMATCH;
    }
    return ZONE_NAMEPLATE_CHECK_OK;
}

const char *zone_nameplate_check_str(zone_nameplate_check_t r)
{
    switch (r) {
    case ZONE_NAMEPLATE_CHECK_OK: return "ok";
    case ZONE_NAMEPLATE_CHECK_NO_NAMEPLATE: return "no usable nameplate/coil-share/mains-voltage yet";
    case ZONE_NAMEPLATE_CHECK_NO_MEASUREMENT: return "no measured normal current for this zone yet";
    case ZONE_NAMEPLATE_CHECK_MISMATCH:
        return "measured normal current disagrees with the nameplate-implied expectation";
    default: return "unknown";
    }
}

const char *zone_sweep_refusal_str(zone_sweep_refusal_t r)
{
    switch (r) {
    case ZONE_SWEEP_REFUSE_OK: return "ok";
    case ZONE_SWEEP_REFUSE_ALREADY_RUNNING: return "a current sweep is already running";
    case ZONE_SWEEP_REFUSE_NO_HW: return "board I/O is not available this boot";
    case ZONE_SWEEP_REFUSE_CONFIG_INVALID: return "zone config did not load cleanly -- cannot energize relays";
    case ZONE_SWEEP_REFUSE_NO_ZONES: return "no zones are configured";
    case ZONE_SWEEP_REFUSE_PROFILE_RUNNING: return "a firing profile is running or paused";
    case ZONE_SWEEP_REFUSE_AUTOTUNE_RUNNING: return "autotune is running";
    case ZONE_SWEEP_REFUSE_LINK_DOWN: return "the safety link is down";
    case ZONE_SWEEP_REFUSE_TRIP_LATCHED: return "a safety trip is latched";
    case ZONE_SWEEP_REFUSE_RELAYS_ON: return "a relay is already on -- turn it off before sweeping";
    case ZONE_SWEEP_REFUSE_CT_TOPOLOGY_UNKNOWN:
        return "CT topology has not been fetched from the safety processor yet -- retry once the link has synced";
    case ZONE_SWEEP_REFUSE_RESTORE_IN_FLIGHT:
        return "a backup restore is in progress; wait for it to finish before starting";
    default: return "unknown refusal";
    }
}

/* Pure, host-tested refusal decision -- every input is a plain value the
 * caller (zones_current_sweep_start() below) gathers from the real
 * subsystems; this function itself touches no hardware and can be exercised
 * completely off-target. First matching reason wins; order matches the
 * doc comment on zones_current_sweep_start() in zones_http.h. */
zone_sweep_refusal_t zone_sweep_check_refusal(bool already_running, bool have_hw, bool config_valid,
                                                      uint8_t thermo_count, bool profile_running_or_paused,
                                                      bool autotune_active, bool link_up, bool trip_latched,
                                                      bool relays_on, bool ct_topology_unknown)
{
    if (already_running) {
        return ZONE_SWEEP_REFUSE_ALREADY_RUNNING;
    }
    if (!have_hw) {
        return ZONE_SWEEP_REFUSE_NO_HW;
    }
    if (!config_valid) {
        return ZONE_SWEEP_REFUSE_CONFIG_INVALID;
    }
    if (thermo_count == 0) {
        return ZONE_SWEEP_REFUSE_NO_ZONES;
    }
    if (profile_running_or_paused) {
        return ZONE_SWEEP_REFUSE_PROFILE_RUNNING;
    }
    if (autotune_active) {
        return ZONE_SWEEP_REFUSE_AUTOTUNE_RUNNING;
    }
    if (!link_up) {
        return ZONE_SWEEP_REFUSE_LINK_DOWN;
    }
    if (trip_latched) {
        return ZONE_SWEEP_REFUSE_TRIP_LATCHED;
    }
    if (relays_on) {
        return ZONE_SWEEP_REFUSE_RELAYS_ON;
    }
    if (ct_topology_unknown) {
        return ZONE_SWEEP_REFUSE_CT_TOPOLOGY_UNKNOWN;
    }
    return ZONE_SWEEP_REFUSE_OK;
}

/* H2 (opus reviews, 2026-08-27 and 2026-08-28): max_temp_c <= 0 is
 * zones_config_get_temp_limits()'s documented "no ceiling configured" state
 * -- and the DEFAULT on any zone that has never been commissioned. This
 * sweep is specifically a COMMISSIONING tool: an uncommissioned board is
 * exactly the board it will first be run on, so "no ceiling configured"
 * cannot mean "no thermal abort at all" here the way it legitimately can for
 * a firing profile the operator already reviewed. Refusing to sweep an
 * unceilinged zone would make the tool useless on the one board that most
 * needs it (nothing to measure a normal current against yet), so instead:
 * derive an effective ceiling.
 *
 * The 2026-08-27 pass reused OTA_INTERLOCK_TEMP_CEILING_C (100 C) for this.
 * The 2026-08-28 review rejected that: OTA_INTERLOCK_TEMP_CEILING_C was
 * chosen as a conservative OTA precondition for a kiln AT REST, not as a
 * thermal abort for a deliberate energize, and it drifts independently of
 * this file's own needs -- worse, THIS bench is capped at 80 C by every
 * zone's own configured ceiling, so a 100 C fallback can structurally never
 * fire on the hardware this sweep actually runs against. Fixed:
 * zone_sweep_effective_ceiling_c() below derives the ceiling instead of
 * borrowing OTA's -- the tightest configured ceiling across every zone that
 * HAS one, self-calibrating to whatever board this is (80 C on this bench,
 * the operator's tightest real ceiling on a real kiln). Only when NO zone
 * anywhere has a ceiling configured does it fall back to the fixed
 * ZONE_SWEEP_UNCOMMISSIONED_CEILING_C (60 C): a 5s energize on a stone-cold,
 * never-commissioned chamber raises it a few degrees at most, so a low
 * absolute costs nothing in false aborts.
 *
 * !actual_valid (no usable thermocouple reading) is NOT a ceiling hit
 * either: it is a different failure (see N1's consecutive-invalid-poll
 * counter in zone_sweep_run_one_zone() below, which is what actually stops
 * an unsupervised run once the thermo bus stops answering -- link_up()
 * watches the ESP<->Pico UART, not temperature, and cannot substitute for
 * this). */
#define ZONE_SWEEP_UNCOMMISSIONED_CEILING_C 60.0f

static float zone_sweep_effective_ceiling_c(void)
{
    bool have_any = false;
    float min_ceiling_c = 0.0f;
    uint8_t n = s_zones.cfg.thermo_count;
    if (n > MAX31856_CHANNEL_COUNT) {
        n = MAX31856_CHANNEL_COUNT;
    }
    for (uint8_t zi = 0; zi < n; zi++) {
        float max_temp_c = 0.0f, min_temp_c = 0.0f;
        if (!zones_config_get_temp_limits(zi, &max_temp_c, &min_temp_c)) {
            continue;
        }
        if (max_temp_c > 0.0f && (!have_any || max_temp_c < min_ceiling_c)) {
            min_ceiling_c = max_temp_c;
            have_any = true;
        }
    }
    return have_any ? min_ceiling_c : ZONE_SWEEP_UNCOMMISSIONED_CEILING_C;
}

static bool zone_sweep_ceiling_hit(float actual_c, bool actual_valid, float effective_ceiling_c)
{
    if (!actual_valid || isnan(actual_c)) {
        return false;
    }
    return actual_c >= effective_ceiling_c;
}

static bool zone_sweep_should_sample(uint32_t elapsed_ms)
{
    return elapsed_ms >= ZONE_SWEEP_SETTLE_MS;
}

static bool zone_sweep_zone_done(uint32_t elapsed_ms)
{
    return elapsed_ms >= ZONE_SWEEP_ENERGIZE_MS;
}

/* zone_sweep_ctx_t moved to zones_http_internal.h (its own doc comment there
 * is unchanged) -- zones_current_sweep_task.c reads/writes s_sweep too. The
 * instance itself is still defined here, non-static: this is where the
 * sweep's live state actually lives. */
zone_sweep_ctx_t s_sweep = {
    .active = false, .abort_requested = false, .state = ZONE_SWEEP_IDLE,
    .zone_index = 0, .zones_done = 0, .zones_total = 0, .reason = "", .task = NULL,
};

/* THE single choke point every exit path of the sweep goes through to make
 * sure relays end up off -- abort, ceiling hit, link loss, or ordinary
 * completion all call this and NOTHING ELSE turns a relay off in this
 * module's sweep code.
 *
 * B1 fix (opus review, 2026-08-27): this used to call kiln_io_all_relays_off()
 * directly. kiln_io_owner.h's own top comment names that call out by name as
 * outside its documented licence when used as a NORMAL end-of-measurement
 * path rather than a last-resort fail-safe (link watchdog / guard-9 watchdog
 * / kiln_enter_safe_state()) -- this is the normal path, every single zone.
 * Routed through kiln_io_owner_command_all_relays_off() instead: same
 * "unconditional, no gating" behavior kiln_io_owner.h documents for it
 * (relays only ever come off here, never on), now serialized against the
 * other five relay writers through the owner task's queue instead of racing
 * them on a stale SX1509 read. */
void zone_sweep_force_relays_off(void)
{
    if (s_hw_io) {
        /* This is the single choke point the module header comment above
         * names -- every exit path (abort, ceiling hit, link loss, ordinary
         * completion) funnels through here to make sure relays end up off.
         * The call used to be fire-and-forget: an owner-queue timeout
         * (ESP_ERR_TIMEOUT, the same real, reachable failure danger_mode.c's
         * post_and_wait() call can hit) would leave a relay closed with
         * nothing in the log to say so, while every caller of this function
         * moves on as though the coil had actually opened. Name the failure
         * loudly instead of asserting nothing. */
        esp_err_t err = kiln_io_owner_command_all_relays_off();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "zone_sweep_force_relays_off: kiln_io_owner_command_all_relays_off "
                          "FAILED (%s) -- do not assume a relay is open", esp_err_to_name(err));
        }
    }
}

/* Reads zone zi's combined, calibrated temperature the same way
 * profile_executor.c's control tick does (thermo_channels_read() +
 * thermo_combine() over the zone's thermo_mask). This used to duplicate the
 * per-channel spi_failed/isnan/fault_bits_bad filter inline -- profile_
 * executor.c was off-limits for the earlier pass that wrote this function,
 * and its own read was buried inside a much larger per-tick loop with no
 * standalone entry point. Migrated 2026-09-24 to the shared
 * thermo_channels_read() helper (thermo_channel_read.h) now that it exists,
 * so the filter has one owner instead of two copies that can drift apart.
 * Same effective behaviour on real hardware: thermo_channels_read()'s own
 * bus-usable gate (`sim_backend_enabled() || (bus && bus->initialized)`)
 * reduces to this call site's old `s_hw_thermo_bus && s_hw_thermo_bus->
 * initialized` check whenever sim_backend_enabled() is false, i.e. every
 * non-CONFIG_KILNCTL_SIM_PLANT build (the bench board). The one behavioural
 * difference this migration introduces: a CONFIG_KILNCTL_SIM_PLANT build now
 * also honours the sim backend at this call site, which it never did
 * before (flagged rather than silently changed -- see the commit message).
 * *out_valid false means "no usable reading", matching thermo_combine()'s
 * own convention. */
static void zone_sweep_read_zone_temp(uint8_t zi, float *out_c, bool *out_valid)
{
    *out_c = NAN;
    *out_valid = false;
    ThermoChannelSnapshot snap;
    thermo_channels_read(s_hw_thermo_bus, &snap);
    uint8_t tmask = 0;
    zones_config_get_thermo_mask(zi, &tmask);
    bool valid = false;
    float combined = thermo_combine(snap.raw_c, snap.ok, MAX31856_CHANNEL_COUNT, tmask, &valid);
    *out_valid = valid;
    *out_c = valid ? zones_config_apply_cal(zi, combined) : NAN;
}

/* ---- M3 (opus review, 2026-08-27): the per-zone state machine, extracted
 * from zone_sweep_task() below into a form the host tests can drive without
 * xTaskCreate() ever running. The task body used to be untestable by
 * construction (test_zones_http.c stubs xTaskCreate() so it never invokes
 * its task function, and says so in a comment) -- every safety-relevant
 * property named in the finding (relay on/off sequencing, the single-choke-
 * point property, the abort path, the link-loss exit) lived only in that
 * unreachable function body. Dependency injection through zone_sweep_zone_
 * deps_t is what makes it reachable: the SAME sequencing logic that runs on
 * target against real hardware runs in the host tests against fakes that
 * record call order.
 *
 * Deliberately excluded from the deps: the CT-mask -> live_a summation is
 * plain arithmetic over already-fetched values (covered by other, simpler
 * tests) and not itself part of the safety argument this extraction targets;
 * `sample_current` hands the step function an already-summed reading so the
 * deps surface stays focused on the calls that matter here -- energize,
 * de-energize, abort, link status.
 *
 * zone_sweep_zone_deps_t itself moved to zones_http_internal.h --
 * zones_current_sweep_task.c's zone_sweep_task() builds the real `hw_deps`
 * instance from the zone_sweep_hw_*() bindings below. */

/* N1 (opus review, 2026-08-28): consecutive `!actual_valid` polls tolerated
 * before treating "no usable thermocouple reading" as its own abort. One
 * poll of noise tolerance, not zero -- a single dropped/garbled SPI
 * transaction is not itself evidence the bus is wedged. Two consecutive
 * misses (this constant) is. */
#define ZONE_SWEEP_TEMP_LOST_POLLS 2u

typedef enum {
    ZONE_SWEEP_ZONE_SKIPPED = 0,      /* relay_mask == 0 -- nothing wired, nothing measured */
    ZONE_SWEEP_ZONE_ABORTED,
    ZONE_SWEEP_ZONE_CEILING_HIT,
    ZONE_SWEEP_ZONE_LINK_LOST,
    ZONE_SWEEP_ZONE_TRIP_LATCHED,     /* N10: a safety trip latched mid-zone */
    ZONE_SWEEP_ZONE_TEMP_LOST,        /* N1: the ceiling abort went blind -- ZONE_SWEEP_TEMP_LOST_POLLS
                                        * consecutive polls with no usable thermocouple reading */
    ZONE_SWEEP_ZONE_ENERGIZE_REFUSED, /* B1: the owner refused the ON write (owned/safety/updating/io) */
    ZONE_SWEEP_ZONE_OK,
} zone_sweep_zone_outcome_t;

/* Runs one zone of the sweep to completion using `deps` for every side
 * effect. Mirrors the original inline loop body exactly, plus:
 *   - B1: the energize call itself can now be REFUSED (owner-gated), which
 *     the original direct kiln_io_set_relay_mask() call could never report --
 *     that refusal is itself an exit path and goes through the same choke
 *     point as every other one below.
 *   - N1 (opus review, 2026-08-28): the ceiling check is inert while
 *     actual_valid is false (zone_sweep_ceiling_hit() correctly never
 *     invents a hot reading). If the MAX31856 bus faults mid-run -- SPI
 *     wedge, fault bits set, a pulled thermocouple -- every subsequent poll
 *     comes back invalid and the ceiling abort could not fire for the rest
 *     of the zone's 5s energize, with link_up() providing no substitute
 *     supervision (it watches the ESP<->Pico UART, not temperature). Fixed:
 *     count consecutive invalid polls and exit via ZONE_SWEEP_ZONE_TEMP_LOST
 *     after ZONE_SWEEP_TEMP_LOST_POLLS, through the same choke point.
 *   - N10 (opus review, 2026-08-28): a safety trip latching mid-zone used to
 *     be invisible to this loop -- not a heat hazard on its own (the Pico
 *     opens its own contactor independently), but the sweep would grind on
 *     for the rest of its 5s dwell and every zone after this one would then
 *     be refused at the energize step. Exit promptly instead. */
static zone_sweep_zone_outcome_t zone_sweep_run_one_zone(uint8_t zi, uint8_t relay_mask,
                                                          const zone_sweep_zone_deps_t *deps,
                                                          float *out_avg_current_a,
                                                          float *out_per_ch_avg_a,
                                                          uint32_t *out_energize_refused_sources,
                                                          kiln_io_owner_relay_result_t *out_energize_refused_result)
{
    if (relay_mask == 0) {
        return ZONE_SWEEP_ZONE_SKIPPED; /* nothing wired to this zone -- nothing to measure */
    }
    if (deps->abort_requested(deps->ctx)) {
        return ZONE_SWEEP_ZONE_ABORTED;
    }
    uint32_t safety_sources = 0;
    kiln_io_owner_relay_result_t rr = deps->energize(deps->ctx, relay_mask, &safety_sources);
    if (rr != KILN_IO_OWNER_RELAY_OK) {
        deps->force_off(deps->ctx); /* choke point -- nothing was left on, but be explicit */
        if (out_energize_refused_sources) *out_energize_refused_sources = safety_sources;
        /* 2026-09-15 audit fix (review_crash_report_relay_gate_61765de7_
         * 2026-09-15.md, LOW): out_energize_refused_sources alone cannot
         * distinguish a refusal with no SAFETY_FAULT_SRC_* bit set at all
         * (ERR_UPDATING/ERR_CRASH_UNACK, both of which leave *out_sources at
         * its caller-supplied 0 -- see kiln_io_owner.c's relay_on_blocked()
         * doc comment) from a genuine safety fault whose sources happen to
         * decode to nothing. Passing the raw kiln_io_owner_relay_result_t up
         * too lets the message builder below name the REAL reason instead of
         * printing "sources 0x00". */
        if (out_energize_refused_result) *out_energize_refused_result = rr;
        return ZONE_SWEEP_ZONE_ENERGIZE_REFUSED;
    }

    uint32_t elapsed = 0;
    float sum_a = 0.0f;
    uint32_t samples = 0;
    float ch_sum_a[ZONE_CT_CHANNEL_COUNT] = {0};
    uint32_t invalid_streak = 0;
    float effective_ceiling_c = zone_sweep_effective_ceiling_c();
    zone_sweep_zone_outcome_t outcome = ZONE_SWEEP_ZONE_OK;
    while (!zone_sweep_zone_done(elapsed)) {
        if (deps->abort_requested(deps->ctx)) {
            outcome = ZONE_SWEEP_ZONE_ABORTED;
            break;
        }
        deps->delay_poll(deps->ctx);
        elapsed += ZONE_SWEEP_POLL_MS;

        float actual_c;
        bool actual_valid;
        deps->read_temp(deps->ctx, zi, &actual_c, &actual_valid);

        if (!actual_valid || isnan(actual_c)) {
            invalid_streak++;
            if (invalid_streak >= ZONE_SWEEP_TEMP_LOST_POLLS) {
                outcome = ZONE_SWEEP_ZONE_TEMP_LOST;
                break;
            }
        } else {
            invalid_streak = 0;
        }

        if (zone_sweep_ceiling_hit(actual_c, actual_valid, effective_ceiling_c)) {
            outcome = ZONE_SWEEP_ZONE_CEILING_HIT;
            break;
        }

        if (!deps->link_up(deps->ctx)) {
            outcome = ZONE_SWEEP_ZONE_LINK_LOST;
            break;
        }

        if (deps->trip_latched(deps->ctx)) {
            outcome = ZONE_SWEEP_ZONE_TRIP_LATCHED;
            break;
        }

        if (zone_sweep_should_sample(elapsed)) {
            sum_a += deps->sample_current(deps->ctx, zi);
            if (deps->sample_channels) {
                float ch_a[ZONE_CT_CHANNEL_COUNT];
                deps->sample_channels(deps->ctx, ch_a);
                for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
                    ch_sum_a[c] += ch_a[c];
                }
            }
            samples++;
        }
    }

    deps->force_off(deps->ctx); /* choke point -- every path out of this zone goes through here */

    if (outcome == ZONE_SWEEP_ZONE_OK && samples > 0 && out_avg_current_a) {
        *out_avg_current_a = sum_a / (float)samples;
    }
    /* M12: NaN, not 0, whenever there is nothing real to report -- a zone
     * that aborted, took no samples, or ran without a per-channel sampler
     * must read as "cannot tell" to zone_sweep_derive_ct_channel(), never as
     * three channels that all measured zero. */
    if (out_per_ch_avg_a) {
        bool have = (outcome == ZONE_SWEEP_ZONE_OK) && samples > 0 && deps->sample_channels != NULL;
        for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
            out_per_ch_avg_a[c] = have ? (ch_sum_a[c] / (float)samples) : NAN;
        }
    }
    return outcome;
}

/* ---- Real hardware bindings for zone_sweep_zone_deps_t, used only by
 * zone_sweep_task() below -- host tests supply their own fakes instead and
 * never link these. */
kiln_io_owner_relay_result_t zone_sweep_hw_energize(void *ctx, uint8_t relay_mask,
                                                            uint32_t *out_safety_sources)
{
    (void)ctx;
    /* B1: MANUAL, not AUTHORIZED -- deliberate choice (opus review). The
     * AUTHORIZED entry point exists for profile_executor.c/autotune_engine.c
     * because THEY already apply their own zone-level ownership/safety gate
     * (relay_authority_zone_blocked()) before calling in; this module has no
     * equivalent gate of its own; a sweep is not "the owner" of anything the
     * way a running profile zone is. MANUAL is also the one that puts
     * ota_http_heat_blocked_by_update()'s OTA gate on every write (via
     * kiln_io_owner.c's relay_on_blocked()) -- B2's other half: even if the
     * forward interlock (autotune/profile/OTA refusing to START while a
     * sweep is active) were somehow bypassed, MANUAL still refuses to
     * energize while an update is genuinely in flight, mid-sweep, the same
     * way it already refuses a manual dashboard relay-on. ERR_OWNED is the
     * expected refusal if a profile/autotune run is (impossibly, given B2's
     * forward interlock) racing this sweep for the same relay -- fail
     * closed either way.
     *
     * N9 (opus review, 2026-08-28): mask is 0xFF, not relay_mask -- this
     * asserts an all-others-off precondition on every energize write, not
     * just "make sure this zone's relay(s) are on". A relay latched on from
     * the dashboard (or left on by a profile that just ended) before the
     * sweep started used to keep whatever state it had: if it shared a CT
     * channel with the zone under test, its load landed on that channel too
     * and zone_normals_set() persisted the inflated total as the zone's
     * measured "normal" -- permanently poisoning
     * zones_ct_mapping_mismatch()'s reference. zones_current_sweep_start()
     * also now refuses to start at all while any relay shadow bit is set
     * (ZONE_SWEEP_REFUSE_RELAYS_ON), so this is belt-and-suspenders: the
     * start-time refusal is the primary defense, this write is what keeps
     * every OTHER relay off for the whole 5s a zone is actually measured. */
    return kiln_io_owner_command_set_relay_mask(0xFFu, relay_mask, out_safety_sources);
}

void zone_sweep_hw_force_off(void *ctx)
{
    (void)ctx;
    zone_sweep_force_relays_off();
}

void zone_sweep_hw_read_temp(void *ctx, uint8_t zi, float *out_c, bool *out_valid)
{
    (void)ctx;
    zone_sweep_read_zone_temp(zi, out_c, out_valid);
}

float zone_sweep_hw_sample_current(void *ctx, uint8_t zi)
{
    (void)ctx;
    uint8_t ctmask = 0;
    zones_config_get_ct_mask(zi, &ctmask);
    safety_link_status_t st;
    memset(&st, 0, sizeof(st));
    if (!s_hw_safety || safety_link_get_status(s_hw_safety, &st) != ESP_OK) {
        return 0.0f;
    }
    float live_a = 0.0f;
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        if (ctmask & (1u << c)) {
            live_a += st.current_a[c];
        }
    }
    return live_a;
}

/* M12: the same safety_link_get_status() snapshot zone_sweep_hw_sample_
 * current() reads, handed over WITHOUT the ct_mask summing -- see
 * zone_sweep_zone_deps_t::sample_channels for why the mask must not be
 * applied here. A link read that fails leaves every channel NaN, which
 * zone_sweep_derive_ct_channel() reads as "cannot tell". */
void zone_sweep_hw_sample_channels(void *ctx, float *out_a)
{
    (void)ctx;
    safety_link_status_t st;
    memset(&st, 0, sizeof(st));
    bool ok = s_hw_safety && safety_link_get_status(s_hw_safety, &st) == ESP_OK;
    for (uint8_t c = 0; c < ZONE_CT_CHANNEL_COUNT; c++) {
        out_a[c] = ok ? st.current_a[c] : NAN;
    }
}

bool zone_sweep_hw_link_up(void *ctx)
{
    (void)ctx;
    safety_link_status_t st;
    memset(&st, 0, sizeof(st));
    return s_hw_safety && safety_link_get_status(s_hw_safety, &st) == ESP_OK && st.link_up;
}

/* N10 (opus review, 2026-08-28): same fault_asserted/diag-TRIPPED test
 * zones_current_sweep_start() uses to refuse a START, applied mid-run too --
 * see zone_sweep_run_one_zone()'s comment for why this loop needs it. */
bool zone_sweep_hw_trip_latched(void *ctx)
{
    (void)ctx;
    if (!s_hw_safety) {
        return false;
    }
    safety_link_status_t st;
    memset(&st, 0, sizeof(st));
    if (safety_link_get_status(s_hw_safety, &st) != ESP_OK) {
        return false;
    }
    return st.fault_asserted || (st.diag_ever_received && st.diag_state == SAFETY_LINK_DIAG_STATE_TRIPPED);
}

bool zone_sweep_hw_abort_requested(void *ctx)
{
    (void)ctx;
    return s_sweep.abort_requested;
}

void zone_sweep_hw_delay_poll(void *ctx)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(ZONE_SWEEP_POLL_MS));
}

/* ---- M3 (opus review, 2026-08-28): the whole-sweep loop, extracted from
 * zone_sweep_task() below into a form the host tests can drive without
 * xTaskCreate() ever running -- the same reason zone_sweep_run_one_zone()
 * was extracted from it in the earlier pass. Before this, "no two zones on
 * at once" (the comment on zone_sweep_task() below calls it "structural, not
 * a convention") was argued from reading the code, never actually exercised:
 * xTaskCreate() is stubbed under test, so this loop's body never ran.
 * zone_sweep_run_all_zones() is that same loop body, dependency-injected the
 * same way zone_sweep_run_one_zone() is -- `deps` for the per-zone hardware
 * calls, `hooks` for the per-sweep bookkeeping (which zone owns which relay
 * mask, live status updates, persisting a measured normal) that used to be
 * s_sweep/s_zones/zone_normals_set() called directly inline.
 *
 * zone_sweep_all_hooks_t/zone_sweep_all_result_t moved to
 * zones_http_internal.h -- zones_current_sweep_task.c's zone_sweep_task()
 * builds the real `hw_hooks` instance and reads the result. */

void zone_sweep_run_all_zones(uint8_t zones_total, const zone_sweep_zone_deps_t *deps,
                                      const zone_sweep_all_hooks_t *hooks, zone_sweep_all_result_t *out)
{
    out->state = ZONE_SWEEP_DONE;
    out->reason[0] = '\0';
    out->zones_done = 0;

    for (uint8_t zi = 0; zi < zones_total; zi++) {
        uint8_t relay_mask = hooks->relay_mask_for_zone(hooks->ctx, zi);
        if (relay_mask != 0 && hooks->set_zone_index) {
            hooks->set_zone_index(hooks->ctx, zi);
        }

        float avg_a = NAN; /* stays NaN unless zone_sweep_run_one_zone() got >=1 sample */
        float per_ch_avg_a[ZONE_CT_CHANNEL_COUNT];
        uint32_t refused_sources = 0;
        kiln_io_owner_relay_result_t refused_result = KILN_IO_OWNER_RELAY_OK;
        zone_sweep_zone_outcome_t outcome =
            zone_sweep_run_one_zone(zi, relay_mask, deps, &avg_a, per_ch_avg_a, &refused_sources,
                                     &refused_result);

        switch (outcome) {
        case ZONE_SWEEP_ZONE_SKIPPED:
            continue; /* nothing wired to this zone -- nothing to measure */
        case ZONE_SWEEP_ZONE_ABORTED:
            snprintf(out->reason, sizeof(out->reason), "aborted");
            out->state = ZONE_SWEEP_ABORTED;
            return;
        case ZONE_SWEEP_ZONE_CEILING_HIT:
            snprintf(out->reason, sizeof(out->reason), "zone %u reached its temperature ceiling", zi);
            out->state = ZONE_SWEEP_FAILED;
            return;
        case ZONE_SWEEP_ZONE_LINK_LOST:
            snprintf(out->reason, sizeof(out->reason), "safety link dropped during zone %u", zi);
            out->state = ZONE_SWEEP_FAILED;
            return;
        case ZONE_SWEEP_ZONE_TRIP_LATCHED:
            /* N10: not a heat hazard by itself -- the Pico opens its own
             * contactor independently of anything this ESP does -- but the
             * sweep must not grind on for the rest of the dwell, or refuse
             * every later zone at the energize step without saying why. */
            snprintf(out->reason, sizeof(out->reason), "safety trip latched during zone %u", zi);
            out->state = ZONE_SWEEP_FAILED;
            return;
        case ZONE_SWEEP_ZONE_TEMP_LOST:
            /* N1: the ceiling abort went blind -- see zone_sweep_run_one_zone()'s comment. */
            snprintf(out->reason, sizeof(out->reason), "zone %u lost its temperature reading mid-sweep", zi);
            out->state = ZONE_SWEEP_FAILED;
            return;
        case ZONE_SWEEP_ZONE_ENERGIZE_REFUSED:
            /* B1: the owner refused the ON write -- most likely a firmware
             * update started mid-sweep (ERR_UPDATING) or a safety fault
             * asserted mid-sweep (ERR_SAFETY), the exact gap the direct
             * kiln_io_set_relay_mask() call used to have no way to see. */
            /* ROADMAP.md M13: decode the fault-source mask instead of
             * showing a bare hex value. out->reason is char[64] (matching
             * zones_http.h's zone_sweep_status_t), and the full comma-joined
             * safety_fault_source_words() sentence can run to 141 bytes (see
             * safety_trip_words.h's comment), so the full decode does not
             * fit here -- take just the first asserted source's name and
             * note "(+more)" if others are also set, same shortening
             * ui_page_temperature.c's relay-refusal message uses.
             *
             * Kept short and unconditionally non-truncating: zi is uint8_t
             * (max 3 digits), and the "%.16s" precision (not just a big
             * buffer) is what lets -Werror=format-truncation prove this can
             * never overflow out->reason regardless of how long the decoded
             * word actually is: "zone " + 3 + " energize refused: " + 16 +
             * " (+more)" = 5 + 3 + 19 + 16 + 8 = 51 bytes, plus the NUL,
             * fits in 64. */
            /* 2026-09-15 audit fix (review_crash_report_relay_gate_61765de7_
             * 2026-09-15.md, LOW "sources 0x00"): ERR_UPDATING and
             * ERR_CRASH_UNACK both leave refused_sources at 0 (they are not
             * SAFETY_FAULT_SRC_* bitmask reasons), so decoding
             * refused_sources alone made every such refusal print an empty
             * "sources 0x00" reason. refused_result now carries the real
             * kiln_io_owner_relay_result_t up from the energize() call, so
             * name the non-bitmask reasons directly and keep the existing
             * decode only for genuine ERR_SAFETY. */
            if (refused_result == KILN_IO_OWNER_RELAY_ERR_CRASH_UNACK) {
                snprintf(out->reason, sizeof(out->reason),
                         "zone %u energize refused: unacknowledged crash report", zi);
            } else if (refused_result == KILN_IO_OWNER_RELAY_ERR_UPDATING) {
                snprintf(out->reason, sizeof(out->reason),
                         "zone %u energize refused: firmware update in progress", zi);
            } else if (refused_result == KILN_IO_OWNER_RELAY_ERR_RUNNING) {
                /* docs/SYSTEM_MODE_GATE_PLAN.md review, 2026-09-25: not
                 * expected in practice -- relay_authority_heat_sweep_claim_
                 * begin() already refuses to start a sweep while a profile/
                 * autotune run holds the shared heat claim (B2's forward
                 * interlock) -- but this MANUAL energize call reaches the
                 * same system_mode_gate check every other manual relay-on
                 * does, so name it directly rather than falling through to
                 * the bitmask decode below, which would print an empty
                 * "sources 0x00" for the same reason ERR_UPDATING/
                 * ERR_CRASH_UNACK do. */
                snprintf(out->reason, sizeof(out->reason),
                         "zone %u energize refused: firing/autotune/restore active", zi);
            } else {
                char src_words[160];
                safety_fault_source_words(refused_sources, src_words, sizeof(src_words));
                char *comma = strchr(src_words, ',');
                bool more = (comma != NULL);
                if (comma != NULL) {
                    *comma = '\0';
                }
                snprintf(out->reason, sizeof(out->reason), "zone %u energize refused: %.16s%s", zi, src_words,
                         more ? " (+more)" : "");
            }
            out->state = ZONE_SWEEP_FAILED;
            return;
        case ZONE_SWEEP_ZONE_OK:
        default:
            /* M3: the OK-with-zero-samples path -- outcome can be OK with
             * samples == 0 if the zone's whole dwell elapsed without ever
             * reaching zone_sweep_should_sample()'s window (not reachable
             * with today's fixed SETTLE/ENERGIZE constants, but the
             * aggregation logic itself does not assume that and is tested
             * as such below). avg_a stays NaN in that case and must not be
             * recorded as a measured normal, but the zone still counts as
             * "done" -- it completed without aborting/failing. */
            if (!isnan(avg_a) && hooks->record_normal) {
                hooks->record_normal(hooks->ctx, zi, avg_a);
            }
            if (hooks->record_ct_channels) {
                hooks->record_ct_channels(hooks->ctx, zi, relay_mask, per_ch_avg_a);
            }
            out->zones_done++;
            if (hooks->zone_done) {
                hooks->zone_done(hooks->ctx);
            }
            break;
        }
    }
}

