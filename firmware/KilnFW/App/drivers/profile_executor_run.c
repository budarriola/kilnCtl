/* profile_executor_run(): validates and launches a firing -- split out of
 * profile_executor.c (2026-09-01, "files over 1500 lines should be broken
 * up where it makes sense"). See profile_executor_internal.h's own doc
 * comment for the full multi-way split this is one piece of. This is the
 * one-shot setup path (profile/zone validation, warm-start replay, initial
 * relay/segment/heat-enable claims); the per-tick control loop it hands off
 * to is executor_task_entry() in profile_executor.c itself. */

#include "profile_executor_internal.h"

#include <math.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "autotune_engine.h"
#include "heat_enable.h"
#include "kiln_io_owner.h"
#include "ota_state.h"
#include "profiles_store.h"
#include "relay_authority.h"
#include "safety_trip_words.h"
#include "sim_backend.h"
#include "thermo_combine.h"
#include "zones_config_accessors.h"

/* 2026-09-01 multi-zone history fix: s_exec.history is now heap-allocated
 * from PSRAM instead of an inline .bss array (see its own doc comment in
 * profile_executor_internal.h) -- lazily, here, on the first run() a board
 * ever executes, rather than at profile_executor_start(), so a board that
 * never fires a profile never pays for the allocation. Idempotent: a board
 * that has already fired once this boot just reuses the same buffer (its
 * contents get overwritten sample-by-sample as this run's own history_count/
 * history_head reset to 0 below -- no need to clear it here). Leaves
 * s_exec.history NULL on failure rather than aborting the run: every reader/
 * writer already checks for NULL, and losing the graph is not a reason to
 * refuse a firing. */
static void history_buf_ensure_alloc(void)
{
    if (s_exec.history != NULL) {
        return;
    }
    size_t bytes = (size_t)HISTORY_MAX_SAMPLES * sizeof(history_slot_t);
    s_exec.history = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (s_exec.history == NULL) {
        ESP_LOGE(PE_TAG, "history buffer PSRAM allocation failed (%u bytes) -- this run's dashboard "
                      "graph will have no recorded history; firing continues",
                 (unsigned)bytes);
    } else {
        memset(s_exec.history, 0, bytes);
    }
}

bool profile_executor_run(uint8_t profile_id, char *err_msg, size_t err_cap)
{
    /* Recovery mode (boot_guard.h) deliberately skips profile_executor_start()
     * so it can bring the board up with just Wi-Fi and the OTA HTTP routes --
     * but other code that DOES still run in that mode (safety_link.c's poll
     * task among others, found on the bench: safety_poll_task ->
     * safety_build_and_send_context() -> profile_executor_get_status() ->
     * xQueueSemaphoreTake() -> "assert failed: (( pxQueue ))" -> panic) can
     * still call into this module's public API. s_exec.lock is NULL until
     * profile_executor_start() creates it, and taking a NULL FreeRTOS mutex
     * asserts. Every public entry point below tests it first and returns a
     * clean "not running" answer instead of touching s_exec at all. */
    if (s_exec.lock == NULL) {
        ESP_LOGW(PE_TAG, "profile_executor_run() called before profile_executor_start() -- refused");
        if (err_msg) snprintf(err_msg, err_cap, "profile executor not started");
        return false;
    }

    profile_t p;
    if (!profiles_http_get(profile_id, &p)) {
        if (err_msg) snprintf(err_msg, err_cap, "no such profile");
        return false;
    }
    if (p.segment_count == 0) {
        if (err_msg) snprintf(err_msg, err_cap, "profile has no segments");
        return false;
    }
    if (p.zone_mask == 0) {
        /* Two different causes land here and the operator has to be able to
         * tell them apart: a saved profile whose own mask is empty (edit the
         * profile) versus a board with no thermocouples declared yet, which
         * is what a built-in schedule's mask is derived from (configure the
         * zones). Reporting either as "no such profile", as this path used
         * to via profiles_http_get(), was simply false. */
        if (err_msg) {
            if (zones_config_get_thermo_count() == 0) {
                snprintf(err_msg, err_cap,
                         "no zones are configured yet -- set the thermocouple count and zone settings "
                         "before firing (see /settings/zones)");
            } else {
                snprintf(err_msg, err_cap, "profile targets no zones");
            }
        }
        return false;
    }
    /* TODO.md 8.2 "Tie it to the guards, not only the UI": refuse explicitly
     * rather than let this fall through to apply_relay()'s relay_mask == 0
     * check, which cannot tell "genuinely no zones configured" from "zone
     * config failed to load" -- both read as the same zeroed struct. */
    if (!zones_config_is_valid()) {
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "zone config failed to load or has not been saved -- this kiln cannot be started "
                     "until zone config loads cleanly (see /settings/zones)");
        }
        return false;
    }
    // Direction B of the mutual OTA interlock (ota_interlock.h/.c is
    // direction A -- "no update while heating"; this is "no heating while
    // updating"): refuse to start a profile while an update is in progress
    // on either processor. heat_interlock.c is the shared pure predicate;
    // ota_http_heat_blocked_by_update() is its ESP-IDF glue, same split as
    // ota_http_check_interlocks()/ota_interlock_check(). Checked here,
    // before the mutex/feasibility checks below, since it's cheapest and
    // orthogonal to zone state.
    if (ota_http_heat_blocked_by_update(err_msg, err_cap)) {
        return false;
    }
    /* B2 (opus review, 2026-08-27): zones_http.c's per-zone current sweep is
     * a sixth writer of the mains-contactor relays, with its own start-time
     * refusal if a profile is already running/paused -- but that check was
     * only ever made ONE-DIRECTIONAL: nothing here refused to start a
     * profile while a sweep was already energizing a zone. See
     * zones_current_sweep_is_active()'s doc comment (zones_http.h) for the
     * full picture. Checked here, right after the OTA check above, for the
     * same "cheap and orthogonal to zone state" reasoning. */
    if (zones_current_sweep_is_active()) {
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "a zone current sweep is running -- it cannot run at the same time as a firing");
        }
        return false;
    }
    /* Refuse at the door when heat authority is already blocked -- the same
     * check, in the same words, autotune_engine.c's begin_run_locked() makes.
     * Without it a start on a board whose safety link is down answered
     * {"ok":true}, entered RUNNING, and was killed ~1s later by the
     * watchdog's "safety processor link silent for >=30000ms, firing
     * aborted", leaving a latched fault the operator then had to Stop before
     * anything else would start. Reporting success for a firing that cannot
     * heat is the failure this refuses to repeat; the message stays under
     * the char[128] dashboard_http.c's start handler passes. */
    {
        uint32_t sources = 0;
        if (relay_authority_on_blocked(s_exec.safety, &sources)) {
            if (err_msg) {
                /* ROADMAP.md M13: decode the mask instead of showing a bare
                 * hex value -- same shortening (first source + "(+more)")
                 * zones_http.c's ZONE_SWEEP_ZONE_ENERGIZE_REFUSED case and
                 * autotune_engine.c's matching refusal already use. */
                char src_words[160];
                safety_fault_source_words(sources, src_words, sizeof(src_words));
                char *comma = strchr(src_words, ',');
                bool more = (comma != NULL);
                if (comma != NULL) {
                    *comma = '\0';
                }
                snprintf(err_msg, err_cap,
                         "heat is blocked (%.32s%s, usually the safety link down) -- "
                         "a firing cannot start",
                         src_words, more ? " (+more)" : "");
            }
            return false;
        }
    }
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if ((p.zone_mask & (1u << zi)) && autotune_engine_is_active_on_zone(zi)) {
            if (err_msg) {
                snprintf(err_msg, err_cap, "zone %u has an autotune run active -- it cannot run at the "
                                           "same time as a profile (TODO.md 6A.5)",
                         zi);
            }
            return false;
        }
    }

    xSemaphoreTake(s_exec.lock, portMAX_DELAY);

    if (s_exec.state == PROFILE_EXEC_RUNNING || s_exec.state == PROFILE_EXEC_PAUSED) {
        xSemaphoreGive(s_exec.lock);
        if (err_msg) snprintf(err_msg, err_cap, "a profile is already running -- stop it first");
        return false;
    }
    if (s_exec.state == PROFILE_EXEC_FAULTED) {
        xSemaphoreGive(s_exec.lock);
        if (err_msg) snprintf(err_msg, err_cap, "a thermal guard is latched -- acknowledge it (Stop) first");
        return false;
    }

    /* Re-run TODO.md section 5's feasibility check against every
     * participating zone's *current* ceiling -- 6A.5: a profile is only
     * feasible if every one of its zones can sustain the requested rate. */
    for (uint8_t i = 0; i < p.segment_count; i++) {
        float rate = p.segments[i].ramp_c_per_hr;
        if (rate <= 0.0f) continue;
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!(p.zone_mask & (1u << zi))) continue;
            float ceiling = 0.0f;
            zones_config_get_max_ramp(zi, &ceiling);
            if (rate > ceiling) {
                xSemaphoreGive(s_exec.lock);
                if (err_msg) {
                    /* A ceiling of 0 is not a ceiling the operator chose, it
                     * is a zone that was never commissioned -- every rate is
                     * "over" it. Quoting "exceeds zone 1's current 0.0 C/hr
                     * ceiling" sent a bench session looking for a ceiling to
                     * raise when the actual answer was that zones 1 and 2 had
                     * never been configured at all. Say which it is. */
                    if (ceiling <= 0.0f) {
                        snprintf(err_msg, err_cap,
                                 "zone %u has no ramp ceiling configured (max_ramp_c_per_hr is 0) -- "
                                 "commission the zone in Settings > Zones before firing it",
                                 zi);
                    } else {
                        snprintf(err_msg, err_cap,
                                 "segment %u: ramp rate %.1f C/hr exceeds zone %u's current %.1f C/hr ceiling",
                                 i + 1, (double)rate, zi, (double)ceiling);
                    }
                }
                return false;
            }
        }
    }

    /* SAFETY TASK (2026-09-02): re-run profiles_http_save()'s per-segment
     * target-vs-zone-ceiling check against every participating zone's
     * *current* max_temp_c, same reasoning as the ramp-ceiling re-check just
     * above -- a zone's max_temp_c can be edited (or a profile's zone_mask
     * widened to cover a different zone) AFTER the profile was saved, so the
     * save-time check alone is not enough at start time. Skips a zone with
     * max_temp_c == 0 on purpose: that zone is uncommissioned, and the
     * profile_zones_have_ceiling() refusal a few lines below is the one that
     * owns refusing to start on it -- this check only means anything once a
     * real ceiling exists to compare against.
     *
     * PID_EXPANSION_PLAN.md sec 7.2: this is ALSO ramp assist's hard-
     * refusal path -- "a target above the kiln's permitted maximum is
     * refused and NEVER stretched," unconditionally, whether or not
     * ramp_assist_cfg_enabled(). There is no separate ramp-assist check
     * beside this one: profile_executor_ramp_assist.c's auto-stretch
     * instrumentation never writes target_c/seg->target_c, so nothing
     * downstream of this refusal can ever command a target this check
     * would have rejected -- the same segment target this REFUSAL compares
     * against max_temp_c is the same target_c the ramp-lock (sec 7.1)
     * asymptotically approaches and auto-stretch (sec 7.2) only measures
     * the approach time of. A REFUSAL, never a silent
     * clamp, naming the offending segment and the zone's current limit. */
    for (uint8_t i = 0; i < p.segment_count; i++) {
        if (p.segments[i].seg_kind != PROFILE_SEG_KIND_ZONE_RAMP) continue;
        float target = p.segments[i].target_c;
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!(p.zone_mask & (1u << zi))) continue;
            float zone_max_c = 0.0f, zone_min_c = 0.0f;
            zones_config_get_temp_limits(zi, &zone_max_c, &zone_min_c);
            if (!(zone_max_c > 0.0f)) continue;
            if (target > zone_max_c) {
                xSemaphoreGive(s_exec.lock);
                if (err_msg) {
                    snprintf(err_msg, err_cap,
                             "segment %u: target %.1fC exceeds zone %u's current %.1fC limit -- refused, "
                             "not clamped",
                             i + 1, (double)target, zi, (double)zone_max_c);
                }
                return false;
            }
        }
    }

    /* TODO relay/IO segments' SECOND, independent zone-ownership re-check
     * (the storage-side one is profiles_http.c's profile_relay_is_zone_owned(),
     * enforced at save time by validate_io_segment()) -- see this function's
     * own re-run of the ramp-ceiling feasibility check just above for the
     * identical reasoning: a relay can be assigned to a zone AFTER a profile
     * was saved, and this run must not energize a contact a zone now owns. */
    for (uint8_t i = 0; i < p.segment_count; i++) {
        if (p.segments[i].seg_kind != PROFILE_SEG_KIND_RELAY_IO) {
            continue;
        }
        uint8_t t = p.segments[i].io_target;
        bool is_relay = (t >= PROFILE_IO_TARGET_RELAY_BASE) && (t < PROFILE_IO_TARGET_RELAY_BASE + KILN_IO_RELAY_COUNT);
        if (!is_relay) {
            continue; /* general-purpose IO_1..7 has no zone-ownership concept */
        }
        uint8_t owning_zone = 0;
        if (relay_io_target_is_zone_owned(t, &owning_zone)) {
            xSemaphoreGive(s_exec.lock);
            if (err_msg) {
                snprintf(err_msg, err_cap,
                         "segment %u: relay %u is now assigned to zone %u -- this profile cannot run "
                         "until that segment's target is changed",
                         i + 1, t, owning_zone);
            }
            return false;
        }
    }

    /* Guard 5's absolute ceiling, refused the same way the ramp ceiling just
     * above is: TODO.md's 2026-08-27 audit ("Guard 5's absolute ceiling is
     * off by default") found that max_temp_c == 0 means "no ceiling" in
     * thermal_guard.c, so a zone that was never saved through
     * /settings/zones fires with guard 5 permanently a no-op -- every OTHER
     * per-zone threshold in this repo's convention treats 0 as "not
     * configured, substitute a firmware default that still protects" (see
     * zones_http.h's field-by-field doc comments), but max_temp_c is the one
     * field where 0 was instead wired to mean "disabled". That is backwards
     * for the single most dangerous field on the page: a substituted number
     * would have to be invented (there is no physically-meaningful default
     * temperature anywhere in this repo -- ZONE_MAX_TEMP_C_MAX below is a
     * 2500C INPUT-validation sanity bound mirrored from profiles_http.c's
     * PROFILE_TARGET_C_MAX, not a safe ceiling for an arbitrary owner's
     * kiln), and a wrong invented ceiling either does nothing (too high) or
     * nags a correctly-configured kiln (too low). Refusing instead cannot be
     * silently wrong, matches the ramp-ceiling refusal immediately above
     * (same field-is-zero-means-uncommissioned reasoning, same message
     * shape), and matches autotune_engine_run_relay()'s own guard-5 refusal
     * for the identical reason (autotune_engine.c ~line 1164, this task's
     * FILES YOU OWN excludes that file so it is read-only precedent here,
     * not touched). autotune_engine_run() (the step-test path) is the one
     * place in the repo that deliberately tolerates max_temp_c == 0 -- see
     * its STEP_TEST_GUARD_HEADROOM_C comment -- because a step test is a
     * short, operator-watched open-loop probe, not an unattended multi-hour
     * firing; that carve-out does not apply here. */
    {
        uint8_t missing_zone = 0;
        if (!profile_zones_have_ceiling(&p, &missing_zone)) {
            xSemaphoreGive(s_exec.lock);
            if (err_msg) {
                snprintf(err_msg, err_cap,
                         "zone %u has no absolute temperature ceiling configured (max_temp_c is 0) -- "
                         "set Max Temp (C) for this zone in Settings > Zones before firing it",
                         missing_zone);
            }
            return false;
        }
    }

    s_exec.profile = p;
    s_exec.profile_id = profile_id;
    s_exec.segment_index = 0;
    s_exec.dwelling = false;
    s_exec.segment_elapsed_s = 0;
    s_exec.ramp_lock_held = false;
    s_exec.ramp_lock_lagging_mask = 0;
    memset(s_exec.stretch_by_segment_s, 0, sizeof(s_exec.stretch_by_segment_s));
    s_exec.stretch_total_s = 0.0f;
    /* PID_EXPANSION_PLAN.md sec 7.3: dwell credit -- a previous run's last
     * applied spend has no meaning against a freshly (re)started schedule,
     * same "starts owing nothing" reasoning as stretch_total_s just above.
     * Per-zone dwell_credit_s is zeroed by the
     * memset(s_exec.zones, ...) a few lines down. */
    s_exec.dwell_credit_applied_s = 0.0f;
    s_exec.fault_reason[0] = '\0';
    s_exec.fault_guard = THERMAL_GUARD_TRIP_NONE;
    s_exec.global_fault_source = 0;
    /* Per-run, not cumulative: carrying a previous firing's claim forward
     * would let this run's sweep open a relay the last run once drove and an
     * operator has since taken over manually. Each run starts owing nothing
     * and claims what it touches (see s_exec_state_t.claimed_relay_mask). */
    s_exec.claimed_relay_mask = 0;
    /* Same "starts owing nothing" reasoning as claimed_relay_mask just above,
     * for the relay/IO segment machinery: a previous run's io_segs[] state
     * (which segment was active, what its remaining_s countdown was) has no
     * meaning against a freshly (re)started schedule. */
    memset(s_exec.io_segs, 0, sizeof(s_exec.io_segs));
    /* Warm-start state, same "starts owing nothing" reasoning -- overwritten
     * below if this run actually warm-starts. */
    s_exec.warm_started = false;
    s_exec.warm_start_reason[0] = '\0';
    s_exec.warm_start_replayed_count = 0;
    memset(s_exec.warm_start_replayed_segments, 0, sizeof(s_exec.warm_start_replayed_segments));
    /* Feedforward inputs start from their safe values: no ramp commanded yet,
     * and the fallback ambient until a cold junction actually answers below. */
    s_exec.target_rate_c_per_s = 0.0f;
    s_exec.ambient_c = FALLBACK_AMBIENT_C;
    s_exec.ambient_from_cj = false;

    memset(s_exec.zones, 0, sizeof(s_exec.zones)); /* also zeros every zone's fs_* accumulator */

    /* PID_EXPANSION_PLAN.md Phase 7a: fresh firing-stats accumulator for
     * this run. fs_target_min_c/max_c start NAN (not 0) so the first
     * RUNNING tick's target_c seeds both ends of the span unconditionally --
     * 0 would be a real, wrong target for a kiln firing (see the tick
     * loop's fs_target_min_c/max_c update). */
    s_exec.fs_target_min_c = NAN;
    s_exec.fs_target_max_c = NAN;
    s_exec.fs_persisted = false;
    {
        time_t now = time(NULL);
        /* time(NULL) before SNTP sync reads as a small epoch offset (ESP-IDF
         * boots the RTC near 0), not a plausible 2020s+ date -- treat
         * anything before 2020-01-01 UTC (1577836800) as "unsynced" and
         * store 0 rather than a misleadingly precise-looking fake date. */
        s_exec.run_started_unix_s = (now >= (time_t)1577836800) ? (uint32_t)now : 0u;
    }

    /* Sampled BEFORE the per-zone config read below, not after (TODO.md
     * 6A.7): zones_http.c's writers don't take s_exec.lock, so an edit
     * committed while this loop is running would otherwise be swallowed --
     * counted as "already applied" while half the zones still hold the
     * pre-edit values. Sampling first makes that race resolve the safe way:
     * the first tick sees a generation mismatch and re-reads everything. A
     * redundant reload costs one log line; a missed one costs a firing run
     * with settings the operator believes they changed. */
    s_exec.config_generation = zones_config_generation();

    /* TODO.md 6A.5 load-staggering: n_zones for the phase-offset formula
     * "zone i starts its window at i*window_ms/n_zones" -- i is this run's
     * rank among its own active zones (0-based, ascending zone index), not
     * the raw zone index, so a 2-zone run on zones {0,2} still gets a clean
     * 50/50 offset instead of stretching across 3 slots it isn't using. */
    uint8_t n_active_zones = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (p.zone_mask & (1u << zi)) n_active_zones++;
    }

    /* Refuse a firing that cannot heat anything.
     *
     * ZONE_CONTROL_MODE_OFF is 0, which is also what an uncommissioned zone
     * reads as, so "every zone in this profile is OFF" is the DEFAULT state of
     * a board nobody has configured yet -- not an exotic case. Started in that
     * state the run was accepted, reported state "running" with a target
     * ramping convincingly for its full 21 minutes, duty 0.0, faulted false,
     * heat_blocked false, and no message in the log: every single indicator
     * said a firing was under way and not one relay would ever close. The
     * readiness page agreed ("every configured zone has a mode (OFF is a valid
     * choice)"), which is true of one zone and dangerously incomplete of a
     * whole profile. Found by running it on the bench and watching nothing
     * happen for two minutes.
     *
     * OFF stays a valid per-zone choice -- a 3-zone kiln fired on 2 zones is
     * legitimate -- so only the all-OFF case is refused, and the mixed case
     * gets a log line naming which zones will sit idle. */
    uint8_t n_heating_zones = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!(p.zone_mask & (1u << zi))) continue;
        zone_control_mode_t m = ZONE_CONTROL_MODE_OFF;
        zones_config_get_control_mode(zi, &m);
        if (m != ZONE_CONTROL_MODE_OFF) {
            n_heating_zones++;
        } else {
            ESP_LOGW(PE_TAG, "zone %u is in this profile but its control mode is OFF -- it will not heat", zi);
        }
    }
    if (n_heating_zones == 0) {
        xSemaphoreGive(s_exec.lock);
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "every zone in this profile is set to control mode OFF -- nothing would heat. "
                     "Pick bang-bang or PID in Settings > Zones.");
        }
        return false;
    }

    int8_t first_active = -1;
    uint8_t active_rank = 0;
    float baseline_target_c = p.segments[0].target_c;
    /* Warm-start (Q4): "current temperature" is the COOLEST active zone's
     * actual reading, taken from the same start-of-run sample as
     * baseline_target_c above (not a second, separately-timed read) -- if
     * the zones disagree, preferring the coolest one means warm-start can
     * only ever skip work every active zone agrees is already done. NAN
     * until (if) a valid reading is found below. */
    float warm_start_coolest_c = NAN;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!(p.zone_mask & (1u << zi))) continue;
        zone_runtime_t *z = &s_exec.zones[zi];
        z->active = true;
        if (first_active < 0) first_active = (int8_t)zi;

        /* OFF, not BANGBANG, as the fallback if the getter fails: "we could
         * not read this zone's control mode" must not resolve to "close the
         * relay". The all-OFF guard above has already refused a run where
         * every zone lands here. */
        zone_control_mode_t mode = ZONE_CONTROL_MODE_OFF;
        zones_config_get_control_mode(zi, &mode);
        z->control_mode = mode;

        /* Cached only so the mid-run reload (TODO.md 6A.7) can tell a mask
         * edit apart from no change, and can still name the outgoing relays
         * when one happens -- see zone_runtime_t.relay_mask. Everything else
         * in this file keeps reading the live mask through apply_relay(). */
        uint8_t relay_mask = 0;
        zones_config_get_relay_mask(zi, &relay_mask);
        z->relay_mask = relay_mask;
        /* Claimed up front rather than waiting for the first apply_relay():
         * a zone whose mask is edited before it ever energizes still has to
         * be sweepable, and the run has unambiguously taken these relays
         * over the moment it starts. */
        s_exec.claimed_relay_mask |= relay_mask;

        float kp = 0.0f, ki = 0.0f, kd = 0.0f;
        zones_config_get_pid(zi, &kp, &ki, &kd);
        z->pid_cfg = (pid_cfg_t){
            .kp = kp, .ki = ki, .kd = kd,
            .d_filter_tau_s = PID_D_FILTER_TAU_S, .b = PID_SETPOINT_WEIGHT_B,
            .pid_range_c = PID_FUNCTIONAL_RANGE_C,
        };
        pid_reset(&z->pid_state);
        z->fuzzy_prev_effective_ki = 0.0f;

        /* TODO.md 6A.2 feedforward: identified model or nothing. A zone that
         * has never been autotuned simply runs on feedback alone, as every
         * firing did before this existed -- see zone_load_model(). */
        (void)zone_load_model(zi);

        float window_ms = 0.0f, min_on_ms = 0.0f, min_off_ms = 0.0f;
        zones_config_get_heater_cfg(zi, &window_ms, &min_on_ms, &min_off_ms);
        z->heater_cfg = (heater_output_cfg_t){
            .window_ms = (window_ms > 0.0f) ? (uint32_t)window_ms : HEATER_WINDOW_MS,
            .min_on_ms = (min_on_ms > 0.0f) ? (uint32_t)min_on_ms : HEATER_MIN_ON_MS,
            .min_off_ms = (min_off_ms > 0.0f) ? (uint32_t)min_off_ms : HEATER_MIN_OFF_MS,
        };
        heater_output_reset(&z->heater_state);
        if ((z->control_mode == ZONE_CONTROL_MODE_PID || z->control_mode == ZONE_CONTROL_MODE_PID_FUZZY) &&
            n_active_zones > 1) {
            uint32_t phase_offset_ms = ((uint32_t)active_rank * z->heater_cfg.window_ms) / n_active_zones;
            heater_output_seed_phase(&z->heater_state, z->heater_cfg.window_ms, phase_offset_ms);
        }
        active_rank++;

        float max_temp_c = 0.0f, min_temp_c = -20.0f, sanity_rate = 0.0f;
        float cross_zone_delta_c = 0.0f;
        zones_config_get_temp_limits(zi, &max_temp_c, &min_temp_c);
        zones_config_get_sanity_rate(zi, &sanity_rate);
        zones_config_get_cross_zone_delta(zi, &cross_zone_delta_c);
        /* TODO.md 6A.3's remaining named thresholds: raw pass-through, 0 and
         * all, straight from zones_config_get_guard_thresholds() -- unlike
         * sanity_rate_c_per_min above (whose 0->default substitution happens
         * HERE, at the caller), these substitute inside thermal_guard.c
         * itself (effective_f()/effective_ticks()), so there is exactly one
         * place that owns each fallback constant rather than two copies that
         * can drift apart. */
        float wd_window_s = 0.0f, wd_rate = 0.0f, off_settle_s = 0.0f, runaway_rate = 0.0f;
        float runaway_margin = 0.0f, drift_period_s = 0.0f, debounce_ticks = 0.0f, frozen_window_s = 0.0f;
        zones_config_get_guard_thresholds(zi, &wd_window_s, &wd_rate, &off_settle_s, &runaway_rate,
                                          &runaway_margin, &drift_period_s, &debounce_ticks, &frozen_window_s);
        /* The five v8 overrides, same raw pass-through: thermal_guard.c owns
         * every 0->default substitution. */
        float progress_duty_min = 0.0f, progress_window_s = 0.0f, drift_hysteresis_c = 0.0f;
        float frozen_eps_c = 0.0f, cross_zone_period_s = 0.0f;
        zones_config_get_guard_extra(zi, &progress_duty_min, &progress_window_s, &drift_hysteresis_c,
                                     &frozen_eps_c, &cross_zone_period_s);
        z->guard_cfg = (thermal_guard_cfg_t){
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
            /* Guard 8: whatever the operator entered on the zones page, and
             * 0 (the default) still means disabled. No substituted default
             * here on purpose -- the number is supposed to come from a
             * measured cross-gain matrix (TODO.md 6A.5's last bullet), so
             * the firmware offers the field rather than inventing a value.
             * The period is now an operator field too (v8): 0 still means
             * thermal_guard.c's CROSS_ZONE_PERIOD_S_DEFAULT (600 s), so a
             * board that never sets it behaves exactly as before. */
            .cross_zone_max_delta_c = cross_zone_delta_c,
            .cross_zone_period_s = cross_zone_period_s,
            .progress_duty_min = progress_duty_min,
            .progress_window_s = progress_window_s,
            .drift_hysteresis_c = drift_hysteresis_c,
            .frozen_eps_c = frozen_eps_c,
        };
        thermal_guard_reset(&z->guard_state);

        /* Ramp baseline: the first active zone's actual (calibrated)
         * reading if we have one, else the segment's own target (makes
         * ramp math a no-op rather than ramping from a fabricated zero).
         * TODO.md 10.8: this must be the same COMBINED reading the very
         * first control tick will compute for this zone (see the main read
         * block above), not just its legacy same-index channel -- otherwise
         * a multi-thermocouple zone would start its ramp math from a
         * different number than the tick right after it settles on. */
        if ((int8_t)zi == first_active &&
            (sim_backend_enabled() || (s_exec.thermo_bus && s_exec.thermo_bus->initialized))) {
            MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
            size_t count = 0;
            if (sim_backend_enabled()) {
                sim_backend_read_all(readings, MAX31856_CHANNEL_COUNT, &count);
            } else {
                MAX31856_read_all(s_exec.thermo_bus, readings, MAX31856_CHANNEL_COUNT, &count);
            }
            float base_ch_c[MAX31856_CHANNEL_COUNT];
            bool base_ch_ok[MAX31856_CHANNEL_COUNT];
            for (uint8_t ci = 0; ci < MAX31856_CHANNEL_COUNT; ci++) {
                base_ch_c[ci] = NAN;
                base_ch_ok[ci] = false;
            }
            for (size_t i = 0; i < count; i++) {
                uint8_t ci = readings[i].channel;
                if (ci >= MAX31856_CHANNEL_COUNT) continue;
                base_ch_c[ci] = readings[i].tc_temperature_c;
                base_ch_ok[ci] = !readings[i].spi_failed && !isnan(base_ch_c[ci]);
            }
            uint8_t base_tmask = 0;
            zones_config_get_thermo_mask(zi, &base_tmask);
            bool base_valid = false;
            float base_combined =
                thermo_combine(base_ch_c, base_ch_ok, MAX31856_CHANNEL_COUNT, base_tmask, &base_valid);
            if (base_valid) {
                baseline_target_c = zones_config_apply_cal(zi, base_combined);
            }

            /* Warm-start's "current temperature" (Q4): the coolest ACTIVE
             * zone's own combined+calibrated reading, from this same
             * already-fetched `readings` array -- every active zone gets its
             * own thermo_mask/thermo_combine/apply_cal treatment here (not
             * just first_active's), because a zone other than first_active
             * can legitimately be the coolest one and skipping its
             * temperature would risk skipping work it still needs. This
             * block runs exactly once (gated on zi == first_active, same as
             * the baseline_target_c read above), so it loops over every
             * active zone itself rather than relying on the outer loop's
             * per-zi iteration to reach it. */
            for (uint8_t wzi = 0; wzi < MAX31856_CHANNEL_COUNT; wzi++) {
                if (!(p.zone_mask & (1u << wzi))) continue;
                uint8_t w_tmask = 0;
                zones_config_get_thermo_mask(wzi, &w_tmask);
                bool w_valid = false;
                float w_combined = thermo_combine(base_ch_c, base_ch_ok, MAX31856_CHANNEL_COUNT, w_tmask, &w_valid);
                if (!w_valid) continue;
                float w_c = zones_config_apply_cal(wzi, w_combined);
                if (isnan(warm_start_coolest_c) || w_c < warm_start_coolest_c) {
                    warm_start_coolest_c = w_c;
                }
            }

            /* Feedforward's ambient reference (TODO.md 6A.2), taken from THIS
             * read rather than a second one: the cold junction is only honest
             * about the room before the firing has warmed the board, and this
             * is the last moment that is true. Any channel's CJ will do -- all
             * five sit on the same board within centimetres of each other, and
             * accepting the first valid one means a single dead or CJRANGE-
             * flagged channel doesn't cost the whole run its ambient. */
            for (size_t i = 0; i < count; i++) {
                if (!readings[i].spi_failed && !isnan(readings[i].cj_temperature_c)) {
                    s_exec.ambient_c = readings[i].cj_temperature_c;
                    s_exec.ambient_from_cj = true;
                    break;
                }
            }
        }
    }

    /* Warm-start (PROFILES.md, owner request 2026-08-30): decide once, here,
     * before the ramp-lock/segment-stepping machinery ever runs its first
     * tick -- see profile_executor_plan_warm_start()'s own doc comment for
     * the entry-point algorithm and profile_exec_status_t's warm_started
     * field for what's reported back to the operator (Q6). Only APPLIED
     * (segment_index/dwelling/segment_elapsed_s/baseline_target_c
     * overridden) when the plan actually warm-started -- the "not
     * warm-started" branch leaves every one of those exactly as the
     * pre-feature code already set them, byte-for-byte. */
    {
        profile_warm_start_plan_t plan = profile_executor_plan_warm_start(&p, warm_start_coolest_c);
        if (plan.warm_started) {
            s_exec.segment_index = plan.entry_segment_index;
            s_exec.dwelling = plan.entry_dwelling;
            s_exec.segment_elapsed_s = plan.entry_segment_elapsed_s;
            baseline_target_c = plan.entry_target_c;

            s_exec.warm_started = true;
            snprintf(s_exec.warm_start_reason, sizeof(s_exec.warm_start_reason),
                     "starting at segment %u -- kiln already at %.1f C",
                     (unsigned)plan.entry_segment_index + 1, (double)warm_start_coolest_c);
            ESP_LOGI(PE_TAG, "warm start: %s (dwelling=%d, entry target %.1fC, %lus into the entry segment)",
                     s_exec.warm_start_reason, (int)plan.entry_dwelling, (double)plan.entry_target_c,
                     (unsigned long)plan.entry_segment_elapsed_s);

            /* Q1 owner decision: replay every skipped RELAY_IO segment's
             * on/off command, in profile order, before the first ramp tick
             * -- reusing io_seg_start() gets both the hardware write and the
             * relay_authority claim/claimed_relay_mask registration this
             * needs "for free", identical to how a segment reached normally
             * would be started. The one deliberate difference: forcing
             * `blocking = true` afterward makes io_segs_tick() (which skips
             * any segment with blocking == true) leave this segment alone
             * for the rest of the run -- its own hold/dwell_min timer is
             * NOT replayed (that schedule position is already past), only
             * the command is. It is retired the same way a real blocking
             * segment's command is: by the end-of-run sweep
             * (io_segs_force_all_off(), honoring leave_on_at_end only on the
             * clean DONE path, same as always) -- exactly the registration
             * this decision requires so a replayed relay is never left
             * energized with nothing owning it. */
            for (uint8_t i = 0; i < plan.entry_segment_index && i < p.segment_count; i++) {
                if (p.segments[i].seg_kind != PROFILE_SEG_KIND_RELAY_IO) {
                    continue;
                }
                io_seg_start(i, &p.segments[i]);
                s_exec.io_segs[i].blocking = true;
                if (s_exec.warm_start_replayed_count < PROFILE_MAX_SEGMENTS) {
                    s_exec.warm_start_replayed_segments[s_exec.warm_start_replayed_count++] = i;
                }
                ESP_LOGI(PE_TAG, "warm start: replayed relay/IO segment %u command (%s %u %s) -- its own hold "
                              "was NOT restarted, it stays as commanded until the run ends",
                         i + 1, s_exec.io_segs[i].is_relay ? "relay" : "IO_",
                         s_exec.io_segs[i].is_relay
                             ? s_exec.io_segs[i].target
                             : (uint8_t)(s_exec.io_segs[i].target - PROFILE_IO_TARGET_IO_BASE + 1u),
                         s_exec.io_segs[i].state_on ? "ON" : "OFF");
            }
        }
    }

    s_exec.target_c = baseline_target_c;
    s_exec.run_start_c = baseline_target_c;
    s_exec.total_elapsed_s = 0;

    /* PID_EXPANSION_PLAN.md sec 3.6d: seed every zone's own capped setpoint
     * at the same value s_exec.target_c starts this run/warm-start from --
     * NOT left at the memset(s_exec.zones, ...) 0.0f default a few lines up,
     * which would otherwise make a freshly (re)started, capped zone spend
     * real firing time climbing its OWN commanded setpoint from 0 degC
     * before the cap could ever engage usefully. An uncapped zone is
     * unaffected either way -- profile_executor.c's per-tick update always
     * overwrites this with s_exec.target_c directly when uncapped. */
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        s_exec.zones[zi].effective_target_c = baseline_target_c;
    }

    /* One line per firing recording what feedforward will run on, because it
     * is the difference between two firings of the same profile behaving
     * differently and there is no other record of it: which zones have a model
     * at all, and which ambient the hold term is measured against. */
    if (s_exec.ambient_from_cj) {
        ESP_LOGI(PE_TAG, "feedforward ambient reference: %.1fC (cold junction at firing start, not re-sampled)",
                 (double)s_exec.ambient_c);
    } else {
        ESP_LOGW(PE_TAG, "no valid cold-junction reading at firing start -- feedforward ambient falls back to "
                      "%.1fC; the hold term is off by (true ambient - %.1f)/K_dc, a few percent of duty at most",
                 (double)FALLBACK_AMBIENT_C, (double)FALLBACK_AMBIENT_C);
    }
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!s_exec.zones[zi].active) continue;
        if (s_exec.zones[zi].ff_enabled) {
            /* PID_EXPANSION_PLAN.md section 2c/Phase 3b: report whether this
             * zone's coupling row can contribute anything this firing, same
             * as the K_dc/tau line above is the per-firing record of the
             * base model -- an un-commissioned kiln (every coupling_coeff
             * 0, the migration default) logs "coupling OFF" and behaves
             * exactly as before this existed.
             *
             * Post-review fix (finding 4): a neighbour only counts here if
             * zone_feedforward() would actually use it -- same
             * zone_qualifies_as_coupling_neighbor() gate, not just "has a
             * nonzero coefficient". Before this fix the log said "coupling
             * ON: N neighbor(s)" from the coefficient table alone, so an
             * operator could see "coupling ON: 2 neighbour(s)" for a firing
             * where both neighbours were OFF/blocked/faulted and every one
             * of them was excluded at runtime -- the log claimed coupling
             * was active while it contributed exactly 0.0 all firing. Note
             * this is evaluated once, at firing start: a neighbour that
             * changes qualification mid-firing (an operator flips it to OFF,
             * a guard trips it) is not re-logged -- this line is a
             * per-firing summary of what coupling was set up to do, not a
             * live status feed (that's GET /api/profile_exec). */
            uint8_t coupling_neighbors = count_qualifying_coupling_neighbors(zi);
            bool coupling_any = coupling_neighbors > 0;
            if (coupling_any) {
                ESP_LOGI(PE_TAG, "zone %u feedforward ON: K_dc %.4g C/duty, tau %.4gs (TODO.md 6A.2), "
                              "coupling ON: %u neighbor(s) with a measured coefficient (PID_EXPANSION_PLAN.md 2c)",
                         zi, (double)s_exec.zones[zi].ff_k_dc, (double)s_exec.zones[zi].ff_tau_s,
                         coupling_neighbors);
            } else {
                ESP_LOGI(PE_TAG, "zone %u feedforward ON: K_dc %.4g C/duty, tau %.4gs (TODO.md 6A.2), "
                              "coupling OFF: no measured coefficient for any neighbor",
                         zi, (double)s_exec.zones[zi].ff_k_dc, (double)s_exec.zones[zi].ff_tau_s);
            }
        } else {
            ESP_LOGI(PE_TAG, "zone %u feedforward OFF: no identified plant model (run autotune) -- "
                          "feedback alone, unchanged from before 6A.2's feedforward existed", zi);
        }
    }

    TickType_t now = xTaskGetTickCount();
    s_exec.prev_control_tick = now;
    s_exec.history_run_start_tick = now;
    s_exec.history_last_sample_tick = now;
    history_buf_ensure_alloc();
    s_exec.history_count = 0;
    s_exec.history_head = 0;

    /* The atomic gate (relay_authority.h's heat-claim doc comment): this
     * function's own zones_current_sweep_is_active() check far above is a
     * plain, non-atomic read of zones_http.c's state, made before s_exec.lock
     * was even taken -- a sweep can start in the window between that read
     * and this commit. This call is the last possible moment before the
     * commit, s_exec.lock has been held continuously since the "already
     * running"/"faulted" checks confirmed this is a genuine start (not a
     * reentrant call on an already-RUNNING instance -- see
     * relay_heat_zone_claimant_t's doc comment for why that ordering is what
     * makes release_profile_relay_claim()'s unconditional _end() call safe),
     * and it is a single mutex-protected test-and-set against
     * zones_http.c's/autotune_engine.c's matching gates. Refused with the
     * SAME message the early check already reports for the common
     * (non-race) case. */
    if (!relay_authority_heat_zone_claim_begin(RELAY_HEAT_ZONE_CLAIM_PROFILE)) {
        xSemaphoreGive(s_exec.lock);
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "a zone current sweep is running -- it cannot run at the same time as a firing");
        }
        return false;
    }

    /* TODO.md section 0's ownership decision, closing the "manual relay
     * control is not blocked during a firing" gap: claim every relay this
     * run touches so /api/relay and the UART SET_RELAY* commands refuse a
     * manual command against it until pause/halt hands it back. */
    relay_authority_claim_mask(s_exec.claimed_relay_mask, RELAY_OWNER_PROFILE);

    /* Ask the safety processor to permit heating -- i.e. close K4. THE fix
     * of 2026-08-29: this call did not exist, so every firing this firmware
     * has ever run closed its own zone relay (K1) and left K4 open, and no
     * element current ever flowed on the normal path. See heat_enable.h.
     *
     * Placed here, at the commit point, and not earlier: every refusal above
     * returns without having asked for anything, and from this statement on
     * the run is RUNNING and will command heat. A false return is NOT a
     * reason to refuse the run -- the only way it can fail is a safety link
     * that is down, which apply_relay()'s relay_authority_zone_blocked()
     * check already reports per zone through heat_blocked/
     * heat_blocked_sources and which heat_enable_reconcile() (called from
     * the watchdog task) retries. heat_enable.c logs the failure loudly. */
    (void)heat_enable_acquire(HEAT_ENABLE_CLAIMANT_PROFILE);

    s_exec.state = PROFILE_EXEC_RUNNING;
    run_snapshot_buf_t start_snap;
    capture_run_snapshot(&start_snap);
    xSemaphoreGive(s_exec.lock);

    /* First write of this run's breadcrumb, and the one that overwrites any
     * previous run's record in flash. From here on the stored record says a
     * firing is in progress until something records an ending. */
    run_state_note(RUN_STATE_PHASE_RUNNING, &start_snap.snap);

    ESP_LOGI(PE_TAG, "profile '%s' (id %u, zone_mask 0x%02X) running", p.name, profile_id, p.zone_mask);
    return true;
}

