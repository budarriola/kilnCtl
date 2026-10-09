// Split out of test_adaptive_tune.c (2026-09-01, kept the file under this repo's
// 1500-line guidance). Coupled identification: the pure-fit guards (orientation,
// conditioning, plausibility), per-cell blending and its abs/ratio caps, the joint
// observation floor, and the coupled-apply storage orientation.
// #included from test_adaptive_tune.c AFTER its fakes and test helpers -- see
// test_adaptive_tune_dwell.c's header comment for the shared-fixture convention.
// Pure refactor: no test removed, no assertion changed, no comment dropped.

static void test_coupled_fit_refuses_underdetermined_observation_set(void)
{
    // n=3 unknowns per row, ADAPTIVE_TUNE_COUPLED_OBS_MARGIN=2 -> minimum is
    // 5 joint observations. Feed exactly 4 (n+1) -- one short -- with
    // otherwise perfectly well-conditioned, exactly-on-model data (so the
    // ONLY thing that can refuse this is the observation-count guard, not a
    // conditioning problem).
    float duty[4][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f}, {0.35f, 0.65f, 0.15f}};
    float rise[4][MAX31856_CHANNEL_COUNT];
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 3; i++) {
            float s = 0.0f;
            for (int j = 0; j < 3; j++) s += k_ref_C[i][j] * duty[k][j];
            rise[k][i] = q1(s);
        }
    }
    float out_C[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
    adaptive_tune_coupled_result_t r = adaptive_tune_coupled_fit(duty, rise, 4, 3, out_C);
    TEST_CHECK(r == ADAPTIVE_TUNE_COUPLED_TOO_FEW_OBSERVATIONS,
               "4 joint observations (n+1) for a 3-unknown-per-row system must refuse as underdetermined");

    // Sanity: the SAME data with one more observation (n+2 == 5, the
    // documented margin) must NOT refuse for this reason -- proves the guard
    // is checking the count, not silently failing on this data for some
    // other reason.
    float duty5[5][MAX31856_CHANNEL_COUNT];
    memcpy(duty5, duty, sizeof(duty));
    duty5[4][0] = 0.65f; duty5[4][1] = 0.15f; duty5[4][2] = 0.65f;
    float rise5[5][MAX31856_CHANNEL_COUNT];
    memcpy(rise5, rise, sizeof(rise));
    for (int i = 0; i < 3; i++) {
        float s = 0.0f;
        for (int j = 0; j < 3; j++) s += k_ref_C[i][j] * duty5[4][j];
        rise5[4][i] = q1(s);
    }
    adaptive_tune_coupled_result_t r2 = adaptive_tune_coupled_fit(duty5, rise5, 5, 3, out_C);
    TEST_CHECK(r2 == ADAPTIVE_TUNE_COUPLED_OK, "setup: 5 observations (the documented margin) must be accepted");
}

static void test_coupled_fit_refuses_ill_conditioned_observations(void)
{
    // 6 observations, but zone 0's and zone 1's duty are IDENTICAL on every
    // one -- the design matrix (duty^T * duty) is then singular (columns 0
    // and 1 are linearly dependent), however many observations are piled
    // on. Well above the observation-count floor, so this isolates the
    // conditioning guard specifically.
    float duty[6][MAX31856_CHANNEL_COUNT];
    float rise[6][MAX31856_CHANNEL_COUNT];
    float xs[6] = {0.20f, 0.35f, 0.50f, 0.65f, 0.80f, 0.30f};
    float ys[6] = {0.10f, 0.40f, 0.25f, 0.55f, 0.15f, 0.60f};
    for (int k = 0; k < 6; k++) {
        duty[k][0] = xs[k];
        duty[k][1] = xs[k]; // == column 0, always
        duty[k][2] = ys[k];
        for (int i = 0; i < 3; i++) {
            float s = 0.0f;
            for (int j = 0; j < 3; j++) s += k_ref_C[i][j] * duty[k][j];
            rise[k][i] = q1(s);
        }
    }
    float out_C[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
    adaptive_tune_coupled_result_t r = adaptive_tune_coupled_fit(duty, rise, 6, 3, out_C);
    TEST_CHECK(r == ADAPTIVE_TUNE_COUPLED_ILL_CONDITIONED,
               "two identical duty columns (rank-deficient design matrix) must refuse as ill-conditioned, "
               "not silently return a numeric answer");
}

static void test_coupled_fit_recovers_known_asymmetric_matrix(void)
{
    // 6 joint observations, duty combinations spanning a wide, varied range
    // (well above the 5-observation margin, well-conditioned), rises
    // generated from k_ref_C and quantized to 0.1 degC -- realistically
    // quantized synthetic input, not idealized exact floats (this repo's own
    // "unquantized synthetic input hides whole branches" trap).
    float duty[6][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f},
        {0.35f, 0.65f, 0.15f}, {0.65f, 0.15f, 0.65f}, {0.15f, 0.35f, 0.35f}};
    float rise[6][MAX31856_CHANNEL_COUNT];
    for (int k = 0; k < 6; k++) {
        for (int i = 0; i < 3; i++) {
            float s = 0.0f;
            for (int j = 0; j < 3; j++) s += k_ref_C[i][j] * duty[k][j];
            rise[k][i] = q1(s);
        }
    }
    float out_C[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
    adaptive_tune_coupled_result_t r = adaptive_tune_coupled_fit(duty, rise, 6, 3, out_C);
    TEST_CHECK(r == ADAPTIVE_TUNE_COUPLED_OK, "setup: 6 well-spread observations on a well-conditioned matrix must solve");

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            TEST_CHECK_NEAR(out_C[i][j], k_ref_C[i][j], 0.6,
                             "recovered coupling_coeff[affected][stepped] cell must match the known matrix "
                             "within quantization tolerance");
        }
    }
    // The orientation check that actually catches a transpose bug: C[0][1]
    // (26.61) and C[1][0] (15.78) are far enough apart that a transposed
    // solver would fail BOTH of the checks above by more than 10x this
    // tolerance, not pass by coincidence.
    TEST_CHECK(fabsf(out_C[0][1] - out_C[1][0]) > 5.0f,
               "setup: C[0][1] and C[1][0] must be genuinely different values in the fixture, or a "
               "transpose bug could not be distinguished from a correct solve");
    TEST_CHECK_NEAR(out_C[0][1], 26.61f, 0.6, "C[0][1] (affected=0 responding to stepped=1) must NOT read as C[1][0]'s value");
    TEST_CHECK_NEAR(out_C[1][0], 15.78f, 0.6, "C[1][0] (affected=1 responding to stepped=0) must NOT read as C[0][1]'s value");
}

// The same well-spread, well-conditioned duty set every test above uses --
// factored out so the three plausibility tests below only vary the TARGET
// matrix, never the observation geometry, isolating what each test claims
// to isolate.
static void coupled_fit_generate(const float target_C[3][3], float duty_out[6][MAX31856_CHANNEL_COUNT],
                                  float rise_out[6][MAX31856_CHANNEL_COUNT])
{
    static const float duty[6][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f},
        {0.35f, 0.65f, 0.15f}, {0.65f, 0.15f, 0.65f}, {0.15f, 0.35f, 0.35f}};
    memcpy(duty_out, duty, sizeof(duty));
    for (int k = 0; k < 6; k++) {
        for (int i = 0; i < 3; i++) {
            float s = 0.0f;
            for (int j = 0; j < 3; j++) s += target_C[i][j] * duty[k][j];
            rise_out[k][i] = q1(s);
        }
    }
}

// Plausibility gate, negative-coefficient branch: a target matrix that is
// otherwise well-conditioned (same duty geometry as the accepted fit above)
// but has ONE negative off-diagonal entry (row 0's response to zone 1's
// duty) -- mirrors the real 3-observation/3-unknown capture cited in
// PID_EXPANSION_PLAN.md that produced a perfect-residual interpolation with
// three negative entries. This MUST be refused as IMPLAUSIBLE_MATRIX, not
// silently accepted (a negative coupling coefficient means "heating this
// zone cools that one", physically impossible in this kiln) and not
// conflated with ILL_CONDITIONED (this data is NOT ill-conditioned -- the
// conditioning gate alone would pass it, which is exactly the bug this
// gate exists to close).
static const float k_negative_C[3][3] = {
    {39.25f, -5.0f, 20.73f},
    {15.78f, 31.97f, 21.09f},
    {9.70f, 11.38f, 31.68f},
};

static void test_coupled_fit_refuses_negative_coefficient(void)
{
    float duty[6][MAX31856_CHANNEL_COUNT], rise[6][MAX31856_CHANNEL_COUNT];
    coupled_fit_generate(k_negative_C, duty, rise);
    float out_C[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
    adaptive_tune_coupled_result_t r = adaptive_tune_coupled_fit(duty, rise, 6, 3, out_C);
    TEST_CHECK(r == ADAPTIVE_TUNE_COUPLED_IMPLAUSIBLE_MATRIX,
               "a fitted matrix with a negative off-diagonal coefficient must be refused as physically "
               "implausible, even though this observation set is well-conditioned");
}

// Plausibility gate, diagonal-dominance branch: mirrors the REAL row-1
// collapse measured on the 26-observation plant_sim fixture set -- row 1's
// own (diagonal) coefficient down at 0.8 against a real ~32, with a 38 in
// an off-diagonal cell instead. Same well-conditioned duty geometry as
// every other test in this section (condition number is not the problem
// here, by construction), so this isolates the dominance check
// specifically, in the SAME [affected][stepped] storage orientation the
// firmware actually evaluates it in (row 1 == affected zone 1, i.e.
// C[1][1] vs C[1][0]/C[1][2] -- not the transposed wire form).
static const float k_nondominant_C[3][3] = {
    {39.25f, 26.61f, 20.73f},
    {38.0f, 0.8f, 21.09f}, // row 1 (affected=1): own coefficient (0.8) is NOT the row max (38.0 is)
    {9.70f, 11.38f, 31.68f},
};

static void test_coupled_fit_refuses_nondominant_diagonal(void)
{
    float duty[6][MAX31856_CHANNEL_COUNT], rise[6][MAX31856_CHANNEL_COUNT];
    coupled_fit_generate(k_nondominant_C, duty, rise);
    float out_C[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
    adaptive_tune_coupled_result_t r = adaptive_tune_coupled_fit(duty, rise, 6, 3, out_C);
    TEST_CHECK(r == ADAPTIVE_TUNE_COUPLED_IMPLAUSIBLE_MATRIX,
               "a fitted row whose own-zone coefficient is not its row's largest entry (own=0.8 vs "
               "neighbor=38.0, the real row-1 collapse shape) must be refused as physically implausible");
}

// Plausibility gate must not reject a LEGITIMATE fit: the same bench-
// measured asymmetric reference matrix (k_ref_C) test_coupled_fit_recovers_
// known_asymmetric_matrix() above already proves is recovered correctly --
// re-asserted here as its own named test so a future change to the
// plausibility gate that starts rejecting good matrices fails a test whose
// NAME says exactly what broke, not a numeric-recovery test with no
// apparent connection to plausibility.
static void test_coupled_fit_accepts_plausible_matrix(void)
{
    float duty[6][MAX31856_CHANNEL_COUNT], rise[6][MAX31856_CHANNEL_COUNT];
    coupled_fit_generate(k_ref_C, duty, rise);
    float out_C[MAX31856_CHANNEL_COUNT][MAX31856_CHANNEL_COUNT];
    adaptive_tune_coupled_result_t r = adaptive_tune_coupled_fit(duty, rise, 6, 3, out_C);
    TEST_CHECK(r == ADAPTIVE_TUNE_COUPLED_OK,
               "the bench-measured reference matrix (all-positive, every row diagonal-dominant) must be "
               "accepted -- the plausibility gate must not reject a legitimate fit");
}

// ---------------------------------------------------------------------
// Integral (Ki) diagnosis -- adaptive_tune_diagnose_ki() pure-math tests.
// 10s ticks (DT_KI), matching the hardware logging cadence this repo's
// vacuity-trap note calls out; every temperature sample below goes through
// q1() (0.1 degC quantization), same convention as the rest of this file.
// ---------------------------------------------------------------------
#define DT_KI 10.0f

// D1: a coupling cell must converge to truth across repeated runs from a
// 0.0 prior, not freeze partway. MUST FAIL on the pre-D1-fix code (that red
// was captured before applying the fix).
// ---------------------------------------------------------------------
static void test_coupling_cell_converges_from_zero_prior_over_repeated_runs(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 1.0f; // irrelevant to this test -- just needs to be nonzero/positive
    const float ambient = 20.0f;
    const float duty_pts[5][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f},
        {0.35f, 0.65f, 0.15f}, {0.65f, 0.15f, 0.65f}};
    for (int k = 0; k < 5; k++) {
        float target[MAX31856_CHANNEL_COUNT];
        for (int i = 0; i < 3; i++) {
            float rise = 0.0f;
            for (int j = 0; j < 3; j++) rise += k_ref_C[i][j] * duty_pts[k][j];
            target[i] = ambient + rise;
        }
        feed_joint_settled_dwell(target, ambient, duty_pts[k], SETTLE_TICKS, DT_S);
    }
    TEST_CHECK(adaptive_tune_joint_ring_count == 5, "setup: 5 distinct joint dwells queued");

    // 30 runs against the SAME fixed evidence -- a stand-in for 30 firings
    // that all measured the same true coupling, exercising exactly the
    // iterated-blend convergence path the review calls out.
    for (int run = 0; run < 30; run++) {
        profile_firing_run_record_t rec = make_clean_record(20, 0, 900);
        adaptive_tune_run_end(&rec, true);
    }

    TEST_CHECK_NEAR(s_fake_coupling[0][1], k_ref_C[0][1], 1.0,
                     "coupling cell [0][1] must converge to truth across repeated runs from a 0.0 prior");
    TEST_CHECK_NEAR(s_fake_coupling[0][2], k_ref_C[0][2], 1.0,
                     "coupling cell [0][2] must converge to truth across repeated runs from a 0.0 prior");
}

// F5/D3: ADAPTIVE_TUNE_COUPLING_MAX_ABS_MOVE must actually bind a single
// run's move -- zero coverage previously let it silently revert 6.0 -> 10.0
// (or anything else) with 109/109 still green. Prior is confident-but-low
// (2.0, so the near-zero branch is NOT the one exercised) and the fit is
// far enough away that the uncapped 15% blend would clear the cap
// comfortably.
// ---------------------------------------------------------------------
static void test_coupling_cell_per_run_move_is_bounded_by_abs_cap(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 1.0f;
    s_fake_coupling[0][1] = 1.0f; // confident prior (> NEAR_ZERO); uncapped blend would be
                                  // 1.0 + 0.15*(45.0-1.0) = 7.6, well past the 6.0 cap
    const float ambient = 20.0f;
    // Row 0's own (diagonal) coefficient is raised to 50.0, above the 45.0
    // fitted off-diagonal being clamp-tested -- otherwise this fixture would
    // itself be a non-diagonal-dominant matrix and get refused by the
    // physical-plausibility gate before ever reaching the per-run abs-move
    // cap this test exists to exercise. Only c[0][0] moved; c[0][1]=45.0 is
    // unchanged since that fitted value is what the test asserts on.
    static const float k_test_C[3][3] = {
        {50.0f, 45.0f, 20.73f}, {15.78f, 31.97f, 21.09f}, {9.70f, 11.38f, 31.68f}};
    const float duty_pts[6][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f},
        {0.35f, 0.65f, 0.15f}, {0.65f, 0.15f, 0.65f}, {0.15f, 0.35f, 0.35f}};
    for (int k = 0; k < 6; k++) {
        float target[MAX31856_CHANNEL_COUNT];
        for (int i = 0; i < 3; i++) {
            float rise = 0.0f;
            for (int j = 0; j < 3; j++) rise += k_test_C[i][j] * duty_pts[k][j];
            target[i] = ambient + rise;
        }
        feed_joint_settled_dwell(target, ambient, duty_pts[k], SETTLE_TICKS, DT_S);
    }
    profile_firing_run_record_t rec = make_clean_record(90, 0, 900);
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(adaptive_tune_zones[0].coupled_applied, "setup: the coupled solve must have applied");
    TEST_CHECK_NEAR(s_fake_coupling[0][1], 1.0f + ADAPTIVE_TUNE_COUPLING_MAX_ABS_MOVE, 0.3,
                     "D3: a single run's coupling-cell move must be clamped exactly at the per-run absolute cap");
    TEST_CHECK(s_fake_coupling[0][1] <= 1.0f + ADAPTIVE_TUNE_COUPLING_MAX_ABS_MOVE + 1e-3f,
               "D3: a single run's coupling-cell move must never exceed the per-run absolute cap");
}

// A4 review follow-up B (2026-09-28): zones_config_get_coupling() always
// masks an on/off zone's COLUMN to 0.0 (docs/ON_OFF_ZONE.md sec 1), but
// that 0.0 is a live-control-loop mask, not "no prior on record" -- the real
// coefficient measured before the zone was retyped is still sitting in flash.
// Before this fix, adaptive_tune_refine_coupled_locked() read that masked 0.0
// as prior_row[j], took the near-zero blend branch, and permanently
// overwrote the real stored cell with 0.15*fit. The fix skips any column
// whose zone is on/off outright, so the stored cell must be completely
// untouched by an adaptive update, not merely "close to its old value" --
// this test seeds a real, nonzero coefficient in the on/off column and
// checks it for byte-for-byte survival.
static void test_coupled_refine_skips_on_off_column_leaves_stored_cell_untouched(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 1.0f;
    s_stub_zone_is_on_off[1] = true; // zone 1 (column under test) is on/off
    const float real_prior_before_retype = 17.25f;
    s_fake_coupling[0][1] = real_prior_before_retype; // real cell measured while zone 1 was still a heater
    const float ambient = 20.0f;
    const float duty_pts[5][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f},
        {0.35f, 0.65f, 0.15f}, {0.65f, 0.15f, 0.65f}};
    for (int k = 0; k < 5; k++) {
        float target[MAX31856_CHANNEL_COUNT];
        for (int i = 0; i < 3; i++) {
            float rise = 0.0f;
            for (int j = 0; j < 3; j++) rise += k_ref_C[i][j] * duty_pts[k][j];
            target[i] = ambient + rise;
        }
        feed_joint_settled_dwell(target, ambient, duty_pts[k], SETTLE_TICKS, DT_S);
    }
    TEST_CHECK(adaptive_tune_joint_ring_count == 5, "setup: 5 distinct joint dwells queued");

    for (int run = 0; run < 30; run++) {
        profile_firing_run_record_t rec = make_clean_record(20, 0, 900);
        adaptive_tune_run_end(&rec, true);
    }

    TEST_CHECK(s_fake_coupling[0][1] == real_prior_before_retype,
               "an on/off column's stored coupling cell survives an adaptive update untouched");
    // Column 2 (a heater, not on/off) still refines normally -- proves the
    // skip is scoped to the on/off column and not a wider regression.
    TEST_CHECK_NEAR(s_fake_coupling[0][2], k_ref_C[0][2], 1.0,
                     "a non-on/off column in the same row still converges normally");
}

// docs/SPARE_RELAY_ONOFF_PLAN.md sec 10: a monitor-only zone (HEATER, no
// heater relay) is masked by zones_config_get_coupling() exactly like an
// on/off zone, so its COLUMN must be skipped the same way -- same seeded-cell,
// byte-for-byte survival check as the on/off test above.
static void test_coupled_refine_skips_monitor_only_column_leaves_stored_cell_untouched(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 1.0f;
    s_stub_zone_is_monitor_only[1] = true; // zone 1 (column under test) lost its heater relay
    const float real_prior_before_convert = 17.25f;
    s_fake_coupling[0][1] = real_prior_before_convert;
    const float ambient = 20.0f;
    const float duty_pts[5][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f},
        {0.35f, 0.65f, 0.15f}, {0.65f, 0.15f, 0.65f}};
    for (int k = 0; k < 5; k++) {
        float target[MAX31856_CHANNEL_COUNT];
        for (int i = 0; i < 3; i++) {
            float rise = 0.0f;
            for (int j = 0; j < 3; j++) rise += k_ref_C[i][j] * duty_pts[k][j];
            target[i] = ambient + rise;
        }
        feed_joint_settled_dwell(target, ambient, duty_pts[k], SETTLE_TICKS, DT_S);
    }
    for (int run = 0; run < 30; run++) {
        profile_firing_run_record_t rec = make_clean_record(20, 0, 900);
        adaptive_tune_run_end(&rec, true);
    }
    TEST_CHECK(s_fake_coupling[0][1] == real_prior_before_convert,
               "a monitor-only column's stored coupling cell survives an adaptive update untouched");
    TEST_CHECK_NEAR(s_fake_coupling[0][2], k_ref_C[0][2], 1.0,
                     "a driven-heater column in the same row still converges normally");
}

// Same rule, other axis: a monitor-only zone's OWN row reads all-zero through
// the masking getter. If run_end refined it anyway, the near-zero-prior blend
// would write 0.15*fit over every real stored cell, and its K_dc would move on
// evidence from a run that never drove it. adaptive_tune_run_end() must skip
// the zone outright (no row cell, no K_dc change).
static void test_run_end_skips_monitor_only_zone_row_and_model_untouched(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 1.0f;
    s_stub_zone_is_monitor_only[0] = true;
    s_fake_coupling[0][1] = 17.25f;
    s_fake_coupling[0][2] = 9.5f;
    const float ambient = 20.0f;
    const float duty_pts[5][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f},
        {0.35f, 0.65f, 0.15f}, {0.65f, 0.15f, 0.65f}};
    for (int k = 0; k < 5; k++) {
        float target[MAX31856_CHANNEL_COUNT];
        for (int i = 0; i < 3; i++) {
            float rise = 0.0f;
            for (int j = 0; j < 3; j++) rise += k_ref_C[i][j] * duty_pts[k][j];
            target[i] = ambient + rise;
        }
        feed_joint_settled_dwell(target, ambient, duty_pts[k], SETTLE_TICKS, DT_S);
    }
    for (int run = 0; run < 30; run++) {
        profile_firing_run_record_t rec = make_clean_record(20, 0, 900);
        adaptive_tune_run_end(&rec, true);
    }
    TEST_CHECK(s_fake_coupling[0][1] == 17.25f && s_fake_coupling[0][2] == 9.5f,
               "a monitor-only zone's own stored coupling row survives run_end untouched");
    TEST_CHECK(s_fake_zone_cfg[0].k_dc == 1.0f, "a monitor-only zone's K_dc is not refined");
}

// F5/D1: zero coverage of either direction of the ratio guard's upper
// bound (upper = max(prior*5, ADAPTIVE_TUNE_COUPLING_IMPLAUSIBLE_ABS)).
// Accept direction: a confident-but-low prior must still accept a fit far
// past its own 5x ratio, as long as it is within the absolute plausibility
// ceiling -- otherwise a cell blended up from a near-zero prior could never
// converge past ~5.3x its own early blend step.
// ---------------------------------------------------------------------
static void test_coupling_ratio_guard_upper_bound_accepts_high_fit_from_low_confident_prior(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 1.0f;
    s_fake_coupling[0][1] = 2.0f; // confident prior -- a plain 5x ratio ceiling alone would be 10.0
    const float ambient = 20.0f;
    // c[0][0] raised to 55.0 (above the 49.0 fitted off-diagonal) for the
    // same reason as test_coupling_cell_per_run_move_is_bounded_by_abs_cap()
    // above: a non-diagonal-dominant fixture would be refused by the
    // physical-plausibility gate before this test's ratio-guard logic ever
    // runs.
    static const float k_test_C[3][3] = {
        {55.0f, 49.0f, 20.73f}, {15.78f, 31.97f, 21.09f}, {9.70f, 11.38f, 31.68f}};
    const float duty_pts[6][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f},
        {0.35f, 0.65f, 0.15f}, {0.65f, 0.15f, 0.65f}, {0.15f, 0.35f, 0.35f}};
    for (int k = 0; k < 6; k++) {
        float target[MAX31856_CHANNEL_COUNT];
        for (int i = 0; i < 3; i++) {
            float rise = 0.0f;
            for (int j = 0; j < 3; j++) rise += k_test_C[i][j] * duty_pts[k][j];
            target[i] = ambient + rise;
        }
        feed_joint_settled_dwell(target, ambient, duty_pts[k], SETTLE_TICKS, DT_S);
    }
    profile_firing_run_record_t rec = make_clean_record(91, 0, 900);
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(s_fake_coupling[0][1] > 2.0f, "D1 (accept direction): a fit of ~49 against a confident-but-low "
                                              "prior of 2.0 (49 >> 2*5) must still move the cell UP, not be "
                                              "refused for exceeding a plain 5x ratio ceiling");
}

// F5/D1, reject direction: the widened upper bound is not unlimited -- a
// fit above the absolute plausibility ceiling must still be refused.
static void test_coupling_ratio_guard_upper_bound_rejects_fit_above_absolute_ceiling(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 1.0f;
    s_fake_coupling[0][1] = 2.0f;
    const float ambient = 20.0f;
    static const float k_test_C[3][3] = {
        {39.25f, 55.0f, 20.73f}, {15.78f, 31.97f, 21.09f}, {9.70f, 11.38f, 31.68f}};
    const float duty_pts[6][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f},
        {0.35f, 0.65f, 0.15f}, {0.65f, 0.15f, 0.65f}, {0.15f, 0.35f, 0.35f}};
    for (int k = 0; k < 6; k++) {
        float target[MAX31856_CHANNEL_COUNT];
        for (int i = 0; i < 3; i++) {
            float rise = 0.0f;
            for (int j = 0; j < 3; j++) rise += k_test_C[i][j] * duty_pts[k][j];
            target[i] = ambient + rise;
        }
        feed_joint_settled_dwell(target, ambient, duty_pts[k], SETTLE_TICKS, DT_S);
    }
    profile_firing_run_record_t rec = make_clean_record(92, 0, 900);
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(s_fake_coupling[0][1] == 2.0f, "D1 (reject direction): a fit of 55 (above the 50.0 absolute "
                                               "plausibility ceiling) against a confident prior must be refused, "
                                               "not accepted -- the widened upper bound is not unlimited");
}

// D4: the joint-observation floor must count DISTINCT dwells, not rows --
// N simultaneously-enabled zones settling on the SAME dwell must contribute
// exactly ONE joint row, not N near-identical ones.
// ---------------------------------------------------------------------
static void test_joint_floor_counts_distinct_dwells_not_rows(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    adaptive_tune_zones[1].enabled = true;
    adaptive_tune_zones[2].enabled = true;
    const float ambient = 20.0f;
    const float duty_pts[2][MAX31856_CHANNEL_COUNT] = {{0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}};
    for (int k = 0; k < 2; k++) {
        float target[MAX31856_CHANNEL_COUNT];
        for (int i = 0; i < 3; i++) {
            float rise = 0.0f;
            for (int j = 0; j < 3; j++) rise += k_ref_C[i][j] * duty_pts[k][j];
            target[i] = ambient + rise;
        }
        feed_joint_settled_dwell(target, ambient, duty_pts[k], SETTLE_TICKS, DT_S);
    }
    // The regression this guards: with 3 zones enabled and 2 distinct
    // dwells, a per-zone (rather than per-dwell) commit would leave 6 rows
    // in the ring here, not 2 -- silently clearing the 5-observation floor
    // with only 2 real operating points for 3 unknowns.
    TEST_CHECK(adaptive_tune_joint_ring_count == 2, "2 distinct dwells with 3 zones enabled must commit exactly 2 joint rows, "
                                         "not one per zone");

    profile_firing_run_record_t rec = make_clean_record(21, 0, 900);
    rec.zones[1].active = true;
    rec.zones[1].stats.sample_count = 900;
    rec.zones[2].active = true;
    rec.zones[2].stats.sample_count = 900;
    adaptive_tune_run_end(&rec, true);
    TEST_CHECK(!adaptive_tune_zones[0].coupled_applied, "2 distinct dwells (below the 5-observation margin) must refuse the "
                                             "coupled solve, not silently accept an underdetermined fit");
    // F4: the check above passes even under the REVERTED D4 gate (per-zone
    // rather than per-dwell counting), because 3 zones x 2 dwells = 6
    // near-duplicate rows are then singular (identical columns) and get
    // refused as ILL_CONDITIONED anyway -- a DIFFERENT refusal path than
    // the one this test is meant to guard. Pin the actual refusal reason so
    // a D4 regression (which would still leave coupled_applied false, but
    // for the wrong reason) is caught here instead of silently passing.
    TEST_CHECK(strstr(adaptive_tune_zones[0].coupled_refusal_reason, "joint dwell observations") != NULL,
               "the coupled solve must refuse specifically for TOO FEW joint observations (2 distinct dwells "
               "against the 5-observation floor) -- a D4 regression to per-zone counting would instead leave 6 "
               "near-duplicate rows that clear the floor and refuse as ill-conditioned instead, which the plain "
               "!coupled_applied check above cannot tell apart from this");
}

// D6: adaptive_tune_refine_coupled_locked()'s OWN indexing (reading C[zi][j] and
// calling set_coupling_cell(zi, j, ...)) has no coverage from the pure-math
// adaptive_tune_coupled_fit() tests above -- a transpose at either call
// site would still pass all of them. This drives the full apply path (via
// adaptive_tune_run_end()) against the same asymmetric bench matrix and
// checks the fake coupling TABLE lands cells in the storage orientation
// (coupling_coeff[affected][stepped]), not swapped.
// ---------------------------------------------------------------------
static void test_coupled_apply_writes_cells_in_storage_orientation(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    adaptive_tune_zones[1].enabled = true;
    const float ambient = 20.0f;
    const float duty_pts[6][MAX31856_CHANNEL_COUNT] = {
        {0.20f, 0.50f, 0.80f}, {0.50f, 0.80f, 0.20f}, {0.80f, 0.20f, 0.50f},
        {0.35f, 0.65f, 0.15f}, {0.65f, 0.15f, 0.65f}, {0.15f, 0.35f, 0.35f}};
    for (int k = 0; k < 6; k++) {
        float target[MAX31856_CHANNEL_COUNT];
        for (int i = 0; i < 3; i++) {
            float rise = 0.0f;
            for (int j = 0; j < 3; j++) rise += k_ref_C[i][j] * duty_pts[k][j];
            target[i] = ambient + rise;
        }
        feed_joint_settled_dwell(target, ambient, duty_pts[k], SETTLE_TICKS, DT_S);
    }
    TEST_CHECK(adaptive_tune_joint_ring_count == 6, "setup: 6 distinct joint dwells queued");

    profile_firing_run_record_t rec = make_clean_record(22, 0, 900);
    rec.zones[1].active = true;
    rec.zones[1].stats.sample_count = 900;
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(adaptive_tune_zones[0].coupled_applied, "setup: zone 0's coupled solve must have applied");
    TEST_CHECK(adaptive_tune_zones[1].coupled_applied, "setup: zone 1's coupled solve must have applied");

    // One run's 15% blend from a 0.0 prior: expect ~= ALPHA * truth for
    // each cell. [0][1] (26.61) and [1][0] (15.78) are far enough apart
    // (their blended values differ by > 1.0) that a transposed read of
    // C[][] or a transposed set_coupling_cell() call would land the WRONG
    // number in the fake table, not just a slightly-off one.
    float expect_01 = ADAPTIVE_TUNE_COUPLING_BLEND_ALPHA * k_ref_C[0][1];
    float expect_10 = ADAPTIVE_TUNE_COUPLING_BLEND_ALPHA * k_ref_C[1][0];
    TEST_CHECK(fabsf(expect_01 - expect_10) > 1.0f,
               "setup: expected [0][1] and [1][0] blended values must be genuinely different, or a transpose bug "
               "could not be distinguished from a correct apply");
    TEST_CHECK_NEAR(s_fake_coupling[0][1], expect_01, 0.3,
                     "fake_coupling[0][1] (affected=0 responding to stepped=1) must NOT read as [1][0]'s value");
    TEST_CHECK_NEAR(s_fake_coupling[1][0], expect_10, 0.3,
                     "fake_coupling[1][0] (affected=1 responding to stepped=0) must NOT read as [0][1]'s value");
}
