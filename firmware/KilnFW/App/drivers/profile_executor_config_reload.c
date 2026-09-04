/* Mid-run config reload for one zone (TODO.md 6A.7, "config reload while
 * running") -- split out of profile_executor.c (2026-09-01, "files over 1500
 * lines should be broken up where it makes sense"). See
 * profile_executor_internal.h's own doc comment for the full multi-way split
 * this is one piece of. reload_config_if_changed() (the generation-counter
 * poll that decides WHEN to call this per zone) stays in profile_executor.c
 * itself, immediately alongside executor_task_entry() which is its only
 * caller. */

#include "profile_executor_internal.h"

#include <math.h>

#include "esp_log.h"

#include "zones_http.h"

bool reload_zone_config(uint8_t zi)
{
    zone_runtime_t *z = &s_exec.zones[zi];
    bool changed = false;

    /* Every getter here fails identically for a zone_index past thermo_count,
     * so the control-mode read doubles as "is this zone still configured at
     * all". A shrunk thermo_count must not silently drop a zone out of a
     * running firing -- the run's active set is what the profile was
     * validated against, and quietly editing it mid-flight would leave the
     * operator's dashboard and the physical kiln disagreeing about what is
     * being driven. Keep it active on its last-known settings, say so at
     * ERROR, and open its contacts: apply_relay() refuses to energize a zone
     * whose mask it can't read, but "refuses" there means "returns without
     * touching the hardware", which on its own would strand an already-closed
     * relay. */
    zone_control_mode_t mode = ZONE_CONTROL_MODE_OFF;
    if (!zones_config_get_control_mode(zi, &mode)) {
        ESP_LOGE(PE_TAG, "zone %u is active in this run but is no longer configured (thermo_count shrank?) -- "
                      "keeping it in the run on its last-known settings, forcing its relays off; "
                      "it cannot be re-energized while the config can't name its relays", zi);
        force_relay_mask_off(zi, z->relay_mask);
        return true;
    }

    /* Mask first, mode second: both may force the zone off, and doing the
     * mask change first means the mode change's force-off already acts on the
     * new mask instead of re-opening contacts that were just handed away. */
    uint8_t mask = 0;
    if (zones_config_get_relay_mask(zi, &mask) && mask != z->relay_mask) {
        ESP_LOGW(PE_TAG, "OPERATOR ACTION MID-FIRING: zone %u relay mask 0x%02X -> 0x%02X -- old mask forced off "
                      "before the new one is adopted", zi, z->relay_mask, mask);
        force_relay_mask_off(zi, z->relay_mask);
        z->relay_mask = mask;
        changed = true;
    }

    /* A mode change is a discontinuity in what the output even means (a duty
     * fraction vs. a hysteresis latch vs. nothing), so there is no meaningful
     * handover to attempt -- TODO.md 6A.2's bumpless rule is about carrying a
     * controller's own state across a tuning change, not about translating
     * between controllers. Drop the heat, start the new mode cold, and make
     * the operator's action loud in the log. */
    bool mode_changed = (mode != z->control_mode);
    if (mode_changed) {
        ESP_LOGW(PE_TAG, "OPERATOR ACTION MID-FIRING: zone %u control mode %d -> %d -- relays forced off and "
                      "the PID restarted cold; no handover is attempted between modes",
                 zi, (int)z->control_mode, (int)mode);
        force_zone_relay_off(zi);
        z->control_mode = mode;
        pid_reset(&z->pid_state);
        z->fuzzy_prev_effective_ki = 0.0f; /* no bump-transfer history to carry into a cold start */
        /* cooling_limited is a PID/PID_FUZZY-only diagnostic (see its field
         * comment, and profile_executor_internal.h's legal-state rule 2) --
         * a mode change away from PID must retire it here, at the moment
         * control_mode moves.
         *
         * The tick's own non-PID branches already clear it, but they only
         * run for zones that are `active && !faulted`, while THIS function
         * runs for every ACTIVE zone, faulted or not. So a zone that took a
         * per-zone guard trip with continue_on_zone_trip enabled -- active,
         * faulted, still carrying cooling_limited=true from its PID period --
         * and is then switched to BANGBANG/OFF by the operator would keep a
         * stale true against a non-PID control_mode forever: exactly rule 2's
         * illegal combination, which exec_mode_state_check() asserts on every
         * tick. Clearing it at the mode change closes that regardless of
         * whether the tick's switch will ever visit this zone again. */
        z->cooling_limited_hold_s = 0.0f;
        z->cooling_limited = false;
        changed = true;
    }

    /* Tuning edits are the case TODO.md 6A.7 exists for: they must land
     * without restarting the firing. pid_seed_bumpless() solves the integral
     * that reproduces the duty this zone last commanded under the NEW gains,
     * so the element keeps doing what it was doing and the new tuning takes
     * over from there -- instead of the step change a cold integral would
     * produce halfway up a ramp. */
    float kp = 0.0f, ki = 0.0f, kd = 0.0f;
    if (zones_config_get_pid(zi, &kp, &ki, &kd) &&
        (kp != z->pid_cfg.kp || ki != z->pid_cfg.ki || kd != z->pid_cfg.kd)) {
        ESP_LOGI(PE_TAG, "zone %u PID gains reloaded mid-firing: kp %.4g->%.4g, ki %.4g->%.4g, kd %.4g->%.4g",
                 zi, (double)z->pid_cfg.kp, (double)kp, (double)z->pid_cfg.ki, (double)ki,
                 (double)z->pid_cfg.kd, (double)kd);
        z->pid_cfg.kp = kp;
        z->pid_cfg.ki = ki;
        z->pid_cfg.kd = kd;
        /* PID_FUZZY covered here too, not just plain PID: it shares the same
         * pid_state/pid_cfg base gains, so an operator's tuning edit needs
         * the identical bumpless handling either way. */
        if (!mode_changed &&
            (z->control_mode == ZONE_CONTROL_MODE_PID || z->control_mode == ZONE_CONTROL_MODE_PID_FUZZY)) {
            if (z->actual_valid) {
                seed_bumpless_with_ff(z, zi, z->duty);
                /* Reseeded off the BASE gains (u_desired's split assumes
                 * cfg.ki, not whatever cell the fuzzy layer last picked), so
                 * that's the "previous effective Ki" hazard-3's bump-transfer
                 * should compare next fuzzy tick against. */
                z->fuzzy_prev_effective_ki = ki;
            } else {
                /* Seeding off a fabricated measurement would bake this tick's
                 * bad reading into the integral and keep driving from it long
                 * after the sensor recovers. A cold restart costs one
                 * transient; a poisoned integral costs the rest of the run. */
                pid_reset(&z->pid_state);
                z->fuzzy_prev_effective_ki = 0.0f;
            }
        }
        changed = true;
    }

    /* The identified plant model, re-read on the same path and for the same
     * reason as the gains above -- autotune writes both through zones_http at
     * one acceptance point, so an autotune that finishes DURING a firing (on a
     * zone this run isn't driving, which is the only way it can run at all)
     * must be able to switch this zone's feedforward on without the operator
     * restarting a multi-hour firing.
     *
     * And it needs the bumpless treatment more than a gain edit does, not
     * less: a model appearing where there was none takes u_ff from 0 to
     * whatever the kiln costs to hold at the current setpoint, which on a hot
     * kiln is most of the duty. Without re-seeding, that lands as an
     * instantaneous step on top of an integral that was built to supply the
     * same heat -- the element would go to full for as long as the integrator
     * needs to unwind. Re-seeding hands the same total duty over to the new
     * split between ff and I, and lets the PID walk from there. */
    if (zone_load_model(zi)) {
        ESP_LOGI(PE_TAG, "zone %u plant model reloaded mid-firing: feedforward %s (K_dc %.4g, tau %.4gs) -- "
                      "PID re-seeded so the duty split changes without the duty itself stepping",
                 zi, z->ff_enabled ? "ON" : "OFF", (double)z->ff_k_dc, (double)z->ff_tau_s);
        if (!mode_changed &&
            (z->control_mode == ZONE_CONTROL_MODE_PID || z->control_mode == ZONE_CONTROL_MODE_PID_FUZZY)) {
            if (z->actual_valid) {
                seed_bumpless_with_ff(z, zi, z->duty);
                z->fuzzy_prev_effective_ki = z->pid_cfg.ki; /* see the gain-reload path above */
            } else {
                pid_reset(&z->pid_state); /* same reasoning as the gain path above */
                z->fuzzy_prev_effective_ki = 0.0f;
            }
        }
        changed = true;
    }

    /* Same 0-means-not-configured substitution as run(), applied before the
     * comparison so an operator clearing a field back to blank reads as
     * "returned to the default", not as a spurious change every reload. */
    float window_ms = 0.0f, min_on_ms = 0.0f, min_off_ms = 0.0f;
    if (zones_config_get_heater_cfg(zi, &window_ms, &min_on_ms, &min_off_ms)) {
        uint32_t w = (window_ms > 0.0f) ? (uint32_t)window_ms : HEATER_WINDOW_MS;
        uint32_t on = (min_on_ms > 0.0f) ? (uint32_t)min_on_ms : HEATER_MIN_ON_MS;
        uint32_t off = (min_off_ms > 0.0f) ? (uint32_t)min_off_ms : HEATER_MIN_OFF_MS;
        if (w != z->heater_cfg.window_ms || on != z->heater_cfg.min_on_ms || off != z->heater_cfg.min_off_ms) {
            ESP_LOGI(PE_TAG, "zone %u heater timing reloaded mid-firing: window %lu->%lums, min_on %lu->%lums, "
                          "min_off %lu->%lums (current window finishes on the old values)",
                     zi, (unsigned long)z->heater_cfg.window_ms, (unsigned long)w,
                     (unsigned long)z->heater_cfg.min_on_ms, (unsigned long)on,
                     (unsigned long)z->heater_cfg.min_off_ms, (unsigned long)off);
            z->heater_cfg.window_ms = w;
            z->heater_cfg.min_on_ms = on;
            z->heater_cfg.min_off_ms = off;
            changed = true;
        }
    }

    /* Guard thresholds are TODO.md 6A.7's "more dangerous" branch: the doc
     * offers "require the zone to be idle, or log it loudly as an operator
     * action", and this takes the second option -- refusing the edit outright
     * would be worse in the case that actually matters, an operator who has
     * just realised a ceiling is wrong for the ware in the kiln right now and
     * needs it corrected without aborting a multi-hour firing. So each
     * threshold moves immediately and each one is logged individually at WARN
     * with old -> new, because the log is the only record that the protection
     * envelope this firing ran under is not the one the zone is configured
     * with today. Note this can only ever change what trips NEXT tick -- an
     * already-latched trip is untouched (see this function's header). */
    float max_temp_c = 0.0f, min_temp_c = -20.0f;
    if (zones_config_get_temp_limits(zi, &max_temp_c, &min_temp_c)) {
        if (max_temp_c != z->guard_cfg.max_temp_c) {
            ESP_LOGW(PE_TAG, "OPERATOR ACTION MID-FIRING: zone %u guard max_temp_c %.1f -> %.1f",
                     zi, (double)z->guard_cfg.max_temp_c, (double)max_temp_c);
            z->guard_cfg.max_temp_c = max_temp_c;
            changed = true;
        }
        if (min_temp_c != z->guard_cfg.min_temp_c) {
            ESP_LOGW(PE_TAG, "OPERATOR ACTION MID-FIRING: zone %u guard min_temp_c %.1f -> %.1f",
                     zi, (double)z->guard_cfg.min_temp_c, (double)min_temp_c);
            z->guard_cfg.min_temp_c = min_temp_c;
            changed = true;
        }
    }

    float sanity_rate = 0.0f;
    if (zones_config_get_sanity_rate(zi, &sanity_rate)) {
        float applied = (sanity_rate > 0.0f) ? sanity_rate : PROFILE_EXECUTOR_DEFAULT_SANITY_RATE_C_PER_MIN;
        if (applied != z->guard_cfg.sanity_rate_c_per_min) {
            ESP_LOGW(PE_TAG, "OPERATOR ACTION MID-FIRING: zone %u guard sanity_rate_c_per_min %.3f -> %.3f",
                     zi, (double)z->guard_cfg.sanity_rate_c_per_min, (double)applied);
            z->guard_cfg.sanity_rate_c_per_min = applied;
            changed = true;
        }
    }

    /* TODO.md 6A.3's remaining named thresholds -- same "raw pass-through,
     * thermal_guard.c owns the 0->default substitution" reasoning as run()'s
     * own guard_cfg build above, and the same loud-logging-per-field
     * discipline as every other guard threshold in this function. */
    {
        float wd_window_s = 0.0f, wd_rate = 0.0f, off_settle_s = 0.0f, runaway_rate = 0.0f;
        float runaway_margin = 0.0f, drift_period_s = 0.0f, debounce_ticks = 0.0f, frozen_window_s = 0.0f;
        if (zones_config_get_guard_thresholds(zi, &wd_window_s, &wd_rate, &off_settle_s, &runaway_rate,
                                              &runaway_margin, &drift_period_s, &debounce_ticks,
                                              &frozen_window_s)) {
#define RELOAD_GUARD_FIELD(field, new_val, fmt)                                                          \
            if ((new_val) != z->guard_cfg.field) {                                                       \
                ESP_LOGW(PE_TAG, "OPERATOR ACTION MID-FIRING: zone %u guard " #field " " fmt " -> " fmt,     \
                         zi, (double)z->guard_cfg.field, (double)(new_val));                              \
                z->guard_cfg.field = (new_val);                                                           \
                changed = true;                                                                           \
            }
            RELOAD_GUARD_FIELD(wrong_dir_window_s, wd_window_s, "%.1f")
            RELOAD_GUARD_FIELD(wrong_dir_rate_c_per_min, wd_rate, "%.3f")
            RELOAD_GUARD_FIELD(off_settle_s, off_settle_s, "%.1f")
            RELOAD_GUARD_FIELD(runaway_rate_c_per_min, runaway_rate, "%.3f")
            RELOAD_GUARD_FIELD(runaway_margin_c, runaway_margin, "%.1f")
            RELOAD_GUARD_FIELD(drift_period_s, drift_period_s, "%.1f")
            RELOAD_GUARD_FIELD(sensor_fault_debounce_ticks, debounce_ticks, "%.0f")
            RELOAD_GUARD_FIELD(frozen_window_s, frozen_window_s, "%.1f")
            {
                /* The five v8 overrides reload mid-firing on exactly the same
                 * terms as the eight above -- an operator who widens a window
                 * during a run must see it take effect, and must see it
                 * logged. */
                float progress_duty_min = 0.0f, progress_window_s = 0.0f, drift_hysteresis_c = 0.0f;
                float frozen_eps_c = 0.0f, cross_zone_period_s = 0.0f;
                if (zones_config_get_guard_extra(zi, &progress_duty_min, &progress_window_s,
                                                 &drift_hysteresis_c, &frozen_eps_c, &cross_zone_period_s)) {
                    RELOAD_GUARD_FIELD(progress_duty_min, progress_duty_min, "%.3f")
                    RELOAD_GUARD_FIELD(progress_window_s, progress_window_s, "%.1f")
                    RELOAD_GUARD_FIELD(drift_hysteresis_c, drift_hysteresis_c, "%.1f")
                    RELOAD_GUARD_FIELD(frozen_eps_c, frozen_eps_c, "%.3f")
                    RELOAD_GUARD_FIELD(cross_zone_period_s, cross_zone_period_s, "%.1f")
                }
            }
#undef RELOAD_GUARD_FIELD
        }
    }

    float cross_zone_delta_c = 0.0f;
    if (zones_config_get_cross_zone_delta(zi, &cross_zone_delta_c) &&
        cross_zone_delta_c != z->guard_cfg.cross_zone_max_delta_c) {
        /* 0 here disarms guard 8 entirely (zones_http.h's deliberate opposite
         * convention to sanity_rate), so this particular edit can silently
         * remove a protection rather than merely widen it -- all the more
         * reason for it to be in the log by name. */
        ESP_LOGW(PE_TAG, "OPERATOR ACTION MID-FIRING: zone %u guard cross_zone_max_delta_c %.1f -> %.1f%s",
                 zi, (double)z->guard_cfg.cross_zone_max_delta_c, (double)cross_zone_delta_c,
                 (cross_zone_delta_c <= 0.0f) ? " (guard 8 now DISABLED for this zone)" : "");
        z->guard_cfg.cross_zone_max_delta_c = cross_zone_delta_c;
        changed = true;
    }

    /* TODO.md 6A.7's "re-check max_ramp_c_per_hr against a running profile":
     * this ceiling is a run-*start* feasibility gate (profiles_http.c), so
     * an operator lowering it mid-firing below what the running segment
     * demands previously went unnoticed until the ware finished. This does
     * not abort or throttle anything -- the ramp itself is unaffected, same
     * as every other guard-threshold edit above -- it only makes the gap
     * loud in the log, once per zone per time it newly becomes infeasible
     * (not every tick), mirroring this function's existing pattern for
     * guard thresholds. z->max_ramp_warned resets the instant the segment
     * changes or the ceiling is raised back above it, so a real re-trip
     * after that logs again instead of staying silently latched. */
    {
        float ceiling = 0.0f;
        /* `ceiling > 0.0f` here would mean this one site treats 0 as "no
         * ceiling", while the start check above, profile_feasibility.c and
         * profiles_http.c all treat 0 as "every rate is over it". Same value,
         * opposite policy, inside one feature. Fail-closed is the agreed
         * reading (an uncommissioned zone should not fire), so this warning
         * follows it: a zone whose ceiling was zeroed mid-firing is exactly
         * the case worth shouting about, and skipping it was the quietest
         * possible response to it. */
        bool have_ceiling = zones_config_get_max_ramp(zi, &ceiling);
        const profile_segment_t *seg =
            (s_exec.segment_index < s_exec.profile.segment_count)
                ? &s_exec.profile.segments[s_exec.segment_index]
                : NULL;
        bool now_infeasible = have_ceiling && seg && seg->ramp_c_per_hr > ceiling;
        if (now_infeasible && !z->max_ramp_warned) {
            ESP_LOGW(PE_TAG, "OPERATOR ACTION MID-FIRING: zone %u max_ramp_c_per_hr lowered to %.1f, below the "
                          "current segment's %.1f C/hr -- the running ramp is UNCHANGED, this only flags that "
                          "it now exceeds the configured ceiling", zi, (double)ceiling, (double)seg->ramp_c_per_hr);
            z->max_ramp_warned = true;
        } else if (!now_infeasible) {
            z->max_ramp_warned = false;
        }
    }

    return changed;
}

/* One counter comparison per tick, and on the overwhelmingly common
 * unchanged path that is the entire cost -- no getters, no config walk, no
 * NVS. Must be called with s_exec.lock held, from the RUNNING path only:
 * a PAUSED or FAULTED run has no control math to keep bumpless, and picking
 * the edit up when it resumes (via this same path) is both simpler and
 * closer to what the operator expects. */
/* The four per-zone executor thresholds the owner asked to stop being magic
 * numbers (v8). Unlike the guard thresholds -- which thermal_guard.c
 * substitutes for, so its constants stay the single source of the default --
 * these are this module's own numbers, so the 0 -> named-default substitution
 * belongs here. A zone that has never been configured, or an index past
 * thermo_count, reads exactly the constant that was hardcoded before. */
