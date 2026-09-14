// Split out of test_adaptive_tune.c (2026-09-01, kept the file under this repo's
// 1500-line guidance). Ki diagnosis bounding: closed-loop convergence, the
// cumulative bound/floor (both directions) under repeated application, the
// per-run move cap, the reboot-durable baseline (latch, clear-and-relatch,
// tracking the model layer's own fresh SIMC output), and the two flash-worker
// re-entrancy checks on the accept and halt paths. Verdict classification lives
// in test_adaptive_tune_ki_verdict.c, the sibling file.
// #included from test_adaptive_tune.c AFTER its fakes and test helpers -- see
// test_adaptive_tune_dwell.c's header comment for the shared-fixture convention.
// Pure refactor: no test removed, no assertion changed, no comment dropped.

// ---------------------------------------------------------------------
// H3: adaptive_tune_refine_ki_locked() has no cumulative bound of its own, only
// ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE per run (20%). On real hardware the
// loop is CLOSED -- a rising Ki genuinely shrinks dwell_err_mean_c on the
// next firing, so the correction is self-limiting -- and zones_config_set_
// pid() rejects a gain above ZONE_PID_GAIN_MAX as a backstop. Before H3, the
// host fake had neither: it accepted any Ki unconditionally, and no test
// modeled the closed loop, so a probe with STEADY, UNCHANGING evidence
// (idealized-test-input bug class) showed Ki compounding x1.2/run forever:
// 1.0 -> 8.92 in 12 runs with no test to catch it. This test drives
// dwell_err_mean_c FROM the zone's own just-updated Ki every run (err
// shrinks as Ki rises, modeling the real closed loop), and separately proves
// the fake's new reject-above-ZONE_PID_GAIN_MAX backstop (H3(a) fix above)
// actually holds when the loop is NOT closed (constant error, same as the
// idealized probe).
// ---------------------------------------------------------------------

// H3(b): closed-loop model. dwell_err_mean_c = KI_TEST_ERR_K / ki -- a
// simple inverse relationship standing in for "more integral action shrinks
// steady-state error," which is qualitatively what a real PID loop does.
//
// P2 (opus review, K5->K6): KI_TEST_ERR_K used to be 8.0, chosen so this
// fixture's convergence point (~26.7x baseline) sat just under the OLD 50x
// cumulative bound -- which made the bound look validated by this test when
// it was actually the reverse: this constant was a FREE PARAMETER of a
// synthetic fixture, and mutating it (8.0 -> 2.0, convergence ~6.7x) left
// every test green, proving the plant model placed no real constraint on
// what the guard should permit. ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT is now
// justified independently, against plant/model reasoning, in its own
// comment (adaptive_tune_internal.h) -- 5x, the SAME figure this file
// already uses elsewhere (ADAPTIVE_TUNE_MAX_JUMP_RATIO) for "beyond this,
// the MODEL is wrong, not the data." KI_TEST_ERR_K is chosen here only to
// give this fixture a comfortable, non-circular convergence point WELL
// inside that independently-set bound (not up against it) -- 1.0 converges
// at err==0.3 once ki reaches KI_TEST_ERR_K/0.3 ~= 3.33x baseline, roughly 7
// runs of the 20%/run cap (1.2^7 ~= 3.58) -- run well past that (60 runs)
// and require the loop to have actually STOPPED moving, not merely slowed
// down. Changing KI_TEST_ERR_K again changes only where THIS fixture
// converges, never what the cumulative-bound guard itself permits -- that
// is pinned by the guard's own dedicated test below
// (test_ki_diagnosis_runaway_under_constant_error_is_capped_by_cumulative_bound()),
// which does not depend on KI_TEST_ERR_K at all.
#define KI_TEST_ERR_K 1.0f

static void test_ki_diagnosis_converges_under_closed_loop_plant_feedback(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f; // this fixture's own dwells are too few for adaptive_tune_refine_zone_locked() to
                                      // even attempt a fit (ring_count 1 < ADAPTIVE_TUNE_MIN_OBSERVATIONS(4) --
                                      // see the single feed_settled_dwell() call per run below), NOT because the
                                      // blend would be immaterial -- keep the model refine permanently un-due so
                                      // the Ki diagnosis gets a turn on EVERY run (D5 would otherwise starve it
                                      // exactly like F1's own bug, defeating the point of this test)
    s_fake_zone_cfg[1].ki = 1.0f;

    float last_ki = 1.0f;
    bool converged = false;
    int converged_at_run = -1;
    for (int run = 0; run < 60; run++) {
        float ki_before = s_fake_zone_cfg[1].ki;
        float err_mean = KI_TEST_ERR_K / ki_before;
        float err_max = err_mean * 1.1f; // steady -- inside OFFSET_MAX_OVER_MEAN(1.6), same posture as the
                                          // pure-math OFFSET tests above

        // One short dwell (keeps ring_count well under ADAPTIVE_TUNE_MIN_
        // OBSERVATIONS, so adaptive_tune_refine_zone_locked() never has enough data to
        // fire and D5 never starves the Ki diagnosis) plus a long dwell that
        // actually feeds the Ki trace -- same two-call shape as
        // test_ki_diagnosis_skipped_same_run_as_model_refine()'s fixture.
        feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);

        profile_firing_run_record_t rec = make_clean_record(100 + run, 1, 900);
        rec.zones[1].stats.dwell_err_mean_c = err_mean;
        rec.zones[1].stats.dwell_err_max_c = err_max;
        adaptive_tune_run_end(&rec, true);

        TEST_CHECK(!adaptive_tune_zones[1].has_applied, "setup: the model refine must never fire in this fixture -- only "
                                             "the Ki diagnosis is under test here");

        float ki_after = s_fake_zone_cfg[1].ki;
        if (!adaptive_tune_zones[1].ki_applied && ki_after == ki_before && !converged) {
            converged = true;
            converged_at_run = run;
        }
        last_ki = ki_after;
    }

    TEST_CHECK(converged, "H3: under closed-loop plant feedback (error shrinking as Ki rises), the Ki diagnosis "
                           "must eventually stop applying corrections -- it must NOT compound forever the way the "
                           "idealized constant-error probe showed");
    TEST_CHECK(converged_at_run >= 0 && converged_at_run < 20,
               "H3: convergence should happen within the expected ~7-run window for this fixture's error/Ki "
               "relationship, not accidentally at the very end of the 60-run loop");
    TEST_CHECK(last_ki < 10.0f, "H3: a closed loop must converge to a BOUNDED Ki, nowhere near an unclamped "
                                 "runaway");
    // P2/K6: this fixture's ~3.3x-baseline convergence must land WELL inside
    // the independently-justified cumulative bound (ADAPTIVE_TUNE_KI_
    // CUMULATIVE_MAX_MULT == 5x baseline, see that constant's own comment)
    // -- comfortably, not just barely, since KI_TEST_ERR_K was deliberately
    // chosen to converge far under the bound rather than up against it (see
    // this test's own header comment on why that circularity is exactly
    // what this fix removes).
    TEST_CHECK(last_ki < ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT * 1.0f /* ki_baseline captured as 1.0 */,
               "P2/K6: this fixture's legitimate convergence must land comfortably inside the cumulative bound, "
               "proving the bound does not clip a real closed-loop zone");
    TEST_CHECK(strstr(adaptive_tune_zones[1].ki_refusal_reason, "cumulative bound") == NULL,
               "P2/K6: a legitimately converging zone must never be refused by the cumulative bound");

    // Re-run several more times past convergence -- Ki must genuinely have
    // STOPPED, not merely slowed (a test that only checks "less than some
    // number" cannot tell "converged" from "still growing slowly").
    for (int run = 60; run < 65; run++) {
        feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
        profile_firing_run_record_t rec = make_clean_record(100 + run, 1, 900);
        rec.zones[1].stats.dwell_err_mean_c = KI_TEST_ERR_K / s_fake_zone_cfg[1].ki;
        rec.zones[1].stats.dwell_err_max_c = rec.zones[1].stats.dwell_err_mean_c * 1.1f;
        adaptive_tune_run_end(&rec, true);
    }
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].ki, last_ki, 1e-4,
                     "H3: Ki must be genuinely stable past convergence, not merely growing more slowly");
}

// H3(a)/K5: WITHOUT closed-loop feedback (the idealized constant-error shape
// the original review probe used), K5's REAL finding was that the only
// backstop this layer had -- zones_config_set_pid()'s absolute
// ZONE_PID_GAIN_MAX ceiling -- is not an OPERATIONAL bound: instrumented,
// under this exact constant-error probe Ki climbed 1.0 -> 850.6 over 37 runs
// before that ceiling finally refused it, and was left parked at 850.6x its
// starting value. TEST_CHECK(ki <= 1000) is true of that 850x runaway and
// proves nothing -- exactly the "assertion passes whether or not the code
// is correct" failure mode this whole review round is about.
//
// P2/K6 fix: a per-zone CUMULATIVE bound (ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_
// MULT, 5x the autotuned baseline -- re-justified against plant/model
// reasoning, not this test's own convergence fixture, see that constant's
// own comment) that this layer enforces well before the absolute gain
// ceiling would. This test proves THAT bound actually binds -- Ki must
// plateau near 5x baseline (== 5.0, baseline 1.0), not climb toward 850x --
// and that the refusal reason names the cumulative bound specifically (and
// recommends a re-autotune, the reviewer's preferred remediation), not just
// an opaque setter rejection.
static void test_ki_diagnosis_runaway_under_constant_error_is_capped_by_cumulative_bound(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].ki = 1.0f;

    bool saw_a_cumulative_refusal_after_growth = false;
    for (int run = 0; run < 60; run++) {
        feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
        profile_firing_run_record_t rec = make_clean_record(200 + run, 1, 900);
        // Constant, never-shrinking error -- the idealized-input shape the
        // review's probe used, deliberately preserved here as the NEGATIVE
        // case this module must not silently tolerate on host any more.
        rec.zones[1].stats.dwell_err_mean_c = 0.45f;
        rec.zones[1].stats.dwell_err_max_c = 0.50f;
        adaptive_tune_run_end(&rec, true);
        if (s_fake_zone_cfg[1].ki > 3.0f && !adaptive_tune_zones[1].ki_applied &&
            strstr(adaptive_tune_zones[1].ki_refusal_reason, "cumulative bound") != NULL) {
            saw_a_cumulative_refusal_after_growth = true;
        }
    }
    TEST_CHECK(saw_a_cumulative_refusal_after_growth,
               "P2/K6: under constant (non-shrinking) error, Ki must climb until the CUMULATIVE bound actually "
               "refuses it, and say so by name -- not silently keep growing toward the 850x runaway the "
               "pre-cumulative-bound probe measured");
    TEST_CHECK(strstr(adaptive_tune_zones[1].ki_refusal_reason, "re-autotune") != NULL,
               "P2: once the cumulative bound binds, the refusal must recommend re-autotuning this zone -- the "
               "reviewer's preferred remediation for a zone that needs this much integral correction, rather "
               "than continued Ki growth");
    // 5x baseline (1.0) == 5.0 -- allow a small amount of slack above that
    // for the run that first crosses it (the per-run 20% cap can land the
    // ATTEMPTED new_ki slightly past the ceiling on the very run that gets
    // refused; the ceiling refusal means that attempt is never written, so
    // the STORED Ki must never exceed the ceiling itself).
    TEST_CHECK(s_fake_zone_cfg[1].ki <= ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT * 1.0f + 1e-3f,
               "P2/K6: the stored Ki must never be left above the cumulative bound once it starts binding");
    TEST_CHECK(s_fake_zone_cfg[1].ki < 10.0f,
               "P2/K6: Ki must plateau near the 5x cumulative bound, nowhere close to the 850.6x runaway measured "
               "before this bound existed");
    // Sanity: the absolute gain ceiling is still comfortably intact too --
    // the cumulative bound is a NEW, tighter backstop, not a replacement for
    // the existing one.
    TEST_CHECK(s_fake_zone_cfg[1].ki <= TEST_ZONE_PID_GAIN_MAX, "sanity: the absolute gain ceiling still holds");
}

// K4: ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE (20%) is this layer's only
// per-run bound, and before this fix nothing in the file actually pinned it
// -- mutating 0.20f -> 5.0f (500%/run) left 218/218 GREEN, because
// adaptive_tune_diagnose_ki()'s raw ki_correction_pct was a SEPARATELY
// hardcoded +-20.0f literal that happened to equal the cap's value, so the
// clamp in adaptive_tune_refine_ki_locked() could never actually bind on any real
// input (raw was never > cap). Fixed by deriving the raw magnitude from
// this SAME named constant (see its own comment) instead of a duplicate
// literal -- so this test, which asserts the actual per-run RATIO (not
// merely an endpoint some other value could also satisfy), is now
// genuinely sensitive to the constant: mutating it to 5.0f makes both the
// raw correction AND the cap 500%, so the measured ratio becomes 6.0x/run
// instead of 1.2x/run, and the assertion below goes red.
static void test_ki_diagnosis_per_run_move_is_bounded_by_configured_fraction(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f; // ring never reaches ADAPTIVE_TUNE_MIN_OBSERVATIONS in this fixture --
                                      // see test_ki_diagnosis_converges_...()'s identical setup comment
    s_fake_zone_cfg[1].ki = 1.0f;

    float expected_ratio = 1.0f + ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE;
    int applied_runs_checked = 0;
    for (int run = 0; run < 5; run++) {
        float ki_before = s_fake_zone_cfg[1].ki;
        feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
        profile_firing_run_record_t rec = make_clean_record(300 + run, 1, 900);
        // Constant, steady, non-oscillating offset -- always classifies as
        // OFFSET_TOO_SMALL (verdict never changes across these 5 runs), so
        // every run genuinely applies and every ratio measured below is a
        // real Ki(before)->Ki(after) move, not an artifact of the verdict
        // changing partway through.
        rec.zones[1].stats.dwell_err_mean_c = 0.45f;
        rec.zones[1].stats.dwell_err_max_c = 0.50f;
        adaptive_tune_run_end(&rec, true);
        TEST_CHECK(adaptive_tune_zones[1].ki_applied, "setup: every run in this fixture must genuinely apply a Ki correction");
        float ki_after = s_fake_zone_cfg[1].ki;
        TEST_CHECK_NEAR(ki_after / ki_before, expected_ratio, 1e-3,
                         "K4: each run's Ki move must be pinned at exactly 1+ADAPTIVE_TUNE_KI_MAX_FRACTIONAL_MOVE "
                         "-- this is the assertion that must go RED if that constant is mutated (e.g. 0.20f -> "
                         "5.0f), proving the per-run cap is load-bearing rather than decorative");
        applied_runs_checked++;
    }
    TEST_CHECK(applied_runs_checked == 5, "sanity: all 5 runs in this fixture were exercised");
    // 1.2^5 ~= 2.49 -- stays comfortably inside the 5x cumulative bound
    // (P2/K6), so this test is exercising ONLY the per-run cap, not also
    // tripping over the cumulative one.
    TEST_CHECK(s_fake_zone_cfg[1].ki < ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT,
               "sanity: this fixture's 5 runs must stay well inside the cumulative bound");
}

static void test_ki_diagnosis_decreasing_direction_stabilizes_at_cumulative_floor(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f; // permanently under ADAPTIVE_TUNE_MIN_OBSERVATIONS -- same posture as
                                      // the increasing-direction fixture above
    s_fake_zone_cfg[1].ki = 100.0f;  // start deliberately oscillating

    const float base_amplitude_c = 5.0f; // well above the 0.05C noise floor at ki=100
    float last_ki = s_fake_zone_cfg[1].ki;
    bool ever_decreased = false; // Q1: see this variable's own role below -- without it, "stabilized" can
                                  // latch trivially at run 0 (ki_after == ki_before because NOTHING ever
                                  // fired), which is exactly the vacuous pass this fixture is supposed to
                                  // rule out.
    bool stabilized = false;
    int stabilized_at_run = -1;

    for (int run = 0; run < 60; run++) {
        float ki_before = s_fake_zone_cfg[1].ki;
        // Plant feedback: oscillation amplitude scales with Ki relative to
        // its start -- less Ki genuinely means less oscillation, the exact
        // self-limiting mechanism under test.
        // Q1: was ADAPTIVE_TUNE_KI_MIN_SAMPLES + 4 (16 samples) -- at this
        // fixture's 8-sample period that produced only 3 zero crossings,
        // below ADAPTIVE_TUNE_KI_MIN_CROSSINGS (4), so EVERY one of the 65
        // runs below classified as KI_OK (verdict never LIMIT_CYCLE/
        // OSCILLATING) and the decreasing-direction code path this test
        // claims to exercise never ran even once: replacing adaptive_tune_
        // ki.c's crossing/regularity branch condition with `if (0)` --
        // deleting the entire Ki-decrease capability -- left this test
        // GREEN. ADAPTIVE_TUNE_KI_TRACE_CAPACITY (24 samples, 3 complete
        // periods) instead gives ~6 crossings, comfortably above the floor,
        // so the decreasing direction genuinely runs on every iteration.
        //
        // Even with that fixed, the ORIGINAL stabilization detection below
        // (`if (!stabilized && ki_after == ki_before)`, with no prior
        // "actually decreased at least once" requirement) is STILL vacuous
        // against the `if (0)` mutation on its own: with Ki-decrease
        // entirely deleted, ki_after == ki_before == 100 already holds at
        // run 0, so `stabilized` latches true immediately and every
        // assertion below (stabilized, last_ki > 1.0f) passes trivially
        // without a single real decrease ever having happened. ever_
        // decreased (and the TEST_CHECK on it below) is what actually rules
        // that out -- it can only become true from a genuine ki_after <
        // ki_before observed inside this loop.
        float amplitude = base_amplitude_c * (ki_before / 100.0f);
        feed_oscillating_trace(1, 100.0f, amplitude, ADAPTIVE_TUNE_KI_TRACE_CAPACITY, DT_S);

        profile_firing_run_record_t rec = make_clean_record(300 + run, 1, 900);
        rec.zones[1].stats.dwell_err_mean_c = 0.0f; // irrelevant while the trace is still oscillating --
        rec.zones[1].stats.dwell_err_max_c = 0.0f;  // the crossing/regularity check wins first when it fires
        adaptive_tune_run_end(&rec, true);

        float ki_after = s_fake_zone_cfg[1].ki;
        if (ki_after < ki_before) {
            ever_decreased = true;
        }
        if (ever_decreased && !stabilized && ki_after == ki_before) {
            stabilized = true;
            stabilized_at_run = run;
        }
        last_ki = ki_after;
    }

    TEST_CHECK(ever_decreased, "P6/Q1: the decreasing direction must actually FIRE at least once in this "
                                "fixture -- MUST go red if adaptive_tune_ki.c's crossing/regularity branch "
                                "(the `if (amplitude > NOISE_FLOOR && ncross >= MIN_CROSSINGS)` condition, "
                                ":93) is disabled, e.g. mutated to `if (0)`");
    TEST_CHECK(stabilized, "P6: the decreasing direction must eventually STOP moving -- bound by the Q2 "
                            "cumulative floor at this fixture's amplitude scaling (see this test's own header "
                            "comment: amplitude here never falls anywhere near the noise floor on its own) -- "
                            "not collapse toward the ~1e-38 floor the bare positivity check alone would permit");
    TEST_CHECK(stabilized_at_run >= 0, "sanity: stabilization was actually observed within the 60-run loop");
    TEST_CHECK(last_ki < 100.0f, "P6: the decreasing direction must have genuinely moved Ki DOWN from its "
                                  "100.0 starting point by the time it stabilizes");
    TEST_CHECK(last_ki > 1.0f, "P6: the decreasing direction must stabilize at a real, physically meaningful "
                                "Ki, nowhere near a collapsed near-zero value");

    // Continue past stabilization and confirm it really has stopped, not
    // merely slowed (same "genuinely stable, not just decelerating" proof
    // the increasing-direction test above requires).
    for (int run = 60; run < 65; run++) {
        float amplitude = base_amplitude_c * (s_fake_zone_cfg[1].ki / 100.0f);
        feed_oscillating_trace(1, 100.0f, amplitude, ADAPTIVE_TUNE_KI_TRACE_CAPACITY, DT_S);
        profile_firing_run_record_t rec = make_clean_record(300 + run, 1, 900);
        adaptive_tune_run_end(&rec, true);
    }
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].ki, last_ki, 1e-4,
                     "P6: Ki must be genuinely stable past stabilization, not still slowly decaying");
}

// Q2: symmetric LOWER cumulative bound. P6 above drives the decreasing
// direction with an oscillation source that DOES scale with Ki -- and even
// there (see P6's own R3-corrected header comment), it is THIS floor, not
// plant feedback, that actually stops the decrease at this repo's fixture
// amplitudes. This test is the more direct proof, with a fixed-amplitude
// source that does NOT scale with Ki at all -- coupling from a neighbouring
// zone, relay chatter, thermocouple noise -- which the OLD code (only
// `!(new_ki > 0.0f)`) would decay without limit, 20%/run, toward the
// setter's positivity floor. MUST FAIL if adaptive_tune_ki.c's new lower-
// bound check is removed (or its comparison inverted) -- the reason string
// would never contain "cumulative floor" and stored Ki would keep dropping
// well below baseline/ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT.
// ---------------------------------------------------------------------
static void test_ki_diagnosis_cumulative_floor_binds_and_names_itself(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f; // permanently under ADAPTIVE_TUNE_MIN_OBSERVATIONS -- keeps the model
                                      // refine un-due so the Ki diagnosis gets every run (same posture as
                                      // every other Ki-diagnosis fixture in this file)
    s_fake_zone_cfg[1].ki = 100.0f;

    bool saw_a_floor_refusal_after_shrink = false;
    for (int run = 0; run < 30; run++) {
        // Fixed amplitude -- deliberately NOT scaled with Ki, unlike P6's
        // fixture: this is the "source does not scale with Ki" case the
        // Q2 fix exists for. Stays well above the 0.05C noise floor even
        // once Ki has shrunk to a small fraction of its start.
        feed_oscillating_trace(1, 100.0f, 5.0f, ADAPTIVE_TUNE_KI_TRACE_CAPACITY, DT_S);
        profile_firing_run_record_t rec = make_clean_record(400 + run, 1, 900);
        rec.zones[1].stats.dwell_err_mean_c = 0.0f;
        rec.zones[1].stats.dwell_err_max_c = 0.0f;
        adaptive_tune_run_end(&rec, true);
        if (s_fake_zone_cfg[1].ki < 100.0f && !adaptive_tune_zones[1].ki_applied &&
            strstr(adaptive_tune_zones[1].ki_refusal_reason, "cumulative floor") != NULL) {
            saw_a_floor_refusal_after_shrink = true;
        }
    }
    TEST_CHECK(saw_a_floor_refusal_after_shrink,
               "Q2: under an oscillation source that does not scale with Ki, Ki must shrink until the "
               "CUMULATIVE FLOOR actually refuses it, and say so by name -- not silently keep decaying");
    TEST_CHECK(strstr(adaptive_tune_zones[1].ki_refusal_reason, "re-autotune") != NULL,
               "Q2: once the cumulative floor binds, the refusal must recommend re-autotuning this zone, same "
               "remediation as the upper cumulative bound");
    float floor = 100.0f / ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT; // baseline latched at 100.0 (setup value)
    TEST_CHECK(s_fake_zone_cfg[1].ki >= floor - 1e-3f,
               "Q2: the stored Ki must never be left below the cumulative floor once it starts binding");
    TEST_CHECK(s_fake_zone_cfg[1].ki < 100.0f,
               "sanity: Ki must have genuinely shrunk from its starting value across this loop");
}

// Q2: the "not blocked" half -- a zone whose oscillation genuinely stops
// needing correction after ONE modest, well-inside-the-floor shrink must
// apply that correction normally, with no floor refusal anywhere near it.
// Distinguishes "the floor exists" (test above) from "the floor is not so
// aggressive it clips an ordinary single-run correction" -- a floor guard
// implemented as e.g. "refuse ANY decrease once baseline is latched" would
// pass the test above but fail this one.
static void test_ki_diagnosis_cumulative_floor_does_not_block_legitimate_convergence(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].ki = 100.0f;

    feed_oscillating_trace(1, 100.0f, 5.0f, ADAPTIVE_TUNE_KI_TRACE_CAPACITY, DT_S);
    profile_firing_run_record_t rec = make_clean_record(500, 1, 900);
    rec.zones[1].stats.dwell_err_mean_c = 0.0f;
    rec.zones[1].stats.dwell_err_max_c = 0.0f;
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(adaptive_tune_zones[1].ki_applied,
               "Q2: a single, modest (20%%-capped) shrink well inside the 5x floor must apply normally");
    TEST_CHECK(strstr(adaptive_tune_zones[1].ki_refusal_reason, "cumulative floor") == NULL,
               "Q2: a legitimately converging zone's first correction must never be refused by the new floor "
               "guard -- MUST go red if the floor is implemented as an unconditional decrease-refusal instead "
               "of the actual baseline/5 comparison");
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].ki, 80.0f, 1e-2,
                     "sanity: the applied correction is exactly the per-run cap (100 * (1 - 0.20))");
}

// P1: the cumulative Ki bound (ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT) is
// defeated by one power cycle if ki_baseline/ki_baseline_valid are RAM-only.
// Instrumented (see this fix's own review): boot 0 latches baseline=1.000
// (live Ki also 1.0 -- first touch); by boot 1 Ki has grown to 46.005 and,
// with no persisted baseline, RE-LATCHES from that already-grown value;
// boot 2 reaches the exact 850.564 runaway the bound exists to prevent.
// Kilns are power-cycled between firings, so the RAM-only bound gave one
// session of protection and then none.
//
// This test simulates exactly that: latch a baseline, grow Ki past it,
// "reboot" (memset adaptive_tune_zones -- the same simulated-reboot idiom test_enable_
// round_trips_through_persistence() above already uses for en_mask), then
// prove the NEXT latch attempt reloads the ORIGINAL baseline from NVS
// rather than re-latching from the grown live Ki. MUST FAIL on code with no
// Ki-baseline NVS persistence (adaptive_tune_init() only ever reloading
// en_mask) -- there, after the simulated reboot, ki_baseline_valid is false
// again, and the very next adaptive_tune_refine_ki_locked() call re-latches from
// whatever Ki is then live (the grown value), reproducing the runaway.
static void test_ki_baseline_survives_reboot_not_relatched_from_grown_ki(void)
{
    reset_module_state();
    fake_kv_reset_all();
    hal_kv_init_partition(ADAPTIVE_TUNE_NVS_PARTITION);

    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f; // permanently under ADAPTIVE_TUNE_MIN_OBSERVATIONS -- see the
                                      // closed-loop convergence fixture's identical setup comment: keeps
                                      // the model refine un-due so the Ki diagnosis gets every run
    s_fake_zone_cfg[1].ki = 1.0f;

    // Run 1: a genuine OFFSET_TOO_SMALL correction, which latches the
    // baseline at the CURRENT (pre-growth) live Ki, then grows it by the
    // per-run cap.
    feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
    profile_firing_run_record_t rec1 = make_clean_record(1, 1, 900);
    rec1.zones[1].stats.dwell_err_mean_c = 0.45f;
    rec1.zones[1].stats.dwell_err_max_c = 0.50f;
    adaptive_tune_run_end(&rec1, true);
    TEST_CHECK(adaptive_tune_zones[1].ki_applied, "setup: run 1 must genuinely apply a Ki correction");
    TEST_CHECK(adaptive_tune_zones[1].ki_baseline_valid, "setup: run 1 must latch a baseline");
    TEST_CHECK_NEAR(adaptive_tune_zones[1].ki_baseline, 1.0f, 1e-4, "setup: baseline latches at the PRE-growth Ki");
    TEST_CHECK(s_fake_zone_cfg[1].ki > 1.0f, "setup: Ki must have grown past the baseline this same run");
    float grown_ki = s_fake_zone_cfg[1].ki;

    // Simulate a reboot: RAM state gone, reload from (stubbed) NVS -- same
    // idiom as test_enable_round_trips_through_persistence() above. The
    // fake zone config table (s_fake_zone_cfg, standing in for the REAL
    // persisted zones_config blob) is deliberately NOT reset here -- a real
    // reboot keeps the persisted Ki exactly where the last firing left it,
    // it only loses THIS module's own RAM state.
    memset(adaptive_tune_zones, 0, sizeof(adaptive_tune_zones));
    adaptive_tune_init();
    adaptive_tune_zones[1].enabled = true; // re-opt-in, as an operator would find it (persisted via en_mask on
                                // real hardware; set directly here since enable persistence is
                                // covered by its own test above and is not what this test is about)

    TEST_CHECK(s_fake_zone_cfg[1].ki > 1.0f && s_fake_zone_cfg[1].ki == grown_ki,
               "setup: the persisted Ki (unlike this module's RAM state) survives the simulated reboot, "
               "already grown past the original baseline");

    // Run 2, post-reboot: another genuine OFFSET_TOO_SMALL correction. The
    // defect under test is what baseline this latches (or re-latches) at.
    feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
    profile_firing_run_record_t rec2 = make_clean_record(2, 1, 900);
    rec2.zones[1].stats.dwell_err_mean_c = 0.45f;
    rec2.zones[1].stats.dwell_err_max_c = 0.50f;
    adaptive_tune_run_end(&rec2, true);
    TEST_CHECK(adaptive_tune_zones[1].ki_applied, "setup: run 2 (post-reboot) must also genuinely apply a Ki correction");

    TEST_CHECK(adaptive_tune_zones[1].ki_baseline_valid, "P1: the baseline must be valid again after a reboot");
    TEST_CHECK_NEAR(adaptive_tune_zones[1].ki_baseline, 1.0f, 1e-4,
                     "P1: the baseline must RELOAD from persisted NVS state (1.0, the original autotuned "
                     "value) after a reboot, not RE-LATCH from the already-grown live Ki -- this is the "
                     "exact defect that let one power cycle reach an 850.6x runaway (measured: boot 0 "
                     "latch=1.000, boot 1 latch=46.005 from the grown live Ki, boot 2 latch=850.564)");

    fake_kv_reset_all(); // leave the shared fake state as every other test in this binary expects
}

// Q3: adaptive_tune_clear_ki_baseline() is the escape hatch the cumulative-
// bound refusal message names ("-- re-autotune this zone"). This proves it
// actually works: latch a baseline, grow Ki past a point where a NEW
// (lower) baseline would raise the ceiling, clear it, and confirm the very
// next latch takes the CURRENT live Ki, not the stale one -- both in RAM and
// in the persisted NVS blob (same reboot-survival proof P1's own test uses).
// MUST FAIL if adaptive_tune_clear_ki_baseline() is a no-op (e.g. its two
// RAM-clearing lines removed): ki_baseline_valid would stay true throughout,
// so the post-clear run below would never re-latch at all.
// ---------------------------------------------------------------------
static void test_clear_ki_baseline_lets_the_next_run_relatch_fresh(void)
{
    reset_module_state();
    fake_kv_reset_all();
    hal_kv_init_partition(ADAPTIVE_TUNE_NVS_PARTITION);

    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].ki = 1.0f;

    feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
    profile_firing_run_record_t rec1 = make_clean_record(1, 1, 900);
    rec1.zones[1].stats.dwell_err_mean_c = 0.45f;
    rec1.zones[1].stats.dwell_err_max_c = 0.50f;
    adaptive_tune_run_end(&rec1, true);
    TEST_CHECK(adaptive_tune_zones[1].ki_baseline_valid, "setup: run 1 must latch a baseline");
    TEST_CHECK_NEAR(adaptive_tune_zones[1].ki_baseline, 1.0f, 1e-4, "setup: baseline latches at the pre-growth Ki");
    float grown_ki = s_fake_zone_cfg[1].ki;
    TEST_CHECK(grown_ki > 1.0f, "setup: Ki must have grown past the baseline this same run");

    // The "re-autotune" the refusal message names -- clears the stale
    // baseline for this zone, exactly as autotune_engine.c's accept path
    // now does after committing a fresh result.
    adaptive_tune_clear_ki_baseline(1);
    TEST_CHECK(!adaptive_tune_zones[1].ki_baseline_valid,
               "Q3: adaptive_tune_clear_ki_baseline() must invalidate the RAM baseline immediately");

    // Reboot-survival, checked BEFORE the next run re-latches anything: the
    // clear must have persisted too, or a power cycle between the clear and
    // the next run would resurrect the stale baseline from NVS -- same
    // defect class P1 fixed for growth, now checked for the clear path
    // specifically. Uses its own re-opt-in, same idiom as P1's own reboot
    // test above (en_mask persistence is that test's concern, not this
    // one's).
    memset(adaptive_tune_zones, 0, sizeof(adaptive_tune_zones));
    adaptive_tune_init();
    TEST_CHECK(!adaptive_tune_zones[1].ki_baseline_valid,
               "Q3: a cleared baseline must reload as INVALID after a reboot too -- the clear must reach NVS, "
               "not just RAM");
    adaptive_tune_zones[1].enabled = true; // re-opt-in, as an operator would find it post-reboot

    // Next run, post-reboot: another genuine OFFSET_TOO_SMALL correction.
    // The baseline must re-latch at the CURRENT (already-grown) live Ki, not
    // the value cleared above -- this zone's plant model genuinely changed
    // (that is what "re-autotuned" means), so grown_ki is now the right
    // reference.
    feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
    profile_firing_run_record_t rec2 = make_clean_record(2, 1, 900);
    rec2.zones[1].stats.dwell_err_mean_c = 0.45f;
    rec2.zones[1].stats.dwell_err_max_c = 0.50f;
    adaptive_tune_run_end(&rec2, true);
    TEST_CHECK(adaptive_tune_zones[1].ki_applied, "setup: run 2 (post-clear) must also genuinely apply a Ki correction");
    TEST_CHECK(adaptive_tune_zones[1].ki_baseline_valid, "Q3: the baseline must be valid again after re-latching");
    TEST_CHECK_NEAR(adaptive_tune_zones[1].ki_baseline, grown_ki, 1e-4,
                     "Q3: the re-latched baseline must track the CURRENT live Ki (the whole point of clearing "
                     "it after a re-autotune), not silently keep the value that was just cleared");

    fake_kv_reset_all(); // leave the shared fake state as every other test in this binary expects
}

// ---------------------------------------------------------------------
// R1 (opus review of commit 7c47683 -- SHIPPING BLOCKER): autotune_engine_
// accept()'s call to adaptive_tune_clear_ki_baseline() is reachable from the
// PC-tools UART GUI's Accept button (both step and relay accept paths), and
// on THAT path autotune_engine_accept() is already running ON the flash-safe
// worker (uart_bridge_ext.c's AUTOTUNE_CMD_ACCEPT dispatches its whole switch
// body there). adaptive_tune_clear_ki_baseline() itself dispatches onto that
// same worker to persist the cleared baseline -- naively doing so a second
// time, from inside the first dispatch, deadlocks the board permanently
// (non-recursive lock already held by the blocked original caller; the
// 1-deep queue behind it is only ever drained by the very worker task now
// stuck trying to enqueue into it). Recovery required a reboot, and it took
// down control/profiles/autotune UART bridges and safety_cfg_store's
// deferred NVS flush with it -- see uart_bridge_ext.c's HAZARD block and
// adaptive_tune_clear_ki_baseline()'s own comment for the full incident.
//
// This test simulates that exact call shape: dispatch a job onto the (host)
// worker stub, and from INSIDE that job -- with s_stub_on_flash_worker set
// true, standing in for "we are bx_flash_worker" -- call the real
// adaptive_tune_clear_ki_baseline(). The fix is that this function must
// consult uart_bridge_ext_is_on_flash_worker() and do the save inline rather
// than dispatching again. MUST GO RED if that check is removed (i.e. if
// adaptive_tune_clear_ki_baseline() unconditionally calls uart_bridge_ext_
// run_on_flash_worker()): the stub above models the real worker's non-
// recursive-lock/depth-1-queue semantics, so the re-entrant dispatch trips
// its busy check and fails loudly instead of silently succeeding -- which is
// exactly what the OLD (bare `fn(arg)`) stub could never have caught. Proven
// red 2026-09-01 by commenting out the uart_bridge_ext_is_on_flash_worker()
// branch in adaptive_tune_clear_ki_baseline() and re-running this file: this
// test failed with the re-entrancy message above, everything else unchanged.
// ---------------------------------------------------------------------
static uint8_t s_accept_like_zone;

static void accept_like_job_calls_clear_ki_baseline(void *arg)
{
    (void)arg;
    // Mirrors autotune_engine_accept() running ON the worker (UART bridge
    // accept path) and then calling adaptive_tune_clear_ki_baseline() before
    // returning -- see this function's caller for the outer dispatch that
    // puts us here in the first place.
    s_stub_on_flash_worker = true;
    adaptive_tune_clear_ki_baseline(s_accept_like_zone);
    s_stub_on_flash_worker = false;
}

static void test_accept_path_clear_ki_baseline_does_not_reenter_worker(void)
{
    reset_module_state();
    fake_kv_reset_all();
    hal_kv_init_partition(ADAPTIVE_TUNE_NVS_PARTITION);

    adaptive_tune_zones[2].enabled = true;
    adaptive_tune_zones[2].ki_baseline_valid = true;
    adaptive_tune_zones[2].ki_baseline = 3.5f;
    s_accept_like_zone = 2;

    int failures_before = g_test_failures;

    // The outer dispatch: models AUTOTUNE_CMD_ACCEPT handing the whole
    // switch body -- including autotune_engine_accept()'s call into
    // adaptive_tune_clear_ki_baseline() -- to bx_flash_worker.
    esp_err_t err = uart_bridge_ext_run_on_flash_worker(accept_like_job_calls_clear_ki_baseline, NULL);

    TEST_CHECK(err == ESP_OK, "R1: the accept-path dispatch itself must not fail");
    TEST_CHECK(g_test_failures == failures_before,
               "R1: adaptive_tune_clear_ki_baseline() must not re-enter the flash worker when the caller is "
               "already on it -- a re-entrant dispatch here is the exact shape of the commit-7c47683 deadlock");
    TEST_CHECK(!adaptive_tune_zones[2].ki_baseline_valid,
               "R1: the baseline must still actually be cleared -- the fix must not just avoid the deadlock by "
               "skipping the work");

    fake_kv_reset_all();
}

// ---------------------------------------------------------------------
// S1 (2026-09-01 audit of ae5905f, corrected 2026-09-01 re-audit): the
// same dispatch mechanism as R1, reached from a second call site --
// profile_executor_halt() (profile_executor_status.c) is itself one of
// uart_bridge_ext.c's own on-worker calls, and it reaches
// adaptive_tune_run_end() directly. adaptive_tune_run_end() dispatches
// save_kibase_job() onto the flash worker with NO uart_bridge_ext_is_on_
// flash_worker() guard whenever baseline_newly_latched is true -- but that
// path is NOT actually re-entrant through profile_executor_halt(): as
// literally written today, profile_executor_status.c always passes
// `clean=false` to this call, which forces every zone's run_end loop
// iteration through the skip-and-continue branch and so can never flip
// baseline_newly_latched true through THAT specific call site. The guard
// added here is defensive -- correct if `clean` ever stops being a
// constant -- not a fix for a live deadlock.
//
// This test calls adaptive_tune_run_end() directly rather than through
// profile_executor_halt() itself, and deliberately drives it into the
// state profile_executor_halt() can never reach: it uses the model-refine
// recipe from test_model_refine_relatches_ki_baseline_to_fresh_simc_ki()
// below to genuinely latch a NEW baseline (baseline_newly_latched = true)
// inside the call under test, under the same worker-dispatch stub
// profile_executor_halt() would use.
//
// MUST GO RED if adaptive_tune_run_end()'s uart_bridge_ext_is_on_flash_
// worker() check (adaptive_tune.c, the `if (baseline_newly_latched)` block)
// is reverted to an unconditional uart_bridge_ext_run_on_flash_worker()
// call: the shared stub's busy check (stubs/bx_worker_stub.h) trips and
// fails loudly, exactly like R1's test above. Proven red 2026-09-01 by
// reverting that guard and re-running this file -- captured in the S1/S2
// audit report.
static uint8_t s_halt_like_zone;

static void halt_like_job_calls_run_end(void *arg)
{
    profile_firing_run_record_t *rec = (profile_firing_run_record_t *)arg;
    // Mirrors profile_executor_halt() running ON the worker (reached over
    // the UART bridge, per uart_bridge_ext.c's own on-worker list) and then
    // calling adaptive_tune_run_end() before returning.
    s_stub_on_flash_worker = true;
    adaptive_tune_run_end(rec, true);
    s_stub_on_flash_worker = false;
}

static void test_halt_path_run_end_does_not_reenter_worker(void)
{
    reset_module_state();
    fake_kv_reset_all();
    hal_kv_init_partition(ADAPTIVE_TUNE_NVS_PARTITION);

    s_halt_like_zone = 0;
    adaptive_tune_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 10.0f;
    s_fake_zone_cfg[0].tau_s = 120.0f;
    s_fake_zone_cfg[0].dead_time_s = 15.0f;
    s_fake_zone_cfg[0].ki = 0.01f; // deliberately NOT SIMC-consistent, so this run's recompute
                                    // is guaranteed to write a genuinely different Ki and latch
                                    // a fresh baseline -- see test_model_refine_relatches_ki_
                                    // baseline_to_fresh_simc_ki() below for the same recipe.
    TEST_CHECK(!adaptive_tune_zones[0].ki_baseline_valid, "setup: zone starts with no baseline latched");

    feed_settled_dwell(0, 22.0f + 10.5f * 0.30f, 22.0f, 0.30f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.5f * 0.50f, 22.0f, 0.50f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.5f * 0.70f, 22.0f, 0.70f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.5f * 0.90f, 22.0f, 0.90f, SETTLE_TICKS, DT_S);

    profile_firing_run_record_t rec = make_clean_record(601, 0, 900);

    int failures_before = g_test_failures;

    // The outer dispatch: models the UART bridge handing an on-worker
    // command (which ends in profile_executor_halt()) to bx_flash_worker.
    esp_err_t err = uart_bridge_ext_run_on_flash_worker(halt_like_job_calls_run_end, &rec);

    TEST_CHECK(err == ESP_OK, "S1: the halt-path dispatch itself must not fail");
    TEST_CHECK(g_test_failures == failures_before,
               "S1: adaptive_tune_run_end() must not re-enter the flash worker when the caller is already on "
               "it -- a re-entrant dispatch here is the identical deadlock shape as R1's accept path, just "
               "reached from profile_executor_halt() instead of autotune_engine_accept()");
    TEST_CHECK(adaptive_tune_zones[0].ki_baseline_valid,
               "S1: the baseline must still actually be latched and saved -- the fix must not just avoid the "
               "deadlock by skipping the work");

    fake_kv_reset_all();
}

// Q4: the model layer (adaptive_tune_refine_zone_locked(), adaptive_tune_model.c)
// rewrites Ki from a fresh SIMC recompute independent of the Ki-diagnosis
// layer, and used to leave ki_baseline untouched -- so a zone whose SIMC
// refine legitimately raised Ki past 5x a STALE baseline had its diagnosis
// layer muted permanently, with the "re-autotune" remedy not even being the
// path that actually fires here (D5: the model refine runs INSTEAD of the
// Ki diagnosis on any run where it applies). This proves the model layer
// now re-latches ki_baseline to its own freshly-written SIMC Ki. MUST FAIL
// if adaptive_tune_model.c's two ki_baseline-writing lines are removed:
// ki_baseline would stay at its old value (or invalid) instead of tracking
// gains.ki.
// ---------------------------------------------------------------------
static void test_model_refine_relatches_ki_baseline_to_fresh_simc_ki(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 10.0f;
    s_fake_zone_cfg[0].tau_s = 120.0f;
    s_fake_zone_cfg[0].dead_time_s = 15.0f;
    s_fake_zone_cfg[0].ki = 0.01f; // deliberately NOT SIMC-consistent with k_dc/tau/dead_time, so this run's
                                    // recompute is guaranteed to write a genuinely different Ki

    TEST_CHECK(!adaptive_tune_zones[0].ki_baseline_valid, "setup: zone starts with no baseline latched at all");

    // A clean, well-spread ring that clears every model-refine guard --
    // same shape as test_refinement_improves_gain_estimate_on_known_plant()
    // above, true gain close enough to prior K to clear the jump-ratio
    // guard but far enough to be a material move.
    feed_settled_dwell(0, 22.0f + 10.5f * 0.30f, 22.0f, 0.30f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.5f * 0.50f, 22.0f, 0.50f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.5f * 0.70f, 22.0f, 0.70f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.5f * 0.90f, 22.0f, 0.90f, SETTLE_TICKS, DT_S);

    profile_firing_run_record_t rec = make_clean_record(600, 0, 900);
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(adaptive_tune_zones[0].has_applied, "setup: the model refine must genuinely apply this run");
    float fresh_ki = s_fake_zone_cfg[0].ki;
    TEST_CHECK(fabsf(fresh_ki - 0.01f) > 1e-6f, "setup: the SIMC recompute must have genuinely rewritten Ki");

    TEST_CHECK(adaptive_tune_zones[0].ki_baseline_valid,
               "Q4: the model refine must latch a baseline even though adaptive_tune_refine_ki_locked() never ran this "
               "run (D5: the model refine wins, the Ki diagnosis is skipped)");
    TEST_CHECK_NEAR(adaptive_tune_zones[0].ki_baseline, fresh_ki, 1e-6,
                     "Q4: the baseline must track the model layer's fresh SIMC Ki specifically -- the value "
                     "zones_config_set_pid() was just called with -- not the old pre-refine Ki (0.01) it "
                     "replaced");
}

// ---------------------------------------------------------------------
// R6 (opus review, gain_scheduling_design_2026-09-13.md, appended
// 2026-09-13, docs/audits/adaptive_tune_ki_effective_reference_loop_
// 2026-09-13.md): adaptive_tune_ki.c's diagnosis is computed from the
// EFFECTIVE closed-loop trace (z->trace_actual_c[]/trace_duty[]), but its
// correction is written into zones_config's REFERENCE Ki. When a zone runs
// ZONE_CONTROL_MODE_PID_FUZZY with fuzzy_strength_pct > 0,
// pid_fuzzy_adjust() rescales the tick's applied Ki away from that
// reference -- so a persistent fuzzy nudge (the centre rule cell, where
// 100% of one real hardware capture's samples land per
// docs/audits/fuzzy_nine_cell_offline_probe_2026-09-11.md) shows up in the
// trace as a steady offset this layer would otherwise "correct" by
// ratcheting the reference by a fixed ~20%/run, forever, without ever
// resolving the divergence fuzzy is causing -- structurally the same
// generating fault the K_dc ratchets fixed in 97288659/36f88d62 (a
// correction inferred from a transformed observable, written back to the
// untransformed reference), now for Ki instead of K_dc.
//
// This proves the fix (adaptive_tune_ki.c's effective-vs-reference guard,
// checked ahead of the OFFSET_TOO_SMALL apply path): with the zone in
// PID_FUZZY at strength 50 and a CONSTANT offset trace that would
// otherwise classify OFFSET_TOO_SMALL on every single run (same fixture
// shape as test_ki_diagnosis_runaway_under_constant_error_is_capped_by_
// cumulative_bound() above, which proves the OPPOSITE zone -- plain PID --
// genuinely does apply and ratchet under this identical trace), the
// reference Ki must not move across repeated runs, and the refusal reason
// must name fuzzy so an operator is not left guessing.
static void test_ki_diagnosis_withholds_correction_when_zone_is_pid_fuzzy(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f; // ring never reaches ADAPTIVE_TUNE_MIN_OBSERVATIONS -- same convention
                                      // as this file's other closed-loop/runaway fixtures, keeps the model
                                      // refine permanently un-due so the Ki diagnosis gets every run
    s_fake_zone_cfg[1].ki = 1.0f;
    s_fake_zone_cfg[1].control_mode = ZONE_CONTROL_MODE_PID_FUZZY;
    s_fake_zone_cfg[1].fuzzy_strength_pct = 50.0f;

    for (int run = 0; run < 10; run++) {
        feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
        profile_firing_run_record_t rec = make_clean_record(700 + run, 1, 900);
        // Identical constant-offset shape to the plain-PID runaway fixture
        // above -- this is the SAME evidence that genuinely ratchets a
        // plain-PID zone; the only difference here is control_mode/
        // fuzzy_strength_pct.
        rec.zones[1].stats.dwell_err_mean_c = 0.45f;
        rec.zones[1].stats.dwell_err_max_c = 0.50f;
        adaptive_tune_run_end(&rec, true);

        TEST_CHECK(!adaptive_tune_zones[1].ki_applied,
                   "R6: a PID_FUZZY zone at non-zero strength must never have its reference Ki corrected by "
                   "this layer -- the trace reflects fuzzy's effective Ki, not the stored reference");
        TEST_CHECK(strstr(adaptive_tune_zones[1].ki_refusal_reason, "fuzzy") != NULL ||
                       strstr(adaptive_tune_zones[1].ki_refusal_reason, "FUZZY") != NULL,
                   "R6: the refusal reason must name fuzzy as the cause, not read like an ordinary "
                   "no-correction-indicated verdict");
        // K7 (docs/audits/ki_refusal_truncation_and_drift_check_lock_2026-09-13.md):
        // the previous wording here was 163 bytes into the 96-byte
        // ki_refusal_reason buffer and vsnprintf silently truncated it at
        // "...not the stored " -- a plain strstr(..., "fuzzy") check above
        // still passed because "fuzzy" lands at character 57, well inside
        // the surviving prefix, so it never caught the loss of the whole
        // actionable second half of the message. Guard against that class
        // recurring: the formatted length must leave room for the NUL (a
        // vsnprintf that filled/truncated the buffer writes bufsz-1 chars
        // plus NUL), and a token that only appears in the END of the
        // intended message must survive.
        size_t reason_len = strlen(adaptive_tune_zones[1].ki_refusal_reason);
        TEST_CHECK(reason_len < sizeof(adaptive_tune_zones[1].ki_refusal_reason) - 1,
                   "R6/K7: fuzzy refusal reason must not fill (i.e. truncate into) its 96-byte buffer");
        TEST_CHECK(strstr(adaptive_tune_zones[1].ki_refusal_reason, "reference") != NULL,
                   "R6/K7: 'reference' (the actionable end of the message -- why the correction is "
                   "withheld) must survive; its absence is exactly what the old 163-byte wording did");
    }
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].ki, 1.0f, 1e-6,
                     "R6: 10 runs of the exact trace that ratchets a plain-PID zone past 3x baseline "
                     "(see the sibling cumulative-bound test) must leave a PID_FUZZY zone's reference Ki "
                     "COMPLETELY UNCHANGED -- any movement here is the effective-vs-reference loop closing");
}

// ---------------------------------------------------------------------
// K8, defect 1 (docs/audits/adaptive_tune_ki_guard_timing_and_failopen_
// 2026-09-14.md): the guard above must engage from the state the trace was
// actually captured under, not whatever zones_config reports at refine
// time. Proves the fix by reproducing exactly the failure the review
// described: fuzzy is ON for the whole dwell that produces the trace, then
// switched OFF (control_mode back to plain PID) before adaptive_tune_
// run_end() ever runs. A refine-time read (the pre-fix behaviour) would see
// plain PID and let the same constant-offset trace ratchet the reference
// Ki, exactly like test_ki_diagnosis_runaway_under_constant_error_is_capped_
// by_cumulative_bound() above proves it does for a genuinely-plain-PID
// zone. The dwell-entry snapshot must still catch it.
// ---------------------------------------------------------------------
static void test_ki_diagnosis_withholds_when_fuzzy_active_during_capture_but_off_at_refine(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f; // keeps the model refine permanently un-due, same convention as
                                      // the sibling PID_FUZZY test above
    s_fake_zone_cfg[1].ki = 1.0f;

    // Fuzzy ON for the ENTIRE dwell that generates this trace -- the
    // dwell-entry snapshot (adaptive_tune_zone_tick()'s dwell_just_entered
    // branch) must observe this and latch trace_fuzzy_active true.
    s_fake_zone_cfg[1].control_mode = ZONE_CONTROL_MODE_PID_FUZZY;
    s_fake_zone_cfg[1].fuzzy_strength_pct = 50.0f;
    feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);

    // Switched OFF before refine -- a real board can only do this between
    // firings (the interlocks this file's own guard comment cites refuse
    // while firing/hot/heater-commanded), which is exactly the gap this
    // fix closes: adaptive_tune_run_end() below runs with plain PID
    // configured NOW, even though the trace it is about to diagnose was
    // gathered entirely under fuzzy.
    s_fake_zone_cfg[1].control_mode = ZONE_CONTROL_MODE_PID;
    s_fake_zone_cfg[1].fuzzy_strength_pct = 0.0f;

    profile_firing_run_record_t rec = make_clean_record(701, 1, 900);
    rec.zones[1].stats.dwell_err_mean_c = 0.45f;
    rec.zones[1].stats.dwell_err_max_c = 0.50f;
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(!adaptive_tune_zones[1].ki_applied,
               "K8/defect1: fuzzy was active for the ENTIRE traced dwell -- the guard must still "
               "withhold even though control_mode now reads plain PID at refine time");
    TEST_CHECK(strstr(adaptive_tune_zones[1].ki_refusal_reason, "fuzzy") != NULL ||
                   strstr(adaptive_tune_zones[1].ki_refusal_reason, "FUZZY") != NULL,
               "K8/defect1: the refusal reason must still name fuzzy as the cause");
    size_t reason_len = strlen(adaptive_tune_zones[1].ki_refusal_reason);
    TEST_CHECK(reason_len < sizeof(adaptive_tune_zones[1].ki_refusal_reason) - 1,
               "K8/defect1: fuzzy-at-capture refusal reason must not fill (truncate into) its 96-byte buffer");
    TEST_CHECK(strstr(adaptive_tune_zones[1].ki_refusal_reason, "reference") != NULL,
               "K8/defect1: 'reference' (the actionable end of the message) must survive");
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].ki, 1.0f, 1e-6,
                     "K8/defect1: the reference Ki must be left completely unchanged -- a refine-time-only "
                     "read would have let this exact trace ratchet it, same as the plain-PID runaway "
                     "fixture");
}

// ---------------------------------------------------------------------
// K8, defect 2 (same audit doc): a zones_config accessor failure at
// snapshot time must withhold the correction (fail CLOSED), never let it
// proceed as if fuzzy were simply not active. Uses the fail-injection
// knobs added to the shared zones_config_get_control_mode()/
// get_fuzzy_strength_pct() fakes (test_adaptive_tune.c) -- the ordinary
// TEST_MAX_ZONES range check can never exercise this, since every real
// zone index used in these tests is well inside that range.
// ---------------------------------------------------------------------
static void test_ki_diagnosis_withholds_when_control_mode_accessor_fails_at_capture(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].ki = 1.0f;
    s_fake_zone_cfg[1].control_mode = ZONE_CONTROL_MODE_PID; // plain PID -- if the guard failed OPEN on
                                                               // the accessor error, nothing else here
                                                               // would stop the correction from applying
    s_fake_zone_cfg[1].fuzzy_strength_pct = 0.0f;

    s_fake_control_mode_fail = true; // forces the dwell-entry snapshot's accessor call to fail
    feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
    s_fake_control_mode_fail = false; // restore before run_end -- a live re-read must not paper over
                                       // a failure that happened at capture time

    profile_firing_run_record_t rec = make_clean_record(702, 1, 900);
    rec.zones[1].stats.dwell_err_mean_c = 0.45f;
    rec.zones[1].stats.dwell_err_max_c = 0.50f;
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(!adaptive_tune_zones[1].ki_applied,
               "K8/defect2: a control-mode accessor failure at dwell-entry snapshot time must withhold "
               "the correction, not let it proceed because the zone is (as far as anything else can "
               "tell) plain PID");
    TEST_CHECK(strstr(adaptive_tune_zones[1].ki_refusal_reason, "accessor") != NULL,
               "K8/defect2: the refusal reason must name the accessor failure distinctly from the "
               "ordinary fuzzy-active refusal");
    TEST_CHECK(strstr(adaptive_tune_zones[1].ki_refusal_reason, "fail-closed") != NULL,
               "K8/defect2: the refusal reason must say this is the fail-closed path");
    size_t reason_len = strlen(adaptive_tune_zones[1].ki_refusal_reason);
    TEST_CHECK(reason_len < sizeof(adaptive_tune_zones[1].ki_refusal_reason) - 1,
               "K8/defect2: accessor-failure refusal reason must not fill (truncate into) its 96-byte "
               "buffer");
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].ki, 1.0f, 1e-6,
                     "K8/defect2: the reference Ki must be left completely unchanged when the accessor "
                     "fails at capture time");
}
