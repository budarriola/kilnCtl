// Split out of test_adaptive_tune.c (2026-09-01, kept the file under this repo's
// 1500-line guidance). Ki diagnosis verdict classification: insufficient samples,
// steady-offset small-Ki, floored-vs-small-Ki, monotonic-drift-vs-oscillation,
// limit-cycle Ku/Tu, irregular hunting, clean tracking -- plus the two gating
// rules around when a diagnosis runs at all (never the same run as a model
// refine, but eventually gets a turn) and the status-field clearing when it is
// skipped. Bounding/capping the resulting Ki move is test_adaptive_tune_ki_
// bounds.c, the sibling file.
// #included from test_adaptive_tune.c AFTER its fakes and test helpers -- see
// test_adaptive_tune_dwell.c's header comment for the shared-fixture convention.
// Pure refactor: no test removed, no assertion changed, no comment dropped.

static void test_ki_diagnose_insufficient_below_min_samples(void)
{
    float a[8], d[8];
    for (int i = 0; i < 8; i++) { a[i] = q1(100.0f); d[i] = 0.4f; }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 8, DT_KI, 0.0f, 0.0f, &diag), "call must return true");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_INSUFFICIENT, "8 samples (below the 12-sample minimum) must be INSUFFICIENT");
}

static void test_ki_diagnose_steady_offset_flags_small_ki(void)
{
    // Flat, quantized temperature (no oscillation, no drift), duty varying
    // in the middle of its range (nowhere near a rail) -- and a dwell error
    // figure the profile executor would report for a steadily hot-running
    // zone. This must read as "Ki too small", not floored (see the paired
    // floored test below, which is IDENTICAL except for the duty pattern).
    float a[16], d[16];
    for (int i = 0; i < 16; i++) {
        a[i] = q1(101.3f);
        d[i] = 0.40f + ((i % 2) ? 0.03f : -0.03f); // varying, centered ~0.40, real duty variance
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 16, DT_KI, /*dwell_err_mean_c=*/0.45f, /*dwell_err_max_c=*/0.50f, &diag),
               "call must return true");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL,
               "a steady 0.45C dwell error with a non-flat, non-rail duty must diagnose as Ki too small");
    TEST_CHECK(diag.ki_correction_pct > 0.0f, "the OFFSET verdict must suggest INCREASING Ki (positive correction)");
}

static void test_ki_diagnose_floored_not_misdiagnosed_as_small_ki(void)
{
    // Same flat temperature and the SAME 0.45C/0.50C dwell error figures as
    // the OFFSET test above -- the ONLY difference is duty: pinned low and
    // essentially not moving, the -ff_hold floor's signature (pid.c). This
    // must NOT reuse the OFFSET_TOO_SMALL verdict, and must suggest no
    // correction at all.
    float a[16], d[16];
    for (int i = 0; i < 16; i++) {
        a[i] = q1(101.3f);
        d[i] = 0.02f; // pinned near the 0 rail, essentially zero variance
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 16, DT_KI, 0.45f, 0.50f, &diag), "call must return true");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_FLOORED,
               "identical error figures but duty pinned near a rail must diagnose as FLOORED, not small Ki");
    TEST_CHECK(diag.ki_correction_pct == 0.0f, "a FLOORED verdict must never suggest a Ki correction");
}

// D2 (was test_ki_diagnose_drift_flags_large_ki): a monotonic, one-directional
// drift is NOT oscillation evidence (it crosses its own window mean only
// once) and this function is never handed the setpoint, so it cannot know
// whether the drift is approaching or receding from target. The OLD code
// called fabsf(drift) > threshold "OSCILLATING" / Ki-too-large regardless --
// backwards for the classic "still slowly settling" case, where Ki is
// actually too SMALL. This is that same monotonic-drift fixture (unchanged),
// now asserting the corrected behavior: with no dwell-error evidence handed
// in (0.0f/0.0f, as before), and no multi-crossing oscillation, the correct
// verdict is OK (no unjustified correction) -- not a wrong-signed
// "decrease Ki".
static void test_ki_diagnose_monotonic_drift_is_not_misread_as_oscillation(void)
{
    float a[18], d[18];
    for (int i = 0; i < 18; i++) {
        a[i] = q1(100.0f + 0.06f * (float)i); // +1.02C total drift over the window
        d[i] = 0.40f;
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 18, DT_KI, 0.0f, 0.0f, &diag), "call must return true");
    TEST_CHECK(diag.verdict != ADAPTIVE_TUNE_KI_OSCILLATING && diag.verdict != ADAPTIVE_TUNE_KI_LIMIT_CYCLE,
               "a one-directional (single-crossing) drift must never be read as oscillation -- only "
               "crossing/regularity evidence may produce that verdict");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_OK, "with no dwell-error evidence supplied, a monotonic drift alone "
                                                      "must diagnose OK, not guess a (possibly wrong-signed) correction");
}

// D2, continued: the realistic version of the same scenario -- a zone still
// slowly converging on setpoint has EXACTLY this monotonic-drift shape AND
// a steady non-trivial dwell error (the profile executor's own dwell_err_
// mean/max_c). This is "the classic too-small-Ki signature" the review
// names: it must diagnose OFFSET_TOO_SMALL (Ki should INCREASE), never
// OSCILLATING/decrease -- proving the fix is not just "stop guessing" but
// "let the correctly-signed offset evidence drive the verdict instead".
static void test_ki_diagnose_monotonic_drift_with_steady_offset_flags_small_ki(void)
{
    float a[18], d[18];
    for (int i = 0; i < 18; i++) {
        a[i] = q1(100.0f + 0.06f * (float)i);
        d[i] = 0.40f + ((i % 2) ? 0.03f : -0.03f); // varying, not floored -- see the floored-not-misdiagnosed test
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 18, DT_KI, /*dwell_err_mean_c=*/0.45f, /*dwell_err_max_c=*/0.50f, &diag),
               "call must return true");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL,
               "a monotonic approach-to-setpoint drift with a steady non-floored dwell error must diagnose as "
               "Ki too small, not oscillation");
    TEST_CHECK(diag.ki_correction_pct > 0.0f, "the corrected verdict must suggest INCREASING Ki, not decreasing it");
}

// D2: floored must win even when the SAME window would otherwise satisfy
// the (now-removed) drift heuristic's threshold -- proves floored is
// checked unconditionally, ahead of any other classification, not just
// ahead of the old drift branch specifically.
static void test_ki_diagnose_floored_wins_even_with_monotonic_drift(void)
{
    float a[18], d[18];
    for (int i = 0; i < 18; i++) {
        a[i] = q1(100.0f + 0.06f * (float)i); // same drifting temperature as the tests above
        d[i] = 0.02f;                          // pinned near the 0 rail, essentially zero variance
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 18, DT_KI, 0.45f, 0.50f, &diag), "call must return true");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_FLOORED,
               "a floored duty trace must diagnose FLOORED even though the temperature trace is drifting");
    TEST_CHECK(diag.ki_correction_pct == 0.0f, "a FLOORED verdict must never suggest a Ki correction");
}

// D7: the floored conjunction (dvar < FLOOR_VARIANCE) && (near a rail) has
// two independent clauses. The existing floored/offset fixture pair only
// ever varies BOTH clauses together (flat+railed vs varying+mid-range), so
// deleting either clause from the guard would still pass every existing
// test. These two fixtures hold one clause floored-shaped and flip the
// other, so each clause is independently load-bearing.
static void test_ki_diagnose_flat_duty_mid_range_is_not_floored(void)
{
    // Flat (near-zero variance) duty, same as the floored fixture -- but
    // parked in the MIDDLE of its range, nowhere near either rail. Must NOT
    // read as floored: a mid-range flat duty is not the -ff_hold signature.
    float a[16], d[16];
    for (int i = 0; i < 16; i++) {
        a[i] = q1(101.3f);
        d[i] = 0.50f; // flat, but mid-range -- not near 0.0 or 1.0
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 16, DT_KI, 0.45f, 0.50f, &diag), "call must return true");
    TEST_CHECK(diag.verdict != ADAPTIVE_TUNE_KI_FLOORED,
               "a flat but MID-RANGE duty (not near a rail) must not diagnose as floored");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL,
               "with a steady offset and no rail evidence, the correct verdict is Ki too small");
}

static void test_ki_diagnose_near_rail_but_varying_is_not_floored(void)
{
    // Duty parked near the 0 rail ON AVERAGE, but genuinely moving
    // (variance well above the floor threshold) -- the opposite flip: rail
    // clause true, variance clause false. Must NOT read as floored either.
    float a[16], d[16];
    for (int i = 0; i < 16; i++) {
        a[i] = q1(101.3f);
        d[i] = 0.01f + ((i % 2) ? 0.06f : 0.0f); // mean ~0.04 (near the 0 rail), but swinging, real variance
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 16, DT_KI, 0.45f, 0.50f, &diag), "call must return true");
    TEST_CHECK(diag.verdict != ADAPTIVE_TUNE_KI_FLOORED,
               "duty near a rail ON AVERAGE but genuinely varying must not diagnose as floored");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL,
               "with a steady offset and a varying (not flat) duty, the correct verdict is Ki too small");
}

static void test_ki_diagnose_limit_cycle_yields_ku_tu(void)
{
    // A clean, regular oscillation: period 8 samples (3 full cycles across
    // 24 samples), amplitude 0.3C (well above the 0.05C noise floor),
    // quantized to 0.1C. Duty oscillates with the same period.
    float a[24], d[24];
    const float pi = 3.14159265358979f;
    for (int i = 0; i < 24; i++) {
        a[i] = q1(100.0f + 0.30f * sinf(2.0f * pi * (float)i / 8.0f));
        d[i] = 0.40f + 0.10f * sinf(2.0f * pi * (float)i / 8.0f);
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 24, DT_KI, 0.0f, 0.0f, &diag), "call must return true");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_LIMIT_CYCLE, "a clean, regular 8-sample-period oscillation must diagnose as a limit cycle");
    TEST_CHECK(diag.zero_crossings >= 4, "setup: the fixture must actually cross its own mean at least 4 times");
    TEST_CHECK(diag.ku_estimate > 0.0f, "a limit cycle must hand over a positive Ku estimate");
    TEST_CHECK(diag.tu_estimate_s > 0.0f, "a limit cycle must hand over a positive Tu estimate");
    TEST_CHECK(diag.ki_correction_pct < 0.0f, "a limit cycle must suggest DECREASING Ki");
}

// F5: ADAPTIVE_TUNE_KI_OSCILLATING is reachable but previously had NO test
// asserting it is ever actually produced (only a `!=` check elsewhere).
// Two different periods spliced together (fast then slow) give plenty of
// crossings but an irregular gap spacing -- multi-crossing, but not a clean
// limit cycle.
static void test_ki_diagnose_irregular_hunting_yields_oscillating(void)
{
    float a[24], d[24];
    const float pi = 3.14159265358979f;
    for (int i = 0; i < 24; i++) {
        float t = (float)i;
        float val = (i < 12) ? (100.0f + 0.30f * sinf(2.0f * pi * t / 4.0f))
                              : (100.0f + 0.30f * sinf(2.0f * pi * (t - 12.0f) / 16.0f));
        a[i] = q1(val);
        d[i] = 0.40f;
    }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 24, DT_KI, 0.0f, 0.0f, &diag), "call must return true");
    TEST_CHECK(diag.zero_crossings >= ADAPTIVE_TUNE_KI_MIN_CROSSINGS,
               "setup: the fixture must actually cross its own mean at least 4 times");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_OSCILLATING,
               "an irregular (non-regular-spacing) but multi-crossing oscillation must actually produce the "
               "OSCILLATING verdict -- not just avoid OK/LIMIT_CYCLE, which a `!=` check alone cannot prove");
    TEST_CHECK(diag.ki_correction_pct < 0.0f, "an OSCILLATING verdict must suggest DECREASING Ki, like LIMIT_CYCLE");
}

static void test_ki_diagnose_ok_when_tracking_cleanly(void)
{
    float a[16], d[16];
    for (int i = 0; i < 16; i++) { a[i] = q1(100.0f); d[i] = 0.40f; }
    adaptive_tune_ki_diag_t diag;
    TEST_CHECK(adaptive_tune_diagnose_ki(a, d, 16, DT_KI, /*dwell_err_mean_c=*/0.05f, /*dwell_err_max_c=*/0.08f, &diag),
               "call must return true");
    TEST_CHECK(diag.verdict == ADAPTIVE_TUNE_KI_OK, "flat trace, tiny dwell error, must diagnose OK (no correction)");
    TEST_CHECK(diag.ki_correction_pct == 0.0f, "an OK verdict must suggest no correction");
}

// D5: the model/PID refinement (SIMC recompute) and the Ki diagnosis must
// not stack in the same run -- the Ki diagnosis's trace evidence was
// gathered under the OLD Ki, so applying its +/-20% scale on top of a
// JUST-rewritten SIMC Ki would double up an unrelated correction.
// ---------------------------------------------------------------------
static void test_ki_diagnosis_skipped_same_run_as_model_refine(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f; // prior -- true gain 15, well within jump/spread guards
    s_fake_zone_cfg[1].ki = 1.0f;    // a positive Ki to refine -- needed below to prove the fixture is capable
    const float true_k = 15.0f, ambient = 22.3f;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};
    for (int i = 0; i < 4; i++) {
        // Last dwell runs long (20 ticks, not just SETTLE_TICKS) so its
        // trailing trace clears ADAPTIVE_TUNE_KI_MIN_SAMPLES -- the Ki
        // diagnosis reads only the MOST RECENT dwell's trace (reset every
        // dwell entry), so this is what a real, capable fixture needs.
        int ticks = (i == 3) ? 20 : SETTLE_TICKS;
        feed_settled_dwell(1, ambient + true_k * duties[i], ambient, duties[i], ticks, DT_S);
    }
    profile_firing_run_record_t rec = make_clean_record(11, 1, 900);
    // A steady, non-floored, mid-range-duty dwell error -- exactly the
    // OFFSET_TOO_SMALL shape (duty 0.35 last dwell is nowhere near a rail).
    rec.zones[1].stats.dwell_err_mean_c = 0.45f;
    rec.zones[1].stats.dwell_err_max_c = 0.50f;
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(adaptive_tune_zones[1].has_applied, "setup: the diagonal model refinement must have applied this run");
    TEST_CHECK(!adaptive_tune_zones[1].ki_applied, "the Ki diagnosis must NOT also apply in the same run as the model refine");
    TEST_CHECK(strstr(adaptive_tune_zones[1].ki_refusal_reason, "skipped") != NULL,
               "the Ki refusal reason should say it was skipped because the model refine already ran this run");

    // F4: the two checks above pass EVEN WITHOUT the D5 skip gate, because
    // this fixture's dwells (SETTLE_TICKS=7 each) never clear
    // ADAPTIVE_TUNE_KI_MIN_SAMPLES on their own -- the "!ki_applied" check
    // was vacuous, only the "skipped" reason-string check was load-bearing.
    // Prove this fixture is now genuinely capable of an applied Ki
    // correction by calling adaptive_tune_refine_ki_locked() directly (this file
    // #includes adaptive_tune.c, so its static functions are reachable),
    // bypassing the D5 gate entirely, on the SAME trace/stats this run just
    // produced.
    adaptive_tune_refine_ki_locked(1, &rec.zones[1].stats);
    TEST_CHECK(adaptive_tune_zones[1].ki_applied, "setup: this fixture's trace/stats must genuinely trigger an applied Ki "
                                       "correction when nothing skips it -- otherwise the !ki_applied check above "
                                       "would pass regardless of whether the D5 gate does anything at all");
}

// F1: adaptive_tune_refine_zone_locked() used to report "applied" on ANY nonzero
// blend, however small -- an asymptotically-converging sequence of fits
// (each blend closer to the true gain, never exactly equal) kept
// model_refined true forever, permanently starving adaptive_tune_refine_ki_locked()
// of a turn. MUST FAIL on pre-F1-fix code: 8+ consecutive well-formed runs
// against the same true gain never produce a run where the model refine
// reports no material change, so the Ki refusal reason always says
// "skipped".
// ---------------------------------------------------------------------
static void test_ki_diagnosis_eventually_runs_after_repeated_converging_refinements(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].ki = 1.0f;
    const float true_k = 15.0f, ambient = 22.3f;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};

    bool ki_ever_ran = false;
    for (int run = 0; run < 30; run++) {
        for (int i = 0; i < 4; i++) {
            feed_settled_dwell(1, ambient + true_k * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
        }
        profile_firing_run_record_t rec = make_clean_record(70 + run, 1, 900);
        adaptive_tune_run_end(&rec, true);
        if (strstr(adaptive_tune_zones[1].ki_refusal_reason, "skipped") == NULL) {
            ki_ever_ran = true;
        }
    }
    TEST_CHECK(ki_ever_ran, "F1: across 30 runs converging on the same true gain, the model refine must eventually "
                             "report no material change so the Ki diagnosis gets a genuine turn -- it must never "
                             "be permanently starved");
    // H4(b): the blend has a PERMANENT steady-state residual, not a transient
    // one -- see ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_FRAC's corrected comment in
    // adaptive_tune.c. At the material-change floor's freeze point,
    // k/k_true == ALPHA/(ALPHA+MIN_MATERIAL_MOVE_FRAC) == 0.15/0.155, so
    // k_dc parks at 15.0 * (0.15/0.155) = 14.516 and never moves further --
    // measured 14.578 here (close enough given the 0.1 degC quantization
    // this fixture's temperatures go through). 0.5 tolerance left only 0.078
    // of slack around that measured value, a standing flake risk. Widened to
    // 0.6 -- still comfortably tighter than the 1.59 residual produced when
    // MIN_MATERIAL_MOVE_FRAC is mistakenly retuned to 0.02f (freeze point
    // 13.41, |15-13.41| = 1.59 >> 0.6), so this assertion still catches that
    // regression; it must NOT be loosened further without re-checking against
    // that mutation.
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].k_dc, true_k, 0.6,
                     "setup: the repeated refinements must actually have converged k_dc close to the true gain "
                     "(within its expected permanent residual), or 'never applying again' would be trivially true "
                     "for the wrong reason");
}

// F2: adaptive_tune_run_end()'s D5 skip branch must not leave ki_verdict/
// ki_correction_pct holding a PREVIOUS run's real diagnosis -- adaptive_
// tune_get_status() publishes both verbatim, and a skipped run must not be
// misreported as a live verdict for the run that actually skipped it. MUST
// FAIL on pre-F2-fix code: run 2's ki_verdict/ki_correction_pct still read
// run 1's OFFSET_TOO_SMALL/positive values.
// ---------------------------------------------------------------------
static void test_ki_status_fields_cleared_when_diagnosis_skipped(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].ki = 1.0f;

    // Run 1: too few dwell observations for the model refine to fire (only
    // 1, well under ADAPTIVE_TUNE_MIN_OBSERVATIONS), but a single LONG
    // dwell gives the Ki trace plenty of samples, and an explicit steady,
    // non-floored offset drives a genuine OFFSET_TOO_SMALL verdict.
    feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
    TEST_CHECK(adaptive_tune_zones[1].ring_count < ADAPTIVE_TUNE_MIN_OBSERVATIONS,
               "setup: the model refine must not have enough observations to fire");
    profile_firing_run_record_t rec1 = make_clean_record(80, 1, 900);
    rec1.zones[1].stats.dwell_err_mean_c = 0.45f;
    rec1.zones[1].stats.dwell_err_max_c = 0.50f;
    adaptive_tune_run_end(&rec1, true);
    TEST_CHECK(!adaptive_tune_zones[1].has_applied, "setup: the model refine must not have applied this run");
    TEST_CHECK(adaptive_tune_zones[1].ki_verdict == (uint8_t)ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL,
               "setup: the Ki diagnosis must have actually run and produced a real, non-default verdict");
    TEST_CHECK(adaptive_tune_zones[1].ki_correction_pct > 0.0f, "setup: a real, nonzero correction must be published");

    // Run 2: feed enough well-spread, on-model observations that the model
    // refine DOES fire this time -- the Ki diagnosis must be skipped.
    const float true_k = 15.0f, ambient = 22.3f;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(1, ambient + true_k * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
    }
    profile_firing_run_record_t rec2 = make_clean_record(81, 1, 900);
    adaptive_tune_run_end(&rec2, true);
    TEST_CHECK(adaptive_tune_zones[1].has_applied, "setup: the model refine must have applied this run");
    TEST_CHECK(!adaptive_tune_zones[1].ki_applied, "the Ki diagnosis must not apply when skipped");
    TEST_CHECK(adaptive_tune_zones[1].ki_verdict == (uint8_t)ADAPTIVE_TUNE_KI_INSUFFICIENT,
               "F2: a skipped-this-run Ki diagnosis must not keep publishing a PREVIOUS run's verdict");
    TEST_CHECK(adaptive_tune_zones[1].ki_correction_pct == 0.0f,
               "F2: a skipped-this-run Ki diagnosis must not keep publishing a PREVIOUS run's correction pct");
}
