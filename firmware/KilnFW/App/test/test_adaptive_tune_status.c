// Split out of test_adaptive_tune.c (2026-09-01, kept the file under this repo's
// 1500-line guidance). Opt-in gating and the public accessor surface (same one
// adaptive_tune_http.c calls), run_end() skip paths and the status-field
// clearing they must leave behind, the U2 opt-in-flag migration out of the old
// NVS namespace, and U1 one-click revert.
// #included from test_adaptive_tune.c AFTER its fakes and test helpers -- see
// test_adaptive_tune_dwell.c's header comment for the shared-fixture convention.
// Pure refactor: no test removed, no assertion changed, no comment dropped.

static void test_opt_in_default_off_records_nothing(void)
{
    reset_module_state();
    // Deliberately NOT setting adaptive_tune_zones[0].enabled -- struct-zero default,
    // same as adaptive_tune_init() would leave an unconfigured zone.
    TEST_CHECK(adaptive_tune_zones[0].enabled == false, "opt-in must default to OFF");
    feed_settled_dwell(0, 100.0f, 22.0f, 0.5f, SETTLE_TICKS, DT_S);
    TEST_CHECK(adaptive_tune_zones[0].ring_count == 0, "a settled dwell on a zone that never opted in must record nothing");

    profile_firing_run_record_t rec = make_clean_record(1, 0, 900);
    float k_before = s_fake_zone_cfg[0].k_dc;
    adaptive_tune_run_end(&rec, true);
    TEST_CHECK(s_fake_zone_cfg[0].k_dc == k_before, "run_end on an opted-out zone must never touch its model");
    TEST_CHECK(!adaptive_tune_zones[0].has_applied, "an opted-out zone must never report an applied refinement");
}

// P3: disabling the `!z->enabled` skip check at adaptive_tune_run_end()
// leaves 250/250 green, because the pre-existing opt-in test (test_opt_in_
// default_off_records_nothing() above) only ever exercises the situation
// where a disabled zone's ring is EMPTY -- that alone already explains the
// refusal via the observation-count floor, with or without the enabled
// check at run_end. This test seeds a full, well-spread ring WHILE enabled
// (as an operator's genuinely-recorded run would look), then opts the zone
// OUT before calling run_end -- there is now more than enough data to clear
// every OTHER guard, so a refusal here can only be explained by the opt-in
// check itself.
// ---------------------------------------------------------------------
static void test_run_end_skips_disabled_zone_even_with_ring_data_present(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 10.0f;
    // True gain == prior (10.0) with real spread, so nothing else about this
    // fit is refusable -- if the run_end opt-in check did not exist, this
    // would apply cleanly.
    feed_settled_dwell(0, 22.0f + 10.0f * 0.30f, 22.0f, 0.30f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.0f * 0.50f, 22.0f, 0.50f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.0f * 0.70f, 22.0f, 0.70f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.0f * 0.90f, 22.0f, 0.90f, SETTLE_TICKS, DT_S);
    TEST_CHECK(adaptive_tune_zones[0].ring_count >= ADAPTIVE_TUNE_MIN_OBSERVATIONS,
               "setup: enough dwell observations queued to clear the observation-count floor");

    adaptive_tune_zones[0].enabled = false; // opted out AFTER the data was recorded, before run_end

    profile_firing_run_record_t rec = make_clean_record(5, 0, 900);
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(!adaptive_tune_zones[0].has_applied,
               "P3: a zone opted out before run_end must never apply, even with a full, well-spread ring");
    TEST_CHECK(s_fake_zone_cfg[0].k_dc == 10.0f, "P3: the model must be untouched when opted out before run_end");
    TEST_CHECK(strstr(adaptive_tune_zones[0].last_refusal_reason, "not opted into") != NULL,
               "P3: the refusal must name the opt-in gate specifically, not an observation-count/spread guard "
               "that would also explain a refusal on its own -- MUST go red by mutation if adaptive_tune_run_"
               "end()'s `!z->enabled` skip check is removed");
}

// on/off zone gap closed 2026-09-14 (docs/audits/on_off_zone_decisions_2026-
// 09-14.md sec 2, item 1): an on/off zone has no PID and no model, so
// adaptive_tune must never train on one even if it is opted in, active in
// the profile's zone mask, and carries a full, well-spread ring -- the same
// shape as test_run_end_skips_disabled_zone_even_with_ring_data_present()
// above but with the on/off flag as the ONLY thing standing between this
// data and a refinement. MUST go red by mutation if adaptive_tune_run_end()'s
// zone_is_on_off() skip check is removed.
static void test_run_end_skips_on_off_zone_even_with_ring_data_present(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 10.0f;
    feed_settled_dwell(0, 22.0f + 10.0f * 0.30f, 22.0f, 0.30f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.0f * 0.50f, 22.0f, 0.50f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.0f * 0.70f, 22.0f, 0.70f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.0f * 0.90f, 22.0f, 0.90f, SETTLE_TICKS, DT_S);
    TEST_CHECK(adaptive_tune_zones[0].ring_count >= ADAPTIVE_TUNE_MIN_OBSERVATIONS,
               "setup: enough dwell observations queued to clear the observation-count floor");

    s_stub_zone_is_on_off[0] = true; // the only thing this test changes vs. the enabled/active case

    profile_firing_run_record_t rec = make_clean_record(6, 0, 900);
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(!adaptive_tune_zones[0].has_applied,
               "an on/off zone must never apply a refinement, even with a full, well-spread ring");
    TEST_CHECK(s_fake_zone_cfg[0].k_dc == 10.0f, "the model must be untouched for an on/off zone");
    TEST_CHECK(strstr(adaptive_tune_zones[0].last_refusal_reason, "on/off") != NULL,
               "the refusal must name the on/off gate specifically, not the opt-in or active-mask guards "
               "that would also explain a refusal on their own");
}

// Public accessor tests -- these exercise EXACTLY the surface
// adaptive_tune_http.c's status/enable handlers call (adaptive_tune_get_
// enabled/set_enabled/get_status), not the internal adaptive_tune_zones struct
// directly, so they prove the accessor path itself, not just the module's
// internal state.
// ---------------------------------------------------------------------

static void test_default_off_for_every_zone(void)
{
    reset_module_state();
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        TEST_CHECK(adaptive_tune_get_enabled(zi) == false,
                   "every zone's opt-in must default to OFF, via the public getter");
    }
}

static void test_enable_one_zone_leaves_others_untouched(void)
{
    // Asymmetric fixture: enable ONLY zone 1, then assert zones 0 and 2
    // (neighbours on either side) stay off -- a mask off-by-one or a
    // transposed index would flip one of those two, not zone 1 itself.
    // U2: adaptive_tune_set_enabled()'s return value now reflects zones_
    // config_set_adaptive_tune_enabled()'s own success (the fake above),
    // not this module's own NVS namespace -- no nvs_test_enable() needed
    // for this path any more.
    reset_module_state();
    TEST_CHECK(adaptive_tune_set_enabled(1, true), "enabling zone 1 should report success");
    TEST_CHECK(adaptive_tune_get_enabled(0) == false, "zone 0 must stay off when only zone 1 is enabled");
    TEST_CHECK(adaptive_tune_get_enabled(1) == true, "zone 1 must be on");
    TEST_CHECK(adaptive_tune_get_enabled(2) == false, "zone 2 must stay off when only zone 1 is enabled");
}

static void test_enable_round_trips_through_persistence(void)
{
    // U2: persistence now lives in the zone config blob (s_fake_adaptive_
    // enabled here), not this module's own RAM -- so "reboot" means wiping
    // adaptive_tune_zones (this module's RAM cache) WITHOUT touching s_fake_
    // adaptive_enabled (the persisted store), then calling adaptive_tune_
    // load_enable_flags() (not adaptive_tune_init(), which no longer loads
    // this at all -- see its own doc comment) to reload the cache from the
    // persisted store, exactly as main.c does after zones_http_start().
    reset_module_state();
    TEST_CHECK(adaptive_tune_set_enabled(1, true), "setting zone 1's opt-in should report success");

    memset(adaptive_tune_zones, 0, sizeof(adaptive_tune_zones)); // simulate a reboot: RAM cache gone,
                                                                  // persisted store (s_fake_adaptive_enabled)
                                                                  // untouched
    adaptive_tune_load_enable_flags();

    TEST_CHECK(adaptive_tune_get_enabled(0) == false, "zone 0 must reload as off");
    TEST_CHECK(adaptive_tune_get_enabled(1) == true, "zone 1 must reload as on -- this is the persisted value");
    TEST_CHECK(adaptive_tune_get_enabled(2) == false, "zone 2 must reload as off");
}

static void test_status_reports_engine_held_fields_not_test_written_values(void)
{
    // Drives a real refinement through adaptive_tune_run_end() (same as
    // test_refinement_improves_gain_estimate_on_known_plant()) and then reads
    // it back ONLY through adaptive_tune_get_status() -- the same accessor
    // adaptive_tune_http.c's status handler calls. Every field checked here
    // comes from the engine's own bookkeeping (z->ring_count, z->has_applied,
    // z->prior_k_dc, z->applied_k_dc, ...), never a value this test wrote
    // into the status struct itself.
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f; // prior -- true gain 15
    const float true_k = 15.0f, ambient = 22.3f;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(1, ambient + true_k * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
    }

    adaptive_tune_zone_status_t before;
    adaptive_tune_get_status(1, &before);
    TEST_CHECK(before.enabled == true, "status must reflect this zone's opt-in");
    TEST_CHECK(before.ring_count == 4, "status must report the 4 dwell observations collected so far");
    TEST_CHECK(before.has_applied == false, "no refinement has run yet -- has_applied must still be false");

    profile_firing_run_record_t rec = make_clean_record(7, 1, 900);
    adaptive_tune_run_end(&rec, true);

    adaptive_tune_zone_status_t after;
    adaptive_tune_get_status(1, &after);
    TEST_CHECK(after.has_applied == true, "status must report the refinement as applied");
    TEST_CHECK_NEAR(after.prior_k_dc, 10.0f, 1e-4, "status's prior_k_dc must be the model K_dc before this apply");
    TEST_CHECK(after.applied_k_dc > after.prior_k_dc,
               "status's applied_k_dc must show the new (higher, toward true gain 15) K_dc");
    TEST_CHECK(after.last_delta_pct > 0.0f, "status's last_delta_pct must be positive (K_dc moved up)");
    TEST_CHECK(after.last_applied_profile_id == 7, "status must report which profile produced the applied change");
    TEST_CHECK(after.last_refusal_reason[0] == '\0', "a clean apply must leave the refusal reason empty");
}

// H2/K1/K2/K3: adaptive_tune_run_end()'s skip paths (faulted, excluded-
// fraction, and -- K1's own finding -- disabled/inactive-zone too) must
// reset every genuinely PER-RUN status field (ki_applied/ki_verdict/
// ki_correction_pct/coupled_applied/coupled_cells_changed) to a neutral
// state, exactly as H2 originally intended. But has_applied (and its
// prior_k_dc/applied_k_dc/last_delta_pct/last_applied_profile_id siblings)
// is a documented LIFETIME LATCH (adaptive_tune.h), not a per-run field --
// zones_page.html's "Last applied change" column reads it that way -- so a
// skip path must NOT clear it (K2: H2's original fix did, which is a UI
// regression: a faulted run after a real applied refinement would make the
// UI revert to "no refinement applied yet" even though the applied change
// is still exactly what is live on the zone).
//
// To make BOTH claims load-bearing (K2/K3), this fixture drives THREE
// clean-then-faulted runs so that, immediately before the faulted run:
//   - has_applied is true (a genuine model refine applied in run 1)
//   - coupled_applied is true and coupled_cells_changed > 0 (a genuine
//     coupled solve applied in run 1, from independent joint-ring data)
//   - ki_applied is true (a genuine Ki diagnosis applied in run 2)
// so the faulted run's assertions distinguish "correctly preserved" from
// "trivially already false/absent" the way the pre-K2/K3 versions of this
// test could not (deleting the has_applied clear, or the coupled_* fields
// entirely, left 218/218 GREEN because neither was ever actually
// established first).
// ---------------------------------------------------------------------
static void test_run_status_fields_cleared_on_faulted_run(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].ki = 1.0f;

    // Run 1: a genuine diagonal model refine (has_applied=true, K2's target)
    // AND a genuine coupled solve (coupled_applied=true, K3's target) --
    // independent ring buffers, so both legitimately fire in the same run.
    const float true_k1 = 15.0f, ambient = 22.0f;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(1, ambient + true_k1 * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
    }
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
    profile_firing_run_record_t rec1 = make_clean_record(90, 1, 900);
    adaptive_tune_run_end(&rec1, true);
    TEST_CHECK(adaptive_tune_zones[1].has_applied, "setup: run 1 must have genuinely applied a model refine");
    TEST_CHECK(adaptive_tune_zones[1].coupled_applied, "setup: run 1 must have genuinely applied a coupled solve");
    TEST_CHECK(adaptive_tune_zones[1].coupled_cells_changed > 0, "setup: run 1's coupled solve must have changed >0 cells");

    // Force the NEXT diagonal fit to be refused as implausible (jump-ratio
    // guard) rather than simply re-converging -- ALPHA=0.15 leaves run 1's
    // blend far from material-move-floor territory, so without this the
    // same static ring data would keep firing adaptive_tune_refine_zone_locked() for
    // many more runs (see test_per_run_move_is_bounded_even_with_many_
    // dwells()'s own numbers), which would starve the Ki diagnosis via D5
    // and make it impossible to also establish ki_applied=true within a
    // reasonable number of runs. Simulates "the model was independently
    // changed" (e.g. a hand edit) between runs -- a legitimate real scenario,
    // not just a test convenience.
    // docs/audits/adaptive_tune_vs_owner_requirements_2026-09-11.md: the
    // implausible-jump guard now checks the fit against autotune_baseline_
    // k_dc (fixed at the last full autotune Accept), not the live k_dc this
    // module itself moves -- so forcing the refusal here means moving BOTH,
    // exactly what a real "run a fresh autotune, then hand-edit before the
    // next firing" sequence would do to this zone's persisted config.
    s_fake_zone_cfg[1].k_dc = 1000.0f;
    s_fake_zone_cfg[1].autotune_baseline_k_dc = 1000.0f;

    // Run 2: clean, model refine now refuses (implausible jump), which
    // frees the Ki diagnosis (D5) to genuinely apply this run.
    feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
    profile_firing_run_record_t rec2 = make_clean_record(91, 1, 900);
    rec2.zones[1].stats.dwell_err_mean_c = 0.45f;
    rec2.zones[1].stats.dwell_err_max_c = 0.50f;
    adaptive_tune_run_end(&rec2, true);
    TEST_CHECK(adaptive_tune_zones[1].has_applied, "setup: has_applied must still be true (a latch) after run 2");
    // K9 (docs/audits/simc_sole_gain_writer_2026-09-14.md): adaptive_tune_ki.c
    // is diagnostic-only now -- ki_applied is always false. What this setup
    // actually needs is a genuine, non-default Ki DIAGNOSIS this run (proving
    // D5 really did hand the Ki layer its turn), which the reset-on-skip
    // assertions below (run 3) then prove get cleared -- not that anything
    // was ever applied.
    TEST_CHECK(!adaptive_tune_zones[1].ki_applied, "K9: adaptive_tune_ki.c never applies a correction any more");
    TEST_CHECK(adaptive_tune_zones[1].ki_verdict == (uint8_t)ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL,
               "setup: run 2's Ki verdict must be real (OFFSET_TOO_SMALL), not a default");
    TEST_CHECK(adaptive_tune_zones[1].ki_correction_pct > 0.0f, "setup: run 2's Ki correction pct must be real and nonzero");
    TEST_CHECK(adaptive_tune_zones[1].coupled_applied, "setup: run 2's coupled solve must still be applying (same persisted "
                                             "joint ring, still far from converged)");

    // Run 3: same zone, faulted.
    profile_firing_run_record_t rec3 = make_clean_record(92, 1, 900);
    adaptive_tune_run_end(&rec3, /*clean=*/false);

    TEST_CHECK(adaptive_tune_zones[1].has_applied,
               "K2: a faulted run must NOT clear has_applied -- it is a lifetime latch, and the UI's \"Last "
               "applied change\" column reads it that way");
    TEST_CHECK(!adaptive_tune_zones[1].ki_applied, "H2: a faulted run must not keep publishing a PREVIOUS run's ki_applied=1");
    TEST_CHECK(adaptive_tune_zones[1].ki_verdict == (uint8_t)ADAPTIVE_TUNE_KI_INSUFFICIENT,
               "H2: a faulted run must reset ki_verdict, not keep publishing OFFSET_TOO_SMALL from run 2");
    TEST_CHECK(adaptive_tune_zones[1].ki_correction_pct == 0.0f,
               "H2: a faulted run must reset ki_correction_pct, not keep publishing run 2's +20%%");
    TEST_CHECK(!adaptive_tune_zones[1].coupled_applied,
               "K3: a faulted run must not report coupled_applied from a previous run (genuinely established true "
               "in runs 1/2, not trivially false)");
    TEST_CHECK(adaptive_tune_zones[1].coupled_cells_changed == 0,
               "K3: a faulted run must reset coupled_cells_changed, not keep publishing a previous run's count "
               "(genuinely established >0 in run 1, not trivially zero)");
    TEST_CHECK(strstr(adaptive_tune_zones[1].ki_refusal_reason, "faulted or stopped early") != NULL,
               "H2: the Ki refusal reason must say why THIS run produced no diagnosis");
}

// H2/K1/K2/K3, excluded-fraction path -- same defects, different guard.
// Same three-run structure as test_run_status_fields_cleared_on_faulted_
// run() above, for the same reasons.
static void test_run_status_fields_cleared_on_excluded_fraction_refusal(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].ki = 1.0f;

    const float true_k1 = 15.0f, ambient = 22.0f;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(1, ambient + true_k1 * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
    }
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
    profile_firing_run_record_t rec1 = make_clean_record(93, 1, 900);
    adaptive_tune_run_end(&rec1, true);
    TEST_CHECK(adaptive_tune_zones[1].has_applied, "setup: run 1 must have genuinely applied a model refine");
    TEST_CHECK(adaptive_tune_zones[1].coupled_applied, "setup: run 1 must have genuinely applied a coupled solve");
    TEST_CHECK(adaptive_tune_zones[1].coupled_cells_changed > 0, "setup: run 1's coupled solve must have changed >0 cells");

    // docs/audits/adaptive_tune_vs_owner_requirements_2026-09-11.md: the
    // implausible-jump guard now checks the fit against autotune_baseline_
    // k_dc (fixed at the last full autotune Accept), not the live k_dc this
    // module itself moves -- so forcing the refusal here means moving BOTH,
    // exactly what a real "run a fresh autotune, then hand-edit before the
    // next firing" sequence would do to this zone's persisted config.
    s_fake_zone_cfg[1].k_dc = 1000.0f;
    s_fake_zone_cfg[1].autotune_baseline_k_dc = 1000.0f; // see faulted-run test's own comment on this line

    feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
    profile_firing_run_record_t rec2 = make_clean_record(94, 1, 900);
    rec2.zones[1].stats.dwell_err_mean_c = 0.45f;
    rec2.zones[1].stats.dwell_err_max_c = 0.50f;
    adaptive_tune_run_end(&rec2, true);
    // K9: diagnostic-only now -- see the faulted-run test's identical comment above.
    TEST_CHECK(!adaptive_tune_zones[1].ki_applied, "K9: adaptive_tune_ki.c never applies a correction any more");
    TEST_CHECK(adaptive_tune_zones[1].ki_verdict == (uint8_t)ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL,
               "setup: run 2's Ki verdict must be real (OFFSET_TOO_SMALL), not a default");
    TEST_CHECK(adaptive_tune_zones[1].coupled_applied, "setup: run 2's coupled solve must still be applying");

    profile_firing_run_record_t rec3 = make_clean_record(95, 1, 900);
    rec3.zones[1].stats.excluded_sample_count = 100; // 100/(900+100) = 10% > 5%
    adaptive_tune_run_end(&rec3, /*clean=*/true);

    TEST_CHECK(adaptive_tune_zones[1].has_applied,
               "K2: an excluded-fraction refusal must NOT clear has_applied -- it is a lifetime latch");
    TEST_CHECK(!adaptive_tune_zones[1].ki_applied,
               "H2: an excluded-fraction refusal must not keep publishing a PREVIOUS run's ki_applied=1");
    TEST_CHECK(adaptive_tune_zones[1].ki_verdict == (uint8_t)ADAPTIVE_TUNE_KI_INSUFFICIENT,
               "H2: an excluded-fraction refusal must reset ki_verdict");
    TEST_CHECK(adaptive_tune_zones[1].ki_correction_pct == 0.0f,
               "H2: an excluded-fraction refusal must reset ki_correction_pct");
    TEST_CHECK(!adaptive_tune_zones[1].coupled_applied,
               "K3: an excluded-fraction refusal must not report a stale coupled_applied (genuinely established "
               "true beforehand, not trivially false)");
    TEST_CHECK(adaptive_tune_zones[1].coupled_cells_changed == 0,
               "K3: an excluded-fraction refusal must reset coupled_cells_changed (genuinely established >0 "
               "beforehand, not trivially zero)");
    TEST_CHECK(strstr(adaptive_tune_zones[1].ki_refusal_reason, "excluded") != NULL,
               "H2: the Ki refusal reason must say why THIS run produced no diagnosis");
}

// K1: the two skip paths H2's own fix forgot -- !z->enabled and !zr->active
// (the profile's zone mask) -- are the SAME shape as the two above, and on
// a kiln with fewer zones than MAX31856_CHANNEL_COUNT, !zr->active is hit by
// ANY ordinary profile that does not touch a given zone. MUST FAIL on
// pre-K1-fix code (the old `if (!zr->active) continue;` with no reset).
static void test_run_status_fields_cleared_when_zone_masked_out_of_profile(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].ki = 1.0f;

    feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
    profile_firing_run_record_t rec1 = make_clean_record(96, 1, 900);
    rec1.zones[1].stats.dwell_err_mean_c = 0.45f;
    rec1.zones[1].stats.dwell_err_max_c = 0.50f;
    adaptive_tune_run_end(&rec1, true);
    // K9: diagnostic-only now -- see test_run_status_fields_cleared_on_faulted_run()'s identical comment.
    TEST_CHECK(!adaptive_tune_zones[1].ki_applied, "K9: adaptive_tune_ki.c never applies a correction any more");
    TEST_CHECK(adaptive_tune_zones[1].ki_verdict == (uint8_t)ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL, "setup: real verdict");

    // Run 2: zone 1 not touched by this profile at all (make_clean_record()
    // only marks the zone index it is given as active -- every other zone,
    // including zone 1 here, defaults to inactive via the memset).
    profile_firing_run_record_t rec2 = make_clean_record(97, 0, 900);
    adaptive_tune_run_end(&rec2, true);

    TEST_CHECK(!adaptive_tune_zones[1].ki_applied,
               "K1: a zone masked out of this profile must not keep publishing a PREVIOUS run's ki_applied=1");
    TEST_CHECK(adaptive_tune_zones[1].ki_verdict == (uint8_t)ADAPTIVE_TUNE_KI_INSUFFICIENT,
               "K1: a zone masked out of this profile must reset ki_verdict");
    TEST_CHECK(adaptive_tune_zones[1].ki_correction_pct == 0.0f,
               "K1: a zone masked out of this profile must reset ki_correction_pct");
    TEST_CHECK(strstr(adaptive_tune_zones[1].ki_refusal_reason, "zone mask") != NULL,
               "K1: the Ki refusal reason must say why THIS run produced no diagnosis for this zone");
}

// ---------------------------------------------------------------------
// U2: opt-in flag migration (PID_EXPANSION_PLAN.md 3.3 "consolidate the
// opt-in flag into the zone config blob"). Real NVS round trip via stubs/
// nvs.h's stub store, same convention test_enable_round_trips_through_
// persistence() used before U2 -- these write the OLD 'adap_tune'/en_mask
// byte directly (this file has the macros in scope via adaptive_tune_
// internal.h, pulled in transitively by adaptive_tune.c's own #include).
// ---------------------------------------------------------------------

static void write_old_en_mask(uint8_t mask)
{
    hal_kv_handle_t h;
    TEST_CHECK(hal_kv_open(&h, ADAPTIVE_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, ADAPTIVE_TUNE_NVS_PARTITION) ==
                   HAL_OK,
               "setup: fake_kv open must succeed once its partition is initialized");
    hal_kv_set_u8(&h, ADAPTIVE_TUNE_NVS_KEY_ENMASK, mask);
    hal_kv_close(&h);
}

static bool read_old_en_migrated(uint8_t *out)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, ADAPTIVE_TUNE_NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, ADAPTIVE_TUNE_NVS_PARTITION) !=
        HAL_OK) {
        return false;
    }
    hal_status_t err = hal_kv_get_u8(&h, ADAPTIVE_TUNE_NVS_KEY_ENMASK_MIGRATED, out);
    hal_kv_close(&h);
    return err == HAL_OK;
}

// MUST GO RED if adaptive_tune_migrate_enable_flags() (or the call to it
// from adaptive_tune_load_enable_flags()) is deleted: with no migration at
// all, every zone reloads at its struct-zero default (off), and this test's
// zone 0/zone 2 checks fail.
static void test_migrate_pulls_old_mask_into_zone_config(void)
{
    reset_module_state();
    fake_kv_reset_all();
    hal_kv_init_partition(ADAPTIVE_TUNE_NVS_PARTITION);

    write_old_en_mask((uint8_t)((1u << 0) | (1u << 2))); // zones 0 and 2 were opted in under the old scheme

    adaptive_tune_load_enable_flags();

    TEST_CHECK(adaptive_tune_get_enabled(0) == true, "zone 0's old en_mask bit must migrate into its new home");
    TEST_CHECK(adaptive_tune_get_enabled(1) == false, "zone 1 (never set in the old mask) must stay off");
    TEST_CHECK(adaptive_tune_get_enabled(2) == true, "zone 2's old en_mask bit must migrate into its new home");
    TEST_CHECK(s_fake_adaptive_enabled[0] && s_fake_adaptive_enabled[2] && !s_fake_adaptive_enabled[1],
               "the migration must have actually written the NEW home (zones_config_set_adaptive_tune_enabled()), "
               "not just this module's own RAM cache");

    uint8_t migrated = 0;
    TEST_CHECK(read_old_en_migrated(&migrated) && migrated == 1,
               "migration must record en_migrated=1 so it is never re-consulted");

    fake_kv_reset_all();
}

// The idempotency half of "stop consulting the old one": once migrated, an
// operator's explicit choice in the NEW home must survive a second boot
// even though the stale old byte is still sitting there unchanged. MUST GO
// RED if the en_migrated check is removed (or its write is skipped) --
// without it, this second load re-applies the stale old_mask bit and zone
// 0 comes back enabled, overriding the operator's own later choice.
static void test_migrate_does_not_reconsult_old_mask_after_first_migration(void)
{
    reset_module_state();
    fake_kv_reset_all();
    hal_kv_init_partition(ADAPTIVE_TUNE_NVS_PARTITION);

    write_old_en_mask((uint8_t)(1u << 0)); // zone 0 opted in under the old scheme
    adaptive_tune_load_enable_flags();
    TEST_CHECK(adaptive_tune_get_enabled(0) == true, "setup: zone 0 migrated on first load");

    // Operator explicitly turns it back off in the NEW home.
    TEST_CHECK(adaptive_tune_set_enabled(0, false), "operator disables zone 0 in its new home");

    // Simulate a reboot: this module's RAM cache is gone, but neither the
    // zone config blob (s_fake_adaptive_enabled -- the operator's real
    // choice) nor the stub NVS (old en_mask, still 0x01) changes.
    memset(adaptive_tune_zones, 0, sizeof(adaptive_tune_zones));
    adaptive_tune_load_enable_flags();

    TEST_CHECK(adaptive_tune_get_enabled(0) == false,
               "a second boot must NOT re-apply the stale old en_mask bit over the operator's later, explicit "
               "opt-out in the new home");

    fake_kv_reset_all();
}

// The old key was never written at all (a board that never opted any zone
// in under the old scheme, or a brand-new one) -- must not crash, must
// leave every zone at its default (off), and must still record en_migrated
// so this namespace is not re-opened and re-checked every single boot
// forever.
static void test_migrate_handles_absent_old_key(void)
{
    reset_module_state();
    fake_kv_reset_all();
    hal_kv_init_partition(ADAPTIVE_TUNE_NVS_PARTITION); // namespace opens, but en_mask itself was never written

    adaptive_tune_load_enable_flags();

    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        TEST_CHECK(adaptive_tune_get_enabled(zi) == false, "an absent old mask must leave every zone at its default (off)");
    }
    uint8_t migrated = 0;
    TEST_CHECK(read_old_en_migrated(&migrated) && migrated == 1,
               "an absent old key is still a completed migration (nothing to carry) -- must be marked so");

    fake_kv_reset_all();
}

// U1: one-click revert (PID_EXPANSION_PLAN.md 3.3).
// ---------------------------------------------------------------------

// MUST GO RED if the restore is made approximate (e.g. re-deriving a value
// instead of replaying the exact snapshot, or reverting gains without also
// restoring ki_baseline): the fresh SIMC gains this test's refinement
// produces are deliberately far from the priors (a genuine blended K_dc
// move plus an independently-computed SIMC Kp/Ki/Kd triple), so a revert
// that is off by even a rounding/formula difference fails these tight
// equality checks, and a revert that merely clears ki_baseline instead of
// restoring it fails the baseline check specifically.
static void test_revert_restores_exact_prior_gains_and_ki_baseline(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;         // prior -- true gain 15, see test_refinement_improves_...
    s_fake_zone_cfg[1].kp = 2.0f;            // prior PID -- deliberately NOT what SIMC will compute
    s_fake_zone_cfg[1].ki = 0.03f;
    s_fake_zone_cfg[1].kd = 0.4f;
    adaptive_tune_zones[1].ki_baseline_valid = true;
    adaptive_tune_zones[1].ki_baseline = 0.03f; // matches the prior Ki, a plausible pre-existing baseline

    const float true_k = 15.0f, ambient = 22.3f;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(1, ambient + true_k * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
    }
    profile_firing_run_record_t rec = make_clean_record(20, 1, 900);
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(adaptive_tune_zones[1].has_applied, "setup: the refinement must genuinely apply");
    float k_after = s_fake_zone_cfg[1].k_dc, kp_after = s_fake_zone_cfg[1].kp, ki_after = s_fake_zone_cfg[1].ki,
          kd_after = s_fake_zone_cfg[1].kd;
    TEST_CHECK(fabsf(k_after - 10.0f) > 1e-3f, "setup: K_dc must have genuinely moved");
    TEST_CHECK(fabsf(ki_after - 0.03f) > 1e-6f, "setup: SIMC's fresh Ki must genuinely differ from the prior");
    TEST_CHECK(adaptive_tune_zones[1].revert_available, "a genuinely applied change must offer a revert");

    char reason[96];
    adaptive_tune_revert_result_t r = adaptive_tune_revert(1, reason, sizeof(reason));
    TEST_CHECK(r == ADAPTIVE_TUNE_REVERT_OK, "revert of a real applied change must succeed");

    TEST_CHECK_NEAR(s_fake_zone_cfg[1].k_dc, 10.0f, 1e-5, "revert must restore K_dc EXACTLY, not approximately");
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].kp, 2.0f, 1e-5, "revert must restore Kp EXACTLY");
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].ki, 0.03f, 1e-5, "revert must restore Ki EXACTLY");
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].kd, 0.4f, 1e-5, "revert must restore Kd EXACTLY");
    TEST_CHECK(adaptive_tune_zones[1].ki_baseline_valid, "revert must restore the ki_baseline that was live before "
                                                          "the reverted change, not leave it cleared");
    TEST_CHECK_NEAR(adaptive_tune_zones[1].ki_baseline, 0.03f, 1e-5,
                     "revert must restore ki_baseline to its EXACT pre-change value, not the fresh (now reverted-"
                     "away) SIMC Ki -- a stale-relative-to-live baseline is exactly the reboot-ratchet defect this "
                     "layer already shipped once");
    TEST_CHECK(!adaptive_tune_zones[1].has_applied, "a reverted change is no longer 'applied'");
    TEST_CHECK(!adaptive_tune_zones[1].revert_available, "revert is one-shot -- the snapshot is consumed");

    char reason2[96];
    adaptive_tune_revert_result_t r2 = adaptive_tune_revert(1, reason2, sizeof(reason2));
    TEST_CHECK(r2 == ADAPTIVE_TUNE_REVERT_NOTHING_TO_REVERT, "a second revert with nothing new applied must refuse "
                                                              "cleanly, not silently reapply the same old values");
    TEST_CHECK(reason2[0] != '\0', "the refusal must carry a reason string");
    (void)kp_after; (void)kd_after;
}

static void test_revert_refuses_when_nothing_to_revert(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true; // never refined -- nothing was ever applied
    char reason[96];
    adaptive_tune_revert_result_t r = adaptive_tune_revert(1, reason, sizeof(reason));
    TEST_CHECK(r == ADAPTIVE_TUNE_REVERT_NOTHING_TO_REVERT, "a zone with no applied change must refuse the revert");
    TEST_CHECK(reason[0] != '\0', "the refusal must carry a reason string");
}

// MUST GO RED if adaptive_tune_revert()'s profile_executor_get_status()
// check is removed: with the guard gone, this returns ADAPTIVE_TUNE_
// REVERT_OK (and actually reverts) instead of refusing, and the
// revert_available check right after would then also fail (the snapshot
// would already be consumed).
static void test_revert_refuses_while_firing_active(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    const float true_k = 15.0f, ambient = 22.3f;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(1, ambient + true_k * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
    }
    profile_firing_run_record_t rec = make_clean_record(21, 1, 900);
    adaptive_tune_run_end(&rec, true);
    TEST_CHECK(adaptive_tune_zones[1].revert_available, "setup: a real applied change must be available to revert");

    char reason[96];
    s_fake_exec_state = PROFILE_EXEC_RUNNING;
    TEST_CHECK(adaptive_tune_revert(1, reason, sizeof(reason)) == ADAPTIVE_TUNE_REVERT_FIRING_ACTIVE,
               "revert must refuse while a firing is RUNNING");
    TEST_CHECK(adaptive_tune_zones[1].revert_available, "a refused revert must not consume the snapshot");

    s_fake_exec_state = PROFILE_EXEC_PAUSED;
    TEST_CHECK(adaptive_tune_revert(1, reason, sizeof(reason)) == ADAPTIVE_TUNE_REVERT_FIRING_ACTIVE,
               "revert must also refuse while a firing is PAUSED");
    TEST_CHECK(adaptive_tune_zones[1].revert_available, "a refused revert must not consume the snapshot");

    s_fake_exec_state = PROFILE_EXEC_IDLE;
    TEST_CHECK(adaptive_tune_revert(1, reason, sizeof(reason)) == ADAPTIVE_TUNE_REVERT_OK,
               "sanity: once idle again, the identical revert must succeed");
}

// ---------------------------------------------------------------------
// F3 follow-up (docs/audits/FLASH_WORKER_LOCK_INVERSION_AUDIT_2026-10-09.md):
// adaptive_tune_run_end() writes the zones_config setters with
// adaptive_tune_lock RELEASED, and adaptive_tune_revert() does the same. The
// executor calls run_end with its state already DONE/FAULTED, so the HTTP
// revert route is open during that window. These pin the per-zone
// write_in_flight interlock between the two, using the setter fakes' one-shot
// hooks to land the competing call inside the unlocked window.

static void at_seed_zone1_for_refine(void)
{
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].kp = 2.0f;
    s_fake_zone_cfg[1].ki = 0.03f;
    s_fake_zone_cfg[1].kd = 0.4f;
    const float true_k = 15.0f, ambient = 22.3f;
    const float duties[4] = {0.20f, 0.50f, 0.80f, 0.35f};
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(1, ambient + true_k * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
    }
}

static adaptive_tune_revert_result_t s_hook_revert_result;
static void hook_revert_zone1(void)
{
    char reason[96];
    s_hook_revert_result = adaptive_tune_revert(1, reason, sizeof(reason));
}

// MUST GO RED if adaptive_tune_revert() stops refusing while run_end's apply
// pass is in flight: the revert would consume the snapshot plan just captured
// and write the priors, run_end's set_pid would then overwrite them, and the
// commit would mark the change applied with no revert left to offer.
static void test_revert_during_run_end_apply_is_refused_busy(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    at_seed_zone1_for_refine();
    s_hook_revert_result = ADAPTIVE_TUNE_REVERT_OK;
    s_set_model_hook = hook_revert_zone1; // fires between run_end's set_model and set_pid
    profile_firing_run_record_t rec = make_clean_record(30, 1, 900);
    adaptive_tune_run_end(&rec, true);

    TEST_CHECK(s_set_model_hook == NULL, "setup: the hook must have fired inside run_end's apply pass");
    TEST_CHECK(s_hook_revert_result == ADAPTIVE_TUNE_REVERT_BUSY,
               "a revert landing inside run_end's unlocked apply window must refuse BUSY, not report OK");
    TEST_CHECK(adaptive_tune_zones[1].has_applied, "run_end's change landed and must be recorded as applied");
    TEST_CHECK(adaptive_tune_zones[1].revert_available, "the refused revert must not consume the snapshot");
    TEST_CHECK(!adaptive_tune_zones[1].write_in_flight, "commit must clear write_in_flight");

    char reason[96];
    TEST_CHECK(adaptive_tune_revert(1, reason, sizeof(reason)) == ADAPTIVE_TUNE_REVERT_OK,
               "once run_end has committed, the revert must go through");
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].k_dc, 10.0f, 1e-5, "revert restores the prior K_dc");
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].ki, 0.03f, 1e-5, "revert restores the prior Ki");
    TEST_CHECK(!adaptive_tune_zones[1].write_in_flight, "a finished revert must clear write_in_flight");
}

static void hook_run_end_zone1(void)
{
    profile_firing_run_record_t rec2 = make_clean_record(32, 1, 900);
    adaptive_tune_run_end(&rec2, true);
}

// MUST GO RED if run_end stops skipping a zone a revert is writing: run_end
// would plan against the half-reverted config and its set_model would land
// after the revert's, leaving the "reverted" zone on a fresh model.
static void test_run_end_during_revert_write_skips_the_zone(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    at_seed_zone1_for_refine();
    profile_firing_run_record_t rec = make_clean_record(31, 1, 900);
    adaptive_tune_run_end(&rec, true);
    TEST_CHECK(adaptive_tune_zones[1].revert_available, "setup: a real applied change must be available to revert");

    // A second run's worth of settled data at a different gain, so a run_end
    // that is NOT skipped genuinely writes a new model.
    const float ambient = 22.3f;
    const float duties[4] = {0.25f, 0.55f, 0.75f, 0.40f};
    for (int i = 0; i < 4; i++) {
        feed_settled_dwell(1, ambient + 18.0f * duties[i], ambient, duties[i], SETTLE_TICKS, DT_S);
    }

    s_set_model_hook = hook_run_end_zone1; // fires between the revert's set_model and set_pid
    char reason[96];
    adaptive_tune_revert_result_t r = adaptive_tune_revert(1, reason, sizeof(reason));
    TEST_CHECK(s_set_model_hook == NULL, "setup: the hook must have fired inside the revert's write");
    TEST_CHECK(r == ADAPTIVE_TUNE_REVERT_OK, "the revert itself must succeed");
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].k_dc, 10.0f, 1e-5,
                    "a run_end landing inside the revert's write must not leave its own model behind");
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].kp, 2.0f, 1e-5, "revert restores the prior Kp");
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].ki, 0.03f, 1e-5, "revert restores the prior Ki");
    TEST_CHECK(!adaptive_tune_zones[1].has_applied, "the reverted zone must not read as applied");
    TEST_CHECK(!adaptive_tune_zones[1].revert_available, "the revert consumed the snapshot");
    TEST_CHECK(strstr(adaptive_tune_zones[1].last_refusal_reason, "revert") != NULL,
               "run_end must record why it skipped the zone");
    TEST_CHECK(!adaptive_tune_zones[1].write_in_flight, "a finished revert must clear write_in_flight");
}

static bool s_hook_in_flight_seen;
static void hook_probe_in_flight(void)
{
    s_hook_in_flight_seen = adaptive_tune_any_write_in_flight();
}

// MUST GO RED if adaptive_tune_any_write_in_flight() stops reporting the apply window: Accept
// (autotune_engine_accept) keys its 409 refusal on it (dev review 9 L1).
static void test_any_write_in_flight_covers_the_apply_window(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    at_seed_zone1_for_refine();
    s_hook_in_flight_seen = false;
    TEST_CHECK(!adaptive_tune_any_write_in_flight(), "idle: nothing in flight");
    s_set_model_hook = hook_probe_in_flight;
    profile_firing_run_record_t rec = make_clean_record(34, 1, 900);
    adaptive_tune_run_end(&rec, true);
    TEST_CHECK(s_set_model_hook == NULL, "setup: hook fired inside the apply window");
    TEST_CHECK(s_hook_in_flight_seen, "in flight must read true inside run_end's unlocked apply window");
    TEST_CHECK(!adaptive_tune_any_write_in_flight(), "cleared after commit");
}

static void hook_accept_before_apply(void)
{
    s_fake_zone_cfg[1].ki = 0.07f; // an Accept lands between run_end's plan and its first write
}

// MUST GO RED if run_end's apply stops skipping a zone whose gains changed after the plan: it would
// overwrite the accepted gains with SIMC's, and the stored revert snapshot (the pre-Accept gains)
// would later undo the Accept.
static void test_apply_skips_zone_whose_gains_changed_since_plan(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    at_seed_zone1_for_refine();
    s_get_pid_in_flight_hook = hook_accept_before_apply;
    profile_firing_run_record_t rec = make_clean_record(35, 1, 900);
    adaptive_tune_run_end(&rec, true);
    TEST_CHECK(s_get_pid_in_flight_hook == NULL, "setup: hook fired between plan and apply");
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].ki, 0.07f, 1e-6, "the accepted Ki must survive; run_end must not overwrite it");
    TEST_CHECK_NEAR(s_fake_zone_cfg[1].k_dc, 10.0f, 1e-5, "no model write for a skipped zone");
    TEST_CHECK(!adaptive_tune_zones[1].has_applied && !adaptive_tune_zones[1].revert_available,
               "a skipped zone has no applied record and no stale revert snapshot");
    TEST_CHECK(!adaptive_tune_zones[1].write_in_flight, "commit must clear write_in_flight");
}

static float s_hook_simc_ki;
static uint8_t s_hook_clear_zone;
static void hook_accept_clears_baseline(void)
{
    s_hook_simc_ki = s_fake_zone_cfg[1].ki; // what run_end's set_pid just wrote
    if (s_hook_clear_zone == 1) {
        s_fake_zone_cfg[1].ki = 0.05f; // a UART Accept on zone 1 rewrites its live PID ...
    }
    adaptive_tune_clear_ki_baseline(s_hook_clear_zone); // ... then clears that zone's baseline
}

// MUST GO RED if adaptive_tune_ki_clear_gen goes back to one global counter:
// an Accept on zone 0 inside zone 1's apply window would then stop zone 1 from
// re-latching its fresh SIMC Ki. The second pass pins the guard itself: an
// Accept on zone 1 must stop the commit latching the SIMC Ki that Accept has
// already replaced in the live config.
static void test_ki_clear_gen_is_per_zone(void)
{
    for (int pass = 0; pass < 2; pass++) {
        reset_module_state();
        at_mount_scratch();
        adaptive_tune_zones[1].enabled = true;
        at_seed_zone1_for_refine();
        adaptive_tune_zones[1].ki_baseline_valid = true;
        adaptive_tune_zones[1].ki_baseline = 0.03f;
        s_hook_simc_ki = -1.0f;
        s_hook_clear_zone = (pass == 0) ? 0 : 1;
        s_set_pid_hook = hook_accept_clears_baseline;
        profile_firing_run_record_t rec = make_clean_record(33, 1, 900);
        adaptive_tune_run_end(&rec, true);
        TEST_CHECK(s_set_pid_hook == NULL && s_hook_simc_ki > 0.0f, "setup: the hook must have fired after set_pid");
        TEST_CHECK(fabsf(s_hook_simc_ki - 0.03f) > 1e-6f, "setup: SIMC's fresh Ki must differ from the prior");
        if (pass == 0) {
            TEST_CHECK(adaptive_tune_zones[1].has_applied, "setup: the refinement must genuinely apply");
        } else {
            // Dev review 9 L1: the Accept replaced the SIMC gains, so recording has_applied (and a
            // revert snapshot) would later undo the operator's accepted gains.
            TEST_CHECK(!adaptive_tune_zones[1].has_applied && !adaptive_tune_zones[1].revert_available,
                       "L1: a writer that replaced the SIMC gains after our set_pid must leave no applied record "
                       "and no revert snapshot");
        }
        bool latched_simc = adaptive_tune_zones[1].ki_baseline_valid &&
                            fabsf(adaptive_tune_zones[1].ki_baseline - s_hook_simc_ki) < 1e-6f;
        if (pass == 0) {
            TEST_CHECK(latched_simc, "an Accept on ANOTHER zone must not stop zone 1 re-latching its fresh SIMC Ki");
        } else {
            TEST_CHECK(!latched_simc, "an Accept on zone 1 inside its apply window must stop the commit latching "
                                      "the SIMC Ki the Accept already replaced");
        }
    }
}
