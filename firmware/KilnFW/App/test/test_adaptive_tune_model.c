// Split out of test_adaptive_tune.c (2026-09-01, kept the file under this repo's
// 1500-line guidance). Diagonal model refine: the K_dc fitting guards (minimum
// observations, duty spread, implausible-jump, dirty-run exclusion), the bounded
// per-run move, and the end-to-end refinement-improves-the-estimate proof.
// #included from test_adaptive_tune.c AFTER its fakes and test helpers -- see
// test_adaptive_tune_dwell.c's header comment for the shared-fixture convention.
// Pure refactor: no test removed, no assertion changed, no comment dropped.

static void test_min_observations_guard_rejects_too_few(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    // Only 3 dwells (ADAPTIVE_TUNE_MIN_OBSERVATIONS is 4), otherwise a
    // perfectly good, well-spread, on-model fit.
    feed_settled_dwell(1, 25.0f, 22.0f, 0.2f, SETTLE_TICKS, DT_S);   // rise 3, u 0.2
    feed_settled_dwell(1, 27.0f, 22.0f, 0.5f, SETTLE_TICKS, DT_S);   // rise 5, u 0.5
    feed_settled_dwell(1, 30.0f, 22.0f, 0.8f, SETTLE_TICKS, DT_S);   // rise 8, u 0.8
    TEST_CHECK(adaptive_tune_zones[1].ring_count == 3, "setup: exactly 3 observations queued");

    profile_firing_run_record_t rec = make_clean_record(2, 1, 900);
    adaptive_tune_run_end(&rec, true);
    TEST_CHECK(s_fake_zone_cfg[1].k_dc == 10.0f, "fewer than the minimum observation count must refuse the update");
    TEST_CHECK(strstr(adaptive_tune_zones[1].last_refusal_reason, "observations") != NULL,
               "refusal reason should name the observation-count shortfall");
}

// F3: adaptive_tune_run_end() must call the zones setters (model, pid, baseline
// bootstrap) with adaptive_tune_lock released -- see SETTER_LOCK_PROBE().
static void test_run_end_holds_no_lock_across_zones_setters(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].autotune_baseline_k_dc = 0.0f; // forces the baseline bootstrap setter too
    feed_settled_dwell(1, 25.0f, 22.0f, 0.2f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 27.0f, 22.0f, 0.5f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 30.0f, 22.0f, 0.8f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 29.0f, 22.0f, 0.65f, SETTLE_TICKS, DT_S);
    g_setter_calls = 0;
    g_setter_max_lock_depth = 0;
    TEST_CHECK(g_test_stub_lock_depth == 0, "setup: no lock held before run_end");
    profile_firing_run_record_t rec = make_clean_record(7, 1, 900);
    adaptive_tune_run_end(&rec, true);
    TEST_CHECK(g_setter_calls >= 3, "setup: run_end must reach the baseline, model and pid setters");
    TEST_CHECK(g_setter_max_lock_depth == 0, "F3: adaptive_tune_lock must not be held while the zones setters run");
    TEST_CHECK(g_test_stub_lock_depth == 0, "run_end must leave adaptive_tune_lock released");
    TEST_CHECK(adaptive_tune_zones[1].has_applied, "the refine must still commit its result after the apply");
}

static void test_duty_spread_guard_rejects_clustered_observations(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    // 4 observations, all essentially the same duty (spread << 0.05).
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(1, 25.0f, 22.0f, 0.50f, SETTLE_TICKS, DT_S);
    }
    TEST_CHECK(adaptive_tune_zones[1].ring_count == 4, "setup: 4 observations queued");

    profile_firing_run_record_t rec = make_clean_record(3, 1, 900);
    adaptive_tune_run_end(&rec, true);
    TEST_CHECK(s_fake_zone_cfg[1].k_dc == 10.0f, "clustered duty values (no spread) must refuse the update");
    TEST_CHECK(strstr(adaptive_tune_zones[1].last_refusal_reason, "clustered") != NULL,
               "refusal reason should name the duty-spread shortfall");
}

// P4: ADAPTIVE_TUNE_MIN_DUTY_SPREAD's own test above (test_duty_spread_
// guard_rejects_clustered_observations()) uses ZERO-spread duties, which
// proves the guard exists but pins no actual threshold value -- mutating
// the constant from 0.05 to 0.0001 leaves that test green (zero spread is
// still "clustered" under any positive floor). This test straddles the
// REAL boundary: spreads just under and just over the configured floor,
// with the underlying fit otherwise clean (true K == prior K == 10.0, no
// material move either way) so a "clustered" refusal can only come from
// the spread guard itself, in either direction.
// ---------------------------------------------------------------------
// Q5: the original straddle defined BOTH spreads as ADAPTIVE_TUNE_MIN_DUTY_
// SPREAD +/- 0.001f -- values COMPUTED FROM the very constant under test.
// That pins only one direction: LOWERING the threshold (0.05 -> 0.0001)
// reddens it (0.049 is no longer clustered against the new floor), but
// RAISING it (0.05 -> 0.5) leaves it GREEN, because both literals move in
// lockstep with the constant and the "over" spread (0.051) is still well
// under a 0.5 floor -- still clustered, so the "must clear the spread guard"
// check trivially fails to catch the widened threshold... except it doesn't
// even fail: it was never exercising the wider threshold's actual boundary
// at all. Fixed by pinning both literals to values INDEPENDENT of the
// constant (hand-picked around the real default of 0.05, not derived from
// the symbol) -- so a raise to 0.5 now makes 0.049/0.051 both "clustered"
// and the "must clear" check goes red, same as a lower to 0.0001 already did.
static void test_duty_spread_guard_pins_exact_threshold(void)
{
    float under_spread = 0.049f; // independent literal -- see this test's header comment
    float over_spread = 0.051f;  // independent literal -- see this test's header comment

    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    feed_settled_dwell(1, 22.0f + 10.0f * 0.500f, 22.0f, 0.500f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 10.0f * 0.500f, 22.0f, 0.500f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 10.0f * 0.500f, 22.0f, 0.500f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 10.0f * (0.500f + under_spread), 22.0f, 0.500f + under_spread, SETTLE_TICKS, DT_S);
    profile_firing_run_record_t rec_under = make_clean_record(10, 1, 900);
    adaptive_tune_run_end(&rec_under, true);
    TEST_CHECK(strstr(adaptive_tune_zones[1].last_refusal_reason, "clustered") != NULL,
               "P4: a duty spread just BELOW the configured floor must still refuse as clustered -- MUST go "
               "red by mutation if ADAPTIVE_TUNE_MIN_DUTY_SPREAD is lowered below this spread");

    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    feed_settled_dwell(1, 22.0f + 10.0f * 0.500f, 22.0f, 0.500f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 10.0f * 0.500f, 22.0f, 0.500f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 10.0f * 0.500f, 22.0f, 0.500f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 10.0f * (0.500f + over_spread), 22.0f, 0.500f + over_spread, SETTLE_TICKS, DT_S);
    profile_firing_run_record_t rec_over = make_clean_record(11, 1, 900);
    adaptive_tune_run_end(&rec_over, true);
    TEST_CHECK(strstr(adaptive_tune_zones[1].last_refusal_reason, "clustered") == NULL,
               "P4: a duty spread just ABOVE the configured floor must clear the spread guard specifically -- "
               "any remaining refusal here must come from a LATER guard (e.g. material-move), never spread");
}

// ---------------------------------------------------------------------
// P6: adaptive_tune_refine_ki_locked() has no LOWER bound on Ki -- only !(new_ki >
// 0.0f) at the setter boundary. The reviewer accepts this as self-limiting
// (less Ki -> less oscillation -> the LIMIT_CYCLE/OSCILLATING verdict stops
// firing) rather than an overtemp hazard, but that self-limiting behavior
// was UNPROVEN: only unit-level sign checks existed (Ki never goes
// negative), no multi-run integration test showing the DECREASING direction
// actually stabilizes the way the INCREASING direction's test above does.
// This drives the decreasing direction across many runs with oscillation
// amplitude modeled as scaling with Ki (a real closed loop: less integral
// gain, less overshoot/hunting) and requires convergence to a real,
// physically meaningful Ki -- not a collapse toward the ~1e-38 the bare
// positivity check alone would permit.
//
// R3 CORRECTION (opus review of commit 7c47683, 2026-09-01): this test's
// original name and comments claimed the decrease "self-limits under real
// plant feedback" -- i.e. that shrinking oscillation amplitude is what stops
// it. That claim is FALSE for the amplitude scaling actually used below
// (base_amplitude_c=5.0, amplitude = base_amplitude_c * ki/100): mutating
// ONLY the Q2 cumulative floor to `if (0)` (adaptive_tune_ki.c ~line 295)
// makes the last_ki > 1.0f assertion below FAIL. This fixture stabilizes at
// last_ki ~= 100*0.8^7 ~= 20.97 -- pinned AT the Q2 floor
// (ki_baseline/ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT = 100/5 = 20), not by
// plant feedback: at Ki=21 the amplitude here is still 5.0*0.21 = 1.05C,
// ~21x the 0.05C ADAPTIVE_TUNE_KI_NOISE_FLOOR_C, nowhere near the amplitude
// threshold that would make adaptive_tune_diagnose_ki() stop returning
// LIMIT_CYCLE on its own. So what this test actually proves is: the
// decreasing direction is bounded, and the Q2 cumulative floor is what
// bounds it -- not "plant feedback self-limits it", which would require the
// amplitude itself to fall to the noise floor first. Named and commented
// accurately below; see test_ki_diagnosis_cumulative_floor_binds_and_names_
// itself() (Q2, further down) for the dedicated floor test this one turns
// out to be a duplicate proof of, from the decreasing-Ki-diagnosis side
// rather than the fixed-amplitude side.
// ---------------------------------------------------------------------
static void feed_oscillating_trace(uint8_t zi, float mean_c, float amplitude_c, int n_samples, float dt_s)
{
    const float pi = 3.14159265358979f;
    adaptive_tune_zone_tick(zi, q1(mean_c), true, 0.5f, false, 22.0f, dt_s); // fresh dwell -- resets the trace
    for (int k = 0; k < n_samples; k++) {
        float phase = 2.0f * pi * (float)k / 8.0f; // period 8 samples, same as test_ki_diagnose_limit_cycle_
                                                     // yields_ku_tu()'s fixture above -- known to classify
                                                     // cleanly as a regular limit cycle
        float c = q1(mean_c + amplitude_c * sinf(phase));
        float duty = 0.5f + 0.1f * sinf(phase); // in-phase duty swing -- variance stays well above the
                                                  // floored-duty guard, so a shrinking trace falls through
                                                  // to the offset/OK evaluation, not a false FLOORED verdict
        adaptive_tune_zone_tick(zi, c, true, duty, true, 22.0f, dt_s);
    }
}

static void test_implausible_jump_guard_rejects_far_off_fit(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f; // prior
    // True gain here is 80 (8x the prior) -- ADAPTIVE_TUNE_MAX_JUMP_RATIO
    // is 5x, so this must be refused as implausible, not blended in.
    feed_settled_dwell(1, 22.0f + 80.0f * 0.2f, 22.0f, 0.2f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 80.0f * 0.4f, 22.0f, 0.4f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 80.0f * 0.6f, 22.0f, 0.6f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 80.0f * 0.8f, 22.0f, 0.8f, SETTLE_TICKS, DT_S);

    profile_firing_run_record_t rec = make_clean_record(4, 1, 900);
    adaptive_tune_run_end(&rec, true);
    TEST_CHECK(s_fake_zone_cfg[1].k_dc == 10.0f, "a fit >5x the prior model must be refused as implausible");
    TEST_CHECK(strstr(adaptive_tune_zones[1].last_refusal_reason, "implausible") != NULL,
               "refusal reason should say the fit was implausible");
}

static void test_dirty_run_is_never_training_data(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    // H1 fix: otherwise-perfect, well-spread observations, but with true K ==
    // 12 (NOT == prior 10) -- deliberately a MATERIAL move (blend = 10 +
    // 0.15*(12-10) = 10.3, a 3% move, well above ADAPTIVE_TUNE_MIN_MATERIAL_
    // MOVE_FRAC's 0.5% floor). The original fixture here used true K == prior
    // K == 10.0 exactly, so the blend was a zero move and F1's material-
    // change floor refused it for the WRONG reason -- masking the excluded-
    // fraction guard this test exists to cover (see the review's H1 finding:
    // with the excluded guard disabled entirely, that pre-existing fixture
    // still passed, 134/134 GREEN). With a genuinely material blend, only
    // the "not clean" / "excluded fraction" guards below can be the reason
    // has_applied stays false.
    feed_settled_dwell(1, 22.0f + 12.0f * 0.2f, 22.0f, 0.2f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 12.0f * 0.5f, 22.0f, 0.5f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 12.0f * 0.8f, 22.0f, 0.8f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(1, 22.0f + 12.0f * 0.35f, 22.0f, 0.35f, SETTLE_TICKS, DT_S);

    profile_firing_run_record_t rec = make_clean_record(5, 1, 900);
    adaptive_tune_run_end(&rec, /*clean=*/false); // faulted or operator-stopped
    TEST_CHECK(!adaptive_tune_zones[1].has_applied, "a faulted/stopped-early run must never be used as training data");
    TEST_CHECK(strstr(adaptive_tune_zones[1].last_refusal_reason, "faulted or stopped early") != NULL,
               "refusal reason should say why (not-clean run)");

    // Same observations, this time reported clean but with heavy sensor
    // dropout (>5% excluded) -- must also refuse.
    profile_firing_run_record_t rec2 = make_clean_record(6, 1, 900);
    rec2.zones[1].stats.excluded_sample_count = 100; // 100/(900+100) = 10% > 5%
    adaptive_tune_run_end(&rec2, /*clean=*/true);
    TEST_CHECK(!adaptive_tune_zones[1].has_applied, "heavy excluded-sample fraction must also refuse to learn");
    TEST_CHECK(strstr(adaptive_tune_zones[1].last_refusal_reason, "excluded") != NULL,
               "H1: refusal reason should name the excluded-sample-fraction guard, not the material-change floor "
               "-- proves this fixture's blend was genuinely material and the excluded guard is what's under test");
}

static void test_refinement_improves_gain_estimate_on_known_plant(void)
{
    reset_module_state();
    // Asymmetric fixture, deliberately DIFFERENT numbers on two zones so a
    // transposed zone index would fail this test: zone 1's prior undershoots
    // its true gain, zone 3's prior overshoots ITS true gain, and the two
    // zones' numbers are all distinct from each other.
    adaptive_tune_zones[1].enabled = true;
    adaptive_tune_zones[2].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;   // prior, zone 1 -- true gain 15
    s_fake_zone_cfg[2].k_dc = 30.0f;   // prior, zone 3 -- true gain 20
    const float true_k1 = 15.0f, true_k3 = 20.0f, ambient = 22.3f;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(1, ambient + true_k1 * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
        feed_settled_dwell(2, ambient + true_k3 * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
    }

    profile_firing_run_record_t rec = make_clean_record(9, 1, 900);
    rec.zones[2].active = true;
    rec.zones[2].stats.sample_count = 900;
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(adaptive_tune_zones[1].has_applied, "zone 1 should have applied a refinement");
    TEST_CHECK(adaptive_tune_zones[2].has_applied, "zone 3 should have applied a refinement");

    // Blend: prior + ALPHA*(fit-prior), fit==true_k here (exact, no noise
    // beyond 0.1 degC quantization). Zone 1 moves UP toward 15, zone 3 moves
    // DOWN toward 20 -- opposite directions, so a swapped index is obvious.
    float expect_k1 = 10.0f + 0.15f * (true_k1 - 10.0f);   // 10.75
    float expect_k3 = 30.0f + 0.15f * (true_k3 - 30.0f);   // 28.5
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].k_dc, expect_k1, 0.05, "zone 1's refined K_dc should move toward its OWN true gain");
    TEST_CHECK_NEAR(s_fake_zone_cfg[2].k_dc, expect_k3, 0.05, "zone 3's refined K_dc should move toward its OWN true gain");

    // The refined estimate must be strictly CLOSER to the true gain than
    // the prior was -- the actual "improves the estimate" property, not
    // just "changed to some new number".
    TEST_CHECK(fabsf(s_fake_zone_cfg[1].k_dc - true_k1) < fabsf(10.0f - true_k1),
               "zone 1's refined estimate must be closer to the true gain than the prior was");
    TEST_CHECK(fabsf(s_fake_zone_cfg[2].k_dc - true_k3) < fabsf(30.0f - true_k3),
               "zone 3's refined estimate must be closer to the true gain than the prior was");

    // PID gains must also have moved (SIMC recomputed from the new K) --
    // not just the model triple.
    TEST_CHECK(s_fake_zone_cfg[1].kp > 0.0f, "zone 1 should have gotten nonzero recomputed PID gains");
}

static void test_per_run_move_is_bounded_even_with_many_dwells(void)
{
    reset_module_state();
    adaptive_tune_zones[2].enabled = true;
    s_fake_zone_cfg[2].k_dc = 10.0f;
    // true gain 45 is 4.5x the prior -- inside ADAPTIVE_TUNE_MAX_JUMP_RATIO
    // (5x, so the implausible-jump guard does not refuse it) but far enough
    // that ALPHA*(fit-prior) = 0.15*35 = 5.25 would blow past the per-run
    // cap (10*0.20 = 2.0) if nothing clamped it -- this scenario genuinely
    // exercises the cap, unlike a small move that never reaches it.
    const float true_k = 45.0f, ambient = 21.7f;
    const float duties[4] = {0.15f, 0.45f, 0.75f, 0.30f};
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(2, ambient + true_k * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
    }
    profile_firing_run_record_t rec = make_clean_record(10, 2, 900);
    adaptive_tune_run_end(&rec, true);
    float max_move = 10.0f * ADAPTIVE_TUNE_MAX_FRACTIONAL_MOVE; // 2.0
    TEST_CHECK(adaptive_tune_zones[2].has_applied, "setup: the 4.5x fit must pass the jump-ratio guard and apply");
    TEST_CHECK_NEAR(s_fake_zone_cfg[2].k_dc, 10.0f + max_move, 0.05,
                     "a single run's applied move must be clamped exactly at the per-run fractional cap");
    TEST_CHECK(s_fake_zone_cfg[2].k_dc <= 10.0f + max_move + 1e-3f,
               "a single run's applied move must never exceed the per-run fractional cap");
}

// docs/audits/adaptive_tune_vs_owner_requirements_2026-09-11.md: THE
// regression test the pre-fix code could not pass. Before this pass, the
// plausibility ratio test read the LIVE model_k_dc -- the exact value THIS
// function is the only thing that ever moves once a zone is opted in -- so a
// fit could be refused as ">5x last week's value" while being many times the
// ORIGINAL autotune measurement, as long as each intervening accepted run's
// own move stayed under 5x its own immediately-prior value. No single run's
// guard was ever wrong in isolation; composed across many accepted runs they
// placed NO ceiling on the total drift.
//
// This test feeds, every run, a fit chosen relative to whatever K_dc is
// CURRENTLY live (fit = live * 4.9 -- always just inside the OLD, live-
// relative ratio guard, so the pre-fix code never refuses it on plausibility
// grounds) and checks after every run that K_dc has not left
// [baseline/ADAPTIVE_TUNE_MAX_JUMP_RATIO, baseline*ADAPTIVE_TUNE_MAX_JUMP_
// RATIO] -- the fixed, provable lifetime envelope this fix's own comment
// (adaptive_tune_model.c, just above the ratio check) derives by induction
// once the ratio test is anchored to the FIXED baseline instead.
//
// NEGATIVE TEST: reverting adaptive_tune_model.c's ratio-test anchor back to
// k_dc (instead of baseline_k_dc) makes this test fail within a handful of
// runs -- each accepted run's own live reference grows ~1.2x (the per-run
// +-20% cap saturating every time, since fit=live*4.9 always overshoots that
// cap), so live compounds roughly like baseline*1.2^run and crosses
// baseline*5 well before run 8. Confirmed by hand: see this task's commit
// message for the restore-and-diff proof.
static void test_repeated_accepted_refinements_stay_within_baseline_envelope(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    const float baseline = 10.0f;
    s_fake_zone_cfg[1].k_dc = baseline; // this IS the original autotune's own result --
                                         // no separate autotune_baseline_k_dc set yet, so
                                         // adaptive_tune_refine_zone_locked() must bootstrap
                                         // it from this value on its first call below.
    const float ambient = 22.0f;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};
    const float lower_bound = baseline / ADAPTIVE_TUNE_MAX_JUMP_RATIO;
    const float upper_bound = baseline * ADAPTIVE_TUNE_MAX_JUMP_RATIO;

    for (int run = 0; run < 10; run++) {
        // Fresh ring each run -- a clean fit to exactly ONE target value,
        // never a blend of this run's and a prior run's differing targets.
        adaptive_tune_zones[1].ring_count = 0;
        adaptive_tune_zones[1].ring_head = 0;
        // Just inside the OLD (live-relative) ratio guard every single run --
        // this is what let the pre-fix code accept every one of these as
        // "plausible", never refusing on that basis.
        float fit = s_fake_zone_cfg[1].k_dc * 4.9f;
        for (int i = 0; i < 4; i++) {
            feed_settled_dwell(1, ambient + fit * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
        }
        profile_firing_run_record_t rec = make_clean_record((uint32_t)(200 + run), 1, 900);
        adaptive_tune_run_end(&rec, true);

        TEST_CHECK(s_fake_zone_cfg[1].k_dc <= upper_bound + 1e-3f,
                   "K_dc must never exceed the ORIGINAL autotune baseline's x5 lifetime envelope, "
                   "however many accepted refinements have run");
        TEST_CHECK(s_fake_zone_cfg[1].k_dc >= lower_bound - 1e-3f,
                   "K_dc must never fall below the ORIGINAL autotune baseline's /5 lifetime envelope, "
                   "however many accepted refinements have run");
    }

    TEST_CHECK(s_fake_zone_cfg[1].autotune_baseline_k_dc == baseline,
               "adaptive_tune's own refinements must NEVER move the baseline anchor itself -- only a fresh "
               "full autotune Accept (autotune_engine_guard.c) may do that");
}

// ADVERSARIAL review 2026-09-11 (opus): the commit that introduced the
// baseline anchor above rests on an induction argument -- that anchoring ONLY
// the plausibility ratio test to a fixed baseline bounds K_dc for all time,
// even though the blend target AND its own per-run +-20% move cap still read
// the LIVE k_dc. The regression test above exercises exactly one observation
// sequence (monotonically increasing, fit = live * 4.9 every run), which is
// also the sequence that argument is most obviously true for. These three
// tests attack the same argument from the directions that test actually
// leaves open:
//
//   (a) DOWNWARD. The upper clamp is the one the existing test saturates;
//       the lower clamp (k_dc - 0.2*k_dc) has entirely different arithmetic,
//       and a /5 lower envelope is a MULTIPLICATIVE floor, not an additive
//       one. Drive every run at the smallest fit the ratio check will accept.
//   (b) ALTERNATING. A sequence that whipsaws between the top and the bottom
//       of the accepted envelope on successive runs -- if any single step's
//       clamp could ever overshoot the interval (rather than only ever moving
//       TOWARD k_dc, as the induction claims), an alternating drive is the
//       shape that exposes it, because it puts k_fit as far from k_dc as the
//       ratio test allows on every single run, in both directions.
//   (c) MAXIMUM WALK. Every run, pick whichever of the two ratio-test extremes
//       (baseline/5 or baseline*5) is FURTHER from the current live k_dc --
//       i.e. an adversary who, knowing the whole implementation, picks the fit
//       that moves K_dc as far as it can be moved this run, for 200 runs.
//       This is the strongest attack on "the bound holds for arbitrary
//       accepted sequences, not just convergent ones".
//
// All three assert the SAME fixed envelope the induction claims, after every
// single run. 200 runs is far past the ~9 at which the pre-fix, live-relative
// anchor blows the bound (see the regression test's own comment).
static void refine_once_with_fit(uint8_t zi, float fit, float ambient, uint32_t run_id)
{
    adaptive_tune_zones[zi].ring_count = 0;
    adaptive_tune_zones[zi].ring_head = 0;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(zi, ambient + fit * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
    }
    profile_firing_run_record_t rec = make_clean_record(run_id, zi, 900);
    adaptive_tune_run_end(&rec, true);
}

static void test_adversarial_refinement_sequences_stay_within_baseline_envelope(void)
{
    const float baseline = 10.0f;
    const float ambient = 22.0f;
    const float lower_bound = baseline / ADAPTIVE_TUNE_MAX_JUMP_RATIO;
    const float upper_bound = baseline * ADAPTIVE_TUNE_MAX_JUMP_RATIO;
    // 1e-3 absolute, matching the regression test above: float arithmetic on
    // values of order 10-50, not a tolerance wide enough to hide a real
    // escape (the pre-fix code overshoots by whole multiples, not epsilons).
    const float eps = 1e-3f;

    // (a) DOWNWARD: every run drives at the lowest fit the fixed ratio test
    // will accept. K_dc must approach, and never pass, baseline/5.
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = baseline;
    for (int run = 0; run < 200; run++) {
        refine_once_with_fit(1, lower_bound, ambient, (uint32_t)(1000 + run));
        TEST_CHECK(s_fake_zone_cfg[1].k_dc >= lower_bound - eps,
                   "downward-driven refinements must never fall below the fixed baseline/5 envelope");
        TEST_CHECK(s_fake_zone_cfg[1].k_dc <= upper_bound + eps,
                   "downward-driven refinements must never exceed the fixed baseline*5 envelope");
    }
    TEST_CHECK(s_fake_zone_cfg[1].autotune_baseline_k_dc == baseline,
               "a downward-driven sequence must not move the baseline anchor either");

    // (b) ALTERNATING: whipsaw between the two extremes of the accepted
    // envelope on successive runs.
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = baseline;
    for (int run = 0; run < 200; run++) {
        float fit = (run % 2 == 0) ? upper_bound : lower_bound;
        refine_once_with_fit(1, fit, ambient, (uint32_t)(3000 + run));
        TEST_CHECK(s_fake_zone_cfg[1].k_dc >= lower_bound - eps,
                   "alternating-extreme refinements must never fall below the fixed baseline/5 envelope");
        TEST_CHECK(s_fake_zone_cfg[1].k_dc <= upper_bound + eps,
                   "alternating-extreme refinements must never exceed the fixed baseline*5 envelope");
    }

    // (c) MAXIMUM WALK: an adversary picking, every run, whichever accepted
    // extreme is further from the live k_dc.
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = baseline;
    for (int run = 0; run < 200; run++) {
        float live = s_fake_zone_cfg[1].k_dc;
        float fit = (upper_bound - live) >= (live - lower_bound) ? upper_bound : lower_bound;
        refine_once_with_fit(1, fit, ambient, (uint32_t)(5000 + run));
        TEST_CHECK(s_fake_zone_cfg[1].k_dc >= lower_bound - eps,
                   "a maximum-displacement adversarial sequence must never fall below baseline/5");
        TEST_CHECK(s_fake_zone_cfg[1].k_dc <= upper_bound + eps,
                   "a maximum-displacement adversarial sequence must never exceed baseline*5");
    }
    TEST_CHECK(s_fake_zone_cfg[1].autotune_baseline_k_dc == baseline,
               "200 adversarially-chosen accepted refinements must still leave the baseline anchor untouched");
}

// ---------------------------------------------------------------------
// Full coupled identification -- adaptive_tune_coupled_fit() pure-math tests.
// ---------------------------------------------------------------------

// The bench-measured reference matrix from PID_EXPANSION_PLAN.md section 2,
// converted from its WIRE form [stepped][affected] (as quoted there) to the
// STORAGE convention adaptive_tune_coupled_fit() and zones_config_get/set_
// coupling() both use: C[affected][stepped]. Deliberately asymmetric
// (C[0][1]=26.61 != C[1][0]=15.78, etc.) so a transposed solver recovers the
// WRONG numbers, not just "numbers" -- see test_coupled_fit_recovers_known_
// asymmetric_matrix() below.
//   wire[stepped=0] = 39.25/15.78/9.70  -> C[0][0]=39.25 C[1][0]=15.78 C[2][0]=9.70
//   wire[stepped=1] = 26.61/31.97/11.38 -> C[0][1]=26.61 C[1][1]=31.97 C[2][1]=11.38
//   wire[stepped=2] = 20.73/21.09/31.68 -> C[0][2]=20.73 C[1][2]=21.09 C[2][2]=31.68
static const float k_ref_C[3][3] = {
    {39.25f, 26.61f, 20.73f},
    {15.78f, 31.97f, 21.09f},
    {9.70f, 11.38f, 31.68f},
};
