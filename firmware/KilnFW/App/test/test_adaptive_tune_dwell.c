// Split out of test_adaptive_tune.c (2026-09-01, kept the file under this repo's
// 1500-line guidance). Dwell harvesting: the settle criterion (temperature slope
// AND duty stability, not slope alone) and joint-row dwell-entry bookkeeping.
// #included from test_adaptive_tune.c AFTER its fakes and test helpers (q1(),
// feed_settled_dwell(), feed_unsettled_dwell(), feed_flat_temp_oscillating_duty_
// dwell(), reset_module_state(), DT_S/SETTLE_TICKS) -- same one-TU convention as
// adaptive_tune.c's own #include of adaptive_tune_model.c/adaptive_tune_ki.c, so
// none of those shared fixtures are duplicated here. Pure refactor: no test
// removed, no assertion changed, no comment dropped.

static void test_unsettled_dwell_is_not_recorded(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    // Climbs 0.05 degC/tick over 30s ticks == 0.00167 C/s -- wait, that IS
    // below the floor. Use a climb clearly above SETTLE_SLOPE_FLOOR
    // (0.003 C/s): 0.2 degC/tick / 30s = 0.0067 C/s, more than 2x the floor.
    feed_unsettled_dwell(0, 100.0f, 22.0f, 0.5f, SETTLE_TICKS, DT_S, 0.2f);
    TEST_CHECK(adaptive_tune_zones[0].ring_count == 0, "a dwell that never stops drifting must record nothing");
}

static void test_settled_dwell_is_recorded(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    feed_settled_dwell(0, 100.0f, 22.0f, 0.5f, SETTLE_TICKS, DT_S);
    TEST_CHECK(adaptive_tune_zones[0].ring_count == 1, "a genuinely flat, settled dwell must record exactly one observation");
}

// 2026-09-01: reproduces the REAL defect shape measured against the coupid6
// hardware capture -- zone 0's duty inside the 46 C dwell went
// 0.023 -> 0.19 -> 0.144 while actual_c held flat (this is exactly the
// under-damped-plant failure mode ADAPTIVE_TUNE_DUTY_STABILITY_ABS/FRAC
// exists to catch, not an idealized synthetic swing). Temperature is fed
// perfectly flat (slope == 0, well under the floor) across all
// SETTLE_TICKS*DT_S == 210s > ADAPTIVE_TUNE_SETTLE_MIN_S, and every value is
// 0.1C-quantized like the rest of this file's fixtures -- so the ONLY thing
// standing between this fixture and a recorded observation is the duty-
// stability check. MUST fail (go red) if that check is removed: with only
// the slope test, this dwell settles trivially (flat temperature) on the
// very first eligible tick.
static void test_oscillating_duty_flat_temperature_is_refused(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    // Real coupid6 zone-0/46C trajectory, extended to SETTLE_TICKS (7)
    // samples: the measured 0.023 -> 0.19 -> 0.144 sequence, then continuing
    // to oscillate around ~0.15 rather than settling -- duty never comes to
    // rest within the window, same as measured on hardware.
    const float duties[SETTLE_TICKS] = {0.023f, 0.19f, 0.144f, 0.10f, 0.17f, 0.12f, 0.16f};
    feed_flat_temp_oscillating_duty_dwell(0, 46.0f, 22.0f, duties, SETTLE_TICKS, DT_S);
    TEST_CHECK(adaptive_tune_zones[0].ring_count == 0,
               "a flat-temperature dwell whose duty is still swinging ~89% of its own value must NOT be "
               "recorded as a DC-gain observation, even though the (defective) temperature-only slope test "
               "alone would have accepted it immediately");
    TEST_CHECK(strstr(adaptive_tune_zones[0].last_refusal_reason, "duty") != NULL &&
               strstr(adaptive_tune_zones[0].last_refusal_reason, "oscillat") != NULL,
               "the refusal must reach the existing refusal-reason surface with a DISTINCT string naming duty "
               "instability, not a generic/blank reason indistinguishable from 'no data yet'");
}

// The companion positive case: duty genuinely at rest (a tiny, realistic
// ripple well inside both ADAPTIVE_TUNE_DUTY_STABILITY_ABS and _FRAC) must
// still be accepted -- proves the new check does not simply refuse every
// dwell regardless of content.
static void test_genuinely_steady_duty_is_still_accepted(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    // Settled duty ~0.50, ripple of +/-0.01 (2% of value, well under the 25%
    // fractional floor and the 0.05 absolute floor) -- what a real PID loop
    // actually at steady state looks like, not a mathematically exact
    // constant.
    const float duties[SETTLE_TICKS] = {0.50f, 0.49f, 0.51f, 0.50f, 0.49f, 0.51f, 0.50f};
    feed_flat_temp_oscillating_duty_dwell(0, 100.0f, 22.0f, duties, SETTLE_TICKS, DT_S);
    TEST_CHECK(adaptive_tune_zones[0].ring_count == 1,
               "a genuinely steady dwell (small realistic duty ripple) must still be recorded -- the "
               "duty-stability check must not reject everything indiscriminately");
}

// F3: dwelling_prev (and everything gated on it -- the settle window,
// trace reset, and adaptive_tune_joint_dwell_row_committed) must reflect whether a zone
// is PHYSICALLY dwelling, independent of whether any given tick's data
// happens to be trustworthy or whether the zone happens to be opted in.
// Both sub-cases below share one root cause: the old code only ever
// touched dwelling_prev from inside branches gated on enabled/actual_valid,
// so a zone that never passed those gates during its real dwell-entry
// tick(s) never recorded having entered at all.
// ---------------------------------------------------------------------

// F3(a): the data-validity half. MUST FAIL on pre-F3-fix code -- dwelling_
// prev stays false after a dwelling tick with bad data, instead of tracking
// the raw `dwelling` state.
static void test_dwelling_prev_tracks_dwelling_state_even_when_data_is_invalid(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    adaptive_tune_zone_tick(0, 100.0f, /*actual_valid=*/false, 0.5f, /*dwelling=*/true, 22.0f, DT_S);
    TEST_CHECK(adaptive_tune_zones[0].dwelling_prev == true,
               "a zone whose FIRST dwelling tick has bad data is still physically dwelling -- dwelling_prev must "
               "reflect that immediately, not silently stay false until the first VALID tick");

    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    adaptive_tune_zone_tick(0, 100.0f, true, 0.5f, true, NAN, DT_S); // NaN ambient, same claim
    TEST_CHECK(adaptive_tune_zones[0].dwelling_prev == true,
               "a zone whose FIRST dwelling tick has a NaN ambient reading is still physically dwelling -- "
               "dwelling_prev must reflect that immediately");
}

// F3(b): the opt-in half, and the end-to-end observable consequence of the
// same root cause -- a zone opted into adaptive tuning MID-DWELL must not
// be misread as having just entered a fresh dwell, or it wrongly reopens
// (and lets something re-commit into) an already-committed joint row from
// THIS SAME physical dwell. MUST FAIL on pre-F3-fix code: adaptive_tune_
// joint_ring_count ends at 2, not 1, for one physical dwell.
static void test_enabling_zone_mid_dwell_does_not_reopen_committed_joint_row(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = false; // starts opted OUT
    adaptive_tune_zones[1].enabled = true;
    adaptive_tune_zones[2].enabled = true;
    const float ambient = 20.0f;
    const float duty[MAX31856_CHANNEL_COUNT] = {0.5f, 0.5f, 0.5f};

    // Zone 0 is disabled but ticking with perfectly good data throughout --
    // a disabled zone still contributes its duty as a joint-cache NEIGHBOUR
    // column (see adaptive_tune_joint_last_duty[]'s own comment), it just never runs
    // its OWN settle/ring bookkeeping while opted out.
    adaptive_tune_zone_tick(0, q1(25.0f), true, duty[0], true, ambient, DT_S);
    adaptive_tune_zone_tick(1, q1(25.0f), true, duty[1], false, ambient, DT_S);
    adaptive_tune_zone_tick(2, q1(25.0f), true, duty[2], false, ambient, DT_S);
    for (int i = 0; i < SETTLE_TICKS; i++) {
        adaptive_tune_zone_tick(0, q1(25.0f), true, duty[0], true, ambient, DT_S);
        adaptive_tune_zone_tick(1, q1(25.0f), true, duty[1], true, ambient, DT_S);
        adaptive_tune_zone_tick(2, q1(25.0f), true, duty[2], true, ambient, DT_S);
    }
    TEST_CHECK(adaptive_tune_joint_ring_count == 1, "setup: zones 1/2 settling with zone 0 disabled-but-valid must commit "
                                         "exactly one joint row");

    // Zone 0 gets opted IN mid-dwell -- still the SAME physical dwell (it
    // was never disabled from the joint cache's perspective, only from its
    // own adaptive-tune bookkeeping).
    adaptive_tune_zones[0].enabled = true;
    for (int i = 0; i < SETTLE_TICKS; i++) {
        adaptive_tune_zone_tick(0, q1(25.0f), true, duty[0], true, ambient, DT_S);
        adaptive_tune_zone_tick(1, q1(25.0f), true, duty[1], true, ambient, DT_S);
        adaptive_tune_zone_tick(2, q1(25.0f), true, duty[2], true, ambient, DT_S);
    }
    TEST_CHECK(adaptive_tune_joint_ring_count == 1, "F3(b): opting a zone into adaptive tuning mid-dwell must NOT be misread "
                                         "as that zone entering a fresh dwell -- it must not reopen and re-commit "
                                         "an already-committed joint row from this same physical dwell");
}
