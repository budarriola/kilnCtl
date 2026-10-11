#include "profile_executor.h"
#include "profile_executor_internal.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h> /* free() -- reload_live_profile_if_changed()'s candidate blob is heap-allocated, see profile_executor_firing_stats.c's established pattern */
#include <string.h>

#include <time.h>

#include "esp_heap_caps.h" /* heap_caps_malloc() */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "adaptive_tune.h" /* PID_EXPANSION_PLAN.md Phase 7d -- see that file's own header comment */
#include "autotune_engine.h"
#include "heat_enable.h"
#include "heater_output.h"
#include "kiln_io_owner.h"
#include "live_profile.h" /* live_profile_generation()/live_profile_load_working() -- pass 1 pickup poll */
#include "ota_state.h" /* ota_http_heat_blocked_by_update() -- heat_interlock.h's own doc comment */
#include "profile_executor_live_pickup.h"
/* profiles_validate_candidate() lives in profiles_http_internal.h, which pulls
 * in esp_http_server.h -- NOT host-safe, and test_profile_executor_prestart.c
 * #includes this whole file directly (host-only translation unit). Declared
 * locally instead, matching that header's real signature exactly (types come
 * from profiles_types.h, already visible), so this stays a thin extern
 * reference rather than dragging the httpd tier into a control-tier file.
 * LOW (review, 2026-09-19): the two enums (this one and the real
 * profile_validate_mode_t) cannot be the same C type across this TU
 * boundary, so their VALUES are pinned instead by a _Static_assert in
 * profiles_http_internal.h, the TU that owns the real enum -- if that
 * assert ever fires, PROFILE_VALIDATE_HARD_LOCAL below must be updated to
 * match. */
typedef enum { PROFILE_VALIDATE_ADVISORY_LOCAL = 0, PROFILE_VALIDATE_HARD_LOCAL = 1 } profile_validate_mode_local_t;
extern bool profiles_validate_candidate(const profile_t *candidate, int mode, char *warnings_json,
                                         size_t warnings_json_cap, char *err_msg, size_t err_cap);
#include "pid.h"
#include "pid_fuzzy.h"
#include "ramp_assist_cfg.h"
#include "relay_authority.h"
#include "relay_cycles.h"
#include "run_state.h"
#include "dualwrite_window.h" /* FILESYSTEM.md dual-write window: note a completed firing
                                * that ran with cfg_fs live, toward the "one complete firing run
                                * file-backed" exit criterion -- see the two call sites below */
#include "cfg_fs.h" /* cfg_fs_is_available() -- gates the note above on the filesystem
                      * actually having been the live path for this run */
#include "safety_trip_words.h" /* safety_fault_source_words() -- ROADMAP.md M13, decode the
                                 * fault-source mask for the operator instead of a bare hex value */
#include "sim_backend.h"
#include "stack_margin.h" /* stack_margin_register() -- Opus review round 3, item 5: a real
                           * uxTaskGetStackHighWaterMark() reading for this task and its watchdog,
                           * readable over the link's GET_STACK_MARGIN (uart_bridge.c) without a
                           * debugger, before this fix's ~250-300B estimated peak stack addition
                           * (solve_hold_for_zone()/gauss_solve_partial_pivot()) is trusted on
                           * hardware. */
#include "thermo_channel_read.h"
#include "thermo_combine.h"
#include "zone_coupling_solve.h"
#include "zones_config_accessors.h"

/* PID_EXPANSION_PLAN.md sec 3.6d / PER_ZONE_TARGET_DESIGN_STUDY.md option
 * (b): declared here rather than pulled in via zones_config_json.h (this
 * pass's touched-files list does not include this file's own header chain
 * gaining a new #include), same local-forward-declaration convention
 * profile_executor_feedforward.c already uses for zones_config_get_ease_
 * off_window_mult() -- see that file's own comment. Keep this in sync with
 * zones_config_json.h's own declaration. */
bool zones_config_get_approach_rate_cap_c_per_hr(uint8_t zone_index, float *out_cap_c_per_hr);

const char *PE_TAG = "profile_executor";

/* Guard 9: control-tick liveness (TODO.md 6A.3/6A.7). A second, independent
 * task -- "a control loop cannot be its own watchdog" -- checks that the
 * control task's last-tick timestamp keeps moving and force-drops relays if
 * it doesn't. */
#define WATCHDOG_CHECK_PERIOD_MS 2000u
#define WATCHDOG_TICK_DEAD_MS 10000u

/* Packed history storage (see profile_executor.h's note on
 * profile_history_entry_t and history_slot_t's own doc comment in profile_
 * executor_internal.h for the current per-slot byte count and the 2026-09-01
 * move to PSRAM).
 *
 * Resolution, and why each is enough for a *trend line over a firing*:
 *   elapsed  units of HISTORY_SAMPLE_PERIOD_S (30 s), u16 -> 22 days
 *   temps    0.1 degC, i16 -> +/-3276.7 degC, well past any thermocouple's
 *            range; HISTORY_TEMP_INVALID encodes NaN
 *   duty     whole percent, u8; 255 encodes "no value"
 * The graph this feeds is a multi-hour trend, not a scope trace. */
s_exec_state_t s_exec;

/* ROADMAP.md M15 "Mode-state sprawl" -- see profile_executor_internal.h's
 * big table comment for the full rule list and rationale; this is just the
 * mechanical check against it. Called once per control tick (below, near
 * the end of the locked section) and by host tests directly. Pure: reads
 * s_exec and autotune_engine_get_status(), writes nothing, does no I/O of
 * its own beyond the ESP_LOGE/assert() on a violation. */
uint32_t exec_mode_state_check(char *out_first_violation, size_t out_cap)
{
    uint32_t violations = 0;
    char first[160] = {0};

#define EXEC_MODE_VIOLATION(fmt, ...)                                                            \
    do {                                                                                          \
        violations++;                                                                             \
        char _msg[160];                                                                           \
        snprintf(_msg, sizeof(_msg), fmt, ##__VA_ARGS__);                                          \
        ESP_LOGE(PE_TAG, "exec_mode_state_check: %s", _msg);                                       \
        if (first[0] == '\0') {                                                                   \
            memcpy(first, _msg, sizeof(first));                                                    \
        }                                                                                          \
    } while (0)

    /* Rule 4: no active zone while IDLE. */
    if (s_exec.state == PROFILE_EXEC_IDLE) {
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (s_exec.zones[zi].active) {
                EXEC_MODE_VIOLATION("rule 4: zone %u active while state==IDLE", (unsigned)zi);
            }
        }
    }

    /* Rule 5: dwelling only while RUNNING/PAUSED. */
    if (s_exec.dwelling && s_exec.state != PROFILE_EXEC_RUNNING && s_exec.state != PROFILE_EXEC_PAUSED) {
        EXEC_MODE_VIOLATION("rule 5: dwelling==true while state=%d (not RUNNING/PAUSED)", (int)s_exec.state);
    }

    /* Rule 1: a zone this run has active cannot also be the zone an
     * in-progress (heat-driving) autotune run owns. */
    autotune_engine_status_t at;
    autotune_engine_get_status(&at);
    bool autotune_driving = (at.state == AUTOTUNE_ENGINE_SETTLING || at.state == AUTOTUNE_ENGINE_STEPPING ||
                             at.state == AUTOTUNE_ENGINE_RELAY_APPROACH ||
                             at.state == AUTOTUNE_ENGINE_RELAY_CYCLING);
    if (autotune_driving && (s_exec.state == PROFILE_EXEC_RUNNING || s_exec.state == PROFILE_EXEC_PAUSED) &&
        at.zone_index < MAX31856_CHANNEL_COUNT && s_exec.zones[at.zone_index].active) {
        EXEC_MODE_VIOLATION("rule 1: zone %u active in a %s profile run AND autotune state=%d driving it",
                            (unsigned)at.zone_index,
                            s_exec.state == PROFILE_EXEC_RUNNING ? "RUNNING" : "PAUSED", (int)at.state);
    }

    /* Rule 6: no_setpoint must agree with method -- checked structurally
     * here since s_exec has no window into autotune's own s_at.method, but
     * no_setpoint is documented as `method != AUTOTUNE_METHOD_RELAY`
     * (autotune_engine.c): a RELAY run always has a genuine relay_setpoint_c
     * to check drift against, so relay_setpoint_c == 0.0f while method
     * implies RELAY (relay_cycles_target/relay_amplitude_duty configured)
     * would itself be the tell -- kept as a light structural check rather
     * than reaching into autotune's private state. */
    if (at.method == AUTOTUNE_METHOD_RELAY && !(at.relay_setpoint_c > 0.0f) &&
        (at.state == AUTOTUNE_ENGINE_RELAY_APPROACH || at.state == AUTOTUNE_ENGINE_RELAY_CYCLING)) {
        EXEC_MODE_VIOLATION("rule 6: method=RELAY driving (state=%d) but relay_setpoint_c<=0 (no_setpoint "
                            "should be false)", (int)at.state);
    }

    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        zone_runtime_t *z = &s_exec.zones[zi];

        /* Rule 2: cooling_limited is a PID/PID_FUZZY-only diagnostic. */
        if (z->cooling_limited && z->control_mode != ZONE_CONTROL_MODE_PID &&
            z->control_mode != ZONE_CONTROL_MODE_PID_FUZZY) {
            EXEC_MODE_VIOLATION("rule 2: zone %u cooling_limited==true but control_mode=%d (not PID/PID_FUZZY)",
                                (unsigned)zi, (int)z->control_mode);
        }

        /* Rule 3: a faulted zone's relay must never read commanded on. */
        if (z->faulted && z->relay_commanded_on) {
            EXEC_MODE_VIOLATION("rule 3: zone %u faulted==true but relay_commanded_on==true", (unsigned)zi);
        }
    }

#undef EXEC_MODE_VIOLATION

    if (out_first_violation != NULL && out_cap > 0) {
        strncpy(out_first_violation, first, out_cap - 1);
        out_first_violation[out_cap - 1] = '\0';
    }
    return violations;
}

/* docs/audits/profile_executor_panic_2026-09-24.md, owner decision 2026-09-24:
 * see exec_mode_state_check()'s doc comment above for the full "why" --
 * the assert() at this module's real control-loop call site used to reboot
 * the board in production the one time it fired live (a global thermal
 * guard tripping mid-dwell). On ESP_PLATFORM that call site calls this
 * instead. Latched via s_exec.mode_state_fault_latched (cleared at the next
 * profile_executor_run()) so a violation that persists tick to tick forces
 * FAULTED/heaters-off/log exactly once per run, not every tick -- a control
 * loop that force-drops relays and ESP_LOGE's every 100ms forever is its own
 * kind of harm. mode_state_violation_count is a lifetime-of-this-boot
 * diagnostics counter (never reset), reported over GET /api/profile_exec,
 * so this class of violation is visible without grepping the device log
 * ring (which drops lines silently under volume). Returns true only on the
 * call that newly forced FAULTED, so the caller can record the run-state
 * breadcrumb exactly as it does for a guard trip (run_faulted_this_tick).
 * Must be called with s_exec.lock held. */
bool exec_handle_mode_state_violation(uint32_t mode_violations, const char *first_violation)
{
    if (mode_violations == 0) {
        return false;
    }
    s_exec.mode_state_violation_count++;
    if (s_exec.mode_state_fault_latched) {
        /* Already forced FAULTED for this run -- don't re-log or re-force
         * relays off every tick. */
        return false;
    }
    s_exec.mode_state_fault_latched = true;
    ESP_LOGE(PE_TAG,
             "exec_mode_state_check found %lu violation(s) -- forcing FAULTED, heaters off, run continues "
             "(no reboot): %s",
             (unsigned long)mode_violations, first_violation != NULL ? first_violation : "?");
    /* An already-FAULTED run (a guard trip earlier this tick) keeps its own
     * fault_reason/fault_guard -- that is the cause the operator needs; the
     * violation itself is still logged above and counted. */
    bool was_faulted = (s_exec.state == PROFILE_EXEC_FAULTED);
    /* Sampled BEFORE the FAULTED transition below, for the rule-1 autotune
     * re-derivation at the end of this function -- rule 1 only fires for a
     * RUNNING/PAUSED run, and exec_enter_terminal_state() is about to
     * overwrite that. */
    bool was_running_or_paused = (s_exec.state == PROFILE_EXEC_RUNNING || s_exec.state == PROFILE_EXEC_PAUSED);
    exec_enter_terminal_state(PROFILE_EXEC_FAULTED);
    if (!was_faulted) {
        strncpy(s_exec.fault_reason, first_violation != NULL ? first_violation : "exec_mode_state_check violation",
                sizeof(s_exec.fault_reason) - 1);
        s_exec.fault_reason[sizeof(s_exec.fault_reason) - 1] = '\0';
        s_exec.fault_guard = THERMAL_GUARD_TRIP_NONE; /* not a thermal_guard_trip_t fault -- see fault_guard's own doc */
    }
    /* Abnormal stop -- same relay teardown escalate_guard_trip()'s GLOBAL
     * branch and the watchdog's forced FAULTED transition use: force every
     * active zone's relay off, force every relay/IO segment off without
     * honoring leave_on_at_end (io_segs_tick() only runs while RUNNING, so
     * nothing else would ever finish a segment once this run is FAULTED),
     * then release this run's relay claim, heat claim and K4 request so a
     * relay this run touched is not left refused to /api/relay, the LCD, or
     * the UART bridge as "owned by a running profile" -- see
     * release_profile_relay_claim()'s own doc comment. */
    force_all_relays_off();
    io_segs_force_all_off(false);
    release_profile_relay_claim();

    /* docs/audits/profile_executor_panic_2026-09-24.md's advisory: rule 1
     * (this run and an in-progress, heat-driving autotune session both
     * claiming the same zone) forces THIS run FAULTED above, but autotune
     * holds its own, entirely separate claims on that zone's relay
     * (RELAY_OWNER_AUTOTUNE via relay_authority_claim_mask()) and heat
     * (HEAT_ENABLE_CLAIMANT_AUTOTUNE, autotune_engine.c) -- release_profile_
     * relay_claim() above only releases THIS run's own claim, so a violated
     * autotune session would otherwise keep driving that zone's heater right
     * through this run's fault. Stop it too.
     *
     * The condition is rule 1's own (exec_mode_state_check() above),
     * including its RUNNING/PAUSED clause evaluated on the PRE-transition
     * state: `active` survives the FAULTED transition (exec_enter_terminal_
     * state() clears it only on IDLE), so without that clause any other
     * violation on an already-FAULTED run would also abort an autotune an
     * operator legitimately started on a zone that stale run still lists as
     * active (autotune's own start check, profile_executor_zone_is_active(),
     * only refuses RUNNING/PAUSED).
     *
     * Lock order: autotune_engine_abort() takes s_at.lock, then (via
     * abort_locked() -> force_relays_off()) only leaf locks -- the
     * kiln_io_owner slot lock/queue wait (bounded, KILN_IO_OWNER_WAIT_MS),
     * relay_authority's portMUX, s_he.lock, and the sim lock on a sim build
     * -- none of which ever takes s_exec.lock or calls back into this
     * module, so calling it here, under s_exec.lock, keeps the required
     * s_exec.lock -> s_at.lock order. */
    autotune_engine_status_t at;
    autotune_engine_get_status(&at);
    bool autotune_driving = (at.state == AUTOTUNE_ENGINE_SETTLING || at.state == AUTOTUNE_ENGINE_STEPPING ||
                             at.state == AUTOTUNE_ENGINE_RELAY_APPROACH ||
                             at.state == AUTOTUNE_ENGINE_RELAY_CYCLING);
    if (autotune_driving && was_running_or_paused && at.zone_index < MAX31856_CHANNEL_COUNT &&
        s_exec.zones[at.zone_index].active) {
        autotune_engine_abort("profile_executor: rule-1 mode-state violation -- zone double-owned by a "
                              "profile run and an active autotune session");
    }
    return true;
}

static int16_t history_pack_temp(float c)
{
    if (isnan(c)) {
        return HISTORY_TEMP_INVALID;
    }
    float dc = c * 10.0f;
    if (dc > 32767.0f) dc = 32767.0f;
    if (dc < -32767.0f) dc = -32767.0f; /* -32768 is the NaN sentinel, don't collide with it */
    return (int16_t)(dc >= 0.0f ? dc + 0.5f : dc - 0.5f);
}

static float history_unpack_temp(int16_t dc)
{
    return (dc == HISTORY_TEMP_INVALID) ? NAN : (float)dc / 10.0f;
}

static uint8_t history_pack_duty(float duty)
{
    if (isnan(duty)) {
        return HISTORY_DUTY_INVALID;
    }
    float pct = duty * 100.0f;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    return (uint8_t)(pct + 0.5f);
}

/* elapsed/desired are shared across every zone in the run (one ramp,
 * TODO.md 6A.5); actual/duty/guard are per zone -- inactive_mask marks which
 * indices of the fixed-size per-zone arrays below have nothing this run
 * (bit set = zone not in play), so they pack as HISTORY_TEMP_INVALID/
 * HISTORY_DUTY_INVALID/0 rather than stale data from a previous run's
 * zone_mask. */
static void history_pack(history_slot_t *slot, uint32_t elapsed_s, float desired_c,
                         const float *actual_c, const float *duty, const uint8_t *guard,
                         uint8_t inactive_mask)
{
    uint32_t periods = elapsed_s / HISTORY_SAMPLE_PERIOD_S;
    slot->elapsed_periods = (periods > UINT16_MAX) ? UINT16_MAX : (uint16_t)periods;
    slot->desired_dc = history_pack_temp(desired_c);
    /* This sample's own zone_mask, not the executor's current-run one read
     * back later -- see history_slot_t's doc comment (profile_executor_
     * internal.h) for why. inactive_mask is already exactly "not in this
     * run's zone_mask" (see this function's caller), so the active mask is
     * just its complement over HISTORY_ZONE_COUNT bits. */
    slot->zone_mask = (uint8_t)(~inactive_mask & ((1u << HISTORY_ZONE_COUNT) - 1u));
    for (uint8_t zi = 0; zi < HISTORY_ZONE_COUNT; zi++) {
        if (inactive_mask & (1u << zi)) {
            slot->actual_dc[zi] = HISTORY_TEMP_INVALID;
            slot->duty_pct[zi] = HISTORY_DUTY_INVALID;
            slot->guard[zi] = 0;
        } else {
            slot->actual_dc[zi] = history_pack_temp(actual_c[zi]);
            slot->duty_pct[zi] = history_pack_duty(duty[zi]);
            slot->guard[zi] = guard[zi];
        }
    }
}

void history_unpack(const history_slot_t *slot, profile_history_entry_t *out)
{
    out->elapsed_s = (uint32_t)slot->elapsed_periods * HISTORY_SAMPLE_PERIOD_S;
    out->desired_c = history_unpack_temp(slot->desired_dc);
    for (uint8_t zi = 0; zi < HISTORY_ZONE_COUNT; zi++) {
        out->actual_c[zi] = history_unpack_temp(slot->actual_dc[zi]);
        out->duty[zi] = (slot->duty_pct[zi] == HISTORY_DUTY_INVALID) ? NAN : (float)slot->duty_pct[zi] / 100.0f;
        out->guard[zi] = slot->guard[zi];
    }
    out->zone_mask = slot->zone_mask;
}

/* ---- small helpers -------------------------------------------------------- */

static uint32_t ticks_to_s(TickType_t ticks)
{
    return (uint32_t)(ticks / configTICK_RATE_HZ);
}

static uint32_t ticks_to_ms(TickType_t ticks)
{
    return (uint32_t)ticks * (1000u / configTICK_RATE_HZ);
}
static uint32_t pc_link_abort_silence_ms(void)
{
    float cfg_ms = 0.0f;
    if (zones_config_get_pc_link_abort_silence_ms(&cfg_ms) && cfg_ms > 0.0f) {
        return (uint32_t)cfg_ms;
    }
    return PROFILE_EXECUTOR_PC_LINK_ABORT_SILENCE_MS;
}

static void reload_config_if_changed(void)
{
    uint32_t gen = zones_config_generation();
    if (gen == s_exec.config_generation) {
        return;
    }
    uint32_t prev = s_exec.config_generation;
    s_exec.config_generation = gen;

    uint8_t rechecked = 0;
    uint8_t changed_mask = 0;
    for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        if (!s_exec.zones[zi].active) continue;
        rechecked++;
        if (reload_zone_config(zi)) {
            changed_mask |= (uint8_t)(1u << zi);
        }
    }

    /* Logged even when changed_mask is 0 -- an edit to a zone this run isn't
     * driving, or to a field the executor doesn't consume (a zone name, the
     * calibration offset, which is read fresh every tick anyway), still moves
     * the generation, and the operator log should show that settings were
     * touched during a firing at all, not only when it altered this run. */
    ESP_LOGI(PE_TAG, "config reloaded mid-run (TODO.md 6A.7): generation %lu -> %lu, %u active zone(s) re-read, "
                  "zones changed 0x%02X",
             (unsigned long)prev, (unsigned long)gen, rechecked, changed_mask);
}

static bool live_pickup_validate_hard(void *ctx, const profile_t *candidate, char *err_msg, size_t err_cap)
{
    (void)ctx;
    return profiles_validate_candidate(candidate, (int)PROFILE_VALIDATE_HARD_LOCAL, NULL, 0, err_msg, err_cap);
}

/* docs/LIVE_PROFILE_EDIT.md pass 1, section 3/7: same one-counter-per-
 * tick poll shape as reload_config_if_changed() just above, called
 * immediately alongside it for the same "every edit lands at one known
 * point in the tick" reason. Only RUNNING adopts a swap (plan section 3) --
 * PAUSED/FAULTED runs have no control math for a swap to stay bumpless
 * against, and per owner decision 4 editing is still ALLOWED while paused or
 * faulted, it just doesn't take effect until the run is RUNNING again, at
 * which point this same poll picks it up (the generation counter does not
 * reset just because a tick was skipped). */
/* HIGH-2 (review) fix: this is called from executor_task_entry() OUTSIDE
 * s_exec.lock (see call site) -- heap_caps_malloc(), live_profile_load_
 * working_for_origin() (blocking NVS read) and live_pickup_validate_hard()
 * (takes the zones-config lock) must never run while s_exec.lock is held
 * (CLAUDE.md: "never hold a module lock across a blocking producer call").
 * The lock is taken twice, briefly: once up front to snapshot just enough
 * to decide whether there is anything to do, and once at the end to
 * re-check state/segment_index (which can move while unlocked -- a pause, a
 * segment advance, even a warm-started new run) and perform the window
 * check + swap. HARD-validate is done in the UNLOCKED middle section, ahead
 * of the (locked) window check, so its order relative to the window check
 * is reversed from a lock-once implementation -- both are still evaluated
 * before any generation advance or adoption decision is made, so this is
 * cosmetic, not a behavior change. */
static void reload_live_profile_if_changed(void)
{
    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    uint32_t gen = live_profile_generation();
    bool gen_changed = (gen != s_exec.live_edit_generation);
    uint8_t profile_id = s_exec.profile_id;
    bool running_now = (s_exec.state == PROFILE_EXEC_RUNNING);
    xSemaphoreGive(s_exec.lock);

    if (!gen_changed) {
        return;
    }
    if (!running_now) {
        /* HIGH-1: cheap early-out while PAUSED/FAULTED -- do NOT touch
         * s_exec.live_edit_generation (leaving it unconsumed is the whole
         * fix: the tick that finds RUNNING again re-observes this same
         * generation as still-new and does the real work then). Avoids
         * spending a malloc/NVS-read/HARD-validate on every tick of a
         * possibly long pause for an edit that cannot be adopted yet
         * anyway -- the locked re-check in the RUNNING path below still
         * exists to catch the state changing WHILE this function is
         * unlocked, which this early check cannot. */
        return;
    }

    /* Heap-allocated, not a stack local: check_executor_task_stack_budget.ps1's
     * ceiling (executor_task_entry's own reachable stack) is exceeded once
     * this profile_t-sized candidate sits on top of live_profile_load_
     * working()'s own internal decode buffer and profile_decode_blob()'s
     * frame further down the same call chain. Same established pattern as
     * profile_executor_firing_stats.c's firing_stats_persist()/
     * firing_stats_load() (heap_caps_malloc() + free() on every return
     * path, never a bigger stack) -- freed before every return below.
     * Internal DRAM: this path reads NVS via hal_kv, same reasoning as
     * firing_stats_load()'s own MALLOC_CAP_INTERNAL comment. */
    profile_t *candidate = heap_caps_malloc(sizeof(*candidate), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (candidate == NULL) {
        ESP_LOGE(PE_TAG, "reload_live_profile_if_changed: malloc(%u) failed -- live edit not adopted this tick",
                 (unsigned)sizeof(*candidate));
        /* HIGH-1: transient, not a definitive answer -- do NOT consume gen. */
        return;
    }

    /* MEDIUM-1 (review): refuses a pending record left over from a
     * DIFFERENT run's undecided edit (foreign origin_id) as well as "no
     * pending edit at all". HIGH (review, 2026-09-19): unlike a genuinely
     * TRANSIENT load failure, NONE_FOR_ORIGIN IS a definitive verdict about
     * the persisted state (there is nothing pending for this run's
     * origin_id at all right now), so it DOES consume the generation --
     * otherwise a live_profile_clear() (discard/save-as/overwrite) that
     * leaves no pending record behind bumps live_profile_generation() once,
     * and this poll re-observes that same bump as "still new" on every
     * future tick forever, spending a malloc + blocking NVS read on the
     * control task each time for an edit that will never exist to adopt. */
    live_profile_load_result_t load_result = live_profile_load_working_for_origin(profile_id, candidate);
    if (load_result != LIVE_PROFILE_LOAD_OK) {
        free(candidate);
        profile_live_pickup_poll_outcome_kind_t load_kind;
        switch (load_result) {
        case LIVE_PROFILE_LOAD_NONE_FOR_ORIGIN:
            load_kind = PROFILE_LIVE_PICKUP_POLL_NOT_APPLICABLE;
            break;
        case LIVE_PROFILE_LOAD_PERMANENT:
            load_kind = PROFILE_LIVE_PICKUP_POLL_LOAD_PERMANENT;
            break;
        case LIVE_PROFILE_LOAD_TRANSIENT:
        default:
            load_kind = PROFILE_LIVE_PICKUP_POLL_LOAD_TRANSIENT;
            break;
        }
        if (profile_live_pickup_should_advance_generation(load_kind, PROFILE_LIVE_PICKUP_OK)) {
            xSemaphoreTake(s_exec.lock, portMAX_DELAY);
            s_exec.live_edit_generation = gen;
            if (load_kind == PROFILE_LIVE_PICKUP_POLL_LOAD_PERMANENT) {
                /* Pass-3 review fix: live_profile.c already logged the
                 * specific reason at ESP_LOGE when it made this call; record
                 * a refusal here too so GET /api/profile/live's operator-
                 * facing view (MEDIUM-3's mechanism) surfaces it, rather than
                 * this generation silently vanishing the way the original
                 * bug made every unloadable pending edit disappear. */
                s_exec.live_edit_last_refusal.valid = true;
                s_exec.live_edit_last_refusal.generation = gen;
                s_exec.live_edit_last_refusal.result = PROFILE_LIVE_PICKUP_REFUSED_INVALID;
                snprintf(s_exec.live_edit_last_refusal.err_msg, sizeof(s_exec.live_edit_last_refusal.err_msg),
                         "pending live edit could not be loaded (see log) -- discarded");
            }
            xSemaphoreGive(s_exec.lock);
        }
        /* LOAD_TRANSIENT: leave the generation unconsumed so the next tick
         * retries the load. */
        return;
    }

    char err_msg[128];
    bool candidate_valid = live_pickup_validate_hard(NULL, candidate, err_msg, sizeof(err_msg));

    xSemaphoreTake(s_exec.lock, portMAX_DELAY);
    profile_live_pickup_poll_outcome_kind_t kind;
    profile_live_pickup_result_t result = PROFILE_LIVE_PICKUP_OK;
    if (s_exec.state != PROFILE_EXEC_RUNNING || s_exec.profile_id != profile_id) {
        /* HIGH-1: PAUSED/FAULTED (or this run ended/changed) since the
         * unlocked section started -- leave the generation unconsumed so
         * whichever future tick finds RUNNING again for the right run picks
         * this same edit back up (owner decision 4: editing while paused is
         * allowed, it just doesn't apply until resumed). */
        kind = PROFILE_LIVE_PICKUP_POLL_NOT_RUNNING;
    } else if (!candidate_valid) {
        kind = PROFILE_LIVE_PICKUP_POLL_CHECKED;
        result = PROFILE_LIVE_PICKUP_REFUSED_INVALID;
    } else if (live_edit_check_window(&s_exec.profile, candidate, s_exec.segment_index, err_msg, sizeof(err_msg))) {
        kind = PROFILE_LIVE_PICKUP_POLL_CHECKED;
        result = PROFILE_LIVE_PICKUP_REFUSED_WINDOW;
    } else {
        kind = PROFILE_LIVE_PICKUP_POLL_CHECKED;
        result = PROFILE_LIVE_PICKUP_OK;

        /* MEDIUM (review, 2026-09-19): io_seg_runtime_t.remaining_s is
         * seeded ONCE from the OLD profile's dwell_min at io_seg_start() and
         * only ever counted down from there (io_segs_tick()) -- swapping
         * s_exec.profile's content below does nothing to it, so an adopted
         * dwell_min change on a currently-active NON-BLOCKING relay/IO
         * segment was logged as adopted but silently had no effect on that
         * segment's own countdown. Re-derive it here, under this same lock,
         * against the OLD profile still in s_exec.profile (about to be
         * overwritten) so the amount of time already spent in the segment is
         * computed from the dwell it was actually started with, then applied
         * to the NEW dwell -- pure arithmetic, no I/O, no other io_seg_
         * runtime_t field touched (segment_index/dwelling/every firing-stats
         * accumulator keep the same continuity guarantee as the profile
         * swap itself). A BLOCKING relay/IO segment doesn't need this: it is
         * still the CURRENT schedule segment, so its own dwell is read fresh
         * out of s_exec.profile.segments[segment_index] by the ordinary
         * segment-stepping path once the swap below lands -- there is no
         * separate stale copy of its dwell anywhere.
         *
         * Pass-3 review fix (2026-09-19): this loop reads BOTH
         * s_exec.profile.segments[i] (the OLD profile, for elapsed_s) and
         * candidate->segments[i] (the NEW one, for new_dwell_s), so it must
         * be bounded by the SMALLER of the two segment_counts, not just the
         * old one -- a live edit that shortens the profile (legal per
         * live_edit_check_window(), segment_count may drop to
         * segment_index + 1) could otherwise index candidate->segments[]
         * past its own segment_count. An active IO segment at an index the
         * new profile no longer has is left to the ordinary end-of-segment
         * path once the swap lands, same as it already handles any segment
         * beyond the new segment_count today. Bound and per-segment
         * arithmetic are both pulled out to profile_executor_live_pickup.c
         * so they are host-testable on their own (that pure module's own
         * test executable can drive a shortened-profile case directly). */
        uint8_t seg_n = profile_live_pickup_io_seg_rederive_count(s_exec.profile.segment_count,
                                                                   candidate->segment_count, PROFILE_MAX_SEGMENTS);
        for (uint8_t i = 0; i < seg_n; i++) {
            io_seg_runtime_t *r = &s_exec.io_segs[i];
            if (!r->active || r->blocking) {
                continue;
            }
            r->remaining_s = profile_live_pickup_rederive_remaining_s(s_exec.profile.segments[i].dwell_min,
                                                                       r->remaining_s,
                                                                       candidate->segments[i].dwell_min);
        }

        /* Swap CONTENT only -- segment_index/segment_elapsed_s/dwelling/
         * io_segs/every firing-stats accumulator are left exactly as they
         * were, which is the continuity guarantee plan section 1 asks for
         * ("the running segment keeps running"). Race-free against a
         * concurrent segment advance: both the window check just above and
         * this swap run under the SAME lock acquisition, against the SAME
         * s_exec.segment_index read, so nothing can move between the two. */
        s_exec.profile = *candidate;
    }

    /* HIGH-1: only a CHECKED outcome (OK or a REFUSED_*) may consume this
     * generation -- see profile_live_pickup_should_advance_generation()'s
     * doc comment. */
    if (profile_live_pickup_should_advance_generation(kind, result)) {
        s_exec.live_edit_generation = gen;
    }

    if (kind == PROFILE_LIVE_PICKUP_POLL_CHECKED) {
        if (result == PROFILE_LIVE_PICKUP_OK) {
            ESP_LOGW(PE_TAG, "OPERATOR ACTION MID-FIRING: live profile edit adopted at segment %u",
                     s_exec.segment_index);
            s_exec.live_edit_last_refusal.valid = false;
        } else {
            /* MEDIUM-3 (review): recorded, not just logged, so pass 2's
             * planned GET /api/profile/live has something to read. */
            ESP_LOGW(PE_TAG, "live profile edit NOT adopted (result %d): %s", (int)result, err_msg);
            s_exec.live_edit_last_refusal.valid = true;
            s_exec.live_edit_last_refusal.generation = gen;
            s_exec.live_edit_last_refusal.result = result;
            snprintf(s_exec.live_edit_last_refusal.err_msg, sizeof(s_exec.live_edit_last_refusal.err_msg), "%s",
                     err_msg);
        }
    }
    xSemaphoreGive(s_exec.lock);
    free(candidate);
}

/* profile_resolve_on_off_rule() -- see profile_executor_internal.h for the
 * full contract. Pure lookup: linear scan of at most PROFILE_MAX_ON_OFF_RULES
 * entries, no side effects. */
on_off_trigger_rule_t profile_resolve_on_off_rule(const profile_t *p, uint8_t zone_index, uint8_t segment_index)
{
    on_off_trigger_rule_t resolved = {
        .enable = false,
        .phase_mask = 0,
        .direction_mask = 0,
        .temp_cmp = ON_OFF_TEMP_CMP_NONE,
        .temp_threshold_c = 0.0f,
        .time_start_s = 0,
        .time_stop_s = 0,
        .invert = false,
    };
    if (!p) {
        return resolved;
    }
    for (uint8_t ri = 0; ri < p->on_off_rule_count && ri < PROFILE_MAX_ON_OFF_RULES; ri++) {
        const profile_on_off_rule_t *pr = &p->on_off_rules[ri];
        if (!pr->enable || pr->zone_index != zone_index || pr->segment_index != segment_index) {
            continue;
        }
        resolved.enable = true;
        resolved.phase_mask = pr->phase_mask;
        resolved.direction_mask = pr->direction_mask;
        resolved.temp_cmp = (pr->temp_source == 1) ? (on_off_temp_cmp_t)pr->temp_cmp : ON_OFF_TEMP_CMP_NONE;
        resolved.temp_threshold_c = pr->temp_threshold_c;
        resolved.time_start_s = pr->time_start_s;
        resolved.time_stop_s = pr->time_stop_s;
        resolved.invert = pr->invert;
        break; /* validate_on_off_rules() (profiles_http.c) enforces at most one stored rule
                * per (segment, zone) pair -- first match is the only one */
    }
    return resolved;
}

/* HP-02 observability: accumulate the seconds a zone has spent asking for
 * (near) full heat while its relay was NOT commanded on, resetting the moment
 * the two agree again. Called once per active zone per tick, after this
 * tick's apply_relay() has settled relay_commanded_on. A few seconds here is
 * normal PWM timing (first window, min-off hold); minutes is the bench's
 * "duty 1.00, relay off, nothing else set" signature. */
static void profile_executor_account_relay_starvation(zone_runtime_t *z, float dt_s)
{
    if (z->duty >= PROFILE_EXECUTOR_RELAY_STARVED_DUTY && !z->relay_commanded_on) {
        z->relay_starved_s += dt_s;
    } else {
        z->relay_starved_s = 0.0f;
    }
}

/* ---- control task ----------------------------------------------------------- */

/* Accumulate one tick's measured dt into a whole-second counter without
 * per-tick rounding loss: the sub-second remainder carries in *rem_ms
 * (firing review 2026-10-09 item 3; the old (uint32_t)(dt_s + 0.5f) per tick
 * let a 0.4 s tick add 0 and a 0.6 s tick add 1, drifting elapsed time and so
 * dwell/rule timing). NaN/negative/absurd dt adds nothing. */
static void exec_elapsed_accumulate(uint32_t *seconds, uint16_t *rem_ms, float dt_s)
{
    if (!(dt_s > 0.0f) || dt_s > 3600.0f) return;
    uint32_t ms = (uint32_t)(dt_s * 1000.0f + 0.5f) + (uint32_t)*rem_ms;
    *seconds += ms / 1000u;
    *rem_ms = (uint16_t)(ms % 1000u);
}

void executor_task_entry(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PROFILE_EXECUTOR_TICK_MS));

        /* HIGH-2 (review): called OUTSIDE s_exec.lock -- it takes the lock
         * itself, briefly, at need (see its own doc comment). Placed ahead
         * of reload_config_if_changed()/the rest of the locked tick body on
         * purpose: an adopted edit must be visible to THIS tick's control
         * math the same as a config reload is (same comment at the old call
         * site, preserved below), and that requires it to have already run
         * before the lock below is taken. */
        reload_live_profile_if_changed();

        /* CLAUDE.md "never hold a module lock across a producer call": the
         * MAX31856/sim-backend read below can block on the bus/thermo owner
         * queue for up to MAX31856_LOCK_TIMEOUT_MS per channel, and every
         * other s_exec.lock caller (safety_link_frames.c's poll via
         * profile_executor_get_status(), HTTP/LCD/telemetry status readers)
         * waits with portMAX_DELAY -- so this read must not run while
         * holding the lock. Peek s_exec.state without the lock first, same
         * tolerated-race pattern as autotune_engine.c's task_entry() peeking
         * s_at.zone_index for other_zone_active_hint: only bother with the
         * SPI/sim read when the peek says RUNNING, since that is the only
         * branch below that consumes it. If the peek disagrees with the
         * definitive state read right after the lock is taken: a run that
         * stopped in this window lands in the not-RUNNING branch and the
         * snapshot is discarded; a run that started (or resumed) in this
         * window skips its control body for this one tick (see the
         * !peek_running skip below) -- never runs it on a missing reading. */
        bool peek_running = (s_exec.state == PROFILE_EXEC_RUNNING);
        ThermoChannelSnapshot pre_lock_snap;
        if (peek_running) {
            thermo_channels_read(s_exec.thermo_bus, &pre_lock_snap);
        }

        /* F1/F2: fresh Pico TRIPPED report, read outside s_exec.lock (same
         * rule as the watchdog's safety_link_get_status()). */
        bool pre_lock_pico_tripped = false;
        if (s_exec.safety) {
            safety_link_status_t pre_st;
            if (safety_link_get_status(s_exec.safety, &pre_st) == ESP_OK) {
                pre_lock_pico_tripped = pre_st.diag_ever_received &&
                                        !safety_link_is_stale(pre_st.age_ms, SAFETY_LINK_STALE_MS) &&
                                        pre_st.diag_state == SAFETY_LINK_DIAG_STATE_TRIPPED;
            }
        }

        xSemaphoreTake(s_exec.lock, portMAX_DELAY);
        TickType_t now = xTaskGetTickCount();
        s_exec.last_tick_tick = now; /* guard 9 -- updated every iteration regardless of run state */

        if (s_exec.state != PROFILE_EXEC_RUNNING) {
            force_all_relays_off();
            /* honor_leave_on=false unconditionally: PAUSED, FAULTED and every
             * post-DONE tick land here, and none of those is the one clean
             * ending (segment-stepping's DONE branch below) that is allowed
             * to honor a segment's leave_on_at_end -- see io_seg_finish()'s
             * doc comment. A segment already finished (DONE already swept
             * it, or it never started) costs nothing extra here. */
            io_segs_force_all_off(false);
            /* Spare-relay WP-3: retry a run-end aux OFF whose write failed.
             * Never while PAUSED (a pause holds aux at its last state while no fault is asserted -- see profile_executor_aux_fault_drop()) and a
             * no-op once the write has landed, so it cannot fight a later
             * manual toggle. */
            zone_off_pending_retry();
            if (s_exec.aux_off_pending && s_exec.state != PROFILE_EXEC_PAUSED) {
                force_aux_relays_off();
            }
            /* Backstop for K4, the same shape as the force-off above it: any
             * state that is not RUNNING must not be holding the safety
             * processor's permission to heat, whether or not the transition
             * that got here remembered to release it. Sends at most one
             * frame -- heat_enable_release() only puts anything on the wire
             * on the last-claimant edge, so every tick after the first is
             * free rather than a release frame per tick. PAUSED lands here too, which is what a pause is
             * supposed to mean -- see profile_executor_pause(). */
            heat_enable_release_backstop(HEAT_ENABLE_CLAIMANT_PROFILE);
            /* F1/F2: aux outputs drop on any safety fault even while paused
             * or idle; never assume K4 covers them (wiring varies). */
            profile_executor_aux_fault_drop(pre_lock_pico_tripped);
            /* PID_EXPANSION_PLAN.md Phase 7a: the moment a run first lands
             * in DONE or FAULTED, persist its firing stats -- this is the
             * "run completion" write, not waiting on the operator to press
             * Stop (profile_executor_halt() covers the OTHER ending, an
             * operator stop straight out of RUNNING/PAUSED that never
             * passes through here). PAUSED also lands in this branch every
             * tick and must NOT finalize -- a paused run can still resume --
             * hence the explicit state check rather than relying on
             * firing_stats_maybe_finalize()'s own (looser) IDLE-only guard. */
            bool fs_need_persist = false;
            profile_firing_run_record_t fs_rec;
            bool at_clean_run = false; /* PID_EXPANSION_PLAN.md Phase 7d: DONE only -- a FAULTED run
                                        * must never become adaptive-tune training data. */
            if (s_exec.state == PROFILE_EXEC_DONE || s_exec.state == PROFILE_EXEC_FAULTED) {
                fs_need_persist = firing_stats_maybe_finalize(&fs_rec);
                at_clean_run = (s_exec.state == PROFILE_EXEC_DONE);
            }
            xSemaphoreGive(s_exec.lock);
            if (fs_need_persist) {
                firing_stats_persist(&fs_rec);
                /* Outside s_exec.lock, same as the persist call above --
                 * this can do a batch fit and an NVS write of its own (via
                 * the flash worker), and must not hold the control loop's
                 * lock across either. Run-end is also the safe boundary
                 * that makes a mid-firing gain bump structurally
                 * impossible -- see adaptive_tune.c's top comment. */
                adaptive_tune_run_end(&fs_rec, at_clean_run);
            }
            continue;
        }

        /* The peek above missed a transition INTO RUNNING (profile_executor_run()
         * or profile_executor_resume() committed it between the peek and this
         * lock), so no pre-lock reading exists for this tick. Skip the control
         * body for this ONE tick rather than running it on an all-invalid
         * snapshot: an invalid tick would bump every active zone's guard-6
         * sensor_fault_streak, and guard_sensor_fault_debounce_ticks is
         * configurable down to 1 (ZONE_GUARD_DEBOUNCE_TICKS_MAX's range), which
         * would trip SENSOR_INVALID and fault a healthy run on its first tick.
         * Reading the bus here instead would reintroduce the lock-held producer
         * call this peek exists to avoid. Relays are already off (every
         * not-RUNNING tick forced them off), last_tick_tick above still feeds
         * guard 9, and prev_control_tick is left alone: run()/resume() set it
         * at their commit point, so the next tick's dt_ms runs from that
         * commit (about one period plus the peek-to-lock gap), not two. */
        if (!peek_running) {
            xSemaphoreGive(s_exec.lock);
            continue;
        }

        uint32_t dt_ms = ticks_to_ms(now - s_exec.prev_control_tick);
        if (dt_ms == 0) {
            dt_ms = PROFILE_EXECUTOR_TICK_MS;
        }
        s_exec.prev_control_tick = now;
        float dt_s = (float)dt_ms / 1000.0f;
        /* Real elapsed time for this run -- counts through a ramp-lock
         * stall (that's genuine wall-clock time passing while the plan's
         * remaining_s estimate quietly falls behind), only excluded while
         * PAUSED, since this whole block is skipped then. See
         * profile_exec_status_t.total_elapsed_s. */
        exec_elapsed_accumulate(&s_exec.total_elapsed_s, &s_exec.total_elapsed_rem_ms, dt_s);

        /* TODO relay/IO segments: tick every active NON-BLOCKING segment's
         * own hold timer, independent of whichever ramp/dwell segment is
         * current below -- see io_segs_tick()'s doc comment. */
        io_segs_tick(dt_s);

        /* Review-2 MEDIUM-1: retry pending OFF bits for relays this run does not own. */
        zone_off_pending_retry_running(now);

        /* --- Read every physical channel (raw), then combine per zone
         * (TODO.md 10.8) into that zone's control temperature ------------
         * Two index spaces below on purpose: ch_raw_c/ch_sensor_ok are
         * indexed by physical MAX31856 CHANNEL (whatever the bus/sim
         * backend answered), raw_c/sensor_ok stay indexed by ZONE like
         * every downstream consumer of them already expects (ramp-lock,
         * thermal_guard_input_t below, etc.) -- only their MEANING changed,
         * from "zone zi's one hard-wired channel" to "zone zi's combined
         * reading across every channel its thermo_mask names". Nothing
         * downstream needed to change to pick that up. */
        /* Consume the pre-lock read taken above (the !peek_running case
         * never reaches here -- see the skip right after the not-RUNNING
         * branch). */
        const float *ch_raw_c = pre_lock_snap.raw_c;
        const bool *ch_sensor_ok = pre_lock_snap.ok;
        float raw_c[MAX31856_CHANNEL_COUNT];    /* per ZONE: this zone's combined raw reading */
        bool zone_on_off[MAX31856_CHANNEL_COUNT]; /* per ZONE: docs/ON_OFF_ZONE.md sec 1 --
                                                    * snapshotted once per tick, same "one consistent
                                                    * picture" reasoning as raw_c/sensor_ok, and handed to
                                                    * thermal_guard_tick() both as this zone's own
                                                    * on_off_zone flag and as every OTHER zone's
                                                    * peer_is_on_off[] so guard 8/cross-zone excludes an
                                                    * on/off zone from both sides of the comparison. */
        bool zone_guard_exempt[MAX31856_CHANNEL_COUNT]; /* on/off OR monitor-only: guards 1/2/3/4/cross-zone are
                                                    * not meaningful for it (no heat commanded, or no
                                                    * setpoint). Guards 5/6/7 still run. */
        bool sensor_ok[MAX31856_CHANNEL_COUNT]; /* per ZONE: true iff >=1 assigned channel is valid --
                                                  * this IS guard 6's extended "invalid" definition
                                                  * (TODO.md 10.8: "all assigned thermocouples for
                                                  * this zone are invalid"), computed once here rather
                                                  * than inside thermal_guard.c, which already takes
                                                  * sensor_ok as an opaque caller-decided bool (see
                                                  * thermal_guard.h's thermal_guard_input_t comment) --
                                                  * nothing in thermal_guard.c needed to change. */
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            raw_c[zi] = NAN;
            sensor_ok[zi] = false;
            zone_on_off[zi] = zone_is_on_off(zi);
            s_exec.zones[zi].monitor_only = zone_is_monitor_only(zi);
            zone_guard_exempt[zi] = zone_on_off[zi] || s_exec.zones[zi].monitor_only;
            if (!s_exec.zones[zi].active) continue;
            /* Live read every tick, not cached in zone_runtime_t -- same
             * "no hardware-safety handover needed" reasoning
             * zones_config_apply_cal()'s per-tick call already relies on:
             * unlike relay_mask, a thermo_mask change never needs a
             * force-off-the-old-mask dance (see reload_zone_config()),
             * because it only ever affects what this zone READS, never
             * what it drives. */
            uint8_t tmask = 0;
            zones_config_get_thermo_mask(zi, &tmask);
            bool valid = false;
            float combined = thermo_combine(ch_raw_c, ch_sensor_ok, MAX31856_CHANNEL_COUNT, tmask, &valid);
            raw_c[zi] = combined;
            sensor_ok[zi] = valid;
            s_exec.zones[zi].actual_valid = valid;
            s_exec.zones[zi].actual_c = valid ? zones_config_apply_cal(zi, combined) : NAN;

            /* Cross-zone coupling filter (PID_EXPANSION_PLAN.md 2c/Phase 3b,
             * BLOCKING review finding 3): updated exactly once per control
             * tick, via this helper rather than inline inside
             * zone_feedforward() -- see coupling_filter_tick()'s own doc
             * comment for why. */
            coupling_filter_tick(&s_exec.zones[zi], valid, dt_s);
        }

        /* --- Config reload (TODO.md 6A.7) ----------------------------------
         * Placed after the readings are stored and before any control math:
         * the bumpless seed a gain change needs is only honest against THIS
         * tick's measurement, and every gain/mode/mask/threshold the passes
         * below read must already be the post-edit one, so an edit can never
         * be half-applied across a single tick's decide/apply split.
         * reload_live_profile_if_changed() ran earlier this same tick,
         * BEFORE s_exec.lock was taken (HIGH-2 review fix, see its call site
         * near the top of this loop and its own doc comment) -- its result
         * is already reflected in s_exec.profile by the time this line
         * runs, same "visible to this tick's control math" property this
         * comment already documents for the config reload below. */
        reload_config_if_changed();

        /* --- Ramp-lock (TODO.md 6A.5(d)): is every active, not-already-
         * faulted zone within band of the CURRENT shared target? Only
         * zones still actually being driven count -- a zone that already
         * tripped its own guard is guard 1/2/4/7's problem, not ramp-lock's
         * to also hold the whole firing hostage over. -------------------- */
        bool lock_ok = true;
        uint8_t lagging = 0;
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!s_exec.zones[zi].active || s_exec.zones[zi].faulted) continue;
            /* docs/ON_OFF_ZONE.md sec 1: an on/off zone has no actual_c
             * obligation to a shared setpoint -- leaving it in this loop
             * would let a zone sitting at ambient (or with no thermocouple
             * at all) freeze the whole firing's ramp forever. Hard
             * requirement, not an optimisation. */
            /* SPARE_RELAY_ONOFF_PLAN.md sec 10: a monitor-only zone (relay
             * converted to an aux) cannot heat, so it must not hold the
             * firing's ramp either. Both exclusions are the one shared
             * predicate. */
            if (!profile_executor_zone_drives_run(zi)) continue;
            /* ONE-SIDED (2026-09-03, hot-start defect): only a zone that is
             * COLDER than the shared target by more than the band can hold
             * the lock. A zone that is HOTTER than target by the same
             * amount used to hold it too (fabsf made the two directions
             * bit-identical), but the lock exists to stop the setpoint
             * outrunning a zone that CANNOT KEEP UP -- inherently a
             * one-sided problem. Holding the setpoint back for a zone that
             * is already too hot does not help that zone (it can only
             * passively cool; a frozen setpoint changes nothing about that)
             * and actively denies it the rising setpoint that would let it
             * reconverge from above by the schedule catching up to it. A
             * hot start (e.g. a re-fire soon after a previous run, one zone
             * still 50C+ above a cold baseline target) used to freeze
             * segment_elapsed_s/target_c on tick 1 and never release, since
             * nothing else in the executor ever un-sticks a zone that
             * commands zero duty (guards 1/2/7 all gate on nonzero/>=0.5
             * commanded duty). !sensor_ok[zi] still holds the lock exactly
             * as before -- an invalid reading says nothing about direction
             * and must still stop the ramp. */
            if (!sensor_ok[zi] || (s_exec.target_c - s_exec.zones[zi].actual_c) > EXEC_RAMP_LOCK_BAND_C(zi)) {
                lock_ok = false;
                lagging |= (uint8_t)(1u << zi);
            }
        }
        s_exec.ramp_lock_held = !lock_ok;
        s_exec.ramp_lock_lagging_mask = lagging;

        /* Read once per tick, reused below by both the segment-stepping
         * stretch decision and the ramp-assist accounting block further
         * down (removes that block's own duplicate read). */
        bool ramp_assist_on = ramp_assist_cfg_enabled();

        /* PID_EXPANSION_PLAN.md sec 7.1/7.2: sustained-lag detection runs
         * HERE, right after this tick's own lock_ok/lagging are known and
         * BEFORE the stretch decision below reads lag_sustained/lag_held_s/
         * lag_start_actual_c -- not down in the "ramp assist accounting"
         * block further down, which runs after segment-stepping. Moved
         * deliberately (2026-09-03, sec 7.2 landing): auto-stretch's rate
         * computation needs THIS tick's freshly updated lag state, not the
         * previous tick's -- calling ramp_assist_zone_lag_tick() after the
         * stretch decision would make every stretch decision one tick stale,
         * including the tick lag_held_s actually crosses EXEC_SUSTAINED_LAG_S
         * (the stretch would not engage until the tick after). Safe to run
         * this early: lag_tick only reads lagging_now (already final, from
         * the ramp-lock loop just above) and each zone's own actual_c
         * (already this tick's fresh reading, same ordering note lag_tick's
         * own doc comment already relies on) -- it does not depend on
         * anything the segment-stepping block below computes. */
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!s_exec.zones[zi].active || s_exec.zones[zi].faulted) continue;
            bool lagging_now = (lagging & (1u << zi)) != 0;
            ramp_assist_zone_lag_tick(&s_exec.zones[zi], lagging_now, dt_s);
        }

        /* --- Ramp/dwell segment stepping (shared across all active zones) - */
        bool segment_changed = false; /* reboot breadcrumb: worth its own NVS write, see below */
        const profile_segment_t *seg = &s_exec.profile.segments[s_exec.segment_index];
        /* PID_EXPANSION_PLAN.md sec 7.2: auto-stretch. lock_ok alone (sec
         * 7.1) already guarantees every ramp endpoint is eventually reached
         * by freezing target_c/segment_elapsed_s while a zone lags -- that
         * guarantee is unconditional and untouched below. What this adds:
         * once the lock would otherwise hold INDEFINITELY (a lagging zone's
         * lag_sustained true), and only with the flag on, replace the
         * commanded seg->ramp_c_per_hr with the slowest sustained-lagging
         * zone's own demonstrated achievable rate for this tick's advance,
         * so the setpoint creeps forward at a rate the kiln can actually
         * hold instead of sitting fully still. stretch_rate_c_per_s is the
         * sentinel -1.0f (no stretch, use the strict hold) unless the flag
         * is on and at least one lagging zone qualifies -- see
         * ramp_assist_stretch_rate_c_per_s()'s own doc comment
         * (profile_executor_internal.h). Only relevant to a genuine ramp
         * (positive seg->ramp_c_per_hr); a step segment's instant jump is
         * untouched, same as before this feature existed. */
        float stretch_rate_c_per_s = -1.0f;
        bool stretched_this_tick = false;
        if (!lock_ok && seg->seg_kind == PROFILE_SEG_KIND_ZONE_RAMP && !s_exec.dwelling &&
            seg->ramp_c_per_hr > 0.0f) {
            stretch_rate_c_per_s = ramp_assist_stretch_rate_c_per_s(&s_exec, lagging, ramp_assist_on);
            stretched_this_tick = (stretch_rate_c_per_s >= 0.0f);
        }
        /* Zero unless a ramp is actually being commanded this tick. Falling
         * through this default covers dwelling, a step segment (target_c jumps
         * in one tick -- there is no sustained rate for tau to act on), and
         * ramp-lock: while the lock holds the setpoint deliberately is NOT
         * moving, so paying feedforward for a climb that is not happening
         * would push duty up on exactly the zones the lock is waiting for the
         * laggards to catch up with. */
        s_exec.target_rate_c_per_s = 0.0f;
        if (seg->seg_kind == PROFILE_SEG_KIND_RELAY_IO) {
            /* Owner's request, verbatim (profiles_http.h): a relay/IO segment
             * is not a temperature step at all, so none of the ramp-lock
             * machinery above (which exists solely to keep the shared
             * SETPOINT from outrunning a lagging zone) applies to it -- it
             * always advances on wall-clock time, lock_ok or not. target_c/
             * dwelling/segment_elapsed_s (the ZONE_RAMP machine's own state)
             * are deliberately left untouched here. */
            if (!s_exec.io_segs[s_exec.segment_index].active) {
                io_seg_start(s_exec.segment_index, seg); /* first tick this segment is current */
            }
            bool ready_to_advance;
            if (seg->io_blocking) {
                /* Behaves exactly like a dwell: the shared schedule does not
                 * move to the next segment until this one's own hold time has
                 * elapsed -- "blocking... before the next segment", the
                 * owner's own wording. Reuses segment_elapsed_s (this
                 * segment owns it exclusively while current, same as a
                 * ZONE_RAMP dwell does) rather than the per-segment
                 * remaining_s a non-blocking segment uses, since a blocking
                 * segment's timer IS the schedule's timer. */
                exec_elapsed_accumulate(&s_exec.segment_elapsed_s, &s_exec.segment_elapsed_rem_ms, dt_s);
                ready_to_advance = s_exec.segment_elapsed_s >= seg->dwell_min * 60u;
            } else {
                /* "Runs WITH the next segment": advance on the very same tick
                 * it starts. Its own on/off state keeps running afterward,
                 * independent of segment_index -- see io_segs_tick() above. */
                ready_to_advance = true;
            }
            if (ready_to_advance) {
                if (seg->io_blocking) {
                    /* A blocking segment's own command always ends the moment
                     * the schedule moves past it -- there is nothing else
                     * still "running alongside" it for leave_on_at_end to
                     * apply to (validate_io_segment() already refuses that
                     * flag on a blocking segment at save time; this is belt
                     * and braces, not a second decision point). */
                    io_seg_finish(s_exec.segment_index, false);
                }
                s_exec.segment_index++;
                s_exec.segment_elapsed_s = 0;
                s_exec.segment_elapsed_rem_ms = 0;
                segment_changed = true;
                if (s_exec.segment_index >= s_exec.profile.segment_count) {
                    exec_enter_terminal_state(PROFILE_EXEC_DONE);
                    force_all_relays_off();
                    /* The one path allowed to honor leave_on_at_end: the
                     * schedule reached its own natural end with every
                     * segment accounted for, exactly as planned. */
                    io_segs_force_all_off(true);
                    release_profile_relay_claim();
                    run_snapshot_buf_t done_snap;
                    capture_run_snapshot(&done_snap);
                    xSemaphoreGive(s_exec.lock);
                    run_state_note(RUN_STATE_PHASE_DONE, &done_snap.snap);
                    if (cfg_fs_is_available()) {
                        /* FILESYSTEM.md dual-write window: this run reached a genuine
                         * completion with cfg_fs live -- counts toward the exit criterion's
                         * "one complete firing run file-backed" leg. Sticky/idempotent. */
                        dualwrite_window_note_firing_complete();
                    }
                    continue;
                }
                seg = &s_exec.profile.segments[s_exec.segment_index];
            }
        } else if (lock_ok || stretched_this_tick) {
            exec_elapsed_accumulate(&s_exec.segment_elapsed_s, &s_exec.segment_elapsed_rem_ms, dt_s);
            if (!s_exec.dwelling) {
                float new_target;
                if (seg->ramp_c_per_hr <= 0.0f) {
                    new_target = seg->target_c;
                } else {
                    /* PID_EXPANSION_PLAN.md sec 7.2: while stretched_this_tick,
                     * advance at the achievable rate instead of the commanded
                     * one -- everything else below (the "reached" clamp to
                     * seg->target_c, the dwelling handoff) is identical either
                     * way, so the next segment's own target/shape is read
                     * fresh from s_exec.profile once this one arrives and is
                     * untouched by which rate got it there. */
                    float rate_c_per_hr = stretched_this_tick ? (stretch_rate_c_per_s * 3600.0f)
                                                                : seg->ramp_c_per_hr;
                    float direction = (seg->target_c >= s_exec.target_c) ? 1.0f : -1.0f;
                    new_target = s_exec.target_c + direction * rate_c_per_hr * (dt_s / 3600.0f);
                    bool reached = (direction > 0.0f) ? (new_target >= seg->target_c) : (new_target <= seg->target_c);
                    if (reached) {
                        new_target = seg->target_c;
                    } else {
                        /* The rate actually driving target_c this tick,
                         * signed and in the units the feedforward term
                         * wants -- the stretched rate while stretched, same
                         * as before otherwise. Only claimed while the ramp
                         * still has distance left to run: the tick that
                         * arrives at the segment target is already a partial
                         * one, and the ticks after it are a dwell. */
                        s_exec.target_rate_c_per_s = direction * rate_c_per_hr / 3600.0f;
                    }
                }
                s_exec.target_c = new_target;
                if (s_exec.target_c == seg->target_c) {
                    s_exec.dwelling = true;
                    s_exec.segment_elapsed_s = 0;
                s_exec.segment_elapsed_rem_ms = 0;
                    /* PID_EXPANSION_PLAN.md sec 7.3: dwell credit spend, at
                     * the single point this shared schedule actually enters
                     * a dwell. Resets every active zone's banked dwell_
                     * credit_s (see ramp_assist_dwell_credit_spend()'s own
                     * doc comment for why that reset is unconditional even
                     * with the flag off) and, ONLY when ramp_assist_cfg_
                     * enabled() is true, records how much of dwell_min this
                     * dwell's ready_to_advance check below will treat as
                     * already satisfied. With the flag off this is always
                     * 0.0f, so the check below is bit-identical to before
                     * this feature existed. */
                    s_exec.dwell_credit_applied_s = ramp_assist_dwell_credit_spend(
                        &s_exec, (float)(seg->dwell_min * 60u), ramp_assist_cfg_enabled());
                    /* PID_EXPANSION_PLAN.md sec 7.6, replacing the earlier
                     * "circular-dwell hazard" freeze (2026-09-03): credit
                     * banked AFTER this point (during this very dwell) is now
                     * deliberately ALLOWED to shorten this same dwell -- the
                     * owner's decision, because the freeze made the terminal
                     * dwell's banked credit (the heavy-load motivating case)
                     * permanently unspendable, there being no "next" dwell to
                     * apply it to. What used to be an unconditional freeze is
                     * now BOUNDED instead: dwell_credit_cap_s below fixes the
                     * ceiling once, right here, so no amount of later top-up
                     * can widen it, and the `else` branch's target_reached
                     * check (ramp_assist_dwell_target_reached()) means no
                     * amount of credit can end the dwell before the zone is
                     * physically there. See EXEC_DWELL_CREDIT_MAX_FRACTION's
                     * own doc comment (profile_executor_internal.h) for the
                     * full two-bound argument. dwell_credit_applied_s itself
                     * stays a one-time entry snapshot, exactly as before --
                     * only what the `else` branch DOES with it changed. */
                    s_exec.dwell_credit_cap_s = (float)(seg->dwell_min * 60u) * EXEC_DWELL_CREDIT_MAX_FRACTION;
                    if (s_exec.dwell_credit_applied_s > s_exec.dwell_credit_cap_s) {
                        s_exec.dwell_credit_applied_s = s_exec.dwell_credit_cap_s;
                    }
                }
            } else {
                s_exec.target_c = seg->target_c;
                /* PID_EXPANSION_PLAN.md sec 7.6: bounded in-dwell credit.
                 * nominal_s is the un-credited floor -- reaching it always
                 * ends the dwell, credit or not, exactly as before sec 7.3
                 * ever existed (this is what keeps a credit-less run, or any
                 * run with ramp_assist_cfg_enabled() == false, bit-identical:
                 * total_spend_s below is 0.0f in both those cases, since
                 * dwell_credit_applied_s was never set above cap_s == 0 and
                 * ramp_assist_dwell_credit_peek_min_s() only ever sees zones
                 * whose dwell_credit_s ramp_assist_dwell_credit_tick() left
                 * at 0 -- see that function's own gate). credited_threshold_s
                 * is the EARLY-exit floor credit can buy, never used alone:
                 * it only takes effect together with target_reached (Bound
                 * 2), so a credited exit can happen before nominal_s, but
                 * never before the zone is physically at target_c. */
                uint32_t nominal_s = seg->dwell_min * 60u;
                float total_spend_s = ramp_assist_dwell_credit_total_spend_s(
                    &s_exec, s_exec.dwell_credit_applied_s, s_exec.dwell_credit_cap_s);
                uint32_t credited_threshold_s = (total_spend_s >= (float)nominal_s)
                    ? 0u : nominal_s - (uint32_t)total_spend_s;
                bool target_reached = ramp_assist_dwell_target_reached(&s_exec);
                bool dwell_done = (s_exec.segment_elapsed_s >= nominal_s) ||
                    (s_exec.segment_elapsed_s >= credited_threshold_s && target_reached);
                if (dwell_done) {
                    s_exec.segment_index++;
                    if (s_exec.segment_index >= s_exec.profile.segment_count) {
                        exec_enter_terminal_state(PROFILE_EXEC_DONE);
                        force_all_relays_off();
                        /* This is ALSO a clean end -- the run's LAST segment
                         * happened to be a ZONE_RAMP dwell, but an earlier
                         * non-blocking relay/IO segment may still be active
                         * and running alongside it (that's the whole point of
                         * non-blocking). Same one path allowed to honor
                         * leave_on_at_end as the RELAY_IO branch's own DONE
                         * transition above -- see io_seg_finish()'s doc
                         * comment. */
                        io_segs_force_all_off(true);
                        release_profile_relay_claim();
                        /* A clean end, and it MUST be recorded as one: a
                         * completed firing whose record still says RUNNING
                         * would greet the next boot as an interrupted one and
                         * teach the operator to ignore the warning. */
                        run_snapshot_buf_t done_snap;
                        capture_run_snapshot(&done_snap);
                        xSemaphoreGive(s_exec.lock);
                        run_state_note(RUN_STATE_PHASE_DONE, &done_snap.snap);
                        if (cfg_fs_is_available()) {
                            /* FILESYSTEM.md dual-write window: this run reached a genuine
                             * completion with cfg_fs live -- counts toward the exit criterion's
                             * "one complete firing run file-backed" leg. Sticky/idempotent. */
                            dualwrite_window_note_firing_complete();
                        }
                        continue;
                    }
                    s_exec.dwelling = false;
                    s_exec.segment_elapsed_s = 0;
                s_exec.segment_elapsed_rem_ms = 0;
                    seg = &s_exec.profile.segments[s_exec.segment_index];
                    /* target_c stays where it is -- that's the new segment's ramp start. */
                    /* A segment boundary is the transition that most changes
                     * what an interrupted record would say, so it gets its own
                     * write rather than waiting for the periodic refresh. */
                    segment_changed = true;
                }
            }
        }

        /* --- Firing quality stats accumulation (PID_EXPANSION_PLAN.md Phase
         * 7a), per active zone --------------------------------------------
         * Placed here: target_c/dwelling/segment_index are already final for
         * this tick (the segment-stepping block above has run), and
         * s_exec.zones[zi].actual_c/actual_valid were set by the reading
         * loop earlier this same tick. The per-zone math itself lives in
         * firing_stats_zone_tick() (a few hundred lines up, pure and
         * argument-driven) specifically so a host test can drive a synthetic
         * error sequence through it tick-by-tick without a real FreeRTOS
         * task loop -- see that function's own doc comment. */
        s_exec.fs_target_min_c = isnan(s_exec.fs_target_min_c) ? s_exec.target_c : fminf(s_exec.fs_target_min_c, s_exec.target_c);
        s_exec.fs_target_max_c = isnan(s_exec.fs_target_max_c) ? s_exec.target_c : fmaxf(s_exec.fs_target_max_c, s_exec.target_c);
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!s_exec.zones[zi].active || s_exec.zones[zi].faulted) continue;
            if (s_exec.zones[zi].monitor_only) continue; /* no setpoint tracking to score */
            firing_stats_zone_tick(&s_exec.zones[zi], s_exec.target_c, s_exec.dwelling, s_exec.total_elapsed_s,
                                    s_exec.segment_index, dt_s);
            /* PID_EXPANSION_PLAN.md Phase 7d, Layer 1: harvest one dwell
             * observation per settled dwell, opt-in per zone, off by
             * default -- see adaptive_tune.c's own doc comment. Placed
             * right alongside firing_stats_zone_tick() for the same
             * reason: dwelling/actual_c/actual_valid are already final for
             * this tick. */
            adaptive_tune_zone_tick(zi, s_exec.zones[zi].actual_c, s_exec.zones[zi].actual_valid,
                                     s_exec.zones[zi].duty, s_exec.dwelling, s_exec.ambient_c, dt_s);
        }

        /* --- Ramp assist (PID_EXPANSION_PLAN.md sec 7): sec 7.3's dwell-
         * credit ACCRUAL, per zone, ALWAYS -- and auto-stretch time
         * accounting, gated on the flag. Sustained-lag detection itself
         * (ramp_assist_zone_lag_tick()) already ran earlier this tick, right
         * after lock_ok/lagging were computed -- see that call site's own
         * comment for why it had to move ahead of the segment-stepping
         * block instead of living here alongside dwell-credit/stretch-
         * accounting. Placed here for the same reason the firing-stats loop
         * just above is: target_c/dwelling/segment_index and every zone's
         * actual_c/lagging bit are already final for this tick. See
         * profile_executor_ramp_assist.c's own doc comment for why neither
         * call below (dwell-credit SPEND is the exception -- see the
         * dwelling-transition code above) is new schedule-altering control
         * behaviour. */
        {
            /* ramp_assist_on: read once, further up this tick (before
             * segment-stepping needed it too) -- not re-read here. */
            /* stretch_ramping_now stays gated on !s_exec.dwelling -- sec 7.2's
             * auto-stretch time accounting is explicitly "during an actual
             * ramp, not a dwell" (see profile_executor_ramp_assist.c's own
             * doc comment on ramp_assist_stretch_tick()) and that is
             * untouched by the sec 7.3 extension below. */
            bool stretch_ramping_now = (seg->seg_kind == PROFILE_SEG_KIND_ZONE_RAMP) && !s_exec.dwelling;
            /* credit_ramping_now: PID_EXPANSION_PLAN.md sec 7.3 extension
             * (2026-09-03) -- deliberately DOES NOT check !s_exec.dwelling.
             * See ramp_assist_credit_should_accrue()'s own doc comment
             * (profile_executor_internal.h) for the gap this closes (ramp-
             * lock releases, ending the ramp step, at a much wider band than
             * credit's own in_band test, so a heavily-lagging zone routinely
             * starts dwelling before it ever enters the credit band) and the
             * hazard it does NOT create (credit banked during this segment's
             * OWN dwell cannot shorten that same dwell -- dwell_credit_
             * applied_s below is captured exactly once, at dwell entry, and
             * every subsequent tick's threshold check reuses that frozen
             * value; it is never recomputed from a live dwell_credit_s). */
            bool credit_ramping_now = ramp_assist_credit_should_accrue(seg->seg_kind);
            for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
                if (!s_exec.zones[zi].active || s_exec.zones[zi].faulted) continue;
                /* NOT `lagging` (the 25C ramp-lock mask) -- that band decides
                 * when the schedule freezes and dwell credit must not be
                 * re-tied to it (it made credit and in_band mutually
                 * exclusive; see ramp_assist_dwell_credit_tick()'s doc
                 * comment, profile_executor_internal.h, and profile_executor_
                 * ramp_assist.c). Credit's own gate is "behind schedule at
                 * all", against the moving s_exec.target_c. */
                if (s_exec.zones[zi].monitor_only) continue;
                bool behind_schedule_now = (s_exec.zones[zi].actual_c < s_exec.target_c);
                /* seg->target_c, not s_exec.target_c: the segment's own
                 * final target, not the still-interpolating commanded
                 * value -- see ramp_assist_dwell_credit_tick()'s doc
                 * comment (profile_executor_internal.h). */
                ramp_assist_dwell_credit_tick(&s_exec.zones[zi], credit_ramping_now, behind_schedule_now,
                                              seg->target_c, dt_s);
            }
            ramp_assist_stretch_tick(&s_exec, s_exec.segment_index, ramp_assist_on, stretch_ramping_now,
                                     s_exec.ramp_lock_held, dt_s);
        }

        /* --- Per-zone approach-rate cap (PID_EXPANSION_PLAN.md sec 3.6d /
         * PER_ZONE_TARGET_DESIGN_STUDY.md option (b)): update each active
         * zone's own effective_target_c toward the shared s_exec.target_c,
         * before the control-mode pass below reads it. Deliberately runs
         * AFTER segment-stepping (s_exec.target_c is this tick's final
         * value) and BEFORE the per-zone pass (feedforward/PID/guard all
         * need the freshly updated value, not last tick's).
         *
         * Uncapped (zones_config_get_approach_rate_cap_c_per_hr() answers
         * 0, the default and every zone before this field existed):
         * effective_target_c is simply set to s_exec.target_c, unconditionally
         * -- no rate limit, no stored state carried between ticks, bit-
         * identical to reading s_exec.target_c directly.
         *
         * Capped: effective_target_c moves toward s_exec.target_c by at most
         * cap_c_per_hr * dt_s/3600 this tick, in whichever direction closes
         * the gap -- this is what makes the cap ONLY EVER TIGHTEN the
         * segment's own commanded rate: if cap_c_per_hr is numerically >=
         * the rate s_exec.target_c is actually moving at this tick,
         * max_step >= the gap every tick, so effective_target_c tracks
         * s_exec.target_c exactly and the cap is a no-op, exactly as
         * PER_ZONE_TARGET_DESIGN_STUDY.md's option (b) requires ("can only
         * ever tighten... never loosen"). Only when the segment's own rate
         * (or a step-segment's instant jump, or a dwell's already-arrived
         * target) would require a larger single-tick move than the cap
         * allows does effective_target_c lag behind, continuing to close
         * the gap on later ticks even after the shared schedule has moved on
         * to a dwell -- "reached... just reached later for the capped zone,"
         * per the design study's own description of this option. */
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!s_exec.zones[zi].active || s_exec.zones[zi].faulted) continue;
            zone_runtime_t *z = &s_exec.zones[zi];
            float cap_c_per_hr = 0.0f;
            (void)zones_config_get_approach_rate_cap_c_per_hr(zi, &cap_c_per_hr);
            if (cap_c_per_hr <= 0.0f) {
                z->effective_target_c = s_exec.target_c;
                continue;
            }
            if (!isfinite(z->effective_target_c)) {
                /* First tick this zone has ever been active under a cap
                 * (freshly activated mid-run, or a stale NAN somehow
                 * survived -- defensive) -- snap rather than climb from an
                 * undefined starting point. profile_executor_run.c already
                 * seeds this at run start for the normal case. */
                z->effective_target_c = s_exec.target_c;
                continue;
            }
            float max_step_c = cap_c_per_hr * (dt_s / 3600.0f);
            float delta_c = s_exec.target_c - z->effective_target_c;
            if (delta_c > max_step_c) {
                delta_c = max_step_c;
            } else if (delta_c < -max_step_c) {
                delta_c = -max_step_c;
            }
            z->effective_target_c += delta_c;
        }

        /* --- Control mode, per active zone (pass 1: decide, don't apply yet)
         * -------------------------------------------------------------------
         * Split from the apply+guard pass below so the load cap (TODO.md
         * 6A.5 load-staggering) can see every active zone's raw want-on
         * before deciding which ones actually get the relay this tick. */
        bool want_relay_on[MAX31856_CHANNEL_COUNT] = {0};
        /* heater_state as it stood BEFORE this tick's heater_output_duty()/
         * heater_output_bangbang() call, so the load-cap loop below can hand
         * a denied zone back the "decided OFF" outcome through
         * heater_output_note_denied() (HP-02 pass-1 window-advance fix,
         * 2026-09-27). */
        bool relay_on_before_tick[MAX31856_CHANNEL_COUNT] = {0};
        uint32_t cycles_before_tick[MAX31856_CHANNEL_COUNT] = {0};
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!s_exec.zones[zi].active || s_exec.zones[zi].faulted) continue;
            zone_runtime_t *z = &s_exec.zones[zi];
            relay_on_before_tick[zi] = z->heater_state.relay_on;
            cycles_before_tick[zi] = z->heater_state.cycle_count;
            z->relay_denied_reason = PROFILE_EXEC_RELAY_DENIED_NONE; /* re-derived every tick below */

            float duty = 0.0f;
            z->last_pid_terms = (pid_terms_t){0}; /* only ZONE_CONTROL_MODE_PID/PID_FUZZY below fill this in */
            if (s_exec.zones[zi].monitor_only) {
                /* Monitor-only (SPARE_RELAY_ONOFF_PLAN.md sec 10): no PID, no
                 * output, no heater_state movement. Its thermocouple is still
                 * read above and the guards below still run. */
                z->duty = 0.0f;
                z->cooling_limited_hold_s = 0.0f;
                z->cooling_limited = false;
                continue;
            }
            if (zone_on_off[zi]) {
                /* HP-02 root cause (bench, 2026-09-25..27, ESP 0fb8ad98): an
                 * on/off-typed zone whose control_mode is still PID used to
                 * fall into the switch below and run the full PID/PWM chain
                 * -- z->duty read 1.00 and heater_state marched through
                 * windows -- while its relay was owned entirely by the on/off
                 * decision path further down (apply_relay() is never called
                 * for an on/off zone from the heater apply loop), so the
                 * dashboard showed a zone calling for full heat that could
                 * never heat, with nothing else set. docs/ON_OFF_ZONE.md
                 * sec 3: an on/off zone's relay follows its rules and the
                 * failsafe, never a PID output. Report duty 0 and leave
                 * heater_state alone; the on/off path keeps its own
                 * cycle_count mirror below. */
                z->duty = 0.0f;
                z->cooling_limited_hold_s = 0.0f;
                z->cooling_limited = false;
                continue;
            }
            switch (z->control_mode) {
            case ZONE_CONTROL_MODE_PID: {
                /* &z->pid_cfg directly -- no copy, no adjustment. Bit-for-bit
                 * the same call this always was; see pid_family_zone_tick()'s
                 * header comment for why the body moved but the behavior
                 * didn't. */
                duty = pid_family_zone_tick(z, zi, &z->pid_cfg, sensor_ok[zi], dt_s, dt_ms,
                                            &want_relay_on[zi]);
                break;
            }
            case ZONE_CONTROL_MODE_PID_FUZZY: {
                /* strength_pct == 0 (or a non-finite error/rate -- faulted
                 * thermocouple) makes pid_fuzzy_prepare_gains() hand back
                 * z->pid_cfg's own kp/ki/kd unchanged (pid_fuzzy_adjust()'s
                 * documented contract), so this path degrades to exactly the
                 * PID case above at the safe default -- same call, same
                 * shared body, only the gains it's given can differ. */
                pid_cfg_t fuzzy_cfg;
                /* docs/audits/simc_sole_gain_writer_2026-09-14.md, Option B:
                 * freeze fuzzy (force strength_pct to 0 for this tick) only
                 * while this zone is BOTH dwelling and opted into adaptive
                 * tuning -- s_exec.dwelling is the same single, module-wide
                 * flag adaptive_tune.c's own dwell_just_entered detection
                 * relies on (see that file's comment on why one shared flag
                 * is a valid proxy for "this zone is in a dwell adaptive_
                 * tune.c may harvest"), so this is exactly the window that
                 * module's settle/observation logic can turn into a K_dc
                 * training point. Fuzzy stays fully active on every ramp,
                 * approach, and on any zone not opted into adaptive tuning. */
                bool harvest_freeze = s_exec.dwelling && adaptive_tune_get_enabled(zi);
                pid_fuzzy_prepare_gains(z, zi, harvest_freeze, dt_s, &fuzzy_cfg);
                duty = pid_family_zone_tick(z, zi, &fuzzy_cfg, sensor_ok[zi], dt_s, dt_ms,
                                            &want_relay_on[zi]);
                break;
            }
            case ZONE_CONTROL_MODE_BANGBANG: {
                bool want_raw = z->relay_commanded_on;
                if (sensor_ok[zi]) {
                    /* PID_EXPANSION_PLAN.md sec 3.6d: z->effective_target_c,
                     * this zone's own (possibly rate-capped) commanded
                     * setpoint, control-mode-agnostic same as the PID path
                     * above -- a capped BANGBANG zone stops calling for heat
                     * once it reaches its OWN capped setpoint rather than the
                     * shared destination every other zone may already be
                     * dwelling at. Uncapped, bit-identical to s_exec.target_c. */
                    float setpoint_c = zone_commanded_setpoint_c(z, zi);
                    if (z->actual_c < setpoint_c - EXEC_BANGBANG_HYSTERESIS_C(zi)) {
                        want_raw = true;
                    } else if (z->actual_c > setpoint_c + EXEC_BANGBANG_HYSTERESIS_C(zi)) {
                        want_raw = false;
                    }
                } else {
                    want_raw = false; /* no trustworthy reading -> never command heat */
                }
                want_relay_on[zi] = heater_output_bangbang(&z->heater_state, &z->heater_cfg, want_raw, dt_ms);
                duty = want_relay_on[zi] ? 1.0f : 0.0f;
                /* Cooling-limited is a PID-mode-only diagnostic (see its
                 * field comment) -- clear it rather than let a stale true
                 * from a PID period before a mode switch linger. */
                z->cooling_limited_hold_s = 0.0f;
                z->cooling_limited = false;
                break;
            }
            case ZONE_CONTROL_MODE_OFF:
            default:
                want_relay_on[zi] = false;
                duty = 0.0f;
                z->cooling_limited_hold_s = 0.0f;
                z->cooling_limited = false;
                break;
            }
            z->duty = duty;
        }

        /* --- Load cap (TODO.md 6A.5 load-staggering): if more active zones
         * want on than zones_config_get_max_simultaneous_relays() allows,
         * suppress the excess -- lowest deferred_on_ms (least "owed") first,
         * so debt self-corrects over time instead of one zone starving
         * forever. Suppressed PID zones accumulate the denied on-time in
         * deferred_on_ms and get it back on their own next window (pass 1,
         * above, next tick); bang-bang zones accumulate it too but it's
         * never paid back -- no window to pay it into, so it's a documented
         * "lost, not deferred" gap for that mode only. 0 = unlimited,
         * unchanged/no-op behavior (matches every run before this pass). */
        uint8_t cap = zones_config_get_max_simultaneous_relays();
        if (cap > 0) {
            uint8_t wanted = 0;
            for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
                if (s_exec.zones[zi].active && !s_exec.zones[zi].faulted && want_relay_on[zi]) wanted++;
            }
            while (wanted > cap) {
                int8_t victim = -1;
                for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
                    if (!s_exec.zones[zi].active || s_exec.zones[zi].faulted || !want_relay_on[zi]) continue;
                    if (victim < 0 || s_exec.zones[zi].deferred_on_ms < s_exec.zones[victim].deferred_on_ms) {
                        victim = (int8_t)zi;
                    }
                }
                if (victim < 0) break; /* shouldn't happen given wanted > cap, but don't loop forever */
                want_relay_on[victim] = false;
                s_exec.zones[victim].deferred_on_ms += (float)dt_ms;
                /* Pass 1 already recorded this zone's relay as ON in
                 * heater_state (heater_output_duty()/_bangbang() run before
                 * the cap can deny). Without this, the min-on hold defended
                 * an on-time that never reached the contacts and cycle_count
                 * counted a switch that never happened. Rewind to the
                 * "decided OFF" outcome from the pre-tick snapshot. */
                heater_output_note_denied(&s_exec.zones[victim].heater_state,
                                          relay_on_before_tick[victim], cycles_before_tick[victim]);
                s_exec.zones[victim].relay_denied_reason = PROFILE_EXEC_RELAY_DENIED_LOAD_CAP;
                wanted--;
            }
        }

        /* --- Apply relays + guards, per active zone -------------------------- */
        bool run_faulted_this_tick = false;
        /* docs/ON_OFF_ZONE.md sec 6: an on/off zone counts toward
         * max_simultaneous_relays (a contactor coil draws the same current
         * whatever it switches) but is suppressed LAST -- only after every
         * heater has already been considered by the pass-1 load-cap loop
         * above. want_relay_on[] at this point already reflects that cap-
         * adjusted heater decision (an on/off zone's own want_relay_on[]
         * entry is the pass-1 default false: pass 1 skips on/off zones
         * outright since the HP-02 fix -- it is decided below, per zone,
         * AFTER this count is taken), so seeding the running
         * count from it and growing it as on/off zones are granted a relay
         * below gives on/off zones the cap's last, unclaimed slots without
         * ever revisiting a heater's already-decided state. Denial is
         * logged, never deferred (there is no window to repay a denied
         * on/off cycle into -- see the pass-1 load-cap comment for the same
         * point about bang-bang zones). cap == 0 is unlimited, unchanged. */
        uint8_t on_off_cap = zones_config_get_max_simultaneous_relays();
        uint8_t relays_on_count = 0;
        if (on_off_cap > 0) {
            for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
                if (s_exec.zones[zi].active && !s_exec.zones[zi].faulted && want_relay_on[zi]) {
                    relays_on_count++;
                }
            }
        }
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT && !run_faulted_this_tick; zi++) {
            if (!s_exec.zones[zi].active || s_exec.zones[zi].faulted) continue;
            zone_runtime_t *z = &s_exec.zones[zi];

            /* On/off zones are NOT applied here -- their relay command is
             * decided below (after this tick's own guard 5/6 result is
             * known) and applied there instead. Heater zones are
             * unaffected: this call and its position are bit-identical to
             * before this feature existed. */
            /* Firing review 2026-10-09 item 2: a heater zone's relay is
             * written AFTER this tick's thermal_guard_tick(), so a tripping
             * reading never produces an ON write followed by the guard's OFF.
             * The guards only need to know whether heat is authority-blocked
             * this tick, a pure query that apply_relay() repeats for its own
             * gate (and uses for its edge logging, so z->heat_blocked itself
             * is still written only there). */
            bool heater_zone_applies_relay = !zone_on_off[zi] && !z->monitor_only;
            bool heat_blocked_for_guards = z->heat_blocked;
            if (heater_zone_applies_relay) {
                uint32_t blocked_sources_pre = 0;
                heat_blocked_for_guards = relay_authority_zone_blocked(s_exec.safety, zi, &blocked_sources_pre);
            }

            /* Guards 1/2/3/7 all gate their multi-tick accumulation windows
             * on commanded_duty (thermal_guard.c), and this used to be fed
             * z->relay_commanded_on -- the POST-PWM, POST-load-cap relay
             * state apply_relay() just set a few lines up. heater_output_
             * duty() (heater_output.c) time-proportions any duty strictly
             * between 0 and 1 across its own window (HEATER_DEFAULT_WINDOW_
             * MS, 60s default), so relay_commanded_on drops to false on
             * every off-pulse of that PWM cycle -- and EVERY ONE of these
             * four windows resets on that same transition (thermal_guard.c:
             * guard 1/2 reset when commanded_duty < progress_duty_min,
             * guard 3 needs a CONTIGUOUS duty==0 run and resets the instant
             * duty is nonzero again, guard 7 needs a CONTIGUOUS duty>0 run
             * and resets the instant duty drops to 0). None of guard 1's
             * 300s, guard 2's 120s, guard 3's 120s or guard 7's 600s window
             * can ever complete at any duty strictly between 0 and 1 -- at
             * duty 0.6 the longest contiguous armed run is 36s. A frozen
             * thermocouple during a partial-duty firing was never caught.
             *
             * Fixed the same way autotune_engine.c's identical defect was:
             * feed the guards the INTENDED duty (z->duty, decided in this
             * tick's own "pass 1" above, before load-cap/PWM/authority ever
             * touch it), not the instantaneous post-PWM relay state. No
             * guard constant changes -- the existing windows are fine once
             * they can actually accumulate.
             *
             * Two things intentionally do NOT feed through as "intended,
             * heat commanded" here, each a deliberate choice (both ways
             * have a real argument, so both are spelled out rather than
             * assumed):
             *
             *   LOAD CAP (this tick's want_relay_on[zi] possibly forced
             *   false by the cap loop just above, z->duty left untouched):
             *   still reported as z->duty, NOT zeroed. Treated as the same
             *   kind of scheduling detail PWM chopping is -- deferred_on_ms
             *   is a genuine, self-correcting promise that this zone gets
             *   its owed on-time back, so a HEALTHY load-cap-throttled zone
             *   should still show real rise across guard 1's window (it is
             *   getting real heat, just multiplexed with other zones) and
             *   should not read as "not commanded" just because this
             *   PARTICULAR tick's relay stayed open for a sibling zone
             *   instead. The alternative (zero whenever deferred) would
             *   recreate this exact defect under a different name: a zone
             *   cycling in and out of load-cap deferral resets these
             *   windows on THAT transition instead of PWM's. The remaining
             *   risk -- a zone so persistently starved by the cap that it
             *   never gets enough real on-time to keep up with rate_cfg --
             *   is judged the CORRECT case for guard 1 to eventually catch
             *   and escalate, not silently swallow: that zone is genuinely
             *   not being heated adequately, whatever the reason.
             *
             *   AUTHORITY/INTERLOCK BLOCK (relay_authority_zone_blocked(),
             *   surfaced here as z->heat_blocked): reported as ZERO whenever
             *   z->heat_blocked reads true, want_relay_on[zi] or not.
             *   z->heat_blocked is now refreshed by apply_relay() on EVERY
             *   tick (see that function's own comment) -- an earlier version
             *   of this fix gated the zero on want_relay_on[zi] too, on the
             *   theory that heat_blocked was only fresh when want_on was
             *   true; that theory, and the gate built on it, were WRONG (a
             *   review caught it as a regression: it broke guard 3 during a
             *   real authority block by re-introducing an alternating
             *   zero/nonzero pattern at the PWM period -- see apply_relay()
             *   and profile_executor_guard_commanded_duty()'s own comments
             *   for the full account). Unlike the load cap, a block is a
             *   DETERMINISTIC, ALREADY-REPORTED fact -- the safety link down,
             *   or an earlier guard trip's own latch -- and re-deriving the
             *   same fact through guard 1/2's noisier "no rise despite
             *   commanded heat" statistics would only produce a redundant,
             *   less specific alarm (or, for a global block, one on EVERY
             *   active zone at once) on top of the real one relay_authority_
             *   on_blocked()/the per-zone latch already surfaced. Guard 3
             *   (welded contact) correctly reads this zone as duty==0 either
             *   way -- the relay genuinely is open -- and guard 7 (frozen
             *   sensor) correctly goes inert, since "is the process
             *   responding to commanded heat" is not a meaningful question
             *   while no heat is being commanded at all.
             *
             * (autotune_engine.c's own copy of this reasoning has a
             * corrected note on relay_authority_zone_blocked() not feeding
             * back into that file's duty -- see that file's comment; that
             * file's guard wiring is unchanged by this pass.) */
            float commanded_duty_for_guards = profile_executor_guard_commanded_duty(heat_blocked_for_guards, z->duty);

            /* Guard 1's "climbing" rise requirement (thermal_guard.c: delta
             * >= rate_cfg * elapsed_min whenever error > progress_band_c) is
             * a FIXED absolute rate with no knowledge of what rate is
             * actually being commanded. Review defect 2: this whole PWM fix
             * widened guard 1's real window from "can only ever complete at
             * duty==1.0" to "completes at any duty >= progress_duty_min
             * (0.5)" -- i.e. most of a real firing -- and a perfectly
             * healthy zone tracking a modest ramp (20 C/hr = 0.33 C/min is
             * routine: candling, quartz inversion, thick ware, a large
             * kiln's own thermal lag) legitimately lags more than
             * progress_band_c behind a setpoint that is ITSELF only moving
             * that fast. Demanding rate_cfg's full 0.5 C/min (30 C/hr) rise
             * from such a zone is demanding it outrun the setpoint it is
             * chasing -- guaranteed to false-trip at 300s, and under the
             * load cap (every zone forced to a high duty, lagging further,
             * rising at roughly a fair share of the commanded rate) able to
             * trip EVERY zone in the same window at once, not a cascade.
             *
             * Fix: cap the expected rate at the commanded ramp's own rate
             * (s_exec.target_rate_c_per_s, set a few hundred lines up --
             * "Only claimed while the ramp still has distance left to run",
             * explicitly 0.0f during a dwell/step-segment/ramp-lock tick) --
             * min(configured rate_cfg, |commanded ramp rate|) -- rather than
             * changing rate_cfg's own stored value (a per-zone config field
             * read elsewhere) or thermal_guard.c's guard 1 math itself (no
             * guard constant needs to change, same as the duty fix above).
             * Deliberately NOT applied during a dwell (target_rate_c_per_s
             * is exactly 0.0f then, which would demand zero rise forever,
             * i.e. disarm guard 1 completely for a lagging dwell -- the
             * ORIGINAL "must actively catch up" case this guard exists for
             * and must not be softened): the cap only ever narrows the
             * requirement while a ramp is genuinely in progress, and is a
             * per-tick LOCAL override of a copy of z->guard_cfg -- the
             * zone's own stored guard_cfg.sanity_rate_c_per_min (read
             * elsewhere) is never mutated. */
            thermal_guard_cfg_t guard_cfg_this_tick = z->guard_cfg;
            /* Review fix (2026-09-04) on top of d800a60's per-zone approach-
             * rate cap: the rate handed to profile_executor_guard_sanity_
             * rate() must describe the SAME setpoint that .setpoint_c below
             * carries (zone_commanded_setpoint_c(), this zone's own capped
             * value), not the shared schedule's rate -- otherwise guard 1
             * demands a rise faster than this zone's own setpoint moves,
             * exactly the false trip that cap was written to prevent. An
             * uncapped zone reads a 0 cap and gets s_exec.target_rate_c_per_s
             * verbatim, bit-identical to before. See profile_executor_guard_
             * zone_ramp_rate()'s own doc comment. */
            float guard_cap_c_per_hr = 0.0f;
            (void)zones_config_get_approach_rate_cap_c_per_hr(zi, &guard_cap_c_per_hr);
            float guard_zone_rate_c_per_s = profile_executor_guard_zone_ramp_rate(
                s_exec.target_rate_c_per_s, guard_cap_c_per_hr,
                z->effective_target_c != s_exec.target_c);
            guard_cfg_this_tick.sanity_rate_c_per_min =
                profile_executor_guard_sanity_rate(z->guard_cfg.sanity_rate_c_per_min, guard_zone_rate_c_per_s);
            /* 2026-09-10 fix (docs/audits/esp_panic_after_zone0_guard_trip_
             * 2026-09-10.md): guard 1's climbing window must never be
             * shorter than this zone's own dead_time_s+tau_s -- see
             * thermal_guard_derive_climb_window_floor_s()'s comment. z->ff_*
             * are already this zone's validated model (zone_load_model()
             * above this tick loop; ff_enabled false means no trustworthy
             * model, in which case this passes model_valid=false and the
             * derivation returns 0.0f -- "don't touch window_s", identical
             * to before this field existed). Computed fresh every tick, on
             * the same per-tick local copy as sanity_rate_c_per_min above,
             * never written back to the zone's persisted guard_cfg. */
            guard_cfg_this_tick.climb_window_floor_s = thermal_guard_derive_climb_window_floor_s(
                z->ff_tau_s, z->ff_dead_time_s, z->ff_enabled);

            thermal_guard_input_t gin = {
                .sensor_ok = sensor_ok[zi],
                .measurement_c = raw_c[zi], /* RAW -- a calibration offset must not hide an out-of-range sensor */
                /* PID_EXPANSION_PLAN.md sec 3.6d / PER_ZONE_TARGET_DESIGN_
                 * STUDY.md section 2.4: this zone's OWN commanded setpoint
                 * (rate-limited toward the shared s_exec.target_c by this
                 * zone's approach_rate_cap_c_per_hr, computed a few hundred
                 * lines up), not the shared destination directly -- an
                 * uncapped zone (the default) has effective_target_c ==
                 * s_exec.target_c always, so this is bit-identical to the
                 * previous `.setpoint_c = s_exec.target_c` for every zone
                 * before this field existed. A capped zone's guard now sees
                 * what it is actually being asked to do THIS tick, avoiding
                 * this repo's own "fake setpoint" bug class
                 * (project_autotune_feeds_fake_setpoint.md) rather than
                 * reproducing it for the new mechanism. */
                .setpoint_c = zone_commanded_setpoint_c(z, zi),
                .commanded_duty = commanded_duty_for_guards,
                .dt_s = dt_s,
                /* Guard 8 (TODO.md 6A.3/6A.5): this tick's whole-bus snapshot,
                 * raw and un-averaged, so a zone can be compared against its
                 * neighbours in the same chamber. Costs nothing extra -- the
                 * arrays were already filled at the top of this tick. The
                 * guard is inert unless the zone also has a configured
                 * cross_zone_max_delta_c, which is not set by default. */
                .peer_c = raw_c,
                .peer_ok = sensor_ok,
                .peer_count = MAX31856_CHANNEL_COUNT,
                .peer_index_self = zi,
                /* docs/ON_OFF_ZONE.md sec 1: guards 1/2/3/4/9 disabled
                 * for an on/off zone (thermal_guard.c gates each block on
                 * this), guards 5/6/7/8 unaffected. peer_is_on_off excludes
                 * every on/off zone from the OTHER side of guard 9 too, for
                 * every zone's tick, not only an on/off zone's own. */
                .peer_is_on_off = zone_guard_exempt,
                .on_off_zone = zone_guard_exempt[zi],
            };
            if (thermal_guard_tick(&z->guard_state, &guard_cfg_this_tick, &gin)) {
                if (escalate_guard_trip(zi, z->guard_state.reason, z->guard_state.detail)) {
                    run_faulted_this_tick = true;
                }
            }

            if (heater_zone_applies_relay && !z->faulted && !run_faulted_this_tick) {
                apply_relay(zi, want_relay_on[zi]);
                if (z->heat_blocked) {
                    z->relay_denied_reason = PROFILE_EXEC_RELAY_DENIED_AUTHORITY;
                }
                profile_executor_account_relay_starvation(z, dt_s);
            }

            /* docs/ON_OFF_ZONE.md sec 3/4/6/8 -- WIRED. Computes this
             * tick's verdict for an on/off zone through the pure on_off_
             * trigger_decide() core, gates it through the actuation-layer
             * min_on_s/min_off_s hold (profile_executor_on_off_actuation_
             * gate(), independent state -- sec "requirement 4"), applies the
             * cap suppression this loop's relays_on_count is tracking, and
             * finally calls apply_relay() -- the SAME chokepoint a heater
             * uses, same claimed_relay_mask/relay_authority_zone_blocked()/
             * kiln_io_owner path, no new relay write path (plan sec 6's
             * non-negotiable). UNEXERCISED ON HARDWARE per this task's
             * instructions -- no on/off zone has been bench-tested with this
             * path live; every zone remains ZONE_TYPE_HEATER (0) until a UI
             * (plan step 6, elsewhere) can type one, so this is inert on
             * every board today exactly like step 7's report-only version
             * was, EXCEPT that from here on the code path is the real,
             * production actuation path a host test can drive end to end. */
            if (zone_on_off[zi]) {
                bool failsafe_on;
                zones_config_get_failsafe_state(zi, &failsafe_on);
                uint16_t min_on_s = 30, min_off_s = 30;
                zones_config_get_min_on_s(zi, &min_on_s);
                zones_config_get_min_off_s(zi, &min_off_s);
                float hyst_c = 2.0f;
                zones_config_get_hyst_c(zi, &hyst_c);
                /* Refreshed HERE, directly, rather than by reading z->
                 * heat_blocked: for a heater zone heat_blocked is already
                 * fresh by this point in the tick because apply_relay() ran
                 * for it earlier in this same loop iteration (line above).
                 * An on/off zone's apply_relay() call happens LATER, below
                 * -- deliberately, because it needs this tick's own verdict
                 * as its want_on argument -- so z->heat_blocked here would
                 * still be LAST tick's answer. relay_authority_zone_
                 * blocked() is a pure query (owners/relay_authority.c) with
                 * no side effect on the latch/mask state, so calling it a
                 * second time this tick (apply_relay() below calls it again
                 * to actually gate its write) is free and cannot desync
                 * anything -- it is asking the same authority module the
                 * same question twice in one tick, not maintaining two
                 * copies of an answer. */
                uint32_t authority_sources_now = 0;
                bool authority_blocked_now = relay_authority_zone_blocked(s_exec.safety, zi, &authority_sources_now);
                (void)authority_sources_now;
                /* Look up this (zone, current segment)'s stored rule, if
                 * any -- profile_t is sparse (PROFILE_MAX_ON_OFF_RULES
                 * entries, each self-keyed by segment_index/zone_index), so
                 * this is a linear scan of at most 8 entries, once per
                 * on/off zone per tick. No match, OR a match with enable==0
                 * (a profile author disabled the rule without deleting it),
                 * both mean "no rule for this segment" -- precedence level
                 * 6. temp_source == 2 (named zone's TC)/3 (executor
                 * setpoint) are reserved encoding (profiles_types.h) not
                 * yet resolved here; a rule using either axis is treated as
                 * temp_cmp NONE (its temperature axis drops out of the AND
                 * as a tautology) until a later step resolves them -- never
                 * silently mis-evaluated against the wrong reading. */
                /* Rule lookup, direction/phase/run-state facts, the dead-sensor
                 * fail-safe and the AND of every run-ending path are all in the
                 * shared builder (profile_executor_relay_io.c), the same one the
                 * aux path uses, so the two producers cannot drift. */
                bool bypass_hold = false;
                on_off_trigger_input_t oin = profile_executor_zone_on_off_input(
                    zi, failsafe_on, min_on_s, min_off_s, hyst_c, authority_blocked_now,
                    stretched_this_tick, dt_s, &bypass_hold);
                /* Requirement 4: the actuation layer enforces min_on_s/
                 * min_off_s AGAIN, independent of on_off_trigger_decide()'s
                 * own hold timer, so a decision-core bug cannot chatter the
                 * physical relay. bypass_hold mirrors on_off_trigger_
                 * decide()'s own precedence levels 1-3 exactly (run_ending_
                 * failsafe, guard_5_6_tripped_now, !run_running_now) -- a
                 * safety-relevant transition is never held at THIS layer
                 * either, matching plan sec 3's "[the hold] sits... below
                 * the safety levels so safety is never delayed by it." Cap
                 * suppression (sec 6, on/off zones LAST) is applied inside
                 * the same call -- see profile_executor_on_off_zone_tick()'s
                 * header comment for why this whole chain is one production
                 * function rather than inline code here. */
                /* Captured BEFORE the tick call: both on_off_trigger_decide()
                 * (inside profile_executor_on_off_zone_tick(), via
                 * z->on_off_trigger_state) and the actuation-gate hold
                 * mutate their state in place, so this is the only chance to
                 * see the "before" side of either transition. prev_decided_on
                 * also doubles as axis_temp()'s on_ref for the diagnostic
                 * mirror above -- the hysteresis memory is the state from
                 * BEFORE this tick's call, same as the real decision core
                 * uses. */
                bool prev_decided_on = z->on_off_trigger_state.commanded_on;
                bool prev_actuated_on = z->on_off_actuated_on;
                /* The hold accumulator BEFORE the gate mutates it -- the
                 * only place the previous state's real held duration is
                 * still readable (2026-09-09, opus review defect C2: the
                 * trace used to read this back after the tick, when the
                 * gate has already reset it to dt_s on any transition, so
                 * every RELAY line printed held_prior_s=1.0). */
                float prev_actuated_held_s = z->on_off_actuated_held_s;
                on_off_zone_tick_result_t tick_result = profile_executor_on_off_zone_tick(
                    &z->on_off_trigger_state, &z->on_off_actuated_on, &z->on_off_actuated_held_s,
                    &oin, bypass_hold, relays_on_count, on_off_cap);
                bool actuated_on = tick_result.actuated_on;
                bool decided_on = z->on_off_trigger_state.commanded_on;

                /* UART trace for the owner's bench-readiness decision
                 * (docs/audits/on_off_zone_bench_readiness_2026-09-08.md's
                 * reading guide has the annotated walkthrough) -- pulled out
                 * to profile_executor_relay_io.c so a host test can call it
                 * directly, the same reason profile_executor_on_off_
                 * actuation_gate() itself is a separate function rather than
                 * inline code here. See that function's own header comment
                 * for the volume budget and edge-trigger reasoning. */
                profile_executor_on_off_log_transition(zi, &oin, prev_decided_on, decided_on,
                                                        prev_actuated_on, actuated_on,
                                                        prev_actuated_held_s, z->on_off_actuated_held_s,
                                                        min_on_s, min_off_s, bypass_hold,
                                                        tick_result.cap_denied);
                if (tick_result.cap_denied) {
                    ESP_LOGW(PE_TAG, "zone %u (on/off) denied its relay this tick: "
                                  "max_simultaneous_relays (%u) already reached by other zones -- "
                                  "on/off zones are always suppressed last, denial is not deferred",
                             zi, (unsigned)on_off_cap);
                }
                if (actuated_on) {
                    relays_on_count++;
                }

                /* Requirement 5: relay-cycles accounting. On/off zones never
                 * run heater_output_bangbang()/heater_output_duty() (pass 1
                 * skips an on/off zone before its control-mode switch --
                 * this was NOT true before the HP-02 fix, when an on/off
                 * zone whose control_mode was still PID ran the whole PWM
                 * chain and both this mirror and heater_output.c advanced
                 * the same counter), so z->heater_state.cycle_count is otherwise NEVER touched
                 * for an on/off zone and relay_cycles_add() below (unchanged
                 * code) would silently attribute it zero cycles for a whole
                 * firing regardless of how often its relay actually
                 * switched. Mirrored from heater_output.c's own note_
                 * transition() (same struct, same counter, same "count any
                 * transition" rule) so the existing unchanged accounting
                 * code just below picks this up for free -- no separate
                 * on/off relay-life code path to drift from the heater one. */
                if (actuated_on != z->heater_state.relay_on) {
                    z->heater_state.cycle_count++;
                    z->heater_state.relay_on = actuated_on;
                }

                apply_relay(zi, actuated_on);
                /* HP-02 observability: name WHY an on/off zone's relay is off
                 * this tick, precedence highest first -- the authority block
                 * and the cap are things an operator can act on now; "no rule
                 * for this segment" is the plan's rule 6 and is expected for
                 * a segment the author left the zone out of, but a whole run
                 * of it (see profile_executor_run.c's start-time refusal) was
                 * exactly the bench symptom. */
                if (!actuated_on) {
                    if (z->heat_blocked) {
                        z->relay_denied_reason = PROFILE_EXEC_RELAY_DENIED_AUTHORITY;
                    } else if (tick_result.cap_denied) {
                        z->relay_denied_reason = PROFILE_EXEC_RELAY_DENIED_LOAD_CAP;
                    } else if (!oin.rule.enable) {
                        z->relay_denied_reason = PROFILE_EXEC_RELAY_DENIED_ON_OFF_NO_RULE;
                    }
                }
                /* relay_starved_s is a duty-vs-relay disagreement counter and
                 * an on/off zone reports duty 0, so it stays 0 here by
                 * construction; the call keeps the two paths symmetric. */
                profile_executor_account_relay_starvation(z, dt_s);
            }

            /* Contact-cycle accounting (TODO.md 6A.1): hand relay_cycles.c
             * only what this zone has switched since the last tick. Its
             * relays switch as a group, so every relay in the mask takes the
             * same count. */
            uint32_t cycles_now = z->heater_state.cycle_count;
            if (cycles_now != z->cycles_reported) {
                uint8_t mask = 0;
                if (zones_config_get_relay_mask(zi, &mask) && mask != 0) {
                    relay_cycles_add(mask, cycles_now - z->cycles_reported);
                }
                z->cycles_reported = cycles_now;
            }
        }

        /* Spare-relay WP-3: aux outputs, evaluated after every zone has
         * claimed its load-cap slot (aux is suppressed last, like an on/off
         * zone) and before the sweep so the sweep judges the state this tick
         * actually left. Skipped if this tick already ended the run -- the
         * terminal transition has turned every aux OFF. */
        if (s_exec.state == PROFILE_EXEC_RUNNING) {
            profile_executor_aux_tick(dt_s, stretched_this_tick, relays_on_count, on_off_cap);
        }

        /* --- Unowned-relay sweep (TODO.md 6A.7) -----------------------------
         * Last thing in the tick that touches copper, deliberately: it must
         * judge the relay state this tick actually left behind, including a
         * guard trip's force-off above that reported an error. Running it
         * before the apply pass would have it clearing contacts the same tick
         * is about to re-command, i.e. chattering the relay it is supposed to
         * be protecting. RUNNING-only for now: the other states force
         * everything off through masks they can still name, and an IDLE
         * executor has no business second-guessing a relay an operator turned
         * on manually after a firing ended. */
        sweep_unowned_relays();

        /* --- History sample (TODO.md section 0 / 6A.9), ALL active zones,
         * one per 30s (2026-09-01: was a single representative zone -- see
         * profile_executor.h's doc comment on profile_history_entry_t for
         * why, and why every zone here now, not just the run's lowest-
         * indexed one. That old gate on "is the ONE representative zone
         * still active" also silently stopped sampling every OTHER zone the
         * moment that one zone alone dropped to a per-zone fault, even while
         * the rest of the run kept going -- gating on "is any zone active"
         * instead fixes that too. s_exec.history is NULL if the PSRAM
         * allocation in profile_executor_run() failed; skip rather than
         * fault the run over losing the graph. ---------------------------- */
        bool any_zone_active_for_history = false;
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (s_exec.zones[zi].active) { any_zone_active_for_history = true; break; }
        }
        if (s_exec.history != NULL && !run_faulted_this_tick && any_zone_active_for_history &&
            ticks_to_s(now - s_exec.history_last_sample_tick) >= HISTORY_SAMPLE_PERIOD_S) {
            s_exec.history_last_sample_tick = now;
            float actual_c[MAX31856_CHANNEL_COUNT];
            float duty[MAX31856_CHANNEL_COUNT];
            uint8_t guard[MAX31856_CHANNEL_COUNT];
            uint8_t inactive_mask = 0;
            for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
                zone_runtime_t *hz = &s_exec.zones[zi];
                if (!hz->active) {
                    inactive_mask |= (uint8_t)(1u << zi);
                    continue;
                }
                actual_c[zi] = hz->actual_valid ? hz->actual_c : NAN;
                duty[zi] = hz->duty;
                guard[zi] = (uint8_t)hz->guard_state.reason;
            }
            history_slot_t *slot = &s_exec.history[s_exec.history_head];
            history_pack(slot, ticks_to_s(now - s_exec.history_run_start_tick), s_exec.target_c,
                        actual_c, duty, guard, inactive_mask);
            s_exec.history_head = (uint16_t)((s_exec.history_head + 1u) % HISTORY_MAX_SAMPLES);
            if (s_exec.history_count < HISTORY_MAX_SAMPLES) {
                s_exec.history_count++;
            }
        }

        /* --- Reboot breadcrumb (TODO.md 6A.3) -------------------------------
         * Captured under the lock, written outside it, same as the
         * relay_cycles persist below: the lock also serves
         * profile_executor_get_status(), which the dashboard polls every 2 s.
         * There is no reason to make a status request wait behind an erase. */
        /* ROADMAP.md M15 "Mode-state sprawl" -- see profile_executor_
         * internal.h's own doc comment on exec_mode_state_check() for why
         * this is the one real call site that checks it, and why plain
         * assert() rather than a project convention (none exists) on the
         * non-ESP_PLATFORM side. Placed here: every field the check reads
         * is final for this tick, and it must run before the lock is
         * released.
         *
         * Owner decision 2026-09-24 (docs/audits/profile_executor_panic_2026-09-24.md):
         * the hard assert() used to reboot the board in production the one
         * time it fired live. On a real target build (ESP_PLATFORM defined
         * -- this repo's only host/target discriminator, see
         * security_backend_placeholder.c) a violation now forces FAULTED
         * with heaters off and the task loop continues; it never reboots.
         * Host tests and any non-ESP_PLATFORM (debug) build keep the hard
         * assert so a violation is still caught loudly there. */
        {
            char mode_violation[160];
            uint32_t mode_violations = exec_mode_state_check(mode_violation, sizeof(mode_violation));
#if defined(ESP_PLATFORM)
            if (exec_handle_mode_state_violation(mode_violations, mode_violation)) {
                run_faulted_this_tick = true; /* record the FAULTED breadcrumb, same as a guard trip */
            }
#else
            assert(mode_violations == 0 && "exec_mode_state_check found a mode-state violation -- see the ESP_LOGE just above for which rule");
#endif
            (void)mode_violation; /* only read by assert()'s message above on a debug build, or by
                                    * exec_handle_mode_state_violation() on ESP_PLATFORM */
        }

        run_snapshot_buf_t tick_snap;
        capture_run_snapshot(&tick_snap);
        bool faulted_now = run_faulted_this_tick;
        xSemaphoreGive(s_exec.lock);

        /* Rate-limited internally (at most one NVS write per 10 min, and only
         * if something changed) -- see relay_cycles.h's flash-wear note.
         *
         * Called AFTER s_exec.lock is released, never under it: a due
         * persist dispatches to the flash worker (persist_snapshot_now() ->
         * uart_bridge_ext_run_on_flash_worker(), which takes s_bx_lock and
         * waits for the worker). Holding s_exec.lock across that wait
         * deadlocked against any worker job that itself takes s_exec.lock --
         * PROFILES DELETE's profile_executor_get_active_id() on the worker, or
         * a save section holding the worker reservation (cfg_save_lock.h) that
         * reads the executor. Lock order is s_bx_lock -> s_exec.lock; see
         * docs/audits/CFG_STORE_SAVE_RACE_2026-10-09.md, "Save mutex vs. flash
         * worker". Nothing here reads executor state, so the move changes only
         * which lock the persist runs under.
         * Also docs/audits/FLASH_WORKER_LOCK_INVERSION_AUDIT_2026-10-09.md F1. */
        relay_cycles_maybe_persist();

        if (faulted_now) {
            /* Ended badly, but ENDED -- the distinction the operator needs is
             * "did it stop on its own terms", and a guard trip did. The
             * guard number and reason ride along so the next boot can say
             * why without the log. */
            run_state_note(RUN_STATE_PHASE_FAULTED, &tick_snap.snap);
        } else if (segment_changed) {
            run_state_note(RUN_STATE_PHASE_RUNNING, &tick_snap.snap);
        } else {
            /* Rate-limited internally to one write per
             * RUN_STATE_REFRESH_INTERVAL_S -- see run_state.c's flash-wear
             * arithmetic. Cheap to call every tick. */
            run_state_note_progress(&tick_snap.snap);
        }
    }
}

/* ---- watchdog task (guard 9) ------------------------------------------------ */

#define WATCHDOG_LOCK_TIMEOUT_MS 1000u

/* Set by guard9_prelock_check() when it cut heat, cleared once the locked
 * bookkeeping (global_fault_source OR) has run. */
static volatile bool s_guard9_bookkeeping_pending = false;
/* Age (ms) the stale tick had when guard9_prelock_check() set the pending flag. */
static volatile uint32_t s_guard9_pending_since_ms = 0;

/* Guard 9, lock-free half (audit 2026-10-09 item 1). Reads the control task's
 * liveness tick without s_exec.lock (single 32-bit writer, volatile field;
 * nothing else is derived from it so no ordering is needed). On stale it
 * asserts SAFETY_FAULT_SRC_APP on the safety link (own lock) and then forces
 * relays off through kiln_io (direct SX1509 write), all without
 * s_exec.lock. Returns true when stale; *since_ms_out gets the age. */
static bool guard9_prelock_check(uint32_t *since_ms_out)
{
    TickType_t now = xTaskGetTickCount();
    uint32_t since_ms = ticks_to_ms(now - s_exec.last_tick_tick);
    if (since_ms_out) *since_ms_out = since_ms;
    if (since_ms <= WATCHDOG_TICK_DEAD_MS) {
        return false;
    }
    ESP_LOGE(PE_TAG, "guard 9: control tick stale for %lums -- cutting heat before taking s_exec.lock",
             (unsigned long)since_ms);
    /* Review F2: assert the APP fault source FIRST so relay_authority blocks any
     * re-assertion of ON, then cut. kiln_io_all_relays_off() writes the SX1509
     * directly from this task (chip access is locked inside
     * sx1509_write_port_locked); it is not serialised through the owner task. */
    if (s_exec.safety) {
        safety_link_set_fault_source(s_exec.safety, SAFETY_FAULT_SRC_APP, true);
    }
    if (s_exec.io) {
        kiln_io_all_relays_off(s_exec.io);
    }
    /* io_segs_force_all_off() mutates s_exec and needs the lock; it runs in
     * the locked half. */
    if (!s_guard9_bookkeeping_pending || since_ms > s_guard9_pending_since_ms) {
        s_guard9_pending_since_ms = since_ms;
    }
    s_guard9_bookkeeping_pending = true;
    return true;
}

/* Review fix F1: folds a verdict latched by an earlier pass (whose bounded lock
 * take timed out) into this pass's fresh one, so a control task that recovers
 * before the next pass still gets the run classified (FAULTED) instead of only
 * having relays cut. Call with s_exec.lock held; consumes the pending flag. */
static bool guard9_merge_pending(bool tick_stale, uint32_t since_ms, uint32_t *since_ms_out)
{
    bool pending = s_guard9_bookkeeping_pending;
    uint32_t ms = since_ms;
    if (pending && (!tick_stale || s_guard9_pending_since_ms > ms)) {
        ms = s_guard9_pending_since_ms;
    }
    s_guard9_bookkeeping_pending = false;
    s_guard9_pending_since_ms = 0;
    if (since_ms_out) *since_ms_out = ms;
    return tick_stale || pending;
}

/* K7 MED-1: relay_state_unknown (an ON write landed and the bus then died, or
 * the chip could not be read after a failed write) means a coil may be
 * energised while the shadow says otherwise. Treat it as a fault, not as OFF:
 * retry the fail-safe all-off every watchdog period (which also repairs the
 * expander once it answers) and hold SAFETY_FAULT_SRC_APP -- which blocks
 * relay-ON everywhere -- until a verified read or all-off clears the flag.
 * Rising edge here, lock-free; the falling edge is released in the locked half
 * (relay_unknown_release_locked). */
static bool s_relay_unknown_fault_asserted = false;

/* K7 review F5: SAFETY_FAULT_SRC_APP is one link bit with several holders. The executor-side
 * holders are tracked here so the bit drops only when NO owner holds it:
 *   - guard 9 / global fault: the APP bit in s_exec.global_fault_source (plus
 *     s_guard9_bookkeeping_pending while the locked half has not run yet);
 *   - relay-unknown hold: pe_app_owner_relay_unknown() (== s_relay_unknown_fault_asserted);
 *   - foreign: APP was already asserted on the link by something this module does not track
 *     (the boot safe-state latch in main*.c); never released here.
 * clear_this_runs_faults() and relay_unknown_release_locked() both consult these. */
bool pe_app_owner_foreign = false;

bool pe_app_owner_relay_unknown(void)
{
    return s_relay_unknown_fault_asserted;
}

/* LOW-2 (c)/(a): holders other than this module register explicitly (the boot safe-state
 * latches in main.c / main_control_bringup.c). The executor no longer infers a foreign holder
 * from the link bit: that inference raced a halt (false foreign, sticky until reboot) and
 * missed holders that asserted while the executor already held APP. Sticky for the boot by
 * design: it fails safe (APP stays held, relay ON refused). */
void profile_executor_note_external_app_hold(void)
{
    pe_app_owner_foreign = true;
}

static void relay_unknown_prelock_check(void)
{
    if (!s_exec.io) return;
    if (!kiln_io_relay_state_unknown(s_exec.io) && !s_relay_unknown_fault_asserted) return;
    if (kiln_io_relay_state_unknown(s_exec.io)) {
        ESP_LOGE(PE_TAG, "relay state unknown -- forcing relays off and holding the app fault source");
        (void)kiln_io_all_relays_off(s_exec.io);
    }
    if (kiln_io_relay_state_unknown(s_exec.io)) {
        /* LOW-2 (b): a level, not an edge. Publish the owner flag first, then assert APP on
         * EVERY pass while unknown: a locked halt that read the flag as false and deasserted
         * after our assert is repaired by the next pass. The assert is idempotent. */
        s_relay_unknown_fault_asserted = true;
        if (s_exec.safety) {
            safety_link_set_fault_source(s_exec.safety, SAFETY_FAULT_SRC_APP, true);
        }
    }
}

/* Must be called with s_exec.lock held. Releases only a hold this check took:
 * a guard-9 / global-fault assertion of the same bit stays. */
static void relay_unknown_release_locked(void)
{
    if (!s_relay_unknown_fault_asserted) return;
    if (s_exec.io && kiln_io_relay_state_unknown(s_exec.io)) return;
    s_relay_unknown_fault_asserted = false;
    if ((s_exec.global_fault_source & SAFETY_FAULT_SRC_APP) == 0 && !s_guard9_bookkeeping_pending &&
        !pe_app_owner_foreign && s_exec.safety) {
        safety_link_set_fault_source(s_exec.safety, SAFETY_FAULT_SRC_APP, false);
    }
}

static bool guard9_take_lock_bounded(void)
{
    return xSemaphoreTake(s_exec.lock, pdMS_TO_TICKS(WATCHDOG_LOCK_TIMEOUT_MS)) == pdTRUE;
}

void watchdog_task_entry(void *arg)
{
    (void)arg;
    /* Edge-trigger for PROFILE_EXECUTOR_WD_ACTION_LOG_IDLE_TRIP -- declared
     * outside the for(;;) below so it persists across loop iterations (this
     * task never returns while running, so that's equivalent to `static`
     * here, just without implying re-entrancy this function never has), and
     * outside the switch case below because it's also read/cleared on ticks
     * that land on a different action entirely (see the reset just after the
     * switch). */
    bool s_idle_trip_logged = false;
    /* PC-link fault-source tracking (see profile_executor.h's
     * PROFILE_EXECUTOR_PC_LINK_ABORT_SILENCE_MS doc comment): unlike the
     * safety link, SAFETY_FAULT_SRC_PC_LINK is a level read back off
     * safety_link_get_fault_sources(), not something with its own "age since
     * last change" -- uart_bridge.c's link_watchdog_task re-asserts it every
     * UART_BRIDGE_LINK_CHECK_MS while down and clears it the moment the link
     * is back (see that task), so this task has to time the level itself:
     * the tick this bit was FIRST seen asserted, persisted across loop
     * iterations same as s_idle_trip_logged above. */
    bool pc_link_was_down = false;
    TickType_t pc_link_down_since_tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_CHECK_PERIOD_MS));

        /* LINK_PROTOCOL.md sec 8 / ROADMAP.md M6: "30 s silence aborts a
         * firing" -- distinct from, and much larger than, the 1.5 s
         * SAFETY_FAULT_SRC_SAFETY_LINK check safety_update_health() already
         * runs on every poll (that one only blocks *new* relay-on via
         * relay_authority_on_blocked(); a single dropped frame must not abort
         * a run already in progress). Queried outside s_exec.lock --
         * safety_link_get_status() takes its own lock and never blocks on the
         * far side, same "never talks to the peer" contract everything else
         * in this driver relies on. safety_link_is_stale() is the same pure
         * comparison safety_link.c uses for the 1.5 s case, just against the
         * larger threshold. */
        uint16_t safety_age_ms = SAFETY_LINK_AGE_NEVER;
        bool safety_diag_valid = false;
        uint8_t safety_diag_state = SAFETY_LINK_DIAG_STATE_INIT;
        uint8_t safety_diag_trip_reason = 0;
        bool safety_k4_closed = false;
        uint32_t safety_reboot_seq = 0u;
        bool safety_diag_since_reboot = false;
        uint8_t safety_boot_reason = 0u;
        bool safety_status_ok = false;
        if (s_exec.safety) {
            safety_link_status_t safety_status;
            if (safety_link_get_status(s_exec.safety, &safety_status) == ESP_OK) {
                safety_status_ok = true;
                safety_age_ms = safety_status.age_ms;
                /* A trip report is only actionable while the DIAG frame it
                 * came from is fresh -- diag_age_ms shares the same
                 * cached_tick as age_ms (safety_link.c), so age_ms doubles as
                 * the diag frame's own age here. Anything stale beyond
                 * SAFETY_LINK_STALE_MS is exactly the "link went silent"
                 * case the 30s check below already owns; this branch must
                 * never act on a trip read off data that old, or a silent
                 * link's LAST-KNOWN diag_state would get relabeled as a
                 * fresh trip. */
                safety_diag_valid = safety_status.diag_ever_received &&
                                     !safety_link_is_stale(safety_age_ms, SAFETY_LINK_STALE_MS);
                safety_diag_state = safety_status.diag_state;
                safety_reboot_seq = safety_status.pico_reboot_seq;
                safety_diag_since_reboot = safety_status.diag_since_reboot;
                safety_boot_reason = safety_status.diag_boot_reason;
                safety_diag_trip_reason = safety_status.diag_trip_reason;
                safety_k4_closed = safety_status.link_up && (safety_status.flags & SAFETY_FLAG_RELAY) != 0u;
            }
        }
        bool safety_link_silent_30s = s_exec.safety != NULL &&
                                       safety_link_is_stale(safety_age_ms, SAFETY_LINK_FIRING_ABORT_SILENCE_MS);
        /* The gap this closes (ROADMAP.md): the link staying HEALTHY while
         * the safety processor itself actively trips is a different failure
         * than the link going silent, and until now nothing here ever read
         * diag_state/diag_trip_reason at all -- relay_authority already
         * blocked new relay-on, but the run itself kept advancing its
         * schedule and both GUIs kept showing a firing in progress while the
         * kiln cooled. */
        bool safety_processor_tripped = safety_diag_valid &&
                                         safety_diag_state == SAFETY_LINK_DIAG_STATE_TRIPPED;

        /* PC link loss (profile_executor.h's PROFILE_EXECUTOR_PC_LINK_ABORT_
         * SILENCE_MS): uart_bridge.c's link_watchdog_task asserts
         * SAFETY_FAULT_SRC_PC_LINK the whole time the PC/UART control link
         * is down and clears it the instant a frame or ACK is seen again --
         * read back here the same way safety_link_get_status() is read
         * above, outside s_exec.lock (safety_link_get_fault_sources() takes
         * its own lock and never blocks on the peer). This task has no
         * "age" for a level bit, so it times the level itself: latch the
         * tick on the rising edge, clear it the instant the bit drops. */
        bool pc_link_now_down = s_exec.safety != NULL &&
                                 (safety_link_get_fault_sources(s_exec.safety) & SAFETY_FAULT_SRC_PC_LINK) != 0;
        TickType_t pc_link_check_now = xTaskGetTickCount();
        if (pc_link_now_down && !pc_link_was_down) {
            pc_link_down_since_tick = pc_link_check_now;
        }
        pc_link_was_down = pc_link_now_down;
        bool pc_link_down_sustained = pc_link_now_down &&
            ticks_to_ms(pc_link_check_now - pc_link_down_since_tick) >= pc_link_abort_silence_ms();

        bool continue_after_lock_timeout = false;
        bool wdt_faulted = false;
        /* Guard 9 audit 2026-10-09 item 1: staleness is tested and the relays
         * are cut BEFORE s_exec.lock is touched. A control task that stalls
         * while holding the lock must not also blind its own watchdog. */
        uint32_t since_ms = 0;
        bool tick_stale = guard9_prelock_check(&since_ms);
        relay_unknown_prelock_check();
        if (!guard9_take_lock_bounded()) {
            /* Lock still held by someone else. Relays are already off and the
             * APP fault source is asserted (above); bookkeeping (state to
             * FAULTED, fault_reason) happens on a later pass once the lock is
             * obtainable. Nothing below may touch s_exec without the lock. */
            ESP_LOGE(PE_TAG, "guard 9: s_exec.lock not obtainable within %ums (tick_stale=%d, %lums)",
                     (unsigned)WATCHDOG_LOCK_TIMEOUT_MS, (int)tick_stale, (unsigned long)since_ms);
            continue_after_lock_timeout = true;
        }
        if (continue_after_lock_timeout) {
            /* No reconcile here: it must stay the single, LAST statement of the loop body
             * (test_heat_enable.c pins that), and it is only a retry -- next pass runs it. */
            continue;
        }

        /* LOW-2 (review 2): merge and assert guard 9 BEFORE relay_unknown_release_locked(), which
         * recomputes the link fault source from global_fault_source; otherwise APP drops briefly. */
        /* F1: carry a verdict latched on an earlier lock-timeout pass. */
        tick_stale = guard9_merge_pending(tick_stale, since_ms, &since_ms);
        if (tick_stale) {
            /* The relay cut itself already happened in guard9_prelock_check();
             * repeated here under the lock as a cheap idempotent retry (the
             * profile_executor_wd_decide() below classifies the STATE, this
             * part is unconditional). */
            if (tick_stale) {
                ESP_LOGE(PE_TAG, "control task tick stale for %lums -- forcing relays off (guard 9)",
                         (unsigned long)since_ms);
            }
            if (s_exec.io) {
                kiln_io_all_relays_off(s_exec.io);
            }
            io_segs_force_all_off(false);
            guard9_assert_stale_tick_fault();
        }
        relay_unknown_release_locked();

        /* profile_executor_wd_decide() (profile_executor.h) is the pure
         * classifier this pass extracted so it could be host-tested without
         * pulling in FreeRTOS/kiln_io/relay_authority -- see that header's
         * doc comment for the full reasoning. Every input it needs was
         * already gathered above (outside s_exec.lock for the safety_link_
         * get_status() call, per this task's own long-standing discipline);
         * this call only classifies, all the I/O (relay writes, logging,
         * state mutation) stays here in the caller. */
        profile_executor_wd_input_t wd_in;
        memset(&wd_in, 0, sizeof(wd_in));
        wd_in.tick_stale = tick_stale;
        wd_in.tick_stale_ms = since_ms;
        wd_in.safety_processor_tripped = safety_processor_tripped;
        wd_in.safety_trip_reason = safety_diag_trip_reason;
        wd_in.safety_link_silent_30s = safety_link_silent_30s;
        wd_in.pc_link_down_sustained = pc_link_down_sustained;
        wd_in.state_running_or_paused = (s_exec.state == PROFILE_EXEC_RUNNING ||
                                          s_exec.state == PROFILE_EXEC_PAUSED);
        wd_in.state_faulted = (s_exec.state == PROFILE_EXEC_FAULTED);
        profile_executor_wd_result_t wd_out = profile_executor_wd_decide(&wd_in);

        switch (wd_out.action) {
        case PROFILE_EXECUTOR_WD_ACTION_FAULT:
            /* "relays dropped and retried until the write succeeds" --
             * kiln_io_all_relays_off() is the same fail-toward-off call guard
             * 9 uses above; this task rechecks it every WATCHDOG_CHECK_
             * PERIOD_MS as long as the firing stays in this faulted state,
             * which is the retry LINK_PROTOCOL.md sec 8 asks for (the RETRY
             * case below is what performs those later rechecks). */
            ESP_LOGE(PE_TAG, "%s", wd_out.fault_reason);
            if (s_exec.io) {
                kiln_io_all_relays_off(s_exec.io);
            }
            io_segs_force_all_off(false); /* abnormal stop -- see escalate_guard_trip()'s branches */
            exec_enter_terminal_state(PROFILE_EXEC_FAULTED);
            strncpy(s_exec.fault_reason, wd_out.fault_reason, sizeof(s_exec.fault_reason) - 1);
            s_exec.fault_reason[sizeof(s_exec.fault_reason) - 1] = '\0';
            /* Same release as escalate_guard_trip()'s FAULTED branches --
             * this is guard 9, a second and independent path into FAULTED
             * (a dead control task, a silent safety link), and it must leave
             * relay ownership in the same clean state those do. */
            release_profile_relay_claim();
            wdt_faulted = true;
            break;
        case PROFILE_EXECUTOR_WD_ACTION_RETRY_RELAYS_OFF:
            /* Already faulted (this cause or another) but the underlying
             * condition (trip still latched, or link still silent) hasn't
             * cleared -- keep retrying the relay-off write per sec 8's
             * "dropped and retried until the write succeeds", without
             * touching fault_reason or re-triggering run_state_note()
             * (wdt_faulted stays false: nothing NEW happened this tick). */
            if (s_exec.io) {
                kiln_io_all_relays_off(s_exec.io);
            }
            io_segs_force_all_off(false); /* keep retrying, same as the relay-off retry above */
            break;
        case PROFILE_EXECUTOR_WD_ACTION_LOG_IDLE_TRIP:
            /* Edge-triggered (s_idle_trip_logged just below) so a trip that
             * stays latched for minutes logs once, not once every
             * WATCHDOG_CHECK_PERIOD_MS. The GUIs surface this state
             * themselves by reading diag_state directly (main_page.html's
             * renderSafetyTrip(), ui_page_home.c's refresh()), so no shared
             * state needs adding here just for display. */
            if (!s_idle_trip_logged) {
                ESP_LOGW(PE_TAG, "safety processor tripped (%s) while idle -- no run to abort",
                         profile_executor_safety_trip_words(safety_diag_trip_reason));
            }
            s_idle_trip_logged = true;
            break;
        case PROFILE_EXECUTOR_WD_ACTION_NONE:
        default:
            break;
        }
        /* Reset the edge-trigger the moment the trip input itself clears,
         * outside the switch so it resets regardless of which action ran
         * this tick (a cleared trip that was previously IDLE-logged may
         * land on ACTION_NONE the very next tick, never revisiting the
         * LOG_IDLE_TRIP case body). */
        if (!safety_processor_tripped) {
            s_idle_trip_logged = false;
        }
        run_snapshot_buf_t wdt_snap;
        if (wdt_faulted) {
            capture_run_snapshot(&wdt_snap);
        }
        xSemaphoreGive(s_exec.lock);

        /* Guard 9 is one of the few paths that can end a firing without the
         * control task ever running again, so it has to record the ending
         * itself -- otherwise a dead control task and a pulled plug look
         * identical next boot, which is precisely the confusion this record
         * exists to remove. Written from this task, outside the lock. */
        if (wdt_faulted) {
            run_state_note(RUN_STATE_PHASE_FAULTED, &wdt_snap.snap);
        }

        /* Retry a heat-enable request that never landed (heat_enable.h). A
         * no-op unless a run is holding a claim whose REQUEST_ENABLE was
         * refused -- which is exactly the "started while the safety link was
         * down, link came back mid-run" case that would otherwise leave a
         * firing running to completion with K4 open. This task, not the
         * control task: it is the one that already runs at a slow fixed
         * period and does not hold s_exec.lock here, and a blocking link
         * exchange must not sit inside the control tick.
         *
         * LAST in the loop body, deliberately, and this ordering is
         * load-bearing (2026-09-15 review of 8813bedd, finding HIGH-2).
         * It used to be FIRST, immediately after the vTaskDelay(). That call
         * can block for ~5.2 s in the worst case -- send_enable("retry") ->
         * he_flush_release_blocking() -> the safety link's xact_lock timeout
         * (see heat_enable.c's HE_FLUSH_MAX_ATTEMPTS comment for the
         * derivation) -- and every guard-9 check in this body sits BEHIND it:
         * the WATCHDOG_TICK_DEAD_MS (10 s) stale-tick test, the 30 s
         * safety-link silence abort, and the safety-processor trip check. A
         * worst-case block at the top of the body therefore pushed guard 9's
         * effective detection latency from roughly 10-12 s toward roughly
         * 15 s -- on the one task whose entire purpose is to still work when
         * the control task does not.
         *
         * Moving it here costs nothing: nothing in this loop body reads or
         * writes heat-enable state, so no computation above depends on the
         * reconcile having already run this tick, and the retry itself is
         * level-triggered (it re-examines s_he.pending/held_mask/granted from
         * scratch every call), not edge-triggered -- so running it one tick
         * "late" relative to the old position is indistinguishable from
         * running it at the old position one tick later. Everything above is
         * either loop-local (pc_link_*, s_idle_trip_logged) or read fresh
         * from safety_link/s_exec under their own locks. Keep it last: a new
         * blocking call must not be put ahead of the staleness checks either.
         */
        /* F1: tell heat_enable what the Pico reports so a lost grant is re-requested. */
        uint32_t wdt_now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        /* A failed status read must never feed reboot seq 0 (looks like a reboot). */
        if (safety_status_ok) {
            heat_enable_note_pico_boot(safety_reboot_seq, safety_diag_since_reboot, safety_boot_reason, wdt_now_ms);
        }
        /* A DIAG from before the latest Pico reboot describes the old boot: feed
         * INIT so its ARMED cannot satisfy the new boot's K4 check (LOW-3). */
        heat_enable_note_pico_state(safety_diag_valid,
                                    safety_diag_since_reboot ? safety_diag_state : SAFETY_LINK_DIAG_STATE_INIT,
                                    safety_k4_closed, wdt_now_ms);
        /* Safety-link fix batch 2 (MED-1, MED-2): an executor-initiated pause
         * (non-blocking: relays off, claim released, release frame deferred to
         * safety_poll_task) instead of a firing that runs cold with K4 open.
         * Before reconcile so a withdrawn request is not retried. */
        bool hold_reboot = heat_enable_reboot_hold();
        bool hold_unconfirmed = !hold_reboot && heat_enable_grant_unconfirmed();
        if (hold_reboot) {
            (void)profile_executor_pause_with_reason_bounded("pico_fatal_reboot");
            /* Autotune has no pause state: abort is its equivalent (relays off). */
            (void)autotune_engine_abort_bounded("pico_fatal_reboot", 100u);
        } else if (hold_unconfirmed) {
            (void)profile_executor_pause_with_reason_bounded("heat_grant_unconfirmed");
            (void)autotune_engine_abort_bounded("heat_grant_unconfirmed", 100u);
        }
        heat_enable_reconcile();
    }
}

