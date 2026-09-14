// Split out of test_adaptive_tune.c (2026-09-01, kept the file under this repo's
// 1500-line guidance). Ki diagnosis bounding: the reboot-durable baseline
// (latch, clear-and-relatch, tracking the model layer's own fresh SIMC
// output) and the two flash-worker re-entrancy checks on the accept and
// halt paths. Verdict classification lives in test_adaptive_tune_ki_verdict.c,
// the sibling file.
// #included from test_adaptive_tune.c AFTER its fakes and test helpers -- see
// test_adaptive_tune_dwell.c's header comment for the shared-fixture convention.
//
// K9 (docs/audits/simc_sole_gain_writer_2026-09-14.md, 2026-09-14): the
// closed-loop-convergence, cumulative-bound/floor, per-run-move-cap and
// effective-vs-reference-guard tests that used to live in this file are
// REMOVED, not merely edited -- they proved behaviour of a write path
// (adaptive_tune_refine_ki_locked() writing zones_config) that no longer
// exists. adaptive_tune_ki.c is diagnostic-only now; SIMC (adaptive_tune_
// model.c) is the sole gain writer. What replaces them: a proof that this
// layer never writes regardless of verdict (test_ki_diagnosis_never_
// applies_any_verdict() below) and the anti-ratchet proof this whole pass
// exists to deliver (test_ki_diagnosis_never_ratchets_with_fuzzy_and_
// adaptive_tune_concurrent(), reusing the exact fixture shape that used to
// measure a genuine 4.2998x-of-baseline walk over 10 runs -- see that
// test's own comment). The reboot-baseline and clear-baseline tests below
// are REWRITTEN (not removed) to latch the baseline through the model/SIMC
// path instead of the now-gone Ki-diagnosis write path, since ki_baseline
// is still a real, persisted piece of state -- adaptive_tune_model.c still
// writes and re-latches it.

// ---------------------------------------------------------------------
// K9 (docs/audits/simc_sole_gain_writer_2026-09-14.md): adaptive_tune_ki.c
// is diagnostic-only now -- it still CLASSIFIES a within-dwell trace
// exactly as before (adaptive_tune_diagnose_ki(), unchanged, host-tested
// directly in test_adaptive_tune_ki_verdict.c) but adaptive_tune_refine_
// ki_locked() never applies that classification to zones_config any more.
// This proves it for every verdict that used to write a correction
// (LIMIT_CYCLE, OSCILLATING, OFFSET_TOO_SMALL): across many repeated runs
// of evidence that formerly ratcheted (or decayed) the reference Ki, the
// stored Ki must be bit-for-bit unchanged, ki_applied must be false every
// run, and the refusal reason must say so is diagnostic-only.
static void test_ki_diagnosis_never_applies_any_verdict(void)
{
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f; // ring never reaches ADAPTIVE_TUNE_MIN_OBSERVATIONS -- keeps the
                                      // model refine permanently un-due, same convention every fixture in
                                      // this file used before this pass, so the Ki diagnosis runs every time
    s_fake_zone_cfg[1].ki = 1.0f;

    // OFFSET_TOO_SMALL shape -- this exact trace used to ratchet a plain-PID
    // zone's Ki by a fixed 1.2x/run (see this file's own git history: the
    // removed test_ki_diagnosis_runaway_under_constant_error_is_capped_by_
    // cumulative_bound()).
    for (int run = 0; run < 30; run++) {
        feed_settled_dwell(1, 25.0f, 22.0f, 0.5f, 20, DT_S);
        profile_firing_run_record_t rec = make_clean_record(200 + run, 1, 900);
        rec.zones[1].stats.dwell_err_mean_c = 0.45f;
        rec.zones[1].stats.dwell_err_max_c = 0.50f;
        adaptive_tune_run_end(&rec, true);

        TEST_CHECK(!adaptive_tune_zones[1].ki_applied,
                   "K9: adaptive_tune_ki.c must never apply a correction any more, any verdict, any run");
        TEST_CHECK(adaptive_tune_zones[1].ki_verdict == (uint8_t)ADAPTIVE_TUNE_KI_OFFSET_TOO_SMALL,
                   "sanity: this fixture's trace must still classify OFFSET_TOO_SMALL -- the classifier "
                   "itself is unchanged");
        TEST_CHECK(strstr(adaptive_tune_zones[1].ki_refusal_reason, "diagnostic only") != NULL,
                   "K9: the refusal reason must say this layer is diagnostic-only");
        TEST_CHECK_NEAR(s_fake_zone_cfg[1].ki, 1.0f, 1e-6,
                         "K9: the stored Ki must be bit-for-bit unchanged -- the exact trace that used to "
                         "ratchet a plain-PID zone past 3x baseline within a handful of runs must now never "
                         "move it at all");
    }

    // LIMIT_CYCLE shape -- this used to decay Ki by a fixed 20%/run until
    // the (now-removed) cumulative floor bound it.
    reset_module_state();
    adaptive_tune_zones[1].enabled = true;
    s_fake_zone_cfg[1].k_dc = 10.0f;
    s_fake_zone_cfg[1].ki = 100.0f;
    for (int run = 0; run < 10; run++) {
        feed_oscillating_trace(1, 100.0f, 5.0f, ADAPTIVE_TUNE_KI_TRACE_CAPACITY, DT_S);
        profile_firing_run_record_t rec = make_clean_record(300 + run, 1, 900);
        rec.zones[1].stats.dwell_err_mean_c = 0.0f;
        rec.zones[1].stats.dwell_err_max_c = 0.0f;
        adaptive_tune_run_end(&rec, true);

        TEST_CHECK(!adaptive_tune_zones[1].ki_applied,
                   "K9: a LIMIT_CYCLE verdict must also never apply a correction");
        TEST_CHECK_NEAR(s_fake_zone_cfg[1].ki, 100.0f, 1e-6,
                         "K9: the stored Ki must be bit-for-bit unchanged under an oscillating trace too");
    }
}

// ---------------------------------------------------------------------

// P1: the Ki-diagnosis baseline (ki_baseline/ki_baseline_valid) must survive
// a power cycle, or a bound depending on it (the plausibility comment on
// ADAPTIVE_TUNE_KI_CUMULATIVE_MAX_MULT, and any future consumer of this same
// field) gets re-anchored from whatever is live post-reboot instead of the
// original value. K9 (docs/audits/simc_sole_gain_writer_2026-09-14.md): the
// ONLY writer of ki_baseline now is the model/SIMC path (adaptive_tune_
// refine_zone_locked(), adaptive_tune_model.c) -- adaptive_tune_ki.c never
// latches or moves it any more -- so this test now latches via a genuine
// SIMC refit (same recipe as test_model_refine_relatches_ki_baseline_to_
// fresh_simc_ki() below) instead of the removed Ki-diagnosis growth path.
static void test_ki_baseline_survives_reboot_not_relatched_from_grown_ki(void)
{
    reset_module_state();
    fake_kv_reset_all();
    hal_kv_init_partition(ADAPTIVE_TUNE_NVS_PARTITION);

    adaptive_tune_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 10.0f;
    s_fake_zone_cfg[0].tau_s = 120.0f;
    s_fake_zone_cfg[0].dead_time_s = 15.0f;
    s_fake_zone_cfg[0].ki = 0.01f; // deliberately not SIMC-consistent -- guarantees a genuine SIMC
                                    // recompute and a fresh baseline latch this run

    feed_settled_dwell(0, 22.0f + 10.5f * 0.30f, 22.0f, 0.30f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.5f * 0.50f, 22.0f, 0.50f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.5f * 0.70f, 22.0f, 0.70f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.5f * 0.90f, 22.0f, 0.90f, SETTLE_TICKS, DT_S);
    profile_firing_run_record_t rec1 = make_clean_record(1, 0, 900);
    adaptive_tune_run_end(&rec1, true);
    TEST_CHECK(adaptive_tune_zones[0].has_applied, "setup: run 1 must genuinely apply a SIMC refinement");
    TEST_CHECK(adaptive_tune_zones[0].ki_baseline_valid, "setup: run 1 must latch a baseline");
    float latched_ki = adaptive_tune_zones[0].ki_baseline;
    TEST_CHECK_NEAR(latched_ki, s_fake_zone_cfg[0].ki, 1e-6, "setup: baseline latches at the fresh SIMC Ki");

    // Simulate a reboot: RAM state gone, reload from (stubbed) NVS -- same
    // idiom as test_enable_round_trips_through_persistence() above. The
    // fake zone config table (standing in for the REAL persisted zones_
    // config blob) is deliberately NOT reset here -- a real reboot keeps the
    // persisted Ki exactly where the last firing left it, it only loses
    // THIS module's own RAM state.
    memset(adaptive_tune_zones, 0, sizeof(adaptive_tune_zones));
    adaptive_tune_init();
    adaptive_tune_zones[0].enabled = true; // re-opt-in, as an operator would find it

    TEST_CHECK(adaptive_tune_zones[0].ki_baseline_valid,
               "P1: the baseline must reload as VALID after a reboot, from persisted NVS state");
    TEST_CHECK_NEAR(adaptive_tune_zones[0].ki_baseline, latched_ki, 1e-4,
                     "P1: the baseline must RELOAD the exact value the SIMC refit latched, not silently "
                     "reset to invalid or re-derive something else");

    fake_kv_reset_all(); // leave the shared fake state as every other test in this binary expects
}

// Q3: adaptive_tune_clear_ki_baseline() is the escape hatch a stale
// baseline's own plausibility reasoning names ("re-autotune this zone").
// This proves it actually works: latch a baseline via a genuine SIMC refit,
// clear it, confirm the clear persists across a reboot, then force a
// SECOND, materially different SIMC refit (a new true plant gain) and
// confirm THAT one re-latches the baseline fresh -- not silently kept at
// the cleared value. K9: uses the SIMC path throughout (see the reboot test
// above's own comment for why the Ki-diagnosis recipe this test used to use
// no longer exists).
static void test_clear_ki_baseline_lets_the_next_refine_relatch_fresh(void)
{
    reset_module_state();
    fake_kv_reset_all();
    hal_kv_init_partition(ADAPTIVE_TUNE_NVS_PARTITION);

    adaptive_tune_zones[0].enabled = true;
    s_fake_zone_cfg[0].k_dc = 10.0f;
    s_fake_zone_cfg[0].tau_s = 120.0f;
    s_fake_zone_cfg[0].dead_time_s = 15.0f;
    s_fake_zone_cfg[0].ki = 0.01f;

    feed_settled_dwell(0, 22.0f + 10.5f * 0.30f, 22.0f, 0.30f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.5f * 0.50f, 22.0f, 0.50f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.5f * 0.70f, 22.0f, 0.70f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 10.5f * 0.90f, 22.0f, 0.90f, SETTLE_TICKS, DT_S);
    profile_firing_run_record_t rec1 = make_clean_record(1, 0, 900);
    adaptive_tune_run_end(&rec1, true);
    TEST_CHECK(adaptive_tune_zones[0].ki_baseline_valid, "setup: run 1 must latch a baseline");
    float first_ki = adaptive_tune_zones[0].ki_baseline;

    // The "re-autotune" remedy -- clears the stale baseline for this zone,
    // exactly as autotune_engine.c's accept path does after committing a
    // fresh result.
    adaptive_tune_clear_ki_baseline(0);
    TEST_CHECK(!adaptive_tune_zones[0].ki_baseline_valid,
               "Q3: adaptive_tune_clear_ki_baseline() must invalidate the RAM baseline immediately");

    // Reboot-survival, checked BEFORE the next refine re-latches anything.
    memset(adaptive_tune_zones, 0, sizeof(adaptive_tune_zones));
    adaptive_tune_init();
    TEST_CHECK(!adaptive_tune_zones[0].ki_baseline_valid,
               "Q3: a cleared baseline must reload as INVALID after a reboot too -- the clear must reach "
               "NVS, not just RAM");
    adaptive_tune_zones[0].enabled = true; // re-opt-in, as an operator would find it post-reboot

    // A materially different plant (true gain 15.0 instead of 10.5, still
    // within ADAPTIVE_TUNE_MAX_JUMP_RATIO of the now-live SIMC-fit Ki) forces
    // a genuine second SIMC refit -- this zone's plant model genuinely
    // changed (that is what "re-autotuned" means), so the baseline must
    // track THIS refit, not the value cleared above.
    feed_settled_dwell(0, 22.0f + 15.0f * 0.30f, 22.0f, 0.30f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 15.0f * 0.50f, 22.0f, 0.50f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 15.0f * 0.70f, 22.0f, 0.70f, SETTLE_TICKS, DT_S);
    feed_settled_dwell(0, 22.0f + 15.0f * 0.90f, 22.0f, 0.90f, SETTLE_TICKS, DT_S);
    profile_firing_run_record_t rec2 = make_clean_record(2, 0, 900);
    adaptive_tune_run_end(&rec2, true);
    TEST_CHECK(adaptive_tune_zones[0].has_applied, "setup: run 2 (post-clear) must also genuinely refine");
    TEST_CHECK(adaptive_tune_zones[0].ki_baseline_valid, "Q3: the baseline must be valid again after re-latching");
    TEST_CHECK(fabsf(adaptive_tune_zones[0].ki_baseline - first_ki) > 1e-6f,
               "Q3: the re-latched baseline must track the NEW SIMC fit, not silently keep the value that "
               "was just cleared");

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
// K9/Option A+B (docs/audits/simc_sole_gain_writer_2026-09-14.md): THE
// DELIVERABLE. Owner decision 2026-09-14: fuzzy and the self-improving PID
// must run CONCURRENTLY WITH NO INTERLOCK. Proves it: a zone configured
// ZONE_CONTROL_MODE_PID_FUZZY at a real, non-zero strength, with
// adaptive_tune enabled, across several sequential simulated run-ends --
// the reference Ki must never walk, and SIMC refinement must still occur.
//
// control_mode/fuzzy_strength_pct are set here for documentation of the
// concurrent-operation claim, not because anything in THIS module reads
// them any more (it doesn't -- see adaptive_tune_ki.c's own top comment).
// That absence is the actual point: fuzzy's state is structurally
// irrelevant to this layer now that it never writes, so there is nothing
// left to interlock. The old effective-vs-reference guard (e78fbc5b,
// hardened by 83627343/ac5c26a3) is what used to make that true only
// conditionally; K9 makes it true unconditionally by removing the write
// path the guard was protecting.
//
// Reuses the EXACT constant-offset OFFSET_TOO_SMALL trace shape that, with
// the pre-K9 guard removed (this file's own negative test, done by hand
// during this pass -- see docs/audits/simc_sole_gain_writer_2026-09-14.md),
// measured the reference Ki walk to 4.2998x baseline over 10 runs. Fed on
// every run here regardless of whether the SIMC refine also fires that
// run, so the Ki diagnosis sees it on whichever runs D5 hands it the turn.
static void test_ki_diagnosis_never_ratchets_with_fuzzy_and_adaptive_tune_concurrent(void)
{
    reset_module_state();
    adaptive_tune_zones[0].enabled = true;
    s_fake_zone_cfg[0].control_mode = ZONE_CONTROL_MODE_PID_FUZZY;
    s_fake_zone_cfg[0].fuzzy_strength_pct = 50.0f;
    s_fake_zone_cfg[0].k_dc = 10.0f;
    s_fake_zone_cfg[0].tau_s = 200.0f;
    s_fake_zone_cfg[0].dead_time_s = 20.0f;
    s_fake_zone_cfg[0].ki = 0.01f; // deliberately not SIMC-consistent -- guarantees the SIMC refine
                                    // genuinely fires and writes a different Ki at least once

    float initial_ki = s_fake_zone_cfg[0].ki;
    bool simc_applied = false;
    float ki_after_first_simc_apply = initial_ki;

    for (int run = 0; run < 10; run++) {
        // Same spread every run -- gives the SIMC refine enough observations
        // to fit, and to keep asymptotically converging (H4(b)'s documented
        // PERMANENT small residual under ADAPTIVE_TUNE_MIN_MATERIAL_MOVE_
        // FRAC, adaptive_tune.c -- NOT a bug, and NOT what this test is
        // guarding against). What this test guards against is the REMOVED
        // Ki-diagnosis write path's OWN failure mode: a fixed ~20%/run
        // compounding ratchet from the SAME OFFSET_TOO_SMALL trace shape fed
        // below, which measured 4.2998x baseline over exactly these 10 runs
        // with the old write path enabled (see this file's git history).
        // SIMC's own bounded convergence is orders of magnitude smaller and
        // asymptotic, not exponential -- the loose per-run bound below (5%)
        // catches an exponential ratchet while tolerating that convergence.
        feed_settled_dwell(0, 22.0f + 10.5f * 0.30f, 22.0f, 0.30f, SETTLE_TICKS, DT_S);
        feed_settled_dwell(0, 22.0f + 10.5f * 0.50f, 22.0f, 0.50f, SETTLE_TICKS, DT_S);
        feed_settled_dwell(0, 22.0f + 10.5f * 0.70f, 22.0f, 0.70f, SETTLE_TICKS, DT_S);
        feed_settled_dwell(0, 22.0f + 10.5f * 0.90f, 22.0f, 0.90f, SETTLE_TICKS, DT_S);

        float ki_before = s_fake_zone_cfg[0].ki;
        profile_firing_run_record_t rec = make_clean_record(800 + run, 0, 900);
        // The exact OFFSET_TOO_SMALL trace shape that used to ratchet a
        // plain-PID zone's reference Ki 1.2x/run -- fed on every run
        // regardless of whether SIMC also applies this run, so the Ki
        // diagnosis sees it on whichever runs D5 hands it the turn.
        rec.zones[0].stats.dwell_err_mean_c = 0.45f;
        rec.zones[0].stats.dwell_err_max_c = 0.50f;
        adaptive_tune_run_end(&rec, true);

        TEST_CHECK(!adaptive_tune_zones[0].ki_applied,
                   "K9: adaptive_tune_ki.c must never apply a correction, with fuzzy active or not");

        float ki_now = s_fake_zone_cfg[0].ki;
        // The FIRST SIMC apply is deliberately a big, one-time jump (ki
        // starts at 0.01, well off the SIMC-consistent value, precisely so
        // it is unmistakably genuine) -- the 5% per-run bound below only
        // applies to runs AFTER that first apply, where a ratchet (not a
        // one-shot correction to a deliberately-wrong starting value) would
        // actually show up.
        if (simc_applied) {
            TEST_CHECK(ki_now <= ki_before * 1.05f && ki_now >= ki_before * 0.95f,
                       "K9: once SIMC has already applied once, Ki must never move more than 5% in a "
                       "single further run -- the removed Ki-diagnosis write path moved it a fixed 20%/run "
                       "under this exact trace; SIMC's own bounded convergence (H4(b)) is asymptotic and "
                       "far smaller than that per-run figure");
        }
        if (!simc_applied && fabsf(ki_now - initial_ki) > 1e-6f) {
            simc_applied = true;
        }
        ki_after_first_simc_apply = ki_now;
    }

    TEST_CHECK(simc_applied, "setup: SIMC refinement must have genuinely occurred at least once across these "
                              "10 runs -- proving concurrent operation still adapts, not merely that it fails "
                              "to ratchet because nothing ever adapts at all");
    (void)ki_after_first_simc_apply; // tracked for readability in the per-run loop above; no further check needed --
                                      // the per-run 5% bound already proves no run-over-run ratchet occurred
}
