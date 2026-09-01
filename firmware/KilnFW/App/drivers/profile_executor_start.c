/* profile_executor_start(): board/hw wiring hookup, plus the pre-run
 * feasibility checks and warm-start plan builder profile_executor_run()
 * calls -- split out of profile_executor.c (2026-09-01, "files over 1500
 * lines should be broken up where it makes sense"). See
 * profile_executor_internal.h's own doc comment for the full multi-way
 * split this is one piece of. */

#include "profile_executor_internal.h"

#include <math.h>

#include "esp_log.h"

#include "adaptive_tune.h"
#include "autotune_engine.h"
#include "relay_cycles.h"
#include "run_state.h"
#include "stack_margin.h"
#include "zones_http.h"

esp_err_t profile_executor_start(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                                  SafetyLinkClass *safety_or_null)
{
    memset(&s_exec, 0, sizeof(s_exec));
    s_exec.io = io_or_null;
    s_exec.thermo_bus = thermo_bus_or_null;
    s_exec.safety = safety_or_null;
    s_exec.state = PROFILE_EXEC_IDLE;
    s_exec.last_tick_tick = xTaskGetTickCount();

    s_exec.lock = xSemaphoreCreateMutex();
    if (!s_exec.lock) {
        ESP_LOGE(PE_TAG, "xSemaphoreCreateMutex failed");
        return ESP_ERR_NO_MEM;
    }

    /* Load (and log) the previous run's breadcrumb before the control task
     * exists, so the WARN banner lands in the boot log ahead of any tick
     * output. Loading is ALL that happens: the executor comes up IDLE with
     * every relay already off, exactly as it did before this record existed,
     * and nothing below consults it. TODO.md 6A.3: no auto-resume, ever.
     * A failure costs the breadcrumb, not the executor -- same non-fatal
     * convention as relay_cycles_init() in main.c. */
    esp_err_t rs_err = run_state_init();
    if (rs_err != ESP_OK) {
        ESP_LOGW(PE_TAG, "run_state_init failed: %s -- no reboot breadcrumb kept this boot",
                 esp_err_to_name(rs_err));
    }

    /* PID_EXPANSION_PLAN.md Phase 7d: loads the per-zone opt-in flags (own
     * NVS namespace, see adaptive_tune.c's top comment) and registers its
     * HTTP endpoints. No init entry point of its own in main.c -- hooked
     * here since profile_executor.c is this feature's owner and this
     * function already runs once, from app_main's task, before the control
     * task exists. */
    adaptive_tune_init();

    /* Priority 5, matching the UART bridge tasks (uart_bridge.c, all 5) --
     * TODO.md 6A.7 calls for "below the link-loss watchdog (6), above the
     * bridge tasks". FreeRTOS priorities are integers with nothing between
     * 5 and 6, so exactly "above the bridge tasks" isn't representable
     * without also renumbering uart_bridge.c's tasks (out of scope here);
     * tying at 5 is the closest achievable approximation, still strictly
     * below link_watchdog_task/UART_PROTOCOL_TASK_PRIORITY (6).
     *
     * 4096: MAX31856_read_all/pid_update_terms/thermal_guard_tick/
     * kiln_io_set_relay_mask/safety_link_set_fault_source all run on this
     * stack, now looped up to MAX31856_CHANNEL_COUNT times per tick. */
    /* INTERNAL stack, deliberately. A 2026-08-22 pass moved this to PSRAM on
     * the reasoning that every flash-touching call was made by
     * profile_executor_run()/_halt()/_pause() from whichever task called
     * them, never from this loop. That reasoning was WRONG and the board
     * crashed on the bench the first time a real firing was started:
     * the tick path itself calls run_state_note() (RUNNING/FAULTED/DONE
     * breadcrumbs, ~line 1516) and relay_cycles_maybe_persist() (~line 1481),
     * both of which write NVS. A task whose stack lives in PSRAM cannot be
     * running when the flash cache is disabled -- ESP-IDF asserts
     * esp_task_stack_is_sane_cache_disabled() in
     * spi_flash_disable_interrupts_caches_and_other_cpu() and panics.
     *
     * Do not move this back without first removing every flash write from
     * the tick path, which is not a stack-placement question but a design
     * one: the run-state breadcrumb exists precisely so a power loss mid-tick
     * is recoverable. */
    BaseType_t ok = xTaskCreatePinnedToCore(executor_task_entry, "profile_executor", 4096, NULL, 5,
                                            &s_exec.task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(PE_TAG, "xTaskCreatePinnedToCoreWithCaps(profile_executor) failed");
        vSemaphoreDelete(s_exec.lock);
        s_exec.lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* Opus review round 3, item 5: registered even on the ok==pdPASS-only
     * path (matching every other stack_margin_register() call site in this
     * codebase -- creation failure already returned above, so this line is
     * only ever reached with a real handle). 4096 must match the literal
     * xTaskCreatePinnedToCore() argument two lines up exactly -- see
     * stack_margin.h's own doc comment on why this number is never assumed
     * equal to another task's. */
    stack_margin_register("profile_executor", &s_exec.task, 4096);
    /* Small and independent on purpose -- guard 9 exists precisely because
     * the control task cannot be trusted to notice its own death. Same
     * priority as the control task it's watching.
     *
     * INTERNAL stack, for the same reason as executor_task_entry() above and
     * with the same bench crash behind it. The claim that this task "only
     * reads status and forces relays off" missed run_state_note() on its own
     * abort path (~line 1728): when the safety link goes silent it aborts the
     * firing AND persists why. That is exactly the moment this task must not
     * fail, so its stack must be reachable with the flash cache disabled. */
    ok = xTaskCreatePinnedToCore(watchdog_task_entry, "profile_exec_wdt", 2560, NULL, 5,
                                 &s_exec.watchdog_task, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(PE_TAG, "xTaskCreatePinnedToCoreWithCaps(profile_exec_wdt) failed -- guard 9 unavailable this boot");
    }
    /* Registered unconditionally, ok==pdPASS or not -- stack_margin_register()
     * reads *task_handle_slot fresh at report time (stack_margin.h's own
     * doc comment), so a creation failure just reads back alive=false
     * rather than needing a second branch here. */
    stack_margin_register("profile_exec_wdt", &s_exec.watchdog_task, 2560);

    ESP_LOGI(PE_TAG, "profile executor up (io_ready=%d, thermo_ready=%d, safety_ready=%d) -- "
                  "NOT YET VERIFIED AGAINST REAL RELAY/THERMOCOUPLE HARDWARE (single- or multi-zone), "
                  "see profile_executor.h",
             io_or_null != NULL, thermo_bus_or_null != NULL && thermo_bus_or_null->initialized,
             safety_or_null != NULL);
    return ESP_OK;
}

/* Guard 5's absolute ceiling, checked BEFORE a firing is allowed to start.
 * TODO.md's 2026-08-27 audit ("Guard 5's absolute ceiling is off by
 * default") found that max_temp_c == 0 means "no ceiling" in
 * thermal_guard.c, so a zone that was never saved through /settings/zones
 * fires with guard 5 permanently a no-op -- every OTHER per-zone threshold
 * in this repo's convention treats 0 as "not configured, substitute a
 * firmware default that still protects" (see zones_http.h's field-by-field
 * doc comments), but max_temp_c is the one field where 0 was instead wired
 * to mean "disabled". That is backwards for the single most dangerous field
 * on the page: a substituted number would have to be invented (there is no
 * physically-meaningful default temperature anywhere in this repo --
 * ZONE_MAX_TEMP_C_MAX is a 1400C INPUT-validation sanity bound mirrored
 * from profiles_http.c's PROFILE_TARGET_C_MAX, not a safe ceiling for an
 * arbitrary owner's kiln), and a wrong invented ceiling either does nothing
 * (too high) or nags a correctly-configured kiln (too low). Refusing
 * instead cannot be silently wrong, matches the ramp-ceiling refusal in
 * profile_executor_run() (same field-is-zero-means-uncommissioned
 * reasoning, same message shape), and matches autotune_engine_run_relay()'s
 * own guard-5 refusal for the identical reason (autotune_engine.c ~line
 * 1164, this task's FILES YOU OWN excludes that file so it is read-only
 * precedent here, not touched). autotune_engine_run() (the step-test path)
 * is the one place in the repo that deliberately tolerates max_temp_c == 0
 * -- see its STEP_TEST_GUARD_HEADROOM_C comment -- because a step test is a
 * short, operator-watched open-loop probe, not an unattended multi-hour
 * firing; that carve-out does not apply here.
 *
 * Pulled out to its own function (rather than left inline in
 * profile_executor_run()) purely so a host test can drive it directly
 * against a profile_t without needing profile_executor_start()'s full
 * FreeRTOS/relay/thermocouple harness -- see
 * test_profile_executor_prestart.c's test_profile_zones_have_ceiling_*.
 *
 * Checked against zones that can actually command heat, not merely against
 * p->zone_mask (2026-08-27, revised after the owner's live board reply:
 * heaters are now physically wired, and a real GET /api/zones read back
 * zone1/zone2 at max_temp_c==0, control_mode==0/OFF, never assigned to any
 * profile that actually drives them -- exactly the case this carve-out
 * exists for). ZONE_CONTROL_MODE_OFF (zones_http.h) "never commands heat" --
 * confirmed by reading heater_output_duty()'s switch in this file, which has
 * no case that can assert a relay for an OFF zone. Guard 5 exists to catch a
 * runaway zone that IS being driven; a zone this profile targets but that
 * cannot physically command a relay has nothing for guard 5 to protect
 * against, so refusing the whole firing over it would be a nuisance refusal
 * of exactly the kind SAFETY_MODEL.md's doctrine warns against, and the
 * fastest way to get this check disabled by whoever hits it. This mirrors
 * profile_executor_run()'s own n_heating_zones logic just below (same
 * "OFF stays a valid per-zone choice" reasoning, same zones_config_get_
 * control_mode() call, same OFF-is-the-safe-fallback-on-read-failure
 * default). Returns false and, if out_missing_zone is non-NULL, the first
 * (lowest-index) offending zone the instant any zone that CAN heat reads
 * max_temp_c == 0; returns true when every zone this profile can actually
 * drive has a real ceiling (including the case where none of them can heat
 * at all -- profile_executor_run()'s separate all-OFF refusal owns that
 * case, not this function). */
bool profile_zones_have_ceiling(const profile_t *p, uint8_t *out_missing_zone)
{
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!(p->zone_mask & (1u << zi))) continue;

        /* OFF, not BANGBANG, as the fallback if the getter fails -- same
         * fail-safe default profile_executor_run()'s n_heating_zones loop
         * uses: "we could not read this zone's control mode" must not be
         * read as "assume it can heat, and gate a real firing on it". */
        zone_control_mode_t mode = ZONE_CONTROL_MODE_OFF;
        zones_config_get_control_mode(zi, &mode);
        if (mode == ZONE_CONTROL_MODE_OFF) continue;

        float max_temp_c = 0.0f, min_temp_c = 0.0f;
        zones_config_get_temp_limits(zi, &max_temp_c, &min_temp_c);
        if (!(max_temp_c > 0.0f)) {
            if (out_missing_zone) *out_missing_zone = zi;
            return false;
        }
    }
    return true;
}

/* ---- Warm-start (PROFILES.md "Warm-start: joining a profile already at
 * temperature", owner request 2026-08-30) -----------------------------------
 *
 * Pure decision core, deliberately pulled out of profile_executor_run() the
 * same way profile_executor_wd_decide() (profile_executor.h) is pulled out
 * of watchdog_task_entry() -- no I/O, no locking, host-testable directly.
 * Decides ONLY where the schedule should enter (segment index, dwelling or
 * not, starting target_c, and how much of the entry segment's ramp time is
 * already spent); profile_executor_run() is the only caller and is the one
 * that turns "entry_segment_index > 0" into replayed RELAY_IO commands and a
 * log line.
 *
 * current_c is the reading profile_executor_run() decided is "current
 * temperature" for this decision -- Q4: the SAME coolest-active-zone reading
 * used elsewhere in this run's own start-of-run baseline, not a second,
 * differently-sourced notion of "now". NAN means no valid reading was
 * available at all (an absent thermocouple bus, every active zone's sensor
 * unhealthy) -- the safe answer is never skip anything without knowing where
 * the kiln actually is, so this returns the exact pre-feature default
 * (segment 0, target_c = the profile's own first segment target, not
 * warm-started).
 *
 * Q2, mid-ramp entry: prev_level tracks the temperature the CURRENT
 * ZONE_RAMP segment under examination ramps FROM (its predecessor's own
 * target, or current_c for segment 0 -- segment 0 has always ramped from
 * "wherever the kiln actually is right now", warm-start or not, which is
 * exactly why the i==0 case below produces byte-identical output to the
 * pre-feature code whether or not a warm start ends up happening). The first
 * segment whose OWN target_c is at or above current_c is where the ramp
 * would cross current_c, so that is where the run enters -- at the crossing
 * point (entry_target_c = current_c, carrying the remaining ramp distance/
 * time automatically: profile_executor_run() seeds s_exec.target_c with
 * entry_target_c and the ordinary per-tick ramp step in the control loop
 * takes it from there), never at the segment's own start (which would be
 * BELOW current_c and reintroduce the bug this feature exists to remove).
 *
 * Q3, dwell segments: deliberately NOT special-cased. A segment already at
 * or above current_c for its own target enters as a DWELL with
 * entry_segment_elapsed_s == 0 -- the full configured soak still runs. The
 * tempting wrong optimization would be "we're already at temperature, count
 * the soak as satisfied too" -- a soak is time AT temperature, not time
 * spent arriving there, and this function never shortens one.
 *
 * Q5, falling edges: the loop scans segments in profile order and BREAKS at
 * the first ZONE_RAMP segment whose target_c is below the level the
 * previous ZONE_RAMP segment left off at -- the "leading ascent" only.
 * RELAY_IO segments are skipped over (their target_c is meaningless, Q1) and
 * never count as an ascent or a descent themselves. A profile that comes
 * down again after climbing (a controlled cool, an anneal) has that descent
 * entirely out of reach of this function -- if current_c is not reached
 * within the leading ascent, warm_started stays false rather than ever
 * considering a later, lower segment.
 *
 * Hotter-than-everything (falls off the end of the loop without ever
 * finding a segment whose target_c >= current_c): lands on the LAST
 * ZONE_RAMP segment of the leading ascent, entered as a DWELL from its own
 * start (entry_segment_elapsed_s == 0) -- i.e. treated the same as "already
 * at this segment's target" above, running that segment's full configured
 * soak. Rejected alternatives and why:
 *   - Refuse to start: the kiln is at a perfectly fireable temperature
 *     (hotter than the profile only means "further along than planned"),
 *     and refusing a start over that would make the feature this exists to
 *     fix -- "firing back-to-back loads... wastes hours" -- worse, not
 *     better, for the exact case (a kiln that never fully cooled) the
 *     owner's request opens with.
 *   - Silently mark DONE / skip straight to whatever comes after the ascent
 *     (a cooling leg, or end of profile): would skip the top segment's own
 *     dwell -- Q3's "a soak is time at temperature" applies here at least as
 *     much as it does mid-profile; the top of the ascent is usually the
 *     whole point of the firing (the final maturing soak of a glaze/bisque
 *     schedule) and is exactly the segment a "just run the last dwell"
 *     choice must not shortcut.
 *   Landing on the top segment's dwell keeps that soak intact and then lets
 *   the ordinary segment-stepping machinery carry on from there completely
 *   unmodified -- if a cooling leg follows, it runs normally once the dwell
 *   finishes, same as any other run that reaches that point the ordinary
 *   way. */
profile_warm_start_plan_t profile_executor_plan_warm_start(const profile_t *p, float current_c)
{
    profile_warm_start_plan_t plan;
    memset(&plan, 0, sizeof(plan));
    plan.entry_segment_index = 0;
    plan.entry_dwelling = false;
    plan.entry_segment_elapsed_s = 0;
    plan.warm_started = false;

    if (p == NULL || p->segment_count == 0 || isnan(current_c)) {
        /* No usable reading (or nothing to scan) -- the pre-feature default:
         * segment 0's own target, ramping from wherever run() otherwise
         * decided the baseline was (it does not use entry_target_c in this
         * branch; see profile_executor_run()'s "only apply plan fields when
         * plan.warm_started" rule). */
        plan.entry_target_c = (p != NULL && p->segment_count > 0) ? p->segments[0].target_c : 0.0f;
        return plan;
    }

    /* Pass 1 (Q5): find the leading-ascent boundary using ONLY the
     * segments' own target_c sequence, completely independent of current_c.
     * Doing this as its own pass (rather than seeding the walk below's
     * "previous level" with current_c and testing descent against THAT)
     * matters: segment 0 always ramps from wherever the kiln actually is
     * (current_c), so a kiln reading above segment 0's own target would
     * otherwise look like an immediate "descent" relative to current_c and
     * wrongly cut the ascent down to nothing, even on a purely ascending
     * profile -- exactly the "hotter than segment 0" case this feature
     * exists to handle, not a real cool-down/anneal profile at all. */
    uint8_t ascent_end = p->segment_count; /* exclusive */
    bool have_last_seg_target = false;
    float last_seg_target = 0.0f;
    for (uint8_t i = 0; i < p->segment_count; i++) {
        const profile_segment_t *seg = &p->segments[i];
        if (seg->seg_kind == PROFILE_SEG_KIND_RELAY_IO) {
            continue; /* no temperature of its own -- neither ascent nor descent */
        }
        if (have_last_seg_target && seg->target_c < last_seg_target) {
            ascent_end = i;
            break;
        }
        last_seg_target = seg->target_c;
        have_last_seg_target = true;
    }

    /* Pass 2 (Q2/Q3/Q4): walk the leading ascent looking for where current_c
     * fits. prev_level starts at current_c -- segment 0 (or the first
     * ZONE_RAMP segment, if segment 0 is a RELAY_IO) has always ramped from
     * "wherever the kiln actually is right now", warm-start or not, which is
     * why landing here at i==0 produces byte-identical output to the
     * pre-feature code. Every later ZONE_RAMP segment instead uses the
     * PRECEDING segment's own target_c as prev_level -- current_c already
     * played its one role (picking the ascent boundary above) and must not
     * also masquerade as an earlier segment's target here. */
    float prev_level = current_c;
    for (uint8_t i = 0; i < ascent_end; i++) {
        const profile_segment_t *seg = &p->segments[i];
        if (seg->seg_kind == PROFILE_SEG_KIND_RELAY_IO) {
            continue; /* Q1: no temperature of its own */
        }

        if (current_c <= seg->target_c) {
            /* This segment's ramp reaches (or already starts at/above)
             * current_c -- this is where the run enters. */
            plan.entry_segment_index = i;
            plan.entry_dwelling = false;
            if (current_c <= prev_level) {
                /* Kiln is at/above where this segment's own ramp starts --
                 * i==0 always lands here (prev_level == current_c), which is
                 * exactly the pre-feature start: segment 0, offset 0,
                 * ramping from current_c. Not a warm start. A later segment
                 * can also land here if an earlier one's target already
                 * reached/exceeded current_c -- entering it at its own start
                 * is correct and IS a warm start (segments before it were
                 * skipped). */
                plan.entry_target_c = prev_level;
                plan.entry_segment_elapsed_s = 0;
                plan.warm_started = (i > 0);
            } else if (seg->ramp_c_per_hr > 0.0f) {
                /* Q2: mid-ramp entry. Seeding target_c at current_c (instead
                 * of prev_level) is what carries the remaining ramp time --
                 * the ordinary per-tick ramp step takes it from there. The
                 * elapsed figure is reported for visibility only (Q6); the
                 * control loop does not consume it during a ramp. */
                plan.entry_target_c = current_c;
                plan.entry_segment_elapsed_s =
                    (uint32_t)(((current_c - prev_level) / seg->ramp_c_per_hr) * 3600.0f + 0.5f);
                plan.warm_started = true;
            } else {
                /* A step segment (no ramp rate configured) has no partial
                 * distance to carry -- it jumps straight to its target. */
                plan.entry_target_c = seg->target_c;
                plan.entry_segment_elapsed_s = 0;
                plan.warm_started = true;
            }
            return plan;
        }

        /* current_c is past this whole segment already -- keep scanning the
         * ascent, and remember this as the fallback "hotter than the whole
         * ascent" landing spot (see this function's header comment). */
        prev_level = seg->target_c;
        plan.entry_segment_index = i;
        plan.entry_target_c = seg->target_c;
        plan.entry_dwelling = true;
        plan.entry_segment_elapsed_s = 0;
        plan.warm_started = true;
    }
    return plan;
}

