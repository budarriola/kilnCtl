#include "autotune_engine_internal.h"
#include "heat_enable.h"

#include "esp_attr.h" /* EXT_RAM_BSS_ATTR -- see s_at's definition below */

/* Task/lifecycle, the shared tick state machine, run setup and the plain
 * lifecycle/status API. See autotune_engine_internal.h's top comment for the
 * full five-way split this file is one piece of. */

const char *AT_TAG = "autotune_engine";

/* Moved off internal DRAM to PSRAM 2026-09-06 (DRAM_PSRAM_PLAN.md's
 * ranked-consumer table): 10428 B, second-largest single static after
 * kiln_cfg_store.c's s_store. Safe to move -- unlike s_store, nothing in
 * this file or its four siblings (autotune_engine_guard.c,
 * autotune_engine_relay.c, autotune_engine_step_identify.c,
 * autotune_engine_coupling.c) passes &s_at or a field of it as an
 * nvs_get_blob()/nvs_set_blob() buffer; the one NVS write reachable from
 * this engine (autotune_finalize_fit() -> zones_config_set_coupling_cell())
 * already reads s_at.model/s_at.coupling into locals and persists through
 * the flash worker (see autotune_engine_step_identify.c's 2026-08-31 PANIC
 * FIX comment) rather than handing NVS a pointer into this struct. Still a
 * plain global (not heap_caps_malloc'd), so every extern s_at_t s_at;
 * reference in the other four files needs no change. */
EXT_RAM_BSS_ATTR s_at_t s_at;

/* "A test is in progress" -- the one definition of it. There are four running
 * states across two methods now, and every place that used to spell out
 * "SETTLING || STEPPING" (relay authority, mutual exclusion with
 * profile_executor, the tick's own early-out, the abort paths) has to agree
 * about all four or the engine can end up driving relays in a state the rest
 * of the firmware believes is idle. */
/* One tick of the SETTLING/STEPPING/RELAY_APPROACH/RELAY_CYCLING state
 * machine, factored out of task_entry() so host tests can drive it
 * deterministically (see test_autotune_engine_prestart.c's STEPPING-loop
 * guard-coverage tests) without a real FreeRTOS task or vTaskDelay(). Caller
 * must already hold s_at.lock and have confirmed state_is_running(s_at.state)
 * -- this function never takes or gives the lock itself, matching
 * task_entry()'s existing discipline; every early-return below stands in for
 * that loop's "xSemaphoreGive(s_at.lock); continue;" pairs. */
/* Extracted 2026-09-10 (check_all_task_stack_budgets FAIL: autotune_engine
 * 2960 B vs its 2944 B ceiling, 16 B over) out of autotune_engine_tick_locked()
 * below. The three float locals + bool this needs to bridge zone_model_at()'s
 * outputs into thermal_guard_derive_climb_window_floor_s()'s inputs were
 * costing tick_locked its own 16 B of frame even though neither callee sits
 * on that task's deepest measured call chain (see the check's own printout --
 * safety_link_send_announce_version_burst's chain is what actually sets the
 * ceiling) -- a function's own frame size counts every local it declares
 * anywhere in its body, not just the ones on the path the profiler happens to
 * print. Moving them into their own frame here removes that 16 B from
 * tick_locked entirely; unlike the safety_poll helper-extraction case (CLAUDE.md's
 * "a CALL is not free on Xtensa" note), this is a genuine net win because the
 * two calls it wraps do not deepen whatever the true worst-case chain through
 * them is -- it merely moves already-existing locals into a leaf frame instead
 * of holding them live in the middle of a much deeper caller.
 *
 * __attribute__((noinline)) is required, not decorative: this is a single-
 * call-site static function, and GCC's optimizer inlines exactly those by
 * default regardless of -Os/-O2, which folds the locals straight back into
 * tick_locked's frame and silently undoes the whole point of extracting them
 * (confirmed by measuring -- the plain `static float` version measured
 * byte-identical to the pre-extraction inline block, 2960 B/16 B over). */
/* Portable noinline -- same guard as safety_cfg_store.c's
 * SAFETY_CFG_STORE_NOINLINE (see that file's comment): this file's own host
 * tests (test_autotune_engine_prestart.c, MSVC via build_host_tests.ps1)
 * hard-fail on GCC/Xtensa's __attribute__((noinline)) syntax under cl.exe --
 * not a no-op, a syntax error (confirmed: C2143/C2059/C2091/C2085 on this
 * exact line). Real ESP-IDF (Xtensa GCC) target behavior is unchanged; the
 * host .exe simply never inlines this differently than any other static
 * function, which is irrelevant off-target (no stack-budget checker runs
 * against a host build). */
#if defined(_MSC_VER)
#define AUTOTUNE_ENGINE_NOINLINE
#else
#define AUTOTUNE_ENGINE_NOINLINE __attribute__((noinline))
#endif

static AUTOTUNE_ENGINE_NOINLINE float autotune_zone_climb_window_floor_s(uint8_t zone_index, float actual_c)
{
    float model_k_dc = 0.0f, model_tau_s = 0.0f, model_dead_time_s = 0.0f;
    bool model_valid = zone_model_at(zone_index, actual_c, &model_k_dc, &model_tau_s, &model_dead_time_s);
    return thermal_guard_derive_climb_window_floor_s(model_tau_s, model_dead_time_s, model_valid);
}

static void autotune_engine_tick_locked(void)
{
    TickType_t now = xTaskGetTickCount();
    uint32_t dt_ms = at_ticks_to_ms(now - s_at.prev_tick);
    if (dt_ms == 0) dt_ms = AUTOTUNE_ENGINE_TICK_MS;
    s_at.prev_tick = now;

    /* Whole-run budget (review finding 6) -- STEP method only, see
     * AUTOTUNE_ENGINE_WHOLE_RUN_MAX_DURATION_S's own comment for why
     * AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S alone (measured from
     * phase_start_tick, which DOES reset at every phase transition) cannot
     * bound target mode's settle+probe+settle+identify sequence. Checked
     * before anything else this tick, same as a guard trip. */
    if (s_at.method == AUTOTUNE_METHOD_STEP &&
        at_ticks_to_s(now - s_at.run_start_tick) >= AUTOTUNE_ENGINE_WHOLE_RUN_MAX_DURATION_S) {
        abort_locked("whole-run time budget exceeded (settle+probe+settle+identify sum, not just one phase)");
        return;
    }

    /* Review blocker 1, live half: refused at START (autotune_engine_run()/
     * _run_to_target()), but a profile can also start on another zone AFTER
     * this run is already under way -- checked every tick too, STEP method
     * only. Not gated to STEPPING specifically: SETTLING drives duty 0 so
     * a neighbour's heat cannot bias a baseline capture either, and
     * catching it here (rather than only once STEPPING begins) means a
     * neighbour that started during THIS run's own SETTLING is caught
     * before any trace is even recorded, not partway through it.
     *
     * LOCK-ORDER FIX (review, hard blocker): this used to call
     * any_other_zone_profile_active() -> profile_executor_zone_is_active()
     * directly HERE, i.e. from inside autotune_engine_tick_locked() while
     * s_at.lock is already held (task_entry() takes it before calling this
     * function). profile_executor_zone_is_active() takes s_exec.lock --
     * and profile_executor.c's own sweep_unowned_relays() (called with
     * s_exec.lock held) calls autotune_engine_is_active_on_zone() (s_at.
     * lock) the OTHER way round. Two control tasks, both polling at ~1Hz
     * with portMAX_DELAY, taking the same two locks in opposite orders is
     * a textbook AB-BA deadlock -- profile_executor.c's own doc comment on
     * that function states the invariant this broke: "autotune never does
     * the reverse [of taking s_exec.lock while holding s_at.lock]". Fixed
     * by reading a HINT computed by task_entry() BEFORE it takes s_at.lock
     * (see that function's own comment, and other_zone_profile_active_
     * hint's field comment) instead of querying live from in here -- the
     * two START-TIME call sites in autotune_engine_run()/_run_to_target()
     * were already correct (both run before autotune_begin_run_locked() takes the
     * lock) and are untouched. */
    if (s_at.method == AUTOTUNE_METHOD_STEP && s_at.other_zone_profile_active_hint) {
        abort_locked("a profile started on another zone mid-run -- this step test's element-alive "
                     "detection can no longer trust this zone's own reading");
        return;
    }

    float raw_c = NAN;
    bool sensor_ok = false;
    /* Cold-junction reference for this tick's tested-zone channel, captured
     * alongside the TC reading below -- used ONLY to set s_at.step_ambient_c
     * at the SETTLING->STEPPING transition (see that transition's own
     * comment and autotune_finalize_fit()'s physical-plausibility check). NaN unless
     * the tested zone's own channel answered this tick. */
    float cj_c = NAN;
    /* TODO.md 6A.5(b): every channel's reading is captured this tick,
     * not just the zone under test -- MAX31856_read_all() already reads
     * the whole bus, so logging every zone's response is free (no extra
     * SPI traffic).
     *
     * ch_raw_c/ch_ok are indexed by physical MAX31856 channel, exactly
     * like profile_executor.c's identical split (TODO.md 10.8). Every
     * OTHER slot of raw_by_zone/ok_by_zone below still means what it
     * always has here -- "physical channel z's own reading", because the
     * coupling-matrix cross-fit in autotune_finalize_fit() is unchanged 6A.5(b)
     * scope and still assumes channel i == zone i for the peer zones.
     * Only index s_at.zone_index -- the zone actually under test, whose
     * baseline/trace/actual_c this tick's control math reads -- is
     * overwritten with thermo_combine()'s result across every channel
     * that zone's thermo_mask names, same pattern profile_executor.c's
     * control tick uses for every active zone. TODO.md 10.8 called this
     * file out by name as the one read path a previous pass left on the
     * legacy single-channel mapping. */
    float raw_by_zone[MAX31856_CHANNEL_COUNT];
    bool  ok_by_zone[MAX31856_CHANNEL_COUNT];
    for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
        raw_by_zone[z] = NAN;
        ok_by_zone[z] = false;
    }
    if (sim_backend_enabled() || (s_at.thermo_bus && s_at.thermo_bus->initialized)) {
        float ch_raw_c[MAX31856_CHANNEL_COUNT];
        bool  ch_ok[MAX31856_CHANNEL_COUNT];
        for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
            ch_raw_c[z] = NAN;
            ch_ok[z] = false;
        }
        MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
        size_t count = 0;
        if (sim_backend_enabled()) {
            sim_backend_read_all(readings, MAX31856_CHANNEL_COUNT, &count);
        } else {
            MAX31856_read_all(s_at.thermo_bus, readings, MAX31856_CHANNEL_COUNT, &count);
        }
        for (size_t i = 0; i < count; i++) {
            uint8_t ch = readings[i].channel;
            if (ch >= MAX31856_CHANNEL_COUNT) continue;
            bool fault_bits_bad = (readings[i].fault_status & (0x01u | 0x02u | 0x40u)) != 0;
            bool ok = !readings[i].spi_failed && !isnan(readings[i].tc_temperature_c) && !fault_bits_bad;
            ch_raw_c[ch] = readings[i].tc_temperature_c;
            ch_ok[ch] = ok;
            if (ch == s_at.zone_index && !isnan(readings[i].cj_temperature_c)) {
                cj_c = readings[i].cj_temperature_c;
            }
        }
        /* Peer zones (autotune_finalize_fit()'s cross-gain rows): legacy
         * channel-equals-zone mapping, unchanged from before 10.8. */
        for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
            raw_by_zone[z] = ch_raw_c[z];
            ok_by_zone[z] = ch_ok[z];
        }
        /* The zone actually under test: its real thermo_mask, combined,
         * is what drives the step, the guards, and the fit. */
        uint8_t tmask = 0;
        zones_config_get_thermo_mask(s_at.zone_index, &tmask);
        bool combined_valid = false;
        float combined_c =
            thermo_combine(ch_raw_c, ch_ok, MAX31856_CHANNEL_COUNT, tmask, &combined_valid);
        raw_by_zone[s_at.zone_index] = combined_c;
        ok_by_zone[s_at.zone_index] = combined_valid;
        raw_c = combined_c;
        sensor_ok = combined_valid;
    }
    s_at.actual_valid = sensor_ok;
    s_at.actual_c = sensor_ok ? zones_config_apply_cal(s_at.zone_index, raw_c) : NAN;

    /* The relay law has to run in BOTH of its states: the approach is the
     * same law, just not yet recorded. Its edge report is consumed by the
     * phase logic further down, after the guards have had their say --
     * nothing about a cycle boundary may pre-empt a guard trip. */
    bool relay_edge = false;
    bool relay_level_changed = false;
    float want_duty;
    if (s_at.method == AUTOTUNE_METHOD_RELAY) {
        bool relay_branch_before = s_at.relay_on;
        want_duty = relay_law_tick(sensor_ok, s_at.actual_c, &relay_edge);
        relay_level_changed = (s_at.relay_on != relay_branch_before);
    } else {
        want_duty = (s_at.state == AUTOTUNE_ENGINE_SETTLING) ? 0.0f : s_at.step_duty;
    }
    /* RELAY method: force the actuator's time-proportioning window to end
     * and restart on the exact tick the relay law's own branch flips, rather
     * than waiting up to window_ms (60 s default) for the ordinary window
     * boundary -- see heater_output_duty_relay_step()'s doc comment for why
     * that delay is large enough against this plant's dead time (34-53 s) to
     * keep a relay-feedback run from ever fitting a clean limit cycle. STEP
     * method and SETTLING are unaffected -- heater_output_duty_relay_step()
     * with level_changed=false behaves exactly like heater_output_duty(). */
    bool want_relay_on = heater_output_duty_relay_step(&s_at.heater_state, &s_at.heater_cfg,
                                                        sensor_ok ? want_duty : 0.0f, dt_ms, relay_level_changed);
    autotune_apply_relay(want_relay_on);
    s_at.duty = want_relay_on ? want_duty : 0.0f;

    /* Contact-cycle accounting -- same high-water-mark pattern as
     * profile_executor.c's per-zone block (TODO.md 6A.1): hand relay_cycles.c
     * only what THIS run has switched since the last tick. Before this,
     * relay_cycles was fed only by profile_executor's cycle accounting, and
     * autotune_engine switches the same physical relays -- a relay-feedback
     * test deliberately cycles them many times -- with none of it counted, so
     * the contacts aged invisibly. Confirmed on the bench: a full step-test
     * autotune left relay_cycles completely unchanged. cycle_count is a
     * lifetime counter that survives heater_output_reset() (see that file's
     * own comment), so cycles_reported is likewise never reset at the start
     * of a run -- only the delta since the last report is ever added, whether
     * that run is this one or an earlier one. */
    uint32_t cycles_now = s_at.heater_state.cycle_count;
    if (cycles_now != s_at.cycles_reported) {
        uint8_t cycle_mask = 0;
        if (zones_config_get_relay_mask(s_at.zone_index, &cycle_mask) && cycle_mask != 0) {
            relay_cycles_add(cycle_mask, cycles_now - s_at.cycles_reported);
        }
        s_at.cycles_reported = cycles_now;
    }

    /* "This element has proven it heats" -- STEP method, STEPPING only.
     * Two things happen here, both against s_at.step_rise_running_max_c
     * (tracked unconditionally, see its own comment):
     *
     *   (1) LATCH: once cumulative rise crosses AUTOTUNE_ELEMENT_ALIVE_
     *       RISE_C (a small ABSOLUTE threshold -- NOT autotune_min_rise_c(),
     *       see AUTOTUNE_ELEMENT_ALIVE_RISE_C's own comment for why the
     *       first version's reuse of that threshold never actually engaged
     *       on the measured plant), with a genuine onset already detected
     *       (step_onset_seen -- rules out a couple of coincidental
     *       quantization ticks). Checked only while still false, so a
     *       momentary noisy dip back under the threshold cannot un-latch it
     *       mid-plateau, which is exactly the scenario this exists to
     *       survive.
     *
     *   (2) DEATH CHECK: once latched, EITHER a drop of AUTOTUNE_ELEMENT_
     *       DEATH_DROP_C below the running peak, OR rise_c falling back
     *       below AUTOTUNE_ELEMENT_ALIVE_RISE_C (review blocker 3 -- the
     *       relative check alone is arithmetically dead below a 5.0C peak),
     *       aborts the run -- this file's own stand-in for guard 2, which
     *       cannot reach this case during a step test (see that constant's
     *       comment for why). Checked EVERY tick once proven, not just at
     *       latch time. */
    if (s_at.method == AUTOTUNE_METHOD_STEP && s_at.state == AUTOTUNE_ENGINE_STEPPING && s_at.actual_valid &&
        s_at.zone_baseline_valid[s_at.zone_index]) {
        /* Scaled once per tick off THIS run's own probe_k_rough (0 for a
         * plain, non-target-mode run -- see AUTOTUNE_REFERENCE_K_C_PER_
         * DUTY's comment for why that falls back to the bare constants
         * unchanged). Same relative ordering as the un-scaled constants
         * (alive_rise_c > death_floor_margin_c), preserved because both are
         * scaled by the identical factor. */
        float alive_rise_c = autotune_scale_threshold_c(AUTOTUNE_ELEMENT_ALIVE_RISE_C, s_at.probe_k_rough);
        float death_drop_c = autotune_scale_threshold_c(AUTOTUNE_ELEMENT_DEATH_DROP_C, s_at.probe_k_rough);
        float death_floor_margin_c =
            autotune_scale_threshold_c(AUTOTUNE_ELEMENT_DEATH_FLOOR_MARGIN_C, s_at.probe_k_rough);
        float rise_c = s_at.actual_c - s_at.zone_baseline_c[s_at.zone_index];
        if (rise_c > s_at.step_rise_running_max_c) {
            s_at.step_rise_running_max_c = rise_c;
        }
        if (!s_at.step_element_proven && s_at.step_onset_seen && rise_c >= alive_rise_c) {
            s_at.step_element_proven = true;
            ESP_LOGI(AT_TAG, "autotune zone %u: element proven (rise %.2fC) -- guard 1's rise requirement is "
                          "relaxed for the rest of this step; the running-peak death check now covers "
                          "guard 2's job instead",
                     s_at.zone_index, (double)rise_c);
        }
        /* Review blocker 3: the RELATIVE check alone (drop from running
         * peak) is arithmetically incapable of firing when the peak never
         * reaches AUTOTUNE_ELEMENT_DEATH_DROP_C (5.0) in the first place --
         * step_rise_running_max_c starts at 0.0f, so a peak anywhere in
         * [ALIVE_RISE_C, DEATH_DROP_C) = [3.0, 5.0) makes the drop needed
         * to reach 5.0 larger than the peak itself. Measured: the 0.15 duty
         * probe phase on this hardware's plant peaks around 5.4C by the
         * 600s budget -- and even THAT best case needs a fall from 5.4 to
         * 0.4 (a 5.0C drop) to trip, which at this plant's own cooling rate
         * takes 741s against a 600s budget. The whole probe phase sat in
         * this dead zone: proven, guard 1 relaxed, guard 2 unreachable, and
         * the death check unable to fire at all.
         *
         * Fixed with a SECOND, ABSOLUTE condition alongside the relative
         * one: once proven, rise_c falling back below AUTOTUNE_ELEMENT_
         * ALIVE_RISE_C also trips -- symmetric with the condition that
         * proved it in the first place ("this only counts as alive above
         * the alive floor" cuts both ways). This closes the dead zone
         * entirely: below a 5.0C peak, the absolute floor is the ONLY one
         * that can ever fire (the relative drop mathematically cannot);
         * above it, whichever condition is reached first fires. */
        bool relative_drop = (s_at.step_rise_running_max_c - rise_c) >= death_drop_c;
        /* Review round-3 finding 2: a DEADBAND (2.5C, not the 3.0C the
         * latch itself uses) plus a DWELL (must read below the deadband
         * for AUTOTUNE_ELEMENT_DEATH_FLOOR_CONSECUTIVE_TICKS ticks in a
         * row, not just once) -- see AUTOTUNE_ELEMENT_DEATH_FLOOR_MARGIN_C's
         * own comment for why a bare "rise_c < ALIVE_RISE_C" false-tripped
         * a healthy plateau. The streak counter is maintained every tick
         * (proven or not, though it is only ever READ while proven) so it
         * is already correct the instant proven flips true. */
        float death_floor_c = alive_rise_c - death_floor_margin_c;
        if (rise_c < death_floor_c) {
            if (s_at.step_below_death_floor_ticks < UINT16_MAX) {
                s_at.step_below_death_floor_ticks++;
            }
        } else {
            s_at.step_below_death_floor_ticks = 0u;
        }
        bool below_alive_floor = s_at.step_below_death_floor_ticks >= AUTOTUNE_ELEMENT_DEATH_FLOOR_CONSECUTIVE_TICKS;
        if (s_at.step_element_proven && (relative_drop || below_alive_floor)) {
            char detail[80];
            if (relative_drop) {
                snprintf(detail, sizeof(detail), "element died after proving alive: dropped %.2fC from a %.2fC peak",
                         (double)(s_at.step_rise_running_max_c - rise_c), (double)s_at.step_rise_running_max_c);
            } else {
                snprintf(detail, sizeof(detail), "element died after proving alive: rise %.2fC stayed below the "
                                                 "%.2fC floor",
                         (double)rise_c, (double)death_floor_c);
            }
            autotune_escalate_and_abort(THERMAL_GUARD_TRIP_WRONG_DIRECTION, detail);
            return;
        }
    }

    /* Review blocker 4's own fix, see autotune_step_guard_sanity_rate()'s
     * comment -- a per-tick LOCAL copy of s_at.guard_cfg, same pattern as
     * profile_executor.c's guard_cfg_this_tick: the zone's own stored
     * guard_cfg.sanity_rate_c_per_min is never mutated, only what THIS
     * tick's thermal_guard_tick() call is handed. STEP method, STEPPING
     * only -- SETTLING commands duty 0 (nothing to scale against) and the
     * RELAY method's guard suite is deliberately left at the zone's full,
     * unscaled configuration (see this file's own doc note on why the
     * relay test runs the full guard suite, never a relaxed one). */
    thermal_guard_cfg_t guard_cfg_this_tick = s_at.guard_cfg;
    if (s_at.method == AUTOTUNE_METHOD_STEP && s_at.state == AUTOTUNE_ENGINE_STEPPING) {
        guard_cfg_this_tick.sanity_rate_c_per_min =
            autotune_step_guard_sanity_rate(s_at.guard_cfg.sanity_rate_c_per_min, s_at.step_duty);
    }
    /* 2026-09-10 opus review finding B: cf3b5adb floored guard 1's climbing-
     * branch window at dead_time_s+tau_s in profile_executor.c's tick
     * (guard_cfg_this_tick.climb_window_floor_s = thermal_guard_derive_
     * climb_window_floor_s(...)) but never set the SAME field here --
     * thermal_guard_cfg_t is a struct with two producers
     * (profile_executor.c and this file) and, until now, only one of them
     * filled in this field, leaving it at guard_cfg's own persisted 0 for
     * every autotune tick regardless of method. A STEP run on a slow zone
     * (this bench's tau ~264-271s) evaluates guard 1's climbing branch
     * against the unfloored wrong_dir_window_s/PROGRESS_WINDOW_S default,
     * which can be shorter than the zone's own dead_time_s+tau_s -- the
     * exact false HEATING_FAILED trip cf3b5adb exists to prevent, just
     * reachable from the other caller. progress_rise_check_relaxed (set
     * just above) only covers the window AFTER step_element_proven, so this
     * gap was open for the whole early-climb phase leading up to that.
     *
     * Same derivation as profile_executor.c's call site, same zone_model_at()
     * seam profile_executor_feedforward.c/profile_feasibility.c already use
     * (a temperature-scheduled passthrough to zones_config_get_model() today
     * -- see zones_config_accessors.c's own comment; this file already
     * includes zones_config_accessors.h via autotune_engine_internal.h) so
     * a future gain schedule benefits both callers identically rather than
     * this file quietly reading the un-scheduled accessor underneath it.
     * model_valid=false (no trustworthy fit yet, or none ever produced)
     * makes the derivation return 0.0f -- "don't touch window_s" --
     * identical to today's behaviour for a zone with no model, exactly like
     * the profile_executor call site's own fallback. Computed fresh every
     * tick from s_at.zone_index/s_at.actual_c, never written back to any
     * persisted config. Factored into autotune_zone_climb_window_floor_s()
     * above this function -- see that helper's own comment for why (stack
     * budget, not readability). */
    guard_cfg_this_tick.climb_window_floor_s =
        autotune_zone_climb_window_floor_s(s_at.zone_index, s_at.actual_c);
    /* Scaled the same way as the alive/death thresholds above -- see
     * AUTOTUNE_REFERENCE_K_C_PER_DUTY's comment. Falls back to the bare
     * 5.0C constant when probe_k_rough is unavailable (plain runs, or
     * before PHASE 1 completes), which is what guard 4's own "behaviorally
     * identical for any strictly positive value" reasoning (this constant's
     * comment) already tolerates. */
    float step_test_guard_headroom_c = autotune_scale_threshold_c(STEP_TEST_GUARD_HEADROOM_C, s_at.probe_k_rough);

    thermal_guard_input_t gin = {
        .sensor_ok = sensor_ok,
        .measurement_c = raw_c,
        .progress_rise_check_relaxed = (s_at.method == AUTOTUNE_METHOD_STEP) && s_at.step_element_proven,
        /* A relay run has a real setpoint, so guard 4 (drift after
         * settling) becomes a genuine check that the oscillation stayed
         * around the target instead of walking away from it -- the step
         * test has no setpoint and can only feed the guard its ceiling.
         * The comparison is raw-vs-calibrated by exactly the zone's
         * calibration offset, which is worth far less than guard 4's 25degC
         * band, and guards must keep seeing raw readings (TODO.md 6A.7:
         * a calibration offset may never hide a sensor from a guard).
         *
         * When no ceiling is configured (max_temp_c == 0, the default for a
         * zone the operator has never set one on -- readiness_http.c's
         * non-blocking "guard_max_temp" item), this used to fall back to
         * raw_c itself. That made setpoint_c == measurement_c on every tick,
         * pinning thermal_guard's `error` at exactly 0.0f for the whole run.
         * error > 0.0f is guard 1's (heating-failed / no-progress) branch
         * selector, so with error permanently at 0 guard 1 never ran at
         * all -- a dead element or a flat-but-plausible thermocouple could
         * duty-cycle for the full 4h budget undetected. Guard 2
         * (wrong-direction) isn't actually broken by this -- its own math
         * (thermal_guard.c) only ever reads real measurement deltas across
         * the progress window, never setpoint_c's magnitude -- but pinning
         * error at exactly 0 permanently steers every window into guard 2's
         * branch instead of guard 1's, so a stalled-but-not-yet-falling zone
         * (delta ~= 0, the common dead-element case) still passed guard 2's
         * "not falling faster than threshold" test with nothing to catch it
         * on the way through.
         *
         * Fix: fall back to raw_c + a fixed positive headroom instead of
         * bare raw_c. That keeps error strictly positive every tick, which
         * keeps every progress window on guard 1's branch -- the guard whose
         * job this actually is, since "delta < expected-rise" already
         * catches both a flat AND a falling reading, guard 2's narrower
         * "falling" case included. This was chosen over refusing to start a
         * step test without a configured ceiling (which would contradict
         * autotune_engine_run_relay()'s own comment that no-ceiling is
         * "survivable for a step test the operator watches climb" -- that
         * reasoning covers guard 5, not guards 1/2, but a step test's open
         * loop and short-ish default budget make an outright refusal more
         * restrictive than this bug warrants) and over silently disabling
         * guards 1/2 with a logged flag (unnecessary now that they are
         * genuinely covered). Guard 4 keeps behaving exactly as before in
         * this configuration -- see STEP_TEST_GUARD_HEADROOM_C's own comment.
         *
         * SETTLING EXCEPTION (2026-09-03, guard 4 arming backstop regression):
         * thermal_guard.c's guard 4 gained an "idle arming backstop" (commit
         * "Fix ramp-lock hot-start stall") -- once commanded_duty stays below
         * progress_duty_min for drift_period_s (600s default), guard 4 arms
         * even if the zone never once settled within drift_hysteresis_c of
         * setpoint_c, then trips if it stays outside that band for another
         * drift_period_s. That backstop is correct for profile_executor.c's
         * real setpoints, but this STEP branch's fallback above is not a real
         * setpoint at all while SETTLING -- it is max_temp_c (the zone's
         * configured CEILING), fed only so guard 1's error stays > 0 once
         * STEPPING starts driving real duty. During SETTLING commanded_duty
         * is always 0 (want_duty below), so this fake ceiling-as-setpoint is
         * *always* tens of degrees from raw_c by construction -- feeding it
         * to guard 4 here means every ordinary SETTLING phase idles long
         * enough to arm the backstop and looks permanently "outside band",
         * which used to be harmless (guard 4 could never arm without a real
         * settle) and is now a false trip waiting to happen the moment
         * idle_elapsed_s clears drift_period_s twice over. SETTLING has no
         * real setpoint to give guard 4 in the first place -- feed raw_c
         * itself so error is pinned at 0.0f (harmless: guard 1/2 never see
         * this tick anyway, since they require commanded_duty >=
         * progress_duty_min, which SETTLING's duty of 0 never clears).
         * STEPPING (and RELAY, which always has a genuine relay_setpoint_c)
         * are completely unaffected -- this only changes the value fed
         * while SETTLING. */
        .setpoint_c = (s_at.method == AUTOTUNE_METHOD_RELAY)
                          ? s_at.relay_setpoint_c
                          : (s_at.state == AUTOTUNE_ENGINE_SETTLING)
                                ? raw_c
                          : (s_at.guard_cfg.max_temp_c > 0.0f ? s_at.guard_cfg.max_temp_c
                                                               : raw_c + step_test_guard_headroom_c),
        /* STEP has no real setpoint at any state -- setpoint_c above is
         * purely a placeholder to keep guard 1's error sign positive (or, in
         * SETTLING, pinned harmlessly at 0; see that comment block above).
         * no_setpoint tells thermal_guard.c's guard 4 (drift-at-setpoint) not
         * to treat that placeholder as a real target -- see thermal_guard.h's
         * doc comment on the field. Without this, guard 4 read STEP's
         * max_temp_c-ceiling placeholder as if it were a genuine setpoint and
         * could false-trip a healthy step test once idle_elapsed_s armed its
         * backstop (e.g. a step_duty configured below progress_duty_min).
         * RELAY has a genuine oscillation setpoint (relay_setpoint_c) and
         * leaves guard 4 fully armed, as before. */
        .no_setpoint = (s_at.method != AUTOTUNE_METHOD_RELAY),
        /* STEP method: the INTENDED duty (want_duty, pre-PWM), not the
         * post-PWM want_relay_on-gated value -- found while testing review
         * finding 3 (progress_duty_min override) at a realistic sub-1.0
         * duty: heater_output_duty() PWM-chops any duty below 1.0 into
         * on/off pulses within its own window_ms, and commanded_duty
         * dropping to 0 every "off" pulse was resetting guard 1/2's
         * (unrelated, much longer -- 300s default) progress window every
         * single PWM cycle, so lowering progress_duty_min ALONE still left
         * the window unable to ever accumulate 300s at any duty below
         * 1.0 -- only duty==1.0 (heater_output_duty()'s documented
         * always-on special case) ever worked, which is exactly why every
         * pre-existing guard-1 test in this file used step_duty=1.0. Using
         * want_duty (what this run is COMMANDING this phase, matching
         * s_at.step_duty during STEPPING) instead of the instantaneous PWM
         * state is what thermal_guard_input_t's own "what this tick decided
         * to drive... AFTER any safety-refusal" doc comment is reaching for.
         *
         * CORRECTION (review, after the same fix landed in
         * profile_executor.c): an earlier version of this comment claimed "a
         * relay_authority block still zeroes it" here. That is FALSE --
         * relay_authority_zone_blocked() is evaluated inside autotune_apply_relay()
         * (this file, above) purely to gate autotune_apply_relay()'s OWN local
         * want_on before it writes the relay; the result never feeds back
         * into want_duty, which is computed earlier in this function and
         * unconditionally fed to thermal_guard_tick() as commanded_duty
         * regardless of whether autotune_apply_relay() went on to refuse the write.
         * This file has NOT been given profile_executor.c's equivalent fix
         * (a per-zone "was this tick's want_on actually blocked" signal,
         * fed as a genuine zero rather than the intended duty) -- an
         * autotune run that is relay-authority-blocked for its entire
         * STEPPING phase still reports its full commanded duty to the
         * guards here, exactly as it always did. Left uncorrected in THIS
         * pass (target-mode work, including this file's guard wiring, is
         * paused pending review of separate findings) -- sensor_ok is the
         * only thing this expression actually gates.
         *
         * RELAY method had the SAME defect and was left uncorrected the
         * first time this comment was written ("its want_duty deliberately
         * toggles between two extremes as part of the bang-bang law itself,
         * a different situation this fix does not address"). That reasoning
         * does not survive contact with what was actually fed to the guard:
         * want_relay_on is heater_output_duty_relay_step()'s own PWM state
         * for whichever branch relay_law_tick() selected (AUTOTUNE_RELAY_
         * CENTER_DUTY +/- relay_d, e.g. 0.85 / 0.15 at the default d=0.35 --
         * see relay_law_tick()'s own comment on why the low branch is
         * nonzero, not off), and `want_relay_on ? want_duty : 0.0f` zeroes
         * commanded_duty on every PWM off-pulse of EITHER branch, not just
         * during a genuine low-branch period -- exactly the class of defect
         * profile_executor.c's fix below describes, just unfixed here.
         * Guards 1/2's progress window (thermal_guard.c: resets whenever
         * commanded_duty < progress_duty_min) and guard 7's frozen-sensor
         * window (resets whenever commanded_duty drops to <= 0) therefore
         * both restarted every ~window_ms during a relay run, structurally
         * unable to complete: armed in guard_cfg, but inert against the
         * duty this method actually drives -- found in review, not a bench
         * trip, same discovery shape as the step-test defect just above.
         *
         * Fixed the same way: feed both methods the INTENDED duty (want_duty,
         * pre-PWM -- relay_law_tick()'s own branch value for RELAY, exactly
         * as already computed above), not the instantaneous post-PWM
         * want_relay_on-gated state. want_duty already carries the right
         * per-method value (STEP: 0 during SETTLING, step_duty during
         * STEPPING; RELAY: relay_law_tick()'s current branch), so a single
         * expression now covers both methods -- see
         * autotune_engine_run_relay()'s own progress_duty_min override
         * (mirrors AUTOTUNE_PROGRESS_DUTY_MIN_FOR_STEP_TEST) for the other
         * half of this fix: without it the low branch (0.15 by default,
         * strictly below thermal_guard.c's stock 0.5 progress_duty_min)
         * would still degate the window every other half-cycle even though
         * it is now fed continuously.
         *
         * Guard 3 (runaway, heat off) is UNCHANGED by this fix and stays
         * correctly inert for a normal relay run: want_duty's low branch is
         * a real nonzero duty (0.15 by default), so commanded_duty <= 0
         * never holds except at the edge case d == AUTOTUNE_RELAY_MAX_D
         * (0.5, low branch truly 0). CORRECTION: an earlier version of this
         * comment claimed that edge case is "exactly the case where heat
         * really is off and guard 3 SHOULD be able to see it" -- that is
         * FALSE and contradicts relay_law_tick()'s own comment just above in
         * this file (see "One consequence of centring on 0.5..."): a kiln
         * keeps climbing for minutes after heat is genuinely cut, which is
         * precisely why relay_law_tick() deliberately keeps the low branch
         * nonzero at every OTHER d -- to keep guard 3's window from ever
         * opening during ordinary cycling. At d == MAX_D that protection
         * lapses and guard 3's window opens exactly as it would for any
         * other zero-duty period, generic-heat-off false-positive risk and
         * all; this fix neither improves nor worsens that one pre-existing
         * edge case, it just correctly leaves it alone. The observable
         * CLAIM this whole comment makes -- guard 3's behaviour at every d is
         * identical before and after this fix, because the low branch was 0
         * only at that same edge before and after -- remains true; only the
         * sentence trying to justify it as a feature was wrong.
         *
         * profile_executor.c's own identical PWM/progress-window defect
         * (commanded_duty fed from the post-PWM relay state, resetting
         * guards 1/2/3/7's windows on every PWM cycle at any duty strictly
         * between 0 and 1) has been fixed separately and shipped on its own
         * -- see that file's apply-relays-and-guards loop for the fix and
         * its explicit reasoning on the load-cap and authority-block
         * questions the same defect class raises there. */
        .commanded_duty = sensor_ok ? want_duty : 0.0f,
        .dt_s = (float)dt_ms / 1000.0f,
        /* Review finding (this fix's own false-trip exposure): feeding
         * want_duty above finally armed guards 1/2 for the WHOLE relay run
         * (both branches sit above progress_duty_min now), but the
         * directional climbing/falling test they inherited from the step
         * path is unsafe against a limit cycle -- a single window landing on
         * the low branch's downswing, or on the cooling half of a
         * wide-hysteresis cycle, reads as guard 2 or guard 1 respectively
         * even though the run is healthy. See
         * thermal_guard_input_t.relay_min_swing_c for the discriminator this
         * uses instead (amplitude, not direction) and why 2*relay_h is a
         * hard physical floor for ANY live element on this path:
         * relay_law_tick() only flips branches when the measurement reaches
         * setpoint -/+ relay_h, so a genuinely responding element cannot
         * avoid producing at least that much peak-to-trough swing, while a
         * dead one produces none of it regardless of which branch the law
         * currently thinks it's driving. STEP leaves this at its zero
         * default and keeps the existing directional test unchanged. */
        .relay_min_swing_c = (s_at.method == AUTOTUNE_METHOD_RELAY) ? (2.0f * s_at.relay_h) : 0.0f,
    };
    if (thermal_guard_tick(&s_at.guard_state, &guard_cfg_this_tick, &gin)) {
        autotune_escalate_and_abort(s_at.guard_state.reason, s_at.guard_state.detail);
        return;
    }

    s_at.elapsed_s = at_ticks_to_s(now - s_at.phase_start_tick);

    if (s_at.state == AUTOTUNE_ENGINE_SETTLING) {
        /* First valid-or-not reading of THIS SETTLING phase -- see
         * readiness_start_c's own comment. Captured once, on the very
         * first tick after entering SETTLING (or re-entering it, for
         * target mode's probe->identify re-settle), regardless of how far
         * elapsed_s still has to go. */
        if (!s_at.readiness_start_captured) {
            for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
                s_at.readiness_start_valid[z] = ok_by_zone[z];
                s_at.readiness_start_c[z] = ok_by_zone[z] ? zones_config_apply_cal(z, raw_by_zone[z]) : 0.0f;
            }
            s_at.readiness_start_captured = true;
        }
        if (s_at.elapsed_s >= AUTOTUNE_ENGINE_SETTLE_S) {
            /* Ambient reference for step_ambient_c / autotune_finalize_fit()'s
             * physical-plausibility check further down -- NOT used by the
             * readiness check just below any more (see check_thermal_
             * readiness_locked()'s own "THE REFERENCE PROBLEM" comment for
             * why: the CJ-to-chamber offset is fixed per board/zone, not a
             * sign of residual heat, and folding it into a refusal made
             * this check permanently strict on this rig). The cj_c
             * captured above this tick if the tested zone's own channel
             * answered, else the same documented fallback profile_
             * executor.c uses (FALLBACK_AMBIENT_C, 20.0C). */
            float ambient_c_now = isnan(cj_c) ? AUTOTUNE_FALLBACK_AMBIENT_C : cj_c;

            char readiness_reason[96];
            if (!check_thermal_readiness_locked(raw_by_zone, ok_by_zone, readiness_reason,
                                                sizeof(readiness_reason))) {
                abort_locked(readiness_reason);
                ESP_LOGW(AT_TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
                return;
            }

            for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
                s_at.zone_baseline_valid[z] = ok_by_zone[z];
                s_at.zone_baseline_c[z] = ok_by_zone[z] ? zones_config_apply_cal(z, raw_by_zone[z]) : 0.0f;
                s_at.zone_last_valid_c[z] = s_at.zone_baseline_c[z];
            }
            s_at.state = AUTOTUNE_ENGINE_STEPPING;
            s_at.phase_start_tick = now;
            s_at.last_sample_tick = now;
            s_at.trace_count = 0;
            s_at.step_peak_slope_c_per_s = 0.0f;
            s_at.step_peak_slope_at_s = 0u;
            s_at.step_settled = false;
            s_at.step_onset_seen = false;
            s_at.step_onset_trace_count = 0u;
            s_at.step_element_proven = false; /* re-earned fresh every STEPPING phase -- see its own comment */
            s_at.step_rise_running_max_c = 0.0f;
            s_at.step_below_death_floor_ticks = 0u;
            s_at.probe_last_k_c_per_duty = 0.0f;
            s_at.probe_stable_checks = 0u;
            s_at.step_ambient_c = ambient_c_now;
            heater_output_reset(&s_at.heater_state);
            ESP_LOGI(AT_TAG, "autotune zone %u: settled at %.1fC, stepping duty to %.2f", s_at.zone_index,
                     (double)s_at.zone_baseline_c[s_at.zone_index], (double)s_at.step_duty);
        }
    } else if (s_at.state == AUTOTUNE_ENGINE_STEPPING) {
        /* Target mode's PHASE 1 (probe) uses a much shorter budget than the
         * real identification step -- see AUTOTUNE_ENGINE_PROBE_DURATION_S's
         * comment for the tradeoff. Both budgets, and both settle-detector
         * hits, route to handle_probe_done_locked() instead of
         * autotune_finalize_fit() while probing; step_settled/step_peak_slope_* etc.
         * get reset again at the probe->identify SETTLING->STEPPING
         * transition, so nothing from the probe phase leaks into the real
         * fit's own settle detection. */
        bool probing = s_at.target_mode && s_at.probe_phase;
        uint32_t phase_budget_s = probing ? AUTOTUNE_ENGINE_PROBE_DURATION_S : AUTOTUNE_ENGINE_DEFAULT_MAX_DURATION_S;
        if (s_at.elapsed_s >= phase_budget_s) {
            if (probing) {
                handle_probe_done_locked();
            } else {
                autotune_finalize_fit(); /* attempt a fit on whatever we have; ABORTED if it doesn't fit */
            }
            return;
        }
        if (at_ticks_to_s(now - s_at.last_sample_tick) >= AUTOTUNE_ENGINE_SAMPLE_PERIOD_S) {
            s_at.last_sample_tick = now;
            /* The shared index only advances on the tested zone's own valid
             * sample -- other zones carry forward zone_last_valid_c on a
             * momentary bad read of their own, so a single dropped reading
             * on a non-tested zone can't shift its trace out of alignment
             * with the tested zone's t_s. */
            if (s_at.actual_valid) {
                record_trace_sample(raw_by_zone, ok_by_zone);
            }

            /* Probe self-termination (task B): OR'd with the shared settle
             * detector, never instead of it -- see AUTOTUNE_PROBE_GAIN_
             * STABLE_FRAC's own comment for why the probe needs a second,
             * earlier-firing criterion the identify phase does not. Only
             * evaluated while probing; the identify phase is unaffected --
             * probe_gain_converged_locked() is not even called for it, so
             * its dwell counters never advance on real identification data. */
            bool probe_converged = probing && probe_gain_converged_locked();
            if (step_settle_check_locked() || probe_converged) {
                s_at.step_settled = true;
                if (probing) {
                    handle_probe_done_locked();
                } else {
                    autotune_finalize_fit();
                }
                return;
            }
        }
    } else if (s_at.state == AUTOTUNE_ENGINE_RELAY_APPROACH) {
        /* Nothing is recorded here. The kiln is climbing (or falling) to
         * the setpoint under the relay's high (or low) branch, and that
         * transit is a step response at best and a half-cycle of nothing
         * at worst -- feeding it to pid_autotune_fit_relay() would put a
         * long monotonic ramp in the head of the trace, which both wastes
         * the fixed-size buffer and drags the midline the crossing
         * detector slices cycles against away from the oscillation's own
         * centre. Recording starts at the first high->low edge instead:
         * the first moment the plant is provably at temperature and the
         * relay's phase is known. */
        if (s_at.elapsed_s >= AUTOTUNE_RELAY_APPROACH_MAX_S) {
            char msg[96];
            snprintf(msg, sizeof(msg), "did not reach %.0fC within %us -- setpoint out of reach",
                     (double)s_at.relay_setpoint_c, (unsigned)AUTOTUNE_RELAY_APPROACH_MAX_S);
            abort_locked(msg);
            ESP_LOGW(AT_TAG, "autotune zone %u: %s", s_at.zone_index, s_at.abort_reason);
            return;
        }
        if (relay_edge) {
            s_at.state = AUTOTUNE_ENGINE_RELAY_CYCLING;
            s_at.phase_start_tick = now;   /* the cycling budget is its own, not the approach's leftovers */
            s_at.last_sample_tick = now;
            s_at.trace_count = 0;
            s_at.relay_cycles_seen = 0;
            for (uint8_t z = 0; z < MAX31856_CHANNEL_COUNT; z++) {
                s_at.zone_last_valid_c[z] = ok_by_zone[z] ? zones_config_apply_cal(z, raw_by_zone[z]) : NAN;
            }
            ESP_LOGI(AT_TAG, "autotune zone %u: reached %.1fC, relay cycling around %.1fC (d=%.2f h=%.1fC)",
                     s_at.zone_index, (double)s_at.actual_c, (double)s_at.relay_setpoint_c,
                     (double)s_at.relay_d, (double)s_at.relay_h);
        }
    } else if (s_at.state == AUTOTUNE_ENGINE_RELAY_CYCLING) {
        /* Edges are counted before the budget check so a run that
         * completes its last cycle on the same tick the budget expires is
         * treated as the success it is. */
        if (relay_edge && s_at.relay_cycles_seen < UINT16_MAX) {
            s_at.relay_cycles_seen++;
            ESP_LOGI(AT_TAG, "autotune zone %u: relay cycle %u of %u complete", s_at.zone_index,
                     (unsigned)s_at.relay_cycles_seen, (unsigned)AUTOTUNE_RELAY_TARGET_CYCLES);
        }

        if (at_ticks_to_s(now - s_at.last_sample_tick) >= AUTOTUNE_ENGINE_SAMPLE_PERIOD_S) {
            s_at.last_sample_tick = now;
            /* Same rule as the step path: the shared index only advances on
             * a valid reading of the tested zone, so its row never contains
             * a hole. For a relay run that also protects Tu, since t_s is
             * implicit in the sample index -- a recorded gap would show up
             * as a shortened period rather than as missing data. A dropout
             * long enough to matter trips guard 6 and ends the run anyway. */
            if (s_at.actual_valid) {
                record_trace_sample(raw_by_zone, ok_by_zone);
            }
        }

        /* Enough cycles, or the trace buffer is full (which at 10s
         * sampling is the 4h budget by another name) -- fit. */
        if (s_at.relay_cycles_seen >= AUTOTUNE_RELAY_TARGET_CYCLES ||
            s_at.trace_count >= AUTOTUNE_ENGINE_MAX_SAMPLES ||
            s_at.elapsed_s >= AUTOTUNE_RELAY_CYCLE_MAX_S) {
            /* On budget expiry this still attempts the fit rather than
             * aborting outright, exactly as the step path does: the fit is
             * the thing that decides whether the data is usable, and
             * pid_autotune_fit_relay() refuses a trace that never settled
             * into a limit cycle. Either way finalize_relay_fit() leaves
             * the relays off -- DONE with a proposal, or ABORTED with the
             * fitter's own reason. */
            finalize_relay_fit();
            return;
        }
    }
}

static void task_entry(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(AUTOTUNE_ENGINE_TICK_MS));

        /* Review blocker 1 fix (lock-order deadlock) -- evaluated BEFORE
         * s_at.lock is taken, never after, preserving the documented lock
         * order between this module and profile_executor.c: this module
         * may take s_exec.lock (via profile_executor_zone_is_active(),
         * inside any_other_zone_profile_active()) only while s_at.lock is
         * NOT held, because profile_executor.c's sweep_unowned_relays()
         * (called with s_exec.lock held) calls autotune_engine_is_active_
         * on_zone() (s_at.lock) the other way round -- see profile_
         * executor.c's own doc comment on that ordering, and any_other_
         * zone_profile_active()'s comment here for the full account of the
         * defect this replaced (querying live from INSIDE autotune_engine_
         * tick_locked(), i.e. while s_at.lock was already held).
         *
         * Reads s_at.zone_index without the lock -- tolerated because it is
         * write-once for the duration of a run (only autotune_begin_run_locked()
         * ever changes it, and only between runs); a momentarily stale read
         * here affects at most one tick's neighbour hint, corrected the
         * very next tick, and this whole computation is simply discarded
         * below whenever the engine turns out not to be running at all. */
        bool other_zone_active_hint = any_other_zone_profile_active(s_at.zone_index);

        xSemaphoreTake(s_at.lock, portMAX_DELAY);
        bool not_running = !state_is_running(s_at.state);
        if (!not_running) {
            s_at.other_zone_profile_active_hint = other_zone_active_hint;
            autotune_engine_tick_locked();
        }
        xSemaphoreGive(s_at.lock);

        if (not_running) {
            /* Backstop, same shape and reasoning as profile_executor.c's in
             * its own not-RUNNING branch: a state that is not running must
             * not be holding the safety processor's permission to heat,
             * whether or not the transition that got here remembered to
             * release it. Sends at most one frame -- heat_enable_release()
             * only puts anything on the wire on the last-claimant edge.
             * 2026-09-15 review of 059a896e, MEDIUM-4: moved outside
             * s_at.lock for consistency with the acquire side above, even
             * though heat_enable_release() itself only defers bookkeeping
             * and never blocks -- see heat_enable.c. */
            heat_enable_release_backstop(HEAT_ENABLE_CLAIMANT_AUTOTUNE);
            continue;
        }
    }
}

esp_err_t autotune_engine_start(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                                SafetyLinkClass *safety_or_null)
{
    memset(&s_at, 0, sizeof(s_at));
    s_at.io = io_or_null;
    s_at.thermo_bus = thermo_bus_or_null;
    s_at.safety = safety_or_null;
    s_at.state = AUTOTUNE_ENGINE_IDLE;

    s_at.lock = xSemaphoreCreateMutex();
    if (!s_at.lock) {
        return ESP_ERR_NO_MEM;
    }

    /* Same priority as profile_executor's control task (5) -- the two never
     * run their control logic simultaneously (mutual exclusion in
     * autotune_engine_run()/profile_executor_run()), so there's no
     * starvation concern between them; both still sit below the link-loss
     * watchdog (6). */
    /* 2026-08-22: PSRAM stack -- audited against uart_bridge_ext.c's
     * cache-disable hazard (see that file's boot-time comment). At the time
     * of this audit task_entry() never touched flash/NVS itself
     * (autotune_engine_accept()'s zones_http writes run on whichever task
     * calls it, not this one) and reached hardware only through
     * thermo_owner_task/kiln_io_owner_task's queues (owner tasks keep their
     * own internal stacks; the caller-owned result struct they write into
     * being in PSRAM is a plain memory store, not a DMA target). Safe to
     * move off internal SRAM, which several other tasks are contending for
     * during the WiFi-driver boot-time buffer storm that same comment
     * documents.
     *
     * 2026-08-31 UPDATE -- that "never touches flash/NVS" premise broke:
     * autotune_finalize_fit() gained a direct NVS-writing call this session
     * (zones_config_set_coupling_cell(), to persist a step test's measured
     * cross-zone coupling) and it panicked the board on hardware the first
     * time it ran for real -- see autotune_finalize_fit()'s own comment for the
     * coredump. Fixed there by routing that write through bx_flash_worker
     * (uart_bridge_ext_run_on_flash_worker()) rather than moving this task's
     * stack back to internal SRAM, which would reopen the WiFi-driver
     * internal-DRAM race this comment's first half describes. This task's
     * stack stays in PSRAM; task_entry() itself must still never call
     * anything that reaches flash/NVS directly -- route it through the
     * worker instead, same as autotune_finalize_fit() now does. */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(task_entry, "autotune_engine", 4096, NULL, 5, &s_at.task,
                                                    tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        vSemaphoreDelete(s_at.lock);
        s_at.lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* DRAM_PSRAM_PLAN.md Phase 0 (4.2): registration only, no size change --
     * only reached with a real handle since the failure branch above already
     * returned. 4096 must match the xTaskCreatePinnedToCoreWithCaps() literal
     * above. */
    stack_margin_register("autotune_engine", &s_at.task, 4096);
    ESP_LOGI(AT_TAG, "autotune engine up -- step test (default) and relay-feedback methods, see autotune_engine.h "
                  "for scope; neither has ever produced a fit on real hardware");
    return ESP_OK;
}

/* Everything both methods must do before either can start: prove the zone is
 * ours to drive, prove nothing else is driving it, and arm the heater window
 * and the FULL guard suite from that zone's own configuration.
 *
 * The guard configuration is built identically for both methods, and that is
 * deliberate and load-bearing: a relay test drives real elements around a real
 * setpoint, so it is the run that most needs guards 1-8, not the one that can
 * afford a relaxed set. Nothing here consults `method`.
 *
 * Returns with s_at.lock HELD on success -- the caller then fills in the
 * method-specific fields and sets the initial state under the same lock, so
 * the tick task can never observe a half-configured run. Returns false with
 * the lock NOT held on failure. */
bool autotune_begin_run_locked(uint8_t zone_index, char *err_msg, size_t err_cap)
{
    /* THE READINESS INTERLOCK (owner decision 2026-09-09; readiness_gate.h has
     * the full rationale and the standing "NO OVERRIDE" instruction). Extended
     * to autotune the same day, same owner decision: autotune commands heat
     * through the same relays a firing does, and until this point
     * profile_executor_run() was the only gated entry point -- an unverified
     * E-stop or an unacknowledged crash report blocked a firing but not an
     * autotune run.
     *
     * Placed HERE, in autotune_begin_run_locked(), because this is the single
     * choke point every autotune start path funnels through: the step-test
     * entry (autotune_engine_run()), the target-temperature step entry
     * (autotune_engine_run_to_target()) and the relay-feedback entry
     * (autotune_engine_run_relay(), autotune_engine_relay.c) all call this
     * function before doing anything else, exactly the way
     * profile_executor_run() is the one choke point for a firing. The two
     * live callers of those three functions are POST /api/autotune/start
     * (dashboard_autotune_http.c) and the benchproto AUTOTUNE command
     * (uart_bridge_ext_autotune.c) -- there is no LCD autotune-start button
     * today, so those two are the whole set; the next one added inherits this
     * gate for free by calling through the same three functions rather than
     * autotune_begin_run_locked() directly.
     *
     * Checked FIRST -- before even the s_at.lock == NULL guard below -- for
     * the same two reasons profile_executor_run() checks it first: the answer
     * must not depend on which zone or method was asked for, and in recovery
     * mode s_at.lock IS NULL, so reaching the gate first means a recovery-mode
     * refusal names recovery mode instead of "autotune engine not started".
     * Nothing here touches s_at, so running before that guard is safe. */
    {
        readiness_gate_block_t which = READINESS_GATE_OK;
        if (readiness_gate_refuses_start(err_msg, err_cap, &which)) {
            ESP_LOGW(AT_TAG, "autotune begin_run(zone %u) refused by the readiness interlock (item %d): %s",
                     (unsigned)zone_index, (int)which, err_msg ? err_msg : "(no message)");
            return false;
        }
    }

    /* Recovery mode (boot_guard.h) deliberately skips autotune_engine_start()
     * -- but other code that DOES still run in that mode can still call into
     * this module's public API. s_at.lock is NULL until
     * autotune_engine_start() creates it, and taking a NULL FreeRTOS mutex
     * asserts (the exact failure profile_executor.c hit on the bench --
     * safety_poll_task -> ... -> xQueueSemaphoreTake() -> "assert failed:
     * (( pxQueue ))" -> panic; see that file for the full backtrace). Every
     * public entry point below tests it first and returns a clean
     * "not running" answer instead of touching s_at at all. This covers both
     * autotune_engine_run() and autotune_engine_run_relay(), which both
     * funnel through here before taking the lock themselves. */
    if (s_at.lock == NULL) {
        ESP_LOGW(AT_TAG, "autotune begin_run() called before autotune_engine_start() -- refused");
        if (err_msg) snprintf(err_msg, err_cap, "autotune engine not started");
        return false;
    }

    /* Direction B of the mutual OTA interlock (see profile_executor_run()'s
     * identical check and ota_http.h's doc comment above
     * ota_http_heat_blocked_by_update()): refuse to start EITHER autotune
     * method while an update is in progress on either processor. Checked
     * first, before any zone-config/relay-mask state, for the same
     * "cheapest and orthogonal" reasoning ota_interlock_check() documents
     * for its own mutex check. */
    if (ota_http_heat_blocked_by_update(err_msg, err_cap)) {
        return false;
    }

    /* B2 (opus review, 2026-08-27): same reverse interlock as
     * profile_executor.c's identical check -- see
     * zones_current_sweep_is_active()'s doc comment (zones_http.h). Checked
     * here, right after the OTA check above, for the same reasoning. */
    if (zones_current_sweep_is_active()) {
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "a zone current sweep is running -- autotune cannot run at the same time");
        }
        return false;
    }

    /* Refuse up front if heat is blocked at all -- most importantly a safety
     * link that is down or faulted. Every heat command this engine issues is
     * already gated (see apply_duty()'s relay_authority_zone_blocked() call),
     * so a run started this way was never DANGEROUS: the relays simply never
     * closed. It was dishonest, which is its own problem. Observed on the
     * bench: with no safety processor answering, POST /api/autotune/start
     * returned {"ok":true} and the engine sat in "settling" indefinitely,
     * heating nothing and explaining nothing, while the same condition makes
     * a manual relay-ON return a 403 that says exactly what is wrong and
     * makes a profile abort with "safety processor link silent". An autotune
     * that cannot heat should say so at the point the operator asks for it,
     * in the same words. */
    {
        uint32_t sources = 0;
        if (relay_authority_on_blocked(s_at.safety, &sources)) {
            if (err_msg) {
                /* Kept under 128 chars: dashboard_http.c's autotune handler
                 * passes a char[128], and the first draft of this message was
                 * truncated mid-word on the page ("...so it w"). ROADMAP.md
                 * M13: decode the mask instead of showing a bare hex value --
                 * same shortening (first source + "(+more)") zones_http.c's
                 * ZONE_SWEEP_ZONE_ENERGIZE_REFUSED case already uses, since
                 * the full comma-joined safety_fault_source_words() sentence
                 * can run to 141 bytes on its own. */
                char src_words[160];
                safety_fault_source_words(sources, src_words, sizeof(src_words));
                char *comma = strchr(src_words, ',');
                bool more = (comma != NULL);
                if (comma != NULL) {
                    *comma = '\0';
                }
                snprintf(err_msg, err_cap,
                         "heat is blocked (%.32s%s, usually the safety link down) -- "
                         "autotune cannot drive the element",
                         src_words, more ? " (+more)" : "");
            }
            return false;
        }
    }

    /* TODO.md 8.2 "Tie it to the guards, not only the UI": same explicit
     * refusal as profile_executor_run() -- must not rely on the relay_mask
     * check below happening to read 0 for a failed-to-load config too. */
    if (!zones_config_is_valid()) {
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "zone config failed to load or has not been saved -- autotune cannot run until "
                     "zone config loads cleanly (see /settings/zones)");
        }
        return false;
    }
    uint8_t mask = 0;
    if (!zones_config_get_relay_mask(zone_index, &mask) || mask == 0) {
        if (err_msg) snprintf(err_msg, err_cap, "zone has no relay mask configured");
        return false;
    }

    /* Autotune readiness audit (docs/audits/autotune_readiness_2026-09-07.md,
     * item 3): a zone with no thermocouple channel assigned (thermo_mask ==
     * 0) is not caught by any existing prestart check -- zones_config_
     * is_valid() only confirms the config blob loaded, and the relay_mask
     * check above is a completely independent field. Left unchecked, such a
     * zone sails through autotune_begin_run_locked() and even through
     * check_thermal_readiness_locked()'s SETTLING-phase gate, because that
     * function's own "cannot judge it, so it cannot block the run" degrade
     * path (see its doc comment) skips exactly this zone's own row whenever
     * ok_by_zone[zone_index] is false -- which it always is here, since
     * thermo_combine() over an empty mask can never produce a valid reading.
     * The run then proceeds to STEPPING with actual_valid permanently false,
     * sensor_ok gating every commanded duty to 0.0f (see the thermal_guard_
     * input_t.commanded_duty comment below), and drives nothing for the
     * FULL step-test budget (up to AUTOTUNE_ENGINE_WHOLE_RUN_MAX_DURATION_S,
     * hours) before autotune_finalize_fit() finally refuses the trace as
     * "flat or noise-dominated" -- exactly the wasted-tune shape this refusal
     * exists to prevent cheaply, up front, instead of hours in. Checked here,
     * not only in the tick, for the same "refuse before any heating starts"
     * convention as every other check in this function. */
    /* docs/ON_OFF_ZONE_PLAN.md sec 1/step 1: refuse an on/off zone BEFORE
     * the thermo_mask check just below, same "refuse before any heating"
     * convention as every other check in this function -- a TC-equipped
     * on/off zone (legal per plan sec 2) must get THIS message, not sail
     * past the thermo_mask check only to fail later on a flat trace that
     * looks identical to a genuinely dead element. An on/off zone is never a
     * heat source (plan sec 1's "one rule governs everything"): autotune has
     * no step response to identify on it at all. */
    if (zone_is_on_off(zone_index)) {
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "zone %u is an on/off device, not a heater -- autotune has nothing to identify",
                     zone_index);
        }
        return false;
    }

    uint8_t tmask = 0;
    if (!zones_config_get_thermo_mask(zone_index, &tmask) || tmask == 0) {
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "zone %u has no thermocouple channel assigned -- autotune cannot measure a "
                     "response with no sensor to read",
                     zone_index);
        }
        return false;
    }

    /* TODO.md 6A.5: per-zone, not "any profile anywhere" -- a profile
     * running on a *different* zone no longer blocks autotune here, now
     * that concurrent multi-zone execution exists. Zones must still be
     * autotuned one at a time relative to each OTHER autotune run (this
     * engine is a single global instance, see autotune_engine_run()'s own
     * SETTLING/STEPPING check below), and never on a zone a profile is
     * actively driving. */
    if (profile_executor_zone_is_active(zone_index)) {
        if (err_msg) {
            snprintf(err_msg, err_cap, "a profile is running on zone %u -- autotune cannot run on it at "
                                       "the same time",
                     zone_index);
        }
        return false;
    }

    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    if (state_is_running(s_at.state)) {
        xSemaphoreGive(s_at.lock);
        if (err_msg) snprintf(err_msg, err_cap, "autotune already running on zone %u", s_at.zone_index);
        return false;
    }

    /* iter_tune_http.c restore_commissioned race close (step-7 review,
     * 2026-09-23): refuse to start on a zone iter_tune_http.c's handler has
     * reserved via autotune_engine_reserve_zone_for_external_write() -- see
     * that function's and s_at_t's own comments. Checked under the SAME
     * lock and at the SAME commit point as the state_is_running() check just
     * above, so a reservation held right now can never be raced by a start
     * that reads "not running" a moment before the reservation was taken. */
    /* iter_tune_http.c restore_commissioned race close (step-7 review,
     * 2026-09-23): refuse to start on a zone iter_tune_http.c's handler has
     * reserved via autotune_engine_reserve_zone_for_external_write() -- see
     * that function's and s_at_t's own comments. Checked under the SAME
     * lock and at the SAME commit point as the state_is_running() check just
     * above, so a reservation held right now can never be raced by a start
     * that reads "not running" a moment before the reservation was taken. */
    if (s_at.external_write_reserved && s_at.external_write_reserved_zone == zone_index) {
        xSemaphoreGive(s_at.lock);
        if (err_msg) {
            snprintf(err_msg, err_cap, "an iter_tune restore is in progress on zone %u", (unsigned)zone_index);
        }
        return false;
    }

    /* The atomic gate (relay_authority.h's heat-claim doc comment): the
     * zones_current_sweep_is_active() check above is a plain, non-atomic
     * read made before s_at.lock was even taken -- a sweep can start in the
     * window between that read and this function's commit. This is the last
     * possible moment before the commit: s_at.lock has been held
     * continuously since state_is_running() just above confirmed this is a
     * genuine start, not a reentrant call on an already-running instance
     * (see relay_heat_zone_claimant_t's doc comment for why that ordering is
     * what makes force_relays_off()'s unconditional _end() call safe), and
     * it is a single mutex-protected test-and-set against zones_http.c's/
     * profile_executor.c's matching gates. Refused with the SAME message
     * the early check already reports for the common (non-race) case. */
    if (!relay_authority_heat_zone_claim_begin(RELAY_HEAT_ZONE_CLAIM_AUTOTUNE)) {
        xSemaphoreGive(s_at.lock);
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "a zone current sweep is running -- autotune cannot run at the same time");
        }
        return false;
    }

    s_at.zone_index = zone_index;
    s_at.trace_count = 0;
    s_at.abort_reason[0] = '\0';
    s_at.per_zone_blocked = false;

    /* TODO.md 6A.6: claim this zone's relays for the duration of the run, same
     * shape as profile_executor.c's RELAY_OWNER_PROFILE claim -- see
     * force_relays_off()'s matching release and relay_authority.h's
     * RELAY_OWNER_AUTOTUNE doc comment for why this exists. `mask` was already
     * validated non-zero above (before this function took the lock), so this
     * cannot silently claim nothing. */
    /* Clear a per-zone block latched by an EARLIER run's guard trip, exactly
     * as profile_executor.c's clear_this_runs_faults() does when the operator
     * starts a new firing -- the same gesture ("run heat on this zone") after
     * the same kind of trip, so it gets the same policy rather than a new one.
     *
     * Without this the latch outlived the firing that set it and there was no
     * way out but a reboot or another firing: a bench guard-1 trip left zone 0
     * blocked, and the autotune started right afterwards settled for three
     * minutes, drove the relay 0 times out of 249 samples, and concluded
     * "response too small to fit (trace flat or noise-dominated)" -- reading
     * as a verdict on the kiln. Logged loudly because clearing a guard trip is
     * never a detail. */
    if (relay_authority_zone_latched_blocked(zone_index)) {
        ESP_LOGW(AT_TAG, "zone %u was still blocked by an earlier guard trip -- clearing it because the "
                      "operator asked for an autotune on this zone",
                 zone_index);
        relay_authority_set_zone_blocked(zone_index, false);
    }
    /* Same gesture, same policy, for a GLOBAL guard trip an earlier run left
     * asserted -- see autotune_escalate_and_abort()'s `global` branch and
     * global_fault_source's own comment. Unlike the per-zone latch above,
     * this one is not scoped to `zone_index`: a global trip blocks every
     * zone's relay-ON board-wide (relay_authority_on_blocked(), not the
     * per-zone check), so the operator starting ANY autotune after one is
     * exactly the "run heat again after a trip" gesture profile_executor.c's
     * clear_this_runs_faults() treats as consent to clear it. Logged loudly
     * for the same reason the per-zone case is: clearing a guard trip is
     * never a detail. */
    if (s_at.global_fault_source != 0) {
        ESP_LOGW(AT_TAG, "fault source 0x%02X was still asserted by an earlier autotune's guard trip -- "
                      "clearing it because the operator started a new autotune (zone %u)",
                 (unsigned)s_at.global_fault_source, zone_index);
        if (s_at.safety) {
            esp_err_t err = safety_link_set_fault_source(s_at.safety, s_at.global_fault_source, false);
            if (err != ESP_OK) {
                ESP_LOGE(AT_TAG, "clearing fault source 0x%02X failed: %s", (unsigned)s_at.global_fault_source,
                         esp_err_to_name(err));
            }
        }
        s_at.global_fault_source = 0;
    }
    relay_authority_claim_mask(mask, RELAY_OWNER_AUTOTUNE);
    /* Both results are fully cleared at the start of every run, whichever
     * method follows: exactly one of them will be filled in.
     *
     * Fixed 2026-08-31 (found alongside the autotune-instrumentation work):
     * this used to clear ONLY the `valid` flag ("a stale `valid` from the
     * previous run would let autotune_engine_accept() write gains this run
     * never produced"), leaving k_gain_c_per_duty/tau_s/dead_time_s/
     * invalid_reason (and relay's ku/tu_s/amplitude_c/cycles_used) holding
     * the PREVIOUS run's numbers -- the struct is otherwise only zeroed once,
     * at boot (autotune_engine_start()'s memset). autotune_engine_get_status()
     * copies s_at.model/s_at.relay into its output whenever state == DONE,
     * and dashboard_http.c's /api/autotune serializes k_gain_c_per_duty etc.
     * unconditionally alongside model_valid, so those numbers are only
     * "discounted" by a caller that actually checks the valid flag first --
     * exactly the same producer/consumer reset-one-side shape as the
     * refusal/refusal_reason bug this function already guards against just
     * below. A zeroed struct with valid=false is indistinguishable from the
     * boot-time "never tuned yet" state, which is the correct thing for a
     * run in progress to report. */
    s_at.model = (fopdt_model_t){0};
    s_at.relay = (relay_model_t){0};
    s_at.relay_cycles_seen = 0;
    /* And the gains those results produce, for the same reason one step
     * further out: /api/autotune serializes proposed_gains.refusal and
     * .refusal_reason (2026-08-30), and unlike kp/ki/kd those have no
     * companion `valid` flag the page can use to discount them. Left
     * un-cleared, a run that refused ("dead time 3.0s is below the 5.0s
     * Cohen-Coon needs", say) would keep reporting that verdict through the
     * whole of the NEXT run -- and permanently, if that run aborts before
     * autotune_finalize_fit()/finalize_relay_fit() overwrites it. Zero is
     * AUTOTUNE_REFUSAL_OK with an empty reason, i.e. exactly the
     * never-tuned-yet state autotune_engine_start()'s memset() leaves. */
    s_at.proposed_gains = (autotune_gains_t){0};
    s_at.predicted_max_ramp_c_per_hr = 0.0f;
    s_at.predicted_max_ramp_ambient_c_per_hr = 0.0f;
    /* Target-mode fields default off/zero for every run; autotune_engine_
     * run_to_target() sets target_mode/probe_phase/target_c right after this
     * function returns, exactly as it already does for method/step_duty --
     * see that function. Without this reset a plain autotune_engine_run()
     * following an earlier target-mode run would inherit its stale
     * target_mode=true and route STEPPING's end through handle_probe_done_
     * locked() instead of autotune_finalize_fit(). */
    s_at.target_mode = false;
    s_at.probe_phase = false;
    s_at.target_c = 0.0f;
    s_at.probe_k_rough = 0.0f;
    s_at.target_achieved_c = 0.0f;
    /* Same staleness class as target_achieved_c immediately above: only set
     * once STEPPING is reached (~line 2102), so a run whose SETTLING phase
     * is polled before that would otherwise report the PREVIOUS run's
     * cold-junction reading as this run's. */
    s_at.step_ambient_c = 0.0f;
    /* Review finding 5: these two used to be reset ONLY at the SETTLING->
     * STEPPING transition, so a brand-new run's SETTLING phase carried the
     * PREVIOUS run's true step_element_proven through it -- benign only
     * because SETTLING always commands duty 0 (guard 1's climbing branch
     * never evaluates there regardless), but stale safety state surviving a
     * run boundary for no reason. Reset here too so it never happens. */
    s_at.step_element_proven = false;
    s_at.step_rise_running_max_c = 0.0f;
    s_at.step_below_death_floor_ticks = 0u;
    s_at.probe_last_k_c_per_duty = 0.0f;
    s_at.probe_stable_checks = 0u;
    /* Same staleness class -- a new run's first SETTLING tick must capture
     * its OWN start-of-settle reading, never inherit the previous run's. */
    s_at.readiness_start_captured = false;
    /* Same reasoning -- task_entry() sets a fresh value every tick before
     * calling autotune_engine_tick_locked(), but reset here too so a new
     * run's very first tick (before task_entry() has run its own pre-lock
     * computation for THIS run) never reads a previous run's true. */
    s_at.other_zone_profile_active_hint = false;

    float window_ms = 0.0f, min_on_ms = 0.0f, min_off_ms = 0.0f;
    zones_config_get_heater_cfg(zone_index, &window_ms, &min_on_ms, &min_off_ms);
    s_at.heater_cfg = (heater_output_cfg_t){
        .window_ms = (window_ms > 0.0f) ? (uint32_t)window_ms : HEATER_WINDOW_MS,
        .min_on_ms = (min_on_ms > 0.0f) ? (uint32_t)min_on_ms : HEATER_MIN_ON_MS,
        .min_off_ms = (min_off_ms > 0.0f) ? (uint32_t)min_off_ms : HEATER_MIN_OFF_MS,
    };
    heater_output_reset(&s_at.heater_state);

    float max_temp_c = 0.0f, min_temp_c = -20.0f, sanity_rate = 0.0f;
    zones_config_get_temp_limits(zone_index, &max_temp_c, &min_temp_c);
    zones_config_get_sanity_rate(zone_index, &sanity_rate);
    /* TODO.md 6A.3's remaining named thresholds -- an autotune run arms the
     * full thermal_guard suite (this file's own doc note) exactly like a
     * firing does, so it must honour the same per-zone overrides a firing
     * would, not fall back to firmware-wide constants a running profile no
     * longer uses. Raw pass-through, same reasoning as profile_executor.c's
     * identical block: thermal_guard.c owns the 0->default substitution. */
    float wd_window_s = 0.0f, wd_rate = 0.0f, off_settle_s = 0.0f, runaway_rate = 0.0f;
    float runaway_margin = 0.0f, drift_period_s = 0.0f, debounce_ticks = 0.0f, frozen_window_s = 0.0f;
    zones_config_get_guard_thresholds(zone_index, &wd_window_s, &wd_rate, &off_settle_s, &runaway_rate,
                                      &runaway_margin, &drift_period_s, &debounce_ticks, &frozen_window_s);
    /* Guard 1's arrival band (ZONES_CFG_VERSION 21->22, docs/audits/
     * consumer_without_producer_2026-09-06.md finding 1) -- same reasoning
     * as profile_executor_run.c's identical block: this getter already does
     * the 0->default substitution, so the value here is final. An autotune
     * run arms the full thermal_guard suite exactly like a firing does (this
     * file's own doc note above), so it must see the same per-zone override
     * a firing would, not silently fall back to the firmware-wide constant. */
    float progress_band_c = 0.0f;
    zones_config_get_progress_band_c(zone_index, &progress_band_c);
    s_at.guard_cfg = (thermal_guard_cfg_t){
        .max_temp_c = max_temp_c, .min_temp_c = min_temp_c,
        .sanity_rate_c_per_min = (sanity_rate > 0.0f) ? sanity_rate : PROFILE_EXECUTOR_DEFAULT_SANITY_RATE_C_PER_MIN,
        .wrong_dir_window_s = wd_window_s,
        .wrong_dir_rate_c_per_min = wd_rate,
        .off_settle_s = off_settle_s,
        .runaway_rate_c_per_min = runaway_rate,
        .runaway_margin_c = runaway_margin,
        .drift_period_s = drift_period_s,
        .sensor_fault_debounce_ticks = debounce_ticks,
        .frozen_window_s = frozen_window_s,
        .progress_band_c = progress_band_c,
    };
    thermal_guard_reset(&s_at.guard_state);

    /* Ask the safety processor to permit heating -- i.e. close K4. Missing
     * until 2026-08-29 (see heat_enable.h), and NOT done here since the
     * 2026-09-15 adversarial review of 059a896e (MEDIUM-4): this call used to
     * happen right here, under s_at.lock, and heat_enable_acquire() can now
     * run a blocking link exchange of its own (HIGH-1's he_flush_release_
     * blocking(), ~5.5s worst case) -- holding s_at.lock across that is
     * exactly the "never hold a module lock across a blocking call" rule this
     * file's own lock-order comment elsewhere enforces. Every caller of
     * autotune_begin_run_locked() (autotune_engine_run(), autotune_engine_
     * run_to_target()) now calls heat_enable_acquire() itself, AFTER its own
     * xSemaphoreGive(s_at.lock) -- see each call site's comment.
     *
     * The return is still not checked there, and that is not the
     * danger_mode.c mistake repeated: the ONLY failure is a down safety link,
     * which relay_authority_on_blocked() above already refused this start
     * over, and heat_enable_reconcile() (profile_executor.c's watchdog task)
     * retries a link that drops and returns mid-run. heat_enable.c logs it
     * loudly. */

    TickType_t now = xTaskGetTickCount();
    s_at.phase_start_tick = now;
    s_at.prev_tick = now;
    /* Set ONCE per run, never touched again -- see
     * AUTOTUNE_ENGINE_WHOLE_RUN_MAX_DURATION_S's own comment. */
    s_at.run_start_tick = now;
    /* Lock stays held -- caller sets method, method-specific fields, state. */
    return true;
}

bool autotune_engine_run(uint8_t zone_index, float step_duty, autotune_rule_t rule, char *err_msg, size_t err_cap)
{
    if (!(step_duty > 0.0f) || step_duty > 1.0f) {
        if (err_msg) snprintf(err_msg, err_cap, "step_duty must be in (0, 1]");
        return false;
    }
    if (rule != AUTOTUNE_RULE_SIMC && rule != AUTOTUNE_RULE_COHEN_COON) {
        /* ZN/Tyreus-Luyben are relay-only -- pid_autotune_tune_from_fopdt()
         * already refuses them (AUTOTUNE_REFUSAL_RULE_NOT_ON_THIS_PATH), but
         * catching it here avoids running a full step test just to hand the
         * operator zero gains at the end of it, same reasoning as the relay
         * path's SIMC rejection above. */
        if (err_msg) snprintf(err_msg, err_cap, "step-test rule must be SIMC or Cohen-Coon");
        return false;
    }
    /* Review blocker 1: refused BEFORE any heating starts, same convention
     * as every other pre-autotune_begin_run_locked() refusal -- see
     * any_other_zone_profile_active()'s own comment for why isolation, not
     * attribution, is this file's fix. */
    if (any_other_zone_profile_active(zone_index)) {
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "a profile is running on another zone -- a step test's element-alive detection "
                     "cannot tell that zone's heat from this one's");
        }
        return false;
    }
    if (!autotune_begin_run_locked(zone_index, err_msg, err_cap)) {
        return false;
    }

    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.step_duty = step_duty;
    s_at.step_rule = rule;
    /* Review finding 3: thermal_guard.c's default progress_duty_min (0.5)
     * leaves guards 1/2 completely inert below that duty -- every step test
     * arms them explicitly at any commanded duty > 0 instead. See
     * AUTOTUNE_PROGRESS_DUTY_MIN_FOR_STEP_TEST's own comment. Relay method
     * is untouched -- set here, not in autotune_begin_run_locked(), because that
     * function runs before the caller has chosen a method. */
    s_at.guard_cfg.progress_duty_min = AUTOTUNE_PROGRESS_DUTY_MIN_FOR_STEP_TEST;
    s_at.state = AUTOTUNE_ENGINE_SETTLING;
    bool no_ceiling = !(s_at.guard_cfg.max_temp_c > 0.0f);
    uint32_t he_epoch = heat_enable_claim_epoch(HEAT_ENABLE_CLAIMANT_AUTOTUNE);
    xSemaphoreGive(s_at.lock);

    /* 2026-09-15 review of 059a896e, MEDIUM-4: moved out from under s_at.lock
     * (see autotune_begin_run_locked()'s comment) -- this can now block for
     * seconds. Which is exactly why the epoch above is sampled while the lock
     * is still held: an abort landing in that window must make this refuse,
     * not re-claim heat for a run that has already stopped (heat_enable.h). */
    (void)heat_enable_acquire_since(HEAT_ENABLE_CLAIMANT_AUTOTUNE, he_epoch);

    ESP_LOGI(AT_TAG, "autotune zone %u starting: settling %us at duty 0 before stepping to %.2f", zone_index,
             AUTOTUNE_ENGINE_SETTLE_S, (double)step_duty);
    if (no_ceiling) {
        /* Not a refusal (see the guard-fallback comment at this run's
         * thermal_guard_input_t construction) -- guards 1/2 stay fully
         * covered without a ceiling. Guard 4 (drift near the ceiling) does
         * not, though, and this is the one place that's ever surfaced: the
         * failure mode must be visible, not just silently absorbed the way
         * the old error==0 fallback used to hide it. */
        ESP_LOGW(AT_TAG, "autotune zone %u: no max_temp_c configured -- guard 4 (drift near ceiling) has "
                      "nothing to compare against for this run; guards 1/2/5/6 are unaffected",
                 zone_index);
    }
    return true;
}

bool autotune_engine_run_to_target(uint8_t zone_index, float target_c, autotune_rule_t rule, char *err_msg,
                                   size_t err_cap)
{
    if (rule != AUTOTUNE_RULE_SIMC && rule != AUTOTUNE_RULE_COHEN_COON) {
        /* Same restriction as autotune_engine_run() -- this is still a step
         * test underneath, just with the duty chosen for the caller. */
        if (err_msg) snprintf(err_msg, err_cap, "step-test rule must be SIMC or Cohen-Coon");
        return false;
    }

    /* Ceiling validation happens here, BEFORE autotune_begin_run_locked() is even
     * called -- i.e. strictly before any heating. (autotune_begin_run_locked()'s own
     * SETTLING phase drives duty 0, but this check must refuse before that
     * phase is even entered, per "validated ... before any heating starts".)
     * zones_config_get_temp_limits() is a cheap cached-config read;
     * autotune_begin_run_locked() reads the same config again just below for the
     * guard suite it arms -- reading it twice here is simpler than
     * restructuring autotune_begin_run_locked() to hand this back out, and nothing
     * can change max_temp_c between the two reads (no run is active yet,
     * and this function is the only writer of a NEW run's target_c). */
    float max_temp_c = 0.0f, min_temp_c = -20.0f;
    zones_config_get_temp_limits(zone_index, &max_temp_c, &min_temp_c);

    if (!(target_c > 0.0f)) {
        /* target_c <= 0 means "use the default": AUTOTUNE_ENGINE_TARGET_
         * DEFAULT_FRACTION of max_temp_c. ZERO-SEMANTICS TRAP, same
         * convention as every other max_temp_c check in this file:
         * max_temp_c == 0 means the ceiling guard is DISABLED for this
         * zone, not "the ceiling is zero degrees" -- there is therefore no
         * usable maximum to take a fraction of, and 0.75 * 0 would silently
         * hand back a target of 0C. Refuse and require an explicit target
         * instead of inventing a fallback ceiling of our own. */
        if (!(max_temp_c > 0.0f)) {
            if (err_msg) {
                snprintf(err_msg, err_cap,
                         "zone %u has no max_temp_c configured -- cannot derive a default target, specify "
                         "one explicitly",
                         zone_index);
            }
            return false;
        }
        target_c = AUTOTUNE_ENGINE_TARGET_DEFAULT_FRACTION * max_temp_c;
    }

    /* Validated against the ceiling with margin whether target_c was given
     * explicitly or just defaulted above -- the default is comfortably
     * inside this margin (75% vs. this check's headroom requirement) but
     * still goes through the SAME check, not a bypass, so a future change to
     * either constant cannot silently reopen the gap. */
    if (max_temp_c > 0.0f && target_c >= max_temp_c - AUTOTUNE_ENGINE_TARGET_CEILING_MARGIN_C) {
        if (err_msg) {
            snprintf(err_msg, err_cap, "target %.1fC too close to zone ceiling %.1fC", (double)target_c,
                     (double)max_temp_c);
        }
        return false;
    }

    /* Review blocker 1 -- same refusal as autotune_engine_run()'s, see
     * any_other_zone_profile_active()'s own comment. Checked here too
     * (target mode's probe phase is exactly the scenario the blocker's
     * measured-hardware numbers describe). */
    if (any_other_zone_profile_active(zone_index)) {
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "a profile is running on another zone -- a step test's element-alive detection "
                     "cannot tell that zone's heat from this one's");
        }
        return false;
    }

    if (!autotune_begin_run_locked(zone_index, err_msg, err_cap)) {
        return false;
    }

    s_at.method = AUTOTUNE_METHOD_STEP;
    s_at.step_rule = rule;
    s_at.target_mode = true;
    s_at.probe_phase = true;
    s_at.target_c = target_c;
    s_at.probe_k_rough = 0.0f;
    s_at.step_duty = AUTOTUNE_ENGINE_PROBE_DUTY; /* PHASE 1: probe, not the real identification duty yet */
    /* Same override as the plain duty path -- see its own comment. Target
     * mode's own duties (0.15 probe, typically well under 0.5 identify too)
     * are exactly the case this exists for. */
    s_at.guard_cfg.progress_duty_min = AUTOTUNE_PROGRESS_DUTY_MIN_FOR_STEP_TEST;
    s_at.state = AUTOTUNE_ENGINE_SETTLING;
    uint32_t he_epoch = heat_enable_claim_epoch(HEAT_ENABLE_CLAIMANT_AUTOTUNE);
    xSemaphoreGive(s_at.lock);

    /* 2026-09-15 review of 059a896e, MEDIUM-4: moved out from under s_at.lock
     * (see autotune_begin_run_locked()'s comment) -- this can now block for
     * seconds. Epoch sampled under the lock for the same reason as the plain
     * duty path above -- see its comment and heat_enable.h. */
    (void)heat_enable_acquire_since(HEAT_ENABLE_CLAIMANT_AUTOTUNE, he_epoch);

    ESP_LOGI(AT_TAG, "autotune zone %u starting target-temperature run: settling %us before probing at duty "
                  "%.2f, target %.1fC",
             zone_index, AUTOTUNE_ENGINE_SETTLE_S, (double)AUTOTUNE_ENGINE_PROBE_DUTY, (double)target_c);
    return true;
}

void autotune_engine_get_status(autotune_engine_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    /* See autotune_begin_run_locked()'s guard comment above. The zeroed struct above
     * already reads as a well-formed IDLE snapshot (AUTOTUNE_ENGINE_IDLE ==
     * 0), so a caller here needs nothing more than "don't touch the NULL
     * lock". */
    if (s_at.lock == NULL) {
        LOG_PRESTART_ONCE("autotune_engine_get_status() called before autotune_engine_start() -- reporting IDLE");
        return;
    }
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    out->state = s_at.state;
    out->method = s_at.method;
    out->zone_index = s_at.zone_index;
    out->elapsed_s = s_at.elapsed_s;
    out->sample_count = s_at.trace_count;
    out->actual_c = s_at.actual_c;
    out->actual_valid = s_at.actual_valid;
    out->duty = s_at.duty;
    strncpy(out->abort_reason, s_at.abort_reason, sizeof(out->abort_reason) - 1);
    /* Relay parameters and cycle progress are reported in every state, not
     * just DONE: while cycling they are the only way for the operator to see
     * how much longer the kiln will be oscillating, and after an abort they
     * are what makes the abort reason interpretable. */
    out->relay_setpoint_c = s_at.relay_setpoint_c;
    out->relay_amplitude_duty = s_at.relay_d;
    out->relay_hysteresis_c = s_at.relay_h;
    out->relay_cycles_seen = s_at.relay_cycles_seen;
    out->relay_cycles_target = AUTOTUNE_RELAY_TARGET_CYCLES;
    /* Target mode's own progress, visible in every state (not just DONE) for
     * the same reason the relay fields above are: while probing/identifying
     * this is the only way a caller can show WHICH phase is running and WHY
     * a particular duty was picked, and after an abort probe_k_rough is
     * still meaningful diagnostic context for the abort_reason text. */
    out->target_mode = s_at.target_mode;
    out->probe_phase = s_at.probe_phase;
    out->target_c = s_at.target_c;
    out->probe_k_rough = s_at.probe_k_rough;
    /* Only meaningful once autotune_finalize_fit() has actually run on the
     * identification step (target_mode + a valid model) -- 0 otherwise,
     * same "only meaningful when ..." convention as model/relay below. */
    out->target_achieved_c = s_at.target_achieved_c;
    /* Insurance against a permanently leaked reservation -- see s_at_t's own
     * comment: there is no timeout, so the only way this can ever stay true
     * forever is a bug (a reserve() with no matching release()) or a crash
     * mid-window, and both are reboot-only conditions (this flag lives in
     * s_at, not NVS, so a reboot always clears it). Exposed unconditionally,
     * same convention as the other fields above, so a caller/operator has a
     * way to SEE a leak exists without a JTAG read. */
    out->external_write_reserved = s_at.external_write_reserved;
    out->external_write_reserved_zone = s_at.external_write_reserved_zone;
    /* See autotune_engine_status_t::step_ambient_c's own comment -- exposed
     * unconditionally like target_achieved_c above, not gated to DONE, so a
     * caller can see it was captured even while STEPPING is still running. */
    out->step_ambient_c = s_at.step_ambient_c;
    /* 2026-08-31 fix: this used to gate on state == DONE only, which erases
     * out->model (baseline_c/final_c/raw_rise_c/rise_inf_c/k_gain_c_per_duty
     * etc, all fields fopdt_model_t's own comment calls out as existing
     * specifically to make a rejected fit diagnosable) at the exact moment
     * they are needed most: autotune_finalize_fit()'s (B)/(C0)/(C) refusal paths all
     * populate s_at.model (pid_autotune_fit_fopdt() already ran, see
     * autotune_finalize_fit()'s very first call) BEFORE setting state = ABORTED and
     * returning, so the data exists in s_at.model the whole time -- it was
     * only this getter throwing it away on the way out. A real aborted run
     * confirmed this: every diagnostic field read 0.00 even though the abort
     * message's own numbers proved the fit had computed a real, non-zero
     * gain.
     *
     * This does NOT reintroduce the stale-payload bug autotune_begin_run_locked()'s
     * full-zero fix (commit 49d979d) exists to prevent: s_at.model is
     * zeroed there at the START of every new run (`s_at.model = (fopdt_
     * model_t){0}`, above), so a fresh run always begins with zeroed
     * diagnostics regardless of this getter. What changes here is only
     * whether THIS run's own diagnostics, once computed, are still handed
     * out after a reject -- exposing them on ABORTED, not clearing them
     * again on the way out, is exactly the "cleared at start, preserved on
     * this run's own reject" distinction the fix needs. relay/proposed_gains/
     * predicted_max_ramp_c_per_hr are included on ABORTED too, for the same
     * reason and because none of them carry stale-run risk either -- they
     * are only ever written by THIS run's own autotune_finalize_fit()/finalize_
     * relay_fit(), never left over from a previous one (autotune_begin_run_locked()
     * zeros s_at.proposed_gains/s_at.relay the same way). */
    if (s_at.state == AUTOTUNE_ENGINE_DONE || s_at.state == AUTOTUNE_ENGINE_ABORTED) {
        out->model = s_at.model;
        out->relay = s_at.relay;
        out->proposed_gains = s_at.proposed_gains;
        out->predicted_max_ramp_c_per_hr = s_at.predicted_max_ramp_c_per_hr;
        out->predicted_max_ramp_ambient_c_per_hr = s_at.predicted_max_ramp_ambient_c_per_hr;
    }
    xSemaphoreGive(s_at.lock);
}

size_t autotune_engine_get_trace(autotune_sample_t *out, size_t start_index, size_t max_entries)
{
    if (!out || max_entries == 0) return 0;
    /* See autotune_begin_run_locked()'s guard comment above. */
    if (s_at.lock == NULL) {
        LOG_PRESTART_ONCE("autotune_engine_get_trace() called before autotune_engine_start() -- refused");
        return 0;
    }
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    if (start_index >= s_at.trace_count) {
        xSemaphoreGive(s_at.lock);
        return 0;
    }
    size_t count = (size_t)s_at.trace_count - start_index;
    if (count > max_entries) count = max_entries;
    /* Trace is written oldest-first into a flat (non-ring) array during
     * STEPPING -- no modular indexing needed, unlike the history buffer.
     * This is the zone under test's own row; the other zones' rows (TODO.md
     * 6A.5(b)) aren't downloadable as CSV in this pass, only their fitted
     * models via autotune_engine_get_coupling_matrix(). */
    for (size_t i = 0; i < count; i++) {
        size_t idx = start_index + i;
        int16_t dc = s_at.zone_trace[s_at.zone_index][idx];
        out[i].t_s = (float)(idx * AUTOTUNE_ENGINE_SAMPLE_PERIOD_S);
        out[i].measurement_c = (dc == AUTOTUNE_TRACE_TEMP_INVALID) ? NAN : (float)dc / 10.0f;
    }
    xSemaphoreGive(s_at.lock);
    return count;
}

bool autotune_engine_is_active(void)
{
    /* See autotune_begin_run_locked()'s guard comment above. */
    if (s_at.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    bool active = state_is_running(s_at.state);
    xSemaphoreGive(s_at.lock);
    return active;
}

bool autotune_engine_is_active_on_zone(uint8_t zone_index)
{
    /* See autotune_begin_run_locked()'s guard comment above. */
    if (s_at.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    bool active = state_is_running(s_at.state) && s_at.zone_index == zone_index;
    xSemaphoreGive(s_at.lock);
    return active;
}

bool autotune_engine_reserve_zone_for_external_write(uint8_t zone_index)
{
    /* See autotune_begin_run_locked()'s guard comment above: recovery mode
     * (or a not-yet-started engine) means s_at.lock is NULL and nothing can
     * ever start a run, so there is nothing to reserve against. */
    if (s_at.lock == NULL) {
        return true;
    }
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    if (state_is_running(s_at.state) && s_at.zone_index == zone_index) {
        xSemaphoreGive(s_at.lock);
        return false;
    }
    if (s_at.external_write_reserved) {
        /* Single reservation slot, not a per-zone array -- see s_at_t's own
         * comment for why one live external writer is the only case that can
         * exist today. Refuse rather than silently clobber the existing
         * holder's accounting. */
        xSemaphoreGive(s_at.lock);
        return false;
    }
    s_at.external_write_reserved = true;
    s_at.external_write_reserved_zone = zone_index;
    xSemaphoreGive(s_at.lock);
    return true;
}

void autotune_engine_release_zone_for_external_write(uint8_t zone_index)
{
    if (s_at.lock == NULL) {
        return;
    }
    xSemaphoreTake(s_at.lock, portMAX_DELAY);
    if (s_at.external_write_reserved && s_at.external_write_reserved_zone == zone_index) {
        s_at.external_write_reserved = false;
    }
    xSemaphoreGive(s_at.lock);
}
