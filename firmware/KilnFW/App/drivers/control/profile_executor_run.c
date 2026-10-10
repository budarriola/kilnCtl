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
#include "danger_mode.h"
#include "backup_restore_state.h" /* backup_import_restore_in_flight() -- 2026-09-28
                                    * A4 review follow-up A */
#include "heat_enable.h"
#include "kiln_io_owner.h"
#include "live_profile.h"
#include "ota_state.h"
#include "profiles_builtin.h" /* PROFILE_BUILTIN_ID_BASE -- HP-02 refusal exempts builtins */
#include "profile_rule_target.h" /* spare-relay WP-3: aux rule targets 8..11 */
#include "profiles_store.h"
#include "readiness_gate.h"
#include "relay_authority.h"
#include "relay_off_tracker.h"
#include "safety_trip_words.h"
#include "sim_backend.h"
#include "system_mode_gate.h"
#include "thermo_channel_read.h"
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

/* See profile_executor_run()'s own call-site comment for the full "why".
 * Called with s_exec.lock already held (same as clear_this_runs_faults(),
 * which this is the run-START counterpart to) -- relay_authority is a leaf,
 * so no ordering concern calling it from here. Exposed via
 * profile_executor_internal.h so the host tests can exercise it directly,
 * the same way they call escalate_guard_trip()/clear_this_runs_faults(). */
void clear_stale_zone_latches_for_new_run(uint8_t zone_mask)
{
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (zone_mask & (1u << zi)) {
            relay_authority_set_zone_blocked(zi, false);
        }
    }
}

/* The one predicate for "this zone's reading/state is part of the run's shared
 * temperature drive": not an on/off zone (it has no obligation to the shared
 * setpoint, ON_OFF_ZONE.md sec 1) and not monitor-only (never driven,
 * SPARE_RELAY_ONOFF_PLAN.md sec 10). Used by the baseline pick, the warm-start
 * coolest pick and the ramp-lock loop so the three cannot disagree. Deliberately
 * NOT used by the ramp-rate-ceiling feasibility check: an on/off zone has a
 * ceiling it is still validated against there. */
bool profile_executor_zone_drives_run(uint8_t zi)
{
    return !zone_is_on_off(zi) && !zone_is_monitor_only(zi);
}

/* Lowest zone in zone_mask that drives the run
 * (profile_executor_zone_drives_run(): neither on/off nor monitor-only), or
 * -1 if there is none. The run-start temperature baseline and the warm-start
 * pick both key off this (profile_executor_capture_baseline() is its only
 * caller, and profile_executor_run() applies that capture without
 * re-picking): an on/off zone has no obligation to the shared setpoint and a
 * monitor-only zone (HEATER with relay_mask==0, zone_is_monitor_only(), the
 * one rule) is never driven, so neither reading may seed the run's target or
 * decide a warm start. A mask with no such zone returns -1; the capture then
 * yields no baseline/warm-start (fail closed). An all-monitor-only mask is
 * refused at start anyway via the n_heating_zones == 0 check. */
static int8_t profile_executor_baseline_zone(uint8_t zone_mask)
{
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if ((zone_mask & (1u << zi)) && profile_executor_zone_drives_run(zi)) {
            return (int8_t)zi;
        }
    }
    return -1;
}

/* Baseline thermocouple snapshot for the ramp-start/warm-start/ambient seeds
 * set further down (under s_exec.lock). Read HERE, before s_exec.lock is
 * ever taken -- CLAUDE.md's "never hold a module lock across a producer
 * call": a MAX31856 SPI transaction can block on the bus / thermo owner
 * queue for up to ~200ms, and profile_executor_run() used to do this exact
 * read while already holding s_exec.lock, blocking every other s_exec.lock
 * caller (the dashboard/LCD status readers, profiles_stop(), etc.) for that
 * whole window.
 *
 * first_active (the lowest zone index set in zone_mask that drives the run,
 * see profile_executor_baseline_zone()) is purely a property
 * of the profile being started -- it does not depend on any s_exec state
 * guarded by the lock, so it is safe to compute here, before that state
 * (s_exec.zones[]) is even touched. zones_config_get_thermo_mask()/
 * zones_config_apply_cal() are config-store accessors with their own
 * locking, called elsewhere in this file both under and outside
 * s_exec.lock already -- s_exec.lock never guards them.
 *
 * If s_exec.state changes between this call and the lock being taken (e.g.
 * another run starts first), the caller's existing state==RUNNING/PAUSED/
 * FAULTED refusal right after xSemaphoreTake() already rejects this attempt
 * before any of this snapshot is used -- so no separate re-validation is
 * needed here; a discarded snapshot on a refused start is harmless. */
static void profile_executor_capture_baseline(uint8_t zone_mask,
                                               bool *out_baseline_valid, float *out_baseline_c,
                                               float *out_warm_start_coolest_c,
                                               bool *out_ambient_valid, float *out_ambient_c)
{
    *out_baseline_valid = false;
    *out_baseline_c = NAN;
    *out_warm_start_coolest_c = NAN;
    *out_ambient_valid = false;
    *out_ambient_c = 0.0f;

    if (!(sim_backend_enabled() || (s_exec.thermo_bus && s_exec.thermo_bus->initialized))) {
        return;
    }

    int8_t first_active = profile_executor_baseline_zone(zone_mask);
    if (first_active < 0) {
        return;
    }

    ThermoChannelSnapshot snap;
    thermo_channels_read(s_exec.thermo_bus, &snap);
    const float *base_ch_c = snap.raw_c;
    const bool *base_ch_ok = snap.ok;

    uint8_t base_tmask = 0;
    zones_config_get_thermo_mask((uint8_t)first_active, &base_tmask);
    bool base_valid = false;
    float base_combined =
        thermo_combine(base_ch_c, base_ch_ok, MAX31856_CHANNEL_COUNT, base_tmask, &base_valid);
    if (base_valid) {
        *out_baseline_valid = true;
        *out_baseline_c = zones_config_apply_cal((uint8_t)first_active, base_combined);
    }

    /* Warm-start's "current temperature" (Q4): the coolest ACTIVE zone's own
     * combined+calibrated reading, from this same already-fetched `readings`
     * array -- every active zone gets its own thermo_mask/thermo_combine/
     * apply_cal treatment here (not just first_active's), because a zone
     * other than first_active can legitimately be the coolest one. */
    for (uint8_t wzi = 0; wzi < MAX31856_CHANNEL_COUNT; wzi++) {
        if (!(zone_mask & (1u << wzi))) continue;
        if (!profile_executor_zone_drives_run(wzi)) continue; /* on/off and monitor-only zones never drive the warm-start pick */
        uint8_t w_tmask = 0;
        zones_config_get_thermo_mask(wzi, &w_tmask);
        bool w_valid = false;
        float w_combined = thermo_combine(base_ch_c, base_ch_ok, MAX31856_CHANNEL_COUNT, w_tmask, &w_valid);
        if (!w_valid) continue;
        float w_c = zones_config_apply_cal(wzi, w_combined);
        if (isnan(*out_warm_start_coolest_c) || w_c < *out_warm_start_coolest_c) {
            *out_warm_start_coolest_c = w_c;
        }
    }

    /* Feedforward's ambient reference (TODO.md 6A.2), taken from THIS read
     * rather than a second one: the cold junction is only honest about the
     * room before the firing has warmed the board, and this is the last
     * moment that is true. Any channel's CJ will do -- all five sit on the
     * same board within centimetres of each other, and accepting the first
     * valid one means a single dead or CJRANGE-flagged channel doesn't cost
     * the whole run its ambient. */
    for (uint8_t ci = 0; ci < MAX31856_CHANNEL_COUNT; ci++) {
        if (!isnan(snap.cj_c[ci])) {
            *out_ambient_valid = true;
            *out_ambient_c = snap.cj_c[ci];
            break;
        }
    }
}

bool profile_executor_run(uint8_t profile_id, char *err_msg, size_t err_cap)
{
    /* Zones config generation before ANY config read below; re-checked under zones_cfg_lock()
     * after the heat claim is published (see the "zones config changed" late check). */
    const uint32_t gen_at_entry = zones_config_generation();

    /* THE READINESS INTERLOCK (owner decision 2026-09-09; readiness_gate.h
     * has the full rationale and the standing "NO OVERRIDE" instruction).
     *
     * Placed HERE, in profile_executor_run(), rather than at the HTTP door,
     * because this function is the single choke point every start path funnels
     * through: POST /api/profile_exec/start (dashboard_exec_http.c), both LCD
     * start buttons (ui_page_home_actions.c, ui_page_profile_detail.c) and the
     * benchproto RUN command (uart_bridge_ext_control.c). A gate at any one
     * door would have left the other three open, and the next door added would
     * have started life ungated. dashboard_exec_http.c ALSO checks it before
     * reading the request body, purely so the HTTP client gets a 409 with the
     * item named rather than a generic 400 -- that is a legibility duplicate of
     * this check, never a substitute for it.
     *
     * Checked FIRST -- before even the s_exec.lock == NULL guard below --
     * for two reasons. (a) The answer must not depend on which profile was
     * asked for or on how far bring-up got: these four conditions are facts
     * about the BOARD, and "which item is red" must read the same whatever is
     * being started. (b) In recovery mode the lock IS NULL, and the generic
     * "profile executor not started" refusal below tells an operator nothing
     * about why. Reaching the gate first means the recovery-mode refusal
     * names recovery mode. Nothing here touches s_exec, so running before
     * that guard is safe; readiness_gate.c documents the fail-safe direction
     * of every fact it reads on a board that has not finished starting.
     *
     * Slice 2 (docs/SYSTEM_MODE_GATE.md section 3.6): collects the
     * readiness facts once and runs system_mode_gate_check()'s recovery-mode
     * rule against them BEFORE readiness_gate_evaluate() -- the same facts,
     * no second collect -- so the recovery-mode wording an operator sees here
     * is the SAME string dashboard_exec_http.c now sends over HTTP (retiring
     * App/drivers/http/recovery_start_refusal.h's separate, HTTP-only
     * wording), and the UART/LCD start paths get it too since they funnel
     * through this same function. */
    {
        readiness_gate_facts_t facts;
        readiness_gate_collect(&facts);

        sys_mode_snapshot_t mode_snap;
        memset(&mode_snap, 0, sizeof(mode_snap));
        mode_snap.recovery_mode = facts.recovery_mode;
        /* 2026-09-28, A4 review follow-up A: a backup restore's commit pass
         * writes the same profile/zone state this start would read -- refuse
         * while backup_import.c has one in flight, same choke point as the
         * recovery-mode check just above. */
        mode_snap.restore_in_flight = backup_import_restore_in_flight();
        mode_snap.danger_mode_active = danger_mode_blocks_start(); /* LCD review N2; R3: fail closed */
        if (system_mode_gate_check(SYS_ACTION_START_PROFILE, &mode_snap, err_msg, err_cap)) {
            ESP_LOGW(PE_TAG, "profile_executor_run(%u) refused by the system mode gate (recovery mode, restore or danger mode)",
                     (unsigned)profile_id);
            return false;
        }

        readiness_gate_block_t which = readiness_gate_evaluate(&facts, err_msg, err_cap);
        if (which != READINESS_GATE_OK) {
            ESP_LOGW(PE_TAG, "profile_executor_run(%u) refused by the readiness interlock (item %d): %s",
                     (unsigned)profile_id, (int)which, err_msg ? err_msg : "(no message)");
            return false;
        }
    }


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

    /* L23 residual: capture the slot's save revision BEFORE the copy. */
    const uint32_t slot_rev = profiles_http_slot_rev(profile_id);
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
        /* CLAUDE.md's ota_rollback_esp() hazard, closed 2026-09-16: name what
         * actually happened -- a real, previously-tuned config this firmware
         * refused to decode (rollback past a schema bump, or a blob one
         * migration step short of what this build understands), not the
         * unrelated "nobody has configured this board yet" case the generic
         * message below still covers. zones_config_get_load_fault() answers
         * false (and leaves *fault unlatched) on a genuinely fresh board, so
         * that message is unchanged for that case. No override path exists
         * here on purpose -- see zones_config_get_load_fault()'s own doc
         * comment: this state means the board does not trust its own
         * tuning data, and firing on unknown gains/limits is exactly the
         * hazard this gate exists to close. */
        zones_cfg_load_fault_t fault;
        if (zones_config_get_load_fault(&fault)) {
            if (err_msg) {
                if (fault.kind == ZONES_CFG_LOAD_FAULT_NEWER) {
                    snprintf(err_msg, err_cap,
                             "stored zone config is version %u; this firmware only understands up to version %u "
                             "-- likely an OTA rollback past a config schema bump. Firing refused: reflash the "
                             "matching (or newer) firmware to restore the tuned config (see ota_rollback_esp() "
                             "in CLAUDE.md)",
                             (unsigned)fault.on_disk_version, (unsigned)fault.fw_version);
                } else {
                    snprintf(err_msg, err_cap,
                             "stored zone config is version %u and this firmware (version %u) cannot migrate it "
                             "forward: %s. Firing refused: reflash the firmware version that saved this config, "
                             "or reconfigure zones from scratch",
                             (unsigned)fault.on_disk_version, (unsigned)fault.fw_version, fault.reason);
                }
            }
        } else if (err_msg) {
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
        if (relay_authority_start_blocked(s_exec.safety, err_msg, err_cap,
                                          "a firing cannot start")) {
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

    /* Baseline/warm-start/ambient SPI read, taken before the lock -- see
     * profile_executor_capture_baseline()'s own doc comment. */
    bool baseline_valid = false;
    float baseline_c = NAN;
    float warm_start_coolest_captured = NAN;
    bool ambient_valid = false;
    float ambient_c_captured = 0.0f;
    profile_executor_capture_baseline(p.zone_mask, &baseline_valid, &baseline_c,
                                       &warm_start_coolest_captured,
                                       &ambient_valid, &ambient_c_captured);

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

    /* HTTP input parsing audit L23: `p` was copied before this lock, so a
     * profile delete (profiles_delete_slot(), profiles_http.c) may be erasing
     * the slot right now, or may have finished since. The delete sets its
     * in-flight mark BEFORE its own running check, which takes this same lock;
     * this re-check sits in the same locked section that commits RUNNING
     * below, so one side always sees the other. Lock-free read only: never
     * take the profiles save lock or wait on the flash worker under
     * s_exec.lock (order: flash worker -> save lock -> s_exec.lock). */
    if (!profiles_http_slot_runnable_rev(profile_id, slot_rev)) {
        xSemaphoreGive(s_exec.lock);
        if (err_msg) {
            snprintf(err_msg, err_cap, profiles_http_loaded()
                         ? "profile is being deleted, was deleted or was re-saved -- not started"
                         : "profiles still loading -- not started, retry in a moment");
        }
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
            /* SPARE_RELAY_ONOFF_PLAN.md sec 10: a monitor-only zone is never
             * driven, so a ramp-rate ceiling means nothing for it. */
            if (zone_is_monitor_only(zi)) continue;
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
        /* Spare-relay WP-3 (WP-4 hand-back item 2): a relay bound to an
         * ENABLED aux output belongs to the aux evaluator -- a RELAY_IO
         * segment on it would fight the aux rule. profiles_http.c refuses
         * this at save; an aux enabled AFTER the profile was saved (or a
         * profile imported before the aux existed) is caught here. */
        if ((aux_outputs_cfg_enabled_mask() & (uint8_t)(1u << (t - PROFILE_IO_TARGET_RELAY_BASE))) != 0) {
            xSemaphoreGive(s_exec.lock);
            ESP_LOGW(PE_TAG, "refusing run: segment %u RELAY_IO targets relay %u, now bound to an enabled aux output",
                     (unsigned)i + 1u, (unsigned)t);
            if (err_msg) {
                snprintf(err_msg, err_cap,
                         "segment %u: relay %u is now bound to an aux output -- this profile cannot run "
                         "until that segment's target is changed",
                         i + 1, t);
            }
            return false;
        }
    }

    /* Spare-relay WP-3 (WP-4 hand-back items 1 and 4): re-check every aux
     * on/off rule target against the CURRENT aux store and zones. Rules
     * already in NVS are not re-validated on load, and an aux can be
     * disabled, conflicted, or have its relay assigned to a zone after the
     * profile was saved, so the save-time check alone is not enough. A rule
     * in a segment past segment_count can never run and is ignored, matching
     * the resolver. Same refusal shape as the zone-ownership check above. */
    for (uint8_t ri = 0; ri < p.on_off_rule_count && ri < PROFILE_MAX_ON_OFF_RULES; ri++) {
        const profile_on_off_rule_t *pr = &p.on_off_rules[ri];
        if (!pr->enable || !profile_rule_target_is_aux(pr->zone_index) || pr->segment_index >= p.segment_count) {
            continue;
        }
        uint8_t relay = profile_rule_target_aux_relay(pr->zone_index);
        aux_output_t ax;
        const char *why = NULL;
        uint8_t owning_zone = 0;
        char why_buf[96];
        if (!aux_outputs_cfg_get(relay, &ax)) {
            why = "cannot be read";
        } else if (ax.conflicted) {
            why = "is conflicted (a zone also claims that relay)";
        } else if (!ax.enabled) {
            why = "is not an enabled aux output";
        } else if (relay_io_target_is_zone_owned(relay, &owning_zone)) {
            snprintf(why_buf, sizeof(why_buf), "is now assigned to zone %u", (unsigned)owning_zone);
            why = why_buf;
        } else if (pr->temp_source == 1 && pr->temp_cmp != ON_OFF_TEMP_CMP_NONE) {
            /* A temperature axis reads the aux's own thermocouple zone out
             * of the executor's per-zone readings, which only exist for the
             * zones this profile runs. */
            if (ax.tc_zone == AUX_TC_ZONE_NONE) {
                why = "has a temperature rule but no thermocouple zone set";
            } else if (!(p.zone_mask & (1u << ax.tc_zone))) {
                snprintf(why_buf, sizeof(why_buf), "reads zone %u's thermocouple, which is not in this profile",
                         (unsigned)ax.tc_zone);
                why = why_buf;
            }
        }
        if (why) {
            xSemaphoreGive(s_exec.lock);
            if (err_msg) {
                snprintf(err_msg, err_cap,
                         "segment %u: aux relay %u %s -- this profile cannot run until that rule is "
                         "fixed or removed",
                         (unsigned)pr->segment_index + 1u, (unsigned)relay, why);
            }
            ESP_LOGW(PE_TAG, "refusing run: aux relay %u rule in segment %u: %s", (unsigned)relay,
                     (unsigned)pr->segment_index + 1u, why);
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
    s_exec.segment_elapsed_rem_ms = 0;
    s_exec.ramp_lock_held = false;
    s_exec.ramp_lock_lagging_mask = 0;
    /* docs/audits/profile_executor_panic_2026-09-24.md: latched per-run so a
     * violation forces FAULTED exactly once; a fresh run starts un-latched.
     * mode_state_violation_count is a lifetime-of-this-boot diagnostics
     * counter and is deliberately NOT reset here. */
    s_exec.mode_state_fault_latched = false;
    memset(s_exec.stretch_by_segment_s, 0, sizeof(s_exec.stretch_by_segment_s));
    s_exec.stretch_total_s = 0.0f;
    /* PID_EXPANSION_PLAN.md sec 7.3: dwell credit -- a previous run's last
     * applied spend has no meaning against a freshly (re)started schedule,
     * same "starts owing nothing" reasoning as stretch_total_s just above.
     * Per-zone dwell_credit_s is zeroed by the
     * memset(s_exec.zones, ...) a few lines down. */
    s_exec.dwell_credit_applied_s = 0.0f;
    s_exec.fault_reason[0] = '\0';
    s_exec.pause_reason[0] = '\0';
    s_exec.fault_guard = THERMAL_GUARD_TRIP_NONE;
    s_exec.global_fault_source = 0;
    /* Per-run, not cumulative: carrying a previous firing's claim forward
     * would let this run's sweep open a relay the last run once drove and an
     * operator has since taken over manually. Each run starts owing nothing
     * and claims what it touches (see s_exec_state_t.claimed_relay_mask). */
    s_exec.claimed_relay_mask = 0;
    /* zone_off_pending_mask is deliberately NOT zeroed here (review-2 MEDIUM-1): a bit still set is an OFF write that
     * has not landed, and forgetting it would strand a closed relay. Ownership is checked at retry time. */
    /* Spare-relay WP-3: per-run aux decision/actuation state starts owing
     * nothing too. aux_claim_mask is NOT zeroed here: a bit still set is a
     * run-end OFF write that failed and has not landed yet, and forgetting it
     * would strand a closed relay. The handoff below folds it in and writes
     * OFF to it again. */
    for (uint8_t ai = 0; ai < AUX_OUTPUTS_COUNT; ai++) {
        /* Holds seeded from the relay's own last ON-to-OFF time (relay_off_
         * tracker), not a flat "settled": a stop followed by a quick restart
         * must still wait out min_off_s. */
        profile_executor_aux_reset_runtime(ai);
        s_exec.aux[ai].on_time_s = 0.0f;
        s_exec.aux[ai].switch_count = 0;
    }
    /* Same "starts owing nothing" reasoning as claimed_relay_mask just above,
     * for the relay/IO segment machinery: a previous run's io_segs[] state
     * (which segment was active, what its remaining_s countdown was) has no
     * meaning against a freshly (re)started schedule. */
    memset(s_exec.io_segs, 0, sizeof(s_exec.io_segs));
    /* Warm-start state, same "starts owing nothing" reasoning -- overwritten
     * below if this run actually warm-starts. */
    s_exec.warm_started = false;
    s_exec.warm_start_reason[0] = '\0';
    /* MEDIUM-3 (review): a refusal from a PREVIOUS firing must never be
     * reported against this one. */
    memset(&s_exec.live_edit_last_refusal, 0, sizeof(s_exec.live_edit_last_refusal));
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
    /* Same "sample before this run can miss an edit" reasoning as the line
     * above, for the live-edit working slot (pass 1, section 3): a fork()
     * that landed moments before profile_executor_run() must not be lost
     * because this run's first tick already believed generation 0.
     *
     * MEDIUM-2 (review): seeding straight from live_profile_generation() is
     * wrong on a warm-start resume across a reboot, because that counter is
     * RAM-only and resets to 0 while a pending working copy on disk does
     * not (live_profile_generation()'s own doc comment, corrected). If this
     * exact profile_id already has a pending live edit, force this run's
     * baseline to NOT match the current generation, so the very first tick's
     * reload_live_profile_if_changed() poll sees a "change" and picks the
     * persisted edit up immediately, instead of silently treating it as
     * already-seen until some unrelated later edit bumps the counter again.
     * The ordinary (no pending edit for this profile) case is unaffected --
     * same cheap seed as before. */
    {
        uint32_t gen = live_profile_generation();
        s_exec.live_edit_generation = live_profile_has_pending_for_origin(profile_id) ? (gen - 1u) : gen;
    }

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
    uint8_t n_monitor_only_zones = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!(p.zone_mask & (1u << zi))) continue;
        if (zone_is_monitor_only(zi)) {
            n_monitor_only_zones++;
            ESP_LOGW(PE_TAG, "zone %u is in this profile but has no heater relay (monitor-only) -- it will not heat", zi);
            continue;
        }
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
        /* Name the real cause: a monitor-only zone (relay moved to an aux
         * output) has a control mode but no heater relay, so "control mode
         * OFF" alone would send the operator to the wrong field. Both
         * monitor-only strings fit the HTTP start handler's 128 B err_msg. */
        if (err_msg) {
            if (n_monitor_only_zones == 0) {
                snprintf(err_msg, err_cap,
                         "every zone in this profile is set to control mode OFF -- nothing would heat. "
                         "Pick bang-bang or PID in Settings > Zones.");
            } else if (n_monitor_only_zones == n_active_zones) {
                snprintf(err_msg, err_cap,
                         "every zone in this profile is monitor-only (no heater relay) -- nothing would heat. "
                         "Assign a relay in Settings > Zones.");
            } else {
                snprintf(err_msg, err_cap,
                         "no zone in this profile can heat: each is monitor-only (no heater relay) or "
                         "control mode OFF. Fix in Settings > Zones.");
            }
        }
        return false;
    }

    /* docs/ON_OFF_ZONE.md sec 6: "If the cap is reached by on/off zones
     * alone, that is a configuration error; refuse at run start rather than
     * discovering it mid-firing." An on/off zone counts toward
     * max_simultaneous_relays exactly like a heater (a contactor coil draws
     * the same current whatever it switches) but on/off zones are always the
     * LAST ones suppressed when the cap binds during a run (see the tick
     * loop's cap-adjustment for on/off zones) -- so a profile whose on/off
     * zone COUNT ALONE already meets or exceeds the cap would silently deny
     * every one of them, every tick, for the whole firing, with no heater
     * ever contending for those slots to make the denial look transient.
     * Caught here, once, instead of as a mystery "why won't my vent ever
     * turn on" during a live firing. cap == 0 is "unlimited", unchanged. */
    uint8_t cap_for_on_off_check = zones_config_get_max_simultaneous_relays();
    if (cap_for_on_off_check > 0) {
        uint8_t n_on_off_zones = 0;
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!(p.zone_mask & (1u << zi))) continue;
            if (zone_is_on_off(zi)) n_on_off_zones++;
        }
        /* Spare-relay WP-3 (plan sec 5): an aux output switched by this
         * profile's rules draws a load slot like an on/off zone. Count each
         * distinct enabled aux relay an enabled rule targets (the loop
         * above already refused any that are not usable). */
        uint8_t aux_seen_mask = 0;
        for (uint8_t ri = 0; ri < p.on_off_rule_count && ri < PROFILE_MAX_ON_OFF_RULES; ri++) {
            const profile_on_off_rule_t *pr = &p.on_off_rules[ri];
            if (pr->enable && profile_rule_target_is_aux(pr->zone_index) && pr->segment_index < p.segment_count) {
                aux_seen_mask |= (uint8_t)(1u << (profile_rule_target_aux_relay(pr->zone_index) - 1u));
            }
        }
        uint8_t n_aux = 0;
        for (uint8_t b = 0; b < AUX_OUTPUTS_COUNT; b++) {
            if (aux_seen_mask & (1u << b)) n_aux++;
        }
        if (n_aux > 0 && (uint16_t)n_on_off_zones + n_aux >= cap_for_on_off_check) {
            xSemaphoreGive(s_exec.lock);
            if (err_msg) {
                snprintf(err_msg, err_cap,
                         "this profile has %u on/off zone(s) and %u aux output(s) but "
                         "max_simultaneous_relays is %u -- on/off devices are always suppressed last when "
                         "the cap binds, so at least one would never be able to turn on for the whole "
                         "firing. Raise the cap in Settings, or reduce the on/off zones or aux rules in "
                         "this profile.",
                         (unsigned)n_on_off_zones, (unsigned)n_aux, (unsigned)cap_for_on_off_check);
            }
            ESP_LOGW(PE_TAG, "refusing run: %u on/off zone(s) + %u aux >= max_simultaneous_relays %u",
                     (unsigned)n_on_off_zones, (unsigned)n_aux, (unsigned)cap_for_on_off_check);
            return false;
        }
        if (n_on_off_zones >= cap_for_on_off_check) {
            xSemaphoreGive(s_exec.lock);
            if (err_msg) {
                snprintf(err_msg, err_cap,
                         "this profile has %u on/off zone(s) but max_simultaneous_relays is %u -- "
                         "on/off zones are always suppressed last when the cap binds, so at least one "
                         "on/off device would never be able to turn on for the whole firing. Raise the "
                         "cap in Settings, or reduce the on/off zones in this profile.",
                         (unsigned)n_on_off_zones, (unsigned)cap_for_on_off_check);
            }
            return false;
        }
    }

    /* HP-02 bench bug (2026-09-25..27, ESP 0fb8ad98): an on/off-typed zone
     * (ZONE_TYPE_ON_OFF, left behind by HP-03/HP-07's zone_type override)
     * in a profile carrying no on/off rule for it. docs/ON_OFF_ZONE.md
     * sec 3 rule 6 holds such a zone's relay OFF for the whole firing, and
     * the tick loop never applies its PID/bang-bang decision (the on/off
     * path owns its apply_relay() call) -- so the bench saw duty 1.00,
     * relay off, faulted false, heat_blocked false for 76 s while zone 2
     * rose 0.9 C against 11 C and 9 C for its neighbours, with nothing
     * naming why. A zone that no segment of this profile can ever turn on
     * is a configuration mismatch between the profile and the zone type;
     * refuse it here, once, naming the fix, rather than run a firing whose
     * third zone can never heat. A rule in ANY segment is enough to pass:
     * per-segment "no rule" is legitimate (a vent that only opens during
     * the cooling segment) and is reported live instead, through
     * relay_denied_reason == PROFILE_EXEC_RELAY_DENIED_ON_OFF_NO_RULE.
     *
     * Builtin schedules are exempt (review fix): profiles_http_get() gives a
     * builtin the mask of EVERY configured zone and a builtin can carry no
     * on/off rules, so refusing here would make every builtin unrunnable on
     * any board with a vent typed on/off -- none of the three remedies is
     * available for a builtin short of retyping the vent. Plan sec 3 rule 6
     * (device OFF) is the intended behaviour there, and relay_denied_reason
     * still names it live. The message fits the HTTP start handler's 128 B
     * err_msg (dashboard_exec_http.c) so the remedy is not truncated away. */
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (profile_id >= PROFILE_BUILTIN_ID_BASE) break;
        if (!(p.zone_mask & (1u << zi)) || !zone_is_on_off(zi)) continue;
        bool any_rule = false;
        for (uint8_t ri = 0; ri < p.on_off_rule_count && ri < PROFILE_MAX_ON_OFF_RULES; ri++) {
            const profile_on_off_rule_t *pr = &p.on_off_rules[ri];
            if (pr->enable && pr->zone_index == zi && pr->segment_index < p.segment_count) {
                any_rule = true;
                break;
            }
        }
        if (any_rule) continue;
        xSemaphoreGive(s_exec.lock);
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "zone %u is typed on/off but no on/off rule in this profile targets it: "
                     "add one, drop it from the mask, or make it a heater",
                     (unsigned)zi);
        }
        ESP_LOGW(PE_TAG, "refusing run: on/off zone %u has no on/off rule in any of %u segment(s)",
                 zi, (unsigned)p.segment_count);
        return false;
    }

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
        /* ADAPTIVE_FUZZY_EVALUATION.md sec 3, N3: a limit cycle from a
         * PREVIOUS firing must never carry a stale trip into this one --
         * the "reset one side of a pair" class this project has hit before
         * (see pid_fuzzy_oscillation_state_t's own header comment). */
        pid_fuzzy_oscillation_reset(&z->fuzzy_osc);

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
        /* Guard 1's arrival band (ZONES_CFG_VERSION 21->22, docs/audits/
         * consumer_without_producer_2026-09-06.md finding 1): unlike the
         * five v8 overrides above, this one's own getter already does the
         * 0->default substitution (same shape as error_band_c/rate_band_c_
         * per_s, since there is no "no band" answer guard 1's arrival test
         * can accept) -- so the value handed to thermal_guard_cfg_t here is
         * already final, and thermal_guard.c's own effective_f(cfg->
         * progress_band_c, PROGRESS_BAND_C) substitution is now a no-op in
         * practice for this field (kept anyway, defence in depth, same as
         * every other guard threshold). */
        float progress_band_c = 0.0f;
        zones_config_get_progress_band_c(zi, &progress_band_c);
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
            .progress_band_c = progress_band_c,
        };
        thermal_guard_reset(&z->guard_state);
        /* docs/ON_OFF_ZONE.md sec 5: "Resume starts every on/off device
         * in its fail-safe state and quasi_dwell = false" -- profile_
         * executor_run() is the single entry point for both a fresh run and
         * the warm-start/resume path, so resetting here covers both without
         * a second call site to keep in sync. */
        on_off_trigger_state_reset(&z->on_off_trigger_state);
        /* Actuation-layer hold state (plan step 8) mirrors the same
         * fail-safe-shaped reset: never actuated. Both holds are seeded from
         * the zone relay's last ON-to-OFF time (relay_off_tracker): settled
         * if it has not been ON since boot, so min_off_s does not delay the
         * first ON of the first run, but still counting down after a stop
         * followed by a quick restart. */
        z->on_off_actuated_on = false;
        profile_executor_on_off_seed_hold(&z->on_off_trigger_state, &z->on_off_actuated_held_s, relay_mask);
        /* HP-02 starvation reporting starts from zero every run/resume. */
        z->relay_starved_s = 0.0f;
        z->relay_denied_reason = PROFILE_EXEC_RELAY_DENIED_NONE;
    }

    /* Ramp baseline: the baseline zone's actual (calibrated) reading if we
     * have one, else the segment's own target (makes ramp math a no-op
     * rather than ramping from a fabricated zero). TODO.md 10.8: this must
     * be the same COMBINED reading the very first control tick will compute
     * for that zone, not just its legacy same-index channel -- otherwise a
     * multi-thermocouple zone would start its ramp math from a different
     * number than the tick right after it settles on.
     *
     * The MAX31856 SPI read itself already happened in
     * profile_executor_capture_baseline(), BEFORE s_exec.lock was taken
     * (see that function's doc comment), and that function alone picks the
     * baseline zone (profile_executor_baseline_zone(): lowest zone in the
     * mask that is neither on/off nor monitor-only). The snapshot is applied here, once,
     * after the zone loop, rather than at a second "zi == first_active"
     * gate inside it: a second pick evaluated later, under the lock, could
     * disagree with the capture's if a zone's type or relay mask changed in
     * between, and every captured value is already invalid/NAN when the
     * capture found no baseline zone. */
    if (baseline_valid) {
        baseline_target_c = baseline_c;
    }
    warm_start_coolest_c = warm_start_coolest_captured;
    if (ambient_valid) {
        s_exec.ambient_c = ambient_c_captured;
        s_exec.ambient_from_cj = true;
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
            s_exec.segment_elapsed_rem_ms = 0;
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
    s_exec.total_elapsed_rem_ms = 0;

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

    /* Cross-module zone-ownership race close (review of 933a7eec,
     * docs/audits/profile_executor_panic_2026-09-24.md follow-up): the
     * per-zone autotune_engine_is_active_on_zone() loop far above is a
     * plain, non-atomic peek made BEFORE s_exec.lock was even taken --
     * autotune_engine_run()/autotune_engine_run_relay() can commit on one of
     * this profile's zones in the window between that peek and this
     * function's own commit, and those functions have the exact same problem
     * in the other direction (their own profile_executor_zone_is_active()
     * peek, made before s_at.lock). relay_authority_zone_claim_begin() is the
     * atomic close: a single spinlock-protected test-and-set against
     * autotune_begin_run_locked()'s matching call, keyed by this profile's
     * zone_mask. Deliberately NOT the RELAY_HEAT_ZONE_CLAIM_* pair used a few
     * lines below -- see relay_authority_zone_claim_begin()'s own doc comment
     * (relay_authority.h) for why that pair is the wrong arbiter for this
     * question. s_exec.lock has been held continuously since the "already
     * running"/"faulted" checks confirmed this is a genuine start, so
     * release_profile_relay_claim()'s unconditional
     * relay_authority_zone_claim_end() call is safe, same reasoning as the
     * heat claim below. On conflict, name the lowest-numbered contended zone
     * -- the same message shape the early, non-atomic loop above already
     * uses for the common (non-race) case. */
    {
        uint8_t conflict_mask = 0;
        if (!relay_authority_zone_claim_begin(RELAY_HEAT_ZONE_CLAIM_PROFILE, p.zone_mask, &conflict_mask)) {
            xSemaphoreGive(s_exec.lock);
            if (err_msg) {
                uint8_t conflict_zone = 0;
                for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
                    if (conflict_mask & (1u << zi)) {
                        conflict_zone = zi;
                        break;
                    }
                }
                snprintf(err_msg, err_cap, "zone %u has an autotune run active -- it cannot run at the "
                                           "same time as a profile (TODO.md 6A.5)",
                         conflict_zone);
            }
            return false;
        }
    }

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
        relay_authority_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE, p.zone_mask);
        xSemaphoreGive(s_exec.lock);
        if (err_msg) {
            snprintf(err_msg, err_cap,
                     "a zone current sweep is running -- it cannot run at the same time as a firing");
        }
        return false;
    }

    /* Update-in-flight, second look (review 3 MED-1): the early check at the
     * top of this function precedes slow baseline reads, so an update claim
     * could be taken after it. Re-test now that the heat claim is published.
     * Pairs with update_http.c claim_refuses(): that side takes the update
     * claim and THEN re-reads the heat run state; this side publishes the
     * heat claim and THEN re-reads the update claim -- at least one refuses. */
    if (ota_http_heat_blocked_by_update(err_msg, err_cap)) {
        relay_authority_heat_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE);
        relay_authority_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE, p.zone_mask);
        xSemaphoreGive(s_exec.lock);
        return false;
    }

    /* Restore-in-flight, second look (A4 review follow-up A, reviewer fix):
     * the gate check at the top of this function reads the flag long before
     * this commit (baseline SPI reads, config reads in between), so a restore
     * could set it after that read and still see no heat claim at its own
     * job-side re-check. Re-reading it HERE, after the heat claim above is
     * published, pairs with backup_import_job(): that side stores the flag
     * and then reads the claim (relay_authority_heat_run_active()), this
     * side publishes the claim and then reads the flag -- both seq_cst /
     * critical-section ordered, so at least one of the two refuses. */
    if (backup_import_restore_in_flight()) {
        relay_authority_heat_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE);
        relay_authority_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE, p.zone_mask);
        xSemaphoreGive(s_exec.lock);
        sys_mode_snapshot_t late_snap;
        memset(&late_snap, 0, sizeof(late_snap));
        late_snap.restore_in_flight = true;
        (void)system_mode_gate_check(SYS_ACTION_START_PROFILE, &late_snap, err_msg, err_cap);
        ESP_LOGW(PE_TAG, "profile_executor_run(%u) refused at commit: a backup restore started meanwhile",
                 (unsigned)profile_id);
        return false;
    }

    /* Danger mode, second look (LCD review R2): the early gate read precedes the baseline reads.
     * danger_mode_request_start() opens its window and THEN reads the heat claim published above;
     * this side publishes the claim and THEN reads the window (fail closed) -- at least one refuses,
     * so a firing never commits inside a danger window. */
    if (danger_mode_blocks_start()) {
        relay_authority_heat_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE);
        relay_authority_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE, p.zone_mask);
        xSemaphoreGive(s_exec.lock);
        sys_mode_snapshot_t late_snap;
        memset(&late_snap, 0, sizeof(late_snap));
        late_snap.danger_mode_active = true;
        (void)system_mode_gate_check(SYS_ACTION_START_PROFILE, &late_snap, err_msg, err_cap);
        ESP_LOGW(PE_TAG, "profile_executor_run(%u) refused at commit: danger mode opened meanwhile",
                 (unsigned)profile_id);
        return false;
    }

    /* Factory reset in flight, second look: factory_reset.c's execute_scope() sets the reset mark and
     * then re-reads the heat claim before it dispatches the erase; this side has published the heat
     * claim (above) and now reads the mark, both under relay_authority's leaf spinlock -- so either the
     * reset refuses 409 or this refuses, never an erase under a live run (relay_authority.h). */
    if (relay_authority_reset_in_flight()) {
        relay_authority_heat_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE);
        relay_authority_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE, p.zone_mask);
        xSemaphoreGive(s_exec.lock);
        if (err_msg) {
            snprintf(err_msg, err_cap, "factory reset in progress -- the controller reboots when it finishes");
        }
        ESP_LOGW(PE_TAG, "profile_executor_run(%u) refused at commit: factory reset in progress",
                 (unsigned)profile_id);
        return false;
    }

    /* Zones config changed, second look (HTTP audit E1 finding 1): POST /api/zones gates on the
     * heat claim at entry, then does a slow Pico ceiling raise before committing, so it could commit
     * a new config between this function's config reads above and the RUNNING commit below -- and
     * the executor would then re-read that config mid-run. The POST re-reads the heat claim inside
     * its commit critical section; this side publishes the heat claim (above) and then reads the
     * generation under that same lock. Either the POST sees the claim and refuses 409, or this sees
     * its bump and refuses here; the operator retries against the new config. */
    if (zones_config_changed_since(gen_at_entry)) {
        relay_authority_heat_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE);
        relay_authority_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE, p.zone_mask);
        xSemaphoreGive(s_exec.lock);
        if (err_msg) {
            snprintf(err_msg, err_cap, "the zones configuration changed while the firing was starting -- start it again");
        }
        ESP_LOGW(PE_TAG, "profile_executor_run(%u) refused at commit: zones config changed meanwhile",
                 (unsigned)profile_id);
        return false;
    }

    /* relay_authority's own per-zone latch is a SEPARATE module, not touched
     * by the s_exec.zones memset above -- release it for every zone this run
     * is about to activate. Without this, a zone whose per-zone guard
     * tripped in a PAST run that reached DONE/FAULTED and was never
     * explicitly halted/dismissed stays latched blocked forever: profile_
     * executor_halt() is the only caller of clear_this_runs_faults(), and
     * that function only clears zones marked active in the run BEING halted
     * -- it does nothing for a run that simply finished on its own (the
     * natural-DONE path in profile_executor.c) or for a later run that never
     * reactivated the stuck zone. The state guard above only refuses
     * starting over RUNNING/PAUSED/FAULTED, not DONE, so a fresh run can
     * start directly over an undismissed DONE run and inherit its stale
     * latch. HP-02 (2026-09-25): a 3-zone firing where zone 2 alone read
     * ~0C of rise while zones 0/1 rose normally -- duty computed normally,
     * every relay command silently refused at this chokepoint. See
     * relay_authority.h's corrected comment on
     * relay_authority_zone_latched_blocked().
     *
     * Placed HERE, after relay_authority_heat_zone_claim_begin() has
     * actually succeeded, rather than up at the top of this function
     * (review of 1f2e9b28, MEDIUM): every refusal path between the top of
     * this function and this point returns false without starting a run --
     * a refused start must never change relay_authority's safety-latch
     * state. Zone-claim conflicts (relay_authority_zone_claim_begin(),
     * above), the heat-zone claim conflict just above, the all-OFF and
     * relay-cap refusals earlier, and any future refusal added between here
     * and the top all leave a stale latch exactly as tripped, matching every
     * other safety-relevant commit in this function (s_exec.state itself
     * isn't set to RUNNING until below this point either). */
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if ((p.zone_mask & (1u << zi)) && relay_authority_zone_latched_blocked(zi)) {
            ESP_LOGW(PE_TAG, "zone %u was still blocked by an earlier guard trip -- clearing it because "
                              "this run activates it",
                     zi);
        }
    }
    clear_stale_zone_latches_for_new_run(p.zone_mask);

    /* TODO.md section 0's ownership decision, closing the "manual relay
     * control is not blocked during a firing" gap: claim every relay this
     * run touches so /api/relay and the UART SET_RELAY* commands refuse a
     * manual command against it until pause/halt hands it back. */
    /* Spare-relay WP-3 manual-toggle handoff (owner decision: firing start
     * hands aux control from the manual toggle to the profile rule). Every
     * aux enabled right now is taken over for this run: added to the claim
     * (so /api/relay refuses it until the run ends) and written OFF, so a
     * relay the operator left ON by hand starts the firing in the same state
     * as the executor's own per-aux state (OFF, never actuated) -- the two
     * halves of that pair must agree before the first decision. */
    {
        uint8_t aux_take = (uint8_t)(aux_outputs_cfg_enabled_mask() | s_exec.aux_claim_mask);
        s_exec.aux_claim_mask = aux_take;
        s_exec.claimed_relay_mask |= aux_take;
        s_exec.aux_off_pending = false;
        if (aux_take != 0 && s_exec.io) {
            esp_err_t aux_err = kiln_io_owner_command_set_relay_mask_authorized(aux_take, 0);
            if (aux_err != ESP_OK) {
                ESP_LOGW(PE_TAG, "aux start handoff OFF write failed (mask 0x%02X): %s -- retried by aux_off_pending / the first aux tick",
                         (unsigned)aux_take, esp_err_to_name(aux_err));
                /* aux_off_pending's retry branch only runs when not RUNNING; in
                 * practice profile_executor_aux_tick() rewrites every claimed
                 * aux on the next tick, so the aux does not stay as the
                 * operator left it until the first rule decision. */
                s_exec.aux_off_pending = true;
            } else {
                relay_off_tracker_note_write(aux_take, 0);
            }
        }
    }
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
     * the watchdog task) retries. heat_enable.c logs the failure loudly.
     *
     * 2026-09-15 (review of 1c8d7f6e, finding MEDIUM-3): the actual call is
     * made AFTER s_exec.lock is released below, not here -- heat_enable_
     * acquire() can now block on up to two sequential link exchanges (a
     * flush of any still-pending release, then this enable=true), and
     * holding s_exec.lock across that stalls every other task that blocks
     * on it (safety_poll_task via profile_executor_get_status(), the HTTP
     * status handlers). The commit-point ordering guarantee this comment
     * describes is unaffected: nothing below this point can un-commit the
     * run, so asking for heat a few instructions later, lock-free, is the
     * same "definitely RUNNING, ask for K4" sequence from every caller's
     * point of view. */
    s_exec.state = PROFILE_EXEC_RUNNING;
    run_snapshot_buf_t start_snap;
    capture_run_snapshot(&start_snap);
    /* Sampled under s_exec.lock, spent after it is dropped: if an operator
     * halt runs the whole stop path in the gap below, the acquire refuses
     * rather than requesting K4 for a firing that no longer exists. See
     * heat_enable.h's release-epoch section. */
    uint32_t he_epoch = heat_enable_claim_epoch(HEAT_ENABLE_CLAIMANT_PROFILE);
    xSemaphoreGive(s_exec.lock);
    if (!heat_enable_acquire_since(HEAT_ENABLE_CLAIMANT_PROFILE, he_epoch)) {
        ESP_LOGW(PE_TAG, "start: heat_enable_acquire_since() failed -- run starts with heat blocked until "
                         "heat_enable_reconcile() succeeds");
    }

    /* First write of this run's breadcrumb, and the one that overwrites any
     * previous run's record in flash. From here on the stored record says a
     * firing is in progress until something records an ending. */
    run_state_note(RUN_STATE_PHASE_RUNNING, &start_snap.snap);

    ESP_LOGI(PE_TAG, "profile '%s' (id %u, zone_mask 0x%02X) running", p.name, profile_id, p.zone_mask);
    return true;
}

